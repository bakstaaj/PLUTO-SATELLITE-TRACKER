#define _POSIX_C_SOURCE 200809L

#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <stdint.h>
#include <pthread.h>

#define APP_VERSION "2.9.2-dev"
#define DEFAULT_BIND_ADDR "127.0.0.1"
#define DEFAULT_NET_BIND_ADDR "0.0.0.0"
#define DEFAULT_PORT 8080
#define DEFAULT_DATA_DIR "data"
#define DEFAULT_WEB_DIR "web"
#define DEFAULT_CONFIG_DIR "config"
#define REQ_BUF_SIZE 131072
#define PATH_BUF_SIZE 1024
#define PLUTO_MIN_HZ 70000000LL
#define PLUTO_MAX_HZ 6000000000LL
#define AUDIO_SAMPLE_RATE 24000
#define AUDIO_IQ_SAMPLE_RATE 2400000
#define AUDIO_DECIMATION 100
#define AUDIO_PCM_CHUNK_SAMPLES 4800
#define AUDIO_IIO_BUFFER_SAMPLES 24000
#define AUDIO_CAPTURE_IQ_SAMPLES 2400000
#define AUDIO_LIVE_MIN_BLOCK_SAMPLES 1200
#define AUDIO_LIVE_DEFAULT_BLOCK_SAMPLES 12000
#define AUDIO_LIVE_MAX_BLOCK_SAMPLES 48000
#define AUDIO_LIVE_PCM_PATH "/tmp/pluto_live_audio.pcm"

static volatile sig_atomic_t g_running = 1;
static pthread_mutex_t g_track_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_track_thread;
static int g_track_auto_running = 0;
static pthread_mutex_t g_radio_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_audio_live_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *g_audio_debug_log = "/tmp/pluto_audio_debug.log";

struct app_config {
    const char *bind_addr;
    int port;
    const char *data_dir;
    const char *web_dir;
    const char *config_dir;
    int interactive;
};

struct client_job {
    int fd;
    const struct app_config *cfg;
};

struct fm_demod_state {
    long long acc;
    int acc_count;
    double prev_i;
    double prev_q;
    double prev_demod;
    double dc_y;
    int have_prev;
};

struct live_audio_state {
    pid_t pid;
    int running;
    long long frequency_hz;
    time_t started_epoch;
};

static int query_param(const char *query, const char *name, char *out, size_t out_size);
static void json_escape(char *dst, size_t dst_size, const char *src);
static int json_string_value(const char *json, const char *key, char *out, size_t out_size);
static int json_double_value(const char *json, const char *key, double *out);
static int send_wav_stream(int fd, const struct app_config *cfg, const char *query);
static void append_audio_debug(const char *message);
static struct live_audio_state g_live_audio = {0};

static void on_signal(int signum)
{
    (void)signum;
    g_running = 0;
}

static const char *env_or_default(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return (value && value[0]) ? value : fallback;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [options]\n\n"
            "Options:\n"
            "  --net                 Bind to 0.0.0.0 instead of 127.0.0.1.\n"
            "  --bind ADDR           Bind address.\n"
            "  --port PORT           HTTP port, default 8080.\n"
            "  --data-dir DIR        Satellite data directory.\n"
            "  --web-dir DIR         Web asset directory.\n"
            "  --config-dir DIR      Observer config directory.\n"
            "  --interactive         Print request log lines.\n"
            "  --help                Show this help.\n",
            argv0);
}

static int parse_args(int argc, char **argv, struct app_config *cfg)
{
    int i;

    cfg->bind_addr = DEFAULT_BIND_ADDR;
    cfg->port = DEFAULT_PORT;
    cfg->data_dir = env_or_default("PLUTO_SAT_DATA_DIR", DEFAULT_DATA_DIR);
    cfg->web_dir = env_or_default("PLUTO_SAT_WEB_DIR", DEFAULT_WEB_DIR);
    cfg->config_dir = env_or_default("PLUTO_SAT_CONFIG_DIR", DEFAULT_CONFIG_DIR);
    cfg->interactive = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--net") == 0) {
            cfg->bind_addr = DEFAULT_NET_BIND_ADDR;
        } else if (strcmp(argv[i], "--interactive") == 0) {
            cfg->interactive = 1;
        } else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
            cfg->bind_addr = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            cfg->port = atoi(argv[++i]);
            if (cfg->port <= 0 || cfg->port > 65535) {
                fprintf(stderr, "Invalid port.\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            cfg->data_dir = argv[++i];
        } else if (strcmp(argv[i], "--web-dir") == 0 && i + 1 < argc) {
            cfg->web_dir = argv[++i];
        } else if (strcmp(argv[i], "--config-dir") == 0 && i + 1 < argc) {
            cfg->config_dir = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
            usage(argv[0]);
            return -1;
        }
    }

    return 0;
}

static int send_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t written = send(fd, buf, len, 0);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        buf += written;
        len -= (size_t)written;
    }
    return 0;
}

static void send_text(int fd, int status, const char *reason,
                      const char *content_type, const char *body)
{
    char header[512];
    size_t body_len = strlen(body);
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lu\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, reason, content_type, (unsigned long)body_len);
    if (n > 0) {
        send_all(fd, header, (size_t)n);
        send_all(fd, body, body_len);
    }
}

static void send_redirect(int fd, const char *location)
{
    char header[512];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 302 Found\r\n"
                     "Location: %s\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     location);
    if (n > 0) {
        send_all(fd, header, (size_t)n);
    }
}

static int send_bytes(int fd, const void *buf, size_t len)
{
    return send_all(fd, (const char *)buf, len);
}

static int send_chunk(int fd, const void *buf, size_t len)
{
    char header[32];
    int n = snprintf(header, sizeof(header), "%lx\r\n", (unsigned long)len);
    if (n <= 0) {
        return -1;
    }
    if (send_bytes(fd, header, (size_t)n) != 0) {
        return -1;
    }
    if (len > 0 && send_bytes(fd, buf, len) != 0) {
        return -1;
    }
    return send_bytes(fd, "\r\n", 2);
}

static int finish_chunked_response(int fd)
{
    return send_bytes(fd, "0\r\n\r\n", 5);
}

static int send_audio_header(int fd)
{
    const char *header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: audio/wav\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";
    return send_bytes(fd, header, strlen(header));
}

static int send_pcm_buffer_response(int fd, const short *samples, size_t sample_count)
{
    char header[256];
    size_t body_len = sample_count * sizeof(short);
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: application/octet-stream\r\n"
                     "Content-Length: %lu\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     (unsigned long)body_len);
    if (n <= 0) {
        return -1;
    }
    if (send_bytes(fd, header, (size_t)n) != 0) {
        return -1;
    }
    if (body_len > 0 && send_bytes(fd, samples, body_len) != 0) {
        return -1;
    }
    return 0;
}

static void send_empty_response(int fd, int status, const char *reason,
                                const char *content_type, const char *extra_headers)
{
    char header[512];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: 0\r\n"
                     "Cache-Control: no-store\r\n"
                     "%s"
                     "Connection: close\r\n"
                     "\r\n",
                     status, reason, content_type ? content_type : "application/octet-stream",
                     extra_headers ? extra_headers : "");
    if (n > 0) {
        send_all(fd, header, (size_t)n);
    }
}

static void write_le16(unsigned char *dst, unsigned int value)
{
    dst[0] = (unsigned char)(value & 0xffu);
    dst[1] = (unsigned char)((value >> 8) & 0xffu);
}

static void write_le32(unsigned char *dst, unsigned int value)
{
    dst[0] = (unsigned char)(value & 0xffu);
    dst[1] = (unsigned char)((value >> 8) & 0xffu);
    dst[2] = (unsigned char)((value >> 16) & 0xffu);
    dst[3] = (unsigned char)((value >> 24) & 0xffu);
}

static int send_wav_header_chunk(int fd, int sample_rate, int channels, int bits_per_sample)
{
    unsigned char wav[44];
    unsigned int block_align = (unsigned int)(channels * (bits_per_sample / 8));
    unsigned int byte_rate = (unsigned int)sample_rate * block_align;
    unsigned int data_len = 0xffffffffu - 36u;

    memset(wav, 0, sizeof(wav));
    memcpy(wav, "RIFF", 4);
    write_le32(wav + 4, 36u + data_len);
    memcpy(wav + 8, "WAVEfmt ", 8);
    write_le32(wav + 16, 16u);
    write_le16(wav + 20, 1u);
    write_le16(wav + 22, (unsigned int)channels);
    write_le32(wav + 24, (unsigned int)sample_rate);
    write_le32(wav + 28, byte_rate);
    write_le16(wav + 32, block_align);
    write_le16(wav + 34, (unsigned int)bits_per_sample);
    memcpy(wav + 36, "data", 4);
    write_le32(wav + 40, data_len);

    return send_chunk(fd, wav, sizeof(wav));
}

static int send_wav_block_response(int fd, const short *samples, size_t sample_count,
                                   size_t from_sample, size_t available_samples)
{
    unsigned char wav[44];
    char header[512];
    size_t pcm_len = sample_count * sizeof(short);
    size_t total_len = sizeof(wav) + pcm_len;
    unsigned int block_align = 2u;
    unsigned int byte_rate = (unsigned int)AUDIO_SAMPLE_RATE * block_align;
    int n;

    memset(wav, 0, sizeof(wav));
    memcpy(wav, "RIFF", 4);
    write_le32(wav + 4, (unsigned int)(36u + pcm_len));
    memcpy(wav + 8, "WAVEfmt ", 8);
    write_le32(wav + 16, 16u);
    write_le16(wav + 20, 1u);
    write_le16(wav + 22, 1u);
    write_le32(wav + 24, (unsigned int)AUDIO_SAMPLE_RATE);
    write_le32(wav + 28, byte_rate);
    write_le16(wav + 32, block_align);
    write_le16(wav + 34, 16u);
    memcpy(wav + 36, "data", 4);
    write_le32(wav + 40, (unsigned int)pcm_len);

    n = snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: audio/wav\r\n"
                 "Content-Length: %lu\r\n"
                 "Cache-Control: no-store\r\n"
                 "X-Source-Samples: %lu\r\n"
                 "X-Audio-From-Sample: %lu\r\n"
                 "X-Audio-Available-Samples: %lu\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 (unsigned long)total_len,
                 (unsigned long)sample_count,
                 (unsigned long)from_sample,
                 (unsigned long)available_samples);
    if (n <= 0) {
        return -1;
    }
    if (send_bytes(fd, header, (size_t)n) != 0) {
        return -1;
    }
    if (send_bytes(fd, wav, sizeof(wav)) != 0) {
        return -1;
    }
    if (pcm_len > 0 && send_bytes(fd, samples, pcm_len) != 0) {
        return -1;
    }
    return 0;
}

static void append_audio_debug(const char *message)
{
    FILE *f = fopen(g_audio_debug_log, "ab");
    if (!f) {
        return;
    }
    if (message) {
        fputs(message, f);
    }
    fputc('\n', f);
    fclose(f);
}

static int send_pcm_silence_chunk(int fd, size_t sample_count)
{
    short silence[960];
    size_t remaining = sample_count;

    memset(silence, 0, sizeof(silence));
    while (remaining > 0) {
        size_t chunk = remaining > 960 ? 960 : remaining;
        if (send_chunk(fd, silence, chunk * sizeof(short)) != 0) {
            return -1;
        }
        remaining -= chunk;
    }
    return 0;
}

static const char *content_type_for(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) {
        return "application/octet-stream";
    }
    if (strcmp(ext, ".html") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcmp(ext, ".css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (strcmp(ext, ".js") == 0) {
        return "application/javascript; charset=utf-8";
    }
    if (strcmp(ext, ".json") == 0) {
        return "application/json; charset=utf-8";
    }
    if (strcmp(ext, ".png") == 0) {
        return "image/png";
    }
    if (strcmp(ext, ".svg") == 0) {
        return "image/svg+xml";
    }
    return "application/octet-stream";
}

static int read_file_to_string(const char *path, char **out, size_t *out_len)
{
    FILE *f;
    long len;
    char *buf;
    size_t got;

    f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    len = ftell(f);
    if (len < 0) {
        fclose(f);
        return -1;
    }
    rewind(f);

    buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';

static void send_live_audio_stream(int fd, const char *query)
{
    char seconds_text[64] = "";
    int max_seconds = 180;
    time_t started = time(NULL);
    size_t read_offset = 0;
    short pcm[AUDIO_PCM_CHUNK_SAMPLES];
    int idle_loops = 0;

    query_param(query, "seconds", seconds_text, sizeof(seconds_text));
    if (seconds_text[0]) {
        char *end = NULL;
        long parsed = strtol(seconds_text, &end, 10);
        if (end && *end == '\0' && parsed >= 5 && parsed <= 3600) {
            max_seconds = (int)parsed;
        }
    }

    pthread_mutex_lock(&g_audio_live_lock);
    if (!refresh_live_audio_process_locked()) {
        pthread_mutex_unlock(&g_audio_live_lock);
        send_audio_error_json(fd, 409, "Conflict", "live analog audio is not running");
        return;
    }
    pthread_mutex_unlock(&g_audio_live_lock);

    if (send_audio_header(fd) != 0 || send_wav_header_chunk(fd, AUDIO_SAMPLE_RATE, 1, 16) != 0) {
        return;
    }

    while (g_running && (time(NULL) - started) < max_seconds) {
        size_t available = live_audio_available_samples();

        if (available > read_offset) {
            FILE *input = fopen(AUDIO_LIVE_PCM_PATH, "rb");
            if (input) {
                size_t wanted = available - read_offset;
                size_t got;
                if (wanted > AUDIO_PCM_CHUNK_SAMPLES) {
                    wanted = AUDIO_PCM_CHUNK_SAMPLES;
                }
                if (fseek(input, (long)(read_offset * sizeof(short)), SEEK_SET) == 0) {
                    got = fread(pcm, sizeof(short), wanted, input);
                    if (got > 0) {
                        if (send_chunk(fd, pcm, got * sizeof(short)) != 0) {
                            fclose(input);
                            return;
                        }
                        read_offset += got;
                        idle_loops = 0;
                    }
                }
                fclose(input);
                usleep(20000);
                continue;
            }
        }

        pthread_mutex_lock(&g_audio_live_lock);
        if (!refresh_live_audio_process_locked()) {
            pthread_mutex_unlock(&g_audio_live_lock);
            break;
        }
        pthread_mutex_unlock(&g_audio_live_lock);

        /* Keep the browser decoder alive while the backend helper warms up. */
        if (++idle_loops >= 3) {
            if (send_pcm_silence_chunk(fd, 2400) != 0) {
                return;
            }
            idle_loops = 0;
        }
        usleep(100000);
    }

    finish_chunked_response(fd);
}







static void send_live_audio_block(int fd, const char *query)
{
    char from_text[64] = "";
    char samples_text[64] = "";
    char extra_headers[128];
    short *pcm_buffer = NULL;
    FILE *input = NULL;
    size_t from_sample = 0;
    size_t requested_samples = AUDIO_LIVE_DEFAULT_BLOCK_SAMPLES;
    size_t available_samples;
    size_t sample_count;
    char body[256];

    pthread_mutex_lock(&g_audio_live_lock);
    if (!refresh_live_audio_process_locked()) {
        pthread_mutex_unlock(&g_audio_live_lock);
        send_audio_error_json(fd, 409, "Conflict", "live analog audio is not running");
        return;
    }
    pthread_mutex_unlock(&g_audio_live_lock);

    query_param(query, "from", from_text, sizeof(from_text));
    if (from_text[0] && !parse_size_param(from_text, &from_sample)) {
        send_audio_error_json(fd, 400, "Bad Request", "valid from cursor is required");
        return;
    }

    query_param(query, "samples", samples_text, sizeof(samples_text));
    if (samples_text[0] && !parse_size_param(samples_text, &requested_samples)) {
        send_audio_error_json(fd, 400, "Bad Request", "valid samples count is required");
        return;
    }

    if (requested_samples < AUDIO_LIVE_MIN_BLOCK_SAMPLES) {
        requested_samples = AUDIO_LIVE_MIN_BLOCK_SAMPLES;
    } else if (requested_samples > AUDIO_LIVE_MAX_BLOCK_SAMPLES) {
        requested_samples = AUDIO_LIVE_MAX_BLOCK_SAMPLES;
    }

    available_samples = live_audio_available_samples();
    if (from_sample > available_samples) {
        snprintf(body, sizeof(body),
                 "{\"ok\":false,\"error\":\"requested cursor beyond available audio\",\"available_samples\":%lu}\n",
                 (unsigned long)available_samples);
        send_text(fd, 416, "Requested Range Not Satisfiable", "application/json; charset=utf-8", body);
        return;
    }

    sample_count = requested_samples;
    if (sample_count > available_samples - from_sample) {
        sample_count = available_samples - from_sample;
    }

    if (sample_count < AUDIO_LIVE_MIN_BLOCK_SAMPLES) {
        snprintf(extra_headers, sizeof(extra_headers),
                 "X-Audio-Available-Samples: %lu\r\n",
                 (unsigned long)available_samples);
        send_empty_response(fd, 204, "No Content", "audio/wav", extra_headers);
        return;
    }

    input = fopen(AUDIO_LIVE_PCM_PATH, "rb");
    if (!input) {
        send_audio_error_json(fd, 500, "Internal Server Error", "could not open live audio buffer");
        return;
    }
    if (fseek(input, (long)(from_sample * 2), SEEK_SET) != 0) {
        fclose(input);
        send_audio_error_json(fd, 500, "Internal Server Error", "could not seek live audio buffer");
        return;
    }

    pcm_buffer = (short *)malloc(sample_count * sizeof(short));
    if (!pcm_buffer) {
        fclose(input);
        send_audio_error_json(fd, 500, "Internal Server Error", "could not allocate live audio block");
        return;
    }

    sample_count = fread(pcm_buffer, sizeof(short), sample_count, input);
    fclose(input);

    if (sample_count == 0) {
        free(pcm_buffer);
        send_empty_response(fd, 204, "No Content", "audio/wav", NULL);
        return;
    }

    send_wav_block_response(fd, pcm_buffer, sample_count, from_sample, available_samples);
    free(pcm_buffer);
}



static int contains_nocase(const char *haystack, const char *needle)
{
    size_t needle_len;
    const char *p;

    if (!haystack || !needle) {
        return 0;
    }
    needle_len = strlen(needle);
    if (needle_len == 0) {
        return 1;
    }

    for (p = haystack; *p; p++) {
        size_t i;
        for (i = 0; i < needle_len; i++) {
            if (!p[i] || tolower((unsigned char)p[i]) != tolower((unsigned char)needle[i])) {
                break;
            }
        }
        if (i == needle_len) {
            return 1;
        }
    }
    return 0;
}

static void write_radio_profile(const struct app_config *cfg, const char *body)
{
    char path[PATH_BUF_SIZE];
    char tmp_path[PATH_BUF_SIZE + 8];
    FILE *f;

    join_path(path, sizeof(path), cfg->data_dir, "radio_profile.json");
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    f = fopen(tmp_path, "wb");
    if (!f) {
        return;
    }
    fputs(body, f);
    fclose(f);
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
    }
}

static void send_radio_hardware(int fd, const struct app_config *cfg)
{
    const char *paths[] = {
        "/sys/bus/iio/devices/iio:device1/out_altvoltage0_RX_LO_frequency",
        "/sys/bus/iio/devices/iio:device0/out_altvoltage0_RX_LO_frequency",
        "/sys/bus/iio/devices/iio:device2/out_altvoltage0_RX_LO_frequency",
        "/sys/kernel/debug/iio/iio:device1/out_altvoltage0_RX_LO_frequency",
        "/sys/kernel/debug/iio/iio:device0/out_altvoltage0_RX_LO_frequency",
    };
    char rx_path[PATH_BUF_SIZE] = "";
    char device_dir[PATH_BUF_SIZE] = "";
    char name_path[PATH_BUF_SIZE] = "";
    char available_path[PATH_BUF_SIZE] = "";
    char device_name[256] = "";
    char current[128] = "";
    char available[512] = "";
    char device_name_json[512];
    char available_json[1024];
    char body[3072];
    const char *profile_id = "unknown";
    const char *profile_label = "Unknown AD936x profile";
    const char *firmware_mode = "unknown";
    long long capability_min_hz = PLUTO_MIN_HZ;
    long long capability_max_hz = PLUTO_MAX_HZ;
    int vhf_tunable = 1;
    int uhf_tunable = 1;
    size_t i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (access(paths[i], W_OK) == 0 || access(paths[i], R_OK) == 0) {
            snprintf(rx_path, sizeof(rx_path), "%s", paths[i]);
            break;
        }
    }

    if (rx_path[0]) {
        char *slash;

        snprintf(device_dir, sizeof(device_dir), "%s", rx_path);
        slash = strrchr(device_dir, '/');
        if (slash) {

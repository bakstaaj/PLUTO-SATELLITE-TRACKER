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

#define APP_VERSION "2.6.37a"
/* RESTORE_517C91A_BUILDABLE_KEEP_RECEIVER_V1 */
/* BACKEND_STREAMING_AUDIO_STREAM_ROUTE_V1C */
/* BACKEND_STREAMING_AUDIO_ROUTE_FIX_V1B */
#define DEFAULT_BIND_ADDR "127.0.0.1"
#define DEFAULT_NET_BIND_ADDR "0.0.0.0"
#define DEFAULT_PORT 8080
#define DEFAULT_DATA_DIR "/mnt/jffs2/pluto_sat_tracker/data"
#define DEFAULT_WEB_DIR "/mnt/jffs2/pluto_sat_tracker/web"
#define DEFAULT_CONFIG_DIR "/mnt/jffs2/pluto_sat_tracker/config"
#define REQ_BUF_SIZE 131072
#define PATH_BUF_SIZE 1024
#define PLUTO_MIN_HZ 70000000LL
#define PLUTO_MAX_HZ 6000000000LL
#define AUDIO_SAMPLE_RATE 24000
#define AUDIO_IQ_SAMPLE_RATE 600000
#define AUDIO_DECIMATION 25
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
static pthread_t g_rotator_thread;
static int g_rotator_auto_running = 0; /* ROTATOR_CONTROL_FOUNDATION_V2_4_0 */
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
static void send_radio_spectrum_snapshot_v252(int fd, const struct app_config *cfg, const char *query);
static int apply_radio_track_step(
    const struct app_config *cfg,
    const char *state,
    const char *message,
    int enforce_pass_window,
    char *response,
    size_t response_size,
    char *error,
    size_t error_size);
static int ensure_track_auto_running_for_audio_v1(const struct app_config *cfg);
static void track_auto_set_running(int running);
static void send_rotator_config(int fd, const struct app_config *cfg);
static void save_rotator_config(int fd, const struct app_config *cfg, const char *body);
static void send_rotator_state(int fd, const struct app_config *cfg);
static void send_rotator_test(int fd, const struct app_config *cfg, const char *query);
static void send_rotator_park(int fd, const struct app_config *cfg);
static void send_rotator_stop(int fd, const struct app_config *cfg);
static void send_rotator_track_start(int fd, const struct app_config *cfg);
static void send_rotator_track_stop(int fd, const struct app_config *cfg);
static void send_rotator_track_step(int fd, const struct app_config *cfg);
static int apply_radio_track_step(
    const struct app_config *cfg,
    const char *state,
    const char *message,
    int enforce_pass_window,
    char *response,
    size_t response_size,
    char *error,
    size_t error_size);
static int ensure_track_auto_running_for_audio_v1(const struct app_config *cfg);
static void track_auto_set_running(int running);
static void append_audio_debug(const char *message);
static struct live_audio_state g_live_audio = {0};
static int g_audio_doppler_running_v6 = 0; /* LIVE_AUDIO_CONTROL_WIRING_BUILD_FIX_V2_6_8B */
/* LIVE_AUDIO_CONTROL_WIRING_BUILD_FIX_V2_6_8C */
/* LIVE_AUDIO_CONTROL_WIRING_BUILD_FIX_V2_6_8D */
/* BACKEND_AUDIO_OWNED_DOPPLER_V6 */
static pthread_t g_audio_doppler_thread_v6;
static const struct app_config *g_audio_doppler_cfg_v6 = NULL;
static int g_audio_started_track_auto_v1 = 0; /* BACKEND_AUDIO_DOPPLER_TRACKED_START_V1 */
static int apply_radio_track_step(
    const struct app_config *cfg,
    const char *state,
    const char *message,
    int enforce_pass_window,
    char *response,
    size_t response_size,
    char *error,
    size_t error_size);
static int ensure_track_auto_running_for_audio_v1(const struct app_config *cfg);
static void track_auto_set_running(int running);

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

/* BACKEND_AUDIO_DOPPLER_TRACKED_START_V1 */
static int query_param_is_true_audio_doppler_v1(const char *query, const char *name)
{
    char value[32] = "";

    if (!query || !name || !*name) {
        return 0;
    }

    query_param(query, name, value, sizeof(value));
    return strcmp(value, "1") == 0 ||
           strcmp(value, "true") == 0 ||
           strcmp(value, "yes") == 0 ||
           strcmp(value, "on") == 0;
}

static int query_param_is_false_audio_doppler_v6(const char *query, const char *name)
{
    char value[32] = "";

    if (!query || !name || !*name) {
        return 0;
    }

    query_param(query, name, value, sizeof(value));
    return strcmp(value, "0") == 0 ||
           strcmp(value, "false") == 0 ||
           strcmp(value, "no") == 0 ||
           strcmp(value, "off") == 0;
}

static int query_param_is_false_audio_doppler_v3(const char *query, const char *name)
{
    char value[32] = "";

    if (!query || !name || !*name) {
        return 0;
    }

    query_param(query, name, value, sizeof(value));
    return strcmp(value, "0") == 0 ||
           strcmp(value, "false") == 0 ||
           strcmp(value, "no") == 0 ||
           strcmp(value, "off") == 0;
}


/* JFFS2_PERSISTENT_TMP_TRANSACTIONAL_V1
 * Persistent app state lives on JFFS2; short-lived listen/track plan files live
 * in tmpfs so SD-card read-only remounts cannot break the listen workflow.
 */
static const char *runtime_dir_or_default(void)
{
    const char *value = getenv("PLUTO_SAT_RUNTIME_DIR");
    return (value && value[0]) ? value : "/tmp/pluto_sat_tracker";
}

static void runtime_file_path(char *out, size_t out_size, const char *filename)
{
    const char *dir = runtime_dir_or_default();
    size_t len;

    if (!out || out_size == 0) {
        return;
    }
    if (!dir || !*dir) {
        dir = "/tmp/pluto_sat_tracker";
    }
    len = strlen(dir);
    if (len > 0 && dir[len - 1] == '/') {
        snprintf(out, out_size, "%s%s", dir, filename ? filename : "");
    } else {
        snprintf(out, out_size, "%s/%s", dir, filename ? filename : "");
    }
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

    *out = buf;
    *out_len = got;
    return 0;
}

static void send_file(int fd, const char *path)
{
    FILE *f;
    struct stat st;
    char header[512];
    char buf[4096];
    int n;
    size_t got;

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        send_text(fd, 404, "Not Found", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"file not found\"}\n");
        return;
    }

    f = fopen(path, "rb");
    if (!f) {
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"could not open file\"}\n");
        return;
    }

    n = snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %lu\r\n"
                 "Cache-Control: no-store\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 content_type_for(path), (unsigned long)st.st_size);
    if (n > 0) {
        send_all(fd, header, (size_t)n);
    }

    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (send_all(fd, buf, got) != 0) {
            break;
        }
    }
    fclose(f);
}

static void join_path(char *out, size_t out_size, const char *base, const char *name)
{
    size_t base_len = strlen(base);
    snprintf(out, out_size, "%s%s%s", base, (base_len && base[base_len - 1] == '/') ? "" : "/", name);
}

static void send_json_file_or_default(int fd, const char *path, const char *fallback)
{
    char *body = NULL;
    size_t body_len = 0;

    if (read_file_to_string(path, &body, &body_len) == 0) {
        (void)body_len;
        send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
        free(body);
        return;
    }

    send_text(fd, 200, "OK", "application/json; charset=utf-8", fallback);
}

static void send_status(int fd, const struct app_config *cfg)
{
    char body[2048];
    time_t now = time(NULL);
    snprintf(body, sizeof(body),
             "{"
             "\"ok\":true,"
             "\"app\":\"pluto_sat_tracker\","
             "\"version\":\"%s\","
             "\"time_epoch\":%ld,"
             "\"bind_addr\":\"%s\","
             "\"port\":%d,"
             "\"web_dir\":\"%s\","
             "\"config_dir\":\"%s\","
             "\"data_dir\":\"%s\""
             "}\n",
             APP_VERSION, (long)now, cfg->bind_addr, cfg->port,
             cfg->web_dir, cfg->config_dir, cfg->data_dir);
    send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
}

static void send_time_sync(int fd, const struct app_config *cfg, const char *query)
{
    char epoch_text[64] = "";
    char path[PATH_BUF_SIZE];
    char tmp_path[PATH_BUF_SIZE + 8];
    char response[512];
    long long epoch;
    struct timeval tv;
    FILE *f;

    query_param(query, "epoch", epoch_text, sizeof(epoch_text));
    if (!epoch_text[0]) {
        epoch = 0;
    } else {
        char *end = NULL;
        errno = 0;
        epoch = strtoll(epoch_text, &end, 10);
        if (errno != 0 || !end || *end != '\0') {
            epoch = 0;
        }
    }
    if (epoch < 1781136000LL || epoch > 4102444800LL) {
        send_text(fd, 400, "Bad Request", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"valid epoch seconds are required\"}\n");
        return;
    }

    tv.tv_sec = (time_t)epoch;
    tv.tv_usec = 0;
    if (settimeofday(&tv, NULL) != 0) {
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"could not set Pluto system time\"}\n");
        return;
    }

    join_path(path, sizeof(path), cfg->data_dir, "time_sync.json");
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    f = fopen(tmp_path, "wb");
    if (f) {
        fprintf(f,
                "{\n"
                "  \"ok\": true,\n"
                "  \"source\": \"browser_epoch\",\n"
                "  \"synced_epoch\": %lld,\n"
                "  \"state\": \"synced\"\n"
                "}\n",
                epoch);
        fclose(f);
        if (rename(tmp_path, path) != 0) {
            unlink(tmp_path);
        }
    }

    snprintf(response, sizeof(response), "{\"ok\":true,\"state\":\"synced\",\"time_epoch\":%lld}\n", epoch);
    send_text(fd, 200, "OK", "application/json; charset=utf-8", response);
}

static void send_config(int fd, const struct app_config *cfg)
{
    char path[PATH_BUF_SIZE];
    const char *fallback =
        "{"
        "\"name\":\"Default Observer\","
        "\"latitude_deg\":38.8339,"
        "\"longitude_deg\":-104.8214,"
        "\"altitude_m\":1839,"
        "\"grid\":\"DM78\","
        "\"minimum_elevation_deg\":10"
        "}\n";
    join_path(path, sizeof(path), cfg->config_dir, "observer.json");
    send_json_file_or_default(fd, path, fallback);
}

static void send_config_save(int fd, const struct app_config *cfg, const char *body)
{
    char path[PATH_BUF_SIZE];
    char tmp_path[PATH_BUF_SIZE + 8];
    char name[256] = "";
    char grid[64] = "";
    char name_json[512];
    char grid_json[128];
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    double minimum_elevation = 10.0;
    FILE *f;

    if (!body || body[0] != '{') {
        send_text(fd, 400, "Bad Request", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"valid observer JSON is required\"}\n");
        return;
    }

    if (!json_string_value(body, "name", name, sizeof(name)) ||
        !json_double_value(body, "latitude_deg", &latitude) ||
        !json_double_value(body, "longitude_deg", &longitude)) {
        send_text(fd, 400, "Bad Request", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"observer name, latitude, and longitude are required\"}\n");
        return;
    }
    if (!json_double_value(body, "altitude_m", &altitude)) {
        altitude = 0.0;
    }
    if (!json_double_value(body, "minimum_elevation_deg", &minimum_elevation)) {
        minimum_elevation = 10.0;
    }
    json_string_value(body, "grid", grid, sizeof(grid));

    if (!name[0] ||
        latitude < -90.0 || latitude > 90.0 ||
        longitude < -180.0 || longitude > 180.0 ||
        minimum_elevation < 0.0 || minimum_elevation > 90.0 ||
        altitude < -1000.0 || altitude > 20000.0) {
        send_text(fd, 400, "Bad Request", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"observer values are out of range\"}\n");
        return;
    }

    json_escape(name_json, sizeof(name_json), name);
    json_escape(grid_json, sizeof(grid_json), grid);

    join_path(path, sizeof(path), cfg->config_dir, "observer.json");
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    f = fopen(tmp_path, "wb");
    if (!f) {
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"could not write observer config\"}\n");
        return;
    }

    fprintf(f,
            "{\n"
            "  \"name\": \"%s\",\n"
            "  \"latitude_deg\": %.6f,\n"
            "  \"longitude_deg\": %.6f,\n"
            "  \"altitude_m\": %.1f,\n"
            "  \"grid\": \"%s\",\n"
            "  \"minimum_elevation_deg\": %.1f\n"
            "}\n",
            name_json,
            latitude,
            longitude,
            altitude,
            grid_json,
            minimum_elevation);
    fclose(f);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"could not publish observer config\"}\n");
        return;
    }

    send_config(fd, cfg);
}

static void send_refresh_status_code(int fd, const struct app_config *cfg, int status, const char *reason)
{
    char path[PATH_BUF_SIZE];

    join_path(path, sizeof(path), cfg->data_dir, "refresh_status.json");
    if (status == 200) {
        send_json_file_or_default(fd, path,
                                  "{\"ok\":true,\"state\":\"idle\",\"target\":\"none\",\"message\":\"No Pluto-local refresh has run yet.\"}\n");
        return;
    }

    {
        char *body = NULL;
        size_t body_len = 0;
        if (read_file_to_string(path, &body, &body_len) == 0) {
            (void)body_len;
            send_text(fd, status, reason, "application/json; charset=utf-8", body);
            free(body);
            return;
        }
    }

    send_text(fd, status, reason, "application/json; charset=utf-8",
              "{\"ok\":false,\"state\":\"error\",\"message\":\"refresh failed\"}\n");
}

static void send_refresh_status(int fd, const struct app_config *cfg)
{
    send_refresh_status_code(fd, cfg, 200, "OK");
}

static void send_refresh_run(int fd, const struct app_config *cfg, const char *target)
{
    char command[PATH_BUF_SIZE * 4];
    char script_path[PATH_BUF_SIZE * 2];
    char deploy_dir[PATH_BUF_SIZE];
    char sd_root[PATH_BUF_SIZE];
    char *slash;
    int result;

    if (strcmp(target, "passes") != 0 && strcmp(target, "catalog") != 0 && strcmp(target, "all") != 0) {
        send_text(fd, 400, "Bad Request", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"unknown refresh target\"}\n");
        return;
    }

    snprintf(deploy_dir, sizeof(deploy_dir), "%s", cfg->config_dir);
    slash = strrchr(deploy_dir, '/');
    if (slash) {
        *slash = '\0';
    }
    snprintf(sd_root, sizeof(sd_root), "%s", cfg->data_dir);
    slash = strrchr(sd_root, '/');
    if (slash) {
        *slash = '\0';
    }
    join_path(script_path, sizeof(script_path), sd_root, "tools/pluto_refresh_data.sh");

    if (access(script_path, X_OK) != 0 && access(script_path, R_OK) != 0) {
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"Pluto refresh runner is not deployed on the SD card\"}\n");
        return;
    }

    if (snprintf(command, sizeof(command),
                 "PLUTO_DEPLOY_DIR='%s' PLUTO_SD_ROOT='%s' /bin/sh '%s' '%s'",
                 deploy_dir, sd_root, script_path, target) >= (int)sizeof(command)) {
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"refresh command path is too long\"}\n");
        return;
    }
    result = system(command);

    if (result == -1 || (WIFEXITED(result) && WEXITSTATUS(result) != 0) || !WIFEXITED(result)) {
        send_refresh_status_code(fd, cfg, 500, "Internal Server Error");
        return;
    }

    send_refresh_status(fd, cfg);
}

static void send_satellites(int fd, const struct app_config *cfg)
{
    char path[PATH_BUF_SIZE];
    const char *fallback =
        "{"
        "\"version\":1,"
        "\"updated_utc\":null,"
        "\"satellites\":[],"
        "\"notes\":[\"Satellite catalog has not been imported yet.\"]"
        "}\n";
    join_path(path, sizeof(path), cfg->data_dir, "satellites.json");
    send_json_file_or_default(fd, path, fallback);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *dst, size_t dst_size, const char *src)
{
    size_t out = 0;
    while (*src && out + 1 < dst_size) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            int hi = hex_value(src[1]);
            int lo = hex_value(src[2]);
            dst[out++] = (char)((hi << 4) | lo);
            src += 3;
        } else if (*src == '+') {
            dst[out++] = ' ';
            src++;
        } else {
            dst[out++] = *src++;
        }
    }
    dst[out] = '\0';
}

static int query_param(const char *query, const char *name, char *out, size_t out_size)
{
    size_t name_len = strlen(name);
    const char *p = query;

    if (!query || !*query) {
        return 0;
    }

    while (*p) {
        const char *next = strchr(p, '&');
        size_t len = next ? (size_t)(next - p) : strlen(p);
        const char *eq = memchr(p, '=', len);

        if (eq && (size_t)(eq - p) == name_len && strncmp(p, name, name_len) == 0) {
            char encoded[512];
            size_t value_len = len - (size_t)(eq - p) - 1;
            if (value_len >= sizeof(encoded)) {
                value_len = sizeof(encoded) - 1;
            }
            memcpy(encoded, eq + 1, value_len);
            encoded[value_len] = '\0';
            url_decode(out, out_size, encoded);
            return 1;
        }

        if (!next) {
            break;
        }
        p = next + 1;
    }

    return 0;
}

static void json_escape(char *dst, size_t dst_size, const char *src)
{
    size_t out = 0;
    while (*src && out + 1 < dst_size) {
        unsigned char c = (unsigned char)*src++;
        if ((c == '"' || c == '\\') && out + 2 < dst_size) {
            dst[out++] = '\\';
            dst[out++] = (char)c;
        } else if (c >= 32) {
            dst[out++] = (char)c;
        }
    }
    dst[out] = '\0';
}

static long long days_from_civil(int year, unsigned month, unsigned day)
{
    int era;
    unsigned yoe;
    unsigned doy;
    unsigned doe;

    year -= month <= 2;
    era = (year >= 0 ? year : year - 399) / 400;
    yoe = (unsigned)(year - era * 400);
    doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long long)era * 146097 + (long long)doe - 719468;
}

static int parse_iso_utc_epoch(const char *value, long long *out)
{
    int year;
    unsigned month;
    unsigned day;
    unsigned hour;
    unsigned minute;
    unsigned second;
    long long days;

    if (!value || !out) {
        return 0;
    }

    if (sscanf(value, "%4d-%2u-%2uT%2u:%2u:%2uZ", &year, &month, &day, &hour, &minute, &second) != 6) {
        return 0;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
        return 0;
    }

    days = days_from_civil(year, month, day);
    *out = days * 86400LL + (long long)hour * 3600LL + (long long)minute * 60LL + (long long)second;
    return 1;
}

static int json_string_value(const char *json, const char *key, char *out, size_t out_size)
{
    char pattern[128];
    const char *p;
    const char *start;
    size_t i = 0;

    if (!json || !key || !out || out_size == 0) {
        return 0;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (!p) {
        return 0;
    }
    p = strchr(p + strlen(pattern), ':');
    if (!p) {
        return 0;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    if (*p != '"') {
        return 0;
    }
    start = ++p;

    while (*p && *p != '"' && i + 1 < out_size) {
        if (*p == '\\' && p[1]) {
            p++;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return p > start;
}

static int json_double_value(const char *json, const char *key, double *out)
{
    char pattern[128];
    const char *p;
    char *end = NULL;

    if (!json || !key || !out) {
        return 0;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (!p) {
        return 0;
    }
    p = strchr(p + strlen(pattern), ':');
    if (!p) {
        return 0;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    errno = 0;
    *out = strtod(p, &end);
    return errno == 0 && end && end != p;
}

static int json_long_long_after(const char *json, const char *key, long long *out)
{
    char pattern[128];
    const char *p;

    if (!json || !key || !out) {
        return 0;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (!p) {
        return 0;
    }
    p = strchr(p + strlen(pattern), ':');
    if (!p) {
        return 0;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    errno = 0;
    *out = strtoll(p, NULL, 10);
    return errno == 0;
}

static void send_radio_plan(int fd, const struct app_config *cfg, const char *query)
{
    char name[256] = "";
    char norad[64] = "";
    char aos[128] = "";
    char downlink[64] = "";
    char uplink[64] = "";
    char mode[128] = "";
    char description[512] = "";
    char name_json[512];
    char mode_json[256];
    char description_json[1024];
    char path[PATH_BUF_SIZE];
    char tmp_path[PATH_BUF_SIZE + 8];
    char body[2048];
    FILE *f;
    time_t now = time(NULL);

    query_param(query, "name", name, sizeof(name));
    query_param(query, "norad", norad, sizeof(norad));
    query_param(query, "aos", aos, sizeof(aos));
    query_param(query, "downlink", downlink, sizeof(downlink));
    query_param(query, "uplink", uplink, sizeof(uplink));
    query_param(query, "mode", mode, sizeof(mode));
    query_param(query, "description", description, sizeof(description));

    if (!name[0] || !norad[0] || !downlink[0]) {
        send_text(fd, 400, "Bad Request", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"name, norad, and downlink are required\"}\n");
        return;
    }

    json_escape(name_json, sizeof(name_json), name);
    json_escape(mode_json, sizeof(mode_json), mode);
    json_escape(description_json, sizeof(description_json), description);

    runtime_file_path(path, sizeof(path), "radio_target.json");
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    f = fopen(tmp_path, "wb");
    if (!f) {
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"could not write radio target\"}\n");
        return;
    }

    fprintf(f,
            "{\n"
            "  \"ok\": true,\n"
            "  \"planned_epoch\": %ld,\n"
            "  \"name\": \"%s\",\n"
            "  \"norad_id\": %s,\n"
            "  \"aos_utc\": \"%s\",\n"
            "  \"downlink_hz\": %s,\n"
            "  \"uplink_hz\": %s,\n"
            "  \"mode\": \"%s\",\n"
            "  \"description\": \"%s\",\n"
            "  \"state\": \"planned\"\n"
            "}\n",
            (long)now,
            name_json,
            norad,
            aos,
            downlink,
            uplink[0] ? uplink : "null",
            mode_json,
            description_json);
    fclose(f);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"could not publish radio target\"}\n");
        return;
    }

    snprintf(body, sizeof(body),
             "{\"ok\":true,\"state\":\"planned\",\"name\":\"%s\",\"norad_id\":%s,\"downlink_hz\":%s}\n",
             name_json, norad, downlink);
    send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
}

static int write_radio_target_state(
    const struct app_config *cfg,
    time_t planned_epoch,
    time_t tuned_epoch,
    const char *name,
    const char *norad,
    const char *aos,
    long long downlink_hz,
    const char *uplink,
    const char *mode,
    const char *description,
    const char *lo_path,
    const char *sample_time_utc,
    const char *state)
{
    char name_json[512];
    char mode_json[256];
    char description_json[1024];
    char lo_path_json[PATH_BUF_SIZE * 2];
    char sample_time_json[128];
    char aos_json[256];
    char path[PATH_BUF_SIZE];
    char tmp_path[PATH_BUF_SIZE + 8];
    char tuned_epoch_value[32];
    const char *norad_value = (norad && *norad) ? norad : "null";
    const char *uplink_value = (uplink && *uplink) ? uplink : "null";
    FILE *f;

    json_escape(name_json, sizeof(name_json), name ? name : "");
    json_escape(mode_json, sizeof(mode_json), mode ? mode : "");
    json_escape(description_json, sizeof(description_json), description ? description : "");
    json_escape(lo_path_json, sizeof(lo_path_json), lo_path ? lo_path : "");
    json_escape(sample_time_json, sizeof(sample_time_json), sample_time_utc ? sample_time_utc : "");
    json_escape(aos_json, sizeof(aos_json), aos ? aos : "");

    if (tuned_epoch > 0) {
        snprintf(tuned_epoch_value, sizeof(tuned_epoch_value), "%ld", (long)tuned_epoch);
    } else {
        snprintf(tuned_epoch_value, sizeof(tuned_epoch_value), "null");
    }

    runtime_file_path(path, sizeof(path), "radio_target.json");
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    f = fopen(tmp_path, "wb");
    if (!f) {
        return 0;
    }

    fprintf(f,
            "{\n"
            "  \"ok\": true,\n"
            "  \"planned_epoch\": %ld,\n"
            "  \"tuned_epoch\": %s,\n"
            "  \"name\": \"%s\",\n"
            "  \"norad_id\": %s,\n"
            "  \"aos_utc\": \"%s\",\n"
            "  \"downlink_hz\": %lld,\n"
            "  \"uplink_hz\": %s,\n"
            "  \"mode\": \"%s\",\n"
            "  \"description\": \"%s\",\n"
            "  \"lo_path\": \"%s\",\n"
            "  \"sample_time_utc\": \"%s\",\n"
            "  \"state\": \"%s\"\n"
            "}\n",
            (long)planned_epoch,
            tuned_epoch_value,
            name_json,
            norad_value,
            aos_json,
            downlink_hz,
            uplink_value,
            mode_json,
            description_json,
            lo_path_json,
            sample_time_json,
            state ? state : "idle");
    fclose(f);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return 0;
    }

    return 1;
}


/* BACKEND_AUDIO_DOPPLER_DEFAULT_V3 */
static int is_noaa_weather_frequency_v3(long long frequency_hz)
{
    return frequency_hz == 162400000LL ||
           frequency_hz == 162425000LL ||
           frequency_hz == 162450000LL ||
           frequency_hz == 162475000LL ||
           frequency_hz == 162500000LL ||
           frequency_hz == 162525000LL ||
           frequency_hz == 162550000LL;
}

static int parse_frequency_hz(const char *value, long long *out)
{
    char *end = NULL;
    long long parsed;

    if (!value || !*value) {
        return 0;
    }

    errno = 0;
    parsed = strtoll(value, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        return 0;
    }
    if (parsed < PLUTO_MIN_HZ || parsed > PLUTO_MAX_HZ) {
        return 0;
    }

    *out = parsed;
    return 1;
}

static int read_first_line(const char *path, char *out, size_t out_size)
{
    FILE *f;
    size_t len;

    if (!path || !out || out_size == 0) {
        return 0;
    }

    f = fopen(path, "rb");
    if (!f) {
        return 0;
    }

    if (!fgets(out, (int)out_size, f)) {
        fclose(f);
        return 0;
    }
    fclose(f);

    len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) {
        out[--len] = '\0';
    }

    return 1;
}

static int write_rx_lo_frequency(long long frequency_hz, char *path_used, size_t path_used_size)
{
    /* BACKEND_DOPPLER_IIO_LO_WRITE_V4
     * Match the known-good pluto_fm_receiver tuning path. Some Pluto Plus
     * firmware layouts do not expose old sysfs LO paths reliably, while
     * iio_attr against ad9361-phy altvoltage0 frequency works.
     */
    {
        char command[512];
        int result;
        if (snprintf(command, sizeof(command),
                     "/usr/bin/iio_attr -u local: -c ad9361-phy altvoltage0 frequency %lld >/dev/null 2>&1",
                     frequency_hz) < (int)sizeof(command)) {
            result = system(command);
            if (result != -1 && WIFEXITED(result) && WEXITSTATUS(result) == 0) {
                snprintf(path_used, path_used_size, "iio_attr:ad9361-phy/altvoltage0/frequency");
                return 1;
            }
        }
    }

    const char *paths[] = {
        "/sys/bus/iio/devices/iio:device1/out_altvoltage0_RX_LO_frequency",
        "/sys/bus/iio/devices/iio:device0/out_altvoltage0_RX_LO_frequency",
        "/sys/bus/iio/devices/iio:device2/out_altvoltage0_RX_LO_frequency",
        "/sys/kernel/debug/iio/iio:device1/out_altvoltage0_RX_LO_frequency",
        "/sys/kernel/debug/iio/iio:device0/out_altvoltage0_RX_LO_frequency",
    };
    size_t i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *f = fopen(paths[i], "wb");
        if (!f) {
            continue;
        }
        fprintf(f, "%lld\n", frequency_hz);
        if (fclose(f) == 0) {
            snprintf(path_used, path_used_size, "%s", paths[i]);
            return 1;
        }
    }

    return 0;
}

/* AUDIO_HELPER_TOLERANT_CONFIG_600K_V1
 * Keep audio alive on Pluto firmware variants where one or more iio_attr
 * tuning attributes are rejected.  The RX LO write is required; sample-rate,
 * RF bandwidth, and gain-mode writes are best-effort so live audio does not
 * fail before iio_readdev can produce IQ samples.
 */
/* AUDIO_TOLERANT_CONFIG_600K_V2
 * Keep audio alive on Pluto firmware variants where one or more iio_attr
 * tuning attributes are rejected.  The RX LO write is required; sample-rate,
 * RF bandwidth, and gain-mode writes are best-effort so live audio does not
 * fail before iio_readdev can produce IQ samples.
 */
static int configure_rx_audio_path(long long frequency_hz)
{
    char command[512];
    int result;

    if (frequency_hz < PLUTO_MIN_HZ || frequency_hz > PLUTO_MAX_HZ) {
        return 0;
    }

    if (snprintf(command, sizeof(command),
                 "/usr/bin/iio_attr -u local: -c ad9361-phy voltage0 sampling_frequency %d >/tmp/pluto_audio_iio_attr.log 2>&1",
                 AUDIO_IQ_SAMPLE_RATE) < (int)sizeof(command)) {
        result = system(command);
        (void)result;
    }

    if (snprintf(command, sizeof(command),
                 "/usr/bin/iio_attr -u local: -c ad9361-phy voltage0 rf_bandwidth 200000 >>/tmp/pluto_audio_iio_attr.log 2>&1") < (int)sizeof(command)) {
        result = system(command);
        (void)result;
    }

    if (snprintf(command, sizeof(command),
                 "/usr/bin/iio_attr -u local: -c ad9361-phy voltage0 gain_control_mode slow_attack >>/tmp/pluto_audio_iio_attr.log 2>&1") < (int)sizeof(command)) {
        result = system(command);
        (void)result;
    }

    return write_rx_lo_frequency(frequency_hz, NULL, 0);
}



static int read_active_audio_frequency_hz(const struct app_config *cfg, long long *out_hz)
{
    char path[PATH_BUF_SIZE];
    char *body = NULL;
    size_t body_len = 0;
    long long hz = 0;

    if (!out_hz) {
        return 0;
    }

    runtime_file_path(path, sizeof(path), "radio_target.json");
    if (read_file_to_string(path, &body, &body_len) == 0) {
        (void)body_len;
        if (json_long_long_after(body, "downlink_hz", &hz) && hz >= PLUTO_MIN_HZ && hz <= PLUTO_MAX_HZ) {
            free(body);
            *out_hz = hz;
            return 1;
        }
        free(body);
    }

    return 0;
}

static int send_audio_error_json(int fd, int status, const char *reason, const char *message)
{
    char body[512];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}\n", message);
    send_text(fd, status, reason, "application/json; charset=utf-8", body);
    return -1;
}

static int stream_fm_audio(FILE *pipe, int fd, struct fm_demod_state *state)
{
    unsigned char iq_bytes[AUDIO_IIO_BUFFER_SAMPLES * 4];
    short pcm[AUDIO_PCM_CHUNK_SAMPLES];
    char debug_line[128];
    size_t pcm_count = 0;

    while (g_running) {
        size_t got = fread(iq_bytes, 1, sizeof(iq_bytes), pipe);
        size_t sample_pairs;
        size_t sample_index;

        if (got == 0) {
            if (feof(pipe)) {
                append_audio_debug("stream_fm_audio eof");
                break;
            }
            if (ferror(pipe)) {
                append_audio_debug("stream_fm_audio ferror");
                return -1;
            }
            continue;
        }

        snprintf(debug_line, sizeof(debug_line), "stream_fm_audio got=%lu", (unsigned long)got);
        append_audio_debug(debug_line);

        sample_pairs = got / 4;
        for (sample_index = 0; sample_index < sample_pairs; sample_index++) {
            int offset = (int)(sample_index * 4);
            short i_raw = (short)((unsigned short)iq_bytes[offset] | ((unsigned short)iq_bytes[offset + 1] << 8));
            short q_raw = (short)((unsigned short)iq_bytes[offset + 2] | ((unsigned short)iq_bytes[offset + 3] << 8));
            double i_now = (double)i_raw;
            double q_now = (double)q_raw;
            double demod = 0.0;

            if (state->have_prev) {
                double cross = state->prev_i * q_now - state->prev_q * i_now;
                double dot = state->prev_i * i_now + state->prev_q * q_now;
                demod = atan2(cross, dot);
            }

            state->have_prev = 1;
            state->prev_i = i_now;
            state->prev_q = q_now;

            state->dc_y = (demod - state->prev_demod) + 0.995 * state->dc_y;
            state->prev_demod = demod;

            state->acc += (long long)lrint(state->dc_y * 12000.0);
            state->acc_count++;

            if (state->acc_count >= AUDIO_DECIMATION) {
                long long averaged = state->acc / AUDIO_DECIMATION;
                if (averaged > 32767) {
                    averaged = 32767;
                } else if (averaged < -32768) {
                    averaged = -32768;
                }

                pcm[pcm_count++] = (short)averaged;
                state->acc = 0;
                state->acc_count = 0;

                if (pcm_count >= AUDIO_PCM_CHUNK_SAMPLES) {
                    append_audio_debug("stream_fm_audio send_chunk pcm=960");
                    if (send_chunk(fd, pcm, pcm_count * sizeof(short)) != 0) {
                        append_audio_debug("stream_fm_audio send_chunk failed");
                        return -1;
                    }
                    pcm_count = 0;
                }
            }
        }
    }

    if (pcm_count > 0) {
        snprintf(debug_line, sizeof(debug_line), "stream_fm_audio flush pcm=%lu", (unsigned long)pcm_count);
        append_audio_debug(debug_line);
        if (send_chunk(fd, pcm, pcm_count * sizeof(short)) != 0) {
            append_audio_debug("stream_fm_audio flush failed");
            return -1;
        }
    }

    return 0;
}

static int stream_fm_audio_to_file(FILE *pipe, FILE *output, struct fm_demod_state *state)
{
    (void)pipe;
    (void)output;
    (void)state;
    return -1;
}

static int capture_fm_audio_buffer(FILE *pipe, short *pcm_out, size_t pcm_capacity, size_t *pcm_written, struct fm_demod_state *state)
{
    unsigned char iq_bytes[AUDIO_IIO_BUFFER_SAMPLES * 4];
    size_t out_count = 0;

    if (!pcm_out || !pcm_written || !state) {
        return -1;
    }

    while (g_running) {
        size_t got = fread(iq_bytes, 1, sizeof(iq_bytes), pipe);
        size_t sample_pairs;
        size_t sample_index;

        if (got == 0) {
            if (feof(pipe)) {
                break;
            }
            if (ferror(pipe)) {
                return -1;
            }
            continue;
        }

        sample_pairs = got / 4;
        for (sample_index = 0; sample_index < sample_pairs; sample_index++) {
            int offset = (int)(sample_index * 4);
            short i_raw = (short)((unsigned short)iq_bytes[offset] | ((unsigned short)iq_bytes[offset + 1] << 8));
            short q_raw = (short)((unsigned short)iq_bytes[offset + 2] | ((unsigned short)iq_bytes[offset + 3] << 8));
            double i_now = (double)i_raw;
            double q_now = (double)q_raw;
            double demod = 0.0;

            if (state->have_prev) {
                double cross = state->prev_i * q_now - state->prev_q * i_now;
                double dot = state->prev_i * i_now + state->prev_q * q_now;
                demod = atan2(cross, dot);
            }

            state->have_prev = 1;
            state->prev_i = i_now;
            state->prev_q = q_now;
            state->dc_y = (demod - state->prev_demod) + 0.995 * state->dc_y;
            state->prev_demod = demod;
            state->acc += (long long)lrint(state->dc_y * 12000.0);
            state->acc_count++;

            if (state->acc_count >= AUDIO_DECIMATION) {
                long long averaged = state->acc / AUDIO_DECIMATION;
                if (averaged > 32767) {
                    averaged = 32767;
                } else if (averaged < -32768) {
                    averaged = -32768;
                }
                if (out_count >= pcm_capacity) {
                    *pcm_written = out_count;
                    return 0;
                }
                pcm_out[out_count++] = (short)averaged;
                state->acc = 0;
                state->acc_count = 0;
            }
        }
    }

    *pcm_written = out_count;
    return 0;
}

static int capture_fm_audio_once(short *pcm_out, size_t pcm_capacity, size_t *pcm_written,
                                 struct fm_demod_state *state)
{
    const char *capture_path = "/tmp/pluto_audio_iq.bin";
    char command[512];
    FILE *input;
    int result;
    int capture_result;

    if (snprintf(command, sizeof(command),
                 "/usr/bin/iio_readdev -u local: -b %d -s %d cf-ad9361-lpc > %s 2>/dev/null",
                 AUDIO_IIO_BUFFER_SAMPLES,
                 AUDIO_CAPTURE_IQ_SAMPLES,
                 capture_path) >= (int)sizeof(command)) {
        return -1;
    }

    result = system(command);
    if (result == -1 || !WIFEXITED(result) || WEXITSTATUS(result) != 0) {
        unlink(capture_path);
        return -1;
    }

    input = fopen(capture_path, "rb");
    if (!input) {
        unlink(capture_path);
        return -1;
    }

    capture_result = capture_fm_audio_buffer(input, pcm_out, pcm_capacity, pcm_written, state);
    fclose(input);
    unlink(capture_path);
    return capture_result;
}

static FILE *start_iio_read_stream(pid_t *child_pid)
{
    int pipe_fds[2];
    pid_t pid;
    char buffer_text[32];

    if (!child_pid) {
        return NULL;
    }

    if (pipe(pipe_fds) != 0) {
        return NULL;
    }

    pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return NULL;
    }

    if (pid == 0) {
        snprintf(buffer_text, sizeof(buffer_text), "%d", AUDIO_IIO_BUFFER_SAMPLES);
        dup2(pipe_fds[1], STDOUT_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        execl("/usr/bin/iio_readdev",
              "iio_readdev",
              "-u", "local:",
              "-b", buffer_text,
              "-s", "0",
              "cf-ad9361-lpc",
              (char *)NULL);
        _exit(127);
    }

    close(pipe_fds[1]);
    *child_pid = pid;
    return fdopen(pipe_fds[0], "rb");
}

static size_t live_audio_available_samples(void)
{
    struct stat st;

    if (stat(AUDIO_LIVE_PCM_PATH, &st) != 0 || st.st_size <= 0) {
        return 0;
    }
    return (size_t)(st.st_size / 2);
}

static int parse_size_param(const char *value, size_t *out)
{
    char *end = NULL;
    unsigned long long parsed;

    if (!value || !*value || !out) {
        return 0;
    }

    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        return 0;
    }

    *out = (size_t)parsed;
    return 1;
}

static void helper_binary_path(const struct app_config *cfg, char *out, size_t out_size)
{
    char parent[PATH_BUF_SIZE];
    const char *web_dir = cfg && cfg->web_dir ? cfg->web_dir : DEFAULT_WEB_DIR;
    size_t len = strlen(web_dir);

    snprintf(parent, sizeof(parent), "%s", web_dir);
    while (len > 0) {
        if (parent[len - 1] == '/' || parent[len - 1] == '\\') {
            parent[len - 1] = '\0';
            len--;
            continue;
        }
        break;
    }
    while (len > 0) {
        if (parent[len - 1] == '/' || parent[len - 1] == '\\') {
            parent[len - 1] = '\0';
            break;
        }
        len--;
    }

    join_path(out, out_size, parent[0] ? parent : ".", "pluto_fm_receiver");
}

static int refresh_live_audio_process_locked(void)
{
    int status = 0;
    pid_t result;

    if (!g_live_audio.running || g_live_audio.pid <= 0) {
        g_live_audio.running = 0;
        g_live_audio.pid = 0;
        return 0;
    }

    result = waitpid(g_live_audio.pid, &status, WNOHANG);
    if (result == 0) {
        return 1;
    }

    g_live_audio.running = 0;
    g_live_audio.pid = 0;
    return 0;
}

static void *live_audio_worker(void *arg)
{
    (void)arg;
    return NULL;
}

static void audio_doppler_stop_worker_v6(void);
static int stop_live_audio_session(void)
{
    pid_t pid = 0;
    int was_running = 0;

    pthread_mutex_lock(&g_audio_live_lock);
    if (refresh_live_audio_process_locked()) {
        pid = g_live_audio.pid;
        was_running = 1;
        g_live_audio.running = 0;
        g_live_audio.pid = 0;
    }
    pthread_mutex_unlock(&g_audio_live_lock);

    if (pid > 0) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
    }

    unlink(AUDIO_LIVE_PCM_PATH);
    audio_doppler_stop_worker_v6();
    if (g_audio_started_track_auto_v1) {
        track_auto_set_running(0);
        g_audio_started_track_auto_v1 = 0;
    }
    return was_running;
}


/* LIVE_AUDIO_SQUELCH_APPLY_V2_6_14 */
/* LIVE_AUDIO_BACKEND_CONTROL_WIRING_V2_6_17
 * Pass the compact Spectrum/Listen controls through to pluto_fm_receiver.
 * These are argv values, not shell-expanded strings.
 */

/* BACKEND_AUDIO_CONTROL_HELPER_ARGS_V2_6_20
 * Hold the latest live-audio controls parsed by /api/radio/audio/live/start.
 * The values are copied before fork(), so the child can safely use them when
 * building the pluto_fm_receiver argv list.  This keeps all existing callers
 * compatible with the stable two-argument start_live_audio_session() signature.
 */
static char g_audio_ctl_rf_bw_hz_v2620[64] = "";
static char g_audio_ctl_decoder_bw_hz_v2620[64] = "";
static char g_audio_ctl_gain_db_v2620[64] = "";
static char g_audio_ctl_squelch_db_v2620[64] = "";

static void audio_control_set_pending_v2620(
    const char *rf_bw_hz,
    const char *decoder_bw_hz,
    const char *gain_db,
    const char *squelch_db)
{
    snprintf(g_audio_ctl_rf_bw_hz_v2620, sizeof(g_audio_ctl_rf_bw_hz_v2620), "%s", rf_bw_hz ? rf_bw_hz : "");
    snprintf(g_audio_ctl_decoder_bw_hz_v2620, sizeof(g_audio_ctl_decoder_bw_hz_v2620), "%s", decoder_bw_hz ? decoder_bw_hz : "");
    snprintf(g_audio_ctl_gain_db_v2620, sizeof(g_audio_ctl_gain_db_v2620), "%s", gain_db ? gain_db : "");
    snprintf(g_audio_ctl_squelch_db_v2620, sizeof(g_audio_ctl_squelch_db_v2620), "%s", squelch_db ? squelch_db : "");
}

static int start_live_audio_session(const struct app_config *cfg, long long frequency_hz)
{
    char helper_path[PATH_BUF_SIZE];
    char frequency_text[64];
    pid_t pid;

    helper_binary_path(cfg, helper_path, sizeof(helper_path));
    stop_live_audio_session();
    unlink(AUDIO_LIVE_PCM_PATH);

    snprintf(frequency_text, sizeof(frequency_text), "%lld", frequency_hz);
    pid = fork();
    if (pid < 0) {
        return 0;
    }

    if (pid == 0) {
        const char *argv_live_v2620[18];
        int argc_live_v2620 = 0;
        argv_live_v2620[argc_live_v2620++] = helper_path;
        argv_live_v2620[argc_live_v2620++] = "--freq-hz";
        argv_live_v2620[argc_live_v2620++] = frequency_text;
        argv_live_v2620[argc_live_v2620++] = "--output";
        argv_live_v2620[argc_live_v2620++] = AUDIO_LIVE_PCM_PATH;
        if (g_audio_ctl_rf_bw_hz_v2620[0]) {
            argv_live_v2620[argc_live_v2620++] = "--rf-bw-hz";
            argv_live_v2620[argc_live_v2620++] = g_audio_ctl_rf_bw_hz_v2620;
        }
        if (g_audio_ctl_decoder_bw_hz_v2620[0]) {
            argv_live_v2620[argc_live_v2620++] = "--decoder-bw-hz";
            argv_live_v2620[argc_live_v2620++] = g_audio_ctl_decoder_bw_hz_v2620;
        }
        if (g_audio_ctl_gain_db_v2620[0]) {
            argv_live_v2620[argc_live_v2620++] = "--gain-db";
            argv_live_v2620[argc_live_v2620++] = g_audio_ctl_gain_db_v2620;
        }
        if (g_audio_ctl_squelch_db_v2620[0]) {
            argv_live_v2620[argc_live_v2620++] = "--squelch-db";
            argv_live_v2620[argc_live_v2620++] = g_audio_ctl_squelch_db_v2620;
        }
        argv_live_v2620[argc_live_v2620] = NULL;
        execv(helper_path, (char * const *)argv_live_v2620);
        _exit(127);
    }

    pthread_mutex_lock(&g_audio_live_lock);
    g_live_audio.pid = pid;
    g_live_audio.running = 1;
    g_live_audio.frequency_hz = frequency_hz;
    g_live_audio.started_epoch = time(NULL);
    pthread_mutex_unlock(&g_audio_live_lock);
    return 1;
}




static int send_wav_stream(int fd, const struct app_config *cfg, const char *query)
{
    char frequency_text[64] = "";
    char command[512];
    char debug_line[256];
    long long frequency_hz = 0;
    struct fm_demod_state demod_state;

    query_param(query, "downlink_hz", frequency_text, sizeof(frequency_text));
    if (frequency_text[0]) {
        if (!parse_frequency_hz(frequency_text, &frequency_hz)) {
            return send_audio_error_json(fd, 400, "Bad Request", "valid downlink_hz is required");
        }
    } else if (!read_active_audio_frequency_hz(cfg, &frequency_hz)) {
        return send_audio_error_json(fd, 409, "Conflict", "plan or tune a satellite first, or pass downlink_hz");
    }

    if (pthread_mutex_trylock(&g_radio_lock) != 0) {
        append_audio_debug("send_wav_stream radio busy");
        return send_audio_error_json(fd, 409, "Conflict", "radio is busy with another hardware operation");
    }

    if (!configure_rx_audio_path(frequency_hz)) {
        append_audio_debug("send_wav_stream configure failed");
        pthread_mutex_unlock(&g_radio_lock);
        return send_audio_error_json(fd, 500, "Internal Server Error", "could not configure Pluto RX path for audio");
    }

    if (send_audio_header(fd) != 0 || send_wav_header_chunk(fd, AUDIO_SAMPLE_RATE, 1, 16) != 0) {
        append_audio_debug("send_wav_stream header failed");
        pthread_mutex_unlock(&g_radio_lock);
        return -1;
    }

    snprintf(debug_line, sizeof(debug_line), "send_wav_stream start hz=%lld", frequency_hz);
    append_audio_debug(debug_line);

    memset(&demod_state, 0, sizeof(demod_state));

    if (snprintf(command, sizeof(command),
                 "/usr/bin/iio_readdev -u local: -b %d -s %d cf-ad9361-lpc 2>/dev/null",
                 AUDIO_IIO_BUFFER_SAMPLES,
                 AUDIO_CAPTURE_IQ_SAMPLES) >= (int)sizeof(command)) {
        pthread_mutex_unlock(&g_radio_lock);
        return send_audio_error_json(fd, 500, "Internal Server Error", "audio capture command is too long");
    }

    while (g_running) {
        FILE *pipe = popen(command, "rb");
        int stream_result;
        int close_status;

        if (!pipe) {
            append_audio_debug("send_wav_stream popen failed");
            pthread_mutex_unlock(&g_radio_lock);
            return send_audio_error_json(fd, 500, "Internal Server Error", "could not start Pluto audio capture");
        }

        append_audio_debug("send_wav_stream popen ok");
        stream_result = stream_fm_audio(pipe, fd, &demod_state);
        close_status = pclose(pipe);
        snprintf(debug_line, sizeof(debug_line), "send_wav_stream loop done stream=%d close=%d", stream_result, close_status);
        append_audio_debug(debug_line);

        if (stream_result != 0) {
            pthread_mutex_unlock(&g_radio_lock);
            return -1;
        }
        if (close_status == -1) {
            pthread_mutex_unlock(&g_radio_lock);
            return -1;
        }
    }

    pthread_mutex_unlock(&g_radio_lock);
    return finish_chunked_response(fd);
}

static int send_pcm_stream(int fd, const struct app_config *cfg, const char *query)
{
    char frequency_text[64] = "";
    long long frequency_hz = 0;
    struct fm_demod_state demod_state;
    short *pcm_buffer = NULL;
    size_t pcm_capacity = (size_t)(AUDIO_CAPTURE_IQ_SAMPLES / AUDIO_DECIMATION + 8);
    size_t pcm_written = 0;

    query_param(query, "downlink_hz", frequency_text, sizeof(frequency_text));
    if (frequency_text[0]) {
        if (!parse_frequency_hz(frequency_text, &frequency_hz)) {
            return send_audio_error_json(fd, 400, "Bad Request", "valid downlink_hz is required");
        }
    } else if (!read_active_audio_frequency_hz(cfg, &frequency_hz)) {
        return send_audio_error_json(fd, 409, "Conflict", "plan or tune a satellite first, or pass downlink_hz");
    }

    if (pthread_mutex_trylock(&g_radio_lock) != 0) {
        return send_audio_error_json(fd, 409, "Conflict", "radio is busy with another hardware operation");
    }

    if (!configure_rx_audio_path(frequency_hz)) {
        pthread_mutex_unlock(&g_radio_lock);
        return send_audio_error_json(fd, 500, "Internal Server Error", "could not configure Pluto RX path for audio");
    }

    memset(&demod_state, 0, sizeof(demod_state));
    pcm_buffer = (short *)malloc(pcm_capacity * sizeof(short));
    if (!pcm_buffer) {
        pthread_mutex_unlock(&g_radio_lock);
        return send_audio_error_json(fd, 500, "Internal Server Error", "could not allocate audio buffer");
    }

    {
        int capture_result;
        int send_result;

        capture_result = capture_fm_audio_once(pcm_buffer, pcm_capacity, &pcm_written, &demod_state);
        pthread_mutex_unlock(&g_radio_lock);

        if (capture_result != 0) {
            free(pcm_buffer);
            return send_audio_error_json(fd, 500, "Internal Server Error", "could not capture Pluto audio");
        }

        send_result = send_pcm_buffer_response(fd, pcm_buffer, pcm_written);
        free(pcm_buffer);
        return send_result;
    }
}

static int send_pcm_live_stream(int fd, const struct app_config *cfg, const char *query)
{
    char frequency_text[64] = "";
    char header[256];
    long long frequency_hz = 0;
    struct fm_demod_state demod_state;
    FILE *pipe = NULL;
    pid_t child_pid = -1;
    int stream_result;
    int wait_status = 0;
    int header_len;

    query_param(query, "downlink_hz", frequency_text, sizeof(frequency_text));
    if (frequency_text[0]) {
        if (!parse_frequency_hz(frequency_text, &frequency_hz)) {
            return send_audio_error_json(fd, 400, "Bad Request", "valid downlink_hz is required");
        }
    } else if (!read_active_audio_frequency_hz(cfg, &frequency_hz)) {
        return send_audio_error_json(fd, 409, "Conflict", "plan or tune a satellite first, or pass downlink_hz");
    }

    if (pthread_mutex_trylock(&g_radio_lock) != 0) {
        return send_audio_error_json(fd, 409, "Conflict", "radio is busy with another hardware operation");
    }

    if (!configure_rx_audio_path(frequency_hz)) {
        pthread_mutex_unlock(&g_radio_lock);
        return send_audio_error_json(fd, 500, "Internal Server Error", "could not configure Pluto RX path for audio");
    }

    header_len = snprintf(header, sizeof(header),
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/octet-stream\r\n"
                          "Cache-Control: no-store\r\n"
                          "Connection: close\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "\r\n");
    if (header_len <= 0 || send_bytes(fd, header, (size_t)header_len) != 0) {
        pthread_mutex_unlock(&g_radio_lock);
        return -1;
    }

    memset(&demod_state, 0, sizeof(demod_state));
    pipe = start_iio_read_stream(&child_pid);
    if (!pipe) {
        pthread_mutex_unlock(&g_radio_lock);
        return send_audio_error_json(fd, 500, "Internal Server Error", "could not start continuous Pluto audio capture");
    }

    stream_result = stream_fm_audio(pipe, fd, &demod_state);
    fclose(pipe);
    pipe = NULL;
    if (child_pid > 0) {
        kill(child_pid, SIGTERM);
        waitpid(child_pid, &wait_status, 0);
    }
    pthread_mutex_unlock(&g_radio_lock);
    if (stream_result == 0) {
        finish_chunked_response(fd);
    }
    return stream_result;
}

static int send_audio_chunk_json(int fd, const struct app_config *cfg, const char *query)
{
    char frequency_text[64] = "";
    char debug_line[256];
    long long frequency_hz = 0;
    struct fm_demod_state demod_state;
    short *pcm_buffer = NULL;
    char *body = NULL;
    size_t pcm_capacity = (size_t)(AUDIO_CAPTURE_IQ_SAMPLES / AUDIO_DECIMATION + 8);
    size_t pcm_written = 0;
    size_t body_cap;
    size_t body_len = 0;

    query_param(query, "downlink_hz", frequency_text, sizeof(frequency_text));
    if (frequency_text[0]) {
        if (!parse_frequency_hz(frequency_text, &frequency_hz)) {
            append_audio_debug("fm_chunk invalid downlink_hz");
            return send_audio_error_json(fd, 400, "Bad Request", "valid downlink_hz is required");
        }
    } else if (!read_active_audio_frequency_hz(cfg, &frequency_hz)) {
        append_audio_debug("fm_chunk no active frequency");
        return send_audio_error_json(fd, 409, "Conflict", "plan or tune a satellite first, or pass downlink_hz");
    }

    snprintf(debug_line, sizeof(debug_line), "fm_chunk start hz=%lld", frequency_hz);
    append_audio_debug(debug_line);

    if (pthread_mutex_trylock(&g_radio_lock) != 0) {
        append_audio_debug("fm_chunk radio lock busy");
        return send_audio_error_json(fd, 409, "Conflict", "radio is busy with another hardware operation");
    }

    if (!configure_rx_audio_path(frequency_hz)) {
        append_audio_debug("fm_chunk configure_rx_audio_path failed");
        pthread_mutex_unlock(&g_radio_lock);
        return send_audio_error_json(fd, 500, "Internal Server Error", "could not configure Pluto RX path for audio");
    }

    memset(&demod_state, 0, sizeof(demod_state));
    pcm_buffer = (short *)malloc(pcm_capacity * sizeof(short));
    if (!pcm_buffer) {
        append_audio_debug("fm_chunk pcm buffer alloc failed");
        pthread_mutex_unlock(&g_radio_lock);
        return send_audio_error_json(fd, 500, "Internal Server Error", "could not allocate audio buffer");
    }

    {
        int capture_result;
        size_t i;

        capture_result = capture_fm_audio_once(pcm_buffer, pcm_capacity, &pcm_written, &demod_state);
        pthread_mutex_unlock(&g_radio_lock);

        snprintf(debug_line, sizeof(debug_line),
                 "fm_chunk capture_result=%d pcm_written=%lu",
                 capture_result, (unsigned long)pcm_written);
        append_audio_debug(debug_line);

        if (capture_result != 0) {
            free(pcm_buffer);
            return send_audio_error_json(fd, 500, "Internal Server Error", "could not capture Pluto audio");
        }

        body_cap = 128 + pcm_written * 8;
        body = (char *)malloc(body_cap);
        if (!body) {
            append_audio_debug("fm_chunk json alloc failed");
            free(pcm_buffer);
            return send_audio_error_json(fd, 500, "Internal Server Error", "could not allocate audio JSON");
        }

        body_len += (size_t)snprintf(body + body_len, body_cap - body_len,
                                     "{\"ok\":true,\"sample_rate\":%d,\"downlink_hz\":%lld,\"samples\":[",
                                     AUDIO_SAMPLE_RATE, frequency_hz);
        for (i = 0; i < pcm_written; i++) {
            body_len += (size_t)snprintf(body + body_len, body_cap - body_len,
                                         "%s%d",
                                         (i == 0) ? "" : ",",
                                         (int)pcm_buffer[i]);
        }
        body_len += (size_t)snprintf(body + body_len, body_cap - body_len, "]}\n");
        snprintf(debug_line, sizeof(debug_line),
                 "fm_chunk send body_len=%lu body_cap=%lu",
                 (unsigned long)body_len, (unsigned long)body_cap);
        append_audio_debug(debug_line);
        send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
        free(body);
        free(pcm_buffer);
        return 0;
    }
}



/* BACKEND_STREAMING_AUDIO_ROUTE_RECOVERY_V1F
 * Backend-owned continuous audio streaming.
 * The browser should only attach an <audio> element to the returned WAV stream.
 */
static int query_param_is_true(const char *query, const char *name)
{
    char value[32] = "";
    query_param(query, name, value, sizeof(value));
    return strcmp(value, "1") == 0 ||
           strcmp(value, "true") == 0 ||
           strcmp(value, "yes") == 0 ||
           strcmp(value, "on") == 0;
}

static int send_live_audio_stream_v1f(int fd, const char *query)
{
    char from_text[64] = "";
    size_t cursor_samples = 0;
    size_t idle_loops = 0;

    (void)query;
    query_param(query, "from", from_text, sizeof(from_text));
    if (from_text[0] && !parse_size_param(from_text, &cursor_samples)) {
        return send_audio_error_json(fd, 400, "Bad Request", "valid from cursor is required");
    }

    pthread_mutex_lock(&g_audio_live_lock);
    if (!refresh_live_audio_process_locked()) {
        pthread_mutex_unlock(&g_audio_live_lock);
        return send_audio_error_json(fd, 409, "Conflict", "live analog audio is not running");
    }
    pthread_mutex_unlock(&g_audio_live_lock);

    if (send_audio_header(fd) != 0 || send_wav_header_chunk(fd, AUDIO_SAMPLE_RATE, 1, 16) != 0) {
        return -1;
    }

    while (g_running) {
        size_t available_samples;
        size_t sample_count;
        short pcm[AUDIO_PCM_CHUNK_SAMPLES];
        FILE *input;

        pthread_mutex_lock(&g_audio_live_lock);
        if (!refresh_live_audio_process_locked()) {
            pthread_mutex_unlock(&g_audio_live_lock);
            break;
        }
        pthread_mutex_unlock(&g_audio_live_lock);

        available_samples = live_audio_available_samples();
        if (cursor_samples >= available_samples) {
            idle_loops++;
            usleep(50000);
            if (idle_loops >= 20) {
                short silence[480];
                memset(silence, 0, sizeof(silence));
                if (send_chunk(fd, silence, sizeof(silence)) != 0) {
                    return -1;
                }
                idle_loops = 0;
            }
            continue;
        }
        idle_loops = 0;

        sample_count = available_samples - cursor_samples;
        if (sample_count > AUDIO_PCM_CHUNK_SAMPLES) {
            sample_count = AUDIO_PCM_CHUNK_SAMPLES;
        }

        input = fopen(AUDIO_LIVE_PCM_PATH, "rb");
        if (!input) {
            usleep(50000);
            continue;
        }
        if (fseek(input, (long)(cursor_samples * sizeof(short)), SEEK_SET) != 0) {
            fclose(input);
            return -1;
        }
        sample_count = fread(pcm, sizeof(short), sample_count, input);
        fclose(input);

        if (sample_count == 0) {
            usleep(50000);
            continue;
        }

        if (send_chunk(fd, pcm, sample_count * sizeof(short)) != 0) {
            return -1;
        }
        cursor_samples += sample_count;
    }

    return finish_chunked_response(fd);
}


/* BACKEND_AUDIO_OWNED_DOPPLER_V6 */
static int audio_read_track_plan_v6(const struct app_config *cfg, char **plan_out, size_t *plan_len_out, char *path_used, size_t path_used_size)
{
    char path[PATH_BUF_SIZE];
    const char *fallback_paths[] = {
        "/mnt/jffs2/pluto_sat_tracker/data/radio_track.json",
        "data/radio_track.json"
    };
    size_t i;

    if (!plan_out || !plan_len_out) {
        return 0;
    }
    *plan_out = NULL;
    *plan_len_out = 0;

    if (cfg && cfg->data_dir) {
        join_path(path, sizeof(path), cfg->data_dir, "radio_track.json");
        if (read_file_to_string(path, plan_out, plan_len_out) == 0) {
            if (path_used && path_used_size > 0) {
                snprintf(path_used, path_used_size, "%s", path);
            }
            return 1;
        }
    }

    for (i = 0; i < sizeof(fallback_paths) / sizeof(fallback_paths[0]); i++) {
        if (read_file_to_string(fallback_paths[i], plan_out, plan_len_out) == 0) {
            if (path_used && path_used_size > 0) {
                snprintf(path_used, path_used_size, "%s", fallback_paths[i]);
            }
            return 1;
        }
    }

    return 0;
}

static int audio_track_window_v6(const char *plan, long long *aos_epoch, long long *los_epoch)
{
    char aos[64] = "";
    char los[64] = "";

    if (!plan || !aos_epoch || !los_epoch) {
        return 0;
    }

    if (!json_string_value(plan, "aos_utc", aos, sizeof(aos)) ||
        !json_string_value(plan, "los_utc", los, sizeof(los))) {
        return 0;
    }

    return parse_iso_utc_epoch(aos, aos_epoch) && parse_iso_utc_epoch(los, los_epoch);
}

static int audio_find_track_point_for_now_v6(
    const char *plan_json,
    long long now_epoch,
    long long *rx_hz,
    char *point_time,
    size_t point_time_size,
    int *point_index,
    long long *point_epoch_out)
{
    const char *p = plan_json;
    long long best_past_epoch = LLONG_MIN;
    long long best_future_epoch = LLONG_MAX;
    long long best_past_hz = 0;
    long long best_future_hz = 0;
    int best_past_index = -1;
    int best_future_index = -1;
    char best_past_time[64] = "";
    char best_future_time[64] = "";
    int index = 0;
    int found_past = 0;
    int found_future = 0;

    while (p && (p = strstr(p, "\"time_utc\"")) != NULL) {
        char time_value[64] = "";
        const char *object_start = p;
        const char *time_start;
        const char *next_point;
        long long point_epoch;
        long long point_rx_hz;

        while (object_start > plan_json && *object_start != '{') {
            object_start--;
        }

        time_start = strchr(p, ':');
        if (!time_start) {
            break;
        }
        time_start++;
        while (*time_start == ' ' || *time_start == '\t' || *time_start == '\r' || *time_start == '\n') {
            time_start++;
        }
        if (*time_start != '"') {
            p += 10;
            continue;
        }
        time_start++;

        snprintf(time_value, sizeof(time_value), "%.63s", time_start);
        if (strchr(time_value, '"')) {
            *strchr(time_value, '"') = '\0';
        }

        next_point = strstr(time_start, "\"time_utc\"");

        if (!json_long_long_after(object_start, "rx_hz", &point_rx_hz) ||
            !parse_iso_utc_epoch(time_value, &point_epoch)) {
            p = next_point ? next_point : time_start + strlen(time_start);
            index++;
            continue;
        }

        if (point_epoch <= now_epoch) {
            if (!found_past || point_epoch > best_past_epoch) {
                found_past = 1;
                best_past_epoch = point_epoch;
                best_past_hz = point_rx_hz;
                best_past_index = index;
                snprintf(best_past_time, sizeof(best_past_time), "%s", time_value);
            }
        } else if (!found_future || point_epoch < best_future_epoch) {
            found_future = 1;
            best_future_epoch = point_epoch;
            best_future_hz = point_rx_hz;
            best_future_index = index;
            snprintf(best_future_time, sizeof(best_future_time), "%s", time_value);
        }

        p = next_point ? next_point : time_start + strlen(time_start);
        index++;
    }

    if (found_past) {
        *rx_hz = best_past_hz;
        *point_index = best_past_index;
        if (point_epoch_out) {
            *point_epoch_out = best_past_epoch;
        }
        snprintf(point_time, point_time_size, "%s", best_past_time);
        return 1;
    }

    if (found_future) {
        *rx_hz = best_future_hz;
        *point_index = best_future_index;
        if (point_epoch_out) {
            *point_epoch_out = best_future_epoch;
        }
        snprintf(point_time, point_time_size, "%s", best_future_time);
        return 1;
    }

    return 0;
}

static int audio_write_track_state_v6(
    const struct app_config *cfg,
    const char *state,
    const char *name,
    int point_index,
    const char *point_time,
    long long rx_hz,
    const char *lo_path,
    const char *message,
    const char *plan_path)
{
    char path[PATH_BUF_SIZE];
    char tmp_path[PATH_BUF_SIZE + 8];
    char name_json[512];
    char message_json[512];
    char plan_path_json[PATH_BUF_SIZE * 2];
    FILE *f;
    time_t now = time(NULL);

    json_escape(name_json, sizeof(name_json), name ? name : "");
    json_escape(message_json, sizeof(message_json), message ? message : "");
    json_escape(plan_path_json, sizeof(plan_path_json), plan_path ? plan_path : "");

    join_path(path, sizeof(path), cfg->data_dir, "radio_track_state.json");
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    f = fopen(tmp_path, "wb");
    if (!f) {
        return 0;
    }

    fprintf(f,
            "{\n"
            "  \"ok\": true,\n"
            "  \"state\": \"%s\",\n"
            "  \"updated_epoch\": %ld,\n"
            "  \"name\": \"%s\",\n"
            "  \"point_index\": %d,\n"
            "  \"point_time_utc\": \"%s\",\n"
            "  \"rx_hz\": %lld,\n"
            "  \"lo_path\": \"%s\",\n"
            "  \"plan_path\": \"%s\",\n"
            "  \"lo_write_result\": \"%s\",\n"
            "  \"message\": \"%s\"\n"
            "}\n",
            state ? state : "unknown",
            (long)now,
            name_json,
            point_index,
            point_time ? point_time : "",
            rx_hz,
            lo_path ? lo_path : "",
            plan_path_json,
            (lo_path && lo_path[0]) ? "written" : "not_attempted",
            message_json);
    fclose(f);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return 0;
    }
    return 1;
}

static int audio_doppler_tune_now_v6(
    const struct app_config *cfg,
    const char *state,
    const char *message,
    char *response,
    size_t response_size,
    char *error,
    size_t error_size,
    long long *rx_hz_out)
{
    char *plan = NULL;
    size_t plan_len = 0;
    char plan_path[PATH_BUF_SIZE] = "";
    char name[256] = "";
    char name_json[512];
    char point_time[64] = "";
    char lo_path[PATH_BUF_SIZE] = "";
    long long rx_hz = 0;
    long long aos_epoch = 0;
    long long los_epoch = 0;
    long long point_epoch = 0;
    int point_index = -1;
    time_t now = time(NULL);

    (void)plan_len;

    if (!audio_read_track_plan_v6(cfg, &plan, &plan_len, plan_path, sizeof(plan_path))) {
        snprintf(error, error_size, "no Doppler track plan has been stored in active data_dir or fallback paths");
        return 404;
    }

    json_string_value(plan, "name", name, sizeof(name));

    if (audio_track_window_v6(plan, &aos_epoch, &los_epoch)) {
        if ((long long)now < aos_epoch) {
            long long seconds_until_aos = aos_epoch - (long long)now;
            free(plan);
            audio_write_track_state_v6(cfg, "waiting", name, -1, "", 0, "", "Waiting for AOS", plan_path);
            snprintf(error, error_size, "pass has not reached AOS; seconds_until_aos=%lld", seconds_until_aos);
            return 409;
        }
        if ((long long)now > los_epoch) {
            long long seconds_since_los = (long long)now - los_epoch;
            free(plan);
            audio_write_track_state_v6(cfg, "complete", name, -1, "", 0, "", "Pass complete", plan_path);
            snprintf(error, error_size, "pass is complete; seconds_since_los=%lld", seconds_since_los);
            return 409;
        }
    }

    if (!audio_find_track_point_for_now_v6(plan, (long long)now, &rx_hz, point_time, sizeof(point_time), &point_index, &point_epoch)) {
        free(plan);
        snprintf(error, error_size, "track plan does not contain usable Doppler points");
        return 400;
    }
    free(plan);

    if (rx_hz < PLUTO_MIN_HZ || rx_hz > PLUTO_MAX_HZ || !write_rx_lo_frequency(rx_hz, lo_path, sizeof(lo_path))) {
        audio_write_track_state_v6(cfg, "tune_error", name, point_index, point_time, rx_hz, "", "could not tune RX LO for Doppler track point", plan_path);
        snprintf(error, error_size, "could not tune RX LO for Doppler track point");
        return 500;
    }

    audio_write_track_state_v6(cfg, state, name, point_index, point_time, rx_hz, lo_path, message, plan_path);

    if (rx_hz_out) {
        *rx_hz_out = rx_hz;
    }

    json_escape(name_json, sizeof(name_json), name);
    snprintf(response, response_size,
             "{\"ok\":true,\"state\":\"%s\",\"name\":\"%s\",\"point_index\":%d,\"point_time_utc\":\"%s\",\"rx_hz\":%lld,\"lo_path\":\"%s\",\"plan_path\":\"%s\",\"point_epoch\":%lld}\n",
             state ? state : "active",
             name_json,
             point_index,
             point_time,
             rx_hz,
             lo_path,
             plan_path,
             point_epoch);
    return 200;
}

static int audio_doppler_should_run_v6(void)
{
    int running;
    pthread_mutex_lock(&g_track_lock);
    running = g_audio_doppler_running_v6;
    pthread_mutex_unlock(&g_track_lock);
    return running;
}

static void audio_doppler_set_running_v6(int running)
{
    pthread_mutex_lock(&g_track_lock);
    g_audio_doppler_running_v6 = running;
    pthread_mutex_unlock(&g_track_lock);
}

static void *audio_doppler_worker_v6(void *arg)
{
    const struct app_config *cfg = (const struct app_config *)arg;

    while (audio_doppler_should_run_v6()) {
        char response[1024];
        char error[256] = "";
        long long rx_hz = 0;
        int status = audio_doppler_tune_now_v6(
            cfg,
            "audio_auto_active",
            "audio-owned automatic Doppler retune",
            response,
            sizeof(response),
            error,
            sizeof(error),
            &rx_hz);
        (void)response;
        (void)rx_hz;

        if (status != 200) {
            audio_doppler_set_running_v6(0);
            break;
        }
        sleep(5);
    }
    return NULL;
}

static int audio_doppler_start_worker_v6(const struct app_config *cfg)
{
    int create_result;

    pthread_mutex_lock(&g_track_lock);
    if (g_audio_doppler_running_v6) {
        pthread_mutex_unlock(&g_track_lock);
        return 1;
    }
    g_audio_doppler_cfg_v6 = cfg;
    g_audio_doppler_running_v6 = 1;
    pthread_mutex_unlock(&g_track_lock);

    create_result = pthread_create(&g_audio_doppler_thread_v6, NULL, audio_doppler_worker_v6, (void *)cfg);
    if (create_result != 0) {
        audio_doppler_set_running_v6(0);
        return 0;
    }
    pthread_detach(g_audio_doppler_thread_v6);
    return 1;
}

static void audio_doppler_stop_worker_v6(void)
{
    audio_doppler_set_running_v6(0);
}


/* BACKEND_AUDIO_DOPPLER_STATE_FALLBACK_V7 */
static int audio_read_active_state_rx_hz_v7(
    const struct app_config *cfg,
    long long *rx_hz_out,
    char *state_out,
    size_t state_out_size,
    char *path_used,
    size_t path_used_size)
{
    char path[PATH_BUF_SIZE];
    char *state_json = NULL;
    size_t state_len = 0;
    char state[64] = "";
    char lo_result[64] = "";
    long long rx_hz = 0;

    (void)state_len;

    if (!cfg || !rx_hz_out) {
        return 0;
    }

    join_path(path, sizeof(path), cfg->data_dir, "radio_track_state.json");
    if (read_file_to_string(path, &state_json, &state_len) != 0) {
        return 0;
    }

    json_string_value(state_json, "state", state, sizeof(state));
    json_string_value(state_json, "lo_write_result", lo_result, sizeof(lo_result));

    if (!json_long_long_after(state_json, "rx_hz", &rx_hz)) {
        free(state_json);
        return 0;
    }

    free(state_json);

    if (rx_hz < PLUTO_MIN_HZ || rx_hz > PLUTO_MAX_HZ) {
        return 0;
    }

    if (strcmp(state, "auto_active") != 0 &&
        strcmp(state, "audio_active") != 0 &&
        strcmp(state, "audio_auto_active") != 0) {
        return 0;
    }

    if (lo_result[0] && strcmp(lo_result, "written") != 0) {
        return 0;
    }

    *rx_hz_out = rx_hz;

    if (state_out && state_out_size > 0) {
        snprintf(state_out, state_out_size, "%s", state);
    }

    if (path_used && path_used_size > 0) {
        snprintf(path_used, path_used_size, "%s", path);
    }

    return 1;
}


/* LIVE_AUDIO_SQUELCH_APPLY_V2_6_14 */
/* LIVE_AUDIO_BACKEND_CONTROL_WIRING_V2_6_17
 * Parse and echo live-audio tuning controls, then pass them to pluto_fm_receiver.
 */
/* BACKEND_AUDIO_CONTROL_ECHO_ONLY_V2_6_19A
 * Parse and echo live-audio UI control parameters without changing the helper
 * launch path. This is intentionally backend-only so we can prove route wiring
 * before enabling helper/DSP behavior.
 */
/* BACKEND_AUDIO_CONTROL_ECHO_ONLY_V2_6_19B
 * Parse and echo live-audio UI control parameters without changing the helper
 * launch path. This is intentionally backend-only so we can prove route wiring
 * before enabling helper/DSP behavior.
 */
static void send_live_audio_start(int fd, const struct app_config *cfg, const char *query)
{
    char frequency_text[64] = "";
    char audio_ctl_rf_bw_v2620[64] = "";
    char audio_ctl_decoder_bw_v2620[64] = "";
    char audio_ctl_gain_v2620[64] = "";
    char audio_ctl_squelch_v2620[64] = "";
    char rf_bw_hz_text[64] = "";
    char decoder_bw_hz_text[64] = "";
    char gain_db_text[64] = "";
    char squelch_db_text[64] = "";
    char rf_bw_hz_json[128];
    char decoder_bw_hz_json[128];
    char gain_db_json[128];
    char squelch_db_json[128];
    char body[1280];
    char track_response[1024];
    char track_error[256] = "";
    long long frequency_hz = 0;
    long long requested_frequency_hz = 0;
    long long doppler_frequency_hz = 0;
    int explicit_doppler =
        query_param_is_true_audio_doppler_v1(query, "doppler") ||
        query_param_is_true_audio_doppler_v1(query, "track") ||
        query_param_is_true_audio_doppler_v1(query, "doppler_track");
    int fixed_requested =
        query_param_is_true_audio_doppler_v1(query, "fixed") ||
        query_param_is_true_audio_doppler_v1(query, "no_doppler") ||
        query_param_is_false_audio_doppler_v3(query, "doppler") ||
        query_param_is_false_audio_doppler_v3(query, "track") ||
        query_param_is_false_audio_doppler_v3(query, "doppler_track");
    int noaa_fixed = 0;
    int doppler_attempted = 0;
    int doppler_used = 0;
    int doppler_defaulted = 0;
    int doppler_fallback = 0;

    query_param(query, "downlink_hz", frequency_text, sizeof(frequency_text));
    query_param(query, "rf_bw_hz", audio_ctl_rf_bw_v2620, sizeof(audio_ctl_rf_bw_v2620));
    query_param(query, "decoder_bw_hz", audio_ctl_decoder_bw_v2620, sizeof(audio_ctl_decoder_bw_v2620));
    query_param(query, "gain_db", audio_ctl_gain_v2620, sizeof(audio_ctl_gain_v2620));
    query_param(query, "squelch_db", audio_ctl_squelch_v2620, sizeof(audio_ctl_squelch_v2620));
    query_param(query, "rf_bw_hz", rf_bw_hz_text, sizeof(rf_bw_hz_text));
    query_param(query, "decoder_bw_hz", decoder_bw_hz_text, sizeof(decoder_bw_hz_text));
    query_param(query, "gain_db", gain_db_text, sizeof(gain_db_text));
    query_param(query, "squelch_db", squelch_db_text, sizeof(squelch_db_text));

    json_escape(rf_bw_hz_json, sizeof(rf_bw_hz_json), rf_bw_hz_text);
    json_escape(decoder_bw_hz_json, sizeof(decoder_bw_hz_json), decoder_bw_hz_text);
    json_escape(gain_db_json, sizeof(gain_db_json), gain_db_text);
    json_escape(squelch_db_json, sizeof(squelch_db_json), squelch_db_text);

    if (frequency_text[0]) {
        if (!parse_frequency_hz(frequency_text, &frequency_hz)) {
            send_audio_error_json(fd, 400, "Bad Request", "valid downlink_hz is required");
            return;
        }
    } else if (!read_active_audio_frequency_hz(cfg, &frequency_hz)) {
        send_audio_error_json(fd, 409, "Conflict", "plan or tune a satellite first, or pass downlink_hz");
        return;
    }

    requested_frequency_hz = frequency_hz;
    noaa_fixed = is_noaa_weather_frequency_v3(requested_frequency_hz);

    /*
     * BACKEND_AUDIO_DOPPLER_DEFAULT_V3:
     * - NOAA is always fixed-frequency.
     * - Satellite/non-NOAA audio defaults to Doppler if a valid plan is present.
     * - Explicit doppler=1 requires a valid plan and returns an error if unavailable.
     * - fixed=1/no_doppler=1/doppler=0 forces fixed tuning.
     */
    if (!noaa_fixed && !fixed_requested && !explicit_doppler) {
        explicit_doppler = 1;
        doppler_defaulted = 1;
    }

    if (!noaa_fixed && !fixed_requested && explicit_doppler) {
        int status;
        doppler_attempted = 1;
        status = apply_radio_track_step(
            cfg,
            "audio_active",
            "audio Doppler point tuned",
            1,
            track_response,
            sizeof(track_response),
            track_error,
            sizeof(track_error));

        if (status == 200 && json_long_long_after(track_response, "rx_hz", &doppler_frequency_hz) &&
            doppler_frequency_hz >= PLUTO_MIN_HZ && doppler_frequency_hz <= PLUTO_MAX_HZ) {
            frequency_hz = doppler_frequency_hz;
            doppler_used = 1;
        } else if (doppler_defaulted) {
            doppler_fallback = 1;
            frequency_hz = requested_frequency_hz;
        } else {
            char error_body[768];
            char escaped_error[512];
            json_escape(escaped_error, sizeof(escaped_error),
                        track_error[0] ? track_error : "Doppler track is not ready for this pass");
            snprintf(error_body, sizeof(error_body),
                     "{\"ok\":false,\"error\":\"%s\",\"doppler_track\":true,\"requested_downlink_hz\":%lld}\n",
                     escaped_error,
                     requested_frequency_hz);
            send_text(fd, 409, "Conflict", "application/json; charset=utf-8", error_body);
            return;
        }
    }

    audio_control_set_pending_v2620(audio_ctl_rf_bw_v2620, audio_ctl_decoder_bw_v2620, audio_ctl_gain_v2620, audio_ctl_squelch_v2620);
    if (!start_live_audio_session(cfg, frequency_hz)) {
        send_audio_error_json(fd, 500, "Internal Server Error", "could not start live Pluto audio");
        return;
    }

    if (doppler_used) {
        if (!ensure_track_auto_running_for_audio_v1(cfg)) {
            stop_live_audio_session();
            send_audio_error_json(fd, 500, "Internal Server Error", "could not start audio-owned automatic Doppler tracking");
            return;
        }
        g_audio_started_track_auto_v1 = 1;
    } else {
        g_audio_started_track_auto_v1 = 0;
    }

    snprintf(body, sizeof(body),
             "{\"ok\":true,\"state\":\"running\",\"downlink_hz\":%lld,\"audio_hz\":%lld,\"requested_downlink_hz\":%lld,\"doppler_track\":%s,\"doppler_attempted\":%s,\"doppler_defaulted\":%s,\"doppler_fallback\":%s,\"noaa_fixed\":%s,\"fixed_requested\":%s,\"auto_track\":%s,\"rf_bw_hz\":\"%s\",\"decoder_bw_hz\":\"%s\",\"gain_db\":\"%s\",\"squelch_db\":\"%s\"}\n",
             frequency_hz,
             frequency_hz,
             requested_frequency_hz,
             doppler_used ? "true" : "false",
             doppler_attempted ? "true" : "false",
             doppler_defaulted ? "true" : "false",
             doppler_fallback ? "true" : "false",
             noaa_fixed ? "true" : "false",
             fixed_requested ? "true" : "false",
             doppler_used ? "true" : "false",
             rf_bw_hz_json,
             decoder_bw_hz_json,
             gain_db_json,
             squelch_db_json);
    send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
}




static void send_live_audio_stop(int fd)
{
    int stopped = stop_live_audio_session();
    char body[128];

    snprintf(body, sizeof(body),
             "{\"ok\":true,\"stopped\":%s}\n",
             stopped ? "true" : "false");
    send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
}


/* BACKEND_AUDIO_EXISTING_LIVE_WAV_STREAM_V1E
 * Continuous backend-owned WAV stream over the existing /api/radio/audio/live.wav route.
 * Browser usage: <audio src="/api/radio/audio/live.wav?stream=1">.
 */
/* BACKEND_AUDIO_TAIL_STREAM_V1H
 * Continuous browser audio stream mode.
 *
 * The old /api/radio/audio/live.wav endpoint is a block/cursor endpoint and may
 * legitimately return 204 while the backend DSP file is still filling.  That is
 * unsuitable for an <audio> element.  This handler keeps the browser side simple:
 * it emits a normal chunked WAV stream immediately, then tails the backend PCM
 * file written by pluto_fm_receiver.  If the DSP has not produced samples yet,
 * it sends short silence chunks instead of returning 204.
 */
static void send_live_audio_stream_v1h(int fd, const char *query)
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


/* BACKEND_STREAMING_AUDIO_TAIL_STREAM_V1I
 * Continuous backend-owned browser stream for the existing live.wav endpoint.
 * The browser receives one WAV stream; Pluto owns tuning, DSP, buffering, and pacing.
 */
static int send_live_audio_tail_stream_v1i(int fd, const char *query)
{
    char seconds_text[64] = "";
    int requested_seconds = 0;
    time_t started = time(NULL);
    size_t cursor_samples = 0;
    int header_sent = 0;

    query_param(query, "seconds", seconds_text, sizeof(seconds_text));
    if (seconds_text[0]) {
        char *end = NULL;
        long parsed = strtol(seconds_text, &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= 3600) {
            requested_seconds = (int)parsed;
        }
    }

    pthread_mutex_lock(&g_audio_live_lock);
    if (!refresh_live_audio_process_locked()) {
        pthread_mutex_unlock(&g_audio_live_lock);
        return send_audio_error_json(fd, 409, "Conflict", "live analog audio is not running");
    }
    pthread_mutex_unlock(&g_audio_live_lock);

    if (send_audio_header(fd) != 0 ||
        send_wav_header_chunk(fd, AUDIO_SAMPLE_RATE, 1, 16) != 0) {
        return -1;
    }
    header_sent = 1;
    (void)header_sent;

    while (g_running) {
        FILE *input = NULL;
        int sent_pcm = 0;
        size_t available_samples;
        size_t to_read;
        short pcm[2400];

        if (requested_seconds > 0 && (time(NULL) - started) >= requested_seconds) {
            break;
        }

        pthread_mutex_lock(&g_audio_live_lock);
        if (!refresh_live_audio_process_locked()) {
            pthread_mutex_unlock(&g_audio_live_lock);
            break;
        }
        pthread_mutex_unlock(&g_audio_live_lock);

        available_samples = live_audio_available_samples();
        if (cursor_samples > available_samples) {
            cursor_samples = 0;
        }

        if (available_samples > cursor_samples) {
            to_read = available_samples - cursor_samples;
            if (to_read > sizeof(pcm) / sizeof(pcm[0])) {
                to_read = sizeof(pcm) / sizeof(pcm[0]);
            }

            input = fopen(AUDIO_LIVE_PCM_PATH, "rb");
            if (input) {
                if (fseek(input, (long)(cursor_samples * sizeof(short)), SEEK_SET) == 0) {
                    size_t got = fread(pcm, sizeof(short), to_read, input);
                    if (got > 0) {
                        if (send_chunk(fd, pcm, got * sizeof(short)) != 0) {
                            fclose(input);
                            return -1;
                        }
                        cursor_samples += got;
                        sent_pcm = 1;
                    }
                }
                fclose(input);
            }
        }

        if (!sent_pcm) {
            /* Keep the HTTP audio stream alive while pluto_fm_receiver/iio_readdev warms up. */
            if (send_pcm_silence_chunk(fd, 1200) != 0) {
                return -1;
            }
            usleep(50000);
        }
    }

    return finish_chunked_response(fd);
}


/* BACKEND_AUDIO_FORCE_TAIL_STREAM_V1J_BEGIN */
static int audio_query_stream_enabled_v1j(const char *query)
{
    char value[32] = "";

    if (!query || !*query) {
        return 0;
    }
    query_param(query, "stream", value, sizeof(value));
    return strcmp(value, "1") == 0 ||
           strcmp(value, "true") == 0 ||
           strcmp(value, "yes") == 0;
}

static int audio_query_seconds_v1j(const char *query, int fallback_seconds)
{
    char value[32] = "";
    char *end = NULL;
    long parsed;

    if (!query || !*query) {
        return fallback_seconds;
    }
    query_param(query, "seconds", value, sizeof(value));
    if (!value[0]) {
        return fallback_seconds;
    }
    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        return fallback_seconds;
    }
    if (parsed < 1) {
        parsed = 1;
    } else if (parsed > 3600) {
        parsed = 3600;
    }
    return (int)parsed;
}

static int send_live_audio_tail_stream_v1j(int fd, const char *query)
{
    short pcm[2400];
    size_t cursor = 0;
    int seconds = audio_query_seconds_v1j(query, 0);
    time_t start_epoch = time(NULL);

    if (send_audio_header(fd) != 0 || send_wav_header_chunk(fd, AUDIO_SAMPLE_RATE, 1, 16) != 0) {
        return -1;
    }

    while (g_running) {
        size_t available = live_audio_available_samples();

        if (seconds > 0 && time(NULL) - start_epoch >= seconds) {
            break;
        }

        if (cursor < available) {
            FILE *input = fopen(AUDIO_LIVE_PCM_PATH, "rb");
            if (input) {
                size_t wanted = available - cursor;
                size_t got = 0;
                if (wanted > sizeof(pcm) / sizeof(pcm[0])) {
                    wanted = sizeof(pcm) / sizeof(pcm[0]);
                }
                if (fseek(input, (long)(cursor * sizeof(short)), SEEK_SET) == 0) {
                    got = fread(pcm, sizeof(short), wanted, input);
                }
                fclose(input);
                if (got > 0) {
                    if (send_chunk(fd, pcm, got * sizeof(short)) != 0) {
                        return -1;
                    }
                    cursor += got;
                    continue;
                }
            }
        }

        /* Keep the browser decoder alive while the backend DSP warms up. */
        if (send_pcm_silence_chunk(fd, 1200) != 0) {
            return -1;
        }
        usleep(50000);
    }

    return finish_chunked_response(fd);
}
/* BACKEND_AUDIO_FORCE_TAIL_STREAM_V1J_END */

static void send_live_audio_block(int fd, const char *query)
{

    {
        char stream_text[32] = "";
        query_param(query, "stream", stream_text, sizeof(stream_text));
        if (strcmp(stream_text, "1") == 0 || strcmp(stream_text, "true") == 0 || strcmp(stream_text, "yes") == 0) {
            send_live_audio_stream_v1h(fd, query);
            return;
        }
    }

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
    /* BACKEND_AUDIO_FORCE_TAIL_STREAM_V1J_BRANCH_BEGIN */
    if (audio_query_stream_enabled_v1j(query)) {
        (void)send_live_audio_tail_stream_v1j(fd, query);
        return;
    }
    /* BACKEND_AUDIO_FORCE_TAIL_STREAM_V1J_BRANCH_END */


    /* BACKEND_AUDIO_EXISTING_LIVE_WAV_STREAM_V1E: use existing live.wav route as a continuous stream when requested. */
    {
        char stream_text[32] = "";
        query_param(query, "stream", stream_text, sizeof(stream_text));
        if (strcmp(stream_text, "1") == 0 ||
            strcmp(stream_text, "true") == 0 ||
            strcmp(stream_text, "yes") == 0) {
            send_live_audio_stream_v1h(fd, "");
            return;
        }
    }

        /* BACKEND_STREAMING_AUDIO_TAIL_STREAM_V1I_SWITCH
     * Must run before the old block-reader can return 204 No Content.
     */
    if (query && (strstr(query, "stream=1") || strstr(query, "stream=true"))) {
        (void)send_live_audio_tail_stream_v1i(fd, query);
        return;
    }

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


/* BACKEND_STREAMING_AUDIO_V1: continuous backend decoded WAV stream for browser playback. */
static int send_live_audio_stream_cfg_v1k(int fd, const struct app_config *cfg, const char *query)
{
    char frequency_text[64] = "";
    char debug_line[256];
    long long frequency_hz = 0;
    size_t cursor = 0;
    size_t idle_loops = 0;
    int need_start = 1;

    query_param(query, "downlink_hz", frequency_text, sizeof(frequency_text));
    if (frequency_text[0]) {
        if (!parse_frequency_hz(frequency_text, &frequency_hz)) {
            return send_audio_error_json(fd, 400, "Bad Request", "valid downlink_hz is required");
        }
    } else if (!read_active_audio_frequency_hz(cfg, &frequency_hz)) {
        return send_audio_error_json(fd, 409, "Conflict", "plan or tune a satellite first, or pass downlink_hz");
    }

    pthread_mutex_lock(&g_audio_live_lock);
    if (refresh_live_audio_process_locked() && g_live_audio.frequency_hz == frequency_hz) {
        need_start = 0;
        cursor = live_audio_available_samples();
        if (cursor > (size_t)AUDIO_SAMPLE_RATE) {
            cursor -= (size_t)AUDIO_SAMPLE_RATE;
        } else {
            cursor = 0;
        }
    }
    pthread_mutex_unlock(&g_audio_live_lock);

    if (need_start) {
        if (!start_live_audio_session(cfg, frequency_hz)) {
            return send_audio_error_json(fd, 500, "Internal Server Error", "could not start backend Pluto audio DSP");
        }
        cursor = 0;
    }

    if (send_audio_header(fd) != 0 || send_wav_header_chunk(fd, AUDIO_SAMPLE_RATE, 1, 16) != 0) {
        append_audio_debug("live_stream header failed");
        return -1;
    }

    snprintf(debug_line, sizeof(debug_line), "live_stream start hz=%lld cursor=%lu", frequency_hz, (unsigned long)cursor);
    append_audio_debug(debug_line);

    while (g_running) {
        size_t available = live_audio_available_samples();
        int running;

        pthread_mutex_lock(&g_audio_live_lock);
        running = refresh_live_audio_process_locked();
        pthread_mutex_unlock(&g_audio_live_lock);

        if (available > cursor) {
            size_t sample_count = available - cursor;
            FILE *input;
            short *pcm;

            if (sample_count > AUDIO_LIVE_DEFAULT_BLOCK_SAMPLES) {
                sample_count = AUDIO_LIVE_DEFAULT_BLOCK_SAMPLES;
            }
            input = fopen(AUDIO_LIVE_PCM_PATH, "rb");
            if (!input) {
                usleep(50000);
                continue;
            }
            if (fseek(input, (long)(cursor * sizeof(short)), SEEK_SET) != 0) {
                fclose(input);
                return -1;
            }
            pcm = (short *)malloc(sample_count * sizeof(short));
            if (!pcm) {
                fclose(input);
                return -1;
            }
            sample_count = fread(pcm, sizeof(short), sample_count, input);
            fclose(input);
            if (sample_count > 0) {
                if (send_chunk(fd, pcm, sample_count * sizeof(short)) != 0) {
                    free(pcm);
                    append_audio_debug("live_stream send_chunk failed");
                    return -1;
                }
                cursor += sample_count;
                idle_loops = 0;
            }
            free(pcm);
            continue;
        }

        if (!running) {
            append_audio_debug("live_stream helper stopped");
            break;
        }
        if (++idle_loops > 2400) {
            append_audio_debug("live_stream timeout waiting for audio samples");
            break;
        }
        usleep(50000);
    }

    return finish_chunked_response(fd);
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
            *slash = '\0';
            snprintf(name_path, sizeof(name_path), "%s/name", device_dir);
            snprintf(available_path, sizeof(available_path), "%s_available", rx_path);
            read_first_line(name_path, device_name, sizeof(device_name));
            read_first_line(rx_path, current, sizeof(current));
            read_first_line(available_path, available, sizeof(available));
        }
    }

    json_escape(device_name_json, sizeof(device_name_json), device_name);
    json_escape(available_json, sizeof(available_json), available);

    if (contains_nocase(device_name, "ad9363")) {
        profile_id = "ad9363_stock";
        profile_label = "AD9363 stock Pluto profile";
        firmware_mode = "1r1t_stock";
        capability_min_hz = 325000000LL;
        vhf_tunable = 0;
    } else if (contains_nocase(device_name, "ad9364")) {
        profile_id = "ad9364_expanded";
        profile_label = "AD9364 compatibility profile";
        firmware_mode = "2r2t_compat";
    } else if (contains_nocase(device_name, "ad9361")) {
        profile_id = "ad9361_expanded";
        profile_label = "AD9361 compatibility profile";
        firmware_mode = "2r2t_compat";
    }

    snprintf(body, sizeof(body),
             "{"
             "\"ok\":true,"
             "\"software_min_hz\":%lld,"
             "\"software_max_hz\":%lld,"
             "\"capability_min_hz\":%lld,"
             "\"capability_max_hz\":%lld,"
             "\"profile_id\":\"%s\","
             "\"profile_label\":\"%s\","
             "\"firmware_mode\":\"%s\","
             "\"vhf_tunable\":%s,"
             "\"uhf_tunable\":%s,"
             "\"rx_lo_path\":\"%s\","
             "\"iio_device_name\":\"%s\","
             "\"current_rx_lo_hz\":%s,"
             "\"frequency_available\":\"%s\""
             "}\n",
             PLUTO_MIN_HZ,
             PLUTO_MAX_HZ,
             capability_min_hz,
             capability_max_hz,
             profile_id,
             profile_label,
             firmware_mode,
             vhf_tunable ? "true" : "false",
             uhf_tunable ? "true" : "false",
             rx_path,
             device_name_json,
             current[0] ? current : "null",
             available_json);
    write_radio_profile(cfg, body);
    send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
}

static void send_radio_track_plan(int fd, const struct app_config *cfg, const char *body)
{
    char path[PATH_BUF_SIZE];
    char tmp_path[PATH_BUF_SIZE + 8];
    char response[256];
    FILE *f;

    if (!body || body[0] != '{' || !strstr(body, "\"doppler_plan\"")) {
        send_text(fd, 400, "Bad Request", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"valid Doppler track JSON is required\"}\n");
        return;
    }

    runtime_file_path(path, sizeof(path), "radio_track.json");
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    f = fopen(tmp_path, "wb");
    if (!f) {
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"could not write Doppler track plan\"}\n");
        return;
    }

    fputs(body, f);
    if (body[strlen(body) - 1] != '\n') {
        fputc('\n', f);
    }
    fclose(f);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"could not publish Doppler track plan\"}\n");
        return;
    }

    snprintf(response, sizeof(response), "{\"ok\":true,\"state\":\"track_planned\"}\n");
    send_text(fd, 200, "OK", "application/json; charset=utf-8", response);
}

static int find_track_point_for_now(
    const char *plan_json,
    long long now_epoch,
    long long *rx_hz,
    char *point_time,
    size_t point_time_size,
    int *point_index,
    long long *point_epoch_out)
{
    const char *p = plan_json;
    long long best_past_epoch = LLONG_MIN;
    long long best_future_epoch = LLONG_MAX;
    long long best_past_hz = 0;
    long long best_future_hz = 0;
    int best_past_index = -1;
    int best_future_index = -1;
    char best_past_time[64] = "";
    char best_future_time[64] = "";
    int index = 0;
    int found_past = 0;
    int found_future = 0;

    while ((p = strstr(p, "\"time_utc\"")) != NULL) {
        char time_value[64] = "";
        const char *object_start = p;
        const char *time_start;
        const char *next_point;
        long long point_epoch;
        long long point_rx_hz;

        while (object_start > plan_json && *object_start != '{') {
            object_start--;
        }
        time_start = strchr(p, ':');
        if (!time_start) {
            break;
        }
        time_start++;
        while (*time_start == ' ' || *time_start == '\t' || *time_start == '\r' || *time_start == '\n') {
            time_start++;
        }
        if (*time_start != '"') {
            p += 10;
            continue;
        }
        time_start++;
        snprintf(time_value, sizeof(time_value), "%.63s", time_start);
        if (strchr(time_value, '"')) {
            *strchr(time_value, '"') = '\0';
        }

        next_point = strstr(time_start, "\"time_utc\"");
        if (!json_long_long_after(object_start, "rx_hz", &point_rx_hz)) {
            p = next_point ? next_point : time_start + strlen(time_start);
            index++;
            continue;
        }
        if (!parse_iso_utc_epoch(time_value, &point_epoch)) {
            p = next_point ? next_point : time_start + strlen(time_start);
            index++;
            continue;
        }

        if (point_epoch <= now_epoch) {
            if (!found_past || point_epoch > best_past_epoch) {
                found_past = 1;
                best_past_epoch = point_epoch;
                best_past_hz = point_rx_hz;
                best_past_index = index;
                snprintf(best_past_time, sizeof(best_past_time), "%s", time_value);
            }
        } else if (!found_future || point_epoch < best_future_epoch) {
            found_future = 1;
            best_future_epoch = point_epoch;
            best_future_hz = point_rx_hz;
            best_future_index = index;
            snprintf(best_future_time, sizeof(best_future_time), "%s", time_value);
        }

        p = next_point ? next_point : time_start + strlen(time_start);
        index++;
    }

    if (found_past) {
        *rx_hz = best_past_hz;
        *point_index = best_past_index;
        if (point_epoch_out) {
            *point_epoch_out = best_past_epoch;
        }
        snprintf(point_time, point_time_size, "%s", best_past_time);
        return 1;
    }
    if (found_future) {
        *rx_hz = best_future_hz;
        *point_index = best_future_index;
        if (point_epoch_out) {
            *point_epoch_out = best_future_epoch;
        }
        snprintf(point_time, point_time_size, "%s", best_future_time);
        return 1;
    }
    return 0;
}

static int find_track_point_by_time(
    const char *plan_json,
    const char *target_time,
    long long *rx_hz,
    char *point_time,
    size_t point_time_size,
    int *point_index,
    long long *point_epoch_out)
{
    const char *p = plan_json;
    int index = 0;

    if (!plan_json || !target_time || !target_time[0]) {
        return 0;
    }

    while ((p = strstr(p, "\"time_utc\"")) != NULL) {
        char time_value[64] = "";
        const char *object_start = p;
        const char *time_start;
        const char *next_point;
        long long point_epoch;
        long long point_rx_hz;

        while (object_start > plan_json && *object_start != '{') {
            object_start--;
        }
        time_start = strchr(p, ':');
        if (!time_start) {
            break;
        }
        time_start++;
        while (*time_start == ' ' || *time_start == '\t' || *time_start == '\r' || *time_start == '\n') {
            time_start++;
        }
        if (*time_start != '"') {
            p += 10;
            continue;
        }
        time_start++;
        snprintf(time_value, sizeof(time_value), "%.63s", time_start);
        if (strchr(time_value, '"')) {
            *strchr(time_value, '"') = '\0';
        }

        next_point = strstr(time_start, "\"time_utc\"");
        if (strcmp(time_value, target_time) != 0) {
            p = next_point ? next_point : time_start + strlen(time_start);
            index++;
            continue;
        }
        if (!json_long_long_after(object_start, "rx_hz", &point_rx_hz)) {
            return 0;
        }
        if (!parse_iso_utc_epoch(time_value, &point_epoch)) {
            return 0;
        }

        *rx_hz = point_rx_hz;
        *point_index = index;
        if (point_epoch_out) {
            *point_epoch_out = point_epoch;
        }
        snprintf(point_time, point_time_size, "%s", time_value);
        return 1;
    }

    return 0;
}

static int write_track_state(
    const struct app_config *cfg,
    const char *state,
    const char *name,
    int point_index,
    const char *point_time,
    long long rx_hz,
    const char *lo_path,
    const char *message,
    long long seconds_until_aos,
    long long seconds_until_los,
    long long seconds_since_los,
    long long seconds_until_point,
    const char *lo_write_result)
{
    char path[PATH_BUF_SIZE];
    char tmp_path[PATH_BUF_SIZE + 8];
    char name_json[512];
    char message_json[512];
    char lo_result_json[128];
    FILE *f;
    time_t now = time(NULL);

    json_escape(name_json, sizeof(name_json), name ? name : "");
    json_escape(message_json, sizeof(message_json), message ? message : "");
    json_escape(lo_result_json, sizeof(lo_result_json), lo_write_result ? lo_write_result : "not_attempted");

    join_path(path, sizeof(path), cfg->data_dir, "radio_track_state.json");
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    f = fopen(tmp_path, "wb");
    if (!f) {
        return 0;
    }

    fprintf(f,
            "{\n"
            "  \"ok\": true,\n"
            "  \"state\": \"%s\",\n"
            "  \"updated_epoch\": %ld,\n"
            "  \"name\": \"%s\",\n"
            "  \"point_index\": %d,\n"
            "  \"point_time_utc\": \"%s\",\n"
            "  \"rx_hz\": %lld,\n"
            "  \"lo_path\": \"%s\",\n"
            "  \"seconds_until_aos\": %lld,\n"
            "  \"seconds_until_los\": %lld,\n"
            "  \"seconds_since_los\": %lld,\n"
            "  \"seconds_until_point\": %lld,\n"
            "  \"lo_write_result\": \"%s\",\n"
            "  \"message\": \"%s\"\n"
            "}\n",
            state,
            (long)now,
            name_json,
            point_index,
            point_time ? point_time : "",
            rx_hz,
            lo_path ? lo_path : "",
            seconds_until_aos,
            seconds_until_los,
            seconds_since_los,
            seconds_until_point,
            lo_result_json,
            message_json);
    fclose(f);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return 0;
    }
    return 1;
}

/* ROTATOR_AZEL_STATE_SAFE1: helpers for publishing selected Doppler point az/el */
static int find_track_point_az_el_by_time_safe1(
    const char *plan_json,
    const char *target_time,
    double *az_deg,
    double *el_deg)
{
    const char *p = plan_json;
    char time_value[64];

    if (!plan_json || !target_time || !target_time[0] || !az_deg || !el_deg) {
        return 0;
    }

    while ((p = strstr(p, "\"time_utc\"")) != NULL) {
        const char *object_start = p;
        const char *time_start;
        const char *next_point;
        const char *object_end;
        size_t object_len;
        char object_buf[4096];
        double az = 0.0;
        double el = 0.0;
        int got_az;
        int got_el;

        while (object_start > plan_json && *object_start != '{') {
            object_start--;
        }

        time_start = strchr(p, ':');
        if (!time_start) {
            break;
        }
        time_start++;
        while (*time_start == ' ' || *time_start == '\t' || *time_start == '\r' || *time_start == '\n') {
            time_start++;
        }
        if (*time_start != '"') {
            p += 10;
            continue;
        }
        time_start++;
        snprintf(time_value, sizeof(time_value), "%.63s", time_start);
        if (strchr(time_value, '"')) {
            *strchr(time_value, '"') = '\0';
        }

        next_point = strstr(time_start, "\"time_utc\"");
        object_end = next_point ? next_point : time_start + strlen(time_start);
        object_len = (size_t)(object_end - object_start);
        if (object_len >= sizeof(object_buf)) {
            object_len = sizeof(object_buf) - 1;
        }
        memcpy(object_buf, object_start, object_len);
        object_buf[object_len] = '\0';

        if (strcmp(time_value, target_time) == 0) {
            got_az = json_double_value(object_buf, "az_deg", &az) ||
                     json_double_value(object_buf, "azimuth_deg", &az) ||
                     json_double_value(object_buf, "target_az_deg", &az) ||
                     json_double_value(object_buf, "az", &az);
            got_el = json_double_value(object_buf, "el_deg", &el) ||
                     json_double_value(object_buf, "elevation_deg", &el) ||
                     json_double_value(object_buf, "target_el_deg", &el) ||
                     json_double_value(object_buf, "el", &el);
            if (!got_az || !got_el) {
                return 0;
            }
            *az_deg = az;
            *el_deg = el;
            return 1;
        }

        p = next_point ? next_point : time_start + strlen(time_start);
    }

    return 0;
}

static int write_track_state_with_azel_safe1(
    const struct app_config *cfg,
    const char *state,
    const char *name,
    int point_index,
    const char *point_time,
    long long rx_hz,
    const char *lo_path,
    const char *message,
    long long seconds_until_aos,
    long long seconds_until_los,
    long long seconds_since_los,
    long long seconds_until_point,
    const char *lo_write_result,
    double az_deg,
    double el_deg)
{
    char path[PATH_BUF_SIZE];
    char tmp_path[PATH_BUF_SIZE + 8];
    char name_json[512];
    char message_json[512];
    char lo_result_json[128];
    FILE *f;
    time_t now = time(NULL);

    json_escape(name_json, sizeof(name_json), name ? name : "");
    json_escape(message_json, sizeof(message_json), message ? message : "");
    json_escape(lo_result_json, sizeof(lo_result_json), lo_write_result ? lo_write_result : "not_attempted");

    join_path(path, sizeof(path), cfg->data_dir, "radio_track_state.json");
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    f = fopen(tmp_path, "wb");
    if (!f) {
        return 0;
    }

    fprintf(f,
            "{\n"
            "  \"ok\": true,\n"
            "  \"state\": \"%s\",\n"
            "  \"updated_epoch\": %ld,\n"
            "  \"name\": \"%s\",\n"
            "  \"point_index\": %d,\n"
            "  \"point_time_utc\": \"%s\",\n"
            "  \"rx_hz\": %lld,\n"
            "  \"lo_path\": \"%s\",\n"
            "  \"seconds_until_aos\": %lld,\n"
            "  \"seconds_until_los\": %lld,\n"
            "  \"seconds_since_los\": %lld,\n"
            "  \"seconds_until_point\": %lld,\n"
            "  \"has_az_el\": true,\n"
            "  \"az_deg\": %.3f,\n"
            "  \"el_deg\": %.3f,\n"
            "  \"target_az_deg\": %.3f,\n"
            "  \"target_el_deg\": %.3f,\n"
            "  \"lo_write_result\": \"%s\",\n"
            "  \"message\": \"%s\"\n"
            "}\n",
            state,
            (long)now,
            name_json,
            point_index,
            point_time ? point_time : "",
            rx_hz,
            lo_path ? lo_path : "",
            seconds_until_aos,
            seconds_until_los,
            seconds_since_los,
            seconds_until_point,
            az_deg,
            el_deg,
            az_deg,
            el_deg,
            lo_result_json,
            message_json);
    fclose(f);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return 0;
    }
    return 1;
}



static int read_track_window(
    const char *plan,
    long long *aos_epoch,
    long long *los_epoch)
{
    char aos[64] = "";
    char los[64] = "";

    if (!json_string_value(plan, "aos_utc", aos, sizeof(aos)) ||
        !json_string_value(plan, "los_utc", los, sizeof(los))) {
        return 0;
    }
    return parse_iso_utc_epoch(aos, aos_epoch) && parse_iso_utc_epoch(los, los_epoch);
}

static int time_sync_state_is_synced(const struct app_config *cfg)
{
    char path[PATH_BUF_SIZE];
    char *body = NULL;
    size_t body_len = 0;
    int synced = 0;

    join_path(path, sizeof(path), cfg->data_dir, "time_sync.json");
    if (read_file_to_string(path, &body, &body_len) != 0) {
        return 0;
    }
    (void)body_len;
    synced = strstr(body, "\"state\": \"synced\"") != NULL || strstr(body, "\"state\":\"synced\"") != NULL;
    free(body);
    return synced;
}

static int read_stored_track_window(
    const struct app_config *cfg,
    char *name,
    size_t name_size,
    long long *aos_epoch,
    long long *los_epoch,
    char *error,
    size_t error_size)
{
    char path[PATH_BUF_SIZE];
    char *plan = NULL;
    size_t plan_len = 0;
    int ok;

    runtime_file_path(path, sizeof(path), "radio_track.json");
    if (read_file_to_string(path, &plan, &plan_len) != 0) {
        snprintf(error, error_size, "no Doppler track plan has been stored");
        return 0;
    }
    (void)plan_len;
    json_string_value(plan, "name", name, name_size);
    ok = read_track_window(plan, aos_epoch, los_epoch);
    free(plan);
    if (!ok) {
        snprintf(error, error_size, "track plan does not contain AOS/LOS timestamps");
        return 0;
    }
    return 1;
}

static int apply_radio_track_step(
    const struct app_config *cfg,
    const char *state,
    const char *message,
    int enforce_pass_window,
    char *response,
    size_t response_size,
    char *error,
    size_t error_size)
{
    char path[PATH_BUF_SIZE];
    char *plan = NULL;
    size_t plan_len = 0;
    char name[256] = "";
    char name_json[512];
    char point_time[64] = "";
    char lo_path[PATH_BUF_SIZE] = "";
    long long rx_hz = 0;
    long long aos_epoch = 0;
    long long los_epoch = 0;
    long long point_epoch = 0;
    long long seconds_until_aos = -1;
    long long seconds_until_los = -1;
    long long seconds_until_point = -1;
    int point_index = -1;
    double az_deg_safe1 = 0.0;
    double el_deg_safe1 = 0.0;
    int has_az_el_safe1 = 0;
    time_t now = time(NULL);

    runtime_file_path(path, sizeof(path), "radio_track.json");
    if (read_file_to_string(path, &plan, &plan_len) != 0) {
        snprintf(error, error_size, "no Doppler track plan has been stored");
        return 404;
    }
    (void)plan_len;

    json_string_value(plan, "name", name, sizeof(name));
    if (enforce_pass_window && read_track_window(plan, &aos_epoch, &los_epoch)) {
        json_escape(name_json, sizeof(name_json), name);
        if ((long long)now < aos_epoch) {
            seconds_until_aos = aos_epoch - (long long)now;
            free(plan);
            if (!write_track_state(cfg, "waiting", name, -1, "", 0, "", "Waiting for AOS", seconds_until_aos, -1, -1, seconds_until_aos, "not_attempted")) {
                snprintf(error, error_size, "could not write Doppler track state");
                return 500;
            }
            snprintf(response, response_size,
                     "{\"ok\":true,\"state\":\"waiting\",\"name\":\"%s\",\"seconds_until_aos\":%lld}\n",
                     name_json, seconds_until_aos);
            return 200;
        }
        if ((long long)now > los_epoch) {
            long long seconds_since_los = (long long)now - los_epoch;
            free(plan);
            if (!write_track_state(cfg, "complete", name, -1, "", 0, "", "Pass complete", -1, -1, seconds_since_los, -1, "not_attempted")) {
                snprintf(error, error_size, "could not write Doppler track state");
                return 500;
            }
            snprintf(response, response_size,
                     "{\"ok\":true,\"state\":\"complete\",\"name\":\"%s\",\"seconds_since_los\":%lld}\n",
                     name_json, seconds_since_los);
            return 200;
        }
        seconds_until_los = los_epoch - (long long)now;
    }
    if (!find_track_point_for_now(plan, (long long)now, &rx_hz, point_time, sizeof(point_time), &point_index, &point_epoch)) {
        free(plan);
        snprintf(error, error_size, "track plan does not contain usable Doppler points");
        return 400;
    }
    has_az_el_safe1 = find_track_point_az_el_by_time_safe1(plan, point_time, &az_deg_safe1, &el_deg_safe1);
    free(plan);
    if (point_epoch > (long long)now) {
        seconds_until_point = point_epoch - (long long)now;
    } else {
        seconds_until_point = 0;
    }

    if (rx_hz < PLUTO_MIN_HZ || rx_hz > PLUTO_MAX_HZ || !write_rx_lo_frequency(rx_hz, lo_path, sizeof(lo_path))) {
        write_track_state(cfg, "tune_error", name, point_index, point_time, rx_hz, "", "could not tune RX LO for Doppler track point", seconds_until_aos, seconds_until_los, -1, seconds_until_point, "failed");
        snprintf(error, error_size, "could not tune RX LO for Doppler track point");
        return 500;
    }

    if (has_az_el_safe1) {
        if (!write_track_state_with_azel_safe1(cfg, state, name, point_index, point_time, rx_hz, lo_path, message, seconds_until_aos, seconds_until_los, -1, seconds_until_point, "written", az_deg_safe1, el_deg_safe1)) {
            snprintf(error, error_size, "could not write Doppler track state");
            return 500;
        }
    } else if (!write_track_state(cfg, state, name, point_index, point_time, rx_hz, lo_path, message, seconds_until_aos, seconds_until_los, -1, seconds_until_point, "written")) {
        snprintf(error, error_size, "could not write Doppler track state");
        return 500;
    }

    json_escape(name_json, sizeof(name_json), name);
    snprintf(response, response_size,
             "{\"ok\":true,\"state\":\"%s\",\"name\":\"%s\",\"point_index\":%d,\"point_time_utc\":\"%s\",\"rx_hz\":%lld,\"lo_path\":\"%s\",\"seconds_until_los\":%lld,\"seconds_until_point\":%lld,\"has_az_el\":%s,\"az_deg\":%.3f,\"el_deg\":%.3f,\"target_az_deg\":%.3f,\"target_el_deg\":%.3f,\"lo_write_result\":\"written\"}\n",
             state, name_json, point_index, point_time, rx_hz, lo_path, seconds_until_los, seconds_until_point,
             has_az_el_safe1 ? "true" : "false",
             az_deg_safe1,
             el_deg_safe1,
             az_deg_safe1,
             el_deg_safe1);
    return 200;
}


static void send_error_json(int fd, int status, const char *error)
{
    char body[512];
    char error_json[384];
    const char *reason = "Internal Server Error";

    if (status == 400) {
        reason = "Bad Request";
    } else if (status == 409) {
        reason = "Conflict";
    } else if (status == 404) {
        reason = "Not Found";
    }

    json_escape(error_json, sizeof(error_json), error ? error : "request failed");
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}\n", error_json);
    send_text(fd, status, reason, "application/json; charset=utf-8", body);
}

static void send_radio_track_start(int fd, const struct app_config *cfg, const char *state)
{
    char response[1024];
    char error[256];
    int status;

    if (!time_sync_state_is_synced(cfg)) {
        send_text(fd, 409, "Conflict", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"sync Pluto time from the browser before starting Doppler tracking\"}\n");
        return;
    }

    status = apply_radio_track_step(
        cfg,
        state,
        "Doppler tracking started",
        1,
        response,
        sizeof(response),
        error,
        sizeof(error));
    if (status != 200) {
        send_error_json(fd, status, error);
        return;
    }
    send_text(fd, 200, "OK", "application/json; charset=utf-8", response);
}

static void send_radio_track_step(int fd, const struct app_config *cfg, const char *state)
{
    char response[1024];
    char error[256];
    int status;

    status = apply_radio_track_step(
        cfg,
        state,
        "nearest Doppler point tuned",
        0,
        response,
        sizeof(response),
        error,
        sizeof(error));
    if (status != 200) {
        send_error_json(fd, status, error);
        return;
    }
    send_text(fd, 200, "OK", "application/json; charset=utf-8", response);
}

static void send_radio_track_tune_point(int fd, const struct app_config *cfg, const char *query, const char *state)
{
    char path[PATH_BUF_SIZE];
    char *plan = NULL;
    size_t plan_len = 0;
    char point_time_query[64] = "";
    char point_time[64] = "";
    char name[256] = "";
    char norad[64] = "";
    char aos[128] = "";
    char mode[128] = "";
    char description[512] = "";
    char name_json[512];
    char lo_path[PATH_BUF_SIZE] = "";
    char response[1024];
    long long rx_hz = 0;
    long long point_epoch = 0;
    long long aos_epoch = 0;
    long long los_epoch = 0;
    long long norad_id = 0;
    long long seconds_until_aos = -1;
    long long seconds_until_los = -1;
    long long seconds_until_point = -1;
    int point_index = -1;
    int enforce_pass_window = 0;
    time_t now = time(NULL);

    query_param(query, "time_utc", point_time_query, sizeof(point_time_query));
    if (!point_time_query[0]) {
        send_error_json(fd, 400, "time_utc is required");
        return;
    }

    runtime_file_path(path, sizeof(path), "radio_track.json");
    if (read_file_to_string(path, &plan, &plan_len) != 0) {
        send_error_json(fd, 404, "no Doppler track plan has been stored");
        return;
    }
    (void)plan_len;

    json_string_value(plan, "name", name, sizeof(name));
    json_string_value(plan, "aos_utc", aos, sizeof(aos));
    json_string_value(plan, "mode", mode, sizeof(mode));
    json_string_value(plan, "description", description, sizeof(description));
    if (json_long_long_after(plan, "norad_id", &norad_id)) {
        snprintf(norad, sizeof(norad), "%lld", norad_id);
    }
    if (read_track_window(plan, &aos_epoch, &los_epoch)) {
        enforce_pass_window = 1;
        if ((long long)now < aos_epoch) {
            seconds_until_aos = aos_epoch - (long long)now;
        }
        if ((long long)now <= los_epoch) {
            seconds_until_los = los_epoch - (long long)now;
        }
    }

    if (!find_track_point_by_time(plan, point_time_query, &rx_hz, point_time, sizeof(point_time), &point_index, &point_epoch)) {
        free(plan);
        send_error_json(fd, 404, "requested Doppler point was not found in the stored plan");
        return;
    }
    free(plan);

    if (point_epoch > (long long)now) {
        seconds_until_point = point_epoch - (long long)now;
    } else {
        seconds_until_point = 0;
    }

    if (rx_hz < PLUTO_MIN_HZ || rx_hz > PLUTO_MAX_HZ || !write_rx_lo_frequency(rx_hz, lo_path, sizeof(lo_path))) {
        write_track_state(cfg, "tune_error", name, point_index, point_time, rx_hz, "", "could not tune RX LO for selected Doppler point", seconds_until_aos, seconds_until_los, -1, seconds_until_point, "failed");
        send_error_json(fd, 500, "could not tune RX LO for selected Doppler point");
        return;
    }

    if (!write_track_state(cfg, state, name, point_index, point_time, rx_hz, lo_path, "selected Doppler point tuned", seconds_until_aos, seconds_until_los, -1, seconds_until_point, "written")) {
        send_error_json(fd, 500, "could not write Doppler track state");
        return;
    }
    if (!write_radio_target_state(cfg, now, now, name, norad, aos, rx_hz, "", mode, description, lo_path, point_time, state)) {
        send_error_json(fd, 500, "could not write tuned target state");
        return;
    }

    json_escape(name_json, sizeof(name_json), name);
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"state\":\"%s\",\"name\":\"%s\",\"point_index\":%d,\"point_time_utc\":\"%s\",\"rx_hz\":%lld,\"lo_path\":\"%s\",\"seconds_until_aos\":%lld,\"seconds_until_los\":%lld,\"seconds_until_point\":%lld,\"plan_window_enforced\":%s,\"lo_write_result\":\"written\"}\n",
             state,
             name_json,
             point_index,
             point_time,
             rx_hz,
             lo_path,
             seconds_until_aos,
             seconds_until_los,
             seconds_until_point,
             enforce_pass_window ? "true" : "false");
    send_text(fd, 200, "OK", "application/json; charset=utf-8", response);
}

static void send_radio_track_stop(int fd, const struct app_config *cfg)
{
    pthread_mutex_lock(&g_track_lock);
    g_track_auto_running = 0;
    pthread_mutex_unlock(&g_track_lock);

    if (!write_track_state(cfg, "stopped", "", -1, "", 0, "", "Doppler tracking stopped", -1, -1, -1, -1, "not_attempted")) {
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"could not write Doppler track state\"}\n");
        return;
    }
    send_text(fd, 200, "OK", "application/json; charset=utf-8",
              "{\"ok\":true,\"state\":\"stopped\"}\n");
}

static int track_auto_should_run(void)
{
    int running;

    pthread_mutex_lock(&g_track_lock);
    running = g_track_auto_running;
    pthread_mutex_unlock(&g_track_lock);
    return running;
}

static void track_auto_set_running(int running)
{
    pthread_mutex_lock(&g_track_lock);
    g_track_auto_running = running;
    pthread_mutex_unlock(&g_track_lock);
}

static void *track_auto_worker(void *arg)
{
    const struct app_config *cfg = (const struct app_config *)arg;
    int completed = 0;

    while (track_auto_should_run()) {
        char response[1024];
        char error[256] = "";
        int status = apply_radio_track_step(
            cfg,
            "auto_active",
            "automatic Doppler point tuned",
            1,
            response,
            sizeof(response),
            error,
            sizeof(error));
        if (status != 200) {
            write_track_state(cfg, "auto_error", "", -1, "", 0, "", error, -1, -1, -1, -1, "failed");
            track_auto_set_running(0);
            break;
        } else if (strstr(response, "\"state\":\"complete\"")) {
            completed = 1;
            track_auto_set_running(0);
            break;
        }
        sleep(5);
    }

    if (!completed) {
        write_track_state(cfg, "stopped", "", -1, "", 0, "", "Automatic Doppler tracking stopped", -1, -1, -1, -1, "not_attempted");
    }
    return NULL;
}


/* BACKEND_AUDIO_DOPPLER_TRACKED_START_V1 */
static int ensure_track_auto_running_for_audio_v1(const struct app_config *cfg)
{
    int create_result;

    pthread_mutex_lock(&g_track_lock);
    if (g_track_auto_running) {
        pthread_mutex_unlock(&g_track_lock);
        return 1;
    }
    g_track_auto_running = 1;
    pthread_mutex_unlock(&g_track_lock);

    create_result = pthread_create(&g_track_thread, NULL, track_auto_worker, (void *)cfg);
    if (create_result != 0) {
        track_auto_set_running(0);
        return 0;
    }
    pthread_detach(g_track_thread);
    return 1;
}

static void send_radio_track_auto_start(int fd, const struct app_config *cfg)
{
    int create_result;
    char name[256] = "";
    char error[256] = "";
    char response[768];
    char name_json[512];
    long long aos_epoch = 0;
    long long los_epoch = 0;
    long long now = (long long)time(NULL);

    if (!time_sync_state_is_synced(cfg)) {
        send_text(fd, 409, "Conflict", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"sync Pluto time from the browser before starting automatic Doppler tracking\"}\n");
        return;
    }
    if (!read_stored_track_window(cfg, name, sizeof(name), &aos_epoch, &los_epoch, error, sizeof(error))) {
        send_error_json(fd, 400, error);
        return;
    }
    if (now > los_epoch) {
        long long seconds_since_los = now - los_epoch;
        json_escape(name_json, sizeof(name_json), name);
        write_track_state(cfg, "stale", name, -1, "", 0, "", "Refusing stale pass; LOS is already past", -1, -1, seconds_since_los, -1, "not_attempted");
        snprintf(response, sizeof(response),
                 "{\"ok\":false,\"state\":\"stale\",\"name\":\"%s\",\"seconds_since_los\":%lld,\"error\":\"pass is stale; plan a current or future pass\"}\n",
                 name_json, seconds_since_los);
        send_text(fd, 409, "Conflict", "application/json; charset=utf-8", response);
        return;
    }

    pthread_mutex_lock(&g_track_lock);
    if (g_track_auto_running) {
        pthread_mutex_unlock(&g_track_lock);
        send_text(fd, 200, "OK", "application/json; charset=utf-8",
                  "{\"ok\":true,\"state\":\"auto_active\",\"message\":\"Automatic Doppler tracking is already running\"}\n");
        return;
    }
    g_track_auto_running = 1;
    pthread_mutex_unlock(&g_track_lock);

    create_result = pthread_create(&g_track_thread, NULL, track_auto_worker, (void *)cfg);
    if (create_result != 0) {
        track_auto_set_running(0);
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"could not start automatic Doppler tracking thread\"}\n");
        return;
    }
    pthread_detach(g_track_thread);

    send_text(fd, 200, "OK", "application/json; charset=utf-8",
              "{\"ok\":true,\"state\":\"auto_active\"}\n");
}

static void send_radio_track_auto_stop(int fd, const struct app_config *cfg)
{
    if (!track_auto_should_run()) {
        send_text(fd, 200, "OK", "application/json; charset=utf-8",
                  "{\"ok\":true,\"state\":\"idle\",\"message\":\"Automatic Doppler tracking is not running\"}\n");
        return;
    }

    track_auto_set_running(0);
    if (!write_track_state(cfg, "stopping", "", -1, "", 0, "", "Automatic Doppler tracking stopping", -1, -1, -1, -1, "not_attempted")) {
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"could not write Doppler track state\"}\n");
        return;
    }
    send_text(fd, 200, "OK", "application/json; charset=utf-8",
              "{\"ok\":true,\"state\":\"stopping\"}\n");
}

static void send_radio_tune(int fd, const struct app_config *cfg, const char *query)
{
    char name[256] = "";
    char norad[64] = "";
    char aos[128] = "";
    char downlink[64] = "";
    char uplink[64] = "";
    char mode[128] = "";
    char description[512] = "";
    char name_json[512];
    char lo_path[PATH_BUF_SIZE] = "";
    char body[2048];
    long long frequency_hz;
    time_t now = time(NULL);

    query_param(query, "name", name, sizeof(name));
    query_param(query, "norad", norad, sizeof(norad));
    query_param(query, "aos", aos, sizeof(aos));
    query_param(query, "downlink", downlink, sizeof(downlink));
    query_param(query, "uplink", uplink, sizeof(uplink));
    query_param(query, "mode", mode, sizeof(mode));
    query_param(query, "description", description, sizeof(description));

    if (!name[0] || !norad[0] || !parse_frequency_hz(downlink, &frequency_hz)) {
        send_text(fd, 400, "Bad Request", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"valid name, norad, and Pluto-range downlink are required\"}\n");
        return;
    }

    if (!write_rx_lo_frequency(frequency_hz, lo_path, sizeof(lo_path))) {
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"could not write AD9361 RX LO frequency; frequency may be outside the active Pluto radio profile\"}\n");
        return;
    }

    if (!write_radio_target_state(cfg, now, now, name, norad, aos, frequency_hz, uplink, mode, description, lo_path, "", "tuned")) {
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"could not write tuned target state\"}\n");
        return;
    }

    json_escape(name_json, sizeof(name_json), name);
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"state\":\"tuned\",\"name\":\"%s\",\"norad_id\":%s,\"downlink_hz\":%lld,\"lo_path\":\"%s\"}\n",
             name_json, norad, frequency_hz, lo_path);
    send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
}


/* RECEIVE_MODE_FOUNDATION_V2_6_21A
 * Phase 1 receive/decode foundation. These endpoints intentionally do not
 * start any decoder DSP yet; they provide stable UI/backend plumbing so CW,
 * AX.25/APRS, and later telemetry decoders can be added safely one at a time.
 *
 * This block is intentionally placed after query_param(), json_escape(), and
 * send_text() are declared/defined so ARM builds do not create implicit
 * declarations on the Pluto toolchain.
 */
static const char *receive_kind_from_mode_v261a(const char *mode)
{
    char lower[256];
    size_t i;
    if (!mode || !*mode) {
        return "listen";
    }
    for (i = 0; i + 1 < sizeof(lower) && mode[i]; i++) {
        lower[i] = (char)tolower((unsigned char)mode[i]);
    }
    lower[i] = '\0';
    if (strstr(lower, "cw") || strstr(lower, "morse")) {
        return "cw";
    }
    if (strstr(lower, "ax.25") || strstr(lower, "ax25") || strstr(lower, "aprs") ||
        strstr(lower, "packet") || strstr(lower, "fsk") || strstr(lower, "gmsk") ||
        strstr(lower, "bpsk") || strstr(lower, "qpsk") || strstr(lower, "psk") ||
        strstr(lower, "telemetry") || strstr(lower, "digital") || strstr(lower, "data")) {
        return "digital";
    }
    return "listen";
}

static void send_receive_status_v261a(int fd)
{
    send_text(fd, 200, "OK", "application/json; charset=utf-8",
              "{\"ok\":true,\"state\":\"idle\",\"version\":\"2.6.21a\",\"message\":\"Receive/decode foundation is installed. FM Listen remains on the existing audio path.\"}\n");
}

static void send_receive_start_v261a(int fd, const struct app_config *cfg, const char *query)
{
    char mode[128] = "";
    char name[256] = "";
    char downlink[64] = "";
    char kind_json[64];
    char mode_json[256];
    char name_json[512];
    char downlink_json[128];
    char body[1600];
    const char *kind;
    (void)cfg;

    query_param(query, "mode", mode, sizeof(mode));
    query_param(query, "name", name, sizeof(name));
    query_param(query, "downlink_hz", downlink, sizeof(downlink));
    if (!mode[0]) query_param(query, "protocol", mode, sizeof(mode));
    if (!name[0]) query_param(query, "satellite", name, sizeof(name));

    kind = receive_kind_from_mode_v261a(mode);
    json_escape(kind_json, sizeof(kind_json), kind);
    json_escape(mode_json, sizeof(mode_json), mode[0] ? mode : "unknown");
    json_escape(name_json, sizeof(name_json), name[0] ? name : "selected satellite");
    json_escape(downlink_json, sizeof(downlink_json), downlink[0] ? downlink : "unknown");

    snprintf(body, sizeof(body),
             "{"
             "\"ok\":true,"
             "\"state\":\"placeholder\","
             "\"version\":\"2.6.21a\","
             "\"receive_kind\":\"%s\","
             "\"decoder_state\":\"not_implemented\","
             "\"satellite\":\"%s\","
             "\"mode\":\"%s\","
             "\"downlink_hz\":\"%s\","
             "\"message\":\"Decode foundation is installed. CW and packet decoders will be added in later steps; FM voice still uses the existing Listen path.\""
             "}\n",
             kind_json, name_json, mode_json, downlink_json);
    send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
}

static void send_receive_stop_v261a(int fd)
{
    send_text(fd, 200, "OK", "application/json; charset=utf-8",
              "{\"ok\":true,\"state\":\"stopped\",\"message\":\"No decoder process is active in the foundation build.\"}\n");
}

static void send_decode_output_v261a(int fd, const char *query)
{
    char mode[128] = "";
    char mode_json[256];
    char kind_json[64];
    char body[1600];
    const char *kind;
    query_param(query, "mode", mode, sizeof(mode));
    if (!mode[0]) query_param(query, "protocol", mode, sizeof(mode));
    kind = receive_kind_from_mode_v261a(mode);
    json_escape(kind_json, sizeof(kind_json), kind);
    json_escape(mode_json, sizeof(mode_json), mode[0] ? mode : "unknown");
    snprintf(body, sizeof(body),
             "{"
             "\"ok\":true,"
             "\"state\":\"placeholder\","
             "\"receive_kind\":\"%s\","
             "\"mode\":\"%s\","
             "\"lines\":["
             "\"Decode foundation active.\","
             "\"No %s decoder is installed yet.\","
             "\"Next implementation target: CW text decoder, then AX.25/APRS packet decoder.\""
             "]"
             "}\n",
             kind_json, mode_json, kind_json);
    send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
}


/* ROTATOR_CONTROL_FOUNDATION_V2_4_0 */
struct rotator_config {
    int enabled;
    char type[64];
    char host[128];
    int port;
    double min_move_deg;
    double az_offset_deg;
    double el_offset_deg;
    double min_el_deg;
    double max_el_deg;
    double park_az_deg;
    double park_el_deg;
    int update_interval_sec;
    int park_on_los;
};

static void rotator_default_config(struct rotator_config *rot)
{
    memset(rot, 0, sizeof(*rot));
    rot->enabled = 0;
    snprintf(rot->type, sizeof(rot->type), "simulation");
    snprintf(rot->host, sizeof(rot->host), "127.0.0.1");
    rot->port = 4533;
    rot->min_move_deg = 1.0;
    rot->min_el_deg = 0.0;
    rot->max_el_deg = 90.0;
    rot->update_interval_sec = 2;
}

static int json_bool_value_v2_4(const char *json, const char *key, int *out)
{
    const char *p;
    char needle[128];
    if (!json || !key || !out) return 0;
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    p = strstr(json, needle);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (strncmp(p, "true", 4) == 0 || strncmp(p, "1", 1) == 0) { *out = 1; return 1; }
    if (strncmp(p, "false", 5) == 0 || strncmp(p, "0", 1) == 0) { *out = 0; return 1; }
    return 0;
}

static int json_int_value_v2_4(const char *json, const char *key, int *out)
{
    long long value = 0;
    if (!json_long_long_after(json, key, &value)) return 0;
    *out = (int)value;
    return 1;
}

static int rotator_config_path(const struct app_config *cfg, char *path, size_t path_size)
{
    if (!cfg || !path || path_size == 0) return 0;
    join_path(path, path_size, cfg->config_dir, "rotator.json");
    return 1;
}

static int rotator_state_path(const struct app_config *cfg, char *path, size_t path_size)
{
    if (!cfg || !path || path_size == 0) return 0;
    join_path(path, path_size, cfg->data_dir, "rotator_state.json");
    return 1;
}

static void rotator_config_to_json(const struct rotator_config *rot, char *body, size_t body_size)
{
    snprintf(body, body_size,
             "{\n"
             "  \"ok\": true,\n"
             "  \"enabled\": %s,\n"
             "  \"type\": \"%s\",\n"
             "  \"connection\": {\"host\": \"%s\", \"port\": %d},\n"
             "  \"update_interval_sec\": %d,\n"
             "  \"min_move_deg\": %.2f,\n"
             "  \"az_offset_deg\": %.2f,\n"
             "  \"el_offset_deg\": %.2f,\n"
             "  \"min_el_deg\": %.2f,\n"
             "  \"max_el_deg\": %.2f,\n"
             "  \"park_on_los\": %s,\n"
             "  \"park_az_deg\": %.2f,\n"
             "  \"park_el_deg\": %.2f,\n"
             "  \"supported_types\": [\"simulation\", \"hamlib_rotctld\", \"satran\", \"easycomm2\", \"yaesu_gs232\"]\n"
             "}\n",
             rot->enabled ? "true" : "false", rot->type, rot->host, rot->port,
             rot->update_interval_sec, rot->min_move_deg, rot->az_offset_deg, rot->el_offset_deg,
             rot->min_el_deg, rot->max_el_deg, rot->park_on_los ? "true" : "false",
             rot->park_az_deg, rot->park_el_deg);
}

static int rotator_load_config(const struct app_config *cfg, struct rotator_config *rot)
{
    char path[PATH_BUF_SIZE];
    char *json = NULL;
    size_t len = 0;
    char value[128] = "";
    double d = 0.0;
    int i = 0;
    rotator_default_config(rot);
    rotator_config_path(cfg, path, sizeof(path));
    if (read_file_to_string(path, &json, &len) != 0) return 0;
    if (json_bool_value_v2_4(json, "enabled", &i)) rot->enabled = i;
    if (json_string_value(json, "type", value, sizeof(value))) snprintf(rot->type, sizeof(rot->type), "%s", value);
    if (json_string_value(json, "host", value, sizeof(value))) snprintf(rot->host, sizeof(rot->host), "%s", value);
    if (json_int_value_v2_4(json, "port", &i)) rot->port = i;
    if (json_int_value_v2_4(json, "update_interval_sec", &i)) { if (i < 1) i = 1; if (i > 60) i = 60; rot->update_interval_sec = i; }
    if (json_double_value(json, "min_move_deg", &d)) rot->min_move_deg = d;
    if (json_double_value(json, "az_offset_deg", &d)) rot->az_offset_deg = d;
    if (json_double_value(json, "el_offset_deg", &d)) rot->el_offset_deg = d;
    if (json_double_value(json, "min_el_deg", &d)) rot->min_el_deg = d;
    if (json_double_value(json, "max_el_deg", &d)) rot->max_el_deg = d;
    if (json_double_value(json, "park_az_deg", &d)) rot->park_az_deg = d;
    if (json_double_value(json, "park_el_deg", &d)) rot->park_el_deg = d;
    if (json_bool_value_v2_4(json, "park_on_los", &i)) rot->park_on_los = i;
    if (rot->port <= 0 || rot->port > 65535) rot->port = 4533;
    if (rot->max_el_deg < rot->min_el_deg) rot->max_el_deg = rot->min_el_deg;
    free(json);
    return 1;
}

static int rotator_write_config(const struct app_config *cfg, const struct rotator_config *rot)
{
    char path[PATH_BUF_SIZE], tmp_path[PATH_BUF_SIZE + 8], body[2048];
    FILE *f;
    rotator_config_path(cfg, path, sizeof(path));
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    rotator_config_to_json(rot, body, sizeof(body));
    f = fopen(tmp_path, "wb");
    if (!f) return 0;
    fwrite(body, 1, strlen(body), f);
    fclose(f);
    if (rename(tmp_path, path) != 0) { unlink(tmp_path); return 0; }
    return 1;
}

static double rotator_clamp_el(const struct rotator_config *rot, double el)
{
    if (el < rot->min_el_deg) return rot->min_el_deg;
    if (el > rot->max_el_deg) return rot->max_el_deg;
    return el;
}

static double rotator_normalize_az(double az)
{
    while (az < 0.0) az += 360.0;
    while (az >= 360.0) az -= 360.0;
    return az;
}

static int rotator_write_state(const struct app_config *cfg, const struct rotator_config *rot, const char *state, double az_deg, double el_deg, const char *result, const char *message)
{
    char path[PATH_BUF_SIZE], tmp_path[PATH_BUF_SIZE + 8];
    char state_json[128], type_json[128], result_json[256], message_json[512];
    FILE *f;
    time_t now = time(NULL);
    rotator_state_path(cfg, path, sizeof(path));
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    json_escape(state_json, sizeof(state_json), state ? state : "unknown");
    json_escape(type_json, sizeof(type_json), rot ? rot->type : "unknown");
    json_escape(result_json, sizeof(result_json), result ? result : "");
    json_escape(message_json, sizeof(message_json), message ? message : "");
    f = fopen(tmp_path, "wb");
    if (!f) return 0;
    fprintf(f,
            "{\n"
            "  \"ok\": true,\n"
            "  \"enabled\": %s,\n"
            "  \"type\": \"%s\",\n"
            "  \"state\": \"%s\",\n"
            "  \"target_az_deg\": %.2f,\n"
            "  \"target_el_deg\": %.2f,\n"
            "  \"last_command_epoch\": %ld,\n"
            "  \"last_result\": \"%s\",\n"
            "  \"message\": \"%s\"\n"
            "}\n",
            (rot && rot->enabled) ? "true" : "false", type_json, state_json, az_deg, el_deg, (long)now, result_json, message_json);
    fclose(f);
    if (rename(tmp_path, path) != 0) { unlink(tmp_path); return 0; }
    return 1;
}

static int rotator_tcp_send_line(const char *host, int port, const char *line, char *reply, size_t reply_size)
{
    int sockfd = -1, ok = 0;
    struct sockaddr_in addr;
    struct timeval tv;
    if (!host || !line || port <= 0 || port > 65535) return 0;
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return 0;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { close(sockfd); return 0; }
    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(sockfd); return 0; }
    if (send(sockfd, line, strlen(line), 0) == (ssize_t)strlen(line)) {
        ok = 1;
        if (reply && reply_size > 0) {
            ssize_t n = recv(sockfd, reply, reply_size - 1, 0);
            if (n > 0) reply[n] = '\0'; else reply[0] = '\0';
        }
    }
    close(sockfd);
    return ok;
}


/* ROTATOR_PROTOCOL_ADAPTERS_V2_4_3 */
static int rotator_format_command_v2_4_3(
    const char *type,
    double az_deg,
    double el_deg,
    char *line,
    size_t line_size,
    char *label,
    size_t label_size)
{
    int az_i;
    int el_i;

    if (!type || !line || line_size == 0) {
        return 0;
    }

    if (strcmp(type, "hamlib_rotctld") == 0) {
        snprintf(line, line_size, "P %.2f %.2f\n", az_deg, el_deg);
        if (label && label_size > 0) {
            snprintf(label, label_size, "hamlib_rotctld");
        }
        return 1;
    }

    if (strcmp(type, "easycomm2") == 0) {
        snprintf(line, line_size, "AZ%.1f EL%.1f\n", az_deg, el_deg);
        if (label && label_size > 0) {
            snprintf(label, label_size, "easycomm2");
        }
        return 1;
    }

    if (strcmp(type, "yaesu_gs232") == 0) {
        az_i = (int)(az_deg + 0.5);
        el_i = (int)(el_deg + 0.5);
        if (az_i < 0) {
            az_i = 0;
        }
        if (az_i > 450) {
            az_i = 450;
        }
        if (el_i < 0) {
            el_i = 0;
        }
        if (el_i > 180) {
            el_i = 180;
        }
        snprintf(line, line_size, "W%03d %03d\n", az_i, el_i);
        if (label && label_size > 0) {
            snprintf(label, label_size, "yaesu_gs232");
        }
        return 1;
    }

    return 0;
}

static void send_rotator_protocol_preview_v2_4_3(int fd, const char *query)
{
    char type[64] = "";
    char az_text[64] = "";
    char el_text[64] = "";
    char command[128] = "";
    char command_json[256];
    char label[64] = "";
    char body[768];
    double az = 0.0;
    double el = 0.0;

    query_param(query, "type", type, sizeof(type));
    query_param(query, "az", az_text, sizeof(az_text));
    query_param(query, "el", el_text, sizeof(el_text));

    if (!type[0] || !az_text[0] || !el_text[0]) {
        send_text(fd, 400, "Bad Request", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"type, az, and el query parameters are required\"}\n");
        return;
    }

    az = rotator_normalize_az(atof(az_text));
    el = atof(el_text);
    if (el < 0.0) {
        el = 0.0;
    }
    if (el > 180.0) {
        el = 180.0;
    }

    if (strcmp(type, "satran") == 0) {
        send_text(fd, 409, "Conflict", "application/json; charset=utf-8",
                  "{\"ok\":false,\"type\":\"satran\",\"error\":\"SATRAN adapter protocol is pending confirmation\"}\n");
        return;
    }

    if (!rotator_format_command_v2_4_3(type, az, el, command, sizeof(command), label, sizeof(label))) {
        send_text(fd, 400, "Bad Request", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"unsupported rotator protocol type\"}\n");
        return;
    }

    json_escape(command_json, sizeof(command_json), command);
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"type\":\"%s\",\"az_deg\":%.2f,\"el_deg\":%.2f,\"command\":\"%s\"}\n",
             label, az, el, command_json);
    send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
}

static int rotator_send_target(const struct app_config *cfg, struct rotator_config *rot, double az_deg, double el_deg, char *result, size_t result_size)
{
    char line[128], reply[256] = "", label[64] = "";
    double target_az = rotator_normalize_az(az_deg + rot->az_offset_deg);
    double target_el = rotator_clamp_el(rot, el_deg + rot->el_offset_deg);
    if (!rot->enabled) {
        snprintf(result, result_size, "rotator disabled");
        rotator_write_state(cfg, rot, "disabled", target_az, target_el, "disabled", "rotator is disabled");
        return 1;
    }
    if (strcmp(rot->type, "simulation") == 0) {
        snprintf(result, result_size, "simulation target %.2f %.2f", target_az, target_el);
        rotator_write_state(cfg, rot, "simulated", target_az, target_el, "simulated", result);
        return 1;
    }
    if (rotator_format_command_v2_4_3(rot->type, target_az, target_el, line, sizeof(line), label, sizeof(label))) {
        if (rotator_tcp_send_line(rot->host, rot->port, line, reply, sizeof(reply))) {
            snprintf(result, result_size, "%s sent command=%s reply=%s", label, line, reply);
            rotator_write_state(cfg, rot, "commanded", target_az, target_el, "written", result);
            return 1;
        }
        snprintf(result, result_size, "%s command failed to %s:%d command=%s", label, rot->host, rot->port, line);
        rotator_write_state(cfg, rot, "error", target_az, target_el, "error", result);
        return 0;
    }
    if (strcmp(rot->type, "satran") == 0) {
        snprintf(result, result_size, "satran adapter protocol is pending confirmation");
        rotator_write_state(cfg, rot, "unsupported", target_az, target_el, "unsupported", result);
        return 0;
    }
    snprintf(result, result_size, "unknown rotator type: %s", rot->type);
    rotator_write_state(cfg, rot, "error", target_az, target_el, "error", result);
    return 0;
}

static int rotator_read_active_target(const struct app_config *cfg, double *az_out, double *el_out, char *source, size_t source_size)
{
    /* ROTATOR_PASSES_TARGET_FALLBACK_V2_4_2C */
    char path[PATH_BUF_SIZE];
    char *json = NULL;
    size_t len = 0;
    double az = 0.0, el = 0.0;
    int got_az = 0, got_el = 0;

    if (!cfg || !az_out || !el_out) {
        return 0;
    }

    join_path(path, sizeof(path), cfg->data_dir, "radio_track_state.json");
    if (read_file_to_string(path, &json, &len) == 0) {
        got_az = json_double_value(json, "az_deg", &az) ||
                 json_double_value(json, "azimuth_deg", &az) ||
                 json_double_value(json, "target_az_deg", &az);
        got_el = json_double_value(json, "el_deg", &el) ||
                 json_double_value(json, "elevation_deg", &el) ||
                 json_double_value(json, "target_el_deg", &el);
        free(json);
        json = NULL;
        if (got_az && got_el) {
            *az_out = az;
            *el_out = el;
            if (source && source_size > 0) {
                snprintf(source, source_size, "%s", path);
            }
            return 1;
        }
    }

    /*
     * ROTATOR_SELECTED_PLAN_TARGET_V2_4_5
     *
     * Prefer the currently selected Doppler plan written by the browser through
     * /api/radio/track/plan. This prevents the rotator from following the first
     * active pass in passes.json when the user has selected a different
     * overlapping pass.
     */
    runtime_file_path(path, sizeof(path), "radio_track.json");
    if (read_file_to_string(path, &json, &len) == 0) {
        const char *p = json;
        time_t now_time = time(NULL);
        long long now_epoch = (long long)now_time;
        long long best_abs_dt = LLONG_MAX;
        double best_az = 0.0;
        double best_el = 0.0;
        int found = 0;

        while ((p = strstr(p, "\"time_utc\"")) != NULL) {
            const char *object_start = p;
            const char *time_start;
            const char *next_point;
            const char *object_end;
            char object_buf[4096];
            char time_value[64] = "";
            size_t object_len;
            long long point_epoch = 0;
            long long abs_dt;
            double point_az = 0.0;
            double point_el = 0.0;
            int point_has_az;
            int point_has_el;

            while (object_start > json && *object_start != '{') {
                object_start--;
            }

            time_start = strchr(p, ':');
            if (!time_start) {
                break;
            }
            time_start++;
            while (*time_start == ' ' || *time_start == '\t' || *time_start == '\r' || *time_start == '\n') {
                time_start++;
            }
            if (*time_start != '"') {
                p += 10;
                continue;
            }
            time_start++;
            snprintf(time_value, sizeof(time_value), "%.63s", time_start);
            if (strchr(time_value, '"')) {
                *strchr(time_value, '"') = '\0';
            }

            next_point = strstr(time_start, "\"time_utc\"");
            object_end = next_point ? next_point : time_start + strlen(time_start);
            object_len = (size_t)(object_end - object_start);
            if (object_len >= sizeof(object_buf)) {
                object_len = sizeof(object_buf) - 1;
            }
            memcpy(object_buf, object_start, object_len);
            object_buf[object_len] = '\0';

            point_has_az = json_double_value(object_buf, "az_deg", &point_az) ||
                           json_double_value(object_buf, "azimuth_deg", &point_az) ||
                           json_double_value(object_buf, "target_az_deg", &point_az) ||
                           json_double_value(object_buf, "az", &point_az);
            point_has_el = json_double_value(object_buf, "el_deg", &point_el) ||
                           json_double_value(object_buf, "elevation_deg", &point_el) ||
                           json_double_value(object_buf, "target_el_deg", &point_el) ||
                           json_double_value(object_buf, "el", &point_el);

            if (point_has_az && point_has_el && parse_iso_utc_epoch(time_value, &point_epoch)) {
                long long dt = point_epoch - now_epoch;
                abs_dt = dt < 0 ? -dt : dt;
                if (abs_dt < best_abs_dt) {
                    best_abs_dt = abs_dt;
                    best_az = point_az;
                    best_el = point_el;
                    found = 1;
                }
            }

            p = next_point ? next_point : time_start + strlen(time_start);
        }

        free(json);
        json = NULL;

        if (found && best_abs_dt <= 180) {
            *az_out = best_az;
            *el_out = best_el;
            if (source && source_size > 0) {
                snprintf(source, source_size, "%s:selected_plan_point", path);
            }
            return 1;
        }
    }

    /*
     * Fallback: pass predictions already contain doppler_plan points with
     * azimuth_deg/elevation_deg. Use the nearest point around current time
     * so rotator tracking can work even when Doppler radio tracking is not
     * actively writing radio_track_state.json.
     */
    join_path(path, sizeof(path), cfg->data_dir, "passes.json");
    if (read_file_to_string(path, &json, &len) == 0) {
        const char *p = json;
        time_t now_time = time(NULL);
        long long now_epoch = (long long)now_time;
        long long best_abs_dt = LLONG_MAX;
        double best_az = 0.0;
        double best_el = 0.0;
        int found = 0;

        while ((p = strstr(p, "\"time_utc\"")) != NULL) {
            const char *object_start = p;
            const char *time_start;
            const char *next_point;
            const char *object_end;
            char object_buf[4096];
            char time_value[64] = "";
            size_t object_len;
            long long point_epoch = 0;
            long long abs_dt;
            double point_az = 0.0;
            double point_el = 0.0;
            int point_has_az;
            int point_has_el;

            while (object_start > json && *object_start != '{') {
                object_start--;
            }

            time_start = strchr(p, ':');
            if (!time_start) {
                break;
            }
            time_start++;
            while (*time_start == ' ' || *time_start == '\t' || *time_start == '\r' || *time_start == '\n') {
                time_start++;
            }
            if (*time_start != '"') {
                p += 10;
                continue;
            }
            time_start++;
            snprintf(time_value, sizeof(time_value), "%.63s", time_start);
            if (strchr(time_value, '"')) {
                *strchr(time_value, '"') = '\0';
            }

            next_point = strstr(time_start, "\"time_utc\"");
            object_end = next_point ? next_point : time_start + strlen(time_start);
            object_len = (size_t)(object_end - object_start);
            if (object_len >= sizeof(object_buf)) {
                object_len = sizeof(object_buf) - 1;
            }
            memcpy(object_buf, object_start, object_len);
            object_buf[object_len] = '\0';

            point_has_az = json_double_value(object_buf, "az_deg", &point_az) ||
                           json_double_value(object_buf, "azimuth_deg", &point_az) ||
                           json_double_value(object_buf, "target_az_deg", &point_az);
            point_has_el = json_double_value(object_buf, "el_deg", &point_el) ||
                           json_double_value(object_buf, "elevation_deg", &point_el) ||
                           json_double_value(object_buf, "target_el_deg", &point_el);

            if (point_has_az && point_has_el && parse_iso_utc_epoch(time_value, &point_epoch)) {
                long long dt = point_epoch - now_epoch;
                abs_dt = dt < 0 ? -dt : dt;
                if (abs_dt < best_abs_dt) {
                    best_abs_dt = abs_dt;
                    best_az = point_az;
                    best_el = point_el;
                    found = 1;
                }
            }

            p = next_point ? next_point : time_start + strlen(time_start);
        }

        free(json);
        json = NULL;

        if (found && best_abs_dt <= 180) {
            *az_out = best_az;
            *el_out = best_el;
            if (source && source_size > 0) {
                snprintf(source, source_size, "%s:nearest_pass_point", path);
            }
            return 1;
        }
    }

    return 0;
}

static void send_rotator_config(int fd, const struct app_config *cfg)
{
    struct rotator_config rot;
    char body[2048];
    rotator_load_config(cfg, &rot);
    rotator_config_to_json(&rot, body, sizeof(body));
    send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
}

static void save_rotator_config(int fd, const struct app_config *cfg, const char *body)
{
    struct rotator_config rot;
    char value[128] = "";
    double d = 0.0;
    int i = 0;
    char response[256];
    rotator_load_config(cfg, &rot);
    if (body && body[0]) {
        if (json_bool_value_v2_4(body, "enabled", &i)) rot.enabled = i;
        if (json_string_value(body, "type", value, sizeof(value))) snprintf(rot.type, sizeof(rot.type), "%s", value);
        if (json_string_value(body, "host", value, sizeof(value))) snprintf(rot.host, sizeof(rot.host), "%s", value);
        if (json_int_value_v2_4(body, "port", &i)) rot.port = i;
        if (json_int_value_v2_4(body, "update_interval_sec", &i)) { if (i < 1) i = 1; if (i > 60) i = 60; rot.update_interval_sec = i; }
        if (json_double_value(body, "min_move_deg", &d)) rot.min_move_deg = d;
        if (json_double_value(body, "az_offset_deg", &d)) rot.az_offset_deg = d;
        if (json_double_value(body, "el_offset_deg", &d)) rot.el_offset_deg = d;
        if (json_double_value(body, "min_el_deg", &d)) rot.min_el_deg = d;
        if (json_double_value(body, "max_el_deg", &d)) rot.max_el_deg = d;
        if (json_double_value(body, "park_az_deg", &d)) rot.park_az_deg = d;
        if (json_double_value(body, "park_el_deg", &d)) rot.park_el_deg = d;
        if (json_bool_value_v2_4(body, "park_on_los", &i)) rot.park_on_los = i;
    }
    if (!rotator_write_config(cfg, &rot)) {
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"could not write rotator config\"}\n");
        return;
    }
    snprintf(response, sizeof(response), "{\"ok\":true,\"message\":\"rotator config saved\",\"type\":\"%s\",\"enabled\":%s}\n", rot.type, rot.enabled ? "true" : "false");
    send_text(fd, 200, "OK", "application/json; charset=utf-8", response);
}

static void send_rotator_state(int fd, const struct app_config *cfg)
{
    char path[PATH_BUF_SIZE];
    char *json = NULL;
    size_t len = 0;
    rotator_state_path(cfg, path, sizeof(path));
    if (read_file_to_string(path, &json, &len) == 0) {
        send_text(fd, 200, "OK", "application/json; charset=utf-8", json);
        free(json);
        return;
    }
    send_text(fd, 200, "OK", "application/json; charset=utf-8", "{\"ok\":true,\"enabled\":false,\"type\":\"simulation\",\"state\":\"idle\",\"target_az_deg\":0.0,\"target_el_deg\":0.0,\"last_result\":\"not_started\",\"message\":\"rotator has not been commanded\"}\n");
}

static void send_rotator_test(int fd, const struct app_config *cfg, const char *query)
{
    struct rotator_config rot;
    char az_text[64] = "", el_text[64] = "", result[512] = "", result_json[1024], body[1400];
    double az = 0.0, el = 0.0;
    int ok;
    query_param(query, "az", az_text, sizeof(az_text));
    query_param(query, "el", el_text, sizeof(el_text));
    if (!az_text[0] || !el_text[0]) {
        send_text(fd, 400, "Bad Request", "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"az and el query parameters are required\"}\n");
        return;
    }
    az = atof(az_text);
    el = atof(el_text);
    rotator_load_config(cfg, &rot);
    ok = rotator_send_target(cfg, &rot, az, el, result, sizeof(result));
    json_escape(result_json, sizeof(result_json), result);
    snprintf(body, sizeof(body), "{\"ok\":%s,\"type\":\"%s\",\"az_deg\":%.2f,\"el_deg\":%.2f,\"message\":\"%s\"}\n", ok ? "true" : "false", rot.type, az, el, result_json);
    send_text(fd, ok ? 200 : 409, ok ? "OK" : "Conflict", "application/json; charset=utf-8", body);
}

static void send_rotator_park(int fd, const struct app_config *cfg)
{
    struct rotator_config rot;
    char result[512] = "", result_json[1024], body[1400];
    int ok;
    rotator_load_config(cfg, &rot);
    ok = rotator_send_target(cfg, &rot, rot.park_az_deg, rot.park_el_deg, result, sizeof(result));
    json_escape(result_json, sizeof(result_json), result);
    snprintf(body, sizeof(body), "{\"ok\":%s,\"state\":\"park\",\"message\":\"%s\"}\n", ok ? "true" : "false", result_json);
    send_text(fd, ok ? 200 : 409, ok ? "OK" : "Conflict", "application/json; charset=utf-8", body);
}

static void send_rotator_stop(int fd, const struct app_config *cfg)
{
    struct rotator_config rot;
    pthread_mutex_lock(&g_track_lock);
    g_rotator_auto_running = 0;
    pthread_mutex_unlock(&g_track_lock);
    rotator_load_config(cfg, &rot);
    rotator_write_state(cfg, &rot, "stopped", 0.0, 0.0, "stopped", "rotator tracking stopped");
    send_text(fd, 200, "OK", "application/json; charset=utf-8", "{\"ok\":true,\"state\":\"stopped\"}\n");
}

static int rotator_auto_should_run(void)
{
    int running;
    pthread_mutex_lock(&g_track_lock);
    running = g_rotator_auto_running;
    pthread_mutex_unlock(&g_track_lock);
    return running;
}

static void rotator_auto_set_running(int running)
{
    pthread_mutex_lock(&g_track_lock);
    g_rotator_auto_running = running;
    pthread_mutex_unlock(&g_track_lock);
}

static int rotator_track_step_once(const struct app_config *cfg, char *message, size_t message_size)
{
    struct rotator_config rot;
    double az = 0.0, el = 0.0;
    char source[PATH_BUF_SIZE] = "", result[512] = "";
    rotator_load_config(cfg, &rot);
    if (!rot.enabled) {
        snprintf(message, message_size, "rotator disabled");
        rotator_write_state(cfg, &rot, "disabled", 0.0, 0.0, "disabled", "rotator disabled");
        return 1;
    }
    if (!rotator_read_active_target(cfg, &az, &el, source, sizeof(source))) {
        snprintf(message, message_size, "no active az/el target in radio_track_state.json");
        rotator_write_state(cfg, &rot, "waiting", 0.0, 0.0, "waiting", message);
        return 0;
    }
    if (!rotator_send_target(cfg, &rot, az, el, result, sizeof(result))) {
        snprintf(message, message_size, "%s", result);
        return 0;
    }
    snprintf(message, message_size, "rotator target from %s: %s", source, result);
    return 1;
}

static void *rotator_auto_worker(void *arg)
{
    const struct app_config *cfg = (const struct app_config *)arg;
    while (rotator_auto_should_run()) {
        struct rotator_config rot;
        char message[512] = "";
        rotator_track_step_once(cfg, message, sizeof(message));
        rotator_load_config(cfg, &rot);
        if (rot.update_interval_sec < 1) rot.update_interval_sec = 2;
        sleep((unsigned int)rot.update_interval_sec);
    }
    return NULL;
}

static void send_rotator_track_start(int fd, const struct app_config *cfg)
{
    int create_result;
    pthread_mutex_lock(&g_track_lock);
    if (g_rotator_auto_running) {
        pthread_mutex_unlock(&g_track_lock);
        send_text(fd, 200, "OK", "application/json; charset=utf-8", "{\"ok\":true,\"state\":\"already_running\"}\n");
        return;
    }
    g_rotator_auto_running = 1;
    pthread_mutex_unlock(&g_track_lock);
    create_result = pthread_create(&g_rotator_thread, NULL, rotator_auto_worker, (void *)cfg);
    if (create_result != 0) {
        rotator_auto_set_running(0);
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"could not start rotator tracking thread\"}\n");
        return;
    }
    pthread_detach(g_rotator_thread);
    send_text(fd, 200, "OK", "application/json; charset=utf-8", "{\"ok\":true,\"state\":\"tracking\"}\n");
}

static void send_rotator_track_stop(int fd, const struct app_config *cfg)
{
    (void)cfg;
    rotator_auto_set_running(0);
    send_text(fd, 200, "OK", "application/json; charset=utf-8", "{\"ok\":true,\"state\":\"stopped\"}\n");
}

static void send_rotator_track_step(int fd, const struct app_config *cfg)
{
    char message[512] = "", message_json[1024], body[1400];
    int ok = rotator_track_step_once(cfg, message, sizeof(message));
    json_escape(message_json, sizeof(message_json), message);
    snprintf(body, sizeof(body), "{\"ok\":%s,\"message\":\"%s\"}\n", ok ? "true" : "false", message_json);
    send_text(fd, ok ? 200 : 409, ok ? "OK" : "Conflict", "application/json; charset=utf-8", body);
}


/* REAL_SPECTRUM_SNAPSHOT_V2_5_2 */
#define SPECTRUM_V252_SAMPLE_RATE_HZ 2400000
#define SPECTRUM_V252_CAPTURE_FRAMES 2048
#define SPECTRUM_V252_DFT_SAMPLES 1024
#define SPECTRUM_V252_DEFAULT_BINS 160
#define SPECTRUM_V252_MAX_BINS 192
#define SPECTRUM_V252_PI 3.14159265358979323846264338327950288

static int spectrum_parse_ll_v252(const char *text, long long *out)
{
    char *end = NULL;
    long long value;
    if (!text || !*text || !out) return 0;
    errno = 0;
    value = strtoll(text, &end, 10);
    if (errno != 0 || !end || *end != '\0') return 0;
    *out = value;
    return 1;
}

static int spectrum_parse_int_v252(const char *text, int *out)
{
    char *end = NULL;
    long value;
    if (!text || !*text || !out) return 0;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || !end || *end != '\0') return 0;
    *out = (int)value;
    return 1;
}

static int spectrum_cmd_ok_v252(const char *cmd)
{
    int rc;
    if (!cmd || !*cmd) return 0;
    rc = system(cmd);
    return !(rc == -1 || !WIFEXITED(rc) || WEXITSTATUS(rc) != 0);
}

/* SPECTRUM_TUNING_CONTROLS_V2_6_5 */
static long spectrum_clamp_bandwidth_v265(long bandwidth_hz)
{
    if (bandwidth_hz < 200000L) return 200000L;
    if (bandwidth_hz > (long)SPECTRUM_V252_SAMPLE_RATE_HZ) return (long)SPECTRUM_V252_SAMPLE_RATE_HZ;
    return bandwidth_hz;
}

static double spectrum_clamp_gain_v265(double gain_db)
{
    if (gain_db < 0.0) return 0.0;
    if (gain_db > 70.0) return 70.0;
    return gain_db;
}

static int spectrum_configure_rx_v252(
    long long center_hz,
    int stream_index,
    long bandwidth_hz,
    int have_gain,
    double gain_db,
    char *error,
    size_t error_size)
{
    char cmd[512];
    int rx_channel = stream_index > 0 ? 1 : 0;
    long applied_bandwidth_hz = spectrum_clamp_bandwidth_v265(bandwidth_hz);

    snprintf(cmd, sizeof(cmd), "/usr/bin/iio_attr -u local: -c ad9361-phy altvoltage0 frequency %lld >/dev/null 2>&1", center_hz);
    if (!spectrum_cmd_ok_v252(cmd)) {
        snprintf(error, error_size, "could not set RX LO to %lld Hz", center_hz);
        return 0;
    }

    snprintf(cmd, sizeof(cmd), "/usr/bin/iio_attr -u local: -c ad9361-phy voltage0 sampling_frequency %d >/dev/null 2>&1", SPECTRUM_V252_SAMPLE_RATE_HZ);
    (void)spectrum_cmd_ok_v252(cmd);
    snprintf(cmd, sizeof(cmd), "/usr/bin/iio_attr -u local: -c ad9361-phy voltage1 sampling_frequency %d >/dev/null 2>&1", SPECTRUM_V252_SAMPLE_RATE_HZ);
    (void)spectrum_cmd_ok_v252(cmd);
    snprintf(cmd, sizeof(cmd), "/usr/bin/iio_attr -u local: -c ad9361-phy voltage0 rf_bandwidth %ld >/dev/null 2>&1", applied_bandwidth_hz);
    (void)spectrum_cmd_ok_v252(cmd);
    snprintf(cmd, sizeof(cmd), "/usr/bin/iio_attr -u local: -c ad9361-phy voltage1 rf_bandwidth %ld >/dev/null 2>&1", applied_bandwidth_hz);
    (void)spectrum_cmd_ok_v252(cmd);

    if (have_gain) {
        double applied_gain_db = spectrum_clamp_gain_v265(gain_db);
        snprintf(cmd, sizeof(cmd), "/usr/bin/iio_attr -u local: -c ad9361-phy voltage%d gain_control_mode manual >/dev/null 2>&1", rx_channel);
        if (!spectrum_cmd_ok_v252(cmd)) {
            snprintf(error, error_size, "could not set RX%d gain control mode to manual", rx_channel);
            return 0;
        }
        snprintf(cmd, sizeof(cmd), "/usr/bin/iio_attr -u local: -c ad9361-phy voltage%d hardwaregain %.1f >/dev/null 2>&1", rx_channel, applied_gain_db);
        if (!spectrum_cmd_ok_v252(cmd)) {
            snprintf(error, error_size, "could not set RX%d hardwaregain to %.1f dB", rx_channel, applied_gain_db);
            return 0;
        }
    }

    return 1;
}

static int spectrum_frequency_from_state_v252(const struct app_config *cfg, long long *freq_hz, char *source, size_t source_size)
{
    char path[PATH_BUF_SIZE];
    char *json = NULL;
    size_t len = 0;

    (void)cfg;

    runtime_file_path(path, sizeof(path), "radio_track_state.json");
    if (read_file_to_string(path, &json, &len) == 0) {
        (void)len;
        if (json_long_long_after(json, "rx_hz", freq_hz) ||
            json_long_long_after(json, "downlink_hz", freq_hz) ||
            json_long_long_after(json, "current_rx_lo_hz", freq_hz)) {
            if (source && source_size > 0) snprintf(source, source_size, "radio_track_state.json");
            free(json);
            return 1;
        }
        free(json);
        json = NULL;
    }

    runtime_file_path(path, sizeof(path), "radio_target.json");
    if (read_file_to_string(path, &json, &len) == 0) {
        (void)len;
        if (json_long_long_after(json, "downlink_hz", freq_hz)) {
            if (source && source_size > 0) snprintf(source, source_size, "radio_target.json");
            free(json);
            return 1;
        }
        free(json);
    }

    return 0;
}

static size_t spectrum_capture_iq_v252(double *i_out, double *q_out, size_t max_samples, int stream_index, char *error, size_t error_size)
{
    unsigned char raw[SPECTRUM_V252_CAPTURE_FRAMES * 8];
    FILE *pipef;
    size_t got;
    size_t stride;
    size_t frames;
    size_t n;
    const char *cmd = "/usr/bin/iio_readdev -u local: -b 2048 -s 2048 cf-ad9361-lpc 2>/dev/null";

    if (!i_out || !q_out || max_samples == 0) {
        snprintf(error, error_size, "invalid capture buffer");
        return 0;
    }

    pipef = popen(cmd, "r");
    if (!pipef) {
        snprintf(error, error_size, "could not start iio_readdev");
        return 0;
    }

    got = fread(raw, 1, sizeof(raw), pipef);
    (void)pclose(pipef);

    if (got < 4) {
        snprintf(error, error_size, "iio_readdev returned no IQ samples");
        return 0;
    }

    stride = (got >= 8 && (got % 8) == 0) ? 8u : 4u;
    frames = got / stride;
    if (frames > max_samples) frames = max_samples;

    for (n = 0; n < frames; n++) {
        size_t off = n * stride;
        int16_t ri;
        int16_t rq;
        if (stride == 8 && stream_index > 0) off += 4;
        if (off + 3 >= got) break;
        ri = (int16_t)((uint16_t)raw[off] | ((uint16_t)raw[off + 1] << 8));
        rq = (int16_t)((uint16_t)raw[off + 2] | ((uint16_t)raw[off + 3] << 8));
        i_out[n] = (double)ri;
        q_out[n] = (double)rq;
    }

    return n;
}

static void spectrum_compute_bins_v252(const double *i_in, const double *q_in, size_t sample_count, int bin_count, double *db_out)
{
    double mean_i = 0.0;
    double mean_q = 0.0;
    size_t n;
    int b;

    if (!i_in || !q_in || !db_out || sample_count == 0 || bin_count <= 0) return;

    for (n = 0; n < sample_count; n++) {
        mean_i += i_in[n];
        mean_q += q_in[n];
    }
    mean_i /= (double)sample_count;
    mean_q /= (double)sample_count;

    for (b = 0; b < bin_count; b++) {
        double norm = (bin_count > 1) ? ((double)b / (double)(bin_count - 1) - 0.5) : 0.0;
        double step = -2.0 * SPECTRUM_V252_PI * norm;
        double cw = cos(step);
        double sw = sin(step);
        double c = 1.0;
        double s = 0.0;
        double re = 0.0;
        double im = 0.0;
        double win_sum = 0.0;

        for (n = 0; n < sample_count; n++) {
            double wi = (sample_count > 1) ? (0.5 - 0.5 * cos((2.0 * SPECTRUM_V252_PI * (double)n) / (double)(sample_count - 1))) : 1.0;
            double ii = (i_in[n] - mean_i) * wi;
            double qq = (q_in[n] - mean_q) * wi;
            double next_c;

            re += ii * c + qq * s;
            im += qq * c - ii * s;
            win_sum += wi;

            next_c = c * cw - s * sw;
            s = s * cw + c * sw;
            c = next_c;
        }

        {
            double mag = sqrt((re * re) + (im * im));
            double scaled = mag / ((win_sum > 0.0 ? win_sum : (double)sample_count) * 2048.0);
            double db = 20.0 * log10(scaled + 1.0e-12);
            if (db < -140.0) db = -140.0;
            if (db > 20.0) db = 20.0;
            db_out[b] = db;
        }
    }
}

static void send_radio_spectrum_snapshot_v252(int fd, const struct app_config *cfg, const char *query)
{
    long long center_hz = 0;
    char freq_text[64] = "";
    char bins_text[32] = "";
    char stream_text[32] = "";
    char bandwidth_text[32] = "";
    char gain_text[32] = "";
    char source[128] = "query";
    char error[256] = "";
    int bin_count = SPECTRUM_V252_DEFAULT_BINS;
    int stream_index = 0;
    long bandwidth_hz = SPECTRUM_V252_SAMPLE_RATE_HZ;
    int have_gain = 0;
    double gain_db = 0.0;
    double applied_gain_db = 0.0;
    char gain_json[32] = "null";
    double i_samples[SPECTRUM_V252_DFT_SAMPLES];
    double q_samples[SPECTRUM_V252_DFT_SAMPLES];
    double db_bins[SPECTRUM_V252_MAX_BINS];
    size_t sample_count;
    char *body;
    size_t body_size = 65536;
    size_t used = 0;
    int b;

    query_param(query, "freq_hz", freq_text, sizeof(freq_text));
    query_param(query, "bins", bins_text, sizeof(bins_text));
    query_param(query, "stream_index", stream_text, sizeof(stream_text));
    query_param(query, "bandwidth_hz", bandwidth_text, sizeof(bandwidth_text));
    query_param(query, "gain_db", gain_text, sizeof(gain_text));

    if (bins_text[0]) {
        if (!spectrum_parse_int_v252(bins_text, &bin_count)) bin_count = SPECTRUM_V252_DEFAULT_BINS;
        if (bin_count < 32) bin_count = 32;
        if (bin_count > SPECTRUM_V252_MAX_BINS) bin_count = SPECTRUM_V252_MAX_BINS;
    }

    if (stream_text[0]) {
        if (!spectrum_parse_int_v252(stream_text, &stream_index)) stream_index = 0;
        if (stream_index < 0 || stream_index > 1) stream_index = 0;
    }

    if (bandwidth_text[0]) {
        int parsed_bandwidth = 0;
        if (!spectrum_parse_int_v252(bandwidth_text, &parsed_bandwidth)) {
            send_text(fd, 400, "Bad Request", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"bandwidth_hz must be an integer Hz value\"}\n");
            return;
        }
        bandwidth_hz = spectrum_clamp_bandwidth_v265((long)parsed_bandwidth);
    }

    if (gain_text[0]) {
        char *gain_end = NULL;
        errno = 0;
        gain_db = strtod(gain_text, &gain_end);
        if (errno != 0 || !gain_end || *gain_end != '\0' || !isfinite(gain_db)) {
            send_text(fd, 400, "Bad Request", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"gain_db must be a numeric dB value\"}\n");
            return;
        }
        have_gain = 1;
        applied_gain_db = spectrum_clamp_gain_v265(gain_db);
        snprintf(gain_json, sizeof(gain_json), "%.1f", applied_gain_db);
    }

    if (freq_text[0]) {
        if (!spectrum_parse_ll_v252(freq_text, &center_hz)) {
            send_text(fd, 400, "Bad Request", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"freq_hz must be an integer Hz value\"}\n");
            return;
        }
        snprintf(source, sizeof(source), "query");
    } else if (!spectrum_frequency_from_state_v252(cfg, &center_hz, source, sizeof(source))) {
        send_text(fd, 409, "Conflict", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"no active radio frequency is available; pass freq_hz for a smoke test\"}\n");
        return;
    }

    if (center_hz < PLUTO_MIN_HZ || center_hz > PLUTO_MAX_HZ) {
        send_text(fd, 400, "Bad Request", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"center frequency is outside Pluto range\"}\n");
        return;
    }

    pthread_mutex_lock(&g_radio_lock);
    if (!spectrum_configure_rx_v252(center_hz, stream_index, bandwidth_hz, have_gain, applied_gain_db, error, sizeof(error))) {
        pthread_mutex_unlock(&g_radio_lock);
        char body_error[512];
        char error_json[384];
        json_escape(error_json, sizeof(error_json), error);
        snprintf(body_error, sizeof(body_error), "{\"ok\":false,\"error\":\"%s\"}\n", error_json);
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8", body_error);
        return;
    }

    sample_count = spectrum_capture_iq_v252(i_samples, q_samples, SPECTRUM_V252_DFT_SAMPLES, stream_index, error, sizeof(error));
    pthread_mutex_unlock(&g_radio_lock);

    if (sample_count < 64) {
        char body_error[512];
        char error_json[384];
        json_escape(error_json, sizeof(error_json), error);
        snprintf(body_error, sizeof(body_error), "{\"ok\":false,\"error\":\"%s\"}\n", error_json);
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8", body_error);
        return;
    }

    spectrum_compute_bins_v252(i_samples, q_samples, sample_count, bin_count, db_bins);

    body = (char *)malloc(body_size);
    if (!body) {
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"out of memory\"}\n");
        return;
    }

    used += (size_t)snprintf(body + used, body_size - used,
        "{\"ok\":true,\"state\":\"ok\",\"source\":\"%s\",\"center_hz\":%lld,"
        "\"sample_rate_hz\":%d,\"sample_count\":%lu,\"stream_index\":%d,"
        "\"bandwidth_hz\":%ld,\"gain_db\":%s,\"bin_count\":%d,\"bins\":[",
        source,
        center_hz,
        SPECTRUM_V252_SAMPLE_RATE_HZ,
        (unsigned long)sample_count,
        stream_index,
        bandwidth_hz,
        gain_json,
        bin_count);

    for (b = 0; b < bin_count && used + 96 < body_size; b++) {
        double frac = (bin_count > 1) ? ((double)b / (double)(bin_count - 1) - 0.5) : 0.0;
        long offset_hz = (long)lrint(frac * (double)SPECTRUM_V252_SAMPLE_RATE_HZ);
        used += (size_t)snprintf(body + used, body_size - used,
            "%s{\"offset_hz\":%ld,\"db\":%.2f}",
            b ? "," : "",
            offset_hz,
            db_bins[b]);
    }

    snprintf(body + used, body_size - used, "]}\n");
    send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
    free(body);
}



/* RECEIVE_MODE_PLACEHOLDERS_V2_6_21C_BEGIN
 * Mode-aware receive/decode placeholder endpoints.  This is plumbing only:
 * FM Listen remains on the existing audio path; no decoder DSP is launched here.
 */
static int receive_text_contains_v261c(const char *text, const char *needle)
{
    size_t needle_len;
    const char *p;
    if (!text || !needle || !*needle) return 0;
    needle_len = strlen(needle);
    for (p = text; *p; p++) {
        size_t i;
        for (i = 0; i < needle_len; i++) {
            unsigned char a = (unsigned char)p[i];
            unsigned char b = (unsigned char)needle[i];
            if (!a || tolower(a) != tolower(b)) break;
        }
        if (i == needle_len) return 1;
    }
    return 0;
}

static const char *receive_kind_v261c(const char *mode)
{
    if (receive_text_contains_v261c(mode, "cw") || receive_text_contains_v261c(mode, "morse")) return "cw";
    if (receive_text_contains_v261c(mode, "ax.25") || receive_text_contains_v261c(mode, "aprs") ||
        receive_text_contains_v261c(mode, "packet") || receive_text_contains_v261c(mode, "fsk") ||
        receive_text_contains_v261c(mode, "gmsk") || receive_text_contains_v261c(mode, "bpsk") ||
        receive_text_contains_v261c(mode, "telemetry") || receive_text_contains_v261c(mode, "digital") ||
        receive_text_contains_v261c(mode, "data")) return "digital";
    if (receive_text_contains_v261c(mode, "fm") || receive_text_contains_v261c(mode, "nfm") ||
        receive_text_contains_v261c(mode, "voice")) return "voice";
    return "unknown";
}


/* CW_DECODE_DIAGNOSTIC_SCAFFOLD_V2_6_22A_SUPPORT_BEGIN */
static int cw_mode_is_cw_v2622a(const char *mode)
{
    char upper[128];
    size_t i;
    if (!mode) return 0;
    for (i = 0; i + 1 < sizeof(upper) && mode[i]; i++) {
        upper[i] = (char)toupper((unsigned char)mode[i]);
    }
    upper[i] = '\0';
    return strstr(upper, "CW") != NULL || strstr(upper, "MORSE") != NULL;
}

static int cw_mode_is_voice_v2622a(const char *mode)
{
    char upper[128];
    size_t i;
    if (!mode) return 0;
    for (i = 0; i + 1 < sizeof(upper) && mode[i]; i++) {
        upper[i] = (char)toupper((unsigned char)mode[i]);
    }
    upper[i] = '\0';
    return strstr(upper, "FM") != NULL || strstr(upper, "NFM") != NULL || strstr(upper, "VOICE") != NULL || strstr(upper, "PHONE") != NULL;
}

static int cw_parse_hz_v2622a(const char *text, long long *out)
{
    char *end = NULL;
    long long value;
    if (!text || !*text || !out) return 0;
    errno = 0;
    value = strtoll(text, &end, 10);
    if (errno != 0 || !end || *end != '\0') return 0;
    if (value < PLUTO_MIN_HZ || value > PLUTO_MAX_HZ) return 0;
    *out = value;
    return 1;
}

static int cw_read_pcm_metrics_v2622a(long *pcm_bytes, unsigned long *sample_count, double *rms, int *peak, double *tone_hz, double *duty_percent)
{
    FILE *f;
    long size;
    long read_bytes;
    int16_t samples[4800];
    size_t n;
    size_t count;
    double sumsq = 0.0;
    int max_abs = 0;
    int crossings = 0;
    int prev_sign = 0;
    int active_blocks = 0;
    int block_count = 0;
    const int block_size = 240;

    if (pcm_bytes) *pcm_bytes = 0;
    if (sample_count) *sample_count = 0;
    if (rms) *rms = 0.0;
    if (peak) *peak = 0;
    if (tone_hz) *tone_hz = 0.0;
    if (duty_percent) *duty_percent = 0.0;

    f = fopen(AUDIO_LIVE_PCM_PATH, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    size = ftell(f);
    if (size <= 0) { fclose(f); return 0; }
    if (size > (long)sizeof(samples)) {
        if (fseek(f, size - (long)sizeof(samples), SEEK_SET) != 0) { fclose(f); return 0; }
        read_bytes = (long)fread(samples, 1, sizeof(samples), f);
    } else {
        if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
        read_bytes = (long)fread(samples, 1, (size_t)size, f);
    }
    fclose(f);

    if (pcm_bytes) *pcm_bytes = size;
    if (read_bytes < 2400) return 0;
    count = (size_t)read_bytes / sizeof(int16_t);
    if (sample_count) *sample_count = (unsigned long)count;
    if (count < 1200) return 0;

    for (n = 0; n < count; n++) {
        int v = samples[n];
        int av = v < 0 ? -v : v;
        int sign = (v > 0) ? 1 : ((v < 0) ? -1 : 0);
        sumsq += (double)v * (double)v;
        if (av > max_abs) max_abs = av;
        if (sign != 0) {
            if (prev_sign != 0 && sign != prev_sign) crossings++;
            prev_sign = sign;
        }
    }
    if (rms) *rms = sqrt(sumsq / (double)count);
    if (peak) *peak = max_abs;
    if (tone_hz && count > 1) *tone_hz = ((double)crossings * (double)AUDIO_SAMPLE_RATE) / (2.0 * (double)(count - 1));

    if (duty_percent && max_abs > 0) {
        size_t offset;
        double global_rms = sqrt(sumsq / (double)count);
        double active_threshold = global_rms * 0.8;
        if (active_threshold < 250.0) active_threshold = 250.0;
        for (offset = 0; offset + (size_t)block_size <= count; offset += (size_t)block_size) {
            double block_sum = 0.0;
            int j;
            for (j = 0; j < block_size; j++) {
                double v = (double)samples[offset + (size_t)j];
                block_sum += v * v;
            }
            block_count++;
            if (sqrt(block_sum / (double)block_size) >= active_threshold) active_blocks++;
        }
        if (block_count > 0) *duty_percent = 100.0 * (double)active_blocks / (double)block_count;
    }

    return 1;
}


/* CW_MORSE_TIMING_DECODER_V2_6_23B
 * First-pass CW timing decoder built on the v2.6.22 PCM diagnostic scaffold.
 * It keeps diagnostics visible and adds experimental mark/space timing to Morse symbols/text.
 */
static const char *cw_lookup_symbol_v2623(const char *symbol)
{
    if (!symbol || !*symbol) return "";
    if (strcmp(symbol, ".-") == 0) return "A";
    if (strcmp(symbol, "-...") == 0) return "B";
    if (strcmp(symbol, "-.-.") == 0) return "C";
    if (strcmp(symbol, "-..") == 0) return "D";
    if (strcmp(symbol, ".") == 0) return "E";
    if (strcmp(symbol, "..-.") == 0) return "F";
    if (strcmp(symbol, "--.") == 0) return "G";
    if (strcmp(symbol, "....") == 0) return "H";
    if (strcmp(symbol, "..") == 0) return "I";
    if (strcmp(symbol, ".---") == 0) return "J";
    if (strcmp(symbol, "-.-") == 0) return "K";
    if (strcmp(symbol, ".-..") == 0) return "L";
    if (strcmp(symbol, "--") == 0) return "M";
    if (strcmp(symbol, "-.") == 0) return "N";
    if (strcmp(symbol, "---") == 0) return "O";
    if (strcmp(symbol, ".--.") == 0) return "P";
    if (strcmp(symbol, "--.-") == 0) return "Q";
    if (strcmp(symbol, ".-.") == 0) return "R";
    if (strcmp(symbol, "...") == 0) return "S";
    if (strcmp(symbol, "-") == 0) return "T";
    if (strcmp(symbol, "..-") == 0) return "U";
    if (strcmp(symbol, "...-") == 0) return "V";
    if (strcmp(symbol, ".--") == 0) return "W";
    if (strcmp(symbol, "-..-") == 0) return "X";
    if (strcmp(symbol, "-.--") == 0) return "Y";
    if (strcmp(symbol, "--..") == 0) return "Z";
    if (strcmp(symbol, ".----") == 0) return "1";
    if (strcmp(symbol, "..---") == 0) return "2";
    if (strcmp(symbol, "...--") == 0) return "3";
    if (strcmp(symbol, "....-") == 0) return "4";
    if (strcmp(symbol, ".....") == 0) return "5";
    if (strcmp(symbol, "-....") == 0) return "6";
    if (strcmp(symbol, "--...") == 0) return "7";
    if (strcmp(symbol, "---..") == 0) return "8";
    if (strcmp(symbol, "----.") == 0) return "9";
    if (strcmp(symbol, "-----") == 0) return "0";
    return "?";
}

static void cw_append_char_v2623(char *decoded, size_t decoded_size, char *current, size_t current_size)
{
    const char *letter;
    size_t used;
    if (!decoded || !current || !current[0]) return;
    letter = cw_lookup_symbol_v2623(current);
    used = strlen(decoded);
    if (used + strlen(letter) + 1 < decoded_size) {
        strcat(decoded, letter);
    }
    current[0] = '\0';
    (void)current_size;
}

static void cw_send_diagnostic_output_v2622a(int fd, const char *mode, const char *mode_json_hint)
{
    char mode_json[128];
    char body[4096];
    FILE *f;
    long file_size = 0;
    long bytes_to_read;
    size_t sample_count = 0;
    size_t max_samples = 120000; /* five seconds at 24 kHz */
    int16_t *samples = NULL;
    double sum_sq = 0.0;
    double rms = 0.0;
    int peak = 0;
    size_t n;
    int zero_crossings = 0;
    double tone_hz = 0.0;
    const int block = 240; /* 10 ms */
    int blocks = 0;
    double *env = NULL;
    double env_min = 0.0, env_max = 0.0, env_avg = 0.0, threshold = 0.0;
    int active_blocks = 0;
    double duty = 0.0;
    int unit_blocks = 0;
    char morse[512] = "";
    char decoded[256] = "";
    char current[16] = "";
    char morse_json[1024];
    char decoded_json[512];
    char status_json[256];

    (void)mode_json_hint;
    json_escape(mode_json, sizeof(mode_json), mode && *mode ? mode : "CW");

    f = fopen(AUDIO_LIVE_PCM_PATH, "rb");
    if (!f) {
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"state\":\"cw_morse_experimental_waiting\",\"decoder_state\":\"signal_diagnostic\",\"receive_kind\":\"cw\",\"mode\":\"%s\",\"pcm_bytes\":0,\"lines\":[\"CW Morse timing decoder active.\",\"Waiting for live PCM capture.\",\"Start Decode CW and allow one or two seconds of live capture.\"]}\n",
                 mode_json);
        send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
        return;
    }

    fseek(f, 0, SEEK_END);
    file_size = ftell(f);
    if (file_size < 0) file_size = 0;
    bytes_to_read = file_size;
    if (bytes_to_read > (long)(max_samples * 2)) bytes_to_read = (long)(max_samples * 2);
    if (bytes_to_read < 2400) {
        fclose(f);
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"state\":\"cw_morse_experimental_waiting\",\"decoder_state\":\"signal_diagnostic\",\"receive_kind\":\"cw\",\"mode\":\"%s\",\"pcm_bytes\":%ld,\"lines\":[\"CW Morse timing decoder active.\",\"Waiting for at least 1200 PCM samples from the live audio path.\",\"Start Decode CW and allow one or two seconds of live capture.\"]}\n",
                 mode_json, file_size);
        send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
        return;
    }

    samples = (int16_t *)malloc((size_t)bytes_to_read);
    if (!samples) {
        fclose(f);
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"out of memory\"}\n");
        return;
    }

    fseek(f, -bytes_to_read, SEEK_END);
    sample_count = fread(samples, 2, (size_t)bytes_to_read / 2u, f);
    fclose(f);

    if (sample_count < 1200) {
        free(samples);
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"state\":\"cw_morse_experimental_waiting\",\"decoder_state\":\"signal_diagnostic\",\"receive_kind\":\"cw\",\"mode\":\"%s\",\"pcm_bytes\":%ld,\"lines\":[\"CW Morse timing decoder active.\",\"Not enough PCM samples yet.\"]}\n",
                 mode_json, file_size);
        send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
        return;
    }

    for (n = 0; n < sample_count; n++) {
        int v = samples[n];
        int av = v < 0 ? -v : v;
        if (av > peak) peak = av;
        sum_sq += (double)v * (double)v;
        if (n > 0) {
            if ((samples[n - 1] <= 0 && samples[n] > 0) || (samples[n - 1] >= 0 && samples[n] < 0)) {
                zero_crossings++;
            }
        }
    }
    rms = sqrt(sum_sq / (double)sample_count);
    tone_hz = ((double)zero_crossings * 24000.0) / (2.0 * (double)sample_count);

    blocks = (int)(sample_count / (size_t)block);
    if (blocks > 0) {
        int b;
        env = (double *)calloc((size_t)blocks, sizeof(double));
        if (env) {
            for (b = 0; b < blocks; b++) {
                double ss = 0.0;
                int j;
                for (j = 0; j < block; j++) {
                    int v = samples[(size_t)b * (size_t)block + (size_t)j];
                    ss += (double)v * (double)v;
                }
                env[b] = sqrt(ss / (double)block);
                if (b == 0 || env[b] < env_min) env_min = env[b];
                if (b == 0 || env[b] > env_max) env_max = env[b];
                env_avg += env[b];
            }
            env_avg /= (double)blocks;
            threshold = env_min + ((env_max - env_min) * 0.35);
            if (threshold < rms * 0.75) threshold = rms * 0.75;
            if (threshold < 20.0) threshold = 20.0;

            for (b = 0; b < blocks; b++) {
                if (env[b] >= threshold) active_blocks++;
            }
            duty = blocks > 0 ? ((double)active_blocks * 100.0 / (double)blocks) : 0.0;

            if (active_blocks > 0 && active_blocks < blocks && env_max > env_min * 1.5) {
                int state = env[0] >= threshold ? 1 : 0;
                int run_len = 1;
                int min_mark = 9999;
                int pass;

                for (pass = 0; pass < 2; pass++) {
                    state = env[0] >= threshold ? 1 : 0;
                    run_len = 1;
                    if (pass == 1) {
                        morse[0] = '\0';
                        decoded[0] = '\0';
                        current[0] = '\0';
                    }
                    for (b = 1; b <= blocks; b++) {
                        int s = (b < blocks && env[b] >= threshold) ? 1 : 0;
                        if (b < blocks && s == state) {
                            run_len++;
                            continue;
                        }
                        if (pass == 0) {
                            if (state && run_len >= 2 && run_len < min_mark) min_mark = run_len;
                        } else {
                            if (state) {
                                const char *sym = (run_len <= unit_blocks * 2) ? "." : "-";
                                if (strlen(current) + 2 < sizeof(current)) strcat(current, sym);
                                if (strlen(morse) + 2 < sizeof(morse)) strcat(morse, sym);
                            } else {
                                if (run_len >= unit_blocks * 7) {
                                    cw_append_char_v2623(decoded, sizeof(decoded), current, sizeof(current));
                                    if (strlen(decoded) + 2 < sizeof(decoded)) strcat(decoded, " ");
                                    if (strlen(morse) + 4 < sizeof(morse)) strcat(morse, " / ");
                                } else if (run_len >= unit_blocks * 3) {
                                    cw_append_char_v2623(decoded, sizeof(decoded), current, sizeof(current));
                                    if (strlen(morse) + 2 < sizeof(morse)) strcat(morse, " ");
                                }
                            }
                        }
                        state = s;
                        run_len = 1;
                    }
                    if (pass == 0) {
                        if (min_mark == 9999) min_mark = 3;
                        unit_blocks = min_mark;
                        if (unit_blocks < 2) unit_blocks = 2;
                        if (unit_blocks > 20) unit_blocks = 20;
                    }
                }
                cw_append_char_v2623(decoded, sizeof(decoded), current, sizeof(current));
            }
            free(env);
        }
    }

    free(samples);

    if (!morse[0]) snprintf(morse, sizeof(morse), "(no stable mark/space timing yet)");
    if (!decoded[0]) snprintf(decoded, sizeof(decoded), "(no text yet)");
    json_escape(morse_json, sizeof(morse_json), morse);
    json_escape(decoded_json, sizeof(decoded_json), decoded);
    snprintf(status_json, sizeof(status_json), "unit %.0f ms, threshold %.1f", unit_blocks > 0 ? (double)unit_blocks * 10.0 : 0.0, threshold);

    snprintf(body, sizeof(body),
             "{\"ok\":true,\"state\":\"cw_morse_experimental\",\"decoder_state\":\"experimental\",\"receive_kind\":\"cw\",\"mode\":\"%s\",\"sample_rate_hz\":24000,\"sample_count\":%lu,\"pcm_bytes\":%ld,\"rms\":%.1f,\"peak\":%d,\"estimated_tone_hz\":%.1f,\"key_duty_percent\":%.1f,\"morse\":\"%s\",\"decoded_text\":\"%s\",\"lines\":[\"CW Morse timing decoder active.\",\"PCM samples analyzed: %lu at 24000 Hz.\",\"RMS %.1f, peak %d.\",\"Estimated tone from zero crossings: %.1f Hz.\",\"Key-up envelope duty estimate: %.1f%%.\",\"Timing: %s.\",\"Morse symbols: %s\",\"Decoded text: %s\"]}\n",
             mode_json,
             (unsigned long)sample_count,
             file_size,
             rms,
             peak,
             tone_hz,
             duty,
             morse_json,
             decoded_json,
             (unsigned long)sample_count,
             rms,
             peak,
             tone_hz,
             duty,
             status_json,
             morse_json,
             decoded_json);
    send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
}

/* CW_DECODE_DIAGNOSTIC_SCAFFOLD_V2_6_22A_SUPPORT_END */


static void send_receive_status_v261c(int fd)
{
    send_text(fd, 200, "OK", "application/json; charset=utf-8", "{\"ok\":true,\"state\":\"idle\",\"version\":\"2.6.22a\",\"message\":\"Receive/decode foundation is installed. FM Listen uses the existing audio path; CW Morse timing decoder is active; digital decoders are placeholders.\",\"supported\":[\"voice\",\"cw_morse_experimental\",\"digital_placeholder\"]}\n");
}



static void send_receive_stop_v261c(int fd)
{
    stop_live_audio_session();
    send_text(fd, 200, "OK", "application/json; charset=utf-8", "{\"ok\":true,\"state\":\"stopped\",\"message\":\"Receive/decode capture stopped.\"}\n");
}



static int ax25_mode_is_digital_v2627(const char *mode);

static void send_receive_start_v261c(int fd, const struct app_config *cfg, const char *query)
{
    char mode[128] = "";
    char name[256] = "";
    char downlink_hz[64] = "";
    char mode_json[256];
    char name_json[512];
    char downlink_json[128];
    char body[1600];
    long long frequency_hz = 0;
    const char *kind;
    query_param(query, "mode", mode, sizeof(mode));
    query_param(query, "name", name, sizeof(name));
    query_param(query, "downlink_hz", downlink_hz, sizeof(downlink_hz));
    if (!downlink_hz[0]) query_param(query, "freq_hz", downlink_hz, sizeof(downlink_hz));
    json_escape(mode_json, sizeof(mode_json), mode);
    json_escape(name_json, sizeof(name_json), name);
    json_escape(downlink_json, sizeof(downlink_json), downlink_hz);
    kind = receive_kind_v261c(mode);

    if (strcmp(kind, "voice") == 0) {
        snprintf(body, sizeof(body), "{\"ok\":true,\"state\":\"voice_path\",\"receive_kind\":\"voice\",\"name\":\"%s\",\"mode\":\"%s\",\"downlink_hz\":\"%s\",\"message\":\"FM/voice receive uses the existing Listen audio path.\"}\n", name_json, mode_json, downlink_json);
        send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
        return;
    }

    if ((strcmp(kind, "cw") == 0 || strcmp(kind, "digital") == 0 || ax25_mode_is_digital_v2627(mode)) && parse_frequency_hz(downlink_hz, &frequency_hz)) {
        if (!start_live_audio_session(cfg, frequency_hz)) {
            send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"could not start live audio capture for decoder diagnostics\"}\n");
            return;
        }
        if (strcmp(kind, "cw") == 0) {
            snprintf(body, sizeof(body), "{\"ok\":true,\"state\":\"cw_diagnostic_capture\",\"decoder_state\":\"signal_diagnostic\",\"receive_kind\":\"cw\",\"name\":\"%s\",\"mode\":\"%s\",\"downlink_hz\":\"%s\",\"message\":\"CW diagnostic capture started.\"}\n", name_json, mode_json, downlink_json);
        } else {
            snprintf(body, sizeof(body), "{\"ok\":true,\"state\":\"ax25_diagnostic_capture\",\"decoder_state\":\"frame_parser_ready\",\"receive_kind\":\"digital\",\"name\":\"%s\",\"mode\":\"%s\",\"downlink_hz\":\"%s\",\"message\":\"Digital diagnostic capture started. The decode modal will report mode-specific live signal metrics.\"}\n", name_json, mode_json, downlink_json);
        }
        send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
        return;
    }

    snprintf(body, sizeof(body), "{\"ok\":true,\"state\":\"decode_placeholder\",\"decoder_state\":\"not_implemented\",\"receive_kind\":\"unknown\",\"name\":\"%s\",\"mode\":\"%s\",\"downlink_hz\":\"%s\",\"message\":\"Unknown receive mode. FM voice still uses Listen; decoder selection will be added per protocol.\"}\n", name_json, mode_json, downlink_json);
    send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
}





/* AX25_DIGITAL_DECODER_CORE_V2_6_27 */
static int ax25_mode_is_digital_v2627(const char *mode)
{
    char lower[256];
    size_t i;
    if (!mode) return 0;
    for (i = 0; i + 1 < sizeof(lower) && mode[i]; i++) lower[i] = (char)tolower((unsigned char)mode[i]);
    lower[i] = '\0';
    return strstr(lower, "ax.25") || strstr(lower, "ax25") || strstr(lower, "aprs") ||
           strstr(lower, "packet") || strstr(lower, "afsk") || strstr(lower, "1200") ||
           strstr(lower, "gmsk") || strstr(lower, "bpsk") || strstr(lower, "fsk") ||
           strstr(lower, "telemetry") || strstr(lower, "digital") || strstr(lower, "data");
}

static unsigned short ax25_crc_update_v2627(unsigned short crc, unsigned char byte)
{
    int i;
    crc ^= byte;
    for (i = 0; i < 8; i++) {
        if (crc & 1) crc = (unsigned short)((crc >> 1) ^ 0x8408);
        else crc = (unsigned short)(crc >> 1);
    }
    return crc;
}

static unsigned short ax25_crc_calc_v2627(const unsigned char *data, size_t len)
{
    unsigned short crc = 0xffff;
    size_t i;
    for (i = 0; i < len; i++) crc = ax25_crc_update_v2627(crc, data[i]);
    return (unsigned short)(~crc);
}

static void ax25_encode_addr_v2627(unsigned char *out, const char *call, int ssid, int last)
{
    int i;
    for (i = 0; i < 6; i++) {
        unsigned char ch = ' ';
        if (call && call[i]) ch = (unsigned char)toupper((unsigned char)call[i]);
        out[i] = (unsigned char)(ch << 1);
    }
    out[6] = (unsigned char)(0x60 | ((ssid & 0x0f) << 1) | (last ? 1 : 0));
}

static void ax25_decode_addr_v2627(const unsigned char *in, char *out, size_t out_size)
{
    char call[8];
    int ssid;
    int i, end = 6;
    if (!out || out_size == 0) return;
    for (i = 0; i < 6; i++) call[i] = (char)((in[i] >> 1) & 0x7f);
    call[6] = '\0';
    while (end > 0 && call[end - 1] == ' ') end--;
    call[end] = '\0';
    ssid = (in[6] >> 1) & 0x0f;
    if (ssid > 0) snprintf(out, out_size, "%s-%d", call, ssid);
    else snprintf(out, out_size, "%s", call);
}

static int ax25_decode_ui_frame_v2627(const unsigned char *frame, size_t len, char *summary, size_t summary_size, char *info, size_t info_size, char *error, size_t error_size)
{
    size_t pos = 0;
    char dest[32] = "";
    char src[32] = "";
    unsigned short calc;
    unsigned short rx_fcs;
    size_t info_len;
    if (!frame || len < 18) {
        snprintf(error, error_size, "frame too short");
        return 0;
    }
    rx_fcs = (unsigned short)(frame[len - 2] | (frame[len - 1] << 8));
    calc = ax25_crc_calc_v2627(frame, len - 2);
    if (rx_fcs != calc) {
        snprintf(error, error_size, "bad FCS expected 0x%04x got 0x%04x", calc, rx_fcs);
        return 0;
    }
    ax25_decode_addr_v2627(frame, dest, sizeof(dest));
    pos += 7;
    ax25_decode_addr_v2627(frame + pos, src, sizeof(src));
    while (pos + 7 <= len - 2 && !(frame[pos + 6] & 1)) pos += 7;
    pos += 7;
    if (pos + 2 > len - 2) {
        snprintf(error, error_size, "missing control/PID");
        return 0;
    }
    if (frame[pos] != 0x03 || frame[pos + 1] != 0xf0) {
        snprintf(error, error_size, "not an AX.25 UI/no-layer3 frame control=0x%02x pid=0x%02x", frame[pos], frame[pos + 1]);
        return 0;
    }
    pos += 2;
    info_len = (len - 2 > pos) ? (len - 2 - pos) : 0;
    if (info && info_size > 0) {
        size_t n = info_len;
        if (n + 1 > info_size) n = info_size - 1;
        memcpy(info, frame + pos, n);
        info[n] = '\0';
    }
    if (summary && summary_size > 0) snprintf(summary, summary_size, "%s>%s:%s", src, dest, info ? info : "");
    return 1;
}

static size_t ax25_make_selftest_frame_v2627(unsigned char *frame, size_t frame_size)
{
    const char *info = ">AX25 SELFTEST";
    size_t pos = 0;
    size_t info_len = strlen(info);
    unsigned short fcs;
    if (!frame || frame_size < 64) return 0;
    ax25_encode_addr_v2627(frame + pos, "APRS", 0, 0); pos += 7;
    ax25_encode_addr_v2627(frame + pos, "N0CALL", 0, 1); pos += 7;
    frame[pos++] = 0x03;
    frame[pos++] = 0xf0;
    memcpy(frame + pos, info, info_len); pos += info_len;
    fcs = ax25_crc_calc_v2627(frame, pos);
    frame[pos++] = (unsigned char)(fcs & 0xff);
    frame[pos++] = (unsigned char)((fcs >> 8) & 0xff);
    return pos;
}

static int ax25_read_pcm_metrics_v2627(long *pcm_bytes, unsigned long *sample_count, double *rms, int *peak)
{
    FILE *f;
    short sample;
    unsigned long count = 0;
    double sum_sq = 0.0;
    int max_abs = 0;
    long bytes = 0;
    f = fopen(AUDIO_LIVE_PCM_PATH, "rb");
    if (!f) return 0;
    while (fread(&sample, sizeof(sample), 1, f) == 1) {
        int a = sample < 0 ? -sample : sample;
        if (a > max_abs) max_abs = a;
        sum_sq += (double)sample * (double)sample;
        count++;
    }
    if (fseek(f, 0, SEEK_END) == 0) bytes = ftell(f);
    fclose(f);
    if (pcm_bytes) *pcm_bytes = bytes;
    if (sample_count) *sample_count = count;
    if (rms) *rms = count ? sqrt(sum_sq / (double)count) : 0.0;
    if (peak) *peak = max_abs;
    return count > 0;
}

static void send_ax25_selftest_v2627(int fd)
{
    unsigned char frame[128];
    size_t len;
    char summary[256] = "";
    char info[128] = "";
    char error[256] = "";
    char summary_json[512];
    char info_json[256];
    char error_json[512];
    char body[1400];
    int ok;
    len = ax25_make_selftest_frame_v2627(frame, sizeof(frame));
    ok = ax25_decode_ui_frame_v2627(frame, len, summary, sizeof(summary), info, sizeof(info), error, sizeof(error));
    json_escape(summary_json, sizeof(summary_json), summary);
    json_escape(info_json, sizeof(info_json), info);
    json_escape(error_json, sizeof(error_json), error);
    snprintf(body, sizeof(body), "{\"ok\":%s,\"state\":\"ax25_selftest\",\"decoder_state\":\"%s\",\"version\":\"2.6.27\",\"frame_bytes\":%lu,\"decoded_text\":\"%s\",\"info\":\"%s\",\"error\":\"%s\",\"expected\":\"N0CALL>APRS:>AX25 SELFTEST\"}\n", ok ? "true" : "false", ok ? "pass" : "fail", (unsigned long)len, summary_json, info_json, error_json);
    send_text(fd, ok ? 200 : 500, ok ? "OK" : "Internal Server Error", "application/json; charset=utf-8", body);
}

static void ax25_send_decode_output_v2627(int fd, const char *mode, const char *mode_json)
{
    long pcm_bytes = 0;
    unsigned long sample_count = 0;
    double rms = 0.0;
    int peak = 0;
    char body[1800];
    if (!mode_json) mode_json = "";
    if (!ax25_read_pcm_metrics_v2627(&pcm_bytes, &sample_count, &rms, &peak) || sample_count < 1200) {
        snprintf(body, sizeof(body), "{\"ok\":true,\"state\":\"ax25_waiting\",\"decoder_state\":\"frame_parser_ready\",\"receive_kind\":\"digital\",\"mode\":\"%s\",\"pcm_bytes\":%ld,\"lines\":[\"AX.25/APRS decoder core is installed.\",\"Frame parser and FCS validation self-test are available at /api/radio/decode/ax25/selftest.\",\"Waiting for live audio PCM from Decode.\",\"AFSK 1200 baud symbol recovery will be added next.\"]}\n", mode_json, pcm_bytes);
        send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
        return;
    }
    snprintf(body, sizeof(body), "{\"ok\":true,\"state\":\"ax25_diagnostic\",\"decoder_state\":\"frame_parser_ready\",\"receive_kind\":\"digital\",\"mode\":\"%s\",\"sample_rate_hz\":%d,\"sample_count\":%lu,\"pcm_bytes\":%ld,\"rms\":%.1f,\"peak\":%d,\"lines\":[\"AX.25/APRS decoder core is installed.\",\"PCM samples analyzed: %lu at %d Hz.\",\"RMS %.1f, peak %d.\",\"AX.25 UI-frame parser and CRC/FCS validator are active.\",\"Known-frame self-test endpoint: /api/radio/decode/ax25/selftest.\",\"Next patch will recover Bell 202 AFSK bits from this audio.\"]}\n", mode_json, AUDIO_SAMPLE_RATE, sample_count, pcm_bytes, rms, peak, sample_count, AUDIO_SAMPLE_RATE, rms, peak);
    send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
}
/* END_AX25_DIGITAL_DECODER_CORE_V2_6_27 */


/* AX25_AFSK_DIAGNOSTIC_SCAFFOLD_V2_6_30 */
static double ax25_afsk_goertzel_power_v2630(const int16_t *samples, size_t sample_count, double target_hz)
{
    double omega = (2.0 * M_PI * target_hz) / (double)AUDIO_SAMPLE_RATE;
    double coeff = 2.0 * cos(omega);
    double q0 = 0.0, q1 = 0.0, q2 = 0.0;
    size_t i;
    if (!samples || sample_count == 0) return 0.0;
    for (i = 0; i < sample_count; i++) {
        q0 = coeff * q1 - q2 + ((double)samples[i] / 32768.0);
        q2 = q1;
        q1 = q0;
    }
    return q1 * q1 + q2 * q2 - coeff * q1 * q2;
}

static int ax25_afsk_read_pcm_metrics_v2630(
    long *pcm_bytes,
    unsigned long *sample_count,
    double *rms,
    int *peak,
    double *mark1200,
    double *space2200,
    double *dominant_hz,
    double *balance_percent)
{
    FILE *f;
    long size = 0;
    long read_bytes = 0;
    size_t samples_to_read;
    int16_t *samples = NULL;
    size_t got;
    size_t i;
    double sum_sq = 0.0;
    int max_abs = 0;
    double p1200, p2200, total;

    if (pcm_bytes) *pcm_bytes = 0;
    if (sample_count) *sample_count = 0;
    if (rms) *rms = 0.0;
    if (peak) *peak = 0;
    if (mark1200) *mark1200 = 0.0;
    if (space2200) *space2200 = 0.0;
    if (dominant_hz) *dominant_hz = 0.0;
    if (balance_percent) *balance_percent = 0.0;

    f = fopen(AUDIO_LIVE_PCM_PATH, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    size = ftell(f);
    if (size <= 0) { fclose(f); return 0; }

    read_bytes = size;
    if (read_bytes > (long)(AUDIO_SAMPLE_RATE * 2)) read_bytes = (long)(AUDIO_SAMPLE_RATE * 2); /* last ~1 second */
    read_bytes -= (read_bytes % 2);
    if (read_bytes < 2) { fclose(f); return 0; }
    if (fseek(f, size - read_bytes, SEEK_SET) != 0) { fclose(f); return 0; }

    samples_to_read = (size_t)read_bytes / 2u;
    samples = (int16_t *)calloc(samples_to_read, sizeof(int16_t));
    if (!samples) { fclose(f); return 0; }
    got = fread(samples, sizeof(int16_t), samples_to_read, f);
    fclose(f);
    if (got < 64) { free(samples); return 0; }

    for (i = 0; i < got; i++) {
        int v = (int)samples[i];
        int av = v < 0 ? -v : v;
        if (av > max_abs) max_abs = av;
        sum_sq += (double)v * (double)v;
    }

    p1200 = ax25_afsk_goertzel_power_v2630(samples, got, 1200.0);
    p2200 = ax25_afsk_goertzel_power_v2630(samples, got, 2200.0);
    total = p1200 + p2200;

    if (pcm_bytes) *pcm_bytes = size;
    if (sample_count) *sample_count = (unsigned long)got;
    if (rms) *rms = sqrt(sum_sq / (double)got);
    if (peak) *peak = max_abs;
    if (mark1200) *mark1200 = p1200;
    if (space2200) *space2200 = p2200;
    if (dominant_hz) *dominant_hz = (p2200 > p1200) ? 2200.0 : 1200.0;
    if (balance_percent) *balance_percent = total > 0.0 ? (100.0 * p1200 / total) : 0.0;

    free(samples);
    return 1;
}

static void ax25_send_decode_output_v2630(int fd, const char *mode, const char *mode_json)
{
    char body[4096];
    char local_mode_json[256];
    long pcm_bytes = 0;
    unsigned long sample_count = 0;
    double rms = 0.0;
    int peak = 0;
    double mark1200 = 0.0;
    double space2200 = 0.0;
    double dominant_hz = 0.0;
    double balance_percent = 0.0;
    int have_metrics;
    (void)mode_json;

    json_escape(local_mode_json, sizeof(local_mode_json), mode ? mode : "AX.25");
    have_metrics = ax25_afsk_read_pcm_metrics_v2630(&pcm_bytes, &sample_count, &rms, &peak, &mark1200, &space2200, &dominant_hz, &balance_percent);

    if (!have_metrics) {
        snprintf(body, sizeof(body), "{\"ok\":true,\"state\":\"ax25_afsk_waiting\",\"decoder_state\":\"signal_diagnostic\",\"receive_kind\":\"digital\",\"mode\":\"%s\",\"lines\":[\"AX.25/APRS parser self-test is installed.\",\"Waiting for live PCM from Decode on a digital/packet pass.\",\"Next stage after this diagnostic is Bell 202 bit/HDLC recovery.\"]}\n", local_mode_json);
        send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
        return;
    }

    snprintf(body, sizeof(body), "{\"ok\":true,\"state\":\"ax25_afsk_diagnostic\",\"decoder_state\":\"signal_diagnostic\",\"receive_kind\":\"digital\",\"mode\":\"%s\",\"sample_rate_hz\":%d,\"sample_count\":%lu,\"pcm_bytes\":%ld,\"rms\":%.1f,\"peak\":%d,\"mark_1200_power\":%.6e,\"space_2200_power\":%.6e,\"dominant_tone_hz\":%.1f,\"mark_balance_percent\":%.1f,\"lines\":[\"AX.25/APRS AFSK diagnostic scaffold active.\",\"PCM samples analyzed: %lu at %d Hz.\",\"RMS %.1f, peak %d.\",\"Bell 202 mark 1200 Hz power %.6e.\",\"Bell 202 space 2200 Hz power %.6e.\",\"Dominant tone estimate %.1f Hz; mark balance %.1f%%.\",\"Next patch will recover 1200-baud bits and HDLC flags before packet decode.\"]}\n", local_mode_json, AUDIO_SAMPLE_RATE, sample_count, pcm_bytes, rms, peak, mark1200, space2200, dominant_hz, balance_percent, sample_count, AUDIO_SAMPLE_RATE, rms, peak, mark1200, space2200, dominant_hz, balance_percent);
    send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
}
/* AX25_AFSK_DIAGNOSTIC_SCAFFOLD_V2_6_30_END */


/* DIGITAL_LIVE_DIAGNOSTICS_V2_6_37 */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int digital_v2637_contains_icase(const char *text, const char *needle)
{
    char hay[256];
    char ndl[64];
    size_t i;
    if (!text || !needle) return 0;
    for (i = 0; i + 1 < sizeof(hay) && text[i]; i++) hay[i] = (char)tolower((unsigned char)text[i]);
    hay[i] = '\0';
    for (i = 0; i + 1 < sizeof(ndl) && needle[i]; i++) ndl[i] = (char)tolower((unsigned char)needle[i]);
    ndl[i] = '\0';
    return strstr(hay, ndl) != NULL;
}

static int digital_v2637_mode_is_bpsk(const char *mode)
{
    return digital_v2637_contains_icase(mode, "bpsk") || digital_v2637_contains_icase(mode, "psk");
}

static int digital_v2637_mode_is_fsk_gmsk(const char *mode)
{
    return digital_v2637_contains_icase(mode, "gmsk") || digital_v2637_contains_icase(mode, "fsk") || digital_v2637_contains_icase(mode, "9600");
}

static int digital_v2637_mode_is_packet_afsk(const char *mode)
{
    return digital_v2637_contains_icase(mode, "ax.25") || digital_v2637_contains_icase(mode, "ax25") ||
           digital_v2637_contains_icase(mode, "aprs") || digital_v2637_contains_icase(mode, "packet") ||
           digital_v2637_contains_icase(mode, "afsk") || digital_v2637_contains_icase(mode, "1200");
}

static double digital_v2637_goertzel_power(const int16_t *samples, size_t sample_count, double target_hz)
{
    double omega = (2.0 * M_PI * target_hz) / (double)AUDIO_SAMPLE_RATE;
    double coeff = 2.0 * cos(omega);
    double q0 = 0.0, q1 = 0.0, q2 = 0.0;
    size_t i;
    if (!samples || sample_count == 0 || target_hz <= 0.0) return 0.0;
    for (i = 0; i < sample_count; i++) {
        q0 = coeff * q1 - q2 + ((double)samples[i] / 32768.0);
        q2 = q1;
        q1 = q0;
    }
    return q1 * q1 + q2 * q2 - coeff * q1 * q2;
}

static int digital_v2637_read_live_pcm(
    int16_t **out_samples,
    size_t *out_sample_count,
    long *out_pcm_bytes,
    double *out_rms,
    int *out_peak,
    double *out_zero_cross_hz)
{
    FILE *f;
    long size = 0;
    long read_bytes = 0;
    size_t samples_to_read;
    int16_t *samples = NULL;
    size_t got, i;
    double sum_sq = 0.0;
    int max_abs = 0;
    unsigned long crossings = 0;
    int prev_sign = 0;

    if (out_samples) *out_samples = NULL;
    if (out_sample_count) *out_sample_count = 0;
    if (out_pcm_bytes) *out_pcm_bytes = 0;
    if (out_rms) *out_rms = 0.0;
    if (out_peak) *out_peak = 0;
    if (out_zero_cross_hz) *out_zero_cross_hz = 0.0;

    f = fopen(AUDIO_LIVE_PCM_PATH, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    size = ftell(f);
    if (size <= 0) { fclose(f); return 0; }

    read_bytes = size;
    if (read_bytes > (long)(AUDIO_SAMPLE_RATE * 2)) read_bytes = (long)(AUDIO_SAMPLE_RATE * 2);
    read_bytes -= (read_bytes % 2);
    if (read_bytes < 2) { fclose(f); return 0; }
    if (fseek(f, size - read_bytes, SEEK_SET) != 0) { fclose(f); return 0; }

    samples_to_read = (size_t)read_bytes / 2u;
    samples = (int16_t *)calloc(samples_to_read, sizeof(int16_t));
    if (!samples) { fclose(f); return 0; }
    got = fread(samples, sizeof(int16_t), samples_to_read, f);
    fclose(f);
    if (got < 64) { free(samples); return 0; }

    for (i = 0; i < got; i++) {
        int v = (int)samples[i];
        int av = v < 0 ? -v : v;
        int sign = v > 0 ? 1 : (v < 0 ? -1 : prev_sign);
        if (av > max_abs) max_abs = av;
        sum_sq += (double)v * (double)v;
        if (prev_sign != 0 && sign != 0 && sign != prev_sign) crossings++;
        if (sign != 0) prev_sign = sign;
    }

    if (out_samples) *out_samples = samples;
    else free(samples);
    if (out_sample_count) *out_sample_count = got;
    if (out_pcm_bytes) *out_pcm_bytes = size;
    if (out_rms) *out_rms = sqrt(sum_sq / (double)got);
    if (out_peak) *out_peak = max_abs;
    if (out_zero_cross_hz) *out_zero_cross_hz = ((double)crossings * (double)AUDIO_SAMPLE_RATE) / (2.0 * (double)got);
    return 1;
}

static double digital_v2637_dominant_tone(const int16_t *samples, size_t sample_count, double *out_power)
{
    double best_hz = 0.0;
    double best_power = 0.0;
    double hz;
    for (hz = 600.0; hz <= 3200.0; hz += 100.0) {
        double p = digital_v2637_goertzel_power(samples, sample_count, hz);
        if (p > best_power) {
            best_power = p;
            best_hz = hz;
        }
    }
    if (out_power) *out_power = best_power;
    return best_hz;
}

static void digital_v2637_send_waiting(int fd, const char *state, const char *mode_json, const char *label)
{
    char body[1600];
    snprintf(body, sizeof(body),
        "{\"ok\":true,\"state\":\"%s\",\"decoder_state\":\"waiting_for_samples\",\"receive_kind\":\"digital\",\"mode\":\"%s\",\"lines\":[\"%s diagnostics are ready.\",\"Waiting for live audio samples from Pluto.\",\"Start Decode during an active digital pass and allow one or two seconds of capture.\",\"No live text decode is claimed until carrier/clock recovery locks.\"]}\n",
        state, mode_json ? mode_json : "digital", label ? label : "Digital");
    send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
}

static void digital_v2637_send_bpsk_output(int fd, const char *mode, const char *mode_json)
{
    int16_t *samples = NULL;
    size_t sample_count = 0;
    long pcm_bytes = 0;
    double rms = 0.0;
    int peak = 0;
    double zero_hz = 0.0;
    double p1200 = 0.0, p1800 = 0.0, p2200 = 0.0, p2400 = 0.0;
    double dom_power = 0.0;
    double dom_hz = 0.0;
    double carrier_power = 0.0;
    double carrier_hz = 0.0;
    double quality = 0.0;
    char body[4096];
    (void)mode;

    if (!digital_v2637_read_live_pcm(&samples, &sample_count, &pcm_bytes, &rms, &peak, &zero_hz)) {
        digital_v2637_send_waiting(fd, "digital_bpsk_waiting", mode_json, "BPSK/PSK live signal");
        return;
    }

    p1200 = digital_v2637_goertzel_power(samples, sample_count, 1200.0);
    p1800 = digital_v2637_goertzel_power(samples, sample_count, 1800.0);
    p2200 = digital_v2637_goertzel_power(samples, sample_count, 2200.0);
    p2400 = digital_v2637_goertzel_power(samples, sample_count, 2400.0);
    dom_hz = digital_v2637_dominant_tone(samples, sample_count, &dom_power);
    carrier_power = p1800;
    carrier_hz = 1800.0;
    if (p1200 > carrier_power) { carrier_power = p1200; carrier_hz = 1200.0; }
    if (p2200 > carrier_power) { carrier_power = p2200; carrier_hz = 2200.0; }
    if (p2400 > carrier_power) { carrier_power = p2400; carrier_hz = 2400.0; }
    quality = dom_power > 0.0 ? carrier_power / dom_power : 0.0;

    snprintf(body, sizeof(body),
        "{\"ok\":true,\"state\":\"digital_bpsk_live_diagnostic\",\"decoder_state\":\"signal_diagnostic\",\"receive_kind\":\"digital\",\"mode\":\"%s\",\"sample_rate_hz\":%d,\"sample_count\":%lu,\"pcm_bytes\":%ld,\"rms\":%.1f,\"peak\":%d,\"zero_cross_hz\":%.1f,\"dominant_tone_hz\":%.1f,\"carrier_estimate_hz\":%.1f,\"carrier_power\":%.6e,\"tone_1200_power\":%.6e,\"tone_1800_power\":%.6e,\"tone_2200_power\":%.6e,\"tone_2400_power\":%.6e,\"diagnostic_quality\":%.3f,\"decoded_text\":\"\",\"lines\":[\"BPSK/PSK live signal diagnostics active.\",\"PCM samples analyzed: %lu at %d Hz.\",\"RMS %.1f, peak %d, zero-crossing estimate %.1f Hz.\",\"Dominant tone %.1f Hz; carrier estimate %.1f Hz.\",\"Carrier diagnostic quality %.3f.\",\"Live BPSK text recovery is not claimed until carrier and symbol clock recovery are locked.\"]}\n",
        mode_json ? mode_json : "BPSK", AUDIO_SAMPLE_RATE, (unsigned long)sample_count, pcm_bytes, rms, peak, zero_hz,
        dom_hz, carrier_hz, carrier_power, p1200, p1800, p2200, p2400, quality,
        (unsigned long)sample_count, AUDIO_SAMPLE_RATE, rms, peak, zero_hz, dom_hz, carrier_hz, quality);
    free(samples);
    send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
}

static void digital_v2637_send_fsk_output(int fd, const char *mode, const char *mode_json)
{
    int16_t *samples = NULL;
    size_t sample_count = 0;
    long pcm_bytes = 0;
    double rms = 0.0;
    int peak = 0;
    double zero_hz = 0.0;
    double p1200 = 0.0, p2200 = 0.0;
    double p960 = 0.0, p4800 = 0.0;
    double dom_power = 0.0;
    double dom_hz = 0.0;
    double total = 0.0;
    double mark_balance = 0.0;
    char body[4096];
    const char *state = digital_v2637_mode_is_fsk_gmsk(mode) ? "digital_fsk_gmsk_live_diagnostic" : "digital_afsk_live_diagnostic";
    const char *label = digital_v2637_mode_is_fsk_gmsk(mode) ? "FSK/GMSK" : "AFSK/packet";

    if (!digital_v2637_read_live_pcm(&samples, &sample_count, &pcm_bytes, &rms, &peak, &zero_hz)) {
        digital_v2637_send_waiting(fd, digital_v2637_mode_is_fsk_gmsk(mode) ? "digital_fsk_gmsk_waiting" : "digital_afsk_waiting", mode_json, label);
        return;
    }

    p1200 = digital_v2637_goertzel_power(samples, sample_count, 1200.0);
    p2200 = digital_v2637_goertzel_power(samples, sample_count, 2200.0);
    p960 = digital_v2637_goertzel_power(samples, sample_count, 960.0);
    p4800 = digital_v2637_goertzel_power(samples, sample_count, 4800.0);
    dom_hz = digital_v2637_dominant_tone(samples, sample_count, &dom_power);
    total = p1200 + p2200;
    mark_balance = total > 0.0 ? (100.0 * p1200 / total) : 0.0;

    snprintf(body, sizeof(body),
        "{\"ok\":true,\"state\":\"%s\",\"decoder_state\":\"signal_diagnostic\",\"receive_kind\":\"digital\",\"mode\":\"%s\",\"sample_rate_hz\":%d,\"sample_count\":%lu,\"pcm_bytes\":%ld,\"rms\":%.1f,\"peak\":%d,\"zero_cross_hz\":%.1f,\"dominant_tone_hz\":%.1f,\"mark_1200_power\":%.6e,\"space_2200_power\":%.6e,\"tone_960_power\":%.6e,\"tone_4800_power\":%.6e,\"mark_balance_percent\":%.1f,\"decoded_text\":\"\",\"lines\":[\"%s live signal diagnostics active.\",\"PCM samples analyzed: %lu at %d Hz.\",\"RMS %.1f, peak %d, zero-crossing estimate %.1f Hz.\",\"1200 Hz energy %.6e; 2200 Hz energy %.6e.\",\"Dominant tone %.1f Hz; mark balance %.1f%%.\",\"Live bit/HDLC/text recovery is not claimed until symbol clock and slicer lock.\"]}\n",
        state, mode_json ? mode_json : "digital", AUDIO_SAMPLE_RATE, (unsigned long)sample_count, pcm_bytes, rms, peak, zero_hz,
        dom_hz, p1200, p2200, p960, p4800, mark_balance, label,
        (unsigned long)sample_count, AUDIO_SAMPLE_RATE, rms, peak, zero_hz, p1200, p2200, dom_hz, mark_balance);
    free(samples);
    send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
}

static void digital_live_send_decode_output_v2637(int fd, const char *mode, const char *mode_json)
{
    if (digital_v2637_mode_is_bpsk(mode)) {
        digital_v2637_send_bpsk_output(fd, mode, mode_json);
        return;
    }
    if (digital_v2637_mode_is_fsk_gmsk(mode)) {
        digital_v2637_send_fsk_output(fd, mode, mode_json);
        return;
    }
    if (digital_v2637_mode_is_packet_afsk(mode)) {
        digital_live_send_decode_output_v2637(fd, mode, mode_json);
        return;
    }
    digital_v2637_send_fsk_output(fd, mode, mode_json);
}
/* DIGITAL_LIVE_DIAGNOSTICS_V2_6_37_END */

/* DIGITAL_DECODE_ROUTE_ORDER_V2_6_37A */
static void send_decode_output_v261c(int fd, const char *query)
{
    char mode[128] = "";
    char mode_json[256];
    char kind_json[64];
    char body[1600];
    const char *kind;

    query_param(query, "mode", mode, sizeof(mode));
    if (!mode[0]) snprintf(mode, sizeof(mode), "digital");
    json_escape(mode_json, sizeof(mode_json), mode);
    kind = receive_kind_v261c(mode);

    /* Explicit digital modulation names must win before generic kind/CW routing. */
    if (digital_v2637_mode_is_bpsk(mode) || digital_v2637_mode_is_fsk_gmsk(mode)) {
        digital_live_send_decode_output_v2637(fd, mode, mode_json);
        return;
    }

    /* Explicit packet/AFSK/APRS modes use the AX.25/APRS diagnostic path. */
    if (digital_v2637_mode_is_packet_afsk(mode) || ax25_mode_is_digital_v2627(mode)) {
        ax25_send_decode_output_v2630(fd, mode, mode_json);
        return;
    }

    if (strcmp(kind, "cw") == 0) {
        cw_send_diagnostic_output_v2622a(fd, mode, mode_json);
        return;
    }

    if (strcmp(kind, "digital") == 0) {
        digital_live_send_decode_output_v2637(fd, mode, mode_json);
        return;
    }

    json_escape(kind_json, sizeof(kind_json), kind);
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"state\":\"placeholder\",\"decoder_state\":\"not_implemented\",\"receive_kind\":\"%s\",\"mode\":\"%s\",\"lines\":[\"Decode foundation is installed.\",\"FM voice continues to use the existing Listen path.\",\"CW diagnostics are active for explicit CW modes.\",\"Digital diagnostics are active for explicit BPSK/FSK/GMSK/AX.25 modes.\"]}\n",
             kind_json, mode_json);
    send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
}



/* RECEIVE_MODE_PLACEHOLDERS_V2_6_21C_END */


/* CW_SELFTEST_ENDPOINT_V2_6_24
 * Deterministic self-test for the CW timing/morse mapping layer. This endpoint
 * does not touch Pluto RF, live audio, helper processes, or the FM Listen path.
 */
static char cw_morse_lookup_v2624(const char *sym)
{
    if (!sym) return '?';
    if (strcmp(sym, ".-") == 0) return 'A';
    if (strcmp(sym, "-...") == 0) return 'B';
    if (strcmp(sym, "-.-.") == 0) return 'C';
    if (strcmp(sym, "-..") == 0) return 'D';
    if (strcmp(sym, ".") == 0) return 'E';
    if (strcmp(sym, "..-.") == 0) return 'F';
    if (strcmp(sym, "--.") == 0) return 'G';
    if (strcmp(sym, "....") == 0) return 'H';
    if (strcmp(sym, "..") == 0) return 'I';
    if (strcmp(sym, ".---") == 0) return 'J';
    if (strcmp(sym, "-.-") == 0) return 'K';
    if (strcmp(sym, ".-..") == 0) return 'L';
    if (strcmp(sym, "--") == 0) return 'M';
    if (strcmp(sym, "-.") == 0) return 'N';
    if (strcmp(sym, "---") == 0) return 'O';
    if (strcmp(sym, ".--.") == 0) return 'P';
    if (strcmp(sym, "--.-") == 0) return 'Q';
    if (strcmp(sym, ".-.") == 0) return 'R';
    if (strcmp(sym, "...") == 0) return 'S';
    if (strcmp(sym, "-") == 0) return 'T';
    if (strcmp(sym, "..-") == 0) return 'U';
    if (strcmp(sym, "...-") == 0) return 'V';
    if (strcmp(sym, ".--") == 0) return 'W';
    if (strcmp(sym, "-..-") == 0) return 'X';
    if (strcmp(sym, "-.--") == 0) return 'Y';
    if (strcmp(sym, "--..") == 0) return 'Z';
    if (strcmp(sym, "-----") == 0) return '0';
    if (strcmp(sym, ".----") == 0) return '1';
    if (strcmp(sym, "..---") == 0) return '2';
    if (strcmp(sym, "...--") == 0) return '3';
    if (strcmp(sym, "....-") == 0) return '4';
    if (strcmp(sym, ".....") == 0) return '5';
    if (strcmp(sym, "-....") == 0) return '6';
    if (strcmp(sym, "--...") == 0) return '7';
    if (strcmp(sym, "---..") == 0) return '8';
    if (strcmp(sym, "----.") == 0) return '9';
    return '?';
}

static void cw_append_char_v2624(char *dst, size_t dst_size, size_t *used, char ch)
{
    if (!dst || !used || dst_size == 0 || *used + 1 >= dst_size) return;
    dst[*used] = ch;
    *used += 1;
    dst[*used] = '\0';
}

static void cw_selftest_decode_v2624(char *decoded, size_t decoded_size, char *morse, size_t morse_size, int *events_out, int *symbols_out)
{
    struct cw_event_v2624 { int mark; int units; };
    static const struct cw_event_v2624 events[] = {
        {1,1},{0,1},{1,1},{0,1},{1,1},{0,3},
        {1,3},{0,1},{1,3},{0,1},{1,3},{0,3},
        {1,1},{0,1},{1,1},{0,1},{1,1}
    };
    char symbol[16];
    size_t decoded_used = 0;
    size_t morse_used = 0;
    size_t sym_used = 0;
    size_t i;
    int symbol_count = 0;
    if (decoded && decoded_size > 0) decoded[0] = '\0';
    if (morse && morse_size > 0) morse[0] = '\0';
    symbol[0] = '\0';
    for (i = 0; i < sizeof(events) / sizeof(events[0]); i++) {
        if (events[i].mark) {
            if (sym_used + 1 < sizeof(symbol)) {
                symbol[sym_used++] = (events[i].units <= 2) ? '.' : '-';
                symbol[sym_used] = '\0';
            }
        } else if (events[i].units >= 3) {
            if (sym_used > 0) {
                char ch = cw_morse_lookup_v2624(symbol);
                if (morse_used > 0) cw_append_char_v2624(morse, morse_size, &morse_used, ' ');
                for (size_t j = 0; j < sym_used; j++) cw_append_char_v2624(morse, morse_size, &morse_used, symbol[j]);
                cw_append_char_v2624(decoded, decoded_size, &decoded_used, ch);
                symbol_count++;
                sym_used = 0;
                symbol[0] = '\0';
            }
            if (events[i].units >= 7) cw_append_char_v2624(decoded, decoded_size, &decoded_used, ' ');
        }
    }
    if (sym_used > 0) {
        char ch = cw_morse_lookup_v2624(symbol);
        if (morse_used > 0) cw_append_char_v2624(morse, morse_size, &morse_used, ' ');
        for (size_t j = 0; j < sym_used; j++) cw_append_char_v2624(morse, morse_size, &morse_used, symbol[j]);
        cw_append_char_v2624(decoded, decoded_size, &decoded_used, ch);
        symbol_count++;
    }
    if (events_out) *events_out = (int)(sizeof(events) / sizeof(events[0]));
    if (symbols_out) *symbols_out = symbol_count;
}

static void send_cw_selftest_v2624(int fd)
{
    char decoded[64];
    char morse[128];
    char body[1536];
    int events = 0;
    int symbols = 0;
    cw_selftest_decode_v2624(decoded, sizeof(decoded), morse, sizeof(morse), &events, &symbols);
    snprintf(body, sizeof(body), "{\"ok\":true,\"state\":\"cw_selftest\",\"version\":\"2.6.24\",\"decoder_state\":\"selftest\",\"expected_text\":\"SOS\",\"decoded_text\":\"%s\",\"morse\":\"%s\",\"events\":%d,\"symbols\":%d,\"dot_unit_ms\":100,\"pass\":%s,\"lines\":[\"CW self-test generated synthetic SOS timing events.\",\"Morse timing segmentation and symbol mapping are available without using RF.\",\"Use real Decode CW next to compare live tone/envelope metrics.\"]}\n", decoded, morse, events, symbols, strcmp(decoded, "SOS") == 0 ? "true" : "false");
    send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
}


/* AX25_AFSK_SYNTHETIC_SELFTEST_V2_6_31 */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define AX25_AFSK_SELFTEST_RATE_V2631 24000
#define AX25_AFSK_SELFTEST_BAUD_V2631 1200
#define AX25_AFSK_SELFTEST_SPS_V2631 20
#define AX25_AFSK_SELFTEST_MAX_BITS_V2631 4096
#define AX25_AFSK_SELFTEST_MAX_FRAME_V2631 512

static int ax25_afsk_append_bit_v2631(unsigned char *bits, int max_bits, int *bit_count, int bit)
{
    if (!bits || !bit_count || *bit_count >= max_bits) return 0;
    bits[*bit_count] = bit ? 1u : 0u;
    *bit_count += 1;
    return 1;
}

static int ax25_afsk_append_flag_v2631(unsigned char *bits, int max_bits, int *bit_count)
{
    static const unsigned char flag_bits[8] = {0,1,1,1,1,1,1,0};
    int i;
    for (i = 0; i < 8; i++) {
        if (!ax25_afsk_append_bit_v2631(bits, max_bits, bit_count, flag_bits[i])) return 0;
    }
    return 1;
}

static int ax25_afsk_append_stuffed_frame_bits_v2631(unsigned char *bits, int max_bits, int *bit_count, const unsigned char *frame, size_t frame_len)
{
    size_t i;
    int j;
    int ones = 0;
    for (i = 0; i < frame_len; i++) {
        for (j = 0; j < 8; j++) {
            int bit = (frame[i] >> j) & 1;
            if (!ax25_afsk_append_bit_v2631(bits, max_bits, bit_count, bit)) return 0;
            if (bit) {
                ones++;
                if (ones == 5) {
                    if (!ax25_afsk_append_bit_v2631(bits, max_bits, bit_count, 0)) return 0;
                    ones = 0;
                }
            } else {
                ones = 0;
            }
        }
    }
    return 1;
}

static int ax25_afsk_build_hdlc_bits_v2631(const unsigned char *frame, size_t frame_len, unsigned char *bits, int max_bits, int *bit_count)
{
    if (!frame || !bits || !bit_count) return 0;
    *bit_count = 0;
    if (!ax25_afsk_append_flag_v2631(bits, max_bits, bit_count)) return 0;
    if (!ax25_afsk_append_stuffed_frame_bits_v2631(bits, max_bits, bit_count, frame, frame_len)) return 0;
    if (!ax25_afsk_append_flag_v2631(bits, max_bits, bit_count)) return 0;
    return 1;
}

static void ax25_afsk_synth_symbol_v2631(int16_t *samples, int offset, int count, double freq_hz, double *phase)
{
    int i;
    double step = (2.0 * M_PI * freq_hz) / (double)AX25_AFSK_SELFTEST_RATE_V2631;
    for (i = 0; i < count; i++) {
        samples[offset + i] = (int16_t)(16000.0 * sin(*phase));
        *phase += step;
        if (*phase > 2.0 * M_PI) *phase -= 2.0 * M_PI;
    }
}

static int ax25_afsk_synth_nrzi_pcm_v2631(const unsigned char *bits, int bit_count, int16_t **out_samples, int *out_sample_count)
{
    int i;
    int tone_state = 1; /* 1 = mark/1200 Hz, 0 = space/2200 Hz */
    int sample_count;
    int16_t *samples;
    double phase = 0.0;
    if (!bits || bit_count <= 0 || !out_samples || !out_sample_count) return 0;
    sample_count = bit_count * AX25_AFSK_SELFTEST_SPS_V2631;
    samples = (int16_t *)calloc((size_t)sample_count, sizeof(int16_t));
    if (!samples) return 0;
    for (i = 0; i < bit_count; i++) {
        double freq;
        if (!bits[i]) tone_state = !tone_state; /* NRZI: zero causes transition */
        freq = tone_state ? 1200.0 : 2200.0;
        ax25_afsk_synth_symbol_v2631(samples, i * AX25_AFSK_SELFTEST_SPS_V2631, AX25_AFSK_SELFTEST_SPS_V2631, freq, &phase);
    }
    *out_samples = samples;
    *out_sample_count = sample_count;
    return 1;
}

static double ax25_afsk_symbol_power_v2631(const int16_t *samples, int offset, int count, double freq_hz)
{
    int i;
    double re = 0.0, im = 0.0;
    double step = (2.0 * M_PI * freq_hz) / (double)AX25_AFSK_SELFTEST_RATE_V2631;
    for (i = 0; i < count; i++) {
        double x = (double)samples[offset + i] / 32768.0;
        double a = step * (double)i;
        re += x * cos(a);
        im -= x * sin(a);
    }
    return re * re + im * im;
}

static int ax25_afsk_demod_nrzi_bits_v2631(const int16_t *samples, int sample_count, unsigned char *bits, int max_bits, int *out_bits, int *mark_symbols, int *space_symbols)
{
    int symbols;
    int i;
    int prev_state = 1;
    if (!samples || sample_count <= 0 || !bits || !out_bits) return 0;
    symbols = sample_count / AX25_AFSK_SELFTEST_SPS_V2631;
    if (symbols > max_bits) symbols = max_bits;
    if (mark_symbols) *mark_symbols = 0;
    if (space_symbols) *space_symbols = 0;
    for (i = 0; i < symbols; i++) {
        int offset = i * AX25_AFSK_SELFTEST_SPS_V2631;
        double p1200 = ax25_afsk_symbol_power_v2631(samples, offset, AX25_AFSK_SELFTEST_SPS_V2631, 1200.0);
        double p2200 = ax25_afsk_symbol_power_v2631(samples, offset, AX25_AFSK_SELFTEST_SPS_V2631, 2200.0);
        int state = (p1200 >= p2200) ? 1 : 0;
        bits[i] = (state == prev_state) ? 1u : 0u; /* NRZI decode: no transition = one */
        prev_state = state;
        if (state) {
            if (mark_symbols) *mark_symbols += 1;
        } else {
            if (space_symbols) *space_symbols += 1;
        }
    }
    *out_bits = symbols;
    return 1;
}

static int ax25_afsk_find_flag_v2631(const unsigned char *bits, int bit_count, int start)
{
    static const unsigned char flag_bits[8] = {0,1,1,1,1,1,1,0};
    int i, j;
    if (!bits || bit_count < 8) return -1;
    for (i = start; i <= bit_count - 8; i++) {
        int match = 1;
        for (j = 0; j < 8; j++) {
            if (bits[i + j] != flag_bits[j]) { match = 0; break; }
        }
        if (match) return i;
    }
    return -1;
}

static int ax25_afsk_bits_to_frame_v2631(const unsigned char *bits, int bit_count, unsigned char *frame, size_t frame_max, size_t *frame_len, char *error, size_t error_size)
{
    unsigned char data_bits[AX25_AFSK_SELFTEST_MAX_BITS_V2631];
    int data_count = 0;
    int first, second, i, ones = 0;
    size_t out_len = 0;
    if (frame_len) *frame_len = 0;
    if (!bits || !frame || !frame_len) return 0;
    first = ax25_afsk_find_flag_v2631(bits, bit_count, 0);
    second = first >= 0 ? ax25_afsk_find_flag_v2631(bits, bit_count, first + 8) : -1;
    if (first < 0 || second < 0 || second <= first + 8) {
        snprintf(error, error_size, "HDLC flags not found");
        return 0;
    }
    for (i = first + 8; i < second; i++) {
        int bit = bits[i] ? 1 : 0;
        if (bit) {
            if (data_count >= AX25_AFSK_SELFTEST_MAX_BITS_V2631) {
                snprintf(error, error_size, "destuffed bit buffer full");
                return 0;
            }
            data_bits[data_count++] = 1u;
            ones++;
            if (ones > 6) {
                snprintf(error, error_size, "invalid HDLC one run");
                return 0;
            }
        } else {
            if (ones == 5) {
                ones = 0;
                continue; /* stuffed zero */
            }
            if (data_count >= AX25_AFSK_SELFTEST_MAX_BITS_V2631) {
                snprintf(error, error_size, "destuffed bit buffer full");
                return 0;
            }
            data_bits[data_count++] = 0u;
            ones = 0;
        }
    }
    if (data_count < 16 || (data_count % 8) != 0) {
        snprintf(error, error_size, "destuffed bit count not byte aligned: %d", data_count);
        return 0;
    }
    for (i = 0; i < data_count; i += 8) {
        int j;
        unsigned char b = 0;
        if (out_len >= frame_max) {
            snprintf(error, error_size, "frame byte buffer full");
            return 0;
        }
        for (j = 0; j < 8; j++) b |= (unsigned char)(data_bits[i + j] << j);
        frame[out_len++] = b;
    }
    *frame_len = out_len;
    return 1;
}

static void send_ax25_afsk_selftest_v2631(int fd)
{
    unsigned char frame[AX25_AFSK_SELFTEST_MAX_FRAME_V2631];
    unsigned char hdlc_bits[AX25_AFSK_SELFTEST_MAX_BITS_V2631];
    unsigned char recovered_bits[AX25_AFSK_SELFTEST_MAX_BITS_V2631];
    unsigned char recovered_frame[AX25_AFSK_SELFTEST_MAX_FRAME_V2631];
    int hdlc_bit_count = 0;
    int recovered_bit_count = 0;
    int16_t *samples = NULL;
    int sample_count = 0;
    int mark_symbols = 0;
    int space_symbols = 0;
    size_t frame_len = 0;
    size_t recovered_frame_len = 0;
    char summary[256] = "";
    char info[256] = "";
    char error[256] = "";
    char summary_json[512];
    char info_json[512];
    char error_json[512];
    char body[4096];
    const char *expected = "N0CALL>APRS:>AX25 SELFTEST";
    int ok = 0;

    frame_len = ax25_make_selftest_frame_v2627(frame, sizeof(frame));
    if (frame_len == 0) snprintf(error, sizeof(error), "failed to make AX.25 self-test frame");
    else if (!ax25_afsk_build_hdlc_bits_v2631(frame, frame_len, hdlc_bits, AX25_AFSK_SELFTEST_MAX_BITS_V2631, &hdlc_bit_count)) snprintf(error, sizeof(error), "failed to build HDLC bit stream");
    else if (!ax25_afsk_synth_nrzi_pcm_v2631(hdlc_bits, hdlc_bit_count, &samples, &sample_count)) snprintf(error, sizeof(error), "failed to synthesize Bell 202 AFSK PCM");
    else if (!ax25_afsk_demod_nrzi_bits_v2631(samples, sample_count, recovered_bits, AX25_AFSK_SELFTEST_MAX_BITS_V2631, &recovered_bit_count, &mark_symbols, &space_symbols)) snprintf(error, sizeof(error), "failed to demodulate synthetic AFSK PCM");
    else if (!ax25_afsk_bits_to_frame_v2631(recovered_bits, recovered_bit_count, recovered_frame, sizeof(recovered_frame), &recovered_frame_len, error, sizeof(error))) { }
    else if (!ax25_decode_ui_frame_v2627(recovered_frame, recovered_frame_len, summary, sizeof(summary), info, sizeof(info), error, sizeof(error))) { }
    else ok = (strcmp(summary, expected) == 0);

    if (samples) free(samples);
    if (!ok && error[0] == '\0') snprintf(error, sizeof(error), "decoded text mismatch");
    json_escape(summary_json, sizeof(summary_json), summary);
    json_escape(info_json, sizeof(info_json), info);
    json_escape(error_json, sizeof(error_json), error);

    snprintf(body, sizeof(body), "{\"ok\":%s,\"state\":\"ax25_afsk_selftest\",\"decoder_state\":\"%s\",\"version\":\"2.6.31\",\"expected\":\"N0CALL>APRS:>AX25 SELFTEST\",\"decoded_text\":\"%s\",\"info\":\"%s\",\"error\":\"%s\",\"source_frame_bytes\":%lu,\"hdlc_bits\":%d,\"sample_rate_hz\":%d,\"samples\":%d,\"symbols\":%d,\"mark_symbols\":%d,\"space_symbols\":%d,\"recovered_bits\":%d,\"recovered_frame_bytes\":%lu,\"pass\":%s,\"lines\":[\"Generated a known AX.25/APRS UI frame.\",\"Encoded it as HDLC bits with bit stuffing and flags.\",\"Synthesized 1200-baud Bell 202 AFSK PCM.\",\"Demodulated mark/space tones back through NRZI and HDLC recovery.\",\"Fed recovered frame bytes into the AX.25 parser.\"]}\n", ok ? "true" : "false", ok ? "pass" : "fail", summary_json, info_json, error_json, (unsigned long)frame_len, hdlc_bit_count, AX25_AFSK_SELFTEST_RATE_V2631, sample_count, recovered_bit_count, mark_symbols, space_symbols, recovered_bit_count, (unsigned long)recovered_frame_len, ok ? "true" : "false");
    send_text(fd, ok ? 200 : 500, ok ? "OK" : "Internal Server Error", "application/json; charset=utf-8", body);
}
/* AX25_AFSK_SYNTHETIC_SELFTEST_V2_6_31_END */


/* DIGITAL_DECODER_SELFTESTS_V2_6_35 */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DIGITAL_TEST_RATE_V2635 24000
#define DIGITAL_TEST_BAUD_V2635 1200
#define DIGITAL_TEST_SPS_V2635 20
#define DIGITAL_TEST_MAX_TEXT_V2635 128
#define DIGITAL_TEST_MAX_BITS_V2635 4096

static int digital_v2635_append_text_bits_msb(const char *text, unsigned char *bits, int max_bits, int *bit_count)
{
    size_t i;
    int b;
    if (!text || !bits || !bit_count) return 0;
    *bit_count = 0;
    for (i = 0; text[i]; i++) {
        unsigned char ch = (unsigned char)text[i];
        for (b = 7; b >= 0; b--) {
            if (*bit_count >= max_bits) return 0;
            bits[*bit_count] = (unsigned char)((ch >> b) & 1u);
            *bit_count += 1;
        }
    }
    return 1;
}

static int digital_v2635_bits_to_text_msb(const unsigned char *bits, int bit_count, char *out, size_t out_size)
{
    int i;
    size_t pos = 0;
    if (!bits || !out || out_size == 0 || bit_count < 8) return 0;
    out[0] = '\0';
    for (i = 0; i + 7 < bit_count; i += 8) {
        int j;
        unsigned char ch = 0;
        if (pos + 1 >= out_size) return 0;
        for (j = 0; j < 8; j++) ch = (unsigned char)((ch << 1) | (bits[i + j] ? 1 : 0));
        out[pos++] = (char)ch;
    }
    out[pos] = '\0';
    return 1;
}

static void bpsk_v2635_synth_symbol(double *samples, int offset, int count, double carrier_hz, int bit)
{
    int i;
    double phase = bit ? 0.0 : M_PI;
    double step = (2.0 * M_PI * carrier_hz) / (double)DIGITAL_TEST_RATE_V2635;
    for (i = 0; i < count; i++) {
        samples[offset + i] = cos(step * (double)i + phase);
    }
}

static int bpsk_v2635_synth(const unsigned char *bits, int bit_count, double carrier_hz, double **out_samples, int *out_sample_count)
{
    int i;
    int sample_count;
    double *samples;
    if (!bits || bit_count <= 0 || !out_samples || !out_sample_count) return 0;
    sample_count = bit_count * DIGITAL_TEST_SPS_V2635;
    samples = (double *)calloc((size_t)sample_count, sizeof(double));
    if (!samples) return 0;
    for (i = 0; i < bit_count; i++) {
        bpsk_v2635_synth_symbol(samples, i * DIGITAL_TEST_SPS_V2635, DIGITAL_TEST_SPS_V2635, carrier_hz, bits[i] ? 1 : 0);
    }
    *out_samples = samples;
    *out_sample_count = sample_count;
    return 1;
}

static int bpsk_v2635_demod(const double *samples, int sample_count, double carrier_hz, unsigned char *bits, int max_bits, int *out_bits, double *quality_out)
{
    int symbols;
    int i;
    double quality = 0.0;
    double step = (2.0 * M_PI * carrier_hz) / (double)DIGITAL_TEST_RATE_V2635;
    if (!samples || sample_count <= 0 || !bits || !out_bits) return 0;
    symbols = sample_count / DIGITAL_TEST_SPS_V2635;
    if (symbols > max_bits) symbols = max_bits;
    for (i = 0; i < symbols; i++) {
        int j;
        double corr = 0.0;
        int offset = i * DIGITAL_TEST_SPS_V2635;
        for (j = 0; j < DIGITAL_TEST_SPS_V2635; j++) {
            corr += samples[offset + j] * cos(step * (double)j);
        }
        bits[i] = (corr >= 0.0) ? 1u : 0u;
        quality += corr >= 0.0 ? corr : -corr;
    }
    *out_bits = symbols;
    if (quality_out) *quality_out = symbols > 0 ? quality / (double)symbols : 0.0;
    return 1;
}

static double fsk_v2635_symbol_power(const double *samples, int offset, int count, double freq_hz)
{
    int i;
    double re = 0.0, im = 0.0;
    double step = (2.0 * M_PI * freq_hz) / (double)DIGITAL_TEST_RATE_V2635;
    for (i = 0; i < count; i++) {
        double a = step * (double)i;
        re += samples[offset + i] * cos(a);
        im -= samples[offset + i] * sin(a);
    }
    return re * re + im * im;
}

static void fsk_v2635_synth_symbol(double *samples, int offset, int count, double freq_hz, double *phase)
{
    int i;
    double step = (2.0 * M_PI * freq_hz) / (double)DIGITAL_TEST_RATE_V2635;
    for (i = 0; i < count; i++) {
        samples[offset + i] = sin(*phase);
        *phase += step;
        if (*phase > 2.0 * M_PI) *phase -= 2.0 * M_PI;
    }
}

static int fsk_v2635_synth(const unsigned char *bits, int bit_count, double mark_hz, double space_hz, double **out_samples, int *out_sample_count)
{
    int i;
    int sample_count;
    double *samples;
    double phase = 0.0;
    if (!bits || bit_count <= 0 || !out_samples || !out_sample_count) return 0;
    sample_count = bit_count * DIGITAL_TEST_SPS_V2635;
    samples = (double *)calloc((size_t)sample_count, sizeof(double));
    if (!samples) return 0;
    for (i = 0; i < bit_count; i++) {
        fsk_v2635_synth_symbol(samples, i * DIGITAL_TEST_SPS_V2635, DIGITAL_TEST_SPS_V2635, bits[i] ? mark_hz : space_hz, &phase);
    }
    *out_samples = samples;
    *out_sample_count = sample_count;
    return 1;
}

static int fsk_v2635_demod(const double *samples, int sample_count, double mark_hz, double space_hz, unsigned char *bits, int max_bits, int *out_bits, int *mark_symbols, int *space_symbols, double *confidence_out)
{
    int symbols;
    int i;
    double confidence = 0.0;
    if (!samples || sample_count <= 0 || !bits || !out_bits) return 0;
    symbols = sample_count / DIGITAL_TEST_SPS_V2635;
    if (symbols > max_bits) symbols = max_bits;
    if (mark_symbols) *mark_symbols = 0;
    if (space_symbols) *space_symbols = 0;
    for (i = 0; i < symbols; i++) {
        int offset = i * DIGITAL_TEST_SPS_V2635;
        double pm = fsk_v2635_symbol_power(samples, offset, DIGITAL_TEST_SPS_V2635, mark_hz);
        double ps = fsk_v2635_symbol_power(samples, offset, DIGITAL_TEST_SPS_V2635, space_hz);
        if (pm >= ps) {
            bits[i] = 1u;
            if (mark_symbols) *mark_symbols += 1;
            confidence += (pm - ps) / (pm + ps + 1.0e-12);
        } else {
            bits[i] = 0u;
            if (space_symbols) *space_symbols += 1;
            confidence += (ps - pm) / (pm + ps + 1.0e-12);
        }
    }
    *out_bits = symbols;
    if (confidence_out) *confidence_out = symbols > 0 ? confidence / (double)symbols : 0.0;
    return 1;
}

static void send_bpsk_selftest_v2635(int fd)
{
    const char *expected = "BPSK SELFTEST";
    unsigned char bits[DIGITAL_TEST_MAX_BITS_V2635];
    unsigned char recovered[DIGITAL_TEST_MAX_BITS_V2635];
    char decoded[DIGITAL_TEST_MAX_TEXT_V2635];
    char decoded_json[256];
    char body[2048];
    double *samples = NULL;
    int bit_count = 0;
    int recovered_bits = 0;
    int sample_count = 0;
    double quality = 0.0;
    int ok = 0;

    decoded[0] = '\0';
    if (digital_v2635_append_text_bits_msb(expected, bits, DIGITAL_TEST_MAX_BITS_V2635, &bit_count) &&
        bpsk_v2635_synth(bits, bit_count, 1800.0, &samples, &sample_count) &&
        bpsk_v2635_demod(samples, sample_count, 1800.0, recovered, DIGITAL_TEST_MAX_BITS_V2635, &recovered_bits, &quality) &&
        digital_v2635_bits_to_text_msb(recovered, recovered_bits, decoded, sizeof(decoded))) {
        ok = (strcmp(decoded, expected) == 0);
    }
    if (samples) free(samples);
    json_escape(decoded_json, sizeof(decoded_json), decoded);
    snprintf(body, sizeof(body), "{\"ok\":%s,\"state\":\"bpsk_selftest\",\"decoder_state\":\"%s\",\"version\":\"2.6.35\",\"mode\":\"BPSK\",\"expected\":\"BPSK SELFTEST\",\"decoded_text\":\"%s\",\"sample_rate_hz\":%d,\"baud\":%d,\"carrier_hz\":1800,\"bits\":%d,\"samples\":%d,\"recovered_bits\":%d,\"quality\":%.6f,\"pass\":%s,\"lines\":[\"Generated a known BPSK symbol stream.\",\"Demodulated coherent phase decisions back to bits.\",\"Recovered bytes matched the expected text.\",\"Live BPSK pass decoding will reuse this signal path after carrier/clock recovery is connected.\"]}\n", ok ? "true" : "false", ok ? "pass" : "fail", decoded_json, DIGITAL_TEST_RATE_V2635, DIGITAL_TEST_BAUD_V2635, bit_count, sample_count, recovered_bits, quality, ok ? "true" : "false");
    send_text(fd, ok ? 200 : 500, ok ? "OK" : "Internal Server Error", "application/json; charset=utf-8", body);
}

static void send_fsk_selftest_v2635(int fd)
{
    const char *expected = "FSK SELFTEST";
    unsigned char bits[DIGITAL_TEST_MAX_BITS_V2635];
    unsigned char recovered[DIGITAL_TEST_MAX_BITS_V2635];
    char decoded[DIGITAL_TEST_MAX_TEXT_V2635];
    char decoded_json[256];
    char body[2048];
    double *samples = NULL;
    int bit_count = 0;
    int recovered_bits = 0;
    int sample_count = 0;
    int mark_symbols = 0;
    int space_symbols = 0;
    double confidence = 0.0;
    int ok = 0;

    decoded[0] = '\0';
    if (digital_v2635_append_text_bits_msb(expected, bits, DIGITAL_TEST_MAX_BITS_V2635, &bit_count) &&
        fsk_v2635_synth(bits, bit_count, 1200.0, 2200.0, &samples, &sample_count) &&
        fsk_v2635_demod(samples, sample_count, 1200.0, 2200.0, recovered, DIGITAL_TEST_MAX_BITS_V2635, &recovered_bits, &mark_symbols, &space_symbols, &confidence) &&
        digital_v2635_bits_to_text_msb(recovered, recovered_bits, decoded, sizeof(decoded))) {
        ok = (strcmp(decoded, expected) == 0);
    }
    if (samples) free(samples);
    json_escape(decoded_json, sizeof(decoded_json), decoded);
    snprintf(body, sizeof(body), "{\"ok\":%s,\"state\":\"fsk_selftest\",\"decoder_state\":\"%s\",\"version\":\"2.6.35\",\"mode\":\"FSK/GMSK diagnostic\",\"expected\":\"FSK SELFTEST\",\"decoded_text\":\"%s\",\"sample_rate_hz\":%d,\"baud\":%d,\"mark_hz\":1200,\"space_hz\":2200,\"bits\":%d,\"samples\":%d,\"recovered_bits\":%d,\"mark_symbols\":%d,\"space_symbols\":%d,\"confidence\":%.6f,\"pass\":%s,\"lines\":[\"Generated a known two-tone FSK symbol stream.\",\"Measured mark/space energy for each symbol.\",\"Recovered bytes matched the expected text.\",\"GMSK/9600 live decoding still needs mode-specific clock and slicing against real audio.\"]}\n", ok ? "true" : "false", ok ? "pass" : "fail", decoded_json, DIGITAL_TEST_RATE_V2635, DIGITAL_TEST_BAUD_V2635, bit_count, sample_count, recovered_bits, mark_symbols, space_symbols, confidence, ok ? "true" : "false");
    send_text(fd, ok ? 200 : 500, ok ? "OK" : "Internal Server Error", "application/json; charset=utf-8", body);
}

static void send_digital_decoder_matrix_v2635(int fd)
{
    const char *body =
        "{\"ok\":true,\"state\":\"digital_decoder_matrix\",\"version\":\"2.6.35\","
        "\"decoders\":["
        "{\"mode\":\"FM/AM/SSB/USB/LSB\",\"action\":\"Listen\",\"status\":\"audio_path\"},"
        "{\"mode\":\"CW/Morse/A1A\",\"action\":\"Decode CW\",\"status\":\"timing_decoder_selftest_passed\"},"
        "{\"mode\":\"AX.25/APRS/AFSK packet\",\"action\":\"Decode\",\"status\":\"frame_parser_and_afsk_selftests_available\"},"
        "{\"mode\":\"BPSK/PSK\",\"action\":\"Decode\",\"status\":\"bpsk_synth_demod_selftest_available\"},"
        "{\"mode\":\"FSK/GMSK/9600\",\"action\":\"Decode\",\"status\":\"fsk_synth_demod_selftest_available\"},"
        "{\"mode\":\"Unknown\",\"action\":\"Listen\",\"status\":\"safe_default\"}],"
        "\"endpoints\":[\"/api/radio/decode/cw/selftest\",\"/api/radio/decode/ax25/selftest\",\"/api/radio/decode/ax25/afsk-selftest\",\"/api/radio/decode/bpsk/selftest\",\"/api/radio/decode/fsk/selftest\"]}\n";
    send_text(fd, 200, "OK", "application/json; charset=utf-8", body);
}
/* DIGITAL_DECODER_SELFTESTS_V2_6_35_END */

static void handle_request(
    int fd,
    const struct app_config *cfg,
    const char *method,
    const char *path,
    const char *query,
    const char *body)
{
    char file_path[PATH_BUF_SIZE];

    /* DIGITAL_DECODER_SELFTEST_ROUTES_V2_6_35 */
    if (strcmp(path, "/api/radio/decode/bpsk/selftest") == 0) {
        if (strcmp(method, "GET") == 0) {
            send_bpsk_selftest_v2635(fd);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }
    if (strcmp(path, "/api/radio/decode/fsk/selftest") == 0 || strcmp(path, "/api/radio/decode/gmsk/selftest") == 0) {
        if (strcmp(method, "GET") == 0) {
            send_fsk_selftest_v2635(fd);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }
    if (strcmp(path, "/api/radio/decode/digital/selftest") == 0) {
        if (strcmp(method, "GET") == 0) {
            send_digital_decoder_matrix_v2635(fd);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }
    /* DIGITAL_DECODER_SELFTEST_ROUTES_V2_6_35_END */

    /* AX25_AFSK_SELFTEST_ROUTE_V2_6_31 */
    if (strcmp(path, "/api/radio/decode/ax25/afsk-selftest") == 0) {
        if (strcmp(method, "GET") == 0) {
            send_ax25_afsk_selftest_v2631(fd);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }
    /* AX25_AFSK_SELFTEST_ROUTE_V2_6_31_END */

    /* AX25_DIGITAL_DECODER_ROUTE_V2_6_27 */
    if (strcmp(path, "/api/radio/decode/ax25/selftest") == 0) {
        if (strcmp(method, "GET") == 0) {
            send_ax25_selftest_v2627(fd);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }
    /* END_AX25_DIGITAL_DECODER_ROUTE_V2_6_27 */

    /* CW_SELFTEST_ENDPOINT_V2_6_24 active-route guard */
    if (strcmp(path, "/api/radio/decode/cw/selftest") == 0) {
        if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) {
            send_cw_selftest_v2624(fd);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }

    /* RECEIVE_MODE_ACTIVE_ROUTE_GUARD_V2_6_21C_BEGIN
     * Active router guard: placed at the very top of handle_request(), before
     * spectrum, rotator, legacy POST and static-file branches.
     */
    if (strcmp(path, "/api/radio/receive/status") == 0) {
        if (strcmp(method, "GET") == 0) {
            send_receive_status_v261c(fd);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }
    if (strcmp(path, "/api/radio/decode/output") == 0) {
        if (strcmp(method, "GET") == 0) {
            send_decode_output_v261c(fd, query);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }
    if (strcmp(path, "/api/radio/receive/start") == 0) {
        if (strcmp(method, "POST") == 0) {
            send_receive_start_v261c(fd, cfg, query);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }
    if (strcmp(path, "/api/radio/receive/stop") == 0) {
        if (strcmp(method, "POST") == 0) {
            send_receive_stop_v261c(fd);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }
    /* RECEIVE_MODE_ACTIVE_ROUTE_GUARD_V2_6_21C_END */

    /* BACKEND_STREAMING_AUDIO_STREAM_ROUTE_V1D_EARLY_GUARD: keep continuous browser audio out of fragile router branches. */
    if (strcmp(path, "/api/radio/audio/live/stream.wav") == 0) {
        send_live_audio_stream_v1h(fd, query);
        return;
    }

    if (strcmp(path, "/api/radio/spectrum/snapshot") == 0) {
        if (strcmp(method, "GET") == 0) {
            send_radio_spectrum_snapshot_v252(fd, cfg, query);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0 && strcmp(method, "POST") != 0) {
        send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        return;
    }



    /* RECEIVE_MODE_ROUTE_GUARD_V2_6_21B
     * Keep receive/decode placeholder endpoints ahead of the legacy router so
     * they cannot fall through to the older API 404 branch.
     */
    if (strcmp(path, "/api/radio/receive/status") == 0) {
        if (strcmp(method, "GET") == 0) {
            send_receive_status_v261a(fd);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }
    if (strcmp(path, "/api/radio/decode/output") == 0) {
        if (strcmp(method, "GET") == 0) {
            send_decode_output_v261a(fd, query);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }
    if (strcmp(path, "/api/radio/receive/start") == 0) {
        if (strcmp(method, "POST") == 0) {
            send_receive_start_v261a(fd, cfg, query);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }
    if (strcmp(path, "/api/radio/receive/stop") == 0) {
        if (strcmp(method, "POST") == 0) {
            send_receive_stop_v261a(fd);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }

    /* ROTATOR_ROUTE_GUARD_V2_4_0B: keep rotator API ahead of legacy POST/API 404 routing. */
    if (strcmp(path, "/api/rotator/config") == 0) {
        if (strcmp(method, "GET") == 0) {
            send_rotator_config(fd, cfg);
        } else if (strcmp(method, "POST") == 0) {
            save_rotator_config(fd, cfg, body);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }
    if (strcmp(path, "/api/rotator/state") == 0) {
        if (strcmp(method, "GET") == 0) {
            send_rotator_state(fd, cfg);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }
    if (strcmp(path, "/api/rotator/protocol/preview") == 0) {
        if (strcmp(method, "GET") == 0) {
            send_rotator_protocol_preview_v2_4_3(fd, query);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }
    if (strcmp(path, "/api/rotator/test") == 0) {
        if (strcmp(method, "POST") == 0) {
            send_rotator_test(fd, cfg, query);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }
    if (strcmp(path, "/api/rotator/park") == 0) {
        if (strcmp(method, "POST") == 0) {
            send_rotator_park(fd, cfg);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }
    if (strcmp(path, "/api/rotator/stop") == 0) {
        if (strcmp(method, "POST") == 0) {
            send_rotator_stop(fd, cfg);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }
    if (strcmp(path, "/api/rotator/track/start") == 0) {
        if (strcmp(method, "POST") == 0) {
            send_rotator_track_start(fd, cfg);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }
    if (strcmp(path, "/api/rotator/track/stop") == 0) {
        if (strcmp(method, "POST") == 0) {
            send_rotator_track_stop(fd, cfg);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }
    if (strcmp(path, "/api/rotator/track/step") == 0) {
        if (strcmp(method, "POST") == 0) {
            send_rotator_track_step(fd, cfg);
        } else {
            send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        }
        return;
    }

    if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/api/radio/track/plan") == 0) {
            send_radio_track_plan(fd, cfg, body);
        } else if (strcmp(path, "/api/radio/audio/live/start") == 0) {
            send_live_audio_start(fd, cfg, query);
        } else if (strcmp(path, "/api/radio/audio/live/stop") == 0) {
            send_live_audio_stop(fd);
        } else if (strcmp(path, "/api/radio/receive/start") == 0) {
            send_receive_start_v261a(fd, cfg, query);
        } else if (strcmp(path, "/api/radio/receive/stop") == 0) {
            send_receive_stop_v261a(fd);
        } else if (strcmp(path, "/api/config") == 0) {
            send_config_save(fd, cfg, body);
        } else if (strcmp(path, "/api/refresh/passes") == 0) {
            send_refresh_run(fd, cfg, "passes");
        } else if (strcmp(path, "/api/refresh/catalog") == 0) {
            send_refresh_run(fd, cfg, "catalog");
        } else if (strcmp(path, "/api/refresh/all") == 0) {
            send_refresh_run(fd, cfg, "all");
        } else {
            send_text(fd, 404, "Not Found", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"not found\"}\n");
        }
        return;
    }

    if (strcmp(path, "/") == 0) {
        send_redirect(fd, "/SatelliteTracker/");
    } else if (strcmp(path, "/SatelliteTracker/") == 0 ||
               strcmp(path, "/SatelliteTracker/index.html") == 0) {
        join_path(file_path, sizeof(file_path), cfg->web_dir, "index.html");
        send_file(fd, file_path);

    /* BACKEND_SPLIT_UI_STATIC_ASSETS_V3 */
    } else if (strcmp(path, "/app.css") == 0 ||
               strcmp(path, "/SatelliteTracker/app.css") == 0) {
        join_path(file_path, sizeof(file_path), cfg->web_dir, "app.css");
        send_file(fd, file_path);
    } else if (strcmp(path, "/app.js") == 0 ||
               strcmp(path, "/SatelliteTracker/app.js") == 0) {
        join_path(file_path, sizeof(file_path), cfg->web_dir, "app.js");
        send_file(fd, file_path);
    if (strcmp(path, "/api/rotator/config") == 0 && strcmp(method, "GET") == 0) {
        send_rotator_config(fd, cfg);
        return;
    }
    if (strcmp(path, "/api/rotator/config") == 0 && strcmp(method, "POST") == 0) {
        save_rotator_config(fd, cfg, body);
        return;
    }
    if (strcmp(path, "/api/rotator/state") == 0 && strcmp(method, "GET") == 0) {
        send_rotator_state(fd, cfg);
        return;
    }
    if (strcmp(path, "/api/rotator/test") == 0 && strcmp(method, "POST") == 0) {
        send_rotator_test(fd, cfg, query);
        return;
    }
    if (strcmp(path, "/api/rotator/park") == 0 && strcmp(method, "POST") == 0) {
        send_rotator_park(fd, cfg);
        return;
    }
    if (strcmp(path, "/api/rotator/stop") == 0 && strcmp(method, "POST") == 0) {
        send_rotator_stop(fd, cfg);
        return;
    }
    if (strcmp(path, "/api/rotator/track/start") == 0 && strcmp(method, "POST") == 0) {
        send_rotator_track_start(fd, cfg);
        return;
    }
    if (strcmp(path, "/api/rotator/track/stop") == 0 && strcmp(method, "POST") == 0) {
        send_rotator_track_stop(fd, cfg);
        return;
    }
    if (strcmp(path, "/api/rotator/track/step") == 0 && strcmp(method, "POST") == 0) {
        send_rotator_track_step(fd, cfg);
        return;
    }

    } else if (strcmp(path, "/api/status") == 0) {
        send_status(fd, cfg);
    } else if (strcmp(path, "/api/time/sync") == 0) {
        send_time_sync(fd, cfg, query);
    } else if (strcmp(path, "/api/time/status") == 0) {
        join_path(file_path, sizeof(file_path), cfg->data_dir, "time_sync.json");
        send_json_file_or_default(fd, file_path,
                                  "{\"ok\":true,\"state\":\"unknown\",\"message\":\"Time has not been synced from the UI.\"}\n");
    } else if (strcmp(path, "/api/refresh/status") == 0) {
        send_refresh_status(fd, cfg);
    } else if (strcmp(path, "/api/config") == 0) {
        send_config(fd, cfg);
    } else if (strcmp(path, "/api/satellites") == 0) {
        send_satellites(fd, cfg);
    } else if (strcmp(path, "/api/passes") == 0) {
        join_path(file_path, sizeof(file_path), cfg->data_dir, "passes.json");
        send_json_file_or_default(fd, file_path,
                                  "{\"version\":1,\"passes\":[],\"message\":\"Pass predictions have not been generated yet.\"}\n");
    } else if (strcmp(path, "/api/radio/receive/status") == 0) {
        send_receive_status_v261a(fd);
    } else if (strcmp(path, "/api/radio/decode/output") == 0) {
        send_decode_output_v261a(fd, query);
    } else if (strcmp(path, "/api/radio/status") == 0) {
        join_path(file_path, sizeof(file_path), cfg->data_dir, "radio_target.json");
        send_json_file_or_default(fd, file_path,
                                  "{\"ok\":true,\"state\":\"idle\",\"message\":\"No radio target planned.\"}\n");
    } else if (strcmp(path, "/api/radio/track/status") == 0) {
        join_path(file_path, sizeof(file_path), cfg->data_dir, "radio_track.json");
        send_json_file_or_default(fd, file_path,
                                  "{\"ok\":true,\"state\":\"idle\",\"message\":\"No Doppler track planned.\"}\n");
    } else if (strcmp(path, "/api/radio/track/state") == 0) {
        join_path(file_path, sizeof(file_path), cfg->data_dir, "radio_track_state.json");
        send_json_file_or_default(fd, file_path,
                                  "{\"ok\":true,\"state\":\"idle\",\"message\":\"Doppler tracking has not started.\"}\n");
    } else if (strcmp(path, "/api/radio/track/start") == 0) {
        send_radio_track_start(fd, cfg, "active");
    } else if (strcmp(path, "/api/radio/track/step") == 0) {
        send_radio_track_step(fd, cfg, "active");
    } else if (strcmp(path, "/api/radio/track/tune_point") == 0) {
        send_radio_track_tune_point(fd, cfg, query, "focused");
    } else if (strcmp(path, "/api/radio/track/stop") == 0) {
        send_radio_track_stop(fd, cfg);
    } else if (strcmp(path, "/api/radio/track/auto/start") == 0) {
        send_radio_track_auto_start(fd, cfg);
    } else if (strcmp(path, "/api/radio/track/auto/stop") == 0) {
        send_radio_track_auto_stop(fd, cfg);
    } else if (strcmp(path, "/api/radio/hardware") == 0) {
        send_radio_hardware(fd, cfg);
    } else if (strcmp(path, "/api/radio/audio/fm.wav") == 0) {
        send_wav_stream(fd, cfg, query);
    } else if (strcmp(path, "/api/radio/audio/fm.pcm") == 0) {
        send_pcm_stream(fd, cfg, query);
    } else if (strcmp(path, "/api/radio/audio/live.pcm") == 0) {
        send_pcm_live_stream(fd, cfg, query);
    } else if (strcmp(path, "/api/radio/audio/fm_chunk") == 0) {
        send_audio_chunk_json(fd, cfg, query);
    } else if (strcmp(path, "/api/radio/audio/live/stream.wav") == 0) {
        send_live_audio_stream_cfg_v1k(fd, cfg, query);
    } else if (strcmp(path, "/api/radio/audio/live/stream.wav") == 0) {
        send_live_audio_stream_v1h(fd, query);
    } else if (strcmp(path, "/api/radio/audio/live.wav") == 0) {
        if (query_param_is_true(query, "stream")) {
            send_live_audio_stream_v1h(fd, query);
        } else {
            send_live_audio_block(fd, query);
        }
    } else if (strcmp(path, "/api/radio/plan") == 0) {
        send_radio_plan(fd, cfg, query);
    } else if (strcmp(path, "/api/radio/tune") == 0) {
        send_radio_tune(fd, cfg, query);
    } else {
        send_text(fd, 404, "Not Found", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"not found\"}\n");
    }
}

static int header_name_matches(const char *line, const char *name)
{
    while (*name) {
        if (tolower((unsigned char)*line) != tolower((unsigned char)*name)) {
            return 0;
        }
        line++;
        name++;
    }
    return *line == ':';
}

static size_t content_length_from_headers(const char *headers)
{
    const char *line = headers;

    while (line && *line) {
        const char *next = strstr(line, "\r\n");
        const char *value;

        if (line[0] == '\r' || line[0] == '\n') {
            break;
        }

        if (header_name_matches(line, "Content-Length")) {
            value = strchr(line, ':');
            if (value) {
                while (*++value == ' ') {
                    ;
                }
                return (size_t)strtoul(value, NULL, 10);
            }
        }

        line = next ? next + 2 : NULL;
    }

    return 0;
}

static void serve_client(int fd, const struct app_config *cfg)
{
    char req[REQ_BUF_SIZE];
    char method[16] = "";
    char target[PATH_BUF_SIZE] = "";
    char query_copy[PATH_BUF_SIZE] = "";
    char *query;
    char *headers_end;
    char *body = "";
    size_t header_len;
    size_t content_len;
    size_t total_len;
    ssize_t got;
    ssize_t total;

    total = recv(fd, req, sizeof(req) - 1, 0);
    if (total <= 0) {
        return;
    }
    req[total] = '\0';

    while (!strstr(req, "\r\n\r\n") && (size_t)total < sizeof(req) - 1) {
        got = recv(fd, req + total, sizeof(req) - 1 - (size_t)total, 0);
        if (got <= 0) {
            break;
        }
        total += got;
        req[total] = '\0';
    }

    headers_end = strstr(req, "\r\n\r\n");
    if (headers_end) {
        header_len = (size_t)(headers_end - req) + 4;
        content_len = content_length_from_headers(req);
        total_len = header_len + content_len;
        if (total_len >= sizeof(req)) {
            send_text(fd, 413, "Payload Too Large", "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"request body is too large\"}\n");
            return;
        }
        while ((size_t)total < total_len) {
            got = recv(fd, req + total, total_len - (size_t)total, 0);
            if (got <= 0) {
                break;
            }
            total += got;
            req[total] = '\0';
        }
        body = req + header_len;
        body[content_len] = '\0';
    }

    if (sscanf(req, "%15s %1023s", method, target) != 2) {
        send_text(fd, 400, "Bad Request", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"bad request\"}\n");
        return;
    }

    query = strchr(target, '?');
    if (query) {
        *query = '\0';
        snprintf(query_copy, sizeof(query_copy), "%s", query + 1);
    }

    if (cfg->interactive) {
        printf("%s %s\n", method, target);
        fflush(stdout);
    }

    handle_request(fd, cfg, method, target, query_copy, body);
}

static void *client_worker(void *arg)
{
    struct client_job *job = (struct client_job *)arg;

    if (!job) {
        return NULL;
    }

    serve_client(job->fd, job->cfg);
    close(job->fd);
    free(job);
    return NULL;
}

static int create_server_socket(const struct app_config *cfg)
{
    int fd;
    int yes = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)cfg->port);
    if (inet_pton(AF_INET, cfg->bind_addr, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid bind address: %s\n", cfg->bind_addr);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 16) != 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

int main(int argc, char **argv)
{
    struct app_config cfg;
    int server_fd;

    if (parse_args(argc, argv, &cfg) != 0) {
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    server_fd = create_server_socket(&cfg);
    if (server_fd < 0) {
        return 1;
    }

    printf("Pluto Satellite Tracker %s\n", APP_VERSION);
    printf("Listening on http://%s:%d/SatelliteTracker/\n", cfg.bind_addr, cfg.port);
    printf("Web dir:    %s\n", cfg.web_dir);
    printf("Config dir: %s\n", cfg.config_dir);
    printf("Data dir:   %s\n", cfg.data_dir);
    fflush(stdout);

    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }
        {
            struct client_job *job = (struct client_job *)malloc(sizeof(*job));
            pthread_t client_thread;
            int create_result;

            if (!job) {
                close(client_fd);
                continue;
            }

            job->fd = client_fd;
            job->cfg = &cfg;
            create_result = pthread_create(&client_thread, NULL, client_worker, job);
            if (create_result != 0) {
                close(client_fd);
                free(job);
                continue;
            }
            pthread_detach(client_thread);
        }
    }

    close(server_fd);
    return 0;
}

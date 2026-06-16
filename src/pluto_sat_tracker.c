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

#define APP_VERSION "0.2.0"
/* RESTORE_517C91A_BUILDABLE_KEEP_RECEIVER_V1 */
/* BACKEND_STREAMING_AUDIO_STREAM_ROUTE_V1C */
/* BACKEND_STREAMING_AUDIO_ROUTE_FIX_V1B */
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

    join_path(path, sizeof(path), cfg->data_dir, "radio_target.json");
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

    join_path(path, sizeof(path), cfg->data_dir, "radio_target.json");
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

static int configure_rx_audio_path(long long frequency_hz)
{
    char command[512];
    int result;

    if (frequency_hz < PLUTO_MIN_HZ || frequency_hz > PLUTO_MAX_HZ) {
        return 0;
    }

    if (snprintf(command, sizeof(command),
                 "/usr/bin/iio_attr -u local: -c ad9361-phy voltage0 sampling_frequency %d >/dev/null 2>&1",
                 AUDIO_IQ_SAMPLE_RATE) >= (int)sizeof(command)) {
        return 0;
    }
    result = system(command);
    if (result == -1 || !WIFEXITED(result) || WEXITSTATUS(result) != 0) {
        return 0;
    }

    if (snprintf(command, sizeof(command),
                 "/usr/bin/iio_attr -u local: -c ad9361-phy voltage0 rf_bandwidth 200000 >/dev/null 2>&1") >= (int)sizeof(command)) {
        return 0;
    }
    result = system(command);
    if (result == -1 || !WIFEXITED(result) || WEXITSTATUS(result) != 0) {
        return 0;
    }

    if (snprintf(command, sizeof(command),
                 "/usr/bin/iio_attr -u local: -c ad9361-phy voltage0 gain_control_mode slow_attack >/dev/null 2>&1") >= (int)sizeof(command)) {
        return 0;
    }
    result = system(command);
    if (result == -1 || !WIFEXITED(result) || WEXITSTATUS(result) != 0) {
        return 0;
    }

    return write_rx_lo_frequency(frequency_hz, command, sizeof(command));
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

    join_path(path, sizeof(path), cfg->data_dir, "radio_target.json");
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
    return was_running;
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
        execl(helper_path,
              helper_path,
              "--freq-hz", frequency_text,
              "--output", AUDIO_LIVE_PCM_PATH,
              (char *)NULL);
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

static void send_live_audio_start(int fd, const struct app_config *cfg, const char *query)
{
    char frequency_text[64] = "";
    char body[256];
    long long frequency_hz = 0;

    query_param(query, "downlink_hz", frequency_text, sizeof(frequency_text));
    if (frequency_text[0]) {
        if (!parse_frequency_hz(frequency_text, &frequency_hz)) {
            send_audio_error_json(fd, 400, "Bad Request", "valid downlink_hz is required");
            return;
        }
    } else if (!read_active_audio_frequency_hz(cfg, &frequency_hz)) {
        send_audio_error_json(fd, 409, "Conflict", "plan or tune a satellite first, or pass downlink_hz");
        return;
    }

    if (!start_live_audio_session(cfg, frequency_hz)) {
        send_audio_error_json(fd, 500, "Internal Server Error", "could not start live Pluto audio");
        return;
    }

    snprintf(body, sizeof(body),
             "{\"ok\":true,\"state\":\"running\",\"downlink_hz\":%lld}\n",
             frequency_hz);
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

    join_path(path, sizeof(path), cfg->data_dir, "radio_track.json");
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

    join_path(path, sizeof(path), cfg->data_dir, "radio_track.json");
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
    time_t now = time(NULL);

    join_path(path, sizeof(path), cfg->data_dir, "radio_track.json");
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

    if (!write_track_state(cfg, state, name, point_index, point_time, rx_hz, lo_path, message, seconds_until_aos, seconds_until_los, -1, seconds_until_point, "written")) {
        snprintf(error, error_size, "could not write Doppler track state");
        return 500;
    }

    json_escape(name_json, sizeof(name_json), name);
    snprintf(response, response_size,
             "{\"ok\":true,\"state\":\"%s\",\"name\":\"%s\",\"point_index\":%d,\"point_time_utc\":\"%s\",\"rx_hz\":%lld,\"lo_path\":\"%s\",\"seconds_until_los\":%lld,\"seconds_until_point\":%lld,\"lo_write_result\":\"written\"}\n",
             state, name_json, point_index, point_time, rx_hz, lo_path, seconds_until_los, seconds_until_point);
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

    join_path(path, sizeof(path), cfg->data_dir, "radio_track.json");
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

static void handle_request(
    int fd,
    const struct app_config *cfg,
    const char *method,
    const char *path,
    const char *query,
    const char *body)
{
    char file_path[PATH_BUF_SIZE];

    /* BACKEND_STREAMING_AUDIO_STREAM_ROUTE_V1D_EARLY_GUARD: keep continuous browser audio out of fragile router branches. */
    if (strcmp(path, "/api/radio/audio/live/stream.wav") == 0) {
        send_live_audio_stream_v1h(fd, query);
        return;
    }

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0 && strcmp(method, "POST") != 0) {
        send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        return;
    }

    if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/api/radio/track/plan") == 0) {
            send_radio_track_plan(fd, cfg, body);
        } else if (strcmp(path, "/api/radio/audio/live/start") == 0) {
            send_live_audio_start(fd, cfg, query);
        } else if (strcmp(path, "/api/radio/audio/live/stop") == 0) {
            send_live_audio_stop(fd);
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

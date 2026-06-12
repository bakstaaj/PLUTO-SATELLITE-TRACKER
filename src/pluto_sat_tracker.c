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
#include <time.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>

#define APP_VERSION "0.1.0"
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

static volatile sig_atomic_t g_running = 1;
static pthread_mutex_t g_track_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_track_thread;
static int g_track_auto_running = 0;

struct app_config {
    const char *bind_addr;
    int port;
    const char *data_dir;
    const char *web_dir;
    const char *config_dir;
    int interactive;
};

static int query_param(const char *query, const char *name, char *out, size_t out_size);

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
    char mode_json[256];
    char description_json[1024];
    char lo_path[PATH_BUF_SIZE] = "";
    char path[PATH_BUF_SIZE];
    char tmp_path[PATH_BUF_SIZE + 8];
    char body[2048];
    long long frequency_hz;
    FILE *f;
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

    json_escape(name_json, sizeof(name_json), name);
    json_escape(mode_json, sizeof(mode_json), mode);
    json_escape(description_json, sizeof(description_json), description);

    join_path(path, sizeof(path), cfg->data_dir, "radio_target.json");
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    f = fopen(tmp_path, "wb");
    if (!f) {
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"could not write tuned target state\"}\n");
        return;
    }

    fprintf(f,
            "{\n"
            "  \"ok\": true,\n"
            "  \"planned_epoch\": %ld,\n"
            "  \"tuned_epoch\": %ld,\n"
            "  \"name\": \"%s\",\n"
            "  \"norad_id\": %s,\n"
            "  \"aos_utc\": \"%s\",\n"
            "  \"downlink_hz\": %lld,\n"
            "  \"uplink_hz\": %s,\n"
            "  \"mode\": \"%s\",\n"
            "  \"description\": \"%s\",\n"
            "  \"lo_path\": \"%s\",\n"
            "  \"state\": \"tuned\"\n"
            "}\n",
            (long)now,
            (long)now,
            name_json,
            norad,
            aos,
            frequency_hz,
            uplink[0] ? uplink : "null",
            mode_json,
            description_json,
            lo_path);
    fclose(f);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"could not publish tuned target state\"}\n");
        return;
    }

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

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0 && strcmp(method, "POST") != 0) {
        send_text(fd, 405, "Method Not Allowed", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"method not allowed\"}\n");
        return;
    }

    if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/api/radio/track/plan") == 0) {
            send_radio_track_plan(fd, cfg, body);
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
    } else if (strcmp(path, "/api/radio/track/stop") == 0) {
        send_radio_track_stop(fd, cfg);
    } else if (strcmp(path, "/api/radio/track/auto/start") == 0) {
        send_radio_track_auto_start(fd, cfg);
    } else if (strcmp(path, "/api/radio/track/auto/stop") == 0) {
        send_radio_track_auto_stop(fd, cfg);
    } else if (strcmp(path, "/api/radio/hardware") == 0) {
        send_radio_hardware(fd, cfg);
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

        serve_client(client_fd, &cfg);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}

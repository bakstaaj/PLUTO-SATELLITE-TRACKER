#define _POSIX_C_SOURCE 200809L

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
#include <sys/types.h>
#include <time.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>

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

struct app_config {
    const char *bind_addr;
    int port;
    const char *data_dir;
    const char *web_dir;
    const char *config_dir;
    int interactive;
};

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

static void send_radio_hardware(int fd)
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
    char body[2048];
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

    snprintf(body, sizeof(body),
             "{"
             "\"ok\":true,"
             "\"software_min_hz\":%lld,"
             "\"software_max_hz\":%lld,"
             "\"rx_lo_path\":\"%s\","
             "\"iio_device_name\":\"%s\","
             "\"current_rx_lo_hz\":%s,"
             "\"frequency_available\":\"%s\""
             "}\n",
             PLUTO_MIN_HZ,
             PLUTO_MAX_HZ,
             rx_path,
             device_name_json,
             current[0] ? current : "null",
             available_json);
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

static int find_nearest_track_point(
    const char *plan_json,
    long long now_epoch,
    long long *rx_hz,
    char *point_time,
    size_t point_time_size,
    int *point_index)
{
    const char *p = plan_json;
    long long best_delta = LLONG_MAX;
    int index = 0;
    int found = 0;

    while ((p = strstr(p, "\"time_utc\"")) != NULL) {
        char time_value[64] = "";
        const char *object_start = p;
        const char *time_start;
        const char *next_point;
        long long point_epoch;
        long long point_rx_hz;
        long long delta;

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

        delta = point_epoch >= now_epoch ? point_epoch - now_epoch : now_epoch - point_epoch;
        if (!found || delta < best_delta) {
            found = 1;
            best_delta = delta;
            *rx_hz = point_rx_hz;
            *point_index = index;
            snprintf(point_time, point_time_size, "%s", time_value);
        }

        p = next_point ? next_point : time_start + strlen(time_start);
        index++;
    }

    return found;
}

static int write_track_state(
    const struct app_config *cfg,
    const char *state,
    const char *name,
    int point_index,
    const char *point_time,
    long long rx_hz,
    const char *lo_path,
    const char *message)
{
    char path[PATH_BUF_SIZE];
    char tmp_path[PATH_BUF_SIZE + 8];
    char name_json[512];
    char message_json[512];
    FILE *f;
    time_t now = time(NULL);

    json_escape(name_json, sizeof(name_json), name ? name : "");
    json_escape(message_json, sizeof(message_json), message ? message : "");

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
            "  \"message\": \"%s\"\n"
            "}\n",
            state,
            (long)now,
            name_json,
            point_index,
            point_time ? point_time : "",
            rx_hz,
            lo_path ? lo_path : "",
            message_json);
    fclose(f);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return 0;
    }
    return 1;
}

static void send_radio_track_step(int fd, const struct app_config *cfg, const char *state)
{
    char path[PATH_BUF_SIZE];
    char *plan = NULL;
    size_t plan_len = 0;
    char name[256] = "";
    char name_json[512];
    char point_time[64] = "";
    char lo_path[PATH_BUF_SIZE] = "";
    char response[1024];
    long long rx_hz = 0;
    int point_index = -1;
    time_t now = time(NULL);

    join_path(path, sizeof(path), cfg->data_dir, "radio_track.json");
    if (read_file_to_string(path, &plan, &plan_len) != 0) {
        send_text(fd, 404, "Not Found", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"no Doppler track plan has been stored\"}\n");
        return;
    }
    (void)plan_len;

    json_string_value(plan, "name", name, sizeof(name));
    if (!find_nearest_track_point(plan, (long long)now, &rx_hz, point_time, sizeof(point_time), &point_index)) {
        free(plan);
        send_text(fd, 400, "Bad Request", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"track plan does not contain usable Doppler points\"}\n");
        return;
    }
    free(plan);

    if (rx_hz < PLUTO_MIN_HZ || rx_hz > PLUTO_MAX_HZ || !write_rx_lo_frequency(rx_hz, lo_path, sizeof(lo_path))) {
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"could not tune RX LO for Doppler track point\"}\n");
        return;
    }

    if (!write_track_state(cfg, state, name, point_index, point_time, rx_hz, lo_path, "nearest Doppler point tuned")) {
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"could not write Doppler track state\"}\n");
        return;
    }

    json_escape(name_json, sizeof(name_json), name);
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"state\":\"%s\",\"name\":\"%s\",\"point_index\":%d,\"point_time_utc\":\"%s\",\"rx_hz\":%lld,\"lo_path\":\"%s\"}\n",
             state, name_json, point_index, point_time, rx_hz, lo_path);
    send_text(fd, 200, "OK", "application/json; charset=utf-8", response);
}

static void send_radio_track_stop(int fd, const struct app_config *cfg)
{
    if (!write_track_state(cfg, "stopped", "", -1, "", 0, "", "Doppler tracking stopped")) {
        send_text(fd, 500, "Internal Server Error", "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"could not write Doppler track state\"}\n");
        return;
    }
    send_text(fd, 200, "OK", "application/json; charset=utf-8",
              "{\"ok\":true,\"state\":\"stopped\"}\n");
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
        send_radio_track_step(fd, cfg, "active");
    } else if (strcmp(path, "/api/radio/track/step") == 0) {
        send_radio_track_step(fd, cfg, "active");
    } else if (strcmp(path, "/api/radio/track/stop") == 0) {
        send_radio_track_stop(fd, cfg);
    } else if (strcmp(path, "/api/radio/hardware") == 0) {
        send_radio_hardware(fd);
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

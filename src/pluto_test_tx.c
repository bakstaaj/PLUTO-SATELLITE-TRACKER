#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifndef PLUTO_PI
#define PLUTO_PI 3.14159265358979323846264338327950288
#endif

#define DEFAULT_FREQ_HZ       435000000ULL
#define DEFAULT_SAMPLE_RATE   2400000U
#define DEFAULT_BW_HZ         200000U
#define DEFAULT_SECONDS       10U
#define DEFAULT_TX_GAIN_DB    (-30.0)
#define DEFAULT_AMPLITUDE     0.15
#define DEFAULT_TONE_HZ       1000.0
#define DEFAULT_DEVIATION_HZ  5000.0
#define DEFAULT_OFFSET_HZ     25000.0
#define DEFAULT_TX_CHANNEL    1
#define DEFAULT_CW_WPM        12.0
#define DEFAULT_CW_TEXT       "CQ PLUTO TEST"

#define TX_STREAM_DEVICE "cf-ad9361-dds-core-lpc"
#define PHY_DEVICE_NAME  "ad9361-phy"

typedef enum {
    MODE_CARRIER = 0,
    MODE_FM_TONE = 1,
    MODE_CW = 2
} tx_mode_t;

typedef struct {
    int on;
    int units;
} cw_event_t;

typedef struct {
    cw_event_t *events;
    size_t count;
    size_t cap;
} cw_pattern_t;

typedef struct {
    tx_mode_t mode;
    uint64_t freq_hz;
    unsigned sample_rate;
    unsigned bandwidth_hz;
    unsigned seconds;
    double tx_gain_db;
    double amplitude;
    double tone_hz;
    double deviation_hz;
    double offset_hz;
    double cw_wpm;
    char cw_text[256];
    int tx_channel;
    int dry_run;
} options_t;

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s --mode carrier|fm-tone|cw [options]\n"
        "\n"
        "Explicit Pluto Plus TX channel mapping:\n"
        "  --tx-channel 1 uses iio_writedev channels: voltage0 voltage1\n"
        "  --tx-channel 2 uses iio_writedev channels: voltage2 voltage3\n"
        "\n"
        "Options:\n"
        "  --tx-channel <1|2>      Physical TX channel pair, default %d\n"
        "  --freq-hz <hz>          RF frequency, default %" PRIu64 "\n"
        "  --seconds <n>           Duration, default %u\n"
        "  --sample-rate <hz>      TX sample rate, default %u\n"
        "  --bandwidth-hz <hz>     TX RF bandwidth, default %u\n"
        "  --tx-gain-db <db>       AD9361 TX hardwaregain, default %.2f\n"
        "  --amplitude <0..1>      IQ amplitude scale, default %.3f\n"
        "  --offset-hz <hz>        Carrier/baseband offset, default %.1f\n"
        "  --tone-hz <hz>          FM tone frequency, default %.1f\n"
        "  --deviation-hz <hz>     FM deviation, default %.1f\n"
        "  --cw-text <text>        CW text, default \"%s\"\n"
        "  --cw-wpm <wpm>          CW speed, default %.1f\n"
        "  --dry-run               Configure only; do not transmit samples\n"
        "\n"
        "Examples:\n"
        "  %s --mode carrier --tx-channel 1 --freq-hz 435000000 --offset-hz 25000\n"
        "  %s --mode fm-tone --tx-channel 1 --freq-hz 435000000 --offset-hz 25000 --tone-hz 1000\n"
        "  %s --mode cw --tx-channel 1 --freq-hz 435000000 --offset-hz 25000 --cw-text \"CQ PLUTO TEST\" --cw-wpm 12\n",
        prog,
        DEFAULT_TX_CHANNEL,
        (uint64_t)DEFAULT_FREQ_HZ,
        DEFAULT_SECONDS,
        DEFAULT_SAMPLE_RATE,
        DEFAULT_BW_HZ,
        DEFAULT_TX_GAIN_DB,
        DEFAULT_AMPLITUDE,
        DEFAULT_OFFSET_HZ,
        DEFAULT_TONE_HZ,
        DEFAULT_DEVIATION_HZ,
        DEFAULT_CW_TEXT,
        DEFAULT_CW_WPM,
        prog, prog, prog
    );
}

static int parse_u64(const char *s, uint64_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || !end || *end != '\0') return -1;
    *out = (uint64_t)v;
    return 0;
}

static int parse_uint(const char *s, unsigned *out) {
    uint64_t v = 0;
    if (parse_u64(s, &v) != 0 || v > 4000000000ULL) return -1;
    *out = (unsigned)v;
    return 0;
}

static int parse_double(const char *s, double *out) {
    char *end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (errno || !end || *end != '\0' || !isfinite(v)) return -1;
    *out = v;
    return 0;
}

static int read_text_file(const char *path, char *buf, size_t buflen) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, (int)buflen, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    size_t n = strlen(buf);
    while (n > 0 && isspace((unsigned char)buf[n - 1])) buf[--n] = '\0';
    return 0;
}

static int write_text_file(const char *path, const char *value, int required) {
    FILE *f = fopen(path, "w");
    if (!f) {
        if (required) fprintf(stderr, "ERROR: open %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    if (fprintf(f, "%s", value) < 0) {
        if (required) fprintf(stderr, "ERROR: write %s failed: %s\n", path, strerror(errno));
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) {
        if (required) fprintf(stderr, "ERROR: close %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int find_iio_device_by_name(const char *name, char *out, size_t out_len) {
    DIR *d = opendir("/sys/bus/iio/devices");
    if (!d) {
        fprintf(stderr, "ERROR: cannot open /sys/bus/iio/devices: %s\n", strerror(errno));
        return -1;
    }

    struct dirent *de = NULL;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "iio:device", 10) != 0) continue;
        char name_path[512];
        snprintf(name_path, sizeof(name_path), "/sys/bus/iio/devices/%s/name", de->d_name);

        char found[256];
        if (read_text_file(name_path, found, sizeof(found)) == 0 && strcmp(found, name) == 0) {
            snprintf(out, out_len, "/sys/bus/iio/devices/%s", de->d_name);
            closedir(d);
            return 0;
        }
    }

    closedir(d);
    fprintf(stderr, "ERROR: IIO device named %s not found\n", name);
    return -1;
}

static int write_attr(const char *dir, const char *attr, const char *value, int required) {
    char path[768];
    snprintf(path, sizeof(path), "%s/%s", dir, attr);
    return write_text_file(path, value, required);
}

static void fmt_u64(char *buf, size_t buflen, uint64_t v) { snprintf(buf, buflen, "%" PRIu64, v); }
static void fmt_uint(char *buf, size_t buflen, unsigned v) { snprintf(buf, buflen, "%u", v); }
static void fmt_double(char *buf, size_t buflen, double v) { snprintf(buf, buflen, "%.6f", v); }

static int configure_tx(const options_t *opt) {
    char phy_dir[512], tx_dir[512];

    if (find_iio_device_by_name(PHY_DEVICE_NAME, phy_dir, sizeof(phy_dir)) != 0) return -1;
    if (find_iio_device_by_name(TX_STREAM_DEVICE, tx_dir, sizeof(tx_dir)) != 0) return -1;

    char v[64];

    fmt_u64(v, sizeof(v), opt->freq_hz);
    if (write_attr(phy_dir, "out_altvoltage1_TX_LO_frequency", v, 1) != 0) return -1;

    fmt_uint(v, sizeof(v), opt->sample_rate);
    write_attr(phy_dir, "out_voltage_sampling_frequency", v, 0);
    write_attr(phy_dir, "out_voltage0_sampling_frequency", v, 0);
    write_attr(phy_dir, "out_voltage1_sampling_frequency", v, 0);
    write_attr(phy_dir, "out_voltage2_sampling_frequency", v, 0);
    write_attr(phy_dir, "out_voltage3_sampling_frequency", v, 0);

    fmt_uint(v, sizeof(v), opt->bandwidth_hz);
    write_attr(phy_dir, "out_voltage_rf_bandwidth", v, 0);
    write_attr(phy_dir, "out_voltage0_rf_bandwidth", v, 0);
    write_attr(phy_dir, "out_voltage1_rf_bandwidth", v, 0);
    write_attr(phy_dir, "out_voltage2_rf_bandwidth", v, 0);
    write_attr(phy_dir, "out_voltage3_rf_bandwidth", v, 0);

    fmt_double(v, sizeof(v), opt->tx_gain_db);
    if (opt->tx_channel == 1) {
        write_attr(phy_dir, "out_voltage0_hardwaregain", v, 0);
        write_attr(phy_dir, "out_voltage1_hardwaregain", v, 0);
    } else {
        write_attr(phy_dir, "out_voltage2_hardwaregain", v, 0);
        write_attr(phy_dir, "out_voltage3_hardwaregain", v, 0);
    }

    for (int i = 0; i < 4; i++) {
        char attr[96];
        snprintf(attr, sizeof(attr), "scan_elements/out_voltage%d_en", i);
        write_attr(tx_dir, attr, "0", 0);
    }
    if (opt->tx_channel == 1) {
        write_attr(tx_dir, "scan_elements/out_voltage0_en", "1", 0);
        write_attr(tx_dir, "scan_elements/out_voltage1_en", "1", 0);
    } else {
        write_attr(tx_dir, "scan_elements/out_voltage2_en", "1", 0);
        write_attr(tx_dir, "scan_elements/out_voltage3_en", "1", 0);
    }

    fprintf(stderr, "TX configured:\n");
    fprintf(stderr, "  phy:          %s\n", phy_dir);
    fprintf(stderr, "  stream:       %s\n", tx_dir);
    fprintf(stderr, "  tx_channel:   %d\n", opt->tx_channel);
    fprintf(stderr, "  iio channels: %s\n", opt->tx_channel == 1 ? "voltage0 voltage1" : "voltage2 voltage3");
    fprintf(stderr, "  freq_hz:      %" PRIu64 "\n", opt->freq_hz);
    fprintf(stderr, "  sample_rate:  %u\n", opt->sample_rate);
    fprintf(stderr, "  bandwidth_hz: %u\n", opt->bandwidth_hz);
    fprintf(stderr, "  tx_gain_db:   %.2f\n", opt->tx_gain_db);
    fprintf(stderr, "  amplitude:    %.4f\n", opt->amplitude);
    fprintf(stderr, "  offset_hz:    %.1f\n", opt->offset_hz);
    if (opt->mode == MODE_CW) {
        fprintf(stderr, "  cw_text:      %s\n", opt->cw_text);
        fprintf(stderr, "  cw_wpm:       %.1f\n", opt->cw_wpm);
    }

    return 0;
}

static int16_t clamp_i16(double x) {
    if (x > 32767.0) return 32767;
    if (x < -32768.0) return -32768;
    return (int16_t)lrint(x);
}

static const char *morse_for_char(char c) {
    switch (toupper((unsigned char)c)) {
        case 'A': return ".-";    case 'B': return "-...";  case 'C': return "-.-.";
        case 'D': return "-..";   case 'E': return ".";     case 'F': return "..-.";
        case 'G': return "--.";   case 'H': return "....";  case 'I': return "..";
        case 'J': return ".---";  case 'K': return "-.-";   case 'L': return ".-..";
        case 'M': return "--";    case 'N': return "-.";    case 'O': return "---";
        case 'P': return ".--.";  case 'Q': return "--.-";  case 'R': return ".-.";
        case 'S': return "...";   case 'T': return "-";     case 'U': return "..-";
        case 'V': return "...-";  case 'W': return ".--";   case 'X': return "-..-";
        case 'Y': return "-.--";  case 'Z': return "--..";
        case '0': return "-----"; case '1': return ".----"; case '2': return "..---";
        case '3': return "...--"; case '4': return "....-"; case '5': return ".....";
        case '6': return "-...."; case '7': return "--..."; case '8': return "---..";
        case '9': return "----.";
        case '/': return "-..-."; case '?': return "..--.."; case '.': return ".-.-.-";
        case ',': return "--..--"; case '-': return "-....-";
        default: return NULL;
    }
}

static int cw_add_event(cw_pattern_t *pat, int on, int units) {
    if (units <= 0) return 0;
    if (pat->count > 0 && pat->events[pat->count - 1].on == on) {
        pat->events[pat->count - 1].units += units;
        return 0;
    }
    if (pat->count == pat->cap) {
        size_t new_cap = pat->cap ? pat->cap * 2 : 128;
        cw_event_t *tmp = realloc(pat->events, new_cap * sizeof(*tmp));
        if (!tmp) return -1;
        pat->events = tmp;
        pat->cap = new_cap;
    }
    pat->events[pat->count].on = on;
    pat->events[pat->count].units = units;
    pat->count++;
    return 0;
}

static int cw_build_pattern(const char *text, cw_pattern_t *pat) {
    memset(pat, 0, sizeof(*pat));

    int first_symbol = 1;
    for (size_t i = 0; text[i]; i++) {
        char ch = text[i];

        if (isspace((unsigned char)ch)) {
            cw_add_event(pat, 0, 7);
            first_symbol = 1;
            continue;
        }

        const char *m = morse_for_char(ch);
        if (!m) continue;

        if (!first_symbol) cw_add_event(pat, 0, 3);

        for (size_t j = 0; m[j]; j++) {
            if (j > 0) cw_add_event(pat, 0, 1);
            cw_add_event(pat, 1, (m[j] == '-') ? 3 : 1);
        }

        first_symbol = 0;
    }

    cw_add_event(pat, 0, 7);
    return pat->count > 0 ? 0 : -1;
}

static int cw_key_on_for_sample(const cw_pattern_t *pat, double unit_samples, uint64_t sample_index) {
    if (!pat || pat->count == 0 || unit_samples <= 1.0) return 0;

    uint64_t sample_cursor = 0;
    for (size_t i = 0; i < pat->count; i++) {
        uint64_t dur = (uint64_t)llround((double)pat->events[i].units * unit_samples);
        if (dur < 1) dur = 1;
        if (sample_index >= sample_cursor && sample_index < sample_cursor + dur) {
            return pat->events[i].on;
        }
        sample_cursor += dur;
    }

    return 0;
}

static int transmit_samples(const options_t *opt) {
    uint64_t total_samples = (uint64_t)opt->sample_rate * (uint64_t)opt->seconds;
    double amp = opt->amplitude * 32767.0;

    const char *chans = (opt->tx_channel == 1) ? "voltage0 voltage1" : "voltage2 voltage3";

    char cmd[384];
    snprintf(cmd, sizeof(cmd),
             "iio_writedev -u local: -b 8192 -s %" PRIu64 " %s %s",
             total_samples, TX_STREAM_DEVICE, chans);

    fprintf(stderr, "Starting TX stream:\n");
    fprintf(stderr, "  command: %s\n", cmd);
    fprintf(stderr, "  samples: %" PRIu64 "\n", total_samples);
    fprintf(stderr, "  lanes:   2 int16 values per scan sample, explicit channel args\n");
    fprintf(stderr, "  mode:    %s\n", opt->mode == MODE_CARRIER ? "carrier" : (opt->mode == MODE_FM_TONE ? "fm-tone" : "cw"));

    FILE *pipe = popen(cmd, "w");
    if (!pipe) {
        fprintf(stderr, "ERROR: popen iio_writedev failed: %s\n", strerror(errno));
        return -1;
    }

    const size_t chunk_samples = 4096;
    int16_t *buf = calloc(chunk_samples * 2, sizeof(int16_t));
    if (!buf) {
        fprintf(stderr, "ERROR: calloc failed\n");
        pclose(pipe);
        return -1;
    }

    cw_pattern_t cw = {0};
    double cw_unit_samples = 0.0;
    if (opt->mode == MODE_CW) {
        if (cw_build_pattern(opt->cw_text, &cw) != 0) {
            fprintf(stderr, "ERROR: could not build CW pattern from text: %s\n", opt->cw_text);
            free(buf);
            pclose(pipe);
            return -1;
        }
        double unit_seconds = 1.2 / opt->cw_wpm;
        cw_unit_samples = unit_seconds * (double)opt->sample_rate;
        fprintf(stderr, "  cw_unit_sec: %.6f\n", unit_seconds);
        fprintf(stderr, "  cw_events:   %zu\n", cw.count);
    }

    uint64_t produced = 0;
    double iq_phase = 0.0;
    double offset_phase = 0.0;
    double offset_step = 2.0 * PLUTO_PI * opt->offset_hz / (double)opt->sample_rate;
    double tone_phase = 0.0;
    double tone_step = 2.0 * PLUTO_PI * opt->tone_hz / (double)opt->sample_rate;

    while (produced < total_samples) {
        size_t n = chunk_samples;
        if ((uint64_t)n > total_samples - produced) n = (size_t)(total_samples - produced);

        for (size_t i = 0; i < n; i++) {
            uint64_t absolute_sample = produced + i;
            double iv = 0.0, qv = 0.0;

            if (opt->mode == MODE_CARRIER) {
                iv = amp * cos(offset_phase);
                qv = amp * sin(offset_phase);
                offset_phase += offset_step;
                if (offset_phase > 2.0 * PLUTO_PI) offset_phase -= 2.0 * PLUTO_PI;
                if (offset_phase < -2.0 * PLUTO_PI) offset_phase += 2.0 * PLUTO_PI;
            } else if (opt->mode == MODE_CW) {
                int on = cw_key_on_for_sample(&cw, cw_unit_samples, absolute_sample);
                if (on) {
                    iv = amp * cos(offset_phase);
                    qv = amp * sin(offset_phase);
                } else {
                    iv = 0.0;
                    qv = 0.0;
                }
                offset_phase += offset_step;
                if (offset_phase > 2.0 * PLUTO_PI) offset_phase -= 2.0 * PLUTO_PI;
                if (offset_phase < -2.0 * PLUTO_PI) offset_phase += 2.0 * PLUTO_PI;
            } else {
                double inst_dev = opt->deviation_hz * sin(tone_phase);
                iq_phase += 2.0 * PLUTO_PI * inst_dev / (double)opt->sample_rate;
                offset_phase += offset_step;
                tone_phase += tone_step;

                if (iq_phase > PLUTO_PI) iq_phase -= 2.0 * PLUTO_PI;
                if (iq_phase < -PLUTO_PI) iq_phase += 2.0 * PLUTO_PI;
                if (offset_phase > 2.0 * PLUTO_PI) offset_phase -= 2.0 * PLUTO_PI;
                if (offset_phase < -2.0 * PLUTO_PI) offset_phase += 2.0 * PLUTO_PI;
                if (tone_phase > 2.0 * PLUTO_PI) tone_phase -= 2.0 * PLUTO_PI;

                double total_phase = offset_phase + iq_phase;
                iv = amp * cos(total_phase);
                qv = amp * sin(total_phase);
            }

            buf[i * 2 + 0] = clamp_i16(iv);
            buf[i * 2 + 1] = clamp_i16(qv);
        }

        size_t wrote = fwrite(buf, sizeof(int16_t) * 2, n, pipe);
        if (wrote != n) {
            fprintf(stderr, "ERROR: fwrite to iio_writedev failed after %" PRIu64 " samples\n", produced);
            free(cw.events);
            free(buf);
            pclose(pipe);
            return -1;
        }

        produced += n;
    }

    free(cw.events);
    free(buf);

    int rc = pclose(pipe);
    if (rc != 0) {
        fprintf(stderr, "ERROR: iio_writedev exited with status %d\n", rc);
        return -1;
    }

    fprintf(stderr, "TX complete.\n");
    return 0;
}

int main(int argc, char **argv) {
    options_t opt;
    memset(&opt, 0, sizeof(opt));

    opt.mode = MODE_CARRIER;
    opt.freq_hz = DEFAULT_FREQ_HZ;
    opt.sample_rate = DEFAULT_SAMPLE_RATE;
    opt.bandwidth_hz = DEFAULT_BW_HZ;
    opt.seconds = DEFAULT_SECONDS;
    opt.tx_gain_db = DEFAULT_TX_GAIN_DB;
    opt.amplitude = DEFAULT_AMPLITUDE;
    opt.tone_hz = DEFAULT_TONE_HZ;
    opt.deviation_hz = DEFAULT_DEVIATION_HZ;
    opt.offset_hz = DEFAULT_OFFSET_HZ;
    opt.cw_wpm = DEFAULT_CW_WPM;
    snprintf(opt.cw_text, sizeof(opt.cw_text), "%s", DEFAULT_CW_TEXT);
    opt.tx_channel = DEFAULT_TX_CHANNEL;

    int mode_seen = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(a, "--dry-run") == 0) {
            opt.dry_run = 1;
        } else if (strcmp(a, "--mode") == 0 && i + 1 < argc) {
            const char *m = argv[++i];
            if (strcasecmp(m, "carrier") == 0) opt.mode = MODE_CARRIER;
            else if (strcasecmp(m, "fm-tone") == 0 || strcasecmp(m, "fmtone") == 0) opt.mode = MODE_FM_TONE;
            else if (strcasecmp(m, "cw") == 0 || strcasecmp(m, "morse") == 0) opt.mode = MODE_CW;
            else {
                fprintf(stderr, "ERROR: unsupported mode: %s\n", m);
                usage(argv[0]);
                return 2;
            }
            mode_seen = 1;
        } else if (strcmp(a, "--tx-channel") == 0 && i + 1 < argc) {
            unsigned ch = 0;
            if (parse_uint(argv[++i], &ch) != 0 || (ch != 1 && ch != 2)) {
                fprintf(stderr, "ERROR: invalid --tx-channel; use 1 or 2\n");
                return 2;
            }
            opt.tx_channel = (int)ch;
        } else if (strcmp(a, "--freq-hz") == 0 && i + 1 < argc) {
            if (parse_u64(argv[++i], &opt.freq_hz) != 0) {
                fprintf(stderr, "ERROR: invalid --freq-hz\n");
                return 2;
            }
        } else if (strcmp(a, "--seconds") == 0 && i + 1 < argc) {
            if (parse_uint(argv[++i], &opt.seconds) != 0 || opt.seconds == 0 || opt.seconds > 120) {
                fprintf(stderr, "ERROR: invalid --seconds; use 1..120\n");
                return 2;
            }
        } else if (strcmp(a, "--sample-rate") == 0 && i + 1 < argc) {
            if (parse_uint(argv[++i], &opt.sample_rate) != 0 || opt.sample_rate < 2083333 || opt.sample_rate > 30720000) {
                fprintf(stderr, "ERROR: invalid --sample-rate; Pluto range is about 2083333..30720000\n");
                return 2;
            }
        } else if (strcmp(a, "--bandwidth-hz") == 0 && i + 1 < argc) {
            if (parse_uint(argv[++i], &opt.bandwidth_hz) != 0 || opt.bandwidth_hz < 200000) {
                fprintf(stderr, "ERROR: invalid --bandwidth-hz\n");
                return 2;
            }
        } else if (strcmp(a, "--tx-gain-db") == 0 && i + 1 < argc) {
            if (parse_double(argv[++i], &opt.tx_gain_db) != 0 || opt.tx_gain_db < -89.75 || opt.tx_gain_db > 0.0) {
                fprintf(stderr, "ERROR: invalid --tx-gain-db; use -89.75..0.0\n");
                return 2;
            }
        } else if (strcmp(a, "--amplitude") == 0 && i + 1 < argc) {
            if (parse_double(argv[++i], &opt.amplitude) != 0 || opt.amplitude <= 0.0 || opt.amplitude > 1.0) {
                fprintf(stderr, "ERROR: invalid --amplitude; use >0 and <=1\n");
                return 2;
            }
        } else if (strcmp(a, "--offset-hz") == 0 && i + 1 < argc) {
            if (parse_double(argv[++i], &opt.offset_hz) != 0 || fabs(opt.offset_hz) > 500000.0) {
                fprintf(stderr, "ERROR: invalid --offset-hz; use -500000..500000\n");
                return 2;
            }
        } else if (strcmp(a, "--tone-hz") == 0 && i + 1 < argc) {
            if (parse_double(argv[++i], &opt.tone_hz) != 0 || opt.tone_hz <= 0.0 || opt.tone_hz > 12000.0) {
                fprintf(stderr, "ERROR: invalid --tone-hz\n");
                return 2;
            }
        } else if (strcmp(a, "--deviation-hz") == 0 && i + 1 < argc) {
            if (parse_double(argv[++i], &opt.deviation_hz) != 0 || opt.deviation_hz <= 0.0 || opt.deviation_hz > 50000.0) {
                fprintf(stderr, "ERROR: invalid --deviation-hz\n");
                return 2;
            }
        } else if (strcmp(a, "--cw-wpm") == 0 && i + 1 < argc) {
            if (parse_double(argv[++i], &opt.cw_wpm) != 0 || opt.cw_wpm < 3.0 || opt.cw_wpm > 60.0) {
                fprintf(stderr, "ERROR: invalid --cw-wpm; use 3..60\n");
                return 2;
            }
        } else if (strcmp(a, "--cw-text") == 0 && i + 1 < argc) {
            snprintf(opt.cw_text, sizeof(opt.cw_text), "%s", argv[++i]);
        } else {
            fprintf(stderr, "ERROR: unknown or incomplete argument: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    if (!mode_seen) {
        fprintf(stderr, "ERROR: --mode is required\n");
        usage(argv[0]);
        return 2;
    }

    if (configure_tx(&opt) != 0) return 1;

    if (opt.dry_run) {
        fprintf(stderr, "Dry run complete; no samples transmitted.\n");
        return 0;
    }

    return transmit_samples(&opt) == 0 ? 0 : 1;
}

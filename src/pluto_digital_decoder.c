/*
 * pluto_digital_decoder.c
 *
 * Digital signal decoder for Pluto Plus SDR.
 * Supported modes: aprs, cw
 *
 * Usage:
 *   pluto_digital_decoder --freq-hz <hz> --mode <aprs|cw> --output <path>
 *
 * Captures IQ from iio_readdev, demodulates, and writes NDJSON frames
 * (one JSON object per line) to the output file.
 *
 * APRS: FM demod -> AFSK Bell 202 -> NRZ-I -> AX.25 -> APRS parse
 * CW:   IQ magnitude envelope -> dit/dah timing -> Morse -> text
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Hardware constants                                                   */
/* ------------------------------------------------------------------ */

#define PLUTO_MIN_HZ       70000000LL
#define PLUTO_MAX_HZ     6000000000LL

#define IQ_SAMPLE_RATE   2400000       /* iio_readdev sample rate     */
#define FM_DECIMATION    100           /* IQ -> audio decimation       */
#define AUDIO_RATE       24000         /* = IQ_SAMPLE_RATE/FM_DECIM   */
#define IIO_BUFFER_SAMPS 24000         /* iio_readdev buffer size      */

/* ------------------------------------------------------------------ */
/* APRS / AFSK constants                                               */
/* ------------------------------------------------------------------ */

#define APRS_BAUD        1200
#define AFSK_MARK_HZ     1200          /* binary 1 tone                */
#define AFSK_SPACE_HZ    2200          /* binary 0 tone                */
#define SAMPS_PER_SYM    (AUDIO_RATE / APRS_BAUD)  /* = 20            */

/* AX.25 */
#define AX25_FLAG        0x7E
#define AX25_MAX_FRAME   330

/* ------------------------------------------------------------------ */
/* CW constants                                                        */
/* ------------------------------------------------------------------ */

#define CW_DECIMATION    200           /* IQ -> envelope decimation    */
#define CW_ENVELOPE_RATE (IQ_SAMPLE_RATE / CW_DECIMATION)  /* 12 kHz  */
#define CW_WPM_INIT      20            /* starting WPM estimate        */
#define CW_DIT_MS_INIT   (1200 / CW_WPM_INIT)  /* dit length ms       */

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_running = 1;
static int g_seq = 0;

static void on_signal(int signum) { (void)signum; g_running = 0; }

/* ------------------------------------------------------------------ */
/* IIO / hardware                                                      */
/* ------------------------------------------------------------------ */

static int write_rx_lo(long long hz)
{
    const char *paths[] = {
        "/sys/bus/iio/devices/iio:device1/out_altvoltage0_RX_LO_frequency",
        "/sys/bus/iio/devices/iio:device0/out_altvoltage0_RX_LO_frequency",
        "/sys/bus/iio/devices/iio:device2/out_altvoltage0_RX_LO_frequency",
        "/sys/kernel/debug/iio/iio:device1/out_altvoltage0_RX_LO_frequency",
        "/sys/kernel/debug/iio/iio:device0/out_altvoltage0_RX_LO_frequency",
    };
    size_t i;
    for (i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) {
        FILE *f = fopen(paths[i], "wb");
        if (!f) continue;
        fprintf(f, "%lld\n", hz);
        if (fclose(f) == 0) return 1;
    }
    return 0;
}

static int configure_rx(long long hz, int sample_rate_hz)
{
    char cmd[512];
    int r;
    if (hz < PLUTO_MIN_HZ || hz > PLUTO_MAX_HZ) return 0;
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/iio_attr -u local: -c ad9361-phy voltage0 sampling_frequency %d >/dev/null 2>&1",
        sample_rate_hz);
    r = system(cmd); if (r == -1 || !WIFEXITED(r) || WEXITSTATUS(r) != 0) return 0;
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/iio_attr -u local: -c ad9361-phy voltage0 rf_bandwidth 200000 >/dev/null 2>&1");
    r = system(cmd); if (r == -1 || !WIFEXITED(r) || WEXITSTATUS(r) != 0) return 0;
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/iio_attr -u local: -c ad9361-phy voltage0 gain_control_mode slow_attack >/dev/null 2>&1");
    r = system(cmd); if (r == -1 || !WIFEXITED(r) || WEXITSTATUS(r) != 0) return 0;
    return write_rx_lo(hz);
}

static FILE *start_iio_stream(pid_t *child_pid, int buffer_samps)
{
    int fds[2];
    pid_t pid;
    char buf[32];
    if (pipe(fds) != 0) return NULL;
    pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return NULL; }
    if (pid == 0) {
        snprintf(buf, sizeof(buf), "%d", buffer_samps);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]); close(fds[1]);
        execl("/usr/bin/iio_readdev", "iio_readdev",
              "-u", "local:", "-b", buf, "-s", "0",
              "cf-ad9361-lpc", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    *child_pid = pid;
    return fdopen(fds[0], "rb");
}

/* ------------------------------------------------------------------ */
/* Timestamp helper                                                    */
/* ------------------------------------------------------------------ */

static void utc_now(char *out, size_t size)
{
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    if (tm)
        strftime(out, size, "%Y-%m-%dT%H:%M:%SZ", tm);
    else
        snprintf(out, size, "unknown");
}

/* ------------------------------------------------------------------ */
/* JSON string escape                                                  */
/* ------------------------------------------------------------------ */

static void json_str(char *dst, size_t sz, const char *src)
{
    size_t d = 0;
    if (sz == 0) return;
    while (*src && d + 4 < sz) {
        unsigned char c = (unsigned char)*src++;
        if (c == '"')  { dst[d++] = '\\'; dst[d++] = '"'; }
        else if (c == '\\') { dst[d++] = '\\'; dst[d++] = '\\'; }
        else if (c < 0x20) { /* skip control chars */ }
        else { dst[d++] = (char)c; }
    }
    dst[d] = '\0';
}

/* ------------------------------------------------------------------ */
/* FM discriminator (shared by APRS and CW warm-up)                   */
/* ------------------------------------------------------------------ */

struct fm_state {
    double prev_i, prev_q, prev_demod, dc_y;
    int have_prev;
    long long acc;
    int acc_n;
};

static void fm_state_init(struct fm_state *s) { memset(s, 0, sizeof(*s)); }

/*
 * Process one IQ pair through FM discriminator and decimation.
 * Returns 1 and fills *pcm when a decimated sample is ready.
 */
static int fm_process(struct fm_state *s, short i_raw, short q_raw,
                      int decimation, short *pcm)
{
    double i = (double)i_raw;
    double q = (double)q_raw;
    double demod = 0.0;

    if (s->have_prev) {
        double cross = s->prev_i * q - s->prev_q * i;
        double dot   = s->prev_i * i + s->prev_q * q;
        demod = atan2(cross, dot);
    }
    s->have_prev = 1;
    s->prev_i = i; s->prev_q = q;
    s->dc_y = (demod - s->prev_demod) + 0.995 * s->dc_y;
    s->prev_demod = demod;
    s->acc += (long long)lrint(s->dc_y * 12000.0);
    s->acc_n++;

    if (s->acc_n >= decimation) {
        long long v = s->acc / decimation;
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        *pcm = (short)v;
        s->acc = 0; s->acc_n = 0;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Goertzel tone detector                                              */
/* ------------------------------------------------------------------ */

struct goertzel {
    double coeff;
    double s0, s1;
    int n, N;
};

static void goertzel_init(struct goertzel *g, double freq, int fs, int block)
{
    g->coeff = 2.0 * cos(2.0 * M_PI * freq / fs);
    g->s0 = g->s1 = 0.0;
    g->n = 0; g->N = block;
}

/* Feed one sample; returns energy when block complete, else -1. */
static double goertzel_push(struct goertzel *g, double sample)
{
    double s2 = sample + g->coeff * g->s0 - g->s1;
    g->s1 = g->s0; g->s0 = s2;
    g->n++;
    if (g->n >= g->N) {
        double real = g->s0 - g->s1 * 0.5 * g->coeff;
        double imag = g->s1 * sin(2.0 * M_PI * (double)g->N / g->N);
        /* energy = real^2 + imag^2; simplified: */
        double energy = g->s0*g->s0 + g->s1*g->s1 - g->coeff*g->s0*g->s1;
        (void)real; (void)imag;
        g->s0 = g->s1 = 0.0; g->n = 0;
        return energy;
    }
    return -1.0;
}

/* ------------------------------------------------------------------ */
/* AX.25 HDLC framer                                                  */
/* ------------------------------------------------------------------ */

struct ax25_state {
    uint8_t shift;          /* 8-bit shift register for bit sync      */
    int ones;               /* consecutive 1 bits (bit stuffing count) */
    int in_frame;           /* inside a frame?                        */
    uint16_t crc;           /* running CRC-CCITT                      */
    uint8_t buf[AX25_MAX_FRAME];
    int len;
    int last_nrzi;          /* last NRZI level for differential decode */
};

static void ax25_init(struct ax25_state *s) { memset(s, 0, sizeof(*s)); }

static uint16_t crc_ccitt_update(uint16_t crc, uint8_t byte)
{
    uint8_t i;
    crc ^= (uint16_t)byte;
    for (i = 0; i < 8; i++) {
        if (crc & 1) crc = (crc >> 1) ^ 0x8408;
        else         crc >>= 1;
    }
    return crc;
}

/*
 * Feed one decoded bit (after NRZ-I differential decode).
 * Returns pointer to completed frame in s->buf (length in s->len)
 * or NULL if no frame ready yet.
 */
static const uint8_t *ax25_push_bit(struct ax25_state *s, int bit, int *frame_len)
{
    /* NRZ-I: received bit=0 means level change, bit=1 means no change */
    int nrzi_bit = (bit == s->last_nrzi) ? 1 : 0;
    s->last_nrzi = bit;

    /* Detect flag byte 0x7E = 01111110 in shift register */
    s->shift = (s->shift >> 1) | ((uint8_t)(nrzi_bit ? 0x80 : 0));

    if (s->shift == AX25_FLAG) {
        if (s->in_frame && s->len > 2) {
            /* Check CRC - last two bytes are CRC (little-endian) */
            if (s->len >= 2 && s->crc == 0xF0B8) {
                *frame_len = s->len - 2;  /* strip CRC bytes */
                return s->buf;
            }
        }
        /* Start new frame */
        s->in_frame = 1;
        s->len = 0;
        s->crc = 0xFFFF;
        s->ones = 0;
        return NULL;
    }

    if (!s->in_frame) return NULL;

    /* Bit stuffing: after 5 consecutive 1s, a 0 is inserted and discarded */
    if (nrzi_bit == 1) {
        s->ones++;
        if (s->ones > 5) { /* abort frame - 6+ ones = invalid */ s->in_frame = 0; return NULL; }
    } else {
        if (s->ones == 5) { s->ones = 0; return NULL; } /* stuffed 0, discard */
        s->ones = 0;
    }

    /* Accumulate bits into bytes (LSB first) */
    s->shift = s->shift; /* already shifted above */
    /* We need to track bit position within current byte separately */
    /* Re-accumulate: track byte assembly in parallel */
    /* (shift register already has the bit in MSB position from above) */

    /* Byte complete every 8 bits -- track separately */
    /* NOTE: we use s->ones==0 reset above, s->shift tracks last 8 bits */
    /* Check if we have 8 bits of data (not flag) */
    {
        /* We rely on the shift register containing the last 8 bits.
         * When it equals a complete byte pattern we store it.
         * This requires knowing bit alignment. Track with a separate counter. */
    }
    /* Simplified: store byte when shift reg doesn't match flag */
    /* This is handled by counting bits per byte below */

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Better AX.25 implementation using explicit bit counter             */
/* ------------------------------------------------------------------ */

struct hdlc_state {
    int ones;           /* consecutive 1-bits seen (for bit stuffing)  */
    int in_frame;       /* are we between flags?                       */
    int bit_pos;        /* bit position in current byte (0-7)          */
    uint8_t cur_byte;   /* byte being assembled                        */
    uint16_t crc;       /* running CRC-CCITT                          */
    uint8_t buf[AX25_MAX_FRAME];
    int len;
    /* NRZ-I state */
    int last_level;
    /* Flag detector using shift register */
    uint8_t flag_sr;
};

static void hdlc_init(struct hdlc_state *s) { memset(s, 0, sizeof(*s)); s->flag_sr = 0xFF; }

static const uint8_t *hdlc_push_bit(struct hdlc_state *s, int raw_bit, int *frame_len)
{
    /* NRZ-I differential decode: 1 = no transition, 0 = transition */
    int level = raw_bit;
    int data_bit = (level == s->last_level) ? 1 : 0;
    s->last_level = level;

    /* Flag detect via shift register (LSB first) */
    s->flag_sr = (s->flag_sr >> 1) | (data_bit ? 0x80 : 0);

    if (s->flag_sr == AX25_FLAG) {
        /* Flag received */
        if (s->in_frame && s->len >= 4 && s->bit_pos == 0) {
            /* Frame complete; last 2 bytes are CRC */
            if (s->crc == 0xF0B8) {
                *frame_len = s->len - 2;
                s->in_frame = 0;
                return s->buf;
            }
        }
        s->in_frame = 1;
        s->len = 0;
        s->bit_pos = 0;
        s->cur_byte = 0;
        s->crc = 0xFFFF;
        s->ones = 0;
        return NULL;
    }

    if (!s->in_frame) return NULL;

    /* Bit stuffing */
    if (data_bit == 1) {
        s->ones++;
        if (s->ones >= 6) { s->in_frame = 0; return NULL; } /* abort */
    } else {
        if (s->ones == 5) { s->ones = 0; return NULL; } /* stuffed 0 */
        s->ones = 0;
    }

    /* Assemble byte LSB first */
    if (data_bit) s->cur_byte |= (1 << s->bit_pos);
    s->bit_pos++;

    if (s->bit_pos == 8) {
        s->bit_pos = 0;
        if (s->len < AX25_MAX_FRAME) {
            s->buf[s->len] = s->cur_byte;
            if (s->len >= 2) {
                /* Update CRC on data bytes (not first two which are sync) */
                /* Actually CRC covers everything except the CRC bytes themselves */
            }
            s->crc = crc_ccitt_update(s->crc, s->cur_byte);
            s->len++;
        } else {
            s->in_frame = 0; /* frame too long */
        }
        s->cur_byte = 0;
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* AX.25 address and APRS parse                                       */
/* ------------------------------------------------------------------ */

static void ax25_decode_callsign(const uint8_t *addr, char *out, size_t sz)
{
    int i;
    size_t n = 0;
    for (i = 0; i < 6 && n + 1 < sz; i++) {
        char c = (char)((addr[i] >> 1) & 0x7F);
        if (c != ' ') out[n++] = c;
    }
    int ssid = (addr[6] >> 1) & 0x0F;
    if (ssid > 0 && n + 3 < sz) {
        out[n++] = '-';
        if (ssid >= 10) { out[n++] = '1'; out[n++] = '0' + (char)(ssid - 10); }
        else { out[n++] = '0' + (char)ssid; }
    }
    out[n] = '\0';
}

static int aprs_parse_latlon(const char *info, double *lat, double *lon)
{
    /* Handles: !DDMM.MMN/DDDMM.MMW> and /HHMMSS h DDMM.MMN/DDDMM.MMW> */
    const char *p = info;
    double lat_deg, lat_min, lon_deg, lon_min;
    char lat_ns, lon_ew;

    /* Skip to first digit that looks like lat */
    while (*p && !(*p >= '0' && *p <= '9')) p++;
    if (!*p) return 0;

    if (sscanf(p, "%2lf%5lf%c", &lat_deg, &lat_min, &lat_ns) < 3) return 0;
    p += 8; /* skip past DDMM.MMN */
    if (*p == '/' || *p == '\\') p++; /* symbol table */
    if (!*p) return 0;
    if (sscanf(p, "%3lf%5lf%c", &lon_deg, &lon_min, &lon_ew) < 3) return 0;

    *lat = lat_deg + lat_min / 60.0;
    if (lat_ns == 'S' || lat_ns == 's') *lat = -*lat;
    *lon = lon_deg + lon_min / 60.0;
    if (lon_ew == 'W' || lon_ew == 'w') *lon = -*lon;
    return 1;
}

static void write_aprs_frame(FILE *out, const uint8_t *frame, int flen)
{
    char ts[32], from[12], to[12], path[64], info[256], info_esc[512];
    char lat_field[32] = "", lon_field[32] = "";
    int n_addr, i;

    if (flen < 14) return; /* need at least dest + src */

    utc_now(ts, sizeof(ts));

    ax25_decode_callsign(frame, to, sizeof(to));       /* dest */
    ax25_decode_callsign(frame + 7, from, sizeof(from)); /* src */

    /* Repeater path */
    path[0] = '\0';
    n_addr = 2;
    while (n_addr * 7 < flen) {
        char rep[12];
        int offset = n_addr * 7;
        if (offset + 7 > flen) break;
        ax25_decode_callsign(frame + offset, rep, sizeof(rep));
        if (path[0]) { strncat(path, ",", sizeof(path) - strlen(path) - 1); }
        strncat(path, rep, sizeof(path) - strlen(path) - 1);
        if (frame[offset + 6] & 0x01) break; /* last address */
        n_addr++;
        if (n_addr > 8) break;
    }

    /* Info field: after address block (control + PID = 2 bytes) */
    int info_start = n_addr * 7 + 2;
    if (info_start >= flen) return;
    int info_len = flen - info_start;
    if (info_len <= 0) return;
    if (info_len > (int)sizeof(info) - 1) info_len = (int)sizeof(info) - 1;
    memcpy(info, frame + info_start, info_len);
    info[info_len] = '\0';

    /* Try to extract lat/lon */
    double lat = 0.0, lon = 0.0;
    if (aprs_parse_latlon(info, &lat, &lon)) {
        snprintf(lat_field, sizeof(lat_field), ",\"lat\":%.5f", lat);
        snprintf(lon_field, sizeof(lon_field), ",\"lon\":%.5f", lon);
    }

    json_str(info_esc, sizeof(info_esc), info);

    /* Build path JSON array */
    char path_json[128] = "[";
    if (path[0]) {
        char *tok, *save, pcopy[64];
        strncpy(pcopy, path, sizeof(pcopy) - 1); pcopy[sizeof(pcopy)-1] = '\0';
        tok = strtok_r(pcopy, ",", &save);
        int first = 1;
        while (tok) {
            char e[24]; json_str(e, sizeof(e), tok);
            if (!first) strncat(path_json, ",", sizeof(path_json) - strlen(path_json) - 1);
            strncat(path_json, "\"", sizeof(path_json) - strlen(path_json) - 1);
            strncat(path_json, e, sizeof(path_json) - strlen(path_json) - 1);
            strncat(path_json, "\"", sizeof(path_json) - strlen(path_json) - 1);
            first = 0;
            tok = strtok_r(NULL, ",", &save);
        }
    }
    strncat(path_json, "]", sizeof(path_json) - strlen(path_json) - 1);

    (void)i;
    fprintf(out,
        "{\"seq\":%d,\"time_utc\":\"%s\",\"type\":\"aprs\","
        "\"from\":\"%s\",\"to\":\"%s\",\"path\":%s,"
        "\"info\":\"%s\"%s%s}\n",
        ++g_seq, ts, from, to, path_json, info_esc,
        lat_field, lon_field);
    fflush(out);
}

/* ------------------------------------------------------------------ */
/* APRS decode loop                                                    */
/* ------------------------------------------------------------------ */

static void run_aprs(long long freq_hz, FILE *out)
{
    pid_t child = -1;
    FILE *pipe = NULL;
    struct fm_state fm;
    struct hdlc_state hdlc;
    struct goertzel g1200, g2200;
    unsigned char raw[IIO_BUFFER_SAMPS * 4];
    int last_bit = 0;

    fm_state_init(&fm);
    hdlc_init(&hdlc);

    /*
     * Goertzel block size: use 20 samples = 1 symbol period at 1200 baud / 24 kHz.
     * This gives one bit decision per block.
     */
    goertzel_init(&g1200, AFSK_MARK_HZ,  AUDIO_RATE, SAMPS_PER_SYM);
    goertzel_init(&g2200, AFSK_SPACE_HZ, AUDIO_RATE, SAMPS_PER_SYM);

    if (!configure_rx(freq_hz, IQ_SAMPLE_RATE)) {
        fprintf(stderr, "decoder: configure_rx failed for APRS\n");
        return;
    }

    pipe = start_iio_stream(&child, IIO_BUFFER_SAMPS);
    if (!pipe) { fprintf(stderr, "decoder: iio stream failed\n"); return; }

    while (g_running) {
        size_t got = fread(raw, 1, sizeof(raw), pipe);
        size_t pairs = got / 4;
        size_t idx;

        for (idx = 0; idx < pairs && g_running; idx++) {
            int off = (int)(idx * 4);
            short i_raw = (short)((uint16_t)raw[off]   | ((uint16_t)raw[off+1] << 8));
            short q_raw = (short)((uint16_t)raw[off+2] | ((uint16_t)raw[off+3] << 8));
            short pcm;

            if (!fm_process(&fm, i_raw, q_raw, FM_DECIMATION, &pcm)) continue;

            /* Feed audio into both Goertzel detectors */
            double s = (double)pcm / 32768.0;
            double e1 = goertzel_push(&g1200, s);
            double e2 = goertzel_push(&g2200, s);

            if (e1 >= 0.0) {
                /* Block complete: compare 1200 Hz vs 2200 Hz energy */
                int bit = (e1 > e2) ? 1 : 0;  /* 1 = mark = 1200 Hz */
                int frame_len = 0;
                const uint8_t *frame = hdlc_push_bit(&hdlc, bit, &frame_len);
                last_bit = bit;
                if (frame) write_aprs_frame(out, frame, frame_len);
            }
        }

        if (got == 0 && feof(pipe)) break;
    }
    (void)last_bit;

    fclose(pipe);
    if (child > 0) { kill(child, SIGTERM); waitpid(child, NULL, 0); }
}

/* ------------------------------------------------------------------ */
/* Morse decode table                                                  */
/* ------------------------------------------------------------------ */

typedef struct { const char *code; char ch; } morse_entry;

static const morse_entry MORSE[] = {
    {".-", 'A'}, {"-...", 'B'}, {"-.-.", 'C'}, {"-..", 'D'},
    {".", 'E'},  {"..-.", 'F'}, {"--.", 'G'},  {"....", 'H'},
    {"..", 'I'}, {".---", 'J'}, {"-.-", 'K'},  {".-..", 'L'},
    {"--", 'M'}, {"-.", 'N'},   {"---", 'O'},  {".--.", 'P'},
    {"--.-", 'Q'},{"-.", 'R'},  {"...", 'S'},   {"-", 'T'},
    {"..-", 'U'},{"..-", 'V'}, {".--", 'W'},   {"-..-", 'X'},
    {"-.--", 'Y'},{"--.", 'Z'},
    {"-----", '0'},{"----", '1'},{".---", '2'},{"..---", '3'},
    {"...--", '4'},{"....-", '5'},{".....", '6'},{"-....", '7'},
    {"--...", '8'},{"----..", '9'},
    {".-.-.-", '.'},{"--..--", ','},{"..--..", '?'},
    {NULL, 0}
};

static char morse_lookup(const char *code)
{
    const morse_entry *e;
    for (e = MORSE; e->code; e++) {
        if (strcmp(e->code, code) == 0) return e->ch;
    }
    return '?';
}

/* ------------------------------------------------------------------ */
/* CW decode loop                                                      */
/* ------------------------------------------------------------------ */

static void run_cw(long long freq_hz, FILE *out)
{
    pid_t child = -1;
    FILE *pipe = NULL;
    unsigned char raw[IIO_BUFFER_SAMPS * 4];

    /* Envelope state */
    double env_smooth = 0.0;
    double threshold = 0.0;
    double env_max = 0.0;
    long long env_acc = 0;
    int env_n = 0;

    /* Dit timing (adaptive) */
    int dit_samples = (CW_ENVELOPE_RATE * CW_DIT_MS_INIT) / 1000;
    int on_count = 0, off_count = 0;
    int in_tone = 0;

    /* Morse buffer */
    char morse_buf[16];
    int morse_len = 0;
    char decoded_text[256];
    int text_len = 0;

    if (!configure_rx(freq_hz, IQ_SAMPLE_RATE)) {
        fprintf(stderr, "decoder: configure_rx failed for CW\n");
        return;
    }

    pipe = start_iio_stream(&child, IIO_BUFFER_SAMPS);
    if (!pipe) { fprintf(stderr, "decoder: iio stream failed\n"); return; }

    /* Output file for CW text accumulation */
    char ts[32];
    utc_now(ts, sizeof(ts));

    while (g_running) {
        size_t got = fread(raw, 1, sizeof(raw), pipe);
        size_t pairs = got / 4;
        size_t idx;

        for (idx = 0; idx < pairs && g_running; idx++) {
            int off = (int)(idx * 4);
            short i_raw = (short)((uint16_t)raw[off]   | ((uint16_t)raw[off+1] << 8));
            short q_raw = (short)((uint16_t)raw[off+2] | ((uint16_t)raw[off+3] << 8));

            /* Magnitude envelope */
            double mag = sqrt((double)i_raw*(double)i_raw + (double)q_raw*(double)q_raw);
            env_smooth = 0.95 * env_smooth + 0.05 * mag;

            /* Decimation: one decision every CW_DECIMATION samples */
            env_acc += (long long)env_smooth;
            env_n++;
            if (env_n < CW_DECIMATION) continue;

            double env = (double)(env_acc / env_n);
            env_acc = 0; env_n = 0;

            /* Adaptive threshold: 40% of recent peak */
            if (env > env_max) env_max = env;
            else env_max *= 0.9999;
            threshold = env_max * 0.40;

            int tone_on = (env > threshold && env_max > 100.0);

            if (tone_on && !in_tone) {
                /* Rising edge */
                in_tone = 1;
                /* Classify silence before this tone */
                if (off_count > 0) {
                    if (off_count > dit_samples * 5) {
                        /* Word space */
                        if (text_len > 0) decoded_text[text_len++] = ' ';
                    } else if (off_count > dit_samples * 2) {
                        /* Letter space: emit decoded char */
                        if (morse_len > 0) {
                            morse_buf[morse_len] = '\0';
                            char c = morse_lookup(morse_buf);
                            if (text_len < (int)sizeof(decoded_text) - 1)
                                decoded_text[text_len++] = c;
                            morse_len = 0;
                        }
                    }
                }
                off_count = 0;
                on_count = 0;
            } else if (!tone_on && in_tone) {
                /* Falling edge: classify dit or dah */
                in_tone = 0;
                char element = (on_count > dit_samples * 2) ? '-' : '.';
                if (morse_len < (int)sizeof(morse_buf) - 1)
                    morse_buf[morse_len++] = element;

                /* Adaptive dit timing */
                if (element == '.') {
                    dit_samples = (dit_samples * 7 + on_count) / 8;
                }

                on_count = 0;
                off_count = 0;
            } else if (in_tone) {
                on_count++;
                /* Long key-down = word separator (>7 dits): emit word */
                if (on_count > dit_samples * 10) {
                    in_tone = 0; on_count = 0;
                    morse_len = 0; /* abort malformed sequence */
                }
            } else {
                off_count++;
                /* Flush decoded text after a long pause */
                if (off_count == dit_samples * 7 + 1 && text_len > 0) {
                    decoded_text[text_len] = '\0';
                    utc_now(ts, sizeof(ts));
                    char text_esc[512];
                    json_str(text_esc, sizeof(text_esc), decoded_text);
                    fprintf(out,
                        "{\"seq\":%d,\"time_utc\":\"%s\",\"type\":\"cw\","
                        "\"text\":\"%s\"}\n",
                        ++g_seq, ts, text_esc);
                    fflush(out);
                    text_len = 0;
                }
            }
        }

        if (got == 0 && feof(pipe)) break;
    }

    /* Flush remaining text */
    if (text_len > 0) {
        decoded_text[text_len] = '\0';
        utc_now(ts, sizeof(ts));
        char text_esc[512];
        json_str(text_esc, sizeof(text_esc), decoded_text);
        fprintf(out,
            "{\"seq\":%d,\"time_utc\":\"%s\",\"type\":\"cw\","
            "\"text\":\"%s\"}\n",
            ++g_seq, ts, text_esc);
        fflush(out);
    }

    fclose(pipe);
    if (child > 0) { kill(child, SIGTERM); waitpid(child, NULL, 0); }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    long long freq_hz = 0;
    const char *mode = NULL;
    const char *output_path = NULL;
    FILE *out = NULL;
    int i;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--freq-hz") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            freq_hz = strtoll(argv[++i], &end, 10);
            if (errno || !end || *end) {
                fprintf(stderr, "Invalid --freq-hz\n"); return 2;
            }
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else {
            fprintf(stderr, "Usage: %s --freq-hz <hz> --mode <aprs|cw> --output <path>\n", argv[0]);
            return 2;
        }
    }

    if (freq_hz < PLUTO_MIN_HZ || freq_hz > PLUTO_MAX_HZ || !mode || !output_path) {
        fprintf(stderr, "Usage: %s --freq-hz <hz> --mode <aprs|cw> --output <path>\n", argv[0]);
        return 2;
    }

    out = fopen(output_path, "a");
    if (!out) { perror("fopen"); return 1; }
    setvbuf(out, NULL, _IOLBF, 0);

    if (strcmp(mode, "aprs") == 0) {
        run_aprs(freq_hz, out);
    } else if (strcmp(mode, "cw") == 0) {
        run_cw(freq_hz, out);
    } else {
        fprintf(stderr, "Unknown mode: %s (supported: aprs, cw)\n", mode);
        fclose(out);
        return 2;
    }

    fclose(out);
    return 0;
}

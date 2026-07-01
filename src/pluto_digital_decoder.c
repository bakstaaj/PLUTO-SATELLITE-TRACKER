/*
 * pluto_digital_decoder.c
 *
 * Digital signal decoder for Pluto Plus SDR.
 * Supported modes: aprs, cw, gfsk
 *
 * Usage:
 *   pluto_digital_decoder --freq-hz <hz> --mode <aprs|cw|gfsk> --output <path>
 *                         [--baud <rate>]
 *
 * Captures IQ from iio_readdev, demodulates, and writes NDJSON frames
 * (one JSON object per line) to the output file.
 *
 * APRS: FM demod -> AFSK Bell 202 -> NRZ-I -> AX.25 -> APRS parse
 * CW:   IQ magnitude envelope -> dit/dah timing -> Morse -> text
 * GFSK: FM demod (25x decim) -> DC block -> MA filter -> Gardner TED ->
 *       NRZ-I -> AX.25 (9600/4800/2400/1200 baud via --baud, default 9600)
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <errno.h>
#include <math.h>
#ifndef CW_MIXER_PI
#define CW_MIXER_PI 3.14159265358979323846
#endif
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
#define DEFAULT_RX_CHANNEL 1            /* Pluto Plus RX1               */
#define DEFAULT_OFFSET_HZ  0.0          /* Backward compatible default  */
#define DEFAULT_RX_BW_HZ   200000       /* APRS/CW/GFSK narrowband      */

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
/* GFSK constants (G3RUH / AX.25 9600 baud, configurable)             */
/* ------------------------------------------------------------------ */

/*
 * Decimation 25× gives 96 000 sps audio, which divides evenly into all
 * common satellite baud rates:
 *   9600 baud → 10 sps/sym   (most CubeSats, G3RUH standard)
 *   4800 baud → 20 sps/sym
 *   2400 baud → 40 sps/sym
 *   1200 baud → 80 sps/sym
 */
#define GFSK_DECIM       25
#define GFSK_AUDIO_RATE  (IQ_SAMPLE_RATE / GFSK_DECIM)   /* 96 000 Hz */

/*
 * Gardner TED loop gain.  The timing error is normalised by signal
 * amplitude, so e ∈ [−2, +2].
 *
 * Convergence: starting from worst-case half-symbol offset (sps/2 = 5
 * samples), locking to within 0.5 samples requires N transitions where
 * (1-gain)^N = 0.1.  At gain=0.05: N≈45 transitions (≈22 flag bytes —
 * too long for an 8-byte preamble).  At gain=0.10: N≈22 transitions
 * (≈11 flag bytes), which fits within the 24-byte test_gen preamble
 * (≈48 usable transitions after 1 ms DC-block convergence).  For real
 * satellites with 50+ flag preambles this is also fine.
 */
#define GFSK_TED_GAIN    0.10

/* ------------------------------------------------------------------ */
/* CW constants                                                        */
/* ------------------------------------------------------------------ */

#define CW_DECIMATION    200           /* IQ -> envelope decimation    */
#define CW_ENVELOPE_RATE (IQ_SAMPLE_RATE / CW_DECIMATION)  /* 12 kHz  */
#define CW_WPM_INIT      20            /* starting WPM estimate        */
#define CW_DIT_MS_INIT   (1200 / CW_WPM_INIT)  /* dit length ms       */
#define CW_WPM_MIN       5             /* slow satellite CW beacon    */
#define CW_WPM_MAX       40            /* fast practical CW beacon    */
#define DEFAULT_CW_MIN_SPAN 60.0       /* mixed envelope signal gate   */
#define DEFAULT_CW_MIN_PEAK 90.0       /* mixed envelope peak gate     */
#define DEFAULT_CW_OPEN_FRAC 0.55      /* Schmitt open fraction        */
#define DEFAULT_CW_CLOSE_FRAC 0.32     /* Schmitt close fraction       */
#define DEFAULT_CW_MIN_ELEMENT_FRAC 0.40 /* min valid element in dits   */
#define DEFAULT_CW_MIX_SIGN 1          /* +1 or -1 IF mixer sign       */

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_running = 1;
static int g_seq = 0;
static int g_debug = 0;   /* --debug: verbose stderr diagnostics */

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

static void configure_rx_scan_elements(int rx_channel)
{
    /* Pluto Plus 2R2T exposes RX1 as voltage0/voltage1 and RX2 as
     * voltage2/voltage3 on cf-ad9361-lpc.  The decoder consumes exactly
     * one complex stream, so explicitly enable the selected I/Q pair and
     * disable the other pair before starting iio_readdev. */
    int dev, ch;
    char path[128];
    FILE *f;

    for (dev = 0; dev <= 9; dev++) {
        for (ch = 0; ch <= 3; ch++) {
            int want = 0;
            if (rx_channel == 1 && (ch == 0 || ch == 1)) want = 1;
            if (rx_channel == 2 && (ch == 2 || ch == 3)) want = 1;

            snprintf(path, sizeof(path),
                "/sys/bus/iio/devices/iio:device%d/scan_elements/in_voltage%d_en",
                dev, ch);
            f = fopen(path, "w");
            if (f) {
                fprintf(f, "%d\n", want);
                fclose(f);
                if (g_debug) {
                    fprintf(stderr, "rx: iio:device%d voltage%d_en=%d\n", dev, ch, want);
                }
            }
        }
    }
}

static int configure_rx(long long rx_lo_hz, int sample_rate_hz, int rx_channel, int bandwidth_hz)
{
    char cmd[512];
    int r;
    const char *phy_voltage = (rx_channel == 2) ? "voltage1" : "voltage0";

    if (rx_lo_hz < PLUTO_MIN_HZ || rx_lo_hz > PLUTO_MAX_HZ) return 0;
    if (rx_channel != 1 && rx_channel != 2) return 0;
    if (bandwidth_hz < 200000) bandwidth_hz = 200000;

    snprintf(cmd, sizeof(cmd),
        "/usr/bin/iio_attr -u local: -c ad9361-phy %s sampling_frequency %d >/dev/null 2>&1",
        phy_voltage, sample_rate_hz);
    r = system(cmd); if (r == -1 || !WIFEXITED(r) || WEXITSTATUS(r) != 0) return 0;

    snprintf(cmd, sizeof(cmd),
        "/usr/bin/iio_attr -u local: -c ad9361-phy %s rf_bandwidth %d >/dev/null 2>&1",
        phy_voltage, bandwidth_hz);
    r = system(cmd); if (r == -1 || !WIFEXITED(r) || WEXITSTATUS(r) != 0) return 0;

    snprintf(cmd, sizeof(cmd),
        "/usr/bin/iio_attr -u local: -c ad9361-phy %s gain_control_mode slow_attack >/dev/null 2>&1",
        phy_voltage);
    r = system(cmd); if (r == -1 || !WIFEXITED(r) || WEXITSTATUS(r) != 0) return 0;

    if (!write_rx_lo(rx_lo_hz)) return 0;

    configure_rx_scan_elements(rx_channel);

    fprintf(stderr,
        "decoder: configure_rx rx_lo=%lld rx_channel=%d phy=%s sample_rate=%d bandwidth=%d\n",
        rx_lo_hz, rx_channel, phy_voltage, sample_rate_hz, bandwidth_hz);

    return 1;
}

static FILE *start_iio_stream(pid_t *child_pid, int buffer_samps, int rx_channel)
{
    int fds[2];
    pid_t pid;
    char buf[32];
    const char *v_i = (rx_channel == 2) ? "voltage2" : "voltage0";
    const char *v_q = (rx_channel == 2) ? "voltage3" : "voltage1";

    if (pipe(fds) != 0) return NULL;
    pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return NULL; }
    if (pid == 0) {
        snprintf(buf, sizeof(buf), "%d", buffer_samps);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]); close(fds[1]);
        execl("/usr/bin/iio_readdev", "iio_readdev",
              "-u", "local:", "-b", buf, "-s", "0",
              "cf-ad9361-lpc", v_i, v_q, (char *)NULL);
        _exit(127);
    }
    fprintf(stderr, "decoder: iio_readdev cf-ad9361-lpc %s %s\n", v_i, v_q);
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
    double prev_i, prev_q;
    int have_prev;
    long long acc;
    int acc_n;
};

static void fm_state_init(struct fm_state *s) { memset(s, 0, sizeof(*s)); }

/*
 * Process one IQ pair through FM discriminator and decimation.
 * Returns 1 and fills *pcm when a decimated sample is ready.
 *
 * atan2(cross, dot) gives the instantaneous phase increment between
 * consecutive IQ samples, which is directly proportional to the
 * instantaneous frequency deviation — that IS the FM demodulated audio.
 * Accumulate it directly; no further differentiation.
 *
 * DC offset from a mistuned carrier is constant and doesn't affect the
 * Goertzel energy comparison at 1200 Hz vs 2200 Hz, so no DC block needed.
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

    /*
     * Scale factor: demod is ~2π*f_dev/Fs radians per sample (tiny).
     * Multiply by a large constant so the decimated average stays in
     * short range. The Goertzel comparison is relative so exact scale
     * doesn't matter — just needs to avoid acc overflow over 100 samples.
     * 2π * 3000 Hz / 2400000 sps * 100 samples * 8192 ≈ 6434 — well within range.
     */
    s->acc += (long long)lrint(demod * 8192.0);
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
        /* Flag received.
         *
         * When in_frame=1 the 7 bits of the closing flag that precede the
         * 8th (trigger) bit are processed through byte-assembly, advancing
         * bit_pos from 0 to 7.  So a correctly-terminated frame always
         * arrives here with bit_pos==7, not 0.
         */
        if (g_debug && (s->in_frame || s->len || s->bit_pos || s->ones || s->crc)) {
            fprintf(stderr, "hdlc: FLAG in_frame=%d len=%d bit_pos=%d ones=%d crc=0x%04X\n",
                    s->in_frame, s->len, s->bit_pos, s->ones, s->crc);
        }
        if (s->in_frame && s->len >= 4 && s->bit_pos == 7) {
            /* Frame complete; last 2 bytes are CRC */
            if (g_debug) {
                fprintf(stderr, "hdlc: FRAME_CANDIDATE len=%d crc=0x%04X (want 0xF0B8)\n",
                        s->len, s->crc);
            }
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
        if (s->ones > 6) { s->in_frame = 0; return NULL; } /* abort: 7+ ones = error */
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

static void run_aprs(long long freq_hz, double offset_hz, int rx_channel, int bandwidth_hz, FILE *out)
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

    long long rx_lo_hz = freq_hz - (long long)llround(offset_hz);
    if (!configure_rx(rx_lo_hz, IQ_SAMPLE_RATE, rx_channel, bandwidth_hz)) {
        fprintf(stderr, "decoder: configure_rx failed for APRS\n");
        return;
    }
    fprintf(stderr, "decoder: APRS downlink=%lld offset_hz=%.1f rx_lo=%lld rx_channel=%d\n",
            freq_hz, offset_hz, rx_lo_hz, rx_channel);

    pipe = start_iio_stream(&child, IIO_BUFFER_SAMPS, rx_channel);
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
    {".-",   'A'}, {"-...", 'B'}, {"-.-.", 'C'}, {"-..", 'D'},
    {".",    'E'}, {"..-.", 'F'}, {"--.",  'G'}, {"....", 'H'},
    {"..",   'I'}, {".---", 'J'}, {"-.-",  'K'}, {".-..", 'L'},
    {"--",   'M'}, {"-.",   'N'}, {"---",  'O'}, {".--.", 'P'},
    {"--.-", 'Q'}, {".-.",  'R'}, {"...",  'S'}, {"-",    'T'},
    {"..-",  'U'}, {"...-", 'V'}, {".--",  'W'}, {"-..-", 'X'},
    {"-.--", 'Y'}, {"--..", 'Z'},
    {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'},
    {"....-", '4'}, {".....", '5'}, {"-....", '6'}, {"--...", '7'},
    {"---..", '8'}, {"----.", '9'},
    {".-.-.-", '.'}, {"--..--", ','}, {"..--..", '?'},
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

static void run_cw(long long freq_hz, double offset_hz, int rx_channel, int bandwidth_hz, double cw_wpm, double cw_min_span, double cw_min_peak, double cw_open_frac, double cw_close_frac, double cw_min_element_frac, int cw_mix_sign, double cw_abs_low, double cw_abs_high, FILE *out)
{
    pid_t child = -1;
    FILE *pipe = NULL;
    unsigned char raw[IIO_BUFFER_SAMPS * 4];

    /* CW offset mixer.
     *
     * The receiver tunes RX LO to freq_hz - offset_hz, so the wanted CW
     * carrier appears at +offset_hz in the complex baseband.  Mix that IF
     * carrier back to DC before taking magnitude.  This prevents raw IQ
     * magnitude beating against DC/LO leakage from fragmenting key-down
     * intervals into many ~40-sample glitches.
     */
    double cw_phase = 0.0;
    double cw_phase_inc = 2.0 * CW_MIXER_PI * offset_hz * (double)cw_mix_sign / (double)CW_ENVELOPE_RATE;
    double cw_i_lp = 0.0;
    double cw_q_lp = 0.0;
    int cw_lp_ready = 0;
    const double cw_lp_alpha = exp(-2.0 * CW_MIXER_PI * 600.0 / (double)CW_ENVELOPE_RATE);

    /* Envelope state.
     *
     * Track floor and peak separately, then use hysteresis and edge debounce.
     * This prevents RF/AGC flutter from splitting dashes into many dots.
     */
    double env_smooth = 0.0;
    double threshold = 0.0;
    double env_floor = 0.0;
    double env_peak = 0.0;
    int have_env = 0;
    long long env_acc = 0;
    int env_n = 0;

    /* Dit timing.
     * If --cw-wpm is supplied, lock timing to the requested WPM.  If not,
     * start at CW_WPM_INIT and allow conservative adaptation with clamps.
     */
    int cw_locked_wpm = (cw_wpm > 0.0);
    int dit_samples = cw_locked_wpm
        ? (int)lrint((double)CW_ENVELOPE_RATE * (1200.0 / cw_wpm) / 1000.0)
        : (CW_ENVELOPE_RATE * CW_DIT_MS_INIT) / 1000;
    const int dit_min_samples = (int)lrint((double)CW_ENVELOPE_RATE * (1200.0 / CW_WPM_MAX) / 1000.0);
    const int dit_max_samples = (int)lrint((double)CW_ENVELOPE_RATE * (1200.0 / CW_WPM_MIN) / 1000.0);
    if (dit_samples < dit_min_samples) dit_samples = dit_min_samples;
    if (dit_samples > dit_max_samples) dit_samples = dit_max_samples;

    int on_count = 0, off_count = 0;
    int in_tone = 0;

    /* WPM-relative timing filters.
     * Anything shorter than 1/4 dit is not a valid CW element.  This is
     * critical with Pluto/AD9361 AGC ripple: without this guard, short
     * amplitude fragments become bogus dots and corrupt the Morse buffer.
     */
    int min_element_samples = (int)lrint((double)dit_samples * cw_min_element_frac);
    int dash_threshold_samples = (dit_samples * 22) / 10;  /* 2.2 dits */
    int letter_gap_samples = (dit_samples * 22) / 10;      /* >=2.2 dits */
    int word_gap_samples = (dit_samples * 55) / 10;        /* >=5.5 dits */
    /* Debounce raw threshold crossings before changing CW key state. */
    int raw_on_count = 0;
    int raw_off_count = 0;
    const int edge_debounce = CW_ENVELOPE_RATE / 500; /* 2 ms */
    if (min_element_samples < edge_debounce) min_element_samples = edge_debounce;
    long long dbg_env_ticks = 0;
    long long dbg_on_ticks = 0;
    long long dbg_sig_ticks = 0;
    double dbg_env_min = 1.0e30;
    double dbg_env_max = 0.0;

    /* Morse buffer */
    char morse_buf[16];
    int morse_len = 0;
    char decoded_text[256];
    int text_len = 0;

    long long rx_lo_hz = freq_hz - (long long)llround(offset_hz);
    if (!configure_rx(rx_lo_hz, IQ_SAMPLE_RATE, rx_channel, bandwidth_hz)) {
        fprintf(stderr, "decoder: configure_rx failed for CW\n");
        return;
    }
    fprintf(stderr, "decoder: CW downlink=%lld offset_hz=%.1f rx_lo=%lld rx_channel=%d\n",
            freq_hz, offset_hz, rx_lo_hz, rx_channel);
    fprintf(stderr,
            "decoder: CW timing cw_wpm=%.1f locked=%d dit_samples=%d min=%d max=%d min_element=%d dash_th=%d letter_gap=%d word_gap=%d\n",
            cw_wpm, cw_locked_wpm, dit_samples, dit_min_samples, dit_max_samples,
            min_element_samples, dash_threshold_samples, letter_gap_samples, word_gap_samples);
    fprintf(stderr,
            "decoder: CW mixer offset_hz=%.1f mix_sign=%d phase_inc=%.9f mixer_rate=%d lp_alpha=%.9f\n",
            offset_hz, cw_mix_sign, cw_phase_inc, CW_ENVELOPE_RATE, cw_lp_alpha);
    fprintf(stderr,
            "decoder: CW signal gate min_span=%.2f min_peak=%.2f open_frac=%.2f close_frac=%.2f min_element_frac=%.2f\n",
            cw_min_span, cw_min_peak, cw_open_frac, cw_close_frac, cw_min_element_frac);

    pipe = start_iio_stream(&child, IIO_BUFFER_SAMPS, rx_channel);
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

            /* Offset-aware CW envelope.
             *
             * Mix the +offset_hz IF carrier down to DC, then low-pass the
             * complex baseband before magnitude detection.  This keeps a
             * steady key-down carrier steady in the envelope detector.
             */
            double c = cos(cw_phase);
            double sn = sin(cw_phase);
            double mix_i = (double)i_raw * c + (double)q_raw * sn;
            double mix_q = (double)q_raw * c - (double)i_raw * sn;

            cw_phase += cw_phase_inc;
            if (cw_phase >  CW_MIXER_PI) cw_phase -= 2.0 * CW_MIXER_PI;
            if (cw_phase < -CW_MIXER_PI) cw_phase += 2.0 * CW_MIXER_PI;

            if (!cw_lp_ready) {
                cw_i_lp = mix_i;
                cw_q_lp = mix_q;
                cw_lp_ready = 1;
            } else {
                cw_i_lp = cw_lp_alpha * cw_i_lp + (1.0 - cw_lp_alpha) * mix_i;
                cw_q_lp = cw_lp_alpha * cw_q_lp + (1.0 - cw_lp_alpha) * mix_q;
            }

            double mag = sqrt(cw_i_lp * cw_i_lp + cw_q_lp * cw_q_lp);
            if (!have_env) env_smooth = mag;
            else env_smooth = 0.98 * env_smooth + 0.02 * mag;

            /* Decimation: one decision every CW_DECIMATION samples */
            env_acc += (long long)env_smooth;
            env_n++;
            if (env_n < CW_DECIMATION) continue;

            double env = (double)(env_acc / env_n);
            env_acc = 0; env_n = 0;

            /* Adaptive floor/peak threshold with hysteresis.
             *
             * Floor updates mostly while key-up.  Peak tracks key-down signal
             * and decays slowly.  A valid key requires enough peak/floor
             * separation to suppress pre-TX noise and weak envelope bumps.
             */
            if (!have_env) {
                env_floor = env;
                env_peak = env;
                have_env = 1;
            }

            if (!in_tone) {
                if (env < env_floor) {
                    env_floor = 0.90 * env_floor + 0.10 * env;      /* down fast */
                } else {
                    env_floor = 0.9995 * env_floor + 0.0005 * env;  /* up slow */
                }
            }
            if (env > env_peak) env_peak = env;
            else env_peak = 0.9995 * env_peak + 0.0005 * env_floor;
            if (env_peak < env_floor + 0.001) env_peak = env_floor + 0.001;

            double span = env_peak - env_floor;
            int use_abs_threshold = (cw_abs_high > 0.0 && cw_abs_low >= 0.0 && cw_abs_low < cw_abs_high);
            double high_threshold = use_abs_threshold ? cw_abs_high : (env_floor + cw_open_frac * span);
            double low_threshold  = use_abs_threshold ? cw_abs_low  : (env_floor + cw_close_frac * span);
            threshold = in_tone ? low_threshold : high_threshold;

            int signal_present = use_abs_threshold ? 1 : (span >= cw_min_span && env_peak >= cw_min_peak);
            int raw_tone_on = 0;
            if (signal_present) {
                raw_tone_on = in_tone ? (env > low_threshold) : (env > high_threshold);
            }

            if (g_debug) {
                dbg_env_ticks++;
                if (env < dbg_env_min) dbg_env_min = env;
                if (env > dbg_env_max) dbg_env_max = env;
                if (signal_present) dbg_sig_ticks++;
                if (in_tone || raw_tone_on) dbg_on_ticks++;
                if (dbg_env_ticks >= CW_ENVELOPE_RATE) {
                    fprintf(stderr,
                        "cw: env min=%.2f max=%.2f floor=%.2f peak=%.2f span=%.2f th=%.2f sig=%lld/%lld on=%lld/%lld dit=%d debounce=%d abs_low=%.2f abs_high=%.2f\n",
                        dbg_env_min, dbg_env_max, env_floor, env_peak, span, threshold,
                        dbg_sig_ticks, dbg_env_ticks, dbg_on_ticks, dbg_env_ticks,
                        dit_samples, edge_debounce, cw_abs_low, cw_abs_high);
                    dbg_env_ticks = 0;
                    dbg_on_ticks = 0;
                    dbg_sig_ticks = 0;
                    dbg_env_min = 1.0e30;
                    dbg_env_max = 0.0;
                }
            }

            if (raw_tone_on) {
                raw_on_count++;
                raw_off_count = 0;
            } else {
                raw_off_count++;
                raw_on_count = 0;
            }

            int tone_on = in_tone;
            if (!in_tone && raw_on_count >= edge_debounce) tone_on = 1;
            if ( in_tone && raw_off_count >= edge_debounce) tone_on = 0;

            if (tone_on && !in_tone) {
                /* Rising edge */
                in_tone = 1;
                /* Classify silence before this tone */
                if (off_count > 0) {
                    if (off_count >= word_gap_samples) {
                        /* Word space */
                        if (morse_len > 0) {
                            morse_buf[morse_len] = '\0';
                            char c = morse_lookup(morse_buf);
                            if (text_len < (int)sizeof(decoded_text) - 1)
                                decoded_text[text_len++] = c;
                            if (g_debug)
                                fprintf(stderr, "cw: emit char '%c' code=%s gap=%d(word)\n", c, morse_buf, off_count);
                            morse_len = 0;
                        }
                        if (text_len > 0 && text_len < (int)sizeof(decoded_text) - 1)
                            decoded_text[text_len++] = ' ';
                    } else if (off_count >= letter_gap_samples) {
                        /* Letter space: emit decoded char */
                        if (morse_len > 0) {
                            morse_buf[morse_len] = '\0';
                            char c = morse_lookup(morse_buf);
                            if (text_len < (int)sizeof(decoded_text) - 1)
                                decoded_text[text_len++] = c;
                            if (g_debug)
                                fprintf(stderr, "cw: emit char '%c' code=%s gap=%d(letter)\n", c, morse_buf, off_count);
                            morse_len = 0;
                        }
                    }
                }
                off_count = 0;
                on_count = 0;
            } else if (!tone_on && in_tone) {
                /* Falling edge: classify dit or dah.
                 * Ignore very short key-down glitches caused by threshold
                 * chatter; they are not valid CW elements.
                 */
                in_tone = 0;
                if (on_count >= min_element_samples) {
                    char element = (on_count >= dash_threshold_samples) ? '-' : '.';
                    if (morse_len < (int)sizeof(morse_buf) - 1)
                        morse_buf[morse_len++] = element;
                    if (g_debug)
                        fprintf(stderr, "cw: element %c on_count=%d dit=%d morse_len=%d\n",
                                element, on_count, dit_samples, morse_len);

                    /* Adaptive dit timing from clean dots only.
                     * Do not adapt when --cw-wpm locked the timing, and never
                     * allow a glitch-sized dot to collapse the decoder timing.
                     */
                    if (!cw_locked_wpm && element == '.' &&
                        on_count >= dit_min_samples && on_count <= dit_max_samples) {
                        dit_samples = (dit_samples * 7 + on_count) / 8;
                        if (dit_samples < dit_min_samples) dit_samples = dit_min_samples;
                        if (dit_samples > dit_max_samples) dit_samples = dit_max_samples;
                        min_element_samples = (int)lrint((double)dit_samples * cw_min_element_frac);
                        dash_threshold_samples = (dit_samples * 22) / 10;
                        letter_gap_samples = (dit_samples * 22) / 10;
                        word_gap_samples = (dit_samples * 55) / 10;
                        if (min_element_samples < edge_debounce) min_element_samples = edge_debounce;
                    }
                } else if (g_debug) {
                    fprintf(stderr, "cw: ignored short on glitch count=%d min_element=%d\n",
                            on_count, min_element_samples);
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
                if (off_count == word_gap_samples + dit_samples && text_len > 0) {
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

    /*
     * CW EOF final flush:
     * If the stream ends while a Morse element or decoded word is pending,
     * emit it instead of requiring additional trailing silence. This prevents
     * short beacon/test captures from losing the final character.
     */
    if (morse_len > 0) {
        morse_buf[morse_len] = '\0';
        char c = morse_lookup(morse_buf);
        if (text_len < (int)sizeof(decoded_text) - 1)
            decoded_text[text_len++] = c;
        if (g_debug)
            fprintf(stderr, "cw: emit char '%c' code=%s gap=eof-flush\n", c, morse_buf);
        morse_len = 0;
    }
    if (text_len > 0) {
        decoded_text[text_len] = '\0';
        char ts[32];
        time_t now = time(NULL);
        struct tm tm_utc;
        gmtime_r(&now, &tm_utc);
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
        fprintf(out, "{\"seq\":%lld,\"time_utc\":\"%s\",\"type\":\"cw\",\"text\":\"%s\"}\n",
                ++g_seq, ts, decoded_text);
        fflush(out);
        if (g_debug)
            fprintf(stderr, "cw: emit text reason=eof-flush text=%s\n", decoded_text);
        text_len = 0;
        decoded_text[0] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* G3RUH descrambler                                                  */
/* ------------------------------------------------------------------ */

/*
 * G3RUH self-synchronizing scrambler/descrambler.
 * Polynomial: g(x) = 1 + x^12 + x^17
 *
 * Descramble: out[n] = in[n] XOR in[n-12] XOR in[n-17]
 *
 * Self-synchronizing means no framing alignment is needed — the
 * descrambler locks within 17 bits of the start of a valid signal.
 * This is the de facto standard for 9600 baud satellite packet radio
 * (G3RUH modem, JAMSAT standard, used by most CubeSat downlinks).
 *
 * The shift register holds the last 17 *received* (scrambled) bits.
 * Before shifting in bit n:
 *   sr[11] = received bit n-12
 *   sr[16] = received bit n-17
 */

struct g3ruh_state {
    uint32_t sr;   /* 17-bit shift register of received scrambled bits */
};

static void g3ruh_init(struct g3ruh_state *s) { s->sr = 0; }

static int g3ruh_descramble(struct g3ruh_state *s, int bit)
{
    int out = bit ^ ((int)(s->sr >> 11) & 1) ^ ((int)(s->sr >> 16) & 1);
    s->sr = ((s->sr << 1) | (unsigned)(bit & 1)) & 0x1FFFFU;
    return out;
}

/* ------------------------------------------------------------------ */
/* GFSK decode loop (G3RUH / AX.25 9600 baud, configurable)          */
/* ------------------------------------------------------------------ */

/*
 * FM demod → DC block → moving-average matched filter →
 * Gardner TED clock recovery → NRZ-I HDLC → AX.25 frame output.
 *
 * Decimation: GFSK_DECIM=25 → 96 000 sps audio.
 * sps = 96000 / baud  (10 at 9600, 20 at 4800, 40 at 2400, 80 at 1200)
 *
 * The moving-average filter integrates FM energy over exactly one symbol
 * period.  This is the integrate-and-dump matched filter for rectangular
 * FSK symbols and a good approximation for GFSK (BT=0.5) where the
 * Gaussian shaping spreads energy to ~1.5 symbol widths.
 *
 * Gardner TED: at every symbol boundary we have:
 *   e = x_mid × (sign(x_now) − sign(x_prev))
 * Normalised by signal amplitude so loop gain is data-independent.
 * When consecutive bits are equal (no transition) sign difference = 0,
 * so TED is quiescent — it only adjusts on transitions, as intended.
 */
static void run_gfsk(long long freq_hz, double offset_hz, int rx_channel, int bandwidth_hz, int baud, int use_g3ruh, FILE *out)
{
    pid_t child = -1;
    FILE *iq_pipe = NULL;
    struct fm_state fm;
    struct hdlc_state hdlc;
    struct g3ruh_state g3ruh;
    unsigned char raw[IIO_BUFFER_SAMPS * 4];

    int sps;
    double *ma_buf;
    int    ma_idx;
    double ma_sum;

    double dc_avg;
    double phi;
    double filt_prev, filt_mid;
    int    mid_taken;

    sps = GFSK_AUDIO_RATE / baud;
    if (sps < 4) {
        fprintf(stderr, "gfsk: baud %d too high for audio rate %d (sps=%d < 4)\n",
                baud, GFSK_AUDIO_RATE, sps);
        return;
    }

    ma_buf = (double *)calloc((size_t)sps, sizeof(double));
    if (!ma_buf) { fprintf(stderr, "gfsk: calloc failed\n"); return; }

    fm_state_init(&fm);
    hdlc_init(&hdlc);
    g3ruh_init(&g3ruh);

    dc_avg    = 0.0;
    ma_sum    = 0.0;
    ma_idx    = 0;
    phi       = 0.0;
    filt_prev = 0.0;
    filt_mid  = 0.0;
    mid_taken = 0;

    /* IQ power block accumulator for carrier detection.
     * We sum i²+q² over each 25-IQ-sample decimation block and compare
     * the block total to a fixed threshold rather than using a slow EMA.
     *
     * WHY NOT A SLOW EMA: the slow EMA (τ=500ms) stays above the threshold
     * even during 200ms silence (decays only to ~67% of signal level), so
     * the gate is always open and dc_avg gets dragged toward 0 by noise.
     *
     * Measured values (from debug):
     *   Noise floor block total : 25 × 215        =     5,375
     *   Signal block total      : 25 × 2,187,441  = 54,686,025
     * Threshold 1,000,000 sits 186× above noise and 54× below signal.
     * During silence the gate closes IMMEDIATELY (within 1 block = 0.26 µs),
     * holding dc_avg at its converged value for the next preamble. */
    double iq_pwr_block_acc = 0.0;  /* accumulates i²+q² over current block */
    double dbg_iq_pwr_peak  = 0.0;  /* peak block power per reporting window */
    long long dbg_carrier_hold = 0; /* audio samples where dc_avg was held */
    double    dbg_pcm_sum    = 0.0; /* sum of pcm on gate-open samples */
    long long dbg_pcm_n      = 0;   /* count of gate-open samples */

    /* Per-symbol gate tracker.
     * sym_gate_ok  : set when gate opens for any sample in current symbol.
     * gate_quiet   : consecutive gated-off symbols; used to reset HDLC after
     *                a silence gap so stale frame state does not corrupt the
     *                next real frame.  At 9600 baud, 48 symbols = 5 ms. */
    int sym_gate_ok  = 0;
    int gate_quiet   = 0;

    long long rx_lo_hz = freq_hz - (long long)llround(offset_hz);
    if (!configure_rx(rx_lo_hz, IQ_SAMPLE_RATE, rx_channel, bandwidth_hz)) {
        fprintf(stderr, "gfsk: configure_rx failed\n");
        free(ma_buf);
        return;
    }
    fprintf(stderr, "decoder: GFSK downlink=%lld offset_hz=%.1f rx_lo=%lld rx_channel=%d baud=%d g3ruh=%d\n",
            freq_hz, offset_hz, rx_lo_hz, rx_channel, baud, use_g3ruh);

    iq_pipe = start_iio_stream(&child, IIO_BUFFER_SAMPS, rx_channel);
    if (!iq_pipe) {
        fprintf(stderr, "gfsk: iio stream failed\n");
        free(ma_buf);
        return;
    }

    /* Debug diagnostic counters */
    long long dbg_bits = 0, dbg_ones = 0;
    double dbg_peak = 0.0;      /* peak MA-filtered output */
    double dbg_pcm_peak = 0.0;  /* peak raw FM demod PCM (before MA filter) */
    /* Fire first report after 1 s, then every 5 s.
     * dbg_bits counts symbols (bits), not audio samples, so compare to baud. */
    long long dbg_next_report = (long long)baud * 1; /* first report after 1 s */

    while (g_running) {
        size_t got = fread(raw, 1, sizeof(raw), iq_pipe);
        size_t pairs = got / 4;
        size_t idx;

        for (idx = 0; idx < pairs && g_running; idx++) {
            int    off   = (int)(idx * 4);
            short  i_raw = (short)((uint16_t)raw[off]   | ((uint16_t)raw[off+1] << 8));
            short  q_raw = (short)((uint16_t)raw[off+2] | ((uint16_t)raw[off+3] << 8));
            short  pcm;

            /* 0. Accumulate i²+q² for this IQ sample into the current block. */
            iq_pwr_block_acc += (double)i_raw * i_raw + (double)q_raw * q_raw;

            /* 1. FM demodulate + decimate 25× → GFSK_AUDIO_RATE */
            if (!fm_process(&fm, i_raw, q_raw, GFSK_DECIM, &pcm)) continue;

            /* debug: track raw FM demod level (before DC block and MA filter) */
            if (g_debug) {
                double rp = fabs((double)pcm);
                if (rp > dbg_pcm_peak) dbg_pcm_peak = rp;
            }

            /* 2. DC block — instantaneous IQ-block-power-gated hold.
             *    α=0.9995 → τ=2000 audio samples = 200 bit periods.
             *    Tracks the static carrier frequency offset (~12 PCM),
             *    NOT individual bit deviations (±52 PCM).  Converges in
             *    5τ=10000 gate-open samples; we get ~81K/window so fine.
             *
             *    Drain the block accumulator (25 IQ samples → 1 audio sample).
             *    Gate: above noise floor AND below ADC saturation.
             *    25×2048² = 209,715,200 = full ADC clip.  The upper bound
             *    rejects the slow-attack AGC transient at each frame start
             *    (where FM output is garbage) while still updating during
             *    the settled-signal window (iq_block ≈ 27,225).
             *    iq_gate_ok is also used to zero the MA filter input during
             *    invalid (saturated or noise) blocks. */
            int iq_gate_ok;
            {
                double iq_block = iq_pwr_block_acc;
                iq_pwr_block_acc = 0.0;
                if (g_debug && iq_block > dbg_iq_pwr_peak) dbg_iq_pwr_peak = iq_block;
                /* Gate: above noise floor AND below ADC saturation transient.
                 * Noise floor peak ≈ 9K; GATE_LO=15K keeps noise out.
                 * slow_attack AGC transient pushes iq_block >100M at frame
                 * start; GATE_HI=100M excludes that clipped/distorted window.
                 * Settled signal (after AGC attack) lands in 15K-100M. */
                iq_gate_ok = (iq_block > 15000.0 && iq_block < 100000000.0);
                if (iq_gate_ok) {
                    sym_gate_ok = 1;  /* signal present in this symbol period */
                    dc_avg = 0.9995 * dc_avg + 0.0005 * (double)pcm;
                    if (g_debug) { dbg_pcm_sum += (double)pcm; dbg_pcm_n++; }
                } else {
                    if (g_debug) dbg_carrier_hold++;
                }
            }
            /* When IQ power is outside the valid gate window (noise or ADC
             * saturation transient) feed zero into the MA filter so garbage
             * PCM cannot corrupt the filtered output and produce false peaks. */
            double y = iq_gate_ok ? ((double)pcm - dc_avg) : 0.0;

            /* 3. Moving-average matched filter over one symbol period.
             *    Circular buffer: drop oldest, add newest. */
            ma_sum -= ma_buf[ma_idx];
            ma_buf[ma_idx] = y;
            ma_sum += y;
            ma_idx = (ma_idx + 1) % sps;
            double filtered = ma_sum;

            /* debug: track signal level */
            if (g_debug) {
                double af = fabs(filtered);
                if (af > dbg_peak) dbg_peak = af;
            }

            /* 4. Advance fractional symbol-clock phase by one audio sample */
            phi += 1.0;

            /* Capture mid-symbol sample for Gardner TED on first crossing */
            if (!mid_taken && phi >= (double)sps * 0.5) {
                filt_mid  = filtered;
                mid_taken = 1;
            }

            /* 5. Symbol boundary: make bit decision and update clock */
            if (phi >= (double)sps) {
                phi -= (double)sps;
                mid_taken = 0;

                /* Hard bit decision: positive FM deviation = mark = 1 */
                int bit = (filtered >= 0.0) ? 1 : 0;

                /*
                 * 6. Gardner timing error detector (sign-normalised).
                 *
                 *   sign(x_now) - sign(x_prev) ∈ {−2, 0, +2}
                 *   Zero when no transition → TED is silent during bit runs.
                 *   Non-zero only at transitions where timing info exists.
                 *
                 *   Normalise by amplitude so gain is signal-level independent.
                 *   e ∈ [−2, +2]; GFSK_TED_GAIN = 0.05 → max correction 0.1 sps.
                 */
                double sign_now  = (filtered  >= 0.0) ?  1.0 : -1.0;
                double sign_prev = (filt_prev >= 0.0) ?  1.0 : -1.0;
                double amplitude = fabs(filtered) + fabs(filt_prev) + 1e-10;
                double ted_err   = (filt_mid / amplitude) * (sign_now - sign_prev);

                phi -= GFSK_TED_GAIN * ted_err;
                /* Clamp phi into [0, sps) to prevent runaway */
                if (phi <  0.0)           phi += (double)sps;
                if (phi >= (double)sps)   phi -= (double)sps;

                filt_prev = filtered;

                /* debug counters */
                if (g_debug) {
                    dbg_bits++;
                    if (bit) dbg_ones++;
                    if (dbg_bits >= dbg_next_report) {
                        /* pcm_peak expected ~51 for 2400 Hz dev at 2.4 MHz sps.
                         * filtered peak expected ~510 (MA over 10 sps).
                         * If pcm_peak < 5, FM demod sees no signal (noise only). */
                        fprintf(stderr,
                            "gfsk: %lld bits  ones=%lld%%  pcm_peak=%.0f"
                            "  filtered_peak=%.0f  iq_blk_peak=%.0f"
                            "  dc_avg=%.1f  held=%lld  pcm_mean=%.1f(n=%lld)\n",
                            dbg_bits, dbg_ones * 100 / (dbg_bits + 1),
                            dbg_pcm_peak, dbg_peak,
                            dbg_iq_pwr_peak, dc_avg, dbg_carrier_hold,
                            dbg_pcm_n > 0 ? dbg_pcm_sum / (double)dbg_pcm_n : 0.0,
                            dbg_pcm_n);
                        dbg_peak = 0.0;
                        dbg_pcm_peak = 0.0;
                        dbg_iq_pwr_peak = 0.0;
                        dbg_carrier_hold = 0;
                        dbg_pcm_sum = 0.0;
                        dbg_pcm_n = 0;
                        /* subsequent reports every 5 s */
                        dbg_next_report += (long long)baud * 5;
                    }
                }

                /* 7. Optional G3RUH descramble, then NRZ-I HDLC + AX.25.
                 *    Only push bits when the gate was open for this symbol.
                 *    During silence / AGC transient the gate is closed and
                 *    y=0 → bit=1 always — injecting false 1-bits that would
                 *    corrupt HDLC alignment.  After GATE_QUIET_RESET consecutive
                 *    gated-off symbols reset HDLC so stale frame state does not
                 *    poison the next real preamble. */
#define GATE_QUIET_RESET  480   /* 480 symbols × (1/9600 s) = 50 ms */
                if (sym_gate_ok) {
                    gate_quiet = 0;
                    int decoded_bit = use_g3ruh ? g3ruh_descramble(&g3ruh, bit) : bit;
                    int frame_len = 0;
                    const uint8_t *frame = hdlc_push_bit(&hdlc, decoded_bit, &frame_len);
                    if (frame) write_aprs_frame(out, frame, frame_len);
                } else {
                    gate_quiet++;
                    if (gate_quiet == GATE_QUIET_RESET) {
                        hdlc_init(&hdlc);   /* clear stale frame state */
                        gate_quiet = 0;
                    }
                }
                sym_gate_ok = 0;  /* reset for next symbol period */
            }
        }

        if (got == 0 && feof(iq_pipe)) break;
    }

    free(ma_buf);
    fclose(iq_pipe);
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
    int baud = 9600;    /* GFSK baud rate; ignored for aprs/cw */
    int use_g3ruh = 0;  /* --g3ruh: enable G3RUH descrambler for GFSK */
    double offset_hz = DEFAULT_OFFSET_HZ;
    int rx_channel = DEFAULT_RX_CHANNEL;
    int bandwidth_hz = DEFAULT_RX_BW_HZ;
    double cw_wpm = 0.0;  /* 0 = adaptive, >0 = lock CW timing */
    double cw_min_span = DEFAULT_CW_MIN_SPAN;
    double cw_min_peak = DEFAULT_CW_MIN_PEAK;
    double cw_open_frac = DEFAULT_CW_OPEN_FRAC;
    double cw_close_frac = DEFAULT_CW_CLOSE_FRAC;
    double cw_min_element_frac = DEFAULT_CW_MIN_ELEMENT_FRAC;
    int cw_mix_sign = DEFAULT_CW_MIX_SIGN;
    double cw_abs_low = -1.0;
    double cw_abs_high = -1.0;
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
        } else if (strcmp(argv[i], "--baud") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            baud = (int)strtol(argv[++i], &end, 10);
            if (errno || !end || *end || baud <= 0) {
                fprintf(stderr, "Invalid --baud\n"); return 2;
            }
        } else if (strcmp(argv[i], "--g3ruh") == 0) {
            use_g3ruh = 1;
        } else if (strcmp(argv[i], "--debug") == 0) {
            g_debug = 1;
        } else if (strcmp(argv[i], "--rx-channel") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            rx_channel = (int)strtol(argv[++i], &end, 10);
            if (errno || !end || *end || (rx_channel != 1 && rx_channel != 2)) {
                fprintf(stderr, "Invalid --rx-channel; use 1 or 2\n"); return 2;
            }
        } else if (strcmp(argv[i], "--offset-hz") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            offset_hz = strtod(argv[++i], &end);
            if (errno || !end || *end || fabs(offset_hz) > 500000.0) {
                fprintf(stderr, "Invalid --offset-hz; use -500000..500000\n"); return 2;
            }
        } else if (strcmp(argv[i], "--cw-wpm") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            cw_wpm = strtod(argv[++i], &end);
            if (errno || !end || *end || cw_wpm < CW_WPM_MIN || cw_wpm > CW_WPM_MAX) {
                fprintf(stderr, "Invalid --cw-wpm; use %d..%d\n", CW_WPM_MIN, CW_WPM_MAX); return 2;
            }
        } else if (strcmp(argv[i], "--cw-min-span") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            cw_min_span = strtod(argv[++i], &end);
            if (errno || !end || *end || cw_min_span < 0.0) {
                fprintf(stderr, "Invalid --cw-min-span; use >=0\n"); return 2;
            }
        } else if (strcmp(argv[i], "--cw-min-peak") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            cw_min_peak = strtod(argv[++i], &end);
            if (errno || !end || *end || cw_min_peak < 0.0) {
                fprintf(stderr, "Invalid --cw-min-peak; use >=0\n"); return 2;
            }
        } else if (strcmp(argv[i], "--cw-open-frac") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            cw_open_frac = strtod(argv[++i], &end);
            if (errno || !end || *end || cw_open_frac <= 0.0 || cw_open_frac >= 1.0) {
                fprintf(stderr, "Invalid --cw-open-frac; use 0..1\n"); return 2;
            }
        } else if (strcmp(argv[i], "--cw-close-frac") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            cw_close_frac = strtod(argv[++i], &end);
            if (errno || !end || *end || cw_close_frac <= 0.0 || cw_close_frac >= 1.0 || cw_close_frac >= cw_open_frac) {
                fprintf(stderr, "Invalid --cw-close-frac; use 0..1 and less than open frac\n"); return 2;
            }
        } else if (strcmp(argv[i], "--cw-min-element-frac") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            cw_min_element_frac = strtod(argv[++i], &end);
            if (errno || !end || *end || cw_min_element_frac <= 0.0 || cw_min_element_frac > 1.5) {
                fprintf(stderr, "Invalid --cw-min-element-frac; use >0 and <=1.5\n"); return 2;
            }
        } else if (strcmp(argv[i], "--cw-mix-sign") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            cw_mix_sign = (int)strtol(argv[++i], &end, 10);
            if (errno || !end || *end || (cw_mix_sign != 1 && cw_mix_sign != -1)) {
                fprintf(stderr, "Invalid --cw-mix-sign; use 1 or -1\n"); return 2;
            }
        } else if (strcmp(argv[i], "--cw-abs-low") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            cw_abs_low = strtod(argv[++i], &end);
            if (errno || !end || *end || cw_abs_low < 0.0) {
                fprintf(stderr, "Invalid --cw-abs-low; use >=0\n"); return 2;
            }
        } else if (strcmp(argv[i], "--cw-abs-high") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            cw_abs_high = strtod(argv[++i], &end);
            if (errno || !end || *end || cw_abs_high <= 0.0) {
                fprintf(stderr, "Invalid --cw-abs-high; use >0\n"); return 2;
            }
        } else if (strcmp(argv[i], "--bandwidth-hz") == 0 && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            bandwidth_hz = (int)strtol(argv[++i], &end, 10);
            if (errno || !end || *end || bandwidth_hz < 200000) {
                fprintf(stderr, "Invalid --bandwidth-hz; use >=200000\n"); return 2;
            }
        } else {
            fprintf(stderr,
                "Usage: %s --freq-hz <hz> --mode <aprs|cw|gfsk> --output <path>"
                " [--baud <rate>] [--g3ruh] [--rx-channel 1|2] [--offset-hz hz] [--bandwidth-hz hz] [--cw-wpm wpm] [--cw-min-span v] [--cw-min-peak v] [--cw-open-frac v] [--cw-close-frac v] [--cw-min-element-frac v] [--cw-mix-sign 1|-1] [--cw-abs-low v] [--cw-abs-high v]\n", argv[0]);
            return 2;
        }
    }

    if (freq_hz < PLUTO_MIN_HZ || freq_hz > PLUTO_MAX_HZ || !mode || !output_path) {
        fprintf(stderr,
            "Usage: %s --freq-hz <hz> --mode <aprs|cw|gfsk> --output <path>"
            " [--baud <rate>] [--g3ruh] [--rx-channel 1|2] [--offset-hz hz] [--bandwidth-hz hz] [--cw-wpm wpm] [--cw-min-span v] [--cw-min-peak v] [--cw-open-frac v] [--cw-close-frac v] [--cw-min-element-frac v] [--cw-mix-sign 1|-1] [--cw-abs-low v] [--cw-abs-high v]\n", argv[0]);
        return 2;
    }

    out = fopen(output_path, "a");
    if (!out) { perror("fopen"); return 1; }
    setvbuf(out, NULL, _IOLBF, 0);

    if (strcmp(mode, "aprs") == 0) {
        run_aprs(freq_hz, offset_hz, rx_channel, bandwidth_hz, out);
    } else if (strcmp(mode, "cw") == 0) {
        run_cw(freq_hz, offset_hz, rx_channel, bandwidth_hz, cw_wpm, cw_min_span, cw_min_peak, cw_open_frac, cw_close_frac, cw_min_element_frac, cw_mix_sign, cw_abs_low, cw_abs_high, out);
    } else if (strcmp(mode, "gfsk") == 0) {
        run_gfsk(freq_hz, offset_hz, rx_channel, bandwidth_hz, baud, use_g3ruh, out);
    } else {
        fprintf(stderr, "Unknown mode: %s (supported: aprs, cw, gfsk)\n", mode);
        fclose(out);
        return 2;
    }

    fclose(out);
    return 0;
}

/*
 * pluto_test_gen.c
 *
 * Test signal generator for loopback testing of pluto_digital_decoder.
 * Transmits known AX.25 UI frames via the Pluto TX port.
 *
 * Hardware setup:
 *   Connect Pluto TX SMA → 30-40 dB attenuator → Pluto RX SMA
 *   (30 dB attenuator at ~-10 dB TX gain gives ~-33 dBm at RX input)
 *
 * Modes:
 *   gfsk  - 9600 baud FSK (plain rectangular), optional G3RUH scrambler
 *   afsk  - 1200 baud Bell 202 (APRS)
 *   cw    - Morse code carrier
 *
 * Usage:
 *   pluto_test_gen --freq-hz <hz> --mode <gfsk|afsk|cw>
 *                  [--baud <rate>] [--g3ruh] [--loop] [--count <n>]
 *                  [--atten <db>]
 *
 * Example (GFSK loopback):
 *   pluto_test_gen --freq-hz 437550000 --mode gfsk --baud 9600 --g3ruh --loop
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
/* Hardware constants (match pluto_digital_decoder)                   */
/* ------------------------------------------------------------------ */

#define PLUTO_MIN_HZ       70000000LL
#define PLUTO_MAX_HZ     6000000000LL
#define IQ_SAMPLE_RATE   2400000       /* samples/sec                  */
#define IIO_BUFFER_SAMPS 24000         /* iio_writedev buffer size     */

/* GFSK */
#define GFSK_DECIM       25            /* 2.4M / 25 = 96k audio rate   */
#define GFSK_AUDIO_RATE  (IQ_SAMPLE_RATE / GFSK_DECIM)

/* AFSK Bell 202 */
#define AFSK_MARK_HZ     1200
#define AFSK_SPACE_HZ    2200
#define AFSK_BAUD        1200

/* TX attenuation range (AD9361: 0 to -89.75 dB in 0.25 dB steps)    */
#define TX_ATTEN_DEFAULT 10            /* 10 dB below max = safe for loopback */

/* AX.25 */
#define AX25_FLAG        0x7E
#define AX25_MAX_FRAME   330

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_running = 1;
static void on_signal(int s) { (void)s; g_running = 0; }

/* ------------------------------------------------------------------ */
/* TX hardware setup                                                   */
/* ------------------------------------------------------------------ */

static int configure_tx(long long hz, int sample_rate_hz, int atten_db)
{
    char cmd[512];
    int r;

    if (hz < PLUTO_MIN_HZ || hz > PLUTO_MAX_HZ) return 0;

    /* Sample rate */
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/iio_attr -u local: -c ad9361-phy voltage0 "
        "sampling_frequency %d >/dev/null 2>&1", sample_rate_hz);
    r = system(cmd); if (r == -1 || !WIFEXITED(r) || WEXITSTATUS(r) != 0) return 0;

    /* RF bandwidth */
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/iio_attr -u local: -c ad9361-phy voltage1 "
        "rf_bandwidth 200000 >/dev/null 2>&1");
    r = system(cmd); if (r == -1 || !WIFEXITED(r) || WEXITSTATUS(r) != 0) return 0;

    /* TX attenuation (in millidB for the sysfs interface) */
    snprintf(cmd, sizeof(cmd),
        "/usr/bin/iio_attr -u local: -c ad9361-phy voltage0 "
        "hardwaregain -- -%d >/dev/null 2>&1", atten_db);
    system(cmd); /* non-fatal if attr name differs across FW versions */

    /* TX LO */
    {
        const char *paths[] = {
            "/sys/bus/iio/devices/iio:device1/out_altvoltage1_TX_LO_frequency",
            "/sys/bus/iio/devices/iio:device0/out_altvoltage1_TX_LO_frequency",
            "/sys/bus/iio/devices/iio:device2/out_altvoltage1_TX_LO_frequency",
        };
        size_t i;
        for (i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) {
            FILE *f = fopen(paths[i], "wb");
            if (!f) continue;
            fprintf(f, "%lld\n", hz);
            if (fclose(f) == 0) return 1;
        }
    }
    return 0;
}

static FILE *start_iio_tx(pid_t *child_pid, int buffer_samps)
{
    int fds[2];
    pid_t pid;
    char buf[32];

    if (pipe(fds) != 0) return NULL;
    pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return NULL; }
    if (pid == 0) {
        snprintf(buf, sizeof(buf), "%d", buffer_samps);
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]); close(fds[1]);
        /* iio_writedev reads IQ int16 pairs from stdin */
        execl("/usr/bin/iio_writedev", "iio_writedev",
              "-u", "local:", "-b", buf,
              "cf-ad9361-dds-core-lpc", (char *)NULL);
        _exit(127);
    }
    close(fds[0]);
    *child_pid = pid;
    return fdopen(fds[1], "wb");
}

/* ------------------------------------------------------------------ */
/* IQ output helpers                                                   */
/* ------------------------------------------------------------------ */

/* Circular buffer for IQ output.  We batch writes to avoid syscall overhead. */
#define IQ_BUF_SAMPS (IIO_BUFFER_SAMPS * 4)
static int16_t s_iq_buf[IQ_BUF_SAMPS * 2];
static int     s_iq_n = 0;
static FILE   *s_iq_out = NULL;

static void iq_flush(void)
{
    if (s_iq_n > 0 && s_iq_out) {
        fwrite(s_iq_buf, sizeof(int16_t), (size_t)(s_iq_n * 2), s_iq_out);
        s_iq_n = 0;
    }
}

static void iq_push(double i, double q)
{
    /* Clamp and scale to int16 range (leave ~6 dB headroom) */
    double scale = 16384.0;
    int16_t iv = (int16_t)(i * scale < -32767 ? -32767 : i * scale > 32767 ? 32767 : i * scale);
    int16_t qv = (int16_t)(q * scale < -32767 ? -32767 : q * scale > 32767 ? 32767 : q * scale);
    s_iq_buf[s_iq_n * 2]     = iv;
    s_iq_buf[s_iq_n * 2 + 1] = qv;
    s_iq_n++;
    if (s_iq_n >= IQ_BUF_SAMPS) iq_flush();
}

/* FM modulator state: running phase accumulator */
static double s_phase = 0.0;

static void fm_emit(double freq_hz, int n_samps)
{
    /* freq_hz relative to carrier (positive = mark, negative = space) */
    double phase_inc = 2.0 * M_PI * freq_hz / (double)IQ_SAMPLE_RATE;
    int i;
    for (i = 0; i < n_samps; i++) {
        iq_push(cos(s_phase), sin(s_phase));
        s_phase += phase_inc;
        if (s_phase >  M_PI) s_phase -= 2.0 * M_PI;
        if (s_phase < -M_PI) s_phase += 2.0 * M_PI;
    }
}

/* ------------------------------------------------------------------ */
/* CRC-CCITT (AX.25)                                                  */
/* ------------------------------------------------------------------ */

static uint16_t crc_ccitt(const uint8_t *data, int len)
{
    uint16_t crc = 0xFFFF;
    int i, b;
    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (b = 0; b < 8; b++) {
            if (crc & 1) crc = (crc >> 1) ^ 0x8408;
            else         crc >>= 1;
        }
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/* AX.25 frame builder                                                */
/* ------------------------------------------------------------------ */

static void ax25_encode_callsign(const char *call, int ssid, uint8_t *out, int last)
{
    char padded[7] = "      ";
    int i;
    strncpy(padded, call, 6);
    for (i = 0; i < 6; i++) out[i] = (uint8_t)((padded[i] & 0x7F) << 1);
    out[6] = (uint8_t)((ssid & 0x0F) << 1) | 0x60 | (last ? 0x01 : 0x00);
}

/*
 * Build a minimal AX.25 UI frame:
 *   dest NOCALL-0 → src TEST-1 → ctrl 0x03 → PID 0xF0 → info
 * Returns frame length (including CRC appended at end).
 */
static int build_ax25_frame(const char *info, uint8_t *frame, int max_len)
{
    int info_len = (int)strlen(info);
    int frame_len;
    uint16_t crc;

    if (info_len > max_len - 16) info_len = max_len - 16;

    ax25_encode_callsign("NOCALL", 0, frame + 0, 0);   /* dest  */
    ax25_encode_callsign("TEST",   1, frame + 7, 1);   /* src   */
    frame[14] = 0x03;   /* control: UI */
    frame[15] = 0xF0;   /* PID: no layer 3 */
    memcpy(frame + 16, info, (size_t)info_len);
    frame_len = 16 + info_len;

    crc = crc_ccitt(frame, frame_len);
    frame[frame_len++] = (uint8_t)(crc & 0xFF);
    frame[frame_len++] = (uint8_t)((crc >> 8) & 0xFF);

    return frame_len;
}

/* ------------------------------------------------------------------ */
/* HDLC bit stream builder                                            */
/* ------------------------------------------------------------------ */

/*
 * Encode a raw AX.25 frame into an HDLC physical-level bit stream.
 *
 * All bits — flags and data alike — are NRZ-I encoded:
 *   data bit 1 → no level change   (mark)
 *   data bit 0 → level change      (space)
 *
 * Bit stuffing (insert a 0 after every 5 consecutive data 1s) is applied
 * only to the frame payload, not to flags.
 *
 * The output bits[] array contains physical TX levels (0 or 1).
 * The FSK modulator maps 1 → +deviation (mark), 0 → −deviation (space).
 * The receiver FM-demodulates and NRZ-I-decodes these levels to recover
 * the original data bits.
 *
 * Returns number of physical levels written to bits[].
 */
static int hdlc_encode(const uint8_t *frame, int frame_len,
                       uint8_t *bits, int max_bits)
{
    int n = 0;
    int nrzi_last = 0;  /* last transmitted physical level          */
    int ones = 0;       /* consecutive data 1-bits (for stuffing)   */
    int i, b;

    /*
     * NRZI(d): NRZ-I encode data bit d → physical level → bits[n++]
     *   d=1: no transition, level stays
     *   d=0: transition, level flips
     */
#define NRZI(d) do {                                                \
    int _lv = ((d) & 1) ? nrzi_last : (1 - nrzi_last);            \
    nrzi_last = _lv;                                                \
    if (n < max_bits) bits[n++] = (uint8_t)_lv;                   \
} while(0)

    /*
     * DATA(d): NRZ-I encode data bit d WITH bit stuffing.
     * After 5 consecutive 1s, automatically inserts a stuffed 0.
     */
#define DATA(d) do {                                                \
    int _d = (d) & 1;                                              \
    NRZI(_d);                                                      \
    if (_d) { if (++ones == 5) { NRZI(0); ones = 0; } }           \
    else    { ones = 0; }                                          \
} while(0)

    /* Preamble: 90 bytes of 0x55 (DC-balanced) + 20 HDLC flag bytes.
     *
     * Root cause of earlier failures: 0x7E in NRZ-I encodes as 7 marks +
     * 1 space (87.5% marks, pcm_mean ~39).  With alpha=0.9995 the dc_avg
     * block converged toward 39 instead of the true carrier offset (~2),
     * leaving mark bits with y ~+15 -- near the noise floor -- causing bit
     * errors that generate false 0x7E patterns in the payload and truncate
     * frames to random short lengths (len=5, 11).
     *
     * Fix: precede the HDLC flags with 90 bytes of 0x55.  In NRZ-I, 0x55
     * encodes as 4 marks + 4 spaces per byte (50%, pcm_mean = carrier
     * offset ~2).  This reduces the preamble DC bias so dc_avg is only
     * mildly distorted when the gate opens.
     *
     * NOTE: the 0x55 alternating-FM pattern causes a longer slow_attack AGC
     * transient (~81 ms) than a pure 0x7E tone (~37 ms).  The 20 trailing
     * 0x7E flags (16.7 ms) extend the flag section beyond the transient end
     * (75+16.7=91.7 ms > 81 ms), leaving ~10 complete flags visible to the
     * decoder for reliable HDLC byte-alignment.  Total preamble: 880 bits
     * = 91.7 ms at 9600 baud.
     *
     * Note: 0x55 decoded through the HDLC shift-register never matches
     * 0x7E, so no false flag events occur during the pre-preamble. */
    for (i = 0; i < 90; i++)
        for (b = 0; b < 8; b++) NRZI((0x55 >> b) & 1);
    for (i = 0; i < 20; i++)
        for (b = 0; b < 8; b++) NRZI((AX25_FLAG >> b) & 1);

    /* Frame payload: NRZ-I + bit stuffing, LSB first per byte */
    ones = 0;
    for (i = 0; i < frame_len; i++)
        for (b = 0; b < 8; b++) DATA((frame[i] >> b) & 1);

    /* Closing flag + 4 postamble flags, NRZ-I encoded, no bit stuffing */
    for (i = 0; i < 5; i++)
        for (b = 0; b < 8; b++) NRZI((AX25_FLAG >> b) & 1);

#undef NRZI
#undef DATA

    return n;
}

/* ------------------------------------------------------------------ */
/* G3RUH scrambler (mirrors the decoder's descrambler)               */
/* ------------------------------------------------------------------ */

static uint32_t g3ruh_sr = 0;

static void g3ruh_reset(void) { g3ruh_sr = 0; }

/*
 * Scramble one bit: out = in XOR sr[11] XOR sr[16], then shift in out.
 * (Encoder feeds back the SCRAMBLED bit, making it self-synchronizing
 *  at the decoder which feeds back the RECEIVED bit — same polynomial.)
 */
static int g3ruh_scramble(int bit)
{
    int out = bit ^ ((int)(g3ruh_sr >> 11) & 1) ^ ((int)(g3ruh_sr >> 16) & 1);
    g3ruh_sr = ((g3ruh_sr << 1) | (unsigned)(out & 1)) & 0x1FFFFU;
    return out;
}

/* ------------------------------------------------------------------ */
/* GFSK transmit loop                                                 */
/* ------------------------------------------------------------------ */

/*
 * Frequency deviation for GFSK: BT=0.5 → deviation ≈ baud/4.
 * At 9600 baud: deviation = 2400 Hz (modulation index 0.5).
 * We use rectangular FSK (no Gaussian shaping) for the test generator;
 * the decoder's MA matched filter handles this correctly.
 */
static double gfsk_deviation_hz(int baud) { return (double)baud * 0.25; }

static void run_gfsk_gen(long long freq_hz, int baud, int use_g3ruh,
                         int count, int loop, FILE *tx)
{
    uint8_t frame[AX25_MAX_FRAME + 4];
    uint8_t bits[AX25_MAX_FRAME * 16 + 256];
    int samps_per_bit = IQ_SAMPLE_RATE / baud;
    double dev = gfsk_deviation_hz(baud);
    int frame_num = 0;
    int run;

    s_iq_out = tx;

    fprintf(stderr, "gfsk_gen: freq=%lld Hz  baud=%d  sps=%d  dev=%.0f Hz  g3ruh=%s\n",
            freq_hz, baud, samps_per_bit, dev, use_g3ruh ? "yes" : "no");

    /* Inter-frame silence: 200 ms — gives HDLC 150 ms clean reset window
     * before each frame after the 50 ms GATE_QUIET_RESET fires. */
    int silence_samps = IQ_SAMPLE_RATE / 5;

    for (run = 0; (loop || run < count) && g_running; run++) {
        char info[80];
        int frame_len, n_bits, i;

        snprintf(info, sizeof(info),
                 ">PLUTO GFSK TEST FRAME %d  BAUD=%d  G3RUH=%s",
                 ++frame_num, baud, use_g3ruh ? "ON" : "OFF");

        frame_len = build_ax25_frame(info, frame, (int)sizeof(frame));
        n_bits = hdlc_encode(frame, frame_len, bits, (int)sizeof(bits));

        if (use_g3ruh) g3ruh_reset();

        for (i = 0; i < n_bits && g_running; i++) {
            int bit = bits[i];
            if (use_g3ruh) bit = g3ruh_scramble(bit);
            /* bit=1 → mark → +deviation; bit=0 → space → -deviation */
            double f = (bit ? +dev : -dev);
            fm_emit(f, samps_per_bit);
        }

        /* Inter-frame silence (carrier off = zero IQ) */
        {
            int s;
            for (s = 0; s < silence_samps && g_running; s++) iq_push(0.0, 0.0);
        }

        iq_flush();
        fprintf(stderr, "gfsk_gen: sent frame %d (%d bits, %d bytes)\n",
                frame_num, n_bits, frame_len);
    }
}

/* ------------------------------------------------------------------ */
/* AFSK Bell 202 transmit loop                                        */
/* ------------------------------------------------------------------ */

static void run_afsk_gen(long long freq_hz, int count, int loop, FILE *tx)
{
    uint8_t frame[AX25_MAX_FRAME + 4];
    uint8_t bits[AX25_MAX_FRAME * 16 + 256];
    int samps_per_bit = IQ_SAMPLE_RATE / AFSK_BAUD;
    int frame_num = 0;
    int run;

    /*
     * For AFSK, the audio tones (1200/2200 Hz) are FM-modulated onto
     * the carrier with ±2500 Hz deviation (narrowband FM, like APRS).
     * We use a secondary phase accumulator for the audio tone and FM-mod
     * that onto the carrier.
     */
    double audio_phase = 0.0;
    double fm_dev_scale = 2.0 * M_PI * 2500.0 / (double)IQ_SAMPLE_RATE;

    (void)freq_hz; /* LO already set by caller */
    s_iq_out = tx;

    fprintf(stderr, "afsk_gen: Bell 202 1200/2200 Hz  baud=1200\n");

    int silence_samps = IQ_SAMPLE_RATE / 5;

    for (run = 0; (loop || run < count) && g_running; run++) {
        char info[80];
        int frame_len, n_bits, i;

        snprintf(info, sizeof(info),
                 ">PLUTO AFSK TEST FRAME %d", ++frame_num);

        frame_len = build_ax25_frame(info, frame, (int)sizeof(frame));
        n_bits = hdlc_encode(frame, frame_len, bits, (int)sizeof(bits));

        for (i = 0; i < n_bits && g_running; i++) {
            double tone_hz = bits[i] ? AFSK_MARK_HZ : AFSK_SPACE_HZ;
            double audio_inc = 2.0 * M_PI * tone_hz / (double)IQ_SAMPLE_RATE;
            int s;
            for (s = 0; s < samps_per_bit; s++) {
                /* FM modulate the audio tone onto the carrier */
                double audio_sample = sin(audio_phase);
                s_phase += fm_dev_scale * audio_sample;
                iq_push(cos(s_phase), sin(s_phase));
                audio_phase += audio_inc;
                if (audio_phase > 2.0 * M_PI) audio_phase -= 2.0 * M_PI;
            }
        }

        {
            int s;
            for (s = 0; s < silence_samps && g_running; s++) iq_push(0.0, 0.0);
        }

        iq_flush();
        fprintf(stderr, "afsk_gen: sent frame %d\n", frame_num);
    }
}

/* ------------------------------------------------------------------ */
/* CW Morse transmit loop                                             */
/* ------------------------------------------------------------------ */

static const struct { const char *code; char ch; } MORSE_TABLE[] = {
    {".-",'A'},{"-...",'B'},{"-.-.",'C'},{"-..",'D'},{".",'E'},
    {"..-.",'F'},{"--.",'G'},{"....", 'H'},{"..",'I'},{".---",'J'},
    {"-.-",'K'},{".-..",'L'},{"--",'M'},{"-.",'N'},{"---",'O'},
    {".--.",'P'},{"--.-",'Q'},{".-.",'R'},{"...",'S'},{"-",'T'},
    {"..-",'U'},{"...-",'V'},{".--",'W'},{"-..-",'X'},{"-.--",'Y'},
    {"--..",'Z'},
    {"-----",'0'},{".----",'1'},{"..---",'2'},{"...--",'3'},
    {"....-",'4'},{".....",'5'},{"-....",  '6'},{"--...",'7'},
    {"---..",'8'},{"----.",'9'},
    {NULL,0}
};

static const char *morse_for(char c)
{
    int i;
    c = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
    for (i = 0; MORSE_TABLE[i].code; i++)
        if (MORSE_TABLE[i].ch == c) return MORSE_TABLE[i].code;
    return NULL;
}

static void run_cw_gen(long long freq_hz, int count, int loop, FILE *tx)
{
    /* CW offset from carrier: +800 Hz (audible sidetone) */
    double cw_offset = 800.0;
    /* WPM = 20, dit = 60ms */
    int dit_samps  = IQ_SAMPLE_RATE * 60  / 1000;
    int dah_samps  = IQ_SAMPLE_RATE * 180 / 1000;
    int sym_space  = IQ_SAMPLE_RATE * 60  / 1000;
    int char_space = IQ_SAMPLE_RATE * 180 / 1000;
    int word_space = IQ_SAMPLE_RATE * 420 / 1000;
    const char *msg = "PLUTO TEST DE TEST-1";
    int run, frame_num = 0;

    (void)freq_hz;
    s_iq_out = tx;
    fprintf(stderr, "cw_gen: offset=%.0f Hz  20 WPM\n", cw_offset);

    for (run = 0; (loop || run < count) && g_running; run++) {
        const char *p;
        fprintf(stderr, "cw_gen: sending '%s' (frame %d)\n", msg, ++frame_num);
        for (p = msg; *p && g_running; p++) {
            if (*p == ' ') {
                int s; for (s = 0; s < word_space; s++) iq_push(0.0, 0.0);
                continue;
            }
            const char *code = morse_for(*p);
            if (!code) continue;
            int first = 1;
            const char *e;
            for (e = code; *e && g_running; e++) {
                if (!first) { int s; for (s=0;s<sym_space;s++) iq_push(0.0,0.0); }
                first = 0;
                int dur = (*e == '-') ? dah_samps : dit_samps;
                fm_emit(cw_offset, dur);
                iq_flush();
            }
            { int s; for (s=0;s<char_space;s++) iq_push(0.0,0.0); }
        }
        /* 1s gap between messages */
        { int s; for (s=0;s<IQ_SAMPLE_RATE;s++) iq_push(0.0,0.0); }
        iq_flush();
    }
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    long long freq_hz = 0;
    const char *mode  = NULL;
    int baud       = 9600;
    int use_g3ruh  = 0;
    int count      = 5;
    int loop       = 0;
    int atten_db   = TX_ATTEN_DEFAULT;
    int i;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--freq-hz") && i+1 < argc) {
            char *e = NULL; errno = 0;
            freq_hz = strtoll(argv[++i], &e, 10);
            if (errno || !e || *e) { fprintf(stderr,"Invalid --freq-hz\n"); return 2; }
        } else if (!strcmp(argv[i], "--mode") && i+1 < argc) {
            mode = argv[++i];
        } else if (!strcmp(argv[i], "--baud") && i+1 < argc) {
            baud = atoi(argv[++i]);
            if (baud <= 0) { fprintf(stderr,"Invalid --baud\n"); return 2; }
        } else if (!strcmp(argv[i], "--g3ruh")) {
            use_g3ruh = 1;
        } else if (!strcmp(argv[i], "--loop")) {
            loop = 1;
        } else if (!strcmp(argv[i], "--count") && i+1 < argc) {
            count = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--atten") && i+1 < argc) {
            atten_db = atoi(argv[++i]);
        } else {
            fprintf(stderr,
                "Usage: %s --freq-hz <hz> --mode <gfsk|afsk|cw>\n"
                "         [--baud <rate>] [--g3ruh] [--loop] [--count <n>]\n"
                "         [--atten <db>]\n"
                "\n"
                "Hardware: connect TX SMA -> 30dB attenuator -> RX SMA\n"
                "Modes:\n"
                "  gfsk  9600/4800/2400/1200 baud FSK AX.25\n"
                "  afsk  1200 baud Bell 202 APRS\n"
                "  cw    Morse code\n",
                argv[0]);
            return 2;
        }
    }

    if (freq_hz < PLUTO_MIN_HZ || freq_hz > PLUTO_MAX_HZ || !mode) {
        fprintf(stderr, "Need --freq-hz and --mode\n");
        return 2;
    }

    if (!configure_tx(freq_hz, IQ_SAMPLE_RATE, atten_db)) {
        fprintf(stderr, "test_gen: configure_tx failed (is Pluto connected?)\n");
        return 1;
    }
    fprintf(stderr, "test_gen: TX configured  freq=%lld Hz  atten=%d dB\n",
            freq_hz, atten_db);

    pid_t child = -1;
    FILE *tx = start_iio_tx(&child, IIO_BUFFER_SAMPS);
    if (!tx) {
        fprintf(stderr, "test_gen: could not start iio_writedev\n");
        return 1;
    }

    if (!strcmp(mode, "gfsk")) {
        run_gfsk_gen(freq_hz, baud, use_g3ruh, count, loop, tx);
    } else if (!strcmp(mode, "afsk")) {
        run_afsk_gen(freq_hz, count, loop, tx);
    } else if (!strcmp(mode, "cw")) {
        run_cw_gen(freq_hz, count, loop, tx);
    } else {
        fprintf(stderr, "Unknown mode: %s\n", mode);
        fclose(tx);
        if (child > 0) { kill(child, SIGTERM); waitpid(child, NULL, 0); }
        return 2;
    }

    iq_flush();
    fclose(tx);
    if (child > 0) { kill(child, SIGTERM); waitpid(child, NULL, 0); }
    return 0;
}

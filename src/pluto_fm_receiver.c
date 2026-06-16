#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define PLUTO_MIN_HZ 70000000LL
#define PLUTO_MAX_HZ 6000000000LL
/* FM_RECEIVER_CPU_REDUCTION_600K_V2: 600 kSPS / 25 = 24 kHz PCM output. */
#define AUDIO_IQ_SAMPLE_RATE 600000
#define AUDIO_DECIMATION 25
#define AUDIO_PCM_CHUNK_SAMPLES 4800
#define AUDIO_IIO_BUFFER_SAMPLES 24000

static volatile sig_atomic_t g_running = 1;

struct fm_demod_state {
    long long acc;
    int acc_count;
    double prev_i;
    double prev_q;
    double prev_demod;
    double dc_y;
    int have_prev;
};

static void on_signal(int signum)
{
    (void)signum;
    g_running = 0;
}

static int write_rx_lo_frequency(long long frequency_hz)
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

    return write_rx_lo_frequency(frequency_hz);
}




static void fm_log_config(long long frequency_hz, const char *output_path)
{
    fprintf(stderr,
            "pluto_fm_receiver: freq_hz=%lld iq_rate=%d decimation=%d pcm_rate=%d iio_buffer_samples=%d pcm_chunk_samples=%d output=%s\n",
            frequency_hz,
            AUDIO_IQ_SAMPLE_RATE,
            AUDIO_DECIMATION,
            AUDIO_IQ_SAMPLE_RATE / AUDIO_DECIMATION,
            AUDIO_IIO_BUFFER_SAMPLES,
            AUDIO_PCM_CHUNK_SAMPLES,
            output_path ? output_path : "");
    fflush(stderr);
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

static int stream_fm_audio_to_file(FILE *pipe, FILE *output, struct fm_demod_state *state)
{
    unsigned char iq_bytes[AUDIO_IIO_BUFFER_SAMPLES * 4];
    short pcm[AUDIO_PCM_CHUNK_SAMPLES];
    size_t pcm_count = 0;
    size_t flush_pending = 0;

    
    unsigned long long total_input_pairs = 0;
    unsigned long long total_output_samples = 0;
    unsigned long long next_log_input_pairs = (unsigned long long)AUDIO_IQ_SAMPLE_RATE * 5ULL;
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
        
        total_input_pairs += (unsigned long long)sample_pairs;for (sample_index = 0; sample_index < sample_pairs; sample_index++) {
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
                
                total_output_samples++;state->acc = 0;
                state->acc_count = 0;

                if (pcm_count >= AUDIO_PCM_CHUNK_SAMPLES) {
                    if (fwrite(pcm, sizeof(short), pcm_count, output) != pcm_count) {
                        return -1;
                    }
                    flush_pending += pcm_count;
                    if (flush_pending >= 12000) {
                        fflush(output);
                        flush_pending = 0;
                    }
                    pcm_count = 0;
                }
            }
        }

        if (total_input_pairs >= next_log_input_pairs) {
            fprintf(stderr,
                    "pluto_fm_receiver: samples input=%llu output=%llu target_pcm_rate=%d\n",
                    total_input_pairs,
                    total_output_samples,
                    AUDIO_IQ_SAMPLE_RATE / AUDIO_DECIMATION);
            fflush(stderr);
            next_log_input_pairs += (unsigned long long)AUDIO_IQ_SAMPLE_RATE * 5ULL;
        }
    }

    if (pcm_count > 0) {
        if (fwrite(pcm, sizeof(short), pcm_count, output) != pcm_count) {
            return -1;
        }
        flush_pending += pcm_count;
    }
    if (flush_pending > 0) {
        fflush(output);
    }

    return 0;
}

int main(int argc, char **argv)
{
    long long frequency_hz = 0;
    const char *output_path = NULL;
    FILE *output = NULL;
    FILE *pipe = NULL;
    pid_t child_pid = -1;
    struct fm_demod_state demod_state;
    int argi;

    for (argi = 1; argi < argc; argi++) {
        if (strcmp(argv[argi], "--freq-hz") == 0 && argi + 1 < argc) {
            char *end = NULL;
            errno = 0;
            frequency_hz = strtoll(argv[++argi], &end, 10);
            if (errno != 0 || !end || *end != '\0') {
                fprintf(stderr, "Invalid --freq-hz value.\n");
                return 2;
            }
        } else if (strcmp(argv[argi], "--output") == 0 && argi + 1 < argc) {
            output_path = argv[++argi];
        } else {
            fprintf(stderr, "Usage: %s --freq-hz <hz> --output <path>\n", argv[0]);
            return 2;
        }
    }

    if (frequency_hz < PLUTO_MIN_HZ || frequency_hz > PLUTO_MAX_HZ || !output_path || !*output_path) {
        fprintf(stderr, "Usage: %s --freq-hz <hz> --output <path>\n", argv[0]);
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (!configure_rx_audio_path(frequency_hz)) {
        fprintf(stderr, "Unable to configure Pluto RX path.\n");
        return 1;
    }

    output = fopen(output_path, "wb");
    if (!output) {
        perror("fopen");
        return 1;
    }
    setvbuf(output, NULL, _IOFBF, 65536);

    memset(&demod_state, 0, sizeof(demod_state));
    pipe = start_iio_read_stream(&child_pid);
    if (!pipe) {
        fprintf(stderr, "Unable to start iio_readdev stream.\n");
        fclose(output);
        return 1;
    }

    fm_log_config(frequency_hz, output_path);
    if (stream_fm_audio_to_file(pipe, output, &demod_state) != 0) {
        fprintf(stderr, "Audio stream failed.\n");
    }

    fclose(pipe);
    fclose(output);
    if (child_pid > 0) {
        kill(child_pid, SIGTERM);
        waitpid(child_pid, NULL, 0);
    }

    return 0;
}

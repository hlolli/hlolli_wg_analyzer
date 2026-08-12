#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "alignment.h"
#include "alignment_file.h"
#include "dsp.h"
#include "features.h"
#include "internal.h"
#include "output.h"
#include "report.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#define HWA_BENCH_PI 3.14159265358979323846264338327950288
#define HWA_BENCH_FFT_SIZE 4096U
#define HWA_BENCH_FFT_REPEATS 16U
#define HWA_BENCH_JSON_REPEATS 64U
#define HWA_BENCH_MAX_SAMPLES 101U

typedef int (*HWABenchRun)(void *context,
                           uint64_t *work,
                           uint64_t *checksum,
                           char *error,
                           size_t error_size);

typedef struct HWABenchCase {
    const char *name;
    const char *unit;
    HWABenchRun run;
    void *context;
    double *seconds_per_iteration;
    double *units_per_second;
    uint64_t work_per_iteration;
    uint64_t checksum;
    uint64_t tracked_work_bytes;
    int long_case;
} HWABenchCase;

typedef struct HWADecodeState {
    char path[1024];
    uint64_t frames;
    uint16_t channels;
} HWADecodeState;

typedef struct HWAFFTState {
    HwaDspComplex *source;
    HwaDspComplex *work;
} HWAFFTState;

typedef struct HWAFeatureState {
    double *samples;
    uint64_t frames;
    uint16_t channels;
    uint32_t sample_rate;
    HWAAnalysisOptions options;
    uint64_t tracked_work_bytes;
    int work_kind;
} HWAFeatureState;

typedef struct HWASourceState {
    unsigned char header[44];
    HWAByteSource source;
    HWAAnalysisOptions options;
    uint64_t frames;
    uint64_t tracked_work_bytes;
} HWASourceState;

typedef struct HWAAlignmentState {
    HWAAlignFrame *reference_frames;
    HWAAlignFrame *target_frames;
    HWAAlignTrack reference;
    HWAAlignTrack target;
    HWAAlignmentOptions options;
} HWAAlignmentState;

typedef struct HWAJsonState {
    HWAAnalysis analysis;
    HWAChannelMetrics channels[8];
    char path[768];
} HWAJsonState;

static volatile uint64_t hwa_bench_sink;

static void bench_error(char *error, size_t error_size, const char *message) {
    if (error != NULL && error_size != 0U) {
        (void)snprintf(error, error_size, "%s", message);
    }
}

static uint64_t double_bits(double value) {
    uint64_t bits = 0U;
    (void)memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static uint64_t checksum_mix(uint64_t checksum, uint64_t value) {
    checksum ^= value + UINT64_C(0x9e3779b97f4a7c15) +
                (checksum << 6U) + (checksum >> 2U);
    return checksum;
}

static double now_seconds(void) {
#if defined(_WIN32)
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&counter) || frequency.QuadPart <= 0) {
        return -1.0;
    }
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return -1.0;
    }
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
#endif
}

static long process_id(void) {
#if defined(_WIN32)
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}

static const char *architecture_name(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#elif defined(__wasm64__)
    return "wasm64";
#elif defined(__wasm32__)
    return "wasm32";
#else
    return "unknown";
#endif
}

static void machine_name(char *buffer, size_t buffer_size) {
    if (buffer_size == 0U) {
        return;
    }
    buffer[0] = '\0';
#if defined(_WIN32)
    {
        DWORD count = (DWORD)buffer_size;
        if (!GetComputerNameA(buffer, &count)) {
            buffer[0] = '\0';
        }
    }
#else
    if (gethostname(buffer, buffer_size) != 0) {
        buffer[0] = '\0';
    }
    buffer[buffer_size - 1U] = '\0';
#endif
    if (buffer[0] == '\0') {
        (void)snprintf(buffer, buffer_size, "unknown");
    }
}

static void put_u16_le(unsigned char *bytes, uint16_t value) {
    bytes[0] = (unsigned char)(value & UINT16_C(0xff));
    bytes[1] = (unsigned char)(value >> 8U);
}

static void put_u32_le(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)(value & UINT32_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT32_C(0xff));
    bytes[2] = (unsigned char)((value >> 16U) & UINT32_C(0xff));
    bytes[3] = (unsigned char)(value >> 24U);
}

static int minimum_feature_work(uint32_t sample_rate, uint16_t channels,
                                uint64_t frames,
                                const HWAAnalysisOptions *options,
                                uint64_t *minimum) {
    uint64_t lower = 0U;
    uint64_t upper = options->max_work_bytes;
    char ignored_error[HWA_ERROR_SIZE];

    while (lower < upper) {
        const uint64_t middle = lower + (upper - lower) / 2U;
        HWAAnalysisOptions trial = *options;
        HWAFeatureProcessor *processor = NULL;
        trial.max_work_bytes = middle;
        if (hwa_features_create(&processor, sample_rate, channels, 0U, frames,
                                &trial, ignored_error,
                                sizeof(ignored_error)) == 0) {
            hwa_features_destroy(processor);
            upper = middle;
        } else {
            lower = middle + 1U;
        }
    }
    {
        HWAAnalysisOptions trial = *options;
        HWAFeatureProcessor *processor = NULL;
        trial.max_work_bytes = lower;
        if (hwa_features_create(&processor, sample_rate, channels, 0U, frames,
                                &trial, ignored_error,
                                sizeof(ignored_error)) != 0) {
            return 0;
        }
        hwa_features_destroy(processor);
    }
    *minimum = lower;
    return 1;
}

static int generated_source_read(void *context, uint64_t offset,
                                 unsigned char *destination, size_t size) {
    const HWASourceState *state = (const HWASourceState *)context;
    size_t index;
    if (destination == NULL || offset > state->source.size ||
        (uint64_t)size > state->source.size - offset) {
        return -1;
    }
    for (index = 0U; index < size; ++index) {
        const uint64_t position = offset + (uint64_t)index;
        if (position < sizeof(state->header)) {
            destination[index] = state->header[(size_t)position];
        } else {
            const uint64_t data_position = position - sizeof(state->header);
            const uint64_t frame = data_position / 2U;
            const uint32_t phase = (uint32_t)(frame * UINT64_C(9973));
            const int32_t signed_sample =
                (int32_t)(phase & UINT32_C(0xffff)) - INT32_C(32768);
            const uint16_t sample = (uint16_t)(int16_t)signed_sample;
            destination[index] = (data_position & 1U) == 0U
                                     ? (unsigned char)(sample & UINT16_C(0xff))
                                     : (unsigned char)(sample >> 8U);
        }
    }
    return 0;
}

static int source_setup(HWASourceState *state, uint64_t frames,
                        char *error, size_t error_size) {
    const uint64_t data_bytes = frames * 2U;
    uint64_t feature_work;
    uint64_t outer_work;
    HWAAnalysisOptions trial;
    HWAAnalysis analysis;
    memset(state, 0, sizeof(*state));
    if (data_bytes > UINT32_MAX - 36U) {
        bench_error(error, error_size, "generated WAVE exceeds RIFF limits");
        return 0;
    }
    state->frames = frames;
    (void)memcpy(state->header, "RIFF", 4U);
    put_u32_le(state->header + 4U, (uint32_t)data_bytes + 36U);
    (void)memcpy(state->header + 8U, "WAVEfmt ", 8U);
    put_u32_le(state->header + 16U, 16U);
    put_u16_le(state->header + 20U, 1U);
    put_u16_le(state->header + 22U, 1U);
    put_u32_le(state->header + 24U, 48000U);
    put_u32_le(state->header + 28U, 96000U);
    put_u16_le(state->header + 32U, 2U);
    put_u16_le(state->header + 34U, 16U);
    (void)memcpy(state->header + 36U, "data", 4U);
    put_u32_le(state->header + 40U, (uint32_t)data_bytes);
    state->source.context = state;
    state->source.name = "generated-benchmark-summary.wav";
    state->source.size = sizeof(state->header) + data_bytes;
    state->source.read_at = generated_source_read;
    hwa_analysis_options_default(&state->options);
    state->options.max_input_bytes = state->source.size;
    state->options.max_input_frames = frames;
    state->options.max_work_bytes = UINT64_C(536870912);
    state->options.max_transforms = 100000U;
    state->options.max_track_points = 100000U;
    state->options.max_spectrum_values = 1000000U;
    state->options.max_lag_samples = 32U;
    state->options.true_peak_oversample = 1U;
    state->options.collect_tracks = 0;
    state->options.collect_spectrogram = 0;
    if (!minimum_feature_work(48000U, 1U, frames, &state->options,
                              &feature_work)) {
        bench_error(error, error_size,
                    "could not measure the summary work boundary");
        return 0;
    }
    outer_work = (uint64_t)state->options.decode_block_frames * UINT64_C(11) +
                 (uint64_t)strlen(state->source.name) + UINT64_C(1);
    if (feature_work > UINT64_MAX - outer_work) {
        bench_error(error, error_size, "summary work boundary overflows");
        return 0;
    }
    state->tracked_work_bytes = feature_work + outer_work;
    trial = state->options;
    trial.max_work_bytes = state->tracked_work_bytes;
    if (hwa_analyze_wav_source(&state->source, &trial, &analysis,
                               error, error_size) != 0) {
        bench_error(error, error_size,
                    "exact summary work boundary was rejected");
        return 0;
    }
    hwa_analysis_free(&analysis);
    trial.max_work_bytes--;
    if (hwa_analyze_wav_source(&state->source, &trial, &analysis,
                               error, error_size) == 0) {
        hwa_analysis_free(&analysis);
        bench_error(error, error_size,
                    "one-under summary work boundary was accepted");
        return 0;
    }
    state->options.max_work_bytes = state->tracked_work_bytes;
    return 1;
}

static int run_source_summary(void *context, uint64_t *work,
                              uint64_t *checksum,
                              char *error, size_t error_size) {
    HWASourceState *state = (HWASourceState *)context;
    HWAAnalysis analysis;
    uint64_t hash = UINT64_C(0x6eed0e9da4d94a4f);
    if (hwa_analyze_wav_source(&state->source, &state->options, &analysis,
                               error, error_size) != 0) {
        return 0;
    }
    hash = checksum_mix(hash, double_bits(analysis.channels[0].rms));
    hash = checksum_mix(hash, double_bits(analysis.loudness.integrated_lufs));
    hash = checksum_mix(hash, double_bits(analysis.spectrum.centroid_hz));
    hash = checksum_mix(hash, analysis.spectrum.transform_count);
    *work = analysis.format.frames;
    *checksum = hash;
    hwa_analysis_free(&analysis);
    return *work == state->frames;
}

static int decode_setup(HWADecodeState *state, char *error, size_t error_size) {
    const char *temporary_directory;
    FILE *file;
    unsigned char header[44];
    unsigned char block[8192];
    uint64_t written = 0U;
    uint32_t data_bytes;
    int path_length;

    memset(state, 0, sizeof(*state));
#if defined(_WIN32)
    temporary_directory = getenv("TEMP");
    if (temporary_directory == NULL || temporary_directory[0] == '\0') {
        temporary_directory = ".";
    }
#else
    temporary_directory = getenv("TMPDIR");
    if (temporary_directory == NULL || temporary_directory[0] == '\0') {
        temporary_directory = "/tmp";
    }
#endif
    state->frames = UINT64_C(240000);
    state->channels = 1U;
    path_length = snprintf(state->path, sizeof(state->path),
                           "%s/hwa-benchmark-%ld.wav",
                           temporary_directory, process_id());
    if (path_length < 0 || (size_t)path_length >= sizeof(state->path)) {
        bench_error(error, error_size, "temporary WAVE path is too long");
        return 0;
    }
    data_bytes = (uint32_t)(state->frames * 2U);
    memset(header, 0, sizeof(header));
    (void)memcpy(header, "RIFF", 4U);
    put_u32_le(header + 4U, data_bytes + 36U);
    (void)memcpy(header + 8U, "WAVEfmt ", 8U);
    put_u32_le(header + 16U, 16U);
    put_u16_le(header + 20U, 1U);
    put_u16_le(header + 22U, state->channels);
    put_u32_le(header + 24U, 48000U);
    put_u32_le(header + 28U, 96000U);
    put_u16_le(header + 32U, 2U);
    put_u16_le(header + 34U, 16U);
    (void)memcpy(header + 36U, "data", 4U);
    put_u32_le(header + 40U, data_bytes);

    file = fopen(state->path, "wb");
    if (file == NULL || fwrite(header, 1U, sizeof(header), file) != sizeof(header)) {
        if (file != NULL) {
            (void)fclose(file);
        }
        bench_error(error, error_size, "could not create decode fixture");
        return 0;
    }
    while (written < state->frames) {
        size_t frames = (size_t)(state->frames - written);
        size_t index;
        if (frames > sizeof(block) / 2U) {
            frames = sizeof(block) / 2U;
        }
        for (index = 0U; index < frames; ++index) {
            const uint32_t phase =
                (uint32_t)((written + (uint64_t)index) * UINT64_C(9973));
            const int32_t signed_sample =
                (int32_t)(phase & UINT32_C(0xffff)) - INT32_C(32768);
            const uint16_t sample = (uint16_t)(int16_t)signed_sample;
            put_u16_le(block + index * 2U, sample);
        }
        if (fwrite(block, 2U, frames, file) != frames) {
            (void)fclose(file);
            (void)remove(state->path);
            bench_error(error, error_size, "could not write decode fixture");
            return 0;
        }
        written += (uint64_t)frames;
    }
    if (fclose(file) != 0) {
        (void)remove(state->path);
        bench_error(error, error_size, "could not close decode fixture");
        return 0;
    }
    return 1;
}

static void decode_cleanup(HWADecodeState *state) {
    if (state->path[0] != '\0') {
        (void)remove(state->path);
        state->path[0] = '\0';
    }
}

static int run_decode(void *context, uint64_t *work, uint64_t *checksum,
                      char *error, size_t error_size) {
    HWADecodeState *state = (HWADecodeState *)context;
    HWAWavReader reader;
    unsigned char *raw;
    size_t raw_size;
    uint64_t decoded_frames = 0U;
    uint64_t hash = UINT64_C(1469598103934665603);

    if (hwa_wav_reader_open(&reader, state->path, UINT64_C(1048576),
                            error, error_size) != 0) {
        return 0;
    }
    raw_size = (size_t)reader.format.block_align * 4096U;
    raw = (unsigned char *)malloc(raw_size);
    if (raw == NULL) {
        hwa_wav_reader_close(&reader);
        bench_error(error, error_size, "out of memory for decode buffer");
        return 0;
    }
    for (;;) {
        size_t frames_read = 0U;
        size_t frame;
        if (hwa_wav_reader_read_frames(&reader, raw, 4096U, &frames_read,
                                       error, error_size) != 0) {
            free(raw);
            hwa_wav_reader_close(&reader);
            return 0;
        }
        for (frame = 0U; frame < frames_read; ++frame) {
            uint16_t channel;
            for (channel = 0U; channel < reader.format.channels; ++channel) {
                int clipped = 0;
                const unsigned char *sample =
                    raw + frame * (size_t)reader.format.block_align +
                    (size_t)channel * reader.bytes_per_sample;
                const double value = hwa_wav_decode_sample(&reader, sample,
                                                           &clipped);
                hash = checksum_mix(hash, double_bits(value));
                hash = checksum_mix(hash, (uint64_t)(unsigned int)clipped);
            }
        }
        decoded_frames += (uint64_t)frames_read;
        if (frames_read == 0U) {
            break;
        }
    }
    free(raw);
    hwa_wav_reader_close(&reader);
    if (decoded_frames != state->frames) {
        bench_error(error, error_size, "decode fixture frame count changed");
        return 0;
    }
    *work = decoded_frames * (uint64_t)state->channels;
    *checksum = hash;
    return 1;
}

static int fft_setup(HWAFFTState *state, char *error, size_t error_size) {
    size_t index;
    memset(state, 0, sizeof(*state));
    state->source = (HwaDspComplex *)calloc(HWA_BENCH_FFT_SIZE,
                                            sizeof(*state->source));
    state->work = (HwaDspComplex *)calloc(HWA_BENCH_FFT_SIZE,
                                          sizeof(*state->work));
    if (state->source == NULL || state->work == NULL) {
        free(state->source);
        free(state->work);
        memset(state, 0, sizeof(*state));
        bench_error(error, error_size, "out of memory for FFT fixture");
        return 0;
    }
    for (index = 0U; index < HWA_BENCH_FFT_SIZE; ++index) {
        const double phase = 2.0 * HWA_BENCH_PI * (double)index /
                             (double)HWA_BENCH_FFT_SIZE;
        state->source[index].real =
            0.60 * sin(17.0 * phase) + 0.25 * cos(113.0 * phase);
        state->source[index].imag = 0.05 * sin(7.0 * phase);
    }
    return 1;
}

static void fft_cleanup(HWAFFTState *state) {
    free(state->source);
    free(state->work);
    memset(state, 0, sizeof(*state));
}

static int run_fft(void *context, uint64_t *work, uint64_t *checksum,
                   char *error, size_t error_size) {
    HWAFFTState *state = (HWAFFTState *)context;
    uint64_t hash = UINT64_C(0x84222325cbf29ce4);
    size_t repeat;
    for (repeat = 0U; repeat < HWA_BENCH_FFT_REPEATS; ++repeat) {
        (void)memcpy(state->work, state->source,
                     HWA_BENCH_FFT_SIZE * sizeof(*state->work));
        if (hwa_dsp_fft(state->work, HWA_BENCH_FFT_SIZE, 0) != HWA_DSP_OK) {
            bench_error(error, error_size, "FFT benchmark failed");
            return 0;
        }
        hash = checksum_mix(hash, double_bits(state->work[repeat].real));
        hash = checksum_mix(hash, double_bits(state->work[repeat].imag));
    }
    *work = (uint64_t)HWA_BENCH_FFT_REPEATS *
            ((uint64_t)HWA_BENCH_FFT_SIZE / 2U) * 12U;
    *checksum = hash;
    return 1;
}

static int feature_setup(HWAFeatureState *state, uint64_t frames,
                         uint16_t channels, int pitch, size_t max_lag,
                         int work_kind, char *error, size_t error_size) {
    uint64_t frame;
    uint64_t item_count;
    uint32_t noise = UINT32_C(0x12345678);

    memset(state, 0, sizeof(*state));
    state->frames = frames;
    state->channels = channels;
    state->sample_rate = 48000U;
    state->work_kind = work_kind;
    if (frames > SIZE_MAX / (uint64_t)channels) {
        bench_error(error, error_size, "feature fixture is too large");
        return 0;
    }
    item_count = frames * (uint64_t)channels;
    state->samples = (double *)calloc((size_t)item_count,
                                      sizeof(*state->samples));
    if (state->samples == NULL) {
        bench_error(error, error_size, "out of memory for feature fixture");
        return 0;
    }
    for (frame = 0U; frame < frames; ++frame) {
        const double time = (double)frame / (double)state->sample_rate;
        double left;
        if (channels == 1U) {
            left = 0.45 * sin(2.0 * HWA_BENCH_PI * 440.0 * time) +
                   0.12 * sin(2.0 * HWA_BENCH_PI * 660.0 * time);
            state->samples[(size_t)frame] = left;
        } else {
            noise = noise * UINT32_C(1664525) + UINT32_C(1013904223);
            left = ((double)(noise >> 8U) / 16777215.0 - 0.5) * 0.8;
            state->samples[(size_t)(frame * 2U)] = left;
            if (frame < 17U) {
                state->samples[(size_t)(frame * 2U + 1U)] = 0.25;
            } else {
                state->samples[(size_t)(frame * 2U + 1U)] =
                    state->samples[(size_t)((frame - 17U) * 2U)];
            }
        }
    }
    hwa_analysis_options_default(&state->options);
    state->options.max_input_frames = frames;
    state->options.max_work_bytes = UINT64_C(536870912);
    state->options.max_transforms = 100000U;
    state->options.max_track_points = 100000U;
    state->options.max_spectrum_values = 1000000U;
    state->options.max_lag_samples = max_lag;
    state->options.true_peak_oversample = 1U;
    state->options.collect_tracks = pitch;
    state->options.collect_spectrogram = 0;
    if (!minimum_feature_work(state->sample_rate, channels, frames,
                              &state->options,
                              &state->tracked_work_bytes)) {
        free(state->samples);
        memset(state, 0, sizeof(*state));
        bench_error(error, error_size,
                    "could not measure the feature work boundary");
        return 0;
    }
    return 1;
}

static void feature_cleanup(HWAFeatureState *state) {
    free(state->samples);
    memset(state, 0, sizeof(*state));
}

static int run_features(void *context, uint64_t *work, uint64_t *checksum,
                        char *error, size_t error_size) {
    HWAFeatureState *state = (HWAFeatureState *)context;
    HWAFeatureProcessor *processor = NULL;
    HWAAnalysis analysis;
    uint64_t offset = 0U;
    uint64_t hash = UINT64_C(0xa0761d6478bd642f);

    memset(&analysis, 0, sizeof(analysis));
    if (hwa_features_create(&processor, state->sample_rate, state->channels,
                            0U, state->frames, &state->options,
                            error, error_size) != 0) {
        return 0;
    }
    while (offset < state->frames) {
        size_t block = (size_t)(state->frames - offset);
        if (block > 4096U) {
            block = 4096U;
        }
        if (hwa_features_push(
                processor,
                state->samples + (size_t)(offset * (uint64_t)state->channels),
                NULL, block, error, error_size) != 0) {
            hwa_features_destroy(processor);
            return 0;
        }
        offset += (uint64_t)block;
    }
    if (hwa_features_finish(processor, &analysis, error, error_size) != 0) {
        hwa_features_destroy(processor);
        return 0;
    }
    hwa_features_destroy(processor);

    hash = checksum_mix(hash, double_bits(analysis.channels[0].rms));
    hash = checksum_mix(hash, double_bits(analysis.spectrum.centroid_hz));
    hash = checksum_mix(hash, double_bits(analysis.stereo.interchannel_delay_samples));
    hash = checksum_mix(hash, (uint64_t)analysis.track_count);
    hash = checksum_mix(hash, analysis.spectrum.transform_count);
    if (state->work_kind == 0) {
        *work = analysis.spectrum.transform_count;
    } else if (state->work_kind == 1) {
        *work = (uint64_t)analysis.track_count;
    } else {
        const uint64_t lag = (uint64_t)state->options.max_lag_samples;
        *work = state->frames * (2U * lag + 1U) - lag * (lag + 1U);
    }
    *checksum = hash;
    hwa_analysis_free(&analysis);
    return *work != 0U;
}

static void fill_align_frame(HWAAlignFrame *frame, size_t index,
                             size_t source_count, size_t mapped_count) {
    const size_t mapped = index * source_count / mapped_count;
    const unsigned int note = (unsigned int)((mapped / 20U) % 12U);
    memset(frame, 0, sizeof(*frame));
    frame->time_seconds = ((double)index + 0.5) * 0.05;
    frame->chroma[note] = 1.0;
    frame->pitch_class = (double)note;
    frame->pitch_confidence = 1.0;
    frame->activity = 1.0;
    frame->log_energy = -18.0 + (double)(mapped % 7U) * 0.25;
    frame->evidence_flags =
        HWA_ALIGNMENT_EVIDENCE_CHROMA |
        HWA_ALIGNMENT_EVIDENCE_PITCH |
        HWA_ALIGNMENT_EVIDENCE_ENVELOPE |
        HWA_ALIGNMENT_EVIDENCE_SPECTRAL_ONSET |
        HWA_ALIGNMENT_EVIDENCE_ENERGY_ONSET;
    if (mapped % 20U == 0U) {
        frame->spectral_onset = 1.0;
        frame->energy_onset = 1.0;
        frame->combined_onset = 1.0;
    }
    frame->event_index = SIZE_MAX;
}

static int alignment_setup(HWAAlignmentState *state,
                           char *error, size_t error_size) {
    const size_t reference_count = 1200U;
    const size_t target_count = 1260U;
    size_t index;

    memset(state, 0, sizeof(*state));
    state->reference_frames = (HWAAlignFrame *)calloc(
        reference_count, sizeof(*state->reference_frames));
    state->target_frames = (HWAAlignFrame *)calloc(
        target_count, sizeof(*state->target_frames));
    if (state->reference_frames == NULL || state->target_frames == NULL) {
        free(state->reference_frames);
        free(state->target_frames);
        memset(state, 0, sizeof(*state));
        bench_error(error, error_size, "out of memory for alignment fixture");
        return 0;
    }
    for (index = 0U; index < reference_count; ++index) {
        fill_align_frame(&state->reference_frames[index], index,
                         reference_count, reference_count);
    }
    for (index = 0U; index < target_count; ++index) {
        fill_align_frame(&state->target_frames[index], index,
                         reference_count, target_count);
    }
    state->reference.frames = state->reference_frames;
    state->reference.frame_count = reference_count;
    state->reference.step_seconds = 0.05;
    state->reference.duration_seconds = (double)reference_count * 0.05;
    state->reference.tuning_confidence = 1.0;
    state->target.frames = state->target_frames;
    state->target.frame_count = target_count;
    state->target.step_seconds = 0.05;
    state->target.duration_seconds = (double)target_count * 0.05;
    state->target.tuning_confidence = 1.0;

    hwa_alignment_options_default(&state->options);
    state->options.dtw_band_seconds = 3.0;
    state->options.fine_radius_seconds = 0.50;
    state->options.refine_radius_seconds = 0.10;
    state->options.max_dtw_cells = UINT64_C(2000000);
    state->options.max_alignment_work_bytes = UINT64_C(268435456);
    state->options.max_alignment_points = 10000U;
    return 1;
}

static void alignment_cleanup(HWAAlignmentState *state) {
    free(state->reference_frames);
    free(state->target_frames);
    memset(state, 0, sizeof(*state));
}

static int run_alignment(void *context, uint64_t *work, uint64_t *checksum,
                         char *error, size_t error_size) {
    HWAAlignmentState *state = (HWAAlignmentState *)context;
    HWAAlignment result;
    uint64_t hash = UINT64_C(0xe7037ed1a0b428db);
    if (hwa_align_tracks(&state->reference, &state->target, &state->options,
                         NULL, 0U, &result, error, error_size) != 0) {
        return 0;
    }
    hash = checksum_mix(hash, result.dtw_cells);
    hash = checksum_mix(hash, result.path_points);
    hash = checksum_mix(hash, double_bits(result.normalized_cost));
    hash = checksum_mix(hash, double_bits(result.global_confidence));
    *work = result.dtw_cells;
    *checksum = hash;
    hwa_alignment_free(&result);
    return *work != 0U;
}

static void json_setup(HWAJsonState *state) {
    size_t index;
    memset(state, 0, sizeof(*state));
    (void)snprintf(
        state->path, sizeof(state->path),
        "benchmark/a-deliberately-long-input-name-used-to-cover-json-"
        "escaping-and-output-throughput/audio-analysis-0001.wav");
    state->analysis.path = state->path;
    state->analysis.format.container = HWA_CONTAINER_RIFF;
    state->analysis.format.encoding = HWA_ENCODING_PCM;
    state->analysis.format.channels = 8U;
    state->analysis.format.sample_rate_hz = 96000U;
    state->analysis.format.bits_per_sample = 24U;
    state->analysis.format.valid_bits_per_sample = 24U;
    state->analysis.format.block_align = 24U;
    state->analysis.format.frames = UINT64_C(5760000);
    state->analysis.format.data_bytes = UINT64_C(138240000);
    state->analysis.format.duration_seconds = 60.0;
    hwa_analysis_options_default(&state->analysis.options);
    state->analysis.analyzed_channels = 8U;
    state->analysis.channels = state->channels;
    for (index = 0U; index < 8U; ++index) {
        state->channels[index].peak = 0.8 - (double)index * 0.01;
        state->channels[index].true_peak = 0.81 - (double)index * 0.01;
        state->channels[index].rms = 0.2 + (double)index * 0.005;
        state->channels[index].dc_offset = (double)index * 0.0001;
        state->channels[index].crest_factor = 4.0;
        state->channels[index].zero_crossing_rate = 0.11;
        state->channels[index].crest_factor_valid = 1;
        state->channels[index].true_peak_valid = 1;
    }
    state->analysis.loudness.integrated_lufs = -18.2;
    state->analysis.loudness.loudness_range_lu = 8.5;
    state->analysis.loudness.momentary_max_lufs = -12.5;
    state->analysis.loudness.short_term_max_lufs = -14.1;
    state->analysis.loudness.integrated_valid = 1;
    state->analysis.loudness.range_valid = 1;
    state->analysis.loudness.momentary_valid = 1;
    state->analysis.loudness.short_term_valid = 1;
    state->analysis.spectrum.centroid_hz = 1820.0;
    state->analysis.spectrum.spread_hz = 930.0;
    state->analysis.spectrum.rolloff_85_hz = 4200.0;
    state->analysis.spectrum.flatness = 0.13;
    state->analysis.spectrum.slope_db_per_octave = -4.2;
    state->analysis.spectrum.mean_flux = 0.08;
    state->analysis.spectrum.transform_count = UINT64_C(89984);
    state->analysis.spectrum.valid = 1;
    for (index = 0U; index < HWA_BAND_COUNT; ++index) {
        state->analysis.spectrum.band_power[index] =
            0.01 / ((double)index + 1.0);
        state->analysis.stereo.band_width[index] =
            0.2 + (double)index * 0.03;
    }
    state->analysis.activity.threshold_dbfs = -60.0;
    state->analysis.activity.silence_fraction = 0.03;
    state->analysis.activity.active_start_seconds = 0.1;
    state->analysis.activity.active_end_seconds = 59.8;
    state->analysis.activity.classified_valid = 1;
    state->analysis.activity.active_span_valid = 1;
    state->analysis.stereo.available = 1;
    state->analysis.stereo.level_valid = 1;
    state->analysis.stereo.width_valid = 1;
    state->analysis.stereo.correlation_valid = 1;
    state->analysis.stereo.delay_valid = 1;
    state->analysis.stereo.correlation = 0.72;
    state->analysis.stereo.mid_rms = 0.19;
    state->analysis.stereo.side_rms = 0.08;
    state->analysis.stereo.balance_db = -0.2;
    state->analysis.stereo.width_ratio = 0.42;
    state->analysis.stereo.interchannel_delay_samples = 17.0;
    state->analysis.stereo.interchannel_delay_seconds = 17.0 / 96000.0;
    state->analysis.stereo.interchannel_delay_confidence = 0.91;
    state->analysis.stereo.band_width_valid_mask =
        (uint16_t)(((uint16_t)1U << HWA_BAND_COUNT) - 1U);
}

static int run_json(void *context, uint64_t *work, uint64_t *checksum,
                    char *error, size_t error_size) {
    HWAJsonState *state = (HWAJsonState *)context;
    FILE *file = tmpfile();
    size_t repeat;
    long output_size;
    if (file == NULL) {
        bench_error(error, error_size, "could not open JSON output stream");
        return 0;
    }
    for (repeat = 0U; repeat < HWA_BENCH_JSON_REPEATS; ++repeat) {
        if (hwa_report_analysis_json(file, &state->analysis) != 0 ||
            fputc('\n', file) == EOF) {
            (void)fclose(file);
            bench_error(error, error_size, "JSON report benchmark failed");
            return 0;
        }
    }
    output_size = ftell(file);
    if (output_size <= 0L || fclose(file) != 0) {
        bench_error(error, error_size, "could not finish JSON output");
        return 0;
    }
    *work = (uint64_t)output_size;
    *checksum = checksum_mix(UINT64_C(0x8ebc6af09c88c6e3), *work);
    return 1;
}

static int run_sample(HWABenchCase *benchmark, double minimum_seconds,
                      double *seconds_per_iteration,
                      double *units_per_second,
                      char *error, size_t error_size) {
    uint64_t repetitions = 1U;
    for (;;) {
        uint64_t repeat;
        double start = now_seconds();
        double finish;
        if (start < 0.0) {
            bench_error(error, error_size, "monotonic clock failed");
            return 0;
        }
        for (repeat = 0U; repeat < repetitions; ++repeat) {
            uint64_t work = 0U;
            uint64_t checksum = 0U;
            if (!benchmark->run(benchmark->context, &work, &checksum,
                                error, error_size)) {
                return 0;
            }
            if (work != benchmark->work_per_iteration ||
                checksum != benchmark->checksum) {
                bench_error(error, error_size,
                            "benchmark work or checksum changed between runs");
                return 0;
            }
            hwa_bench_sink ^= checksum;
        }
        finish = now_seconds();
        if (finish < start) {
            bench_error(error, error_size, "monotonic clock moved backwards");
            return 0;
        }
        if (finish - start >= minimum_seconds ||
            repetitions >= UINT64_C(1073741824)) {
            const double elapsed = finish - start;
            if (!(elapsed > 0.0)) {
                bench_error(error, error_size, "benchmark timer has no resolution");
                return 0;
            }
            *seconds_per_iteration = elapsed / (double)repetitions;
            *units_per_second =
                (double)benchmark->work_per_iteration *
                (double)repetitions / elapsed;
            return 1;
        }
        repetitions *= 2U;
    }
}

static int warm_case(HWABenchCase *benchmark,
                     char *error, size_t error_size) {
    if (!benchmark->run(benchmark->context,
                        &benchmark->work_per_iteration,
                        &benchmark->checksum, error, error_size)) {
        return 0;
    }
    if (benchmark->work_per_iteration == 0U) {
        bench_error(error, error_size, "benchmark reported no work");
        return 0;
    }
    hwa_bench_sink ^= benchmark->checksum;
    return 1;
}

static int parse_count(const char *text, size_t minimum, size_t maximum,
                       size_t *value) {
    char *end = NULL;
    unsigned long long parsed;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed < (unsigned long long)minimum ||
        parsed > (unsigned long long)maximum) {
        return 0;
    }
    *value = (size_t)parsed;
    return 1;
}

static int selected_case(const char *filter, const HWABenchCase *benchmark,
                         int long_enabled) {
    if (filter != NULL) {
        return strcmp(filter, benchmark->name) == 0;
    }
    return !benchmark->long_case || long_enabled;
}

static int write_results(FILE *stream, HWABenchCase *cases, size_t case_count,
                         const char *filter, size_t sample_count,
                         size_t minimum_ms, int long_enabled) {
    size_t case_index;
    int first_case = 1;
    char hostname[256];
    machine_name(hostname, sizeof(hostname));
    if (fputs("{\"schema\":\"hwa-benchmark\",\"schema_version\":1,"
              "\"analyzer_version\":", stream) == EOF ||
        hwa_json_write_string(stream, HWA_VERSION) != 0 ||
        fputs(",\"machine\":{\"hostname\":", stream) == EOF ||
        hwa_json_write_string(stream, hostname) != 0 ||
        fputs(",\"os\":", stream) == EOF ||
        hwa_json_write_string(stream, hwa_build_target_os()) != 0 ||
        fputs(",\"arch\":", stream) == EOF ||
        hwa_json_write_string(stream, architecture_name()) != 0 ||
        fprintf(stream, ",\"pointer_bits\":%u,\"endianness\":",
                hwa_build_pointer_bits()) < 0 ||
        hwa_json_write_string(stream, hwa_build_endianness()) != 0 ||
        fputs(",\"compiler_family\":", stream) == EOF ||
        hwa_json_write_string(stream, hwa_build_compiler_family()) != 0 ||
        fputs(",\"compiler_version\":", stream) == EOF ||
        hwa_json_write_string(stream, hwa_build_compiler_version()) != 0 ||
        fputs(",\"c_standard\":", stream) == EOF ||
        hwa_json_write_string(stream, hwa_build_c_standard()) != 0 ||
        fputs(",\"build_mode\":", stream) == EOF ||
        hwa_json_write_string(stream, hwa_build_mode()) != 0 ||
        fprintf(stream,
                "},\"timer\":\"monotonic_wall_clock\",\"samples_per_case\":%zu,"
                "\"minimum_sample_ms\":%zu,\"long_cases\":%s,\"cases\":[",
                sample_count, minimum_ms, long_enabled ? "true" : "false") < 0) {
        return 0;
    }
    for (case_index = 0U; case_index < case_count; ++case_index) {
        HWABenchCase *benchmark = &cases[case_index];
        size_t sample;
        if (!selected_case(filter, benchmark, long_enabled)) {
            continue;
        }
        if ((!first_case && fputc(',', stream) == EOF) ||
            fputs("{\"name\":", stream) == EOF ||
            hwa_json_write_string(stream, benchmark->name) != 0 ||
            fputs(",\"unit\":", stream) == EOF ||
            hwa_json_write_string(stream, benchmark->unit) != 0 ||
            fprintf(stream,
                    ",\"work_per_iteration\":%" PRIu64
                    ",\"tracked_work_bytes\":%" PRIu64
                    ",\"checksum\":\"%016" PRIx64
                    "\",\"seconds_per_iteration\":[",
                    benchmark->work_per_iteration,
                    benchmark->tracked_work_bytes,
                    benchmark->checksum) < 0) {
            return 0;
        }
        for (sample = 0U; sample < sample_count; ++sample) {
            if ((sample != 0U && fputc(',', stream) == EOF) ||
                fprintf(stream, "%.17g",
                        benchmark->seconds_per_iteration[sample]) < 0) {
                return 0;
            }
        }
        if (fputs("],\"units_per_second\":[", stream) == EOF) {
            return 0;
        }
        for (sample = 0U; sample < sample_count; ++sample) {
            if ((sample != 0U && fputc(',', stream) == EOF) ||
                fprintf(stream, "%.17g",
                        benchmark->units_per_second[sample]) < 0) {
                return 0;
            }
        }
        if (fputs("]}", stream) == EOF) {
            return 0;
        }
        first_case = 0;
    }
    return fprintf(stream, "],\"sink\":\"%016" PRIx64 "\"}\n",
                   (uint64_t)hwa_bench_sink) >= 0;
}

static void print_usage(const char *program) {
    (void)fprintf(
        stderr,
        "usage: %s [--samples N] [--minimum-ms N] [--case NAME] "
        "[--output PATH] [--long] [--list]\n",
        program);
}

int main(int argc, char **argv) {
    HWADecodeState decode_state;
    HWAFFTState fft_state;
    HWASourceState summary_state;
    HWASourceState long_summary_state;
    HWAFeatureState pitch_state;
    HWAFeatureState stereo_state;
    HWAAlignmentState alignment_state;
    HWAJsonState json_state;
    HWABenchCase cases[8];
    const size_t case_count = sizeof(cases) / sizeof(cases[0]);
    size_t sample_count = 7U;
    size_t minimum_ms = 100U;
    const char *filter = NULL;
    const char *output_path = NULL;
    int list_only = 0;
    int long_enabled = 0;
    int run_long_summary = 0;
    int argument;
    char error[HWA_ERROR_SIZE];
    size_t case_index;
    int matched = 0;
    FILE *output = stdout;
    int status = 1;

    for (argument = 1; argument < argc; ++argument) {
        if (strcmp(argv[argument], "--samples") == 0 && argument + 1 < argc) {
            if (!parse_count(argv[++argument], 1U, HWA_BENCH_MAX_SAMPLES,
                             &sample_count)) {
                print_usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[argument], "--minimum-ms") == 0 &&
                   argument + 1 < argc) {
            if (!parse_count(argv[++argument], 1U, 60000U, &minimum_ms)) {
                print_usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[argument], "--case") == 0 &&
                   argument + 1 < argc) {
            filter = argv[++argument];
        } else if (strcmp(argv[argument], "--output") == 0 &&
                   argument + 1 < argc) {
            output_path = argv[++argument];
        } else if (strcmp(argv[argument], "--list") == 0) {
            list_only = 1;
        } else if (strcmp(argv[argument], "--long") == 0) {
            long_enabled = 1;
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    memset(&decode_state, 0, sizeof(decode_state));
    memset(&fft_state, 0, sizeof(fft_state));
    memset(&summary_state, 0, sizeof(summary_state));
    memset(&long_summary_state, 0, sizeof(long_summary_state));
    memset(&pitch_state, 0, sizeof(pitch_state));
    memset(&stereo_state, 0, sizeof(stereo_state));
    memset(&alignment_state, 0, sizeof(alignment_state));
    json_setup(&json_state);
    memset(cases, 0, sizeof(cases));
    cases[0] = (HWABenchCase){"decode.pcm16_mono", "decoded_samples",
                              run_decode, &decode_state, NULL, NULL,
                              0U, 0U, 0U, 0};
    cases[1] = (HWABenchCase){"fft.complex_4096", "butterflies",
                              run_fft, &fft_state, NULL, NULL,
                              0U, 0U, 0U, 0};
    cases[2] = (HWABenchCase){"e2e.summary_mono_10s", "source_frames",
                              run_source_summary, &summary_state, NULL, NULL,
                              0U, 0U, 0U, 0};
    cases[3] = (HWABenchCase){"features.pitch_track", "track_frames",
                              run_features, &pitch_state, NULL, NULL,
                              0U, 0U, 0U, 0};
    cases[4] = (HWABenchCase){"features.stereo_delay_2048", "delay_pairs",
                              run_features, &stereo_state, NULL, NULL,
                              0U, 0U, 0U, 0};
    cases[5] = (HWABenchCase){"alignment.dtw_1200x1260", "dtw_cells",
                              run_alignment, &alignment_state, NULL, NULL,
                              0U, 0U, 0U, 0};
    cases[6] = (HWABenchCase){"json.analysis_report", "output_bytes",
                              run_json, &json_state, NULL, NULL,
                              0U, 0U, 0U, 0};
    cases[7] = (HWABenchCase){"e2e.summary_mono_10min", "source_frames",
                              run_source_summary, &long_summary_state,
                              NULL, NULL, 0U, 0U, 0U, 1};

    if (list_only) {
        for (case_index = 0U; case_index < case_count; ++case_index) {
            (void)puts(cases[case_index].name);
        }
        return 0;
    }
    if (filter != NULL) {
        for (case_index = 0U; case_index < case_count; ++case_index) {
            if (strcmp(filter, cases[case_index].name) == 0) {
                matched = 1;
            }
        }
        if (!matched) {
            (void)fprintf(stderr, "unknown benchmark case: %s\n", filter);
            return 2;
        }
    }

    run_long_summary = long_enabled ||
        (filter != NULL && strcmp(filter, cases[7].name) == 0);

    if (!decode_setup(&decode_state, error, sizeof(error)) ||
        !fft_setup(&fft_state, error, sizeof(error)) ||
        !source_setup(&summary_state, UINT64_C(480000),
                      error, sizeof(error)) ||
        !feature_setup(&pitch_state, UINT64_C(240000), 1U, 1, 32U, 1,
                       error, sizeof(error)) ||
        !feature_setup(&stereo_state, UINT64_C(48000), 2U, 0, 2048U, 2,
                       error, sizeof(error)) ||
        !alignment_setup(&alignment_state, error, sizeof(error))) {
        (void)fprintf(stderr, "benchmark setup failed: %s\n", error);
        goto cleanup;
    }
    cases[2].tracked_work_bytes = summary_state.tracked_work_bytes;
    cases[3].tracked_work_bytes = pitch_state.tracked_work_bytes;
    cases[4].tracked_work_bytes = stereo_state.tracked_work_bytes;
    if (run_long_summary) {
        if (!source_setup(&long_summary_state, UINT64_C(28800000),
                          error, sizeof(error))) {
            (void)fprintf(stderr, "long benchmark setup failed: %s\n", error);
            goto cleanup;
        }
        cases[7].tracked_work_bytes = long_summary_state.tracked_work_bytes;
        if (long_summary_state.tracked_work_bytes !=
            summary_state.tracked_work_bytes) {
            (void)fprintf(stderr,
                          "summary processor work grew with input duration\n");
            goto cleanup;
        }
    }

    for (case_index = 0U; case_index < case_count; ++case_index) {
        HWABenchCase *benchmark = &cases[case_index];
        size_t sample;
        if (!selected_case(filter, benchmark, run_long_summary)) {
            continue;
        }
        benchmark->seconds_per_iteration = (double *)calloc(
            sample_count, sizeof(*benchmark->seconds_per_iteration));
        benchmark->units_per_second = (double *)calloc(
            sample_count, sizeof(*benchmark->units_per_second));
        if (benchmark->seconds_per_iteration == NULL ||
            benchmark->units_per_second == NULL) {
            (void)fprintf(stderr, "out of memory for benchmark samples\n");
            goto cleanup;
        }
        if (!warm_case(benchmark, error, sizeof(error))) {
            (void)fprintf(stderr, "%s warmup failed: %s\n",
                          benchmark->name, error);
            goto cleanup;
        }
        for (sample = 0U; sample < sample_count; ++sample) {
            if (!run_sample(benchmark, (double)minimum_ms / 1000.0,
                            &benchmark->seconds_per_iteration[sample],
                            &benchmark->units_per_second[sample],
                            error, sizeof(error))) {
                (void)fprintf(stderr, "%s failed: %s\n",
                              benchmark->name, error);
                goto cleanup;
            }
        }
        (void)fprintf(stderr, "completed %s\n", benchmark->name);
    }

    if (output_path != NULL) {
        output = fopen(output_path, "wb");
        if (output == NULL) {
            (void)fprintf(stderr, "could not open output: %s\n", output_path);
            goto cleanup;
        }
    }
    if (!write_results(output, cases, case_count, filter,
                       sample_count, minimum_ms, run_long_summary)) {
        (void)fprintf(stderr, "could not write benchmark JSON\n");
        goto cleanup;
    }
    if (output != stdout) {
        if (fclose(output) != 0) {
            output = stdout;
            (void)fprintf(stderr, "could not close benchmark JSON\n");
            goto cleanup;
        }
        output = stdout;
    }
    status = 0;

cleanup:
    if (output != stdout) {
        (void)fclose(output);
    }
    for (case_index = 0U; case_index < case_count; ++case_index) {
        free(cases[case_index].seconds_per_iteration);
        free(cases[case_index].units_per_second);
    }
    alignment_cleanup(&alignment_state);
    feature_cleanup(&stereo_state);
    feature_cleanup(&pitch_state);
    fft_cleanup(&fft_state);
    decode_cleanup(&decode_state);
    return status;
}

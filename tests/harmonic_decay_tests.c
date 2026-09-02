#include "hlolli_wg_analyzer.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#define HWA_TEST_UNLINK _unlink
#else
#include <unistd.h>
#define HWA_TEST_UNLINK unlink
#endif

#define TEST_RATE 48000U
#define TEST_FUNDAMENTAL_HZ 220.0
#define TEST_HARMONICS 6U

static int failures;

#define CHECK(condition, message)                                      \
    do {                                                               \
        if (!(condition)) {                                            \
            (void)fprintf(stderr, "FAIL: %s\n", message);             \
            failures++;                                                \
        }                                                              \
    } while (0)

static long test_process_id(void)
{
#if defined(_WIN32)
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}

static const char *test_temporary_root(void)
{
#if defined(_WIN32)
    const char *root = getenv("TEMP");
    return root != NULL && root[0] != '\0' ? root : ".";
#else
    return "/tmp";
#endif
}

static int write_bytes(FILE *stream, const void *bytes, size_t size)
{
    return fwrite(bytes, 1U, size, stream) == size;
}

static int write_u16(FILE *stream, uint16_t value)
{
    unsigned char bytes[2];
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)(value >> 8U);
    return write_bytes(stream, bytes, sizeof(bytes));
}

static int write_u32(FILE *stream, uint32_t value)
{
    unsigned char bytes[4];
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xffU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xffU);
    bytes[3] = (unsigned char)(value >> 24U);
    return write_bytes(stream, bytes, sizeof(bytes));
}

static int write_i24(FILE *stream, int32_t value)
{
    unsigned char bytes[3];
    uint32_t encoded = (uint32_t)value & UINT32_C(0x00ffffff);
    bytes[0] = (unsigned char)(encoded & 0xffU);
    bytes[1] = (unsigned char)((encoded >> 8U) & 0xffU);
    bytes[2] = (unsigned char)((encoded >> 16U) & 0xffU);
    return write_bytes(stream, bytes, sizeof(bytes));
}

static int write_harmonic_variant(const char *path,
                                  double fundamental_hz,
                                  const double *taus,
                                  const double *gains,
                                  double lead_seconds,
                                  double duration_seconds,
                                  size_t channels,
                                  const double *channel_scales)
{
    const uint32_t frames = (uint32_t)llround(
        (double)TEST_RATE * duration_seconds);
    const uint32_t lead = (uint32_t)llround(
        lead_seconds * (double)TEST_RATE);
    const uint32_t data_bytes = frames * (uint32_t)channels * 2U;
    FILE *stream = fopen(path, "wb");
    uint32_t frame;

    if (stream == NULL || channels < 1U || channels > 2U) return 0;
    if (!write_bytes(stream, "RIFF", 4U) ||
        !write_u32(stream, 36U + data_bytes) ||
        !write_bytes(stream, "WAVEfmt ", 8U) ||
        !write_u32(stream, 16U) || !write_u16(stream, 1U) ||
        !write_u16(stream, (uint16_t)channels) ||
        !write_u32(stream, TEST_RATE) ||
        !write_u32(stream, TEST_RATE * (uint32_t)channels * 2U) ||
        !write_u16(stream, (uint16_t)(channels * 2U)) ||
        !write_u16(stream, 16U) || !write_bytes(stream, "data", 4U) ||
        !write_u32(stream, data_bytes)) {
        (void)fclose(stream);
        return 0;
    }
    for (frame = 0U; frame < frames; ++frame) {
        double value = 0.0;
        if (frame >= lead) {
            size_t harmonic;
            double time = (double)(frame - lead) / (double)TEST_RATE;
            for (harmonic = 1U; harmonic <= TEST_HARMONICS; ++harmonic) {
                double phase = 0.13 * (double)harmonic;
                value += gains[harmonic - 1U] *
                    exp(-time / taus[harmonic - 1U]) * sin(
                    6.2831853071795864769 * fundamental_hz *
                    (double)harmonic * time + phase);
            }
        }
        {
            size_t channel;
            for (channel = 0U; channel < channels; ++channel) {
                double scaled = value * channel_scales[channel];
                int16_t encoded;
                if (scaled > 1.0) scaled = 1.0;
                if (scaled < -1.0) scaled = -1.0;
                encoded = (int16_t)lrint(scaled * 32767.0);
                if (!write_u16(stream, (uint16_t)encoded)) {
                    (void)fclose(stream);
                    return 0;
                }
            }
        }
    }
    return fclose(stream) == 0;
}

static int write_harmonic_wave(const char *path,
                               double tau_seconds,
                               double lead_seconds)
{
    double taus[TEST_HARMONICS];
    double gains[TEST_HARMONICS];
    const double channel_scales[1] = {1.0};
    size_t harmonic;
    for (harmonic = 0U; harmonic < TEST_HARMONICS; ++harmonic) {
        taus[harmonic] = tau_seconds;
        gains[harmonic] = 0.172 - 0.018 * (double)harmonic;
    }
    return write_harmonic_variant(
        path, TEST_FUNDAMENTAL_HZ, taus, gains, lead_seconds, 3.0,
        1U, channel_scales);
}

static int write_shallow_range_harmonic_wave(const char *path)
{
    const uint32_t frames = TEST_RATE * 3U;
    const uint32_t lead = TEST_RATE / 10U;
    const uint32_t level_drop = TEST_RATE / 2U;
    const uint32_t data_bytes = frames * 2U;
    FILE *stream = fopen(path, "wb");
    uint32_t frame;

    if (stream == NULL) return 0;
    if (!write_bytes(stream, "RIFF", 4U) ||
        !write_u32(stream, 36U + data_bytes) ||
        !write_bytes(stream, "WAVEfmt ", 8U) ||
        !write_u32(stream, 16U) || !write_u16(stream, 1U) ||
        !write_u16(stream, 1U) || !write_u32(stream, TEST_RATE) ||
        !write_u32(stream, TEST_RATE * 2U) || !write_u16(stream, 2U) ||
        !write_u16(stream, 16U) || !write_bytes(stream, "data", 4U) ||
        !write_u32(stream, data_bytes)) {
        (void)fclose(stream);
        return 0;
    }
    for (frame = 0U; frame < frames; ++frame) {
        double value = 0.0;
        if (frame >= lead) {
            size_t harmonic;
            double time = (double)(frame - lead) / (double)TEST_RATE;
            double scale = exp(-time / 0.4) *
                           (frame < level_drop ? 1.0 : 0.005);
            for (harmonic = 1U; harmonic <= TEST_HARMONICS; ++harmonic) {
                double gain = 0.172 - 0.018 * (double)(harmonic - 1U);
                value += scale * gain * sin(
                    6.2831853071795864769 * TEST_FUNDAMENTAL_HZ *
                    (double)harmonic * time + 0.13 * (double)harmonic);
            }
        }
        if (value > 1.0) value = 1.0;
        if (value < -1.0) value = -1.0;
        if (!write_u16(stream, (uint16_t)(int16_t)lrint(value * 32767.0))) {
            (void)fclose(stream);
            return 0;
        }
    }
    return fclose(stream) == 0;
}

static int write_colored_harmonic_wave(const char *path,
                                       int modal,
                                       int short_filter,
                                       double lead_seconds)
{
    const uint32_t frames = TEST_RATE * 3U;
    const uint32_t lead = (uint32_t)llround(
        lead_seconds * (double)TEST_RATE);
    const uint32_t data_bytes = frames * 2U;
    double *samples = (double *)calloc(frames, sizeof(*samples));
    FILE *stream;
    uint32_t frame;

    if (samples == NULL) return 0;
    for (frame = lead; frame < frames; ++frame) {
        size_t harmonic;
        double time = (double)(frame - lead) / (double)TEST_RATE;
        for (harmonic = 1U; harmonic <= TEST_HARMONICS; ++harmonic) {
            double gain = 0.172 - 0.018 * (double)(harmonic - 1U);
            double base = TEST_FUNDAMENTAL_HZ * (double)harmonic;
            double envelope = exp(-time / 0.45);
            if (modal) {
                samples[frame] += gain * envelope * (
                    0.62 * sin(6.2831853071795864769 *
                               (base - 1.7) * time +
                               0.19 * (double)harmonic) +
                    0.38 * sin(6.2831853071795864769 *
                               (base + 1.7) * time -
                               0.23 * (double)harmonic));
            } else {
                samples[frame] += gain * envelope * sin(
                    6.2831853071795864769 * base * time +
                    0.13 * (double)harmonic);
            }
        }
    }
    if (short_filter) {
        for (frame = frames; frame-- > 0U;) {
            double value = 0.72 * samples[frame];
            if (frame >= 47U) value += 0.20 * samples[frame - 47U];
            if (frame >= 211U) value -= 0.12 * samples[frame - 211U];
            samples[frame] = value;
        }
    }
    stream = fopen(path, "wb");
    if (stream == NULL ||
        !write_bytes(stream, "RIFF", 4U) ||
        !write_u32(stream, 36U + data_bytes) ||
        !write_bytes(stream, "WAVEfmt ", 8U) ||
        !write_u32(stream, 16U) || !write_u16(stream, 1U) ||
        !write_u16(stream, 1U) || !write_u32(stream, TEST_RATE) ||
        !write_u32(stream, TEST_RATE * 2U) || !write_u16(stream, 2U) ||
        !write_u16(stream, 16U) || !write_bytes(stream, "data", 4U) ||
        !write_u32(stream, data_bytes)) {
        if (stream != NULL) (void)fclose(stream);
        free(samples);
        return 0;
    }
    for (frame = 0U; frame < frames; ++frame) {
        double value = samples[frame];
        int16_t encoded;
        if (value > 1.0) value = 1.0;
        if (value < -1.0) value = -1.0;
        encoded = (int16_t)lrint(value * 32767.0);
        if (!write_u16(stream, (uint16_t)encoded)) {
            (void)fclose(stream);
            free(samples);
            return 0;
        }
    }
    free(samples);
    return fclose(stream) == 0;
}

static int write_extensible_pcm24_harmonic_wave(const char *path)
{
    const uint32_t frames = TEST_RATE * 3U;
    const uint32_t lead = TEST_RATE / 5U;
    const uint32_t data_bytes = frames * 6U;
    const unsigned char pcm_guid[16] = {
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
        0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
    };
    FILE *stream = fopen(path, "wb");
    uint32_t frame;

    if (stream == NULL) return 0;
    if (!write_bytes(stream, "RIFF", 4U) ||
        !write_u32(stream, 60U + data_bytes) ||
        !write_bytes(stream, "WAVEfmt ", 8U) ||
        !write_u32(stream, 40U) || !write_u16(stream, 0xfffeU) ||
        !write_u16(stream, 2U) || !write_u32(stream, TEST_RATE) ||
        !write_u32(stream, TEST_RATE * 6U) || !write_u16(stream, 6U) ||
        !write_u16(stream, 24U) || !write_u16(stream, 22U) ||
        !write_u16(stream, 24U) || !write_u32(stream, 3U) ||
        !write_bytes(stream, pcm_guid, sizeof(pcm_guid)) ||
        !write_bytes(stream, "data", 4U) || !write_u32(stream, data_bytes)) {
        (void)fclose(stream);
        return 0;
    }
    for (frame = 0U; frame < frames; ++frame) {
        double value = 0.0;
        if (frame >= lead) {
            size_t harmonic;
            double time = (double)(frame - lead) / (double)TEST_RATE;
            for (harmonic = 1U; harmonic <= TEST_HARMONICS; ++harmonic) {
                double gain = 0.172 - 0.018 * (double)(harmonic - 1U);
                value += gain * exp(-time / 0.45) * sin(
                    6.2831853071795864769 * TEST_FUNDAMENTAL_HZ *
                    (double)harmonic * time + 0.13 * (double)harmonic);
            }
        }
        if (!write_i24(stream, (int32_t)llround(
                                  fmax(-1.0, fmin(1.0, value)) * 8388607.0)) ||
            !write_i24(stream, (int32_t)llround(
                                  fmax(-1.0, fmin(1.0, -0.61 * value)) *
                                  8388607.0))) {
            (void)fclose(stream);
            return 0;
        }
    }
    return fclose(stream) == 0;
}

static int write_two_pluck_wave(const char *path)
{
    const uint32_t frames = TEST_RATE * 3U;
    const uint32_t first = TEST_RATE / 10U;
    const uint32_t second = TEST_RATE * 6U / 5U;
    const uint32_t data_bytes = frames * 2U;
    FILE *stream = fopen(path, "wb");
    uint32_t frame;

    if (stream == NULL) return 0;
    if (!write_bytes(stream, "RIFF", 4U) ||
        !write_u32(stream, 36U + data_bytes) ||
        !write_bytes(stream, "WAVEfmt ", 8U) ||
        !write_u32(stream, 16U) || !write_u16(stream, 1U) ||
        !write_u16(stream, 1U) || !write_u32(stream, TEST_RATE) ||
        !write_u32(stream, TEST_RATE * 2U) || !write_u16(stream, 2U) ||
        !write_u16(stream, 16U) || !write_bytes(stream, "data", 4U) ||
        !write_u32(stream, data_bytes)) {
        (void)fclose(stream);
        return 0;
    }
    for (frame = 0U; frame < frames; ++frame) {
        double value = 0.0;
        size_t event;
        const uint32_t events[2] = {first, second};
        for (event = 0U; event < 2U; ++event) {
            if (frame >= events[event]) {
                size_t harmonic;
                double time = (double)(frame - events[event]) /
                              (double)TEST_RATE;
                for (harmonic = 1U; harmonic <= TEST_HARMONICS; ++harmonic) {
                    double gain = 0.172 - 0.018 * (double)(harmonic - 1U);
                    value += gain * exp(-time / 0.35) * sin(
                        6.2831853071795864769 * TEST_FUNDAMENTAL_HZ *
                        (double)harmonic * time +
                        0.13 * (double)harmonic);
                }
            }
        }
        if (value > 1.0) value = 1.0;
        if (value < -1.0) value = -1.0;
        if (!write_u16(stream, (uint16_t)(int16_t)lrint(value * 32767.0))) {
            (void)fclose(stream);
            return 0;
        }
    }
    return fclose(stream) == 0;
}

static int write_close_slow_repluck_wave(const char *path)
{
    const uint32_t frames = TEST_RATE * 3U;
    const uint32_t first = TEST_RATE / 10U;
    const uint32_t second = TEST_RATE * 2U / 5U;
    const uint32_t data_bytes = frames * 2U;
    FILE *stream = fopen(path, "wb");
    uint32_t frame;

    if (stream == NULL) return 0;
    if (!write_bytes(stream, "RIFF", 4U) ||
        !write_u32(stream, 36U + data_bytes) ||
        !write_bytes(stream, "WAVEfmt ", 8U) ||
        !write_u32(stream, 16U) || !write_u16(stream, 1U) ||
        !write_u16(stream, 1U) || !write_u32(stream, TEST_RATE) ||
        !write_u32(stream, TEST_RATE * 2U) || !write_u16(stream, 2U) ||
        !write_u16(stream, 16U) || !write_bytes(stream, "data", 4U) ||
        !write_u32(stream, data_bytes)) {
        (void)fclose(stream);
        return 0;
    }
    for (frame = 0U; frame < frames; ++frame) {
        double value = 0.0;
        size_t harmonic;
        if (frame >= first) {
            double time = (double)(frame - first) / (double)TEST_RATE;
            for (harmonic = 1U; harmonic <= TEST_HARMONICS; ++harmonic) {
                double gain = 0.172 - 0.018 * (double)(harmonic - 1U);
                value += gain * exp(-time / 0.45) * sin(
                    6.2831853071795864769 * TEST_FUNDAMENTAL_HZ *
                    (double)harmonic * time + 0.13 * (double)harmonic);
            }
        }
        if (frame >= second) {
            double time = (double)(frame - second) / (double)TEST_RATE;
            for (harmonic = 1U; harmonic <= TEST_HARMONICS; ++harmonic) {
                double gain = 0.5 *
                    (0.172 - 0.018 * (double)(harmonic - 1U));
                value += gain * exp(-time / 1.0) * sin(
                    6.2831853071795864769 * TEST_FUNDAMENTAL_HZ *
                    (double)harmonic * time + 0.13 * (double)harmonic);
            }
        }
        if (value > 1.0) value = 1.0;
        if (value < -1.0) value = -1.0;
        if (!write_u16(stream, (uint16_t)(int16_t)lrint(value * 32767.0))) {
            (void)fclose(stream);
            return 0;
        }
    }
    return fclose(stream) == 0;
}

static void test_known_exponential_t60_per_band(void)
{
    char reference_path[256];
    char model_path[256];
    HWAHarmonicDecayOptions options;
    HWAHarmonicDecayResult result;
    char error[HWA_ERROR_SIZE] = {0};
    const double expected_t60 = 3.108489875541961;
    size_t index;

    (void)snprintf(reference_path, sizeof(reference_path),
                   "%s/hwa-harmonic-reference-%ld.wav",
                   test_temporary_root(), test_process_id());
    (void)snprintf(model_path, sizeof(model_path),
                   "%s/hwa-harmonic-model-%ld.wav",
                   test_temporary_root(), test_process_id());
    (void)HWA_TEST_UNLINK(reference_path);
    (void)HWA_TEST_UNLINK(model_path);
    CHECK(write_harmonic_wave(reference_path, 0.45, 0.10),
          "write harmonic reference");
    CHECK(write_harmonic_wave(model_path, 0.45, 0.27),
          "write harmonic model");

    hwa_harmonic_decay_options_default(&options);
    options.expected_hz = TEST_FUNDAMENTAL_HZ;
    memset(&result, 0x5a, sizeof(result));
    CHECK(hwa_harmonic_decay_wavs(
              reference_path, model_path, &options, &result,
              error, sizeof(error)) == 0,
          error[0] != '\0' ? error : "harmonic-decay analysis");
    CHECK(result.reference.valid_band_count == TEST_HARMONICS,
          "reference valid band count");
    CHECK(result.model.valid_band_count == TEST_HARMONICS,
          "model valid band count");
    CHECK(result.shared_valid_band_count == TEST_HARMONICS,
          "shared valid band count");
    CHECK(result.comparison_valid != 0, "comparison is valid");
    for (index = 0U; index < TEST_HARMONICS; ++index) {
        const HWAHarmonicDecayBand *reference =
            &result.reference.bands[index];
        const HWAHarmonicDecayBand *model = &result.model.bands[index];
        const HWAHarmonicDecayComparison *comparison =
            &result.comparisons[index];
        CHECK(reference->valid != 0, "reference band valid");
        CHECK(model->valid != 0, "model band valid");
        CHECK(fabs(reference->t60_seconds - expected_t60) <
                  0.02 * expected_t60,
              "reference band T60 within two percent");
        CHECK(fabs(model->t60_seconds - expected_t60) <
                  0.02 * expected_t60,
              "model band T60 within two percent");
        CHECK(comparison->valid != 0, "harmonic comparison valid");
        CHECK(fabs(comparison->t60_log_error_db) < 0.120412,
              "matching harmonic error below 0.02 octave");
    }
    hwa_harmonic_decay_result_free(&result);
    (void)HWA_TEST_UNLINK(reference_path);
    (void)HWA_TEST_UNLINK(model_path);
}

static void test_gain_polarity_stereo_lead_and_named_mismatch(void)
{
    char reference_path[256];
    char model_path[256];
    HWAHarmonicDecayOptions options;
    HWAHarmonicDecayResult result;
    char error[HWA_ERROR_SIZE] = {0};
    double reference_taus[TEST_HARMONICS];
    double model_taus[TEST_HARMONICS];
    double reference_gains[TEST_HARMONICS] = {
        0.17, 0.13, 0.11, 0.09, 0.075, 0.06
    };
    double model_gains[TEST_HARMONICS] = {
        0.045, 0.19, 0.065, 0.15, 0.052, 0.12
    };
    const double mono_scale[1] = {1.0};
    const double anti_phase_stereo[2] = {-1.0, 0.63};
    size_t index;

    for (index = 0U; index < TEST_HARMONICS; ++index) {
        reference_taus[index] = 0.45;
        model_taus[index] = 0.45;
    }
    (void)snprintf(reference_path, sizeof(reference_path),
                   "%s/hwa-harmonic-invariant-reference-%ld.wav",
                   test_temporary_root(), test_process_id());
    (void)snprintf(model_path, sizeof(model_path),
                   "%s/hwa-harmonic-invariant-model-%ld.wav",
                   test_temporary_root(), test_process_id());
    (void)HWA_TEST_UNLINK(reference_path);
    (void)HWA_TEST_UNLINK(model_path);
    CHECK(write_harmonic_variant(
              reference_path, TEST_FUNDAMENTAL_HZ,
              reference_taus, reference_gains,
              0.08, 3.0, 1U, mono_scale),
          "write invariant reference");
    CHECK(write_harmonic_variant(
              model_path, TEST_FUNDAMENTAL_HZ, model_taus, model_gains,
              0.31, 3.0, 2U, anti_phase_stereo),
          "write invariant anti-phase model");
    hwa_harmonic_decay_options_default(&options);
    options.expected_hz = TEST_FUNDAMENTAL_HZ;
    memset(&result, 0x5a, sizeof(result));
    CHECK(hwa_harmonic_decay_wavs(
              reference_path, model_path, &options, &result,
              error, sizeof(error)) == 0,
          error[0] != '\0' ? error : "invariant analysis");
    CHECK(result.shared_valid_band_count == TEST_HARMONICS,
          "invariant shared band count");
    for (index = 0U; index < TEST_HARMONICS; ++index) {
        CHECK(result.comparisons[index].valid != 0,
              "invariant comparison band valid");
        CHECK(fabs(result.comparisons[index].t60_log_error_db) < 0.120412,
              "gain polarity stereo and lead invariant");
    }
    hwa_harmonic_decay_result_free(&result);

    model_taus[2] = 0.70;
    (void)HWA_TEST_UNLINK(model_path);
    CHECK(write_harmonic_variant(
              model_path, TEST_FUNDAMENTAL_HZ, model_taus, model_gains,
              0.24, 3.0, 2U, anti_phase_stereo),
          "write one changed harmonic");
    error[0] = '\0';
    memset(&result, 0x5a, sizeof(result));
    CHECK(hwa_harmonic_decay_wavs(
              reference_path, model_path, &options, &result,
              error, sizeof(error)) == 0,
          error[0] != '\0' ? error : "changed harmonic analysis");
    CHECK(result.comparison_count >= TEST_HARMONICS,
          "ordered comparison rows retained");
    for (index = 0U; index < TEST_HARMONICS; ++index) {
        CHECK(result.comparisons[index].harmonic_number == index + 1U,
              "comparison row names its harmonic");
        if (index == 2U) {
            CHECK(result.comparisons[index].t60_log_error_db > 3.5,
                  "changed harmonic has a nonzero named error");
        } else {
            CHECK(fabs(result.comparisons[index].t60_log_error_db) < 0.15,
                  "unchanged harmonic stays close");
        }
    }
    hwa_harmonic_decay_result_free(&result);
    (void)HWA_TEST_UNLINK(reference_path);
    (void)HWA_TEST_UNLINK(model_path);
}

static void test_absent_bands_are_skipped_and_four_are_required(void)
{
    char path[256];
    HWAHarmonicDecayOptions options;
    HWAHarmonicDecayResult result;
    char error[HWA_ERROR_SIZE] = {0};
    double taus[TEST_HARMONICS];
    double gains[TEST_HARMONICS] = {0.22, 0.16, 0.11, 0.0, 0.0, 0.0};
    const double channel_scale[1] = {1.0};
    size_t index;

    for (index = 0U; index < TEST_HARMONICS; ++index) taus[index] = 0.45;
    (void)snprintf(path, sizeof(path),
                   "%s/hwa-harmonic-sparse-%ld.wav",
                   test_temporary_root(), test_process_id());
    (void)HWA_TEST_UNLINK(path);
    CHECK(write_harmonic_variant(
              path, TEST_FUNDAMENTAL_HZ, taus, gains,
              0.11, 3.0, 1U, channel_scale),
          "write sparse harmonic fixture");
    hwa_harmonic_decay_options_default(&options);
    options.expected_hz = TEST_FUNDAMENTAL_HZ;
    memset(&result, 0x5a, sizeof(result));
    CHECK(hwa_harmonic_decay_wavs(
              path, NULL, &options, &result, error, sizeof(error)) == 0,
          error[0] != '\0' ? error : "sparse harmonic analysis");
    CHECK(result.reference.valid_band_count == 3U,
          "only present harmonic bands are valid");
    CHECK(result.reference.valid == 0,
          "three harmonics do not validate the estimate");
    CHECK((result.reference.rejection_mask &
           HWA_HARMONIC_DECAY_REJECT_LOW_HARMONIC_COVERAGE) != 0U,
          "low harmonic coverage is named");
    for (index = 3U; index < result.reference.band_count; ++index) {
        CHECK(result.reference.bands[index].valid == 0,
              "absent harmonic is skipped");
    }
    CHECK((result.reference.bands[3].rejection_mask &
           HWA_HARMONIC_DECAY_REJECT_LOW_ANCHOR_SNR) != 0U,
          "absent band fails the sideband SNR gate");
    hwa_harmonic_decay_result_free(&result);
    (void)HWA_TEST_UNLINK(path);
}

static void test_short_model_and_truncated_fit_do_not_compare(void)
{
    char reference_path[256];
    char model_path[256];
    HWAHarmonicDecayOptions options;
    HWAHarmonicDecayResult result;
    char error[HWA_ERROR_SIZE] = {0};
    double taus[TEST_HARMONICS];
    double gains[TEST_HARMONICS] = {0.17, 0.14, 0.11, 0.09, 0.07, 0.055};
    const double channel_scale[1] = {1.0};
    size_t index;
    size_t truncated = 0U;

    for (index = 0U; index < TEST_HARMONICS; ++index) taus[index] = 0.18;
    (void)snprintf(reference_path, sizeof(reference_path),
                   "%s/hwa-harmonic-long-%ld.wav",
                   test_temporary_root(), test_process_id());
    (void)snprintf(model_path, sizeof(model_path),
                   "%s/hwa-harmonic-short-%ld.wav",
                   test_temporary_root(), test_process_id());
    (void)HWA_TEST_UNLINK(reference_path);
    (void)HWA_TEST_UNLINK(model_path);
    CHECK(write_harmonic_variant(
              reference_path, 110.0, taus, gains,
              0.10, 2.0, 1U, channel_scale),
          "write long harmonic reference");
    CHECK(write_harmonic_variant(
              model_path, 110.0, taus, gains,
              0.10, 0.80, 1U, channel_scale),
          "write truncated harmonic model");
    hwa_harmonic_decay_options_default(&options);
    options.expected_hz = 110.0;
    memset(&result, 0x5a, sizeof(result));
    CHECK(hwa_harmonic_decay_wavs(
              reference_path, model_path, &options, &result,
              error, sizeof(error)) == 0,
          error[0] != '\0' ? error : "truncated harmonic analysis");
    CHECK(result.reference.valid_band_count == TEST_HARMONICS,
          "long reference remains valid");
    CHECK(result.model.valid_band_count < 4U,
          "short model does not meet harmonic support");
    CHECK(result.comparison_valid == 0,
          "short model cannot publish a comparison");
    for (index = 0U; index < TEST_HARMONICS; ++index) {
        if ((result.model.bands[index].rejection_mask &
             HWA_HARMONIC_DECAY_REJECT_TRUNCATED_FIT) != 0U) {
            truncated++;
        }
    }
    CHECK(truncated != 0U,
          "one-window anti-truncation rule names the failure");
    hwa_harmonic_decay_result_free(&result);
    (void)HWA_TEST_UNLINK(reference_path);
    (void)HWA_TEST_UNLINK(model_path);
}

static void test_shallow_tail_does_not_supply_the_full_fit_range(void)
{
    char path[256];
    HWAHarmonicDecayOptions options;
    HWAHarmonicDecayResult result;
    char error[HWA_ERROR_SIZE] = {0};
    size_t index;

    (void)snprintf(path, sizeof(path),
                   "%s/hwa-harmonic-shallow-range-%ld.wav",
                   test_temporary_root(), test_process_id());
    (void)HWA_TEST_UNLINK(path);
    CHECK(write_shallow_range_harmonic_wave(path),
          "write shallow dynamic-range fixture");
    hwa_harmonic_decay_options_default(&options);
    options.expected_hz = TEST_FUNDAMENTAL_HZ;
    memset(&result, 0x5a, sizeof(result));
    CHECK(hwa_harmonic_decay_wavs(
              path, NULL, &options, &result, error, sizeof(error)) == 0,
          error[0] != '\0' ? error : "shallow dynamic-range analysis");
    CHECK(result.reference.valid == 0,
          "a shallow tail cannot validate the profile");
    CHECK(result.reference.valid_band_count == 0U,
          "a shallow tail validates no harmonic bands");
    for (index = 0U; index < TEST_HARMONICS; ++index) {
        CHECK(result.reference.bands[index].valid == 0,
              "shallow harmonic band is invalid");
        CHECK((result.reference.bands[index].rejection_mask &
               HWA_HARMONIC_DECAY_REJECT_LOW_DYNAMIC_RANGE) != 0U,
              "shallow harmonic band names low dynamic range");
    }
    hwa_harmonic_decay_result_free(&result);
    (void)HWA_TEST_UNLINK(path);
}

static void test_short_body_filter_and_same_tau_modal_beating_stay_close(void)
{
    char reference_path[256];
    char model_path[256];
    HWAHarmonicDecayOptions options;
    HWAHarmonicDecayResult result;
    char error[HWA_ERROR_SIZE] = {0};
    size_t index;

    (void)snprintf(reference_path, sizeof(reference_path),
                   "%s/hwa-harmonic-color-reference-%ld.wav",
                   test_temporary_root(), test_process_id());
    (void)snprintf(model_path, sizeof(model_path),
                   "%s/hwa-harmonic-color-model-%ld.wav",
                   test_temporary_root(), test_process_id());
    (void)HWA_TEST_UNLINK(reference_path);
    (void)HWA_TEST_UNLINK(model_path);
    CHECK(write_colored_harmonic_wave(reference_path, 0, 0, 0.08),
          "write body-filter reference");
    CHECK(write_colored_harmonic_wave(model_path, 0, 1, 0.23),
          "write short body-filter model");
    hwa_harmonic_decay_options_default(&options);
    options.expected_hz = TEST_FUNDAMENTAL_HZ;
    memset(&result, 0x5a, sizeof(result));
    CHECK(hwa_harmonic_decay_wavs(
              reference_path, model_path, &options, &result,
              error, sizeof(error)) == 0,
          error[0] != '\0' ? error : "body-filter analysis");
    CHECK(result.shared_valid_band_count == TEST_HARMONICS,
          "short body filter keeps all bands");
    for (index = 0U; index < TEST_HARMONICS; ++index) {
        CHECK(fabs(result.comparisons[index].t60_log_error_db) < 0.30103,
              "short body filter changes T60 by under 0.05 octave");
    }
    hwa_harmonic_decay_result_free(&result);

    (void)HWA_TEST_UNLINK(model_path);
    CHECK(write_colored_harmonic_wave(model_path, 1, 0, 0.19),
          "write same-tau modal model");
    error[0] = '\0';
    memset(&result, 0x5a, sizeof(result));
    CHECK(hwa_harmonic_decay_wavs(
              reference_path, model_path, &options, &result,
              error, sizeof(error)) == 0,
          error[0] != '\0' ? error : "modal beating analysis");
    CHECK(result.shared_valid_band_count >= 4U,
          "modal beating keeps enough shared bands");
    CHECK(result.t60_log_rmse_db < 0.30103,
          "same-tau modal beating stays within 0.05 octave");
    hwa_harmonic_decay_result_free(&result);
    (void)HWA_TEST_UNLINK(reference_path);
    (void)HWA_TEST_UNLINK(model_path);
}

static void test_resource_caps_fail_without_publishing_a_result(void)
{
    char path[256];
    HWAHarmonicDecayOptions options;
    HWAHarmonicDecayResult result;
    char error[HWA_ERROR_SIZE] = {0};
    size_t index;
    struct LimitCase {
        uint64_t bytes;
        uint64_t frames;
        uint64_t work;
        uint64_t evaluations;
        const char *message;
    } cases[] = {
        {1024U, UINT64_C(2000000000), UINT64_C(268435456),
         UINT64_C(500000000), "byte"},
        {UINT64_C(17179869184), 100U, UINT64_C(268435456),
         UINT64_C(500000000), "frame"},
        {UINT64_C(17179869184), UINT64_C(2000000000), 1024U,
         UINT64_C(500000000), "work"},
        {UINT64_C(17179869184), UINT64_C(2000000000),
         UINT64_C(268435456), 100U, "evaluation"},
    };

    (void)snprintf(path, sizeof(path),
                   "%s/hwa-harmonic-limits-%ld.wav",
                   test_temporary_root(), test_process_id());
    (void)HWA_TEST_UNLINK(path);
    CHECK(write_harmonic_wave(path, 0.45, 0.10),
          "write resource-limit fixture");
    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        hwa_harmonic_decay_options_default(&options);
        options.expected_hz = TEST_FUNDAMENTAL_HZ;
        options.max_input_bytes = cases[index].bytes;
        options.max_input_frames = cases[index].frames;
        options.max_work_bytes = cases[index].work;
        options.max_evaluations = cases[index].evaluations;
        memset(&result, 0x5a, sizeof(result));
        error[0] = '\0';
        CHECK(hwa_harmonic_decay_wavs(
                  path, NULL, &options, &result,
                  error, sizeof(error)) != 0,
              "resource cap rejects analysis");
        CHECK(strstr(error, cases[index].message) != NULL,
              "resource cap gives the named error");
        CHECK(result.reference.path == NULL &&
                  result.reference.bands == NULL &&
                  result.comparisons == NULL &&
                  result.evaluation_count == 0U,
              "failed resource call publishes no partial result");
        hwa_harmonic_decay_result_free(&result);
    }
    (void)HWA_TEST_UNLINK(path);
}

static void test_pcm24_extensible_uses_mean_channel_power(void)
{
    char reference_path[256];
    char model_path[256];
    HWAHarmonicDecayOptions options;
    HWAHarmonicDecayResult result;
    char error[HWA_ERROR_SIZE] = {0};
    size_t index;

    (void)snprintf(reference_path, sizeof(reference_path),
                   "%s/hwa-harmonic-pcm16-%ld.wav",
                   test_temporary_root(), test_process_id());
    (void)snprintf(model_path, sizeof(model_path),
                   "%s/hwa-harmonic-extensible24-%ld.wav",
                   test_temporary_root(), test_process_id());
    (void)HWA_TEST_UNLINK(reference_path);
    (void)HWA_TEST_UNLINK(model_path);
    CHECK(write_harmonic_wave(reference_path, 0.45, 0.09),
          "write PCM16 reference");
    CHECK(write_extensible_pcm24_harmonic_wave(model_path),
          "write extensible PCM24 model");
    hwa_harmonic_decay_options_default(&options);
    options.expected_hz = TEST_FUNDAMENTAL_HZ;
    memset(&result, 0x5a, sizeof(result));
    CHECK(hwa_harmonic_decay_wavs(
              reference_path, model_path, &options, &result,
              error, sizeof(error)) == 0,
          error[0] != '\0' ? error : "PCM24 extensible analysis");
    CHECK(result.model.format.bits_per_sample == 24U,
          "PCM24 format retained");
    CHECK(result.model.format.channels == 2U,
          "stereo extensible format retained");
    CHECK(result.shared_valid_band_count == TEST_HARMONICS,
          "PCM24 extensible shared bands");
    for (index = 0U; index < TEST_HARMONICS; ++index) {
        CHECK(fabs(result.comparisons[index].t60_log_error_db) < 0.120412,
              "PCM24 extensible T60 matches PCM16");
    }
    hwa_harmonic_decay_result_free(&result);
    (void)HWA_TEST_UNLINK(reference_path);
    (void)HWA_TEST_UNLINK(model_path);
}

static void test_one_event_gate_rejects_silence_and_second_pluck(void)
{
    char silence_path[256];
    char double_path[256];
    HWAHarmonicDecayOptions options;
    HWAHarmonicDecayResult result;
    char error[HWA_ERROR_SIZE] = {0};
    double taus[TEST_HARMONICS];
    double gains[TEST_HARMONICS] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    const double channel_scale[1] = {1.0};
    size_t index;

    for (index = 0U; index < TEST_HARMONICS; ++index) taus[index] = 0.45;
    (void)snprintf(silence_path, sizeof(silence_path),
                   "%s/hwa-harmonic-silence-%ld.wav",
                   test_temporary_root(), test_process_id());
    (void)snprintf(double_path, sizeof(double_path),
                   "%s/hwa-harmonic-two-plucks-%ld.wav",
                   test_temporary_root(), test_process_id());
    (void)HWA_TEST_UNLINK(silence_path);
    (void)HWA_TEST_UNLINK(double_path);
    CHECK(write_harmonic_variant(
              silence_path, TEST_FUNDAMENTAL_HZ, taus, gains,
              0.10, 2.0, 1U, channel_scale),
          "write no-onset fixture");
    CHECK(write_two_pluck_wave(double_path), "write second-pluck fixture");
    hwa_harmonic_decay_options_default(&options);
    options.expected_hz = TEST_FUNDAMENTAL_HZ;

    memset(&result, 0x5a, sizeof(result));
    CHECK(hwa_harmonic_decay_wavs(
              silence_path, NULL, &options, &result,
              error, sizeof(error)) == 0,
          error[0] != '\0' ? error : "no-onset analysis");
    CHECK((result.reference.rejection_mask &
           HWA_HARMONIC_DECAY_REJECT_NO_ONSET) != 0U,
          "no onset is named");
    CHECK(result.reference.valid == 0, "no-onset profile is invalid");
    hwa_harmonic_decay_result_free(&result);

    error[0] = '\0';
    memset(&result, 0x5a, sizeof(result));
    CHECK(hwa_harmonic_decay_wavs(
              double_path, NULL, &options, &result,
              error, sizeof(error)) == 0,
          error[0] != '\0' ? error : "second-pluck analysis");
    CHECK((result.reference.rejection_mask &
           HWA_HARMONIC_DECAY_REJECT_LATE_PULSE) != 0U,
          "second pluck is named");
    CHECK(result.reference.valid == 0, "second-pluck profile is invalid");
    hwa_harmonic_decay_result_free(&result);
    (void)HWA_TEST_UNLINK(silence_path);
    (void)HWA_TEST_UNLINK(double_path);
}

static void test_one_event_gate_rejects_a_close_slow_repluck(void)
{
    char path[256];
    HWAHarmonicDecayOptions options;
    HWAHarmonicDecayResult result;
    char error[HWA_ERROR_SIZE] = {0};

    (void)snprintf(path, sizeof(path),
                   "%s/hwa-harmonic-close-repluck-%ld.wav",
                   test_temporary_root(), test_process_id());
    (void)HWA_TEST_UNLINK(path);
    CHECK(write_close_slow_repluck_wave(path),
          "write close slow-repluck fixture");
    hwa_harmonic_decay_options_default(&options);
    options.expected_hz = TEST_FUNDAMENTAL_HZ;
    memset(&result, 0x5a, sizeof(result));
    CHECK(hwa_harmonic_decay_wavs(
              path, NULL, &options, &result, error, sizeof(error)) == 0,
          error[0] != '\0' ? error : "close slow-repluck analysis");
    CHECK((result.reference.rejection_mask &
           HWA_HARMONIC_DECAY_REJECT_LATE_PULSE) != 0U,
          "close slow repluck is named as a late pulse");
    CHECK(result.reference.valid == 0,
          "close slow repluck cannot validate the profile");
    CHECK(result.reference.valid_band_count == 0U,
          "close slow repluck is rejected before band fitting");
    hwa_harmonic_decay_result_free(&result);
    (void)HWA_TEST_UNLINK(path);
}

static void test_invalid_options_initialize_the_result(void)
{
    HWAHarmonicDecayOptions options;
    HWAHarmonicDecayResult result;
    char error[HWA_ERROR_SIZE] = {0};
    const double invalid[] = {NAN, 19.99, 5000.01};
    size_t index;

    for (index = 0U; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        hwa_harmonic_decay_options_default(&options);
        options.expected_hz = invalid[index];
        memset(&result, 0x5a, sizeof(result));
        error[0] = '\0';
        CHECK(hwa_harmonic_decay_wavs(
                  "unused.wav", NULL, &options, &result,
                  error, sizeof(error)) != 0,
              "invalid expected frequency is rejected");
        CHECK(strstr(error, "expected frequency") != NULL,
              "invalid expected frequency gives a clear error");
        CHECK(result.reference.path == NULL &&
                  result.reference.bands == NULL &&
                  result.comparisons == NULL,
              "invalid options publish no result");
        hwa_harmonic_decay_result_free(&result);
    }
}

int main(void)
{
    test_known_exponential_t60_per_band();
    test_gain_polarity_stereo_lead_and_named_mismatch();
    test_absent_bands_are_skipped_and_four_are_required();
    test_short_model_and_truncated_fit_do_not_compare();
    test_shallow_tail_does_not_supply_the_full_fit_range();
    test_short_body_filter_and_same_tau_modal_beating_stay_close();
    test_resource_caps_fail_without_publishing_a_result();
    test_pcm24_extensible_uses_mean_channel_power();
    test_one_event_gate_rejects_silence_and_second_pluck();
    test_one_event_gate_rejects_a_close_slow_repluck();
    test_invalid_options_initialize_the_result();
    if (failures != 0) return 1;
    (void)puts("PASS: checked harmonic-decay analysis");
    return 0;
}

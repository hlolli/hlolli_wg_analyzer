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
#define TEST_EXPECTED_HZ 41.20344461410875

static int failures;

#define CHECK(condition, message)                                            \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "FAIL: %s\n", message);                  \
            failures++;                                                      \
        }                                                                    \
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

static int write_decay_wave(const char *path, double tau_seconds)
{
    const uint32_t frames = TEST_RATE * 3U;
    const uint32_t lead = TEST_RATE / 10U;
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
        double sample = 0.0;
        int16_t encoded;
        if (frame >= lead) {
            double time = (double)(frame - lead) / (double)TEST_RATE;
            double phase = 6.2831853071795864769 * TEST_EXPECTED_HZ * time;
            sample = 0.82 * exp(-time / tau_seconds) *
                     (0.76 * sin(phase) + 0.24 * sin(2.0 * phase + 0.2));
        }
        if (sample > 1.0) sample = 1.0;
        if (sample < -1.0) sample = -1.0;
        encoded = (int16_t)lrint(sample * 32767.0);
        if (!write_u16(stream, (uint16_t)encoded)) {
            (void)fclose(stream);
            return 0;
        }
    }
    return fclose(stream) == 0;
}

static void test_low_e_pitch_and_decay(void)
{
    char path[256];
    HWAIsolatedNoteOptions options;
    HWAIsolatedNoteResult result;
    char error[HWA_ERROR_SIZE] = {0};
    double wanted_slope = -20.0 / (log(10.0) * 0.55);

    (void)snprintf(path, sizeof(path), "%s/hwa-isolated-note-%ld.wav",
                   test_temporary_root(), test_process_id());
    (void)HWA_TEST_UNLINK(path);
    CHECK(write_decay_wave(path, 0.55), "write low E fixture");
    hwa_isolated_note_options_default(&options);
    options.expected_hz = TEST_EXPECTED_HZ;
    options.metric_mask = HWA_ISOLATED_NOTE_PITCH |
                          HWA_ISOLATED_NOTE_PASSIVE_DECAY;
    memset(&result, 0x5a, sizeof(result));
    CHECK(hwa_analyze_isolated_note_wav(path, &options, &result,
                                       error, sizeof(error)) == 0,
          error[0] != '\0' ? error : "isolated-note analysis");
    CHECK(result.requested_mask == options.metric_mask,
          "requested metric mask");
    CHECK(result.valid_mask == options.metric_mask, "valid metric mask");
    CHECK(fabs(result.expected_hz - TEST_EXPECTED_HZ) < 1.0e-12,
          "expected frequency remains exact");
    CHECK(fabs(result.pitch.cents) < 2.0, "low E pitch cents");
    CHECK(result.pitch.confidence > 0.80, "low E pitch confidence");
    CHECK(result.pitch.coverage > 0.40, "low E pitch coverage");
    CHECK(fabs(result.decay.slope_db_per_second - wanted_slope) < 1.5,
          "passive decay slope");
    CHECK(fabs(result.decay.t60_seconds +
                   60.0 / wanted_slope) < 0.40,
          "passive decay T60");
    CHECK(result.decay.support_seconds > 1.0,
          "passive decay support");
    CHECK(result.decay.dynamic_range_db > 15.0,
          "passive decay dynamic range");
    CHECK(result.decay.start_sample < result.decay.end_sample &&
              result.decay.end_sample <= TEST_RATE * 3U,
          "passive decay sample bounds");
    hwa_isolated_note_result_free(&result);
    hwa_isolated_note_options_default(&options);
    options.expected_hz = TEST_EXPECTED_HZ;
    options.metric_mask = HWA_ISOLATED_NOTE_PITCH;
    options.max_work_bytes = 100U;
    memset(&result, 0x5a, sizeof(result));
    error[0] = '\0';
    CHECK(hwa_analyze_isolated_note_wav(path, &options, &result,
                                       error, sizeof(error)) != 0,
          "small work cap rejects analysis");
    CHECK(result.path == NULL && result.requested_mask == 0U &&
              result.valid_mask == 0U && result.rejection_mask == 0U,
          "failed call initializes the public result");
    hwa_isolated_note_result_free(&result);
    (void)HWA_TEST_UNLINK(path);
}

int main(void)
{
    test_low_e_pitch_and_decay();
    if (failures != 0) return 1;
    (void)puts("PASS: checked isolated-note analysis");
    return 0;
}

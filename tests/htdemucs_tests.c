#include "htdemucs.h"
#include "inference_clock.h"
#include "internal.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_WORK_BYTES (UINT64_C(64) * UINT64_C(1024) * UINT64_C(1024))

static int failures;

#define CHECK(condition, ...)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            (void)fprintf(stderr, __VA_ARGS__);                              \
            (void)fputc('\n', stderr);                                       \
            failures++;                                                      \
        }                                                                    \
    } while (0)

typedef struct TestWave {
    unsigned char *bytes;
    size_t size;
    HWAFormat format;
    HWAByteSource source;
} TestWave;

typedef enum TestModelMode {
    TEST_MODEL_MONO = 1,
    TEST_MODEL_OVERLAP,
    TEST_MODEL_NONFINITE,
    TEST_MODEL_DELAY
} TestModelMode;

typedef struct TestModel {
    TestModelMode mode;
    size_t calls;
    size_t destroys;
} TestModel;

static void test_le16(unsigned char bytes[2], uint16_t value)
{
    bytes[0] = (unsigned char)(value & UINT16_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT16_C(0xff));
}

static void test_le32(unsigned char bytes[4], uint32_t value)
{
    bytes[0] = (unsigned char)(value & UINT32_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT32_C(0xff));
    bytes[2] = (unsigned char)((value >> 16U) & UINT32_C(0xff));
    bytes[3] = (unsigned char)((value >> 24U) & UINT32_C(0xff));
}

static uint32_t test_read_le32(const unsigned char bytes[4])
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static int test_read_at(void *context,
                        uint64_t offset,
                        unsigned char *destination,
                        size_t size)
{
    const TestWave *wave = (const TestWave *)context;
    if (wave == NULL || destination == NULL ||
        offset > (uint64_t)wave->size ||
        (uint64_t)size > (uint64_t)wave->size - offset)
        return -1;
    memcpy(destination, wave->bytes + (size_t)offset, size);
    return 0;
}

static int test_wave_create(TestWave *wave,
                            uint64_t frames,
                            uint16_t channels)
{
    uint64_t data_bytes = frames * (uint64_t)channels * UINT64_C(2);
    uint64_t file_bytes = data_bytes + UINT64_C(44);
    uint16_t block_align = (uint16_t)(channels * 2U);
    uint64_t frame;
    if (wave == NULL || channels == 0U ||
        data_bytes > UINT32_MAX - UINT32_C(36) ||
        file_bytes > (uint64_t)SIZE_MAX)
        return -1;
    memset(wave, 0, sizeof(*wave));
    wave->bytes = (unsigned char *)calloc((size_t)file_bytes, 1U);
    if (wave->bytes == NULL) return -1;
    wave->size = (size_t)file_bytes;
    memcpy(wave->bytes, "RIFF", 4U);
    test_le32(wave->bytes + 4U, (uint32_t)data_bytes + UINT32_C(36));
    memcpy(wave->bytes + 8U, "WAVEfmt ", 8U);
    test_le32(wave->bytes + 16U, UINT32_C(16));
    test_le16(wave->bytes + 20U, UINT16_C(1));
    test_le16(wave->bytes + 22U, channels);
    test_le32(wave->bytes + 24U, HWA_HTDEMUCS_SAMPLE_RATE);
    test_le32(wave->bytes + 28U,
              HWA_HTDEMUCS_SAMPLE_RATE * (uint32_t)block_align);
    test_le16(wave->bytes + 32U, block_align);
    test_le16(wave->bytes + 34U, UINT16_C(16));
    memcpy(wave->bytes + 36U, "data", 4U);
    test_le32(wave->bytes + 40U, (uint32_t)data_bytes);
    for (frame = 0U; frame < frames; ++frame) {
        size_t offset = 44U + (size_t)frame * block_align;
        test_le16(wave->bytes + offset, UINT16_C(8192));
        if (channels == 2U)
            test_le16(wave->bytes + offset + 2U, UINT16_C(57344));
    }
    wave->format.container = HWA_CONTAINER_RIFF;
    wave->format.encoding = HWA_ENCODING_PCM;
    wave->format.channels = channels;
    wave->format.sample_rate_hz = HWA_HTDEMUCS_SAMPLE_RATE;
    wave->format.bits_per_sample = 16U;
    wave->format.valid_bits_per_sample = 16U;
    wave->format.block_align = block_align;
    wave->format.channel_mask = 0U;
    wave->format.frames = frames;
    wave->format.data_bytes = data_bytes;
    wave->format.duration_seconds =
        (double)frames / (double)HWA_HTDEMUCS_SAMPLE_RATE;
    wave->source.context = wave;
    wave->source.name = "test.wav";
    wave->source.size = file_bytes;
    wave->source.read_at = test_read_at;
    return 0;
}

static int test_float_wave_create(TestWave *wave, float sample)
{
    uint32_t bits;
    const size_t file_bytes = 48U;
    if (wave == NULL) return -1;
    memset(wave, 0, sizeof(*wave));
    wave->bytes = (unsigned char *)calloc(file_bytes, 1U);
    if (wave->bytes == NULL) return -1;
    wave->size = file_bytes;
    memcpy(wave->bytes, "RIFF", 4U);
    test_le32(wave->bytes + 4U, UINT32_C(40));
    memcpy(wave->bytes + 8U, "WAVEfmt ", 8U);
    test_le32(wave->bytes + 16U, UINT32_C(16));
    test_le16(wave->bytes + 20U, UINT16_C(3));
    test_le16(wave->bytes + 22U, UINT16_C(1));
    test_le32(wave->bytes + 24U, HWA_HTDEMUCS_SAMPLE_RATE);
    test_le32(wave->bytes + 28U, HWA_HTDEMUCS_SAMPLE_RATE * UINT32_C(4));
    test_le16(wave->bytes + 32U, UINT16_C(4));
    test_le16(wave->bytes + 34U, UINT16_C(32));
    memcpy(wave->bytes + 36U, "data", 4U);
    test_le32(wave->bytes + 40U, UINT32_C(4));
    memcpy(&bits, &sample, sizeof(bits));
    test_le32(wave->bytes + 44U, bits);
    wave->format.container = HWA_CONTAINER_RIFF;
    wave->format.encoding = HWA_ENCODING_IEEE_FLOAT;
    wave->format.channels = 1U;
    wave->format.sample_rate_hz = HWA_HTDEMUCS_SAMPLE_RATE;
    wave->format.bits_per_sample = 32U;
    wave->format.valid_bits_per_sample = 32U;
    wave->format.block_align = 4U;
    wave->format.channel_mask = 0U;
    wave->format.frames = UINT64_C(1);
    wave->format.data_bytes = UINT64_C(4);
    wave->format.duration_seconds =
        1.0 / (double)HWA_HTDEMUCS_SAMPLE_RATE;
    wave->source.context = wave;
    wave->source.name = "float.wav";
    wave->source.size = (uint64_t)file_bytes;
    wave->source.read_at = test_read_at;
    return 0;
}

static void test_wave_destroy(TestWave *wave)
{
    if (wave == NULL) return;
    free(wave->bytes);
    memset(wave, 0, sizeof(*wave));
}

static float test_model_value(size_t call,
                              size_t stem,
                              size_t channel)
{
    return (float)((call + 1U) * 10U + stem * 2U + channel);
}

static int test_close(float left, float right)
{
    return fabs((double)left - (double)right) < 1.0e-5;
}

static int test_input_is(const float *input,
                         size_t channel,
                         size_t begin,
                         size_t end,
                         float expected)
{
    size_t frame;
    const float *plane = input + channel * HWA_HTDEMUCS_INPUT_SAMPLES;
    for (frame = begin; frame < end; ++frame) {
        if (!test_close(plane[frame], expected)) return 0;
    }
    return 1;
}

static int test_model_run(void *context,
                          const float input[HWA_HTDEMUCS_INPUT_CELLS],
                          float output[HWA_HTDEMUCS_OUTPUT_CELLS],
                          char *error,
                          size_t error_size)
{
    TestModel *model = (TestModel *)context;
    size_t plane;
    size_t frame;
    int input_valid = 1;
    if (model == NULL || input == NULL || output == NULL) return -1;
    if (model->mode == TEST_MODEL_MONO) {
        input_valid = model->calls == 0U &&
                      test_input_is(input, 0U, 0U, 5U, 0.25f) &&
                      test_input_is(input, 1U, 0U, 5U, 0.25f) &&
                      test_input_is(input, 0U, 5U,
                                    HWA_HTDEMUCS_INPUT_SAMPLES, 0.0f) &&
                      test_input_is(input, 1U, 5U,
                                    HWA_HTDEMUCS_INPUT_SAMPLES, 0.0f);
    } else if (model->mode == TEST_MODEL_OVERLAP) {
        if (model->calls == 0U) {
            input_valid =
                test_input_is(input, 0U, 0U,
                              HWA_HTDEMUCS_INPUT_SAMPLES, 0.25f) &&
                test_input_is(input, 1U, 0U,
                              HWA_HTDEMUCS_INPUT_SAMPLES, -0.25f);
        } else if (model->calls == 1U) {
            input_valid =
                test_input_is(input, 0U, 0U,
                              HWA_HTDEMUCS_OVERLAP_SAMPLES, 0.25f) &&
                test_input_is(input, 1U, 0U,
                              HWA_HTDEMUCS_OVERLAP_SAMPLES, -0.25f) &&
                test_input_is(input, 0U, HWA_HTDEMUCS_OVERLAP_SAMPLES,
                              HWA_HTDEMUCS_INPUT_SAMPLES, 0.0f) &&
                test_input_is(input, 1U, HWA_HTDEMUCS_OVERLAP_SAMPLES,
                              HWA_HTDEMUCS_INPUT_SAMPLES, 0.0f);
        } else {
            input_valid = 0;
        }
    }
    if (!input_valid) {
        if (error != NULL && error_size != 0U)
            (void)snprintf(error, error_size,
                           "fake model received the wrong window");
        return -1;
    }
    for (plane = 0U;
         plane < HWA_HTDEMUCS_STEM_COUNT * HWA_HTDEMUCS_CHANNELS;
         ++plane) {
        size_t stem = plane / HWA_HTDEMUCS_CHANNELS;
        size_t channel = plane % HWA_HTDEMUCS_CHANNELS;
        float value = test_model_value(model->calls, stem, channel);
        for (frame = 0U; frame < HWA_HTDEMUCS_INPUT_SAMPLES; ++frame)
            output[plane * HWA_HTDEMUCS_INPUT_SAMPLES + frame] = value;
    }
    if (model->mode == TEST_MODEL_NONFINITE)
        output[HWA_HTDEMUCS_OUTPUT_CELLS - 1U] = NAN;
    model->calls++;
    if (model->mode == TEST_MODEL_DELAY) {
        uint64_t started = 0U;
        char ignored[HWA_ERROR_SIZE] = {0};
        if (hwa_inference_deadline_start(
                &started, error, error_size) != 0)
            return -1;
        while (hwa_inference_deadline_check(
                   started, UINT64_C(25), ignored, sizeof(ignored)) == 0) {
        }
    }
    return 0;
}

static void test_model_destroy(void *context)
{
    TestModel *model = (TestModel *)context;
    if (model != NULL) model->destroys++;
}

static int test_runner_init(TestModel *model,
                            uint64_t max_work_bytes,
                            HWAInstrumentStemRunner *runner,
                            char *error,
                            size_t error_size)
{
    HWAHTDemucsModelRunner low_level;
    memset(&low_level, 0, sizeof(low_level));
    low_level.context = model;
    low_level.runtime_name = "fake-onnx-runtime";
    low_level.runtime_version = "1.0";
    low_level.backend = "cpu";
    low_level.fallback = "";
    low_level.run_window = test_model_run;
    low_level.destroy = test_model_destroy;
    return hwa_htdemucs_instrument_runner_init(
        runner, max_work_bytes, &low_level, error, error_size);
}

static float test_output_sample(const HWAByteSource *source,
                                uint64_t frame,
                                size_t channel)
{
    unsigned char bytes[4] = {0};
    uint32_t bits;
    float value = NAN;
    uint64_t offset = UINT64_C(44) +
                      (frame * HWA_HTDEMUCS_CHANNELS + (uint64_t)channel) *
                          UINT64_C(4);
    if (source->read_at(source->context, offset, bytes, sizeof(bytes)) != 0)
        return NAN;
    bits = test_read_le32(bytes);
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void test_mono_order_and_shape(void)
{
    static const char *const ids[HWA_HTDEMUCS_STEM_COUNT] = {
        "drums", "bass", "other", "vocals", "guitar", "piano"
    };
    TestWave wave;
    TestModel model;
    HWAInstrumentStemRunner runner;
    HWAInstrumentStemResult *stems = NULL;
    size_t stem_count = 0U;
    char error[HWA_ERROR_SIZE] = {0};
    size_t index;
    memset(&model, 0, sizeof(model));
    memset(&runner, 0, sizeof(runner));
    model.mode = TEST_MODEL_MONO;
    CHECK(test_wave_create(&wave, UINT64_C(5), UINT16_C(1)) == 0,
          "cannot create mono fixture");
    if (wave.bytes == NULL) return;
    CHECK(test_runner_init(
              &model, TEST_WORK_BYTES, &runner,
              error, sizeof(error)) == 0,
          "cannot initialize mono runner: %s", error);
    if (runner.context == NULL) {
        test_wave_destroy(&wave);
        return;
    }
    CHECK(strcmp(runner.runtime_name, "fake-onnx-runtime") == 0 &&
              strcmp(runner.runtime_version, "1.0") == 0 &&
              strcmp(runner.backend, "cpu") == 0 &&
              strcmp(runner.fallback, "") == 0,
          "runner metadata is wrong");
    CHECK(runner.run(
              runner.context, &wave.source, &wave.format,
              UINT64_C(123), UINT64_C(10000),
              &stems, &stem_count, error, sizeof(error)) == 0,
          "mono separation failed: %s", error);
    CHECK(model.calls == 1U, "mono separation used %zu model calls",
          model.calls);
    CHECK(stem_count == HWA_HTDEMUCS_STEM_COUNT && stems != NULL,
          "mono separation returned the wrong stem count");
    if (stems != NULL) {
        for (index = 0U; index < stem_count; ++index) {
            HWAWavReader reader;
            memset(&reader, 0, sizeof(reader));
            CHECK(strcmp(stems[index].stem_id, ids[index]) == 0 &&
                      strcmp(stems[index].instrument, ids[index]) == 0,
                  "stem %zu has the wrong label", index);
            CHECK(stems[index].score == 0.0 &&
                      stems[index].score_valid == 0,
                  "stem %zu has an unexpected score", index);
            CHECK(stems[index].wave.size == UINT64_C(84),
                  "stem %zu has the wrong byte size", index);
            CHECK(hwa_wav_reader_open_source(
                      &reader, &stems[index].wave,
                      stems[index].wave.size,
                      error, sizeof(error)) == 0,
                  "stem %zu is not a valid WAVE: %s", index, error);
            if (reader.source.read_at != NULL) {
                CHECK(reader.format.encoding == HWA_ENCODING_IEEE_FLOAT &&
                          reader.format.channels == 2U &&
                          reader.format.sample_rate_hz ==
                              HWA_HTDEMUCS_SAMPLE_RATE &&
                          reader.format.bits_per_sample == 32U &&
                          reader.format.frames == UINT64_C(5),
                      "stem %zu has the wrong WAVE format", index);
                hwa_wav_reader_close(&reader);
            }
            CHECK(test_close(
                      test_output_sample(&stems[index].wave, UINT64_C(0), 0U),
                      0.0f) &&
                      test_close(
                          test_output_sample(
                              &stems[index].wave, UINT64_C(0), 1U),
                          0.0f),
                  "stem %zu did not apply the zero head weight", index);
            CHECK(test_close(
                      test_output_sample(&stems[index].wave, UINT64_C(1), 0U),
                      test_model_value(0U, index, 0U)) &&
                      test_close(
                          test_output_sample(
                              &stems[index].wave, UINT64_C(1), 1U),
                          test_model_value(0U, index, 1U)),
                  "stem %zu has the wrong samples", index);
        }
        runner.results_destroy(runner.context, stems, stem_count);
    }
    runner.destroy(runner.context);
    CHECK(model.destroys == 1U, "model context was not destroyed once");
    test_wave_destroy(&wave);
}

static void test_overlap_add(void)
{
    TestWave wave;
    TestModel model;
    HWAInstrumentStemRunner runner;
    HWAInstrumentStemResult *stems = NULL;
    size_t stem_count = 0U;
    char error[HWA_ERROR_SIZE] = {0};
    size_t middle = HWA_HTDEMUCS_OVERLAP_SAMPLES / 2U;
    float right_weight =
        (float)((double)middle /
                (double)(HWA_HTDEMUCS_OVERLAP_SAMPLES - 1U));
    float left_weight =
        (float)((double)(HWA_HTDEMUCS_OVERLAP_SAMPLES - 1U - middle) /
                (double)(HWA_HTDEMUCS_OVERLAP_SAMPLES - 1U));
    float expected =
        (test_model_value(0U, 0U, 0U) * left_weight +
         test_model_value(1U, 0U, 0U) * right_weight) /
        (left_weight + right_weight);
    memset(&model, 0, sizeof(model));
    memset(&runner, 0, sizeof(runner));
    model.mode = TEST_MODEL_OVERLAP;
    CHECK(test_wave_create(
              &wave, HWA_HTDEMUCS_INPUT_SAMPLES, UINT16_C(2)) == 0,
          "cannot create overlap fixture");
    if (wave.bytes == NULL) return;
    CHECK(test_runner_init(
              &model, TEST_WORK_BYTES, &runner,
              error, sizeof(error)) == 0,
          "cannot initialize overlap runner: %s", error);
    if (runner.context == NULL) {
        test_wave_destroy(&wave);
        return;
    }
    CHECK(runner.run(
              runner.context, &wave.source, &wave.format,
              UINT64_C(0), UINT64_C(10000),
              &stems, &stem_count, error, sizeof(error)) == 0,
          "overlap separation failed: %s", error);
    CHECK(model.calls == 2U, "overlap separation used %zu calls",
          model.calls);
    if (stems != NULL && stem_count == HWA_HTDEMUCS_STEM_COUNT) {
        CHECK(test_close(
                  test_output_sample(
                      &stems[0].wave,
                      HWA_HTDEMUCS_STRIDE_SAMPLES, 0U),
                  test_model_value(0U, 0U, 0U)),
              "overlap start has the wrong value");
        CHECK(test_close(
                  test_output_sample(
                      &stems[0].wave,
                      HWA_HTDEMUCS_STRIDE_SAMPLES + (uint64_t)middle, 0U),
                  expected),
              "overlap middle has the wrong value");
        CHECK(test_close(
                  test_output_sample(
                      &stems[0].wave,
                      HWA_HTDEMUCS_INPUT_SAMPLES - UINT64_C(1), 0U),
                  test_model_value(1U, 0U, 0U)),
              "overlap end has the wrong value");
        CHECK(test_close(
                  test_output_sample(
                      &stems[5].wave,
                      HWA_HTDEMUCS_INPUT_SAMPLES - UINT64_C(1), 1U),
                  test_model_value(1U, 5U, 1U)),
              "last stem or channel has the wrong value");
        runner.results_destroy(runner.context, stems, stem_count);
    }
    runner.destroy(runner.context);
    CHECK(model.destroys == 1U, "overlap model was not destroyed once");
    test_wave_destroy(&wave);
}

static void test_work_limit_and_bad_format(void)
{
    TestWave wave;
    TestModel model;
    HWAInstrumentStemRunner runner;
    HWAInstrumentStemResult *stems = NULL;
    size_t stem_count = 0U;
    char error[HWA_ERROR_SIZE] = {0};
    memset(&model, 0, sizeof(model));
    memset(&runner, 0, sizeof(runner));
    model.mode = TEST_MODEL_MONO;
    CHECK(test_wave_create(&wave, UINT64_C(5), UINT16_C(1)) == 0,
          "cannot create limit fixture");
    if (wave.bytes == NULL) return;
    CHECK(test_runner_init(
              &model, UINT64_C(4096), &runner,
              error, sizeof(error)) == 0,
          "small runner setup failed too early: %s", error);
    if (runner.context != NULL) {
        CHECK(runner.run(
                  runner.context, &wave.source, &wave.format,
                  UINT64_C(0), UINT64_C(10000),
                  &stems, &stem_count, error, sizeof(error)) != 0 &&
                  strstr(error, "work limit") != NULL &&
                  stems == NULL && stem_count == 0U && model.calls == 0U,
              "adapter heap limit was not enforced: %s", error);
        runner.destroy(runner.context);
    }
    memset(&runner, 0, sizeof(runner));
    error[0] = '\0';
    CHECK(test_runner_init(
              &model, TEST_WORK_BYTES, &runner,
              error, sizeof(error)) == 0,
          "bad-format runner setup failed: %s", error);
    if (runner.context != NULL) {
        HWAFormat wrong = wave.format;
        HWAFormat too_long = wave.format;
        wrong.sample_rate_hz = UINT32_C(48000);
        CHECK(runner.run(
                  runner.context, &wave.source, &wrong,
                  UINT64_C(0), UINT64_C(10000),
                  &stems, &stem_count, error, sizeof(error)) != 0 &&
                  stems == NULL && stem_count == 0U && model.calls == 0U,
              "wrong sample rate was accepted");
        too_long.frames = UINT64_C(536870908);
        CHECK(runner.run(
                  runner.context, &wave.source, &too_long,
                  UINT64_C(0), UINT64_C(10000),
                  &stems, &stem_count, error, sizeof(error)) != 0 &&
                  stems == NULL && stem_count == 0U && model.calls == 0U &&
                  strstr(error, "RIFF stem frame limit") != NULL,
              "oversized source was accepted: %s", error);
        runner.destroy(runner.context);
    }
    CHECK(model.destroys == 2U,
          "work/format model contexts were not destroyed twice");
    test_wave_destroy(&wave);
}

static void test_nonfinite_and_deadline(void)
{
    TestModelMode modes[2] = {TEST_MODEL_NONFINITE, TEST_MODEL_DELAY};
    size_t index;
    for (index = 0U; index < 2U; ++index) {
        TestWave wave;
        TestModel model;
        HWAInstrumentStemRunner runner;
        HWAInstrumentStemResult *stems = NULL;
        size_t stem_count = 0U;
        char error[HWA_ERROR_SIZE] = {0};
        uint64_t timeout = modes[index] == TEST_MODEL_DELAY
                               ? UINT64_C(5) : UINT64_C(10000);
        memset(&model, 0, sizeof(model));
        memset(&runner, 0, sizeof(runner));
        model.mode = modes[index];
        CHECK(test_wave_create(&wave, UINT64_C(5), UINT16_C(1)) == 0,
              "cannot create failure fixture");
        if (wave.bytes == NULL) continue;
        CHECK(test_runner_init(
                  &model, TEST_WORK_BYTES, &runner,
                  error, sizeof(error)) == 0,
              "failure runner setup failed: %s", error);
        if (runner.context != NULL) {
            CHECK(runner.run(
                      runner.context, &wave.source, &wave.format,
                      UINT64_C(0), timeout, &stems, &stem_count,
                      error, sizeof(error)) != 0 &&
                      stems == NULL && stem_count == 0U,
                  "%s output was accepted: %s",
                  modes[index] == TEST_MODEL_NONFINITE
                      ? "non-finite" : "late",
                  error);
            if (modes[index] == TEST_MODEL_NONFINITE) {
                CHECK(model.calls == 1U, "failure case used %zu calls",
                      model.calls);
                CHECK(strstr(error, "non-finite") != NULL,
                      "non-finite error is unclear: %s", error);
            } else {
                CHECK(model.calls <= 1U, "failure case used %zu calls",
                      model.calls);
                CHECK(strstr(error, "deadline expired") != NULL,
                      "deadline error is unclear: %s", error);
            }
            runner.destroy(runner.context);
        }
        CHECK(model.destroys == 1U,
              "failure model was not destroyed once");
        test_wave_destroy(&wave);
    }
}

static void test_input_range(void)
{
    TestWave wave;
    TestModel model;
    HWAInstrumentStemRunner runner;
    HWAInstrumentStemResult *stems = NULL;
    size_t stem_count = 0U;
    char error[HWA_ERROR_SIZE] = {0};
    memset(&model, 0, sizeof(model));
    memset(&runner, 0, sizeof(runner));
    model.mode = TEST_MODEL_MONO;
    CHECK(test_float_wave_create(&wave, 1.25f) == 0,
          "cannot create out-of-range float fixture");
    if (wave.bytes == NULL) return;
    CHECK(test_runner_init(
              &model, TEST_WORK_BYTES, &runner,
              error, sizeof(error)) == 0,
          "range runner setup failed: %s", error);
    if (runner.context != NULL) {
        CHECK(runner.run(
                  runner.context, &wave.source, &wave.format,
                  UINT64_C(0), UINT64_C(10000), &stems, &stem_count,
                  error, sizeof(error)) != 0 &&
                  stems == NULL && stem_count == 0U && model.calls == 0U &&
                  strstr(error, "outside [-1, 1]") != NULL,
              "out-of-range input was accepted: %s", error);
        runner.destroy(runner.context);
    }
    CHECK(model.destroys == 1U,
          "range model context was not destroyed once");
    test_wave_destroy(&wave);
}

static void test_init_failure_keeps_context(void)
{
    TestModel model;
    HWAHTDemucsModelRunner low_level;
    HWAInstrumentStemRunner runner;
    char error[HWA_ERROR_SIZE] = {0};
    memset(&model, 0, sizeof(model));
    memset(&low_level, 0, sizeof(low_level));
    memset(&runner, 0, sizeof(runner));
    low_level.context = &model;
    low_level.runtime_name = "fake";
    low_level.runtime_version = "1";
    low_level.backend = "cpu";
    low_level.fallback = "";
    low_level.run_window = test_model_run;
    low_level.destroy = test_model_destroy;
    CHECK(hwa_htdemucs_instrument_runner_init(
              &runner, UINT64_C(1), &low_level,
              error, sizeof(error)) != 0 &&
              runner.context == NULL && model.destroys == 0U,
          "failed init took ownership of the model context");
}

int main(void)
{
    test_mono_order_and_shape();
    test_overlap_add();
    test_work_limit_and_bad_format();
    test_nonfinite_and_deadline();
    test_input_range();
    test_init_failure_keeps_context();
    if (failures != 0) {
        (void)fprintf(stderr, "%d HTDemucs test(s) failed\n", failures);
        return 1;
    }
    (void)puts("HTDemucs adapter tests passed");
    return 0;
}

#include "basic_pitch_provider.h"
#include "inference_clock.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_SOURCE_FRAMES UINT64_C(73000)
#define TEST_WORK_BYTES (UINT64_C(64) * 1024U * 1024U)

static const char test_model_sha256[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char test_adapter_sha256[] =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

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

typedef struct TestBytes {
    const unsigned char *data;
    size_t size;
} TestBytes;

typedef struct TestWave {
    unsigned char *data;
    size_t size;
    uint64_t frames;
    uint32_t sample_rate;
    uint16_t channels;
} TestWave;

typedef struct TestRunnerState {
    uint64_t source_frames;
    size_t calls;
    size_t destroy_calls;
    int emit_second_note;
    uint64_t runner_delay_milliseconds;
} TestRunnerState;

typedef struct TestRunnerContext {
    TestRunnerState *state;
} TestRunnerContext;

typedef struct TestCase {
    TestWave wave;
    TestBytes bytes;
    TestRunnerState runner_state;
    HWAInferenceInput input;
    HWAInferenceRequest request;
    HWAInferenceProvider provider;
    HWABasicPitchDecoderOptions decoder_options;
    char source_sha256[HWA_SHA256_HEX_SIZE];
    char *settings_json;
    void *task;
    TestRunnerContext *unowned_runner;
    int provider_open;
} TestCase;

static void test_le16(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char)(value & UINT16_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT16_C(0xff));
}

static void test_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value & UINT32_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT32_C(0xff));
    bytes[2] = (unsigned char)((value >> 16U) & UINT32_C(0xff));
    bytes[3] = (unsigned char)((value >> 24U) & UINT32_C(0xff));
}

static int32_t test_sample_value(uint64_t frame)
{
    return (int32_t)(frame % UINT64_C(20001)) - INT32_C(10000);
}

static int test_wave_build(TestWave *wave,
                           uint64_t frames,
                           uint32_t sample_rate,
                           uint16_t channels)
{
    uint64_t data_bytes;
    uint64_t file_bytes;
    uint32_t block_align;
    uint64_t frame;
    if (wave == NULL || frames == 0U || sample_rate == 0U ||
        channels == 0U ||
        frames > UINT64_MAX / (uint64_t)channels / 2U)
        return -1;
    memset(wave, 0, sizeof(*wave));
    data_bytes = frames * (uint64_t)channels * 2U;
    file_bytes = data_bytes + 44U;
    block_align = (uint32_t)channels * 2U;
    if (data_bytes > UINT32_MAX - 36U || file_bytes > (uint64_t)SIZE_MAX ||
        block_align > UINT16_MAX ||
        sample_rate > UINT32_MAX / block_align)
        return -1;
    wave->data = (unsigned char *)calloc((size_t)file_bytes, 1U);
    if (wave->data == NULL) return -1;
    wave->size = (size_t)file_bytes;
    wave->frames = frames;
    wave->sample_rate = sample_rate;
    wave->channels = channels;
    memcpy(wave->data, "RIFF", 4U);
    test_le32(wave->data + 4U, (uint32_t)data_bytes + 36U);
    memcpy(wave->data + 8U, "WAVEfmt ", 8U);
    test_le32(wave->data + 16U, UINT32_C(16));
    test_le16(wave->data + 20U, UINT16_C(1));
    test_le16(wave->data + 22U, channels);
    test_le32(wave->data + 24U, sample_rate);
    test_le32(wave->data + 28U, sample_rate * block_align);
    test_le16(wave->data + 32U, (uint16_t)block_align);
    test_le16(wave->data + 34U, UINT16_C(16));
    memcpy(wave->data + 36U, "data", 4U);
    test_le32(wave->data + 40U, (uint32_t)data_bytes);
    for (frame = 0U; frame < frames; ++frame) {
        uint16_t channel;
        int32_t sample = test_sample_value(frame);
        uint16_t bits = sample < 0
                            ? (uint16_t)(UINT32_C(65536) +
                                         (uint32_t)sample)
                            : (uint16_t)sample;
        for (channel = 0U; channel < channels; ++channel) {
            uint64_t sample_index =
                frame * (uint64_t)channels + (uint64_t)channel;
            test_le16(wave->data + 44U + (size_t)sample_index * 2U, bits);
        }
    }
    return 0;
}

static int test_read_at(void *context,
                        uint64_t offset,
                        unsigned char *destination,
                        size_t size)
{
    const TestBytes *bytes = (const TestBytes *)context;
    if (bytes == NULL || destination == NULL ||
        offset > (uint64_t)bytes->size ||
        (uint64_t)size > (uint64_t)bytes->size - offset)
        return -1;
    memcpy(destination, bytes->data + (size_t)offset, size);
    return 0;
}

static void test_runner_error(char *error,
                              size_t error_size,
                              const char *message)
{
    if (error == NULL || error_size == 0U) return;
    (void)snprintf(error, error_size, "%s", message);
    error[error_size - 1U] = '\0';
}

static int test_run_window(
    void *context_value,
    const float input[HWA_BASIC_PITCH_INPUT_SAMPLES],
    float note_output[HWA_BASIC_PITCH_OUTPUT_FRAMES *
                      HWA_BASIC_PITCH_NOTE_BINS],
    float onset_output[HWA_BASIC_PITCH_OUTPUT_FRAMES *
                       HWA_BASIC_PITCH_NOTE_BINS],
    char *error,
    size_t error_size)
{
    TestRunnerContext *context = (TestRunnerContext *)context_value;
    TestRunnerState *state;
    size_t input_index;
    size_t output_frame;
    size_t call;
    if (context == NULL || context->state == NULL || input == NULL ||
        note_output == NULL || onset_output == NULL) {
        test_runner_error(error, error_size, "invalid fake runner call");
        return -1;
    }
    state = context->state;
    call = state->calls;
    for (input_index = 0U;
         input_index < HWA_BASIC_PITCH_INPUT_SAMPLES;
         ++input_index) {
        int64_t source_frame =
            (int64_t)(call * HWA_BASIC_PITCH_WINDOW_STEP_SAMPLES) -
            (int64_t)HWA_BASIC_PITCH_LEFT_PAD_SAMPLES +
            (int64_t)input_index;
        float expected = 0.0f;
        if (source_frame >= 0 &&
            (uint64_t)source_frame < state->source_frames) {
            expected = (float)((double)test_sample_value(
                                   (uint64_t)source_frame) /
                               32768.0);
        }
        if (input[input_index] != expected) {
            test_runner_error(
                error, error_size,
                "fake runner received the wrong scheduled sample");
            return -1;
        }
    }
    memset(note_output, 0,
           HWA_BASIC_PITCH_OUTPUT_FRAMES * HWA_BASIC_PITCH_NOTE_BINS *
               sizeof(*note_output));
    memset(onset_output, 0,
           HWA_BASIC_PITCH_OUTPUT_FRAMES * HWA_BASIC_PITCH_NOTE_BINS *
               sizeof(*onset_output));
    for (output_frame = 0U;
         output_frame < HWA_BASIC_PITCH_CROP_FRAMES;
         ++output_frame) {
        note_output[output_frame * HWA_BASIC_PITCH_NOTE_BINS + 7U] = 0.95f;
    }
    onset_output[2U * HWA_BASIC_PITCH_NOTE_BINS + 7U] = 0.99f;
    for (output_frame = HWA_BASIC_PITCH_OUTPUT_FRAMES -
                            HWA_BASIC_PITCH_CROP_FRAMES;
         output_frame < HWA_BASIC_PITCH_OUTPUT_FRAMES;
         ++output_frame) {
        note_output[output_frame * HWA_BASIC_PITCH_NOTE_BINS + 8U] = 0.95f;
    }
    onset_output[(HWA_BASIC_PITCH_OUTPUT_FRAMES -
                  HWA_BASIC_PITCH_CROP_FRAMES + 2U) *
                     HWA_BASIC_PITCH_NOTE_BINS +
                 8U] = 0.99f;
    for (output_frame = 0U;
         output_frame < HWA_BASIC_PITCH_KEPT_FRAMES;
         ++output_frame) {
        size_t global_frame =
            call * HWA_BASIC_PITCH_KEPT_FRAMES + output_frame;
        size_t model_frame = output_frame + HWA_BASIC_PITCH_CROP_FRAMES;
        if (global_frame >= 140U && global_frame < 160U)
            note_output[model_frame * HWA_BASIC_PITCH_NOTE_BINS + 48U] =
                0.8f;
        if (global_frame == 140U)
            onset_output[model_frame * HWA_BASIC_PITCH_NOTE_BINS + 48U] =
                0.9f;
        if (state->emit_second_note &&
            global_frame >= 100U && global_frame < 120U)
            note_output[model_frame * HWA_BASIC_PITCH_NOTE_BINS + 52U] =
                0.75f;
        if (state->emit_second_note && global_frame == 100U)
            onset_output[model_frame * HWA_BASIC_PITCH_NOTE_BINS + 52U] =
                0.85f;
    }
    if (state->runner_delay_milliseconds != 0U) {
        uint64_t started = 0U;
        if (hwa_inference_deadline_start(
                &started, error, error_size) != 0)
            return -1;
        while (hwa_inference_deadline_check(
                   started, state->runner_delay_milliseconds,
                   error, error_size) == 0) {
        }
    }
    state->calls++;
    return 0;
}

static void test_runner_destroy(void *context_value)
{
    TestRunnerContext *context = (TestRunnerContext *)context_value;
    if (context == NULL) return;
    if (context->state != NULL) context->state->destroy_calls++;
    free(context);
}

static void test_case_cleanup(TestCase *test)
{
    if (test->task != NULL && test->provider.task_free != NULL) {
        test->provider.task_free(test->provider.context, test->task);
        test->task = NULL;
    }
    if (test->provider_open) {
        hwa_inference_provider_destroy(&test->provider);
        test->provider_open = 0;
    }
    if (test->unowned_runner != NULL) {
        test_runner_destroy(test->unowned_runner);
        test->unowned_runner = NULL;
    }
    free(test->settings_json);
    test->settings_json = NULL;
    free(test->wave.data);
    test->wave.data = NULL;
}

static int test_case_init(TestCase *test,
                          uint64_t frames,
                          uint32_t sample_rate,
                          uint16_t channels,
                          uint64_t provider_work_bytes)
{
    HWABasicPitchRunner runner;
    char error[HWA_ERROR_SIZE] = {0};
    uint32_t block_align = (uint32_t)channels * 2U;
    memset(test, 0, sizeof(*test));
    memset(&runner, 0, sizeof(runner));
    if (test_wave_build(
            &test->wave, frames, sample_rate, channels) != 0) {
        CHECK(0, "cannot build the WAVE fixture");
        return 0;
    }
    test->bytes.data = test->wave.data;
    test->bytes.size = test->wave.size;
    test->input.id = "source";
    test->input.role = "source-recording";
    test->input.media_type = "audio/wav";
    test->input.sha256 = test->source_sha256;
    test->input.bytes.context = &test->bytes;
    test->input.bytes.name = "memory.wav";
    test->input.bytes.size = (uint64_t)test->wave.size;
    test->input.bytes.read_at = test_read_at;
    if (hwa_inference_byte_source_sha256(
            &test->input.bytes, (uint64_t)test->wave.size,
            test->source_sha256, error, sizeof(error)) != 0) {
        CHECK(0, "cannot hash the WAVE fixture: %s", error);
        test_case_cleanup(test);
        return 0;
    }
    hwa_basic_pitch_decoder_options_default(&test->decoder_options);
    test->decoder_options.infer_onsets = 0;
    test->decoder_options.melodia = 0;
    if (hwa_basic_pitch_task_settings_build(
            &test->decoder_options, &test->settings_json,
            error, sizeof(error)) != 0) {
        CHECK(0, "cannot build test task settings: %s", error);
        test_case_cleanup(test);
        return 0;
    }
    test->runner_state.source_frames = frames;
    test->unowned_runner =
        (TestRunnerContext *)calloc(1U, sizeof(*test->unowned_runner));
    if (test->unowned_runner == NULL) {
        CHECK(0, "cannot allocate the fake runner");
        test_case_cleanup(test);
        return 0;
    }
    test->unowned_runner->state = &test->runner_state;
    runner.context = test->unowned_runner;
    runner.runtime_name = "fake-runtime";
    runner.runtime_version = "1.2.3";
    runner.backend = "cpu";
    runner.fallback = "";
    runner.run_window = test_run_window;
    runner.destroy = test_runner_destroy;
    if (hwa_basic_pitch_provider_init(
            &test->provider, test_model_sha256, test_adapter_sha256,
            &test->decoder_options, provider_work_bytes, &runner,
            error, sizeof(error)) != 0) {
        CHECK(0, "cannot initialize test provider: %s", error);
        test_case_cleanup(test);
        return 0;
    }
    test->provider_open = 1;
    test->unowned_runner = NULL;
    memset(&test->request, 0, sizeof(test->request));
    hwa_event_bundle_limits_default(&test->request.output_limits);
    test->request.output_limits.max_events = 16U;
    test->request.output_limits.max_values = 16U;
    test->request.output_limits.max_work_bytes = TEST_WORK_BYTES;
    test->request.task = HWA_BASIC_PITCH_TASK_NAME;
    test->request.settings_json = test->settings_json;
    test->request.expected_provider_name = HWA_BASIC_PITCH_PROVIDER_NAME;
    test->request.expected_provider_version =
        HWA_BASIC_PITCH_PROVIDER_VERSION;
    test->request.expected_model_sha256 = test_model_sha256;
    test->request.seed = UINT64_C(0);
    test->request.source_recording_id = UINT64_C(7);
    test->request.source_input_id = test->input.id;
    test->request.inputs = &test->input;
    test->request.input_count = 1U;
    test->request.source_format.container = HWA_CONTAINER_RIFF;
    test->request.source_format.encoding = HWA_ENCODING_PCM;
    test->request.source_format.channels = channels;
    test->request.source_format.sample_rate_hz = sample_rate;
    test->request.source_format.bits_per_sample = 16U;
    test->request.source_format.valid_bits_per_sample = 16U;
    test->request.source_format.block_align = (uint16_t)block_align;
    test->request.source_format.frames = frames;
    test->request.source_format.data_bytes =
        frames * (uint64_t)block_align;
    test->request.source_format.duration_seconds =
        (double)frames / (double)sample_rate;
    test->request.max_input_file_bytes = (uint64_t)test->wave.size;
    test->request.max_input_bytes = (uint64_t)test->wave.size;
    test->request.timeout_milliseconds = UINT64_C(1000);
    return 1;
}

static void test_schedule_events_provenance_and_ownership(void)
{
    static const char expected_task_settings[] =
        "{\"thresholds\":{\"energy_tolerance_frames\":11,"
        "\"frame_threshold\":0.29999999999999999,"
        "\"minimum_note_frames\":11,\"onset_threshold\":0.5},"
        "\"decoder\":{\"infer_onsets\":false,\"melodia\":false},"
        "\"model_frame_rate\":{\"numerator\":11025,\"denominator\":128},"
        "\"mapping_rule\":\"model-frame-boundary-start-floor-end-ceil-clip-v1\","
        "\"window_schedule\":{\"crop_frames_each_side\":15,"
        "\"input_samples\":43844,\"kept_frames\":142,"
        "\"left_pad_samples\":3840,\"output_frames\":172,"
        "\"step_samples\":36352}}";
    TestCase test;
    HWAInferencePollState state = HWA_INFERENCE_PENDING;
    const HWAInferenceOutput *output = NULL;
    const HWAEventBundle *bundle;
    const HWAPerformanceEvent *event;
    const HWAEventValue *value;
    char expected_provenance[4096];
    char error[HWA_ERROR_SIZE] = {0};
    int written;
    if (!test_case_init(
            &test, TEST_SOURCE_FRAMES, HWA_BASIC_PITCH_SAMPLE_RATE,
            1U, TEST_WORK_BYTES))
        return;
    CHECK(strcmp(test.settings_json, expected_task_settings) == 0,
          "task settings are not canonical: %s", test.settings_json);
    if (test.provider.start(
            test.provider.context, &test.request, &test.task,
            error, sizeof(error)) != 0) {
        CHECK(0, "valid provider start failed: %s", error);
        test_case_cleanup(&test);
        return;
    }
    CHECK(test.task != NULL, "provider start returned no task");
    CHECK(test.runner_state.calls == 3U,
          "uniform schedule ran %zu windows, wanted 3",
          test.runner_state.calls);
    CHECK(test.provider.poll(
              test.provider.context, test.task, &state, &output,
              error, sizeof(error)) == 0,
          "provider poll failed: %s", error);
    CHECK(state == HWA_INFERENCE_READY && output != NULL,
          "synchronous provider was not ready on its first poll");
    if (output == NULL || output->bundle == NULL) {
        test_case_cleanup(&test);
        return;
    }
    bundle = output->bundle;
    CHECK(output->payload_count == 0U && output->payloads == NULL,
          "provider returned payloads");
    CHECK(bundle->provider_count == 1U && bundle->audio_count == 1U &&
              bundle->trace_count == 0U && bundle->event_count == 1U,
          "provider returned the wrong bundle counts");
    CHECK(strcmp(bundle->providers[0].name,
                 HWA_BASIC_PITCH_PROVIDER_NAME) == 0 &&
              strcmp(bundle->providers[0].version,
                     HWA_BASIC_PITCH_PROVIDER_VERSION) == 0 &&
              strcmp(bundle->providers[0].model_sha256,
                     test_model_sha256) == 0,
          "provider row has the wrong identity");
    written = snprintf(
        expected_provenance, sizeof(expected_provenance),
        "{\"task\":\"%s\",\"seed\":\"00000000000000000000\","
        "\"inputs\":[{\"id\":\"source\",\"role\":\"source-recording\","
        "\"media_type\":\"audio/wav\",\"name\":\"memory.wav\","
        "\"bytes\":\"%020llu\",\"sha256\":\"%s\"}],"
        "\"runtime\":{\"name\":\"fake-runtime\",\"version\":\"1.2.3\","
        "\"backend\":\"cpu\",\"fallback\":\"\","
        "\"adapter_sha256\":\"%s\"},\"task_settings\":%s}",
        HWA_BASIC_PITCH_TASK_NAME,
        (unsigned long long)test.wave.size, test.source_sha256,
        test_adapter_sha256, expected_task_settings);
    CHECK(written > 0 && (size_t)written < sizeof(expected_provenance),
          "expected provenance text overflowed");
    if (written > 0 && (size_t)written < sizeof(expected_provenance)) {
        CHECK(strcmp(bundle->providers[0].settings_json,
                     expected_provenance) == 0,
              "provider provenance is not canonical: %s",
              bundle->providers[0].settings_json);
    }
    CHECK(bundle->audio[0].id == UINT64_C(7) &&
              bundle->audio[0].kind == HWA_EVENT_SOURCE_RECORDING &&
              strcmp(bundle->audio[0].name, "memory.wav") == 0 &&
              strcmp(bundle->audio[0].sha256, test.source_sha256) == 0 &&
              bundle->audio[0].format.frames == TEST_SOURCE_FRAMES,
          "source audio row does not match its request");
    event = &bundle->events[0];
    CHECK(strcmp(event->kind, "note") == 0 &&
              event->source_recording_id == UINT64_C(7) &&
              event->evidence_audio_id_valid &&
              event->evidence_audio_id == UINT64_C(7) &&
              event->start_sample == UINT64_C(35840) &&
              event->end_sample == UINT64_C(40960) &&
              event->value_count == 1U && event->trace_ref_count == 0U,
          "cross-window note has the wrong event fields");
    value = &event->values[0];
    CHECK(strcmp(value->name, "pitch-hz") == 0 &&
              value->kind == HWA_EVENT_VALUE_F64 &&
              value->basis == HWA_EVENT_INFERENCE &&
              fabs(value->number - 440.0) < 1e-12 &&
              strcmp(value->unit, "Hz") == 0 &&
              fabs(value->score - 0.8) < 1e-6 &&
              value->provider_id_valid &&
              value->provider_id == UINT64_C(1) &&
              value->score_valid && value->selected,
          "cross-window note has the wrong selected pitch");
    CHECK(hwa_inference_output_validate_for_request(
              &test.provider, &test.request, output,
              error, sizeof(error)) == 0,
          "provider output did not validate: %s", error);
    test_case_cleanup(&test);
    CHECK(test.runner_state.destroy_calls == 1U,
          "provider destroyed the runner %zu times",
          test.runner_state.destroy_calls);
}

static void test_bad_settings_fail_before_runner(void)
{
    TestCase test;
    char error[HWA_ERROR_SIZE] = {0};
    if (!test_case_init(
            &test, TEST_SOURCE_FRAMES, HWA_BASIC_PITCH_SAMPLE_RATE,
            1U, TEST_WORK_BYTES))
        return;
    test.request.settings_json = "{}";
    CHECK(test.provider.start(
              test.provider.context, &test.request, &test.task,
              error, sizeof(error)) != 0 && test.task == NULL,
          "provider accepted non-canonical task settings");
    CHECK(test.runner_state.calls == 0U,
          "bad settings reached the runner");
    CHECK(strstr(error, "settings") != NULL,
          "bad settings gave the wrong error: %s", error);
    test_case_cleanup(&test);
    CHECK(test.runner_state.destroy_calls == 1U,
          "bad-settings cleanup did not destroy the owned runner");
}

static void test_settings_whitespace_is_normalized(void)
{
    TestCase test;
    HWAInferencePollState state = HWA_INFERENCE_PENDING;
    const HWAInferenceOutput *output = NULL;
    char *spaced = NULL;
    size_t settings_size;
    char error[HWA_ERROR_SIZE] = {0};
    if (!test_case_init(
            &test, TEST_SOURCE_FRAMES, HWA_BASIC_PITCH_SAMPLE_RATE,
            1U, TEST_WORK_BYTES))
        return;
    settings_size = strlen(test.settings_json);
    spaced = (char *)malloc(settings_size + 5U);
    if (spaced == NULL) {
        CHECK(0, "cannot allocate spaced task settings");
        test_case_cleanup(&test);
        return;
    }
    (void)snprintf(spaced, settings_size + 5U, " \n%s\t ",
                   test.settings_json);
    test.request.settings_json = spaced;
    CHECK(test.provider.start(
              test.provider.context, &test.request, &test.task,
              error, sizeof(error)) == 0 && test.task != NULL,
          "provider rejected JSON white space: %s", error);
    if (test.task != NULL) {
        CHECK(test.provider.poll(
                  test.provider.context, test.task, &state, &output,
                  error, sizeof(error)) == 0 &&
                  state == HWA_INFERENCE_READY && output != NULL,
              "spaced-settings task did not become ready: %s", error);
        if (output != NULL && output->bundle != NULL &&
            output->bundle->provider_count == 1U) {
            CHECK(strstr(output->bundle->providers[0].settings_json,
                         "\"task_settings\":{") != NULL,
                  "saved settings were not normalized: %s",
                  output->bundle->providers[0].settings_json);
        }
    }
    test_case_cleanup(&test);
    free(spaced);
}

static void test_extra_input_fails_before_runner(void)
{
    TestCase test;
    HWAInferenceInput inputs[2];
    char error[HWA_ERROR_SIZE] = {0};
    if (!test_case_init(
            &test, TEST_SOURCE_FRAMES, HWA_BASIC_PITCH_SAMPLE_RATE,
            1U, TEST_WORK_BYTES))
        return;
    inputs[0] = test.input;
    inputs[1] = test.input;
    inputs[1].id = "context";
    inputs[1].role = "score-context";
    test.request.inputs = inputs;
    test.request.input_count = 2U;
    test.request.max_input_bytes = (uint64_t)test.wave.size * 2U;
    CHECK(test.provider.start(
              test.provider.context, &test.request, &test.task,
              error, sizeof(error)) != 0 && test.task == NULL,
          "provider accepted an unhandled extra input");
    CHECK(test.runner_state.calls == 0U,
          "extra input reached the runner");
    CHECK(strstr(error, "input count") != NULL,
          "extra input gave the wrong error: %s", error);
    test_case_cleanup(&test);
}

static void test_late_runner_result_is_rejected(void)
{
    TestCase test;
    char error[HWA_ERROR_SIZE] = {0};
    if (!test_case_init(
            &test, TEST_SOURCE_FRAMES, HWA_BASIC_PITCH_SAMPLE_RATE,
            1U, TEST_WORK_BYTES))
        return;
    test.runner_state.runner_delay_milliseconds = UINT64_C(60);
    test.request.timeout_milliseconds = UINT64_C(50);
    CHECK(test.provider.start(
              test.provider.context, &test.request, &test.task,
              error, sizeof(error)) != 0 && test.task == NULL,
          "provider accepted a late runner result");
    CHECK(test.runner_state.calls == 1U,
          "deadline ran %zu model windows, wanted 1",
          test.runner_state.calls);
    CHECK(strstr(error, "deadline expired") != NULL,
          "late runner gave the wrong error: %s", error);
    test_case_cleanup(&test);
}

static void test_bad_clock_fails_before_runner(void)
{
    TestCase test;
    char error[HWA_ERROR_SIZE] = {0};
    if (!test_case_init(
            &test, UINT64_C(512), UINT32_C(44100),
            1U, TEST_WORK_BYTES))
        return;
    CHECK(test.provider.start(
              test.provider.context, &test.request, &test.task,
              error, sizeof(error)) != 0 && test.task == NULL,
          "provider accepted a 44100 Hz source");
    CHECK(test.runner_state.calls == 0U,
          "bad source clock reached the runner");
    CHECK(strstr(error, "mono 22050 Hz") != NULL,
          "bad source clock gave the wrong error: %s", error);
    test_case_cleanup(&test);
}

static void test_work_and_event_limits_fail_closed(void)
{
    TestCase test;
    char error[HWA_ERROR_SIZE] = {0};
    if (test_case_init(
            &test, TEST_SOURCE_FRAMES, HWA_BASIC_PITCH_SAMPLE_RATE,
            1U, UINT64_C(4096))) {
        CHECK(test.provider.start(
                  test.provider.context, &test.request, &test.task,
                  error, sizeof(error)) != 0 && test.task == NULL,
              "provider accepted a tiny inference work limit");
        CHECK(test.runner_state.calls == 0U,
              "tiny work limit reached the runner");
        CHECK(strstr(error, "work limit") != NULL,
              "tiny work limit gave the wrong error: %s", error);
        test_case_cleanup(&test);
    }
    if (!test_case_init(
            &test, TEST_SOURCE_FRAMES, HWA_BASIC_PITCH_SAMPLE_RATE,
            1U, TEST_WORK_BYTES))
        return;
    test.request.output_limits.max_work_bytes = UINT64_C(4096);
    CHECK(test.provider.start(
              test.provider.context, &test.request, &test.task,
              error, sizeof(error)) != 0 && test.task == NULL,
          "provider ignored a tiny request work limit");
    CHECK(test.runner_state.calls == 0U,
          "tiny request work limit reached the runner");
    CHECK(strstr(error, "work limit") != NULL,
          "tiny request work limit gave the wrong error: %s", error);
    test.request.output_limits.max_work_bytes = TEST_WORK_BYTES;
    error[0] = '\0';
    test.runner_state.emit_second_note = 1;
    test.request.output_limits.max_events = 1U;
    test.request.output_limits.max_values = 1U;
    CHECK(test.provider.start(
              test.provider.context, &test.request, &test.task,
              error, sizeof(error)) != 0 && test.task == NULL,
          "provider exceeded the note event limit");
    CHECK(strstr(error, "note count") != NULL,
          "event limit gave the wrong error: %s", error);
    test_case_cleanup(&test);
}

static void test_failed_init_keeps_runner_ownership(void)
{
    TestRunnerState state;
    TestRunnerContext *context;
    HWABasicPitchRunner runner;
    HWABasicPitchDecoderOptions options;
    HWAInferenceProvider provider;
    char error[HWA_ERROR_SIZE] = {0};
    memset(&state, 0, sizeof(state));
    memset(&runner, 0, sizeof(runner));
    memset(&provider, 0xff, sizeof(provider));
    context = (TestRunnerContext *)calloc(1U, sizeof(*context));
    if (context == NULL) {
        CHECK(0, "cannot allocate failed-init runner");
        return;
    }
    context->state = &state;
    runner.context = context;
    runner.runtime_name = "fake-runtime";
    runner.runtime_version = "1.2.3";
    runner.backend = "cpu";
    runner.fallback = "";
    runner.run_window = test_run_window;
    runner.destroy = test_runner_destroy;
    hwa_basic_pitch_decoder_options_default(&options);
    CHECK(hwa_basic_pitch_provider_init(
              &provider, test_model_sha256, "not-a-sha256", &options,
              TEST_WORK_BYTES, &runner, error, sizeof(error)) != 0,
          "provider init accepted a bad adapter hash");
    CHECK(provider.context == NULL && state.destroy_calls == 0U,
          "failed provider init took runner ownership");
    test_runner_destroy(context);
    CHECK(state.destroy_calls == 1U,
          "caller could not destroy its retained runner");
}

int main(void)
{
    test_schedule_events_provenance_and_ownership();
    test_bad_settings_fail_before_runner();
    test_settings_whitespace_is_normalized();
    test_extra_input_fails_before_runner();
    test_late_runner_result_is_rejected();
    test_bad_clock_fails_before_runner();
    test_work_and_event_limits_fail_closed();
    test_failed_init_keeps_runner_ownership();
    if (failures != 0) {
        (void)fprintf(stderr, "%d Basic Pitch provider test(s) failed\n",
                      failures);
        return 1;
    }
    (void)puts("Basic Pitch provider tests passed");
    return 0;
}

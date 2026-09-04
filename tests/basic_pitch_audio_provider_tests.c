#include "basic_pitch_audio_provider.h"

#include "basic_pitch_provider.h"
#include "inference_clock.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_RAW_FRAMES UINT64_C(129)
#define TEST_PREPARED_FRAMES UINT64_C(65)
#define TEST_MAX_WORK_BYTES (UINT64_C(1024) * UINT64_C(1024))

static const char test_model_sha256[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char test_adapter_sha256[] =
    "dddddddddddddddddddddddddddddddd"
    "dddddddddddddddddddddddddddddddd";

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
    unsigned char *data;
    size_t size;
} TestBytes;

typedef struct TestRawWave {
    TestBytes bytes;
    HWAFormat format;
} TestRawWave;

typedef struct TestChildState {
    size_t starts;
    size_t frees;
    size_t destroys;
    uint64_t delay_milliseconds;
    int return_null_task;
    uint64_t prepared_frames[4];
    uint64_t prepared_bytes[4];
    uint64_t child_timeouts[4];
    uint64_t child_work_bytes[4];
    float centre_samples[4];
    char prepared_sha256[4][HWA_SHA256_HEX_SIZE];
} TestChildState;

typedef struct TestChildTask {
    HWAInferenceOutput output;
    HWAEventBundle bundle;
    HWAEventProvider provider;
    HWAEventAudio audio;
    HWAPerformanceEvent events[2];
    HWAEventValue values[2];
} TestChildTask;

typedef struct TestCase {
    TestRawWave wave;
    HWAInferenceInput input;
    HWAInferenceRequest request;
    char source_sha256[HWA_SHA256_HEX_SIZE];
    TestChildState child_state;
    HWAInferenceProvider child;
    HWAInferenceProvider provider;
    int child_open;
    int provider_open;
} TestCase;

static int test_child_start(void *context,
                            const HWAInferenceRequest *request,
                            void **task,
                            char *error,
                            size_t error_size);
static int test_child_poll(void *context,
                           void *task,
                           HWAInferencePollState *state,
                           const HWAInferenceOutput **output,
                           char *error,
                           size_t error_size);
static void test_child_task_free(void *context, void *task);
static void test_child_destroy(void *context);

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

static uint16_t test_get_le16(const unsigned char *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] |
                      ((uint16_t)bytes[1] << 8U));
}

static uint32_t test_get_le32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static void test_float_le(unsigned char *bytes, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    test_le32(bytes, bits);
}

static int test_wave_build(TestRawWave *wave)
{
    const uint16_t channels = UINT16_C(2);
    const uint16_t block_align = UINT16_C(8);
    const uint32_t data_bytes =
        (uint32_t)(TEST_RAW_FRAMES * (uint64_t)block_align);
    const size_t file_bytes = (size_t)data_bytes + 44U;
    uint64_t frame;
    memset(wave, 0, sizeof(*wave));
    wave->bytes.data = (unsigned char *)calloc(file_bytes, 1U);
    if (wave->bytes.data == NULL) return -1;
    wave->bytes.size = file_bytes;
    memcpy(wave->bytes.data, "RIFF", 4U);
    test_le32(wave->bytes.data + 4U, data_bytes + UINT32_C(36));
    memcpy(wave->bytes.data + 8U, "WAVEfmt ", 8U);
    test_le32(wave->bytes.data + 16U, UINT32_C(16));
    test_le16(wave->bytes.data + 20U, UINT16_C(3));
    test_le16(wave->bytes.data + 22U, channels);
    test_le32(wave->bytes.data + 24U, UINT32_C(44100));
    test_le32(wave->bytes.data + 28U, UINT32_C(352800));
    test_le16(wave->bytes.data + 32U, block_align);
    test_le16(wave->bytes.data + 34U, UINT16_C(32));
    memcpy(wave->bytes.data + 36U, "data", 4U);
    test_le32(wave->bytes.data + 40U, data_bytes);
    for (frame = 0U; frame < TEST_RAW_FRAMES; ++frame) {
        size_t offset = 44U + (size_t)frame * (size_t)block_align;
        test_float_le(wave->bytes.data + offset, 0.25f);
        test_float_le(wave->bytes.data + offset + 4U, 0.75f);
    }
    wave->format.container = HWA_CONTAINER_RIFF;
    wave->format.encoding = HWA_ENCODING_IEEE_FLOAT;
    wave->format.channels = channels;
    wave->format.sample_rate_hz = UINT32_C(44100);
    wave->format.bits_per_sample = UINT16_C(32);
    wave->format.valid_bits_per_sample = UINT16_C(32);
    wave->format.block_align = block_align;
    wave->format.frames = TEST_RAW_FRAMES;
    wave->format.data_bytes = data_bytes;
    wave->format.duration_seconds =
        (double)TEST_RAW_FRAMES / 44100.0;
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

static void test_error(char *error,
                       size_t error_size,
                       const char *message)
{
    if (error == NULL || error_size == 0U) return;
    (void)snprintf(error, error_size, "%s", message);
    error[error_size - 1U] = '\0';
}

static void test_child_descriptor(TestChildState *state,
                                  HWAInferenceProvider *provider)
{
    memset(provider, 0, sizeof(*provider));
    provider->name = HWA_BASIC_PITCH_PROVIDER_NAME;
    provider->version = HWA_BASIC_PITCH_PROVIDER_VERSION;
    provider->model_sha256 = test_model_sha256;
    provider->context = state;
    provider->start = test_child_start;
    provider->poll = test_child_poll;
    provider->task_free = test_child_task_free;
    provider->destroy = test_child_destroy;
}

static int test_prepared_input(TestChildState *state,
                               size_t call,
                               const HWAInferenceRequest *request,
                               char *error,
                               size_t error_size)
{
    const HWAInferenceInput *input = &request->inputs[0];
    unsigned char header[44];
    unsigned char encoded[4];
    char actual_sha256[HWA_SHA256_HEX_SIZE];
    uint32_t bits;
    float centre;
    if (strcmp(request->task, HWA_BASIC_PITCH_TASK_NAME) != 0 ||
        request->input_count != 1U ||
        strcmp(input->id, "prepared-source") != 0 ||
        strcmp(input->bytes.name, "basic-pitch-mono-22050.wav") != 0 ||
        request->source_format.container != HWA_CONTAINER_RIFF ||
        request->source_format.encoding != HWA_ENCODING_IEEE_FLOAT ||
        request->source_format.channels != UINT16_C(1) ||
        request->source_format.sample_rate_hz != UINT32_C(22050) ||
        request->source_format.bits_per_sample != UINT16_C(32) ||
        request->source_format.valid_bits_per_sample != UINT16_C(32) ||
        request->source_format.block_align != UINT16_C(4) ||
        request->source_format.frames != TEST_PREPARED_FRAMES ||
        input->bytes.size != UINT64_C(44) +
                                 TEST_PREPARED_FRAMES * UINT64_C(4)) {
        test_error(error, error_size, "wrong prepared Basic Pitch input");
        return -1;
    }
    if (input->bytes.read_at(input->bytes.context, 0U,
                             header, sizeof(header)) != 0 ||
        memcmp(header, "RIFF", 4U) != 0 ||
        memcmp(header + 8U, "WAVEfmt ", 8U) != 0 ||
        test_get_le16(header + 20U) != UINT16_C(3) ||
        test_get_le16(header + 22U) != UINT16_C(1) ||
        test_get_le32(header + 24U) != UINT32_C(22050) ||
        test_get_le16(header + 32U) != UINT16_C(4) ||
        test_get_le16(header + 34U) != UINT16_C(32) ||
        memcmp(header + 36U, "data", 4U) != 0 ||
        test_get_le32(header + 40U) !=
            (uint32_t)(TEST_PREPARED_FRAMES * UINT64_C(4))) {
        test_error(error, error_size, "wrong prepared WAVE header");
        return -1;
    }
    if (input->bytes.read_at(
            input->bytes.context,
            UINT64_C(44) + UINT64_C(32) * UINT64_C(4),
            encoded, sizeof(encoded)) != 0) {
        test_error(error, error_size, "cannot read prepared centre sample");
        return -1;
    }
    bits = test_get_le32(encoded);
    memcpy(&centre, &bits, sizeof(centre));
    if (!isfinite((double)centre) || fabs((double)centre - 0.5) > 1e-6) {
        test_error(error, error_size, "wrong prepared centre sample");
        return -1;
    }
    if (hwa_inference_byte_source_sha256(
            &input->bytes, input->bytes.size, actual_sha256,
            error, error_size) != 0 ||
        strcmp(actual_sha256, input->sha256) != 0)
        return -1;
    state->prepared_frames[call] = request->source_format.frames;
    state->prepared_bytes[call] = input->bytes.size;
    state->child_timeouts[call] = request->timeout_milliseconds;
    state->child_work_bytes[call] =
        request->output_limits.max_work_bytes;
    state->centre_samples[call] = centre;
    memcpy(state->prepared_sha256[call], actual_sha256,
           HWA_SHA256_HEX_SIZE);
    return 0;
}

static void test_set_child_note(TestChildTask *task,
                                size_t row,
                                uint64_t start_sample,
                                uint64_t end_sample,
                                double pitch)
{
    HWAPerformanceEvent *event = &task->events[row];
    HWAEventValue *value = &task->values[row];
    memset(event, 0, sizeof(*event));
    memset(value, 0, sizeof(*value));
    value->name = "pitch-hz";
    value->kind = HWA_EVENT_VALUE_F64;
    value->basis = HWA_EVENT_INFERENCE;
    value->number = pitch;
    value->unit = "Hz";
    value->score = 0.8;
    value->score_valid = 1;
    value->provider_id = UINT64_C(11);
    value->provider_id_valid = 1;
    value->selected = 1;
    event->id = (uint64_t)row + UINT64_C(1);
    event->kind = "note";
    event->source_recording_id = task->audio.id;
    event->evidence_audio_id = task->audio.id;
    event->evidence_audio_id_valid = 1;
    event->start_sample = start_sample;
    event->end_sample = end_sample;
    event->voice = "";
    event->part = "";
    event->score_event_id = "";
    event->values = value;
    event->value_count = 1U;
}

static void test_delay(uint64_t milliseconds)
{
    uint64_t started = 0U;
    char ignored[HWA_ERROR_SIZE] = {0};
    if (milliseconds == 0U ||
        hwa_inference_deadline_start(
            &started, ignored, sizeof(ignored)) != 0)
        return;
    while (hwa_inference_deadline_check(
               started, milliseconds, ignored, sizeof(ignored)) == 0) {
    }
}

static int test_child_start(void *context,
                            const HWAInferenceRequest *request,
                            void **task_value,
                            char *error,
                            size_t error_size)
{
    TestChildState *state = (TestChildState *)context;
    HWAInferenceProvider descriptor;
    TestChildTask *task;
    size_t call;
    if (task_value != NULL) *task_value = NULL;
    test_child_descriptor(state, &descriptor);
    if (state == NULL || task_value == NULL || state->starts >= 4U ||
        hwa_inference_request_validate(
            &descriptor, request, error, error_size) != 0)
        return -1;
    call = state->starts;
    if (test_prepared_input(
            state, call, request, error, error_size) != 0)
        return -1;
    state->starts++;
    if (state->return_null_task) return 0;
    task = (TestChildTask *)calloc(1U, sizeof(*task));
    if (task == NULL) {
        test_error(error, error_size, "cannot allocate fake child task");
        return -1;
    }
    task->provider.id = UINT64_C(11);
    task->provider.name = HWA_BASIC_PITCH_PROVIDER_NAME;
    task->provider.version = HWA_BASIC_PITCH_PROVIDER_VERSION;
    memcpy(task->provider.model_sha256, test_model_sha256,
           HWA_SHA256_HEX_SIZE);
    task->provider.settings_json = "{\"child\":\"fixture\"}";
    task->audio.id = request->source_recording_id;
    task->audio.kind = HWA_EVENT_SOURCE_RECORDING;
    task->audio.name = (char *)request->inputs[0].bytes.name;
    task->audio.relative_path = "";
    task->audio.path_hint = "";
    memcpy(task->audio.sha256, request->inputs[0].sha256,
           HWA_SHA256_HEX_SIZE);
    task->audio.file_bytes = request->inputs[0].bytes.size;
    task->audio.format = request->source_format;
    test_set_child_note(task, 0U, UINT64_C(1), UINT64_C(3), 440.0);
    test_set_child_note(task, 1U,
                        TEST_PREPARED_FRAMES - UINT64_C(2),
                        TEST_PREPARED_FRAMES, 660.0);
    task->bundle.providers = &task->provider;
    task->bundle.provider_count = 1U;
    task->bundle.audio = &task->audio;
    task->bundle.audio_count = 1U;
    task->bundle.events = task->events;
    task->bundle.event_count = 2U;
    task->output.bundle = &task->bundle;
    *task_value = task;
    test_delay(state->delay_milliseconds);
    return 0;
}

static int test_child_poll(void *context,
                           void *task_value,
                           HWAInferencePollState *state,
                           const HWAInferenceOutput **output,
                           char *error,
                           size_t error_size)
{
    TestChildTask *task = (TestChildTask *)task_value;
    (void)context;
    (void)error;
    (void)error_size;
    if (task == NULL || state == NULL || output == NULL) return -1;
    *state = HWA_INFERENCE_READY;
    *output = &task->output;
    return 0;
}

static void test_child_task_free(void *context, void *task)
{
    TestChildState *state = (TestChildState *)context;
    if (state != NULL) state->frees++;
    free(task);
}

static void test_child_destroy(void *context)
{
    TestChildState *state = (TestChildState *)context;
    if (state != NULL) state->destroys++;
}

static int test_case_hash_source(TestCase *test,
                                 char *error,
                                 size_t error_size)
{
    if (hwa_inference_byte_source_sha256(
            &test->input.bytes, (uint64_t)test->wave.bytes.size,
            test->source_sha256, error, error_size) != 0)
        return -1;
    test->input.sha256 = test->source_sha256;
    return 0;
}

static int test_case_init(TestCase *test, uint64_t adapter_work_bytes)
{
    char error[HWA_ERROR_SIZE] = {0};
    memset(test, 0, sizeof(*test));
    if (test_wave_build(&test->wave) != 0) {
        CHECK(0, "cannot build raw-audio WAVE fixture");
        return 0;
    }
    test->input.id = "source";
    test->input.role = "source-recording";
    test->input.media_type = "audio/wav";
    test->input.bytes.context = &test->wave.bytes;
    test->input.bytes.name = "raw-stem.wav";
    test->input.bytes.size = (uint64_t)test->wave.bytes.size;
    test->input.bytes.read_at = test_read_at;
    if (test_case_hash_source(test, error, sizeof(error)) != 0) {
        CHECK(0, "cannot hash raw-audio WAVE fixture: %s", error);
        free(test->wave.bytes.data);
        return 0;
    }
    test_child_descriptor(&test->child_state, &test->child);
    test->child_open = 1;
    if (hwa_basic_pitch_audio_provider_init(
            &test->provider, &test->child, test_adapter_sha256,
            adapter_work_bytes, error, sizeof(error)) != 0) {
        CHECK(0, "cannot initialize raw-audio provider: %s", error);
        hwa_inference_provider_destroy(&test->child);
        test->child_open = 0;
        free(test->wave.bytes.data);
        return 0;
    }
    test->provider_open = 1;
    memset(&test->request, 0, sizeof(test->request));
    hwa_event_bundle_limits_default(&test->request.output_limits);
    test->request.output_limits.max_audio_files = 4U;
    test->request.output_limits.max_events = 8U;
    test->request.output_limits.max_values = 8U;
    test->request.output_limits.max_providers = 4U;
    test->request.output_limits.max_work_bytes = TEST_MAX_WORK_BYTES;
    test->request.task = HWA_BASIC_PITCH_AUDIO_TASK_NAME;
    test->request.settings_json = "{}";
    test->request.expected_provider_name = test->provider.name;
    test->request.expected_provider_version = test->provider.version;
    test->request.expected_model_sha256 = test->provider.model_sha256;
    test->request.seed = UINT64_C(42);
    test->request.source_recording_id = UINT64_C(7);
    test->request.source_input_id = test->input.id;
    test->request.inputs = &test->input;
    test->request.input_count = 1U;
    test->request.source_format = test->wave.format;
    test->request.max_input_file_bytes =
        (uint64_t)test->wave.bytes.size;
    test->request.max_input_bytes = (uint64_t)test->wave.bytes.size;
    test->request.timeout_milliseconds = UINT64_C(900);
    return 1;
}

static void test_case_cleanup(TestCase *test)
{
    if (test->provider_open) {
        hwa_inference_provider_destroy(&test->provider);
        test->provider_open = 0;
    }
    if (test->child_open) {
        hwa_inference_provider_destroy(&test->child);
        test->child_open = 0;
    }
    free(test->wave.bytes.data);
    test->wave.bytes.data = NULL;
}

static int test_start_and_poll(TestCase *test,
                               void **task,
                               const HWAInferenceOutput **output,
                               char error[HWA_ERROR_SIZE])
{
    HWAInferencePollState state = HWA_INFERENCE_PENDING;
    if (task != NULL) *task = NULL;
    if (output != NULL) *output = NULL;
    if (test->provider.start(
            test->provider.context, &test->request, task,
            error, HWA_ERROR_SIZE) != 0)
        return -1;
    if (test->provider.poll(
            test->provider.context, *task, &state, output,
            error, HWA_ERROR_SIZE) != 0)
        return -1;
    if (state != HWA_INFERENCE_READY || output == NULL || *output == NULL) {
        test_error(error, HWA_ERROR_SIZE,
                   "raw-audio provider did not become ready");
        return -1;
    }
    return 0;
}

static char *test_copy_text(const char *text)
{
    size_t size = strlen(text);
    char *copy = (char *)malloc(size + 1U);
    if (copy != NULL) memcpy(copy, text, size + 1U);
    return copy;
}

static void test_valid_mapping_provenance_and_reuse(void)
{
    TestCase test;
    const HWAInferenceOutput *output = NULL;
    const HWAEventBundle *bundle;
    const char *settings;
    char *first_settings = NULL;
    void *task = NULL;
    char error[HWA_ERROR_SIZE] = {0};
    if (!test_case_init(&test, TEST_MAX_WORK_BYTES)) return;
    CHECK(test_start_and_poll(&test, &task, &output, error) == 0,
          "valid raw-audio run failed: %s", error);
    bundle = output == NULL ? NULL : output->bundle;
    CHECK(bundle != NULL && bundle->provider_count == 1U &&
              bundle->audio_count == 1U && bundle->event_count == 2U &&
              output->payload_count == 0U,
          "raw-audio output has the wrong shape");
    if (bundle != NULL && bundle->provider_count == 1U &&
        bundle->event_count == 2U) {
        settings = bundle->providers[0].settings_json;
        CHECK(bundle->providers[0].id == UINT64_C(1) &&
                  strcmp(bundle->providers[0].name,
                         HWA_BASIC_PITCH_AUDIO_PROVIDER_NAME) == 0 &&
                  strcmp(bundle->providers[0].version,
                         HWA_BASIC_PITCH_AUDIO_PROVIDER_VERSION) == 0 &&
                  strcmp(bundle->providers[0].model_sha256,
                         test_model_sha256) == 0,
              "raw-audio wrapper identity is wrong");
        CHECK(strstr(settings, HWA_BASIC_PITCH_AUDIO_TASK_NAME) != NULL &&
                  strstr(settings, HWA_BASIC_PITCH_AUDIO_TRANSFORM_NAME) !=
                      NULL &&
                  strstr(settings, test_adapter_sha256) != NULL &&
                  strstr(settings, test.source_sha256) != NULL &&
                  strstr(settings,
                         test.child_state.prepared_sha256[0]) != NULL &&
                  strstr(settings, HWA_BASIC_PITCH_PROVIDER_NAME) != NULL &&
                  strstr(settings, "{\"child\":\"fixture\"}") != NULL,
              "raw-audio wrapper settings omit provenance: %s", settings);
        CHECK(bundle->audio[0].id == UINT64_C(7) &&
                  bundle->audio[0].format.frames == TEST_RAW_FRAMES &&
                  bundle->audio[0].format.sample_rate_hz ==
                      UINT32_C(44100) &&
                  strcmp(bundle->audio[0].sha256,
                         test.source_sha256) == 0,
              "raw source row changed");
        CHECK(bundle->events[0].start_sample == UINT64_C(2) &&
                  bundle->events[0].end_sample == UINT64_C(6) &&
                  bundle->events[0].source_recording_id == UINT64_C(7) &&
                  bundle->events[0].evidence_audio_id == UINT64_C(7) &&
                  strcmp(bundle->events[0].part, "") == 0 &&
                  bundle->events[0].values[0].provider_id == UINT64_C(1),
              "first raw note did not map by two");
        CHECK(bundle->events[1].start_sample == UINT64_C(126) &&
                  bundle->events[1].end_sample == TEST_RAW_FRAMES &&
                  strcmp(bundle->events[1].part, "") == 0 &&
                  bundle->events[1].values[0].provider_id == UINT64_C(1),
              "final raw note did not clip to the source clock");
        first_settings = test_copy_text(settings);
        CHECK(first_settings != NULL,
              "cannot retain first wrapper settings for repeat check");
    }
    CHECK(test.child_state.prepared_frames[0] == TEST_PREPARED_FRAMES &&
              test.child_state.prepared_bytes[0] ==
                  UINT64_C(44) + TEST_PREPARED_FRAMES * UINT64_C(4) &&
              fabs((double)test.child_state.centre_samples[0] - 0.5) < 1e-6,
          "prepared WAVE has the wrong clock, size, or downmix");
    CHECK(test.child_state.child_work_bytes[0] != 0U &&
              test.child_state.child_work_bytes[0] < TEST_MAX_WORK_BYTES,
          "raw-audio wrapper did not reserve merge work from its child");
    if (task != NULL) {
        test.provider.task_free(test.provider.context, task);
        task = NULL;
    }
    output = NULL;
    error[0] = '\0';
    CHECK(test_start_and_poll(&test, &task, &output, error) == 0,
          "second raw-audio run failed: %s", error);
    CHECK(output != NULL && first_settings != NULL &&
              strcmp(first_settings,
                     output->bundle->providers[0].settings_json) == 0 &&
              strcmp(test.child_state.prepared_sha256[0],
                     test.child_state.prepared_sha256[1]) == 0 &&
              test.child_state.starts == 2U &&
              test.child_state.frees == 2U,
          "raw-audio provider reuse changed output or ownership");
    if (task != NULL) test.provider.task_free(test.provider.context, task);
    free(first_settings);
    hwa_inference_provider_destroy(&test.provider);
    test.provider_open = 0;
    CHECK(test.child_state.destroys == 0U,
          "wrapper destroyed its borrowed Basic Pitch provider");
    hwa_inference_provider_destroy(&test.child);
    test.child_open = 0;
    CHECK(test.child_state.destroys == 1U,
          "caller could not destroy the borrowed provider once");
    test_case_cleanup(&test);
}

static void test_non_finite_input_is_rejected(void)
{
    TestCase test;
    void *task = NULL;
    char error[HWA_ERROR_SIZE] = {0};
    if (!test_case_init(&test, TEST_MAX_WORK_BYTES)) return;
    test_le32(test.wave.bytes.data + 44U, UINT32_C(0x7fc00000));
    CHECK(test_case_hash_source(&test, error, sizeof(error)) == 0,
          "cannot rehash non-finite WAVE: %s", error);
    error[0] = '\0';
    CHECK(test.provider.start(
              test.provider.context, &test.request, &task,
              error, sizeof(error)) != 0 && task == NULL &&
              test.child_state.starts == 0U &&
              strstr(error, "non-finite") != NULL,
          "non-finite raw sample reached Basic Pitch: %s", error);
    test_case_cleanup(&test);
}

static void test_work_limit_stops_before_child(void)
{
    TestCase test;
    void *task = NULL;
    char error[HWA_ERROR_SIZE] = {0};
    if (!test_case_init(&test, UINT64_C(128))) return;
    CHECK(test.provider.start(
              test.provider.context, &test.request, &task,
              error, sizeof(error)) != 0 && task == NULL &&
              test.child_state.starts == 0U &&
              strstr(error, "work limit") != NULL,
          "raw-audio work limit did not stop preparation: %s", error);
    test_case_cleanup(&test);
}

static void test_deadline_releases_late_child(void)
{
    TestCase test;
    void *task = NULL;
    char error[HWA_ERROR_SIZE] = {0};
    if (!test_case_init(&test, TEST_MAX_WORK_BYTES)) return;
    test.child_state.delay_milliseconds = UINT64_C(150);
    test.request.timeout_milliseconds = UINT64_C(100);
    CHECK(test.provider.start(
              test.provider.context, &test.request, &task,
              error, sizeof(error)) != 0 && task == NULL &&
              test.child_state.starts == 1U &&
              test.child_state.frees == 1U &&
              strstr(error, "deadline") != NULL,
          "late Basic Pitch child was not rejected and freed: %s", error);
    test_case_cleanup(&test);
}

static void test_null_child_task_fails_closed(void)
{
    TestCase test;
    void *task = NULL;
    char error[HWA_ERROR_SIZE] = {0};
    if (!test_case_init(&test, TEST_MAX_WORK_BYTES)) return;
    test.child_state.return_null_task = 1;
    CHECK(test.provider.start(
              test.provider.context, &test.request, &task,
              error, sizeof(error)) != 0 && task == NULL &&
              test.child_state.starts == 1U &&
              test.child_state.frees == 0U &&
              strstr(error, "no task") != NULL,
          "null Basic Pitch child task did not fail closed: %s", error);
    test_case_cleanup(&test);
}

int main(void)
{
    test_valid_mapping_provenance_and_reuse();
    test_non_finite_input_is_rejected();
    test_work_limit_stops_before_child();
    test_deadline_releases_late_child();
    test_null_child_task_fails_closed();
    if (failures != 0) {
        (void)fprintf(stderr,
                      "%d Basic Pitch raw-audio provider test(s) failed\n",
                      failures);
        return 1;
    }
    (void)puts("Basic Pitch raw-audio provider tests passed");
    return 0;
}

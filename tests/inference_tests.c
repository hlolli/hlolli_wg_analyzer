#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "inference_provider.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#define TEST_MKDIR(path) _mkdir(path)
#define TEST_PID() ((long)_getpid())
#define TEST_RMDIR(path) _rmdir(path)
#define TEST_UNLINK(path) _unlink(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define TEST_MKDIR(path) mkdir(path, 0700)
#define TEST_PID() ((long)getpid())
#define TEST_RMDIR(path) rmdir(path)
#define TEST_UNLINK(path) unlink(path)
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int failures;

#define CHECK(condition, ...)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "FAIL: ");                                \
            (void)fprintf(stderr, __VA_ARGS__);                              \
            (void)fputc('\n', stderr);                                       \
            failures++;                                                      \
        }                                                                    \
    } while (0)

typedef struct TestBytes {
    const unsigned char *data;
    size_t size;
} TestBytes;

typedef struct TestChangingBytes {
    const unsigned char *first_data;
    const unsigned char *later_data;
    size_t size;
    size_t read_count;
    size_t fail_at_read;
} TestChangingBytes;

static const unsigned char test_source_data[428] = {
    0x52U, 0x49U, 0x46U, 0x46U, 0xa4U, 0x01U, 0x00U, 0x00U,
    0x57U, 0x41U, 0x56U, 0x45U, 0x66U, 0x6dU, 0x74U, 0x20U,
    0x10U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x01U, 0x00U,
    0x80U, 0xbbU, 0x00U, 0x00U, 0x00U, 0x77U, 0x01U, 0x00U,
    0x02U, 0x00U, 0x10U, 0x00U, 0x64U, 0x61U, 0x74U, 0x61U,
    0x80U, 0x01U, 0x00U, 0x00U
};
static const char test_source_sha256[] =
    "5f46074b7841761c7897091cf1a6e0c1"
    "8826560961d2cef360518b719a8fef21";
static const unsigned char test_trace_data[8] = {
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xf0U, 0x3fU
};
static const char test_trace_sha256[] =
    "6c3c396ed6b5c36dcae172271f462051"
    "b1266b851e92df3deea8ac65478fd712";

static int test_read_at(void *context,
                        uint64_t offset,
                        unsigned char *destination,
                        size_t size)
{
    const TestBytes *bytes = (const TestBytes *)context;
    if (bytes == NULL || destination == NULL ||
        offset > (uint64_t)bytes->size ||
        (uint64_t)size > (uint64_t)bytes->size - offset) return -1;
    memcpy(destination, bytes->data + (size_t)offset, size);
    return 0;
}

static int test_changing_read_at(void *context,
                                 uint64_t offset,
                                 unsigned char *destination,
                                 size_t size)
{
    TestChangingBytes *bytes = (TestChangingBytes *)context;
    const unsigned char *data;
    if (bytes == NULL || destination == NULL ||
        offset > (uint64_t)bytes->size ||
        (uint64_t)size > (uint64_t)bytes->size - offset) return -1;
    bytes->read_count++;
    if (bytes->fail_at_read != 0U &&
        bytes->read_count == bytes->fail_at_read) return -1;
    data = bytes->read_count == 1U || bytes->later_data == NULL
               ? bytes->first_data
               : bytes->later_data;
    memcpy(destination, data + (size_t)offset, size);
    return 0;
}

static int test_make_unused_output_path(char path[PATH_MAX], const char *name)
{
    unsigned attempt;
#if defined(_WIN32)
    const char *temporary = getenv("TEMP");
    if (temporary == NULL || temporary[0] == '\0') temporary = ".";
#else
    const char *temporary = "/tmp";
#endif
    for (attempt = 0U; attempt < 100U; ++attempt) {
        int written = snprintf(path, PATH_MAX,
                               "%s/hwa-inference-%s-%ld-%u.hwa-events",
                               temporary, name, TEST_PID(), attempt);
        if (written < 0 || (size_t)written >= PATH_MAX) return 0;
        if (TEST_MKDIR(path) == 0) {
            if (TEST_RMDIR(path) != 0) return 0;
            return 1;
        }
        if (errno != EEXIST) return 0;
    }
    return 0;
}

static int test_path_exists(const char *path)
{
#if defined(_WIN32)
    struct _stat64 status;
    return _stat64(path, &status) == 0;
#else
    struct stat status;
    return stat(path, &status) == 0;
#endif
}

static void test_remove_failed_output(const char *directory)
{
    static const char *const files[] = {
        "audio/source.wav", "events.jsonl", "traces.jsonl", "manifest.json"
    };
    char path[PATH_MAX];
    size_t index;
    for (index = 0U; index < sizeof(files) / sizeof(files[0]); ++index) {
        int written = snprintf(path, sizeof(path), "%s/%s",
                               directory, files[index]);
        if (written > 0 && (size_t)written < sizeof(path))
            (void)TEST_UNLINK(path);
    }
    if (snprintf(path, sizeof(path), "%s/audio", directory) > 0)
        (void)TEST_RMDIR(path);
    (void)TEST_RMDIR(directory);
}

static void test_request_init(HWAInferenceRequest *request,
                              TestBytes *bytes,
                              HWAInferenceInput inputs[2])
{
    memset(bytes, 0, sizeof(*bytes));
    memset(request, 0, sizeof(*request));
    memset(inputs, 0, 2U * sizeof(*inputs));
    hwa_event_bundle_limits_default(&request->output_limits);
    bytes->data = test_source_data;
    bytes->size = sizeof(test_source_data);
    request->task = "org.hlolli.fixed-note";
    request->settings_json = "{}";
    request->expected_provider_name = "org.hlolli.fixed-inference";
    request->expected_provider_version = "1";
    request->expected_model_sha256 = "";
    request->seed = UINT64_C(0);
    request->source_recording_id = UINT64_C(7);
    request->source_input_id = "recording";
    request->inputs = inputs;
    request->input_count = 1U;
    inputs[0].id = "recording";
    inputs[0].role = "source-recording";
    inputs[0].media_type = "audio/wav";
    inputs[0].sha256 = test_source_sha256;
    inputs[0].bytes.context = bytes;
    inputs[0].bytes.name = "fixed source.wav";
    inputs[0].bytes.size = (uint64_t)sizeof(test_source_data);
    inputs[0].bytes.read_at = test_read_at;
    request->source_format.container = HWA_CONTAINER_RIFF;
    request->source_format.encoding = HWA_ENCODING_PCM;
    request->source_format.channels = 1U;
    request->source_format.sample_rate_hz = 48000U;
    request->source_format.bits_per_sample = 16U;
    request->source_format.valid_bits_per_sample = 16U;
    request->source_format.block_align = 2U;
    request->source_format.frames = UINT64_C(192);
    request->source_format.data_bytes = UINT64_C(384);
    request->source_format.duration_seconds = 0.004;
    request->max_input_file_bytes = UINT64_C(1048576);
    request->max_input_bytes = UINT64_C(2097152);
    request->timeout_milliseconds = UINT64_C(1000);
}

static int test_provider_init(HWAInferenceProvider *provider)
{
    memset(provider, 0, sizeof(*provider));
    hwa_inference_fixed_provider_init(provider);
    if (provider->name == NULL || provider->version == NULL ||
        provider->model_sha256 == NULL || provider->start == NULL ||
        provider->poll == NULL || provider->destroy == NULL ||
        provider->task_free == NULL) {
        CHECK(0, "fixed provider did not fill its descriptor and callbacks");
        return 0;
    }
    return 1;
}

static void test_request_validator_checks_shape_limits_and_hashes(void)
{
    static const char wrong_sha256[] =
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    TestBytes bytes;
    HWAInferenceInput inputs[2];
    HWAInferenceRequest request;
    HWAInferenceProvider provider;
    unsigned char corrupt_source[sizeof(test_source_data)];
    char corrupt_sha256[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE] = {0};

    test_request_init(&request, &bytes, inputs);
    if (!test_provider_init(&provider)) return;
    CHECK(hwa_inference_request_validate(&provider, &request,
                                         error, sizeof(error)) == 0,
          "valid inference request failed validation: %s", error);

    request.timeout_milliseconds = 0U;
    CHECK(hwa_inference_request_validate(&provider, &request,
                                         error, sizeof(error)) != 0,
          "request validator accepted a zero timeout");
    request.timeout_milliseconds = UINT64_C(1000);

    request.max_input_file_bytes =
        (uint64_t)sizeof(test_source_data) - 1U;
    CHECK(hwa_inference_request_validate(&provider, &request,
                                         error, sizeof(error)) != 0,
          "request validator ignored its per-file input cap");
    request.max_input_file_bytes = (uint64_t)sizeof(test_source_data);

    request.source_format.channel_mask = 1U;
    CHECK(hwa_inference_request_validate(&provider, &request,
                                         error, sizeof(error)) != 0,
          "request validator ignored a WAVE format mismatch");
    CHECK(strstr(error, "format") != NULL,
          "WAVE format mismatch gave the wrong error: %s", error);
    request.source_format.channel_mask = 0U;

    memcpy(corrupt_source, test_source_data, sizeof(corrupt_source));
    corrupt_source[0] = (unsigned char)'X';
    bytes.data = corrupt_source;
    CHECK(hwa_inference_byte_source_sha256(
              &inputs[0].bytes, request.max_input_file_bytes,
              corrupt_sha256, error, sizeof(error)) == 0,
          "cannot hash corrupt WAVE fixture: %s", error);
    inputs[0].sha256 = corrupt_sha256;
    CHECK(hwa_inference_request_validate(&provider, &request,
                                         error, sizeof(error)) != 0,
          "request validator accepted a corrupt WAVE with a correct hash");
    CHECK(strstr(error, "WAVE") != NULL,
          "corrupt WAVE gave the wrong error: %s", error);
    bytes.data = test_source_data;
    inputs[0].sha256 = test_source_sha256;

    inputs[1] = inputs[0];
    inputs[1].id = "score";
    inputs[1].role = "score-context";
    inputs[1].media_type = "application/vnd.recordare.musicxml+xml";
    inputs[1].bytes.name = "context.musicxml";
    request.input_count = 2U;
    request.max_input_bytes =
        (uint64_t)(2U * sizeof(test_source_data)) - 1U;
    CHECK(hwa_inference_request_validate(&provider, &request,
                                         error, sizeof(error)) != 0,
          "request validator ignored its total input cap");

    request.max_input_bytes = (uint64_t)(2U * sizeof(test_source_data));
    inputs[1].sha256 = wrong_sha256;
    CHECK(hwa_inference_request_validate(&provider, &request,
                                         error, sizeof(error)) != 0,
          "request validator ignored a context input hash mismatch");
    CHECK(strstr(error, "hash") != NULL,
          "input hash mismatch gave the wrong error: %s", error);
    inputs[1].sha256 = test_source_sha256;

    inputs[1].role = "source-recording";
    inputs[1].media_type = "audio/wav";
    CHECK(hwa_inference_request_validate(&provider, &request,
                                         error, sizeof(error)) != 0,
          "request validator accepted a second source recording");
    CHECK(strstr(error, "source") != NULL,
          "second source recording gave the wrong error: %s", error);

    hwa_inference_provider_destroy(&provider);
}

static void test_request_validator_checks_settings_format_and_callbacks(void)
{
    TestBytes bytes;
    HWAInferenceInput inputs[2];
    HWAInferenceRequest request;
    HWAInferenceProvider provider;
    HWAInferencePollFunction poll;
    char invalid_utf8[] = {'b', 'a', 'd', (char)0xff, '\0'};
    char error[HWA_ERROR_SIZE] = {0};

    test_request_init(&request, &bytes, inputs);
    if (!test_provider_init(&provider)) return;
    request.settings_json =
        " {\"enabled\":true,\"values\":[null,-1.25e+2,\"ok\"]} ";
    CHECK(hwa_inference_request_validate(&provider, &request,
                                         error, sizeof(error)) == 0,
          "request validator rejected a valid settings object: %s", error);
    request.settings_json = "{\"enabled\":}";
    CHECK(hwa_inference_request_validate(&provider, &request,
                                         error, sizeof(error)) != 0,
          "request validator accepted malformed settings JSON");
    request.settings_json = "[]";
    CHECK(hwa_inference_request_validate(&provider, &request,
                                         error, sizeof(error)) != 0,
          "request validator accepted non-object settings JSON");
    request.settings_json = "{\"value\":\"\xff\"}";
    CHECK(hwa_inference_request_validate(&provider, &request,
                                         error, sizeof(error)) != 0,
          "request validator accepted invalid UTF-8 settings");
    request.settings_json = "{\"value\":\"\\u0000\"}";
    CHECK(hwa_inference_request_validate(&provider, &request,
                                         error, sizeof(error)) != 0,
          "request validator accepted a null Unicode escape");
    request.settings_json = "{\"value\":\"\\ud800\"}";
    CHECK(hwa_inference_request_validate(&provider, &request,
                                         error, sizeof(error)) != 0,
          "request validator accepted a lone high surrogate");
    request.settings_json = "{\"value\":\"\\udc00\"}";
    CHECK(hwa_inference_request_validate(&provider, &request,
                                         error, sizeof(error)) != 0,
          "request validator accepted a lone low surrogate");
    request.settings_json = "{\"value\":\"\\ud83d\\ude00\"}";
    CHECK(hwa_inference_request_validate(&provider, &request,
                                         error, sizeof(error)) == 0,
          "request validator rejected a surrogate pair: %s", error);

    request.settings_json = "{}";
    inputs[0].bytes.name = invalid_utf8;
    CHECK(hwa_inference_request_validate(&provider, &request,
                                         error, sizeof(error)) != 0,
          "request validator accepted an invalid UTF-8 input name");
    inputs[0].bytes.name = "fixed source.wav";

    provider.name = invalid_utf8;
    request.expected_provider_name = invalid_utf8;
    CHECK(hwa_inference_request_validate(&provider, &request,
                                         error, sizeof(error)) != 0,
          "request validator accepted an invalid UTF-8 provider name");
    provider.name = "org.hlolli.fixed-inference";
    request.expected_provider_name = "org.hlolli.fixed-inference";

    request.source_format.duration_seconds = 0.005;
    CHECK(hwa_inference_request_validate(&provider, &request,
                                         error, sizeof(error)) != 0,
          "request validator accepted inconsistent source duration");
    request.source_format.duration_seconds = 0.004;

    poll = provider.poll;
    provider.poll = NULL;
    CHECK(hwa_inference_request_validate(&provider, &request,
                                         error, sizeof(error)) != 0,
          "request validator accepted an incomplete provider descriptor");
    provider.poll = poll;
    hwa_inference_provider_destroy(&provider);
}

static void test_output_validator_binds_request_identity(void)
{
    TestBytes bytes;
    HWAInferenceInput inputs[2];
    HWAInferenceRequest request;
    HWAInferenceProvider provider;
    HWAInferencePollState state = HWA_INFERENCE_PENDING;
    const HWAInferenceOutput *ready = NULL;
    HWAInferenceOutput output;
    HWAEventBundle bundle;
    HWAEventProvider providers[2];
    HWAEventAudio audio[2];
    HWAPerformanceEvent event;
    HWAEventValue value;
    void *task = NULL;
    char error[HWA_ERROR_SIZE] = {0};

    test_request_init(&request, &bytes, inputs);
    if (!test_provider_init(&provider)) return;
    CHECK(provider.start(provider.context, &request, &task,
                         error, sizeof(error)) == 0 && task != NULL,
          "could not start request-binding fixture: %s", error);
    if (task == NULL) goto cleanup;
    CHECK(provider.poll(provider.context, task, &state, &ready,
                        error, sizeof(error)) == 0 &&
              state == HWA_INFERENCE_PENDING && ready == NULL,
          "request-binding fixture did not report pending");
    CHECK(provider.poll(provider.context, task, &state, &ready,
                        error, sizeof(error)) == 0 &&
              state == HWA_INFERENCE_READY && ready != NULL,
          "request-binding fixture did not report ready: %s", error);
    if (ready == NULL || ready->bundle == NULL) goto cleanup;
    CHECK(hwa_inference_output_validate_for_request(
              &provider, &request, ready, error, sizeof(error)) == 0,
          "valid request-bound output failed validation: %s", error);

    output = *ready;
    bundle = *ready->bundle;
    output.bundle = &bundle;
    audio[0] = ready->bundle->audio[0];
    audio[0].name = (char *)"another.wav";
    bundle.audio = audio;
    CHECK(hwa_inference_output_validate_for_request(
              &provider, &request, &output, error, sizeof(error)) != 0,
          "request-bound validator accepted another source name");

    bundle = *ready->bundle;
    audio[0] = ready->bundle->audio[0];
    audio[0].format.channel_mask = 1U;
    bundle.audio = audio;
    CHECK(hwa_inference_output_validate_for_request(
              &provider, &request, &output, error, sizeof(error)) != 0,
          "request-bound validator ignored a source format field");

    bundle = *ready->bundle;
    audio[0] = ready->bundle->audio[0];
    audio[1] = ready->bundle->audio[0];
    audio[1].id = UINT64_C(8);
    audio[1].name = (char *)"second.wav";
    bundle.audio = audio;
    bundle.audio_count = 2U;
    CHECK(hwa_inference_output_validate_for_request(
              &provider, &request, &output, error, sizeof(error)) != 0,
          "request-bound validator accepted a second source clock");

    bundle = *ready->bundle;
    providers[0] = ready->bundle->providers[0];
    providers[1] = ready->bundle->providers[0];
    providers[1].id = UINT64_C(2);
    providers[1].name = (char *)"org.hlolli.other-inference";
    bundle.providers = providers;
    bundle.provider_count = 2U;
    event = ready->bundle->events[0];
    value = ready->bundle->events[0].values[0];
    value.provider_id = UINT64_C(2);
    event.values = &value;
    bundle.events = &event;
    CHECK(hwa_inference_output_validate_for_request(
              &provider, &request, &output, error, sizeof(error)) != 0,
          "request-bound validator accepted another inference provider");

    providers[1].name = providers[0].name;
    value.provider_id = UINT64_C(1);
    CHECK(hwa_inference_output_validate_for_request(
              &provider, &request, &output, error, sizeof(error)) != 0,
          "request-bound validator accepted duplicate provider identity");

cleanup:
    provider.task_free(provider.context, task);
    hwa_inference_provider_destroy(&provider);
}

static void test_fixed_provider_returns_one_exact_note(void)
{
    TestBytes bytes;
    HWAInferenceInput inputs[2];
    HWAInferenceRequest request;
    HWAInferenceProvider provider;
    HWAInferencePollState state = HWA_INFERENCE_PENDING;
    const HWAInferenceOutput *output = NULL;
    const HWAInferenceOutput *ready_output;
    const HWAEventBundle *bundle;
    const HWAPerformanceEvent *event;
    const HWAEventValue *value;
    void *task = (void *)(uintptr_t)1U;
    char error[HWA_ERROR_SIZE] = {0};

    test_request_init(&request, &bytes, inputs);
    if (!test_provider_init(&provider)) return;
    CHECK(strcmp(provider.name, "org.hlolli.fixed-inference") == 0 &&
              strcmp(provider.version, "1") == 0 &&
              strcmp(provider.model_sha256, "") == 0,
          "fixed provider descriptor is wrong");
    CHECK(provider.start(provider.context, &request, &task,
                         error, sizeof(error)) == 0,
          "fixed provider start failed: %s", error);
    CHECK(task != NULL, "fixed provider returned no task");
    if (task == NULL) goto cleanup;
    output = (const HWAInferenceOutput *)(uintptr_t)1U;
    CHECK(provider.poll(provider.context, task, &state, &output,
                        error, sizeof(error)) == 0,
          "fixed provider first poll failed: %s", error);
    CHECK(state == HWA_INFERENCE_PENDING && output == NULL,
          "fixed provider did not report pending first");
    CHECK(provider.poll(provider.context, task, &state, &output,
                        error, sizeof(error)) == 0,
          "fixed provider ready poll failed: %s", error);
    CHECK(state == HWA_INFERENCE_READY && output != NULL,
          "fixed provider did not complete after its pending poll");
    if (output == NULL) goto cleanup;
    ready_output = output;
    state = HWA_INFERENCE_PENDING;
    output = (const HWAInferenceOutput *)(uintptr_t)1U;
    CHECK(provider.poll(provider.context, task, &state, &output,
                        error, sizeof(error)) == 0,
          "fixed provider repeated ready poll failed: %s", error);
    CHECK(state == HWA_INFERENCE_READY && output == ready_output,
          "fixed provider did not keep the same ready output");
    if (output != ready_output) goto cleanup;

    CHECK(output->payload_count == 0U && output->payloads == NULL,
          "fixed provider returned an unexpected payload");
    bundle = output->bundle;
    CHECK(bundle != NULL, "fixed provider returned no event bundle");
    if (bundle == NULL) goto cleanup;
    CHECK(hwa_inference_output_validate_for_request(
              &provider, &request, output, error, sizeof(error)) == 0,
          "fixed provider returned an invalid request-bound output: %s",
          error);
    CHECK(bundle->provider_count == 1U && bundle->audio_count == 1U &&
              bundle->event_count == 1U && bundle->trace_count == 0U &&
              bundle->warning_count == 0U,
          "fixed provider returned the wrong bundle shape");
    if (bundle->provider_count != 1U || bundle->audio_count != 1U ||
        bundle->event_count != 1U) goto cleanup;

    CHECK(bundle->providers[0].id == UINT64_C(1) &&
              strcmp(bundle->providers[0].name,
                     "org.hlolli.fixed-inference") == 0 &&
              strcmp(bundle->providers[0].version, "1") == 0 &&
              bundle->providers[0].model_sha256[0] == '\0' &&
              strcmp(bundle->providers[0].settings_json, "{}") == 0,
          "fixed provider identity is wrong");
    CHECK(bundle->audio[0].id == UINT64_C(7) &&
              bundle->audio[0].kind == HWA_EVENT_SOURCE_RECORDING &&
              strcmp(bundle->audio[0].name, "fixed source.wav") == 0 &&
              strcmp(bundle->audio[0].relative_path, "") == 0 &&
              strcmp(bundle->audio[0].path_hint, "") == 0 &&
              strcmp(bundle->audio[0].sha256, test_source_sha256) == 0 &&
              bundle->audio[0].file_bytes == sizeof(test_source_data) &&
              bundle->audio[0].format.sample_rate_hz == 48000U &&
              bundle->audio[0].format.frames == UINT64_C(192),
          "fixed provider changed the source recording facts");

    event = &bundle->events[0];
    CHECK(event->id == UINT64_C(1) && strcmp(event->kind, "note") == 0 &&
              event->source_recording_id == UINT64_C(7) &&
              event->evidence_audio_id_valid &&
              event->evidence_audio_id == UINT64_C(7) &&
              event->start_sample == UINT64_C(64) &&
              event->end_sample == UINT64_C(192) &&
              event->value_count == 1U,
          "fixed provider returned the wrong note event");
    if (event->value_count != 1U) goto cleanup;
    value = &event->values[0];
    CHECK(strcmp(value->name, "pitch-hz") == 0 &&
              value->kind == HWA_EVENT_VALUE_F64 &&
              value->basis == HWA_EVENT_INFERENCE &&
              value->number == 440.00000000000006 &&
              strcmp(value->unit, "Hz") == 0 && value->score_valid &&
              value->score == 0.875 && value->provider_id_valid &&
              value->provider_id == UINT64_C(1) && value->selected,
          "fixed provider returned the wrong pitch inference");

cleanup:
    if (provider.task_free != NULL) provider.task_free(provider.context, task);
    hwa_inference_provider_destroy(&provider);
}

static void test_fixed_provider_obeys_output_count_limits(void)
{
    TestBytes bytes;
    HWAInferenceInput inputs[2];
    HWAInferenceRequest request;
    HWAInferenceProvider provider;
    void *task = (void *)(uintptr_t)1U;
    char error[HWA_ERROR_SIZE] = {0};

    test_request_init(&request, &bytes, inputs);
    request.output_limits.max_events = 0U;
    if (!test_provider_init(&provider)) return;
    CHECK(provider.start(provider.context, &request, &task,
                         error, sizeof(error)) != 0,
          "fixed provider ignored the event limit");
    CHECK(task == NULL, "failed fixed provider start returned a task");
    CHECK(strstr(error, "limit") != NULL,
          "fixed provider gave no useful limit error: %s", error);
    provider.task_free(provider.context, task);
    hwa_inference_provider_destroy(&provider);
}

static void test_fixed_provider_rejects_result_outside_bundle_limits(void)
{
    TestBytes bytes;
    HWAInferenceInput inputs[2];
    HWAInferenceRequest request;
    HWAInferenceProvider provider;
    void *task = NULL;
    char error[HWA_ERROR_SIZE] = {0};

    test_request_init(&request, &bytes, inputs);
    request.output_limits.max_payload_file_bytes = 1U;
    if (!test_provider_init(&provider)) return;
    CHECK(provider.start(provider.context, &request, &task,
                         error, sizeof(error)) != 0,
          "fixed provider returned a bundle outside its byte limits");
    CHECK(task == NULL, "invalid fixed provider result returned a task");
    CHECK(error[0] != '\0',
          "invalid fixed provider result gave no error");
    provider.task_free(provider.context, task);
    hwa_inference_provider_destroy(&provider);
}

static void test_request_validator_rejects_zero_output_limits(void)
{
    TestBytes bytes;
    HWAInferenceInput inputs[2];
    HWAInferenceRequest request;
    HWAInferenceProvider provider;
    void *task = NULL;
    char error[HWA_ERROR_SIZE] = {0};

    test_request_init(&request, &bytes, inputs);
    request.output_limits.max_manifest_bytes = 0U;
    request.output_limits.max_index_bytes = 0U;
    request.output_limits.max_bundle_bytes = 0U;
    if (!test_provider_init(&provider)) return;
    CHECK(provider.start(provider.context, &request, &task,
                         error, sizeof(error)) != 0 && task == NULL,
          "fixed provider accepted zero output byte limits");
    CHECK(strstr(error, "limit") != NULL,
          "zero output byte limits gave the wrong error: %s", error);
    provider.task_free(provider.context, task);
    hwa_inference_provider_destroy(&provider);
}

static void test_fixed_provider_accepts_spaced_empty_settings(void)
{
    TestBytes bytes;
    HWAInferenceInput inputs[2];
    HWAInferenceRequest request;
    HWAInferenceProvider provider;
    HWAInferencePollState state = HWA_INFERENCE_READY;
    const HWAInferenceOutput *output = NULL;
    void *task = NULL;
    char error[HWA_ERROR_SIZE] = {0};

    test_request_init(&request, &bytes, inputs);
    request.settings_json = " { \n } \t";
    if (!test_provider_init(&provider)) return;
    CHECK(provider.start(provider.context, &request, &task,
                         error, sizeof(error)) == 0,
          "fixed provider rejected empty JSON settings: %s", error);
    CHECK(task != NULL, "fixed provider returned no settings task");
    if (task == NULL) goto cleanup;
    CHECK(provider.poll(provider.context, task, &state, &output,
                        error, sizeof(error)) == 0 &&
              state == HWA_INFERENCE_PENDING && output == NULL,
          "fixed provider did not report pending for settings: %s", error);
    CHECK(provider.poll(provider.context, task, &state, &output,
                        error, sizeof(error)) == 0 &&
              state == HWA_INFERENCE_READY && output != NULL,
          "fixed provider could not return canonical settings: %s", error);
    if (output != NULL && output->bundle != NULL &&
        output->bundle->provider_count == 1U) {
        CHECK(strcmp(output->bundle->providers[0].settings_json, "{}") == 0,
              "fixed provider did not canonicalize empty settings");
    }
cleanup:
    provider.task_free(provider.context, task);
    hwa_inference_provider_destroy(&provider);
}

static void test_fixed_provider_rejects_other_model_identity(void)
{
    static const char other_model_sha256[] =
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    TestBytes bytes;
    HWAInferenceInput inputs[2];
    HWAInferenceRequest request;
    HWAInferenceProvider provider;
    void *task = NULL;
    char error[HWA_ERROR_SIZE] = {0};

    test_request_init(&request, &bytes, inputs);
    request.expected_model_sha256 = other_model_sha256;
    if (!test_provider_init(&provider)) return;
    CHECK(provider.start(provider.context, &request, &task,
                         error, sizeof(error)) != 0,
          "fixed provider accepted another model identity");
    CHECK(task == NULL, "identity mismatch returned a task");
    CHECK(strstr(error, "identity") != NULL,
          "identity mismatch gave no useful error: %s", error);
    provider.task_free(provider.context, task);
    hwa_inference_provider_destroy(&provider);
}

static void test_fixed_provider_rejects_other_name_and_version(void)
{
    static const char *const names[] = {
        "org.hlolli.other-inference", "org.hlolli.fixed-inference"
    };
    static const char *const versions[] = {"1", "2"};
    static const char *const labels[] = {"name", "version"};
    size_t index;

    for (index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
        TestBytes bytes;
        HWAInferenceInput inputs[2];
        HWAInferenceRequest request;
        HWAInferenceProvider provider;
        void *task = (void *)(uintptr_t)1U;
        char error[HWA_ERROR_SIZE] = {0};

        test_request_init(&request, &bytes, inputs);
        request.expected_provider_name = names[index];
        request.expected_provider_version = versions[index];
        if (!test_provider_init(&provider)) return;
        CHECK(provider.start(provider.context, &request, &task,
                             error, sizeof(error)) != 0,
              "fixed provider accepted another provider %s", labels[index]);
        CHECK(task == NULL, "provider %s mismatch returned a task",
              labels[index]);
        CHECK(strstr(error, "identity") != NULL,
              "provider %s mismatch gave the wrong error: %s",
              labels[index], error);
        provider.task_free(provider.context, task);
        hwa_inference_provider_destroy(&provider);
    }
}

static void test_fixed_provider_rejects_duplicate_input_ids(void)
{
    TestBytes bytes;
    HWAInferenceInput inputs[2];
    HWAInferenceRequest request;
    HWAInferenceProvider provider;
    void *task = (void *)(uintptr_t)1U;
    char error[HWA_ERROR_SIZE] = {0};

    test_request_init(&request, &bytes, inputs);
    inputs[1] = inputs[0];
    inputs[1].bytes.name = "duplicate source.wav";
    request.input_count = 2U;
    if (!test_provider_init(&provider)) return;
    CHECK(provider.start(provider.context, &request, &task,
                         error, sizeof(error)) != 0,
          "fixed provider accepted duplicate input IDs");
    CHECK(task == NULL, "duplicate input IDs returned a task");
    CHECK(strstr(error, "duplicate") != NULL,
          "duplicate input IDs gave the wrong error: %s", error);
    provider.task_free(provider.context, task);
    hwa_inference_provider_destroy(&provider);
}

static void test_fixed_provider_enforces_input_count_bounds(void)
{
    const size_t excessive_count =
        (size_t)HWA_INFERENCE_MAX_INPUTS + 1U;
    TestBytes bytes;
    HWAInferenceInput inputs[2];
    HWAInferenceRequest request;
    HWAInferenceProvider provider;
    HWAInferenceInput *many_inputs = NULL;
    char (*ids)[32] = NULL;
    void *task = (void *)(uintptr_t)1U;
    char error[HWA_ERROR_SIZE] = {0};
    size_t index;

    test_request_init(&request, &bytes, inputs);
    request.input_count = 0U;
    if (!test_provider_init(&provider)) return;
    CHECK(provider.start(provider.context, &request, &task,
                         error, sizeof(error)) != 0,
          "fixed provider accepted zero inputs");
    CHECK(task == NULL, "zero inputs returned a task");
    CHECK(strstr(error, "shape") != NULL,
          "zero inputs gave the wrong error: %s", error);
    provider.task_free(provider.context, task);
    hwa_inference_provider_destroy(&provider);

    many_inputs = (HWAInferenceInput *)calloc(
        excessive_count, sizeof(*many_inputs));
    ids = (char (*)[32])calloc(excessive_count, sizeof(*ids));
    if (many_inputs == NULL || ids == NULL) {
        CHECK(0, "cannot allocate excessive input-count fixture");
        free(ids);
        free(many_inputs);
        return;
    }
    test_request_init(&request, &bytes, inputs);
    for (index = 0U; index < excessive_count; ++index) {
        int written = snprintf(ids[index], sizeof(ids[index]),
                               "input-%zu", index);
        if (written < 0 || (size_t)written >= sizeof(ids[index])) {
            CHECK(0, "cannot format excessive input ID %zu", index);
            free(ids);
            free(many_inputs);
            return;
        }
        many_inputs[index] = inputs[0];
        many_inputs[index].id = ids[index];
        many_inputs[index].role = index == 0U
                                      ? "source-recording" : "context";
        many_inputs[index].media_type = index == 0U
                                            ? "audio/wav"
                                            : "application/octet-stream";
        many_inputs[index].bytes.name = index == 0U
                                            ? "fixed source.wav"
                                            : "context.bin";
    }
    request.inputs = many_inputs;
    request.input_count = excessive_count;
    request.source_input_id = ids[0];
    task = (void *)(uintptr_t)1U;
    error[0] = '\0';
    if (!test_provider_init(&provider)) {
        free(ids);
        free(many_inputs);
        return;
    }
    CHECK(provider.start(provider.context, &request, &task,
                         error, sizeof(error)) != 0,
          "fixed provider accepted %zu inputs", excessive_count);
    CHECK(task == NULL, "%zu inputs returned a task", excessive_count);
    CHECK(strstr(error, "shape") != NULL,
          "%zu inputs gave the wrong error: %s", excessive_count, error);
    provider.task_free(provider.context, task);
    hwa_inference_provider_destroy(&provider);
    free(ids);
    free(many_inputs);
}

static void test_fixed_provider_accepts_named_score_context(void)
{
    TestBytes bytes;
    HWAInferenceInput inputs[2];
    HWAInferenceRequest request;
    HWAInferenceProvider provider;
    void *task = NULL;
    char error[HWA_ERROR_SIZE] = {0};

    test_request_init(&request, &bytes, inputs);
    inputs[1].id = "score";
    inputs[1].role = "score-context";
    inputs[1].media_type = "application/vnd.recordare.musicxml+xml";
    inputs[1].sha256 = test_source_sha256;
    inputs[1].bytes = inputs[0].bytes;
    inputs[1].bytes.name = "context.musicxml";
    request.input_count = 2U;
    if (!test_provider_init(&provider)) return;
    CHECK(provider.start(provider.context, &request, &task,
                         error, sizeof(error)) == 0,
          "fixed provider rejected named score context: %s", error);
    CHECK(task != NULL, "fixed provider returned no task with score context");
    provider.task_free(provider.context, task);
    hwa_inference_provider_destroy(&provider);
}

static void test_failed_poll_returns_no_stale_output(void)
{
    HWAInferenceProvider provider;
    HWAInferencePollState state = HWA_INFERENCE_READY;
    const HWAInferenceOutput *output =
        (const HWAInferenceOutput *)(uintptr_t)1U;
    char error[HWA_ERROR_SIZE] = {0};

    if (!test_provider_init(&provider)) return;
    CHECK(provider.poll(provider.context, NULL, &state, &output,
                        error, sizeof(error)) != 0,
          "fixed provider accepted a null task");
    CHECK(state == HWA_INFERENCE_PENDING && output == NULL,
          "failed poll left stale output visible");
    hwa_inference_provider_destroy(&provider);
}

static void test_fixed_provider_cancels_pending_task(void)
{
    TestBytes bytes;
    HWAInferenceInput inputs[2];
    HWAInferenceRequest request;
    HWAInferenceProvider provider;
    HWAInferencePollState state = HWA_INFERENCE_READY;
    const HWAInferenceOutput *output =
        (const HWAInferenceOutput *)(uintptr_t)1U;
    void *task = NULL;
    char error[HWA_ERROR_SIZE] = {0};

    test_request_init(&request, &bytes, inputs);
    if (!test_provider_init(&provider)) return;
    CHECK(provider.start(provider.context, &request, &task,
                         error, sizeof(error)) == 0 && task != NULL,
          "fixed provider could not start cancellable task: %s", error);
    if (task != NULL) {
        CHECK(provider.poll(provider.context, task, &state, &output,
                            error, sizeof(error)) == 0 &&
                  state == HWA_INFERENCE_PENDING && output == NULL,
              "fixed provider task was not pending before cancellation");
        provider.task_free(provider.context, task);
        task = NULL;
    }
    hwa_inference_provider_destroy(&provider);
}

static void test_destroy_counter(void *context)
{
    int *count = (int *)context;
    if (count != NULL) (*count)++;
}

static void test_provider_destroy_owns_context_cleanup(void)
{
    HWAInferenceProvider provider;
    int destroy_count = 0;

    memset(&provider, 0, sizeof(provider));
    provider.context = &destroy_count;
    provider.destroy = test_destroy_counter;
    hwa_inference_provider_destroy(&provider);
    CHECK(destroy_count == 1, "provider context destroy was not called once");
    CHECK(provider.context == NULL && provider.destroy == NULL &&
              provider.start == NULL && provider.poll == NULL &&
              provider.task_free == NULL,
          "provider descriptor was not cleared after destroy");
    hwa_inference_provider_destroy(&provider);
    CHECK(destroy_count == 1, "provider context destroy was called twice");
}

static void test_payload_audio_init(HWAEventAudio *audio,
                                    uint64_t id,
                                    const char *name,
                                    const char *relative_path)
{
    memset(audio, 0, sizeof(*audio));
    audio->id = id;
    audio->kind = HWA_EVENT_SOURCE_RECORDING;
    audio->name = (char *)name;
    audio->relative_path = (char *)relative_path;
    audio->path_hint = (char *)"";
    memcpy(audio->sha256, test_source_sha256, HWA_SHA256_HEX_SIZE);
    audio->file_bytes = (uint64_t)sizeof(test_source_data);
    audio->format.container = HWA_CONTAINER_RIFF;
    audio->format.encoding = HWA_ENCODING_PCM;
    audio->format.channels = 1U;
    audio->format.sample_rate_hz = 48000U;
    audio->format.bits_per_sample = 16U;
    audio->format.valid_bits_per_sample = 16U;
    audio->format.block_align = 2U;
    audio->format.frames = UINT64_C(192);
    audio->format.data_bytes = UINT64_C(384);
    audio->format.duration_seconds = 0.004;
}

static void test_payload_init(HWAInferencePayload *payload,
                              TestBytes *bytes,
                              const char *relative_path)
{
    memset(payload, 0, sizeof(*payload));
    bytes->data = test_source_data;
    bytes->size = sizeof(test_source_data);
    payload->relative_path = relative_path;
    payload->bytes.context = bytes;
    payload->bytes.name = relative_path;
    payload->bytes.size = (uint64_t)sizeof(test_source_data);
    payload->bytes.read_at = test_read_at;
}

static void test_output_validator_accepts_exact_payloads_in_any_order(void)
{
    HWAEventAudio audio[2];
    HWAEventBundle bundle;
    HWAInferencePayload payloads[2];
    HWAInferenceOutput output;
    HWAEventBundleLimits limits;
    TestBytes bytes[2];
    char error[HWA_ERROR_SIZE] = {0};

    memset(&bundle, 0, sizeof(bundle));
    memset(&output, 0, sizeof(output));
    hwa_event_bundle_limits_default(&limits);
    test_payload_audio_init(&audio[0], UINT64_C(1), "first",
                            "audio/first.wav");
    test_payload_audio_init(&audio[1], UINT64_C(2), "second",
                            "audio/second.wav");
    test_payload_init(&payloads[0], &bytes[0], "audio/second.wav");
    test_payload_init(&payloads[1], &bytes[1], "audio/first.wav");
    bundle.audio = audio;
    bundle.audio_count = 2U;
    output.bundle = &bundle;
    output.payloads = payloads;
    output.payload_count = 2U;
    CHECK(hwa_inference_output_validate(&output, &limits,
                                        error, sizeof(error)) == 0,
          "exact inference payloads failed validation: %s", error);
}

static void test_output_validator_accepts_trace_payload(void)
{
    HWAEventAudio audio;
    HWAEventTrace trace;
    HWAEventBundle bundle;
    HWAInferencePayload payload;
    HWAInferenceOutput output;
    HWAEventBundleLimits limits;
    TestBytes bytes;
    char error[HWA_ERROR_SIZE] = {0};

    memset(&bundle, 0, sizeof(bundle));
    memset(&trace, 0, sizeof(trace));
    memset(&payload, 0, sizeof(payload));
    memset(&output, 0, sizeof(output));
    hwa_event_bundle_limits_default(&limits);
    test_payload_audio_init(&audio, UINT64_C(1), "source", "");
    trace.id = UINT64_C(1);
    trace.name = (char *)"pitch-hz";
    trace.unit = (char *)"Hz";
    trace.relative_path = (char *)"traces/pitch.f64le";
    memcpy(trace.sha256, test_trace_sha256, HWA_SHA256_HEX_SIZE);
    trace.format = HWA_EVENT_TRACE_F64LE;
    trace.source_recording_id = UINT64_C(1);
    trace.hop_samples = UINT64_C(1);
    trace.window_samples = UINT64_C(1);
    trace.point_count = UINT64_C(1);
    trace.value_width = 1U;
    trace.file_bytes = (uint64_t)sizeof(test_trace_data);
    bytes.data = test_trace_data;
    bytes.size = sizeof(test_trace_data);
    payload.relative_path = "traces/pitch.f64le";
    payload.bytes.context = &bytes;
    payload.bytes.name = "pitch.f64le";
    payload.bytes.size = (uint64_t)sizeof(test_trace_data);
    payload.bytes.read_at = test_read_at;
    bundle.audio = &audio;
    bundle.audio_count = 1U;
    bundle.traces = &trace;
    bundle.trace_count = 1U;
    output.bundle = &bundle;
    output.payloads = &payload;
    output.payload_count = 1U;
    CHECK(hwa_inference_output_validate(&output, &limits,
                                        error, sizeof(error)) == 0,
          "exact trace payload failed validation: %s", error);
}

static void test_output_validator_rejects_missing_extra_and_duplicate_payloads(void)
{
    HWAEventAudio audio[2];
    HWAEventBundle bundle;
    HWAInferencePayload payloads[2];
    HWAInferenceOutput output;
    HWAEventBundleLimits limits;
    TestBytes bytes[2];
    char error[HWA_ERROR_SIZE] = {0};

    memset(&bundle, 0, sizeof(bundle));
    memset(&output, 0, sizeof(output));
    hwa_event_bundle_limits_default(&limits);
    test_payload_audio_init(&audio[0], UINT64_C(1), "first",
                            "audio/first.wav");
    bundle.audio = audio;
    bundle.audio_count = 1U;
    output.bundle = &bundle;
    CHECK(hwa_inference_output_validate(&output, &limits,
                                        error, sizeof(error)) != 0,
          "output validator accepted a missing payload");

    audio[0].relative_path = (char *)"";
    test_payload_init(&payloads[0], &bytes[0], "audio/extra.wav");
    output.payloads = payloads;
    output.payload_count = 1U;
    CHECK(hwa_inference_output_validate(&output, &limits,
                                        error, sizeof(error)) != 0,
          "output validator accepted an extra payload");

    test_payload_audio_init(&audio[0], UINT64_C(1), "first",
                            "audio/first.wav");
    test_payload_audio_init(&audio[1], UINT64_C(2), "second",
                            "audio/second.wav");
    test_payload_init(&payloads[0], &bytes[0], "audio/first.wav");
    test_payload_init(&payloads[1], &bytes[1], "audio/first.wav");
    bundle.audio_count = 2U;
    output.payload_count = 2U;
    CHECK(hwa_inference_output_validate(&output, &limits,
                                        error, sizeof(error)) != 0,
          "output validator accepted a duplicate payload");
}

static void test_output_validator_checks_payload_size_hash_and_total(void)
{
    HWAEventAudio audio;
    HWAEventBundle bundle;
    HWAInferencePayload payload;
    HWAInferenceOutput output;
    HWAEventBundleLimits limits;
    TestBytes bytes;
    char error[HWA_ERROR_SIZE] = {0};

    memset(&bundle, 0, sizeof(bundle));
    memset(&output, 0, sizeof(output));
    hwa_event_bundle_limits_default(&limits);
    test_payload_audio_init(&audio, UINT64_C(1), "source",
                            "audio/source.wav");
    test_payload_init(&payload, &bytes, "audio/source.wav");
    bundle.audio = &audio;
    bundle.audio_count = 1U;
    output.bundle = &bundle;
    output.payloads = &payload;
    output.payload_count = 1U;

    payload.bytes.size--;
    CHECK(hwa_inference_output_validate(&output, &limits,
                                        error, sizeof(error)) != 0,
          "output validator accepted a wrong payload size");
    payload.bytes.size++;

    memset(audio.sha256, 'b', 64U);
    audio.sha256[64] = '\0';
    CHECK(hwa_inference_output_validate(&output, &limits,
                                        error, sizeof(error)) != 0,
          "output validator accepted a wrong payload hash");
    memcpy(audio.sha256, test_source_sha256, HWA_SHA256_HEX_SIZE);

    limits.max_bundle_bytes = (uint64_t)sizeof(test_source_data) - 1U;
    CHECK(hwa_inference_output_validate(&output, &limits,
                                        error, sizeof(error)) != 0,
          "output validator ignored the total payload byte limit");
}

static void test_output_write_rolls_back_when_source_read_fails(void)
{
    HWAEventAudio audio;
    HWAEventBundle bundle;
    HWAInferencePayload payload;
    HWAInferenceOutput output;
    HWAEventBundleLimits limits;
    TestChangingBytes bytes;
    char directory[PATH_MAX];
    char error[HWA_ERROR_SIZE] = {0};

    if (!test_make_unused_output_path(directory, "read-failure")) {
        CHECK(0, "cannot reserve output path for read failure test");
        return;
    }
    memset(&bundle, 0, sizeof(bundle));
    memset(&payload, 0, sizeof(payload));
    memset(&output, 0, sizeof(output));
    memset(&bytes, 0, sizeof(bytes));
    hwa_event_bundle_limits_default(&limits);
    test_payload_audio_init(&audio, UINT64_C(1), "source",
                            "audio/source.wav");
    bytes.first_data = test_source_data;
    bytes.size = sizeof(test_source_data);
    bytes.fail_at_read = 2U;
    payload.relative_path = "audio/source.wav";
    payload.bytes.context = &bytes;
    payload.bytes.name = "read-failure.wav";
    payload.bytes.size = (uint64_t)sizeof(test_source_data);
    payload.bytes.read_at = test_changing_read_at;
    bundle.audio = &audio;
    bundle.audio_count = 1U;
    output.bundle = &bundle;
    output.payloads = &payload;
    output.payload_count = 1U;

    CHECK(hwa_inference_output_write(directory, &output, &limits,
                                     error, sizeof(error)) != 0,
          "inference output write accepted a source read failure");
    CHECK(bytes.read_count == 2U,
          "source read failure did not occur on the write pass");
    CHECK(strstr(error, "cannot read event byte-source binding") != NULL,
          "source read failure returned the wrong error: %s", error);
    CHECK(!test_path_exists(directory),
          "source read failure left the destination bundle behind");
    test_remove_failed_output(directory);
}

static void test_output_write_rolls_back_when_source_bytes_change(void)
{
    HWAEventAudio audio;
    HWAEventBundle bundle;
    HWAInferencePayload payload;
    HWAInferenceOutput output;
    HWAEventBundleLimits limits;
    TestChangingBytes bytes;
    unsigned char changed_data[sizeof(test_source_data)];
    char directory[PATH_MAX];
    char error[HWA_ERROR_SIZE] = {0};

    if (!test_make_unused_output_path(directory, "changed-source")) {
        CHECK(0, "cannot reserve output path for changed source test");
        return;
    }
    memcpy(changed_data, test_source_data, sizeof(changed_data));
    changed_data[44] = 1U;
    memset(&bundle, 0, sizeof(bundle));
    memset(&payload, 0, sizeof(payload));
    memset(&output, 0, sizeof(output));
    memset(&bytes, 0, sizeof(bytes));
    hwa_event_bundle_limits_default(&limits);
    test_payload_audio_init(&audio, UINT64_C(1), "source",
                            "audio/source.wav");
    bytes.first_data = test_source_data;
    bytes.later_data = changed_data;
    bytes.size = sizeof(test_source_data);
    payload.relative_path = "audio/source.wav";
    payload.bytes.context = &bytes;
    payload.bytes.name = "changed-source.wav";
    payload.bytes.size = (uint64_t)sizeof(test_source_data);
    payload.bytes.read_at = test_changing_read_at;
    bundle.audio = &audio;
    bundle.audio_count = 1U;
    output.bundle = &bundle;
    output.payloads = &payload;
    output.payload_count = 1U;

    CHECK(hwa_inference_output_write(directory, &output, &limits,
                                     error, sizeof(error)) != 0,
          "inference output write accepted bytes changed after validation");
    CHECK(bytes.read_count == 2U,
          "changed source was not read once per validation and write pass");
    CHECK(strstr(error, "does not match its descriptor") != NULL,
          "changed source returned the wrong error: %s", error);
    CHECK(!test_path_exists(directory),
          "changed source left the destination bundle behind");
    test_remove_failed_output(directory);
}

static void test_fixed_provider_bounds_source_name_before_allocation(void)
{
    TestBytes bytes;
    HWAInferenceInput inputs[2];
    HWAInferenceRequest request;
    HWAInferenceProvider provider;
    char long_name[256];
    void *task = NULL;
    char error[HWA_ERROR_SIZE] = {0};

    test_request_init(&request, &bytes, inputs);
    request.output_limits.max_work_bytes = 600U;
    if (!test_provider_init(&provider)) return;
    CHECK(provider.start(provider.context, &request, &task,
                         error, sizeof(error)) == 0 && task != NULL,
          "fixed provider rejected a short source name at the test limit: %s",
          error);
    provider.task_free(provider.context, task);
    task = NULL;
    hwa_inference_provider_destroy(&provider);

    memset(long_name, 'n', sizeof(long_name));
    long_name[sizeof(long_name) - 1U] = '\0';
    inputs[0].bytes.name = long_name;
    if (!test_provider_init(&provider)) return;
    CHECK(provider.start(provider.context, &request, &task,
                         error, sizeof(error)) != 0 && task == NULL,
          "fixed provider accepted a source name over its work limit");
    CHECK(strstr(error, "source name") != NULL,
          "source name cap failed too late: %s", error);
    provider.task_free(provider.context, task);
    hwa_inference_provider_destroy(&provider);
}

int main(void)
{
    test_request_validator_checks_shape_limits_and_hashes();
    test_request_validator_checks_settings_format_and_callbacks();
    test_output_validator_binds_request_identity();
    test_fixed_provider_returns_one_exact_note();
    test_fixed_provider_obeys_output_count_limits();
    test_fixed_provider_rejects_result_outside_bundle_limits();
    test_request_validator_rejects_zero_output_limits();
    test_fixed_provider_accepts_spaced_empty_settings();
    test_fixed_provider_rejects_other_model_identity();
    test_fixed_provider_rejects_other_name_and_version();
    test_fixed_provider_rejects_duplicate_input_ids();
    test_fixed_provider_enforces_input_count_bounds();
    test_fixed_provider_accepts_named_score_context();
    test_failed_poll_returns_no_stale_output();
    test_fixed_provider_cancels_pending_task();
    test_provider_destroy_owns_context_cleanup();
    test_output_validator_accepts_exact_payloads_in_any_order();
    test_output_validator_accepts_trace_payload();
    test_output_validator_rejects_missing_extra_and_duplicate_payloads();
    test_output_validator_checks_payload_size_hash_and_total();
    test_output_write_rolls_back_when_source_read_fails();
    test_output_write_rolls_back_when_source_bytes_change();
    test_fixed_provider_bounds_source_name_before_allocation();
    if (failures != 0) {
        (void)fprintf(stderr, "%d inference test(s) failed\n", failures);
        return 1;
    }
    (void)puts("inference tests passed");
    return 0;
}

#include "inference_provider.h"

#include "event_bundle.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HWA_INFERENCE_JSON_SAFE_INTEGER UINT64_C(9007199254740991)
#define HWA_FIXED_PROVIDER_NAME "org.hlolli.fixed-inference"
#define HWA_FIXED_PROVIDER_VERSION "1"
#define HWA_FIXED_PROVIDER_MODEL_SHA256 ""
#define HWA_INFERENCE_HASH_BLOCK_SIZE 16384U
#define HWA_INFERENCE_PATH_LIMIT 4096U

typedef struct HWAFixedInferenceTask {
    HWAInferenceOutput output;
    HWAEventBundle bundle;
    unsigned poll_count;
} HWAFixedInferenceTask;

typedef struct HWAInferencePayloadIndex {
    const char *relative_path;
    const HWAInferencePayload *payload;
} HWAInferencePayloadIndex;

static void hwa_inference_error(char *error,
                                size_t error_size,
                                const char *message)
{
    if (error == NULL || error_size == 0U) return;
    (void)snprintf(error, error_size, "%s", message);
    error[error_size - 1U] = '\0';
}

static char *hwa_inference_copy_size(const char *text, size_t size)
{
    char *result;
    if (text == NULL || size == 0U) return NULL;
    result = (char *)malloc(size);
    if (result != NULL) memcpy(result, text, size);
    return result;
}

static int hwa_inference_lower_sha256(const char *text)
{
    size_t index;
    if (text == NULL) return 0;
    for (index = 0U; index < 64U; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (!((value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
              (value >= (unsigned char)'a' && value <= (unsigned char)'f')))
            return 0;
    }
    return text[64] == '\0';
}

static int hwa_inference_path_valid(const char *text)
{
    size_t index;
    if (text == NULL || text[0] == '\0') return 0;
    for (index = 1U; index < HWA_INFERENCE_PATH_LIMIT; ++index)
        if (text[index] == '\0') return 1;
    return 0;
}

static const char *hwa_inference_json_space(const char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;
    return text;
}

static int hwa_inference_empty_settings(const char *text)
{
    if (text == NULL) return 0;
    text = hwa_inference_json_space(text);
    if (*text++ != '{') return 0;
    text = hwa_inference_json_space(text);
    if (*text++ != '}') return 0;
    text = hwa_inference_json_space(text);
    return *text == '\0';
}

static int hwa_inference_payload_index_compare(const void *left_value,
                                               const void *right_value)
{
    const HWAInferencePayloadIndex *left =
        (const HWAInferencePayloadIndex *)left_value;
    const HWAInferencePayloadIndex *right =
        (const HWAInferencePayloadIndex *)right_value;
    return strcmp(left->relative_path, right->relative_path);
}

static const HWAInferencePayload *hwa_inference_payload_find(
    const HWAInferencePayloadIndex *index,
    size_t count,
    const char *relative_path)
{
    size_t first = 0U;
    size_t last = count;
    while (first < last) {
        size_t middle = first + (last - first) / 2U;
        int order = strcmp(relative_path, index[middle].relative_path);
        if (order < 0)
            last = middle;
        else if (order > 0)
            first = middle + 1U;
        else
            return index[middle].payload;
    }
    return NULL;
}

int hwa_inference_byte_source_sha256(
    const HWAByteSource *source,
    uint64_t max_bytes,
    char result[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    unsigned char buffer[HWA_INFERENCE_HASH_BLOCK_SIZE];
    unsigned char digest[32];
    HWASha256 hash;
    uint64_t offset = 0U;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (source == NULL || source->name == NULL ||
        source->name[0] == '\0' || source->read_at == NULL ||
        result == NULL || max_bytes == 0U || source->size > max_bytes ||
        source->size > UINT64_MAX / UINT64_C(8)) {
        hwa_inference_error(error, error_size,
                            "invalid or oversized inference byte source");
        if (result != NULL) memset(result, 0, HWA_SHA256_HEX_SIZE);
        return -1;
    }
    hwa_sha256_init(&hash);
    while (offset < source->size) {
        uint64_t remaining = source->size - offset;
        size_t count = remaining > (uint64_t)sizeof(buffer)
                           ? sizeof(buffer)
                           : (size_t)remaining;
        if (source->read_at(source->context, offset, buffer, count) != 0) {
            hwa_inference_error(error, error_size,
                                "cannot read inference payload bytes");
            memset(result, 0, HWA_SHA256_HEX_SIZE);
            return -1;
        }
        hwa_sha256_update(&hash, buffer, count);
        offset += (uint64_t)count;
    }
    hwa_sha256_final(&hash, digest);
    hwa_sha256_hex(digest, result);
    return 0;
}

static int hwa_inference_payload_check(
    const HWAInferencePayloadIndex *index,
    size_t count,
    const char *relative_path,
    uint64_t expected_size,
    const char *expected_sha256,
    const HWAEventBundleLimits *limits,
    char *error,
    size_t error_size)
{
    const HWAInferencePayload *payload =
        hwa_inference_payload_find(index, count, relative_path);
    char actual_sha256[HWA_SHA256_HEX_SIZE];
    if (payload == NULL) {
        hwa_inference_error(error, error_size,
                            "inference output payload is missing");
        return -1;
    }
    if (payload->bytes.size != expected_size ||
        payload->bytes.size > limits->max_payload_file_bytes) {
        hwa_inference_error(error, error_size,
                            "inference output payload size is wrong");
        return -1;
    }
    if (hwa_inference_byte_source_sha256(
            &payload->bytes, limits->max_payload_file_bytes, actual_sha256,
            error, error_size) != 0)
        return -1;
    if (strcmp(actual_sha256, expected_sha256) != 0) {
        hwa_inference_error(error, error_size,
                            "inference output payload hash is wrong");
        return -1;
    }
    return 0;
}

int hwa_inference_output_validate(const HWAInferenceOutput *output,
                                  const HWAEventBundleLimits *limits,
                                  char *error,
                                  size_t error_size)
{
    HWAInferencePayloadIndex *index = NULL;
    size_t expected_count;
    size_t payload_index;
    size_t row;
    uint64_t total_bytes = 0U;
    int result = -1;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (output == NULL || output->bundle == NULL || limits == NULL) {
        hwa_inference_error(error, error_size,
                            "invalid inference output");
        return -1;
    }
    if (hwa_event_bundle_validate(output->bundle, limits,
                                  error, error_size) != 0)
        return -1;
    expected_count = 0U;
    for (row = 0U; row < output->bundle->audio_count; ++row) {
        const HWAEventAudio *audio = &output->bundle->audio[row];
        const char *path = audio->relative_path;
        if (path != NULL && path[0] != '\0') {
            if (expected_count == SIZE_MAX) {
                hwa_inference_error(error, error_size,
                                    "inference payload count overflows");
                return -1;
            }
            expected_count++;
            if (total_bytes > limits->max_bundle_bytes ||
                audio->file_bytes > limits->max_bundle_bytes - total_bytes) {
                hwa_inference_error(
                    error, error_size,
                    "inference output payload bytes exceed limit");
                return -1;
            }
            total_bytes += audio->file_bytes;
        }
    }
    for (row = 0U; row < output->bundle->trace_count; ++row) {
        const HWAEventTrace *trace = &output->bundle->traces[row];
        if (expected_count == SIZE_MAX) {
            hwa_inference_error(error, error_size,
                                "inference payload count overflows");
            return -1;
        }
        expected_count++;
        if (total_bytes > limits->max_bundle_bytes ||
            trace->file_bytes > limits->max_bundle_bytes - total_bytes) {
            hwa_inference_error(error, error_size,
                                "inference output payload bytes exceed limit");
            return -1;
        }
        total_bytes += trace->file_bytes;
    }
    if (output->payload_count != expected_count ||
        (output->payload_count == 0U && output->payloads != NULL) ||
        (output->payload_count != 0U && output->payloads == NULL)) {
        hwa_inference_error(error, error_size,
                            "inference output payload count is wrong");
        return -1;
    }
    if (output->payload_count == 0U) return 0;
    if (output->payload_count > SIZE_MAX / sizeof(*index) ||
        (uint64_t)output->payload_count >
            limits->max_work_bytes / (uint64_t)sizeof(*index)) {
        hwa_inference_error(error, error_size,
                            "inference payload index exceeds work limit");
        return -1;
    }
    index = (HWAInferencePayloadIndex *)calloc(output->payload_count,
                                               sizeof(*index));
    if (index == NULL) {
        hwa_inference_error(error, error_size,
                            "cannot allocate inference payload index");
        return -1;
    }
    for (payload_index = 0U; payload_index < output->payload_count;
         ++payload_index) {
        const HWAInferencePayload *payload = &output->payloads[payload_index];
        if (!hwa_inference_path_valid(payload->relative_path) ||
            payload->bytes.name == NULL || payload->bytes.name[0] == '\0' ||
            payload->bytes.read_at == NULL) {
            hwa_inference_error(error, error_size,
                                "invalid inference output payload");
            goto cleanup;
        }
        index[payload_index].relative_path = payload->relative_path;
        index[payload_index].payload = payload;
    }
    if (output->payload_count > 1U)
        qsort(index, output->payload_count, sizeof(*index),
              hwa_inference_payload_index_compare);
    for (payload_index = 1U; payload_index < output->payload_count;
         ++payload_index) {
        if (strcmp(index[payload_index - 1U].relative_path,
                   index[payload_index].relative_path) == 0) {
            hwa_inference_error(error, error_size,
                                "duplicate inference output payload");
            goto cleanup;
        }
    }
    for (row = 0U; row < output->bundle->audio_count; ++row) {
        const HWAEventAudio *audio = &output->bundle->audio[row];
        if (audio->relative_path == NULL || audio->relative_path[0] == '\0')
            continue;
        if (hwa_inference_payload_check(
                index, output->payload_count, audio->relative_path,
                audio->file_bytes, audio->sha256, limits,
                error, error_size) != 0)
            goto cleanup;
    }
    for (row = 0U; row < output->bundle->trace_count; ++row) {
        const HWAEventTrace *trace = &output->bundle->traces[row];
        if (hwa_inference_payload_check(
                index, output->payload_count, trace->relative_path,
                trace->file_bytes, trace->sha256, limits,
                error, error_size) != 0)
            goto cleanup;
    }
    result = 0;
cleanup:
    free(index);
    return result;
}

int hwa_inference_output_write(const char *output_directory,
                               const HWAInferenceOutput *output,
                               const HWAEventBundleLimits *limits,
                               char *error,
                               size_t error_size)
{
    HWAEventSourceBinding *bindings = NULL;
    size_t index;
    int result;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (output_directory == NULL || output_directory[0] == '\0' ||
        output == NULL || limits == NULL) {
        hwa_inference_error(error, error_size,
                            "invalid inference output write arguments");
        return -1;
    }
    if (hwa_inference_output_validate(output, limits,
                                      error, error_size) != 0)
        return -1;
    if (output->payload_count != 0U) {
        uint64_t work;
        if (output->payload_count > SIZE_MAX / sizeof(*bindings)) {
            hwa_inference_error(error, error_size,
                                "inference output binding count overflows");
            return -1;
        }
        work = (uint64_t)output->payload_count *
               (uint64_t)sizeof(*bindings);
        if (work > limits->max_work_bytes) {
            hwa_inference_error(error, error_size,
                                "inference output bindings exceed work limit");
            return -1;
        }
        bindings = (HWAEventSourceBinding *)calloc(
            output->payload_count, sizeof(*bindings));
        if (bindings == NULL) {
            hwa_inference_error(error, error_size,
                                "cannot allocate inference output bindings");
            return -1;
        }
        for (index = 0U; index < output->payload_count; ++index) {
            bindings[index].relative_path =
                output->payloads[index].relative_path;
            bindings[index].source = output->payloads[index].bytes;
        }
    }
    result = hwa_event_bundle_write_sources(
        output_directory, output->bundle, bindings, output->payload_count,
        limits, error, error_size);
    free(bindings);
    return result;
}

void hwa_inference_provider_destroy(HWAInferenceProvider *provider)
{
    if (provider == NULL) return;
    if (provider->destroy != NULL) provider->destroy(provider->context);
    memset(provider, 0, sizeof(*provider));
}

static void hwa_inference_fixed_task_free(void *context, void *task_value)
{
    HWAFixedInferenceTask *task = (HWAFixedInferenceTask *)task_value;
    (void)context;
    if (task == NULL) return;
    hwa_event_bundle_free(&task->bundle);
    free(task);
}

static int hwa_inference_input_valid(const HWAInferenceInput *input)
{
    return input->id != NULL && input->id[0] != '\0' &&
           input->role != NULL && input->role[0] != '\0' &&
           input->media_type != NULL && input->media_type[0] != '\0' &&
           hwa_inference_lower_sha256(input->sha256) &&
           input->bytes.name != NULL && input->bytes.name[0] != '\0' &&
           input->bytes.read_at != NULL &&
           input->bytes.size <= HWA_INFERENCE_JSON_SAFE_INTEGER;
}

static int hwa_inference_fixed_request_valid(
    const HWAInferenceRequest *request,
    const HWAInferenceInput **source_result)
{
    const HWAInferenceInput *source = NULL;
    size_t index;
    size_t prior;
    if (request == NULL || source_result == NULL || request->task == NULL ||
        strcmp(request->task, "org.hlolli.fixed-note") != 0 ||
        !hwa_inference_empty_settings(request->settings_json) ||
        request->source_recording_id == 0U ||
        request->source_recording_id > HWA_INFERENCE_JSON_SAFE_INTEGER ||
        request->source_input_id == NULL ||
        request->source_input_id[0] == '\0' || request->inputs == NULL ||
        request->input_count == 0U ||
        request->input_count > HWA_INFERENCE_MAX_INPUTS ||
        request->source_format.sample_rate_hz == 0U ||
        request->source_format.frames < UINT64_C(192) ||
        request->source_format.frames > HWA_INFERENCE_JSON_SAFE_INTEGER) {
        return 0;
    }
    for (index = 0U; index < request->input_count; ++index) {
        const HWAInferenceInput *input = &request->inputs[index];
        if (!hwa_inference_input_valid(input)) return 0;
        for (prior = 0U; prior < index; ++prior)
            if (strcmp(request->inputs[prior].id, input->id) == 0) return 0;
        if (strcmp(input->id, request->source_input_id) == 0) source = input;
    }
    if (source == NULL || strcmp(source->role, "source-recording") != 0 ||
        strcmp(source->media_type, "audio/wav") != 0) return 0;
    *source_result = source;
    return 1;
}

static int hwa_inference_fixed_identity_valid(
    const HWAInferenceRequest *request)
{
    return request != NULL && request->expected_provider_name != NULL &&
           strcmp(request->expected_provider_name,
                  HWA_FIXED_PROVIDER_NAME) == 0 &&
           request->expected_provider_version != NULL &&
           strcmp(request->expected_provider_version,
                  HWA_FIXED_PROVIDER_VERSION) == 0 &&
           request->expected_model_sha256 != NULL &&
           strcmp(request->expected_model_sha256,
                  HWA_FIXED_PROVIDER_MODEL_SHA256) == 0;
}

static int hwa_inference_fixed_source_name_size(
    const char *name,
    uint64_t max_work_bytes,
    size_t *result)
{
    uint64_t base_work =
        (uint64_t)sizeof(HWAEventProvider) +
        (uint64_t)sizeof(HWAEventAudio) +
        (uint64_t)sizeof(HWAPerformanceEvent) +
        (uint64_t)sizeof(HWAEventValue) +
        (uint64_t)sizeof(HWA_FIXED_PROVIDER_NAME) +
        (uint64_t)sizeof(HWA_FIXED_PROVIDER_VERSION) +
        (uint64_t)sizeof("{}") +
        (uint64_t)sizeof("") +
        (uint64_t)sizeof("") +
        (uint64_t)sizeof("note") +
        (uint64_t)sizeof("") +
        (uint64_t)sizeof("") +
        (uint64_t)sizeof("") +
        (uint64_t)sizeof("pitch-hz") +
        (uint64_t)sizeof("Hz");
    uint64_t available;
    size_t maximum;
    size_t index;
    if (name == NULL || result == NULL || base_work >= max_work_bytes)
        return -1;
    available = max_work_bytes - base_work;
#if SIZE_MAX < UINT64_MAX
    maximum = available > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)available;
#else
    maximum = (size_t)available;
#endif
    for (index = 0U; index < maximum; ++index) {
        if (name[index] == '\0') {
            *result = index + 1U;
            return 0;
        }
    }
    return -1;
}

static int hwa_inference_fixed_start(void *context,
                                     const HWAInferenceRequest *request,
                                     void **task_result,
                                     char *error,
                                     size_t error_size)
{
    HWAFixedInferenceTask *task;
    HWAEventProvider *provider;
    HWAEventAudio *audio;
    HWAPerformanceEvent *event;
    HWAEventValue *value;
    const HWAInferenceInput *source = NULL;
    size_t source_name_size;
    (void)context;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (task_result == NULL) {
        hwa_inference_error(error, error_size,
                            "fixed inference task output is null");
        return -1;
    }
    *task_result = NULL;
    if (!hwa_inference_fixed_identity_valid(request)) {
        hwa_inference_error(error, error_size,
                            "fixed inference provider identity mismatch");
        return -1;
    }
    if (!hwa_inference_fixed_request_valid(request, &source)) {
        hwa_inference_error(error, error_size,
                            "invalid fixed inference request");
        return -1;
    }
    if (request->output_limits.max_audio_files < 1U ||
        request->output_limits.max_events < 1U ||
        request->output_limits.max_values < 1U ||
        request->output_limits.max_providers < 1U) {
        hwa_inference_error(error, error_size,
                            "fixed inference output limit exceeded");
        return -1;
    }
    if (hwa_inference_fixed_source_name_size(
            source->bytes.name, request->output_limits.max_work_bytes,
            &source_name_size) != 0) {
        hwa_inference_error(error, error_size,
                            "fixed inference source name exceeds output work limit");
        return -1;
    }
    task = (HWAFixedInferenceTask *)calloc(1U, sizeof(*task));
    if (task == NULL) goto allocation_failed;
    task->bundle.providers =
        (HWAEventProvider *)calloc(1U, sizeof(*task->bundle.providers));
    task->bundle.audio =
        (HWAEventAudio *)calloc(1U, sizeof(*task->bundle.audio));
    task->bundle.events =
        (HWAPerformanceEvent *)calloc(1U, sizeof(*task->bundle.events));
    if (task->bundle.providers == NULL || task->bundle.audio == NULL ||
        task->bundle.events == NULL) goto allocation_failed_task;
    task->bundle.provider_count = 1U;
    task->bundle.audio_count = 1U;
    task->bundle.event_count = 1U;
    provider = &task->bundle.providers[0];
    audio = &task->bundle.audio[0];
    event = &task->bundle.events[0];
    event->values = (HWAEventValue *)calloc(1U, sizeof(*event->values));
    if (event->values == NULL) goto allocation_failed_task;
    event->value_count = 1U;
    value = &event->values[0];

    provider->id = UINT64_C(1);
    provider->name = hwa_inference_copy_size(
        HWA_FIXED_PROVIDER_NAME, sizeof(HWA_FIXED_PROVIDER_NAME));
    provider->version = hwa_inference_copy_size(
        HWA_FIXED_PROVIDER_VERSION, sizeof(HWA_FIXED_PROVIDER_VERSION));
    provider->settings_json = hwa_inference_copy_size("{}", sizeof("{}"));
    audio->id = request->source_recording_id;
    audio->kind = HWA_EVENT_SOURCE_RECORDING;
    audio->name = hwa_inference_copy_size(source->bytes.name,
                                          source_name_size);
    audio->relative_path = hwa_inference_copy_size("", sizeof(""));
    audio->path_hint = hwa_inference_copy_size("", sizeof(""));
    memcpy(audio->sha256, source->sha256, HWA_SHA256_HEX_SIZE);
    audio->file_bytes = source->bytes.size;
    audio->format = request->source_format;
    event->id = UINT64_C(1);
    event->kind = hwa_inference_copy_size("note", sizeof("note"));
    event->source_recording_id = request->source_recording_id;
    event->evidence_audio_id = request->source_recording_id;
    event->evidence_audio_id_valid = 1;
    event->start_sample = UINT64_C(64);
    event->end_sample = UINT64_C(192);
    event->voice = hwa_inference_copy_size("", sizeof(""));
    event->part = hwa_inference_copy_size("", sizeof(""));
    event->score_event_id = hwa_inference_copy_size("", sizeof(""));
    value->name = hwa_inference_copy_size("pitch-hz", sizeof("pitch-hz"));
    value->kind = HWA_EVENT_VALUE_F64;
    value->basis = HWA_EVENT_INFERENCE;
    value->number = 440.00000000000006;
    value->unit = hwa_inference_copy_size("Hz", sizeof("Hz"));
    value->score = 0.875;
    value->provider_id = UINT64_C(1);
    value->score_valid = 1;
    value->provider_id_valid = 1;
    value->selected = 1;
    if (provider->name == NULL || provider->version == NULL ||
        provider->settings_json == NULL || audio->name == NULL ||
        audio->relative_path == NULL || audio->path_hint == NULL ||
        event->kind == NULL || event->voice == NULL || event->part == NULL ||
        event->score_event_id == NULL || value->name == NULL ||
        value->unit == NULL) goto allocation_failed_task;
    task->output.bundle = &task->bundle;
    if (hwa_inference_output_validate(&task->output,
                                      &request->output_limits,
                                      error, error_size) != 0) {
        hwa_inference_fixed_task_free(NULL, task);
        return -1;
    }
    *task_result = task;
    return 0;

allocation_failed_task:
    hwa_inference_fixed_task_free(NULL, task);
allocation_failed:
    hwa_inference_error(error, error_size,
                        "cannot allocate fixed inference task");
    return -1;
}

static int hwa_inference_fixed_poll(void *context,
                                    void *task_value,
                                    HWAInferencePollState *state,
                                    const HWAInferenceOutput **output,
                                    char *error,
                                    size_t error_size)
{
    HWAFixedInferenceTask *task = (HWAFixedInferenceTask *)task_value;
    (void)context;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (state != NULL) *state = HWA_INFERENCE_PENDING;
    if (output != NULL) *output = NULL;
    if (task == NULL || state == NULL || output == NULL) {
        hwa_inference_error(error, error_size,
                            "invalid fixed inference poll");
        return -1;
    }
    if (task->poll_count == 0U) {
        task->poll_count++;
        return 0;
    }
    *state = HWA_INFERENCE_READY;
    *output = &task->output;
    return 0;
}

static void hwa_inference_fixed_destroy(void *context)
{
    (void)context;
}

void hwa_inference_fixed_provider_init(HWAInferenceProvider *provider)
{
    if (provider == NULL) return;
    memset(provider, 0, sizeof(*provider));
    provider->name = HWA_FIXED_PROVIDER_NAME;
    provider->version = HWA_FIXED_PROVIDER_VERSION;
    provider->model_sha256 = HWA_FIXED_PROVIDER_MODEL_SHA256;
    provider->start = hwa_inference_fixed_start;
    provider->poll = hwa_inference_fixed_poll;
    provider->task_free = hwa_inference_fixed_task_free;
    provider->destroy = hwa_inference_fixed_destroy;
}

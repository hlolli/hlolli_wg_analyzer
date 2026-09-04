#include "inference_provenance.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HWA_INFERENCE_PROVENANCE_TEXT_LIMIT 4096U

typedef struct HWAInferenceJsonBuilder {
    char *text;
    size_t length;
    size_t capacity;
    size_t maximum;
} HWAInferenceJsonBuilder;

static void hwa_inference_provenance_error(char *error,
                                           size_t error_size,
                                           const char *message)
{
    if (error == NULL || error_size == 0U) return;
    (void)snprintf(error, error_size, "%s", message);
    error[error_size - 1U] = '\0';
}

static int hwa_inference_provenance_text_size(const char *text,
                                              size_t maximum,
                                              size_t *size)
{
    size_t index;
    if (text == NULL || size == NULL) return -1;
    for (index = 0U; index < maximum; ++index) {
        if (text[index] == '\0') {
            *size = index;
            return 0;
        }
    }
    return -1;
}

static size_t hwa_inference_provenance_utf8_size(
    const unsigned char *text,
    size_t remaining)
{
    unsigned char first;
    if (text == NULL || remaining == 0U) return 0U;
    first = text[0];
    if (first < 0x80U) return 1U;
    if (first >= 0xc2U && first <= 0xdfU) {
        return remaining >= 2U && text[1] >= 0x80U && text[1] <= 0xbfU
                   ? 2U : 0U;
    }
    if (first >= 0xe0U && first <= 0xefU) {
        if (remaining < 3U || text[2] < 0x80U || text[2] > 0xbfU)
            return 0U;
        if (first == 0xe0U)
            return text[1] >= 0xa0U && text[1] <= 0xbfU ? 3U : 0U;
        if (first == 0xedU)
            return text[1] >= 0x80U && text[1] <= 0x9fU ? 3U : 0U;
        return text[1] >= 0x80U && text[1] <= 0xbfU ? 3U : 0U;
    }
    if (first >= 0xf0U && first <= 0xf4U) {
        if (remaining < 4U || text[2] < 0x80U || text[2] > 0xbfU ||
            text[3] < 0x80U || text[3] > 0xbfU)
            return 0U;
        if (first == 0xf0U)
            return text[1] >= 0x90U && text[1] <= 0xbfU ? 4U : 0U;
        if (first == 0xf4U)
            return text[1] >= 0x80U && text[1] <= 0x8fU ? 4U : 0U;
        return text[1] >= 0x80U && text[1] <= 0xbfU ? 4U : 0U;
    }
    return 0U;
}

static int hwa_inference_provenance_lower_sha256(const char *text)
{
    size_t index;
    if (text == NULL) return 0;
    for (index = 0U; index < 64U; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (value == 0U) return 0;
        if (!((value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
              (value >= (unsigned char)'a' && value <= (unsigned char)'f')))
            return 0;
    }
    return text[64] == '\0';
}

static int hwa_inference_provenance_utf8_text(const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    size_t remaining;
    if (hwa_inference_provenance_text_size(
            text, HWA_INFERENCE_PROVENANCE_TEXT_LIMIT, &remaining) != 0)
        return 0;
    while (remaining != 0U) {
        size_t count = hwa_inference_provenance_utf8_size(cursor, remaining);
        if (count == 0U) return 0;
        cursor += count;
        remaining -= count;
    }
    return 1;
}

static int hwa_inference_json_reserve(HWAInferenceJsonBuilder *builder,
                                      size_t extra)
{
    size_t needed;
    size_t capacity;
    char *grown;
    if (builder == NULL || extra > SIZE_MAX - builder->length - 1U)
        return -1;
    needed = builder->length + extra + 1U;
    if (needed > builder->maximum) return -1;
    if (needed <= builder->capacity) return 0;
    capacity = builder->capacity == 0U ? 256U : builder->capacity;
    while (capacity < needed) {
        size_t next = capacity > SIZE_MAX / 2U ? SIZE_MAX : capacity * 2U;
        if (next > builder->maximum) next = builder->maximum;
        if (next <= capacity) return -1;
        capacity = next;
    }
    grown = (char *)realloc(builder->text, capacity);
    if (grown == NULL) return -1;
    builder->text = grown;
    builder->capacity = capacity;
    return 0;
}

static int hwa_inference_json_append_size(HWAInferenceJsonBuilder *builder,
                                          const char *text,
                                          size_t size)
{
    if (text == NULL || hwa_inference_json_reserve(builder, size) != 0)
        return -1;
    memcpy(builder->text + builder->length, text, size);
    builder->length += size;
    builder->text[builder->length] = '\0';
    return 0;
}

static int hwa_inference_json_append(HWAInferenceJsonBuilder *builder,
                                     const char *text)
{
    size_t size;
    if (hwa_inference_provenance_text_size(
            text, builder->maximum, &size) != 0)
        return -1;
    return hwa_inference_json_append_size(builder, text, size);
}

static int hwa_inference_json_string(HWAInferenceJsonBuilder *builder,
                                     const char *text)
{
    static const char hex[] = "0123456789abcdef";
    size_t size;
    size_t index;
    if (hwa_inference_provenance_text_size(
            text, HWA_INFERENCE_PROVENANCE_TEXT_LIMIT, &size) != 0 ||
        hwa_inference_json_append_size(builder, "\"", 1U) != 0)
        return -1;
    for (index = 0U; index < size; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (value == (unsigned char)'\"' || value == (unsigned char)'\\') {
            char escaped[2];
            escaped[0] = '\\';
            escaped[1] = (char)value;
            if (hwa_inference_json_append_size(builder, escaped, 2U) != 0)
                return -1;
        } else if (value < 0x20U) {
            char escaped[6] = {'\\', 'u', '0', '0', '0', '0'};
            escaped[4] = hex[value >> 4U];
            escaped[5] = hex[value & 0x0fU];
            if (hwa_inference_json_append_size(builder, escaped, 6U) != 0)
                return -1;
        } else if (value >= 0x80U) {
            size_t sequence = hwa_inference_provenance_utf8_size(
                (const unsigned char *)text + index, size - index);
            if (sequence == 0U) {
                char escaped[6] = {'\\', 'u', '0', '0', '0', '0'};
                escaped[4] = hex[value >> 4U];
                escaped[5] = hex[value & 0x0fU];
                if (hwa_inference_json_append_size(
                        builder, escaped, 6U) != 0)
                    return -1;
            } else {
                if (hwa_inference_json_append_size(
                        builder, text + index, sequence) != 0)
                    return -1;
                index += sequence - 1U;
            }
        } else if (hwa_inference_json_append_size(
                       builder, (const char *)&text[index], 1U) != 0) {
            return -1;
        }
    }
    return hwa_inference_json_append_size(builder, "\"", 1U);
}

static int hwa_inference_json_fixed_u64(HWAInferenceJsonBuilder *builder,
                                        uint64_t value)
{
    char text[23];
    int written = snprintf(text, sizeof(text), "\"%020" PRIu64 "\"", value);
    if (written != 22) return -1;
    return hwa_inference_json_append_size(builder, text, 22U);
}

static int hwa_inference_input_pointer_compare(const void *left_value,
                                               const void *right_value)
{
    const HWAInferenceInput *const *left =
        (const HWAInferenceInput *const *)left_value;
    const HWAInferenceInput *const *right =
        (const HWAInferenceInput *const *)right_value;
    return strcmp((*left)->id, (*right)->id);
}

static int hwa_inference_provenance_object(const char *text, size_t maximum)
{
    size_t size;
    size_t first = 0U;
    size_t last;
    if (hwa_inference_provenance_text_size(text, maximum, &size) != 0)
        return 0;
    while (first < size &&
           (text[first] == ' ' || text[first] == '\t' ||
            text[first] == '\r' || text[first] == '\n'))
        first++;
    last = size;
    while (last > first &&
           (text[last - 1U] == ' ' || text[last - 1U] == '\t' ||
            text[last - 1U] == '\r' || text[last - 1U] == '\n'))
        last--;
    return last > first + 1U && text[first] == '{' && text[last - 1U] == '}';
}

static int hwa_inference_provenance_fields_valid(
    const HWAInferenceRequest *request,
    const HWAInferenceRuntimeProvenance *runtime,
    size_t maximum)
{
    size_t index;
    if (request == NULL || runtime == NULL || request->task == NULL ||
        request->task[0] == '\0' || request->settings_json == NULL ||
        request->inputs == NULL || request->input_count == 0U ||
        request->input_count > HWA_INFERENCE_MAX_INPUTS ||
        runtime->name == NULL || runtime->name[0] == '\0' ||
        runtime->version == NULL || runtime->version[0] == '\0' ||
        runtime->backend == NULL || runtime->backend[0] == '\0' ||
        runtime->fallback == NULL || runtime->adapter_sha256 == NULL ||
        !hwa_inference_provenance_object(request->settings_json, maximum) ||
        !hwa_inference_provenance_utf8_text(request->task) ||
        !hwa_inference_provenance_utf8_text(runtime->name) ||
        !hwa_inference_provenance_utf8_text(runtime->version) ||
        !hwa_inference_provenance_utf8_text(runtime->backend) ||
        !hwa_inference_provenance_utf8_text(runtime->fallback) ||
        !hwa_inference_provenance_utf8_text(runtime->adapter_sha256) ||
        !hwa_inference_provenance_lower_sha256(runtime->adapter_sha256))
        return 0;
    for (index = 0U; index < request->input_count; ++index) {
        const HWAInferenceInput *input = &request->inputs[index];
        if (input->id == NULL || input->id[0] == '\0' ||
            input->role == NULL || input->role[0] == '\0' ||
            input->media_type == NULL || input->media_type[0] == '\0' ||
            input->sha256 == NULL || input->bytes.name == NULL ||
            !hwa_inference_provenance_utf8_text(input->id) ||
            !hwa_inference_provenance_utf8_text(input->role) ||
            !hwa_inference_provenance_utf8_text(input->media_type) ||
            !hwa_inference_provenance_utf8_text(input->bytes.name) ||
            !hwa_inference_provenance_lower_sha256(input->sha256))
            return 0;
    }
    return 1;
}

int hwa_inference_provenance_settings_build(
    const HWAInferenceRequest *request,
    const HWAInferenceRuntimeProvenance *runtime,
    uint64_t max_settings_bytes,
    char **settings_json,
    char *error,
    size_t error_size)
{
    HWAInferenceJsonBuilder builder;
    const HWAInferenceInput **inputs = NULL;
    char *exact;
    size_t index;
    size_t maximum;
    int result = -1;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (settings_json == NULL) {
        hwa_inference_provenance_error(error, error_size,
                                       "inference settings output is null");
        return -1;
    }
    *settings_json = NULL;
    if (max_settings_bytes == 0U ||
        max_settings_bytes >= (uint64_t)SIZE_MAX) {
        hwa_inference_provenance_error(error, error_size,
                                       "invalid inference settings byte limit");
        return -1;
    }
    maximum = (size_t)max_settings_bytes + 1U;
    if (!hwa_inference_provenance_fields_valid(request, runtime, maximum) ||
        request->input_count > SIZE_MAX / sizeof(*inputs)) {
        hwa_inference_provenance_error(error, error_size,
                                       "invalid inference provenance facts");
        return -1;
    }
    inputs = (const HWAInferenceInput **)calloc(
        request->input_count, sizeof(*inputs));
    if (inputs == NULL) {
        hwa_inference_provenance_error(error, error_size,
                                       "cannot allocate inference input index");
        return -1;
    }
    for (index = 0U; index < request->input_count; ++index)
        inputs[index] = &request->inputs[index];
    if (request->input_count > 1U)
        qsort(inputs, request->input_count, sizeof(*inputs),
              hwa_inference_input_pointer_compare);
    for (index = 1U; index < request->input_count; ++index) {
        if (strcmp(inputs[index - 1U]->id, inputs[index]->id) == 0) {
            hwa_inference_provenance_error(error, error_size,
                                           "duplicate inference input ID");
            goto cleanup;
        }
    }
    memset(&builder, 0, sizeof(builder));
    builder.maximum = maximum;
    if (hwa_inference_json_append(&builder, "{\"task\":") != 0 ||
        hwa_inference_json_string(&builder, request->task) != 0 ||
        hwa_inference_json_append(&builder, ",\"seed\":") != 0 ||
        hwa_inference_json_fixed_u64(&builder, request->seed) != 0 ||
        hwa_inference_json_append(&builder, ",\"inputs\":[") != 0)
        goto too_large;
    for (index = 0U; index < request->input_count; ++index) {
        const HWAInferenceInput *input = inputs[index];
        if ((index != 0U && hwa_inference_json_append(&builder, ",") != 0) ||
            hwa_inference_json_append(&builder, "{\"id\":") != 0 ||
            hwa_inference_json_string(&builder, input->id) != 0 ||
            hwa_inference_json_append(&builder, ",\"role\":") != 0 ||
            hwa_inference_json_string(&builder, input->role) != 0 ||
            hwa_inference_json_append(&builder, ",\"media_type\":") != 0 ||
            hwa_inference_json_string(&builder, input->media_type) != 0 ||
            hwa_inference_json_append(&builder, ",\"name\":") != 0 ||
            hwa_inference_json_string(&builder, input->bytes.name) != 0 ||
            hwa_inference_json_append(&builder, ",\"bytes\":") != 0 ||
            hwa_inference_json_fixed_u64(&builder, input->bytes.size) != 0 ||
            hwa_inference_json_append(&builder, ",\"sha256\":") != 0 ||
            hwa_inference_json_string(&builder, input->sha256) != 0 ||
            hwa_inference_json_append(&builder, "}") != 0)
            goto too_large;
    }
    if (hwa_inference_json_append(
            &builder, "],\"runtime\":{\"name\":") != 0 ||
        hwa_inference_json_string(&builder, runtime->name) != 0 ||
        hwa_inference_json_append(&builder, ",\"version\":") != 0 ||
        hwa_inference_json_string(&builder, runtime->version) != 0 ||
        hwa_inference_json_append(&builder, ",\"backend\":") != 0 ||
        hwa_inference_json_string(&builder, runtime->backend) != 0 ||
        hwa_inference_json_append(&builder, ",\"fallback\":") != 0 ||
        hwa_inference_json_string(&builder, runtime->fallback) != 0 ||
        hwa_inference_json_append(&builder, ",\"adapter_sha256\":") != 0 ||
        hwa_inference_json_string(&builder, runtime->adapter_sha256) != 0 ||
        hwa_inference_json_append(&builder, "},\"task_settings\":") != 0 ||
        hwa_inference_json_append(&builder, request->settings_json) != 0 ||
        hwa_inference_json_append(&builder, "}") != 0)
        goto too_large;
    exact = (char *)realloc(builder.text, builder.length + 1U);
    if (exact == NULL) {
        hwa_inference_provenance_error(
            error, error_size, "cannot finalize inference settings");
        free(builder.text);
        goto cleanup;
    }
    builder.text = exact;
    *settings_json = builder.text;
    builder.text = NULL;
    result = 0;
    goto cleanup;

too_large:
    hwa_inference_provenance_error(error, error_size,
                                   "inference settings exceed byte limit");
    free(builder.text);
cleanup:
    free(inputs);
    return result;
}

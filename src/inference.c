#include "inference_provider.h"

#include "event_bundle.h"
#include "internal.h"
#include "sha256.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HWA_INFERENCE_JSON_SAFE_INTEGER UINT64_C(9007199254740991)
#define HWA_FIXED_PROVIDER_NAME "org.hlolli.fixed-inference"
#define HWA_FIXED_PROVIDER_VERSION "1"
#define HWA_FIXED_PROVIDER_MODEL_SHA256 ""
#define HWA_INFERENCE_HASH_BLOCK_SIZE 16384U
#define HWA_INFERENCE_PATH_LIMIT 4096U
#define HWA_INFERENCE_TEXT_LIMIT 4096U
#define HWA_INFERENCE_SETTINGS_LIMIT 65536U
#define HWA_INFERENCE_SETTINGS_DEPTH_LIMIT 64U

typedef struct HWAFixedInferenceTask {
    HWAInferenceOutput output;
    HWAEventBundle bundle;
    unsigned poll_count;
} HWAFixedInferenceTask;

typedef struct HWAInferencePayloadIndex {
    const char *relative_path;
    const HWAInferencePayload *payload;
} HWAInferencePayloadIndex;

typedef struct HWAInferenceJson {
    const unsigned char *data;
    size_t size;
    size_t offset;
    size_t depth;
} HWAInferenceJson;

static int hwa_inference_format_equal(const HWAFormat *left,
                                      const HWAFormat *right);

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
        if (value == 0U) return 0;
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

static int hwa_inference_bounded_text(const char *text,
                                      size_t limit,
                                      size_t *length)
{
    size_t index;
    if (text == NULL || limit == 0U) return 0;
    for (index = 0U; index < limit; ++index) {
        if (text[index] == '\0') {
            if (length != NULL) *length = index;
            return 1;
        }
    }
    return 0;
}

static size_t hwa_inference_utf8_size(const unsigned char *text,
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

static int hwa_inference_utf8_text(const char *text, size_t limit)
{
    const unsigned char *cursor = (const unsigned char *)text;
    size_t remaining;
    if (!hwa_inference_bounded_text(text, limit, &remaining)) return 0;
    while (remaining != 0U) {
        size_t count = hwa_inference_utf8_size(cursor, remaining);
        if (count == 0U) return 0;
        cursor += count;
        remaining -= count;
    }
    return 1;
}

static int hwa_inference_ascii_name(const char *text)
{
    size_t length;
    size_t index;
    if (!hwa_inference_bounded_text(text, HWA_INFERENCE_TEXT_LIMIT,
                                    &length) || length == 0U)
        return 0;
    for (index = 0U; index < length; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (!((value >= (unsigned char)'a' &&
               value <= (unsigned char)'z') ||
              (value >= (unsigned char)'A' &&
               value <= (unsigned char)'Z') ||
              (value >= (unsigned char)'0' &&
               value <= (unsigned char)'9') ||
              value == (unsigned char)'.' || value == (unsigned char)'_' ||
              value == (unsigned char)'-'))
            return 0;
    }
    return 1;
}

static int hwa_inference_media_type(const char *text)
{
    size_t length;
    size_t index;
    size_t slash_count = 0U;
    if (!hwa_inference_bounded_text(text, HWA_INFERENCE_TEXT_LIMIT,
                                    &length) || length < 3U)
        return 0;
    for (index = 0U; index < length; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (value == (unsigned char)'/') {
            if (index == 0U || index + 1U == length) return 0;
            slash_count++;
        } else if (!((value >= (unsigned char)'a' &&
                      value <= (unsigned char)'z') ||
                     (value >= (unsigned char)'A' &&
                      value <= (unsigned char)'Z') ||
                     (value >= (unsigned char)'0' &&
                      value <= (unsigned char)'9') ||
                     value == (unsigned char)'!' ||
                     value == (unsigned char)'#' ||
                     value == (unsigned char)'$' ||
                     value == (unsigned char)'&' ||
                     value == (unsigned char)'^' ||
                     value == (unsigned char)'_' ||
                     value == (unsigned char)'.' ||
                     value == (unsigned char)'+' ||
                     value == (unsigned char)'-')) {
            return 0;
        }
    }
    return slash_count == 1U;
}

static void hwa_inference_json_space_value(HWAInferenceJson *json)
{
    while (json->offset < json->size) {
        unsigned char value = json->data[json->offset];
        if (value != (unsigned char)' ' && value != (unsigned char)'\t' &&
            value != (unsigned char)'\r' && value != (unsigned char)'\n')
            break;
        json->offset++;
    }
}

static int hwa_inference_json_hex4(HWAInferenceJson *json, unsigned *code)
{
    size_t digit;
    unsigned value = 0U;
    if (json->size - json->offset < 4U || code == NULL) return 0;
    for (digit = 0U; digit < 4U; ++digit) {
        unsigned char hex = json->data[json->offset++];
        unsigned nibble;
        if (hex >= (unsigned char)'0' && hex <= (unsigned char)'9')
            nibble = (unsigned)(hex - (unsigned char)'0');
        else if (hex >= (unsigned char)'a' && hex <= (unsigned char)'f')
            nibble = 10U + (unsigned)(hex - (unsigned char)'a');
        else if (hex >= (unsigned char)'A' && hex <= (unsigned char)'F')
            nibble = 10U + (unsigned)(hex - (unsigned char)'A');
        else
            return 0;
        value = (value << 4U) | nibble;
    }
    *code = value;
    return 1;
}

static int hwa_inference_json_string_value(HWAInferenceJson *json)
{
    if (json->offset >= json->size ||
        json->data[json->offset] != (unsigned char)'"')
        return 0;
    json->offset++;
    while (json->offset < json->size) {
        unsigned char value = json->data[json->offset++];
        if (value == (unsigned char)'"') return 1;
        if (value < 0x20U) return 0;
        if (value >= 0x80U) {
            size_t sequence = hwa_inference_utf8_size(
                json->data + json->offset - 1U,
                json->size - json->offset + 1U);
            if (sequence == 0U) return 0;
            json->offset += sequence - 1U;
            continue;
        }
        if (value == (unsigned char)'\\') {
            if (json->offset >= json->size) return 0;
            value = json->data[json->offset++];
            if (value == (unsigned char)'u') {
                unsigned code;
                if (!hwa_inference_json_hex4(json, &code) || code == 0U)
                    return 0;
                if (code >= 0xd800U && code <= 0xdbffU) {
                    unsigned low;
                    if (json->size - json->offset < 6U ||
                        json->data[json->offset] != (unsigned char)'\\' ||
                        json->data[json->offset + 1U] != (unsigned char)'u')
                        return 0;
                    json->offset += 2U;
                    if (!hwa_inference_json_hex4(json, &low) ||
                        low < 0xdc00U || low > 0xdfffU)
                        return 0;
                } else if (code >= 0xdc00U && code <= 0xdfffU) {
                    return 0;
                }
            } else if (value != (unsigned char)'"' &&
                       value != (unsigned char)'\\' &&
                       value != (unsigned char)'/' &&
                       value != (unsigned char)'b' &&
                       value != (unsigned char)'f' &&
                       value != (unsigned char)'n' &&
                       value != (unsigned char)'r' &&
                       value != (unsigned char)'t') {
                return 0;
            }
        }
    }
    return 0;
}

static int hwa_inference_json_value(HWAInferenceJson *json);

static int hwa_inference_json_array(HWAInferenceJson *json)
{
    if (json->depth >= HWA_INFERENCE_SETTINGS_DEPTH_LIMIT) return 0;
    json->depth++;
    json->offset++;
    hwa_inference_json_space_value(json);
    if (json->offset < json->size &&
        json->data[json->offset] == (unsigned char)']') {
        json->offset++;
        json->depth--;
        return 1;
    }
    for (;;) {
        if (!hwa_inference_json_value(json)) return 0;
        hwa_inference_json_space_value(json);
        if (json->offset >= json->size) return 0;
        if (json->data[json->offset] == (unsigned char)']') {
            json->offset++;
            json->depth--;
            return 1;
        }
        if (json->data[json->offset] != (unsigned char)',') return 0;
        json->offset++;
    }
}

static int hwa_inference_json_object(HWAInferenceJson *json)
{
    if (json->depth >= HWA_INFERENCE_SETTINGS_DEPTH_LIMIT) return 0;
    json->depth++;
    json->offset++;
    hwa_inference_json_space_value(json);
    if (json->offset < json->size &&
        json->data[json->offset] == (unsigned char)'}') {
        json->offset++;
        json->depth--;
        return 1;
    }
    for (;;) {
        hwa_inference_json_space_value(json);
        if (!hwa_inference_json_string_value(json)) return 0;
        hwa_inference_json_space_value(json);
        if (json->offset >= json->size ||
            json->data[json->offset] != (unsigned char)':')
            return 0;
        json->offset++;
        if (!hwa_inference_json_value(json)) return 0;
        hwa_inference_json_space_value(json);
        if (json->offset >= json->size) return 0;
        if (json->data[json->offset] == (unsigned char)'}') {
            json->offset++;
            json->depth--;
            return 1;
        }
        if (json->data[json->offset] != (unsigned char)',') return 0;
        json->offset++;
    }
}

static int hwa_inference_json_number(HWAInferenceJson *json)
{
    size_t offset = json->offset;
    if (offset < json->size && json->data[offset] == (unsigned char)'-')
        offset++;
    if (offset >= json->size) return 0;
    if (json->data[offset] == (unsigned char)'0') {
        offset++;
        if (offset < json->size && json->data[offset] >= (unsigned char)'0' &&
            json->data[offset] <= (unsigned char)'9')
            return 0;
    } else {
        if (json->data[offset] < (unsigned char)'1' ||
            json->data[offset] > (unsigned char)'9')
            return 0;
        do {
            offset++;
        } while (offset < json->size &&
                 json->data[offset] >= (unsigned char)'0' &&
                 json->data[offset] <= (unsigned char)'9');
    }
    if (offset < json->size && json->data[offset] == (unsigned char)'.') {
        offset++;
        if (offset >= json->size ||
            json->data[offset] < (unsigned char)'0' ||
            json->data[offset] > (unsigned char)'9')
            return 0;
        do {
            offset++;
        } while (offset < json->size &&
                 json->data[offset] >= (unsigned char)'0' &&
                 json->data[offset] <= (unsigned char)'9');
    }
    if (offset < json->size &&
        (json->data[offset] == (unsigned char)'e' ||
         json->data[offset] == (unsigned char)'E')) {
        offset++;
        if (offset < json->size &&
            (json->data[offset] == (unsigned char)'+' ||
             json->data[offset] == (unsigned char)'-'))
            offset++;
        if (offset >= json->size ||
            json->data[offset] < (unsigned char)'0' ||
            json->data[offset] > (unsigned char)'9')
            return 0;
        do {
            offset++;
        } while (offset < json->size &&
                 json->data[offset] >= (unsigned char)'0' &&
                 json->data[offset] <= (unsigned char)'9');
    }
    json->offset = offset;
    return 1;
}

static int hwa_inference_json_literal(HWAInferenceJson *json,
                                      const char *literal,
                                      size_t size)
{
    if (json->size - json->offset < size ||
        memcmp(json->data + json->offset, literal, size) != 0)
        return 0;
    json->offset += size;
    return 1;
}

static int hwa_inference_json_value(HWAInferenceJson *json)
{
    hwa_inference_json_space_value(json);
    if (json->offset >= json->size) return 0;
    switch (json->data[json->offset]) {
    case (unsigned char)'{': return hwa_inference_json_object(json);
    case (unsigned char)'[': return hwa_inference_json_array(json);
    case (unsigned char)'"': return hwa_inference_json_string_value(json);
    case (unsigned char)'t':
        return hwa_inference_json_literal(json, "true", 4U);
    case (unsigned char)'f':
        return hwa_inference_json_literal(json, "false", 5U);
    case (unsigned char)'n':
        return hwa_inference_json_literal(json, "null", 4U);
    default: return hwa_inference_json_number(json);
    }
}

static int hwa_inference_settings_object(const char *text)
{
    HWAInferenceJson json;
    size_t size;
    if (!hwa_inference_bounded_text(text, HWA_INFERENCE_SETTINGS_LIMIT,
                                    &size) || size == 0U)
        return 0;
    memset(&json, 0, sizeof(json));
    json.data = (const unsigned char *)text;
    json.size = size;
    hwa_inference_json_space_value(&json);
    if (json.offset >= json.size ||
        json.data[json.offset] != (unsigned char)'{' ||
        !hwa_inference_json_object(&json))
        return 0;
    hwa_inference_json_space_value(&json);
    return json.offset == json.size;
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

static int hwa_inference_input_pointer_compare(const void *left_value,
                                               const void *right_value)
{
    const HWAInferenceInput *const *left =
        (const HWAInferenceInput *const *)left_value;
    const HWAInferenceInput *const *right =
        (const HWAInferenceInput *const *)right_value;
    return strcmp((*left)->id, (*right)->id);
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

static int hwa_inference_output_limits_valid(
    const HWAEventBundleLimits *limits)
{
    return limits != NULL && limits->max_manifest_bytes != 0U &&
           limits->max_index_bytes != 0U &&
           limits->max_payload_file_bytes != 0U &&
           limits->max_bundle_bytes != 0U && limits->max_work_bytes != 0U &&
           limits->max_audio_files != 0U && limits->max_events != 0U &&
           limits->max_values != 0U && limits->max_traces != 0U &&
           limits->max_trace_refs != 0U && limits->max_providers != 0U &&
           limits->max_warnings != 0U &&
           limits->max_nesting_depth != 0U &&
           limits->max_json_depth != 0U &&
           limits->max_json_tokens != 0U;
}

static unsigned hwa_inference_bit_count32(uint32_t value)
{
    unsigned count = 0U;
    while (value != 0U) {
        count += value & 1U;
        value >>= 1U;
    }
    return count;
}

static int hwa_inference_source_format_valid(const HWAFormat *format,
                                             uint64_t source_bytes)
{
    uint64_t expected_align;
    if (format == NULL ||
        (format->container != HWA_CONTAINER_RIFF &&
         format->container != HWA_CONTAINER_RF64) ||
        (format->encoding != HWA_ENCODING_PCM &&
         format->encoding != HWA_ENCODING_IEEE_FLOAT) ||
        format->channels == 0U || format->channels > 1024U ||
        format->sample_rate_hz == 0U || format->bits_per_sample == 0U ||
        (format->bits_per_sample & 7U) != 0U ||
        format->valid_bits_per_sample == 0U ||
        format->valid_bits_per_sample > format->bits_per_sample ||
        format->block_align == 0U ||
        format->frames > HWA_INFERENCE_JSON_SAFE_INTEGER ||
        format->data_bytes > HWA_INFERENCE_JSON_SAFE_INTEGER ||
        format->data_bytes > source_bytes ||
        format->data_bytes % (uint64_t)format->block_align != 0U ||
        format->frames !=
            format->data_bytes / (uint64_t)format->block_align ||
        !isfinite(format->duration_seconds) ||
        format->duration_seconds < 0.0 ||
        format->duration_seconds !=
            (double)format->frames / (double)format->sample_rate_hz)
        return 0;
    if ((format->encoding == HWA_ENCODING_PCM &&
         format->bits_per_sample != 8U &&
         format->bits_per_sample != 16U &&
         format->bits_per_sample != 24U &&
         format->bits_per_sample != 32U) ||
        (format->encoding == HWA_ENCODING_IEEE_FLOAT &&
         format->bits_per_sample != 32U &&
         format->bits_per_sample != 64U) ||
        (format->encoding == HWA_ENCODING_IEEE_FLOAT &&
         format->valid_bits_per_sample != format->bits_per_sample) ||
        (format->channel_mask != 0U &&
         hwa_inference_bit_count32(format->channel_mask) !=
             (unsigned)format->channels))
        return 0;
    expected_align = (uint64_t)format->channels *
                     ((uint64_t)format->bits_per_sample / 8U);
    if (expected_align > UINT16_MAX ||
        format->block_align != (uint16_t)expected_align ||
        expected_align > UINT32_MAX / (uint64_t)format->sample_rate_hz)
        return 0;
    return 1;
}

static const HWAInferenceInput *hwa_inference_request_source(
    const HWAInferenceRequest *request)
{
    size_t index;
    if (request == NULL || request->inputs == NULL ||
        request->source_input_id == NULL)
        return NULL;
    for (index = 0U; index < request->input_count; ++index) {
        if (strcmp(request->inputs[index].id,
                   request->source_input_id) == 0)
            return &request->inputs[index];
    }
    return NULL;
}

int hwa_inference_request_validate(const HWAInferenceProvider *provider,
                                   const HWAInferenceRequest *request,
                                   char *error,
                                   size_t error_size)
{
    const HWAInferenceInput *source = NULL;
    const HWAInferenceInput **input_index = NULL;
    uint64_t total_bytes = 0U;
    size_t source_rows = 0U;
    size_t selected_rows = 0U;
    size_t index;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (provider == NULL ||
        !hwa_inference_utf8_text(provider->name,
                                 HWA_INFERENCE_TEXT_LIMIT) ||
        provider->name[0] == '\0' ||
        !hwa_inference_utf8_text(provider->version,
                                 HWA_INFERENCE_TEXT_LIMIT) ||
        provider->version[0] == '\0' || provider->model_sha256 == NULL ||
        (provider->model_sha256[0] != '\0' &&
         !hwa_inference_lower_sha256(provider->model_sha256)) ||
        provider->start == NULL || provider->poll == NULL ||
        provider->task_free == NULL || provider->destroy == NULL) {
        hwa_inference_error(error, error_size,
                            "invalid inference provider descriptor");
        return -1;
    }
    if (request == NULL ||
        !hwa_inference_bounded_text(request->task,
                                    HWA_INFERENCE_TEXT_LIMIT, NULL) ||
        request->task[0] == '\0' ||
        !hwa_inference_settings_object(request->settings_json) ||
        !hwa_inference_bounded_text(request->expected_provider_name,
                                    HWA_INFERENCE_TEXT_LIMIT, NULL) ||
        request->expected_provider_name[0] == '\0' ||
        !hwa_inference_bounded_text(request->expected_provider_version,
                                    HWA_INFERENCE_TEXT_LIMIT, NULL) ||
        request->expected_provider_version[0] == '\0' ||
        request->expected_model_sha256 == NULL ||
        (request->expected_model_sha256[0] != '\0' &&
         !hwa_inference_lower_sha256(request->expected_model_sha256)) ||
        !hwa_inference_ascii_name(request->source_input_id) ||
        request->source_recording_id == 0U ||
        request->source_recording_id > HWA_INFERENCE_JSON_SAFE_INTEGER ||
        request->inputs == NULL || request->input_count == 0U ||
        request->input_count > HWA_INFERENCE_MAX_INPUTS) {
        hwa_inference_error(error, error_size,
                            "invalid inference request shape");
        return -1;
    }
    if (strcmp(request->expected_provider_name, provider->name) != 0 ||
        strcmp(request->expected_provider_version, provider->version) != 0 ||
        strcmp(request->expected_model_sha256,
               provider->model_sha256) != 0) {
        hwa_inference_error(error, error_size,
                            "inference provider identity mismatch");
        return -1;
    }
    if (request->max_input_file_bytes == 0U ||
        request->max_input_bytes == 0U ||
        request->max_input_file_bytes > request->max_input_bytes ||
        request->timeout_milliseconds == 0U ||
        !hwa_inference_output_limits_valid(&request->output_limits)) {
        hwa_inference_error(error, error_size,
                            "invalid inference request limits");
        return -1;
    }
    for (index = 0U; index < request->input_count; ++index) {
        const HWAInferenceInput *input = &request->inputs[index];
        if (!hwa_inference_ascii_name(input->id) ||
            !hwa_inference_ascii_name(input->role) ||
            !hwa_inference_media_type(input->media_type) ||
            !hwa_inference_lower_sha256(input->sha256) ||
            !hwa_inference_utf8_text(input->bytes.name,
                                     HWA_INFERENCE_TEXT_LIMIT) ||
            input->bytes.name[0] == '\0' || input->bytes.read_at == NULL ||
            input->bytes.size > HWA_INFERENCE_JSON_SAFE_INTEGER ||
            input->bytes.size > request->max_input_file_bytes) {
            hwa_inference_error(error, error_size,
                                "invalid or oversized inference input");
            return -1;
        }
        if (total_bytes > request->max_input_bytes - input->bytes.size) {
            hwa_inference_error(error, error_size,
                                "inference input bytes exceed total limit");
            return -1;
        }
        total_bytes += input->bytes.size;
        if (strcmp(input->role, "source-recording") == 0) source_rows++;
        if (strcmp(input->id, request->source_input_id) == 0) {
            selected_rows++;
            source = input;
        }
    }
    if (request->input_count > SIZE_MAX / sizeof(*input_index)) {
        hwa_inference_error(error, error_size,
                            "inference input index size overflows");
        return -1;
    }
    input_index = (const HWAInferenceInput **)calloc(
        request->input_count, sizeof(*input_index));
    if (input_index == NULL) {
        hwa_inference_error(error, error_size,
                            "cannot allocate inference input index");
        return -1;
    }
    for (index = 0U; index < request->input_count; ++index)
        input_index[index] = &request->inputs[index];
    if (request->input_count > 1U)
        qsort(input_index, request->input_count, sizeof(*input_index),
              hwa_inference_input_pointer_compare);
    for (index = 1U; index < request->input_count; ++index) {
        if (strcmp(input_index[index - 1U]->id, input_index[index]->id) == 0) {
            free(input_index);
            hwa_inference_error(error, error_size,
                                "duplicate inference input id");
            return -1;
        }
    }
    free(input_index);
    if (selected_rows != 1U || source_rows != 1U || source == NULL ||
        strcmp(source->role, "source-recording") != 0 ||
        strcmp(source->media_type, "audio/wav") != 0 ||
        source->bytes.size == 0U ||
        !hwa_inference_source_format_valid(&request->source_format,
                                           source->bytes.size)) {
        hwa_inference_error(error, error_size,
                            "invalid inference source recording");
        return -1;
    }
    {
        HWAWavReader reader;
        if (hwa_wav_reader_open_source(
                &reader, &source->bytes, request->max_input_file_bytes,
                error, error_size) != 0)
            return -1;
        if (!hwa_inference_format_equal(&reader.format,
                                        &request->source_format)) {
            hwa_wav_reader_close(&reader);
            hwa_inference_error(error, error_size,
                                "inference source format does not match WAVE");
            return -1;
        }
        hwa_wav_reader_close(&reader);
    }
    for (index = 0U; index < request->input_count; ++index) {
        char actual_sha256[HWA_SHA256_HEX_SIZE];
        if (hwa_inference_byte_source_sha256(
                &request->inputs[index].bytes,
                request->max_input_file_bytes, actual_sha256,
                error, error_size) != 0)
            return -1;
        if (strcmp(actual_sha256, request->inputs[index].sha256) != 0) {
            hwa_inference_error(error, error_size,
                                "inference input hash is wrong");
            return -1;
        }
    }
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

static int hwa_inference_format_equal(const HWAFormat *left,
                                      const HWAFormat *right)
{
    return left->container == right->container &&
           left->encoding == right->encoding &&
           left->channels == right->channels &&
           left->sample_rate_hz == right->sample_rate_hz &&
           left->bits_per_sample == right->bits_per_sample &&
           left->valid_bits_per_sample == right->valid_bits_per_sample &&
           left->block_align == right->block_align &&
           left->channel_mask == right->channel_mask &&
           left->frames == right->frames &&
           left->data_bytes == right->data_bytes &&
           left->duration_seconds == right->duration_seconds;
}

int hwa_inference_output_validate_for_request(
    const HWAInferenceProvider *provider,
    const HWAInferenceRequest *request,
    const HWAInferenceOutput *output,
    char *error,
    size_t error_size)
{
    const HWAEventBundle *bundle;
    const HWAInferenceInput *source;
    uint64_t provider_id = 0U;
    size_t provider_matches = 0U;
    size_t source_rows = 0U;
    size_t row;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (request == NULL) {
        hwa_inference_error(error, error_size,
                            "invalid inference request output binding");
        return -1;
    }
    if (hwa_inference_output_validate(output, &request->output_limits,
                                      error, error_size) != 0)
        return -1;
    if (hwa_inference_request_validate(provider, request,
                                       error, error_size) != 0)
        return -1;
    bundle = output->bundle;
    source = hwa_inference_request_source(request);
    if (source == NULL) {
        hwa_inference_error(error, error_size,
                            "inference source input is missing");
        return -1;
    }
    for (row = 0U; row < bundle->provider_count; ++row) {
        const HWAEventProvider *candidate = &bundle->providers[row];
        if (strcmp(candidate->name, provider->name) == 0 &&
            strcmp(candidate->version, provider->version) == 0 &&
            strcmp(candidate->model_sha256,
                   provider->model_sha256) == 0) {
            provider_matches++;
            provider_id = candidate->id;
        }
    }
    if (provider_matches != 1U) {
        hwa_inference_error(error, error_size,
                            "inference output provider identity is not unique");
        return -1;
    }
    for (row = 0U; row < bundle->audio_count; ++row) {
        const HWAEventAudio *audio = &bundle->audio[row];
        if (audio->kind == HWA_EVENT_SOURCE_RECORDING) {
            source_rows++;
            if (audio->id != request->source_recording_id ||
                strcmp(audio->name, source->bytes.name) != 0 ||
                strcmp(audio->sha256, source->sha256) != 0 ||
                audio->file_bytes != source->bytes.size ||
                !hwa_inference_format_equal(&audio->format,
                                            &request->source_format)) {
                hwa_inference_error(
                    error, error_size,
                    "inference output source recording does not match request");
                return -1;
            }
        } else if (!audio->source_recording_id_valid ||
                   audio->source_recording_id !=
                       request->source_recording_id ||
                   audio->format.sample_rate_hz !=
                       request->source_format.sample_rate_hz ||
                   audio->format.frames != request->source_format.frames) {
            hwa_inference_error(error, error_size,
                                "inference output derived audio uses another clock");
            return -1;
        }
    }
    if (source_rows != 1U) {
        hwa_inference_error(error, error_size,
                            "inference output source recording is not unique");
        return -1;
    }
    for (row = 0U; row < bundle->trace_count; ++row) {
        if (bundle->traces[row].source_recording_id !=
            request->source_recording_id) {
            hwa_inference_error(error, error_size,
                                "inference output trace uses another clock");
            return -1;
        }
    }
    for (row = 0U; row < bundle->event_count; ++row) {
        const HWAPerformanceEvent *event = &bundle->events[row];
        size_t value_index;
        if (event->source_recording_id != request->source_recording_id) {
            hwa_inference_error(error, error_size,
                                "inference output event uses another clock");
            return -1;
        }
        for (value_index = 0U; value_index < event->value_count;
             ++value_index) {
            const HWAEventValue *value = &event->values[value_index];
            if (value->basis == HWA_EVENT_INFERENCE &&
                (!value->provider_id_valid ||
                 value->provider_id != provider_id)) {
                hwa_inference_error(
                    error, error_size,
                    "inference output value uses another provider");
                return -1;
            }
        }
    }
    return 0;
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

static int hwa_inference_fixed_request_valid(
    const HWAInferenceRequest *request,
    const HWAInferenceInput **source_result)
{
    const HWAInferenceInput *source;
    if (request == NULL || source_result == NULL ||
        strcmp(request->task, "org.hlolli.fixed-note") != 0 ||
        !hwa_inference_empty_settings(request->settings_json) ||
        request->source_format.frames < UINT64_C(192) ||
        request->source_format.frames > HWA_INFERENCE_JSON_SAFE_INTEGER) {
        return 0;
    }
    source = hwa_inference_request_source(request);
    if (source == NULL) return 0;
    *source_result = source;
    return 1;
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
    HWAInferenceProvider descriptor;
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
    hwa_inference_fixed_provider_init(&descriptor);
    if (hwa_inference_request_validate(&descriptor, request,
                                       error, error_size) != 0)
        return -1;
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
    if (hwa_inference_output_validate_for_request(
            &descriptor, request, &task->output,
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

#include "instrument_stem_provider.h"

#include "inference_clock.h"
#include "inference_provenance.h"
#include "internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HWA_INSTRUMENT_STEM_ID_BYTES 128U
#define HWA_INSTRUMENT_STEM_TEXT_BYTES 4096U
#define HWA_INSTRUMENT_STEM_PROVENANCE_BYTES 65536U
#define HWA_INSTRUMENT_STEM_JSON_SAFE_INTEGER \
    UINT64_C(9007199254740991)
#define HWA_INSTRUMENT_STEM_PROVIDER_ID UINT64_C(1)

typedef struct HWAInstrumentStemProviderContext {
    char model_sha256[HWA_SHA256_HEX_SIZE];
    char adapter_sha256[HWA_SHA256_HEX_SIZE];
    char *runtime_name;
    char *runtime_version;
    char *backend;
    char *fallback;
    uint64_t max_work_bytes;
    void *runner_context;
    HWAInstrumentStemRunFunction run;
    HWAInstrumentStemResultsDestroyFunction results_destroy;
    HWAInstrumentStemRunnerDestroyFunction runner_destroy;
} HWAInstrumentStemProviderContext;

typedef struct HWAInstrumentStemTask {
    HWAInferenceOutput output;
    HWAEventBundle bundle;
    HWAInferencePayload *payloads;
    HWAInstrumentStemResult *runner_stems;
    size_t runner_stem_count;
    void *runner_context;
    HWAInstrumentStemResultsDestroyFunction results_destroy;
} HWAInstrumentStemTask;

static int hwa_instrument_stem_provider_start(
    void *context,
    const HWAInferenceRequest *request,
    void **task,
    char *error,
    size_t error_size);
static int hwa_instrument_stem_provider_poll(
    void *context,
    void *task,
    HWAInferencePollState *state,
    const HWAInferenceOutput **output,
    char *error,
    size_t error_size);
static void hwa_instrument_stem_provider_task_free(void *context, void *task);
static void hwa_instrument_stem_provider_destroy(void *context);

static void hwa_instrument_stem_error(char *error,
                                      size_t error_size,
                                      const char *message)
{
    if (error == NULL || error_size == 0U) return;
    (void)snprintf(error, error_size, "%s", message);
    error[error_size - 1U] = '\0';
}

static int hwa_instrument_stem_text_size(const char *text,
                                         size_t maximum,
                                         int allow_empty,
                                         size_t *size)
{
    size_t index;
    if (text == NULL || maximum == 0U) return -1;
    for (index = 0U; index < maximum; ++index) {
        if (text[index] == '\0') {
            if (!allow_empty && index == 0U) return -1;
            if (size != NULL) *size = index;
            return 0;
        }
    }
    return -1;
}

static size_t hwa_instrument_stem_utf8_size(const unsigned char *text,
                                            size_t remaining)
{
    unsigned char first;
    if (text == NULL || remaining == 0U) return 0U;
    first = text[0];
    if (first < 0x80U) return first >= 0x20U || first == '\t' ? 1U : 0U;
    if (first >= 0xc2U && first <= 0xdfU)
        return remaining >= 2U && text[1] >= 0x80U && text[1] <= 0xbfU
                   ? 2U : 0U;
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

static int hwa_instrument_stem_utf8_text(const char *text,
                                         int allow_empty,
                                         size_t *size)
{
    const unsigned char *cursor = (const unsigned char *)text;
    size_t remaining;
    if (hwa_instrument_stem_text_size(
            text, HWA_INSTRUMENT_STEM_TEXT_BYTES,
            allow_empty, &remaining) != 0)
        return 0;
    if (size != NULL) *size = remaining;
    while (remaining != 0U) {
        size_t count = hwa_instrument_stem_utf8_size(cursor, remaining);
        if (count == 0U) return 0;
        cursor += count;
        remaining -= count;
    }
    return 1;
}

static int hwa_instrument_stem_id_valid(const char *text, size_t *size)
{
    size_t length;
    size_t index;
    if (hwa_instrument_stem_text_size(
            text, HWA_INSTRUMENT_STEM_ID_BYTES, 0, &length) != 0 ||
        text[0] < 'a' || text[0] > 'z')
        return 0;
    for (index = 1U; index < length; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (!((value >= (unsigned char)'a' &&
               value <= (unsigned char)'z') ||
              (value >= (unsigned char)'0' &&
               value <= (unsigned char)'9') ||
              value == (unsigned char)'-' || value == (unsigned char)'_'))
            return 0;
    }
    if (size != NULL) *size = length;
    return 1;
}

static int hwa_instrument_stem_lower_sha256(const char *text,
                                            int allow_empty)
{
    size_t index;
    if (text == NULL) return 0;
    if (allow_empty && text[0] == '\0') return 1;
    for (index = 0U; index < 64U; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (!((value >= (unsigned char)'0' &&
               value <= (unsigned char)'9') ||
              (value >= (unsigned char)'a' &&
               value <= (unsigned char)'f')))
            return 0;
    }
    return text[64] == '\0';
}

static int hwa_instrument_stem_empty_settings(const char *text)
{
    if (text == NULL) return 0;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;
    if (*text++ != '{') return 0;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;
    if (*text++ != '}') return 0;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;
    return *text == '\0';
}

static char *hwa_instrument_stem_copy_text(const char *text, int allow_empty)
{
    size_t size;
    char *copy;
    if (!hwa_instrument_stem_utf8_text(text, allow_empty, &size) ||
        size == SIZE_MAX)
        return NULL;
    copy = (char *)malloc(size + 1U);
    if (copy != NULL) memcpy(copy, text, size + 1U);
    return copy;
}

static int hwa_instrument_stem_work_add(uint64_t *total,
                                        size_t count,
                                        size_t item_size,
                                        uint64_t maximum)
{
    size_t bytes;
    if (total == NULL || (item_size != 0U && count > SIZE_MAX / item_size))
        return -1;
    bytes = count * item_size;
    if ((uintmax_t)bytes > UINT64_MAX || *total > maximum ||
        (uint64_t)bytes > maximum - *total)
        return -1;
    *total += (uint64_t)bytes;
    return 0;
}

static int hwa_instrument_stem_work_add_text(uint64_t *total,
                                             const char *text,
                                             int allow_empty,
                                             uint64_t maximum)
{
    size_t size;
    if (!hwa_instrument_stem_utf8_text(text, allow_empty, &size) ||
        size == SIZE_MAX)
        return -1;
    return hwa_instrument_stem_work_add(total, size + 1U, 1U, maximum);
}

static void hwa_instrument_stem_descriptor(
    HWAInstrumentStemProviderContext *context,
    HWAInferenceProvider *provider)
{
    memset(provider, 0, sizeof(*provider));
    provider->name = HWA_INSTRUMENT_STEM_PROVIDER_NAME;
    provider->version = HWA_INSTRUMENT_STEM_PROVIDER_VERSION;
    provider->model_sha256 = context->model_sha256;
    provider->context = context;
    provider->start = hwa_instrument_stem_provider_start;
    provider->poll = hwa_instrument_stem_provider_poll;
    provider->task_free = hwa_instrument_stem_provider_task_free;
    provider->destroy = hwa_instrument_stem_provider_destroy;
}

static const HWAInferenceInput *hwa_instrument_stem_source(
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

static int hwa_instrument_stem_pointer_compare(const void *left_value,
                                               const void *right_value)
{
    const HWAInstrumentStemResult *const *left =
        (const HWAInstrumentStemResult *const *)left_value;
    const HWAInstrumentStemResult *const *right =
        (const HWAInstrumentStemResult *const *)right_value;
    return strcmp((*left)->stem_id, (*right)->stem_id);
}

static uint64_t hwa_instrument_stem_audio_id(uint64_t source_id,
                                             size_t sorted_index)
{
    uint64_t id = (uint64_t)sorted_index + UINT64_C(1);
    if (id >= source_id) id++;
    return id;
}

typedef struct HWAInstrumentStemProvenanceJson {
    const unsigned char *data;
    size_t size;
    size_t offset;
} HWAInstrumentStemProvenanceJson;

static int hwa_instrument_stem_json_literal(
    HWAInstrumentStemProvenanceJson *json,
    const char *literal)
{
    size_t size = strlen(literal);
    if (json == NULL || json->offset > json->size ||
        size > json->size - json->offset ||
        memcmp(json->data + json->offset, literal, size) != 0)
        return -1;
    json->offset += size;
    return 0;
}

static int hwa_instrument_stem_json_lower_hex(unsigned char value)
{
    if (value >= (unsigned char)'0' && value <= (unsigned char)'9')
        return (int)(value - (unsigned char)'0');
    if (value >= (unsigned char)'a' && value <= (unsigned char)'f')
        return 10 + (int)(value - (unsigned char)'a');
    return -1;
}

static int hwa_instrument_stem_json_string_byte(
    const char *expected,
    size_t *expected_offset,
    size_t *decoded_size,
    unsigned char value)
{
    if (value == 0U || (value < 0x20U && value != (unsigned char)'\t') ||
        *decoded_size >= HWA_INSTRUMENT_STEM_TEXT_BYTES - 1U)
        return -1;
    if (expected != NULL &&
        (unsigned char)expected[*expected_offset] != value)
        return -1;
    if (expected != NULL) (*expected_offset)++;
    (*decoded_size)++;
    return 0;
}

/* Accept only the string spelling produced by inference_provenance.c. */
static int hwa_instrument_stem_json_string(
    HWAInstrumentStemProvenanceJson *json,
    const char *expected,
    int allow_empty)
{
    size_t expected_offset = 0U;
    size_t decoded_size = 0U;
    if (json == NULL || json->offset >= json->size ||
        json->data[json->offset++] != (unsigned char)'\"')
        return -1;
    while (json->offset < json->size &&
           json->data[json->offset] != (unsigned char)'\"') {
        unsigned char value = json->data[json->offset++];
        if (value == (unsigned char)'\\') {
            if (json->offset >= json->size) return -1;
            value = json->data[json->offset++];
            if (value == (unsigned char)'\"' ||
                value == (unsigned char)'\\') {
                if (hwa_instrument_stem_json_string_byte(
                        expected, &expected_offset, &decoded_size,
                        value) != 0)
                    return -1;
            } else if (value == (unsigned char)'u') {
                int high;
                int low;
                if (json->offset + 4U > json->size ||
                    json->data[json->offset] != (unsigned char)'0' ||
                    json->data[json->offset + 1U] != (unsigned char)'0')
                    return -1;
                high = hwa_instrument_stem_json_lower_hex(
                    json->data[json->offset + 2U]);
                low = hwa_instrument_stem_json_lower_hex(
                    json->data[json->offset + 3U]);
                if (high < 0 || low < 0 ||
                    (unsigned)(high * 16 + low) >= 0x20U)
                    return -1;
                json->offset += 4U;
                if (hwa_instrument_stem_json_string_byte(
                        expected, &expected_offset, &decoded_size,
                        (unsigned char)(high * 16 + low)) != 0)
                    return -1;
            } else {
                return -1;
            }
        } else if (value >= 0x80U) {
            size_t count = hwa_instrument_stem_utf8_size(
                json->data + json->offset - 1U,
                json->size - json->offset + 1U);
            size_t index;
            if (count == 0U || count - 1U > json->size - json->offset)
                return -1;
            for (index = 0U; index < count; ++index) {
                if (hwa_instrument_stem_json_string_byte(
                        expected, &expected_offset, &decoded_size,
                        json->data[json->offset - 1U + index]) != 0)
                    return -1;
            }
            json->offset += count - 1U;
        } else {
            if (value < 0x20U ||
                hwa_instrument_stem_json_string_byte(
                    expected, &expected_offset, &decoded_size,
                    value) != 0)
                return -1;
        }
    }
    if (json->offset >= json->size ||
        json->data[json->offset++] != (unsigned char)'\"' ||
        (!allow_empty && decoded_size == 0U) ||
        (expected != NULL && expected[expected_offset] != '\0'))
        return -1;
    return 0;
}

static int hwa_instrument_stem_json_ascii_name(
    HWAInstrumentStemProvenanceJson *json)
{
    size_t size = 0U;
    if (json == NULL || json->offset >= json->size ||
        json->data[json->offset++] != (unsigned char)'\"')
        return -1;
    while (json->offset < json->size &&
           json->data[json->offset] != (unsigned char)'\"') {
        unsigned char value = json->data[json->offset++];
        if (!((value >= (unsigned char)'a' &&
               value <= (unsigned char)'z') ||
              (value >= (unsigned char)'A' &&
               value <= (unsigned char)'Z') ||
              (value >= (unsigned char)'0' &&
               value <= (unsigned char)'9') ||
              value == (unsigned char)'.' ||
              value == (unsigned char)'_' ||
              value == (unsigned char)'-') ||
            size >= HWA_INSTRUMENT_STEM_TEXT_BYTES - 1U)
            return -1;
        size++;
    }
    if (size == 0U || json->offset >= json->size ||
        json->data[json->offset++] != (unsigned char)'\"')
        return -1;
    return 0;
}

static int hwa_instrument_stem_json_fixed_u64(
    HWAInstrumentStemProvenanceJson *json,
    uint64_t *result)
{
    uint64_t value = 0U;
    size_t index;
    if (json == NULL || result == NULL || json->offset >= json->size ||
        json->data[json->offset++] != (unsigned char)'\"' ||
        21U > json->size - json->offset)
        return -1;
    for (index = 0U; index < 20U; ++index) {
        unsigned char digit = json->data[json->offset++];
        if (digit < (unsigned char)'0' || digit > (unsigned char)'9' ||
            value > (UINT64_MAX - (uint64_t)(digit - (unsigned char)'0')) /
                        UINT64_C(10))
            return -1;
        value = value * UINT64_C(10) +
                (uint64_t)(digit - (unsigned char)'0');
    }
    if (json->data[json->offset++] != (unsigned char)'\"') return -1;
    *result = value;
    return 0;
}

static int hwa_instrument_stem_json_sha256(
    HWAInstrumentStemProvenanceJson *json,
    const char *expected)
{
    size_t index;
    if (json == NULL || json->offset >= json->size ||
        json->data[json->offset++] != (unsigned char)'\"' ||
        65U > json->size - json->offset)
        return -1;
    for (index = 0U; index < 64U; ++index) {
        unsigned char value = json->data[json->offset++];
        if (hwa_instrument_stem_json_lower_hex(value) < 0 ||
            (expected != NULL &&
             (unsigned char)expected[index] != value))
            return -1;
    }
    if (json->data[json->offset++] != (unsigned char)'\"' ||
        (expected != NULL && expected[64] != '\0'))
        return -1;
    return 0;
}

static int hwa_instrument_stem_provenance_valid_v1(
    const char *settings_json,
    const HWAEventAudio *source)
{
    HWAInstrumentStemProvenanceJson json;
    uint64_t seed;
    uint64_t input_bytes;
    size_t size;
    if (source == NULL || source->name == NULL || settings_json == NULL ||
        hwa_instrument_stem_text_size(
            settings_json, HWA_INSTRUMENT_STEM_PROVENANCE_BYTES + 1U,
            0, &size) != 0)
        return 0;
    json.data = (const unsigned char *)settings_json;
    json.size = size;
    json.offset = 0U;
    if (hwa_instrument_stem_json_literal(&json, "{\"task\":") != 0 ||
        hwa_instrument_stem_json_string(
            &json, HWA_INSTRUMENT_STEM_TASK_NAME, 0) != 0 ||
        hwa_instrument_stem_json_literal(&json, ",\"seed\":") != 0 ||
        hwa_instrument_stem_json_fixed_u64(&json, &seed) != 0 ||
        hwa_instrument_stem_json_literal(
            &json, ",\"inputs\":[{\"id\":") != 0 ||
        hwa_instrument_stem_json_ascii_name(&json) != 0 ||
        hwa_instrument_stem_json_literal(&json, ",\"role\":") != 0 ||
        hwa_instrument_stem_json_string(
            &json, "source-recording", 0) != 0 ||
        hwa_instrument_stem_json_literal(&json, ",\"media_type\":") != 0 ||
        hwa_instrument_stem_json_string(&json, "audio/wav", 0) != 0 ||
        hwa_instrument_stem_json_literal(&json, ",\"name\":") != 0 ||
        hwa_instrument_stem_json_string(&json, source->name, 0) != 0 ||
        hwa_instrument_stem_json_literal(&json, ",\"bytes\":") != 0 ||
        hwa_instrument_stem_json_fixed_u64(&json, &input_bytes) != 0 ||
        input_bytes > HWA_INSTRUMENT_STEM_JSON_SAFE_INTEGER ||
        input_bytes != source->file_bytes ||
        hwa_instrument_stem_json_literal(&json, ",\"sha256\":") != 0 ||
        hwa_instrument_stem_json_sha256(&json, source->sha256) != 0 ||
        hwa_instrument_stem_json_literal(
            &json, "}],\"runtime\":{\"name\":") != 0 ||
        hwa_instrument_stem_json_string(&json, NULL, 0) != 0 ||
        hwa_instrument_stem_json_literal(&json, ",\"version\":") != 0 ||
        hwa_instrument_stem_json_string(&json, NULL, 0) != 0 ||
        hwa_instrument_stem_json_literal(&json, ",\"backend\":") != 0 ||
        hwa_instrument_stem_json_string(&json, NULL, 0) != 0 ||
        hwa_instrument_stem_json_literal(&json, ",\"fallback\":") != 0 ||
        hwa_instrument_stem_json_string(&json, NULL, 1) != 0 ||
        hwa_instrument_stem_json_literal(
            &json, ",\"adapter_sha256\":") != 0 ||
        hwa_instrument_stem_json_sha256(&json, NULL) != 0 ||
        hwa_instrument_stem_json_literal(
            &json, "},\"task_settings\":{}}") != 0 ||
        json.offset != json.size)
        return 0;
    (void)seed;
    return 1;
}

int hwa_instrument_stem_bundle_validate_v1(
    const HWAEventBundle *bundle,
    char *error,
    size_t error_size)
{
    const HWAEventProvider *provider;
    const HWAEventAudio *source;
    size_t index;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (bundle == NULL || bundle->providers == NULL ||
        bundle->audio == NULL || bundle->events == NULL ||
        bundle->provider_count != 1U ||
        bundle->audio_count < 2U ||
        bundle->event_count != bundle->audio_count - 1U ||
        bundle->trace_count != 0U || bundle->warning_count != 0U) {
        hwa_instrument_stem_error(
            error, error_size,
            "bundle is not canonical instrument-stems-v1 output");
        return -1;
    }
    provider = &bundle->providers[0];
    source = &bundle->audio[0];
    if (provider->name == NULL || provider->version == NULL ||
        provider->settings_json == NULL || source->name == NULL ||
        source->relative_path == NULL || source->path_hint == NULL ||
        provider->id != HWA_INSTRUMENT_STEM_PROVIDER_ID ||
        strcmp(provider->name, HWA_INSTRUMENT_STEM_PROVIDER_NAME) != 0 ||
        strcmp(provider->version, HWA_INSTRUMENT_STEM_PROVIDER_VERSION) != 0 ||
        !hwa_instrument_stem_provenance_valid_v1(
            provider->settings_json, source) ||
        source->kind != HWA_EVENT_SOURCE_RECORDING ||
        source->relative_path[0] != '\0' || source->path_hint[0] != '\0' ||
        source->source_recording_id_valid || source->format.frames == 0U) {
        hwa_instrument_stem_error(
            error, error_size,
            "bundle is not canonical instrument-stems-v1 output");
        return -1;
    }
    for (index = 0U; index + 1U < bundle->audio_count; ++index) {
        const HWAEventAudio *audio = &bundle->audio[index + 1U];
        const HWAPerformanceEvent *event = &bundle->events[index];
        const HWAEventValue *value;
        char expected_path[HWA_INSTRUMENT_STEM_ID_BYTES + sizeof("audio/.wav")];
        int written;
        if (!hwa_instrument_stem_id_valid(audio->name, NULL)) goto invalid;
        written = snprintf(expected_path, sizeof(expected_path),
                           "audio/%s.wav", audio->name);
        if (written < 0 || (size_t)written >= sizeof(expected_path) ||
            audio->id != hwa_instrument_stem_audio_id(source->id, index) ||
            audio->kind != HWA_EVENT_INSTRUMENT_STEM ||
            audio->relative_path == NULL ||
            strcmp(audio->relative_path, expected_path) != 0 ||
            audio->path_hint == NULL || audio->path_hint[0] != '\0' ||
            !audio->source_recording_id_valid ||
            audio->source_recording_id != source->id ||
            audio->format.sample_rate_hz != source->format.sample_rate_hz ||
            audio->format.frames != source->format.frames ||
            (index != 0U &&
             strcmp(bundle->audio[index].name, audio->name) >= 0) ||
            event->id != (uint64_t)index + UINT64_C(1) ||
            strcmp(event->kind, "instrument-region") != 0 ||
            event->source_recording_id != source->id ||
            !event->evidence_audio_id_valid ||
            event->evidence_audio_id != audio->id || event->parent_id_valid ||
            event->start_sample != 0U ||
            event->end_sample != source->format.frames ||
            event->voice == NULL || event->voice[0] != '\0' ||
            event->part == NULL || strcmp(event->part, audio->name) != 0 ||
            event->score_event_id == NULL ||
            event->score_event_id[0] != '\0' || event->value_count != 1U ||
            event->trace_ref_count != 0U)
            goto invalid;
        value = &event->values[0];
        if (strcmp(value->name, "instrument") != 0 ||
            value->kind != HWA_EVENT_VALUE_TEXT ||
            value->basis != HWA_EVENT_INFERENCE ||
            !hwa_instrument_stem_utf8_text(value->text, 0, NULL) ||
            value->unit == NULL || value->unit[0] != '\0' ||
            (value->score_valid &&
             (!isfinite(value->score) || value->score < 0.0 ||
              value->score > 1.0)) ||
            (!value->score_valid && value->score != 0.0) ||
            !value->provider_id_valid ||
            value->provider_id != HWA_INSTRUMENT_STEM_PROVIDER_ID ||
            !value->selected)
            goto invalid;
    }
    return 0;

invalid:
    hwa_instrument_stem_error(
        error, error_size,
        "bundle is not canonical instrument-stems-v1 output");
    return -1;
}

static int hwa_instrument_stem_output_work(
    const HWAInferenceRequest *request,
    const HWAInferenceInput *source,
    HWAInstrumentStemResult *const *order,
    size_t stem_count,
    const char *settings_json,
    uint64_t maximum,
    char *error,
    size_t error_size)
{
    uint64_t work = 0U;
    size_t index;
    if (hwa_instrument_stem_work_add(
            &work, 1U, sizeof(HWAInstrumentStemTask), maximum) != 0 ||
        hwa_instrument_stem_work_add(
            &work, 1U, sizeof(HWAEventProvider), maximum) != 0 ||
        hwa_instrument_stem_work_add(
            &work, stem_count + 1U, sizeof(HWAEventAudio), maximum) != 0 ||
        hwa_instrument_stem_work_add(
            &work, stem_count, sizeof(HWAPerformanceEvent), maximum) != 0 ||
        hwa_instrument_stem_work_add(
            &work, stem_count, sizeof(HWAEventValue), maximum) != 0 ||
        hwa_instrument_stem_work_add(
            &work, stem_count, sizeof(HWAInferencePayload), maximum) != 0 ||
        hwa_instrument_stem_work_add(
            &work, stem_count, sizeof(*order), maximum) != 0 ||
        hwa_instrument_stem_work_add_text(
            &work, HWA_INSTRUMENT_STEM_PROVIDER_NAME, 0, maximum) != 0 ||
        hwa_instrument_stem_work_add_text(
            &work, HWA_INSTRUMENT_STEM_PROVIDER_VERSION, 0, maximum) != 0 ||
        hwa_instrument_stem_work_add_text(
            &work, source->bytes.name, 0, maximum) != 0 ||
        hwa_instrument_stem_work_add_text(
            &work, settings_json, 0, maximum) != 0 ||
        hwa_instrument_stem_work_add(&work, 2U, 1U, maximum) != 0) {
        hwa_instrument_stem_error(
            error, error_size, "instrument stem output exceeds its work limit");
        return -1;
    }
    for (index = 0U; index < stem_count; ++index) {
        const HWAInstrumentStemResult *stem = order[index];
        size_t stem_id_size;
        if (!hwa_instrument_stem_id_valid(stem->stem_id, &stem_id_size) ||
            hwa_instrument_stem_work_add(
                &work, stem_id_size + sizeof("audio/.wav"), 1U,
                maximum) != 0 ||
            hwa_instrument_stem_work_add(
                &work, stem_id_size + 1U, 1U, maximum) != 0 ||
            hwa_instrument_stem_work_add_text(
                &work, "", 1, maximum) != 0 ||
            hwa_instrument_stem_work_add_text(
                &work, "instrument-region", 0, maximum) != 0 ||
            hwa_instrument_stem_work_add_text(
                &work, "", 1, maximum) != 0 ||
            hwa_instrument_stem_work_add(
                &work, stem_id_size + 1U, 1U, maximum) != 0 ||
            hwa_instrument_stem_work_add_text(
                &work, "", 1, maximum) != 0 ||
            hwa_instrument_stem_work_add_text(
                &work, "instrument", 0, maximum) != 0 ||
            hwa_instrument_stem_work_add_text(
                &work, stem->instrument, 0, maximum) != 0 ||
            hwa_instrument_stem_work_add_text(
                &work, "", 1, maximum) != 0) {
            hwa_instrument_stem_error(
                error, error_size,
                "instrument stem output exceeds its work limit");
            return -1;
        }
    }
    (void)request;
    return 0;
}

static int hwa_instrument_stem_result_valid(
    const HWAInstrumentStemResult *stem,
    const HWAInferenceRequest *request,
    uint64_t deadline_started,
    HWAFormat *format,
    char *sha256,
    char *error,
    size_t error_size)
{
    HWAWavReader reader;
    int reader_open = 0;
    int status = -1;
    memset(&reader, 0, sizeof(reader));
    if (stem == NULL ||
        !hwa_instrument_stem_id_valid(stem->stem_id, NULL) ||
        !hwa_instrument_stem_utf8_text(stem->instrument, 0, NULL) ||
        (stem->score_valid != 0 && stem->score_valid != 1) ||
        (stem->score_valid &&
         (!isfinite(stem->score) || stem->score < 0.0 ||
          stem->score > 1.0)) ||
        (!stem->score_valid && stem->score != 0.0) ||
        !hwa_instrument_stem_utf8_text(stem->wave.name, 0, NULL) ||
        stem->wave.read_at == NULL || stem->wave.size == 0U ||
        stem->wave.size > request->output_limits.max_payload_file_bytes) {
        hwa_instrument_stem_error(
            error, error_size, "invalid instrument stem result");
        return -1;
    }
    if (hwa_inference_deadline_check(
            deadline_started, request->timeout_milliseconds,
            error, error_size) != 0)
        return -1;
    if (hwa_wav_reader_open_source(
            &reader, &stem->wave,
            request->output_limits.max_payload_file_bytes,
            error, error_size) != 0)
        return -1;
    reader_open = 1;
    if (reader.format.sample_rate_hz !=
            request->source_format.sample_rate_hz ||
        reader.format.frames != request->source_format.frames) {
        hwa_instrument_stem_error(
            error, error_size,
            "instrument stem WAVE uses another source clock");
        goto cleanup;
    }
    *format = reader.format;
    if (hwa_inference_deadline_check(
            deadline_started, request->timeout_milliseconds,
            error, error_size) != 0)
        goto cleanup;
    if (hwa_inference_byte_source_sha256(
            &stem->wave, request->output_limits.max_payload_file_bytes,
            sha256, error, error_size) != 0)
        goto cleanup;
    if (hwa_inference_deadline_check(
            deadline_started, request->timeout_milliseconds,
            error, error_size) != 0)
        goto cleanup;
    status = 0;
cleanup:
    if (reader_open) hwa_wav_reader_close(&reader);
    return status;
}

static int hwa_instrument_stem_build_task(
    HWAInstrumentStemProviderContext *context,
    const HWAInferenceRequest *request,
    const HWAInferenceInput *source,
    HWAInstrumentStemResult *stems,
    size_t stem_count,
    HWAInstrumentStemResult **order,
    uint64_t max_work_bytes,
    uint64_t deadline_started,
    char **settings_json,
    HWAInstrumentStemTask **task_result,
    char *error,
    size_t error_size)
{
    HWAInstrumentStemTask *task = NULL;
    HWAEventProvider *provider;
    HWAEventAudio *source_audio;
    size_t index;
    if (hwa_instrument_stem_output_work(
            request, source, order, stem_count, *settings_json,
            max_work_bytes, error, error_size) != 0)
        return -1;
    task = (HWAInstrumentStemTask *)calloc(1U, sizeof(*task));
    if (task == NULL) goto allocation_failed;
    task->bundle.providers =
        (HWAEventProvider *)calloc(1U, sizeof(*task->bundle.providers));
    task->bundle.audio = (HWAEventAudio *)calloc(
        stem_count + 1U, sizeof(*task->bundle.audio));
    task->bundle.events = (HWAPerformanceEvent *)calloc(
        stem_count, sizeof(*task->bundle.events));
    task->payloads = (HWAInferencePayload *)calloc(
        stem_count, sizeof(*task->payloads));
    if (task->bundle.providers == NULL || task->bundle.audio == NULL ||
        task->bundle.events == NULL || task->payloads == NULL)
        goto allocation_failed;
    task->bundle.provider_count = 1U;
    task->bundle.audio_count = stem_count + 1U;
    task->bundle.event_count = stem_count;
    task->output.bundle = &task->bundle;
    task->output.payloads = task->payloads;
    task->output.payload_count = stem_count;

    provider = &task->bundle.providers[0];
    provider->id = HWA_INSTRUMENT_STEM_PROVIDER_ID;
    provider->name = hwa_instrument_stem_copy_text(
        HWA_INSTRUMENT_STEM_PROVIDER_NAME, 0);
    provider->version = hwa_instrument_stem_copy_text(
        HWA_INSTRUMENT_STEM_PROVIDER_VERSION, 0);
    provider->settings_json = *settings_json;
    *settings_json = NULL;
    memcpy(provider->model_sha256, context->model_sha256,
           HWA_SHA256_HEX_SIZE);

    source_audio = &task->bundle.audio[0];
    source_audio->id = request->source_recording_id;
    source_audio->kind = HWA_EVENT_SOURCE_RECORDING;
    source_audio->name = hwa_instrument_stem_copy_text(
        source->bytes.name, 0);
    source_audio->relative_path = hwa_instrument_stem_copy_text("", 1);
    source_audio->path_hint = hwa_instrument_stem_copy_text("", 1);
    memcpy(source_audio->sha256, source->sha256, HWA_SHA256_HEX_SIZE);
    source_audio->file_bytes = source->bytes.size;
    source_audio->format = request->source_format;
    if (provider->name == NULL || provider->version == NULL ||
        provider->settings_json == NULL || source_audio->name == NULL ||
        source_audio->relative_path == NULL || source_audio->path_hint == NULL)
        goto allocation_failed;

    for (index = 0U; index < stem_count; ++index) {
        const HWAInstrumentStemResult *stem = order[index];
        HWAEventAudio *audio = &task->bundle.audio[index + 1U];
        HWAPerformanceEvent *event = &task->bundle.events[index];
        HWAEventValue *value;
        size_t stem_id_size;
        char *path;
        int written;
        if (hwa_inference_deadline_check(
                deadline_started, request->timeout_milliseconds,
                error, error_size) != 0)
            goto failure;
        if (!hwa_instrument_stem_id_valid(stem->stem_id, &stem_id_size)) {
            hwa_instrument_stem_error(
                error, error_size, "invalid instrument stem ID");
            goto failure;
        }
        path = (char *)malloc(stem_id_size + sizeof("audio/.wav"));
        if (path == NULL) goto allocation_failed;
        written = snprintf(path, stem_id_size + sizeof("audio/.wav"),
                           "audio/%s.wav", stem->stem_id);
        if (written < 0 ||
            (size_t)written >= stem_id_size + sizeof("audio/.wav")) {
            free(path);
            hwa_instrument_stem_error(
                error, error_size, "instrument stem path overflows");
            goto failure;
        }
        audio->id = hwa_instrument_stem_audio_id(
            request->source_recording_id, index);
        audio->kind = HWA_EVENT_INSTRUMENT_STEM;
        audio->name = hwa_instrument_stem_copy_text(stem->stem_id, 0);
        audio->relative_path = path;
        audio->path_hint = hwa_instrument_stem_copy_text("", 1);
        audio->file_bytes = stem->wave.size;
        audio->source_recording_id = request->source_recording_id;
        audio->source_recording_id_valid = 1;
        if (audio->name == NULL || audio->path_hint == NULL ||
            hwa_instrument_stem_result_valid(
                stem, request, deadline_started,
                &audio->format, audio->sha256,
                error, error_size) != 0)
            goto failure;

        task->payloads[index].relative_path = audio->relative_path;
        task->payloads[index].bytes = stem->wave;

        event->values = (HWAEventValue *)calloc(
            1U, sizeof(*event->values));
        if (event->values == NULL) goto allocation_failed;
        event->value_count = 1U;
        event->id = (uint64_t)index + UINT64_C(1);
        event->kind = hwa_instrument_stem_copy_text("instrument-region", 0);
        event->source_recording_id = request->source_recording_id;
        event->evidence_audio_id = audio->id;
        event->evidence_audio_id_valid = 1;
        event->start_sample = 0U;
        event->end_sample = request->source_format.frames;
        event->voice = hwa_instrument_stem_copy_text("", 1);
        event->part = hwa_instrument_stem_copy_text(stem->stem_id, 0);
        event->score_event_id = hwa_instrument_stem_copy_text("", 1);
        value = &event->values[0];
        value->name = hwa_instrument_stem_copy_text("instrument", 0);
        value->kind = HWA_EVENT_VALUE_TEXT;
        value->basis = HWA_EVENT_INFERENCE;
        value->text = hwa_instrument_stem_copy_text(stem->instrument, 0);
        value->unit = hwa_instrument_stem_copy_text("", 1);
        value->score = stem->score;
        value->provider_id = HWA_INSTRUMENT_STEM_PROVIDER_ID;
        value->score_valid = stem->score_valid;
        value->provider_id_valid = 1;
        value->selected = 1;
        if (event->kind == NULL || event->voice == NULL ||
            event->part == NULL || event->score_event_id == NULL ||
            value->name == NULL || value->text == NULL ||
            value->unit == NULL)
            goto allocation_failed;
    }
    task->runner_stems = stems;
    task->runner_stem_count = stem_count;
    task->runner_context = context->runner_context;
    task->results_destroy = context->results_destroy;
    *task_result = task;
    return 0;

allocation_failed:
    hwa_instrument_stem_error(
        error, error_size, "cannot allocate instrument stem output");
failure:
    hwa_instrument_stem_provider_task_free(NULL, task);
    return -1;
}

static int hwa_instrument_stem_provider_start(
    void *context_value,
    const HWAInferenceRequest *request,
    void **task_result,
    char *error,
    size_t error_size)
{
    HWAInstrumentStemProviderContext *context =
        (HWAInstrumentStemProviderContext *)context_value;
    HWAInferenceProvider descriptor;
    HWAInferenceRuntimeProvenance runtime;
    HWAInferenceRequest normalized_request;
    const HWAInferenceInput *source;
    HWAInstrumentStemResult *stems = NULL;
    HWAInstrumentStemResult **order = NULL;
    HWAInstrumentStemTask *task = NULL;
    char *settings_json = NULL;
    size_t stem_count = 0U;
    size_t index;
    uint64_t total_payload_bytes = 0U;
    uint64_t provenance_bytes;
    uint64_t max_work_bytes;
    uint64_t provenance_reserved;
    size_t order_bytes;
    size_t input_index_bytes;
    uint64_t deadline_started = 0U;
    int results_owned = 0;
    int status = -1;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (task_result == NULL) {
        hwa_instrument_stem_error(
            error, error_size, "instrument stem task output is null");
        return -1;
    }
    *task_result = NULL;
    if (context == NULL) {
        hwa_instrument_stem_error(
            error, error_size, "instrument stem provider context is null");
        return -1;
    }
    if (hwa_inference_deadline_start(
            &deadline_started, error, error_size) != 0)
        return -1;
    hwa_instrument_stem_descriptor(context, &descriptor);
    if (hwa_inference_request_validate(
            &descriptor, request, error, error_size) != 0)
        return -1;
    if (strcmp(request->task, HWA_INSTRUMENT_STEM_TASK_NAME) != 0 ||
        !hwa_instrument_stem_empty_settings(request->settings_json) ||
        request->input_count != 1U) {
        hwa_instrument_stem_error(
            error, error_size,
            "instrument stem task, settings, or input count do not match");
        return -1;
    }
    source = hwa_instrument_stem_source(request);
    if (source == NULL || request->source_format.frames == 0U) {
        hwa_instrument_stem_error(
            error, error_size, "instrument stem source is empty");
        return -1;
    }
    if (request->output_limits.max_audio_files < 2U ||
        request->output_limits.max_events < 1U ||
        request->output_limits.max_values < 1U) {
        hwa_instrument_stem_error(
            error, error_size,
            "instrument stem output limits cannot hold one stem");
        return -1;
    }
    if (hwa_inference_deadline_check(
            deadline_started, request->timeout_milliseconds,
            error, error_size) != 0)
        return -1;
    if (context->run(
            context->runner_context, &source->bytes,
            &request->source_format, request->seed,
            request->timeout_milliseconds, &stems, &stem_count,
            error, error_size) != 0) {
        context->results_destroy(
            context->runner_context, stems,
            stems != NULL ? stem_count : 0U);
        return -1;
    }
    results_owned = 1;
    if (hwa_inference_deadline_check(
            deadline_started, request->timeout_milliseconds,
            error, error_size) != 0)
        goto cleanup;
    if (stems == NULL || stem_count == 0U ||
        stem_count > SIZE_MAX - 1U ||
        (uint64_t)stem_count >= HWA_INSTRUMENT_STEM_JSON_SAFE_INTEGER ||
        request->output_limits.max_audio_files < stem_count + 1U ||
        request->output_limits.max_events < stem_count ||
        request->output_limits.max_values < stem_count) {
        hwa_instrument_stem_error(
            error, error_size,
            "instrument stem output count is zero or exceeds its limit");
        goto cleanup;
    }
    if (stem_count > SIZE_MAX / sizeof(*order) ||
        request->input_count > SIZE_MAX / sizeof(HWAInferenceInput *)) {
        hwa_instrument_stem_error(
            error, error_size, "instrument stem work size overflows");
        goto cleanup;
    }
    order_bytes = stem_count * sizeof(*order);
    input_index_bytes = request->input_count * sizeof(HWAInferenceInput *);
    max_work_bytes = context->max_work_bytes;
    if (max_work_bytes > request->output_limits.max_work_bytes)
        max_work_bytes = request->output_limits.max_work_bytes;
    if ((uint64_t)order_bytes > max_work_bytes) {
        hwa_instrument_stem_error(
            error, error_size, "instrument stem index exceeds its work limit");
        goto cleanup;
    }
    order = (HWAInstrumentStemResult **)calloc(stem_count, sizeof(*order));
    if (order == NULL) {
        hwa_instrument_stem_error(
            error, error_size, "cannot allocate instrument stem index");
        goto cleanup;
    }
    for (index = 0U; index < stem_count; ++index) {
        if (!hwa_instrument_stem_id_valid(stems[index].stem_id, NULL)) {
            hwa_instrument_stem_error(
                error, error_size, "invalid instrument stem ID");
            goto cleanup;
        }
        order[index] = &stems[index];
    }
    if (stem_count > 1U)
        qsort(order, stem_count, sizeof(*order),
              hwa_instrument_stem_pointer_compare);
    if (hwa_inference_deadline_check(
            deadline_started, request->timeout_milliseconds,
            error, error_size) != 0)
        goto cleanup;
    for (index = 0U; index < stem_count; ++index) {
        const HWAInstrumentStemResult *stem = order[index];
        if (!hwa_instrument_stem_id_valid(stem->stem_id, NULL) ||
            (index != 0U &&
             strcmp(order[index - 1U]->stem_id, stem->stem_id) == 0)) {
            hwa_instrument_stem_error(
                error, error_size, "invalid or duplicate instrument stem ID");
            goto cleanup;
        }
        if (total_payload_bytes > request->output_limits.max_bundle_bytes ||
            stem->wave.size >
                request->output_limits.max_bundle_bytes - total_payload_bytes) {
            hwa_instrument_stem_error(
                error, error_size,
                "instrument stem payload bytes exceed bundle limit");
            goto cleanup;
        }
        total_payload_bytes += stem->wave.size;
    }
    memset(&runtime, 0, sizeof(runtime));
    runtime.name = context->runtime_name;
    runtime.version = context->runtime_version;
    runtime.backend = context->backend;
    runtime.fallback = context->fallback;
    runtime.adapter_sha256 = context->adapter_sha256;
    normalized_request = *request;
    normalized_request.settings_json = "{}";
    provenance_reserved = (uint64_t)order_bytes;
    if (provenance_reserved > max_work_bytes ||
        (uint64_t)input_index_bytes > max_work_bytes - provenance_reserved) {
        hwa_instrument_stem_error(
            error, error_size,
            "instrument stem provenance exceeds its work limit");
        goto cleanup;
    }
    provenance_reserved += (uint64_t)input_index_bytes;
    if (provenance_reserved >= max_work_bytes) {
        hwa_instrument_stem_error(
            error, error_size,
            "instrument stem provenance exceeds its work limit");
        goto cleanup;
    }
    provenance_bytes = max_work_bytes - provenance_reserved - 1U;
    if (provenance_bytes > HWA_INSTRUMENT_STEM_PROVENANCE_BYTES)
        provenance_bytes = HWA_INSTRUMENT_STEM_PROVENANCE_BYTES;
    if (hwa_inference_provenance_settings_build(
            &normalized_request, &runtime,
            provenance_bytes,
            &settings_json, error, error_size) != 0)
        goto cleanup;
    if (hwa_inference_deadline_check(
            deadline_started, request->timeout_milliseconds,
            error, error_size) != 0)
        goto cleanup;
    if (hwa_instrument_stem_build_task(
            context, request, source, stems, stem_count, order,
            max_work_bytes, deadline_started, &settings_json, &task,
            error, error_size) != 0) {
        goto cleanup;
    }
    stems = NULL;
    results_owned = 0;
    free(order);
    order = NULL;
    if (hwa_inference_deadline_check(
            deadline_started, request->timeout_milliseconds,
            error, error_size) != 0)
        goto cleanup;
    if (hwa_inference_output_validate_for_request(
            &descriptor, request, &task->output,
            error, error_size) != 0 ||
        hwa_instrument_stem_bundle_validate_v1(
            &task->bundle, error, error_size) != 0)
        goto cleanup;
    if (hwa_inference_deadline_check(
            deadline_started, request->timeout_milliseconds,
            error, error_size) != 0)
        goto cleanup;
    *task_result = task;
    task = NULL;
    status = 0;
cleanup:
    hwa_instrument_stem_provider_task_free(NULL, task);
    if (results_owned)
        context->results_destroy(
            context->runner_context, stems,
            stems != NULL ? stem_count : 0U);
    free(settings_json);
    free(order);
    return status;
}

static int hwa_instrument_stem_provider_poll(
    void *context,
    void *task_value,
    HWAInferencePollState *state,
    const HWAInferenceOutput **output,
    char *error,
    size_t error_size)
{
    HWAInstrumentStemTask *task = (HWAInstrumentStemTask *)task_value;
    (void)context;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (state != NULL) *state = HWA_INFERENCE_PENDING;
    if (output != NULL) *output = NULL;
    if (task == NULL || state == NULL || output == NULL) {
        hwa_instrument_stem_error(
            error, error_size, "invalid instrument stem inference poll");
        return -1;
    }
    *state = HWA_INFERENCE_READY;
    *output = &task->output;
    return 0;
}

static void hwa_instrument_stem_provider_task_free(void *context,
                                                   void *task_value)
{
    HWAInstrumentStemTask *task = (HWAInstrumentStemTask *)task_value;
    (void)context;
    if (task == NULL) return;
    hwa_event_bundle_free(&task->bundle);
    if (task->results_destroy != NULL)
        task->results_destroy(
            task->runner_context, task->runner_stems,
            task->runner_stem_count);
    free(task->payloads);
    free(task);
}

static void hwa_instrument_stem_provider_destroy(void *context_value)
{
    HWAInstrumentStemProviderContext *context =
        (HWAInstrumentStemProviderContext *)context_value;
    if (context == NULL) return;
    if (context->runner_destroy != NULL)
        context->runner_destroy(context->runner_context);
    free(context->runtime_name);
    free(context->runtime_version);
    free(context->backend);
    free(context->fallback);
    free(context);
}

int hwa_instrument_stem_provider_init(
    HWAInferenceProvider *provider,
    const char *model_sha256,
    const char *adapter_sha256,
    uint64_t max_work_bytes,
    const HWAInstrumentStemRunner *runner,
    char *error,
    size_t error_size)
{
    HWAInstrumentStemProviderContext *context = NULL;
    int status = -1;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (provider == NULL) {
        hwa_instrument_stem_error(
            error, error_size, "instrument stem provider output is null");
        return -1;
    }
    memset(provider, 0, sizeof(*provider));
    if (!hwa_instrument_stem_lower_sha256(model_sha256, 1) ||
        !hwa_instrument_stem_lower_sha256(adapter_sha256, 0) ||
        max_work_bytes == 0U || runner == NULL || runner->run == NULL ||
        runner->results_destroy == NULL ||
        (runner->context != NULL && runner->destroy == NULL) ||
        !hwa_instrument_stem_utf8_text(runner->runtime_name, 0, NULL) ||
        !hwa_instrument_stem_utf8_text(runner->runtime_version, 0, NULL) ||
        !hwa_instrument_stem_utf8_text(runner->backend, 0, NULL) ||
        !hwa_instrument_stem_utf8_text(runner->fallback, 1, NULL)) {
        hwa_instrument_stem_error(
            error, error_size, "invalid instrument stem provider arguments");
        return -1;
    }
    context = (HWAInstrumentStemProviderContext *)calloc(
        1U, sizeof(*context));
    if (context == NULL) goto allocation_failed;
    context->runtime_name = hwa_instrument_stem_copy_text(
        runner->runtime_name, 0);
    context->runtime_version = hwa_instrument_stem_copy_text(
        runner->runtime_version, 0);
    context->backend = hwa_instrument_stem_copy_text(runner->backend, 0);
    context->fallback = hwa_instrument_stem_copy_text(runner->fallback, 1);
    if (context->runtime_name == NULL || context->runtime_version == NULL ||
        context->backend == NULL || context->fallback == NULL)
        goto allocation_failed;
    memcpy(context->model_sha256, model_sha256, strlen(model_sha256) + 1U);
    memcpy(context->adapter_sha256, adapter_sha256, HWA_SHA256_HEX_SIZE);
    context->max_work_bytes = max_work_bytes;
    context->runner_context = runner->context;
    context->run = runner->run;
    context->results_destroy = runner->results_destroy;
    context->runner_destroy = runner->destroy;
    hwa_instrument_stem_descriptor(context, provider);
    status = 0;
    goto cleanup;

allocation_failed:
    hwa_instrument_stem_error(
        error, error_size, "cannot allocate instrument stem provider");
cleanup:
    if (status != 0 && context != NULL) {
        free(context->runtime_name);
        free(context->runtime_version);
        free(context->backend);
        free(context->fallback);
        free(context);
    }
    return status;
}

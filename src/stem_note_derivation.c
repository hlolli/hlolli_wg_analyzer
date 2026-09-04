#include "stem_note_derivation.h"

#include "event_bundle.h"
#include "inference_clock.h"
#include "instrument_stem_provider.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HWA_STEM_NOTE_SAFE_ID_MAX UINT64_C(9007199254740991)
#define HWA_STEM_NOTE_ID_LIMIT 127U
#define HWA_STEM_NOTE_BASE_PROVIDER_NAME \
    "org.hlolli.instrument-stem-provider"
#define HWA_STEM_NOTE_BASE_PROVIDER_VERSION "1"

typedef struct HWAStemNoteRow {
    const HWAEventAudio *audio;
    const HWAPerformanceEvent *region;
    const HWAStemNoteAudioSource *source;
    size_t audio_row;
} HWAStemNoteRow;

typedef struct HWAStemNoteIdAllocator {
    uint64_t *used;
    size_t used_count;
    size_t cursor;
    uint64_t candidate;
} HWAStemNoteIdAllocator;

struct HWAStemNoteDerivation {
    HWAInferenceOutput output;
    HWAEventBundle bundle;
    HWAInferencePayload *payloads;
};

void hwa_stem_note_derivation_options_default(
    HWAStemNoteDerivationOptions *options)
{
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    options->note_task = "org.hlolli.note-events-on-audio-v1";
    options->note_settings_json = "{}";
    options->max_input_file_bytes =
        UINT64_C(16) * 1024U * 1024U * 1024U;
    options->max_input_bytes =
        UINT64_C(64) * 1024U * 1024U * 1024U;
    options->timeout_milliseconds = UINT64_C(600000);
    options->max_note_events = 1000000U;
    hwa_event_bundle_limits_default(&options->output_limits);
}

static void hwa_stem_note_error(char *error,
                                size_t error_size,
                                const char *message)
{
    if (error == NULL || error_size == 0U) return;
    (void)snprintf(error, error_size, "%s", message);
    error[error_size - 1U] = '\0';
}

static char *hwa_stem_note_copy_text(const char *text)
{
    char *copy;
    size_t size;
    if (text == NULL) return NULL;
    size = strlen(text);
    if (size == SIZE_MAX) return NULL;
    copy = (char *)malloc(size + 1U);
    if (copy != NULL) memcpy(copy, text, size + 1U);
    return copy;
}

static int hwa_stem_note_clone_provider(HWAEventProvider *destination,
                                        const HWAEventProvider *source)
{
    memset(destination, 0, sizeof(*destination));
    destination->id = source->id;
    memcpy(destination->model_sha256, source->model_sha256,
           sizeof(destination->model_sha256));
    destination->name = hwa_stem_note_copy_text(source->name);
    destination->version = hwa_stem_note_copy_text(source->version);
    destination->settings_json =
        hwa_stem_note_copy_text(source->settings_json);
    return destination->name != NULL && destination->version != NULL &&
                   destination->settings_json != NULL
               ? 0
               : -1;
}

static int hwa_stem_note_clone_audio(HWAEventAudio *destination,
                                     const HWAEventAudio *source)
{
    *destination = *source;
    destination->name = NULL;
    destination->relative_path = NULL;
    destination->path_hint = NULL;
    destination->name = hwa_stem_note_copy_text(source->name);
    if (source->relative_path != NULL)
        destination->relative_path =
            hwa_stem_note_copy_text(source->relative_path);
    if (source->path_hint != NULL)
        destination->path_hint = hwa_stem_note_copy_text(source->path_hint);
    if (destination->name == NULL ||
        (source->relative_path != NULL &&
         destination->relative_path == NULL) ||
        (source->path_hint != NULL && destination->path_hint == NULL))
        return -1;
    return 0;
}

static int hwa_stem_note_clone_trace(HWAEventTrace *destination,
                                     const HWAEventTrace *source)
{
    *destination = *source;
    destination->name = NULL;
    destination->unit = NULL;
    destination->relative_path = NULL;
    destination->name = hwa_stem_note_copy_text(source->name);
    destination->unit = hwa_stem_note_copy_text(source->unit);
    destination->relative_path =
        hwa_stem_note_copy_text(source->relative_path);
    return destination->name != NULL && destination->unit != NULL &&
                   destination->relative_path != NULL
               ? 0
               : -1;
}

static int hwa_stem_note_clone_value(HWAEventValue *destination,
                                     const HWAEventValue *source)
{
    *destination = *source;
    destination->name = NULL;
    destination->text = NULL;
    destination->unit = NULL;
    destination->name = hwa_stem_note_copy_text(source->name);
    if (source->text != NULL)
        destination->text = hwa_stem_note_copy_text(source->text);
    if (source->unit != NULL)
        destination->unit = hwa_stem_note_copy_text(source->unit);
    if (destination->name == NULL ||
        (source->text != NULL && destination->text == NULL) ||
        (source->unit != NULL && destination->unit == NULL))
        return -1;
    return 0;
}

static int hwa_stem_note_clone_event(HWAPerformanceEvent *destination,
                                     const HWAPerformanceEvent *source)
{
    size_t index;
    *destination = *source;
    destination->kind = NULL;
    destination->voice = NULL;
    destination->part = NULL;
    destination->score_event_id = NULL;
    destination->values = NULL;
    destination->trace_refs = NULL;
    destination->value_count = 0U;
    destination->trace_ref_count = 0U;
    destination->kind = hwa_stem_note_copy_text(source->kind);
    if (source->voice != NULL)
        destination->voice = hwa_stem_note_copy_text(source->voice);
    if (source->part != NULL)
        destination->part = hwa_stem_note_copy_text(source->part);
    if (source->score_event_id != NULL)
        destination->score_event_id =
            hwa_stem_note_copy_text(source->score_event_id);
    if (destination->kind == NULL ||
        (source->voice != NULL && destination->voice == NULL) ||
        (source->part != NULL && destination->part == NULL) ||
        (source->score_event_id != NULL &&
         destination->score_event_id == NULL))
        return -1;
    if (source->value_count != 0U) {
        if (source->value_count > SIZE_MAX / sizeof(*destination->values))
            return -1;
        destination->values = (HWAEventValue *)calloc(
            source->value_count, sizeof(*destination->values));
        if (destination->values == NULL) return -1;
        destination->value_count = source->value_count;
        for (index = 0U; index < source->value_count; ++index)
            if (hwa_stem_note_clone_value(
                    &destination->values[index], &source->values[index]) != 0)
                return -1;
    }
    if (source->trace_ref_count != 0U) {
        if (source->trace_ref_count >
            SIZE_MAX / sizeof(*destination->trace_refs))
            return -1;
        destination->trace_refs = (HWAEventTraceRef *)calloc(
            source->trace_ref_count, sizeof(*destination->trace_refs));
        if (destination->trace_refs == NULL) return -1;
        destination->trace_ref_count = source->trace_ref_count;
        for (index = 0U; index < source->trace_ref_count; ++index) {
            destination->trace_refs[index] = source->trace_refs[index];
            destination->trace_refs[index].role = hwa_stem_note_copy_text(
                source->trace_refs[index].role);
            if (destination->trace_refs[index].role == NULL) return -1;
        }
    }
    return 0;
}

static int hwa_stem_note_clone_warning(HWAEventWarning *destination,
                                       const HWAEventWarning *source)
{
    *destination = *source;
    destination->code = hwa_stem_note_copy_text(source->code);
    destination->message = hwa_stem_note_copy_text(source->message);
    return destination->code != NULL && destination->message != NULL ? 0 : -1;
}

static int hwa_stem_note_clone_bundle(HWAEventBundle *destination,
                                      const HWAEventBundle *source)
{
    size_t index;
    memset(destination, 0, sizeof(*destination));
    if (source->directory != NULL) {
        destination->directory =
            hwa_stem_note_copy_text(source->directory);
        if (destination->directory == NULL) return -1;
    }
    if ((source->provider_count != 0U &&
         source->provider_count > SIZE_MAX / sizeof(*destination->providers)) ||
        source->audio_count > SIZE_MAX / sizeof(*destination->audio) ||
        (source->trace_count != 0U &&
         source->trace_count > SIZE_MAX / sizeof(*destination->traces)) ||
        (source->event_count != 0U &&
         source->event_count > SIZE_MAX / sizeof(*destination->events)) ||
        (source->warning_count != 0U &&
         source->warning_count > SIZE_MAX / sizeof(*destination->warnings)))
        return -1;
    if (source->provider_count != 0U) {
        destination->providers = (HWAEventProvider *)calloc(
            source->provider_count, sizeof(*destination->providers));
        if (destination->providers == NULL) return -1;
        destination->provider_count = source->provider_count;
        for (index = 0U; index < source->provider_count; ++index)
            if (hwa_stem_note_clone_provider(
                    &destination->providers[index],
                    &source->providers[index]) != 0)
                return -1;
    }
    destination->audio = (HWAEventAudio *)calloc(
        source->audio_count, sizeof(*destination->audio));
    if (destination->audio == NULL) return -1;
    destination->audio_count = source->audio_count;
    for (index = 0U; index < source->audio_count; ++index)
        if (hwa_stem_note_clone_audio(
                &destination->audio[index], &source->audio[index]) != 0)
            return -1;
    if (source->trace_count != 0U) {
        destination->traces = (HWAEventTrace *)calloc(
            source->trace_count, sizeof(*destination->traces));
        if (destination->traces == NULL) return -1;
        destination->trace_count = source->trace_count;
        for (index = 0U; index < source->trace_count; ++index)
            if (hwa_stem_note_clone_trace(
                    &destination->traces[index], &source->traces[index]) != 0)
                return -1;
    }
    if (source->event_count != 0U) {
        destination->events = (HWAPerformanceEvent *)calloc(
            source->event_count, sizeof(*destination->events));
        if (destination->events == NULL) return -1;
        destination->event_count = source->event_count;
        for (index = 0U; index < source->event_count; ++index)
            if (hwa_stem_note_clone_event(
                    &destination->events[index], &source->events[index]) != 0)
                return -1;
    }
    if (source->warning_count != 0U) {
        destination->warnings = (HWAEventWarning *)calloc(
            source->warning_count, sizeof(*destination->warnings));
        if (destination->warnings == NULL) return -1;
        destination->warning_count = source->warning_count;
        for (index = 0U; index < source->warning_count; ++index)
            if (hwa_stem_note_clone_warning(
                    &destination->warnings[index],
                    &source->warnings[index]) != 0)
                return -1;
    }
    return 0;
}

static int hwa_stem_note_id_valid(const char *text)
{
    size_t index;
    if (text == NULL || text[0] < 'a' || text[0] > 'z') return 0;
    for (index = 1U; index <= HWA_STEM_NOTE_ID_LIMIT; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (value == 0U) return 1;
        if (!((value >= (unsigned char)'a' &&
               value <= (unsigned char)'z') ||
              (value >= (unsigned char)'0' &&
               value <= (unsigned char)'9') ||
              value == (unsigned char)'-' || value == (unsigned char)'_'))
            return 0;
    }
    return 0;
}

static int hwa_stem_note_row_compare(const void *left, const void *right)
{
    const HWAStemNoteRow *left_row = (const HWAStemNoteRow *)left;
    const HWAStemNoteRow *right_row = (const HWAStemNoteRow *)right;
    int order = strcmp(left_row->audio->name, right_row->audio->name);
    if (order != 0) return order;
    if (left_row->audio->id < right_row->audio->id) return -1;
    if (left_row->audio->id > right_row->audio->id) return 1;
    return 0;
}

static const HWAStemNoteAudioSource *hwa_stem_note_find_source(
    const HWAStemNoteAudioSource *sources,
    size_t count,
    uint64_t audio_id,
    size_t *match_count)
{
    const HWAStemNoteAudioSource *match = NULL;
    size_t found = 0U;
    size_t index;
    for (index = 0U; index < count; ++index) {
        if (sources[index].audio_id == audio_id) {
            match = &sources[index];
            found++;
        }
    }
    if (match_count != NULL) *match_count = found;
    return match;
}

static int hwa_stem_note_region_value_valid(
    const HWAPerformanceEvent *event,
    uint64_t provider_id)
{
    const HWAEventValue *value;
    if (event->value_count != 1U) return 0;
    value = &event->values[0];
    return strcmp(value->name, "instrument") == 0 &&
           value->kind == HWA_EVENT_VALUE_TEXT &&
           value->basis == HWA_EVENT_INFERENCE && value->text != NULL &&
           value->unit != NULL && value->unit[0] == '\0' &&
           value->provider_id_valid && value->provider_id == provider_id &&
           value->selected;
}

static int hwa_stem_note_collect_rows(
    const HWAEventBundle *bundle,
    const HWAStemNoteAudioSource *sources,
    size_t source_count,
    const HWAStemNoteDerivationOptions *options,
    uint64_t deadline_started,
    HWAStemNoteRow **rows_result,
    size_t *row_count_result,
    const HWAEventAudio **source_result,
    char *error,
    size_t error_size)
{
    HWAStemNoteRow *rows = NULL;
    const HWAEventAudio *source_audio = NULL;
    size_t stem_count = 0U;
    size_t source_rows = 0U;
    size_t index;
    uint64_t input_bytes = 0U;
    uint64_t provider_id;
    if (bundle->provider_count != 1U ||
        strcmp(bundle->providers[0].name,
               HWA_STEM_NOTE_BASE_PROVIDER_NAME) != 0 ||
        strcmp(bundle->providers[0].version,
               HWA_STEM_NOTE_BASE_PROVIDER_VERSION) != 0 ||
        bundle->trace_count != 0U ||
        bundle->warning_count != 0U) {
        hwa_stem_note_error(
            error, error_size, "input is not a plain instrument-stem bundle");
        return -1;
    }
    provider_id = bundle->providers[0].id;
    for (index = 0U; index < bundle->audio_count; ++index) {
        const HWAEventAudio *audio = &bundle->audio[index];
        if (audio->kind == HWA_EVENT_SOURCE_RECORDING) {
            source_audio = audio;
            source_rows++;
        } else if (audio->kind == HWA_EVENT_INSTRUMENT_STEM) {
            stem_count++;
        } else {
            hwa_stem_note_error(
                error, error_size, "input has non-stem derived audio");
            return -1;
        }
    }
    if (source_rows != 1U || stem_count == 0U ||
        bundle->audio_count != stem_count + 1U ||
        bundle->event_count != stem_count || source_count != stem_count ||
        (source_count != 0U && sources == NULL)) {
        hwa_stem_note_error(
            error, error_size, "input stem rows or source bindings are incomplete");
        return -1;
    }
    if (source_audio->relative_path != NULL &&
        source_audio->relative_path[0] != '\0') {
        hwa_stem_note_error(
            error, error_size, "input source recording must stay external");
        return -1;
    }
    if (stem_count > SIZE_MAX / sizeof(*rows)) {
        hwa_stem_note_error(error, error_size, "stem row index overflows");
        return -1;
    }
    rows = (HWAStemNoteRow *)calloc(stem_count, sizeof(*rows));
    if (rows == NULL) {
        hwa_stem_note_error(error, error_size, "cannot allocate stem row index");
        return -1;
    }
    stem_count = 0U;
    for (index = 0U; index < bundle->audio_count; ++index) {
        const HWAEventAudio *audio = &bundle->audio[index];
        const HWAStemNoteAudioSource *bound_source;
        const HWAPerformanceEvent *region = NULL;
        size_t binding_matches = 0U;
        size_t region_matches = 0U;
        size_t event_index;
        char actual_sha256[HWA_SHA256_HEX_SIZE];
        if (audio->kind != HWA_EVENT_INSTRUMENT_STEM) continue;
        if (!hwa_stem_note_id_valid(audio->name) ||
            audio->relative_path == NULL || audio->relative_path[0] == '\0' ||
            !audio->source_recording_id_valid ||
            audio->source_recording_id != source_audio->id) {
            hwa_stem_note_error(error, error_size, "invalid instrument stem row");
            goto failure;
        }
        for (event_index = 0U; event_index < bundle->event_count;
             ++event_index) {
            const HWAPerformanceEvent *candidate =
                &bundle->events[event_index];
            if (candidate->evidence_audio_id_valid &&
                candidate->evidence_audio_id == audio->id &&
                strcmp(candidate->kind, "instrument-region") == 0) {
                region = candidate;
                region_matches++;
            }
        }
        if (region_matches != 1U || region == NULL ||
            region->source_recording_id != source_audio->id ||
            region->parent_id_valid || region->start_sample != 0U ||
            region->end_sample != source_audio->format.frames ||
            region->part == NULL || strcmp(region->part, audio->name) != 0 ||
            region->trace_ref_count != 0U ||
            !hwa_stem_note_region_value_valid(region, provider_id)) {
            hwa_stem_note_error(
                error, error_size,
                "instrument stem has no unique full-span region");
            goto failure;
        }
        bound_source = hwa_stem_note_find_source(
            sources, source_count, audio->id, &binding_matches);
        if (binding_matches != 1U || bound_source == NULL ||
            bound_source->bytes.read_at == NULL ||
            bound_source->bytes.size != audio->file_bytes ||
            bound_source->bytes.size > options->max_input_file_bytes ||
            input_bytes > options->max_input_bytes ||
            bound_source->bytes.size > options->max_input_bytes - input_bytes) {
            hwa_stem_note_error(
                error, error_size, "invalid or oversized stem byte source");
            goto failure;
        }
        input_bytes += bound_source->bytes.size;
        if (hwa_inference_byte_source_sha256(
                &bound_source->bytes, options->max_input_file_bytes,
                actual_sha256, error, error_size) != 0 ||
            strcmp(actual_sha256, audio->sha256) != 0) {
            if (error != NULL && error_size != 0U && error[0] == '\0')
                hwa_stem_note_error(
                    error, error_size, "stem byte source hash is wrong");
            goto failure;
        }
        if (hwa_inference_deadline_check(
                deadline_started, options->timeout_milliseconds,
                error, error_size) != 0)
            goto failure;
        rows[stem_count].audio = audio;
        rows[stem_count].region = region;
        rows[stem_count].source = bound_source;
        rows[stem_count].audio_row = index;
        stem_count++;
    }
    if (stem_count > 1U)
        qsort(rows, stem_count, sizeof(*rows), hwa_stem_note_row_compare);
    for (index = 1U; index < stem_count; ++index) {
        if (strcmp(rows[index - 1U].audio->name,
                   rows[index].audio->name) == 0) {
            hwa_stem_note_error(error, error_size, "duplicate instrument stem ID");
            goto failure;
        }
    }
    for (index = 0U; index < source_count; ++index) {
        size_t matches = 0U;
        (void)hwa_stem_note_find_source(
            sources, source_count, sources[index].audio_id, &matches);
        if (matches != 1U) {
            hwa_stem_note_error(
                error, error_size, "duplicate stem byte-source binding");
            goto failure;
        }
    }
    *rows_result = rows;
    *row_count_result = stem_count;
    *source_result = source_audio;
    return 0;
failure:
    free(rows);
    return -1;
}

static int hwa_stem_note_u64_compare(const void *left, const void *right)
{
    uint64_t left_id = *(const uint64_t *)left;
    uint64_t right_id = *(const uint64_t *)right;
    if (left_id < right_id) return -1;
    if (left_id > right_id) return 1;
    return 0;
}

static int hwa_stem_note_id_allocator_init(
    HWAStemNoteIdAllocator *allocator,
    size_t count,
    uint64_t max_work_bytes,
    char *error,
    size_t error_size)
{
    uint64_t bytes;
    if (allocator == NULL ||
        count > SIZE_MAX / sizeof(*allocator->used)) {
        hwa_stem_note_error(error, error_size, "ID index size overflows");
        return -1;
    }
    memset(allocator, 0, sizeof(*allocator));
    allocator->candidate = 1U;
    bytes = (uint64_t)count * (uint64_t)sizeof(*allocator->used);
    if (bytes > max_work_bytes) {
        hwa_stem_note_error(error, error_size, "ID index exceeds work limit");
        return -1;
    }
    if (count != 0U) {
        allocator->used = (uint64_t *)malloc(count * sizeof(*allocator->used));
        if (allocator->used == NULL) {
            hwa_stem_note_error(error, error_size, "cannot allocate ID index");
            return -1;
        }
    }
    allocator->used_count = count;
    return 0;
}

static int hwa_stem_note_id_allocate(HWAStemNoteIdAllocator *allocator,
                                     uint64_t *id,
                                     char *error,
                                     size_t error_size)
{
    if (allocator == NULL || id == NULL) {
        hwa_stem_note_error(error, error_size, "invalid ID allocator");
        return -1;
    }
    while (allocator->cursor < allocator->used_count &&
           allocator->used[allocator->cursor] < allocator->candidate)
        allocator->cursor++;
    while (allocator->cursor < allocator->used_count &&
           allocator->used[allocator->cursor] == allocator->candidate) {
        if (allocator->candidate >= HWA_STEM_NOTE_SAFE_ID_MAX) {
            hwa_stem_note_error(error, error_size, "merged ID space is full");
            return -1;
        }
        allocator->candidate++;
        allocator->cursor++;
        while (allocator->cursor < allocator->used_count &&
               allocator->used[allocator->cursor] < allocator->candidate)
            allocator->cursor++;
    }
    if (allocator->candidate > HWA_STEM_NOTE_SAFE_ID_MAX) {
        hwa_stem_note_error(error, error_size, "merged ID space is full");
        return -1;
    }
    *id = allocator->candidate;
    if (allocator->candidate == HWA_STEM_NOTE_SAFE_ID_MAX)
        allocator->candidate = HWA_STEM_NOTE_SAFE_ID_MAX + UINT64_C(1);
    else
        allocator->candidate++;
    return 0;
}

static void hwa_stem_note_id_allocator_free(
    HWAStemNoteIdAllocator *allocator)
{
    if (allocator == NULL) return;
    free(allocator->used);
    memset(allocator, 0, sizeof(*allocator));
}

static int hwa_stem_note_selected_pitch(const HWAPerformanceEvent *event,
                                        uint64_t provider_id)
{
    size_t index;
    size_t selected = 0U;
    for (index = 0U; index < event->value_count; ++index) {
        const HWAEventValue *value = &event->values[index];
        if (strcmp(value->name, "pitch-hz") == 0 && value->selected) {
            if (value->kind != HWA_EVENT_VALUE_F64 ||
                value->basis != HWA_EVENT_INFERENCE ||
                !isfinite(value->number) || value->number <= 0.0 ||
                value->unit == NULL || strcmp(value->unit, "Hz") != 0 ||
                !value->provider_id_valid ||
                value->provider_id != provider_id)
                return 0;
            selected++;
        }
    }
    return selected == 1U;
}

static int hwa_stem_note_child_shape(
    const HWAInferenceOutput *output,
    uint64_t stem_audio_id,
    uint64_t stem_frames,
    uint64_t *child_provider_id,
    size_t *child_value_count,
    char *error,
    size_t error_size)
{
    const HWAEventBundle *bundle = output->bundle;
    size_t values = 0U;
    size_t index;
    if (bundle->provider_count != 1U || bundle->audio_count != 1U ||
        bundle->audio[0].kind != HWA_EVENT_SOURCE_RECORDING ||
        bundle->audio[0].id != stem_audio_id || bundle->trace_count != 0U ||
        bundle->warning_count != 0U || output->payload_count != 0U) {
        hwa_stem_note_error(
            error, error_size, "note provider returned an unsupported shape");
        return -1;
    }
    *child_provider_id = bundle->providers[0].id;
    for (index = 0U; index < bundle->event_count; ++index) {
        const HWAPerformanceEvent *event = &bundle->events[index];
        size_t value_index;
        if (strcmp(event->kind, "note") != 0 ||
            event->source_recording_id != stem_audio_id ||
            !event->evidence_audio_id_valid ||
            event->evidence_audio_id != stem_audio_id ||
            event->parent_id_valid || event->end_sample > stem_frames ||
            event->trace_ref_count != 0U ||
            !hwa_stem_note_selected_pitch(event, *child_provider_id)) {
            hwa_stem_note_error(
                error, error_size, "note provider returned a non-note event");
            return -1;
        }
        if (values > SIZE_MAX - event->value_count) {
            hwa_stem_note_error(error, error_size, "note value count overflows");
            return -1;
        }
        values += event->value_count;
        for (value_index = 0U; value_index < event->value_count;
             ++value_index) {
            const HWAEventValue *value = &event->values[value_index];
            if (value->provider_id_valid &&
                value->provider_id != *child_provider_id) {
                hwa_stem_note_error(
                    error, error_size,
                    "note value names an unexpected provider");
                return -1;
            }
        }
    }
    *child_value_count = values;
    return 0;
}

static int hwa_stem_note_append_provider(
    HWAEventBundle *bundle,
    const HWAEventProvider *source,
    uint64_t provider_id,
    char *error,
    size_t error_size)
{
    HWAEventProvider *rows;
    HWAEventProvider provider;
    size_t next;
    memset(&provider, 0, sizeof(provider));
    if (bundle->provider_count == SIZE_MAX ||
        bundle->provider_count + 1U >
            SIZE_MAX / sizeof(*bundle->providers)) {
        hwa_stem_note_error(error, error_size, "provider row count overflows");
        return -1;
    }
    if (hwa_stem_note_clone_provider(&provider, source) != 0) {
        free(provider.name);
        free(provider.version);
        free(provider.settings_json);
        hwa_stem_note_error(error, error_size, "cannot copy note provider row");
        return -1;
    }
    provider.id = provider_id;
    next = bundle->provider_count + 1U;
    rows = (HWAEventProvider *)realloc(
        bundle->providers, next * sizeof(*bundle->providers));
    if (rows == NULL) {
        free(provider.name);
        free(provider.version);
        free(provider.settings_json);
        hwa_stem_note_error(error, error_size, "cannot grow provider rows");
        return -1;
    }
    bundle->providers = rows;
    bundle->providers[bundle->provider_count] = provider;
    bundle->provider_count = next;
    return 0;
}

static int hwa_stem_note_append_events(
    HWAEventBundle *bundle,
    const HWAEventBundle *child,
    const HWAStemNoteRow *stem,
    uint64_t source_recording_id,
    uint64_t provider_id,
    uint64_t child_provider_id,
    HWAStemNoteIdAllocator *event_ids,
    char *error,
    size_t error_size)
{
    HWAPerformanceEvent *rows;
    size_t original_count = bundle->event_count;
    size_t next;
    size_t index;
    if (child->event_count > SIZE_MAX - original_count ||
        original_count + child->event_count >
            SIZE_MAX / sizeof(*bundle->events)) {
        hwa_stem_note_error(error, error_size, "event row count overflows");
        return -1;
    }
    next = original_count + child->event_count;
    if (child->event_count != 0U) {
        rows = (HWAPerformanceEvent *)realloc(
            bundle->events, next * sizeof(*bundle->events));
        if (rows == NULL) {
            hwa_stem_note_error(error, error_size, "cannot grow note event rows");
            return -1;
        }
        bundle->events = rows;
        memset(bundle->events + original_count, 0,
               child->event_count * sizeof(*bundle->events));
    }
    for (index = 0U; index < child->event_count; ++index) {
        HWAPerformanceEvent *event = &bundle->events[original_count + index];
        size_t value_index;
        uint64_t event_id = 0U;
        char *voice;
        char *part;
        char *score_event_id;
        if (hwa_stem_note_id_allocate(
                event_ids, &event_id, error, error_size) != 0 ||
            hwa_stem_note_clone_event(event, &child->events[index]) != 0) {
            bundle->event_count = original_count + index + 1U;
            if (error != NULL && error_size != 0U && error[0] == '\0')
                hwa_stem_note_error(error, error_size, "cannot copy note event");
            return -1;
        }
        bundle->event_count = original_count + index + 1U;
        voice = hwa_stem_note_copy_text("");
        part = hwa_stem_note_copy_text("");
        score_event_id = hwa_stem_note_copy_text("");
        if (voice == NULL || part == NULL || score_event_id == NULL) {
            free(voice);
            free(part);
            free(score_event_id);
            hwa_stem_note_error(
                error, error_size, "cannot clear note score fields");
            return -1;
        }
        free(event->voice);
        free(event->part);
        free(event->score_event_id);
        event->voice = voice;
        event->part = part;
        event->score_event_id = score_event_id;
        event->id = event_id;
        event->source_recording_id = source_recording_id;
        event->evidence_audio_id = stem->audio->id;
        event->evidence_audio_id_valid = 1;
        event->parent_id = stem->region->id;
        event->parent_id_valid = 1;
        for (value_index = 0U; value_index < event->value_count;
             ++value_index) {
            HWAEventValue *value = &event->values[value_index];
            if (value->provider_id_valid &&
                value->provider_id == child_provider_id)
                value->provider_id = provider_id;
        }
    }
    bundle->event_count = next;
    return 0;
}

static size_t hwa_stem_note_value_count(const HWAEventBundle *bundle)
{
    size_t total = 0U;
    size_t index;
    for (index = 0U; index < bundle->event_count; ++index) {
        if (total > SIZE_MAX - bundle->events[index].value_count)
            return SIZE_MAX;
        total += bundle->events[index].value_count;
    }
    return total;
}

static int hwa_stem_note_work_add(uint64_t *total,
                                  size_t count,
                                  size_t item_size,
                                  uint64_t maximum)
{
    uint64_t bytes;
    if (total == NULL || (count != 0U && item_size > SIZE_MAX / count))
        return -1;
    bytes = (uint64_t)(count * item_size);
    if (*total > maximum || bytes > maximum - *total) return -1;
    *total += bytes;
    return 0;
}

static int hwa_stem_note_work_add_text(uint64_t *total,
                                       const char *text,
                                       uint64_t maximum)
{
    size_t size;
    if (text == NULL) return 0;
    size = strlen(text);
    if (size == SIZE_MAX) return -1;
    return hwa_stem_note_work_add(total, size + 1U, 1U, maximum);
}

static int hwa_stem_note_bundle_work_before_append(
    const HWAEventBundle *bundle,
    const HWAEventBundle *child,
    uint64_t maximum,
    uint64_t *work_result,
    char *error,
    size_t error_size)
{
    uint64_t total = 0U;
    size_t index;
    if (work_result == NULL || bundle->provider_count == SIZE_MAX ||
        child->event_count > SIZE_MAX - bundle->event_count ||
        hwa_stem_note_work_add(
            &total, bundle->provider_count + 1U,
            sizeof(*bundle->providers), maximum) != 0 ||
        hwa_stem_note_work_add(
            &total, bundle->audio_count,
            sizeof(*bundle->audio), maximum) != 0 ||
        hwa_stem_note_work_add(
            &total, bundle->trace_count,
            sizeof(*bundle->traces), maximum) != 0 ||
        hwa_stem_note_work_add(
            &total, bundle->event_count + child->event_count,
            sizeof(*bundle->events), maximum) != 0 ||
        hwa_stem_note_work_add(
            &total, bundle->warning_count,
            sizeof(*bundle->warnings), maximum) != 0)
        goto too_large;
    for (index = 0U; index < bundle->provider_count; ++index) {
        const HWAEventProvider *provider = &bundle->providers[index];
        if (hwa_stem_note_work_add_text(
                &total, provider->name, maximum) != 0 ||
            hwa_stem_note_work_add_text(
                &total, provider->version, maximum) != 0 ||
            hwa_stem_note_work_add_text(
                &total, provider->settings_json, maximum) != 0)
            goto too_large;
    }
    if (hwa_stem_note_work_add_text(
            &total, child->providers[0].name, maximum) != 0 ||
        hwa_stem_note_work_add_text(
            &total, child->providers[0].version, maximum) != 0 ||
        hwa_stem_note_work_add_text(
            &total, child->providers[0].settings_json, maximum) != 0)
        goto too_large;
    for (index = 0U; index < bundle->audio_count; ++index) {
        const HWAEventAudio *audio = &bundle->audio[index];
        if (hwa_stem_note_work_add_text(
                &total, audio->name, maximum) != 0 ||
            hwa_stem_note_work_add_text(
                &total, audio->relative_path, maximum) != 0 ||
            hwa_stem_note_work_add_text(
                &total, audio->path_hint, maximum) != 0)
            goto too_large;
    }
    for (index = 0U; index < bundle->trace_count; ++index) {
        const HWAEventTrace *trace = &bundle->traces[index];
        if (hwa_stem_note_work_add_text(
                &total, trace->name, maximum) != 0 ||
            hwa_stem_note_work_add_text(
                &total, trace->unit, maximum) != 0 ||
            hwa_stem_note_work_add_text(
                &total, trace->relative_path, maximum) != 0)
            goto too_large;
    }
    for (index = 0U; index < bundle->event_count; ++index) {
        const HWAPerformanceEvent *event = &bundle->events[index];
        size_t value_index;
        size_t ref_index;
        if (hwa_stem_note_work_add_text(
                &total, event->kind, maximum) != 0 ||
            hwa_stem_note_work_add_text(
                &total, event->voice, maximum) != 0 ||
            hwa_stem_note_work_add_text(
                &total, event->part, maximum) != 0 ||
            hwa_stem_note_work_add_text(
                &total, event->score_event_id, maximum) != 0 ||
            hwa_stem_note_work_add(
                &total, event->value_count,
                sizeof(*event->values), maximum) != 0 ||
            hwa_stem_note_work_add(
                &total, event->trace_ref_count,
                sizeof(*event->trace_refs), maximum) != 0)
            goto too_large;
        for (value_index = 0U; value_index < event->value_count;
             ++value_index) {
            const HWAEventValue *value = &event->values[value_index];
            if (hwa_stem_note_work_add_text(
                    &total, value->name, maximum) != 0 ||
                hwa_stem_note_work_add_text(
                    &total, value->text, maximum) != 0 ||
                hwa_stem_note_work_add_text(
                    &total, value->unit, maximum) != 0)
                goto too_large;
        }
        for (ref_index = 0U; ref_index < event->trace_ref_count;
             ++ref_index)
            if (hwa_stem_note_work_add_text(
                    &total, event->trace_refs[ref_index].role,
                    maximum) != 0)
                goto too_large;
    }
    for (index = 0U; index < child->event_count; ++index) {
        const HWAPerformanceEvent *event = &child->events[index];
        size_t value_index;
        if (hwa_stem_note_work_add_text(
                &total, event->kind, maximum) != 0 ||
            hwa_stem_note_work_add_text(&total, "", maximum) != 0 ||
            hwa_stem_note_work_add_text(&total, "", maximum) != 0 ||
            hwa_stem_note_work_add_text(&total, "", maximum) != 0 ||
            hwa_stem_note_work_add(
                &total, event->value_count,
                sizeof(*event->values), maximum) != 0)
            goto too_large;
        for (value_index = 0U; value_index < event->value_count;
             ++value_index) {
            const HWAEventValue *value = &event->values[value_index];
            if (hwa_stem_note_work_add_text(
                    &total, value->name, maximum) != 0 ||
                hwa_stem_note_work_add_text(
                    &total, value->text, maximum) != 0 ||
                hwa_stem_note_work_add_text(
                    &total, value->unit, maximum) != 0)
                goto too_large;
        }
    }
    for (index = 0U; index < bundle->warning_count; ++index) {
        const HWAEventWarning *warning = &bundle->warnings[index];
        if (hwa_stem_note_work_add_text(
                &total, warning->code, maximum) != 0 ||
            hwa_stem_note_work_add_text(
                &total, warning->message, maximum) != 0)
            goto too_large;
    }
    if (hwa_stem_note_work_add_text(
            &total, bundle->directory, maximum) != 0)
        goto too_large;
    *work_result = total;
    return 0;
too_large:
    hwa_stem_note_error(
        error, error_size, "merged note bundle exceeds its work limit");
    return -1;
}

static int hwa_stem_note_run_one(
    HWAStemNoteDerivation *result,
    const HWAStemNoteRow *stem,
    uint64_t source_recording_id,
    HWAInferenceProvider *provider,
    const HWAStemNoteDerivationOptions *options,
    uint64_t deadline_started,
    uint64_t fixed_work,
    HWAStemNoteIdAllocator *provider_ids,
    HWAStemNoteIdAllocator *event_ids,
    size_t *note_count,
    size_t *value_count,
    char *error,
    size_t error_size)
{
    HWAByteSource source = stem->source->bytes;
    HWAInferenceInput input;
    HWAInferenceRequest request;
    HWAInferencePollState state = HWA_INFERENCE_PENDING;
    const HWAInferenceOutput *child_output = NULL;
    const HWAEventBundle *child;
    void *task = NULL;
    uint64_t remaining = 0U;
    uint64_t current_work = 0U;
    uint64_t child_work = 0U;
    uint64_t merged_work = 0U;
    uint64_t available_work;
    uint64_t child_provider_id = 0U;
    uint64_t provider_id = 0U;
    size_t child_value_count = 0U;
    size_t available_events;
    size_t available_values;
    int status = -1;
    memset(&input, 0, sizeof(input));
    memset(&request, 0, sizeof(request));
    source.name = stem->audio->relative_path;
    input.id = "source";
    input.role = "source-recording";
    input.media_type = "audio/wav";
    input.sha256 = stem->audio->sha256;
    input.bytes = source;
    request.task = options->note_task;
    request.settings_json = options->note_settings_json;
    request.expected_provider_name = provider->name;
    request.expected_provider_version = provider->version;
    request.expected_model_sha256 = provider->model_sha256;
    request.seed = options->seed;
    request.source_recording_id = stem->audio->id;
    request.source_input_id = input.id;
    request.inputs = &input;
    request.input_count = 1U;
    request.source_format = stem->audio->format;
    request.max_input_file_bytes = options->max_input_file_bytes;
    request.max_input_bytes = options->max_input_bytes;
    request.output_limits = options->output_limits;
    if (hwa_event_bundle_measure_work(
            &result->bundle, &options->output_limits, &current_work,
            error, error_size) != 0 ||
        fixed_work >= options->output_limits.max_work_bytes ||
        current_work >=
            options->output_limits.max_work_bytes - fixed_work) {
        hwa_stem_note_error(
            error, error_size, "stem-note workflow exceeds its work limit");
        goto cleanup;
    }
    available_work = options->output_limits.max_work_bytes -
                     fixed_work - current_work;
    request.output_limits.max_work_bytes = available_work / UINT64_C(2);
    if (request.output_limits.max_work_bytes == 0U) {
        hwa_stem_note_error(
            error, error_size, "stem-note workflow exceeds its work limit");
        goto cleanup;
    }
    available_events = options->max_note_events - *note_count;
    if (available_events >
        options->output_limits.max_events - result->bundle.event_count)
        available_events =
            options->output_limits.max_events - result->bundle.event_count;
    request.output_limits.max_events =
        available_events == 0U ? 1U : available_events;
    available_values =
        options->output_limits.max_values - *value_count;
    request.output_limits.max_values =
        available_values == 0U ? 1U : available_values;
    if (hwa_inference_deadline_remaining(
            deadline_started, options->timeout_milliseconds, &remaining,
            error, error_size) != 0)
        goto cleanup;
    request.timeout_milliseconds = remaining;
    if (provider->start(
            provider->context, &request, &task, error, error_size) != 0 ||
        task == NULL) {
        if (error != NULL && error_size != 0U && error[0] == '\0')
            hwa_stem_note_error(error, error_size, "note provider did not start");
        goto cleanup;
    }
    for (;;) {
        if (hwa_inference_deadline_remaining(
                deadline_started, options->timeout_milliseconds, &remaining,
                error, error_size) != 0)
            goto cleanup;
        if (provider->poll(
                provider->context, task, &state, &child_output,
                error, error_size) != 0)
            goto cleanup;
        if (state == HWA_INFERENCE_READY) {
            if (hwa_inference_deadline_check(
                    deadline_started, options->timeout_milliseconds,
                    error, error_size) != 0)
                goto cleanup;
            break;
        }
        if (state != HWA_INFERENCE_PENDING || child_output != NULL) {
            hwa_stem_note_error(error, error_size, "invalid note provider poll");
            goto cleanup;
        }
    }
    if (child_output == NULL ||
        hwa_inference_output_validate_for_request(
            provider, &request, child_output, error, error_size) != 0)
        goto cleanup;
    child = child_output->bundle;
    if (hwa_stem_note_child_shape(
            child_output, stem->audio->id, stem->audio->format.frames,
            &child_provider_id, &child_value_count,
            error, error_size) != 0)
        goto cleanup;
    if (child->event_count > options->max_note_events - *note_count ||
        child->event_count > options->output_limits.max_events -
                                 result->bundle.event_count ||
        child_value_count > options->output_limits.max_values - *value_count ||
        result->bundle.provider_count >= options->output_limits.max_providers) {
        hwa_stem_note_error(
            error, error_size, "merged note output exceeds its row limit");
        goto cleanup;
    }
    if (hwa_event_bundle_measure_work(
            child, &request.output_limits, &child_work,
            error, error_size) != 0 ||
        hwa_stem_note_bundle_work_before_append(
            &result->bundle, child,
            options->output_limits.max_work_bytes, &merged_work,
            error, error_size) != 0 ||
        fixed_work > options->output_limits.max_work_bytes ||
        child_work > options->output_limits.max_work_bytes - fixed_work ||
        merged_work > options->output_limits.max_work_bytes -
                          fixed_work - child_work) {
        if (error != NULL && error_size != 0U && error[0] == '\0')
            hwa_stem_note_error(
                error, error_size,
                "stem-note workflow exceeds its work limit");
        goto cleanup;
    }
    if (hwa_stem_note_id_allocate(
            provider_ids, &provider_id, error, error_size) != 0 ||
        hwa_stem_note_append_provider(
            &result->bundle, &child->providers[0], provider_id,
            error, error_size) != 0 ||
        hwa_stem_note_append_events(
            &result->bundle, child, stem, source_recording_id,
            provider_id, child_provider_id, event_ids,
            error, error_size) != 0)
        goto cleanup;
    *note_count += child->event_count;
    *value_count += child_value_count;
    if (hwa_inference_deadline_check(
            deadline_started, options->timeout_milliseconds,
            error, error_size) != 0)
        goto cleanup;
    status = 0;
cleanup:
    if (task != NULL && provider->task_free != NULL)
        provider->task_free(provider->context, task);
    return status;
}

int hwa_stem_note_derivation_run(
    const HWAEventBundle *stem_bundle,
    const HWAStemNoteAudioSource *stem_sources,
    size_t stem_source_count,
    HWAInferenceProvider *note_provider,
    const HWAStemNoteDerivationOptions *options,
    HWAStemNoteDerivation **result_value,
    char *error,
    size_t error_size)
{
    HWAStemNoteDerivation *result = NULL;
    HWAStemNoteRow *rows = NULL;
    const HWAEventAudio *source_audio = NULL;
    HWAStemNoteIdAllocator provider_ids;
    HWAStemNoteIdAllocator event_ids;
    uint64_t deadline_started = 0U;
    uint64_t fixed_work = 0U;
    uint64_t input_work = 0U;
    uint64_t result_work = 0U;
    size_t note_count = 0U;
    size_t value_count;
    size_t row_count = 0U;
    size_t index;
    int status = -1;
    memset(&provider_ids, 0, sizeof(provider_ids));
    memset(&event_ids, 0, sizeof(event_ids));
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (result_value == NULL) {
        hwa_stem_note_error(error, error_size, "stem-note result is null");
        return -1;
    }
    *result_value = NULL;
    if (stem_bundle == NULL || note_provider == NULL || options == NULL ||
        options->note_task == NULL || options->note_task[0] == '\0' ||
        options->note_settings_json == NULL ||
        options->max_input_file_bytes == 0U ||
        options->max_input_bytes == 0U ||
        options->max_input_file_bytes > options->max_input_bytes ||
        options->timeout_milliseconds == 0U ||
        options->max_note_events == 0U || note_provider->start == NULL ||
        note_provider->poll == NULL || note_provider->task_free == NULL) {
        hwa_stem_note_error(error, error_size, "invalid stem-note arguments");
        return -1;
    }
    if (note_provider->name == NULL || note_provider->name[0] == '\0' ||
        note_provider->version == NULL || note_provider->version[0] == '\0' ||
        note_provider->model_sha256 == NULL ||
        note_provider->destroy == NULL) {
        hwa_stem_note_error(
            error, error_size, "invalid note provider descriptor");
        return -1;
    }
    if (hwa_inference_deadline_start(
            &deadline_started, error, error_size) != 0 ||
        hwa_event_bundle_validate(
            stem_bundle, &options->output_limits,
            error, error_size) != 0 ||
        hwa_instrument_stem_bundle_validate_v1(
            stem_bundle, error, error_size) != 0)
        return -1;
    if (hwa_inference_deadline_check(
            deadline_started, options->timeout_milliseconds,
            error, error_size) != 0)
        return -1;
    if (hwa_stem_note_collect_rows(
            stem_bundle, stem_sources, stem_source_count, options,
            deadline_started, &rows, &row_count, &source_audio,
            error, error_size) != 0)
        goto cleanup;
    if (row_count > options->output_limits.max_providers ||
        stem_bundle->provider_count >
            options->output_limits.max_providers - row_count ||
        stem_bundle->event_count > options->output_limits.max_events ||
        row_count > SIZE_MAX / sizeof(*result->payloads)) {
        hwa_stem_note_error(
            error, error_size, "stem-note base rows exceed output limits");
        goto cleanup;
    }
    if (hwa_event_bundle_measure_work(
            stem_bundle, &options->output_limits, &input_work,
            error, error_size) != 0 ||
        input_work > options->output_limits.max_work_bytes) {
        hwa_stem_note_error(
            error, error_size, "stem-note input exceeds its work limit");
        goto cleanup;
    }
    fixed_work = input_work;
    if (hwa_stem_note_work_add(
            &fixed_work, 1U, sizeof(*result),
            options->output_limits.max_work_bytes) != 0 ||
        hwa_stem_note_work_add(
            &fixed_work, row_count, sizeof(*rows),
            options->output_limits.max_work_bytes) != 0 ||
        hwa_stem_note_work_add(
            &fixed_work, row_count, sizeof(*result->payloads),
            options->output_limits.max_work_bytes) != 0 ||
        hwa_stem_note_work_add(
            &fixed_work, stem_bundle->provider_count, sizeof(uint64_t),
            options->output_limits.max_work_bytes) != 0 ||
        hwa_stem_note_work_add(
            &fixed_work, stem_bundle->event_count, sizeof(uint64_t),
            options->output_limits.max_work_bytes) != 0) {
        hwa_stem_note_error(
            error, error_size, "stem-note support work exceeds its limit");
        goto cleanup;
    }
    if (input_work >
        options->output_limits.max_work_bytes - fixed_work) {
        hwa_stem_note_error(
            error, error_size, "stem-note base copy exceeds its work limit");
        goto cleanup;
    }
    result = (HWAStemNoteDerivation *)calloc(1U, sizeof(*result));
    if (result == NULL) {
        hwa_stem_note_error(error, error_size, "cannot allocate stem-note result");
        goto cleanup;
    }
    if (hwa_stem_note_clone_bundle(&result->bundle, stem_bundle) != 0) {
        hwa_stem_note_error(error, error_size, "cannot copy instrument-stem bundle");
        goto cleanup;
    }
    if (hwa_event_bundle_measure_work(
            &result->bundle, &options->output_limits, &result_work,
            error, error_size) != 0 ||
        result_work > options->output_limits.max_work_bytes - fixed_work) {
        hwa_stem_note_error(
            error, error_size, "stem-note base copy exceeds its work limit");
        goto cleanup;
    }
    result->payloads = (HWAInferencePayload *)calloc(
        row_count, sizeof(*result->payloads));
    if (result->payloads == NULL) {
        hwa_stem_note_error(error, error_size, "cannot allocate stem payload rows");
        goto cleanup;
    }
    for (index = 0U; index < row_count; ++index) {
        HWAInferencePayload *payload = &result->payloads[index];
        payload->relative_path =
            result->bundle.audio[rows[index].audio_row].relative_path;
        payload->bytes = rows[index].source->bytes;
        payload->bytes.name = payload->relative_path;
    }
    result->output.bundle = &result->bundle;
    result->output.payloads = result->payloads;
    result->output.payload_count = row_count;
    if (hwa_stem_note_id_allocator_init(
            &provider_ids, stem_bundle->provider_count,
            options->output_limits.max_work_bytes,
            error, error_size) != 0 ||
        hwa_stem_note_id_allocator_init(
            &event_ids, stem_bundle->event_count,
            options->output_limits.max_work_bytes,
            error, error_size) != 0)
        goto cleanup;
    for (index = 0U; index < stem_bundle->provider_count; ++index)
        provider_ids.used[index] = stem_bundle->providers[index].id;
    for (index = 0U; index < stem_bundle->event_count; ++index)
        event_ids.used[index] = stem_bundle->events[index].id;
    if (provider_ids.used_count > 1U)
        qsort(provider_ids.used, provider_ids.used_count,
              sizeof(*provider_ids.used), hwa_stem_note_u64_compare);
    if (event_ids.used_count > 1U)
        qsort(event_ids.used, event_ids.used_count,
              sizeof(*event_ids.used), hwa_stem_note_u64_compare);
    value_count = hwa_stem_note_value_count(&result->bundle);
    if (value_count == SIZE_MAX ||
        value_count > options->output_limits.max_values) {
        hwa_stem_note_error(error, error_size, "base event values overflow");
        goto cleanup;
    }
    for (index = 0U; index < row_count; ++index) {
        if (hwa_stem_note_run_one(
                result, &rows[index], source_audio->id, note_provider,
                options, deadline_started, fixed_work,
                &provider_ids, &event_ids,
                &note_count, &value_count, error, error_size) != 0)
            goto cleanup;
    }
    if (hwa_inference_output_validate(
            &result->output, &options->output_limits,
            error, error_size) != 0 ||
        hwa_inference_deadline_check(
            deadline_started, options->timeout_milliseconds,
            error, error_size) != 0)
        goto cleanup;
    *result_value = result;
    result = NULL;
    status = 0;
cleanup:
    hwa_stem_note_id_allocator_free(&provider_ids);
    hwa_stem_note_id_allocator_free(&event_ids);
    free(rows);
    hwa_stem_note_derivation_free(result);
    return status;
}

const HWAInferenceOutput *hwa_stem_note_derivation_output(
    const HWAStemNoteDerivation *result)
{
    return result == NULL ? NULL : &result->output;
}

void hwa_stem_note_derivation_free(HWAStemNoteDerivation *result)
{
    if (result == NULL) return;
    hwa_event_bundle_free(&result->bundle);
    free(result->payloads);
    free(result);
}

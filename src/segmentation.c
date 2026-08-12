#include "segmentation.h"

#include "alignment_file.h"
#include "internal.h"
#include "item_file.h"
#include "sha256.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HWA_SEG_DEFAULT_WORK UINT64_C(268435456)
#define HWA_SEG_DEFAULT_EVALUATIONS UINT64_C(50000000)
#define HWA_SEG_MAX_FIELD_BYTES UINT64_C(1048576)
#define HWA_SEG_PITCH_CONFIDENCE 0.50
#define HWA_SEG_ONSET_FLOOR 0.08
#define HWA_SEG_SETTLED_ONSET 0.20

typedef struct HWAWork {
    uint64_t bytes;
    uint64_t retained_bytes;
    uint64_t limit;
} HWAWork;

typedef enum HWAEventClass {
    HWA_EVENT_NOTE = 1,
    HWA_EVENT_REST = 2,
    HWA_EVENT_ORNAMENT = 3,
    HWA_EVENT_CADENZA = 4
} HWAEventClass;

typedef struct HWAEventAux {
    HWAEventClass class_id;
    int midi_note;
    size_t note_group;
    uint64_t parent_item_id;
    int midi_valid;
} HWAEventAux;

typedef struct HWANoteGroup {
    size_t first_event;
    size_t last_event;
    size_t event_count;
    uint64_t start_sample;
    uint64_t end_sample;
    uint64_t attack_start;
    uint64_t body_start;
    uint64_t release_start;
    uint64_t release_end;
    uint64_t tail_end;
    uint64_t note_item_id;
    uint64_t attack_item_id;
    uint64_t body_item_id;
    uint64_t release_item_id;
    uint64_t tail_item_id;
    size_t simultaneous_count;
    double start_beat;
    double end_beat;
    double alignment_confidence;
    double onset_confidence;
    double body_confidence;
    double release_confidence;
    uint32_t quality_flags;
    int midi_note;
    const char *voice;
} HWANoteGroup;

typedef struct HWATieOrder {
    size_t event_index;
    const char *voice;
    int midi_note;
    double start_beat;
    uint64_t event_id;
} HWATieOrder;

typedef struct HWAItemBuilder {
    HWAItemSet *result;
    const HWASegmentationOptions *options;
    HWAWork work;
    size_t item_capacity;
    size_t member_capacity;
    size_t warning_capacity;
    uint64_t evaluations;
    uint32_t sample_rate;
    uint64_t total_samples;
} HWAItemBuilder;

typedef struct HWABoundary {
    uint64_t sample;
    size_t frame_index;
    double confidence;
    uint32_t evidence;
    int found;
} HWABoundary;

typedef struct HWASweepPoint {
    uint64_t sample;
    size_t group_index;
    int start;
} HWASweepPoint;

typedef struct HWAScoreGroupOrder {
    size_t group_index;
    double start_beat;
    int midi_note;
    const char *event_id;
    uint64_t numeric_event_id;
} HWAScoreGroupOrder;

typedef struct HWAKeyIndex {
    const char *key;
    size_t index;
} HWAKeyIndex;

typedef struct HWAGroupOrder {
    size_t group_index;
    const char *voice;
    double start_beat;
    int midi_note;
    uint64_t event_id;
} HWAGroupOrder;

static double hwa_seg_clamp(double value, double low, double high)
{
    return value < low ? low : value > high ? high : value;
}

static int hwa_seg_add_bytes(HWAWork *work, size_t count, size_t size)
{
    uint64_t amount;

    if (count != 0U && size > SIZE_MAX / count) {
        return -1;
    }
    amount = (uint64_t)count * (uint64_t)size;
    if (amount > UINT64_MAX - work->bytes ||
        work->bytes + amount > work->limit) {
        return -1;
    }
    work->bytes += amount;
    return 0;
}

static int hwa_seg_add_retained_bytes(HWAWork *work,
                                      size_t count,
                                      size_t size)
{
    uint64_t before = work->bytes;
    uint64_t amount;

    if (hwa_seg_add_bytes(work, count, size) != 0) return -1;
    amount = work->bytes - before;
    if (amount > UINT64_MAX - work->retained_bytes) return -1;
    work->retained_bytes += amount;
    return 0;
}

static void hwa_seg_release_bytes(HWAWork *work, size_t count, size_t size)
{
    uint64_t amount = (uint64_t)count * (uint64_t)size;

    work->bytes -= amount;
}

static void hwa_seg_release_key(HWAWork *work, char *key)
{
    if (key != NULL) {
        hwa_seg_release_bytes(work, strlen(key) + 1U, 1U);
        free(key);
    }
}

static char *hwa_seg_copy_string(HWAWork *work, const char *source)
{
    size_t length;
    char *copy;

    if (source == NULL) {
        return NULL;
    }
    length = strlen(source);
    if (length == SIZE_MAX ||
        hwa_seg_add_retained_bytes(work, length + 1U, 1U) != 0) {
        return NULL;
    }
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, source, length + 1U);
    return copy;
}

static void hwa_seg_free_labels(HWATypedLabels *labels)
{
    free(labels->pitch);
    free(labels->register_name);
    free(labels->dynamic);
    free(labels->articulation);
    free(labels->part);
    free(labels->physical_element);
    free(labels->controller);
    free(labels->technique);
    free(labels->score_section);
    free(labels->transition);
    free(labels->gesture);
    memset(labels, 0, sizeof(*labels));
}

static void hwa_seg_free_event(HWAItemEvent *event)
{
    free(event->event_id);
    free(event->kind);
    free(event->voice);
    free(event->midi_note);
    free(event->velocity);
    free(event->tie);
    free(event->dynamic);
    free(event->mark);
    free(event->score_position);
    hwa_seg_free_labels(&event->labels);
    memset(event, 0, sizeof(*event));
}

void hwa_item_set_free(HWAItemSet *items)
{
    size_t index;

    if (items == NULL) {
        return;
    }
    for (index = 0U; index < items->event_count; ++index) {
        hwa_seg_free_event(&items->events[index]);
    }
    for (index = 0U; index < items->item_count; ++index) {
        free(items->items[index].key);
        free(items->items[index].role);
        free(items->items[index].exclusion_reason);
    }
    for (index = 0U; index < items->warning_count; ++index) {
        free(items->warnings[index].code);
        free(items->warnings[index].message);
    }
    free(items->events);
    free(items->items);
    free(items->members);
    free(items->warnings);
    free(items->alignment_path);
    free(items->audio_path);
    free(items->labels_path);
    free(items->amendment_path);
    free(items->source_score_path);
    memset(items, 0, sizeof(*items));
}

void hwa_segmentation_options_default(HWASegmentationOptions *options)
{
    if (options == NULL) {
        return;
    }
    memset(options, 0, sizeof(*options));
    options->decode_block_frames = 4096U;
    options->max_input_bytes = UINT64_C(17179869184);
    options->max_input_frames = UINT64_C(2000000000);
    options->max_analysis_work_bytes = HWA_SEG_DEFAULT_WORK;
    options->max_transforms = 200000U;
    options->max_track_points = 200000U;
    options->boundary_search_seconds = 0.15;
    options->tail_limit_seconds = 1.5;
    options->min_phase_seconds = 0.020;
    options->min_body_seconds = 0.050;
    options->item_confidence_threshold = 0.45;
    options->max_segmentation_work_bytes = HWA_SEG_DEFAULT_WORK;
    options->max_boundary_evaluations = HWA_SEG_DEFAULT_EVALUATIONS;
    options->max_events = 200000U;
    options->max_items = 1000000U;
    options->max_item_members = 2000000U;
    options->max_label_rows = 200000U;
    options->max_manual_items = 100000U;
}

static int hwa_seg_options_valid(const HWASegmentationOptions *options)
{
    return options != NULL && options->decode_block_frames != 0U &&
           options->max_input_bytes != 0U &&
           options->max_input_frames != 0U &&
           options->max_analysis_work_bytes != 0U &&
           options->max_transforms != 0U &&
           options->max_track_points != 0U &&
           isfinite(options->boundary_search_seconds) &&
           options->boundary_search_seconds >= 0.0 &&
           options->boundary_search_seconds <= 10.0 &&
           isfinite(options->tail_limit_seconds) &&
           options->tail_limit_seconds >= 0.0 &&
           options->tail_limit_seconds <= 10.0 &&
           isfinite(options->min_phase_seconds) &&
           options->min_phase_seconds >= 0.0 &&
           options->min_phase_seconds <= 1.0 &&
           isfinite(options->min_body_seconds) &&
           options->min_body_seconds >= 0.0 &&
           options->min_body_seconds <= 2.0 &&
           isfinite(options->item_confidence_threshold) &&
           options->item_confidence_threshold >= 0.0 &&
           options->item_confidence_threshold <= 1.0 &&
           options->max_segmentation_work_bytes != 0U &&
           options->max_boundary_evaluations != 0U &&
           options->max_events != 0U && options->max_items != 0U &&
           options->max_item_members != 0U &&
           options->max_label_rows != 0U &&
           options->max_manual_items != 0U;
}

static int hwa_seg_parse_midi(const char *text, int *value)
{
    char *end = NULL;
    long parsed;

    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        parsed < 0L || parsed > 127L) {
        return -1;
    }
    *value = (int)parsed;
    return 0;
}

static int hwa_seg_event_class(const char *kind, HWAEventClass *class_id)
{
    if (kind != NULL && strcmp(kind, "note") == 0) {
        *class_id = HWA_EVENT_NOTE;
    } else if (kind != NULL && strcmp(kind, "rest") == 0) {
        *class_id = HWA_EVENT_REST;
    } else if (kind != NULL && strcmp(kind, "ornament") == 0) {
        *class_id = HWA_EVENT_ORNAMENT;
    } else if (kind != NULL && strcmp(kind, "cadenza") == 0) {
        *class_id = HWA_EVENT_CADENZA;
    } else {
        return -1;
    }
    return 0;
}

static int hwa_seg_beat_equal(double left, double right)
{
    double scale = fmax(1.0, fmax(fabs(left), fabs(right)));
    return fabs(left - right) <= 1e-9 * scale;
}

static int hwa_seg_seconds_to_sample(double seconds,
                                     uint32_t rate,
                                     uint64_t total,
                                     uint64_t *sample)
{
    long double scaled;
    long double rounded;

    if (!isfinite(seconds) || seconds < 0.0 || rate == 0U) {
        return -1;
    }
    scaled = (long double)seconds * (long double)rate;
    if (scaled > (long double)UINT64_MAX - 0.5L) {
        return -1;
    }
    rounded = floorl(scaled + 0.5L);
    if (rounded > (long double)total) {
        long double tolerance = fmaxl(1.0L, (long double)total) * 1e-12L;
        if (rounded - (long double)total > tolerance) {
            return -1;
        }
        rounded = (long double)total;
    }
    *sample = (uint64_t)rounded;
    return 0;
}

static double hwa_seg_sample_seconds(const HWAItemBuilder *builder,
                                     uint64_t sample)
{
    return (double)sample / (double)builder->sample_rate;
}

static int hwa_seg_grow_items(HWAItemBuilder *builder)
{
    size_t capacity = builder->item_capacity == 0U
                          ? 64U : builder->item_capacity * 2U;
    HWAItem *grown;

    if (capacity < builder->item_capacity ||
        capacity > builder->options->max_items) {
        capacity = builder->options->max_items;
    }
    if (capacity <= builder->item_capacity ||
        hwa_seg_add_retained_bytes(&builder->work,
                                   capacity - builder->item_capacity,
                                   sizeof(*grown)) != 0) {
        return -1;
    }
    grown = (HWAItem *)realloc(builder->result->items,
                               capacity * sizeof(*grown));
    if (grown == NULL) {
        return -1;
    }
    memset(grown + builder->item_capacity, 0,
           (capacity - builder->item_capacity) * sizeof(*grown));
    builder->result->items = grown;
    builder->item_capacity = capacity;
    return 0;
}

static int hwa_seg_grow_members(HWAItemBuilder *builder)
{
    size_t capacity = builder->member_capacity == 0U
                          ? 128U : builder->member_capacity * 2U;
    HWAItemMember *grown;

    if (capacity < builder->member_capacity ||
        capacity > builder->options->max_item_members) {
        capacity = builder->options->max_item_members;
    }
    if (capacity <= builder->member_capacity ||
        hwa_seg_add_retained_bytes(&builder->work,
                                   capacity - builder->member_capacity,
                                   sizeof(*grown)) != 0) {
        return -1;
    }
    grown = (HWAItemMember *)realloc(builder->result->members,
                                     capacity * sizeof(*grown));
    if (grown == NULL) {
        return -1;
    }
    builder->result->members = grown;
    builder->member_capacity = capacity;
    return 0;
}

static int hwa_seg_grow_warnings(HWAItemBuilder *builder)
{
    size_t limit = builder->options->max_items;
    size_t capacity = builder->warning_capacity == 0U
                          ? 16U : builder->warning_capacity * 2U;
    HWAItemWarning *grown;

    if (capacity < builder->warning_capacity || capacity > limit) {
        capacity = limit;
    }
    if (capacity <= builder->warning_capacity ||
        hwa_seg_add_retained_bytes(&builder->work,
                                   capacity - builder->warning_capacity,
                                   sizeof(*grown)) != 0) {
        return -1;
    }
    grown = (HWAItemWarning *)realloc(builder->result->warnings,
                                      capacity * sizeof(*grown));
    if (grown == NULL) {
        return -1;
    }
    memset(grown + builder->warning_capacity, 0,
           (capacity - builder->warning_capacity) * sizeof(*grown));
    builder->result->warnings = grown;
    builder->warning_capacity = capacity;
    return 0;
}

static int hwa_seg_add_warning(HWAItemBuilder *builder,
                               const char *code,
                               const char *message,
                               uint64_t item_id,
                               uint64_t event_id)
{
    HWAItemWarning *warning;

    if (builder->result->warning_count == builder->warning_capacity &&
        hwa_seg_grow_warnings(builder) != 0) {
        return -1;
    }
    warning = &builder->result->warnings[builder->result->warning_count];
    warning->id = (uint64_t)builder->result->warning_count + 1U;
    warning->code = hwa_seg_copy_string(&builder->work, code);
    warning->message = hwa_seg_copy_string(&builder->work, message);
    if (warning->code == NULL || warning->message == NULL) {
        free(warning->code);
        free(warning->message);
        memset(warning, 0, sizeof(*warning));
        return -1;
    }
    if (item_id != 0U) {
        warning->item_id = item_id;
        warning->item_id_valid = 1;
    }
    if (event_id != 0U) {
        warning->event_id = event_id;
        warning->event_id_valid = 1;
    }
    builder->result->warning_count++;
    return 0;
}

static HWAItem *hwa_seg_add_item(HWAItemBuilder *builder,
                                 HWAItemKind kind,
                                 const char *key,
                                 const char *role,
                                 uint64_t start,
                                 uint64_t end,
                                 double score_start,
                                 double score_end,
                                 double confidence,
                                 uint32_t evidence,
                                 uint32_t quality,
                                 uint64_t parent_id)
{
    HWAItem *item;

    if (start > end || end > builder->total_samples ||
        !isfinite(score_start) || !isfinite(score_end) ||
        score_start < 0.0 || score_end < score_start ||
        !isfinite(confidence)) {
        return NULL;
    }
    if (builder->result->item_count == builder->item_capacity &&
        hwa_seg_grow_items(builder) != 0) {
        return NULL;
    }
    item = &builder->result->items[builder->result->item_count];
    item->id = (uint64_t)builder->result->item_count + 1U;
    item->key = hwa_seg_copy_string(&builder->work, key);
    item->role = hwa_seg_copy_string(&builder->work, role);
    if (item->key == NULL || item->role == NULL) {
        free(item->key);
        free(item->role);
        memset(item, 0, sizeof(*item));
        return NULL;
    }
    item->kind = kind;
    item->start_sample = start;
    item->end_sample = end;
    item->start_seconds = hwa_seg_sample_seconds(builder, start);
    item->end_seconds = hwa_seg_sample_seconds(builder, end);
    item->score_start_beat = score_start;
    item->score_end_beat = score_end;
    item->confidence = hwa_seg_clamp(confidence, 0.0, 1.0);
    item->evidence_flags = evidence;
    item->quality_flags = quality;
    item->origin = HWA_ITEM_ORIGIN_AUTO;
    if (parent_id != 0U) {
        item->parent_id = parent_id;
        item->parent_valid = 1;
    }
    if (start == end) {
        item->quality_flags |= HWA_ITEM_QUALITY_COLLAPSED;
    }
    if (item->confidence < builder->options->item_confidence_threshold) {
        item->quality_flags |= HWA_ITEM_QUALITY_LOW_CONFIDENCE;
    }
    if ((item->evidence_flags &
         (HWA_ITEM_EVIDENCE_ONSET | HWA_ITEM_EVIDENCE_ENERGY |
          HWA_ITEM_EVIDENCE_PITCH | HWA_ITEM_EVIDENCE_SCORE)) == 0U) {
        item->quality_flags |= HWA_ITEM_QUALITY_NO_EVIDENCE;
    }
    builder->result->item_count++;
    return item;
}

static int hwa_seg_add_member(HWAItemBuilder *builder,
                              uint64_t item_id,
                              uint64_t event_id,
                              HWAItemMemberRole role,
                              uint32_t order)
{
    HWAItemMember *member;

    if (item_id == 0U || event_id == 0U) {
        return -1;
    }
    if (builder->result->member_count == builder->member_capacity &&
        hwa_seg_grow_members(builder) != 0) {
        return -1;
    }
    member = &builder->result->members[builder->result->member_count++];
    member->item_id = item_id;
    member->event_id = event_id;
    member->role = role;
    member->order = order;
    return 0;
}

static char *hwa_seg_hex_key(HWAWork *work,
                             const char *role,
                             const char *first,
                             const char *second,
                             size_t ordinal)
{
    static const char digits[] = "0123456789abcdef";
    size_t role_length = strlen(role);
    size_t first_length = first != NULL ? strlen(first) : 0U;
    size_t second_length = second != NULL ? strlen(second) : 0U;
    char ordinal_text[32];
    size_t ordinal_length = 0U;
    size_t length;
    char *key;
    size_t output = 0U;
    size_t index;

    if (ordinal != 0U) {
        int count = snprintf(ordinal_text, sizeof(ordinal_text), "%zu", ordinal);
        if (count < 0 || (size_t)count >= sizeof(ordinal_text)) {
            return NULL;
        }
        ordinal_length = (size_t)count;
    }
    if (first_length > (SIZE_MAX - role_length - 4U) / 2U ||
        second_length >
            (SIZE_MAX - role_length - 4U - first_length * 2U) / 2U) {
        return NULL;
    }
    length = role_length + 1U + first_length * 2U;
    if (second != NULL) {
        length += 1U + second_length * 2U;
    }
    if (ordinal != 0U) {
        if (ordinal_length > SIZE_MAX - length - 1U) {
            return NULL;
        }
        length += 1U + ordinal_length;
    }
    if (hwa_seg_add_bytes(work, length + 1U, 1U) != 0) {
        return NULL;
    }
    key = (char *)malloc(length + 1U);
    if (key == NULL) {
        return NULL;
    }
    memcpy(key + output, role, role_length);
    output += role_length;
    key[output++] = ':';
    for (index = 0U; index < first_length; ++index) {
        unsigned byte = (unsigned)(unsigned char)first[index];
        key[output++] = digits[byte >> 4U];
        key[output++] = digits[byte & 15U];
    }
    if (second != NULL) {
        key[output++] = ':';
        for (index = 0U; index < second_length; ++index) {
            unsigned byte = (unsigned)(unsigned char)second[index];
            key[output++] = digits[byte >> 4U];
            key[output++] = digits[byte & 15U];
        }
    }
    if (ordinal != 0U) {
        key[output++] = ':';
        memcpy(key + output, ordinal_text, ordinal_length);
        output += ordinal_length;
    }
    key[output] = '\0';
    return key;
}

static uint64_t hwa_seg_frame_sample(const HWAAnalysis *analysis, size_t index)
{
    uint64_t start;
    uint64_t center;
    uint64_t half = (uint64_t)analysis->options.frame_size / 2U;
    uint64_t total = analysis->format.frames;

    if ((uint64_t)index > UINT64_MAX /
                              (uint64_t)analysis->options.hop_size) {
        return total;
    }
    start = (uint64_t)index * (uint64_t)analysis->options.hop_size;
    center = start > UINT64_MAX - half ? UINT64_MAX : start + half;
    return center < total ? center : total;
}

static int hwa_seg_evaluate(HWAItemBuilder *builder)
{
    if (builder->evaluations == builder->options->max_boundary_evaluations) {
        return -1;
    }
    builder->evaluations++;
    return 0;
}

static size_t hwa_seg_lower_frame(const HWAAnalysis *analysis, uint64_t sample)
{
    size_t low = 0U;
    size_t high = analysis->track_count;

    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        if (hwa_seg_frame_sample(analysis, middle) < sample) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return low;
}

static HWABoundary hwa_seg_find_onset(HWAItemBuilder *builder,
                                      const HWAAnalysis *analysis,
                                      uint64_t predicted,
                                      uint64_t radius,
                                      uint64_t upper_bound)
{
    HWABoundary result;
    uint64_t start = predicted > radius ? predicted - radius : 0U;
    uint64_t end = predicted > UINT64_MAX - radius
                       ? builder->total_samples : predicted + radius;
    size_t index;
    double best = -1.0;

    memset(&result, 0, sizeof(result));
    if (end > builder->total_samples) {
        end = builder->total_samples;
    }
    if (end > upper_bound) end = upper_bound;
    result.sample = predicted;
    index = hwa_seg_lower_frame(analysis, start);
    for (; index < analysis->track_count; ++index) {
        const HWAFrameMetrics *frame = &analysis->tracks[index];
        uint64_t sample = hwa_seg_frame_sample(analysis, index);
        uint64_t distance;
        double proximity;
        double score;

        if (sample > end) {
            break;
        }
        if (hwa_seg_evaluate(builder) != 0) {
            result.found = -1;
            return result;
        }
        distance = sample > predicted ? sample - predicted : predicted - sample;
        proximity = radius != 0U
                        ? 1.0 - (double)distance / (double)radius : 1.0;
        score = 0.82 * frame->combined_onset_strength +
                0.18 * hwa_seg_clamp(proximity, 0.0, 1.0);
        if (score > best || (score == best && sample < result.sample)) {
            best = score;
            result.sample = sample;
            result.frame_index = index;
            result.confidence = hwa_seg_clamp(score, 0.0, 1.0);
            result.evidence = HWA_ITEM_EVIDENCE_ONSET;
            if (frame->energy_onset_strength > 0.0) {
                result.evidence |= HWA_ITEM_EVIDENCE_ENERGY;
            }
            if (frame->pitch_change_valid != 0 &&
                frame->pitch_change_strength > 0.0) {
                result.evidence |= HWA_ITEM_EVIDENCE_PITCH;
            }
        }
    }
    result.found = best >= HWA_SEG_ONSET_FLOOR;
    if (!result.found) {
        result.sample = predicted;
        result.confidence = 0.0;
        result.evidence = 0U;
    }
    return result;
}

static int hwa_seg_copy_labels(HWAWork *work,
                               const HWAAlignmentMatch *match,
                               const HWATypedLabelRow *row,
                               HWATypedLabels *target)
{
    const HWATypedLabels *source = row != NULL ? &row->labels : NULL;

#define HWA_SEG_COPY_LABEL(field, bit, fallback)                              \
    do {                                                                      \
        const char *selected =                                                \
            source != NULL && (source->override_flags & (bit)) != 0U          \
                ? source->field : (fallback);                                 \
        target->field = hwa_seg_copy_string(work, selected);                  \
        if (selected != NULL && target->field == NULL) {                      \
            return -1;                                                        \
        }                                                                     \
        if (source != NULL && (source->override_flags & (bit)) != 0U) {       \
            target->override_flags |= (bit);                                  \
        }                                                                     \
    } while (0)

    HWA_SEG_COPY_LABEL(pitch, HWA_LABEL_OVERRIDE_PITCH, match->midi_note);
    HWA_SEG_COPY_LABEL(register_name, HWA_LABEL_OVERRIDE_REGISTER, NULL);
    HWA_SEG_COPY_LABEL(dynamic, HWA_LABEL_OVERRIDE_DYNAMIC, match->dynamic);
    HWA_SEG_COPY_LABEL(articulation, HWA_LABEL_OVERRIDE_ARTICULATION, NULL);
    HWA_SEG_COPY_LABEL(part, HWA_LABEL_OVERRIDE_PART, match->voice);
    HWA_SEG_COPY_LABEL(physical_element,
                       HWA_LABEL_OVERRIDE_PHYSICAL_ELEMENT, NULL);
    HWA_SEG_COPY_LABEL(controller, HWA_LABEL_OVERRIDE_CONTROLLER, NULL);
    HWA_SEG_COPY_LABEL(technique, HWA_LABEL_OVERRIDE_TECHNIQUE, NULL);
    HWA_SEG_COPY_LABEL(score_section,
                       HWA_LABEL_OVERRIDE_SCORE_SECTION,
                       match->score_position);
    HWA_SEG_COPY_LABEL(transition, HWA_LABEL_OVERRIDE_TRANSITION, NULL);
    HWA_SEG_COPY_LABEL(gesture, HWA_LABEL_OVERRIDE_GESTURE, NULL);
#undef HWA_SEG_COPY_LABEL
    return 0;
}

static int hwa_seg_copy_event_string(HWAWork *work,
                                     char **target,
                                     const char *source)
{
    *target = hwa_seg_copy_string(work, source);
    return source != NULL && *target == NULL ? -1 : 0;
}

static int hwa_seg_event_key_compare(const void *left, const void *right)
{
    const HWAKeyIndex *first = (const HWAKeyIndex *)left;
    const HWAKeyIndex *second = (const HWAKeyIndex *)right;
    int order = strcmp(first->key, second->key);

    if (order != 0) return order;
    if (first->index < second->index) return -1;
    if (first->index > second->index) return 1;
    return 0;
}

static int hwa_seg_make_events(HWAItemBuilder *builder,
                               const HWAAlignment *alignment,
                               const HWATypedLabelSet *labels,
                               HWAEventAux **owned_aux,
                               char *error,
                               size_t error_size)
{
    HWAEventAux *aux;
    HWAKeyIndex *event_keys;
    unsigned char *label_used = NULL;
    size_t index;

    if (alignment->match_count == 0U ||
        alignment->match_count > builder->options->max_events ||
        alignment->match_count > (size_t)UINT32_MAX ||
        alignment->match_count > SIZE_MAX / sizeof(*builder->result->events) ||
        alignment->match_count > SIZE_MAX / sizeof(*aux)) {
        hwa_set_error(error, error_size,
                      "alignment has no events or exceeds the event limit");
        return -1;
    }
    if (hwa_seg_add_retained_bytes(&builder->work, alignment->match_count,
                                   sizeof(*builder->result->events)) != 0 ||
        hwa_seg_add_bytes(&builder->work, alignment->match_count,
                          sizeof(*aux)) != 0 ||
        hwa_seg_add_bytes(&builder->work, alignment->match_count,
                          sizeof(*event_keys)) != 0 ||
        (labels != NULL &&
         hwa_seg_add_bytes(&builder->work, labels->row_count,
                           sizeof(*label_used)) != 0)) {
        hwa_set_error(error, error_size,
                      "events exceed the segmentation work-byte limit");
        return -1;
    }
    builder->result->events = (HWAItemEvent *)calloc(
        alignment->match_count, sizeof(*builder->result->events));
    aux = (HWAEventAux *)calloc(alignment->match_count, sizeof(*aux));
    event_keys = (HWAKeyIndex *)calloc(alignment->match_count,
                                       sizeof(*event_keys));
    if (labels != NULL && labels->row_count != 0U) {
        label_used = (unsigned char *)calloc(labels->row_count,
                                             sizeof(*label_used));
    }
    if (builder->result->events == NULL || aux == NULL || event_keys == NULL ||
        (labels != NULL && labels->row_count != 0U && label_used == NULL)) {
        free(event_keys);
        free(label_used);
        free(aux);
        hwa_set_error(error, error_size, "out of memory for score events");
        return -1;
    }
    for (index = 0U; index < alignment->match_count; ++index) {
        const char *event_id = alignment->matches[index].event_id;
        if (event_id == NULL || event_id[0] == '\0') {
            hwa_set_error(error, error_size,
                          "alignment match %zu has no event ID", index + 1U);
            free(event_keys);
            free(label_used);
            free(aux);
            return -1;
        }
        event_keys[index].key = event_id;
        event_keys[index].index = index;
    }
    qsort(event_keys, alignment->match_count,
          sizeof(*event_keys), hwa_seg_event_key_compare);
    for (index = 1U; index < alignment->match_count; ++index) {
        if (strcmp(event_keys[index - 1U].key,
                   event_keys[index].key) == 0) {
            hwa_set_error(error, error_size,
                          "alignment repeats event ID '%s'",
                          event_keys[index].key);
            free(event_keys);
            free(label_used);
            free(aux);
            return -1;
        }
    }
    hwa_seg_release_bytes(&builder->work, alignment->match_count,
                          sizeof(*event_keys));
    free(event_keys);
    builder->result->event_count = alignment->match_count;
    for (index = 0U; index < alignment->match_count; ++index) {
        const HWAAlignmentMatch *match = &alignment->matches[index];
        const HWATypedLabelRow *label = NULL;
        HWAItemEvent *event = &builder->result->events[index];
        uint64_t start_sample;
        uint64_t end_sample;

        if (!match->score_span_valid || !match->tempo_valid ||
            match->event_id == NULL || match->event_id[0] == '\0' ||
            !isfinite(match->score_start_beat) ||
            !isfinite(match->score_end_beat) ||
            match->score_start_beat < 0.0 ||
            match->score_end_beat < match->score_start_beat ||
            !isfinite(match->reference_start_seconds) ||
            !isfinite(match->reference_end_seconds) ||
            match->reference_start_seconds < 0.0 ||
            match->reference_end_seconds < match->reference_start_seconds ||
            !isfinite(match->tempo_bpm) || match->tempo_bpm <= 0.0 ||
            !isfinite(match->confidence) || match->confidence < 0.0 ||
            match->confidence > 1.0 ||
            hwa_seg_event_class(match->kind, &aux[index].class_id) != 0 ||
            hwa_seg_seconds_to_sample(match->target_start_seconds,
                                      builder->sample_rate,
                                      builder->total_samples,
                                      &start_sample) != 0 ||
            hwa_seg_seconds_to_sample(match->target_end_seconds,
                                      builder->sample_rate,
                                      builder->total_samples,
                                      &end_sample) != 0 ||
            end_sample < start_sample) {
            hwa_set_error(error, error_size,
                          "alignment match %zu is not a usable score event",
                          index + 1U);
            free(label_used);
            free(aux);
            return -1;
        }
        if (aux[index].class_id == HWA_EVENT_NOTE ||
            aux[index].class_id == HWA_EVENT_ORNAMENT) {
            if (hwa_seg_parse_midi(match->midi_note,
                                   &aux[index].midi_note) != 0) {
                hwa_set_error(error, error_size,
                              "score event '%s' has an invalid MIDI pitch",
                              match->event_id);
                free(label_used);
                free(aux);
                return -1;
            }
            aux[index].midi_valid = 1;
        }
        if (labels != NULL) {
            size_t label_index;

            label = hwa_typed_labels_find(labels, match->event_id);
            if (label != NULL) {
                label_index = (size_t)(label - labels->rows);
                if (label_index >= labels->row_count) {
                    hwa_set_error(error, error_size,
                                  "typed-label lookup returned an invalid row");
                    free(label_used);
                    free(aux);
                    return -1;
                }
                label_used[label_index] = 1U;
            }
        }
        event->id = (uint64_t)index + 1U;
        if (hwa_seg_copy_event_string(&builder->work, &event->event_id,
                                      match->event_id) != 0 ||
            hwa_seg_copy_event_string(&builder->work, &event->kind,
                                      match->kind) != 0 ||
            hwa_seg_copy_event_string(&builder->work, &event->voice,
                                      match->voice) != 0 ||
            hwa_seg_copy_event_string(&builder->work, &event->midi_note,
                                      match->midi_note) != 0 ||
            hwa_seg_copy_event_string(&builder->work, &event->velocity,
                                      match->velocity) != 0 ||
            hwa_seg_copy_event_string(&builder->work, &event->tie,
                                      match->tie) != 0 ||
            hwa_seg_copy_event_string(&builder->work, &event->dynamic,
                                      match->dynamic) != 0 ||
            hwa_seg_copy_event_string(&builder->work, &event->mark,
                                      match->mark) != 0 ||
            hwa_seg_copy_event_string(&builder->work, &event->score_position,
                                      match->score_position) != 0 ||
            hwa_seg_copy_labels(&builder->work, match, label,
                                &event->labels) != 0) {
            hwa_set_error(error, error_size,
                          "event strings exceed the work limit or memory");
            free(label_used);
            free(aux);
            return -1;
        }
        event->score_start_beat = match->score_start_beat;
        event->score_end_beat = match->score_end_beat;
        event->score_start_seconds = match->reference_start_seconds;
        event->score_end_seconds = match->reference_end_seconds;
        event->audio_start_sample = start_sample;
        event->audio_end_sample = end_sample;
        event->audio_start_seconds = hwa_seg_sample_seconds(builder,
                                                            start_sample);
        event->audio_end_seconds = hwa_seg_sample_seconds(builder, end_sample);
        event->tempo_bpm = match->tempo_bpm;
        event->alignment_confidence = match->confidence;
        event->alignment_evidence_flags = match->evidence_flags;
        event->alignment_status = match->status;
        event->tempo_valid = match->tempo_valid;
    }
    if (labels != NULL) {
        for (index = 0U; index < labels->row_count; ++index) {
            if (label_used[index] == 0U) {
                hwa_set_error(error, error_size,
                              "typed labels name unknown event_id '%s'",
                              labels->rows[index].event_id);
                free(label_used);
                free(aux);
                return -1;
            }
        }
    }
    if (labels != NULL) {
        hwa_seg_release_bytes(&builder->work, labels->row_count,
                              sizeof(*label_used));
    }
    free(label_used);
    *owned_aux = aux;
    return 0;
}

static int hwa_seg_tie_compare(const void *left, const void *right)
{
    const HWATieOrder *first = (const HWATieOrder *)left;
    const HWATieOrder *second = (const HWATieOrder *)right;
    int voice_order = strcmp(first->voice, second->voice);

    if (voice_order != 0) return voice_order;
    if (first->midi_note < second->midi_note) return -1;
    if (first->midi_note > second->midi_note) return 1;
    if (first->start_beat < second->start_beat) return -1;
    if (first->start_beat > second->start_beat) return 1;
    if (first->event_id < second->event_id) return -1;
    if (first->event_id > second->event_id) return 1;
    return 0;
}

static uint32_t hwa_seg_base_quality(const HWAItemEvent *event,
                                     double threshold);

static int hwa_seg_new_note_group(HWANoteGroup *groups,
                                  size_t capacity,
                                  size_t *count,
                                  const HWAItemEvent *event,
                                  size_t event_index,
                                  int midi_note,
                                  double threshold)
{
    HWANoteGroup *group;

    if (*count == capacity) {
        return -1;
    }
    group = &groups[*count];
    memset(group, 0, sizeof(*group));
    group->first_event = event_index;
    group->last_event = event_index;
    group->event_count = 1U;
    group->start_sample = event->audio_start_sample;
    group->end_sample = event->audio_end_sample;
    group->start_beat = event->score_start_beat;
    group->end_beat = event->score_end_beat;
    group->alignment_confidence = event->alignment_confidence;
    group->quality_flags = hwa_seg_base_quality(event, threshold);
    group->midi_note = midi_note;
    group->voice = event->voice != NULL ? event->voice : "";
    (*count)++;
    return 0;
}

static int hwa_seg_make_note_groups(HWAItemBuilder *builder,
                                    HWAEventAux *aux,
                                    HWANoteGroup **owned_groups,
                                    size_t *owned_count,
                                    size_t *owned_capacity,
                                    char *error,
                                    size_t error_size)
{
    size_t note_count = 0U;
    HWATieOrder *order;
    HWANoteGroup *groups;
    size_t group_count = 0U;
    size_t index;

    for (index = 0U; index < builder->result->event_count; ++index) {
        if (aux[index].class_id == HWA_EVENT_NOTE) {
            note_count++;
        }
        aux[index].note_group = SIZE_MAX;
    }
    if (note_count == 0U) {
        *owned_groups = NULL;
        *owned_count = 0U;
        *owned_capacity = 0U;
        return 0;
    }
    if (hwa_seg_add_bytes(&builder->work, note_count, sizeof(*order)) != 0 ||
        hwa_seg_add_bytes(&builder->work, note_count, sizeof(*groups)) != 0) {
        hwa_set_error(error, error_size,
                      "note grouping exceeds the segmentation work limit");
        return -1;
    }
    order = (HWATieOrder *)calloc(note_count, sizeof(*order));
    groups = (HWANoteGroup *)calloc(note_count, sizeof(*groups));
    if (order == NULL || groups == NULL) {
        free(order);
        free(groups);
        hwa_set_error(error, error_size, "out of memory for note groups");
        return -1;
    }
    note_count = 0U;
    for (index = 0U; index < builder->result->event_count; ++index) {
        const HWAItemEvent *event = &builder->result->events[index];

        if (aux[index].class_id != HWA_EVENT_NOTE) {
            continue;
        }
        order[note_count].event_index = index;
        order[note_count].voice = event->voice != NULL ? event->voice : "";
        order[note_count].midi_note = aux[index].midi_note;
        order[note_count].start_beat = event->score_start_beat;
        order[note_count].event_id = event->id;
        note_count++;
    }
    qsort(order, note_count, sizeof(*order), hwa_seg_tie_compare);
    index = 0U;
    while (index < note_count) {
        size_t end = index + 1U;
        size_t open_group = SIZE_MAX;
        double expected_beat = 0.0;

        while (end < note_count &&
               order[end].midi_note == order[index].midi_note &&
               strcmp(order[end].voice, order[index].voice) == 0) {
            end++;
        }
        while (index < end) {
            size_t event_index = order[index].event_index;
            HWAItemEvent *event = &builder->result->events[event_index];
            const char *tie = event->tie != NULL ? event->tie : "";

            if (tie[0] == '\0' || strcmp(tie, "none") == 0) {
                if (open_group != SIZE_MAX ||
                    hwa_seg_new_note_group(groups, note_count, &group_count,
                                           event, event_index,
                                           order[index].midi_note,
                                           builder->options
                                               ->item_confidence_threshold) != 0) {
                    goto bad_tie;
                }
                aux[event_index].note_group = group_count - 1U;
            } else if (strcmp(tie, "start") == 0) {
                if (open_group != SIZE_MAX ||
                    hwa_seg_new_note_group(groups, note_count, &group_count,
                                           event, event_index,
                                           order[index].midi_note,
                                           builder->options
                                               ->item_confidence_threshold) != 0) {
                    goto bad_tie;
                }
                open_group = group_count - 1U;
                expected_beat = event->score_end_beat;
                aux[event_index].note_group = open_group;
            } else if (strcmp(tie, "continue") == 0 ||
                       strcmp(tie, "stop") == 0) {
                HWANoteGroup *group;
                uint32_t event_quality;

                if (open_group == SIZE_MAX ||
                    !hwa_seg_beat_equal(event->score_start_beat,
                                        expected_beat)) {
                    goto bad_tie;
                }
                group = &groups[open_group];
                group->last_event = event_index;
                group->event_count++;
                if (event->audio_end_sample > group->end_sample) {
                    group->end_sample = event->audio_end_sample;
                }
                group->end_beat = event->score_end_beat;
                if (event->alignment_confidence < group->alignment_confidence) {
                    group->alignment_confidence = event->alignment_confidence;
                }
                event_quality = hwa_seg_base_quality(
                    event, builder->options->item_confidence_threshold);
                group->quality_flags |=
                    event_quality & HWA_ITEM_QUALITY_LOW_CONFIDENCE;
                if ((event_quality & HWA_ITEM_QUALITY_NO_EVIDENCE) == 0U) {
                    group->quality_flags &=
                        ~(uint32_t)HWA_ITEM_QUALITY_NO_EVIDENCE;
                }
                expected_beat = event->score_end_beat;
                aux[event_index].note_group = open_group;
                if (strcmp(tie, "stop") == 0) {
                    open_group = SIZE_MAX;
                }
            } else {
                goto bad_tie;
            }
            index++;
        }
        if (open_group != SIZE_MAX) {
            goto bad_tie;
        }
    }
    free(order);
    hwa_seg_release_bytes(&builder->work, note_count, sizeof(*order));
    *owned_groups = groups;
    *owned_count = group_count;
    *owned_capacity = note_count;
    return 0;

bad_tie:
    hwa_set_error(error, error_size,
                  "alignment contains an invalid note tie chain");
    free(order);
    free(groups);
    return -1;
}

static uint64_t hwa_seg_seconds_count(double seconds, uint32_t rate)
{
    long double value = ceill((long double)seconds * (long double)rate);

    if (value <= 0.0L) return 0U;
    if (value >= (long double)UINT64_MAX) return UINT64_MAX;
    return (uint64_t)value;
}

static uint64_t hwa_seg_attack_start(HWAItemBuilder *builder,
                                     const HWAAnalysis *analysis,
                                     const HWABoundary *onset,
                                     uint64_t radius,
                                     double *confidence,
                                     uint32_t *evidence)
{
    uint64_t start = onset->sample > radius ? onset->sample - radius : 0U;
    size_t first = hwa_seg_lower_frame(analysis, start);
    size_t end = onset->found != 0
                     ? onset->frame_index : hwa_seg_lower_frame(
                           analysis, onset->sample);
    double minimum = 0.0;
    double peak = -300.0;
    double threshold;
    size_t index;
    uint64_t result = onset->sample;
    int have = 0;

    if (end >= analysis->track_count) {
        end = analysis->track_count - 1U;
    }
    for (index = first; index <= end; ++index) {
        double level = analysis->tracks[index].rms_dbfs;
        if (hwa_seg_evaluate(builder) != 0) {
            *confidence = -1.0;
            return result;
        }
        if (!have || level < minimum) minimum = level;
        if (level > peak) peak = level;
        have = 1;
    }
    if (!have || peak - minimum < 2.0) {
        uint64_t minimum_phase = hwa_seg_seconds_count(
            builder->options->min_phase_seconds, builder->sample_rate);
        *confidence = onset->confidence * 0.5;
        *evidence = onset->evidence;
        return onset->sample > minimum_phase
                   ? onset->sample - minimum_phase : 0U;
    }
    threshold = minimum + 0.15 * (peak - minimum);
    for (index = first; index <= end; ++index) {
        const HWAFrameMetrics *frame = &analysis->tracks[index];
        if (hwa_seg_evaluate(builder) != 0) {
            *confidence = -1.0;
            return result;
        }
        if (frame->rms_dbfs >= threshold ||
            frame->energy_onset_strength > 0.05) {
            uint64_t center = hwa_seg_frame_sample(analysis, index);
            uint64_t half_hop = (uint64_t)analysis->options.hop_size / 2U;
            result = center > half_hop ? center - half_hop : 0U;
            break;
        }
    }
    *confidence = hwa_seg_clamp((peak - minimum) / 18.0, 0.0, 1.0);
    *evidence = onset->evidence | HWA_ITEM_EVIDENCE_ENERGY;
    return result;
}

static uint64_t hwa_seg_body_start(HWAItemBuilder *builder,
                                   const HWAAnalysis *analysis,
                                   const HWABoundary *onset,
                                   uint64_t note_end,
                                   double *confidence,
                                   uint32_t *evidence)
{
    uint64_t minimum = hwa_seg_seconds_count(
        builder->options->min_phase_seconds, builder->sample_rate);
    uint64_t earliest = onset->sample > UINT64_MAX - minimum
                            ? UINT64_MAX : onset->sample + minimum;
    uint64_t search_end = onset->sample > UINT64_MAX -
                                               hwa_seg_seconds_count(
                                                   builder->options
                                                       ->boundary_search_seconds,
                                                   builder->sample_rate)
                              ? note_end
                              : onset->sample + hwa_seg_seconds_count(
                                    builder->options->boundary_search_seconds,
                                    builder->sample_rate);
    size_t index;
    unsigned settled = 0U;

    if (search_end > note_end) search_end = note_end;
    index = hwa_seg_lower_frame(analysis, earliest);
    for (; index < analysis->track_count; ++index) {
        const HWAFrameMetrics *frame = &analysis->tracks[index];
        uint64_t sample = hwa_seg_frame_sample(analysis, index);

        if (sample > search_end) break;
        if (hwa_seg_evaluate(builder) != 0) {
            *confidence = -1.0;
            return earliest < note_end ? earliest : note_end;
        }
        if (frame->combined_onset_strength <= HWA_SEG_SETTLED_ONSET) {
            settled++;
        } else {
            settled = 0U;
        }
        if (settled >= 2U) {
            *confidence = hwa_seg_clamp(
                1.0 - frame->combined_onset_strength, 0.0, 1.0);
            *evidence = HWA_ITEM_EVIDENCE_ENERGY;
            if (frame->pitch_valid != 0 &&
                frame->pitch_confidence >= HWA_SEG_PITCH_CONFIDENCE) {
                *confidence = 0.7 * *confidence +
                              0.3 * frame->pitch_confidence;
                *evidence |= HWA_ITEM_EVIDENCE_PITCH;
            }
            return sample;
        }
    }
    *confidence = 0.0;
    *evidence = 0U;
    return earliest < note_end ? earliest : note_end;
}

static uint64_t hwa_seg_release_start(HWAItemBuilder *builder,
                                      const HWAAnalysis *analysis,
                                      uint64_t predicted,
                                      uint64_t radius,
                                      double *confidence,
                                      uint32_t *evidence)
{
    uint64_t start = predicted > radius ? predicted - radius : 0U;
    uint64_t end = predicted > UINT64_MAX - radius
                       ? builder->total_samples : predicted + radius;
    size_t index = hwa_seg_lower_frame(analysis, start);
    double best = -1.0;
    uint64_t result = predicted;

    if (end > builder->total_samples) end = builder->total_samples;
    for (; index < analysis->track_count; ++index) {
        const HWAFrameMetrics *frame = &analysis->tracks[index];
        uint64_t sample = hwa_seg_frame_sample(analysis, index);
        uint64_t distance;
        double drop = 0.0;
        double pitch_change = frame->pitch_change_valid != 0
                                  ? frame->pitch_change_strength : 0.0;
        double proximity;
        double score;

        if (sample > end) break;
        if (hwa_seg_evaluate(builder) != 0) {
            *confidence = -1.0;
            return result;
        }
        if (index != 0U) {
            drop = hwa_seg_clamp(
                (analysis->tracks[index - 1U].rms_dbfs - frame->rms_dbfs) /
                    12.0,
                0.0, 1.0);
        }
        distance = sample > predicted ? sample - predicted : predicted - sample;
        proximity = radius != 0U
                        ? 1.0 - (double)distance / (double)radius : 1.0;
        score = 0.50 * drop + 0.30 * pitch_change +
                0.20 * hwa_seg_clamp(proximity, 0.0, 1.0);
        if (score > best || (score == best && sample < result)) {
            best = score;
            result = sample;
            *confidence = hwa_seg_clamp(score, 0.0, 1.0);
            *evidence = 0U;
            if (drop > 0.0) *evidence |= HWA_ITEM_EVIDENCE_ENERGY;
            if (pitch_change > 0.0) *evidence |= HWA_ITEM_EVIDENCE_PITCH;
        }
    }
    if (best < 0.10) {
        *confidence = 0.0;
        *evidence = 0U;
        return predicted;
    }
    return result;
}

static void hwa_seg_release_ends(HWAItemBuilder *builder,
                                 const HWAAnalysis *analysis,
                                 uint64_t release_start,
                                 uint64_t note_start,
                                 uint64_t *release_end,
                                 uint64_t *tail_end,
                                 double *confidence,
                                 uint32_t *evidence)
{
    uint64_t limit_count = hwa_seg_seconds_count(
        builder->options->tail_limit_seconds, builder->sample_rate);
    uint64_t limit = release_start > UINT64_MAX - limit_count
                         ? builder->total_samples : release_start + limit_count;
    size_t body_first = hwa_seg_lower_frame(analysis, note_start);
    size_t release_first = hwa_seg_lower_frame(analysis, release_start);
    size_t index;
    double body_level = -300.0;
    int have_body = 0;
    unsigned direct_below = 0U;
    unsigned tail_below = 0U;

    if (limit > builder->total_samples) limit = builder->total_samples;
    for (index = body_first;
         index < analysis->track_count && index < release_first; ++index) {
        if (hwa_seg_evaluate(builder) != 0) {
            *confidence = -1.0;
            return;
        }
        if (!have_body || analysis->tracks[index].rms_dbfs > body_level) {
            body_level = analysis->tracks[index].rms_dbfs;
            have_body = 1;
        }
    }
    *release_end = release_start;
    *tail_end = limit;
    for (index = release_first; index < analysis->track_count; ++index) {
        const HWAFrameMetrics *frame = &analysis->tracks[index];
        uint64_t sample = hwa_seg_frame_sample(analysis, index);
        double direct_floor = fmax(analysis->options.silence_threshold_dbfs,
                                   body_level - 12.0);
        double tail_floor = fmax(analysis->options.silence_threshold_dbfs,
                                 body_level - 30.0);

        if (sample > limit) break;
        if (hwa_seg_evaluate(builder) != 0) {
            *confidence = -1.0;
            return;
        }
        direct_below = frame->rms_dbfs <= direct_floor
                           ? (direct_below < 2U ? direct_below + 1U : 2U)
                           : 0U;
        tail_below = frame->rms_dbfs <= tail_floor
                         ? (tail_below < 3U ? tail_below + 1U : 3U)
                         : 0U;
        if (*release_end == release_start && direct_below >= 2U) {
            *release_end = sample;
        }
        if (tail_below >= 3U) {
            *tail_end = sample;
            break;
        }
    }
    if (*release_end == release_start) {
        uint64_t fallback = hwa_seg_seconds_count(
            fmin(0.25, builder->options->tail_limit_seconds),
            builder->sample_rate);
        *release_end = release_start > UINT64_MAX - fallback
                           ? limit : release_start + fallback;
        if (*release_end > limit) *release_end = limit;
        *confidence = 0.20;
    } else {
        *confidence = have_body != 0 ? 0.75 : 0.40;
    }
    if (*tail_end < *release_end) *tail_end = *release_end;
    *evidence = have_body != 0 ? HWA_ITEM_EVIDENCE_ENERGY : 0U;
}

static uint32_t hwa_seg_base_quality(const HWAItemEvent *event,
                                     double threshold)
{
    uint32_t quality = 0U;

    if (event->alignment_confidence < threshold ||
        event->alignment_status == HWA_ALIGNMENT_LOW_CONFIDENCE) {
        quality |= HWA_ITEM_QUALITY_LOW_CONFIDENCE;
    }
    if (event->alignment_evidence_flags == 0U) {
        quality |= HWA_ITEM_QUALITY_NO_EVIDENCE;
    }
    return quality;
}

static uint32_t hwa_seg_merge_group_quality(uint32_t left,
                                            uint32_t right)
{
    uint32_t merged = (left | right) &
                      ~(uint32_t)HWA_ITEM_QUALITY_NO_EVIDENCE;

    if ((left & HWA_ITEM_QUALITY_NO_EVIDENCE) != 0U &&
        (right & HWA_ITEM_QUALITY_NO_EVIDENCE) != 0U) {
        merged |= HWA_ITEM_QUALITY_NO_EVIDENCE;
    }
    return merged;
}

static int hwa_seg_add_note_items(HWAItemBuilder *builder,
                                  const HWAAnalysis *analysis,
                                  HWAEventAux *aux,
                                  HWANoteGroup *groups,
                                  size_t group_count,
                                  char *error,
                                  size_t error_size)
{
    uint64_t radius = hwa_seg_seconds_count(
        builder->options->boundary_search_seconds, builder->sample_rate);
    uint64_t min_body = hwa_seg_seconds_count(
        builder->options->min_body_seconds, builder->sample_rate);
    size_t group_index;

    for (group_index = 0U; group_index < group_count; ++group_index) {
        HWANoteGroup *group = &groups[group_index];
        const HWAItemEvent *first =
            &builder->result->events[group->first_event];
        HWABoundary onset = hwa_seg_find_onset(
            builder, analysis, group->start_sample, radius,
            group->end_sample);
        double attack_confidence;
        double body_confidence;
        double release_start_confidence = 0.0;
        double release_end_confidence;
        uint32_t attack_evidence;
        uint32_t body_evidence;
        uint32_t release_start_evidence = 0U;
        uint32_t release_end_evidence;
        uint32_t quality = group->quality_flags;
        char *key;
        HWAItem *item;

        if (onset.found < 0) goto evaluations;
        group->onset_confidence = onset.confidence;
        group->attack_start = hwa_seg_attack_start(
            builder, analysis, &onset, radius,
            &attack_confidence, &attack_evidence);
        if (attack_confidence < 0.0) goto evaluations;
        group->body_start = hwa_seg_body_start(
            builder, analysis, &onset, group->end_sample,
            &body_confidence, &body_evidence);
        if (body_confidence < 0.0) goto evaluations;
        if (group->attack_start > group->body_start) {
            group->attack_start = group->body_start;
            attack_confidence = 0.0;
            attack_evidence = 0U;
        }
        group->release_start = hwa_seg_release_start(
            builder, analysis, group->end_sample, radius,
            &release_start_confidence, &release_start_evidence);
        if (release_start_confidence < 0.0) goto evaluations;
        if (group->release_start < group->body_start) {
            group->release_start = group->body_start;
        }
        if (group->release_start - group->body_start < min_body) {
            group->body_start = group->release_start;
            body_confidence = 0.0;
        }
        hwa_seg_release_ends(builder, analysis,
                             group->release_start, group->body_start,
                             &group->release_end, &group->tail_end,
                             &release_end_confidence,
                             &release_end_evidence);
        if (release_end_confidence < 0.0) goto evaluations;
        group->body_confidence = body_confidence;
        group->release_confidence = hwa_seg_clamp(
            0.5 * release_start_confidence +
            0.5 * release_end_confidence, 0.0, 1.0);

        key = hwa_seg_hex_key(&builder->work, "note", first->event_id,
                              NULL, 0U);
        if (key == NULL) goto memory;
        item = hwa_seg_add_item(
            builder, HWA_ITEM_NOTE, key, "note",
            group->attack_start, group->release_end,
            group->start_beat, group->end_beat,
            group->alignment_confidence,
            HWA_ITEM_EVIDENCE_ALIGNMENT | HWA_ITEM_EVIDENCE_SCORE |
                onset.evidence,
            quality, 0U);
        hwa_seg_release_key(&builder->work, key);
        if (item == NULL) goto memory;
        group->note_item_id = item->id;

        key = hwa_seg_hex_key(&builder->work, "attack", first->event_id,
                              NULL, 0U);
        if (key == NULL) goto memory;
        item = hwa_seg_add_item(
            builder, HWA_ITEM_ATTACK, key, "attack",
            group->attack_start, group->body_start,
            group->start_beat, group->end_beat,
            fmin(group->alignment_confidence,
                 0.5 * onset.confidence + 0.5 * attack_confidence),
            HWA_ITEM_EVIDENCE_ALIGNMENT | onset.evidence | attack_evidence,
            quality, group->note_item_id);
        hwa_seg_release_key(&builder->work, key);
        if (item == NULL) goto memory;
        group->attack_item_id = item->id;

        key = hwa_seg_hex_key(&builder->work, "body", first->event_id,
                              NULL, 0U);
        if (key == NULL) goto memory;
        item = hwa_seg_add_item(
            builder, HWA_ITEM_BODY, key, "body",
            group->body_start, group->release_start,
            group->start_beat, group->end_beat,
            fmin(group->alignment_confidence, body_confidence),
            HWA_ITEM_EVIDENCE_ALIGNMENT | body_evidence,
            quality, group->note_item_id);
        hwa_seg_release_key(&builder->work, key);
        if (item == NULL) goto memory;
        group->body_item_id = item->id;

        key = hwa_seg_hex_key(&builder->work, "release", first->event_id,
                              NULL, 0U);
        if (key == NULL) goto memory;
        item = hwa_seg_add_item(
            builder, HWA_ITEM_RELEASE, key, "release",
            group->release_start, group->release_end,
            group->start_beat, group->end_beat,
            fmin(group->alignment_confidence, group->release_confidence),
            HWA_ITEM_EVIDENCE_ALIGNMENT | release_start_evidence |
                release_end_evidence,
            quality, group->note_item_id);
        hwa_seg_release_key(&builder->work, key);
        if (item == NULL) goto memory;
        group->release_item_id = item->id;

        key = hwa_seg_hex_key(&builder->work, "tail", first->event_id,
                              NULL, 0U);
        if (key == NULL) goto memory;
        item = hwa_seg_add_item(
            builder, HWA_ITEM_RESIDUAL_TAIL, key, "residual-tail",
            group->release_end, group->tail_end,
            group->start_beat, group->end_beat,
            fmin(group->alignment_confidence, release_end_confidence),
            HWA_ITEM_EVIDENCE_ALIGNMENT | release_end_evidence,
            quality, group->note_item_id);
        hwa_seg_release_key(&builder->work, key);
        if (item == NULL) goto memory;
        group->tail_item_id = item->id;
    }
    /* Add tie members in one event pass rather than one whole pass per group. */
    for (group_index = 0U; group_index < builder->result->event_count;
         ++group_index) {
        size_t note_group = aux[group_index].note_group;
        const HWAItemEvent *event = &builder->result->events[group_index];
        HWANoteGroup *group;

        if (note_group == SIZE_MAX) continue;
        group = &groups[note_group];
        aux[group_index].parent_item_id = group->note_item_id;
        if (hwa_seg_add_member(builder, group->note_item_id, event->id,
                               HWA_ITEM_MEMBER_SOURCE,
                               (uint32_t)group_index) != 0 ||
            hwa_seg_add_member(builder, group->attack_item_id, event->id,
                               HWA_ITEM_MEMBER_SOURCE,
                               (uint32_t)group_index) != 0 ||
            hwa_seg_add_member(builder, group->body_item_id, event->id,
                               HWA_ITEM_MEMBER_SOURCE,
                               (uint32_t)group_index) != 0 ||
            hwa_seg_add_member(builder, group->release_item_id, event->id,
                               HWA_ITEM_MEMBER_SOURCE,
                               (uint32_t)group_index) != 0 ||
            hwa_seg_add_member(builder, group->tail_item_id, event->id,
                               HWA_ITEM_MEMBER_SOURCE,
                               (uint32_t)group_index) != 0) {
            goto memory;
        }
    }
    return 0;

evaluations:
    hwa_set_error(error, error_size,
                  "segmentation boundary-evaluation limit exceeded");
    return -1;
memory:
    hwa_set_error(error, error_size,
                  "segmentation item, member, or work-byte limit exceeded");
    return -1;
}

static int hwa_seg_add_score_items(HWAItemBuilder *builder,
                                   const HWAAnalysis *analysis,
                                   HWAEventAux *aux,
                                   char *error,
                                   size_t error_size)
{
    size_t event_index;
    uint64_t half_phase = hwa_seg_seconds_count(
        builder->options->min_phase_seconds, builder->sample_rate) / 2U;

    for (event_index = 0U; event_index < builder->result->event_count;
         ++event_index) {
        const HWAItemEvent *event = &builder->result->events[event_index];
        const char *role;
        HWAItemKind kind;
        char *key;
        HWAItem *parent;
        uint64_t parent_id;
        uint32_t quality;

        if (aux[event_index].class_id == HWA_EVENT_NOTE) continue;
        if (aux[event_index].class_id == HWA_EVENT_REST) {
            role = "rest";
            kind = HWA_ITEM_REST;
        } else if (aux[event_index].class_id == HWA_EVENT_ORNAMENT) {
            role = "ornament";
            kind = HWA_ITEM_GESTURE;
        } else {
            role = "cadenza";
            kind = HWA_ITEM_GESTURE;
        }
        quality = hwa_seg_base_quality(
            event, builder->options->item_confidence_threshold);
        key = hwa_seg_hex_key(&builder->work, role, event->event_id,
                              NULL, 0U);
        if (key == NULL) goto memory;
        parent = hwa_seg_add_item(
            builder, kind, key, role,
            event->audio_start_sample, event->audio_end_sample,
            event->score_start_beat, event->score_end_beat,
            event->alignment_confidence,
            HWA_ITEM_EVIDENCE_ALIGNMENT | HWA_ITEM_EVIDENCE_SCORE,
            quality, 0U);
        hwa_seg_release_key(&builder->work, key);
        if (parent == NULL ||
            hwa_seg_add_member(builder, parent->id, event->id,
                               HWA_ITEM_MEMBER_SOURCE, 0U) != 0) {
            goto memory;
        }
        parent_id = parent->id;
        aux[event_index].parent_item_id = parent_id;
        if (aux[event_index].class_id == HWA_EVENT_ORNAMENT ||
            aux[event_index].class_id == HWA_EVENT_CADENZA) {
            size_t frame = hwa_seg_lower_frame(
                analysis, event->audio_start_sample);
            uint64_t last_sample = 0U;
            size_t ordinal = 0U;

            for (; frame < analysis->track_count; ++frame) {
                uint64_t sample = hwa_seg_frame_sample(analysis, frame);
                const HWAFrameMetrics *metrics = &analysis->tracks[frame];
                int peak;

                if (sample >= event->audio_end_sample) break;
                if (hwa_seg_evaluate(builder) != 0) {
                    hwa_set_error(error, error_size,
                                  "segmentation boundary-evaluation limit exceeded");
                    return -1;
                }
                peak = metrics->combined_onset_strength >= 0.25 &&
                       (frame == 0U ||
                        metrics->combined_onset_strength >=
                            analysis->tracks[frame - 1U]
                                .combined_onset_strength) &&
                       (frame + 1U == analysis->track_count ||
                        metrics->combined_onset_strength >
                            analysis->tracks[frame + 1U]
                                .combined_onset_strength);
                if (!peak ||
                    (ordinal != 0U && sample - last_sample < half_phase * 2U)) {
                    continue;
                }
                {
                    uint64_t start = sample > half_phase
                                         ? sample - half_phase : 0U;
                    uint64_t end = sample > UINT64_MAX - half_phase
                                       ? builder->total_samples
                                       : sample + half_phase;
                    HWAItem *candidate;

                    if (start < event->audio_start_sample) {
                        start = event->audio_start_sample;
                    }
                    if (end > event->audio_end_sample) {
                        end = event->audio_end_sample;
                    }
                    if (ordinal >= (size_t)UINT32_MAX) goto memory;
                    ordinal++;
                    key = hwa_seg_hex_key(&builder->work,
                                          "acoustic-event",
                                          event->event_id, NULL, ordinal);
                    if (key == NULL) goto memory;
                    candidate = hwa_seg_add_item(
                        builder, HWA_ITEM_GESTURE, key,
                        "acoustic-event-candidate", start, end,
                        event->score_start_beat, event->score_end_beat,
                        fmin(event->alignment_confidence,
                             metrics->combined_onset_strength),
                        HWA_ITEM_EVIDENCE_ALIGNMENT |
                            HWA_ITEM_EVIDENCE_ONSET,
                        quality, parent_id);
                    hwa_seg_release_key(&builder->work, key);
                    if (candidate == NULL ||
                        hwa_seg_add_member(builder, candidate->id, event->id,
                                           HWA_ITEM_MEMBER_SOURCE,
                                           (uint32_t)ordinal) != 0) {
                        goto memory;
                    }
                    last_sample = sample;
                }
            }
        }
    }
    return 0;

memory:
    hwa_set_error(error, error_size,
                  "score-role items exceed an item, member, or work limit");
    return -1;
}

static int hwa_seg_add_typed_event_gestures(HWAItemBuilder *builder,
                                            const HWAEventAux *aux,
                                            char *error,
                                            size_t error_size)
{
    size_t event_index;

    for (event_index = 0U; event_index < builder->result->event_count;
         ++event_index) {
        const HWAItemEvent *event = &builder->result->events[event_index];
        const char *gesture = event->labels.gesture;
        uint64_t parent_id = aux[event_index].parent_item_id;
        const HWAItem *parent;
        uint64_t start;
        uint64_t end;
        double score_start;
        double score_end;
        double confidence;
        uint32_t evidence;
        uint32_t quality;
        char *key;
        HWAItem *item;

        if (gesture == NULL || gesture[0] == '\0') continue;
        if (parent_id == 0U || parent_id > builder->result->item_count) {
            hwa_set_error(error, error_size,
                          "typed gesture event has no parent item");
            return -1;
        }
        parent = &builder->result->items[parent_id - 1U];
        start = parent->start_sample;
        end = parent->end_sample;
        score_start = parent->score_start_beat;
        score_end = parent->score_end_beat;
        confidence = fmin(parent->confidence, event->alignment_confidence);
        evidence = parent->evidence_flags | HWA_ITEM_EVIDENCE_SCORE;
        quality = parent->quality_flags;
        key = hwa_seg_hex_key(&builder->work, "gesture",
                              event->event_id, gesture, 0U);
        if (key == NULL) goto memory;
        item = hwa_seg_add_item(
            builder, HWA_ITEM_GESTURE, key, gesture,
            start, end, score_start, score_end,
            confidence, evidence, quality, parent_id);
        hwa_seg_release_key(&builder->work, key);
        if (item == NULL ||
            hwa_seg_add_member(builder, item->id, event->id,
                               HWA_ITEM_MEMBER_SOURCE, 0U) != 0) {
            goto memory;
        }
    }
    return 0;

memory:
    hwa_set_error(error, error_size,
                  "typed gestures exceed an item, member, or work limit");
    return -1;
}

static int hwa_seg_sweep_compare(const void *left, const void *right)
{
    const HWASweepPoint *first = (const HWASweepPoint *)left;
    const HWASweepPoint *second = (const HWASweepPoint *)right;

    if (first->sample < second->sample) return -1;
    if (first->sample > second->sample) return 1;
    /* Half-open spans end before another note starts at the same sample. */
    if (first->start < second->start) return -1;
    if (first->start > second->start) return 1;
    if (first->group_index < second->group_index) return -1;
    if (first->group_index > second->group_index) return 1;
    return 0;
}

static int hwa_seg_size_compare(const void *left, const void *right)
{
    size_t first = *(const size_t *)left;
    size_t second = *(const size_t *)right;

    if (first < second) return -1;
    if (first > second) return 1;
    return 0;
}

static int hwa_seg_score_group_compare(const void *left, const void *right)
{
    const HWAScoreGroupOrder *first = (const HWAScoreGroupOrder *)left;
    const HWAScoreGroupOrder *second = (const HWAScoreGroupOrder *)right;
    int event_order;

    if (first->start_beat < second->start_beat) return -1;
    if (first->start_beat > second->start_beat) return 1;
    if (first->midi_note < second->midi_note) return -1;
    if (first->midi_note > second->midi_note) return 1;
    event_order = strcmp(first->event_id, second->event_id);
    if (event_order != 0) return event_order;
    if (first->numeric_event_id < second->numeric_event_id) return -1;
    if (first->numeric_event_id > second->numeric_event_id) return 1;
    return 0;
}

static int hwa_seg_add_score_chords(HWAItemBuilder *builder,
                                    HWANoteGroup *groups,
                                    size_t group_count,
                                    char *error,
                                    size_t error_size)
{
    HWAScoreGroupOrder *order;
    size_t index;

    if (group_count < 2U) return 0;
    if (hwa_seg_add_bytes(&builder->work, group_count,
                          sizeof(*order)) != 0) {
        hwa_set_error(error, error_size,
                      "score chord ordering exceeds the work-byte limit");
        return -1;
    }
    order = (HWAScoreGroupOrder *)calloc(group_count, sizeof(*order));
    if (order == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for score chord ordering");
        return -1;
    }
    for (index = 0U; index < group_count; ++index) {
        const HWAItemEvent *event =
            &builder->result->events[groups[index].first_event];
        order[index].group_index = index;
        order[index].start_beat = groups[index].start_beat;
        order[index].midi_note = groups[index].midi_note;
        order[index].event_id = event->event_id;
        order[index].numeric_event_id = event->id;
    }
    qsort(order, group_count, sizeof(*order), hwa_seg_score_group_compare);
    index = 0U;
    while (index < group_count) {
        size_t end = index + 1U;
        size_t member_index;

        while (end < group_count &&
               hwa_seg_beat_equal(order[end].start_beat,
                                  order[index].start_beat)) {
            end++;
        }
        for (member_index = index; member_index < end; ++member_index) {
            groups[order[member_index].group_index].simultaneous_count =
                end - index;
        }
        if (end - index >= 2U) {
            const HWANoteGroup *first_group =
                &groups[order[index].group_index];
            const HWAItemEvent *first_event = &builder->result->events[
                first_group->first_event];
            const HWAItemEvent *last_event = &builder->result->events[
                groups[order[end - 1U].group_index].first_event];
            uint64_t start = first_group->attack_start;
            uint64_t audio_end = first_group->release_end;
            double score_end = first_group->end_beat;
            double confidence = first_group->alignment_confidence;
            uint32_t quality = first_group->quality_flags;
            char *key;
            HWAItem *item;
            uint64_t item_id;

            for (member_index = index + 1U;
                 member_index < end; ++member_index) {
                const HWANoteGroup *group =
                    &groups[order[member_index].group_index];
                if (group->attack_start > start) start = group->attack_start;
                if (group->release_end < audio_end) {
                    audio_end = group->release_end;
                }
                if (group->end_beat > score_end) score_end = group->end_beat;
                if (group->alignment_confidence < confidence) {
                    confidence = group->alignment_confidence;
                }
                quality = hwa_seg_merge_group_quality(
                    quality, group->quality_flags);
            }
            if (audio_end < start) audio_end = start;
            key = hwa_seg_hex_key(&builder->work, "chord",
                                  first_event->event_id,
                                  last_event->event_id, 0U);
            if (key == NULL) goto memory;
            item = hwa_seg_add_item(
                builder, HWA_ITEM_MULTI_NOTE, key, "chord",
                start, audio_end, order[index].start_beat, score_end,
                confidence,
                HWA_ITEM_EVIDENCE_ALIGNMENT | HWA_ITEM_EVIDENCE_SCORE,
                quality, 0U);
            hwa_seg_release_key(&builder->work, key);
            if (item == NULL) goto memory;
            item_id = item->id;
            for (member_index = index;
                 member_index < end; ++member_index) {
                const HWAItemEvent *event = &builder->result->events[
                    groups[order[member_index].group_index].first_event];
                if (hwa_seg_add_member(
                        builder, item_id, event->id,
                        HWA_ITEM_MEMBER_ACTIVE,
                        (uint32_t)(member_index - index)) != 0) {
                    goto memory;
                }
            }
        }
        index = end;
    }
    hwa_seg_release_bytes(&builder->work, group_count, sizeof(*order));
    free(order);
    return 0;

memory:
    free(order);
    hwa_set_error(error, error_size,
                  "score chord items exceed an item, member, or work limit");
    return -1;
}

static int hwa_seg_add_multi_region(HWAItemBuilder *builder,
                                    const HWANoteGroup *groups,
                                    const size_t *region,
                                    size_t region_count,
                                    uint64_t start,
                                    uint64_t end,
                                    size_t ordinal,
                                    char *error,
                                    size_t error_size)
{
    size_t index;
    size_t first_group;
    size_t last_group;
    int same_score_onset = 1;
    double score_start;
    double score_end;
    double confidence;
    uint32_t quality;
    const HWAItemEvent *first_event;
    const HWAItemEvent *last_event;
    char *key;
    HWAItem *item;

    if (region_count < 2U || !(end > start)) return 0;
    first_group = region[0U];
    last_group = region[region_count - 1U];
    score_start = groups[first_group].start_beat;
    score_end = groups[first_group].end_beat;
    confidence = groups[first_group].alignment_confidence;
    quality = groups[first_group].quality_flags;
    for (index = 1U; index < region_count; ++index) {
        const HWANoteGroup *group = &groups[region[index]];
        if (!hwa_seg_beat_equal(group->start_beat,
                                groups[region[0U]].start_beat)) {
            same_score_onset = 0;
        }
        if (group->start_beat < score_start) score_start = group->start_beat;
        if (group->end_beat > score_end) score_end = group->end_beat;
        if (group->alignment_confidence < confidence) {
            confidence = group->alignment_confidence;
        }
        quality = hwa_seg_merge_group_quality(quality,
                                              group->quality_flags);
        if (group->first_event < groups[first_group].first_event) {
            first_group = region[index];
        }
        if (group->first_event > groups[last_group].first_event) {
            last_group = region[index];
        }
    }
    /* The score-chord item already owns this exact active-note subset. */
    if (same_score_onset != 0 &&
        region_count == groups[region[0U]].simultaneous_count) {
        return 0;
    }
    first_event = &builder->result->events[groups[first_group].first_event];
    last_event = &builder->result->events[groups[last_group].first_event];
    key = hwa_seg_hex_key(&builder->work, "multi",
                          first_event->event_id, last_event->event_id,
                          ordinal);
    if (key == NULL) goto memory;
    item = hwa_seg_add_item(
        builder, HWA_ITEM_MULTI_NOTE, key, "multi-note",
        start, end, score_start, score_end, confidence,
        HWA_ITEM_EVIDENCE_ALIGNMENT | HWA_ITEM_EVIDENCE_SCORE,
        quality, 0U);
    hwa_seg_release_key(&builder->work, key);
    if (item == NULL) goto memory;
    for (index = 0U; index < region_count; ++index) {
        const HWANoteGroup *group = &groups[region[index]];
        const HWAItemEvent *event =
            &builder->result->events[group->first_event];
        if (hwa_seg_add_member(builder, item->id, event->id,
                               HWA_ITEM_MEMBER_ACTIVE,
                               (uint32_t)index) != 0) {
            goto memory;
        }
    }
    return 0;

memory:
    hwa_set_error(error, error_size,
                  "multi-note items exceed an item, member, or work limit");
    return -1;
}

static int hwa_seg_add_multi_note_items(HWAItemBuilder *builder,
                                        const HWANoteGroup *groups,
                                        size_t group_count,
                                        char *error,
                                        size_t error_size)
{
    HWASweepPoint *points;
    size_t *active_groups;
    size_t *positions;
    size_t *region;
    size_t point_count = 0U;
    size_t active_count = 0U;
    size_t ordinal = 0U;
    size_t index;

    if (group_count < 2U) return 0;
    if (group_count > SIZE_MAX / 2U) {
        hwa_set_error(error, error_size, "too many note overlap points");
        return -1;
    }
    for (index = 0U; index < group_count; ++index) {
        if (groups[index].release_end > groups[index].attack_start) {
            point_count += 2U;
        }
    }
    if (point_count < 4U) return 0;
    if (hwa_seg_add_bytes(&builder->work, point_count, sizeof(*points)) != 0 ||
        hwa_seg_add_bytes(&builder->work, group_count,
                          sizeof(*active_groups)) != 0 ||
        hwa_seg_add_bytes(&builder->work, group_count,
                          sizeof(*positions)) != 0 ||
        hwa_seg_add_bytes(&builder->work, group_count, sizeof(*region)) != 0) {
        hwa_set_error(error, error_size,
                      "note overlap work exceeds the segmentation limit");
        return -1;
    }
    points = (HWASweepPoint *)calloc(point_count, sizeof(*points));
    active_groups = (size_t *)calloc(group_count, sizeof(*active_groups));
    positions = (size_t *)malloc(group_count * sizeof(*positions));
    region = (size_t *)calloc(group_count, sizeof(*region));
    if (points == NULL || active_groups == NULL || positions == NULL ||
        region == NULL) {
        free(region);
        free(positions);
        free(active_groups);
        free(points);
        hwa_set_error(error, error_size,
                      "out of memory for note overlap work");
        return -1;
    }
    for (index = 0U; index < group_count; ++index) {
        positions[index] = SIZE_MAX;
    }
    point_count = 0U;
    for (index = 0U; index < group_count; ++index) {
        if (groups[index].release_end <= groups[index].attack_start) continue;
        points[point_count].sample = groups[index].attack_start;
        points[point_count].group_index = index;
        points[point_count].start = 1;
        point_count++;
        points[point_count].sample = groups[index].release_end;
        points[point_count].group_index = index;
        points[point_count].start = 0;
        point_count++;
    }
    qsort(points, point_count, sizeof(*points), hwa_seg_sweep_compare);
    index = 0U;
    while (index < point_count) {
        uint64_t sample = points[index].sample;
        size_t bucket_end = index + 1U;
        size_t region_count = 0U;
        uint64_t next_sample;
        size_t group_index;

        while (bucket_end < point_count &&
               points[bucket_end].sample == sample) {
            bucket_end++;
        }
        for (; index < bucket_end; ++index) {
            const HWASweepPoint *point = &points[index];
            size_t position = positions[point->group_index];

            if (point->start != 0 && position == SIZE_MAX) {
                positions[point->group_index] = active_count;
                active_groups[active_count++] = point->group_index;
            } else if (point->start == 0 && position != SIZE_MAX) {
                size_t last_group = active_groups[active_count - 1U];
                active_count--;
                active_groups[position] = last_group;
                positions[last_group] = position;
                positions[point->group_index] = SIZE_MAX;
            }
        }
        if (index == point_count || active_count < 2U) continue;
        next_sample = points[index].sample;
        if (next_sample <= sample) continue;
        if (hwa_seg_evaluate(builder) != 0) goto evaluations;
        for (group_index = 0U; group_index < active_count; ++group_index) {
            if (hwa_seg_evaluate(builder) != 0) goto evaluations;
            region[region_count++] = active_groups[group_index];
        }
        qsort(region, region_count, sizeof(*region), hwa_seg_size_compare);
        ordinal++;
        if (hwa_seg_add_multi_region(
                builder, groups, region, region_count,
                sample, next_sample, ordinal,
                error, error_size) != 0) {
            goto failure;
        }
    }
    hwa_seg_release_bytes(&builder->work, point_count, sizeof(*points));
    hwa_seg_release_bytes(&builder->work, group_count,
                          sizeof(*active_groups));
    hwa_seg_release_bytes(&builder->work, group_count, sizeof(*positions));
    hwa_seg_release_bytes(&builder->work, group_count, sizeof(*region));
    free(region);
    free(positions);
    free(active_groups);
    free(points);
    return 0;

evaluations:
    hwa_set_error(error, error_size,
                  "segmentation boundary/relation-evaluation limit exceeded");
failure:
    free(region);
    free(positions);
    free(active_groups);
    free(points);
    return -1;
}

static int hwa_seg_group_compare(const void *left, const void *right)
{
    const HWAGroupOrder *first = (const HWAGroupOrder *)left;
    const HWAGroupOrder *second = (const HWAGroupOrder *)right;
    int voice_order = strcmp(first->voice, second->voice);

    if (voice_order != 0) return voice_order;
    if (first->start_beat < second->start_beat) return -1;
    if (first->start_beat > second->start_beat) return 1;
    if (first->midi_note < second->midi_note) return -1;
    if (first->midi_note > second->midi_note) return 1;
    if (first->event_id < second->event_id) return -1;
    if (first->event_id > second->event_id) return 1;
    return 0;
}

static int hwa_seg_text(const char *text)
{
    return text != NULL && text[0] != '\0';
}

static int hwa_seg_pitch_continuity(HWAItemBuilder *builder,
                                    const HWAAnalysis *analysis,
                                    const HWANoteGroup *from,
                                    const HWANoteGroup *to,
                                    double *confidence)
{
    size_t index = hwa_seg_lower_frame(analysis, from->release_start);
    double prior = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    size_t valid = 0U;
    size_t directed = 0U;
    int direction = to->midi_note > from->midi_note ? 1 : -1;

    for (; index < analysis->track_count; ++index) {
        const HWAFrameMetrics *frame = &analysis->tracks[index];
        uint64_t sample = hwa_seg_frame_sample(analysis, index);
        double midi;

        if (sample > to->body_start) break;
        if (hwa_seg_evaluate(builder) != 0) return -1;
        if (frame->pitch_valid == 0 || frame->pitch_hz <= 0.0 ||
            frame->pitch_confidence < HWA_SEG_PITCH_CONFIDENCE) {
            continue;
        }
        midi = 69.0 + 12.0 * log2(frame->pitch_hz / 440.0);
        if (valid == 0U) {
            minimum = midi;
            maximum = midi;
        } else {
            double change = midi - prior;
            if (midi < minimum) minimum = midi;
            if (midi > maximum) maximum = midi;
            if ((direction > 0 && change >= -0.05) ||
                (direction < 0 && change <= 0.05)) {
                directed++;
            }
        }
        prior = midi;
        valid++;
    }
    if (valid < 3U || maximum - minimum < 0.5) {
        *confidence = 0.0;
        return 0;
    }
    *confidence = hwa_seg_clamp(
        (double)directed / (double)(valid - 1U), 0.0, 1.0);
    return *confidence >= 0.65;
}

static int hwa_seg_bucket_same_pitches(const HWAGroupOrder *order,
                                       size_t first_start,
                                       size_t first_end,
                                       size_t second_start,
                                       size_t second_end)
{
    size_t first_count = first_end - first_start;
    size_t second_count = second_end - second_start;
    size_t index;

    if (first_count != second_count) return 0;
    for (index = 0U; index < first_count; ++index) {
        if (order[first_start + index].midi_note !=
            order[second_start + index].midi_note) {
            return 0;
        }
    }
    return 1;
}

static int hwa_seg_bucket_transition_label(
    const HWAItemSet *items,
    const HWANoteGroup *groups,
    const HWAGroupOrder *order,
    size_t start,
    size_t end,
    const char **label,
    const char **first_event_id,
    const char **conflict_event_id)
{
    size_t index;

    *label = NULL;
    *first_event_id = NULL;
    *conflict_event_id = NULL;
    for (index = start; index < end; ++index) {
        const HWAItemEvent *event = &items->events[
            groups[order[index].group_index].first_event];
        const char *value = event->labels.transition;

        if (!hwa_seg_text(value)) continue;
        if (*label == NULL) {
            *label = value;
            *first_event_id = event->event_id;
        } else if (strcmp(*label, value) != 0) {
            *conflict_event_id = event->event_id;
            return -1;
        }
    }
    return 0;
}

static int hwa_seg_bucket_has_articulation(
    const HWAItemSet *items,
    const HWANoteGroup *groups,
    const HWAGroupOrder *order,
    size_t start,
    size_t end,
    const char *articulation)
{
    size_t index;

    for (index = start; index < end; ++index) {
        const HWAItemEvent *event = &items->events[
            groups[order[index].group_index].first_event];
        const char *value = event->labels.articulation;

        if (hwa_seg_text(value) && strcmp(value, articulation) == 0) {
            return 1;
        }
    }
    return 0;
}

static int hwa_seg_add_transition_members(HWAItemBuilder *builder,
                                          uint64_t item_id,
                                          const HWANoteGroup *groups,
                                          const HWAGroupOrder *order,
                                          size_t from_start,
                                          size_t from_end,
                                          size_t to_start,
                                          size_t to_end)
{
    size_t index;
    uint32_t member_order = 0U;

    for (index = from_start; index < from_end; ++index) {
        const HWAItemEvent *event = &builder->result->events[
            groups[order[index].group_index].first_event];
        if (hwa_seg_add_member(builder, item_id, event->id,
                               HWA_ITEM_MEMBER_FROM,
                               member_order++) != 0) return -1;
    }
    for (index = to_start; index < to_end; ++index) {
        const HWAItemEvent *event = &builder->result->events[
            groups[order[index].group_index].first_event];
        if (hwa_seg_add_member(builder, item_id, event->id,
                               HWA_ITEM_MEMBER_TO,
                               member_order++) != 0) return -1;
    }
    return 0;
}

static HWAItem *hwa_seg_add_gesture_for_transition(
    HWAItemBuilder *builder,
    const HWANoteGroup *groups,
    const HWAGroupOrder *order,
    size_t from_start,
    size_t from_end,
    size_t to_start,
    size_t to_end,
    const char *role,
    size_t ordinal,
    uint64_t start,
    uint64_t end,
    double score_start,
    double score_end,
    double confidence,
    uint32_t evidence,
    uint32_t quality,
    uint64_t parent_id)
{
    const HWAItemEvent *from_event = &builder->result->events[
        groups[order[from_start].group_index].first_event];
    const HWAItemEvent *to_event = &builder->result->events[
        groups[order[to_start].group_index].first_event];
    char *key = hwa_seg_hex_key(&builder->work, role,
                                from_event->event_id,
                                to_event->event_id, ordinal);
    HWAItem *item;

    if (key == NULL) return NULL;
    item = hwa_seg_add_item(builder, HWA_ITEM_GESTURE, key, role,
                            start, end, score_start, score_end,
                            confidence, evidence, quality, parent_id);
    hwa_seg_release_key(&builder->work, key);
    if (item != NULL &&
        hwa_seg_add_transition_members(builder, item->id, groups, order,
                                       from_start, from_end,
                                       to_start, to_end) != 0) {
        return NULL;
    }
    return item;
}

static int hwa_seg_add_transition(HWAItemBuilder *builder,
                                  const HWAAnalysis *analysis,
                                  const HWANoteGroup *groups,
                                  const HWAGroupOrder *order,
                                  size_t from_start,
                                  size_t from_end,
                                  size_t to_start,
                                  size_t to_end,
                                  size_t ordinal,
                                  char *error,
                                  size_t error_size)
{
    const HWANoteGroup *from = &groups[order[from_start].group_index];
    const HWANoteGroup *to = &groups[order[to_start].group_index];
    const HWAItemEvent *from_event =
        &builder->result->events[from->first_event];
    const HWAItemEvent *to_event =
        &builder->result->events[to->first_event];
    uint64_t from_release_start = from->release_start;
    uint64_t from_release_end = from->release_end;
    uint64_t to_attack_start = to->attack_start;
    uint64_t to_body_start = to->body_start;
    const char *typed_transition;
    const char *transition_event_id;
    const char *conflict_event_id;
    int has_slur = hwa_seg_bucket_has_articulation(
        builder->result, groups, order, to_start, to_end, "slur");
    const char *role;
    double confidence = fmin(from->alignment_confidence,
                             to->alignment_confidence);
    double to_onset_min = to->onset_confidence;
    double to_onset_max = to->onset_confidence;
    uint32_t evidence = HWA_ITEM_EVIDENCE_ALIGNMENT;
    uint32_t quality = hwa_seg_merge_group_quality(
        from->quality_flags, to->quality_flags);
    char *key;
    HWAItem *item;
    uint64_t transition_id;
    size_t index;
    double score_start = from->end_beat < to->start_beat
                             ? from->end_beat : to->start_beat;
    double score_end = from->end_beat > to->start_beat
                           ? from->end_beat : to->start_beat;
    uint64_t start;
    uint64_t end;

    if (hwa_seg_bucket_transition_label(
            builder->result, groups, order, to_start, to_end,
            &typed_transition, &transition_event_id,
            &conflict_event_id) != 0) {
        hwa_set_error(error, error_size,
                      "typed transition roles conflict for events '%s' and '%s'",
                      transition_event_id, conflict_event_id);
        return -1;
    }

    for (index = from_start + 1U; index < from_end; ++index) {
        const HWANoteGroup *group =
            &groups[order[index].group_index];
        if (group->release_start < from_release_start) {
            from_release_start = group->release_start;
        }
        if (group->release_end > from_release_end) {
            from_release_end = group->release_end;
        }
        if (group->end_beat < score_start) score_start = group->end_beat;
        if (group->end_beat > score_end) score_end = group->end_beat;
        if (group->alignment_confidence < confidence) {
            confidence = group->alignment_confidence;
        }
        quality = hwa_seg_merge_group_quality(quality,
                                              group->quality_flags);
    }
    for (index = to_start + 1U; index < to_end; ++index) {
        const HWANoteGroup *group =
            &groups[order[index].group_index];
        if (group->attack_start < to_attack_start) {
            to_attack_start = group->attack_start;
        }
        if (group->body_start > to_body_start) {
            to_body_start = group->body_start;
        }
        if (group->start_beat < score_start) score_start = group->start_beat;
        if (group->start_beat > score_end) score_end = group->start_beat;
        if (group->alignment_confidence < confidence) {
            confidence = group->alignment_confidence;
        }
        quality = hwa_seg_merge_group_quality(quality,
                                              group->quality_flags);
        if (group->onset_confidence < to_onset_min) {
            to_onset_min = group->onset_confidence;
        }
        if (group->onset_confidence > to_onset_max) {
            to_onset_max = group->onset_confidence;
        }
    }
    start = from_release_start < to_attack_start
                ? from_release_start : to_attack_start;
    end = from_release_end > to_body_start
              ? from_release_end : to_body_start;
    if (end < start) end = start;
    if (typed_transition != NULL) {
        role = typed_transition;
        evidence |= HWA_ITEM_EVIDENCE_SCORE;
    } else if (to_attack_start < from_release_end) {
        role = "transition-overlap";
    } else if (to_attack_start > from_release_end) {
        role = "transition-gap";
    } else {
        role = "transition-touch";
    }
    key = hwa_seg_hex_key(&builder->work, "transition",
                          from_event->event_id, to_event->event_id, ordinal);
    if (key == NULL) goto memory;
    item = hwa_seg_add_item(builder, HWA_ITEM_TRANSITION, key, role,
                            start, end, score_start, score_end,
                            confidence, evidence, quality, 0U);
    hwa_seg_release_key(&builder->work, key);
    if (item == NULL ||
        hwa_seg_add_transition_members(builder, item->id, groups, order,
                                       from_start, from_end,
                                       to_start, to_end) != 0) {
        goto memory;
    }
    transition_id = item->id;
    if (has_slur != 0 &&
        hwa_seg_add_gesture_for_transition(
            builder, groups, order, from_start, from_end, to_start, to_end,
            "slur", 1U, start, end, score_start, score_end,
            confidence, evidence | HWA_ITEM_EVIDENCE_SCORE,
            quality,
            transition_id) == NULL) goto memory;
    if (hwa_seg_bucket_same_pitches(order, from_start, from_end,
                                    to_start, to_end) &&
        hwa_seg_add_gesture_for_transition(
            builder, groups, order, from_start, from_end, to_start, to_end,
            "repeated-note", 2U, start, end,
            score_start, score_end,
            fmin(confidence, to_onset_min),
            evidence | (to_onset_min >= HWA_SEG_ONSET_FLOOR
                            ? HWA_ITEM_EVIDENCE_ONSET : 0U),
            quality,
            transition_id) == NULL) goto memory;

    /* A physical-element change is explicit. Audio alone never names it. */
    for (index = from_start; index < from_end; ++index) {
        const HWAItemEvent *left = &builder->result->events[
            groups[order[index].group_index].first_event];
        size_t right_index;
        if (!hwa_seg_text(left->labels.physical_element)) continue;
        for (right_index = to_start; right_index < to_end; ++right_index) {
            const HWAItemEvent *right = &builder->result->events[
                groups[order[right_index].group_index].first_event];
            if (hwa_seg_evaluate(builder) != 0) {
                hwa_set_error(error, error_size,
                              "segmentation boundary-evaluation limit exceeded");
                return -1;
            }
            if (hwa_seg_text(right->labels.physical_element) &&
                strcmp(left->labels.physical_element,
                       right->labels.physical_element) != 0) {
                if (hwa_seg_add_gesture_for_transition(
                        builder, groups, order, from_start, from_end,
                        to_start, to_end, "physical-element-change", 3U,
                        start, end, score_start, score_end,
                        confidence,
                        evidence | HWA_ITEM_EVIDENCE_SCORE,
                        quality,
                        transition_id) == NULL) goto memory;
                index = from_end;
                break;
            }
        }
    }
    if (from_end - from_start == 1U && to_end - to_start == 1U &&
        from->midi_note != to->midi_note) {
        double pitch_confidence;
        int continuous = hwa_seg_pitch_continuity(
            builder, analysis, from, to, &pitch_confidence);
        if (continuous < 0) {
            hwa_set_error(error, error_size,
                          "segmentation boundary-evaluation limit exceeded");
            return -1;
        }
        if (continuous != 0 &&
            hwa_seg_add_gesture_for_transition(
                builder, groups, order, from_start, from_end,
                to_start, to_end, "continuous-pitch-transition", 4U,
                start, end, score_start, score_end,
                fmin(confidence, pitch_confidence),
                evidence | HWA_ITEM_EVIDENCE_PITCH,
                quality,
                transition_id) == NULL) goto memory;
    }
    if (typed_transition == NULL && to_onset_max < 0.20 &&
        to_attack_start <= from_release_end &&
        hwa_seg_add_gesture_for_transition(
            builder, groups, order, from_start, from_end, to_start, to_end,
            "connected-transition-candidate", 5U,
            start, end, score_start, score_end,
            fmin(confidence, 1.0 - to_onset_max),
            evidence | HWA_ITEM_EVIDENCE_ENERGY,
            quality,
            transition_id) == NULL) goto memory;
    return 0;

memory:
    hwa_set_error(error, error_size,
                  "transition items exceed an item, member, or work limit");
    return -1;
}

static int hwa_seg_add_transitions(HWAItemBuilder *builder,
                                   const HWAAnalysis *analysis,
                                   const HWANoteGroup *groups,
                                   size_t group_count,
                                   char *error,
                                   size_t error_size)
{
    HWAGroupOrder *order;
    size_t index;
    size_t ordinal = 0U;

    if (group_count < 2U) return 0;
    if (hwa_seg_add_bytes(&builder->work, group_count,
                          sizeof(*order)) != 0) {
        hwa_set_error(error, error_size,
                      "transition ordering exceeds the work-byte limit");
        return -1;
    }
    order = (HWAGroupOrder *)calloc(group_count, sizeof(*order));
    if (order == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for transition ordering");
        return -1;
    }
    for (index = 0U; index < group_count; ++index) {
        order[index].group_index = index;
        order[index].voice = groups[index].voice;
        order[index].start_beat = groups[index].start_beat;
        order[index].midi_note = groups[index].midi_note;
        order[index].event_id = builder->result->events[
            groups[index].first_event].id;
    }
    qsort(order, group_count, sizeof(*order), hwa_seg_group_compare);
    index = 0U;
    while (index < group_count) {
        size_t voice_end = index + 1U;
        size_t prior_start;
        size_t prior_end;

        while (voice_end < group_count &&
               strcmp(order[voice_end].voice, order[index].voice) == 0) {
            voice_end++;
        }
        prior_start = index;
        prior_end = prior_start + 1U;
        while (prior_end < voice_end &&
               hwa_seg_beat_equal(order[prior_end].start_beat,
                                  order[prior_start].start_beat)) {
            prior_end++;
        }
        while (prior_end < voice_end) {
            size_t next_start = prior_end;
            size_t next_end = next_start + 1U;
            while (next_end < voice_end &&
                   hwa_seg_beat_equal(order[next_end].start_beat,
                                      order[next_start].start_beat)) {
                next_end++;
            }
            ordinal++;
            if (hwa_seg_add_transition(builder, analysis, groups, order,
                                       prior_start, prior_end,
                                       next_start, next_end, ordinal,
                                       error, error_size) != 0) {
                free(order);
                return -1;
            }
            prior_start = next_start;
            prior_end = next_end;
        }
        index = voice_end;
    }
    hwa_seg_release_bytes(&builder->work, group_count, sizeof(*order));
    free(order);
    return 0;
}

static int hwa_seg_key_compare(const void *left, const void *right)
{
    const HWAKeyIndex *first = (const HWAKeyIndex *)left;
    const HWAKeyIndex *second = (const HWAKeyIndex *)right;
    int order = strcmp(first->key, second->key);

    if (order != 0) return order;
    if (first->index < second->index) return -1;
    if (first->index > second->index) return 1;
    return 0;
}

static size_t hwa_seg_find_key(const HWAKeyIndex *items,
                               size_t count,
                               const char *key)
{
    size_t low = 0U;
    size_t high = count;

    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        int order = strcmp(items[middle].key, key);
        if (order < 0) low = middle + 1U;
        else high = middle;
    }
    return low < count && strcmp(items[low].key, key) == 0
               ? items[low].index : SIZE_MAX;
}

static int hwa_seg_apply_edits(HWAItemBuilder *builder,
                               const HWAItemEdit *edits,
                               size_t edit_count,
                               char *error,
                               size_t error_size)
{
    HWAKeyIndex *item_keys = NULL;
    HWAKeyIndex *edit_keys = NULL;
    size_t index;

    if ((edit_count != 0U && edits == NULL) ||
        edit_count > builder->options->max_manual_items ||
        hwa_seg_add_bytes(&builder->work, builder->result->item_count,
                          sizeof(*item_keys)) != 0 ||
        hwa_seg_add_bytes(&builder->work, edit_count,
                          sizeof(*edit_keys)) != 0) {
        hwa_set_error(error, error_size,
                      "manual item edits exceed their count or work limit");
        return -1;
    }
    item_keys = (HWAKeyIndex *)calloc(builder->result->item_count,
                                      sizeof(*item_keys));
    if (edit_count != 0U) {
        edit_keys = (HWAKeyIndex *)calloc(edit_count, sizeof(*edit_keys));
    }
    if (item_keys == NULL || (edit_count != 0U && edit_keys == NULL)) {
        free(edit_keys);
        free(item_keys);
        hwa_set_error(error, error_size,
                      "out of memory while indexing manual item edits");
        return -1;
    }
    for (index = 0U; index < builder->result->item_count; ++index) {
        item_keys[index].key = builder->result->items[index].key;
        item_keys[index].index = index;
    }
    for (index = 0U; index < edit_count; ++index) {
        const HWAItemEdit *edit = &edits[index];
        if (edit->key == NULL || edit->key[0] == '\0' ||
            (edit->locked != 0 && edit->locked != 1) ||
            (edit->exclusion_set != 0 && edit->exclusion_set != 1) ||
            (edit->excluded != 0 && edit->excluded != 1) ||
            (!edit->locked &&
             (edit->start_sample != 0U || edit->end_sample != 0U)) ||
            (edit->locked &&
             (edit->start_sample > edit->end_sample ||
              edit->end_sample > builder->total_samples)) ||
            (!edit->exclusion_set &&
             (edit->excluded || edit->exclusion_reason != NULL)) ||
            (edit->exclusion_set && edit->excluded &&
             (edit->exclusion_reason == NULL ||
              edit->exclusion_reason[0] == '\0')) ||
            (edit->exclusion_set && !edit->excluded &&
             edit->exclusion_reason != NULL &&
             edit->exclusion_reason[0] != '\0')) {
            hwa_set_error(error, error_size,
                          "manual item edit %zu is invalid", index + 1U);
            free(edit_keys);
            free(item_keys);
            return -1;
        }
        edit_keys[index].key = edit->key;
        edit_keys[index].index = index;
    }
    qsort(item_keys, builder->result->item_count,
          sizeof(*item_keys), hwa_seg_key_compare);
    if (edit_count != 0U) {
        qsort(edit_keys, edit_count,
              sizeof(*edit_keys), hwa_seg_key_compare);
    }
    for (index = 1U; index < builder->result->item_count; ++index) {
        if (strcmp(item_keys[index - 1U].key, item_keys[index].key) == 0) {
            hwa_set_error(error, error_size,
                          "automatic segmentation produced a duplicate key");
            free(edit_keys);
            free(item_keys);
            return -1;
        }
    }
    for (index = 1U; index < edit_count; ++index) {
        if (strcmp(edit_keys[index - 1U].key, edit_keys[index].key) == 0) {
            hwa_set_error(error, error_size,
                          "manual item edits repeat key '%s'",
                          edit_keys[index].key);
            free(edit_keys);
            free(item_keys);
            return -1;
        }
    }
    for (index = 0U; index < edit_count; ++index) {
        const HWAItemEdit *edit = &edits[index];
        size_t item_index = hwa_seg_find_key(
            item_keys, builder->result->item_count, edit->key);
        HWAItem *item;

        if (item_index == SIZE_MAX) {
            hwa_set_error(error, error_size,
                          "manual item edit names unknown key '%s'",
                          edit->key);
            free(edit_keys);
            free(item_keys);
            return -1;
        }
        item = &builder->result->items[item_index];
        if (edit->locked) {
            item->start_sample = edit->start_sample;
            item->end_sample = edit->end_sample;
            item->start_seconds = hwa_seg_sample_seconds(
                builder, item->start_sample);
            item->end_seconds = hwa_seg_sample_seconds(
                builder, item->end_sample);
            item->locked = 1;
            item->origin = HWA_ITEM_ORIGIN_MANUAL;
            item->evidence_flags |= HWA_ITEM_EVIDENCE_MANUAL;
            if (item->start_sample == item->end_sample) {
                item->quality_flags |= HWA_ITEM_QUALITY_COLLAPSED;
            } else {
                item->quality_flags &=
                    ~(uint32_t)HWA_ITEM_QUALITY_COLLAPSED;
            }
        }
        if (edit->exclusion_set) {
            char *reason = NULL;
            if (edit->excluded) {
                reason = hwa_seg_copy_string(
                    &builder->work, edit->exclusion_reason);
                if (reason == NULL) {
                    hwa_set_error(error, error_size,
                                  "exclusion reason exceeds the work limit");
                    free(edit_keys);
                    free(item_keys);
                    return -1;
                }
            }
            free(item->exclusion_reason);
            item->exclusion_reason = reason;
            item->excluded = edit->excluded;
            item->origin = HWA_ITEM_ORIGIN_MANUAL;
            item->evidence_flags |= HWA_ITEM_EVIDENCE_MANUAL;
        }
    }
    hwa_seg_release_bytes(&builder->work, builder->result->item_count,
                          sizeof(*item_keys));
    hwa_seg_release_bytes(&builder->work, edit_count, sizeof(*edit_keys));
    free(edit_keys);
    free(item_keys);
    return 0;
}

static void hwa_seg_finish_counts(HWAItemSet *items)
{
    size_t index;

    items->locked_item_count = 0U;
    items->excluded_item_count = 0U;
    items->low_confidence_item_count = 0U;
    for (index = 0U; index < items->item_count; ++index) {
        const HWAItem *item = &items->items[index];
        if (item->locked) items->locked_item_count++;
        if (item->excluded) items->excluded_item_count++;
        if ((item->quality_flags & HWA_ITEM_QUALITY_LOW_CONFIDENCE) != 0U) {
            items->low_confidence_item_count++;
        }
    }
}

int hwa_segmentation_build(const HWAAlignment *alignment,
                           const HWAAnalysis *analysis,
                           const HWATypedLabelSet *labels,
                           const HWASegmentationOptions *options,
                           const HWAItemEdit *edits,
                           size_t edit_count,
                           HWAItemSet *items,
                           char *error,
                           size_t error_size)
{
    HWASegmentationOptions copied_options;
    HWAItemBuilder builder;
    HWAEventAux *aux = NULL;
    HWANoteGroup *groups = NULL;
    size_t group_count = 0U;
    size_t group_capacity = 0U;
    int result = -1;

    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (items == NULL) {
        hwa_set_error(error, error_size, "missing segmentation result");
        return -1;
    }
    if (options != NULL) copied_options = *options;
    else hwa_segmentation_options_default(&copied_options);
    memset(items, 0, sizeof(*items));
    items->options = copied_options;
    if (alignment == NULL || analysis == NULL ||
        !hwa_seg_options_valid(&copied_options) ||
        alignment->mode != HWA_ALIGNMENT_SCORE_TO_AUDIO ||
        alignment->matches == NULL || alignment->match_count == 0U ||
        !isfinite(alignment->reference_duration_seconds) ||
        alignment->reference_duration_seconds < 0.0 ||
        !isfinite(alignment->target_duration_seconds) ||
        alignment->target_duration_seconds < 0.0 ||
        !isfinite(alignment->global_confidence) ||
        alignment->global_confidence < 0.0 ||
        alignment->global_confidence > 1.0 ||
        analysis->tracks == NULL || analysis->track_count == 0U ||
        analysis->track_count > copied_options.max_track_points ||
        analysis->format.sample_rate_hz == 0U ||
        analysis->format.frames == 0U ||
        analysis->format.frames > copied_options.max_input_frames ||
        analysis->options.hop_size == 0U ||
        analysis->options.frame_size == 0U ||
        edit_count > copied_options.max_manual_items ||
        (edit_count != 0U && edits == NULL) ||
        (labels != NULL &&
         (labels->row_count > copied_options.max_label_rows ||
          (labels->row_count != 0U && labels->rows == NULL)))) {
        hwa_set_error(error, error_size,
                      "invalid score segmentation inputs or options");
        return -1;
    }
    memset(&builder, 0, sizeof(builder));
    builder.result = items;
    builder.options = &items->options;
    builder.work.limit = copied_options.max_segmentation_work_bytes;
    builder.sample_rate = analysis->format.sample_rate_hz;
    builder.total_samples = analysis->format.frames;
    items->audio_format = analysis->format;
    items->source_score_duration_seconds =
        alignment->reference_duration_seconds;
    items->alignment_confidence = alignment->global_confidence;
    memcpy(items->source_score_sha256, alignment->score_sha256,
           HWA_SHA256_HEX_SIZE);
    items->source_score_path = hwa_seg_copy_string(
        &builder.work, alignment->score_path);
    if (alignment->score_path != NULL && items->source_score_path == NULL) {
        hwa_set_error(error, error_size,
                      "score path exceeds the segmentation work limit");
        goto cleanup;
    }
    if (hwa_seg_make_events(&builder, alignment, labels, &aux,
                            error, error_size) != 0 ||
        hwa_seg_make_note_groups(&builder, aux, &groups, &group_count,
                                 &group_capacity,
                                 error, error_size) != 0 ||
        hwa_seg_add_note_items(&builder, analysis, aux, groups, group_count,
                               error, error_size) != 0 ||
        hwa_seg_add_score_items(&builder, analysis, aux,
                                error, error_size) != 0 ||
        hwa_seg_add_typed_event_gestures(&builder, aux,
                                         error, error_size) != 0 ||
        hwa_seg_add_score_chords(&builder, groups, group_count,
                                 error, error_size) != 0 ||
        hwa_seg_add_multi_note_items(&builder, groups, group_count,
                                     error, error_size) != 0 ||
        hwa_seg_add_transitions(&builder, analysis, groups, group_count,
                                error, error_size) != 0) {
        goto cleanup;
    }
    if (groups != NULL) {
        hwa_seg_release_bytes(&builder.work, group_capacity, sizeof(*groups));
        free(groups);
        groups = NULL;
    }
    hwa_seg_release_bytes(&builder.work, alignment->match_count, sizeof(*aux));
    free(aux);
    aux = NULL;
    if (hwa_seg_apply_edits(&builder, edits, edit_count,
                            error, error_size) != 0) {
        goto cleanup;
    }
    items->boundary_evaluations = builder.evaluations;
    hwa_seg_finish_counts(items);
    if (items->low_confidence_item_count != 0U &&
        hwa_seg_add_warning(&builder, "low-confidence-items",
                            "One or more item bounds have weak alignment or audio evidence.",
                            0U, 0U) != 0) {
        hwa_set_error(error, error_size,
                      "warning output exceeds the segmentation work limit");
        goto cleanup;
    }
    if (builder.work.bytes != builder.work.retained_bytes) {
        hwa_set_error(error, error_size,
                      "segmentation work ledger did not release scratch storage");
        goto cleanup;
    }
    items->retained_work_bytes = builder.work.retained_bytes;
    result = 0;

cleanup:
    free(groups);
    free(aux);
    if (result != 0) {
        hwa_item_set_free(items);
        items->options = copied_options;
    }
    return result;
}

static char *hwa_seg_plain_copy(const char *text, uint64_t limit)
{
    size_t length;
    char *copy;

    if (text == NULL) return NULL;
    length = strlen(text);
    if (length == SIZE_MAX || (uint64_t)length + 1U > limit) return NULL;
    copy = (char *)malloc(length + 1U);
    if (copy != NULL) memcpy(copy, text, length + 1U);
    return copy;
}

int hwa_segment_score_alignment_wav(
    const char *alignment_path,
    const char *audio_path,
    const char *labels_path,
    const char *amendment_path,
    const HWASegmentationOptions *options,
    const HWAItemEdit *edits,
    size_t edit_count,
    HWAItemSet *items,
    char *error,
    size_t error_size)
{
    HWASegmentationOptions copied_options;
    HWASegmentationOptions engine_options;
    HWAAlignmentFileLimits alignment_limits;
    HWAItemFileLimits item_limits;
    HWAAlignment alignment;
    HWAAnalysis analysis;
    HWAAnalysisOptions analysis_options;
    HWATypedLabelSet labels;
    HWAItemEditSet edit_set;
    const HWAItemEdit *effective_edits = edits;
    size_t effective_edit_count = edit_count;
    char alignment_hash[HWA_SHA256_HEX_SIZE];
    char audio_hash[HWA_SHA256_HEX_SIZE];
    char labels_hash[HWA_SHA256_HEX_SIZE];
    char check_hash[HWA_SHA256_HEX_SIZE];
    uint64_t parser_bytes;
    uint64_t provenance_bytes = 0U;
    uint64_t segmentation_remaining;
    int have_alignment = 0;
    int have_analysis = 0;
    int have_labels = 0;
    int have_edits = 0;
    int built = 0;
    int result = -1;

    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (items == NULL) {
        hwa_set_error(error, error_size, "missing segmentation result");
        return -1;
    }
    if (options != NULL) copied_options = *options;
    else hwa_segmentation_options_default(&copied_options);
    memset(items, 0, sizeof(*items));
    items->options = copied_options;
    memset(&alignment, 0, sizeof(alignment));
    memset(&analysis, 0, sizeof(analysis));
    memset(&labels, 0, sizeof(labels));
    memset(&edit_set, 0, sizeof(edit_set));
    alignment_hash[0] = '\0';
    audio_hash[0] = '\0';
    labels_hash[0] = '\0';
    if (!hwa_seg_options_valid(&copied_options) ||
        alignment_path == NULL || alignment_path[0] == '\0' ||
        audio_path == NULL || audio_path[0] == '\0' ||
        (labels_path != NULL && labels_path[0] == '\0') ||
        (amendment_path != NULL &&
         (amendment_path[0] == '\0' || edits != NULL || edit_count != 0U)) ||
        (amendment_path == NULL && edit_count != 0U && edits == NULL)) {
        hwa_set_error(error, error_size,
                      "invalid segmentation paths, edit source, or options");
        return -1;
    }
    {
        const char *paths[] = {
            alignment_path, audio_path, labels_path, amendment_path
        };
        size_t path_index;
        for (path_index = 0U;
             path_index < sizeof(paths) / sizeof(paths[0]); ++path_index) {
            if (paths[path_index] != NULL) {
                size_t length = strlen(paths[path_index]);
                if (length == SIZE_MAX ||
                    (uint64_t)length + 1U >
                        UINT64_MAX - provenance_bytes) {
                    hwa_set_error(error, error_size,
                                  "segmentation path storage overflows");
                    return -1;
                }
                provenance_bytes += (uint64_t)length + 1U;
            }
        }
    }
    if (provenance_bytes >= copied_options.max_segmentation_work_bytes) {
        hwa_set_error(error, error_size,
                      "provenance paths exceed the segmentation work limit");
        return -1;
    }
    segmentation_remaining =
        copied_options.max_segmentation_work_bytes - provenance_bytes;
    parser_bytes = copied_options.max_input_bytes;
    if (hwa_sha256_file(alignment_path, parser_bytes, alignment_hash,
                        error, error_size) != 0) goto cleanup;
    hwa_alignment_file_limits_default(&alignment_limits);
    alignment_limits.max_bytes = parser_bytes;
    alignment_limits.max_work_bytes = segmentation_remaining;
    alignment_limits.max_matches = copied_options.max_events;
    if (alignment_limits.max_field_bytes > HWA_SEG_MAX_FIELD_BYTES) {
        alignment_limits.max_field_bytes = HWA_SEG_MAX_FIELD_BYTES;
    }
    if (hwa_alignment_file_read(alignment_path, &alignment_limits,
                                &alignment, error, error_size) != 0) {
        goto cleanup;
    }
    have_alignment = 1;
    if (alignment.retained_work_bytes > segmentation_remaining) {
        hwa_set_error(error, error_size,
                      "alignment retained work exceeds the segmentation limit");
        goto cleanup;
    }
    segmentation_remaining -= alignment.retained_work_bytes;
    if (alignment.mode != HWA_ALIGNMENT_SCORE_TO_AUDIO) {
        hwa_set_error(error, error_size,
                      "Stage 3 requires a score-to-audio alignment");
        goto cleanup;
    }
    if (hwa_sha256_file(audio_path, copied_options.max_input_bytes,
                        audio_hash, error, error_size) != 0) goto cleanup;
    if (strcmp(audio_hash, alignment.target_sha256) != 0) {
        hwa_set_error(error, error_size,
                      "audio SHA-256 does not match the alignment target");
        goto cleanup;
    }
    analysis_options = alignment.options.analysis;
    analysis_options.decode_block_frames = copied_options.decode_block_frames;
    analysis_options.max_input_bytes = copied_options.max_input_bytes;
    analysis_options.max_input_frames = copied_options.max_input_frames;
    analysis_options.max_work_bytes = copied_options.max_analysis_work_bytes;
    analysis_options.max_transforms = copied_options.max_transforms;
    analysis_options.max_track_points = copied_options.max_track_points;
    analysis_options.collect_tracks = 1;
    analysis_options.collect_spectrogram = 0;
    analysis_options.true_peak_oversample = 1U;
    if (hwa_analyze_wav_with_options(audio_path, &analysis_options,
                                     &analysis, error, error_size) != 0) {
        goto cleanup;
    }
    have_analysis = 1;
    if (analysis.format.frames > copied_options.max_input_frames ||
        analysis.format.sample_rate_hz == 0U ||
        fabs(analysis.format.duration_seconds -
             alignment.target_duration_seconds) >
            0.5 / (double)analysis.format.sample_rate_hz) {
        hwa_set_error(error, error_size,
                      "audio duration does not match the alignment target");
        goto cleanup;
    }
    if (labels_path != NULL) {
        if (labels_path[0] == '\0' || segmentation_remaining == 0U ||
            hwa_typed_labels_load(
                labels_path, parser_bytes,
                segmentation_remaining,
                copied_options.max_label_rows,
                (size_t)HWA_SEG_MAX_FIELD_BYTES,
                &labels, error, error_size) != 0) {
            goto cleanup;
        }
        have_labels = 1;
        if (labels.retained_work_bytes > segmentation_remaining) {
            hwa_set_error(error, error_size,
                          "typed labels retained work exceeds the segmentation limit");
            goto cleanup;
        }
        segmentation_remaining -= labels.retained_work_bytes;
        memcpy(labels_hash, labels.sha256, HWA_SHA256_HEX_SIZE);
    }
    if (amendment_path != NULL) {
        if (segmentation_remaining == 0U) {
            hwa_set_error(error, error_size,
                          "item amendment exceeds the segmentation work limit");
            goto cleanup;
        }
        hwa_item_file_limits_default(&item_limits);
        item_limits.max_bytes = parser_bytes;
        item_limits.max_work_bytes = segmentation_remaining;
        item_limits.max_events = copied_options.max_events;
        item_limits.max_items = copied_options.max_items;
        item_limits.max_manual_items = copied_options.max_manual_items;
        item_limits.max_members = copied_options.max_item_members;
        item_limits.max_warnings = copied_options.max_items;
        if (item_limits.max_field_bytes > HWA_SEG_MAX_FIELD_BYTES) {
            item_limits.max_field_bytes = HWA_SEG_MAX_FIELD_BYTES;
        }
        if (hwa_item_file_read_edits(amendment_path, &item_limits,
                                     &edit_set, error, error_size) != 0 ||
            hwa_item_edit_set_matches(&edit_set, alignment_hash, audio_hash,
                                      labels_hash, error, error_size) != 0) {
            goto cleanup;
        }
        have_edits = 1;
        if (edit_set.retained_work_bytes > segmentation_remaining) {
            hwa_set_error(error, error_size,
                          "item amendment retained work exceeds the segmentation limit");
            goto cleanup;
        }
        segmentation_remaining -= edit_set.retained_work_bytes;
        effective_edits = edit_set.edits;
        effective_edit_count = edit_set.edit_count;
    }
    if (segmentation_remaining == 0U) {
        hwa_set_error(error, error_size,
                      "no segmentation work remains for output items");
        goto cleanup;
    }
    engine_options = copied_options;
    engine_options.max_segmentation_work_bytes = segmentation_remaining;
    if (hwa_segmentation_build(&alignment, &analysis,
                               have_labels ? &labels : NULL,
                               &engine_options,
                               effective_edits, effective_edit_count,
                               items, error, error_size) != 0) {
        goto cleanup;
    }
    built = 1;
    items->options = copied_options;
    if (hwa_sha256_file(alignment_path, parser_bytes, check_hash,
                        error, error_size) != 0 ||
        strcmp(check_hash, alignment_hash) != 0) {
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "alignment changed while segmentation ran");
        }
        goto cleanup;
    }
    if (hwa_sha256_file(audio_path, copied_options.max_input_bytes,
                        check_hash, error, error_size) != 0 ||
        strcmp(check_hash, audio_hash) != 0) {
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "audio changed while segmentation ran");
        }
        goto cleanup;
    }
    if (labels_path != NULL &&
        (hwa_sha256_file(labels_path, parser_bytes, check_hash,
                         error, error_size) != 0 ||
         strcmp(check_hash, labels_hash) != 0)) {
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "typed labels changed while segmentation ran");
        }
        goto cleanup;
    }
    if (amendment_path != NULL &&
        (hwa_sha256_file(amendment_path, parser_bytes, check_hash,
                         error, error_size) != 0 ||
         strcmp(check_hash, edit_set.sha256) != 0)) {
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "item amendment changed while segmentation ran");
        }
        goto cleanup;
    }
    items->alignment_path = hwa_seg_plain_copy(
        alignment_path, copied_options.max_segmentation_work_bytes);
    items->audio_path = hwa_seg_plain_copy(
        audio_path, copied_options.max_segmentation_work_bytes);
    items->labels_path = labels_path != NULL
                             ? hwa_seg_plain_copy(
                                   labels_path,
                                   copied_options.max_segmentation_work_bytes)
                             : NULL;
    items->amendment_path = amendment_path != NULL
                                ? hwa_seg_plain_copy(
                                      amendment_path,
                                      copied_options.max_segmentation_work_bytes)
                                : NULL;
    if (items->alignment_path == NULL || items->audio_path == NULL ||
        (labels_path != NULL && items->labels_path == NULL) ||
        (amendment_path != NULL && items->amendment_path == NULL)) {
        hwa_set_error(error, error_size,
                      "segmentation provenance paths exceed the work limit");
        goto cleanup;
    }
    {
        if (provenance_bytes > copied_options.max_segmentation_work_bytes -
                             items->retained_work_bytes) {
            hwa_set_error(error, error_size,
                          "provenance paths exceed the segmentation work limit");
            goto cleanup;
        }
        items->retained_work_bytes += provenance_bytes;
    }
    memcpy(items->alignment_sha256, alignment_hash, HWA_SHA256_HEX_SIZE);
    memcpy(items->audio_sha256, audio_hash, HWA_SHA256_HEX_SIZE);
    if (labels_path != NULL) {
        memcpy(items->labels_sha256, labels_hash, HWA_SHA256_HEX_SIZE);
    }
    if (amendment_path != NULL) {
        memcpy(items->amendment_sha256, edit_set.sha256,
               HWA_SHA256_HEX_SIZE);
    }
    result = 0;

cleanup:
    if (have_edits) hwa_item_edit_set_free(&edit_set);
    if (have_labels) hwa_typed_labels_free(&labels);
    if (have_analysis) hwa_analysis_free(&analysis);
    if (have_alignment) hwa_alignment_free(&alignment);
    if (result != 0) {
        if (built) hwa_item_set_free(items);
        items->options = copied_options;
    }
    return result;
}

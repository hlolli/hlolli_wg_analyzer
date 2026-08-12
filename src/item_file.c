#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "item_file.h"

#include "alignment_file.h"
#include "internal.h"
#include "sha256.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define HWA_ITEM_FILE_MAX_FIELDS 35U

static int hwa_item_csv_field(FILE *stream, const char *text)
{
    const unsigned char *cursor;
    int quoted = 0;

    if (text == NULL) text = "";
    cursor = (const unsigned char *)text;
    while (*cursor != 0U) {
        if (*cursor == (unsigned char)',' || *cursor == (unsigned char)'"' ||
            *cursor == (unsigned char)'\r' || *cursor == (unsigned char)'\n') {
            quoted = 1;
            break;
        }
        cursor++;
    }
    if (!quoted) return fputs(text, stream) == EOF ? -1 : 0;
    if (fputc('"', stream) == EOF) return -1;
    cursor = (const unsigned char *)text;
    while (*cursor != 0U) {
        if (*cursor == (unsigned char)'"' && fputc('"', stream) == EOF) {
            return -1;
        }
        if (fputc((int)*cursor, stream) == EOF) return -1;
        cursor++;
    }
    return fputc('"', stream) == EOF ? -1 : 0;
}

static int hwa_item_csv_number(FILE *stream, double value)
{
    if (!isfinite(value)) return -1;
    return fprintf(stream, "%.17g", value == 0.0 ? 0.0 : value) < 0 ? -1 : 0;
}

static int hwa_item_csv_optional_u64(FILE *stream,
                                    uint64_t value,
                                    int valid)
{
    return valid && fprintf(stream, "%" PRIu64, value) < 0 ? -1 : 0;
}

static int hwa_item_csv_optional_number(FILE *stream,
                                       double value,
                                       int valid)
{
    return valid ? hwa_item_csv_number(stream, value) : 0;
}

static const char *hwa_item_kind_text(HWAItemKind kind)
{
    switch (kind) {
    case HWA_ITEM_NOTE: return "note";
    case HWA_ITEM_ATTACK: return "attack";
    case HWA_ITEM_BODY: return "body";
    case HWA_ITEM_RELEASE: return "release";
    case HWA_ITEM_RESIDUAL_TAIL: return "residual-tail";
    case HWA_ITEM_REST: return "rest";
    case HWA_ITEM_TRANSITION: return "transition";
    case HWA_ITEM_GESTURE: return "gesture";
    case HWA_ITEM_MULTI_NOTE: return "multi-note";
    default: return NULL;
    }
}

static const char *hwa_item_origin_text(HWAItemOrigin origin)
{
    return origin == HWA_ITEM_ORIGIN_AUTO ? "auto" :
           origin == HWA_ITEM_ORIGIN_MANUAL ? "manual" : NULL;
}

static const char *hwa_item_member_role_text(HWAItemMemberRole role)
{
    switch (role) {
    case HWA_ITEM_MEMBER_SOURCE: return "source";
    case HWA_ITEM_MEMBER_FROM: return "from";
    case HWA_ITEM_MEMBER_TO: return "to";
    case HWA_ITEM_MEMBER_ACTIVE: return "active";
    default: return NULL;
    }
}

static const char *hwa_item_alignment_status_text(HWAAlignmentStatus status)
{
    switch (status) {
    case HWA_ALIGNMENT_MATCHED: return "matched";
    case HWA_ALIGNMENT_LOW_CONFIDENCE: return "low-confidence";
    case HWA_ALIGNMENT_SKIPPED: return "skipped";
    case HWA_ALIGNMENT_REPEATED: return "repeated";
    case HWA_ALIGNMENT_REST: return "rest";
    case HWA_ALIGNMENT_ORNAMENT: return "ornament";
    case HWA_ALIGNMENT_CADENZA: return "cadenza";
    default: return NULL;
    }
}

static int hwa_item_write_meta_text(FILE *stream,
                                    const char *key,
                                    const char *value,
                                    const char *unit)
{
    return fputs("META,", stream) == EOF ||
           hwa_item_csv_field(stream, key) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_item_csv_field(stream, value) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_item_csv_field(stream, unit) != 0 ||
           fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_item_write_meta_number(FILE *stream,
                                      const char *key,
                                      double value,
                                      const char *unit)
{
    return fputs("META,", stream) == EOF ||
           hwa_item_csv_field(stream, key) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_item_csv_number(stream, value) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_item_csv_field(stream, unit) != 0 ||
           fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_item_write_meta_u64(FILE *stream,
                                   const char *key,
                                   uint64_t value,
                                   const char *unit)
{
    return fprintf(stream, "META,%s,%" PRIu64 ",%s\r\n",
                   key, value, unit) < 0 ? -1 : 0;
}

static int hwa_item_write_meta_size(FILE *stream,
                                    const char *key,
                                    size_t value,
                                    const char *unit)
{
    return fprintf(stream, "META,%s,%zu,%s\r\n",
                   key, value, unit) < 0 ? -1 : 0;
}

static int hwa_item_write_meta(FILE *stream, const HWAItemSet *items)
{
    const HWASegmentationOptions *option = &items->options;

    return hwa_item_write_meta_text(stream, "tool_version", HWA_VERSION, "") ||
           hwa_item_write_meta_text(stream, "analysis_method_version",
                                    HWA_ANALYSIS_METHOD_VERSION, "") ||
           hwa_item_write_meta_text(stream, "alignment_method_version",
                                    HWA_ALIGNMENT_METHOD_VERSION, "") ||
           hwa_item_write_meta_text(stream, "segmentation_method_version",
                                    HWA_SEGMENTATION_METHOD_VERSION, "") ||
           hwa_item_write_meta_text(stream, "build_compiler_family",
                                    hwa_build_compiler_family(), "") ||
           hwa_item_write_meta_text(stream, "build_compiler_version",
                                    hwa_build_compiler_version(), "") ||
           hwa_item_write_meta_text(stream, "build_c_standard",
                                    hwa_build_c_standard(), "") ||
           hwa_item_write_meta_text(stream, "build_target_os",
                                    hwa_build_target_os(), "") ||
           hwa_item_write_meta_u64(stream, "build_pointer_bits",
                                   hwa_build_pointer_bits(), "bits") ||
           hwa_item_write_meta_text(stream, "build_endianness",
                                    hwa_build_endianness(), "") ||
           hwa_item_write_meta_text(stream, "build_mode",
                                    hwa_build_mode(), "") ||
           hwa_item_write_meta_u64(stream, "sample_rate_hz",
                                   items->audio_format.sample_rate_hz, "Hz") ||
           hwa_item_write_meta_u64(stream, "audio_frames",
                                   items->audio_format.frames, "frames") ||
           hwa_item_write_meta_number(stream, "audio_duration_seconds",
                                      items->audio_format.duration_seconds,
                                      "seconds") ||
           hwa_item_write_meta_number(stream,
                                      "source_score_duration_seconds",
                                      items->source_score_duration_seconds,
                                      "seconds") ||
           hwa_item_write_meta_number(stream, "alignment_confidence",
                                      items->alignment_confidence, "ratio") ||
           hwa_item_write_meta_u64(stream, "boundary_evaluations",
                                   items->boundary_evaluations, "evaluations") ||
           hwa_item_write_meta_u64(stream, "retained_work_bytes",
                                   items->retained_work_bytes, "bytes") ||
           hwa_item_write_meta_size(stream, "decode_block_frames",
                                    option->decode_block_frames, "frames") ||
           hwa_item_write_meta_u64(stream, "max_input_bytes",
                                   option->max_input_bytes, "bytes") ||
           hwa_item_write_meta_u64(stream, "max_input_frames",
                                   option->max_input_frames, "frames") ||
           hwa_item_write_meta_u64(stream, "max_analysis_work_bytes",
                                   option->max_analysis_work_bytes, "bytes") ||
           hwa_item_write_meta_size(stream, "max_transforms",
                                    option->max_transforms, "transforms") ||
           hwa_item_write_meta_size(stream, "max_track_points",
                                    option->max_track_points, "points") ||
           hwa_item_write_meta_number(stream, "boundary_search_seconds",
                                      option->boundary_search_seconds,
                                      "seconds") ||
           hwa_item_write_meta_number(stream, "tail_limit_seconds",
                                      option->tail_limit_seconds, "seconds") ||
           hwa_item_write_meta_number(stream, "min_phase_seconds",
                                      option->min_phase_seconds, "seconds") ||
           hwa_item_write_meta_number(stream, "min_body_seconds",
                                      option->min_body_seconds, "seconds") ||
           hwa_item_write_meta_number(stream, "item_confidence_threshold",
                                      option->item_confidence_threshold,
                                      "ratio") ||
           hwa_item_write_meta_u64(stream, "max_segmentation_work_bytes",
                                   option->max_segmentation_work_bytes,
                                   "bytes") ||
           hwa_item_write_meta_u64(stream, "max_boundary_evaluations",
                                   option->max_boundary_evaluations,
                                   "evaluations") ||
           hwa_item_write_meta_size(stream, "max_events",
                                    option->max_events, "events") ||
           hwa_item_write_meta_size(stream, "max_items",
                                    option->max_items, "items") ||
           hwa_item_write_meta_size(stream, "max_item_members",
                                    option->max_item_members, "members") ||
           hwa_item_write_meta_size(stream, "max_label_rows",
                                    option->max_label_rows, "rows") ||
           hwa_item_write_meta_size(stream, "max_manual_items",
                                    option->max_manual_items, "items") ||
           hwa_item_write_meta_size(stream, "event_count",
                                    items->event_count, "events") ||
           hwa_item_write_meta_size(stream, "item_count",
                                    items->item_count, "items") ||
           hwa_item_write_meta_size(stream, "member_count",
                                    items->member_count, "members") ||
           hwa_item_write_meta_size(stream, "warning_count",
                                    items->warning_count, "warnings") ||
           hwa_item_write_meta_size(stream, "locked_item_count",
                                    items->locked_item_count, "items") ||
           hwa_item_write_meta_size(stream, "excluded_item_count",
                                    items->excluded_item_count, "items") ||
           hwa_item_write_meta_size(stream, "low_confidence_item_count",
                                    items->low_confidence_item_count, "items")
               ? -1
               : 0;
}

static int hwa_item_write_path_hex(FILE *stream, const char *path)
{
    const unsigned char *cursor = (const unsigned char *)path;
    while (*cursor != 0U) {
        if (fprintf(stream, "%02x", (unsigned)*cursor) < 0) return -1;
        cursor++;
    }
    return 0;
}

static int hwa_item_write_input(FILE *stream,
                                const char *role,
                                const char *path,
                                const char *sha256,
                                const HWAFormat *format)
{
    if (path == NULL || path[0] == '\0' || sha256 == NULL ||
        fprintf(stream, "INPUT,%s,", role) < 0 ||
        hwa_item_write_path_hex(stream, path) != 0 ||
        fprintf(stream, ",%s,", sha256) < 0) {
        return -1;
    }
    if (format != NULL) {
        if (hwa_item_csv_number(stream, format->duration_seconds) != 0 ||
            fprintf(stream, ",%" PRIu32 ",%" PRIu64,
                    format->sample_rate_hz, format->frames) < 0) {
            return -1;
        }
    } else if (fputs(",,", stream) == EOF) {
        return -1;
    }
    return fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_item_write_source(FILE *stream, const HWAItemSet *items)
{
    if (items->source_score_path == NULL ||
        fputs("SOURCE,score,", stream) == EOF ||
        hwa_item_write_path_hex(stream, items->source_score_path) != 0 ||
        fprintf(stream, ",%s,", items->source_score_sha256) < 0 ||
        hwa_item_csv_number(stream,
                            items->source_score_duration_seconds) != 0) {
        return -1;
    }
    return fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_item_write_event(FILE *stream,
                                const HWAItemEvent *event,
                                const HWAItemSet *set)
{
    const char *status = hwa_item_alignment_status_text(
        event->alignment_status);
    const char *label_values[] = {
        event->labels.pitch,
        event->labels.register_name,
        event->labels.dynamic,
        event->labels.articulation,
        event->labels.part,
        event->labels.physical_element,
        event->labels.controller,
        event->labels.technique,
        event->labels.score_section,
        event->labels.transition,
        event->labels.gesture
    };
    const char *source_values[] = {
        event->voice, event->midi_note, event->velocity, event->tie,
        event->dynamic, event->mark, event->score_position
    };
    double expected_start;
    double expected_end;
    size_t index;

    if (set->audio_format.sample_rate_hz == 0U) return -1;
    expected_start = (double)event->audio_start_sample /
                     (double)set->audio_format.sample_rate_hz;
    expected_end = (double)event->audio_end_sample /
                   (double)set->audio_format.sample_rate_hz;
    if (event->event_id == NULL || event->event_id[0] == '\0' ||
        event->kind == NULL || event->kind[0] == '\0' || status == NULL ||
        !isfinite(event->score_start_beat) ||
        !isfinite(event->score_end_beat) ||
        !isfinite(event->score_start_seconds) ||
        !isfinite(event->score_end_seconds) ||
        !isfinite(event->audio_start_seconds) ||
        !isfinite(event->audio_end_seconds) ||
        !isfinite(event->alignment_confidence) ||
        event->score_start_beat < 0.0 ||
        event->score_end_beat < event->score_start_beat ||
        event->score_start_seconds < 0.0 ||
        event->score_end_seconds < event->score_start_seconds ||
        event->audio_start_sample > event->audio_end_sample ||
        event->audio_end_sample > set->audio_format.frames ||
        fabs(event->audio_start_seconds - expected_start) > 1e-12 ||
        fabs(event->audio_end_seconds - expected_end) > 1e-12 ||
        event->alignment_confidence < 0.0 ||
        event->alignment_confidence > 1.0 ||
        (event->tempo_valid &&
         (!isfinite(event->tempo_bpm) || event->tempo_bpm <= 0.0)) ||
        fprintf(stream, "EVENT,%" PRIu64 ",", event->id) < 0 ||
        hwa_item_csv_field(stream, event->event_id) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_item_csv_field(stream, event->kind) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_item_csv_number(stream, event->score_start_beat) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_item_csv_number(stream, event->score_end_beat) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_item_csv_number(stream, event->score_start_seconds) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_item_csv_number(stream, event->score_end_seconds) != 0 ||
        fprintf(stream, ",%" PRIu64 ",%" PRIu64 ",",
                event->audio_start_sample, event->audio_end_sample) < 0 ||
        hwa_item_csv_number(stream, event->audio_start_seconds) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_item_csv_number(stream, event->audio_end_seconds) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_item_csv_number(stream, event->alignment_confidence) != 0 ||
        fputc(',', stream) == EOF ||
        fputs(status, stream) == EOF ||
        fprintf(stream, ",%" PRIu32, event->alignment_evidence_flags) < 0) {
        return -1;
    }
    for (index = 0U;
         index < sizeof(source_values) / sizeof(source_values[0]); ++index) {
        if (fputc(',', stream) == EOF ||
            hwa_item_csv_field(stream, source_values[index]) != 0) {
            return -1;
        }
    }
    if (fputc(',', stream) == EOF ||
        hwa_item_csv_optional_number(stream, event->tempo_bpm,
                                     event->tempo_valid) != 0) {
        return -1;
    }
    for (index = 0U;
         index < sizeof(label_values) / sizeof(label_values[0]); ++index) {
        if (fputc(',', stream) == EOF ||
            hwa_item_csv_field(stream, label_values[index]) != 0) {
            return -1;
        }
    }
    return fprintf(stream, ",%" PRIu32 "\r\n",
                   event->labels.override_flags) < 0 ? -1 : 0;
}

static int hwa_item_write_item(FILE *stream,
                               const HWAItem *item,
                               const HWAItemSet *set)
{
    const char *kind = hwa_item_kind_text(item->kind);
    const char *origin = hwa_item_origin_text(item->origin);
    double expected_start;
    double expected_end;

    if (set->audio_format.sample_rate_hz == 0U) return -1;
    expected_start = (double)item->start_sample /
                     (double)set->audio_format.sample_rate_hz;
    expected_end = (double)item->end_sample /
                   (double)set->audio_format.sample_rate_hz;
    if (kind == NULL || origin == NULL || item->key == NULL ||
        item->key[0] == '\0' || item->role == NULL || item->role[0] == '\0' ||
        !isfinite(item->start_seconds) || !isfinite(item->end_seconds) ||
        !isfinite(item->score_start_beat) ||
        !isfinite(item->score_end_beat) || !isfinite(item->confidence) ||
        item->start_sample > item->end_sample ||
        item->end_sample > set->audio_format.frames ||
        fabs(item->start_seconds - expected_start) > 1e-12 ||
        fabs(item->end_seconds - expected_end) > 1e-12 ||
        item->score_start_beat < 0.0 ||
        item->score_end_beat < item->score_start_beat ||
        item->confidence < 0.0 || item->confidence > 1.0 ||
        (item->excluded &&
         (item->exclusion_reason == NULL ||
          item->exclusion_reason[0] == '\0')) ||
        (!item->excluded && item->exclusion_reason != NULL &&
         item->exclusion_reason[0] != '\0') ||
        fprintf(stream, "ITEM,%" PRIu64 ",", item->id) < 0 ||
        hwa_item_csv_field(stream, item->key) != 0 ||
        fputc(',', stream) == EOF || fputs(kind, stream) == EOF ||
        fputc(',', stream) == EOF ||
        hwa_item_csv_field(stream, item->role) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_item_csv_optional_u64(stream, item->parent_id,
                                  item->parent_valid) != 0 ||
        fprintf(stream, ",%" PRIu64 ",%" PRIu64 ",",
                item->start_sample, item->end_sample) < 0 ||
        hwa_item_csv_number(stream, item->start_seconds) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_item_csv_number(stream, item->end_seconds) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_item_csv_number(stream, item->score_start_beat) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_item_csv_number(stream, item->score_end_beat) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_item_csv_number(stream, item->confidence) != 0 ||
        fprintf(stream, ",%" PRIu32 ",%" PRIu32 ",%s,%d,%d,",
                item->evidence_flags, item->quality_flags, origin,
                item->locked ? 1 : 0, item->excluded ? 1 : 0) < 0 ||
        hwa_item_csv_field(stream, item->exclusion_reason) != 0) {
        return -1;
    }
    return fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_item_write_member(FILE *stream,
                                 const HWAItemMember *member)
{
    const char *role = hwa_item_member_role_text(member->role);
    return role == NULL ||
           fprintf(stream, "MEMBER,%" PRIu64 ",%" PRIu64 ",%" PRIu32 ",%s\r\n",
                   member->item_id, member->event_id,
                   member->order, role) < 0 ? -1 : 0;
}

static int hwa_item_write_warning(FILE *stream,
                                  const HWAItemWarning *warning)
{
    if (warning->code == NULL || warning->code[0] == '\0' ||
        warning->message == NULL ||
        fprintf(stream, "WARNING,%" PRIu64 ",", warning->id) < 0 ||
        hwa_item_csv_field(stream, warning->code) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_item_csv_field(stream, warning->message) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_item_csv_optional_u64(stream, warning->item_id,
                                  warning->item_id_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_item_csv_optional_u64(stream, warning->event_id,
                                  warning->event_id_valid) != 0) {
        return -1;
    }
    return fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_item_pointer_id_compare(const void *left, const void *right)
{
    const uint64_t left_id = **(const uint64_t *const *)left;
    const uint64_t right_id = **(const uint64_t *const *)right;
    return left_id < right_id ? -1 : left_id > right_id ? 1 : 0;
}

static int hwa_item_member_pointer_compare(const void *left,
                                           const void *right)
{
    const HWAItemMember *a = *(const HWAItemMember *const *)left;
    const HWAItemMember *b = *(const HWAItemMember *const *)right;

    if (a->item_id != b->item_id) return a->item_id < b->item_id ? -1 : 1;
    if (a->order != b->order) return a->order < b->order ? -1 : 1;
    if (a->event_id != b->event_id) return a->event_id < b->event_id ? -1 : 1;
    return a->role < b->role ? -1 : a->role > b->role ? 1 : 0;
}

static void **hwa_item_sorted_pointers(const void *base,
                                       size_t count,
                                       size_t item_size,
                                       int (*compare)(const void *,
                                                      const void *))
{
    const unsigned char *bytes = (const unsigned char *)base;
    void **pointers;
    size_t index;

    if (count == 0U) return NULL;
    if (base == NULL || count > SIZE_MAX / sizeof(*pointers) ||
        count > SIZE_MAX / item_size) {
        return NULL;
    }
    pointers = (void **)malloc(count * sizeof(*pointers));
    if (pointers == NULL) return NULL;
    for (index = 0U; index < count; ++index) {
        pointers[index] = (void *)(bytes + index * item_size);
    }
    qsort(pointers, count, sizeof(*pointers), compare);
    return pointers;
}

static int hwa_item_valid_sha256(const char *text)
{
    size_t index;
    if (text == NULL || strlen(text) != HWA_SHA256_HEX_SIZE - 1U) return 0;
    for (index = 0U; index < HWA_SHA256_HEX_SIZE - 1U; ++index) {
        if (!((text[index] >= '0' && text[index] <= '9') ||
              (text[index] >= 'a' && text[index] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

int hwa_item_file_write(FILE *stream,
                        const HWAItemSet *items,
                        char *error,
                        size_t error_size)
{
    void **events = NULL;
    void **sorted_items = NULL;
    void **members = NULL;
    void **warnings = NULL;
    uint64_t pointer_count;
    uint64_t pointer_bytes;
    size_t index;
    int result = -1;

    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (stream == NULL || items == NULL ||
        items->audio_format.sample_rate_hz == 0U ||
        !hwa_item_valid_sha256(items->alignment_sha256) ||
        !hwa_item_valid_sha256(items->audio_sha256) ||
        !hwa_item_valid_sha256(items->source_score_sha256) ||
        (items->labels_path != NULL &&
         !hwa_item_valid_sha256(items->labels_sha256)) ||
        (items->labels_path == NULL && items->labels_sha256[0] != '\0') ||
        (items->amendment_path != NULL &&
         !hwa_item_valid_sha256(items->amendment_sha256)) ||
        (items->amendment_path == NULL &&
         items->amendment_sha256[0] != '\0') ||
        (items->event_count != 0U && items->events == NULL) ||
        (items->item_count != 0U && items->items == NULL) ||
        (items->member_count != 0U && items->members == NULL) ||
        (items->warning_count != 0U && items->warnings == NULL) ||
        items->event_count > items->options.max_events ||
        items->item_count > items->options.max_items ||
        items->member_count > items->options.max_item_members) {
        hwa_set_error(error, error_size, "invalid item-file arguments");
        return -1;
    }
    pointer_count = (uint64_t)items->event_count;
    if ((uint64_t)items->item_count > UINT64_MAX - pointer_count) goto overflow;
    pointer_count += (uint64_t)items->item_count;
    if ((uint64_t)items->member_count > UINT64_MAX - pointer_count) goto overflow;
    pointer_count += (uint64_t)items->member_count;
    if ((uint64_t)items->warning_count > UINT64_MAX - pointer_count) {
        goto overflow;
    }
    pointer_count += (uint64_t)items->warning_count;
    if (pointer_count > UINT64_MAX / (uint64_t)sizeof(void *)) goto overflow;
    pointer_bytes = pointer_count * (uint64_t)sizeof(void *);
    if (items->retained_work_bytes >
            items->options.max_segmentation_work_bytes ||
        pointer_bytes > items->options.max_segmentation_work_bytes -
                            items->retained_work_bytes) {
        hwa_set_error(error, error_size,
                      "canonical items exceed the segmentation work-byte limit");
        return -1;
    }
    events = hwa_item_sorted_pointers(
        items->events, items->event_count, sizeof(*items->events),
        hwa_item_pointer_id_compare);
    sorted_items = hwa_item_sorted_pointers(
        items->items, items->item_count, sizeof(*items->items),
        hwa_item_pointer_id_compare);
    members = hwa_item_sorted_pointers(
        items->members, items->member_count, sizeof(*items->members),
        hwa_item_member_pointer_compare);
    warnings = hwa_item_sorted_pointers(
        items->warnings, items->warning_count, sizeof(*items->warnings),
        hwa_item_pointer_id_compare);
    if ((items->event_count != 0U && events == NULL) ||
        (items->item_count != 0U && sorted_items == NULL) ||
        (items->member_count != 0U && members == NULL) ||
        (items->warning_count != 0U && warnings == NULL)) {
        hwa_set_error(error, error_size,
                      "out of memory for canonical item output");
        goto cleanup;
    }
    if (fputs("HWA_ITEMS,1\r\n", stream) == EOF ||
        hwa_item_write_meta(stream, items) != 0 ||
        hwa_item_write_input(stream, "alignment", items->alignment_path,
                             items->alignment_sha256, NULL) != 0 ||
        hwa_item_write_input(stream, "audio", items->audio_path,
                             items->audio_sha256, &items->audio_format) != 0 ||
        (items->labels_path != NULL &&
         hwa_item_write_input(stream, "labels", items->labels_path,
                              items->labels_sha256, NULL) != 0) ||
        (items->amendment_path != NULL &&
         hwa_item_write_input(stream, "amendment", items->amendment_path,
                              items->amendment_sha256, NULL) != 0) ||
        hwa_item_write_source(stream, items) != 0) {
        hwa_set_error(error, error_size, "cannot write item metadata");
        goto cleanup;
    }
    for (index = 0U; index < items->event_count; ++index) {
        const HWAItemEvent *event = (const HWAItemEvent *)events[index];
        if (event->id != (uint64_t)index + 1U ||
            hwa_item_write_event(stream, event, items) != 0) {
            hwa_set_error(error, error_size,
                          "cannot write item event %zu", index);
            goto cleanup;
        }
    }
    for (index = 0U; index < items->item_count; ++index) {
        const HWAItem *item = (const HWAItem *)sorted_items[index];
        if (item->id != (uint64_t)index + 1U ||
            (item->parent_valid &&
             (item->parent_id == 0U ||
              item->parent_id > (uint64_t)items->item_count)) ||
            hwa_item_write_item(stream, item, items) != 0) {
            hwa_set_error(error, error_size,
                          "cannot write item %zu", index);
            goto cleanup;
        }
    }
    for (index = 0U; index < items->member_count; ++index) {
        const HWAItemMember *member =
            (const HWAItemMember *)members[index];
        if (member->item_id == 0U ||
            member->item_id > (uint64_t)items->item_count ||
            member->event_id == 0U ||
            member->event_id > (uint64_t)items->event_count ||
            hwa_item_write_member(stream, member) != 0) {
            hwa_set_error(error, error_size,
                          "cannot write item member %zu", index);
            goto cleanup;
        }
    }
    for (index = 0U; index < items->warning_count; ++index) {
        const HWAItemWarning *warning =
            (const HWAItemWarning *)warnings[index];
        if (warning->id != (uint64_t)index + 1U ||
            (warning->item_id_valid &&
             (warning->item_id == 0U ||
              warning->item_id > (uint64_t)items->item_count)) ||
            (warning->event_id_valid &&
             (warning->event_id == 0U ||
              warning->event_id > (uint64_t)items->event_count)) ||
            hwa_item_write_warning(stream, warning) != 0) {
            hwa_set_error(error, error_size,
                          "cannot write item warning %zu", index);
            goto cleanup;
        }
    }
    result = 0;
    goto cleanup;

overflow:
    hwa_set_error(error, error_size,
                  "canonical item pointer storage overflows");

cleanup:
    free(warnings);
    free(members);
    free(sorted_items);
    free(events);
    return result;
}

static const char *const hwa_item_meta_keys[] = {
    "tool_version",
    "analysis_method_version",
    "alignment_method_version",
    "segmentation_method_version",
    "build_compiler_family",
    "build_compiler_version",
    "build_c_standard",
    "build_target_os",
    "build_pointer_bits",
    "build_endianness",
    "build_mode",
    "sample_rate_hz",
    "audio_frames",
    "audio_duration_seconds",
    "source_score_duration_seconds",
    "alignment_confidence",
    "boundary_evaluations",
    "retained_work_bytes",
    "decode_block_frames",
    "max_input_bytes",
    "max_input_frames",
    "max_analysis_work_bytes",
    "max_transforms",
    "max_track_points",
    "boundary_search_seconds",
    "tail_limit_seconds",
    "min_phase_seconds",
    "min_body_seconds",
    "item_confidence_threshold",
    "max_segmentation_work_bytes",
    "max_boundary_evaluations",
    "max_events",
    "max_items",
    "max_item_members",
    "max_label_rows",
    "max_manual_items",
    "event_count",
    "item_count",
    "member_count",
    "warning_count",
    "locked_item_count",
    "excluded_item_count",
    "low_confidence_item_count"
};

static const char *const hwa_item_meta_units[] = {
    "", "", "", "", "", "", "", "", "bits", "", "", "Hz", "frames",
    "seconds", "seconds", "ratio", "evaluations", "bytes", "frames", "bytes",
    "frames", "bytes", "transforms", "points", "seconds", "seconds",
    "seconds", "seconds", "ratio", "bytes", "evaluations", "events",
    "items", "members", "rows", "items", "events", "items", "members",
    "warnings", "items", "items", "items"
};

#define HWA_ITEM_META_COUNT +    (sizeof(hwa_item_meta_keys) / sizeof(hwa_item_meta_keys[0]))

_Static_assert(HWA_ITEM_META_COUNT ==
                   sizeof(hwa_item_meta_units) /
                       sizeof(hwa_item_meta_units[0]),
               "item META key and unit tables must match");

typedef enum HWAItemFileSection {
    HWA_ITEM_SECTION_HEADER = 0,
    HWA_ITEM_SECTION_META = 1,
    HWA_ITEM_SECTION_INPUT = 2,
    HWA_ITEM_SECTION_SOURCE = 3,
    HWA_ITEM_SECTION_EVENT = 4,
    HWA_ITEM_SECTION_ITEM = 5,
    HWA_ITEM_SECTION_MEMBER = 6,
    HWA_ITEM_SECTION_WARNING = 7
} HWAItemFileSection;

typedef struct HWAItemReadState {
    HWAItemFileLimits limits;
    HWAItemEditSet result;
    uint64_t work_bytes;
    uint64_t audio_frames;
    uint32_t sample_rate_hz;
    char **item_keys;
    size_t edit_capacity;
    size_t item_key_capacity;
    size_t item_key_count;
    size_t meta_index;
    size_t input_count;
    size_t source_count;
    size_t event_count;
    size_t item_count;
    size_t member_count;
    size_t warning_count;
    size_t locked_count;
    size_t excluded_count;
    size_t low_confidence_count;
    size_t expected_event_count;
    size_t expected_item_count;
    size_t expected_member_count;
    size_t expected_warning_count;
    size_t expected_locked_count;
    size_t expected_excluded_count;
    size_t expected_low_confidence_count;
    size_t row_count;
    uint64_t item_key_work_bytes;
    HWAItemFileSection section;
    int saw_labels_input;
    int saw_amendment_input;
    int retain_edits;
    int retain_item_keys;
} HWAItemReadState;

typedef int (*HWAItemRowFunction)(char **fields,
                                  size_t field_count,
                                  size_t row,
                                  void *user,
                                  char *error,
                                  size_t error_size);

static int hwa_item_parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;
    if (text[0] == '\0' || text[0] == '-' ||
        isspace((unsigned char)text[0])) {
        return -1;
    }
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0') return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static int hwa_item_parse_size(const char *text, size_t *value)
{
    uint64_t parsed;
    if (hwa_item_parse_u64(text, &parsed) != 0 ||
        parsed > (uint64_t)SIZE_MAX) {
        return -1;
    }
    *value = (size_t)parsed;
    return 0;
}

static int hwa_item_parse_u32(const char *text, uint32_t *value)
{
    uint64_t parsed;
    if (hwa_item_parse_u64(text, &parsed) != 0 || parsed > UINT32_MAX) {
        return -1;
    }
    *value = (uint32_t)parsed;
    return 0;
}

static int hwa_item_parse_double(const char *text,
                                 int blank,
                                 double *value)
{
    char *end = NULL;
    double parsed;
    if (text[0] == '\0') return blank ? 1 : -1;
    if (isspace((unsigned char)text[0])) return -1;
    errno = 0;
    parsed = strtod(text, &end);
    if (errno == ERANGE || end == text || *end != '\0' ||
        !isfinite(parsed)) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static int hwa_item_parse_bit(const char *text, int *value)
{
    if (strcmp(text, "0") == 0) {
        *value = 0;
        return 0;
    }
    if (strcmp(text, "1") == 0) {
        *value = 1;
        return 0;
    }
    return -1;
}

static int hwa_item_parse_optional_u64(const char *text,
                                       uint64_t *value,
                                       int *valid)
{
    if (text[0] == '\0') {
        *valid = 0;
        *value = 0U;
        return 0;
    }
    *valid = 1;
    return hwa_item_parse_u64(text, value);
}

static int hwa_item_hex_path_valid(const char *text)
{
    size_t length = strlen(text);
    size_t index;
    if (length == 0U || (length & 1U) != 0U) return 0;
    for (index = 0U; index < length; index += 2U) {
        char high = text[index];
        char low = text[index + 1U];
        unsigned high_value;
        unsigned low_value;
        if (!((high >= '0' && high <= '9') ||
              (high >= 'a' && high <= 'f')) ||
            !((low >= '0' && low <= '9') ||
              (low >= 'a' && low <= 'f'))) {
            return 0;
        }
        high_value = high <= '9' ? (unsigned)(high - '0') :
                                   (unsigned)(high - 'a') + 10U;
        low_value = low <= '9' ? (unsigned)(low - '0') :
                                 (unsigned)(low - 'a') + 10U;
        if (high_value * 16U + low_value == 0U) return 0;
    }
    return 1;
}

static int hwa_item_read_regular(const char *path,
                                 uint64_t max_bytes,
                                 unsigned char **data,
                                 size_t *size,
                                 char *error,
                                 size_t error_size)
{
    uint64_t source_size;
    FILE *stream;
    unsigned char *buffer;
#if defined(_WIN32)
    struct _stat64 before;
    struct _stat64 opened;
#else
    struct stat before;
    struct stat opened;
#endif

    *data = NULL;
    *size = 0U;
    if (path == NULL || strcmp(path, "-") == 0 || max_bytes == 0U) {
        hwa_set_error(error, error_size,
                      "item amendment must be a named regular file");
        return -1;
    }
#if defined(_WIN32)
    if (_stat64(path, &before) != 0 ||
        (before.st_mode & _S_IFMT) != _S_IFREG || before.st_size < 0) {
#else
    if (stat(path, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size < 0) {
#endif
        hwa_set_error(error, error_size,
                      "cannot inspect item amendment '%s'", path);
        return -1;
    }
    source_size = (uint64_t)before.st_size;
    if (source_size > max_bytes ||
        source_size > (uint64_t)(SIZE_MAX - 1U)) {
        hwa_set_error(error, error_size,
                      "item amendment exceeds the current byte limit");
        return -1;
    }
    stream = fopen(path, "rb");
    if (stream == NULL) {
        hwa_set_error(error, error_size,
                      "cannot open item amendment '%s': %s",
                      path, strerror(errno));
        return -1;
    }
#if defined(_WIN32)
    if (_fstat64(_fileno(stream), &opened) != 0 ||
        opened.st_dev != before.st_dev || opened.st_ino != before.st_ino ||
        opened.st_size != before.st_size) {
#else
    if (fstat(fileno(stream), &opened) != 0 ||
        opened.st_dev != before.st_dev || opened.st_ino != before.st_ino ||
        opened.st_size != before.st_size) {
#endif
        hwa_set_error(error, error_size,
                      "item amendment changed before it was opened");
        (void)fclose(stream);
        return -1;
    }
    buffer = (unsigned char *)malloc((size_t)source_size + 1U);
    if (buffer == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for item amendment");
        (void)fclose(stream);
        return -1;
    }
    {
        int read_failed = source_size != 0U &&
                          fread(buffer, 1U, (size_t)source_size, stream) !=
                              (size_t)source_size;
        int extra = read_failed ? EOF : fgetc(stream);
        int stream_error = ferror(stream);
        int close_failed = fclose(stream) != 0;
        if (read_failed || extra != EOF || stream_error || close_failed) {
            hwa_set_error(error, error_size,
                          "cannot read item amendment '%s'", path);
            free(buffer);
            return -1;
        }
    }
    buffer[(size_t)source_size] = 0U;
    *data = buffer;
    *size = (size_t)source_size;
    return 0;
}

static int hwa_item_csv_rows(const unsigned char *data,
                             size_t size,
                             HWAItemRowFunction function,
                             void *user,
                             char *error,
                             size_t error_size)
{
    char **fields = NULL;
    char *storage = NULL;
    size_t position = 0U;
    size_t physical_row = 1U;
    int saw_row = 0;

    fields = (char **)malloc(HWA_ITEM_FILE_MAX_FIELDS * sizeof(*fields));
    storage = (char *)malloc(size + 1U);
    if (fields == NULL || storage == NULL) {
        free(storage);
        free(fields);
        hwa_set_error(error, error_size,
                      "out of memory while parsing item amendment");
        return -1;
    }
    while (position < size) {
        size_t field_count = 0U;
        size_t output = 0U;
        size_t logical_row = physical_row;
        int row_done = 0;
        while (!row_done) {
            int quoted = 0;
            int closed_quote = 0;
            if (field_count == HWA_ITEM_FILE_MAX_FIELDS) {
                hwa_set_error(error, error_size,
                              "item row %zu has too many fields", logical_row);
                free(storage);
                free(fields);
                return -1;
            }
            fields[field_count++] = storage + output;
            if (position < size && data[position] == (unsigned char)'"') {
                quoted = 1;
                position++;
            }
            while (position < size) {
                unsigned char byte = data[position];
                if (byte == 0U) {
                    hwa_set_error(error, error_size,
                                  "item row %zu contains a NUL byte",
                                  logical_row);
                    free(storage);
                    free(fields);
                    return -1;
                }
                if (quoted) {
                    if (byte == (unsigned char)'"') {
                        if (position + 1U < size &&
                            data[position + 1U] == (unsigned char)'"') {
                            storage[output++] = '"';
                            position += 2U;
                            continue;
                        }
                        closed_quote = 1;
                        position++;
                        break;
                    }
                    if (byte == (unsigned char)'\r') {
                        if (position + 1U >= size ||
                            data[position + 1U] != (unsigned char)'\n') {
                            hwa_set_error(error, error_size,
                                          "item row %zu has a bare carriage return",
                                          logical_row);
                            free(storage);
                            free(fields);
                            return -1;
                        }
                        storage[output++] = '\r';
                        storage[output++] = '\n';
                        position += 2U;
                        physical_row++;
                        continue;
                    }
                    storage[output++] = (char)byte;
                    if (byte == (unsigned char)'\n') physical_row++;
                    position++;
                    continue;
                }
                if (byte == (unsigned char)'"') {
                    hwa_set_error(error, error_size,
                                  "item row %zu has an unescaped quote",
                                  logical_row);
                    free(storage);
                    free(fields);
                    return -1;
                }
                if (byte == (unsigned char)',' ||
                    byte == (unsigned char)'\r' ||
                    byte == (unsigned char)'\n') {
                    break;
                }
                storage[output++] = (char)byte;
                position++;
            }
            if (quoted && !closed_quote) {
                hwa_set_error(error, error_size,
                              "item row %zu has an unclosed quote",
                              logical_row);
                free(storage);
                free(fields);
                return -1;
            }
            storage[output++] = '\0';
            if (position == size) {
                row_done = 1;
            } else if (data[position] == (unsigned char)',') {
                position++;
            } else if (data[position] == (unsigned char)'\n') {
                position++;
                physical_row++;
                row_done = 1;
            } else if (data[position] == (unsigned char)'\r') {
                if (position + 1U >= size ||
                    data[position + 1U] != (unsigned char)'\n') {
                    hwa_set_error(error, error_size,
                                  "item row %zu has a bare carriage return",
                                  logical_row);
                    free(storage);
                    free(fields);
                    return -1;
                }
                position += 2U;
                physical_row++;
                row_done = 1;
            } else {
                hwa_set_error(error, error_size,
                              "item row %zu has text after a quote",
                              logical_row);
                free(storage);
                free(fields);
                return -1;
            }
        }
        if (field_count == 1U && fields[0][0] == '\0') {
            hwa_set_error(error, error_size,
                          "item amendment has an empty row at %zu",
                          logical_row);
            free(storage);
            free(fields);
            return -1;
        }
        if (function(fields, field_count, logical_row, user,
                     error, error_size) != 0) {
            free(storage);
            free(fields);
            return -1;
        }
        saw_row = 1;
    }
    free(storage);
    free(fields);
    if (!saw_row) {
        hwa_set_error(error, error_size, "item amendment is empty");
        return -1;
    }
    return 0;
}

static int hwa_item_read_charge(HWAItemReadState *state,
                                uint64_t bytes,
                                char *error,
                                size_t error_size)
{
    if (state->work_bytes > state->limits.max_work_bytes ||
        bytes > state->limits.max_work_bytes - state->work_bytes) {
        hwa_set_error(error, error_size,
                      "item amendment exceeds the current work-byte limit");
        return -1;
    }
    state->work_bytes += bytes;
    return 0;
}

static char *hwa_item_read_copy(HWAItemReadState *state,
                                const char *text,
                                int empty_is_null,
                                char *error,
                                size_t error_size)
{
    size_t length;
    char *copy;

    if (empty_is_null && text[0] == '\0') return NULL;
    length = strlen(text);
    if (length == SIZE_MAX ||
        hwa_item_read_charge(state, (uint64_t)length + 1U,
                             error, error_size) != 0) {
        return NULL;
    }
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for item amendment text");
        return NULL;
    }
    memcpy(copy, text, length + 1U);
    return copy;
}

static int hwa_item_key_pointer_compare(const void *left, const void *right)
{
    const char *const *a = (const char *const *)left;
    const char *const *b = (const char *const *)right;
    return strcmp(*a, *b);
}

static void hwa_item_read_free_keys(HWAItemReadState *state)
{
    size_t index;

    for (index = 0U; index < state->item_key_count; ++index) {
        free(state->item_keys[index]);
    }
    free(state->item_keys);
    if (state->item_key_work_bytes <= state->work_bytes) {
        state->work_bytes -= state->item_key_work_bytes;
    }
    state->item_keys = NULL;
    state->item_key_capacity = 0U;
    state->item_key_count = 0U;
    state->item_key_work_bytes = 0U;
}

static int hwa_item_read_add_key(HWAItemReadState *state,
                                 const char *key,
                                 char *error,
                                 size_t error_size)
{
    size_t length = strlen(key);
    char *copy;

    if (state->item_key_count == state->item_key_capacity) {
        size_t next = state->item_key_capacity == 0U
                          ? 64U : state->item_key_capacity * 2U;
        size_t added;
        uint64_t bytes;
        char **grown;

        if (next < state->item_key_capacity || next > state->limits.max_items) {
            next = state->limits.max_items;
        }
        if (next <= state->item_key_capacity ||
            next > SIZE_MAX / sizeof(*state->item_keys)) {
            hwa_set_error(error, error_size,
                          "item key index storage overflows");
            return -1;
        }
        added = next - state->item_key_capacity;
        bytes = (uint64_t)added * (uint64_t)sizeof(*state->item_keys);
        if (hwa_item_read_charge(state, bytes, error, error_size) != 0) {
            return -1;
        }
        grown = (char **)realloc(state->item_keys,
                                 next * sizeof(*state->item_keys));
        if (grown == NULL) {
            state->work_bytes -= bytes;
            hwa_set_error(error, error_size,
                          "out of memory for the item key index");
            return -1;
        }
        state->item_keys = grown;
        state->item_key_capacity = next;
        state->item_key_work_bytes += bytes;
    }
    if (length == SIZE_MAX ||
        hwa_item_read_charge(state, (uint64_t)length + 1U,
                             error, error_size) != 0) {
        return -1;
    }
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        state->work_bytes -= (uint64_t)length + 1U;
        hwa_set_error(error, error_size,
                      "out of memory for an item key");
        return -1;
    }
    memcpy(copy, key, length + 1U);
    state->item_keys[state->item_key_count++] = copy;
    state->item_key_work_bytes += (uint64_t)length + 1U;
    return 0;
}

static int hwa_item_read_meta(char **fields,
                              size_t field_count,
                              size_t row,
                              HWAItemReadState *state,
                              char *error,
                              size_t error_size)
{
    const char *key;
    uint64_t u64;
    double number;

    if (field_count != 4U ||
        state->meta_index >= HWA_ITEM_META_COUNT ||
        strcmp(fields[1], hwa_item_meta_keys[state->meta_index]) != 0 ||
        strcmp(fields[3], hwa_item_meta_units[state->meta_index]) != 0) {
        hwa_set_error(error, error_size,
                      "item row %zu has an unexpected META key", row);
        return -1;
    }
    key = fields[1];
    if (strcmp(key, "tool_version") == 0) {
        if (fields[2][0] == '\0') goto bad;
    } else if (strcmp(key, "analysis_method_version") == 0) {
        if (strcmp(fields[2], HWA_ANALYSIS_METHOD_VERSION) != 0) goto bad;
    } else if (strcmp(key, "alignment_method_version") == 0) {
        if (strcmp(fields[2], HWA_ALIGNMENT_METHOD_VERSION) != 0) goto bad;
    } else if (strcmp(key, "segmentation_method_version") == 0) {
        if (strcmp(fields[2], HWA_SEGMENTATION_METHOD_VERSION) != 0) goto bad;
    } else if (strncmp(key, "build_", 6U) == 0 &&
               strcmp(key, "build_pointer_bits") != 0) {
        if (fields[2][0] == '\0') goto bad;
    } else if (strcmp(key, "sample_rate_hz") == 0) {
        if (hwa_item_parse_u64(fields[2], &u64) != 0 ||
            u64 == 0U || u64 > UINT32_MAX) {
            goto bad;
        }
        state->sample_rate_hz = (uint32_t)u64;
    } else if (strcmp(key, "audio_frames") == 0) {
        if (hwa_item_parse_u64(fields[2], &state->audio_frames) != 0) goto bad;
    } else if (strcmp(key, "event_count") == 0) {
        if (hwa_item_parse_size(fields[2], &state->expected_event_count) != 0) {
            goto bad;
        }
    } else if (strcmp(key, "item_count") == 0) {
        if (hwa_item_parse_size(fields[2], &state->expected_item_count) != 0) {
            goto bad;
        }
    } else if (strcmp(key, "member_count") == 0) {
        if (hwa_item_parse_size(fields[2], &state->expected_member_count) != 0) {
            goto bad;
        }
    } else if (strcmp(key, "warning_count") == 0) {
        if (hwa_item_parse_size(fields[2], &state->expected_warning_count) != 0) {
            goto bad;
        }
    } else if (strcmp(key, "locked_item_count") == 0) {
        if (hwa_item_parse_size(fields[2], &state->expected_locked_count) != 0) {
            goto bad;
        }
    } else if (strcmp(key, "excluded_item_count") == 0) {
        if (hwa_item_parse_size(
                fields[2], &state->expected_excluded_count) != 0) {
            goto bad;
        }
    } else if (strcmp(key, "low_confidence_item_count") == 0) {
        if (hwa_item_parse_size(
                fields[2], &state->expected_low_confidence_count) != 0) {
            goto bad;
        }
    } else if (strcmp(fields[3], "seconds") == 0 ||
               strcmp(fields[3], "ratio") == 0) {
        if (hwa_item_parse_double(fields[2], 0, &number) != 0 ||
            (strcmp(fields[3], "ratio") == 0 &&
             (number < 0.0 || number > 1.0)) ||
            (strcmp(fields[3], "seconds") == 0 && number < 0.0)) {
            goto bad;
        }
    } else if (hwa_item_parse_u64(fields[2], &u64) != 0) {
        goto bad;
    }
    state->meta_index++;
    return 0;

bad:
    hwa_set_error(error, error_size,
                  "item row %zu has an invalid META value", row);
    return -1;
}

static int hwa_item_read_input(char **fields,
                               size_t field_count,
                               size_t row,
                               HWAItemReadState *state,
                               char *error,
                               size_t error_size)
{
    const char *role;
    double duration;
    uint64_t rate;
    uint64_t frames;

    if (field_count != 7U || !hwa_item_hex_path_valid(fields[2]) ||
        !hwa_item_valid_sha256(fields[3])) {
        goto bad;
    }
    role = fields[1];
    if (state->input_count == 0U) {
        if (strcmp(role, "alignment") != 0) goto bad;
        memcpy(state->result.alignment_sha256, fields[3],
               HWA_SHA256_HEX_SIZE);
    } else if (state->input_count == 1U) {
        if (strcmp(role, "audio") != 0) goto bad;
        memcpy(state->result.audio_sha256, fields[3],
               HWA_SHA256_HEX_SIZE);
    } else if (!state->saw_labels_input &&
               !state->saw_amendment_input &&
               strcmp(role, "labels") == 0) {
        memcpy(state->result.labels_sha256, fields[3],
               HWA_SHA256_HEX_SIZE);
        state->saw_labels_input = 1;
    } else if (!state->saw_amendment_input &&
               strcmp(role, "amendment") == 0) {
        /* The prior amendment is provenance, not an input identity. */
        state->saw_amendment_input = 1;
    } else {
        goto bad;
    }
    if (strcmp(role, "audio") == 0) {
        if (hwa_item_parse_double(fields[4], 0, &duration) != 0 ||
            hwa_item_parse_u64(fields[5], &rate) != 0 ||
            hwa_item_parse_u64(fields[6], &frames) != 0 ||
            duration < 0.0 || rate != state->sample_rate_hz ||
            frames != state->audio_frames) {
            goto bad;
        }
    } else if (fields[4][0] != '\0' || fields[5][0] != '\0' ||
               fields[6][0] != '\0') {
        goto bad;
    }
    state->input_count++;
    return 0;

bad:
    hwa_set_error(error, error_size,
                  "item row %zu has an invalid INPUT record", row);
    return -1;
}

static int hwa_item_read_source(char **fields,
                                size_t field_count,
                                size_t row,
                                HWAItemReadState *state,
                                char *error,
                                size_t error_size)
{
    double duration;
    if (field_count != 5U || state->source_count != 0U ||
        strcmp(fields[1], "score") != 0 ||
        !hwa_item_hex_path_valid(fields[2]) ||
        !hwa_item_valid_sha256(fields[3]) ||
        hwa_item_parse_double(fields[4], 0, &duration) != 0 ||
        duration < 0.0) {
        hwa_set_error(error, error_size,
                      "item row %zu has an invalid SOURCE record", row);
        return -1;
    }
    state->source_count++;
    return 0;
}

static int hwa_item_status_value(const char *text)
{
    return strcmp(text, "matched") == 0 ||
           strcmp(text, "low-confidence") == 0 ||
           strcmp(text, "skipped") == 0 ||
           strcmp(text, "repeated") == 0 ||
           strcmp(text, "rest") == 0 ||
           strcmp(text, "ornament") == 0 ||
           strcmp(text, "cadenza") == 0;
}

static int hwa_item_read_event(char **fields,
                               size_t field_count,
                               size_t row,
                               HWAItemReadState *state,
                               char *error,
                               size_t error_size)
{
    uint64_t id;
    uint64_t start_sample;
    uint64_t end_sample;
    uint32_t flags;
    uint32_t overrides;
    double score_start;
    double score_end;
    double score_time_start;
    double score_time_end;
    double audio_start;
    double audio_end;
    double confidence;
    double tempo = 0.0;
    int tempo_result;
    double expected_start;
    double expected_end;

    tempo_result = field_count == HWA_ITEM_FILE_MAX_FIELDS
                       ? hwa_item_parse_double(fields[22], 1, &tempo)
                       : -1;
    if (field_count != HWA_ITEM_FILE_MAX_FIELDS ||
        state->event_count == state->limits.max_events ||
        hwa_item_parse_u64(fields[1], &id) != 0 ||
        id != (uint64_t)state->event_count + 1U ||
        fields[2][0] == '\0' || fields[3][0] == '\0' ||
        hwa_item_parse_double(fields[4], 0, &score_start) != 0 ||
        hwa_item_parse_double(fields[5], 0, &score_end) != 0 ||
        hwa_item_parse_double(fields[6], 0, &score_time_start) != 0 ||
        hwa_item_parse_double(fields[7], 0, &score_time_end) != 0 ||
        hwa_item_parse_u64(fields[8], &start_sample) != 0 ||
        hwa_item_parse_u64(fields[9], &end_sample) != 0 ||
        hwa_item_parse_double(fields[10], 0, &audio_start) != 0 ||
        hwa_item_parse_double(fields[11], 0, &audio_end) != 0 ||
        hwa_item_parse_double(fields[12], 0, &confidence) != 0 ||
        !hwa_item_status_value(fields[13]) ||
        hwa_item_parse_u32(fields[14], &flags) != 0 ||
        tempo_result < 0 ||
        hwa_item_parse_u32(fields[34], &overrides) != 0 ||
        score_start < 0.0 || score_end < score_start ||
        score_time_start < 0.0 || score_time_end < score_time_start ||
        start_sample > end_sample || end_sample > state->audio_frames ||
        confidence < 0.0 || confidence > 1.0 ||
        (tempo_result == 0 && tempo <= 0.0)) {
        goto bad;
    }
    expected_start = (double)start_sample / (double)state->sample_rate_hz;
    expected_end = (double)end_sample / (double)state->sample_rate_hz;
    if (fabs(audio_start - expected_start) > 1e-12 ||
        fabs(audio_end - expected_end) > 1e-12) {
        goto bad;
    }
    (void)flags;
    (void)overrides;
    state->event_count++;
    return 0;

bad:
    hwa_set_error(error, error_size,
                  "item row %zu has an invalid EVENT record", row);
    return -1;
}

static int hwa_item_kind_value(const char *text)
{
    return strcmp(text, "note") == 0 ||
           strcmp(text, "attack") == 0 ||
           strcmp(text, "body") == 0 ||
           strcmp(text, "release") == 0 ||
           strcmp(text, "residual-tail") == 0 ||
           strcmp(text, "rest") == 0 ||
           strcmp(text, "transition") == 0 ||
           strcmp(text, "gesture") == 0 ||
           strcmp(text, "multi-note") == 0;
}

static int hwa_item_read_reserve_edit(HWAItemReadState *state,
                                      char *error,
                                      size_t error_size)
{
    size_t next;
    size_t added;
    HWAItemEdit *grown;
    if (state->result.edit_count == state->limits.max_manual_items) {
        hwa_set_error(error, error_size,
                      "item amendment exceeds the current item limit");
        return -1;
    }
    if (state->result.edit_count < state->edit_capacity) return 0;
    next = state->edit_capacity == 0U ? 16U : state->edit_capacity * 2U;
    if (next < state->edit_capacity ||
        next > state->limits.max_manual_items) {
        next = state->limits.max_manual_items;
    }
    if (next <= state->edit_capacity ||
        next > SIZE_MAX / sizeof(*state->result.edits)) {
        hwa_set_error(error, error_size, "item edit storage overflows");
        return -1;
    }
    added = next - state->edit_capacity;
    if ((uint64_t)added >
            UINT64_MAX / (uint64_t)sizeof(*state->result.edits) ||
        hwa_item_read_charge(
            state,
            (uint64_t)added * (uint64_t)sizeof(*state->result.edits),
            error, error_size) != 0) {
        return -1;
    }
    grown = (HWAItemEdit *)realloc(
        state->result.edits, next * sizeof(*state->result.edits));
    if (grown == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for item edits");
        return -1;
    }
    memset(grown + state->edit_capacity, 0, added * sizeof(*grown));
    state->result.edits = grown;
    state->edit_capacity = next;
    return 0;
}

static int hwa_item_read_item(char **fields,
                              size_t field_count,
                              size_t row,
                              HWAItemReadState *state,
                              char *error,
                              size_t error_size)
{
    HWAItemEdit *edit = NULL;
    uint64_t id;
    uint64_t parent;
    uint64_t start_sample;
    uint64_t end_sample;
    int parent_valid;
    double start_seconds;
    double end_seconds;
    double score_start;
    double score_end;
    double confidence;
    uint32_t evidence;
    uint32_t quality;
    int locked;
    int excluded;
    int manual_origin;
    int retain_edit;
    double expected_start;
    double expected_end;

    if (field_count != 19U ||
        state->item_count == state->limits.max_items ||
        hwa_item_parse_u64(fields[1], &id) != 0 ||
        id != (uint64_t)state->item_count + 1U ||
        fields[2][0] == '\0' || !hwa_item_kind_value(fields[3]) ||
        fields[4][0] == '\0' ||
        hwa_item_parse_optional_u64(fields[5], &parent,
                                    &parent_valid) != 0 ||
        hwa_item_parse_u64(fields[6], &start_sample) != 0) {
        goto bad;
    }
    if (hwa_item_parse_u64(fields[7], &end_sample) != 0 ||
        hwa_item_parse_double(fields[8], 0, &start_seconds) != 0 ||
        hwa_item_parse_double(fields[9], 0, &end_seconds) != 0 ||
        hwa_item_parse_double(fields[10], 0, &score_start) != 0 ||
        hwa_item_parse_double(fields[11], 0, &score_end) != 0 ||
        hwa_item_parse_double(fields[12], 0, &confidence) != 0 ||
        hwa_item_parse_u32(fields[13], &evidence) != 0 ||
        hwa_item_parse_u32(fields[14], &quality) != 0 ||
        !(strcmp(fields[15], "auto") == 0 ||
          strcmp(fields[15], "manual") == 0) ||
        hwa_item_parse_bit(fields[16], &locked) != 0 ||
        hwa_item_parse_bit(fields[17], &excluded) != 0 ||
        start_sample > end_sample ||
        end_sample > state->audio_frames ||
        score_start < 0.0 || score_end < score_start ||
        confidence < 0.0 || confidence > 1.0 ||
        (excluded && fields[18][0] == '\0') ||
        (!excluded && fields[18][0] != '\0') ||
        (parent_valid &&
         (parent == 0U ||
          parent > (uint64_t)state->expected_item_count))) {
        goto bad;
    }
    expected_start = (double)start_sample /
                     (double)state->sample_rate_hz;
    expected_end = (double)end_sample /
                   (double)state->sample_rate_hz;
    if (!locked &&
        (fabs(start_seconds - expected_start) > 1e-12 ||
         fabs(end_seconds - expected_end) > 1e-12)) {
        goto bad;
    }
    manual_origin = strcmp(fields[15], "manual") == 0;
    if (state->retain_item_keys &&
        hwa_item_read_add_key(state, fields[2],
                              error, error_size) != 0) {
        goto bad;
    }
    retain_edit = state->retain_edits &&
                  (manual_origin || locked || excluded);
    if (retain_edit &&
        hwa_item_read_reserve_edit(state, error, error_size) != 0) {
        goto bad;
    }
    if (retain_edit) {
        edit = &state->result.edits[state->result.edit_count];
        edit->start_sample = locked ? start_sample : 0U;
        edit->end_sample = locked ? end_sample : 0U;
        edit->key = hwa_item_read_copy(state, fields[2], 0,
                                       error, error_size);
        if (edit->key == NULL) goto bad;
        edit->locked = locked;
        edit->exclusion_set = 1;
        edit->excluded = excluded;
        edit->exclusion_reason = hwa_item_read_copy(
            state, fields[18], 1, error, error_size);
        if (excluded && edit->exclusion_reason == NULL) {
            free((char *)edit->key);
            memset(edit, 0, sizeof(*edit));
            goto bad;
        }
        state->result.edit_count++;
    }
    if (locked) state->locked_count++;
    if (excluded) state->excluded_count++;
    if ((quality & HWA_ITEM_QUALITY_LOW_CONFIDENCE) != 0U) {
        state->low_confidence_count++;
    }
    (void)evidence;
    state->item_count++;
    return 0;

bad:
    hwa_set_error(error, error_size,
                  "item row %zu has an invalid ITEM record", row);
    return -1;
}

static int hwa_item_member_role_value(const char *text)
{
    return strcmp(text, "source") == 0 || strcmp(text, "from") == 0 ||
           strcmp(text, "to") == 0 || strcmp(text, "active") == 0;
}

static int hwa_item_read_member(char **fields,
                                size_t field_count,
                                size_t row,
                                HWAItemReadState *state,
                                char *error,
                                size_t error_size)
{
    uint64_t item_id;
    uint64_t event_id;
    uint32_t order;
    if (field_count != 5U ||
        state->member_count == state->limits.max_members ||
        hwa_item_parse_u64(fields[1], &item_id) != 0 ||
        hwa_item_parse_u64(fields[2], &event_id) != 0 ||
        hwa_item_parse_u32(fields[3], &order) != 0 ||
        !hwa_item_member_role_value(fields[4]) ||
        item_id == 0U || item_id > (uint64_t)state->item_count ||
        event_id == 0U || event_id > (uint64_t)state->event_count) {
        hwa_set_error(error, error_size,
                      "item row %zu has an invalid MEMBER record", row);
        return -1;
    }
    (void)order;
    state->member_count++;
    return 0;
}

static int hwa_item_read_warning(char **fields,
                                 size_t field_count,
                                 size_t row,
                                 HWAItemReadState *state,
                                 char *error,
                                 size_t error_size)
{
    uint64_t id;
    uint64_t item_id;
    uint64_t event_id;
    int item_valid;
    int event_valid;
    if (field_count != 6U ||
        state->warning_count == state->limits.max_warnings ||
        hwa_item_parse_u64(fields[1], &id) != 0 ||
        id != (uint64_t)state->warning_count + 1U ||
        fields[2][0] == '\0' ||
        hwa_item_parse_optional_u64(fields[4], &item_id, &item_valid) != 0 ||
        hwa_item_parse_optional_u64(fields[5], &event_id, &event_valid) != 0 ||
        (item_valid &&
         (item_id == 0U || item_id > (uint64_t)state->item_count)) ||
        (event_valid &&
         (event_id == 0U || event_id > (uint64_t)state->event_count))) {
        hwa_set_error(error, error_size,
                      "item row %zu has an invalid WARNING record", row);
        return -1;
    }
    state->warning_count++;
    return 0;
}

static int hwa_item_read_row(char **fields,
                             size_t field_count,
                             size_t row,
                             void *user,
                             char *error,
                             size_t error_size)
{
    HWAItemReadState *state = (HWAItemReadState *)user;
    HWAItemFileSection section;
    size_t index;

    if (field_count > state->limits.max_fields_per_row) {
        hwa_set_error(error, error_size,
                      "item row %zu exceeds the current field limit", row);
        return -1;
    }
    for (index = 0U; index < field_count; ++index) {
        if (strlen(fields[index]) > state->limits.max_field_bytes) {
            hwa_set_error(error, error_size,
                          "item row %zu exceeds the current field-byte limit",
                          row);
            return -1;
        }
    }
    if (state->row_count++ == 0U) {
        if (field_count != 2U || strcmp(fields[0], "HWA_ITEMS") != 0 ||
            strcmp(fields[1], "1") != 0) {
            hwa_set_error(error, error_size,
                          "item amendment has an unsupported header");
            return -1;
        }
        state->section = HWA_ITEM_SECTION_META;
        return 0;
    }
    if (strcmp(fields[0], "META") == 0) {
        section = HWA_ITEM_SECTION_META;
    } else if (strcmp(fields[0], "INPUT") == 0) {
        section = HWA_ITEM_SECTION_INPUT;
    } else if (strcmp(fields[0], "SOURCE") == 0) {
        section = HWA_ITEM_SECTION_SOURCE;
    } else if (strcmp(fields[0], "EVENT") == 0) {
        section = HWA_ITEM_SECTION_EVENT;
    } else if (strcmp(fields[0], "ITEM") == 0) {
        section = HWA_ITEM_SECTION_ITEM;
    } else if (strcmp(fields[0], "MEMBER") == 0) {
        section = HWA_ITEM_SECTION_MEMBER;
    } else if (strcmp(fields[0], "WARNING") == 0) {
        section = HWA_ITEM_SECTION_WARNING;
    } else {
        hwa_set_error(error, error_size,
                      "item row %zu has an unknown record type", row);
        return -1;
    }
    if (section < state->section) {
        hwa_set_error(error, error_size,
                      "item row %zu is outside canonical record order", row);
        return -1;
    }
    if (section > HWA_ITEM_SECTION_META &&
        state->meta_index != HWA_ITEM_META_COUNT) {
        hwa_set_error(error, error_size,
                      "item row %zu starts before META is complete", row);
        return -1;
    }
    if (section > HWA_ITEM_SECTION_INPUT && state->input_count < 2U) {
        hwa_set_error(error, error_size,
                      "item row %zu starts before INPUT is complete", row);
        return -1;
    }
    if (section > HWA_ITEM_SECTION_SOURCE && state->source_count != 1U) {
        hwa_set_error(error, error_size,
                      "item row %zu starts before SOURCE is complete", row);
        return -1;
    }
    state->section = section;
    switch (section) {
    case HWA_ITEM_SECTION_META:
        return hwa_item_read_meta(fields, field_count, row, state,
                                  error, error_size);
    case HWA_ITEM_SECTION_INPUT:
        return hwa_item_read_input(fields, field_count, row, state,
                                   error, error_size);
    case HWA_ITEM_SECTION_SOURCE:
        return hwa_item_read_source(fields, field_count, row, state,
                                    error, error_size);
    case HWA_ITEM_SECTION_EVENT:
        return hwa_item_read_event(fields, field_count, row, state,
                                   error, error_size);
    case HWA_ITEM_SECTION_ITEM:
        return hwa_item_read_item(fields, field_count, row, state,
                                  error, error_size);
    case HWA_ITEM_SECTION_MEMBER:
        return hwa_item_read_member(fields, field_count, row, state,
                                    error, error_size);
    case HWA_ITEM_SECTION_WARNING:
        return hwa_item_read_warning(fields, field_count, row, state,
                                     error, error_size);
    default:
        hwa_set_error(error, error_size,
                      "item row %zu has an invalid record", row);
        return -1;
    }
}

static int hwa_item_edit_pointer_compare(const void *left, const void *right)
{
    const HWAItemEdit *a = (const HWAItemEdit *)left;
    const HWAItemEdit *b = (const HWAItemEdit *)right;
    return strcmp(a->key, b->key);
}

void hwa_item_file_limits_default(HWAItemFileLimits *limits)
{
    if (limits == NULL) return;
    limits->max_bytes = UINT64_C(67108864);
    limits->max_work_bytes = UINT64_C(268435456);
    limits->max_fields_per_row = HWA_ITEM_FILE_MAX_FIELDS;
    limits->max_field_bytes = 65536U;
    limits->max_events = 200000U;
    limits->max_items = 1000000U;
    limits->max_manual_items = 100000U;
    limits->max_members = 2000000U;
    limits->max_warnings = 200000U;
}

int hwa_item_file_read_edits(const char *path,
                             const HWAItemFileLimits *limits,
                             HWAItemEditSet *edits,
                             char *error,
                             size_t error_size)
{
    HWAItemFileLimits copied;
    HWAItemReadState state;
    unsigned char *data = NULL;
    size_t size = 0U;
    uint64_t parser_bytes;
    uint64_t file_limit;
    unsigned char digest[32];
    HWASha256 sha;
    char after[HWA_SHA256_HEX_SIZE];
    size_t index;

    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (limits == NULL || edits == NULL) {
        hwa_set_error(error, error_size,
                      "invalid item-amendment reader arguments");
        return -1;
    }
    copied = *limits;
    memset(edits, 0, sizeof(*edits));
    if (copied.max_bytes == 0U || copied.max_work_bytes == 0U ||
        copied.max_fields_per_row == 0U || copied.max_field_bytes == 0U ||
        copied.max_events == 0U || copied.max_items == 0U ||
        copied.max_manual_items == 0U ||
        copied.max_members == 0U || copied.max_warnings == 0U) {
        hwa_set_error(error, error_size,
                      "item-amendment reader limits must be nonzero");
        return -1;
    }
    file_limit = copied.max_bytes;
    if (file_limit > copied.max_work_bytes / 2U) {
        file_limit = copied.max_work_bytes / 2U;
    }
    if (file_limit == 0U ||
        hwa_item_read_regular(path, file_limit, &data, &size,
                              error, error_size) != 0) {
        return -1;
    }
    if ((uint64_t)size + 1U > UINT64_MAX / 2U ||
        (uint64_t)HWA_ITEM_FILE_MAX_FIELDS >
            UINT64_MAX / (uint64_t)sizeof(char *)) {
        hwa_set_error(error, error_size,
                      "item amendment parser storage overflows");
        free(data);
        return -1;
    }
    parser_bytes = ((uint64_t)size + 1U) * 2U +
                   (uint64_t)HWA_ITEM_FILE_MAX_FIELDS *
                       (uint64_t)sizeof(char *);
    if (parser_bytes > copied.max_work_bytes) {
        hwa_set_error(error, error_size,
                      "item amendment parser exceeds the work-byte limit");
        free(data);
        return -1;
    }
    memset(&state, 0, sizeof(state));
    state.limits = copied;
    state.work_bytes = parser_bytes;
    state.retain_edits = 1;
    state.retain_item_keys = 1;
    hwa_sha256_init(&sha);
    hwa_sha256_update(&sha, data, size);
    hwa_sha256_final(&sha, digest);
    hwa_sha256_hex(digest, state.result.sha256);
    if (hwa_item_csv_rows(data, size, hwa_item_read_row, &state,
                          error, error_size) != 0 ||
        state.row_count == 0U ||
        state.meta_index != HWA_ITEM_META_COUNT ||
        state.input_count < 2U || state.source_count != 1U ||
        state.event_count != state.expected_event_count ||
        state.item_count != state.expected_item_count ||
        state.member_count != state.expected_member_count ||
        state.warning_count != state.expected_warning_count ||
        state.low_confidence_count !=
            state.expected_low_confidence_count) {
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "item amendment counts do not match META");
        }
        free(data);
        hwa_item_read_free_keys(&state);
        hwa_item_edit_set_free(&state.result);
        return -1;
    }
    free(data);
    if (state.item_key_count > 1U) {
        qsort(state.item_keys, state.item_key_count,
              sizeof(*state.item_keys), hwa_item_key_pointer_compare);
        for (index = 1U; index < state.item_key_count; ++index) {
            if (strcmp(state.item_keys[index - 1U],
                       state.item_keys[index]) == 0) {
                hwa_set_error(error, error_size,
                              "item amendment repeats key '%s'",
                              state.item_keys[index]);
                hwa_item_read_free_keys(&state);
                hwa_item_edit_set_free(&state.result);
                return -1;
            }
        }
    }
    hwa_item_read_free_keys(&state);
    if (state.result.edit_count > 1U) {
        qsort(state.result.edits, state.result.edit_count,
              sizeof(*state.result.edits), hwa_item_edit_pointer_compare);
        for (index = 1U; index < state.result.edit_count; ++index) {
            if (strcmp(state.result.edits[index - 1U].key,
                       state.result.edits[index].key) == 0) {
                hwa_set_error(error, error_size,
                              "item amendment repeats key '%s'",
                              state.result.edits[index].key);
                hwa_item_edit_set_free(&state.result);
                return -1;
            }
        }
    }
    state.result.path = hwa_item_read_copy(
        &state, path, 0, error, error_size);
    if (state.result.path == NULL ||
        hwa_sha256_file(path, copied.max_bytes, after,
                        error, error_size) != 0 ||
        strcmp(after, state.result.sha256) != 0) {
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "item amendment changed while it was read");
        }
        hwa_item_edit_set_free(&state.result);
        return -1;
    }
    state.result.retained_work_bytes = state.work_bytes - parser_bytes;
    *edits = state.result;
    return 0;
}

int hwa_item_edit_set_matches(
    const HWAItemEditSet *edits,
    const char alignment_sha256[HWA_SHA256_HEX_SIZE],
    const char audio_sha256[HWA_SHA256_HEX_SIZE],
    const char labels_sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    const char *expected_labels =
        labels_sha256 != NULL ? labels_sha256 : "";
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (edits == NULL || alignment_sha256 == NULL ||
        audio_sha256 == NULL ||
        !hwa_item_valid_sha256(alignment_sha256) ||
        !hwa_item_valid_sha256(audio_sha256) ||
        (expected_labels[0] != '\0' &&
         !hwa_item_valid_sha256(expected_labels)) ||
        strcmp(edits->alignment_sha256, alignment_sha256) != 0 ||
        strcmp(edits->audio_sha256, audio_sha256) != 0 ||
        strcmp(edits->labels_sha256, expected_labels) != 0) {
        hwa_set_error(error, error_size,
                      "item amendment input hashes do not match");
        return -1;
    }
    return 0;
}

void hwa_item_edit_set_free(HWAItemEditSet *edits)
{
    size_t index;
    if (edits == NULL) return;
    for (index = 0U; index < edits->edit_count; ++index) {
        free((char *)edits->edits[index].exclusion_reason);
        free((char *)edits->edits[index].key);
    }
    free(edits->edits);
    free(edits->path);
    memset(edits, 0, sizeof(*edits));
}

typedef struct HWAItemFullReadState {
    HWAItemReadState validator;
    HWAItemFileLimits limits;
    HWAItemFileData result;
    uint64_t work_bytes;
    size_t event_capacity;
    size_t item_capacity;
    size_t member_capacity;
    size_t warning_capacity;
    double saved_audio_duration;
    double saved_source_duration;
    size_t saved_locked_count;
    size_t saved_excluded_count;
    size_t saved_low_confidence_count;
    int saved_audio_duration_valid;
    int saved_source_duration_valid;
    int have_prior_member;
    HWAItemMember prior_member;
} HWAItemFullReadState;

static int hwa_item_full_charge(HWAItemFullReadState *state,
                                uint64_t bytes,
                                char *error,
                                size_t error_size)
{
    if (state->work_bytes > state->limits.max_work_bytes ||
        bytes > state->limits.max_work_bytes - state->work_bytes) {
        hwa_set_error(error, error_size,
                      "item file exceeds the current work-byte limit");
        return -1;
    }
    state->work_bytes += bytes;
    return 0;
}

static int hwa_item_full_reserve(HWAItemFullReadState *state,
                                 void **array,
                                 size_t *capacity,
                                 size_t count,
                                 size_t maximum,
                                 size_t item_size,
                                 const char *name,
                                 char *error,
                                 size_t error_size)
{
    size_t next;
    size_t added;
    uint64_t bytes;
    void *grown;

    if (count >= maximum) {
        hwa_set_error(error, error_size,
                      "item file exceeds the current %s limit", name);
        return -1;
    }
    if (count < *capacity) return 0;
    next = *capacity == 0U ? 16U : *capacity * 2U;
    if (next < *capacity || next > maximum) next = maximum;
    if (next <= *capacity || next > SIZE_MAX / item_size) {
        hwa_set_error(error, error_size, "item %s storage overflows", name);
        return -1;
    }
    added = next - *capacity;
    if ((uint64_t)added > UINT64_MAX / (uint64_t)item_size) {
        hwa_set_error(error, error_size, "item %s storage overflows", name);
        return -1;
    }
    bytes = (uint64_t)added * (uint64_t)item_size;
    if (hwa_item_full_charge(state, bytes, error, error_size) != 0) {
        return -1;
    }
    grown = realloc(*array, next * item_size);
    if (grown == NULL) {
        state->work_bytes -= bytes;
        hwa_set_error(error, error_size,
                      "out of memory for item %s", name);
        return -1;
    }
    memset((unsigned char *)grown + *capacity * item_size, 0,
           added * item_size);
    *array = grown;
    *capacity = next;
    return 0;
}

static char *hwa_item_full_copy(HWAItemFullReadState *state,
                                const char *text,
                                int empty_is_null,
                                char *error,
                                size_t error_size)
{
    size_t length;
    char *copy;

    if (text == NULL || (empty_is_null && text[0] == '\0')) return NULL;
    length = strlen(text);
    if (length == SIZE_MAX ||
        hwa_item_full_charge(state, (uint64_t)length + 1U,
                             error, error_size) != 0) {
        return NULL;
    }
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        state->work_bytes -= (uint64_t)length + 1U;
        hwa_set_error(error, error_size, "out of memory for item text");
        return NULL;
    }
    memcpy(copy, text, length + 1U);
    return copy;
}

static char *hwa_item_full_decode_path(HWAItemFullReadState *state,
                                       const char *hex,
                                       char *error,
                                       size_t error_size)
{
    static const char digits[] = "0123456789abcdef";
    size_t hex_length = strlen(hex);
    size_t length = hex_length / 2U;
    size_t index;
    char *path;

    if (hex_length == 0U || (hex_length & 1U) != 0U ||
        hwa_item_full_charge(state, (uint64_t)length + 1U,
                             error, error_size) != 0) {
        if (hex_length == 0U) {
            hwa_set_error(error, error_size, "item input path is empty");
        }
        return NULL;
    }
    path = (char *)malloc(length + 1U);
    if (path == NULL) {
        state->work_bytes -= (uint64_t)length + 1U;
        hwa_set_error(error, error_size,
                      "out of memory for item input path");
        return NULL;
    }
    for (index = 0U; index < length; ++index) {
        const char *high = strchr(digits, hex[index * 2U]);
        const char *low = strchr(digits, hex[index * 2U + 1U]);
        unsigned value;

        if (high == NULL || low == NULL) {
            free(path);
            state->work_bytes -= (uint64_t)length + 1U;
            hwa_set_error(error, error_size,
                          "item input path has invalid hex bytes");
            return NULL;
        }
        value = (unsigned)(high - digits) * 16U +
                (unsigned)(low - digits);
        if (value == 0U) {
            free(path);
            state->work_bytes -= (uint64_t)length + 1U;
            hwa_set_error(error, error_size,
                          "item input path contains an embedded NUL byte");
            return NULL;
        }
        path[index] = (char)value;
    }
    path[length] = '\0';
    return path;
}

static HWAAlignmentStatus hwa_item_full_status(const char *text)
{
    if (strcmp(text, "matched") == 0) return HWA_ALIGNMENT_MATCHED;
    if (strcmp(text, "low-confidence") == 0) {
        return HWA_ALIGNMENT_LOW_CONFIDENCE;
    }
    if (strcmp(text, "skipped") == 0) return HWA_ALIGNMENT_SKIPPED;
    if (strcmp(text, "repeated") == 0) return HWA_ALIGNMENT_REPEATED;
    if (strcmp(text, "rest") == 0) return HWA_ALIGNMENT_REST;
    if (strcmp(text, "ornament") == 0) return HWA_ALIGNMENT_ORNAMENT;
    return HWA_ALIGNMENT_CADENZA;
}

static HWAItemKind hwa_item_full_kind(const char *text)
{
    if (strcmp(text, "note") == 0) return HWA_ITEM_NOTE;
    if (strcmp(text, "attack") == 0) return HWA_ITEM_ATTACK;
    if (strcmp(text, "body") == 0) return HWA_ITEM_BODY;
    if (strcmp(text, "release") == 0) return HWA_ITEM_RELEASE;
    if (strcmp(text, "residual-tail") == 0) return HWA_ITEM_RESIDUAL_TAIL;
    if (strcmp(text, "rest") == 0) return HWA_ITEM_REST;
    if (strcmp(text, "transition") == 0) return HWA_ITEM_TRANSITION;
    if (strcmp(text, "gesture") == 0) return HWA_ITEM_GESTURE;
    return HWA_ITEM_MULTI_NOTE;
}

static HWAItemMemberRole hwa_item_full_member_role(const char *text)
{
    if (strcmp(text, "source") == 0) return HWA_ITEM_MEMBER_SOURCE;
    if (strcmp(text, "from") == 0) return HWA_ITEM_MEMBER_FROM;
    if (strcmp(text, "to") == 0) return HWA_ITEM_MEMBER_TO;
    return HWA_ITEM_MEMBER_ACTIVE;
}

static int hwa_item_full_store_meta(char **fields,
                                    HWAItemFullReadState *state,
                                    char *error,
                                    size_t error_size)
{
    const char *key = fields[1];
    const char *text = fields[2];
    HWAItemSet *items = &state->result.items;
    HWASegmentationOptions *option = &items->options;
    uint64_t u64;
    double number;
    size_t size_value;

    if (strcmp(key, "sample_rate_hz") == 0) {
        if (hwa_item_parse_u64(text, &u64) != 0 || u64 > UINT32_MAX) {
            return -1;
        }
        items->audio_format.sample_rate_hz = (uint32_t)u64;
    } else if (strcmp(key, "audio_frames") == 0) {
        if (hwa_item_parse_u64(text, &items->audio_format.frames) != 0) {
            return -1;
        }
    } else if (strcmp(key, "audio_duration_seconds") == 0) {
        if (hwa_item_parse_double(text, 0, &number) != 0) return -1;
        items->audio_format.duration_seconds = number;
        state->saved_audio_duration = number;
        state->saved_audio_duration_valid = 1;
    } else if (strcmp(key, "source_score_duration_seconds") == 0) {
        if (hwa_item_parse_double(text, 0, &number) != 0) return -1;
        items->source_score_duration_seconds = number;
        state->saved_source_duration = number;
        state->saved_source_duration_valid = 1;
    } else if (strcmp(key, "alignment_confidence") == 0) {
        if (hwa_item_parse_double(text, 0,
                                  &items->alignment_confidence) != 0) {
            return -1;
        }
    } else if (strcmp(key, "boundary_evaluations") == 0) {
        if (hwa_item_parse_u64(text, &items->boundary_evaluations) != 0) {
            return -1;
        }
    } else if (strcmp(key, "retained_work_bytes") == 0) {
        if (hwa_item_parse_u64(text, &items->retained_work_bytes) != 0) {
            return -1;
        }
    } else if (strcmp(key, "decode_block_frames") == 0) {
        if (hwa_item_parse_size(text, &option->decode_block_frames) != 0) {
            return -1;
        }
    } else if (strcmp(key, "max_input_bytes") == 0) {
        if (hwa_item_parse_u64(text, &option->max_input_bytes) != 0) return -1;
    } else if (strcmp(key, "max_input_frames") == 0) {
        if (hwa_item_parse_u64(text, &option->max_input_frames) != 0) return -1;
    } else if (strcmp(key, "max_analysis_work_bytes") == 0) {
        if (hwa_item_parse_u64(text,
                               &option->max_analysis_work_bytes) != 0) {
            return -1;
        }
    } else if (strcmp(key, "max_transforms") == 0) {
        if (hwa_item_parse_size(text, &option->max_transforms) != 0) return -1;
    } else if (strcmp(key, "max_track_points") == 0) {
        if (hwa_item_parse_size(text, &option->max_track_points) != 0) return -1;
    } else if (strcmp(key, "max_segmentation_work_bytes") == 0) {
        if (hwa_item_parse_u64(text,
                               &option->max_segmentation_work_bytes) != 0) {
            return -1;
        }
    } else if (strcmp(key, "max_boundary_evaluations") == 0) {
        if (hwa_item_parse_u64(text,
                               &option->max_boundary_evaluations) != 0) {
            return -1;
        }
    } else if (strcmp(key, "max_events") == 0) {
        if (hwa_item_parse_size(text, &option->max_events) != 0) return -1;
    } else if (strcmp(key, "max_items") == 0) {
        if (hwa_item_parse_size(text, &option->max_items) != 0) return -1;
    } else if (strcmp(key, "max_item_members") == 0) {
        if (hwa_item_parse_size(text, &option->max_item_members) != 0) return -1;
    } else if (strcmp(key, "max_label_rows") == 0) {
        if (hwa_item_parse_size(text, &option->max_label_rows) != 0) return -1;
    } else if (strcmp(key, "max_manual_items") == 0) {
        if (hwa_item_parse_size(text, &option->max_manual_items) != 0) return -1;
    } else if (strcmp(key, "locked_item_count") == 0) {
        if (hwa_item_parse_size(text, &state->saved_locked_count) != 0) {
            return -1;
        }
    } else if (strcmp(key, "excluded_item_count") == 0) {
        if (hwa_item_parse_size(text, &state->saved_excluded_count) != 0) {
            return -1;
        }
    } else if (strcmp(key, "low_confidence_item_count") == 0) {
        if (hwa_item_parse_size(text,
                                &state->saved_low_confidence_count) != 0) {
            return -1;
        }
    } else if (strcmp(key, "boundary_search_seconds") == 0 ||
               strcmp(key, "tail_limit_seconds") == 0 ||
               strcmp(key, "min_phase_seconds") == 0 ||
               strcmp(key, "min_body_seconds") == 0 ||
               strcmp(key, "item_confidence_threshold") == 0) {
        double *target = strcmp(key, "boundary_search_seconds") == 0
                             ? &option->boundary_search_seconds
                         : strcmp(key, "tail_limit_seconds") == 0
                             ? &option->tail_limit_seconds
                         : strcmp(key, "min_phase_seconds") == 0
                             ? &option->min_phase_seconds
                         : strcmp(key, "min_body_seconds") == 0
                             ? &option->min_body_seconds
                             : &option->item_confidence_threshold;
        if (hwa_item_parse_double(text, 0, target) != 0) return -1;
    } else if ((strcmp(key, "event_count") == 0 ||
                strcmp(key, "item_count") == 0 ||
                strcmp(key, "member_count") == 0 ||
                strcmp(key, "warning_count") == 0) &&
               hwa_item_parse_size(text, &size_value) != 0) {
        return -1;
    }
    (void)error;
    (void)error_size;
    return 0;
}

static int hwa_item_full_store_input(char **fields,
                                     HWAItemFullReadState *state,
                                     char *error,
                                     size_t error_size)
{
    HWAItemSet *items = &state->result.items;
    char *path = hwa_item_full_decode_path(state, fields[2],
                                           error, error_size);

    if (path == NULL) return -1;
    if (strcmp(fields[1], "alignment") == 0) {
        items->alignment_path = path;
        memcpy(items->alignment_sha256, fields[3], HWA_SHA256_HEX_SIZE);
    } else if (strcmp(fields[1], "audio") == 0) {
        double duration;
        uint64_t rate;
        uint64_t frames;
        if (hwa_item_parse_double(fields[4], 0, &duration) != 0 ||
            hwa_item_parse_u64(fields[5], &rate) != 0 ||
            hwa_item_parse_u64(fields[6], &frames) != 0 ||
            rate != items->audio_format.sample_rate_hz ||
            frames != items->audio_format.frames ||
            fabs(duration - items->audio_format.duration_seconds) > 1e-12) {
            free(path);
            hwa_set_error(error, error_size,
                          "item audio INPUT does not match META");
            return -1;
        }
        items->audio_path = path;
        memcpy(items->audio_sha256, fields[3], HWA_SHA256_HEX_SIZE);
    } else if (strcmp(fields[1], "labels") == 0) {
        items->labels_path = path;
        memcpy(items->labels_sha256, fields[3], HWA_SHA256_HEX_SIZE);
    } else {
        items->amendment_path = path;
        memcpy(items->amendment_sha256, fields[3], HWA_SHA256_HEX_SIZE);
    }
    return 0;
}

static int hwa_item_full_store_source(char **fields,
                                      HWAItemFullReadState *state,
                                      char *error,
                                      size_t error_size)
{
    HWAItemSet *items = &state->result.items;
    double duration;

    items->source_score_path = hwa_item_full_decode_path(
        state, fields[2], error, error_size);
    if (items->source_score_path == NULL ||
        hwa_item_parse_double(fields[4], 0, &duration) != 0 ||
        !state->saved_source_duration_valid ||
        duration != state->saved_source_duration) {
        hwa_set_error(error, error_size,
                      "item score SOURCE does not match META");
        return -1;
    }
    memcpy(items->source_score_sha256, fields[3], HWA_SHA256_HEX_SIZE);
    return 0;
}

static int hwa_item_full_copy_field(HWAItemFullReadState *state,
                                    char **target,
                                    const char *text,
                                    int empty_is_null,
                                    char *error,
                                    size_t error_size)
{
    *target = hwa_item_full_copy(state, text, empty_is_null,
                                 error, error_size);
    return (!empty_is_null || text[0] != '\0') && *target == NULL ? -1 : 0;
}

static int hwa_item_full_store_event(char **fields,
                                     HWAItemFullReadState *state,
                                     char *error,
                                     size_t error_size)
{
    HWAItemSet *items = &state->result.items;
    HWAItemEvent *event;
    char **source_targets[7];
    char **label_targets[11];
    size_t index;
    int tempo_result;

    if (hwa_item_full_reserve(
            state, (void **)&items->events, &state->event_capacity,
            items->event_count, state->limits.max_events,
            sizeof(*items->events), "event", error, error_size) != 0) {
        return -1;
    }
    event = &items->events[items->event_count++];
    if (hwa_item_parse_u64(fields[1], &event->id) != 0 ||
        hwa_item_parse_double(fields[4], 0, &event->score_start_beat) != 0 ||
        hwa_item_parse_double(fields[5], 0, &event->score_end_beat) != 0 ||
        hwa_item_parse_double(fields[6], 0,
                              &event->score_start_seconds) != 0 ||
        hwa_item_parse_double(fields[7], 0,
                              &event->score_end_seconds) != 0 ||
        hwa_item_parse_u64(fields[8], &event->audio_start_sample) != 0 ||
        hwa_item_parse_u64(fields[9], &event->audio_end_sample) != 0 ||
        hwa_item_parse_double(fields[10], 0,
                              &event->audio_start_seconds) != 0 ||
        hwa_item_parse_double(fields[11], 0,
                              &event->audio_end_seconds) != 0 ||
        hwa_item_parse_double(fields[12], 0,
                              &event->alignment_confidence) != 0 ||
        hwa_item_parse_u32(fields[14],
                           &event->alignment_evidence_flags) != 0 ||
        hwa_item_parse_u32(fields[34],
                           &event->labels.override_flags) != 0) {
        return -1;
    }
    tempo_result = hwa_item_parse_double(fields[22], 1, &event->tempo_bpm);
    if (tempo_result < 0 ||
        hwa_item_full_copy_field(state, &event->event_id, fields[2], 0,
                                 error, error_size) != 0 ||
        hwa_item_full_copy_field(state, &event->kind, fields[3], 0,
                                 error, error_size) != 0) {
        return -1;
    }
    event->tempo_valid = tempo_result == 0;
    event->alignment_status = hwa_item_full_status(fields[13]);
    source_targets[0] = &event->voice;
    source_targets[1] = &event->midi_note;
    source_targets[2] = &event->velocity;
    source_targets[3] = &event->tie;
    source_targets[4] = &event->dynamic;
    source_targets[5] = &event->mark;
    source_targets[6] = &event->score_position;
    for (index = 0U; index < 7U; ++index) {
        if (hwa_item_full_copy_field(state, source_targets[index],
                                     fields[15U + index], 1,
                                     error, error_size) != 0) {
            return -1;
        }
    }
    label_targets[0] = &event->labels.pitch;
    label_targets[1] = &event->labels.register_name;
    label_targets[2] = &event->labels.dynamic;
    label_targets[3] = &event->labels.articulation;
    label_targets[4] = &event->labels.part;
    label_targets[5] = &event->labels.physical_element;
    label_targets[6] = &event->labels.controller;
    label_targets[7] = &event->labels.technique;
    label_targets[8] = &event->labels.score_section;
    label_targets[9] = &event->labels.transition;
    label_targets[10] = &event->labels.gesture;
    for (index = 0U; index < 11U; ++index) {
        if (hwa_item_full_copy_field(state, label_targets[index],
                                     fields[23U + index], 1,
                                     error, error_size) != 0) {
            return -1;
        }
    }
    return 0;
}

static int hwa_item_full_store_item(char **fields,
                                    HWAItemFullReadState *state,
                                    char *error,
                                    size_t error_size)
{
    HWAItemSet *items = &state->result.items;
    HWAItem *item;
    int parent_valid;

    if (hwa_item_full_reserve(
            state, (void **)&items->items, &state->item_capacity,
            items->item_count, state->limits.max_items,
            sizeof(*items->items), "row", error, error_size) != 0) {
        return -1;
    }
    item = &items->items[items->item_count++];
    if (hwa_item_parse_u64(fields[1], &item->id) != 0 ||
        hwa_item_parse_optional_u64(fields[5], &item->parent_id,
                                    &parent_valid) != 0 ||
        hwa_item_parse_u64(fields[6], &item->start_sample) != 0 ||
        hwa_item_parse_u64(fields[7], &item->end_sample) != 0 ||
        hwa_item_parse_double(fields[8], 0, &item->start_seconds) != 0 ||
        hwa_item_parse_double(fields[9], 0, &item->end_seconds) != 0 ||
        hwa_item_parse_double(fields[10], 0, &item->score_start_beat) != 0 ||
        hwa_item_parse_double(fields[11], 0, &item->score_end_beat) != 0 ||
        hwa_item_parse_double(fields[12], 0, &item->confidence) != 0 ||
        hwa_item_parse_u32(fields[13], &item->evidence_flags) != 0 ||
        hwa_item_parse_u32(fields[14], &item->quality_flags) != 0 ||
        hwa_item_parse_bit(fields[16], &item->locked) != 0 ||
        hwa_item_parse_bit(fields[17], &item->excluded) != 0 ||
        hwa_item_full_copy_field(state, &item->key, fields[2], 0,
                                 error, error_size) != 0 ||
        hwa_item_full_copy_field(state, &item->role, fields[4], 0,
                                 error, error_size) != 0 ||
        hwa_item_full_copy_field(state, &item->exclusion_reason,
                                 fields[18], 1,
                                 error, error_size) != 0) {
        return -1;
    }
    item->kind = hwa_item_full_kind(fields[3]);
    item->origin = strcmp(fields[15], "auto") == 0
                       ? HWA_ITEM_ORIGIN_AUTO : HWA_ITEM_ORIGIN_MANUAL;
    item->parent_valid = parent_valid;
    if (item->locked) {
        item->start_seconds = (double)item->start_sample /
                              (double)items->audio_format.sample_rate_hz;
        item->end_seconds = (double)item->end_sample /
                            (double)items->audio_format.sample_rate_hz;
    }
    return 0;
}

static int hwa_item_full_member_compare(const HWAItemMember *left,
                                        const HWAItemMember *right)
{
    if (left->item_id != right->item_id) {
        return left->item_id < right->item_id ? -1 : 1;
    }
    if (left->order != right->order) {
        return left->order < right->order ? -1 : 1;
    }
    if (left->event_id != right->event_id) {
        return left->event_id < right->event_id ? -1 : 1;
    }
    return left->role < right->role ? -1 : left->role > right->role ? 1 : 0;
}

static int hwa_item_full_store_member(char **fields,
                                      HWAItemFullReadState *state,
                                      char *error,
                                      size_t error_size)
{
    HWAItemSet *items = &state->result.items;
    HWAItemMember *member;

    if (hwa_item_full_reserve(
            state, (void **)&items->members, &state->member_capacity,
            items->member_count, state->limits.max_members,
            sizeof(*items->members), "member", error, error_size) != 0) {
        return -1;
    }
    member = &items->members[items->member_count++];
    if (hwa_item_parse_u64(fields[1], &member->item_id) != 0 ||
        hwa_item_parse_u64(fields[2], &member->event_id) != 0 ||
        hwa_item_parse_u32(fields[3], &member->order) != 0) {
        return -1;
    }
    member->role = hwa_item_full_member_role(fields[4]);
    if (state->have_prior_member &&
        hwa_item_full_member_compare(&state->prior_member, member) >= 0) {
        hwa_set_error(error, error_size,
                      "item MEMBER rows are outside canonical order");
        return -1;
    }
    state->prior_member = *member;
    state->have_prior_member = 1;
    return 0;
}

static int hwa_item_full_store_warning(char **fields,
                                       HWAItemFullReadState *state,
                                       char *error,
                                       size_t error_size)
{
    HWAItemSet *items = &state->result.items;
    HWAItemWarning *warning;

    if (hwa_item_full_reserve(
            state, (void **)&items->warnings, &state->warning_capacity,
            items->warning_count, state->limits.max_warnings,
            sizeof(*items->warnings), "warning", error, error_size) != 0) {
        return -1;
    }
    warning = &items->warnings[items->warning_count++];
    if (hwa_item_parse_u64(fields[1], &warning->id) != 0 ||
        hwa_item_parse_optional_u64(fields[4], &warning->item_id,
                                    &warning->item_id_valid) != 0 ||
        hwa_item_parse_optional_u64(fields[5], &warning->event_id,
                                    &warning->event_id_valid) != 0 ||
        hwa_item_full_copy_field(state, &warning->code, fields[2], 0,
                                 error, error_size) != 0 ||
        hwa_item_full_copy_field(state, &warning->message, fields[3], 0,
                                 error, error_size) != 0) {
        return -1;
    }
    return 0;
}

static int hwa_item_full_read_row(char **fields,
                                  size_t field_count,
                                  size_t row,
                                  void *user,
                                  char *error,
                                  size_t error_size)
{
    HWAItemFullReadState *state = (HWAItemFullReadState *)user;
    size_t index;

    if (field_count > state->limits.max_fields_per_row) {
        hwa_set_error(error, error_size,
                      "item row %zu exceeds the current field limit", row);
        return -1;
    }
    for (index = 0U; index < field_count; ++index) {
        if (strlen(fields[index]) > state->limits.max_field_bytes) {
            hwa_set_error(error, error_size,
                          "item row %zu exceeds the current field-byte limit",
                          row);
            return -1;
        }
    }
    if (hwa_item_read_row(fields, field_count, row, &state->validator,
                          error, error_size) != 0) {
        return -1;
    }
    if (state->validator.row_count == 1U) return 0;
    if (strcmp(fields[0], "META") == 0) {
        if (hwa_item_full_store_meta(fields, state, error, error_size) != 0) {
            hwa_set_error(error, error_size,
                          "item row %zu has an unsupported META value", row);
            return -1;
        }
        return 0;
    }
    if (strcmp(fields[0], "INPUT") == 0) {
        return hwa_item_full_store_input(fields, state, error, error_size);
    }
    if (strcmp(fields[0], "SOURCE") == 0) {
        return hwa_item_full_store_source(fields, state, error, error_size);
    }
    if (strcmp(fields[0], "EVENT") == 0) {
        return hwa_item_full_store_event(fields, state, error, error_size);
    }
    if (strcmp(fields[0], "ITEM") == 0) {
        return hwa_item_full_store_item(fields, state, error, error_size);
    }
    if (strcmp(fields[0], "MEMBER") == 0) {
        return hwa_item_full_store_member(fields, state, error, error_size);
    }
    return hwa_item_full_store_warning(fields, state, error, error_size);
}

static int hwa_item_full_event_pointer_compare(const void *left,
                                               const void *right)
{
    const HWAItemEvent *const *first =
        (const HWAItemEvent *const *)left;
    const HWAItemEvent *const *second =
        (const HWAItemEvent *const *)right;
    return strcmp((*first)->event_id, (*second)->event_id);
}

static int hwa_item_full_key_pointer_compare(const void *left,
                                             const void *right)
{
    const HWAItem *const *first = (const HWAItem *const *)left;
    const HWAItem *const *second = (const HWAItem *const *)right;
    return strcmp((*first)->key, (*second)->key);
}

static int hwa_item_full_validate_unique(HWAItemFullReadState *state,
                                         char *error,
                                         size_t error_size)
{
    HWAItemSet *items = &state->result.items;
    size_t maximum = items->event_count > items->item_count
                         ? items->event_count : items->item_count;
    void **order;
    uint64_t bytes;
    size_t index;

    if (maximum < 2U) return 0;
    if (maximum > SIZE_MAX / sizeof(*order) ||
        (uint64_t)maximum > UINT64_MAX / (uint64_t)sizeof(*order)) {
        hwa_set_error(error, error_size,
                      "item identity index storage overflows");
        return -1;
    }
    bytes = (uint64_t)maximum * (uint64_t)sizeof(*order);
    if (hwa_item_full_charge(state, bytes, error, error_size) != 0) return -1;
    order = (void **)malloc(maximum * sizeof(*order));
    if (order == NULL) {
        state->work_bytes -= bytes;
        hwa_set_error(error, error_size,
                      "out of memory for item identity checks");
        return -1;
    }
    if (items->event_count > 1U) {
        for (index = 0U; index < items->event_count; ++index) {
            order[index] = &items->events[index];
        }
        qsort(order, items->event_count, sizeof(*order),
              hwa_item_full_event_pointer_compare);
        for (index = 1U; index < items->event_count; ++index) {
            const HWAItemEvent *prior =
                (const HWAItemEvent *)order[index - 1U];
            const HWAItemEvent *current =
                (const HWAItemEvent *)order[index];
            if (strcmp(prior->event_id, current->event_id) == 0) {
                hwa_set_error(error, error_size,
                              "item EVENT event_id values must be unique");
                free(order);
                state->work_bytes -= bytes;
                return -1;
            }
        }
    }
    if (items->item_count > 1U) {
        for (index = 0U; index < items->item_count; ++index) {
            order[index] = &items->items[index];
        }
        qsort(order, items->item_count, sizeof(*order),
              hwa_item_full_key_pointer_compare);
        for (index = 1U; index < items->item_count; ++index) {
            const HWAItem *prior = (const HWAItem *)order[index - 1U];
            const HWAItem *current = (const HWAItem *)order[index];
            if (strcmp(prior->key, current->key) == 0) {
                hwa_set_error(error, error_size,
                              "item ITEM key values must be unique");
                free(order);
                state->work_bytes -= bytes;
                return -1;
            }
        }
    }
    free(order);
    state->work_bytes -= bytes;
    return 0;
}

static int hwa_item_full_validate_result(HWAItemFullReadState *state,
                                         char *error,
                                         size_t error_size)
{
    HWAItemSet *items = &state->result.items;
    const HWASegmentationOptions *option = &items->options;
    const uint32_t alignment_evidence_mask =
        HWA_ALIGNMENT_EVIDENCE_CHROMA |
        HWA_ALIGNMENT_EVIDENCE_SPECTRAL_ONSET |
        HWA_ALIGNMENT_EVIDENCE_ENERGY_ONSET |
        HWA_ALIGNMENT_EVIDENCE_PHASE_ONSET |
        HWA_ALIGNMENT_EVIDENCE_PITCH |
        HWA_ALIGNMENT_EVIDENCE_ENVELOPE;
    const uint32_t label_mask =
        HWA_LABEL_OVERRIDE_PITCH | HWA_LABEL_OVERRIDE_REGISTER |
        HWA_LABEL_OVERRIDE_DYNAMIC | HWA_LABEL_OVERRIDE_ARTICULATION |
        HWA_LABEL_OVERRIDE_PART | HWA_LABEL_OVERRIDE_PHYSICAL_ELEMENT |
        HWA_LABEL_OVERRIDE_CONTROLLER | HWA_LABEL_OVERRIDE_TECHNIQUE |
        HWA_LABEL_OVERRIDE_SCORE_SECTION | HWA_LABEL_OVERRIDE_TRANSITION |
        HWA_LABEL_OVERRIDE_GESTURE;
    const uint32_t item_evidence_mask =
        HWA_ITEM_EVIDENCE_ALIGNMENT | HWA_ITEM_EVIDENCE_ONSET |
        HWA_ITEM_EVIDENCE_ENERGY | HWA_ITEM_EVIDENCE_PITCH |
        HWA_ITEM_EVIDENCE_SCORE | HWA_ITEM_EVIDENCE_MANUAL;
    const uint32_t item_quality_mask =
        HWA_ITEM_QUALITY_LOW_CONFIDENCE | HWA_ITEM_QUALITY_NO_EVIDENCE |
        HWA_ITEM_QUALITY_COLLAPSED | HWA_ITEM_QUALITY_TRUNCATED;
    double expected_duration;
    size_t index;

    if (!state->saved_audio_duration_valid ||
        !state->saved_source_duration_valid ||
        items->audio_format.sample_rate_hz == 0U) {
        hwa_set_error(error, error_size, "item file is missing duration facts");
        return -1;
    }
    expected_duration = (double)items->audio_format.frames /
                        (double)items->audio_format.sample_rate_hz;
    if (fabs(items->audio_format.duration_seconds - expected_duration) >
            1e-12 ||
        option->decode_block_frames == 0U || option->max_input_bytes == 0U ||
        option->max_input_frames == 0U ||
        option->max_analysis_work_bytes == 0U ||
        option->max_transforms == 0U || option->max_track_points == 0U ||
        option->max_segmentation_work_bytes == 0U ||
        option->max_boundary_evaluations == 0U ||
        option->max_events == 0U || option->max_items == 0U ||
        option->max_item_members == 0U || option->max_label_rows == 0U ||
        option->max_manual_items == 0U ||
        items->event_count > option->max_events ||
        items->item_count > option->max_items ||
        items->member_count > option->max_item_members ||
        items->retained_work_bytes > option->max_segmentation_work_bytes) {
        hwa_set_error(error, error_size,
                      "item file has inconsistent saved limits or duration");
        return -1;
    }
    for (index = 0U; index < items->event_count; ++index) {
        const HWAItemEvent *event = &items->events[index];
        if ((event->alignment_evidence_flags & ~alignment_evidence_mask) != 0U ||
            (event->labels.override_flags & ~label_mask) != 0U) {
            hwa_set_error(error, error_size,
                          "item EVENT has unknown flag bits");
            return -1;
        }
    }
    for (index = 0U; index < items->item_count; ++index) {
        const HWAItem *item = &items->items[index];
        if ((item->evidence_flags & ~item_evidence_mask) != 0U ||
            (item->quality_flags & ~item_quality_mask) != 0U) {
            hwa_set_error(error, error_size,
                          "item ITEM has unknown flag bits");
            return -1;
        }
    }
    items->locked_item_count = state->validator.locked_count;
    items->excluded_item_count = state->validator.excluded_count;
    items->low_confidence_item_count = state->validator.low_confidence_count;
    (void)state->saved_locked_count;
    (void)state->saved_excluded_count;
    if (state->saved_low_confidence_count !=
        items->low_confidence_item_count) {
        hwa_set_error(error, error_size,
                      "item low-confidence count does not match META");
        return -1;
    }
    return hwa_item_full_validate_unique(state, error, error_size);
}

int hwa_item_file_read_full(const char *path,
                            const HWAItemFileLimits *limits,
                            HWAItemFileData *result,
                            char *error,
                            size_t error_size)
{
    HWAItemFileLimits copied;
    HWAItemFullReadState state;
    unsigned char *data = NULL;
    size_t size = 0U;
    uint64_t parser_bytes;
    uint64_t file_limit;
    unsigned char digest[32];
    HWASha256 sha;
    char after[HWA_SHA256_HEX_SIZE];

    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (limits == NULL || result == NULL) {
        hwa_set_error(error, error_size,
                      "invalid full item-reader arguments");
        return -1;
    }
    copied = *limits;
    memset(result, 0, sizeof(*result));
    if (copied.max_bytes == 0U || copied.max_work_bytes == 0U ||
        copied.max_fields_per_row == 0U || copied.max_field_bytes == 0U ||
        copied.max_events == 0U || copied.max_items == 0U ||
        copied.max_manual_items == 0U || copied.max_members == 0U ||
        copied.max_warnings == 0U) {
        hwa_set_error(error, error_size,
                      "full item-reader limits must be nonzero");
        return -1;
    }
    file_limit = copied.max_bytes;
    if (file_limit > copied.max_work_bytes / 2U) {
        file_limit = copied.max_work_bytes / 2U;
    }
    if (file_limit == 0U ||
        hwa_item_read_regular(path, file_limit, &data, &size,
                              error, error_size) != 0) {
        return -1;
    }
    if ((uint64_t)size + 1U > UINT64_MAX / 2U ||
        (uint64_t)HWA_ITEM_FILE_MAX_FIELDS >
            UINT64_MAX / (uint64_t)sizeof(char *)) {
        hwa_set_error(error, error_size, "item parser storage overflows");
        free(data);
        return -1;
    }
    parser_bytes = ((uint64_t)size + 1U) * 2U +
                   (uint64_t)HWA_ITEM_FILE_MAX_FIELDS *
                       (uint64_t)sizeof(char *);
    if (parser_bytes > copied.max_work_bytes) {
        hwa_set_error(error, error_size,
                      "item parser exceeds the current work-byte limit");
        free(data);
        return -1;
    }
    memset(&state, 0, sizeof(state));
    state.limits = copied;
    state.validator.limits = copied;
    state.validator.retain_edits = 0;
    state.validator.retain_item_keys = 0;
    state.work_bytes = parser_bytes;
    hwa_segmentation_options_default(&state.result.items.options);
    hwa_sha256_init(&sha);
    hwa_sha256_update(&sha, data, size);
    hwa_sha256_final(&sha, digest);
    hwa_sha256_hex(digest, state.result.sha256);
    if (hwa_item_csv_rows(data, size, hwa_item_full_read_row, &state,
                          error, error_size) != 0 ||
        state.validator.row_count == 0U ||
        state.validator.meta_index != HWA_ITEM_META_COUNT ||
        state.validator.input_count < 2U ||
        state.validator.source_count != 1U ||
        state.validator.event_count != state.validator.expected_event_count ||
        state.validator.item_count != state.validator.expected_item_count ||
        state.validator.member_count != state.validator.expected_member_count ||
        state.validator.warning_count !=
            state.validator.expected_warning_count ||
        state.result.items.event_count != state.validator.event_count ||
        state.result.items.item_count != state.validator.item_count ||
        state.result.items.member_count != state.validator.member_count ||
        state.result.items.warning_count != state.validator.warning_count ||
        hwa_item_full_validate_result(&state, error, error_size) != 0) {
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "item file counts do not match META");
        }
        free(data);
        hwa_item_file_data_free(&state.result);
        return -1;
    }
    free(data);
    state.work_bytes -= parser_bytes;
    if (hwa_sha256_file(path, copied.max_bytes, after,
                        error, error_size) != 0 ||
        strcmp(after, state.result.sha256) != 0) {
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "item file changed while it was read");
        }
        hwa_item_file_data_free(&state.result);
        return -1;
    }
    state.result.path = hwa_item_full_copy(
        &state, path, 0, error, error_size);
    if (state.result.path == NULL) {
        hwa_item_file_data_free(&state.result);
        return -1;
    }
    state.result.retained_work_bytes = state.work_bytes;
    *result = state.result;
    return 0;
}

void hwa_item_file_data_free(HWAItemFileData *result)
{
    if (result == NULL) return;
    hwa_item_set_free(&result->items);
    free(result->path);
    memset(result, 0, sizeof(*result));
}

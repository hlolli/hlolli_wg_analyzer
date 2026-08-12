#include "item_report.h"

#include "alignment_file.h"
#include "item_file.h"
#include "output.h"

#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>

static int hwa_item_json_number(FILE *stream, double value)
{
    if (!isfinite(value)) return -1;
    return fprintf(stream, "%.17g", value == 0.0 ? 0.0 : value) < 0 ? -1 : 0;
}

static int hwa_item_json_nullable(FILE *stream, const char *text)
{
    return text == NULL ? (fputs("null", stream) == EOF ? -1 : 0) :
                          hwa_json_write_string(stream, text);
}

static const char *hwa_item_json_kind(HWAItemKind kind)
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

static const char *hwa_item_json_origin(HWAItemOrigin origin)
{
    return origin == HWA_ITEM_ORIGIN_AUTO ? "auto" :
           origin == HWA_ITEM_ORIGIN_MANUAL ? "manual" : NULL;
}

static const char *hwa_item_json_member_role(HWAItemMemberRole role)
{
    switch (role) {
    case HWA_ITEM_MEMBER_SOURCE: return "source";
    case HWA_ITEM_MEMBER_FROM: return "from";
    case HWA_ITEM_MEMBER_TO: return "to";
    case HWA_ITEM_MEMBER_ACTIVE: return "active";
    default: return NULL;
    }
}

static const char *hwa_item_json_alignment_status(HWAAlignmentStatus status)
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

static int hwa_item_json_build(FILE *stream)
{
    return fputs("{\"compiler_family\":", stream) == EOF ||
           hwa_json_write_string(stream, hwa_build_compiler_family()) != 0 ||
           fputs(",\"compiler_version\":", stream) == EOF ||
           hwa_json_write_string(stream, hwa_build_compiler_version()) != 0 ||
           fputs(",\"c_standard\":", stream) == EOF ||
           hwa_json_write_string(stream, hwa_build_c_standard()) != 0 ||
           fputs(",\"target_os\":", stream) == EOF ||
           hwa_json_write_string(stream, hwa_build_target_os()) != 0 ||
           fprintf(stream, ",\"pointer_bits\":%u,\"endianness\":",
                   hwa_build_pointer_bits()) < 0 ||
           hwa_json_write_string(stream, hwa_build_endianness()) != 0 ||
           fputs(",\"mode\":", stream) == EOF ||
           hwa_json_write_string(stream, hwa_build_mode()) != 0 ||
           fputc('}', stream) == EOF ? -1 : 0;
}

static int hwa_item_json_options(FILE *stream,
                                 const HWASegmentationOptions *option)
{
    if (fprintf(stream,
                "{\"decode_block_frames\":%zu"
                ",\"max_input_bytes\":%" PRIu64
                ",\"max_input_frames\":%" PRIu64
                ",\"max_analysis_work_bytes\":%" PRIu64
                ",\"max_transforms\":%zu"
                ",\"max_track_points\":%zu"
                ",\"boundary_search_seconds\":",
                option->decode_block_frames,
                option->max_input_bytes,
                option->max_input_frames,
                option->max_analysis_work_bytes,
                option->max_transforms,
                option->max_track_points) < 0 ||
        hwa_item_json_number(stream, option->boundary_search_seconds) != 0 ||
        fputs(",\"tail_limit_seconds\":", stream) == EOF ||
        hwa_item_json_number(stream, option->tail_limit_seconds) != 0 ||
        fputs(",\"min_phase_seconds\":", stream) == EOF ||
        hwa_item_json_number(stream, option->min_phase_seconds) != 0 ||
        fputs(",\"min_body_seconds\":", stream) == EOF ||
        hwa_item_json_number(stream, option->min_body_seconds) != 0 ||
        fputs(",\"item_confidence_threshold\":", stream) == EOF ||
        hwa_item_json_number(stream, option->item_confidence_threshold) != 0 ||
        fprintf(stream,
                ",\"max_segmentation_work_bytes\":%" PRIu64
                ",\"max_boundary_evaluations\":%" PRIu64
                ",\"max_events\":%zu"
                ",\"max_items\":%zu"
                ",\"max_item_members\":%zu"
                ",\"max_label_rows\":%zu"
                ",\"max_manual_items\":%zu}",
                option->max_segmentation_work_bytes,
                option->max_boundary_evaluations,
                option->max_events,
                option->max_items,
                option->max_item_members,
                option->max_label_rows,
                option->max_manual_items) < 0) {
        return -1;
    }
    return 0;
}

static int hwa_item_json_input(FILE *stream,
                               const char *role,
                               const char *path,
                               const char *sha256,
                               const HWAFormat *format)
{
    if (fputs("{\"role\":", stream) == EOF ||
        hwa_json_write_string(stream, role) != 0 ||
        fputs(",\"path\":", stream) == EOF ||
        hwa_json_write_string(stream, path) != 0 ||
        fputs(",\"path_encoding\":"
              "\"utf8_with_invalid_bytes_as_u00xx\","
              "\"path_bytes_hex\":", stream) == EOF ||
        hwa_json_write_byte_hex(stream, path) != 0 ||
        fputs(",\"sha256\":", stream) == EOF ||
        hwa_json_write_string(stream, sha256) != 0 ||
        fputs(",\"duration_seconds\":", stream) == EOF) {
        return -1;
    }
    if (format != NULL) {
        if (hwa_item_json_number(stream, format->duration_seconds) != 0 ||
            fprintf(stream,
                    ",\"sample_rate_hz\":%" PRIu32
                    ",\"frames\":%" PRIu64,
                    format->sample_rate_hz, format->frames) < 0) {
            return -1;
        }
    } else if (fputs("null,\"sample_rate_hz\":null,\"frames\":null",
                     stream) == EOF) {
        return -1;
    }
    return fputc('}', stream) == EOF ? -1 : 0;
}

static int hwa_item_json_inputs(FILE *stream, const HWAItemSet *items)
{
    if (fputc('[', stream) == EOF ||
        hwa_item_json_input(stream, "alignment", items->alignment_path,
                            items->alignment_sha256, NULL) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_item_json_input(stream, "audio", items->audio_path,
                            items->audio_sha256, &items->audio_format) != 0) {
        return -1;
    }
    if (items->labels_path != NULL &&
        (fputc(',', stream) == EOF ||
         hwa_item_json_input(stream, "labels", items->labels_path,
                             items->labels_sha256, NULL) != 0)) {
        return -1;
    }
    if (items->amendment_path != NULL &&
        (fputc(',', stream) == EOF ||
         hwa_item_json_input(stream, "amendment", items->amendment_path,
                             items->amendment_sha256, NULL) != 0)) {
        return -1;
    }
    return fputc(']', stream) == EOF ? -1 : 0;
}

static int hwa_item_json_labels(FILE *stream, const HWATypedLabels *labels)
{
    const char *names[] = {
        "pitch", "register", "dynamic", "articulation", "part",
        "physical_element", "controller", "technique", "score_section",
        "transition", "gesture"
    };
    const char *values[] = {
        labels->pitch, labels->register_name, labels->dynamic,
        labels->articulation, labels->part, labels->physical_element,
        labels->controller, labels->technique, labels->score_section,
        labels->transition, labels->gesture
    };
    size_t index;

    if (fputc('{', stream) == EOF) return -1;
    for (index = 0U; index < 11U; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_json_write_string(stream, names[index]) != 0 ||
            fputc(':', stream) == EOF ||
            hwa_item_json_nullable(stream, values[index]) != 0) {
            return -1;
        }
    }
    return fprintf(stream, ",\"override_flags\":%" PRIu32 "}",
                   labels->override_flags) < 0 ? -1 : 0;
}

static int hwa_item_json_event(FILE *stream, const HWAItemEvent *event)
{
    const char *status = hwa_item_json_alignment_status(
        event->alignment_status);
    if (status == NULL ||
        fprintf(stream, "{\"id\":%" PRIu64 ",\"event_id\":",
                event->id) < 0 ||
        hwa_json_write_string(stream, event->event_id) != 0 ||
        fputs(",\"kind\":", stream) == EOF ||
        hwa_json_write_string(stream, event->kind) != 0 ||
        fputs(",\"score_start_beat\":", stream) == EOF ||
        hwa_item_json_number(stream, event->score_start_beat) != 0 ||
        fputs(",\"score_end_beat\":", stream) == EOF ||
        hwa_item_json_number(stream, event->score_end_beat) != 0 ||
        fputs(",\"score_start_seconds\":", stream) == EOF ||
        hwa_item_json_number(stream, event->score_start_seconds) != 0 ||
        fputs(",\"score_end_seconds\":", stream) == EOF ||
        hwa_item_json_number(stream, event->score_end_seconds) != 0 ||
        fprintf(stream,
                ",\"audio_start_sample\":%" PRIu64
                ",\"audio_end_sample\":%" PRIu64
                ",\"audio_start_seconds\":",
                event->audio_start_sample, event->audio_end_sample) < 0 ||
        hwa_item_json_number(stream, event->audio_start_seconds) != 0 ||
        fputs(",\"audio_end_seconds\":", stream) == EOF ||
        hwa_item_json_number(stream, event->audio_end_seconds) != 0 ||
        fputs(",\"alignment_confidence\":", stream) == EOF ||
        hwa_item_json_number(stream, event->alignment_confidence) != 0 ||
        fputs(",\"alignment_status\":", stream) == EOF ||
        hwa_json_write_string(stream, status) != 0 ||
        fprintf(stream,
                ",\"alignment_evidence_flags\":%" PRIu32
                ",\"voice\":",
                event->alignment_evidence_flags) < 0 ||
        hwa_item_json_nullable(stream, event->voice) != 0 ||
        fputs(",\"midi_note\":", stream) == EOF ||
        hwa_item_json_nullable(stream, event->midi_note) != 0 ||
        fputs(",\"velocity\":", stream) == EOF ||
        hwa_item_json_nullable(stream, event->velocity) != 0 ||
        fputs(",\"tie\":", stream) == EOF ||
        hwa_item_json_nullable(stream, event->tie) != 0 ||
        fputs(",\"dynamic\":", stream) == EOF ||
        hwa_item_json_nullable(stream, event->dynamic) != 0 ||
        fputs(",\"mark\":", stream) == EOF ||
        hwa_item_json_nullable(stream, event->mark) != 0 ||
        fputs(",\"score_position\":", stream) == EOF ||
        hwa_item_json_nullable(stream, event->score_position) != 0 ||
        fputs(",\"tempo_bpm\":", stream) == EOF) {
        return -1;
    }
    if (event->tempo_valid) {
        if (hwa_item_json_number(stream, event->tempo_bpm) != 0) return -1;
    } else if (fputs("null", stream) == EOF) {
        return -1;
    }
    if (fputs(",\"labels\":", stream) == EOF ||
        hwa_item_json_labels(stream, &event->labels) != 0 ||
        fputc('}', stream) == EOF) {
        return -1;
    }
    return 0;
}

static int hwa_item_json_item(FILE *stream, const HWAItem *item)
{
    const char *kind = hwa_item_json_kind(item->kind);
    const char *origin = hwa_item_json_origin(item->origin);
    if (kind == NULL || origin == NULL ||
        fprintf(stream, "{\"id\":%" PRIu64 ",\"key\":", item->id) < 0 ||
        hwa_json_write_string(stream, item->key) != 0 ||
        fputs(",\"kind\":", stream) == EOF ||
        hwa_json_write_string(stream, kind) != 0 ||
        fputs(",\"role\":", stream) == EOF ||
        hwa_json_write_string(stream, item->role) != 0 ||
        fputs(",\"parent_id\":", stream) == EOF) {
        return -1;
    }
    if (item->parent_valid) {
        if (fprintf(stream, "%" PRIu64, item->parent_id) < 0) return -1;
    } else if (fputs("null", stream) == EOF) {
        return -1;
    }
    if (fprintf(stream,
                ",\"start_sample\":%" PRIu64
                ",\"end_sample\":%" PRIu64
                ",\"start_seconds\":",
                item->start_sample, item->end_sample) < 0 ||
        hwa_item_json_number(stream, item->start_seconds) != 0 ||
        fputs(",\"end_seconds\":", stream) == EOF ||
        hwa_item_json_number(stream, item->end_seconds) != 0 ||
        fputs(",\"score_start_beat\":", stream) == EOF ||
        hwa_item_json_number(stream, item->score_start_beat) != 0 ||
        fputs(",\"score_end_beat\":", stream) == EOF ||
        hwa_item_json_number(stream, item->score_end_beat) != 0 ||
        fputs(",\"confidence\":", stream) == EOF ||
        hwa_item_json_number(stream, item->confidence) != 0 ||
        fprintf(stream,
                ",\"evidence_flags\":%" PRIu32
                ",\"quality_flags\":%" PRIu32
                ",\"origin\":",
                item->evidence_flags, item->quality_flags) < 0 ||
        hwa_json_write_string(stream, origin) != 0 ||
        fprintf(stream, ",\"locked\":%s,\"excluded\":%s,"
                        "\"exclusion_reason\":",
                item->locked ? "true" : "false",
                item->excluded ? "true" : "false") < 0 ||
        hwa_item_json_nullable(stream, item->exclusion_reason) != 0 ||
        fputc('}', stream) == EOF) {
        return -1;
    }
    return 0;
}

static int hwa_item_json_member(FILE *stream,
                                const HWAItemMember *member)
{
    const char *role = hwa_item_json_member_role(member->role);
    return role == NULL ||
           fprintf(stream,
                   "{\"item_id\":%" PRIu64
                   ",\"event_id\":%" PRIu64
                   ",\"order\":%" PRIu32 ",\"relation\":",
                   member->item_id, member->event_id, member->order) < 0 ||
           hwa_json_write_string(stream, role) != 0 ||
           fputc('}', stream) == EOF ? -1 : 0;
}

static int hwa_item_json_warning(FILE *stream,
                                 const HWAItemWarning *warning)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"code\":",
                warning->id) < 0 ||
        hwa_json_write_string(stream, warning->code) != 0 ||
        fputs(",\"message\":", stream) == EOF ||
        hwa_json_write_string(stream, warning->message) != 0 ||
        fputs(",\"item_id\":", stream) == EOF) {
        return -1;
    }
    if (warning->item_id_valid) {
        if (fprintf(stream, "%" PRIu64, warning->item_id) < 0) return -1;
    } else if (fputs("null", stream) == EOF) return -1;
    if (fputs(",\"event_id\":", stream) == EOF) return -1;
    if (warning->event_id_valid) {
        if (fprintf(stream, "%" PRIu64, warning->event_id) < 0) return -1;
    } else if (fputs("null", stream) == EOF) return -1;
    return fputc('}', stream) == EOF ? -1 : 0;
}

int hwa_report_items_json(FILE *stream, const HWAItemSet *items)
{
    size_t index;
    if (stream == NULL || items == NULL ||
        fputs("{\"schema_version\":4,\"command\":\"segment\","
              "\"items_schema_version\":1,\"tool_version\":", stream) == EOF ||
        hwa_json_write_string(stream, HWA_VERSION) != 0 ||
        fputs(",\"analysis_method_version\":", stream) == EOF ||
        hwa_json_write_string(stream, HWA_ANALYSIS_METHOD_VERSION) != 0 ||
        fputs(",\"alignment_method_version\":", stream) == EOF ||
        hwa_json_write_string(stream, HWA_ALIGNMENT_METHOD_VERSION) != 0 ||
        fputs(",\"method_version\":", stream) == EOF ||
        hwa_json_write_string(stream, HWA_SEGMENTATION_METHOD_VERSION) != 0 ||
        fputs(",\"build\":", stream) == EOF ||
        hwa_item_json_build(stream) != 0 ||
        fputs(",\"options\":", stream) == EOF ||
        hwa_item_json_options(stream, &items->options) != 0 ||
        fputs(",\"inputs\":", stream) == EOF ||
        hwa_item_json_inputs(stream, items) != 0 ||
        fputs(",\"source\":{\"role\":\"score\",\"path\":", stream) == EOF ||
        hwa_json_write_string(stream, items->source_score_path) != 0 ||
        fputs(",\"path_encoding\":"
              "\"utf8_with_invalid_bytes_as_u00xx\","
              "\"path_bytes_hex\":", stream) == EOF ||
        hwa_json_write_byte_hex(stream, items->source_score_path) != 0 ||
        fputs(",\"sha256\":", stream) == EOF ||
        hwa_json_write_string(stream, items->source_score_sha256) != 0 ||
        fputs(",\"duration_seconds\":", stream) == EOF ||
        hwa_item_json_number(stream,
                             items->source_score_duration_seconds) != 0 ||
        fputs("},\"summary\":{\"alignment_confidence\":", stream) == EOF ||
        hwa_item_json_number(stream, items->alignment_confidence) != 0 ||
        fprintf(stream,
                ",\"boundary_evaluations\":%" PRIu64
                ",\"retained_work_bytes\":%" PRIu64
                ",\"event_count\":%zu,\"item_count\":%zu"
                ",\"member_count\":%zu,\"warning_count\":%zu"
                ",\"locked_item_count\":%zu"
                ",\"excluded_item_count\":%zu"
                ",\"low_confidence_item_count\":%zu},\"events\":[",
                items->boundary_evaluations,
                items->retained_work_bytes,
                items->event_count,
                items->item_count,
                items->member_count,
                items->warning_count,
                items->locked_item_count,
                items->excluded_item_count,
                items->low_confidence_item_count) < 0) {
        return -1;
    }
    for (index = 0U; index < items->event_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_item_json_event(stream, &items->events[index]) != 0) {
            return -1;
        }
    }
    if (fputs("],\"items\":[", stream) == EOF) return -1;
    for (index = 0U; index < items->item_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_item_json_item(stream, &items->items[index]) != 0) {
            return -1;
        }
    }
    if (fputs("],\"members\":[", stream) == EOF) return -1;
    for (index = 0U; index < items->member_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_item_json_member(stream, &items->members[index]) != 0) {
            return -1;
        }
    }
    if (fputs("],\"warnings\":[", stream) == EOF) return -1;
    for (index = 0U; index < items->warning_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_item_json_warning(stream, &items->warnings[index]) != 0) {
            return -1;
        }
    }
    return fputs("]}", stream) == EOF ? -1 : 0;
}

static int hwa_item_text_path(FILE *stream, const char *path)
{
    const unsigned char *cursor = (const unsigned char *)path;
    while (*cursor != 0U) {
        if (*cursor >= 0x20U && *cursor <= 0x7eU &&
            *cursor != (unsigned char)'\\') {
            if (fputc((int)*cursor, stream) == EOF) return -1;
        } else if (*cursor == (unsigned char)'\\') {
            if (fputs("\\\\", stream) == EOF) return -1;
        } else if (fprintf(stream, "\\x%02x", (unsigned)*cursor) < 0) {
            return -1;
        }
        cursor++;
    }
    return 0;
}

int hwa_report_items_text(FILE *stream, const HWAItemSet *items)
{
    if (stream == NULL || items == NULL ||
        fputs("Segmentation: score to audio\nAlignment: ", stream) == EOF ||
        hwa_item_text_path(stream, items->alignment_path) != 0 ||
        fputs("\nAudio: ", stream) == EOF ||
        hwa_item_text_path(stream, items->audio_path) != 0 ||
        fprintf(stream,
                "\nAlignment confidence: %.3f\n"
                "Events: %zu\nItems: %zu\nMembers: %zu\n"
                "Locked items: %zu\nExcluded items: %zu\n"
                "Low-confidence items: %zu\nWarnings: %zu\n"
                "Boundary evaluations: %" PRIu64 "\n",
                items->alignment_confidence,
                items->event_count,
                items->item_count,
                items->member_count,
                items->locked_item_count,
                items->excluded_item_count,
                items->low_confidence_item_count,
                items->warning_count,
                items->boundary_evaluations) < 0) {
        return -1;
    }
    return 0;
}

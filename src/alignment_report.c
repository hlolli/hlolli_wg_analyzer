#include "alignment_report.h"

#include "alignment_file.h"
#include "output.h"

#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static int hwa_alignment_json_number(FILE *stream, double value)
{
    if (!isfinite(value)) {
        return fputs("null", stream) == EOF ? -1 : 0;
    }
    return fprintf(stream, "%.17g", value == 0.0 ? 0.0 : value) < 0 ? -1 : 0;
}

static int hwa_alignment_json_optional_number(FILE *stream,
                                              double value,
                                              int valid)
{
    return valid ? hwa_alignment_json_number(stream, value) :
                   (fputs("null", stream) == EOF ? -1 : 0);
}

static int hwa_alignment_json_nullable_string(FILE *stream, const char *text)
{
    return text == NULL ? (fputs("null", stream) == EOF ? -1 : 0) :
                          hwa_json_write_string(stream, text);
}

static const char *hwa_alignment_json_mode(HWAAlignmentMode mode)
{
    return mode == HWA_ALIGNMENT_AUDIO_TO_AUDIO ? "audio_audio" :
           mode == HWA_ALIGNMENT_SCORE_TO_AUDIO ? "score_audio" : "unknown";
}

static const char *hwa_alignment_json_origin(HWAAlignmentOrigin origin)
{
    return origin == HWA_ALIGNMENT_ORIGIN_AUTO ? "auto" :
           origin == HWA_ALIGNMENT_ORIGIN_MANUAL ? "manual" : "unknown";
}

static const char *hwa_alignment_json_side(HWAAlignmentSide side)
{
    return side == HWA_ALIGNMENT_REFERENCE ? "reference" :
           side == HWA_ALIGNMENT_TARGET ? "target" : "unknown";
}

static const char *hwa_alignment_json_status(HWAAlignmentStatus status)
{
    switch (status) {
    case HWA_ALIGNMENT_MATCHED: return "matched";
    case HWA_ALIGNMENT_LOW_CONFIDENCE: return "low_confidence";
    case HWA_ALIGNMENT_SKIPPED: return "skipped";
    case HWA_ALIGNMENT_REPEATED: return "repeated";
    case HWA_ALIGNMENT_REST: return "rest";
    case HWA_ALIGNMENT_ORNAMENT: return "ornament";
    case HWA_ALIGNMENT_CADENZA: return "cadenza";
    default: return "unknown";
    }
}

static const char *hwa_alignment_json_reason(HWAUnmatchedReason reason)
{
    switch (reason) {
    case HWA_UNMATCHED_PREFIX: return "prefix";
    case HWA_UNMATCHED_SUFFIX: return "suffix";
    case HWA_UNMATCHED_SKIP: return "skip";
    case HWA_UNMATCHED_REPEAT: return "repeat";
    case HWA_UNMATCHED_REST: return "rest";
    case HWA_UNMATCHED_CADENZA: return "cadenza";
    case HWA_UNMATCHED_LOW_CONFIDENCE: return "low_confidence";
    case HWA_UNMATCHED_NO_EVIDENCE: return "no_evidence";
    default: return "unknown";
    }
}

static const char *hwa_alignment_json_channel_mode(HWAChannelMode mode)
{
    return mode == HWA_CHANNEL_KEEP ? "keep" :
           mode == HWA_CHANNEL_SELECT ? "select" :
           mode == HWA_CHANNEL_MIX ? "mix" : "unknown";
}

static int hwa_alignment_json_build(FILE *stream)
{
    if (fputs("{\"compiler_family\":", stream) == EOF ||
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
        fputc('}', stream) == EOF) {
        return -1;
    }
    return 0;
}

static int hwa_alignment_json_options(FILE *stream,
                                      const HWAAlignmentOptions *options)
{
    const HWAAnalysisOptions *analysis = &options->analysis;

    if (fputs("{\"analysis\":{\"channel_mode\":", stream) == EOF ||
        hwa_json_write_string(
            stream, hwa_alignment_json_channel_mode(analysis->channel_mode)) != 0 ||
        fprintf(stream,
                ",\"selected_channel\":%u,\"decode_block_frames\":%zu"
                ",\"frame_size\":%zu,\"hop_size\":%zu"
                ",\"silence_threshold_dbfs\":",
                (unsigned)analysis->selected_channel,
                analysis->decode_block_frames,
                analysis->frame_size,
                analysis->hop_size) < 0 ||
        hwa_alignment_json_number(stream,
                                  analysis->silence_threshold_dbfs) != 0 ||
        fprintf(stream,
                ",\"max_input_bytes\":%" PRIu64
                ",\"max_input_frames\":%" PRIu64
                ",\"max_work_bytes\":%" PRIu64
                ",\"max_transforms\":%zu,\"max_track_points\":%zu"
                ",\"max_spectrum_values\":%zu,\"max_lag_samples\":%zu"
                ",\"true_peak_oversample\":%u},\"alignment\":{"
                "\"alignment_step_seconds\":",
                analysis->max_input_bytes,
                analysis->max_input_frames,
                analysis->max_work_bytes,
                analysis->max_transforms,
                analysis->max_track_points,
                analysis->max_spectrum_values,
                analysis->max_lag_samples,
                analysis->true_peak_oversample) < 0 ||
        hwa_alignment_json_number(stream,
                                  options->alignment_step_seconds) != 0 ||
        fputs(",\"coarse_step_seconds\":", stream) == EOF ||
        hwa_alignment_json_number(stream, options->coarse_step_seconds) != 0 ||
        fputs(",\"dtw_band_seconds\":", stream) == EOF ||
        hwa_alignment_json_number(stream, options->dtw_band_seconds) != 0 ||
        fputs(",\"fine_radius_seconds\":", stream) == EOF ||
        hwa_alignment_json_number(stream, options->fine_radius_seconds) != 0 ||
        fputs(",\"refine_radius_seconds\":", stream) == EOF ||
        hwa_alignment_json_number(stream, options->refine_radius_seconds) != 0 ||
        fputs(",\"match_threshold\":", stream) == EOF ||
        hwa_alignment_json_number(stream, options->match_threshold) != 0 ||
        fputs(",\"chroma_weight\":", stream) == EOF ||
        hwa_alignment_json_number(stream, options->chroma_weight) != 0 ||
        fputs(",\"onset_weight\":", stream) == EOF ||
        hwa_alignment_json_number(stream, options->onset_weight) != 0 ||
        fputs(",\"pitch_weight\":", stream) == EOF ||
        hwa_alignment_json_number(stream, options->pitch_weight) != 0 ||
        fputs(",\"envelope_weight\":", stream) == EOF ||
        hwa_alignment_json_number(stream, options->envelope_weight) != 0 ||
        fputs(",\"activity_weight\":", stream) == EOF ||
        hwa_alignment_json_number(stream, options->activity_weight) != 0 ||
        fputs(",\"skip_cost\":", stream) == EOF ||
        hwa_alignment_json_number(stream, options->skip_cost) != 0 ||
        fputs(",\"repeat_cost\":", stream) == EOF ||
        hwa_alignment_json_number(stream, options->repeat_cost) != 0 ||
        fputs(",\"ornament_cost\":", stream) == EOF ||
        hwa_alignment_json_number(stream, options->ornament_cost) != 0 ||
        fputs(",\"rest_cost\":", stream) == EOF ||
        hwa_alignment_json_number(stream, options->rest_cost) != 0 ||
        fputs(",\"cadenza_cost\":", stream) == EOF ||
        hwa_alignment_json_number(stream, options->cadenza_cost) != 0 ||
        fprintf(stream,
                ",\"max_dtw_cells\":%" PRIu64
                ",\"max_alignment_work_bytes\":%" PRIu64
                ",\"max_alignment_points\":%zu,\"max_score_events\":%zu"
                ",\"max_manual_anchors\":%zu}}",
                options->max_dtw_cells,
                options->max_alignment_work_bytes,
                options->max_alignment_points,
                options->max_score_events,
                options->max_manual_anchors) < 0) {
        return -1;
    }
    return 0;
}

static int hwa_alignment_json_input(FILE *stream,
                                    const char *role,
                                    const char *path,
                                    const char *sha256,
                                    double duration)
{
    if (fputs("{\"role\":", stream) == EOF ||
        hwa_json_write_string(stream, role) != 0 ||
        fputs(",\"path\":", stream) == EOF ||
        hwa_json_write_string(stream, path) != 0 ||
        fputs(",\"path_encoding\":\"utf8_with_invalid_bytes_as_u00xx\","
              "\"path_bytes_hex\":", stream) == EOF ||
        hwa_json_write_byte_hex(stream, path) != 0 ||
        fputs(",\"sha256\":", stream) == EOF ||
        hwa_json_write_string(stream, sha256) != 0 ||
        fputs(",\"duration_seconds\":", stream) == EOF ||
        hwa_alignment_json_number(stream, duration) != 0 ||
        fputc('}', stream) == EOF) {
        return -1;
    }
    return 0;
}

static int hwa_alignment_json_inputs(FILE *stream,
                                     const HWAAlignment *alignment)
{
    if (fputc('[', stream) == EOF) {
        return -1;
    }
    if (alignment->mode == HWA_ALIGNMENT_AUDIO_TO_AUDIO) {
        if (hwa_alignment_json_input(stream, "reference",
                                     alignment->reference_path,
                                     alignment->reference_sha256,
                                     alignment->reference_duration_seconds) != 0 ||
            fputc(',', stream) == EOF ||
            hwa_alignment_json_input(stream, "target",
                                     alignment->target_path,
                                     alignment->target_sha256,
                                     alignment->target_duration_seconds) != 0) {
            return -1;
        }
    } else {
        if (hwa_alignment_json_input(stream, "score",
                                     alignment->score_path,
                                     alignment->score_sha256,
                                     alignment->reference_duration_seconds) != 0 ||
            fputc(',', stream) == EOF ||
            hwa_alignment_json_input(stream, "audio",
                                     alignment->target_path,
                                     alignment->target_sha256,
                                     alignment->target_duration_seconds) != 0) {
            return -1;
        }
    }
    return fputc(']', stream) == EOF ? -1 : 0;
}

static int hwa_alignment_json_anchor(FILE *stream,
                                     const HWAAlignmentAnchor *anchor)
{
    if (fprintf(stream,
                "{\"id\":%" PRIu64 ",\"reference_seconds\":",
                anchor->id) < 0 ||
        hwa_alignment_json_number(stream, anchor->reference_seconds) != 0 ||
        fputs(",\"target_seconds\":", stream) == EOF ||
        hwa_alignment_json_number(stream, anchor->target_seconds) != 0 ||
        fputs(",\"score_beat\":", stream) == EOF ||
        hwa_alignment_json_optional_number(stream, anchor->score_beat,
                                           anchor->score_beat_valid) != 0 ||
        fputs(",\"confidence\":", stream) == EOF ||
        hwa_alignment_json_number(stream, anchor->confidence) != 0 ||
        fputs(",\"origin\":", stream) == EOF ||
        hwa_json_write_string(stream,
                              hwa_alignment_json_origin(anchor->origin)) != 0 ||
        fprintf(stream,
                ",\"locked\":%s,\"evidence_flags\":%" PRIu32 "}",
                anchor->locked ? "true" : "false",
                anchor->evidence_flags) < 0) {
        return -1;
    }
    return 0;
}

static int hwa_alignment_json_match(FILE *stream,
                                    const HWAAlignmentMatch *match)
{
    if (fprintf(stream,
                "{\"id\":%" PRIu64 ",\"reference_start_seconds\":",
                match->id) < 0 ||
        hwa_alignment_json_number(stream,
                                  match->reference_start_seconds) != 0 ||
        fputs(",\"reference_end_seconds\":", stream) == EOF ||
        hwa_alignment_json_number(stream, match->reference_end_seconds) != 0 ||
        fputs(",\"target_start_seconds\":", stream) == EOF ||
        hwa_alignment_json_number(stream, match->target_start_seconds) != 0 ||
        fputs(",\"target_end_seconds\":", stream) == EOF ||
        hwa_alignment_json_number(stream, match->target_end_seconds) != 0 ||
        fputs(",\"score_start_beat\":", stream) == EOF ||
        hwa_alignment_json_optional_number(stream, match->score_start_beat,
                                           match->score_span_valid) != 0 ||
        fputs(",\"score_end_beat\":", stream) == EOF ||
        hwa_alignment_json_optional_number(stream, match->score_end_beat,
                                           match->score_span_valid) != 0 ||
        fputs(",\"confidence\":", stream) == EOF ||
        hwa_alignment_json_number(stream, match->confidence) != 0 ||
        fputs(",\"status\":", stream) == EOF ||
        hwa_json_write_string(stream,
                              hwa_alignment_json_status(match->status)) != 0 ||
        fputs(",\"event_id\":", stream) == EOF ||
        hwa_alignment_json_nullable_string(stream, match->event_id) != 0 ||
        fputs(",\"kind\":", stream) == EOF ||
        hwa_alignment_json_nullable_string(stream, match->kind) != 0 ||
        fputs(",\"voice\":", stream) == EOF ||
        hwa_alignment_json_nullable_string(stream, match->voice) != 0 ||
        fputs(",\"midi_note\":", stream) == EOF ||
        hwa_alignment_json_nullable_string(stream, match->midi_note) != 0 ||
        fputs(",\"velocity\":", stream) == EOF ||
        hwa_alignment_json_nullable_string(stream, match->velocity) != 0 ||
        fputs(",\"tie\":", stream) == EOF ||
        hwa_alignment_json_nullable_string(stream, match->tie) != 0 ||
        fputs(",\"dynamic\":", stream) == EOF ||
        hwa_alignment_json_nullable_string(stream, match->dynamic) != 0 ||
        fputs(",\"mark\":", stream) == EOF ||
        hwa_alignment_json_nullable_string(stream, match->mark) != 0 ||
        fputs(",\"score_position\":", stream) == EOF ||
        hwa_alignment_json_nullable_string(stream, match->score_position) != 0 ||
        fputs(",\"tempo_bpm\":", stream) == EOF ||
        hwa_alignment_json_optional_number(stream, match->tempo_bpm,
                                           match->tempo_valid) != 0 ||
        fprintf(stream, ",\"evidence_flags\":%" PRIu32 "}",
                match->evidence_flags) < 0) {
        return -1;
    }
    return 0;
}

static int hwa_alignment_json_unmatched(FILE *stream,
                                        const HWAUnmatchedSpan *span)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"side\":", span->id) < 0 ||
        hwa_json_write_string(stream,
                              hwa_alignment_json_side(span->side)) != 0 ||
        fputs(",\"start_seconds\":", stream) == EOF ||
        hwa_alignment_json_number(stream, span->start_seconds) != 0 ||
        fputs(",\"end_seconds\":", stream) == EOF ||
        hwa_alignment_json_number(stream, span->end_seconds) != 0 ||
        fputs(",\"start_beat\":", stream) == EOF ||
        hwa_alignment_json_optional_number(stream, span->start_beat,
                                           span->score_span_valid) != 0 ||
        fputs(",\"end_beat\":", stream) == EOF ||
        hwa_alignment_json_optional_number(stream, span->end_beat,
                                           span->score_span_valid) != 0 ||
        fputs(",\"reason\":", stream) == EOF ||
        hwa_json_write_string(stream,
                              hwa_alignment_json_reason(span->reason)) != 0 ||
        fputs(",\"confidence\":", stream) == EOF ||
        hwa_alignment_json_number(stream, span->confidence) != 0 ||
        fputc('}', stream) == EOF) {
        return -1;
    }
    return 0;
}

int hwa_report_alignment_json(FILE *stream,
                              const HWAAlignment *alignment)
{
    size_t index;

    if (stream == NULL || alignment == NULL ||
        fprintf(stream,
                "{\"schema_version\":3,\"command\":\"align\","
                "\"alignment_schema_version\":%u,\"tool_version\":",
                HWA_ALIGNMENT_FILE_SCHEMA_VERSION) < 0 ||
        hwa_json_write_string(stream, HWA_VERSION) != 0 ||
        fputs(",\"analysis_method_version\":", stream) == EOF ||
        hwa_json_write_string(stream, HWA_ANALYSIS_METHOD_VERSION) != 0 ||
        fputs(",\"method_version\":", stream) == EOF ||
        hwa_json_write_string(stream, HWA_ALIGNMENT_METHOD_VERSION) != 0 ||
        fputs(",\"build\":", stream) == EOF ||
        hwa_alignment_json_build(stream) != 0 ||
        fputs(",\"mode\":", stream) == EOF ||
        hwa_json_write_string(stream,
                              hwa_alignment_json_mode(alignment->mode)) != 0 ||
        fputs(",\"options\":", stream) == EOF ||
        hwa_alignment_json_options(stream, &alignment->options) != 0 ||
        fputs(",\"inputs\":", stream) == EOF ||
        hwa_alignment_json_inputs(stream, alignment) != 0 ||
        fputs(",\"summary\":{\"tuning_offset_cents\":", stream) == EOF ||
        hwa_alignment_json_number(stream,
                                  alignment->tuning_offset_cents) != 0 ||
        fputs(",\"tuning_confidence\":", stream) == EOF ||
        hwa_alignment_json_number(stream,
                                  alignment->tuning_confidence) != 0 ||
        fputs(",\"total_cost\":", stream) == EOF ||
        hwa_alignment_json_number(stream, alignment->total_cost) != 0 ||
        fputs(",\"normalized_cost\":", stream) == EOF ||
        hwa_alignment_json_number(stream, alignment->normalized_cost) != 0 ||
        fputs(",\"matched_coverage\":", stream) == EOF ||
        hwa_alignment_json_number(stream, alignment->matched_coverage) != 0 ||
        fputs(",\"global_confidence\":", stream) == EOF ||
        hwa_alignment_json_number(stream, alignment->global_confidence) != 0 ||
        fprintf(stream,
                ",\"dtw_cells\":%" PRIu64
                ",\"path_points\":%" PRIu64
                ",\"anchor_count\":%zu,\"match_count\":%zu"
                ",\"unmatched_span_count\":%zu,\"warning_count\":%zu},"
                "\"anchors\":[",
                alignment->dtw_cells,
                alignment->path_points,
                alignment->anchor_count,
                alignment->match_count,
                alignment->unmatched_span_count,
                alignment->warning_count) < 0) {
        return -1;
    }
    for (index = 0U; index < alignment->anchor_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_alignment_json_anchor(stream, &alignment->anchors[index]) != 0) {
            return -1;
        }
    }
    if (fputs("],\"matches\":[", stream) == EOF) {
        return -1;
    }
    for (index = 0U; index < alignment->match_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_alignment_json_match(stream, &alignment->matches[index]) != 0) {
            return -1;
        }
    }
    if (fputs("],\"unmatched_spans\":[", stream) == EOF) {
        return -1;
    }
    for (index = 0U; index < alignment->unmatched_span_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_alignment_json_unmatched(
                stream, &alignment->unmatched_spans[index]) != 0) {
            return -1;
        }
    }
    if (fputs("],\"warnings\":[", stream) == EOF) {
        return -1;
    }
    for (index = 0U; index < alignment->warning_count; ++index) {
        const HWAAlignmentWarning *warning = &alignment->warnings[index];
        if ((index != 0U && fputc(',', stream) == EOF) ||
            fprintf(stream, "{\"id\":%" PRIu64 ",\"code\":",
                    warning->id) < 0 ||
            hwa_alignment_json_nullable_string(stream, warning->code) != 0 ||
            fputs(",\"message\":", stream) == EOF ||
            hwa_alignment_json_nullable_string(stream, warning->message) != 0 ||
            fputc('}', stream) == EOF) {
            return -1;
        }
    }
    return fputs("]}", stream) == EOF ? -1 : 0;
}

static int hwa_alignment_text_path(FILE *stream, const char *path)
{
    const unsigned char *byte = (const unsigned char *)path;

    while (*byte != 0U) {
        if (*byte >= 0x20U && *byte <= 0x7eU &&
            *byte != (unsigned char)'\\') {
            if (fputc((int)*byte, stream) == EOF) {
                return -1;
            }
        } else if (*byte == (unsigned char)'\\') {
            if (fputs("\\\\", stream) == EOF) {
                return -1;
            }
        } else if (fprintf(stream, "\\x%02x", (unsigned)*byte) < 0) {
            return -1;
        }
        byte++;
    }
    return 0;
}

int hwa_report_alignment_text(FILE *stream,
                              const HWAAlignment *alignment)
{
    const char *first_label;
    const char *second_label;
    const char *first_path;
    const char *second_path;
    size_t locked_count = 0U;
    size_t index;

    if (stream == NULL || alignment == NULL) {
        return -1;
    }
    if (alignment->mode == HWA_ALIGNMENT_SCORE_TO_AUDIO) {
        first_label = "Score";
        second_label = "Audio";
        first_path = alignment->score_path;
        second_path = alignment->target_path;
    } else {
        first_label = "Reference";
        second_label = "Target";
        first_path = alignment->reference_path;
        second_path = alignment->target_path;
    }
    if (first_path == NULL || second_path == NULL ||
        fprintf(stream, "Alignment: %s\n%s: ",
                alignment->mode == HWA_ALIGNMENT_SCORE_TO_AUDIO ?
                    "score to audio" : "audio to audio",
                first_label) < 0 ||
        hwa_alignment_text_path(stream, first_path) != 0 ||
        fprintf(stream, "\n%s: ", second_label) < 0 ||
        hwa_alignment_text_path(stream, second_path) != 0) {
        return -1;
    }
    for (index = 0U; index < alignment->anchor_count; ++index) {
        if (alignment->anchors[index].locked) {
            locked_count++;
        }
    }
    if (fprintf(stream,
                "\nConfidence: %.3f\nMatched coverage: %.3f\n"
                "Normalized cost: %.6f\n"
                "Anchors: %zu (%zu locked)\nMatches: %zu\n"
                "Unmatched spans: %zu\nWarnings: %zu\n"
                "DTW cells: %" PRIu64 "\n",
                alignment->global_confidence,
                alignment->matched_coverage,
                alignment->normalized_cost,
                alignment->anchor_count,
                locked_count,
                alignment->match_count,
                alignment->unmatched_span_count,
                alignment->warning_count,
                alignment->dtw_cells) < 0) {
        return -1;
    }
    if (alignment->warning_count != 0U) {
        for (index = 0U; index < alignment->warning_count; ++index) {
            const HWAAlignmentWarning *warning = &alignment->warnings[index];
            if (fprintf(stream, "Warning %zu: ", index + 1U) < 0 ||
                hwa_alignment_text_path(
                    stream, warning->code != NULL ? warning->code : "") != 0 ||
                fputs(": ", stream) == EOF ||
                hwa_alignment_text_path(
                    stream, warning->message != NULL ? warning->message : "") != 0 ||
                fputc('\n', stream) == EOF) {
                return -1;
            }
        }
    }
    return 0;
}

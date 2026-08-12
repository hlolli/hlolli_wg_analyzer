#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "alignment_file.h"

#include "internal.h"

#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <sys/stat.h>
#include "windows_file_identity.h"
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define HWA_ALIGNMENT_FILE_MAX_FIELDS 21U

#define HWA_STRINGIFY_INNER(value) #value
#define HWA_STRINGIFY(value) HWA_STRINGIFY_INNER(value)

const char *hwa_build_compiler_family(void)
{
#if defined(__clang__)
    return "clang";
#elif defined(_MSC_VER)
    return "msvc";
#elif defined(__GNUC__)
    return "gcc";
#else
    return "unknown";
#endif
}

const char *hwa_build_compiler_version(void)
{
#if defined(__clang__)
    return __clang_version__;
#elif defined(_MSC_FULL_VER)
    return HWA_STRINGIFY(_MSC_FULL_VER);
#elif defined(_MSC_VER)
    return HWA_STRINGIFY(_MSC_VER);
#elif defined(__VERSION__)
    return __VERSION__;
#else
    return "unknown";
#endif
}

const char *hwa_build_c_standard(void)
{
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
    return "c23";
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201710L
    return "c17";
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    return "c11";
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
    return "c99";
#else
    return "pre-c99";
#endif
}

const char *hwa_build_target_os(void)
{
#if defined(__EMSCRIPTEN__)
    return "wasm";
#elif defined(__wasi__)
    return "wasi";
#elif defined(_WIN32)
    return "windows";
#elif defined(__APPLE__) && defined(__MACH__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#elif defined(__FreeBSD__)
    return "freebsd";
#elif defined(__unix__)
    return "unix";
#else
    return "unknown";
#endif
}

const char *hwa_build_endianness(void)
{
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return "little";
#elif defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
      __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return "big";
#elif defined(_WIN32)
    return "little";
#else
    const uint16_t marker = UINT16_C(0x0102);
    const unsigned char *bytes = (const unsigned char *)&marker;
    if (bytes[0] == 0x02U && bytes[1] == 0x01U) {
        return "little";
    }
    if (bytes[0] == 0x01U && bytes[1] == 0x02U) {
        return "big";
    }
    return "mixed";
#endif
}

const char *hwa_build_mode(void)
{
#if defined(NDEBUG)
    return "release";
#else
    return "debug";
#endif
}

unsigned hwa_build_pointer_bits(void)
{
    return (unsigned)(sizeof(void *) * (size_t)CHAR_BIT);
}

static int hwa_alignment_csv_field(FILE *stream, const char *text)
{
    const unsigned char *byte = (const unsigned char *)text;
    int quote = 0;

    if (text == NULL) {
        text = "";
        byte = (const unsigned char *)text;
    }
    while (*byte != 0U) {
        if (*byte == (unsigned char)',' || *byte == (unsigned char)'\"' ||
            *byte == (unsigned char)'\r' || *byte == (unsigned char)'\n') {
            quote = 1;
            break;
        }
        byte++;
    }
    if (!quote) {
        return fputs(text, stream) == EOF ? -1 : 0;
    }
    if (fputc('\"', stream) == EOF) {
        return -1;
    }
    byte = (const unsigned char *)text;
    while (*byte != 0U) {
        if (*byte == (unsigned char)'\"' && fputc('\"', stream) == EOF) {
            return -1;
        }
        if (fputc((int)*byte, stream) == EOF) {
            return -1;
        }
        byte++;
    }
    return fputc('\"', stream) == EOF ? -1 : 0;
}

static int hwa_alignment_csv_number(FILE *stream, double value)
{
    if (!isfinite(value)) {
        return -1;
    }
    return fprintf(stream, "%.17g", value == 0.0 ? 0.0 : value) < 0 ? -1 : 0;
}

static int hwa_alignment_csv_optional_number(FILE *stream,
                                             double value,
                                             int valid)
{
    return valid ? hwa_alignment_csv_number(stream, value) : 0;
}

static const char *hwa_alignment_mode_text(HWAAlignmentMode mode)
{
    return mode == HWA_ALIGNMENT_SCORE_TO_AUDIO ? "score-audio" :
           mode == HWA_ALIGNMENT_AUDIO_TO_AUDIO ? "audio-audio" : NULL;
}

static const char *hwa_alignment_origin_text(HWAAlignmentOrigin origin)
{
    return origin == HWA_ALIGNMENT_ORIGIN_AUTO ? "auto" :
           origin == HWA_ALIGNMENT_ORIGIN_MANUAL ? "manual" : NULL;
}

static const char *hwa_alignment_side_text(HWAAlignmentSide side)
{
    return side == HWA_ALIGNMENT_REFERENCE ? "reference" :
           side == HWA_ALIGNMENT_TARGET ? "target" : NULL;
}

static const char *hwa_alignment_status_text(HWAAlignmentStatus status)
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

static const char *hwa_alignment_reason_text(HWAUnmatchedReason reason)
{
    switch (reason) {
    case HWA_UNMATCHED_PREFIX: return "prefix";
    case HWA_UNMATCHED_SUFFIX: return "suffix";
    case HWA_UNMATCHED_SKIP: return "skip";
    case HWA_UNMATCHED_REPEAT: return "repeat";
    case HWA_UNMATCHED_REST: return "rest";
    case HWA_UNMATCHED_CADENZA: return "cadenza";
    case HWA_UNMATCHED_LOW_CONFIDENCE: return "low-confidence";
    case HWA_UNMATCHED_NO_EVIDENCE: return "no-evidence";
    default: return NULL;
    }
}

static const char *hwa_alignment_channel_mode_text(HWAChannelMode mode)
{
    return mode == HWA_CHANNEL_KEEP ? "keep" :
           mode == HWA_CHANNEL_SELECT ? "select" :
           mode == HWA_CHANNEL_MIX ? "mix" : NULL;
}

static int hwa_alignment_write_meta_text(FILE *stream,
                                         const char *key,
                                         const char *value,
                                         const char *unit)
{
    return fputs("META,", stream) == EOF ||
           hwa_alignment_csv_field(stream, key) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_alignment_csv_field(stream, value) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_alignment_csv_field(stream, unit) != 0 ||
           fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_alignment_write_meta_number(FILE *stream,
                                           const char *key,
                                           double value,
                                           const char *unit)
{
    return fputs("META,", stream) == EOF ||
           hwa_alignment_csv_field(stream, key) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_alignment_csv_number(stream, value) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_alignment_csv_field(stream, unit) != 0 ||
           fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_alignment_write_meta_u64(FILE *stream,
                                        const char *key,
                                        uint64_t value,
                                        const char *unit)
{
    return fprintf(stream, "META,%s,%" PRIu64 ",%s\r\n",
                   key, value, unit) < 0 ? -1 : 0;
}

static int hwa_alignment_write_meta_size(FILE *stream,
                                         const char *key,
                                         size_t value,
                                         const char *unit)
{
    return fprintf(stream, "META,%s,%zu,%s\r\n",
                   key, value, unit) < 0 ? -1 : 0;
}

static int hwa_alignment_write_path_hex(FILE *stream, const char *path)
{
    const unsigned char *byte = (const unsigned char *)path;

    while (*byte != 0U) {
        if (fprintf(stream, "%02x", (unsigned)*byte) < 0) {
            return -1;
        }
        byte++;
    }
    return 0;
}

static int hwa_alignment_valid_sha256(const char *text)
{
    size_t index;

    if (text == NULL || strlen(text) != 64U) {
        return 0;
    }
    for (index = 0U; index < 64U; ++index) {
        if (!((text[index] >= '0' && text[index] <= '9') ||
              (text[index] >= 'a' && text[index] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static int hwa_alignment_write_input(FILE *stream,
                                     const char *role,
                                     const char *path,
                                     const char *sha256,
                                     double duration)
{
    if (role == NULL || path == NULL || !hwa_alignment_valid_sha256(sha256) ||
        !isfinite(duration) || duration < 0.0 ||
        fputs("INPUT,", stream) == EOF ||
        hwa_alignment_csv_field(stream, role) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_write_path_hex(stream, path) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_field(stream, sha256) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_number(stream, duration) != 0 ||
        fputs("\r\n", stream) == EOF) {
        return -1;
    }
    return 0;
}

static int hwa_alignment_anchor_pointer_compare(const void *left,
                                                const void *right)
{
    const HWAAlignmentAnchor *const *a =
        (const HWAAlignmentAnchor *const *)left;
    const HWAAlignmentAnchor *const *b =
        (const HWAAlignmentAnchor *const *)right;
    if ((*a)->reference_seconds < (*b)->reference_seconds) return -1;
    if ((*a)->reference_seconds > (*b)->reference_seconds) return 1;
    if ((*a)->target_seconds < (*b)->target_seconds) return -1;
    if ((*a)->target_seconds > (*b)->target_seconds) return 1;
    return (*a)->id < (*b)->id ? -1 : (*a)->id > (*b)->id ? 1 : 0;
}

static int hwa_alignment_match_pointer_compare(const void *left,
                                               const void *right)
{
    const HWAAlignmentMatch *const *a =
        (const HWAAlignmentMatch *const *)left;
    const HWAAlignmentMatch *const *b =
        (const HWAAlignmentMatch *const *)right;
    if ((*a)->reference_start_seconds < (*b)->reference_start_seconds) return -1;
    if ((*a)->reference_start_seconds > (*b)->reference_start_seconds) return 1;
    return (*a)->id < (*b)->id ? -1 : (*a)->id > (*b)->id ? 1 : 0;
}

static int hwa_alignment_span_pointer_compare(const void *left,
                                              const void *right)
{
    const HWAUnmatchedSpan *const *a =
        (const HWAUnmatchedSpan *const *)left;
    const HWAUnmatchedSpan *const *b =
        (const HWAUnmatchedSpan *const *)right;
    if ((*a)->side < (*b)->side) return -1;
    if ((*a)->side > (*b)->side) return 1;
    if ((*a)->start_seconds < (*b)->start_seconds) return -1;
    if ((*a)->start_seconds > (*b)->start_seconds) return 1;
    return (*a)->id < (*b)->id ? -1 : (*a)->id > (*b)->id ? 1 : 0;
}

static int hwa_alignment_warning_pointer_compare(const void *left,
                                                 const void *right)
{
    const HWAAlignmentWarning *const *a =
        (const HWAAlignmentWarning *const *)left;
    const HWAAlignmentWarning *const *b =
        (const HWAAlignmentWarning *const *)right;
    return (*a)->id < (*b)->id ? -1 : (*a)->id > (*b)->id ? 1 : 0;
}

static int hwa_alignment_write_meta(FILE *stream,
                                    const HWAAlignment *alignment)
{
    const HWAAlignmentOptions *option = &alignment->options;
    const HWAAnalysisOptions *analysis = &option->analysis;
    const char *mode = hwa_alignment_mode_text(alignment->mode);
    const char *channel_mode = hwa_alignment_channel_mode_text(
        analysis->channel_mode);

    if (mode == NULL || channel_mode == NULL ||
        !isfinite(alignment->reference_duration_seconds) ||
        alignment->reference_duration_seconds < 0.0 ||
        !isfinite(alignment->target_duration_seconds) ||
        alignment->target_duration_seconds < 0.0 ||
        !isfinite(alignment->tuning_offset_cents) ||
        !isfinite(alignment->tuning_confidence) ||
        alignment->tuning_confidence < 0.0 ||
        alignment->tuning_confidence > 1.0 ||
        !isfinite(alignment->total_cost) || alignment->total_cost < 0.0 ||
        !isfinite(alignment->normalized_cost) ||
        alignment->normalized_cost < 0.0 ||
        !isfinite(alignment->matched_coverage) ||
        alignment->matched_coverage < 0.0 ||
        alignment->matched_coverage > 1.0 ||
        !isfinite(alignment->global_confidence) ||
        alignment->global_confidence < 0.0 ||
        alignment->global_confidence > 1.0 ||
        !isfinite(option->match_threshold) || option->match_threshold < 0.0 ||
        option->match_threshold > 1.0 ||
        !isfinite(option->chroma_weight) || option->chroma_weight < 0.0 ||
        !isfinite(option->onset_weight) || option->onset_weight < 0.0 ||
        !isfinite(option->pitch_weight) || option->pitch_weight < 0.0 ||
        !isfinite(option->envelope_weight) || option->envelope_weight < 0.0 ||
        !isfinite(option->activity_weight) || option->activity_weight < 0.0 ||
        !isfinite(option->skip_cost) || option->skip_cost < 0.0 ||
        !isfinite(option->repeat_cost) || option->repeat_cost < 0.0 ||
        !isfinite(option->ornament_cost) || option->ornament_cost < 0.0 ||
        !isfinite(option->rest_cost) || option->rest_cost < 0.0 ||
        !isfinite(option->cadenza_cost) || option->cadenza_cost < 0.0 ||
        hwa_alignment_write_meta_text(stream, "tool_version",
                                      HWA_VERSION, "") != 0 ||
        hwa_alignment_write_meta_text(stream, "analysis_method_version",
                                      HWA_ANALYSIS_METHOD_VERSION, "") != 0 ||
        hwa_alignment_write_meta_text(stream, "alignment_method_version",
                                      HWA_ALIGNMENT_METHOD_VERSION, "") != 0 ||
        hwa_alignment_write_meta_text(stream, "build_compiler_family",
                                      hwa_build_compiler_family(), "") != 0 ||
        hwa_alignment_write_meta_text(stream, "build_compiler_version",
                                      hwa_build_compiler_version(), "") != 0 ||
        hwa_alignment_write_meta_text(stream, "build_c_standard",
                                      hwa_build_c_standard(), "") != 0 ||
        hwa_alignment_write_meta_text(stream, "build_target_os",
                                      hwa_build_target_os(), "") != 0 ||
        hwa_alignment_write_meta_u64(stream, "build_pointer_bits",
                                     hwa_build_pointer_bits(), "bits") != 0 ||
        hwa_alignment_write_meta_text(stream, "build_endianness",
                                      hwa_build_endianness(), "") != 0 ||
        hwa_alignment_write_meta_text(stream, "build_mode",
                                      hwa_build_mode(), "") != 0 ||
        hwa_alignment_write_meta_text(stream, "mode", mode, "") != 0 ||
        hwa_alignment_write_meta_number(stream, "reference_duration_seconds",
                                        alignment->reference_duration_seconds,
                                        "seconds") != 0 ||
        hwa_alignment_write_meta_number(stream, "target_duration_seconds",
                                        alignment->target_duration_seconds,
                                        "seconds") != 0 ||
        hwa_alignment_write_meta_number(stream, "tuning_offset_cents",
                                        alignment->tuning_offset_cents,
                                        "cents") != 0 ||
        hwa_alignment_write_meta_number(stream, "tuning_confidence",
                                        alignment->tuning_confidence,
                                        "ratio") != 0 ||
        hwa_alignment_write_meta_number(stream, "total_cost",
                                        alignment->total_cost, "cost") != 0 ||
        hwa_alignment_write_meta_number(stream, "normalized_cost",
                                        alignment->normalized_cost,
                                        "cost_per_path_point") != 0 ||
        hwa_alignment_write_meta_number(stream, "matched_coverage",
                                        alignment->matched_coverage,
                                        "ratio") != 0 ||
        hwa_alignment_write_meta_number(stream, "global_confidence",
                                        alignment->global_confidence,
                                        "ratio") != 0 ||
        hwa_alignment_write_meta_u64(stream, "dtw_cells",
                                     alignment->dtw_cells, "cells") != 0 ||
        hwa_alignment_write_meta_u64(stream, "path_points",
                                     alignment->path_points, "points") != 0 ||
        hwa_alignment_write_meta_text(stream, "channel_mode",
                                      channel_mode, "") != 0 ||
        hwa_alignment_write_meta_u64(stream, "selected_channel",
                                     analysis->selected_channel, "channel") != 0 ||
        hwa_alignment_write_meta_size(stream, "decode_block_frames",
                                      analysis->decode_block_frames, "frames") != 0 ||
        hwa_alignment_write_meta_size(stream, "frame_size",
                                      analysis->frame_size, "samples") != 0 ||
        hwa_alignment_write_meta_size(stream, "hop_size",
                                      analysis->hop_size, "samples") != 0 ||
        hwa_alignment_write_meta_number(stream, "silence_threshold_dbfs",
                                        analysis->silence_threshold_dbfs,
                                        "dBFS") != 0 ||
        hwa_alignment_write_meta_u64(stream, "max_input_bytes",
                                     analysis->max_input_bytes, "bytes") != 0 ||
        hwa_alignment_write_meta_u64(stream, "max_input_frames",
                                     analysis->max_input_frames, "frames") != 0 ||
        hwa_alignment_write_meta_u64(stream, "max_analysis_work_bytes",
                                     analysis->max_work_bytes, "bytes") != 0 ||
        hwa_alignment_write_meta_size(stream, "max_transforms",
                                      analysis->max_transforms, "transforms") != 0 ||
        hwa_alignment_write_meta_size(stream, "max_track_points",
                                      analysis->max_track_points, "points") != 0 ||
        hwa_alignment_write_meta_size(stream, "max_spectrum_values",
                                      analysis->max_spectrum_values, "values") != 0 ||
        hwa_alignment_write_meta_size(stream, "max_lag_samples",
                                      analysis->max_lag_samples, "samples") != 0 ||
        hwa_alignment_write_meta_u64(stream, "true_peak_oversample",
                                     analysis->true_peak_oversample, "ratio") != 0 ||
        hwa_alignment_write_meta_number(stream, "alignment_step_seconds",
                                        option->alignment_step_seconds,
                                        "seconds") != 0 ||
        hwa_alignment_write_meta_number(stream, "coarse_step_seconds",
                                        option->coarse_step_seconds,
                                        "seconds") != 0 ||
        hwa_alignment_write_meta_number(stream, "dtw_band_seconds",
                                        option->dtw_band_seconds,
                                        "seconds") != 0 ||
        hwa_alignment_write_meta_number(stream, "fine_radius_seconds",
                                        option->fine_radius_seconds,
                                        "seconds") != 0 ||
        hwa_alignment_write_meta_number(stream, "refine_radius_seconds",
                                        option->refine_radius_seconds,
                                        "seconds") != 0 ||
        hwa_alignment_write_meta_number(stream, "match_threshold",
                                        option->match_threshold, "ratio") != 0 ||
        hwa_alignment_write_meta_number(stream, "chroma_weight",
                                        option->chroma_weight, "weight") != 0 ||
        hwa_alignment_write_meta_number(stream, "onset_weight",
                                        option->onset_weight, "weight") != 0 ||
        hwa_alignment_write_meta_number(stream, "pitch_weight",
                                        option->pitch_weight, "weight") != 0 ||
        hwa_alignment_write_meta_number(stream, "envelope_weight",
                                        option->envelope_weight, "weight") != 0 ||
        hwa_alignment_write_meta_number(stream, "activity_weight",
                                        option->activity_weight, "weight") != 0 ||
        hwa_alignment_write_meta_number(stream, "skip_cost",
                                        option->skip_cost, "cost") != 0 ||
        hwa_alignment_write_meta_number(stream, "repeat_cost",
                                        option->repeat_cost, "cost") != 0 ||
        hwa_alignment_write_meta_number(stream, "ornament_cost",
                                        option->ornament_cost, "cost") != 0 ||
        hwa_alignment_write_meta_number(stream, "rest_cost",
                                        option->rest_cost, "cost") != 0 ||
        hwa_alignment_write_meta_number(stream, "cadenza_cost",
                                        option->cadenza_cost, "cost") != 0 ||
        hwa_alignment_write_meta_u64(stream, "max_dtw_cells",
                                     option->max_dtw_cells, "cells") != 0 ||
        hwa_alignment_write_meta_u64(stream, "max_alignment_work_bytes",
                                     option->max_alignment_work_bytes,
                                     "bytes") != 0 ||
        hwa_alignment_write_meta_size(stream, "max_alignment_points",
                                      option->max_alignment_points, "points") != 0 ||
        hwa_alignment_write_meta_size(stream, "max_score_events",
                                      option->max_score_events, "events") != 0 ||
        hwa_alignment_write_meta_size(stream, "max_manual_anchors",
                                      option->max_manual_anchors, "anchors") != 0) {
        return -1;
    }
    return 0;
}

static int hwa_alignment_write_anchor(FILE *stream,
                                      const HWAAlignmentAnchor *anchor)
{
    const char *origin = hwa_alignment_origin_text(anchor->origin);

    if (origin == NULL || !isfinite(anchor->reference_seconds) ||
        !isfinite(anchor->target_seconds) || anchor->reference_seconds < 0.0 ||
        anchor->target_seconds < 0.0 || !isfinite(anchor->confidence) ||
        anchor->confidence < 0.0 || anchor->confidence > 1.0 ||
        (anchor->score_beat_valid &&
         (!isfinite(anchor->score_beat) || anchor->score_beat < 0.0)) ||
        fprintf(stream, "ANCHOR,%" PRIu64 ",", anchor->id) < 0 ||
        hwa_alignment_csv_number(stream, anchor->reference_seconds) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_number(stream, anchor->target_seconds) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_optional_number(stream, anchor->score_beat,
                                          anchor->score_beat_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_number(stream, anchor->confidence) != 0 ||
        fputc(',', stream) == EOF || fputs(origin, stream) == EOF ||
        fprintf(stream, ",%d,%" PRIu32 "\r\n",
                anchor->locked ? 1 : 0, anchor->evidence_flags) < 0) {
        return -1;
    }
    return 0;
}

static int hwa_alignment_write_match(FILE *stream,
                                     const HWAAlignmentMatch *match)
{
    const char *status = hwa_alignment_status_text(match->status);

    if (status == NULL || !isfinite(match->reference_start_seconds) ||
        !isfinite(match->reference_end_seconds) ||
        !isfinite(match->target_start_seconds) ||
        !isfinite(match->target_end_seconds) ||
        match->reference_start_seconds < 0.0 ||
        match->reference_end_seconds < match->reference_start_seconds ||
        match->target_start_seconds < 0.0 ||
        match->target_end_seconds < match->target_start_seconds ||
        !isfinite(match->confidence) || match->confidence < 0.0 ||
        match->confidence > 1.0 ||
        (match->score_span_valid &&
         (!isfinite(match->score_start_beat) ||
          !isfinite(match->score_end_beat) || match->score_start_beat < 0.0 ||
          match->score_end_beat < match->score_start_beat)) ||
        (match->tempo_valid &&
         (!isfinite(match->tempo_bpm) || match->tempo_bpm <= 0.0)) ||
        fprintf(stream, "MATCH,%" PRIu64 ",", match->id) < 0 ||
        hwa_alignment_csv_number(stream, match->reference_start_seconds) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_number(stream, match->reference_end_seconds) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_number(stream, match->target_start_seconds) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_number(stream, match->target_end_seconds) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_optional_number(stream, match->score_start_beat,
                                          match->score_span_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_optional_number(stream, match->score_end_beat,
                                          match->score_span_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_number(stream, match->confidence) != 0 ||
        fputc(',', stream) == EOF || fputs(status, stream) == EOF ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_field(stream, match->event_id) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_field(stream, match->kind) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_field(stream, match->voice) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_field(stream, match->midi_note) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_field(stream, match->velocity) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_field(stream, match->tie) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_field(stream, match->dynamic) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_field(stream, match->mark) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_field(stream, match->score_position) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_optional_number(stream, match->tempo_bpm,
                                          match->tempo_valid) != 0 ||
        fprintf(stream, ",%" PRIu32 "\r\n", match->evidence_flags) < 0) {
        return -1;
    }
    return 0;
}

static int hwa_alignment_write_span(FILE *stream,
                                    const HWAUnmatchedSpan *span)
{
    const char *side = hwa_alignment_side_text(span->side);
    const char *reason = hwa_alignment_reason_text(span->reason);

    if (side == NULL || reason == NULL || !isfinite(span->start_seconds) ||
        !isfinite(span->end_seconds) || span->start_seconds < 0.0 ||
        span->end_seconds < span->start_seconds ||
        !isfinite(span->confidence) || span->confidence < 0.0 ||
        span->confidence > 1.0 ||
        (span->score_span_valid &&
         (!isfinite(span->start_beat) || !isfinite(span->end_beat) ||
          span->start_beat < 0.0 || span->end_beat < span->start_beat)) ||
        fprintf(stream, "UNMATCHED,%" PRIu64 ",%s,", span->id, side) < 0 ||
        hwa_alignment_csv_number(stream, span->start_seconds) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_number(stream, span->end_seconds) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_optional_number(stream, span->start_beat,
                                          span->score_span_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_optional_number(stream, span->end_beat,
                                          span->score_span_valid) != 0 ||
        fputc(',', stream) == EOF || fputs(reason, stream) == EOF ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_number(stream, span->confidence) != 0 ||
        fputs("\r\n", stream) == EOF) {
        return -1;
    }
    return 0;
}

static int hwa_alignment_write_warning(FILE *stream,
                                       const HWAAlignmentWarning *warning)
{
    if (warning->code == NULL || warning->code[0] == '\0' ||
        warning->message == NULL ||
        fprintf(stream, "WARNING,%" PRIu64 ",", warning->id) < 0 ||
        hwa_alignment_csv_field(stream, warning->code) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_alignment_csv_field(stream, warning->message) != 0 ||
        fputs("\r\n", stream) == EOF) {
        return -1;
    }
    return 0;
}

static void **hwa_alignment_sorted_pointers(const void *base,
                                            size_t count,
                                            size_t item_size,
                                            int (*compare)(const void *,
                                                           const void *))
{
    void **pointers;
    const unsigned char *bytes = (const unsigned char *)base;
    size_t index;

    if (count == 0U) {
        return NULL;
    }
    if (base == NULL || count > SIZE_MAX / sizeof(*pointers) ||
        (item_size != 0U && count > SIZE_MAX / item_size)) {
        return NULL;
    }
    pointers = (void **)malloc(count * sizeof(*pointers));
    if (pointers == NULL) {
        return NULL;
    }
    for (index = 0U; index < count; ++index) {
        pointers[index] = (void *)(bytes + index * item_size);
    }
    qsort(pointers, count, sizeof(*pointers), compare);
    return pointers;
}

int hwa_alignment_file_write(FILE *stream,
                             const HWAAlignment *alignment,
                             char *error,
                             size_t error_size)
{
    void **anchors = NULL;
    void **matches = NULL;
    void **spans = NULL;
    void **warnings = NULL;
    size_t index;
    uint64_t pointer_count;
    uint64_t pointer_bytes;
    int result = -1;

    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (stream == NULL || alignment == NULL ||
        hwa_alignment_mode_text(alignment->mode) == NULL) {
        hwa_set_error(error, error_size,
                      "invalid alignment-file arguments");
        return -1;
    }
    pointer_count = (uint64_t)alignment->anchor_count;
    if ((uint64_t)alignment->match_count > UINT64_MAX - pointer_count) {
        hwa_set_error(error, error_size,
                      "canonical output pointer count overflows");
        return -1;
    }
    pointer_count += (uint64_t)alignment->match_count;
    if ((uint64_t)alignment->unmatched_span_count >
        UINT64_MAX - pointer_count) {
        hwa_set_error(error, error_size,
                      "canonical output pointer count overflows");
        return -1;
    }
    pointer_count += (uint64_t)alignment->unmatched_span_count;
    if ((uint64_t)alignment->warning_count > UINT64_MAX - pointer_count ||
        pointer_count + (uint64_t)alignment->warning_count >
            UINT64_MAX / (uint64_t)sizeof(void *)) {
        hwa_set_error(error, error_size,
                      "canonical output pointer storage overflows");
        return -1;
    }
    pointer_count += (uint64_t)alignment->warning_count;
    pointer_bytes = pointer_count * (uint64_t)sizeof(void *);
    if (alignment->options.max_alignment_work_bytes != 0U &&
        pointer_bytes > alignment->options.max_alignment_work_bytes) {
        hwa_set_error(error, error_size,
                      "canonical output exceeds the alignment work-byte limit");
        return -1;
    }
    if ((alignment->anchor_count != 0U && alignment->anchors == NULL) ||
        (alignment->match_count != 0U && alignment->matches == NULL) ||
        (alignment->unmatched_span_count != 0U &&
         alignment->unmatched_spans == NULL) ||
        (alignment->warning_count != 0U && alignment->warnings == NULL)) {
        hwa_set_error(error, error_size,
                      "alignment result has a missing result array");
        return -1;
    }
    anchors = hwa_alignment_sorted_pointers(
        alignment->anchors, alignment->anchor_count,
        sizeof(*alignment->anchors), hwa_alignment_anchor_pointer_compare);
    matches = hwa_alignment_sorted_pointers(
        alignment->matches, alignment->match_count,
        sizeof(*alignment->matches), hwa_alignment_match_pointer_compare);
    spans = hwa_alignment_sorted_pointers(
        alignment->unmatched_spans, alignment->unmatched_span_count,
        sizeof(*alignment->unmatched_spans), hwa_alignment_span_pointer_compare);
    warnings = hwa_alignment_sorted_pointers(
        alignment->warnings, alignment->warning_count,
        sizeof(*alignment->warnings), hwa_alignment_warning_pointer_compare);
    if ((alignment->anchor_count != 0U && anchors == NULL) ||
        (alignment->match_count != 0U && matches == NULL) ||
        (alignment->unmatched_span_count != 0U && spans == NULL) ||
        (alignment->warning_count != 0U && warnings == NULL)) {
        hwa_set_error(error, error_size,
                      "out of memory for canonical alignment output");
        goto cleanup;
    }
    if (fputs("HWA_ALIGNMENT,1\r\n", stream) == EOF ||
        hwa_alignment_write_meta(stream, alignment) != 0) {
        hwa_set_error(error, error_size,
                      "cannot write alignment metadata");
        goto cleanup;
    }
    if (alignment->mode == HWA_ALIGNMENT_AUDIO_TO_AUDIO) {
        if (hwa_alignment_write_input(stream, "reference",
                                      alignment->reference_path,
                                      alignment->reference_sha256,
                                      alignment->reference_duration_seconds) != 0 ||
            hwa_alignment_write_input(stream, "target",
                                      alignment->target_path,
                                      alignment->target_sha256,
                                      alignment->target_duration_seconds) != 0) {
            hwa_set_error(error, error_size,
                          "cannot write alignment inputs");
            goto cleanup;
        }
    } else if (hwa_alignment_write_input(stream, "score",
                                         alignment->score_path,
                                         alignment->score_sha256,
                                         alignment->reference_duration_seconds) != 0 ||
               hwa_alignment_write_input(stream, "audio",
                                         alignment->target_path,
                                         alignment->target_sha256,
                                         alignment->target_duration_seconds) != 0) {
        hwa_set_error(error, error_size,
                      "cannot write alignment inputs");
        goto cleanup;
    }
    for (index = 0U; index < alignment->anchor_count; ++index) {
        const HWAAlignmentAnchor *anchor =
            (const HWAAlignmentAnchor *)anchors[index];
        if ((alignment->mode == HWA_ALIGNMENT_SCORE_TO_AUDIO) !=
                (anchor->score_beat_valid != 0) ||
            (index != 0U &&
             (anchor->reference_seconds <=
                  ((const HWAAlignmentAnchor *)anchors[index - 1U])
                      ->reference_seconds ||
              anchor->target_seconds <=
                  ((const HWAAlignmentAnchor *)anchors[index - 1U])
                      ->target_seconds)) ||
            hwa_alignment_write_anchor(stream, anchor) != 0) {
            hwa_set_error(error, error_size,
                          "alignment anchors are not a strict finite map");
            goto cleanup;
        }
    }
    for (index = 0U; index < alignment->match_count; ++index) {
        const HWAAlignmentMatch *match =
            (const HWAAlignmentMatch *)matches[index];
        int score_mode = alignment->mode == HWA_ALIGNMENT_SCORE_TO_AUDIO;

        if ((score_mode &&
             (!match->score_span_valid || !match->tempo_valid ||
              match->event_id == NULL || match->event_id[0] == '\0' ||
              match->kind == NULL || match->kind[0] == '\0')) ||
            (!score_mode && (match->score_span_valid || match->tempo_valid)) ||
            match->reference_end_seconds >
                alignment->reference_duration_seconds ||
            match->target_end_seconds > alignment->target_duration_seconds ||
            hwa_alignment_write_match(stream, match) != 0) {
            hwa_set_error(error, error_size,
                          "cannot write alignment match %zu", index);
            goto cleanup;
        }
    }
    for (index = 0U; index < alignment->unmatched_span_count; ++index) {
        const HWAUnmatchedSpan *span =
            (const HWAUnmatchedSpan *)spans[index];
        int score_span_required =
            alignment->mode == HWA_ALIGNMENT_SCORE_TO_AUDIO &&
            span->side == HWA_ALIGNMENT_REFERENCE;
        double duration = span->side == HWA_ALIGNMENT_REFERENCE ?
                              alignment->reference_duration_seconds :
                              alignment->target_duration_seconds;
        if (score_span_required != (span->score_span_valid != 0) ||
            span->end_seconds > duration ||
            hwa_alignment_write_span(stream, span) != 0) {
            hwa_set_error(error, error_size,
                          "cannot write unmatched span %zu", index);
            goto cleanup;
        }
    }
    for (index = 0U; index < alignment->warning_count; ++index) {
        if (hwa_alignment_write_warning(
                stream, (const HWAAlignmentWarning *)warnings[index]) != 0) {
            hwa_set_error(error, error_size,
                          "cannot write alignment warning %zu", index);
            goto cleanup;
        }
    }
    result = 0;

cleanup:
    free(warnings);
    free(spans);
    free(matches);
    free(anchors);
    return result;
}

static const char *const hwa_alignment_meta_keys[] = {
    "tool_version",
    "analysis_method_version",
    "alignment_method_version",
    "build_compiler_family",
    "build_compiler_version",
    "build_c_standard",
    "build_target_os",
    "build_pointer_bits",
    "build_endianness",
    "build_mode",
    "mode",
    "reference_duration_seconds",
    "target_duration_seconds",
    "tuning_offset_cents",
    "tuning_confidence",
    "total_cost",
    "normalized_cost",
    "matched_coverage",
    "global_confidence",
    "dtw_cells",
    "path_points",
    "channel_mode",
    "selected_channel",
    "decode_block_frames",
    "frame_size",
    "hop_size",
    "silence_threshold_dbfs",
    "max_input_bytes",
    "max_input_frames",
    "max_analysis_work_bytes",
    "max_transforms",
    "max_track_points",
    "max_spectrum_values",
    "max_lag_samples",
    "true_peak_oversample",
    "alignment_step_seconds",
    "coarse_step_seconds",
    "dtw_band_seconds",
    "fine_radius_seconds",
    "refine_radius_seconds",
    "match_threshold",
    "chroma_weight",
    "onset_weight",
    "pitch_weight",
    "envelope_weight",
    "activity_weight",
    "skip_cost",
    "repeat_cost",
    "ornament_cost",
    "rest_cost",
    "cadenza_cost",
    "max_dtw_cells",
    "max_alignment_work_bytes",
    "max_alignment_points",
    "max_score_events",
    "max_manual_anchors"
};

static const char *const hwa_alignment_meta_units[] = {
    "", "", "", "", "", "", "", "bits", "", "", "", "seconds",
    "seconds", "cents", "ratio", "cost",
    "cost_per_path_point", "ratio", "ratio", "cells", "points", "",
    "channel", "frames", "samples", "samples", "dBFS", "bytes", "frames",
    "bytes", "transforms", "points", "values", "samples", "ratio",
    "seconds", "seconds", "seconds", "seconds", "seconds", "ratio",
    "weight", "weight", "weight", "weight", "weight", "cost", "cost",
    "cost", "cost", "cost", "cells", "bytes", "points", "events",
    "anchors"
};

#define HWA_ALIGNMENT_META_COUNT \
    (sizeof(hwa_alignment_meta_keys) / sizeof(hwa_alignment_meta_keys[0]))

_Static_assert(HWA_ALIGNMENT_META_COUNT ==
                   sizeof(hwa_alignment_meta_units) /
                       sizeof(hwa_alignment_meta_units[0]),
               "alignment META key and unit tables must match");

typedef enum HWAAlignmentFileSection {
    HWA_ALIGNMENT_SECTION_HEADER = 0,
    HWA_ALIGNMENT_SECTION_META = 1,
    HWA_ALIGNMENT_SECTION_INPUT = 2,
    HWA_ALIGNMENT_SECTION_ANCHOR = 3,
    HWA_ALIGNMENT_SECTION_MATCH = 4,
    HWA_ALIGNMENT_SECTION_UNMATCHED = 5,
    HWA_ALIGNMENT_SECTION_WARNING = 6
} HWAAlignmentFileSection;

typedef struct HWAAlignmentReadState {
    HWAAlignmentLockedSet result;
    size_t anchor_capacity;
    size_t max_anchors;
    size_t meta_index;
    size_t input_count;
    size_t row_count;
    HWAAlignmentFileSection section;
    double prior_reference_seconds;
    double prior_target_seconds;
    int have_prior_anchor;
    int retain_locked;
} HWAAlignmentReadState;

typedef int (*HWAAlignmentRowFunction)(char **fields,
                                       size_t field_count,
                                       size_t row,
                                       void *user,
                                       char *error,
                                       size_t error_size);

static int hwa_alignment_parse_double(const char *text,
                                      int allow_blank,
                                      double *value)
{
    char *end = NULL;
    double parsed;

    if (text[0] == '\0') {
        return allow_blank ? 1 : -1;
    }
    if (isspace((unsigned char)text[0])) {
        return -1;
    }
    errno = 0;
    parsed = strtod(text, &end);
    if (errno == ERANGE || end == text || *end != '\0' || !isfinite(parsed)) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static int hwa_alignment_parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    if (text[0] == '\0' || text[0] == '-' ||
        isspace((unsigned char)text[0])) {
        return -1;
    }
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0') {
        return -1;
    }
    *value = (uint64_t)parsed;
    return 0;
}

static int hwa_alignment_parse_u32(const char *text, uint32_t *value)
{
    uint64_t parsed;
    if (hwa_alignment_parse_u64(text, &parsed) != 0 ||
        parsed > UINT32_MAX) {
        return -1;
    }
    *value = (uint32_t)parsed;
    return 0;
}

static int hwa_alignment_parse_bit(const char *text, int *value)
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

static int hwa_alignment_hex_field(const char *text)
{
    size_t length = strlen(text);
    size_t index;

    if ((length & 1U) != 0U) {
        return 0;
    }
    for (index = 0U; index < length; ++index) {
        if (!((text[index] >= '0' && text[index] <= '9') ||
              (text[index] >= 'a' && text[index] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static int hwa_alignment_mode_value(const char *text,
                                    HWAAlignmentMode *mode)
{
    if (strcmp(text, "audio-audio") == 0) {
        *mode = HWA_ALIGNMENT_AUDIO_TO_AUDIO;
        return 0;
    }
    if (strcmp(text, "score-audio") == 0) {
        *mode = HWA_ALIGNMENT_SCORE_TO_AUDIO;
        return 0;
    }
    return -1;
}

static int hwa_alignment_origin_value(const char *text,
                                      HWAAlignmentOrigin *origin)
{
    if (strcmp(text, "auto") == 0) {
        *origin = HWA_ALIGNMENT_ORIGIN_AUTO;
        return 0;
    }
    if (strcmp(text, "manual") == 0) {
        *origin = HWA_ALIGNMENT_ORIGIN_MANUAL;
        return 0;
    }
    return -1;
}

static int hwa_alignment_status_value(const char *text)
{
    return strcmp(text, "matched") == 0 ||
           strcmp(text, "low-confidence") == 0 ||
           strcmp(text, "skipped") == 0 ||
           strcmp(text, "repeated") == 0 ||
           strcmp(text, "rest") == 0 ||
           strcmp(text, "ornament") == 0 ||
           strcmp(text, "cadenza") == 0;
}

static int hwa_alignment_side_value(const char *text)
{
    return strcmp(text, "reference") == 0 || strcmp(text, "target") == 0;
}

static int hwa_alignment_reason_value(const char *text)
{
    return strcmp(text, "prefix") == 0 ||
           strcmp(text, "suffix") == 0 ||
           strcmp(text, "skip") == 0 ||
           strcmp(text, "repeat") == 0 ||
           strcmp(text, "rest") == 0 ||
           strcmp(text, "cadenza") == 0 ||
           strcmp(text, "low-confidence") == 0 ||
           strcmp(text, "no-evidence") == 0;
}

static int hwa_alignment_read_regular(const char *path,
                                      uint64_t max_bytes,
                                      unsigned char **data,
                                      size_t *size,
                                      char *error,
                                      size_t error_size)
{
    uint64_t size_u64;
    FILE *stream;
    unsigned char *buffer;
#if defined(_WIN32)
    HWAWindowsFileIdentity expected_identity;
#else
    dev_t expected_device;
    ino_t expected_inode;
#endif

    *data = NULL;
    *size = 0U;
    if (path == NULL || strcmp(path, "-") == 0 || max_bytes == 0U) {
        hwa_set_error(error, error_size,
                      "prior alignment must be a named regular file");
        return -1;
    }
#if defined(_WIN32)
    {
        if (hwa_windows_identity_from_path(path, &expected_identity) != 0) {
            hwa_set_error(error, error_size,
                          "cannot inspect prior alignment '%s'", path);
            return -1;
        }
        size_u64 = expected_identity.size;
    }
#else
    {
        struct stat status;
        if (stat(path, &status) != 0) {
            hwa_set_error(error, error_size,
                          "cannot inspect prior alignment '%s': %s",
                          path, strerror(errno));
            return -1;
        }
        if (!S_ISREG(status.st_mode) || status.st_size < 0) {
            hwa_set_error(error, error_size,
                          "prior alignment is not a regular file");
            return -1;
        }
        size_u64 = (uint64_t)status.st_size;
        expected_device = status.st_dev;
        expected_inode = status.st_ino;
    }
#endif
    if (size_u64 > max_bytes || size_u64 > (uint64_t)(SIZE_MAX - 1U)) {
        hwa_set_error(error, error_size,
                      "prior alignment exceeds the byte limit");
        return -1;
    }
    stream = fopen(path, "rb");
    if (stream == NULL) {
        hwa_set_error(error, error_size,
                      "cannot open prior alignment '%s': %s",
                      path, strerror(errno));
        return -1;
    }
#if defined(_WIN32)
    {
        HWAWindowsFileIdentity opened_identity;
        if (hwa_windows_identity_from_stream(stream, &opened_identity) != 0 ||
            !hwa_windows_identity_equal(&expected_identity,
                                        &opened_identity)) {
            hwa_set_error(error, error_size,
                          "prior alignment changed before it was opened");
            (void)fclose(stream);
            return -1;
        }
    }
#else
    {
        struct stat opened;
        int descriptor = fileno(stream);
        if (descriptor < 0 || fstat(descriptor, &opened) != 0 ||
            !S_ISREG(opened.st_mode) || opened.st_size < 0 ||
            opened.st_dev != expected_device || opened.st_ino != expected_inode ||
            (uint64_t)opened.st_size != size_u64) {
            hwa_set_error(error, error_size,
                          "prior alignment changed before it was opened");
            (void)fclose(stream);
            return -1;
        }
    }
#endif
    buffer = (unsigned char *)malloc((size_t)size_u64 + 1U);
    if (buffer == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for the prior alignment");
        (void)fclose(stream);
        return -1;
    }
    if (size_u64 != 0U &&
        fread(buffer, 1U, (size_t)size_u64, stream) != (size_t)size_u64) {
        hwa_set_error(error, error_size,
                      "cannot read prior alignment '%s'", path);
        free(buffer);
        (void)fclose(stream);
        return -1;
    }
    if (fgetc(stream) != EOF || ferror(stream)) {
        hwa_set_error(error, error_size,
                      "prior alignment changed while it was read");
        free(buffer);
        (void)fclose(stream);
        return -1;
    }
    if (fclose(stream) != 0) {
        hwa_set_error(error, error_size,
                      "cannot close prior alignment '%s'", path);
        free(buffer);
        return -1;
    }
    buffer[(size_t)size_u64] = 0U;
    *data = buffer;
    *size = (size_t)size_u64;
    return 0;
}

static int hwa_alignment_csv_rows(const unsigned char *data,
                                  size_t size,
                                  HWAAlignmentRowFunction function,
                                  void *user,
                                  char *error,
                                  size_t error_size)
{
    char **fields;
    char *storage;
    size_t position = 0U;
    size_t physical_row = 1U;
    int saw_row = 0;

    fields = (char **)malloc(HWA_ALIGNMENT_FILE_MAX_FIELDS * sizeof(*fields));
    storage = (char *)malloc(size + 1U);
    if (fields == NULL || storage == NULL) {
        free(storage);
        free(fields);
        hwa_set_error(error, error_size,
                      "out of memory while parsing the prior alignment");
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

            if (field_count == HWA_ALIGNMENT_FILE_MAX_FIELDS) {
                hwa_set_error(error, error_size,
                              "alignment row %zu has too many fields",
                              logical_row);
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
                                  "alignment row %zu contains a NUL byte",
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
                                          "alignment row %zu has a bare carriage return",
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
                    if (byte == (unsigned char)'\n') {
                        physical_row++;
                    }
                    position++;
                    continue;
                }
                if (byte == (unsigned char)'"') {
                    hwa_set_error(error, error_size,
                                  "alignment row %zu has an unescaped quote",
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
                              "alignment row %zu has an unclosed quote",
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
                                  "alignment row %zu has a bare carriage return",
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
                              "alignment row %zu has text after a closing quote",
                              logical_row);
                free(storage);
                free(fields);
                return -1;
            }
        }
        if (field_count == 1U && fields[0][0] == '\0') {
            hwa_set_error(error, error_size,
                          "prior alignment contains an empty row at %zu",
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
        hwa_set_error(error, error_size, "prior alignment is empty");
        return -1;
    }
    return 0;
}

static int hwa_alignment_read_meta(char **fields,
                                   size_t field_count,
                                   size_t row,
                                   HWAAlignmentReadState *state,
                                   char *error,
                                   size_t error_size)
{
    const char *key;
    double value = 0.0;
    uint64_t integer_value = 0U;
    int integer_meta;

    if (field_count != 4U || state->meta_index >= HWA_ALIGNMENT_META_COUNT ||
        strcmp(fields[1], hwa_alignment_meta_keys[state->meta_index]) != 0 ||
        strcmp(fields[3], hwa_alignment_meta_units[state->meta_index]) != 0) {
        hwa_set_error(error, error_size,
                      "alignment row %zu has an unexpected META key", row);
        return -1;
    }
    key = fields[1];
    integer_meta =
        strcmp(key, "build_pointer_bits") == 0 ||
        strcmp(key, "dtw_cells") == 0 ||
        strcmp(key, "path_points") == 0 ||
        strcmp(key, "selected_channel") == 0 ||
        strcmp(key, "decode_block_frames") == 0 ||
        strcmp(key, "frame_size") == 0 ||
        strcmp(key, "hop_size") == 0 ||
        strcmp(key, "max_input_bytes") == 0 ||
        strcmp(key, "max_input_frames") == 0 ||
        strcmp(key, "max_analysis_work_bytes") == 0 ||
        strcmp(key, "max_transforms") == 0 ||
        strcmp(key, "max_track_points") == 0 ||
        strcmp(key, "max_spectrum_values") == 0 ||
        strcmp(key, "max_lag_samples") == 0 ||
        strcmp(key, "true_peak_oversample") == 0 ||
        strcmp(key, "max_dtw_cells") == 0 ||
        strcmp(key, "max_alignment_work_bytes") == 0 ||
        strcmp(key, "max_alignment_points") == 0 ||
        strcmp(key, "max_score_events") == 0 ||
        strcmp(key, "max_manual_anchors") == 0;
    if (strcmp(key, "tool_version") == 0) {
        if (fields[2][0] == '\0') {
            hwa_set_error(error, error_size,
                          "alignment row %zu has an empty tool version", row);
            return -1;
        }
    } else if (strcmp(key, "analysis_method_version") == 0) {
        if (strcmp(fields[2], HWA_ANALYSIS_METHOD_VERSION) != 0) {
            hwa_set_error(error, error_size,
                          "alignment row %zu uses an unsupported analysis method",
                          row);
            return -1;
        }
    } else if (strcmp(key, "alignment_method_version") == 0) {
        if (strcmp(fields[2], HWA_ALIGNMENT_METHOD_VERSION) != 0) {
            hwa_set_error(error, error_size,
                          "alignment row %zu uses an unsupported alignment method",
                          row);
            return -1;
        }
    } else if (strcmp(key, "mode") == 0) {
        if (hwa_alignment_mode_value(fields[2], &state->result.mode) != 0) {
            hwa_set_error(error, error_size,
                          "alignment row %zu has an invalid mode", row);
            return -1;
        }
    } else if (strcmp(key, "channel_mode") == 0) {
        if (!(strcmp(fields[2], "keep") == 0 ||
              strcmp(fields[2], "select") == 0 ||
              strcmp(fields[2], "mix") == 0)) {
            hwa_set_error(error, error_size,
                          "alignment row %zu has an invalid channel mode", row);
            return -1;
        }
    } else if (strncmp(key, "build_", 6U) == 0 &&
               strcmp(key, "build_pointer_bits") != 0) {
        if (fields[2][0] == '\0') {
            hwa_set_error(error, error_size,
                          "alignment row %zu has an empty build fact", row);
            return -1;
        }
    } else {
        if ((integer_meta &&
             hwa_alignment_parse_u64(fields[2], &integer_value) != 0) ||
            (!integer_meta &&
             hwa_alignment_parse_double(fields[2], 0, &value) != 0)) {
            hwa_set_error(error, error_size,
                          "alignment row %zu has a non-numeric META value",
                          row);
            return -1;
        }
        if (integer_meta) {
            value = (double)integer_value;
        }
        if (strcmp(key, "reference_duration_seconds") == 0) {
            if (value < 0.0) {
                hwa_set_error(error, error_size,
                              "alignment row %zu has a negative duration", row);
                return -1;
            }
            state->result.reference_duration_seconds = value;
        } else if (strcmp(key, "target_duration_seconds") == 0) {
            if (value < 0.0) {
                hwa_set_error(error, error_size,
                              "alignment row %zu has a negative duration", row);
                return -1;
            }
            state->result.target_duration_seconds = value;
        } else if ((strcmp(key, "tuning_confidence") == 0 ||
                    strcmp(key, "matched_coverage") == 0 ||
                    strcmp(key, "global_confidence") == 0 ||
                    strcmp(key, "match_threshold") == 0) &&
                   (value < 0.0 || value > 1.0)) {
            hwa_set_error(error, error_size,
                          "alignment row %zu has an out-of-range META ratio",
                          row);
            return -1;
        } else if ((strcmp(fields[3], "cost") == 0 ||
                    strcmp(fields[3], "cost_per_path_point") == 0 ||
                    strcmp(fields[3], "weight") == 0) && value < 0.0) {
            hwa_set_error(error, error_size,
                          "alignment row %zu has a negative META cost or weight",
                          row);
            return -1;
        } else if (strcmp(key, "build_pointer_bits") == 0 &&
                   integer_value == 0U) {
            hwa_set_error(error, error_size,
                          "alignment row %zu has zero pointer bits", row);
            return -1;
        }
    }
    state->meta_index++;
    return 0;
}

static int hwa_alignment_read_input(char **fields,
                                    size_t field_count,
                                    size_t row,
                                    HWAAlignmentReadState *state,
                                    char *error,
                                    size_t error_size)
{
    const char *role;
    double duration;
    double expected_duration;
    char *hash_target;

    if (field_count != 5U || state->input_count >= 2U ||
        !hwa_alignment_hex_field(fields[2]) ||
        !hwa_alignment_valid_sha256(fields[3]) ||
        hwa_alignment_parse_double(fields[4], 0, &duration) != 0 ||
        duration < 0.0) {
        hwa_set_error(error, error_size,
                      "alignment row %zu has an invalid INPUT record", row);
        return -1;
    }
    if (state->result.mode == HWA_ALIGNMENT_AUDIO_TO_AUDIO) {
        role = state->input_count == 0U ? "reference" : "target";
        expected_duration = state->input_count == 0U ?
            state->result.reference_duration_seconds :
            state->result.target_duration_seconds;
        hash_target = state->input_count == 0U ?
            state->result.reference_sha256 : state->result.target_sha256;
    } else {
        role = state->input_count == 0U ? "score" : "audio";
        expected_duration = state->input_count == 0U ?
            state->result.reference_duration_seconds :
            state->result.target_duration_seconds;
        hash_target = state->input_count == 0U ?
            state->result.score_sha256 : state->result.target_sha256;
    }
    if (strcmp(fields[1], role) != 0 || duration != expected_duration) {
        hwa_set_error(error, error_size,
                      "alignment row %zu has the wrong INPUT role or duration",
                      row);
        return -1;
    }
    memcpy(hash_target, fields[3], HWA_SHA256_HEX_SIZE);
    state->input_count++;
    return 0;
}

static int hwa_alignment_store_locked(HWAAlignmentReadState *state,
                                      const HWAAlignmentAnchor *anchor,
                                      char *error,
                                      size_t error_size)
{
    HWAAlignmentAnchor *grown;
    size_t capacity;

    if (!anchor->locked || !state->retain_locked) {
        return 0;
    }
    if (state->result.anchor_count == state->max_anchors) {
        hwa_set_error(error, error_size,
                      "prior alignment exceeds the manual-anchor limit");
        return -1;
    }
    if (state->result.anchor_count == state->anchor_capacity) {
        capacity = state->anchor_capacity == 0U ? 16U :
                   state->anchor_capacity * 2U;
        if (capacity < state->anchor_capacity || capacity > state->max_anchors) {
            capacity = state->max_anchors;
        }
        if (capacity > SIZE_MAX / sizeof(*grown)) {
            hwa_set_error(error, error_size, "too many locked anchors");
            return -1;
        }
        grown = (HWAAlignmentAnchor *)realloc(
            state->result.anchors, capacity * sizeof(*grown));
        if (grown == NULL) {
            hwa_set_error(error, error_size,
                          "out of memory for locked anchors");
            return -1;
        }
        state->result.anchors = grown;
        state->anchor_capacity = capacity;
    }
    state->result.anchors[state->result.anchor_count++] = *anchor;
    return 0;
}

static int hwa_alignment_read_anchor(char **fields,
                                     size_t field_count,
                                     size_t row,
                                     HWAAlignmentReadState *state,
                                     char *error,
                                     size_t error_size)
{
    HWAAlignmentAnchor anchor;
    int score_result;

    memset(&anchor, 0, sizeof(anchor));
    score_result = field_count == 9U ?
        hwa_alignment_parse_double(fields[4], 1, &anchor.score_beat) : -1;
    anchor.score_beat_valid = score_result == 0;
    if (field_count != 9U ||
        hwa_alignment_parse_u64(fields[1], &anchor.id) != 0 ||
        hwa_alignment_parse_double(fields[2], 0,
                                   &anchor.reference_seconds) != 0 ||
        hwa_alignment_parse_double(fields[3], 0,
                                   &anchor.target_seconds) != 0 ||
        score_result < 0 ||
        hwa_alignment_parse_double(fields[5], 0, &anchor.confidence) != 0 ||
        hwa_alignment_origin_value(fields[6], &anchor.origin) != 0 ||
        hwa_alignment_parse_bit(fields[7], &anchor.locked) != 0 ||
        hwa_alignment_parse_u32(fields[8], &anchor.evidence_flags) != 0 ||
        anchor.reference_seconds < 0.0 ||
        anchor.reference_seconds > state->result.reference_duration_seconds ||
        anchor.target_seconds < 0.0 ||
        anchor.target_seconds > state->result.target_duration_seconds ||
        anchor.confidence < 0.0 || anchor.confidence > 1.0 ||
        (anchor.score_beat_valid && anchor.score_beat < 0.0) ||
        (state->result.mode == HWA_ALIGNMENT_AUDIO_TO_AUDIO &&
         anchor.score_beat_valid) ||
        (state->result.mode == HWA_ALIGNMENT_SCORE_TO_AUDIO &&
         !anchor.score_beat_valid) ||
        (state->have_prior_anchor &&
         (anchor.reference_seconds <= state->prior_reference_seconds ||
          anchor.target_seconds <= state->prior_target_seconds))) {
        hwa_set_error(error, error_size,
                      "alignment row %zu has an invalid or non-monotone anchor",
                      row);
        return -1;
    }
    state->prior_reference_seconds = anchor.reference_seconds;
    state->prior_target_seconds = anchor.target_seconds;
    state->have_prior_anchor = 1;
    return hwa_alignment_store_locked(state, &anchor, error, error_size);
}

static int hwa_alignment_read_match(char **fields,
                                    size_t field_count,
                                    size_t row,
                                    const HWAAlignmentReadState *state,
                                    char *error,
                                    size_t error_size)
{
    uint64_t id;
    uint32_t evidence;
    double reference_start;
    double reference_end;
    double target_start;
    double target_end;
    double score_start = 0.0;
    double score_end = 0.0;
    double confidence;
    double tempo = 0.0;
    int score_start_result;
    int score_end_result;
    int tempo_result;
    int score_mode = state->result.mode == HWA_ALIGNMENT_SCORE_TO_AUDIO;

    score_start_result = field_count == 21U ?
        hwa_alignment_parse_double(fields[6], 1, &score_start) : -1;
    score_end_result = field_count == 21U ?
        hwa_alignment_parse_double(fields[7], 1, &score_end) : -1;
    tempo_result = field_count == 21U ?
        hwa_alignment_parse_double(fields[19], 1, &tempo) : -1;
    if (field_count != 21U ||
        hwa_alignment_parse_u64(fields[1], &id) != 0 ||
        hwa_alignment_parse_double(fields[2], 0, &reference_start) != 0 ||
        hwa_alignment_parse_double(fields[3], 0, &reference_end) != 0 ||
        hwa_alignment_parse_double(fields[4], 0, &target_start) != 0 ||
        hwa_alignment_parse_double(fields[5], 0, &target_end) != 0 ||
        score_start_result < 0 || score_end_result < 0 ||
        score_start_result != score_end_result ||
        hwa_alignment_parse_double(fields[8], 0, &confidence) != 0 ||
        !hwa_alignment_status_value(fields[9]) ||
        tempo_result < 0 ||
        hwa_alignment_parse_u32(fields[20], &evidence) != 0 ||
        reference_start < 0.0 || reference_end < reference_start ||
        reference_end > state->result.reference_duration_seconds ||
        target_start < 0.0 || target_end < target_start ||
        target_end > state->result.target_duration_seconds ||
        confidence < 0.0 || confidence > 1.0 ||
        (score_start_result == 0 &&
         (score_start < 0.0 || score_end < score_start)) ||
        (tempo_result == 0 && tempo <= 0.0) ||
        (score_mode &&
         (score_start_result != 0 || tempo_result != 0 ||
          fields[10][0] == '\0' || fields[11][0] == '\0')) ||
        (!score_mode && (score_start_result == 0 || tempo_result == 0))) {
        (void)id;
        (void)evidence;
        hwa_set_error(error, error_size,
                      "alignment row %zu has an invalid MATCH record", row);
        return -1;
    }
    return 0;
}

static int hwa_alignment_read_unmatched(char **fields,
                                        size_t field_count,
                                        size_t row,
                                        const HWAAlignmentReadState *state,
                                        char *error,
                                        size_t error_size)
{
    uint64_t id;
    double start;
    double end;
    double start_beat = 0.0;
    double end_beat = 0.0;
    double confidence;
    int start_result;
    int end_result;
    int reference_side;
    double duration;

    start_result = field_count == 9U ?
        hwa_alignment_parse_double(fields[5], 1, &start_beat) : -1;
    end_result = field_count == 9U ?
        hwa_alignment_parse_double(fields[6], 1, &end_beat) : -1;
    reference_side = field_count == 9U &&
                     strcmp(fields[2], "reference") == 0;
    duration = reference_side ? state->result.reference_duration_seconds :
                                state->result.target_duration_seconds;
    if (field_count != 9U ||
        hwa_alignment_parse_u64(fields[1], &id) != 0 ||
        !hwa_alignment_side_value(fields[2]) ||
        hwa_alignment_parse_double(fields[3], 0, &start) != 0 ||
        hwa_alignment_parse_double(fields[4], 0, &end) != 0 ||
        start_result < 0 || end_result < 0 || start_result != end_result ||
        !hwa_alignment_reason_value(fields[7]) ||
        hwa_alignment_parse_double(fields[8], 0, &confidence) != 0 ||
        start < 0.0 || end < start || end > duration ||
        confidence < 0.0 || confidence > 1.0 ||
        (start_result == 0 && (start_beat < 0.0 || end_beat < start_beat)) ||
        (state->result.mode == HWA_ALIGNMENT_AUDIO_TO_AUDIO &&
         start_result == 0) ||
        (state->result.mode == HWA_ALIGNMENT_SCORE_TO_AUDIO &&
         reference_side != (start_result == 0))) {
        (void)id;
        hwa_set_error(error, error_size,
                      "alignment row %zu has an invalid UNMATCHED record", row);
        return -1;
    }
    return 0;
}

static int hwa_alignment_read_row(char **fields,
                                  size_t field_count,
                                  size_t row,
                                  void *user,
                                  char *error,
                                  size_t error_size)
{
    HWAAlignmentReadState *state = (HWAAlignmentReadState *)user;
    HWAAlignmentFileSection section;
    uint64_t warning_id;

    if (state->row_count++ == 0U) {
        if (field_count != 2U ||
            strcmp(fields[0], "HWA_ALIGNMENT") != 0 ||
            strcmp(fields[1], "1") != 0) {
            hwa_set_error(error, error_size,
                          "prior alignment has an unsupported header");
            return -1;
        }
        state->section = HWA_ALIGNMENT_SECTION_META;
        return 0;
    }
    if (strcmp(fields[0], "META") == 0) {
        section = HWA_ALIGNMENT_SECTION_META;
    } else if (strcmp(fields[0], "INPUT") == 0) {
        section = HWA_ALIGNMENT_SECTION_INPUT;
    } else if (strcmp(fields[0], "ANCHOR") == 0) {
        section = HWA_ALIGNMENT_SECTION_ANCHOR;
    } else if (strcmp(fields[0], "MATCH") == 0) {
        section = HWA_ALIGNMENT_SECTION_MATCH;
    } else if (strcmp(fields[0], "UNMATCHED") == 0) {
        section = HWA_ALIGNMENT_SECTION_UNMATCHED;
    } else if (strcmp(fields[0], "WARNING") == 0) {
        section = HWA_ALIGNMENT_SECTION_WARNING;
    } else {
        hwa_set_error(error, error_size,
                      "alignment row %zu has an unknown record type", row);
        return -1;
    }
    if (section < state->section) {
        hwa_set_error(error, error_size,
                      "alignment row %zu is outside canonical record order",
                      row);
        return -1;
    }
    if (section > HWA_ALIGNMENT_SECTION_META &&
        state->meta_index != HWA_ALIGNMENT_META_COUNT) {
        hwa_set_error(error, error_size,
                      "alignment row %zu starts before META is complete", row);
        return -1;
    }
    if (section > HWA_ALIGNMENT_SECTION_INPUT && state->input_count != 2U) {
        hwa_set_error(error, error_size,
                      "alignment row %zu starts before INPUT is complete", row);
        return -1;
    }
    state->section = section;
    switch (section) {
    case HWA_ALIGNMENT_SECTION_META:
        return hwa_alignment_read_meta(fields, field_count, row, state,
                                       error, error_size);
    case HWA_ALIGNMENT_SECTION_INPUT:
        return hwa_alignment_read_input(fields, field_count, row, state,
                                        error, error_size);
    case HWA_ALIGNMENT_SECTION_ANCHOR:
        return hwa_alignment_read_anchor(fields, field_count, row, state,
                                         error, error_size);
    case HWA_ALIGNMENT_SECTION_MATCH:
        return hwa_alignment_read_match(fields, field_count, row, state,
                                        error, error_size);
    case HWA_ALIGNMENT_SECTION_UNMATCHED:
        return hwa_alignment_read_unmatched(fields, field_count, row, state,
                                            error, error_size);
    case HWA_ALIGNMENT_SECTION_WARNING:
        if (field_count != 4U ||
            hwa_alignment_parse_u64(fields[1], &warning_id) != 0 ||
            fields[2][0] == '\0') {
            hwa_set_error(error, error_size,
                          "alignment row %zu has an invalid WARNING record",
                          row);
            return -1;
        }
        return 0;
    default:
        hwa_set_error(error, error_size,
                      "alignment row %zu has an invalid record", row);
        return -1;
    }
}

int hwa_alignment_file_read_locked(const char *path,
                                   uint64_t max_bytes,
                                   size_t max_anchors,
                                   HWAAlignmentLockedSet *locked,
                                   char *error,
                                   size_t error_size)
{
    unsigned char *data = NULL;
    size_t size = 0U;
    HWAAlignmentReadState state;

    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (locked == NULL || max_anchors == 0U) {
        hwa_set_error(error, error_size,
                      "invalid prior-alignment arguments");
        return -1;
    }
    memset(&state, 0, sizeof(state));
    state.max_anchors = max_anchors;
    state.retain_locked = 1;
    if (hwa_alignment_read_regular(path, max_bytes, &data, &size,
                                   error, error_size) != 0) {
        return -1;
    }
    if (hwa_alignment_csv_rows(data, size, hwa_alignment_read_row, &state,
                               error, error_size) != 0 ||
        state.row_count == 0U ||
        state.meta_index != HWA_ALIGNMENT_META_COUNT ||
        state.input_count != 2U) {
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "prior alignment is incomplete");
        }
        free(data);
        hwa_alignment_locked_set_free(&state.result);
        return -1;
    }
    free(data);
    *locked = state.result;
    return 0;
}

int hwa_alignment_locked_set_matches(
    const HWAAlignmentLockedSet *locked,
    HWAAlignmentMode mode,
    const char reference_sha256[HWA_SHA256_HEX_SIZE],
    const char target_sha256[HWA_SHA256_HEX_SIZE],
    const char score_sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    int matches;

    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (locked == NULL || locked->mode != mode ||
        target_sha256 == NULL || !hwa_alignment_valid_sha256(target_sha256)) {
        hwa_set_error(error, error_size,
                      "prior alignment mode or target identity does not match");
        return -1;
    }
    if (mode == HWA_ALIGNMENT_AUDIO_TO_AUDIO) {
        matches = reference_sha256 != NULL &&
                  hwa_alignment_valid_sha256(reference_sha256) &&
                  strcmp(locked->reference_sha256, reference_sha256) == 0 &&
                  strcmp(locked->target_sha256, target_sha256) == 0;
    } else if (mode == HWA_ALIGNMENT_SCORE_TO_AUDIO) {
        matches = score_sha256 != NULL &&
                  hwa_alignment_valid_sha256(score_sha256) &&
                  strcmp(locked->score_sha256, score_sha256) == 0 &&
                  strcmp(locked->target_sha256, target_sha256) == 0;
    } else {
        matches = 0;
    }
    if (!matches) {
        hwa_set_error(error, error_size,
                      "prior alignment input hashes do not match");
        return -1;
    }
    return 0;
}

void hwa_alignment_locked_set_free(HWAAlignmentLockedSet *locked)
{
    if (locked == NULL) {
        return;
    }
    free(locked->anchors);
    memset(locked, 0, sizeof(*locked));
}

typedef struct HWAAlignmentFullReadState {
    HWAAlignmentReadState validator;
    HWAAlignmentFileLimits limits;
    HWAAlignment result;
    uint64_t work_bytes;
    size_t anchor_capacity;
    size_t match_capacity;
    size_t span_capacity;
    size_t warning_capacity;
} HWAAlignmentFullReadState;

static int hwa_alignment_full_charge(HWAAlignmentFullReadState *state,
                                     uint64_t bytes,
                                     char *error,
                                     size_t error_size)
{
    if (bytes > state->limits.max_work_bytes - state->work_bytes) {
        hwa_set_error(error, error_size,
                      "alignment file exceeds the current work-byte limit");
        return -1;
    }
    state->work_bytes += bytes;
    return 0;
}

static int hwa_alignment_full_reserve(HWAAlignmentFullReadState *state,
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
    void *grown;

    if (count >= maximum) {
        hwa_set_error(error, error_size,
                      "alignment file exceeds the current %s limit", name);
        return -1;
    }
    if (count < *capacity) {
        return 0;
    }
    next = *capacity == 0U ? 16U : *capacity * 2U;
    if (next < *capacity || next > maximum) {
        next = maximum;
    }
    if (next <= *capacity || next > SIZE_MAX / item_size) {
        hwa_set_error(error, error_size,
                      "alignment %s storage overflows", name);
        return -1;
    }
    added = next - *capacity;
    if ((uint64_t)added > UINT64_MAX / (uint64_t)item_size ||
        hwa_alignment_full_charge(
            state, (uint64_t)added * (uint64_t)item_size,
            error, error_size) != 0) {
        return -1;
    }
    grown = realloc(*array, next * item_size);
    if (grown == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for alignment %s", name);
        return -1;
    }
    memset((unsigned char *)grown + *capacity * item_size, 0,
           added * item_size);
    *array = grown;
    *capacity = next;
    return 0;
}

static char *hwa_alignment_full_copy(HWAAlignmentFullReadState *state,
                                     const char *text,
                                     int empty_is_null,
                                     char *error,
                                     size_t error_size)
{
    size_t length;
    char *copy;

    if (text == NULL || (empty_is_null && text[0] == '\0')) {
        return NULL;
    }
    length = strlen(text);
    if (length == SIZE_MAX ||
        hwa_alignment_full_charge(state, (uint64_t)length + 1U,
                                  error, error_size) != 0) {
        return NULL;
    }
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for alignment text");
        return NULL;
    }
    memcpy(copy, text, length + 1U);
    return copy;
}

static char *hwa_alignment_full_decode_path(HWAAlignmentFullReadState *state,
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
        hwa_alignment_full_charge(state, (uint64_t)length + 1U,
                                  error, error_size) != 0) {
        if (hex_length == 0U && error != NULL && error_size != 0U) {
            hwa_set_error(error, error_size,
                          "alignment input path is empty");
        }
        return NULL;
    }
    path = (char *)malloc(length + 1U);
    if (path == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for alignment input path");
        return NULL;
    }
    for (index = 0U; index < length; ++index) {
        const char *high = strchr(digits, hex[index * 2U]);
        const char *low = strchr(digits, hex[index * 2U + 1U]);
        unsigned value;

        if (high == NULL || low == NULL) {
            free(path);
            hwa_set_error(error, error_size,
                          "alignment input path has invalid hex bytes");
            return NULL;
        }
        value = (unsigned)(high - digits) * 16U +
                (unsigned)(low - digits);
        if (value == 0U) {
            free(path);
            hwa_set_error(error, error_size,
                          "alignment input path contains an embedded NUL byte");
            return NULL;
        }
        path[index] = (char)value;
    }
    path[length] = '\0';
    return path;
}

static int hwa_alignment_full_parse_size(const char *text, size_t *value)
{
    uint64_t parsed;

    if (hwa_alignment_parse_u64(text, &parsed) != 0 ||
        parsed > (uint64_t)SIZE_MAX) {
        return -1;
    }
    *value = (size_t)parsed;
    return 0;
}

static int hwa_alignment_full_parse_unsigned(const char *text,
                                             unsigned *value)
{
    uint64_t parsed;

    if (hwa_alignment_parse_u64(text, &parsed) != 0 || parsed > UINT_MAX) {
        return -1;
    }
    *value = (unsigned)parsed;
    return 0;
}

static int hwa_alignment_full_store_meta(char **fields,
                                         HWAAlignmentFullReadState *state)
{
    const char *key = fields[1];
    const char *text = fields[2];
    HWAAlignment *result = &state->result;
    HWAAnalysisOptions *analysis = &result->options.analysis;
    double number;
    uint64_t u64;

    if (strcmp(key, "mode") == 0) {
        result->mode = state->validator.result.mode;
    } else if (strcmp(key, "reference_duration_seconds") == 0) {
        result->reference_duration_seconds =
            state->validator.result.reference_duration_seconds;
    } else if (strcmp(key, "target_duration_seconds") == 0) {
        result->target_duration_seconds =
            state->validator.result.target_duration_seconds;
    } else if (strcmp(key, "channel_mode") == 0) {
        analysis->channel_mode = strcmp(text, "keep") == 0 ? HWA_CHANNEL_KEEP :
                                 strcmp(text, "select") == 0 ?
                                     HWA_CHANNEL_SELECT : HWA_CHANNEL_MIX;
    } else if (strcmp(key, "selected_channel") == 0) {
        if (hwa_alignment_parse_u64(text, &u64) != 0 || u64 > UINT16_MAX) {
            return -1;
        }
        analysis->selected_channel = (uint16_t)u64;
    } else if (strcmp(key, "decode_block_frames") == 0) {
        if (hwa_alignment_full_parse_size(text,
                                          &analysis->decode_block_frames) != 0) {
            return -1;
        }
    } else if (strcmp(key, "frame_size") == 0) {
        if (hwa_alignment_full_parse_size(text, &analysis->frame_size) != 0) {
            return -1;
        }
    } else if (strcmp(key, "hop_size") == 0) {
        if (hwa_alignment_full_parse_size(text, &analysis->hop_size) != 0) {
            return -1;
        }
    } else if (strcmp(key, "silence_threshold_dbfs") == 0) {
        if (hwa_alignment_parse_double(text, 0,
                                       &analysis->silence_threshold_dbfs) != 0) {
            return -1;
        }
    } else if (strcmp(key, "max_input_bytes") == 0) {
        if (hwa_alignment_parse_u64(text, &analysis->max_input_bytes) != 0) {
            return -1;
        }
    } else if (strcmp(key, "max_input_frames") == 0) {
        if (hwa_alignment_parse_u64(text, &analysis->max_input_frames) != 0) {
            return -1;
        }
    } else if (strcmp(key, "max_analysis_work_bytes") == 0) {
        if (hwa_alignment_parse_u64(text, &analysis->max_work_bytes) != 0) {
            return -1;
        }
    } else if (strcmp(key, "max_transforms") == 0) {
        if (hwa_alignment_full_parse_size(text, &analysis->max_transforms) != 0) {
            return -1;
        }
    } else if (strcmp(key, "max_track_points") == 0) {
        if (hwa_alignment_full_parse_size(text,
                                          &analysis->max_track_points) != 0) {
            return -1;
        }
    } else if (strcmp(key, "max_spectrum_values") == 0) {
        if (hwa_alignment_full_parse_size(
                text, &analysis->max_spectrum_values) != 0) {
            return -1;
        }
    } else if (strcmp(key, "max_lag_samples") == 0) {
        if (hwa_alignment_full_parse_size(text,
                                          &analysis->max_lag_samples) != 0) {
            return -1;
        }
    } else if (strcmp(key, "true_peak_oversample") == 0) {
        if (hwa_alignment_full_parse_unsigned(
                text, &analysis->true_peak_oversample) != 0) {
            return -1;
        }
    } else if (strcmp(key, "dtw_cells") == 0) {
        if (hwa_alignment_parse_u64(text, &result->dtw_cells) != 0) {
            return -1;
        }
    } else if (strcmp(key, "path_points") == 0) {
        if (hwa_alignment_parse_u64(text, &result->path_points) != 0) {
            return -1;
        }
    } else if (strcmp(key, "max_dtw_cells") == 0) {
        if (hwa_alignment_parse_u64(text,
                                    &result->options.max_dtw_cells) != 0) {
            return -1;
        }
    } else if (strcmp(key, "max_alignment_work_bytes") == 0) {
        if (hwa_alignment_parse_u64(
                text, &result->options.max_alignment_work_bytes) != 0) {
            return -1;
        }
    } else if (strcmp(key, "max_alignment_points") == 0) {
        if (hwa_alignment_full_parse_size(
                text, &result->options.max_alignment_points) != 0) {
            return -1;
        }
    } else if (strcmp(key, "max_score_events") == 0) {
        if (hwa_alignment_full_parse_size(
                text, &result->options.max_score_events) != 0) {
            return -1;
        }
    } else if (strcmp(key, "max_manual_anchors") == 0) {
        if (hwa_alignment_full_parse_size(
                text, &result->options.max_manual_anchors) != 0) {
            return -1;
        }
    } else if (hwa_alignment_parse_double(text, 0, &number) == 0) {
#define HWA_STORE_NUMBER(meta_key, field)                                  \
        if (strcmp(key, (meta_key)) == 0) {                                \
            (field) = number;                                               \
        } else
        HWA_STORE_NUMBER("tuning_offset_cents", result->tuning_offset_cents)
        HWA_STORE_NUMBER("tuning_confidence", result->tuning_confidence)
        HWA_STORE_NUMBER("total_cost", result->total_cost)
        HWA_STORE_NUMBER("normalized_cost", result->normalized_cost)
        HWA_STORE_NUMBER("matched_coverage", result->matched_coverage)
        HWA_STORE_NUMBER("global_confidence", result->global_confidence)
        HWA_STORE_NUMBER("alignment_step_seconds",
                         result->options.alignment_step_seconds)
        HWA_STORE_NUMBER("coarse_step_seconds",
                         result->options.coarse_step_seconds)
        HWA_STORE_NUMBER("dtw_band_seconds",
                         result->options.dtw_band_seconds)
        HWA_STORE_NUMBER("fine_radius_seconds",
                         result->options.fine_radius_seconds)
        HWA_STORE_NUMBER("refine_radius_seconds",
                         result->options.refine_radius_seconds)
        HWA_STORE_NUMBER("match_threshold", result->options.match_threshold)
        HWA_STORE_NUMBER("chroma_weight", result->options.chroma_weight)
        HWA_STORE_NUMBER("onset_weight", result->options.onset_weight)
        HWA_STORE_NUMBER("pitch_weight", result->options.pitch_weight)
        HWA_STORE_NUMBER("envelope_weight", result->options.envelope_weight)
        HWA_STORE_NUMBER("activity_weight", result->options.activity_weight)
        HWA_STORE_NUMBER("skip_cost", result->options.skip_cost)
        HWA_STORE_NUMBER("repeat_cost", result->options.repeat_cost)
        HWA_STORE_NUMBER("ornament_cost", result->options.ornament_cost)
        HWA_STORE_NUMBER("rest_cost", result->options.rest_cost)
        HWA_STORE_NUMBER("cadenza_cost", result->options.cadenza_cost)
        { }
#undef HWA_STORE_NUMBER
    }
    return 0;
}

static HWAAlignmentStatus hwa_alignment_full_status(const char *text)
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

static HWAAlignmentSide hwa_alignment_full_side(const char *text)
{
    return strcmp(text, "reference") == 0 ? HWA_ALIGNMENT_REFERENCE :
                                             HWA_ALIGNMENT_TARGET;
}

static HWAUnmatchedReason hwa_alignment_full_reason(const char *text)
{
    if (strcmp(text, "prefix") == 0) return HWA_UNMATCHED_PREFIX;
    if (strcmp(text, "suffix") == 0) return HWA_UNMATCHED_SUFFIX;
    if (strcmp(text, "skip") == 0) return HWA_UNMATCHED_SKIP;
    if (strcmp(text, "repeat") == 0) return HWA_UNMATCHED_REPEAT;
    if (strcmp(text, "rest") == 0) return HWA_UNMATCHED_REST;
    if (strcmp(text, "cadenza") == 0) return HWA_UNMATCHED_CADENZA;
    if (strcmp(text, "low-confidence") == 0) {
        return HWA_UNMATCHED_LOW_CONFIDENCE;
    }
    return HWA_UNMATCHED_NO_EVIDENCE;
}

static int hwa_alignment_full_store_input(char **fields,
                                          HWAAlignmentFullReadState *state,
                                          char *error,
                                          size_t error_size)
{
    char *path = hwa_alignment_full_decode_path(state, fields[2],
                                                error, error_size);
    HWAAlignment *result = &state->result;

    if (path == NULL) {
        return -1;
    }
    if (strcmp(fields[1], "reference") == 0) {
        result->reference_path = path;
        memcpy(result->reference_sha256, fields[3], HWA_SHA256_HEX_SIZE);
    } else if (strcmp(fields[1], "target") == 0 ||
               strcmp(fields[1], "audio") == 0) {
        result->target_path = path;
        memcpy(result->target_sha256, fields[3], HWA_SHA256_HEX_SIZE);
    } else {
        result->score_path = path;
        memcpy(result->score_sha256, fields[3], HWA_SHA256_HEX_SIZE);
    }
    return 0;
}

static int hwa_alignment_full_store_anchor(char **fields,
                                           HWAAlignmentFullReadState *state,
                                           char *error,
                                           size_t error_size)
{
    HWAAlignment *result = &state->result;
    HWAAlignmentAnchor *anchor;
    int score_result;

    if (hwa_alignment_full_reserve(
            state, (void **)&result->anchors, &state->anchor_capacity,
            result->anchor_count, state->limits.max_anchors,
            sizeof(*result->anchors), "anchor", error, error_size) != 0) {
        return -1;
    }
    anchor = &result->anchors[result->anchor_count];
    score_result = hwa_alignment_parse_double(fields[4], 1,
                                               &anchor->score_beat);
    if (hwa_alignment_parse_u64(fields[1], &anchor->id) != 0 ||
        hwa_alignment_parse_double(fields[2], 0,
                                   &anchor->reference_seconds) != 0 ||
        hwa_alignment_parse_double(fields[3], 0,
                                   &anchor->target_seconds) != 0 ||
        score_result < 0 ||
        hwa_alignment_parse_double(fields[5], 0,
                                   &anchor->confidence) != 0 ||
        hwa_alignment_origin_value(fields[6], &anchor->origin) != 0 ||
        hwa_alignment_parse_bit(fields[7], &anchor->locked) != 0 ||
        hwa_alignment_parse_u32(fields[8], &anchor->evidence_flags) != 0) {
        hwa_set_error(error, error_size,
                      "cannot retain alignment anchor");
        return -1;
    }
    anchor->score_beat_valid = score_result == 0;
    result->anchor_count++;
    return 0;
}

static int hwa_alignment_full_copy_match_strings(
    HWAAlignmentFullReadState *state,
    HWAAlignmentMatch *match,
    char **fields,
    char *error,
    size_t error_size)
{
    char **targets[] = {
        &match->event_id, &match->kind, &match->voice,
        &match->midi_note, &match->velocity, &match->tie,
        &match->dynamic, &match->mark, &match->score_position
    };
    size_t index;

    for (index = 0U; index < sizeof(targets) / sizeof(targets[0]); ++index) {
        *targets[index] = hwa_alignment_full_copy(
            state, fields[10U + index], 1, error, error_size);
        if (fields[10U + index][0] != '\0' && *targets[index] == NULL) {
            size_t cleanup;
            for (cleanup = 0U; cleanup < index; ++cleanup) {
                free(*targets[cleanup]);
                *targets[cleanup] = NULL;
            }
            return -1;
        }
    }
    return 0;
}

static int hwa_alignment_full_store_match(char **fields,
                                          HWAAlignmentFullReadState *state,
                                          char *error,
                                          size_t error_size)
{
    HWAAlignment *result = &state->result;
    HWAAlignmentMatch *match;
    int score_start_result;
    int score_end_result;
    int tempo_result;

    if (hwa_alignment_full_reserve(
            state, (void **)&result->matches, &state->match_capacity,
            result->match_count, state->limits.max_matches,
            sizeof(*result->matches), "match", error, error_size) != 0) {
        return -1;
    }
    match = &result->matches[result->match_count];
    score_start_result = hwa_alignment_parse_double(
        fields[6], 1, &match->score_start_beat);
    score_end_result = hwa_alignment_parse_double(
        fields[7], 1, &match->score_end_beat);
    tempo_result = hwa_alignment_parse_double(fields[19], 1,
                                              &match->tempo_bpm);
    if (hwa_alignment_parse_u64(fields[1], &match->id) != 0 ||
        hwa_alignment_parse_double(fields[2], 0,
                                   &match->reference_start_seconds) != 0 ||
        hwa_alignment_parse_double(fields[3], 0,
                                   &match->reference_end_seconds) != 0 ||
        hwa_alignment_parse_double(fields[4], 0,
                                   &match->target_start_seconds) != 0 ||
        hwa_alignment_parse_double(fields[5], 0,
                                   &match->target_end_seconds) != 0 ||
        score_start_result < 0 || score_end_result < 0 ||
        hwa_alignment_parse_double(fields[8], 0,
                                   &match->confidence) != 0 ||
        tempo_result < 0 ||
        hwa_alignment_parse_u32(fields[20],
                                &match->evidence_flags) != 0 ||
        (result->match_count != 0U &&
         (match->reference_start_seconds <
              result->matches[result->match_count - 1U]
                  .reference_start_seconds ||
          (match->reference_start_seconds ==
               result->matches[result->match_count - 1U]
                   .reference_start_seconds &&
           match->id <= result->matches[result->match_count - 1U].id))) ||
        hwa_alignment_full_copy_match_strings(
            state, match, fields, error, error_size) != 0) {
        hwa_set_error(error, error_size,
                      "cannot retain canonical alignment match order");
        return -1;
    }
    match->score_span_valid = score_start_result == 0;
    match->tempo_valid = tempo_result == 0;
    match->status = hwa_alignment_full_status(fields[9]);
    result->match_count++;
    return 0;
}

static int hwa_alignment_full_store_span(char **fields,
                                         HWAAlignmentFullReadState *state,
                                         char *error,
                                         size_t error_size)
{
    HWAAlignment *result = &state->result;
    HWAUnmatchedSpan *span;
    int start_result;
    int end_result;

    if (hwa_alignment_full_reserve(
            state, (void **)&result->unmatched_spans,
            &state->span_capacity, result->unmatched_span_count,
            state->limits.max_unmatched_spans,
            sizeof(*result->unmatched_spans), "unmatched-span",
            error, error_size) != 0) {
        return -1;
    }
    span = &result->unmatched_spans[result->unmatched_span_count];
    start_result = hwa_alignment_parse_double(fields[5], 1,
                                              &span->start_beat);
    end_result = hwa_alignment_parse_double(fields[6], 1,
                                            &span->end_beat);
    span->side = hwa_alignment_full_side(fields[2]);
    span->reason = hwa_alignment_full_reason(fields[7]);
    if (hwa_alignment_parse_u64(fields[1], &span->id) != 0 ||
        hwa_alignment_parse_double(fields[3], 0,
                                   &span->start_seconds) != 0 ||
        hwa_alignment_parse_double(fields[4], 0,
                                   &span->end_seconds) != 0 ||
        start_result < 0 || end_result < 0 ||
        hwa_alignment_parse_double(fields[8], 0,
                                   &span->confidence) != 0 ||
        (result->unmatched_span_count != 0U &&
         (span->side <
              result->unmatched_spans[result->unmatched_span_count - 1U]
                  .side ||
          (span->side ==
               result->unmatched_spans[result->unmatched_span_count - 1U]
                   .side &&
           (span->start_seconds <
                result->unmatched_spans[
                    result->unmatched_span_count - 1U].start_seconds ||
            (span->start_seconds ==
                 result->unmatched_spans[
                     result->unmatched_span_count - 1U].start_seconds &&
             span->id <=
                 result->unmatched_spans[
                     result->unmatched_span_count - 1U].id)))))) {
        hwa_set_error(error, error_size,
                      "cannot retain canonical unmatched-span order");
        return -1;
    }
    span->score_span_valid = start_result == 0;
    result->unmatched_span_count++;
    return 0;
}

static int hwa_alignment_full_store_warning(char **fields,
                                            HWAAlignmentFullReadState *state,
                                            char *error,
                                            size_t error_size)
{
    HWAAlignment *result = &state->result;
    HWAAlignmentWarning *warning;

    if (hwa_alignment_full_reserve(
            state, (void **)&result->warnings, &state->warning_capacity,
            result->warning_count, state->limits.max_warnings,
            sizeof(*result->warnings), "warning", error, error_size) != 0) {
        return -1;
    }
    warning = &result->warnings[result->warning_count];
    if (hwa_alignment_parse_u64(fields[1], &warning->id) != 0 ||
        (result->warning_count != 0U &&
         warning->id <= result->warnings[result->warning_count - 1U].id)) {
        hwa_set_error(error, error_size,
                      "cannot retain canonical alignment warning order");
        return -1;
    }
    warning->code = hwa_alignment_full_copy(state, fields[2], 0,
                                            error, error_size);
    warning->message = hwa_alignment_full_copy(state, fields[3], 0,
                                               error, error_size);
    if (warning->code == NULL || warning->message == NULL) {
        free(warning->message);
        free(warning->code);
        warning->message = NULL;
        warning->code = NULL;
        return -1;
    }
    result->warning_count++;
    return 0;
}

static int hwa_alignment_full_read_row(char **fields,
                                       size_t field_count,
                                       size_t row,
                                       void *user,
                                       char *error,
                                       size_t error_size)
{
    HWAAlignmentFullReadState *state =
        (HWAAlignmentFullReadState *)user;
    size_t index;

    if (field_count > state->limits.max_fields_per_row) {
        hwa_set_error(error, error_size,
                      "alignment row %zu exceeds the current field limit",
                      row);
        return -1;
    }
    for (index = 0U; index < field_count; ++index) {
        if (strlen(fields[index]) > state->limits.max_field_bytes) {
            hwa_set_error(error, error_size,
                          "alignment row %zu exceeds the current field-byte limit",
                          row);
            return -1;
        }
    }
    if (hwa_alignment_read_row(fields, field_count, row, &state->validator,
                               error, error_size) != 0) {
        return -1;
    }
    if (state->validator.row_count == 1U) {
        return 0;
    }
    if (strcmp(fields[0], "META") == 0) {
        if (hwa_alignment_full_store_meta(fields, state) != 0) {
            hwa_set_error(error, error_size,
                          "alignment row %zu has an unsupported META value",
                          row);
            return -1;
        }
        return 0;
    }
    state->result.mode = state->validator.result.mode;
    state->result.reference_duration_seconds =
        state->validator.result.reference_duration_seconds;
    state->result.target_duration_seconds =
        state->validator.result.target_duration_seconds;
    if (strcmp(fields[0], "INPUT") == 0) {
        return hwa_alignment_full_store_input(fields, state,
                                              error, error_size);
    }
    if (strcmp(fields[0], "ANCHOR") == 0) {
        return hwa_alignment_full_store_anchor(fields, state,
                                               error, error_size);
    }
    if (strcmp(fields[0], "MATCH") == 0) {
        return hwa_alignment_full_store_match(fields, state,
                                              error, error_size);
    }
    if (strcmp(fields[0], "UNMATCHED") == 0) {
        return hwa_alignment_full_store_span(fields, state,
                                             error, error_size);
    }
    return hwa_alignment_full_store_warning(fields, state,
                                            error, error_size);
}

static int hwa_alignment_full_mark_id(unsigned char *seen,
                                      size_t count,
                                      uint64_t id,
                                      const char *kind,
                                      char *error,
                                      size_t error_size)
{
    if (id == 0U || id > (uint64_t)count || seen[(size_t)id] != 0U) {
        hwa_set_error(error, error_size,
                      "alignment %s IDs must be unique and complete from 1 to %zu",
                      kind, count);
        return -1;
    }
    seen[(size_t)id] = 1U;
    return 0;
}

static int hwa_alignment_full_validate_ids(HWAAlignmentFullReadState *state,
                                           char *error,
                                           size_t error_size)
{
    const HWAAlignment *result = &state->result;
    size_t maximum = result->anchor_count;
    size_t index;
    uint64_t bytes;
    unsigned char *seen;

    if (result->match_count > maximum) maximum = result->match_count;
    if (result->unmatched_span_count > maximum) {
        maximum = result->unmatched_span_count;
    }
    if (result->warning_count > maximum) maximum = result->warning_count;
    if (maximum == SIZE_MAX) {
        hwa_set_error(error, error_size,
                      "alignment ID validation storage overflows");
        return -1;
    }
    bytes = (uint64_t)maximum + 1U;
    if (hwa_alignment_full_charge(state, bytes, error, error_size) != 0) {
        return -1;
    }
    seen = (unsigned char *)calloc(maximum + 1U, 1U);
    if (seen == NULL) {
        state->work_bytes -= bytes;
        hwa_set_error(error, error_size,
                      "out of memory for alignment ID validation");
        return -1;
    }
#define HWA_VALIDATE_IDS(array, count, kind)                                \
    do {                                                                     \
        memset(seen, 0, (count) + 1U);                                       \
        for (index = 0U; index < (count); ++index) {                         \
            if (hwa_alignment_full_mark_id(                                 \
                    seen, (count), (array)[index].id, (kind),               \
                    error, error_size) != 0) {                              \
                free(seen);                                                  \
                state->work_bytes -= bytes;                                  \
                return -1;                                                   \
            }                                                                \
        }                                                                    \
    } while (0)
    HWA_VALIDATE_IDS(result->anchors, result->anchor_count, "anchor");
    HWA_VALIDATE_IDS(result->matches, result->match_count, "match");
    HWA_VALIDATE_IDS(result->unmatched_spans,
                     result->unmatched_span_count, "unmatched-span");
    HWA_VALIDATE_IDS(result->warnings, result->warning_count, "warning");
#undef HWA_VALIDATE_IDS
    free(seen);
    state->work_bytes -= bytes;
    return 0;
}

static int hwa_alignment_full_match_event_compare(const void *left,
                                                   const void *right)
{
    const HWAAlignmentMatch *const *first =
        (const HWAAlignmentMatch *const *)left;
    const HWAAlignmentMatch *const *second =
        (const HWAAlignmentMatch *const *)right;
    return strcmp((*first)->event_id, (*second)->event_id);
}

static int hwa_alignment_full_validate_event_ids(
    HWAAlignmentFullReadState *state,
    char *error,
    size_t error_size)
{
    HWAAlignment *result = &state->result;
    HWAAlignmentMatch **order;
    uint64_t bytes;
    size_t count = 0U;
    size_t index;

    for (index = 0U; index < result->match_count; ++index) {
        if (result->matches[index].event_id != NULL &&
            result->matches[index].event_id[0] != '\0') {
            count++;
        }
    }
    if (count < 2U) return 0;
    if (count > SIZE_MAX / sizeof(*order)
#if SIZE_MAX > UINT64_MAX
        || count > UINT64_MAX / (uint64_t)sizeof(*order)
#endif
    ) {
        hwa_set_error(error, error_size,
                      "alignment event-ID index storage overflows");
        return -1;
    }
    bytes = (uint64_t)count * (uint64_t)sizeof(*order);
    if (hwa_alignment_full_charge(state, bytes, error, error_size) != 0) {
        return -1;
    }
    order = (HWAAlignmentMatch **)malloc(count * sizeof(*order));
    if (order == NULL) {
        state->work_bytes -= bytes;
        hwa_set_error(error, error_size,
                      "out of memory for alignment event-ID index");
        return -1;
    }
    count = 0U;
    for (index = 0U; index < result->match_count; ++index) {
        if (result->matches[index].event_id != NULL &&
            result->matches[index].event_id[0] != '\0') {
            order[count++] = &result->matches[index];
        }
    }
    qsort(order, count, sizeof(*order),
          hwa_alignment_full_match_event_compare);
    for (index = 1U; index < count; ++index) {
        if (strcmp(order[index - 1U]->event_id,
                   order[index]->event_id) == 0) {
            hwa_set_error(error, error_size,
                          "alignment MATCH event_id values must be unique");
            free(order);
            state->work_bytes -= bytes;
            return -1;
        }
    }
    free(order);
    state->work_bytes -= bytes;
    return 0;
}

void hwa_alignment_file_limits_default(HWAAlignmentFileLimits *limits)
{
    if (limits == NULL) {
        return;
    }
    limits->max_bytes = UINT64_C(67108864);
    limits->max_work_bytes = UINT64_C(268435456);
    limits->max_fields_per_row = HWA_ALIGNMENT_FILE_MAX_FIELDS;
    limits->max_field_bytes = 65536U;
    limits->max_anchors = 200000U;
    limits->max_matches = 200000U;
    limits->max_unmatched_spans = 200000U;
    limits->max_warnings = 200000U;
}

int hwa_alignment_file_read(const char *path,
                            const HWAAlignmentFileLimits *limits,
                            HWAAlignment *alignment,
                            char *error,
                            size_t error_size)
{
    HWAAlignmentFileLimits copied;
    HWAAlignmentFullReadState state;
    unsigned char *data = NULL;
    size_t size = 0U;
    uint64_t parser_bytes;
    uint64_t file_limit;

    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (alignment == NULL || limits == NULL) {
        hwa_set_error(error, error_size,
                      "invalid full alignment-reader arguments");
        return -1;
    }
    copied = *limits;
    memset(alignment, 0, sizeof(*alignment));
    if (copied.max_bytes == 0U || copied.max_work_bytes == 0U ||
        copied.max_fields_per_row == 0U || copied.max_field_bytes == 0U ||
        copied.max_anchors == 0U || copied.max_matches == 0U ||
        copied.max_unmatched_spans == 0U || copied.max_warnings == 0U) {
        hwa_set_error(error, error_size,
                      "full alignment-reader limits must be nonzero");
        return -1;
    }
    file_limit = copied.max_bytes;
    if (file_limit > copied.max_work_bytes / 2U) {
        file_limit = copied.max_work_bytes / 2U;
    }
    if (file_limit == 0U ||
        hwa_alignment_read_regular(path, file_limit, &data, &size,
                                   error, error_size) != 0) {
        return -1;
    }
    memset(&state, 0, sizeof(state));
    state.limits = copied;
    state.validator.retain_locked = 0;
    hwa_alignment_options_default(&state.result.options);
    if ((uint64_t)size + 1U > UINT64_MAX / 2U ||
        (uint64_t)HWA_ALIGNMENT_FILE_MAX_FIELDS >
            UINT64_MAX / (uint64_t)sizeof(char *)) {
        hwa_set_error(error, error_size,
                      "alignment parser storage overflows");
        free(data);
        return -1;
    }
    parser_bytes = ((uint64_t)size + 1U) * 2U +
                   (uint64_t)HWA_ALIGNMENT_FILE_MAX_FIELDS *
                       (uint64_t)sizeof(char *);
    if (parser_bytes > copied.max_work_bytes) {
        hwa_set_error(error, error_size,
                      "alignment parser exceeds the current work-byte limit");
        free(data);
        return -1;
    }
    state.work_bytes = parser_bytes;
    if (hwa_alignment_csv_rows(data, size, hwa_alignment_full_read_row,
                               &state, error, error_size) != 0 ||
        state.validator.row_count == 0U ||
        state.validator.meta_index != HWA_ALIGNMENT_META_COUNT ||
        state.validator.input_count != 2U ||
        hwa_alignment_full_validate_ids(&state, error, error_size) != 0 ||
        hwa_alignment_full_validate_event_ids(
            &state, error, error_size) != 0) {
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "prior alignment is incomplete");
        }
        free(data);
        hwa_alignment_free(&state.result);
        return -1;
    }
    free(data);
    state.result.retained_work_bytes = state.work_bytes - parser_bytes;
    *alignment = state.result;
    return 0;
}

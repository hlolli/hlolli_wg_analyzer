#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "alignment_file.h"
#include "item_file.h"
#include "typed_labels.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int failures;

#define CHECK(condition, ...)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);       \
            (void)fprintf(stderr, __VA_ARGS__);                              \
            (void)fputc('\n', stderr);                                       \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static long test_process_id(void)
{
#if defined(_WIN32)
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}

static int test_make_directory(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

static int test_remove_directory(const char *path)
{
#if defined(_WIN32)
    return _rmdir(path);
#else
    return rmdir(path);
#endif
}

static int test_remove_file(const char *path)
{
#if defined(_WIN32)
    return _unlink(path);
#else
    return unlink(path);
#endif
}

static int test_workspace(char path[PATH_MAX])
{
    unsigned attempt;
#if defined(_WIN32)
    const char *root = getenv("TEMP");
    if (root == NULL || root[0] == '\0') root = ".";
#else
    const char *root = "/tmp";
#endif
    for (attempt = 0U; attempt < 100U; ++attempt) {
        int length = snprintf(path, PATH_MAX,
                              "%s/hwa-stage3-persistence-%ld-%u",
                              root, test_process_id(), attempt);
        if (length < 0 || length >= PATH_MAX) return 0;
        if (test_make_directory(path) == 0) return 1;
        if (errno != EEXIST) return 0;
    }
    return 0;
}

static int test_path(char output[PATH_MAX],
                     const char *directory,
                     const char *name)
{
    int length = snprintf(output, PATH_MAX, "%s/%s", directory, name);
    return length >= 0 && length < PATH_MAX;
}

static int test_write_bytes(const char *path, const void *bytes, size_t size)
{
    FILE *stream = fopen(path, "wb");
    int ok = stream != NULL && fwrite(bytes, 1U, size, stream) == size;
    if (stream != NULL && fclose(stream) != 0) ok = 0;
    return ok;
}

static char *test_read_bytes(const char *path, size_t *size)
{
    FILE *stream = fopen(path, "rb");
    long length;
    char *bytes;
    *size = 0U;
    if (stream == NULL || fseek(stream, 0L, SEEK_END) != 0) {
        if (stream != NULL) (void)fclose(stream);
        return NULL;
    }
    length = ftell(stream);
    if (length < 0 || fseek(stream, 0L, SEEK_SET) != 0 ||
        (uintmax_t)length > (uintmax_t)(SIZE_MAX - 1U)) {
        (void)fclose(stream);
        return NULL;
    }
    bytes = (char *)malloc((size_t)length + 1U);
    if (bytes == NULL) {
        (void)fclose(stream);
        return NULL;
    }
    if (fread(bytes, 1U, (size_t)length, stream) != (size_t)length ||
        fclose(stream) != 0) {
        free(bytes);
        return NULL;
    }
    bytes[length] = '\0';
    *size = (size_t)length;
    return bytes;
}

static int test_analysis_options_equal(const HWAAnalysisOptions *left,
                                       const HWAAnalysisOptions *right)
{
    return left->channel_mode == right->channel_mode &&
           left->selected_channel == right->selected_channel &&
           left->decode_block_frames == right->decode_block_frames &&
           left->frame_size == right->frame_size &&
           left->hop_size == right->hop_size &&
           left->silence_threshold_dbfs == right->silence_threshold_dbfs &&
           left->max_input_bytes == right->max_input_bytes &&
           left->max_input_frames == right->max_input_frames &&
           left->max_work_bytes == right->max_work_bytes &&
           left->max_transforms == right->max_transforms &&
           left->max_track_points == right->max_track_points &&
           left->max_spectrum_values == right->max_spectrum_values &&
           left->max_lag_samples == right->max_lag_samples &&
           left->true_peak_oversample == right->true_peak_oversample &&
           left->collect_tracks == right->collect_tracks &&
           left->collect_spectrogram == right->collect_spectrogram;
}

static int test_alignment_options_equal(const HWAAlignmentOptions *left,
                                        const HWAAlignmentOptions *right)
{
    return test_analysis_options_equal(&left->analysis, &right->analysis) &&
           left->alignment_step_seconds == right->alignment_step_seconds &&
           left->coarse_step_seconds == right->coarse_step_seconds &&
           left->dtw_band_seconds == right->dtw_band_seconds &&
           left->fine_radius_seconds == right->fine_radius_seconds &&
           left->refine_radius_seconds == right->refine_radius_seconds &&
           left->match_threshold == right->match_threshold &&
           left->chroma_weight == right->chroma_weight &&
           left->onset_weight == right->onset_weight &&
           left->pitch_weight == right->pitch_weight &&
           left->envelope_weight == right->envelope_weight &&
           left->activity_weight == right->activity_weight &&
           left->skip_cost == right->skip_cost &&
           left->repeat_cost == right->repeat_cost &&
           left->ornament_cost == right->ornament_cost &&
           left->rest_cost == right->rest_cost &&
           left->cadenza_cost == right->cadenza_cost &&
           left->max_dtw_cells == right->max_dtw_cells &&
           left->max_alignment_work_bytes ==
               right->max_alignment_work_bytes &&
           left->max_alignment_points == right->max_alignment_points &&
           left->max_score_events == right->max_score_events &&
           left->max_manual_anchors == right->max_manual_anchors;
}

static int test_write_swapped_rows(const char *path,
                                   const char *bytes,
                                   size_t size,
                                   const char *first_prefix,
                                   const char *second_prefix)
{
    const char *first = strstr(bytes, first_prefix);
    const char *second = strstr(bytes, second_prefix);
    const char *first_end;
    const char *second_end;
    char *swapped;
    size_t prefix_size;
    size_t first_size;
    size_t second_size;
    size_t suffix_size;
    size_t offset = 0U;
    int result;

    if (first == NULL || second == NULL || first >= second) return 0;
    first_end = strchr(first, '\n');
    second_end = strchr(second, '\n');
    if (first_end == NULL || second_end == NULL || first_end + 1 != second) {
        return 0;
    }
    first_end++;
    second_end++;
    prefix_size = (size_t)(first - bytes);
    first_size = (size_t)(first_end - first);
    second_size = (size_t)(second_end - second);
    suffix_size = size - (size_t)(second_end - bytes);
    swapped = (char *)malloc(size);
    if (swapped == NULL) return 0;
    memcpy(swapped + offset, bytes, prefix_size);
    offset += prefix_size;
    memcpy(swapped + offset, second, second_size);
    offset += second_size;
    memcpy(swapped + offset, first, first_size);
    offset += first_size;
    memcpy(swapped + offset, second_end, suffix_size);
    offset += suffix_size;
    result = offset == size && test_write_bytes(path, swapped, size);
    free(swapped);
    return result;
}

static int test_alignment_rejected(const char *path,
                                   const HWAAlignmentFileLimits *limits,
                                   const char *error_part)
{
    HWAAlignment alignment;
    char error[HWA_ERROR_SIZE];
    int result;

    memset(&alignment, 0, sizeof(alignment));
    result = hwa_alignment_file_read(path, limits, &alignment,
                                     error, sizeof(error));
    hwa_alignment_free(&alignment);
    return result != 0 &&
           (error_part == NULL || strstr(error, error_part) != NULL);
}

static void test_make_alignment(HWAAlignment *alignment,
                                HWAAlignmentAnchor anchors[2],
                                HWAAlignmentMatch matches[2],
                                HWAUnmatchedSpan spans[2],
                                HWAAlignmentWarning warnings[2],
                                char score_path[7])
{
    memset(alignment, 0, sizeof(*alignment));
    memset(anchors, 0, 2U * sizeof(*anchors));
    memset(matches, 0, 2U * sizeof(*matches));
    memset(spans, 0, 2U * sizeof(*spans));
    memset(warnings, 0, 2U * sizeof(*warnings));
    hwa_alignment_options_default(&alignment->options);
    alignment->options.analysis.channel_mode = HWA_CHANNEL_SELECT;
    alignment->options.analysis.selected_channel = 2U;
    alignment->options.analysis.decode_block_frames = 2048U;
    alignment->options.analysis.frame_size = 1024U;
    alignment->options.analysis.hop_size = 256U;
    alignment->options.analysis.silence_threshold_dbfs = -48.0;
    alignment->options.analysis.max_input_bytes = UINT64_C(123456789);
    alignment->options.analysis.max_input_frames = UINT64_C(7654321);
    alignment->options.analysis.max_work_bytes = UINT64_C(33554432);
    alignment->options.analysis.max_transforms = 12345U;
    alignment->options.analysis.max_track_points = 23456U;
    alignment->options.analysis.max_spectrum_values = 34567U;
    alignment->options.analysis.max_lag_samples = 64U;
    alignment->options.analysis.true_peak_oversample = 1U;
    alignment->options.alignment_step_seconds = 0.125;
    alignment->options.coarse_step_seconds = 0.5;
    alignment->options.dtw_band_seconds = 16.0;
    alignment->options.fine_radius_seconds = 1.0;
    alignment->options.refine_radius_seconds = 0.125;
    alignment->options.match_threshold = 0.5;
    alignment->options.chroma_weight = 0.5;
    alignment->options.onset_weight = 0.25;
    alignment->options.pitch_weight = 0.125;
    alignment->options.envelope_weight = 0.0625;
    alignment->options.activity_weight = 0.0625;
    alignment->options.skip_cost = 0.5;
    alignment->options.repeat_cost = 0.625;
    alignment->options.ornament_cost = 0.25;
    alignment->options.rest_cost = 0.125;
    alignment->options.cadenza_cost = 0.0625;
    alignment->options.max_dtw_cells = UINT64_C(1234567);
    alignment->options.max_alignment_work_bytes = UINT64_C(33554432);
    alignment->options.max_alignment_points = 3456U;
    alignment->options.max_score_events = 4567U;
    alignment->options.max_manual_anchors = 567U;
    alignment->mode = HWA_ALIGNMENT_SCORE_TO_AUDIO;
    alignment->score_path = score_path;
    alignment->target_path = (char *)"audio.wav";
    (void)strcpy(alignment->score_sha256,
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    (void)strcpy(alignment->target_sha256,
                 "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    alignment->reference_duration_seconds = 1.0;
    alignment->target_duration_seconds = 1.0;
    alignment->tuning_offset_cents = -12.5;
    alignment->tuning_confidence = 0.75;
    alignment->total_cost = 2.5;
    alignment->normalized_cost = 0.125;
    alignment->matched_coverage = 0.875;
    alignment->global_confidence = 0.625;
    alignment->dtw_cells = UINT64_C(1234);
    alignment->path_points = UINT64_C(99);
    alignment->anchors = anchors;
    alignment->anchor_count = 2U;
    anchors[0].id = 2U;
    anchors[0].confidence = 0.9;
    anchors[0].origin = HWA_ALIGNMENT_ORIGIN_AUTO;
    anchors[0].score_beat_valid = 1;
    anchors[1] = anchors[0];
    anchors[1].id = 1U;
    anchors[1].reference_seconds = 1.0;
    anchors[1].target_seconds = 1.0;
    anchors[1].score_beat = 2.0;
    alignment->matches = matches;
    alignment->match_count = 2U;
    matches[0].id = 2U;
    matches[0].reference_end_seconds = 0.5;
    matches[0].target_end_seconds = 0.5;
    matches[0].score_end_beat = 1.0;
    matches[0].confidence = 0.9;
    matches[0].status = HWA_ALIGNMENT_MATCHED;
    matches[0].score_span_valid = 1;
    matches[0].event_id = (char *)"n1";
    matches[0].kind = (char *)"note";
    matches[0].voice = (char *)"v1";
    matches[0].midi_note = (char *)"60";
    matches[0].velocity = (char *)"80";
    matches[0].tie = (char *)"none";
    matches[0].dynamic = (char *)"mf";
    matches[0].score_position = (char *)"m1";
    matches[0].tempo_bpm = 120.0;
    matches[0].tempo_valid = 1;
    matches[1] = matches[0];
    matches[1].id = 1U;
    matches[1].reference_start_seconds = 0.5;
    matches[1].reference_end_seconds = 1.0;
    matches[1].target_start_seconds = 0.5;
    matches[1].target_end_seconds = 1.0;
    matches[1].score_start_beat = 1.0;
    matches[1].score_end_beat = 2.0;
    matches[1].event_id = (char *)"n2";

    alignment->unmatched_spans = spans;
    alignment->unmatched_span_count = 2U;
    spans[0].id = 1U;
    spans[0].side = HWA_ALIGNMENT_TARGET;
    spans[0].start_seconds = 0.75;
    spans[0].end_seconds = 0.8;
    spans[0].reason = HWA_UNMATCHED_LOW_CONFIDENCE;
    spans[0].confidence = 0.5;
    spans[1].id = 2U;
    spans[1].side = HWA_ALIGNMENT_REFERENCE;
    spans[1].start_seconds = 0.25;
    spans[1].end_seconds = 0.3;
    spans[1].start_beat = 0.5;
    spans[1].end_beat = 0.6;
    spans[1].score_span_valid = 1;
    spans[1].reason = HWA_UNMATCHED_SKIP;
    spans[1].confidence = 0.5;

    alignment->warnings = warnings;
    alignment->warning_count = 2U;
    warnings[0].id = 2U;
    warnings[0].code = (char *)"second";
    warnings[0].message = (char *)"second warning";
    warnings[1].id = 1U;
    warnings[1].code = (char *)"first";
    warnings[1].message = (char *)"first warning";
}

static int test_write_alignment(const char *path,
                                const HWAAlignment *alignment)
{
    char error[HWA_ERROR_SIZE];
    FILE *stream = fopen(path, "wb");
    int result;
    if (stream == NULL) return 0;
    result = hwa_alignment_file_write(stream, alignment,
                                      error, sizeof(error));
    if (fclose(stream) != 0) result = -1;
    return result == 0;
}

static void test_full_alignment_reader(const char *directory)
{
    char path[PATH_MAX];
    char roundtrip_path[PATH_MAX];
    char nul_path[PATH_MAX];
    char duplicate_path[PATH_MAX];
    char duplicate_event_path[PATH_MAX];
    char anchor_order_path[PATH_MAX];
    char match_order_path[PATH_MAX];
    char span_order_path[PATH_MAX];
    char warning_order_path[PATH_MAX];
    char score_path[] = "s\xff.csv";
    HWAAlignment source;
    HWAAlignment loaded;
    HWAAlignmentAnchor anchors[2];
    HWAAlignmentMatch matches[2];
    HWAUnmatchedSpan spans[2];
    HWAAlignmentWarning warnings[2];
    HWAAlignmentFileLimits limits;
    char error[HWA_ERROR_SIZE];
    char *bytes;
    char *input;
    size_t size;

    CHECK(test_path(path, directory, "full.hwa-align") &&
              test_path(roundtrip_path, directory,
                        "roundtrip.hwa-align") &&
              test_path(nul_path, directory, "nul.hwa-align") &&
              test_path(duplicate_path, directory,
                        "duplicate-id.hwa-align") &&
              test_path(duplicate_event_path, directory,
                        "duplicate-event.hwa-align") &&
              test_path(anchor_order_path, directory,
                        "anchor-order.hwa-align") &&
              test_path(match_order_path, directory,
                        "match-order.hwa-align") &&
              test_path(span_order_path, directory,
                        "span-order.hwa-align") &&
              test_path(warning_order_path, directory,
                        "warning-order.hwa-align"),
          "cannot make alignment paths");
    test_make_alignment(&source, anchors, matches, spans, warnings,
                        score_path);
    CHECK(test_write_alignment(path, &source),
          "cannot write full-reader fixture");
    hwa_alignment_file_limits_default(&limits);
    memset(&loaded, 0, sizeof(loaded));
    CHECK(hwa_alignment_file_read(path, &limits, &loaded,
                                  error, sizeof(error)) == 0,
          "full reader failed: %s", error);
    CHECK(loaded.mode == HWA_ALIGNMENT_SCORE_TO_AUDIO &&
              loaded.anchor_count == 2U &&
              loaded.anchors[0].id == 2U && loaded.anchors[1].id == 1U &&
              loaded.match_count == 2U &&
              loaded.matches[0].id == 2U && loaded.matches[1].id == 1U &&
              loaded.matches[0].event_id != NULL &&
              strcmp(loaded.matches[0].event_id, "n1") == 0 &&
              loaded.unmatched_span_count == 2U &&
              loaded.unmatched_spans[0].id == 2U &&
              loaded.unmatched_spans[1].id == 1U &&
              loaded.warning_count == 2U &&
              loaded.warnings[0].id == 1U &&
              loaded.warnings[1].id == 2U,
          "full reader lost data or rejected canonical non-ID order");
    CHECK(loaded.options.analysis.collect_tracks == 1,
          "full reader did not restore collected analysis tracks");
    CHECK(loaded.options.analysis.collect_spectrogram == 0,
          "full reader enabled alignment spectrogram collection");
    CHECK(test_alignment_options_equal(&source.options, &loaded.options),
          "full reader did not exactly restore saved alignment options");
    CHECK(loaded.tuning_offset_cents == source.tuning_offset_cents &&
              loaded.tuning_confidence == source.tuning_confidence &&
              loaded.total_cost == source.total_cost &&
              loaded.normalized_cost == source.normalized_cost &&
              loaded.matched_coverage == source.matched_coverage &&
              loaded.global_confidence == source.global_confidence &&
              loaded.dtw_cells == source.dtw_cells &&
              loaded.path_points == source.path_points,
          "full reader did not exactly restore saved result facts");
    CHECK(loaded.score_path != NULL &&
              (unsigned char)loaded.score_path[1] == 0xffU,
          "full reader did not preserve invalid path bytes");
    CHECK(loaded.retained_work_bytes != 0U,
          "full reader did not report retained storage");
    CHECK(test_write_alignment(roundtrip_path, &loaded),
          "cannot rewrite a fully loaded alignment");
    hwa_alignment_free(&loaded);

    memset(&loaded, 0, sizeof(loaded));
    CHECK(hwa_alignment_file_read(roundtrip_path, &limits, &loaded,
                                  error, sizeof(error)) == 0,
          "cannot read the rewritten alignment: %s", error);
    CHECK(test_alignment_options_equal(&source.options, &loaded.options) &&
              loaded.options.analysis.collect_tracks == 1 &&
              loaded.options.analysis.collect_spectrogram == 0,
          "load/write/load changed successful Stage 2 analysis options");
    hwa_alignment_free(&loaded);

    bytes = test_read_bytes(path, &size);
    CHECK(bytes != NULL, "cannot read canonical alignment fixture");
    if (bytes != NULL) {
        CHECK(test_write_swapped_rows(
                  anchor_order_path, bytes, size,
                  "ANCHOR,2,", "ANCHOR,1,"),
              "cannot write swapped-anchor alignment");
        CHECK(test_alignment_rejected(anchor_order_path, &limits,
                                      "non-monotone"),
              "full reader accepted swapped canonical anchors");
        CHECK(test_write_swapped_rows(
                  match_order_path, bytes, size,
                  "MATCH,2,", "MATCH,1,"),
              "cannot write swapped-MATCH alignment");
        CHECK(test_alignment_rejected(match_order_path, &limits,
                                      "canonical"),
              "full reader accepted swapped canonical MATCH rows");
        CHECK(test_write_swapped_rows(
                  span_order_path, bytes, size,
                  "UNMATCHED,2,reference,", "UNMATCHED,1,target,"),
              "cannot write swapped-UNMATCHED alignment");
        CHECK(test_alignment_rejected(span_order_path, &limits,
                                      "canonical"),
              "full reader accepted swapped canonical UNMATCHED rows");
        CHECK(test_write_swapped_rows(
                  warning_order_path, bytes, size,
                  "WARNING,1,", "WARNING,2,"),
              "cannot write swapped-WARNING alignment");
        CHECK(test_alignment_rejected(warning_order_path, &limits,
                                      "canonical"),
              "full reader accepted swapped canonical WARNING rows");
    }

    input = bytes != NULL ? strstr(bytes, ",n2,note,") : NULL;
    CHECK(input != NULL, "cannot find the second MATCH event_id");
    if (input != NULL) {
        input[2] = '1';
        CHECK(test_write_bytes(duplicate_event_path, bytes, size),
              "cannot write duplicate-event alignment");
        CHECK(test_alignment_rejected(duplicate_event_path, &limits,
                                      "event_id"),
              "full reader accepted duplicate MATCH event_id values");
        input[2] = '2';
    }

    input = bytes != NULL ? strstr(bytes, "UNMATCHED,1,target,") : NULL;
    CHECK(input != NULL, "cannot find target UNMATCHED row");
    if (input != NULL) {
        input[strlen("UNMATCHED,")] = '2';
        CHECK(test_write_bytes(duplicate_path, bytes, size),
              "cannot write duplicate-ID alignment");
        memset(&loaded, 0, sizeof(loaded));
        CHECK(hwa_alignment_file_read(duplicate_path, &limits, &loaded,
                                      error, sizeof(error)) != 0 &&
                  strstr(error, "unique and complete") != NULL,
              "full reader accepted duplicate IDs: %s", error);
        hwa_alignment_free(&loaded);
        input[strlen("UNMATCHED,")] = '1';
    }
    input = bytes != NULL ? strstr(bytes, "INPUT,score,") : NULL;
    CHECK(input != NULL, "cannot find score INPUT row");
    if (input != NULL) {
        input += strlen("INPUT,score,");
        input[0] = '0';
        input[1] = '0';
        CHECK(test_write_bytes(nul_path, bytes, size),
              "cannot write embedded-NUL alignment");
        memset(&loaded, 0, sizeof(loaded));
        CHECK(hwa_alignment_file_read(nul_path, &limits, &loaded,
                                      error, sizeof(error)) != 0 &&
                  strstr(error, "embedded NUL") != NULL,
              "full reader accepted a decoded NUL path: %s", error);
        hwa_alignment_free(&loaded);
    }
    free(bytes);
    (void)test_remove_file(warning_order_path);
    (void)test_remove_file(span_order_path);
    (void)test_remove_file(match_order_path);
    (void)test_remove_file(anchor_order_path);
    (void)test_remove_file(duplicate_event_path);
    (void)test_remove_file(duplicate_path);
    (void)test_remove_file(nul_path);
    (void)test_remove_file(roundtrip_path);
    (void)test_remove_file(path);
}

static void test_typed_labels(const char *directory)
{
    char path[PATH_MAX];
    char duplicate_path[PATH_MAX];
    const char text[] =
        HWA_TYPED_LABEL_HEADER "\r\n"
        "n2,G4,high,mf,slur,lead,resonator-b,exciter,sustain,B,blend,vibrato\r\n"
        "n1,C4,low,p,detach,lead,resonator-a,exciter,impulse,A,,\r\n";
    const char duplicate[] =
        HWA_TYPED_LABEL_HEADER "\n"
        "n1,C4,,,,,,,,,,\n"
        "n1,D4,,,,,,,,,,\n";
    HWATypedLabelSet labels;
    const HWATypedLabelRow *row;
    char error[HWA_ERROR_SIZE];

    CHECK(test_path(path, directory, "labels.csv") &&
              test_path(duplicate_path, directory, "duplicate.csv"),
          "cannot make label paths");
    CHECK(test_write_bytes(path, text, sizeof(text) - 1U),
          "cannot write labels");
    memset(&labels, 0, sizeof(labels));
    CHECK(hwa_typed_labels_load(
              path, 65536U, (uint64_t)(sizeof(text) - 1U),
              4U, 4096U, &labels, error, sizeof(error)) != 0 &&
              strstr(error, "limit") != NULL,
          "typed-label reader let its input terminator bypass the work cap: %s",
          error);
    hwa_typed_labels_free(&labels);
    memset(&labels, 0, sizeof(labels));
    CHECK(hwa_typed_labels_load(path, 65536U, 65536U, 4U, 4096U,
                                &labels, error, sizeof(error)) == 0,
          "typed-label reader failed: %s", error);
    row = hwa_typed_labels_find(&labels, "n2");
    CHECK(labels.row_count == 2U && row != NULL &&
              row->labels.transition != NULL &&
              strcmp(row->labels.transition, "blend") == 0 &&
              (row->labels.override_flags &
               HWA_LABEL_OVERRIDE_TRANSITION) != 0U,
          "typed-label reader lost typed data");
    CHECK(labels.retained_work_bytes != 0U,
          "typed-label reader did not report retained storage");
    hwa_typed_labels_free(&labels);

    CHECK(test_write_bytes(duplicate_path, duplicate,
                           sizeof(duplicate) - 1U),
          "cannot write duplicate labels");
    memset(&labels, 0, sizeof(labels));
    CHECK(hwa_typed_labels_load(duplicate_path, 65536U, 65536U, 4U,
                                4096U, &labels, error, sizeof(error)) != 0,
          "typed-label reader accepted a duplicate event_id");
    hwa_typed_labels_free(&labels);
    (void)test_remove_file(duplicate_path);
    (void)test_remove_file(path);
}

static void test_make_item_set(HWAItemSet *set, HWAItem items[5])
{
    size_t index;
    static char keys[5][8] = {"auto-1", "auto-2", "auto-3", "auto-4", "manual"};

    memset(set, 0, sizeof(*set));
    memset(items, 0, 5U * sizeof(*items));
    hwa_segmentation_options_default(&set->options);
    set->alignment_path = (char *)"alignment.hwa-align";
    set->audio_path = (char *)"audio.wav";
    set->source_score_path = (char *)"score.csv";
    (void)strcpy(set->alignment_sha256,
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    (void)strcpy(set->audio_sha256,
                 "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    (void)strcpy(set->source_score_sha256,
                 "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
    set->audio_format.sample_rate_hz = 1000U;
    set->audio_format.frames = 1000U;
    set->audio_format.duration_seconds = 1.0;
    set->source_score_duration_seconds = 1.0;
    set->alignment_confidence = 0.9;
    set->items = items;
    set->item_count = 5U;
    set->locked_item_count = 1U;
    for (index = 0U; index < 5U; ++index) {
        items[index].id = (uint64_t)index + 1U;
        items[index].key = keys[index];
        items[index].role = (char *)"note";
        items[index].kind = HWA_ITEM_NOTE;
        items[index].start_sample = (uint64_t)index * 100U;
        items[index].end_sample = items[index].start_sample + 80U;
        items[index].start_seconds =
            (double)items[index].start_sample / 1000.0;
        items[index].end_seconds =
            (double)items[index].end_sample / 1000.0;
        items[index].score_start_beat = (double)index;
        items[index].score_end_beat = (double)index + 0.5;
        items[index].confidence = 0.9;
        items[index].origin = HWA_ITEM_ORIGIN_AUTO;
    }
    items[4].origin = HWA_ITEM_ORIGIN_MANUAL;
    items[4].locked = 1;
}

static int test_write_items(const char *path, const HWAItemSet *items)
{
    char error[HWA_ERROR_SIZE];
    FILE *stream = fopen(path, "wb");
    int result;
    if (stream == NULL) return 0;
    result = hwa_item_file_write(stream, items, error, sizeof(error));
    if (fclose(stream) != 0) result = -1;
    return result == 0;
}

static const HWAItemEdit *test_find_edit(const HWAItemEditSet *edits,
                                         const char *key)
{
    size_t index;
    for (index = 0U; index < edits->edit_count; ++index) {
        if (strcmp(edits->edits[index].key, key) == 0) {
            return &edits->edits[index];
        }
    }
    return NULL;
}

static void test_item_edits_and_writer_cap(const char *directory)
{
    char path[PATH_MAX];
    char editable_path[PATH_MAX];
    char edited_path[PATH_MAX];
    char duplicate_auto_path[PATH_MAX];
    char duplicate_mixed_path[PATH_MAX];
    HWAItemSet set;
    HWAItem items[5];
    HWAItemFileLimits limits;
    HWAItemEditSet edits;
    const HWAItemEdit *edit;
    char error[HWA_ERROR_SIZE];
    char *bytes;
    char *row;
    char *samples;
    char *lock;
    char *key_field;
    size_t size;
    FILE *stream;
    uint64_t pointer_bytes = 5U * (uint64_t)sizeof(void *);

    CHECK(test_path(path, directory, "items.hwa-items") &&
              test_path(editable_path, directory,
                        "editable.hwa-items") &&
              test_path(edited_path, directory,
                        "edited.hwa-items") &&
              test_path(duplicate_auto_path, directory,
                        "duplicate-auto-key.hwa-items") &&
              test_path(duplicate_mixed_path, directory,
                        "duplicate-mixed-key.hwa-items"),
          "cannot make item path");
    test_make_item_set(&set, items);
    CHECK(test_write_items(path, &set), "cannot write item fixture");
    hwa_item_file_limits_default(&limits);
    limits.max_items = 5U;
    limits.max_manual_items = 1U;
    memset(&edits, 0, sizeof(edits));
    CHECK(hwa_item_file_read_edits(path, &limits, &edits,
                                   error, sizeof(error)) == 0,
          "cannot read manual item: %s", error);
    CHECK(edits.edit_count == 1U &&
              strcmp(edits.edits[0].key, "manual") == 0 &&
              edits.edits[0].locked &&
              edits.edits[0].exclusion_set,
          "auto rows consumed the manual-edit cap");
    CHECK(edits.retained_work_bytes != 0U,
          "item reader did not report retained storage");
    hwa_item_edit_set_free(&edits);

    items[4].locked = 0;
    set.locked_item_count = 0U;
    CHECK(test_write_items(editable_path, &set),
          "cannot write editable item fixture");
    bytes = test_read_bytes(editable_path, &size);
    row = bytes != NULL ? strstr(bytes, "ITEM,5,manual,") : NULL;
    samples = row != NULL ? strstr(row, ",400,480,") : NULL;
    lock = row != NULL ? strstr(row, ",manual,0,0,") : NULL;
    CHECK(samples != NULL && lock != NULL,
          "cannot find editable sample or lock fields");
    key_field = bytes != NULL ? strstr(bytes, "ITEM,2,auto-2,") : NULL;
    CHECK(key_field != NULL, "cannot find an automatic item key");
    if (key_field != NULL) {
        key_field += strlen("ITEM,2,");
        memcpy(key_field, "auto-1", strlen("auto-1"));
        CHECK(test_write_bytes(duplicate_auto_path, bytes, size),
              "cannot write duplicate automatic item keys");
        memset(&edits, 0, sizeof(edits));
        CHECK(hwa_item_file_read_edits(
                  duplicate_auto_path, &limits, &edits,
                  error, sizeof(error)) != 0 &&
                  strstr(error, "repeats key") != NULL,
              "reader accepted duplicate automatic item keys: %s", error);
        hwa_item_edit_set_free(&edits);
        memcpy(key_field, "auto-2", strlen("auto-2"));
    }
    key_field = bytes != NULL ? strstr(bytes, "ITEM,1,auto-1,") : NULL;
    CHECK(key_field != NULL, "cannot find the first automatic item key");
    if (key_field != NULL) {
        key_field += strlen("ITEM,1,");
        memcpy(key_field, "manual", strlen("manual"));
        CHECK(test_write_bytes(duplicate_mixed_path, bytes, size),
              "cannot write mixed duplicate item keys");
        memset(&edits, 0, sizeof(edits));
        CHECK(hwa_item_file_read_edits(
                  duplicate_mixed_path, &limits, &edits,
                  error, sizeof(error)) != 0 &&
                  strstr(error, "repeats key") != NULL,
              "reader accepted an automatic key that duplicates an edit: %s",
              error);
        hwa_item_edit_set_free(&edits);
        memcpy(key_field, "auto-1", strlen("auto-1"));
    }
    if (samples != NULL && lock != NULL) {
        samples[2] = '1';
        samples[6] = '7';
        lock[strlen(",manual,")] = '1';
        CHECK(test_write_bytes(edited_path, bytes, size),
              "cannot write samples-only item amendment");
        memset(&edits, 0, sizeof(edits));
        CHECK(hwa_item_file_read_edits(edited_path, &limits, &edits,
                                       error, sizeof(error)) == 0,
              "samples-only item amendment failed: %s", error);
        edit = test_find_edit(&edits, "manual");
        CHECK(edits.edit_count == 1U && edit != NULL && edit->locked &&
                  edit->start_sample == 410U && edit->end_sample == 470U,
              "reader did not use authoritative edited samples");
        hwa_item_edit_set_free(&edits);
    }
    free(bytes);
    items[4].locked = 1;
    set.locked_item_count = 1U;

    items[0].excluded = 1;
    items[0].exclusion_reason = (char *)"manual reject";
    items[1].origin = HWA_ITEM_ORIGIN_MANUAL;
    CHECK(test_write_items(path, &set),
          "cannot write stale-count exclusion amendment fixture");
    limits.max_manual_items = 3U;
    memset(&edits, 0, sizeof(edits));
    CHECK(hwa_item_file_read_edits(path, &limits, &edits,
                                   error, sizeof(error)) == 0,
          "cannot read exclusion amendments with stale META: %s", error);
    edit = test_find_edit(&edits, "auto-1");
    CHECK(edits.edit_count == 3U && edit != NULL &&
              !edit->locked && edit->start_sample == 0U &&
              edit->end_sample == 0U && edit->exclusion_set &&
              edit->excluded && edit->exclusion_reason != NULL &&
              strcmp(edit->exclusion_reason, "manual reject") == 0,
          "exclusion-only edit retained bounds or lost its reason");
    edit = test_find_edit(&edits, "auto-2");
    CHECK(edit != NULL && !edit->locked &&
              edit->start_sample == 0U && edit->end_sample == 0U &&
              edit->exclusion_set && !edit->excluded &&
              edit->exclusion_reason == NULL,
          "clear-exclusion edit did not remain an unlocked clear");
    edit = test_find_edit(&edits, "manual");
    CHECK(edit != NULL && edit->locked &&
              edit->start_sample == items[4].start_sample &&
              edit->end_sample == items[4].end_sample,
          "locked edit lost its saved bounds");
    hwa_item_edit_set_free(&edits);

    (void)strcpy(set.amendment_sha256,
                 "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
    stream = tmpfile();
    CHECK(stream != NULL &&
              hwa_item_file_write(stream, &set,
                                  error, sizeof(error)) != 0,
          "writer dropped an amendment hash without an amendment path");
    if (stream != NULL) (void)fclose(stream);
    set.amendment_sha256[0] = '\0';

    set.options.max_segmentation_work_bytes = 4096U;
    set.retained_work_bytes =
        set.options.max_segmentation_work_bytes - pointer_bytes;
    stream = tmpfile();
    CHECK(stream != NULL &&
              hwa_item_file_write(stream, &set,
                                  error, sizeof(error)) == 0,
          "writer rejected the exact remaining pointer budget: %s", error);
    if (stream != NULL) (void)fclose(stream);
    set.retained_work_bytes++;
    stream = tmpfile();
    CHECK(stream != NULL &&
              hwa_item_file_write(stream, &set,
                                  error, sizeof(error)) != 0,
          "writer exceeded the command-wide work cap");
    if (stream != NULL) (void)fclose(stream);
    (void)test_remove_file(edited_path);
    (void)test_remove_file(duplicate_mixed_path);
    (void)test_remove_file(duplicate_auto_path);
    (void)test_remove_file(editable_path);
    (void)test_remove_file(path);
}

int main(void)
{
    char directory[PATH_MAX];
    if (!test_workspace(directory)) {
        (void)fprintf(stderr, "cannot create test workspace\n");
        return 1;
    }
    test_full_alignment_reader(directory);
    test_typed_labels(directory);
    test_item_edits_and_writer_cap(directory);
    CHECK(test_remove_directory(directory) == 0,
          "cannot remove test workspace");
    return failures == 0 ? 0 : 1;
}

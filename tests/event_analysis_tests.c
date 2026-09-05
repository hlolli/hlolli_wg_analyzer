#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "event_analysis.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#define TEST_MKDIR(path) _mkdir(path)
#define TEST_RMDIR(path) _rmdir(path)
#define TEST_UNLINK(path) _unlink(path)
#define TEST_PID() ((long)_getpid())
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define TEST_MKDIR(path) mkdir((path), 0700)
#define TEST_RMDIR(path) rmdir(path)
#define TEST_UNLINK(path) unlink(path)
#define TEST_PID() ((long)getpid())
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TEST_RATE UINT32_C(8000)
#define TEST_FRAMES UINT32_C(16000)
#define TEST_FRAME_SIZE 512U
#define TEST_HOP_SIZE 128U
#define TEST_TRACE_POINTS 122U
#define TEST_TRACE_COUNT 9U
#define TEST_BOUND_TOLERANCE UINT64_C(640)
#define TEST_EVENT_LEVEL_TOLERANCE 0.50

#define TEST_FIRST_START UINT64_C(2560)
#define TEST_FIRST_END UINT64_C(6400)
#define TEST_SECOND_START UINT64_C(7680)
#define TEST_SECOND_END UINT64_C(12800)
#define TEST_CONTIGUOUS_BOUNDARY UINT64_C(8000)
#define TEST_SHORT_FRAMES UINT32_C(256)
#define TEST_GAP_FRAME_SIZE 256U
#define TEST_GAP_START UINT64_C(8192)
#define TEST_GAP_END (TEST_GAP_START + TEST_GAP_FRAME_SIZE)
#define TEST_GAP_POINT_COUNT UINT64_C(62)

static const char *const test_trace_names[TEST_TRACE_COUNT] = {
    "rms-dbfs",
    "pitch-hz",
    "pitch-confidence",
    "pitch-valid",
    "onset-strength",
    "spectral-centroid-hz",
    "spectral-rolloff-85-hz",
    "spectral-flatness",
    "spectrum-valid"
};

static int failures;

#define CHECK(condition, ...)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            (void)fprintf(stderr, __VA_ARGS__);                              \
            (void)fputc('\n', stderr);                                       \
            failures++;                                                      \
        }                                                                    \
    } while (0)

typedef enum TestSignal {
    TEST_SIGNAL_SILENCE = 0,
    TEST_SIGNAL_TWO_TONES = 1,
    TEST_SIGNAL_STEADY_TONE = 2,
    TEST_SIGNAL_CONTIGUOUS_TONES = 3,
    TEST_SIGNAL_ONE_FRAME_GAP = 4
} TestSignal;

static int join_path(char path[PATH_MAX],
                     const char *directory,
                     const char *name)
{
    int length = snprintf(path, PATH_MAX, "%s/%s", directory, name);
    return length >= 0 && (size_t)length < PATH_MAX;
}

static int make_workspace(char directory[PATH_MAX])
{
    unsigned attempt;
#if defined(_WIN32)
    const char *root = getenv("TEMP");
    if (root == NULL || root[0] == '\0') root = ".";
#else
    const char *root = "/tmp";
#endif
    for (attempt = 0U; attempt < 100U; ++attempt) {
        int length = snprintf(directory, PATH_MAX,
                              "%s/hwa-event-analysis-%ld-%u",
                              root, TEST_PID(), attempt);
        if (length < 0 || (size_t)length >= PATH_MAX) return 0;
        if (TEST_MKDIR(directory) == 0) return 1;
        if (errno != EEXIST) return 0;
    }
    return 0;
}

static int path_exists(const char *path)
{
#if defined(_WIN32)
    struct _stat64 status;
    return _stat64(path, &status) == 0;
#else
    struct stat status;
    return stat(path, &status) == 0;
#endif
}

static int write_bytes(FILE *stream, const void *bytes, size_t size)
{
    return fwrite(bytes, 1U, size, stream) == size;
}

static int write_u16(FILE *stream, uint16_t value)
{
    unsigned char bytes[2];
    bytes[0] = (unsigned char)(value & UINT16_C(0xff));
    bytes[1] = (unsigned char)(value >> 8U);
    return write_bytes(stream, bytes, sizeof(bytes));
}

static int write_u32(FILE *stream, uint32_t value)
{
    unsigned char bytes[4];
    bytes[0] = (unsigned char)(value & UINT32_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT32_C(0xff));
    bytes[2] = (unsigned char)((value >> 16U) & UINT32_C(0xff));
    bytes[3] = (unsigned char)(value >> 24U);
    return write_bytes(stream, bytes, sizeof(bytes));
}

static int16_t signal_sample(uint32_t frame, TestSignal signal)
{
    static const int16_t first_cycle[16] = {
        16384, 15137, 11585, 6270, 0, -6270, -11585, -15137,
        -16384, -15137, -11585, -6270, 0, 6270, 11585, 15137
    };
    static const int16_t second_cycle[8] = {
        8192, 5793, 0, -5793, -8192, -5793, 0, 5793
    };
    static const int16_t second_full_cycle[8] = {
        16384, 11585, 0, -11585, -16384, -11585, 0, 11585
    };
    if (signal == TEST_SIGNAL_STEADY_TONE ||
        signal == TEST_SIGNAL_ONE_FRAME_GAP) {
        if (signal == TEST_SIGNAL_ONE_FRAME_GAP &&
            (uint64_t)frame >= TEST_GAP_START &&
            (uint64_t)frame < TEST_GAP_END)
            return 0;
        return first_cycle[frame % 16U];
    }
    if (signal == TEST_SIGNAL_CONTIGUOUS_TONES) {
        if ((uint64_t)frame < TEST_CONTIGUOUS_BOUNDARY)
            return first_cycle[frame % 16U];
        return second_full_cycle[
            (frame - (uint32_t)TEST_CONTIGUOUS_BOUNDARY) % 8U];
    }
    if (signal == TEST_SIGNAL_TWO_TONES &&
        (uint64_t)frame >= TEST_FIRST_START &&
        (uint64_t)frame < TEST_FIRST_END) {
        return first_cycle[(frame - (uint32_t)TEST_FIRST_START) % 16U];
    }
    if (signal == TEST_SIGNAL_TWO_TONES &&
        (uint64_t)frame >= TEST_SECOND_START &&
        (uint64_t)frame < TEST_SECOND_END) {
        return second_cycle[(frame - (uint32_t)TEST_SECOND_START) % 8U];
    }
    return 0;
}

static int write_wave_frames(const char *path,
                             TestSignal signal,
                             uint32_t frame_count)
{
    const uint32_t data_bytes = frame_count * UINT32_C(2);
    FILE *stream = fopen(path, "wb");
    uint32_t frame;
    if (stream == NULL) return 0;
    if (!write_bytes(stream, "RIFF", 4U) ||
        !write_u32(stream, UINT32_C(36) + data_bytes) ||
        !write_bytes(stream, "WAVEfmt ", 8U) ||
        !write_u32(stream, UINT32_C(16)) ||
        !write_u16(stream, UINT16_C(1)) ||
        !write_u16(stream, UINT16_C(1)) ||
        !write_u32(stream, TEST_RATE) ||
        !write_u32(stream, TEST_RATE * UINT32_C(2)) ||
        !write_u16(stream, UINT16_C(2)) ||
        !write_u16(stream, UINT16_C(16)) ||
        !write_bytes(stream, "data", 4U) ||
        !write_u32(stream, data_bytes)) {
        (void)fclose(stream);
        return 0;
    }
    for (frame = 0U; frame < frame_count; ++frame) {
        if (!write_u16(stream, (uint16_t)signal_sample(frame, signal))) {
            (void)fclose(stream);
            return 0;
        }
    }
    return fclose(stream) == 0;
}

static int write_wave(const char *path, TestSignal signal)
{
    return write_wave_frames(path, signal, TEST_FRAMES);
}

static void test_options(HWAEventAnalysisOptions *options)
{
    hwa_event_analysis_options_default(options);
    options->analysis.decode_block_frames = 257U;
    options->analysis.frame_size = TEST_FRAME_SIZE;
    options->analysis.hop_size = TEST_HOP_SIZE;
    options->analysis.silence_threshold_dbfs = -60.0;
    options->min_pitch_confidence = 0.70;
    options->pitch_split_semitones = 6.0;
    options->onset_split_strength = 0.95;
    options->min_note_points = 3U;
    options->max_gap_points = 2U;
}

static void edge_test_options(HWAEventAnalysisOptions *options)
{
    hwa_event_analysis_options_default(options);
    options->analysis.frame_size = TEST_FRAME_SIZE;
    options->analysis.hop_size = TEST_HOP_SIZE;
}

static const HWAEventTrace *find_trace(const HWAEventBundle *bundle,
                                       const char *name)
{
    const HWAEventTrace *found = NULL;
    size_t index;
    for (index = 0U; index < bundle->trace_count; ++index) {
        if (bundle->traces[index].name != NULL &&
            strcmp(bundle->traces[index].name, name) == 0) {
            if (found != NULL) return NULL;
            found = &bundle->traces[index];
        }
    }
    return found;
}

static const HWAEventValue *find_selected_number(
    const HWAPerformanceEvent *event,
    const char *name)
{
    const HWAEventValue *found = NULL;
    size_t index;
    for (index = 0U; index < event->value_count; ++index) {
        const HWAEventValue *value = &event->values[index];
        if (value->name != NULL && strcmp(value->name, name) == 0 &&
            value->selected && value->kind == HWA_EVENT_VALUE_F64) {
            if (found != NULL) return NULL;
            found = value;
        }
    }
    return found;
}

static uint64_t sample_distance(uint64_t first, uint64_t second)
{
    return first > second ? first - second : second - first;
}

static int pitch_near(double actual, double expected)
{
    return actual > 0.0 && isfinite(actual) &&
           fabs(1200.0 * log2(actual / expected)) <= 20.0;
}

static int event_refers_to(const HWAPerformanceEvent *event, uint64_t trace_id)
{
    size_t index;
    size_t count = 0U;
    for (index = 0U; index < event->trace_ref_count; ++index) {
        if (event->trace_refs[index].trace_id == trace_id) count++;
    }
    return count == 1U;
}

static int read_f64le_trace(const char *directory,
                            const HWAEventTrace *trace,
                            double **out_values)
{
    char path[PATH_MAX];
    FILE *stream = NULL;
    double *values = NULL;
    size_t point;
    int result = 0;
    *out_values = NULL;
    if (sizeof(double) != 8U || trace == NULL ||
        trace->format != HWA_EVENT_TRACE_F64LE ||
        trace->value_width != 1U || trace->point_count > (uint64_t)SIZE_MAX ||
        !join_path(path, directory, trace->relative_path)) return 0;
    if ((size_t)trace->point_count > SIZE_MAX / sizeof(*values)) return 0;
    values = (double *)malloc((size_t)trace->point_count * sizeof(*values));
    stream = fopen(path, "rb");
    if (values == NULL || stream == NULL) goto cleanup;
    for (point = 0U; point < (size_t)trace->point_count; ++point) {
        unsigned char bytes[8];
        uint64_t bits;
        if (fread(bytes, 1U, sizeof(bytes), stream) != sizeof(bytes))
            goto cleanup;
        bits = (uint64_t)bytes[0] |
               (uint64_t)bytes[1] << 8U |
               (uint64_t)bytes[2] << 16U |
               (uint64_t)bytes[3] << 24U |
               (uint64_t)bytes[4] << 32U |
               (uint64_t)bytes[5] << 40U |
               (uint64_t)bytes[6] << 48U |
               (uint64_t)bytes[7] << 56U;
        memcpy(&values[point], &bits, sizeof(bits));
        if (!isfinite(values[point])) goto cleanup;
    }
    if (fgetc(stream) != EOF || ferror(stream)) goto cleanup;
    result = 1;
cleanup:
    if (stream != NULL && fclose(stream) != 0) result = 0;
    if (!result) {
        free(values);
        values = NULL;
    }
    *out_values = values;
    return result;
}

static int files_equal(const char *first, const char *second)
{
    FILE *left = fopen(first, "rb");
    FILE *right = fopen(second, "rb");
    int result = left != NULL && right != NULL;
    if (!result) goto cleanup;
    for (;;) {
        unsigned char left_bytes[4096];
        unsigned char right_bytes[4096];
        size_t left_count = fread(left_bytes, 1U, sizeof(left_bytes), left);
        size_t right_count = fread(right_bytes, 1U, sizeof(right_bytes), right);
        if (left_count != right_count ||
            memcmp(left_bytes, right_bytes, left_count) != 0) {
            result = 0;
            break;
        }
        if (left_count < sizeof(left_bytes)) {
            if (ferror(left) || ferror(right)) result = 0;
            break;
        }
    }
cleanup:
    if (left != NULL && fclose(left) != 0) result = 0;
    if (right != NULL && fclose(right) != 0) result = 0;
    return result;
}

static void remove_bundle(const char *directory,
                          const HWAEventBundle *bundle)
{
    static const char *const roots[] = {
        "manifest.json", "events.jsonl", "traces.jsonl"
    };
    char path[PATH_MAX];
    size_t index;
    if (directory == NULL || directory[0] == '\0') return;
    if (bundle != NULL) {
        for (index = 0U; index < bundle->trace_count; ++index) {
            if (bundle->traces[index].relative_path != NULL &&
                join_path(path, directory,
                          bundle->traces[index].relative_path))
                (void)TEST_UNLINK(path);
        }
        for (index = 0U; index < bundle->audio_count; ++index) {
            if (bundle->audio[index].relative_path != NULL &&
                bundle->audio[index].relative_path[0] != '\0' &&
                join_path(path, directory,
                          bundle->audio[index].relative_path))
                (void)TEST_UNLINK(path);
        }
    }
    for (index = 0U; index < sizeof(roots) / sizeof(roots[0]); ++index) {
        if (join_path(path, directory, roots[index])) (void)TEST_UNLINK(path);
    }
    if (join_path(path, directory, "traces")) (void)TEST_RMDIR(path);
    if (join_path(path, directory, "audio")) (void)TEST_RMDIR(path);
    (void)TEST_RMDIR(directory);
}

static void check_trace_contract(const HWAEventBundle *bundle)
{
    size_t name_index;
    CHECK(bundle->trace_count == TEST_TRACE_COUNT,
          "expected %u traces, got %zu",
          (unsigned)TEST_TRACE_COUNT, bundle->trace_count);
    for (name_index = 0U; name_index < TEST_TRACE_COUNT; ++name_index) {
        const HWAEventTrace *trace = find_trace(bundle,
                                                test_trace_names[name_index]);
        CHECK(trace != NULL, "missing unique %s trace",
              test_trace_names[name_index]);
        if (trace == NULL) continue;
        CHECK(trace->format == HWA_EVENT_TRACE_F64LE &&
                  trace->first_sample == 0U &&
                  trace->hop_samples == TEST_HOP_SIZE &&
                  trace->window_samples == TEST_FRAME_SIZE &&
                  trace->point_count == TEST_TRACE_POINTS &&
                  trace->value_width == 1U &&
                  trace->file_bytes == TEST_TRACE_POINTS * UINT64_C(8),
              "%s trace has the wrong format or clock",
              test_trace_names[name_index]);
    }
}

static int event_value_has_contract(const HWAEventValue *value,
                                    const char *unit,
                                    HWAEventValueBasis basis,
                                    int score_valid)
{
    return value != NULL && value->unit != NULL &&
           strcmp(value->unit, unit) == 0 &&
           value->basis == basis && isfinite(value->number) &&
           value->score_valid == score_valid &&
           (!score_valid || isfinite(value->score)) &&
           value->provider_id_valid && value->provider_id == UINT64_C(1) &&
           value->selected;
}

static void check_event(const HWAEventBundle *bundle,
                        const HWAPerformanceEvent *event,
                        uint64_t id,
                        uint64_t expected_start,
                        uint64_t expected_end,
                        double expected_pitch,
                        double expected_level)
{
    const HWAEventValue *pitch = find_selected_number(event, "pitch-hz");
    const HWAEventValue *level = find_selected_number(event, "rms-dbfs");
    const HWAEventValue *onset = find_selected_number(
        event, "onset-strength");
    const HWAEventValue *centroid = find_selected_number(
        event, "spectral-centroid-hz");
    const HWAEventValue *rolloff = find_selected_number(
        event, "spectral-rolloff-85-hz");
    const HWAEventValue *flatness = find_selected_number(
        event, "spectral-flatness");
    size_t trace_index;
    CHECK(event->id == id && event->kind != NULL &&
              strcmp(event->kind, "note") == 0,
          "event %llu has the wrong ID or kind",
          (unsigned long long)id);
    CHECK(sample_distance(event->start_sample, expected_start) <=
                  TEST_BOUND_TOLERANCE &&
              sample_distance(event->end_sample, expected_end) <=
                  TEST_BOUND_TOLERANCE &&
              event->start_sample < event->end_sample,
          "event %llu bounds are [%llu,%llu), expected near [%llu,%llu)",
          (unsigned long long)id,
          (unsigned long long)event->start_sample,
          (unsigned long long)event->end_sample,
          (unsigned long long)expected_start,
          (unsigned long long)expected_end);
    CHECK(event->value_count == 6U,
          "event %llu has %zu values, expected six",
          (unsigned long long)id, event->value_count);
    CHECK(event_value_has_contract(pitch, "Hz", HWA_EVENT_INFERENCE, 1) &&
              pitch_near(pitch->number, expected_pitch) &&
              pitch->score >= 0.70 && pitch->score <= 1.0,
          "event %llu has no dependable selected pitch (actual %.9g)",
          (unsigned long long)id,
          pitch != NULL ? pitch->number : 0.0);
    CHECK(event_value_has_contract(level, "dBFS",
                                   HWA_EVENT_OBSERVATION, 0) &&
              fabs(level->number - expected_level) <=
                  TEST_EVENT_LEVEL_TOLERANCE,
          "event %llu has the wrong selected RMS level (actual %.9g)",
          (unsigned long long)id,
          level != NULL ? level->number : 0.0);
    CHECK(event_value_has_contract(onset, "ratio",
                                   HWA_EVENT_OBSERVATION, 0) &&
              onset->number >= 0.0 && onset->number <= 1.0,
          "event %llu has invalid onset strength (actual %.9g)",
          (unsigned long long)id,
          onset != NULL ? onset->number : 0.0);
    CHECK(event_value_has_contract(centroid, "Hz",
                                   HWA_EVENT_OBSERVATION, 0) &&
              centroid->number >= 0.0 &&
              centroid->number <= (double)TEST_RATE * 0.5,
          "event %llu has invalid spectral centroid (actual %.9g)",
          (unsigned long long)id,
          centroid != NULL ? centroid->number : 0.0);
    CHECK(event_value_has_contract(rolloff, "Hz",
                                   HWA_EVENT_OBSERVATION, 0) &&
              rolloff->number >= 0.0 &&
              rolloff->number <= (double)TEST_RATE * 0.5,
          "event %llu has invalid spectral rolloff (actual %.9g)",
          (unsigned long long)id,
          rolloff != NULL ? rolloff->number : 0.0);
    CHECK(event_value_has_contract(flatness, "ratio",
                                   HWA_EVENT_OBSERVATION, 0) &&
              flatness->number >= 0.0 && flatness->number <= 1.0,
          "event %llu has invalid spectral flatness (actual %.9g)",
          (unsigned long long)id,
          flatness != NULL ? flatness->number : 0.0);
    CHECK(event->trace_ref_count == TEST_TRACE_COUNT,
          "event %llu has %zu trace refs, expected %u",
          (unsigned long long)id, event->trace_ref_count,
          (unsigned)TEST_TRACE_COUNT);
    for (trace_index = 0U; trace_index < bundle->trace_count; ++trace_index) {
        CHECK(event_refers_to(event, bundle->traces[trace_index].id),
              "event %llu does not refer once to trace %llu",
              (unsigned long long)id,
              (unsigned long long)bundle->traces[trace_index].id);
    }
}

static void check_trace_values(const char *directory,
                               const HWAEventBundle *bundle)
{
    const HWAEventTrace *level_trace = find_trace(bundle, "rms-dbfs");
    const HWAEventTrace *pitch_trace = find_trace(bundle, "pitch-hz");
    const HWAEventTrace *confidence_trace = find_trace(
        bundle, "pitch-confidence");
    const HWAEventTrace *valid_trace = find_trace(bundle, "pitch-valid");
    const HWAEventTrace *onset_trace = find_trace(bundle, "onset-strength");
    const HWAEventTrace *centroid_trace = find_trace(
        bundle, "spectral-centroid-hz");
    const HWAEventTrace *rolloff_trace = find_trace(
        bundle, "spectral-rolloff-85-hz");
    const HWAEventTrace *flatness_trace = find_trace(
        bundle, "spectral-flatness");
    const HWAEventTrace *spectrum_valid_trace = find_trace(
        bundle, "spectrum-valid");
    double *levels = NULL;
    double *pitches = NULL;
    double *confidence = NULL;
    double *validity = NULL;
    double *onsets = NULL;
    double *centroids = NULL;
    double *rolloffs = NULL;
    double *flatness = NULL;
    double *spectrum_validity = NULL;
    size_t first_voiced = 0U;
    size_t second_voiced = 0U;
    size_t point;
    CHECK(read_f64le_trace(directory, level_trace, &levels),
          "cannot read rms-dbfs trace");
    CHECK(read_f64le_trace(directory, pitch_trace, &pitches),
          "cannot read pitch-hz trace");
    CHECK(read_f64le_trace(directory, confidence_trace, &confidence),
          "cannot read pitch-confidence trace");
    CHECK(read_f64le_trace(directory, valid_trace, &validity),
          "cannot read pitch-valid trace");
    CHECK(read_f64le_trace(directory, onset_trace, &onsets),
          "cannot read onset-strength trace");
    CHECK(read_f64le_trace(directory, centroid_trace, &centroids),
          "cannot read spectral-centroid-hz trace");
    CHECK(read_f64le_trace(directory, rolloff_trace, &rolloffs),
          "cannot read spectral-rolloff-85-hz trace");
    CHECK(read_f64le_trace(directory, flatness_trace, &flatness),
          "cannot read spectral-flatness trace");
    CHECK(read_f64le_trace(directory, spectrum_valid_trace,
                           &spectrum_validity),
          "cannot read spectrum-valid trace");
    if (levels == NULL || pitches == NULL || confidence == NULL ||
        validity == NULL || onsets == NULL || centroids == NULL ||
        rolloffs == NULL || flatness == NULL || spectrum_validity == NULL)
        goto cleanup;
    for (point = 0U; point < TEST_TRACE_POINTS; ++point) {
        uint64_t start = (uint64_t)point * TEST_HOP_SIZE;
        uint64_t end = start + TEST_FRAME_SIZE;
        int first = start >= TEST_FIRST_START && end <= TEST_FIRST_END;
        int second = start >= TEST_SECOND_START && end <= TEST_SECOND_END;
        int silent = end <= TEST_FIRST_START ||
                     (start >= TEST_FIRST_END && end <= TEST_SECOND_START) ||
                     start >= TEST_SECOND_END;
        CHECK(isfinite(levels[point]) && isfinite(pitches[point]) &&
                  isfinite(confidence[point]) && isfinite(validity[point]) &&
                  isfinite(onsets[point]) && isfinite(centroids[point]) &&
                  isfinite(rolloffs[point]) && isfinite(flatness[point]) &&
                  isfinite(spectrum_validity[point]),
              "trace point %zu contains a non-finite value", point);
        CHECK(validity[point] == 0.0 || validity[point] == 1.0,
              "pitch-valid point %zu is not zero or one", point);
        if (validity[point] == 0.0) {
            CHECK(pitches[point] == 0.0 && confidence[point] == 0.0,
                  "invalid pitch point %zu has nonzero pitch data", point);
        } else {
            CHECK(pitches[point] >= 40.0 && pitches[point] <= 2000.0 &&
                      confidence[point] >= 0.30 &&
                      confidence[point] <= 1.0,
                  "valid pitch point %zu has pitch %.9g and confidence %.9g",
                  point, pitches[point], confidence[point]);
        }
        CHECK(onsets[point] >= 0.0 && onsets[point] <= 1.0,
              "onset-strength point %zu is %.9g", point, onsets[point]);
        CHECK(spectrum_validity[point] == 0.0 ||
                  spectrum_validity[point] == 1.0,
              "spectrum-valid point %zu is not zero or one", point);
        if (spectrum_validity[point] == 0.0) {
            CHECK(centroids[point] == 0.0 && rolloffs[point] == 0.0 &&
                      flatness[point] == 0.0,
                  "invalid spectrum point %zu has nonzero data", point);
        } else {
            CHECK(centroids[point] >= 0.0 &&
                      centroids[point] <= (double)TEST_RATE * 0.5 &&
                      rolloffs[point] >= 0.0 &&
                      rolloffs[point] <= (double)TEST_RATE * 0.5 &&
                      flatness[point] >= 0.0 && flatness[point] <= 1.0,
                  "valid spectrum point %zu is out of range", point);
        }
        if (silent) {
            CHECK(validity[point] == 0.0 && pitches[point] == 0.0 &&
                      levels[point] == -300.0 &&
                      spectrum_validity[point] == 0.0 &&
                      centroids[point] == 0.0 && rolloffs[point] == 0.0 &&
                      flatness[point] == 0.0,
                  "wholly silent trace point %zu is not silent", point);
        }
        if (first) {
            CHECK(fabs(levels[point] - (-9.0309)) <= 0.15,
                  "first-tone level point %zu is %.9g dBFS", point,
                  levels[point]);
            if (validity[point] == 1.0) {
                first_voiced++;
                CHECK(pitch_near(pitches[point], 500.0),
                      "first-tone pitch point %zu is %.9g Hz", point,
                      pitches[point]);
            }
        }
        if (second) {
            CHECK(fabs(levels[point] - (-15.0512)) <= 0.15,
                  "second-tone level point %zu is %.9g dBFS", point,
                  levels[point]);
            if (validity[point] == 1.0) {
                second_voiced++;
                CHECK(pitch_near(pitches[point], 1000.0),
                      "second-tone pitch point %zu is %.9g Hz", point,
                      pitches[point]);
            }
        }
    }
    CHECK(first_voiced >= 21U,
          "only %zu of 27 full first-tone windows have pitch", first_voiced);
    CHECK(second_voiced >= 29U,
          "only %zu of 37 full second-tone windows have pitch", second_voiced);
cleanup:
    free(levels);
    free(pitches);
    free(confidence);
    free(validity);
    free(onsets);
    free(centroids);
    free(rolloffs);
    free(flatness);
    free(spectrum_validity);
}

static void test_two_tones_and_determinism(const char *workspace)
{
    HWAEventAnalysisOptions options;
    HWAEventBundleLimits limits;
    HWAEventBundle first;
    HWAEventBundle second;
    char input[PATH_MAX] = {0};
    char first_directory[PATH_MAX] = {0};
    char second_directory[PATH_MAX] = {0};
    char first_path[PATH_MAX];
    char second_path[PATH_MAX];
    char error[HWA_ERROR_SIZE] = {0};
    size_t trace_index;
    int first_ready = 0;
    int second_ready = 0;
    int initial_failures = failures;
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    CHECK(join_path(input, workspace, "two-tones.wav") &&
              join_path(first_directory, workspace, "first.hwa-events") &&
              join_path(second_directory, workspace, "second.hwa-events") &&
              write_wave(input, TEST_SIGNAL_TWO_TONES),
          "cannot make two-tone fixture");
    if (failures != initial_failures) goto cleanup;
    test_options(&options);
    CHECK(hwa_analyze_events_wav(input, first_directory, &options,
                                 error, sizeof(error)) == 0,
          "first event analysis failed: %s", error);
    CHECK(hwa_analyze_events_wav(input, second_directory, &options,
                                 error, sizeof(error)) == 0,
          "second event analysis failed: %s", error);
    hwa_event_bundle_limits_default(&limits);
    if (hwa_event_bundle_read(first_directory, &limits, &first,
                              error, sizeof(error)) == 0) {
        first_ready = 1;
    } else {
        CHECK(0, "cannot read first event bundle: %s", error);
    }
    if (hwa_event_bundle_read(second_directory, &limits, &second,
                              error, sizeof(error)) == 0) {
        second_ready = 1;
    } else {
        CHECK(0, "cannot read second event bundle: %s", error);
    }
    if (!first_ready || !second_ready) goto cleanup;
    CHECK(first.audio_count == 1U &&
              first.audio[0].kind == HWA_EVENT_SOURCE_RECORDING &&
              first.audio[0].format.channels == 1U &&
              first.audio[0].format.sample_rate_hz == TEST_RATE &&
              first.audio[0].format.frames == TEST_FRAMES,
          "event bundle changed source clock facts");
    CHECK(first.event_count == 2U,
          "two separated tones produced %zu events", first.event_count);
    check_trace_contract(&first);
    if (first.event_count == 2U) {
        check_event(&first, &first.events[0], 1U,
                    TEST_FIRST_START, TEST_FIRST_END, 500.0, -9.0309);
        check_event(&first, &first.events[1], 2U,
                    TEST_SECOND_START, TEST_SECOND_END, 1000.0, -15.0512);
        CHECK(first.events[0].end_sample <= first.events[1].start_sample,
              "two separated events overlap");
    }
    check_trace_values(first_directory, &first);
    CHECK(first.trace_count == second.trace_count &&
              first.event_count == second.event_count,
          "repeated analysis changed row counts");
    if (join_path(first_path, first_directory, "manifest.json") &&
        join_path(second_path, second_directory, "manifest.json"))
        CHECK(files_equal(first_path, second_path),
              "repeated analysis changed manifest bytes");
    if (join_path(first_path, first_directory, "events.jsonl") &&
        join_path(second_path, second_directory, "events.jsonl"))
        CHECK(files_equal(first_path, second_path),
              "repeated analysis changed event bytes");
    if (join_path(first_path, first_directory, "traces.jsonl") &&
        join_path(second_path, second_directory, "traces.jsonl"))
        CHECK(files_equal(first_path, second_path),
              "repeated analysis changed trace index bytes");
    for (trace_index = 0U;
         trace_index < first.trace_count && trace_index < second.trace_count;
         ++trace_index) {
        CHECK(first.traces[trace_index].relative_path != NULL &&
                  second.traces[trace_index].relative_path != NULL &&
                  join_path(first_path, first_directory,
                            first.traces[trace_index].relative_path) &&
                  join_path(second_path, second_directory,
                            second.traces[trace_index].relative_path) &&
                  files_equal(first_path, second_path),
              "repeated analysis changed trace payload %zu", trace_index);
    }
cleanup:
    if (first_ready) remove_bundle(first_directory, &first);
    else remove_bundle(first_directory, NULL);
    if (second_ready) remove_bundle(second_directory, &second);
    else remove_bundle(second_directory, NULL);
    hwa_event_bundle_free(&first);
    hwa_event_bundle_free(&second);
    (void)TEST_UNLINK(input);
}

static void test_steady_tone_is_one_event(const char *workspace)
{
    HWAEventAnalysisOptions options;
    HWAEventBundleLimits limits;
    HWAEventBundle bundle;
    char input[PATH_MAX] = {0};
    char directory[PATH_MAX] = {0};
    char error[HWA_ERROR_SIZE] = {0};
    int ready = 0;
    int initial_failures = failures;
    memset(&bundle, 0, sizeof(bundle));
    CHECK(join_path(input, workspace, "steady.wav") &&
              join_path(directory, workspace, "steady.hwa-events") &&
              write_wave(input, TEST_SIGNAL_STEADY_TONE),
          "cannot make steady-tone fixture");
    if (failures != initial_failures) goto cleanup;
    edge_test_options(&options);
    CHECK(hwa_analyze_events_wav(input, directory, &options,
                                 error, sizeof(error)) == 0,
          "steady-tone analysis failed: %s", error);
    hwa_event_bundle_limits_default(&limits);
    if (hwa_event_bundle_read(directory, &limits, &bundle,
                              error, sizeof(error)) == 0) {
        ready = 1;
    } else {
        CHECK(0, "cannot read steady-tone event bundle: %s", error);
        goto cleanup;
    }
    CHECK(bundle.event_count == 1U,
          "steady tone produced %zu events", bundle.event_count);
    CHECK(bundle.warning_count == 0U,
          "steady tone produced %zu warnings", bundle.warning_count);
    check_trace_contract(&bundle);
    if (bundle.event_count == 1U) {
        check_event(&bundle, &bundle.events[0], UINT64_C(1), UINT64_C(0),
                    TEST_FRAMES, 500.0, -9.0309);
    }
cleanup:
    if (ready) remove_bundle(directory, &bundle);
    else remove_bundle(directory, NULL);
    hwa_event_bundle_free(&bundle);
    (void)TEST_UNLINK(input);
}

static void test_contiguous_pitch_change_splits(const char *workspace)
{
    HWAEventAnalysisOptions options;
    HWAEventBundleLimits limits;
    HWAEventBundle bundle;
    const HWAEventValue *first_pitch;
    const HWAEventValue *second_pitch;
    char input[PATH_MAX] = {0};
    char directory[PATH_MAX] = {0};
    char error[HWA_ERROR_SIZE] = {0};
    int ready = 0;
    int initial_failures = failures;
    memset(&bundle, 0, sizeof(bundle));
    CHECK(join_path(input, workspace, "contiguous.wav") &&
              join_path(directory, workspace, "contiguous.hwa-events") &&
              write_wave(input, TEST_SIGNAL_CONTIGUOUS_TONES),
          "cannot make contiguous-tone fixture");
    if (failures != initial_failures) goto cleanup;
    edge_test_options(&options);
    CHECK(hwa_analyze_events_wav(input, directory, &options,
                                 error, sizeof(error)) == 0,
          "contiguous-tone analysis failed: %s", error);
    hwa_event_bundle_limits_default(&limits);
    if (hwa_event_bundle_read(directory, &limits, &bundle,
                              error, sizeof(error)) == 0) {
        ready = 1;
    } else {
        CHECK(0, "cannot read contiguous-tone event bundle: %s", error);
        goto cleanup;
    }
    CHECK(bundle.event_count == 2U,
          "contiguous tones produced %zu events", bundle.event_count);
    CHECK(bundle.warning_count == 0U,
          "contiguous tones produced %zu warnings", bundle.warning_count);
    check_trace_contract(&bundle);
    if (bundle.event_count == 2U) {
        const HWAPerformanceEvent *first = &bundle.events[0];
        const HWAPerformanceEvent *second = &bundle.events[1];
        first_pitch = find_selected_number(first, "pitch-hz");
        second_pitch = find_selected_number(second, "pitch-hz");
        CHECK(first->id == UINT64_C(1) && second->id == UINT64_C(2) &&
                  first->start_sample == UINT64_C(0) &&
                  first->start_sample < first->end_sample &&
                  first->end_sample == second->start_sample &&
                  second->start_sample < second->end_sample &&
                  second->end_sample == TEST_FRAMES,
              "contiguous events have wrong order or bounds");
        CHECK(sample_distance(first->end_sample,
                              TEST_CONTIGUOUS_BOUNDARY) <=
                  TEST_BOUND_TOLERANCE,
              "contiguous split is at sample %llu, expected near %llu",
              (unsigned long long)first->end_sample,
              (unsigned long long)TEST_CONTIGUOUS_BOUNDARY);
        CHECK(first_pitch != NULL && pitch_near(first_pitch->number, 500.0),
              "first contiguous event pitch is %.9g, expected near 500 Hz",
              first_pitch != NULL ? first_pitch->number : 0.0);
        CHECK(second_pitch != NULL &&
                  pitch_near(second_pitch->number, 1000.0),
              "second contiguous event pitch is %.9g, expected near 1000 Hz",
              second_pitch != NULL ? second_pitch->number : 0.0);
    }
cleanup:
    if (ready) remove_bundle(directory, &bundle);
    else remove_bundle(directory, NULL);
    hwa_event_bundle_free(&bundle);
    (void)TEST_UNLINK(input);
}

static void test_short_wave_has_no_points(const char *workspace)
{
    HWAEventAnalysisOptions options;
    HWAEventBundleLimits limits;
    HWAEventBundle bundle;
    char input[PATH_MAX] = {0};
    char directory[PATH_MAX] = {0};
    char error[HWA_ERROR_SIZE] = {0};
    int ready = 0;
    int initial_failures = failures;
    memset(&bundle, 0, sizeof(bundle));
    CHECK(join_path(input, workspace, "short.wav") &&
              join_path(directory, workspace, "short.hwa-events") &&
              write_wave_frames(input, TEST_SIGNAL_STEADY_TONE,
                                TEST_SHORT_FRAMES),
          "cannot make short-WAVE fixture");
    if (failures != initial_failures) goto cleanup;
    edge_test_options(&options);
    CHECK(hwa_analyze_events_wav(input, directory, &options,
                                 error, sizeof(error)) == 0,
          "short-WAVE analysis failed: %s", error);
    hwa_event_bundle_limits_default(&limits);
    if (hwa_event_bundle_read(directory, &limits, &bundle,
                              error, sizeof(error)) == 0) {
        ready = 1;
    } else {
        CHECK(0, "cannot read short-WAVE event bundle: %s", error);
        goto cleanup;
    }
    CHECK(bundle.audio_count == 1U && bundle.audio != NULL &&
              bundle.audio[0].format.frames == TEST_SHORT_FRAMES,
          "short-WAVE bundle has wrong source facts");
    CHECK(bundle.trace_count == 0U,
          "short WAVE produced %zu traces", bundle.trace_count);
    CHECK(bundle.event_count == 0U,
          "short WAVE produced %zu events", bundle.event_count);
    CHECK(bundle.warning_count == 1U && bundle.warnings != NULL &&
              bundle.warnings[0].code != NULL &&
              strcmp(bundle.warnings[0].code, "no-active-events") == 0,
          "short WAVE did not produce one no-active-events warning");
cleanup:
    if (ready) remove_bundle(directory, &bundle);
    else remove_bundle(directory, NULL);
    hwa_event_bundle_free(&bundle);
    (void)TEST_UNLINK(input);
}

static void test_one_frame_gap_is_joined(const char *workspace)
{
    HWAEventAnalysisOptions options;
    HWAEventBundleLimits limits;
    HWAEventBundle bundle;
    const HWAEventTrace *pitch_valid;
    double *validity = NULL;
    char input[PATH_MAX] = {0};
    char directory[PATH_MAX] = {0};
    char error[HWA_ERROR_SIZE] = {0};
    size_t gap_point = (size_t)(TEST_GAP_START / TEST_GAP_FRAME_SIZE);
    int ready = 0;
    int initial_failures = failures;
    memset(&bundle, 0, sizeof(bundle));
    CHECK(join_path(input, workspace, "one-frame-gap.wav") &&
              join_path(directory, workspace, "one-frame-gap.hwa-events") &&
              write_wave(input, TEST_SIGNAL_ONE_FRAME_GAP),
          "cannot make one-frame-gap fixture");
    if (failures != initial_failures) goto cleanup;
    hwa_event_analysis_options_default(&options);
    options.analysis.frame_size = TEST_GAP_FRAME_SIZE;
    options.analysis.hop_size = TEST_GAP_FRAME_SIZE;
    options.onset_split_strength = 1.0;
    CHECK(hwa_analyze_events_wav(input, directory, &options,
                                 error, sizeof(error)) == 0,
          "one-frame-gap analysis failed: %s", error);
    hwa_event_bundle_limits_default(&limits);
    if (hwa_event_bundle_read(directory, &limits, &bundle,
                              error, sizeof(error)) == 0) {
        ready = 1;
    } else {
        CHECK(0, "cannot read one-frame-gap event bundle: %s", error);
        goto cleanup;
    }
    CHECK(bundle.event_count == 1U && bundle.events != NULL &&
              bundle.events[0].start_sample == UINT64_C(0) &&
              bundle.events[0].end_sample == TEST_FRAMES &&
              bundle.events[0].trace_ref_count == TEST_TRACE_COUNT,
          "one inactive frame split or shortened the steady event");
    pitch_valid = find_trace(&bundle, "pitch-valid");
    CHECK(pitch_valid != NULL &&
              pitch_valid->point_count == TEST_GAP_POINT_COUNT &&
              read_f64le_trace(directory, pitch_valid, &validity),
          "cannot read one-frame-gap pitch validity");
    if (validity != NULL) {
        CHECK(gap_point > 0U &&
                  gap_point + 1U < (size_t)TEST_GAP_POINT_COUNT &&
                  validity[gap_point - 1U] == 1.0 &&
                  validity[gap_point] == 0.0 &&
                  validity[gap_point + 1U] == 1.0,
              "fixture did not produce one joined inactive pitch frame");
    }
cleanup:
    free(validity);
    if (ready) remove_bundle(directory, &bundle);
    else remove_bundle(directory, NULL);
    hwa_event_bundle_free(&bundle);
    (void)TEST_UNLINK(input);
}

static void test_segment_and_bundle_work_share_limit(const char *workspace)
{
    HWAEventAnalysisOptions options;
    HWAEventBundleLimits limits;
    HWAEventBundle baseline;
    HWAEventBundle limited;
    char input[PATH_MAX] = {0};
    char baseline_directory[PATH_MAX] = {0};
    char limited_directory[PATH_MAX] = {0};
    char error[HWA_ERROR_SIZE] = {0};
    uint64_t retained_work = 0U;
    int baseline_ready = 0;
    int limited_ready = 0;
    int analysis_result;
    int initial_failures = failures;
    memset(&baseline, 0, sizeof(baseline));
    memset(&limited, 0, sizeof(limited));
    CHECK(join_path(input, workspace, "work-cap.wav") &&
              join_path(baseline_directory, workspace,
                        "work-a.hwa-events") &&
              join_path(limited_directory, workspace,
                        "work-b.hwa-events") &&
              write_wave(input, TEST_SIGNAL_STEADY_TONE),
          "cannot make event work-cap fixture");
    if (failures != initial_failures) goto cleanup;
    edge_test_options(&options);
    CHECK(hwa_analyze_events_wav(input, baseline_directory, &options,
                                 error, sizeof(error)) == 0,
          "baseline work-cap analysis failed: %s", error);
    hwa_event_bundle_limits_default(&limits);
    if (hwa_event_bundle_read(baseline_directory, &limits, &baseline,
                              error, sizeof(error)) == 0) {
        baseline_ready = 1;
        retained_work = baseline.retained_work_bytes;
    } else {
        CHECK(0, "cannot read baseline work-cap bundle: %s", error);
        goto cleanup;
    }
    CHECK(retained_work != 0U,
          "baseline work-cap bundle reported no retained work");
    if (retained_work == 0U) goto cleanup;
    options.bundle_limits.max_work_bytes = retained_work;
    error[0] = '\0';
    analysis_result = hwa_analyze_events_wav(
        input, limited_directory, &options, error, sizeof(error));
    CHECK(analysis_result != 0,
          "event analysis ignored combined segment and bundle work");
    if (analysis_result == 0) {
        if (hwa_event_bundle_read(limited_directory, &limits, &limited,
                                  error, sizeof(error)) == 0)
            limited_ready = 1;
    } else {
        CHECK(strstr(error,
                     "retained bundle and segment work exceed limit") != NULL,
              "combined work failure reported the wrong error: %s", error);
        CHECK(!path_exists(limited_directory),
              "combined work preflight created an output directory");
    }
cleanup:
    if (baseline_ready) remove_bundle(baseline_directory, &baseline);
    else remove_bundle(baseline_directory, NULL);
    if (limited_ready) remove_bundle(limited_directory, &limited);
    else remove_bundle(limited_directory, NULL);
    hwa_event_bundle_free(&baseline);
    hwa_event_bundle_free(&limited);
    (void)TEST_UNLINK(input);
}

static void test_silence(const char *workspace)
{
    HWAEventAnalysisOptions options;
    HWAEventBundleLimits limits;
    HWAEventBundle bundle;
    const HWAEventTrace *level_trace;
    const HWAEventTrace *pitch_trace;
    const HWAEventTrace *confidence_trace;
    const HWAEventTrace *valid_trace;
    double *levels = NULL;
    double *pitches = NULL;
    double *confidence = NULL;
    double *validity = NULL;
    char input[PATH_MAX] = {0};
    char directory[PATH_MAX] = {0};
    char error[HWA_ERROR_SIZE] = {0};
    size_t point;
    int ready = 0;
    int initial_failures = failures;
    memset(&bundle, 0, sizeof(bundle));
    CHECK(join_path(input, workspace, "silence.wav") &&
              join_path(directory, workspace, "silence.hwa-events") &&
              write_wave(input, TEST_SIGNAL_SILENCE),
          "cannot make silence fixture");
    if (failures != initial_failures) goto cleanup;
    test_options(&options);
    CHECK(hwa_analyze_events_wav(input, directory, &options,
                                 error, sizeof(error)) == 0,
          "silence analysis failed: %s", error);
    hwa_event_bundle_limits_default(&limits);
    if (hwa_event_bundle_read(directory, &limits, &bundle,
                              error, sizeof(error)) == 0) {
        ready = 1;
    } else {
        CHECK(0, "cannot read silence event bundle: %s", error);
        goto cleanup;
    }
    CHECK(bundle.event_count == 0U,
          "digital silence produced %zu events", bundle.event_count);
    check_trace_contract(&bundle);
    level_trace = find_trace(&bundle, "rms-dbfs");
    pitch_trace = find_trace(&bundle, "pitch-hz");
    confidence_trace = find_trace(&bundle, "pitch-confidence");
    valid_trace = find_trace(&bundle, "pitch-valid");
    CHECK(read_f64le_trace(directory, level_trace, &levels) &&
              read_f64le_trace(directory, pitch_trace, &pitches) &&
              read_f64le_trace(directory, confidence_trace, &confidence) &&
              read_f64le_trace(directory, valid_trace, &validity),
          "cannot read silence traces");
    if (levels != NULL && pitches != NULL && confidence != NULL &&
        validity != NULL) {
        for (point = 0U; point < TEST_TRACE_POINTS; ++point) {
            CHECK(levels[point] == -300.0 && pitches[point] == 0.0 &&
                      confidence[point] == 0.0 && validity[point] == 0.0,
                  "silence trace point %zu is not empty", point);
        }
    }
cleanup:
    free(levels);
    free(pitches);
    free(confidence);
    free(validity);
    if (ready) remove_bundle(directory, &bundle);
    else remove_bundle(directory, NULL);
    hwa_event_bundle_free(&bundle);
    (void)TEST_UNLINK(input);
}

static void test_existing_output_is_preserved(const char *workspace)
{
    static const char sentinel[] = "keep this output\n";
    HWAEventAnalysisOptions options;
    char input[PATH_MAX] = {0};
    char directory[PATH_MAX] = {0};
    char missing_input[PATH_MAX] = {0};
    char sentinel_path[PATH_MAX] = {0};
    char manifest_path[PATH_MAX] = {0};
    char bytes[sizeof(sentinel)];
    char error[HWA_ERROR_SIZE] = {0};
    FILE *stream = NULL;
    size_t count = 0U;
    int directory_ready = 0;
    int initial_failures = failures;
    CHECK(join_path(input, workspace, "refusal.wav") &&
              join_path(directory, workspace, "existing.hwa-events") &&
              join_path(missing_input, workspace, "missing.wav") &&
              join_path(sentinel_path, directory, "sentinel.txt") &&
              join_path(manifest_path, directory, "manifest.json") &&
              write_wave(input, TEST_SIGNAL_TWO_TONES) &&
              TEST_MKDIR(directory) == 0,
          "cannot make existing-output fixture");
    if (failures != initial_failures) goto cleanup;
    directory_ready = 1;
    stream = fopen(sentinel_path, "wb");
    if (stream == NULL) {
        CHECK(0, "cannot write output sentinel");
        goto cleanup;
    }
    if (fwrite(sentinel, 1U, sizeof(sentinel) - 1U, stream) !=
        sizeof(sentinel) - 1U) {
        CHECK(0, "cannot write output sentinel");
        (void)fclose(stream);
        stream = NULL;
        goto cleanup;
    }
    if (fclose(stream) != 0) {
        stream = NULL;
        CHECK(0, "cannot close output sentinel");
        goto cleanup;
    }
    stream = NULL;
    test_options(&options);
    CHECK(hwa_analyze_events_wav(missing_input, directory, &options,
                                 error, sizeof(error)) != 0 &&
              strstr(error, "already exists") != NULL,
          "existing output was not rejected before input work: %s", error);
    error[0] = '\0';
    CHECK(hwa_analyze_events_wav(input, directory, &options,
                                 error, sizeof(error)) != 0,
          "event analysis replaced an existing directory");
    stream = fopen(sentinel_path, "rb");
    if (stream != NULL) {
        count = fread(bytes, 1U, sizeof(bytes), stream);
        if (fclose(stream) != 0) count = 0U;
        stream = NULL;
    }
    CHECK(count == sizeof(sentinel) - 1U &&
              memcmp(bytes, sentinel, sizeof(sentinel) - 1U) == 0,
          "existing-output refusal changed the sentinel");
    stream = fopen(manifest_path, "rb");
    CHECK(stream == NULL,
          "existing-output refusal added a manifest");
    if (stream != NULL) {
        (void)fclose(stream);
        stream = NULL;
    }
cleanup:
    if (stream != NULL) (void)fclose(stream);
    (void)TEST_UNLINK(sentinel_path);
    if (directory_ready) (void)TEST_RMDIR(directory);
    (void)TEST_UNLINK(input);
}

int main(void)
{
    char workspace[PATH_MAX];
    CHECK(make_workspace(workspace), "cannot make event-analysis workspace");
    if (failures != 0) return 1;
    test_two_tones_and_determinism(workspace);
    test_steady_tone_is_one_event(workspace);
    test_contiguous_pitch_change_splits(workspace);
    test_short_wave_has_no_points(workspace);
    test_one_frame_gap_is_joined(workspace);
    test_segment_and_bundle_work_share_limit(workspace);
    test_silence(workspace);
    test_existing_output_is_preserved(workspace);
    (void)TEST_RMDIR(workspace);
    if (failures != 0) {
        (void)fprintf(stderr, "%d event-analysis test(s) failed\n", failures);
        return 1;
    }
    (void)puts("event analysis tests passed");
    return 0;
}

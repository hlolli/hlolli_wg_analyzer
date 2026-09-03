#if !defined(_WIN32)
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "event_analysis.h"

#include "inference_provider.h"
#include "internal.h"
#include "numeric_locale.h"
#include "sha256.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#define HWA_EVENT_ANALYSIS_PROVIDER_NAME \
    "org.hlolli.monophonic-analysis"
#define HWA_EVENT_ANALYSIS_PROVIDER_VERSION "1"
#define HWA_EVENT_ANALYSIS_TRACE_COUNT 9U
#define HWA_EVENT_ANALYSIS_VALUE_COUNT 6U
#define HWA_EVENT_ANALYSIS_SETTINGS_SIZE 1024U
#define HWA_EVENT_ANALYSIS_NO_EVENTS_CODE "no-active-events"
#define HWA_EVENT_ANALYSIS_NO_EVENTS_MESSAGE \
    "No voiced span met the current level, pitch confidence, " \
    "and duration settings."
#define HWA_EVENT_ANALYSIS_COPY_BLOCK_SIZE 65536U

typedef enum HWAEventAnalysisTraceField {
    HWA_TRACE_RMS_DBFS = 0,
    HWA_TRACE_PITCH_HZ = 1,
    HWA_TRACE_PITCH_CONFIDENCE = 2,
    HWA_TRACE_PITCH_VALID = 3,
    HWA_TRACE_ONSET_STRENGTH = 4,
    HWA_TRACE_SPECTRAL_CENTROID_HZ = 5,
    HWA_TRACE_SPECTRAL_ROLLOFF_HZ = 6,
    HWA_TRACE_SPECTRAL_FLATNESS = 7,
    HWA_TRACE_SPECTRUM_VALID = 8
} HWAEventAnalysisTraceField;

typedef struct HWAEventAnalysisTraceSpec {
    const char *name;
    const char *unit;
    const char *path;
} HWAEventAnalysisTraceSpec;

typedef struct HWAEventAnalysisTraceSource {
    const HWAAnalysis *analysis;
    size_t point_count;
    HWAEventAnalysisTraceField field;
} HWAEventAnalysisTraceSource;

typedef struct HWAEventAnalysisSegment {
    size_t first_point;
    size_t last_point;
} HWAEventAnalysisSegment;

typedef struct HWAEventAnalysisValueSpec {
    const char *name;
    const char *unit;
} HWAEventAnalysisValueSpec;

static const HWAEventAnalysisTraceSpec hwa_event_trace_specs[] = {
    {"rms-dbfs", "dBFS", "traces/rms-dbfs.f64le"},
    {"pitch-hz", "Hz", "traces/pitch-hz.f64le"},
    {"pitch-confidence", "ratio", "traces/pitch-confidence.f64le"},
    {"pitch-valid", "bool", "traces/pitch-valid.f64le"},
    {"onset-strength", "ratio", "traces/onset-strength.f64le"},
    {"spectral-centroid-hz", "Hz", "traces/spectral-centroid-hz.f64le"},
    {"spectral-rolloff-85-hz", "Hz",
     "traces/spectral-rolloff-85-hz.f64le"},
    {"spectral-flatness", "ratio", "traces/spectral-flatness.f64le"},
    {"spectrum-valid", "bool", "traces/spectrum-valid.f64le"}
};

static const HWAEventAnalysisValueSpec hwa_event_value_specs[] = {
    {"pitch-hz", "Hz"},
    {"rms-dbfs", "dBFS"},
    {"onset-strength", "ratio"},
    {"spectral-centroid-hz", "Hz"},
    {"spectral-rolloff-85-hz", "Hz"},
    {"spectral-flatness", "ratio"}
};

_Static_assert(sizeof(hwa_event_trace_specs) /
                   sizeof(hwa_event_trace_specs[0]) ==
                   HWA_EVENT_ANALYSIS_TRACE_COUNT,
               "event analysis trace count mismatch");
_Static_assert(sizeof(hwa_event_value_specs) /
                   sizeof(hwa_event_value_specs[0]) ==
                   HWA_EVENT_ANALYSIS_VALUE_COUNT,
               "event analysis value count mismatch");

static void hwa_event_analysis_error(char *error,
                                     size_t error_size,
                                     const char *message)
{
    if (error == NULL || error_size == 0U) return;
    (void)snprintf(error, error_size, "%s", message);
    error[error_size - 1U] = '\0';
}

static char *hwa_event_analysis_copy(const char *text)
{
    size_t length;
    char *copy;
    if (text == NULL) return NULL;
    length = strlen(text);
    if (length == SIZE_MAX) return NULL;
    copy = (char *)malloc(length + 1U);
    if (copy != NULL) memcpy(copy, text, length + 1U);
    return copy;
}

static const char *hwa_event_analysis_basename(const char *path)
{
    const char *slash;
    const char *backslash;
    const char *name;
    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');
    name = slash == NULL ? path : slash + 1;
    if (backslash != NULL && backslash + 1 > name) name = backslash + 1;
    return name;
}

static int hwa_event_analysis_output_absent(const char *path,
                                            char *error,
                                            size_t error_size)
{
#if defined(_WIN32)
    struct _stat64 status;
    if (_stat64(path, &status) == 0) {
#else
    struct stat status;
    if (lstat(path, &status) == 0) {
#endif
        hwa_event_analysis_error(error, error_size,
                                 "event bundle output already exists");
        return -1;
    }
    if (errno != ENOENT) {
        hwa_event_analysis_error(error, error_size,
                                 "cannot inspect event bundle output path");
        return -1;
    }
    return 0;
}

static int hwa_event_analysis_open_file_size(const HWAWavReader *reader,
                                             uint64_t *size,
                                             char *error,
                                             size_t error_size)
{
#if defined(_WIN32)
    struct _stat64 status;
    int descriptor = reader == NULL || reader->file == NULL
                         ? -1 : _fileno(reader->file);
    if (descriptor < 0 || _fstat64(descriptor, &status) != 0 ||
        status.st_size < 0) {
#else
    struct stat status;
    int descriptor = reader == NULL || reader->file == NULL
                         ? -1 : fileno(reader->file);
    if (descriptor < 0 || fstat(descriptor, &status) != 0 ||
        status.st_size < 0) {
#endif
        hwa_event_analysis_error(error, error_size,
                                 "cannot inspect open event analysis input");
        return -1;
    }
    *size = (uint64_t)status.st_size;
    return 0;
}

static int hwa_event_analysis_snapshot(
    const HWAByteSource *source,
    uint64_t max_bytes,
    FILE **snapshot_result,
    char sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    unsigned char buffer[HWA_EVENT_ANALYSIS_COPY_BLOCK_SIZE];
    unsigned char digest[32];
    HWASha256 hash;
    FILE *snapshot;
    uint64_t offset = 0U;
    *snapshot_result = NULL;
    if (source == NULL || source->read_at == NULL || sha256 == NULL ||
        max_bytes == 0U || source->size > max_bytes ||
        source->size > UINT64_MAX / UINT64_C(8)) {
        hwa_event_analysis_error(error, error_size,
                                 "invalid event analysis snapshot source");
        return -1;
    }
    snapshot = tmpfile();
    if (snapshot == NULL) {
        hwa_event_analysis_error(error, error_size,
                                 "cannot create event analysis snapshot");
        return -1;
    }
    hwa_sha256_init(&hash);
    while (offset < source->size) {
        uint64_t remaining = source->size - offset;
        size_t count = remaining > (uint64_t)sizeof(buffer)
                           ? sizeof(buffer) : (size_t)remaining;
        if (source->read_at(source->context, offset, buffer, count) != 0 ||
            fwrite(buffer, 1U, count, snapshot) != count) {
            (void)fclose(snapshot);
            hwa_event_analysis_error(error, error_size,
                                     "cannot copy event analysis snapshot");
            return -1;
        }
        hwa_sha256_update(&hash, buffer, count);
        offset += (uint64_t)count;
    }
    if (fflush(snapshot) != 0) {
        (void)fclose(snapshot);
        hwa_event_analysis_error(error, error_size,
                                 "cannot finish event analysis snapshot");
        return -1;
    }
    hwa_sha256_final(&hash, digest);
    hwa_sha256_hex(digest, sha256);
    *snapshot_result = snapshot;
    return 0;
}

void hwa_event_analysis_options_default(HWAEventAnalysisOptions *options)
{
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    hwa_analysis_options_default(&options->analysis);
    options->analysis.collect_tracks = 1;
    options->analysis.collect_spectrogram = 0;
    options->min_pitch_confidence = 0.60;
    options->pitch_split_semitones = 0.75;
    options->onset_split_strength = 0.65;
    options->min_note_points = 2U;
    options->max_gap_points = 1U;
    hwa_event_bundle_limits_default(&options->bundle_limits);
}

int hwa_event_analysis_options_validate(
    const HWAEventAnalysisOptions *options,
    char *error,
    size_t error_size)
{
    if (options == NULL || !isfinite(options->min_pitch_confidence) ||
        options->min_pitch_confidence < 0.0 ||
        options->min_pitch_confidence > 1.0 ||
        !isfinite(options->pitch_split_semitones) ||
        options->pitch_split_semitones <= 0.0 ||
        options->pitch_split_semitones > 48.0 ||
        !isfinite(options->onset_split_strength) ||
        options->onset_split_strength < 0.0 ||
        options->onset_split_strength > 1.0 ||
        options->min_note_points == 0U ||
        options->max_gap_points == SIZE_MAX) {
        hwa_event_analysis_error(error, error_size,
                                 "invalid event analysis options");
        return -1;
    }
    return hwa_analysis_options_validate(&options->analysis,
                                         error, error_size);
}

static int hwa_event_analysis_point_active(
    const HWAFrameMetrics *point,
    const HWAEventAnalysisOptions *options)
{
    return isfinite(point->rms_dbfs) &&
           point->rms_dbfs >= options->analysis.silence_threshold_dbfs &&
           point->pitch_valid && isfinite(point->pitch_hz) &&
           point->pitch_hz > 0.0 && isfinite(point->pitch_confidence) &&
           point->pitch_confidence >= options->min_pitch_confidence;
}

static double hwa_event_analysis_pitch_distance(double first,
                                                double second)
{
    if (!isfinite(first) || !isfinite(second) ||
        first <= 0.0 || second <= 0.0) return 0.0;
    return fabs(12.0 * log2(first / second));
}

static int hwa_event_analysis_pitch_split(
    const HWAAnalysis *analysis,
    size_t point,
    const HWAEventAnalysisOptions *options)
{
    const HWAFrameMetrics *a;
    const HWAFrameMetrics *b;
    const HWAFrameMetrics *c;
    const HWAFrameMetrics *d;
    double before;
    double after;
    double stable_limit = options->pitch_split_semitones * 0.5;
    if (point < 2U || point + 1U >= analysis->track_count) return 0;
    a = &analysis->tracks[point - 2U];
    b = &analysis->tracks[point - 1U];
    c = &analysis->tracks[point];
    d = &analysis->tracks[point + 1U];
    if (!hwa_event_analysis_point_active(a, options) ||
        !hwa_event_analysis_point_active(b, options) ||
        !hwa_event_analysis_point_active(c, options) ||
        !hwa_event_analysis_point_active(d, options)) return 0;
    if (hwa_event_analysis_pitch_distance(a->pitch_hz, b->pitch_hz) >
            stable_limit ||
        hwa_event_analysis_pitch_distance(c->pitch_hz, d->pitch_hz) >
            stable_limit)
        return 0;
    before = sqrt(a->pitch_hz * b->pitch_hz);
    after = sqrt(c->pitch_hz * d->pitch_hz);
    return hwa_event_analysis_pitch_distance(before, after) >=
           options->pitch_split_semitones;
}

static int hwa_event_analysis_onset_split(
    const HWAAnalysis *analysis,
    size_t point,
    const HWAEventAnalysisOptions *options)
{
    double prior;
    double current;
    double next;
    if (point == 0U || point + 1U >= analysis->track_count ||
        !hwa_event_analysis_point_active(&analysis->tracks[point], options))
        return 0;
    prior = analysis->tracks[point - 1U].combined_onset_strength;
    current = analysis->tracks[point].combined_onset_strength;
    next = analysis->tracks[point + 1U].combined_onset_strength;
    return isfinite(prior) && isfinite(current) && isfinite(next) &&
           current >= options->onset_split_strength &&
           current >= prior && current > next;
}

static int hwa_event_analysis_append_segment(
    HWAEventAnalysisSegment *segments,
    size_t capacity,
    size_t *count,
    size_t first,
    size_t last,
    char *error,
    size_t error_size)
{
    if (*count >= capacity || first > last) {
        hwa_event_analysis_error(error, error_size,
                                 "event segment count exceeds its bound");
        return -1;
    }
    segments[*count].first_point = first;
    segments[*count].last_point = last;
    (*count)++;
    return 0;
}

static int hwa_event_analysis_split_island(
    const HWAAnalysis *analysis,
    const HWAEventAnalysisOptions *options,
    size_t island_first,
    size_t island_last,
    HWAEventAnalysisSegment *segments,
    size_t segment_capacity,
    size_t *segment_count,
    char *error,
    size_t error_size)
{
    size_t island_output_start = *segment_count;
    size_t segment_first = island_first;
    size_t active_count = 0U;
    size_t point;
    for (point = island_first; point <= island_last; ++point) {
        int active = hwa_event_analysis_point_active(
            &analysis->tracks[point], options);
        int split = 0;
        if (point > segment_first && active &&
            point - segment_first >= options->min_note_points &&
            island_last - point + 1U >= options->min_note_points) {
            split = hwa_event_analysis_pitch_split(analysis, point, options) ||
                    hwa_event_analysis_onset_split(analysis, point, options);
        }
        if (split && active_count >= options->min_note_points) {
            if (hwa_event_analysis_append_segment(
                    segments, segment_capacity, segment_count,
                    segment_first, point - 1U, error, error_size) != 0)
                return -1;
            segment_first = point;
            active_count = 1U;
        } else if (active) {
            active_count++;
        }
        if (point == SIZE_MAX) break;
    }
    if (active_count >= options->min_note_points) {
        return hwa_event_analysis_append_segment(
            segments, segment_capacity, segment_count,
            segment_first, island_last, error, error_size);
    }
    if (*segment_count > island_output_start) {
        segments[*segment_count - 1U].last_point = island_last;
    }
    return 0;
}

static int hwa_event_analysis_detect_segments(
    const HWAAnalysis *analysis,
    size_t point_count,
    const HWAEventAnalysisOptions *options,
    HWAEventAnalysisSegment **segments_result,
    size_t *segment_count_result,
    char *error,
    size_t error_size)
{
    HWAEventAnalysisSegment *segments;
    size_t segment_count = 0U;
    size_t point = 0U;
    uint64_t work;
    *segments_result = NULL;
    *segment_count_result = 0U;
    if (point_count == 0U) return 0;
    if (point_count > SIZE_MAX / sizeof(*segments)) {
        hwa_event_analysis_error(error, error_size,
                                 "event segment storage overflows");
        return -1;
    }
    work = (uint64_t)point_count * (uint64_t)sizeof(*segments);
    if (work > options->bundle_limits.max_work_bytes) {
        hwa_event_analysis_error(error, error_size,
                                 "event segments exceed the work limit");
        return -1;
    }
    segments = (HWAEventAnalysisSegment *)calloc(point_count,
                                                  sizeof(*segments));
    if (segments == NULL) {
        hwa_event_analysis_error(error, error_size,
                                 "cannot allocate event segments");
        return -1;
    }
    while (point < point_count) {
        size_t first;
        size_t last;
        size_t active_count = 0U;
        size_t gap_count = 0U;
        while (point < point_count &&
               !hwa_event_analysis_point_active(
                   &analysis->tracks[point], options))
            point++;
        if (point == point_count) break;
        first = point;
        last = point;
        while (point < point_count) {
            if (hwa_event_analysis_point_active(
                    &analysis->tracks[point], options)) {
                last = point;
                active_count++;
                gap_count = 0U;
            } else {
                gap_count++;
                if (gap_count > options->max_gap_points) break;
            }
            point++;
        }
        if (active_count >= options->min_note_points &&
            hwa_event_analysis_split_island(
                analysis, options, first, last, segments, point_count,
                &segment_count, error, error_size) != 0) {
            free(segments);
            return -1;
        }
    }
    *segments_result = segments;
    *segment_count_result = segment_count;
    return 0;
}

static int hwa_event_analysis_format_settings(
    const HWAEventAnalysisOptions *options,
    char *buffer,
    size_t buffer_size,
    char *error,
    size_t error_size)
{
    HWANumericLocale locale;
    char silence[64];
    char confidence[64];
    char pitch_split[64];
    char onset_split[64];
    const char *channel;
    int length;
    int status = -1;
    memset(&locale, 0, sizeof(locale));
    if (buffer == NULL || buffer_size == 0U) {
        hwa_event_analysis_error(error, error_size,
                                 "invalid event analysis settings buffer");
        return -1;
    }
    buffer[0] = '\0';
    if (options->analysis.channel_mode == HWA_CHANNEL_KEEP)
        channel = "keep";
    else if (options->analysis.channel_mode == HWA_CHANNEL_SELECT)
        channel = "select";
    else
        channel = "mix";
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_event_analysis_error(error, error_size,
                                 "cannot enter C numeric locale");
        return -1;
    }
    if (hwa_c_locale_format_double(&locale, silence, sizeof(silence),
                                   options->analysis.silence_threshold_dbfs) != 0 ||
        hwa_c_locale_format_double(&locale, confidence, sizeof(confidence),
                                   options->min_pitch_confidence) != 0 ||
        hwa_c_locale_format_double(&locale, pitch_split, sizeof(pitch_split),
                                   options->pitch_split_semitones) != 0 ||
        hwa_c_locale_format_double(&locale, onset_split, sizeof(onset_split),
                                   options->onset_split_strength) != 0) {
        hwa_event_analysis_error(error, error_size,
                                 "cannot format event analysis settings");
        goto cleanup;
    }
    length = snprintf(
        buffer, buffer_size,
        "{\"algorithm\":\"monophonic-v1\",\"channel_mode\":\"%s\","
        "\"selected_channel\":%u,\"frame_size\":%zu,\"hop_size\":%zu,"
        "\"silence_threshold_dbfs\":%s,\"min_pitch_confidence\":%s,"
        "\"pitch_split_semitones\":%s,\"onset_split_strength\":%s,"
        "\"min_note_points\":%zu,\"max_gap_points\":%zu}",
        channel, (unsigned)options->analysis.selected_channel,
        options->analysis.frame_size, options->analysis.hop_size,
        silence, confidence, pitch_split, onset_split,
        options->min_note_points, options->max_gap_points);
    if (length < 0 || (size_t)length >= buffer_size) {
        hwa_event_analysis_error(error, error_size,
                                 "event analysis settings are too long");
        goto cleanup;
    }
    status = 0;
cleanup:
    if (hwa_c_numeric_locale_end(&locale) != 0 && status == 0) {
        buffer[0] = '\0';
        hwa_event_analysis_error(error, error_size,
                                 "cannot restore numeric locale");
        status = -1;
    }
    return status;
}

static int hwa_event_analysis_work_add(uint64_t *total,
                                       size_t count,
                                       size_t item_size,
                                       uint64_t maximum)
{
    uint64_t bytes;
    if (count != 0U && item_size > SIZE_MAX / count) return -1;
    if ((uintmax_t)(count * item_size) > UINT64_MAX) return -1;
    bytes = (uint64_t)(count * item_size);
    if (*total > maximum || bytes > maximum - *total) return -1;
    *total += bytes;
    return 0;
}

static int hwa_event_analysis_work_add_text(uint64_t *total,
                                            const char *text,
                                            uint64_t maximum)
{
    size_t length;
    if (text == NULL) return 0;
    length = strlen(text);
    if (length == SIZE_MAX) return -1;
    return hwa_event_analysis_work_add(
        total, length + 1U, 1U, maximum);
}

static int hwa_event_analysis_preflight_work(
    const char *input_path,
    const char *settings_json,
    size_t full_point_count,
    size_t segment_count,
    const HWAEventAnalysisOptions *options,
    char *error,
    size_t error_size)
{
    uint64_t total = 0U;
    uint64_t event_work = 0U;
    uint64_t maximum = options->bundle_limits.max_work_bytes;
    size_t trace_count = full_point_count == 0U
                             ? 0U
                             : HWA_EVENT_ANALYSIS_TRACE_COUNT;
    size_t index;
    if (hwa_event_analysis_work_add(
            &total, full_point_count, sizeof(HWAEventAnalysisSegment),
            maximum) != 0 ||
        hwa_event_analysis_work_add(
            &total, 1U, sizeof(HWAEventProvider), maximum) != 0 ||
        hwa_event_analysis_work_add(
            &total, 1U, sizeof(HWAEventAudio), maximum) != 0 ||
        hwa_event_analysis_work_add(
            &total, trace_count, sizeof(HWAEventTrace), maximum) != 0 ||
        hwa_event_analysis_work_add_text(
            &total, HWA_EVENT_ANALYSIS_PROVIDER_NAME, maximum) != 0 ||
        hwa_event_analysis_work_add_text(
            &total, HWA_EVENT_ANALYSIS_PROVIDER_VERSION, maximum) != 0 ||
        hwa_event_analysis_work_add_text(
            &total, settings_json, maximum) != 0 ||
        hwa_event_analysis_work_add_text(
            &total, hwa_event_analysis_basename(input_path), maximum) != 0 ||
        hwa_event_analysis_work_add_text(&total, "", maximum) != 0 ||
        hwa_event_analysis_work_add_text(
            &total, input_path, maximum) != 0)
        goto too_large;
    for (index = 0U; index < trace_count; ++index) {
        if (hwa_event_analysis_work_add_text(
                &total, hwa_event_trace_specs[index].name, maximum) != 0 ||
            hwa_event_analysis_work_add_text(
                &total, hwa_event_trace_specs[index].unit, maximum) != 0 ||
            hwa_event_analysis_work_add_text(
                &total, hwa_event_trace_specs[index].path, maximum) != 0)
            goto too_large;
    }
    if (segment_count == 0U) {
        if (hwa_event_analysis_work_add(
                &total, 1U, sizeof(HWAEventWarning), maximum) != 0 ||
            hwa_event_analysis_work_add_text(
                &total, HWA_EVENT_ANALYSIS_NO_EVENTS_CODE, maximum) != 0 ||
            hwa_event_analysis_work_add_text(
                &total, HWA_EVENT_ANALYSIS_NO_EVENTS_MESSAGE, maximum) != 0)
            goto too_large;
        return 0;
    }
    if (hwa_event_analysis_work_add(
            &event_work, 1U, sizeof(HWAPerformanceEvent), UINT64_MAX) != 0 ||
        hwa_event_analysis_work_add(
            &event_work, HWA_EVENT_ANALYSIS_VALUE_COUNT,
            sizeof(HWAEventValue), UINT64_MAX) != 0 ||
        hwa_event_analysis_work_add(
            &event_work, HWA_EVENT_ANALYSIS_TRACE_COUNT,
            sizeof(HWAEventTraceRef), UINT64_MAX) != 0 ||
        hwa_event_analysis_work_add_text(
            &event_work, "note", UINT64_MAX) != 0 ||
        hwa_event_analysis_work_add(
            &event_work, 3U, 1U, UINT64_MAX) != 0)
        goto too_large;
    for (index = 0U; index < HWA_EVENT_ANALYSIS_VALUE_COUNT; ++index) {
        if (hwa_event_analysis_work_add_text(
                &event_work, hwa_event_value_specs[index].name,
                UINT64_MAX) != 0 ||
            hwa_event_analysis_work_add_text(
                &event_work, hwa_event_value_specs[index].unit,
                UINT64_MAX) != 0)
            goto too_large;
    }
    for (index = 0U; index < HWA_EVENT_ANALYSIS_TRACE_COUNT; ++index) {
        if (hwa_event_analysis_work_add_text(
                &event_work, hwa_event_trace_specs[index].name,
                UINT64_MAX) != 0)
            goto too_large;
    }
    if ((uintmax_t)event_work > (uintmax_t)SIZE_MAX ||
        hwa_event_analysis_work_add(
            &total, segment_count, (size_t)event_work, maximum) != 0)
        goto too_large;
    return 0;
too_large:
    hwa_event_analysis_error(
        error, error_size,
        "event analysis retained bundle and segment work exceed limit");
    return -1;
}

static double hwa_event_analysis_trace_value(
    const HWAEventAnalysisTraceSource *source,
    size_t point)
{
    const HWAFrameMetrics *frame = &source->analysis->tracks[point];
    switch (source->field) {
    case HWA_TRACE_RMS_DBFS:
        return frame->rms_dbfs;
    case HWA_TRACE_PITCH_HZ:
        return frame->pitch_valid ? frame->pitch_hz : 0.0;
    case HWA_TRACE_PITCH_CONFIDENCE:
        return frame->pitch_valid ? frame->pitch_confidence : 0.0;
    case HWA_TRACE_PITCH_VALID:
        return frame->pitch_valid ? 1.0 : 0.0;
    case HWA_TRACE_ONSET_STRENGTH:
        return frame->combined_onset_strength;
    case HWA_TRACE_SPECTRAL_CENTROID_HZ:
        return frame->spectrum_valid ? frame->spectral_centroid_hz : 0.0;
    case HWA_TRACE_SPECTRAL_ROLLOFF_HZ:
        return frame->spectrum_valid ? frame->spectral_rolloff_85_hz : 0.0;
    case HWA_TRACE_SPECTRAL_FLATNESS:
        return frame->spectrum_valid ? frame->spectral_flatness : 0.0;
    case HWA_TRACE_SPECTRUM_VALID:
        return frame->spectrum_valid ? 1.0 : 0.0;
    default:
        return 0.0;
    }
}

static int hwa_event_analysis_trace_read(void *context,
                                         uint64_t offset,
                                         unsigned char *destination,
                                         size_t size)
{
    const HWAEventAnalysisTraceSource *source =
        (const HWAEventAnalysisTraceSource *)context;
    uint64_t total;
    size_t copied = 0U;
    if (source == NULL || source->analysis == NULL || destination == NULL ||
        source->point_count > UINT64_MAX / UINT64_C(8)) return -1;
    total = (uint64_t)source->point_count * UINT64_C(8);
    if (offset > total || (uint64_t)size > total - offset) return -1;
    while (copied < size) {
        uint64_t absolute = offset + (uint64_t)copied;
        size_t point = (size_t)(absolute / UINT64_C(8));
        size_t byte_offset = (size_t)(absolute % UINT64_C(8));
        size_t count = 8U - byte_offset;
        unsigned char bytes[8];
        uint64_t bits;
        double value = hwa_event_analysis_trace_value(source, point);
        size_t byte;
        if (!isfinite(value) || sizeof(value) != sizeof(bits)) return -1;
        memcpy(&bits, &value, sizeof(bits));
        for (byte = 0U; byte < sizeof(bytes); ++byte)
            bytes[byte] = (unsigned char)(bits >> (byte * 8U));
        if (count > size - copied) count = size - copied;
        memcpy(destination + copied, bytes + byte_offset, count);
        copied += count;
    }
    return 0;
}

static int hwa_event_analysis_value_init(HWAEventValue *value,
                                         const char *name,
                                         HWAEventValueBasis basis,
                                         double number,
                                         const char *unit,
                                         double score,
                                         int score_valid,
                                         char *error,
                                         size_t error_size)
{
    memset(value, 0, sizeof(*value));
    value->name = hwa_event_analysis_copy(name);
    value->unit = hwa_event_analysis_copy(unit);
    if (value->name == NULL || value->unit == NULL) {
        free(value->name);
        free(value->unit);
        value->name = NULL;
        value->unit = NULL;
        hwa_event_analysis_error(error, error_size,
                                 "cannot allocate event value");
        return -1;
    }
    value->kind = HWA_EVENT_VALUE_F64;
    value->basis = basis;
    value->number = number;
    value->score = score;
    value->provider_id = UINT64_C(1);
    value->score_valid = score_valid;
    value->provider_id_valid = 1;
    value->selected = 1;
    return 0;
}

static void hwa_event_analysis_bounds(
    const HWAAnalysis *analysis,
    const HWAEventAnalysisSegment *segment,
    size_t full_point_count,
    uint64_t *start,
    uint64_t *end)
{
    uint64_t hop = (uint64_t)analysis->options.hop_size;
    uint64_t frame = (uint64_t)analysis->options.frame_size;
    uint64_t first_center =
        (uint64_t)segment->first_point * hop + frame / UINT64_C(2);
    uint64_t last_center =
        (uint64_t)segment->last_point * hop + frame / UINT64_C(2);
    uint64_t left = hop / UINT64_C(2);
    uint64_t right = hop - left;
    *start = segment->first_point == 0U
                 ? UINT64_C(0)
                 : first_center - left;
    *end = segment->last_point + 1U == full_point_count
               ? analysis->format.frames
               : last_center + right;
    if (*end > analysis->format.frames) *end = analysis->format.frames;
}

static int hwa_event_analysis_fill_event(
    HWAEventBundle *bundle,
    size_t event_index,
    const HWAAnalysis *analysis,
    const HWAEventAnalysisOptions *options,
    const HWAEventAnalysisSegment *segment,
    size_t full_point_count,
    char *error,
    size_t error_size)
{
    HWAPerformanceEvent *event = &bundle->events[event_index];
    long double pitch_log_sum = 0.0L;
    long double pitch_weight_sum = 0.0L;
    long double confidence_sum = 0.0L;
    long double power_sum = 0.0L;
    long double centroid_sum = 0.0L;
    long double rolloff_sum = 0.0L;
    long double flatness_sum = 0.0L;
    size_t pitch_count = 0U;
    size_t spectrum_count = 0U;
    size_t point_count = segment->last_point - segment->first_point + 1U;
    double onset_max = 0.0;
    size_t point;
    size_t value_count = 0U;
    size_t trace_index;
    event->id = (uint64_t)event_index + UINT64_C(1);
    event->kind = hwa_event_analysis_copy("note");
    event->source_recording_id = UINT64_C(1);
    event->evidence_audio_id = UINT64_C(1);
    event->evidence_audio_id_valid = 1;
    event->voice = hwa_event_analysis_copy("");
    event->part = hwa_event_analysis_copy("");
    event->score_event_id = hwa_event_analysis_copy("");
    hwa_event_analysis_bounds(analysis, segment, full_point_count,
                              &event->start_sample, &event->end_sample);
    event->values = (HWAEventValue *)calloc(
        HWA_EVENT_ANALYSIS_VALUE_COUNT, sizeof(*event->values));
    event->trace_refs = (HWAEventTraceRef *)calloc(
        HWA_EVENT_ANALYSIS_TRACE_COUNT, sizeof(*event->trace_refs));
    if (event->kind == NULL || event->voice == NULL || event->part == NULL ||
        event->score_event_id == NULL || event->values == NULL ||
        event->trace_refs == NULL ||
        event->start_sample >= event->end_sample) {
        hwa_event_analysis_error(error, error_size,
                                 "cannot allocate or bound performance event");
        return -1;
    }
    for (point = segment->first_point; point <= segment->last_point; ++point) {
        const HWAFrameMetrics *frame = &analysis->tracks[point];
        double onset = frame->combined_onset_strength;
        power_sum += pow(10.0L, (long double)frame->rms_dbfs / 10.0L);
        if (hwa_event_analysis_point_active(frame, options)) {
            long double weight = (long double)frame->pitch_confidence;
            pitch_log_sum += weight * log((long double)frame->pitch_hz);
            pitch_weight_sum += weight;
            confidence_sum += weight;
            pitch_count++;
        }
        if (frame->spectrum_valid) {
            centroid_sum += (long double)frame->spectral_centroid_hz;
            rolloff_sum += (long double)frame->spectral_rolloff_85_hz;
            flatness_sum += (long double)frame->spectral_flatness;
            spectrum_count++;
        }
        if (isfinite(onset) && onset > onset_max) onset_max = onset;
        if (point == SIZE_MAX) break;
    }
    if (pitch_count == 0U || pitch_weight_sum <= 0.0L) {
        hwa_event_analysis_error(error, error_size,
                                 "detected event has no valid pitch support");
        return -1;
    }
    if (hwa_event_analysis_value_init(
            &event->values[value_count], hwa_event_value_specs[0].name,
            HWA_EVENT_INFERENCE,
            (double)exp(pitch_log_sum / pitch_weight_sum),
            hwa_event_value_specs[0].unit,
            (double)(confidence_sum / (long double)pitch_count), 1,
            error, error_size) != 0)
        return -1;
    event->value_count = ++value_count;
    if (hwa_event_analysis_value_init(
            &event->values[value_count], hwa_event_value_specs[1].name,
            HWA_EVENT_OBSERVATION,
            10.0 * log10((double)(power_sum / (long double)point_count)),
            hwa_event_value_specs[1].unit, 0.0, 0, error, error_size) != 0)
        return -1;
    event->value_count = ++value_count;
    if (hwa_event_analysis_value_init(
            &event->values[value_count], hwa_event_value_specs[2].name,
            HWA_EVENT_OBSERVATION, onset_max,
            hwa_event_value_specs[2].unit, 0.0, 0,
            error, error_size) != 0)
        return -1;
    event->value_count = ++value_count;
    if (spectrum_count != 0U) {
        if (hwa_event_analysis_value_init(
                &event->values[value_count], hwa_event_value_specs[3].name,
                HWA_EVENT_OBSERVATION,
                (double)(centroid_sum / (long double)spectrum_count),
                hwa_event_value_specs[3].unit, 0.0, 0,
                error, error_size) != 0)
            return -1;
        event->value_count = ++value_count;
        if (hwa_event_analysis_value_init(
                &event->values[value_count], hwa_event_value_specs[4].name,
                HWA_EVENT_OBSERVATION,
                (double)(rolloff_sum / (long double)spectrum_count),
                hwa_event_value_specs[4].unit, 0.0, 0,
                error, error_size) != 0)
            return -1;
        event->value_count = ++value_count;
        if (hwa_event_analysis_value_init(
                &event->values[value_count], hwa_event_value_specs[5].name,
                HWA_EVENT_OBSERVATION,
                (double)(flatness_sum / (long double)spectrum_count),
                hwa_event_value_specs[5].unit, 0.0, 0,
                error, error_size) != 0)
            return -1;
        event->value_count = ++value_count;
    }
    for (trace_index = 0U;
         trace_index < HWA_EVENT_ANALYSIS_TRACE_COUNT; ++trace_index) {
        HWAEventTraceRef *ref = &event->trace_refs[trace_index];
        ref->trace_id = (uint64_t)trace_index + UINT64_C(1);
        ref->role = hwa_event_analysis_copy(
            hwa_event_trace_specs[trace_index].name);
        ref->first_point = (uint64_t)segment->first_point;
        ref->point_count = (uint64_t)point_count;
        if (ref->role == NULL) {
            hwa_event_analysis_error(error, error_size,
                                     "cannot allocate event trace reference");
            return -1;
        }
        event->trace_ref_count = trace_index + 1U;
    }
    return 0;
}

static int hwa_event_analysis_build_bundle(
    HWAEventBundle *bundle,
    const HWAAnalysis *analysis,
    const char *input_path,
    uint64_t input_bytes,
    const char input_sha256[HWA_SHA256_HEX_SIZE],
    const char *settings_json,
    const HWAEventAnalysisOptions *options,
    const HWAEventAnalysisSegment *segments,
    size_t segment_count,
    size_t full_point_count,
    HWAEventAnalysisTraceSource trace_sources[HWA_EVENT_ANALYSIS_TRACE_COUNT],
    HWAInferencePayload payloads[HWA_EVENT_ANALYSIS_TRACE_COUNT],
    char *error,
    size_t error_size)
{
    HWAEventProvider *provider;
    HWAEventAudio *audio;
    size_t index;
    size_t trace_count = full_point_count == 0U
                             ? 0U
                             : HWA_EVENT_ANALYSIS_TRACE_COUNT;
    if (options->bundle_limits.max_audio_files < 1U ||
        options->bundle_limits.max_providers < 1U ||
        trace_count > options->bundle_limits.max_traces ||
        segment_count > options->bundle_limits.max_events ||
        (segment_count != 0U &&
         (segment_count > options->bundle_limits.max_values /
                              HWA_EVENT_ANALYSIS_VALUE_COUNT ||
          segment_count >
              options->bundle_limits.max_trace_refs /
                  HWA_EVENT_ANALYSIS_TRACE_COUNT)) ||
        (segment_count == 0U && options->bundle_limits.max_warnings < 1U)) {
        hwa_event_analysis_error(error, error_size,
                                 "event analysis output exceeds row limits");
        return -1;
    }
    bundle->providers = (HWAEventProvider *)calloc(
        1U, sizeof(*bundle->providers));
    bundle->audio = (HWAEventAudio *)calloc(1U, sizeof(*bundle->audio));
    if (bundle->providers == NULL || bundle->audio == NULL) {
        hwa_event_analysis_error(error, error_size,
                                 "cannot allocate event bundle source rows");
        return -1;
    }
    bundle->provider_count = 1U;
    bundle->audio_count = 1U;
    provider = &bundle->providers[0];
    audio = &bundle->audio[0];
    provider->id = UINT64_C(1);
    provider->name = hwa_event_analysis_copy(
        HWA_EVENT_ANALYSIS_PROVIDER_NAME);
    provider->version = hwa_event_analysis_copy(
        HWA_EVENT_ANALYSIS_PROVIDER_VERSION);
    provider->settings_json = hwa_event_analysis_copy(settings_json);
    if (provider->name == NULL || provider->version == NULL ||
        provider->settings_json == NULL) {
        hwa_event_analysis_error(error, error_size,
                                 "cannot allocate event provider metadata");
        return -1;
    }
    audio->id = UINT64_C(1);
    audio->kind = HWA_EVENT_SOURCE_RECORDING;
    audio->name = hwa_event_analysis_copy(
        hwa_event_analysis_basename(input_path));
    audio->relative_path = hwa_event_analysis_copy("");
    audio->path_hint = hwa_event_analysis_copy(input_path);
    memcpy(audio->sha256, input_sha256, HWA_SHA256_HEX_SIZE);
    audio->file_bytes = input_bytes;
    audio->format = analysis->format;
    if (audio->name == NULL || audio->name[0] == '\0' ||
        audio->relative_path == NULL || audio->path_hint == NULL) {
        hwa_event_analysis_error(error, error_size,
                                 "cannot allocate event bundle source metadata");
        return -1;
    }
    if (trace_count != 0U) {
        uint64_t trace_bytes;
        if ((uint64_t)full_point_count > UINT64_MAX / UINT64_C(8)) {
            hwa_event_analysis_error(error, error_size,
                                     "event trace byte count overflows");
            return -1;
        }
        trace_bytes = (uint64_t)full_point_count * UINT64_C(8);
        bundle->traces = (HWAEventTrace *)calloc(
            trace_count, sizeof(*bundle->traces));
        if (bundle->traces == NULL) {
            hwa_event_analysis_error(error, error_size,
                                     "cannot allocate event traces");
            return -1;
        }
        bundle->trace_count = trace_count;
        for (index = 0U; index < trace_count; ++index) {
            HWAEventTrace *trace = &bundle->traces[index];
            trace->id = (uint64_t)index + UINT64_C(1);
            trace->name = hwa_event_analysis_copy(
                hwa_event_trace_specs[index].name);
            trace->unit = hwa_event_analysis_copy(
                hwa_event_trace_specs[index].unit);
            trace->relative_path = hwa_event_analysis_copy(
                hwa_event_trace_specs[index].path);
            trace->format = HWA_EVENT_TRACE_F64LE;
            trace->source_recording_id = UINT64_C(1);
            trace->first_sample = UINT64_C(0);
            trace->hop_samples = (uint64_t)analysis->options.hop_size;
            trace->window_samples = (uint64_t)analysis->options.frame_size;
            trace->point_count = (uint64_t)full_point_count;
            trace->value_width = UINT32_C(1);
            trace->file_bytes = trace_bytes;
            if (trace->name == NULL || trace->unit == NULL ||
                trace->relative_path == NULL) {
                hwa_event_analysis_error(error, error_size,
                                         "cannot allocate event trace metadata");
                return -1;
            }
            trace_sources[index].analysis = analysis;
            trace_sources[index].point_count = full_point_count;
            trace_sources[index].field = (HWAEventAnalysisTraceField)index;
            payloads[index].relative_path = trace->relative_path;
            payloads[index].bytes.context = &trace_sources[index];
            payloads[index].bytes.name = trace->relative_path;
            payloads[index].bytes.size = trace_bytes;
            payloads[index].bytes.read_at = hwa_event_analysis_trace_read;
            if (hwa_inference_byte_source_sha256(
                    &payloads[index].bytes,
                    options->bundle_limits.max_payload_file_bytes,
                    trace->sha256, error, error_size) != 0)
                return -1;
        }
    }
    if (segment_count != 0U) {
        bundle->events = (HWAPerformanceEvent *)calloc(
            segment_count, sizeof(*bundle->events));
        if (bundle->events == NULL) {
            hwa_event_analysis_error(error, error_size,
                                     "cannot allocate performance events");
            return -1;
        }
        bundle->event_count = segment_count;
        for (index = 0U; index < segment_count; ++index) {
            if (hwa_event_analysis_fill_event(
                    bundle, index, analysis, options, &segments[index],
                    full_point_count, error, error_size) != 0)
                return -1;
        }
    } else {
        bundle->warnings = (HWAEventWarning *)calloc(
            1U, sizeof(*bundle->warnings));
        if (bundle->warnings == NULL) {
            hwa_event_analysis_error(error, error_size,
                                     "cannot allocate event warning");
            return -1;
        }
        bundle->warning_count = 1U;
        bundle->warnings[0].id = UINT64_C(1);
        bundle->warnings[0].code = hwa_event_analysis_copy(
            HWA_EVENT_ANALYSIS_NO_EVENTS_CODE);
        bundle->warnings[0].message = hwa_event_analysis_copy(
            HWA_EVENT_ANALYSIS_NO_EVENTS_MESSAGE);
        if (bundle->warnings[0].code == NULL ||
            bundle->warnings[0].message == NULL) {
            hwa_event_analysis_error(error, error_size,
                                     "cannot allocate event warning text");
            return -1;
        }
    }
    return 0;
}

int hwa_analyze_events_wav(const char *input_path,
                           const char *output_directory,
                           const HWAEventAnalysisOptions *provided_options,
                           char *error,
                           size_t error_size)
{
    HWAEventAnalysisOptions defaults;
    HWAEventAnalysisOptions selected;
    const HWAEventAnalysisOptions *options;
    HWAWavReader reader;
    HWAByteSource input_source;
    HWAAnalysis analysis;
    HWAEventAnalysisSegment *segments = NULL;
    size_t segment_count = 0U;
    size_t full_point_count = 0U;
    HWAEventBundle bundle;
    HWAInferencePayload payloads[HWA_EVENT_ANALYSIS_TRACE_COUNT];
    HWAEventAnalysisTraceSource
        trace_sources[HWA_EVENT_ANALYSIS_TRACE_COUNT];
    HWAInferenceOutput output;
    FILE *snapshot = NULL;
    uint64_t input_bytes;
    uint64_t input_bytes_after;
    uint64_t full_points_u64;
    char input_sha256[HWA_SHA256_HEX_SIZE];
    char verified_sha256[HWA_SHA256_HEX_SIZE];
    char settings_json[HWA_EVENT_ANALYSIS_SETTINGS_SIZE];
    int reader_open = 0;
    int result = -1;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    memset(&analysis, 0, sizeof(analysis));
    memset(&reader, 0, sizeof(reader));
    memset(&input_source, 0, sizeof(input_source));
    memset(&bundle, 0, sizeof(bundle));
    memset(payloads, 0, sizeof(payloads));
    memset(trace_sources, 0, sizeof(trace_sources));
    memset(&output, 0, sizeof(output));
    if (provided_options == NULL) {
        hwa_event_analysis_options_default(&defaults);
        selected = defaults;
    } else {
        selected = *provided_options;
    }
    selected.analysis.collect_tracks = 1;
    selected.analysis.collect_spectrogram = 0;
    if (selected.analysis.channel_mode != HWA_CHANNEL_SELECT)
        selected.analysis.selected_channel = 0U;
    options = &selected;
    if (input_path == NULL || input_path[0] == '\0' ||
        strcmp(input_path, "-") == 0 || output_directory == NULL ||
        output_directory[0] == '\0' || strcmp(output_directory, "-") == 0) {
        hwa_event_analysis_error(error, error_size,
                                 "event analysis needs named input and output paths");
        return -1;
    }
    if (hwa_event_analysis_options_validate(options,
                                            error, error_size) != 0)
        return -1;
    if (hwa_event_analysis_format_settings(
            options, settings_json, sizeof(settings_json),
            error, error_size) != 0)
        return -1;
    if (hwa_event_analysis_output_absent(output_directory,
                                         error, error_size) != 0)
        return -1;
    if (hwa_wav_reader_open(&reader, input_path,
                            options->analysis.max_input_bytes,
                            error, error_size) != 0)
        return -1;
    reader_open = 1;
    if (reader.format.frames > options->analysis.max_input_frames) {
        hwa_set_error(error, error_size,
                      "input has %llu frames, above the %llu-frame limit",
                      (unsigned long long)reader.format.frames,
                      (unsigned long long)options->analysis.max_input_frames);
        goto cleanup;
    }
    if (options->analysis.channel_mode == HWA_CHANNEL_SELECT &&
        options->analysis.selected_channel > reader.format.channels) {
        hwa_set_error(error, error_size,
                      "selected channel %u exceeds the %u-channel input",
                      (unsigned)options->analysis.selected_channel,
                      (unsigned)reader.format.channels);
        goto cleanup;
    }
    input_source = reader.source;
    input_source.name = input_path;
    input_bytes = input_source.size;
    if (hwa_event_analysis_snapshot(
            &input_source, options->analysis.max_input_bytes,
            &snapshot, input_sha256, error, error_size) != 0 ||
        hwa_inference_byte_source_sha256(
            &input_source, options->analysis.max_input_bytes,
            verified_sha256, error, error_size) != 0 ||
        hwa_event_analysis_open_file_size(
            &reader, &input_bytes_after, error, error_size) != 0)
        goto cleanup;
    if (input_bytes != input_bytes_after ||
        strcmp(input_sha256, verified_sha256) != 0) {
        hwa_event_analysis_error(error, error_size,
                                 "event analysis input changed while it was read");
        goto cleanup;
    }
    hwa_wav_reader_close(&reader);
    reader_open = 0;
    if (hwa_wav_reader_open_file(
            &reader, snapshot, options->analysis.max_input_bytes,
            error, error_size) != 0) {
        snapshot = NULL;
        goto cleanup;
    }
    snapshot = NULL;
    reader_open = 1;
    if (hwa_analyze_wav_reader(&reader, input_path, &options->analysis,
                               &analysis, error, error_size) != 0)
        goto cleanup;
    if (analysis.format.frames >=
        (uint64_t)options->analysis.frame_size) {
        full_points_u64 = UINT64_C(1) +
            (analysis.format.frames -
             (uint64_t)options->analysis.frame_size) /
                (uint64_t)options->analysis.hop_size;
        if (full_points_u64 > (uint64_t)SIZE_MAX ||
            full_points_u64 > (uint64_t)analysis.track_count) {
            hwa_event_analysis_error(error, error_size,
                                     "complete analysis point count overflows");
            goto cleanup;
        }
        full_point_count = (size_t)full_points_u64;
    }
    if (hwa_event_analysis_detect_segments(
            &analysis, full_point_count, options, &segments,
            &segment_count, error, error_size) != 0)
        goto cleanup;
    if (hwa_event_analysis_preflight_work(
            input_path, settings_json, full_point_count, segment_count,
            options, error, error_size) != 0)
        goto cleanup;
    if (hwa_event_analysis_build_bundle(
            &bundle, &analysis, input_path, input_bytes,
            input_sha256, settings_json, options, segments, segment_count,
            full_point_count, trace_sources, payloads,
            error, error_size) != 0)
        goto cleanup;
    free(segments);
    segments = NULL;
    output.bundle = &bundle;
    output.payloads = full_point_count == 0U ? NULL : payloads;
    output.payload_count = full_point_count == 0U
                               ? 0U
                               : HWA_EVENT_ANALYSIS_TRACE_COUNT;
    if (hwa_inference_output_write(
            output_directory, &output, &options->bundle_limits,
            error, error_size) != 0)
        goto cleanup;
    result = 0;
cleanup:
    hwa_event_bundle_free(&bundle);
    free(segments);
    hwa_analysis_free(&analysis);
    if (reader_open) hwa_wav_reader_close(&reader);
    if (snapshot != NULL) (void)fclose(snapshot);
    return result;
}

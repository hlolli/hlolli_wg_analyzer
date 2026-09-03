#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "hlolli_wg_analyzer.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#define TEST_PID _getpid
#define TEST_RMDIR _rmdir
#define TEST_UNLINK _unlink
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define TEST_PID getpid
#define TEST_RMDIR rmdir
#define TEST_UNLINK unlink
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int failures;

#define CHECK(condition, ...)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "FAIL: ");                                \
            (void)fprintf(stderr, __VA_ARGS__);                              \
            (void)fputc('\n', stderr);                                       \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static int make_unused_path(char path[PATH_MAX])
{
    unsigned attempt;
#if defined(_WIN32)
    const char *temporary = getenv("TEMP");
    if (temporary == NULL || temporary[0] == '\0') temporary = ".";
#else
    const char *temporary = "/tmp";
#endif
    for (attempt = 0U; attempt < 100U; ++attempt) {
        int written = snprintf(path, PATH_MAX, "%s/hwa-events-test-%ld-%u",
                               temporary, (long)TEST_PID(), attempt);
        if (written < 0 || (size_t)written >= PATH_MAX) return 0;
#if defined(_WIN32)
        if (_mkdir(path) == 0) {
#else
        if (mkdir(path, 0700) == 0) {
#endif
            if (TEST_RMDIR(path) != 0) return 0;
            return 1;
        }
        if (errno != EEXIST) return 0;
    }
    return 0;
}

static void remove_bundle(const char *directory)
{
    static const char *const names[] = {
        "events.jsonl", "manifest.json", "traces.jsonl"
    };
    char path[PATH_MAX];
    size_t index;
    int written = snprintf(path, sizeof(path), "%s/traces/pitch.csv",
                           directory);
    if (written > 0 && (size_t)written < sizeof(path))
        (void)TEST_UNLINK(path);
    written = snprintf(path, sizeof(path), "%s/traces", directory);
    if (written > 0 && (size_t)written < sizeof(path))
        (void)TEST_RMDIR(path);
    for (index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
        written = snprintf(path, sizeof(path), "%s/%s",
                           directory, names[index]);
        if (written > 0 && (size_t)written < sizeof(path))
            (void)TEST_UNLINK(path);
    }
    (void)TEST_RMDIR(directory);
}

static void test_round_trip_preserves_exact_event(void)
{
    static const char zero_sha256[] =
        "00000000000000000000000000000000"
        "00000000000000000000000000000000";
    HWAEventBundleLimits limits;
    HWAEventAudio audio;
    HWAEventValue value;
    HWAPerformanceEvent event;
    HWAEventBundle source;
    HWAEventBundle loaded;
    char directory[PATH_MAX];
    char error[HWA_ERROR_SIZE] = {0};
    double exact_pitch = 440.00000000000006;

    memset(&audio, 0, sizeof(audio));
    memset(&value, 0, sizeof(value));
    memset(&event, 0, sizeof(event));
    memset(&source, 0, sizeof(source));
    memset(&loaded, 0, sizeof(loaded));
    CHECK(make_unused_path(directory), "cannot reserve bundle path");
    if (failures != 0) return;

    hwa_event_bundle_limits_default(&limits);
    audio.id = 1U;
    audio.kind = HWA_EVENT_SOURCE_RECORDING;
    audio.name = "quartet";
    audio.path_hint = "quartet.wav";
    memcpy(audio.sha256, zero_sha256, sizeof(audio.sha256));
    audio.format.container = HWA_CONTAINER_RIFF;
    audio.format.encoding = HWA_ENCODING_PCM;
    audio.format.channels = 2U;
    audio.format.sample_rate_hz = 48000U;
    audio.format.bits_per_sample = 24U;
    audio.format.valid_bits_per_sample = 24U;
    audio.format.block_align = 6U;
    audio.format.frames = 96000U;
    audio.format.data_bytes = 576000U;
    audio.format.duration_seconds = 2.0;

    value.name = "pitch-hz";
    value.kind = HWA_EVENT_VALUE_F64;
    value.basis = HWA_EVENT_OBSERVATION;
    value.number = exact_pitch;
    value.unit = "Hz";
    value.selected = 1;

    event.id = 7U;
    event.kind = "note";
    event.source_recording_id = 1U;
    event.evidence_audio_id = 1U;
    event.evidence_audio_id_valid = 1;
    event.start_sample = 123U;
    event.end_sample = 4567U;
    event.voice = "violin-1";
    event.part = "violin";
    event.score_event_id = "score-17";
    event.values = &value;
    event.value_count = 1U;

    source.audio = &audio;
    source.audio_count = 1U;
    source.events = &event;
    source.event_count = 1U;

    CHECK(hwa_event_bundle_validate(&source, &limits,
                                    error, sizeof(error)) == 0,
          "cannot validate event bundle in memory: %s", error);
    audio.relative_path = "audio/source/";
    audio.file_bytes = 1U;
    CHECK(hwa_event_bundle_validate(&source, &limits,
                                    error, sizeof(error)) != 0,
          "payload path with an empty final part was accepted");
    audio.relative_path = NULL;
    audio.file_bytes = 0U;
    CHECK(hwa_event_bundle_write(directory, &source, NULL, 0U, &limits,
                                 error, sizeof(error)) == 0,
          "cannot write event bundle: %s", error);
    CHECK(hwa_event_bundle_read(directory, &limits, &loaded,
                                error, sizeof(error)) == 0,
          "cannot read event bundle: %s", error);
    if (loaded.audio_count == 1U && loaded.event_count == 1U &&
        loaded.events[0].value_count == 1U) {
        CHECK(loaded.audio[0].format.sample_rate_hz == 48000U,
              "sample clock changed");
        CHECK(loaded.events[0].start_sample == 123U &&
                  loaded.events[0].end_sample == 4567U,
              "event sample bounds changed");
        CHECK(memcmp(&loaded.events[0].values[0].number, &exact_pitch,
                     sizeof(exact_pitch)) == 0,
              "binary64 pitch did not round trip");
        CHECK(strcmp(loaded.events[0].score_event_id, "score-17") == 0,
              "score link changed");
    } else {
        CHECK(0, "round trip returned the wrong row counts");
    }

    hwa_event_bundle_free(&loaded);
    remove_bundle(directory);
}

static void test_invalid_audio_format_enums_are_rejected(void)
{
    static const char zero_sha256[] =
        "00000000000000000000000000000000"
        "00000000000000000000000000000000";
    HWAEventBundleLimits limits;
    HWAEventAudio audio;
    HWAEventBundle bundle;
    char directory[PATH_MAX];
    char error[HWA_ERROR_SIZE] = {0};
    int write_result;

    memset(&audio, 0, sizeof(audio));
    memset(&bundle, 0, sizeof(bundle));
    CHECK(make_unused_path(directory),
          "cannot reserve invalid-format bundle path");
    if (failures != 0) return;

    hwa_event_bundle_limits_default(&limits);
    audio.id = 1U;
    audio.kind = HWA_EVENT_SOURCE_RECORDING;
    audio.name = "source";
    memcpy(audio.sha256, zero_sha256, sizeof(audio.sha256));
    audio.format.container = HWA_CONTAINER_RIFF;
    audio.format.encoding = HWA_ENCODING_PCM;
    audio.format.channels = 1U;
    audio.format.sample_rate_hz = 48000U;
    audio.format.bits_per_sample = 16U;
    audio.format.valid_bits_per_sample = 16U;
    audio.format.block_align = 2U;
    audio.format.frames = 1000U;
    audio.format.data_bytes = 2000U;
    audio.format.duration_seconds = 1.0 / 48.0;
    bundle.audio = &audio;
    bundle.audio_count = 1U;

    audio.format.container = (HWAContainer)0;
    CHECK(hwa_event_bundle_validate(&bundle, &limits,
                                    error, sizeof(error)) != 0,
          "invalid audio container passed validation");
    write_result = hwa_event_bundle_write(directory, &bundle, NULL, 0U,
                                           &limits, error, sizeof(error));
    CHECK(write_result != 0,
          "writer accepted an audio container rejected by validation");
    if (write_result == 0) remove_bundle(directory);

    audio.format.container = HWA_CONTAINER_RIFF;
    audio.format.encoding = (HWAEncoding)0;
    CHECK(hwa_event_bundle_validate(&bundle, &limits,
                                    error, sizeof(error)) != 0,
          "invalid audio encoding passed validation");
    write_result = hwa_event_bundle_write(directory, &bundle, NULL, 0U,
                                           &limits, error, sizeof(error));
    CHECK(write_result != 0,
          "writer accepted an audio encoding rejected by validation");
    if (write_result == 0) remove_bundle(directory);
}

static void test_child_event_must_fit_parent(void)
{
    static const char zero_sha256[] =
        "00000000000000000000000000000000"
        "00000000000000000000000000000000";
    HWAEventBundleLimits limits;
    HWAEventAudio audio;
    HWAPerformanceEvent events[2];
    HWAEventBundle bundle;
    char directory[PATH_MAX];
    char error[HWA_ERROR_SIZE] = {0};
    int write_result;

    memset(&audio, 0, sizeof(audio));
    memset(events, 0, sizeof(events));
    memset(&bundle, 0, sizeof(bundle));
    CHECK(make_unused_path(directory), "cannot reserve nested bundle path");
    if (failures != 0) return;

    hwa_event_bundle_limits_default(&limits);
    audio.id = 1U;
    audio.kind = HWA_EVENT_SOURCE_RECORDING;
    audio.name = "quartet";
    memcpy(audio.sha256, zero_sha256, sizeof(audio.sha256));
    audio.format.container = HWA_CONTAINER_RIFF;
    audio.format.encoding = HWA_ENCODING_PCM;
    audio.format.channels = 1U;
    audio.format.sample_rate_hz = 48000U;
    audio.format.bits_per_sample = 16U;
    audio.format.valid_bits_per_sample = 16U;
    audio.format.block_align = 2U;
    audio.format.frames = 1000U;
    audio.format.data_bytes = 2000U;
    audio.format.duration_seconds = 1.0 / 48.0;

    events[0].id = 1U;
    events[0].kind = "trill";
    events[0].source_recording_id = 1U;
    events[0].start_sample = 100U;
    events[0].end_sample = 200U;
    events[1].id = 2U;
    events[1].kind = "note";
    events[1].source_recording_id = 1U;
    events[1].parent_id = 1U;
    events[1].parent_id_valid = 1;
    events[1].start_sample = 50U;
    events[1].end_sample = 150U;

    bundle.audio = &audio;
    bundle.audio_count = 1U;
    bundle.events = events;
    bundle.event_count = 2U;
    write_result = hwa_event_bundle_write(directory, &bundle, NULL, 0U,
                                           &limits, error, sizeof(error));
    CHECK(write_result != 0, "child outside its parent was accepted");
    CHECK(strstr(error, "parent") != NULL,
          "parent failure was not clear: %s", error);
    if (write_result == 0) remove_bundle(directory);
}

static void test_parent_cycle_and_depth_are_rejected(void)
{
    static const char zero_sha256[] =
        "00000000000000000000000000000000"
        "00000000000000000000000000000000";
    HWAEventBundleLimits limits;
    HWAEventAudio audio;
    HWAPerformanceEvent events[3];
    HWAEventBundle bundle;
    char error[HWA_ERROR_SIZE] = {0};
    size_t index;

    memset(&audio, 0, sizeof(audio));
    memset(events, 0, sizeof(events));
    memset(&bundle, 0, sizeof(bundle));
    hwa_event_bundle_limits_default(&limits);

    audio.id = 1U;
    audio.kind = HWA_EVENT_SOURCE_RECORDING;
    audio.name = "quartet";
    memcpy(audio.sha256, zero_sha256, sizeof(audio.sha256));
    audio.format.container = HWA_CONTAINER_RIFF;
    audio.format.encoding = HWA_ENCODING_PCM;
    audio.format.channels = 1U;
    audio.format.sample_rate_hz = 48000U;
    audio.format.bits_per_sample = 16U;
    audio.format.valid_bits_per_sample = 16U;
    audio.format.block_align = 2U;
    audio.format.frames = 1000U;
    audio.format.data_bytes = 2000U;
    audio.format.duration_seconds = 1.0 / 48.0;

    for (index = 0U; index < 3U; ++index) {
        events[index].id = (uint64_t)index + 1U;
        events[index].kind = "note";
        events[index].source_recording_id = 1U;
        events[index].start_sample = 100U + (uint64_t)index;
        events[index].end_sample = 200U - (uint64_t)index;
        if (index != 0U) {
            events[index].parent_id = (uint64_t)index;
            events[index].parent_id_valid = 1;
        }
    }
    bundle.audio = &audio;
    bundle.audio_count = 1U;
    bundle.events = events;
    bundle.event_count = 3U;

    limits.max_nesting_depth = 1U;
    CHECK(hwa_event_bundle_validate(&bundle, &limits,
                                    error, sizeof(error)) != 0,
          "event nesting beyond the depth limit was accepted");
    CHECK(strstr(error, "nesting") != NULL,
          "nesting-depth failure was not clear: %s", error);
    limits.max_nesting_depth = 2U;
    CHECK(hwa_event_bundle_validate(&bundle, &limits,
                                    error, sizeof(error)) == 0,
          "event nesting at the depth limit was rejected: %s", error);

    events[0].start_sample = 100U;
    events[0].end_sample = 200U;
    events[1].start_sample = 100U;
    events[1].end_sample = 200U;
    events[0].parent_id = 2U;
    events[0].parent_id_valid = 1;
    events[1].parent_id = 1U;
    events[1].parent_id_valid = 1;
    events[2].parent_id_valid = 0;
    CHECK(hwa_event_bundle_validate(&bundle, &limits,
                                    error, sizeof(error)) != 0,
          "event parent cycle was accepted");
    CHECK(strstr(error, "nesting") != NULL,
          "parent-cycle failure was not clear: %s", error);
}

static void test_trace_payload_and_reference_round_trip(void)
{
    static const char zero_sha256[] =
        "00000000000000000000000000000000"
        "00000000000000000000000000000000";
    static const char trace_sha256[] =
        "683358d575b432af8809559258dfc7c7"
        "ee71b388c0fcd4d6f22c84569d5b074c";
    static const char trace_bytes[] = "440.00000000000006\n441\n";
    HWAEventBundleLimits limits;
    HWAEventAudio audio;
    HWAEventTrace trace;
    HWAEventTraceRef trace_ref;
    HWAPerformanceEvent event;
    HWAEventBundle source;
    HWAEventBundle loaded;
    HWAEventFileBinding binding;
    char trace_input[PATH_MAX];
    char directory[PATH_MAX];
    char error[HWA_ERROR_SIZE] = {0};
    FILE *stream;

    memset(&audio, 0, sizeof(audio));
    memset(&trace, 0, sizeof(trace));
    memset(&trace_ref, 0, sizeof(trace_ref));
    memset(&event, 0, sizeof(event));
    memset(&source, 0, sizeof(source));
    memset(&loaded, 0, sizeof(loaded));
    CHECK(make_unused_path(trace_input), "cannot reserve trace input path");
    if (failures != 0) return;
    stream = fopen(trace_input, "wb");
    CHECK(stream != NULL, "cannot create trace input");
    if (stream == NULL) return;
    CHECK(fwrite(trace_bytes, 1U, sizeof(trace_bytes) - 1U, stream) ==
              sizeof(trace_bytes) - 1U && fclose(stream) == 0,
          "cannot write trace input");
    CHECK(make_unused_path(directory), "cannot reserve trace bundle path");
    if (failures != 0) {
        (void)TEST_UNLINK(trace_input);
        return;
    }

    hwa_event_bundle_limits_default(&limits);
    audio.id = 1U;
    audio.kind = HWA_EVENT_SOURCE_RECORDING;
    audio.name = "violin";
    memcpy(audio.sha256, zero_sha256, sizeof(audio.sha256));
    audio.format.container = HWA_CONTAINER_RIFF;
    audio.format.encoding = HWA_ENCODING_PCM;
    audio.format.channels = 1U;
    audio.format.sample_rate_hz = 48000U;
    audio.format.bits_per_sample = 16U;
    audio.format.valid_bits_per_sample = 16U;
    audio.format.block_align = 2U;
    audio.format.frames = 10000U;
    audio.format.data_bytes = 20000U;
    audio.format.duration_seconds = 5.0 / 24.0;

    trace.id = 3U;
    trace.name = "pitch-hz";
    trace.unit = "Hz";
    trace.relative_path = "traces/pitch.csv";
    memcpy(trace.sha256, trace_sha256, sizeof(trace.sha256));
    trace.format = HWA_EVENT_TRACE_CSV_F64;
    trace.source_recording_id = 1U;
    trace.first_sample = 123U;
    trace.hop_samples = 64U;
    trace.window_samples = 2048U;
    trace.point_count = 2U;
    trace.value_width = 1U;
    trace.file_bytes = sizeof(trace_bytes) - 1U;

    trace_ref.trace_id = 3U;
    trace_ref.role = "pitch";
    trace_ref.first_point = 0U;
    trace_ref.point_count = 2U;
    event.id = 1U;
    event.kind = "note";
    event.source_recording_id = 1U;
    event.start_sample = 100U;
    event.end_sample = 5000U;
    event.trace_refs = &trace_ref;
    event.trace_ref_count = 1U;

    source.audio = &audio;
    source.audio_count = 1U;
    source.traces = &trace;
    source.trace_count = 1U;
    source.events = &event;
    source.event_count = 1U;
    binding.relative_path = "traces/pitch.csv";
    binding.source_path = trace_input;

    CHECK(hwa_event_bundle_write(directory, &source, &binding, 1U, &limits,
                                 error, sizeof(error)) == 0,
          "cannot write trace bundle: %s", error);
    (void)TEST_UNLINK(trace_input);
    CHECK(hwa_event_bundle_read(directory, &limits, &loaded,
                                error, sizeof(error)) == 0,
          "cannot read trace bundle: %s", error);
    if (loaded.trace_count == 1U && loaded.event_count == 1U &&
        loaded.events[0].trace_ref_count == 1U) {
        CHECK(loaded.traces[0].first_sample == 123U &&
                  loaded.traces[0].hop_samples == 64U,
              "trace clock changed");
        CHECK(loaded.events[0].trace_refs[0].trace_id == 3U &&
                  loaded.events[0].trace_refs[0].point_count == 2U,
              "trace reference changed");
    } else {
        CHECK(0, "trace round trip returned the wrong row counts");
    }
    hwa_event_bundle_free(&loaded);
    remove_bundle(directory);
}

static void test_competing_values_have_one_selection(void)
{
    static const char zero_sha256[] =
        "00000000000000000000000000000000"
        "00000000000000000000000000000000";
    HWAEventBundleLimits limits;
    HWAEventAudio audio;
    HWAEventValue values[2];
    HWAPerformanceEvent event;
    HWAEventBundle bundle;
    char directory[PATH_MAX];
    char error[HWA_ERROR_SIZE] = {0};
    int write_result;

    memset(&audio, 0, sizeof(audio));
    memset(values, 0, sizeof(values));
    memset(&event, 0, sizeof(event));
    memset(&bundle, 0, sizeof(bundle));
    CHECK(make_unused_path(directory), "cannot reserve candidate bundle path");
    if (failures != 0) return;

    hwa_event_bundle_limits_default(&limits);
    audio.id = 1U;
    audio.kind = HWA_EVENT_SOURCE_RECORDING;
    audio.name = "quartet";
    memcpy(audio.sha256, zero_sha256, sizeof(audio.sha256));
    audio.format.container = HWA_CONTAINER_RIFF;
    audio.format.encoding = HWA_ENCODING_PCM;
    audio.format.channels = 1U;
    audio.format.sample_rate_hz = 48000U;
    audio.format.bits_per_sample = 16U;
    audio.format.valid_bits_per_sample = 16U;
    audio.format.block_align = 2U;
    audio.format.frames = 1000U;
    audio.format.data_bytes = 2000U;
    audio.format.duration_seconds = 1.0 / 48.0;

    values[0].name = "instrument";
    values[0].kind = HWA_EVENT_VALUE_TEXT;
    values[0].basis = HWA_EVENT_INFERENCE;
    values[0].text = "violin";
    values[0].score = 0.75;
    values[0].score_valid = 1;
    values[0].selected = 1;
    values[1] = values[0];
    values[1].text = "viola";
    values[1].score = 0.70;

    event.id = 1U;
    event.kind = "note";
    event.source_recording_id = 1U;
    event.start_sample = 10U;
    event.end_sample = 100U;
    event.values = values;
    event.value_count = 2U;
    bundle.audio = &audio;
    bundle.audio_count = 1U;
    bundle.events = &event;
    bundle.event_count = 1U;

    write_result = hwa_event_bundle_write(directory, &bundle, NULL, 0U,
                                           &limits, error, sizeof(error));
    CHECK(write_result != 0, "two selected instrument guesses were accepted");
    CHECK(strstr(error, "selected") != NULL,
          "selection failure was not clear: %s", error);
    if (write_result == 0) remove_bundle(directory);
}

static void test_large_event_set_round_trip(void)
{
    enum { EVENT_COUNT = 20000, VALUE_COUNT = 4096 };
    static const char zero_sha256[] =
        "00000000000000000000000000000000"
        "00000000000000000000000000000000";
    HWAEventBundleLimits limits;
    HWAEventAudio audio;
    HWAPerformanceEvent *events;
    HWAEventValue *values;
    HWAEventBundle source;
    HWAEventBundle loaded;
    char directory[PATH_MAX];
    char error[HWA_ERROR_SIZE] = {0};
    size_t index;

    memset(&audio, 0, sizeof(audio));
    memset(&source, 0, sizeof(source));
    memset(&loaded, 0, sizeof(loaded));
    events = (HWAPerformanceEvent *)calloc(EVENT_COUNT, sizeof(*events));
    values = (HWAEventValue *)calloc(VALUE_COUNT, sizeof(*values));
    CHECK(events != NULL && values != NULL,
          "cannot allocate large round-trip fixture");
    if (events == NULL || values == NULL) {
        free(events);
        free(values);
        return;
    }
    CHECK(make_unused_path(directory), "cannot reserve large bundle path");
    if (failures != 0) {
        free(events);
        free(values);
        return;
    }

    hwa_event_bundle_limits_default(&limits);
    audio.id = 1U;
    audio.kind = HWA_EVENT_SOURCE_RECORDING;
    audio.name = "source";
    memcpy(audio.sha256, zero_sha256, sizeof(audio.sha256));
    audio.format.container = HWA_CONTAINER_RIFF;
    audio.format.encoding = HWA_ENCODING_PCM;
    audio.format.channels = 1U;
    audio.format.sample_rate_hz = 48000U;
    audio.format.bits_per_sample = 16U;
    audio.format.valid_bits_per_sample = 16U;
    audio.format.block_align = 2U;
    audio.format.frames = 2U;
    audio.format.data_bytes = 4U;
    audio.format.duration_seconds = 2.0 / 48000.0;
    for (index = 0U; index < EVENT_COUNT; ++index) {
        events[index].id = (uint64_t)index + 1U;
        events[index].kind = "note";
        events[index].source_recording_id = 1U;
        events[index].start_sample = 0U;
        events[index].end_sample = 1U;
    }
    for (index = 0U; index < VALUE_COUNT; ++index) {
        values[index].name = "pitch-hz";
        values[index].kind = HWA_EVENT_VALUE_F64;
        values[index].basis = HWA_EVENT_OBSERVATION;
        values[index].number = 440.0;
        values[index].unit = "Hz";
    }
    values[0].selected = 1;
    events[0].values = values;
    events[0].value_count = VALUE_COUNT;
    source.audio = &audio;
    source.audio_count = 1U;
    source.events = events;
    source.event_count = EVENT_COUNT;

    CHECK(hwa_event_bundle_write(directory, &source, NULL, 0U, &limits,
                                 error, sizeof(error)) == 0,
          "cannot write large event set: %s", error);
    if (failures == 0) {
        CHECK(hwa_event_bundle_read(directory, &limits, &loaded,
                                    error, sizeof(error)) == 0,
              "cannot read large event set: %s", error);
        CHECK(loaded.event_count == EVENT_COUNT &&
                  loaded.events[EVENT_COUNT - 1U].id == EVENT_COUNT &&
                  loaded.events[0].value_count == VALUE_COUNT,
              "large event row count or order changed");
    }
    hwa_event_bundle_free(&loaded);
    remove_bundle(directory);
    free(events);
    free(values);
}

int main(void)
{
    test_round_trip_preserves_exact_event();
    test_invalid_audio_format_enums_are_rejected();
    test_child_event_must_fit_parent();
    test_parent_cycle_and_depth_are_rejected();
    test_competing_values_have_one_selection();
    test_trace_payload_and_reference_round_trip();
    test_large_event_set_round_trip();
    if (failures != 0) {
        (void)fprintf(stderr, "%d event-bundle test(s) failed\n", failures);
        return 1;
    }
    (void)puts("event bundle tests passed");
    return 0;
}

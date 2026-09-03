#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "hlolli_wg_analyzer.h"
#include "sha256.h"

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
#define TEST_MKDIR(path) _mkdir(path)
#define TEST_RMDIR _rmdir
#define TEST_UNLINK _unlink
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define TEST_PID getpid
#define TEST_MKDIR(path) mkdir((path), 0700)
#define TEST_RMDIR rmdir
#define TEST_UNLINK unlink
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static const char zero_sha256[] =
    "00000000000000000000000000000000"
    "00000000000000000000000000000000";
static const char trace_sha256[] =
    "683358d575b432af8809559258dfc7c7"
    "ee71b388c0fcd4d6f22c84569d5b074c";
static const char trace_bytes[] = "440.00000000000006\n441\n";

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

typedef struct TestBundleFixture {
    HWAEventAudio audio;
    HWAPerformanceEvent event;
    HWAEventBundle bundle;
} TestBundleFixture;

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
        int written = snprintf(path, PATH_MAX,
                               "%s/hwa-events-conformance-%ld-%u",
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

static int join_path(char path[PATH_MAX],
                     const char *directory,
                     const char *relative_path)
{
    int written = snprintf(path, PATH_MAX, "%s/%s", directory, relative_path);
    return written >= 0 && (size_t)written < PATH_MAX;
}

static void remove_file_below(const char *directory, const char *relative_path)
{
    char path[PATH_MAX];
    if (join_path(path, directory, relative_path)) (void)TEST_UNLINK(path);
}

static void remove_directory_below(const char *directory,
                                   const char *relative_path)
{
    char path[PATH_MAX];
    if (join_path(path, directory, relative_path)) (void)TEST_RMDIR(path);
}

static void remove_bundle(const char *directory)
{
    remove_file_below(directory, "traces/pitch.csv");
    remove_file_below(directory, "traces/unsafe.csv");
    remove_file_below(directory, "unsafe.csv");
    remove_file_below(directory, "events.jsonl");
    remove_file_below(directory, "traces.jsonl");
    remove_file_below(directory, "manifest.json");
    remove_directory_below(directory, "traces");
    (void)TEST_RMDIR(directory);
}

static int write_trace_input(char path[PATH_MAX])
{
    FILE *stream;
    int okay;

    if (!make_unused_path(path)) return 0;
    stream = fopen(path, "wb");
    if (stream == NULL) return 0;
    okay = fwrite(trace_bytes, 1U, sizeof(trace_bytes) - 1U, stream) ==
            sizeof(trace_bytes) - 1U;
    if (fclose(stream) != 0) okay = 0;
    if (!okay) (void)TEST_UNLINK(path);
    return okay;
}

static int write_bytes(const char *path, const void *bytes, size_t size)
{
    FILE *stream = fopen(path, "wb");
    int okay;
    if (stream == NULL) return 0;
    okay = fwrite(bytes, 1U, size, stream) == size;
    if (fclose(stream) != 0) okay = 0;
    return okay;
}

static int replace_first_line_feed_with_carriage_return(const char *path)
{
    FILE *stream = fopen(path, "r+b");
    int byte;
    int okay = 0;
    if (stream == NULL) return 0;
    while ((byte = fgetc(stream)) != EOF) {
        if (byte == '\n') {
            if (fseek(stream, -1L, SEEK_CUR) == 0 &&
                fputc('\r', stream) != EOF) {
                okay = 1;
            }
            break;
        }
    }
    if (fclose(stream) != 0) okay = 0;
    return okay;
}

static void init_fixture(TestBundleFixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));

    fixture->audio.id = 1U;
    fixture->audio.kind = HWA_EVENT_SOURCE_RECORDING;
    fixture->audio.name = "source";
    memcpy(fixture->audio.sha256, zero_sha256,
           sizeof(fixture->audio.sha256));
    fixture->audio.format.container = HWA_CONTAINER_RIFF;
    fixture->audio.format.encoding = HWA_ENCODING_PCM;
    fixture->audio.format.channels = 1U;
    fixture->audio.format.sample_rate_hz = 48000U;
    fixture->audio.format.bits_per_sample = 16U;
    fixture->audio.format.valid_bits_per_sample = 16U;
    fixture->audio.format.block_align = 2U;
    fixture->audio.format.frames = 4096U;
    fixture->audio.format.data_bytes = 8192U;
    fixture->audio.format.duration_seconds = 4096.0 / 48000.0;

    fixture->event.id = 1U;
    fixture->event.kind = "note";
    fixture->event.source_recording_id = 1U;
    fixture->event.start_sample = 10U;
    fixture->event.end_sample = 900U;

    fixture->bundle.audio = &fixture->audio;
    fixture->bundle.audio_count = 1U;
    fixture->bundle.events = &fixture->event;
    fixture->bundle.event_count = 1U;
}

static void init_trace(HWAEventTrace *trace, char *relative_path)
{
    memset(trace, 0, sizeof(*trace));
    trace->id = 3U;
    trace->name = "pitch-hz";
    trace->unit = "Hz";
    trace->relative_path = relative_path;
    memcpy(trace->sha256, trace_sha256, sizeof(trace->sha256));
    trace->format = HWA_EVENT_TRACE_CSV_F64;
    trace->source_recording_id = 1U;
    trace->first_sample = 10U;
    trace->hop_samples = 64U;
    trace->window_samples = 128U;
    trace->point_count = 2U;
    trace->value_width = 1U;
    trace->file_bytes = sizeof(trace_bytes) - 1U;
}

static void test_unsafe_trace_path_rejected(void)
{
    TestBundleFixture fixture;
    HWAEventBundleLimits limits;
    HWAEventTrace trace;
    HWAEventFileBinding binding;
    char input_path[PATH_MAX] = {0};
    char output_path[PATH_MAX] = {0};
    char error[HWA_ERROR_SIZE] = {0};
    int write_result;

    init_fixture(&fixture);
    init_trace(&trace, "traces/../unsafe.csv");
    fixture.bundle.traces = &trace;
    fixture.bundle.trace_count = 1U;
    if (!write_trace_input(input_path)) {
        CHECK(0, "cannot create unsafe-path input");
        return;
    }
    if (!make_unused_path(output_path)) {
        CHECK(0, "cannot reserve unsafe-path output");
        (void)TEST_UNLINK(input_path);
        return;
    }
    binding.relative_path = trace.relative_path;
    binding.source_path = input_path;
    hwa_event_bundle_limits_default(&limits);

    write_result = hwa_event_bundle_write(output_path, &fixture.bundle,
                                           &binding, 1U, &limits,
                                           error, sizeof(error));
    CHECK(write_result != 0, "unsafe trace path was accepted");
    CHECK(error[0] != '\0', "unsafe trace path gave no error detail");

    (void)TEST_UNLINK(input_path);
    remove_bundle(output_path);
}

static void test_trace_grid_past_source_rejected(void)
{
    TestBundleFixture fixture;
    HWAEventBundleLimits limits;
    HWAEventTrace trace;
    HWAEventFileBinding binding;
    char input_path[PATH_MAX] = {0};
    char output_path[PATH_MAX] = {0};
    char error[HWA_ERROR_SIZE] = {0};
    int write_result;

    init_fixture(&fixture);
    init_trace(&trace, "traces/pitch.csv");
    trace.first_sample = 4000U;
    trace.hop_samples = 64U;
    trace.window_samples = 128U;
    fixture.bundle.traces = &trace;
    fixture.bundle.trace_count = 1U;
    if (!write_trace_input(input_path)) {
        CHECK(0, "cannot create grid input");
        return;
    }
    if (!make_unused_path(output_path)) {
        CHECK(0, "cannot reserve grid output");
        (void)TEST_UNLINK(input_path);
        return;
    }
    binding.relative_path = trace.relative_path;
    binding.source_path = input_path;
    hwa_event_bundle_limits_default(&limits);

    write_result = hwa_event_bundle_write(output_path, &fixture.bundle,
                                           &binding, 1U, &limits,
                                           error, sizeof(error));
    CHECK(write_result != 0, "trace grid past its source was accepted");

    (void)TEST_UNLINK(input_path);
    remove_bundle(output_path);
}

static void test_payload_size_mismatch_fails_before_target_creation(void)
{
    TestBundleFixture fixture;
    HWAEventBundleLimits limits;
    HWAEventTrace trace;
    HWAEventFileBinding binding;
    char input_path[PATH_MAX] = {0};
    char output_path[PATH_MAX] = {0};
    char target_path[PATH_MAX] = {0};
    char error[HWA_ERROR_SIZE] = {0};
    FILE *target;
    int write_result;

    init_fixture(&fixture);
    init_trace(&trace, "traces/pitch.csv");
    trace.file_bytes++;
    fixture.bundle.traces = &trace;
    fixture.bundle.trace_count = 1U;
    if (!write_trace_input(input_path)) {
        CHECK(0, "cannot create size-mismatch input");
        return;
    }
    if (!make_unused_path(output_path) ||
        !join_path(target_path, output_path, trace.relative_path)) {
        CHECK(0, "cannot reserve size-mismatch output");
        (void)TEST_UNLINK(input_path);
        return;
    }
    binding.relative_path = trace.relative_path;
    binding.source_path = input_path;
    hwa_event_bundle_limits_default(&limits);

    write_result = hwa_event_bundle_write(output_path, &fixture.bundle,
                                           &binding, 1U, &limits,
                                           error, sizeof(error));
    CHECK(write_result != 0, "payload source size mismatch was accepted");
    CHECK(strstr(error, "source size") != NULL,
          "payload size mismatch was not caught by preflight: %s", error);
    target = fopen(target_path, "rb");
    CHECK(target == NULL,
          "payload target appeared despite source-size preflight");
    if (target != NULL) (void)fclose(target);

    (void)TEST_UNLINK(input_path);
    remove_bundle(output_path);
}

static void test_missing_trace_reference_rejected(void)
{
    TestBundleFixture fixture;
    HWAEventBundleLimits limits;
    HWAEventTraceRef reference;
    char output_path[PATH_MAX] = {0};
    char error[HWA_ERROR_SIZE] = {0};
    int write_result;

    init_fixture(&fixture);
    memset(&reference, 0, sizeof(reference));
    reference.trace_id = 99U;
    reference.role = "pitch";
    reference.point_count = 1U;
    fixture.event.trace_refs = &reference;
    fixture.event.trace_ref_count = 1U;
    if (!make_unused_path(output_path)) {
        CHECK(0, "cannot reserve trace-ref output");
        return;
    }
    hwa_event_bundle_limits_default(&limits);

    write_result = hwa_event_bundle_write(output_path, &fixture.bundle,
                                           NULL, 0U, &limits,
                                           error, sizeof(error));
    CHECK(write_result != 0, "missing trace reference was accepted");

    remove_bundle(output_path);
}

static void test_provider_id_must_resolve(void)
{
    TestBundleFixture fixture;
    HWAEventBundleLimits limits;
    HWAEventValue value;
    char output_path[PATH_MAX] = {0};
    char error[HWA_ERROR_SIZE] = {0};
    int write_result;

    init_fixture(&fixture);
    memset(&value, 0, sizeof(value));
    value.name = "pitch-hz";
    value.kind = HWA_EVENT_VALUE_F64;
    value.basis = HWA_EVENT_INFERENCE;
    value.number = 440.0;
    value.unit = "Hz";
    value.provider_id = 77U;
    value.provider_id_valid = 1;
    value.selected = 1;
    fixture.event.values = &value;
    fixture.event.value_count = 1U;
    if (!make_unused_path(output_path)) {
        CHECK(0, "cannot reserve provider-ref output");
        return;
    }
    hwa_event_bundle_limits_default(&limits);

    write_result = hwa_event_bundle_write(output_path, &fixture.bundle,
                                           NULL, 0U, &limits,
                                           error, sizeof(error));
    CHECK(write_result != 0, "missing provider reference was accepted");

    remove_bundle(output_path);
}

static void test_provider_settings_require_lf_only_text(void)
{
    TestBundleFixture fixture;
    HWAEventBundleLimits limits;
    HWAEventProvider provider;
    char error[HWA_ERROR_SIZE] = {0};

    init_fixture(&fixture);
    memset(&provider, 0, sizeof(provider));
    provider.id = 1U;
    provider.name = "crlf-settings";
    provider.version = "1";
    provider.settings_json = "{\r\n\"threshold\":0.5\r\n}";
    fixture.bundle.providers = &provider;
    fixture.bundle.provider_count = 1U;
    hwa_event_bundle_limits_default(&limits);

    CHECK(hwa_event_bundle_validate(&fixture.bundle, &limits,
                                    error, sizeof(error)) != 0,
          "provider settings with carriage returns were accepted");
}

static void test_manifest_requires_lf_only_text(void)
{
    TestBundleFixture fixture;
    HWAEventBundleLimits limits;
    HWAEventBundle loaded;
    char output_path[PATH_MAX] = {0};
    char manifest_path[PATH_MAX];
    char error[HWA_ERROR_SIZE] = {0};

    init_fixture(&fixture);
    memset(&loaded, 0, sizeof(loaded));
    CHECK(make_unused_path(output_path),
          "cannot reserve carriage-return bundle path");
    if (failures != 0) return;
    hwa_event_bundle_limits_default(&limits);
    CHECK(hwa_event_bundle_write(output_path, &fixture.bundle, NULL, 0U,
                                 &limits, error, sizeof(error)) == 0 &&
              join_path(manifest_path, output_path, "manifest.json") &&
              replace_first_line_feed_with_carriage_return(manifest_path),
          "cannot make carriage-return manifest: %s", error);
    if (failures == 0) {
        error[0] = '\0';
        CHECK(hwa_event_bundle_read(output_path, &limits, &loaded,
                                    error, sizeof(error)) != 0,
              "manifest with a carriage-return line ending was accepted");
    }
    hwa_event_bundle_free(&loaded);
    remove_bundle(output_path);
}

static void test_large_event_set_validates(void)
{
    enum { EVENT_COUNT = 100000 };
    TestBundleFixture fixture;
    HWAEventBundleLimits limits;
    HWAPerformanceEvent *events;
    char error[HWA_ERROR_SIZE] = {0};
    size_t index;

    init_fixture(&fixture);
    events = (HWAPerformanceEvent *)calloc(EVENT_COUNT, sizeof(*events));
    CHECK(events != NULL, "cannot allocate large event fixture");
    if (events == NULL) return;
    for (index = 0U; index < EVENT_COUNT; ++index) {
        events[index].id = (uint64_t)index + 1U;
        events[index].kind = "note";
        events[index].source_recording_id = 1U;
        events[index].start_sample = 1U;
        events[index].end_sample = 2U;
    }
    fixture.bundle.events = events;
    fixture.bundle.event_count = EVENT_COUNT;
    hwa_event_bundle_limits_default(&limits);

    CHECK(hwa_event_bundle_validate(&fixture.bundle, &limits,
                                    error, sizeof(error)) == 0,
          "large event set failed validation: %s", error);
    free(events);
}

static void test_provider_and_warning_round_trip(void)
{
    static const char model_sha256[] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    TestBundleFixture fixture;
    HWAEventBundleLimits limits;
    HWAEventProvider provider;
    HWAEventValue value;
    HWAEventWarning warning;
    HWAEventBundle loaded;
    char output_path[PATH_MAX] = {0};
    char error[HWA_ERROR_SIZE] = {0};
    int write_result;
    int read_result = -1;

    init_fixture(&fixture);
    memset(&provider, 0, sizeof(provider));
    memset(&value, 0, sizeof(value));
    memset(&warning, 0, sizeof(warning));
    memset(&loaded, 0, sizeof(loaded));

    provider.id = 8U;
    provider.name = "fixed-test";
    provider.version = "1";
    memcpy(provider.model_sha256, model_sha256,
           sizeof(provider.model_sha256));
    provider.settings_json = "{\"constant\":440}";
    value.name = "pitch-hz";
    value.kind = HWA_EVENT_VALUE_F64;
    value.basis = HWA_EVENT_INFERENCE;
    value.number = 440.0;
    value.unit = "Hz";
    value.provider_id = 8U;
    value.provider_id_valid = 1;
    value.selected = 1;
    warning.id = 4U;
    warning.code = "low-confidence";
    warning.message = "The note has weak pitch support.";
    warning.event_id = 1U;
    warning.event_id_valid = 1;

    fixture.bundle.providers = &provider;
    fixture.bundle.provider_count = 1U;
    fixture.bundle.warnings = &warning;
    fixture.bundle.warning_count = 1U;
    fixture.event.values = &value;
    fixture.event.value_count = 1U;
    if (!make_unused_path(output_path)) {
        CHECK(0, "cannot reserve round-trip output");
        return;
    }
    hwa_event_bundle_limits_default(&limits);

    write_result = hwa_event_bundle_write(output_path, &fixture.bundle,
                                           NULL, 0U, &limits,
                                           error, sizeof(error));
    CHECK(write_result == 0, "cannot write provider bundle: %s", error);
    if (write_result == 0) {
        error[0] = '\0';
        read_result = hwa_event_bundle_read(output_path, &limits, &loaded,
                                            error, sizeof(error));
        CHECK(read_result == 0, "cannot read provider bundle: %s", error);
    }
    if (read_result == 0) {
        CHECK(loaded.provider_count == 1U,
              "provider count changed in round trip");
        CHECK(loaded.warning_count == 1U,
              "warning count changed in round trip");
        CHECK(loaded.event_count == 1U &&
                  loaded.events[0].value_count == 1U,
              "provider value was lost in round trip");
        if (loaded.provider_count == 1U) {
            CHECK(loaded.providers[0].name != NULL &&
                      strcmp(loaded.providers[0].name, "fixed-test") == 0,
                  "provider name changed");
            CHECK(loaded.providers[0].version != NULL &&
                      strcmp(loaded.providers[0].version, "1") == 0,
                  "provider version changed");
            CHECK(strcmp(loaded.providers[0].model_sha256,
                         model_sha256) == 0,
                  "provider model hash changed");
            CHECK(loaded.providers[0].settings_json != NULL &&
                      strcmp(loaded.providers[0].settings_json,
                             "{\"constant\":440}") == 0,
                  "provider settings changed");
        }
        if (loaded.warning_count == 1U) {
            CHECK(loaded.warnings[0].code != NULL &&
                      strcmp(loaded.warnings[0].code, "low-confidence") == 0,
                  "warning code changed");
            CHECK(loaded.warnings[0].message != NULL &&
                      strcmp(loaded.warnings[0].message,
                             "The note has weak pitch support.") == 0,
                  "warning message changed");
            CHECK(loaded.warnings[0].event_id_valid &&
                      loaded.warnings[0].event_id == 1U,
                  "warning event link changed");
        }
        if (loaded.event_count == 1U &&
            loaded.events[0].value_count == 1U) {
            CHECK(loaded.events[0].values[0].provider_id_valid &&
                      loaded.events[0].values[0].provider_id == 8U,
                  "value provider link changed");
        }
    }

    hwa_event_bundle_free(&loaded);
    remove_bundle(output_path);
}

static void test_trace_payload_hash_tamper_rejected(void)
{
    TestBundleFixture fixture;
    HWAEventBundleLimits limits;
    HWAEventTrace trace;
    HWAEventFileBinding binding;
    HWAEventBundle loaded;
    char input_path[PATH_MAX] = {0};
    char output_path[PATH_MAX] = {0};
    char payload_path[PATH_MAX] = {0};
    char error[HWA_ERROR_SIZE] = {0};
    FILE *stream;
    int write_result;

    init_fixture(&fixture);
    init_trace(&trace, "traces/pitch.csv");
    fixture.bundle.traces = &trace;
    fixture.bundle.trace_count = 1U;
    memset(&loaded, 0, sizeof(loaded));
    if (!write_trace_input(input_path)) {
        CHECK(0, "cannot create tamper input");
        return;
    }
    if (!make_unused_path(output_path)) {
        CHECK(0, "cannot reserve tamper output");
        (void)TEST_UNLINK(input_path);
        return;
    }
    binding.relative_path = trace.relative_path;
    binding.source_path = input_path;
    hwa_event_bundle_limits_default(&limits);

    write_result = hwa_event_bundle_write(output_path, &fixture.bundle,
                                           &binding, 1U, &limits,
                                           error, sizeof(error));
    CHECK(write_result == 0, "cannot write tamper bundle: %s", error);
    (void)TEST_UNLINK(input_path);
    if (write_result == 0) {
        int path_ok = join_path(payload_path, output_path,
                                "traces/pitch.csv");
        CHECK(path_ok, "tamper payload path is too long");
        stream = path_ok ? fopen(payload_path, "r+b") : NULL;
        CHECK(stream != NULL, "cannot open trace payload to tamper");
        if (stream != NULL) {
            int tampered = fputc('5', stream) != EOF;
            if (fclose(stream) != 0) tampered = 0;
            CHECK(tampered, "cannot tamper with trace payload");
            if (tampered) {
                error[0] = '\0';
                CHECK(hwa_event_bundle_read(output_path, &limits, &loaded,
                                            error, sizeof(error)) != 0,
                      "trace payload with a bad hash was accepted");
            }
        }
    }

    hwa_event_bundle_free(&loaded);
    remove_bundle(output_path);
}

static void test_non_finite_csv_trace_rejected(void)
{
    static const char bad_trace_bytes[] = "nan\n441\n";
    TestBundleFixture fixture;
    HWAEventBundleLimits limits;
    HWAEventTrace trace;
    HWAEventFileBinding binding;
    char input_path[PATH_MAX] = {0};
    char output_path[PATH_MAX] = {0};
    char error[HWA_ERROR_SIZE] = {0};
    FILE *stream;
    int input_ok;
    int write_result;

    init_fixture(&fixture);
    init_trace(&trace, "traces/pitch.csv");
    CHECK(make_unused_path(input_path), "cannot reserve bad trace input");
    if (failures != 0) return;
    stream = fopen(input_path, "wb");
    CHECK(stream != NULL, "cannot open bad trace input");
    if (stream == NULL) return;
    input_ok = fwrite(bad_trace_bytes, 1U,
                      sizeof(bad_trace_bytes) - 1U, stream) ==
               sizeof(bad_trace_bytes) - 1U;
    if (fclose(stream) != 0) input_ok = 0;
    CHECK(input_ok, "cannot write bad trace input");
    if (!input_ok) {
        (void)TEST_UNLINK(input_path);
        return;
    }
    CHECK(hwa_sha256_file(input_path, UINT64_MAX, trace.sha256,
                          error, sizeof(error)) == 0,
          "cannot hash bad trace input: %s", error);
    trace.file_bytes = sizeof(bad_trace_bytes) - 1U;
    fixture.bundle.traces = &trace;
    fixture.bundle.trace_count = 1U;
    CHECK(make_unused_path(output_path), "cannot reserve bad trace output");
    if (failures != 0) {
        (void)TEST_UNLINK(input_path);
        return;
    }
    binding.relative_path = trace.relative_path;
    binding.source_path = input_path;
    hwa_event_bundle_limits_default(&limits);

    write_result = hwa_event_bundle_write(output_path, &fixture.bundle,
                                           &binding, 1U, &limits,
                                           error, sizeof(error));
    CHECK(write_result != 0, "non-finite csv-f64 trace was accepted");
    CHECK(strstr(error, "csv-f64") != NULL,
          "bad csv-f64 error was unclear: %s", error);

    (void)TEST_UNLINK(input_path);
    remove_bundle(output_path);
}

static void test_value_key_order_and_unicode_escape(void)
{
    static const char events[] =
        "{\"id\":1,\"kind\":\"note\",\"source_recording_id\":1,"
        "\"evidence_audio_id\":null,\"parent_id\":null,"
        "\"start_sample\":1,\"end_sample\":100,\"voice\":\"\\u03bb\","
        "\"part\":\"\",\"score_event_id\":\"\",\"values\":[{"
        "\"value\":440.00000000000006,\"name\":\"pitch-hz\","
        "\"basis\":\"observation\",\"kind\":\"f64\",\"unit\":\"Hz\","
        "\"score\":null,\"provider_id\":null,\"selected\":true}],"
        "\"trace_refs\":[]}\n";
    static const char empty_sha256[] =
        "e3b0c44298fc1c149afbf4c8996fb924"
        "27ae41e4649b934ca495991b7852b855";
    HWAEventBundleLimits limits;
    HWAEventBundle loaded;
    char directory[PATH_MAX] = {0};
    char events_path[PATH_MAX];
    char traces_path[PATH_MAX];
    char manifest_path[PATH_MAX];
    char events_sha256[HWA_SHA256_HEX_SIZE];
    char manifest[4096];
    char error[HWA_ERROR_SIZE] = {0};
    int length;

    memset(&loaded, 0, sizeof(loaded));
    CHECK(make_unused_path(directory), "cannot reserve ordered-value bundle");
    if (failures != 0) return;
    CHECK(TEST_MKDIR(directory) == 0 &&
              join_path(events_path, directory, "events.jsonl") &&
              join_path(traces_path, directory, "traces.jsonl") &&
              join_path(manifest_path, directory, "manifest.json") &&
              write_bytes(events_path, events, sizeof(events) - 1U) &&
              write_bytes(traces_path, "", 0U) &&
              hwa_sha256_file(events_path, UINT64_MAX, events_sha256,
                              error, sizeof(error)) == 0,
          "cannot create ordered-value bundle files: %s", error);
    if (failures != 0) {
        remove_bundle(directory);
        return;
    }
    length = snprintf(
        manifest, sizeof(manifest),
        "{\"schema\":\"hwa-events\",\"schema_version\":1,"
        "\"tool_version\":\"fixture\",\"counts\":{\"providers\":0,"
        "\"audio\":1,\"events\":1,\"values\":1,\"traces\":0,"
        "\"trace_refs\":0,\"warnings\":0},\"providers\":[],"
        "\"audio\":[{\"id\":1,\"kind\":\"source-recording\","
        "\"name\":\"source\",\"relative_path\":\"\","
        "\"path_hint\":\"\",\"sha256\":\"%s\",\"file_bytes\":0,"
        "\"format\":{\"container\":\"riff\",\"encoding\":\"pcm\","
        "\"channels\":1,\"sample_rate_hz\":48000,"
        "\"bits_per_sample\":16,\"valid_bits_per_sample\":16,"
        "\"block_align\":2,\"channel_mask\":0,\"frames\":1000,"
        "\"data_bytes\":2000,"
        "\"duration_seconds\":0.020833333333333332},"
        "\"source_recording_id\":null}],\"warnings\":[],\"files\":[{"
        "\"relative_path\":\"events.jsonl\",\"file_bytes\":%zu,"
        "\"sha256\":\"%s\"},{\"relative_path\":\"traces.jsonl\","
        "\"file_bytes\":0,\"sha256\":\"%s\"}]}",
        zero_sha256, sizeof(events) - 1U, events_sha256, empty_sha256);
    CHECK(length > 0 && (size_t)length < sizeof(manifest) &&
              write_bytes(manifest_path, manifest, (size_t)length),
          "cannot write ordered-value manifest");
    hwa_event_bundle_limits_default(&limits);
    CHECK(hwa_event_bundle_read(directory, &limits, &loaded,
                                error, sizeof(error)) == 0,
          "value-before-kind or Unicode escape was rejected: %s", error);
    if (loaded.event_count == 1U && loaded.events[0].value_count == 1U) {
        CHECK(strcmp(loaded.events[0].voice, "\xce\xbb") == 0,
              "Unicode escape did not decode to UTF-8");
        CHECK(loaded.events[0].values[0].number == 440.00000000000006,
              "value-before-kind changed the exact number");
    }
    hwa_event_bundle_free(&loaded);
    remove_bundle(directory);
}

int main(void)
{
    test_unsafe_trace_path_rejected();
    test_trace_grid_past_source_rejected();
    test_payload_size_mismatch_fails_before_target_creation();
    test_missing_trace_reference_rejected();
    test_provider_id_must_resolve();
    test_provider_settings_require_lf_only_text();
    test_manifest_requires_lf_only_text();
    test_large_event_set_validates();
    test_provider_and_warning_round_trip();
    test_trace_payload_hash_tamper_rejected();
    test_non_finite_csv_trace_rejected();
    test_value_key_order_and_unicode_escape();
    if (failures != 0) {
        (void)fprintf(stderr, "%d event-bundle conformance test(s) failed\n",
                      failures);
        return 1;
    }
    (void)puts("event bundle conformance tests passed");
    return 0;
}

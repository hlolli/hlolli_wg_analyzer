#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "hlolli_wg_analyzer.h"
#include "sha256.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#include "windows_test_process.h"
#define TEST_PID _getpid
#define TEST_RMDIR _rmdir
#define TEST_UNLINK _unlink
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define TEST_PID getpid
#define TEST_RMDIR rmdir
#define TEST_UNLINK unlink
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct TestFiles {
    char bundle[PATH_MAX];
    char trace_input[PATH_MAX];
    char output[PATH_MAX];
    char errors[PATH_MAX];
} TestFiles;

static const char *analyzer_path;
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

static int test_unused_path(char path[PATH_MAX], const char *suffix)
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
                              "%s/hwa-event-cli-%ld-%u%s", root,
                              (long)TEST_PID(), attempt, suffix);
        FILE *stream;
        if (length < 0 || length >= PATH_MAX) return 0;
        stream = fopen(path, "rb");
        if (stream == NULL && errno == ENOENT) return 1;
        if (stream != NULL) (void)fclose(stream);
    }
    return 0;
}

static int test_join(char path[PATH_MAX],
                     const char *directory,
                     const char *name)
{
    int length = snprintf(path, PATH_MAX, "%s/%s", directory, name);
    return length >= 0 && length < PATH_MAX;
}

static void test_files_close(TestFiles *files)
{
    char path[PATH_MAX];
    if (test_join(path, files->bundle, "traces/pitch.csv"))
        (void)TEST_UNLINK(path);
    if (test_join(path, files->bundle, "traces"))
        (void)TEST_RMDIR(path);
    if (test_join(path, files->bundle, "events.jsonl"))
        (void)TEST_UNLINK(path);
    if (test_join(path, files->bundle, "traces.jsonl"))
        (void)TEST_UNLINK(path);
    if (test_join(path, files->bundle, "manifest.json"))
        (void)TEST_UNLINK(path);
    (void)TEST_RMDIR(files->bundle);
    (void)TEST_UNLINK(files->trace_input);
    (void)TEST_UNLINK(files->output);
    (void)TEST_UNLINK(files->errors);
}

static int test_write_file(const char *path, const void *bytes, size_t size)
{
    FILE *stream = fopen(path, "wb");
    int result;
    if (stream == NULL) return 0;
    result = fwrite(bytes, 1U, size, stream) == size;
    if (fclose(stream) != 0) result = 0;
    return result;
}

static int test_file_bytes(const char *path, uint64_t *bytes)
{
#if defined(_WIN32)
    struct _stat64 status;
    if (_stat64(path, &status) != 0 || status.st_size < 0) return 0;
#else
    struct stat status;
    if (stat(path, &status) != 0 || status.st_size < 0) return 0;
#endif
    *bytes = (uint64_t)status.st_size;
    return 1;
}

static int test_bundle_bytes(const TestFiles *files, uint64_t *total)
{
    static const char *const names[] = {
        "manifest.json", "events.jsonl", "traces.jsonl", "traces/pitch.csv"
    };
    char path[PATH_MAX];
    uint64_t sum = 0U;
    size_t index;
    for (index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
        uint64_t bytes;
        if (!test_join(path, files->bundle, names[index]) ||
            !test_file_bytes(path, &bytes) || UINT64_MAX - sum < bytes) {
            return 0;
        }
        sum += bytes;
    }
    *total = sum;
    return 1;
}

static int test_files_open(TestFiles *files)
{
    static const char trace_bytes[] = "440\n";
    static const char zero_sha256[] =
        "00000000000000000000000000000000"
        "00000000000000000000000000000000";
    HWAEventBundleLimits limits;
    HWAEventProvider provider;
    HWAEventAudio audio;
    HWAEventValue values[2];
    HWAEventTrace trace;
    HWAEventTraceRef trace_ref;
    HWAPerformanceEvent event;
    HWAEventWarning warning;
    HWAEventBundle bundle;
    HWAEventFileBinding binding;
    char trace_sha256[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE] = {0};

    memset(files, 0, sizeof(*files));
    memset(&provider, 0, sizeof(provider));
    memset(&audio, 0, sizeof(audio));
    memset(values, 0, sizeof(values));
    memset(&trace, 0, sizeof(trace));
    memset(&trace_ref, 0, sizeof(trace_ref));
    memset(&event, 0, sizeof(event));
    memset(&warning, 0, sizeof(warning));
    memset(&bundle, 0, sizeof(bundle));

    if (!test_unused_path(files->bundle, ".hwa-events") ||
        !test_unused_path(files->trace_input, ".csv") ||
        !test_unused_path(files->output, ".out") ||
        !test_unused_path(files->errors, ".err") ||
        !test_write_file(files->trace_input, trace_bytes,
                         sizeof(trace_bytes) - 1U) ||
        hwa_sha256_file(files->trace_input, sizeof(trace_bytes) - 1U,
                        trace_sha256, error, sizeof(error)) != 0) {
        test_files_close(files);
        return 0;
    }

    hwa_event_bundle_limits_default(&limits);
    provider.id = 1U;
    provider.name = "fixture";
    provider.version = "1";
    provider.settings_json = "{}";

    audio.id = 1U;
    audio.kind = HWA_EVENT_SOURCE_RECORDING;
    audio.name = "source";
    audio.relative_path = "";
    audio.path_hint = "source.wav";
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

    values[0].name = "pitch-hz";
    values[0].kind = HWA_EVENT_VALUE_F64;
    values[0].basis = HWA_EVENT_OBSERVATION;
    values[0].number = 440.0;
    values[0].unit = "Hz";
    values[0].provider_id = 1U;
    values[0].provider_id_valid = 1;
    values[0].selected = 1;
    values[1].name = "instrument";
    values[1].kind = HWA_EVENT_VALUE_TEXT;
    values[1].basis = HWA_EVENT_INFERENCE;
    values[1].text = "violin";
    values[1].unit = "";
    values[1].provider_id = 1U;
    values[1].provider_id_valid = 1;
    values[1].selected = 1;

    trace.id = 1U;
    trace.name = "pitch-hz";
    trace.unit = "Hz";
    trace.relative_path = "traces/pitch.csv";
    memcpy(trace.sha256, trace_sha256, sizeof(trace.sha256));
    trace.format = HWA_EVENT_TRACE_CSV_F64;
    trace.source_recording_id = 1U;
    trace.hop_samples = 1U;
    trace.window_samples = 1U;
    trace.point_count = 1U;
    trace.value_width = 1U;
    trace.file_bytes = sizeof(trace_bytes) - 1U;

    trace_ref.trace_id = 1U;
    trace_ref.role = "pitch";
    trace_ref.point_count = 1U;

    event.id = 1U;
    event.kind = "note";
    event.source_recording_id = 1U;
    event.start_sample = 0U;
    event.end_sample = 100U;
    event.voice = "solo";
    event.part = "violin";
    event.score_event_id = "";
    event.values = values;
    event.value_count = 2U;
    event.trace_refs = &trace_ref;
    event.trace_ref_count = 1U;

    warning.id = 1U;
    warning.code = "fixture-warning";
    warning.message = "Fixture warning.";
    warning.event_id = 1U;
    warning.event_id_valid = 1;

    bundle.providers = &provider;
    bundle.provider_count = 1U;
    bundle.audio = &audio;
    bundle.audio_count = 1U;
    bundle.traces = &trace;
    bundle.trace_count = 1U;
    bundle.events = &event;
    bundle.event_count = 1U;
    bundle.warnings = &warning;
    bundle.warning_count = 1U;
    binding.relative_path = trace.relative_path;
    binding.source_path = files->trace_input;
    if (hwa_event_bundle_write(files->bundle, &bundle, &binding, 1U,
                               &limits, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "cannot write CLI fixture: %s\n", error);
        test_files_close(files);
        return 0;
    }
    return 1;
}

static char *test_read_text(const char *path)
{
    FILE *stream = fopen(path, "rb");
    long length;
    char *text;
    int result;
    if (stream == NULL || fseek(stream, 0L, SEEK_END) != 0 ||
        (length = ftell(stream)) < 0L || fseek(stream, 0L, SEEK_SET) != 0) {
        if (stream != NULL) (void)fclose(stream);
        return NULL;
    }
    text = (char *)malloc((size_t)length + 1U);
    if (text == NULL) {
        (void)fclose(stream);
        return NULL;
    }
    result = fread(text, 1U, (size_t)length, stream) == (size_t)length;
    if (fclose(stream) != 0) result = 0;
    if (!result) {
        free(text);
        return NULL;
    }
    text[(size_t)length] = '\0';
    return text;
}

static int test_run(const TestFiles *files,
                    const char *const *arguments,
                    size_t argument_count)
{
#if defined(_WIN32)
    return hwa_test_spawn_redirected(analyzer_path, arguments, argument_count,
                                     NULL, files->output, files->errors);
#else
    pid_t child = fork();
    int status;
    if (child < 0) return -1;
    if (child == 0) {
        char **argv = (char **)calloc(argument_count + 2U, sizeof(*argv));
        int output;
        int errors;
        size_t index;
        if (argv == NULL) _exit(126);
        argv[0] = (char *)analyzer_path;
        for (index = 0U; index < argument_count; ++index)
            argv[index + 1U] = (char *)arguments[index];
        output = open(files->output, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        errors = open(files->errors, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (output < 0 || errors < 0 || dup2(output, STDOUT_FILENO) < 0 ||
            dup2(errors, STDERR_FILENO) < 0) _exit(126);
        (void)close(output);
        (void)close(errors);
        (void)execv(analyzer_path, argv);
        _exit(127);
    }
    if (waitpid(child, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#endif
}

static void test_valid_bundle_json(void)
{
    TestFiles files;
    const char *arguments[3];
    char *output;
    char *errors;
    char byte_field[96];
    uint64_t expected_bytes = 0U;
    CHECK(test_files_open(&files), "cannot open CLI test fixture");
    if (failures != 0) return;
    CHECK(test_bundle_bytes(&files, &expected_bytes),
          "cannot count fixture bytes");
    (void)snprintf(byte_field, sizeof(byte_field),
                   "\"total_file_bytes\":%" PRIu64, expected_bytes);
    arguments[0] = "--json";
    arguments[1] = "validate-event-bundle";
    arguments[2] = files.bundle;
    CHECK(test_run(&files, arguments, 3U) == 0,
          "valid bundle JSON command failed");
    output = test_read_text(files.output);
    errors = test_read_text(files.errors);
    CHECK(output != NULL &&
              strstr(output,
                     "\"schema\":\"hwa-event-bundle-validation\"") != NULL &&
              strstr(output, "\"schema_version\":1") != NULL &&
              strstr(output,
                     "\"command\":\"validate-event-bundle\"") != NULL &&
              strstr(output, "\"valid\":true") != NULL &&
              strstr(output,
                     "\"valid_means\":\"schema conformance\"") != NULL &&
              strstr(output,
                     "\"providers\":1,\"audio\":1,\"events\":1,"
                     "\"values\":2,\"traces\":1,\"trace_refs\":1,"
                     "\"warnings\":1") != NULL &&
              strstr(output, "\"retained_work_bytes\":") != NULL &&
              strstr(output, byte_field) != NULL,
          "JSON summary omitted its schema, validity, counts, or byte total: %s",
          output != NULL ? output : "(unreadable)");
    CHECK(errors != NULL && errors[0] == '\0',
          "valid JSON command wrote an error: %s",
          errors != NULL ? errors : "(unreadable)");
    free(output);
    free(errors);
    test_files_close(&files);
}

static void test_valid_bundle_text_states_scope(void)
{
    TestFiles files;
    const char *arguments[2];
    char *output;
    char *errors;
    char byte_line[96];
    uint64_t expected_bytes = 0U;
    CHECK(test_files_open(&files), "cannot open text CLI test fixture");
    if (failures != 0) return;
    CHECK(test_bundle_bytes(&files, &expected_bytes),
          "cannot count text fixture bytes");
    (void)snprintf(byte_line, sizeof(byte_line),
                   "Total file bytes: %" PRIu64, expected_bytes);
    arguments[0] = "validate-event-bundle";
    arguments[1] = files.bundle;
    CHECK(test_run(&files, arguments, 2U) == 0,
          "valid bundle text command failed");
    output = test_read_text(files.output);
    errors = test_read_text(files.errors);
    CHECK(output != NULL &&
              strstr(output, "Event bundle validation passed") != NULL &&
              strstr(output, "Valid means schema conformance.") != NULL &&
              strstr(output,
                     "Counts: 1 provider, 1 audio, 1 event, 2 values, "
                     "1 trace, 1 trace reference, 1 warning") != NULL &&
              strstr(output, "Retained work bytes: ") != NULL &&
              strstr(output, byte_line) != NULL,
          "text summary omitted its scope, counts, or byte total: %s",
          output != NULL ? output : "(unreadable)");
    CHECK(errors != NULL && errors[0] == '\0',
          "valid text command wrote an error: %s",
          errors != NULL ? errors : "(unreadable)");
    free(output);
    free(errors);
    test_files_close(&files);
}

static void test_invalid_bundle_has_no_success_report(void)
{
    TestFiles files;
    const char *arguments[2];
    char events_path[PATH_MAX];
    char *output;
    char *errors;
    FILE *stream;
    int altered;
    CHECK(test_files_open(&files), "cannot open invalid CLI test fixture");
    if (failures != 0) return;
    CHECK(test_join(events_path, files.bundle, "events.jsonl"),
          "invalid fixture path is too long");
    stream = fopen(events_path, "ab");
    if (stream == NULL) {
        CHECK(0, "cannot open events index for alteration");
        test_files_close(&files);
        return;
    }
    altered = fputc('\n', stream) != EOF;
    if (fclose(stream) != 0) altered = 0;
    CHECK(altered, "cannot alter events index");
    arguments[0] = "validate-event-bundle";
    arguments[1] = files.bundle;
    CHECK(test_run(&files, arguments, 2U) == 1,
          "changed bundle did not fail validation");
    output = test_read_text(files.output);
    errors = test_read_text(files.errors);
    CHECK(output != NULL && output[0] == '\0',
          "failed validation wrote a success report: %s",
          output != NULL ? output : "(unreadable)");
    CHECK(errors != NULL &&
              strstr(errors, "event index inventory mismatch") != NULL,
          "failed validation lacked a clear error: %s",
          errors != NULL ? errors : "(unreadable)");
    free(output);
    free(errors);
    test_files_close(&files);
}

static void test_command_rejects_every_other_option(void)
{
    TestFiles files;
    const char *missing[] = {"validate-event-bundle"};
    const char *extra[3];
    const char *stdin_path[] = {"validate-event-bundle", "-"};
    const char *output_option[4];
    const char *replace_option[3];
    const char *limit_option[4];
    CHECK(test_files_open(&files), "cannot open option CLI test fixture");
    if (failures != 0) return;
    extra[0] = "validate-event-bundle";
    extra[1] = files.bundle;
    extra[2] = "extra";
    output_option[0] = "validate-event-bundle";
    output_option[1] = files.bundle;
    output_option[2] = "--output";
    output_option[3] = files.output;
    replace_option[0] = "--replace";
    replace_option[1] = "validate-event-bundle";
    replace_option[2] = files.bundle;
    limit_option[0] = "--max-bytes";
    limit_option[1] = "1";
    limit_option[2] = "validate-event-bundle";
    limit_option[3] = files.bundle;
    CHECK(test_run(&files, missing, 1U) == 2,
          "missing directory was not command misuse");
    CHECK(test_run(&files, extra, 3U) == 2,
          "extra argument was not command misuse");
    CHECK(test_run(&files, stdin_path, 2U) == 2,
          "stdin was accepted as an event bundle");
    CHECK(test_run(&files, output_option, 4U) == 2,
          "--output was accepted");
    CHECK(test_run(&files, replace_option, 3U) == 2,
          "--replace was accepted");
    CHECK(test_run(&files, limit_option, 4U) == 2,
          "--max-bytes was accepted or ignored");
    test_files_close(&files);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        (void)fputs("usage: event_bundle_cli_tests ANALYZER\n", stderr);
        return 2;
    }
    analyzer_path = argv[1];
    test_valid_bundle_json();
    test_valid_bundle_text_states_scope();
    test_invalid_bundle_has_no_success_report();
    test_command_rejects_every_other_option();
    if (failures != 0) {
        (void)fprintf(stderr, "%d event-bundle CLI test(s) failed\n", failures);
        return 1;
    }
    return 0;
}

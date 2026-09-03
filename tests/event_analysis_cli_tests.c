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
#include <sys/stat.h>
#include "windows_test_process.h"
#define TEST_MKDIR(path) _mkdir(path)
#define TEST_PID() ((long)_getpid())
#define TEST_RMDIR(path) _rmdir(path)
#define TEST_UNLINK(path) _unlink(path)
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define TEST_MKDIR(path) mkdir((path), 0700)
#define TEST_PID() ((long)getpid())
#define TEST_RMDIR(path) rmdir(path)
#define TEST_UNLINK(path) unlink(path)
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TEST_RATE UINT32_C(8000)
#define TEST_FRAMES UINT32_C(32768)
#define TEST_FIRST_START UINT32_C(4096)
#define TEST_FIRST_END UINT32_C(14336)
#define TEST_SECOND_START UINT32_C(17408)
#define TEST_SECOND_END UINT32_C(28672)
#define TEST_TRACE_COUNT 9U
#define TEST_TRACE_POINTS UINT64_C(61)

typedef struct TestWorkspace {
    char directory[PATH_MAX];
    char input[PATH_MAX];
    char bundle[PATH_MAX];
    char rejected_bundle[PATH_MAX];
    char existing_bundle[PATH_MAX];
    char sentinel[PATH_MAX];
    char output[PATH_MAX];
    char errors[PATH_MAX];
} TestWorkspace;

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

static int test_join(char path[PATH_MAX],
                     const char *directory,
                     const char *name)
{
    int length = snprintf(path, PATH_MAX, "%s/%s", directory, name);
    return length >= 0 && (size_t)length < PATH_MAX;
}

static int test_path_exists(const char *path)
{
#if defined(_WIN32)
    struct _stat64 status;
    return _stat64(path, &status) == 0;
#else
    struct stat status;
    return stat(path, &status) == 0;
#endif
}

static int test_write_bytes(FILE *stream, const void *bytes, size_t size)
{
    return fwrite(bytes, 1U, size, stream) == size;
}

static int test_write_u16(FILE *stream, uint16_t value)
{
    unsigned char bytes[2];
    bytes[0] = (unsigned char)(value & UINT16_C(0xff));
    bytes[1] = (unsigned char)(value >> 8U);
    return test_write_bytes(stream, bytes, sizeof(bytes));
}

static int test_write_u32(FILE *stream, uint32_t value)
{
    unsigned char bytes[4];
    bytes[0] = (unsigned char)(value & UINT32_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT32_C(0xff));
    bytes[2] = (unsigned char)((value >> 16U) & UINT32_C(0xff));
    bytes[3] = (unsigned char)(value >> 24U);
    return test_write_bytes(stream, bytes, sizeof(bytes));
}

static int16_t test_signal_sample(uint32_t frame)
{
    static const int16_t first_cycle[16] = {
        16384, 15137, 11585, 6270, 0, -6270, -11585, -15137,
        -16384, -15137, -11585, -6270, 0, 6270, 11585, 15137
    };
    static const int16_t second_cycle[8] = {
        8192, 5793, 0, -5793, -8192, -5793, 0, 5793
    };
    if (frame >= TEST_FIRST_START && frame < TEST_FIRST_END)
        return first_cycle[(frame - TEST_FIRST_START) % UINT32_C(16)];
    if (frame >= TEST_SECOND_START && frame < TEST_SECOND_END)
        return second_cycle[(frame - TEST_SECOND_START) % UINT32_C(8)];
    return 0;
}

static int test_write_wave(const char *path)
{
    const uint32_t data_bytes = TEST_FRAMES * UINT32_C(2);
    FILE *stream = fopen(path, "wb");
    uint32_t frame;
    int result = 0;
    if (stream == NULL) return 0;
    if (!test_write_bytes(stream, "RIFF", 4U) ||
        !test_write_u32(stream, UINT32_C(36) + data_bytes) ||
        !test_write_bytes(stream, "WAVEfmt ", 8U) ||
        !test_write_u32(stream, UINT32_C(16)) ||
        !test_write_u16(stream, UINT16_C(1)) ||
        !test_write_u16(stream, UINT16_C(1)) ||
        !test_write_u32(stream, TEST_RATE) ||
        !test_write_u32(stream, TEST_RATE * UINT32_C(2)) ||
        !test_write_u16(stream, UINT16_C(2)) ||
        !test_write_u16(stream, UINT16_C(16)) ||
        !test_write_bytes(stream, "data", 4U) ||
        !test_write_u32(stream, data_bytes)) {
        goto cleanup;
    }
    for (frame = 0U; frame < TEST_FRAMES; ++frame) {
        if (!test_write_u16(stream, (uint16_t)test_signal_sample(frame)))
            goto cleanup;
    }
    result = 1;
cleanup:
    if (fclose(stream) != 0) result = 0;
    return result;
}

static int test_write_file(const char *path, const void *bytes, size_t size)
{
    FILE *stream = fopen(path, "wb");
    int result;
    if (stream == NULL) return 0;
    result = test_write_bytes(stream, bytes, size);
    if (fclose(stream) != 0) result = 0;
    return result;
}

static int test_file_equals(const char *path, const void *bytes, size_t size)
{
    unsigned char buffer[128];
    FILE *stream;
    int result;
    if (size > sizeof(buffer)) return 0;
    stream = fopen(path, "rb");
    if (stream == NULL) return 0;
    result = fread(buffer, 1U, size, stream) == size &&
             memcmp(buffer, bytes, size) == 0 && fgetc(stream) == EOF &&
             !ferror(stream);
    if (fclose(stream) != 0) result = 0;
    return result;
}

static int test_file_is_empty(const char *path)
{
    FILE *stream = fopen(path, "rb");
    int result;
    if (stream == NULL) return 0;
    result = fgetc(stream) == EOF && !ferror(stream);
    if (fclose(stream) != 0) result = 0;
    return result;
}

static int test_file_contains(const char *path, const char *needle)
{
    char buffer[4096];
    FILE *stream = fopen(path, "rb");
    size_t count;
    int result;
    if (stream == NULL) return 0;
    count = fread(buffer, 1U, sizeof(buffer) - 1U, stream);
    buffer[count] = '\0';
    result = !ferror(stream) && strstr(buffer, needle) != NULL;
    if (fclose(stream) != 0) result = 0;
    return result;
}

static int test_make_workspace(TestWorkspace *workspace)
{
    unsigned attempt;
    const char *root;
    memset(workspace, 0, sizeof(*workspace));
#if defined(_WIN32)
    root = getenv("TEMP");
    if (root == NULL || root[0] == '\0') root = ".";
#else
    root = "/tmp";
#endif
    for (attempt = 0U; attempt < 100U; ++attempt) {
        int length = snprintf(workspace->directory, PATH_MAX,
                              "%s/hwa-event-analysis-cli-%ld-%u",
                              root, TEST_PID(), attempt);
        if (length < 0 || (size_t)length >= PATH_MAX) return 0;
        if (TEST_MKDIR(workspace->directory) == 0) break;
        if (errno != EEXIST) return 0;
    }
    if (attempt == 100U ||
        !test_join(workspace->input, workspace->directory, "input.wav") ||
        !test_join(workspace->bundle, workspace->directory,
                   "result.hwa-events") ||
        !test_join(workspace->rejected_bundle, workspace->directory,
                   "rejected.hwa-events") ||
        !test_join(workspace->existing_bundle, workspace->directory,
                   "existing.hwa-events") ||
        !test_join(workspace->sentinel, workspace->existing_bundle,
                   "sentinel.txt") ||
        !test_join(workspace->output, workspace->directory, "stdout.txt") ||
        !test_join(workspace->errors, workspace->directory, "stderr.txt") ||
        !test_write_wave(workspace->input)) {
        return 0;
    }
    return 1;
}

static int test_run(const TestWorkspace *workspace,
                    const char *const *arguments,
                    size_t argument_count)
{
#if defined(_WIN32)
    return hwa_test_spawn_redirected(analyzer_path, arguments, argument_count,
                                     NULL, workspace->output,
                                     workspace->errors);
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
        output = open(workspace->output,
                      O_CREAT | O_TRUNC | O_WRONLY, 0600);
        errors = open(workspace->errors,
                      O_CREAT | O_TRUNC | O_WRONLY, 0600);
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

static void test_remove_bundle(const char *directory)
{
    static const char *const trace_paths[] = {
        "traces/rms-dbfs.f64le",
        "traces/pitch-hz.f64le",
        "traces/pitch-confidence.f64le",
        "traces/pitch-valid.f64le",
        "traces/onset-strength.f64le",
        "traces/spectral-centroid-hz.f64le",
        "traces/spectral-rolloff-85-hz.f64le",
        "traces/spectral-flatness.f64le",
        "traces/spectrum-valid.f64le"
    };
    static const char *const root_paths[] = {
        "manifest.json", "events.jsonl", "traces.jsonl"
    };
    char path[PATH_MAX];
    size_t index;
    if (directory == NULL || directory[0] == '\0') return;
    for (index = 0U;
         index < sizeof(trace_paths) / sizeof(trace_paths[0]); ++index) {
        if (test_join(path, directory, trace_paths[index]))
            (void)TEST_UNLINK(path);
    }
    for (index = 0U;
         index < sizeof(root_paths) / sizeof(root_paths[0]); ++index) {
        if (test_join(path, directory, root_paths[index]))
            (void)TEST_UNLINK(path);
    }
    if (test_join(path, directory, "traces")) (void)TEST_RMDIR(path);
    if (test_join(path, directory, "audio")) (void)TEST_RMDIR(path);
    (void)TEST_RMDIR(directory);
}

static void test_workspace_close(TestWorkspace *workspace)
{
    (void)TEST_UNLINK(workspace->sentinel);
    test_remove_bundle(workspace->bundle);
    test_remove_bundle(workspace->rejected_bundle);
    test_remove_bundle(workspace->existing_bundle);
    (void)TEST_UNLINK(workspace->input);
    (void)TEST_UNLINK(workspace->output);
    (void)TEST_UNLINK(workspace->errors);
    (void)TEST_RMDIR(workspace->directory);
}

static void test_analyze_events_writes_readable_bundle(
    const TestWorkspace *workspace)
{
    const char *arguments[] = {
        "analyze-events", workspace->input,
        "--output", workspace->bundle
    };
    HWAEventBundleLimits limits;
    HWAEventBundle bundle;
    char input_sha256[HWA_SHA256_HEX_SIZE] = {0};
    char error[HWA_ERROR_SIZE] = {0};
    size_t index;
    int read_result;
    memset(&bundle, 0, sizeof(bundle));
    CHECK(hwa_sha256_file(workspace->input, UINT64_MAX, input_sha256,
                          error, sizeof(error)) == 0,
          "cannot hash analyze-events input: %s", error);
    CHECK(test_run(workspace, arguments,
                   sizeof(arguments) / sizeof(arguments[0])) == 0,
          "analyze-events command failed");
    CHECK(test_file_is_empty(workspace->output),
          "successful analyze-events wrote unexpected standard output");
    CHECK(test_file_is_empty(workspace->errors),
          "successful analyze-events wrote unexpected standard error");
    hwa_event_bundle_limits_default(&limits);
    read_result = hwa_event_bundle_read(workspace->bundle, &limits, &bundle,
                                        error, sizeof(error));
    CHECK(read_result == 0, "cannot read analyze-events output: %s", error);
    if (read_result == 0) {
        CHECK(bundle.provider_count == 1U && bundle.providers != NULL &&
                  bundle.providers[0].id == UINT64_C(1) &&
                  bundle.providers[0].name != NULL &&
                  strcmp(bundle.providers[0].name,
                         "org.hlolli.monophonic-analysis") == 0 &&
                  bundle.providers[0].version != NULL &&
                  strcmp(bundle.providers[0].version, "1") == 0 &&
                  bundle.providers[0].model_sha256[0] == '\0' &&
                  bundle.providers[0].settings_json != NULL &&
                  strstr(bundle.providers[0].settings_json,
                         "\"algorithm\":\"monophonic-v1\"") != NULL &&
                  strstr(bundle.providers[0].settings_json,
                         "\"frame_size\":2048") != NULL &&
                  strstr(bundle.providers[0].settings_json,
                         "\"hop_size\":512") != NULL,
              "analyze-events saved the wrong provider");
        CHECK(bundle.audio_count == 1U && bundle.audio != NULL &&
                  bundle.audio[0].id == UINT64_C(1) &&
                  bundle.audio[0].kind == HWA_EVENT_SOURCE_RECORDING &&
                  bundle.audio[0].name != NULL &&
                  strcmp(bundle.audio[0].name, "input.wav") == 0 &&
                  bundle.audio[0].relative_path != NULL &&
                  bundle.audio[0].relative_path[0] == '\0' &&
                  bundle.audio[0].path_hint != NULL &&
                  strcmp(bundle.audio[0].path_hint, workspace->input) == 0 &&
                  strcmp(bundle.audio[0].sha256, input_sha256) == 0 &&
                  bundle.audio[0].file_bytes ==
                      UINT64_C(44) + (uint64_t)TEST_FRAMES * UINT64_C(2) &&
                  bundle.audio[0].format.channels == 1U &&
                  bundle.audio[0].format.sample_rate_hz == TEST_RATE &&
                  bundle.audio[0].format.bits_per_sample == 16U &&
                  bundle.audio[0].format.block_align == 2U &&
                  bundle.audio[0].format.frames == TEST_FRAMES,
              "analyze-events changed source clock facts");
        CHECK(bundle.event_count > 0U && bundle.events != NULL,
              "analyze-events saved no events");
        CHECK(bundle.trace_count == TEST_TRACE_COUNT && bundle.traces != NULL,
              "analyze-events saved %zu traces, expected %u",
              bundle.trace_count, (unsigned)TEST_TRACE_COUNT);
        for (index = 0U; index < bundle.event_count; ++index) {
            CHECK(bundle.events[index].start_sample <
                      bundle.events[index].end_sample &&
                      bundle.events[index].end_sample <= TEST_FRAMES &&
                      bundle.events[index].value_count >= 3U &&
                      bundle.events[index].trace_ref_count == TEST_TRACE_COUNT,
                  "event %zu has invalid bounds, values, or trace links", index);
        }
        for (index = 0U; index < bundle.trace_count; ++index) {
            CHECK(bundle.traces[index].id == (uint64_t)index + UINT64_C(1) &&
                      bundle.traces[index].relative_path != NULL &&
                      bundle.traces[index].relative_path[0] != '\0' &&
                      bundle.traces[index].format == HWA_EVENT_TRACE_F64LE &&
                      bundle.traces[index].source_recording_id == UINT64_C(1) &&
                      bundle.traces[index].first_sample == UINT64_C(0) &&
                      bundle.traces[index].hop_samples == UINT64_C(512) &&
                      bundle.traces[index].window_samples == UINT64_C(2048) &&
                      bundle.traces[index].point_count == TEST_TRACE_POINTS &&
                      bundle.traces[index].value_width == UINT32_C(1) &&
                      bundle.traces[index].file_bytes ==
                          TEST_TRACE_POINTS * UINT64_C(8),
                  "trace %zu has the wrong path, format, or clock", index);
        }
    }
    hwa_event_bundle_free(&bundle);
}

static void test_accepted_options_reach_saved_settings(
    const TestWorkspace *workspace)
{
    const char *mix_arguments[] = {
        "--mixdown", "--block-frames", "1024", "--frame-size", "1024",
        "--hop-size", "256", "--silence-threshold", "-50",
        "--max-bytes", "100000", "--max-frames", "40000",
        "--max-work-bytes", "33554432", "--max-transforms", "1000",
        "--max-track-points", "200", "analyze-events", workspace->input,
        "--output", workspace->rejected_bundle
    };
    const char *channel_arguments[] = {
        "--channel", "1", "analyze-events", workspace->input,
        "--output", workspace->rejected_bundle
    };
    HWAEventBundleLimits limits;
    HWAEventBundle bundle;
    char error[HWA_ERROR_SIZE] = {0};
    int read_result;
    memset(&bundle, 0, sizeof(bundle));
    CHECK(test_run(workspace, mix_arguments,
                   sizeof(mix_arguments) / sizeof(mix_arguments[0])) == 0,
          "analyze-events rejected supported custom options");
    hwa_event_bundle_limits_default(&limits);
    read_result = hwa_event_bundle_read(workspace->rejected_bundle, &limits,
                                        &bundle, error, sizeof(error));
    CHECK(read_result == 0, "cannot read custom-option bundle: %s", error);
    if (read_result == 0) {
        const char *settings = bundle.providers[0].settings_json;
        CHECK(settings != NULL &&
                  strstr(settings, "\"channel_mode\":\"mix\"") != NULL &&
                  strstr(settings, "\"selected_channel\":0") != NULL &&
                  strstr(settings, "\"frame_size\":1024") != NULL &&
                  strstr(settings, "\"hop_size\":256") != NULL &&
                  strstr(settings,
                         "\"silence_threshold_dbfs\":-50") != NULL,
              "custom options did not reach saved settings");
    }
    hwa_event_bundle_free(&bundle);
    test_remove_bundle(workspace->rejected_bundle);

    memset(&bundle, 0, sizeof(bundle));
    error[0] = '\0';
    CHECK(test_run(workspace, channel_arguments,
                   sizeof(channel_arguments) /
                       sizeof(channel_arguments[0])) == 0,
          "analyze-events rejected a valid selected channel");
    read_result = hwa_event_bundle_read(workspace->rejected_bundle, &limits,
                                        &bundle, error, sizeof(error));
    CHECK(read_result == 0, "cannot read selected-channel bundle: %s", error);
    if (read_result == 0) {
        const char *settings = bundle.providers[0].settings_json;
        CHECK(settings != NULL &&
                  strstr(settings, "\"channel_mode\":\"select\"") != NULL &&
                  strstr(settings, "\"selected_channel\":1") != NULL,
              "selected channel did not reach saved settings");
    }
    hwa_event_bundle_free(&bundle);
    test_remove_bundle(workspace->rejected_bundle);
}

static void test_command_misuse_is_rejected(const TestWorkspace *workspace)
{
    const char *stdin_input[] = {
        "analyze-events", "-", "--output", workspace->rejected_bundle
    };
    const char *stdout_output[] = {
        "analyze-events", workspace->input, "--output", "-"
    };
    const char *json[] = {
        "--json", "analyze-events", workspace->input,
        "--output", workspace->rejected_bundle
    };
    const char *replace[] = {
        "--replace", "analyze-events", workspace->input,
        "--output", workspace->rejected_bundle
    };
    const char *missing_output[] = {
        "analyze-events", workspace->input
    };
    const char *unrelated[] = {
        "analyze-events", workspace->input, "--score", workspace->input,
        "--output", workspace->rejected_bundle
    };
    const char *channel_conflict[] = {
        "--channel", "1", "--mixdown", "analyze-events", workspace->input,
        "--output", workspace->rejected_bundle
    };
    const char *bad_frame[] = {
        "--frame-size", "257", "analyze-events", workspace->input,
        "--output", workspace->rejected_bundle
    };
    const char *bad_hop[] = {
        "--frame-size", "256", "--hop-size", "257", "analyze-events",
        workspace->input, "--output", workspace->rejected_bundle
    };
    const char *bad_block[] = {
        "--block-frames", "1048577", "analyze-events", workspace->input,
        "--output", workspace->rejected_bundle
    };
    const char *bad_silence[] = {
        "--silence-threshold", "1", "analyze-events", workspace->input,
        "--output", workspace->rejected_bundle
    };
    const char *spectrum_values[] = {
        "--max-spectrum-values", "1", "analyze-events", workspace->input,
        "--output", workspace->rejected_bundle
    };
    const char *max_lag[] = {
        "--max-lag", "1", "analyze-events", workspace->input,
        "--output", workspace->rejected_bundle
    };
    const char *true_peak[] = {
        "--true-peak-oversample", "1", "analyze-events", workspace->input,
        "--output", workspace->rejected_bundle
    };
    CHECK(test_run(workspace, stdin_input,
                   sizeof(stdin_input) / sizeof(stdin_input[0])) == 2,
          "analyze-events accepted standard input");
    CHECK(test_run(workspace, stdout_output,
                   sizeof(stdout_output) / sizeof(stdout_output[0])) == 2,
          "analyze-events accepted standard output");
    CHECK(test_run(workspace, json, sizeof(json) / sizeof(json[0])) == 2,
          "analyze-events accepted --json");
    CHECK(test_run(workspace, replace,
                   sizeof(replace) / sizeof(replace[0])) == 2,
          "analyze-events accepted --replace");
    CHECK(test_run(workspace, missing_output,
                   sizeof(missing_output) / sizeof(missing_output[0])) == 2,
          "analyze-events accepted a missing output");
    CHECK(test_run(workspace, unrelated,
                   sizeof(unrelated) / sizeof(unrelated[0])) == 2,
          "analyze-events accepted an unrelated option");
    CHECK(test_run(workspace, channel_conflict,
                   sizeof(channel_conflict) / sizeof(channel_conflict[0])) == 2,
          "analyze-events accepted conflicting channel options");
    CHECK(test_run(workspace, bad_frame,
                   sizeof(bad_frame) / sizeof(bad_frame[0])) == 2,
          "analyze-events accepted a non-power-of-two frame size");
    CHECK(test_run(workspace, bad_hop,
                   sizeof(bad_hop) / sizeof(bad_hop[0])) == 2,
          "analyze-events accepted a hop larger than its frame");
    CHECK(test_run(workspace, bad_block,
                   sizeof(bad_block) / sizeof(bad_block[0])) == 2,
          "analyze-events accepted an oversized decode block");
    CHECK(test_run(workspace, bad_silence,
                   sizeof(bad_silence) / sizeof(bad_silence[0])) == 2,
          "analyze-events accepted an out-of-range silence threshold");
    CHECK(test_run(workspace, spectrum_values,
                   sizeof(spectrum_values) / sizeof(spectrum_values[0])) == 2,
          "analyze-events accepted --max-spectrum-values");
    CHECK(test_run(workspace, max_lag,
                   sizeof(max_lag) / sizeof(max_lag[0])) == 2,
          "analyze-events accepted --max-lag");
    CHECK(test_run(workspace, true_peak,
                   sizeof(true_peak) / sizeof(true_peak[0])) == 2,
          "analyze-events accepted --true-peak-oversample");
    CHECK(!test_path_exists(workspace->rejected_bundle),
          "command misuse created an event bundle");
}

static void test_existing_output_is_unchanged(const TestWorkspace *workspace)
{
    static const char sentinel[] = "keep this directory\n";
    const char *arguments[] = {
        "analyze-events", workspace->input,
        "--output", workspace->existing_bundle
    };
    char manifest[PATH_MAX];
    int setup_failures = failures;
    CHECK(TEST_MKDIR(workspace->existing_bundle) == 0 &&
              test_write_file(workspace->sentinel, sentinel,
                              sizeof(sentinel) - 1U),
          "cannot create existing-output fixture");
    if (failures != setup_failures) return;
    CHECK(test_run(workspace, arguments,
                   sizeof(arguments) / sizeof(arguments[0])) == 1,
          "analyze-events did not reject an existing output directory");
    CHECK(test_file_contains(workspace->errors, "already exists"),
          "existing output failure gave no clear error");
    CHECK(test_file_equals(workspace->sentinel, sentinel,
                           sizeof(sentinel) - 1U),
          "analyze-events changed the existing output sentinel");
    CHECK(test_join(manifest, workspace->existing_bundle, "manifest.json") &&
              !test_path_exists(manifest),
          "analyze-events added files to the existing output directory");
}

int main(int argc, char **argv)
{
    TestWorkspace workspace;
    if (argc != 2) {
        (void)fputs("usage: event_analysis_cli_tests ANALYZER\n", stderr);
        return 2;
    }
    analyzer_path = argv[1];
    if (!test_make_workspace(&workspace)) {
        (void)fputs("cannot create event-analysis CLI fixture\n", stderr);
        test_workspace_close(&workspace);
        return 1;
    }
    test_analyze_events_writes_readable_bundle(&workspace);
    test_accepted_options_reach_saved_settings(&workspace);
    test_command_misuse_is_rejected(&workspace);
    test_existing_output_is_unchanged(&workspace);
    test_workspace_close(&workspace);
    if (failures != 0) {
        (void)fprintf(stderr, "%d event-analysis CLI test(s) failed\n",
                      failures);
        return 1;
    }
    return 0;
}

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

#define TEST_SAMPLE_RATE UINT32_C(22050)
#define TEST_FRAME_COUNT (UINT32_C(2) * TEST_SAMPLE_RATE)
#define TEST_TONE_START (TEST_SAMPLE_RATE * UINT32_C(2) / UINT32_C(5))
#define TEST_TONE_END (TEST_SAMPLE_RATE * UINT32_C(6) / UINT32_C(5))
#define TEST_WAVE_BYTES (UINT64_C(44) + (uint64_t)TEST_FRAME_COUNT * 2U)

#ifndef HWA_BASIC_PITCH_TEST_MODEL_SHA256
#error "HWA_BASIC_PITCH_TEST_MODEL_SHA256 must name the configured model hash"
#endif

static const char test_model_sha256[] =
    HWA_BASIC_PITCH_TEST_MODEL_SHA256;

static const char test_source_sha256[] =
    "aef0bef5f180415415847835c2d738c3"
    "f512736691122db852498a9e1fcd3969";

typedef struct TestWorkspace {
    char directory[PATH_MAX];
    char input[PATH_MAX];
    char first[PATH_MAX];
    char second[PATH_MAX];
    char output[PATH_MAX];
    char errors[PATH_MAX];
} TestWorkspace;

static const char *analyzer_path;
static const char *model_path;
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

static int16_t test_tone_sample(uint32_t frame)
{
    static const int16_t cycle[32] = {
        0, 1598, 3135, 4551, 5793, 6811, 7568, 8035,
        8192, 8035, 7568, 6811, 5793, 4551, 3135, 1598,
        0, -1598, -3135, -4551, -5793, -6811, -7568, -8035,
        -8192, -8035, -7568, -6811, -5793, -4551, -3135, -1598
    };
    if (frame < TEST_TONE_START || frame >= TEST_TONE_END) return 0;
    return cycle[(frame - TEST_TONE_START) % UINT32_C(32)];
}

static int test_write_wave(const char *path)
{
    const uint32_t data_bytes = TEST_FRAME_COUNT * UINT32_C(2);
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
        !test_write_u32(stream, TEST_SAMPLE_RATE) ||
        !test_write_u32(stream, TEST_SAMPLE_RATE * UINT32_C(2)) ||
        !test_write_u16(stream, UINT16_C(2)) ||
        !test_write_u16(stream, UINT16_C(16)) ||
        !test_write_bytes(stream, "data", 4U) ||
        !test_write_u32(stream, data_bytes)) {
        goto cleanup;
    }
    for (frame = 0U; frame < TEST_FRAME_COUNT; ++frame) {
        if (!test_write_u16(stream, (uint16_t)test_tone_sample(frame)))
            goto cleanup;
    }
    result = 1;
cleanup:
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
                              "%s/hwa-basic-pitch-onnx-%ld-%u",
                              root, TEST_PID(), attempt);
        if (length < 0 || (size_t)length >= PATH_MAX) return 0;
        if (TEST_MKDIR(workspace->directory) == 0) break;
        if (errno != EEXIST) return 0;
    }
    return attempt < 100U &&
           test_join(workspace->input, workspace->directory, "tone.wav") &&
           test_join(workspace->first, workspace->directory,
                     "first.hwa-events") &&
           test_join(workspace->second, workspace->directory,
                     "second.hwa-events") &&
           test_join(workspace->output, workspace->directory, "stdout.txt") &&
           test_join(workspace->errors, workspace->directory, "stderr.txt") &&
           test_write_wave(workspace->input);
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

static int test_files_equal(const char *first, const char *second)
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

static const HWAEventValue *test_selected_pitch(
    const HWAPerformanceEvent *event)
{
    const HWAEventValue *found = NULL;
    size_t index;
    if (event == NULL) return NULL;
    for (index = 0U; index < event->value_count; ++index) {
        const HWAEventValue *value = &event->values[index];
        if (value->name != NULL && strcmp(value->name, "pitch-hz") == 0 &&
            value->selected) {
            if (found != NULL) return NULL;
            found = value;
        }
    }
    return found;
}

static void test_check_bundle(const HWAEventBundle *bundle,
                              const TestWorkspace *workspace)
{
    const HWAPerformanceEvent *event;
    const HWAEventValue *pitch;
    CHECK(bundle->provider_count == 1U && bundle->providers != NULL,
          "real-model bundle has the wrong provider count");
    CHECK(bundle->audio_count == 1U && bundle->audio != NULL,
          "real-model bundle has the wrong audio count");
    CHECK(bundle->event_count == 1U && bundle->events != NULL,
          "real-model bundle has %zu events, wanted 1", bundle->event_count);
    CHECK(bundle->trace_count == 0U && bundle->warning_count == 0U,
          "real-model bundle has unexpected traces or warnings");
    if (bundle->provider_count == 1U && bundle->providers != NULL) {
        const HWAEventProvider *provider = &bundle->providers[0];
        CHECK(provider->id == UINT64_C(1) && provider->name != NULL &&
                  strcmp(provider->name, "org.hlolli.basic-pitch-onnx") == 0 &&
                  provider->version != NULL &&
                  strcmp(provider->version, "1") == 0 &&
                  strcmp(provider->model_sha256, test_model_sha256) == 0 &&
                  provider->settings_json != NULL &&
                  strstr(provider->settings_json,
                         "\"runtime\":{\"name\":\"onnxruntime\"") != NULL &&
                  strstr(provider->settings_json,
                         "\"backend\":\"CPUExecutionProvider\"") != NULL,
              "real-model provider identity or runtime is wrong");
    }
    if (bundle->audio_count == 1U && bundle->audio != NULL) {
        const HWAEventAudio *audio = &bundle->audio[0];
        CHECK(audio->id == UINT64_C(1) &&
                  audio->kind == HWA_EVENT_SOURCE_RECORDING &&
                  audio->name != NULL &&
                  strcmp(audio->name, workspace->input) == 0 &&
                  audio->relative_path != NULL &&
                  audio->relative_path[0] == '\0' &&
                  audio->path_hint != NULL &&
                  audio->path_hint[0] == '\0' &&
                  strcmp(audio->sha256, test_source_sha256) == 0 &&
                  audio->file_bytes == TEST_WAVE_BYTES &&
                  audio->format.container == HWA_CONTAINER_RIFF &&
                  audio->format.encoding == HWA_ENCODING_PCM &&
                  audio->format.channels == 1U &&
                  audio->format.sample_rate_hz == TEST_SAMPLE_RATE &&
                  audio->format.bits_per_sample == 16U &&
                  audio->format.valid_bits_per_sample == 16U &&
                  audio->format.block_align == 2U &&
                  audio->format.frames == TEST_FRAME_COUNT &&
                  audio->format.data_bytes == TEST_FRAME_COUNT * UINT64_C(2),
              "real-model source identity or WAVE clock is wrong");
    }
    if (bundle->event_count != 1U || bundle->events == NULL) return;
    event = &bundle->events[0];
    pitch = test_selected_pitch(event);
    CHECK(event->id == UINT64_C(1) && event->kind != NULL &&
              strcmp(event->kind, "note") == 0 &&
              event->source_recording_id == UINT64_C(1) &&
              event->evidence_audio_id_valid &&
              event->evidence_audio_id == UINT64_C(1) &&
              !event->parent_id_valid &&
              event->start_sample >= UINT64_C(7000) &&
              event->start_sample <= UINT64_C(10500) &&
              event->end_sample >= UINT64_C(23500) &&
              event->end_sample <= UINT64_C(29000) &&
              event->start_sample < event->end_sample &&
              event->end_sample <= TEST_FRAME_COUNT &&
              event->value_count == 1U &&
              event->trace_ref_count == 0U,
          "real-model note has the wrong source links or broad bounds");
    CHECK(pitch != NULL && pitch->kind == HWA_EVENT_VALUE_F64 &&
              pitch->basis == HWA_EVENT_INFERENCE &&
              pitch->number >= 680.0 && pitch->number <= 715.0 &&
              pitch->unit != NULL && strcmp(pitch->unit, "Hz") == 0 &&
              pitch->score_valid && pitch->score >= 0.45 &&
              pitch->score <= 0.85 && pitch->provider_id_valid &&
              pitch->provider_id == UINT64_C(1),
          "real-model note has the wrong selected pitch or score");
}

static int test_infer(const TestWorkspace *workspace, const char *output)
{
    const char *arguments[] = {
        "infer-note-events", workspace->input,
        "--model", model_path,
        "--expect-model-sha256", test_model_sha256,
        "--output", output
    };
    int status = test_run(
        workspace, arguments, sizeof(arguments) / sizeof(arguments[0]));
    CHECK(status == 0, "real-model inference exited with status %d", status);
    CHECK(test_file_is_empty(workspace->output),
          "successful real-model inference wrote standard output");
    CHECK(test_file_is_empty(workspace->errors),
          "successful real-model inference wrote standard error");
    return status == 0;
}

static void test_validate(const TestWorkspace *workspace,
                          const char *bundle)
{
    const char *arguments[] = {"validate-event-bundle", bundle};
    int status = test_run(
        workspace, arguments, sizeof(arguments) / sizeof(arguments[0]));
    CHECK(status == 0, "bundle validation exited with status %d", status);
    CHECK(test_file_contains(workspace->output,
                             "Event bundle validation passed\n"),
          "bundle validation wrote no success report");
    CHECK(test_file_is_empty(workspace->errors),
          "successful bundle validation wrote standard error");
}

static void test_compare_bundle_files(const TestWorkspace *workspace)
{
    static const char *const names[] = {
        "manifest.json", "events.jsonl", "traces.jsonl"
    };
    char first[PATH_MAX];
    char second[PATH_MAX];
    size_t index;
    for (index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
        CHECK(test_join(first, workspace->first, names[index]) &&
                  test_join(second, workspace->second, names[index]) &&
                  test_files_equal(first, second),
              "repeated inference changed %s bytes", names[index]);
    }
}

static void test_remove_bundle(const char *directory,
                               const HWAEventBundle *bundle)
{
    static const char *const root_names[] = {
        "manifest.json", "events.jsonl", "traces.jsonl"
    };
    char path[PATH_MAX];
    size_t index;
    if (directory == NULL || directory[0] == '\0') return;
    if (bundle != NULL) {
        for (index = 0U; index < bundle->trace_count; ++index) {
            if (bundle->traces[index].relative_path != NULL &&
                test_join(path, directory,
                          bundle->traces[index].relative_path))
                (void)TEST_UNLINK(path);
        }
        for (index = 0U; index < bundle->audio_count; ++index) {
            if (bundle->audio[index].relative_path != NULL &&
                bundle->audio[index].relative_path[0] != '\0' &&
                test_join(path, directory,
                          bundle->audio[index].relative_path))
                (void)TEST_UNLINK(path);
        }
    }
    for (index = 0U;
         index < sizeof(root_names) / sizeof(root_names[0]); ++index) {
        if (test_join(path, directory, root_names[index]))
            (void)TEST_UNLINK(path);
    }
    if (test_join(path, directory, "traces")) (void)TEST_RMDIR(path);
    if (test_join(path, directory, "audio")) (void)TEST_RMDIR(path);
    (void)TEST_RMDIR(directory);
}

static void test_workspace_close(TestWorkspace *workspace,
                                 const HWAEventBundle *first,
                                 const HWAEventBundle *second)
{
    if (workspace->first[0] != '\0')
        test_remove_bundle(workspace->first, first);
    if (workspace->second[0] != '\0')
        test_remove_bundle(workspace->second, second);
    if (workspace->input[0] != '\0') (void)TEST_UNLINK(workspace->input);
    if (workspace->output[0] != '\0') (void)TEST_UNLINK(workspace->output);
    if (workspace->errors[0] != '\0') (void)TEST_UNLINK(workspace->errors);
    if (workspace->directory[0] != '\0')
        (void)TEST_RMDIR(workspace->directory);
}

static void test_real_model(void)
{
    TestWorkspace workspace;
    HWAEventBundleLimits limits;
    HWAEventBundle first;
    HWAEventBundle second;
    char error[HWA_ERROR_SIZE] = {0};
    int first_ready = 0;
    int second_ready = 0;
    memset(&workspace, 0, sizeof(workspace));
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    CHECK(test_make_workspace(&workspace),
          "cannot make Basic Pitch ONNX workspace");
    if (failures != 0) {
        test_workspace_close(&workspace, NULL, NULL);
        return;
    }
    if (!test_infer(&workspace, workspace.first) ||
        !test_infer(&workspace, workspace.second))
        goto cleanup;
    test_validate(&workspace, workspace.first);
    test_validate(&workspace, workspace.second);
    test_compare_bundle_files(&workspace);
    hwa_event_bundle_limits_default(&limits);
    if (hwa_event_bundle_read(workspace.first, &limits, &first,
                              error, sizeof(error)) == 0) {
        first_ready = 1;
        test_check_bundle(&first, &workspace);
    } else {
        CHECK(0, "cannot read first real-model bundle: %s", error);
    }
    error[0] = '\0';
    if (hwa_event_bundle_read(workspace.second, &limits, &second,
                              error, sizeof(error)) == 0) {
        second_ready = 1;
        test_check_bundle(&second, &workspace);
    } else {
        CHECK(0, "cannot read second real-model bundle: %s", error);
    }
cleanup:
    test_workspace_close(&workspace,
                         first_ready ? &first : NULL,
                         second_ready ? &second : NULL);
    hwa_event_bundle_free(&first);
    hwa_event_bundle_free(&second);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        (void)fputs(
            "usage: basic_pitch_onnx_cli_tests ANALYZER MODEL\n", stderr);
        return 2;
    }
    analyzer_path = argv[1];
    model_path = argv[2];
    test_real_model();
    if (failures != 0) {
        (void)fprintf(stderr, "%d Basic Pitch ONNX CLI test(s) failed\n",
                      failures);
        return 1;
    }
    (void)puts("Basic Pitch ONNX CLI tests passed");
    return 0;
}

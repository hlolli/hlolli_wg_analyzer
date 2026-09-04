#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "hlolli_wg_analyzer.h"
#include "internal.h"

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

#define TEST_SAMPLE_RATE UINT32_C(44100)
#define TEST_FRAME_COUNT UINT32_C(4096)
#define TEST_SOURCE_BLOCK_ALIGN UINT32_C(4)
#define TEST_STEM_BLOCK_ALIGN UINT32_C(8)
#define TEST_SOURCE_BYTES                                                \
    (UINT64_C(44) +                                                    \
     (uint64_t)TEST_FRAME_COUNT * TEST_SOURCE_BLOCK_ALIGN)
#define TEST_STEM_BYTES                                                  \
    (UINT64_C(44) +                                                    \
     (uint64_t)TEST_FRAME_COUNT * TEST_STEM_BLOCK_ALIGN)
#define TEST_READ_FRAMES 1024U

#ifndef HWA_HTDEMUCS_TEST_MODEL_SHA256
#error "HWA_HTDEMUCS_TEST_MODEL_SHA256 must name the configured model hash"
#endif

static const char test_model_sha256[] =
    HWA_HTDEMUCS_TEST_MODEL_SHA256;

static const char *const test_stem_names[] = {
    "bass", "drums", "guitar", "other", "piano", "vocals"
};

typedef struct TestWorkspace {
    char directory[PATH_MAX];
    char input[PATH_MAX];
    char bundle[PATH_MAX];
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

static int16_t test_signal_sample(uint32_t frame, uint16_t channel)
{
    static const int16_t left_cycle[16] = {
        0, 1567, 2896, 3784, 4096, 3784, 2896, 1567,
        0, -1567, -2896, -3784, -4096, -3784, -2896, -1567
    };
    static const int16_t right_cycle[16] = {
        2048, 1892, 1448, 783, 0, -783, -1448, -1892,
        -2048, -1892, -1448, -783, 0, 783, 1448, 1892
    };
    return channel == 0U ? left_cycle[frame % UINT32_C(16)]
                         : right_cycle[frame % UINT32_C(16)];
}

static int test_write_wave(const char *path)
{
    const uint32_t data_bytes =
        TEST_FRAME_COUNT * TEST_SOURCE_BLOCK_ALIGN;
    FILE *stream = fopen(path, "wb");
    uint32_t frame;
    int result = 0;
    if (stream == NULL) return 0;
    if (!test_write_bytes(stream, "RIFF", 4U) ||
        !test_write_u32(stream, UINT32_C(36) + data_bytes) ||
        !test_write_bytes(stream, "WAVEfmt ", 8U) ||
        !test_write_u32(stream, UINT32_C(16)) ||
        !test_write_u16(stream, UINT16_C(1)) ||
        !test_write_u16(stream, UINT16_C(2)) ||
        !test_write_u32(stream, TEST_SAMPLE_RATE) ||
        !test_write_u32(
            stream, TEST_SAMPLE_RATE * TEST_SOURCE_BLOCK_ALIGN) ||
        !test_write_u16(stream, (uint16_t)TEST_SOURCE_BLOCK_ALIGN) ||
        !test_write_u16(stream, UINT16_C(16)) ||
        !test_write_bytes(stream, "data", 4U) ||
        !test_write_u32(stream, data_bytes)) {
        goto cleanup;
    }
    for (frame = 0U; frame < TEST_FRAME_COUNT; ++frame) {
        if (!test_write_u16(
                stream, (uint16_t)test_signal_sample(frame, 0U)) ||
            !test_write_u16(
                stream, (uint16_t)test_signal_sample(frame, 1U)))
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
                              "%s/hwa-htdemucs-onnx-%ld-%u",
                              root, TEST_PID(), attempt);
        if (length < 0 || (size_t)length >= PATH_MAX) return 0;
        if (TEST_MKDIR(workspace->directory) == 0) break;
        if (errno != EEXIST) return 0;
    }
    return attempt < 100U &&
           test_join(workspace->input, workspace->directory, "input.wav") &&
           test_join(workspace->bundle, workspace->directory,
                     "result.hwa-events") &&
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
            dup2(errors, STDERR_FILENO) < 0)
            _exit(126);
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

static int test_infer(const TestWorkspace *workspace)
{
    const char *arguments[] = {
        "separate-instruments", workspace->input,
        "--model", model_path,
        "--expect-model-sha256", test_model_sha256,
        "--output", workspace->bundle
    };
    int status = test_run(
        workspace, arguments, sizeof(arguments) / sizeof(arguments[0]));
    CHECK(status == 0, "real-model separation exited with status %d", status);
    CHECK(test_file_is_empty(workspace->output),
          "successful real-model separation wrote standard output");
    CHECK(test_file_is_empty(workspace->errors),
          "successful real-model separation wrote standard error");
    return status == 0;
}

static void test_validate(const TestWorkspace *workspace)
{
    const char *arguments[] = {
        "validate-event-bundle", workspace->bundle
    };
    int status = test_run(
        workspace, arguments, sizeof(arguments) / sizeof(arguments[0]));
    CHECK(status == 0, "bundle validation exited with status %d", status);
    CHECK(test_file_contains(workspace->output,
                             "Event bundle validation passed\n"),
          "bundle validation wrote no success report");
    CHECK(test_file_is_empty(workspace->errors),
          "successful bundle validation wrote standard error");
}

static int test_stem_payload_is_finite(const char *path,
                                       char *error,
                                       size_t error_size)
{
    HWAWavReader reader;
    unsigned char raw[TEST_READ_FRAMES * TEST_STEM_BLOCK_ALIGN];
    uint64_t frames = 0U;
    int reader_open = 0;
    int result = 0;
    memset(&reader, 0, sizeof(reader));
    if (hwa_wav_reader_open(
            &reader, path, TEST_STEM_BYTES, error, error_size) != 0)
        return 0;
    reader_open = 1;
    if (reader.format.container != HWA_CONTAINER_RIFF ||
        reader.format.encoding != HWA_ENCODING_IEEE_FLOAT ||
        reader.format.channels != 2U ||
        reader.format.sample_rate_hz != TEST_SAMPLE_RATE ||
        reader.format.bits_per_sample != 32U ||
        reader.format.valid_bits_per_sample != 32U ||
        reader.format.block_align != TEST_STEM_BLOCK_ALIGN ||
        reader.format.frames != TEST_FRAME_COUNT ||
        reader.format.data_bytes !=
            (uint64_t)TEST_FRAME_COUNT * TEST_STEM_BLOCK_ALIGN) {
        (void)snprintf(error, error_size,
                       "stem payload has the wrong WAVE format");
        goto cleanup;
    }
    while (frames < TEST_FRAME_COUNT) {
        size_t wanted = (size_t)(TEST_FRAME_COUNT - frames);
        size_t got = 0U;
        size_t frame;
        if (wanted > TEST_READ_FRAMES) wanted = TEST_READ_FRAMES;
        if (hwa_wav_reader_read_frames(
                &reader, raw, wanted, &got, error, error_size) != 0 ||
            got == 0U)
            goto cleanup;
        for (frame = 0U; frame < got; ++frame) {
            size_t channel;
            for (channel = 0U; channel < 2U; ++channel) {
                int clipped = 0;
                double value = hwa_wav_decode_sample(
                    &reader,
                    raw + frame * TEST_STEM_BLOCK_ALIGN +
                        channel * sizeof(float),
                    &clipped);
                (void)clipped;
                if (!isfinite(value)) {
                    (void)snprintf(error, error_size,
                                   "stem payload has a non-finite sample");
                    goto cleanup;
                }
            }
        }
        frames += (uint64_t)got;
    }
    if (reader.bytes_remaining != 0U) {
        (void)snprintf(error, error_size,
                       "stem payload has trailing sample bytes");
        goto cleanup;
    }
    result = 1;
cleanup:
    if (reader_open) hwa_wav_reader_close(&reader);
    return result;
}

static void test_check_provider(const HWAEventBundle *bundle)
{
    CHECK(bundle->provider_count == 1U && bundle->providers != NULL,
          "real-model bundle has the wrong provider count");
    if (bundle->provider_count == 1U && bundle->providers != NULL) {
        const HWAEventProvider *provider = &bundle->providers[0];
        CHECK(provider->id == UINT64_C(1) && provider->name != NULL &&
                  strcmp(provider->name,
                         "org.hlolli.instrument-stem-provider") == 0 &&
                  provider->version != NULL &&
                  strcmp(provider->version, "1") == 0 &&
                  strcmp(provider->model_sha256,
                         test_model_sha256) == 0 &&
                  provider->settings_json != NULL &&
                  strstr(provider->settings_json,
                         "\"runtime\":{\"name\":\"onnxruntime\"") != NULL &&
                  strstr(provider->settings_json,
                         "\"backend\":\"CPUExecutionProvider\"") != NULL &&
                  strstr(provider->settings_json,
                         "\"task_settings\":{}") != NULL,
              "real-model provider identity or runtime is wrong");
    }
}

static void test_check_source_audio(const HWAEventAudio *audio,
                                    const TestWorkspace *workspace)
{
    CHECK(audio->id == UINT64_C(1) &&
              audio->kind == HWA_EVENT_SOURCE_RECORDING &&
              audio->name != NULL &&
              strcmp(audio->name, workspace->input) == 0 &&
              audio->relative_path != NULL &&
              audio->relative_path[0] == '\0' &&
              audio->path_hint != NULL && audio->path_hint[0] == '\0' &&
              audio->file_bytes == TEST_SOURCE_BYTES &&
              audio->format.container == HWA_CONTAINER_RIFF &&
              audio->format.encoding == HWA_ENCODING_PCM &&
              audio->format.channels == 2U &&
              audio->format.sample_rate_hz == TEST_SAMPLE_RATE &&
              audio->format.bits_per_sample == 16U &&
              audio->format.valid_bits_per_sample == 16U &&
              audio->format.block_align == TEST_SOURCE_BLOCK_ALIGN &&
              audio->format.frames == TEST_FRAME_COUNT &&
              audio->format.data_bytes ==
                  (uint64_t)TEST_FRAME_COUNT * TEST_SOURCE_BLOCK_ALIGN &&
              !audio->source_recording_id_valid,
          "real-model source identity or WAVE clock is wrong");
}

static void test_check_stem(const HWAEventBundle *bundle,
                            const TestWorkspace *workspace,
                            size_t index)
{
    const char *name = test_stem_names[index];
    const HWAEventAudio *audio = &bundle->audio[index + 1U];
    const HWAPerformanceEvent *event = &bundle->events[index];
    const HWAEventValue *value =
        event->value_count == 1U && event->values != NULL
            ? &event->values[0] : NULL;
    char expected_relative[PATH_MAX];
    char payload[PATH_MAX];
    char error[HWA_ERROR_SIZE] = {0};
    int relative_ready = snprintf(
        expected_relative, sizeof(expected_relative),
        "audio/%s.wav", name);
    CHECK(relative_ready > 0 &&
              (size_t)relative_ready < sizeof(expected_relative),
          "expected stem path overflowed for %s", name);
    if (relative_ready <= 0 ||
        (size_t)relative_ready >= sizeof(expected_relative))
        return;
    CHECK(audio->id == (uint64_t)index + UINT64_C(2) &&
              audio->kind == HWA_EVENT_INSTRUMENT_STEM &&
              audio->name != NULL && strcmp(audio->name, name) == 0 &&
              audio->relative_path != NULL &&
              strcmp(audio->relative_path, expected_relative) == 0 &&
              audio->path_hint != NULL && audio->path_hint[0] == '\0' &&
              audio->file_bytes == TEST_STEM_BYTES &&
              audio->source_recording_id_valid &&
              audio->source_recording_id == UINT64_C(1) &&
              audio->format.container == HWA_CONTAINER_RIFF &&
              audio->format.encoding == HWA_ENCODING_IEEE_FLOAT &&
              audio->format.channels == 2U &&
              audio->format.sample_rate_hz == TEST_SAMPLE_RATE &&
              audio->format.bits_per_sample == 32U &&
              audio->format.valid_bits_per_sample == 32U &&
              audio->format.block_align == TEST_STEM_BLOCK_ALIGN &&
              audio->format.frames == TEST_FRAME_COUNT &&
              audio->format.data_bytes ==
                  (uint64_t)TEST_FRAME_COUNT * TEST_STEM_BLOCK_ALIGN,
          "real-model %s audio row is wrong", name);
    CHECK(event->id == (uint64_t)index + UINT64_C(1) &&
              event->kind != NULL &&
              strcmp(event->kind, "instrument-region") == 0 &&
              event->source_recording_id == UINT64_C(1) &&
              event->evidence_audio_id_valid &&
              event->evidence_audio_id == audio->id &&
              !event->parent_id_valid && event->start_sample == 0U &&
              event->end_sample == TEST_FRAME_COUNT &&
              event->part != NULL && strcmp(event->part, name) == 0 &&
              event->value_count == 1U && event->trace_ref_count == 0U,
          "real-model %s event is wrong", name);
    CHECK(value != NULL && value->name != NULL &&
              strcmp(value->name, "instrument") == 0 &&
              value->kind == HWA_EVENT_VALUE_TEXT &&
              value->basis == HWA_EVENT_INFERENCE &&
              value->text != NULL && strcmp(value->text, name) == 0 &&
              value->unit != NULL && value->unit[0] == '\0' &&
              !value->score_valid && value->score == 0.0 &&
              value->provider_id_valid &&
              value->provider_id == UINT64_C(1) && value->selected,
          "real-model %s instrument value is wrong", name);
    CHECK(test_join(payload, workspace->bundle, expected_relative) &&
              test_stem_payload_is_finite(
                  payload, error, sizeof(error)),
          "real-model %s payload is missing or invalid: %s", name,
          error[0] != '\0' ? error : "cannot form payload path");
}

static void test_check_bundle(const HWAEventBundle *bundle,
                              const TestWorkspace *workspace)
{
    size_t index;
    test_check_provider(bundle);
    CHECK(bundle->audio_count == 7U && bundle->audio != NULL,
          "real-model bundle has %zu audio rows, wanted 7",
          bundle->audio_count);
    CHECK(bundle->event_count == 6U && bundle->events != NULL,
          "real-model bundle has %zu events, wanted 6",
          bundle->event_count);
    CHECK(bundle->trace_count == 0U && bundle->warning_count == 0U,
          "real-model bundle has unexpected traces or warnings");
    if (bundle->audio_count != 7U || bundle->audio == NULL ||
        bundle->event_count != 6U || bundle->events == NULL)
        return;
    test_check_source_audio(&bundle->audio[0], workspace);
    for (index = 0U;
         index < sizeof(test_stem_names) / sizeof(test_stem_names[0]);
         ++index)
        test_check_stem(bundle, workspace, index);
}

static void test_remove_bundle(const char *directory)
{
    static const char *const root_names[] = {
        "manifest.json", "events.jsonl", "traces.jsonl"
    };
    char relative[PATH_MAX];
    char path[PATH_MAX];
    size_t index;
    if (directory == NULL || directory[0] == '\0') return;
    for (index = 0U;
         index < sizeof(test_stem_names) / sizeof(test_stem_names[0]);
         ++index) {
        int length = snprintf(relative, sizeof(relative),
                              "audio/%s.wav", test_stem_names[index]);
        if (length > 0 && (size_t)length < sizeof(relative) &&
            test_join(path, directory, relative))
            (void)TEST_UNLINK(path);
    }
    if (test_join(path, directory, "audio")) (void)TEST_RMDIR(path);
    for (index = 0U;
         index < sizeof(root_names) / sizeof(root_names[0]); ++index) {
        if (test_join(path, directory, root_names[index]))
            (void)TEST_UNLINK(path);
    }
    if (test_join(path, directory, "traces")) (void)TEST_RMDIR(path);
    (void)TEST_RMDIR(directory);
}

static void test_workspace_close(TestWorkspace *workspace)
{
    if (workspace->bundle[0] != '\0')
        test_remove_bundle(workspace->bundle);
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
    HWAEventBundle bundle;
    char error[HWA_ERROR_SIZE] = {0};
    memset(&workspace, 0, sizeof(workspace));
    memset(&bundle, 0, sizeof(bundle));
    CHECK(test_make_workspace(&workspace),
          "cannot make HTDemucs ONNX workspace");
    if (failures != 0) {
        test_workspace_close(&workspace);
        return;
    }
    if (!test_infer(&workspace)) goto cleanup;
    test_validate(&workspace);
    hwa_event_bundle_limits_default(&limits);
    if (hwa_event_bundle_read(workspace.bundle, &limits, &bundle,
                              error, sizeof(error)) == 0) {
        test_check_bundle(&bundle, &workspace);
    } else {
        CHECK(0, "cannot read real-model bundle: %s", error);
    }
cleanup:
    hwa_event_bundle_free(&bundle);
    test_workspace_close(&workspace);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        (void)fputs(
            "usage: htdemucs_onnx_cli_tests ANALYZER MODEL\n", stderr);
        return 2;
    }
    analyzer_path = argv[1];
    model_path = argv[2];
    test_real_model();
    if (failures != 0) {
        (void)fprintf(stderr, "%d HTDemucs ONNX CLI test(s) failed\n",
                      failures);
        return 1;
    }
    (void)puts("HTDemucs ONNX CLI tests passed");
    return 0;
}

#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "hlolli_wg_analyzer.h"
#include "run_file.h"
#include "sha256.h"

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
#include <windows.h>
#include "windows_test_process.h"
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TEST_RATE 8000U
#define TEST_FRAMES 16000U
#define TEST_PI 3.14159265358979323846264338327950288

typedef struct TestFiles {
    char directory[PATH_MAX];
    char manifest[PATH_MAX];
    char reference[PATH_MAX];
    char model[PATH_MAX];
    char output[PATH_MAX];
    char second[PATH_MAX];
    char alias[PATH_MAX];
    char normalized_a[PATH_MAX];
    char normalized_b[PATH_MAX];
    char stdout_path[PATH_MAX];
    char stderr_path[PATH_MAX];
} TestFiles;

static const char *analyzer_path;
static int failures;

#define CHECK(condition, ...)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "FAIL: ");                                \
            (void)fprintf(stderr, __VA_ARGS__);                              \
            (void)fputc('\n', stderr);                                      \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static long test_pid(void)
{
#if defined(_WIN32)
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}

static int test_mkdir(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

static int test_unlink(const char *path)
{
#if defined(_WIN32)
    return _unlink(path);
#else
    return unlink(path);
#endif
}

static int test_rmdir(const char *path)
{
#if defined(_WIN32)
    return _rmdir(path);
#else
    return rmdir(path);
#endif
}

static int test_link(const char *source, const char *alias)
{
#if defined(_WIN32)
    return CreateHardLinkA(alias, source, NULL) != 0;
#else
    return link(source, alias) == 0;
#endif
}

#if !defined(_WIN32)
static int test_symlink(const char *source, const char *alias)
{
    return symlink(source, alias) == 0;
}
#endif

static int test_join(char path[PATH_MAX],
                     const char *directory,
                     const char *name)
{
    int written = snprintf(path, PATH_MAX, "%s/%s", directory, name);
    return written > 0 && written < PATH_MAX;
}

static int test_files_open(TestFiles *files)
{
    unsigned attempt;
#if defined(_WIN32)
    const char *root = getenv("TEMP");
    if (root == NULL || root[0] == '\0') root = ".";
#else
    const char *root = "/tmp";
#endif
    memset(files, 0, sizeof(*files));
    for (attempt = 0U; attempt < 100U; ++attempt) {
        int written = snprintf(files->directory, sizeof(files->directory),
                               "%s/hwa-stage7-cli-%ld-%u",
                               root, test_pid(), attempt);
        if (written < 0 ||
            (size_t)written >= sizeof(files->directory)) return 0;
        if (test_mkdir(files->directory) == 0) break;
        if (errno != EEXIST) return 0;
    }
    return attempt < 100U &&
           test_join(files->manifest, files->directory, "run.json") &&
           test_join(files->reference, files->directory, "reference.wav") &&
           test_join(files->model, files->directory, "model.wav") &&
           test_join(files->output, files->directory, "result.hwa-run") &&
           test_join(files->second, files->directory, "second.hwa-run") &&
           test_join(files->alias, files->directory, "alias.hwa-run") &&
           test_join(files->normalized_a, files->directory,
                     "normalized-a.hwa-run") &&
           test_join(files->normalized_b, files->directory,
                     "normalized-b.hwa-run") &&
           test_join(files->stdout_path, files->directory, "stdout.txt") &&
           test_join(files->stderr_path, files->directory, "stderr.txt");
}

static void test_files_close(TestFiles *files)
{
    const char *paths[] = {
        files->manifest, files->reference, files->model, files->output,
        files->second, files->alias, files->normalized_a,
        files->normalized_b, files->stdout_path, files->stderr_path
    };
    size_t index;
    for (index = 0U; index < sizeof(paths) / sizeof(paths[0]); ++index) {
        (void)test_unlink(paths[index]);
    }
    (void)test_rmdir(files->alias);
    (void)test_rmdir(files->directory);
}

static int test_write_bytes(FILE *stream, const void *bytes, size_t size)
{
    return fwrite(bytes, 1U, size, stream) == size;
}

static int test_write_u16(FILE *stream, uint16_t value)
{
    unsigned char bytes[2];
    bytes[0] = (unsigned char)(value & UINT16_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT16_C(0xff));
    return test_write_bytes(stream, bytes, sizeof(bytes));
}

static int test_write_u32(FILE *stream, uint32_t value)
{
    unsigned char bytes[4];
    bytes[0] = (unsigned char)(value & UINT32_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT32_C(0xff));
    bytes[2] = (unsigned char)((value >> 16U) & UINT32_C(0xff));
    bytes[3] = (unsigned char)((value >> 24U) & UINT32_C(0xff));
    return test_write_bytes(stream, bytes, sizeof(bytes));
}

static int test_write_wav(const char *path, double amplitude, double phase)
{
    FILE *stream = fopen(path, "wb");
    uint32_t frame;
    int okay;
    if (stream == NULL) return 0;
    okay = test_write_bytes(stream, "RIFF", 4U) &&
           test_write_u32(stream, 36U + TEST_FRAMES * 2U) &&
           test_write_bytes(stream, "WAVE", 4U) &&
           test_write_bytes(stream, "fmt ", 4U) &&
           test_write_u32(stream, 16U) && test_write_u16(stream, 1U) &&
           test_write_u16(stream, 1U) && test_write_u32(stream, TEST_RATE) &&
           test_write_u32(stream, TEST_RATE * 2U) &&
           test_write_u16(stream, 2U) && test_write_u16(stream, 16U) &&
           test_write_bytes(stream, "data", 4U) &&
           test_write_u32(stream, TEST_FRAMES * 2U);
    for (frame = 0U; okay && frame < TEST_FRAMES; ++frame) {
        double time = (double)frame / (double)TEST_RATE;
        double sample = amplitude *
                        sin(2.0 * TEST_PI * 220.0 * time + phase);
        okay = test_write_u16(
            stream, (uint16_t)(int16_t)lrint(sample * 32767.0));
    }
    if (fclose(stream) != 0) okay = 0;
    return okay;
}

static int test_write_text(const char *path, const char *text)
{
    FILE *stream = fopen(path, "wb");
    size_t size = strlen(text);
    int okay = stream != NULL && fwrite(text, 1U, size, stream) == size;
    if (stream != NULL && fclose(stream) != 0) okay = 0;
    return okay;
}

static int test_write_manifest(const TestFiles *files)
{
    char reference_hash[HWA_SHA256_HEX_SIZE];
    char model_hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE] = {0};
    char manifest[2048];
    int written;

    if (hwa_sha256_file(files->reference, UINT64_C(1048576),
                        reference_hash, error, sizeof(error)) != 0 ||
        hwa_sha256_file(files->model, UINT64_C(1048576),
                        model_hash, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "fixture hash: %s\n", error);
        return 0;
    }
    written = snprintf(
        manifest, sizeof(manifest),
        "{\"schema\":\"hwa-run\",\"schema_version\":1,"
        "\"method_version\":\"stage7-1\",\"clock_rate_hz\":8000,"
        "\"stems\":[{\"id\":\"ref.final\",\"side\":\"reference\","
        "\"role\":\"final\",\"sha256\":\"%s\",\"start_sample\":0,"
        "\"gain_db\":0,\"rate_hz\":8000,\"channels\":1},"
        "{\"id\":\"model.final\",\"side\":\"model\","
        "\"role\":\"final\",\"sha256\":\"%s\",\"start_sample\":0,"
        "\"gain_db\":0,\"rate_hz\":8000,\"channels\":1}],"
        "\"probes\":[],\"links\":[]}\n",
        reference_hash, model_hash);
    return written > 0 && (size_t)written < sizeof(manifest) &&
           test_write_text(files->manifest, manifest);
}

static unsigned char *test_read(const char *path, size_t *size)
{
    FILE *stream = fopen(path, "rb");
    long end;
    unsigned char *bytes;
    int okay;
    *size = 0U;
    if (stream == NULL || fseek(stream, 0L, SEEK_END) != 0 ||
        (end = ftell(stream)) < 0 || fseek(stream, 0L, SEEK_SET) != 0) {
        if (stream != NULL) (void)fclose(stream);
        return NULL;
    }
    bytes = (unsigned char *)malloc((size_t)end + 1U);
    if (bytes == NULL) {
        (void)fclose(stream);
        return NULL;
    }
    okay = fread(bytes, 1U, (size_t)end, stream) == (size_t)end;
    if (fclose(stream) != 0) okay = 0;
    if (!okay) {
        free(bytes);
        return NULL;
    }
    bytes[(size_t)end] = 0U;
    *size = (size_t)end;
    return bytes;
}

static int test_unchanged(const char *path,
                          const unsigned char *before,
                          size_t before_size)
{
    size_t after_size;
    unsigned char *after = test_read(path, &after_size);
    int same = after != NULL && after_size == before_size &&
               memcmp(after, before, after_size) == 0;
    free(after);
    return same;
}

static int test_exists(const char *path)
{
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) return 0;
    (void)fclose(stream);
    return 1;
}

static int test_contains(const char *path, const char *needle)
{
    size_t size;
    unsigned char *bytes = test_read(path, &size);
    int found = bytes != NULL && strstr((const char *)bytes, needle) != NULL;
    (void)size;
    free(bytes);
    return found;
}

static int test_file_equal(const char *left, const char *right)
{
    size_t left_size;
    size_t right_size;
    unsigned char *left_bytes = test_read(left, &left_size);
    unsigned char *right_bytes = test_read(right, &right_size);
    int equal = left_bytes != NULL && right_bytes != NULL &&
                left_size == right_size &&
                memcmp(left_bytes, right_bytes, left_size) == 0;
    free(left_bytes);
    free(right_bytes);
    return equal;
}

#if !defined(_WIN32)
static int test_append(char *command,
                       size_t capacity,
                       size_t *length,
                       const char *text)
{
    size_t added = strlen(text);
    if (*length >= capacity || added >= capacity - *length) return 0;
    memcpy(command + *length, text, added + 1U);
    *length += added;
    return 1;
}

static int test_argument(char *command,
                         size_t capacity,
                         size_t *length,
                         const char *argument)
{
    const unsigned char *cursor = (const unsigned char *)argument;
#if defined(_WIN32)
    if (!test_append(command, capacity, length, "\"")) return 0;
    while (*cursor != 0U) {
        char one[2] = {(char)*cursor, '\0'};
        if (*cursor == (unsigned char)'\"') {
            if (!test_append(command, capacity, length, "\\\"")) return 0;
        } else if (!test_append(command, capacity, length, one)) {
            return 0;
        }
        cursor++;
    }
    return test_append(command, capacity, length, "\"");
#else
    if (!test_append(command, capacity, length, "'")) return 0;
    while (*cursor != 0U) {
        char one[2] = {(char)*cursor, '\0'};
        if (*cursor == (unsigned char)'\'') {
            if (!test_append(command, capacity, length, "'\\''")) return 0;
        } else if (!test_append(command, capacity, length, one)) {
            return 0;
        }
        cursor++;
    }
    return test_append(command, capacity, length, "'");
#endif
}
#endif

static int test_run(const TestFiles *files,
                    const char *const *arguments,
                    size_t argument_count)
{
#if defined(_WIN32)
    return hwa_test_spawn_redirected(
        analyzer_path, arguments, argument_count, NULL,
        files->stdout_path, files->stderr_path);
#else
    char command[PATH_MAX * 16U];
    size_t length = 0U;
    size_t index;
    int status;
    command[0] = '\0';
    if (!test_argument(command, sizeof(command), &length, analyzer_path)) {
        return -1;
    }
    for (index = 0U; index < argument_count; ++index) {
        if (!test_append(command, sizeof(command), &length, " ") ||
            !test_argument(command, sizeof(command), &length,
                           arguments[index])) return -1;
    }
    if (!test_append(command, sizeof(command), &length, " >") ||
        !test_argument(command, sizeof(command), &length,
                       files->stdout_path) ||
        !test_append(command, sizeof(command), &length, " 2>") ||
        !test_argument(command, sizeof(command), &length,
                       files->stderr_path)) return -1;
    status = system(command);
    if (status == -1) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#endif
}

#if !defined(_WIN32)
static int test_broken_stdout(const TestFiles *files,
                              const char *const *arguments,
                              size_t argument_count)
{
    char *exec_arguments[48];
    int output_pipe[2];
    pid_t child;
    pid_t waited;
    size_t index;
    int status;
    if (argument_count > 46U || pipe(output_pipe) != 0) return -1;
    if (close(output_pipe[0]) != 0) {
        (void)close(output_pipe[1]);
        return -1;
    }
    child = fork();
    if (child < 0) {
        (void)close(output_pipe[1]);
        return -1;
    }
    if (child == 0) {
        int error_file;
        if (dup2(output_pipe[1], STDOUT_FILENO) < 0) _exit(126);
        (void)close(output_pipe[1]);
        error_file = open(files->stderr_path,
                          O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (error_file < 0 || dup2(error_file, STDERR_FILENO) < 0) {
            _exit(126);
        }
        (void)close(error_file);
        exec_arguments[0] = (char *)analyzer_path;
        for (index = 0U; index < argument_count; ++index) {
            exec_arguments[index + 1U] = (char *)arguments[index];
        }
        exec_arguments[argument_count + 1U] = NULL;
        (void)execv(analyzer_path, exec_arguments);
        _exit(127);
    }
    (void)close(output_pipe[1]);
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == child && WIFEXITED(status) ?
               WEXITSTATUS(status) : -1;
}
#endif

static int test_normalize_run(const char *input, const char *output)
{
    HWARunOptions limits;
    HWARunResult result;
    char sha256[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE] = {0};
    FILE *stream;
    int status = -1;

    memset(&result, 0, sizeof(result));
    hwa_run_options_default(&limits);
    if (hwa_run_file_read(input, &limits, &result, sha256,
                          error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "run normalize read: %s\n", error);
        return 0;
    }
    result.options.decode_block_frames = limits.decode_block_frames;
    stream = fopen(output, "wb");
    if (stream != NULL) {
        status = hwa_run_file_write(stream, &result, error, sizeof(error));
        if (fclose(stream) != 0) status = -1;
    }
    if (status != 0) {
        (void)fprintf(stderr, "run normalize write: %s\n", error);
    }
    hwa_run_result_free(&result);
    return status == 0;
}

static int test_make_success_inputs(TestFiles *files,
                                    char reference_binding[PATH_MAX + 32U],
                                    char model_binding[PATH_MAX + 32U])
{
    int reference_written;
    int model_written;

    if (!test_write_wav(files->reference, 0.18, 0.0) ||
        !test_write_wav(files->model, 0.12, 0.1) ||
        !test_write_manifest(files)) return 0;
    reference_written = snprintf(reference_binding, PATH_MAX + 32U,
                                 "ref.final=%s", files->reference);
    model_written = snprintf(model_binding, PATH_MAX + 32U,
                             "model.final=%s", files->model);
    return reference_written > 0 && reference_written < PATH_MAX + 32 &&
           model_written > 0 && model_written < PATH_MAX + 32;
}

static void test_parse_and_failure_safety(void)
{
    static const char *zero_flags[] = {
        "--max-run-manifest-bytes", "--max-run-input-bytes",
        "--max-run-input-frames", "--max-run-probe-bytes",
        "--max-run-probe-values", "--max-run-work-bytes",
        "--max-run-evaluations", "--max-run-stems", "--max-run-probes",
        "--max-run-links", "--max-run-json-depth", "--max-run-json-tokens",
        "--max-run-result-rows", "--max-run-warnings"
    };
    TestFiles files;
    char reference_binding[PATH_MAX + 32U];
    char model_binding[PATH_MAX + 32U];
    unsigned char *manifest_before;
    unsigned char *reference_before;
    unsigned char *model_before;
    unsigned char *output_before;
    size_t manifest_size;
    size_t reference_size;
    size_t model_size;
    size_t output_size;
    size_t index;
    const char *bad_analysis[] = {
        "analyze-run", files.manifest,
        "--bind", reference_binding, "--bind", model_binding,
        "--output", files.output
    };
    const char *replace_failure[] = {
        "--replace", "analyze-run", files.manifest,
        "--bind", reference_binding, "--bind", model_binding,
        "--output", files.output
    };
    const char *json_stdout[] = {
        "--json", "analyze-run", files.manifest,
        "--bind", reference_binding, "--bind", model_binding,
        "--output", "-"
    };
    const char *replace_stdout[] = {
        "--replace", "analyze-run", files.manifest,
        "--bind", reference_binding, "--bind", model_binding,
        "--output", "-"
    };
    const char *stdin_manifest[] = {
        "analyze-run", "-", "--bind", reference_binding,
        "--bind", model_binding, "--output", files.output
    };
    const char *bad_binding[] = {
        "analyze-run", files.manifest, "--bind", "ref.final=",
        "--bind", model_binding, "--output", files.output
    };
    const char *small_manifest[] = {
        "--max-run-manifest-bytes", "1", "analyze-run", files.manifest,
        "--bind", reference_binding, "--bind", model_binding,
        "--output", files.output
    };

    CHECK(test_files_open(&files), "cannot make Stage 7 failure workspace");
    if (failures != 0) return;
    CHECK(test_make_success_inputs(&files, reference_binding, model_binding),
          "cannot make Stage 7 failure fixtures");
    manifest_before = test_read(files.manifest, &manifest_size);
    reference_before = test_read(files.reference, &reference_size);
    model_before = test_read(files.model, &model_size);
    CHECK(manifest_before != NULL && reference_before != NULL &&
              model_before != NULL,
          "cannot snapshot Stage 7 failure inputs");

    CHECK(test_write_text(files.manifest, "{bad json\n"),
          "cannot corrupt Stage 7 manifest");
    CHECK(test_run(&files, bad_analysis,
                   sizeof(bad_analysis) / sizeof(bad_analysis[0])) != 0 &&
              !test_exists(files.output),
          "failed Stage 7 analysis published an output");
    CHECK(test_write_text(files.output, "old run output"),
          "cannot make Stage 7 replacement sentinel");
    output_before = test_read(files.output, &output_size);
    CHECK(output_before != NULL &&
              test_run(&files, replace_failure,
                       sizeof(replace_failure) /
                           sizeof(replace_failure[0])) != 0 &&
              test_unchanged(files.output, output_before, output_size),
          "failed Stage 7 replacement changed the old output");
    CHECK(test_write_text(files.manifest,
                          (const char *)manifest_before),
          "cannot restore Stage 7 manifest");

    CHECK(test_run(&files, json_stdout,
                   sizeof(json_stdout) / sizeof(json_stdout[0])) != 0,
          "Stage 7 accepted JSON with canonical standard output");
    CHECK(test_run(&files, replace_stdout,
                   sizeof(replace_stdout) / sizeof(replace_stdout[0])) != 0,
          "Stage 7 accepted replace with canonical standard output");
    CHECK(test_run(&files, stdin_manifest,
                   sizeof(stdin_manifest) / sizeof(stdin_manifest[0])) != 0,
          "Stage 7 accepted a standard-input manifest");
    CHECK(test_run(&files, bad_binding,
                   sizeof(bad_binding) / sizeof(bad_binding[0])) != 0,
          "Stage 7 accepted an empty binding path");
    CHECK(test_run(&files, small_manifest,
                   sizeof(small_manifest) / sizeof(small_manifest[0])) != 0,
          "Stage 7 accepted a one-byte manifest cap");
    for (index = 0U;
         index < sizeof(zero_flags) / sizeof(zero_flags[0]); ++index) {
        const char *zero[] = {
            zero_flags[index], "0", "analyze-run", files.manifest,
            "--bind", reference_binding, "--bind", model_binding,
            "--output", files.second
        };
        CHECK(test_run(&files, zero, sizeof(zero) / sizeof(zero[0])) != 0 &&
                  !test_exists(files.second),
              "Stage 7 accepted zero for %s", zero_flags[index]);
    }
    CHECK(test_unchanged(files.manifest, manifest_before, manifest_size) &&
              test_unchanged(files.reference,
                             reference_before, reference_size) &&
              test_unchanged(files.model, model_before, model_size),
          "Stage 7 changed an input during a failed command");

    free(output_before);
    free(manifest_before);
    free(reference_before);
    free(model_before);
    test_files_close(&files);
}

static void test_success_and_publication(void)
{
    TestFiles files;
    char reference_binding[PATH_MAX + 32U];
    char model_binding[PATH_MAX + 32U];
    unsigned char *manifest_before;
    unsigned char *reference_before;
    unsigned char *model_before;
    unsigned char *sentinel_before = NULL;
    size_t manifest_size;
    size_t reference_size;
    size_t model_size;
    const char *basic[] = {
        "analyze-run", files.manifest,
        "--bind", reference_binding, "--bind", model_binding,
        "--output", files.output
    };
    const char *replace[] = {
        "--replace", "analyze-run", files.manifest,
        "--bind", reference_binding, "--bind", model_binding,
        "--output", files.output
    };
    const char *json[] = {
        "--json", "analyze-run", files.manifest,
        "--bind", reference_binding, "--bind", model_binding,
        "--output", files.second
    };
    const char *canonical_stdout[] = {
        "analyze-run", files.manifest,
        "--bind", reference_binding, "--bind", model_binding,
        "--output", "-"
    };
    const char *all_caps[] = {
        "--max-run-manifest-bytes", "1048576",
        "--max-run-input-bytes", "1048576",
        "--max-run-input-frames", "16000",
        "--max-run-probe-bytes", "1048576",
        "--max-run-probe-values", "1000000",
        "--max-run-work-bytes", "67108864",
        "--max-run-evaluations", "1000000000",
        "--max-run-stems", "2", "--max-run-probes", "1",
        "--max-run-links", "1", "--max-run-json-depth", "32",
        "--max-run-json-tokens", "1000",
        "--max-run-result-rows", "1000", "--max-run-warnings", "100",
        "analyze-run", files.manifest,
        "--bind", reference_binding, "--bind", model_binding,
        "--output", files.second
    };
    const char *under_stem_cap[] = {
        "--max-run-stems", "1", "analyze-run", files.manifest,
        "--bind", reference_binding, "--bind", model_binding,
        "--output", files.second
    };
    const char *block_one[] = {
        "--replace", "--block-frames", "1", "analyze-run", files.manifest,
        "--bind", reference_binding, "--bind", model_binding,
        "--output", files.output
    };
    const char *block_many[] = {
        "--block-frames", "4096", "analyze-run", files.manifest,
        "--bind", reference_binding, "--bind", model_binding,
        "--output", files.second
    };
    const char *alias_output[] = {
        "--replace", "analyze-run", files.manifest,
        "--bind", reference_binding, "--bind", model_binding,
        "--output", files.alias
    };
#if !defined(_WIN32)
    size_t sentinel_size = 0U;
    const char *broken[] = {
        "--replace", "analyze-run", files.manifest,
        "--bind", reference_binding, "--bind", model_binding,
        "--output", files.second
    };
#endif

    CHECK(test_files_open(&files), "cannot make Stage 7 success workspace");
    if (failures != 0) return;
    CHECK(test_make_success_inputs(&files, reference_binding, model_binding),
          "cannot make Stage 7 success fixtures");
    manifest_before = test_read(files.manifest, &manifest_size);
    reference_before = test_read(files.reference, &reference_size);
    model_before = test_read(files.model, &model_size);
    CHECK(manifest_before != NULL && reference_before != NULL &&
              model_before != NULL,
          "cannot snapshot Stage 7 success inputs");

    CHECK(test_run(&files, basic, sizeof(basic) / sizeof(basic[0])) == 0 &&
              test_contains(files.output, "HWA_RUN,1") &&
              test_contains(files.stdout_path, "Run analysis") &&
              test_contains(files.stdout_path, "features"),
          "named Stage 7 text output failed");
    CHECK(test_run(&files, basic, sizeof(basic) / sizeof(basic[0])) != 0,
          "Stage 7 replaced an output without --replace");
    CHECK(test_run(&files, replace,
                   sizeof(replace) / sizeof(replace[0])) == 0,
          "Stage 7 explicit replacement failed");
    CHECK(test_run(&files, json, sizeof(json) / sizeof(json[0])) == 0 &&
              test_contains(files.second, "HWA_RUN,1") &&
              test_contains(files.stdout_path, "\"schema_version\":9") &&
              test_contains(files.stdout_path, "\"sources\":[") &&
              test_contains(files.stdout_path, "\"stages\":["),
          "Stage 7 schema 9 named report failed");
    (void)test_unlink(files.second);
    CHECK(test_run(&files, canonical_stdout,
                   sizeof(canonical_stdout) /
                       sizeof(canonical_stdout[0])) == 0 &&
              test_contains(files.stdout_path, "HWA_RUN,1") &&
              !test_contains(files.stdout_path, "\r\r\n"),
          "canonical Stage 7 standard output failed");
    CHECK(test_run(&files, all_caps,
                   sizeof(all_caps) / sizeof(all_caps[0])) == 0,
          "Stage 7 resource cap flags did not reach the run options");
    (void)test_unlink(files.second);
    CHECK(test_run(&files, under_stem_cap,
                   sizeof(under_stem_cap) /
                       sizeof(under_stem_cap[0])) != 0 &&
              !test_exists(files.second),
          "one-under Stage 7 stem cap published an output");

    CHECK(test_run(&files, block_one,
                   sizeof(block_one) / sizeof(block_one[0])) == 0 &&
              test_run(&files, block_many,
                       sizeof(block_many) / sizeof(block_many[0])) == 0 &&
              test_normalize_run(files.output, files.normalized_a) &&
              test_normalize_run(files.second, files.normalized_b) &&
              test_file_equal(files.normalized_a, files.normalized_b),
          "Stage 7 result changed with decode block size");
    (void)test_unlink(files.second);
    (void)test_unlink(files.alias);
    CHECK(test_link(files.manifest, files.alias),
          "cannot make Stage 7 manifest hard-link alias");
    CHECK(test_run(&files, alias_output,
                   sizeof(alias_output) / sizeof(alias_output[0])) != 0 &&
              test_unchanged(files.manifest, manifest_before, manifest_size),
          "Stage 7 wrote through a manifest hard link");
    (void)test_unlink(files.alias);
    CHECK(test_link(files.reference, files.alias),
          "cannot make Stage 7 binding hard-link alias");
    CHECK(test_run(&files, alias_output,
                   sizeof(alias_output) / sizeof(alias_output[0])) != 0 &&
              test_unchanged(files.reference,
                             reference_before, reference_size),
          "Stage 7 wrote through a binding hard link");
    (void)test_unlink(files.alias);
#if !defined(_WIN32)
    CHECK(test_symlink(files.output, files.alias),
          "cannot make Stage 7 output symlink");
    CHECK(test_run(&files, alias_output,
                   sizeof(alias_output) / sizeof(alias_output[0])) != 0,
          "Stage 7 accepted a symlink output");
    (void)test_unlink(files.alias);
    CHECK(test_mkdir(files.alias) == 0,
          "cannot make Stage 7 output directory");
    CHECK(test_run(&files, alias_output,
                   sizeof(alias_output) / sizeof(alias_output[0])) != 0,
          "Stage 7 accepted a directory output");
    (void)test_rmdir(files.alias);
    CHECK(mkfifo(files.alias, 0600) == 0,
          "cannot make Stage 7 output FIFO");
    CHECK(test_run(&files, alias_output,
                   sizeof(alias_output) / sizeof(alias_output[0])) != 0,
          "Stage 7 accepted a FIFO output");
    (void)test_unlink(files.alias);
    CHECK(test_write_text(files.second, "old run output"),
          "cannot make Stage 7 broken-report sentinel");
    sentinel_before = test_read(files.second, &sentinel_size);
    CHECK(sentinel_before != NULL &&
              test_broken_stdout(
                  &files, broken, sizeof(broken) / sizeof(broken[0])) == 1 &&
              test_unchanged(files.second,
                             sentinel_before, sentinel_size),
          "broken Stage 7 report changed an old replacement");
#endif
    CHECK(test_unchanged(files.manifest, manifest_before, manifest_size) &&
              test_unchanged(files.reference,
                             reference_before, reference_size) &&
              test_unchanged(files.model, model_before, model_size),
          "Stage 7 changed a successful-run input");

    free(sentinel_before);
    free(manifest_before);
    free(reference_before);
    free(model_before);
    test_files_close(&files);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        (void)fputs("usage: stage7_cli_tests ANALYZER\n", stderr);
        return 2;
    }
    analyzer_path = argv[1];
    test_parse_and_failure_safety();
    test_success_and_publication();
    if (failures != 0) {
        (void)fprintf(stderr, "%d Stage 7 CLI test(s) failed\n", failures);
        return 1;
    }
    (void)puts("Stage 7 CLI tests passed");
    return 0;
}

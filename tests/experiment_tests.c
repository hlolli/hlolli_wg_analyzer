#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "experiment.h"
#include "hlolli_wg_analyzer.h"
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
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct TestFiles {
    char root[PATH_MAX];
    char reference[PATH_MAX];
    char manifest[PATH_MAX];
    char output[PATH_MAX];
    char second[PATH_MAX];
    char third[PATH_MAX];
} TestFiles;

typedef struct TestRenderer {
    size_t calls;
    size_t fail_at;
    int request_bad;
} TestRenderer;

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

static int test_exists(const char *path)
{
#if defined(_WIN32)
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat info;
    return lstat(path, &info) == 0;
#endif
}

static int test_join(char out[PATH_MAX], const char *left, const char *right)
{
    int length = snprintf(out, PATH_MAX, "%s/%s", left, right);
    return length > 0 && length < PATH_MAX;
}

static int test_remove_tree(const char *path)
{
#if defined(_WIN32)
    WIN32_FIND_DATAA entry;
    char pattern[PATH_MAX];
    HANDLE search;
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return 0;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U)
        return DeleteFileA(path) != 0 ? 0 : -1;
    if (snprintf(pattern, sizeof(pattern), "%s/*", path) < 0) return -1;
    search = FindFirstFileA(pattern, &entry);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            char child[PATH_MAX];
            if (strcmp(entry.cFileName, ".") == 0 ||
                strcmp(entry.cFileName, "..") == 0) continue;
            if (!test_join(child, path, entry.cFileName) ||
                test_remove_tree(child) != 0) {
                FindClose(search);
                return -1;
            }
        } while (FindNextFileA(search, &entry));
        FindClose(search);
    }
    return RemoveDirectoryA(path) != 0 ? 0 : -1;
#else
    struct stat info;
    DIR *directory;
    struct dirent *entry;
    if (lstat(path, &info) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(info.st_mode)) return unlink(path);
    directory = opendir(path);
    if (directory == NULL) return -1;
    while ((entry = readdir(directory)) != NULL) {
        char child[PATH_MAX];
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;
        if (!test_join(child, path, entry->d_name) ||
            test_remove_tree(child) != 0) {
            (void)closedir(directory);
            return -1;
        }
    }
    if (closedir(directory) != 0) return -1;
    return rmdir(path);
#endif
}

static int test_files_open(TestFiles *files)
{
    unsigned attempt;
    memset(files, 0, sizeof(*files));
    for (attempt = 0U; attempt < 100U; ++attempt) {
        int length = snprintf(files->root, sizeof(files->root),
                              "/tmp/hwa-experiment-%ld-%u",
                              test_pid(), attempt);
        if (length < 0 || (size_t)length >= sizeof(files->root)) return 0;
        if (test_mkdir(files->root) == 0) break;
        if (errno != EEXIST) return 0;
    }
    return attempt < 100U &&
        test_join(files->reference, files->root, "reference.wav") &&
        test_join(files->manifest, files->root, "experiment.json") &&
        test_join(files->output, files->root, "bundle-a") &&
        test_join(files->second, files->root, "bundle-b") &&
        test_join(files->third, files->root, "bundle-c");
}

static void test_u16(FILE *stream, uint16_t value)
{
    (void)fputc((int)(value & UINT16_C(0xff)), stream);
    (void)fputc((int)((value >> 8U) & UINT16_C(0xff)), stream);
}

static void test_u32(FILE *stream, uint32_t value)
{
    (void)fputc((int)(value & UINT32_C(0xff)), stream);
    (void)fputc((int)((value >> 8U) & UINT32_C(0xff)), stream);
    (void)fputc((int)((value >> 16U) & UINT32_C(0xff)), stream);
    (void)fputc((int)((value >> 24U) & UINT32_C(0xff)), stream);
}

static int test_wave_write(const char *path, double scale)
{
    const uint32_t frames = UINT32_C(4096);
    const uint32_t bytes = frames * UINT32_C(2);
    FILE *stream = fopen(path, "wb");
    uint32_t index;
    if (stream == NULL) return -1;
    if (fwrite("RIFF", 1U, 4U, stream) != 4U) goto failed;
    test_u32(stream, UINT32_C(36) + bytes);
    if (fwrite("WAVEfmt ", 1U, 8U, stream) != 8U) goto failed;
    test_u32(stream, UINT32_C(16));
    test_u16(stream, UINT16_C(1));
    test_u16(stream, UINT16_C(1));
    test_u32(stream, UINT32_C(48000));
    test_u32(stream, UINT32_C(96000));
    test_u16(stream, UINT16_C(2));
    test_u16(stream, UINT16_C(16));
    if (fwrite("data", 1U, 4U, stream) != 4U) goto failed;
    test_u32(stream, bytes);
    for (index = 0U; index < frames; ++index) {
        double phase = (double)(index % 97U) / 97.0;
        long sample = lround((phase * 2.0 - 1.0) * 12000.0 * scale);
        if (sample > INT16_MAX) sample = INT16_MAX;
        if (sample < INT16_MIN) sample = INT16_MIN;
        test_u16(stream, (uint16_t)(int16_t)sample);
    }
    if (fflush(stream) != 0 || ferror(stream) || fclose(stream) != 0)
        return -1;
    return 0;
failed:
    (void)fclose(stream);
    return -1;
}

static int test_absolute(const char *path)
{
#if defined(_WIN32)
    return strlen(path) > 2U && path[1] == ':';
#else
    return path != NULL && path[0] == '/';
#endif
}

static int test_render(void *context,
                       const HWAExperimentRenderRequest *request,
                       char *error,
                       size_t error_size)
{
    TestRenderer *renderer = (TestRenderer *)context;
    size_t index;
    double alpha = 0.0;
    double beta = 0.0;
    double scale;
    renderer->calls++;
    if (renderer->fail_at != 0U && renderer->calls == renderer->fail_at) {
        (void)snprintf(error, error_size, "planned renderer failure");
        return -1;
    }
    if (request == NULL || request->job_id != renderer->calls ||
        request->job_key == NULL || strlen(request->job_key) != 64U ||
        request->input_count != 1U || request->output_count != 1U ||
        request->inputs == NULL || request->outputs == NULL ||
        request->inputs[0].kind != HWA_RUN_SOURCE_STEM ||
        request->inputs[0].side != HWA_RUN_REFERENCE ||
        request->inputs[0].role != HWA_RUN_STEM_FINAL ||
        request->outputs[0].kind != HWA_RUN_SOURCE_STEM ||
        request->outputs[0].side != HWA_RUN_MODEL ||
        request->outputs[0].role != HWA_RUN_STEM_FINAL ||
        request->outputs[0].rate_hz != 48000U ||
        request->outputs[0].channels != 1U ||
        request->max_output_file_bytes == 0U ||
        request->max_output_bytes < request->max_output_file_bytes ||
        !test_absolute(request->job_directory) ||
        !test_absolute(request->request_path) ||
        !test_absolute(request->inputs[0].path) ||
        !test_absolute(request->outputs[0].path)) {
        renderer->request_bad = 1;
        (void)snprintf(error, error_size, "bad request descriptors");
        return -1;
    }
    for (index = 0U; index < request->parameter_count; ++index) {
        if (strcmp(request->parameters[index].id, "alpha") == 0)
            alpha = request->parameters[index].value;
        else if (strcmp(request->parameters[index].id, "beta") == 0)
            beta = request->parameters[index].value;
    }
    scale = 0.45 + 0.18 * alpha + 0.11 * beta + 0.04 * alpha * beta;
    if (scale < 0.05) scale = 0.05;
    return test_wave_write(request->outputs[0].path, scale);
}

static int test_manifest_write_mode(const TestFiles *files,
                                    const char *parameters,
                                    const char *plan,
                                    const char *model_input,
                                    const char *model_output)
{
    char hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    FILE *stream;
    int okay;
    if (hwa_sha256_file(files->reference, UINT64_C(1048576), hash,
                        error, sizeof(error)) != 0) return 0;
    stream = fopen(files->manifest, "wb");
    if (stream == NULL) return 0;
    okay = fprintf(
        stream,
        "{\"schema\":\"hwa-experiment\",\"schema_version\":1,"
        "\"method_version\":\"stage8-1\",\"clock_rate_hz\":48000,"
        "\"inputs\":[{\"id\":\"artist\",\"sha256\":\"%s\"}],"
        "\"parameters\":%s,\"plan\":%s,\"cases\":["
        "{\"id\":\"check\",\"split\":\"check\",\"weight\":1,"
        "\"stems\":[{\"id\":\"model.final\",\"side\":\"model\","
        "\"role\":\"final\",\"input_id\":%s,\"output\":%s,"
        "\"start_sample\":0,\"gain_db\":0,\"rate_hz\":48000,"
        "\"channels\":1},{\"id\":\"reference.final\","
        "\"side\":\"reference\",\"role\":\"final\","
        "\"input_id\":\"artist\",\"output\":null,\"start_sample\":0,"
        "\"gain_db\":0,\"rate_hz\":48000,\"channels\":1}],"
        "\"probes\":[],\"links\":[]},"
        "{\"id\":\"fit\",\"split\":\"fit\",\"weight\":2,"
        "\"stems\":[{\"id\":\"model.final\",\"side\":\"model\","
        "\"role\":\"final\",\"input_id\":%s,\"output\":%s,"
        "\"start_sample\":0,\"gain_db\":0,\"rate_hz\":48000,"
        "\"channels\":1},{\"id\":\"reference.final\","
        "\"side\":\"reference\",\"role\":\"final\","
        "\"input_id\":\"artist\",\"output\":null,\"start_sample\":0,"
        "\"gain_db\":0,\"rate_hz\":48000,\"channels\":1}],"
        "\"probes\":[],\"links\":[]}],"
        "\"responses\":[{\"id\":\"final.rms\",\"role\":\"final\","
        "\"feature\":\"rms_dbfs\",\"index\":0}]}\n",
        hash, parameters, plan, model_input, model_output,
        model_input, model_output) > 0;
    if (fflush(stream) != 0 || ferror(stream) || fclose(stream) != 0)
        okay = 0;
    return okay;
}

static int test_manifest_write(const TestFiles *files,
                               const char *parameters,
                               const char *plan)
{
    return test_manifest_write_mode(
        files, parameters, plan, "null", "\"model.wav\"");
}

static int test_execute(const TestFiles *files,
                        const char *output,
                        const HWAExperimentOptions *options,
                        TestRenderer *state,
                        HWAExperimentResult *result,
                        char *error)
{
    HWARunBinding binding;
    HWAExperimentRenderer renderer;
    binding.id = "artist";
    binding.path = files->reference;
    renderer.id = "test-renderer";
    renderer.sha256 =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    renderer.context = state;
    renderer.render = test_render;
    return hwa_execute_experiment_files(
        files->manifest, &binding, 1U, output, NULL, &renderer,
        options, result, error, HWA_ERROR_SIZE);
}

static int test_execute_resume(const TestFiles *files,
                               const char *output,
                               const char *resume,
                               const HWAExperimentOptions *options,
                               TestRenderer *state,
                               HWAExperimentResult *result,
                               char *error)
{
    HWARunBinding binding;
    HWAExperimentRenderer renderer;
    binding.id = "artist";
    binding.path = files->reference;
    renderer.id = "test-renderer";
    renderer.sha256 =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    renderer.context = state;
    renderer.render = test_render;
    return hwa_execute_experiment_files(
        files->manifest, &binding, 1U, output, resume, &renderer,
        options, result, error, HWA_ERROR_SIZE);
}

static unsigned char *test_read(const char *path, size_t *size)
{
    FILE *stream = fopen(path, "rb");
    long length;
    unsigned char *data;
    if (stream == NULL || fseek(stream, 0L, SEEK_END) != 0 ||
        (length = ftell(stream)) < 0L || fseek(stream, 0L, SEEK_SET) != 0) {
        if (stream != NULL) (void)fclose(stream);
        return NULL;
    }
    data = (unsigned char *)malloc((size_t)length);
    if (data == NULL || fread(data, 1U, (size_t)length, stream) !=
                            (size_t)length || fclose(stream) != 0) {
        free(data);
        return NULL;
    }
    *size = (size_t)length;
    return data;
}

static int test_bundle_equal(const char *left_root, const char *right_root)
{
    char left[PATH_MAX];
    char right[PATH_MAX];
    unsigned char *left_data;
    unsigned char *right_data;
    size_t left_size = 0U;
    size_t right_size = 0U;
    int equal;
    if (!test_join(left, left_root, "result.hwa-experiment") ||
        !test_join(right, right_root, "result.hwa-experiment")) return 0;
    left_data = test_read(left, &left_size);
    right_data = test_read(right, &right_size);
    equal = left_data != NULL && right_data != NULL &&
        left_size == right_size && memcmp(left_data, right_data, left_size) == 0;
    free(left_data);
    free(right_data);
    return equal;
}

static void test_oat_and_limits(TestFiles *files)
{
    static const char parameters[] =
        "[{\"id\":\"alpha\",\"unit\":\"ratio\",\"minimum\":0,"
        "\"maximum\":2,\"baseline\":1,\"levels\":[0,1,2]},"
        "{\"id\":\"beta\",\"unit\":\"ratio\",\"minimum\":0,"
        "\"maximum\":1,\"baseline\":1,\"levels\":[0,1]}]";
    static const char plan[] =
        "{\"kind\":\"one-at-a-time\",\"seed\":19,"
        "\"sample_count\":0,\"replicates\":2}";
    HWAExperimentOptions options;
    HWAExperimentResult first;
    HWAExperimentResult second;
    HWAExperimentResult blocked;
    TestRenderer renderer;
    char error[HWA_ERROR_SIZE];
    uint64_t exact_evaluations;
    char existing[PATH_MAX];
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    memset(&blocked, 0, sizeof(blocked));
    memset(&renderer, 0, sizeof(renderer));
    hwa_experiment_options_default(&options);
    CHECK(test_manifest_write(files, parameters, plan),
          "cannot write OAT manifest");
    CHECK(test_execute(files, files->output, &options, &renderer,
                       &first, error) == 0,
          "OAT execution failed: %s", error);
    CHECK(!renderer.request_bad && renderer.calls == 16U &&
              first.point_count == 4U && first.job_count == 16U &&
              first.observation_count == 16U && first.candidate_count == 8U &&
              first.sensitivity_count == 4U && first.interaction_count == 2U,
          "OAT row counts or callback request order are wrong");
    CHECK(strcmp(first.jobs[0].key,
                 "7e5caf6a75d0f95e0cdd9d8f0237cfe4d4488436ae93eeb892e8d526082fefa8") == 0,
          "canonical job-key preimage changed");
    CHECK(first.values[0].value == 1.0 && first.values[1].value == 1.0 &&
              first.values[2].value == 0.0 && first.values[3].value == 1.0 &&
              first.values[4].value == 2.0 && first.values[5].value == 1.0 &&
              first.values[6].value == 1.0 && first.values[7].value == 0.0,
          "OAT point order is wrong");
    CHECK(first.sensitivities[0].linear_valid &&
              first.sensitivities[0].point_count == 3U &&
              !first.sensitivities[0].effect_valid &&
              first.interactions[0].availability == HWA_RUN_UNAVAILABLE,
          "OAT sensitivity or interaction contract is wrong");
    exact_evaluations = first.total_run_evaluations;
    memset(&renderer, 0, sizeof(renderer));
    CHECK(test_execute(files, files->second, &options, &renderer,
                       &second, error) == 0 &&
              test_bundle_equal(files->output, files->second),
          "same OAT plan did not produce identical canonical bytes: %s", error);
    memset(&renderer, 0, sizeof(renderer));
    CHECK(test_execute_resume(files, files->third, files->output, &options,
                              &renderer, &blocked, error) == 0 &&
              renderer.calls == 0U &&
              blocked.rendered_job_count == 0U &&
              blocked.reused_job_count == blocked.job_count &&
              blocked.resume_directory != NULL &&
              test_bundle_equal(files->output, files->third),
          "fully resumed OAT bundle differs: %s", error);
    hwa_experiment_result_free(&blocked);
    (void)test_remove_tree(files->third);
    CHECK(hwa_experiment_output_remove(&second, error, sizeof(error)) == 0,
          "cannot replace same-cap fixture: %s", error);
    hwa_experiment_result_free(&second);
    options.run.max_warnings++;
    memset(&renderer, 0, sizeof(renderer));
    CHECK(test_execute(files, files->second, &options, &renderer,
                       &second, error) == 0,
          "changed-cap fresh run failed: %s", error);
    memset(&renderer, 0, sizeof(renderer));
    CHECK(test_execute_resume(files, files->third, files->output, &options,
                              &renderer, &blocked, error) == 0 &&
              renderer.calls == 0U,
          "changed-cap resume failed: %s", error);
    CHECK(test_bundle_equal(files->second, files->third),
          "changed-cap fresh and resumed canonical bytes differ");
    hwa_experiment_result_free(&blocked);
    (void)test_remove_tree(files->third);
    hwa_experiment_options_default(&options);
    options.run.decode_block_frames /= 2U;
    memset(&renderer, 0, sizeof(renderer));
    CHECK(test_execute_resume(files, files->third, files->output, &options,
                              &renderer, &blocked, error) != 0 &&
              renderer.calls == 0U && !test_exists(files->third),
          "changed decode block did not reject resume keys");
    hwa_experiment_result_free(&blocked);
    (void)test_remove_tree(files->third);
    hwa_experiment_options_default(&options);
    options.max_work_bytes = UINT64_C(1024);
    memset(&renderer, 0, sizeof(renderer));
    CHECK(test_execute(files, files->third, &options, &renderer,
                       &blocked, error) != 0 && renderer.calls == 0U &&
              !test_exists(files->third),
          "peak work cap failed after render or published output");
    hwa_experiment_options_default(&options);
    options.max_output_file_bytes = UINT64_C(32);
    memset(&renderer, 0, sizeof(renderer));
    CHECK(test_execute(files, files->third, &options, &renderer,
                       &blocked, error) != 0 && renderer.calls == 0U &&
              !test_exists(files->third),
          "request file cap failed after render or published output");
    hwa_experiment_options_default(&options);
    options.max_points = 3U;
    memset(&renderer, 0, sizeof(renderer));
    CHECK(test_execute(files, files->third, &options, &renderer,
                       &blocked, error) != 0 && renderer.calls == 0U &&
              !test_exists(files->third),
          "point cap failed after render or published output");
    hwa_experiment_options_default(&options);
    options.max_jobs = 15U;
    memset(&renderer, 0, sizeof(renderer));
    CHECK(test_execute(files, files->third, &options, &renderer,
                       &blocked, error) != 0 && renderer.calls == 0U &&
              !test_exists(files->third),
          "job cap failed after render or published output");
    hwa_experiment_options_default(&options);
    options.max_sensitivities = 3U;
    memset(&renderer, 0, sizeof(renderer));
    CHECK(test_execute(files, files->third, &options, &renderer,
                       &blocked, error) != 0 && renderer.calls == 0U &&
              !test_exists(files->third),
          "sensitivity cap failed after render or published output");
    hwa_experiment_options_default(&options);
    options.max_interactions = 1U;
    memset(&renderer, 0, sizeof(renderer));
    CHECK(test_execute(files, files->third, &options, &renderer,
                       &blocked, error) != 0 && renderer.calls == 0U &&
              !test_exists(files->third),
          "interaction cap failed after render or published output");
    hwa_experiment_options_default(&options);
    options.max_total_run_evaluations = exact_evaluations - UINT64_C(1);
    memset(&renderer, 0, sizeof(renderer));
    CHECK(test_execute(files, files->third, &options, &renderer,
                       &blocked, error) != 0 && !test_exists(files->third),
          "one-under evaluation cap published output");
    hwa_experiment_options_default(&options);
    renderer.fail_at = 3U;
    CHECK(test_execute(files, files->third, &options, &renderer,
                       &blocked, error) != 0 && !test_exists(files->third),
          "renderer failure left a bundle");
    CHECK(test_join(existing, files->third, "sentinel") &&
              test_mkdir(files->third) == 0,
          "cannot create existing output fixture");
    {
        FILE *sentinel = fopen(existing, "wb");
        CHECK(sentinel != NULL && fputs("keep", sentinel) != EOF &&
                  fclose(sentinel) == 0,
              "cannot write existing output sentinel");
    }
    memset(&renderer, 0, sizeof(renderer));
    CHECK(test_execute(files, files->third, &options, &renderer,
                       &blocked, error) != 0 && test_exists(existing),
          "existing output was replaced");
    {
        char extra[PATH_MAX];
        FILE *extra_stream = NULL;
        CHECK(test_join(extra, files->output, "extra") &&
                  (extra_stream = fopen(extra, "wb")) != NULL &&
                  fputs("changed", extra_stream) != EOF &&
                  fclose(extra_stream) == 0,
              "cannot write output-removal authority fixture");
        CHECK(hwa_experiment_output_remove(&first, error, sizeof(error)) != 0 &&
                  test_exists(files->output),
              "bundle removal accepted an extra file");
        CHECK(test_remove_tree(extra) == 0,
              "cannot remove output-removal fixture");
    }
    CHECK(hwa_experiment_output_remove(&first, error, sizeof(error)) == 0 &&
              !test_exists(files->output),
          "identity-bound bundle removal failed: %s", error);
    hwa_experiment_result_free(&blocked);
    hwa_experiment_result_free(&second);
    hwa_experiment_result_free(&first);
    (void)test_remove_tree(files->second);
    (void)test_remove_tree(files->third);
}

static long double test_variance(const double *values, size_t count)
{
    long double sum = 0.0L;
    long double square = 0.0L;
    size_t index;
    for (index = 0U; index < count; ++index) {
        sum += values[index];
        square += (long double)values[index] * values[index];
    }
    return square / (long double)count -
           (sum / (long double)count) * (sum / (long double)count);
}

static void test_grid(TestFiles *files)
{
    static const char parameters[] =
        "[{\"id\":\"alpha\",\"unit\":\"ratio\",\"minimum\":0,"
        "\"maximum\":1,\"baseline\":1,\"levels\":[0,1]},"
        "{\"id\":\"beta\",\"unit\":\"ratio\",\"minimum\":10,"
        "\"maximum\":20,\"baseline\":20,\"levels\":[10,20]}]";
    static const char plan[] =
        "{\"kind\":\"grid\",\"seed\":31,\"sample_count\":0,"
        "\"replicates\":1}";
    HWAExperimentResult result;
    HWAExperimentOptions options;
    TestRenderer renderer;
    char error[HWA_ERROR_SIZE];
    double response_values[4];
    size_t point;
    memset(&result, 0, sizeof(result));
    memset(&renderer, 0, sizeof(renderer));
    hwa_experiment_options_default(&options);
    CHECK(test_manifest_write(files, parameters, plan),
          "cannot write grid manifest");
    CHECK(test_execute(files, files->output, &options, &renderer,
                       &result, error) == 0,
          "grid execution failed: %s", error);
    CHECK(result.point_count == 4U && result.job_count == 8U &&
              result.values[0].value == 1.0 && result.values[1].value == 20.0 &&
              result.values[2].value == 0.0 && result.values[3].value == 10.0 &&
              result.values[4].value == 0.0 && result.values[5].value == 20.0 &&
              result.values[6].value == 1.0 && result.values[7].value == 10.0,
          "grid baseline-first lexicographic order is wrong");
    CHECK(result.sensitivities[0].effect_valid &&
              result.interactions[0].effect_valid &&
              result.interactions[0].point_count == 4U,
          "grid main effect or interaction is unavailable");
    for (point = 0U; point < 4U; ++point)
        response_values[point] = result.candidates[point * 2U].mean_gap;
    CHECK(test_variance(response_values, 4U) > 0.0L &&
              result.sensitivities[0].effect_fraction >= 0.0 &&
              result.sensitivities[0].effect_fraction <= 1.0000000001 &&
              result.interactions[0].effect_fraction >= 0.0,
          "grid variance formulas are out of range");
    hwa_experiment_result_free(&result);
    (void)test_remove_tree(files->output);
}

static uint64_t test_splitmix64(uint64_t *state)
{
    uint64_t value;
    *state += UINT64_C(0x9e3779b97f4a7c15);
    value = *state;
    value = (value ^ (value >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27U)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

static double test_random(uint64_t *state)
{
    return (double)(test_splitmix64(state) >> 11U) / 9007199254740992.0;
}

static void test_random_plan(TestFiles *files)
{
    static const char parameters[] =
        "[{\"id\":\"alpha\",\"unit\":\"ratio\",\"minimum\":0,"
        "\"maximum\":2,\"baseline\":1,\"levels\":[]},"
        "{\"id\":\"beta\",\"unit\":\"ratio\",\"minimum\":10,"
        "\"maximum\":20,\"baseline\":15,\"levels\":[]}]";
    static const char plan[] =
        "{\"kind\":\"random\",\"seed\":41,\"sample_count\":3,"
        "\"replicates\":1}";
    HWAExperimentResult result;
    HWAExperimentOptions options;
    TestRenderer renderer;
    char error[HWA_ERROR_SIZE];
    uint64_t state = UINT64_C(41);
    size_t point;
    memset(&result, 0, sizeof(result));
    memset(&renderer, 0, sizeof(renderer));
    hwa_experiment_options_default(&options);
    CHECK(test_manifest_write(files, parameters, plan),
          "cannot write random manifest");
    CHECK(test_execute(files, files->output, &options, &renderer,
                       &result, error) == 0,
          "random execution failed: %s", error);
    CHECK(result.point_count == 4U && result.level_count == 0U &&
              result.values[0].value == 1.0 && result.values[1].value == 15.0,
          "random baseline is wrong");
    for (point = 1U; point < 4U; ++point) {
        double alpha = 2.0 * test_random(&state);
        double beta = 10.0 + 10.0 * test_random(&state);
        CHECK(result.values[point * 2U].value == alpha &&
                  result.values[point * 2U + 1U].value == beta,
              "random SplitMix64 point %zu differs", point);
    }
    CHECK((result.sensitivities[0].quality_flags &
           HWA_EXPERIMENT_QUALITY_RANDOM_LINEAR_ONLY) != 0U &&
              !result.sensitivities[0].effect_valid &&
              result.interactions[0].availability == HWA_RUN_UNAVAILABLE,
          "random method flags or unavailable effects are wrong");
    hwa_experiment_result_free(&result);
    (void)test_remove_tree(files->output);
}

static void test_authority_and_hostile(TestFiles *files)
{
    static const char parameters[] =
        "[{\"id\":\"alpha\",\"unit\":\"ratio\",\"minimum\":0,"
        "\"maximum\":1,\"baseline\":0,\"levels\":[0,1]}]";
    static const char plan[] =
        "{\"kind\":\"one-at-a-time\",\"seed\":1,"
        "\"sample_count\":0,\"replicates\":1}";
    HWAExperimentOptions options;
    HWAExperimentResult result;
    HWAExperimentRenderer renderer;
    TestRenderer state;
    HWARunBinding binding;
    char error[HWA_ERROR_SIZE];
    FILE *stream;
    memset(&result, 0, sizeof(result));
    memset(&state, 0, sizeof(state));
    hwa_experiment_options_default(&options);
    CHECK(test_manifest_write(files, parameters, plan),
          "cannot write authority manifest");
    renderer.id = "test-renderer";
    renderer.sha256 =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    renderer.context = &state;
    renderer.render = test_render;
    binding.id = "wrong";
    binding.path = files->reference;
    CHECK(hwa_execute_experiment_files(
              files->manifest, &binding, 1U, files->output, NULL,
              &renderer, &options, &result, error, sizeof(error)) != 0 &&
              state.calls == 0U && !test_exists(files->output),
          "wrong fixed-input authority reached renderer");
    binding.id = "artist";
    CHECK(test_manifest_write_mode(files, parameters, plan,
                                   "\"artist\"", "null"),
          "cannot write zero-output manifest");
    state.calls = 0U;
    CHECK(hwa_execute_experiment_files(
              files->manifest, &binding, 1U, files->output, NULL,
              &renderer, &options, &result, error, sizeof(error)) != 0 &&
              state.calls == 0U && !test_exists(files->output),
          "zero-output case reached renderer");
    stream = fopen(files->manifest, "wb");
    CHECK(stream != NULL &&
              fputs("{\"schema\":\"hwa-experiment\",\"schema_version\":1,"
                    "\"method_version\":\"stage8-1\",\"clock_rate_hz\":48000,"
                    "\"inputs\":[],\"parameters\":[{\"id\":\"x\","
                    "\"unit\":\"ratio\",\"minimum\":0,\"maximum\":1,"
                    "\"baseline\":NaN,\"levels\":[]}],\"plan\":{},"
                    "\"cases\":[],\"responses\":[]}", stream) != EOF &&
              fclose(stream) == 0,
          "cannot write hostile manifest");
    state.calls = 0U;
    CHECK(hwa_execute_experiment_files(
              files->manifest, NULL, 0U, files->output, NULL,
              &renderer, &options, &result, error, sizeof(error)) != 0 &&
              state.calls == 0U && !test_exists(files->output),
          "hostile non-finite manifest reached renderer");
    stream = fopen(files->manifest, "wb");
    CHECK(stream != NULL &&
              fputs("{\"schema\":\"hwa-experiment\",\"schema_version\":1,"
                    "\"method_version\":\"stage8-1\","
                    "\"clock_rate_hz\":48000,\"inputs\":[],"
                    "\"parameters\":[],\"plan\":{\"kind\":\"grid\","
                    "\"seed\":0,\"sample_count\":0,\"replicates\":1},"
                    "\"cases\":[{\"id\":\"partial\",\"split\":\"fit\","
                    "\"weight\":1,\"stems\":[{\"id\":\"held\"}],"
                    "\"unknown\":0}],\"responses\":[]}", stream) != EOF &&
              fclose(stream) == 0,
          "cannot write partial-case manifest");
    state.calls = 0U;
    CHECK(hwa_execute_experiment_files(
              files->manifest, NULL, 0U, files->output, NULL,
              &renderer, &options, &result, error, sizeof(error)) != 0 &&
              state.calls == 0U && !test_exists(files->output),
          "partial malformed case reached renderer");
    hwa_experiment_result_free(&result);
}

int main(void)
{
    TestFiles files;
    if (!test_files_open(&files) || test_wave_write(files.reference, 1.0) != 0)
        return 2;
    test_oat_and_limits(&files);
    test_grid(&files);
    test_random_plan(&files);
    test_authority_and_hostile(&files);
    CHECK(test_remove_tree(files.root) == 0,
          "cannot remove experiment fixture tree");
    if (failures != 0) {
        (void)fprintf(stderr, "%d experiment test(s) failed\n", failures);
        return 1;
    }
    (void)puts("experiment tests passed");
    return 0;
}

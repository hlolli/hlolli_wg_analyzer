#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "measure_compare.h"
#include "measure_file.h"
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
#define TEST_FRAMES 8000U
#define TEST_PI 3.14159265358979323846264338327950288

typedef struct TestFiles {
    char directory[PATH_MAX];
    char reference[PATH_MAX];
    char model[PATH_MAX];
    char wave[PATH_MAX];
    char output[PATH_MAX];
    char second[PATH_MAX];
    char alias[PATH_MAX];
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
                               "%s/hwa-stage5-cli-%ld-%u",
                               root, test_pid(), attempt);
        if (written < 0 ||
            (size_t)written >= sizeof(files->directory)) return 0;
        if (test_mkdir(files->directory) == 0) break;
        if (errno != EEXIST) return 0;
    }
    return attempt < 100U &&
           test_join(files->reference, files->directory, "reference.hwa-measures") &&
           test_join(files->model, files->directory, "model.hwa-measures") &&
           test_join(files->wave, files->directory, "scan.wav") &&
           test_join(files->output, files->directory, "result.hwa-physical") &&
           test_join(files->second, files->directory, "second.hwa-physical") &&
           test_join(files->alias, files->directory, "alias.hwa-physical") &&
           test_join(files->stdout_path, files->directory, "stdout.txt") &&
           test_join(files->stderr_path, files->directory, "stderr.txt");
}

static void test_files_close(TestFiles *files)
{
    const char *paths[] = {
        files->reference, files->model, files->wave, files->output,
        files->second, files->alias, files->stdout_path, files->stderr_path
    };
    size_t index;
    for (index = 0U; index < sizeof(paths) / sizeof(paths[0]); ++index) {
        (void)test_unlink(paths[index]);
    }
    (void)test_rmdir(files->directory);
}

static char *test_profile_copy(HWAMeasurementSet *set, const char *text)
{
    size_t size = strlen(text) + 1U;
    char *copy = (char *)malloc(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
        set->retained_work_bytes += (uint64_t)size;
    }
    return copy;
}

static void test_hash(char target[HWA_SHA256_HEX_SIZE], char byte)
{
    size_t index;
    for (index = 0U; index < HWA_SHA256_HEX_SIZE - 1U; ++index) {
        target[index] = byte;
    }
    target[HWA_SHA256_HEX_SIZE - 1U] = '\0';
}

static int test_make_profile(HWAMeasurementSet *set,
                             double first,
                             double second)
{
    static const double confidence[3] = {0.9, 0.7, 0.5};
    static const char *elements[3] = {
        "element-a", "element-b", "element-a"
    };
    double values[3];
    size_t item;
    char error[HWA_ERROR_SIZE] = {0};
    memset(set, 0, sizeof(*set));
    hwa_measurement_options_default(&set->options);
    set->options.fft_size = 1024U;
    set->options.hop_size = 128U;
    set->options.max_partials = 4U;
    set->options.max_work_bytes = UINT64_C(67108864);
    set->items_path = test_profile_copy(set, "/not-opened/items.hwa-items");
    set->audio_path = test_profile_copy(set, "/not-opened/audio.wav");
    set->alignment_path = test_profile_copy(set, "/not-opened/take.hwa-align");
    set->source_score_path = test_profile_copy(set, "/not-opened/score.csv");
    test_hash(set->items_sha256, 'a');
    test_hash(set->audio_sha256, 'b');
    test_hash(set->alignment_sha256, 'c');
    test_hash(set->source_score_sha256, 'd');
    set->audio_format.container = HWA_CONTAINER_RIFF;
    set->audio_format.encoding = HWA_ENCODING_PCM;
    set->audio_format.channels = 1U;
    set->audio_format.sample_rate_hz = 16000U;
    set->audio_format.bits_per_sample = 16U;
    set->audio_format.valid_bits_per_sample = 16U;
    set->audio_format.block_align = 2U;
    set->audio_format.frames = 16000U;
    set->audio_format.data_bytes = 32000U;
    set->audio_format.duration_seconds = 1.0;
    set->level_reference_dbfs = 0.5 * (first + second);
    set->level_reference_item_count = 2U;
    set->level_reference_valid = 1;
    set->item_frame_evaluations = 6U;
    set->transform_count = 2U;
    set->context_count = 3U;
    set->measurement_count = 6U;
    set->warning_count = 1U;
    set->contexts = (HWAMeasureItemContext *)calloc(
        set->context_count, sizeof(*set->contexts));
    set->measurements = (HWAMeasureObservation *)calloc(
        set->measurement_count, sizeof(*set->measurements));
    set->warnings = (HWAMeasureWarning *)calloc(
        set->warning_count, sizeof(*set->warnings));
    if (set->contexts == NULL || set->measurements == NULL ||
        set->warnings == NULL || set->items_path == NULL ||
        set->audio_path == NULL || set->alignment_path == NULL ||
        set->source_score_path == NULL) {
        hwa_measurement_set_free(set);
        return 0;
    }
    set->retained_work_bytes +=
        (uint64_t)set->context_count * sizeof(*set->contexts) +
        (uint64_t)set->measurement_count * sizeof(*set->measurements) +
        (uint64_t)set->warning_count * sizeof(*set->warnings);
    values[0] = first;
    values[1] = second;
    values[2] = 0.0;
    for (item = 0U; item < set->context_count; ++item) {
        HWAMeasureItemContext *context = &set->contexts[item];
        HWAMeasureObservation *raw = &set->measurements[item * 2U];
        HWAMeasureObservation *relative = raw + 1;
        char key[32];
        (void)snprintf(key, sizeof(key), "body:%zu", item + 1U);
        context->item_id = (uint64_t)item + 1U;
        context->item_key = test_profile_copy(set, key);
        context->item_kind = HWA_ITEM_BODY;
        context->item_role = test_profile_copy(set, "body");
        context->labels.physical_element =
            test_profile_copy(set, elements[item]);
        context->labels.override_flags =
            HWA_LABEL_OVERRIDE_PHYSICAL_ELEMENT;
        context->start_sample = (uint64_t)item * 4000U;
        context->end_sample = context->start_sample + 3000U;
        context->source_event_count = 1U;
        context->item_confidence = confidence[item];
        raw->id = (uint64_t)item * 2U + 1U;
        raw->item_id = context->item_id;
        raw->kind = HWA_MEASURE_RMS_DBFS;
        raw->unit = HWA_MEASURE_UNIT_DBFS;
        raw->view = HWA_MEASURE_VIEW_RAW;
        raw->status = item < 2U ? HWA_MEASURE_STATUS_VALID :
                                 HWA_MEASURE_STATUS_NO_SIGNAL;
        raw->value = values[item];
        raw->confidence = confidence[item];
        relative->id = raw->id + 1U;
        relative->item_id = raw->item_id;
        relative->kind = raw->kind;
        relative->unit = HWA_MEASURE_UNIT_DB;
        relative->view = HWA_MEASURE_VIEW_LEVEL_RELATIVE;
        relative->status = raw->status;
        relative->value = item < 2U
                              ? values[item] - set->level_reference_dbfs : 0.0;
        relative->confidence = raw->confidence;
        relative->evidence_flags = item < 2U
            ? HWA_MEASURE_EVIDENCE_LEVEL_REFERENCE : 0U;
        if (context->item_key == NULL || context->item_role == NULL ||
            context->labels.physical_element == NULL) {
            hwa_measurement_set_free(set);
            return 0;
        }
    }
    set->warnings[0].id = 1U;
    set->warnings[0].code =
        test_profile_copy(set, "stage4-capability");
    set->warnings[0].message = test_profile_copy(
        set, "Production-corrected measurements are not available.");
    set->warnings[0].item_id = 1U;
    set->warnings[0].observation_id = 1U;
    set->warnings[0].item_id_valid = 1;
    set->warnings[0].observation_id_valid = 1;
    if (set->warnings[0].code == NULL || set->warnings[0].message == NULL ||
        hwa_measure_build_profile(set, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "profile fixture: %s\n", error);
        hwa_measurement_set_free(set);
        return 0;
    }
    return 1;
}

static int test_write_profile(const char *path,
                              const HWAMeasurementSet *set)
{
    FILE *stream = fopen(path, "wb");
    char error[HWA_ERROR_SIZE] = {0};
    int result;
    if (stream == NULL) return 0;
    result = hwa_measure_file_write(stream, set, error, sizeof(error));
    if (fclose(stream) != 0) result = -1;
    if (result != 0) (void)fprintf(stderr, "profile write: %s\n", error);
    return result == 0;
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

static int test_write_wav(const char *path)
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
        double sample = 0.1 * sin(2.0 * TEST_PI * 220.0 * time);
        okay = test_write_u16(
            stream, (uint16_t)(int16_t)lrint(sample * 32767.0));
    }
    if (fclose(stream) != 0) okay = 0;
    return okay;
}

#if !defined(_WIN32)
static int test_write_text(const char *path, const char *text)
{
    FILE *stream = fopen(path, "wb");
    size_t size = strlen(text);
    int okay = stream != NULL && fwrite(text, 1U, size, stream) == size;
    if (stream != NULL && fclose(stream) != 0) okay = 0;
    return okay;
}
#endif

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
        if (*cursor == (unsigned char)'"') {
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
    char command[PATH_MAX * 14U];
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
    char *exec_arguments[34];
    int output_pipe[2];
    pid_t child;
    pid_t waited;
    size_t index;
    int status;
    if (argument_count > 32U || pipe(output_pipe) != 0) return -1;
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

static int test_unchanged(const char *path,
                          const unsigned char *before,
                          size_t before_size)
{
    size_t size;
    unsigned char *after = test_read(path, &size);
    int same = after != NULL && size == before_size &&
               memcmp(after, before, size) == 0;
    free(after);
    return same;
}

static void test_cli(void)
{
    TestFiles files;
    HWAMeasurementSet reference;
    HWAMeasurementSet model;
    unsigned char *reference_before;
    unsigned char *model_before;
    unsigned char *wave_before;
    size_t reference_size;
    size_t model_size;
    size_t wave_size;
#if !defined(_WIN32)
    unsigned char *sentinel_before;
    size_t sentinel_size;
#endif
    char binding[PATH_MAX + 64U];
    const char *basic[] = {
        "check-physical", files.reference, files.model,
        "--output", files.output
    };
    const char *exclusive[] = {
        "check-physical", files.reference, files.model,
        "--output", files.output
    };
    const char *replace[] = {
        "--replace", "check-physical", files.reference, files.model,
        "--output", files.output
    };
    const char *json[] = {
        "--json", "check-physical", files.reference, files.model,
        "--output", files.second
    };
    const char *canonical_stdout[] = {
        "check-physical", files.reference, files.model, "--output", "-"
    };
    const char *json_stdout[] = {
        "--json", "check-physical", files.reference, files.model,
        "--output", "-"
    };
    const char *cap[] = {
        "--max-physical-work-bytes", "1",
        "check-physical", files.reference, files.model,
        "--output", files.second
    };
    const char *alias[] = {
        "--replace", "check-physical", files.reference, files.model,
        "--output", files.alias
    };
#if !defined(_WIN32)
    const char *broken[] = {
        "--replace", "check-physical", files.reference, files.model,
        "--output", files.second
    };
#endif
    const char *named[] = {
        "--bind", binding, "check-physical", files.reference, files.model,
        "--output", files.second
    };
    const char *bad_binding[] = {
        "--bind", "reference:probe:bad=-",
        "check-physical", files.reference, files.model,
        "--output", files.second
    };
    const char *binding_alias[] = {
        "--replace", "--bind", binding,
        "check-physical", files.reference, files.model,
        "--output", files.alias
    };
    const char *identical[] = {
        "check-physical", files.reference, files.reference,
        "--output", files.second
    };
    CHECK(test_files_open(&files), "cannot make Stage 5 CLI workspace");
    if (failures != 0) return;
    memset(&reference, 0, sizeof(reference));
    memset(&model, 0, sizeof(model));
    if (!test_make_profile(&reference, -20.0, -12.0) ||
        !test_make_profile(&model, -18.0, -8.0) ||
        !test_write_profile(files.reference, &reference) ||
        !test_write_profile(files.model, &model) ||
        !test_write_wav(files.wave)) {
        CHECK(0, "cannot make Stage 5 CLI inputs");
        hwa_measurement_set_free(&reference);
        hwa_measurement_set_free(&model);
        test_files_close(&files);
        return;
    }
    hwa_measurement_set_free(&reference);
    hwa_measurement_set_free(&model);
    reference_before = test_read(files.reference, &reference_size);
    model_before = test_read(files.model, &model_size);
    wave_before = test_read(files.wave, &wave_size);
    CHECK(reference_before != NULL && model_before != NULL &&
              wave_before != NULL, "cannot snapshot Stage 5 inputs");
    CHECK(test_run(&files, basic, sizeof(basic) / sizeof(basic[0])) == 0,
          "basic physical check failed");
    CHECK(test_contains(files.output, "HWA_PHYSICAL,1") &&
              test_contains(files.stdout_path, "Physical checks") &&
              test_contains(files.stdout_path, "unavailable"),
          "named physical output or missing-evidence text is incomplete");
    CHECK(test_unchanged(files.reference, reference_before, reference_size) &&
              test_unchanged(files.model, model_before, model_size),
          "physical check changed a profile input");
    CHECK(test_run(&files, exclusive,
                   sizeof(exclusive) / sizeof(exclusive[0])) != 0,
          "exclusive physical output replaced an existing file");
    CHECK(test_run(&files, replace,
                   sizeof(replace) / sizeof(replace[0])) == 0,
          "explicit physical output replacement failed");
    CHECK(test_run(&files, json, sizeof(json) / sizeof(json[0])) == 0 &&
              test_contains(files.stdout_path, "\"schema_version\":7") &&
              test_contains(files.stdout_path, "\"checks\":[") &&
              test_contains(files.second, "HWA_PHYSICAL,1"),
          "schema 7 named physical report failed");
    (void)test_unlink(files.second);
    CHECK(test_run(&files, canonical_stdout,
                   sizeof(canonical_stdout) /
                       sizeof(canonical_stdout[0])) == 0 &&
              test_contains(files.stdout_path, "HWA_PHYSICAL,1") &&
              !test_contains(files.stdout_path, "\r\r\n"),
          "canonical physical stdout failed or expanded CRLF twice");
    CHECK(test_run(&files, json_stdout,
                   sizeof(json_stdout) / sizeof(json_stdout[0])) != 0,
          "JSON and canonical standard output conflict was accepted");
    CHECK(test_run(&files, cap, sizeof(cap) / sizeof(cap[0])) != 0 &&
              !test_exists(files.second),
          "physical work cap did not stop publication");
    CHECK(test_link(files.reference, files.alias),
          "cannot make profile output hard link");
    CHECK(test_run(&files, alias, sizeof(alias) / sizeof(alias[0])) != 0 &&
              test_unchanged(files.reference, reference_before,
                             reference_size),
          "physical output wrote through a profile hard link");
    (void)test_unlink(files.alias);
    (void)snprintf(binding, sizeof(binding),
                   "reference:scan:scale=%s", files.wave);
    CHECK(test_run(&files, named, sizeof(named) / sizeof(named[0])) == 0 &&
              test_contains(files.second, "reference:scan:scale"),
          "explicit raw binding failed");
    CHECK(test_unchanged(files.wave, wave_before, wave_size),
          "physical binding changed its WAVE input");
    (void)test_unlink(files.second);
    CHECK(test_link(files.wave, files.alias),
          "cannot make WAVE output hard link");
    CHECK(test_run(&files, binding_alias,
                   sizeof(binding_alias) / sizeof(binding_alias[0])) != 0 &&
              test_unchanged(files.wave, wave_before, wave_size),
          "physical output wrote through a WAVE hard link");
    (void)test_unlink(files.alias);
    CHECK(test_run(&files, identical,
                   sizeof(identical) / sizeof(identical[0])) != 0 &&
              !test_exists(files.second),
          "byte-identical profile pair was accepted");
    CHECK(test_run(&files, bad_binding,
                   sizeof(bad_binding) / sizeof(bad_binding[0])) != 0 &&
              !test_exists(files.second),
          "bad or standard-input binding was accepted");
#if !defined(_WIN32)
    CHECK(test_write_text(files.second, "old sentinel"),
          "cannot make replacement sentinel");
    sentinel_before = test_read(files.second, &sentinel_size);
    CHECK(sentinel_before != NULL, "cannot snapshot replacement sentinel");
    CHECK(test_broken_stdout(
              &files, broken, sizeof(broken) / sizeof(broken[0])) == 1 &&
              sentinel_before != NULL &&
              test_unchanged(files.second, sentinel_before, sentinel_size),
          "broken summary output changed an old replacement");
    free(sentinel_before);
#endif
    free(reference_before);
    free(model_before);
    free(wave_before);
    test_files_close(&files);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        (void)fputs("usage: stage5_cli_tests ANALYZER\n", stderr);
        return 2;
    }
    analyzer_path = argv[1];
    test_cli();
    if (failures != 0) {
        (void)fprintf(stderr, "%d Stage 5 CLI test(s) failed\n", failures);
        return 1;
    }
    (void)puts("Stage 5 CLI tests passed");
    return 0;
}

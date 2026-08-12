#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "hlolli_wg_analyzer.h"
#include "measure_compare.h"
#include "measure_file.h"
#include "production.h"
#include "production_file.h"
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
    char reference_profile[PATH_MAX];
    char reference_audio[PATH_MAX];
    char model_profile[PATH_MAX];
    char model_audio[PATH_MAX];
    char room_ir[PATH_MAX];
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
                               "%s/hwa-stage6-cli-%ld-%u",
                               root, test_pid(), attempt);
        if (written < 0 ||
            (size_t)written >= sizeof(files->directory)) return 0;
        if (test_mkdir(files->directory) == 0) break;
        if (errno != EEXIST) return 0;
    }
    return attempt < 100U &&
           test_join(files->reference_profile, files->directory,
                     "reference.hwa-measures") &&
           test_join(files->reference_audio, files->directory,
                     "reference.wav") &&
           test_join(files->model_profile, files->directory,
                     "model.hwa-measures") &&
           test_join(files->model_audio, files->directory, "model.wav") &&
           test_join(files->room_ir, files->directory, "room.wav") &&
           test_join(files->output, files->directory,
                     "result.hwa-production") &&
           test_join(files->second, files->directory,
                     "second.hwa-production") &&
           test_join(files->alias, files->directory,
                     "alias.hwa-production") &&
           test_join(files->normalized_a, files->directory,
                     "normalized-a.hwa-production") &&
           test_join(files->normalized_b, files->directory,
                     "normalized-b.hwa-production") &&
           test_join(files->stdout_path, files->directory, "stdout.txt") &&
           test_join(files->stderr_path, files->directory, "stderr.txt");
}

static void test_files_close(TestFiles *files)
{
    const char *paths[] = {
        files->reference_profile, files->reference_audio,
        files->model_profile, files->model_audio, files->room_ir,
        files->output, files->second, files->alias,
        files->normalized_a, files->normalized_b,
        files->stdout_path, files->stderr_path
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

static int test_key_for_split(HWAProductionSplit wanted,
                              char key[32])
{
    unsigned candidate;
    for (candidate = 0U; candidate < 10000U; ++candidate) {
        HWAProductionSplit actual;
        int written = snprintf(key, 32U, "tail:%04u", candidate);
        if (written < 0 || written >= 32) return 0;
        if (hwa_production_split_for_item_key(key, &actual) == 0 &&
            actual == wanted) return 1;
    }
    return 0;
}

static int test_make_profile(HWAMeasurementSet *set,
                             const char *stored_audio_path,
                             const char audio_hash[HWA_SHA256_HEX_SIZE],
                             double first,
                             double second)
{
    char keys[2][32];
    double values[2];
    size_t item;
    char error[HWA_ERROR_SIZE] = {0};
    memset(set, 0, sizeof(*set));
    if (!test_key_for_split(HWA_PRODUCTION_TRAIN, keys[0]) ||
        !test_key_for_split(HWA_PRODUCTION_CHECK, keys[1])) return 0;
    hwa_measurement_options_default(&set->options);
    set->options.fft_size = 1024U;
    set->options.hop_size = 128U;
    set->options.max_partials = 4U;
    set->options.max_work_bytes = UINT64_C(67108864);
    set->items_path = test_profile_copy(set, "/not-opened/items.hwa-items");
    set->audio_path = test_profile_copy(set, stored_audio_path);
    set->alignment_path = test_profile_copy(
        set, "/not-opened/take.hwa-align");
    set->source_score_path = test_profile_copy(
        set, "/not-opened/score.csv");
    test_hash(set->items_sha256, 'a');
    memcpy(set->audio_sha256, audio_hash, HWA_SHA256_HEX_SIZE);
    test_hash(set->alignment_sha256, 'c');
    test_hash(set->source_score_sha256, 'd');
    set->audio_format.container = HWA_CONTAINER_RIFF;
    set->audio_format.encoding = HWA_ENCODING_PCM;
    set->audio_format.channels = 1U;
    set->audio_format.sample_rate_hz = TEST_RATE;
    set->audio_format.bits_per_sample = 16U;
    set->audio_format.valid_bits_per_sample = 16U;
    set->audio_format.block_align = 2U;
    set->audio_format.frames = TEST_FRAMES;
    set->audio_format.data_bytes = TEST_FRAMES * 2U;
    set->audio_format.duration_seconds = 1.0;
    set->level_reference_dbfs = 0.0;
    set->level_reference_item_count = 0U;
    set->level_reference_valid = 0;
    set->item_frame_evaluations = 4U;
    set->transform_count = 2U;
    set->context_count = 2U;
    set->measurement_count = 4U;
    set->contexts = (HWAMeasureItemContext *)calloc(
        set->context_count, sizeof(*set->contexts));
    set->measurements = (HWAMeasureObservation *)calloc(
        set->measurement_count, sizeof(*set->measurements));
    if (set->contexts == NULL || set->measurements == NULL ||
        set->items_path == NULL || set->audio_path == NULL ||
        set->alignment_path == NULL || set->source_score_path == NULL) {
        hwa_measurement_set_free(set);
        return 0;
    }
    set->retained_work_bytes +=
        (uint64_t)set->context_count * sizeof(*set->contexts) +
        (uint64_t)set->measurement_count * sizeof(*set->measurements);
    values[0] = first;
    values[1] = second;
    for (item = 0U; item < set->context_count; ++item) {
        HWAMeasureItemContext *context = &set->contexts[item];
        HWAMeasureObservation *raw = &set->measurements[item * 2U];
        HWAMeasureObservation *relative = raw + 1;
        context->item_id = (uint64_t)item + 1U;
        context->item_key = test_profile_copy(set, keys[item]);
        context->item_kind = HWA_ITEM_RELEASE;
        context->item_role = test_profile_copy(set, "tail");
        context->start_sample = (uint64_t)item * 800U;
        context->end_sample = context->start_sample + 800U;
        context->source_event_count = 1U;
        context->item_confidence = 0.9;
        raw->id = (uint64_t)item * 2U + 1U;
        raw->item_id = context->item_id;
        raw->kind = HWA_MEASURE_RMS_DBFS;
        raw->unit = HWA_MEASURE_UNIT_DBFS;
        raw->view = HWA_MEASURE_VIEW_RAW;
        raw->status = HWA_MEASURE_STATUS_VALID;
        raw->value = values[item];
        raw->confidence = 0.9;
        relative->id = raw->id + 1U;
        relative->item_id = raw->item_id;
        relative->kind = raw->kind;
        relative->unit = HWA_MEASURE_UNIT_DB;
        relative->view = HWA_MEASURE_VIEW_LEVEL_RELATIVE;
        relative->status = HWA_MEASURE_STATUS_NO_REFERENCE;
        relative->value = 0.0;
        relative->confidence = raw->confidence;
        relative->evidence_flags = 0U;
        if (context->item_key == NULL || context->item_role == NULL) {
            hwa_measurement_set_free(set);
            return 0;
        }
    }
    if (hwa_measure_build_profile(set, error, sizeof(error)) != 0) {
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

static int test_write_wav(const char *path, double amplitude)
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
        double sample = amplitude * sin(2.0 * TEST_PI * 220.0 * time);
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
    if (!equal && left_bytes != NULL && right_bytes != NULL) {
        size_t limit = left_size < right_size ? left_size : right_size;
        size_t offset = 0U;
        while (offset < limit && left_bytes[offset] == right_bytes[offset]) {
            offset++;
        }
        (void)fprintf(stderr,
                      "normalized outputs first differ at byte %zu "
                      "(sizes %zu/%zu)\n",
                      offset, left_size, right_size);
        if (offset < limit) {
            size_t left_start = offset;
            size_t left_end = offset;
            size_t right_start = offset;
            size_t right_end = offset;
            while (left_start != 0U &&
                   left_bytes[left_start - 1U] != (unsigned char)'\n') {
                left_start--;
            }
            while (left_end < left_size &&
                   left_bytes[left_end] != (unsigned char)'\n') left_end++;
            while (right_start != 0U &&
                   right_bytes[right_start - 1U] != (unsigned char)'\n') {
                right_start--;
            }
            while (right_end < right_size &&
                   right_bytes[right_end] != (unsigned char)'\n') right_end++;
            (void)fprintf(stderr, "left: %.*s\nright: %.*s\n",
                          (int)(left_end - left_start),
                          (const char *)left_bytes + left_start,
                          (int)(right_end - right_start),
                          (const char *)right_bytes + right_start);
        }
    }
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
    char command[PATH_MAX * 12U];
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

static int test_normalize_production(const char *input,
                                     const char *output)
{
    HWAProductionOptions limits;
    HWAProductionResult result;
    char sha[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE] = {0};
    FILE *stream;
    int status = -1;
    memset(&result, 0, sizeof(result));
    hwa_production_options_default(&limits);
    if (hwa_production_file_read(
            input, &limits, &result, sha, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "production normalize read: %s\n", error);
        return 0;
    }
    result.options.decode_block_frames = limits.decode_block_frames;
    stream = fopen(output, "wb");
    if (stream != NULL) {
        status = hwa_production_file_write(
            stream, &result, error, sizeof(error));
        if (fclose(stream) != 0) status = -1;
    }
    if (status != 0) {
        (void)fprintf(stderr, "production normalize write: %s\n", error);
    }
    hwa_production_result_free(&result);
    return status == 0;
}

static void test_parse_and_failure_safety(void)
{
    TestFiles files;
    unsigned char *reference_before;
    unsigned char *reference_audio_before;
    unsigned char *model_before;
    unsigned char *model_audio_before;
    unsigned char *output_before;
    size_t reference_size;
    size_t reference_audio_size;
    size_t model_size;
    size_t model_audio_size;
    size_t output_size;
    const char *basic[] = {
        "account-production", files.reference_profile, files.reference_audio,
        files.model_profile, files.model_audio,
        "--output", files.output
    };
    const char *replace_failure[] = {
        "--replace", "account-production",
        files.reference_profile, files.reference_audio,
        files.model_profile, files.model_audio,
        "--output", files.output
    };
    const char *json_stdout[] = {
        "--json", "account-production",
        files.reference_profile, files.reference_audio,
        files.model_profile, files.model_audio, "--output", "-"
    };
    const char *replace_stdout[] = {
        "--replace", "account-production",
        files.reference_profile, files.reference_audio,
        files.model_profile, files.model_audio, "--output", "-"
    };
    const char *stdin_profile[] = {
        "account-production", "-", files.reference_audio,
        files.model_profile, files.model_audio,
        "--output", files.output
    };
    const char *stdin_room[] = {
        "--room-ir", "-", "account-production",
        files.reference_profile, files.reference_audio,
        files.model_profile, files.model_audio,
        "--output", files.output
    };
    const char *removed_option[] = {
        "--production-fft-size", "1024", "account-production",
        files.reference_profile, files.reference_audio,
        files.model_profile, files.model_audio,
        "--output", files.output
    };
    const char *zero_limit[] = {
        "--max-production-spans", "0", "account-production",
        files.reference_profile, files.reference_audio,
        files.model_profile, files.model_audio,
        "--output", files.output
    };

    CHECK(test_files_open(&files), "cannot make Stage 6 CLI workspace");
    if (failures != 0) return;
    CHECK(test_write_text(files.reference_profile, "bad reference profile") &&
              test_write_text(files.reference_audio, "bad reference WAVE") &&
              test_write_text(files.model_profile, "bad model profile") &&
              test_write_text(files.model_audio, "bad model WAVE"),
          "cannot make Stage 6 failure inputs");
    reference_before = test_read(files.reference_profile, &reference_size);
    reference_audio_before = test_read(
        files.reference_audio, &reference_audio_size);
    model_before = test_read(files.model_profile, &model_size);
    model_audio_before = test_read(files.model_audio, &model_audio_size);
    CHECK(reference_before != NULL && reference_audio_before != NULL &&
              model_before != NULL && model_audio_before != NULL,
          "cannot snapshot Stage 6 failure inputs");

    CHECK(test_run(&files, basic, sizeof(basic) / sizeof(basic[0])) != 0 &&
              !test_exists(files.output),
          "failed Stage 6 account published an output");
    CHECK(test_write_text(files.output, "old production output"),
          "cannot make Stage 6 replacement sentinel");
    output_before = test_read(files.output, &output_size);
    CHECK(output_before != NULL &&
              test_run(&files, replace_failure,
                       sizeof(replace_failure) /
                           sizeof(replace_failure[0])) != 0 &&
              test_unchanged(files.output, output_before, output_size),
          "failed Stage 6 replacement changed the old output");
    CHECK(test_run(&files, json_stdout,
                   sizeof(json_stdout) / sizeof(json_stdout[0])) != 0,
          "Stage 6 accepted JSON with canonical standard output");
    CHECK(test_run(&files, replace_stdout,
                   sizeof(replace_stdout) / sizeof(replace_stdout[0])) != 0,
          "Stage 6 accepted replace with canonical standard output");
    CHECK(test_run(&files, stdin_profile,
                   sizeof(stdin_profile) / sizeof(stdin_profile[0])) != 0,
          "Stage 6 accepted a standard-input profile");
    CHECK(test_run(&files, stdin_room,
                   sizeof(stdin_room) / sizeof(stdin_room[0])) != 0,
          "Stage 6 accepted a standard-input room IR");
    CHECK(test_run(&files, removed_option,
                   sizeof(removed_option) / sizeof(removed_option[0])) != 0,
          "Stage 6 accepted a removed FFT option");
    CHECK(test_run(&files, zero_limit,
                   sizeof(zero_limit) / sizeof(zero_limit[0])) != 0,
          "Stage 6 accepted a zero row cap");
    CHECK(test_unchanged(files.reference_profile,
                         reference_before, reference_size) &&
              test_unchanged(files.reference_audio,
                             reference_audio_before, reference_audio_size) &&
              test_unchanged(files.model_profile,
                             model_before, model_size) &&
              test_unchanged(files.model_audio,
                             model_audio_before, model_audio_size),
          "Stage 6 changed a failed-account input");

    free(output_before);
    free(reference_before);
    free(reference_audio_before);
    free(model_before);
    free(model_audio_before);
    test_files_close(&files);
}

static void test_success_and_publication(void)
{
    TestFiles files;
    HWAMeasurementSet reference;
    HWAMeasurementSet model;
    char reference_hash[HWA_SHA256_HEX_SIZE];
    char model_hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE] = {0};
    unsigned char *reference_before = NULL;
    unsigned char *reference_audio_before = NULL;
    unsigned char *model_before = NULL;
    unsigned char *model_audio_before = NULL;
    unsigned char *sentinel_before = NULL;
    size_t reference_size = 0U;
    size_t reference_audio_size = 0U;
    size_t model_size = 0U;
    size_t model_audio_size = 0U;
    const char *basic[] = {
        "account-production", files.reference_profile, files.reference_audio,
        files.model_profile, files.model_audio,
        "--output", files.output
    };
    const char *exclusive[] = {
        "account-production", files.reference_profile, files.reference_audio,
        files.model_profile, files.model_audio,
        "--output", files.output
    };
    const char *replace[] = {
        "--replace", "account-production",
        files.reference_profile, files.reference_audio,
        files.model_profile, files.model_audio,
        "--output", files.output
    };
    const char *json[] = {
        "--json", "account-production",
        files.reference_profile, files.reference_audio,
        files.model_profile, files.model_audio,
        "--output", files.second
    };
    const char *canonical_stdout[] = {
        "account-production", files.reference_profile, files.reference_audio,
        files.model_profile, files.model_audio, "--output", "-"
    };
    const char *exact_cap[] = {
        "--max-production-spans", "2", "account-production",
        files.reference_profile, files.reference_audio,
        files.model_profile, files.model_audio,
        "--output", files.second
    };
    const char *under_cap[] = {
        "--max-production-spans", "1", "account-production",
        files.reference_profile, files.reference_audio,
        files.model_profile, files.model_audio,
        "--output", files.second
    };
    const char *block_one[] = {
        "--replace", "--block-frames", "1", "account-production",
        files.reference_profile, files.reference_audio,
        files.model_profile, files.model_audio,
        "--output", files.output
    };
    const char *block_many[] = {
        "--block-frames", "4096", "account-production",
        files.reference_profile, files.reference_audio,
        files.model_profile, files.model_audio,
        "--output", files.second
    };
    const char *alias_output[] = {
        "--replace", "account-production",
        files.reference_profile, files.reference_audio,
        files.model_profile, files.model_audio,
        "--output", files.alias
    };
#if !defined(_WIN32)
    size_t sentinel_size = 0U;
    const char *broken[] = {
        "--replace", "account-production",
        files.reference_profile, files.reference_audio,
        files.model_profile, files.model_audio,
        "--output", files.second
    };
#endif

    CHECK(test_files_open(&files), "cannot make Stage 6 success workspace");
    if (failures != 0) return;
    memset(&reference, 0, sizeof(reference));
    memset(&model, 0, sizeof(model));
    if (!test_write_wav(files.reference_audio, 0.10) ||
        !test_write_wav(files.model_audio, 0.07) ||
        hwa_sha256_file(files.reference_audio, UINT64_C(1048576),
                        reference_hash, error, sizeof(error)) != 0 ||
        hwa_sha256_file(files.model_audio, UINT64_C(1048576),
                        model_hash, error, sizeof(error)) != 0 ||
        !test_make_profile(&reference, "/not-opened/reference.wav",
                           reference_hash, -20.0, -22.0) ||
        !test_make_profile(&model, "/not-opened/model.wav",
                           model_hash, -23.0, -25.0) ||
        !test_write_profile(files.reference_profile, &reference) ||
        !test_write_profile(files.model_profile, &model)) {
        CHECK(0, "cannot make Stage 6 success inputs: %s", error);
        hwa_measurement_set_free(&reference);
        hwa_measurement_set_free(&model);
        test_files_close(&files);
        return;
    }
    hwa_measurement_set_free(&reference);
    hwa_measurement_set_free(&model);
    reference_before = test_read(files.reference_profile, &reference_size);
    reference_audio_before = test_read(
        files.reference_audio, &reference_audio_size);
    model_before = test_read(files.model_profile, &model_size);
    model_audio_before = test_read(files.model_audio, &model_audio_size);
    CHECK(reference_before != NULL && reference_audio_before != NULL &&
              model_before != NULL && model_audio_before != NULL,
          "cannot snapshot Stage 6 inputs");

    CHECK(test_run(&files, basic, sizeof(basic) / sizeof(basic[0])) == 0 &&
              test_contains(files.output, "HWA_PRODUCTION,1") &&
              test_contains(files.stdout_path, "Production account") &&
              test_contains(files.stdout_path, "Source profile method"),
          "named Stage 6 text output failed");
    CHECK(test_run(&files, exclusive,
                   sizeof(exclusive) / sizeof(exclusive[0])) != 0,
          "Stage 6 replaced an output without --replace");
    CHECK(test_run(&files, replace,
                   sizeof(replace) / sizeof(replace[0])) == 0,
          "Stage 6 explicit replacement failed");
    CHECK(test_run(&files, json, sizeof(json) / sizeof(json[0])) == 0 &&
              test_contains(files.second, "HWA_PRODUCTION,1") &&
              test_contains(files.stdout_path, "\"schema_version\":8") &&
              test_contains(files.stdout_path, "\"evaluations\":[") &&
              test_contains(files.stdout_path, "\"views\":["),
          "Stage 6 schema 8 named report failed");
    (void)test_unlink(files.second);
    CHECK(test_run(&files, canonical_stdout,
                   sizeof(canonical_stdout) /
                       sizeof(canonical_stdout[0])) == 0 &&
              test_contains(files.stdout_path, "HWA_PRODUCTION,1") &&
              !test_contains(files.stdout_path, "\r\r\n"),
          "canonical Stage 6 standard output failed");
    CHECK(test_run(&files, exact_cap,
                   sizeof(exact_cap) / sizeof(exact_cap[0])) == 0,
          "exact Stage 6 span cap failed");
    (void)test_unlink(files.second);
    CHECK(test_run(&files, under_cap,
                   sizeof(under_cap) / sizeof(under_cap[0])) != 0 &&
              !test_exists(files.second),
          "one-under Stage 6 span cap published an output");

    CHECK(test_run(&files, block_one,
                   sizeof(block_one) / sizeof(block_one[0])) == 0 &&
              test_run(&files, block_many,
                       sizeof(block_many) / sizeof(block_many[0])) == 0 &&
              test_normalize_production(files.output, files.normalized_a) &&
              test_normalize_production(files.second, files.normalized_b) &&
              test_file_equal(files.normalized_a, files.normalized_b),
          "Stage 6 result changed with decode block size");
    (void)test_unlink(files.second);
    (void)test_unlink(files.alias);
    CHECK(test_link(files.reference_profile, files.alias),
          "cannot make Stage 6 hard-link alias");
    CHECK(test_run(&files, alias_output,
                   sizeof(alias_output) / sizeof(alias_output[0])) != 0 &&
              test_unchanged(files.reference_profile,
                             reference_before, reference_size),
          "Stage 6 wrote through an input hard link");
    (void)test_unlink(files.alias);
#if !defined(_WIN32)
    CHECK(test_symlink(files.reference_profile, files.alias),
          "cannot make Stage 6 symlink alias");
    CHECK(test_run(&files, alias_output,
                   sizeof(alias_output) / sizeof(alias_output[0])) != 0 &&
              test_unchanged(files.reference_profile,
                             reference_before, reference_size),
          "Stage 6 wrote through an input symlink");
    (void)test_unlink(files.alias);
    CHECK(test_write_text(files.second, "old production output"),
          "cannot make Stage 6 broken-output sentinel");
    sentinel_before = test_read(files.second, &sentinel_size);
    CHECK(sentinel_before != NULL &&
              test_broken_stdout(
                  &files, broken, sizeof(broken) / sizeof(broken[0])) == 1 &&
              test_unchanged(files.second,
                             sentinel_before, sentinel_size),
          "broken Stage 6 report output changed an old replacement");
#endif
    CHECK(test_unchanged(files.reference_profile,
                         reference_before, reference_size) &&
              test_unchanged(files.reference_audio,
                             reference_audio_before, reference_audio_size) &&
              test_unchanged(files.model_profile,
                             model_before, model_size) &&
              test_unchanged(files.model_audio,
                             model_audio_before, model_audio_size),
          "Stage 6 changed a successful-account input");

    free(sentinel_before);
    free(reference_before);
    free(reference_audio_before);
    free(model_before);
    free(model_audio_before);
    test_files_close(&files);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        (void)fputs("usage: stage6_cli_tests ANALYZER\n", stderr);
        return 2;
    }
    analyzer_path = argv[1];
    test_parse_and_failure_safety();
    test_success_and_publication();
    if (failures != 0) {
        (void)fprintf(stderr, "%d Stage 6 CLI test(s) failed\n", failures);
        return 1;
    }
    (void)puts("Stage 6 CLI tests passed");
    return 0;
}

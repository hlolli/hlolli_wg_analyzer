#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "experiment_file.h"
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
#include <windows.h>
#include "windows_test_process.h"
#else
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct TestFiles {
    char directory[PATH_MAX];
    char manifest[PATH_MAX];
    char reference[PATH_MAX];
    char output[PATH_MAX];
    char second[PATH_MAX];
    char third[PATH_MAX];
    char fourth[PATH_MAX];
    char fifth[PATH_MAX];
    char stdout_path[PATH_MAX];
    char stderr_path[PATH_MAX];
    char renderer_link[PATH_MAX];
} TestFiles;

static const char *analyzer_path;
static const char *renderer_ok;
static const char *renderer_fail;
static const char *renderer_slow;
static const char *renderer_large;
static const char *renderer_extra;
static const char *renderer_worker;
static const char *renderer_quick;
static const char *renderer_churn;
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
                               "%s/hwa-stage8-cli-%ld-%u",
                               root, test_pid(), attempt);
        if (written < 0 ||
            (size_t)written >= sizeof(files->directory)) return 0;
        if (test_mkdir(files->directory) == 0) break;
        if (errno != EEXIST) return 0;
    }
    return attempt < 100U &&
           test_join(files->manifest, files->directory, "experiment.json") &&
           test_join(files->reference, files->directory, "reference.wav") &&
           test_join(files->output, files->directory, "result.hwa-bundle") &&
           test_join(files->second, files->directory, "second.hwa-bundle") &&
           test_join(files->third, files->directory, "third.hwa-bundle") &&
           test_join(files->fourth, files->directory, "fourth.hwa-bundle") &&
           test_join(files->fifth, files->directory, "fifth.hwa-bundle") &&
           test_join(files->stdout_path, files->directory, "stdout.txt") &&
           test_join(files->stderr_path, files->directory, "stderr.txt") &&
           test_join(files->renderer_link, files->directory,
                     "renderer-link");
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
                (void)FindClose(search);
                return -1;
            }
        } while (FindNextFileA(search, &entry) != 0);
        (void)FindClose(search);
    }
    return RemoveDirectoryA(path) != 0 ? 0 : -1;
#else
    struct stat status;
    DIR *directory;
    struct dirent *entry;
    if (lstat(path, &status) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(status.st_mode)) return unlink(path);
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

static int test_tree_has_name(const char *path, const char *name)
{
#if defined(_WIN32)
    WIN32_FIND_DATAA entry;
    char pattern[PATH_MAX];
    HANDLE search;
    if (snprintf(pattern, sizeof(pattern), "%s/*", path) < 0) return 1;
    search = FindFirstFileA(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) return 0;
    do {
        char child[PATH_MAX];
        if (strcmp(entry.cFileName, ".") == 0 ||
            strcmp(entry.cFileName, "..") == 0) continue;
        if (strcmp(entry.cFileName, name) == 0 ||
            !test_join(child, path, entry.cFileName) ||
            ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
             test_tree_has_name(child, name))) {
            (void)FindClose(search);
            return 1;
        }
    } while (FindNextFileA(search, &entry) != 0);
    (void)FindClose(search);
    return 0;
#else
    DIR *directory = opendir(path);
    struct dirent *entry;
    if (directory == NULL) return 0;
    while ((entry = readdir(directory)) != NULL) {
        char child[PATH_MAX];
        struct stat status;
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;
        if (strcmp(entry->d_name, name) == 0 ||
            !test_join(child, path, entry->d_name) ||
            (lstat(child, &status) == 0 && S_ISDIR(status.st_mode) &&
             test_tree_has_name(child, name))) {
            (void)closedir(directory);
            return 1;
        }
    }
    (void)closedir(directory);
    return 0;
#endif
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

static int test_write_wave(const char *path)
{
    const uint32_t frames = UINT32_C(4096);
    const uint32_t data_bytes = frames * UINT32_C(2);
    uint32_t state = UINT32_C(0x12345678);
    FILE *stream = fopen(path, "wb");
    uint32_t index;
    if (stream == NULL) return 0;
    if (fwrite("RIFF", 1U, 4U, stream) != 4U) goto failed;
    test_u32(stream, UINT32_C(36) + data_bytes);
    if (fwrite("WAVEfmt ", 1U, 8U, stream) != 8U) goto failed;
    test_u32(stream, UINT32_C(16));
    test_u16(stream, UINT16_C(1));
    test_u16(stream, UINT16_C(1));
    test_u32(stream, UINT32_C(48000));
    test_u32(stream, UINT32_C(96000));
    test_u16(stream, UINT16_C(2));
    test_u16(stream, UINT16_C(16));
    if (fwrite("data", 1U, 4U, stream) != 4U) goto failed;
    test_u32(stream, data_bytes);
    for (index = 0U; index < frames; ++index) {
        int16_t sample;
        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
        sample = (int16_t)((state >> 17U) & UINT32_C(0x7fff));
        sample = (int16_t)(sample - INT16_C(16384));
        test_u16(stream, (uint16_t)sample);
    }
    if (fflush(stream) != 0 || ferror(stream) || fclose(stream) != 0) return 0;
    return 1;
failed:
    (void)fclose(stream);
    return 0;
}

static int test_write_manifest(const TestFiles *files)
{
    char hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE] = {0};
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
        "\"parameters\":[{\"id\":\"gain\",\"unit\":\"ratio\","
        "\"minimum\":0,\"maximum\":1,\"baseline\":0,"
        "\"levels\":[0,1]}],"
        "\"plan\":{\"kind\":\"one-at-a-time\",\"seed\":17,"
        "\"sample_count\":0,\"replicates\":1},"
        "\"cases\":["
        "{\"id\":\"check-case\",\"split\":\"check\",\"weight\":1,"
        "\"stems\":["
        "{\"id\":\"model.final\",\"side\":\"model\","
        "\"role\":\"final\",\"input_id\":null,\"output\":\"model.wav\","
        "\"start_sample\":0,\"gain_db\":0,\"rate_hz\":48000,"
        "\"channels\":1},"
        "{\"id\":\"reference.final\",\"side\":\"reference\","
        "\"role\":\"final\",\"input_id\":\"artist\",\"output\":null,"
        "\"start_sample\":0,\"gain_db\":0,\"rate_hz\":48000,"
        "\"channels\":1}],\"probes\":[],\"links\":[]},"
        "{\"id\":\"fit-case\",\"split\":\"fit\",\"weight\":1,"
        "\"stems\":["
        "{\"id\":\"model.final\",\"side\":\"model\","
        "\"role\":\"final\",\"input_id\":null,\"output\":\"model.wav\","
        "\"start_sample\":0,\"gain_db\":0,\"rate_hz\":48000,"
        "\"channels\":1},"
        "{\"id\":\"reference.final\",\"side\":\"reference\","
        "\"role\":\"final\",\"input_id\":\"artist\",\"output\":null,"
        "\"start_sample\":0,\"gain_db\":0,\"rate_hz\":48000,"
        "\"channels\":1}],\"probes\":[],\"links\":[]}],"
        "\"responses\":[{\"id\":\"final.rms\",\"role\":\"final\","
        "\"feature\":\"rms_dbfs\",\"index\":0}]}\n",
        hash) > 0;
    if (fflush(stream) != 0 || ferror(stream) || fclose(stream) != 0) okay = 0;
    return okay;
}

static int test_exists(const char *path)
{
#if defined(_WIN32)
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat status;
    return lstat(path, &status) == 0;
#endif
}

static unsigned char *test_read(const char *path, size_t *size)
{
    FILE *stream = fopen(path, "rb");
    long length;
    unsigned char *bytes;
    if (stream == NULL || fseek(stream, 0L, SEEK_END) != 0 ||
        (length = ftell(stream)) < 0L || fseek(stream, 0L, SEEK_SET) != 0) {
        if (stream != NULL) (void)fclose(stream);
        return NULL;
    }
    bytes = (unsigned char *)malloc((size_t)length + 1U);
    if (bytes == NULL || fread(bytes, 1U, (size_t)length, stream) !=
                             (size_t)length || fclose(stream) != 0) {
        free(bytes);
        return NULL;
    }
    bytes[(size_t)length] = 0U;
    *size = (size_t)length;
    return bytes;
}

static int test_contains(const char *path, const char *needle)
{
    size_t size = 0U;
    unsigned char *bytes = test_read(path, &size);
    int found = bytes != NULL && strstr((const char *)bytes, needle) != NULL;
    (void)size;
    free(bytes);
    return found;
}

static int test_same_file(const char *left_path, const char *right_path)
{
    size_t left_size = 0U;
    size_t right_size = 0U;
    unsigned char *left = test_read(left_path, &left_size);
    unsigned char *right = test_read(right_path, &right_size);
    int same = left != NULL && right != NULL && left_size == right_size &&
               memcmp(left, right, left_size) == 0;
    free(left);
    free(right);
    return same;
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
        } else if (!test_append(command, capacity, length, one)) return 0;
        cursor++;
    }
    return test_append(command, capacity, length, "\"");
#else
    if (!test_append(command, capacity, length, "'")) return 0;
    while (*cursor != 0U) {
        char one[2] = {(char)*cursor, '\0'};
        if (*cursor == (unsigned char)'\'') {
            if (!test_append(command, capacity, length, "'\\''")) return 0;
        } else if (!test_append(command, capacity, length, one)) return 0;
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
    char command[PATH_MAX * 24U];
    size_t length = 0U;
    size_t index;
    int status;
    command[0] = '\0';
    if (!test_argument(command, sizeof(command), &length, analyzer_path))
        return -1;
    for (index = 0U; index < argument_count; ++index) {
        if (!test_append(command, sizeof(command), &length, " ") ||
            !test_argument(command, sizeof(command), &length,
                           arguments[index])) return -1;
    }
    if (!test_append(command, sizeof(command), &length, " >") ||
        !test_argument(command, sizeof(command), &length, files->stdout_path) ||
        !test_append(command, sizeof(command), &length, " 2>") ||
        !test_argument(command, sizeof(command), &length, files->stderr_path))
        return -1;
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
        if (error_file < 0 || dup2(error_file, STDERR_FILENO) < 0) _exit(126);
        (void)close(error_file);
        exec_arguments[0] = (char *)analyzer_path;
        for (index = 0U; index < argument_count; ++index)
            exec_arguments[index + 1U] = (char *)arguments[index];
        exec_arguments[argument_count + 1U] = NULL;
        (void)execv(analyzer_path, exec_arguments);
        _exit(127);
    }
    (void)close(output_pipe[1]);
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == child && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

static int test_result_path(char path[PATH_MAX], const char *directory)
{
    return test_join(path, directory, "result.hwa-experiment");
}

static void test_failures(const TestFiles *files, const char *binding)
{
    const char *no_consent[] = {
        "--renderer", renderer_ok, "--bind", binding, "--output",
        files->output, "experiment", files->manifest
    };
    const char *bad_renderer[] = {
        "--renderer", renderer_fail, "--allow-run", "--bind", binding,
        "--output", files->output, "experiment", files->manifest
    };
    const char *slow[] = {
        "--renderer", renderer_slow, "--allow-run", "--bind", binding,
        "--max-experiment-job-ms", "50", "--output", files->output,
        "experiment", files->manifest
    };
    const char *large[] = {
        "--renderer", renderer_large, "--allow-run", "--bind", binding,
        "--max-experiment-output-file-bytes", "10000", "--output",
        files->output, "experiment", files->manifest
    };
    const char *extra[] = {
        "--renderer", renderer_extra, "--allow-run", "--bind", binding,
        "--output", files->output, "experiment", files->manifest
    };
    const char *quick[] = {
        "--renderer", renderer_quick, "--allow-run", "--bind", binding,
        "--max-experiment-job-ms", "1", "--output", files->output,
        "experiment", files->manifest
    };
#if !defined(_WIN32)
    const char *linked_renderer[] = {
        "--renderer", files->renderer_link, "--allow-run", "--bind", binding,
        "--output", files->output, "experiment", files->manifest
    };
#endif
    CHECK(test_run(files, no_consent,
                   sizeof(no_consent) / sizeof(no_consent[0])) != 0 &&
              !test_exists(files->output),
          "process launch without --allow-run was not rejected");
    CHECK(test_run(files, bad_renderer,
                   sizeof(bad_renderer) / sizeof(bad_renderer[0])) != 0 &&
              !test_exists(files->output),
          "renderer failure published output");
    CHECK(test_contains(files->stderr_path, "status 23"),
          "renderer exit status was not reported");
    CHECK(test_run(files, slow, sizeof(slow) / sizeof(slow[0])) != 0 &&
              !test_exists(files->output),
          "renderer timeout published output");
    CHECK(test_run(files, large, sizeof(large) / sizeof(large[0])) != 0 &&
              !test_exists(files->output),
          "renderer output cap failure published output");
    CHECK(test_contains(files->stderr_path, "renderer stopped by signal") ||
              test_contains(files->stderr_path,
                            "renderer created a non-regular or oversized output") ||
              test_contains(files->stderr_path,
                            "renderer exceeded its output byte limit"),
          "process adapter did not enforce the per-file output cap");
    CHECK(test_run(files, extra, sizeof(extra) / sizeof(extra[0])) != 0 &&
              !test_exists(files->output),
          "undeclared renderer output was accepted");
    CHECK(test_run(files, quick, sizeof(quick) / sizeof(quick[0])) != 0 &&
              !test_exists(files->output),
          "renderer completion after the deadline was accepted");
#if !defined(_WIN32)
    CHECK(test_run(files, linked_renderer,
                   sizeof(linked_renderer) / sizeof(linked_renderer[0])) != 0 &&
              !test_exists(files->output),
          "renderer symlink was accepted");
#endif
}

static void test_success(TestFiles *files, const char *binding)
{
    char saved_result[PATH_MAX];
    char resumed_result[PATH_MAX];
    const char *json[] = {
        "--json", "--renderer", renderer_ok, "--allow-run", "--bind",
        binding, "--output", files->output, "experiment", files->manifest
    };
    const char *again[] = {
        "--renderer", renderer_ok, "--allow-run", "--bind", binding,
        "--output", files->output, "experiment", files->manifest
    };
    const char *resume[] = {
        "--renderer", renderer_ok, "--allow-run", "--bind", binding,
        "--resume-from", files->output, "--output", files->second,
        "experiment", files->manifest
    };
    CHECK(test_run(files, json, sizeof(json) / sizeof(json[0])) == 0,
          "successful Stage 8 CLI run failed");
    CHECK(test_result_path(saved_result, files->output) &&
              test_exists(saved_result) &&
              test_contains(files->stdout_path, "\"schema_version\":10") &&
              test_contains(files->stdout_path,
                            "\"command\":\"experiment\"") &&
              test_contains(saved_result, "HWA_EXPERIMENT,1\r\n"),
          "successful Stage 8 output or report is incomplete");
    CHECK(!test_tree_has_name(files->output, "request.json") &&
              !test_tree_has_name(files->output, "stdout.txt") &&
              !test_tree_has_name(files->output, "stderr.txt"),
          "successful Stage 8 output retained renderer scratch files");
    CHECK(test_run(files, resume, sizeof(resume) / sizeof(resume[0])) == 0 &&
              test_result_path(resumed_result, files->second) &&
              test_same_file(saved_result, resumed_result) &&
              !test_tree_has_name(files->second, "request.json") &&
              !test_tree_has_name(files->second, "stdout.txt") &&
              !test_tree_has_name(files->second, "stderr.txt"),
          "fully resumed Stage 8 result differs from the fresh result");
    CHECK(test_run(files, again, sizeof(again) / sizeof(again[0])) != 0 &&
              test_exists(saved_result),
          "existing Stage 8 output was replaced");
#if !defined(_WIN32)
    {
        char worker_result[PATH_MAX];
        struct timespec delay;
        const char *worker[] = {
            "--renderer", renderer_worker, "--allow-run", "--bind", binding,
            "--output", files->third, "experiment", files->manifest
        };
        CHECK(test_run(files, worker, sizeof(worker) / sizeof(worker[0])) == 0,
              "renderer worker cleanup run failed");
        delay.tv_sec = 0;
        delay.tv_nsec = 700000000L;
        (void)nanosleep(&delay, NULL);
        CHECK(test_result_path(worker_result, files->third) &&
                  test_exists(worker_result) &&
                  !test_tree_has_name(files->third, "late.bin"),
              "renderer worker survived the synchronous callback");
    }
#endif
#if !defined(_WIN32)
    {
        char churn_result[PATH_MAX];
        const char *churn[] = {
            "--renderer", renderer_churn, "--allow-run", "--bind", binding,
            "--output", files->fourth, "experiment", files->manifest
        };
        CHECK(test_run(files, churn, sizeof(churn) / sizeof(churn[0])) == 0 &&
                  test_result_path(churn_result, files->fourth) &&
                  test_exists(churn_result) &&
                  !test_tree_has_name(files->fourth, ".scratch-0"),
              "transient regular renderer scratch caused a false failure");
    }
#endif
#if !defined(_WIN32)
    {
        const char *broken[] = {
            "--renderer", renderer_ok, "--allow-run", "--bind", binding,
            "--output", files->fifth, "experiment", files->manifest
        };
        CHECK(test_broken_stdout(files, broken,
                                 sizeof(broken) / sizeof(broken[0])) != 0 &&
                  !test_exists(files->fifth),
              "broken stdout left a committed Stage 8 bundle");
    }
#endif
}

int main(int argc, char **argv)
{
    TestFiles files;
    char binding[PATH_MAX + 32U];
    int written;
    if (argc != 10) {
        (void)fprintf(stderr,
                      "usage: %s ANALYZER OK FAIL SLOW LARGE EXTRA WORKER "
                      "QUICK CHURN\n",
                      argv[0]);
        return 2;
    }
    analyzer_path = argv[1];
    renderer_ok = argv[2];
    renderer_fail = argv[3];
    renderer_slow = argv[4];
    renderer_large = argv[5];
    renderer_extra = argv[6];
    renderer_worker = argv[7];
    renderer_quick = argv[8];
    renderer_churn = argv[9];
    if (!test_files_open(&files)) return 2;
    written = snprintf(binding, sizeof(binding), "artist=%s", files.reference);
    if (written < 0 || (size_t)written >= sizeof(binding) ||
        !test_write_wave(files.reference) || !test_write_manifest(&files)
#if !defined(_WIN32)
        || symlink(renderer_ok, files.renderer_link) != 0
#endif
        ) {
        (void)test_remove_tree(files.directory);
        return 2;
    }
    test_failures(&files, binding);
    test_success(&files, binding);
    if (failures != 0) {
        (void)fprintf(stderr, "Stage 8 CLI fixtures kept at %s\n",
                      files.directory);
        (void)fprintf(stderr, "%d Stage 8 CLI test(s) failed\n", failures);
        return 1;
    }
    CHECK(test_remove_tree(files.directory) == 0,
          "cannot remove Stage 8 CLI fixture tree");
    (void)puts("stage8 cli tests passed");
    return 0;
}

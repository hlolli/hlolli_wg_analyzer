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
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct TestFiles {
    char root[PATH_MAX];
    char manifest[PATH_MAX];
    char source[PATH_MAX];
    char output[PATH_MAX];
    char second[PATH_MAX];
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

static int test_join(char path[PATH_MAX], const char *root, const char *name)
{
    int written = snprintf(path, PATH_MAX, "%s/%s", root, name);
    return written > 0 && written < PATH_MAX;
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
    struct stat status;
    return lstat(path, &status) == 0;
#endif
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
    (void)closedir(directory);
    return rmdir(path);
#endif
}

static int test_open(TestFiles *files)
{
    unsigned attempt;
#if defined(_WIN32)
    const char *temporary = getenv("TEMP");
    if (temporary == NULL || temporary[0] == '\0') temporary = ".";
#else
    const char *temporary = "/tmp";
#endif
    memset(files, 0, sizeof(*files));
    for (attempt = 0U; attempt < 100U; ++attempt) {
        int written = snprintf(files->root, sizeof(files->root),
                               "%s/hwa-stage9-cli-%ld-%u",
                               temporary, test_pid(), attempt);
        if (written < 0 || (size_t)written >= sizeof(files->root)) return 0;
        if (test_mkdir(files->root) == 0) break;
        if (errno != EEXIST) return 0;
    }
    return attempt < 100U &&
           test_join(files->manifest, files->root, "report.json") &&
           test_join(files->source, files->root, "source.hwa") &&
           test_join(files->output, files->root, "report") &&
           test_join(files->second, files->root, "report-2") &&
           test_join(files->stdout_path, files->root, "stdout.txt") &&
           test_join(files->stderr_path, files->root, "stderr.txt");
}

static int test_write(const char *path, const char *text)
{
    FILE *stream = fopen(path, "wb");
    size_t length = strlen(text);
    int ok = stream != NULL && fwrite(text, 1U, length, stream) == length;
    if (stream != NULL && fclose(stream) != 0) ok = 0;
    return ok;
}

static int test_contains(const char *path, const char *needle)
{
    FILE *stream = fopen(path, "rb");
    char buffer[4096];
    size_t used;
    int found = 0;
    if (stream == NULL) return 0;
    used = fread(buffer, 1U, sizeof(buffer) - 1U, stream);
    buffer[used] = '\0';
    if (!ferror(stream) && strstr(buffer, needle) != NULL) found = 1;
    if (fclose(stream) != 0) found = 0;
    return found;
}

static int test_run(const TestFiles *files,
                    const char *const *arguments,
                    size_t argument_count)
{
#if defined(_WIN32)
    const char **argv = (const char **)calloc(argument_count + 2U,
                                                sizeof(*argv));
    intptr_t status;
    size_t index;
    int saved_stdout;
    int saved_stderr;
    int output;
    int errors;
    if (argv == NULL) return -1;
    argv[0] = analyzer_path;
    for (index = 0U; index < argument_count; ++index)
        argv[index + 1U] = arguments[index];
    saved_stdout = _dup(_fileno(stdout));
    saved_stderr = _dup(_fileno(stderr));
    output = _open(files->stdout_path, _O_CREAT | _O_TRUNC | _O_WRONLY |
                   _O_BINARY, _S_IREAD | _S_IWRITE);
    errors = _open(files->stderr_path, _O_CREAT | _O_TRUNC | _O_WRONLY |
                   _O_BINARY, _S_IREAD | _S_IWRITE);
    if (saved_stdout < 0 || saved_stderr < 0 || output < 0 || errors < 0) {
        free(argv);
        return -1;
    }
    (void)_dup2(output, _fileno(stdout));
    (void)_dup2(errors, _fileno(stderr));
    status = _spawnv(_P_WAIT, analyzer_path, argv);
    (void)_dup2(saved_stdout, _fileno(stdout));
    (void)_dup2(saved_stderr, _fileno(stderr));
    (void)_close(output);
    (void)_close(errors);
    (void)_close(saved_stdout);
    (void)_close(saved_stderr);
    free(argv);
    return (int)status;
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
        output = open(files->stdout_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        errors = open(files->stderr_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
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

static void test_cli_contract(void)
{
    static const char *zero_flags[] = {
        "--max-report-manifest-bytes", "--max-report-input-bytes",
        "--max-report-input-frames", "--max-report-work-bytes",
        "--max-report-evaluations", "--max-report-output-file-bytes",
        "--max-report-bundle-bytes", "--max-report-excerpt-frames",
        "--max-report-total-excerpt-frames", "--max-report-sources",
        "--max-report-labels", "--max-report-candidates",
        "--max-report-families", "--max-report-groups",
        "--max-report-cases", "--max-report-excerpts",
        "--max-report-warnings", "--max-report-json-depth",
        "--max-report-json-tokens"
    };
    TestFiles files;
    const char *rank_output[] = {
        "rank", files.manifest, "--output", files.output
    };
    const char *excerpt_no_output[] = {"excerpt", files.manifest};
    const char *report_stdout[] = {
        "report", files.manifest, "--output", "-"
    };
    const char *replace[] = {
        "--replace", "report", files.manifest, "--output", files.output
    };
    const char *allow[] = {
        "--allow-run", "rank", files.manifest
    };
    const char *stdin_manifest[] = {"rank", "-"};
    const char *bad_binding[] = {
        "rank", files.manifest, "--bind", "source="
    };
    const char *unrelated_cap[] = {
        "--max-report-sources", "1", "inspect", files.source
    };
    size_t index;

    CHECK(test_open(&files), "cannot make Stage 9 CLI workspace");
    if (failures != 0) return;
    CHECK(test_write(files.manifest, "{}\n") &&
              test_write(files.source, "source\n"),
          "cannot make Stage 9 CLI parse fixtures");
    CHECK(test_run(&files, rank_output,
                   sizeof(rank_output) / sizeof(rank_output[0])) == 2,
          "rank accepted --output");
    CHECK(test_run(&files, excerpt_no_output,
                   sizeof(excerpt_no_output) /
                       sizeof(excerpt_no_output[0])) == 2,
          "excerpt accepted a missing output");
    CHECK(test_run(&files, report_stdout,
                   sizeof(report_stdout) / sizeof(report_stdout[0])) == 2,
          "report accepted standard output as its bundle");
    CHECK(test_run(&files, replace, sizeof(replace) / sizeof(replace[0])) == 2,
          "Stage 9 accepted --replace");
    CHECK(test_run(&files, allow, sizeof(allow) / sizeof(allow[0])) == 2,
          "Stage 9 accepted --allow-run");
    CHECK(test_run(&files, stdin_manifest,
                   sizeof(stdin_manifest) / sizeof(stdin_manifest[0])) == 2,
          "Stage 9 accepted a standard-input manifest");
    CHECK(test_run(&files, bad_binding,
                   sizeof(bad_binding) / sizeof(bad_binding[0])) != 0,
          "Stage 9 accepted an empty binding path");
    CHECK(test_run(&files, unrelated_cap,
                   sizeof(unrelated_cap) / sizeof(unrelated_cap[0])) == 2,
          "another command accepted a Stage 9 cap");
    for (index = 0U;
         index < sizeof(zero_flags) / sizeof(zero_flags[0]); ++index) {
        const char *zero[] = {
            zero_flags[index], "0", "rank", files.manifest
        };
        CHECK(test_run(&files, zero, sizeof(zero) / sizeof(zero[0])) == 2,
              "Stage 9 accepted zero for %s", zero_flags[index]);
    }
    CHECK(!test_exists(files.output) && !test_exists(files.second),
          "a rejected Stage 9 command published output");
    (void)test_remove_tree(files.root);
}

static void test_cli_success(void)
{
    TestFiles files;
    char hash[HWA_SHA256_HEX_SIZE];
    char binding[PATH_MAX + 16U];
    char manifest[2048];
    char result_path[PATH_MAX];
    char csv_path[PATH_MAX];
    char json_path[PATH_MAX];
    char html_path[PATH_MAX];
    char audio_path[PATH_MAX];
    char error[HWA_ERROR_SIZE];
    const char *rank[5];
    const char *report[7];
    int written;

    CHECK(test_open(&files), "cannot make Stage 9 success workspace");
    if (failures != 0) return;
    CHECK(test_write(files.source, "bound Stage 9 source\n") &&
              hwa_sha256_file(files.source, UINT64_C(1048576), hash,
                              error, sizeof(error)) == 0,
          "cannot make Stage 9 success source");
    written = snprintf(
        manifest, sizeof(manifest),
        "{\"schema\":\"hwa-gap-report\",\"schema_version\":1,"
        "\"method_version\":\"stage9-1\","
        "\"audibility_method\":\"hwa-audibility-1\","
        "\"title\":\"CLI success\",\"sources\":[{"
        "\"id\":\"wave\",\"kind\":\"wave\",\"sha256\":\"%s\"}],"
        "\"case_labels\":[],\"excerpts\":[]}", hash);
    CHECK(written > 0 && (size_t)written < sizeof(manifest) &&
              test_write(files.manifest, manifest),
          "cannot make Stage 9 success manifest");
    written = snprintf(binding, sizeof(binding), "wave=%s", files.source);
    CHECK(written > 0 && (size_t)written < sizeof(binding),
          "cannot make Stage 9 success binding");
    if (failures != 0) {
        (void)test_remove_tree(files.root);
        return;
    }

    rank[0] = "rank";
    rank[1] = files.manifest;
    rank[2] = "--bind";
    rank[3] = binding;
    rank[4] = NULL;
    CHECK(test_run(&files, rank, 4U) == 0,
          "valid Stage 9 rank command failed");
    CHECK(test_contains(files.stdout_path, "Stage 9 gap report") &&
              test_contains(files.stdout_path, "CLI success"),
          "rank output omits its heading or title");
    CHECK(!test_exists(files.output), "rank published an output tree");

    report[0] = "report";
    report[1] = files.manifest;
    report[2] = "--bind";
    report[3] = binding;
    report[4] = "--output";
    report[5] = files.output;
    report[6] = NULL;
    CHECK(test_run(&files, report, 6U) == 0,
          "valid Stage 9 report command failed");
    CHECK(test_join(result_path, files.output, "result.hwa-report") &&
              test_join(csv_path, files.output, "report.csv") &&
              test_join(json_path, files.output, "report.json") &&
              test_join(html_path, files.output, "report.html") &&
              test_join(audio_path, files.output, "audio") &&
              test_exists(result_path) && test_exists(csv_path) &&
              test_exists(json_path) && test_exists(html_path) &&
              test_exists(audio_path),
          "report did not publish the fixed tree");
    CHECK(test_run(&files, report, 6U) != 0 &&
              test_exists(result_path),
          "report replaced or damaged an existing output tree");
    (void)test_remove_tree(files.root);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        (void)fputs("usage: stage9_cli_tests ANALYZER\n", stderr);
        return 2;
    }
    analyzer_path = argv[1];
    test_cli_contract();
    test_cli_success();
    if (failures != 0) {
        (void)fprintf(stderr, "%d Stage 9 CLI test(s) failed\n", failures);
        return 1;
    }
    return 0;
}

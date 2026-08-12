#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "file_output.h"

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

static int failures;

#define CHECK(condition, ...)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);       \
            (void)fprintf(stderr, __VA_ARGS__);                              \
            (void)fputc('\n', stderr);                                       \
            failures++;                                                      \
        }                                                                    \
    } while (0)

typedef struct TestWorkspace {
    char directory[PATH_MAX];
} TestWorkspace;

static long test_process_id(void)
{
#if defined(_WIN32)
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}

static int make_directory(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

static int remove_directory(const char *path)
{
#if defined(_WIN32)
    return _rmdir(path);
#else
    return rmdir(path);
#endif
}

static int remove_file(const char *path)
{
#if defined(_WIN32)
    return _unlink(path);
#else
    return unlink(path);
#endif
}

static int path_exists(const char *path)
{
#if defined(_WIN32)
    struct _stat64 status;
    return _stat64(path, &status) == 0;
#else
    struct stat status;
    return lstat(path, &status) == 0;
#endif
}

static int workspace_open(TestWorkspace *workspace)
{
    unsigned attempt;
#if defined(_WIN32)
    const char *temporary_root = getenv("TEMP");
    if (temporary_root == NULL || temporary_root[0] == '\0') {
        temporary_root = ".";
    }
#else
    const char *temporary_root = "/tmp";
#endif

    memset(workspace, 0, sizeof(*workspace));
    for (attempt = 0U; attempt < 100U; ++attempt) {
        int length = snprintf(workspace->directory,
                              sizeof(workspace->directory),
                              "%s/hwa-file-output-%ld-%u",
                              temporary_root, test_process_id(), attempt);

        if (length < 0 ||
            (size_t)length >= sizeof(workspace->directory)) {
            return 0;
        }
        if (make_directory(workspace->directory) == 0) {
            return 1;
        }
        if (errno != EEXIST) {
            return 0;
        }
    }
    return 0;
}

static int join_path(char *path,
                     size_t path_size,
                     const TestWorkspace *workspace,
                     const char *name)
{
    int length = snprintf(path, path_size, "%s/%s",
                          workspace->directory, name);

    return length >= 0 && (size_t)length < path_size;
}

static int write_text(const char *path, const char *text)
{
    FILE *stream = fopen(path, "wb");
    size_t length = strlen(text);
    int ok = stream != NULL &&
             fwrite(text, 1U, length, stream) == length;

    if (stream != NULL && fclose(stream) != 0) {
        ok = 0;
    }
    return ok;
}

static char *read_text(const char *path)
{
    FILE *stream = fopen(path, "rb");
    long length;
    char *text;

    if (stream == NULL || fseek(stream, 0L, SEEK_END) != 0) {
        if (stream != NULL) {
            (void)fclose(stream);
        }
        return NULL;
    }
    length = ftell(stream);
    if (length < 0 || fseek(stream, 0L, SEEK_SET) != 0) {
        (void)fclose(stream);
        return NULL;
    }
    if ((uintmax_t)length > (uintmax_t)(SIZE_MAX - 1U)) {
        (void)fclose(stream);
        return NULL;
    }
    text = (char *)malloc((size_t)length + 1U);
    if (text == NULL) {
        (void)fclose(stream);
        return NULL;
    }
    if (fread(text, 1U, (size_t)length, stream) != (size_t)length ||
        fclose(stream) != 0) {
        free(text);
        return NULL;
    }
    text[length] = '\0';
    return text;
}

static int temporary_file_count(const TestWorkspace *workspace)
{
#if defined(_WIN32)
    char pattern[PATH_MAX];
    struct _finddata_t entry;
    intptr_t search;
    int count = 0;

    if (!join_path(pattern, sizeof(pattern), workspace, "*.hwa-tmp-*")) {
        return -1;
    }
    search = _findfirst(pattern, &entry);
    if (search == (intptr_t)-1) {
        return errno == ENOENT ? 0 : -1;
    }
    do {
        count++;
    } while (_findnext(search, &entry) == 0);
    (void)_findclose(search);
    return count;
#else
    DIR *directory = opendir(workspace->directory);
    struct dirent *entry;
    int count = 0;

    if (directory == NULL) {
        return -1;
    }
    while ((entry = readdir(directory)) != NULL) {
        if (strstr(entry->d_name, ".hwa-tmp-") != NULL) {
            count++;
        }
    }
    if (closedir(directory) != 0) {
        return -1;
    }
    return count;
#endif
}

static void test_new_output_and_abort(TestWorkspace *workspace)
{
    HWAFileOutput output;
    char path[PATH_MAX];
    char error[256];
    char *text;

    CHECK(join_path(path, sizeof(path), workspace, "new.txt"),
          "new output path is too long");
    CHECK(hwa_file_output_open(&output, path, NULL, 0U, 0,
                               error, sizeof(error)) == 0,
          "new output open failed: %s", error);
    CHECK(fputs("complete\n", hwa_file_output_stream(&output)) != EOF,
          "new output write failed");
    CHECK(hwa_file_output_finish(&output, "test output",
                                 error, sizeof(error)) == 0,
          "new output finish failed: %s", error);
    text = read_text(path);
    CHECK(text != NULL && strcmp(text, "complete\n") == 0,
          "new output content is wrong");
    free(text);

    CHECK(hwa_file_output_open(&output, path, NULL, 0U, 0,
                               error, sizeof(error)) != 0,
          "exclusive create replaced an existing output");
    text = read_text(path);
    CHECK(text != NULL && strcmp(text, "complete\n") == 0,
          "exclusive-create refusal changed the old output");
    free(text);
    (void)remove_file(path);

    CHECK(join_path(path, sizeof(path), workspace, "abort.txt"),
          "abort output path is too long");
    CHECK(hwa_file_output_open(&output, path, NULL, 0U, 0,
                               error, sizeof(error)) == 0,
          "abort output open failed: %s", error);
    CHECK(fputs("partial", hwa_file_output_stream(&output)) != EOF,
          "abort output write failed");
    hwa_file_output_abort(&output);
    CHECK(!path_exists(path), "abort left a new partial file");
}

static void test_identity_read_failure_cleanup(TestWorkspace *workspace)
{
    HWAFileOutput output;
    char direct[PATH_MAX];
    char replacement[PATH_MAX];
    char error[256];
    char *text;

    CHECK(join_path(direct, sizeof(direct), workspace,
                    "identity-read-direct.txt") &&
              join_path(replacement, sizeof(replacement), workspace,
                        "identity-read-replace.txt"),
          "identity-read failure paths are too long");
    hwa_file_output_test_fail_created_identity(1U);
    CHECK(hwa_file_output_open(&output, direct, NULL, 0U, 0,
                               error, sizeof(error)) != 0,
          "forced direct identity failure was accepted");
    CHECK(!path_exists(direct),
          "direct identity-read failure left an empty output");

    CHECK(write_text(replacement, "old"),
          "could not make identity-read replacement fixture");
    hwa_file_output_test_fail_created_identity(1U);
    CHECK(hwa_file_output_open(&output, replacement, NULL, 0U, 1,
                               error, sizeof(error)) != 0,
          "forced temporary identity failure was accepted");
    text = read_text(replacement);
    CHECK(text != NULL && strcmp(text, "old") == 0,
          "temporary identity-read failure changed the old output");
    free(text);
    CHECK(temporary_file_count(workspace) == 0,
          "temporary identity-read failure left an empty file");
    (void)remove_file(replacement);
}

static void test_protected_inputs(TestWorkspace *workspace)
{
    HWAFileOutput output;
    char first[PATH_MAX];
    char second[PATH_MAX];
    char alias[PATH_MAX];
    char error[256];
    const char *protected_paths[2];

    CHECK(join_path(first, sizeof(first), workspace, "first.input"),
          "first input path is too long");
    CHECK(join_path(second, sizeof(second), workspace, "second.input"),
          "second input path is too long");
    CHECK(join_path(alias, sizeof(alias), workspace, "input.alias"),
          "alias path is too long");
    CHECK(write_text(first, "first") && write_text(second, "second"),
          "could not make protected input fixtures");
    protected_paths[0] = first;
    protected_paths[1] = second;
    CHECK(hwa_file_output_open(&output, second, protected_paths, 2U, 1,
                               error, sizeof(error)) != 0 &&
              strstr(error, "protected input") != NULL,
          "output did not protect the second input: %s", error);
#if defined(_WIN32)
    CHECK(CreateHardLinkA(alias, first, NULL) != 0,
          "could not make protected hard link");
#else
    CHECK(link(first, alias) == 0,
          "could not make protected hard link: %s", strerror(errno));
#endif
    if (path_exists(alias)) {
        CHECK(hwa_file_output_open(&output, alias, protected_paths, 2U, 1,
                                   error, sizeof(error)) != 0 &&
                  strstr(error, "protected input") != NULL,
              "hard-link output did not protect its input: %s", error);
    }
    CHECK(hwa_file_output_open(&output, first, NULL, 1U, 0,
                               error, sizeof(error)) != 0,
          "null protected path array was accepted");
    protected_paths[0] = NULL;
    CHECK(hwa_file_output_open(&output, first, protected_paths, 1U, 0,
                               error, sizeof(error)) != 0,
          "null protected path was accepted");
    (void)remove_file(alias);
    (void)remove_file(second);
    (void)remove_file(first);
}

static void test_replacement(TestWorkspace *workspace)
{
    static const char old_text[] = "old bytes\n";
    HWAFileOutput output;
    char path[PATH_MAX];
    char error[256];
    char *text;
#if !defined(_WIN32)
    struct stat status;
#endif

    CHECK(join_path(path, sizeof(path), workspace, "replace.txt"),
          "replacement path is too long");
    CHECK(write_text(path, old_text),
          "could not make replacement fixture");
#if !defined(_WIN32)
    CHECK(chmod(path, 0600) == 0,
          "could not set replacement mode");
#endif
    CHECK(hwa_file_output_open(&output, path, NULL, 0U, 1,
                               error, sizeof(error)) == 0,
          "replacement open failed: %s", error);
    CHECK(fputs("new bytes\n", hwa_file_output_stream(&output)) != EOF,
          "replacement write failed");
    CHECK(hwa_file_output_finish(&output, "test output",
                                 error, sizeof(error)) == 0,
          "replacement finish failed: %s", error);
    text = read_text(path);
    CHECK(text != NULL && strcmp(text, "new bytes\n") == 0,
          "replacement content is wrong");
    free(text);
#if !defined(_WIN32)
    CHECK(stat(path, &status) == 0 && (status.st_mode & 0777) == 0600,
          "replacement did not keep mode 0600");
#endif

    CHECK(hwa_file_output_open(&output, path, NULL, 0U, 1,
                               error, sizeof(error)) == 0,
          "replacement abort open failed: %s", error);
    CHECK(fputs("partial", hwa_file_output_stream(&output)) != EOF,
          "replacement abort write failed");
    hwa_file_output_abort(&output);
    text = read_text(path);
    CHECK(text != NULL && strcmp(text, "new bytes\n") == 0,
          "replacement abort changed the old output");
    free(text);
    CHECK(temporary_file_count(workspace) == 0,
          "replacement left a temporary file");
    (void)remove_file(path);
}

static void test_replacement_identity(TestWorkspace *workspace)
{
    HWAFileOutput output;
    char path[PATH_MAX];
    char moved[PATH_MAX];
    char appeared[PATH_MAX];
    char error[256];
    char *text;

    CHECK(join_path(path, sizeof(path), workspace, "identity.txt"),
          "identity path is too long");
    CHECK(join_path(moved, sizeof(moved), workspace, "identity.old"),
          "moved path is too long");
    CHECK(write_text(path, "original"),
          "could not make identity fixture");
    CHECK(hwa_file_output_open(&output, path, NULL, 0U, 1,
                               error, sizeof(error)) == 0,
          "identity replacement open failed: %s", error);
    CHECK(fputs("wanted", hwa_file_output_stream(&output)) != EOF,
          "identity replacement write failed");
    CHECK(rename(path, moved) == 0 && write_text(path, "intruder"),
          "could not swap the replacement identity");
    CHECK(hwa_file_output_finish(&output, "test output",
                                 error, sizeof(error)) != 0 &&
              strstr(error, "changed") != NULL,
          "changed replacement identity was committed: %s", error);
    text = read_text(path);
    CHECK(text != NULL && strcmp(text, "intruder") == 0,
          "identity rejection changed the new target");
    free(text);
    CHECK(temporary_file_count(workspace) == 0,
          "identity rejection left a temporary file");
    (void)remove_file(path);
    (void)remove_file(moved);

    CHECK(join_path(appeared, sizeof(appeared), workspace, "appeared.txt"),
          "appeared path is too long");
    CHECK(hwa_file_output_open(&output, appeared, NULL, 0U, 1,
                               error, sizeof(error)) == 0,
          "missing replacement open failed: %s", error);
    CHECK(fputs("wanted", hwa_file_output_stream(&output)) != EOF,
          "missing replacement write failed");
    CHECK(write_text(appeared, "appeared"),
          "could not make a replacement target appear");
    CHECK(hwa_file_output_finish(&output, "test output",
                                 error, sizeof(error)) != 0 &&
              strstr(error, "appeared") != NULL,
          "appeared replacement target was overwritten: %s", error);
    text = read_text(appeared);
    CHECK(text != NULL && strcmp(text, "appeared") == 0,
          "appeared-target rejection changed the target");
    free(text);
    CHECK(temporary_file_count(workspace) == 0,
          "appeared-target rejection left a temporary file");
    (void)remove_file(appeared);
}

#if !defined(_WIN32)
static void test_created_file_identity_swaps(TestWorkspace *workspace)
{
    HWAFileOutput output;
    char direct[PATH_MAX];
    char direct_moved[PATH_MAX];
    char direct_abort[PATH_MAX];
    char direct_abort_moved[PATH_MAX];
    char replacement[PATH_MAX];
    char temporary[PATH_MAX];
    char temporary_moved[PATH_MAX];
    char error[256];
    char *text;

    CHECK(join_path(direct, sizeof(direct), workspace,
                    "direct-finish.txt") &&
              join_path(direct_moved, sizeof(direct_moved), workspace,
                        "direct-finish-owned.txt") &&
              join_path(direct_abort, sizeof(direct_abort), workspace,
                        "direct-abort.txt") &&
              join_path(direct_abort_moved, sizeof(direct_abort_moved), workspace,
                        "direct-abort-owned.txt") &&
              join_path(replacement, sizeof(replacement), workspace,
                        "temporary-swap.txt") &&
              join_path(temporary_moved, sizeof(temporary_moved), workspace,
                        "temporary-owned.txt"),
          "created-identity paths are too long");

    CHECK(hwa_file_output_open(&output, direct, NULL, 0U, 0,
                               error, sizeof(error)) == 0,
          "direct swap finish open failed: %s", error);
    CHECK(fputs("wanted", hwa_file_output_stream(&output)) != EOF &&
              rename(direct, direct_moved) == 0 &&
              write_text(direct, "intruder"),
          "could not swap the direct finish path");
    CHECK(hwa_file_output_finish(&output, "test output",
                                 error, sizeof(error)) != 0 &&
              strstr(error, "changed") != NULL,
          "direct finish accepted a changed created path: %s", error);
    text = read_text(direct);
    CHECK(text != NULL && strcmp(text, "intruder") == 0,
          "direct finish changed the swapped-in file");
    free(text);
    (void)remove_file(direct);
    (void)remove_file(direct_moved);

    CHECK(hwa_file_output_open(&output, direct_abort, NULL, 0U, 0,
                               error, sizeof(error)) == 0,
          "direct swap abort open failed: %s", error);
    CHECK(fputs("wanted", hwa_file_output_stream(&output)) != EOF &&
              rename(direct_abort, direct_abort_moved) == 0 &&
              write_text(direct_abort, "intruder"),
          "could not swap the direct abort path");
    CHECK(hwa_file_output_abort(&output) != 0,
          "direct abort accepted a changed created path");
    text = read_text(direct_abort);
    CHECK(text != NULL && strcmp(text, "intruder") == 0,
          "direct abort removed the swapped-in file");
    free(text);
    (void)remove_file(direct_abort);
    (void)remove_file(direct_abort_moved);

    CHECK(write_text(replacement, "original"),
          "could not make the temporary swap target");
    CHECK(hwa_file_output_open(&output, replacement, NULL, 0U, 1,
                               error, sizeof(error)) == 0,
          "temporary swap finish open failed: %s", error);
    CHECK(output.temporary_path != NULL &&
              snprintf(temporary, sizeof(temporary), "%s",
                       output.temporary_path) >= 0 &&
              strlen(output.temporary_path) < sizeof(temporary),
          "could not copy the temporary finish path");
    CHECK(fputs("wanted", hwa_file_output_stream(&output)) != EOF &&
              rename(temporary, temporary_moved) == 0 &&
              write_text(temporary, "intruder"),
          "could not swap the temporary finish path");
    CHECK(hwa_file_output_finish(&output, "test output",
                                 error, sizeof(error)) != 0 &&
              strstr(error, "changed") != NULL,
          "replacement committed a changed temporary path: %s", error);
    text = read_text(replacement);
    CHECK(text != NULL && strcmp(text, "original") == 0,
          "temporary finish changed the replacement target");
    free(text);
    text = read_text(temporary);
    CHECK(text != NULL && strcmp(text, "intruder") == 0,
          "temporary finish changed the swapped-in file");
    free(text);
    (void)remove_file(temporary);
    (void)remove_file(temporary_moved);

    CHECK(hwa_file_output_open(&output, replacement, NULL, 0U, 1,
                               error, sizeof(error)) == 0,
          "temporary swap abort open failed: %s", error);
    CHECK(output.temporary_path != NULL &&
              snprintf(temporary, sizeof(temporary), "%s",
                       output.temporary_path) >= 0 &&
              strlen(output.temporary_path) < sizeof(temporary),
          "could not copy the temporary abort path");
    CHECK(fputs("wanted", hwa_file_output_stream(&output)) != EOF &&
              rename(temporary, temporary_moved) == 0 &&
              write_text(temporary, "intruder"),
          "could not swap the temporary abort path");
    CHECK(hwa_file_output_abort(&output) != 0,
          "replacement abort accepted a changed temporary path");
    text = read_text(replacement);
    CHECK(text != NULL && strcmp(text, "original") == 0,
          "temporary abort changed the replacement target");
    free(text);
    text = read_text(temporary);
    CHECK(text != NULL && strcmp(text, "intruder") == 0,
          "temporary abort removed the swapped-in file");
    free(text);
    (void)remove_file(temporary);
    (void)remove_file(temporary_moved);
    (void)remove_file(replacement);
}
#endif

static void test_special_paths_and_stdout(TestWorkspace *workspace)
{
    HWAFileOutput output;
    char directory[PATH_MAX];
    char error[256];
    unsigned stdout_calls = hwa_file_output_test_stdout_binary_calls();

    CHECK(join_path(directory, sizeof(directory), workspace, "target-dir"),
          "special directory path is too long");
    CHECK(make_directory(directory) == 0,
          "could not make special output directory");
    CHECK(hwa_file_output_open(&output, directory, NULL, 0U, 1,
                               error, sizeof(error)) != 0 &&
              strstr(error, "regular file") != NULL,
          "directory replacement was accepted: %s", error);
    (void)remove_directory(directory);

#if !defined(_WIN32)
    {
        char target[PATH_MAX];
        char symbolic[PATH_MAX];
        char fifo[PATH_MAX];

        CHECK(join_path(target, sizeof(target), workspace, "target.txt") &&
                  join_path(symbolic, sizeof(symbolic), workspace,
                            "target.link") &&
                  join_path(fifo, sizeof(fifo), workspace, "target.fifo"),
              "special output path is too long");
        CHECK(write_text(target, "target") &&
                  symlink(target, symbolic) == 0 &&
                  mkfifo(fifo, 0600) == 0,
              "could not make special output fixtures");
        CHECK(hwa_file_output_open(&output, symbolic, NULL, 0U, 1,
                                   error, sizeof(error)) != 0 &&
                  strstr(error, "regular file") != NULL,
              "symlink replacement was accepted: %s", error);
        CHECK(hwa_file_output_open(&output, fifo, NULL, 0U, 1,
                                   error, sizeof(error)) != 0 &&
                  strstr(error, "regular file") != NULL,
              "FIFO replacement was accepted: %s", error);
        (void)remove_file(fifo);
        (void)remove_file(symbolic);
        (void)remove_file(target);
    }
#endif

    CHECK(hwa_file_output_open(&output, "-", NULL, 0U, 1,
                               error, sizeof(error)) != 0 &&
              strstr(error, "standard output") != NULL,
          "replace was accepted for standard output: %s", error);
    CHECK(hwa_file_output_test_stdout_binary_calls() == stdout_calls,
          "rejected stdout replacement changed its descriptor mode");

    hwa_file_output_test_fail_stdout_binary(1U);
    CHECK(hwa_file_output_open(&output, "-", NULL, 0U, 0,
                               error, sizeof(error)) != 0 &&
              strstr(error, "binary mode") != NULL &&
              hwa_file_output_stream(&output) == NULL,
          "stdout binary-mode failure was accepted: %s", error);
    CHECK(hwa_file_output_test_stdout_binary_calls() == stdout_calls + 1U,
          "stdout binary-mode failure did not use the output boundary");

    CHECK(hwa_file_output_open(&output, "-", NULL, 0U, 0,
                               error, sizeof(error)) == 0 &&
              hwa_file_output_stream(&output) == stdout,
          "standard output open failed: %s", error);
    CHECK(hwa_file_output_test_stdout_binary_calls() == stdout_calls + 2U,
          "stdout open did not prepare the descriptor once");
    CHECK(hwa_file_output_finish(&output, "standard output",
                                 error, sizeof(error)) == 0,
          "standard output finish failed: %s", error);
}

int main(void)
{
    TestWorkspace workspace;

    CHECK(workspace_open(&workspace),
          "could not make test workspace");
    if (failures == 0) {
        test_new_output_and_abort(&workspace);
        test_identity_read_failure_cleanup(&workspace);
        test_protected_inputs(&workspace);
        test_replacement(&workspace);
        test_replacement_identity(&workspace);
#if !defined(_WIN32)
        test_created_file_identity_swaps(&workspace);
#endif
        test_special_paths_and_stdout(&workspace);
        CHECK(temporary_file_count(&workspace) == 0,
              "test suite left a temporary file");
    }
    (void)remove_directory(workspace.directory);
    if (failures != 0) {
        (void)fprintf(stderr, "%d file-output test(s) failed\n", failures);
        return 1;
    }
    return 0;
}

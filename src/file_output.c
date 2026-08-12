#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "file_output.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#if defined(HWA_FILE_OUTPUT_TESTING)
static unsigned hwa_test_created_identity_failures;
static unsigned hwa_test_stdout_binary_failures;
static unsigned hwa_test_stdout_binary_calls;

void hwa_file_output_test_fail_created_identity(unsigned count)
{
    hwa_test_created_identity_failures = count;
}

void hwa_file_output_test_fail_stdout_binary(unsigned count)
{
    hwa_test_stdout_binary_failures = count;
}

unsigned hwa_file_output_test_stdout_binary_calls(void)
{
    return hwa_test_stdout_binary_calls;
}
#endif

static void hwa_output_error(char *error,
                             size_t error_size,
                             const char *message)
{
    if (error != NULL && error_size > 0U) {
        (void)snprintf(error, error_size, "%s", message);
    }
}

static int hwa_prepare_standard_output(char *error, size_t error_size)
{
#if defined(HWA_FILE_OUTPUT_TESTING)
    hwa_test_stdout_binary_calls++;
    if (hwa_test_stdout_binary_failures != 0U) {
        hwa_test_stdout_binary_failures--;
        hwa_output_error(error, error_size,
                         "cannot set standard output to binary mode");
        return -1;
    }
#endif
#if defined(_WIN32)
    {
        int descriptor = _fileno(stdout);

        if (descriptor < 0 || fflush(stdout) != 0 ||
            _setmode(descriptor, _O_BINARY) == -1) {
            hwa_output_error(error, error_size,
                             "cannot set standard output to binary mode");
            return -1;
        }
        /* The CLI owns stdout until exit, so no later mode restore is needed. */
    }
#else
    (void)error;
    (void)error_size;
#endif
    return 0;
}

static char *hwa_output_copy_path(const char *path)
{
    size_t length;
    char *copy;

    if (path == NULL) {
        return NULL;
    }
    length = strlen(path);
    if (length == SIZE_MAX) {
        return NULL;
    }
    copy = (char *)malloc(length + 1U);
    if (copy != NULL) {
        memcpy(copy, path, length + 1U);
    }
    return copy;
}

static int hwa_output_descriptor(const char *path)
{
#if defined(_WIN32)
    return _open(path,
                 _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT,
                 _S_IREAD | _S_IWRITE);
#else
    int flags = O_WRONLY | O_CREAT | O_EXCL;

#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    return open(path, flags, 0644);
#endif
}

static FILE *hwa_stream_from_descriptor(int descriptor)
{
#if defined(_WIN32)
    return _fdopen(descriptor, "wb");
#else
    return fdopen(descriptor, "wb");
#endif
}

static void hwa_close_descriptor(int descriptor)
{
#if defined(_WIN32)
    (void)_close(descriptor);
#else
    (void)close(descriptor);
#endif
}

static int hwa_unlink_path(const char *path)
{
#if defined(_WIN32)
    return _unlink(path);
#else
    return unlink(path);
#endif
}

static int hwa_same_file(const char *left, const char *right)
{
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION left_info;
    BY_HANDLE_FILE_INFORMATION right_info;
    HANDLE left_handle = CreateFileA(
        left, 0U, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    HANDLE right_handle = CreateFileA(
        right, 0U, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    int same = 0;

    if (left_handle != INVALID_HANDLE_VALUE &&
        right_handle != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(left_handle, &left_info) &&
        GetFileInformationByHandle(right_handle, &right_info)) {
        same = left_info.dwVolumeSerialNumber ==
                   right_info.dwVolumeSerialNumber &&
               left_info.nFileIndexHigh == right_info.nFileIndexHigh &&
               left_info.nFileIndexLow == right_info.nFileIndexLow;
    }
    if (right_handle != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(right_handle);
    }
    if (left_handle != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(left_handle);
    }
    return same;
#else
    struct stat left_status;
    struct stat right_status;

    return stat(left, &left_status) == 0 &&
           stat(right, &right_status) == 0 &&
           left_status.st_dev == right_status.st_dev &&
           left_status.st_ino == right_status.st_ino;
#endif
}

static int hwa_check_protected_paths(const char *path,
                                     const char *const *protected_paths,
                                     size_t protected_path_count,
                                     char *error,
                                     size_t error_size)
{
    size_t index;

    if (protected_path_count != 0U && protected_paths == NULL) {
        hwa_output_error(error, error_size,
                         "protected input path list is null");
        return -1;
    }
    for (index = 0U; index < protected_path_count; ++index) {
        const char *input = protected_paths[index];

        if (input == NULL) {
            hwa_output_error(error, error_size,
                             "protected input path is null");
            return -1;
        }
        if (strcmp(input, "-") != 0 && hwa_same_file(path, input)) {
            hwa_output_error(error, error_size,
                             "output resolves to the input or another protected input file");
            return -1;
        }
    }
    return 0;
}

#if defined(_WIN32)
static int hwa_windows_identity(const char *path,
                                uint64_t *device,
                                uint64_t *file,
                                int *mode,
                                int *missing,
                                char *error,
                                size_t error_size)
{
    BY_HANDLE_FILE_INFORMATION information;
    DWORD attributes = GetFileAttributesA(path);
    HANDLE handle;
    struct _stat64 status;

    *missing = 0;
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD windows_error = GetLastError();

        if (windows_error == ERROR_FILE_NOT_FOUND ||
            windows_error == ERROR_PATH_NOT_FOUND) {
            *missing = 1;
            return 0;
        }
        if (error != NULL && error_size > 0U) {
            (void)snprintf(error, error_size,
                           "cannot inspect output: Windows error %lu",
                           (unsigned long)windows_error);
        }
        return -1;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        hwa_output_error(error, error_size,
                         "replacement target is not a regular file");
        return -1;
    }
    if (_stat64(path, &status) != 0 ||
        (status.st_mode & _S_IFMT) != _S_IFREG) {
        hwa_output_error(error, error_size,
                         "replacement target is not a regular file");
        return -1;
    }
    handle = CreateFileA(path, 0U,
                         FILE_SHARE_READ | FILE_SHARE_WRITE |
                             FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(handle, &information)) {
        DWORD windows_error = GetLastError();

        if (handle != INVALID_HANDLE_VALUE) {
            (void)CloseHandle(handle);
        }
        if (error != NULL && error_size > 0U) {
            (void)snprintf(
                error, error_size,
                "cannot inspect output identity: Windows error %lu",
                (unsigned long)windows_error);
        }
        return -1;
    }
    (void)CloseHandle(handle);
    *device = (uint64_t)information.dwVolumeSerialNumber;
    *file = ((uint64_t)information.nFileIndexHigh << 32U) |
            (uint64_t)information.nFileIndexLow;
    *mode = status.st_mode;
    return 0;
}
#else
static int hwa_posix_identity(const char *path,
                              uint64_t *device,
                              uint64_t *file,
                              int *mode,
                              int *missing,
                              char *error,
                              size_t error_size)
{
    struct stat status;

    *missing = 0;
    if (lstat(path, &status) == 0) {
        if (!S_ISREG(status.st_mode)) {
            hwa_output_error(error, error_size,
                             "replacement target is not a regular file");
            return -1;
        }
        *device = (uint64_t)status.st_dev;
        *file = (uint64_t)status.st_ino;
        *mode = (int)(status.st_mode & 0777);
        return 0;
    }
    if (errno == ENOENT) {
        *missing = 1;
        return 0;
    }
    if (error != NULL && error_size > 0U) {
        (void)snprintf(error, error_size,
                       "cannot inspect output '%s': %s",
                       path, strerror(errno));
    }
    return -1;
}
#endif

static int hwa_read_replacement_identity(const char *path,
                                         uint64_t *device,
                                         uint64_t *file,
                                         int *mode,
                                         int *missing,
                                         char *error,
                                         size_t error_size)
{
#if defined(_WIN32)
    return hwa_windows_identity(path, device, file, mode, missing,
                                error, error_size);
#else
    return hwa_posix_identity(path, device, file, mode, missing,
                              error, error_size);
#endif
}

static int hwa_record_created_identity(HWAFileOutput *output,
                                       int descriptor,
                                       char *error,
                                       size_t error_size)
{
#if defined(HWA_FILE_OUTPUT_TESTING)
    if (hwa_test_created_identity_failures != 0U) {
        hwa_test_created_identity_failures--;
        errno = EIO;
        hwa_output_error(error, error_size,
                         "cannot inspect created output identity");
        return -1;
    }
#endif
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION information;
    intptr_t native_handle = _get_osfhandle(descriptor);

    if (native_handle == (intptr_t)-1 ||
        !GetFileInformationByHandle((HANDLE)native_handle, &information)) {
        hwa_output_error(error, error_size,
                         "cannot inspect created output identity");
        return -1;
    }
    output->created_device = (uint64_t)information.dwVolumeSerialNumber;
    output->created_file =
        ((uint64_t)information.nFileIndexHigh << 32U) |
        (uint64_t)information.nFileIndexLow;
#else
    struct stat status;

    if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
        if (error != NULL && error_size > 0U) {
            (void)snprintf(error, error_size,
                           "cannot inspect created output identity: %s",
                           strerror(errno));
        }
        return -1;
    }
    output->created_device = (uint64_t)status.st_dev;
    output->created_file = (uint64_t)status.st_ino;
#endif
    output->created_identity_valid = 1;
    return 0;
}

static int hwa_discard_unidentified_output(int descriptor, const char *path)
{
    int result;

    /* Exclusive create just made this path. Removing it without an identity
     * check has the same active-local-race limit as the later checked unlink,
     * but avoids leaving an empty file when the identity read itself fails. */
#if defined(_WIN32)
    hwa_close_descriptor(descriptor);
    result = hwa_unlink_path(path);
#else
    result = hwa_unlink_path(path);
    hwa_close_descriptor(descriptor);
#endif
    return result;
}

static int hwa_created_path_unchanged(const HWAFileOutput *output,
                                      const char *path,
                                      char *error,
                                      size_t error_size)
{
    uint64_t device = 0U;
    uint64_t file = 0U;
    int mode = 0;
    int missing = 0;

    if (!output->created_identity_valid ||
        hwa_read_replacement_identity(path, &device, &file, &mode, &missing,
                                      error, error_size) != 0) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_output_error(error, error_size,
                             "created output identity is unavailable");
        }
        return -1;
    }
    (void)mode;
    if (missing || device != output->created_device ||
        file != output->created_file) {
        hwa_output_error(error, error_size,
                         "created output path changed before completion");
        return -1;
    }
    return 0;
}

static int hwa_remove_created_path(const HWAFileOutput *output,
                                   const char *path)
{
    char ignored_error[128];

    ignored_error[0] = '\0';
    if (hwa_created_path_unchanged(output, path,
                                   ignored_error, sizeof(ignored_error)) != 0) {
        return -1;
    }
    /* The identity check and path unlink cannot be one atomic operation on all
     * supported hosts. This guards ordinary swaps, not an active local race. */
    return hwa_unlink_path(path) == 0 ? 0 : -1;
}

static int hwa_open_temporary_output(HWAFileOutput *output,
                                     char *error,
                                     size_t error_size)
{
    static const size_t suffix_space = 64U;
    size_t path_length = strlen(output->path);
    unsigned long process_id;
    unsigned attempt;

    if (path_length > SIZE_MAX - suffix_space) {
        hwa_output_error(error, error_size, "output path is too long");
        return -1;
    }
    output->temporary_path =
        (char *)malloc(path_length + suffix_space);
    if (output->temporary_path == NULL) {
        hwa_output_error(error, error_size,
                         "out of memory for the output path");
        return -1;
    }
#if defined(_WIN32)
    process_id = (unsigned long)_getpid();
#else
    process_id = (unsigned long)getpid();
#endif
    for (attempt = 0U; attempt < 1024U; ++attempt) {
        int descriptor;
        int length = snprintf(output->temporary_path,
                              path_length + suffix_space,
                              "%s.hwa-tmp-%lu-%u",
                              output->path, process_id, attempt);

        if (length < 0 ||
            (size_t)length >= path_length + suffix_space) {
            hwa_output_error(error, error_size,
                             "output path is too long");
            return -1;
        }
        descriptor = hwa_output_descriptor(output->temporary_path);
        if (descriptor >= 0) {
            if (hwa_record_created_identity(output, descriptor,
                                            error, error_size) != 0) {
                (void)hwa_discard_unidentified_output(
                    descriptor, output->temporary_path);
                return -1;
            }
#if defined(_WIN32)
            if (output->replacement_existed &&
                _chmod(output->temporary_path,
                       output->replacement_mode &
                           (_S_IREAD | _S_IWRITE)) != 0) {
#else
            if (output->replacement_existed &&
                fchmod(descriptor,
                       (mode_t)output->replacement_mode) != 0) {
#endif
                int saved_error = errno;

                hwa_close_descriptor(descriptor);
                (void)hwa_remove_created_path(output, output->temporary_path);
                if (error != NULL && error_size > 0U) {
                    (void)snprintf(
                        error, error_size,
                        "cannot preserve output permissions: %s",
                        strerror(saved_error));
                }
                return -1;
            }
            output->stream = hwa_stream_from_descriptor(descriptor);
            if (output->stream == NULL) {
                int saved_error = errno;

                hwa_close_descriptor(descriptor);
                (void)hwa_remove_created_path(output, output->temporary_path);
                if (error != NULL && error_size > 0U) {
                    (void)snprintf(error, error_size,
                                   "cannot open output stream: %s",
                                   strerror(saved_error));
                }
                return -1;
            }
            return 0;
        }
        if (errno != EEXIST) {
            break;
        }
    }
    if (error != NULL && error_size > 0U) {
        (void)snprintf(
            error, error_size,
            "cannot create temporary output for '%s': %s",
            output->path, strerror(errno));
    }
    return -1;
}

static int hwa_replacement_unchanged(const HWAFileOutput *output,
                                     char *error,
                                     size_t error_size)
{
    uint64_t device = 0U;
    uint64_t file = 0U;
    int mode = 0;
    int missing = 0;

    if (hwa_read_replacement_identity(output->path, &device, &file,
                                      &mode, &missing,
                                      error, error_size) != 0) {
        return -1;
    }
    (void)mode;
    if (output->replacement_existed) {
        if (missing || device != output->replacement_device ||
            file != output->replacement_file) {
            hwa_output_error(error, error_size,
                             "replacement target changed before commit");
            return -1;
        }
    } else if (!missing) {
        hwa_output_error(error, error_size,
                         "replacement target appeared before commit");
        return -1;
    }
    return 0;
}

static int hwa_commit_temporary_output(const char *temporary_path,
                                       const char *path,
                                       char *error,
                                       size_t error_size)
{
#if defined(_WIN32)
    if (!MoveFileExA(temporary_path, path,
                     MOVEFILE_REPLACE_EXISTING |
                         MOVEFILE_WRITE_THROUGH)) {
        if (error != NULL && error_size > 0U) {
            (void)snprintf(error, error_size,
                           "Windows error %lu",
                           (unsigned long)GetLastError());
        }
        return -1;
    }
#else
    if (rename(temporary_path, path) != 0) {
        if (error != NULL && error_size > 0U) {
            (void)snprintf(error, error_size,
                           "%s", strerror(errno));
        }
        return -1;
    }
#endif
    return 0;
}

static void hwa_file_output_clear(HWAFileOutput *output)
{
    free(output->temporary_path);
    free(output->path);
    memset(output, 0, sizeof(*output));
}

int hwa_file_output_open(HWAFileOutput *output,
                         const char *path,
                         const char *const *protected_paths,
                         size_t protected_path_count,
                         int replace,
                         char *error,
                         size_t error_size)
{
    int descriptor;

    if (error != NULL && error_size > 0U) {
        error[0] = '\0';
    }
    if (output == NULL) {
        hwa_output_error(error, error_size,
                         "file output state is null");
        return -1;
    }
    memset(output, 0, sizeof(*output));
    if (path == NULL || path[0] == '\0') {
        hwa_output_error(error, error_size,
                         "output path is empty");
        return -1;
    }
    if (strcmp(path, "-") == 0) {
        if (replace) {
            hwa_output_error(error, error_size,
                             "cannot replace standard output");
            return -1;
        }
        if (hwa_check_protected_paths(path, protected_paths,
                                      protected_path_count,
                                      error, error_size) != 0) {
            return -1;
        }
        if (hwa_prepare_standard_output(error, error_size) != 0) {
            return -1;
        }
        output->stream = stdout;
        output->standard_output = 1;
        return 0;
    }
    output->path = hwa_output_copy_path(path);
    if (output->path == NULL) {
        hwa_output_error(error, error_size,
                         "out of memory for the output path");
        return -1;
    }
    if (hwa_check_protected_paths(path, protected_paths,
                                  protected_path_count,
                                  error, error_size) != 0) {
        hwa_file_output_abort(output);
        return -1;
    }
    if (replace) {
        int missing = 0;

        if (hwa_read_replacement_identity(
                path, &output->replacement_device,
                &output->replacement_file,
                &output->replacement_mode, &missing,
                error, error_size) != 0) {
            hwa_file_output_abort(output);
            return -1;
        }
        output->replacement_existed = !missing;
        if (hwa_open_temporary_output(output,
                                      error, error_size) != 0) {
            hwa_file_output_abort(output);
            return -1;
        }
        return 0;
    }
    descriptor = hwa_output_descriptor(path);
    if (descriptor < 0) {
        if (error != NULL && error_size > 0U) {
            (void)snprintf(error, error_size,
                           "cannot create output '%s': %s",
                           path, strerror(errno));
        }
        hwa_file_output_abort(output);
        return -1;
    }
    output->direct_path_created = 1;
    if (hwa_record_created_identity(output, descriptor,
                                    error, error_size) != 0) {
        (void)hwa_discard_unidentified_output(descriptor, output->path);
        hwa_file_output_clear(output);
        return -1;
    }
    output->stream = hwa_stream_from_descriptor(descriptor);
    if (output->stream == NULL) {
        int saved_error = errno;

        hwa_close_descriptor(descriptor);
        hwa_file_output_abort(output);
        if (error != NULL && error_size > 0U) {
            (void)snprintf(error, error_size,
                           "cannot open output stream: %s",
                           strerror(saved_error));
        }
        return -1;
    }
    return 0;
}

FILE *hwa_file_output_stream(HWAFileOutput *output)
{
    return output != NULL ? output->stream : NULL;
}

int hwa_file_output_abort(HWAFileOutput *output)
{
    int result = 0;

    if (output == NULL) {
        return 0;
    }
    if (output->stream != NULL && !output->standard_output) {
        (void)fclose(output->stream);
        output->stream = NULL;
    }
    if (output->temporary_path != NULL) {
        result = hwa_remove_created_path(output, output->temporary_path);
    } else if (output->direct_path_created && output->path != NULL) {
        result = hwa_remove_created_path(output, output->path);
    }
    hwa_file_output_clear(output);
    return result;
}

int hwa_file_output_finish(HWAFileOutput *output,
                           const char *output_name,
                           char *error,
                           size_t error_size)
{
    int flush_result;
    int stream_error;
    int close_result = 0;

    if (error != NULL && error_size > 0U) {
        error[0] = '\0';
    }
    if (output == NULL || output->stream == NULL) {
        hwa_output_error(error, error_size,
                         "file output is not open");
        return -1;
    }
    if (output_name == NULL || output_name[0] == '\0') {
        output_name = "output";
    }
    flush_result = fflush(output->stream);
    stream_error = ferror(output->stream);
    if (!output->standard_output) {
        close_result = fclose(output->stream);
        output->stream = NULL;
    }
    if (flush_result != 0 || stream_error || close_result != 0) {
        if (error != NULL && error_size > 0U) {
            (void)snprintf(error, error_size,
                           "cannot write %s", output_name);
        }
        hwa_file_output_abort(output);
        return -1;
    }
    if (output->standard_output) {
        hwa_file_output_clear(output);
        return 0;
    }
    if (output->temporary_path != NULL) {
        if (hwa_created_path_unchanged(output, output->temporary_path,
                                       error, error_size) != 0) {
            hwa_file_output_clear(output);
            return -1;
        }
        if (hwa_replacement_unchanged(output,
                                      error, error_size) != 0 ||
            /* These checks and rename cannot be atomic on every host. */
            hwa_commit_temporary_output(output->temporary_path,
                                        output->path,
                                        error, error_size) != 0) {
            hwa_file_output_abort(output);
            return -1;
        }
    } else if (output->direct_path_created &&
               hwa_created_path_unchanged(output, output->path,
                                          error, error_size) != 0) {
        hwa_file_output_clear(output);
        return -1;
    }
    hwa_file_output_clear(output);
    return 0;
}

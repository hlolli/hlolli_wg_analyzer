#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0601
#endif

#if !defined(_WIN32)
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#endif

#include "experiment_process.h"
#include "sha256.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#ifndef _S_IEXEC
#define _S_IEXEC 0
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <time.h>
#include <unistd.h>
#endif

#if !defined(O_NOFOLLOW)
#define O_NOFOLLOW 0
#endif

static void hwa_process_error(char *error,
                              size_t error_size,
                              const char *message)
{
    if (error != NULL && error_size > 0U) {
        (void)snprintf(error, error_size, "%s", message);
    }
}

static char *hwa_process_copy(const char *text)
{
    size_t size;
    char *copy;

    if (text == NULL) return NULL;
    size = strlen(text);
    if (size == SIZE_MAX) return NULL;
    copy = (char *)malloc(size + 1U);
    if (copy != NULL) memcpy(copy, text, size + 1U);
    return copy;
}

static int hwa_process_absolute_path(const char *path,
                                     char **absolute,
                                     char *error,
                                     size_t error_size)
{
    char resolved[4096];

    if (path == NULL || path[0] == '\0' || strcmp(path, "-") == 0) {
        hwa_process_error(error, error_size, "invalid renderer path");
        return -1;
    }
#if defined(_WIN32)
    if (_fullpath(resolved, path, sizeof(resolved)) == NULL) {
        hwa_process_error(error, error_size, "cannot resolve renderer path");
        return -1;
    }
#else
    if (path[0] == '/') {
        int written = snprintf(resolved, sizeof(resolved), "%s", path);
        if (written < 0 || (size_t)written >= sizeof(resolved)) {
            hwa_process_error(error, error_size, "renderer path is too long");
            return -1;
        }
    } else {
        size_t size;
        if (getcwd(resolved, sizeof(resolved)) == NULL) {
            hwa_process_error(error, error_size,
                              "cannot resolve renderer path");
            return -1;
        }
        size = strlen(resolved);
        if (size >= sizeof(resolved) - 1U || strlen(path) >=
                sizeof(resolved) - size - 1U) {
            hwa_process_error(error, error_size, "renderer path is too long");
            return -1;
        }
        resolved[size++] = '/';
        memcpy(resolved + size, path, strlen(path) + 1U);
    }
#endif
    *absolute = hwa_process_copy(resolved);
    if (*absolute == NULL) {
        hwa_process_error(error, error_size, "cannot retain renderer path");
        return -1;
    }
    return 0;
}

static int hwa_process_private_copy(HWAExperimentProcessRenderer *renderer,
                                    const char *source_path,
                                    char *error,
                                    size_t error_size)
{
    unsigned char buffer[65536];
    unsigned char digest[32];
    HWASha256 hash;
    FILE *source = NULL;
    FILE *destination = NULL;
    char directory[4096] = {0};
    char executable[4096] = {0};
    uint64_t total = 0U;
    int okay = 0;
#if defined(_WIN32)
    HANDLE private_lock = NULL;
    BY_HANDLE_FILE_INFORMATION written_identity;
#endif

#if defined(_WIN32)
    memset(&written_identity, 0, sizeof(written_identity));
    {
        char base[MAX_PATH];
        unsigned attempt;
        DWORD length = GetTempPathA((DWORD)sizeof(base), base);
        if (length == 0U || length >= (DWORD)sizeof(base)) goto cleanup;
        for (attempt = 0U; attempt < 100U; ++attempt) {
            int written = snprintf(directory, sizeof(directory),
                                   "%shwa-stage8-renderer-%lu-%u",
                                   base, (unsigned long)GetCurrentProcessId(),
                                   attempt);
            if (written < 0 || (size_t)written >= sizeof(directory))
                goto cleanup;
            if (CreateDirectoryA(directory, NULL) != 0) break;
            if (GetLastError() != ERROR_ALREADY_EXISTS) goto cleanup;
        }
        if (attempt == 100U) goto cleanup;
    }
#else
    (void)snprintf(directory, sizeof(directory),
                   "/tmp/hwa-stage8-renderer-XXXXXX");
    if (mkdtemp(directory) == NULL) goto cleanup;
#endif
#if defined(_WIN32)
    {
        int written = snprintf(executable, sizeof(executable),
                               "%s\\renderer.exe", directory);
        if (written < 0 || (size_t)written >= sizeof(executable)) goto cleanup;
    }
#else
    {
        int written = snprintf(executable, sizeof(executable),
                               "%s/renderer", directory);
        if (written < 0 || (size_t)written >= sizeof(executable)) goto cleanup;
    }
#endif
#if defined(_WIN32)
    {
        SECURITY_ATTRIBUTES security;
        BY_HANDLE_FILE_INFORMATION before;
        HANDLE handle;
        int descriptor = -1;
        uint64_t size;

        memset(&security, 0, sizeof(security));
        memset(&before, 0, sizeof(before));
        security.nLength = sizeof(security);
        handle = CreateFileA(source_path, GENERIC_READ, FILE_SHARE_READ,
                             &security, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL |
                                 FILE_FLAG_OPEN_REPARSE_POINT,
                             NULL);
        if (handle == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandle(handle, &before) ||
            (before.dwFileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
              FILE_ATTRIBUTE_DEVICE)) != 0U ||
            ((size = ((uint64_t)before.nFileSizeHigh << 32U) |
                     (uint64_t)before.nFileSizeLow) == 0U) ||
            size > renderer->max_executable_bytes) {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            goto cleanup;
        }
        descriptor = _open_osfhandle((intptr_t)handle,
                                    _O_RDONLY | _O_BINARY);
        if (descriptor >= 0) source = _fdopen(descriptor, "rb");
        if (descriptor >= 0 && source == NULL) (void)_close(descriptor);
        if (descriptor < 0) CloseHandle(handle);
        if (source == NULL) goto cleanup;
    }
#else
    {
        int descriptor = open(source_path,
                              O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
        struct stat status;
        if (descriptor >= 0 &&
            (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
             status.st_size <= 0 ||
             (uint64_t)status.st_size > renderer->max_executable_bytes)) {
            (void)close(descriptor);
            descriptor = -1;
        }
        if (descriptor >= 0) {
            int flags = fcntl(descriptor, F_GETFL, 0);
            if (flags < 0 ||
                fcntl(descriptor, F_SETFL, flags & ~O_NONBLOCK) < 0) {
                (void)close(descriptor);
                descriptor = -1;
            }
        }
        if (descriptor >= 0) source = fdopen(descriptor, "rb");
        if (descriptor >= 0 && source == NULL) (void)close(descriptor);
    }
#endif
    if (source == NULL) goto cleanup;
#if defined(_WIN32)
    {
        HANDLE handle = CreateFileA(
            executable, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
            NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        int descriptor = handle == INVALID_HANDLE_VALUE
                             ? -1
                             : _open_osfhandle((intptr_t)handle,
                                              _O_RDWR | _O_BINARY);
        if (descriptor >= 0) destination = _fdopen(descriptor, "wb");
        if (descriptor >= 0 && destination == NULL) (void)_close(descriptor);
        if (descriptor < 0 && handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
#else
    {
        int descriptor = open(executable,
                              O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0700);
        if (descriptor >= 0) destination = fdopen(descriptor, "wb");
        if (descriptor >= 0 && destination == NULL) (void)close(descriptor);
    }
#endif
    if (destination == NULL) goto cleanup;
    hwa_sha256_init(&hash);
    for (;;) {
        size_t count = fread(buffer, 1U, sizeof(buffer), source);
        if (count != 0U) {
            if ((uint64_t)count > renderer->max_executable_bytes - total ||
                fwrite(buffer, 1U, count, destination) != count) goto cleanup;
            total += (uint64_t)count;
            hwa_sha256_update(&hash, buffer, count);
        }
        if (count < sizeof(buffer)) {
            if (ferror(source)) goto cleanup;
            break;
        }
    }
    if (total == 0U || fflush(destination) != 0 || ferror(destination)) {
        goto cleanup;
    }
#if defined(_WIN32)
    if (!GetFileInformationByHandle(
            (HANDLE)_get_osfhandle(_fileno(destination)),
            &written_identity) ||
        (written_identity.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
          FILE_ATTRIBUTE_DEVICE)) != 0U ||
        (((uint64_t)written_identity.nFileSizeHigh << 32U) |
         (uint64_t)written_identity.nFileSizeLow) != total) {
        goto cleanup;
    }
#endif
    if (fclose(destination) != 0) {
        destination = NULL;
        goto cleanup;
    }
    destination = NULL;
#if defined(_WIN32)
    {
        BY_HANDLE_FILE_INFORMATION locked_identity;
        memset(&locked_identity, 0, sizeof(locked_identity));
        private_lock = CreateFileA(
            executable, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (private_lock == INVALID_HANDLE_VALUE) {
            private_lock = NULL;
            goto cleanup;
        }
        if (!GetFileInformationByHandle(private_lock, &locked_identity) ||
            (locked_identity.dwFileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
              FILE_ATTRIBUTE_DEVICE)) != 0U ||
            locked_identity.dwVolumeSerialNumber !=
                written_identity.dwVolumeSerialNumber ||
            locked_identity.nFileIndexHigh != written_identity.nFileIndexHigh ||
            locked_identity.nFileIndexLow != written_identity.nFileIndexLow ||
            locked_identity.nFileSizeHigh != written_identity.nFileSizeHigh ||
            locked_identity.nFileSizeLow != written_identity.nFileSizeLow) {
            goto cleanup;
        }
    }
#endif
    if (fclose(source) != 0) {
        source = NULL;
        goto cleanup;
    }
    source = NULL;
    hwa_sha256_final(&hash, digest);
    hwa_sha256_hex(digest, renderer->sha256);
#if defined(_WIN32)
    renderer->locked_file = private_lock;
    private_lock = NULL;
#endif
    renderer->path = hwa_process_copy(executable);
    renderer->private_directory = hwa_process_copy(directory);
    if (renderer->path == NULL || renderer->private_directory == NULL)
        goto cleanup;
    okay = 1;
cleanup:
    if (source != NULL) (void)fclose(source);
    if (destination != NULL) (void)fclose(destination);
    if (!okay) {
#if defined(_WIN32)
        if (private_lock != NULL) CloseHandle(private_lock);
        if (renderer->locked_file != NULL) {
            CloseHandle((HANDLE)renderer->locked_file);
            renderer->locked_file = NULL;
        }
#endif
        if (executable[0] != '\0') (void)remove(executable);
#if defined(_WIN32)
        if (directory[0] != '\0') (void)RemoveDirectoryA(directory);
#else
        if (directory[0] != '\0') (void)rmdir(directory);
#endif
        hwa_process_error(error, error_size,
                          "cannot bind a private renderer copy");
        return -1;
    }
    return 0;
}

static int hwa_process_regular_file(const char *path,
                                    uint64_t max_bytes,
                                    uint64_t *file_bytes,
                                    char *error,
                                    size_t error_size)
{
#if defined(_WIN32)
    DWORD attributes;
    struct _stat64 status;

    attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY |
                       FILE_ATTRIBUTE_REPARSE_POINT)) != 0U ||
        _stat64(path, &status) != 0 ||
        (status.st_mode & _S_IFMT) != _S_IFREG || status.st_size < 0) {
        hwa_process_error(error, error_size,
                          "renderer is not a named regular file");
        return -1;
    }
    *file_bytes = (uint64_t)status.st_size;
#else
    struct stat status;

    if (lstat(path, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size < 0) {
        hwa_process_error(error, error_size,
                          "renderer is not a named regular file");
        return -1;
    }
    *file_bytes = (uint64_t)status.st_size;
#endif
    if (*file_bytes == 0U || *file_bytes > max_bytes) {
        hwa_process_error(error, error_size,
                          "renderer exceeds the executable byte limit");
        return -1;
    }
    return 0;
}

static int hwa_process_renderer_unchanged(
    const HWAExperimentProcessRenderer *renderer,
    char *error,
    size_t error_size)
{
    char hash[HWA_SHA256_HEX_SIZE];
    uint64_t file_bytes;

    if (renderer == NULL || renderer->path == NULL ||
        hwa_process_regular_file(renderer->path,
                                 renderer->max_executable_bytes,
                                 &file_bytes, error, error_size) != 0 ||
        hwa_sha256_file(renderer->path, renderer->max_executable_bytes,
                        hash, error, error_size) != 0) {
        return -1;
    }
    (void)file_bytes;
    if (strcmp(hash, renderer->sha256) != 0) {
        hwa_process_error(error, error_size,
                          "renderer changed during the experiment");
        return -1;
    }
    return 0;
}

int hwa_experiment_process_renderer_open(
    HWAExperimentProcessRenderer *renderer,
    const char *path,
    uint64_t max_executable_bytes,
    char *error,
    size_t error_size)
{
    uint64_t file_bytes;
    char *absolute = NULL;

    if (error != NULL && error_size > 0U) error[0] = '\0';
    if (renderer == NULL) {
        hwa_process_error(error, error_size, "renderer state is null");
        return -1;
    }
    memset(renderer, 0, sizeof(*renderer));
    renderer->max_executable_bytes = max_executable_bytes;
    if (max_executable_bytes == 0U ||
        hwa_process_absolute_path(path, &absolute, error, error_size) != 0 ||
        hwa_process_regular_file(absolute, max_executable_bytes, &file_bytes,
                                 error, error_size) != 0) {
        free(absolute);
        return -1;
    }
    if (hwa_process_private_copy(renderer, absolute, error, error_size) != 0) {
        free(absolute);
        hwa_experiment_process_renderer_close(renderer);
        return -1;
    }
    free(absolute);
    (void)file_bytes;
    return 0;
}

#if !defined(_WIN32)
static int hwa_process_now_milliseconds(uint64_t *milliseconds)
{
    struct timespec now;

    if (milliseconds == NULL || clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
        (uint64_t)now.tv_sec >
            (UINT64_MAX - (uint64_t)now.tv_nsec / UINT64_C(1000000)) /
                UINT64_C(1000)) return -1;
    *milliseconds = (uint64_t)now.tv_sec * UINT64_C(1000) +
                    (uint64_t)now.tv_nsec / UINT64_C(1000000);
    return 0;
}

static int hwa_process_expected_path(
    const HWAExperimentRenderRequest *request,
    const char *path)
{
    size_t index;

    if (strcmp(path, request->request_path) == 0 ||
        strcmp(path, request->stdout_path) == 0 ||
        strcmp(path, request->stderr_path) == 0) return 1;
    for (index = 0U; index < request->output_count; ++index)
        if (request->outputs[index].path != NULL &&
            strcmp(path, request->outputs[index].path) == 0) return 1;
    return 0;
}

static int hwa_process_directory_bytes(
    const HWAExperimentRenderRequest *request,
    int enforce_names,
    uint64_t *bytes,
    char *error,
    size_t error_size)
{
    DIR *directory;
    struct dirent *entry;
    uint64_t total = 0U;

    directory = opendir(request->job_directory);
    if (directory == NULL) {
        hwa_process_error(error, error_size,
                          "cannot inspect renderer output directory");
        return -1;
    }
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        char *child;
        size_t path_size;
        size_t name_size;
        struct stat status;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;
        path_size = strlen(request->job_directory);
        name_size = strlen(entry->d_name);
        if (path_size > SIZE_MAX - name_size - 2U) {
            (void)closedir(directory);
            hwa_process_error(error, error_size,
                              "renderer output path is too long");
            return -1;
        }
        child = (char *)malloc(path_size + name_size + 2U);
        if (child == NULL) {
            (void)closedir(directory);
            hwa_process_error(error, error_size,
                              "cannot inspect renderer output");
            return -1;
        }
        (void)snprintf(child, path_size + name_size + 2U,
                       "%s/%s", request->job_directory, entry->d_name);
        if (lstat(child, &status) != 0) {
            int saved_errno = errno;
            free(child);
            if (!enforce_names && saved_errno == ENOENT) {
                errno = 0;
                continue;
            }
            (void)closedir(directory);
            hwa_process_error(error, error_size,
                              "renderer created a non-regular or oversized output");
            return -1;
        }
        if ((enforce_names && !hwa_process_expected_path(request, child)) ||
            !S_ISREG(status.st_mode) ||
            status.st_size < 0 ||
            (uint64_t)status.st_size > request->max_output_file_bytes ||
            total > UINT64_MAX - (uint64_t)status.st_size) {
            free(child);
            (void)closedir(directory);
            hwa_process_error(error, error_size,
                              "renderer created a non-regular or oversized output");
            return -1;
        }
        total += (uint64_t)status.st_size;
        free(child);
        errno = 0;
    }
    if (errno != 0 || closedir(directory) != 0) {
        hwa_process_error(error, error_size,
                          "cannot finish inspecting renderer output");
        return -1;
    }
    *bytes = total;
    if (enforce_names) {
        size_t index;
        for (index = 0U; index < request->output_count; ++index) {
            struct stat status;
            if (request->outputs[index].path == NULL ||
                lstat(request->outputs[index].path, &status) != 0 ||
                !S_ISREG(status.st_mode)) {
                hwa_process_error(error, error_size,
                                  "renderer omitted a declared output");
                return -1;
            }
        }
    }
    return 0;
}

static void hwa_process_stop_group(pid_t child)
{
    if (child > 0) {
        (void)kill(-child, SIGKILL);
        (void)kill(child, SIGKILL);
    }
}

static int hwa_process_stop_group_and_wait(pid_t child)
{
    unsigned attempt;
    int status;
    pid_t waited;

    hwa_process_stop_group(child);
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0 && errno != ECHILD) return -1;
    for (attempt = 0U; attempt < 30000U; ++attempt) {
        struct timespec delay;
        /* macOS can report EPERM for a killed orphan while it is reaped. */
        if (kill(-child, 0) != 0)
            return errno == ESRCH || errno == EPERM ? 0 : -1;
        delay.tv_sec = 0;
        delay.tv_nsec = 1000000L;
        (void)nanosleep(&delay, NULL);
    }
    return -1;
}

static void hwa_process_close_child_descriptors(void)
{
#if defined(__APPLE__) && defined(F_CLOSEM)
    (void)fcntl(3, F_CLOSEM, 0);
#else
#if defined(__linux__) && defined(SYS_close_range)
    if (syscall(SYS_close_range, 3U, UINT_MAX, 0U) == 0) return;
#endif
    long configured = sysconf(_SC_OPEN_MAX);
    unsigned long limit = (unsigned long)INT_MAX + 1UL;
    unsigned long descriptor;
    if (configured >= 0L) {
        unsigned long value = (unsigned long)configured;
        if (value < limit) limit = value;
    } else {
        struct rlimit file_limit;
        if (getrlimit(RLIMIT_NOFILE, &file_limit) == 0 &&
            file_limit.rlim_cur != RLIM_INFINITY &&
            file_limit.rlim_cur < (rlim_t)limit)
            limit = (unsigned long)file_limit.rlim_cur;
    }
    for (descriptor = 3UL; descriptor < limit; ++descriptor)
        (void)close((int)descriptor);
#endif
}

static int hwa_process_limit_child_file_size(uint64_t maximum)
{
    struct rlimit current;
    struct rlimit changed;
    rlim_t requested;

    if (getrlimit(RLIMIT_FSIZE, &current) != 0) return -1;
    requested = maximum >= (uint64_t)RLIM_INFINITY
                    ? RLIM_INFINITY
                    : (rlim_t)maximum;
    changed = current;
    if (current.rlim_max != RLIM_INFINITY &&
        (requested == RLIM_INFINITY || requested > current.rlim_max)) {
        requested = current.rlim_max;
    }
    changed.rlim_cur = requested;
    return setrlimit(RLIMIT_FSIZE, &changed);
}
#endif

#if defined(_WIN32)
static int hwa_process_windows_expected_path(
    const HWAExperimentRenderRequest *request,
    const char *path)
{
    size_t index;
    if (_stricmp(path, request->request_path) == 0 ||
        _stricmp(path, request->stdout_path) == 0 ||
        _stricmp(path, request->stderr_path) == 0) return 1;
    for (index = 0U; index < request->output_count; ++index)
        if (request->outputs[index].path != NULL &&
            _stricmp(path, request->outputs[index].path) == 0) return 1;
    return 0;
}

static int hwa_process_windows_directory_bytes(
    const HWAExperimentRenderRequest *request,
    int enforce_names,
    uint64_t *bytes,
    char *error,
    size_t error_size)
{
    WIN32_FIND_DATAA entry;
    HANDLE search;
    char pattern[4096];
    uint64_t total = 0U;
    int written = snprintf(pattern, sizeof(pattern), "%s\\*",
                           request->job_directory);
    if (written < 0 || (size_t)written >= sizeof(pattern))
        goto inspect_failed;
    search = FindFirstFileA(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) goto inspect_failed;
    do {
        char child[4096];
        uint64_t size;
        if (strcmp(entry.cFileName, ".") == 0 ||
            strcmp(entry.cFileName, "..") == 0) continue;
        written = snprintf(child, sizeof(child), "%s\\%s",
                           request->job_directory, entry.cFileName);
        size = ((uint64_t)entry.nFileSizeHigh << 32U) |
               (uint64_t)entry.nFileSizeLow;
        if (written < 0 || (size_t)written >= sizeof(child) ||
            (enforce_names &&
             !hwa_process_windows_expected_path(request, child)) ||
            (entry.dwFileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
              FILE_ATTRIBUTE_DEVICE)) != 0U ||
            size > request->max_output_file_bytes ||
            total > UINT64_MAX - size) {
            (void)FindClose(search);
            goto invalid_output;
        }
        total += size;
    } while (FindNextFileA(search, &entry) != 0);
    if (GetLastError() != ERROR_NO_MORE_FILES || FindClose(search) == 0)
        goto inspect_failed;
    *bytes = total;
    if (enforce_names) {
        size_t index;
        for (index = 0U; index < request->output_count; ++index) {
            DWORD attributes;
            if (request->outputs[index].path == NULL) goto missing_output;
            attributes = GetFileAttributesA(request->outputs[index].path);
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & (FILE_ATTRIBUTE_DIRECTORY |
                               FILE_ATTRIBUTE_REPARSE_POINT |
                               FILE_ATTRIBUTE_DEVICE)) != 0U)
                goto missing_output;
        }
    }
    return 0;
invalid_output:
    hwa_process_error(error, error_size,
                      "renderer created a non-regular or oversized output");
    return -1;
missing_output:
    hwa_process_error(error, error_size,
                      "renderer omitted a declared output");
    return -1;
inspect_failed:
    hwa_process_error(error, error_size,
                      "cannot inspect renderer output directory");
    return -1;
}

static int hwa_process_windows_wait_job_empty(HANDLE job)
{
    ULONGLONG started = GetTickCount64();
    for (;;) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting;
        ULONGLONG now;
        memset(&accounting, 0, sizeof(accounting));
        if (!QueryInformationJobObject(
                job, JobObjectBasicAccountingInformation,
                &accounting, sizeof(accounting), NULL)) return -1;
        if (accounting.ActiveProcesses == 0U) return 0;
        now = GetTickCount64();
        if (now < started || now - started >= UINT64_C(30000)) return -1;
        Sleep(1U);
    }
}

static int hwa_process_windows_render(
    const HWAExperimentProcessRenderer *renderer,
    const HWAExperimentRenderRequest *request,
    char *error,
    size_t error_size)
{
    STARTUPINFOEXA startup;
    PROCESS_INFORMATION process;
    SECURITY_ATTRIBUTES security;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits;
    HANDLE stdout_handle = INVALID_HANDLE_VALUE;
    HANDLE stderr_handle = INVALID_HANDLE_VALUE;
    HANDLE stdin_handle = INVALID_HANDLE_VALUE;
    HANDLE job = NULL;
    HANDLE inherited[3];
    SIZE_T attribute_bytes = 0U;
    char command[32768];
    char empty_environment[2] = {0, 0};
    ULONGLONG started;
    DWORD exit_code = 1U;
    int process_started = 0;
    int result = -1;
    int written;

    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    memset(&security, 0, sizeof(security));
    memset(&job_limits, 0, sizeof(job_limits));
    startup.StartupInfo.cb = sizeof(startup);
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    stdout_handle = CreateFileA(request->stdout_path, GENERIC_WRITE, 0,
                                &security, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    stderr_handle = CreateFileA(request->stderr_path, GENERIC_WRITE, 0,
                                &security, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    stdin_handle = CreateFileA("NUL", GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &security, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    if (stdout_handle == INVALID_HANDLE_VALUE ||
        stderr_handle == INVALID_HANDLE_VALUE ||
        stdin_handle == INVALID_HANDLE_VALUE) {
        hwa_process_error(error, error_size, "cannot create renderer logs");
        goto cleanup;
    }
    inherited[0] = stdin_handle;
    inherited[1] = stdout_handle;
    inherited[2] = stderr_handle;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdin_handle;
    startup.StartupInfo.hStdOutput = stdout_handle;
    startup.StartupInfo.hStdError = stderr_handle;
    (void)InitializeProcThreadAttributeList(NULL, 1U, 0U,
                                            &attribute_bytes);
    if (attribute_bytes == 0U) {
        hwa_process_error(error, error_size,
                          "cannot restrict renderer handles");
        goto cleanup;
    }
    startup.lpAttributeList =
        (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(
            GetProcessHeap(), 0U, attribute_bytes);
    if (startup.lpAttributeList == NULL ||
        !InitializeProcThreadAttributeList(startup.lpAttributeList, 1U, 0U,
                                           &attribute_bytes) ||
        !UpdateProcThreadAttribute(
            startup.lpAttributeList, 0U, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited, sizeof(inherited), NULL, NULL)) {
        hwa_process_error(error, error_size,
                          "cannot restrict renderer handles");
        goto cleanup;
    }
    job = CreateJobObjectA(NULL, NULL);
    job_limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job == NULL || !SetInformationJobObject(
            job, JobObjectExtendedLimitInformation,
            &job_limits, sizeof(job_limits))) {
        hwa_process_error(error, error_size,
                          "cannot create renderer job object");
        goto cleanup;
    }
    written = snprintf(command, sizeof(command),
                       "\"%s\" --hwa-experiment-job \"%s\" "
                       "--output-dir \"%s\"",
                       renderer->path, request->request_path,
                       request->job_directory);
    if (written < 0 || (size_t)written >= sizeof(command) ||
        !CreateProcessA(
            renderer->path, command, NULL, NULL, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED |
                EXTENDED_STARTUPINFO_PRESENT,
            empty_environment, request->job_directory,
            &startup.StartupInfo, &process)) {
        hwa_process_error(error, error_size, "cannot start renderer");
        goto cleanup;
    }
    process_started = 1;
    if (!AssignProcessToJobObject(job, process.hProcess) ||
        ResumeThread(process.hThread) == (DWORD)-1) {
        hwa_process_error(error, error_size,
                          "cannot contain renderer process");
        (void)TerminateProcess(process.hProcess, 124U);
        goto cleanup;
    }
    started = GetTickCount64();
    for (;;) {
        DWORD wait_result = WaitForSingleObject(process.hProcess, 10U);
        uint64_t output_bytes = 0U;
        ULONGLONG now = GetTickCount64();
        if (wait_result == WAIT_OBJECT_0) break;
        if (wait_result == WAIT_FAILED ||
            hwa_process_windows_directory_bytes(
                request, 0, &output_bytes, error, error_size) != 0 ||
            output_bytes > request->max_output_bytes) {
            if (error != NULL && error_size > 0U && error[0] == '\0')
                hwa_process_error(error, error_size,
                                  "renderer exceeded its output byte limit");
            goto cleanup;
        }
        if (now < started || now - started >=
                request->timeout_milliseconds) {
            hwa_process_error(error, error_size, "renderer timed out");
            goto cleanup;
        }
    }
    {
        ULONGLONG finished_at = GetTickCount64();
        if (finished_at < started ||
            finished_at - started >= request->timeout_milliseconds) {
            hwa_process_error(error, error_size, "renderer timed out");
            goto cleanup;
        }
    }
    if (!GetExitCodeProcess(process.hProcess, &exit_code) || exit_code != 0U) {
        char message[96];
        (void)snprintf(message, sizeof(message),
                       "renderer exited with status %lu",
                       (unsigned long)exit_code);
        hwa_process_error(error, error_size, message);
        goto cleanup;
    }
    if (!TerminateJobObject(job, 0U) ||
        hwa_process_windows_wait_job_empty(job) != 0) {
        hwa_process_error(error, error_size,
                          "cannot stop renderer descendants");
        goto cleanup;
    }
    {
        uint64_t output_bytes = 0U;
        if (hwa_process_windows_directory_bytes(
                request, 1, &output_bytes, error, error_size) != 0 ||
            output_bytes > request->max_output_bytes) {
            if (error != NULL && error_size > 0U && error[0] == '\0')
                hwa_process_error(error, error_size,
                                  "renderer exceeded its output byte limit");
            goto cleanup;
        }
    }
    result = 0;
cleanup:
    if (result != 0 && job != NULL) {
        (void)TerminateJobObject(job, 124U);
        (void)hwa_process_windows_wait_job_empty(job);
    }
    if (process_started) {
        DWORD wait_result = WaitForSingleObject(process.hProcess, 30000U);
        if (wait_result != WAIT_OBJECT_0) {
            (void)TerminateProcess(process.hProcess, 124U);
            (void)WaitForSingleObject(process.hProcess, 30000U);
        }
    }
    if (process.hThread != NULL) CloseHandle(process.hThread);
    if (process.hProcess != NULL) CloseHandle(process.hProcess);
    if (job != NULL) CloseHandle(job);
    if (startup.lpAttributeList != NULL) {
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        (void)HeapFree(GetProcessHeap(), 0U, startup.lpAttributeList);
    }
    if (stdin_handle != INVALID_HANDLE_VALUE) CloseHandle(stdin_handle);
    if (stdout_handle != INVALID_HANDLE_VALUE) CloseHandle(stdout_handle);
    if (stderr_handle != INVALID_HANDLE_VALUE) CloseHandle(stderr_handle);
    return result;
}
#endif

int hwa_experiment_process_renderer_render(
    void *context,
    const HWAExperimentRenderRequest *request,
    char *error,
    size_t error_size)
{
    HWAExperimentProcessRenderer *renderer =
        (HWAExperimentProcessRenderer *)context;

    if (error != NULL && error_size > 0U) error[0] = '\0';
    if (renderer == NULL || request == NULL ||
        request->job_directory == NULL || request->request_path == NULL ||
        request->stdout_path == NULL || request->stderr_path == NULL ||
        request->max_output_file_bytes == 0U ||
        request->max_output_bytes == 0U ||
        request->timeout_milliseconds == 0U ||
        hwa_process_renderer_unchanged(renderer, error, error_size) != 0) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_process_error(error, error_size, "invalid renderer request");
        }
        return -1;
    }
#if defined(_WIN32)
    if (hwa_process_windows_render(renderer, request,
                                   error, error_size) != 0) return -1;
#else
    {
        int stdout_descriptor;
        int stderr_descriptor;
        pid_t child;
        int status = 0;
        uint64_t started;
        int finished = 0;

        stdout_descriptor = open(request->stdout_path,
                                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
                                 0600);
        stderr_descriptor = open(request->stderr_path,
                                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
                                 0600);
        if (stdout_descriptor < 0 || stderr_descriptor < 0) {
            if (stdout_descriptor >= 0) (void)close(stdout_descriptor);
            if (stderr_descriptor >= 0) (void)close(stderr_descriptor);
            hwa_process_error(error, error_size, "cannot create renderer logs");
            return -1;
        }
        child = fork();
        if (child == 0) {
            char *const arguments[] = {
                renderer->path,
                (char *)"--hwa-experiment-job",
                (char *)request->request_path,
                (char *)"--output-dir",
                (char *)request->job_directory,
                NULL
            };
            char *const environment[] = {NULL};

            (void)setpgid(0, 0);
            if (dup2(stdout_descriptor, STDOUT_FILENO) < 0 ||
                dup2(stderr_descriptor, STDERR_FILENO) < 0 ||
                close(stdout_descriptor) != 0 ||
                close(stderr_descriptor) != 0 ||
                chdir(request->job_directory) != 0 ||
                hwa_process_limit_child_file_size(
                    request->max_output_file_bytes) != 0) {
                _exit(126);
            }
            hwa_process_close_child_descriptors();
            execve(renderer->path, arguments, environment);
            _exit(127);
        }
        (void)close(stdout_descriptor);
        (void)close(stderr_descriptor);
        if (child < 0) {
            hwa_process_error(error, error_size, "cannot start renderer");
            return -1;
        }
        (void)setpgid(child, child);
        if (hwa_process_now_milliseconds(&started) != 0) {
            hwa_process_stop_group_and_wait(child);
            (void)waitpid(child, &status, 0);
            hwa_process_error(error, error_size,
                              "cannot read renderer timeout clock");
            return -1;
        }
        while (!finished) {
            pid_t waited = waitpid(child, &status, WNOHANG);
            uint64_t output_bytes = 0U;
            uint64_t now;

            if (waited == child) {
                finished = 1;
                break;
            }
            if (waited < 0) {
                hwa_process_stop_group_and_wait(child);
                (void)waitpid(child, &status, 0);
                hwa_process_error(error, error_size,
                                  "cannot wait for renderer");
                return -1;
            }
            if (hwa_process_now_milliseconds(&now) != 0) {
                hwa_process_stop_group_and_wait(child);
                (void)waitpid(child, &status, 0);
                hwa_process_error(error, error_size,
                                  "cannot read renderer timeout clock");
                return -1;
            }
            if (hwa_process_directory_bytes(request, 0, &output_bytes,
                                            error, error_size) != 0 ||
                output_bytes > request->max_output_bytes) {
                hwa_process_stop_group_and_wait(child);
                (void)waitpid(child, &status, 0);
                if (error != NULL && error_size > 0U && error[0] == '\0') {
                    hwa_process_error(error, error_size,
                                      "renderer exceeded its output byte limit");
                }
                return -1;
            }
            if (now < started ||
                now - started >= request->timeout_milliseconds) {
                hwa_process_stop_group_and_wait(child);
                (void)waitpid(child, &status, 0);
                hwa_process_error(error, error_size, "renderer timed out");
                return -1;
            }
            {
                struct timespec delay;
                delay.tv_sec = 0;
                delay.tv_nsec = 10000000L;
                (void)nanosleep(&delay, NULL);
            }
        }
        {
            uint64_t finished_at;
            if (hwa_process_now_milliseconds(&finished_at) != 0 ||
                finished_at < started ||
                finished_at - started >= request->timeout_milliseconds) {
                hwa_process_stop_group_and_wait(child);
                hwa_process_error(error, error_size, "renderer timed out");
                return -1;
            }
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            char message[96];
            hwa_process_stop_group_and_wait(child);
            if (WIFEXITED(status))
                (void)snprintf(message, sizeof(message),
                               "renderer exited with status %d",
                               WEXITSTATUS(status));
            else
                (void)snprintf(message, sizeof(message),
                               "renderer stopped by signal %d",
                               WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            hwa_process_error(error, error_size, message);
            return -1;
        }
        if (hwa_process_stop_group_and_wait(child) != 0) {
            hwa_process_error(error, error_size,
                              "cannot stop renderer descendants");
            return -1;
        }
        {
            uint64_t output_bytes = 0U;
            if (hwa_process_directory_bytes(request, 1, &output_bytes,
                                            error, error_size) != 0 ||
                output_bytes > request->max_output_bytes) {
                if (error != NULL && error_size > 0U && error[0] == '\0')
                    hwa_process_error(
                        error, error_size,
                        "renderer exceeded its output byte limit");
                return -1;
            }
        }
    }
#endif
    if (hwa_process_renderer_unchanged(renderer, error, error_size) != 0) {
        return -1;
    }
    return 0;
}

void hwa_experiment_process_renderer_close(
    HWAExperimentProcessRenderer *renderer)
{
    if (renderer == NULL) return;
#if defined(_WIN32)
    if (renderer->locked_file != NULL) {
        CloseHandle((HANDLE)renderer->locked_file);
        renderer->locked_file = NULL;
    }
#endif
    if (renderer->path != NULL) (void)remove(renderer->path);
    if (renderer->private_directory != NULL) {
#if defined(_WIN32)
        (void)RemoveDirectoryA(renderer->private_directory);
#else
        (void)rmdir(renderer->private_directory);
#endif
    }
    free(renderer->path);
    free(renderer->private_directory);
    memset(renderer, 0, sizeof(*renderer));
}

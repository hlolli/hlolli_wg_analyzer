#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "fuzz_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

int hwa_fuzz_read_at(void *context,
                     uint64_t offset,
                     unsigned char *destination,
                     size_t size)
{
    const HWAFuzzBytes *bytes = (const HWAFuzzBytes *)context;
    if (bytes == NULL || destination == NULL ||
        offset > (uint64_t)bytes->size ||
        size > bytes->size - (size_t)offset)
        return -1;
    if (size != 0U)
        memcpy(destination, bytes->data + (size_t)offset, size);
    return 0;
}

int hwa_fuzz_write_temp(const unsigned char *data,
                        size_t size,
                        HWAFuzzTempFile *temporary)
{
    if (temporary == NULL || (size != 0U && data == NULL) ||
        (uint64_t)size > HWA_FUZZ_MAX_INPUT_BYTES)
        return -1;
    memset(temporary, 0, sizeof(*temporary));
#if defined(_WIN32)
    {
        char directory[HWA_FUZZ_PATH_CAP];
        HANDLE file;
        DWORD written;
        if (GetTempPathA((DWORD)sizeof(directory), directory) == 0U ||
            GetTempFileNameA(directory, "hwa", 0U, temporary->path) == 0U)
            return -1;
        file = CreateFileA(temporary->path, GENERIC_WRITE, 0U, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            DeleteFileA(temporary->path);
            temporary->path[0] = '\0';
            return -1;
        }
        if (size > 0U &&
            (size > (size_t)UINT32_MAX ||
             !WriteFile(file, data, (DWORD)size, &written, NULL) ||
             written != (DWORD)size)) {
            CloseHandle(file);
            DeleteFileA(temporary->path);
            temporary->path[0] = '\0';
            return -1;
        }
        if (!CloseHandle(file)) {
            DeleteFileA(temporary->path);
            temporary->path[0] = '\0';
            return -1;
        }
        temporary->active = 1;
    }
#else
    {
        const char *directory = getenv("TMPDIR");
        int descriptor;
        size_t written = 0U;
        if (directory == NULL || directory[0] == '\0') directory = "/tmp";
        if (snprintf(temporary->path, sizeof(temporary->path),
                     "%s/hwa-fuzz-XXXXXX", directory) >=
            (int)sizeof(temporary->path))
            return -1;
        descriptor = mkstemp(temporary->path);
        if (descriptor < 0) {
            temporary->path[0] = '\0';
            return -1;
        }
        while (written < size) {
            ssize_t done = write(descriptor, data + written, size - written);
            if (done <= 0) {
                close(descriptor);
                unlink(temporary->path);
                temporary->path[0] = '\0';
                return -1;
            }
            written += (size_t)done;
        }
        if (close(descriptor) != 0) {
            unlink(temporary->path);
            temporary->path[0] = '\0';
            return -1;
        }
        temporary->active = 1;
    }
#endif
    return 0;
}

void hwa_fuzz_remove_temp(HWAFuzzTempFile *temporary)
{
    if (temporary == NULL || !temporary->active) return;
#if defined(_WIN32)
    DeleteFileA(temporary->path);
#else
    unlink(temporary->path);
#endif
    memset(temporary, 0, sizeof(*temporary));
}

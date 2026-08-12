#ifndef HWA_WINDOWS_FILE_IDENTITY_H
#define HWA_WINDOWS_FILE_IDENTITY_H

#if defined(_WIN32)

#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <windows.h>

typedef struct HWAWindowsFileIdentity {
    uint64_t device;
    uint64_t file;
    uint64_t size;
} HWAWindowsFileIdentity;

static inline int hwa_windows_identity_from_information(
    const BY_HANDLE_FILE_INFORMATION *information,
    HWAWindowsFileIdentity *identity)
{
    if (information == NULL || identity == NULL ||
        (information->dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        return -1;
    }
    identity->device = (uint64_t)information->dwVolumeSerialNumber;
    identity->file = ((uint64_t)information->nFileIndexHigh << 32U) |
                     (uint64_t)information->nFileIndexLow;
    identity->size = ((uint64_t)information->nFileSizeHigh << 32U) |
                     (uint64_t)information->nFileSizeLow;
    return 0;
}

static inline int hwa_windows_identity_from_path(
    const char *path,
    HWAWindowsFileIdentity *identity)
{
    BY_HANDLE_FILE_INFORMATION information;
    HANDLE handle;
    int result;
    if (path == NULL || identity == NULL) return -1;
    /* Open the directory entry itself so a reparse point cannot hide here. */
    handle = CreateFileA(
        path, 0U, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE) return -1;
    result = GetFileType(handle) == FILE_TYPE_DISK &&
             GetFileInformationByHandle(handle, &information)
                 ? hwa_windows_identity_from_information(&information,
                                                         identity)
                 : -1;
    (void)CloseHandle(handle);
    return result;
}

static inline int hwa_windows_identity_from_stream(
    FILE *stream,
    HWAWindowsFileIdentity *identity)
{
    BY_HANDLE_FILE_INFORMATION information;
    int descriptor;
    intptr_t raw;
    if (stream == NULL || identity == NULL) return -1;
    descriptor = _fileno(stream);
    if (descriptor < 0) return -1;
    raw = _get_osfhandle(descriptor);
    /* fopen follows links; reject a reparse-backed handle after the open. */
    if (raw == (intptr_t)-1 || GetFileType((HANDLE)raw) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle((HANDLE)raw, &information)) {
        return -1;
    }
    return hwa_windows_identity_from_information(&information, identity);
}

static inline int hwa_windows_identity_equal(
    const HWAWindowsFileIdentity *left,
    const HWAWindowsFileIdentity *right)
{
    return left != NULL && right != NULL &&
           left->device == right->device && left->file == right->file &&
           left->size == right->size;
}

#endif

#endif

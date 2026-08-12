#if !defined(_WIN32)
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define HWA_STDIN_BLOCK_SIZE 65536U

typedef struct HWAFileByteSource {
    FILE *file;
} HWAFileByteSource;

static int hwa_file_seek(FILE *file, uint64_t offset)
{
    if (offset > (uint64_t)INT64_MAX) return -1;
#if defined(_WIN32)
    return _fseeki64(file, (__int64)offset, SEEK_SET);
#else
    if (sizeof(off_t) < 8U && offset > (uint64_t)INT32_MAX) return -1;
    return fseeko(file, (off_t)offset, SEEK_SET);
#endif
}

static int hwa_file_size(FILE *file, uint64_t *size)
{
#if defined(_WIN32)
    __int64 position;
    if (_fseeki64(file, 0, SEEK_END) != 0) return -1;
    position = _ftelli64(file);
#else
    off_t position;
    if (fseeko(file, (off_t)0, SEEK_END) != 0) return -1;
    position = ftello(file);
#endif
    if (position < 0) return -1;
    *size = (uint64_t)position;
    return 0;
}

static int hwa_file_read_at(void *context,
                            uint64_t offset,
                            unsigned char *destination,
                            size_t size)
{
    HWAFileByteSource *source = (HWAFileByteSource *)context;
    if (source == NULL || source->file == NULL ||
        (size != 0U && destination == NULL) ||
        hwa_file_seek(source->file, offset) != 0) return -1;
    return size == 0U || fread(destination, 1U, size, source->file) == size
               ? 0 : -1;
}

static void hwa_file_source_close(void *context)
{
    HWAFileByteSource *source = (HWAFileByteSource *)context;
    if (source != NULL) {
        if (source->file != NULL) (void)fclose(source->file);
        free(source);
    }
}

static FILE *hwa_open_regular_readonly(const char *path,
                                       char *error,
                                       size_t error_size)
{
    int descriptor;
    FILE *file;
#if defined(_WIN32)
    struct _stat64 status;
    descriptor = _open(path, _O_RDONLY | _O_BINARY | _O_NOINHERIT);
    if (descriptor < 0 || _fstat64(descriptor, &status) != 0 ||
        (status.st_mode & _S_IFMT) != _S_IFREG) {
        if (descriptor >= 0) (void)_close(descriptor);
#else
    struct stat status;
    int flags = O_RDONLY | O_NONBLOCK;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
    descriptor = open(path, flags);
    if (descriptor < 0 || fstat(descriptor, &status) != 0 ||
        !S_ISREG(status.st_mode)) {
        if (descriptor >= 0) (void)close(descriptor);
#endif
        hwa_set_error(error, error_size,
                      "cannot open '%s' as a regular read-only file: %s",
                      path, strerror(errno));
        return NULL;
    }
#if defined(_WIN32)
    file = _fdopen(descriptor, "rb");
#else
    file = fdopen(descriptor, "rb");
#endif
    if (file == NULL) {
#if defined(_WIN32)
        (void)_close(descriptor);
#else
        (void)close(descriptor);
#endif
        hwa_set_error(error, error_size,
                      "cannot create a read stream for '%s': %s",
                      path, strerror(errno));
    }
    return file;
}

static FILE *hwa_spool_stdin(uint64_t max_input_bytes,
                             char *error,
                             size_t error_size)
{
    unsigned char buffer[HWA_STDIN_BLOCK_SIZE];
    uint64_t total = 0U;
    FILE *file;
    if (max_input_bytes == 0U) return NULL;
#if defined(_WIN32)
    if (_setmode(_fileno(stdin), _O_BINARY) == -1) return NULL;
#endif
    file = tmpfile();
    if (file == NULL) return NULL;
    while (total < max_input_bytes) {
        uint64_t remaining = max_input_bytes - total;
        size_t request = remaining < sizeof(buffer)
                             ? (size_t)remaining : sizeof(buffer);
        size_t count = fread(buffer, 1U, request, stdin);
        if (count != 0U && fwrite(buffer, 1U, count, file) != count) goto failed;
        total += (uint64_t)count;
        if (count < request) {
            if (ferror(stdin)) goto failed;
            if (feof(stdin)) break;
            goto failed;
        }
    }
    if (total == max_input_bytes) {
        int extra = fgetc(stdin);
        if (extra != EOF || ferror(stdin)) goto failed;
    }
    if (fflush(file) != 0 || hwa_file_seek(file, 0U) != 0) goto failed;
    return file;
failed:
    (void)fclose(file);
    hwa_set_error(error, error_size, "cannot spool bounded standard input");
    return NULL;
}

int hwa_wav_reader_open_file(HWAWavReader *reader,
                             FILE *file,
                             uint64_t max_input_bytes,
                             char *error,
                             size_t error_size)
{
    HWAFileByteSource *context;
    HWAByteSource source;
    uint64_t size;
    int status;
    if (reader == NULL || file == NULL || hwa_file_size(file, &size) != 0) {
        if (file != NULL) (void)fclose(file);
        hwa_set_error(error, error_size, "cannot determine WAVE input size");
        return -1;
    }
    context = (HWAFileByteSource *)malloc(sizeof(*context));
    if (context == NULL) {
        (void)fclose(file);
        hwa_set_error(error, error_size, "out of memory for WAVE input");
        return -1;
    }
    context->file = file;
    source.context = context;
    source.name = "";
    source.size = size;
    source.read_at = hwa_file_read_at;
    status = hwa_wav_reader_open_source(reader, &source, max_input_bytes,
                                        error, error_size);
    if (status != 0) {
        hwa_file_source_close(context);
        return -1;
    }
    reader->file = file;
    reader->close_context = hwa_file_source_close;
    return 0;
}

int hwa_wav_reader_open(HWAWavReader *reader,
                        const char *path,
                        uint64_t max_input_bytes,
                        char *error,
                        size_t error_size)
{
    FILE *file;
    if (error != NULL && error_size > 0U) error[0] = '\0';
    if (reader == NULL || path == NULL) {
        hwa_set_error(error, error_size, "invalid WAVE reader arguments");
        return -1;
    }
    file = strcmp(path, "-") == 0
               ? hwa_spool_stdin(max_input_bytes, error, error_size)
               : hwa_open_regular_readonly(path, error, error_size);
    if (file == NULL) return -1;
    return hwa_wav_reader_open_file(reader, file, max_input_bytes,
                                    error, error_size);
}

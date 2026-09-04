#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "sha256.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#else
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

static long process_id(void)
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

static int make_workspace(char directory[PATH_MAX], char path[PATH_MAX])
{
    unsigned attempt;
#if defined(_WIN32)
    const char *root = getenv("TEMP");
    if (root == NULL || root[0] == '\0') {
        root = ".";
    }
#else
    const char *root = "/tmp";
#endif

    for (attempt = 0U; attempt < 100U; ++attempt) {
        int length = snprintf(directory, PATH_MAX,
                              "%s/hwa-sha-test-%ld-%u",
                              root, process_id(), attempt);
        if (length < 0 || length >= PATH_MAX) {
            return 0;
        }
        if (make_directory(directory) == 0) {
            length = snprintf(path, PATH_MAX, "%s/input.bin", directory);
            return length >= 0 && length < PATH_MAX;
        }
    }
    return 0;
}

static int write_bytes(const char *path, const void *data, size_t size)
{
    FILE *stream = fopen(path, "wb");
    int ok = stream != NULL && fwrite(data, 1U, size, stream) == size;

    if (stream != NULL && fclose(stream) != 0) {
        ok = 0;
    }
    return ok;
}

static void digest_chunks(const unsigned char *data,
                          size_t size,
                          const size_t *chunks,
                          size_t chunk_count,
                          char hex[65])
{
    HWASha256 context;
    unsigned char digest[32];
    size_t position = 0U;
    size_t chunk_index = 0U;

    hwa_sha256_init(&context);
    while (position < size) {
        size_t count = chunks[chunk_index % chunk_count];
        if (count > size - position) {
            count = size - position;
        }
        hwa_sha256_update(&context, data + position, count);
        position += count;
        chunk_index++;
    }
    hwa_sha256_update(&context, NULL, 0U);
    hwa_sha256_final(&context, digest);
    hwa_sha256_hex(digest, hex);
}

static void check_vector(const char *text, const char *expected)
{
    static const size_t all_at_once[] = {SIZE_MAX};
    static const size_t one_byte[] = {1U};
    static const size_t split[] = {2U, 7U, 31U, 3U, 64U, 5U};
    char first[65];
    char second[65];
    char third[65];

    digest_chunks((const unsigned char *)text, strlen(text),
                  all_at_once, 1U, first);
    digest_chunks((const unsigned char *)text, strlen(text),
                  one_byte, 1U, second);
    digest_chunks((const unsigned char *)text, strlen(text),
                  split, sizeof(split) / sizeof(split[0]), third);
    CHECK(strcmp(first, expected) == 0,
          "SHA-256 vector mismatch: got %s", first);
    CHECK(strcmp(second, expected) == 0 && strcmp(third, expected) == 0,
          "SHA-256 changed with chunk splits");
}

static void test_known_vectors(void)
{
    check_vector("",
                 "e3b0c44298fc1c149afbf4c8996fb924"
                 "27ae41e4649b934ca495991b7852b855");
    check_vector("abc",
                 "ba7816bf8f01cfea414140de5dae2223"
                 "b00361a396177a9cb410ff61f20015ad");
    check_vector("abcdbcdecdefdefgefghfghighijhijk"
                 "ijkljklmklmnlmnomnopnopq",
                 "248d6a61d20638b8e5c026930c3e6039"
                 "a33ce45964ff2167f6ecedd419db06c1");
}

static void test_file_hash(const char *path)
{
    static const char payload[] = "abc";
    static const char expected[] =
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad";
    char hex[65];
    char error[256];
    unsigned char *bytes = NULL;
    size_t byte_count = 0U;

    CHECK(write_bytes(path, payload, sizeof(payload) - 1U),
          "could not write SHA file fixture");
    CHECK(hwa_sha256_file(path, 3U, hex, error, sizeof(error)) == 0 &&
              strcmp(hex, expected) == 0,
          "file SHA-256 failed: %s", error);
    CHECK(hwa_sha256_file(path, 2U, hex, error, sizeof(error)) != 0 &&
              strstr(error, "byte limit") != NULL,
          "one byte below the hash cap passed: %s", error);
    CHECK(hwa_sha256_file("-", 3U, hex, error, sizeof(error)) != 0 &&
              strstr(error, "named input") != NULL,
          "standard input passed named-file hashing: %s", error);
    CHECK(hwa_sha256_read_file(
              path, 3U, &bytes, &byte_count, hex,
              error, sizeof(error)) == 0 &&
              byte_count == 3U && memcmp(bytes, payload, 3U) == 0 &&
              strcmp(hex, expected) == 0,
          "bounded file read and hash failed: %s", error);
    free(bytes);
    bytes = (unsigned char *)1;
    byte_count = 99U;
    CHECK(hwa_sha256_read_file(
              path, 2U, &bytes, &byte_count, hex,
              error, sizeof(error)) != 0 &&
              bytes == NULL && byte_count == 0U,
          "bounded file read ignored its cap: %s", error);
}

#if !defined(_WIN32)
static void test_named_file_types(const char *directory, const char *path)
{
    static const char payload[] = "abc";
    static const char expected[] =
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad";
    char symlink_path[PATH_MAX];
    char fifo_path[PATH_MAX];
    char hex[65];
    char error[256];
    int length;

    length = snprintf(symlink_path, sizeof(symlink_path),
                      "%s/input-link.bin", directory);
    CHECK(length >= 0 && (size_t)length < sizeof(symlink_path),
          "SHA symlink path is too long");
    length = snprintf(fifo_path, sizeof(fifo_path), "%s/input.fifo", directory);
    CHECK(length >= 0 && (size_t)length < sizeof(fifo_path),
          "SHA FIFO path is too long");
    CHECK(write_bytes(path, payload, sizeof(payload) - 1U),
          "could not write SHA symlink target");
    CHECK(symlink(path, symlink_path) == 0,
          "could not make SHA symlink fixture");
    CHECK(hwa_sha256_file(symlink_path, 3U, hex, error, sizeof(error)) == 0 &&
              strcmp(hex, expected) == 0,
          "symlink to a regular hash input failed: %s", error);
    CHECK(mkfifo(fifo_path, 0600) == 0,
          "could not make SHA FIFO fixture");
    CHECK(hwa_sha256_file(fifo_path, 100U, hex, error, sizeof(error)) != 0 &&
              strstr(error, "regular file") != NULL,
          "SHA FIFO was not rejected before open: %s", error);
    (void)remove_file(fifo_path);
    (void)remove_file(symlink_path);
}
#endif

int main(void)
{
    char directory[PATH_MAX];
    char path[PATH_MAX];

    CHECK(make_workspace(directory, path),
          "could not make SHA test workspace");
    if (failures == 0) {
        test_known_vectors();
        test_file_hash(path);
#if !defined(_WIN32)
        test_named_file_types(directory, path);
#endif
    }
    (void)remove_file(path);
    (void)remove_directory(directory);
    if (failures != 0) {
        (void)fprintf(stderr, "%d SHA-256 test(s) failed\n", failures);
        return 1;
    }
    return 0;
}

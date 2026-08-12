#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "sha256.h"

#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <sys/stat.h>
#include "windows_file_identity.h"
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

static const uint32_t hwa_sha256_round_constants[64] = {
    UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf),
    UINT32_C(0xe9b5dba5), UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
    UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5), UINT32_C(0xd807aa98),
    UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
    UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7),
    UINT32_C(0xc19bf174), UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
    UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc), UINT32_C(0x2de92c6f),
    UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
    UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8),
    UINT32_C(0xbf597fc7), UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
    UINT32_C(0x06ca6351), UINT32_C(0x14292967), UINT32_C(0x27b70a85),
    UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
    UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e),
    UINT32_C(0x92722c85), UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
    UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3), UINT32_C(0xd192e819),
    UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
    UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c),
    UINT32_C(0x34b0bcb5), UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
    UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3), UINT32_C(0x748f82ee),
    UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
    UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7),
    UINT32_C(0xc67178f2)
};

static uint32_t hwa_sha256_rotr(uint32_t value, unsigned shift)
{
    return (value >> shift) | (value << (32U - shift));
}

static uint32_t hwa_sha256_load_be32(const unsigned char *bytes)
{
    return ((uint32_t)bytes[0] << 24U) |
           ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) |
           (uint32_t)bytes[3];
}

static void hwa_sha256_transform(HWASha256 *context,
                                 const unsigned char block[64])
{
    uint32_t words[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    size_t index;

    for (index = 0U; index < 16U; ++index) {
        words[index] = hwa_sha256_load_be32(block + index * 4U);
    }
    for (index = 16U; index < 64U; ++index) {
        uint32_t first = words[index - 15U];
        uint32_t second = words[index - 2U];
        uint32_t sigma0 = hwa_sha256_rotr(first, 7U) ^
                          hwa_sha256_rotr(first, 18U) ^ (first >> 3U);
        uint32_t sigma1 = hwa_sha256_rotr(second, 17U) ^
                          hwa_sha256_rotr(second, 19U) ^ (second >> 10U);
        words[index] = words[index - 16U] + sigma0 + words[index - 7U] + sigma1;
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];
    for (index = 0U; index < 64U; ++index) {
        uint32_t sum1 = hwa_sha256_rotr(e, 6U) ^
                        hwa_sha256_rotr(e, 11U) ^ hwa_sha256_rotr(e, 25U);
        uint32_t choose = (e & f) ^ ((~e) & g);
        uint32_t temporary1 = h + sum1 + choose +
                              hwa_sha256_round_constants[index] + words[index];
        uint32_t sum0 = hwa_sha256_rotr(a, 2U) ^
                        hwa_sha256_rotr(a, 13U) ^ hwa_sha256_rotr(a, 22U);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temporary2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

void hwa_sha256_init(HWASha256 *context)
{
    if (context == NULL) {
        return;
    }
    context->state[0] = UINT32_C(0x6a09e667);
    context->state[1] = UINT32_C(0xbb67ae85);
    context->state[2] = UINT32_C(0x3c6ef372);
    context->state[3] = UINT32_C(0xa54ff53a);
    context->state[4] = UINT32_C(0x510e527f);
    context->state[5] = UINT32_C(0x9b05688c);
    context->state[6] = UINT32_C(0x1f83d9ab);
    context->state[7] = UINT32_C(0x5be0cd19);
    context->byte_count = 0U;
    context->block_size = 0U;
    context->overflowed = 0;
}

void hwa_sha256_update(HWASha256 *context,
                       const unsigned char *data,
                       size_t size)
{
    size_t consumed = 0U;

    if (context == NULL || (data == NULL && size != 0U)) {
        return;
    }
    if ((uint64_t)size > (UINT64_MAX / UINT64_C(8)) - context->byte_count) {
        context->overflowed = 1;
        return;
    }
    context->byte_count += (uint64_t)size;
    if (context->block_size != 0U) {
        size_t available = 64U - context->block_size;
        size_t take = size < available ? size : available;
        memcpy(context->block + context->block_size, data, take);
        context->block_size += take;
        consumed += take;
        if (context->block_size == 64U) {
            hwa_sha256_transform(context, context->block);
            context->block_size = 0U;
        }
    }
    while (size - consumed >= 64U) {
        hwa_sha256_transform(context, data + consumed);
        consumed += 64U;
    }
    if (consumed < size) {
        context->block_size = size - consumed;
        memcpy(context->block, data + consumed, context->block_size);
    }
}

void hwa_sha256_final(HWASha256 *context, unsigned char digest[32])
{
    uint64_t bit_count;
    size_t index;

    if (context == NULL || digest == NULL) {
        return;
    }
    if (context->overflowed) {
        memset(digest, 0, 32U);
        memset(context, 0, sizeof(*context));
        return;
    }
    bit_count = context->byte_count * UINT64_C(8);
    context->block[context->block_size++] = 0x80U;
    if (context->block_size > 56U) {
        memset(context->block + context->block_size,
               0, 64U - context->block_size);
        hwa_sha256_transform(context, context->block);
        context->block_size = 0U;
    }
    memset(context->block + context->block_size, 0, 56U - context->block_size);
    for (index = 0U; index < 8U; ++index) {
        context->block[63U - index] = (unsigned char)(bit_count & UINT64_C(0xff));
        bit_count >>= 8U;
    }
    hwa_sha256_transform(context, context->block);
    for (index = 0U; index < 8U; ++index) {
        uint32_t word = context->state[index];
        digest[index * 4U] = (unsigned char)(word >> 24U);
        digest[index * 4U + 1U] = (unsigned char)(word >> 16U);
        digest[index * 4U + 2U] = (unsigned char)(word >> 8U);
        digest[index * 4U + 3U] = (unsigned char)word;
    }
    memset(context, 0, sizeof(*context));
}

void hwa_sha256_hex(const unsigned char digest[32], char hex[65])
{
    static const char digits[] = "0123456789abcdef";
    size_t index;

    if (digest == NULL || hex == NULL) {
        return;
    }
    for (index = 0U; index < 32U; ++index) {
        hex[index * 2U] = digits[digest[index] >> 4U];
        hex[index * 2U + 1U] = digits[digest[index] & 0x0fU];
    }
    hex[64] = '\0';
}

typedef struct HWASha256FileIdentity {
    uint64_t size;
#if defined(_WIN32)
    uint64_t device;
    uint64_t inode;
#else
    dev_t device;
    ino_t inode;
#endif
} HWASha256FileIdentity;

static int hwa_sha256_preflight_file(const char *path,
                                     uint64_t max_bytes,
                                     HWASha256FileIdentity *identity,
                                     char *error,
                                     size_t error_size)
{
#if defined(_WIN32)
    HWAWindowsFileIdentity status;
    if (hwa_windows_identity_from_path(path, &status) != 0) {
        hwa_set_error(error, error_size, "cannot inspect hash input '%s'", path);
#else
    struct stat status;
    if (stat(path, &status) != 0) {
        hwa_set_error(error, error_size, "cannot inspect hash input '%s': %s",
                      path,
                      strerror(errno));
#endif
        return -1;
    }
#if !defined(_WIN32)
    if (!S_ISREG(status.st_mode)) {
        hwa_set_error(error, error_size, "hash input is not a regular file");
        return -1;
    }
    if (status.st_size < 0) {
        hwa_set_error(error, error_size, "hash input has a negative size");
        return -1;
    }
    identity->size = (uint64_t)status.st_size;
    identity->device = status.st_dev;
    identity->inode = status.st_ino;
#else
    identity->size = status.size;
    identity->device = status.device;
    identity->inode = status.file;
#endif
    if (identity->size > max_bytes ||
        identity->size > UINT64_MAX / UINT64_C(8)) {
        hwa_set_error(error, error_size,
                      "hash input exceeds the byte limit");
        return -1;
    }
    return 0;
}

static int hwa_sha256_verify_open_file(FILE *stream,
                                       const HWASha256FileIdentity *identity,
                                       char *error,
                                       size_t error_size)
{
#if defined(_WIN32)
    HWAWindowsFileIdentity status;
    if (hwa_windows_identity_from_stream(stream, &status) != 0) {
        hwa_set_error(error, error_size, "cannot inspect open hash input");
#else
    struct stat status;
    int descriptor = fileno(stream);
    if (descriptor < 0 || fstat(descriptor, &status) != 0) {
        hwa_set_error(error, error_size, "cannot inspect open hash input: %s",
                      strerror(errno));
#endif
        return -1;
    }
#if !defined(_WIN32)
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        hwa_set_error(error, error_size, "hash input is not a regular file");
        return -1;
    }
    if (status.st_dev != identity->device || status.st_ino != identity->inode ||
        (uint64_t)status.st_size != identity->size) {
#else
    {
        HWAWindowsFileIdentity expected;
        expected.device = identity->device;
        expected.file = identity->inode;
        expected.size = identity->size;
        if (!hwa_windows_identity_equal(&status, &expected)) {
#endif
        hwa_set_error(error, error_size,
                      "hash input changed before it was opened");
        return -1;
    }
#if defined(_WIN32)
    }
#endif
    return 0;
}

int hwa_sha256_file(const char *path,
                    uint64_t max_bytes,
                    char hex[65],
                    char *error,
                    size_t error_size)
{
    unsigned char buffer[65536];
    unsigned char digest[32];
    HWASha256 context;
    HWASha256FileIdentity identity;
    uint64_t total = 0U;
    FILE *stream;
    int result = -1;

    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (path == NULL || hex == NULL || max_bytes == 0U) {
        hwa_set_error(error, error_size, "invalid hash arguments");
        return -1;
    }
    if (strcmp(path, "-") == 0) {
        hwa_set_error(error, error_size,
                      "standard input cannot be hashed as a named input");
        return -1;
    }
    if (max_bytes > UINT64_MAX / UINT64_C(8)) {
        max_bytes = UINT64_MAX / UINT64_C(8);
    }
    if (hwa_sha256_preflight_file(path, max_bytes, &identity,
                                  error, error_size) != 0) {
        return -1;
    }
    stream = fopen(path, "rb");
    if (stream == NULL) {
        hwa_set_error(error, error_size, "cannot open hash input '%s': %s",
                      path, strerror(errno));
        return -1;
    }
    if (hwa_sha256_verify_open_file(stream, &identity,
                                    error, error_size) != 0) {
        (void)fclose(stream);
        return -1;
    }
    hwa_sha256_init(&context);
    for (;;) {
        size_t count = fread(buffer, 1U, sizeof(buffer), stream);
        if (count != 0U) {
            if ((uint64_t)count > max_bytes - total) {
                hwa_set_error(error, error_size,
                              "hash input exceeds the byte limit");
                break;
            }
            total += (uint64_t)count;
            hwa_sha256_update(&context, buffer, count);
        }
        if (count < sizeof(buffer)) {
            if (ferror(stream)) {
                hwa_set_error(error, error_size,
                              "cannot read hash input '%s'", path);
            } else if (total != identity.size) {
                hwa_set_error(error, error_size,
                              "hash input changed while it was read");
            } else {
                hwa_sha256_final(&context, digest);
                hwa_sha256_hex(digest, hex);
                result = 0;
            }
            break;
        }
    }
    if (fclose(stream) != 0 && result == 0) {
        hwa_set_error(error, error_size, "cannot close hash input '%s'", path);
        result = -1;
    }
    if (result != 0) {
        memset(hex, 0, 65U);
    }
    return result;
}

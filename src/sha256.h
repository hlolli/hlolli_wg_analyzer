#ifndef HWA_SHA256_H
#define HWA_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct HWASha256 {
    uint32_t state[8];
    uint64_t byte_count;
    unsigned char block[64];
    size_t block_size;
    int overflowed;
} HWASha256;

void hwa_sha256_init(HWASha256 *context);
void hwa_sha256_update(HWASha256 *context,
                       const unsigned char *data,
                       size_t size);
void hwa_sha256_final(HWASha256 *context, unsigned char digest[32]);
void hwa_sha256_hex(const unsigned char digest[32], char hex[65]);

int hwa_sha256_file(const char *path,
                    uint64_t max_bytes,
                    char hex[65],
                    char *error,
                    size_t error_size);

#endif

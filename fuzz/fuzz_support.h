#ifndef HWA_FUZZ_SUPPORT_H
#define HWA_FUZZ_SUPPORT_H

#include "hlolli_wg_analyzer.h"

#include <stddef.h>
#include <stdint.h>

#define HWA_FUZZ_MAX_INPUT_BYTES UINT64_C(1048576)
/* Includes the terminator. Longer temporary paths are rejected. */
#define HWA_FUZZ_PATH_CAP 4096U

typedef struct HWAFuzzBytes {
    const unsigned char *data;
    size_t size;
} HWAFuzzBytes;

typedef struct HWAFuzzTempFile {
    char path[HWA_FUZZ_PATH_CAP];
    int active;
} HWAFuzzTempFile;

int hwa_fuzz_read_at(void *context,
                     uint64_t offset,
                     unsigned char *destination,
                     size_t size);

int hwa_fuzz_write_temp(const unsigned char *data,
                        size_t size,
                        HWAFuzzTempFile *temporary);

void hwa_fuzz_remove_temp(HWAFuzzTempFile *temporary);

#endif

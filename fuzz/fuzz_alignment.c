#include "fuzz_support.h"

#include "alignment_file.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    HWAFuzzTempFile temporary;
    HWAAlignmentFileLimits limits;
    HWAAlignmentLockedSet locked;
    HWAAlignment alignment;
    char error[HWA_ERROR_SIZE];
    if ((uint64_t)size > HWA_FUZZ_MAX_INPUT_BYTES) return 0;
    if (hwa_fuzz_write_temp(data, size, &temporary) != 0) return 0;
    hwa_alignment_file_limits_default(&limits);
    limits.max_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    limits.max_work_bytes = UINT64_C(67108864);
    limits.max_anchors = 2048U;
    limits.max_matches = 2048U;
    limits.max_unmatched_spans = 2048U;
    limits.max_warnings = 2048U;
    if (hwa_alignment_file_read_locked(temporary.path,
                                       HWA_FUZZ_MAX_INPUT_BYTES,
                                       2048U, &locked,
                                       error, sizeof(error)) == 0)
        hwa_alignment_locked_set_free(&locked);
    if (hwa_alignment_file_read(temporary.path, &limits, &alignment,
                                error, sizeof(error)) == 0)
        hwa_alignment_free(&alignment);
    hwa_fuzz_remove_temp(&temporary);
    return 0;
}

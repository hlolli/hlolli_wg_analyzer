#ifndef HWA_ALIGNMENT_FILE_H
#define HWA_ALIGNMENT_FILE_H

#include "hlolli_wg_analyzer.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define HWA_ALIGNMENT_FILE_SCHEMA_VERSION 1U

const char *hwa_build_compiler_family(void);
const char *hwa_build_compiler_version(void);
const char *hwa_build_c_standard(void);
const char *hwa_build_target_os(void);
const char *hwa_build_endianness(void);
const char *hwa_build_mode(void);
unsigned hwa_build_pointer_bits(void);

typedef struct HWAAlignmentLockedSet {
    HWAAlignmentMode mode;
    char reference_sha256[HWA_SHA256_HEX_SIZE];
    char target_sha256[HWA_SHA256_HEX_SIZE];
    char score_sha256[HWA_SHA256_HEX_SIZE];
    double reference_duration_seconds;
    double target_duration_seconds;
    HWAAlignmentAnchor *anchors;
    size_t anchor_count;
} HWAAlignmentLockedSet;

/* Current-run parser caps. Saved META values never raise these limits. */
typedef struct HWAAlignmentFileLimits {
    uint64_t max_bytes;
    uint64_t max_work_bytes;
    size_t max_fields_per_row;
    size_t max_field_bytes;
    size_t max_anchors;
    size_t max_matches;
    size_t max_unmatched_spans;
    size_t max_warnings;
} HWAAlignmentFileLimits;

int hwa_alignment_file_write(FILE *stream,
                             const HWAAlignment *alignment,
                             char *error,
                             size_t error_size);

int hwa_alignment_file_read_locked(const char *path,
                                   uint64_t max_bytes,
                                   size_t max_anchors,
                                   HWAAlignmentLockedSet *locked,
                                   char *error,
                                   size_t error_size);

void hwa_alignment_file_limits_default(HWAAlignmentFileLimits *limits);

/*
 * Read and validate the complete canonical Stage 2 result. The first call
 * initializes `alignment`, including on failure. The returned object owns the
 * same fields as a successful alignment call and is freed the same way.
 */
int hwa_alignment_file_read(const char *path,
                            const HWAAlignmentFileLimits *limits,
                            HWAAlignment *alignment,
                            char *error,
                            size_t error_size);

int hwa_alignment_locked_set_matches(
    const HWAAlignmentLockedSet *locked,
    HWAAlignmentMode mode,
    const char reference_sha256[HWA_SHA256_HEX_SIZE],
    const char target_sha256[HWA_SHA256_HEX_SIZE],
    const char score_sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size);

void hwa_alignment_locked_set_free(HWAAlignmentLockedSet *locked);

#endif

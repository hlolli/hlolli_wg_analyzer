#ifndef HWA_RUN_FILE_H
#define HWA_RUN_FILE_H

#include "run.h"
#include "numeric_locale.h"

#include <stddef.h>
#include <stdio.h>

/* Write canonical RFC 4180 HWA_RUN,1 bytes to a binary stream. */
int hwa_run_file_write(FILE *stream,
                       const HWARunResult *result,
                       char *error,
                       size_t error_size);

/*
 * Read one complete result under current caller limits. Saved caps remain
 * provenance. The reader opens only `path`; retained manifest and source
 * paths stay inert.
 */
int hwa_run_file_read(const char *path,
                      const HWARunOptions *limits,
                      HWARunResult *result,
                      char file_sha256[HWA_SHA256_HEX_SIZE],
                      char *error,
                      size_t error_size);

/* Internal seam for callers that already own an active C numeric locale. */
int hwa_run_file_read_locale(
    const char *path,
    const HWARunOptions *limits,
    HWARunResult *result,
    char file_sha256[HWA_SHA256_HEX_SIZE],
    const HWANumericLocale *locale,
    char *error,
    size_t error_size);

#endif

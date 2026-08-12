#ifndef HWA_GAP_REPORT_FILE_H
#define HWA_GAP_REPORT_FILE_H

#include "gap_report.h"

#include <stddef.h>
#include <stdio.h>

/* Write canonical RFC 4180 HWA_REPORT,1 bytes. */
int hwa_gap_report_file_write(FILE *stream,
                              const HWAGapReportResult *result,
                              char *error,
                              size_t error_size);

/*
 * Read one complete result under current caller limits. The reader opens only
 * `path`; every saved source and clip path stays inert.
 */
int hwa_gap_report_file_read(
    const char *path,
    const HWAGapReportOptions *limits,
    HWAGapReportResult *result,
    char file_sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size);

#endif

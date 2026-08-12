#ifndef HWA_MEASURE_FILE_H
#define HWA_MEASURE_FILE_H

#include "hlolli_wg_analyzer.h"

#include <stddef.h>
#include <stdio.h>

#define HWA_MEASURE_FILE_SCHEMA_VERSION 1U
#define HWA_PROFILE_COMPARISON_FILE_SCHEMA_VERSION 1U

int hwa_measure_file_write(FILE *stream,
                           const HWAMeasurementSet *set,
                           char *error,
                           size_t error_size);

/*
 * Load one complete canonical profile under current caller limits. The saved
 * hard limits are provenance only. The reader hashes the named file before
 * and after parsing and returns that hash through file_sha256.
 */
int hwa_measure_file_read(
    const char *path,
    const HWAProfileComparisonOptions *limits,
    HWAMeasurementSet *set,
    char file_sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size);

int hwa_profile_comparison_file_write(
    FILE *stream,
    const HWAProfileComparisonSet *set,
    char *error,
    size_t error_size);

#endif

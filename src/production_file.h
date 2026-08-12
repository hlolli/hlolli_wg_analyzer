#ifndef HWA_PRODUCTION_FILE_H
#define HWA_PRODUCTION_FILE_H

#include "hlolli_wg_analyzer.h"

#include <stddef.h>
#include <stdio.h>

#define HWA_PRODUCTION_FILE_SCHEMA_VERSION 1U

int hwa_production_result_retained_bytes(
    const HWAProductionResult *result,
    uint64_t *bytes);

/* Replace VIEW rows with the method-owned aggregates of EVALUATION rows. */
int hwa_production_view_rows_rebuild(HWAProductionResult *result,
                                     char *error,
                                     size_t error_size);

int hwa_production_result_validate(const HWAProductionResult *result,
                                   char *error,
                                   size_t error_size);

/* Write canonical RFC 4180 HWA_PRODUCTION,1 bytes to a binary stream. */
int hwa_production_file_write(FILE *stream,
                              const HWAProductionResult *result,
                              char *error,
                              size_t error_size);

/*
 * Read one complete result under current caller limits. Saved caps remain
 * provenance. The reader opens only `path`; retained source paths stay inert.
 */
int hwa_production_file_read(
    const char *path,
    const HWAProductionOptions *limits,
    HWAProductionResult *result,
    char file_sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size);

#endif

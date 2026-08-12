#ifndef HWA_PHYSICAL_FILE_H
#define HWA_PHYSICAL_FILE_H

#include "hlolli_wg_analyzer.h"

#include <stddef.h>
#include <stdio.h>

#define HWA_PHYSICAL_FILE_SCHEMA_VERSION 1U

/* Compute retained result arrays and owned strings with checked arithmetic. */
int hwa_physical_check_set_retained_bytes(
    const HWAPhysicalCheckSet *set,
    uint64_t *bytes);

/* Total canonical CHECK-key order. Equality means a duplicate check key. */
int hwa_physical_check_canonical_compare(const HWAPhysicalCheck *left,
                                         const HWAPhysicalCheck *right);

/* Derive the one scored finding, if any, fixed by an available CHECK row. */
int hwa_physical_scored_finding_for_check(
    const HWAPhysicalCheck *check,
    HWAPhysicalFindingClass *finding_class,
    HWAPhysicalSeverity *severity,
    const char **code,
    const char **message,
    double *score);

/* Validate the complete public result without writing bytes. */
int hwa_physical_check_set_validate(const HWAPhysicalCheckSet *set,
                                    char *error,
                                    size_t error_size);

/* Write the canonical RFC 4180 HWA_PHYSICAL,1 form to a binary stream. */
int hwa_physical_file_write(FILE *stream,
                            const HWAPhysicalCheckSet *set,
                            char *error,
                            size_t error_size);

/*
 * Read one complete valid result under current caller limits and normalize it
 * for canonical rewrite. Saved caps remain provenance; the loaded set uses
 * current caller caps and its current retained-byte ledger. The reader opens
 * only `path`; retained source paths stay inert.
 */
int hwa_physical_file_read(
    const char *path,
    const HWAPhysicalOptions *limits,
    HWAPhysicalCheckSet *set,
    char file_sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size);

#endif

#ifndef HWA_EXPERIMENT_FILE_H
#define HWA_EXPERIMENT_FILE_H

#include "experiment.h"

#include <stddef.h>
#include <stdio.h>

/* Write canonical RFC 4180 HWA_EXPERIMENT,1 bytes to a binary stream. */
int hwa_experiment_file_write(FILE *stream,
                              const HWAExperimentResult *result,
                              char *error,
                              size_t error_size);

/* Create one new regular file. Existing paths and links are never replaced. */
int hwa_experiment_file_write_path(const char *path,
                                   const HWAExperimentResult *result,
                                   char *error,
                                   size_t error_size);

/* Internal seam for callers that already own an active C numeric locale. */
int hwa_experiment_file_write_path_locale(
    const char *path,
    const HWAExperimentResult *result,
    const HWANumericLocale *locale,
    char *error,
    size_t error_size);

/*
 * Read one complete result under current caller limits. The reader opens only
 * `path`; all paths retained in the saved result stay inert.
 */
int hwa_experiment_file_read(
    const char *path,
    const HWAExperimentOptions *limits,
    HWAExperimentResult *result,
    char file_sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size);

/* Internal seam for callers that already own an active C numeric locale. */
int hwa_experiment_file_read_locale(
    const char *path,
    const HWAExperimentOptions *limits,
    HWAExperimentResult *result,
    char file_sha256[HWA_SHA256_HEX_SIZE],
    const HWANumericLocale *locale,
    char *error,
    size_t error_size);

#endif

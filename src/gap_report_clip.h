#ifndef HWA_GAP_REPORT_CLIP_H
#define HWA_GAP_REPORT_CLIP_H

#include "gap_report.h"

#include <stddef.h>

/*
 * Render the authorized excerpts into a private sibling of
 * result->output_directory, write the report tree, and commit it without
 * replacing an existing path. RANK results do not use this function.
 */
int hwa_gap_report_build_clip_bundle(HWAGapReportResult *result,
                                     uint64_t starting_evaluations,
                                     char *error,
                                     size_t error_size);

/* Platform path rule used by the transaction seam and its host tests. */
int hwa_gap_report_clip_path_absolute(const char *path);

/* Pure authority checks shared by rendering and focused contract tests. */
int hwa_gap_report_production_view_authorized(
    const HWAProductionResult *production,
    uint64_t candidate_row,
    HWAGapReportView view,
    const char reference_sha256[HWA_SHA256_HEX_SIZE],
    const char model_sha256[HWA_SHA256_HEX_SIZE],
    const char *room_sha256,
    double eq_gain[HWA_PRODUCTION_EQ_NODE_COUNT],
    double *room_early_db,
    double *room_late_seconds);

int hwa_gap_report_run_view_authorized(
    const HWARunResult *run,
    uint64_t candidate_row,
    HWAGapReportView view,
    const char reference_sha256[HWA_SHA256_HEX_SIZE],
    const char model_sha256[HWA_SHA256_HEX_SIZE]);

#endif

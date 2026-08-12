#ifndef HWA_GAP_REPORT_OUTPUT_H
#define HWA_GAP_REPORT_OUTPUT_H

#include "gap_report.h"

#include <stddef.h>
#include <stdio.h>

int hwa_gap_report_text(FILE *stream, const HWAGapReportResult *result);
int hwa_gap_report_json(FILE *stream, const HWAGapReportResult *result);

int hwa_gap_report_output_projected_upper(
    const HWAGapReportResult *result,
    HWAGapReportMode mode,
    uint64_t projected_audio_bytes,
    uint64_t *tree_bytes);

/* Remove only the exact output tree owned by a successful result. */
int hwa_gap_report_output_remove(const HWAGapReportResult *result,
                                 char *error,
                                 size_t error_size);

/* HTML may open only verified module-owned clip paths under this root. */
int hwa_gap_report_html(FILE *stream,
                        const char *bundle_directory,
                        const HWAGapReportResult *result);

/* Write the requested canonical reports into an existing private tree. */
int hwa_gap_report_output_write_tree(const char *private_directory,
                                     HWAGapReportMode mode,
                                     HWAGapReportResult *result,
                                     uint64_t outer_live_work,
                                     char *error,
                                     size_t error_size);

#endif

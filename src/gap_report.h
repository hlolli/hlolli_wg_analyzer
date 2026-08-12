#ifndef HWA_GAP_REPORT_H
#define HWA_GAP_REPORT_H

#include "hlolli_wg_analyzer.h"

#include <stddef.h>
#include <stdint.h>

const char *hwa_gap_report_mode_name(HWAGapReportMode value);
const char *hwa_gap_report_source_kind_name(HWAGapReportSourceKind value);
const char *hwa_gap_report_availability_name(HWAGapReportAvailability value);
const char *hwa_gap_report_axis_name(HWAGapReportAxis value);
const char *hwa_gap_report_view_name(HWAGapReportView value);
const char *hwa_gap_report_candidate_kind_name(HWAGapReportCandidateKind value);

int hwa_gap_report_mode_from_name(const char *name, HWAGapReportMode *value);
int hwa_gap_report_source_kind_from_name(
    const char *name, HWAGapReportSourceKind *value);
int hwa_gap_report_availability_from_name(
    const char *name, HWAGapReportAvailability *value);
int hwa_gap_report_axis_from_name(const char *name, HWAGapReportAxis *value);
int hwa_gap_report_view_from_name(const char *name, HWAGapReportView *value);
int hwa_gap_report_candidate_kind_from_name(
    const char *name, HWAGapReportCandidateKind *value);

int hwa_gap_report_options_validate(const HWAGapReportOptions *options,
                                    char *error,
                                    size_t error_size);

int hwa_gap_report_result_validate(const HWAGapReportResult *result,
                                   char *error,
                                   size_t error_size);

/* Rebuild factors, linked primaries, ranks, groups, and case scores in place. */
int hwa_gap_report_result_rebuild(HWAGapReportResult *result,
                                  char *error,
                                  size_t error_size);

/* Rebuild the fixed warning catalog after excerpt availability changes. */
int hwa_gap_report_warnings_rebuild(HWAGapReportResult *result,
                                    char *error,
                                    size_t error_size);

int hwa_gap_report_result_retained_bytes(const HWAGapReportResult *result,
                                         uint64_t *bytes);

/* Saved forms replace host paths with fixed inert projections. */
int hwa_gap_report_result_canonical_retained_bytes(
    const HWAGapReportResult *result,
    uint64_t *bytes);

int hwa_gap_report_result_peak_work_bytes(
    const HWAGapReportResult *result,
    uint64_t reader_bytes,
    uint64_t *peak);

int hwa_gap_report_candidate_catalog_fits(
    const HWAGapReportResult *result,
    const HWAGapReportOptions *options);
int hwa_gap_report_case_catalog_fits(
    const HWAGapReportResult *result,
    const HWAGapReportOptions *options);

/* Return the deterministic X side: nonzero means reference, zero means model. */
int hwa_gap_report_excerpt_x_is_reference(
    const HWAGapReportResult *result,
    const HWAGapReportExcerpt *excerpt);

int hwa_gap_report_manifest_validate_bytes(
    const unsigned char *data,
    size_t size,
    const HWAGapReportOptions *options,
    char *error,
    size_t error_size);

#endif

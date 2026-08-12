#ifndef HWA_RUN_H
#define HWA_RUN_H

#include "hlolli_wg_analyzer.h"

#include <stddef.h>
#include <stdint.h>

#define HWA_RUN_FILE_SCHEMA_VERSION 1U

/*
 * Manifest schema 1 has exact top-level keys schema, schema_version,
 * method_version, clock_rate_hz, stems, probes, and links. Each stem/probe
 * object carries its expected lowercase SHA-256; paths come only from the
 * caller's exact binding list. Probe CSV is index,value with contiguous
 * zero-based indexes. Binary is HWAPRB1\0, a little-endian u64 count, then
 * that many little-endian IEEE binary64 values.
 */

typedef struct HWARunWarningSpec {
    const char *code;
    const char *message;
    uint64_t source_id;
    uint64_t clock_id;
    uint64_t stage_id;
    uint64_t link_id;
    int source_id_valid;
    int clock_id_valid;
    int stage_id_valid;
    int link_id_valid;
} HWARunWarningSpec;

const char *hwa_run_availability_name(HWARunAvailability value);
const char *hwa_run_source_kind_name(HWARunSourceKind value);
const char *hwa_run_side_name(HWARunSide value);
const char *hwa_run_stem_role_name(HWARunStemRole value);
const char *hwa_run_probe_format_name(HWARunProbeFormat value);
const char *hwa_run_feature_kind_name(HWARunFeatureKind value);
const char *hwa_run_unit_name(HWARunUnit value);

size_t hwa_run_feature_catalog_count(void);
int hwa_run_feature_catalog_at(size_t offset,
                               HWARunFeatureKind *kind,
                               uint32_t *index,
                               HWARunUnit *unit);

size_t hwa_run_stage_catalog_count(void);
int hwa_run_stage_catalog_at(size_t offset,
                             HWARunStemRole *from_role,
                             HWARunStemRole *to_role);

int hwa_run_source_canonical_compare(const void *left, const void *right);
int hwa_run_clock_canonical_compare(const void *left, const void *right);
int hwa_run_feature_canonical_compare(const void *left, const void *right);
int hwa_run_stage_canonical_compare(const void *left, const void *right);
int hwa_run_probe_canonical_compare(const void *left, const void *right);
int hwa_run_link_canonical_compare(const void *left, const void *right);

int hwa_run_clock_derived_expected(
    const HWARunResult *result,
    const HWARunClock *clock,
    int64_t *start_offset_samples,
    int64_t *end_offset_samples,
    int64_t *drift_samples,
    uint64_t *overlap_frames,
    double *drift_ppm,
    uint32_t *quality_flags);

int hwa_run_feature_derived_expected(const HWARunFeature *feature,
                                     double *delta,
                                     double *normalized_gap);

int hwa_run_link_derived_expected(const HWARunResult *result,
                                  const HWARunLink *link,
                                  int64_t *lag_samples,
                                  double *r_squared,
                                  double *coverage,
                                  uint32_t *required_quality_flags);

int hwa_run_evaluations_expected(const HWARunResult *result,
                                 uint64_t *expected);

/* Replace all three stage rows with the method-owned feature-gap result. */
int hwa_run_stage_rows_rebuild(HWARunResult *result,
                               char *error,
                               size_t error_size);

size_t hwa_run_warning_spec_count(const HWARunResult *result);
int hwa_run_warning_spec_at(const HWARunResult *result,
                            size_t offset,
                            HWARunWarningSpec *spec);
int hwa_run_warnings_rebuild(HWARunResult *result,
                             char *error,
                             size_t error_size);

int hwa_run_result_retained_bytes(const HWARunResult *result,
                                  uint64_t *bytes);

int hwa_run_result_validate(const HWARunResult *result,
                            char *error,
                            size_t error_size);

int hwa_run_manifest_validate_bytes(const unsigned char *data,
                                    size_t size,
                                    const HWARunOptions *options,
                                    char *error,
                                    size_t error_size);

int hwa_run_probe_parse_bytes(HWARunProbeFormat format,
                              const unsigned char *data,
                              size_t size,
                              uint64_t expected_count,
                              double *values,
                              char *error,
                              size_t error_size);

#endif

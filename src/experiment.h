#ifndef HWA_EXPERIMENT_H
#define HWA_EXPERIMENT_H

#include "hlolli_wg_analyzer.h"
#include "numeric_locale.h"

#include <stddef.h>
#include <stdint.h>

#define HWA_EXPERIMENT_FILE_SCHEMA_VERSION 1U

/*
 * Manifest schema 1 has the exact top-level keys schema, schema_version,
 * method_version, clock_rate_hz, inputs, parameters, plan, cases, and
 * responses. schema is "hwa-experiment" and method_version is "stage8-1".
 *
 * inputs: [{id,sha256}]
 * parameters: [{id,unit,minimum,maximum,baseline,levels}]
 * plan: {kind,seed,sample_count,replicates}
 * cases: [{id,split,weight,stems,probes,links}]
 * responses: [{id,role,feature,index}]
 *
 * A case stem has {id,side,role,input_id,output,start_sample,gain_db,
 * rate_hz,channels}; exactly one of input_id and output is a non-empty string
 * and the other is null. A probe has {id,side,name,unit,format,input_id,
 * output,start_sample,rate_numerator,rate_denominator,value_count} under the
 * same rule. output is one safe path component. A link has
 * {stem,probe,feature}; Stage 7 currently accepts rms_dbfs.
 *
 * IDs use the Stage 7 token grammar. Parameter, level, case, and response
 * arrays are strictly sorted by ID or numeric value. OAT and grid levels are
 * finite, unique, in range, and include baseline. Random parameters have
 * minimum < maximum and no levels. Point 1 is baseline. OAT points then use
 * parameter and level order, skipping each baseline. Grid points use the
 * lexicographic product with the last parameter changing fastest, moving the
 * baseline tuple to point 1 without dropping another tuple. Random points use
 * SplitMix64 and its high 53 bits:
 * minimum + (maximum-minimum) * ((next >> 11) / 2^53).
 *
 * Jobs use point, case, then replicate order. Their seed is one SplitMix64
 * value derived from plan seed and those three zero-based coordinates. The
 * job and point keys are lowercase SHA-256 over fixed canonical JSON bytes.
 * Observations use the matching Stage 7 normalized_gap.
 *
 * Candidate mean gaps are case-weighted means for one point, response, and
 * split. improvement is baseline mean minus current mean. worst_harm is the
 * largest per-case current-minus-baseline mean. Sensitivity uses OLS slope,
 * Pearson correlation, R squared, and conditional-mean monotonicity. Grid
 * effect is Var(E[y|x])/Var(y). Grid pair interaction is
 * Var(cell-mainA-mainB+grand)/Var(y). Replicate noise is the pooled within-cell
 * population variance. Flat means range <= 1e-6, noisy means SD > 0.02, and
 * check harm means worst_harm > 0.02. Warning order is check-harm, flat, noise.
 */

const char *hwa_experiment_plan_name(HWAExperimentPlanKind value);
const char *hwa_experiment_split_name(HWAExperimentSplit value);
const char *hwa_experiment_monotonicity_name(
    HWAExperimentMonotonicity value);

/* Rebuild all method-owned rows from values, jobs, and observations. */
int hwa_experiment_candidates_rebuild(HWAExperimentResult *result,
                                      char *error,
                                      size_t error_size);
int hwa_experiment_sensitivities_rebuild(HWAExperimentResult *result,
                                         char *error,
                                         size_t error_size);
int hwa_experiment_interactions_rebuild(HWAExperimentResult *result,
                                        char *error,
                                        size_t error_size);
int hwa_experiment_warnings_rebuild(HWAExperimentResult *result,
                                    char *error,
                                    size_t error_size);
int hwa_experiment_derived_rebuild(HWAExperimentResult *result,
                                   char *error,
                                   size_t error_size);
/* Internal deadline seam used by execute and validator-owned rebuilds. */
void hwa_experiment_deadline_enter(uint64_t started, uint64_t maximum);
void hwa_experiment_deadline_leave(void);
int hwa_experiment_deadline_poll(char *error, size_t error_size);

int hwa_experiment_total_run_evaluations_expected(
    const HWAExperimentResult *result,
    uint64_t *expected);
int hwa_experiment_total_output_bytes_expected(
    const HWAExperimentResult *result,
    uint64_t *expected);
int hwa_experiment_result_retained_bytes(const HWAExperimentResult *result,
                                         uint64_t *bytes);
/* Saved files project host paths to `.` and omit the runtime resume path. */
int hwa_experiment_result_canonical_retained_bytes(
    const HWAExperimentResult *result,
    uint64_t *bytes);
/* Conservative live peak for one retained result plus rebuild/validation. */
int hwa_experiment_result_peak_work_bytes(
    const HWAExperimentResult *result,
    uint64_t extra_bytes,
    uint64_t *bytes);
int hwa_experiment_result_validate(const HWAExperimentResult *result,
                                   char *error,
                                   size_t error_size);
/* Use this seam when the caller already owns an active C numeric locale. */
int hwa_experiment_result_validate_locale(
    const HWAExperimentResult *result,
    const HWANumericLocale *locale,
    char *error,
    size_t error_size);

/* Remove only the unchanged, module-owned bundle after a caller output fault. */
int hwa_experiment_output_remove(const HWAExperimentResult *result,
                                 char *error,
                                 size_t error_size);

/* Saved derived floats accept at most 32 nextafter steps from rebuilt values. */
int hwa_experiment_derived_double_matches(double saved, double expected);
double hwa_experiment_derived_double_normalize(double saved, double expected);

int hwa_experiment_manifest_validate_bytes(const unsigned char *data,
                                           size_t size,
                                           const HWAExperimentOptions *options,
                                           char *error,
                                           size_t error_size);

#endif

#include "experiment_report.h"

#include "alignment_file.h"
#include "experiment.h"
#include "numeric_locale.h"
#include "output.h"
#include "run.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int hwa_er_number(FILE *stream, double value)
{
    return fprintf(stream, "%.17g", value == 0.0 ? 0.0 : value) < 0 ? -1 : 0;
}

static int hwa_er_optional_number(FILE *stream, double value, int valid)
{
    return valid ? hwa_er_number(stream, value) :
                   (fputs("null", stream) == EOF ? -1 : 0);
}

static int hwa_er_input_path(FILE *stream,
                             const char *path,
                             const char *sha256)
{
    return fputs("{\"path\":", stream) == EOF ||
           hwa_json_write_string(stream, path) != 0 ||
           fputs(",\"path_encoding\":\"utf8_with_invalid_bytes_as_u00xx\","
                 "\"path_bytes_hex\":", stream) == EOF ||
           hwa_json_write_byte_hex(stream, path) != 0 ||
           fputs(",\"sha256\":", stream) == EOF ||
           hwa_json_write_string(stream, sha256) != 0 ||
           fputc('}', stream) == EOF ? -1 : 0;
}

static int hwa_er_build(FILE *stream)
{
    return fputs("{\"compiler_family\":", stream) == EOF ||
           hwa_json_write_string(stream, hwa_build_compiler_family()) != 0 ||
           fputs(",\"compiler_version\":", stream) == EOF ||
           hwa_json_write_string(stream, hwa_build_compiler_version()) != 0 ||
           fputs(",\"c_standard\":", stream) == EOF ||
           hwa_json_write_string(stream, hwa_build_c_standard()) != 0 ||
           fputs(",\"target_os\":", stream) == EOF ||
           hwa_json_write_string(stream, hwa_build_target_os()) != 0 ||
           fprintf(stream, ",\"pointer_bits\":%u,\"endianness\":",
                   hwa_build_pointer_bits()) < 0 ||
           hwa_json_write_string(stream, hwa_build_endianness()) != 0 ||
           fputs(",\"mode\":", stream) == EOF ||
           hwa_json_write_string(stream, hwa_build_mode()) != 0 ||
           fputc('}', stream) == EOF ? -1 : 0;
}

static int hwa_er_run_options(FILE *stream, const HWARunOptions *o)
{
    return fprintf(
        stream,
        "{\"decode_block_frames\":%zu,\"max_manifest_bytes\":%" PRIu64
        ",\"max_input_bytes\":%" PRIu64
        ",\"max_input_frames\":%" PRIu64
        ",\"max_probe_bytes\":%" PRIu64
        ",\"max_probe_values\":%" PRIu64
        ",\"max_work_bytes\":%" PRIu64
        ",\"max_evaluations\":%" PRIu64
        ",\"max_stems\":%zu,\"max_probes\":%zu,\"max_links\":%zu"
        ",\"max_json_depth\":%zu,\"max_json_tokens\":%zu"
        ",\"max_result_rows\":%zu,\"max_warnings\":%zu}",
        o->decode_block_frames, o->max_manifest_bytes, o->max_input_bytes,
        o->max_input_frames, o->max_probe_bytes, o->max_probe_values,
        o->max_work_bytes, o->max_evaluations, o->max_stems, o->max_probes,
        o->max_links, o->max_json_depth, o->max_json_tokens,
        o->max_result_rows, o->max_warnings) < 0 ? -1 : 0;
}

static int hwa_er_options(FILE *stream, const HWAExperimentOptions *o)
{
    if (fputs("{\"run\":", stream) == EOF ||
        hwa_er_run_options(stream, &o->run) != 0) return -1;
    return fprintf(
        stream,
        ",\"max_manifest_bytes\":%" PRIu64
        ",\"max_input_bytes\":%" PRIu64
        ",\"max_work_bytes\":%" PRIu64
        ",\"max_bundle_bytes\":%" PRIu64
        ",\"max_output_file_bytes\":%" PRIu64
        ",\"max_total_run_evaluations\":%" PRIu64
        ",\"max_job_milliseconds\":%" PRIu64
        ",\"max_total_milliseconds\":%" PRIu64
        ",\"max_parameters\":%zu,\"max_levels\":%zu,\"max_cases\":%zu"
        ",\"max_responses\":%zu,\"max_points\":%zu,\"max_jobs\":%zu"
        ",\"max_replicates\":%zu,\"max_artifacts\":%zu"
        ",\"max_observations\":%zu,\"max_sensitivities\":%zu"
        ",\"max_interactions\":%zu,\"max_warnings\":%zu}",
        o->max_manifest_bytes, o->max_input_bytes, o->max_work_bytes,
        o->max_bundle_bytes, o->max_output_file_bytes,
        o->max_total_run_evaluations, o->max_job_milliseconds,
        o->max_total_milliseconds, o->max_parameters, o->max_levels,
        o->max_cases, o->max_responses, o->max_points, o->max_jobs,
        o->max_replicates, o->max_artifacts, o->max_observations,
        o->max_sensitivities, o->max_interactions, o->max_warnings) < 0 ?
        -1 : 0;
}

static int hwa_er_input(FILE *stream, const HWAExperimentInput *r)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"binding_id\":", r->id) < 0 ||
        hwa_json_write_string(stream, r->binding_id) != 0 ||
        fputs(",\"sha256\":", stream) == EOF ||
        hwa_json_write_string(stream, r->sha256) != 0 ||
        fprintf(stream, ",\"file_bytes\":%" PRIu64 "}", r->file_bytes) < 0) {
        return -1;
    }
    return 0;
}

static int hwa_er_parameter(FILE *stream, const HWAExperimentParameter *r)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"name\":", r->id) < 0 ||
        hwa_json_write_string(stream, r->name) != 0 ||
        fputs(",\"unit\":", stream) == EOF ||
        hwa_json_write_string(stream, r->unit) != 0 ||
        fprintf(stream, ",\"minimum\":%.17g,\"maximum\":%.17g"
                ",\"baseline\":%.17g,\"first_level\":%zu,\"level_count\":%zu}",
                r->minimum, r->maximum, r->baseline,
                r->first_level, r->level_count) < 0) return -1;
    return 0;
}

static int hwa_er_level(FILE *stream, const HWAExperimentLevel *r)
{
    return fprintf(stream, "{\"id\":%" PRIu64
                   ",\"parameter_id\":%" PRIu64 ",\"value\":%.17g}",
                   r->id, r->parameter_id, r->value) < 0 ? -1 : 0;
}

static int hwa_er_case(FILE *stream, const HWAExperimentCase *r)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"name\":", r->id) < 0 ||
        hwa_json_write_string(stream, r->name) != 0 ||
        fputs(",\"split\":", stream) == EOF ||
        hwa_json_write_string(stream, hwa_experiment_split_name(r->split)) != 0 ||
        fprintf(stream, ",\"weight\":%.17g}", r->weight) < 0) return -1;
    return 0;
}

static int hwa_er_response(FILE *stream, const HWAExperimentResponse *r)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"name\":", r->id) < 0 ||
        hwa_json_write_string(stream, r->name) != 0 ||
        fputs(",\"role\":", stream) == EOF ||
        hwa_json_write_string(stream, hwa_run_stem_role_name(r->role)) != 0 ||
        fputs(",\"feature\":", stream) == EOF ||
        hwa_json_write_string(stream,
                              hwa_run_feature_kind_name(r->feature)) != 0 ||
        fprintf(stream, ",\"feature_index\":%" PRIu32 "}",
                r->feature_index) < 0) return -1;
    return 0;
}

static int hwa_er_point(FILE *stream, const HWAExperimentPoint *r)
{
    return fprintf(stream, "{\"id\":%" PRIu64 ",\"key\":\"%s\""
                   ",\"baseline\":%s}", r->id, r->key,
                   r->baseline ? "true" : "false") < 0 ? -1 : 0;
}

static int hwa_er_value(FILE *stream, const HWAExperimentValue *r)
{
    return fprintf(stream, "{\"id\":%" PRIu64
                   ",\"point_id\":%" PRIu64
                   ",\"parameter_id\":%" PRIu64 ",\"value\":%.17g}",
                   r->id, r->point_id, r->parameter_id, r->value) < 0 ?
                   -1 : 0;
}

static int hwa_er_job(FILE *stream, const HWAExperimentJob *r)
{
    const char *path = r->run_result_path;
    if (path == NULL || strncmp(path, "jobs/", 5U) != 0) return -1;
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"key\":\"%s\""
                ",\"point_id\":%" PRIu64 ",\"case_id\":%" PRIu64
                ",\"replicate\":%zu,\"seed\":%" PRIu64 ",\"run_result\":",
                r->id, r->key, r->point_id, r->case_id,
                r->replicate, r->seed) < 0 ||
        hwa_er_input_path(stream, path,
                          r->run_result_sha256) != 0 ||
        fputs(",\"run_manifest_sha256\":", stream) == EOF ||
        hwa_json_write_string(stream, r->run_manifest_sha256) != 0 ||
        fprintf(stream, ",\"output_bytes\":%" PRIu64
                ",\"run_evaluations\":%" PRIu64 "}",
                r->output_bytes, r->run_evaluations) < 0) {
        return -1;
    }
    return 0;
}

static int hwa_er_artifact(FILE *stream, const HWAExperimentArtifact *r)
{
    const char *path = r->path;
    if (path == NULL || strncmp(path, "jobs/", 5U) != 0) return -1;
    if (fprintf(stream, "{\"id\":%" PRIu64
                ",\"job_id\":%" PRIu64 ",\"resource_id\":",
                r->id, r->job_id) < 0 ||
        hwa_json_write_string(stream, r->resource_id) != 0 ||
        fputs(",\"artifact\":", stream) == EOF ||
        hwa_er_input_path(stream, path, r->sha256) != 0 ||
        fprintf(stream, ",\"file_bytes\":%" PRIu64 ",\"kind\":",
                r->file_bytes) < 0 ||
        hwa_json_write_string(stream, hwa_run_source_kind_name(r->kind)) != 0 ||
        fputc('}', stream) == EOF) return -1;
    return 0;
}

static int hwa_er_observation(FILE *stream,
                              const HWAExperimentObservation *r)
{
    if (fprintf(stream, "{\"id\":%" PRIu64
                ",\"job_id\":%" PRIu64
                ",\"response_id\":%" PRIu64 ",\"availability\":",
                r->id, r->job_id, r->response_id) < 0 ||
        hwa_json_write_string(stream,
                              hwa_run_availability_name(r->availability)) != 0 ||
        fputs(",\"value\":", stream) == EOF ||
        hwa_er_optional_number(stream, r->value, r->value_valid) != 0 ||
        fprintf(stream, ",\"quality_flags\":%" PRIu32
                ",\"value_valid\":%s}", r->quality_flags,
                r->value_valid ? "true" : "false") < 0) return -1;
    return 0;
}

static int hwa_er_candidate(FILE *stream, const HWAExperimentCandidate *r)
{
    if (fprintf(stream, "{\"id\":%" PRIu64
                ",\"point_id\":%" PRIu64
                ",\"response_id\":%" PRIu64 ",\"split\":",
                r->id, r->point_id, r->response_id) < 0 ||
        hwa_json_write_string(stream, hwa_experiment_split_name(r->split)) != 0 ||
        fputs(",\"availability\":", stream) == EOF ||
        hwa_json_write_string(stream,
                              hwa_run_availability_name(r->availability)) != 0 ||
        fputs(",\"mean_gap\":", stream) == EOF ||
        hwa_er_optional_number(stream, r->mean_gap, r->values_valid) != 0 ||
        fputs(",\"improvement\":", stream) == EOF ||
        hwa_er_optional_number(stream, r->improvement, r->values_valid) != 0 ||
        fputs(",\"worst_harm\":", stream) == EOF ||
        hwa_er_optional_number(stream, r->worst_harm, r->values_valid) != 0 ||
        fprintf(stream, ",\"case_count\":%zu,\"quality_flags\":%" PRIu32
                ",\"values_valid\":%s}", r->case_count, r->quality_flags,
                r->values_valid ? "true" : "false") < 0) return -1;
    return 0;
}

static int hwa_er_sensitivity(FILE *stream,
                              const HWAExperimentSensitivity *r)
{
    if (fprintf(stream, "{\"id\":%" PRIu64
                ",\"parameter_id\":%" PRIu64
                ",\"response_id\":%" PRIu64 ",\"split\":",
                r->id, r->parameter_id, r->response_id) < 0 ||
        hwa_json_write_string(stream, hwa_experiment_split_name(r->split)) != 0 ||
        fputs(",\"availability\":", stream) == EOF ||
        hwa_json_write_string(stream,
                              hwa_run_availability_name(r->availability)) != 0 ||
        fputs(",\"slope\":", stream) == EOF ||
        hwa_er_optional_number(stream, r->slope, r->linear_valid) != 0 ||
        fputs(",\"pearson\":", stream) == EOF ||
        hwa_er_optional_number(stream, r->pearson, r->linear_valid) != 0 ||
        fputs(",\"linear_r_squared\":", stream) == EOF ||
        hwa_er_optional_number(stream, r->linear_r_squared,
                               r->linear_valid) != 0 ||
        fputs(",\"effect_fraction\":", stream) == EOF ||
        hwa_er_optional_number(stream, r->effect_fraction,
                               r->effect_valid) != 0 ||
        fputs(",\"response_range\":", stream) == EOF ||
        hwa_er_number(stream, r->response_range) != 0 ||
        fputs(",\"noise_sd\":", stream) == EOF ||
        hwa_er_optional_number(stream, r->noise_sd, r->noise_valid) != 0 ||
        fprintf(stream, ",\"point_count\":%zu,\"monotonicity\":",
                r->point_count) < 0 ||
        hwa_json_write_string(
            stream, hwa_experiment_monotonicity_name(r->monotonicity)) != 0 ||
        fprintf(stream, ",\"quality_flags\":%" PRIu32
                ",\"linear_valid\":%s,\"effect_valid\":%s"
                ",\"noise_valid\":%s}", r->quality_flags,
                r->linear_valid ? "true" : "false",
                r->effect_valid ? "true" : "false",
                r->noise_valid ? "true" : "false") < 0) return -1;
    return 0;
}

static int hwa_er_interaction(FILE *stream,
                              const HWAExperimentInteraction *r)
{
    if (fprintf(stream, "{\"id\":%" PRIu64
                ",\"left_parameter_id\":%" PRIu64
                ",\"right_parameter_id\":%" PRIu64
                ",\"response_id\":%" PRIu64 ",\"split\":",
                r->id, r->left_parameter_id, r->right_parameter_id,
                r->response_id) < 0 ||
        hwa_json_write_string(stream, hwa_experiment_split_name(r->split)) != 0 ||
        fputs(",\"availability\":", stream) == EOF ||
        hwa_json_write_string(stream,
                              hwa_run_availability_name(r->availability)) != 0 ||
        fputs(",\"effect_fraction\":", stream) == EOF ||
        hwa_er_optional_number(stream, r->effect_fraction,
                               r->effect_valid) != 0 ||
        fprintf(stream, ",\"point_count\":%zu,\"effect_valid\":%s}",
                r->point_count, r->effect_valid ? "true" : "false") < 0) {
        return -1;
    }
    return 0;
}

static int hwa_er_warning(FILE *stream, const HWAExperimentWarning *r)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"code\":", r->id) < 0 ||
        hwa_json_write_string(stream, r->code) != 0 ||
        fputs(",\"message\":", stream) == EOF ||
        hwa_json_write_string(stream, r->message) != 0 ||
        fputs(",\"job_id\":", stream) == EOF) return -1;
    if (r->job_id_valid) {
        if (fprintf(stream, "%" PRIu64, r->job_id) < 0) return -1;
    } else if (fputs("null", stream) == EOF) return -1;
    if (fputs(",\"point_id\":", stream) == EOF) return -1;
    if (r->point_id_valid) {
        if (fprintf(stream, "%" PRIu64, r->point_id) < 0) return -1;
    } else if (fputs("null", stream) == EOF) return -1;
    if (fputs(",\"parameter_id\":", stream) == EOF) return -1;
    if (r->parameter_id_valid) {
        if (fprintf(stream, "%" PRIu64, r->parameter_id) < 0) return -1;
    } else if (fputs("null", stream) == EOF) return -1;
    if (fputs(",\"response_id\":", stream) == EOF) return -1;
    if (r->response_id_valid) {
        if (fprintf(stream, "%" PRIu64, r->response_id) < 0) return -1;
    } else if (fputs("null", stream) == EOF) return -1;
    return fputc('}', stream) == EOF ? -1 : 0;
}

typedef int (*HWAERowWriter)(FILE *, const void *);

static int hwa_er_array(FILE *stream,
                        const void *rows,
                        size_t count,
                        size_t row_size,
                        HWAERowWriter writer)
{
    const unsigned char *bytes = (const unsigned char *)rows;
    size_t index;
    if (fputc('[', stream) == EOF) return -1;
    for (index = 0U; index < count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            writer(stream, bytes + index * row_size) != 0) return -1;
    }
    return fputc(']', stream) == EOF ? -1 : 0;
}

#define EWRAP(name, type, function)                                        \
    static int name(FILE *stream, const void *row)                         \
    { return function(stream, (const type *)row); }
EWRAP(hwa_er_input_any, HWAExperimentInput, hwa_er_input)
EWRAP(hwa_er_parameter_any, HWAExperimentParameter, hwa_er_parameter)
EWRAP(hwa_er_level_any, HWAExperimentLevel, hwa_er_level)
EWRAP(hwa_er_case_any, HWAExperimentCase, hwa_er_case)
EWRAP(hwa_er_response_any, HWAExperimentResponse, hwa_er_response)
EWRAP(hwa_er_point_any, HWAExperimentPoint, hwa_er_point)
EWRAP(hwa_er_value_any, HWAExperimentValue, hwa_er_value)
EWRAP(hwa_er_job_any, HWAExperimentJob, hwa_er_job)
EWRAP(hwa_er_artifact_any, HWAExperimentArtifact, hwa_er_artifact)
EWRAP(hwa_er_observation_any, HWAExperimentObservation, hwa_er_observation)
EWRAP(hwa_er_candidate_any, HWAExperimentCandidate, hwa_er_candidate)
EWRAP(hwa_er_sensitivity_any, HWAExperimentSensitivity, hwa_er_sensitivity)
EWRAP(hwa_er_interaction_any, HWAExperimentInteraction, hwa_er_interaction)
EWRAP(hwa_er_warning_any, HWAExperimentWarning, hwa_er_warning)
#undef EWRAP

static int hwa_er_json_impl(FILE *stream,
                            const HWAExperimentResult *result)
{
    uint64_t canonical_retained;
    if (stream == NULL || result == NULL ||
        hwa_experiment_result_canonical_retained_bytes(
            result, &canonical_retained) != 0) return -1;
    if (fputs("{\"schema_version\":10,\"command\":\"experiment\",\"tool_version\":",
              stream) == EOF ||
        hwa_json_write_string(stream, HWA_VERSION) != 0 ||
        fputs(",\"method_version\":", stream) == EOF ||
        hwa_json_write_string(stream, HWA_EXPERIMENT_METHOD_VERSION) != 0 ||
        fputs(",\"experiment_schema_version\":1,\"build\":", stream) == EOF ||
        hwa_er_build(stream) != 0 ||
        fputs(",\"options\":", stream) == EOF ||
        hwa_er_options(stream, &result->options) != 0 ||
        fputs(",\"plan\":{\"kind\":", stream) == EOF ||
        hwa_json_write_string(stream,
                              hwa_experiment_plan_name(result->plan_kind)) != 0 ||
        fprintf(stream, ",\"seed\":%" PRIu64 ",\"replicates\":%zu}",
                result->plan_seed, result->plan_replicates) < 0 ||
        fputs(",\"manifest_sha256\":", stream) == EOF ||
        hwa_json_write_string(stream, result->manifest_sha256) != 0 ||
        fputs(",\"renderer\":{\"id\":", stream) == EOF ||
        hwa_json_write_string(stream, result->renderer_id) != 0 ||
        fputs(",\"sha256\":", stream) == EOF ||
        hwa_json_write_string(stream, result->renderer_sha256) != 0 ||
        fprintf(stream, "},\"ledgers\":{\"retained_work_bytes\":%" PRIu64
                ",\"total_run_evaluations\":%" PRIu64
                ",\"total_output_bytes\":%" PRIu64 "}",
                canonical_retained, result->total_run_evaluations,
                result->total_output_bytes) < 0) {
        return -1;
    }
#define EJSON(name, field, count, type, writer)                             \
    do {                                                                   \
        if (fputs(",\"" name "\":", stream) == EOF ||                      \
            hwa_er_array(stream, result->field, result->count,              \
                         sizeof(type), writer) != 0) return -1;             \
    } while (0)
    EJSON("inputs", inputs, input_count, HWAExperimentInput, hwa_er_input_any);
    EJSON("parameters", parameters, parameter_count, HWAExperimentParameter,
          hwa_er_parameter_any);
    EJSON("levels", levels, level_count, HWAExperimentLevel, hwa_er_level_any);
    EJSON("cases", cases, case_count, HWAExperimentCase, hwa_er_case_any);
    EJSON("responses", responses, response_count, HWAExperimentResponse,
          hwa_er_response_any);
    EJSON("points", points, point_count, HWAExperimentPoint, hwa_er_point_any);
    EJSON("values", values, value_count, HWAExperimentValue, hwa_er_value_any);
    EJSON("jobs", jobs, job_count, HWAExperimentJob, hwa_er_job_any);
    EJSON("artifacts", artifacts, artifact_count, HWAExperimentArtifact,
          hwa_er_artifact_any);
    EJSON("observations", observations, observation_count,
          HWAExperimentObservation, hwa_er_observation_any);
    EJSON("candidates", candidates, candidate_count, HWAExperimentCandidate,
          hwa_er_candidate_any);
    EJSON("sensitivities", sensitivities, sensitivity_count,
          HWAExperimentSensitivity, hwa_er_sensitivity_any);
    EJSON("interactions", interactions, interaction_count,
          HWAExperimentInteraction, hwa_er_interaction_any);
    EJSON("warnings", warnings, warning_count, HWAExperimentWarning,
          hwa_er_warning_any);
#undef EJSON
    return fputs("}\n", stream) == EOF ? -1 : 0;
}

static int hwa_er_text_impl(FILE *stream,
                            const HWAExperimentResult *result)
{
    size_t index;
    uint64_t canonical_retained;
    if (stream == NULL || result == NULL ||
        hwa_experiment_result_canonical_retained_bytes(
            result, &canonical_retained) != 0) return -1;
    if (fprintf(stream,
                "Experiment analysis\n"
                "Method: %s\n"
                "Plan: %s, seed %" PRIu64 ", %zu replicate(s)\n"
                "Rows: %zu input(s), %zu parameter(s), %zu level(s), "
                "%zu case(s), %zu response(s), %zu point(s), %zu job(s)\n"
                "Ledgers: %" PRIu64 " retained bytes, %" PRIu64
                " Stage 7 visits, %" PRIu64 " output bytes\n"
                "Candidates: %zu; sensitivities: %zu; interactions: %zu; "
                "warnings: %zu\n"
                "These associations do not prove a causal effect.\n",
                HWA_EXPERIMENT_METHOD_VERSION,
                hwa_experiment_plan_name(result->plan_kind),
                result->plan_seed, result->plan_replicates,
                result->input_count, result->parameter_count,
                result->level_count, result->case_count,
                result->response_count, result->point_count,
                result->job_count, canonical_retained,
                result->total_run_evaluations, result->total_output_bytes,
                result->candidate_count, result->sensitivity_count,
                result->interaction_count, result->warning_count) < 0) {
        return -1;
    }
    for (index = 0U; index < result->candidate_count; ++index) {
        const HWAExperimentCandidate *r = &result->candidates[index];
        if (fprintf(stream,
                    "Candidate %" PRIu64 ": point %" PRIu64
                    ", response %" PRIu64 ", %s, %s",
                    r->id, r->point_id, r->response_id,
                    hwa_experiment_split_name(r->split),
                    hwa_run_availability_name(r->availability)) < 0) return -1;
        if (r->values_valid &&
            fprintf(stream, ", mean gap %.6g, improvement %.6g, "
                    "worst harm %.6g", r->mean_gap,
                    r->improvement, r->worst_harm) < 0) return -1;
        if (fputc('\n', stream) == EOF) return -1;
    }
    for (index = 0U; index < result->sensitivity_count; ++index) {
        const HWAExperimentSensitivity *r = &result->sensitivities[index];
        if (fprintf(stream,
                    "Sensitivity %" PRIu64 ": parameter %" PRIu64
                    ", response %" PRIu64 ", %s, %s, %s",
                    r->id, r->parameter_id, r->response_id,
                    hwa_experiment_split_name(r->split),
                    hwa_run_availability_name(r->availability),
                    hwa_experiment_monotonicity_name(r->monotonicity)) < 0) {
            return -1;
        }
        if (r->linear_valid &&
            fprintf(stream, ", slope %.6g, Pearson %.6g, R2 %.6g",
                    r->slope, r->pearson, r->linear_r_squared) < 0) return -1;
        if (fprintf(stream, ", range %.6g", r->response_range) < 0) return -1;
        if (r->effect_valid &&
            fprintf(stream, ", effect %.6g", r->effect_fraction) < 0) return -1;
        if (r->noise_valid &&
            fprintf(stream, ", replicate noise SD %.6g",
                    r->noise_sd) < 0) return -1;
        if (fputc('\n', stream) == EOF) return -1;
    }
    for (index = 0U; index < result->interaction_count; ++index) {
        const HWAExperimentInteraction *r = &result->interactions[index];
        if (fprintf(stream,
                    "Interaction %" PRIu64 ": parameters %" PRIu64
                    " and %" PRIu64 ", response %" PRIu64 ", %s, %s",
                    r->id, r->left_parameter_id, r->right_parameter_id,
                    r->response_id, hwa_experiment_split_name(r->split),
                    hwa_run_availability_name(r->availability)) < 0) return -1;
        if (r->effect_valid &&
            fprintf(stream, ", effect %.6g", r->effect_fraction) < 0) {
            return -1;
        }
        if (fputc('\n', stream) == EOF) return -1;
    }
    for (index = 0U; index < result->warning_count; ++index) {
        if (fprintf(stream, "Warning %s: %s\n",
                    result->warnings[index].code,
                    result->warnings[index].message) < 0) return -1;
    }
    return 0;
}

static int hwa_er_with_locale(
    FILE *stream,
    const HWAExperimentResult *result,
    int (*writer)(FILE *, const HWAExperimentResult *))
{
    HWANumericLocale locale;
    int status;
    if (stream == NULL || result == NULL ||
        hwa_experiment_result_validate(result, NULL, 0U) != 0 ||
        hwa_c_numeric_locale_begin(&locale) != 0) return -1;
    status = writer(stream, result);
    if (hwa_c_numeric_locale_end(&locale) != 0) return -1;
    return status;
}

int hwa_report_experiment_text(FILE *stream,
                               const HWAExperimentResult *result)
{
    return hwa_er_with_locale(stream, result, hwa_er_text_impl);
}

int hwa_report_experiment_json(FILE *stream,
                               const HWAExperimentResult *result)
{
    return hwa_er_with_locale(stream, result, hwa_er_json_impl);
}

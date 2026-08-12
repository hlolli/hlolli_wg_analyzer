#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "experiment_file.h"

#include "alignment_file.h"
#include "internal.h"
#include "numeric_locale.h"
#include "run.h"
#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define HWA_E_MAX_FIELDS 32U
#define HWA_E_MAX_FIELD_BYTES 65536U
#define HWA_E_RUN_QUALITY_ALL ((UINT32_C(1) << 5U) - UINT32_C(1))

typedef struct HWAExperimentIdentity {
    uint64_t device;
    uint64_t file;
    uint64_t size;
} HWAExperimentIdentity;

static int hwa_e_identity_path(const char *path, HWAExperimentIdentity *id);
static int hwa_e_identity_stream(FILE *stream, HWAExperimentIdentity *id);
static int hwa_e_identity_equal(const HWAExperimentIdentity *a,
                                const HWAExperimentIdentity *b);

typedef struct HWAECsvField {
    char *text;
    const unsigned char *raw;
    size_t raw_size;
    int quoted;
} HWAECsvField;

typedef struct HWAECsvRow {
    HWAECsvField fields[HWA_E_MAX_FIELDS];
    size_t count;
} HWAECsvRow;

typedef struct HWAECsvReader {
    const unsigned char *data;
    size_t size;
    size_t cursor;
} HWAECsvReader;

typedef struct HWAESavedMeta {
    HWAExperimentOptions options;
    HWAExperimentPlanKind plan_kind;
    uint64_t plan_seed;
    size_t plan_replicates;
    char manifest_sha256[HWA_SHA256_HEX_SIZE];
    char *renderer_id;
    char renderer_sha256[HWA_SHA256_HEX_SIZE];
    uint64_t retained_work_bytes;
    uint64_t total_run_evaluations;
    uint64_t total_output_bytes;
    size_t counts[14];
} HWAESavedMeta;

typedef struct HWAEMetaKey {
    const char *key;
    const char *unit;
} HWAEMetaKey;

enum {
    HWA_E_INPUTS,
    HWA_E_PARAMETERS,
    HWA_E_LEVELS,
    HWA_E_CASES,
    HWA_E_RESPONSES,
    HWA_E_POINTS,
    HWA_E_VALUES,
    HWA_E_JOBS,
    HWA_E_ARTIFACTS,
    HWA_E_OBSERVATIONS,
    HWA_E_CANDIDATES,
    HWA_E_SENSITIVITIES,
    HWA_E_INTERACTIONS,
    HWA_E_WARNINGS,
    HWA_E_ARRAY_COUNT
};

static const HWAEMetaKey hwa_e_meta_keys[] = {
    {"tool_version", ""},
    {"experiment_method_version", ""},
    {"experiment_file_schema_version", ""},
    {"plan_kind", ""},
    {"plan_seed", ""},
    {"plan_replicates", "replicates"},
    {"manifest_sha256", ""},
    {"renderer_id", ""},
    {"renderer_sha256", ""},
    {"build_compiler_family", ""},
    {"build_compiler_version", ""},
    {"build_c_standard", ""},
    {"build_target_os", ""},
    {"build_pointer_bits", "bits"},
    {"build_endianness", ""},
    {"build_mode", ""},
    {"run_decode_block_frames", "frames"},
    {"run_max_manifest_bytes", "bytes"},
    {"run_max_input_bytes", "bytes"},
    {"run_max_input_frames", "frames"},
    {"run_max_probe_bytes", "bytes"},
    {"run_max_probe_values", "values"},
    {"run_max_work_bytes", "bytes"},
    {"run_max_evaluations", "evaluations"},
    {"run_max_stems", "stems"},
    {"run_max_probes", "probes"},
    {"run_max_links", "links"},
    {"run_max_json_depth", "levels"},
    {"run_max_json_tokens", "tokens"},
    {"run_max_result_rows", "rows"},
    {"run_max_warnings", "warnings"},
    {"max_manifest_bytes", "bytes"},
    {"max_input_bytes", "bytes"},
    {"max_work_bytes", "bytes"},
    {"max_bundle_bytes", "bytes"},
    {"max_output_file_bytes", "bytes"},
    {"max_total_run_evaluations", "evaluations"},
    {"max_job_milliseconds", "milliseconds"},
    {"max_total_milliseconds", "milliseconds"},
    {"max_parameters", "parameters"},
    {"max_levels", "levels"},
    {"max_cases", "cases"},
    {"max_responses", "responses"},
    {"max_points", "points"},
    {"max_jobs", "jobs"},
    {"max_replicates", "replicates"},
    {"max_artifacts", "artifacts"},
    {"max_observations", "observations"},
    {"max_sensitivities", "sensitivities"},
    {"max_interactions", "interactions"},
    {"max_warnings", "warnings"},
    {"retained_work_bytes", "bytes"},
    {"total_run_evaluations", "evaluations"},
    {"total_output_bytes", "bytes"},
    {"input_count", "inputs"},
    {"parameter_count", "parameters"},
    {"level_count", "levels"},
    {"case_count", "cases"},
    {"response_count", "responses"},
    {"point_count", "points"},
    {"value_count", "values"},
    {"job_count", "jobs"},
    {"artifact_count", "artifacts"},
    {"observation_count", "observations"},
    {"candidate_count", "candidates"},
    {"sensitivity_count", "sensitivities"},
    {"interaction_count", "interactions"},
    {"warning_count", "warnings"}
};

static void hwa_e_error(char *error, size_t size, const char *message)
{
    if (error != NULL && size != 0U) {
        (void)snprintf(error, size, "%s", message);
    }
}

static int hwa_e_csv_field(FILE *stream, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    int quote = 0;
    if (text == NULL) return -1;
    while (*cursor != 0U) {
        if (*cursor == ',' || *cursor == '"' ||
            *cursor == '\r' || *cursor == '\n') {
            quote = 1;
            break;
        }
        cursor++;
    }
    if (!quote) return fputs(text, stream) == EOF ? -1 : 0;
    if (fputc('"', stream) == EOF) return -1;
    cursor = (const unsigned char *)text;
    while (*cursor != 0U) {
        if (*cursor == '"' && fputc('"', stream) == EOF) return -1;
        if (fputc((int)*cursor++, stream) == EOF) return -1;
    }
    return fputc('"', stream) == EOF ? -1 : 0;
}

static int hwa_e_path_hex(FILE *stream, const char *path)
{
    const unsigned char *cursor = (const unsigned char *)path;
    if (path == NULL) return -1;
    while (*cursor != 0U) {
        if (fprintf(stream, "%02x", (unsigned)*cursor++) < 0) return -1;
    }
    return 0;
}

static int hwa_e_number(FILE *stream, double value)
{
    if (!isfinite(value)) return -1;
    return fprintf(stream, "%.17g", value == 0.0 ? 0.0 : value) < 0 ? -1 : 0;
}

static int hwa_e_optional_number(FILE *stream, double value, int valid)
{
    return valid ? hwa_e_number(stream, value) : 0;
}

static int hwa_e_meta_text(FILE *stream,
                           const char *key,
                           const char *value,
                           const char *unit)
{
    return fputs("META,", stream) == EOF ||
           hwa_e_csv_field(stream, key) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_e_csv_field(stream, value) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_e_csv_field(stream, unit) != 0 ||
           fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_e_meta_u64(FILE *stream,
                          const char *key,
                          uint64_t value,
                          const char *unit)
{
    char text[32];
    int length = snprintf(text, sizeof(text), "%" PRIu64, value);
    return length < 0 || (size_t)length >= sizeof(text) ?
           -1 : hwa_e_meta_text(stream, key, text, unit);
}

static int hwa_e_meta_size(FILE *stream,
                           const char *key,
                           size_t value,
                           const char *unit)
{
    char text[32];
    int length = snprintf(text, sizeof(text), "%zu", value);
    return length < 0 || (size_t)length >= sizeof(text) ?
           -1 : hwa_e_meta_text(stream, key, text, unit);
}

static int hwa_e_write_meta(FILE *stream,
                            const HWAExperimentResult *result,
                            uint64_t canonical_retained)
{
    const HWAExperimentOptions *o = &result->options;
    const HWARunOptions *r = &o->run;
#define ET(key, value, unit) do { if (hwa_e_meta_text(                     \
    stream, key, value, unit) != 0) return -1; } while (0)
#define EU(key, value, unit) do { if (hwa_e_meta_u64(                      \
    stream, key, value, unit) != 0) return -1; } while (0)
#define ES(key, value, unit) do { if (hwa_e_meta_size(                     \
    stream, key, value, unit) != 0) return -1; } while (0)
    ET("tool_version", HWA_VERSION, "");
    ET("experiment_method_version", HWA_EXPERIMENT_METHOD_VERSION, "");
    EU("experiment_file_schema_version", HWA_EXPERIMENT_FILE_SCHEMA_VERSION,
       "");
    ET("plan_kind", hwa_experiment_plan_name(result->plan_kind), "");
    EU("plan_seed", result->plan_seed, "");
    ES("plan_replicates", result->plan_replicates, "replicates");
    ET("manifest_sha256", result->manifest_sha256, "");
    ET("renderer_id", result->renderer_id, "");
    ET("renderer_sha256", result->renderer_sha256, "");
    ET("build_compiler_family", hwa_build_compiler_family(), "");
    ET("build_compiler_version", hwa_build_compiler_version(), "");
    ET("build_c_standard", hwa_build_c_standard(), "");
    ET("build_target_os", hwa_build_target_os(), "");
    EU("build_pointer_bits", hwa_build_pointer_bits(), "bits");
    ET("build_endianness", hwa_build_endianness(), "");
    ET("build_mode", hwa_build_mode(), "");
    ES("run_decode_block_frames", r->decode_block_frames, "frames");
    EU("run_max_manifest_bytes", r->max_manifest_bytes, "bytes");
    EU("run_max_input_bytes", r->max_input_bytes, "bytes");
    EU("run_max_input_frames", r->max_input_frames, "frames");
    EU("run_max_probe_bytes", r->max_probe_bytes, "bytes");
    EU("run_max_probe_values", r->max_probe_values, "values");
    EU("run_max_work_bytes", r->max_work_bytes, "bytes");
    EU("run_max_evaluations", r->max_evaluations, "evaluations");
    ES("run_max_stems", r->max_stems, "stems");
    ES("run_max_probes", r->max_probes, "probes");
    ES("run_max_links", r->max_links, "links");
    ES("run_max_json_depth", r->max_json_depth, "levels");
    ES("run_max_json_tokens", r->max_json_tokens, "tokens");
    ES("run_max_result_rows", r->max_result_rows, "rows");
    ES("run_max_warnings", r->max_warnings, "warnings");
    EU("max_manifest_bytes", o->max_manifest_bytes, "bytes");
    EU("max_input_bytes", o->max_input_bytes, "bytes");
    EU("max_work_bytes", o->max_work_bytes, "bytes");
    EU("max_bundle_bytes", o->max_bundle_bytes, "bytes");
    EU("max_output_file_bytes", o->max_output_file_bytes, "bytes");
    EU("max_total_run_evaluations", o->max_total_run_evaluations,
       "evaluations");
    EU("max_job_milliseconds", o->max_job_milliseconds, "milliseconds");
    EU("max_total_milliseconds", o->max_total_milliseconds, "milliseconds");
    ES("max_parameters", o->max_parameters, "parameters");
    ES("max_levels", o->max_levels, "levels");
    ES("max_cases", o->max_cases, "cases");
    ES("max_responses", o->max_responses, "responses");
    ES("max_points", o->max_points, "points");
    ES("max_jobs", o->max_jobs, "jobs");
    ES("max_replicates", o->max_replicates, "replicates");
    ES("max_artifacts", o->max_artifacts, "artifacts");
    ES("max_observations", o->max_observations, "observations");
    ES("max_sensitivities", o->max_sensitivities, "sensitivities");
    ES("max_interactions", o->max_interactions, "interactions");
    ES("max_warnings", o->max_warnings, "warnings");
    EU("retained_work_bytes", canonical_retained, "bytes");
    EU("total_run_evaluations", result->total_run_evaluations,
       "evaluations");
    EU("total_output_bytes", result->total_output_bytes, "bytes");
    ES("input_count", result->input_count, "inputs");
    ES("parameter_count", result->parameter_count, "parameters");
    ES("level_count", result->level_count, "levels");
    ES("case_count", result->case_count, "cases");
    ES("response_count", result->response_count, "responses");
    ES("point_count", result->point_count, "points");
    ES("value_count", result->value_count, "values");
    ES("job_count", result->job_count, "jobs");
    ES("artifact_count", result->artifact_count, "artifacts");
    ES("observation_count", result->observation_count, "observations");
    ES("candidate_count", result->candidate_count, "candidates");
    ES("sensitivity_count", result->sensitivity_count, "sensitivities");
    ES("interaction_count", result->interaction_count, "interactions");
    ES("warning_count", result->warning_count, "warnings");
#undef ES
#undef EU
#undef ET
    return 0;
}

static int hwa_e_write_input(FILE *stream, const HWAExperimentInput *r)
{
    return fprintf(stream, "INPUT,%" PRIu64 ",", r->id) < 0 ||
           hwa_e_csv_field(stream, r->binding_id) != 0 ||
           fprintf(stream, ",%s,%" PRIu64 "\r\n",
                   r->sha256, r->file_bytes) < 0 ? -1 : 0;
}

static int hwa_e_write_parameter(FILE *stream,
                                 const HWAExperimentParameter *r)
{
    if (fprintf(stream, "PARAMETER,%" PRIu64 ",", r->id) < 0 ||
        hwa_e_csv_field(stream, r->name) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_e_csv_field(stream, r->unit) != 0 ||
        fputc(',', stream) == EOF || hwa_e_number(stream, r->minimum) != 0 ||
        fputc(',', stream) == EOF || hwa_e_number(stream, r->maximum) != 0 ||
        fputc(',', stream) == EOF || hwa_e_number(stream, r->baseline) != 0 ||
        fprintf(stream, ",%zu,%zu\r\n",
                r->first_level, r->level_count) < 0) return -1;
    return 0;
}

static int hwa_e_write_level(FILE *stream, const HWAExperimentLevel *r)
{
    return fprintf(stream, "LEVEL,%" PRIu64 ",%" PRIu64 ",",
                   r->id, r->parameter_id) < 0 ||
           hwa_e_number(stream, r->value) != 0 ||
           fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_e_write_case(FILE *stream, const HWAExperimentCase *r)
{
    return fprintf(stream, "CASE,%" PRIu64 ",", r->id) < 0 ||
           hwa_e_csv_field(stream, r->name) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_e_csv_field(stream, hwa_experiment_split_name(r->split)) != 0 ||
           fputc(',', stream) == EOF || hwa_e_number(stream, r->weight) != 0 ||
           fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_e_write_response(FILE *stream,
                                const HWAExperimentResponse *r)
{
    return fprintf(stream, "RESPONSE,%" PRIu64 ",", r->id) < 0 ||
           hwa_e_csv_field(stream, r->name) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_e_csv_field(stream, hwa_run_stem_role_name(r->role)) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_e_csv_field(stream, hwa_run_feature_kind_name(r->feature)) != 0 ||
           fprintf(stream, ",%" PRIu32 "\r\n", r->feature_index) < 0 ? -1 : 0;
}

static int hwa_e_write_point(FILE *stream, const HWAExperimentPoint *r)
{
    return fprintf(stream, "POINT,%" PRIu64 ",%s,%d\r\n",
                   r->id, r->key, r->baseline ? 1 : 0) < 0 ? -1 : 0;
}

static int hwa_e_write_value(FILE *stream, const HWAExperimentValue *r)
{
    return fprintf(stream, "VALUE,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",",
                   r->id, r->point_id, r->parameter_id) < 0 ||
           hwa_e_number(stream, r->value) != 0 ||
           fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_e_write_job(FILE *stream, const HWAExperimentJob *r)
{
    const char *path = r->run_result_path;
    if (path == NULL || strncmp(path, "jobs/", 5U) != 0) return -1;
    if (fprintf(stream,
                "JOB,%" PRIu64 ",%s,%" PRIu64 ",%" PRIu64
                ",%zu,%" PRIu64 ",",
                r->id, r->key, r->point_id, r->case_id,
                r->replicate, r->seed) < 0 ||
        hwa_e_path_hex(stream, path) != 0 ||
        fprintf(stream,
                ",%s,%s,%" PRIu64 ",%" PRIu64 "\r\n",
                r->run_manifest_sha256, r->run_result_sha256,
                r->output_bytes, r->run_evaluations) < 0) return -1;
    return 0;
}

static int hwa_e_write_artifact(FILE *stream,
                                const HWAExperimentArtifact *r)
{
    const char *path = r->path;
    if (path == NULL || strncmp(path, "jobs/", 5U) != 0) return -1;
    return fprintf(stream, "ARTIFACT,%" PRIu64 ",%" PRIu64 ",",
                   r->id, r->job_id) < 0 ||
           hwa_e_csv_field(stream, r->resource_id) != 0 ||
           fputc(',', stream) == EOF || hwa_e_path_hex(stream, path) != 0 ||
           fprintf(stream, ",%s,%" PRIu64 ",",
                   r->sha256, r->file_bytes) < 0 ||
           hwa_e_csv_field(stream, hwa_run_source_kind_name(r->kind)) != 0 ||
           fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_e_write_observation(FILE *stream,
                                   const HWAExperimentObservation *r)
{
    if (fprintf(stream, "OBSERVATION,%" PRIu64 ",%" PRIu64
                ",%" PRIu64 ",", r->id, r->job_id, r->response_id) < 0 ||
        hwa_e_csv_field(stream,
                        hwa_run_availability_name(r->availability)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_e_optional_number(stream, r->value, r->value_valid) != 0 ||
        fprintf(stream, ",%" PRIu32 ",%d\r\n",
                r->quality_flags, r->value_valid ? 1 : 0) < 0) return -1;
    return 0;
}

static int hwa_e_write_candidate(FILE *stream,
                                 const HWAExperimentCandidate *r)
{
    if (fprintf(stream, "CANDIDATE,%" PRIu64 ",%" PRIu64
                ",%" PRIu64 ",", r->id, r->point_id, r->response_id) < 0 ||
        hwa_e_csv_field(stream, hwa_experiment_split_name(r->split)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_e_csv_field(stream,
                        hwa_run_availability_name(r->availability)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_e_optional_number(stream, r->mean_gap, r->values_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_e_optional_number(stream, r->improvement, r->values_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_e_optional_number(stream, r->worst_harm, r->values_valid) != 0 ||
        fprintf(stream, ",%zu,%" PRIu32 ",%d\r\n",
                r->case_count, r->quality_flags,
                r->values_valid ? 1 : 0) < 0) return -1;
    return 0;
}

static int hwa_e_write_sensitivity(FILE *stream,
                                   const HWAExperimentSensitivity *r)
{
    if (fprintf(stream, "SENSITIVITY,%" PRIu64 ",%" PRIu64
                ",%" PRIu64 ",", r->id, r->parameter_id,
                r->response_id) < 0 ||
        hwa_e_csv_field(stream, hwa_experiment_split_name(r->split)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_e_csv_field(stream,
                        hwa_run_availability_name(r->availability)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_e_optional_number(stream, r->slope, r->linear_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_e_optional_number(stream, r->pearson, r->linear_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_e_optional_number(stream, r->linear_r_squared,
                              r->linear_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_e_optional_number(stream, r->effect_fraction,
                              r->effect_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_e_number(stream, r->response_range) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_e_optional_number(stream, r->noise_sd, r->noise_valid) != 0 ||
        fprintf(stream, ",%zu,", r->point_count) < 0 ||
        hwa_e_csv_field(stream,
                        hwa_experiment_monotonicity_name(r->monotonicity)) != 0 ||
        fprintf(stream, ",%" PRIu32 ",%d,%d,%d\r\n",
                r->quality_flags, r->linear_valid ? 1 : 0,
                r->effect_valid ? 1 : 0,
                r->noise_valid ? 1 : 0) < 0) return -1;
    return 0;
}

static int hwa_e_write_interaction(FILE *stream,
                                   const HWAExperimentInteraction *r)
{
    if (fprintf(stream, "INTERACTION,%" PRIu64 ",%" PRIu64
                ",%" PRIu64 ",%" PRIu64 ",",
                r->id, r->left_parameter_id, r->right_parameter_id,
                r->response_id) < 0 ||
        hwa_e_csv_field(stream, hwa_experiment_split_name(r->split)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_e_csv_field(stream,
                        hwa_run_availability_name(r->availability)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_e_optional_number(stream, r->effect_fraction,
                              r->effect_valid) != 0 ||
        fprintf(stream, ",%zu,%d\r\n", r->point_count,
                r->effect_valid ? 1 : 0) < 0) return -1;
    return 0;
}

static int hwa_e_write_warning(FILE *stream,
                               const HWAExperimentWarning *r)
{
    if (fprintf(stream, "WARNING,%" PRIu64 ",", r->id) < 0 ||
        hwa_e_csv_field(stream, r->code) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_e_csv_field(stream, r->message) != 0 ||
        fputc(',', stream) == EOF) return -1;
    if (r->job_id_valid &&
        fprintf(stream, "%" PRIu64, r->job_id) < 0) return -1;
    if (fputc(',', stream) == EOF) return -1;
    if (r->point_id_valid &&
        fprintf(stream, "%" PRIu64, r->point_id) < 0) return -1;
    if (fputc(',', stream) == EOF) return -1;
    if (r->parameter_id_valid &&
        fprintf(stream, "%" PRIu64, r->parameter_id) < 0) return -1;
    if (fputc(',', stream) == EOF) return -1;
    if (r->response_id_valid &&
        fprintf(stream, "%" PRIu64, r->response_id) < 0) return -1;
    return fprintf(stream, ",%d,%d,%d,%d\r\n",
                   r->job_id_valid ? 1 : 0,
                   r->point_id_valid ? 1 : 0,
                   r->parameter_id_valid ? 1 : 0,
                   r->response_id_valid ? 1 : 0) < 0 ? -1 : 0;
}

static int hwa_e_token_valid(const char *text)
{
    size_t index;
    size_t length;
    unsigned char first;
    if (text == NULL || text[0] == '\0') return 0;
    length = strlen(text);
    if (length > 127U) return 0;
    first = (unsigned char)text[0];
    if (!((first >= '0' && first <= '9') ||
          (first >= 'A' && first <= 'Z') ||
          (first >= 'a' && first <= 'z'))) return 0;
    for (index = 0U; index < length; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'A' && value <= 'Z') ||
              (value >= 'a' && value <= 'z') || value == '.' ||
              value == '_' || value == ':' || value == '/' || value == '-')) {
            return 0;
        }
    }
    return 1;
}

static int hwa_e_unit_valid(const char *text)
{
    size_t index;
    size_t length;
    if (text == NULL) return 0;
    length = strlen(text);
    if (length == 0U || length > 31U) return 0;
    for (index = 0U; index < length; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (value < 0x21U || value > 0x7eU || value == ',' || value == '"') {
            return 0;
        }
    }
    return 1;
}

static int hwa_e_component_valid(const char *text)
{
    size_t index;
    size_t length;
    if (text == NULL || text[0] == '\0' || strcmp(text, ".") == 0 ||
        strcmp(text, "..") == 0) return 0;
    length = strlen(text);
    if (length > 127U) return 0;
    for (index = 0U; index < length; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'A' && value <= 'Z') ||
              (value >= 'a' && value <= 'z') || value == '.' ||
              value == '_' || value == '-')) return 0;
    }
    return 1;
}

static int hwa_e_job_path_valid(const HWAExperimentJob *job)
{
    char expected[96];
    int length;
    if (job == NULL || job->run_result_path == NULL) return 0;
    length = snprintf(expected, sizeof(expected), "jobs/%s/result.hwa-run",
                      job->key);
    return length >= 0 && (size_t)length < sizeof(expected) &&
           strcmp(job->run_result_path, expected) == 0;
}

static int hwa_e_artifact_path_valid(const HWAExperimentResult *result,
                                     const HWAExperimentArtifact *artifact)
{
    char prefix[80];
    int length;
    if (result == NULL || artifact == NULL || artifact->path == NULL ||
        artifact->job_id == 0U || artifact->job_id > result->job_count) {
        return 0;
    }
    length = snprintf(prefix, sizeof(prefix), "jobs/%s/",
                      result->jobs[artifact->job_id - 1U].key);
    return length >= 0 && (size_t)length < sizeof(prefix) &&
           strncmp(artifact->path, prefix, (size_t)length) == 0 &&
           hwa_e_component_valid(artifact->path + (size_t)length);
}

static int hwa_e_raw_rows_valid(const HWAExperimentResult *result,
                                const HWAExperimentOptions *limits,
                                char *error,
                                size_t error_size)
{
    size_t expected_level = 0U;
    size_t expected_points = 1U;
    size_t artifact_index = 0U;
    size_t fit_count = 0U;
    size_t check_count = 0U;
    double fit_weight = 0.0;
    double check_weight = 0.0;
    size_t index;
    if (result == NULL || limits == NULL || result->input_count == 0U ||
        result->input_count > limits->max_artifacts ||
        result->parameter_count == 0U || result->case_count < 2U ||
        result->response_count == 0U || result->point_count == 0U ||
        result->plan_kind <= 0 ||
        result->plan_kind >= HWA_EXPERIMENT_PLAN_KIND_COUNT ||
        result->plan_replicates == 0U ||
        result->inputs == NULL || result->parameters == NULL ||
        (result->level_count != 0U && result->levels == NULL) ||
        result->cases == NULL || result->responses == NULL ||
        result->points == NULL || result->values == NULL ||
        result->jobs == NULL || result->observations == NULL ||
        (result->artifact_count != 0U && result->artifacts == NULL)) goto invalid;
    for (index = 0U; index < result->input_count; ++index) {
        const HWAExperimentInput *input = &result->inputs[index];
        if (input->id != (uint64_t)index + UINT64_C(1) ||
            !hwa_e_token_valid(input->binding_id) || input->file_bytes == 0U ||
            input->file_bytes > limits->max_input_bytes ||
            (index != 0U && strcmp(result->inputs[index - 1U].binding_id,
                                   input->binding_id) >= 0)) goto invalid;
    }
    for (index = 0U; index < result->parameter_count; ++index) {
        const HWAExperimentParameter *parameter = &result->parameters[index];
        size_t level;
        int baseline_seen = 0;
        if (parameter->id != (uint64_t)index + UINT64_C(1) ||
            !hwa_e_token_valid(parameter->name) ||
            !hwa_e_unit_valid(parameter->unit) ||
            !isfinite(parameter->minimum) || !isfinite(parameter->maximum) ||
            !isfinite(parameter->baseline) ||
            !isfinite(parameter->maximum - parameter->minimum) ||
            parameter->minimum > parameter->baseline ||
            parameter->baseline > parameter->maximum ||
            parameter->first_level != expected_level ||
            parameter->level_count > result->level_count - expected_level ||
            (index != 0U && strcmp(result->parameters[index - 1U].name,
                                   parameter->name) >= 0)) goto invalid;
        if (result->plan_kind == HWA_EXPERIMENT_RANDOM) {
            if (!(parameter->minimum < parameter->maximum) ||
                parameter->level_count != 0U) goto invalid;
        } else if (parameter->level_count == 0U) {
            goto invalid;
        }
        for (level = 0U; level < parameter->level_count; ++level) {
            const HWAExperimentLevel *row =
                &result->levels[expected_level + level];
            if (row->id != (uint64_t)(expected_level + level) + UINT64_C(1) ||
                row->parameter_id != parameter->id || !isfinite(row->value) ||
                row->value < parameter->minimum ||
                row->value > parameter->maximum ||
                (level != 0U &&
                 result->levels[expected_level + level - 1U].value >=
                     row->value)) goto invalid;
            if (row->value == parameter->baseline) baseline_seen = 1;
        }
        if (result->plan_kind != HWA_EXPERIMENT_RANDOM && !baseline_seen) {
            goto invalid;
        }
        if (result->plan_kind == HWA_EXPERIMENT_ONE_AT_A_TIME) {
            size_t add = parameter->level_count - 1U;
            if (add > SIZE_MAX - expected_points) goto invalid;
            expected_points += add;
        } else if (result->plan_kind == HWA_EXPERIMENT_GRID) {
            if (expected_points > SIZE_MAX / parameter->level_count) goto invalid;
            expected_points *= parameter->level_count;
        }
        expected_level += parameter->level_count;
    }
    if (expected_level != result->level_count ||
        (result->plan_kind != HWA_EXPERIMENT_RANDOM &&
         expected_points != result->point_count)) goto invalid;
    for (index = 0U; index < result->case_count; ++index) {
        const HWAExperimentCase *record = &result->cases[index];
        if (record->id != (uint64_t)index + UINT64_C(1) ||
            !hwa_e_token_valid(record->name) || !isfinite(record->weight) ||
            !(record->weight > 0.0) ||
            (index != 0U && strcmp(result->cases[index - 1U].name,
                                   record->name) >= 0)) goto invalid;
        if (record->split == HWA_EXPERIMENT_FIT) {
            if (!isfinite(fit_weight + record->weight)) goto invalid;
            fit_weight += record->weight;
            fit_count++;
        } else if (record->split == HWA_EXPERIMENT_CHECK) {
            if (!isfinite(check_weight + record->weight)) goto invalid;
            check_weight += record->weight;
            check_count++;
        }
        else goto invalid;
    }
    if (fit_count == 0U || check_count == 0U) goto invalid;
    for (index = 0U; index < result->response_count; ++index) {
        const HWAExperimentResponse *response = &result->responses[index];
        if (response->id != (uint64_t)index + UINT64_C(1) ||
            !hwa_e_token_valid(response->name) || response->role <= 0 ||
            response->role >= HWA_RUN_STEM_ROLE_COUNT ||
            response->feature <= 0 ||
            response->feature >= HWA_RUN_FEATURE_KIND_COUNT ||
            (response->feature != HWA_RUN_FEATURE_BAND_LEVEL_DBFS &&
             response->feature_index != 0U) ||
            (response->feature == HWA_RUN_FEATURE_BAND_LEVEL_DBFS &&
             response->feature_index >= HWA_BAND_COUNT) ||
            (index != 0U && strcmp(result->responses[index - 1U].name,
                                   response->name) >= 0)) goto invalid;
    }
    for (index = 0U; index < result->point_count; ++index) {
        if (result->points[index].id != (uint64_t)index + UINT64_C(1) ||
            result->points[index].baseline != (index == 0U)) goto invalid;
    }
    if (result->parameter_count > SIZE_MAX / result->point_count ||
        result->value_count != result->parameter_count * result->point_count) {
        goto invalid;
    }
    for (index = 0U; index < result->value_count; ++index) {
        const HWAExperimentValue *value = &result->values[index];
        size_t point = index / result->parameter_count;
        size_t parameter = index % result->parameter_count;
        if (value->id != (uint64_t)index + UINT64_C(1) ||
            value->point_id != (uint64_t)point + UINT64_C(1) ||
            value->parameter_id != (uint64_t)parameter + UINT64_C(1) ||
            !isfinite(value->value) ||
            value->value < result->parameters[parameter].minimum ||
            value->value > result->parameters[parameter].maximum ||
            (point == 0U &&
             value->value != result->parameters[parameter].baseline)) {
            goto invalid;
        }
    }
    if (result->point_count > SIZE_MAX / result->case_count ||
        result->point_count * result->case_count >
            SIZE_MAX / result->plan_replicates ||
        result->job_count != result->point_count * result->case_count *
                                 result->plan_replicates) goto invalid;
    for (index = 0U; index < result->job_count; ++index) {
        const HWAExperimentJob *job = &result->jobs[index];
        size_t replicate = index % result->plan_replicates;
        size_t cell = index / result->plan_replicates;
        size_t case_index = cell % result->case_count;
        size_t point = cell / result->case_count;
        uint64_t output = 0U;
        if (job->id != (uint64_t)index + UINT64_C(1) ||
            job->point_id != (uint64_t)point + UINT64_C(1) ||
            job->case_id != (uint64_t)case_index + UINT64_C(1) ||
            job->replicate != replicate || !hwa_e_job_path_valid(job) ||
            job->run_evaluations > limits->run.max_evaluations) goto invalid;
        while (artifact_index < result->artifact_count &&
               result->artifacts[artifact_index].job_id == job->id) {
            const HWAExperimentArtifact *artifact =
                &result->artifacts[artifact_index];
            if (artifact->id != (uint64_t)artifact_index + UINT64_C(1) ||
                !hwa_e_token_valid(artifact->resource_id) ||
                !hwa_e_artifact_path_valid(result, artifact) ||
                artifact->file_bytes == 0U ||
                artifact->file_bytes > limits->max_output_file_bytes ||
                (artifact->kind != HWA_RUN_SOURCE_STEM &&
                 artifact->kind != HWA_RUN_SOURCE_PROBE) ||
                artifact->file_bytes > UINT64_MAX - output) goto invalid;
            output += artifact->file_bytes;
            artifact_index++;
        }
        if (job->output_bytes != output) goto invalid;
    }
    if (artifact_index != result->artifact_count) goto invalid;
    if (result->response_count > SIZE_MAX / result->job_count ||
        result->observation_count !=
            result->response_count * result->job_count) goto invalid;
    for (index = 0U; index < result->observation_count; ++index) {
        const HWAExperimentObservation *observation =
            &result->observations[index];
        size_t job = index / result->response_count;
        size_t response = index % result->response_count;
        if (observation->id != (uint64_t)index + UINT64_C(1) ||
            observation->job_id != (uint64_t)job + UINT64_C(1) ||
            observation->response_id != (uint64_t)response + UINT64_C(1) ||
            observation->availability <= 0 ||
            observation->availability >= HWA_RUN_AVAILABILITY_COUNT ||
            (observation->quality_flags & ~HWA_E_RUN_QUALITY_ALL) != 0U ||
            (observation->value_valid &&
             (observation->availability != HWA_RUN_AVAILABLE ||
              !isfinite(observation->value) || observation->value < 0.0 ||
              observation->value > 1.0)) ||
            (!observation->value_valid &&
             (observation->availability == HWA_RUN_AVAILABLE ||
              observation->value != 0.0 || observation->quality_flags != 0U))) {
            goto invalid;
        }
    }
    return 0;
invalid:
    hwa_e_error(error, error_size,
                "experiment result has invalid or over-limit raw rows");
    return -1;
}

static int hwa_e_write_impl(FILE *stream,
                            const HWAExperimentResult *result,
                            const HWANumericLocale *locale,
                            char *error,
                            size_t error_size)
{
    size_t index;
    uint64_t canonical_retained;
    if (stream == NULL) {
        hwa_e_error(error, error_size, "experiment output stream is null");
        return -1;
    }
    if (result == NULL ||
        hwa_e_raw_rows_valid(result, &result->options,
                             error, error_size) != 0 ||
        hwa_experiment_result_validate_locale(
            result, locale, error, error_size) != 0) return -1;
    if (hwa_experiment_result_canonical_retained_bytes(
            result, &canonical_retained) != 0) goto write_error;
    if (fputs("HWA_EXPERIMENT,1\r\n", stream) == EOF ||
        hwa_e_write_meta(stream, result, canonical_retained) != 0) {
        goto write_error;
    }
#define EROWS(field, count, writer)                                        \
    do {                                                                   \
        for (index = 0U; index < result->count; ++index) {                 \
            if (writer(stream, &result->field[index]) != 0) goto write_error; \
        }                                                                  \
    } while (0)
    EROWS(inputs, input_count, hwa_e_write_input);
    EROWS(parameters, parameter_count, hwa_e_write_parameter);
    EROWS(levels, level_count, hwa_e_write_level);
    EROWS(cases, case_count, hwa_e_write_case);
    EROWS(responses, response_count, hwa_e_write_response);
    EROWS(points, point_count, hwa_e_write_point);
    EROWS(values, value_count, hwa_e_write_value);
    EROWS(jobs, job_count, hwa_e_write_job);
    EROWS(artifacts, artifact_count, hwa_e_write_artifact);
    EROWS(observations, observation_count, hwa_e_write_observation);
    EROWS(candidates, candidate_count, hwa_e_write_candidate);
    EROWS(sensitivities, sensitivity_count, hwa_e_write_sensitivity);
    EROWS(interactions, interaction_count, hwa_e_write_interaction);
    EROWS(warnings, warning_count, hwa_e_write_warning);
#undef EROWS
    return 0;
write_error:
    hwa_e_error(error, error_size, "cannot write experiment output");
    return -1;
}

static int hwa_e_write_capped(FILE *stream,
                              const HWAExperimentResult *result,
                              const HWANumericLocale *locale,
                              char *error,
                              size_t error_size)
{
    unsigned char buffer[65536];
    HWAExperimentIdentity identity;
    FILE *scratch;
    size_t count;
    uint64_t copied = 0U;
    int status = -1;
    if (stream == NULL) {
        hwa_e_error(error, error_size, "experiment output stream is null");
        return -1;
    }
    scratch = tmpfile();
    if (scratch == NULL) {
        hwa_e_error(error, error_size,
                    "cannot create private experiment output scratch space");
        return -1;
    }
    if (hwa_e_write_impl(scratch, result, locale, error, error_size) != 0 ||
        fflush(scratch) != 0 ||
        hwa_e_identity_stream(scratch, &identity) != 0) {
        if (error == NULL || error_size == 0U || error[0] == '\0') {
            hwa_e_error(error, error_size,
                        "cannot size the canonical experiment output");
        }
        goto cleanup;
    }
    if (identity.size > result->options.max_bundle_bytes ||
        result->total_output_bytes >
            result->options.max_bundle_bytes - identity.size ||
        identity.size > result->options.max_output_file_bytes) {
        hwa_e_error(error, error_size,
                    "canonical experiment output exceeds its file or bundle cap");
        goto cleanup;
    }
    if (fseek(scratch, 0L, SEEK_SET) != 0) {
        hwa_e_error(error, error_size,
                    "cannot rewind the canonical experiment output");
        goto cleanup;
    }
    while ((count = fread(buffer, 1U, sizeof(buffer), scratch)) != 0U) {
        if (fwrite(buffer, 1U, count, stream) != count ||
            (uint64_t)count > UINT64_MAX - copied) {
            hwa_e_error(error, error_size, "cannot write experiment output");
            goto cleanup;
        }
        copied += (uint64_t)count;
    }
    if (ferror(scratch) || copied != identity.size) {
        hwa_e_error(error, error_size,
                    "cannot read the canonical experiment output scratch file");
        goto cleanup;
    }
    status = 0;
cleanup:
    if (fclose(scratch) != 0 && status == 0) {
        hwa_e_error(error, error_size,
                    "cannot close the canonical experiment output scratch file");
        status = -1;
    }
    return status;
}

int hwa_experiment_file_write(FILE *stream,
                              const HWAExperimentResult *result,
                              char *error,
                              size_t error_size)
{
    HWANumericLocale locale;
    int status;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_e_error(error, error_size,
                    "cannot enter the C numeric locale for experiment output");
        return -1;
    }
    status = hwa_e_write_capped(stream, result, &locale,
                                error, error_size);
    if (hwa_c_numeric_locale_end(&locale) != 0) {
        if (status == 0) {
            hwa_e_error(error, error_size,
                        "cannot restore the numeric locale after experiment output");
        }
        return -1;
    }
    return status;
}

static int hwa_e_file_write_path_locale(
    const char *path,
    const HWAExperimentResult *result,
    const HWANumericLocale *locale,
    HWAExperimentIdentity *created,
    char *error,
    size_t error_size)
{
    FILE *stream;
    HWAExperimentIdentity owned;
    HWAExperimentIdentity current;
    int descriptor;
    int have_owned = 0;
    int path_owned = 0;
    int status;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (created != NULL) memset(created, 0, sizeof(*created));
    if (path == NULL || path[0] == '\0' || strcmp(path, "-") == 0 ||
        locale == NULL || !locale->active) {
        hwa_e_error(error, error_size,
                    "experiment output needs a new named path and C locale");
        return -1;
    }
#if defined(_WIN32)
    descriptor = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                       _S_IREAD | _S_IWRITE);
    stream = descriptor >= 0 ? _fdopen(descriptor, "wb") : NULL;
#else
    descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL
#ifdef O_NOFOLLOW
                      | O_NOFOLLOW
#endif
                      , S_IRUSR | S_IWUSR);
    stream = descriptor >= 0 ? fdopen(descriptor, "wb") : NULL;
#endif
    if (stream == NULL) {
        if (descriptor >= 0) {
#if defined(_WIN32)
            (void)_close(descriptor);
#else
            (void)close(descriptor);
#endif
        }
        hwa_e_error(error, error_size,
                    "cannot create the experiment output file");
        return -1;
    }
    if (hwa_e_identity_stream(stream, &owned) != 0 || owned.size != 0U ||
        hwa_e_identity_path(path, &current) != 0 ||
        !hwa_e_identity_equal(&owned, &current)) {
        have_owned = hwa_e_identity_stream(stream, &owned) == 0;
        (void)fclose(stream);
        if (have_owned && hwa_e_identity_path(path, &current) == 0 &&
            hwa_e_identity_equal(&owned, &current)) {
            (void)remove(path);
        }
        hwa_e_error(error, error_size,
                    "experiment output is not a new regular file");
        return -1;
    }
    have_owned = 1;
    status = hwa_e_write_capped(stream, result, locale, error, error_size);
    if (fflush(stream) != 0) status = -1;
#if defined(_WIN32)
    if (status == 0 && _commit(descriptor) != 0) status = -1;
#else
    if (status == 0 && fsync(descriptor) != 0) status = -1;
#endif
    if (hwa_e_identity_stream(stream, &owned) != 0) {
        have_owned = 0;
        status = -1;
    }
    if (fclose(stream) != 0) status = -1;
    path_owned = have_owned && hwa_e_identity_path(path, &current) == 0 &&
        hwa_e_identity_equal(&owned, &current);
    if (!path_owned) status = -1;
    if (status != 0) {
        if (path_owned) (void)remove(path);
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            hwa_e_error(error, error_size,
                        "cannot finish the experiment output file");
        }
        return -1;
    }
    if (created != NULL) *created = owned;
    return 0;
}

int hwa_experiment_file_write_path_locale(
    const char *path,
    const HWAExperimentResult *result,
    const HWANumericLocale *locale,
    char *error,
    size_t error_size)
{
    return hwa_e_file_write_path_locale(
        path, result, locale, NULL, error, error_size);
}

int hwa_experiment_file_write_path(const char *path,
                                   const HWAExperimentResult *result,
                                   char *error,
                                   size_t error_size)
{
    HWANumericLocale locale;
    HWAExperimentIdentity created;
    HWAExperimentIdentity current;
    int status;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_e_error(error, error_size,
                    "cannot enter the C numeric locale for experiment output");
        return -1;
    }
    status = hwa_e_file_write_path_locale(
        path, result, &locale, &created, error, error_size);
    if (hwa_c_numeric_locale_end(&locale) != 0) {
        if (status == 0 && hwa_e_identity_path(path, &current) == 0 &&
            hwa_e_identity_equal(&created, &current)) {
            (void)remove(path);
        }
        hwa_e_error(error, error_size,
                    "cannot restore the numeric locale after experiment output");
        return -1;
    }
    return status;
}

#if defined(_WIN32)
static int hwa_e_identity_path(const char *path, HWAExperimentIdentity *id)
{
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE handle = CreateFileA(
        path, 0U, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE ||
        GetFileType(handle) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(handle, &info)) {
        if (handle != INVALID_HANDLE_VALUE) (void)CloseHandle(handle);
        return -1;
    }
    (void)CloseHandle(handle);
    if ((info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        return -1;
    }
    id->device = (uint64_t)info.dwVolumeSerialNumber;
    id->file = ((uint64_t)info.nFileIndexHigh << 32U) |
               (uint64_t)info.nFileIndexLow;
    id->size = ((uint64_t)info.nFileSizeHigh << 32U) |
               (uint64_t)info.nFileSizeLow;
    return 0;
}

static int hwa_e_identity_stream(FILE *stream, HWAExperimentIdentity *id)
{
    BY_HANDLE_FILE_INFORMATION info;
    int descriptor = _fileno(stream);
    intptr_t raw = descriptor >= 0 ? _get_osfhandle(descriptor) : (intptr_t)-1;
    if (raw == (intptr_t)-1 ||
        GetFileType((HANDLE)raw) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle((HANDLE)raw, &info) ||
        (info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        return -1;
    }
    id->device = (uint64_t)info.dwVolumeSerialNumber;
    id->file = ((uint64_t)info.nFileIndexHigh << 32U) |
               (uint64_t)info.nFileIndexLow;
    id->size = ((uint64_t)info.nFileSizeHigh << 32U) |
               (uint64_t)info.nFileSizeLow;
    return 0;
}
#else
static int hwa_e_identity_path(const char *path, HWAExperimentIdentity *id)
{
    struct stat facts;
    if (lstat(path, &facts) != 0 || !S_ISREG(facts.st_mode) ||
        facts.st_size < 0) return -1;
    id->device = (uint64_t)facts.st_dev;
    id->file = (uint64_t)facts.st_ino;
    id->size = (uint64_t)facts.st_size;
    return 0;
}

static int hwa_e_identity_stream(FILE *stream, HWAExperimentIdentity *id)
{
    struct stat facts;
    int descriptor = fileno(stream);
    if (descriptor < 0 || fstat(descriptor, &facts) != 0 ||
        !S_ISREG(facts.st_mode) || facts.st_size < 0) return -1;
    id->device = (uint64_t)facts.st_dev;
    id->file = (uint64_t)facts.st_ino;
    id->size = (uint64_t)facts.st_size;
    return 0;
}
#endif

static int hwa_e_identity_equal(const HWAExperimentIdentity *a,
                                const HWAExperimentIdentity *b)
{
    return a->device == b->device && a->file == b->file && a->size == b->size;
}

static void hwa_e_digest_hex(const unsigned char digest[32],
                             char text[HWA_SHA256_HEX_SIZE])
{
    static const char digits[] = "0123456789abcdef";
    size_t index;
    for (index = 0U; index < 32U; ++index) {
        text[index * 2U] = digits[digest[index] >> 4U];
        text[index * 2U + 1U] = digits[digest[index] & 15U];
    }
    text[64] = '\0';
}

static int hwa_e_read_file(const char *path,
                           const HWAExperimentOptions *limits,
                           unsigned char **data,
                           size_t *size,
                           char sha[HWA_SHA256_HEX_SIZE],
                           char *error,
                           size_t error_size)
{
    HWAExperimentIdentity before;
    HWAExperimentIdentity opened;
    HWAExperimentIdentity after;
    HWASha256 hash;
    unsigned char digest[32];
    unsigned char *buffer = NULL;
    FILE *stream = NULL;
    size_t offset = 0U;
    size_t file_size;
    int status = -1;
    *data = NULL;
    *size = 0U;
    if (path == NULL || path[0] == '\0' || strcmp(path, "-") == 0 ||
        hwa_e_identity_path(path, &before) != 0) {
        hwa_e_error(error, error_size,
                    "experiment result must be a named regular file");
        return -1;
    }
    if (before.size > limits->max_bundle_bytes ||
        before.size > limits->max_output_file_bytes ||
        before.size > (uint64_t)(SIZE_MAX - 1U) ||
        before.size == UINT64_MAX ||
        before.size + 1U > limits->max_work_bytes / 3U) {
        hwa_e_error(error, error_size,
                    "experiment result exceeds the current byte or work limit");
        return -1;
    }
    file_size = (size_t)before.size;
    buffer = (unsigned char *)malloc(file_size + 1U);
    if (buffer == NULL) {
        hwa_e_error(error, error_size,
                    "cannot allocate the experiment result reader");
        return -1;
    }
    stream = fopen(path, "rb");
    if (stream == NULL || hwa_e_identity_stream(stream, &opened) != 0 ||
        !hwa_e_identity_equal(&before, &opened)) {
        hwa_e_error(error, error_size,
                    "experiment result changed before it was opened");
        goto cleanup;
    }
    hwa_sha256_init(&hash);
    while (offset < file_size) {
        size_t count = fread(buffer + offset, 1U, file_size - offset, stream);
        if (count == 0U) {
            hwa_e_error(error, error_size,
                        "cannot read the complete experiment result");
            goto cleanup;
        }
        hwa_sha256_update(&hash, buffer + offset, count);
        offset += count;
    }
    if (fgetc(stream) != EOF || ferror(stream)) {
        hwa_e_error(error, error_size,
                    "experiment result changed while it was read");
        goto cleanup;
    }
    if (fclose(stream) != 0) {
        stream = NULL;
        hwa_e_error(error, error_size, "cannot close the experiment result");
        goto cleanup;
    }
    stream = NULL;
    if (hwa_e_identity_path(path, &after) != 0 ||
        !hwa_e_identity_equal(&before, &after)) {
        hwa_e_error(error, error_size,
                    "experiment result changed while it was read");
        goto cleanup;
    }
    buffer[file_size] = 0U;
    hwa_sha256_final(&hash, digest);
    hwa_e_digest_hex(digest, sha);
    *data = buffer;
    *size = file_size;
    buffer = NULL;
    status = 0;
cleanup:
    if (stream != NULL) (void)fclose(stream);
    free(buffer);
    return status;
}

static void hwa_e_row_free(HWAECsvRow *row)
{
    size_t index;
    for (index = 0U; index < row->count; ++index) free(row->fields[index].text);
    memset(row, 0, sizeof(*row));
}

static int hwa_e_copy_field(HWAECsvField *field,
                            const unsigned char *raw,
                            size_t raw_size,
                            int quoted,
                            char *error,
                            size_t error_size)
{
    char *text;
    size_t source;
    size_t target = 0U;
    size_t decoded = 0U;
    if (raw_size > HWA_E_MAX_FIELD_BYTES + 2U) goto invalid;
    if (!quoted) {
        decoded = raw_size;
    } else {
        if (raw_size < 2U || raw[0] != '"' || raw[raw_size - 1U] != '"') {
            goto invalid;
        }
        for (source = 1U; source + 1U < raw_size; ++source) {
            if (raw[source] == '"') {
                if (source + 2U >= raw_size || raw[source + 1U] != '"') {
                    goto invalid;
                }
                source++;
            }
            decoded++;
        }
    }
    if (decoded > HWA_E_MAX_FIELD_BYTES) goto invalid;
    text = (char *)malloc(decoded + 1U);
    if (text == NULL) {
        hwa_e_error(error, error_size,
                    "cannot allocate an experiment result field");
        return -1;
    }
    if (!quoted) {
        memcpy(text, raw, decoded);
        target = decoded;
    } else {
        for (source = 1U; source + 1U < raw_size; ++source) {
            if (raw[source] == '"') source++;
            text[target++] = (char)raw[source];
        }
    }
    text[target] = '\0';
    if (memchr(text, '\0', decoded) != NULL) {
        free(text);
        goto invalid;
    }
    field->text = text;
    field->raw = raw;
    field->raw_size = raw_size;
    field->quoted = quoted;
    return 0;
invalid:
    hwa_e_error(error, error_size,
                "experiment result has an invalid or overlong CSV field");
    return -1;
}

static int hwa_e_csv_next(HWAECsvReader *reader,
                          HWAECsvRow *row,
                          char *error,
                          size_t error_size)
{
    memset(row, 0, sizeof(*row));
    if (reader->cursor == reader->size) return 0;
    for (;;) {
        size_t start;
        size_t raw_size;
        int quoted = 0;
        if (row->count == HWA_E_MAX_FIELDS) goto invalid;
        start = reader->cursor;
        if (reader->data[reader->cursor] == '"') {
            quoted = 1;
            reader->cursor++;
            for (;;) {
                if (reader->cursor >= reader->size) goto invalid;
                if (reader->data[reader->cursor] == '"') {
                    reader->cursor++;
                    if (reader->cursor < reader->size &&
                        reader->data[reader->cursor] == '"') {
                        reader->cursor++;
                        continue;
                    }
                    break;
                }
                reader->cursor++;
            }
        } else {
            while (reader->cursor < reader->size &&
                   reader->data[reader->cursor] != ',' &&
                   reader->data[reader->cursor] != '\r' &&
                   reader->data[reader->cursor] != '\n') {
                if (reader->data[reader->cursor] == '"') goto invalid;
                reader->cursor++;
            }
        }
        raw_size = reader->cursor - start;
        if (hwa_e_copy_field(&row->fields[row->count],
                             reader->data + start, raw_size, quoted,
                             error, error_size) != 0) goto failure;
        row->count++;
        if (reader->cursor >= reader->size) goto invalid;
        if (reader->data[reader->cursor] == ',') {
            reader->cursor++;
            continue;
        }
        if (reader->data[reader->cursor] != '\r' ||
            reader->cursor + 1U >= reader->size ||
            reader->data[reader->cursor + 1U] != '\n') goto invalid;
        reader->cursor += 2U;
        return 1;
    }
invalid:
    hwa_e_error(error, error_size,
                "experiment result must use canonical CRLF CSV");
failure:
    hwa_e_row_free(row);
    return -1;
}

static int hwa_e_field_canonical(const HWAECsvField *field)
{
    const unsigned char *text = (const unsigned char *)field->text;
    size_t size = strlen(field->text);
    size_t source;
    size_t raw = 0U;
    int quote = 0;
    for (source = 0U; source < size; ++source) {
        if (text[source] == ',' || text[source] == '"' ||
            text[source] == '\r' || text[source] == '\n') quote = 1;
    }
    if (quote != field->quoted) return 0;
    if (!quote) {
        return field->raw_size == size &&
               memcmp(field->raw, text, size) == 0;
    }
    if (field->raw_size < 2U || field->raw[raw++] != '"') return 0;
    for (source = 0U; source < size; ++source) {
        if (text[source] == '"' &&
            (raw >= field->raw_size || field->raw[raw++] != '"')) return 0;
        if (raw >= field->raw_size || field->raw[raw++] != text[source]) return 0;
    }
    return raw + 1U == field->raw_size && field->raw[raw] == '"';
}

static int hwa_e_row_canonical(const HWAECsvRow *row)
{
    size_t index;
    for (index = 0U; index < row->count; ++index) {
        if (!hwa_e_field_canonical(&row->fields[index])) return 0;
    }
    return 1;
}

static int hwa_e_u64(const HWAECsvField *field, uint64_t *value)
{
    unsigned long long parsed;
    char canonical[32];
    char *end = NULL;
    int length;
    if (field->quoted || field->text[0] == '\0' ||
        field->text[0] == '+' || field->text[0] == '-') return -1;
    errno = 0;
    parsed = strtoull(field->text, &end, 10);
    if (errno == ERANGE || end == field->text || *end != '\0') return -1;
    *value = (uint64_t)parsed;
    length = snprintf(canonical, sizeof(canonical), "%" PRIu64, *value);
    return length >= 0 && (size_t)length < sizeof(canonical) &&
           strcmp(canonical, field->text) == 0 ? 0 : -1;
}

static int hwa_e_size(const HWAECsvField *field, size_t *value)
{
    uint64_t parsed;
    if (hwa_e_u64(field, &parsed) != 0 || parsed > SIZE_MAX) return -1;
    *value = (size_t)parsed;
    return 0;
}

static int hwa_e_u32(const HWAECsvField *field, uint32_t *value)
{
    uint64_t parsed;
    if (hwa_e_u64(field, &parsed) != 0 || parsed > UINT32_MAX) return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static int hwa_e_bool(const HWAECsvField *field, int *value)
{
    if (!field->quoted && strcmp(field->text, "0") == 0) {
        *value = 0;
        return 0;
    }
    if (!field->quoted && strcmp(field->text, "1") == 0) {
        *value = 1;
        return 0;
    }
    return -1;
}

static int hwa_e_double(const HWANumericLocale *locale,
                        const HWAECsvField *field,
                        double *value)
{
    char canonical[64];
    if (field->quoted || field->text[0] == '\0' ||
        hwa_c_locale_parse_double(locale, field->text, value) != 0 ||
        !isfinite(*value) ||
        hwa_c_locale_format_double(locale, canonical, sizeof(canonical),
                                   *value == 0.0 ? 0.0 : *value) != 0 ||
        strcmp(canonical, field->text) != 0) return -1;
    if (*value == 0.0) *value = 0.0;
    return 0;
}

static int hwa_e_optional_double(const HWANumericLocale *locale,
                                 const HWAECsvField *field,
                                 int valid,
                                 double *value)
{
    if (!valid) {
        if (field->quoted || field->text[0] != '\0') return -1;
        *value = 0.0;
        return 0;
    }
    return hwa_e_double(locale, field, value);
}

static int hwa_e_sha(const HWAECsvField *field,
                     char target[HWA_SHA256_HEX_SIZE])
{
    size_t index;
    if (field->quoted || strlen(field->text) != 64U) return -1;
    for (index = 0U; index < 64U; ++index) {
        char byte = field->text[index];
        if (!((byte >= '0' && byte <= '9') ||
              (byte >= 'a' && byte <= 'f'))) return -1;
    }
    memcpy(target, field->text, HWA_SHA256_HEX_SIZE);
    return 0;
}

static char *hwa_e_take(HWAECsvRow *row, size_t index, int allow_empty)
{
    char *text;
    if (row->fields[index].text == NULL ||
        (!allow_empty && row->fields[index].text[0] == '\0')) return NULL;
    text = row->fields[index].text;
    row->fields[index].text = NULL;
    return text;
}

static int hwa_e_hex_digit(unsigned char byte)
{
    if (byte >= '0' && byte <= '9') return (int)(byte - '0');
    if (byte >= 'a' && byte <= 'f') return (int)(byte - 'a') + 10;
    return -1;
}

static char *hwa_e_path(const HWAECsvField *field, int allow_empty)
{
    size_t hex_size = strlen(field->text);
    size_t size;
    size_t index;
    char *path;
    if (field->quoted || (hex_size & 1U) != 0U ||
        (!allow_empty && hex_size == 0U)) return NULL;
    size = hex_size / 2U;
    path = (char *)malloc(size + 1U);
    if (path == NULL) return NULL;
    for (index = 0U; index < size; ++index) {
        int high = hwa_e_hex_digit((unsigned char)field->text[index * 2U]);
        int low = hwa_e_hex_digit((unsigned char)field->text[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            free(path);
            return NULL;
        }
        path[index] = (char)((unsigned)high * 16U + (unsigned)low);
        if (path[index] == '\0') {
            free(path);
            return NULL;
        }
    }
    path[size] = '\0';
    return path;
}

static int hwa_e_parse_plan(const HWAECsvField *field,
                            HWAExperimentPlanKind *value)
{
    int item;
    if (field->quoted) return -1;
    for (item = HWA_EXPERIMENT_ONE_AT_A_TIME;
         item < HWA_EXPERIMENT_PLAN_KIND_COUNT; ++item) {
        if (strcmp(field->text,
                   hwa_experiment_plan_name((HWAExperimentPlanKind)item)) == 0) {
            *value = (HWAExperimentPlanKind)item;
            return 0;
        }
    }
    return -1;
}

static int hwa_e_parse_split(const HWAECsvField *field,
                             HWAExperimentSplit *value)
{
    int item;
    if (field->quoted) return -1;
    for (item = HWA_EXPERIMENT_FIT;
         item < HWA_EXPERIMENT_SPLIT_COUNT; ++item) {
        if (strcmp(field->text,
                   hwa_experiment_split_name((HWAExperimentSplit)item)) == 0) {
            *value = (HWAExperimentSplit)item;
            return 0;
        }
    }
    return -1;
}

static int hwa_e_parse_monotonicity(
    const HWAECsvField *field,
    HWAExperimentMonotonicity *value)
{
    int item;
    if (field->quoted) return -1;
    for (item = HWA_EXPERIMENT_MONOTONIC_NONE;
         item < HWA_EXPERIMENT_MONOTONICITY_COUNT; ++item) {
        if (strcmp(field->text, hwa_experiment_monotonicity_name(
                                    (HWAExperimentMonotonicity)item)) == 0) {
            *value = (HWAExperimentMonotonicity)item;
            return 0;
        }
    }
    return -1;
}

static int hwa_e_parse_availability(const HWAECsvField *field,
                                    HWARunAvailability *value)
{
    int item;
    if (field->quoted) return -1;
    for (item = HWA_RUN_AVAILABLE; item < HWA_RUN_AVAILABILITY_COUNT; ++item) {
        if (strcmp(field->text,
                   hwa_run_availability_name((HWARunAvailability)item)) == 0) {
            *value = (HWARunAvailability)item;
            return 0;
        }
    }
    return -1;
}

static int hwa_e_parse_role(const HWAECsvField *field,
                            HWARunStemRole *value)
{
    int item;
    if (field->quoted) return -1;
    for (item = HWA_RUN_STEM_SOURCE; item < HWA_RUN_STEM_ROLE_COUNT; ++item) {
        if (strcmp(field->text,
                   hwa_run_stem_role_name((HWARunStemRole)item)) == 0) {
            *value = (HWARunStemRole)item;
            return 0;
        }
    }
    return -1;
}

static int hwa_e_parse_feature(const HWAECsvField *field,
                               HWARunFeatureKind *value)
{
    int item;
    if (field->quoted) return -1;
    for (item = HWA_RUN_FEATURE_RMS_DBFS;
         item < HWA_RUN_FEATURE_KIND_COUNT; ++item) {
        if (strcmp(field->text,
                   hwa_run_feature_kind_name((HWARunFeatureKind)item)) == 0) {
            *value = (HWARunFeatureKind)item;
            return 0;
        }
    }
    return -1;
}

static int hwa_e_parse_source_kind(const HWAECsvField *field,
                                   HWARunSourceKind *value)
{
    int item;
    if (field->quoted) return -1;
    for (item = HWA_RUN_SOURCE_STEM; item < HWA_RUN_SOURCE_KIND_COUNT; ++item) {
        if (strcmp(field->text,
                   hwa_run_source_kind_name((HWARunSourceKind)item)) == 0) {
            *value = (HWARunSourceKind)item;
            return 0;
        }
    }
    return -1;
}

static int hwa_e_options_nonzero(const HWAExperimentOptions *o)
{
    const HWARunOptions *r = &o->run;
    return r->decode_block_frames != 0U &&
           r->max_manifest_bytes != 0U && r->max_input_bytes != 0U &&
           r->max_input_frames != 0U && r->max_probe_bytes != 0U &&
           r->max_probe_values != 0U && r->max_work_bytes != 0U &&
           r->max_evaluations != 0U && r->max_stems != 0U &&
           r->max_probes != 0U && r->max_links != 0U &&
           r->max_json_depth != 0U && r->max_json_tokens != 0U &&
           r->max_result_rows != 0U && r->max_warnings != 0U &&
           o->max_manifest_bytes != 0U && o->max_input_bytes != 0U &&
           o->max_work_bytes != 0U && o->max_bundle_bytes != 0U &&
           o->max_output_file_bytes != 0U &&
           o->max_total_run_evaluations != 0U &&
           o->max_job_milliseconds != 0U &&
           o->max_total_milliseconds != 0U &&
           o->max_parameters != 0U && o->max_levels != 0U &&
           o->max_cases != 0U && o->max_responses != 0U &&
           o->max_points != 0U && o->max_jobs != 0U &&
           o->max_replicates != 0U && o->max_artifacts != 0U &&
           o->max_observations != 0U && o->max_sensitivities != 0U &&
           o->max_interactions != 0U && o->max_warnings != 0U;
}

static int hwa_e_meta_value(size_t index,
                            const HWAECsvField *field,
                            HWAESavedMeta *meta)
{
    HWAExperimentOptions *o = &meta->options;
    HWARunOptions *r = &o->run;
    uint64_t u64;
    switch (index) {
        case 0U:
            return field->text[0] != '\0' ? 0 : -1;
        case 1U:
            return !field->quoted &&
                   strcmp(field->text, HWA_EXPERIMENT_METHOD_VERSION) == 0 ?
                   0 : -1;
        case 2U:
            return !field->quoted && strcmp(field->text, "1") == 0 ? 0 : -1;
        case 3U:
            return hwa_e_parse_plan(field, &meta->plan_kind);
        case 4U: return hwa_e_u64(field, &meta->plan_seed);
        case 5U: return hwa_e_size(field, &meta->plan_replicates);
        case 6U: return hwa_e_sha(field, meta->manifest_sha256);
        case 7U:
            return -1;
        case 8U: return hwa_e_sha(field, meta->renderer_sha256);
        case 9U:
        case 10U:
        case 11U:
        case 12U:
        case 14U:
        case 15U:
            return field->text[0] != '\0' ? 0 : -1;
        case 13U:
            return hwa_e_u64(field, &u64) == 0 && u64 != 0U ? 0 : -1;
        case 16U: return hwa_e_size(field, &r->decode_block_frames);
        case 17U: return hwa_e_u64(field, &r->max_manifest_bytes);
        case 18U: return hwa_e_u64(field, &r->max_input_bytes);
        case 19U: return hwa_e_u64(field, &r->max_input_frames);
        case 20U: return hwa_e_u64(field, &r->max_probe_bytes);
        case 21U: return hwa_e_u64(field, &r->max_probe_values);
        case 22U: return hwa_e_u64(field, &r->max_work_bytes);
        case 23U: return hwa_e_u64(field, &r->max_evaluations);
        case 24U: return hwa_e_size(field, &r->max_stems);
        case 25U: return hwa_e_size(field, &r->max_probes);
        case 26U: return hwa_e_size(field, &r->max_links);
        case 27U: return hwa_e_size(field, &r->max_json_depth);
        case 28U: return hwa_e_size(field, &r->max_json_tokens);
        case 29U: return hwa_e_size(field, &r->max_result_rows);
        case 30U: return hwa_e_size(field, &r->max_warnings);
        case 31U: return hwa_e_u64(field, &o->max_manifest_bytes);
        case 32U: return hwa_e_u64(field, &o->max_input_bytes);
        case 33U: return hwa_e_u64(field, &o->max_work_bytes);
        case 34U: return hwa_e_u64(field, &o->max_bundle_bytes);
        case 35U: return hwa_e_u64(field, &o->max_output_file_bytes);
        case 36U: return hwa_e_u64(field, &o->max_total_run_evaluations);
        case 37U: return hwa_e_u64(field, &o->max_job_milliseconds);
        case 38U: return hwa_e_u64(field, &o->max_total_milliseconds);
        case 39U: return hwa_e_size(field, &o->max_parameters);
        case 40U: return hwa_e_size(field, &o->max_levels);
        case 41U: return hwa_e_size(field, &o->max_cases);
        case 42U: return hwa_e_size(field, &o->max_responses);
        case 43U: return hwa_e_size(field, &o->max_points);
        case 44U: return hwa_e_size(field, &o->max_jobs);
        case 45U: return hwa_e_size(field, &o->max_replicates);
        case 46U: return hwa_e_size(field, &o->max_artifacts);
        case 47U: return hwa_e_size(field, &o->max_observations);
        case 48U: return hwa_e_size(field, &o->max_sensitivities);
        case 49U: return hwa_e_size(field, &o->max_interactions);
        case 50U: return hwa_e_size(field, &o->max_warnings);
        case 51U: return hwa_e_u64(field, &meta->retained_work_bytes);
        case 52U: return hwa_e_u64(field, &meta->total_run_evaluations);
        case 53U: return hwa_e_u64(field, &meta->total_output_bytes);
        default:
            if (index >= 54U && index < 54U + HWA_E_ARRAY_COUNT) {
                return hwa_e_size(field, &meta->counts[index - 54U]);
            }
            return -1;
    }
}

static void hwa_e_meta_free(HWAESavedMeta *meta)
{
    free(meta->renderer_id);
    meta->renderer_id = NULL;
}

static int hwa_e_read_meta(HWAECsvReader *reader,
                           HWAESavedMeta *meta,
                           char *error,
                           size_t error_size)
{
    size_t index;
    memset(meta, 0, sizeof(*meta));
    for (index = 0U;
         index < sizeof(hwa_e_meta_keys) / sizeof(hwa_e_meta_keys[0]);
         ++index) {
        HWAECsvRow row;
        int status = hwa_e_csv_next(reader, &row, error, error_size);
        if (status != 1) goto invalid;
        if (row.count != 4U || !hwa_e_row_canonical(&row) ||
            strcmp(row.fields[0].text, "META") != 0 ||
            strcmp(row.fields[1].text, hwa_e_meta_keys[index].key) != 0 ||
            strcmp(row.fields[3].text, hwa_e_meta_keys[index].unit) != 0) {
            hwa_e_row_free(&row);
            goto invalid;
        }
        if (index == 7U) {
            meta->renderer_id = hwa_e_take(&row, 2U, 0);
            if (meta->renderer_id == NULL) {
                hwa_e_row_free(&row);
                goto invalid;
            }
        } else if (hwa_e_meta_value(index, &row.fields[2], meta) != 0) {
            hwa_e_row_free(&row);
            goto invalid;
        }
        hwa_e_row_free(&row);
    }
    if (!hwa_e_options_nonzero(&meta->options) ||
        meta->options.run.decode_block_frames > 1048576U ||
        meta->plan_replicates == 0U ||
        meta->retained_work_bytes == 0U ||
        meta->retained_work_bytes > meta->options.max_work_bytes ||
        meta->total_run_evaluations >
            meta->options.max_total_run_evaluations ||
        meta->total_output_bytes > meta->options.max_bundle_bytes) goto invalid;
    return 0;
invalid:
    hwa_e_meta_free(meta);
    hwa_e_error(error, error_size,
                "experiment result has invalid or out-of-order META");
    return -1;
}

static int hwa_e_row_begin(HWAECsvReader *reader,
                           HWAECsvRow *row,
                           const char *section,
                           size_t count,
                           char *error,
                           size_t error_size)
{
    int status = hwa_e_csv_next(reader, row, error, error_size);
    if (status != 1 || row->count != count || !hwa_e_row_canonical(row) ||
        strcmp(row->fields[0].text, section) != 0) {
        if (status == 1) hwa_e_row_free(row);
        hwa_e_error(error, error_size,
                    "experiment result has an invalid or out-of-order row");
        return -1;
    }
    return 0;
}

static int hwa_e_parse_input(HWAECsvReader *reader,
                             HWAExperimentInput *r,
                             char *error,
                             size_t error_size)
{
    HWAECsvRow row;
    if (hwa_e_row_begin(reader, &row, "INPUT", 5U, error, error_size) != 0) {
        return -1;
    }
    if (hwa_e_u64(&row.fields[1], &r->id) != 0 ||
        (r->binding_id = hwa_e_take(&row, 2U, 0)) == NULL ||
        hwa_e_sha(&row.fields[3], r->sha256) != 0 ||
        hwa_e_u64(&row.fields[4], &r->file_bytes) != 0 ||
        (r->path = (char *)malloc(2U)) == NULL) goto invalid;
    r->path[0] = '.';
    r->path[1] = '\0';
    hwa_e_row_free(&row);
    return 0;
invalid:
    hwa_e_row_free(&row);
    hwa_e_error(error, error_size, "invalid Stage 8 INPUT row");
    return -1;
}

static int hwa_e_parse_parameter(const HWANumericLocale *locale,
                                 HWAECsvReader *reader,
                                 HWAExperimentParameter *r,
                                 char *error,
                                 size_t error_size)
{
    HWAECsvRow row;
    if (hwa_e_row_begin(reader, &row, "PARAMETER", 9U,
                        error, error_size) != 0) return -1;
    if (hwa_e_u64(&row.fields[1], &r->id) != 0 ||
        (r->name = hwa_e_take(&row, 2U, 0)) == NULL ||
        (r->unit = hwa_e_take(&row, 3U, 0)) == NULL ||
        hwa_e_double(locale, &row.fields[4], &r->minimum) != 0 ||
        hwa_e_double(locale, &row.fields[5], &r->maximum) != 0 ||
        hwa_e_double(locale, &row.fields[6], &r->baseline) != 0 ||
        hwa_e_size(&row.fields[7], &r->first_level) != 0 ||
        hwa_e_size(&row.fields[8], &r->level_count) != 0) goto invalid;
    hwa_e_row_free(&row);
    return 0;
invalid:
    hwa_e_row_free(&row);
    hwa_e_error(error, error_size, "invalid Stage 8 PARAMETER row");
    return -1;
}

static int hwa_e_parse_level(const HWANumericLocale *locale,
                             HWAECsvReader *reader,
                             HWAExperimentLevel *r,
                             char *error,
                             size_t error_size)
{
    HWAECsvRow row;
    if (hwa_e_row_begin(reader, &row, "LEVEL", 4U,
                        error, error_size) != 0) return -1;
    if (hwa_e_u64(&row.fields[1], &r->id) != 0 ||
        hwa_e_u64(&row.fields[2], &r->parameter_id) != 0 ||
        hwa_e_double(locale, &row.fields[3], &r->value) != 0) goto invalid;
    hwa_e_row_free(&row);
    return 0;
invalid:
    hwa_e_row_free(&row);
    hwa_e_error(error, error_size, "invalid Stage 8 LEVEL row");
    return -1;
}

static int hwa_e_parse_case(const HWANumericLocale *locale,
                            HWAECsvReader *reader,
                            HWAExperimentCase *r,
                            char *error,
                            size_t error_size)
{
    HWAECsvRow row;
    if (hwa_e_row_begin(reader, &row, "CASE", 5U,
                        error, error_size) != 0) return -1;
    if (hwa_e_u64(&row.fields[1], &r->id) != 0 ||
        (r->name = hwa_e_take(&row, 2U, 0)) == NULL ||
        hwa_e_parse_split(&row.fields[3], &r->split) != 0 ||
        hwa_e_double(locale, &row.fields[4], &r->weight) != 0) goto invalid;
    hwa_e_row_free(&row);
    return 0;
invalid:
    hwa_e_row_free(&row);
    hwa_e_error(error, error_size, "invalid Stage 8 CASE row");
    return -1;
}

static int hwa_e_parse_response(HWAECsvReader *reader,
                                HWAExperimentResponse *r,
                                char *error,
                                size_t error_size)
{
    HWAECsvRow row;
    if (hwa_e_row_begin(reader, &row, "RESPONSE", 6U,
                        error, error_size) != 0) return -1;
    if (hwa_e_u64(&row.fields[1], &r->id) != 0 ||
        (r->name = hwa_e_take(&row, 2U, 0)) == NULL ||
        hwa_e_parse_role(&row.fields[3], &r->role) != 0 ||
        hwa_e_parse_feature(&row.fields[4], &r->feature) != 0 ||
        hwa_e_u32(&row.fields[5], &r->feature_index) != 0) goto invalid;
    hwa_e_row_free(&row);
    return 0;
invalid:
    hwa_e_row_free(&row);
    hwa_e_error(error, error_size, "invalid Stage 8 RESPONSE row");
    return -1;
}

static int hwa_e_parse_point(HWAECsvReader *reader,
                             HWAExperimentPoint *r,
                             char *error,
                             size_t error_size)
{
    HWAECsvRow row;
    if (hwa_e_row_begin(reader, &row, "POINT", 4U,
                        error, error_size) != 0) return -1;
    if (hwa_e_u64(&row.fields[1], &r->id) != 0 ||
        hwa_e_sha(&row.fields[2], r->key) != 0 ||
        hwa_e_bool(&row.fields[3], &r->baseline) != 0) goto invalid;
    hwa_e_row_free(&row);
    return 0;
invalid:
    hwa_e_row_free(&row);
    hwa_e_error(error, error_size, "invalid Stage 8 POINT row");
    return -1;
}

static int hwa_e_parse_value(const HWANumericLocale *locale,
                             HWAECsvReader *reader,
                             HWAExperimentValue *r,
                             char *error,
                             size_t error_size)
{
    HWAECsvRow row;
    if (hwa_e_row_begin(reader, &row, "VALUE", 5U,
                        error, error_size) != 0) return -1;
    if (hwa_e_u64(&row.fields[1], &r->id) != 0 ||
        hwa_e_u64(&row.fields[2], &r->point_id) != 0 ||
        hwa_e_u64(&row.fields[3], &r->parameter_id) != 0 ||
        hwa_e_double(locale, &row.fields[4], &r->value) != 0) goto invalid;
    hwa_e_row_free(&row);
    return 0;
invalid:
    hwa_e_row_free(&row);
    hwa_e_error(error, error_size, "invalid Stage 8 VALUE row");
    return -1;
}

static int hwa_e_parse_job(HWAECsvReader *reader,
                           HWAExperimentJob *r,
                           char *error,
                           size_t error_size)
{
    HWAECsvRow row;
    if (hwa_e_row_begin(reader, &row, "JOB", 12U,
                        error, error_size) != 0) return -1;
    if (hwa_e_u64(&row.fields[1], &r->id) != 0 ||
        hwa_e_sha(&row.fields[2], r->key) != 0 ||
        hwa_e_u64(&row.fields[3], &r->point_id) != 0 ||
        hwa_e_u64(&row.fields[4], &r->case_id) != 0 ||
        hwa_e_size(&row.fields[5], &r->replicate) != 0 ||
        hwa_e_u64(&row.fields[6], &r->seed) != 0 ||
        (r->run_result_path = hwa_e_path(&row.fields[7], 0)) == NULL ||
        hwa_e_sha(&row.fields[8], r->run_manifest_sha256) != 0 ||
        hwa_e_sha(&row.fields[9], r->run_result_sha256) != 0 ||
        hwa_e_u64(&row.fields[10], &r->output_bytes) != 0 ||
        hwa_e_u64(&row.fields[11], &r->run_evaluations) != 0) goto invalid;
    hwa_e_row_free(&row);
    return 0;
invalid:
    hwa_e_row_free(&row);
    hwa_e_error(error, error_size, "invalid Stage 8 JOB row");
    return -1;
}

static int hwa_e_parse_artifact(HWAECsvReader *reader,
                                HWAExperimentArtifact *r,
                                char *error,
                                size_t error_size)
{
    HWAECsvRow row;
    if (hwa_e_row_begin(reader, &row, "ARTIFACT", 8U,
                        error, error_size) != 0) return -1;
    if (hwa_e_u64(&row.fields[1], &r->id) != 0 ||
        hwa_e_u64(&row.fields[2], &r->job_id) != 0 ||
        (r->resource_id = hwa_e_take(&row, 3U, 0)) == NULL ||
        (r->path = hwa_e_path(&row.fields[4], 0)) == NULL ||
        hwa_e_sha(&row.fields[5], r->sha256) != 0 ||
        hwa_e_u64(&row.fields[6], &r->file_bytes) != 0 ||
        hwa_e_parse_source_kind(&row.fields[7], &r->kind) != 0) goto invalid;
    hwa_e_row_free(&row);
    return 0;
invalid:
    hwa_e_row_free(&row);
    hwa_e_error(error, error_size, "invalid Stage 8 ARTIFACT row");
    return -1;
}

static int hwa_e_parse_observation(const HWANumericLocale *locale,
                                   HWAECsvReader *reader,
                                   HWAExperimentObservation *r,
                                   char *error,
                                   size_t error_size)
{
    HWAECsvRow row;
    if (hwa_e_row_begin(reader, &row, "OBSERVATION", 8U,
                        error, error_size) != 0) return -1;
    if (hwa_e_bool(&row.fields[7], &r->value_valid) != 0 ||
        hwa_e_u64(&row.fields[1], &r->id) != 0 ||
        hwa_e_u64(&row.fields[2], &r->job_id) != 0 ||
        hwa_e_u64(&row.fields[3], &r->response_id) != 0 ||
        hwa_e_parse_availability(&row.fields[4], &r->availability) != 0 ||
        hwa_e_optional_double(locale, &row.fields[5],
                              r->value_valid, &r->value) != 0 ||
        hwa_e_u32(&row.fields[6], &r->quality_flags) != 0) goto invalid;
    hwa_e_row_free(&row);
    return 0;
invalid:
    hwa_e_row_free(&row);
    hwa_e_error(error, error_size, "invalid Stage 8 OBSERVATION row");
    return -1;
}

static int hwa_e_parse_candidate(const HWANumericLocale *locale,
                                 HWAECsvReader *reader,
                                 HWAExperimentCandidate *r,
                                 char *error,
                                 size_t error_size)
{
    HWAECsvRow row;
    if (hwa_e_row_begin(reader, &row, "CANDIDATE", 12U,
                        error, error_size) != 0) return -1;
    if (hwa_e_bool(&row.fields[11], &r->values_valid) != 0 ||
        hwa_e_u64(&row.fields[1], &r->id) != 0 ||
        hwa_e_u64(&row.fields[2], &r->point_id) != 0 ||
        hwa_e_u64(&row.fields[3], &r->response_id) != 0 ||
        hwa_e_parse_split(&row.fields[4], &r->split) != 0 ||
        hwa_e_parse_availability(&row.fields[5], &r->availability) != 0 ||
        hwa_e_optional_double(locale, &row.fields[6],
                              r->values_valid, &r->mean_gap) != 0 ||
        hwa_e_optional_double(locale, &row.fields[7],
                              r->values_valid, &r->improvement) != 0 ||
        hwa_e_optional_double(locale, &row.fields[8],
                              r->values_valid, &r->worst_harm) != 0 ||
        hwa_e_size(&row.fields[9], &r->case_count) != 0 ||
        hwa_e_u32(&row.fields[10], &r->quality_flags) != 0) goto invalid;
    hwa_e_row_free(&row);
    return 0;
invalid:
    hwa_e_row_free(&row);
    hwa_e_error(error, error_size, "invalid Stage 8 CANDIDATE row");
    return -1;
}

static int hwa_e_parse_sensitivity(const HWANumericLocale *locale,
                                   HWAECsvReader *reader,
                                   HWAExperimentSensitivity *r,
                                   char *error,
                                   size_t error_size)
{
    HWAECsvRow row;
    if (hwa_e_row_begin(reader, &row, "SENSITIVITY", 18U,
                        error, error_size) != 0) return -1;
    if (hwa_e_bool(&row.fields[15], &r->linear_valid) != 0 ||
        hwa_e_bool(&row.fields[16], &r->effect_valid) != 0 ||
        hwa_e_bool(&row.fields[17], &r->noise_valid) != 0 ||
        hwa_e_u64(&row.fields[1], &r->id) != 0 ||
        hwa_e_u64(&row.fields[2], &r->parameter_id) != 0 ||
        hwa_e_u64(&row.fields[3], &r->response_id) != 0 ||
        hwa_e_parse_split(&row.fields[4], &r->split) != 0 ||
        hwa_e_parse_availability(&row.fields[5], &r->availability) != 0 ||
        hwa_e_optional_double(locale, &row.fields[6],
                              r->linear_valid, &r->slope) != 0 ||
        hwa_e_optional_double(locale, &row.fields[7],
                              r->linear_valid, &r->pearson) != 0 ||
        hwa_e_optional_double(locale, &row.fields[8],
                              r->linear_valid, &r->linear_r_squared) != 0 ||
        hwa_e_optional_double(locale, &row.fields[9],
                              r->effect_valid, &r->effect_fraction) != 0 ||
        hwa_e_double(locale, &row.fields[10], &r->response_range) != 0 ||
        hwa_e_optional_double(locale, &row.fields[11],
                              r->noise_valid, &r->noise_sd) != 0 ||
        hwa_e_size(&row.fields[12], &r->point_count) != 0 ||
        hwa_e_parse_monotonicity(&row.fields[13],
                                 &r->monotonicity) != 0 ||
        hwa_e_u32(&row.fields[14], &r->quality_flags) != 0) goto invalid;
    hwa_e_row_free(&row);
    return 0;
invalid:
    hwa_e_row_free(&row);
    hwa_e_error(error, error_size, "invalid Stage 8 SENSITIVITY row");
    return -1;
}

static int hwa_e_parse_interaction(const HWANumericLocale *locale,
                                   HWAECsvReader *reader,
                                   HWAExperimentInteraction *r,
                                   char *error,
                                   size_t error_size)
{
    HWAECsvRow row;
    if (hwa_e_row_begin(reader, &row, "INTERACTION", 10U,
                        error, error_size) != 0) return -1;
    if (hwa_e_bool(&row.fields[9], &r->effect_valid) != 0 ||
        hwa_e_u64(&row.fields[1], &r->id) != 0 ||
        hwa_e_u64(&row.fields[2], &r->left_parameter_id) != 0 ||
        hwa_e_u64(&row.fields[3], &r->right_parameter_id) != 0 ||
        hwa_e_u64(&row.fields[4], &r->response_id) != 0 ||
        hwa_e_parse_split(&row.fields[5], &r->split) != 0 ||
        hwa_e_parse_availability(&row.fields[6], &r->availability) != 0 ||
        hwa_e_optional_double(locale, &row.fields[7],
                              r->effect_valid, &r->effect_fraction) != 0 ||
        hwa_e_size(&row.fields[8], &r->point_count) != 0) goto invalid;
    hwa_e_row_free(&row);
    return 0;
invalid:
    hwa_e_row_free(&row);
    hwa_e_error(error, error_size, "invalid Stage 8 INTERACTION row");
    return -1;
}

static int hwa_e_blank(const HWAECsvField *field)
{
    return !field->quoted && field->text[0] == '\0';
}

static int hwa_e_parse_warning(HWAECsvReader *reader,
                               HWAExperimentWarning *r,
                               char *error,
                               size_t error_size)
{
    HWAECsvRow row;
    if (hwa_e_row_begin(reader, &row, "WARNING", 12U,
                        error, error_size) != 0) return -1;
    if (hwa_e_bool(&row.fields[8], &r->job_id_valid) != 0 ||
        hwa_e_bool(&row.fields[9], &r->point_id_valid) != 0 ||
        hwa_e_bool(&row.fields[10], &r->parameter_id_valid) != 0 ||
        hwa_e_bool(&row.fields[11], &r->response_id_valid) != 0 ||
        hwa_e_u64(&row.fields[1], &r->id) != 0 ||
        (r->code = hwa_e_take(&row, 2U, 0)) == NULL ||
        (r->message = hwa_e_take(&row, 3U, 0)) == NULL ||
        (r->job_id_valid ?
             hwa_e_u64(&row.fields[4], &r->job_id) != 0 :
             !hwa_e_blank(&row.fields[4])) ||
        (r->point_id_valid ?
             hwa_e_u64(&row.fields[5], &r->point_id) != 0 :
             !hwa_e_blank(&row.fields[5])) ||
        (r->parameter_id_valid ?
             hwa_e_u64(&row.fields[6], &r->parameter_id) != 0 :
             !hwa_e_blank(&row.fields[6])) ||
        (r->response_id_valid ?
             hwa_e_u64(&row.fields[7], &r->response_id) != 0 :
             !hwa_e_blank(&row.fields[7]))) goto invalid;
    hwa_e_row_free(&row);
    return 0;
invalid:
    hwa_e_row_free(&row);
    hwa_e_error(error, error_size, "invalid Stage 8 WARNING row");
    return -1;
}

static int hwa_e_mul_size(size_t left, size_t right, size_t *product)
{
    if (left != 0U && right > SIZE_MAX / left) return -1;
    *product = left * right;
    return 0;
}

static int hwa_e_counts_fit(const HWAESavedMeta *meta,
                            const HWAExperimentOptions *limits)
{
    size_t expected;
    size_t candidate_cap;
    size_t rows = 0U;
    size_t index;
    if (meta->counts[HWA_E_INPUTS] > limits->max_artifacts ||
        meta->counts[HWA_E_PARAMETERS] > limits->max_parameters ||
        meta->counts[HWA_E_LEVELS] > limits->max_levels ||
        meta->counts[HWA_E_CASES] > limits->max_cases ||
        meta->counts[HWA_E_RESPONSES] > limits->max_responses ||
        meta->counts[HWA_E_POINTS] > limits->max_points ||
        meta->counts[HWA_E_JOBS] > limits->max_jobs ||
        meta->counts[HWA_E_ARTIFACTS] > limits->max_artifacts ||
        meta->counts[HWA_E_OBSERVATIONS] > limits->max_observations ||
        meta->counts[HWA_E_SENSITIVITIES] > limits->max_sensitivities ||
        meta->counts[HWA_E_INTERACTIONS] > limits->max_interactions ||
        meta->counts[HWA_E_WARNINGS] > limits->max_warnings ||
        meta->plan_replicates > limits->max_replicates ||
        meta->total_run_evaluations > limits->max_total_run_evaluations ||
        meta->total_output_bytes > limits->max_bundle_bytes) {
        return 0;
    }
    if (hwa_e_mul_size(meta->counts[HWA_E_POINTS],
                       meta->counts[HWA_E_PARAMETERS], &expected) != 0 ||
        meta->counts[HWA_E_VALUES] != expected ||
        hwa_e_mul_size(meta->counts[HWA_E_POINTS],
                       meta->counts[HWA_E_CASES], &expected) != 0 ||
        hwa_e_mul_size(expected, meta->plan_replicates, &expected) != 0 ||
        meta->counts[HWA_E_JOBS] != expected ||
        hwa_e_mul_size(meta->counts[HWA_E_JOBS],
                       meta->counts[HWA_E_RESPONSES], &expected) != 0 ||
        meta->counts[HWA_E_OBSERVATIONS] != expected ||
        hwa_e_mul_size(meta->counts[HWA_E_POINTS],
                       meta->counts[HWA_E_RESPONSES], &candidate_cap) != 0 ||
        hwa_e_mul_size(candidate_cap, 2U, &candidate_cap) != 0 ||
        meta->counts[HWA_E_CANDIDATES] > candidate_cap) return 0;
    for (index = 0U; index < HWA_E_ARRAY_COUNT; ++index) {
        if (meta->counts[index] > SIZE_MAX - rows) return 0;
        rows += meta->counts[index];
    }
    (void)rows;
    return 1;
}

static int hwa_e_array_bytes(const HWAESavedMeta *meta, uint64_t *bytes)
{
    uint64_t total = 0U;
#define EARRAY(which, type)                                                 \
    do {                                                                   \
        size_t count = meta->counts[which];                                \
        uint64_t part;                                                     \
        if (count > SIZE_MAX / sizeof(type)) return -1;                    \
        part = (uint64_t)(count * sizeof(type));                           \
        if (part > UINT64_MAX - total) return -1;                          \
        total += part;                                                     \
    } while (0)
    EARRAY(HWA_E_INPUTS, HWAExperimentInput);
    EARRAY(HWA_E_PARAMETERS, HWAExperimentParameter);
    EARRAY(HWA_E_LEVELS, HWAExperimentLevel);
    EARRAY(HWA_E_CASES, HWAExperimentCase);
    EARRAY(HWA_E_RESPONSES, HWAExperimentResponse);
    EARRAY(HWA_E_POINTS, HWAExperimentPoint);
    EARRAY(HWA_E_VALUES, HWAExperimentValue);
    EARRAY(HWA_E_JOBS, HWAExperimentJob);
    EARRAY(HWA_E_ARTIFACTS, HWAExperimentArtifact);
    EARRAY(HWA_E_OBSERVATIONS, HWAExperimentObservation);
    EARRAY(HWA_E_CANDIDATES, HWAExperimentCandidate);
    EARRAY(HWA_E_SENSITIVITIES, HWAExperimentSensitivity);
    EARRAY(HWA_E_INTERACTIONS, HWAExperimentInteraction);
    EARRAY(HWA_E_WARNINGS, HWAExperimentWarning);
#undef EARRAY
    *bytes = total;
    return 0;
}

static int hwa_e_allocate(HWAExperimentResult *result,
                          const HWAESavedMeta *meta,
                          char *error,
                          size_t error_size)
{
#define EALLOC(field, count_field, which, type)                             \
    do {                                                                   \
        result->count_field = meta->counts[which];                         \
        result->field = result->count_field == 0U ? NULL :                 \
            (type *)calloc(result->count_field, sizeof(type));             \
        if (result->count_field != 0U && result->field == NULL) goto fail;  \
    } while (0)
    EALLOC(inputs, input_count, HWA_E_INPUTS, HWAExperimentInput);
    EALLOC(parameters, parameter_count, HWA_E_PARAMETERS,
           HWAExperimentParameter);
    EALLOC(levels, level_count, HWA_E_LEVELS, HWAExperimentLevel);
    EALLOC(cases, case_count, HWA_E_CASES, HWAExperimentCase);
    EALLOC(responses, response_count, HWA_E_RESPONSES,
           HWAExperimentResponse);
    EALLOC(points, point_count, HWA_E_POINTS, HWAExperimentPoint);
    EALLOC(values, value_count, HWA_E_VALUES, HWAExperimentValue);
    EALLOC(jobs, job_count, HWA_E_JOBS, HWAExperimentJob);
    EALLOC(artifacts, artifact_count, HWA_E_ARTIFACTS,
           HWAExperimentArtifact);
    EALLOC(observations, observation_count, HWA_E_OBSERVATIONS,
           HWAExperimentObservation);
    EALLOC(candidates, candidate_count, HWA_E_CANDIDATES,
           HWAExperimentCandidate);
    EALLOC(sensitivities, sensitivity_count, HWA_E_SENSITIVITIES,
           HWAExperimentSensitivity);
    EALLOC(interactions, interaction_count, HWA_E_INTERACTIONS,
           HWAExperimentInteraction);
    EALLOC(warnings, warning_count, HWA_E_WARNINGS, HWAExperimentWarning);
#undef EALLOC
    return 0;
fail:
    hwa_e_error(error, error_size,
                "cannot allocate experiment result rows");
    return -1;
}

static int hwa_e_candidate_matches(const HWAExperimentCandidate *saved,
                                   const HWAExperimentCandidate *expected)
{
    return saved->id == expected->id &&
           saved->point_id == expected->point_id &&
           saved->response_id == expected->response_id &&
           saved->split == expected->split &&
           saved->availability == expected->availability &&
           saved->case_count == expected->case_count &&
           saved->quality_flags == expected->quality_flags &&
           saved->values_valid == expected->values_valid &&
           (!saved->values_valid ||
            (hwa_experiment_derived_double_matches(
                 saved->mean_gap, expected->mean_gap) &&
             hwa_experiment_derived_double_matches(
                 saved->improvement, expected->improvement) &&
             hwa_experiment_derived_double_matches(
                 saved->worst_harm, expected->worst_harm)));
}

static int hwa_e_sensitivity_matches(
    const HWAExperimentSensitivity *saved,
    const HWAExperimentSensitivity *expected)
{
    return saved->id == expected->id &&
           saved->parameter_id == expected->parameter_id &&
           saved->response_id == expected->response_id &&
           saved->split == expected->split &&
           saved->availability == expected->availability &&
           saved->point_count == expected->point_count &&
           saved->monotonicity == expected->monotonicity &&
           saved->quality_flags == expected->quality_flags &&
           saved->linear_valid == expected->linear_valid &&
           saved->effect_valid == expected->effect_valid &&
           saved->noise_valid == expected->noise_valid &&
           (!saved->linear_valid ||
            (hwa_experiment_derived_double_matches(
                 saved->slope, expected->slope) &&
             hwa_experiment_derived_double_matches(
                 saved->pearson, expected->pearson) &&
             hwa_experiment_derived_double_matches(
                 saved->linear_r_squared, expected->linear_r_squared))) &&
           (!saved->effect_valid ||
            hwa_experiment_derived_double_matches(
                saved->effect_fraction, expected->effect_fraction)) &&
           hwa_experiment_derived_double_matches(
               saved->response_range, expected->response_range) &&
           (!saved->noise_valid ||
            hwa_experiment_derived_double_matches(
                saved->noise_sd, expected->noise_sd));
}

static int hwa_e_interaction_matches(
    const HWAExperimentInteraction *saved,
    const HWAExperimentInteraction *expected)
{
    return saved->id == expected->id &&
           saved->left_parameter_id == expected->left_parameter_id &&
           saved->right_parameter_id == expected->right_parameter_id &&
           saved->response_id == expected->response_id &&
           saved->split == expected->split &&
           saved->availability == expected->availability &&
           saved->point_count == expected->point_count &&
           saved->effect_valid == expected->effect_valid &&
           (!saved->effect_valid ||
            hwa_experiment_derived_double_matches(
                saved->effect_fraction, expected->effect_fraction));
}

static int hwa_e_warning_matches(const HWAExperimentWarning *saved,
                                 const HWAExperimentWarning *expected)
{
    return saved->code != NULL && expected->code != NULL &&
           saved->message != NULL && expected->message != NULL &&
           saved->id == expected->id &&
           strcmp(saved->code, expected->code) == 0 &&
           strcmp(saved->message, expected->message) == 0 &&
           saved->job_id == expected->job_id &&
           saved->point_id == expected->point_id &&
           saved->parameter_id == expected->parameter_id &&
           saved->response_id == expected->response_id &&
           saved->job_id_valid == expected->job_id_valid &&
           saved->point_id_valid == expected->point_id_valid &&
           saved->parameter_id_valid == expected->parameter_id_valid &&
           saved->response_id_valid == expected->response_id_valid;
}

static void hwa_e_saved_warnings_free(HWAExperimentWarning *rows,
                                      size_t count)
{
    size_t index;
    for (index = 0U; index < count; ++index) {
        free(rows[index].code);
        free(rows[index].message);
    }
    free(rows);
}

static int hwa_e_normalize_derived(HWAExperimentResult *result,
                                   char *error,
                                   size_t error_size)
{
    HWAExperimentCandidate *candidates = result->candidates;
    HWAExperimentSensitivity *sensitivities = result->sensitivities;
    HWAExperimentInteraction *interactions = result->interactions;
    HWAExperimentWarning *warnings = result->warnings;
    size_t candidate_count = result->candidate_count;
    size_t sensitivity_count = result->sensitivity_count;
    size_t interaction_count = result->interaction_count;
    size_t warning_count = result->warning_count;
    size_t index;
    int status = -1;
    result->candidates = NULL;
    result->candidate_count = 0U;
    result->sensitivities = NULL;
    result->sensitivity_count = 0U;
    result->interactions = NULL;
    result->interaction_count = 0U;
    result->warnings = NULL;
    result->warning_count = 0U;
    if (hwa_experiment_derived_rebuild(result, error, error_size) != 0 ||
        result->candidate_count != candidate_count ||
        result->sensitivity_count != sensitivity_count ||
        result->interaction_count != interaction_count ||
        result->warning_count != warning_count) goto invalid;
    for (index = 0U; index < candidate_count; ++index) {
        if (!hwa_e_candidate_matches(&candidates[index],
                                     &result->candidates[index])) goto invalid;
    }
    for (index = 0U; index < sensitivity_count; ++index) {
        if (!hwa_e_sensitivity_matches(&sensitivities[index],
                                       &result->sensitivities[index])) {
            goto invalid;
        }
    }
    for (index = 0U; index < interaction_count; ++index) {
        if (!hwa_e_interaction_matches(&interactions[index],
                                       &result->interactions[index])) {
            goto invalid;
        }
    }
    for (index = 0U; index < warning_count; ++index) {
        if (!hwa_e_warning_matches(&warnings[index],
                                   &result->warnings[index])) goto invalid;
    }
    status = 0;
invalid:
    free(candidates);
    free(sensitivities);
    free(interactions);
    hwa_e_saved_warnings_free(warnings, warning_count);
    if (status != 0 && (error == NULL || error_size == 0U ||
                        error[0] == '\0')) {
        hwa_e_error(error, error_size,
                    "experiment result has invalid derived rows");
    }
    return status;
}

static int hwa_e_read_impl(
    const char *path,
    const HWAExperimentOptions *limits,
    HWAExperimentResult *result,
    char file_sha256[HWA_SHA256_HEX_SIZE],
    const HWANumericLocale *locale,
    char *error,
    size_t error_size)
{
    HWAESavedMeta meta;
    HWAECsvReader reader;
    HWAECsvRow row;
    unsigned char *data = NULL;
    size_t data_size = 0U;
    uint64_t arrays;
    uint64_t retained;
    uint64_t expected;
    uint64_t peak;
    size_t index;
    int status = -1;
    memset(&meta, 0, sizeof(meta));
    if (result == NULL) {
        hwa_e_error(error, error_size,
                    "experiment result pointer is null");
        return -1;
    }
    memset(result, 0, sizeof(*result));
    if (file_sha256 != NULL) file_sha256[0] = '\0';
    if (limits == NULL || file_sha256 == NULL ||
        !hwa_e_options_nonzero(limits) ||
        limits->run.decode_block_frames > 1048576U) {
        hwa_e_error(error, error_size,
                    "invalid experiment result reader arguments");
        return -1;
    }
    if (hwa_e_read_file(path, limits, &data, &data_size, file_sha256,
                        error, error_size) != 0) return -1;
    memset(&reader, 0, sizeof(reader));
    reader.data = data;
    reader.size = data_size;
    if (hwa_e_csv_next(&reader, &row, error, error_size) != 1 ||
        row.count != 2U || !hwa_e_row_canonical(&row) ||
        strcmp(row.fields[0].text, "HWA_EXPERIMENT") != 0 ||
        strcmp(row.fields[1].text, "1") != 0) {
        hwa_e_row_free(&row);
        hwa_e_error(error, error_size,
                    "experiment result has an invalid header");
        goto cleanup;
    }
    hwa_e_row_free(&row);
    if (hwa_e_read_meta(&reader, &meta, error, error_size) != 0 ||
        (uint64_t)data_size > meta.options.max_bundle_bytes ||
        (uint64_t)data_size > meta.options.max_output_file_bytes ||
        meta.total_output_bytes >
            meta.options.max_bundle_bytes - (uint64_t)data_size ||
        meta.total_output_bytes >
            limits->max_bundle_bytes - (uint64_t)data_size ||
        !hwa_e_counts_fit(&meta, limits) ||
        !hwa_e_counts_fit(&meta, &meta.options) ||
        hwa_e_array_bytes(&meta, &arrays) != 0 ||
        arrays > limits->max_work_bytes ||
        (uint64_t)data_size + 1U > limits->max_work_bytes / 3U ||
        arrays > limits->max_work_bytes -
                     ((uint64_t)data_size + 1U) * 3U) {
        if (error == NULL || error_size == 0U || error[0] == '\0') {
            hwa_e_error(error, error_size,
                        "experiment result exceeds current caps or has bad counts");
        }
        goto cleanup;
    }
    result->options = *limits;
    result->plan_kind = meta.plan_kind;
    result->plan_seed = meta.plan_seed;
    result->plan_replicates = meta.plan_replicates;
    result->manifest_path = (char *)malloc(2U);
    if (result->manifest_path != NULL) {
        result->manifest_path[0] = '.';
        result->manifest_path[1] = '\0';
    }
    memcpy(result->manifest_sha256, meta.manifest_sha256,
           HWA_SHA256_HEX_SIZE);
    result->output_directory = (char *)malloc(2U);
    if (result->output_directory != NULL) {
        result->output_directory[0] = '.';
        result->output_directory[1] = '\0';
    }
    result->renderer_id = meta.renderer_id;
    meta.renderer_id = NULL;
    memcpy(result->renderer_sha256, meta.renderer_sha256,
           HWA_SHA256_HEX_SIZE);
    result->total_run_evaluations = meta.total_run_evaluations;
    result->total_output_bytes = meta.total_output_bytes;
    result->total_duration_milliseconds = 0U;
    result->rendered_job_count = meta.counts[HWA_E_JOBS];
    result->reused_job_count = 0U;
    if (result->manifest_path == NULL || result->output_directory == NULL) {
        hwa_e_error(error, error_size,
                    "cannot allocate canonical experiment paths");
        goto cleanup;
    }
    if (hwa_e_allocate(result, &meta, error, error_size) != 0) goto cleanup;
    for (index = 0U; index < result->input_count; ++index) {
        if (hwa_e_parse_input(&reader, &result->inputs[index],
                              error, error_size) != 0) goto cleanup;
    }
    for (index = 0U; index < result->parameter_count; ++index) {
        if (hwa_e_parse_parameter(locale, &reader, &result->parameters[index],
                                  error, error_size) != 0) goto cleanup;
    }
    for (index = 0U; index < result->level_count; ++index) {
        if (hwa_e_parse_level(locale, &reader, &result->levels[index],
                              error, error_size) != 0) goto cleanup;
    }
    for (index = 0U; index < result->case_count; ++index) {
        if (hwa_e_parse_case(locale, &reader, &result->cases[index],
                             error, error_size) != 0) goto cleanup;
    }
    for (index = 0U; index < result->response_count; ++index) {
        if (hwa_e_parse_response(&reader, &result->responses[index],
                                 error, error_size) != 0) goto cleanup;
    }
    for (index = 0U; index < result->point_count; ++index) {
        if (hwa_e_parse_point(&reader, &result->points[index],
                              error, error_size) != 0) goto cleanup;
    }
    for (index = 0U; index < result->value_count; ++index) {
        if (hwa_e_parse_value(locale, &reader, &result->values[index],
                              error, error_size) != 0) goto cleanup;
    }
    for (index = 0U; index < result->job_count; ++index) {
        if (hwa_e_parse_job(&reader, &result->jobs[index],
                            error, error_size) != 0) goto cleanup;
    }
    for (index = 0U; index < result->artifact_count; ++index) {
        if (hwa_e_parse_artifact(&reader, &result->artifacts[index],
                                 error, error_size) != 0) goto cleanup;
    }
    for (index = 0U; index < result->observation_count; ++index) {
        if (hwa_e_parse_observation(locale, &reader,
                                    &result->observations[index],
                                    error, error_size) != 0) goto cleanup;
    }
    for (index = 0U; index < result->candidate_count; ++index) {
        if (hwa_e_parse_candidate(locale, &reader,
                                  &result->candidates[index],
                                  error, error_size) != 0) goto cleanup;
    }
    for (index = 0U; index < result->sensitivity_count; ++index) {
        if (hwa_e_parse_sensitivity(locale, &reader,
                                    &result->sensitivities[index],
                                    error, error_size) != 0) goto cleanup;
    }
    for (index = 0U; index < result->interaction_count; ++index) {
        if (hwa_e_parse_interaction(locale, &reader,
                                    &result->interactions[index],
                                    error, error_size) != 0) goto cleanup;
    }
    for (index = 0U; index < result->warning_count; ++index) {
        if (hwa_e_parse_warning(&reader, &result->warnings[index],
                                error, error_size) != 0) goto cleanup;
    }
    if (reader.cursor != reader.size ||
        hwa_e_raw_rows_valid(result, &meta.options,
                             error, error_size) != 0 ||
        hwa_e_raw_rows_valid(result, limits, error, error_size) != 0 ||
        hwa_experiment_total_run_evaluations_expected(result, &expected) != 0 ||
        expected != result->total_run_evaluations ||
        hwa_experiment_total_output_bytes_expected(result, &expected) != 0 ||
        expected != result->total_output_bytes ||
        hwa_experiment_result_peak_work_bytes(
            result, (uint64_t)data_size + UINT64_C(1), &peak) != 0 ||
        peak > limits->max_work_bytes ||
        hwa_e_normalize_derived(result, error, error_size) != 0 ||
        hwa_experiment_result_retained_bytes(result, &retained) != 0 ||
        retained == 0U || retained > limits->max_work_bytes ||
        (uint64_t)data_size + 1U > limits->max_work_bytes - retained) {
        if (error == NULL || error_size == 0U || error[0] == '\0') {
            hwa_e_error(error, error_size,
                        "invalid or over-limit experiment result");
        }
        goto cleanup;
    }
    result->retained_work_bytes = retained;
    if (hwa_experiment_result_validate_locale(
            result, locale, error, error_size) != 0) {
        goto cleanup;
    }
    status = 0;
cleanup:
    hwa_e_meta_free(&meta);
    free(data);
    if (status != 0) {
        hwa_experiment_result_free(result);
        if (file_sha256 != NULL) file_sha256[0] = '\0';
    }
    return status;
}

int hwa_experiment_file_read_locale(
    const char *path,
    const HWAExperimentOptions *limits,
    HWAExperimentResult *result,
    char file_sha256[HWA_SHA256_HEX_SIZE],
    const HWANumericLocale *locale,
    char *error,
    size_t error_size)
{
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (locale == NULL || !locale->active) {
        if (result != NULL) memset(result, 0, sizeof(*result));
        if (file_sha256 != NULL) file_sha256[0] = '\0';
        hwa_e_error(error, error_size,
                    "experiment input needs an active C numeric locale");
        return -1;
    }
    return hwa_e_read_impl(path, limits, result, file_sha256,
                           locale, error, error_size);
}

int hwa_experiment_file_read(
    const char *path,
    const HWAExperimentOptions *limits,
    HWAExperimentResult *result,
    char file_sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    HWANumericLocale locale;
    int status;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        if (result != NULL) memset(result, 0, sizeof(*result));
        if (file_sha256 != NULL) file_sha256[0] = '\0';
        hwa_e_error(error, error_size,
                    "cannot enter the C numeric locale for experiment input");
        return -1;
    }
    status = hwa_experiment_file_read_locale(
        path, limits, result, file_sha256, &locale, error, error_size);
    if (hwa_c_numeric_locale_end(&locale) != 0) {
        if (status == 0 && result != NULL) hwa_experiment_result_free(result);
        if (file_sha256 != NULL) file_sha256[0] = '\0';
        if (status == 0) {
            hwa_e_error(error, error_size,
                        "cannot restore the numeric locale after experiment input");
        }
        return -1;
    }
    return status;
}

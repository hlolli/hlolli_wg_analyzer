#if !defined(_WIN32)
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "gap_report_file.h"

#include "gap_report_output.h"
#include "internal.h"
#include "numeric_locale.h"
#include "production.h"
#include "sha256.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define HWA_GR_FILE_MAX_FIELDS 40U
#define HWA_GR_FILE_MAX_FIELD_BYTES 65536U
#define HWA_GR_HASH_BUFFER_BYTES 65536U
#define HWA_GR_READER_MAX_ULPS 32U

typedef struct HWAGRIdentity {
    uint64_t device;
    uint64_t file;
    uint64_t size;
    int64_t modified_seconds;
    int64_t modified_nanoseconds;
    int64_t changed_seconds;
    int64_t changed_nanoseconds;
} HWAGRIdentity;

/* Keep this layout in step with the source-binding identity in gap_report.c. */
typedef struct HWAGRSourceIdentityLedger {
    uint64_t size;
#if defined(_WIN32)
    uint64_t device;
    uint64_t inode;
#else
    dev_t device;
    ino_t inode;
#endif
} HWAGRSourceIdentityLedger;

typedef struct HWAGRCsvField {
    char *text;
    const unsigned char *raw;
    size_t raw_size;
    int quoted;
} HWAGRCsvField;

typedef struct HWAGRCsvRow {
    HWAGRCsvField fields[HWA_GR_FILE_MAX_FIELDS];
    size_t count;
} HWAGRCsvRow;

typedef struct HWAGRCsvReader {
    const unsigned char *data;
    size_t size;
    size_t cursor;
} HWAGRCsvReader;

typedef struct HWAGRSavedMeta {
    HWAGapReportOptions options;
    HWAGapReportMode mode;
    char *title;
    char *audibility_method;
    char manifest_sha256[HWA_SHA256_HEX_SIZE];
    uint64_t retained_work_bytes;
    uint64_t total_input_bytes;
    uint64_t total_output_bytes;
    uint64_t evaluation_count;
    size_t source_count;
    size_t label_count;
    size_t candidate_count;
    size_t family_count;
    size_t group_count;
    size_t case_count;
    size_t excerpt_count;
    size_t warning_count;
} HWAGRSavedMeta;

static void hwa_grf_error(char *error,
                          size_t error_size,
                          const char *message)
{
    if (error != NULL && error_size != 0U) {
        (void)snprintf(error, error_size, "%s", message);
    }
}

static int hwa_grf_csv_field(FILE *stream, const char *text)
{
    const unsigned char *bytes =
        (const unsigned char *)(text != NULL ? text : "");
    size_t index;
    int quote = 0;
    for (index = 0U; bytes[index] != 0U; ++index) {
        if (bytes[index] == ',' || bytes[index] == '"' ||
            bytes[index] == '\r' || bytes[index] == '\n') quote = 1;
    }
    if (!quote) return fputs((const char *)bytes, stream) == EOF ? -1 : 0;
    if (fputc('"', stream) == EOF) return -1;
    for (index = 0U; bytes[index] != 0U; ++index) {
        if (bytes[index] == '"' && fputc('"', stream) == EOF) return -1;
        if (fputc((int)bytes[index], stream) == EOF) return -1;
    }
    return fputc('"', stream) == EOF ? -1 : 0;
}

static int hwa_grf_double(const HWANumericLocale *locale,
                          char text[64],
                          double value)
{
    return isfinite(value) &&
           hwa_c_locale_format_double(locale, text, 64U,
                                      value == 0.0 ? 0.0 : value) == 0
               ? 0
               : -1;
}

static int hwa_grf_meta_text(FILE *stream,
                             const char *key,
                             const char *value,
                             const char *unit)
{
    return fputs("META,", stream) == EOF ||
                   hwa_grf_csv_field(stream, key) != 0 ||
                   fputc(',', stream) == EOF ||
                   hwa_grf_csv_field(stream, value) != 0 ||
                   fputc(',', stream) == EOF ||
                   hwa_grf_csv_field(stream, unit) != 0 ||
                   fputs("\r\n", stream) == EOF
               ? -1
               : 0;
}

static int hwa_grf_meta_u64(FILE *stream,
                            const char *key,
                            uint64_t value,
                            const char *unit)
{
    char text[32];
    (void)snprintf(text, sizeof(text), "%" PRIu64, value);
    return hwa_grf_meta_text(stream, key, text, unit);
}

static int hwa_grf_meta_size(FILE *stream,
                             const char *key,
                             size_t value,
                             const char *unit)
{
#if SIZE_MAX > UINT64_MAX
    if (value > UINT64_MAX) return -1;
#endif
    return hwa_grf_meta_u64(stream, key, (uint64_t)value, unit);
}

static int hwa_grf_write_meta(FILE *stream,
                              const HWAGapReportResult *r,
                              uint64_t canonical_retained)
{
    const HWAGapReportOptions *o = &r->options;
#define MT(k, v, u)                                                        \
    do { if (hwa_grf_meta_text(stream, (k), (v), (u)) != 0) return -1; } while (0)
#define MU(k, v, u)                                                        \
    do { if (hwa_grf_meta_u64(stream, (k), (v), (u)) != 0) return -1; } while (0)
#define MS(k, v, u)                                                        \
    do { if (hwa_grf_meta_size(stream, (k), (v), (u)) != 0) return -1; } while (0)
    MT("tool_version", HWA_VERSION, "");
    MT("report_method_version", HWA_GAP_REPORT_METHOD_VERSION, "");
    MT("audibility_method", r->audibility_method, "");
    MT("mode", hwa_gap_report_mode_name(r->mode), "");
    MT("title", r->title, "");
    MT("manifest_sha256", r->manifest_sha256, "");
    MS("decode_block_frames", o->decode_block_frames, "frames");
    MU("max_manifest_bytes", o->max_manifest_bytes, "bytes");
    MU("max_input_bytes", o->max_input_bytes, "bytes");
    MU("max_input_frames", o->max_input_frames, "frames");
    MU("max_work_bytes", o->max_work_bytes, "bytes");
    MU("max_evaluations", o->max_evaluations, "evaluations");
    MU("max_output_file_bytes", o->max_output_file_bytes, "bytes");
    MU("max_bundle_bytes", o->max_bundle_bytes, "bytes");
    MU("max_excerpt_frames", o->max_excerpt_frames, "frames");
    MU("max_total_excerpt_frames", o->max_total_excerpt_frames, "frames");
    MS("max_sources", o->max_sources, "sources");
    MS("max_labels", o->max_labels, "labels");
    MS("max_candidates", o->max_candidates, "candidates");
    MS("max_families", o->max_families, "families");
    MS("max_groups", o->max_groups, "groups");
    MS("max_cases", o->max_cases, "cases");
    MS("max_excerpts", o->max_excerpts, "excerpts");
    MS("max_warnings", o->max_warnings, "warnings");
    MS("max_json_depth", o->max_json_depth, "levels");
    MS("max_json_tokens", o->max_json_tokens, "tokens");
#define PM(p, prefix)                                                       \
    MU(prefix "_max_input_bytes", (p).max_input_bytes, "bytes");           \
    MU(prefix "_max_work_bytes", (p).max_work_bytes, "bytes");             \
    MS(prefix "_max_contexts", (p).max_contexts, "contexts");              \
    MS(prefix "_max_measurements", (p).max_measurements, "measurements");  \
    MS(prefix "_max_groups", (p).max_groups, "groups");                    \
    MS(prefix "_max_group_members", (p).max_group_members, "members");     \
    MS(prefix "_max_statistics", (p).max_statistics, "statistics");       \
    MS(prefix "_max_warnings", (p).max_warnings, "warnings");             \
    MS(prefix "_max_distributions", (p).max_distributions, "distributions"); \
    MS(prefix "_max_gaps", (p).max_gaps, "gaps")
#define RU(p, prefix)                                                       \
    MS(prefix "_decode_block_frames", (p).decode_block_frames, "frames");  \
    MU(prefix "_max_manifest_bytes", (p).max_manifest_bytes, "bytes");     \
    MU(prefix "_max_input_bytes", (p).max_input_bytes, "bytes");           \
    MU(prefix "_max_input_frames", (p).max_input_frames, "frames");        \
    MU(prefix "_max_probe_bytes", (p).max_probe_bytes, "bytes");           \
    MU(prefix "_max_probe_values", (p).max_probe_values, "values");        \
    MU(prefix "_max_work_bytes", (p).max_work_bytes, "bytes");             \
    MU(prefix "_max_evaluations", (p).max_evaluations, "evaluations");     \
    MS(prefix "_max_stems", (p).max_stems, "stems");                       \
    MS(prefix "_max_probes", (p).max_probes, "probes");                    \
    MS(prefix "_max_links", (p).max_links, "links");                       \
    MS(prefix "_max_json_depth", (p).max_json_depth, "levels");            \
    MS(prefix "_max_json_tokens", (p).max_json_tokens, "tokens");          \
    MS(prefix "_max_result_rows", (p).max_result_rows, "rows");            \
    MS(prefix "_max_warnings", (p).max_warnings, "warnings")
    PM(o->measurement, "measurement");
    MS("production_decode_block_frames", o->production.decode_block_frames,
       "frames");
    MU("production_max_input_bytes", o->production.max_input_bytes, "bytes");
    MU("production_max_input_frames", o->production.max_input_frames, "frames");
    MU("production_max_ir_frames", o->production.max_ir_frames, "frames");
    MU("production_max_work_bytes", o->production.max_work_bytes, "bytes");
    MU("production_max_evaluations", o->production.max_evaluations,
       "evaluations");
    MS("production_max_spans", o->production.max_spans, "spans");
    MS("production_max_envelope_points", o->production.max_envelope_points,
       "points");
    MS("production_max_fits", o->production.max_fits, "fits");
    MS("production_max_evaluation_rows",
       o->production.max_evaluation_rows, "rows");
    MS("production_max_view_rows", o->production.max_view_rows, "rows");
    MS("production_max_warnings", o->production.max_warnings, "warnings");
    PM(o->production.profile_limits, "production_profile");
    RU(o->run, "run");
    RU(o->experiment.run, "experiment_run");
    MU("experiment_max_manifest_bytes", o->experiment.max_manifest_bytes,
       "bytes");
    MU("experiment_max_input_bytes", o->experiment.max_input_bytes, "bytes");
    MU("experiment_max_work_bytes", o->experiment.max_work_bytes, "bytes");
    MU("experiment_max_bundle_bytes", o->experiment.max_bundle_bytes, "bytes");
    MU("experiment_max_output_file_bytes",
       o->experiment.max_output_file_bytes, "bytes");
    MU("experiment_max_total_run_evaluations",
       o->experiment.max_total_run_evaluations, "evaluations");
    MU("experiment_max_job_milliseconds",
       o->experiment.max_job_milliseconds, "milliseconds");
    MU("experiment_max_total_milliseconds",
       o->experiment.max_total_milliseconds, "milliseconds");
    MS("experiment_max_parameters", o->experiment.max_parameters, "parameters");
    MS("experiment_max_levels", o->experiment.max_levels, "levels");
    MS("experiment_max_cases", o->experiment.max_cases, "cases");
    MS("experiment_max_responses", o->experiment.max_responses, "responses");
    MS("experiment_max_points", o->experiment.max_points, "points");
    MS("experiment_max_jobs", o->experiment.max_jobs, "jobs");
    MS("experiment_max_replicates", o->experiment.max_replicates, "replicates");
    MS("experiment_max_artifacts", o->experiment.max_artifacts, "artifacts");
    MS("experiment_max_observations",
       o->experiment.max_observations, "observations");
    MS("experiment_max_sensitivities",
       o->experiment.max_sensitivities, "sensitivities");
    MS("experiment_max_interactions",
       o->experiment.max_interactions, "interactions");
    MS("experiment_max_warnings", o->experiment.max_warnings, "warnings");
#undef PM
#undef RU
    MU("retained_work_bytes", canonical_retained, "bytes");
    MU("total_input_bytes", r->total_input_bytes, "bytes");
    MU("total_output_bytes", r->total_output_bytes, "bytes");
    MU("evaluation_count", r->evaluation_count, "evaluations");
    MS("source_count", r->source_count, "sources");
    MS("label_count", r->label_count, "labels");
    MS("candidate_count", r->candidate_count, "candidates");
    MS("family_count", r->family_count, "families");
    MS("group_count", r->group_count, "groups");
    MS("case_count", r->case_count, "cases");
    MS("excerpt_count", r->excerpt_count, "excerpts");
    MS("warning_count", r->warning_count, "warnings");
#undef MT
#undef MU
#undef MS
    return 0;
}

static int hwa_grf_optional(FILE *stream,
                            const HWANumericLocale *locale,
                            double value,
                            int valid)
{
    char text[64];
    if (!valid) return 0;
    return hwa_grf_double(locale, text, value) != 0 ||
                   fputs(text, stream) == EOF
               ? -1
               : 0;
}

static int hwa_grf_source(FILE *s, const HWAGapReportSource *r)
{
    return fprintf(s, "SOURCE,%" PRIu64 ",", r->id) < 0 ||
                   hwa_grf_csv_field(s, r->name) != 0 ||
                   fputc(',', s) == EOF ||
                   hwa_grf_csv_field(
                       s, hwa_gap_report_source_kind_name(r->kind)) != 0 ||
                   fprintf(s, ",%s,%" PRIu64 ",%zu\r\n",
                           r->sha256, r->file_bytes,
                           r->candidate_count) < 0
               ? -1
               : 0;
}

static int hwa_grf_label(FILE *s, const HWAGapReportLabel *r)
{
    if (fprintf(s, "LABEL,%" PRIu64 ",%" PRIu64 ",",
                r->id, r->source_id) < 0 ||
        hwa_grf_csv_field(s, r->case_id) != 0 || fputc(',', s) == EOF ||
        hwa_grf_csv_field(s, r->pitch) != 0 || fputc(',', s) == EOF ||
        hwa_grf_csv_field(s, r->register_name) != 0 ||
        fputc(',', s) == EOF ||
        hwa_grf_csv_field(s, r->dynamic) != 0 || fputc(',', s) == EOF ||
        hwa_grf_csv_field(s, r->gesture) != 0 || fputc(',', s) == EOF ||
        hwa_grf_csv_field(s, r->physical_element) != 0 ||
        fputc(',', s) == EOF ||
        hwa_grf_csv_field(s, r->section) != 0 ||
        fputs("\r\n", s) == EOF) return -1;
    return 0;
}

static int hwa_grf_candidate(FILE *s,
                             const HWANumericLocale *locale,
                             const HWAGapReportCandidate *r)
{
    if (fprintf(s, "CANDIDATE,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",",
        r->id, r->source_id, r->source_row) < 0 ||
        hwa_grf_csv_field(s, r->case_id) != 0 || fputc(',', s) == EOF ||
        hwa_grf_csv_field(s, r->metric) != 0 || fputc(',', s) == EOF ||
        hwa_grf_csv_field(s, r->family_key) != 0 || fputc(',', s) == EOF ||
        hwa_grf_csv_field(
            s, hwa_gap_report_candidate_kind_name(r->kind)) != 0 ||
        fputc(',', s) == EOF ||
        hwa_grf_csv_field(
            s, hwa_gap_report_availability_name(r->availability)) != 0 ||
        fputc(',', s) == EOF ||
        hwa_grf_optional(s, locale, r->raw_value,
                         r->raw_value_valid) != 0 ||
        fputc(',', s) == EOF ||
        hwa_grf_optional(s, locale, r->size_factor,
                         r->size_valid) != 0 ||
        fputc(',', s) == EOF ||
        hwa_grf_optional(s, locale, r->audibility_factor,
                         r->audibility_valid) != 0 ||
        fputc(',', s) == EOF ||
        hwa_grf_optional(s, locale, r->confidence_factor,
                         r->confidence_valid) != 0 ||
        fputc(',', s) == EOF ||
        hwa_grf_optional(s, locale, r->score, r->score_valid) != 0 ||
        fprintf(s, ",%" PRIu64 ",%zu,%" PRIu32
                ",%d,%d,%d,%d,%d,%d,",
                r->linked_family_id, r->rank, r->quality_flags,
                r->raw_value_valid, r->size_valid, r->audibility_valid,
                r->confidence_valid, r->score_valid, r->primary) < 0 ||
        hwa_grf_csv_field(s, r->reason) != 0 ||
        fputs("\r\n", s) == EOF) return -1;
    return 0;
}

static int hwa_grf_occurrence(FILE *s,
                              const HWANumericLocale *locale,
                              const HWAGapReportCandidate *r)
{
    if (fprintf(s, "OCCURRENCE,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",",
                r->id, r->occurrence_count, r->eligible_count) < 0 ||
        hwa_grf_optional(s, locale, r->occurrence_factor,
                         r->occurrence_valid) != 0 ||
        fprintf(s, ",%d\r\n", r->occurrence_valid) < 0) return -1;
    return 0;
}

static int hwa_grf_family(FILE *s, const HWAGapReportFamily *r)
{
    if (fprintf(s, "FAMILY,%" PRIu64 ",", r->id) < 0 ||
        hwa_grf_csv_field(s, r->key) != 0 ||
        fprintf(s, ",%" PRIu64 ",%zu,%zu\r\n",
                r->primary_candidate_id, r->member_count, r->rank) < 0) {
        return -1;
    }
    return 0;
}

static int hwa_grf_group(FILE *s,
                         const HWANumericLocale *locale,
                         const HWAGapReportGroup *r)
{
    if (fprintf(s, "GROUP,%" PRIu64 ",", r->id) < 0 ||
        hwa_grf_csv_field(s, hwa_gap_report_axis_name(r->axis)) != 0 ||
        fputc(',', s) == EOF || hwa_grf_csv_field(s, r->value) != 0 ||
        fprintf(s, ",%zu,%zu,%zu,%zu,%zu,", r->family_count,
                r->candidate_count, r->available_count, r->missing_count,
                r->excluded_count) < 0 ||
        hwa_grf_optional(s, locale, r->q05, r->statistics_valid) != 0 ||
        fputc(',', s) == EOF ||
        hwa_grf_optional(s, locale, r->q25, r->statistics_valid) != 0 ||
        fputc(',', s) == EOF ||
        hwa_grf_optional(s, locale, r->median, r->statistics_valid) != 0 ||
        fputc(',', s) == EOF ||
        hwa_grf_optional(s, locale, r->q75, r->statistics_valid) != 0 ||
        fputc(',', s) == EOF ||
        hwa_grf_optional(s, locale, r->q95, r->statistics_valid) != 0 ||
        fputc(',', s) == EOF ||
        hwa_grf_optional(s, locale, r->spread, r->statistics_valid) != 0 ||
        fputc(',', s) == EOF ||
        hwa_grf_optional(s, locale, r->confidence,
                         r->confidence_valid) != 0 ||
        fprintf(s, ",%d,%d\r\n", r->statistics_valid,
                r->confidence_valid) < 0) return -1;
    return 0;
}

static int hwa_grf_case(FILE *s,
                        const HWANumericLocale *locale,
                        const HWAGapReportCase *r)
{
    if (fprintf(s, "WORST,%" PRIu64 ",%" PRIu64 ",",
                r->id, r->candidate_id) < 0 ||
        hwa_grf_csv_field(s, r->case_id) != 0 || fputc(',', s) == EOF ||
        hwa_grf_csv_field(
            s, hwa_gap_report_availability_name(r->availability)) != 0 ||
        fputc(',', s) == EOF ||
        hwa_grf_optional(s, locale, r->value, r->value_valid) != 0 ||
        fputc(',', s) == EOF ||
        hwa_grf_optional(s, locale, r->confidence,
                         r->confidence_valid) != 0 ||
        fputc(',', s) == EOF ||
        hwa_grf_optional(s, locale, r->score, r->score_valid) != 0 ||
        fprintf(s, ",%d,%d,%d,", r->value_valid, r->confidence_valid,
                r->score_valid) < 0 ||
        hwa_grf_csv_field(s, r->reason) != 0 ||
        fputs("\r\n", s) == EOF) return -1;
    return 0;
}

static int hwa_grf_clip(FILE *s,
                        const HWANumericLocale *locale,
                        const HWAGapReportExcerpt *r)
{
    char rg[64];
    char mg[64];
    if (hwa_grf_double(locale, rg, r->reference_gain_db) != 0 ||
        hwa_grf_double(locale, mg, r->model_gain_db) != 0 ||
        fprintf(s, "CLIP,%" PRIu64 ",", r->id) < 0 ||
        hwa_grf_csv_field(s, r->name) != 0 ||
        fprintf(s, ",%" PRIu64 ",%" PRIu64 ",",
                r->candidate_source_id, r->candidate_row) < 0 ||
        hwa_grf_csv_field(s, hwa_gap_report_view_name(r->view)) != 0 ||
        fprintf(s, ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                ",%" PRIu64 ",%d,",
                r->reference_source_id, r->model_source_id,
                r->reference_start_sample, r->model_start_sample,
                r->frame_count, r->make_x) < 0 ||
        hwa_grf_csv_field(
            s, hwa_gap_report_availability_name(r->availability)) != 0 ||
        fprintf(s, ",%s,%s,%d,", rg, mg, r->x_is_reference) < 0 ||
        hwa_grf_csv_field(s, r->reason) != 0 ||
        fputs("\r\n", s) == EOF) return -1;
    return 0;
}

static int hwa_grf_file(FILE *s,
                        const HWAGapReportExcerpt *r,
                        const char *role,
                        const char *path,
                        const char sha[HWA_SHA256_HEX_SIZE],
                        uint64_t bytes)
{
    if (fprintf(s, "FILE,%" PRIu64 ",", r->id) < 0 ||
        hwa_grf_csv_field(s, role) != 0 || fputc(',', s) == EOF ||
        hwa_grf_csv_field(s, path) != 0 ||
        fprintf(s, ",%s,%" PRIu64 "\r\n", sha, bytes) < 0) return -1;
    return 0;
}

static uint64_t hwa_grf_source_input_limit(
    const HWAGapReportOptions *options,
    HWAGapReportSourceKind kind)
{
    switch (kind) {
    case HWA_GAP_REPORT_SOURCE_MEASUREMENT:
        return options->measurement.max_input_bytes;
    case HWA_GAP_REPORT_SOURCE_PRODUCTION:
        return options->production.max_input_bytes;
    case HWA_GAP_REPORT_SOURCE_RUN:
        return options->run.max_input_bytes;
    case HWA_GAP_REPORT_SOURCE_EXPERIMENT:
        return options->experiment.max_output_file_bytes <
                       options->experiment.max_bundle_bytes
            ? options->experiment.max_output_file_bytes
            : options->experiment.max_bundle_bytes;
    case HWA_GAP_REPORT_SOURCE_WAVE:
        return options->max_input_bytes;
    default:
        return 0U;
    }
}

static int hwa_grf_source_reader_work_required(
    const HWAGapReportOptions *options,
    const HWAGapReportSource *source,
    uint64_t *required,
    uint64_t *maximum)
{
    uint64_t fixed;
    uint64_t variable;
    if (options == NULL || source == NULL || required == NULL ||
        maximum == NULL) return -1;
    if (source->kind == HWA_GAP_REPORT_SOURCE_MEASUREMENT) {
        *maximum = options->measurement.max_work_bytes;
        fixed = UINT64_C(5243002) +
            UINT64_C(40) * (uint64_t)sizeof(char *);
    } else if (source->kind == HWA_GAP_REPORT_SOURCE_PRODUCTION) {
        *maximum = options->production.max_work_bytes;
        fixed = UINT64_C(6357138) +
            UINT64_C(48) * (uint64_t)sizeof(char *);
    } else {
        fixed = 0U;
        *maximum = 0U;
    }
    if (fixed != 0U) {
        if (source->file_bytes == UINT64_MAX ||
            source->file_bytes + UINT64_C(1) >
                (UINT64_MAX - fixed) / UINT64_C(8)) return -1;
        variable = (source->file_bytes + UINT64_C(1)) * UINT64_C(8);
        *required = fixed + variable;
        return 0;
    }
    if (source->kind == HWA_GAP_REPORT_SOURCE_RUN)
        *maximum = options->run.max_work_bytes;
    else if (source->kind == HWA_GAP_REPORT_SOURCE_EXPERIMENT)
        *maximum = options->experiment.max_work_bytes;
    else {
        *required = 0U;
        return 0;
    }
    if (source->file_bytes == UINT64_MAX ||
        source->file_bytes + UINT64_C(1) > UINT64_MAX / UINT64_C(3))
        return -1;
    *required = (source->file_bytes + UINT64_C(1)) * UINT64_C(3);
    return 0;
}

static int hwa_grf_work_add(uint64_t *total, uint64_t value)
{
    if (total == NULL || value > UINT64_MAX - *total) return -1;
    *total += value;
    return 0;
}

static int hwa_grf_work_array(uint64_t *total,
                              size_t count,
                              size_t item_size)
{
    if (item_size != 0U && count > SIZE_MAX / item_size) return -1;
    return hwa_grf_work_add(total, (uint64_t)(count * item_size));
}

static int hwa_grf_work_text(uint64_t *total, const char *text)
{
    size_t length;
    if (text == NULL) return 0;
    length = strlen(text);
    if (length == SIZE_MAX) return -1;
    return hwa_grf_work_add(total, (uint64_t)length + UINT64_C(1));
}

/*
 * Recreate the live result state held when a fresh Stage 9 build invokes a
 * source adapter. Saved paths use their canonical dot form; output paths and
 * final clip status do not change this pre-adapter state.
 */
static int hwa_grf_pre_adapter_live_work(
    const HWAGapReportResult *result,
    uint64_t *bytes)
{
    uint64_t total = sizeof(*result);
    size_t index;
    const char *excerpt_reason;
    if (result == NULL || bytes == NULL ||
        hwa_grf_work_array(&total, result->source_count,
                           sizeof(*result->sources)) != 0 ||
        hwa_grf_work_array(&total, result->label_count,
                           sizeof(*result->labels)) != 0 ||
        hwa_grf_work_array(&total, result->excerpt_count,
                           sizeof(*result->excerpts)) != 0 ||
        hwa_grf_work_array(&total, result->source_count,
                           sizeof(HWAGRSourceIdentityLedger)) != 0 ||
        hwa_grf_work_text(&total, ".") != 0 ||
        (result->mode != HWA_GAP_REPORT_RANK &&
         hwa_grf_work_text(&total, ".") != 0) ||
        hwa_grf_work_text(&total, result->title) != 0 ||
        hwa_grf_work_text(&total, result->audibility_method) != 0)
        return -1;
    for (index = 0U; index < result->source_count; ++index) {
        if (hwa_grf_work_text(&total, result->sources[index].name) != 0 ||
            hwa_grf_work_text(&total, ".") != 0) return -1;
    }
    for (index = 0U; index < result->label_count; ++index) {
        const HWAGapReportLabel *row = &result->labels[index];
        if (hwa_grf_work_text(&total, row->case_id) != 0 ||
            hwa_grf_work_text(&total, row->pitch) != 0 ||
            hwa_grf_work_text(&total, row->register_name) != 0 ||
            hwa_grf_work_text(&total, row->dynamic) != 0 ||
            hwa_grf_work_text(&total, row->gesture) != 0 ||
            hwa_grf_work_text(&total, row->physical_element) != 0 ||
            hwa_grf_work_text(&total, row->section) != 0) return -1;
    }
    excerpt_reason = result->mode == HWA_GAP_REPORT_RANK
        ? "rank-mode-does-not-render-excerpts" : "excerpt-not-rendered";
    for (index = 0U; index < result->excerpt_count; ++index) {
        if (hwa_grf_work_text(&total, result->excerpts[index].name) != 0 ||
            hwa_grf_work_text(&total, "") != 0 ||
            hwa_grf_work_text(&total, "") != 0 ||
            hwa_grf_work_text(&total, "") != 0 ||
            hwa_grf_work_text(&total, excerpt_reason) != 0) return -1;
    }
    *bytes = total;
    return 0;
}

static int hwa_grf_producer_reader_work_fits(
    const HWAGapReportResult *result,
    const HWAGapReportOptions *options)
{
    uint64_t outer;
    size_t index;
    if (hwa_grf_pre_adapter_live_work(result, &outer) != 0 ||
        outer > options->max_work_bytes) return 0;
    for (index = 0U; index < result->source_count; ++index) {
        uint64_t required;
        uint64_t maximum;
        if (hwa_grf_source_reader_work_required(
                options, &result->sources[index],
                &required, &maximum) != 0 ||
            required > maximum ||
            required > options->max_work_bytes - outer) return 0;
    }
    return 1;
}

static int hwa_grf_source_name_valid(const char *name)
{
    size_t index;
    size_t length;
    if (name == NULL || (length = strlen(name)) == 0U || length > 255U)
        return 0;
    for (index = 0U; index < length; ++index) {
        unsigned char c = (unsigned char)name[index];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}

static int hwa_grf_excerpt_name_valid(const char *name)
{
    size_t name_size;
    size_t index;
    if (name == NULL) return 0;
    name_size = strlen(name);
    if (name_size == 0U || name_size > 255U) return 0;
    for (index = 0U; index < name_size; ++index) {
        unsigned char c = (unsigned char)name[index];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) return 0;
    }
    return 1;
}

static int hwa_grf_exact_audio_path(const char *path,
                                    const char *name,
                                    char suffix)
{
    size_t name_size;
    size_t path_size;
    if (path == NULL || !hwa_grf_excerpt_name_valid(name)) return 0;
    name_size = strlen(name);
    path_size = strlen(path);
    return name_size <= SIZE_MAX - 13U &&
           path_size == name_size + 12U &&
           memcmp(path, "audio/", 6U) == 0 &&
           memcmp(path + 6U, name, name_size) == 0 &&
           path[6U + name_size] == '-' &&
           path[7U + name_size] == suffix &&
           memcmp(path + 8U + name_size, ".wav", 5U) == 0;
}

static int hwa_grf_excerpt_paths_canonical(
    const HWAGapReportResult *result)
{
    size_t index;
    for (index = 0U; index < result->excerpt_count; ++index) {
        const HWAGapReportExcerpt *r = &result->excerpts[index];
        if (!hwa_grf_excerpt_name_valid(r->name)) return 0;
        if (r->availability == HWA_GAP_REPORT_AVAILABLE) {
            if (!hwa_grf_exact_audio_path(
                    r->reference_path, r->name, 'a') ||
                !hwa_grf_exact_audio_path(r->model_path, r->name, 'b') ||
                (r->make_x
                    ? !hwa_grf_exact_audio_path(r->x_path, r->name, 'x')
                    : (r->x_path == NULL || r->x_path[0] != '\0')))
                return 0;
        } else if (r->reference_path == NULL ||
                   r->reference_path[0] != '\0' ||
                   r->model_path == NULL || r->model_path[0] != '\0' ||
                   r->x_path == NULL || r->x_path[0] != '\0') {
            return 0;
        }
    }
    return 1;
}

static int hwa_grf_producer_facts_fit(
    const HWAGapReportResult *result,
    const HWAGapReportOptions *options)
{
    size_t index;
    for (index = 0U; index < result->source_count; ++index) {
        const HWAGapReportSource *source = &result->sources[index];
        uint64_t limit = hwa_grf_source_input_limit(options, source->kind);
        if (!hwa_grf_source_name_valid(source->name) || limit == 0U ||
            source->file_bytes > limit) return 0;
    }
    return hwa_gap_report_candidate_catalog_fits(result, options);
}

static int hwa_grf_exclusion(FILE *s, const HWAGapReportCandidate *r)
{
    if (fprintf(s, "EXCLUSION,%" PRIu64 ",", r->id) < 0 ||
        hwa_grf_csv_field(
            s, hwa_gap_report_availability_name(r->availability)) != 0 ||
        fputc(',', s) == EOF || hwa_grf_csv_field(s, r->reason) != 0 ||
        fputs("\r\n", s) == EOF) return -1;
    return 0;
}

static int hwa_grf_failure(FILE *s, const HWAGapReportExcerpt *r)
{
    if (fprintf(s, "FAILURE,%" PRIu64 ",", r->id) < 0 ||
        hwa_grf_csv_field(
            s, hwa_gap_report_availability_name(r->availability)) != 0 ||
        fputc(',', s) == EOF || hwa_grf_csv_field(s, r->reason) != 0 ||
        fputs("\r\n", s) == EOF) return -1;
    return 0;
}

static int hwa_grf_warning(FILE *s, const HWAGapReportWarning *r)
{
    if (fprintf(s, "WARNING,%" PRIu64 ",", r->id) < 0 ||
        hwa_grf_csv_field(s, r->code) != 0 || fputc(',', s) == EOF ||
        hwa_grf_csv_field(s, r->message) != 0 ||
        fprintf(s, ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%d,%d,%d\r\n",
                r->source_id, r->candidate_id, r->excerpt_id,
                r->source_id_valid, r->candidate_id_valid,
                r->excerpt_id_valid) < 0) return -1;
    return 0;
}

static int hwa_grf_bound_add(uint64_t *total, uint64_t value)
{
    if (value > UINT64_MAX - *total) return -1;
    *total += value;
    return 0;
}

static int hwa_grf_bound_rows(uint64_t *total,
                              size_t count,
                              uint64_t per_row)
{
    if ((uint64_t)count > UINT64_MAX / per_row)
        return -1;
    return hwa_grf_bound_add(total, (uint64_t)count * per_row);
}

static int hwa_grf_bound_text(uint64_t *total, const char *text)
{
    size_t length = text == NULL ? 0U : strlen(text);
    if (length > HWA_GR_FILE_MAX_FIELD_BYTES) return -1;
    return hwa_grf_bound_add(
        total, (uint64_t)length * UINT64_C(2) + UINT64_C(2));
}

/*
 * This bound covers fixed metadata, 63-byte numeric fields, CSV quoting, and
 * every row string. It is deliberately above the exact canonical size so a
 * failed cap check happens before tmpfile() can consume disk space.
 */
static int hwa_grf_file_upper_bound(const HWAGapReportResult *result,
                                    uint64_t *bytes)
{
    uint64_t total = UINT64_C(16384);
    size_t index;
    if (hwa_grf_bound_text(&total, result->title) != 0 ||
        hwa_grf_bound_text(&total, result->audibility_method) != 0 ||
        hwa_grf_bound_rows(&total, result->source_count, 256U) != 0 ||
        hwa_grf_bound_rows(&total, result->label_count, 256U) != 0 ||
        hwa_grf_bound_rows(&total, result->candidate_count, 1024U) != 0 ||
        hwa_grf_bound_rows(&total, result->family_count, 256U) != 0 ||
        hwa_grf_bound_rows(&total, result->group_count, 1024U) != 0 ||
        hwa_grf_bound_rows(&total, result->case_count, 512U) != 0 ||
        hwa_grf_bound_rows(&total, result->excerpt_count, 2048U) != 0 ||
        hwa_grf_bound_rows(&total, result->warning_count, 512U) != 0)
        return -1;
    for (index = 0U; index < result->source_count; ++index)
        if (hwa_grf_bound_text(&total, result->sources[index].name) != 0)
            return -1;
    for (index = 0U; index < result->label_count; ++index) {
        const HWAGapReportLabel *r = &result->labels[index];
        if (hwa_grf_bound_text(&total, r->case_id) != 0 ||
            hwa_grf_bound_text(&total, r->pitch) != 0 ||
            hwa_grf_bound_text(&total, r->register_name) != 0 ||
            hwa_grf_bound_text(&total, r->dynamic) != 0 ||
            hwa_grf_bound_text(&total, r->gesture) != 0 ||
            hwa_grf_bound_text(&total, r->physical_element) != 0 ||
            hwa_grf_bound_text(&total, r->section) != 0) return -1;
    }
    for (index = 0U; index < result->candidate_count; ++index) {
        const HWAGapReportCandidate *r = &result->candidates[index];
        if (hwa_grf_bound_text(&total, r->case_id) != 0 ||
            hwa_grf_bound_text(&total, r->metric) != 0 ||
            hwa_grf_bound_text(&total, r->family_key) != 0 ||
            hwa_grf_bound_text(&total, r->reason) != 0 ||
            (r->availability != HWA_GAP_REPORT_AVAILABLE &&
             hwa_grf_bound_text(&total, r->reason) != 0)) return -1;
    }
    for (index = 0U; index < result->family_count; ++index)
        if (hwa_grf_bound_text(&total, result->families[index].key) != 0)
            return -1;
    for (index = 0U; index < result->group_count; ++index)
        if (hwa_grf_bound_text(&total, result->groups[index].value) != 0)
            return -1;
    for (index = 0U; index < result->case_count; ++index) {
        const HWAGapReportCase *r = &result->cases[index];
        if (hwa_grf_bound_text(&total, r->case_id) != 0 ||
            hwa_grf_bound_text(&total, r->reason) != 0) return -1;
    }
    for (index = 0U; index < result->excerpt_count; ++index) {
        const HWAGapReportExcerpt *r = &result->excerpts[index];
        if (hwa_grf_bound_text(&total, r->name) != 0 ||
            hwa_grf_bound_text(&total, r->reference_path) != 0 ||
            hwa_grf_bound_text(&total, r->model_path) != 0 ||
            hwa_grf_bound_text(&total, r->x_path) != 0 ||
            hwa_grf_bound_text(&total, r->reason) != 0 ||
            (r->availability != HWA_GAP_REPORT_AVAILABLE &&
             hwa_grf_bound_text(&total, r->reason) != 0)) return -1;
    }
    for (index = 0U; index < result->warning_count; ++index) {
        const HWAGapReportWarning *r = &result->warnings[index];
        if (hwa_grf_bound_text(&total, r->code) != 0 ||
            hwa_grf_bound_text(&total, r->message) != 0) return -1;
    }
    *bytes = total;
    return 0;
}

static int hwa_grf_file_minimum_bound(const HWAGapReportResult *result,
                                      uint64_t *bytes)
{
    uint64_t total = UINT64_C(14);
    if (hwa_grf_bound_rows(&total, result->source_count, 11U) != 0 ||
        hwa_grf_bound_rows(&total, result->label_count, 9U) != 0 ||
        hwa_grf_bound_rows(&total, result->candidate_count, 27U) != 0 ||
        hwa_grf_bound_rows(&total, result->family_count, 10U) != 0 ||
        hwa_grf_bound_rows(&total, result->group_count, 9U) != 0 ||
        hwa_grf_bound_rows(&total, result->case_count, 9U) != 0 ||
        hwa_grf_bound_rows(&total, result->excerpt_count, 8U) != 0 ||
        hwa_grf_bound_rows(&total, result->warning_count, 11U) != 0)
        return -1;
    *bytes = total;
    return 0;
}

static int hwa_grf_write_impl(FILE *stream,
                              const HWAGapReportResult *result,
                              const HWANumericLocale *locale,
                              char *error,
                              size_t error_size)
{
    uint64_t retained;
    size_t index;
    if (stream == NULL || result == NULL ||
        hwa_gap_report_result_validate(result, error, error_size) != 0 ||
        !hwa_grf_producer_facts_fit(result, &result->options) ||
        !hwa_grf_producer_reader_work_fits(result, &result->options) ||
        !hwa_grf_excerpt_paths_canonical(result) ||
        hwa_gap_report_result_canonical_retained_bytes(
            result, &retained) != 0) return -1;
    if (fputs("HWA_REPORT,1\r\n", stream) == EOF ||
        hwa_grf_write_meta(stream, result, retained) != 0) goto failed;
    for (index = 0U; index < result->source_count; ++index)
        if (hwa_grf_source(stream, &result->sources[index]) != 0) goto failed;
    for (index = 0U; index < result->label_count; ++index)
        if (hwa_grf_label(stream, &result->labels[index]) != 0) goto failed;
    for (index = 0U; index < result->candidate_count; ++index)
        if (hwa_grf_candidate(
                stream, locale, &result->candidates[index]) != 0) goto failed;
    for (index = 0U; index < result->candidate_count; ++index)
        if (hwa_grf_occurrence(
                stream, locale, &result->candidates[index]) != 0) goto failed;
    for (index = 0U; index < result->family_count; ++index)
        if (hwa_grf_family(stream, &result->families[index]) != 0) goto failed;
    for (index = 0U; index < result->group_count; ++index)
        if (hwa_grf_group(stream, locale, &result->groups[index]) != 0)
            goto failed;
    for (index = 0U; index < result->case_count; ++index)
        if (hwa_grf_case(stream, locale, &result->cases[index]) != 0)
            goto failed;
    for (index = 0U; index < result->excerpt_count; ++index)
        if (hwa_grf_clip(stream, locale, &result->excerpts[index]) != 0)
            goto failed;
    for (index = 0U; index < result->excerpt_count; ++index) {
        const HWAGapReportExcerpt *r = &result->excerpts[index];
        if (r->availability == HWA_GAP_REPORT_AVAILABLE) {
            if (hwa_grf_file(stream, r, "a", r->reference_path,
                             r->reference_sha256,
                             r->reference_file_bytes) != 0 ||
                hwa_grf_file(stream, r, "b", r->model_path,
                             r->model_sha256, r->model_file_bytes) != 0 ||
                (r->make_x &&
                 hwa_grf_file(stream, r, "x", r->x_path, r->x_sha256,
                              r->x_file_bytes) != 0)) goto failed;
        }
    }
    for (index = 0U; index < result->candidate_count; ++index)
        if (result->candidates[index].availability !=
                HWA_GAP_REPORT_AVAILABLE &&
            hwa_grf_exclusion(stream, &result->candidates[index]) != 0)
            goto failed;
    for (index = 0U; index < result->excerpt_count; ++index)
        if (result->excerpts[index].availability !=
                HWA_GAP_REPORT_AVAILABLE &&
            hwa_grf_failure(stream, &result->excerpts[index]) != 0)
            goto failed;
    for (index = 0U; index < result->warning_count; ++index)
        if (hwa_grf_warning(stream, &result->warnings[index]) != 0)
            goto failed;
    return 0;
failed:
    hwa_grf_error(error, error_size, "cannot write Stage 9 report result");
    return -1;
}

int hwa_gap_report_file_write(FILE *stream,
                              const HWAGapReportResult *result,
                              char *error,
                              size_t error_size)
{
    HWANumericLocale locale;
    FILE *scratch = NULL;
    unsigned char buffer[65536];
    uint64_t length = 0U;
    uint64_t total = 0U;
    uint64_t minimum = 0U;
    uint64_t upper = 0U;
    uint64_t tree_upper = 0U;
    uint64_t projected_audio = 0U;
    uint64_t available;
    size_t count;
    int status = -1;
    size_t index;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (stream == NULL || result == NULL) {
        hwa_grf_error(
            error, error_size, "invalid Stage 9 report writer arguments");
        return -1;
    }
    if (result->total_output_bytes > result->options.max_bundle_bytes) {
        hwa_grf_error(error, error_size,
                      "Stage 9 report exceeds its bundle limit");
        return -1;
    }
    available = result->options.max_bundle_bytes -
                result->total_output_bytes;
    if (result->mode != HWA_GAP_REPORT_RANK) {
        for (index = 0U; index < result->excerpt_count; ++index) {
            const HWAGapReportExcerpt *excerpt = &result->excerpts[index];
            uint64_t file_bytes;
            uint64_t file_count = excerpt->make_x ? UINT64_C(3) : UINT64_C(2);
            if (excerpt->frame_count >
                    (UINT64_MAX - UINT64_C(44)) / UINT64_C(2))
                return -1;
            file_bytes = UINT64_C(44) + excerpt->frame_count * UINT64_C(2);
            if (file_bytes > UINT64_MAX / file_count ||
                file_bytes * file_count > UINT64_MAX - projected_audio)
                return -1;
            projected_audio += file_bytes * file_count;
        }
    }
    if (hwa_grf_file_minimum_bound(result, &minimum) != 0 ||
        minimum > result->options.max_output_file_bytes ||
        minimum > available ||
        hwa_gap_report_result_validate(result, error, error_size) != 0 ||
        !hwa_grf_producer_facts_fit(result, &result->options) ||
        !hwa_grf_producer_reader_work_fits(result, &result->options) ||
        !hwa_grf_excerpt_paths_canonical(result) ||
        hwa_grf_file_upper_bound(result, &upper) != 0 ||
        upper > result->options.max_output_file_bytes ||
        upper > available ||
        (result->mode != HWA_GAP_REPORT_RANK &&
         (hwa_gap_report_output_projected_upper(
              result, result->mode, projected_audio, &tree_upper) != 0 ||
          tree_upper > result->options.max_bundle_bytes))) {
        if (error == NULL || error_size == 0U || error[0] == '\0')
            hwa_grf_error(error, error_size,
                          "Stage 9 report cannot fit its output limits");
        return -1;
    }
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_grf_error(
            error, error_size, "invalid Stage 9 report writer arguments");
        return -1;
    }
    scratch = tmpfile();
    if (scratch == NULL ||
        hwa_grf_write_impl(
            scratch, result, &locale, error, error_size) != 0 ||
        fflush(scratch) != 0 || fseek(scratch, 0L, SEEK_END) != 0 ||
#if defined(_WIN32)
        ((length = (uint64_t)_ftelli64(scratch)),
         (int64_t)length < 0) ||
#else
        ((length = (uint64_t)ftello(scratch)), (int64_t)length < 0) ||
#endif
        length > result->options.max_output_file_bytes ||
        result->total_output_bytes > result->options.max_bundle_bytes ||
        length > result->options.max_bundle_bytes -
                     result->total_output_bytes ||
        fseek(scratch, 0L, SEEK_SET) != 0) goto cleanup;
    while ((count = fread(buffer, 1U, sizeof(buffer), scratch)) != 0U) {
        if (fwrite(buffer, 1U, count, stream) != count ||
            (uint64_t)count > UINT64_MAX - total) goto cleanup;
        total += (uint64_t)count;
    }
    if (ferror(scratch) || total != length) goto cleanup;
    status = 0;
cleanup:
    if (scratch != NULL && fclose(scratch) != 0) status = -1;
    if (hwa_c_numeric_locale_end(&locale) != 0) status = -1;
    if (status != 0 &&
        (error == NULL || error_size == 0U || error[0] == '\0')) {
        hwa_grf_error(error, error_size,
                      "cannot finish Stage 9 report output");
    }
    return status;
}

#if defined(_WIN32)
static void hwa_grf_windows_identity(
    const BY_HANDLE_FILE_INFORMATION *info,
    HANDLE handle,
    HWAGRIdentity *identity)
{
    FILE_BASIC_INFO basic;
    identity->device = (uint64_t)info->dwVolumeSerialNumber;
    identity->file = ((uint64_t)info->nFileIndexHigh << 32U) |
                     (uint64_t)info->nFileIndexLow;
    identity->size = ((uint64_t)info->nFileSizeHigh << 32U) |
                     (uint64_t)info->nFileSizeLow;
    identity->modified_seconds =
        ((int64_t)info->ftLastWriteTime.dwHighDateTime << 32U) |
        (int64_t)info->ftLastWriteTime.dwLowDateTime;
    identity->modified_nanoseconds = 0;
    identity->changed_seconds =
        GetFileInformationByHandleEx(
            handle, FileBasicInfo, &basic, (DWORD)sizeof(basic))
            ? (int64_t)basic.ChangeTime.QuadPart
            : identity->modified_seconds;
    identity->changed_nanoseconds = 0;
}

static int hwa_grf_path_identity(const char *path, HWAGRIdentity *identity)
{
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE handle = CreateFileA(
        path, 0U, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(handle, &info)) {
        if (handle != INVALID_HANDLE_VALUE) (void)CloseHandle(handle);
        return -1;
    }
    if ((info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        (void)CloseHandle(handle);
        return -1;
    }
    hwa_grf_windows_identity(&info, handle, identity);
    (void)CloseHandle(handle);
    return 0;
}

static int hwa_grf_stream_identity(FILE *stream, HWAGRIdentity *identity)
{
    BY_HANDLE_FILE_INFORMATION info;
    int descriptor = _fileno(stream);
    intptr_t raw = descriptor >= 0
        ? _get_osfhandle(descriptor) : (intptr_t)-1;
    if (raw == (intptr_t)-1 ||
        !GetFileInformationByHandle((HANDLE)raw, &info) ||
        (info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        return -1;
    }
    hwa_grf_windows_identity(&info, (HANDLE)raw, identity);
    return 0;
}
#else
static int hwa_grf_path_identity(const char *path, HWAGRIdentity *identity)
{
    struct stat facts;
    if (lstat(path, &facts) != 0 || !S_ISREG(facts.st_mode) ||
        facts.st_size < 0) return -1;
    identity->device = (uint64_t)facts.st_dev;
    identity->file = (uint64_t)facts.st_ino;
    identity->size = (uint64_t)facts.st_size;
#if defined(__APPLE__)
    identity->modified_seconds = (int64_t)facts.st_mtimespec.tv_sec;
    identity->modified_nanoseconds = (int64_t)facts.st_mtimespec.tv_nsec;
    identity->changed_seconds = (int64_t)facts.st_ctimespec.tv_sec;
    identity->changed_nanoseconds = (int64_t)facts.st_ctimespec.tv_nsec;
#else
    identity->modified_seconds = (int64_t)facts.st_mtim.tv_sec;
    identity->modified_nanoseconds = (int64_t)facts.st_mtim.tv_nsec;
    identity->changed_seconds = (int64_t)facts.st_ctim.tv_sec;
    identity->changed_nanoseconds = (int64_t)facts.st_ctim.tv_nsec;
#endif
    return 0;
}

static int hwa_grf_stream_identity(FILE *stream, HWAGRIdentity *identity)
{
    struct stat facts;
    int descriptor = fileno(stream);
    if (descriptor < 0 || fstat(descriptor, &facts) != 0 ||
        !S_ISREG(facts.st_mode) || facts.st_size < 0) return -1;
    identity->device = (uint64_t)facts.st_dev;
    identity->file = (uint64_t)facts.st_ino;
    identity->size = (uint64_t)facts.st_size;
#if defined(__APPLE__)
    identity->modified_seconds = (int64_t)facts.st_mtimespec.tv_sec;
    identity->modified_nanoseconds = (int64_t)facts.st_mtimespec.tv_nsec;
    identity->changed_seconds = (int64_t)facts.st_ctimespec.tv_sec;
    identity->changed_nanoseconds = (int64_t)facts.st_ctimespec.tv_nsec;
#else
    identity->modified_seconds = (int64_t)facts.st_mtim.tv_sec;
    identity->modified_nanoseconds = (int64_t)facts.st_mtim.tv_nsec;
    identity->changed_seconds = (int64_t)facts.st_ctim.tv_sec;
    identity->changed_nanoseconds = (int64_t)facts.st_ctim.tv_nsec;
#endif
    return 0;
}
#endif

static FILE *hwa_grf_open_read(const char *path)
{
#if defined(_WIN32)
    HANDLE handle = CreateFileA(
        path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BY_HANDLE_FILE_INFORMATION info;
    int descriptor;
    FILE *stream;
    if (handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        if (handle != INVALID_HANDLE_VALUE) (void)CloseHandle(handle);
        return NULL;
    }
    descriptor = _open_osfhandle(
        (intptr_t)handle, _O_RDONLY | _O_BINARY);
    if (descriptor < 0) {
        (void)CloseHandle(handle);
        return NULL;
    }
    stream = _fdopen(descriptor, "rb");
    if (stream == NULL) (void)_close(descriptor);
    return stream;
#else
    int flags = O_RDONLY;
    int descriptor;
    FILE *stream;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    descriptor = open(path, flags);
    if (descriptor < 0) return NULL;
    stream = fdopen(descriptor, "rb");
    if (stream == NULL) (void)close(descriptor);
    return stream;
#endif
}

static int hwa_grf_same_identity(const HWAGRIdentity *a,
                                 const HWAGRIdentity *b)
{
    return a->device == b->device && a->file == b->file &&
           a->size == b->size &&
           a->modified_seconds == b->modified_seconds &&
           a->modified_nanoseconds == b->modified_nanoseconds &&
           a->changed_seconds == b->changed_seconds &&
           a->changed_nanoseconds == b->changed_nanoseconds;
}

static void hwa_grf_digest_hex(const unsigned char digest[32],
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

static int hwa_grf_read_bytes(
    const char *path,
    const HWAGapReportOptions *limits,
    unsigned char **data,
    size_t *size,
    char sha[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    HWAGRIdentity before;
    HWAGRIdentity opened;
    HWAGRIdentity after;
    HWASha256 hash;
    HWASha256 verify_hash;
    unsigned char digest[32];
    unsigned char verify_digest[32];
    unsigned char verify_buffer[HWA_GR_HASH_BUFFER_BYTES];
    FILE *stream = NULL;
    unsigned char *buffer = NULL;
    size_t offset = 0U;
    size_t file_size;
    int status = -1;
    *data = NULL;
    *size = 0U;
    if (path == NULL || path[0] == '\0' || strcmp(path, "-") == 0 ||
        hwa_grf_path_identity(path, &before) != 0) {
        hwa_grf_error(error, error_size,
                      "Stage 9 result must be a named regular file");
        return -1;
    }
    if (before.size > limits->max_input_bytes ||
        before.size > limits->max_output_file_bytes ||
        before.size > limits->max_bundle_bytes ||
        before.size > (uint64_t)(SIZE_MAX - 1U) ||
        before.size + 1U > limits->max_work_bytes / 3U) {
        hwa_grf_error(error, error_size,
                      "Stage 9 result exceeds the current byte or work limit");
        return -1;
    }
    file_size = (size_t)before.size;
    buffer = (unsigned char *)malloc(file_size + 1U);
    if (buffer == NULL) {
        hwa_grf_error(error, error_size,
                      "cannot allocate the Stage 9 result reader");
        return -1;
    }
    stream = hwa_grf_open_read(path);
    if (stream == NULL ||
        hwa_grf_stream_identity(stream, &opened) != 0 ||
        !hwa_grf_same_identity(&before, &opened)) {
        hwa_grf_error(error, error_size,
                      "Stage 9 result changed before it was opened");
        goto cleanup;
    }
    hwa_sha256_init(&hash);
    while (offset < file_size) {
        size_t count = fread(
            buffer + offset, 1U, file_size - offset, stream);
        if (count == 0U) {
            hwa_grf_error(error, error_size,
                          "cannot read the complete Stage 9 result");
            goto cleanup;
        }
        hwa_sha256_update(&hash, buffer + offset, count);
        offset += count;
    }
    if (fgetc(stream) != EOF || ferror(stream) ||
        fseek(stream, 0L, SEEK_SET) != 0) {
        hwa_grf_error(error, error_size,
                      "Stage 9 result changed while it was read");
        goto cleanup;
    }
    hwa_sha256_init(&verify_hash);
    offset = 0U;
    while (offset < file_size) {
        size_t wanted = file_size - offset;
        size_t count;
        if (wanted > sizeof(verify_buffer)) wanted = sizeof(verify_buffer);
        count = fread(verify_buffer, 1U, wanted, stream);
        if (count != wanted) {
            hwa_grf_error(error, error_size,
                          "Stage 9 result changed during final hashing");
            goto cleanup;
        }
        hwa_sha256_update(&verify_hash, verify_buffer, count);
        offset += count;
    }
    if (fgetc(stream) != EOF || ferror(stream)) {
        hwa_grf_error(error, error_size,
                      "Stage 9 result changed during final hashing");
        goto cleanup;
    }
    if (fclose(stream) != 0) {
        stream = NULL;
        hwa_grf_error(error, error_size,
                      "cannot close the Stage 9 result");
        goto cleanup;
    }
    stream = NULL;
    if (hwa_grf_path_identity(path, &after) != 0 ||
        !hwa_grf_same_identity(&before, &after)) {
        hwa_grf_error(error, error_size,
                      "Stage 9 result changed while it was read");
        goto cleanup;
    }
    buffer[file_size] = 0U;
    hwa_sha256_final(&hash, digest);
    hwa_sha256_final(&verify_hash, verify_digest);
    if (memcmp(digest, verify_digest, sizeof(digest)) != 0) {
        hwa_grf_error(error, error_size,
                      "Stage 9 result content changed while it was read");
        goto cleanup;
    }
    hwa_grf_digest_hex(digest, sha);
    *data = buffer;
    *size = file_size;
    buffer = NULL;
    status = 0;
cleanup:
    if (stream != NULL) (void)fclose(stream);
    free(buffer);
    return status;
}

static void hwa_grf_row_free(HWAGRCsvRow *row)
{
    size_t index;
    for (index = 0U; index < row->count; ++index)
        free(row->fields[index].text);
    memset(row, 0, sizeof(*row));
}

static int hwa_grf_copy_field(HWAGRCsvField *field,
                              const unsigned char *raw,
                              size_t raw_size,
                              int quoted)
{
    size_t source;
    size_t target = 0U;
    size_t decoded = raw_size;
    char *text;
    if (quoted) {
        if (raw_size < 2U || raw[0] != '"' ||
            raw[raw_size - 1U] != '"') return -1;
        decoded = 0U;
        for (source = 1U; source + 1U < raw_size; ++source) {
            if (raw[source] == '"') {
                if (source + 2U >= raw_size ||
                    raw[source + 1U] != '"') return -1;
                source++;
            }
            decoded++;
        }
    }
    if (decoded > HWA_GR_FILE_MAX_FIELD_BYTES) return -1;
    text = (char *)malloc(decoded + 1U);
    if (text == NULL) return -1;
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
        return -1;
    }
    field->text = text;
    field->raw = raw;
    field->raw_size = raw_size;
    field->quoted = quoted;
    return 0;
}

static int hwa_grf_csv_next(HWAGRCsvReader *reader,
                            HWAGRCsvRow *row,
                            char *error,
                            size_t error_size)
{
    memset(row, 0, sizeof(*row));
    if (reader->cursor == reader->size) return 0;
    for (;;) {
        size_t start = reader->cursor;
        size_t raw_size;
        int quoted = 0;
        if (row->count == HWA_GR_FILE_MAX_FIELDS) goto invalid;
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
        if (hwa_grf_copy_field(
                &row->fields[row->count], reader->data + start,
                raw_size, quoted) != 0) goto invalid;
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
    hwa_grf_row_free(row);
    hwa_grf_error(error, error_size,
                  "Stage 9 result must use canonical CRLF CSV");
    return -1;
}

static int hwa_grf_field_canonical(const HWAGRCsvField *field)
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
    if (!quote)
        return field->raw_size == size &&
               memcmp(field->raw, text, size) == 0;
    if (field->raw_size < 2U || field->raw[raw++] != '"') return 0;
    for (source = 0U; source < size; ++source) {
        if (text[source] == '"' &&
            (raw >= field->raw_size ||
             field->raw[raw++] != '"')) return 0;
        if (raw >= field->raw_size ||
            field->raw[raw++] != text[source]) return 0;
    }
    return raw + 1U == field->raw_size && field->raw[raw] == '"';
}

static int hwa_grf_row_canonical(const HWAGRCsvRow *row)
{
    size_t index;
    for (index = 0U; index < row->count; ++index)
        if (!hwa_grf_field_canonical(&row->fields[index])) return 0;
    return 1;
}

static int hwa_grf_u64(const HWAGRCsvField *field, uint64_t *value)
{
    unsigned long long parsed;
    char canonical[32];
    char *end = NULL;
    if (field->quoted || field->text[0] == '\0' ||
        field->text[0] == '+' || field->text[0] == '-') return -1;
    errno = 0;
    parsed = strtoull(field->text, &end, 10);
    if (errno == ERANGE || end == field->text || *end != '\0') return -1;
    *value = (uint64_t)parsed;
    (void)snprintf(canonical, sizeof(canonical), "%" PRIu64, *value);
    return strcmp(canonical, field->text) == 0 ? 0 : -1;
}

static int hwa_grf_size(const HWAGRCsvField *field, size_t *value)
{
    uint64_t parsed;
    if (hwa_grf_u64(field, &parsed) != 0 || parsed > SIZE_MAX) return -1;
    *value = (size_t)parsed;
    return 0;
}

static int hwa_grf_u32(const HWAGRCsvField *field, uint32_t *value)
{
    uint64_t parsed;
    if (hwa_grf_u64(field, &parsed) != 0 ||
        parsed > UINT32_MAX) return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static int hwa_grf_bool(const HWAGRCsvField *field, int *value)
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

static int hwa_grf_parse_double(const HWANumericLocale *locale,
                                const HWAGRCsvField *field,
                                double *value)
{
    char canonical[64];
    if (field->quoted || field->text[0] == '\0' ||
        hwa_c_locale_parse_double(locale, field->text, value) != 0 ||
        !isfinite(*value) ||
        hwa_c_locale_format_double(
            locale, canonical, sizeof(canonical),
            *value == 0.0 ? 0.0 : *value) != 0 ||
        strcmp(canonical, field->text) != 0) return -1;
    if (*value == 0.0) *value = 0.0;
    return 0;
}

static int hwa_grf_optional_double(
    const HWANumericLocale *locale,
    const HWAGRCsvField *field,
    int valid,
    double *value)
{
    if (!valid) {
        if (field->quoted || field->text[0] != '\0') return -1;
        *value = 0.0;
        return 0;
    }
    return hwa_grf_parse_double(locale, field, value);
}

static int hwa_grf_sha(const HWAGRCsvField *field,
                       char value[HWA_SHA256_HEX_SIZE])
{
    size_t index;
    if (field->quoted || strlen(field->text) != 64U) return -1;
    for (index = 0U; index < 64U; ++index) {
        char c = field->text[index];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f'))) return -1;
    }
    memcpy(value, field->text, HWA_SHA256_HEX_SIZE);
    return 0;
}

static char *hwa_grf_take(HWAGRCsvField *field)
{
    char *text = field->text;
    field->text = NULL;
    return text;
}

static int hwa_grf_next_type(HWAGRCsvReader *reader,
                             const char *type,
                             size_t count,
                             HWAGRCsvRow *row,
                             char *error,
                             size_t error_size)
{
    if (hwa_grf_csv_next(reader, row, error, error_size) != 1 ||
        row->count != count || !hwa_grf_row_canonical(row) ||
        row->fields[0].quoted ||
        strcmp(row->fields[0].text, type) != 0) {
        hwa_grf_row_free(row);
        hwa_grf_error(error, error_size,
                      "Stage 9 result has an invalid row or section order");
        return -1;
    }
    return 0;
}

static int hwa_grf_meta_row(HWAGRCsvReader *reader,
                            const char *key,
                            const char *unit,
                            HWAGRCsvRow *row,
                            char *error,
                            size_t error_size)
{
    if (hwa_grf_next_type(reader, "META", 4U, row,
                          error, error_size) != 0 ||
        row->fields[1].quoted ||
        strcmp(row->fields[1].text, key) != 0 ||
        strcmp(row->fields[3].text, unit) != 0) {
        hwa_grf_row_free(row);
        hwa_grf_error(error, error_size,
                      "Stage 9 result has invalid metadata");
        return -1;
    }
    return 0;
}

static int hwa_grf_meta_text_read(HWAGRCsvReader *reader,
                                  const char *key,
                                  const char *unit,
                                  char **value,
                                  char *error,
                                  size_t error_size)
{
    HWAGRCsvRow row;
    if (hwa_grf_meta_row(
            reader, key, unit, &row, error, error_size) != 0) return -1;
    *value = hwa_grf_take(&row.fields[2]);
    hwa_grf_row_free(&row);
    return 0;
}

static int hwa_grf_meta_u64_read(HWAGRCsvReader *reader,
                                 const char *key,
                                 const char *unit,
                                 uint64_t *value,
                                 char *error,
                                 size_t error_size)
{
    HWAGRCsvRow row;
    if (hwa_grf_meta_row(
            reader, key, unit, &row, error, error_size) != 0) return -1;
    if (hwa_grf_u64(&row.fields[2], value) != 0) {
        hwa_grf_row_free(&row);
        hwa_grf_error(error, error_size,
                      "Stage 9 result has invalid numeric metadata");
        return -1;
    }
    hwa_grf_row_free(&row);
    return 0;
}

static int hwa_grf_meta_size_read(HWAGRCsvReader *reader,
                                  const char *key,
                                  const char *unit,
                                  size_t *value,
                                  char *error,
                                  size_t error_size)
{
    uint64_t parsed;
    if (hwa_grf_meta_u64_read(
            reader, key, unit, &parsed, error, error_size) != 0 ||
        parsed > SIZE_MAX) return -1;
    *value = (size_t)parsed;
    return 0;
}

static int hwa_grf_meta_sha_read(
    HWAGRCsvReader *reader,
    const char *key,
    char value[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    HWAGRCsvRow row;
    if (hwa_grf_meta_row(
            reader, key, "", &row, error, error_size) != 0) return -1;
    if (hwa_grf_sha(&row.fields[2], value) != 0) {
        hwa_grf_row_free(&row);
        hwa_grf_error(error, error_size,
                      "Stage 9 result has invalid SHA-256 metadata");
        return -1;
    }
    hwa_grf_row_free(&row);
    return 0;
}

static int hwa_grf_read_meta(HWAGRCsvReader *reader,
                             HWAGRSavedMeta *meta,
                             char *error,
                             size_t error_size)
{
    char *text = NULL;
    char *method = NULL;
    char *version = NULL;
    hwa_gap_report_options_default(&meta->options);
#define U(k, f, u)                                                         \
    do { if (hwa_grf_meta_u64_read(reader, (k), (u), &(f),                 \
                                   error, error_size) != 0) goto fail; } while (0)
#define Z(k, f, u)                                                         \
    do { if (hwa_grf_meta_size_read(reader, (k), (u), &(f),                \
                                    error, error_size) != 0) goto fail; } while (0)
    /* Tool releases are provenance. Saved-form and method versions decide
     * whether this reader can rebuild the report. */
    if (hwa_grf_meta_text_read(
            reader, "tool_version", "", &version,
            error, error_size) != 0 ||
        version[0] == '\0' ||
        hwa_grf_meta_text_read(
            reader, "report_method_version", "", &method,
            error, error_size) != 0 ||
        strcmp(method, HWA_GAP_REPORT_METHOD_VERSION) != 0 ||
        hwa_grf_meta_text_read(
            reader, "audibility_method", "", &meta->audibility_method,
            error, error_size) != 0 ||
        strcmp(meta->audibility_method,
               HWA_GAP_REPORT_AUDIBILITY_METHOD) != 0 ||
        hwa_grf_meta_text_read(
            reader, "mode", "", &text, error, error_size) != 0 ||
        hwa_gap_report_mode_from_name(text, &meta->mode) != 0) goto fail;
    free(text);
    text = NULL;
    if (hwa_grf_meta_text_read(
            reader, "title", "", &meta->title,
            error, error_size) != 0 ||
        hwa_grf_meta_sha_read(
            reader, "manifest_sha256", meta->manifest_sha256,
            error, error_size) != 0) goto fail;
    Z("decode_block_frames", meta->options.decode_block_frames, "frames");
    U("max_manifest_bytes", meta->options.max_manifest_bytes, "bytes");
    U("max_input_bytes", meta->options.max_input_bytes, "bytes");
    U("max_input_frames", meta->options.max_input_frames, "frames");
    U("max_work_bytes", meta->options.max_work_bytes, "bytes");
    U("max_evaluations", meta->options.max_evaluations, "evaluations");
    U("max_output_file_bytes",
      meta->options.max_output_file_bytes, "bytes");
    U("max_bundle_bytes", meta->options.max_bundle_bytes, "bytes");
    U("max_excerpt_frames", meta->options.max_excerpt_frames, "frames");
    U("max_total_excerpt_frames",
      meta->options.max_total_excerpt_frames, "frames");
    Z("max_sources", meta->options.max_sources, "sources");
    Z("max_labels", meta->options.max_labels, "labels");
    Z("max_candidates", meta->options.max_candidates, "candidates");
    Z("max_families", meta->options.max_families, "families");
    Z("max_groups", meta->options.max_groups, "groups");
    Z("max_cases", meta->options.max_cases, "cases");
    Z("max_excerpts", meta->options.max_excerpts, "excerpts");
    Z("max_warnings", meta->options.max_warnings, "warnings");
    Z("max_json_depth", meta->options.max_json_depth, "levels");
    Z("max_json_tokens", meta->options.max_json_tokens, "tokens");
#define PM(p, prefix)                                                       \
    U(prefix "_max_input_bytes", (p).max_input_bytes, "bytes");           \
    U(prefix "_max_work_bytes", (p).max_work_bytes, "bytes");             \
    Z(prefix "_max_contexts", (p).max_contexts, "contexts");              \
    Z(prefix "_max_measurements", (p).max_measurements, "measurements");  \
    Z(prefix "_max_groups", (p).max_groups, "groups");                    \
    Z(prefix "_max_group_members", (p).max_group_members, "members");     \
    Z(prefix "_max_statistics", (p).max_statistics, "statistics");       \
    Z(prefix "_max_warnings", (p).max_warnings, "warnings");             \
    Z(prefix "_max_distributions", (p).max_distributions, "distributions"); \
    Z(prefix "_max_gaps", (p).max_gaps, "gaps")
#define RU(p, prefix)                                                       \
    Z(prefix "_decode_block_frames", (p).decode_block_frames, "frames");  \
    U(prefix "_max_manifest_bytes", (p).max_manifest_bytes, "bytes");     \
    U(prefix "_max_input_bytes", (p).max_input_bytes, "bytes");           \
    U(prefix "_max_input_frames", (p).max_input_frames, "frames");        \
    U(prefix "_max_probe_bytes", (p).max_probe_bytes, "bytes");           \
    U(prefix "_max_probe_values", (p).max_probe_values, "values");        \
    U(prefix "_max_work_bytes", (p).max_work_bytes, "bytes");             \
    U(prefix "_max_evaluations", (p).max_evaluations, "evaluations");     \
    Z(prefix "_max_stems", (p).max_stems, "stems");                       \
    Z(prefix "_max_probes", (p).max_probes, "probes");                    \
    Z(prefix "_max_links", (p).max_links, "links");                       \
    Z(prefix "_max_json_depth", (p).max_json_depth, "levels");            \
    Z(prefix "_max_json_tokens", (p).max_json_tokens, "tokens");          \
    Z(prefix "_max_result_rows", (p).max_result_rows, "rows");            \
    Z(prefix "_max_warnings", (p).max_warnings, "warnings")
    PM(meta->options.measurement, "measurement");
    Z("production_decode_block_frames",
      meta->options.production.decode_block_frames, "frames");
    U("production_max_input_bytes",
      meta->options.production.max_input_bytes, "bytes");
    U("production_max_input_frames",
      meta->options.production.max_input_frames, "frames");
    U("production_max_ir_frames",
      meta->options.production.max_ir_frames, "frames");
    U("production_max_work_bytes",
      meta->options.production.max_work_bytes, "bytes");
    U("production_max_evaluations",
      meta->options.production.max_evaluations, "evaluations");
    Z("production_max_spans", meta->options.production.max_spans, "spans");
    Z("production_max_envelope_points",
      meta->options.production.max_envelope_points, "points");
    Z("production_max_fits", meta->options.production.max_fits, "fits");
    Z("production_max_evaluation_rows",
      meta->options.production.max_evaluation_rows, "rows");
    Z("production_max_view_rows",
      meta->options.production.max_view_rows, "rows");
    Z("production_max_warnings",
      meta->options.production.max_warnings, "warnings");
    PM(meta->options.production.profile_limits, "production_profile");
    RU(meta->options.run, "run");
    RU(meta->options.experiment.run, "experiment_run");
    U("experiment_max_manifest_bytes",
      meta->options.experiment.max_manifest_bytes, "bytes");
    U("experiment_max_input_bytes",
      meta->options.experiment.max_input_bytes, "bytes");
    U("experiment_max_work_bytes",
      meta->options.experiment.max_work_bytes, "bytes");
    U("experiment_max_bundle_bytes",
      meta->options.experiment.max_bundle_bytes, "bytes");
    U("experiment_max_output_file_bytes",
      meta->options.experiment.max_output_file_bytes, "bytes");
    U("experiment_max_total_run_evaluations",
      meta->options.experiment.max_total_run_evaluations, "evaluations");
    U("experiment_max_job_milliseconds",
      meta->options.experiment.max_job_milliseconds, "milliseconds");
    U("experiment_max_total_milliseconds",
      meta->options.experiment.max_total_milliseconds, "milliseconds");
    Z("experiment_max_parameters",
      meta->options.experiment.max_parameters, "parameters");
    Z("experiment_max_levels", meta->options.experiment.max_levels, "levels");
    Z("experiment_max_cases", meta->options.experiment.max_cases, "cases");
    Z("experiment_max_responses",
      meta->options.experiment.max_responses, "responses");
    Z("experiment_max_points", meta->options.experiment.max_points, "points");
    Z("experiment_max_jobs", meta->options.experiment.max_jobs, "jobs");
    Z("experiment_max_replicates",
      meta->options.experiment.max_replicates, "replicates");
    Z("experiment_max_artifacts",
      meta->options.experiment.max_artifacts, "artifacts");
    Z("experiment_max_observations",
      meta->options.experiment.max_observations, "observations");
    Z("experiment_max_sensitivities",
      meta->options.experiment.max_sensitivities, "sensitivities");
    Z("experiment_max_interactions",
      meta->options.experiment.max_interactions, "interactions");
    Z("experiment_max_warnings",
      meta->options.experiment.max_warnings, "warnings");
#undef PM
#undef RU
    U("retained_work_bytes", meta->retained_work_bytes, "bytes");
    U("total_input_bytes", meta->total_input_bytes, "bytes");
    U("total_output_bytes", meta->total_output_bytes, "bytes");
    U("evaluation_count", meta->evaluation_count, "evaluations");
    Z("source_count", meta->source_count, "sources");
    Z("label_count", meta->label_count, "labels");
    Z("candidate_count", meta->candidate_count, "candidates");
    Z("family_count", meta->family_count, "families");
    Z("group_count", meta->group_count, "groups");
    Z("case_count", meta->case_count, "cases");
    Z("excerpt_count", meta->excerpt_count, "excerpts");
    Z("warning_count", meta->warning_count, "warnings");
#undef U
#undef Z
    free(version);
    free(method);
    return 0;
fail:
#undef U
#undef Z
    free(text);
    free(version);
    free(method);
    return -1;
}

static int hwa_grf_counts_fit(
    const HWAGRSavedMeta *meta,
    const HWAGapReportOptions *limits)
{
    return meta->source_count <= limits->max_sources &&
           meta->label_count <= limits->max_labels &&
           meta->candidate_count <= limits->max_candidates &&
           meta->family_count <= limits->max_families &&
           meta->group_count <= limits->max_groups &&
           meta->case_count <= limits->max_cases &&
           meta->excerpt_count <= limits->max_excerpts &&
           meta->warning_count <= limits->max_warnings &&
           meta->evaluation_count <= limits->max_evaluations &&
           meta->total_output_bytes <= limits->max_bundle_bytes;
}

static int hwa_grf_report_facts_fit(const HWAGRSavedMeta *meta,
                                    const HWAGapReportOptions *limits,
                                    size_t report_bytes)
{
    const HWAGapReportOptions *saved = &meta->options;
    uint64_t bytes = (uint64_t)report_bytes;
    uint64_t copies = meta->mode == HWA_GAP_REPORT_FULL
        ? UINT64_C(2) : UINT64_C(1);
    if (!hwa_grf_counts_fit(meta, limits) ||
        !hwa_grf_counts_fit(meta, saved) ||
        bytes > limits->max_input_bytes ||
        bytes > limits->max_output_file_bytes ||
        bytes > saved->max_output_file_bytes ||
        meta->retained_work_bytes > saved->max_work_bytes ||
        meta->total_output_bytes > limits->max_bundle_bytes ||
        bytes > (limits->max_bundle_bytes - meta->total_output_bytes) /
                    copies ||
        meta->total_output_bytes > saved->max_bundle_bytes ||
        bytes > (saved->max_bundle_bytes - meta->total_output_bytes) /
                    copies) {
        return 0;
    }
    return 1;
}

static int hwa_grf_allocate(HWAGapReportResult *result,
                            const HWAGRSavedMeta *meta)
{
#define A(field, count, type)                                              \
    do {                                                                   \
        result->count = meta->count;                                       \
        if (result->count != 0U) {                                         \
            if (result->count > SIZE_MAX / sizeof(type)) return -1;        \
            result->field = (type *)calloc(result->count, sizeof(type));   \
            if (result->field == NULL) return -1;                          \
        }                                                                  \
    } while (0)
    A(sources, source_count, HWAGapReportSource);
    A(labels, label_count, HWAGapReportLabel);
    A(candidates, candidate_count, HWAGapReportCandidate);
    A(families, family_count, HWAGapReportFamily);
    A(groups, group_count, HWAGapReportGroup);
    A(cases, case_count, HWAGapReportCase);
    A(excerpts, excerpt_count, HWAGapReportExcerpt);
    A(warnings, warning_count, HWAGapReportWarning);
#undef A
    return 0;
}

static char *hwa_grf_dot(void)
{
    char *text = (char *)malloc(2U);
    if (text != NULL) {
        text[0] = '.';
        text[1] = '\0';
    }
    return text;
}

static char *hwa_grf_empty(void)
{
    char *text = (char *)malloc(1U);
    if (text != NULL) text[0] = '\0';
    return text;
}

static int hwa_grf_parse_source(HWAGRCsvReader *reader,
                                HWAGapReportSource *r,
                                uint64_t expected_id,
                                char *error,
                                size_t error_size)
{
    HWAGRCsvRow row;
    if (hwa_grf_next_type(
            reader, "SOURCE", 7U, &row, error, error_size) != 0) return -1;
    if (hwa_grf_u64(&row.fields[1], &r->id) != 0 ||
        r->id != expected_id ||
        hwa_gap_report_source_kind_from_name(
            row.fields[3].text, &r->kind) != 0 ||
        hwa_grf_sha(&row.fields[4], r->sha256) != 0 ||
        hwa_grf_u64(&row.fields[5], &r->file_bytes) != 0 ||
        hwa_grf_size(&row.fields[6], &r->candidate_count) != 0)
        goto invalid;
    r->name = hwa_grf_take(&row.fields[2]);
    r->path = hwa_grf_dot();
    if (r->path == NULL) goto invalid;
    hwa_grf_row_free(&row);
    return 0;
invalid:
    hwa_grf_row_free(&row);
    hwa_grf_error(error, error_size, "invalid Stage 9 SOURCE row");
    return -1;
}

static int hwa_grf_parse_label(HWAGRCsvReader *reader,
                               HWAGapReportLabel *r,
                               uint64_t expected_id,
                               char *error,
                               size_t error_size)
{
    HWAGRCsvRow row;
    if (hwa_grf_next_type(
            reader, "LABEL", 10U, &row, error, error_size) != 0) return -1;
    if (hwa_grf_u64(&row.fields[1], &r->id) != 0 ||
        r->id != expected_id ||
        hwa_grf_u64(&row.fields[2], &r->source_id) != 0) goto invalid;
    r->case_id = hwa_grf_take(&row.fields[3]);
    r->pitch = hwa_grf_take(&row.fields[4]);
    r->register_name = hwa_grf_take(&row.fields[5]);
    r->dynamic = hwa_grf_take(&row.fields[6]);
    r->gesture = hwa_grf_take(&row.fields[7]);
    r->physical_element = hwa_grf_take(&row.fields[8]);
    r->section = hwa_grf_take(&row.fields[9]);
    hwa_grf_row_free(&row);
    return 0;
invalid:
    hwa_grf_row_free(&row);
    hwa_grf_error(error, error_size, "invalid Stage 9 LABEL row");
    return -1;
}

static int hwa_grf_parse_candidate(const HWANumericLocale *locale,
                                   HWAGRCsvReader *reader,
                                   HWAGapReportCandidate *r,
                                   uint64_t expected_id,
                                   char *error,
                                   size_t error_size)
{
    HWAGRCsvRow row;
    if (hwa_grf_next_type(
            reader, "CANDIDATE", 24U, &row, error, error_size) != 0) return -1;
    if (hwa_grf_bool(&row.fields[17], &r->raw_value_valid) != 0 ||
        hwa_grf_bool(&row.fields[18], &r->size_valid) != 0 ||
        hwa_grf_bool(&row.fields[19], &r->audibility_valid) != 0 ||
        hwa_grf_bool(&row.fields[20], &r->confidence_valid) != 0 ||
        hwa_grf_bool(&row.fields[21], &r->score_valid) != 0 ||
        hwa_grf_bool(&row.fields[22], &r->primary) != 0 ||
        hwa_grf_u64(&row.fields[1], &r->id) != 0 ||
        r->id != expected_id ||
        hwa_grf_u64(&row.fields[2], &r->source_id) != 0 ||
        hwa_grf_u64(&row.fields[3], &r->source_row) != 0 ||
        hwa_gap_report_candidate_kind_from_name(
            row.fields[7].text, &r->kind) != 0 ||
        hwa_gap_report_availability_from_name(
            row.fields[8].text, &r->availability) != 0 ||
        hwa_grf_optional_double(
            locale, &row.fields[9], r->raw_value_valid,
            &r->raw_value) != 0 ||
        hwa_grf_optional_double(
            locale, &row.fields[10], r->size_valid,
            &r->size_factor) != 0 ||
        hwa_grf_optional_double(
            locale, &row.fields[11], r->audibility_valid,
            &r->audibility_factor) != 0 ||
        hwa_grf_optional_double(
            locale, &row.fields[12], r->confidence_valid,
            &r->confidence_factor) != 0 ||
        hwa_grf_optional_double(
            locale, &row.fields[13], r->score_valid, &r->score) != 0 ||
        hwa_grf_u64(&row.fields[14], &r->linked_family_id) != 0 ||
        hwa_grf_size(&row.fields[15], &r->rank) != 0 ||
        hwa_grf_u32(&row.fields[16], &r->quality_flags) != 0) goto invalid;
    r->case_id = hwa_grf_take(&row.fields[4]);
    r->metric = hwa_grf_take(&row.fields[5]);
    r->family_key = hwa_grf_take(&row.fields[6]);
    r->reason = hwa_grf_take(&row.fields[23]);
    hwa_grf_row_free(&row);
    return 0;
invalid:
    hwa_grf_row_free(&row);
    hwa_grf_error(error, error_size, "invalid Stage 9 CANDIDATE row");
    return -1;
}

static int hwa_grf_parse_occurrence(
    const HWANumericLocale *locale,
    HWAGRCsvReader *reader,
    HWAGapReportCandidate *r,
    char *error,
    size_t error_size)
{
    HWAGRCsvRow row;
    uint64_t candidate_id;
    if (hwa_grf_next_type(
            reader, "OCCURRENCE", 6U, &row, error, error_size) != 0)
        return -1;
    if (hwa_grf_u64(&row.fields[1], &candidate_id) != 0 ||
        candidate_id != r->id ||
        hwa_grf_u64(&row.fields[2], &r->occurrence_count) != 0 ||
        hwa_grf_u64(&row.fields[3], &r->eligible_count) != 0 ||
        hwa_grf_bool(&row.fields[5], &r->occurrence_valid) != 0 ||
        hwa_grf_optional_double(
            locale, &row.fields[4], r->occurrence_valid,
            &r->occurrence_factor) != 0) goto invalid;
    hwa_grf_row_free(&row);
    return 0;
invalid:
    hwa_grf_row_free(&row);
    hwa_grf_error(error, error_size, "invalid Stage 9 OCCURRENCE row");
    return -1;
}

static int hwa_grf_parse_family(HWAGRCsvReader *reader,
                                HWAGapReportFamily *r,
                                uint64_t expected_id,
                                char *error,
                                size_t error_size)
{
    HWAGRCsvRow row;
    if (hwa_grf_next_type(
            reader, "FAMILY", 6U, &row, error, error_size) != 0) return -1;
    if (hwa_grf_u64(&row.fields[1], &r->id) != 0 ||
        r->id != expected_id ||
        hwa_grf_u64(&row.fields[3], &r->primary_candidate_id) != 0 ||
        hwa_grf_size(&row.fields[4], &r->member_count) != 0 ||
        hwa_grf_size(&row.fields[5], &r->rank) != 0) goto invalid;
    r->key = hwa_grf_take(&row.fields[2]);
    hwa_grf_row_free(&row);
    return 0;
invalid:
    hwa_grf_row_free(&row);
    hwa_grf_error(error, error_size, "invalid Stage 9 FAMILY row");
    return -1;
}

static int hwa_grf_parse_group(const HWANumericLocale *locale,
                               HWAGRCsvReader *reader,
                               HWAGapReportGroup *r,
                               uint64_t expected_id,
                               char *error,
                               size_t error_size)
{
    HWAGRCsvRow row;
    if (hwa_grf_next_type(
            reader, "GROUP", 18U, &row, error, error_size) != 0) return -1;
    if (hwa_grf_bool(&row.fields[16], &r->statistics_valid) != 0 ||
        hwa_grf_bool(&row.fields[17], &r->confidence_valid) != 0 ||
        hwa_grf_u64(&row.fields[1], &r->id) != 0 ||
        r->id != expected_id ||
        hwa_gap_report_axis_from_name(row.fields[2].text, &r->axis) != 0 ||
        hwa_grf_size(&row.fields[4], &r->family_count) != 0 ||
        hwa_grf_size(&row.fields[5], &r->candidate_count) != 0 ||
        hwa_grf_size(&row.fields[6], &r->available_count) != 0 ||
        hwa_grf_size(&row.fields[7], &r->missing_count) != 0 ||
        hwa_grf_size(&row.fields[8], &r->excluded_count) != 0 ||
        hwa_grf_optional_double(
            locale, &row.fields[9], r->statistics_valid, &r->q05) != 0 ||
        hwa_grf_optional_double(
            locale, &row.fields[10], r->statistics_valid, &r->q25) != 0 ||
        hwa_grf_optional_double(
            locale, &row.fields[11], r->statistics_valid,
            &r->median) != 0 ||
        hwa_grf_optional_double(
            locale, &row.fields[12], r->statistics_valid, &r->q75) != 0 ||
        hwa_grf_optional_double(
            locale, &row.fields[13], r->statistics_valid, &r->q95) != 0 ||
        hwa_grf_optional_double(
            locale, &row.fields[14], r->statistics_valid,
            &r->spread) != 0 ||
        hwa_grf_optional_double(
            locale, &row.fields[15], r->confidence_valid,
            &r->confidence) != 0) goto invalid;
    r->value = hwa_grf_take(&row.fields[3]);
    hwa_grf_row_free(&row);
    return 0;
invalid:
    hwa_grf_row_free(&row);
    hwa_grf_error(error, error_size, "invalid Stage 9 GROUP row");
    return -1;
}

static int hwa_grf_parse_case(const HWANumericLocale *locale,
                              HWAGRCsvReader *reader,
                              HWAGapReportCase *r,
                              uint64_t expected_id,
                              char *error,
                              size_t error_size)
{
    HWAGRCsvRow row;
    if (hwa_grf_next_type(
            reader, "WORST", 12U, &row, error, error_size) != 0) return -1;
    if (hwa_grf_bool(&row.fields[8], &r->value_valid) != 0 ||
        hwa_grf_bool(&row.fields[9], &r->confidence_valid) != 0 ||
        hwa_grf_bool(&row.fields[10], &r->score_valid) != 0 ||
        hwa_grf_u64(&row.fields[1], &r->id) != 0 ||
        r->id != expected_id ||
        hwa_grf_u64(&row.fields[2], &r->candidate_id) != 0 ||
        hwa_gap_report_availability_from_name(
            row.fields[4].text, &r->availability) != 0 ||
        hwa_grf_optional_double(
            locale, &row.fields[5], r->value_valid, &r->value) != 0 ||
        hwa_grf_optional_double(
            locale, &row.fields[6], r->confidence_valid,
            &r->confidence) != 0 ||
        hwa_grf_optional_double(
            locale, &row.fields[7], r->score_valid, &r->score) != 0)
        goto invalid;
    r->case_id = hwa_grf_take(&row.fields[3]);
    r->reason = hwa_grf_take(&row.fields[11]);
    hwa_grf_row_free(&row);
    return 0;
invalid:
    hwa_grf_row_free(&row);
    hwa_grf_error(error, error_size, "invalid Stage 9 WORST row");
    return -1;
}

static int hwa_grf_parse_clip(const HWANumericLocale *locale,
                              HWAGRCsvReader *reader,
                              HWAGapReportExcerpt *r,
                              uint64_t expected_id,
                              char *error,
                              size_t error_size)
{
    HWAGRCsvRow row;
    if (hwa_grf_next_type(
            reader, "CLIP", 17U, &row, error, error_size) != 0) return -1;
    if (hwa_grf_u64(&row.fields[1], &r->id) != 0 ||
        r->id != expected_id ||
        hwa_grf_u64(&row.fields[3], &r->candidate_source_id) != 0 ||
        hwa_grf_u64(&row.fields[4], &r->candidate_row) != 0 ||
        hwa_gap_report_view_from_name(row.fields[5].text, &r->view) != 0 ||
        hwa_grf_u64(&row.fields[6], &r->reference_source_id) != 0 ||
        hwa_grf_u64(&row.fields[7], &r->model_source_id) != 0 ||
        hwa_grf_u64(&row.fields[8], &r->reference_start_sample) != 0 ||
        hwa_grf_u64(&row.fields[9], &r->model_start_sample) != 0 ||
        hwa_grf_u64(&row.fields[10], &r->frame_count) != 0 ||
        hwa_grf_bool(&row.fields[11], &r->make_x) != 0 ||
        hwa_gap_report_availability_from_name(
            row.fields[12].text, &r->availability) != 0 ||
        hwa_grf_parse_double(
            locale, &row.fields[13], &r->reference_gain_db) != 0 ||
        hwa_grf_parse_double(
            locale, &row.fields[14], &r->model_gain_db) != 0 ||
        hwa_grf_bool(&row.fields[15], &r->x_is_reference) != 0) goto invalid;
    r->name = hwa_grf_take(&row.fields[2]);
    r->reason = hwa_grf_take(&row.fields[16]);
    r->reference_path = hwa_grf_empty();
    r->model_path = hwa_grf_empty();
    r->x_path = hwa_grf_empty();
    if (r->reference_path == NULL || r->model_path == NULL ||
        r->x_path == NULL) goto invalid;
    hwa_grf_row_free(&row);
    return 0;
invalid:
    hwa_grf_row_free(&row);
    hwa_grf_error(error, error_size, "invalid Stage 9 CLIP row");
    return -1;
}

static int hwa_grf_parse_file(HWAGRCsvReader *reader,
                              HWAGapReportExcerpt *r,
                              const char *role,
                              char **path,
                              char sha[HWA_SHA256_HEX_SIZE],
                              uint64_t *bytes,
                              char *error,
                              size_t error_size)
{
    HWAGRCsvRow row;
    uint64_t excerpt_id;
    if (hwa_grf_next_type(
            reader, "FILE", 6U, &row, error, error_size) != 0) return -1;
    if (hwa_grf_u64(&row.fields[1], &excerpt_id) != 0 ||
        excerpt_id != r->id || strcmp(row.fields[2].text, role) != 0 ||
        hwa_grf_sha(&row.fields[4], sha) != 0 ||
        hwa_grf_u64(&row.fields[5], bytes) != 0) goto invalid;
    free(*path);
    *path = hwa_grf_take(&row.fields[3]);
    hwa_grf_row_free(&row);
    return 0;
invalid:
    hwa_grf_row_free(&row);
    hwa_grf_error(error, error_size, "invalid Stage 9 FILE row");
    return -1;
}

static int hwa_grf_parse_exclusion(HWAGRCsvReader *reader,
                                   const HWAGapReportCandidate *r,
                                   char *error,
                                   size_t error_size)
{
    HWAGRCsvRow row;
    uint64_t id;
    HWAGapReportAvailability availability;
    if (hwa_grf_next_type(
            reader, "EXCLUSION", 4U, &row, error, error_size) != 0)
        return -1;
    if (hwa_grf_u64(&row.fields[1], &id) != 0 || id != r->id ||
        hwa_gap_report_availability_from_name(
            row.fields[2].text, &availability) != 0 ||
        availability != r->availability ||
        strcmp(row.fields[3].text, r->reason) != 0) goto invalid;
    hwa_grf_row_free(&row);
    return 0;
invalid:
    hwa_grf_row_free(&row);
    hwa_grf_error(error, error_size, "invalid Stage 9 EXCLUSION row");
    return -1;
}

static int hwa_grf_parse_failure(HWAGRCsvReader *reader,
                                 const HWAGapReportExcerpt *r,
                                 char *error,
                                 size_t error_size)
{
    HWAGRCsvRow row;
    uint64_t id;
    HWAGapReportAvailability availability;
    if (hwa_grf_next_type(
            reader, "FAILURE", 4U, &row, error, error_size) != 0)
        return -1;
    if (hwa_grf_u64(&row.fields[1], &id) != 0 || id != r->id ||
        hwa_gap_report_availability_from_name(
            row.fields[2].text, &availability) != 0 ||
        availability != r->availability ||
        strcmp(row.fields[3].text, r->reason) != 0) goto invalid;
    hwa_grf_row_free(&row);
    return 0;
invalid:
    hwa_grf_row_free(&row);
    hwa_grf_error(error, error_size, "invalid Stage 9 FAILURE row");
    return -1;
}

static int hwa_grf_parse_warning(HWAGRCsvReader *reader,
                                 HWAGapReportWarning *r,
                                 uint64_t expected_id,
                                 char *error,
                                 size_t error_size)
{
    HWAGRCsvRow row;
    if (hwa_grf_next_type(
            reader, "WARNING", 10U, &row, error, error_size) != 0) return -1;
    if (hwa_grf_u64(&row.fields[1], &r->id) != 0 ||
        r->id != expected_id ||
        hwa_grf_u64(&row.fields[4], &r->source_id) != 0 ||
        hwa_grf_u64(&row.fields[5], &r->candidate_id) != 0 ||
        hwa_grf_u64(&row.fields[6], &r->excerpt_id) != 0 ||
        hwa_grf_bool(&row.fields[7], &r->source_id_valid) != 0 ||
        hwa_grf_bool(&row.fields[8], &r->candidate_id_valid) != 0 ||
        hwa_grf_bool(&row.fields[9], &r->excerpt_id_valid) != 0) goto invalid;
    r->code = hwa_grf_take(&row.fields[2]);
    r->message = hwa_grf_take(&row.fields[3]);
    hwa_grf_row_free(&row);
    return 0;
invalid:
    hwa_grf_row_free(&row);
    hwa_grf_error(error, error_size, "invalid Stage 9 WARNING row");
    return -1;
}

static uint64_t hwa_grf_abs_bits(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits & UINT64_C(0x7fffffffffffffff);
}

static int hwa_grf_derived_match(double saved, double expected)
{
    uint64_t a;
    uint64_t b;
    uint64_t distance;
    if (!isfinite(saved) || !isfinite(expected)) return 0;
    if (saved == 0.0 && expected == 0.0) return 1;
    if (signbit(saved) != signbit(expected)) return 0;
    a = hwa_grf_abs_bits(saved);
    b = hwa_grf_abs_bits(expected);
    distance = a > b ? a - b : b - a;
    return distance <= HWA_GR_READER_MAX_ULPS;
}

static int hwa_grf_candidate_derived_match(
    const HWAGapReportCandidate *saved,
    const HWAGapReportCandidate *rebuilt)
{
    return saved->id == rebuilt->id &&
           saved->quality_flags == rebuilt->quality_flags &&
           saved->linked_family_id == rebuilt->linked_family_id &&
           saved->rank == rebuilt->rank &&
           saved->size_valid == rebuilt->size_valid &&
           saved->audibility_valid == rebuilt->audibility_valid &&
           saved->occurrence_valid == rebuilt->occurrence_valid &&
           saved->confidence_valid == rebuilt->confidence_valid &&
           saved->score_valid == rebuilt->score_valid &&
           saved->primary == rebuilt->primary &&
           (!saved->size_valid ||
            hwa_grf_derived_match(saved->size_factor,
                                  rebuilt->size_factor)) &&
           (!saved->audibility_valid ||
            hwa_grf_derived_match(saved->audibility_factor,
                                  rebuilt->audibility_factor)) &&
           (!saved->occurrence_valid ||
            hwa_grf_derived_match(saved->occurrence_factor,
                                  rebuilt->occurrence_factor)) &&
           (!saved->confidence_valid ||
            hwa_grf_derived_match(saved->confidence_factor,
                                  rebuilt->confidence_factor)) &&
           (!saved->score_valid ||
            hwa_grf_derived_match(saved->score, rebuilt->score));
}

static int hwa_grf_family_derived_match(
    const HWAGapReportFamily *saved,
    const HWAGapReportFamily *rebuilt)
{
    return saved->id == rebuilt->id &&
           saved->key != NULL && rebuilt->key != NULL &&
           strcmp(saved->key, rebuilt->key) == 0 &&
           saved->primary_candidate_id == rebuilt->primary_candidate_id &&
           saved->member_count == rebuilt->member_count &&
           saved->rank == rebuilt->rank;
}

static int hwa_grf_group_derived_match(
    const HWAGapReportGroup *saved,
    const HWAGapReportGroup *rebuilt)
{
    return saved->id == rebuilt->id &&
           saved->axis == rebuilt->axis &&
           saved->value != NULL && rebuilt->value != NULL &&
           strcmp(saved->value, rebuilt->value) == 0 &&
           saved->family_count == rebuilt->family_count &&
           saved->candidate_count == rebuilt->candidate_count &&
           saved->available_count == rebuilt->available_count &&
           saved->missing_count == rebuilt->missing_count &&
           saved->excluded_count == rebuilt->excluded_count &&
           saved->statistics_valid == rebuilt->statistics_valid &&
           saved->confidence_valid == rebuilt->confidence_valid &&
           (!saved->statistics_valid ||
            (hwa_grf_derived_match(saved->q05, rebuilt->q05) &&
             hwa_grf_derived_match(saved->q25, rebuilt->q25) &&
             hwa_grf_derived_match(saved->median, rebuilt->median) &&
             hwa_grf_derived_match(saved->q75, rebuilt->q75) &&
             hwa_grf_derived_match(saved->q95, rebuilt->q95) &&
             hwa_grf_derived_match(saved->spread, rebuilt->spread))) &&
           (!saved->confidence_valid ||
            hwa_grf_derived_match(saved->confidence,
                                  rebuilt->confidence));
}

static int hwa_grf_case_derived_match(
    const HWAGapReportCase *saved,
    const HWAGapReportCase *rebuilt)
{
    return saved->id == rebuilt->id &&
           saved->score_valid == rebuilt->score_valid &&
           (!saved->score_valid ||
            hwa_grf_derived_match(saved->score, rebuilt->score));
}

static int hwa_grf_warning_match(const HWAGapReportWarning *saved,
                                 const HWAGapReportWarning *rebuilt)
{
    return saved->id == rebuilt->id &&
           strcmp(saved->code, rebuilt->code) == 0 &&
           strcmp(saved->message, rebuilt->message) == 0 &&
           saved->source_id == rebuilt->source_id &&
           saved->candidate_id == rebuilt->candidate_id &&
           saved->excerpt_id == rebuilt->excerpt_id &&
           saved->source_id_valid == rebuilt->source_id_valid &&
           saved->candidate_id_valid == rebuilt->candidate_id_valid &&
           saved->excerpt_id_valid == rebuilt->excerpt_id_valid;
}

static int hwa_grf_normalize_derived(HWAGapReportResult *result,
                                     char *error,
                                     size_t error_size)
{
    HWAGapReportCandidate *candidates = result->candidates;
    HWAGapReportFamily *families = result->families;
    HWAGapReportGroup *groups = result->groups;
    HWAGapReportCase *cases = result->cases;
    HWAGapReportWarning *warnings = result->warnings;
    size_t candidate_count = result->candidate_count;
    size_t family_count = result->family_count;
    size_t group_count = result->group_count;
    size_t case_count = result->case_count;
    size_t warning_count = result->warning_count;
    uint64_t evaluation_count = result->evaluation_count;
    size_t index;
    int status = -1;
    result->candidates = NULL;
    result->candidate_count = 0U;
    result->families = NULL;
    result->family_count = 0U;
    result->groups = NULL;
    result->group_count = 0U;
    result->cases = NULL;
    result->case_count = 0U;
    result->warnings = NULL;
    result->warning_count = 0U;
    /*
     * Rebuild needs raw candidate and case rows, so restore those arrays while
     * keeping copies for comparison. Families and groups are wholly derived.
     */
    result->candidates = candidates;
    result->candidate_count = candidate_count;
    result->cases = cases;
    result->case_count = case_count;
    candidates = candidate_count == 0U ? NULL :
        (HWAGapReportCandidate *)malloc(
            candidate_count * sizeof(*candidates));
    cases = case_count == 0U ? NULL :
        (HWAGapReportCase *)malloc(case_count * sizeof(*cases));
    if ((candidate_count != 0U && candidates == NULL) ||
        (case_count != 0U && cases == NULL)) goto cleanup;
    if (candidate_count != 0U)
        memcpy(candidates, result->candidates,
               candidate_count * sizeof(*candidates));
    if (case_count != 0U)
        memcpy(cases, result->cases, case_count * sizeof(*cases));
    if (hwa_gap_report_result_rebuild(result, error, error_size) != 0 ||
        result->candidate_count != candidate_count ||
        result->family_count != family_count ||
        result->group_count != group_count ||
        result->case_count != case_count ||
        result->warning_count != warning_count ||
        evaluation_count != result->evaluation_count) goto cleanup;
    for (index = 0U; index < candidate_count; ++index)
        if (!hwa_grf_candidate_derived_match(
                &candidates[index], &result->candidates[index])) goto cleanup;
    for (index = 0U; index < family_count; ++index)
        if (!hwa_grf_family_derived_match(
                &families[index], &result->families[index])) goto cleanup;
    for (index = 0U; index < group_count; ++index)
        if (!hwa_grf_group_derived_match(
                &groups[index], &result->groups[index])) goto cleanup;
    for (index = 0U; index < case_count; ++index)
        if (!hwa_grf_case_derived_match(
                &cases[index], &result->cases[index])) goto cleanup;
    for (index = 0U; index < warning_count; ++index)
        if (!hwa_grf_warning_match(
                &warnings[index], &result->warnings[index])) goto cleanup;
    status = 0;
cleanup:
    free(candidates);
    free(cases);
    if (families != result->families) {
        for (index = 0U; index < family_count; ++index)
            free(families[index].key);
        free(families);
    }
    if (groups != result->groups) {
        for (index = 0U; index < group_count; ++index)
            free(groups[index].value);
        free(groups);
    }
    if (warnings != result->warnings) {
        for (index = 0U; index < warning_count; ++index) {
            free(warnings[index].code);
            free(warnings[index].message);
        }
        free(warnings);
    }
    if (status != 0 &&
        (error == NULL || error_size == 0U || error[0] == '\0')) {
        hwa_grf_error(error, error_size,
                      "Stage 9 result has invalid derived rows");
    }
    return status;
}

static int hwa_grf_read_impl(
    const char *path,
    const HWAGapReportOptions *limits,
    HWAGapReportResult *result,
    char file_sha256[HWA_SHA256_HEX_SIZE],
    const HWANumericLocale *locale,
    char *error,
    size_t error_size)
{
    HWAGRSavedMeta meta;
    HWAGRCsvReader reader;
    HWAGRCsvRow row;
    unsigned char *data = NULL;
    size_t data_size = 0U;
    size_t index;
    uint64_t retained;
    uint64_t canonical_retained;
    uint64_t derived_string_bytes = 0U;
    uint64_t source_bytes = 0U;
    uint64_t excerpt_frames = 0U;
    uint64_t output_bytes = 0U;
    uint64_t projected_audio_bytes = 0U;
    uint64_t reader_bytes;
    uint64_t peak;
    uint64_t tree_bytes;
    int status = -1;
    memset(&meta, 0, sizeof(meta));
    memset(&reader, 0, sizeof(reader));
    if (result == NULL) {
        hwa_grf_error(error, error_size, "Stage 9 result pointer is null");
        return -1;
    }
    memset(result, 0, sizeof(*result));
    if (file_sha256 != NULL) file_sha256[0] = '\0';
    if (limits == NULL || file_sha256 == NULL ||
        hwa_gap_report_options_validate(limits, error, error_size) != 0 ||
        locale == NULL || !locale->active) return -1;
    if (hwa_grf_read_bytes(path, limits, &data, &data_size, file_sha256,
                           error, error_size) != 0) return -1;
    reader.data = data;
    reader.size = data_size;
    if (hwa_grf_csv_next(&reader, &row, error, error_size) != 1 ||
        row.count != 2U || !hwa_grf_row_canonical(&row) ||
        strcmp(row.fields[0].text, "HWA_REPORT") != 0 ||
        strcmp(row.fields[1].text, "1") != 0) {
        hwa_grf_row_free(&row);
        hwa_grf_error(error, error_size,
                      "Stage 9 result has an invalid header");
        goto cleanup;
    }
    hwa_grf_row_free(&row);
    if (hwa_grf_read_meta(&reader, &meta, error, error_size) != 0 ||
        hwa_gap_report_options_validate(
            &meta.options, error, error_size) != 0 ||
        !hwa_grf_report_facts_fit(&meta, limits, data_size)) goto cleanup;
    peak = (uint64_t)data_size + UINT64_C(1);
    if (peak > limits->max_work_bytes) goto cleanup;
#define LEDGER(count, type)                                                \
    do {                                                                   \
        if ((count) > SIZE_MAX / sizeof(type) ||                           \
            (uint64_t)((count) * sizeof(type)) >                           \
                limits->max_work_bytes - peak) goto cleanup;               \
        peak += (uint64_t)((count) * sizeof(type));                         \
    } while (0)
    LEDGER(meta.source_count, HWAGapReportSource);
    LEDGER(meta.label_count, HWAGapReportLabel);
    LEDGER(meta.candidate_count, HWAGapReportCandidate);
    LEDGER(meta.family_count, HWAGapReportFamily);
    LEDGER(meta.group_count, HWAGapReportGroup);
    LEDGER(meta.case_count, HWAGapReportCase);
    LEDGER(meta.excerpt_count, HWAGapReportExcerpt);
    LEDGER(meta.warning_count, HWAGapReportWarning);
#undef LEDGER
    /*
     * Decoded strings cannot exceed the file bytes. Reserve that full sum
     * before any result array or row-field string allocation.
     */
    if ((uint64_t)data_size + 1U > limits->max_work_bytes - peak)
        goto cleanup;
    peak += (uint64_t)data_size + 1U;
    /* Loaded results always carry the active caller limits. */
    result->options = *limits;
    result->mode = meta.mode;
    result->manifest_path = hwa_grf_dot();
    result->output_directory =
        meta.mode == HWA_GAP_REPORT_RANK ? NULL : hwa_grf_dot();
    result->title = meta.title;
    meta.title = NULL;
    result->audibility_method = meta.audibility_method;
    meta.audibility_method = NULL;
    memcpy(result->manifest_sha256, meta.manifest_sha256,
           HWA_SHA256_HEX_SIZE);
    result->total_input_bytes = meta.total_input_bytes;
    result->total_output_bytes = meta.total_output_bytes;
    result->evaluation_count = meta.evaluation_count;
    if (result->manifest_path == NULL ||
        (result->mode != HWA_GAP_REPORT_RANK &&
         result->output_directory == NULL) ||
        hwa_grf_allocate(result, &meta) != 0) goto cleanup;
    for (index = 0U; index < result->source_count; ++index) {
        uint64_t current_source_limit;
        uint64_t saved_source_limit;
        if (hwa_grf_parse_source(
                &reader, &result->sources[index], (uint64_t)index + 1U,
                error, error_size) != 0) goto cleanup;
        current_source_limit = hwa_grf_source_input_limit(
            limits, result->sources[index].kind);
        saved_source_limit = hwa_grf_source_input_limit(
            &meta.options, result->sources[index].kind);
        if (current_source_limit == 0U || saved_source_limit == 0U ||
            result->sources[index].file_bytes > limits->max_input_bytes ||
            result->sources[index].file_bytes > meta.options.max_input_bytes ||
            result->sources[index].file_bytes > current_source_limit ||
            result->sources[index].file_bytes > saved_source_limit ||
            result->sources[index].file_bytes > UINT64_MAX - source_bytes)
            goto cleanup;
        source_bytes += result->sources[index].file_bytes;
    }
    if (source_bytes != meta.total_input_bytes) goto cleanup;
    for (index = 0U; index < result->label_count; ++index)
        if (hwa_grf_parse_label(
                &reader, &result->labels[index], (uint64_t)index + 1U,
                error, error_size) != 0) goto cleanup;
    for (index = 0U; index < result->candidate_count; ++index) {
        if (hwa_grf_parse_candidate(
                locale, &reader, &result->candidates[index],
                (uint64_t)index + 1U,
                error, error_size) != 0)
            goto cleanup;
    }
    if (!hwa_grf_producer_facts_fit(result, limits) ||
        !hwa_grf_producer_facts_fit(result, &meta.options))
        goto cleanup;
    for (index = 0U; index < result->candidate_count; ++index)
        if (hwa_grf_parse_occurrence(
                locale, &reader, &result->candidates[index],
                error, error_size) != 0) goto cleanup;
    for (index = 0U; index < result->family_count; ++index)
        if (hwa_grf_parse_family(
                &reader, &result->families[index], (uint64_t)index + 1U,
                error, error_size) != 0) goto cleanup;
    for (index = 0U; index < result->group_count; ++index)
        if (hwa_grf_parse_group(
                locale, &reader, &result->groups[index],
                (uint64_t)index + 1U,
                error, error_size) != 0) goto cleanup;
    for (index = 0U; index < result->case_count; ++index)
        if (hwa_grf_parse_case(
                locale, &reader, &result->cases[index],
                (uint64_t)index + 1U,
                error, error_size) != 0) goto cleanup;
    /* The case catalog is meaningful only after every WORST row is parsed. */
    if (!hwa_gap_report_case_catalog_fits(result, limits) ||
        !hwa_gap_report_case_catalog_fits(result, &meta.options))
        goto cleanup;
    for (index = 0U; index < result->excerpt_count; ++index)
        if (hwa_grf_parse_clip(
                locale, &reader, &result->excerpts[index],
                (uint64_t)index + 1U,
                error, error_size) != 0) goto cleanup;
    if (!hwa_grf_producer_reader_work_fits(result, limits) ||
        !hwa_grf_producer_reader_work_fits(result, &meta.options))
        goto cleanup;
    for (index = 0U; index < result->excerpt_count; ++index) {
        const HWAGapReportExcerpt *r = &result->excerpts[index];
        uint64_t projected_file_bytes;
        uint64_t projected_file_count =
            r->make_x ? UINT64_C(3) : UINT64_C(2);
        if (r->frame_count > limits->max_excerpt_frames ||
            r->frame_count > meta.options.max_excerpt_frames ||
            r->reference_start_sample > limits->max_input_frames ||
            r->model_start_sample > limits->max_input_frames ||
            r->frame_count > limits->max_input_frames -
                r->reference_start_sample ||
            r->frame_count > limits->max_input_frames -
                r->model_start_sample ||
            r->reference_start_sample > meta.options.max_input_frames ||
            r->model_start_sample > meta.options.max_input_frames ||
            r->frame_count > meta.options.max_input_frames -
                r->reference_start_sample ||
            r->frame_count > meta.options.max_input_frames -
                r->model_start_sample ||
            r->frame_count > UINT64_MAX - excerpt_frames ||
            r->frame_count > (UINT64_MAX - UINT64_C(44)) / UINT64_C(2))
            goto cleanup;
        projected_file_bytes = UINT64_C(44) + r->frame_count * UINT64_C(2);
        if (projected_file_bytes > limits->max_output_file_bytes ||
            projected_file_bytes > meta.options.max_output_file_bytes ||
            projected_file_bytes > UINT64_MAX / projected_file_count ||
            projected_file_bytes * projected_file_count >
                UINT64_MAX - projected_audio_bytes)
            goto cleanup;
        projected_audio_bytes += projected_file_bytes * projected_file_count;
        excerpt_frames += r->frame_count;
    }
    if (excerpt_frames > limits->max_total_excerpt_frames ||
        excerpt_frames > meta.options.max_total_excerpt_frames)
        goto cleanup;
    for (index = 0U; index < result->excerpt_count; ++index) {
        HWAGapReportExcerpt *r = &result->excerpts[index];
        if (r->availability == HWA_GAP_REPORT_AVAILABLE) {
            if (hwa_grf_parse_file(
                    &reader, r, "a", &r->reference_path,
                    r->reference_sha256, &r->reference_file_bytes,
                    error, error_size) != 0 ||
                hwa_grf_parse_file(
                    &reader, r, "b", &r->model_path,
                    r->model_sha256, &r->model_file_bytes,
                    error, error_size) != 0 ||
                (r->make_x &&
                 hwa_grf_parse_file(
                     &reader, r, "x", &r->x_path, r->x_sha256,
                     &r->x_file_bytes, error, error_size) != 0)) {
                goto cleanup;
            }
            if (r->reference_file_bytes > limits->max_output_file_bytes ||
                r->reference_file_bytes > meta.options.max_output_file_bytes ||
                r->model_file_bytes > limits->max_output_file_bytes ||
                r->model_file_bytes > meta.options.max_output_file_bytes ||
                (r->make_x &&
                 (r->x_file_bytes > limits->max_output_file_bytes ||
                  r->x_file_bytes > meta.options.max_output_file_bytes)) ||
                r->reference_file_bytes > UINT64_MAX - output_bytes)
                goto cleanup;
            output_bytes += r->reference_file_bytes;
            if (r->model_file_bytes > UINT64_MAX - output_bytes) goto cleanup;
            output_bytes += r->model_file_bytes;
            if (r->make_x) {
                if (r->x_file_bytes > UINT64_MAX - output_bytes) goto cleanup;
                output_bytes += r->x_file_bytes;
            }
        }
    }
    if (!hwa_grf_excerpt_paths_canonical(result)) goto cleanup;
    if (output_bytes != meta.total_output_bytes) goto cleanup;
    for (index = 0U; index < result->candidate_count; ++index)
        if (result->candidates[index].availability !=
                HWA_GAP_REPORT_AVAILABLE &&
            hwa_grf_parse_exclusion(
                &reader, &result->candidates[index],
                error, error_size) != 0) goto cleanup;
    for (index = 0U; index < result->excerpt_count; ++index)
        if (result->excerpts[index].availability !=
                HWA_GAP_REPORT_AVAILABLE &&
            hwa_grf_parse_failure(
                &reader, &result->excerpts[index],
                error, error_size) != 0) goto cleanup;
    for (index = 0U; index < result->warning_count; ++index)
        if (hwa_grf_parse_warning(
                &reader, &result->warnings[index],
                (uint64_t)index + 1U,
                error, error_size) != 0) goto cleanup;
    if (result->mode != HWA_GAP_REPORT_RANK) {
        HWAGapReportResult projected = *result;
        projected.options = meta.options;
        if (hwa_gap_report_output_projected_upper(
                &projected, result->mode, projected_audio_bytes,
                &tree_bytes) != 0 ||
            tree_bytes > meta.options.max_bundle_bytes)
            goto cleanup;
        projected.options = *limits;
        if (hwa_gap_report_output_projected_upper(
                &projected, result->mode, projected_audio_bytes,
                &tree_bytes) != 0 || tree_bytes > limits->max_bundle_bytes)
            goto cleanup;
    }
    for (index = 0U; index < result->family_count; ++index) {
        size_t length = strlen(result->families[index].key) + 1U;
        if ((uint64_t)length > UINT64_MAX - derived_string_bytes)
            goto cleanup;
        derived_string_bytes += (uint64_t)length;
    }
    for (index = 0U; index < result->group_count; ++index) {
        size_t length = strlen(result->groups[index].value) + 1U;
        if ((uint64_t)length > UINT64_MAX - derived_string_bytes)
            goto cleanup;
        derived_string_bytes += (uint64_t)length;
    }
    if ((uint64_t)data_size + UINT64_C(1) >
            UINT64_MAX - derived_string_bytes)
        goto cleanup;
    reader_bytes = (uint64_t)data_size + UINT64_C(1) +
                   derived_string_bytes;
    if (reader.cursor != reader.size ||
        hwa_gap_report_result_peak_work_bytes(
            result, reader_bytes, &peak) != 0 ||
        peak > limits->max_work_bytes ||
        hwa_grf_normalize_derived(result, error, error_size) != 0 ||
        hwa_gap_report_result_retained_bytes(result, &retained) != 0 ||
        hwa_gap_report_result_canonical_retained_bytes(
            result, &canonical_retained) != 0 ||
        canonical_retained != meta.retained_work_bytes ||
        retained > limits->max_work_bytes ||
        (uint64_t)data_size + 1U >
            limits->max_work_bytes - retained) goto cleanup;
    result->retained_work_bytes = retained;
    if (hwa_gap_report_result_validate(
            result, error, error_size) != 0) goto cleanup;
    status = 0;
cleanup:
    free(meta.title);
    free(meta.audibility_method);
    free(data);
    if (status != 0) {
        hwa_gap_report_result_free(result);
        if (file_sha256 != NULL) file_sha256[0] = '\0';
        if (error == NULL || error_size == 0U || error[0] == '\0') {
            hwa_grf_error(error, error_size,
                          "invalid or over-limit Stage 9 result");
        }
    }
    return status;
}

int hwa_gap_report_file_read(
    const char *path,
    const HWAGapReportOptions *limits,
    HWAGapReportResult *result,
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
        hwa_grf_error(error, error_size,
                      "cannot enter the C numeric locale for Stage 9 input");
        return -1;
    }
    status = hwa_grf_read_impl(
        path, limits, result, file_sha256, &locale, error, error_size);
    if (hwa_c_numeric_locale_end(&locale) != 0) {
        if (status == 0 && result != NULL)
            hwa_gap_report_result_free(result);
        if (file_sha256 != NULL) file_sha256[0] = '\0';
        if (status == 0)
            hwa_grf_error(
                error, error_size,
                "cannot restore the numeric locale after Stage 9 input");
        return -1;
    }
    return status;
}

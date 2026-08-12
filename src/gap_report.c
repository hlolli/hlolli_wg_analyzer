#if !defined(_WIN32)
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "gap_report.h"

#include "dsp.h"
#include "experiment_file.h"
#include "hwa_features.h"
#include "gap_report_clip.h"
#include "gap_report_output.h"
#include "internal.h"
#include "measure_compare.h"
#include "measure_file.h"
#include "production.h"
#include "production_file.h"
#include "run.h"
#include "run_file.h"
#include "sha256.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#if !defined(O_NOFOLLOW)
#define O_NOFOLLOW 0
#endif

#if defined(_WIN32)
#define HWA_GAP_PATH_BOUND _MAX_PATH
#else
#define HWA_GAP_PATH_BOUND PATH_MAX
#endif

#define HWA_GAP_THRESHOLD (1.0 / 12.0)
#define HWA_GAP_STRING_MAX 65536U
#define HWA_GAP_WARNING_ALL ((UINT32_C(1) << 6) - 1U)
#define HWA_GAP_MISSING_LABEL "<missing>"

typedef struct HWAGapName {
    int value;
    const char *name;
} HWAGapName;

static const HWAGapName hwa_gap_mode_names[] = {
    {HWA_GAP_REPORT_RANK, "rank"},
    {HWA_GAP_REPORT_EXCERPTS, "excerpts"},
    {HWA_GAP_REPORT_FULL, "full"}
};

static const HWAGapName hwa_gap_source_names[] = {
    {HWA_GAP_REPORT_SOURCE_MEASUREMENT, "measurement"},
    {HWA_GAP_REPORT_SOURCE_PRODUCTION, "production"},
    {HWA_GAP_REPORT_SOURCE_RUN, "run"},
    {HWA_GAP_REPORT_SOURCE_EXPERIMENT, "experiment"},
    {HWA_GAP_REPORT_SOURCE_WAVE, "wave"}
};

static const HWAGapName hwa_gap_availability_names[] = {
    {HWA_GAP_REPORT_AVAILABLE, "available"},
    {HWA_GAP_REPORT_UNAVAILABLE, "unavailable"},
    {HWA_GAP_REPORT_INSUFFICIENT, "insufficient"},
    {HWA_GAP_REPORT_EXCLUDED, "excluded"}
};

static const HWAGapName hwa_gap_axis_names[] = {
    {HWA_GAP_REPORT_AXIS_PITCH, "pitch"},
    {HWA_GAP_REPORT_AXIS_REGISTER, "register"},
    {HWA_GAP_REPORT_AXIS_DYNAMIC, "dynamic"},
    {HWA_GAP_REPORT_AXIS_GESTURE, "gesture"},
    {HWA_GAP_REPORT_AXIS_PHYSICAL_ELEMENT, "physical-element"},
    {HWA_GAP_REPORT_AXIS_SECTION, "section"}
};

static const HWAGapName hwa_gap_view_names[] = {
    {HWA_GAP_REPORT_VIEW_RAW, "raw"},
    {HWA_GAP_REPORT_VIEW_BROAD_EQ_MATCHED, "broad-eq-matched"},
    {HWA_GAP_REPORT_VIEW_ROOM_MATCHED, "room-matched"},
    {HWA_GAP_REPORT_VIEW_STEM, "stem"},
    {HWA_GAP_REPORT_VIEW_PROBE_LINKED, "probe-linked"}
};

static const HWAGapName hwa_gap_candidate_names[] = {
    {HWA_GAP_REPORT_CANDIDATE_MEASUREMENT, "measurement"},
    {HWA_GAP_REPORT_CANDIDATE_PRODUCTION, "production"},
    {HWA_GAP_REPORT_CANDIDATE_RUN_FEATURE, "run-feature"},
    {HWA_GAP_REPORT_CANDIDATE_RUN_STAGE, "run-stage"},
    {HWA_GAP_REPORT_CANDIDATE_EXPERIMENT, "experiment"}
};

typedef struct HWAGapBlob {
    unsigned char *data;
    size_t size;
} HWAGapBlob;

typedef struct HWAGapIdentity {
    uint64_t size;
#if defined(_WIN32)
    uint64_t device;
    uint64_t inode;
#else
    dev_t device;
    ino_t inode;
#endif
} HWAGapIdentity;

typedef struct HWAGapJson {
    const unsigned char *data;
    size_t size;
    size_t offset;
    size_t tokens;
    size_t max_tokens;
    size_t depth;
    size_t max_depth;
    uint64_t max_work;
    uint64_t *live_work;
    char *error;
    size_t error_size;
} HWAGapJson;

typedef struct HWAGapLabelDecl {
    char *source;
    char *case_id;
    char *pitch;
    char *register_name;
    char *dynamic;
    char *gesture;
    char *physical_element;
    char *section;
} HWAGapLabelDecl;

typedef struct HWAGapExcerptDecl {
    char *id;
    char *candidate_source;
    uint64_t candidate_row;
    HWAGapReportView view;
    char *reference;
    char *model;
    uint64_t reference_start_sample;
    uint64_t model_start_sample;
    uint64_t frame_count;
    int make_x;
} HWAGapExcerptDecl;

typedef struct HWAGapManifest {
    HWAGapReportSource *sources;
    size_t source_count;
    size_t source_capacity;
    HWAGapLabelDecl *labels;
    size_t label_count;
    size_t label_capacity;
    HWAGapExcerptDecl *excerpts;
    size_t excerpt_count;
    size_t excerpt_capacity;
    char *title;
} HWAGapManifest;

static const char *hwa_gap_name_for(const HWAGapName *names,
                                    size_t count,
                                    int value)
{
    size_t index;
    for (index = 0U; index < count; ++index)
        if (names[index].value == value) return names[index].name;
    return NULL;
}

static int hwa_gap_name_from(const HWAGapName *names,
                             size_t count,
                             const char *name,
                             int *value)
{
    size_t index;
    if (name == NULL || value == NULL) return -1;
    for (index = 0U; index < count; ++index)
        if (strcmp(names[index].name, name) == 0) {
            *value = names[index].value;
            return 0;
        }
    return -1;
}

#define HWA_GAP_NAME_GETTER(function_, array_, type_)                       \
    const char *function_(type_ value)                                      \
    {                                                                        \
        return hwa_gap_name_for(array_,                                     \
            sizeof(array_) / sizeof((array_)[0]), (int)value);              \
    }

#define HWA_GAP_NAME_PARSER(function_, array_, type_)                       \
    int function_(const char *name, type_ *value)                           \
    {                                                                        \
        int parsed;                                                          \
        if (hwa_gap_name_from(array_,                                       \
                sizeof(array_) / sizeof((array_)[0]), name, &parsed) != 0)   \
            return -1;                                                       \
        *value = (type_)parsed;                                              \
        return 0;                                                            \
    }

HWA_GAP_NAME_GETTER(hwa_gap_report_mode_name,
                    hwa_gap_mode_names, HWAGapReportMode)
HWA_GAP_NAME_GETTER(hwa_gap_report_source_kind_name,
                    hwa_gap_source_names, HWAGapReportSourceKind)
HWA_GAP_NAME_GETTER(hwa_gap_report_availability_name,
                    hwa_gap_availability_names, HWAGapReportAvailability)
HWA_GAP_NAME_GETTER(hwa_gap_report_axis_name,
                    hwa_gap_axis_names, HWAGapReportAxis)
HWA_GAP_NAME_GETTER(hwa_gap_report_view_name,
                    hwa_gap_view_names, HWAGapReportView)
HWA_GAP_NAME_GETTER(hwa_gap_report_candidate_kind_name,
                    hwa_gap_candidate_names, HWAGapReportCandidateKind)

HWA_GAP_NAME_PARSER(hwa_gap_report_mode_from_name,
                    hwa_gap_mode_names, HWAGapReportMode)
HWA_GAP_NAME_PARSER(hwa_gap_report_source_kind_from_name,
                    hwa_gap_source_names, HWAGapReportSourceKind)
HWA_GAP_NAME_PARSER(hwa_gap_report_availability_from_name,
                    hwa_gap_availability_names, HWAGapReportAvailability)
HWA_GAP_NAME_PARSER(hwa_gap_report_axis_from_name,
                    hwa_gap_axis_names, HWAGapReportAxis)
HWA_GAP_NAME_PARSER(hwa_gap_report_view_from_name,
                    hwa_gap_view_names, HWAGapReportView)
HWA_GAP_NAME_PARSER(hwa_gap_report_candidate_kind_from_name,
                    hwa_gap_candidate_names, HWAGapReportCandidateKind)

#undef HWA_GAP_NAME_GETTER
#undef HWA_GAP_NAME_PARSER

static char *hwa_gap_strdup(const char *text)
{
    size_t size;
    char *copy;
    if (text == NULL) text = "";
    size = strlen(text) + 1U;
    copy = (char *)malloc(size);
    if (copy != NULL) memcpy(copy, text, size);
    return copy;
}

static int hwa_gap_add_u64(uint64_t *value, uint64_t add)
{
    if (value == NULL || add > UINT64_MAX - *value) return -1;
    *value += add;
    return 0;
}

static int hwa_gap_mul_u64(uint64_t left, uint64_t right, uint64_t *value)
{
    if (value == NULL || (right != 0U && left > UINT64_MAX / right)) return -1;
    *value = left * right;
    return 0;
}

static int hwa_gap_array_bytes(size_t count, size_t size, uint64_t *total)
{
    if (size != 0U && count > SIZE_MAX / size) return -1;
    return hwa_gap_add_u64(total, (uint64_t)(count * size));
}

static int hwa_gap_string_bytes(const char *text, uint64_t *total)
{
    return text == NULL ? 0 :
        hwa_gap_add_u64(total, (uint64_t)strlen(text) + UINT64_C(1));
}

static int hwa_gap_retained_impl(const HWAGapReportResult *result,
                                 int canonical,
                                 uint64_t *bytes)
{
    uint64_t total = sizeof(*result);
    size_t index;
    if (result == NULL || bytes == NULL ||
        hwa_gap_array_bytes(result->source_count, sizeof(*result->sources),
                            &total) != 0 ||
        hwa_gap_array_bytes(result->label_count, sizeof(*result->labels),
                            &total) != 0 ||
        hwa_gap_array_bytes(result->candidate_count,
                            sizeof(*result->candidates), &total) != 0 ||
        hwa_gap_array_bytes(result->family_count, sizeof(*result->families),
                            &total) != 0 ||
        hwa_gap_array_bytes(result->group_count, sizeof(*result->groups),
                            &total) != 0 ||
        hwa_gap_array_bytes(result->case_count, sizeof(*result->cases),
                            &total) != 0 ||
        hwa_gap_array_bytes(result->excerpt_count, sizeof(*result->excerpts),
                            &total) != 0 ||
        hwa_gap_array_bytes(result->warning_count, sizeof(*result->warnings),
                            &total) != 0 ||
        hwa_gap_string_bytes(canonical ? "." : result->manifest_path,
                             &total) != 0 ||
        hwa_gap_string_bytes(canonical && result->mode != HWA_GAP_REPORT_RANK
                                 ? "." : result->output_directory,
                             &total) != 0 ||
        hwa_gap_string_bytes(result->title, &total) != 0 ||
        hwa_gap_string_bytes(result->audibility_method, &total) != 0)
        return -1;
    for (index = 0U; index < result->source_count; ++index)
        if (hwa_gap_string_bytes(result->sources[index].name, &total) != 0 ||
            hwa_gap_string_bytes(canonical ? "." : result->sources[index].path,
                                 &total) != 0) return -1;
    for (index = 0U; index < result->label_count; ++index) {
        const HWAGapReportLabel *r = &result->labels[index];
        if (hwa_gap_string_bytes(r->case_id, &total) != 0 ||
            hwa_gap_string_bytes(r->pitch, &total) != 0 ||
            hwa_gap_string_bytes(r->register_name, &total) != 0 ||
            hwa_gap_string_bytes(r->dynamic, &total) != 0 ||
            hwa_gap_string_bytes(r->gesture, &total) != 0 ||
            hwa_gap_string_bytes(r->physical_element, &total) != 0 ||
            hwa_gap_string_bytes(r->section, &total) != 0) return -1;
    }
    for (index = 0U; index < result->candidate_count; ++index)
        if (hwa_gap_string_bytes(result->candidates[index].case_id, &total) != 0 ||
            hwa_gap_string_bytes(result->candidates[index].metric, &total) != 0 ||
            hwa_gap_string_bytes(result->candidates[index].family_key,
                                 &total) != 0 ||
            hwa_gap_string_bytes(result->candidates[index].reason, &total) != 0)
            return -1;
    for (index = 0U; index < result->family_count; ++index)
        if (hwa_gap_string_bytes(result->families[index].key, &total) != 0)
            return -1;
    for (index = 0U; index < result->group_count; ++index)
        if (hwa_gap_string_bytes(result->groups[index].value, &total) != 0)
            return -1;
    for (index = 0U; index < result->case_count; ++index)
        if (hwa_gap_string_bytes(result->cases[index].case_id, &total) != 0 ||
            hwa_gap_string_bytes(result->cases[index].reason, &total) != 0)
            return -1;
    for (index = 0U; index < result->excerpt_count; ++index) {
        const HWAGapReportExcerpt *r = &result->excerpts[index];
        if (hwa_gap_string_bytes(r->name, &total) != 0 ||
            hwa_gap_string_bytes(r->reference_path, &total) != 0 ||
            hwa_gap_string_bytes(r->model_path, &total) != 0 ||
            hwa_gap_string_bytes(r->x_path, &total) != 0 ||
            hwa_gap_string_bytes(r->reason, &total) != 0) return -1;
    }
    for (index = 0U; index < result->warning_count; ++index)
        if (hwa_gap_string_bytes(result->warnings[index].code, &total) != 0 ||
            hwa_gap_string_bytes(result->warnings[index].message, &total) != 0)
            return -1;
    *bytes = total;
    return 0;
}

int hwa_gap_report_result_retained_bytes(const HWAGapReportResult *result,
                                         uint64_t *bytes)
{
    return hwa_gap_retained_impl(result, 0, bytes);
}

int hwa_gap_report_result_canonical_retained_bytes(
    const HWAGapReportResult *result,
    uint64_t *bytes)
{
    return hwa_gap_retained_impl(result, 1, bytes);
}

int hwa_gap_report_result_peak_work_bytes(
    const HWAGapReportResult *result,
    uint64_t reader_bytes,
    uint64_t *peak)
{
    uint64_t total;
    uint64_t scratch = 0U;
    uint64_t part;
    uint64_t prospective_groups;
    uint64_t prospective_warnings;
    uint64_t label_bound;
    size_t index;
    if (result == NULL || peak == NULL ||
        hwa_gap_report_result_retained_bytes(result, &total) != 0 ||
        hwa_gap_mul_u64((uint64_t)result->candidate_count,
            (uint64_t)(sizeof(HWAGapReportCandidate) +
                       sizeof(HWAGapReportCandidate *) * 3U +
                       sizeof(double) * 2U), &part) != 0 ||
        hwa_gap_add_u64(&scratch, part) != 0 ||
        hwa_gap_mul_u64((uint64_t)result->case_count,
            (uint64_t)(sizeof(HWAGapReportCase) +
                       sizeof(HWAGapReportCase *) * 2U), &part) != 0 ||
        hwa_gap_add_u64(&scratch, part) != 0 ||
        hwa_gap_mul_u64((uint64_t)result->candidate_count,
            (uint64_t)sizeof(HWAGapReportFamily), &part) != 0 ||
        hwa_gap_add_u64(&scratch, part) != 0 ||
        (prospective_groups = (uint64_t)result->label_count,
         hwa_gap_add_u64(&prospective_groups, UINT64_C(1))) != 0 ||
        hwa_gap_mul_u64(prospective_groups, UINT64_C(6),
                        &prospective_groups) != 0)
        return -1;
    if (prospective_groups > (uint64_t)result->options.max_groups)
        prospective_groups = (uint64_t)result->options.max_groups;
    if (hwa_gap_mul_u64(
            (uint64_t)strlen(HWA_GAP_MISSING_LABEL) + UINT64_C(1),
            UINT64_C(6), &label_bound) != 0) return -1;
    for (index = 0U; index < result->candidate_count; ++index)
        if (hwa_gap_string_bytes(result->candidates[index].family_key,
                                 &scratch) != 0) return -1;
    for (index = 0U; index < result->label_count; ++index) {
        const HWAGapReportLabel *label = &result->labels[index];
        if (hwa_gap_string_bytes(label->pitch, &label_bound) != 0 ||
            hwa_gap_string_bytes(label->register_name, &label_bound) != 0 ||
            hwa_gap_string_bytes(label->dynamic, &label_bound) != 0 ||
            hwa_gap_string_bytes(label->gesture, &label_bound) != 0 ||
            hwa_gap_string_bytes(label->physical_element, &label_bound) != 0 ||
            hwa_gap_string_bytes(label->section, &label_bound) != 0)
            return -1;
    }
    if (hwa_gap_mul_u64(prospective_groups,
                        (uint64_t)sizeof(HWAGapReportGroup), &part) != 0 ||
        hwa_gap_add_u64(&scratch, part) != 0 ||
        hwa_gap_add_u64(&scratch, label_bound) != 0 ||
        hwa_gap_mul_u64((uint64_t)result->candidate_count,
                        (uint64_t)sizeof(double) * UINT64_C(2), &part) != 0 ||
        hwa_gap_add_u64(&scratch, part) != 0)
        return -1;
    prospective_warnings = (uint64_t)result->candidate_count;
    if (hwa_gap_mul_u64(prospective_warnings, UINT64_C(3),
                        &prospective_warnings) != 0 ||
        hwa_gap_add_u64(&prospective_warnings,
                        (uint64_t)result->source_count) != 0 ||
        hwa_gap_add_u64(&prospective_warnings,
                        (uint64_t)result->excerpt_count) != 0)
        return -1;
    if (prospective_warnings > (uint64_t)result->options.max_warnings)
        prospective_warnings = (uint64_t)result->options.max_warnings;
    if (hwa_gap_mul_u64(prospective_warnings,
            (uint64_t)sizeof(HWAGapReportWarning) + UINT64_C(96), &part) != 0 ||
        hwa_gap_add_u64(&scratch, part) != 0 ||
        hwa_gap_add_u64(&total, scratch) != 0 ||
        hwa_gap_add_u64(&total, reader_bytes) != 0) return -1;
    *peak = total;
    return 0;
}

void hwa_gap_report_options_default(HWAGapReportOptions *options)
{
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    options->decode_block_frames = 65536U;
    options->max_manifest_bytes = UINT64_C(8) * 1024U * 1024U;
    options->max_input_bytes = UINT64_C(16) * 1024U * 1024U * 1024U;
    options->max_input_frames = UINT64_C(1000000000);
    options->max_work_bytes = UINT64_C(1) * 1024U * 1024U * 1024U;
    options->max_evaluations = UINT64_C(2000000000);
    options->max_output_file_bytes = UINT64_C(4) * 1024U * 1024U * 1024U;
    options->max_bundle_bytes = UINT64_C(16) * 1024U * 1024U * 1024U;
    options->max_excerpt_frames = UINT64_C(1440000);
    options->max_total_excerpt_frames = UINT64_C(144000000);
    options->max_sources = 1024U;
    options->max_labels = 1000000U;
    options->max_candidates = 100000U;
    options->max_families = 100000U;
    options->max_groups = 1000000U;
    options->max_cases = 1000000U;
    options->max_excerpts = 1000U;
    options->max_warnings = 100000U;
    options->max_json_depth = 12U;
    options->max_json_tokens = 262144U;
    hwa_profile_comparison_options_default(&options->measurement);
    hwa_production_options_default(&options->production);
    hwa_run_options_default(&options->run);
    hwa_experiment_options_default(&options->experiment);
}

int hwa_gap_report_options_validate(const HWAGapReportOptions *options,
                                    char *error,
                                    size_t error_size)
{
    const HWAProfileComparisonOptions *production_profile =
        options == NULL ? NULL : &options->production.profile_limits;
    const HWARunOptions *experiment_run =
        options == NULL ? NULL : &options->experiment.run;
    if (options == NULL || options->decode_block_frames == 0U ||
        options->decode_block_frames > 1048576U ||
        options->max_manifest_bytes == 0U || options->max_input_bytes == 0U ||
        options->max_input_frames == 0U || options->max_work_bytes == 0U ||
        options->max_evaluations == 0U ||
        options->max_output_file_bytes == 0U ||
        options->max_bundle_bytes == 0U ||
        options->max_excerpt_frames == 0U ||
        options->max_total_excerpt_frames == 0U ||
        options->max_sources == 0U || options->max_labels == 0U ||
        options->max_candidates == 0U || options->max_families == 0U ||
        options->max_groups == 0U || options->max_cases == 0U ||
        options->max_excerpts == 0U || options->max_warnings == 0U ||
        options->max_json_depth == 0U || options->max_json_tokens == 0U ||
        options->measurement.max_input_bytes == 0U ||
        options->measurement.max_work_bytes == 0U ||
        options->measurement.max_contexts == 0U ||
        options->measurement.max_measurements == 0U ||
        options->measurement.max_groups == 0U ||
        options->measurement.max_group_members == 0U ||
        options->measurement.max_statistics == 0U ||
        options->measurement.max_warnings == 0U ||
        options->measurement.max_distributions == 0U ||
        options->measurement.max_gaps == 0U ||
        options->production.decode_block_frames == 0U ||
        options->production.max_input_bytes == 0U ||
        options->production.max_input_frames == 0U ||
        options->production.max_ir_frames == 0U ||
        options->production.max_work_bytes == 0U ||
        options->production.max_evaluations == 0U ||
        options->production.max_spans == 0U ||
        options->production.max_envelope_points == 0U ||
        options->production.max_fits == 0U ||
        options->production.max_evaluation_rows == 0U ||
        options->production.max_view_rows == 0U ||
        options->production.max_fits <
            hwa_production_fit_catalog_count() ||
        options->production.max_evaluation_rows <
            hwa_production_metric_catalog_count() * 3U ||
        options->production.max_view_rows <
            hwa_production_metric_catalog_count() * 6U ||
        options->production.max_warnings == 0U ||
        production_profile->max_input_bytes == 0U ||
        production_profile->max_work_bytes == 0U ||
        production_profile->max_contexts == 0U ||
        production_profile->max_measurements == 0U ||
        production_profile->max_groups == 0U ||
        production_profile->max_group_members == 0U ||
        production_profile->max_statistics == 0U ||
        production_profile->max_warnings == 0U ||
        production_profile->max_distributions == 0U ||
        production_profile->max_gaps == 0U ||
        options->run.decode_block_frames == 0U ||
        options->run.max_manifest_bytes == 0U ||
        options->run.max_input_bytes == 0U ||
        options->run.max_input_frames == 0U ||
        options->run.max_probe_bytes == 0U ||
        options->run.max_probe_values == 0U ||
        options->run.max_work_bytes == 0U ||
        options->run.max_evaluations == 0U || options->run.max_stems == 0U ||
        options->run.max_probes == 0U || options->run.max_links == 0U ||
        options->run.max_json_depth == 0U ||
        options->run.max_json_tokens == 0U ||
        options->run.max_result_rows == 0U ||
        options->run.max_warnings == 0U ||
        options->experiment.max_manifest_bytes == 0U ||
        options->experiment.max_input_bytes == 0U ||
        options->experiment.max_work_bytes == 0U ||
        options->experiment.max_bundle_bytes == 0U ||
        options->experiment.max_output_file_bytes == 0U ||
        options->experiment.max_total_run_evaluations == 0U ||
        options->experiment.max_job_milliseconds == 0U ||
        options->experiment.max_total_milliseconds == 0U ||
        options->experiment.max_parameters == 0U ||
        options->experiment.max_levels == 0U ||
        options->experiment.max_cases == 0U ||
        options->experiment.max_responses == 0U ||
        options->experiment.max_points == 0U ||
        (uint64_t)options->experiment.max_points >
            UINT64_MAX /
                (uint64_t)options->experiment.max_responses / UINT64_C(2) ||
        options->experiment.max_jobs == 0U ||
        options->experiment.max_replicates == 0U ||
        options->experiment.max_artifacts == 0U ||
        options->experiment.max_observations == 0U ||
        options->experiment.max_sensitivities == 0U ||
        options->experiment.max_interactions == 0U ||
        options->experiment.max_warnings == 0U ||
        experiment_run->decode_block_frames == 0U ||
        experiment_run->max_manifest_bytes == 0U ||
        experiment_run->max_input_bytes == 0U ||
        experiment_run->max_input_frames == 0U ||
        experiment_run->max_probe_bytes == 0U ||
        experiment_run->max_probe_values == 0U ||
        experiment_run->max_work_bytes == 0U ||
        experiment_run->max_evaluations == 0U ||
        experiment_run->max_stems == 0U ||
        experiment_run->max_probes == 0U ||
        experiment_run->max_links == 0U ||
        experiment_run->max_json_depth == 0U ||
        experiment_run->max_json_tokens == 0U ||
        experiment_run->max_result_rows == 0U ||
        experiment_run->max_warnings == 0U ||
        options->measurement.max_input_bytes > options->max_input_bytes ||
        options->measurement.max_work_bytes > options->max_work_bytes ||
        options->production.max_input_bytes > options->max_input_bytes ||
        options->production.max_work_bytes > options->max_work_bytes ||
        options->production.max_evaluations > options->max_evaluations ||
        production_profile->max_input_bytes > options->max_input_bytes ||
        production_profile->max_work_bytes > options->max_work_bytes ||
        options->run.max_input_bytes > options->max_input_bytes ||
        options->run.max_work_bytes > options->max_work_bytes ||
        options->run.max_evaluations > options->max_evaluations ||
        options->experiment.max_input_bytes > options->max_input_bytes ||
        options->experiment.max_work_bytes > options->max_work_bytes ||
        options->experiment.max_bundle_bytes > options->max_input_bytes ||
        options->experiment.max_total_run_evaluations >
            options->max_evaluations ||
        experiment_run->max_input_bytes > options->max_input_bytes ||
        experiment_run->max_work_bytes > options->max_work_bytes ||
        experiment_run->max_evaluations > options->max_evaluations) {
        hwa_set_error(error, error_size, "invalid Stage 9 options");
        return -1;
    }
    return 0;
}

void hwa_gap_report_result_free(HWAGapReportResult *result)
{
    size_t index;
    if (result == NULL) return;
    free(result->manifest_path);
    free(result->output_directory);
    free(result->title);
    free(result->audibility_method);
    for (index = 0U; index < result->source_count; ++index) {
        free(result->sources[index].name);
        free(result->sources[index].path);
    }
    for (index = 0U; index < result->label_count; ++index) {
        HWAGapReportLabel *r = &result->labels[index];
        free(r->case_id);
        free(r->pitch);
        free(r->register_name);
        free(r->dynamic);
        free(r->gesture);
        free(r->physical_element);
        free(r->section);
    }
    for (index = 0U; index < result->candidate_count; ++index) {
        free(result->candidates[index].case_id);
        free(result->candidates[index].metric);
        free(result->candidates[index].family_key);
        free(result->candidates[index].reason);
    }
    for (index = 0U; index < result->family_count; ++index)
        free(result->families[index].key);
    for (index = 0U; index < result->group_count; ++index)
        free(result->groups[index].value);
    for (index = 0U; index < result->case_count; ++index) {
        free(result->cases[index].case_id);
        free(result->cases[index].reason);
    }
    for (index = 0U; index < result->excerpt_count; ++index) {
        HWAGapReportExcerpt *r = &result->excerpts[index];
        free(r->name);
        free(r->reference_path);
        free(r->model_path);
        free(r->x_path);
        free(r->reason);
    }
    for (index = 0U; index < result->warning_count; ++index) {
        free(result->warnings[index].code);
        free(result->warnings[index].message);
    }
    free(result->sources);
    free(result->labels);
    free(result->candidates);
    free(result->families);
    free(result->groups);
    free(result->cases);
    free(result->excerpts);
    free(result->warnings);
    memset(result, 0, sizeof(*result));
}

static int hwa_gap_json_fail(HWAGapJson *json, const char *message)
{
    hwa_set_error(json->error, json->error_size,
                  "invalid Stage 9 manifest at byte %llu: %s",
                  (unsigned long long)json->offset, message);
    return -1;
}

static int hwa_gap_work_add(uint64_t *live,
                            uint64_t add,
                            uint64_t maximum,
                            char *error,
                            size_t error_size)
{
    if (live == NULL || add > UINT64_MAX - *live || *live + add > maximum) {
        hwa_set_error(error, error_size, "Stage 9 work-byte limit exceeded");
        return -1;
    }
    *live += add;
    return 0;
}

static void hwa_gap_work_sub(uint64_t *live, uint64_t bytes)
{
    if (live != NULL) *live = bytes <= *live ? *live - bytes : 0U;
}

static void hwa_gap_json_space(HWAGapJson *json)
{
    while (json->offset < json->size &&
           (json->data[json->offset] == ' ' ||
            json->data[json->offset] == '\t' ||
            json->data[json->offset] == '\r' ||
            json->data[json->offset] == '\n')) json->offset++;
}

static int hwa_gap_json_token(HWAGapJson *json)
{
    if (json->tokens >= json->max_tokens)
        return hwa_gap_json_fail(json, "token limit exceeded");
    json->tokens++;
    return 0;
}

static int hwa_gap_json_take(HWAGapJson *json, unsigned char wanted)
{
    hwa_gap_json_space(json);
    if (json->offset >= json->size || json->data[json->offset] != wanted)
        return hwa_gap_json_fail(json, "unexpected token");
    json->offset++;
    return 0;
}

static int hwa_gap_json_depth(HWAGapJson *json)
{
    if (json->depth >= json->max_depth)
        return hwa_gap_json_fail(json, "nesting limit exceeded");
    json->depth++;
    return 0;
}

static int hwa_gap_hex(unsigned char value)
{
    if (value >= '0' && value <= '9') return (int)(value - '0');
    if (value >= 'a' && value <= 'f') return 10 + (int)(value - 'a');
    if (value >= 'A' && value <= 'F') return 10 + (int)(value - 'A');
    return -1;
}

static int hwa_gap_json_string(HWAGapJson *json, char **out)
{
    size_t scan;
    size_t length = 0U;
    size_t write = 0U;
    char *text;
    if (out == NULL) return hwa_gap_json_fail(json, "missing string target");
    *out = NULL;
    hwa_gap_json_space(json);
    if (hwa_gap_json_token(json) != 0 || json->offset >= json->size ||
        json->data[json->offset] != '"')
        return hwa_gap_json_fail(json, "string expected");
    scan = ++json->offset;
    while (scan < json->size && json->data[scan] != '"') {
        unsigned char value = json->data[scan++];
        if (value < 0x20U || value >= 0x80U)
            return hwa_gap_json_fail(json, "strings must be ASCII");
        if (value == '\\') {
            unsigned char escaped;
            if (scan >= json->size)
                return hwa_gap_json_fail(json, "bad escape");
            escaped = json->data[scan++];
            if (escaped == 'u') {
                int a, b, c, d;
                unsigned code;
                if (scan + 4U > json->size ||
                    (a = hwa_gap_hex(json->data[scan])) < 0 ||
                    (b = hwa_gap_hex(json->data[scan + 1U])) < 0 ||
                    (c = hwa_gap_hex(json->data[scan + 2U])) < 0 ||
                    (d = hwa_gap_hex(json->data[scan + 3U])) < 0)
                    return hwa_gap_json_fail(json, "bad Unicode escape");
                code = ((unsigned)a << 12U) | ((unsigned)b << 8U) |
                       ((unsigned)c << 4U) | (unsigned)d;
                if (code == 0U || code >= 0x80U)
                    return hwa_gap_json_fail(json, "non-ASCII escape");
                scan += 4U;
            } else if (strchr("\"\\/bfnrt", (int)escaped) == NULL) {
                return hwa_gap_json_fail(json, "bad escape");
            }
        }
        if (++length > HWA_GAP_STRING_MAX)
            return hwa_gap_json_fail(json, "string too long");
    }
    if (scan >= json->size)
        return hwa_gap_json_fail(json, "unterminated string");
    if (hwa_gap_work_add(json->live_work, (uint64_t)length + UINT64_C(1),
                         json->max_work, json->error,
                         json->error_size) != 0) return -1;
    text = (char *)malloc(length + 1U);
    if (text == NULL) {
        hwa_gap_work_sub(json->live_work,
                         (uint64_t)length + UINT64_C(1));
        return hwa_gap_json_fail(json, "cannot allocate string");
    }
    while (json->offset < scan) {
        unsigned char value = json->data[json->offset++];
        if (value == '\\') {
            unsigned char escaped = json->data[json->offset++];
            if (escaped == 'u') {
                int a = hwa_gap_hex(json->data[json->offset]);
                int b = hwa_gap_hex(json->data[json->offset + 1U]);
                int c = hwa_gap_hex(json->data[json->offset + 2U]);
                int d = hwa_gap_hex(json->data[json->offset + 3U]);
                value = (unsigned char)(((unsigned)a << 12U) |
                    ((unsigned)b << 8U) | ((unsigned)c << 4U) | (unsigned)d);
                json->offset += 4U;
            } else if (escaped == 'b') value = '\b';
            else if (escaped == 'f') value = '\f';
            else if (escaped == 'n') value = '\n';
            else if (escaped == 'r') value = '\r';
            else if (escaped == 't') value = '\t';
            else value = escaped;
        }
        text[write++] = (char)value;
    }
    json->offset++;
    text[write] = '\0';
    *out = text;
    return 0;
}

static void hwa_gap_json_string_free(HWAGapJson *json, char **text)
{
    uint64_t bytes;
    if (json == NULL || text == NULL || *text == NULL) return;
    bytes = (uint64_t)strlen(*text) + UINT64_C(1);
    free(*text);
    *text = NULL;
    if (json->live_work != NULL && bytes <= *json->live_work)
        *json->live_work -= bytes;
}

static int hwa_gap_json_u64(HWAGapJson *json, uint64_t *value)
{
    uint64_t parsed = 0U;
    hwa_gap_json_space(json);
    if (value == NULL || hwa_gap_json_token(json) != 0 ||
        json->offset >= json->size || json->data[json->offset] < '0' ||
        json->data[json->offset] > '9')
        return hwa_gap_json_fail(json, "unsigned integer expected");
    if (json->data[json->offset] == '0' && json->offset + 1U < json->size &&
        json->data[json->offset + 1U] >= '0' &&
        json->data[json->offset + 1U] <= '9')
        return hwa_gap_json_fail(json, "leading zero");
    while (json->offset < json->size && json->data[json->offset] >= '0' &&
           json->data[json->offset] <= '9') {
        unsigned digit = (unsigned)(json->data[json->offset++] - '0');
        if (parsed > (UINT64_MAX - digit) / UINT64_C(10))
            return hwa_gap_json_fail(json, "integer overflow");
        parsed = parsed * UINT64_C(10) + digit;
    }
    *value = parsed;
    return 0;
}

static int hwa_gap_json_bool(HWAGapJson *json, int *value)
{
    hwa_gap_json_space(json);
    if (value == NULL || hwa_gap_json_token(json) != 0) return -1;
    if (json->offset + 4U <= json->size &&
        memcmp(json->data + json->offset, "true", 4U) == 0) {
        *value = 1;
        json->offset += 4U;
        return 0;
    }
    if (json->offset + 5U <= json->size &&
        memcmp(json->data + json->offset, "false", 5U) == 0) {
        *value = 0;
        json->offset += 5U;
        return 0;
    }
    return hwa_gap_json_fail(json, "boolean expected");
}

static int hwa_gap_seen(HWAGapJson *json, uint64_t *seen, unsigned bit)
{
    uint64_t mask = UINT64_C(1) << bit;
    if ((*seen & mask) != 0U)
        return hwa_gap_json_fail(json, "duplicate object key");
    *seen |= mask;
    return hwa_gap_json_take(json, ':');
}

static int hwa_gap_token_valid(const char *text, int dots)
{
    size_t index;
    size_t length;
    if (text == NULL || (length = strlen(text)) == 0U || length > 255U)
        return 0;
    for (index = 0U; index < length; ++index) {
        unsigned char c = (unsigned char)text[index];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || (dots && c == '.'))) return 0;
    }
    return 1;
}

static int hwa_gap_hash_valid(const char *text)
{
    size_t index;
    if (text == NULL || strlen(text) != 64U) return 0;
    for (index = 0U; index < 64U; ++index)
        if (!((text[index] >= '0' && text[index] <= '9') ||
              (text[index] >= 'a' && text[index] <= 'f'))) return 0;
    return 1;
}

int hwa_gap_report_excerpt_x_is_reference(
    const HWAGapReportResult *result,
    const HWAGapReportExcerpt *excerpt)
{
    unsigned char input[40];
    unsigned char digest[32];
    HWASha256 state;
    size_t index;
    if (result == NULL || excerpt == NULL ||
        !hwa_gap_hash_valid(result->manifest_sha256)) return 0;
    for (index = 0U; index < 32U; ++index) {
        unsigned char high = (unsigned char)result->manifest_sha256[index * 2U];
        unsigned char low =
            (unsigned char)result->manifest_sha256[index * 2U + 1U];
        unsigned high_value = high <= (unsigned char)'9'
            ? (unsigned)(high - (unsigned char)'0')
            : (unsigned)(high - (unsigned char)'a') + 10U;
        unsigned low_value = low <= (unsigned char)'9'
            ? (unsigned)(low - (unsigned char)'0')
            : (unsigned)(low - (unsigned char)'a') + 10U;
        input[index] = (unsigned char)((high_value << 4U) | low_value);
    }
    for (index = 0U; index < 8U; ++index)
        input[32U + index] =
            (unsigned char)(excerpt->id >> (index * 8U));
    hwa_sha256_init(&state);
    hwa_sha256_update(&state, input, sizeof(input));
    hwa_sha256_final(&state, digest);
    return (digest[31] & 1U) == 0U;
}

static int hwa_gap_grow(void **array,
                        size_t *capacity,
                        size_t count,
                        size_t maximum,
                        size_t size,
                        HWAGapJson *json)
{
    size_t next;
    void *grown;
    uint64_t added;
    if (count < *capacity) return 0;
    if (count >= maximum) return hwa_gap_json_fail(json, "row limit exceeded");
    next = *capacity == 0U ? 4U : *capacity * 2U;
    if (next < *capacity || next > maximum) next = maximum;
    if (size != 0U && next > SIZE_MAX / size)
        return hwa_gap_json_fail(json, "array too large");
    added = (uint64_t)(next - *capacity) * (uint64_t)size;
    if (next < *capacity ||
        hwa_gap_work_add(json->live_work, added,
                         json->max_work, json->error,
                         json->error_size) != 0) return -1;
    grown = realloc(*array, next * size);
    if (grown == NULL) {
        hwa_gap_work_sub(json->live_work, added);
        return hwa_gap_json_fail(json, "cannot grow array");
    }
    memset((unsigned char *)grown + *capacity * size, 0,
           (next - *capacity) * size);
    *array = grown;
    *capacity = next;
    return 0;
}

static void hwa_gap_manifest_free(HWAGapManifest *manifest)
{
    size_t index;
    if (manifest == NULL) return;
    for (index = 0U; index < manifest->source_count; ++index)
        free(manifest->sources[index].name);
    for (index = 0U; index < manifest->label_count; ++index) {
        HWAGapLabelDecl *r = &manifest->labels[index];
        free(r->source); free(r->case_id); free(r->pitch);
        free(r->register_name); free(r->dynamic); free(r->gesture);
        free(r->physical_element); free(r->section);
    }
    for (index = 0U; index < manifest->excerpt_count; ++index) {
        HWAGapExcerptDecl *r = &manifest->excerpts[index];
        free(r->id); free(r->candidate_source); free(r->reference);
        free(r->model);
    }
    free(manifest->sources);
    free(manifest->labels);
    free(manifest->excerpts);
    free(manifest->title);
    memset(manifest, 0, sizeof(*manifest));
}

static int hwa_gap_parse_source(HWAGapJson *json,
                                HWAGapReportSource *source)
{
    uint64_t seen = 0U;
    char *key = NULL;
    char *kind = NULL;
    char *hash = NULL;
    int first = 1;
    memset(source, 0, sizeof(*source));
    if (hwa_gap_json_take(json, '{') != 0 || hwa_gap_json_depth(json) != 0)
        return -1;
    for (;;) {
        hwa_gap_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == '}') {
            json->offset++;
            break;
        }
        if (!first && hwa_gap_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_gap_json_string(json, &key) != 0) goto failed;
        if (strcmp(key, "id") == 0) {
            if (hwa_gap_seen(json, &seen, 0U) != 0 ||
                hwa_gap_json_string(json, &source->name) != 0) goto failed;
        } else if (strcmp(key, "kind") == 0) {
            if (hwa_gap_seen(json, &seen, 1U) != 0 ||
                hwa_gap_json_string(json, &kind) != 0) goto failed;
        } else if (strcmp(key, "sha256") == 0) {
            if (hwa_gap_seen(json, &seen, 2U) != 0 ||
                hwa_gap_json_string(json, &hash) != 0) goto failed;
        } else {
            hwa_gap_json_fail(json, "unknown source key");
            goto failed;
        }
        hwa_gap_json_string_free(json, &key);
    }
    json->depth--;
    if (seen != UINT64_C(7) || !hwa_gap_token_valid(source->name, 1) ||
        hwa_gap_report_source_kind_from_name(kind, &source->kind) != 0 ||
        !hwa_gap_hash_valid(hash)) {
        hwa_gap_json_fail(json, "invalid source");
        hwa_gap_json_string_free(json, &kind);
        hwa_gap_json_string_free(json, &hash);
        hwa_gap_json_string_free(json, &source->name);
        memset(source, 0, sizeof(*source));
        return -1;
    }
    memcpy(source->sha256, hash, HWA_SHA256_HEX_SIZE);
    hwa_gap_json_string_free(json, &kind);
    hwa_gap_json_string_free(json, &hash);
    return 0;
failed:
    json->depth--;
    hwa_gap_json_string_free(json, &key);
    hwa_gap_json_string_free(json, &kind);
    hwa_gap_json_string_free(json, &hash);
    hwa_gap_json_string_free(json, &source->name);
    memset(source, 0, sizeof(*source));
    return -1;
}

static int hwa_gap_parse_label(HWAGapJson *json, HWAGapLabelDecl *label)
{
    uint64_t seen = 0U;
    char *key = NULL;
    int first = 1;
    memset(label, 0, sizeof(*label));
    if (hwa_gap_json_take(json, '{') != 0 || hwa_gap_json_depth(json) != 0)
        return -1;
    for (;;) {
        char **target = NULL;
        unsigned bit = 0U;
        hwa_gap_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == '}') {
            json->offset++;
            break;
        }
        if (!first && hwa_gap_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_gap_json_string(json, &key) != 0) goto failed;
        if (strcmp(key, "source") == 0) { target = &label->source; bit = 0U; }
        else if (strcmp(key, "case") == 0) { target = &label->case_id; bit = 1U; }
        else if (strcmp(key, "pitch") == 0) { target = &label->pitch; bit = 2U; }
        else if (strcmp(key, "register") == 0) {
            target = &label->register_name; bit = 3U;
        } else if (strcmp(key, "dynamic") == 0) {
            target = &label->dynamic; bit = 4U;
        } else if (strcmp(key, "gesture") == 0) {
            target = &label->gesture; bit = 5U;
        } else if (strcmp(key, "physical_element") == 0) {
            target = &label->physical_element; bit = 6U;
        } else if (strcmp(key, "section") == 0) {
            target = &label->section; bit = 7U;
        } else {
            hwa_gap_json_fail(json, "unknown case label key");
            goto failed;
        }
        if (hwa_gap_seen(json, &seen, bit) != 0 ||
            hwa_gap_json_string(json, target) != 0) goto failed;
        hwa_gap_json_string_free(json, &key);
    }
    json->depth--;
    if (seen != UINT64_C(0xff) || !hwa_gap_token_valid(label->source, 1) ||
        label->case_id[0] == '\0' ||
        strcmp(label->pitch, HWA_GAP_MISSING_LABEL) == 0 ||
        strcmp(label->register_name, HWA_GAP_MISSING_LABEL) == 0 ||
        strcmp(label->dynamic, HWA_GAP_MISSING_LABEL) == 0 ||
        strcmp(label->gesture, HWA_GAP_MISSING_LABEL) == 0 ||
        strcmp(label->physical_element, HWA_GAP_MISSING_LABEL) == 0 ||
        strcmp(label->section, HWA_GAP_MISSING_LABEL) == 0) {
        hwa_gap_json_fail(json, "invalid case label");
        hwa_gap_json_string_free(json, &label->source);
        hwa_gap_json_string_free(json, &label->case_id);
        hwa_gap_json_string_free(json, &label->pitch);
        hwa_gap_json_string_free(json, &label->register_name);
        hwa_gap_json_string_free(json, &label->dynamic);
        hwa_gap_json_string_free(json, &label->gesture);
        hwa_gap_json_string_free(json, &label->physical_element);
        hwa_gap_json_string_free(json, &label->section);
        memset(label, 0, sizeof(*label));
        return -1;
    }
    return 0;
failed:
    json->depth--;
    hwa_gap_json_string_free(json, &key);
    hwa_gap_json_string_free(json, &label->source);
    hwa_gap_json_string_free(json, &label->case_id);
    hwa_gap_json_string_free(json, &label->pitch);
    hwa_gap_json_string_free(json, &label->register_name);
    hwa_gap_json_string_free(json, &label->dynamic);
    hwa_gap_json_string_free(json, &label->gesture);
    hwa_gap_json_string_free(json, &label->physical_element);
    hwa_gap_json_string_free(json, &label->section);
    memset(label, 0, sizeof(*label));
    return -1;
}

static int hwa_gap_parse_excerpt(HWAGapJson *json,
                                  HWAGapExcerptDecl *excerpt)
{
    uint64_t seen = 0U;
    char *key = NULL;
    char *view = NULL;
    int first = 1;
    memset(excerpt, 0, sizeof(*excerpt));
    if (hwa_gap_json_take(json, '{') != 0 || hwa_gap_json_depth(json) != 0)
        return -1;
    for (;;) {
        hwa_gap_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == '}') {
            json->offset++;
            break;
        }
        if (!first && hwa_gap_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_gap_json_string(json, &key) != 0) goto failed;
        if (strcmp(key, "id") == 0) {
            if (hwa_gap_seen(json, &seen, 0U) != 0 ||
                hwa_gap_json_string(json, &excerpt->id) != 0) goto failed;
        } else if (strcmp(key, "candidate_source") == 0) {
            if (hwa_gap_seen(json, &seen, 1U) != 0 ||
                hwa_gap_json_string(json, &excerpt->candidate_source) != 0)
                goto failed;
        } else if (strcmp(key, "candidate_row") == 0) {
            if (hwa_gap_seen(json, &seen, 2U) != 0 ||
                hwa_gap_json_u64(json, &excerpt->candidate_row) != 0)
                goto failed;
        } else if (strcmp(key, "view") == 0) {
            if (hwa_gap_seen(json, &seen, 3U) != 0 ||
                hwa_gap_json_string(json, &view) != 0) goto failed;
        } else if (strcmp(key, "reference") == 0) {
            if (hwa_gap_seen(json, &seen, 4U) != 0 ||
                hwa_gap_json_string(json, &excerpt->reference) != 0)
                goto failed;
        } else if (strcmp(key, "model") == 0) {
            if (hwa_gap_seen(json, &seen, 5U) != 0 ||
                hwa_gap_json_string(json, &excerpt->model) != 0) goto failed;
        } else if (strcmp(key, "reference_start_sample") == 0) {
            if (hwa_gap_seen(json, &seen, 6U) != 0 ||
                hwa_gap_json_u64(json, &excerpt->reference_start_sample) != 0)
                goto failed;
        } else if (strcmp(key, "model_start_sample") == 0) {
            if (hwa_gap_seen(json, &seen, 7U) != 0 ||
                hwa_gap_json_u64(json, &excerpt->model_start_sample) != 0)
                goto failed;
        } else if (strcmp(key, "frame_count") == 0) {
            if (hwa_gap_seen(json, &seen, 8U) != 0 ||
                hwa_gap_json_u64(json, &excerpt->frame_count) != 0)
                goto failed;
        } else if (strcmp(key, "make_x") == 0) {
            if (hwa_gap_seen(json, &seen, 9U) != 0 ||
                hwa_gap_json_bool(json, &excerpt->make_x) != 0) goto failed;
        } else {
            hwa_gap_json_fail(json, "unknown excerpt key");
            goto failed;
        }
        hwa_gap_json_string_free(json, &key);
    }
    json->depth--;
    if (seen != UINT64_C(0x3ff) || !hwa_gap_token_valid(excerpt->id, 0) ||
        !hwa_gap_token_valid(excerpt->candidate_source, 1) ||
        !hwa_gap_token_valid(excerpt->reference, 1) ||
        !hwa_gap_token_valid(excerpt->model, 1) ||
        excerpt->candidate_row == 0U || excerpt->frame_count == 0U ||
        hwa_gap_report_view_from_name(view, &excerpt->view) != 0) {
        hwa_gap_json_fail(json, "invalid excerpt");
        hwa_gap_json_string_free(json, &view);
        hwa_gap_json_string_free(json, &excerpt->id);
        hwa_gap_json_string_free(json, &excerpt->candidate_source);
        hwa_gap_json_string_free(json, &excerpt->reference);
        hwa_gap_json_string_free(json, &excerpt->model);
        memset(excerpt, 0, sizeof(*excerpt));
        return -1;
    }
    hwa_gap_json_string_free(json, &view);
    return 0;
failed:
    json->depth--;
    hwa_gap_json_string_free(json, &key);
    hwa_gap_json_string_free(json, &view);
    hwa_gap_json_string_free(json, &excerpt->id);
    hwa_gap_json_string_free(json, &excerpt->candidate_source);
    hwa_gap_json_string_free(json, &excerpt->reference);
    hwa_gap_json_string_free(json, &excerpt->model);
    memset(excerpt, 0, sizeof(*excerpt));
    return -1;
}

static int hwa_gap_parse_sources(HWAGapJson *json,
                                 HWAGapManifest *manifest,
                                 size_t maximum)
{
    int first = 1;
    if (hwa_gap_json_take(json, '[') != 0 || hwa_gap_json_depth(json) != 0)
        return -1;
    for (;;) {
        hwa_gap_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == ']') {
            json->offset++;
            json->depth--;
            return manifest->source_count == 0U ?
                hwa_gap_json_fail(json, "at least one source is required") : 0;
        }
        if (!first && hwa_gap_json_take(json, ',') != 0) break;
        first = 0;
        if (hwa_gap_grow((void **)&manifest->sources,
                         &manifest->source_capacity, manifest->source_count,
                         maximum, sizeof(*manifest->sources), json) != 0 ||
            hwa_gap_parse_source(json,
                                 &manifest->sources[manifest->source_count]) != 0)
            break;
        manifest->source_count++;
    }
    json->depth--;
    return -1;
}

static int hwa_gap_parse_labels(HWAGapJson *json,
                                HWAGapManifest *manifest,
                                size_t maximum)
{
    int first = 1;
    if (hwa_gap_json_take(json, '[') != 0 || hwa_gap_json_depth(json) != 0)
        return -1;
    for (;;) {
        hwa_gap_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == ']') {
            json->offset++;
            json->depth--;
            return 0;
        }
        if (!first && hwa_gap_json_take(json, ',') != 0) break;
        first = 0;
        if (hwa_gap_grow((void **)&manifest->labels,
                         &manifest->label_capacity, manifest->label_count,
                         maximum, sizeof(*manifest->labels), json) != 0 ||
            hwa_gap_parse_label(json,
                                &manifest->labels[manifest->label_count]) != 0)
            break;
        manifest->label_count++;
    }
    json->depth--;
    return -1;
}

static int hwa_gap_parse_excerpts(HWAGapJson *json,
                                  HWAGapManifest *manifest,
                                  size_t maximum)
{
    int first = 1;
    if (hwa_gap_json_take(json, '[') != 0 || hwa_gap_json_depth(json) != 0)
        return -1;
    for (;;) {
        hwa_gap_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == ']') {
            json->offset++;
            json->depth--;
            return 0;
        }
        if (!first && hwa_gap_json_take(json, ',') != 0) break;
        first = 0;
        if (hwa_gap_grow((void **)&manifest->excerpts,
                         &manifest->excerpt_capacity, manifest->excerpt_count,
                         maximum, sizeof(*manifest->excerpts), json) != 0 ||
            hwa_gap_parse_excerpt(
                json, &manifest->excerpts[manifest->excerpt_count]) != 0)
            break;
        manifest->excerpt_count++;
    }
    json->depth--;
    return -1;
}

static int hwa_gap_manifest_parse(const HWAGapBlob *blob,
                                  const HWAGapReportOptions *options,
                                  uint64_t *live_work,
                                  HWAGapManifest *manifest,
                                  char *error,
                                  size_t error_size)
{
    HWAGapJson json;
    uint64_t seen = 0U;
    int first = 1;
    int status = -1;
    memset(manifest, 0, sizeof(*manifest));
    memset(&json, 0, sizeof(json));
    json.data = blob->data;
    json.size = blob->size;
    json.max_tokens = options->max_json_tokens;
    json.max_depth = options->max_json_depth;
    json.max_work = options->max_work_bytes;
    json.live_work = live_work;
    json.error = error;
    json.error_size = error_size;
    if (hwa_gap_json_take(&json, '{') != 0 || hwa_gap_json_depth(&json) != 0)
        goto cleanup;
    for (;;) {
        char *key = NULL;
        hwa_gap_json_space(&json);
        if (json.offset < json.size && json.data[json.offset] == '}') {
            json.offset++;
            break;
        }
        if (!first && hwa_gap_json_take(&json, ',') != 0) goto cleanup_depth;
        first = 0;
        if (hwa_gap_json_string(&json, &key) != 0) goto cleanup_depth;
        if (strcmp(key, "schema") == 0) {
            char *value = NULL;
            if (hwa_gap_seen(&json, &seen, 0U) != 0 ||
                hwa_gap_json_string(&json, &value) != 0 ||
                strcmp(value, "hwa-gap-report") != 0) {
                hwa_gap_json_string_free(&json, &value);
                hwa_gap_json_string_free(&json, &key);
                goto cleanup_depth;
            }
            hwa_gap_json_string_free(&json, &value);
        } else if (strcmp(key, "schema_version") == 0) {
            uint64_t value;
            if (hwa_gap_seen(&json, &seen, 1U) != 0 ||
                hwa_gap_json_u64(&json, &value) != 0 || value != 1U) {
                hwa_gap_json_string_free(&json, &key); goto cleanup_depth;
            }
        } else if (strcmp(key, "method_version") == 0) {
            char *value = NULL;
            if (hwa_gap_seen(&json, &seen, 2U) != 0 ||
                hwa_gap_json_string(&json, &value) != 0 ||
                strcmp(value, HWA_GAP_REPORT_METHOD_VERSION) != 0) {
                hwa_gap_json_string_free(&json, &value);
                hwa_gap_json_string_free(&json, &key);
                goto cleanup_depth;
            }
            hwa_gap_json_string_free(&json, &value);
        } else if (strcmp(key, "audibility_method") == 0) {
            char *value = NULL;
            if (hwa_gap_seen(&json, &seen, 3U) != 0 ||
                hwa_gap_json_string(&json, &value) != 0 ||
                strcmp(value, HWA_GAP_REPORT_AUDIBILITY_METHOD) != 0) {
                hwa_gap_json_string_free(&json, &value);
                hwa_gap_json_string_free(&json, &key);
                goto cleanup_depth;
            }
            hwa_gap_json_string_free(&json, &value);
        } else if (strcmp(key, "title") == 0) {
            if (hwa_gap_seen(&json, &seen, 4U) != 0 ||
                hwa_gap_json_string(&json, &manifest->title) != 0 ||
                manifest->title[0] == '\0') {
                hwa_gap_json_string_free(&json, &key); goto cleanup_depth;
            }
        } else if (strcmp(key, "sources") == 0) {
            if (hwa_gap_seen(&json, &seen, 5U) != 0 ||
                hwa_gap_parse_sources(&json, manifest, options->max_sources) != 0) {
                hwa_gap_json_string_free(&json, &key); goto cleanup_depth;
            }
        } else if (strcmp(key, "case_labels") == 0) {
            if (hwa_gap_seen(&json, &seen, 6U) != 0 ||
                hwa_gap_parse_labels(&json, manifest, options->max_labels) != 0) {
                hwa_gap_json_string_free(&json, &key); goto cleanup_depth;
            }
        } else if (strcmp(key, "excerpts") == 0) {
            if (hwa_gap_seen(&json, &seen, 7U) != 0 ||
                hwa_gap_parse_excerpts(&json, manifest,
                                       options->max_excerpts) != 0) {
                hwa_gap_json_string_free(&json, &key); goto cleanup_depth;
            }
        } else {
            hwa_gap_json_fail(&json, "unknown top-level key");
            hwa_gap_json_string_free(&json, &key);
            goto cleanup_depth;
        }
        hwa_gap_json_string_free(&json, &key);
    }
    json.depth--;
    hwa_gap_json_space(&json);
    if (seen != UINT64_C(0xff) || json.offset != json.size) {
        hwa_gap_json_fail(&json, "incomplete manifest or trailing data");
        goto cleanup;
    }
    status = 0;
    goto cleanup;
cleanup_depth:
    json.depth--;
cleanup:
    if (status != 0) hwa_gap_manifest_free(manifest);
    return status;
}

static int hwa_gap_identity(const char *path,
                            uint64_t maximum,
                            HWAGapIdentity *identity,
                            char *error,
                            size_t error_size)
{
#if defined(_WIN32)
    HANDLE handle;
    BY_HANDLE_FILE_INFORMATION info;
    uint64_t size;
    if (path == NULL || path[0] == '\0' || strcmp(path, "-") == 0) {
        hwa_set_error(error, error_size, "Stage 9 input is not a named regular file");
        return -1;
    }
    handle = CreateFileA(path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL |
        FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                                  FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        hwa_set_error(error, error_size,
                      "Stage 9 input is not a named regular file");
        return -1;
    }
    size = ((uint64_t)info.nFileSizeHigh << 32U) | info.nFileSizeLow;
    CloseHandle(handle);
#else
    struct stat status;
    if (path == NULL || path[0] == '\0' || strcmp(path, "-") == 0 ||
        lstat(path, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size < 0) {
        hwa_set_error(error, error_size, "Stage 9 input is not a named regular file");
        return -1;
    }
#endif
#if defined(_WIN32)
    if (size > maximum) {
#else
    if ((uint64_t)status.st_size > maximum) {
#endif
        hwa_set_error(error, error_size, "Stage 9 input exceeds its byte limit");
        return -1;
    }
#if defined(_WIN32)
    identity->size = size;
    identity->device = info.dwVolumeSerialNumber;
    identity->inode = ((uint64_t)info.nFileIndexHigh << 32U) |
        info.nFileIndexLow;
#else
    identity->size = (uint64_t)status.st_size;
    identity->device = status.st_dev;
    identity->inode = status.st_ino;
#endif
    return 0;
}

static int hwa_gap_identity_equal(const HWAGapIdentity *left,
                                  const HWAGapIdentity *right)
{
    return left->size == right->size && left->device == right->device &&
        left->inode == right->inode;
}

static int hwa_gap_blob_read(const char *path,
                             uint64_t maximum,
                             uint64_t max_work,
                             uint64_t *live_work,
                             HWAGapBlob *blob,
                             HWAGapIdentity *identity,
                             char hash[HWA_SHA256_HEX_SIZE],
                             char *error,
                             size_t error_size)
{
    FILE *stream = NULL;
    HWAGapIdentity after;
    HWAGapIdentity opened;
    size_t size;
    HWASha256 hash_state;
    unsigned char digest[32];
    memset(blob, 0, sizeof(*blob));
    if (hwa_gap_identity(path, maximum, identity, error, error_size) != 0 ||
        identity->size > SIZE_MAX ||
        hwa_gap_work_add(live_work, identity->size + UINT64_C(1), max_work,
                         error, error_size) != 0) return -1;
    size = (size_t)identity->size;
    blob->data = (unsigned char *)malloc(size + 1U);
    if (blob->data == NULL) {
        *live_work -= identity->size + UINT64_C(1);
        hwa_set_error(error, error_size, "cannot allocate Stage 9 input");
        return -1;
    }
#if defined(_WIN32)
    {
        HANDLE handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL |
            FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        BY_HANDLE_FILE_INFORMATION info;
        int descriptor;
        if (handle == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandle(handle, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                                      FILE_ATTRIBUTE_REPARSE_POINT)) != 0U ||
            ((uint64_t)info.nFileSizeHigh << 32U | info.nFileSizeLow) !=
                identity->size) {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            stream = NULL;
        } else {
            opened.size = identity->size;
            opened.device = info.dwVolumeSerialNumber;
            opened.inode = ((uint64_t)info.nFileIndexHigh << 32U) |
                info.nFileIndexLow;
            descriptor = _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY);
            if (descriptor < 0) CloseHandle(handle);
            else {
                stream = _fdopen(descriptor, "rb");
                if (stream == NULL) (void)_close(descriptor);
            }
        }
    }
#else
    {
        int descriptor = open(path, O_RDONLY | O_NOFOLLOW);
        struct stat status;
        if (descriptor < 0 || fstat(descriptor, &status) != 0 ||
            !S_ISREG(status.st_mode) || status.st_size < 0) {
            if (descriptor >= 0) (void)close(descriptor);
            stream = NULL;
        } else {
            opened.size = (uint64_t)status.st_size;
            opened.device = status.st_dev;
            opened.inode = status.st_ino;
            stream = fdopen(descriptor, "rb");
            if (stream == NULL) (void)close(descriptor);
        }
    }
#endif
    if (stream == NULL || !hwa_gap_identity_equal(identity, &opened) ||
        (size != 0U && fread(blob->data, 1U, size, stream) != size) ||
        fgetc(stream) != EOF) {
        if (stream != NULL) (void)fclose(stream);
        free(blob->data);
        blob->data = NULL;
        *live_work -= identity->size + UINT64_C(1);
        if (error != NULL && error_size != 0U && error[0] == '\0')
            hwa_set_error(error, error_size, "cannot read stable Stage 9 input");
        return -1;
    }
    if (fclose(stream) != 0) {
        stream = NULL;
        free(blob->data);
        blob->data = NULL;
        *live_work -= identity->size + UINT64_C(1);
        hwa_set_error(error, error_size, "cannot close Stage 9 input");
        return -1;
    }
    stream = NULL;
    if (hwa_gap_identity(path, maximum, &after, error, error_size) != 0 ||
        !hwa_gap_identity_equal(identity, &after)) {
        free(blob->data);
        blob->data = NULL;
        *live_work -= identity->size + UINT64_C(1);
        hwa_set_error(error, error_size, "Stage 9 input changed during read");
        return -1;
    }
    hwa_sha256_init(&hash_state);
    hwa_sha256_update(&hash_state, blob->data, size);
    hwa_sha256_final(&hash_state, digest);
    hwa_sha256_hex(digest, hash);
    blob->data[size] = '\0';
    blob->size = size;
    return 0;
}

static void hwa_gap_blob_free(HWAGapBlob *blob, uint64_t *live_work)
{
    uint64_t bytes;
    if (blob == NULL || blob->data == NULL) return;
    bytes = (uint64_t)blob->size + UINT64_C(1);
    free(blob->data);
    memset(blob, 0, sizeof(*blob));
    if (live_work != NULL && bytes <= *live_work) *live_work -= bytes;
}

static int hwa_gap_hash_stable(const char *path,
                               uint64_t maximum,
                               HWAGapIdentity *identity,
                               char hash[HWA_SHA256_HEX_SIZE],
                               char *error,
                               size_t error_size)
{
    FILE *stream = NULL;
    HWAGapIdentity opened;
    HWAGapIdentity after;
    HWASha256 state;
    unsigned char digest[32];
    unsigned char buffer[65536];
    uint64_t bytes = 0U;
    size_t got;
    if (hwa_gap_identity(path, maximum, identity, error, error_size) != 0)
        return -1;
#if defined(_WIN32)
    {
        HANDLE handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL |
            FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        BY_HANDLE_FILE_INFORMATION info;
        int descriptor = -1;
        if (handle != INVALID_HANDLE_VALUE &&
            GetFileInformationByHandle(handle, &info) &&
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                                      FILE_ATTRIBUTE_REPARSE_POINT)) == 0U) {
            opened.size = ((uint64_t)info.nFileSizeHigh << 32U) |
                info.nFileSizeLow;
            opened.device = info.dwVolumeSerialNumber;
            opened.inode = ((uint64_t)info.nFileIndexHigh << 32U) |
                info.nFileIndexLow;
            descriptor = _open_osfhandle((intptr_t)handle,
                                          _O_RDONLY | _O_BINARY);
            if (descriptor >= 0) {
                stream = _fdopen(descriptor, "rb");
                if (stream == NULL) (void)_close(descriptor);
            } else CloseHandle(handle);
        } else if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
#else
    {
        int descriptor = open(path, O_RDONLY | O_NOFOLLOW);
        struct stat status;
        if (descriptor >= 0 && fstat(descriptor, &status) == 0 &&
            S_ISREG(status.st_mode) && status.st_size >= 0) {
            opened.size = (uint64_t)status.st_size;
            opened.device = status.st_dev;
            opened.inode = status.st_ino;
            stream = fdopen(descriptor, "rb");
            if (stream == NULL) (void)close(descriptor);
        } else if (descriptor >= 0) (void)close(descriptor);
    }
#endif
    if (stream == NULL || !hwa_gap_identity_equal(identity, &opened)) {
        if (stream != NULL) (void)fclose(stream);
        hwa_set_error(error, error_size, "cannot open stable Stage 9 source");
        return -1;
    }
    hwa_sha256_init(&state);
    while ((got = fread(buffer, 1U, sizeof(buffer), stream)) != 0U) {
        if ((uint64_t)got > maximum - bytes) {
            (void)fclose(stream);
            hwa_set_error(error, error_size,
                          "Stage 9 source exceeds its byte limit");
            return -1;
        }
        hwa_sha256_update(&state, buffer, got);
        bytes += (uint64_t)got;
    }
    {
        int read_failed = ferror(stream);
        int close_failed = fclose(stream) != 0;
        stream = NULL;
        if (read_failed || bytes != identity->size || close_failed ||
        hwa_gap_identity(path, maximum, &after, error, error_size) != 0 ||
        !hwa_gap_identity_equal(identity, &after)) {
            hwa_set_error(error, error_size,
                          "Stage 9 source changed during read");
            return -1;
        }
    }
    hwa_sha256_final(&state, digest);
    hwa_sha256_hex(digest, hash);
    return 0;
}

static uint64_t hwa_gap_source_input_limit(
    const HWAGapReportOptions *options,
    HWAGapReportSourceKind kind)
{
    uint64_t limit = options->max_input_bytes;
    uint64_t nested = limit;
    switch (kind) {
    case HWA_GAP_REPORT_SOURCE_MEASUREMENT:
        nested = options->measurement.max_input_bytes;
        break;
    case HWA_GAP_REPORT_SOURCE_PRODUCTION:
        nested = options->production.max_input_bytes;
        break;
    case HWA_GAP_REPORT_SOURCE_RUN:
        nested = options->run.max_input_bytes;
        break;
    case HWA_GAP_REPORT_SOURCE_EXPERIMENT:
        nested = options->experiment.max_output_file_bytes <
                options->experiment.max_bundle_bytes ?
            options->experiment.max_output_file_bytes :
            options->experiment.max_bundle_bytes;
        break;
    case HWA_GAP_REPORT_SOURCE_WAVE:
        break;
    default:
        return 0U;
    }
    return nested < limit ? nested : limit;
}

static double hwa_gap_clamp(double value)
{
    if (value <= 0.0) return 0.0;
    if (value >= 1.0) return 1.0;
    return value;
}

static unsigned hwa_gap_popcount(uint32_t value)
{
    unsigned count = 0U;
    while (value != 0U) {
        count += value & UINT32_C(1);
        value >>= 1U;
    }
    return count;
}

static int hwa_gap_evaluations_add(uint64_t *evaluations,
                                   uint64_t add,
                                   uint64_t maximum,
                                   char *error,
                                   size_t error_size);

static uint64_t hwa_gap_log_steps(size_t count);

static const HWAGapReportSource *hwa_gap_source_by_id(
    const HWAGapReportResult *result,
    uint64_t id)
{
    return id == 0U || id > result->source_count ? NULL :
        &result->sources[id - 1U];
}

static HWAGapReportSource *hwa_gap_source_by_name(HWAGapReportResult *result,
                                                   const char *name)
{
    size_t index;
    for (index = 0U; index < result->source_count; ++index)
        if (strcmp(result->sources[index].name, name) == 0)
            return &result->sources[index];
    return NULL;
}

static const HWAGapReportCandidate *hwa_gap_candidate_by_source_row(
    const HWAGapReportResult *result,
    uint64_t source_id,
    uint64_t source_row)
{
    size_t index;
    const HWAGapReportCandidate *found = NULL;
    for (index = 0U; index < result->candidate_count; ++index) {
        const HWAGapReportCandidate *candidate = &result->candidates[index];
        if (candidate->source_id != source_id ||
            candidate->source_row != source_row) continue;
        if (found != NULL) return NULL;
        found = candidate;
    }
    return found;
}

static const HWARunBinding *hwa_gap_binding(const HWARunBinding *bindings,
                                             size_t count,
                                             const char *id)
{
    size_t index;
    const HWARunBinding *found = NULL;
    for (index = 0U; index < count; ++index)
        if (bindings[index].id != NULL && strcmp(bindings[index].id, id) == 0) {
            if (found != NULL) return NULL;
            found = &bindings[index];
        }
    return found;
}

static int hwa_gap_append_candidate(HWAGapReportResult *result,
                                    uint64_t source_id,
                                    uint64_t source_row,
                                    const char *case_id,
                                    const char *metric,
                                    const char *family_key,
                                    HWAGapReportCandidateKind kind,
                                    HWAGapReportAvailability availability,
                                    double raw_value,
                                    int raw_valid,
                                    uint32_t quality,
                                    const char *reason,
                                    uint64_t *live_work,
                                    char *error,
                                    size_t error_size,
                                    uint64_t *candidate_id)
{
    HWAGapReportCandidate *grown;
    HWAGapReportCandidate *row;
    uint64_t strings = (uint64_t)strlen(case_id == NULL ? "" : case_id) +
        (uint64_t)strlen(metric == NULL ? "" : metric) +
        (uint64_t)strlen(family_key == NULL ? "" : family_key) +
        (uint64_t)strlen(reason == NULL ? "" : reason) + UINT64_C(4);
    if (result->candidate_count >= result->options.max_candidates ||
        result->candidate_count >= SIZE_MAX / sizeof(*result->candidates) ||
        hwa_gap_work_add(live_work, sizeof(*row) + strings,
                         result->options.max_work_bytes,
                         error, error_size) != 0 ||
        result->candidate_count == SIZE_MAX) {
        hwa_set_error(error, error_size, "Stage 9 candidate cap exceeded");
        return -1;
    }
    grown = (HWAGapReportCandidate *)realloc(
        result->candidates,
        (result->candidate_count + 1U) * sizeof(*result->candidates));
    if (grown == NULL) {
        hwa_set_error(error, error_size, "cannot allocate Stage 9 candidates");
        return -1;
    }
    result->candidates = grown;
    row = &result->candidates[result->candidate_count];
    memset(row, 0, sizeof(*row));
    row->id = (uint64_t)result->candidate_count + UINT64_C(1);
    result->candidate_count++;
    row->source_id = source_id;
    row->source_row = source_row;
    row->case_id = hwa_gap_strdup(case_id);
    row->metric = hwa_gap_strdup(metric);
    row->family_key = hwa_gap_strdup(family_key);
    row->reason = hwa_gap_strdup(reason);
    if (row->case_id == NULL || row->metric == NULL || row->family_key == NULL ||
        row->reason == NULL) {
        hwa_set_error(error, error_size, "cannot allocate Stage 9 candidate text");
        return -1;
    }
    row->kind = kind;
    row->availability = availability;
    row->raw_value = raw_value == 0.0 ? 0.0 : raw_value;
    row->raw_value_valid = raw_valid;
    row->quality_flags = quality;
    *candidate_id = row->id;
    return 0;
}

static int hwa_gap_append_case(HWAGapReportResult *result,
                               uint64_t candidate_id,
                               const char *case_id,
                               HWAGapReportAvailability availability,
                               double value,
                               int value_valid,
                               double confidence,
                               int confidence_valid,
                               const char *reason,
                               uint64_t *live_work,
                               char *error,
                               size_t error_size)
{
    HWAGapReportCase *grown;
    HWAGapReportCase *row;
    uint64_t strings = (uint64_t)strlen(case_id == NULL ? "" : case_id) +
        (uint64_t)strlen(reason == NULL ? "" : reason) + UINT64_C(2);
    if (result->case_count >= result->options.max_cases ||
        result->case_count >= SIZE_MAX / sizeof(*result->cases) ||
        hwa_gap_work_add(live_work, sizeof(*row) + strings,
                         result->options.max_work_bytes,
                         error, error_size) != 0 || result->case_count == SIZE_MAX) {
        hwa_set_error(error, error_size, "Stage 9 case cap exceeded");
        return -1;
    }
    grown = (HWAGapReportCase *)realloc(
        result->cases, (result->case_count + 1U) * sizeof(*result->cases));
    if (grown == NULL) {
        hwa_set_error(error, error_size, "cannot allocate Stage 9 cases");
        return -1;
    }
    result->cases = grown;
    row = &result->cases[result->case_count];
    memset(row, 0, sizeof(*row));
    row->id = (uint64_t)result->case_count + UINT64_C(1);
    result->case_count++;
    row->candidate_id = candidate_id;
    row->case_id = hwa_gap_strdup(case_id);
    row->reason = hwa_gap_strdup(reason);
    if (row->case_id == NULL || row->reason == NULL) {
        hwa_set_error(error, error_size, "cannot allocate Stage 9 case text");
        return -1;
    }
    row->availability = availability;
    row->value = value == 0.0 ? 0.0 : value;
    row->confidence = confidence == 0.0 ? 0.0 : confidence;
    row->value_valid = value_valid;
    row->confidence_valid = confidence_valid;
    return 0;
}

static int hwa_gap_case_compare(const void *left, const void *right)
{
    const HWAGapReportCase *a = (const HWAGapReportCase *)left;
    const HWAGapReportCase *b = (const HWAGapReportCase *)right;
    int order = strcmp(a->case_id, b->case_id);
    if (order != 0) return order;
    return a->id < b->id ? -1 : a->id > b->id ? 1 : 0;
}

static void hwa_gap_sort_candidate_cases(HWAGapReportResult *result,
                                         size_t begin)
{
    size_t index;
    size_t count = result->case_count - begin;
    qsort(result->cases + begin, count, sizeof(*result->cases),
          hwa_gap_case_compare);
    for (index = begin; index < result->case_count; ++index)
        result->cases[index].id = (uint64_t)index + UINT64_C(1);
}

static const char *hwa_gap_measure_metric(HWAMeasureKind kind,
                                          uint32_t index,
                                          char buffer[64])
{
    if (kind == HWA_MEASURE_RMS_DBFS || kind == HWA_MEASURE_PEAK_DBFS ||
        kind == HWA_MEASURE_BAND_BALANCE_DB ||
        kind == HWA_MEASURE_RESIDUAL_LEVEL_DBFS ||
        kind == HWA_MEASURE_HARMONIC_LEVEL_DBFS ||
        kind == HWA_MEASURE_PARTIAL_LEVEL_DBFS)
        return "level";
    if (kind == HWA_MEASURE_CREST_DB) return "crest";
    if (kind == HWA_MEASURE_BAND_LEVEL_DBFS ||
        kind == HWA_MEASURE_RESIDUAL_BAND_LEVEL_DBFS) {
        (void)snprintf(buffer, 64U, "band:%" PRIu32, index);
        return buffer;
    }
    if ((kind >= HWA_MEASURE_PITCH_HZ &&
         kind <= HWA_MEASURE_GLIDE_TIME_SECONDS) ||
        kind == HWA_MEASURE_TRANSITION_PITCH_CHANGE_CENTS)
        return "pitch";
    if ((kind >= HWA_MEASURE_ATTACK_DELAY_SECONDS &&
         kind <= HWA_MEASURE_RISE_90_SECONDS) ||
        kind == HWA_MEASURE_DURATION_SECONDS ||
        kind == HWA_MEASURE_GAP_OVERLAP_SECONDS) return "timing";
    return "other";
}

static const char *hwa_gap_production_metric(HWAProductionMetricKind kind,
                                             uint32_t index,
                                             char buffer[64])
{
    if (kind == HWA_PRODUCTION_METRIC_RMS_DBFS ||
        kind == HWA_PRODUCTION_METRIC_LEVEL_SPREAD_DB) return "level";
    if (kind == HWA_PRODUCTION_METRIC_CREST_DB) return "crest";
    if (kind == HWA_PRODUCTION_METRIC_BAND_LEVEL_DBFS) {
        (void)snprintf(buffer, 64U, "band:%" PRIu32, index);
        return buffer;
    }
    if (kind == HWA_PRODUCTION_METRIC_EARLY_REFLECTION_DB ||
        kind == HWA_PRODUCTION_METRIC_LATE_DECAY_SECONDS) return "timing";
    return "other";
}

static const char *hwa_gap_run_metric(HWARunFeatureKind kind,
                                      uint32_t index,
                                      char buffer[64])
{
    if (kind == HWA_RUN_FEATURE_RMS_DBFS) return "level";
    if (kind == HWA_RUN_FEATURE_CREST_DB) return "crest";
    if (kind == HWA_RUN_FEATURE_BAND_LEVEL_DBFS) {
        (void)snprintf(buffer, 64U, "band:%" PRIu32, index);
        return buffer;
    }
    return "other";
}

static int hwa_gap_metric_valid(const char *metric)
{
    size_t index;
    if (metric == NULL) return 0;
    if (strcmp(metric, "level") == 0 || strcmp(metric, "crest") == 0 ||
        strcmp(metric, "pitch") == 0 || strcmp(metric, "timing") == 0 ||
        strcmp(metric, "other") == 0) return 1;
    if (strncmp(metric, "band:", 5U) != 0 || metric[5] == '\0') return 0;
    for (index = 5U; metric[index] != '\0'; ++index)
        if (metric[index] < '0' || metric[index] > '9') return 0;
    if (metric[5] == '0' && metric[6] != '\0') return 0;
    {
        char *end = NULL;
        unsigned long value = strtoul(metric + 5U, &end, 10);
        return end != NULL && *end == '\0' && value <= 9UL;
    }
}

static char *hwa_gap_format_alloc(int needed,
                                  uint64_t *live_work,
                                  uint64_t maximum,
                                  uint64_t *charged,
                                  char *error,
                                  size_t error_size)
{
    uint64_t bytes;
    char *text;
    *charged = 0U;
    if (needed < 0 || (uint64_t)needed == UINT64_MAX) {
        hwa_set_error(error, error_size, "cannot size Stage 9 text");
        return NULL;
    }
    bytes = (uint64_t)needed + UINT64_C(1);
    if (bytes > SIZE_MAX || hwa_gap_work_add(
            live_work, bytes, maximum, error, error_size) != 0) {
        return NULL;
    }
    text = (char *)malloc((size_t)bytes);
    if (text == NULL) {
        hwa_gap_work_sub(live_work, bytes);
        hwa_set_error(error, error_size, "cannot allocate Stage 9 text");
        return NULL;
    }
    *charged = bytes;
    return text;
}

static char *hwa_gap_format_measurement(uint64_t source_id,
                                         const char *group,
                                         const char *metric,
                                         uint64_t *live_work,
                                         uint64_t maximum,
                                         uint64_t *charged,
                                         char *error,
                                         size_t error_size)
{
    int needed;
    char *text;
    if (strncmp(metric, "band:", 5U) == 0) metric = "band";
    needed = snprintf(NULL, 0U, "source:%" PRIu64
                      ":measurement:%s:%s", source_id, group, metric);
    text = hwa_gap_format_alloc(needed, live_work, maximum, charged,
                                error, error_size);
    if (text != NULL)
        (void)snprintf(text, (size_t)needed + 1U,
                       "source:%" PRIu64 ":measurement:%s:%s",
                       source_id, group, metric);
    return text;
}

static char *hwa_gap_format_production(uint64_t source_id,
                                       const char *metric,
                                       uint64_t *live_work,
                                       uint64_t maximum,
                                       uint64_t *charged,
                                       char *error,
                                       size_t error_size)
{
    int needed;
    char *text;
    if (strncmp(metric, "band:", 5U) == 0) metric = "band";
    needed = snprintf(NULL, 0U, "source:%" PRIu64 ":production:%s",
                      source_id, metric);
    text = hwa_gap_format_alloc(needed, live_work, maximum, charged,
                                error, error_size);
    if (text != NULL)
        (void)snprintf(text, (size_t)needed + 1U,
                       "source:%" PRIu64 ":production:%s", source_id, metric);
    return text;
}

static char *hwa_gap_format_run_role(uint64_t source_id,
                                     int role,
                                     const char *metric,
                                     uint64_t *live_work,
                                     uint64_t maximum,
                                     uint64_t *charged,
                                     char *error,
                                     size_t error_size)
{
    int needed;
    char *text;
    if (strncmp(metric, "band:", 5U) == 0) metric = "band";
    needed = snprintf(NULL, 0U, "source:%" PRIu64 ":run:role:%d:%s",
                      source_id, role, metric);
    text = hwa_gap_format_alloc(needed, live_work, maximum, charged,
                                error, error_size);
    if (text != NULL)
        (void)snprintf(text, (size_t)needed + 1U,
                       "source:%" PRIu64 ":run:role:%d:%s",
                       source_id, role, metric);
    return text;
}

static char *hwa_gap_format_case(const char *prefix,
                                 uint64_t id,
                                 uint64_t *live_work,
                                 uint64_t maximum,
                                 uint64_t *charged,
                                 char *error,
                                 size_t error_size)
{
    int needed = snprintf(NULL, 0U, "%s:%" PRIu64, prefix, id);
    char *text = hwa_gap_format_alloc(needed, live_work, maximum, charged,
                                      error, error_size);
    if (text != NULL)
        (void)snprintf(text, (size_t)needed + 1U,
                       "%s:%" PRIu64, prefix, id);
    return text;
}

static char *hwa_gap_format_run_stage(uint64_t source_id,
                                      int from_role,
                                      int to_role,
                                      uint64_t *live_work,
                                      uint64_t maximum,
                                      uint64_t *charged,
                                      char *error,
                                      size_t error_size)
{
    int needed = snprintf(NULL, 0U, "source:%" PRIu64 ":run:stage:%d:%d",
                          source_id, from_role, to_role);
    char *text = hwa_gap_format_alloc(needed, live_work, maximum, charged,
                                      error, error_size);
    if (text != NULL)
        (void)snprintf(text, (size_t)needed + 1U,
                       "source:%" PRIu64 ":run:stage:%d:%d",
                       source_id, from_role, to_role);
    return text;
}

static char *hwa_gap_format_experiment_role(uint64_t source_id,
                                            int role,
                                            const char *metric,
                                            uint64_t *live_work,
                                            uint64_t maximum,
                                            uint64_t *charged,
                                            char *error,
                                            size_t error_size)
{
    int needed;
    char *text;
    if (strncmp(metric, "band:", 5U) == 0) metric = "band";
    needed = snprintf(NULL, 0U,
                      "source:%" PRIu64 ":experiment:role:%d:%s",
                      source_id, role, metric);
    text = hwa_gap_format_alloc(needed, live_work, maximum, charged,
                                error, error_size);
    if (text != NULL)
        (void)snprintf(text, (size_t)needed + 1U,
                       "source:%" PRIu64 ":experiment:role:%d:%s",
                       source_id, role, metric);
    return text;
}

static int hwa_gap_role_family_valid(uint64_t source_id,
                                     const char *metric,
                                     const char *saved,
                                     int experiment,
                                     int *matched_role)
{
    int role;
    for (role = HWA_RUN_STEM_SOURCE; role < HWA_RUN_STEM_ROLE_COUNT; ++role) {
        char expected[160];
        int length = snprintf(expected, sizeof(expected), experiment
            ? "source:%" PRIu64 ":experiment:role:%d:%s"
            : "source:%" PRIu64 ":run:role:%d:%s",
            source_id, role,
            strncmp(metric, "band:", 5U) == 0 ? "band" : metric);
        int match = length >= 0 && (size_t)length < sizeof(expected) &&
            strcmp(expected, saved) == 0;
        if (match) {
            if (matched_role != NULL) *matched_role = role;
            return 1;
        }
    }
    return 0;
}

static int hwa_gap_measurement_family_valid(
    const HWAGapReportCandidate *candidate)
{
    char prefix[96];
    const char *metric = strncmp(candidate->metric, "band:", 5U) == 0
        ? "band" : candidate->metric;
    int length = snprintf(prefix, sizeof(prefix),
                          "source:%" PRIu64 ":measurement:",
                          candidate->source_id);
    size_t prefix_size;
    size_t case_size;
    size_t metric_size;
    size_t saved_size;
    if (length < 0 || (size_t)length >= sizeof(prefix)) return 0;
    prefix_size = (size_t)length;
    case_size = strlen(candidate->case_id);
    metric_size = strlen(metric);
    saved_size = strlen(candidate->family_key);
    return prefix_size <= SIZE_MAX - case_size - metric_size - 1U &&
        saved_size == prefix_size + case_size + metric_size + 1U &&
        memcmp(candidate->family_key, prefix, prefix_size) == 0 &&
        memcmp(candidate->family_key + prefix_size,
               candidate->case_id, case_size) == 0 &&
        candidate->family_key[prefix_size + case_size] == ':' &&
        memcmp(candidate->family_key + prefix_size + case_size + 1U,
               metric, metric_size) == 0;
}

static int hwa_gap_production_family_valid(
    const HWAGapReportCandidate *candidate,
    const char *metric)
{
    char expected[160];
    int length = snprintf(expected, sizeof(expected),
                          "source:%" PRIu64 ":production:%s",
                          candidate->source_id,
                          strncmp(metric, "band:", 5U) == 0
                              ? "band" : metric);
    return length >= 0 && (size_t)length < sizeof(expected) &&
        strcmp(expected, candidate->family_key) == 0;
}

static int hwa_gap_stage_family_valid(
    const HWAGapReportCandidate *candidate,
    HWARunStemRole from_role,
    HWARunStemRole to_role)
{
    char expected[160];
    int length = snprintf(expected, sizeof(expected),
                          "source:%" PRIu64 ":run:stage:%d:%d",
                          candidate->source_id,
                          (int)from_role, (int)to_role);
    return length >= 0 && (size_t)length < sizeof(expected) &&
        strcmp(expected, candidate->family_key) == 0;
}

static HWAGapReportAvailability hwa_gap_run_availability(
    HWARunAvailability value)
{
    if (value == HWA_RUN_AVAILABLE) return HWA_GAP_REPORT_AVAILABLE;
    if (value == HWA_RUN_INSUFFICIENT) return HWA_GAP_REPORT_INSUFFICIENT;
    return HWA_GAP_REPORT_UNAVAILABLE;
}

static HWAGapReportAvailability hwa_gap_production_availability(
    HWAProductionAvailability value)
{
    if (value == HWA_PRODUCTION_AVAILABLE) return HWA_GAP_REPORT_AVAILABLE;
    if (value == HWA_PRODUCTION_INSUFFICIENT) return HWA_GAP_REPORT_INSUFFICIENT;
    return HWA_GAP_REPORT_UNAVAILABLE;
}

static uint32_t hwa_gap_quality_normalize(uint32_t foreign,
                                          double confidence,
                                          int confidence_valid)
{
    uint32_t quality = foreign == 0U ? 0U :
        HWA_GAP_REPORT_QUALITY_SOURCE_WARNING;
    if (!confidence_valid || confidence < 0.5)
        quality |= HWA_GAP_REPORT_QUALITY_LOW_CONFIDENCE;
    return quality;
}

/*
 * Stage 4 has no visit-limit input. Bound its three saved-file reads, row
 * checks, merge passes, binary searches, and rank sorts before invoking it.
 * A canonical row consumes at least one input byte. Eight logarithmic passes
 * plus 64 linear passes therefore dominate the fixed Stage 4 implementation.
 */
static int hwa_gap_measurement_visit_preflight(
    uint64_t reference_bytes,
    uint64_t model_bytes,
    uint64_t *evaluations,
    uint64_t maximum,
    char *error,
    size_t error_size)
{
    uint64_t bytes = reference_bytes;
    uint64_t value;
    uint64_t logarithm = 0U;
    uint64_t passes;
    uint64_t visits;
    if (hwa_gap_add_u64(&bytes, model_bytes) != 0) return -1;
    value = bytes;
    while (value > UINT64_C(1)) {
        value = (value + UINT64_C(1)) / UINT64_C(2);
        logarithm++;
    }
    if (hwa_gap_mul_u64(logarithm, UINT64_C(8), &passes) != 0 ||
        hwa_gap_add_u64(&passes, UINT64_C(64)) != 0 ||
        hwa_gap_mul_u64(bytes, passes, &visits) != 0)
        return -1;
    return hwa_gap_evaluations_add(
        evaluations, visits, maximum, error, error_size);
}

static int hwa_gap_adapt_measurement_pair(
    HWAGapReportResult *result,
    const HWAGapReportSource *reference_source,
    const HWAGapReportSource *model_source,
    uint64_t *live_work,
    uint64_t *evaluations,
    char *error,
    size_t error_size)
{
    HWAProfileComparisonSet comparison;
    HWAProfileComparisonOptions limits = result->options.measurement;
    HWAMeasurementSet reference;
    HWAMeasurementSet model;
    char reference_hash[HWA_SHA256_HEX_SIZE];
    char model_hash[HWA_SHA256_HEX_SIZE];
    size_t index;
    uint64_t visits = 0U;
    uint64_t comparison_work = 0U;
    uint64_t reference_work = 0U;
    uint64_t model_work = 0U;
    uint64_t new_work;
    uint64_t sort_visits = 0U;
#define HWA_GAP_MEASUREMENT_CLEANUP()                                      \
    do {                                                                     \
        hwa_gap_work_sub(live_work, model_work);                             \
        hwa_gap_work_sub(live_work, reference_work);                         \
        hwa_gap_work_sub(live_work, comparison_work);                        \
        hwa_measurement_set_free(&reference);                                \
        hwa_measurement_set_free(&model);                                    \
        hwa_profile_comparison_set_free(&comparison);                        \
    } while (0)
    memset(&comparison, 0, sizeof(comparison));
    memset(&reference, 0, sizeof(reference));
    memset(&model, 0, sizeof(model));
    if (*live_work >= result->options.max_work_bytes) return -1;
    if (hwa_gap_measurement_visit_preflight(
            reference_source->file_bytes, model_source->file_bytes,
            evaluations, result->options.max_evaluations,
            error, error_size) != 0) return -1;
    if (limits.max_work_bytes > result->options.max_work_bytes - *live_work)
        limits.max_work_bytes = result->options.max_work_bytes - *live_work;
    if (hwa_compare_measure_files(reference_source->path, model_source->path,
                                  &limits, &comparison,
                                  error, error_size) != 0) return -1;
    if (strcmp(comparison.reference_sha256, reference_source->sha256) != 0 ||
        strcmp(comparison.model_sha256, model_source->sha256) != 0) {
        hwa_profile_comparison_set_free(&comparison);
        hwa_set_error(error, error_size,
                      "Stage 9 measurement source hash mismatch");
        return -1;
    }
    comparison_work = comparison.retained_work_bytes;
    if (hwa_gap_work_add(live_work, comparison_work,
            result->options.max_work_bytes, error, error_size) != 0) {
        hwa_profile_comparison_set_free(&comparison);
        return -1;
    }
    limits.max_work_bytes = result->options.max_work_bytes - *live_work;
    if (limits.max_work_bytes == 0U ||
        hwa_measure_file_read(reference_source->path, &limits, &reference,
                              reference_hash, error, error_size) != 0 ||
        strcmp(reference_hash, reference_source->sha256) != 0) {
        HWA_GAP_MEASUREMENT_CLEANUP();
        hwa_set_error(error, error_size,
                      "Stage 9 reference case source mismatch");
        return -1;
    }
    new_work = reference.retained_work_bytes;
    if (hwa_gap_work_add(live_work, new_work,
            result->options.max_work_bytes, error, error_size) != 0) {
        HWA_GAP_MEASUREMENT_CLEANUP();
        return -1;
    }
    reference_work = new_work;
    limits.max_work_bytes = result->options.max_work_bytes - *live_work;
    if (limits.max_work_bytes == 0U ||
        hwa_measure_file_read(model_source->path, &limits, &model,
                              model_hash, error, error_size) != 0 ||
        strcmp(model_hash, model_source->sha256) != 0) {
        HWA_GAP_MEASUREMENT_CLEANUP();
        hwa_set_error(error, error_size,
                      "Stage 9 measurement case source mismatch");
        return -1;
    }
    new_work = model.retained_work_bytes;
    if (hwa_gap_work_add(live_work, new_work,
            result->options.max_work_bytes, error, error_size) != 0) {
        HWA_GAP_MEASUREMENT_CLEANUP();
        return -1;
    }
    model_work = new_work;
    if (hwa_gap_evaluations_add(evaluations,
            reference.item_frame_evaluations,
            result->options.max_evaluations, error, error_size) != 0 ||
        hwa_gap_evaluations_add(evaluations,
            model.item_frame_evaluations,
            result->options.max_evaluations, error, error_size) != 0 ||
        hwa_gap_mul_u64((uint64_t)comparison.gap_count,
            (uint64_t)reference.group_member_count +
                (uint64_t)reference.group_count +
                (uint64_t)reference.measurement_count +
                (uint64_t)model.context_count +
                (uint64_t)model.measurement_count + UINT64_C(1),
            &visits) != 0 ||
        hwa_gap_mul_u64((uint64_t)reference.group_member_count,
            hwa_gap_log_steps(reference.group_member_count),
            &sort_visits) != 0 ||
        hwa_gap_mul_u64(sort_visits,
            (uint64_t)comparison.gap_count, &sort_visits) != 0 ||
        hwa_gap_add_u64(&visits, sort_visits) != 0 ||
        hwa_gap_evaluations_add(evaluations, visits,
            result->options.max_evaluations, error, error_size) != 0) {
        HWA_GAP_MEASUREMENT_CLEANUP();
        return -1;
    }
    for (index = 0U; index < comparison.gap_count; ++index) {
        const HWAProfileGap *gap = &comparison.gaps[index];
        const HWAProfileDistribution *distribution =
            &comparison.distributions[gap->distribution_id - 1U];
        const HWAMeasureGroup *group =
            &comparison.groups[distribution->group_id - 1U];
        const HWAMeasureGroup *reference_group = NULL;
        size_t reference_group_index;
        char metric_buffer[64];
        const char *metric = hwa_gap_measure_metric(
            distribution->kind, distribution->index, metric_buffer);
        uint64_t family_work = 0U;
        char *family = hwa_gap_format_measurement(
            reference_source->id, group->key, metric, live_work,
            result->options.max_work_bytes, &family_work, error, error_size);
        uint64_t candidate_id;
        size_t case_begin = result->case_count;
        double raw = gap->gap_score_valid && gap->valid_coverage_valid &&
                     gap->valid_coverage > 0.0 ?
            gap->gap_score / gap->valid_coverage : 0.0;
        int valid = gap->gap_score_valid && gap->valid_coverage_valid &&
            gap->valid_coverage > 0.0 && isfinite(raw);
        double confidence = valid ? gap->valid_coverage : 0.0;
        for (reference_group_index = 0U;
             reference_group_index < reference.group_count;
             ++reference_group_index)
            if (strcmp(reference.groups[reference_group_index].key,
                       group->key) == 0) {
                reference_group = &reference.groups[reference_group_index];
                break;
            }
        if (valid) raw = hwa_gap_clamp(raw);
        if (family == NULL || hwa_gap_append_candidate(
                result, reference_source->id, gap->id, group->key, metric,
                family, HWA_GAP_REPORT_CANDIDATE_MEASUREMENT,
                valid ? HWA_GAP_REPORT_AVAILABLE : HWA_GAP_REPORT_INSUFFICIENT,
                raw, valid, hwa_gap_quality_normalize(
                    gap->quality_flags, confidence, valid),
                valid ? "" : "measurement-gap-unavailable", live_work,
                error, error_size, &candidate_id) != 0) {
            free(family);
            hwa_gap_work_sub(live_work, family_work);
            HWA_GAP_MEASUREMENT_CLEANUP();
            return -1;
        }
        free(family);
        hwa_gap_work_sub(live_work, family_work);
        {
            size_t member;
            int emitted = 0;
            for (member = 0U; member < reference.group_member_count; ++member) {
                uint64_t reference_item_id =
                    reference.group_members[member].item_id;
                const HWAMeasureItemContext *reference_context;
                size_t model_context_index;
                const HWAMeasureObservation *reference_observation = NULL;
                const HWAMeasureObservation *model_observation = NULL;
                size_t observation;
                if (reference_group == NULL ||
                    reference.group_members[member].group_id !=
                        reference_group->id ||
                    reference_item_id == 0U ||
                    reference_item_id > reference.context_count) continue;
                reference_context = &reference.contexts[reference_item_id - 1U];
                if (reference_context->excluded) continue;
                for (model_context_index = 0U;
                     model_context_index < model.context_count;
                     ++model_context_index)
                    if (!model.contexts[model_context_index].excluded &&
                        strcmp(model.contexts[model_context_index].item_key,
                               reference_context->item_key) == 0) break;
                for (observation = 0U;
                     observation < reference.measurement_count; ++observation) {
                    const HWAMeasureObservation *fact =
                        &reference.measurements[observation];
                    if (fact->item_id == reference_context->item_id &&
                        fact->kind == distribution->kind &&
                        fact->index == distribution->index &&
                        fact->view == distribution->view) {
                        reference_observation = fact;
                        break;
                    }
                }
                if (model_context_index < model.context_count)
                    for (observation = 0U;
                         observation < model.measurement_count; ++observation) {
                        const HWAMeasureObservation *fact =
                            &model.measurements[observation];
                        if (fact->item_id ==
                                model.contexts[model_context_index].item_id &&
                            fact->kind == distribution->kind &&
                            fact->index == distribution->index &&
                            fact->view == distribution->view) {
                            model_observation = fact;
                            break;
                        }
                    }
                {
                    int pair_valid = reference_observation != NULL &&
                        model_observation != NULL &&
                        reference_observation->status ==
                            HWA_MEASURE_STATUS_VALID &&
                        model_observation->status == HWA_MEASURE_STATUS_VALID;
                    double case_value = pair_valid ?
                        fabs(model_observation->value -
                             reference_observation->value) /
                        (1.0 + fabs(model_observation->value -
                                    reference_observation->value)) : 0.0;
                    double case_confidence = pair_valid ? fmin(
                        reference_observation->confidence,
                        model_observation->confidence) : 0.0;
                    if (hwa_gap_append_case(result, candidate_id,
                            reference_context->item_key,
                            pair_valid ? HWA_GAP_REPORT_AVAILABLE :
                                         HWA_GAP_REPORT_UNAVAILABLE,
                            case_value, pair_valid, case_confidence,
                            pair_valid, pair_valid ? "" :
                                "measurement-side-unavailable",
                            live_work, error, error_size) != 0) {
                        HWA_GAP_MEASUREMENT_CLEANUP();
                        return -1;
                    }
                    emitted = 1;
                }
            }
            if (!emitted && hwa_gap_append_case(result, candidate_id,
                    group->key, HWA_GAP_REPORT_UNAVAILABLE, 0.0, 0, 0.0, 0,
                    "measurement-case-unavailable", live_work,
                    error, error_size) != 0) {
                HWA_GAP_MEASUREMENT_CLEANUP();
                return -1;
            }
            hwa_gap_sort_candidate_cases(result, case_begin);
        }
    }
    HWA_GAP_MEASUREMENT_CLEANUP();
#undef HWA_GAP_MEASUREMENT_CLEANUP
    return 0;
}

static int hwa_gap_adapt_production(HWAGapReportResult *result,
                                    const HWAGapReportSource *source,
                                    uint64_t *live_work,
                                    uint64_t *evaluations,
                                    char *error,
                                    size_t error_size)
{
    HWAProductionResult production;
    HWAProductionOptions limits = result->options.production;
    char hash[HWA_SHA256_HEX_SIZE];
    size_t index;
    uint64_t nested_work = 0U;
    uint64_t visits;
    memset(&production, 0, sizeof(production));
    if (*live_work >= result->options.max_work_bytes) return -1;
    if (limits.max_work_bytes > result->options.max_work_bytes - *live_work)
        limits.max_work_bytes = result->options.max_work_bytes - *live_work;
    if (limits.profile_limits.max_work_bytes > limits.max_work_bytes)
        limits.profile_limits.max_work_bytes = limits.max_work_bytes;
    if (limits.max_evaluations > result->options.max_evaluations - *evaluations)
        limits.max_evaluations = result->options.max_evaluations - *evaluations;
    if (limits.max_evaluations == 0U ||
        hwa_production_file_read(source->path, &limits,
                                 &production, hash, error, error_size) != 0 ||
        strcmp(hash, source->sha256) != 0) {
        hwa_production_result_free(&production);
        if (error != NULL && error_size != 0U && error[0] == '\0')
            hwa_set_error(error, error_size, "Stage 9 production hash mismatch");
        return -1;
    }
    nested_work = production.retained_work_bytes;
    if (hwa_gap_work_add(live_work, nested_work,
            result->options.max_work_bytes, error, error_size) != 0) {
        hwa_production_result_free(&production);
        return -1;
    }
    if (hwa_gap_evaluations_add(evaluations, production.evaluation_count,
            result->options.max_evaluations, error, error_size) != 0 ||
        (visits = (uint64_t)production.evaluation_row_count,
         hwa_gap_add_u64(&visits, UINT64_C(1))) != 0 ||
        hwa_gap_mul_u64(visits, (uint64_t)production.span_count,
                        &visits) != 0 ||
        hwa_gap_add_u64(&visits, UINT64_C(1)) != 0 ||
        hwa_gap_mul_u64(visits, (uint64_t)production.view_row_count,
                        &visits) != 0 ||
        hwa_gap_evaluations_add(evaluations, visits,
            result->options.max_evaluations, error, error_size) != 0) {
        hwa_gap_work_sub(live_work, nested_work);
        hwa_production_result_free(&production);
        return -1;
    }
    for (index = 0U; index < production.view_row_count; ++index) {
        const HWAProductionViewRow *row = &production.view_rows[index];
        char metric_buffer[64];
        const char *metric = hwa_gap_production_metric(
            row->kind, row->index, metric_buffer);
        char *family;
        uint64_t family_work = 0U;
        uint64_t candidate_id;
        HWAGapReportAvailability availability;
        int valid;
        double raw;
        uint64_t span;
        if (row->split != HWA_PRODUCTION_CHECK) continue;
        family = hwa_gap_format_production(
            source->id, metric, live_work, result->options.max_work_bytes,
            &family_work, error, error_size);
        valid = row->availability == HWA_PRODUCTION_AVAILABLE && row->gap_valid &&
            isfinite(row->raw_gap_score);
        availability = hwa_gap_production_availability(row->availability);
        raw = valid ? hwa_gap_clamp(fabs(row->raw_gap_score)) : 0.0;
        if (family == NULL || hwa_gap_append_candidate(
                result, source->id, row->id, "aggregate", metric, family,
                HWA_GAP_REPORT_CANDIDATE_PRODUCTION, availability, raw, valid,
                hwa_gap_quality_normalize(row->quality_flags,
                    row->reference_statistics.confidence,
                    row->reference_statistics.valid),
                valid ? "" : "production-row-unavailable",
                live_work, error, error_size, &candidate_id) != 0) {
            free(family); hwa_gap_work_sub(live_work, family_work);
            hwa_gap_work_sub(live_work, nested_work);
            hwa_production_result_free(&production); return -1;
        }
        free(family);
        hwa_gap_work_sub(live_work, family_work);
        for (span = 0U; span < production.span_count; ++span) {
            const HWAProductionSpan *span_row = &production.spans[span];
            size_t eval;
            int found = 0;
            if (span_row->split != HWA_PRODUCTION_CHECK) continue;
            for (eval = 0U; eval < production.evaluation_row_count; ++eval) {
                const HWAProductionEvaluation *fact =
                    &production.evaluations[eval];
                if (fact->span_id == span_row->id && fact->kind == row->kind &&
                    fact->index == row->index && fact->view == row->view) {
                    int fact_valid = fact->availability == HWA_PRODUCTION_AVAILABLE &&
                        fact->delta_valid && isfinite(fact->delta);
                    double value = fact_valid ?
                        fabs(fact->delta) / (1.0 + fabs(fact->delta)) : 0.0;
                    if (hwa_gap_append_case(
                            result, candidate_id, span_row->item_key,
                            hwa_gap_production_availability(fact->availability),
                            value, fact_valid, fact->confidence,
                            fact_valid && isfinite(fact->confidence),
                            fact_valid ? "" : "production-case-unavailable",
                            live_work, error, error_size) != 0) {
                        hwa_gap_work_sub(live_work, nested_work);
                        hwa_production_result_free(&production); return -1;
                    }
                    found = 1;
                    break;
                }
            }
            if (!found && hwa_gap_append_case(
                    result, candidate_id, span_row->item_key,
                    HWA_GAP_REPORT_UNAVAILABLE, 0.0, 0, 0.0, 0,
                    "production-case-missing", live_work,
                    error, error_size) != 0) {
                hwa_gap_work_sub(live_work, nested_work);
                hwa_production_result_free(&production); return -1;
            }
        }
    }
    hwa_gap_work_sub(live_work, nested_work);
    hwa_production_result_free(&production);
    return 0;
}

static int hwa_gap_adapt_run(HWAGapReportResult *result,
                             const HWAGapReportSource *source,
                             uint64_t *live_work,
                             uint64_t *evaluations,
                             char *error,
                             size_t error_size)
{
    HWARunResult run;
    HWARunOptions limits = result->options.run;
    char hash[HWA_SHA256_HEX_SIZE];
    size_t index;
    uint64_t nested_work = 0U;
    uint64_t adapter_visits = 0U;
    memset(&run, 0, sizeof(run));
    if (*live_work >= result->options.max_work_bytes) return -1;
    if (limits.max_work_bytes > result->options.max_work_bytes - *live_work)
        limits.max_work_bytes = result->options.max_work_bytes - *live_work;
    if (limits.max_evaluations > result->options.max_evaluations - *evaluations)
        limits.max_evaluations = result->options.max_evaluations - *evaluations;
    if (limits.max_evaluations == 0U ||
        hwa_run_file_read(source->path, &limits, &run,
                          hash, error, error_size) != 0 ||
        strcmp(hash, source->sha256) != 0) {
        hwa_run_result_free(&run);
        if (error != NULL && error_size != 0U && error[0] == '\0')
            hwa_set_error(error, error_size, "Stage 9 run hash mismatch");
        return -1;
    }
    nested_work = run.retained_work_bytes;
    if (hwa_gap_work_add(live_work, nested_work,
            result->options.max_work_bytes, error, error_size) != 0) {
        hwa_run_result_free(&run);
        return -1;
    }
    if (hwa_gap_evaluations_add(evaluations, run.evaluation_count,
            result->options.max_evaluations, error, error_size) != 0) {
        hwa_gap_work_sub(live_work, nested_work);
        hwa_run_result_free(&run);
        return -1;
    }
    adapter_visits = (uint64_t)run.feature_count;
    if (hwa_gap_add_u64(&adapter_visits,
                        (uint64_t)run.stage_count) != 0 ||
        hwa_gap_mul_u64(adapter_visits, UINT64_C(2),
                        &adapter_visits) != 0 ||
        hwa_gap_evaluations_add(evaluations, adapter_visits,
            result->options.max_evaluations, error, error_size) != 0) {
        hwa_gap_work_sub(live_work, nested_work);
        hwa_run_result_free(&run);
        return -1;
    }
    for (index = 0U; index < run.feature_count; ++index) {
        const HWARunFeature *row = &run.features[index];
        char metric_buffer[64];
        const char *metric = hwa_gap_run_metric(row->kind, row->index,
                                                 metric_buffer);
        uint64_t family_work = 0U;
        uint64_t case_work = 0U;
        char *family = hwa_gap_format_run_role(
            source->id, (int)row->role, metric, live_work,
            result->options.max_work_bytes, &family_work, error, error_size);
        char *case_id = family == NULL ? NULL : hwa_gap_format_case(
            "feature", row->id, live_work, result->options.max_work_bytes,
            &case_work, error, error_size);
        uint64_t candidate_id;
        int valid = row->availability == HWA_RUN_AVAILABLE && row->gap_valid &&
            isfinite(row->normalized_gap);
        double confidence = row->quality_flags == 0U ? 1.0 : 0.75;
        if (family == NULL || case_id == NULL ||
            hwa_gap_append_candidate(
                result, source->id, row->id, case_id, metric, family,
                HWA_GAP_REPORT_CANDIDATE_RUN_FEATURE,
                hwa_gap_run_availability(row->availability),
                valid ? hwa_gap_clamp(fabs(row->normalized_gap)) : 0.0, valid,
                hwa_gap_quality_normalize(row->quality_flags,
                                          confidence, valid),
                valid ? "" : "run-feature-unavailable",
                live_work, error, error_size, &candidate_id) != 0 ||
            hwa_gap_append_case(
                result, candidate_id, case_id,
                hwa_gap_run_availability(row->availability),
                valid ? hwa_gap_clamp(fabs(row->normalized_gap)) : 0.0, valid,
                confidence, valid, valid ? "" : "run-feature-unavailable",
                live_work, error, error_size) != 0) {
            free(family); free(case_id);
            hwa_gap_work_sub(live_work, case_work);
            hwa_gap_work_sub(live_work, family_work);
            hwa_gap_work_sub(live_work, nested_work);
            hwa_run_result_free(&run); return -1;
        }
        free(family); free(case_id);
        hwa_gap_work_sub(live_work, case_work);
        hwa_gap_work_sub(live_work, family_work);
    }
    for (index = 0U; index < run.stage_count; ++index) {
        const HWARunStage *row = &run.stages[index];
        uint64_t family_work = 0U;
        uint64_t case_work = 0U;
        char *family = hwa_gap_format_run_stage(
            source->id, (int)row->from_role, (int)row->to_role, live_work,
            result->options.max_work_bytes, &family_work, error, error_size);
        char *case_id = family == NULL ? NULL : hwa_gap_format_case(
            "stage", row->id, live_work, result->options.max_work_bytes,
            &case_work, error, error_size);
        uint64_t candidate_id;
        int valid = row->availability == HWA_RUN_AVAILABLE && row->gap_valid &&
            isfinite(row->added_gap) && row->added_gap > 0.0;
        HWAGapReportAvailability availability =
            row->availability == HWA_RUN_AVAILABLE && !valid ?
                HWA_GAP_REPORT_EXCLUDED :
                hwa_gap_run_availability(row->availability);
        double confidence = row->quality_flags == 0U ? 1.0 : 0.75;
        if (family == NULL || case_id == NULL ||
            hwa_gap_append_candidate(
                result, source->id, (uint64_t)run.feature_count + row->id,
                case_id, "other", family,
                HWA_GAP_REPORT_CANDIDATE_RUN_STAGE, availability,
                valid ? hwa_gap_clamp(row->added_gap) : 0.0, valid,
                hwa_gap_quality_normalize(row->quality_flags,
                                          confidence, valid),
                valid ? "" : "run-stage-not-positive", live_work,
                error, error_size, &candidate_id) != 0 ||
            hwa_gap_append_case(
                result, candidate_id, case_id, availability,
                valid ? hwa_gap_clamp(row->added_gap) : 0.0,
                valid, confidence, valid,
                valid ? "" : "run-stage-not-positive", live_work,
                error, error_size) != 0) {
            free(family); free(case_id);
            hwa_gap_work_sub(live_work, case_work);
            hwa_gap_work_sub(live_work, family_work);
            hwa_gap_work_sub(live_work, nested_work);
            hwa_run_result_free(&run); return -1;
        }
        free(family); free(case_id);
        hwa_gap_work_sub(live_work, case_work);
        hwa_gap_work_sub(live_work, family_work);
    }
    hwa_gap_work_sub(live_work, nested_work);
    hwa_run_result_free(&run);
    return 0;
}

static int hwa_gap_double_compare(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static int hwa_gap_adapt_experiment(HWAGapReportResult *result,
                                    const HWAGapReportSource *source,
                                    uint64_t *live_work,
                                    uint64_t *evaluations,
                                    char *error,
                                    size_t error_size)
{
    HWAExperimentResult experiment;
    HWAExperimentOptions limits = result->options.experiment;
    char hash[HWA_SHA256_HEX_SIZE];
    size_t index;
    uint64_t visits;
    uint64_t nested_work = 0U;
    uint64_t sort_visits = 0U;
    memset(&experiment, 0, sizeof(experiment));
    if (*live_work >= result->options.max_work_bytes) return -1;
    if (limits.max_work_bytes > result->options.max_work_bytes - *live_work)
        limits.max_work_bytes = result->options.max_work_bytes - *live_work;
    if (limits.run.max_work_bytes > limits.max_work_bytes)
        limits.run.max_work_bytes = limits.max_work_bytes;
    if (limits.max_total_run_evaluations >
        result->options.max_evaluations - *evaluations)
        limits.max_total_run_evaluations =
            result->options.max_evaluations - *evaluations;
    if (limits.run.max_evaluations > limits.max_total_run_evaluations)
        limits.run.max_evaluations = limits.max_total_run_evaluations;
    if (limits.max_total_run_evaluations == 0U ||
        hwa_experiment_file_read(source->path, &limits,
                                 &experiment, hash, error, error_size) != 0 ||
        strcmp(hash, source->sha256) != 0) {
        hwa_experiment_result_free(&experiment);
        if (error != NULL && error_size != 0U && error[0] == '\0')
            hwa_set_error(error, error_size, "Stage 9 experiment hash mismatch");
        return -1;
    }
    nested_work = experiment.retained_work_bytes;
    if (hwa_gap_work_add(live_work, nested_work,
            result->options.max_work_bytes, error, error_size) != 0) {
        hwa_experiment_result_free(&experiment);
        return -1;
    }
    if (hwa_gap_evaluations_add(evaluations,
            experiment.total_run_evaluations,
            result->options.max_evaluations, error, error_size) != 0 ||
        (visits = (uint64_t)experiment.job_count,
         hwa_gap_add_u64(&visits, UINT64_C(1))) != 0 ||
        hwa_gap_mul_u64((uint64_t)experiment.job_count,
            hwa_gap_log_steps(experiment.job_count), &sort_visits) != 0 ||
        hwa_gap_add_u64(&visits, sort_visits) != 0 ||
        hwa_gap_mul_u64(visits,
            (uint64_t)experiment.case_count, &visits) != 0 ||
        hwa_gap_add_u64(&visits, UINT64_C(1)) != 0 ||
        hwa_gap_mul_u64(visits,
            (uint64_t)experiment.candidate_count, &visits) != 0 ||
        hwa_gap_evaluations_add(evaluations, visits,
            result->options.max_evaluations, error, error_size) != 0) {
        hwa_gap_work_sub(live_work, nested_work);
        hwa_experiment_result_free(&experiment);
        return -1;
    }
    for (index = 0U; index < experiment.candidate_count; ++index) {
        const HWAExperimentCandidate *row = &experiment.candidates[index];
        const HWAExperimentResponse *response;
        char metric_buffer[64];
        const char *metric;
        char *family;
        uint64_t family_work = 0U;
        uint64_t candidate_id;
        size_t case_index;
        int valid;
        if (row->point_id != 1U || row->split != HWA_EXPERIMENT_CHECK)
            continue;
        response = &experiment.responses[row->response_id - 1U];
        metric = hwa_gap_run_metric(response->feature, response->feature_index,
                                    metric_buffer);
        family = hwa_gap_format_experiment_role(
            source->id, (int)response->role, metric, live_work,
            result->options.max_work_bytes, &family_work, error, error_size);
        valid = row->availability == HWA_RUN_AVAILABLE && row->values_valid &&
            isfinite(row->mean_gap);
        if (family == NULL || hwa_gap_append_candidate(
                result, source->id, row->id, "baseline-check", metric, family,
                HWA_GAP_REPORT_CANDIDATE_EXPERIMENT,
                hwa_gap_run_availability(row->availability),
                valid ? hwa_gap_clamp(fabs(row->mean_gap)) : 0.0, valid,
                hwa_gap_quality_normalize(row->quality_flags,
                    valid ? 1.0 : 0.0, valid),
                valid ? "" : "experiment-candidate-unavailable", live_work,
                error, error_size, &candidate_id) != 0) {
            free(family); hwa_gap_work_sub(live_work, family_work);
            hwa_gap_work_sub(live_work, nested_work);
            hwa_experiment_result_free(&experiment); return -1;
        }
        free(family);
        hwa_gap_work_sub(live_work, family_work);
        for (case_index = 0U; case_index < experiment.case_count; ++case_index) {
            const HWAExperimentCase *case_row = &experiment.cases[case_index];
            double *values;
            size_t count = 0U;
            size_t job_index;
            size_t eligible = experiment.plan_replicates;
            if (case_row->split != HWA_EXPERIMENT_CHECK) continue;
            if (eligible > SIZE_MAX / sizeof(*values) ||
                hwa_gap_work_add(live_work,
                    (uint64_t)eligible * (uint64_t)sizeof(*values),
                    result->options.max_work_bytes,
                    error, error_size) != 0) {
                hwa_gap_work_sub(live_work, nested_work);
                hwa_experiment_result_free(&experiment);
                return -1;
            }
            values = eligible == 0U ? NULL :
                (double *)malloc(eligible * sizeof(*values));
            if (eligible != 0U && values == NULL) {
                hwa_gap_work_sub(live_work,
                    (uint64_t)eligible * (uint64_t)sizeof(*values));
                hwa_gap_work_sub(live_work, nested_work);
                hwa_experiment_result_free(&experiment);
                return -1;
            }
            for (job_index = 0U; job_index < experiment.job_count; ++job_index) {
                const HWAExperimentJob *job = &experiment.jobs[job_index];
                size_t observation_index;
                if (job->point_id != 1U || job->case_id != case_row->id) continue;
                observation_index = job_index * experiment.response_count +
                    (size_t)(response->id - 1U);
                if (observation_index < experiment.observation_count) {
                    const HWAExperimentObservation *fact =
                        &experiment.observations[observation_index];
                    if (fact->availability == HWA_RUN_AVAILABLE && fact->value_valid &&
                        isfinite(fact->value))
                        values[count++] = hwa_gap_clamp(fabs(fact->value));
                }
            }
            if (count != 0U) qsort(values, count, sizeof(*values),
                                   hwa_gap_double_compare);
            if (hwa_gap_append_case(
                    result, candidate_id, case_row->name,
                    count == 0U ? HWA_GAP_REPORT_UNAVAILABLE :
                                  HWA_GAP_REPORT_AVAILABLE,
                    count == 0U ? 0.0 :
                        (count & 1U ? values[count / 2U] :
                         (values[count / 2U - 1U] + values[count / 2U]) * 0.5),
                    count != 0U,
                    eligible == 0U ? 0.0 : (double)count / (double)eligible,
                    eligible != 0U,
                    count == 0U ? "experiment-case-unavailable" : "",
                    live_work, error, error_size) != 0) {
                free(values);
                hwa_gap_work_sub(live_work,
                    (uint64_t)eligible * (uint64_t)sizeof(*values));
                hwa_gap_work_sub(live_work, nested_work);
                hwa_experiment_result_free(&experiment); return -1;
            }
            free(values);
            hwa_gap_work_sub(live_work,
                (uint64_t)eligible * (uint64_t)sizeof(*values));
        }
    }
    hwa_gap_work_sub(live_work, nested_work);
    hwa_experiment_result_free(&experiment);
    return 0;
}

static int hwa_gap_candidate_compare(const void *left, const void *right)
{
    const HWAGapReportCandidate *a = (const HWAGapReportCandidate *)left;
    const HWAGapReportCandidate *b = (const HWAGapReportCandidate *)right;
    int order = strcmp(a->family_key, b->family_key);
    if (order != 0) return order;
    if (a->source_id != b->source_id) return a->source_id < b->source_id ? -1 : 1;
    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
    if (a->source_row != b->source_row)
        return a->source_row < b->source_row ? -1 : 1;
    return a->id < b->id ? -1 : a->id > b->id ? 1 : 0;
}

static int hwa_gap_candidate_pointer_family_compare(const void *left,
                                                    const void *right)
{
    const HWAGapReportCandidate *a =
        *(HWAGapReportCandidate *const *)left;
    const HWAGapReportCandidate *b =
        *(HWAGapReportCandidate *const *)right;
    return hwa_gap_candidate_compare(a, b);
}

static int hwa_gap_rank_pointer_compare(const void *left, const void *right)
{
    const HWAGapReportCandidate *a =
        *(HWAGapReportCandidate *const *)left;
    const HWAGapReportCandidate *b =
        *(HWAGapReportCandidate *const *)right;
    if (a->score != b->score) return a->score > b->score ? -1 : 1;
    if (a->size_factor != b->size_factor)
        return a->size_factor > b->size_factor ? -1 : 1;
    if (a->occurrence_factor != b->occurrence_factor)
        return a->occurrence_factor > b->occurrence_factor ? -1 : 1;
    if (a->confidence_factor != b->confidence_factor)
        return a->confidence_factor > b->confidence_factor ? -1 : 1;
    return hwa_gap_candidate_compare(a, b);
}

static double hwa_gap_audibility(const char *metric, int *valid)
{
    static const double band[HWA_BAND_COUNT] = {
        0.25, 0.35, 0.55, 0.75, 0.90, 1.0, 1.0, 0.90, 0.70, 0.45
    };
    unsigned long index;
    char *end = NULL;
    *valid = 1;
    if (strcmp(metric, "level") == 0) return 0.85;
    if (strcmp(metric, "crest") == 0) return 0.75;
    if (strcmp(metric, "pitch") == 0 || strcmp(metric, "timing") == 0)
        return 1.0;
    if (strcmp(metric, "other") == 0) return 0.70;
    if (strncmp(metric, "band:", 5U) == 0) {
        errno = 0;
        index = strtoul(metric + 5U, &end, 10);
        if (errno == 0 && end != NULL && *end == '\0' && index < HWA_BAND_COUNT)
            return band[index];
    }
    *valid = 0;
    return 0.0;
}

static int hwa_gap_quantile_compare(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static double hwa_gap_quantile(double *values, size_t count, double q)
{
    double position;
    size_t low;
    size_t high;
    qsort(values, count, sizeof(*values), hwa_gap_quantile_compare);
    if (count == 1U) return values[0];
    position = q * (double)(count - 1U);
    low = (size_t)floor(position);
    high = (size_t)ceil(position);
    return values[low] + (values[high] - values[low]) * (position - (double)low);
}

static void hwa_gap_derived_free(HWAGapReportResult *result)
{
    size_t index;
    for (index = 0U; index < result->family_count; ++index)
        free(result->families[index].key);
    for (index = 0U; index < result->group_count; ++index)
        free(result->groups[index].value);
    free(result->families);
    free(result->groups);
    result->families = NULL;
    result->family_count = 0U;
    result->groups = NULL;
    result->group_count = 0U;
}

static void hwa_gap_warnings_free(HWAGapReportResult *result)
{
    size_t index;
    for (index = 0U; index < result->warning_count; ++index) {
        free(result->warnings[index].code);
        free(result->warnings[index].message);
    }
    free(result->warnings);
    result->warnings = NULL;
    result->warning_count = 0U;
}

static int hwa_gap_warning_add(HWAGapReportResult *result,
                               const char *code,
                               const char *message,
                               uint64_t source_id,
                               uint64_t candidate_id,
                               uint64_t excerpt_id)
{
    HWAGapReportWarning *grown;
    HWAGapReportWarning *warning;
    if (result->warning_count >= result->options.max_warnings ||
        result->warning_count >= SIZE_MAX / sizeof(*result->warnings))
        return -1;
    grown = (HWAGapReportWarning *)realloc(
        result->warnings,
        (result->warning_count + 1U) * sizeof(*result->warnings));
    if (grown == NULL) return -1;
    result->warnings = grown;
    warning = &grown[result->warning_count];
    memset(warning, 0, sizeof(*warning));
    warning->id = (uint64_t)result->warning_count + UINT64_C(1);
    warning->code = hwa_gap_strdup(code);
    warning->message = hwa_gap_strdup(message);
    if (warning->code == NULL || warning->message == NULL) {
        free(warning->code);
        free(warning->message);
        memset(warning, 0, sizeof(*warning));
        return -1;
    }
    warning->source_id = source_id;
    warning->candidate_id = candidate_id;
    warning->excerpt_id = excerpt_id;
    warning->source_id_valid = source_id != 0U;
    warning->candidate_id_valid = candidate_id != 0U;
    warning->excerpt_id_valid = excerpt_id != 0U;
    result->warning_count++;
    return 0;
}

int hwa_gap_report_warnings_rebuild(HWAGapReportResult *result,
                                    char *error,
                                    size_t error_size)
{
    size_t index;
    if (result == NULL) return -1;
    hwa_gap_warnings_free(result);
    for (index = 0U; index < result->source_count; ++index)
        if (result->sources[index].file_bytes == 0U &&
            hwa_gap_warning_add(result, "source-unavailable",
                "source evidence is unavailable", result->sources[index].id,
                0U, 0U) != 0) goto failed;
    for (index = 0U; index < result->candidate_count; ++index) {
        const HWAGapReportCandidate *candidate = &result->candidates[index];
        if (candidate->primary && !candidate->score_valid &&
            candidate->availability != HWA_GAP_REPORT_EXCLUDED &&
            hwa_gap_warning_add(result, "candidate-unranked",
                "candidate has no complete rank score", candidate->source_id,
                candidate->id, 0U) != 0) goto failed;
    }
    for (index = 0U; index < result->candidate_count; ++index) {
        const HWAGapReportCandidate *candidate = &result->candidates[index];
        if ((candidate->quality_flags &
             HWA_GAP_REPORT_QUALITY_LINKED_SECONDARY) != 0U &&
            hwa_gap_warning_add(result, "linked-secondary",
                "candidate stays as linked secondary evidence",
                candidate->source_id, candidate->id, 0U) != 0) goto failed;
    }
    for (index = 0U; index < result->candidate_count; ++index) {
        const HWAGapReportCandidate *candidate = &result->candidates[index];
        if ((candidate->quality_flags &
             HWA_GAP_REPORT_QUALITY_MISSING_LABEL) != 0U &&
            hwa_gap_warning_add(result, "missing-label",
                "candidate has missing case label axes", candidate->source_id,
                candidate->id, 0U) != 0) goto failed;
    }
    for (index = 0U; index < result->excerpt_count; ++index) {
        const HWAGapReportExcerpt *excerpt = &result->excerpts[index];
        if (excerpt->availability != HWA_GAP_REPORT_AVAILABLE &&
            hwa_gap_warning_add(result, "excerpt-failure",
                "excerpt evidence is unavailable", 0U, 0U,
                excerpt->id) != 0) goto failed;
    }
    return 0;
failed:
    hwa_gap_warnings_free(result);
    hwa_set_error(error, error_size,
                  "Stage 9 warning cap or allocation exceeded");
    return -1;
}

static const HWAGapReportLabel *hwa_gap_label_find(
    const HWAGapReportResult *result,
    uint64_t source_id,
    const char *case_id)
{
    size_t low = 0U;
    size_t high = result->label_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        const HWAGapReportLabel *label = &result->labels[middle];
        int order = label->source_id < source_id ? -1 :
            label->source_id > source_id ? 1 : strcmp(label->case_id, case_id);
        if (order < 0) low = middle + 1U;
        else if (order > 0) high = middle;
        else return label;
    }
    return NULL;
}

static void hwa_gap_case_range(const HWAGapReportResult *result,
                               uint64_t candidate_id,
                               size_t *begin,
                               size_t *end)
{
    size_t low = 0U;
    size_t high = result->case_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        if (result->cases[middle].candidate_id < candidate_id) low = middle + 1U;
        else high = middle;
    }
    *begin = low;
    while (low < result->case_count &&
           result->cases[low].candidate_id == candidate_id) low++;
    *end = low;
}

static const char *hwa_gap_label_value(const HWAGapReportLabel *label,
                                       HWAGapReportAxis axis)
{
    const char *value = NULL;
    if (label != NULL) {
        if (axis == HWA_GAP_REPORT_AXIS_PITCH) value = label->pitch;
        else if (axis == HWA_GAP_REPORT_AXIS_REGISTER)
            value = label->register_name;
        else if (axis == HWA_GAP_REPORT_AXIS_DYNAMIC) value = label->dynamic;
        else if (axis == HWA_GAP_REPORT_AXIS_GESTURE) value = label->gesture;
        else if (axis == HWA_GAP_REPORT_AXIS_PHYSICAL_ELEMENT)
            value = label->physical_element;
        else if (axis == HWA_GAP_REPORT_AXIS_SECTION) value = label->section;
    }
    return value == NULL || value[0] == '\0' ? HWA_GAP_MISSING_LABEL : value;
}

static int hwa_gap_group_compare(const void *left, const void *right)
{
    const HWAGapReportGroup *a = (const HWAGapReportGroup *)left;
    const HWAGapReportGroup *b = (const HWAGapReportGroup *)right;
    if (a->axis != b->axis) return a->axis < b->axis ? -1 : 1;
    return strcmp(a->value, b->value);
}

static uint64_t hwa_gap_log_steps(size_t count)
{
    uint64_t steps = 1U;
    size_t value = count;
    while (value > 1U) { value = (value + 1U) / 2U; steps++; }
    return steps;
}

static int hwa_gap_evaluations_add(uint64_t *evaluations,
                                   uint64_t add,
                                   uint64_t maximum,
                                   char *error,
                                   size_t error_size)
{
    if (add > maximum || *evaluations > maximum - add) {
        hwa_set_error(error, error_size, "Stage 9 evaluation cap exceeded");
        return -1;
    }
    *evaluations += add;
    return 0;
}

static int hwa_gap_evaluations_product(uint64_t *evaluations,
                                       uint64_t left,
                                       uint64_t right,
                                       uint64_t maximum,
                                       char *error,
                                       size_t error_size)
{
    uint64_t product;
    if (hwa_gap_mul_u64(left, right, &product) != 0) {
        hwa_set_error(error, error_size, "Stage 9 evaluation cap exceeded");
        return -1;
    }
    return hwa_gap_evaluations_add(evaluations, product, maximum,
                                   error, error_size);
}

static HWAGapReportGroup *hwa_gap_group_add(HWAGapReportResult *result,
                                             HWAGapReportAxis axis,
                                             const char *value,
                                             char *error,
                                             size_t error_size)
{
    HWAGapReportGroup *grown;
    size_t index;
    for (index = 0U; index < result->group_count; ++index)
        if (result->groups[index].axis == axis &&
            strcmp(result->groups[index].value, value) == 0)
            return &result->groups[index];
    if (result->group_count >= result->options.max_groups ||
        result->group_count >= SIZE_MAX / sizeof(*result->groups)) {
        hwa_set_error(error, error_size, "Stage 9 group cap exceeded");
        return NULL;
    }
    grown = (HWAGapReportGroup *)realloc(
        result->groups, (result->group_count + 1U) * sizeof(*result->groups));
    if (grown == NULL) return NULL;
    result->groups = grown;
    memset(&grown[result->group_count], 0, sizeof(*grown));
    grown[result->group_count].axis = axis;
    grown[result->group_count].value = hwa_gap_strdup(value);
    if (grown[result->group_count].value == NULL) return NULL;
    result->group_count++;
    return &grown[result->group_count - 1U];
}

static int hwa_gap_groups_rebuild(HWAGapReportResult *result,
                                  uint64_t *evaluations,
                                  char *error,
                                  size_t error_size)
{
    size_t candidate_index;
    size_t group_index;
    uint64_t discovery;
    uint64_t linear_visits;
    uint64_t stats_per_group;
    uint64_t label_visits;
    uint64_t label_steps = hwa_gap_log_steps(result->label_count);
    if (hwa_gap_mul_u64(
            UINT64_C(6),
            (uint64_t)result->case_count + (uint64_t)result->candidate_count,
            &discovery) != 0 ||
        discovery == UINT64_MAX ||
        hwa_gap_mul_u64(discovery, discovery + UINT64_C(1),
                        &linear_visits) != 0 ||
        hwa_gap_evaluations_add(evaluations, discovery,
            result->options.max_evaluations, error, error_size) != 0 ||
        hwa_gap_evaluations_add(evaluations, linear_visits / UINT64_C(2),
            result->options.max_evaluations, error, error_size) != 0)
        return -1;
    for (candidate_index = 0U; candidate_index < result->candidate_count;
         ++candidate_index) {
        const HWAGapReportCandidate *candidate =
            &result->candidates[candidate_index];
        HWAGapReportAxis axis;
        if (!candidate->primary) continue;
        for (axis = HWA_GAP_REPORT_AXIS_PITCH;
             axis < HWA_GAP_REPORT_AXIS_COUNT;
             axis = (HWAGapReportAxis)((int)axis + 1)) {
            size_t case_index;
            size_t case_end;
            int any = 0;
            hwa_gap_case_range(result, candidate->id, &case_index, &case_end);
            for (; case_index < case_end; ++case_index) {
                const HWAGapReportCase *record = &result->cases[case_index];
                const HWAGapReportLabel *label;
                const char *value;
                if (record->candidate_id != candidate->id) continue;
                label = hwa_gap_label_find(result, candidate->source_id,
                                           record->case_id);
                value = hwa_gap_label_value(label, axis);
                if (hwa_gap_group_add(result, axis, value,
                                      error, error_size) == NULL) return -1;
                any = 1;
            }
            if (!any && hwa_gap_group_add(result, axis, HWA_GAP_MISSING_LABEL,
                                           error, error_size) == NULL) return -1;
        }
    }
    qsort(result->groups, result->group_count, sizeof(*result->groups),
          hwa_gap_group_compare);
    if (hwa_gap_mul_u64((uint64_t)result->case_count, label_steps,
                        &label_visits) != 0 ||
        hwa_gap_add_u64(&label_visits,
                        (uint64_t)result->candidate_count) != 0) {
        hwa_set_error(error, error_size, "Stage 9 evaluation cap exceeded");
        return -1;
    }
    stats_per_group = label_visits;
    if (hwa_gap_evaluations_product(
            evaluations, (uint64_t)result->group_count,
            stats_per_group,
            result->options.max_evaluations, error, error_size) != 0)
        return -1;
    for (group_index = 0U; group_index < result->group_count; ++group_index) {
        HWAGapReportGroup *group = &result->groups[group_index];
        double *values = result->candidate_count == 0U ? NULL :
            (double *)malloc(result->candidate_count * sizeof(*values));
        size_t value_count = 0U;
        long double confidence = 0.0L;
        size_t confidence_count = 0U;
        if (result->candidate_count != 0U && values == NULL) return -1;
        group->id = (uint64_t)group_index + UINT64_C(1);
        for (candidate_index = 0U; candidate_index < result->candidate_count;
             ++candidate_index) {
            const HWAGapReportCandidate *candidate =
                &result->candidates[candidate_index];
            size_t case_index;
            size_t case_end;
            int saw = 0;
            int saw_score = 0;
            double best_score = 0.0;
            double best_confidence = 0.0;
            if (!candidate->primary) continue;
            hwa_gap_case_range(result, candidate->id, &case_index, &case_end);
            if (case_index == case_end &&
                strcmp(group->value, HWA_GAP_MISSING_LABEL) == 0) saw = 1;
            for (; case_index < case_end; ++case_index) {
                const HWAGapReportCase *record = &result->cases[case_index];
                const HWAGapReportLabel *label = hwa_gap_label_find(
                    result, candidate->source_id, record->case_id);
                if (strcmp(hwa_gap_label_value(label, group->axis),
                           group->value) != 0) continue;
                saw = 1;
                if (record->score_valid &&
                    (!saw_score || record->score > best_score)) {
                    saw_score = 1;
                    best_score = record->score;
                    best_confidence = record->confidence_valid ?
                        record->confidence : 0.0;
                }
            }
            if (!saw) continue;
            group->candidate_count++;
            group->family_count++;
            if (candidate->availability == HWA_GAP_REPORT_EXCLUDED)
                group->excluded_count++;
            else if (!saw_score) group->missing_count++;
            else {
                group->available_count++;
                values[value_count++] = best_score;
                if (best_confidence >= 0.0 && best_confidence <= 1.0) {
                    confidence += best_confidence;
                    confidence_count++;
                }
            }
        }
        if (value_count != 0U) {
            double *copy = (double *)malloc(value_count * sizeof(*copy));
            if (copy == NULL) { free(values); return -1; }
#define HWA_GAP_GROUP_Q(field_, q_)                                         \
            memcpy(copy, values, value_count * sizeof(*copy));              \
            group->field_ = hwa_gap_quantile(copy, value_count, (q_))
            HWA_GAP_GROUP_Q(q05, 0.05);
            HWA_GAP_GROUP_Q(q25, 0.25);
            HWA_GAP_GROUP_Q(median, 0.50);
            HWA_GAP_GROUP_Q(q75, 0.75);
            HWA_GAP_GROUP_Q(q95, 0.95);
#undef HWA_GAP_GROUP_Q
            group->spread = group->q95 - group->q05;
            group->statistics_valid = 1;
            free(copy);
        }
        if (confidence_count != 0U) {
            group->confidence =
                (double)(confidence / (long double)confidence_count);
            group->confidence_valid = 1;
        }
        free(values);
    }
    return 0;
}

static int hwa_gap_report_result_rebuild_impl(HWAGapReportResult *result,
                                               uint64_t starting_evaluations,
                                               uint64_t extra_peak_bytes,
                                               char *error,
                                               size_t error_size)
{
    HWAGapReportCandidate **family_order = NULL;
    HWAGapReportCandidate **ranked = NULL;
    size_t index;
    size_t ranked_count = 0U;
    uint64_t evaluations = starting_evaluations;
    uint64_t peak_work = 0U;
    uint64_t label_steps;
    if (result == NULL || result->candidate_count > result->options.max_candidates ||
        result->case_count > result->options.max_cases) {
        hwa_set_error(error, error_size, "invalid Stage 9 rebuild input");
        return -1;
    }
    if (hwa_gap_report_options_validate(&result->options,
                                         error, error_size) != 0 ||
        hwa_gap_report_result_peak_work_bytes(
            result, extra_peak_bytes, &peak_work) != 0 ||
        peak_work > result->options.max_work_bytes) {
        if (error != NULL && error_size != 0U && error[0] == '\0')
            hwa_set_error(error, error_size,
                          "Stage 9 rebuild exceeds its work cap");
        return -1;
    }
    label_steps = hwa_gap_log_steps(result->label_count);
    if (hwa_gap_evaluations_product(&evaluations,
            (uint64_t)result->case_count,
            hwa_gap_log_steps(result->case_count) + label_steps + UINT64_C(2),
            result->options.max_evaluations, error, error_size) != 0 ||
        hwa_gap_evaluations_product(&evaluations,
            (uint64_t)result->candidate_count,
            hwa_gap_log_steps(result->candidate_count) + UINT64_C(2),
            result->options.max_evaluations, error, error_size) != 0)
        return -1;
    hwa_gap_derived_free(result);
    for (index = 0U; index < result->case_count; ++index) {
        if (result->cases[index].id != (uint64_t)index + UINT64_C(1) ||
            (index != 0U && result->cases[index - 1U].candidate_id >
                               result->cases[index].candidate_id)) {
            hwa_set_error(error, error_size,
                          "Stage 9 cases are not in stable ID order");
            return -1;
        }
    }
    for (index = 0U; index < result->candidate_count; ++index) {
        HWAGapReportCandidate *candidate = &result->candidates[index];
        size_t scan;
        size_t scan_end;
        uint64_t eligible = 0U;
        uint64_t occurrence = 0U;
        long double confidence = 0.0L;
        uint64_t confidence_count = 0U;
        int audible_valid;
        if (candidate->id != (uint64_t)index + UINT64_C(1)) {
            hwa_set_error(error, error_size, "invalid Stage 9 candidate IDs");
            return -1;
        }
        candidate->linked_family_id = 0U;
        candidate->rank = 0U;
        candidate->primary = 0;
        candidate->score_valid = 0;
        candidate->quality_flags &= HWA_GAP_REPORT_QUALITY_LOW_CONFIDENCE |
            HWA_GAP_REPORT_QUALITY_SOURCE_WARNING;
        candidate->size_valid = candidate->raw_value_valid &&
            isfinite(candidate->raw_value);
        candidate->size_factor = candidate->size_valid ?
            hwa_gap_clamp(fabs(candidate->raw_value)) : 0.0;
        candidate->audibility_factor = hwa_gap_audibility(
            candidate->metric, &audible_valid);
        candidate->audibility_valid = audible_valid;
        hwa_gap_case_range(result, candidate->id, &scan, &scan_end);
        for (; scan < scan_end; ++scan) {
            HWAGapReportCase *record = &result->cases[scan];
            double case_confidence;
            double case_occurrence;
            if (record->candidate_id != candidate->id) continue;
            record->score = 0.0;
            record->score_valid = 0;
            if (record->availability == HWA_GAP_REPORT_AVAILABLE &&
                record->value_valid && isfinite(record->value) &&
                record->confidence_valid && isfinite(record->confidence) &&
                record->confidence >= 0.0 && record->confidence <= 1.0 &&
                candidate->audibility_valid) {
                case_confidence = hwa_gap_clamp(record->confidence /
                    (1.0 + (double)hwa_gap_popcount(
                        candidate->quality_flags)));
                case_occurrence = fabs(record->value) >= HWA_GAP_THRESHOLD ?
                    1.0 : 0.0;
                record->score = hwa_gap_clamp(fabs(record->value)) *
                    candidate->audibility_factor * case_occurrence *
                    case_confidence;
                record->score_valid = isfinite(record->score);
            }
            if (record->availability == HWA_GAP_REPORT_EXCLUDED) continue;
            eligible++;
            if (record->value_valid && isfinite(record->value) &&
                fabs(record->value) >= HWA_GAP_THRESHOLD) occurrence++;
            if (record->confidence_valid && isfinite(record->confidence) &&
                record->confidence >= 0.0 && record->confidence <= 1.0) {
                confidence += record->confidence;
                confidence_count++;
            }
        }
        candidate->eligible_count = eligible;
        candidate->occurrence_count = occurrence;
        candidate->occurrence_valid = eligible != 0U;
        candidate->occurrence_factor = eligible == 0U ? 0.0 :
            (double)occurrence / (double)eligible;
        candidate->confidence_valid = confidence_count != 0U;
        candidate->confidence_factor = confidence_count == 0U ? 0.0 :
            hwa_gap_clamp((double)(confidence / (long double)confidence_count) /
                (1.0 + (double)hwa_gap_popcount(candidate->quality_flags)));
        if (candidate->availability == HWA_GAP_REPORT_AVAILABLE &&
            candidate->size_valid && candidate->audibility_valid &&
            candidate->occurrence_valid && candidate->confidence_valid) {
            candidate->score = candidate->size_factor *
                candidate->audibility_factor * candidate->occurrence_factor *
                candidate->confidence_factor;
            candidate->score_valid = isfinite(candidate->score);
        }
        if (!candidate->audibility_valid)
            candidate->quality_flags |=
                HWA_GAP_REPORT_QUALITY_AUDIBILITY_UNAVAILABLE;
        if (!candidate->occurrence_valid)
            candidate->quality_flags |=
                HWA_GAP_REPORT_QUALITY_OCCURRENCE_UNAVAILABLE;
        hwa_gap_case_range(result, candidate->id, &scan, &scan_end);
        for (; scan < scan_end; ++scan) {
            const HWAGapReportCase *record = &result->cases[scan];
            const HWAGapReportLabel *label;
            if (record->candidate_id != candidate->id) continue;
            label = hwa_gap_label_find(result, candidate->source_id,
                                       record->case_id);
            if (label == NULL || label->pitch[0] == '\0' ||
                label->register_name[0] == '\0' || label->dynamic[0] == '\0' ||
                label->gesture[0] == '\0' ||
                label->physical_element[0] == '\0' || label->section[0] == '\0') {
                candidate->quality_flags |= HWA_GAP_REPORT_QUALITY_MISSING_LABEL;
                break;
            }
        }
    }
    family_order = result->candidate_count == 0U ? NULL :
        (HWAGapReportCandidate **)malloc(
            result->candidate_count * sizeof(*family_order));
    if (result->candidate_count != 0U && family_order == NULL) return -1;
    for (index = 0U; index < result->candidate_count; ++index)
        family_order[index] = &result->candidates[index];
    qsort(family_order, result->candidate_count, sizeof(*family_order),
          hwa_gap_candidate_pointer_family_compare);
    for (index = 0U; index < result->candidate_count;) {
        size_t end = index + 1U;
        size_t best = index;
        HWAGapReportFamily *grown;
        while (end < result->candidate_count &&
               family_order[index]->source_id ==
                   family_order[end]->source_id &&
               strcmp(family_order[index]->family_key,
                      family_order[end]->family_key) == 0) end++;
        if (result->family_count >= result->options.max_families) {
            hwa_set_error(error, error_size, "Stage 9 family cap exceeded");
            return -1;
        }
        grown = (HWAGapReportFamily *)realloc(
            result->families,
            (result->family_count + 1U) * sizeof(*result->families));
        if (grown == NULL) return -1;
        result->families = grown;
        memset(&grown[result->family_count], 0, sizeof(*grown));
        grown[result->family_count].id =
            (uint64_t)result->family_count + UINT64_C(1);
        grown[result->family_count].key =
            hwa_gap_strdup(family_order[index]->family_key);
        grown[result->family_count].member_count = end - index;
        if (grown[result->family_count].key == NULL) return -1;
        for (; index < end; ++index) {
            HWAGapReportCandidate *candidate = family_order[index];
            candidate->linked_family_id = grown[result->family_count].id;
            if (candidate->score_valid &&
                (!family_order[best]->score_valid ||
                 candidate->score > family_order[best]->score)) best = index;
        }
        family_order[best]->primary = 1;
        grown[result->family_count].primary_candidate_id =
            family_order[best]->id;
        result->family_count++;
    }
    free(family_order);
    family_order = NULL;
    ranked = result->family_count == 0U ? NULL :
        (HWAGapReportCandidate **)malloc(result->family_count * sizeof(*ranked));
    if (result->family_count != 0U && ranked == NULL) return -1;
    for (index = 0U; index < result->candidate_count; ++index)
        if (result->candidates[index].primary &&
            result->candidates[index].score_valid)
            ranked[ranked_count++] = &result->candidates[index];
        else if (!result->candidates[index].primary &&
                 result->candidates[index].linked_family_id != 0U)
            result->candidates[index].quality_flags |=
                HWA_GAP_REPORT_QUALITY_LINKED_SECONDARY;
    qsort(ranked, ranked_count, sizeof(*ranked), hwa_gap_rank_pointer_compare);
    for (index = 0U; index < ranked_count; ++index) {
        ranked[index]->rank = index + 1U;
        result->families[ranked[index]->linked_family_id - 1U].rank = index + 1U;
    }
    free(ranked);
    if (hwa_gap_groups_rebuild(result, &evaluations,
                               error, error_size) != 0) return -1;
    {
        uint64_t warning_visits;
        if (hwa_gap_mul_u64((uint64_t)result->candidate_count,
                            UINT64_C(3), &warning_visits) != 0 ||
            hwa_gap_add_u64(&warning_visits,
                            (uint64_t)result->source_count) != 0 ||
            hwa_gap_add_u64(&warning_visits,
                            (uint64_t)result->excerpt_count) != 0 ||
            hwa_gap_evaluations_add(&evaluations, warning_visits,
                result->options.max_evaluations, error, error_size) != 0)
            return -1;
    }
    if (hwa_gap_report_warnings_rebuild(result, error, error_size) != 0)
        return -1;
    result->evaluation_count = evaluations;
    return 0;
}

int hwa_gap_report_result_rebuild(HWAGapReportResult *result,
                                  char *error,
                                  size_t error_size)
{
    uint64_t starting = 0U;
    if (result == NULL ||
        hwa_gap_add_u64(&starting, (uint64_t)result->source_count) != 0 ||
        hwa_gap_add_u64(&starting, (uint64_t)result->candidate_count) != 0 ||
        hwa_gap_add_u64(&starting, (uint64_t)result->case_count) != 0 ||
        (result->mode != HWA_GAP_REPORT_RANK &&
         hwa_gap_add_u64(&starting, (uint64_t)result->excerpt_count) != 0)) {
        hwa_set_error(error, error_size,
                      "Stage 9 evaluation count overflow");
        return -1;
    }
    return hwa_gap_report_result_rebuild_impl(result, starting, 0U,
                                               error, error_size);
}

static int hwa_gap_source_order_valid(const HWAGapManifest *manifest)
{
    size_t index;
    for (index = 0U; index < manifest->source_count; ++index) {
        if (index != 0U && strcmp(manifest->sources[index - 1U].name,
                                  manifest->sources[index].name) >= 0)
            return 0;
    }
    return 1;
}

static int hwa_gap_label_order_valid(const HWAGapManifest *manifest)
{
    size_t index;
    for (index = 1U; index < manifest->label_count; ++index) {
        const HWAGapLabelDecl *left = &manifest->labels[index - 1U];
        const HWAGapLabelDecl *right = &manifest->labels[index];
        int order = strcmp(left->source, right->source);
        if (order > 0 || (order == 0 &&
                          strcmp(left->case_id, right->case_id) >= 0))
            return 0;
    }
    return 1;
}

static int hwa_gap_excerpt_order_valid(const HWAGapManifest *manifest)
{
    size_t index;
    for (index = 1U; index < manifest->excerpt_count; ++index)
        if (strcmp(manifest->excerpts[index - 1U].id,
                   manifest->excerpts[index].id) >= 0) return 0;
    return 1;
}

static int hwa_gap_compact_manifest(HWAGapManifest *manifest,
                                    uint64_t *live_work)
{
    void *grown;
#define HWA_GAP_COMPACT(field_, count_, capacity_)                          \
    do {                                                                     \
        if ((count_) == 0U) {                                                \
            hwa_gap_work_sub(live_work,                                      \
                (uint64_t)(capacity_) * (uint64_t)sizeof(*(field_)));         \
            free(field_); (field_) = NULL; (capacity_) = 0U;                 \
        } else if ((capacity_) != (count_)) {                                \
            grown = realloc((field_), (count_) * sizeof(*(field_)));         \
            if (grown == NULL) return -1;                                    \
            hwa_gap_work_sub(live_work,                                      \
                (uint64_t)((capacity_) - (count_)) *                         \
                    (uint64_t)sizeof(*(field_)));                             \
            (field_) = grown; (capacity_) = (count_);                        \
        }                                                                    \
    } while (0)
    HWA_GAP_COMPACT(manifest->sources, manifest->source_count,
                    manifest->source_capacity);
    HWA_GAP_COMPACT(manifest->labels, manifest->label_count,
                    manifest->label_capacity);
    HWA_GAP_COMPACT(manifest->excerpts, manifest->excerpt_count,
                    manifest->excerpt_capacity);
#undef HWA_GAP_COMPACT
    return 0;
}

static int hwa_gap_source_suffix(const char *name,
                                 const char *suffix,
                                 size_t *prefix_size)
{
    size_t name_size = strlen(name);
    size_t suffix_size = strlen(suffix);
    if (name_size <= suffix_size ||
        strcmp(name + name_size - suffix_size, suffix) != 0) return 0;
    *prefix_size = name_size - suffix_size;
    return *prefix_size != 0U;
}

static int hwa_gap_measurement_partner(
    const HWAGapReportResult *result,
    size_t source_index,
    size_t *partner_index,
    int *is_reference)
{
    const char *name = result->sources[source_index].name;
    const char *own_suffix;
    const char *wanted_suffix;
    size_t prefix_size;
    size_t index;
    size_t matches = 0U;
    if (hwa_gap_source_suffix(name, ".reference", &prefix_size)) {
        own_suffix = ".reference";
        wanted_suffix = ".model";
        *is_reference = 1;
    } else if (hwa_gap_source_suffix(name, ".model", &prefix_size)) {
        own_suffix = ".model";
        wanted_suffix = ".reference";
        *is_reference = 0;
    } else return -1;
    (void)own_suffix;
    for (index = 0U; index < result->source_count; ++index) {
        size_t other_prefix;
        const HWAGapReportSource *source = &result->sources[index];
        if (source->kind != HWA_GAP_REPORT_SOURCE_MEASUREMENT ||
            !hwa_gap_source_suffix(source->name, wanted_suffix,
                                   &other_prefix) ||
            other_prefix != prefix_size ||
            memcmp(source->name, name, prefix_size) != 0) continue;
        *partner_index = index;
        matches++;
    }
    return matches == 1U ? 0 : -1;
}

static int hwa_gap_transfer_labels(HWAGapManifest *manifest,
                                   HWAGapReportResult *result,
                                   char *error,
                                   size_t error_size)
{
    size_t index;
    if (manifest->label_count != 0U) {
        result->labels = (HWAGapReportLabel *)calloc(
            manifest->label_count, sizeof(*result->labels));
        if (result->labels == NULL) return -1;
    }
    for (index = 0U; index < manifest->label_count; ++index) {
        HWAGapLabelDecl *source = &manifest->labels[index];
        HWAGapReportLabel *target = &result->labels[index];
        HWAGapReportSource *owner = hwa_gap_source_by_name(result,
                                                            source->source);
        if (owner == NULL) {
            hwa_set_error(error, error_size,
                          "Stage 9 label names an unknown source");
            return -1;
        }
        target->id = (uint64_t)index + UINT64_C(1);
        target->source_id = owner->id;
        target->case_id = source->case_id; source->case_id = NULL;
        target->pitch = source->pitch; source->pitch = NULL;
        target->register_name = source->register_name;
        source->register_name = NULL;
        target->dynamic = source->dynamic; source->dynamic = NULL;
        target->gesture = source->gesture; source->gesture = NULL;
        target->physical_element = source->physical_element;
        source->physical_element = NULL;
        target->section = source->section; source->section = NULL;
        free(source->source); source->source = NULL;
        result->label_count++;
    }
    return 0;
}

static int hwa_gap_transfer_excerpts(HWAGapManifest *manifest,
                                     HWAGapReportResult *result,
                                     char *error,
                                     size_t error_size)
{
    size_t index;
    uint64_t total_frames = 0U;
    if (manifest->excerpt_count != 0U) {
        result->excerpts = (HWAGapReportExcerpt *)calloc(
            manifest->excerpt_count, sizeof(*result->excerpts));
        if (result->excerpts == NULL) return -1;
    }
    for (index = 0U; index < manifest->excerpt_count; ++index) {
        HWAGapExcerptDecl *source = &manifest->excerpts[index];
        HWAGapReportExcerpt *target = &result->excerpts[index];
        HWAGapReportSource *candidate = hwa_gap_source_by_name(
            result, source->candidate_source);
        HWAGapReportSource *reference = hwa_gap_source_by_name(
            result, source->reference);
        HWAGapReportSource *model = hwa_gap_source_by_name(
            result, source->model);
        if (candidate == NULL || reference == NULL || model == NULL ||
            candidate->kind == HWA_GAP_REPORT_SOURCE_WAVE ||
            reference->kind != HWA_GAP_REPORT_SOURCE_WAVE ||
            model->kind != HWA_GAP_REPORT_SOURCE_WAVE ||
            source->frame_count > result->options.max_excerpt_frames ||
            hwa_gap_add_u64(&total_frames, source->frame_count) != 0 ||
            total_frames > result->options.max_total_excerpt_frames) {
            hwa_set_error(error, error_size,
                          "invalid Stage 9 excerpt source or frame range");
            return -1;
        }
        target->id = (uint64_t)index + UINT64_C(1);
        target->name = source->id; source->id = NULL;
        target->candidate_source_id = candidate->id;
        target->candidate_row = source->candidate_row;
        target->view = source->view;
        target->reference_source_id = reference->id;
        target->model_source_id = model->id;
        target->reference_start_sample = source->reference_start_sample;
        target->model_start_sample = source->model_start_sample;
        target->frame_count = source->frame_count;
        target->make_x = source->make_x;
        target->availability = HWA_GAP_REPORT_UNAVAILABLE;
        target->reference_path = hwa_gap_strdup("");
        target->model_path = hwa_gap_strdup("");
        target->x_path = hwa_gap_strdup("");
        target->reason = hwa_gap_strdup(result->mode == HWA_GAP_REPORT_RANK ?
            "rank-mode-does-not-render-excerpts" : "excerpt-not-rendered");
        if (target->reference_path == NULL || target->model_path == NULL ||
            target->x_path == NULL || target->reason == NULL) return -1;
        free(source->candidate_source); source->candidate_source = NULL;
        free(source->reference); source->reference = NULL;
        free(source->model); source->model = NULL;
        result->excerpt_count++;
    }
    return 0;
}

static int hwa_gap_prepare_excerpts(HWAGapReportResult *result,
                                    uint64_t *runtime_evaluations,
                                    char *error,
                                    size_t error_size)
{
    size_t excerpt_index;
    uint64_t visits;
    uint64_t projected_work;
    const size_t maximum_reason =
        sizeof("candidate-evidence-unavailable");
    if (result == NULL || runtime_evaluations == NULL ||
        hwa_gap_mul_u64((uint64_t)result->excerpt_count,
                        (uint64_t)result->candidate_count, &visits) != 0 ||
        hwa_gap_evaluations_add(runtime_evaluations, visits,
            result->options.max_evaluations, error, error_size) != 0 ||
        hwa_gap_report_result_retained_bytes(result, &projected_work) != 0)
        return -1;
    for (excerpt_index = 0U; excerpt_index < result->excerpt_count;
         ++excerpt_index) {
        size_t current = strlen(result->excerpts[excerpt_index].reason) + 1U;
        if (current < maximum_reason &&
            hwa_gap_add_u64(&projected_work,
                            (uint64_t)(maximum_reason - current)) != 0)
            return -1;
    }
    if (projected_work > result->options.max_work_bytes) {
        hwa_set_error(error, error_size,
                      "Stage 9 excerpt preparation exceeds its work cap");
        return -1;
    }
    for (excerpt_index = 0U; excerpt_index < result->excerpt_count;
         ++excerpt_index) {
        HWAGapReportExcerpt *excerpt = &result->excerpts[excerpt_index];
        size_t candidate_index;
        int found = 0;
        for (candidate_index = 0U;
             candidate_index < result->candidate_count; ++candidate_index) {
            const HWAGapReportCandidate *candidate =
                &result->candidates[candidate_index];
            if (candidate->source_id == excerpt->candidate_source_id &&
                candidate->source_row == excerpt->candidate_row) {
                found = 1;
                break;
            }
        }
        free(excerpt->reason);
        if (!found) {
            excerpt->availability = HWA_GAP_REPORT_UNAVAILABLE;
            excerpt->reason = hwa_gap_strdup("candidate-evidence-unavailable");
        } else if (excerpt->view != HWA_GAP_REPORT_VIEW_RAW) {
            excerpt->availability = HWA_GAP_REPORT_UNAVAILABLE;
            excerpt->reason = hwa_gap_strdup("view-authority-unavailable");
        } else {
            excerpt->availability = HWA_GAP_REPORT_AVAILABLE;
            excerpt->reason = hwa_gap_strdup("");
        }
        if (excerpt->reason == NULL) return -1;
    }
    return 0;
}

static int hwa_gap_output_component_valid(const char *name)
{
    return name != NULL && name[0] != '\0' && strcmp(name, ".") != 0 &&
        strcmp(name, "..") != 0 && strchr(name, '/') == NULL &&
        strchr(name, '\\') == NULL;
}

static char *hwa_gap_absolute_output(const char *path)
{
#if defined(_WIN32)
    char resolved[_MAX_PATH];
    if (_fullpath(resolved, path, sizeof(resolved)) == NULL) return NULL;
    return hwa_gap_strdup(resolved);
#else
    const char *slash;
    const char *name;
    char *parent = NULL;
    char resolved[PATH_MAX];
    char *output = NULL;
    size_t parent_size;
    size_t resolved_size;
    size_t name_size;
    slash = strrchr(path, '/');
    name = slash == NULL ? path : slash + 1;
    if (!hwa_gap_output_component_valid(name)) return NULL;
    if (slash == NULL) parent = hwa_gap_strdup(".");
    else {
        parent_size = slash == path ? 1U : (size_t)(slash - path);
        parent = (char *)malloc(parent_size + 1U);
        if (parent != NULL) {
            memcpy(parent, path, parent_size);
            parent[parent_size] = '\0';
        }
    }
    if (parent == NULL || realpath(parent, resolved) == NULL) {
        free(parent);
        return NULL;
    }
    free(parent);
    resolved_size = strlen(resolved);
    name_size = strlen(name);
    if (resolved_size > SIZE_MAX - name_size - 2U) return NULL;
    output = (char *)malloc(resolved_size + name_size + 2U);
    if (output != NULL) {
        memcpy(output, resolved, resolved_size);
        if (resolved_size == 0U || resolved[resolved_size - 1U] != '/')
            output[resolved_size++] = '/';
        memcpy(output + resolved_size, name, name_size + 1U);
    }
    return output;
#endif
}

static int hwa_gap_adapt_sources(HWAGapReportResult *result,
                                 uint64_t *live_work,
                                 uint64_t *adapter_evaluations,
                                 char *error,
                                 size_t error_size)
{
    size_t index;
    uint64_t visits;
    if (result == NULL || adapter_evaluations == NULL ||
        hwa_gap_mul_u64((uint64_t)result->source_count,
                        (uint64_t)result->source_count, &visits) != 0 ||
        hwa_gap_add_u64(&visits, (uint64_t)result->source_count) != 0 ||
        hwa_gap_evaluations_add(adapter_evaluations, visits,
            result->options.max_evaluations, error, error_size) != 0)
        return -1;
    for (index = 0U; index < result->source_count; ++index) {
        HWAGapReportSource *source = &result->sources[index];
        size_t first_candidate = result->candidate_count;
        source->candidate_count = 0U;
        if (source->kind == HWA_GAP_REPORT_SOURCE_MEASUREMENT) {
            size_t partner;
            int is_reference;
            if (hwa_gap_measurement_partner(result, index, &partner,
                                            &is_reference) != 0) {
                hwa_set_error(error, error_size,
                              "ambiguous Stage 9 measurement cohort");
                return -1;
            }
            if (is_reference && hwa_gap_adapt_measurement_pair(
                    result, source, &result->sources[partner], live_work,
                    adapter_evaluations,
                    error, error_size) != 0) return -1;
        } else if (source->kind == HWA_GAP_REPORT_SOURCE_PRODUCTION) {
            if (hwa_gap_adapt_production(result, source, live_work,
                                         adapter_evaluations,
                                         error, error_size) != 0) return -1;
        } else if (source->kind == HWA_GAP_REPORT_SOURCE_RUN) {
            if (hwa_gap_adapt_run(result, source, live_work,
                                  adapter_evaluations,
                                  error, error_size) != 0) return -1;
        } else if (source->kind == HWA_GAP_REPORT_SOURCE_EXPERIMENT) {
            if (hwa_gap_adapt_experiment(result, source, live_work,
                                         adapter_evaluations,
                                         error, error_size) != 0) return -1;
        }
        source->candidate_count = result->candidate_count - first_candidate;
    }
    return 0;
}

typedef struct HWAGapMeasureGroupKey {
    int item_kind;
    const char *role;
    size_t role_size;
    int selector;
    const char *value;
    size_t value_size;
} HWAGapMeasureGroupKey;

typedef struct HWAGapMeasureMetricState {
    int kind;
    uint32_t index;
    int view;
    size_t max_partials;
} HWAGapMeasureMetricState;

static int hwa_gap_lower_hex(const char *text, size_t size)
{
    size_t index;
    if ((size & 1U) != 0U) return 0;
    for (index = 0U; index < size; ++index) {
        if (!((text[index] >= '0' && text[index] <= '9') ||
              (text[index] >= 'a' && text[index] <= 'f'))) return 0;
        if ((index & 1U) == 0U && text[index] == '0' &&
            text[index + 1U] == '0') return 0;
    }
    return 1;
}

static int hwa_gap_named_part(const char *text,
                              size_t size,
                              int first,
                              int last,
                              const char *(*name)(int))
{
    int value;
    for (value = first; value < last; ++value) {
        const char *candidate = name(value);
        if (candidate != NULL && strlen(candidate) == size &&
            memcmp(candidate, text, size) == 0) return value;
    }
    return 0;
}

static const char *hwa_gap_item_kind_name_int(int value)
{
    return hwa_measure_item_kind_name((HWAItemKind)value);
}

static const char *hwa_gap_selector_name_int(int value)
{
    return hwa_measure_group_selector_name((HWAMeasureGroupSelector)value);
}

static int hwa_gap_measure_group_key_parse(const char *text,
                                           HWAGapMeasureGroupKey *key)
{
    const char *kind;
    const char *role;
    const char *selector;
    const char *value;
    const char *slash;
    size_t kind_size;
    size_t selector_size;
    if (text == NULL || strncmp(text, "g/", 2U) != 0) return 0;
    kind = text + 2U;
    slash = strchr(kind, '/');
    if (slash == NULL) return 0;
    kind_size = (size_t)(slash - kind);
    role = slash + 1U;
    slash = strchr(role, '/');
    if (slash == NULL) return 0;
    key->role = role;
    key->role_size = (size_t)(slash - role);
    selector = slash + 1U;
    slash = strchr(selector, '/');
    if (slash == NULL) return 0;
    selector_size = (size_t)(slash - selector);
    value = slash + 1U;
    if (strchr(value, '/') != NULL) return 0;
    key->value = value;
    key->value_size = strlen(value);
    key->item_kind = hwa_gap_named_part(
        kind, kind_size, HWA_ITEM_NOTE, HWA_ITEM_MULTI_NOTE + 1,
        hwa_gap_item_kind_name_int);
    key->selector = hwa_gap_named_part(
        selector, selector_size, HWA_MEASURE_GROUP_ALL,
        HWA_MEASURE_GROUP_SELECTOR_COUNT, hwa_gap_selector_name_int);
    return key->item_kind != 0 && key->selector != 0 &&
        key->role_size != 0U &&
        hwa_gap_lower_hex(key->role, key->role_size) &&
        hwa_gap_lower_hex(key->value, key->value_size) &&
        ((key->selector == HWA_MEASURE_GROUP_ALL && key->value_size == 0U) ||
         (key->selector != HWA_MEASURE_GROUP_ALL && key->value_size != 0U));
}

static int hwa_gap_slice_compare(const char *left,
                                 size_t left_size,
                                 const char *right,
                                 size_t right_size)
{
    size_t common = left_size < right_size ? left_size : right_size;
    int order = memcmp(left, right, common);
    if (order != 0) return order;
    return left_size < right_size ? -1 : left_size > right_size ? 1 : 0;
}

static int hwa_gap_measure_group_key_compare(
    const HWAGapMeasureGroupKey *left,
    const HWAGapMeasureGroupKey *right)
{
    int order;
    if (left->item_kind != right->item_kind)
        return left->item_kind < right->item_kind ? -1 : 1;
    order = hwa_gap_slice_compare(left->role, left->role_size,
                                  right->role, right->role_size);
    if (order != 0) return order;
    if (left->selector != right->selector)
        return left->selector < right->selector ? -1 : 1;
    return hwa_gap_slice_compare(left->value, left->value_size,
                                 right->value, right->value_size);
}

static void hwa_gap_measure_metric_state_init(
    HWAGapMeasureMetricState *state)
{
    state->kind = HWA_MEASURE_RMS_DBFS;
    state->index = hwa_measure_kind_index_valid(
        (HWAMeasureKind)state->kind, 0U, 32U) ? 0U : 1U;
    state->view = HWA_MEASURE_VIEW_RAW;
    state->max_partials = 32U;
}

static int hwa_gap_measure_metric_state_advance(
    HWAGapMeasureMetricState *state)
{
    HWAMeasureUnit unit;
    if (state->view == HWA_MEASURE_VIEW_RAW &&
        hwa_measure_kind_unit((HWAMeasureKind)state->kind,
            HWA_MEASURE_VIEW_LEVEL_RELATIVE, &unit) == 0) {
        state->view = HWA_MEASURE_VIEW_LEVEL_RELATIVE;
        return 1;
    }
    state->view = HWA_MEASURE_VIEW_RAW;
    if (state->index != UINT32_MAX && hwa_measure_kind_index_valid(
            (HWAMeasureKind)state->kind, state->index + 1U,
            state->max_partials)) {
        state->index++;
        return 1;
    }
    while (++state->kind < HWA_MEASURE_KIND_COUNT) {
        state->index = hwa_measure_kind_index_valid(
            (HWAMeasureKind)state->kind, 0U, state->max_partials) ? 0U : 1U;
        if (hwa_measure_kind_index_valid((HWAMeasureKind)state->kind,
                state->index, state->max_partials)) return 1;
    }
    return 0;
}

static int hwa_gap_measure_metric_state_accept(
    HWAGapMeasureMetricState *state,
    const char *metric)
{
    for (;;) {
        char buffer[64];
        const char *expected;
        if (state->kind >= HWA_MEASURE_KIND_COUNT) return 0;
        expected = hwa_gap_measure_metric(
            (HWAMeasureKind)state->kind, state->index, buffer);
        if (strcmp(expected, metric) == 0) {
            (void)hwa_gap_measure_metric_state_advance(state);
            return 1;
        }
        if (!hwa_gap_measure_metric_state_advance(state)) return 0;
    }
}

static int hwa_gap_case_ids_equal(const HWAGapReportResult *result,
                                  size_t left,
                                  size_t left_count,
                                  size_t right,
                                  size_t right_count)
{
    size_t index;
    if (left_count != right_count) return 0;
    for (index = 0U; index < left_count; ++index)
        if (result->cases[left + index].case_id == NULL ||
            result->cases[right + index].case_id == NULL ||
            strcmp(result->cases[left + index].case_id,
                   result->cases[right + index].case_id) != 0) return 0;
    return 1;
}

static int hwa_gap_case_row_sets_equal(const HWAGapReportResult *result,
                                       size_t left,
                                       size_t left_count,
                                       size_t right,
                                       size_t right_count)
{
    const HWAGapReportCase *a;
    const HWAGapReportCase *b;
    int a_fallback;
    int b_fallback;
    if (!hwa_gap_case_ids_equal(
            result, left, left_count, right, right_count)) return 0;
    a = &result->cases[left];
    b = &result->cases[right];
    a_fallback = left_count == 1U && a->case_id != NULL && a->reason != NULL &&
        a->availability == HWA_GAP_REPORT_UNAVAILABLE && !a->value_valid &&
        !a->confidence_valid && a->value == 0.0 && a->confidence == 0.0 &&
        strcmp(a->reason, "measurement-case-unavailable") == 0;
    b_fallback = right_count == 1U && b->case_id != NULL && b->reason != NULL &&
        b->availability == HWA_GAP_REPORT_UNAVAILABLE && !b->value_valid &&
        !b->confidence_valid && b->value == 0.0 && b->confidence == 0.0 &&
        strcmp(b->reason, "measurement-case-unavailable") == 0;
    return a_fallback == b_fallback;
}

static int hwa_gap_experiment_case_name_valid(const char *text)
{
    size_t index;
    size_t size;
    if (text == NULL || (size = strlen(text)) == 0U || size > 127U ||
        !((text[0] >= 'a' && text[0] <= 'z') ||
          (text[0] >= 'A' && text[0] <= 'Z') ||
          (text[0] >= '0' && text[0] <= '9'))) return 0;
    for (index = 1U; index < size; ++index)
        if (!((text[index] >= 'a' && text[index] <= 'z') ||
              (text[index] >= 'A' && text[index] <= 'Z') ||
              (text[index] >= '0' && text[index] <= '9') ||
              text[index] == '.' || text[index] == '_' ||
              text[index] == ':' || text[index] == '/' ||
              text[index] == '-')) return 0;
    return 1;
}

static int hwa_gap_production_check_case_valid(const char *text)
{
    HWAProductionSplit split;
    return text != NULL &&
        hwa_production_split_for_item_key(text, &split) == 0 &&
        split == HWA_PRODUCTION_CHECK;
}

int hwa_gap_report_case_catalog_fits(
    const HWAGapReportResult *result,
    const HWAGapReportOptions *options)
{
    size_t source_index;
    size_t candidate_index = 0U;
    size_t case_index = 0U;
    if (result == NULL || options == NULL) return 0;
    for (source_index = 0U; source_index < result->source_count;
         ++source_index) {
        const HWAGapReportSource *source = &result->sources[source_index];
        size_t ordinal;
        size_t baseline_start = 0U;
        size_t baseline_count = 0U;
        const char *baseline_group = NULL;
        uint64_t measurement_members = 0U;
        unsigned experiment_roles = 0U;
        for (ordinal = 0U; ordinal < source->candidate_count; ++ordinal) {
            const HWAGapReportCandidate *candidate =
                &result->candidates[candidate_index + ordinal];
            size_t start = case_index;
            size_t count;
            while (case_index < result->case_count &&
                   result->cases[case_index].candidate_id == candidate->id)
                case_index++;
            count = case_index - start;
            if (count == 0U || result->cases[start].case_id == NULL ||
                result->cases[start].reason == NULL) return 0;
            if (source->kind == HWA_GAP_REPORT_SOURCE_RUN) {
                if (count != 1U ||
                    strcmp(result->cases[start].case_id,
                           candidate->case_id) != 0) return 0;
            } else if (source->kind == HWA_GAP_REPORT_SOURCE_MEASUREMENT) {
                int same_group = baseline_group != NULL &&
                    strcmp(baseline_group, candidate->case_id) == 0;
                int fallback = count == 1U &&
                    strcmp(result->cases[start].case_id,
                           candidate->case_id) == 0 &&
                    result->cases[start].availability ==
                        HWA_GAP_REPORT_UNAVAILABLE &&
                    !result->cases[start].value_valid &&
                    !result->cases[start].confidence_valid &&
                    result->cases[start].value == 0.0 &&
                    result->cases[start].confidence == 0.0 &&
                    strcmp(result->cases[start].reason,
                           "measurement-case-unavailable") == 0;
                if (same_group) {
                    if (!hwa_gap_case_row_sets_equal(result, baseline_start,
                            baseline_count, start, count)) return 0;
                } else {
                    baseline_group = candidate->case_id;
                    baseline_start = start;
                    baseline_count = count;
                    if (!fallback && ((uint64_t)count >
                            options->measurement.max_contexts ||
                            hwa_gap_add_u64(
                            &measurement_members, (uint64_t)count) != 0 ||
                            measurement_members >
                                options->measurement.max_group_members))
                        return 0;
                }
            } else if (source->kind == HWA_GAP_REPORT_SOURCE_PRODUCTION ||
                       source->kind == HWA_GAP_REPORT_SOURCE_EXPERIMENT) {
                uint64_t total_cases = (uint64_t)count;
                uint64_t rows;
                size_t case_offset;
                if (hwa_gap_add_u64(&total_cases, UINT64_C(1)) != 0)
                    return 0;
                if (source->kind == HWA_GAP_REPORT_SOURCE_PRODUCTION) {
                    uint64_t metrics =
                        (uint64_t)hwa_production_metric_catalog_count();
                    uint64_t minimum_evaluations;
                    for (case_offset = 0U; case_offset < count; ++case_offset)
                        if (!hwa_gap_production_check_case_valid(
                                result->cases[start + case_offset].case_id))
                            return 0;
                    if (total_cases > options->production.max_spans ||
                        hwa_gap_mul_u64(total_cases, UINT64_C(2),
                            &minimum_evaluations) != 0 ||
                        minimum_evaluations >
                            options->production.max_evaluations ||
                        hwa_gap_mul_u64(total_cases, metrics, &rows) != 0 ||
                        hwa_gap_mul_u64(rows, UINT64_C(3), &rows) != 0 ||
                        rows > options->production.max_evaluation_rows)
                        return 0;
                } else {
                    int role = 0;
                    if (!hwa_gap_role_family_valid(
                            source->id, candidate->metric,
                            candidate->family_key, 1, &role) ||
                        role <= 0 || role >= 32) return 0;
                    experiment_roles |= 1U << (unsigned)role;
                    if (total_cases > options->experiment.max_cases ||
                        total_cases > options->experiment.max_jobs ||
                        total_cases > options->experiment.max_artifacts ||
                        hwa_gap_mul_u64(total_cases,
                            (uint64_t)source->candidate_count, &rows) != 0 ||
                        rows > options->experiment.max_observations)
                        return 0;
                    for (case_offset = 0U; case_offset < count; ++case_offset)
                        if (!hwa_gap_experiment_case_name_valid(
                                result->cases[start + case_offset].case_id))
                            return 0;
                }
                if (ordinal == 0U) {
                    baseline_start = start;
                    baseline_count = count;
                } else if (!hwa_gap_case_ids_equal(result, baseline_start,
                               baseline_count, start, count)) return 0;
            } else {
                return 0;
            }
        }
        if (source->kind == HWA_GAP_REPORT_SOURCE_EXPERIMENT) {
            size_t role_count = 0U;
            unsigned roles = experiment_roles;
            uint64_t minimum_stems;
            uint64_t minimum_jobs;
            while (roles != 0U) {
                role_count += roles & 1U;
                roles >>= 1U;
            }
            minimum_stems = (uint64_t)role_count;
            minimum_jobs = (uint64_t)baseline_count;
            if (hwa_gap_add_u64(&minimum_stems, UINT64_C(1)) != 0 ||
                minimum_stems > options->experiment.run.max_evaluations ||
                hwa_gap_add_u64(&minimum_jobs, UINT64_C(1)) != 0 ||
                hwa_gap_mul_u64(minimum_jobs, minimum_stems,
                    &minimum_jobs) != 0 ||
                minimum_jobs >
                    options->experiment.max_total_run_evaluations)
                return 0;
        }
        candidate_index += source->candidate_count;
    }
    return candidate_index == result->candidate_count &&
        case_index == result->case_count;
}

int hwa_gap_report_candidate_catalog_fits(
    const HWAGapReportResult *result,
    const HWAGapReportOptions *options)
{
    size_t source_index;
    size_t candidate_index = 0U;
    size_t production_metrics = hwa_production_metric_catalog_count();
    size_t run_features = hwa_run_feature_catalog_count();
    size_t run_stages = hwa_run_stage_catalog_count();
    if (result == NULL || options == NULL) return 0;
    for (source_index = 0U; source_index < result->source_count;
         ++source_index) {
        const HWAGapReportSource *source = &result->sources[source_index];
        size_t count = source->candidate_count;
        size_t ordinal;
        size_t run_feature_count = 0U;
        size_t measurement_groups = 0U;
        int measurement_reference = 0;
        int previous_run_role = 0;
        int run_block_role = 0;
        int saw_final_role = 0;
        unsigned experiment_roles = 0U;
        int have_measurement_group = 0;
        HWAGapMeasureGroupKey previous_measurement_group = {0};
        HWAGapMeasureMetricState measurement_metric_state;
        size_t measurement_group_end = 0U;
        if (count > result->candidate_count - candidate_index) return 0;
        switch (source->kind) {
        case HWA_GAP_REPORT_SOURCE_MEASUREMENT: {
            size_t partner = 0U;
            if (hwa_gap_measurement_partner(
                    result, source_index, &partner,
                    &measurement_reference) != 0 || partner == source_index ||
                (!measurement_reference && count != 0U) ||
                (measurement_reference &&
                 (count > options->measurement.max_gaps ||
                  count > options->measurement.max_distributions ||
                  (options->measurement.max_measurements <= SIZE_MAX / 2U &&
                   count > options->measurement.max_measurements * 2U) ||
                  (options->measurement.max_statistics <= SIZE_MAX / 2U &&
                   count > options->measurement.max_statistics * 2U))))
                return 0;
            break;
        }
        case HWA_GAP_REPORT_SOURCE_PRODUCTION:
            if (production_metrics == 0U ||
                production_metrics > SIZE_MAX / 6U ||
                options->production.max_view_rows <
                    production_metrics * 6U ||
                options->production.max_fits <
                    hwa_production_fit_catalog_count() ||
                options->production.max_spans < 2U ||
                options->production.max_evaluation_rows <
                    production_metrics * 6U ||
                count != production_metrics * 3U) return 0;
            break;
        case HWA_GAP_REPORT_SOURCE_RUN:
            if (run_features == 0U || run_stages == 0U ||
                count < run_features + run_stages) return 0;
            run_feature_count = count - run_stages;
            if (run_feature_count < run_features ||
                run_feature_count % run_features != 0U) return 0;
            {
                size_t model_count = run_feature_count / run_features;
                uint64_t minimum_rows;
                if (model_count == SIZE_MAX ||
                    model_count + 1U > options->run.max_stems ||
                    model_count + 1U > options->run.max_evaluations ||
                    hwa_gap_mul_u64((uint64_t)model_count, UINT64_C(2),
                                    &minimum_rows) != 0 ||
                    hwa_gap_add_u64(&minimum_rows,
                                    (uint64_t)run_feature_count) != 0 ||
                    hwa_gap_add_u64(&minimum_rows, UINT64_C(4)) != 0 ||
                    minimum_rows >
                        (uint64_t)options->run.max_result_rows)
                    return 0;
            }
            break;
        case HWA_GAP_REPORT_SOURCE_EXPERIMENT:
            if (count == 0U ||
                count > options->experiment.max_responses ||
                count > UINT64_MAX / UINT64_C(2) ||
                options->experiment.max_cases < 2U ||
                options->experiment.max_jobs < 2U ||
                count > SIZE_MAX / 2U ||
                options->experiment.max_observations < count * 2U ||
                options->experiment.max_sensitivities < count * 2U)
                return 0;
            break;
        case HWA_GAP_REPORT_SOURCE_WAVE:
            if (count != 0U) return 0;
            break;
        default:
            return 0;
        }
        for (ordinal = 0U; ordinal < count; ++ordinal) {
            const HWAGapReportCandidate *candidate =
                &result->candidates[candidate_index + ordinal];
            uint64_t expected_row = (uint64_t)ordinal + UINT64_C(1);
            HWAGapReportCandidateKind expected_kind;
            char expected_case[64];
            const char *expected_metric = NULL;
            char metric_buffer[64];
            if (candidate->source_id != source->id ||
                !hwa_gap_metric_valid(candidate->metric)) return 0;
            if (source->kind == HWA_GAP_REPORT_SOURCE_MEASUREMENT) {
                HWAGapMeasureGroupKey current_group;
                int order = 1;
                expected_kind = HWA_GAP_REPORT_CANDIDATE_MEASUREMENT;
                if (!hwa_gap_measure_group_key_parse(
                        candidate->case_id, &current_group)) return 0;
                if (have_measurement_group) {
                    order = hwa_gap_measure_group_key_compare(
                        &previous_measurement_group, &current_group);
                    if (order > 0) return 0;
                }
                if (!have_measurement_group || order != 0) {
                    size_t scan = ordinal + 1U;
                    measurement_groups++;
                    if (measurement_groups > options->measurement.max_groups)
                        return 0;
                    while (scan < count &&
                           result->candidates[candidate_index + scan].case_id !=
                               NULL &&
                           strcmp(result->candidates[candidate_index + scan].case_id,
                                  candidate->case_id) == 0) scan++;
                    measurement_group_end = scan;
                    hwa_gap_measure_metric_state_init(
                        &measurement_metric_state);
                }
                previous_measurement_group = current_group;
                have_measurement_group = 1;
                if (ordinal >= measurement_group_end ||
                    !hwa_gap_measure_metric_state_accept(
                        &measurement_metric_state, candidate->metric) ||
                    !hwa_gap_measurement_family_valid(candidate)) return 0;
            } else if (source->kind == HWA_GAP_REPORT_SOURCE_PRODUCTION) {
                HWAProductionMetricKind kind;
                uint32_t metric_index;
                HWAProductionUnit unit;
                expected_row += (uint64_t)production_metrics * UINT64_C(3);
                expected_kind = HWA_GAP_REPORT_CANDIDATE_PRODUCTION;
                if (hwa_production_metric_catalog_at(
                        ordinal % production_metrics, &kind,
                        &metric_index, &unit) != 0) return 0;
                expected_metric = hwa_gap_production_metric(
                    kind, metric_index, metric_buffer);
                if (strcmp(candidate->case_id, "aggregate") != 0) return 0;
                if (!hwa_gap_production_family_valid(
                        candidate, expected_metric)) return 0;
            } else if (source->kind == HWA_GAP_REPORT_SOURCE_RUN) {
                if (ordinal < run_feature_count) {
                    HWARunFeatureKind kind;
                    uint32_t feature_index;
                    HWARunUnit unit;
                    int role;
                    expected_kind = HWA_GAP_REPORT_CANDIDATE_RUN_FEATURE;
                    if (hwa_run_feature_catalog_at(
                            ordinal % run_features, &kind,
                            &feature_index, &unit) != 0) return 0;
                    expected_metric = hwa_gap_run_metric(
                        kind, feature_index, metric_buffer);
                    if (!hwa_gap_role_family_valid(
                            source->id, expected_metric,
                            candidate->family_key, 0, &role)) return 0;
                    if (ordinal % run_features == 0U) {
                        if (role <= previous_run_role) return 0;
                        previous_run_role = role;
                        run_block_role = role;
                        if (role == HWA_RUN_STEM_FINAL) saw_final_role = 1;
                    } else if (role != run_block_role) {
                        return 0;
                    }
                    if (snprintf(expected_case, sizeof(expected_case),
                                 "feature:%" PRIu64, expected_row) < 0 ||
                        strcmp(candidate->case_id, expected_case) != 0)
                        return 0;
                } else {
                    size_t stage = ordinal - run_feature_count;
                    HWARunStemRole from_role;
                    HWARunStemRole to_role;
                    expected_kind = HWA_GAP_REPORT_CANDIDATE_RUN_STAGE;
                    expected_metric = "other";
                    if (hwa_run_stage_catalog_at(
                            stage, &from_role, &to_role) != 0) return 0;
                    if (!hwa_gap_stage_family_valid(
                            candidate, from_role, to_role)) return 0;
                    if (snprintf(expected_case, sizeof(expected_case),
                                 "stage:%zu", stage + 1U) < 0 ||
                        strcmp(candidate->case_id, expected_case) != 0)
                        return 0;
                }
            } else {
                int role = 0;
                expected_row = ((uint64_t)ordinal + UINT64_C(1)) *
                    UINT64_C(2);
                expected_kind = HWA_GAP_REPORT_CANDIDATE_EXPERIMENT;
                if (!(strcmp(candidate->metric, "level") == 0 ||
                      strcmp(candidate->metric, "crest") == 0 ||
                      strncmp(candidate->metric, "band:", 5U) == 0))
                    return 0;
                if (strcmp(candidate->case_id, "baseline-check") != 0)
                    return 0;
                if (!hwa_gap_role_family_valid(
                        source->id, candidate->metric,
                        candidate->family_key, 1, &role) || role <= 0 ||
                    role >= 32) return 0;
                experiment_roles |= 1U << (unsigned)role;
            }
            if (candidate->source_row != expected_row ||
                candidate->kind != expected_kind ||
                (expected_metric != NULL &&
                 strcmp(candidate->metric, expected_metric) != 0)) return 0;
        }
        if (source->kind == HWA_GAP_REPORT_SOURCE_RUN && !saw_final_role)
            return 0;
        if (source->kind == HWA_GAP_REPORT_SOURCE_EXPERIMENT) {
            size_t role_count = 0U;
            unsigned roles = experiment_roles;
            uint64_t minimum_rows;
            while (roles != 0U) {
                role_count += roles & 1U;
                roles >>= 1U;
            }
            if (role_count == SIZE_MAX ||
                role_count + 1U > options->experiment.run.max_stems ||
                hwa_gap_mul_u64((uint64_t)role_count, UINT64_C(14),
                                &minimum_rows) != 0 ||
                hwa_gap_add_u64(&minimum_rows, UINT64_C(4)) != 0 ||
                minimum_rows > options->experiment.run.max_result_rows)
                return 0;
        }
        candidate_index += count;
    }
    return candidate_index == result->candidate_count;
}

static int hwa_gap_text_equal(const char *left, const char *right)
{
    return left != NULL && right != NULL && strcmp(left, right) == 0;
}

static int hwa_gap_candidate_derived_equal(
    const HWAGapReportCandidate *left,
    const HWAGapReportCandidate *right)
{
    return left->size_factor == right->size_factor &&
        left->audibility_factor == right->audibility_factor &&
        left->occurrence_factor == right->occurrence_factor &&
        left->confidence_factor == right->confidence_factor &&
        left->score == right->score &&
        left->occurrence_count == right->occurrence_count &&
        left->eligible_count == right->eligible_count &&
        left->linked_family_id == right->linked_family_id &&
        left->rank == right->rank &&
        left->quality_flags == right->quality_flags &&
        left->size_valid == right->size_valid &&
        left->audibility_valid == right->audibility_valid &&
        left->occurrence_valid == right->occurrence_valid &&
        left->confidence_valid == right->confidence_valid &&
        left->score_valid == right->score_valid &&
        left->primary == right->primary;
}

static int hwa_gap_group_equal(const HWAGapReportGroup *left,
                               const HWAGapReportGroup *right)
{
    return left->id == right->id && left->axis == right->axis &&
        hwa_gap_text_equal(left->value, right->value) &&
        left->family_count == right->family_count &&
        left->candidate_count == right->candidate_count &&
        left->available_count == right->available_count &&
        left->missing_count == right->missing_count &&
        left->excluded_count == right->excluded_count &&
        left->q05 == right->q05 && left->q25 == right->q25 &&
        left->median == right->median && left->q75 == right->q75 &&
        left->q95 == right->q95 && left->spread == right->spread &&
        left->confidence == right->confidence &&
        left->statistics_valid == right->statistics_valid &&
        left->confidence_valid == right->confidence_valid;
}

static int hwa_gap_warning_equal(const HWAGapReportWarning *left,
                                 const HWAGapReportWarning *right)
{
    return left->id == right->id &&
        hwa_gap_text_equal(left->code, right->code) &&
        hwa_gap_text_equal(left->message, right->message) &&
        left->source_id == right->source_id &&
        left->candidate_id == right->candidate_id &&
        left->excerpt_id == right->excerpt_id &&
        left->source_id_valid == right->source_id_valid &&
        left->candidate_id_valid == right->candidate_id_valid &&
        left->excerpt_id_valid == right->excerpt_id_valid;
}

static int hwa_gap_saved_derived_bytes(const HWAGapReportResult *result,
                                       uint64_t *bytes)
{
    uint64_t total = 0U;
    size_t index;
    if (result == NULL || bytes == NULL ||
        hwa_gap_array_bytes(result->family_count,
            sizeof(*result->families), &total) != 0 ||
        hwa_gap_array_bytes(result->group_count,
            sizeof(*result->groups), &total) != 0 ||
        hwa_gap_array_bytes(result->warning_count,
            sizeof(*result->warnings), &total) != 0)
        return -1;
    for (index = 0U; index < result->family_count; ++index)
        if (hwa_gap_string_bytes(result->families[index].key, &total) != 0)
            return -1;
    for (index = 0U; index < result->group_count; ++index)
        if (hwa_gap_string_bytes(result->groups[index].value, &total) != 0)
            return -1;
    for (index = 0U; index < result->warning_count; ++index)
        if (hwa_gap_string_bytes(result->warnings[index].code, &total) != 0 ||
            hwa_gap_string_bytes(result->warnings[index].message,
                                 &total) != 0)
            return -1;
    *bytes = total;
    return 0;
}

static int hwa_gap_derived_validate(const HWAGapReportResult *result,
                                    char *error,
                                    size_t error_size)
{
    HWAGapReportResult rebuilt = *result;
    size_t index;
    int valid = 1;
    uint64_t starting = 0U;
    uint64_t saved_derived_bytes = 0U;
    rebuilt.candidates = NULL;
    rebuilt.cases = NULL;
    rebuilt.families = NULL;
    rebuilt.family_count = 0U;
    rebuilt.groups = NULL;
    rebuilt.group_count = 0U;
    rebuilt.warnings = NULL;
    rebuilt.warning_count = 0U;
    if (result->candidate_count != 0U) {
        rebuilt.candidates = (HWAGapReportCandidate *)malloc(
            result->candidate_count * sizeof(*rebuilt.candidates));
        if (rebuilt.candidates != NULL)
            memcpy(rebuilt.candidates, result->candidates,
                   result->candidate_count * sizeof(*rebuilt.candidates));
    }
    if (result->case_count != 0U) {
        rebuilt.cases = (HWAGapReportCase *)malloc(
            result->case_count * sizeof(*rebuilt.cases));
        if (rebuilt.cases != NULL)
            memcpy(rebuilt.cases, result->cases,
                   result->case_count * sizeof(*rebuilt.cases));
    }
    if (hwa_gap_saved_derived_bytes(result, &saved_derived_bytes) != 0 ||
        hwa_gap_add_u64(&starting, (uint64_t)result->source_count) != 0 ||
        hwa_gap_add_u64(&starting, (uint64_t)result->candidate_count) != 0 ||
        hwa_gap_add_u64(&starting, (uint64_t)result->case_count) != 0 ||
        (result->mode != HWA_GAP_REPORT_RANK &&
         hwa_gap_add_u64(&starting,
                         (uint64_t)result->excerpt_count) != 0) ||
        (result->candidate_count != 0U && rebuilt.candidates == NULL) ||
        (result->case_count != 0U && rebuilt.cases == NULL) ||
        hwa_gap_report_result_rebuild_impl(
            &rebuilt, starting, saved_derived_bytes,
            error, error_size) != 0 ||
        rebuilt.family_count != result->family_count ||
        rebuilt.group_count != result->group_count ||
        rebuilt.warning_count != result->warning_count ||
        result->evaluation_count != rebuilt.evaluation_count) valid = 0;
    for (index = 0U; valid && index < result->candidate_count; ++index)
        valid = hwa_gap_candidate_derived_equal(
            &result->candidates[index], &rebuilt.candidates[index]);
    for (index = 0U; valid && index < result->case_count; ++index)
        valid = result->cases[index].score == rebuilt.cases[index].score &&
            result->cases[index].score_valid ==
                rebuilt.cases[index].score_valid;
    for (index = 0U; valid && index < result->family_count; ++index)
        valid = result->families[index].id == rebuilt.families[index].id &&
            hwa_gap_text_equal(result->families[index].key,
                               rebuilt.families[index].key) &&
            result->families[index].primary_candidate_id ==
                rebuilt.families[index].primary_candidate_id &&
            result->families[index].member_count ==
                rebuilt.families[index].member_count &&
            result->families[index].rank == rebuilt.families[index].rank;
    for (index = 0U; valid && index < result->group_count; ++index)
        valid = hwa_gap_group_equal(&result->groups[index],
                                    &rebuilt.groups[index]);
    for (index = 0U; valid && index < result->warning_count; ++index)
        valid = hwa_gap_warning_equal(&result->warnings[index],
                                      &rebuilt.warnings[index]);
    free(rebuilt.candidates);
    free(rebuilt.cases);
    hwa_gap_derived_free(&rebuilt);
    hwa_gap_warnings_free(&rebuilt);
    if (!valid && error != NULL && error_size != 0U && error[0] == '\0')
        hwa_set_error(error, error_size,
                      "Stage 9 derived rows do not match raw evidence");
    return valid ? 0 : -1;
}

int hwa_gap_report_result_validate(const HWAGapReportResult *result,
                                   char *error,
                                   size_t error_size)
{
    size_t index;
    uint64_t retained;
    uint64_t peak;
    uint64_t input_bytes = 0U;
    uint64_t output_bytes = 0U;
    uint64_t total_excerpt_frames = 0U;
    if (result == NULL ||
        hwa_gap_report_options_validate(result == NULL ? NULL :
            &result->options, error, error_size) != 0 ||
        hwa_gap_report_mode_name(result->mode) == NULL ||
        result->manifest_path == NULL || result->manifest_path[0] == '\0' ||
        result->title == NULL || result->title[0] == '\0' ||
        result->audibility_method == NULL ||
        strcmp(result->audibility_method,
               HWA_GAP_REPORT_AUDIBILITY_METHOD) != 0 ||
        !hwa_gap_hash_valid(result->manifest_sha256) ||
        result->source_count == 0U ||
        result->source_count > result->options.max_sources ||
        result->label_count > result->options.max_labels ||
        result->candidate_count > result->options.max_candidates ||
        result->family_count > result->options.max_families ||
        result->group_count > result->options.max_groups ||
        result->case_count > result->options.max_cases ||
        result->excerpt_count > result->options.max_excerpts ||
        result->warning_count > result->options.max_warnings ||
        (result->source_count != 0U && result->sources == NULL) ||
        (result->label_count != 0U && result->labels == NULL) ||
        (result->candidate_count != 0U && result->candidates == NULL) ||
        (result->family_count != 0U && result->families == NULL) ||
        (result->group_count != 0U && result->groups == NULL) ||
        (result->case_count != 0U && result->cases == NULL) ||
        (result->excerpt_count != 0U && result->excerpts == NULL) ||
        (result->warning_count != 0U && result->warnings == NULL)) {
        hwa_set_error(error, error_size, "invalid Stage 9 result");
        return -1;
    }
    if ((result->mode == HWA_GAP_REPORT_RANK &&
         result->output_directory != NULL) ||
        (result->mode != HWA_GAP_REPORT_RANK &&
         (result->output_directory == NULL ||
          result->output_directory[0] == '\0'))) {
        hwa_set_error(error, error_size, "invalid Stage 9 output path");
        return -1;
    }
    for (index = 0U; index < result->source_count; ++index) {
        const HWAGapReportSource *source = &result->sources[index];
        if (source->id != (uint64_t)index + UINT64_C(1) ||
            !hwa_gap_token_valid(source->name, 1) ||
            source->path == NULL || source->path[0] == '\0' ||
            hwa_gap_report_source_kind_name(source->kind) == NULL ||
            !hwa_gap_hash_valid(source->sha256) ||
            source->file_bytes > result->options.max_input_bytes ||
            hwa_gap_add_u64(&input_bytes, source->file_bytes) != 0 ||
            (index != 0U && strcmp(result->sources[index - 1U].name,
                                   source->name) >= 0)) {
            hwa_set_error(error, error_size, "invalid Stage 9 source rows");
            return -1;
        }
        if (source->kind == HWA_GAP_REPORT_SOURCE_MEASUREMENT) {
            size_t partner_index = 0U;
            int is_reference = 0;
            if (hwa_gap_measurement_partner(
                    result, index, &partner_index, &is_reference) != 0 ||
                partner_index == index) {
                hwa_set_error(error, error_size,
                              "invalid Stage 9 measurement source cohort");
                return -1;
            }
        }
    }
    if (input_bytes != result->total_input_bytes) {
        hwa_set_error(error, error_size, "invalid Stage 9 input byte total");
        return -1;
    }
    for (index = 0U; index < result->label_count; ++index) {
        const HWAGapReportLabel *row = &result->labels[index];
        if (row->id != (uint64_t)index + UINT64_C(1) ||
            hwa_gap_source_by_id(result, row->source_id) == NULL ||
            row->case_id == NULL || row->case_id[0] == '\0' ||
            row->pitch == NULL || row->register_name == NULL ||
            row->dynamic == NULL || row->gesture == NULL ||
            row->physical_element == NULL || row->section == NULL ||
            strcmp(row->pitch, HWA_GAP_MISSING_LABEL) == 0 ||
            strcmp(row->register_name, HWA_GAP_MISSING_LABEL) == 0 ||
            strcmp(row->dynamic, HWA_GAP_MISSING_LABEL) == 0 ||
            strcmp(row->gesture, HWA_GAP_MISSING_LABEL) == 0 ||
            strcmp(row->physical_element, HWA_GAP_MISSING_LABEL) == 0 ||
            strcmp(row->section, HWA_GAP_MISSING_LABEL) == 0 ||
            (index != 0U &&
             (result->labels[index - 1U].source_id > row->source_id ||
              (result->labels[index - 1U].source_id == row->source_id &&
               strcmp(result->labels[index - 1U].case_id,
                      row->case_id) >= 0)))) {
            hwa_set_error(error, error_size, "invalid Stage 9 label rows");
            return -1;
        }
    }
    for (index = 0U; index < result->candidate_count; ++index) {
        const HWAGapReportCandidate *row = &result->candidates[index];
        const HWAGapReportSource *source = hwa_gap_source_by_id(
            result, row->source_id);
        int kind_matches = source != NULL &&
            ((source->kind == HWA_GAP_REPORT_SOURCE_MEASUREMENT &&
              row->kind == HWA_GAP_REPORT_CANDIDATE_MEASUREMENT) ||
             (source->kind == HWA_GAP_REPORT_SOURCE_PRODUCTION &&
              row->kind == HWA_GAP_REPORT_CANDIDATE_PRODUCTION) ||
             (source->kind == HWA_GAP_REPORT_SOURCE_RUN &&
              (row->kind == HWA_GAP_REPORT_CANDIDATE_RUN_FEATURE ||
               row->kind == HWA_GAP_REPORT_CANDIDATE_RUN_STAGE)) ||
             (source->kind == HWA_GAP_REPORT_SOURCE_EXPERIMENT &&
              row->kind == HWA_GAP_REPORT_CANDIDATE_EXPERIMENT));
        if (row->id != (uint64_t)index + UINT64_C(1) ||
            !kind_matches ||
            row->source_row == 0U || row->case_id == NULL ||
            row->case_id[0] == '\0' || row->metric == NULL ||
            row->metric[0] == '\0' || row->family_key == NULL ||
            row->family_key[0] == '\0' || row->reason == NULL ||
            hwa_gap_report_candidate_kind_name(row->kind) == NULL ||
            hwa_gap_report_availability_name(row->availability) == NULL ||
            row->raw_value_valid !=
                (row->availability == HWA_GAP_REPORT_AVAILABLE) ||
            (row->raw_value_valid &&
             (!isfinite(row->raw_value) || row->raw_value < 0.0 ||
              row->raw_value > 1.0)) ||
            (!row->raw_value_valid && row->raw_value != 0.0) ||
            (row->score_valid && (!isfinite(row->score) || row->score < 0.0 ||
                                  row->score > 1.0))) {
            hwa_set_error(error, error_size,
                          "invalid Stage 9 candidate rows");
            return -1;
        }
    }
    if (!hwa_gap_report_candidate_catalog_fits(
            result, &result->options)) {
        hwa_set_error(error, error_size,
                      "invalid Stage 9 source candidate catalog");
        return -1;
    }
    for (index = 0U; index < result->family_count; ++index) {
        const HWAGapReportFamily *row = &result->families[index];
        if (row->id != (uint64_t)index + UINT64_C(1) || row->key == NULL ||
            row->key[0] == '\0' || row->member_count == 0U ||
            row->primary_candidate_id == 0U ||
            row->primary_candidate_id > result->candidate_count ||
            (row->rank != 0U && row->rank > result->family_count)) {
            hwa_set_error(error, error_size, "invalid Stage 9 family rows");
            return -1;
        }
    }
    for (index = 0U; index < result->group_count; ++index) {
        const HWAGapReportGroup *row = &result->groups[index];
        if (row->id != (uint64_t)index + UINT64_C(1) ||
            hwa_gap_report_axis_name(row->axis) == NULL || row->value == NULL ||
            row->value[0] == '\0' ||
            row->available_count + row->missing_count + row->excluded_count !=
                row->candidate_count ||
            (row->statistics_valid &&
             (!isfinite(row->q05) || !isfinite(row->q25) ||
              !isfinite(row->median) || !isfinite(row->q75) ||
              !isfinite(row->q95) || !isfinite(row->spread))) ||
            (row->confidence_valid &&
             (!isfinite(row->confidence) || row->confidence < 0.0 ||
              row->confidence > 1.0)) ||
            (index != 0U && hwa_gap_group_compare(
                &result->groups[index - 1U], row) >= 0)) {
            hwa_set_error(error, error_size, "invalid Stage 9 group rows");
            return -1;
        }
    }
    for (index = 0U; index < result->case_count; ++index) {
        const HWAGapReportCase *row = &result->cases[index];
        if (row->id != (uint64_t)index + UINT64_C(1) ||
            row->candidate_id == 0U ||
            row->candidate_id > result->candidate_count ||
            row->case_id == NULL || row->case_id[0] == '\0' ||
            row->reason == NULL ||
            (index != 0U &&
             (result->cases[index - 1U].candidate_id > row->candidate_id ||
              (result->cases[index - 1U].candidate_id == row->candidate_id &&
               strcmp(result->cases[index - 1U].case_id,
                      row->case_id) >= 0))) ||
            hwa_gap_report_availability_name(row->availability) == NULL ||
            row->value_valid !=
                (row->availability == HWA_GAP_REPORT_AVAILABLE) ||
            (row->value_valid &&
             (!isfinite(row->value) || row->value < 0.0 ||
              row->value > 1.0)) ||
            (!row->value_valid && row->value != 0.0) ||
            (row->confidence_valid &&
             (!isfinite(row->confidence) || row->confidence < 0.0 ||
              row->confidence > 1.0)) ||
            (row->score_valid && (!isfinite(row->score) || row->score < 0.0 ||
                                  row->score > 1.0))) {
            hwa_set_error(error, error_size, "invalid Stage 9 case rows");
            return -1;
        }
    }
    if (!hwa_gap_report_case_catalog_fits(result, &result->options)) {
        hwa_set_error(error, error_size,
                      "invalid Stage 9 source case catalog");
        return -1;
    }
    for (index = 0U; index < result->excerpt_count; ++index) {
        const HWAGapReportExcerpt *row = &result->excerpts[index];
        const HWAGapReportSource *reference = hwa_gap_source_by_id(
            result, row->reference_source_id);
        const HWAGapReportSource *model = hwa_gap_source_by_id(
            result, row->model_source_id);
        const HWAGapReportCandidate *candidate = hwa_gap_candidate_by_source_row(
            result, row->candidate_source_id, row->candidate_row);
        uint64_t expected_bytes;
        int view_matches = candidate != NULL &&
            (row->view == HWA_GAP_REPORT_VIEW_RAW ||
             ((row->view == HWA_GAP_REPORT_VIEW_BROAD_EQ_MATCHED ||
               row->view == HWA_GAP_REPORT_VIEW_ROOM_MATCHED) &&
              candidate->kind == HWA_GAP_REPORT_CANDIDATE_PRODUCTION) ||
             ((row->view == HWA_GAP_REPORT_VIEW_STEM ||
               row->view == HWA_GAP_REPORT_VIEW_PROBE_LINKED) &&
              candidate->kind == HWA_GAP_REPORT_CANDIDATE_RUN_FEATURE));
        int available = row->availability == HWA_GAP_REPORT_AVAILABLE;
        if (hwa_gap_mul_u64(row->frame_count, UINT64_C(2),
                            &expected_bytes) != 0 ||
            hwa_gap_add_u64(&expected_bytes, UINT64_C(44)) != 0 ||
            expected_bytes > result->options.max_output_file_bytes) {
            hwa_set_error(error, error_size, "invalid Stage 9 excerpt size");
            return -1;
        }
        if (row->id != (uint64_t)index + UINT64_C(1) ||
            !hwa_gap_token_valid(row->name, 0) ||
            (index != 0U && strcmp(result->excerpts[index - 1U].name,
                                   row->name) >= 0) ||
            hwa_gap_report_view_name(row->view) == NULL ||
            (available && !view_matches) || row->candidate_row == 0U ||
            reference == NULL || model == NULL ||
            reference->kind != HWA_GAP_REPORT_SOURCE_WAVE ||
            model->kind != HWA_GAP_REPORT_SOURCE_WAVE ||
            row->frame_count == 0U ||
            row->frame_count > result->options.max_excerpt_frames ||
            row->reference_start_sample > result->options.max_input_frames ||
            row->model_start_sample > result->options.max_input_frames ||
            row->frame_count > result->options.max_input_frames -
                row->reference_start_sample ||
            row->frame_count > result->options.max_input_frames -
                row->model_start_sample ||
            hwa_gap_add_u64(&total_excerpt_frames, row->frame_count) != 0 ||
            total_excerpt_frames >
                result->options.max_total_excerpt_frames ||
            (row->make_x != 0 && row->make_x != 1) ||
            (row->x_is_reference != 0 && row->x_is_reference != 1) ||
            (available && row->make_x && row->x_is_reference !=
                hwa_gap_report_excerpt_x_is_reference(result, row)) ||
            hwa_gap_report_availability_name(row->availability) == NULL ||
            row->reason == NULL ||
            (available &&
             (row->reference_path == NULL || row->reference_path[0] == '\0' ||
              row->model_path == NULL || row->model_path[0] == '\0' ||
              !hwa_gap_hash_valid(row->reference_sha256) ||
              !hwa_gap_hash_valid(row->model_sha256) ||
              row->reference_file_bytes != expected_bytes ||
              row->model_file_bytes != expected_bytes ||
              row->reference_file_bytes >
                  result->options.max_output_file_bytes ||
              row->model_file_bytes >
                  result->options.max_output_file_bytes ||
              !isfinite(row->reference_gain_db) ||
              !isfinite(row->model_gain_db) ||
              row->reference_gain_db > 0.0 || row->model_gain_db > 0.0 ||
              (row->make_x &&
               (row->x_path == NULL || row->x_path[0] == '\0' ||
                !hwa_gap_hash_valid(row->x_sha256) ||
                row->x_file_bytes != expected_bytes ||
                row->x_file_bytes > result->options.max_output_file_bytes ||
                strcmp(row->x_sha256, row->x_is_reference ?
                    row->reference_sha256 : row->model_sha256) != 0)) ||
              (!row->make_x &&
               ((row->x_path != NULL && row->x_path[0] != '\0') ||
                row->x_sha256[0] != '\0' || row->x_file_bytes != 0U ||
                row->x_is_reference)))) ||
            (!available &&
             ((row->reference_path != NULL && row->reference_path[0] != '\0') ||
              (row->model_path != NULL && row->model_path[0] != '\0') ||
              (row->x_path != NULL && row->x_path[0] != '\0') ||
              row->reference_sha256[0] != '\0' ||
              row->model_sha256[0] != '\0' || row->x_sha256[0] != '\0' ||
              row->reference_file_bytes != 0U ||
              row->model_file_bytes != 0U || row->x_file_bytes != 0U ||
              row->reference_gain_db != 0.0 || row->model_gain_db != 0.0 ||
              row->x_is_reference != 0)) ||
            hwa_gap_add_u64(&output_bytes, row->reference_file_bytes) != 0 ||
            hwa_gap_add_u64(&output_bytes, row->model_file_bytes) != 0 ||
            hwa_gap_add_u64(&output_bytes, row->x_file_bytes) != 0) {
            hwa_set_error(error, error_size, "invalid Stage 9 excerpt rows");
            return -1;
        }
    }
    if (output_bytes != result->total_output_bytes) {
        hwa_set_error(error, error_size, "invalid Stage 9 output byte total");
        return -1;
    }
    if (result->total_output_bytes > result->options.max_bundle_bytes) {
        hwa_set_error(error, error_size, "invalid Stage 9 bundle byte total");
        return -1;
    }
    for (index = 0U; index < result->warning_count; ++index) {
        const HWAGapReportWarning *row = &result->warnings[index];
        if (row->id != (uint64_t)index + UINT64_C(1) || row->code == NULL ||
            row->code[0] == '\0' || row->message == NULL ||
            (row->source_id_valid &&
             hwa_gap_source_by_id(result, row->source_id) == NULL) ||
            (row->candidate_id_valid &&
             (row->candidate_id == 0U ||
              row->candidate_id > result->candidate_count)) ||
            (row->excerpt_id_valid &&
             (row->excerpt_id == 0U ||
              row->excerpt_id > result->excerpt_count)) ||
            row->source_id_valid != (row->source_id != 0U) ||
            row->candidate_id_valid != (row->candidate_id != 0U) ||
            row->excerpt_id_valid != (row->excerpt_id != 0U)) {
            hwa_set_error(error, error_size, "invalid Stage 9 warning rows");
            return -1;
        }
    }
    if (hwa_gap_report_result_retained_bytes(result, &retained) != 0 ||
        retained != result->retained_work_bytes ||
        retained > result->options.max_work_bytes) {
        hwa_set_error(error, error_size,
                      "invalid Stage 9 retained work accounting");
        return -1;
    }
    {
        HWAGapReportResult projected = *result;
        projected.options.max_work_bytes = UINT64_MAX;
        if (hwa_gap_report_result_peak_work_bytes(
                &projected, 0U, &peak) != 0 ||
            peak > result->options.max_work_bytes) {
            hwa_set_error(error, error_size,
                          "invalid Stage 9 peak work accounting");
            return -1;
        }
    }
    if (hwa_gap_derived_validate(result, error, error_size) != 0) return -1;
    return 0;
}

int hwa_gap_report_manifest_validate_bytes(
    const unsigned char *data,
    size_t size,
    const HWAGapReportOptions *options,
    char *error,
    size_t error_size)
{
    HWAGapReportOptions copied;
    HWAGapManifest manifest;
    HWAGapBlob blob;
    uint64_t live_work = 0U;
    int status = -1;
    memset(&manifest, 0, sizeof(manifest));
    memset(&blob, 0, sizeof(blob));
    if (options == NULL) hwa_gap_report_options_default(&copied);
    else copied = *options;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (data == NULL || size > copied.max_manifest_bytes ||
        (uint64_t)size == UINT64_MAX ||
        hwa_gap_report_options_validate(&copied, error, error_size) != 0 ||
        hwa_gap_work_add(&live_work,
                         (uint64_t)size + UINT64_C(1),
                         copied.max_work_bytes,
                         error, error_size) != 0) {
        if (error != NULL && error_size != 0U && error[0] == '\0')
            hwa_set_error(error, error_size,
                          "invalid Stage 9 manifest bytes");
        return -1;
    }
    blob.data = (unsigned char *)data;
    blob.size = size;
    if (hwa_gap_manifest_parse(&blob, &copied, &live_work, &manifest,
                               error, error_size) != 0) goto cleanup;
    if (!hwa_gap_source_order_valid(&manifest) ||
        !hwa_gap_label_order_valid(&manifest) ||
        !hwa_gap_excerpt_order_valid(&manifest) ||
        hwa_gap_compact_manifest(&manifest, &live_work) != 0) {
        hwa_set_error(error, error_size,
                      "Stage 9 manifest rows are not canonical");
        goto cleanup;
    }
    status = 0;
cleanup:
    hwa_gap_manifest_free(&manifest);
    return status;
}

int hwa_build_gap_report_files(
    const char *manifest_path,
    const HWARunBinding *bindings,
    size_t binding_count,
    const char *output_directory,
    HWAGapReportMode mode,
    const HWAGapReportOptions *options,
    HWAGapReportResult *result,
    char *error,
    size_t error_size)
{
    HWAGapReportOptions copied;
    HWAGapManifest manifest;
    HWAGapBlob manifest_blob;
    HWAGapIdentity manifest_identity;
    HWAGapIdentity *identities = NULL;
    uint64_t live_work = 0U;
    uint64_t adapter_evaluations = 0U;
    uint64_t runtime_evaluations = 0U;
    uint64_t stable_evaluations = 0U;
    uint64_t stable_start = 0U;
    uint64_t construction_peak = 0U;
    uint64_t identities_bytes = 0U;
    uint64_t rebuild_peak = 0U;
    uint64_t pre_adapter_work = 0U;
    size_t index;
    int status = -1;
    memset(&manifest, 0, sizeof(manifest));
    memset(&manifest_blob, 0, sizeof(manifest_blob));
    if (result == NULL) {
        hwa_set_error(error, error_size, "missing Stage 9 result");
        return -1;
    }
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (options == NULL) hwa_gap_report_options_default(&copied);
    else copied = *options;
    memset(result, 0, sizeof(*result));
    result->options = copied;
    result->mode = mode;
    if (hwa_gap_report_options_validate(&copied, error, error_size) != 0 ||
        hwa_gap_report_mode_name(mode) == NULL || manifest_path == NULL ||
        (binding_count != 0U && bindings == NULL) ||
        (mode == HWA_GAP_REPORT_RANK && output_directory != NULL) ||
        (mode != HWA_GAP_REPORT_RANK &&
         (output_directory == NULL || output_directory[0] == '\0'))) {
        hwa_set_error(error, error_size, "invalid Stage 9 arguments");
        return -1;
    }
    if (hwa_gap_blob_read(manifest_path, copied.max_manifest_bytes,
                          copied.max_work_bytes, &live_work, &manifest_blob,
                          &manifest_identity, result->manifest_sha256,
                          error, error_size) != 0 ||
        hwa_gap_manifest_parse(&manifest_blob, &copied, &live_work, &manifest,
                               error, error_size) != 0) goto cleanup;
    if (!hwa_gap_source_order_valid(&manifest) ||
        !hwa_gap_label_order_valid(&manifest) ||
        !hwa_gap_excerpt_order_valid(&manifest) ||
        binding_count != manifest.source_count ||
        hwa_gap_compact_manifest(&manifest, &live_work) != 0) {
        hwa_set_error(error, error_size,
                      "Stage 9 manifest rows or bindings are not canonical");
        goto cleanup;
    }
    construction_peak = live_work;
    if (hwa_gap_add_u64(&construction_peak, (uint64_t)sizeof(*result)) != 0 ||
        hwa_gap_array_bytes(manifest.source_count, sizeof(*identities),
                            &identities_bytes) != 0 ||
        hwa_gap_add_u64(&construction_peak, identities_bytes) != 0 ||
        hwa_gap_array_bytes(manifest.label_count,
            sizeof(*result->labels), &construction_peak) != 0 ||
        hwa_gap_array_bytes(manifest.excerpt_count,
            sizeof(*result->excerpts), &construction_peak) != 0 ||
        hwa_gap_string_bytes(manifest_path, &construction_peak) != 0 ||
        hwa_gap_string_bytes(HWA_GAP_REPORT_AUDIBILITY_METHOD,
                             &construction_peak) != 0 ||
        (output_directory != NULL &&
         (hwa_gap_add_u64(&construction_peak,
              (uint64_t)strlen(output_directory) +
                  (uint64_t)HWA_GAP_PATH_BOUND +
                  UINT64_C(2)) != 0)) ||
        hwa_gap_mul_u64((uint64_t)manifest.excerpt_count,
            (uint64_t)(strlen(mode == HWA_GAP_REPORT_RANK ?
                "rank-mode-does-not-render-excerpts" :
                "excerpt-not-rendered") + 4U), &rebuild_peak) != 0 ||
        hwa_gap_add_u64(&construction_peak, rebuild_peak) != 0) {
        hwa_set_error(error, error_size,
                      "Stage 9 construction work size overflows");
        goto cleanup;
    }
    for (index = 0U; index < manifest.source_count; ++index) {
        const HWARunBinding *binding = hwa_gap_binding(
            bindings, binding_count, manifest.sources[index].name);
        if (binding == NULL || binding->path == NULL ||
            binding->path[0] == '\0' || strcmp(binding->path, "-") == 0 ||
            hwa_gap_string_bytes(binding->path, &construction_peak) != 0) {
            hwa_set_error(error, error_size,
                          "invalid Stage 9 source binding");
            goto cleanup;
        }
    }
    if (construction_peak > copied.max_work_bytes) {
        hwa_set_error(error, error_size,
                      "Stage 9 construction exceeds its work cap");
        goto cleanup;
    }
    result->manifest_path = hwa_gap_strdup(manifest_path);
    result->output_directory = output_directory == NULL ? NULL :
        hwa_gap_absolute_output(output_directory);
    result->title = manifest.title; manifest.title = NULL;
    result->audibility_method = hwa_gap_strdup(
        HWA_GAP_REPORT_AUDIBILITY_METHOD);
    if (result->manifest_path == NULL || result->audibility_method == NULL ||
        (output_directory != NULL && result->output_directory == NULL))
        goto cleanup;
    result->sources = manifest.sources;
    result->source_count = manifest.source_count;
    manifest.sources = NULL;
    manifest.source_count = 0U;
    manifest.source_capacity = 0U;
    identities = result->source_count == 0U ? NULL :
        (HWAGapIdentity *)calloc(result->source_count, sizeof(*identities));
    if (result->source_count != 0U && identities == NULL) goto cleanup;
    result->total_input_bytes = 0U;
    for (index = 0U; index < result->source_count; ++index) {
        HWAGapReportSource *source = &result->sources[index];
        const HWARunBinding *binding = hwa_gap_binding(
            bindings, binding_count, source->name);
        char actual_hash[HWA_SHA256_HEX_SIZE];
        uint64_t source_limit = hwa_gap_source_input_limit(&copied,
                                                            source->kind);
        size_t prior;
        if (binding == NULL || binding->path == NULL ||
            binding->path[0] == '\0' || strcmp(binding->path, "-") == 0 ||
            source_limit == 0U ||
            hwa_gap_hash_stable(binding->path, source_limit,
                                &identities[index], actual_hash,
                                error, error_size) != 0 ||
            strcmp(actual_hash, source->sha256) != 0) {
            hwa_set_error(error, error_size,
                          "Stage 9 source binding or hash mismatch");
            goto cleanup;
        }
        for (prior = 0U; prior < index; ++prior)
            if (hwa_gap_identity_equal(&identities[prior], &identities[index])) {
                hwa_set_error(error, error_size,
                              "Stage 9 source bindings alias one file");
                goto cleanup;
            }
        source->id = (uint64_t)index + UINT64_C(1);
        source->path = hwa_gap_strdup(binding->path);
        source->file_bytes = identities[index].size;
        if (source->path == NULL ||
            hwa_gap_add_u64(&result->total_input_bytes,
                            source->file_bytes) != 0) {
            hwa_set_error(error, error_size,
                          "Stage 9 total input byte count overflow");
            goto cleanup;
        }
    }
    for (index = 0U; index < binding_count; ++index)
        if (bindings[index].id == NULL ||
            hwa_gap_source_by_name(result, bindings[index].id) == NULL) {
            hwa_set_error(error, error_size, "extra Stage 9 source binding");
            goto cleanup;
        }
    if (hwa_gap_transfer_labels(&manifest, result, error, error_size) != 0 ||
        hwa_gap_transfer_excerpts(&manifest, result, error, error_size) != 0)
        goto cleanup;
    hwa_gap_blob_free(&manifest_blob, &live_work);
    hwa_gap_manifest_free(&manifest);
    if (hwa_gap_report_result_retained_bytes(result, &pre_adapter_work) != 0 ||
        hwa_gap_add_u64(&pre_adapter_work, identities_bytes) != 0 ||
        pre_adapter_work > copied.max_work_bytes) {
        hwa_set_error(error, error_size,
                      "Stage 9 retained manifest exceeds its work cap");
        goto cleanup;
    }
    live_work = pre_adapter_work;
    if (hwa_gap_adapt_sources(result, &live_work, &adapter_evaluations,
                              error, error_size) != 0 ||
        hwa_gap_report_result_peak_work_bytes(
            result, identities_bytes, &rebuild_peak) != 0 ||
        rebuild_peak > copied.max_work_bytes ||
        hwa_gap_add_u64(&adapter_evaluations,
                        (uint64_t)result->candidate_count) != 0 ||
        hwa_gap_add_u64(&adapter_evaluations,
                        (uint64_t)result->case_count) != 0 ||
        (mode != HWA_GAP_REPORT_RANK &&
         hwa_gap_add_u64(&adapter_evaluations,
                         (uint64_t)result->excerpt_count) != 0) ||
        adapter_evaluations > copied.max_evaluations ||
        hwa_gap_report_result_rebuild_impl(
            result, adapter_evaluations, identities_bytes,
            error, error_size) != 0)
        goto cleanup;
    runtime_evaluations = result->evaluation_count;
    if (runtime_evaluations < adapter_evaluations ||
        hwa_gap_add_u64(&stable_start, (uint64_t)result->source_count) != 0 ||
        hwa_gap_add_u64(&stable_start,
                        (uint64_t)result->candidate_count) != 0 ||
        hwa_gap_add_u64(&stable_start, (uint64_t)result->case_count) != 0 ||
        (mode != HWA_GAP_REPORT_RANK &&
         hwa_gap_add_u64(&stable_start,
                         (uint64_t)result->excerpt_count) != 0))
        goto cleanup;
    stable_evaluations = runtime_evaluations - adapter_evaluations;
    if (hwa_gap_add_u64(&stable_evaluations, stable_start) != 0)
        goto cleanup;
    result->evaluation_count = stable_evaluations;
    for (index = 0U; index < result->source_count; ++index) {
        HWAGapIdentity final_identity;
        char final_hash[HWA_SHA256_HEX_SIZE];
        uint64_t source_limit = hwa_gap_source_input_limit(
            &copied, result->sources[index].kind);
        if (hwa_gap_hash_stable(result->sources[index].path,
                                source_limit, &final_identity,
                                final_hash, error, error_size) != 0 ||
            !hwa_gap_identity_equal(&identities[index], &final_identity) ||
            strcmp(final_hash, result->sources[index].sha256) != 0) {
            hwa_set_error(error, error_size,
                          "Stage 9 source changed during analysis");
            goto cleanup;
        }
    }
    {
        HWAGapIdentity final_manifest_identity;
        char final_manifest_hash[HWA_SHA256_HEX_SIZE];
        if (hwa_gap_hash_stable(result->manifest_path,
                copied.max_manifest_bytes, &final_manifest_identity,
                final_manifest_hash, error, error_size) != 0 ||
            !hwa_gap_identity_equal(&manifest_identity,
                                    &final_manifest_identity) ||
            strcmp(final_manifest_hash, result->manifest_sha256) != 0) {
            hwa_set_error(error, error_size,
                          "Stage 9 manifest changed during analysis");
            goto cleanup;
        }
    }
    free(identities);
    identities = NULL;
    if (hwa_gap_report_result_retained_bytes(
            result, &result->retained_work_bytes) != 0 ||
        result->retained_work_bytes > copied.max_work_bytes ||
        hwa_gap_report_result_validate(result, error, error_size) != 0)
        goto cleanup;
    if (mode != HWA_GAP_REPORT_RANK) {
        if (hwa_gap_prepare_excerpts(
                result, &runtime_evaluations, error, error_size) != 0 ||
            hwa_gap_report_build_clip_bundle(
                result, runtime_evaluations, error, error_size) != 0)
            goto cleanup;
    }
    if (hwa_gap_report_result_retained_bytes(
            result, &result->retained_work_bytes) != 0 ||
        result->retained_work_bytes > copied.max_work_bytes ||
        hwa_gap_report_result_validate(result, error, error_size) != 0)
        goto cleanup;
    status = 0;
cleanup:
    free(identities);
    hwa_gap_blob_free(&manifest_blob, &live_work);
    hwa_gap_manifest_free(&manifest);
    if (status != 0) hwa_gap_report_result_free(result);
    return status;
}

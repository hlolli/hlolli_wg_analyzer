#if !defined(_WIN32)
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "run.h"

#include "dsp.h"
#include "features.h"
#include "internal.h"
#include "numeric_locale.h"
#include "sha256.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
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

#define HWA_RUN_FEATURES_PER_CLOCK (2U + HWA_BAND_COUNT)
#define HWA_RUN_STAGE_COUNT 3U
#define HWA_RUN_LINK_WINDOW_MILLISECONDS 20U
#define HWA_RUN_LINK_HOP_MILLISECONDS 10U
#define HWA_RUN_LINK_MAX_LAG_MILLISECONDS 250U
#define HWA_RUN_MIN_LINK_PAIRS 32U

typedef struct HWARunName {
    int value;
    const char *name;
} HWARunName;

static const HWARunName hwa_run_availability_names[] = {
    {HWA_RUN_AVAILABLE, "available"},
    {HWA_RUN_UNAVAILABLE, "unavailable"},
    {HWA_RUN_INSUFFICIENT, "insufficient"}
};

static const HWARunName hwa_run_source_kind_names[] = {
    {HWA_RUN_SOURCE_STEM, "stem"},
    {HWA_RUN_SOURCE_PROBE, "probe"}
};

static const HWARunName hwa_run_side_names[] = {
    {HWA_RUN_REFERENCE, "reference"},
    {HWA_RUN_MODEL, "model"}
};

static const HWARunName hwa_run_role_names[] = {
    {HWA_RUN_STEM_SOURCE, "source"},
    {HWA_RUN_STEM_BODY, "body"},
    {HWA_RUN_STEM_WET, "wet"},
    {HWA_RUN_STEM_FINAL, "final"},
    {HWA_RUN_STEM_ROOM, "room"},
    {HWA_RUN_STEM_NOISE, "noise"}
};

static const HWARunName hwa_run_probe_format_names[] = {
    {HWA_RUN_PROBE_CSV_F64, "csv-f64"},
    {HWA_RUN_PROBE_BINARY_F64LE, "binary-f64le"}
};

static const HWARunName hwa_run_feature_names[] = {
    {HWA_RUN_FEATURE_RMS_DBFS, "rms_dbfs"},
    {HWA_RUN_FEATURE_CREST_DB, "crest_db"},
    {HWA_RUN_FEATURE_BAND_LEVEL_DBFS, "band_level_dbfs"}
};

static const HWARunName hwa_run_unit_names[] = {
    {HWA_RUN_UNIT_DBFS, "dBFS"},
    {HWA_RUN_UNIT_DB, "dB"},
    {HWA_RUN_UNIT_RATIO, "ratio"},
    {HWA_RUN_UNIT_SAMPLES, "samples"},
    {HWA_RUN_UNIT_PPM, "ppm"}
};

static const char *hwa_run_name_for(const HWARunName *names,
                                    size_t count,
                                    int value)
{
    size_t index;
    for (index = 0U; index < count; ++index) {
        if (names[index].value == value) return names[index].name;
    }
    return "unknown";
}

const char *hwa_run_availability_name(HWARunAvailability value)
{
    return hwa_run_name_for(hwa_run_availability_names,
                            sizeof(hwa_run_availability_names) /
                                sizeof(hwa_run_availability_names[0]),
                            (int)value);
}

const char *hwa_run_source_kind_name(HWARunSourceKind value)
{
    return hwa_run_name_for(hwa_run_source_kind_names,
                            sizeof(hwa_run_source_kind_names) /
                                sizeof(hwa_run_source_kind_names[0]),
                            (int)value);
}

const char *hwa_run_side_name(HWARunSide value)
{
    return hwa_run_name_for(hwa_run_side_names,
                            sizeof(hwa_run_side_names) /
                                sizeof(hwa_run_side_names[0]),
                            (int)value);
}

const char *hwa_run_stem_role_name(HWARunStemRole value)
{
    return hwa_run_name_for(hwa_run_role_names,
                            sizeof(hwa_run_role_names) /
                                sizeof(hwa_run_role_names[0]),
                            (int)value);
}

const char *hwa_run_probe_format_name(HWARunProbeFormat value)
{
    return hwa_run_name_for(hwa_run_probe_format_names,
                            sizeof(hwa_run_probe_format_names) /
                                sizeof(hwa_run_probe_format_names[0]),
                            (int)value);
}

const char *hwa_run_feature_kind_name(HWARunFeatureKind value)
{
    return hwa_run_name_for(hwa_run_feature_names,
                            sizeof(hwa_run_feature_names) /
                                sizeof(hwa_run_feature_names[0]),
                            (int)value);
}

const char *hwa_run_unit_name(HWARunUnit value)
{
    return hwa_run_name_for(hwa_run_unit_names,
                            sizeof(hwa_run_unit_names) /
                                sizeof(hwa_run_unit_names[0]),
                            (int)value);
}

void hwa_run_options_default(HWARunOptions *options)
{
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    options->decode_block_frames = 4096U;
    options->max_manifest_bytes = UINT64_C(1048576);
    options->max_input_bytes = UINT64_C(17179869184);
    options->max_input_frames = UINT64_C(2000000000);
    options->max_probe_bytes = UINT64_C(1073741824);
    options->max_probe_values = UINT64_C(16000000);
    options->max_work_bytes = UINT64_C(536870912);
    options->max_evaluations = UINT64_C(250000000);
    options->max_stems = 16U;
    options->max_probes = 128U;
    options->max_links = 256U;
    options->max_json_depth = 8U;
    options->max_json_tokens = 65536U;
    options->max_result_rows = 1000000U;
    options->max_warnings = 100000U;
}

void hwa_run_result_free(HWARunResult *result)
{
    size_t index;
    if (result == NULL) return;
    free(result->manifest_path);
    if (result->sources != NULL) {
        for (index = 0U; index < result->source_count; ++index) {
            free(result->sources[index].binding_id);
            free(result->sources[index].path);
            free(result->sources[index].probe_name);
            free(result->sources[index].unit);
        }
    }
    if (result->warnings != NULL) {
        for (index = 0U; index < result->warning_count; ++index) {
            free(result->warnings[index].code);
            free(result->warnings[index].message);
        }
    }
    free(result->sources);
    free(result->clocks);
    free(result->features);
    free(result->stages);
    free(result->probes);
    free(result->links);
    free(result->warnings);
    memset(result, 0, sizeof(*result));
}

static int hwa_run_add_u64(uint64_t *total, uint64_t value)
{
    if (total == NULL || value > UINT64_MAX - *total) return -1;
    *total += value;
    return 0;
}

static int hwa_run_array_bytes(size_t count,
                               size_t element_size,
                               uint64_t *bytes)
{
    if (element_size != 0U && count > SIZE_MAX / element_size) return -1;
    return hwa_run_add_u64(bytes, (uint64_t)(count * element_size));
}

static int hwa_run_string_bytes(const char *text, uint64_t *bytes)
{
    size_t length;
    if (text == NULL) return 0;
    length = strlen(text);
    if (length == SIZE_MAX) return -1;
    return hwa_run_add_u64(bytes, (uint64_t)length + UINT64_C(1));
}

int hwa_run_result_retained_bytes(const HWARunResult *result,
                                  uint64_t *bytes)
{
    uint64_t total = 0U;
    size_t index;
    if (result == NULL || bytes == NULL ||
        hwa_run_string_bytes(result->manifest_path, &total) != 0 ||
        hwa_run_array_bytes(result->source_count, sizeof(*result->sources),
                            &total) != 0 ||
        hwa_run_array_bytes(result->clock_count, sizeof(*result->clocks),
                            &total) != 0 ||
        hwa_run_array_bytes(result->feature_count, sizeof(*result->features),
                            &total) != 0 ||
        hwa_run_array_bytes(result->stage_count, sizeof(*result->stages),
                            &total) != 0 ||
        hwa_run_array_bytes(result->probe_count, sizeof(*result->probes),
                            &total) != 0 ||
        hwa_run_array_bytes(result->link_count, sizeof(*result->links),
                            &total) != 0 ||
        hwa_run_array_bytes(result->warning_count, sizeof(*result->warnings),
                            &total) != 0) return -1;
    for (index = 0U; index < result->source_count; ++index) {
        const HWARunSource *source = &result->sources[index];
        if (hwa_run_string_bytes(source->binding_id, &total) != 0 ||
            hwa_run_string_bytes(source->path, &total) != 0 ||
            hwa_run_string_bytes(source->probe_name, &total) != 0 ||
            hwa_run_string_bytes(source->unit, &total) != 0) return -1;
    }
    for (index = 0U; index < result->warning_count; ++index) {
        if (hwa_run_string_bytes(result->warnings[index].code, &total) != 0 ||
            hwa_run_string_bytes(result->warnings[index].message, &total) != 0)
            return -1;
    }
    *bytes = total;
    return 0;
}

static int hwa_run_lower_hash(const char hash[HWA_SHA256_HEX_SIZE])
{
    size_t index;
    if (hash == NULL) return 0;
    for (index = 0U; index < HWA_SHA256_HEX_SIZE - 1U; ++index) {
        if (!((hash[index] >= '0' && hash[index] <= '9') ||
              (hash[index] >= 'a' && hash[index] <= 'f'))) return 0;
    }
    return hash[HWA_SHA256_HEX_SIZE - 1U] == '\0';
}

size_t hwa_run_feature_catalog_count(void)
{
    return HWA_RUN_FEATURES_PER_CLOCK;
}

int hwa_run_feature_catalog_at(size_t offset,
                               HWARunFeatureKind *kind,
                               uint32_t *index,
                               HWARunUnit *unit)
{
    if (kind == NULL || index == NULL || unit == NULL ||
        offset >= HWA_RUN_FEATURES_PER_CLOCK) return -1;
    if (offset == 0U) {
        *kind = HWA_RUN_FEATURE_RMS_DBFS;
        *index = 0U;
        *unit = HWA_RUN_UNIT_DBFS;
    } else if (offset == 1U) {
        *kind = HWA_RUN_FEATURE_CREST_DB;
        *index = 0U;
        *unit = HWA_RUN_UNIT_DB;
    } else {
        *kind = HWA_RUN_FEATURE_BAND_LEVEL_DBFS;
        *index = (uint32_t)(offset - 2U);
        *unit = HWA_RUN_UNIT_DBFS;
    }
    return 0;
}

size_t hwa_run_stage_catalog_count(void)
{
    return HWA_RUN_STAGE_COUNT;
}

int hwa_run_stage_catalog_at(size_t offset,
                             HWARunStemRole *from_role,
                             HWARunStemRole *to_role)
{
    if (from_role == NULL || to_role == NULL ||
        offset >= HWA_RUN_STAGE_COUNT) return -1;
    *from_role = (HWARunStemRole)(offset + 1U);
    *to_role = (HWARunStemRole)(offset + 2U);
    return 0;
}

int hwa_run_source_canonical_compare(const void *left, const void *right)
{
    const HWARunSource *a = (const HWARunSource *)left;
    const HWARunSource *b = (const HWARunSource *)right;
    if (a == NULL || b == NULL || a->binding_id == NULL ||
        b->binding_id == NULL) return 0;
    return strcmp(a->binding_id, b->binding_id);
}

int hwa_run_clock_canonical_compare(const void *left, const void *right)
{
    const HWARunClock *a = (const HWARunClock *)left;
    const HWARunClock *b = (const HWARunClock *)right;
    return a->role < b->role ? -1 : a->role > b->role ? 1 : 0;
}

int hwa_run_feature_canonical_compare(const void *left, const void *right)
{
    const HWARunFeature *a = (const HWARunFeature *)left;
    const HWARunFeature *b = (const HWARunFeature *)right;
    if (a->clock_id != b->clock_id)
        return a->clock_id < b->clock_id ? -1 : 1;
    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
    return a->index < b->index ? -1 : a->index > b->index ? 1 : 0;
}

int hwa_run_stage_canonical_compare(const void *left, const void *right)
{
    const HWARunStage *a = (const HWARunStage *)left;
    const HWARunStage *b = (const HWARunStage *)right;
    return a->from_role < b->from_role
               ? -1
               : a->from_role > b->from_role ? 1 : 0;
}

int hwa_run_probe_canonical_compare(const void *left, const void *right)
{
    const HWARunProbe *a = (const HWARunProbe *)left;
    const HWARunProbe *b = (const HWARunProbe *)right;
    return a->source_id < b->source_id
               ? -1
               : a->source_id > b->source_id ? 1 : 0;
}

int hwa_run_link_canonical_compare(const void *left, const void *right)
{
    const HWARunLink *a = (const HWARunLink *)left;
    const HWARunLink *b = (const HWARunLink *)right;
    if (a->stem_source_id != b->stem_source_id)
        return a->stem_source_id < b->stem_source_id ? -1 : 1;
    if (a->probe_source_id != b->probe_source_id)
        return a->probe_source_id < b->probe_source_id ? -1 : 1;
    if (a->feature != b->feature) return a->feature < b->feature ? -1 : 1;
    return a->feature_index < b->feature_index
               ? -1
               : a->feature_index > b->feature_index ? 1 : 0;
}

static const HWARunClock *hwa_run_clock_for_role(const HWARunResult *result,
                                                 HWARunStemRole role)
{
    size_t index;
    for (index = 0U; index < result->clock_count; ++index) {
        if (result->clocks[index].role == role) return &result->clocks[index];
    }
    return NULL;
}

static int hwa_run_role_gap(const HWARunResult *result,
                            HWARunStemRole role,
                            double *gap)
{
    const HWARunClock *clock = hwa_run_clock_for_role(result, role);
    long double sum = 0.0L;
    size_t count = 0U;
    size_t index;
    if (clock == NULL || gap == NULL) return -1;
    for (index = 0U; index < result->feature_count; ++index) {
        const HWARunFeature *feature = &result->features[index];
        if (feature->clock_id == clock->id && feature->gap_valid) {
            sum += (long double)feature->normalized_gap;
            count++;
        }
    }
    if (count != HWA_RUN_FEATURES_PER_CLOCK) return -1;
    *gap = (double)(sum / (long double)count);
    return isfinite(*gap) ? 0 : -1;
}

static int hwa_run_stage_rank_before(const HWARunStage *a,
                                     const HWARunStage *b)
{
    if (a->added_gap > b->added_gap) return 1;
    if (a->added_gap < b->added_gap) return 0;
    return a->id < b->id;
}

int hwa_run_stage_rows_rebuild(HWARunResult *result,
                               char *error,
                               size_t error_size)
{
    size_t index;
    if (error != NULL && error_size > 0U) error[0] = '\0';
    if (result == NULL || result->stages == NULL ||
        result->stage_count != HWA_RUN_STAGE_COUNT) {
        hwa_set_error(error, error_size, "invalid Stage 7 stage rebuild");
        return -1;
    }
    for (index = 0U; index < HWA_RUN_STAGE_COUNT; ++index) {
        HWARunStage *stage = &result->stages[index];
        double prior = 0.0;
        double current = 0.0;
        memset(stage, 0, sizeof(*stage));
        stage->id = (uint64_t)index + UINT64_C(1);
        if (hwa_run_stage_catalog_at(index, &stage->from_role,
                                     &stage->to_role) != 0) return -1;
        stage->quality_flags = HWA_RUN_QUALITY_STAGE_CONFOUNDED;
        if (hwa_run_role_gap(result, stage->from_role, &prior) == 0 &&
            hwa_run_role_gap(result, stage->to_role, &current) == 0) {
            stage->availability = HWA_RUN_AVAILABLE;
            stage->prior_gap = prior;
            stage->current_gap = current;
            stage->added_gap = current - prior;
            if (stage->added_gap == 0.0) stage->added_gap = 0.0;
            stage->gap_valid = 1;
        } else {
            stage->availability = HWA_RUN_UNAVAILABLE;
        }
    }
    for (index = 0U; index < HWA_RUN_STAGE_COUNT; ++index) {
        HWARunStage *stage = &result->stages[index];
        size_t other;
        size_t rank = 1U;
        if (!stage->gap_valid) continue;
        for (other = 0U; other < HWA_RUN_STAGE_COUNT; ++other) {
            const HWARunStage *candidate = &result->stages[other];
            if (candidate->gap_valid &&
                hwa_run_stage_rank_before(candidate, stage)) rank++;
        }
        stage->rank = rank;
    }
    return 0;
}

size_t hwa_run_warning_spec_count(const HWARunResult *result)
{
    size_t count = 0U;
    size_t index;
    if (result == NULL) return 0U;
    for (index = 0U; index < result->clock_count; ++index)
        if (result->clocks[index].drift_samples != 0) count++;
    for (index = 0U; index < result->stage_count; ++index)
        if (result->stages[index].gap_valid &&
            result->stages[index].added_gap > 0.0) count++;
    for (index = 0U; index < result->link_count; ++index)
        if (result->links[index].availability != HWA_RUN_AVAILABLE) count++;
    return count;
}

int hwa_run_warning_spec_at(const HWARunResult *result,
                            size_t offset,
                            HWARunWarningSpec *spec)
{
    size_t cursor = 0U;
    size_t index;
    if (result == NULL || spec == NULL) return -1;
    memset(spec, 0, sizeof(*spec));
    for (index = 0U; index < result->clock_count; ++index) {
        if (result->clocks[index].drift_samples != 0) {
            if (cursor++ == offset) {
                spec->code = "clock-end-drift";
                spec->message =
                    "The paired stems end at different common-clock samples; this may be clock drift or unequal trims.";
                spec->clock_id = result->clocks[index].id;
                spec->clock_id_valid = 1;
                return 0;
            }
        }
    }
    for (index = 0U; index < result->stage_count; ++index) {
        if (result->stages[index].gap_valid &&
            result->stages[index].added_gap > 0.0) {
            if (cursor++ == offset) {
                spec->code = "stage-cause-unproven";
                spec->message =
                    "The measured gap grows at this declared tap; the result does not prove cause.";
                spec->stage_id = result->stages[index].id;
                spec->stage_id_valid = 1;
                return 0;
            }
        }
    }
    for (index = 0U; index < result->link_count; ++index) {
        if (result->links[index].availability != HWA_RUN_AVAILABLE) {
            if (cursor++ == offset) {
                spec->code = "link-insufficient";
                spec->message =
                    "The declared probe link has too few usable pairs or no variance.";
                spec->link_id = result->links[index].id;
                spec->link_id_valid = 1;
                return 0;
            }
        }
    }
    return -1;
}

static char *hwa_run_copy_string(const char *text)
{
    size_t length;
    char *copy;
    if (text == NULL) return NULL;
    length = strlen(text);
    if (length == SIZE_MAX) return NULL;
    copy = (char *)malloc(length + 1U);
    if (copy != NULL) memcpy(copy, text, length + 1U);
    return copy;
}

int hwa_run_warnings_rebuild(HWARunResult *result,
                             char *error,
                             size_t error_size)
{
    size_t count;
    size_t index;
    if (error != NULL && error_size > 0U) error[0] = '\0';
    if (result == NULL) {
        hwa_set_error(error, error_size, "invalid Stage 7 warning rebuild");
        return -1;
    }
    if (result->warnings != NULL) {
        for (index = 0U; index < result->warning_count; ++index) {
            free(result->warnings[index].code);
            free(result->warnings[index].message);
        }
    }
    free(result->warnings);
    result->warnings = NULL;
    result->warning_count = 0U;
    count = hwa_run_warning_spec_count(result);
    if (count > result->options.max_warnings) {
        hwa_set_error(error, error_size, "Stage 7 warnings exceed their cap");
        return -1;
    }
    if (count != 0U) {
        result->warnings = (HWARunWarning *)calloc(
            count, sizeof(*result->warnings));
        if (result->warnings == NULL) {
            hwa_set_error(error, error_size,
                          "cannot allocate Stage 7 warnings");
            return -1;
        }
    }
    for (index = 0U; index < count; ++index) {
        HWARunWarningSpec spec;
        HWARunWarning *warning = &result->warnings[index];
        if (hwa_run_warning_spec_at(result, index, &spec) != 0) goto failed;
        warning->id = (uint64_t)index + UINT64_C(1);
        warning->code = hwa_run_copy_string(spec.code);
        warning->message = hwa_run_copy_string(spec.message);
        warning->source_id = spec.source_id;
        warning->clock_id = spec.clock_id;
        warning->stage_id = spec.stage_id;
        warning->link_id = spec.link_id;
        warning->source_id_valid = spec.source_id_valid;
        warning->clock_id_valid = spec.clock_id_valid;
        warning->stage_id_valid = spec.stage_id_valid;
        warning->link_id_valid = spec.link_id_valid;
        if (warning->code == NULL || warning->message == NULL) goto failed;
    }
    result->warning_count = count;
    return 0;
failed:
    result->warning_count = count;
    for (index = 0U; index < count; ++index) {
        free(result->warnings[index].code);
        free(result->warnings[index].message);
    }
    free(result->warnings);
    result->warnings = NULL;
    result->warning_count = 0U;
    hwa_set_error(error, error_size, "cannot build Stage 7 warnings");
    return -1;
}

static int hwa_run_options_valid(const HWARunOptions *options)
{
    return options != NULL && options->decode_block_frames != 0U &&
           options->decode_block_frames <= 1048576U &&
           options->max_manifest_bytes != 0U &&
           options->max_input_bytes != 0U &&
           options->max_input_frames != 0U &&
           options->max_probe_bytes != 0U &&
           options->max_probe_values != 0U &&
           options->max_work_bytes != 0U &&
           options->max_evaluations != 0U &&
           options->max_stems != 0U && options->max_probes != 0U &&
           options->max_links != 0U && options->max_json_depth != 0U &&
           options->max_json_tokens != 0U &&
           options->max_result_rows != 0U && options->max_warnings != 0U;
}

static int hwa_run_token_valid(const char *text,
                               size_t maximum,
                               int require_namespace);
static int hwa_run_unit_valid(const char *text);
static uint64_t hwa_run_gcd_u64(uint64_t a, uint64_t b);
static int hwa_run_u64_multiply(uint64_t left,
                                uint64_t right,
                                uint64_t *product);
static int hwa_run_i64_end(int64_t start, uint64_t frames, int64_t *end);
static int hwa_run_work_reserve(uint64_t *live,
                                uint64_t amount,
                                uint64_t maximum,
                                char *error,
                                size_t error_size);

static int hwa_run_canonical_zero(double value)
{
    return value == 0.0 && !signbit(value);
}

static int hwa_run_boolean(int value)
{
    return value == 0 || value == 1;
}

static int hwa_run_size_add(size_t *total, size_t value)
{
    if (total == NULL || value > SIZE_MAX - *total) return -1;
    *total += value;
    return 0;
}

static unsigned hwa_run_bit_count32(uint32_t value)
{
    unsigned count = 0U;
    while (value != 0U) {
        count += value & UINT32_C(1);
        value >>= 1U;
    }
    return count;
}

int hwa_run_evaluations_expected(const HWARunResult *result,
                                 uint64_t *expected)
{
    uint64_t total = 0U;
    size_t index;
    if (result == NULL || expected == NULL ||
        (result->source_count != 0U && result->sources == NULL) ||
        (result->clock_count != 0U && result->clocks == NULL) ||
        (result->link_count != 0U && result->links == NULL)) return -1;
    for (index = 0U; index < result->source_count; ++index) {
        const HWARunSource *source = &result->sources[index];
        uint64_t visits;
        if (source->kind == HWA_RUN_SOURCE_STEM) {
            if (hwa_run_u64_multiply(source->format.frames,
                                     source->format.channels,
                                     &visits) != 0 ||
                hwa_run_add_u64(&total, visits) != 0) return -1;
        } else if (source->kind == HWA_RUN_SOURCE_PROBE) {
            if (hwa_run_add_u64(&total, source->value_count) != 0) return -1;
        } else {
            return -1;
        }
    }
    for (index = 0U; index < result->clock_count; ++index) {
        uint64_t overlap = result->clocks[index].overlap_frames;
        uint64_t visits;
        if (hwa_run_u64_multiply(overlap, UINT64_C(2), &visits) != 0 ||
            hwa_run_add_u64(&total, visits) != 0) return -1;
        if (overlap >= UINT64_C(1024)) {
            uint64_t transforms = UINT64_C(1) +
                (overlap - UINT64_C(1024)) / UINT64_C(512);
            if (hwa_run_u64_multiply(transforms, UINT64_C(1024),
                                     &visits) != 0 ||
                hwa_run_u64_multiply(visits, UINT64_C(2), &visits) != 0 ||
                hwa_run_add_u64(&total, visits) != 0) return -1;
        }
    }
    for (index = 0U; index < result->link_count; ++index) {
        const HWARunLink *link = &result->links[index];
        const HWARunSource *stem;
        uint64_t window_frames;
        uint64_t hop_frames;
        uint64_t window_count;
        uint64_t max_lag_hops;
        uint64_t lag_count;
        uint64_t visits;
        if (link->stem_source_id == 0U ||
            link->stem_source_id > result->source_count) return -1;
        stem = &result->sources[link->stem_source_id - 1U];
        window_frames = ((uint64_t)result->clock_rate_hz * UINT64_C(20) +
                         UINT64_C(500)) / UINT64_C(1000);
        hop_frames = ((uint64_t)result->clock_rate_hz * UINT64_C(10) +
                      UINT64_C(500)) / UINT64_C(1000);
        if (window_frames == 0U) window_frames = 1U;
        if (hop_frames == 0U) hop_frames = 1U;
        if (stem->format.frames < window_frames) continue;
        window_count = UINT64_C(1) +
            (stem->format.frames - window_frames) / hop_frames;
        if (hwa_run_u64_multiply(window_count, window_frames, &visits) != 0 ||
            hwa_run_add_u64(&total, visits) != 0) return -1;
        max_lag_hops = ((uint64_t)result->clock_rate_hz / UINT64_C(4)) /
                        hop_frames;
        if (max_lag_hops > (UINT64_MAX - UINT64_C(1)) / UINT64_C(2))
            return -1;
        lag_count = max_lag_hops * UINT64_C(2) + UINT64_C(1);
        if (hwa_run_u64_multiply(lag_count, window_count, &visits) != 0 ||
            hwa_run_add_u64(&total, visits) != 0) return -1;
        if (link->fit_valid &&
            hwa_run_add_u64(&total, window_count) != 0) return -1;
    }
    *expected = total;
    return 0;
}

static int hwa_run_valid_source(const HWARunSource *source,
                                uint64_t expected_id,
                                const HWARunResult *result)
{
    uint64_t expected_data_bytes;
    uint64_t bytes_per_sample;
    uint64_t expected_align;
    int64_t end_sample;
    int encoding_valid;
    double expected_duration;
    double duration_tolerance;
    const HWARunOptions *options = &result->options;
    if (source == NULL || source->id != expected_id ||
        !hwa_run_token_valid(source->binding_id, 127U, 0) ||
        source->path == NULL || source->path[0] == '\0' ||
        strcmp(source->path, "-") == 0 || strlen(source->path) > 32768U ||
        source->file_bytes == 0U || !hwa_run_lower_hash(source->sha256) ||
        source->side <= 0 || source->side >= HWA_RUN_SIDE_COUNT) return 0;
    if (source->kind == HWA_RUN_SOURCE_STEM) {
        bytes_per_sample = source->format.bits_per_sample / 8U;
        encoding_valid =
            (source->format.encoding == HWA_ENCODING_PCM &&
             (source->format.bits_per_sample == 8U ||
              source->format.bits_per_sample == 16U ||
              source->format.bits_per_sample == 24U ||
              source->format.bits_per_sample == 32U) &&
             source->format.valid_bits_per_sample != 0U &&
             source->format.valid_bits_per_sample <=
                 source->format.bits_per_sample) ||
            (source->format.encoding == HWA_ENCODING_IEEE_FLOAT &&
             (source->format.bits_per_sample == 32U ||
              source->format.bits_per_sample == 64U) &&
             source->format.valid_bits_per_sample ==
                 source->format.bits_per_sample);
        if (source->format.frames == 0U ||
            source->format.frames > options->max_input_frames ||
            source->file_bytes > options->max_input_bytes ||
            source->format.sample_rate_hz != result->clock_rate_hz ||
            !encoding_valid || bytes_per_sample == 0U ||
            hwa_run_u64_multiply(source->format.channels,
                                 bytes_per_sample, &expected_align) != 0 ||
            expected_align > UINT16_MAX ||
            hwa_run_u64_multiply(source->format.frames,
                                 expected_align, &expected_data_bytes) != 0 ||
            hwa_run_i64_end(source->start_sample, source->format.frames,
                            &end_sample) != 0) return 0;
        (void)end_sample;
        expected_duration = (double)source->format.frames /
                            (double)source->format.sample_rate_hz;
        duration_tolerance = 1e-12 * fmax(1.0, expected_duration);
        return source->role > 0 && source->role < HWA_RUN_STEM_ROLE_COUNT &&
               source->probe_format == 0 && source->probe_name == NULL &&
               source->unit == NULL &&
               (source->format.container == HWA_CONTAINER_RIFF ||
                source->format.container == HWA_CONTAINER_RF64) &&
               source->format.sample_rate_hz == result->clock_rate_hz &&
               source->format.channels != 0U &&
               source->format.channels <= HWA_MAX_CHANNELS &&
               source->format.block_align == (uint16_t)expected_align &&
               (source->format.channel_mask == 0U ||
                hwa_run_bit_count32(source->format.channel_mask) ==
                    source->format.channels) &&
               source->format.data_bytes == expected_data_bytes &&
               source->format.data_bytes <= source->file_bytes &&
               isfinite(source->format.duration_seconds) &&
               fabs(source->format.duration_seconds - expected_duration) <=
                   duration_tolerance &&
               source->rate_numerator == result->clock_rate_hz &&
               source->rate_denominator == 1U &&
               source->value_count == source->format.frames &&
               isfinite(source->gain_db) && source->gain_db >= -120.0 &&
               source->gain_db <= 120.0;
    }
    if (source->kind == HWA_RUN_SOURCE_PROBE) {
        uint64_t expected_binary_bytes = 0U;
        int binary_size_valid = 1;
        if (source->probe_format == HWA_RUN_PROBE_BINARY_F64LE) {
            binary_size_valid =
                source->value_count <= (UINT64_MAX - UINT64_C(16)) /
                                           UINT64_C(8);
            if (binary_size_valid)
                expected_binary_bytes = UINT64_C(16) +
                                        UINT64_C(8) * source->value_count;
        }
        return source->role == 0 &&
               source->probe_format > 0 &&
               source->probe_format < HWA_RUN_PROBE_FORMAT_COUNT &&
               hwa_run_token_valid(source->probe_name, 127U, 1) &&
               hwa_run_unit_valid(source->unit) &&
               source->rate_numerator != 0U &&
               source->rate_denominator != 0U &&
               hwa_run_gcd_u64(source->rate_numerator,
                               source->rate_denominator) == 1U &&
               source->value_count != 0U &&
               source->value_count <= options->max_probe_values &&
               source->file_bytes <= options->max_probe_bytes &&
               binary_size_valid &&
               (source->probe_format != HWA_RUN_PROBE_BINARY_F64LE ||
                source->file_bytes == expected_binary_bytes) &&
               source->format.container == 0 && source->format.encoding == 0 &&
               source->format.channels == 0U &&
               source->format.sample_rate_hz == 0U &&
               source->format.bits_per_sample == 0U &&
               source->format.valid_bits_per_sample == 0U &&
               source->format.block_align == 0U &&
               source->format.channel_mask == 0U &&
               source->format.frames == 0U &&
               source->format.data_bytes == 0U &&
               hwa_run_canonical_zero(source->format.duration_seconds) &&
               hwa_run_canonical_zero(source->gain_db);
    }
    return 0;
}

int hwa_run_result_validate(const HWARunResult *result,
                            char *error,
                            size_t error_size)
{
    uint64_t retained = 0U;
    uint64_t expected_evaluations = 0U;
    uint64_t reference_final_id = 0U;
    unsigned model_roles = 0U;
    size_t index;
    size_t row_count = 0U;
    size_t expected_feature_count;
    size_t stem_source_count = 0U;
    size_t probe_source_count = 0U;
    size_t model_stem_count = 0U;
    size_t probe_source_cursor = 0U;
    if (error != NULL && error_size > 0U) error[0] = '\0';
    if (result == NULL || !hwa_run_options_valid(&result->options) ||
        result->manifest_path == NULL || result->manifest_path[0] == '\0' ||
        strcmp(result->manifest_path, "-") == 0 ||
        strlen(result->manifest_path) > 32768U ||
        !hwa_run_lower_hash(result->manifest_sha256) ||
        result->clock_rate_hz < 8000U || result->clock_rate_hz > 768000U) {
        hwa_set_error(error, error_size, "invalid Stage 7 result metadata");
        return -1;
    }
    if ((result->source_count != 0U && result->sources == NULL) ||
        (result->clock_count != 0U && result->clocks == NULL) ||
        (result->feature_count != 0U && result->features == NULL) ||
        (result->stage_count != 0U && result->stages == NULL) ||
        (result->probe_count != 0U && result->probes == NULL) ||
        (result->link_count != 0U && result->links == NULL) ||
        (result->warning_count != 0U && result->warnings == NULL) ||
        result->stage_count != HWA_RUN_STAGE_COUNT ||
        result->clock_count > SIZE_MAX / HWA_RUN_FEATURES_PER_CLOCK) {
        hwa_set_error(error, error_size, "invalid Stage 7 result shape");
        return -1;
    }
    expected_feature_count =
        result->clock_count * HWA_RUN_FEATURES_PER_CLOCK;
    if (result->feature_count != expected_feature_count ||
        hwa_run_size_add(&row_count, result->source_count) != 0 ||
        hwa_run_size_add(&row_count, result->clock_count) != 0 ||
        hwa_run_size_add(&row_count, result->feature_count) != 0 ||
        hwa_run_size_add(&row_count, result->stage_count) != 0 ||
        hwa_run_size_add(&row_count, result->probe_count) != 0 ||
        hwa_run_size_add(&row_count, result->link_count) != 0 ||
        hwa_run_size_add(&row_count, result->warning_count) != 0 ||
        result->link_count > result->options.max_links ||
        result->warning_count > result->options.max_warnings ||
        row_count > result->options.max_result_rows) {
        hwa_set_error(error, error_size, "Stage 7 result exceeds saved caps");
        return -1;
    }
    for (index = 0U; index < result->source_count; ++index) {
        const HWARunSource *source = &result->sources[index];
        if (!hwa_run_valid_source(source,
                                  (uint64_t)index + UINT64_C(1), result) ||
            (index != 0U &&
             strcmp(result->sources[index - 1U].binding_id,
                    source->binding_id) >= 0)) {
            hwa_set_error(error, error_size, "invalid Stage 7 source row");
            return -1;
        }
        if (source->kind == HWA_RUN_SOURCE_STEM) {
            stem_source_count++;
            if (source->side == HWA_RUN_REFERENCE) {
                if (source->role != HWA_RUN_STEM_FINAL ||
                    reference_final_id != 0U) {
                    hwa_set_error(error, error_size,
                                  "invalid Stage 7 reference cohort");
                    return -1;
                }
                reference_final_id = source->id;
            } else {
                unsigned bit = 1U << (unsigned)source->role;
                if ((model_roles & bit) != 0U) {
                    hwa_set_error(error, error_size,
                                  "duplicate Stage 7 model role");
                    return -1;
                }
                model_roles |= bit;
                model_stem_count++;
            }
        } else {
            probe_source_count++;
        }
    }
    if (reference_final_id == 0U ||
        (model_roles & (1U << (unsigned)HWA_RUN_STEM_FINAL)) == 0U ||
        stem_source_count > result->options.max_stems ||
        probe_source_count > result->options.max_probes ||
        stem_source_count > SIZE_MAX - probe_source_count ||
        result->source_count != stem_source_count + probe_source_count ||
        result->clock_count != model_stem_count ||
        result->probe_count != probe_source_count) {
        hwa_set_error(error, error_size, "invalid Stage 7 source cohort");
        return -1;
    }
    for (index = 0U; index < result->clock_count; ++index) {
        const HWARunClock *clock = &result->clocks[index];
        const HWARunSource *model;
        int64_t expected_start;
        int64_t expected_end;
        int64_t expected_drift;
        uint64_t expected_overlap;
        double expected_ppm;
        uint32_t expected_flags;
        if (clock->id != (uint64_t)index + UINT64_C(1) ||
            clock->role < HWA_RUN_STEM_SOURCE ||
            clock->role >= HWA_RUN_STEM_ROLE_COUNT ||
            (index != 0U && result->clocks[index - 1U].role >= clock->role) ||
            clock->reference_source_id != reference_final_id ||
            clock->model_source_id == 0U ||
            clock->model_source_id > result->source_count ||
            clock->availability != HWA_RUN_AVAILABLE ||
            (model = &result->sources[clock->model_source_id - 1U])->kind !=
                HWA_RUN_SOURCE_STEM ||
            model->side != HWA_RUN_MODEL || model->role != clock->role ||
            hwa_run_clock_derived_expected(
                result, clock, &expected_start, &expected_end,
                &expected_drift, &expected_overlap, &expected_ppm,
                &expected_flags) != 0 ||
            clock->start_offset_samples != expected_start ||
            clock->end_offset_samples != expected_end ||
            clock->drift_samples != expected_drift ||
            clock->overlap_frames != expected_overlap ||
            clock->drift_ppm != expected_ppm ||
            clock->quality_flags != expected_flags) {
            hwa_set_error(error, error_size, "invalid Stage 7 clock row");
            return -1;
        }
    }
    for (index = 0U; index < result->feature_count; ++index) {
        const HWARunFeature *feature = &result->features[index];
        const HWARunClock *feature_clock;
        HWARunFeatureKind kind;
        HWARunUnit unit;
        uint32_t feature_index;
        double expected_delta = 0.0;
        double expected_gap = 0.0;
        size_t catalog_offset = index % HWA_RUN_FEATURES_PER_CLOCK;
        size_t clock_offset = index / HWA_RUN_FEATURES_PER_CLOCK;
        feature_clock = &result->clocks[clock_offset];
        if (hwa_run_feature_catalog_at(catalog_offset, &kind,
                                       &feature_index, &unit) != 0 ||
            feature->id != (uint64_t)index + UINT64_C(1) ||
            feature->clock_id != (uint64_t)clock_offset + UINT64_C(1) ||
            feature->role != result->clocks[clock_offset].role ||
            feature->kind != kind || feature->index != feature_index ||
            feature->unit != unit ||
            !hwa_run_boolean(feature->reference_valid) ||
            !hwa_run_boolean(feature->model_valid) ||
            !hwa_run_boolean(feature->delta_valid) ||
            !hwa_run_boolean(feature->gap_valid) ||
            (feature->availability != HWA_RUN_AVAILABLE &&
             feature->availability != HWA_RUN_INSUFFICIENT) ||
            feature->quality_flags != feature_clock->quality_flags) {
            hwa_set_error(error, error_size, "invalid Stage 7 feature row");
            return -1;
        }
        if (feature->availability == HWA_RUN_AVAILABLE) {
            if (feature->reference_valid != 1 ||
                feature->model_valid != 1 ||
                feature->delta_valid != 1 || feature->gap_valid != 1 ||
                !isfinite(feature->reference_value) ||
                !isfinite(feature->model_value) || !isfinite(feature->delta) ||
                !isfinite(feature->normalized_gap) ||
                hwa_run_feature_derived_expected(feature, &expected_delta,
                                                 &expected_gap) != 0 ||
                feature->delta != expected_delta ||
                feature->normalized_gap != expected_gap) {
                hwa_set_error(error, error_size,
                              "invalid available Stage 7 feature");
                return -1;
            }
        } else if ((feature->reference_valid == 1 &&
                    feature->model_valid == 1) ||
                   (feature->reference_valid &&
                    !isfinite(feature->reference_value)) ||
                   (!feature->reference_valid &&
                    !hwa_run_canonical_zero(feature->reference_value)) ||
                   (feature->model_valid &&
                    !isfinite(feature->model_value)) ||
                   (!feature->model_valid &&
                    !hwa_run_canonical_zero(feature->model_value)) ||
                   feature->delta_valid || feature->gap_valid ||
                   !hwa_run_canonical_zero(feature->delta) ||
                   !hwa_run_canonical_zero(feature->normalized_gap)) {
            hwa_set_error(error, error_size,
                          "invalid unavailable Stage 7 feature");
            return -1;
        }
    }
    {
        HWARunResult expected_result = *result;
        HWARunStage expected_stages[HWA_RUN_STAGE_COUNT];
        memset(expected_stages, 0, sizeof(expected_stages));
        expected_result.stages = expected_stages;
        if (hwa_run_stage_rows_rebuild(&expected_result, NULL, 0U) != 0) {
            hwa_set_error(error, error_size, "cannot derive Stage 7 stages");
            return -1;
        }
        for (index = 0U; index < result->stage_count; ++index) {
            const HWARunStage *stage = &result->stages[index];
            const HWARunStage *expected = &expected_stages[index];
            if (stage->id != expected->id ||
                stage->from_role != expected->from_role ||
                stage->to_role != expected->to_role ||
                stage->availability != expected->availability ||
                stage->prior_gap != expected->prior_gap ||
                stage->current_gap != expected->current_gap ||
                stage->added_gap != expected->added_gap ||
                stage->rank != expected->rank ||
                stage->quality_flags != expected->quality_flags ||
                stage->gap_valid != expected->gap_valid) {
                hwa_set_error(error, error_size, "invalid Stage 7 stage row");
                return -1;
            }
        }
    }
    for (index = 0U; index < result->probe_count; ++index) {
        const HWARunProbe *probe = &result->probes[index];
        const HWARunSource *source;
        while (probe_source_cursor < result->source_count &&
               result->sources[probe_source_cursor].kind !=
                   HWA_RUN_SOURCE_PROBE)
            probe_source_cursor++;
        source = probe_source_cursor < result->source_count
                     ? &result->sources[probe_source_cursor]
                     : NULL;
        if (probe->id != (uint64_t)index + UINT64_C(1) ||
            source == NULL || source->kind != HWA_RUN_SOURCE_PROBE ||
            probe->source_id != source->id ||
            probe->value_count != source->value_count ||
            probe->availability != HWA_RUN_AVAILABLE ||
            probe->statistics_valid != 1 || probe->value_count == 0U ||
            !isfinite(probe->minimum) || !isfinite(probe->maximum) ||
            !isfinite(probe->mean) || !isfinite(probe->population_sd) ||
            probe->minimum > probe->maximum ||
            probe->mean < probe->minimum || probe->mean > probe->maximum ||
            probe->population_sd < 0.0) {
            hwa_set_error(error, error_size, "invalid Stage 7 probe row");
            return -1;
        }
        probe_source_cursor++;
    }
    for (index = 0U; index < result->link_count; ++index) {
        const HWARunLink *link = &result->links[index];
        const HWARunSource *stem;
        const HWARunSource *probe_source;
        int64_t expected_lag_samples = 0;
        double expected_r_squared = 0.0;
        double expected_coverage = 0.0;
        uint32_t expected_flags = 0U;
        if (link->id != (uint64_t)index + UINT64_C(1) ||
            link->stem_source_id == 0U ||
            link->stem_source_id > result->source_count ||
            link->probe_source_id == 0U ||
            link->probe_source_id > result->source_count ||
            (index != 0U && hwa_run_link_canonical_compare(
                &result->links[index - 1U], link) >= 0) ||
            link->feature != HWA_RUN_FEATURE_RMS_DBFS ||
            link->feature_index != 0U ||
            !hwa_run_boolean(link->fit_valid) ||
            (link->availability != HWA_RUN_AVAILABLE &&
             link->availability != HWA_RUN_INSUFFICIENT) ||
            (stem = &result->sources[link->stem_source_id - 1U])->kind !=
                HWA_RUN_SOURCE_STEM ||
            (probe_source =
                 &result->sources[link->probe_source_id - 1U])->kind !=
                HWA_RUN_SOURCE_PROBE ||
            stem->side != probe_source->side ||
            (probe_source->side == HWA_RUN_REFERENCE &&
             stem->role != HWA_RUN_STEM_FINAL) ||
            hwa_run_link_derived_expected(result, link,
                                          &expected_lag_samples,
                                          &expected_r_squared,
                                          &expected_coverage,
                                          &expected_flags) != 0 ||
            link->quality_flags != expected_flags ||
            link->coverage != expected_coverage) {
            hwa_set_error(error, error_size, "invalid Stage 7 link row");
            return -1;
        }
        if (link->availability == HWA_RUN_AVAILABLE) {
            if (link->fit_valid != 1 ||
                link->point_count < HWA_RUN_MIN_LINK_PAIRS ||
                !isfinite(link->correlation) ||
                fabs(link->correlation) > 1.0 ||
                !isfinite(link->slope) || !isfinite(link->intercept) ||
                !isfinite(link->r_squared) ||
                link->lag_samples != expected_lag_samples ||
                link->r_squared != expected_r_squared ||
                !isfinite(link->coverage)) {
                hwa_set_error(error, error_size,
                              "invalid available Stage 7 link");
                return -1;
            }
        } else if (link->fit_valid != 0 || !isfinite(link->coverage) ||
                   link->lag_hops != 0 || link->lag_samples != 0 ||
                   !hwa_run_canonical_zero(link->correlation) ||
                   !hwa_run_canonical_zero(link->slope) ||
                   !hwa_run_canonical_zero(link->intercept) ||
                   !hwa_run_canonical_zero(link->r_squared)) {
            hwa_set_error(error, error_size,
                          "invalid unavailable Stage 7 link");
            return -1;
        }
    }
    if (hwa_run_evaluations_expected(result, &expected_evaluations) != 0 ||
        result->evaluation_count != expected_evaluations ||
        result->evaluation_count > result->options.max_evaluations) {
        hwa_set_error(error, error_size,
                      "invalid Stage 7 evaluation ledger");
        return -1;
    }
    if (result->warning_count != hwa_run_warning_spec_count(result)) {
        hwa_set_error(error, error_size, "invalid Stage 7 warning count");
        return -1;
    }
    for (index = 0U; index < result->warning_count; ++index) {
        const HWARunWarning *warning = &result->warnings[index];
        HWARunWarningSpec spec;
        if (hwa_run_warning_spec_at(result, index, &spec) != 0 ||
            warning->id != (uint64_t)index + UINT64_C(1) ||
            warning->code == NULL || strcmp(warning->code, spec.code) != 0 ||
            warning->message == NULL ||
            strcmp(warning->message, spec.message) != 0 ||
            warning->source_id != spec.source_id ||
            warning->clock_id != spec.clock_id ||
            warning->stage_id != spec.stage_id ||
            warning->link_id != spec.link_id ||
            !hwa_run_boolean(warning->source_id_valid) ||
            !hwa_run_boolean(warning->clock_id_valid) ||
            !hwa_run_boolean(warning->stage_id_valid) ||
            !hwa_run_boolean(warning->link_id_valid) ||
            warning->source_id_valid != spec.source_id_valid ||
            warning->clock_id_valid != spec.clock_id_valid ||
            warning->stage_id_valid != spec.stage_id_valid ||
            warning->link_id_valid != spec.link_id_valid) {
            hwa_set_error(error, error_size, "invalid Stage 7 warning row");
            return -1;
        }
    }
    if (hwa_run_result_retained_bytes(result, &retained) != 0 ||
        retained != result->retained_work_bytes ||
        retained > result->options.max_work_bytes) {
        hwa_set_error(error, error_size,
                      "invalid Stage 7 retained-work ledger");
        return -1;
    }
    return 0;
}

typedef struct HWARunManifestStem {
    char *id;
    char sha256[HWA_SHA256_HEX_SIZE];
    HWARunSide side;
    HWARunStemRole role;
    int64_t start_sample;
    double gain_db;
    uint32_t rate_hz;
    uint16_t channels;
} HWARunManifestStem;

typedef struct HWARunManifestProbe {
    char *id;
    char *name;
    char *unit;
    char sha256[HWA_SHA256_HEX_SIZE];
    HWARunSide side;
    HWARunProbeFormat format;
    int64_t start_sample;
    uint64_t rate_numerator;
    uint64_t rate_denominator;
    uint64_t value_count;
} HWARunManifestProbe;

typedef struct HWARunManifestLink {
    char *stem_id;
    char *probe_id;
    HWARunFeatureKind feature;
    uint32_t feature_index;
} HWARunManifestLink;

typedef struct HWARunManifest {
    uint32_t clock_rate_hz;
    HWARunManifestStem *stems;
    size_t stem_count;
    size_t stem_capacity;
    HWARunManifestProbe *probes;
    size_t probe_count;
    size_t probe_capacity;
    HWARunManifestLink *links;
    size_t link_count;
    size_t link_capacity;
} HWARunManifest;

typedef struct HWARunJson {
    const unsigned char *data;
    size_t size;
    size_t offset;
    size_t token_count;
    size_t depth;
    const HWARunOptions *options;
    const HWANumericLocale *locale;
    uint64_t *live_work;
    char *error;
    size_t error_size;
} HWARunJson;

static void hwa_run_manifest_free(HWARunManifest *manifest)
{
    size_t index;
    if (manifest == NULL) return;
    for (index = 0U; index < manifest->stem_count; ++index)
        free(manifest->stems[index].id);
    for (index = 0U; index < manifest->probe_count; ++index) {
        free(manifest->probes[index].id);
        free(manifest->probes[index].name);
        free(manifest->probes[index].unit);
    }
    for (index = 0U; index < manifest->link_count; ++index) {
        free(manifest->links[index].stem_id);
        free(manifest->links[index].probe_id);
    }
    free(manifest->stems);
    free(manifest->probes);
    free(manifest->links);
    memset(manifest, 0, sizeof(*manifest));
}

static int hwa_run_json_fail(HWARunJson *json, const char *message)
{
    hwa_set_error(json->error, json->error_size,
                  "invalid Stage 7 manifest at byte %llu: %s",
                  (unsigned long long)json->offset, message);
    return -1;
}

static void hwa_run_json_space(HWARunJson *json)
{
    while (json->offset < json->size) {
        unsigned char value = json->data[json->offset];
        if (value != ' ' && value != '\t' && value != '\r' && value != '\n')
            break;
        json->offset++;
    }
}

static int hwa_run_json_token(HWARunJson *json)
{
    if (json->token_count >= json->options->max_json_tokens)
        return hwa_run_json_fail(json, "token limit exceeded");
    json->token_count++;
    return 0;
}

static int hwa_run_json_take(HWARunJson *json, unsigned char wanted)
{
    hwa_run_json_space(json);
    if (json->offset >= json->size || json->data[json->offset] != wanted)
        return hwa_run_json_fail(json, "unexpected token");
    json->offset++;
    return 0;
}

static int hwa_run_json_depth_begin(HWARunJson *json)
{
    if (json->depth >= json->options->max_json_depth)
        return hwa_run_json_fail(json, "nesting limit exceeded");
    json->depth++;
    return 0;
}

static int hwa_run_json_hex(unsigned char value)
{
    if (value >= '0' && value <= '9') return (int)(value - '0');
    if (value >= 'a' && value <= 'f') return 10 + (int)(value - 'a');
    if (value >= 'A' && value <= 'F') return 10 + (int)(value - 'A');
    return -1;
}

static int hwa_run_json_string(HWARunJson *json, char **out)
{
    size_t scan;
    size_t decoded_size = 0U;
    char *decoded;
    size_t write = 0U;
    if (out == NULL) return hwa_run_json_fail(json, "missing string target");
    *out = NULL;
    hwa_run_json_space(json);
    if (hwa_run_json_token(json) != 0 || json->offset >= json->size ||
        json->data[json->offset] != '"')
        return hwa_run_json_fail(json, "string expected");
    scan = ++json->offset;
    while (scan < json->size && json->data[scan] != '"') {
        unsigned char value = json->data[scan++];
        if (value < 0x20U || value >= 0x80U)
            return hwa_run_json_fail(json, "manifest strings must be ASCII");
        if (value == '\\') {
            unsigned char escaped;
            if (scan >= json->size) return hwa_run_json_fail(json, "bad escape");
            escaped = json->data[scan++];
            if (escaped == 'u') {
                int a;
                int b;
                int c;
                int d;
                unsigned code;
                if (scan + 4U > json->size ||
                    (a = hwa_run_json_hex(json->data[scan])) < 0 ||
                    (b = hwa_run_json_hex(json->data[scan + 1U])) < 0 ||
                    (c = hwa_run_json_hex(json->data[scan + 2U])) < 0 ||
                    (d = hwa_run_json_hex(json->data[scan + 3U])) < 0)
                    return hwa_run_json_fail(json, "bad Unicode escape");
                code = ((unsigned)a << 12U) | ((unsigned)b << 8U) |
                       ((unsigned)c << 4U) | (unsigned)d;
                if (code == 0U || code >= 0x80U)
                    return hwa_run_json_fail(json,
                                             "non-ASCII Unicode escape");
                scan += 4U;
            } else if (strchr("\"\\/bfnrt", (int)escaped) == NULL) {
                return hwa_run_json_fail(json, "bad escape");
            }
        }
        if (decoded_size == SIZE_MAX) return hwa_run_json_fail(json, "string too long");
        decoded_size++;
    }
    if (scan >= json->size) return hwa_run_json_fail(json, "unterminated string");
    if (decoded_size == SIZE_MAX)
        return hwa_run_json_fail(json, "string is too long");
    if (hwa_run_work_reserve(json->live_work,
                             (uint64_t)decoded_size + UINT64_C(1),
                             json->options->max_work_bytes,
                             json->error, json->error_size) != 0)
        return hwa_run_json_fail(json, "string exceeds work cap");
    decoded = (char *)malloc(decoded_size + 1U);
    if (decoded == NULL) {
        *json->live_work -= (uint64_t)decoded_size + UINT64_C(1);
        return hwa_run_json_fail(json, "cannot allocate string");
    }
    while (json->offset < scan) {
        unsigned char value = json->data[json->offset++];
        if (value == '\\') {
            unsigned char escaped = json->data[json->offset++];
            if (escaped == 'u') {
                int a = hwa_run_json_hex(json->data[json->offset]);
                int b = hwa_run_json_hex(json->data[json->offset + 1U]);
                int c = hwa_run_json_hex(json->data[json->offset + 2U]);
                int d = hwa_run_json_hex(json->data[json->offset + 3U]);
                value = (unsigned char)(((unsigned)a << 12U) |
                                        ((unsigned)b << 8U) |
                                        ((unsigned)c << 4U) | (unsigned)d);
                json->offset += 4U;
            } else if (escaped == 'b') value = '\b';
            else if (escaped == 'f') value = '\f';
            else if (escaped == 'n') value = '\n';
            else if (escaped == 'r') value = '\r';
            else if (escaped == 't') value = '\t';
            else value = escaped;
        }
        decoded[write++] = (char)value;
    }
    json->offset++;
    decoded[write] = '\0';
    *out = decoded;
    return 0;
}

static void hwa_run_json_string_free(HWARunJson *json, char **text)
{
    size_t length;
    if (json == NULL || text == NULL || *text == NULL) return;
    length = strlen(*text);
    free(*text);
    *text = NULL;
    if (json->live_work != NULL &&
        (uint64_t)length + UINT64_C(1) <= *json->live_work)
        *json->live_work -= (uint64_t)length + UINT64_C(1);
}

static int hwa_run_ascii_digit(unsigned char value)
{
    return value >= (unsigned char)'0' && value <= (unsigned char)'9';
}

static int hwa_run_ascii_alnum(unsigned char value)
{
    return hwa_run_ascii_digit(value) ||
           (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
           (value >= (unsigned char)'a' && value <= (unsigned char)'z');
}

static int hwa_run_json_number_text(HWARunJson *json,
                                    char text[128],
                                    int integer_only)
{
    size_t start;
    size_t end;
    size_t length;
    hwa_run_json_space(json);
    if (hwa_run_json_token(json) != 0) return -1;
    start = json->offset;
    if (json->offset < json->size && json->data[json->offset] == '-')
        json->offset++;
    if (json->offset >= json->size) return hwa_run_json_fail(json, "number expected");
    if (json->data[json->offset] == '0') {
        json->offset++;
        if (json->offset < json->size &&
            hwa_run_ascii_digit(json->data[json->offset]))
            return hwa_run_json_fail(json, "leading zero in number");
    } else {
        if (!hwa_run_ascii_digit(json->data[json->offset]))
            return hwa_run_json_fail(json, "number expected");
        while (json->offset < json->size &&
               hwa_run_ascii_digit(json->data[json->offset])) json->offset++;
    }
    if (!integer_only && json->offset < json->size &&
        json->data[json->offset] == '.') {
        json->offset++;
        if (json->offset >= json->size ||
            !hwa_run_ascii_digit(json->data[json->offset]))
            return hwa_run_json_fail(json, "fraction digit expected");
        while (json->offset < json->size &&
               hwa_run_ascii_digit(json->data[json->offset])) json->offset++;
    }
    if (!integer_only && json->offset < json->size &&
        (json->data[json->offset] == 'e' ||
         json->data[json->offset] == 'E')) {
        json->offset++;
        if (json->offset < json->size &&
            (json->data[json->offset] == '+' ||
             json->data[json->offset] == '-')) json->offset++;
        if (json->offset >= json->size ||
            !hwa_run_ascii_digit(json->data[json->offset]))
            return hwa_run_json_fail(json, "exponent digit expected");
        while (json->offset < json->size &&
               hwa_run_ascii_digit(json->data[json->offset])) json->offset++;
    }
    end = json->offset;
    length = end - start;
    if (length == 0U || length >= 128U)
        return hwa_run_json_fail(json, "number is too long");
    memcpy(text, json->data + start, length);
    text[length] = '\0';
    return 0;
}

static int hwa_run_json_u64(HWARunJson *json, uint64_t *value)
{
    char text[128];
    size_t index;
    uint64_t parsed = 0U;
    if (value == NULL || hwa_run_json_number_text(json, text, 1) != 0)
        return -1;
    if (text[0] == '-') return hwa_run_json_fail(json, "unsigned integer expected");
    for (index = 0U; text[index] != '\0'; ++index) {
        unsigned digit = (unsigned)(text[index] - '0');
        if (parsed > (UINT64_MAX - (uint64_t)digit) / UINT64_C(10))
            return hwa_run_json_fail(json, "integer overflow");
        parsed = parsed * UINT64_C(10) + (uint64_t)digit;
    }
    *value = parsed;
    return 0;
}

static int hwa_run_json_i64(HWARunJson *json, int64_t *value)
{
    char text[128];
    size_t index = 0U;
    uint64_t parsed = 0U;
    int negative = 0;
    uint64_t limit;
    if (value == NULL || hwa_run_json_number_text(json, text, 1) != 0)
        return -1;
    if (text[index] == '-') {
        negative = 1;
        index++;
    }
    limit = negative ? (uint64_t)INT64_MAX + UINT64_C(1) :
                       (uint64_t)INT64_MAX;
    for (; text[index] != '\0'; ++index) {
        unsigned digit = (unsigned)(text[index] - '0');
        if (parsed > (limit - (uint64_t)digit) / UINT64_C(10))
            return hwa_run_json_fail(json, "integer overflow");
        parsed = parsed * UINT64_C(10) + (uint64_t)digit;
    }
    if (negative) {
        *value = parsed == (uint64_t)INT64_MAX + UINT64_C(1)
                     ? INT64_MIN
                     : -(int64_t)parsed;
    } else {
        *value = (int64_t)parsed;
    }
    return 0;
}

static int hwa_run_json_double(HWARunJson *json, double *value)
{
    char text[128];
    if (value == NULL || hwa_run_json_number_text(json, text, 0) != 0)
        return -1;
    if (hwa_c_locale_parse_double(json->locale, text, value) != 0)
        return hwa_run_json_fail(json, "finite number expected");
    return 0;
}

static int hwa_run_token_valid(const char *text,
                               size_t maximum,
                               int require_namespace)
{
    size_t index;
    int namespaced = 0;
    if (text == NULL || text[0] == '\0' || strlen(text) > maximum ||
        !hwa_run_ascii_alnum((unsigned char)text[0])) return 0;
    for (index = 0U; text[index] != '\0'; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (!(hwa_run_ascii_alnum(value) || value == '.' || value == '_' ||
              value == ':' || value == '/' || value == '-')) return 0;
        if (value == '.' || value == ':' || value == '/') namespaced = 1;
    }
    return !require_namespace || namespaced;
}

static int hwa_run_unit_valid(const char *text)
{
    size_t index;
    size_t length;
    if (text == NULL) return 0;
    length = strlen(text);
    if (length == 0U || length > 31U) return 0;
    for (index = 0U; index < length; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (value < 0x21U || value > 0x7eU || value == ',' || value == '"')
            return 0;
    }
    return 1;
}

static int hwa_run_hash_text_valid(const char *text)
{
    return text != NULL && strlen(text) == HWA_SHA256_HEX_SIZE - 1U &&
           hwa_run_lower_hash(text);
}

static int hwa_run_side_parse(const char *text, HWARunSide *side)
{
    if (strcmp(text, "reference") == 0) *side = HWA_RUN_REFERENCE;
    else if (strcmp(text, "model") == 0) *side = HWA_RUN_MODEL;
    else return -1;
    return 0;
}

static int hwa_run_role_parse(const char *text, HWARunStemRole *role)
{
    size_t index;
    for (index = 0U; index < sizeof(hwa_run_role_names) /
                                     sizeof(hwa_run_role_names[0]); ++index) {
        if (strcmp(text, hwa_run_role_names[index].name) == 0) {
            *role = (HWARunStemRole)hwa_run_role_names[index].value;
            return 0;
        }
    }
    return -1;
}

static int hwa_run_format_parse(const char *text, HWARunProbeFormat *format)
{
    if (strcmp(text, "csv-f64") == 0) *format = HWA_RUN_PROBE_CSV_F64;
    else if (strcmp(text, "binary-f64le") == 0)
        *format = HWA_RUN_PROBE_BINARY_F64LE;
    else return -1;
    return 0;
}

static int hwa_run_grow(HWARunJson *json,
                        void **array,
                        size_t *capacity,
                        size_t count,
                        size_t maximum,
                        size_t element_size)
{
    size_t next;
    uint64_t old_bytes;
    uint64_t new_bytes;
    void *grown;
    if (count < *capacity) return 0;
    if (count >= maximum) return -1;
    next = *capacity == 0U ? 4U : *capacity * 2U;
    if (next < *capacity || next > maximum) next = maximum;
    if (element_size != 0U && next > SIZE_MAX / element_size) return -1;
    old_bytes = (uint64_t)(*capacity * element_size);
    new_bytes = (uint64_t)(next * element_size);
    if (hwa_run_work_reserve(json->live_work, new_bytes,
                             json->options->max_work_bytes,
                             json->error, json->error_size) != 0)
        return -1;
    grown = malloc(next * element_size);
    if (grown == NULL) {
        *json->live_work -= new_bytes;
        return -1;
    }
    if (old_bytes != 0U) memcpy(grown, *array, (size_t)old_bytes);
    free(*array);
    if (old_bytes <= *json->live_work) *json->live_work -= old_bytes;
    memset((unsigned char *)grown + *capacity * element_size, 0,
           (next - *capacity) * element_size);
    *array = grown;
    *capacity = next;
    return 0;
}

static int hwa_run_json_key(HWARunJson *json,
                            char **key,
                            unsigned long long *seen,
                            unsigned bit)
{
    unsigned long long mask;
    if (bit >= sizeof(*seen) * CHAR_BIT) return -1;
    mask = 1ULL << bit;
    if ((*seen & mask) != 0ULL) {
        hwa_run_json_string_free(json, key);
        return hwa_run_json_fail(json, "duplicate object key");
    }
    *seen |= mask;
    if (hwa_run_json_take(json, ':') != 0) {
        hwa_run_json_string_free(json, key);
        return -1;
    }
    return 0;
}

static uint64_t hwa_run_gcd_u64(uint64_t a, uint64_t b)
{
    while (b != 0U) {
        uint64_t next = a % b;
        a = b;
        b = next;
    }
    return a;
}

static int hwa_run_parse_stem(HWARunJson *json, HWARunManifestStem *stem)
{
    unsigned long long seen = 0ULL;
    char *key = NULL;
    int first = 1;
    memset(stem, 0, sizeof(*stem));
    if (hwa_run_json_take(json, '{') != 0 ||
        hwa_run_json_depth_begin(json) != 0) return -1;
    for (;;) {
        key = NULL;
        hwa_run_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == '}') {
            json->offset++;
            break;
        }
        if (!first && hwa_run_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_run_json_string(json, &key) != 0) goto failed;
        if (strcmp(key, "id") == 0) {
            if (hwa_run_json_key(json, &key, &seen, 0U) != 0 ||
                hwa_run_json_string(json, &stem->id) != 0) goto failed;
        } else if (strcmp(key, "side") == 0) {
            char *value = NULL;
            if (hwa_run_json_key(json, &key, &seen, 1U) != 0 ||
                hwa_run_json_string(json, &value) != 0) {
                hwa_run_json_string_free(json, &value);
                goto failed;
            }
            if (hwa_run_side_parse(value, &stem->side) != 0) {
                hwa_run_json_string_free(json, &value);
                hwa_run_json_fail(json, "bad stem side");
                goto failed;
            }
            hwa_run_json_string_free(json, &value);
        } else if (strcmp(key, "role") == 0) {
            char *value = NULL;
            if (hwa_run_json_key(json, &key, &seen, 2U) != 0 ||
                hwa_run_json_string(json, &value) != 0) {
                hwa_run_json_string_free(json, &value);
                goto failed;
            }
            if (hwa_run_role_parse(value, &stem->role) != 0) {
                hwa_run_json_string_free(json, &value);
                hwa_run_json_fail(json, "bad stem role");
                goto failed;
            }
            hwa_run_json_string_free(json, &value);
        } else if (strcmp(key, "sha256") == 0) {
            char *value = NULL;
            if (hwa_run_json_key(json, &key, &seen, 3U) != 0 ||
                hwa_run_json_string(json, &value) != 0) {
                hwa_run_json_string_free(json, &value);
                goto failed;
            }
            if (!hwa_run_hash_text_valid(value)) {
                hwa_run_json_string_free(json, &value);
                hwa_run_json_fail(json, "bad stem SHA-256");
                goto failed;
            }
            memcpy(stem->sha256, value, HWA_SHA256_HEX_SIZE);
            hwa_run_json_string_free(json, &value);
        } else if (strcmp(key, "start_sample") == 0) {
            if (hwa_run_json_key(json, &key, &seen, 4U) != 0 ||
                hwa_run_json_i64(json, &stem->start_sample) != 0) goto failed;
        } else if (strcmp(key, "gain_db") == 0) {
            if (hwa_run_json_key(json, &key, &seen, 5U) != 0 ||
                hwa_run_json_double(json, &stem->gain_db) != 0) goto failed;
        } else if (strcmp(key, "rate_hz") == 0) {
            uint64_t value = 0U;
            if (hwa_run_json_key(json, &key, &seen, 6U) != 0 ||
                hwa_run_json_u64(json, &value) != 0) goto failed;
            if (value < 8000U || value > 768000U) {
                hwa_run_json_fail(json, "bad stem rate");
                goto failed;
            }
            stem->rate_hz = (uint32_t)value;
        } else if (strcmp(key, "channels") == 0) {
            uint64_t value = 0U;
            if (hwa_run_json_key(json, &key, &seen, 7U) != 0 ||
                hwa_run_json_u64(json, &value) != 0) goto failed;
            if (value == 0U || value > HWA_MAX_CHANNELS) {
                hwa_run_json_fail(json, "bad stem channel count");
                goto failed;
            }
            stem->channels = (uint16_t)value;
        } else {
            hwa_run_json_fail(json, "unknown stem key");
            goto failed;
        }
        hwa_run_json_string_free(json, &key);
    }
    json->depth--;
    if (seen != 0xffULL || !hwa_run_token_valid(stem->id, 127U, 0) ||
        stem->gain_db < -120.0 || stem->gain_db > 120.0) {
        hwa_run_json_fail(json, "incomplete or invalid stem");
        goto failed_no_depth;
    }
    return 0;
failed:
    hwa_run_json_string_free(json, &key);
    json->depth--;
failed_no_depth:
    hwa_run_json_string_free(json, &stem->id);
    return -1;
}

static int hwa_run_parse_probe(HWARunJson *json, HWARunManifestProbe *probe)
{
    unsigned long long seen = 0ULL;
    char *key = NULL;
    int first = 1;
    memset(probe, 0, sizeof(*probe));
    if (hwa_run_json_take(json, '{') != 0 ||
        hwa_run_json_depth_begin(json) != 0) return -1;
    for (;;) {
        key = NULL;
        hwa_run_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == '}') {
            json->offset++;
            break;
        }
        if (!first && hwa_run_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_run_json_string(json, &key) != 0) goto failed;
        if (strcmp(key, "id") == 0) {
            if (hwa_run_json_key(json, &key, &seen, 0U) != 0 ||
                hwa_run_json_string(json, &probe->id) != 0) goto failed;
        } else if (strcmp(key, "side") == 0) {
            char *value = NULL;
            if (hwa_run_json_key(json, &key, &seen, 1U) != 0 ||
                hwa_run_json_string(json, &value) != 0) {
                hwa_run_json_string_free(json, &value);
                goto failed;
            }
            if (hwa_run_side_parse(value, &probe->side) != 0) {
                hwa_run_json_string_free(json, &value);
                hwa_run_json_fail(json, "bad probe side");
                goto failed;
            }
            hwa_run_json_string_free(json, &value);
        } else if (strcmp(key, "name") == 0) {
            if (hwa_run_json_key(json, &key, &seen, 2U) != 0 ||
                hwa_run_json_string(json, &probe->name) != 0) goto failed;
        } else if (strcmp(key, "unit") == 0) {
            if (hwa_run_json_key(json, &key, &seen, 3U) != 0 ||
                hwa_run_json_string(json, &probe->unit) != 0) goto failed;
        } else if (strcmp(key, "format") == 0) {
            char *value = NULL;
            if (hwa_run_json_key(json, &key, &seen, 4U) != 0 ||
                hwa_run_json_string(json, &value) != 0) {
                hwa_run_json_string_free(json, &value);
                goto failed;
            }
            if (hwa_run_format_parse(value, &probe->format) != 0) {
                hwa_run_json_string_free(json, &value);
                hwa_run_json_fail(json, "bad probe format");
                goto failed;
            }
            hwa_run_json_string_free(json, &value);
        } else if (strcmp(key, "sha256") == 0) {
            char *value = NULL;
            if (hwa_run_json_key(json, &key, &seen, 5U) != 0 ||
                hwa_run_json_string(json, &value) != 0) {
                hwa_run_json_string_free(json, &value);
                goto failed;
            }
            if (!hwa_run_hash_text_valid(value)) {
                hwa_run_json_string_free(json, &value);
                hwa_run_json_fail(json, "bad probe SHA-256");
                goto failed;
            }
            memcpy(probe->sha256, value, HWA_SHA256_HEX_SIZE);
            hwa_run_json_string_free(json, &value);
        } else if (strcmp(key, "start_sample") == 0) {
            if (hwa_run_json_key(json, &key, &seen, 6U) != 0 ||
                hwa_run_json_i64(json, &probe->start_sample) != 0) goto failed;
        } else if (strcmp(key, "rate_numerator") == 0) {
            if (hwa_run_json_key(json, &key, &seen, 7U) != 0 ||
                hwa_run_json_u64(json, &probe->rate_numerator) != 0) goto failed;
        } else if (strcmp(key, "rate_denominator") == 0) {
            if (hwa_run_json_key(json, &key, &seen, 8U) != 0 ||
                hwa_run_json_u64(json, &probe->rate_denominator) != 0) goto failed;
        } else if (strcmp(key, "value_count") == 0) {
            if (hwa_run_json_key(json, &key, &seen, 9U) != 0 ||
                hwa_run_json_u64(json, &probe->value_count) != 0) goto failed;
        } else {
            hwa_run_json_fail(json, "unknown probe key");
            goto failed;
        }
        hwa_run_json_string_free(json, &key);
    }
    json->depth--;
    if (seen != 0x3ffULL ||
        !hwa_run_token_valid(probe->id, 127U, 0) ||
        !hwa_run_token_valid(probe->name, 127U, 1) ||
        !hwa_run_unit_valid(probe->unit) || probe->value_count == 0U ||
        probe->rate_numerator == 0U || probe->rate_denominator == 0U ||
        hwa_run_gcd_u64(probe->rate_numerator,
                        probe->rate_denominator) != 1U) {
        hwa_run_json_fail(json, "incomplete or invalid probe");
        goto failed_no_depth;
    }
    return 0;
failed:
    hwa_run_json_string_free(json, &key);
    json->depth--;
failed_no_depth:
    hwa_run_json_string_free(json, &probe->id);
    hwa_run_json_string_free(json, &probe->name);
    hwa_run_json_string_free(json, &probe->unit);
    memset(probe, 0, sizeof(*probe));
    return -1;
}

static int hwa_run_parse_link(HWARunJson *json, HWARunManifestLink *link)
{
    unsigned long long seen = 0ULL;
    char *key = NULL;
    int first = 1;
    memset(link, 0, sizeof(*link));
    if (hwa_run_json_take(json, '{') != 0 ||
        hwa_run_json_depth_begin(json) != 0) return -1;
    for (;;) {
        key = NULL;
        hwa_run_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == '}') {
            json->offset++;
            break;
        }
        if (!first && hwa_run_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_run_json_string(json, &key) != 0) goto failed;
        if (strcmp(key, "stem") == 0) {
            if (hwa_run_json_key(json, &key, &seen, 0U) != 0 ||
                hwa_run_json_string(json, &link->stem_id) != 0) goto failed;
        } else if (strcmp(key, "probe") == 0) {
            if (hwa_run_json_key(json, &key, &seen, 1U) != 0 ||
                hwa_run_json_string(json, &link->probe_id) != 0) goto failed;
        } else if (strcmp(key, "feature") == 0) {
            char *value = NULL;
            if (hwa_run_json_key(json, &key, &seen, 2U) != 0 ||
                hwa_run_json_string(json, &value) != 0) {
                hwa_run_json_string_free(json, &value);
                goto failed;
            }
            if (strcmp(value, "rms_dbfs") != 0) {
                hwa_run_json_string_free(json, &value);
                hwa_run_json_fail(json, "Stage 7 supports only rms_dbfs links");
                goto failed;
            }
            hwa_run_json_string_free(json, &value);
            link->feature = HWA_RUN_FEATURE_RMS_DBFS;
            link->feature_index = 0U;
        } else {
            hwa_run_json_fail(json, "unknown link key");
            goto failed;
        }
        hwa_run_json_string_free(json, &key);
    }
    json->depth--;
    if (seen != 0x7ULL ||
        !hwa_run_token_valid(link->stem_id, 127U, 0) ||
        !hwa_run_token_valid(link->probe_id, 127U, 0)) {
        hwa_run_json_fail(json, "incomplete or invalid link");
        goto failed_no_depth;
    }
    return 0;
failed:
    hwa_run_json_string_free(json, &key);
    json->depth--;
failed_no_depth:
    hwa_run_json_string_free(json, &link->stem_id);
    hwa_run_json_string_free(json, &link->probe_id);
    memset(link, 0, sizeof(*link));
    return -1;
}

static int hwa_run_parse_stem_array(HWARunJson *json,
                                    HWARunManifest *manifest)
{
    int first = 1;
    if (hwa_run_json_take(json, '[') != 0 ||
        hwa_run_json_depth_begin(json) != 0) return -1;
    for (;;) {
        hwa_run_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == ']') {
            json->offset++;
            break;
        }
        if (!first && hwa_run_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_run_grow(json, (void **)&manifest->stems,
                         &manifest->stem_capacity, manifest->stem_count,
                         json->options->max_stems,
                         sizeof(*manifest->stems)) != 0) {
            if (json->error == NULL || json->error_size == 0U ||
                json->error[0] == '\0')
                hwa_run_json_fail(json, "stem limit or allocation failed");
            goto failed;
        }
        if (hwa_run_parse_stem(json,
                &manifest->stems[manifest->stem_count]) != 0) goto failed;
        manifest->stem_count++;
    }
    json->depth--;
    return 0;
failed:
    json->depth--;
    return -1;
}

static int hwa_run_parse_probe_array(HWARunJson *json,
                                     HWARunManifest *manifest)
{
    int first = 1;
    if (hwa_run_json_take(json, '[') != 0 ||
        hwa_run_json_depth_begin(json) != 0) return -1;
    for (;;) {
        hwa_run_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == ']') {
            json->offset++;
            break;
        }
        if (!first && hwa_run_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_run_grow(json, (void **)&manifest->probes,
                         &manifest->probe_capacity, manifest->probe_count,
                         json->options->max_probes,
                         sizeof(*manifest->probes)) != 0) {
            if (json->error == NULL || json->error_size == 0U ||
                json->error[0] == '\0')
                hwa_run_json_fail(json, "probe limit or allocation failed");
            goto failed;
        }
        if (hwa_run_parse_probe(json,
                &manifest->probes[manifest->probe_count]) != 0) goto failed;
        manifest->probe_count++;
    }
    json->depth--;
    return 0;
failed:
    json->depth--;
    return -1;
}

static int hwa_run_parse_link_array(HWARunJson *json,
                                    HWARunManifest *manifest)
{
    int first = 1;
    if (hwa_run_json_take(json, '[') != 0 ||
        hwa_run_json_depth_begin(json) != 0) return -1;
    for (;;) {
        hwa_run_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == ']') {
            json->offset++;
            break;
        }
        if (!first && hwa_run_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_run_grow(json, (void **)&manifest->links,
                         &manifest->link_capacity, manifest->link_count,
                         json->options->max_links,
                         sizeof(*manifest->links)) != 0) {
            if (json->error == NULL || json->error_size == 0U ||
                json->error[0] == '\0')
                hwa_run_json_fail(json, "link limit or allocation failed");
            goto failed;
        }
        if (hwa_run_parse_link(json,
                &manifest->links[manifest->link_count]) != 0) goto failed;
        manifest->link_count++;
    }
    json->depth--;
    return 0;
failed:
    json->depth--;
    return -1;
}

static const HWARunManifestStem *hwa_run_manifest_stem(
    const HWARunManifest *manifest,
    const char *id)
{
    size_t index;
    for (index = 0U; index < manifest->stem_count; ++index)
        if (strcmp(manifest->stems[index].id, id) == 0)
            return &manifest->stems[index];
    return NULL;
}

static const HWARunManifestProbe *hwa_run_manifest_probe(
    const HWARunManifest *manifest,
    const char *id)
{
    size_t index;
    for (index = 0U; index < manifest->probe_count; ++index)
        if (strcmp(manifest->probes[index].id, id) == 0)
            return &manifest->probes[index];
    return NULL;
}

static int hwa_run_manifest_semantics(HWARunJson *json,
                                      const HWARunManifest *manifest)
{
    size_t index;
    size_t other;
    size_t reference_final = 0U;
    size_t model_final = 0U;
    unsigned model_roles = 0U;
    for (index = 0U; index < manifest->stem_count; ++index) {
        const HWARunManifestStem *stem = &manifest->stems[index];
        if (stem->rate_hz != manifest->clock_rate_hz)
            return hwa_run_json_fail(json,
                                     "stem rate differs from common clock");
        if (stem->side == HWA_RUN_REFERENCE) {
            if (stem->role != HWA_RUN_STEM_FINAL)
                return hwa_run_json_fail(json,
                                         "only reference.final is accepted");
            reference_final++;
        } else {
            unsigned bit = 1U << (unsigned)stem->role;
            if ((model_roles & bit) != 0U)
                return hwa_run_json_fail(json, "duplicate model stem role");
            model_roles |= bit;
            if (stem->role == HWA_RUN_STEM_FINAL) model_final++;
        }
        for (other = index + 1U; other < manifest->stem_count; ++other)
            if (strcmp(stem->id, manifest->stems[other].id) == 0)
                return hwa_run_json_fail(json, "duplicate stream ID");
        for (other = 0U; other < manifest->probe_count; ++other)
            if (strcmp(stem->id, manifest->probes[other].id) == 0)
                return hwa_run_json_fail(json, "duplicate stream ID");
    }
    for (index = 0U; index < manifest->probe_count; ++index) {
        for (other = index + 1U; other < manifest->probe_count; ++other)
            if (strcmp(manifest->probes[index].id,
                       manifest->probes[other].id) == 0)
                return hwa_run_json_fail(json, "duplicate stream ID");
    }
    if (reference_final != 1U || model_final != 1U)
        return hwa_run_json_fail(
            json, "exactly reference.final and model.final are required");
    for (index = 0U; index < manifest->link_count; ++index) {
        const HWARunManifestLink *link = &manifest->links[index];
        const HWARunManifestStem *stem =
            hwa_run_manifest_stem(manifest, link->stem_id);
        const HWARunManifestProbe *probe =
            hwa_run_manifest_probe(manifest, link->probe_id);
        if (stem == NULL || probe == NULL || stem->side != probe->side ||
            (probe->side == HWA_RUN_REFERENCE &&
             stem->role != HWA_RUN_STEM_FINAL))
            return hwa_run_json_fail(json, "invalid same-side link");
        for (other = index + 1U; other < manifest->link_count; ++other) {
            const HWARunManifestLink *candidate = &manifest->links[other];
            if (strcmp(link->stem_id, candidate->stem_id) == 0 &&
                strcmp(link->probe_id, candidate->probe_id) == 0 &&
                link->feature == candidate->feature &&
                link->feature_index == candidate->feature_index)
                return hwa_run_json_fail(json, "duplicate link");
        }
    }
    return 0;
}

static int hwa_run_manifest_parse(const unsigned char *data,
                                  size_t size,
                                  const HWARunOptions *options,
                                  const HWANumericLocale *locale,
                                  uint64_t *live_work,
                                  HWARunManifest *manifest,
                                  char *error,
                                  size_t error_size)
{
    HWARunJson json;
    unsigned long long seen = 0ULL;
    int first = 1;
    int status = -1;
    memset(manifest, 0, sizeof(*manifest));
    memset(&json, 0, sizeof(json));
    json.data = data;
    json.size = size;
    json.options = options;
    json.locale = locale;
    json.live_work = live_work;
    json.error = error;
    json.error_size = error_size;
    if (hwa_run_json_take(&json, '{') != 0 ||
        hwa_run_json_depth_begin(&json) != 0) goto cleanup;
    for (;;) {
        char *key = NULL;
        hwa_run_json_space(&json);
        if (json.offset < json.size && json.data[json.offset] == '}') {
            json.offset++;
            break;
        }
        if (!first && hwa_run_json_take(&json, ',') != 0) goto cleanup_depth;
        first = 0;
        if (hwa_run_json_string(&json, &key) != 0) goto cleanup_depth;
        if (strcmp(key, "schema") == 0) {
            char *value = NULL;
            if (hwa_run_json_key(&json, &key, &seen, 0U) != 0 ||
                hwa_run_json_string(&json, &value) != 0) {
                hwa_run_json_string_free(&json, &value);
                goto cleanup_depth;
            }
            if (strcmp(value, "hwa-run") != 0) {
                hwa_run_json_string_free(&json, &value);
                hwa_run_json_fail(&json, "bad schema name");
                goto cleanup_depth;
            }
            hwa_run_json_string_free(&json, &value);
        } else if (strcmp(key, "schema_version") == 0) {
            uint64_t value = 0U;
            if (hwa_run_json_key(&json, &key, &seen, 1U) != 0 ||
                hwa_run_json_u64(&json, &value) != 0) goto cleanup_depth;
            if (value != 1U) {
                hwa_run_json_fail(&json, "unsupported schema version");
                goto cleanup_depth;
            }
        } else if (strcmp(key, "method_version") == 0) {
            char *value = NULL;
            if (hwa_run_json_key(&json, &key, &seen, 2U) != 0 ||
                hwa_run_json_string(&json, &value) != 0) {
                hwa_run_json_string_free(&json, &value);
                goto cleanup_depth;
            }
            if (strcmp(value, HWA_RUN_METHOD_VERSION) != 0) {
                hwa_run_json_string_free(&json, &value);
                hwa_run_json_fail(&json, "unsupported method version");
                goto cleanup_depth;
            }
            hwa_run_json_string_free(&json, &value);
        } else if (strcmp(key, "clock_rate_hz") == 0) {
            uint64_t value = 0U;
            if (hwa_run_json_key(&json, &key, &seen, 3U) != 0 ||
                hwa_run_json_u64(&json, &value) != 0) goto cleanup_depth;
            if (value < 8000U || value > 768000U) {
                hwa_run_json_fail(&json, "bad common clock rate");
                goto cleanup_depth;
            }
            manifest->clock_rate_hz = (uint32_t)value;
        } else if (strcmp(key, "stems") == 0) {
            if (hwa_run_json_key(&json, &key, &seen, 4U) != 0 ||
                hwa_run_parse_stem_array(&json, manifest) != 0)
                goto cleanup_depth;
        } else if (strcmp(key, "probes") == 0) {
            if (hwa_run_json_key(&json, &key, &seen, 5U) != 0 ||
                hwa_run_parse_probe_array(&json, manifest) != 0)
                goto cleanup_depth;
        } else if (strcmp(key, "links") == 0) {
            if (hwa_run_json_key(&json, &key, &seen, 6U) != 0 ||
                hwa_run_parse_link_array(&json, manifest) != 0)
                goto cleanup_depth;
        } else {
            hwa_run_json_fail(&json, "unknown top-level key");
            goto cleanup_depth;
        }
        hwa_run_json_string_free(&json, &key);
    }
    json.depth--;
    hwa_run_json_space(&json);
    if (seen != 0x7fULL || json.offset != json.size ||
        hwa_run_manifest_semantics(&json, manifest) != 0) {
        if (error != NULL && error_size > 0U && error[0] == '\0')
            hwa_run_json_fail(&json, "incomplete manifest or trailing data");
        goto cleanup;
    }
    status = 0;
    goto cleanup;
cleanup_depth:
    json.depth--;
cleanup:
    if (status != 0) hwa_run_manifest_free(manifest);
    return status;
}

typedef struct HWARunFileIdentity {
    uint64_t device;
    uint64_t inode;
    uint64_t size;
    int valid;
} HWARunFileIdentity;

typedef struct HWARunBlob {
    unsigned char *data;
    size_t size;
    uint64_t work_bytes;
    HWARunFileIdentity identity;
    char sha256[HWA_SHA256_HEX_SIZE];
} HWARunBlob;

typedef struct HWARunStemData {
    uint64_t source_id;
    double *samples;
    uint64_t frame_count;
    uint64_t work_bytes;
} HWARunStemData;

typedef struct HWARunProbeData {
    uint64_t source_id;
    double *values;
    uint64_t value_count;
    uint64_t work_bytes;
} HWARunProbeData;

static int hwa_run_path_identity(const char *path,
                                 HWARunFileIdentity *identity,
                                 char *error,
                                 size_t error_size)
{
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION information;
    HANDLE handle = INVALID_HANDLE_VALUE;
    if (path == NULL || path[0] == '\0' || strcmp(path, "-") == 0 ||
        (handle = CreateFileA(
             path, 0U, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
             NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL)) ==
            INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(handle, &information)) {
        if (handle != INVALID_HANDLE_VALUE) (void)CloseHandle(handle);
        hwa_set_error(error, error_size, "cannot inspect Stage 7 input '%s'",
                      path == NULL ? "" : path);
        return -1;
    }
    (void)CloseHandle(handle);
    if ((information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        hwa_set_error(error, error_size,
                      "Stage 7 input is not a named regular file");
        return -1;
    }
    identity->device = (uint64_t)information.dwVolumeSerialNumber;
    identity->inode = ((uint64_t)information.nFileIndexHigh << 32U) |
                      (uint64_t)information.nFileIndexLow;
    identity->size = ((uint64_t)information.nFileSizeHigh << 32U) |
                     (uint64_t)information.nFileSizeLow;
#else
    struct stat status;
    if (path == NULL || path[0] == '\0' || strcmp(path, "-") == 0 ||
        lstat(path, &status) != 0) {
        hwa_set_error(error, error_size, "cannot inspect Stage 7 input '%s'",
                      path == NULL ? "" : path);
        return -1;
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0) {
        hwa_set_error(error, error_size,
                      "Stage 7 input is not a named regular file");
        return -1;
    }
    identity->device = (uint64_t)status.st_dev;
    identity->inode = (uint64_t)status.st_ino;
    identity->size = (uint64_t)status.st_size;
#endif
    identity->valid = 1;
    return 0;
}

static int hwa_run_identity_equal(const HWARunFileIdentity *left,
                                  const HWARunFileIdentity *right)
{
    return left != NULL && right != NULL && left->valid && right->valid &&
           left->device == right->device && left->inode == right->inode &&
           left->size == right->size;
}

static int hwa_run_stream_identity(FILE *stream,
                                   HWARunFileIdentity *identity)
{
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION information;
    int descriptor = stream == NULL ? -1 : _fileno(stream);
    intptr_t raw = descriptor < 0
                       ? (intptr_t)-1
                       : _get_osfhandle(descriptor);
    if (raw == (intptr_t)-1 ||
        !GetFileInformationByHandle((HANDLE)raw, &information) ||
        (information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U)
        return -1;
    identity->device = (uint64_t)information.dwVolumeSerialNumber;
    identity->inode = ((uint64_t)information.nFileIndexHigh << 32U) |
                      (uint64_t)information.nFileIndexLow;
    identity->size = ((uint64_t)information.nFileSizeHigh << 32U) |
                     (uint64_t)information.nFileSizeLow;
#else
    struct stat status;
    if (stream == NULL || fstat(fileno(stream), &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_size < 0) return -1;
    identity->device = (uint64_t)status.st_dev;
    identity->inode = (uint64_t)status.st_ino;
    identity->size = (uint64_t)status.st_size;
#endif
    identity->valid = 1;
    return 0;
}

static int hwa_run_stream_seek(FILE *stream, uint64_t offset)
{
    if (stream == NULL || offset > (uint64_t)INT64_MAX) return -1;
    clearerr(stream);
#if defined(_WIN32)
    return _fseeki64(stream, (__int64)offset, SEEK_SET) == 0 ? 0 : -1;
#else
    return fseeko(stream, (off_t)offset, SEEK_SET) == 0 ? 0 : -1;
#endif
}

static int hwa_run_stream_sha256(
    FILE *stream,
    const HWARunFileIdentity *expected_identity,
    uint64_t maximum,
    char hex[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    unsigned char buffer[8192];
    unsigned char digest[32];
    HWASha256 context;
    HWARunFileIdentity before;
    HWARunFileIdentity after;
    uint64_t remaining;
    if (stream == NULL || expected_identity == NULL || hex == NULL ||
        !expected_identity->valid || expected_identity->size > maximum ||
        hwa_run_stream_identity(stream, &before) != 0 ||
        !hwa_run_identity_equal(expected_identity, &before) ||
        hwa_run_stream_seek(stream, 0U) != 0) {
        hwa_set_error(error, error_size,
                      "cannot bind the Stage 7 input stream");
        return -1;
    }
    remaining = expected_identity->size;
    hwa_sha256_init(&context);
    while (remaining != 0U) {
        size_t count = remaining < (uint64_t)sizeof(buffer)
                           ? (size_t)remaining
                           : sizeof(buffer);
        if (fread(buffer, 1U, count, stream) != count) {
            hwa_set_error(error, error_size,
                          "cannot hash the Stage 7 input stream");
            return -1;
        }
        hwa_sha256_update(&context, buffer, count);
        remaining -= (uint64_t)count;
    }
    if (fgetc(stream) != EOF || ferror(stream) ||
        hwa_run_stream_identity(stream, &after) != 0 ||
        !hwa_run_identity_equal(expected_identity, &after)) {
        hwa_set_error(error, error_size,
                      "Stage 7 input changed while it was hashed");
        return -1;
    }
    hwa_sha256_final(&context, digest);
    hwa_sha256_hex(digest, hex);
    return 0;
}

static int hwa_run_path_sha256_verify(
    const char *path,
    const HWARunFileIdentity *expected_identity,
    uint64_t maximum,
    const char expected_hash[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    HWARunFileIdentity before;
    HWARunFileIdentity opened;
    HWARunFileIdentity after;
    char actual[HWA_SHA256_HEX_SIZE];
    FILE *stream = NULL;
    int status = -1;
    if (path == NULL || expected_identity == NULL || expected_hash == NULL ||
        hwa_run_path_identity(path, &before, error, error_size) != 0 ||
        !hwa_run_identity_equal(expected_identity, &before))
        goto cleanup;
    stream = fopen(path, "rb");
    if (stream == NULL || hwa_run_stream_identity(stream, &opened) != 0 ||
        !hwa_run_identity_equal(expected_identity, &opened) ||
        hwa_run_stream_sha256(stream, expected_identity, maximum, actual,
                              error, error_size) != 0 ||
        strcmp(actual, expected_hash) != 0)
        goto cleanup;
    if (fclose(stream) != 0) {
        stream = NULL;
        goto cleanup;
    }
    stream = NULL;
    if (hwa_run_path_identity(path, &after, error, error_size) != 0 ||
        !hwa_run_identity_equal(expected_identity, &after))
        goto cleanup;
    status = 0;
cleanup:
    if (stream != NULL) (void)fclose(stream);
    if (status != 0 && error != NULL && error_size > 0U && error[0] == '\0')
        hwa_set_error(error, error_size,
                      "Stage 7 input changed during analysis");
    return status;
}

static void hwa_run_bytes_sha256(const unsigned char *data,
                                 size_t size,
                                 char hex[HWA_SHA256_HEX_SIZE])
{
    HWASha256 context;
    unsigned char digest[32];
    hwa_sha256_init(&context);
    hwa_sha256_update(&context, data, size);
    hwa_sha256_final(&context, digest);
    hwa_sha256_hex(digest, hex);
}

static int hwa_run_blob_read(const char *path,
                             uint64_t max_bytes,
                             uint64_t *live_work,
                             uint64_t max_work_bytes,
                             HWARunBlob *blob,
                             char *error,
                             size_t error_size)
{
    HWARunFileIdentity after;
    FILE *stream = NULL;
    size_t size;
    int status = -1;
    memset(blob, 0, sizeof(*blob));
    if (hwa_run_path_identity(path, &blob->identity, error, error_size) != 0)
        return -1;
    if (blob->identity.size > max_bytes ||
        blob->identity.size > (uint64_t)(SIZE_MAX - 1U)) {
        hwa_set_error(error, error_size, "Stage 7 input exceeds its byte cap");
        return -1;
    }
    size = (size_t)blob->identity.size;
    if (live_work == NULL || *live_work > max_work_bytes ||
        (uint64_t)size + UINT64_C(1) > max_work_bytes - *live_work) {
        hwa_set_error(error, error_size, "Stage 7 input exceeds its work cap");
        return -1;
    }
    *live_work += (uint64_t)size + UINT64_C(1);
    blob->work_bytes = (uint64_t)size + UINT64_C(1);
    blob->data = (unsigned char *)malloc(size + 1U);
    if (blob->data == NULL) {
        hwa_set_error(error, error_size, "cannot allocate Stage 7 input");
        return -1;
    }
    stream = fopen(path, "rb");
    if (stream == NULL ||
        hwa_run_stream_identity(stream, &after) != 0 ||
        !hwa_run_identity_equal(&blob->identity, &after) ||
        (size != 0U &&
        fread(blob->data, 1U, size, stream) != size) || fgetc(stream) != EOF ||
        ferror(stream)) {
        hwa_set_error(error, error_size, "cannot read Stage 7 input '%s'", path);
        goto cleanup;
    }
    if (fclose(stream) != 0) {
        stream = NULL;
        hwa_set_error(error, error_size, "cannot close Stage 7 input '%s'", path);
        goto cleanup;
    }
    stream = NULL;
    if (hwa_run_path_identity(path, &after, error, error_size) != 0 ||
        !hwa_run_identity_equal(&blob->identity, &after)) {
        hwa_set_error(error, error_size,
                      "Stage 7 input changed while it was read");
        goto cleanup;
    }
    blob->data[size] = '\0';
    blob->size = size;
    hwa_run_bytes_sha256(blob->data, blob->size, blob->sha256);
    status = 0;
cleanup:
    if (stream != NULL) (void)fclose(stream);
    if (status != 0) {
        free(blob->data);
        if (blob->work_bytes <= *live_work) *live_work -= blob->work_bytes;
        memset(blob, 0, sizeof(*blob));
    }
    return status;
}

static int hwa_run_blob_hash_matches(const HWARunBlob *blob,
                                     const char expected[HWA_SHA256_HEX_SIZE])
{
    char actual[HWA_SHA256_HEX_SIZE];
    if (blob == NULL || expected == NULL || blob->data == NULL) return 0;
    hwa_run_bytes_sha256(blob->data, blob->size, actual);
    return strcmp(blob->sha256, expected) == 0 &&
           strcmp(actual, expected) == 0;
}

static void hwa_run_blob_free(HWARunBlob *blob, uint64_t *live_work)
{
    if (blob == NULL) return;
    free(blob->data);
    if (live_work != NULL && blob->work_bytes <= *live_work)
        *live_work -= blob->work_bytes;
    memset(blob, 0, sizeof(*blob));
}

static const HWARunBinding *hwa_run_binding_find(
    const HWARunBinding *bindings,
    size_t binding_count,
    const char *id)
{
    size_t index;
    for (index = 0U; index < binding_count; ++index)
        if (bindings[index].id != NULL &&
            strcmp(bindings[index].id, id) == 0) return &bindings[index];
    return NULL;
}

static const HWARunSource *hwa_run_source_find(const HWARunResult *result,
                                               const char *binding_id)
{
    size_t index;
    for (index = 0U; index < result->source_count; ++index)
        if (strcmp(result->sources[index].binding_id, binding_id) == 0)
            return &result->sources[index];
    return NULL;
}

static int hwa_run_bindings_validate(const HWARunManifest *manifest,
                                     const HWARunBinding *bindings,
                                     size_t binding_count,
                                     char *error,
                                     size_t error_size)
{
    size_t expected;
    size_t index;
    size_t other;
    if (manifest->stem_count > SIZE_MAX - manifest->probe_count) return -1;
    expected = manifest->stem_count + manifest->probe_count;
    if (binding_count != expected || (binding_count != 0U && bindings == NULL)) {
        hwa_set_error(error, error_size,
                      "Stage 7 needs exactly one binding per stream");
        return -1;
    }
    for (index = 0U; index < binding_count; ++index) {
        const HWARunBinding *binding = &bindings[index];
        if (!hwa_run_token_valid(binding->id, 127U, 0) ||
            binding->path == NULL ||
            binding->path[0] == '\0' || strcmp(binding->path, "-") == 0 ||
            (hwa_run_manifest_stem(manifest, binding->id) == NULL &&
             hwa_run_manifest_probe(manifest, binding->id) == NULL)) {
            hwa_set_error(error, error_size, "invalid or extra Stage 7 binding");
            return -1;
        }
        for (other = index + 1U; other < binding_count; ++other) {
            if (bindings[other].id != NULL &&
                strcmp(binding->id, bindings[other].id) == 0) {
                hwa_set_error(error, error_size, "duplicate Stage 7 binding");
                return -1;
            }
        }
    }
    for (index = 0U; index < manifest->stem_count; ++index)
        if (hwa_run_binding_find(bindings, binding_count,
                                 manifest->stems[index].id) == NULL) {
            hwa_set_error(error, error_size, "missing Stage 7 stem binding");
            return -1;
        }
    for (index = 0U; index < manifest->probe_count; ++index)
        if (hwa_run_binding_find(bindings, binding_count,
                                 manifest->probes[index].id) == NULL) {
            hwa_set_error(error, error_size, "missing Stage 7 probe binding");
            return -1;
        }
    return 0;
}

static int hwa_run_sources_build(const HWARunManifest *manifest,
                                 const HWARunBinding *bindings,
                                 size_t binding_count,
                                 const HWARunOptions *options,
                                 HWARunResult *result,
                                 HWARunFileIdentity **out_identities,
                                 char *error,
                                 size_t error_size)
{
    size_t source_count = manifest->stem_count + manifest->probe_count;
    HWARunFileIdentity *identities = NULL;
    size_t index;
    size_t other;
    if (source_count > options->max_result_rows) {
        hwa_set_error(error, error_size, "Stage 7 source rows exceed their cap");
        return -1;
    }
    result->sources = (HWARunSource *)calloc(source_count,
                                              sizeof(*result->sources));
    identities = (HWARunFileIdentity *)calloc(source_count,
                                               sizeof(*identities));
    if ((source_count != 0U && result->sources == NULL) ||
        (source_count != 0U && identities == NULL)) {
        hwa_set_error(error, error_size, "cannot allocate Stage 7 sources");
        free(identities);
        return -1;
    }
    result->source_count = source_count;
    for (index = 0U; index < manifest->stem_count; ++index) {
        const HWARunManifestStem *stem = &manifest->stems[index];
        const HWARunBinding *binding = hwa_run_binding_find(
            bindings, binding_count, stem->id);
        HWARunSource *source = &result->sources[index];
        source->binding_id = hwa_run_copy_string(stem->id);
        source->path = hwa_run_copy_string(binding->path);
        memcpy(source->sha256, stem->sha256, HWA_SHA256_HEX_SIZE);
        source->kind = HWA_RUN_SOURCE_STEM;
        source->side = stem->side;
        source->role = stem->role;
        source->start_sample = stem->start_sample;
        source->gain_db = stem->gain_db;
        source->rate_numerator = stem->rate_hz;
        source->rate_denominator = 1U;
        if (source->binding_id == NULL || source->path == NULL) goto allocation;
    }
    for (index = 0U; index < manifest->probe_count; ++index) {
        const HWARunManifestProbe *probe = &manifest->probes[index];
        const HWARunBinding *binding = hwa_run_binding_find(
            bindings, binding_count, probe->id);
        HWARunSource *source =
            &result->sources[manifest->stem_count + index];
        source->binding_id = hwa_run_copy_string(probe->id);
        source->path = hwa_run_copy_string(binding->path);
        source->probe_name = hwa_run_copy_string(probe->name);
        source->unit = hwa_run_copy_string(probe->unit);
        memcpy(source->sha256, probe->sha256, HWA_SHA256_HEX_SIZE);
        source->kind = HWA_RUN_SOURCE_PROBE;
        source->side = probe->side;
        source->probe_format = probe->format;
        source->start_sample = probe->start_sample;
        source->rate_numerator = probe->rate_numerator;
        source->rate_denominator = probe->rate_denominator;
        source->value_count = probe->value_count;
        if (source->binding_id == NULL || source->path == NULL ||
            source->probe_name == NULL || source->unit == NULL) goto allocation;
    }
    qsort(result->sources, result->source_count, sizeof(*result->sources),
          hwa_run_source_canonical_compare);
    for (index = 0U; index < result->source_count; ++index) {
        result->sources[index].id = (uint64_t)index + UINT64_C(1);
        if (index != 0U &&
            strcmp(result->sources[index - 1U].binding_id,
                   result->sources[index].binding_id) >= 0) {
            hwa_set_error(error, error_size,
                          "non-unique Stage 7 source order");
            goto failed;
        }
        if (hwa_run_path_identity(result->sources[index].path,
                                  &identities[index], error, error_size) != 0) {
            if (error != NULL && error_size > 0U && error[0] == '\0')
                hwa_set_error(error, error_size, "invalid Stage 7 input");
            goto failed;
        }
        result->sources[index].file_bytes = identities[index].size;
        for (other = 0U; other < index; ++other) {
            if (hwa_run_identity_equal(&identities[other], &identities[index])) {
                hwa_set_error(error, error_size,
                              "Stage 7 bindings name the same file identity");
                goto failed;
            }
        }
    }
    *out_identities = identities;
    return 0;
allocation:
    hwa_set_error(error, error_size, "cannot copy Stage 7 source metadata");
failed:
    free(identities);
    return -1;
}

static int hwa_run_sources_verify(const HWARunResult *result,
                                  const HWARunFileIdentity *identities,
                                  char *error,
                                  size_t error_size)
{
    size_t index;
    for (index = 0U; index < result->source_count; ++index) {
        const HWARunSource *source = &result->sources[index];
        uint64_t maximum = source->kind == HWA_RUN_SOURCE_STEM
                               ? result->options.max_input_bytes
                               : result->options.max_probe_bytes;
        if (hwa_run_path_sha256_verify(
                source->path, &identities[index], maximum, source->sha256,
                error, error_size) != 0) {
            if (error != NULL && error_size > 0U && error[0] == '\0')
                hwa_set_error(error, error_size,
                              "Stage 7 input changed during analysis");
            return -1;
        }
    }
    return 0;
}

static int hwa_run_work_reserve(uint64_t *live,
                                uint64_t amount,
                                uint64_t maximum,
                                char *error,
                                size_t error_size)
{
    if (live == NULL || *live > maximum || amount > maximum - *live) {
        hwa_set_error(error, error_size, "Stage 7 work exceeds its byte cap");
        return -1;
    }
    *live += amount;
    return 0;
}

static int hwa_run_evaluations_add(HWARunResult *result,
                                   uint64_t amount,
                                   char *error,
                                   size_t error_size)
{
    if (result->evaluation_count > result->options.max_evaluations ||
        amount > result->options.max_evaluations - result->evaluation_count) {
        hwa_set_error(error, error_size,
                      "Stage 7 work exceeds its evaluation cap");
        return -1;
    }
    result->evaluation_count += amount;
    return 0;
}

static int hwa_run_u64_multiply(uint64_t left,
                                uint64_t right,
                                uint64_t *product)
{
    if (product == NULL || (left != 0U && right > UINT64_MAX / left))
        return -1;
    *product = left * right;
    return 0;
}

typedef struct HWARunU128 {
    uint64_t high;
    uint64_t low;
} HWARunU128;

static HWARunU128 hwa_run_u128_multiply(uint64_t left, uint64_t right)
{
    const uint64_t mask = UINT64_C(0xffffffff);
    uint64_t left_low = left & mask;
    uint64_t left_high = left >> 32U;
    uint64_t right_low = right & mask;
    uint64_t right_high = right >> 32U;
    uint64_t low_product = left_low * right_low;
    uint64_t middle = left_high * right_low + (low_product >> 32U);
    uint64_t middle_low = middle & mask;
    uint64_t middle_high = middle >> 32U;
    HWARunU128 product;
    middle_low += left_low * right_high;
    product.high = left_high * right_high + middle_high +
                   (middle_low >> 32U);
    product.low = (middle_low << 32U) | (low_product & mask);
    return product;
}

static int hwa_run_u128_compare(HWARunU128 left, HWARunU128 right)
{
    if (left.high != right.high) return left.high < right.high ? -1 : 1;
    return left.low < right.low ? -1 : left.low > right.low ? 1 : 0;
}

static HWARunU128 hwa_run_u128_subtract(HWARunU128 left, HWARunU128 right)
{
    HWARunU128 difference;
    difference.low = left.low - right.low;
    difference.high = left.high - right.high -
                      (left.low < right.low ? UINT64_C(1) : UINT64_C(0));
    return difference;
}

static unsigned hwa_run_u64_bit_length(uint64_t value)
{
    unsigned bits = 0U;
    while (value != 0U) {
        bits++;
        value >>= 1U;
    }
    return bits;
}

static unsigned hwa_run_u128_bit_length(HWARunU128 value)
{
    return value.high != 0U
               ? 64U + hwa_run_u64_bit_length(value.high)
               : hwa_run_u64_bit_length(value.low);
}

static HWARunU128 hwa_run_u128_shift_left(HWARunU128 value,
                                           unsigned amount)
{
    HWARunU128 shifted = {0U, 0U};
    if (amount == 0U) return value;
    if (amount < 64U) {
        shifted.high = (value.high << amount) |
                       (value.low >> (64U - amount));
        shifted.low = value.low << amount;
    } else if (amount < 128U) {
        shifted.high = value.low << (amount - 64U);
    }
    return shifted;
}

static int hwa_run_u128_divide_u64(HWARunU128 numerator,
                                    HWARunU128 denominator,
                                    uint64_t *quotient)
{
    unsigned numerator_bits;
    unsigned denominator_bits;
    unsigned shift;
    uint64_t result = 0U;
    if (quotient == NULL ||
        (denominator.high == 0U && denominator.low == 0U)) return -1;
    if (hwa_run_u128_compare(numerator, denominator) < 0) {
        *quotient = 0U;
        return 0;
    }
    numerator_bits = hwa_run_u128_bit_length(numerator);
    denominator_bits = hwa_run_u128_bit_length(denominator);
    shift = numerator_bits - denominator_bits;
    if (shift >= 64U) return -1;
    for (;;) {
        HWARunU128 candidate = hwa_run_u128_shift_left(denominator, shift);
        if (hwa_run_u128_compare(numerator, candidate) >= 0) {
            numerator = hwa_run_u128_subtract(numerator, candidate);
            result |= UINT64_C(1) << shift;
        }
        if (shift == 0U) break;
        shift--;
    }
    *quotient = result;
    return 0;
}

static void hwa_run_stem_data_free(HWARunStemData *data, size_t count)
{
    size_t index;
    if (data == NULL) return;
    for (index = 0U; index < count; ++index) free(data[index].samples);
    free(data);
}

static void hwa_run_probe_data_free(HWARunProbeData *data, size_t count)
{
    size_t index;
    if (data == NULL) return;
    for (index = 0U; index < count; ++index) free(data[index].values);
    free(data);
}

static int hwa_run_stems_load(const HWARunManifest *manifest,
                              HWARunResult *result,
                              const HWARunFileIdentity *identities,
                              uint64_t *live_work,
                              HWARunStemData **out_data,
                              char *error,
                              size_t error_size)
{
    HWARunStemData *data;
    uint64_t state_bytes;
    size_t data_index = 0U;
    size_t source_index;
    if (hwa_run_u64_multiply((uint64_t)manifest->stem_count,
                             sizeof(*data), &state_bytes) != 0 ||
        hwa_run_work_reserve(live_work, state_bytes,
                             result->options.max_work_bytes,
                             error, error_size) != 0) return -1;
    data = (HWARunStemData *)calloc(manifest->stem_count, sizeof(*data));
    if (data == NULL) {
        hwa_set_error(error, error_size, "cannot allocate Stage 7 stem state");
        return -1;
    }
    for (source_index = 0U; source_index < result->source_count; ++source_index) {
        HWARunSource *source = &result->sources[source_index];
        const HWARunManifestStem *declared;
        HWAWavReader reader;
        HWARunFileIdentity opened_identity;
        char before_hash[HWA_SHA256_HEX_SIZE];
        char after_hash[HWA_SHA256_HEX_SIZE];
        unsigned char *buffer = NULL;
        uint64_t sample_bytes = 0U;
        uint64_t buffer_bytes = 0U;
        uint64_t written = 0U;
        double gain;
        int status = -1;
        if (source->kind != HWA_RUN_SOURCE_STEM) continue;
        declared = hwa_run_manifest_stem(manifest, source->binding_id);
        memset(&reader, 0, sizeof(reader));
        if (declared == NULL || hwa_wav_reader_open(
                &reader, source->path, result->options.max_input_bytes,
                error, error_size) != 0) goto one_cleanup;
        if (hwa_run_stream_identity(reader.file, &opened_identity) != 0 ||
            !hwa_run_identity_equal(&opened_identity,
                                    &identities[source_index])) {
            hwa_set_error(error, error_size,
                          "Stage 7 WAVE identity changed before decode");
            goto one_cleanup;
        }
        if (hwa_run_stream_sha256(
                reader.file, &identities[source_index],
                result->options.max_input_bytes, before_hash,
                error, error_size) != 0 ||
            strcmp(before_hash, source->sha256) != 0 ||
            hwa_run_stream_seek(reader.file, reader.data_offset) != 0) {
            if (error != NULL && error_size > 0U && error[0] == '\0')
                hwa_set_error(error, error_size,
                              "Stage 7 WAVE SHA-256 does not match manifest");
            goto one_cleanup;
        }
        reader.bytes_remaining = reader.format.data_bytes;
        if (reader.format.sample_rate_hz != declared->rate_hz ||
            reader.format.channels != declared->channels ||
            reader.format.frames == 0U ||
            reader.format.frames > result->options.max_input_frames) {
            hwa_set_error(error, error_size,
                          "Stage 7 stem header does not match its manifest");
            goto one_cleanup;
        }
        if (hwa_run_u64_multiply(reader.format.frames, sizeof(double),
                                 &sample_bytes) != 0 ||
            hwa_run_u64_multiply((uint64_t)result->options.decode_block_frames,
                                 reader.format.block_align,
                                 &buffer_bytes) != 0 ||
            sample_bytes > (uint64_t)SIZE_MAX ||
            buffer_bytes > (uint64_t)SIZE_MAX ||
            hwa_run_work_reserve(live_work, sample_bytes,
                                 result->options.max_work_bytes,
                                 error, error_size) != 0 ||
            hwa_run_work_reserve(live_work, buffer_bytes,
                                 result->options.max_work_bytes,
                                 error, error_size) != 0) goto one_cleanup;
        data[data_index].samples = (double *)malloc((size_t)sample_bytes);
        buffer = (unsigned char *)malloc((size_t)buffer_bytes);
        if (data[data_index].samples == NULL || buffer == NULL) {
            hwa_set_error(error, error_size,
                          "cannot allocate Stage 7 stem samples");
            goto one_cleanup;
        }
        gain = pow(10.0, declared->gain_db / 20.0);
        if (!isfinite(gain)) {
            hwa_set_error(error, error_size, "invalid Stage 7 stem gain");
            goto one_cleanup;
        }
        {
            uint64_t decode_evaluations;
            if (hwa_run_u64_multiply(reader.format.frames,
                                     reader.format.channels,
                                     &decode_evaluations) != 0 ||
                hwa_run_evaluations_add(result, decode_evaluations,
                                        error, error_size) != 0)
                goto one_cleanup;
        }
        for (;;) {
            size_t frames_read = 0U;
            size_t frame;
            if (hwa_wav_reader_read_frames(
                    &reader, buffer, result->options.decode_block_frames,
                    &frames_read, error, error_size) != 0) goto one_cleanup;
            if (frames_read == 0U) break;
            for (frame = 0U; frame < frames_read; ++frame) {
                size_t channel;
                long double sum = 0.0L;
                const unsigned char *frame_data = buffer +
                    frame * (size_t)reader.format.block_align;
                for (channel = 0U; channel < reader.format.channels; ++channel) {
                    const unsigned char *sample = frame_data +
                        channel * reader.bytes_per_sample;
                    int clipped = 0;
                    double decoded = hwa_wav_decode_sample(
                        &reader, sample, &clipped);
                    (void)clipped;
                    if (!isfinite(decoded)) {
                        hwa_set_error(error, error_size,
                                      "non-finite Stage 7 WAVE sample");
                        goto one_cleanup;
                    }
                    sum += (long double)decoded;
                }
                {
                    double mixed =
                        (double)(sum / (long double)reader.format.channels) *
                        gain;
                    if (!isfinite(mixed)) {
                        hwa_set_error(error, error_size,
                                      "non-finite gained Stage 7 stem sample");
                        goto one_cleanup;
                    }
                    data[data_index].samples[written + (uint64_t)frame] = mixed;
                }
            }
            written += (uint64_t)frames_read;
        }
        if (written != reader.format.frames) {
            hwa_set_error(error, error_size, "short Stage 7 stem read");
            goto one_cleanup;
        }
        if (hwa_run_stream_sha256(
                reader.file, &identities[source_index],
                result->options.max_input_bytes, after_hash,
                error, error_size) != 0 ||
            strcmp(before_hash, after_hash) != 0 ||
            strcmp(after_hash, source->sha256) != 0) {
            if (error != NULL && error_size > 0U && error[0] == '\0')
                hwa_set_error(error, error_size,
                              "Stage 7 WAVE changed during decode");
            goto one_cleanup;
        }
        source->format = reader.format;
        source->value_count = reader.format.frames;
        data[data_index].source_id = source->id;
        data[data_index].frame_count = reader.format.frames;
        data[data_index].work_bytes = sample_bytes;
        status = 0;
one_cleanup:
        hwa_wav_reader_close(&reader);
        free(buffer);
        if (buffer_bytes <= *live_work) *live_work -= buffer_bytes;
        if (status != 0) {
            hwa_run_stem_data_free(data, manifest->stem_count);
            return -1;
        }
        data_index++;
    }
    *out_data = data;
    return 0;
}

static uint64_t hwa_run_le64(const unsigned char *bytes)
{
    return (uint64_t)bytes[0] | ((uint64_t)bytes[1] << 8U) |
           ((uint64_t)bytes[2] << 16U) | ((uint64_t)bytes[3] << 24U) |
           ((uint64_t)bytes[4] << 32U) | ((uint64_t)bytes[5] << 40U) |
           ((uint64_t)bytes[6] << 48U) | ((uint64_t)bytes[7] << 56U);
}

static int hwa_run_csv_u64(const unsigned char *text,
                           size_t size,
                           uint64_t *value)
{
    size_t index;
    uint64_t parsed = 0U;
    if (size == 0U || (size > 1U && text[0] == '0')) return -1;
    for (index = 0U; index < size; ++index) {
        unsigned digit;
        if (text[index] < '0' || text[index] > '9') return -1;
        digit = (unsigned)(text[index] - '0');
        if (parsed > (UINT64_MAX - (uint64_t)digit) / UINT64_C(10)) return -1;
        parsed = parsed * UINT64_C(10) + (uint64_t)digit;
    }
    *value = parsed;
    return 0;
}

static int hwa_run_probe_csv(const HWARunBlob *blob,
                             uint64_t expected_count,
                             const HWANumericLocale *locale,
                             double *values,
                             char *error,
                             size_t error_size)
{
    size_t offset = 0U;
    uint64_t row = 0U;
    int crlf = 0;
    static const unsigned char header[] = "index,value";
    if (blob->size < sizeof(header) ||
        memcmp(blob->data, header, sizeof(header) - 1U) != 0) goto malformed;
    offset = sizeof(header) - 1U;
    if (offset < blob->size && blob->data[offset] == '\r') {
        crlf = 1;
        offset++;
    }
    if (offset >= blob->size || blob->data[offset++] != '\n') goto malformed;
    while (offset < blob->size) {
        size_t line_start = offset;
        size_t comma = SIZE_MAX;
        size_t newline;
        size_t line_end;
        char number[128];
        char canonical[128];
        size_t number_size;
        uint64_t index_value;
        while (offset < blob->size && blob->data[offset] != '\n') {
            if (blob->data[offset] == '\r' &&
                (offset + 1U >= blob->size ||
                 blob->data[offset + 1U] != '\n')) goto malformed;
            if (blob->data[offset] == ',') {
                if (comma != SIZE_MAX) goto malformed;
                comma = offset;
            }
            offset++;
        }
        newline = offset;
        if (newline >= blob->size) goto malformed;
        if (crlf) {
            if (newline == line_start || blob->data[newline - 1U] != '\r')
                goto malformed;
            line_end = newline - 1U;
        } else {
            if (newline != line_start && blob->data[newline - 1U] == '\r')
                goto malformed;
            line_end = newline;
        }
        offset = newline + 1U;
        if (
            comma == SIZE_MAX || comma == line_start || comma + 1U >= line_end ||
            row >= expected_count ||
            hwa_run_csv_u64(blob->data + line_start, comma - line_start,
                            &index_value) != 0 || index_value != row)
            goto malformed;
        number_size = line_end - (comma + 1U);
        if (number_size == 0U || number_size >= sizeof(number) ||
            memchr(blob->data + comma + 1U, '\0', number_size) != NULL)
            goto malformed;
        memcpy(number, blob->data + comma + 1U, number_size);
        number[number_size] = '\0';
        if (hwa_c_locale_parse_double(locale, number, &values[row]) != 0 ||
            hwa_c_locale_format_double(locale, canonical, sizeof(canonical),
                                       values[row]) != 0 ||
            strlen(canonical) != number_size ||
            memcmp(number, canonical, number_size) != 0)
            goto malformed;
        row++;
    }
    if (row != expected_count) goto malformed;
    return 0;
malformed:
    hwa_set_error(error, error_size, "malformed Stage 7 CSV probe");
    return -1;
}

static int hwa_run_probe_binary(const HWARunBlob *blob,
                                uint64_t expected_count,
                                double *values,
                                char *error,
                                size_t error_size)
{
    uint64_t body_bytes;
    uint64_t expected_bytes;
    uint64_t index;
    static const unsigned char magic[8] = {'H','W','A','P','R','B','1','\0'};
    if (sizeof(double) != 8U || FLT_RADIX != 2 || DBL_MANT_DIG != 53 ||
        DBL_MAX_EXP != 1024 || DBL_MIN_EXP != -1021 ||
        blob->size < 16U || memcmp(blob->data, magic, sizeof(magic)) != 0 ||
        hwa_run_le64(blob->data + 8U) != expected_count ||
        hwa_run_u64_multiply(expected_count, UINT64_C(8), &body_bytes) != 0 ||
        body_bytes > UINT64_MAX - UINT64_C(16)) goto malformed;
    expected_bytes = body_bytes + UINT64_C(16);
    if (expected_bytes != (uint64_t)blob->size) goto malformed;
    for (index = 0U; index < expected_count; ++index) {
        uint64_t bits = hwa_run_le64(blob->data + 16U + (size_t)index * 8U);
        double value;
        memcpy(&value, &bits, sizeof(value));
        if (!isfinite(value)) goto malformed;
        values[index] = value == 0.0 ? 0.0 : value;
    }
    return 0;
malformed:
    hwa_set_error(error, error_size, "malformed Stage 7 binary probe");
    return -1;
}

static int hwa_run_probe_parse_bytes_locale(HWARunProbeFormat format,
                                            const unsigned char *data,
                                            size_t size,
                                            uint64_t expected_count,
                                            const HWANumericLocale *locale,
                                            double *values,
                                            char *error,
                                            size_t error_size)
{
    HWARunBlob blob;
    if (data == NULL || values == NULL ||
        expected_count > UINT64_MAX / (uint64_t)sizeof(double)) {
        hwa_set_error(error, error_size, "invalid Stage 7 probe parse input");
        return -1;
    }
    memset(&blob, 0, sizeof(blob));
    blob.data = (unsigned char *)data;
    blob.size = size;
    if (format == HWA_RUN_PROBE_CSV_F64)
        return hwa_run_probe_csv(&blob, expected_count, locale, values,
                                 error, error_size);
    if (format == HWA_RUN_PROBE_BINARY_F64LE)
        return hwa_run_probe_binary(&blob, expected_count, values,
                                    error, error_size);
    hwa_set_error(error, error_size, "unknown Stage 7 probe format");
    return -1;
}

int hwa_run_probe_parse_bytes(HWARunProbeFormat format,
                              const unsigned char *data,
                              size_t size,
                              uint64_t expected_count,
                              double *values,
                              char *error,
                              size_t error_size)
{
    HWANumericLocale locale;
    int status;
    int locale_status;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_set_error(error, error_size,
                      "cannot enter the C numeric locale for Stage 7 probe");
        return -1;
    }
    status = hwa_run_probe_parse_bytes_locale(format, data, size,
                                              expected_count, &locale,
                                              values, error, error_size);
    locale_status = hwa_c_numeric_locale_end(&locale);
    if (locale_status != 0) {
        hwa_set_error(error, error_size,
                      "cannot leave the C numeric locale for Stage 7 probe");
        return -1;
    }
    return status;
}

static int hwa_run_probes_load(const HWARunManifest *manifest,
                               const HWANumericLocale *locale,
                               HWARunResult *result,
                               const HWARunFileIdentity *identities,
                               uint64_t *live_work,
                               HWARunProbeData **out_data,
                               char *error,
                               size_t error_size)
{
    HWARunProbeData *data;
    uint64_t row_bytes;
    uint64_t state_bytes;
    size_t data_index = 0U;
    size_t source_index;
    result->probe_count = manifest->probe_count;
    if (hwa_run_u64_multiply((uint64_t)result->probe_count,
                             sizeof(*result->probes), &row_bytes) != 0 ||
        hwa_run_u64_multiply((uint64_t)manifest->probe_count,
                             sizeof(*data), &state_bytes) != 0 ||
        hwa_run_work_reserve(live_work, row_bytes,
                             result->options.max_work_bytes,
                             error, error_size) != 0 ||
        hwa_run_work_reserve(live_work, state_bytes,
                             result->options.max_work_bytes,
                             error, error_size) != 0) return -1;
    result->probes = (HWARunProbe *)calloc(result->probe_count,
                                            sizeof(*result->probes));
    data = (HWARunProbeData *)calloc(manifest->probe_count, sizeof(*data));
    if ((result->probe_count != 0U && result->probes == NULL) ||
        (manifest->probe_count != 0U && data == NULL)) {
        free(data);
        hwa_set_error(error, error_size, "cannot allocate Stage 7 probe rows");
        return -1;
    }
    for (source_index = 0U; source_index < result->source_count; ++source_index) {
        HWARunSource *source = &result->sources[source_index];
        HWARunBlob blob;
        uint64_t value_bytes;
        HwaDspRunningStats stats;
        uint64_t value_index;
        double variance;
        HWARunProbe *probe;
        if (source->kind != HWA_RUN_SOURCE_PROBE) continue;
        if (source->value_count > result->options.max_probe_values ||
            hwa_run_u64_multiply(source->value_count, sizeof(double),
                                 &value_bytes) != 0 ||
            value_bytes > (uint64_t)SIZE_MAX ||
            hwa_run_work_reserve(live_work, value_bytes,
                                 result->options.max_work_bytes,
                                 error, error_size) != 0) goto failed;
        data[data_index].values = (double *)malloc((size_t)value_bytes);
        if (data[data_index].values == NULL) {
            hwa_set_error(error, error_size,
                          "cannot allocate Stage 7 probe values");
            goto failed;
        }
        if (hwa_run_blob_read(source->path, result->options.max_probe_bytes,
                              live_work, result->options.max_work_bytes,
                              &blob, error, error_size) != 0) goto failed;
        if (!hwa_run_identity_equal(&blob.identity, &identities[source_index])) {
            hwa_run_blob_free(&blob, live_work);
            hwa_set_error(error, error_size,
                          "Stage 7 probe identity changed before parse");
            goto failed;
        }
        if (!hwa_run_blob_hash_matches(&blob, source->sha256)) {
            hwa_run_blob_free(&blob, live_work);
            hwa_set_error(error, error_size,
                          "Stage 7 probe changed before parse");
            goto failed;
        }
        if (hwa_run_evaluations_add(result, source->value_count,
                                    error, error_size) != 0) {
            hwa_run_blob_free(&blob, live_work);
            goto failed;
        }
        if (hwa_run_probe_parse_bytes_locale(
                source->probe_format, blob.data, blob.size,
                source->value_count, locale, data[data_index].values,
                error, error_size) != 0) {
            hwa_run_blob_free(&blob, live_work);
            goto failed;
        }
        if (!hwa_run_blob_hash_matches(&blob, source->sha256)) {
            hwa_run_blob_free(&blob, live_work);
            hwa_set_error(error, error_size,
                          "Stage 7 probe changed during parse");
            goto failed;
        }
        hwa_run_blob_free(&blob, live_work);
        data[data_index].source_id = source->id;
        data[data_index].value_count = source->value_count;
        data[data_index].work_bytes = value_bytes;
        probe = &result->probes[data_index];
        probe->id = (uint64_t)data_index + UINT64_C(1);
        probe->source_id = source->id;
        probe->availability = HWA_RUN_AVAILABLE;
        probe->value_count = source->value_count;
        hwa_dsp_stats_reset(&stats);
        probe->minimum = data[data_index].values[0];
        probe->maximum = data[data_index].values[0];
        for (value_index = 0U; value_index < source->value_count; ++value_index) {
            double value = data[data_index].values[value_index];
            if (value < probe->minimum) probe->minimum = value;
            if (value > probe->maximum) probe->maximum = value;
            if (hwa_dsp_stats_push(&stats, value) != HWA_DSP_OK) goto failed;
        }
        if (hwa_dsp_stats_mean(&stats, &probe->mean) != HWA_DSP_OK ||
            hwa_dsp_stats_population_variance(&stats, &variance) != HWA_DSP_OK ||
            variance < 0.0) goto failed;
        probe->population_sd = sqrt(variance);
        probe->statistics_valid = 1;
        data_index++;
    }
    qsort(result->probes, result->probe_count, sizeof(*result->probes),
          hwa_run_probe_canonical_compare);
    for (data_index = 0U; data_index < result->probe_count; ++data_index)
        result->probes[data_index].id = (uint64_t)data_index + UINT64_C(1);
    *out_data = data;
    return 0;
failed:
    hwa_run_probe_data_free(data, manifest->probe_count);
    return -1;
}

typedef struct HWARunFacts {
    double values[HWA_RUN_FEATURES_PER_CLOCK];
    int valid[HWA_RUN_FEATURES_PER_CLOCK];
} HWARunFacts;

static const HWARunStemData *hwa_run_stem_data_find(
    const HWARunStemData *data,
    size_t count,
    uint64_t source_id)
{
    size_t index;
    for (index = 0U; index < count; ++index)
        if (data[index].source_id == source_id) return &data[index];
    return NULL;
}

static const HWARunProbeData *hwa_run_probe_data_find(
    const HWARunProbeData *data,
    size_t count,
    uint64_t source_id)
{
    size_t index;
    for (index = 0U; index < count; ++index)
        if (data[index].source_id == source_id) return &data[index];
    return NULL;
}

static int hwa_run_i64_end(int64_t start, uint64_t frames, int64_t *end)
{
    uint64_t limit;
    if (end == NULL) return -1;
    limit = (uint64_t)INT64_MAX - (uint64_t)start;
    if (frames > limit) return -1;
    if (start >= 0) {
        *end = start + (int64_t)frames;
    } else {
        uint64_t to_zero = UINT64_C(0) - (uint64_t)start;
        if (frames >= to_zero) {
            *end = (int64_t)(frames - to_zero);
        } else {
            uint64_t magnitude = to_zero - frames;
            *end = magnitude == (uint64_t)INT64_MAX + UINT64_C(1)
                       ? INT64_MIN
                       : -(int64_t)magnitude;
        }
    }
    return 0;
}

static int hwa_run_i64_subtract(int64_t left,
                                int64_t right,
                                int64_t *difference)
{
    if (difference == NULL ||
        (right > 0 && left < INT64_MIN + right) ||
        (right < 0 && left > INT64_MAX + right)) return -1;
    *difference = left - right;
    return 0;
}

int hwa_run_clock_derived_expected(
    const HWARunResult *result,
    const HWARunClock *clock,
    int64_t *start_offset_samples,
    int64_t *end_offset_samples,
    int64_t *drift_samples,
    uint64_t *overlap_frames,
    double *drift_ppm,
    uint32_t *quality_flags)
{
    const HWARunSource *reference;
    const HWARunSource *model;
    int64_t reference_end;
    int64_t model_end;
    int64_t overlap_start;
    int64_t overlap_end;
    uint64_t overlap = 0U;
    uint64_t shorter;
    uint32_t flags = 0U;
    if (result == NULL || clock == NULL || start_offset_samples == NULL ||
        end_offset_samples == NULL || drift_samples == NULL ||
        overlap_frames == NULL || drift_ppm == NULL || quality_flags == NULL ||
        clock->reference_source_id == 0U ||
        clock->reference_source_id > result->source_count ||
        clock->model_source_id == 0U ||
        clock->model_source_id > result->source_count) return -1;
    reference = &result->sources[clock->reference_source_id - 1U];
    model = &result->sources[clock->model_source_id - 1U];
    if (reference->kind != HWA_RUN_SOURCE_STEM ||
        reference->side != HWA_RUN_REFERENCE ||
        reference->role != HWA_RUN_STEM_FINAL ||
        model->kind != HWA_RUN_SOURCE_STEM || model->side != HWA_RUN_MODEL ||
        model->role != clock->role || reference->format.frames == 0U ||
        model->format.frames == 0U ||
        hwa_run_i64_end(reference->start_sample, reference->format.frames,
                        &reference_end) != 0 ||
        hwa_run_i64_end(model->start_sample, model->format.frames,
                        &model_end) != 0 ||
        hwa_run_i64_subtract(model->start_sample, reference->start_sample,
                             start_offset_samples) != 0 ||
        hwa_run_i64_subtract(model_end, reference_end,
                             end_offset_samples) != 0 ||
        hwa_run_i64_subtract(*end_offset_samples, *start_offset_samples,
                             drift_samples) != 0) return -1;
    overlap_start = reference->start_sample > model->start_sample
                        ? reference->start_sample
                        : model->start_sample;
    overlap_end = reference_end < model_end ? reference_end : model_end;
    if (overlap_end > overlap_start)
        overlap = (uint64_t)overlap_end - (uint64_t)overlap_start;
    *overlap_frames = overlap;
    *drift_ppm = 1000000.0 * (double)*drift_samples /
                 (double)reference->format.frames;
    if (!isfinite(*drift_ppm)) return -1;
    if (*start_offset_samples != 0) flags |= HWA_RUN_QUALITY_CLOCK_OFFSET;
    if (*drift_samples != 0) flags |= HWA_RUN_QUALITY_CLOCK_DRIFT;
    shorter = reference->format.frames < model->format.frames
                  ? reference->format.frames
                  : model->format.frames;
    if (overlap < shorter / UINT64_C(2) + shorter % UINT64_C(2))
        flags |= HWA_RUN_QUALITY_LOW_OVERLAP;
    *quality_flags = flags;
    return 0;
}

int hwa_run_feature_derived_expected(const HWARunFeature *feature,
                                     double *delta,
                                     double *normalized_gap)
{
    if (feature == NULL || delta == NULL || normalized_gap == NULL ||
        !feature->reference_valid || !feature->model_valid ||
        !isfinite(feature->reference_value) ||
        !isfinite(feature->model_value)) return -1;
    *delta = feature->model_value - feature->reference_value;
    *normalized_gap = fmin(fabs(*delta) / 12.0, 1.0);
    if (!isfinite(*delta) || !isfinite(*normalized_gap)) return -1;
    if (*delta == 0.0) *delta = 0.0;
    return 0;
}

int hwa_run_link_derived_expected(const HWARunResult *result,
                                  const HWARunLink *link,
                                  int64_t *lag_samples,
                                  double *r_squared,
                                  double *coverage,
                                  uint32_t *required_quality_flags)
{
    const HWARunSource *stem;
    uint64_t window_frames;
    uint64_t hop_frames;
    uint64_t window_count = 0U;
    uint64_t points;
    uint64_t absolute_lag;
    uint64_t product;
    uint32_t flags = 0U;
    if (result == NULL || link == NULL || lag_samples == NULL ||
        r_squared == NULL || coverage == NULL ||
        required_quality_flags == NULL ||
        !isfinite(link->coverage) || link->coverage < 0.0 ||
        link->coverage > 1.0 || link->stem_source_id == 0U ||
        link->stem_source_id > result->source_count ||
        result->sources == NULL) return -1;
    stem = &result->sources[link->stem_source_id - 1U];
    if (stem->kind != HWA_RUN_SOURCE_STEM) return -1;
    window_frames = ((uint64_t)result->clock_rate_hz *
                     HWA_RUN_LINK_WINDOW_MILLISECONDS + UINT64_C(500)) /
                    UINT64_C(1000);
    hop_frames = ((uint64_t)result->clock_rate_hz *
                  HWA_RUN_LINK_HOP_MILLISECONDS + UINT64_C(500)) /
                 UINT64_C(1000);
    if (window_frames == 0U) window_frames = 1U;
    if (hop_frames == 0U) hop_frames = 1U;
    if (stem->format.frames >= window_frames)
        window_count = UINT64_C(1) +
            (stem->format.frames - window_frames) / hop_frames;
    points = (uint64_t)link->point_count;
    if ((size_t)points != link->point_count || points > window_count)
        return -1;
    *coverage = window_count == 0U
                    ? 0.0
                    : (double)points / (double)window_count;
    absolute_lag = link->lag_hops < 0
                       ? (link->lag_hops == INT64_MIN
                              ? (uint64_t)INT64_MAX + UINT64_C(1)
                              : (uint64_t)(-link->lag_hops))
                       : (uint64_t)link->lag_hops;
    if (hwa_run_u64_multiply(absolute_lag, hop_frames, &product) != 0 ||
        product > (uint64_t)result->clock_rate_hz / UINT64_C(4) ||
        product > (uint64_t)INT64_MAX) return -1;
    *lag_samples = link->lag_hops < 0 ? -(int64_t)product : (int64_t)product;
    if (link->fit_valid) {
        if (!isfinite(link->correlation) || fabs(link->correlation) > 1.0)
            return -1;
        *r_squared = link->correlation * link->correlation;
        if (*r_squared > 1.0) *r_squared = 1.0;
    } else {
        *r_squared = 0.0;
    }
    if (*coverage < 0.5) flags |= HWA_RUN_QUALITY_LOW_OVERLAP;
    if (!link->fit_valid && link->point_count >= HWA_RUN_MIN_LINK_PAIRS)
        flags |= HWA_RUN_QUALITY_LOW_VARIANCE;
    *required_quality_flags = flags;
    return 0;
}

static int hwa_run_band_for_frequency(double frequency,
                                      double nyquist,
                                      size_t *band)
{
    static const double lower[HWA_BAND_COUNT] = {
        0.0, 60.0, 120.0, 250.0, 500.0,
        1000.0, 2000.0, 4000.0, 8000.0, 16000.0
    };
    static const double upper[HWA_BAND_COUNT - 1U] = {
        60.0, 120.0, 250.0, 500.0, 1000.0,
        2000.0, 4000.0, 8000.0, 16000.0
    };
    size_t index;
    for (index = 0U; index < HWA_BAND_COUNT; ++index) {
        double high = index + 1U == HWA_BAND_COUNT ? nyquist : upper[index];
        if (high > lower[index] && frequency >= lower[index] &&
            (frequency < high ||
             (index + 1U == HWA_BAND_COUNT && frequency <= high))) {
            *band = index;
            return 0;
        }
    }
    return -1;
}

static int hwa_run_slice_facts(const double *samples,
                               uint64_t frame_count,
                               uint32_t rate_hz,
                               HWARunResult *result,
                               uint64_t *live_work,
                               HWARunFacts *facts,
                               char *error,
                               size_t error_size)
{
    enum { HWA_RUN_FFT_SIZE = 1024, HWA_RUN_FFT_HOP = 512 };
    long double sum_squares = 0.0L;
    double peak = 0.0;
    uint64_t frame;
    double *window = NULL;
    HwaDspComplex *bins = NULL;
    uint64_t fft_bytes = (uint64_t)HWA_RUN_FFT_SIZE *
        (uint64_t)(sizeof(*window) + sizeof(*bins));
    long double band_sum[HWA_BAND_COUNT] = {0.0L};
    uint64_t band_bins[HWA_BAND_COUNT] = {0U};
    uint64_t transform_count = 0U;
    uint64_t complete_transforms = 0U;
    uint64_t transform_evaluations = 0U;
    long double window_sum_squares = 0.0L;
    int status = -1;
    memset(facts, 0, sizeof(*facts));
    if (samples == NULL || frame_count == 0U) return 0;
    if (hwa_run_evaluations_add(result, frame_count, error, error_size) != 0)
        return -1;
    for (frame = 0U; frame < frame_count; ++frame) {
        double value = samples[frame];
        double absolute = fabs(value);
        if (!isfinite(value)) {
            hwa_set_error(error, error_size, "non-finite Stage 7 stem sample");
            return -1;
        }
        sum_squares += (long double)value * (long double)value;
        if (absolute > peak) peak = absolute;
    }
    if (sum_squares > 0.0L) {
        double rms = sqrt((double)(sum_squares / (long double)frame_count));
        facts->values[0] = 20.0 * log10(rms);
        facts->valid[0] = isfinite(facts->values[0]);
        if (peak > 0.0 && rms > 0.0) {
            facts->values[1] = 20.0 * log10(peak / rms);
            facts->valid[1] = isfinite(facts->values[1]);
        }
    }
    if (frame_count < HWA_RUN_FFT_SIZE) return 0;
    if (hwa_run_work_reserve(live_work, fft_bytes,
                             result->options.max_work_bytes,
                             error, error_size) != 0) return -1;
    window = (double *)malloc(HWA_RUN_FFT_SIZE * sizeof(*window));
    bins = (HwaDspComplex *)malloc(HWA_RUN_FFT_SIZE * sizeof(*bins));
    if (window == NULL || bins == NULL ||
        hwa_dsp_hann(window, HWA_RUN_FFT_SIZE) != HWA_DSP_OK) {
        hwa_set_error(error, error_size, "cannot allocate Stage 7 FFT work");
        goto cleanup;
    }
    for (frame = 0U; frame < HWA_RUN_FFT_SIZE; ++frame)
        window_sum_squares += (long double)window[frame] * window[frame];
    if (!(window_sum_squares > 0.0L)) goto cleanup;
    complete_transforms = UINT64_C(1) +
        (frame_count - HWA_RUN_FFT_SIZE) / HWA_RUN_FFT_HOP;
    if (hwa_run_u64_multiply(complete_transforms, HWA_RUN_FFT_SIZE,
                             &transform_evaluations) != 0 ||
        hwa_run_evaluations_add(result, transform_evaluations,
                                error, error_size) != 0) goto cleanup;
    for (frame = 0U;
         frame + HWA_RUN_FFT_SIZE <= frame_count;
         frame += HWA_RUN_FFT_HOP) {
        size_t index;
        for (index = 0U; index < HWA_RUN_FFT_SIZE; ++index) {
            bins[index].real = samples[frame + (uint64_t)index] * window[index];
            bins[index].imag = 0.0;
        }
        if (hwa_dsp_fft(bins, HWA_RUN_FFT_SIZE, 0) != HWA_DSP_OK) {
            hwa_set_error(error, error_size, "Stage 7 FFT failed");
            goto cleanup;
        }
        for (index = 0U; index <= HWA_RUN_FFT_SIZE / 2U; ++index) {
            double frequency = (double)index * (double)rate_hz /
                               (double)HWA_RUN_FFT_SIZE;
            size_t band;
            if (hwa_run_band_for_frequency(
                    frequency, (double)rate_hz / 2.0, &band) == 0) {
                long double real = (long double)bins[index].real;
                long double imag = (long double)bins[index].imag;
                long double power = (real * real + imag * imag) /
                    ((long double)HWA_RUN_FFT_SIZE * window_sum_squares);
                if (index != 0U && index != HWA_RUN_FFT_SIZE / 2U)
                    power *= 2.0L;
                band_sum[band] += power;
                band_bins[band]++;
            }
        }
        transform_count++;
    }
    if (transform_count != complete_transforms) goto cleanup;
    if (transform_count != 0U) {
        size_t band;
        for (band = 0U; band < HWA_BAND_COUNT; ++band) {
            if (band_bins[band] != 0U && band_sum[band] > 0.0L) {
                long double mean = band_sum[band] /
                    (long double)transform_count;
                facts->values[2U + band] = 10.0 * log10((double)mean);
                facts->valid[2U + band] =
                    isfinite(facts->values[2U + band]);
            }
        }
    }
    status = 0;
cleanup:
    free(window);
    free(bins);
    if (fft_bytes <= *live_work) *live_work -= fft_bytes;
    return status;
}

static int hwa_run_clocks_features_build(HWARunResult *result,
                                         const HWARunStemData *stem_data,
                                         size_t stem_count,
                                         uint64_t *live_work,
                                         char *error,
                                         size_t error_size)
{
    const HWARunSource *reference = NULL;
    const HWARunStemData *reference_data;
    size_t index;
    size_t model_count = 0U;
    size_t clock_index = 0U;
    uint64_t feature_count;
    uint64_t row_bytes = 0U;
    for (index = 0U; index < result->source_count; ++index) {
        const HWARunSource *source = &result->sources[index];
        if (source->kind == HWA_RUN_SOURCE_STEM &&
            source->side == HWA_RUN_REFERENCE &&
            source->role == HWA_RUN_STEM_FINAL) reference = source;
        if (source->kind == HWA_RUN_SOURCE_STEM &&
            source->side == HWA_RUN_MODEL) model_count++;
    }
    if (reference == NULL || model_count == 0U ||
        hwa_run_u64_multiply((uint64_t)model_count,
                             HWA_RUN_FEATURES_PER_CLOCK,
                             &feature_count) != 0 ||
        feature_count > result->options.max_result_rows ||
        feature_count > (uint64_t)SIZE_MAX) {
        hwa_set_error(error, error_size, "invalid Stage 7 stem cohort");
        return -1;
    }
    reference_data = hwa_run_stem_data_find(
        stem_data, stem_count, reference->id);
    if (reference_data == NULL) return -1;
    result->clock_count = model_count;
    result->feature_count = (size_t)feature_count;
    result->stage_count = HWA_RUN_STAGE_COUNT;
    if (hwa_run_array_bytes(result->clock_count, sizeof(*result->clocks),
                            &row_bytes) != 0 ||
        hwa_run_array_bytes(result->feature_count, sizeof(*result->features),
                            &row_bytes) != 0 ||
        hwa_run_array_bytes(result->stage_count, sizeof(*result->stages),
                            &row_bytes) != 0 ||
        hwa_run_work_reserve(live_work, row_bytes,
                             result->options.max_work_bytes,
                             error, error_size) != 0) return -1;
    result->clocks = (HWARunClock *)calloc(model_count,
                                            sizeof(*result->clocks));
    result->features = (HWARunFeature *)calloc(
        result->feature_count, sizeof(*result->features));
    result->stages = (HWARunStage *)calloc(
        result->stage_count, sizeof(*result->stages));
    if (result->clocks == NULL || result->features == NULL ||
        result->stages == NULL) {
        hwa_set_error(error, error_size,
                      "cannot allocate Stage 7 comparison rows");
        return -1;
    }
    for (index = HWA_RUN_STEM_SOURCE; index < HWA_RUN_STEM_ROLE_COUNT; ++index) {
        const HWARunSource *model = NULL;
        const HWARunStemData *model_data;
        HWARunClock *clock;
        int64_t reference_end;
        int64_t model_end;
        int64_t overlap_start;
        int64_t overlap_end;
        uint64_t overlap = 0U;
        uint64_t reference_offset = 0U;
        uint64_t model_offset = 0U;
        HWARunFacts reference_facts;
        HWARunFacts model_facts;
        size_t source_index;
        size_t feature_offset;
        for (source_index = 0U; source_index < result->source_count;
             ++source_index) {
            const HWARunSource *candidate = &result->sources[source_index];
            if (candidate->kind == HWA_RUN_SOURCE_STEM &&
                candidate->side == HWA_RUN_MODEL &&
                candidate->role == (HWARunStemRole)index) {
                model = candidate;
                break;
            }
        }
        if (model == NULL) continue;
        model_data = hwa_run_stem_data_find(stem_data, stem_count, model->id);
        if (model_data == NULL ||
            hwa_run_i64_end(reference->start_sample,
                            reference_data->frame_count, &reference_end) != 0 ||
            hwa_run_i64_end(model->start_sample,
                            model_data->frame_count, &model_end) != 0) {
            hwa_set_error(error, error_size, "Stage 7 stem endpoint overflows");
            return -1;
        }
        clock = &result->clocks[clock_index];
        clock->id = (uint64_t)clock_index + UINT64_C(1);
        clock->role = model->role;
        clock->reference_source_id = reference->id;
        clock->model_source_id = model->id;
        clock->availability = HWA_RUN_AVAILABLE;
        if (hwa_run_i64_subtract(model->start_sample, reference->start_sample,
                                 &clock->start_offset_samples) != 0 ||
            hwa_run_i64_subtract(model_end, reference_end,
                                 &clock->end_offset_samples) != 0 ||
            hwa_run_i64_subtract(clock->end_offset_samples,
                                 clock->start_offset_samples,
                                 &clock->drift_samples) != 0) {
            hwa_set_error(error, error_size, "Stage 7 clock offset overflows");
            return -1;
        }
        clock->drift_ppm = 1000000.0 * (double)clock->drift_samples /
                           (double)reference_data->frame_count;
        if (!isfinite(clock->drift_ppm)) {
            hwa_set_error(error, error_size, "Stage 7 clock drift is not finite");
            return -1;
        }
        if (clock->start_offset_samples != 0)
            clock->quality_flags |= HWA_RUN_QUALITY_CLOCK_OFFSET;
        if (clock->drift_samples != 0)
            clock->quality_flags |= HWA_RUN_QUALITY_CLOCK_DRIFT;
        overlap_start = reference->start_sample > model->start_sample
                            ? reference->start_sample
                            : model->start_sample;
        overlap_end = reference_end < model_end ? reference_end : model_end;
        if (overlap_end > overlap_start) {
            overlap = (uint64_t)overlap_end - (uint64_t)overlap_start;
            reference_offset = (uint64_t)overlap_start -
                               (uint64_t)reference->start_sample;
            model_offset = (uint64_t)overlap_start -
                           (uint64_t)model->start_sample;
        }
        clock->overlap_frames = overlap;
        {
            uint64_t shorter = reference_data->frame_count <
                                       model_data->frame_count
                                   ? reference_data->frame_count
                                   : model_data->frame_count;
            if (overlap < shorter / UINT64_C(2) + shorter % UINT64_C(2))
                clock->quality_flags |= HWA_RUN_QUALITY_LOW_OVERLAP;
        }
        if (hwa_run_slice_facts(
                reference_data->samples + reference_offset, overlap,
                result->clock_rate_hz, result, live_work, &reference_facts,
                error, error_size) != 0 ||
            hwa_run_slice_facts(
                model_data->samples + model_offset, overlap,
                result->clock_rate_hz, result, live_work, &model_facts,
                error, error_size) != 0) return -1;
        feature_offset = clock_index * HWA_RUN_FEATURES_PER_CLOCK;
        for (source_index = 0U; source_index < HWA_RUN_FEATURES_PER_CLOCK;
             ++source_index) {
            HWARunFeature *feature =
                &result->features[feature_offset + source_index];
            feature->id = (uint64_t)(feature_offset + source_index) + UINT64_C(1);
            feature->clock_id = clock->id;
            feature->role = clock->role;
            if (hwa_run_feature_catalog_at(source_index, &feature->kind,
                                           &feature->index,
                                           &feature->unit) != 0) return -1;
            feature->quality_flags = clock->quality_flags;
            if (reference_facts.valid[source_index]) {
                feature->reference_value = reference_facts.values[source_index];
                feature->reference_valid = 1;
            }
            if (model_facts.valid[source_index]) {
                feature->model_value = model_facts.values[source_index];
                feature->model_valid = 1;
            }
            if (reference_facts.valid[source_index] &&
                model_facts.valid[source_index]) {
                feature->availability = HWA_RUN_AVAILABLE;
                feature->delta = feature->model_value - feature->reference_value;
                feature->normalized_gap = fmin(fabs(feature->delta) / 12.0, 1.0);
                feature->reference_valid = 1;
                feature->model_valid = 1;
                feature->delta_valid = 1;
                feature->gap_valid = 1;
            } else {
                feature->availability = HWA_RUN_INSUFFICIENT;
            }
        }
        clock_index++;
    }
    if (clock_index != result->clock_count ||
        hwa_run_stage_rows_rebuild(result, error, error_size) != 0) return -1;
    return 0;
}

static int hwa_run_probe_index_at(const HWARunSource *probe,
                                  uint32_t clock_rate_hz,
                                  int64_t clock_sample,
                                  uint64_t *index)
{
    uint64_t delta;
    uint64_t numerator;
    uint64_t denominator;
    if (probe == NULL || index == NULL || clock_rate_hz == 0U ||
        probe->rate_numerator == 0U || probe->rate_denominator == 0U ||
        clock_sample < probe->start_sample) return -1;
    delta = (uint64_t)clock_sample - (uint64_t)probe->start_sample;
    if (hwa_run_u64_multiply(delta, probe->rate_numerator, &numerator) == 0 &&
        hwa_run_u64_multiply((uint64_t)clock_rate_hz,
                             probe->rate_denominator, &denominator) == 0 &&
        denominator != 0U) {
        *index = numerator / denominator;
    } else {
        HWARunU128 wide_numerator = hwa_run_u128_multiply(
            delta, probe->rate_numerator);
        HWARunU128 wide_denominator = hwa_run_u128_multiply(
            (uint64_t)clock_rate_hz, probe->rate_denominator);
        if (hwa_run_u128_divide_u64(wide_numerator, wide_denominator,
                                    index) != 0) return -1;
    }
    return *index < probe->value_count ? 0 : -1;
}

static int hwa_run_link_pair_stats(const double *rms,
                                   const unsigned char *rms_valid,
                                   uint64_t window_count,
                                   uint64_t hop_frames,
                                   int64_t stem_start,
                                   uint64_t window_frames,
                                   const HWARunSource *probe,
                                   const HWARunProbeData *probe_data,
                                   uint32_t clock_rate_hz,
                                   int64_t lag_hops,
                                   HwaDspRunningCovariance *stats,
                                   size_t *point_count)
{
    uint64_t window;
    int64_t lag_samples;
    if (hop_frames > (uint64_t)INT64_MAX ||
        (lag_hops > 0 && (uint64_t)lag_hops >
             (uint64_t)INT64_MAX / hop_frames) ||
        (lag_hops < 0 && lag_hops != INT64_MIN &&
         (uint64_t)(-lag_hops) > (uint64_t)INT64_MAX / hop_frames)) return -1;
    lag_samples = lag_hops * (int64_t)hop_frames;
    hwa_dsp_covariance_reset(stats);
    *point_count = 0U;
    for (window = 0U; window < window_count; ++window) {
        uint64_t local_center;
        int64_t center;
        int64_t probe_sample;
        uint64_t probe_index;
        if (!rms_valid[window]) continue;
        if (hwa_run_u64_multiply(window, hop_frames, &local_center) != 0 ||
            local_center > UINT64_MAX - window_frames / 2U) return -1;
        local_center += window_frames / 2U;
        if (local_center > (uint64_t)INT64_MAX ||
            stem_start > INT64_MAX - (int64_t)local_center) return -1;
        center = stem_start + (int64_t)local_center;
        if (hwa_run_i64_subtract(center, lag_samples, &probe_sample) != 0)
            continue;
        if (hwa_run_probe_index_at(probe, clock_rate_hz, probe_sample,
                                   &probe_index) != 0) continue;
        if (hwa_dsp_covariance_push(stats, probe_data->values[probe_index],
                                    rms[window]) != HWA_DSP_OK) return -1;
        (*point_count)++;
    }
    return 0;
}

static int hwa_run_link_analyze(HWARunResult *result,
                                HWARunLink *link,
                                const HWARunStemData *stem_data,
                                const HWARunProbeData *probe_data,
                                uint64_t *live_work,
                                char *error,
                                size_t error_size)
{
    const HWARunSource *stem = &result->sources[link->stem_source_id - 1U];
    const HWARunSource *probe = &result->sources[link->probe_source_id - 1U];
    uint64_t window_frames =
        ((uint64_t)result->clock_rate_hz * HWA_RUN_LINK_WINDOW_MILLISECONDS +
         UINT64_C(500)) / UINT64_C(1000);
    uint64_t hop_frames =
        ((uint64_t)result->clock_rate_hz * HWA_RUN_LINK_HOP_MILLISECONDS +
         UINT64_C(500)) / UINT64_C(1000);
    uint64_t max_lag_samples = (uint64_t)result->clock_rate_hz / UINT64_C(4);
    uint64_t max_lag_hops;
    uint64_t lag_count;
    uint64_t lag_evaluations;
    uint64_t window_count;
    uint64_t rms_bytes;
    uint64_t valid_bytes;
    uint64_t rms_work = 0U;
    uint64_t rms_evaluations;
    double *rms = NULL;
    unsigned char *rms_valid = NULL;
    uint64_t window;
    int64_t lag;
    int64_t best_lag = 0;
    double best_correlation = 0.0;
    size_t best_count = 0U;
    size_t maximum_usable_count = 0U;
    int best_valid = 0;
    int variance_failure = 0;
    int status = -1;
    if (window_frames == 0U) window_frames = 1U;
    if (hop_frames == 0U) hop_frames = 1U;
    max_lag_hops = max_lag_samples / hop_frames;
    link->availability = HWA_RUN_INSUFFICIENT;
    if (stem_data->frame_count < window_frames) {
        link->quality_flags = HWA_RUN_QUALITY_LOW_OVERLAP;
        return 0;
    }
    window_count = UINT64_C(1) +
        (stem_data->frame_count - window_frames) / hop_frames;
    if (hwa_run_u64_multiply(window_count, sizeof(*rms), &rms_bytes) != 0 ||
        hwa_run_u64_multiply(window_count, sizeof(*rms_valid),
                             &valid_bytes) != 0 ||
        hwa_run_add_u64(&rms_work, rms_bytes) != 0 ||
        hwa_run_add_u64(&rms_work, valid_bytes) != 0 ||
        rms_bytes > (uint64_t)SIZE_MAX || valid_bytes > (uint64_t)SIZE_MAX ||
        hwa_run_work_reserve(live_work, rms_work,
                             result->options.max_work_bytes,
                             error, error_size) != 0) return -1;
    rms = (double *)malloc((size_t)rms_bytes);
    rms_valid = (unsigned char *)calloc((size_t)window_count,
                                         sizeof(*rms_valid));
    if (rms == NULL || rms_valid == NULL) {
        hwa_set_error(error, error_size, "cannot allocate Stage 7 link work");
        goto cleanup;
    }
    if (hwa_run_u64_multiply(window_count, window_frames,
                             &rms_evaluations) != 0 ||
        hwa_run_evaluations_add(result, rms_evaluations,
                                error, error_size) != 0) goto cleanup;
    for (window = 0U; window < window_count; ++window) {
        uint64_t start;
        uint64_t offset;
        long double sum = 0.0L;
        if (hwa_run_u64_multiply(window, hop_frames, &start) != 0)
            goto cleanup;
        for (offset = 0U; offset < window_frames; ++offset) {
            double value = stem_data->samples[start + offset];
            sum += (long double)value * (long double)value;
        }
        if (sum > 0.0L) {
            rms[window] = 10.0 * log10(
                (double)(sum / (long double)window_frames));
            rms_valid[window] = (unsigned char)isfinite(rms[window]);
        }
    }
    if (max_lag_hops > (UINT64_MAX - UINT64_C(1)) / UINT64_C(2))
        goto cleanup;
    lag_count = max_lag_hops * UINT64_C(2) + UINT64_C(1);
    if (hwa_run_u64_multiply(lag_count, window_count,
                             &lag_evaluations) != 0 ||
        hwa_run_evaluations_add(result, lag_evaluations,
                                error, error_size) != 0) goto cleanup;
    for (lag = -(int64_t)max_lag_hops;
         lag <= (int64_t)max_lag_hops; ++lag) {
        HwaDspRunningCovariance stats;
        size_t count;
        double correlation;
        if (hwa_run_link_pair_stats(
                rms, rms_valid, window_count, hop_frames, stem->start_sample,
                window_frames, probe, probe_data, result->clock_rate_hz,
                lag, &stats, &count) != 0) goto cleanup;
        if (count > maximum_usable_count) maximum_usable_count = count;
        if (count < HWA_RUN_MIN_LINK_PAIRS ||
            hwa_dsp_covariance_correlation(&stats, &correlation) != HWA_DSP_OK ||
            !isfinite(correlation)) {
            if (count >= HWA_RUN_MIN_LINK_PAIRS) variance_failure = 1;
            continue;
        }
        if (!best_valid || fabs(correlation) > fabs(best_correlation) ||
            (fabs(correlation) == fabs(best_correlation) &&
             (llabs(lag) < llabs(best_lag) ||
              (llabs(lag) == llabs(best_lag) && lag < best_lag)))) {
            best_valid = 1;
            best_lag = lag;
            best_correlation = correlation;
            best_count = count;
        }
    }
    if (best_valid) {
        HwaDspRunningCovariance stats;
        size_t count;
        long double variance_x;
        if (hwa_run_evaluations_add(result, window_count,
                                    error, error_size) != 0) goto cleanup;
        if (hwa_run_link_pair_stats(
                rms, rms_valid, window_count, hop_frames, stem->start_sample,
                window_frames, probe, probe_data, result->clock_rate_hz,
                best_lag, &stats, &count) != 0 || count != best_count ||
            stats.count == 0U) goto cleanup;
        variance_x = stats.m2_x / (long double)stats.count;
        if (!(variance_x > 0.0L)) {
            variance_failure = 1;
        } else {
            link->availability = HWA_RUN_AVAILABLE;
            link->lag_hops = best_lag;
            link->lag_samples = best_lag * (int64_t)hop_frames;
            link->correlation = best_correlation;
            link->slope = (double)((stats.c2 / (long double)stats.count) /
                                   variance_x);
            link->intercept = (double)(stats.mean_y -
                (long double)link->slope * stats.mean_x);
            link->r_squared = best_correlation * best_correlation;
            if (link->r_squared > 1.0) link->r_squared = 1.0;
            link->point_count = count;
            link->coverage = (double)count / (double)window_count;
            if (link->coverage < 0.5)
                link->quality_flags |= HWA_RUN_QUALITY_LOW_OVERLAP;
            link->fit_valid = 1;
        }
    }
    if (!link->fit_valid) {
        link->point_count = maximum_usable_count;
        link->coverage = window_count == 0U
                             ? 0.0
                             : (double)maximum_usable_count /
                                   (double)window_count;
        if (link->coverage < 0.5)
            link->quality_flags |= HWA_RUN_QUALITY_LOW_OVERLAP;
        if (variance_failure)
            link->quality_flags |= HWA_RUN_QUALITY_LOW_VARIANCE;
    }
    status = 0;
cleanup:
    free(rms);
    free(rms_valid);
    if (rms_work <= *live_work) *live_work -= rms_work;
    return status;
}

static int hwa_run_links_build(const HWARunManifest *manifest,
                               HWARunResult *result,
                               const HWARunStemData *stem_data,
                               size_t stem_count,
                               const HWARunProbeData *probe_data,
                               size_t probe_count,
                               uint64_t *live_work,
                               char *error,
                               size_t error_size)
{
    uint64_t row_bytes;
    size_t index;
    result->link_count = manifest->link_count;
    if (hwa_run_u64_multiply((uint64_t)result->link_count,
                             sizeof(*result->links), &row_bytes) != 0 ||
        hwa_run_work_reserve(live_work, row_bytes,
                             result->options.max_work_bytes,
                             error, error_size) != 0) return -1;
    result->links = (HWARunLink *)calloc(result->link_count,
                                          sizeof(*result->links));
    if (result->link_count != 0U && result->links == NULL) {
        hwa_set_error(error, error_size, "cannot allocate Stage 7 links");
        return -1;
    }
    for (index = 0U; index < manifest->link_count; ++index) {
        const HWARunManifestLink *declared = &manifest->links[index];
        const HWARunSource *stem = hwa_run_source_find(result,
                                                        declared->stem_id);
        const HWARunSource *probe = hwa_run_source_find(result,
                                                         declared->probe_id);
        HWARunLink *link = &result->links[index];
        if (stem == NULL || probe == NULL ||
            stem->kind != HWA_RUN_SOURCE_STEM ||
            probe->kind != HWA_RUN_SOURCE_PROBE) return -1;
        link->stem_source_id = stem->id;
        link->probe_source_id = probe->id;
        link->feature = declared->feature;
        link->feature_index = declared->feature_index;
    }
    qsort(result->links, result->link_count, sizeof(*result->links),
          hwa_run_link_canonical_compare);
    for (index = 0U; index < result->link_count; ++index) {
        HWARunLink *link = &result->links[index];
        const HWARunStemData *stem = hwa_run_stem_data_find(
            stem_data, stem_count, link->stem_source_id);
        const HWARunProbeData *probe = hwa_run_probe_data_find(
            probe_data, probe_count, link->probe_source_id);
        link->id = (uint64_t)index + UINT64_C(1);
        if (stem == NULL || probe == NULL ||
            hwa_run_link_analyze(result, link, stem, probe, live_work,
                                 error, error_size) != 0) return -1;
    }
    return 0;
}

static uint64_t hwa_run_manifest_live_bytes(const HWARunManifest *manifest)
{
    uint64_t bytes = 0U;
    size_t index;
    if (hwa_run_array_bytes(manifest->stem_capacity, sizeof(*manifest->stems),
                            &bytes) != 0 ||
        hwa_run_array_bytes(manifest->probe_capacity,
                            sizeof(*manifest->probes), &bytes) != 0 ||
        hwa_run_array_bytes(manifest->link_capacity, sizeof(*manifest->links),
                            &bytes) != 0) return UINT64_MAX;
    for (index = 0U; index < manifest->stem_count; ++index)
        if (hwa_run_string_bytes(manifest->stems[index].id, &bytes) != 0)
            return UINT64_MAX;
    for (index = 0U; index < manifest->probe_count; ++index)
        if (hwa_run_string_bytes(manifest->probes[index].id, &bytes) != 0 ||
            hwa_run_string_bytes(manifest->probes[index].name, &bytes) != 0 ||
            hwa_run_string_bytes(manifest->probes[index].unit, &bytes) != 0)
            return UINT64_MAX;
    for (index = 0U; index < manifest->link_count; ++index)
        if (hwa_run_string_bytes(manifest->links[index].stem_id, &bytes) != 0 ||
            hwa_run_string_bytes(manifest->links[index].probe_id, &bytes) != 0)
            return UINT64_MAX;
    return bytes;
}

static uint64_t hwa_run_source_live_bytes(const HWARunResult *result)
{
    uint64_t bytes = 0U;
    size_t index;
    if (hwa_run_array_bytes(result->source_count, sizeof(*result->sources),
                            &bytes) != 0) return UINT64_MAX;
    for (index = 0U; index < result->source_count; ++index) {
        const HWARunSource *source = &result->sources[index];
        if (hwa_run_string_bytes(source->binding_id, &bytes) != 0 ||
            hwa_run_string_bytes(source->path, &bytes) != 0 ||
            hwa_run_string_bytes(source->probe_name, &bytes) != 0 ||
            hwa_run_string_bytes(source->unit, &bytes) != 0) return UINT64_MAX;
    }
    return bytes;
}

static uint64_t hwa_run_source_required_bytes(
    const HWARunManifest *manifest,
    const HWARunBinding *bindings,
    size_t binding_count)
{
    uint64_t bytes = 0U;
    size_t source_count;
    size_t index;
    if (manifest == NULL ||
        manifest->stem_count > SIZE_MAX - manifest->probe_count)
        return UINT64_MAX;
    source_count = manifest->stem_count + manifest->probe_count;
    if (hwa_run_array_bytes(source_count, sizeof(HWARunSource), &bytes) != 0)
        return UINT64_MAX;
    for (index = 0U; index < manifest->stem_count; ++index) {
        const HWARunManifestStem *stem = &manifest->stems[index];
        const HWARunBinding *binding = hwa_run_binding_find(
            bindings, binding_count, stem->id);
        if (binding == NULL ||
            hwa_run_string_bytes(stem->id, &bytes) != 0 ||
            hwa_run_string_bytes(binding->path, &bytes) != 0)
            return UINT64_MAX;
    }
    for (index = 0U; index < manifest->probe_count; ++index) {
        const HWARunManifestProbe *probe = &manifest->probes[index];
        const HWARunBinding *binding = hwa_run_binding_find(
            bindings, binding_count, probe->id);
        if (binding == NULL ||
            hwa_run_string_bytes(probe->id, &bytes) != 0 ||
            hwa_run_string_bytes(binding->path, &bytes) != 0 ||
            hwa_run_string_bytes(probe->name, &bytes) != 0 ||
            hwa_run_string_bytes(probe->unit, &bytes) != 0)
            return UINT64_MAX;
    }
    return bytes;
}

static uint64_t hwa_run_warning_required_bytes(const HWARunResult *result)
{
    uint64_t bytes = 0U;
    size_t count = hwa_run_warning_spec_count(result);
    size_t index;
    if (hwa_run_array_bytes(count, sizeof(HWARunWarning), &bytes) != 0)
        return UINT64_MAX;
    for (index = 0U; index < count; ++index) {
        HWARunWarningSpec spec;
        if (hwa_run_warning_spec_at(result, index, &spec) != 0 ||
            hwa_run_string_bytes(spec.code, &bytes) != 0 ||
            hwa_run_string_bytes(spec.message, &bytes) != 0)
            return UINT64_MAX;
    }
    return bytes;
}

static void hwa_run_release_stem_work(HWARunStemData *data,
                                      size_t count,
                                      uint64_t *live_work)
{
    uint64_t state_bytes = 0U;
    size_t index;
    for (index = 0U; index < count; ++index)
        if (data[index].work_bytes <= *live_work)
            *live_work -= data[index].work_bytes;
    if (hwa_run_u64_multiply((uint64_t)count, sizeof(*data),
                             &state_bytes) == 0 &&
        state_bytes <= *live_work)
        *live_work -= state_bytes;
    hwa_run_stem_data_free(data, count);
}

static void hwa_run_release_probe_work(HWARunProbeData *data,
                                       size_t count,
                                       uint64_t *live_work)
{
    uint64_t state_bytes = 0U;
    size_t index;
    for (index = 0U; index < count; ++index)
        if (data[index].work_bytes <= *live_work)
            *live_work -= data[index].work_bytes;
    if (hwa_run_u64_multiply((uint64_t)count, sizeof(*data),
                             &state_bytes) == 0 &&
        state_bytes <= *live_work)
        *live_work -= state_bytes;
    hwa_run_probe_data_free(data, count);
}

static int hwa_run_analyze_impl(const char *manifest_path,
                                const HWARunBinding *bindings,
                                size_t binding_count,
                                const HWARunOptions *options,
                                const HWANumericLocale *locale,
                                HWARunResult *result,
                                char *error,
                                size_t error_size)
{
    HWARunBlob manifest_blob;
    HWARunManifest manifest;
    HWARunFileIdentity manifest_identity;
    HWARunFileIdentity *source_identities = NULL;
    HWARunStemData *stem_data = NULL;
    HWARunProbeData *probe_data = NULL;
    uint64_t live_work = 0U;
    uint64_t parser_live_start = 0U;
    uint64_t manifest_work = 0U;
    uint64_t source_work = 0U;
    uint64_t identity_work = 0U;
    uint64_t manifest_path_work = 0U;
    uint64_t warning_work = 0U;
    uint64_t retained = 0U;
    int status = -1;
    memset(&manifest_blob, 0, sizeof(manifest_blob));
    memset(&manifest, 0, sizeof(manifest));
    if (manifest_path == NULL || manifest_path[0] == '\0' ||
        strcmp(manifest_path, "-") == 0 ||
        hwa_run_path_identity(manifest_path, &manifest_identity,
                              error, error_size) != 0 ||
        manifest_identity.size > options->max_manifest_bytes ||
        hwa_run_blob_read(manifest_path, options->max_manifest_bytes,
                          &live_work, options->max_work_bytes,
                          &manifest_blob, error, error_size) != 0) goto cleanup;
    if (!hwa_run_identity_equal(&manifest_identity, &manifest_blob.identity)) {
        hwa_set_error(error, error_size,
                      "Stage 7 manifest changed before parse");
        goto cleanup;
    }
    memcpy(result->manifest_sha256, manifest_blob.sha256,
           HWA_SHA256_HEX_SIZE);
    parser_live_start = live_work;
    if (hwa_run_manifest_parse(manifest_blob.data, manifest_blob.size,
                               options, locale, &live_work, &manifest,
                               error, error_size) != 0) goto cleanup;
    if (!hwa_run_blob_hash_matches(&manifest_blob,
                                   result->manifest_sha256)) {
        hwa_set_error(error, error_size,
                      "Stage 7 manifest changed during parse");
        goto cleanup;
    }
    manifest_work = hwa_run_manifest_live_bytes(&manifest);
    if (manifest_work == UINT64_MAX || live_work < parser_live_start ||
        live_work - parser_live_start != manifest_work) {
        hwa_set_error(error, error_size,
                      "invalid Stage 7 manifest work ledger");
        goto cleanup;
    }
    hwa_run_blob_free(&manifest_blob, &live_work);
    if (hwa_run_bindings_validate(&manifest, bindings, binding_count,
                                  error, error_size) != 0) goto cleanup;
    source_work = hwa_run_source_required_bytes(
        &manifest, bindings, binding_count);
    if (source_work == UINT64_MAX ||
        hwa_run_u64_multiply(
            (uint64_t)(manifest.stem_count + manifest.probe_count),
            sizeof(*source_identities), &identity_work) != 0 ||
        hwa_run_work_reserve(&live_work, source_work,
                             options->max_work_bytes,
                             error, error_size) != 0 ||
        hwa_run_work_reserve(&live_work, identity_work,
                             options->max_work_bytes,
                             error, error_size) != 0 ||
        hwa_run_sources_build(&manifest, bindings, binding_count, options,
                              result, &source_identities,
                              error, error_size) != 0) goto cleanup;
    if (hwa_run_source_live_bytes(result) != source_work ||
        hwa_run_string_bytes(manifest_path, &manifest_path_work) != 0 ||
        hwa_run_work_reserve(&live_work, manifest_path_work,
                             options->max_work_bytes,
                             error, error_size) != 0) goto cleanup;
    result->manifest_path = hwa_run_copy_string(manifest_path);
    result->clock_rate_hz = manifest.clock_rate_hz;
    if (result->manifest_path == NULL ||
        hwa_run_stems_load(&manifest, result, source_identities, &live_work,
                           &stem_data, error, error_size) != 0 ||
        hwa_run_probes_load(&manifest, locale, result, source_identities,
                            &live_work, &probe_data,
                            error, error_size) != 0 ||
        hwa_run_clocks_features_build(
            result, stem_data, manifest.stem_count, &live_work,
            error, error_size) != 0 ||
        hwa_run_links_build(
            &manifest, result, stem_data, manifest.stem_count,
            probe_data, manifest.probe_count, &live_work,
            error, error_size) != 0)
        goto cleanup;
    warning_work = hwa_run_warning_required_bytes(result);
    if (warning_work == UINT64_MAX ||
        hwa_run_work_reserve(&live_work, warning_work,
                             options->max_work_bytes,
                             error, error_size) != 0 ||
        hwa_run_warnings_rebuild(result, error, error_size) != 0)
        goto cleanup;
    if (hwa_run_path_sha256_verify(
            manifest_path, &manifest_identity, options->max_manifest_bytes,
            result->manifest_sha256, error, error_size) != 0 ||
        hwa_run_sources_verify(result, source_identities,
                               error, error_size) != 0) {
        if (error != NULL && error_size > 0U && error[0] == '\0')
            hwa_set_error(error, error_size,
                          "Stage 7 input changed during analysis");
        goto cleanup;
    }
    hwa_run_release_stem_work(stem_data, manifest.stem_count, &live_work);
    stem_data = NULL;
    hwa_run_release_probe_work(probe_data, manifest.probe_count, &live_work);
    probe_data = NULL;
    free(source_identities);
    source_identities = NULL;
    if (identity_work <= live_work) live_work -= identity_work;
    hwa_run_manifest_free(&manifest);
    if (manifest_work <= live_work) live_work -= manifest_work;
    if (hwa_run_result_retained_bytes(result, &retained) != 0 ||
        retained > options->max_work_bytes || live_work != retained) {
        hwa_set_error(error, error_size,
                      "invalid Stage 7 peak-work ledger");
        goto cleanup;
    }
    result->retained_work_bytes = retained;
    if (hwa_run_result_validate(result, error, error_size) != 0) goto cleanup;
    status = 0;
cleanup:
    hwa_run_blob_free(&manifest_blob, &live_work);
    if (stem_data != NULL)
        hwa_run_release_stem_work(stem_data, manifest.stem_count, &live_work);
    if (probe_data != NULL)
        hwa_run_release_probe_work(probe_data, manifest.probe_count, &live_work);
    free(source_identities);
    hwa_run_manifest_free(&manifest);
    if (status != 0) hwa_run_result_free(result);
    return status;
}

int hwa_run_manifest_validate_bytes(const unsigned char *data,
                                    size_t size,
                                    const HWARunOptions *options,
                                    char *error,
                                    size_t error_size)
{
    HWARunOptions copied_options;
    HWANumericLocale locale;
    HWARunManifest manifest;
    uint64_t live_work = 0U;
    uint64_t before_parse;
    uint64_t manifest_work;
    int status = -1;
    int locale_status;
    memset(&manifest, 0, sizeof(manifest));
    if (options == NULL) hwa_run_options_default(&copied_options);
    else copied_options = *options;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (data == NULL || size > copied_options.max_manifest_bytes ||
        !hwa_run_options_valid(&copied_options) ||
        hwa_run_work_reserve(&live_work, (uint64_t)size + UINT64_C(1),
                             copied_options.max_work_bytes,
                             error, error_size) != 0) {
        if (error != NULL && error_size != 0U && error[0] == '\0')
            hwa_set_error(error, error_size, "invalid Stage 7 manifest bytes");
        return -1;
    }
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_set_error(error, error_size,
                      "cannot enter the C numeric locale for Stage 7");
        return -1;
    }
    before_parse = live_work;
    if (hwa_run_manifest_parse(data, size, &copied_options, &locale,
                               &live_work, &manifest, error, error_size) != 0)
        goto cleanup_locale;
    manifest_work = hwa_run_manifest_live_bytes(&manifest);
    if (manifest_work == UINT64_MAX || live_work < before_parse ||
        live_work - before_parse != manifest_work) {
        hwa_set_error(error, error_size,
                      "invalid Stage 7 manifest work ledger");
        goto cleanup_locale;
    }
    status = 0;
cleanup_locale:
    hwa_run_manifest_free(&manifest);
    locale_status = hwa_c_numeric_locale_end(&locale);
    if (locale_status != 0) {
        hwa_set_error(error, error_size,
                      "cannot leave the C numeric locale for Stage 7");
        return -1;
    }
    return status;
}

int hwa_analyze_run_files(const char *manifest_path,
                          const HWARunBinding *bindings,
                          size_t binding_count,
                          const HWARunOptions *options,
                          HWARunResult *result,
                          char *error,
                          size_t error_size)
{
    HWARunOptions copied_options;
    HWANumericLocale locale;
    int status;
    int locale_status;
    if (result == NULL) {
        hwa_set_error(error, error_size, "missing Stage 7 result");
        return -1;
    }
    if (options == NULL) hwa_run_options_default(&copied_options);
    else copied_options = *options;
    if (error != NULL && error_size > 0U) error[0] = '\0';
    memset(result, 0, sizeof(*result));
    result->options = copied_options;
    if (!hwa_run_options_valid(&copied_options) ||
        (binding_count != 0U && bindings == NULL)) {
        hwa_set_error(error, error_size, "invalid Stage 7 options or bindings");
        return -1;
    }
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_set_error(error, error_size,
                      "cannot enter the C numeric locale for Stage 7");
        return -1;
    }
    status = hwa_run_analyze_impl(
        manifest_path, bindings, binding_count, &copied_options, &locale,
        result, error, error_size);
    locale_status = hwa_c_numeric_locale_end(&locale);
    if (locale_status != 0) {
        if (status == 0) hwa_run_result_free(result);
        if (error != NULL && error_size > 0U &&
            (status == 0 || error[0] == '\0'))
            hwa_set_error(error, error_size,
                          "cannot restore the numeric locale after Stage 7");
        return -1;
    }
    if (status != 0) hwa_run_result_free(result);
    else if (error != NULL && error_size > 0U) error[0] = '\0';
    return status;
}

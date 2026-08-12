#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "run_file.h"

#include "alignment_file.h"
#include "internal.h"
#include "numeric_locale.h"
#include "sha256.h"

#include <errno.h>
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

#define HWA_RUN_FILE_MAX_FIELDS 32U
#define HWA_RUN_FILE_MAX_FIELD_BYTES 65536U
#define HWA_RUN_QUALITY_ALL ((UINT32_C(1) << 5) - 1U)
#define HWA_RUN_READER_MAX_ULPS 32U

typedef struct HWARunIdentity {
    uint64_t device;
    uint64_t file;
    uint64_t size;
} HWARunIdentity;

typedef struct HWARunCsvField {
    char *text;
    const unsigned char *raw;
    size_t raw_size;
    int quoted;
} HWARunCsvField;

typedef struct HWARunCsvRow {
    HWARunCsvField fields[HWA_RUN_FILE_MAX_FIELDS];
    size_t count;
} HWARunCsvRow;

typedef struct HWARunCsvReader {
    const unsigned char *data;
    size_t size;
    size_t cursor;
    size_t row_number;
} HWARunCsvReader;

typedef struct HWARunSavedMeta {
    HWARunOptions options;
    char *manifest_path;
    char manifest_sha256[HWA_SHA256_HEX_SIZE];
    uint32_t clock_rate_hz;
    uint64_t retained_work_bytes;
    uint64_t evaluation_count;
    size_t source_count;
    size_t clock_count;
    size_t feature_count;
    size_t stage_count;
    size_t probe_count;
    size_t link_count;
    size_t warning_count;
} HWARunSavedMeta;

typedef struct HWARunMetaKey {
    const char *key;
    const char *unit;
} HWARunMetaKey;

static const HWARunMetaKey hwa_run_meta_keys[] = {
    {"tool_version", ""},
    {"run_method_version", ""},
    {"manifest_path_hex", "hex-bytes"},
    {"manifest_sha256", ""},
    {"clock_rate_hz", "Hz"},
    {"build_compiler_family", ""},
    {"build_compiler_version", ""},
    {"build_c_standard", ""},
    {"build_target_os", ""},
    {"build_pointer_bits", "bits"},
    {"build_endianness", ""},
    {"build_mode", ""},
    {"decode_block_frames", "frames"},
    {"max_manifest_bytes", "bytes"},
    {"max_input_bytes", "bytes"},
    {"max_input_frames", "frames"},
    {"max_probe_bytes", "bytes"},
    {"max_probe_values", "values"},
    {"max_work_bytes", "bytes"},
    {"max_evaluations", "evaluations"},
    {"max_stems", "stems"},
    {"max_probes", "probes"},
    {"max_links", "links"},
    {"max_json_depth", "levels"},
    {"max_json_tokens", "tokens"},
    {"max_result_rows", "rows"},
    {"max_warnings", "warnings"},
    {"retained_work_bytes", "bytes"},
    {"evaluation_count", "evaluations"},
    {"source_count", "sources"},
    {"clock_count", "clocks"},
    {"feature_count", "features"},
    {"stage_count", "stages"},
    {"probe_count", "probes"},
    {"link_count", "links"},
    {"warning_count", "warnings"}
};

static void hwa_run_file_error(char *error,
                               size_t error_size,
                               const char *message)
{
    if (error != NULL && error_size != 0U) {
        (void)snprintf(error, error_size, "%s", message);
    }
}

static int hwa_run_csv_field(FILE *stream, const char *text)
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

static int hwa_run_path_hex(FILE *stream, const char *path)
{
    const unsigned char *cursor = (const unsigned char *)path;
    if (path == NULL) return -1;
    while (*cursor != 0U) {
        if (fprintf(stream, "%02x", (unsigned)*cursor++) < 0) return -1;
    }
    return 0;
}

static int hwa_run_number(FILE *stream, double value)
{
    if (!isfinite(value)) return -1;
    return fprintf(stream, "%.17g", value == 0.0 ? 0.0 : value) < 0 ? -1 : 0;
}

static int hwa_run_optional_number(FILE *stream, double value, int valid)
{
    return valid ? hwa_run_number(stream, value) : 0;
}

static int hwa_run_meta_text(FILE *stream,
                             const char *key,
                             const char *value,
                             const char *unit)
{
    return fputs("META,", stream) == EOF ||
           hwa_run_csv_field(stream, key) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_run_csv_field(stream, value) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_run_csv_field(stream, unit) != 0 ||
           fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_run_meta_u64(FILE *stream,
                            const char *key,
                            uint64_t value,
                            const char *unit)
{
    char text[32];
    int length = snprintf(text, sizeof(text), "%" PRIu64, value);
    if (length < 0 || (size_t)length >= sizeof(text)) return -1;
    return hwa_run_meta_text(stream, key, text, unit);
}

static int hwa_run_meta_size(FILE *stream,
                             const char *key,
                             size_t value,
                             const char *unit)
{
    char text[32];
    int length = snprintf(text, sizeof(text), "%zu", value);
    if (length < 0 || (size_t)length >= sizeof(text)) return -1;
    return hwa_run_meta_text(stream, key, text, unit);
}

static int hwa_run_meta_path(FILE *stream,
                             const char *key,
                             const char *path,
                             const char *unit)
{
    if (fputs("META,", stream) == EOF ||
        hwa_run_csv_field(stream, key) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_run_path_hex(stream, path) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_run_csv_field(stream, unit) != 0 ||
        fputs("\r\n", stream) == EOF) return -1;
    return 0;
}

static int hwa_run_write_meta(FILE *stream, const HWARunResult *result)
{
    const HWARunOptions *options = &result->options;
#define HWA_RUN_META_TEXT(key, value, unit)                                 \
    do { if (hwa_run_meta_text(stream, key, value, unit) != 0) return -1; } \
    while (0)
#define HWA_RUN_META_U64(key, value, unit)                                  \
    do { if (hwa_run_meta_u64(stream, key, value, unit) != 0) return -1; }  \
    while (0)
#define HWA_RUN_META_SIZE(key, value, unit)                                 \
    do { if (hwa_run_meta_size(stream, key, value, unit) != 0) return -1; } \
    while (0)
    HWA_RUN_META_TEXT("tool_version", HWA_VERSION, "");
    HWA_RUN_META_TEXT("run_method_version", HWA_RUN_METHOD_VERSION, "");
    if (hwa_run_meta_path(stream, "manifest_path_hex",
                          result->manifest_path, "hex-bytes") != 0) return -1;
    HWA_RUN_META_TEXT("manifest_sha256", result->manifest_sha256, "");
    HWA_RUN_META_U64("clock_rate_hz", result->clock_rate_hz, "Hz");
    HWA_RUN_META_TEXT("build_compiler_family", hwa_build_compiler_family(), "");
    HWA_RUN_META_TEXT("build_compiler_version", hwa_build_compiler_version(), "");
    HWA_RUN_META_TEXT("build_c_standard", hwa_build_c_standard(), "");
    HWA_RUN_META_TEXT("build_target_os", hwa_build_target_os(), "");
    HWA_RUN_META_U64("build_pointer_bits", hwa_build_pointer_bits(), "bits");
    HWA_RUN_META_TEXT("build_endianness", hwa_build_endianness(), "");
    HWA_RUN_META_TEXT("build_mode", hwa_build_mode(), "");
    HWA_RUN_META_SIZE("decode_block_frames", options->decode_block_frames, "frames");
    HWA_RUN_META_U64("max_manifest_bytes", options->max_manifest_bytes, "bytes");
    HWA_RUN_META_U64("max_input_bytes", options->max_input_bytes, "bytes");
    HWA_RUN_META_U64("max_input_frames", options->max_input_frames, "frames");
    HWA_RUN_META_U64("max_probe_bytes", options->max_probe_bytes, "bytes");
    HWA_RUN_META_U64("max_probe_values", options->max_probe_values, "values");
    HWA_RUN_META_U64("max_work_bytes", options->max_work_bytes, "bytes");
    HWA_RUN_META_U64("max_evaluations", options->max_evaluations, "evaluations");
    HWA_RUN_META_SIZE("max_stems", options->max_stems, "stems");
    HWA_RUN_META_SIZE("max_probes", options->max_probes, "probes");
    HWA_RUN_META_SIZE("max_links", options->max_links, "links");
    HWA_RUN_META_SIZE("max_json_depth", options->max_json_depth, "levels");
    HWA_RUN_META_SIZE("max_json_tokens", options->max_json_tokens, "tokens");
    HWA_RUN_META_SIZE("max_result_rows", options->max_result_rows, "rows");
    HWA_RUN_META_SIZE("max_warnings", options->max_warnings, "warnings");
    HWA_RUN_META_U64("retained_work_bytes", result->retained_work_bytes, "bytes");
    HWA_RUN_META_U64("evaluation_count", result->evaluation_count, "evaluations");
    HWA_RUN_META_SIZE("source_count", result->source_count, "sources");
    HWA_RUN_META_SIZE("clock_count", result->clock_count, "clocks");
    HWA_RUN_META_SIZE("feature_count", result->feature_count, "features");
    HWA_RUN_META_SIZE("stage_count", result->stage_count, "stages");
    HWA_RUN_META_SIZE("probe_count", result->probe_count, "probes");
    HWA_RUN_META_SIZE("link_count", result->link_count, "links");
    HWA_RUN_META_SIZE("warning_count", result->warning_count, "warnings");
#undef HWA_RUN_META_SIZE
#undef HWA_RUN_META_U64
#undef HWA_RUN_META_TEXT
    return 0;
}

static int hwa_run_write_source(FILE *stream, const HWARunSource *source)
{
    const HWAFormat *format = &source->format;
    if (fprintf(stream, "SOURCE,%" PRIu64 ",", source->id) < 0 ||
        hwa_run_csv_field(stream, source->binding_id) != 0 ||
        fputc(',', stream) == EOF || hwa_run_path_hex(stream, source->path) != 0 ||
        fprintf(stream, ",%s,", source->sha256) < 0 ||
        hwa_run_csv_field(stream, hwa_run_source_kind_name(source->kind)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_run_csv_field(stream, hwa_run_side_name(source->side)) != 0 ||
        fputc(',', stream) == EOF) return -1;
    if (source->kind == HWA_RUN_SOURCE_STEM &&
        hwa_run_csv_field(stream, hwa_run_stem_role_name(source->role)) != 0) {
        return -1;
    }
    if (fputc(',', stream) == EOF) return -1;
    if (source->kind == HWA_RUN_SOURCE_PROBE &&
        hwa_run_csv_field(stream,
                          hwa_run_probe_format_name(source->probe_format)) != 0) {
        return -1;
    }
    if (fputc(',', stream) == EOF ||
        (source->kind == HWA_RUN_SOURCE_PROBE &&
         hwa_run_csv_field(stream, source->probe_name) != 0) ||
        fputc(',', stream) == EOF ||
        (source->kind == HWA_RUN_SOURCE_PROBE &&
         hwa_run_csv_field(stream, source->unit) != 0) ||
        fprintf(stream, ",%" PRId64 ",", source->start_sample) < 0 ||
        hwa_run_number(stream, source->gain_db) != 0 ||
        fprintf(stream,
                ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64,
                source->rate_numerator, source->rate_denominator,
                source->file_bytes, source->value_count) < 0) return -1;
    if (source->kind == HWA_RUN_SOURCE_PROBE) {
        return fputs(",,,,,,,,,,,\r\n", stream) == EOF ? -1 : 0;
    }
    return fputc(',', stream) == EOF ||
           hwa_run_csv_field(stream, hwa_container_name(format->container)) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_run_csv_field(stream, hwa_encoding_name(format->encoding)) != 0 ||
           fprintf(stream,
                   ",%u,%" PRIu32 ",%u,%u,%u,%" PRIu32
                   ",%" PRIu64 ",%" PRIu64 ",",
                   (unsigned)format->channels, format->sample_rate_hz,
                   (unsigned)format->bits_per_sample,
                   (unsigned)format->valid_bits_per_sample,
                   (unsigned)format->block_align, format->channel_mask,
                   format->frames, format->data_bytes) < 0 ||
           hwa_run_number(stream, format->duration_seconds) != 0 ||
           fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_run_write_clock(FILE *stream, const HWARunClock *clock)
{
    if (fprintf(stream, "CLOCK,%" PRIu64 ",", clock->id) < 0 ||
        hwa_run_csv_field(stream, hwa_run_stem_role_name(clock->role)) != 0 ||
        fprintf(stream, ",%" PRIu64 ",%" PRIu64 ",",
                clock->reference_source_id, clock->model_source_id) < 0 ||
        hwa_run_csv_field(stream,
                          hwa_run_availability_name(clock->availability)) != 0 ||
        fprintf(stream,
                ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRIu64 ",",
                clock->start_offset_samples, clock->end_offset_samples,
                clock->drift_samples, clock->overlap_frames) < 0 ||
        hwa_run_number(stream, clock->drift_ppm) != 0 ||
        fprintf(stream, ",%" PRIu32 "\r\n", clock->quality_flags) < 0) return -1;
    return 0;
}

static int hwa_run_write_feature(FILE *stream, const HWARunFeature *feature)
{
    if (fprintf(stream, "FEATURE,%" PRIu64 ",%" PRIu64 ",",
                feature->id, feature->clock_id) < 0 ||
        hwa_run_csv_field(stream, hwa_run_stem_role_name(feature->role)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_run_csv_field(stream, hwa_run_feature_kind_name(feature->kind)) != 0 ||
        fprintf(stream, ",%" PRIu32 ",", feature->index) < 0 ||
        hwa_run_csv_field(stream, hwa_run_unit_name(feature->unit)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_run_csv_field(stream,
                          hwa_run_availability_name(feature->availability)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_run_optional_number(stream, feature->reference_value,
                                feature->reference_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_run_optional_number(stream, feature->model_value,
                                feature->model_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_run_optional_number(stream, feature->delta,
                                feature->delta_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_run_optional_number(stream, feature->normalized_gap,
                                feature->gap_valid) != 0 ||
        fprintf(stream, ",%" PRIu32 ",%d,%d,%d,%d\r\n",
                feature->quality_flags, feature->reference_valid ? 1 : 0,
                feature->model_valid ? 1 : 0, feature->delta_valid ? 1 : 0,
                feature->gap_valid ? 1 : 0) < 0) return -1;
    return 0;
}

static int hwa_run_write_stage(FILE *stream, const HWARunStage *stage)
{
    if (fprintf(stream, "STAGE,%" PRIu64 ",", stage->id) < 0 ||
        hwa_run_csv_field(stream, hwa_run_stem_role_name(stage->from_role)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_run_csv_field(stream, hwa_run_stem_role_name(stage->to_role)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_run_csv_field(stream,
                          hwa_run_availability_name(stage->availability)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_run_optional_number(stream, stage->prior_gap, stage->gap_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_run_optional_number(stream, stage->current_gap, stage->gap_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_run_optional_number(stream, stage->added_gap, stage->gap_valid) != 0 ||
        fprintf(stream, ",%zu,%" PRIu32 ",%d\r\n", stage->rank,
                stage->quality_flags, stage->gap_valid ? 1 : 0) < 0) return -1;
    return 0;
}

static int hwa_run_write_probe(FILE *stream, const HWARunProbe *probe)
{
    if (fprintf(stream, "PROBE,%" PRIu64 ",%" PRIu64 ",",
                probe->id, probe->source_id) < 0 ||
        hwa_run_csv_field(stream,
                          hwa_run_availability_name(probe->availability)) != 0 ||
        fprintf(stream, ",%" PRIu64 ",", probe->value_count) < 0 ||
        hwa_run_optional_number(stream, probe->minimum,
                                probe->statistics_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_run_optional_number(stream, probe->maximum,
                                probe->statistics_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_run_optional_number(stream, probe->mean,
                                probe->statistics_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_run_optional_number(stream, probe->population_sd,
                                probe->statistics_valid) != 0 ||
        fprintf(stream, ",%d\r\n", probe->statistics_valid ? 1 : 0) < 0) return -1;
    return 0;
}

static int hwa_run_write_link(FILE *stream, const HWARunLink *link)
{
    if (fprintf(stream,
                "LINK,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",",
                link->id, link->stem_source_id, link->probe_source_id) < 0 ||
        hwa_run_csv_field(stream, hwa_run_feature_kind_name(link->feature)) != 0 ||
        fprintf(stream, ",%" PRIu32 ",", link->feature_index) < 0 ||
        hwa_run_csv_field(stream,
                          hwa_run_availability_name(link->availability)) != 0 ||
        fprintf(stream, ",%" PRId64 ",%" PRId64 ",",
                link->lag_hops, link->lag_samples) < 0 ||
        hwa_run_optional_number(stream, link->correlation, link->fit_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_run_optional_number(stream, link->slope, link->fit_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_run_optional_number(stream, link->intercept, link->fit_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_run_optional_number(stream, link->r_squared, link->fit_valid) != 0 ||
        fprintf(stream, ",%zu,", link->point_count) < 0 ||
        hwa_run_number(stream, link->coverage) != 0 ||
        fprintf(stream, ",%" PRIu32 ",%d\r\n", link->quality_flags,
                link->fit_valid ? 1 : 0) < 0) return -1;
    return 0;
}

static int hwa_run_write_warning(FILE *stream, const HWARunWarning *warning)
{
    if (fprintf(stream, "WARNING,%" PRIu64 ",", warning->id) < 0 ||
        hwa_run_csv_field(stream, warning->code) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_run_csv_field(stream, warning->message) != 0 ||
        fputc(',', stream) == EOF) return -1;
    if (warning->source_id_valid &&
        fprintf(stream, "%" PRIu64, warning->source_id) < 0) return -1;
    if (fputc(',', stream) == EOF) return -1;
    if (warning->clock_id_valid &&
        fprintf(stream, "%" PRIu64, warning->clock_id) < 0) return -1;
    if (fputc(',', stream) == EOF) return -1;
    if (warning->stage_id_valid &&
        fprintf(stream, "%" PRIu64, warning->stage_id) < 0) return -1;
    if (fputc(',', stream) == EOF) return -1;
    if (warning->link_id_valid &&
        fprintf(stream, "%" PRIu64, warning->link_id) < 0) return -1;
    return fprintf(stream, ",%d,%d,%d,%d\r\n",
                   warning->source_id_valid ? 1 : 0,
                   warning->clock_id_valid ? 1 : 0,
                   warning->stage_id_valid ? 1 : 0,
                   warning->link_id_valid ? 1 : 0) < 0 ? -1 : 0;
}

static int hwa_run_file_write_impl(FILE *stream,
                                   const HWARunResult *result,
                                   char *error,
                                   size_t error_size)
{
    size_t index;
    if (stream == NULL) {
        hwa_run_file_error(error, error_size, "run output stream is null");
        return -1;
    }
    if (hwa_run_result_validate(result, error, error_size) != 0) return -1;
    if (fprintf(stream, "HWA_RUN,%u\r\n", HWA_RUN_FILE_SCHEMA_VERSION) < 0 ||
        hwa_run_write_meta(stream, result) != 0) goto write_error;
#define HWA_RUN_WRITE_ROWS(field, count, writer)                            \
    do {                                                                    \
        for (index = 0U; index < result->count; ++index) {                  \
            if (writer(stream, &result->field[index]) != 0) goto write_error; \
        }                                                                   \
    } while (0)
    HWA_RUN_WRITE_ROWS(sources, source_count, hwa_run_write_source);
    HWA_RUN_WRITE_ROWS(clocks, clock_count, hwa_run_write_clock);
    HWA_RUN_WRITE_ROWS(features, feature_count, hwa_run_write_feature);
    HWA_RUN_WRITE_ROWS(stages, stage_count, hwa_run_write_stage);
    HWA_RUN_WRITE_ROWS(probes, probe_count, hwa_run_write_probe);
    HWA_RUN_WRITE_ROWS(links, link_count, hwa_run_write_link);
    HWA_RUN_WRITE_ROWS(warnings, warning_count, hwa_run_write_warning);
#undef HWA_RUN_WRITE_ROWS
    return 0;

write_error:
    hwa_run_file_error(error, error_size, "cannot write run output");
    return -1;
}

int hwa_run_file_write(FILE *stream,
                       const HWARunResult *result,
                       char *error,
                       size_t error_size)
{
    HWANumericLocale locale;
    int status;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_run_file_error(error, error_size,
                           "cannot enter the C numeric locale for run output");
        return -1;
    }
    status = hwa_run_file_write_impl(stream, result, error, error_size);
    if (hwa_c_numeric_locale_end(&locale) != 0) {
        if (status == 0) {
            hwa_run_file_error(error, error_size,
                               "cannot restore the numeric locale after run output");
        }
        return -1;
    }
    return status;
}

#if defined(_WIN32)
static void hwa_run_windows_identity(
    const BY_HANDLE_FILE_INFORMATION *information,
    HWARunIdentity *identity)
{
    identity->device = (uint64_t)information->dwVolumeSerialNumber;
    identity->file = ((uint64_t)information->nFileIndexHigh << 32U) |
                     (uint64_t)information->nFileIndexLow;
    identity->size = ((uint64_t)information->nFileSizeHigh << 32U) |
                     (uint64_t)information->nFileSizeLow;
}

static int hwa_run_path_identity(const char *path,
                                 HWARunIdentity *identity)
{
    BY_HANDLE_FILE_INFORMATION information;
    HANDLE handle = CreateFileA(
        path, 0U, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(handle, &information)) {
        if (handle != INVALID_HANDLE_VALUE) (void)CloseHandle(handle);
        return -1;
    }
    (void)CloseHandle(handle);
    if ((information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        return -1;
    }
    hwa_run_windows_identity(&information, identity);
    return 0;
}

static int hwa_run_stream_identity(FILE *stream, HWARunIdentity *identity)
{
    BY_HANDLE_FILE_INFORMATION information;
    int descriptor = _fileno(stream);
    intptr_t raw = descriptor >= 0 ? _get_osfhandle(descriptor) : (intptr_t)-1;
    if (raw == (intptr_t)-1 ||
        !GetFileInformationByHandle((HANDLE)raw, &information) ||
        (information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        return -1;
    }
    hwa_run_windows_identity(&information, identity);
    return 0;
}
#else
static int hwa_run_path_identity(const char *path,
                                 HWARunIdentity *identity)
{
    struct stat facts;
    if (lstat(path, &facts) != 0 || !S_ISREG(facts.st_mode) ||
        facts.st_size < 0) return -1;
    identity->device = (uint64_t)facts.st_dev;
    identity->file = (uint64_t)facts.st_ino;
    identity->size = (uint64_t)facts.st_size;
    return 0;
}

static int hwa_run_stream_identity(FILE *stream, HWARunIdentity *identity)
{
    struct stat facts;
    int descriptor = fileno(stream);
    if (descriptor < 0 || fstat(descriptor, &facts) != 0 ||
        !S_ISREG(facts.st_mode) || facts.st_size < 0) return -1;
    identity->device = (uint64_t)facts.st_dev;
    identity->file = (uint64_t)facts.st_ino;
    identity->size = (uint64_t)facts.st_size;
    return 0;
}
#endif

static int hwa_run_same_identity(const HWARunIdentity *left,
                                 const HWARunIdentity *right)
{
    return left->device == right->device && left->file == right->file &&
           left->size == right->size;
}

static void hwa_run_digest_hex(const unsigned char digest[32],
                               char hex[HWA_SHA256_HEX_SIZE])
{
    static const char digits[] = "0123456789abcdef";
    size_t index;
    for (index = 0U; index < 32U; ++index) {
        hex[index * 2U] = digits[digest[index] >> 4U];
        hex[index * 2U + 1U] = digits[digest[index] & 0x0fU];
    }
    hex[64] = '\0';
}

static int hwa_run_read_bound_file(
    const char *path,
    uint64_t max_bytes,
    uint64_t max_work_bytes,
    unsigned char **data,
    size_t *size,
    char sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    HWARunIdentity before;
    HWARunIdentity opened;
    HWARunIdentity after;
    FILE *stream = NULL;
    HWASha256 hash;
    unsigned char digest[32];
    unsigned char *buffer = NULL;
    size_t file_size;
    size_t offset = 0U;
    int status = -1;
    *data = NULL;
    *size = 0U;
    if (path == NULL || path[0] == '\0' || strcmp(path, "-") == 0 ||
        hwa_run_path_identity(path, &before) != 0) {
        hwa_run_file_error(error, error_size,
                           "run result must be a named regular file");
        return -1;
    }
    if (before.size > max_bytes || before.size > (uint64_t)(SIZE_MAX - 1U)) {
        hwa_run_file_error(error, error_size,
                           "run result exceeds the current byte limit");
        return -1;
    }
    if (before.size == UINT64_MAX ||
        before.size + UINT64_C(1) > UINT64_MAX / 3U ||
        (before.size + UINT64_C(1)) * 3U > max_work_bytes) {
        hwa_run_file_error(error, error_size,
                           "run result reader exceeds the current work limit");
        return -1;
    }
    file_size = (size_t)before.size;
    buffer = (unsigned char *)malloc(file_size + 1U);
    if (buffer == NULL) {
        hwa_run_file_error(error, error_size,
                           "cannot allocate the run result reader");
        return -1;
    }
    stream = fopen(path, "rb");
    if (stream == NULL || hwa_run_stream_identity(stream, &opened) != 0 ||
        !hwa_run_same_identity(&before, &opened)) {
        hwa_run_file_error(error, error_size,
                           "run result changed before it was opened");
        goto cleanup;
    }
    hwa_sha256_init(&hash);
    while (offset < file_size) {
        size_t count = fread(buffer + offset, 1U, file_size - offset, stream);
        if (count == 0U) {
            hwa_run_file_error(error, error_size,
                               "cannot read the complete run result");
            goto cleanup;
        }
        hwa_sha256_update(&hash, buffer + offset, count);
        offset += count;
    }
    if (fgetc(stream) != EOF || ferror(stream)) {
        hwa_run_file_error(error, error_size,
                           "run result changed while it was read");
        goto cleanup;
    }
    if (fclose(stream) != 0) {
        stream = NULL;
        hwa_run_file_error(error, error_size,
                           "cannot close the run result");
        goto cleanup;
    }
    stream = NULL;
    if (hwa_run_path_identity(path, &after) != 0 ||
        !hwa_run_same_identity(&before, &after)) {
        hwa_run_file_error(error, error_size,
                           "run result changed while it was read");
        goto cleanup;
    }
    buffer[file_size] = 0U;
    hwa_sha256_final(&hash, digest);
    hwa_run_digest_hex(digest, sha256);
    *data = buffer;
    *size = file_size;
    buffer = NULL;
    status = 0;

cleanup:
    if (stream != NULL) (void)fclose(stream);
    free(buffer);
    return status;
}

static void hwa_run_csv_row_free(HWARunCsvRow *row)
{
    size_t index;
    for (index = 0U; index < row->count; ++index) {
        free(row->fields[index].text);
    }
    memset(row, 0, sizeof(*row));
}

static int hwa_run_csv_copy_field(HWARunCsvField *field,
                                  const unsigned char *raw,
                                  size_t raw_size,
                                  int quoted,
                                  char *error,
                                  size_t error_size)
{
    size_t decoded_size = 0U;
    size_t source;
    size_t target = 0U;
    char *text;
    if (raw_size > HWA_RUN_FILE_MAX_FIELD_BYTES + 2U) {
        hwa_run_file_error(error, error_size,
                           "run result field exceeds its byte limit");
        return -1;
    }
    if (!quoted) {
        decoded_size = raw_size;
    } else {
        if (raw_size < 2U || raw[0] != '"' || raw[raw_size - 1U] != '"') {
            hwa_run_file_error(error, error_size,
                               "run result has malformed CSV quoting");
            return -1;
        }
        for (source = 1U; source + 1U < raw_size; ++source) {
            if (raw[source] == '"') {
                if (source + 2U >= raw_size || raw[source + 1U] != '"') {
                    hwa_run_file_error(error, error_size,
                                       "run result has malformed CSV quoting");
                    return -1;
                }
                source++;
            }
            decoded_size++;
        }
    }
    if (decoded_size > HWA_RUN_FILE_MAX_FIELD_BYTES) {
        hwa_run_file_error(error, error_size,
                           "run result field exceeds its byte limit");
        return -1;
    }
    text = (char *)malloc(decoded_size + 1U);
    if (text == NULL) {
        hwa_run_file_error(error, error_size,
                           "cannot allocate a run result field");
        return -1;
    }
    if (!quoted) {
        memcpy(text, raw, decoded_size);
        target = decoded_size;
    } else {
        for (source = 1U; source + 1U < raw_size; ++source) {
            if (raw[source] == '"') source++;
            text[target++] = (char)raw[source];
        }
    }
    text[target] = '\0';
    if (memchr(text, '\0', decoded_size) != NULL) {
        free(text);
        hwa_run_file_error(error, error_size,
                           "run result fields cannot contain NUL bytes");
        return -1;
    }
    field->text = text;
    field->raw = raw;
    field->raw_size = raw_size;
    field->quoted = quoted;
    return 0;
}

static int hwa_run_csv_next(HWARunCsvReader *reader,
                            HWARunCsvRow *row,
                            char *error,
                            size_t error_size)
{
    int row_done = 0;
    memset(row, 0, sizeof(*row));
    if (reader->cursor == reader->size) return 0;
    while (!row_done) {
        size_t start;
        size_t raw_size;
        int quoted = 0;
        if (row->count == HWA_RUN_FILE_MAX_FIELDS) {
            hwa_run_file_error(error, error_size,
                               "run result row has too many fields");
            goto failure;
        }
        start = reader->cursor;
        if (reader->data[reader->cursor] == '"') {
            quoted = 1;
            reader->cursor++;
            for (;;) {
                if (reader->cursor >= reader->size) {
                    hwa_run_file_error(error, error_size,
                                       "run result ends inside a quoted field");
                    goto failure;
                }
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
                if (reader->data[reader->cursor] == '"') {
                    hwa_run_file_error(error, error_size,
                                       "run result has a quote in an unquoted field");
                    goto failure;
                }
                reader->cursor++;
            }
        }
        raw_size = reader->cursor - start;
        if (hwa_run_csv_copy_field(&row->fields[row->count],
                                   reader->data + start, raw_size, quoted,
                                   error, error_size) != 0) goto failure;
        row->count++;
        if (reader->cursor >= reader->size) {
            hwa_run_file_error(error, error_size,
                               "run result needs a final CRLF");
            goto failure;
        }
        if (reader->data[reader->cursor] == ',') {
            reader->cursor++;
            continue;
        }
        if (reader->data[reader->cursor] != '\r' ||
            reader->cursor + 1U >= reader->size ||
            reader->data[reader->cursor + 1U] != '\n') {
            hwa_run_file_error(error, error_size,
                               "run result must use CRLF row endings");
            goto failure;
        }
        reader->cursor += 2U;
        row_done = 1;
    }
    reader->row_number++;
    return 1;

failure:
    hwa_run_csv_row_free(row);
    return -1;
}

static int hwa_run_field_canonical(const HWARunCsvField *field)
{
    const unsigned char *text = (const unsigned char *)field->text;
    size_t text_size = strlen(field->text);
    size_t source;
    size_t raw = 0U;
    int needs_quote = 0;
    for (source = 0U; source < text_size; ++source) {
        if (text[source] == ',' || text[source] == '"' ||
            text[source] == '\r' || text[source] == '\n') needs_quote = 1;
    }
    if (needs_quote != field->quoted) return 0;
    if (!needs_quote) {
        return field->raw_size == text_size &&
               memcmp(field->raw, text, text_size) == 0;
    }
    if (field->raw_size < 2U || field->raw[raw++] != '"') return 0;
    for (source = 0U; source < text_size; ++source) {
        if (text[source] == '"' &&
            (raw >= field->raw_size || field->raw[raw++] != '"')) return 0;
        if (raw >= field->raw_size || field->raw[raw++] != text[source]) return 0;
    }
    return raw + 1U == field->raw_size && field->raw[raw] == '"';
}

static int hwa_run_row_canonical(const HWARunCsvRow *row)
{
    size_t index;
    for (index = 0U; index < row->count; ++index) {
        if (!hwa_run_field_canonical(&row->fields[index])) return 0;
    }
    return 1;
}

static int hwa_run_parse_u64(const HWARunCsvField *field, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;
    char canonical[32];
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

static int hwa_run_parse_size(const HWARunCsvField *field, size_t *value)
{
    uint64_t parsed;
    if (hwa_run_parse_u64(field, &parsed) != 0 || parsed > SIZE_MAX) return -1;
    *value = (size_t)parsed;
    return 0;
}

static int hwa_run_parse_u32(const HWARunCsvField *field, uint32_t *value)
{
    uint64_t parsed;
    if (hwa_run_parse_u64(field, &parsed) != 0 || parsed > UINT32_MAX) return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static int hwa_run_parse_u16(const HWARunCsvField *field, uint16_t *value)
{
    uint64_t parsed;
    if (hwa_run_parse_u64(field, &parsed) != 0 || parsed > UINT16_MAX) return -1;
    *value = (uint16_t)parsed;
    return 0;
}

static int hwa_run_parse_i64(const HWARunCsvField *field, int64_t *value)
{
    char *end = NULL;
    intmax_t parsed;
    char canonical[32];
    int length;
    if (field->quoted || field->text[0] == '\0' || field->text[0] == '+') return -1;
    errno = 0;
    parsed = strtoimax(field->text, &end, 10);
    if (errno == ERANGE || end == field->text || *end != '\0' ||
        parsed < INT64_MIN || parsed > INT64_MAX) return -1;
    *value = (int64_t)parsed;
    length = snprintf(canonical, sizeof(canonical), "%" PRId64, *value);
    return length >= 0 && (size_t)length < sizeof(canonical) &&
           strcmp(canonical, field->text) == 0 ? 0 : -1;
}

static int hwa_run_parse_bool(const HWARunCsvField *field, int *value)
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

static int hwa_run_parse_double(const HWANumericLocale *locale,
                                const HWARunCsvField *field,
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

static int hwa_run_parse_optional_double(
    const HWANumericLocale *locale,
    const HWARunCsvField *field,
    int valid,
    double *value)
{
    if (!valid) {
        if (field->quoted || field->text[0] != '\0') return -1;
        *value = 0.0;
        return 0;
    }
    return hwa_run_parse_double(locale, field, value);
}

static int hwa_run_sha_valid(const char *text)
{
    size_t index;
    if (text == NULL || strlen(text) != HWA_SHA256_HEX_SIZE - 1U) return 0;
    for (index = 0U; index < HWA_SHA256_HEX_SIZE - 1U; ++index) {
        if (!((text[index] >= '0' && text[index] <= '9') ||
              (text[index] >= 'a' && text[index] <= 'f'))) return 0;
    }
    return 1;
}

static int hwa_run_hex_value(unsigned char byte)
{
    if (byte >= '0' && byte <= '9') return (int)(byte - '0');
    if (byte >= 'a' && byte <= 'f') return (int)(byte - 'a') + 10;
    return -1;
}

static char *hwa_run_decode_path(const HWARunCsvField *field)
{
    size_t hex_size = strlen(field->text);
    size_t path_size;
    size_t index;
    char *path;
    if (field->quoted || hex_size == 0U || (hex_size & 1U) != 0U) return NULL;
    path_size = hex_size / 2U;
    path = (char *)malloc(path_size + 1U);
    if (path == NULL) return NULL;
    for (index = 0U; index < path_size; ++index) {
        int high = hwa_run_hex_value((unsigned char)field->text[index * 2U]);
        int low = hwa_run_hex_value((unsigned char)field->text[index * 2U + 1U]);
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
    path[path_size] = '\0';
    return path;
}

static int hwa_run_parse_availability(const HWARunCsvField *field,
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

static int hwa_run_parse_source_kind(const HWARunCsvField *field,
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

static int hwa_run_parse_side(const HWARunCsvField *field, HWARunSide *value)
{
    int item;
    if (field->quoted) return -1;
    for (item = HWA_RUN_REFERENCE; item < HWA_RUN_SIDE_COUNT; ++item) {
        if (strcmp(field->text, hwa_run_side_name((HWARunSide)item)) == 0) {
            *value = (HWARunSide)item;
            return 0;
        }
    }
    return -1;
}

static int hwa_run_parse_role(const HWARunCsvField *field,
                              int allow_zero,
                              HWARunStemRole *value)
{
    int item;
    if (field->quoted) return -1;
    if (allow_zero && field->text[0] == '\0') {
        *value = (HWARunStemRole)0;
        return 0;
    }
    for (item = HWA_RUN_STEM_SOURCE; item < HWA_RUN_STEM_ROLE_COUNT; ++item) {
        if (strcmp(field->text,
                   hwa_run_stem_role_name((HWARunStemRole)item)) == 0) {
            *value = (HWARunStemRole)item;
            return 0;
        }
    }
    return -1;
}

static int hwa_run_parse_probe_format(const HWARunCsvField *field,
                                      int allow_zero,
                                      HWARunProbeFormat *value)
{
    int item;
    if (field->quoted) return -1;
    if (allow_zero && field->text[0] == '\0') {
        *value = (HWARunProbeFormat)0;
        return 0;
    }
    for (item = HWA_RUN_PROBE_CSV_F64;
         item < HWA_RUN_PROBE_FORMAT_COUNT; ++item) {
        if (strcmp(field->text,
                   hwa_run_probe_format_name((HWARunProbeFormat)item)) == 0) {
            *value = (HWARunProbeFormat)item;
            return 0;
        }
    }
    return -1;
}

static int hwa_run_parse_feature_kind(const HWARunCsvField *field,
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

static int hwa_run_parse_unit(const HWARunCsvField *field, HWARunUnit *value)
{
    int item;
    if (field->quoted) return -1;
    for (item = HWA_RUN_UNIT_DBFS; item < HWA_RUN_UNIT_COUNT; ++item) {
        if (strcmp(field->text, hwa_run_unit_name((HWARunUnit)item)) == 0) {
            *value = (HWARunUnit)item;
            return 0;
        }
    }
    return -1;
}

static int hwa_run_parse_container(const HWARunCsvField *field,
                                   HWAContainer *value)
{
    int item;
    if (field->quoted) return -1;
    for (item = HWA_CONTAINER_RIFF; item <= HWA_CONTAINER_RF64; ++item) {
        if (strcmp(field->text, hwa_container_name((HWAContainer)item)) == 0) {
            *value = (HWAContainer)item;
            return 0;
        }
    }
    return -1;
}

static int hwa_run_parse_encoding(const HWARunCsvField *field,
                                  HWAEncoding *value)
{
    static const HWAEncoding values[] = {
        HWA_ENCODING_PCM, HWA_ENCODING_IEEE_FLOAT
    };
    size_t index;
    if (field->quoted) return -1;
    for (index = 0U; index < sizeof(values) / sizeof(values[0]); ++index) {
        if (strcmp(field->text, hwa_encoding_name(values[index])) == 0) {
            *value = values[index];
            return 0;
        }
    }
    return -1;
}

static int hwa_run_options_nonzero(const HWARunOptions *options)
{
    return options->decode_block_frames != 0U &&
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

static int hwa_run_parse_meta_value(const HWANumericLocale *locale,
                                    size_t index,
                                    const HWARunCsvField *field,
                                    HWARunSavedMeta *meta)
{
    uint64_t u64;
    size_t size;
    (void)locale;
    switch (index) {
    case 0U:
        return field->text[0] != '\0' && hwa_run_field_canonical(field) ? 0 : -1;
    case 1U:
        return !field->quoted &&
               strcmp(field->text, HWA_RUN_METHOD_VERSION) == 0 ? 0 : -1;
    case 2U:
        meta->manifest_path = hwa_run_decode_path(field);
        return meta->manifest_path != NULL ? 0 : -1;
    case 3U:
        if (field->quoted || !hwa_run_sha_valid(field->text)) return -1;
        memcpy(meta->manifest_sha256, field->text, HWA_SHA256_HEX_SIZE);
        return 0;
    case 4U:
        if (hwa_run_parse_u64(field, &u64) != 0 || u64 == 0U ||
            u64 > UINT32_MAX) return -1;
        meta->clock_rate_hz = (uint32_t)u64;
        return 0;
    case 5U:
    case 6U:
    case 7U:
    case 8U:
    case 10U:
    case 11U:
        return field->text[0] != '\0' && hwa_run_field_canonical(field) ? 0 : -1;
    case 9U:
        return hwa_run_parse_u64(field, &u64) == 0 && u64 != 0U ? 0 : -1;
    case 12U:
        return hwa_run_parse_size(field, &meta->options.decode_block_frames);
    case 13U:
        return hwa_run_parse_u64(field, &meta->options.max_manifest_bytes);
    case 14U:
        return hwa_run_parse_u64(field, &meta->options.max_input_bytes);
    case 15U:
        return hwa_run_parse_u64(field, &meta->options.max_input_frames);
    case 16U:
        return hwa_run_parse_u64(field, &meta->options.max_probe_bytes);
    case 17U:
        return hwa_run_parse_u64(field, &meta->options.max_probe_values);
    case 18U:
        return hwa_run_parse_u64(field, &meta->options.max_work_bytes);
    case 19U:
        return hwa_run_parse_u64(field, &meta->options.max_evaluations);
    case 20U:
        return hwa_run_parse_size(field, &meta->options.max_stems);
    case 21U:
        return hwa_run_parse_size(field, &meta->options.max_probes);
    case 22U:
        return hwa_run_parse_size(field, &meta->options.max_links);
    case 23U:
        return hwa_run_parse_size(field, &meta->options.max_json_depth);
    case 24U:
        return hwa_run_parse_size(field, &meta->options.max_json_tokens);
    case 25U:
        return hwa_run_parse_size(field, &meta->options.max_result_rows);
    case 26U:
        return hwa_run_parse_size(field, &meta->options.max_warnings);
    case 27U:
        return hwa_run_parse_u64(field, &meta->retained_work_bytes);
    case 28U:
        return hwa_run_parse_u64(field, &meta->evaluation_count);
    case 29U:
        size = 0U;
        if (hwa_run_parse_size(field, &size) != 0) return -1;
        meta->source_count = size;
        return 0;
    case 30U:
        return hwa_run_parse_size(field, &meta->clock_count);
    case 31U:
        return hwa_run_parse_size(field, &meta->feature_count);
    case 32U:
        return hwa_run_parse_size(field, &meta->stage_count);
    case 33U:
        return hwa_run_parse_size(field, &meta->probe_count);
    case 34U:
        return hwa_run_parse_size(field, &meta->link_count);
    case 35U:
        return hwa_run_parse_size(field, &meta->warning_count);
    default:
        return -1;
    }
}

static int hwa_run_read_meta(HWARunCsvReader *reader,
                             const HWANumericLocale *locale,
                             HWARunSavedMeta *meta,
                             char *error,
                             size_t error_size)
{
    size_t index;
    memset(meta, 0, sizeof(*meta));
    for (index = 0U;
         index < sizeof(hwa_run_meta_keys) / sizeof(hwa_run_meta_keys[0]);
         ++index) {
        HWARunCsvRow row;
        int status = hwa_run_csv_next(reader, &row, error, error_size);
        if (status != 1) {
            if (status == 0) {
                hwa_run_file_error(error, error_size,
                                   "run result ends inside META");
            }
            goto failure;
        }
        if (row.count != 4U || !hwa_run_row_canonical(&row) ||
            strcmp(row.fields[0].text, "META") != 0 ||
            strcmp(row.fields[1].text, hwa_run_meta_keys[index].key) != 0 ||
            strcmp(row.fields[3].text, hwa_run_meta_keys[index].unit) != 0 ||
            hwa_run_parse_meta_value(locale, index, &row.fields[2], meta) != 0) {
            hwa_run_csv_row_free(&row);
            hwa_run_file_error(error, error_size,
                               "run result has invalid or out-of-order META");
            goto failure;
        }
        hwa_run_csv_row_free(&row);
    }
    if (!hwa_run_options_nonzero(&meta->options) ||
        meta->options.decode_block_frames > 1048576U ||
        meta->retained_work_bytes == 0U ||
        meta->retained_work_bytes > meta->options.max_work_bytes ||
        meta->evaluation_count > meta->options.max_evaluations) {
        hwa_run_file_error(error, error_size,
                           "run result has invalid saved limits or counts");
        goto failure;
    }
    return 0;

failure:
    free(meta->manifest_path);
    meta->manifest_path = NULL;
    return -1;
}

static int hwa_run_counts_fit_limits(const HWARunSavedMeta *meta,
                                     const HWARunOptions *limits)
{
    size_t rows = 0U;
    if (limits->max_stems > SIZE_MAX - limits->max_probes ||
        meta->source_count > limits->max_stems + limits->max_probes ||
        meta->probe_count > limits->max_probes ||
        meta->link_count > limits->max_links ||
        meta->warning_count > limits->max_warnings ||
        meta->evaluation_count > limits->max_evaluations) return 0;
#define HWA_RUN_ADD_ROWS(value)                                             \
    do { if ((value) > SIZE_MAX - rows) return 0; rows += (value); } while (0)
    HWA_RUN_ADD_ROWS(meta->source_count);
    HWA_RUN_ADD_ROWS(meta->clock_count);
    HWA_RUN_ADD_ROWS(meta->feature_count);
    HWA_RUN_ADD_ROWS(meta->stage_count);
    HWA_RUN_ADD_ROWS(meta->probe_count);
    HWA_RUN_ADD_ROWS(meta->link_count);
    HWA_RUN_ADD_ROWS(meta->warning_count);
#undef HWA_RUN_ADD_ROWS
    return rows <= limits->max_result_rows;
}

static int hwa_run_allocate_rows(HWARunResult *result,
                                 const HWARunSavedMeta *meta,
                                 char *error,
                                 size_t error_size)
{
#define HWA_RUN_ALLOC(field, count, type)                                   \
    do {                                                                    \
        result->count = meta->count;                                        \
        if (meta->count != 0U) {                                            \
            if (meta->count > SIZE_MAX / sizeof(type)) goto overflow;       \
            result->field = (type *)calloc(meta->count, sizeof(type));      \
            if (result->field == NULL) goto allocation;                     \
        }                                                                   \
    } while (0)
    HWA_RUN_ALLOC(sources, source_count, HWARunSource);
    HWA_RUN_ALLOC(clocks, clock_count, HWARunClock);
    HWA_RUN_ALLOC(features, feature_count, HWARunFeature);
    HWA_RUN_ALLOC(stages, stage_count, HWARunStage);
    HWA_RUN_ALLOC(probes, probe_count, HWARunProbe);
    HWA_RUN_ALLOC(links, link_count, HWARunLink);
    HWA_RUN_ALLOC(warnings, warning_count, HWARunWarning);
#undef HWA_RUN_ALLOC
    return 0;

overflow:
    hwa_run_file_error(error, error_size, "run result row storage overflows");
    return -1;
allocation:
    hwa_run_file_error(error, error_size, "cannot allocate run result rows");
    return -1;
}

static int hwa_run_expect_row(HWARunCsvReader *reader,
                              const char *tag,
                              size_t field_count,
                              HWARunCsvRow *row,
                              char *error,
                              size_t error_size)
{
    int status = hwa_run_csv_next(reader, row, error, error_size);
    if (status != 1) {
        if (status == 0) hwa_run_file_error(error, error_size,
                                             "run result is incomplete");
        return -1;
    }
    if (row->count != field_count || !hwa_run_row_canonical(row) ||
        strcmp(row->fields[0].text, tag) != 0) {
        hwa_run_csv_row_free(row);
        hwa_run_file_error(error, error_size,
                           "run result has an invalid or out-of-order row");
        return -1;
    }
    return 0;
}

static char *hwa_run_take_text(HWARunCsvRow *row,
                               size_t field,
                               int allow_empty)
{
    char *text;
    if (field >= row->count ||
        (!allow_empty && row->fields[field].text[0] == '\0')) return NULL;
    text = row->fields[field].text;
    row->fields[field].text = NULL;
    return text;
}

static int hwa_run_blank(const HWARunCsvField *field)
{
    return !field->quoted && field->text[0] == '\0';
}

static int hwa_run_parse_source_row(HWARunCsvReader *reader,
                                    const HWANumericLocale *locale,
                                    HWARunSource *source,
                                    char *error,
                                    size_t error_size)
{
    HWARunCsvRow row;
    uint32_t u32;
    if (hwa_run_expect_row(reader, "SOURCE", 28U, &row,
                           error, error_size) != 0) return -1;
    if (hwa_run_parse_u64(&row.fields[1], &source->id) != 0 ||
        (source->binding_id = hwa_run_take_text(&row, 2U, 0)) == NULL ||
        (source->path = hwa_run_decode_path(&row.fields[3])) == NULL ||
        row.fields[4].quoted || !hwa_run_sha_valid(row.fields[4].text) ||
        hwa_run_parse_source_kind(&row.fields[5], &source->kind) != 0 ||
        hwa_run_parse_side(&row.fields[6], &source->side) != 0 ||
        hwa_run_parse_role(&row.fields[7],
                           source->kind == HWA_RUN_SOURCE_PROBE,
                           &source->role) != 0 ||
        hwa_run_parse_probe_format(&row.fields[8],
                                   source->kind == HWA_RUN_SOURCE_STEM,
                                   &source->probe_format) != 0 ||
        hwa_run_parse_i64(&row.fields[11], &source->start_sample) != 0 ||
        hwa_run_parse_double(locale, &row.fields[12], &source->gain_db) != 0 ||
        hwa_run_parse_u64(&row.fields[13], &source->rate_numerator) != 0 ||
        hwa_run_parse_u64(&row.fields[14], &source->rate_denominator) != 0 ||
        hwa_run_parse_u64(&row.fields[15], &source->file_bytes) != 0 ||
        hwa_run_parse_u64(&row.fields[16], &source->value_count) != 0) {
        goto invalid;
    }
    memcpy(source->sha256, row.fields[4].text, HWA_SHA256_HEX_SIZE);
    if (source->kind == HWA_RUN_SOURCE_PROBE) {
        source->probe_name = hwa_run_take_text(&row, 9U, 0);
        source->unit = hwa_run_take_text(&row, 10U, 0);
        if (source->probe_name == NULL || source->unit == NULL ||
            !hwa_run_blank(&row.fields[17]) ||
            !hwa_run_blank(&row.fields[18]) ||
            !hwa_run_blank(&row.fields[19]) ||
            !hwa_run_blank(&row.fields[20]) ||
            !hwa_run_blank(&row.fields[21]) ||
            !hwa_run_blank(&row.fields[22]) ||
            !hwa_run_blank(&row.fields[23]) ||
            !hwa_run_blank(&row.fields[24]) ||
            !hwa_run_blank(&row.fields[25]) ||
            !hwa_run_blank(&row.fields[26]) ||
            !hwa_run_blank(&row.fields[27])) goto invalid;
    } else {
        if (!hwa_run_blank(&row.fields[9]) ||
            !hwa_run_blank(&row.fields[10]) ||
            hwa_run_parse_container(&row.fields[17],
                                    &source->format.container) != 0 ||
            hwa_run_parse_encoding(&row.fields[18],
                                   &source->format.encoding) != 0 ||
            hwa_run_parse_u16(&row.fields[19],
                              &source->format.channels) != 0 ||
            hwa_run_parse_u32(&row.fields[20],
                              &source->format.sample_rate_hz) != 0 ||
            hwa_run_parse_u16(&row.fields[21],
                              &source->format.bits_per_sample) != 0 ||
            hwa_run_parse_u16(&row.fields[22],
                              &source->format.valid_bits_per_sample) != 0 ||
            hwa_run_parse_u16(&row.fields[23],
                              &source->format.block_align) != 0 ||
            hwa_run_parse_u32(&row.fields[24], &u32) != 0 ||
            hwa_run_parse_u64(&row.fields[25], &source->format.frames) != 0 ||
            hwa_run_parse_u64(&row.fields[26],
                              &source->format.data_bytes) != 0 ||
            hwa_run_parse_double(locale, &row.fields[27],
                                 &source->format.duration_seconds) != 0) {
            goto invalid;
        }
        source->format.channel_mask = u32;
    }
    hwa_run_csv_row_free(&row);
    return 0;

invalid:
    hwa_run_csv_row_free(&row);
    hwa_run_file_error(error, error_size, "invalid Stage 7 SOURCE row");
    return -1;
}

static int hwa_run_parse_clock_row(HWARunCsvReader *reader,
                                   const HWANumericLocale *locale,
                                   HWARunClock *clock,
                                   char *error,
                                   size_t error_size)
{
    HWARunCsvRow row;
    if (hwa_run_expect_row(reader, "CLOCK", 12U, &row,
                           error, error_size) != 0) return -1;
    if (hwa_run_parse_u64(&row.fields[1], &clock->id) != 0 ||
        hwa_run_parse_role(&row.fields[2], 0, &clock->role) != 0 ||
        hwa_run_parse_u64(&row.fields[3], &clock->reference_source_id) != 0 ||
        hwa_run_parse_u64(&row.fields[4], &clock->model_source_id) != 0 ||
        hwa_run_parse_availability(&row.fields[5], &clock->availability) != 0 ||
        hwa_run_parse_i64(&row.fields[6], &clock->start_offset_samples) != 0 ||
        hwa_run_parse_i64(&row.fields[7], &clock->end_offset_samples) != 0 ||
        hwa_run_parse_i64(&row.fields[8], &clock->drift_samples) != 0 ||
        hwa_run_parse_u64(&row.fields[9], &clock->overlap_frames) != 0 ||
        hwa_run_parse_double(locale, &row.fields[10], &clock->drift_ppm) != 0 ||
        hwa_run_parse_u32(&row.fields[11], &clock->quality_flags) != 0 ||
        (clock->quality_flags & ~HWA_RUN_QUALITY_ALL) != 0U) goto invalid;
    hwa_run_csv_row_free(&row);
    return 0;
invalid:
    hwa_run_csv_row_free(&row);
    hwa_run_file_error(error, error_size, "invalid Stage 7 CLOCK row");
    return -1;
}

static int hwa_run_parse_feature_row(HWARunCsvReader *reader,
                                     const HWANumericLocale *locale,
                                     HWARunFeature *feature,
                                     char *error,
                                     size_t error_size)
{
    HWARunCsvRow row;
    if (hwa_run_expect_row(reader, "FEATURE", 17U, &row,
                           error, error_size) != 0) return -1;
    if (hwa_run_parse_bool(&row.fields[13], &feature->reference_valid) != 0 ||
        hwa_run_parse_bool(&row.fields[14], &feature->model_valid) != 0 ||
        hwa_run_parse_bool(&row.fields[15], &feature->delta_valid) != 0 ||
        hwa_run_parse_bool(&row.fields[16], &feature->gap_valid) != 0 ||
        hwa_run_parse_u64(&row.fields[1], &feature->id) != 0 ||
        hwa_run_parse_u64(&row.fields[2], &feature->clock_id) != 0 ||
        hwa_run_parse_role(&row.fields[3], 0, &feature->role) != 0 ||
        hwa_run_parse_feature_kind(&row.fields[4], &feature->kind) != 0 ||
        hwa_run_parse_u32(&row.fields[5], &feature->index) != 0 ||
        hwa_run_parse_unit(&row.fields[6], &feature->unit) != 0 ||
        hwa_run_parse_availability(&row.fields[7],
                                   &feature->availability) != 0 ||
        hwa_run_parse_optional_double(locale, &row.fields[8],
                                      feature->reference_valid,
                                      &feature->reference_value) != 0 ||
        hwa_run_parse_optional_double(locale, &row.fields[9],
                                      feature->model_valid,
                                      &feature->model_value) != 0 ||
        hwa_run_parse_optional_double(locale, &row.fields[10],
                                      feature->delta_valid,
                                      &feature->delta) != 0 ||
        hwa_run_parse_optional_double(locale, &row.fields[11],
                                      feature->gap_valid,
                                      &feature->normalized_gap) != 0 ||
        hwa_run_parse_u32(&row.fields[12], &feature->quality_flags) != 0 ||
        (feature->quality_flags & ~HWA_RUN_QUALITY_ALL) != 0U) goto invalid;
    hwa_run_csv_row_free(&row);
    return 0;
invalid:
    hwa_run_csv_row_free(&row);
    hwa_run_file_error(error, error_size, "invalid Stage 7 FEATURE row");
    return -1;
}

static int hwa_run_parse_stage_row(HWARunCsvReader *reader,
                                   const HWANumericLocale *locale,
                                   HWARunStage *stage,
                                   char *error,
                                   size_t error_size)
{
    HWARunCsvRow row;
    if (hwa_run_expect_row(reader, "STAGE", 11U, &row,
                           error, error_size) != 0) return -1;
    if (hwa_run_parse_bool(&row.fields[10], &stage->gap_valid) != 0 ||
        hwa_run_parse_u64(&row.fields[1], &stage->id) != 0 ||
        hwa_run_parse_role(&row.fields[2], 0, &stage->from_role) != 0 ||
        hwa_run_parse_role(&row.fields[3], 0, &stage->to_role) != 0 ||
        hwa_run_parse_availability(&row.fields[4], &stage->availability) != 0 ||
        hwa_run_parse_optional_double(locale, &row.fields[5], stage->gap_valid,
                                      &stage->prior_gap) != 0 ||
        hwa_run_parse_optional_double(locale, &row.fields[6], stage->gap_valid,
                                      &stage->current_gap) != 0 ||
        hwa_run_parse_optional_double(locale, &row.fields[7], stage->gap_valid,
                                      &stage->added_gap) != 0 ||
        hwa_run_parse_size(&row.fields[8], &stage->rank) != 0 ||
        hwa_run_parse_u32(&row.fields[9], &stage->quality_flags) != 0 ||
        (stage->quality_flags & ~HWA_RUN_QUALITY_ALL) != 0U) goto invalid;
    hwa_run_csv_row_free(&row);
    return 0;
invalid:
    hwa_run_csv_row_free(&row);
    hwa_run_file_error(error, error_size, "invalid Stage 7 STAGE row");
    return -1;
}

static int hwa_run_parse_probe_row(HWARunCsvReader *reader,
                                   const HWANumericLocale *locale,
                                   HWARunProbe *probe,
                                   char *error,
                                   size_t error_size)
{
    HWARunCsvRow row;
    if (hwa_run_expect_row(reader, "PROBE", 10U, &row,
                           error, error_size) != 0) return -1;
    if (hwa_run_parse_bool(&row.fields[9], &probe->statistics_valid) != 0 ||
        hwa_run_parse_u64(&row.fields[1], &probe->id) != 0 ||
        hwa_run_parse_u64(&row.fields[2], &probe->source_id) != 0 ||
        hwa_run_parse_availability(&row.fields[3], &probe->availability) != 0 ||
        hwa_run_parse_u64(&row.fields[4], &probe->value_count) != 0 ||
        hwa_run_parse_optional_double(locale, &row.fields[5],
                                      probe->statistics_valid,
                                      &probe->minimum) != 0 ||
        hwa_run_parse_optional_double(locale, &row.fields[6],
                                      probe->statistics_valid,
                                      &probe->maximum) != 0 ||
        hwa_run_parse_optional_double(locale, &row.fields[7],
                                      probe->statistics_valid,
                                      &probe->mean) != 0 ||
        hwa_run_parse_optional_double(locale, &row.fields[8],
                                      probe->statistics_valid,
                                      &probe->population_sd) != 0) goto invalid;
    hwa_run_csv_row_free(&row);
    return 0;
invalid:
    hwa_run_csv_row_free(&row);
    hwa_run_file_error(error, error_size, "invalid Stage 7 PROBE row");
    return -1;
}

static int hwa_run_parse_link_row(HWARunCsvReader *reader,
                                  const HWANumericLocale *locale,
                                  HWARunLink *link,
                                  char *error,
                                  size_t error_size)
{
    HWARunCsvRow row;
    if (hwa_run_expect_row(reader, "LINK", 17U, &row,
                           error, error_size) != 0) return -1;
    if (hwa_run_parse_bool(&row.fields[16], &link->fit_valid) != 0 ||
        hwa_run_parse_u64(&row.fields[1], &link->id) != 0 ||
        hwa_run_parse_u64(&row.fields[2], &link->stem_source_id) != 0 ||
        hwa_run_parse_u64(&row.fields[3], &link->probe_source_id) != 0 ||
        hwa_run_parse_feature_kind(&row.fields[4], &link->feature) != 0 ||
        hwa_run_parse_u32(&row.fields[5], &link->feature_index) != 0 ||
        hwa_run_parse_availability(&row.fields[6], &link->availability) != 0 ||
        hwa_run_parse_i64(&row.fields[7], &link->lag_hops) != 0 ||
        hwa_run_parse_i64(&row.fields[8], &link->lag_samples) != 0 ||
        hwa_run_parse_optional_double(locale, &row.fields[9], link->fit_valid,
                                      &link->correlation) != 0 ||
        hwa_run_parse_optional_double(locale, &row.fields[10], link->fit_valid,
                                      &link->slope) != 0 ||
        hwa_run_parse_optional_double(locale, &row.fields[11], link->fit_valid,
                                      &link->intercept) != 0 ||
        hwa_run_parse_optional_double(locale, &row.fields[12], link->fit_valid,
                                      &link->r_squared) != 0 ||
        hwa_run_parse_size(&row.fields[13], &link->point_count) != 0 ||
        hwa_run_parse_double(locale, &row.fields[14], &link->coverage) != 0 ||
        hwa_run_parse_u32(&row.fields[15], &link->quality_flags) != 0 ||
        (link->quality_flags & ~HWA_RUN_QUALITY_ALL) != 0U) goto invalid;
    hwa_run_csv_row_free(&row);
    return 0;
invalid:
    hwa_run_csv_row_free(&row);
    hwa_run_file_error(error, error_size, "invalid Stage 7 LINK row");
    return -1;
}

static int hwa_run_parse_warning_row(HWARunCsvReader *reader,
                                     HWARunWarning *warning,
                                     char *error,
                                     size_t error_size)
{
    HWARunCsvRow row;
    if (hwa_run_expect_row(reader, "WARNING", 12U, &row,
                           error, error_size) != 0) return -1;
    if (hwa_run_parse_bool(&row.fields[8], &warning->source_id_valid) != 0 ||
        hwa_run_parse_bool(&row.fields[9], &warning->clock_id_valid) != 0 ||
        hwa_run_parse_bool(&row.fields[10], &warning->stage_id_valid) != 0 ||
        hwa_run_parse_bool(&row.fields[11], &warning->link_id_valid) != 0 ||
        hwa_run_parse_u64(&row.fields[1], &warning->id) != 0 ||
        (warning->code = hwa_run_take_text(&row, 2U, 0)) == NULL ||
        (warning->message = hwa_run_take_text(&row, 3U, 0)) == NULL ||
        (warning->source_id_valid
             ? hwa_run_parse_u64(&row.fields[4], &warning->source_id) != 0
             : !hwa_run_blank(&row.fields[4])) ||
        (warning->clock_id_valid
             ? hwa_run_parse_u64(&row.fields[5], &warning->clock_id) != 0
             : !hwa_run_blank(&row.fields[5])) ||
        (warning->stage_id_valid
             ? hwa_run_parse_u64(&row.fields[6], &warning->stage_id) != 0
             : !hwa_run_blank(&row.fields[6])) ||
        (warning->link_id_valid
             ? hwa_run_parse_u64(&row.fields[7], &warning->link_id) != 0
             : !hwa_run_blank(&row.fields[7]))) goto invalid;
    hwa_run_csv_row_free(&row);
    return 0;
invalid:
    hwa_run_csv_row_free(&row);
    hwa_run_file_error(error, error_size, "invalid Stage 7 WARNING row");
    return -1;
}

static int hwa_run_double_near(double left, double right, uint64_t max_ulps)
{
    uint64_t step;
    double cursor = left;
    if (!isfinite(left) || !isfinite(right)) return 0;
    if (left == right) return 1;
    for (step = 0U; step < max_ulps; ++step) {
        cursor = nextafter(cursor, right);
        if (cursor == right) return 1;
        if (!isfinite(cursor)) return 0;
    }
    return 0;
}

static int hwa_run_normalize_derived_rows(HWARunResult *result,
                                          char *error,
                                          size_t error_size)
{
    size_t index;
    for (index = 0U; index < result->clock_count; ++index) {
        HWARunClock *clock = &result->clocks[index];
        int64_t start_offset_samples;
        int64_t end_offset_samples;
        int64_t drift_samples;
        uint64_t overlap_frames;
        double drift_ppm;
        uint32_t quality_flags;
        if (hwa_run_clock_derived_expected(
                result, clock, &start_offset_samples, &end_offset_samples,
                &drift_samples, &overlap_frames, &drift_ppm,
                &quality_flags) != 0 ||
            clock->start_offset_samples != start_offset_samples ||
            clock->end_offset_samples != end_offset_samples ||
            clock->drift_samples != drift_samples ||
            clock->overlap_frames != overlap_frames ||
            clock->quality_flags != quality_flags ||
            !hwa_run_double_near(clock->drift_ppm, drift_ppm,
                                 HWA_RUN_READER_MAX_ULPS)) {
            hwa_run_file_error(error, error_size,
                               "run result has invalid derived CLOCK rows");
            return -1;
        }
        clock->drift_ppm = drift_ppm;
    }
    for (index = 0U; index < result->feature_count; ++index) {
        HWARunFeature *feature = &result->features[index];
        double delta = 0.0;
        double normalized_gap = 0.0;
        int valid = feature->delta_valid && feature->gap_valid;
        if ((feature->delta_valid != feature->gap_valid) ||
            (valid &&
             (hwa_run_feature_derived_expected(
                  feature, &delta, &normalized_gap) != 0 ||
              !hwa_run_double_near(feature->delta, delta,
                                   HWA_RUN_READER_MAX_ULPS) ||
              !hwa_run_double_near(feature->normalized_gap, normalized_gap,
                                   HWA_RUN_READER_MAX_ULPS))) ||
            (!valid && (feature->delta != 0.0 ||
                        feature->normalized_gap != 0.0))) {
            hwa_run_file_error(error, error_size,
                               "run result has invalid derived FEATURE rows");
            return -1;
        }
        feature->delta = delta;
        feature->normalized_gap = normalized_gap;
    }
    for (index = 0U; index < result->link_count; ++index) {
        HWARunLink *link = &result->links[index];
        int64_t lag_samples;
        double r_squared;
        double coverage;
        uint32_t quality_flags;
        if (hwa_run_link_derived_expected(
                result, link, &lag_samples, &r_squared,
                &coverage,
                &quality_flags) != 0 ||
            link->lag_samples != lag_samples ||
            link->quality_flags != quality_flags ||
            !hwa_run_double_near(link->r_squared, r_squared,
                                 HWA_RUN_READER_MAX_ULPS) ||
            !hwa_run_double_near(link->coverage, coverage,
                                 HWA_RUN_READER_MAX_ULPS)) {
            hwa_run_file_error(error, error_size,
                               "run result has invalid derived LINK rows");
            return -1;
        }
        link->r_squared = r_squared;
        link->coverage = coverage;
    }
    return 0;
}

static int hwa_run_normalize_stages(HWARunResult *result,
                                    char *error,
                                    size_t error_size)
{
    HWARunStage saved[3];
    size_t index;
    if (result->stage_count != hwa_run_stage_catalog_count() ||
        result->stage_count != sizeof(saved) / sizeof(saved[0])) {
        hwa_run_file_error(error, error_size,
                           "run result has the wrong STAGE catalog");
        return -1;
    }
    memcpy(saved, result->stages, sizeof(saved));
    if (hwa_run_stage_rows_rebuild(result, error, error_size) != 0) return -1;
    for (index = 0U; index < result->stage_count; ++index) {
        const HWARunStage *expected = &result->stages[index];
        const HWARunStage *actual = &saved[index];
        if (actual->id != expected->id ||
            actual->from_role != expected->from_role ||
            actual->to_role != expected->to_role ||
            actual->availability != expected->availability ||
            actual->rank != expected->rank ||
            actual->quality_flags != expected->quality_flags ||
            actual->gap_valid != expected->gap_valid ||
            (actual->gap_valid &&
             (!hwa_run_double_near(actual->prior_gap, expected->prior_gap,
                                   HWA_RUN_READER_MAX_ULPS) ||
              !hwa_run_double_near(actual->current_gap, expected->current_gap,
                                   HWA_RUN_READER_MAX_ULPS) ||
              !hwa_run_double_near(actual->added_gap, expected->added_gap,
                                   HWA_RUN_READER_MAX_ULPS)))) {
            hwa_run_file_error(error, error_size,
                               "run result has invalid derived STAGE rows");
            return -1;
        }
    }
    return 0;
}

static int hwa_run_warning_matches(const HWARunWarning *warning,
                                   const HWARunWarningSpec *spec)
{
    return warning->code != NULL && warning->message != NULL &&
           strcmp(warning->code, spec->code) == 0 &&
           strcmp(warning->message, spec->message) == 0 &&
           warning->source_id == spec->source_id &&
           warning->clock_id == spec->clock_id &&
           warning->stage_id == spec->stage_id &&
           warning->link_id == spec->link_id &&
           warning->source_id_valid == spec->source_id_valid &&
           warning->clock_id_valid == spec->clock_id_valid &&
           warning->stage_id_valid == spec->stage_id_valid &&
           warning->link_id_valid == spec->link_id_valid;
}

static int hwa_run_normalize_warnings(HWARunResult *result,
                                      char *error,
                                      size_t error_size)
{
    size_t expected = hwa_run_warning_spec_count(result);
    size_t index;
    if (expected != result->warning_count) {
        hwa_run_file_error(error, error_size,
                           "run result has the wrong WARNING catalog");
        return -1;
    }
    for (index = 0U; index < expected; ++index) {
        HWARunWarningSpec spec;
        if (result->warnings[index].id != (uint64_t)index + UINT64_C(1) ||
            hwa_run_warning_spec_at(result, index, &spec) != 0 ||
            !hwa_run_warning_matches(&result->warnings[index], &spec)) {
            hwa_run_file_error(error, error_size,
                               "run result has an invalid WARNING row");
            return -1;
        }
    }
    return hwa_run_warnings_rebuild(result, error, error_size);
}

static int hwa_run_array_work(const HWARunSavedMeta *meta, uint64_t *bytes)
{
    uint64_t total = 0U;
#define HWA_RUN_ARRAY_WORK(count, type)                                     \
    do {                                                                    \
        if ((count) > SIZE_MAX / sizeof(type) ||                            \
            (uint64_t)((count) * sizeof(type)) > UINT64_MAX - total) {      \
            return -1;                                                      \
        }                                                                   \
        total += (uint64_t)((count) * sizeof(type));                        \
    } while (0)
    HWA_RUN_ARRAY_WORK(meta->source_count, HWARunSource);
    HWA_RUN_ARRAY_WORK(meta->clock_count, HWARunClock);
    HWA_RUN_ARRAY_WORK(meta->feature_count, HWARunFeature);
    HWA_RUN_ARRAY_WORK(meta->stage_count, HWARunStage);
    HWA_RUN_ARRAY_WORK(meta->probe_count, HWARunProbe);
    HWA_RUN_ARRAY_WORK(meta->link_count, HWARunLink);
    HWA_RUN_ARRAY_WORK(meta->warning_count, HWARunWarning);
#undef HWA_RUN_ARRAY_WORK
    *bytes = total;
    return 0;
}

static int hwa_run_reader_work_fits(size_t file_size,
                                    const HWARunSavedMeta *meta,
                                    const HWARunOptions *limits)
{
    uint64_t arrays;
    uint64_t file = (uint64_t)file_size + UINT64_C(1);
    uint64_t live;
    if (hwa_run_array_work(meta, &arrays) != 0 || file > UINT64_MAX / 3U) {
        return 0;
    }
    live = file * 3U;
    return arrays <= UINT64_MAX - live &&
           live + arrays <= limits->max_work_bytes;
}

static int hwa_run_rows_canonical(const HWARunResult *result,
                                  const HWARunOptions *limits,
                                  char *error,
                                  size_t error_size)
{
    size_t index;
    size_t stem_count = 0U;
    size_t probe_source_count = 0U;
    for (index = 0U; index < result->source_count; ++index) {
        if (result->sources[index].kind == HWA_RUN_SOURCE_STEM) {
            stem_count++;
            if (result->sources[index].file_bytes > limits->max_input_bytes ||
                result->sources[index].format.data_bytes >
                    limits->max_input_bytes ||
                result->sources[index].format.frames >
                    limits->max_input_frames) goto invalid;
        }
        if (result->sources[index].kind == HWA_RUN_SOURCE_PROBE) {
            probe_source_count++;
            if (result->sources[index].file_bytes > limits->max_probe_bytes ||
                result->sources[index].value_count >
                    limits->max_probe_values) goto invalid;
        }
        if (index != 0U && hwa_run_source_canonical_compare(
                              &result->sources[index - 1U],
                              &result->sources[index]) >= 0) goto invalid;
    }
    if (stem_count > limits->max_stems ||
        probe_source_count > limits->max_probes ||
        probe_source_count != result->probe_count) goto invalid;
#define HWA_RUN_CHECK_ORDER(field, count, compare)                          \
    do {                                                                    \
        for (index = 1U; index < result->count; ++index) {                  \
            if (compare(&result->field[index - 1U],                         \
                        &result->field[index]) >= 0) goto invalid;          \
        }                                                                   \
    } while (0)
    HWA_RUN_CHECK_ORDER(clocks, clock_count, hwa_run_clock_canonical_compare);
    HWA_RUN_CHECK_ORDER(features, feature_count,
                        hwa_run_feature_canonical_compare);
    HWA_RUN_CHECK_ORDER(stages, stage_count, hwa_run_stage_canonical_compare);
    HWA_RUN_CHECK_ORDER(probes, probe_count, hwa_run_probe_canonical_compare);
    HWA_RUN_CHECK_ORDER(links, link_count, hwa_run_link_canonical_compare);
#undef HWA_RUN_CHECK_ORDER
    return 0;
invalid:
    hwa_run_file_error(error, error_size,
                       "run result rows are not in canonical order");
    return -1;
}

static int hwa_run_file_read_impl(
    const char *path,
    const HWARunOptions *limits,
    HWARunResult *result,
    char file_sha256[HWA_SHA256_HEX_SIZE],
    const HWANumericLocale *locale,
    char *error,
    size_t error_size)
{
    HWARunCsvReader reader;
    HWARunCsvRow row;
    HWARunSavedMeta meta;
    unsigned char *data = NULL;
    size_t data_size = 0U;
    uint64_t retained;
    size_t index;
    int status = -1;
    memset(&meta, 0, sizeof(meta));
    if (result == NULL) {
        hwa_run_file_error(error, error_size, "run result pointer is null");
        return -1;
    }
    memset(result, 0, sizeof(*result));
    if (file_sha256 != NULL) file_sha256[0] = '\0';
    if (path == NULL || path[0] == '\0' || strcmp(path, "-") == 0 ||
        limits == NULL || file_sha256 == NULL ||
        !hwa_run_options_nonzero(limits) ||
        limits->decode_block_frames > 1048576U) {
        hwa_run_file_error(error, error_size,
                           "invalid run result reader arguments");
        return -1;
    }
    if (hwa_run_read_bound_file(path, limits->max_input_bytes,
                                limits->max_work_bytes,
                                &data, &data_size, file_sha256,
                                error, error_size) != 0) return -1;
    if ((uint64_t)data_size + UINT64_C(1) > UINT64_MAX / 3U ||
        ((uint64_t)data_size + UINT64_C(1)) * 3U >
            limits->max_work_bytes) {
        hwa_run_file_error(error, error_size,
                           "run result reader exceeds the current work limit");
        goto cleanup;
    }
    memset(&reader, 0, sizeof(reader));
    reader.data = data;
    reader.size = data_size;
    if (hwa_run_csv_next(&reader, &row, error, error_size) != 1 ||
        row.count != 2U || !hwa_run_row_canonical(&row) ||
        strcmp(row.fields[0].text, "HWA_RUN") != 0 ||
        strcmp(row.fields[1].text, "1") != 0) {
        hwa_run_csv_row_free(&row);
        hwa_run_file_error(error, error_size,
                           "run result has an invalid header");
        goto cleanup;
    }
    hwa_run_csv_row_free(&row);
    if (hwa_run_read_meta(&reader, locale, &meta,
                          error, error_size) != 0 ||
        !hwa_run_counts_fit_limits(&meta, limits) ||
        !hwa_run_counts_fit_limits(&meta, &meta.options) ||
        meta.clock_rate_hz > 768000U ||
        meta.stage_count != hwa_run_stage_catalog_count() ||
        (meta.clock_count != 0U &&
         hwa_run_feature_catalog_count() >
             SIZE_MAX / meta.clock_count) ||
        meta.feature_count !=
            meta.clock_count * hwa_run_feature_catalog_count() ||
        !hwa_run_reader_work_fits(data_size, &meta, limits)) {
        if (error == NULL || error_size == 0U || error[0] == '\0') {
            hwa_run_file_error(error, error_size,
                               "run result exceeds current caps or has bad counts");
        }
        goto cleanup;
    }
    result->options = *limits;
    result->manifest_path = meta.manifest_path;
    meta.manifest_path = NULL;
    memcpy(result->manifest_sha256, meta.manifest_sha256,
           HWA_SHA256_HEX_SIZE);
    result->clock_rate_hz = meta.clock_rate_hz;
    result->evaluation_count = meta.evaluation_count;
    if (hwa_run_allocate_rows(result, &meta, error, error_size) != 0) {
        goto cleanup;
    }
    for (index = 0U; index < result->source_count; ++index) {
        if (hwa_run_parse_source_row(&reader, locale, &result->sources[index],
                                     error, error_size) != 0) goto cleanup;
    }
    for (index = 0U; index < result->clock_count; ++index) {
        if (hwa_run_parse_clock_row(&reader, locale, &result->clocks[index],
                                    error, error_size) != 0) goto cleanup;
    }
    for (index = 0U; index < result->feature_count; ++index) {
        if (hwa_run_parse_feature_row(&reader, locale, &result->features[index],
                                      error, error_size) != 0) goto cleanup;
    }
    for (index = 0U; index < result->stage_count; ++index) {
        if (hwa_run_parse_stage_row(&reader, locale, &result->stages[index],
                                    error, error_size) != 0) goto cleanup;
    }
    for (index = 0U; index < result->probe_count; ++index) {
        if (hwa_run_parse_probe_row(&reader, locale, &result->probes[index],
                                    error, error_size) != 0) goto cleanup;
    }
    for (index = 0U; index < result->link_count; ++index) {
        if (hwa_run_parse_link_row(&reader, locale, &result->links[index],
                                   error, error_size) != 0) goto cleanup;
    }
    for (index = 0U; index < result->warning_count; ++index) {
        if (hwa_run_parse_warning_row(&reader, &result->warnings[index],
                                      error, error_size) != 0) goto cleanup;
    }
    if (reader.cursor != reader.size ||
        hwa_run_rows_canonical(result, &meta.options,
                               error, error_size) != 0 ||
        hwa_run_rows_canonical(result, limits, error, error_size) != 0 ||
        hwa_run_normalize_derived_rows(result, error, error_size) != 0 ||
        hwa_run_normalize_stages(result, error, error_size) != 0 ||
        hwa_run_normalize_warnings(result, error, error_size) != 0 ||
        hwa_run_result_retained_bytes(result, &retained) != 0 ||
        retained > limits->max_work_bytes ||
        (uint64_t)data_size + UINT64_C(1) >
            limits->max_work_bytes - retained) {
        if (error == NULL || error_size == 0U || error[0] == '\0') {
            hwa_run_file_error(error, error_size,
                               "invalid or over-limit run result");
        }
        goto cleanup;
    }
    result->retained_work_bytes = retained;
    if (hwa_run_result_validate(result, error, error_size) != 0) goto cleanup;
    status = 0;

cleanup:
    free(meta.manifest_path);
    free(data);
    if (status != 0) {
        hwa_run_result_free(result);
        if (file_sha256 != NULL) file_sha256[0] = '\0';
    }
    return status;
}

int hwa_run_file_read_locale(
    const char *path,
    const HWARunOptions *limits,
    HWARunResult *result,
    char file_sha256[HWA_SHA256_HEX_SIZE],
    const HWANumericLocale *locale,
    char *error,
    size_t error_size)
{
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (locale == NULL || !locale->active) {
        if (result != NULL) memset(result, 0, sizeof(*result));
        if (file_sha256 != NULL) file_sha256[0] = '\0';
        hwa_run_file_error(error, error_size,
                           "run input needs an active C numeric locale");
        return -1;
    }
    return hwa_run_file_read_impl(path, limits, result, file_sha256,
                                  locale, error, error_size);
}

int hwa_run_file_read(const char *path,
                      const HWARunOptions *limits,
                      HWARunResult *result,
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
        hwa_run_file_error(error, error_size,
                           "cannot enter the C numeric locale for run input");
        return -1;
    }
    status = hwa_run_file_read_locale(path, limits, result, file_sha256,
                                      &locale, error, error_size);
    if (hwa_c_numeric_locale_end(&locale) != 0) {
        if (status == 0 && result != NULL) hwa_run_result_free(result);
        if (file_sha256 != NULL) file_sha256[0] = '\0';
        if (status == 0) {
            hwa_run_file_error(error, error_size,
                               "cannot restore the numeric locale after run input");
        }
        return -1;
    }
    return status;
}

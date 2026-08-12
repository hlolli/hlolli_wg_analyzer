#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "production_file.h"

#include "alignment_file.h"
#include "internal.h"
#include "measure_compare.h"
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
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define HWA_PRODUCTION_FILE_MAX_FIELDS 48U
#define HWA_PRODUCTION_FILE_MAX_FIELD_BYTES 65536U
#define HWA_PRODUCTION_HASH_BUFFER_BYTES 65536U
#define HWA_PRODUCTION_READER_MAX_ULPS 32U
#define HWA_PRODUCTION_SPAN_FLAGS_ALL ((UINT32_C(1) << 4) - 1U)
#define HWA_PRODUCTION_EVIDENCE_ALL ((UINT32_C(1) << 7) - 1U)
#define HWA_PRODUCTION_QUALITY_ALL ((UINT32_C(1) << 6) - 1U)
typedef struct HWAProductionMetaKey {
    const char *key;
    const char *unit;
} HWAProductionMetaKey;

typedef struct HWAProductionFileIdentity {
    uint64_t device;
    uint64_t file;
    uint64_t size;
} HWAProductionFileIdentity;

static const HWAProductionMetaKey hwa_production_meta_keys[] = {
    {"tool_version", ""},
    {"production_method_version", ""},
    {"source_measurement_method_version", ""},
    {"profile_fft_size", "samples"},
    {"profile_hop_size", "samples"},
    {"profile_pitch_confidence_floor", "ratio"},
    {"profile_spectral_floor_dbfs", "dBFS"},
    {"profile_max_partials", "partials"},
    {"build_compiler_family", ""},
    {"build_compiler_version", ""},
    {"build_c_standard", ""},
    {"build_target_os", ""},
    {"build_pointer_bits", "bits"},
    {"build_endianness", ""},
    {"build_mode", ""},
    {"decode_block_frames", "frames"},
    {"max_input_bytes", "bytes"},
    {"max_input_frames", "frames"},
    {"max_ir_frames", "frames"},
    {"max_work_bytes", "bytes"},
    {"max_evaluations", "evaluations"},
    {"max_spans", "spans"},
    {"max_envelope_points", "points"},
    {"max_fits", "fits"},
    {"max_evaluation_rows", "rows"},
    {"max_view_rows", "rows"},
    {"max_warnings", "warnings"},
    {"profile_max_input_bytes", "bytes"},
    {"profile_max_work_bytes", "bytes"},
    {"profile_max_contexts", "contexts"},
    {"profile_max_measurements", "measurements"},
    {"profile_max_groups", "groups"},
    {"profile_max_group_members", "members"},
    {"profile_max_statistics", "statistics"},
    {"profile_max_warnings", "warnings"},
    {"profile_max_distributions", "distributions"},
    {"profile_max_gaps", "gaps"},
    {"retained_work_bytes", "bytes"},
    {"evaluation_count", "evaluations"},
    {"source_count", "sources"},
    {"span_count", "spans"},
    {"fit_count", "fits"},
    {"evaluation_row_count", "rows"},
    {"view_row_count", "rows"},
    {"warning_count", "warnings"}
};

static void hwa_production_error(char *error,
                                 size_t error_size,
                                 const char *message)
{
    if (error != NULL && error_size > 0U) {
        (void)snprintf(error, error_size, "%s", message);
    }
}

#if defined(_WIN32)
static void hwa_production_windows_identity_from_information(
    const BY_HANDLE_FILE_INFORMATION *information,
    HWAProductionFileIdentity *identity)
{
    identity->device = (uint64_t)information->dwVolumeSerialNumber;
    identity->file = ((uint64_t)information->nFileIndexHigh << 32U) |
                     (uint64_t)information->nFileIndexLow;
    identity->size = ((uint64_t)information->nFileSizeHigh << 32U) |
                     (uint64_t)information->nFileSizeLow;
}

static int hwa_production_path_identity(
    const char *path,
    HWAProductionFileIdentity *identity,
    char *error,
    size_t error_size)
{
    BY_HANDLE_FILE_INFORMATION information;
    HANDLE handle = CreateFileA(
        path, 0U, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(handle, &information)) {
        DWORD windows_error = GetLastError();
        if (handle != INVALID_HANDLE_VALUE) (void)CloseHandle(handle);
        hwa_set_error(error, error_size,
                      "cannot inspect production result: Windows error %lu",
                      (unsigned long)windows_error);
        return -1;
    }
    (void)CloseHandle(handle);
    if ((information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        hwa_production_error(error, error_size,
                             "production result is not a regular file");
        return -1;
    }
    hwa_production_windows_identity_from_information(&information, identity);
    return 0;
}

static int hwa_production_stream_identity(
    FILE *stream,
    HWAProductionFileIdentity *identity)
{
    BY_HANDLE_FILE_INFORMATION information;
    int descriptor = _fileno(stream);
    intptr_t raw_handle = descriptor >= 0
        ? _get_osfhandle(descriptor) : (intptr_t)-1;
    if (raw_handle == (intptr_t)-1 ||
        !GetFileInformationByHandle((HANDLE)raw_handle, &information) ||
        (information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        return -1;
    }
    hwa_production_windows_identity_from_information(&information, identity);
    return 0;
}
#else
static int hwa_production_path_identity(
    const char *path,
    HWAProductionFileIdentity *identity,
    char *error,
    size_t error_size)
{
    struct stat facts;
    if (stat(path, &facts) != 0 || !S_ISREG(facts.st_mode) ||
        facts.st_size < 0) {
        hwa_production_error(error, error_size,
                             "cannot inspect production result");
        return -1;
    }
    identity->device = (uint64_t)facts.st_dev;
    identity->file = (uint64_t)facts.st_ino;
    identity->size = (uint64_t)facts.st_size;
    return 0;
}

static int hwa_production_stream_identity(
    FILE *stream,
    HWAProductionFileIdentity *identity)
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

static int hwa_production_same_identity(
    const HWAProductionFileIdentity *left,
    const HWAProductionFileIdentity *right)
{
    return left->device == right->device && left->file == right->file &&
           left->size == right->size;
}

static int hwa_production_sha256_for_identity(
    const char *path,
    const HWAProductionFileIdentity *expected,
    uint64_t max_bytes,
    char hex[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    FILE *stream = NULL;
    HWAProductionFileIdentity before;
    HWAProductionFileIdentity opened;
    HWAProductionFileIdentity after;
    HWASha256 hash;
    unsigned char buffer[HWA_PRODUCTION_HASH_BUFFER_BYTES];
    unsigned char digest[32];
    uint64_t total = 0U;
    int status = -1;
    if (hwa_production_path_identity(
            path, &before, error, error_size) != 0 ||
        !hwa_production_same_identity(expected, &before) ||
        before.size > max_bytes) {
        hwa_production_error(
            error, error_size,
            "production result changed before final hashing");
        return -1;
    }
    stream = fopen(path, "rb");
    if (stream == NULL) {
        hwa_set_error(error, error_size,
                      "cannot open production result '%s' for hashing: %s",
                      path, strerror(errno));
        return -1;
    }
    if (hwa_production_stream_identity(stream, &opened) != 0 ||
        !hwa_production_same_identity(expected, &opened)) {
        hwa_production_error(
            error, error_size,
            "production result changed before final hashing");
        goto cleanup;
    }
    hwa_sha256_init(&hash);
    for (;;) {
        size_t count = fread(buffer, 1U, sizeof(buffer), stream);
        if ((uint64_t)count > max_bytes - total) {
            hwa_production_error(
                error, error_size,
                "production result exceeds the current byte limit");
            goto cleanup;
        }
        total += (uint64_t)count;
        hwa_sha256_update(&hash, buffer, count);
        if (count < sizeof(buffer)) {
            if (ferror(stream)) {
                hwa_production_error(
                    error, error_size,
                    "cannot hash production result");
                goto cleanup;
            }
            break;
        }
        if (total == max_bytes) {
            int extra = fgetc(stream);
            if (extra != EOF) {
                hwa_production_error(
                    error, error_size,
                    "production result exceeds the current byte limit");
                goto cleanup;
            }
            if (ferror(stream)) {
                hwa_production_error(
                    error, error_size,
                    "cannot hash production result");
                goto cleanup;
            }
            break;
        }
    }
    if (total != expected->size ||
        hwa_production_stream_identity(stream, &after) != 0 ||
        !hwa_production_same_identity(expected, &after) ||
        hash.overflowed) {
        hwa_production_error(
            error, error_size,
            "production result changed while it was hashed");
        goto cleanup;
    }
    hwa_sha256_final(&hash, digest);
    hwa_sha256_hex(digest, hex);
    if (fclose(stream) != 0) {
        stream = NULL;
        hwa_production_error(
            error, error_size,
            "cannot close production result after hashing");
        return -1;
    }
    stream = NULL;
    if (hwa_production_path_identity(
            path, &after, error, error_size) != 0 ||
        !hwa_production_same_identity(expected, &after)) {
        hwa_production_error(
            error, error_size,
            "production result changed after final hashing");
        return -1;
    }
    status = 0;

cleanup:
    if (stream != NULL) (void)fclose(stream);
    return status;
}

static int hwa_production_string_valid(const char *text, int allow_empty)
{
    size_t length;
    if (text == NULL || (!allow_empty && text[0] == '\0')) return 0;
    length = strlen(text);
    return length <= HWA_PRODUCTION_FILE_MAX_FIELD_BYTES;
}

static int hwa_production_path_valid(const char *path)
{
    return hwa_production_string_valid(path, 0) && strcmp(path, "-") != 0;
}

static int hwa_production_sha_valid(const char *text)
{
    size_t index;
    if (text == NULL || strlen(text) != HWA_SHA256_HEX_SIZE - 1U) return 0;
    for (index = 0U; index < HWA_SHA256_HEX_SIZE - 1U; ++index) {
        if (!((text[index] >= '0' && text[index] <= '9') ||
              (text[index] >= 'a' && text[index] <= 'f'))) return 0;
    }
    return 1;
}

static int hwa_production_power_of_two(size_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static int hwa_production_profile_limits_valid(
    const HWAProfileComparisonOptions *limits)
{
    return limits != NULL && limits->max_input_bytes != 0U &&
           limits->max_work_bytes != 0U && limits->max_contexts != 0U &&
           limits->max_measurements != 0U && limits->max_groups != 0U &&
           limits->max_group_members != 0U && limits->max_statistics != 0U &&
           limits->max_warnings != 0U && limits->max_distributions != 0U &&
           limits->max_gaps != 0U;
}

static int hwa_production_options_valid(const HWAProductionOptions *options)
{
    return options != NULL && options->decode_block_frames >= 1U &&
           options->decode_block_frames <= 1048576U &&
           options->max_input_bytes != 0U &&
           options->max_input_frames != 0U && options->max_ir_frames != 0U &&
           options->max_work_bytes != 0U && options->max_evaluations != 0U &&
           options->max_spans != 0U &&
           options->max_envelope_points != 0U && options->max_fits != 0U &&
           options->max_evaluation_rows != 0U &&
           options->max_view_rows != 0U && options->max_warnings != 0U &&
           hwa_production_profile_limits_valid(&options->profile_limits);
}

static int hwa_production_profile_method_valid(
    const HWAProductionProfileMethod *method)
{
    return method != NULL && hwa_production_power_of_two(method->fft_size) &&
           method->fft_size >= 256U && method->fft_size <= 16384U &&
           method->hop_size != 0U && method->hop_size <= method->fft_size &&
           isfinite(method->pitch_confidence_floor) &&
           method->pitch_confidence_floor >= 0.0 &&
           method->pitch_confidence_floor <= 1.0 &&
           isfinite(method->spectral_floor_dbfs) &&
           method->spectral_floor_dbfs >= -300.0 &&
           method->spectral_floor_dbfs <= 0.0 &&
           method->max_partials != 0U && method->max_partials <= 32U;
}

static unsigned hwa_production_bit_count32(uint32_t value)
{
    unsigned count = 0U;
    while (value != 0U) {
        count += value & 1U;
        value >>= 1U;
    }
    return count;
}

static int hwa_production_zero_format(const HWAFormat *format)
{
    static const HWAFormat zero = {0};
    return memcmp(format, &zero, sizeof(*format)) == 0;
}

static int hwa_production_wave_format_valid(const HWAFormat *format,
                                            uint64_t byte_limit,
                                            uint64_t frame_limit)
{
    uint64_t expected_align;
    uint64_t expected_bytes;
    double expected_duration;
    double duration_tolerance;
    int encoding_valid;
    if (format == NULL ||
        (format->container != HWA_CONTAINER_RIFF &&
         format->container != HWA_CONTAINER_RF64) ||
        format->channels == 0U || format->channels > 1024U ||
        format->sample_rate_hz < 8000U || format->sample_rate_hz > 768000U ||
        format->frames > frame_limit || format->data_bytes > byte_limit ||
        !isfinite(format->duration_seconds) ||
        format->duration_seconds < 0.0) return 0;
    encoding_valid =
        (format->encoding == HWA_ENCODING_PCM &&
         (format->bits_per_sample == 8U ||
          format->bits_per_sample == 16U ||
          format->bits_per_sample == 24U ||
          format->bits_per_sample == 32U) &&
         format->valid_bits_per_sample >= 1U &&
         format->valid_bits_per_sample <= format->bits_per_sample) ||
        (format->encoding == HWA_ENCODING_IEEE_FLOAT &&
         (format->bits_per_sample == 32U ||
          format->bits_per_sample == 64U) &&
         format->valid_bits_per_sample == format->bits_per_sample);
    if (!encoding_valid) return 0;
    expected_align = (uint64_t)format->channels *
                     ((uint64_t)format->bits_per_sample / 8U);
    if (expected_align == 0U || expected_align > UINT16_MAX ||
        format->block_align != (uint16_t)expected_align ||
        format->frames > UINT64_MAX / expected_align ||
        (format->channel_mask != 0U &&
         hwa_production_bit_count32(format->channel_mask) !=
             (unsigned)format->channels)) return 0;
    expected_bytes = format->frames * expected_align;
    expected_duration = (double)format->frames /
                        (double)format->sample_rate_hz;
    duration_tolerance = 1e-12 * fmax(1.0, expected_duration);
    return format->data_bytes == expected_bytes &&
           fabs(format->duration_seconds - expected_duration) <=
               duration_tolerance;
}

static int hwa_production_add_bytes(uint64_t *total, uint64_t amount)
{
    if (amount > UINT64_MAX - *total) return -1;
    *total += amount;
    return 0;
}

static int hwa_production_add_array(uint64_t *total,
                                    size_t count,
                                    size_t element_size)
{
    if (element_size != 0U && count > UINT64_MAX / element_size) return -1;
    return hwa_production_add_bytes(
        total, (uint64_t)count * (uint64_t)element_size);
}

static int hwa_production_add_string(uint64_t *total, const char *text)
{
    size_t length;
    if (text == NULL) return -1;
    length = strlen(text);
    return length == SIZE_MAX ||
           hwa_production_add_bytes(total, (uint64_t)length + 1U) != 0
               ? -1 : 0;
}

int hwa_production_result_retained_bytes(
    const HWAProductionResult *result,
    uint64_t *bytes)
{
    uint64_t total = 0U;
    size_t index;
    if (result == NULL || bytes == NULL ||
        (result->source_count != 0U && result->sources == NULL) ||
        (result->span_count != 0U && result->spans == NULL) ||
        (result->fit_count != 0U && result->fits == NULL) ||
        (result->evaluation_row_count != 0U &&
         result->evaluations == NULL) ||
        (result->view_row_count != 0U && result->view_rows == NULL) ||
        (result->warning_count != 0U && result->warnings == NULL) ||
        hwa_production_add_array(&total, result->source_count,
                                 sizeof(*result->sources)) != 0 ||
        hwa_production_add_array(&total, result->span_count,
                                 sizeof(*result->spans)) != 0 ||
        hwa_production_add_array(&total, result->fit_count,
                                 sizeof(*result->fits)) != 0 ||
        hwa_production_add_array(&total, result->evaluation_row_count,
                                 sizeof(*result->evaluations)) != 0 ||
        hwa_production_add_array(&total, result->view_row_count,
                                 sizeof(*result->view_rows)) != 0 ||
        hwa_production_add_array(&total, result->warning_count,
                                 sizeof(*result->warnings)) != 0) return -1;
    for (index = 0U; index < result->source_count; ++index) {
        if (hwa_production_add_string(&total, result->sources[index].role) != 0 ||
            hwa_production_add_string(&total,
                                      result->sources[index].path) != 0) {
            return -1;
        }
    }
    for (index = 0U; index < result->span_count; ++index) {
        if (hwa_production_add_string(&total,
                                      result->spans[index].item_key) != 0 ||
            hwa_production_add_string(&total,
                                      result->spans[index].item_role) != 0) {
            return -1;
        }
    }
    for (index = 0U; index < result->warning_count; ++index) {
        if (hwa_production_add_string(&total,
                                      result->warnings[index].code) != 0 ||
            hwa_production_add_string(&total,
                                      result->warnings[index].message) != 0) {
            return -1;
        }
    }
    *bytes = total;
    return 0;
}

static int hwa_production_double_compare(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static double hwa_production_quantile(const double *values,
                                      size_t count,
                                      double probability)
{
    double position;
    size_t lower;
    size_t upper;
    double fraction;
    if (count == 1U) return values[0];
    position = (double)(count - 1U) * probability;
    lower = (size_t)floor(position);
    upper = (size_t)ceil(position);
    fraction = position - (double)lower;
    return values[lower] + (values[upper] - values[lower]) * fraction;
}

static void hwa_production_calculate_statistics(
    double *values,
    const double *confidences,
    size_t valid_count,
    size_t total_count,
    HWAProductionStatistics *statistics)
{
    size_t index;
    long double sum = 0.0L;
    long double confidence_sum = 0.0L;
    long double squared_sum = 0.0L;
    double mean;
    memset(statistics, 0, sizeof(*statistics));
    statistics->total_count = total_count;
    statistics->valid_count = valid_count;
    statistics->missing_count = total_count - valid_count;
    if (valid_count == 0U) return;
    qsort(values, valid_count, sizeof(*values),
          hwa_production_double_compare);
    for (index = 0U; index < valid_count; ++index) {
        sum += (long double)values[index];
        confidence_sum += (long double)confidences[index];
    }
    mean = (double)(sum / (long double)valid_count);
    for (index = 0U; index < valid_count; ++index) {
        long double difference = (long double)values[index] -
                                 (long double)mean;
        squared_sum += difference * difference;
    }
    statistics->minimum = values[0];
    statistics->q05 = hwa_production_quantile(values, valid_count, 0.05);
    statistics->q25 = hwa_production_quantile(values, valid_count, 0.25);
    statistics->q50 = hwa_production_quantile(values, valid_count, 0.50);
    statistics->q75 = hwa_production_quantile(values, valid_count, 0.75);
    statistics->q95 = hwa_production_quantile(values, valid_count, 0.95);
    statistics->maximum = values[valid_count - 1U];
    statistics->mean = mean == 0.0 ? 0.0 : mean;
    statistics->population_sd =
        sqrt((double)(squared_sum / (long double)valid_count));
    statistics->confidence = total_count == 0U ? 0.0 :
        (double)(confidence_sum / (long double)total_count);
    statistics->valid = 1;
}

static void hwa_production_build_gap(HWAProductionViewRow *row)
{
    const HWAProductionStatistics *reference = &row->reference_statistics;
    const HWAProductionStatistics *model = &row->model_statistics;
    double pooled_variance;
    double pooled_sd;
    double robust_iqr_scale;
    double scale;
    double mean_delta;
    double mean_component;
    double quantile_component;
    double scaled_quantile;
    double reference_coverage;
    double model_coverage;
    double coverage;
    if (!reference->valid || !model->valid) return;
    row->median_delta = model->q50 - reference->q50;
    row->quantile_distance =
        (fabs(model->q05 - reference->q05) +
         fabs(model->q25 - reference->q25) +
         fabs(model->q50 - reference->q50) +
         fabs(model->q75 - reference->q75) +
         fabs(model->q95 - reference->q95)) / 5.0;
    mean_delta = model->mean - reference->mean;
    pooled_variance =
        ((double)reference->valid_count * reference->population_sd *
             reference->population_sd +
         (double)model->valid_count * model->population_sd *
             model->population_sd) /
        (double)(reference->valid_count + model->valid_count);
    pooled_sd = sqrt(pooled_variance);
    if (pooled_sd > 0.0) {
        double standardized = mean_delta / pooled_sd;
        mean_component = fabs(standardized) / (1.0 + fabs(standardized));
    } else {
        mean_component = mean_delta == 0.0 ? 0.0 : 1.0;
    }
    robust_iqr_scale =
        ((reference->q75 - reference->q25) +
         (model->q75 - model->q25)) / (2.0 * 1.349);
    scale = pooled_sd > robust_iqr_scale ? pooled_sd : robust_iqr_scale;
    if (scale > 0.0) {
        scaled_quantile = row->quantile_distance / scale;
        quantile_component = scaled_quantile / (1.0 + scaled_quantile);
    } else {
        quantile_component = row->quantile_distance == 0.0 ? 0.0 : 1.0;
    }
    reference_coverage = (double)reference->valid_count /
                         (double)reference->total_count;
    model_coverage = (double)model->valid_count /
                     (double)model->total_count;
    coverage = reference_coverage < model_coverage ?
                   reference_coverage : model_coverage;
    row->gap_score = coverage *
                     (mean_component + quantile_component) / 2.0;
    row->gap_valid = 1;
}

static int hwa_production_source_rows_valid(
    const HWAProductionResult *result)
{
    static const char *const roles[] = {
        "reference:profile", "reference:audio",
        "model:profile", "model:audio", "room-ir"
    };
    size_t index;
    if (result->source_count < 4U || result->source_count > 5U ||
        result->sources == NULL) return 0;
    for (index = 0U; index < result->source_count; ++index) {
        const HWAProductionSource *source = &result->sources[index];
        int expected_wave = index == 1U || index >= 3U;
        uint64_t frame_limit = index == 4U ?
            result->options.max_ir_frames : result->options.max_input_frames;
        if (source->id != (uint64_t)index + 1U ||
            !hwa_production_string_valid(source->role, 0) ||
            strcmp(source->role, roles[index]) != 0 ||
            !hwa_production_path_valid(source->path) ||
            !hwa_production_sha_valid(source->sha256) ||
            source->is_wave != expected_wave ||
            (expected_wave && !hwa_production_wave_format_valid(
                &source->format, result->options.max_input_bytes,
                frame_limit)) ||
            (!expected_wave && !hwa_production_zero_format(&source->format))) {
            return 0;
        }
    }
    return result->sources[1].format.sample_rate_hz ==
           result->sources[3].format.sample_rate_hz;
}

static int hwa_production_span_compare(const HWAProductionSpan *left,
                                       const HWAProductionSpan *right)
{
    int text;
    if (left->split != right->split) {
        return left->split < right->split ? -1 : 1;
    }
    text = strcmp(left->item_key, right->item_key);
    if (text != 0) return text;
    if (left->item_kind != right->item_kind) {
        return left->item_kind < right->item_kind ? -1 : 1;
    }
    text = strcmp(left->item_role, right->item_role);
    if (text != 0) return text;
    if (left->reference_item_id != right->reference_item_id) {
        return left->reference_item_id < right->reference_item_id ? -1 : 1;
    }
    if (left->model_item_id != right->model_item_id) {
        return left->model_item_id < right->model_item_id ? -1 : 1;
    }
    return 0;
}

static int hwa_production_span_duration_at_least(
    uint64_t start,
    uint64_t end,
    uint32_t rate_hz,
    uint64_t numerator,
    uint64_t denominator)
{
    uint64_t required =
        ((uint64_t)rate_hz * numerator + denominator - 1U) / denominator;
    return end - start >= required;
}

static int hwa_production_span_family_shape_valid(
    const HWAProductionResult *result,
    const HWAProductionSpan *span)
{
    uint32_t flags = span->eligibility_flags;
    uint32_t reference_rate =
        result->sources[1].format.sample_rate_hz;
    uint32_t model_rate = result->sources[3].format.sample_rate_hz;
    int note_or_gesture =
        span->item_kind == HWA_ITEM_NOTE ||
        span->item_kind == HWA_ITEM_GESTURE;
    if ((flags & HWA_PRODUCTION_SPAN_EQ) != 0U &&
        (span->item_kind != HWA_ITEM_BODY ||
         !hwa_production_span_duration_at_least(
             span->reference_start_sample, span->reference_end_sample,
             reference_rate, 1U, 4U) ||
         !hwa_production_span_duration_at_least(
             span->model_start_sample, span->model_end_sample,
             model_rate, 1U, 4U))) return 0;
    if ((flags & HWA_PRODUCTION_SPAN_DYNAMICS) != 0U &&
        (!note_or_gesture ||
         !hwa_production_span_duration_at_least(
             span->reference_start_sample, span->reference_end_sample,
             reference_rate, 1U, 1U) ||
         !hwa_production_span_duration_at_least(
             span->model_start_sample, span->model_end_sample,
             model_rate, 1U, 1U))) return 0;
    if ((flags & HWA_PRODUCTION_SPAN_STEREO) != 0U &&
        (!note_or_gesture ||
         result->sources[1].format.channels < 2U ||
         result->sources[3].format.channels < 2U ||
         !hwa_production_span_duration_at_least(
             span->reference_start_sample, span->reference_end_sample,
             reference_rate, 1U, 1U) ||
         !hwa_production_span_duration_at_least(
             span->model_start_sample, span->model_end_sample,
             model_rate, 1U, 1U))) return 0;
    if ((flags & HWA_PRODUCTION_SPAN_DECAY) != 0U &&
        (span->item_kind != HWA_ITEM_RELEASE &&
         span->item_kind != HWA_ITEM_RESIDUAL_TAIL)) return 0;
    if ((flags & HWA_PRODUCTION_SPAN_DECAY) != 0U &&
        (!hwa_production_span_duration_at_least(
             span->reference_start_sample, span->reference_end_sample,
             reference_rate, 1U, 20U) ||
         !hwa_production_span_duration_at_least(
             span->model_start_sample, span->model_end_sample,
             model_rate, 1U, 20U))) return 0;
    return 1;
}

static int hwa_production_u64_compare(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static int hwa_production_span_rows_valid(
    const HWAProductionResult *result,
    size_t *train_count,
    size_t *check_count)
{
    static const uint32_t families[] = {
        HWA_PRODUCTION_SPAN_EQ,
        HWA_PRODUCTION_SPAN_DYNAMICS,
        HWA_PRODUCTION_SPAN_STEREO,
        HWA_PRODUCTION_SPAN_DECAY
    };
    size_t train_families[4] = {0U, 0U, 0U, 0U};
    size_t check_families[4] = {0U, 0U, 0U, 0U};
    size_t index;
    uint64_t *reference_ids = NULL;
    uint64_t *model_ids = NULL;
    uint64_t retained;
    uint64_t scratch;
    int valid = 0;
    *train_count = 0U;
    *check_count = 0U;
    if (result->span_count == 0U || result->spans == NULL ||
        result->span_count > SIZE_MAX / sizeof(*reference_ids) ||
        hwa_production_result_retained_bytes(result, &retained) != 0) return 0;
    scratch = (uint64_t)result->span_count *
              2U * (uint64_t)sizeof(*reference_ids);
    if (scratch > UINT64_MAX - retained ||
        retained + scratch > result->options.max_work_bytes) return 0;
    reference_ids = (uint64_t *)malloc(
        result->span_count * sizeof(*reference_ids));
    model_ids = (uint64_t *)malloc(result->span_count * sizeof(*model_ids));
    if (reference_ids == NULL || model_ids == NULL) goto cleanup;
    for (index = 0U; index < result->span_count; ++index) {
        const HWAProductionSpan *span = &result->spans[index];
        HWAProductionSplit expected_split;
        if (span->id != (uint64_t)index + 1U ||
            !hwa_production_string_valid(span->item_key, 0) ||
            !hwa_production_string_valid(span->item_role, 0) ||
            hwa_measure_item_kind_name(span->item_kind) == NULL ||
            hwa_production_split_name(span->split) == NULL ||
            span->reference_item_id == 0U || span->model_item_id == 0U ||
            span->reference_start_sample >= span->reference_end_sample ||
            span->model_start_sample >= span->model_end_sample ||
            span->reference_end_sample > result->sources[1].format.frames ||
            span->model_end_sample > result->sources[3].format.frames ||
            span->eligibility_flags == 0U ||
            (span->eligibility_flags & ~HWA_PRODUCTION_SPAN_FLAGS_ALL) != 0U ||
            !hwa_production_span_family_shape_valid(result, span) ||
            hwa_production_split_for_item_key(
                span->item_key, &expected_split) != 0 ||
            span->split != expected_split ||
            (index != 0U &&
             strcmp(result->spans[index - 1U].item_key,
                    span->item_key) == 0) ||
            (index != 0U &&
             hwa_production_span_compare(&result->spans[index - 1U],
                                         span) >= 0)) goto cleanup;
        reference_ids[index] = span->reference_item_id;
        model_ids[index] = span->model_item_id;
        if (span->split == HWA_PRODUCTION_TRAIN) {
            size_t family;
            (*train_count)++;
            for (family = 0U; family < 4U; ++family) {
                if ((span->eligibility_flags & families[family]) != 0U) {
                    train_families[family]++;
                }
            }
        } else {
            size_t family;
            (*check_count)++;
            for (family = 0U; family < 4U; ++family) {
                if ((span->eligibility_flags & families[family]) != 0U) {
                    check_families[family]++;
                }
            }
        }
    }
    qsort(reference_ids, result->span_count, sizeof(*reference_ids),
          hwa_production_u64_compare);
    qsort(model_ids, result->span_count, sizeof(*model_ids),
          hwa_production_u64_compare);
    for (index = 1U; index < result->span_count; ++index) {
        if (reference_ids[index - 1U] == reference_ids[index] ||
            model_ids[index - 1U] == model_ids[index]) goto cleanup;
    }
    for (index = 0U; index < 4U; ++index) {
        if (train_families[index] >
                hwa_production_max_train_spans_per_family() ||
            check_families[index] >
                hwa_production_max_check_spans_per_family()) goto cleanup;
    }
    valid = *train_count != 0U && *check_count != 0U;

cleanup:
    free(reference_ids);
    free(model_ids);
    return valid;
}

static int hwa_production_fit_compare(const HWAProductionFit *left,
                                      const HWAProductionFit *right)
{
    if (left->scope != right->scope) {
        return left->scope < right->scope ? -1 : 1;
    }
    if (left->kind != right->kind) {
        return left->kind < right->kind ? -1 : 1;
    }
    if (left->index != right->index) {
        return left->index < right->index ? -1 : 1;
    }
    return 0;
}

static size_t hwa_production_fit_expected_span_count(
    const HWAProductionResult *result,
    const HWAProductionFit *fit)
{
    size_t index;
    size_t count = 0U;
    uint32_t flag;
    if (fit->scope == HWA_PRODUCTION_SCOPE_ROOM_IR) return 0U;
    flag = hwa_production_fit_eligibility_flag(fit->kind);
    if (flag == 0U) return 0U;
    for (index = 0U; index < result->span_count; ++index) {
        if (result->spans[index].split == HWA_PRODUCTION_TRAIN &&
            (result->spans[index].eligibility_flags & flag) != 0U) count++;
    }
    return count;
}

static int hwa_production_fit_room_band_supported(
    const HWAProductionResult *result,
    const HWAProductionFit *fit)
{
    uint32_t rate_hz;
    if (fit->kind != HWA_PRODUCTION_FIT_EARLY_REFLECTION_DB &&
        fit->kind != HWA_PRODUCTION_FIT_LATE_DECAY_SECONDS) return 1;
    if (fit->scope == HWA_PRODUCTION_SCOPE_REFERENCE) {
        rate_hz = result->sources[1].format.sample_rate_hz;
    } else if (fit->scope == HWA_PRODUCTION_SCOPE_MODEL) {
        rate_hz = result->sources[3].format.sample_rate_hz;
    } else if (fit->scope == HWA_PRODUCTION_SCOPE_ROOM_IR &&
               result->source_count == 5U) {
        rate_hz = result->sources[4].format.sample_rate_hz;
    } else {
        return 0;
    }
    return hwa_production_room_band_supported(fit->index, rate_hz);
}

static int hwa_production_fit_rows_valid(const HWAProductionResult *result)
{
    size_t index;
    int saw_eq_gap = 0;
    if (result->fit_count != hwa_production_fit_catalog_count() ||
        result->fits == NULL) return 0;
    for (index = 0U; index < result->fit_count; ++index) {
        const HWAProductionFit *fit = &result->fits[index];
        HWAProductionScope expected_scope;
        HWAProductionFitKind expected_kind;
        HWAProductionUnit expected_unit;
        uint32_t expected_index;
        size_t expected_spans =
            hwa_production_fit_expected_span_count(result, fit);
        int available = fit->availability == HWA_PRODUCTION_AVAILABLE;
        int room_without_input =
            fit->scope == HWA_PRODUCTION_SCOPE_ROOM_IR &&
            result->source_count == 4U;
        int stereo_shape_mismatch =
            hwa_production_fit_eligibility_flag(fit->kind) ==
                HWA_PRODUCTION_SPAN_STEREO &&
            (result->sources[1].format.channels < 2U ||
             result->sources[3].format.channels < 2U);
        if (fit->id != (uint64_t)index + 1U ||
            hwa_production_fit_catalog_at(
                index, &expected_scope, &expected_kind, &expected_index,
                &expected_unit) != 0 ||
            fit->scope != expected_scope || fit->kind != expected_kind ||
            fit->index != expected_index || fit->unit != expected_unit ||
            hwa_production_scope_name(fit->scope) == NULL ||
            hwa_production_fit_kind_name(fit->kind) == NULL ||
            hwa_production_unit_name(fit->unit) == NULL ||
            !hwa_production_fit_shape_valid(fit->kind, fit->index, fit->unit) ||
            hwa_production_availability_name(fit->availability) == NULL ||
            !isfinite(fit->estimate) || !isfinite(fit->q05) ||
            !isfinite(fit->q95) ||
            (fit->quality_flags & ~HWA_PRODUCTION_QUALITY_ALL) != 0U ||
            fit->point_count > result->options.max_envelope_points ||
            fit->span_count != expected_spans ||
            fit->span_count > hwa_production_max_train_spans_per_family() ||
            (available && fit->point_count == 0U) ||
            (available && stereo_shape_mismatch) ||
            (!hwa_production_fit_room_band_supported(result, fit) &&
             fit->availability != HWA_PRODUCTION_UNAVAILABLE) ||
            (available && fit->scope != HWA_PRODUCTION_SCOPE_ROOM_IR &&
             fit->span_count < hwa_production_minimum_train_spans()) ||
            (fit->scope != HWA_PRODUCTION_SCOPE_ROOM_IR &&
             fit->span_count == 0U &&
             fit->availability != HWA_PRODUCTION_UNAVAILABLE) ||
            (fit->availability == HWA_PRODUCTION_UNAVAILABLE &&
             fit->point_count != 0U) ||
            (room_without_input &&
             (fit->availability != HWA_PRODUCTION_UNAVAILABLE ||
              fit->span_count != 0U || fit->point_count != 0U ||
              (fit->quality_flags &
               HWA_PRODUCTION_QUALITY_IR_SUPPLIED) != 0U)) ||
            fit->estimate_valid != available ||
            (!fit->estimate_valid && fit->estimate != 0.0) ||
            (fit->uncertainty_valid && !fit->estimate_valid) ||
            (available && !fit->uncertainty_valid &&
             fit->kind != HWA_PRODUCTION_FIT_CHANNEL_POLARITY &&
             fit->scope != HWA_PRODUCTION_SCOPE_ROOM_IR) ||
            (!fit->uncertainty_valid &&
             (fit->q05 != 0.0 || fit->q95 != 0.0)) ||
            (fit->uncertainty_valid &&
             (fit->q05 > fit->estimate || fit->estimate > fit->q95)) ||
            (fit->estimate_valid &&
             !hwa_production_fit_value_valid(
                 fit->scope, fit->kind, fit->index, fit->estimate,
                 result->sources[1].format.sample_rate_hz,
                 result->sources[3].format.sample_rate_hz)) ||
            (fit->uncertainty_valid &&
             (!hwa_production_fit_value_valid(
                  fit->scope, fit->kind, fit->index, fit->q05,
                  result->sources[1].format.sample_rate_hz,
                  result->sources[3].format.sample_rate_hz) ||
              !hwa_production_fit_value_valid(
                  fit->scope, fit->kind, fit->index, fit->q95,
                  result->sources[1].format.sample_rate_hz,
                  result->sources[3].format.sample_rate_hz))) ||
            (index != 0U && hwa_production_fit_compare(
                                  &result->fits[index - 1U], fit) >= 0)) {
            return 0;
        }
        if (fit->scope == HWA_PRODUCTION_SCOPE_CORRECTION &&
            fit->kind == HWA_PRODUCTION_FIT_EQ_GAIN_DB) {
            int supported = hwa_production_eq_node_supported(
                fit->index, result->sources[1].format.sample_rate_hz,
                result->sources[3].format.sample_rate_hz);
            if ((!supported &&
                 fit->availability != HWA_PRODUCTION_UNAVAILABLE) ||
                (available && saw_eq_gap)) return 0;
            if (!available) {
                saw_eq_gap = 1;
            } else if (fit->index != 0U) {
                const HWAProductionFit *lower = &result->fits[index - 1U];
                if (!hwa_production_eq_adjacent_valid(
                        lower->estimate, fit->estimate) ||
                    !hwa_production_eq_adjacent_valid(
                        lower->q05, fit->q05) ||
                    !hwa_production_eq_adjacent_valid(
                        lower->q95, fit->q95)) return 0;
            }
        }
    }
    return 1;
}

static int hwa_production_expected_evaluation_count(size_t span_count,
                                                    size_t *count)
{
    size_t metric_count = hwa_production_metric_catalog_count();
    size_t view_count = (size_t)HWA_PRODUCTION_VIEW_COUNT - 1U;
    size_t per_span;
    if (metric_count == 0U || view_count > SIZE_MAX / metric_count) {
        return -1;
    }
    per_span = view_count * metric_count;
    if (span_count > SIZE_MAX / per_span) {
        return -1;
    }
    *count = span_count * per_span;
    return 0;
}

static int hwa_production_metric_is_stereo(
    HWAProductionMetricKind kind)
{
    return kind == HWA_PRODUCTION_METRIC_CHANNEL_BALANCE_DB ||
           kind == HWA_PRODUCTION_METRIC_STEREO_WIDTH_RATIO ||
           kind == HWA_PRODUCTION_METRIC_STEREO_CORRELATION ||
           kind == HWA_PRODUCTION_METRIC_STEREO_DELAY_SAMPLES;
}

static int hwa_production_metric_is_room(
    HWAProductionMetricKind kind)
{
    return kind == HWA_PRODUCTION_METRIC_EARLY_REFLECTION_DB ||
           kind == HWA_PRODUCTION_METRIC_LATE_DECAY_SECONDS;
}

static uint32_t hwa_production_raw_evidence(
    HWAProductionMetricKind kind)
{
    switch (kind) {
    case HWA_PRODUCTION_METRIC_RMS_DBFS:
    case HWA_PRODUCTION_METRIC_CREST_DB:
        return HWA_PRODUCTION_EVIDENCE_SAMPLES;
    case HWA_PRODUCTION_METRIC_BAND_LEVEL_DBFS:
    case HWA_PRODUCTION_METRIC_LEVEL_SPREAD_DB:
        return HWA_PRODUCTION_EVIDENCE_PROFILE;
    case HWA_PRODUCTION_METRIC_CHANNEL_BALANCE_DB:
    case HWA_PRODUCTION_METRIC_STEREO_WIDTH_RATIO:
    case HWA_PRODUCTION_METRIC_STEREO_CORRELATION:
    case HWA_PRODUCTION_METRIC_STEREO_DELAY_SAMPLES:
        return HWA_PRODUCTION_EVIDENCE_SAMPLES |
               HWA_PRODUCTION_EVIDENCE_STEREO;
    case HWA_PRODUCTION_METRIC_EARLY_REFLECTION_DB:
    case HWA_PRODUCTION_METRIC_LATE_DECAY_SECONDS:
        return HWA_PRODUCTION_EVIDENCE_SAMPLES |
               HWA_PRODUCTION_EVIDENCE_ENVELOPE |
               HWA_PRODUCTION_EVIDENCE_SPECTRUM;
    default:
        return 0U;
    }
}

static int hwa_production_double_equal(double left,
                                       double right,
                                       unsigned max_ulps);

static int hwa_production_evaluation_equal(
    const HWAProductionEvaluation *left,
    const HWAProductionEvaluation *right,
    unsigned max_ulps)
{
    return left->id == right->id &&
           left->span_id == right->span_id &&
           left->view == right->view &&
           left->kind == right->kind &&
           left->index == right->index &&
           left->unit == right->unit &&
           left->availability == right->availability &&
           left->evidence_flags == right->evidence_flags &&
           left->quality_flags == right->quality_flags &&
           left->reference_valid == right->reference_valid &&
           left->model_valid == right->model_valid &&
           left->delta_valid == right->delta_valid &&
           hwa_production_double_equal(
               left->reference_value, right->reference_value, max_ulps) &&
           hwa_production_double_equal(
               left->model_value, right->model_value, max_ulps) &&
           hwa_production_double_equal(
               left->delta, right->delta, max_ulps) &&
           hwa_production_double_equal(
               left->confidence, right->confidence, max_ulps);
}

static int hwa_production_evaluation_rows_valid(
    const HWAProductionResult *result,
    unsigned max_ulps)
{
    size_t expected_count;
    size_t span_index;
    size_t metric_count = hwa_production_metric_catalog_count();
    size_t evaluations_per_span =
        ((size_t)HWA_PRODUCTION_VIEW_COUNT - 1U) * metric_count;
    if (hwa_production_expected_evaluation_count(
            result->span_count, &expected_count) != 0 ||
        result->evaluation_row_count != expected_count ||
        (expected_count != 0U && result->evaluations == NULL)) return 0;
    for (span_index = 0U; span_index < result->span_count; ++span_index) {
        HWAProductionView view;
        const HWAProductionSpan *span = &result->spans[span_index];
        for (view = HWA_PRODUCTION_VIEW_RAW;
             view < HWA_PRODUCTION_VIEW_COUNT;
             view = (HWAProductionView)((int)view + 1)) {
            size_t metric_offset;
            for (metric_offset = 0U;
                 metric_offset < metric_count;
                 ++metric_offset) {
                size_t row_index =
                    span_index * evaluations_per_span +
                    ((size_t)view - 1U) * metric_count +
                    metric_offset;
                const HWAProductionEvaluation *row =
                    &result->evaluations[row_index];
                HWAProductionMetricKind kind;
                HWAProductionUnit unit;
                uint32_t metric_index;
                int available = row->availability == HWA_PRODUCTION_AVAILABLE;
                int held_out =
                    (row->evidence_flags &
                     HWA_PRODUCTION_EVIDENCE_HELD_OUT) != 0U;
                int stereo_shape_mismatch =
                    hwa_production_metric_is_stereo(row->kind) &&
                    (result->sources[1].format.channels < 2U ||
                     result->sources[3].format.channels < 2U);
                if (hwa_production_metric_is_room(row->kind) &&
                    !hwa_production_room_band_supported(
                        row->index,
                        result->sources[1].format.sample_rate_hz) &&
                    row->availability != HWA_PRODUCTION_UNAVAILABLE) {
                    return 0;
                }
                if (view != HWA_PRODUCTION_VIEW_RAW) {
                    HWAProductionEvaluation expected;
                    if (hwa_production_evaluation_derive(
                            result, span_index, view, metric_offset,
                            &expected, NULL, 0U) != 0 ||
                        !hwa_production_evaluation_equal(
                            row, &expected, max_ulps)) return 0;
                    continue;
                }
                if (hwa_production_metric_catalog_at(
                        metric_offset, &kind, &metric_index, &unit) != 0 ||
                    row->id != (uint64_t)row_index + 1U ||
                    row->span_id != span->id || row->view != view ||
                    row->kind != kind || row->index != metric_index ||
                    row->unit != unit ||
                    !hwa_production_metric_shape_valid(
                        row->kind, row->index, row->unit) ||
                    hwa_production_availability_name(row->availability) == NULL ||
                    !isfinite(row->reference_value) ||
                    !isfinite(row->model_value) || !isfinite(row->delta) ||
                    !isfinite(row->confidence) || row->confidence < 0.0 ||
                    row->confidence > 1.0 ||
                    (row->evidence_flags & ~HWA_PRODUCTION_EVIDENCE_ALL) != 0U ||
                    (row->quality_flags & ~HWA_PRODUCTION_QUALITY_ALL) != 0U ||
                    (row->evidence_flags &
                     HWA_PRODUCTION_EVIDENCE_ROOM_IR) != 0U ||
                    (row->quality_flags &
                     (HWA_PRODUCTION_QUALITY_IR_SUPPLIED |
                      HWA_PRODUCTION_QUALITY_CORRECTION_INCOMPLETE)) != 0U ||
                    held_out != (span->split == HWA_PRODUCTION_CHECK) ||
                    row->reference_valid != available ||
                    row->model_valid != available ||
                    row->delta_valid != available ||
                    (!hwa_production_raw_metric_applicable(
                         span, row->kind) &&
                     row->availability != HWA_PRODUCTION_UNAVAILABLE) ||
                    (available && stereo_shape_mismatch) ||
                    (available &&
                     (row->evidence_flags &
                      (uint32_t)~(uint32_t)
                          HWA_PRODUCTION_EVIDENCE_HELD_OUT) !=
                         hwa_production_raw_evidence(row->kind)) ||
                    (hwa_production_metric_is_room(row->kind) &&
                     !hwa_production_room_band_supported(
                         row->index,
                         result->sources[1].format.sample_rate_hz) &&
                     row->availability != HWA_PRODUCTION_UNAVAILABLE) ||
                    (!available &&
                     (row->reference_value != 0.0 || row->model_value != 0.0 ||
                      row->delta != 0.0 || row->confidence != 0.0)) ||
                    (available &&
                     (!hwa_production_metric_value_valid(
                          row->kind, row->index, row->reference_value,
                          result->sources[1].format.sample_rate_hz) ||
                      !hwa_production_metric_value_valid(
                          row->kind, row->index, row->model_value,
                          result->sources[3].format.sample_rate_hz))) ||
                    (available &&
                     !hwa_production_double_equal(
                         row->delta,
                         row->model_value - row->reference_value,
                         max_ulps))) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

static int hwa_production_evaluation_rows_normalize(
    HWAProductionResult *result,
    char *error,
    size_t error_size)
{
    size_t metric_count = hwa_production_metric_catalog_count();
    size_t evaluations_per_span =
        ((size_t)HWA_PRODUCTION_VIEW_COUNT - 1U) * metric_count;
    size_t span_index;
    for (span_index = 0U; span_index < result->span_count; ++span_index) {
        HWAProductionView view;
        for (view = HWA_PRODUCTION_VIEW_DRY_LIKE;
             view < HWA_PRODUCTION_VIEW_COUNT;
             view = (HWAProductionView)((int)view + 1)) {
            size_t metric_offset;
            for (metric_offset = 0U; metric_offset < metric_count;
                 ++metric_offset) {
                size_t row_index =
                    span_index * evaluations_per_span +
                    ((size_t)view - 1U) * metric_count + metric_offset;
                HWAProductionEvaluation expected;
                if (hwa_production_evaluation_derive(
                        result, span_index, view, metric_offset,
                        &expected, error, error_size) != 0) return -1;
                result->evaluations[row_index] = expected;
            }
        }
    }
    return 0;
}

static int hwa_production_view_cohort_equal(
    const HWAProductionResult *result,
    HWAProductionSplit split,
    HWAProductionView corrected_view,
    size_t metric_offset,
    size_t metric_count)
{
    size_t evaluations_per_span =
        ((size_t)HWA_PRODUCTION_VIEW_COUNT - 1U) * metric_count;
    size_t span_index;
    for (span_index = 0U; span_index < result->span_count; ++span_index) {
        size_t raw_index;
        size_t corrected_index;
        const HWAProductionEvaluation *raw;
        const HWAProductionEvaluation *corrected;
        int raw_valid;
        int corrected_valid;
        if (result->spans[span_index].split != split) continue;
        raw_index = span_index * evaluations_per_span + metric_offset;
        corrected_index = span_index * evaluations_per_span +
            ((size_t)corrected_view - 1U) * metric_count + metric_offset;
        raw = &result->evaluations[raw_index];
        corrected = &result->evaluations[corrected_index];
        raw_valid = raw->reference_valid && raw->model_valid;
        corrected_valid = corrected->reference_valid &&
                          corrected->model_valid;
        if (raw_valid != corrected_valid) return 0;
    }
    return 1;
}

static int hwa_production_build_view_rows(
    const HWAProductionResult *result,
    HWAProductionViewRow **out_rows,
    size_t *out_count,
    unsigned evaluation_ulps,
    char *error,
    size_t error_size)
{
    HWAProductionViewRow *rows = NULL;
    double *reference_values = NULL;
    double *model_values = NULL;
    double *confidences = NULL;
    size_t metric_count = hwa_production_metric_catalog_count();
    size_t evaluations_per_span =
        ((size_t)HWA_PRODUCTION_VIEW_COUNT - 1U) * metric_count;
    size_t count = ((size_t)HWA_PRODUCTION_SPLIT_COUNT - 1U) *
                   ((size_t)HWA_PRODUCTION_VIEW_COUNT - 1U) *
                   metric_count;
    size_t maximum_split = 0U;
    size_t train_count = 0U;
    size_t check_count = 0U;
    size_t row_index = 0U;
    uint64_t retained_bytes;
    uint64_t scratch_bytes;
    HWAProductionSplit split;
    if (!hwa_production_source_rows_valid(result) ||
        !hwa_production_span_rows_valid(result, &train_count, &check_count) ||
        !hwa_production_fit_rows_valid(result) ||
        !hwa_production_evaluation_rows_valid(
            result, evaluation_ulps)) {
        hwa_production_error(error, error_size,
                             "invalid production rows for view rebuild");
        return -1;
    }
    maximum_split = train_count > check_count ? train_count : check_count;
    if (count > result->options.max_view_rows ||
        count > SIZE_MAX / sizeof(*rows) ||
        maximum_split > SIZE_MAX / sizeof(*reference_values) ||
        hwa_production_result_retained_bytes(result, &retained_bytes) != 0 ||
        hwa_production_add_array(&retained_bytes, count, sizeof(*rows)) != 0) {
        hwa_production_error(error, error_size,
                             "production view rows exceed current limits");
        return -1;
    }
    scratch_bytes = (uint64_t)maximum_split *
                    3U * (uint64_t)sizeof(*reference_values);
    if (scratch_bytes > UINT64_MAX - retained_bytes ||
        retained_bytes + scratch_bytes > result->options.max_work_bytes) {
        hwa_production_error(error, error_size,
                             "production view work exceeds the work-byte limit");
        return -1;
    }
    rows = (HWAProductionViewRow *)calloc(count, sizeof(*rows));
    reference_values = maximum_split == 0U ? NULL :
        (double *)malloc(maximum_split * sizeof(*reference_values));
    model_values = maximum_split == 0U ? NULL :
        (double *)malloc(maximum_split * sizeof(*model_values));
    confidences = maximum_split == 0U ? NULL :
        (double *)malloc(maximum_split * sizeof(*confidences));
    if (rows == NULL || (maximum_split != 0U &&
        (reference_values == NULL || model_values == NULL ||
         confidences == NULL))) {
        hwa_production_error(error, error_size,
                             "cannot allocate production view work");
        free(rows);
        free(reference_values);
        free(model_values);
        free(confidences);
        return -1;
    }
    for (split = HWA_PRODUCTION_TRAIN; split < HWA_PRODUCTION_SPLIT_COUNT;
         split = (HWAProductionSplit)((int)split + 1)) {
        HWAProductionView view;
        size_t split_base = row_index;
        for (view = HWA_PRODUCTION_VIEW_RAW;
             view < HWA_PRODUCTION_VIEW_COUNT;
             view = (HWAProductionView)((int)view + 1)) {
            size_t metric_offset;
            for (metric_offset = 0U;
                 metric_offset < metric_count;
                 ++metric_offset) {
                HWAProductionViewRow *row = &rows[row_index];
                size_t span_index;
                size_t total = 0U;
                size_t valid = 0U;
                int saw_insufficient = 0;
                row->id = (uint64_t)row_index + 1U;
                row->split = split;
                row->view = view;
                if (hwa_production_metric_catalog_at(
                        metric_offset, &row->kind, &row->index,
                        &row->unit) != 0) {
                    free(rows);
                    free(reference_values);
                    free(model_values);
                    free(confidences);
                    return -1;
                }
                for (span_index = 0U;
                     span_index < result->span_count; ++span_index) {
                    size_t evaluation_index;
                    const HWAProductionEvaluation *evaluation;
                    if (result->spans[span_index].split != split ||
                        !hwa_production_raw_metric_applicable(
                            &result->spans[span_index], row->kind)) continue;
                    if (hwa_production_metric_is_room(row->kind) &&
                        (!hwa_production_room_band_supported(
                             row->index,
                             result->sources[1].format.sample_rate_hz) ||
                         !hwa_production_room_band_supported(
                             row->index,
                             result->sources[3].format.sample_rate_hz))) {
                        continue;
                    }
                    evaluation_index =
                        span_index * evaluations_per_span +
                        ((size_t)view - 1U) *
                            metric_count + metric_offset;
                    evaluation = &result->evaluations[evaluation_index];
                    total++;
                    row->quality_flags |= evaluation->quality_flags;
                    if (evaluation->availability ==
                        HWA_PRODUCTION_INSUFFICIENT) saw_insufficient = 1;
                    if (!evaluation->reference_valid ||
                        !evaluation->model_valid) continue;
                    reference_values[valid] = evaluation->reference_value;
                    model_values[valid] = evaluation->model_value;
                    confidences[valid] = evaluation->confidence;
                    valid++;
                }
                hwa_production_calculate_statistics(
                    reference_values, confidences, valid, total,
                    &row->reference_statistics);
                hwa_production_calculate_statistics(
                    model_values, confidences, valid, total,
                    &row->model_statistics);
                row->availability = valid != 0U ? HWA_PRODUCTION_AVAILABLE :
                    saw_insufficient ? HWA_PRODUCTION_INSUFFICIENT :
                                       HWA_PRODUCTION_UNAVAILABLE;
                hwa_production_build_gap(row);
                if (view == HWA_PRODUCTION_VIEW_RAW) {
                    row->raw_gap_score = row->gap_valid ? row->gap_score : 0.0;
                } else {
                    const HWAProductionViewRow *raw =
                        &rows[split_base + metric_offset];
                    row->raw_gap_score = raw->gap_valid ? raw->gap_score : 0.0;
                    row->survives =
                        split == HWA_PRODUCTION_CHECK && raw->gap_valid &&
                        row->gap_valid &&
                        raw->reference_statistics.valid_count >= 2U &&
                        raw->model_statistics.valid_count >= 2U &&
                        row->reference_statistics.valid_count >= 2U &&
                        row->model_statistics.valid_count >= 2U &&
                        hwa_production_view_cohort_equal(
                            result, split, view, metric_offset,
                            metric_count) &&
                        raw->gap_score >= 0.20 &&
                        row->gap_score >= 0.20;
                }
                row_index++;
            }
        }
    }
    free(reference_values);
    free(model_values);
    free(confidences);
    *out_rows = rows;
    *out_count = count;
    return 0;
}

int hwa_production_view_rows_rebuild(HWAProductionResult *result,
                                     char *error,
                                     size_t error_size)
{
    HWAProductionViewRow *rows = NULL;
    size_t count = 0U;
    uint64_t retained;
    if (result == NULL || !hwa_production_options_valid(&result->options)) {
        hwa_production_error(error, error_size,
                             "invalid production view rebuild arguments");
        return -1;
    }
    if (hwa_production_build_view_rows(
            result, &rows, &count, 0U, error, error_size) != 0) return -1;
    free(result->view_rows);
    result->view_rows = rows;
    result->view_row_count = count;
    if (hwa_production_result_retained_bytes(result, &retained) != 0 ||
        retained > result->options.max_work_bytes) {
        free(result->view_rows);
        result->view_rows = NULL;
        result->view_row_count = 0U;
        hwa_production_error(error, error_size,
                             "production views exceed the work-byte limit");
        return -1;
    }
    result->retained_work_bytes = retained;
    return 0;
}

static int hwa_production_double_equal(double left,
                                       double right,
                                       unsigned max_ulps)
{
    unsigned step;
    if (left == right) return 1;
    if (!isfinite(left) || !isfinite(right) ||
        signbit(left) != signbit(right)) return 0;
    for (step = 0U; step < max_ulps; ++step) {
        left = nextafter(left, right);
        if (left == right) return 1;
    }
    return 0;
}

static int hwa_production_statistics_equal(
    const HWAProductionStatistics *left,
    const HWAProductionStatistics *right,
    unsigned max_ulps)
{
    return left->total_count == right->total_count &&
           left->valid_count == right->valid_count &&
           left->missing_count == right->missing_count &&
           left->valid == right->valid &&
           hwa_production_double_equal(left->minimum, right->minimum,
                                       max_ulps) &&
           hwa_production_double_equal(left->q05, right->q05, max_ulps) &&
           hwa_production_double_equal(left->q25, right->q25, max_ulps) &&
           hwa_production_double_equal(left->q50, right->q50, max_ulps) &&
           hwa_production_double_equal(left->q75, right->q75, max_ulps) &&
           hwa_production_double_equal(left->q95, right->q95, max_ulps) &&
           hwa_production_double_equal(left->maximum, right->maximum,
                                       max_ulps) &&
           hwa_production_double_equal(left->mean, right->mean, max_ulps) &&
           hwa_production_double_equal(left->population_sd,
                                       right->population_sd, max_ulps) &&
           hwa_production_double_equal(left->confidence,
                                       right->confidence, max_ulps);
}

static int hwa_production_view_rows_equal(
    const HWAProductionResult *result,
    const HWAProductionViewRow *expected,
    size_t expected_count,
    unsigned max_ulps)
{
    size_t index;
    if (result->view_row_count != expected_count ||
        (expected_count != 0U && result->view_rows == NULL)) return 0;
    for (index = 0U; index < expected_count; ++index) {
        const HWAProductionViewRow *actual = &result->view_rows[index];
        const HWAProductionViewRow *want = &expected[index];
        if (actual->id != want->id || actual->split != want->split ||
            actual->view != want->view || actual->kind != want->kind ||
            actual->index != want->index || actual->unit != want->unit ||
            actual->availability != want->availability ||
            actual->quality_flags != want->quality_flags ||
            actual->survives != want->survives ||
            actual->gap_valid != want->gap_valid ||
            !isfinite(actual->median_delta) ||
            !isfinite(actual->quantile_distance) ||
            !isfinite(actual->gap_score) ||
            !isfinite(actual->raw_gap_score) ||
            !hwa_production_statistics_equal(
                &actual->reference_statistics,
                &want->reference_statistics, max_ulps) ||
            !hwa_production_statistics_equal(
                &actual->model_statistics,
                &want->model_statistics, max_ulps) ||
            !hwa_production_double_equal(actual->median_delta,
                                         want->median_delta, max_ulps) ||
            !hwa_production_double_equal(actual->quantile_distance,
                                         want->quantile_distance, max_ulps) ||
            !hwa_production_double_equal(actual->gap_score,
                                         want->gap_score, max_ulps) ||
            !hwa_production_double_equal(actual->raw_gap_score,
                                         want->raw_gap_score, max_ulps)) {
            return 0;
        }
    }
    return 1;
}

static int hwa_production_warning_rows_valid(
    const HWAProductionResult *result)
{
    size_t expected_count =
        hwa_production_warning_spec_count(result);
    size_t index;
    if (result->warning_count != expected_count ||
        (expected_count != 0U && result->warnings == NULL)) return 0;
    for (index = 0U; index < result->warning_count; ++index) {
        const HWAProductionWarning *warning = &result->warnings[index];
        HWAProductionWarningSpec expected;
        if (warning->id != (uint64_t)index + 1U ||
            hwa_production_warning_spec_at(
                result, index, &expected) != 0 ||
            !hwa_production_string_valid(warning->code, 0) ||
            !hwa_production_string_valid(warning->message, 0) ||
            strcmp(warning->code, expected.code) != 0 ||
            strcmp(warning->message, expected.message) != 0 ||
            warning->span_id != expected.span_id ||
            warning->fit_id != expected.fit_id ||
            warning->span_id_valid != expected.span_id_valid ||
            warning->fit_id_valid != expected.fit_id_valid) {
            return 0;
        }
    }
    return 1;
}

static int hwa_production_result_validate_with_ulps(
    const HWAProductionResult *result,
    unsigned max_ulps,
    char *error,
    size_t error_size)
{
    HWAProductionViewRow *expected = NULL;
    size_t expected_count = 0U;
    size_t train_count;
    size_t check_count;
    uint64_t retained;
    int valid;
    if (result == NULL || !hwa_production_options_valid(&result->options) ||
        !hwa_production_profile_method_valid(&result->profile_method) ||
        result->source_count > 5U ||
        result->span_count > result->options.max_spans ||
        result->fit_count > result->options.max_fits ||
        result->evaluation_row_count >
            result->options.max_evaluation_rows ||
        result->view_row_count > result->options.max_view_rows ||
        result->warning_count > result->options.max_warnings ||
        result->evaluation_count > result->options.max_evaluations ||
        !hwa_production_source_rows_valid(result) ||
        !hwa_production_span_rows_valid(result, &train_count, &check_count) ||
        !hwa_production_fit_rows_valid(result) ||
        !hwa_production_evaluation_rows_valid(result, max_ulps) ||
        !hwa_production_warning_rows_valid(result) ||
        hwa_production_result_retained_bytes(result, &retained) != 0 ||
        retained != result->retained_work_bytes ||
        retained > result->options.max_work_bytes) {
        hwa_production_error(error, error_size,
                             "invalid production result metadata");
        return -1;
    }
    (void)train_count;
    (void)check_count;
    if (hwa_production_build_view_rows(
            result, &expected, &expected_count, max_ulps,
            error, error_size) != 0) {
        return -1;
    }
    valid = hwa_production_view_rows_equal(
        result, expected, expected_count, max_ulps);
    free(expected);
    if (!valid) {
        hwa_production_error(error, error_size,
                             "production VIEW rows are not derived");
        return -1;
    }
    return 0;
}

int hwa_production_result_validate(const HWAProductionResult *result,
                                   char *error,
                                   size_t error_size)
{
    return hwa_production_result_validate_with_ulps(
        result, 0U, error, error_size);
}

static int hwa_production_csv_field(FILE *stream, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    int quoted = 0;
    while (*cursor != 0U) {
        if (*cursor == (unsigned char)',' || *cursor == (unsigned char)'"' ||
            *cursor == (unsigned char)'\r' ||
            *cursor == (unsigned char)'\n') {
            quoted = 1;
            break;
        }
        cursor++;
    }
    if (!quoted) return fputs(text, stream) == EOF ? -1 : 0;
    if (fputc('"', stream) == EOF) return -1;
    cursor = (const unsigned char *)text;
    while (*cursor != 0U) {
        if (*cursor == (unsigned char)'"' && fputc('"', stream) == EOF) {
            return -1;
        }
        if (fputc((int)*cursor, stream) == EOF) return -1;
        cursor++;
    }
    return fputc('"', stream) == EOF ? -1 : 0;
}

static int hwa_production_path_hex(FILE *stream, const char *path)
{
    const unsigned char *cursor = (const unsigned char *)path;
    while (*cursor != 0U) {
        if (fprintf(stream, "%02x", (unsigned)*cursor++) < 0) return -1;
    }
    return 0;
}

static int hwa_production_number(FILE *stream, double value)
{
    return !isfinite(value) ||
           fprintf(stream, "%.17g", value == 0.0 ? 0.0 : value) < 0 ? -1 : 0;
}

static int hwa_production_optional_number(FILE *stream,
                                          double value,
                                          int valid)
{
    return valid ? hwa_production_number(stream, value) : 0;
}

static int hwa_production_meta_text(FILE *stream,
                                    const char *key,
                                    const char *value,
                                    const char *unit)
{
    return fputs("META,", stream) == EOF ||
           hwa_production_csv_field(stream, key) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_production_csv_field(stream, value) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_production_csv_field(stream, unit) != 0 ||
           fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_production_meta_u64(FILE *stream,
                                   const char *key,
                                   uint64_t value,
                                   const char *unit)
{
    char text[32];
    int length = snprintf(text, sizeof(text), "%" PRIu64, value);
    if (length < 0 || (size_t)length >= sizeof(text)) return -1;
    return hwa_production_meta_text(stream, key, text, unit);
}

static int hwa_production_meta_size(FILE *stream,
                                    const char *key,
                                    size_t value,
                                    const char *unit)
{
    char text[32];
    int length = snprintf(text, sizeof(text), "%zu", value);
    if (length < 0 || (size_t)length >= sizeof(text)) return -1;
    return hwa_production_meta_text(stream, key, text, unit);
}

static int hwa_production_meta_double(FILE *stream,
                                      const char *key,
                                      double value,
                                      const char *unit)
{
    char text[64];
    int length;
    if (!isfinite(value)) return -1;
    length = snprintf(text, sizeof(text), "%.17g",
                      value == 0.0 ? 0.0 : value);
    if (length < 0 || (size_t)length >= sizeof(text)) return -1;
    return hwa_production_meta_text(stream, key, text, unit);
}

static int hwa_production_write_meta(FILE *stream,
                                     const HWAProductionResult *result)
{
    const HWAProductionOptions *options = &result->options;
    const HWAProfileComparisonOptions *profile = &options->profile_limits;
#define HWA_META_TEXT(key, value, unit) \
    do { \
        if (hwa_production_meta_text(stream, key, value, unit) != 0) \
            return -1; \
    } while (0)
#define HWA_META_U64(key, value, unit) \
    do { \
        if (hwa_production_meta_u64(stream, key, value, unit) != 0) \
            return -1; \
    } while (0)
#define HWA_META_SIZE(key, value, unit) \
    do { \
        if (hwa_production_meta_size(stream, key, value, unit) != 0) \
            return -1; \
    } while (0)
    HWA_META_TEXT("tool_version", HWA_VERSION, "");
    HWA_META_TEXT("production_method_version",
                  HWA_PRODUCTION_METHOD_VERSION, "");
    HWA_META_TEXT("source_measurement_method_version",
                  HWA_PRODUCTION_SOURCE_MEASUREMENT_METHOD_VERSION, "");
    HWA_META_SIZE("profile_fft_size", result->profile_method.fft_size,
                  "samples");
    HWA_META_SIZE("profile_hop_size", result->profile_method.hop_size,
                  "samples");
    if (hwa_production_meta_double(
            stream, "profile_pitch_confidence_floor",
            result->profile_method.pitch_confidence_floor,
            "ratio") != 0 ||
        hwa_production_meta_double(
            stream, "profile_spectral_floor_dbfs",
            result->profile_method.spectral_floor_dbfs,
            "dBFS") != 0) return -1;
    HWA_META_SIZE("profile_max_partials",
                  result->profile_method.max_partials, "partials");
    HWA_META_TEXT("build_compiler_family", hwa_build_compiler_family(), "");
    HWA_META_TEXT("build_compiler_version", hwa_build_compiler_version(), "");
    HWA_META_TEXT("build_c_standard", hwa_build_c_standard(), "");
    HWA_META_TEXT("build_target_os", hwa_build_target_os(), "");
    HWA_META_U64("build_pointer_bits", hwa_build_pointer_bits(), "bits");
    HWA_META_TEXT("build_endianness", hwa_build_endianness(), "");
    HWA_META_TEXT("build_mode", hwa_build_mode(), "");
    HWA_META_SIZE("decode_block_frames", options->decode_block_frames,
                  "frames");
    HWA_META_U64("max_input_bytes", options->max_input_bytes, "bytes");
    HWA_META_U64("max_input_frames", options->max_input_frames, "frames");
    HWA_META_U64("max_ir_frames", options->max_ir_frames, "frames");
    HWA_META_U64("max_work_bytes", options->max_work_bytes, "bytes");
    HWA_META_U64("max_evaluations", options->max_evaluations,
                 "evaluations");
    HWA_META_SIZE("max_spans", options->max_spans, "spans");
    HWA_META_SIZE("max_envelope_points", options->max_envelope_points,
                  "points");
    HWA_META_SIZE("max_fits", options->max_fits, "fits");
    HWA_META_SIZE("max_evaluation_rows", options->max_evaluation_rows,
                  "rows");
    HWA_META_SIZE("max_view_rows", options->max_view_rows, "rows");
    HWA_META_SIZE("max_warnings", options->max_warnings, "warnings");
    HWA_META_U64("profile_max_input_bytes", profile->max_input_bytes,
                 "bytes");
    HWA_META_U64("profile_max_work_bytes", profile->max_work_bytes,
                 "bytes");
    HWA_META_SIZE("profile_max_contexts", profile->max_contexts, "contexts");
    HWA_META_SIZE("profile_max_measurements", profile->max_measurements,
                  "measurements");
    HWA_META_SIZE("profile_max_groups", profile->max_groups, "groups");
    HWA_META_SIZE("profile_max_group_members", profile->max_group_members,
                  "members");
    HWA_META_SIZE("profile_max_statistics", profile->max_statistics,
                  "statistics");
    HWA_META_SIZE("profile_max_warnings", profile->max_warnings, "warnings");
    HWA_META_SIZE("profile_max_distributions", profile->max_distributions,
                  "distributions");
    HWA_META_SIZE("profile_max_gaps", profile->max_gaps, "gaps");
    HWA_META_U64("retained_work_bytes", result->retained_work_bytes, "bytes");
    HWA_META_U64("evaluation_count", result->evaluation_count,
                 "evaluations");
    HWA_META_SIZE("source_count", result->source_count, "sources");
    HWA_META_SIZE("span_count", result->span_count, "spans");
    HWA_META_SIZE("fit_count", result->fit_count, "fits");
    HWA_META_SIZE("evaluation_row_count", result->evaluation_row_count,
                  "rows");
    HWA_META_SIZE("view_row_count", result->view_row_count, "rows");
    HWA_META_SIZE("warning_count", result->warning_count, "warnings");
#undef HWA_META_SIZE
#undef HWA_META_U64
#undef HWA_META_TEXT
    return 0;
}

static int hwa_production_write_source(
    FILE *stream,
    const HWAProductionSource *source)
{
    const HWAFormat *format = &source->format;
    if (fprintf(stream, "INPUT,%" PRIu64 ",", source->id) < 0 ||
        hwa_production_csv_field(stream, source->role) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_path_hex(stream, source->path) != 0 ||
        fprintf(stream, ",%s,%d", source->sha256,
                source->is_wave ? 1 : 0) < 0) return -1;
    if (!source->is_wave) {
        return fputs(",,,,,,,,,,,\r\n", stream) == EOF ? -1 : 0;
    }
    return fputc(',', stream) == EOF ||
           hwa_production_csv_field(
               stream, hwa_container_name(format->container)) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_production_csv_field(
               stream, hwa_encoding_name(format->encoding)) != 0 ||
           fprintf(stream,
                   ",%u,%" PRIu32 ",%u,%u,%u,%" PRIu32 ",%" PRIu64
                   ",%" PRIu64 ",",
                   (unsigned)format->channels, format->sample_rate_hz,
                   (unsigned)format->bits_per_sample,
                   (unsigned)format->valid_bits_per_sample,
                   (unsigned)format->block_align, format->channel_mask,
                   format->frames, format->data_bytes) < 0 ||
           hwa_production_number(stream, format->duration_seconds) != 0 ||
           fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_production_write_span(FILE *stream,
                                     const HWAProductionSpan *span)
{
    if (fprintf(stream, "SPAN,%" PRIu64 ",", span->id) < 0 ||
        hwa_production_csv_field(
            stream, hwa_production_split_name(span->split)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_csv_field(stream, span->item_key) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_csv_field(
            stream, hwa_measure_item_kind_name(span->item_kind)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_csv_field(stream, span->item_role) != 0 ||
        fprintf(stream,
                ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                ",%" PRIu64 ",%" PRIu64 ",%" PRIu32 "\r\n",
                span->reference_item_id, span->reference_start_sample,
                span->reference_end_sample, span->model_item_id,
                span->model_start_sample, span->model_end_sample,
                span->eligibility_flags) < 0) return -1;
    return 0;
}

static int hwa_production_write_fit(FILE *stream,
                                    const HWAProductionFit *fit)
{
    if (fprintf(stream, "FIT,%" PRIu64 ",", fit->id) < 0 ||
        hwa_production_csv_field(
            stream, hwa_production_scope_name(fit->scope)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_csv_field(
            stream, hwa_production_fit_kind_name(fit->kind)) != 0 ||
        fprintf(stream, ",%" PRIu32 ",", fit->index) < 0 ||
        hwa_production_csv_field(
            stream, hwa_production_unit_name(fit->unit)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_csv_field(
            stream, hwa_production_availability_name(fit->availability)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_optional_number(
            stream, fit->estimate, fit->estimate_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_optional_number(
            stream, fit->q05, fit->uncertainty_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_optional_number(
            stream, fit->q95, fit->uncertainty_valid) != 0 ||
        fprintf(stream, ",%zu,%zu,%" PRIu32 ",%d,%d\r\n",
                fit->span_count, fit->point_count, fit->quality_flags,
                fit->estimate_valid ? 1 : 0,
                fit->uncertainty_valid ? 1 : 0) < 0) return -1;
    return 0;
}

static int hwa_production_write_evaluation(
    FILE *stream,
    const HWAProductionEvaluation *row)
{
    if (fprintf(stream, "EVALUATION,%" PRIu64 ",%" PRIu64 ",",
                row->id, row->span_id) < 0 ||
        hwa_production_csv_field(
            stream, hwa_production_view_name(row->view)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_csv_field(
            stream, hwa_production_metric_kind_name(row->kind)) != 0 ||
        fprintf(stream, ",%" PRIu32 ",", row->index) < 0 ||
        hwa_production_csv_field(
            stream, hwa_production_unit_name(row->unit)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_csv_field(
            stream,
            hwa_production_availability_name(row->availability)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_optional_number(
            stream, row->reference_value, row->reference_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_optional_number(
            stream, row->model_value, row->model_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_optional_number(
            stream, row->delta, row->delta_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_number(stream, row->confidence) != 0 ||
        fprintf(stream, ",%" PRIu32 ",%" PRIu32 ",%d,%d,%d\r\n",
                row->evidence_flags, row->quality_flags,
                row->reference_valid ? 1 : 0,
                row->model_valid ? 1 : 0,
                row->delta_valid ? 1 : 0) < 0) return -1;
    return 0;
}

static int hwa_production_write_statistics(
    FILE *stream,
    const HWAProductionStatistics *statistics)
{
    if (fprintf(stream, ",%zu,%zu,%zu,",
                statistics->total_count, statistics->valid_count,
                statistics->missing_count) < 0 ||
        hwa_production_optional_number(
            stream, statistics->minimum, statistics->valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_optional_number(
            stream, statistics->q05, statistics->valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_optional_number(
            stream, statistics->q25, statistics->valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_optional_number(
            stream, statistics->q50, statistics->valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_optional_number(
            stream, statistics->q75, statistics->valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_optional_number(
            stream, statistics->q95, statistics->valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_optional_number(
            stream, statistics->maximum, statistics->valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_optional_number(
            stream, statistics->mean, statistics->valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_optional_number(
            stream, statistics->population_sd, statistics->valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_optional_number(
            stream, statistics->confidence, statistics->valid) != 0 ||
        fprintf(stream, ",%d", statistics->valid ? 1 : 0) < 0) return -1;
    return 0;
}

static int hwa_production_write_view(FILE *stream,
                                     const HWAProductionViewRow *row)
{
    if (fprintf(stream, "VIEW,%" PRIu64 ",", row->id) < 0 ||
        hwa_production_csv_field(
            stream, hwa_production_split_name(row->split)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_csv_field(
            stream, hwa_production_view_name(row->view)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_csv_field(
            stream, hwa_production_metric_kind_name(row->kind)) != 0 ||
        fprintf(stream, ",%" PRIu32 ",", row->index) < 0 ||
        hwa_production_csv_field(
            stream, hwa_production_unit_name(row->unit)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_csv_field(
            stream,
            hwa_production_availability_name(row->availability)) != 0 ||
        hwa_production_write_statistics(
            stream, &row->reference_statistics) != 0 ||
        hwa_production_write_statistics(
            stream, &row->model_statistics) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_optional_number(
            stream, row->median_delta, row->gap_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_optional_number(
            stream, row->quantile_distance, row->gap_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_optional_number(
            stream, row->gap_score, row->gap_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_number(stream, row->raw_gap_score) != 0 ||
        fprintf(stream, ",%" PRIu32 ",%d,%d\r\n",
                row->quality_flags, row->survives ? 1 : 0,
                row->gap_valid ? 1 : 0) < 0) return -1;
    return 0;
}

static int hwa_production_write_warning(
    FILE *stream,
    const HWAProductionWarning *warning)
{
    if (fprintf(stream, "WARNING,%" PRIu64 ",", warning->id) < 0 ||
        hwa_production_csv_field(stream, warning->code) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_production_csv_field(stream, warning->message) != 0 ||
        fputc(',', stream) == EOF) return -1;
    if (warning->span_id_valid &&
        fprintf(stream, "%" PRIu64, warning->span_id) < 0) return -1;
    if (fputc(',', stream) == EOF) return -1;
    if (warning->fit_id_valid &&
        fprintf(stream, "%" PRIu64, warning->fit_id) < 0) return -1;
    return fprintf(stream, ",%d,%d\r\n",
                   warning->span_id_valid ? 1 : 0,
                   warning->fit_id_valid ? 1 : 0) < 0 ? -1 : 0;
}

static int hwa_production_file_write_impl(
    FILE *stream,
    const HWAProductionResult *result,
    char *error,
    size_t error_size)
{
    size_t index;
    if (stream == NULL) {
        hwa_production_error(error, error_size,
                             "production output stream is null");
        return -1;
    }
    if (hwa_production_result_validate(result, error, error_size) != 0) {
        return -1;
    }
    if (fprintf(stream, "HWA_PRODUCTION,%u\r\n",
                HWA_PRODUCTION_FILE_SCHEMA_VERSION) < 0 ||
        hwa_production_write_meta(stream, result) != 0) goto write_error;
    for (index = 0U; index < result->source_count; ++index) {
        if (hwa_production_write_source(
                stream, &result->sources[index]) != 0) goto write_error;
    }
    for (index = 0U; index < result->span_count; ++index) {
        if (hwa_production_write_span(
                stream, &result->spans[index]) != 0) goto write_error;
    }
    for (index = 0U; index < result->fit_count; ++index) {
        if (hwa_production_write_fit(
                stream, &result->fits[index]) != 0) goto write_error;
    }
    for (index = 0U; index < result->evaluation_row_count; ++index) {
        if (hwa_production_write_evaluation(
                stream, &result->evaluations[index]) != 0) goto write_error;
    }
    for (index = 0U; index < result->view_row_count; ++index) {
        if (hwa_production_write_view(
                stream, &result->view_rows[index]) != 0) goto write_error;
    }
    for (index = 0U; index < result->warning_count; ++index) {
        if (hwa_production_write_warning(
                stream, &result->warnings[index]) != 0) goto write_error;
    }
    return 0;

write_error:
    hwa_production_error(error, error_size,
                         "cannot write production output");
    return -1;
}

int hwa_production_file_write(FILE *stream,
                              const HWAProductionResult *result,
                              char *error,
                              size_t error_size)
{
    HWANumericLocale locale;
    int status;
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_production_error(
            error, error_size,
            "cannot enter the C numeric locale for production output");
        return -1;
    }
    status = hwa_production_file_write_impl(
        stream, result, error, error_size);
    if (hwa_c_numeric_locale_end(&locale) != 0) {
        if (status == 0 || error == NULL || error_size == 0U ||
            error[0] == '\0') {
            hwa_production_error(
                error, error_size,
                "cannot restore the numeric locale after production output");
        }
        return -1;
    }
    return status;
}

static int hwa_production_parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    char canonical[32];
    unsigned long long parsed;
    if (text == NULL || text[0] == '\0' || text[0] == '-' ||
        text[0] == '+') return -1;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        parsed > (unsigned long long)UINT64_MAX) return -1;
    *value = (uint64_t)parsed;
    if (snprintf(canonical, sizeof(canonical), "%" PRIu64, *value) < 0 ||
        strcmp(text, canonical) != 0) return -1;
    return 0;
}

static int hwa_production_parse_size(const char *text, size_t *value)
{
    uint64_t parsed;
    if (hwa_production_parse_u64(text, &parsed) != 0 ||
        parsed > (uint64_t)SIZE_MAX) return -1;
    *value = (size_t)parsed;
    return 0;
}

static int hwa_production_parse_u32(const char *text, uint32_t *value)
{
    uint64_t parsed;
    if (hwa_production_parse_u64(text, &parsed) != 0 ||
        parsed > UINT32_MAX) return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static int hwa_production_parse_u16(const char *text, uint16_t *value)
{
    uint64_t parsed;
    if (hwa_production_parse_u64(text, &parsed) != 0 ||
        parsed > UINT16_MAX) return -1;
    *value = (uint16_t)parsed;
    return 0;
}

static int hwa_production_parse_bool(const char *text, int *value)
{
    if (strcmp(text, "0") == 0) {
        *value = 0;
        return 0;
    }
    if (strcmp(text, "1") == 0) {
        *value = 1;
        return 0;
    }
    return -1;
}

static int hwa_production_parse_double(const char *text, double *value)
{
    char *end = NULL;
    char canonical[64];
    double parsed;
    if (text == NULL || text[0] == '\0') return -1;
    errno = 0;
    parsed = strtod(text, &end);
    if (end == text || *end != '\0' || !isfinite(parsed) ||
        (errno == ERANGE && fpclassify(parsed) != FP_SUBNORMAL)) return -1;
    *value = parsed == 0.0 ? 0.0 : parsed;
    if (snprintf(canonical, sizeof(canonical), "%.17g", *value) < 0 ||
        strcmp(text, canonical) != 0) return -1;
    return 0;
}

static int hwa_production_hex_nibble(unsigned char byte)
{
    if (byte >= (unsigned char)'0' && byte <= (unsigned char)'9') {
        return (int)(byte - (unsigned char)'0');
    }
    if (byte >= (unsigned char)'a' && byte <= (unsigned char)'f') {
        return (int)(byte - (unsigned char)'a') + 10;
    }
    return -1;
}

static char *hwa_production_decode_path(const char *hex)
{
    size_t length = strlen(hex);
    size_t index;
    char *path;
    if (length == 0U || (length & 1U) != 0U ||
        length / 2U > HWA_PRODUCTION_FILE_MAX_FIELD_BYTES) return NULL;
    path = (char *)malloc(length / 2U + 1U);
    if (path == NULL) return NULL;
    for (index = 0U; index < length; index += 2U) {
        int high = hwa_production_hex_nibble((unsigned char)hex[index]);
        int low = hwa_production_hex_nibble((unsigned char)hex[index + 1U]);
        unsigned char byte;
        if (high < 0 || low < 0) {
            free(path);
            return NULL;
        }
        byte = (unsigned char)((unsigned)high * 16U + (unsigned)low);
        if (byte == 0U) {
            free(path);
            return NULL;
        }
        path[index / 2U] = (char)byte;
    }
    path[length / 2U] = '\0';
    return path;
}

typedef enum HWAProductionReadSection {
    HWA_PRODUCTION_READ_MAGIC = 1,
    HWA_PRODUCTION_READ_META = 2,
    HWA_PRODUCTION_READ_INPUT = 3,
    HWA_PRODUCTION_READ_SPAN = 4,
    HWA_PRODUCTION_READ_FIT = 5,
    HWA_PRODUCTION_READ_EVALUATION = 6,
    HWA_PRODUCTION_READ_VIEW = 7,
    HWA_PRODUCTION_READ_WARNING = 8,
    HWA_PRODUCTION_READ_DONE = 9
} HWAProductionReadSection;

typedef struct HWAProductionReadState {
    const HWAProductionOptions *limits;
    HWAProductionResult *result;
    HWAProductionReadSection section;
    size_t meta_index;
    size_t source_index;
    size_t span_index;
    size_t fit_index;
    size_t evaluation_index;
    size_t view_index;
    size_t warning_index;
    uint64_t work_live;
    int arrays_ready;
} HWAProductionReadState;

static int hwa_production_read_charge(HWAProductionReadState *state,
                                      uint64_t bytes)
{
    if (state->work_live > state->limits->max_work_bytes ||
        bytes > state->limits->max_work_bytes - state->work_live) return -1;
    state->work_live += bytes;
    return 0;
}

static char *hwa_production_read_copy(HWAProductionReadState *state,
                                      const char *text)
{
    size_t length = strlen(text);
    char *copy;
    if (length > HWA_PRODUCTION_FILE_MAX_FIELD_BYTES ||
        length == SIZE_MAX ||
        hwa_production_read_charge(
            state, (uint64_t)length + 1U) != 0) return NULL;
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        state->work_live -= (uint64_t)length + 1U;
        return NULL;
    }
    memcpy(copy, text, length + 1U);
    return copy;
}

static int hwa_production_read_arrays(HWAProductionReadState *state,
                                      char *error,
                                      size_t error_size)
{
    HWAProductionResult *result = state->result;
    uint64_t bytes;
    size_t expected_evaluations;
    size_t expected_views;
    if (state->arrays_ready) return 0;
    expected_views = ((size_t)HWA_PRODUCTION_SPLIT_COUNT - 1U) *
                     ((size_t)HWA_PRODUCTION_VIEW_COUNT - 1U) *
                     hwa_production_metric_catalog_count();
    if (hwa_production_expected_evaluation_count(
            result->span_count, &expected_evaluations) != 0 ||
        result->source_count < 4U || result->source_count > 5U ||
        result->span_count == 0U ||
        result->span_count > state->limits->max_spans ||
        result->fit_count != hwa_production_fit_catalog_count() ||
        result->fit_count > state->limits->max_fits ||
        result->evaluation_row_count != expected_evaluations ||
        result->evaluation_row_count >
            state->limits->max_evaluation_rows ||
        result->view_row_count != expected_views ||
        result->view_row_count > state->limits->max_view_rows ||
        result->warning_count > state->limits->max_warnings ||
        result->evaluation_count > state->limits->max_evaluations) {
        hwa_production_error(error, error_size,
                             "production result exceeds a current row limit");
        return -1;
    }
#define HWA_PRODUCTION_ALLOC_ROWS(member, count, type)                      \
    do {                                                                    \
        if ((count) > SIZE_MAX / sizeof(type)) {                            \
            hwa_production_error(error, error_size,                         \
                                 "production result allocation overflows"); \
            return -1;                                                      \
        }                                                                   \
        bytes = (uint64_t)(count) * (uint64_t)sizeof(type);                 \
        if (hwa_production_read_charge(state, bytes) != 0) {                \
            hwa_production_error(                                           \
                error, error_size,                                          \
                "production result exceeds the work-byte limit");          \
            return -1;                                                      \
        }                                                                   \
        if ((count) != 0U) {                                                \
            result->member = (type *)calloc((count), sizeof(type));         \
            if (result->member == NULL) {                                   \
                state->work_live -= bytes;                                  \
                hwa_production_error(                                       \
                    error, error_size,                                      \
                    "out of memory for production result");                \
                return -1;                                                  \
            }                                                               \
        }                                                                   \
    } while (0)
    HWA_PRODUCTION_ALLOC_ROWS(
        sources, result->source_count, HWAProductionSource);
    HWA_PRODUCTION_ALLOC_ROWS(spans, result->span_count, HWAProductionSpan);
    HWA_PRODUCTION_ALLOC_ROWS(fits, result->fit_count, HWAProductionFit);
    HWA_PRODUCTION_ALLOC_ROWS(
        evaluations, result->evaluation_row_count, HWAProductionEvaluation);
    HWA_PRODUCTION_ALLOC_ROWS(
        view_rows, result->view_row_count, HWAProductionViewRow);
    HWA_PRODUCTION_ALLOC_ROWS(
        warnings, result->warning_count, HWAProductionWarning);
#undef HWA_PRODUCTION_ALLOC_ROWS
    state->arrays_ready = 1;
    return 0;
}

static int hwa_production_read_meta_value(
    HWAProductionReadState *state,
    size_t index,
    const char *value)
{
    HWAProductionResult *result = state->result;
    HWAProductionOptions *options = &result->options;
    HWAProfileComparisonOptions *profile = &options->profile_limits;
    uint64_t u64;
    switch (index) {
    case 0U: return hwa_production_string_valid(value, 0) ? 0 : -1;
    case 1U:
        return strcmp(value, HWA_PRODUCTION_METHOD_VERSION) == 0 ? 0 : -1;
    case 2U:
        return strcmp(
            value, HWA_PRODUCTION_SOURCE_MEASUREMENT_METHOD_VERSION) == 0
                ? 0 : -1;
    case 3U:
        return hwa_production_parse_size(
            value, &result->profile_method.fft_size);
    case 4U:
        return hwa_production_parse_size(
            value, &result->profile_method.hop_size);
    case 5U:
        return hwa_production_parse_double(
            value, &result->profile_method.pitch_confidence_floor);
    case 6U:
        return hwa_production_parse_double(
            value, &result->profile_method.spectral_floor_dbfs);
    case 7U:
        return hwa_production_parse_size(
            value, &result->profile_method.max_partials);
    case 8U:
    case 9U:
    case 10U:
    case 11U:
    case 13U:
    case 14U:
        return hwa_production_string_valid(value, 0) ? 0 : -1;
    case 12U:
        return hwa_production_parse_u64(value, &u64) == 0 &&
               u64 != 0U ? 0 : -1;
    case 15U:
        return hwa_production_parse_size(
            value, &options->decode_block_frames);
    case 16U:
        return hwa_production_parse_u64(value, &options->max_input_bytes);
    case 17U:
        return hwa_production_parse_u64(value, &options->max_input_frames);
    case 18U:
        return hwa_production_parse_u64(value, &options->max_ir_frames);
    case 19U:
        return hwa_production_parse_u64(value, &options->max_work_bytes);
    case 20U:
        return hwa_production_parse_u64(value, &options->max_evaluations);
    case 21U:
        return hwa_production_parse_size(value, &options->max_spans);
    case 22U:
        return hwa_production_parse_size(
            value, &options->max_envelope_points);
    case 23U:
        return hwa_production_parse_size(value, &options->max_fits);
    case 24U:
        return hwa_production_parse_size(
            value, &options->max_evaluation_rows);
    case 25U:
        return hwa_production_parse_size(value, &options->max_view_rows);
    case 26U:
        return hwa_production_parse_size(value, &options->max_warnings);
    case 27U:
        return hwa_production_parse_u64(value, &profile->max_input_bytes);
    case 28U:
        return hwa_production_parse_u64(value, &profile->max_work_bytes);
    case 29U:
        return hwa_production_parse_size(value, &profile->max_contexts);
    case 30U:
        return hwa_production_parse_size(value, &profile->max_measurements);
    case 31U:
        return hwa_production_parse_size(value, &profile->max_groups);
    case 32U:
        return hwa_production_parse_size(value, &profile->max_group_members);
    case 33U:
        return hwa_production_parse_size(value, &profile->max_statistics);
    case 34U:
        return hwa_production_parse_size(value, &profile->max_warnings);
    case 35U:
        return hwa_production_parse_size(value, &profile->max_distributions);
    case 36U:
        return hwa_production_parse_size(value, &profile->max_gaps);
    case 37U:
        return hwa_production_parse_u64(
            value, &result->retained_work_bytes);
    case 38U:
        return hwa_production_parse_u64(value, &result->evaluation_count);
    case 39U:
        return hwa_production_parse_size(value, &result->source_count);
    case 40U:
        return hwa_production_parse_size(value, &result->span_count);
    case 41U:
        return hwa_production_parse_size(value, &result->fit_count);
    case 42U:
        return hwa_production_parse_size(
            value, &result->evaluation_row_count);
    case 43U:
        return hwa_production_parse_size(value, &result->view_row_count);
    case 44U:
        return hwa_production_parse_size(value, &result->warning_count);
    default:
        return -1;
    }
}

static int hwa_production_read_take_path(
    HWAProductionReadState *state,
    const char *hex,
    char **target)
{
    size_t hex_length = strlen(hex);
    uint64_t bytes;
    char *path;
    if (hex_length == 0U || (hex_length & 1U) != 0U ||
        hex_length / 2U > HWA_PRODUCTION_FILE_MAX_FIELD_BYTES) return -1;
    bytes = (uint64_t)(hex_length / 2U) + 1U;
    if (hwa_production_read_charge(state, bytes) != 0) return -1;
    path = hwa_production_decode_path(hex);
    if (path == NULL) {
        state->work_live -= bytes;
        return -1;
    }
    *target = path;
    return 0;
}

static int hwa_production_read_optional_double(
    const char *text,
    int valid,
    double *value)
{
    *value = 0.0;
    if (!valid) return text[0] == '\0' ? 0 : -1;
    return hwa_production_parse_double(text, value);
}

static int hwa_production_read_source(HWAProductionReadState *state,
                                      char **fields,
                                      size_t field_count)
{
    HWAProductionSource *source;
    uint64_t id;
    int is_wave;
    if (field_count != 17U ||
        state->source_index >= state->result->source_count ||
        hwa_production_parse_u64(fields[1], &id) != 0 ||
        id != (uint64_t)state->source_index + 1U ||
        !hwa_production_sha_valid(fields[4]) ||
        hwa_production_parse_bool(fields[5], &is_wave) != 0) return -1;
    source = &state->result->sources[state->source_index];
    source->id = id;
    source->role = hwa_production_read_copy(state, fields[2]);
    if (source->role == NULL ||
        hwa_production_read_take_path(
            state, fields[3], &source->path) != 0) return -1;
    memcpy(source->sha256, fields[4], HWA_SHA256_HEX_SIZE);
    source->is_wave = is_wave;
    if (!is_wave) {
        size_t index;
        if (state->source_index != 0U && state->source_index != 2U) {
            return -1;
        }
        for (index = 6U; index < 17U; ++index) {
            if (fields[index][0] != '\0') return -1;
        }
    } else {
        HWAFormat *format = &source->format;
        uint16_t channels;
        uint64_t frame_limit = state->source_index == 4U ?
            state->limits->max_ir_frames : state->limits->max_input_frames;
        if ((state->source_index != 1U &&
             state->source_index != 3U &&
             state->source_index != 4U) ||
            (strcmp(fields[6], "RIFF") != 0 &&
             strcmp(fields[6], "RF64") != 0) ||
            (strcmp(fields[7], "PCM") != 0 &&
             strcmp(fields[7], "IEEE float") != 0) ||
            hwa_production_parse_u16(fields[8], &channels) != 0 ||
            hwa_production_parse_u32(
                fields[9], &format->sample_rate_hz) != 0 ||
            hwa_production_parse_u16(
                fields[10], &format->bits_per_sample) != 0 ||
            hwa_production_parse_u16(
                fields[11], &format->valid_bits_per_sample) != 0 ||
            hwa_production_parse_u16(
                fields[12], &format->block_align) != 0 ||
            hwa_production_parse_u32(
                fields[13], &format->channel_mask) != 0 ||
            hwa_production_parse_u64(fields[14], &format->frames) != 0 ||
            hwa_production_parse_u64(
                fields[15], &format->data_bytes) != 0 ||
            hwa_production_parse_double(
                fields[16], &format->duration_seconds) != 0 ||
            format->frames > frame_limit ||
            format->data_bytes > state->limits->max_input_bytes) return -1;
        format->container = strcmp(fields[6], "RIFF") == 0 ?
            HWA_CONTAINER_RIFF : HWA_CONTAINER_RF64;
        format->encoding = strcmp(fields[7], "PCM") == 0 ?
            HWA_ENCODING_PCM : HWA_ENCODING_IEEE_FLOAT;
        format->channels = channels;
    }
    state->source_index++;
    return 0;
}

static int hwa_production_read_span(HWAProductionReadState *state,
                                    char **fields,
                                    size_t field_count)
{
    HWAProductionSpan *span;
    uint64_t id;
    if (field_count != 13U ||
        state->span_index >= state->result->span_count ||
        hwa_production_parse_u64(fields[1], &id) != 0 ||
        id != (uint64_t)state->span_index + 1U) return -1;
    span = &state->result->spans[state->span_index];
    span->id = id;
    span->item_key = hwa_production_read_copy(state, fields[3]);
    span->item_role = hwa_production_read_copy(state, fields[5]);
    if (span->item_key == NULL || span->item_role == NULL ||
        hwa_production_split_from_name(fields[2], &span->split) != 0 ||
        hwa_measure_item_kind_from_name(fields[4], &span->item_kind) != 0 ||
        hwa_production_parse_u64(
            fields[6], &span->reference_item_id) != 0 ||
        hwa_production_parse_u64(
            fields[7], &span->reference_start_sample) != 0 ||
        hwa_production_parse_u64(
            fields[8], &span->reference_end_sample) != 0 ||
        hwa_production_parse_u64(fields[9], &span->model_item_id) != 0 ||
        hwa_production_parse_u64(
            fields[10], &span->model_start_sample) != 0 ||
        hwa_production_parse_u64(
            fields[11], &span->model_end_sample) != 0 ||
        hwa_production_parse_u32(
            fields[12], &span->eligibility_flags) != 0) return -1;
    state->span_index++;
    return 0;
}

static int hwa_production_read_fit(HWAProductionReadState *state,
                                   char **fields,
                                   size_t field_count)
{
    HWAProductionFit *fit;
    uint64_t id;
    if (field_count != 15U ||
        state->fit_index >= state->result->fit_count ||
        hwa_production_parse_u64(fields[1], &id) != 0 ||
        id != (uint64_t)state->fit_index + 1U) return -1;
    fit = &state->result->fits[state->fit_index];
    fit->id = id;
    if (hwa_production_scope_from_name(fields[2], &fit->scope) != 0 ||
        hwa_production_fit_kind_from_name(fields[3], &fit->kind) != 0 ||
        hwa_production_parse_u32(fields[4], &fit->index) != 0 ||
        hwa_production_unit_from_name(fields[5], &fit->unit) != 0 ||
        hwa_production_availability_from_name(
            fields[6], &fit->availability) != 0 ||
        hwa_production_parse_size(fields[10], &fit->span_count) != 0 ||
        hwa_production_parse_size(fields[11], &fit->point_count) != 0 ||
        hwa_production_parse_u32(fields[12], &fit->quality_flags) != 0 ||
        hwa_production_parse_bool(
            fields[13], &fit->estimate_valid) != 0 ||
        hwa_production_parse_bool(
            fields[14], &fit->uncertainty_valid) != 0 ||
        hwa_production_read_optional_double(
            fields[7], fit->estimate_valid, &fit->estimate) != 0 ||
        hwa_production_read_optional_double(
            fields[8], fit->uncertainty_valid, &fit->q05) != 0 ||
        hwa_production_read_optional_double(
            fields[9], fit->uncertainty_valid, &fit->q95) != 0) return -1;
    state->fit_index++;
    return 0;
}

static int hwa_production_read_evaluation(
    HWAProductionReadState *state,
    char **fields,
    size_t field_count)
{
    HWAProductionEvaluation *row;
    uint64_t id;
    if (field_count != 17U ||
        state->evaluation_index >= state->result->evaluation_row_count ||
        hwa_production_parse_u64(fields[1], &id) != 0 ||
        id != (uint64_t)state->evaluation_index + 1U) return -1;
    row = &state->result->evaluations[state->evaluation_index];
    row->id = id;
    if (hwa_production_parse_u64(fields[2], &row->span_id) != 0 ||
        hwa_production_view_from_name(fields[3], &row->view) != 0 ||
        hwa_production_metric_kind_from_name(fields[4], &row->kind) != 0 ||
        hwa_production_parse_u32(fields[5], &row->index) != 0 ||
        hwa_production_unit_from_name(fields[6], &row->unit) != 0 ||
        hwa_production_availability_from_name(
            fields[7], &row->availability) != 0 ||
        hwa_production_parse_double(fields[11], &row->confidence) != 0 ||
        hwa_production_parse_u32(fields[12], &row->evidence_flags) != 0 ||
        hwa_production_parse_u32(fields[13], &row->quality_flags) != 0 ||
        hwa_production_parse_bool(
            fields[14], &row->reference_valid) != 0 ||
        hwa_production_parse_bool(fields[15], &row->model_valid) != 0 ||
        hwa_production_parse_bool(fields[16], &row->delta_valid) != 0 ||
        hwa_production_read_optional_double(
            fields[8], row->reference_valid, &row->reference_value) != 0 ||
        hwa_production_read_optional_double(
            fields[9], row->model_valid, &row->model_value) != 0 ||
        hwa_production_read_optional_double(
            fields[10], row->delta_valid, &row->delta) != 0) return -1;
    state->evaluation_index++;
    return 0;
}

static int hwa_production_read_statistics(
    char **fields,
    size_t offset,
    HWAProductionStatistics *statistics)
{
    if (hwa_production_parse_size(
            fields[offset], &statistics->total_count) != 0 ||
        hwa_production_parse_size(
            fields[offset + 1U], &statistics->valid_count) != 0 ||
        hwa_production_parse_size(
            fields[offset + 2U], &statistics->missing_count) != 0 ||
        hwa_production_parse_bool(
            fields[offset + 13U], &statistics->valid) != 0 ||
        hwa_production_read_optional_double(
            fields[offset + 3U], statistics->valid,
            &statistics->minimum) != 0 ||
        hwa_production_read_optional_double(
            fields[offset + 4U], statistics->valid,
            &statistics->q05) != 0 ||
        hwa_production_read_optional_double(
            fields[offset + 5U], statistics->valid,
            &statistics->q25) != 0 ||
        hwa_production_read_optional_double(
            fields[offset + 6U], statistics->valid,
            &statistics->q50) != 0 ||
        hwa_production_read_optional_double(
            fields[offset + 7U], statistics->valid,
            &statistics->q75) != 0 ||
        hwa_production_read_optional_double(
            fields[offset + 8U], statistics->valid,
            &statistics->q95) != 0 ||
        hwa_production_read_optional_double(
            fields[offset + 9U], statistics->valid,
            &statistics->maximum) != 0 ||
        hwa_production_read_optional_double(
            fields[offset + 10U], statistics->valid,
            &statistics->mean) != 0 ||
        hwa_production_read_optional_double(
            fields[offset + 11U], statistics->valid,
            &statistics->population_sd) != 0 ||
        hwa_production_read_optional_double(
            fields[offset + 12U], statistics->valid,
            &statistics->confidence) != 0) return -1;
    return 0;
}

static int hwa_production_read_view(HWAProductionReadState *state,
                                    char **fields,
                                    size_t field_count)
{
    HWAProductionViewRow *row;
    uint64_t id;
    if (field_count != 43U ||
        state->view_index >= state->result->view_row_count ||
        hwa_production_parse_u64(fields[1], &id) != 0 ||
        id != (uint64_t)state->view_index + 1U) return -1;
    row = &state->result->view_rows[state->view_index];
    row->id = id;
    if (hwa_production_split_from_name(fields[2], &row->split) != 0 ||
        hwa_production_view_from_name(fields[3], &row->view) != 0 ||
        hwa_production_metric_kind_from_name(fields[4], &row->kind) != 0 ||
        hwa_production_parse_u32(fields[5], &row->index) != 0 ||
        hwa_production_unit_from_name(fields[6], &row->unit) != 0 ||
        hwa_production_availability_from_name(
            fields[7], &row->availability) != 0 ||
        hwa_production_read_statistics(
            fields, 8U, &row->reference_statistics) != 0 ||
        hwa_production_read_statistics(
            fields, 22U, &row->model_statistics) != 0 ||
        hwa_production_parse_double(fields[39], &row->raw_gap_score) != 0 ||
        hwa_production_parse_u32(fields[40], &row->quality_flags) != 0 ||
        hwa_production_parse_bool(fields[41], &row->survives) != 0 ||
        hwa_production_parse_bool(fields[42], &row->gap_valid) != 0 ||
        hwa_production_read_optional_double(
            fields[36], row->gap_valid, &row->median_delta) != 0 ||
        hwa_production_read_optional_double(
            fields[37], row->gap_valid, &row->quantile_distance) != 0 ||
        hwa_production_read_optional_double(
            fields[38], row->gap_valid, &row->gap_score) != 0) return -1;
    state->view_index++;
    return 0;
}

static int hwa_production_read_warning(HWAProductionReadState *state,
                                       char **fields,
                                       size_t field_count)
{
    HWAProductionWarning *warning;
    uint64_t id;
    if (field_count != 8U ||
        state->warning_index >= state->result->warning_count ||
        hwa_production_parse_u64(fields[1], &id) != 0 ||
        id != (uint64_t)state->warning_index + 1U) return -1;
    warning = &state->result->warnings[state->warning_index];
    warning->id = id;
    warning->code = hwa_production_read_copy(state, fields[2]);
    warning->message = hwa_production_read_copy(state, fields[3]);
    if (warning->code == NULL || warning->message == NULL ||
        hwa_production_parse_bool(
            fields[6], &warning->span_id_valid) != 0 ||
        hwa_production_parse_bool(
            fields[7], &warning->fit_id_valid) != 0) return -1;
    if (warning->span_id_valid) {
        if (hwa_production_parse_u64(
                fields[4], &warning->span_id) != 0) return -1;
    } else if (fields[4][0] != '\0') {
        return -1;
    }
    if (warning->fit_id_valid) {
        if (hwa_production_parse_u64(
                fields[5], &warning->fit_id) != 0) return -1;
    } else if (fields[5][0] != '\0') {
        return -1;
    }
    state->warning_index++;
    return 0;
}

static int hwa_production_read_row(char **fields,
                                   size_t field_count,
                                   size_t row,
                                   void *user,
                                   char *error,
                                   size_t error_size)
{
    HWAProductionReadState *state = (HWAProductionReadState *)user;
    int status = -1;
    if (state->section == HWA_PRODUCTION_READ_MAGIC) {
        uint64_t version;
        if (field_count == 2U &&
            strcmp(fields[0], "HWA_PRODUCTION") == 0 &&
            hwa_production_parse_u64(fields[1], &version) == 0 &&
            version == HWA_PRODUCTION_FILE_SCHEMA_VERSION) {
            state->section = HWA_PRODUCTION_READ_META;
            return 0;
        }
    } else if (state->section == HWA_PRODUCTION_READ_META) {
        size_t meta_count = sizeof(hwa_production_meta_keys) /
                            sizeof(hwa_production_meta_keys[0]);
        if (field_count == 4U && strcmp(fields[0], "META") == 0 &&
            state->meta_index < meta_count &&
            strcmp(fields[1],
                   hwa_production_meta_keys[state->meta_index].key) == 0 &&
            strcmp(fields[3],
                   hwa_production_meta_keys[state->meta_index].unit) == 0 &&
            hwa_production_read_meta_value(
                state, state->meta_index, fields[2]) == 0) {
            state->meta_index++;
            if (state->meta_index == meta_count) {
                if (!hwa_production_options_valid(
                        &state->result->options) ||
                    hwa_production_read_arrays(
                        state, error, error_size) != 0) return -1;
                state->section = HWA_PRODUCTION_READ_INPUT;
            }
            return 0;
        }
    } else {
        if (state->section == HWA_PRODUCTION_READ_INPUT &&
            state->source_index == state->result->source_count) {
            state->section = HWA_PRODUCTION_READ_SPAN;
        }
        if (state->section == HWA_PRODUCTION_READ_SPAN &&
            state->span_index == state->result->span_count) {
            state->section = HWA_PRODUCTION_READ_FIT;
        }
        if (state->section == HWA_PRODUCTION_READ_FIT &&
            state->fit_index == state->result->fit_count) {
            state->section = HWA_PRODUCTION_READ_EVALUATION;
        }
        if (state->section == HWA_PRODUCTION_READ_EVALUATION &&
            state->evaluation_index ==
                state->result->evaluation_row_count) {
            state->section = HWA_PRODUCTION_READ_VIEW;
        }
        if (state->section == HWA_PRODUCTION_READ_VIEW &&
            state->view_index == state->result->view_row_count) {
            state->section = HWA_PRODUCTION_READ_WARNING;
        }
        if (state->section == HWA_PRODUCTION_READ_WARNING &&
            state->warning_index == state->result->warning_count) {
            state->section = HWA_PRODUCTION_READ_DONE;
        }
        if (state->section == HWA_PRODUCTION_READ_INPUT &&
            strcmp(fields[0], "INPUT") == 0) {
            status = hwa_production_read_source(
                state, fields, field_count);
        } else if (state->section == HWA_PRODUCTION_READ_SPAN &&
                   strcmp(fields[0], "SPAN") == 0) {
            status = hwa_production_read_span(
                state, fields, field_count);
        } else if (state->section == HWA_PRODUCTION_READ_FIT &&
                   strcmp(fields[0], "FIT") == 0) {
            status = hwa_production_read_fit(
                state, fields, field_count);
        } else if (state->section == HWA_PRODUCTION_READ_EVALUATION &&
                   strcmp(fields[0], "EVALUATION") == 0) {
            status = hwa_production_read_evaluation(
                state, fields, field_count);
        } else if (state->section == HWA_PRODUCTION_READ_VIEW &&
                   strcmp(fields[0], "VIEW") == 0) {
            status = hwa_production_read_view(
                state, fields, field_count);
        } else if (state->section == HWA_PRODUCTION_READ_WARNING &&
                   strcmp(fields[0], "WARNING") == 0) {
            status = hwa_production_read_warning(
                state, fields, field_count);
        }
        if (status == 0) return 0;
    }
    hwa_set_error(error, error_size,
                  "invalid production result row %zu", row);
    return -1;
}

typedef int (*HWAProductionRowFunction)(char **fields,
                                        size_t field_count,
                                        size_t row,
                                        void *user,
                                        char *error,
                                        size_t error_size);

static int hwa_production_csv_record(
    const unsigned char *data,
    size_t size,
    size_t row,
    HWAProductionRowFunction function,
    void *user,
    char *error,
    size_t error_size)
{
    char **fields = NULL;
    char *storage = NULL;
    size_t position = 0U;
    size_t output = 0U;
    size_t field_count = 0U;
    int row_done = 0;
    fields = (char **)malloc(
        HWA_PRODUCTION_FILE_MAX_FIELDS * sizeof(*fields));
    storage = (char *)malloc(size + 1U);
    if (fields == NULL || storage == NULL) {
        free(storage);
        free(fields);
        hwa_production_error(
            error, error_size,
            "out of memory while parsing production result");
        return -1;
    }
    while (!row_done) {
        size_t field_start = output;
        int quoted = 0;
        int closed_quote = 0;
        if (field_count == HWA_PRODUCTION_FILE_MAX_FIELDS) {
            hwa_set_error(
                error, error_size,
                "production result row %zu has too many fields", row);
            goto fail;
        }
        fields[field_count++] = storage + output;
        if (position < size && data[position] == (unsigned char)'"') {
            quoted = 1;
            position++;
        }
        while (position < size) {
            unsigned char byte = data[position];
            if (byte == 0U) {
                hwa_set_error(
                    error, error_size,
                    "production result row %zu contains a NUL byte", row);
                goto fail;
            }
            if (quoted) {
                if (byte == (unsigned char)'"') {
                    if (position + 1U < size &&
                        data[position + 1U] == (unsigned char)'"') {
                        storage[output++] = '"';
                        position += 2U;
                        continue;
                    }
                    position++;
                    closed_quote = 1;
                    break;
                }
                if (byte == (unsigned char)'\r') {
                    if (position + 1U >= size ||
                        data[position + 1U] != (unsigned char)'\n') {
                        hwa_set_error(
                            error, error_size,
                            "production result row %zu has a bare carriage return",
                            row);
                        goto fail;
                    }
                    storage[output++] = '\r';
                    storage[output++] = '\n';
                    position += 2U;
                    continue;
                }
                storage[output++] = (char)byte;
                position++;
                continue;
            }
            if (byte == (unsigned char)'"') {
                hwa_set_error(
                    error, error_size,
                    "production result row %zu has an unquoted quote", row);
                goto fail;
            }
            if (byte == (unsigned char)',' ||
                byte == (unsigned char)'\r' ||
                byte == (unsigned char)'\n') break;
            storage[output++] = (char)byte;
            position++;
        }
        if (quoted && !closed_quote) {
            hwa_set_error(
                error, error_size,
                "production result row %zu has an unterminated quote", row);
            goto fail;
        }
        if (output - field_start >
            2U * HWA_PRODUCTION_FILE_MAX_FIELD_BYTES) {
            hwa_set_error(
                error, error_size,
                "production result row %zu has an oversized field", row);
            goto fail;
        }
        {
            size_t byte_index;
            int needs_quote = 0;
            for (byte_index = field_start; byte_index < output;
                 ++byte_index) {
                unsigned char byte =
                    (unsigned char)storage[byte_index];
                if (byte == (unsigned char)',' ||
                    byte == (unsigned char)'"' ||
                    byte == (unsigned char)'\r' ||
                    byte == (unsigned char)'\n') {
                    needs_quote = 1;
                    break;
                }
            }
            if (quoted != needs_quote) {
                hwa_set_error(
                    error, error_size,
                    "production result row %zu has noncanonical quoting",
                    row);
                goto fail;
            }
        }
        storage[output++] = '\0';
        if (quoted && position < size &&
            data[position] != (unsigned char)',' &&
            data[position] != (unsigned char)'\r' &&
            data[position] != (unsigned char)'\n') {
            hwa_set_error(
                error, error_size,
                "production result row %zu has bytes after a quote", row);
            goto fail;
        }
        if (position >= size) {
            row_done = 1;
        } else if (data[position] == (unsigned char)',') {
            position++;
        } else if (data[position] == (unsigned char)'\r') {
            if (position + 1U >= size ||
                data[position + 1U] != (unsigned char)'\n' ||
                position + 2U != size) {
                hwa_set_error(
                    error, error_size,
                    "production result row %zu has bad line ending", row);
                goto fail;
            }
            position += 2U;
            row_done = 1;
        } else {
            if (position + 1U != size) {
                hwa_set_error(
                    error, error_size,
                    "production result row %zu has trailing bytes", row);
                goto fail;
            }
            position++;
            row_done = 1;
        }
    }
    if (function(fields, field_count, row, user,
                 error, error_size) != 0) goto fail;
    free(storage);
    free(fields);
    return 0;

fail:
    free(storage);
    free(fields);
    return -1;
}

static void hwa_production_hash_input_byte(
    HWASha256 *context,
    unsigned char buffer[HWA_PRODUCTION_HASH_BUFFER_BYTES],
    size_t *buffer_size,
    unsigned char byte)
{
    buffer[(*buffer_size)++] = byte;
    if (*buffer_size == HWA_PRODUCTION_HASH_BUFFER_BYTES) {
        hwa_sha256_update(context, buffer, *buffer_size);
        *buffer_size = 0U;
    }
}

static int hwa_production_record_capacity(size_t field_count,
                                          size_t field_bytes,
                                          size_t *capacity)
{
    size_t escaped_field_bytes;
    if (capacity == NULL || field_bytes > (SIZE_MAX - 3U) / 2U) return -1;
    escaped_field_bytes = field_bytes * 2U + 3U;
    if (field_count > (SIZE_MAX - 2U) / escaped_field_bytes) return -1;
    *capacity = field_count * escaped_field_bytes + 2U;
    return 0;
}

static int hwa_production_stream_rows(
    const char *path,
    const HWAProductionOptions *limits,
    const HWAProductionFileIdentity *expected,
    char stream_sha256[HWA_SHA256_HEX_SIZE],
    HWAProductionReadState *state,
    char *error,
    size_t error_size)
{
    FILE *stream = NULL;
    HWASha256 hash_context;
    unsigned char hash_buffer[HWA_PRODUCTION_HASH_BUFFER_BYTES];
    unsigned char hash_digest[32];
    unsigned char *record = NULL;
    size_t hash_buffer_size = 0U;
    size_t record_size = 0U;
    size_t maximum_record;
    size_t row = 1U;
    uint64_t source_bytes = 0U;
    uint64_t parser_bytes;
    uint64_t row_scratch;
    int in_quotes = 0;
    int field_start = 1;
    int status = -1;
    HWAProductionFileIdentity before;
    HWAProductionFileIdentity opened;
    stream_sha256[0] = '\0';
    if (hwa_production_record_capacity(HWA_PRODUCTION_FILE_MAX_FIELDS,
                                       HWA_PRODUCTION_FILE_MAX_FIELD_BYTES,
                                       &maximum_record) != 0) {
        hwa_production_error(
            error, error_size,
            "production result record limit overflows");
        return -1;
    }
    parser_bytes = (uint64_t)maximum_record +
                   HWA_PRODUCTION_HASH_BUFFER_BYTES;
    if (hwa_production_read_charge(state, parser_bytes) != 0) {
        hwa_production_error(
            error, error_size,
            "production row buffer exceeds the work-byte limit");
        return -1;
    }
    record = (unsigned char *)malloc(maximum_record);
    if (record == NULL) {
        state->work_live -= parser_bytes;
        hwa_production_error(
            error, error_size,
            "out of memory for production row buffer");
        return -1;
    }
    if (hwa_production_path_identity(
            path, &before, error, error_size) != 0 ||
        !hwa_production_same_identity(expected, &before)) {
        hwa_production_error(
            error, error_size,
            "production result changed before it was opened");
        goto cleanup;
    }
    if (before.size > limits->max_input_bytes) {
        hwa_production_error(
            error, error_size,
            "production result exceeds the current byte limit");
        goto cleanup;
    }
    stream = fopen(path, "rb");
    if (stream == NULL) {
        hwa_set_error(error, error_size,
                      "cannot open production result '%s': %s",
                      path, strerror(errno));
        goto cleanup;
    }
    if (hwa_production_stream_identity(stream, &opened) != 0 ||
        !hwa_production_same_identity(expected, &opened)) {
        hwa_production_error(
            error, error_size,
            "production result changed before it was opened");
        goto cleanup;
    }
    hwa_sha256_init(&hash_context);
    for (;;) {
        int byte = fgetc(stream);
        int row_done = 0;
        if (byte == EOF) {
            if (ferror(stream)) {
                hwa_production_error(
                    error, error_size,
                    "cannot read production result");
                goto cleanup;
            }
            if (in_quotes) {
                hwa_production_error(
                    error, error_size,
                    "production result has an unterminated quote");
                goto cleanup;
            }
            if (record_size != 0U) {
                hwa_production_error(
                    error, error_size,
                    "production result final row lacks CRLF");
                goto cleanup;
            }
        } else {
            hwa_production_hash_input_byte(
                &hash_context, hash_buffer, &hash_buffer_size,
                (unsigned char)byte);
            if (source_bytes == limits->max_input_bytes ||
                record_size == maximum_record) {
                hwa_production_error(
                    error, error_size,
                    "production result exceeds a byte or row limit");
                goto cleanup;
            }
            source_bytes++;
            record[record_size++] = (unsigned char)byte;
            if (in_quotes && byte == '"') {
                int next = fgetc(stream);
                if (next == '"') {
                    if (source_bytes == limits->max_input_bytes ||
                        record_size == maximum_record) {
                        hwa_production_error(
                            error, error_size,
                            "production result row exceeds limits");
                        goto cleanup;
                    }
                    source_bytes++;
                    record[record_size++] = (unsigned char)next;
                    hwa_production_hash_input_byte(
                        &hash_context, hash_buffer, &hash_buffer_size,
                        (unsigned char)next);
                } else {
                    in_quotes = 0;
                    if (next != EOF && ungetc(next, stream) == EOF) {
                        hwa_production_error(
                            error, error_size,
                            "cannot parse production result");
                        goto cleanup;
                    }
                }
            } else if (!in_quotes && field_start && byte == '"') {
                in_quotes = 1;
                field_start = 0;
            } else if (!in_quotes && byte == ',') {
                field_start = 1;
            } else if (!in_quotes && byte == '\n') {
                hwa_production_error(
                    error, error_size,
                    "production result has a bare line feed");
                goto cleanup;
            } else if (!in_quotes && byte == '\r') {
                int next = fgetc(stream);
                if (next != '\n' ||
                    source_bytes == limits->max_input_bytes ||
                    record_size == maximum_record) {
                    hwa_production_error(
                        error, error_size,
                        "production result has a bare carriage return");
                    goto cleanup;
                }
                source_bytes++;
                record[record_size++] = (unsigned char)next;
                hwa_production_hash_input_byte(
                    &hash_context, hash_buffer, &hash_buffer_size,
                    (unsigned char)next);
                row_done = 1;
                field_start = 1;
            } else if (!in_quotes) {
                field_start = 0;
            }
        }
        if (row_done) {
            row_scratch = (uint64_t)record_size + 1U +
                          (uint64_t)HWA_PRODUCTION_FILE_MAX_FIELDS *
                              (uint64_t)sizeof(char *);
            if (hwa_production_read_charge(state, row_scratch) != 0) {
                hwa_production_error(
                    error, error_size,
                    "production row parse exceeds the work-byte limit");
                goto cleanup;
            }
            if (hwa_production_csv_record(
                    record, record_size, row, hwa_production_read_row,
                    state, error, error_size) != 0) {
                state->work_live -= row_scratch;
                goto cleanup;
            }
            state->work_live -= row_scratch;
            record_size = 0U;
            row++;
        }
        if (byte == EOF) break;
    }
    if (source_bytes != before.size) {
        hwa_production_error(
            error, error_size,
            "production result size changed while reading");
        goto cleanup;
    }
    if (hash_buffer_size != 0U) {
        hwa_sha256_update(
            &hash_context, hash_buffer, hash_buffer_size);
    }
    if (hash_context.overflowed) {
        hwa_production_error(
            error, error_size,
            "production result is too large to hash");
        goto cleanup;
    }
    hwa_sha256_final(&hash_context, hash_digest);
    hwa_sha256_hex(hash_digest, stream_sha256);
    if (fclose(stream) != 0) {
        stream = NULL;
        hwa_production_error(
            error, error_size,
            "cannot close production result after reading");
        goto cleanup;
    }
    stream = NULL;
    status = 0;

cleanup:
    if (stream != NULL) (void)fclose(stream);
    free(record);
    state->work_live -= parser_bytes;
    return status;
}

static int hwa_production_read_complete(
    const HWAProductionReadState *state)
{
    return state->meta_index ==
               sizeof(hwa_production_meta_keys) /
                   sizeof(hwa_production_meta_keys[0]) &&
           state->source_index == state->result->source_count &&
           state->span_index == state->result->span_count &&
           state->fit_index == state->result->fit_count &&
           state->evaluation_index ==
               state->result->evaluation_row_count &&
           state->view_index == state->result->view_row_count &&
           state->warning_index == state->result->warning_count;
}

static void hwa_production_use_current_limits(
    HWAProductionOptions *options,
    const HWAProductionOptions *current)
{
    options->max_input_bytes = current->max_input_bytes;
    options->max_input_frames = current->max_input_frames;
    options->max_ir_frames = current->max_ir_frames;
    options->max_work_bytes = current->max_work_bytes;
    options->max_evaluations = current->max_evaluations;
    options->max_spans = current->max_spans;
    options->max_envelope_points = current->max_envelope_points;
    options->max_fits = current->max_fits;
    options->max_evaluation_rows = current->max_evaluation_rows;
    options->max_view_rows = current->max_view_rows;
    options->max_warnings = current->max_warnings;
    options->profile_limits = current->profile_limits;
}

static int hwa_production_file_read_impl(
    const char *path,
    const HWAProductionOptions *limits,
    HWAProductionResult *result,
    char file_sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    HWAProductionOptions current;
    HWAProductionReadState state;
    HWAProductionFileIdentity input_identity;
    HWAProductionFileIdentity final_identity;
    char parsed[HWA_SHA256_HEX_SIZE];
    char after[HWA_SHA256_HEX_SIZE];
    uint64_t saved_retained;
    uint64_t saved_max_work;
    int status = -1;
    if (result == NULL) {
        hwa_production_error(
            error, error_size,
            "production result pointer is null");
        return -1;
    }
    if (limits != NULL) current = *limits;
    memset(result, 0, sizeof(*result));
    if (file_sha256 != NULL) file_sha256[0] = '\0';
    if (path == NULL || path[0] == '\0' || strcmp(path, "-") == 0 ||
        limits == NULL || !hwa_production_options_valid(&current) ||
        file_sha256 == NULL) {
        hwa_production_error(
            error, error_size,
            "invalid production result reader arguments");
        return -1;
    }
    if (hwa_production_path_identity(
            path, &input_identity, error, error_size) != 0) return -1;
    memset(&state, 0, sizeof(state));
    state.limits = &current;
    state.result = result;
    state.section = HWA_PRODUCTION_READ_MAGIC;
    if (hwa_production_stream_rows(
            path, &current, &input_identity, parsed, &state,
            error, error_size) != 0 ||
        !hwa_production_read_complete(&state)) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_production_error(
                error, error_size,
                "production result is incomplete");
        }
        goto cleanup;
    }
    saved_retained = result->retained_work_bytes;
    saved_max_work = result->options.max_work_bytes;
    if (saved_retained == 0U || saved_retained > saved_max_work) {
        hwa_production_error(
            error, error_size,
            "invalid saved production retained-work byte count");
        goto cleanup;
    }
    result->retained_work_bytes = state.work_live;
    result->options.max_work_bytes = current.max_work_bytes;
    if (hwa_production_result_validate_with_ulps(
            result, HWA_PRODUCTION_READER_MAX_ULPS,
            error, error_size) != 0 ||
        hwa_production_evaluation_rows_normalize(
            result, error, error_size) != 0 ||
        hwa_production_view_rows_rebuild(
            result, error, error_size) != 0) goto cleanup;
    hwa_production_use_current_limits(&result->options, &current);
    if (hwa_production_result_validate(
            result, error, error_size) != 0) goto cleanup;
    if (hwa_production_path_identity(
            path, &final_identity, error, error_size) != 0 ||
        !hwa_production_same_identity(
            &input_identity, &final_identity) ||
        state.work_live > current.max_work_bytes ||
        HWA_PRODUCTION_HASH_BUFFER_BYTES >
            current.max_work_bytes - state.work_live ||
        hwa_production_sha256_for_identity(
            path, &input_identity,
            current.max_input_bytes,
            after, error, error_size) != 0 ||
        strcmp(parsed, after) != 0) {
        hwa_production_error(
            error, error_size,
            "production result changed while it was read");
        goto cleanup;
    }
    memcpy(file_sha256, parsed, HWA_SHA256_HEX_SIZE);
    status = 0;

cleanup:
    if (status != 0) {
        hwa_production_result_free(result);
        file_sha256[0] = '\0';
    }
    return status;
}

int hwa_production_file_read(
    const char *path,
    const HWAProductionOptions *limits,
    HWAProductionResult *result,
    char file_sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    HWANumericLocale locale;
    int status;
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        if (result != NULL) memset(result, 0, sizeof(*result));
        if (file_sha256 != NULL) file_sha256[0] = '\0';
        hwa_production_error(
            error, error_size,
            "cannot enter the C numeric locale for production input");
        return -1;
    }
    status = hwa_production_file_read_impl(
        path, limits, result, file_sha256, error, error_size);
    if (hwa_c_numeric_locale_end(&locale) != 0) {
        if (status == 0 && result != NULL) {
            hwa_production_result_free(result);
        }
        if (file_sha256 != NULL) file_sha256[0] = '\0';
        if (status == 0 || error == NULL || error_size == 0U ||
            error[0] == '\0') {
            hwa_production_error(
                error, error_size,
                "cannot restore the numeric locale after production input");
        }
        return -1;
    }
    return status;
}

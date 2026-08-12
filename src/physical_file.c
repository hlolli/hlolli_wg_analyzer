#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "physical_file.h"

#include "alignment_file.h"
#include "internal.h"
#include "numeric_locale.h"
#include "physical_check.h"
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

#define HWA_PHYSICAL_FILE_MAX_FIELDS 24U
#define HWA_PHYSICAL_FILE_MAX_FIELD_BYTES 65536U
#define HWA_PHYSICAL_HASH_BUFFER_BYTES 65536U
#define HWA_PHYSICAL_READER_SCORE_MAX_ULPS 32U
#define HWA_PHYSICAL_EVIDENCE_ALL ((UINT32_C(1) << 10) - 1U)
#define HWA_PHYSICAL_QUALITY_ALL ((UINT32_C(1) << 7) - 1U)

typedef struct HWAPhysicalMetaKey {
    const char *key;
    const char *unit;
} HWAPhysicalMetaKey;

typedef struct HWAPhysicalFileIdentity {
    uint64_t device;
    uint64_t file;
    uint64_t size;
} HWAPhysicalFileIdentity;

static const HWAPhysicalMetaKey hwa_physical_meta_keys[] = {
    {"tool_version", ""},
    {"physical_check_method_version", ""},
    {"build_compiler_family", ""},
    {"build_compiler_version", ""},
    {"build_c_standard", ""},
    {"build_target_os", ""},
    {"build_pointer_bits", "bits"},
    {"build_endianness", ""},
    {"build_mode", ""},
    {"decode_block_frames", "frames"},
    {"fft_size", "samples"},
    {"hop_size", "samples"},
    {"spectral_floor_dbfs", "dBFS"},
    {"max_wave_bytes", "bytes"},
    {"max_wave_frames", "frames"},
    {"max_work_bytes", "bytes"},
    {"max_pair_evaluations", "evaluations"},
    {"max_bindings", "bindings"},
    {"max_transforms", "transforms"},
    {"max_modes", "modes"},
    {"max_checks", "checks"},
    {"max_findings", "findings"},
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
    {"pair_evaluations", "evaluations"},
    {"transform_count", "transforms"},
    {"source_count", "sources"},
    {"check_count", "checks"},
    {"finding_count", "findings"},
    {"warning_count", "warnings"}
};

static void hwa_physical_error(char *error,
                               size_t error_size,
                               const char *message)
{
    if (error != NULL && error_size > 0U) {
        (void)snprintf(error, error_size, "%s", message);
    }
}

#if defined(_WIN32)
static void hwa_physical_windows_identity_from_information(
    const BY_HANDLE_FILE_INFORMATION *information,
    HWAPhysicalFileIdentity *identity)
{
    identity->device = (uint64_t)information->dwVolumeSerialNumber;
    identity->file =
        ((uint64_t)information->nFileIndexHigh << 32U) |
        (uint64_t)information->nFileIndexLow;
    identity->size =
        ((uint64_t)information->nFileSizeHigh << 32U) |
        (uint64_t)information->nFileSizeLow;
}

static int hwa_physical_path_identity(
    const char *path,
    HWAPhysicalFileIdentity *identity,
    char *error,
    size_t error_size)
{
    BY_HANDLE_FILE_INFORMATION information;
    HANDLE handle = CreateFileA(
        path, 0U,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(handle, &information)) {
        DWORD windows_error = GetLastError();
        if (handle != INVALID_HANDLE_VALUE) (void)CloseHandle(handle);
        hwa_set_error(error, error_size,
                      "cannot inspect physical result: Windows error %lu",
                      (unsigned long)windows_error);
        return -1;
    }
    (void)CloseHandle(handle);
    if ((information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        hwa_physical_error(error, error_size,
                           "physical result is not a regular file");
        return -1;
    }
    hwa_physical_windows_identity_from_information(&information, identity);
    return 0;
}

static int hwa_physical_stream_identity(
    FILE *stream,
    HWAPhysicalFileIdentity *identity)
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
    hwa_physical_windows_identity_from_information(&information, identity);
    return 0;
}

#else
static int hwa_physical_path_identity(
    const char *path,
    HWAPhysicalFileIdentity *identity,
    char *error,
    size_t error_size)
{
    struct stat facts;
    if (stat(path, &facts) != 0 || !S_ISREG(facts.st_mode) ||
        facts.st_size < 0) {
        hwa_physical_error(error, error_size,
                           "cannot inspect physical result");
        return -1;
    }
    identity->device = (uint64_t)facts.st_dev;
    identity->file = (uint64_t)facts.st_ino;
    identity->size = (uint64_t)facts.st_size;
    return 0;
}

static int hwa_physical_stream_identity(
    FILE *stream,
    HWAPhysicalFileIdentity *identity)
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

static int hwa_physical_same_identity(
    const HWAPhysicalFileIdentity *left,
    const HWAPhysicalFileIdentity *right)
{
    return left->device == right->device && left->file == right->file &&
           left->size == right->size;
}

static int hwa_physical_string_valid(const char *text, int allow_empty)
{
    size_t length;
    if (text == NULL || (!allow_empty && text[0] == '\0')) return 0;
    length = strlen(text);
    return length <= HWA_PHYSICAL_FILE_MAX_FIELD_BYTES;
}

static int hwa_physical_path_valid(const char *path)
{
    return path != NULL && path[0] != '\0' &&
           strlen(path) <= HWA_PHYSICAL_FILE_MAX_FIELD_BYTES / 2U;
}

static int hwa_physical_sha_valid(const char *text)
{
    size_t index;
    if (text == NULL || strlen(text) != HWA_SHA256_HEX_SIZE - 1U) return 0;
    for (index = 0U; index < HWA_SHA256_HEX_SIZE - 1U; ++index) {
        unsigned char byte = (unsigned char)text[index];
        if (!((byte >= (unsigned char)'0' && byte <= (unsigned char)'9') ||
              (byte >= (unsigned char)'a' && byte <= (unsigned char)'f'))) {
            return 0;
        }
    }
    return 1;
}

static int hwa_physical_power_of_two(size_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static int hwa_physical_profile_limits_valid(
    const HWAProfileComparisonOptions *limits)
{
    return limits->max_input_bytes != 0U && limits->max_work_bytes != 0U &&
           limits->max_contexts != 0U && limits->max_measurements != 0U &&
           limits->max_groups != 0U && limits->max_group_members != 0U &&
           limits->max_statistics != 0U && limits->max_warnings != 0U &&
           limits->max_distributions != 0U && limits->max_gaps != 0U;
}

static int hwa_physical_options_valid(const HWAPhysicalOptions *options)
{
    return options != NULL && options->decode_block_frames != 0U &&
           hwa_physical_power_of_two(options->fft_size) &&
           options->fft_size >= 256U && options->fft_size <= 1048576U &&
           options->hop_size != 0U && options->hop_size <= options->fft_size &&
           isfinite(options->spectral_floor_dbfs) &&
           options->spectral_floor_dbfs >= -300.0 &&
           options->spectral_floor_dbfs <= 0.0 &&
           options->max_wave_bytes != 0U &&
           options->max_wave_frames != 0U &&
           options->max_work_bytes != 0U &&
           options->max_pair_evaluations != 0U &&
           options->max_bindings != 0U &&
           options->max_transforms != 0U && options->max_modes != 0U &&
           options->max_checks != 0U && options->max_findings != 0U &&
           options->max_warnings != 0U &&
           hwa_physical_profile_limits_valid(&options->profile_limits);
}

static int hwa_physical_zero_format(const HWAFormat *format)
{
    return format->container == 0 && format->encoding == 0 &&
           format->channels == 0U && format->sample_rate_hz == 0U &&
           format->bits_per_sample == 0U &&
           format->valid_bits_per_sample == 0U &&
           format->block_align == 0U && format->channel_mask == 0U &&
           format->frames == 0U && format->data_bytes == 0U &&
           format->duration_seconds == 0.0;
}

static int hwa_physical_wave_format_valid(const HWAFormat *format)
{
    double duration;
    double tolerance;
    uint64_t block_align;
    uint64_t data_bytes;
    int bits_valid = format->encoding == HWA_ENCODING_PCM ?
        (format->bits_per_sample == 8U ||
         format->bits_per_sample == 16U ||
         format->bits_per_sample == 24U ||
         format->bits_per_sample == 32U) :
        (format->bits_per_sample == 32U ||
         format->bits_per_sample == 64U);
    if ((format->container != HWA_CONTAINER_RIFF &&
         format->container != HWA_CONTAINER_RF64) ||
        (format->encoding != HWA_ENCODING_PCM &&
         format->encoding != HWA_ENCODING_IEEE_FLOAT) ||
        format->channels == 0U || format->channels > 64U ||
        format->sample_rate_hz == 0U || !bits_valid ||
        format->valid_bits_per_sample == 0U ||
        format->valid_bits_per_sample > format->bits_per_sample ||
        (format->encoding == HWA_ENCODING_IEEE_FLOAT &&
         format->valid_bits_per_sample != format->bits_per_sample) ||
        format->block_align == 0U || !isfinite(format->duration_seconds) ||
        format->duration_seconds < 0.0) {
        return 0;
    }
    block_align = (uint64_t)format->channels *
                  ((uint64_t)format->bits_per_sample / 8U);
    if (block_align != format->block_align ||
        format->frames > UINT64_MAX / block_align) return 0;
    data_bytes = format->frames * block_align;
    if (data_bytes != format->data_bytes) return 0;
    duration = (double)format->frames / (double)format->sample_rate_hz;
    tolerance = 1e-12 * fmax(1.0, duration);
    return fabs(format->duration_seconds - duration) <= tolerance;
}

static int hwa_physical_case_valid(const char *text)
{
    return text != NULL && strchr(text, ':') == NULL &&
           hwa_physical_string_valid(text, 1);
}

static int hwa_physical_check_values_valid(const HWAPhysicalCheck *check)
{
    double expected_delta = 0.0;
    if (!isfinite(check->reference_value) || !isfinite(check->model_value) ||
        !isfinite(check->delta) || !isfinite(check->confidence) ||
        check->confidence < 0.0 || check->confidence > 1.0) return 0;
    if ((!check->reference_valid && check->reference_value != 0.0) ||
        (!check->model_valid && check->model_value != 0.0) ||
        (!check->delta_valid && check->delta != 0.0)) return 0;
    if (check->delta_valid &&
        (!check->reference_valid || !check->model_valid)) return 0;
    if (check->delta_valid) {
        expected_delta = check->model_value - check->reference_value;
        if (check->delta != expected_delta) return 0;
    }
    if (check->availability == HWA_PHYSICAL_AVAILABLE) {
        if (!check->reference_valid && !check->model_valid) return 0;
    } else if (check->reference_valid || check->model_valid ||
               check->delta_valid || check->confidence != 0.0) {
        return 0;
    }
    return 1;
}

static double hwa_physical_relative_gap(double reference, double model)
{
    return fabs(model - reference) / fmax(fabs(reference), 1.0e-9);
}

static HWAPhysicalSeverity hwa_physical_severity_for_score(double score)
{
    return score >= 0.80 ? HWA_PHYSICAL_SEVERITY_CRITICAL :
           score >= 0.40 ? HWA_PHYSICAL_SEVERITY_WARNING :
                           HWA_PHYSICAL_SEVERITY_INFO;
}

int hwa_physical_scored_finding_for_check(
    const HWAPhysicalCheck *check,
    HWAPhysicalFindingClass *finding_class,
    HWAPhysicalSeverity *severity,
    const char **code,
    const char **message,
    double *score)
{
    double value;
    double raw = 0.0;
    int fixed_frequency_threshold = 0;
    if (check == NULL || finding_class == NULL || severity == NULL ||
        code == NULL || message == NULL || score == NULL ||
        check->availability != HWA_PHYSICAL_AVAILABLE) return 0;
    value = check->model_valid ? check->model_value :
            check->reference_valid ? check->reference_value : 0.0;
    *finding_class = check->kind >= HWA_PHYSICAL_DC_OFFSET
                         ? HWA_PHYSICAL_FINDING_FAULT
                         : HWA_PHYSICAL_FINDING_GAP;
    switch (check->kind) {
    case HWA_PHYSICAL_ELEMENT_TRAIT_DELTA:
        raw = check->delta_valid ? fabs(check->delta) / 2.0 : 0.0;
        break;
    case HWA_PHYSICAL_ELEMENT_DISTINCTNESS_RATIO:
        raw = check->delta_valid
                  ? fabs(check->delta) /
                        fmax(fabs(check->reference_value), 0.25)
                  : 0.0;
        break;
    case HWA_PHYSICAL_ELEMENT_GAIN_ONLY_SCORE:
        raw = check->delta_valid ? fabs(check->delta) : 0.0;
        break;
    case HWA_PHYSICAL_ELEMENT_CARRYOVER_DB:
        raw = check->delta_valid ? fabs(check->delta) / 12.0 : 0.0;
        break;
    case HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ:
        if (check->delta_valid && check->reference_value > 0.0 &&
            check->model_value > 0.0) {
            double threshold;
            int outside = 0;
            if (check->model_value >= check->reference_value) {
                threshold = check->reference_value *
                            0x1.02f97ddc18368p+0;
                outside = isfinite(threshold) &&
                          check->model_value >= threshold;
            } else {
                threshold = check->reference_value *
                            0x1.fa1e827a1b38cp-1;
                outside = check->model_value <= threshold;
            }
            if (outside) {
                raw = fabs(log2(check->model_value) -
                           log2(check->reference_value)) * 12.0;
                fixed_frequency_threshold = 1;
            }
        }
        break;
    case HWA_PHYSICAL_BODY_MODE_BANDWIDTH_HZ:
    case HWA_PHYSICAL_BODY_MODE_Q:
    case HWA_PHYSICAL_BODY_MODE_DECAY_SECONDS:
    case HWA_PHYSICAL_BODY_MODE_DENSITY_PER_KHZ:
        raw = check->delta_valid
                  ? hwa_physical_relative_gap(
                        check->reference_value, check->model_value)
                  : 0.0;
        break;
    case HWA_PHYSICAL_BODY_MODE_PROMINENCE_DB:
        raw = check->delta_valid ? fabs(check->delta) / 12.0 : 0.0;
        break;
    case HWA_PHYSICAL_BODY_MODE_DISTANCE_CENTS:
        raw = check->model_valid ? fabs(check->model_value) / 100.0 : 0.0;
        break;
    case HWA_PHYSICAL_JOINT_RESIDUAL_DB:
        raw = check->delta_valid ? fabs(check->delta) / 20.0 : 0.0;
        break;
    case HWA_PHYSICAL_SHARED_GAIN_DB:
        raw = check->delta_valid ? fabs(check->delta) / 6.0 : 0.0;
        break;
    case HWA_PHYSICAL_INTERMODULATION_RATIO:
    case HWA_PHYSICAL_BEATING_DEPTH_RATIO:
    case HWA_PHYSICAL_ROUGHNESS_RATIO:
        raw = check->delta_valid ? fabs(check->delta) / 0.25 : 0.0;
        break;
    case HWA_PHYSICAL_SUM_TONE_DB:
    case HWA_PHYSICAL_DIFFERENCE_TONE_DB:
        raw = check->delta_valid ? fabs(check->delta) / 20.0 : 0.0;
        break;
    case HWA_PHYSICAL_BEATING_RATE_HZ:
        raw = check->delta_valid ? fabs(check->delta) / 10.0 : 0.0;
        break;
    case HWA_PHYSICAL_PITCH_PULL_CENTS:
        raw = check->delta_valid ? fabs(check->delta) / 25.0 : 0.0;
        break;
    case HWA_PHYSICAL_RENDER_RMS_ERROR_DB:
    case HWA_PHYSICAL_RENDER_SPECTRAL_DISTANCE_DB:
    case HWA_PHYSICAL_RENDER_MAX_ERROR_DBFS:
        raw = check->delta_valid ? fabs(check->delta) / 12.0 : 0.0;
        break;
    case HWA_PHYSICAL_RENDER_CORRELATION:
        raw = check->delta_valid ? fabs(check->delta) / 0.20 : 0.0;
        break;
    case HWA_PHYSICAL_RENDER_LAG_SAMPLES:
        raw = check->delta_valid ? fabs(check->delta) / 32.0 : 0.0;
        break;
    case HWA_PHYSICAL_RENDER_PITCH_DELTA_CENTS:
        raw = check->delta_valid ? fabs(check->delta) / 25.0 : 0.0;
        break;
    case HWA_PHYSICAL_RENDER_ATTACK_DELTA_SECONDS:
    case HWA_PHYSICAL_RENDER_DECAY_DELTA_SECONDS:
        raw = check->delta_valid ? fabs(check->delta) / 0.05 : 0.0;
        break;
    case HWA_PHYSICAL_DC_OFFSET:
    case HWA_PHYSICAL_DC_DRIFT:
        raw = fabs(value) / 0.01;
        break;
    case HWA_PHYSICAL_CLIP_FRACTION:
    case HWA_PHYSICAL_HARD_BOUND_FRACTION:
        raw = value / 0.001;
        break;
    case HWA_PHYSICAL_REPEATED_BLOCK_FRACTION:
    case HWA_PHYSICAL_STUCK_STATE_FRACTION:
        raw = value / 0.10;
        break;
    case HWA_PHYSICAL_MAX_STEP_DBFS:
        raw = value > -12.0 ? (value + 12.0) / 12.0 : 0.0;
        break;
    case HWA_PHYSICAL_RUNAWAY_SLOPE_DB_PER_SECOND:
        raw = value > 0.0 ? value / 6.0 : 0.0;
        break;
    case HWA_PHYSICAL_RETURN_LEVEL_DBFS:
        raw = value > -60.0 ? (value + 60.0) / 60.0 : 0.0;
        break;
    case HWA_PHYSICAL_HIGH_BAND_RATIO:
    case HWA_PHYSICAL_SUBHARMONIC_RATIO:
    case HWA_PHYSICAL_FIXED_TONE_PROMINENCE_DB:
        return 0;
    case HWA_PHYSICAL_DENORMAL_FRACTION:
        raw = value / 1.0e-6;
        break;
    default:
        return 0;
    }
    if (isnan(raw) || raw < 0.0) return 0;
    if (isinf(raw)) raw = 1.0;
    if (!fixed_frequency_threshold && raw < 0.20) return 0;
    if (fixed_frequency_threshold && raw < 0.20) raw = 0.20;
    *score = fmin(1.0, raw);
    *severity = hwa_physical_severity_for_score(*score);
    *code = *finding_class == HWA_PHYSICAL_FINDING_FAULT
                ? "numeric-fault" : "physical-gap";
    *message = *finding_class == HWA_PHYSICAL_FINDING_FAULT
                   ? "A raw output fault exceeds its fixed review threshold."
                   : "A physical check exceeds its fixed review threshold.";
    return 1;
}

static int hwa_physical_value_in_range(double value,
                                       double minimum,
                                       double maximum)
{
    return value >= minimum && value <= maximum;
}

static int hwa_physical_check_value_domain(HWAPhysicalCheckKind kind,
                                           double value)
{
    switch (kind) {
    case HWA_PHYSICAL_ELEMENT_REFERENCE_DISTANCE:
    case HWA_PHYSICAL_ELEMENT_MODEL_DISTANCE:
    case HWA_PHYSICAL_ELEMENT_DISTINCTNESS_RATIO:
    case HWA_PHYSICAL_INTERMODULATION_RATIO:
    case HWA_PHYSICAL_BEATING_RATE_HZ:
    case HWA_PHYSICAL_RENDER_SPECTRAL_DISTANCE_DB:
    case HWA_PHYSICAL_FIXED_TONE_PROMINENCE_DB:
        return value >= 0.0;
    case HWA_PHYSICAL_ELEMENT_GAIN_ONLY_SCORE:
    case HWA_PHYSICAL_ELEMENT_PITCH_ONLY_SCORE:
    case HWA_PHYSICAL_BEATING_DEPTH_RATIO:
    case HWA_PHYSICAL_ROUGHNESS_RATIO:
    case HWA_PHYSICAL_CLIP_FRACTION:
    case HWA_PHYSICAL_HARD_BOUND_FRACTION:
    case HWA_PHYSICAL_REPEATED_BLOCK_FRACTION:
    case HWA_PHYSICAL_STUCK_STATE_FRACTION:
    case HWA_PHYSICAL_HIGH_BAND_RATIO:
    case HWA_PHYSICAL_SUBHARMONIC_RATIO:
    case HWA_PHYSICAL_DENORMAL_FRACTION:
        return hwa_physical_value_in_range(value, 0.0, 1.0);
    case HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ:
    case HWA_PHYSICAL_BODY_MODE_BANDWIDTH_HZ:
    case HWA_PHYSICAL_BODY_MODE_Q:
    case HWA_PHYSICAL_BODY_MODE_DECAY_SECONDS:
        return value > 0.0;
    case HWA_PHYSICAL_BODY_MODE_PROMINENCE_DB:
    case HWA_PHYSICAL_BODY_MODE_DENSITY_PER_KHZ:
    case HWA_PHYSICAL_BODY_MODE_DISTANCE_CENTS:
        return value >= 0.0;
    case HWA_PHYSICAL_BODY_MODE_PAN:
    case HWA_PHYSICAL_RENDER_CORRELATION:
        return hwa_physical_value_in_range(value, -1.0, 1.0);
    default:
        return 1;
    }
}

static const HWAPhysicalSource *hwa_physical_source_for_role(
    const HWAPhysicalCheckSet *set,
    const char *role)
{
    size_t low = 2U;
    size_t high = set->source_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        int order = strcmp(set->sources[middle].role, role);
        if (order < 0) {
            low = middle + 1U;
        } else if (order > 0) {
            high = middle;
        } else {
            return &set->sources[middle];
        }
    }
    return NULL;
}

static int hwa_physical_source_role_exists(
    const HWAPhysicalCheckSet *set,
    const char *prefix,
    const char *case_id);

static int hwa_physical_case_side_complete(
    const HWAPhysicalCheckSet *set,
    int phase,
    int reference,
    const char *case_id)
{
    if (phase == 2) {
        return hwa_physical_source_role_exists(
            set, reference ? "reference:body:" : "model:body:", case_id);
    }
    if (phase == 3) {
        return hwa_physical_source_role_exists(
                   set, reference ? "reference:joint:" : "model:joint:",
                   case_id) &&
               hwa_physical_source_role_exists(
                   set, reference ? "reference:isolated-a:" :
                                    "model:isolated-a:", case_id) &&
               hwa_physical_source_role_exists(
                   set, reference ? "reference:isolated-b:" :
                                    "model:isolated-b:", case_id);
    }
    return hwa_physical_source_role_exists(
               set, reference ? "reference:render-baseline:" :
                                "model:render-baseline:", case_id) &&
           hwa_physical_source_role_exists(
               set, reference ? "reference:render-variant:" :
                                "model:render-variant:", case_id);
}

static int hwa_physical_check_shape_valid(
    const HWAPhysicalCheckSet *set,
    const HWAPhysicalCheck *check)
{
    int pair = check->reference_valid && check->model_valid;
    int relation_phase = check->kind >= HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ &&
                                 check->kind <=
                                     HWA_PHYSICAL_BODY_MODE_DISTANCE_CENTS
                             ? 2
                         : check->kind >= HWA_PHYSICAL_JOINT_RESIDUAL_DB &&
                                   check->kind <= HWA_PHYSICAL_PITCH_PULL_CENTS
                             ? 3
                         : check->kind >= HWA_PHYSICAL_RENDER_RMS_ERROR_DB &&
                                   check->kind <=
                                       HWA_PHYSICAL_RENDER_SPECTRAL_DISTANCE_DB
                             ? 4 : 0;
    if ((check->reference_valid &&
         !hwa_physical_check_value_domain(
             check->kind, check->reference_value)) ||
        (check->model_valid &&
         !hwa_physical_check_value_domain(
             check->kind, check->model_value)) ||
        (pair && !check->delta_valid)) return 0;
    if (relation_phase != 0 &&
        ((check->reference_valid &&
          !hwa_physical_case_side_complete(
              set, relation_phase, 1, check->case_id)) ||
         (check->model_valid &&
          !hwa_physical_case_side_complete(
              set, relation_phase, 0, check->case_id)))) return 0;
    switch (check->kind) {
    case HWA_PHYSICAL_ELEMENT_REFERENCE_DISTANCE:
        return strcmp(check->scope, "reference-profile") == 0 &&
               (check->availability != HWA_PHYSICAL_AVAILABLE ||
                (check->reference_valid && !check->model_valid &&
                 !check->delta_valid));
    case HWA_PHYSICAL_ELEMENT_MODEL_DISTANCE:
        return strcmp(check->scope, "model-profile") == 0 &&
               (check->availability != HWA_PHYSICAL_AVAILABLE ||
                (!check->reference_valid && check->model_valid &&
                 !check->delta_valid));
    case HWA_PHYSICAL_ELEMENT_TRAIT_DELTA:
    case HWA_PHYSICAL_ELEMENT_DISTINCTNESS_RATIO:
    case HWA_PHYSICAL_ELEMENT_GAIN_ONLY_SCORE:
    case HWA_PHYSICAL_ELEMENT_CARRYOVER_DB:
        return strcmp(check->scope, "profiles") == 0 &&
               (check->availability != HWA_PHYSICAL_AVAILABLE ||
                (check->reference_valid && check->model_valid &&
                 check->delta_valid));
    case HWA_PHYSICAL_ELEMENT_PITCH_ONLY_SCORE:
        return strcmp(check->scope, "profiles") == 0 &&
               check->availability == HWA_PHYSICAL_UNAVAILABLE;
    case HWA_PHYSICAL_BODY_MODE_PAN:
        return check->availability == HWA_PHYSICAL_UNAVAILABLE;
    case HWA_PHYSICAL_BEATING_DEPTH_RATIO:
    case HWA_PHYSICAL_BEATING_RATE_HZ:
    case HWA_PHYSICAL_ROUGHNESS_RATIO:
    case HWA_PHYSICAL_PITCH_PULL_CENTS: {
        int has_complete_side =
            hwa_physical_case_side_complete(set, 3, 1, check->case_id) ||
            hwa_physical_case_side_complete(set, 3, 0, check->case_id);
        return check->availability ==
               (has_complete_side ? HWA_PHYSICAL_INSUFFICIENT :
                                    HWA_PHYSICAL_UNAVAILABLE);
    }
    case HWA_PHYSICAL_BODY_MODE_DISTANCE_CENTS:
        return check->availability != HWA_PHYSICAL_AVAILABLE ||
               (!check->reference_valid && check->model_valid &&
                !check->delta_valid);
    default:
        break;
    }
    if (check->kind >= HWA_PHYSICAL_DC_OFFSET &&
        check->kind <= HWA_PHYSICAL_DENORMAL_FRACTION) {
        HWAPhysicalRole role;
        const HWAPhysicalSource *source =
            hwa_physical_source_for_role(set, check->scope);
        if (source == NULL ||
            hwa_physical_role_parse(check->scope, &role) != 0 ||
            strlen(check->case_id) != role.case_id_length ||
            memcmp(check->case_id, role.case_id,
                   role.case_id_length) != 0) return 0;
        if (check->availability == HWA_PHYSICAL_AVAILABLE) {
            if (role.side == HWA_PHYSICAL_ROLE_REFERENCE) {
                return check->reference_valid && !check->model_valid &&
                       !check->delta_valid;
            }
            return !check->reference_valid && check->model_valid &&
                   !check->delta_valid;
        }
    }
    return 1;
}

static HWAPhysicalUnit hwa_physical_expected_unit(HWAPhysicalCheckKind kind)
{
    switch (kind) {
    case HWA_PHYSICAL_ELEMENT_TRAIT_DELTA:
    case HWA_PHYSICAL_ELEMENT_REFERENCE_DISTANCE:
    case HWA_PHYSICAL_ELEMENT_MODEL_DISTANCE:
    case HWA_PHYSICAL_ELEMENT_DISTINCTNESS_RATIO:
    case HWA_PHYSICAL_ELEMENT_GAIN_ONLY_SCORE:
    case HWA_PHYSICAL_ELEMENT_PITCH_ONLY_SCORE:
    case HWA_PHYSICAL_BODY_MODE_Q:
    case HWA_PHYSICAL_BODY_MODE_PAN:
    case HWA_PHYSICAL_INTERMODULATION_RATIO:
    case HWA_PHYSICAL_BEATING_DEPTH_RATIO:
    case HWA_PHYSICAL_ROUGHNESS_RATIO:
    case HWA_PHYSICAL_RENDER_CORRELATION:
    case HWA_PHYSICAL_DC_OFFSET:
    case HWA_PHYSICAL_DC_DRIFT:
    case HWA_PHYSICAL_CLIP_FRACTION:
    case HWA_PHYSICAL_HARD_BOUND_FRACTION:
    case HWA_PHYSICAL_REPEATED_BLOCK_FRACTION:
    case HWA_PHYSICAL_STUCK_STATE_FRACTION:
    case HWA_PHYSICAL_HIGH_BAND_RATIO:
    case HWA_PHYSICAL_SUBHARMONIC_RATIO:
    case HWA_PHYSICAL_DENORMAL_FRACTION:
        return HWA_PHYSICAL_UNIT_RATIO;
    case HWA_PHYSICAL_ELEMENT_CARRYOVER_DB:
    case HWA_PHYSICAL_BODY_MODE_PROMINENCE_DB:
    case HWA_PHYSICAL_JOINT_RESIDUAL_DB:
    case HWA_PHYSICAL_SHARED_GAIN_DB:
    case HWA_PHYSICAL_SUM_TONE_DB:
    case HWA_PHYSICAL_DIFFERENCE_TONE_DB:
    case HWA_PHYSICAL_RENDER_RMS_ERROR_DB:
    case HWA_PHYSICAL_RENDER_SPECTRAL_DISTANCE_DB:
    case HWA_PHYSICAL_FIXED_TONE_PROMINENCE_DB:
        return HWA_PHYSICAL_UNIT_DB;
    case HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ:
    case HWA_PHYSICAL_BODY_MODE_BANDWIDTH_HZ:
    case HWA_PHYSICAL_BEATING_RATE_HZ:
        return HWA_PHYSICAL_UNIT_HZ;
    case HWA_PHYSICAL_BODY_MODE_DECAY_SECONDS:
    case HWA_PHYSICAL_RENDER_ATTACK_DELTA_SECONDS:
    case HWA_PHYSICAL_RENDER_DECAY_DELTA_SECONDS:
        return HWA_PHYSICAL_UNIT_SECONDS;
    case HWA_PHYSICAL_BODY_MODE_DENSITY_PER_KHZ:
        return HWA_PHYSICAL_UNIT_COUNT_VALUE;
    case HWA_PHYSICAL_BODY_MODE_DISTANCE_CENTS:
    case HWA_PHYSICAL_PITCH_PULL_CENTS:
    case HWA_PHYSICAL_RENDER_PITCH_DELTA_CENTS:
        return HWA_PHYSICAL_UNIT_CENTS;
    case HWA_PHYSICAL_RENDER_MAX_ERROR_DBFS:
    case HWA_PHYSICAL_MAX_STEP_DBFS:
    case HWA_PHYSICAL_RETURN_LEVEL_DBFS:
        return HWA_PHYSICAL_UNIT_DBFS;
    case HWA_PHYSICAL_RENDER_LAG_SAMPLES:
        return HWA_PHYSICAL_UNIT_SAMPLES;
    case HWA_PHYSICAL_RUNAWAY_SLOPE_DB_PER_SECOND:
        return HWA_PHYSICAL_UNIT_DB_PER_SECOND;
    default:
        return HWA_PHYSICAL_UNIT_COUNT;
    }
}

static int hwa_physical_body_mode_kind(HWAPhysicalCheckKind kind)
{
    switch (kind) {
    case HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ:
    case HWA_PHYSICAL_BODY_MODE_BANDWIDTH_HZ:
    case HWA_PHYSICAL_BODY_MODE_Q:
    case HWA_PHYSICAL_BODY_MODE_PROMINENCE_DB:
    case HWA_PHYSICAL_BODY_MODE_DECAY_SECONDS:
        return 1;
    default:
        return 0;
    }
}

static int hwa_physical_body_modes_valid(const HWAPhysicalCheckSet *set,
                                         size_t max_modes)
{
    static const HWAPhysicalCheckKind facts[] = {
        HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ,
        HWA_PHYSICAL_BODY_MODE_BANDWIDTH_HZ,
        HWA_PHYSICAL_BODY_MODE_Q,
        HWA_PHYSICAL_BODY_MODE_PROMINENCE_DB,
        HWA_PHYSICAL_BODY_MODE_DECAY_SECONDS
    };
    size_t index = 0U;
    const char *previous_case = NULL;
    uint32_t previous_mode = 0U;
    int previous_available = 0;
    while (index < set->check_count) {
        const HWAPhysicalCheck *check = &set->checks[index];
        size_t fact;
        if (!hwa_physical_body_mode_kind(check->kind)) {
            previous_case = NULL;
            previous_available = 0;
            index++;
            continue;
        }
        if (check->kind != HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ ||
            set->check_count - index < sizeof(facts) / sizeof(facts[0])) {
            return 0;
        }
        if (strcmp(check->scope, "body") != 0 || check->element[0] != '\0' ||
            (uint64_t)check->index >= (uint64_t)max_modes ||
            (check->availability != HWA_PHYSICAL_AVAILABLE &&
             check->index != 0U)) return 0;
        for (fact = 0U; fact < sizeof(facts) / sizeof(facts[0]); ++fact) {
            const HWAPhysicalCheck *member = &set->checks[index + fact];
            if (member->kind != facts[fact] ||
                strcmp(member->scope, "body") != 0 ||
                strcmp(member->case_id, check->case_id) != 0 ||
                member->element[0] != '\0' ||
                member->index != check->index ||
                member->availability != check->availability) return 0;
        }
        if (check->availability == HWA_PHYSICAL_AVAILABLE) {
            if (previous_available && previous_case != NULL &&
                strcmp(previous_case, check->case_id) == 0) {
                if (previous_mode == UINT32_MAX ||
                    check->index != previous_mode + 1U) return 0;
            } else if (check->index != 0U) {
                return 0;
            }
            previous_case = check->case_id;
            previous_mode = check->index;
            previous_available = 1;
        } else {
            previous_case = check->case_id;
            previous_mode = 0U;
            previous_available = 0;
        }
        index += sizeof(facts) / sizeof(facts[0]);
    }
    return 1;
}

static int hwa_physical_check_phase(HWAPhysicalCheckKind kind)
{
    if (kind >= HWA_PHYSICAL_ELEMENT_TRAIT_DELTA &&
        kind <= HWA_PHYSICAL_ELEMENT_CARRYOVER_DB) return 0;
    if (kind >= HWA_PHYSICAL_DC_OFFSET &&
        kind <= HWA_PHYSICAL_DENORMAL_FRACTION) return 1;
    if (kind >= HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ &&
        kind <= HWA_PHYSICAL_BODY_MODE_DISTANCE_CENTS) return 2;
    if (kind >= HWA_PHYSICAL_JOINT_RESIDUAL_DB &&
        kind <= HWA_PHYSICAL_PITCH_PULL_CENTS) return 3;
    if (kind >= HWA_PHYSICAL_RENDER_RMS_ERROR_DB &&
        kind <= HWA_PHYSICAL_RENDER_SPECTRAL_DISTANCE_DB) return 4;
    return -1;
}

static int hwa_physical_compare_text(const char *left, const char *right)
{
    int order = strcmp(left, right);
    return order < 0 ? -1 : order > 0 ? 1 : 0;
}

static int hwa_physical_compare_u32(uint32_t left, uint32_t right)
{
    return left < right ? -1 : left > right ? 1 : 0;
}

static int hwa_physical_compare_kind(HWAPhysicalCheckKind left,
                                     HWAPhysicalCheckKind right)
{
    return left < right ? -1 : left > right ? 1 : 0;
}

int hwa_physical_check_canonical_compare(const HWAPhysicalCheck *left,
                                         const HWAPhysicalCheck *right)
{
    int left_phase = hwa_physical_check_phase(left->kind);
    int right_phase = hwa_physical_check_phase(right->kind);
    int order;
    if (left_phase != right_phase) return left_phase < right_phase ? -1 : 1;
    if (left_phase == 0) {
        order = hwa_physical_compare_text(left->scope, right->scope);
        if (order == 0) {
            order = hwa_physical_compare_text(left->element, right->element);
        }
        if (order == 0) order = hwa_physical_compare_kind(left->kind, right->kind);
        if (order == 0) order = hwa_physical_compare_u32(left->index, right->index);
        if (order == 0) {
            order = hwa_physical_compare_text(left->case_id, right->case_id);
        }
        return order;
    }
    if (left_phase == 1) {
        order = hwa_physical_compare_text(left->scope, right->scope);
        if (order == 0) {
            order = hwa_physical_compare_text(left->case_id, right->case_id);
        }
        if (order == 0) order = hwa_physical_compare_kind(left->kind, right->kind);
        if (order == 0) order = hwa_physical_compare_u32(left->index, right->index);
        if (order == 0) {
            order = hwa_physical_compare_text(left->element, right->element);
        }
        return order;
    }
    order = hwa_physical_compare_text(left->case_id, right->case_id);
    if (order != 0) return order;
    if (left_phase == 2) {
        int left_scalar = !hwa_physical_body_mode_kind(left->kind);
        int right_scalar = !hwa_physical_body_mode_kind(right->kind);
        if (left_scalar != right_scalar) return left_scalar ? 1 : -1;
        if (!left_scalar) {
            order = hwa_physical_compare_u32(left->index, right->index);
            if (order == 0) {
                order = hwa_physical_compare_kind(left->kind, right->kind);
            }
        } else {
            order = hwa_physical_compare_kind(left->kind, right->kind);
            if (order == 0) {
                order = hwa_physical_compare_u32(left->index, right->index);
            }
        }
    } else {
        order = hwa_physical_compare_kind(left->kind, right->kind);
        if (order == 0) order = hwa_physical_compare_u32(left->index, right->index);
    }
    if (order == 0) order = hwa_physical_compare_text(left->scope, right->scope);
    if (order == 0) order = hwa_physical_compare_text(left->element, right->element);
    return order;
}

static int hwa_physical_check_order_valid(const HWAPhysicalCheckSet *set)
{
    size_t index;
    for (index = 0U; index < set->check_count; ++index) {
        const HWAPhysicalCheck *check = &set->checks[index];
        int phase = hwa_physical_check_phase(check->kind);
        if (phase < 0) return 0;
        if (phase == 0) {
            if (check->case_id[0] != '\0') return 0;
        } else if (phase == 1) {
            if (check->index != 0U || check->element[0] != '\0') return 0;
        } else if (phase == 2) {
            if (strcmp(check->scope, "body") != 0 ||
                check->element[0] != '\0' ||
                (!hwa_physical_body_mode_kind(check->kind) &&
                 check->index != 0U)) return 0;
        } else if (phase == 3) {
            if (strcmp(check->scope, "joint") != 0 ||
                check->index != 0U || check->element[0] != '\0') return 0;
        } else if (strcmp(check->scope, "render") != 0 ||
                   check->index != 0U || check->element[0] != '\0') {
            return 0;
        }
        if (index != 0U && hwa_physical_check_canonical_compare(
                &set->checks[index - 1U], check) >= 0) return 0;
    }
    return 1;
}

static int hwa_physical_catalog_check(
    const HWAPhysicalCheck *check,
    const char *scope,
    const char *case_id,
    HWAPhysicalCheckKind kind)
{
    return check->kind == kind && strcmp(check->scope, scope) == 0 &&
           strcmp(check->case_id, case_id) == 0;
}

static int hwa_physical_source_role_exists(
    const HWAPhysicalCheckSet *set,
    const char *prefix,
    const char *case_id)
{
    size_t low = 2U;
    size_t high = set->source_count;
    size_t prefix_length = strlen(prefix);
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        const char *candidate = set->sources[middle].role;
        int order = strncmp(candidate, prefix, prefix_length);
        if (order == 0) order = strcmp(candidate + prefix_length, case_id);
        if (order < 0) {
            low = middle + 1U;
        } else if (order > 0) {
            high = middle;
        } else {
            return 1;
        }
    }
    return 0;
}

static int hwa_physical_case_has_binding(
    const HWAPhysicalCheckSet *set,
    int phase,
    const char *case_id)
{
    static const char *const body_prefixes[] = {
        "model:body:", "reference:body:"
    };
    static const char *const joint_prefixes[] = {
        "model:isolated-a:", "model:isolated-b:", "model:joint:",
        "reference:isolated-a:", "reference:isolated-b:",
        "reference:joint:"
    };
    static const char *const render_prefixes[] = {
        "model:render-baseline:", "model:render-variant:",
        "reference:render-baseline:", "reference:render-variant:"
    };
    const char *const *prefixes;
    size_t prefix_count;
    size_t index;
    if (phase == 2) {
        prefixes = body_prefixes;
        prefix_count = sizeof(body_prefixes) / sizeof(body_prefixes[0]);
    } else if (phase == 3) {
        prefixes = joint_prefixes;
        prefix_count = sizeof(joint_prefixes) / sizeof(joint_prefixes[0]);
    } else {
        prefixes = render_prefixes;
        prefix_count = sizeof(render_prefixes) / sizeof(render_prefixes[0]);
    }
    for (index = 0U; index < prefix_count; ++index) {
        if (hwa_physical_source_role_exists(
                set, prefixes[index], case_id)) return 1;
    }
    return 0;
}

static int hwa_physical_checks_have_case(
    const HWAPhysicalCheckSet *set,
    size_t first,
    size_t last,
    const char *case_id)
{
    while (first < last) {
        size_t middle = first + (last - first) / 2U;
        int order = strcmp(set->checks[middle].case_id, case_id);
        if (order < 0) {
            first = middle + 1U;
        } else if (order > 0) {
            last = middle;
        } else {
            return 1;
        }
    }
    return 0;
}

static int hwa_physical_role_phase(HWAPhysicalRoleKind kind)
{
    if (kind == HWA_PHYSICAL_ROLE_BODY) return 2;
    if (kind == HWA_PHYSICAL_ROLE_JOINT ||
        kind == HWA_PHYSICAL_ROLE_ISOLATED_A ||
        kind == HWA_PHYSICAL_ROLE_ISOLATED_B) return 3;
    if (kind == HWA_PHYSICAL_ROLE_RENDER_BASELINE ||
        kind == HWA_PHYSICAL_ROLE_RENDER_VARIANT) return 4;
    return 0;
}

static int hwa_physical_check_catalog_valid(
    const HWAPhysicalCheckSet *set)
{
    size_t index = 0U;
    size_t source_index;
    size_t trait_count = 0U;
    size_t carryover_count = 0U;
    size_t body_cases = 0U;
    size_t joint_cases = 0U;
    size_t render_cases = 0U;
    size_t body_bindings = 0U;
    size_t joint_bindings = 0U;
    size_t render_bindings = 0U;
    size_t body_first;
    size_t body_last;
    size_t joint_first;
    size_t joint_last;
    size_t render_first;
    size_t render_last;
    while (index < set->check_count &&
           hwa_physical_check_phase(set->checks[index].kind) == 0) {
        if (set->checks[index].kind == HWA_PHYSICAL_ELEMENT_TRAIT_DELTA) {
            trait_count++;
        }
        if (set->checks[index].kind == HWA_PHYSICAL_ELEMENT_CARRYOVER_DB) {
            carryover_count++;
        }
        index++;
    }
    if (trait_count == 0U || carryover_count != 1U) return 0;
    for (source_index = 2U; source_index < set->source_count;
         ++source_index) {
        HWAPhysicalRole role;
        int value;
        if (hwa_physical_role_parse(
                set->sources[source_index].role, &role) != 0) return 0;
        if (hwa_physical_role_phase(role.kind) == 2) body_bindings++;
        if (hwa_physical_role_phase(role.kind) == 3) joint_bindings++;
        if (hwa_physical_role_phase(role.kind) == 4) render_bindings++;
        for (value = (int)HWA_PHYSICAL_DC_OFFSET;
             value <= (int)HWA_PHYSICAL_DENORMAL_FRACTION; ++value) {
            if (index >= set->check_count ||
                !hwa_physical_catalog_check(
                    &set->checks[index], set->sources[source_index].role,
                    role.case_id, (HWAPhysicalCheckKind)value)) return 0;
            index++;
        }
    }
    if (index < set->check_count &&
        hwa_physical_check_phase(set->checks[index].kind) == 1) return 0;
    body_first = index;
    while (index < set->check_count &&
           hwa_physical_check_phase(set->checks[index].kind) == 2) {
        const char *case_id = set->checks[index].case_id;
        size_t mode_rows = 0U;
        int value;
        while (index < set->check_count &&
               hwa_physical_check_phase(set->checks[index].kind) == 2 &&
               strcmp(set->checks[index].case_id, case_id) == 0 &&
               hwa_physical_body_mode_kind(set->checks[index].kind)) {
            mode_rows++;
            index++;
        }
        if (mode_rows == 0U) return 0;
        if ((body_bindings == 0U && case_id[0] != '\0') ||
            (body_bindings != 0U &&
             !hwa_physical_case_has_binding(set, 2, case_id))) return 0;
        for (value = (int)HWA_PHYSICAL_BODY_MODE_PAN;
             value <= (int)HWA_PHYSICAL_BODY_MODE_DISTANCE_CENTS; ++value) {
            if (index >= set->check_count ||
                !hwa_physical_catalog_check(
                    &set->checks[index], "body", case_id,
                    (HWAPhysicalCheckKind)value)) return 0;
            index++;
        }
        body_cases++;
    }
    body_last = index;
    joint_first = index;
    while (index < set->check_count &&
           hwa_physical_check_phase(set->checks[index].kind) == 3) {
        const char *case_id = set->checks[index].case_id;
        int value;
        if ((joint_bindings == 0U && case_id[0] != '\0') ||
            (joint_bindings != 0U &&
             !hwa_physical_case_has_binding(set, 3, case_id))) return 0;
        for (value = (int)HWA_PHYSICAL_JOINT_RESIDUAL_DB;
             value <= (int)HWA_PHYSICAL_PITCH_PULL_CENTS; ++value) {
            if (index >= set->check_count ||
                !hwa_physical_catalog_check(
                    &set->checks[index], "joint", case_id,
                    (HWAPhysicalCheckKind)value)) return 0;
            index++;
        }
        joint_cases++;
    }
    joint_last = index;
    render_first = index;
    while (index < set->check_count &&
           hwa_physical_check_phase(set->checks[index].kind) == 4) {
        const char *case_id = set->checks[index].case_id;
        int value;
        if ((render_bindings == 0U && case_id[0] != '\0') ||
            (render_bindings != 0U &&
             !hwa_physical_case_has_binding(set, 4, case_id))) return 0;
        for (value = (int)HWA_PHYSICAL_RENDER_RMS_ERROR_DB;
             value <= (int)HWA_PHYSICAL_RENDER_SPECTRAL_DISTANCE_DB;
             ++value) {
            if (index >= set->check_count ||
                !hwa_physical_catalog_check(
                    &set->checks[index], "render", case_id,
                    (HWAPhysicalCheckKind)value)) return 0;
            index++;
        }
        render_cases++;
    }
    render_last = index;
    if (index != set->check_count || body_cases == 0U ||
        joint_cases == 0U || render_cases == 0U ||
        (body_bindings == 0U && body_cases != 1U) ||
        (joint_bindings == 0U && joint_cases != 1U) ||
        (render_bindings == 0U && render_cases != 1U)) return 0;
    for (source_index = 2U; source_index < set->source_count;
         ++source_index) {
        HWAPhysicalRole role;
        int phase;
        if (hwa_physical_role_parse(
                set->sources[source_index].role, &role) != 0) return 0;
        phase = hwa_physical_role_phase(role.kind);
        if ((phase == 2 && !hwa_physical_checks_have_case(
                 set, body_first, body_last, role.case_id)) ||
            (phase == 3 && !hwa_physical_checks_have_case(
                 set, joint_first, joint_last, role.case_id)) ||
            (phase == 4 && !hwa_physical_checks_have_case(
                 set, render_first, render_last, role.case_id))) return 0;
    }
    return 1;
}

static int hwa_physical_sources_valid(const HWAPhysicalCheckSet *set)
{
    size_t index;
    if (set->source_count < 2U || set->sources == NULL ||
        set->source_count - 2U > set->options.max_bindings) return 0;
    for (index = 0U; index < set->source_count; ++index) {
        const HWAPhysicalSource *source = &set->sources[index];
        if (source->id != (uint64_t)index + 1U ||
            !hwa_physical_string_valid(source->role, 0) ||
            !hwa_physical_path_valid(source->path) ||
            !hwa_physical_sha_valid(source->sha256)) return 0;
        if (index < 2U) {
            const char *role = index == 0U ? "reference:profile" :
                                             "model:profile";
            const char *path = index == 0U ? set->reference_measures_path :
                                             set->model_measures_path;
            const char *hash = index == 0U ? set->reference_measures_sha256 :
                                             set->model_measures_sha256;
            if (source->is_wave || strcmp(source->role, role) != 0 ||
                strcmp(source->path, path) != 0 ||
                strcmp(source->sha256, hash) != 0 ||
                !hwa_physical_zero_format(&source->format)) return 0;
        } else {
            HWAPhysicalRole role;
            if (!source->is_wave ||
                hwa_physical_role_parse(source->role, &role) != 0 ||
                !hwa_physical_wave_format_valid(&source->format) ||
                source->format.frames > set->options.max_wave_frames ||
                source->format.data_bytes > set->options.max_wave_bytes) {
                return 0;
            }
        }
        if (index > 2U &&
            strcmp(set->sources[index - 1U].role, source->role) >= 0) return 0;
    }
    return 1;
}

static int hwa_physical_finding_canonical_compare(
    const HWAPhysicalFinding *left,
    const HWAPhysicalFinding *right)
{
    if (left->score_valid != right->score_valid) {
        return left->score_valid ? -1 : 1;
    }
    if (left->score_valid && left->score != right->score) {
        return left->score > right->score ? -1 : 1;
    }
    if (left->check_id_valid != right->check_id_valid) {
        return left->check_id_valid ? -1 : 1;
    }
    if (left->check_id < right->check_id) return -1;
    if (left->check_id > right->check_id) return 1;
    if (left->id < right->id) return -1;
    if (left->id > right->id) return 1;
    return 0;
}

static int hwa_physical_score_equal(double left,
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

static int hwa_physical_scored_finding_matches(
    const HWAPhysicalCheckSet *set,
    const HWAPhysicalFinding *finding,
    unsigned max_score_ulps)
{
    const HWAPhysicalCheck *check;
    HWAPhysicalFindingClass finding_class;
    HWAPhysicalSeverity severity;
    const char *code;
    const char *message;
    double score;
    if (!finding->score_valid || !finding->check_id_valid ||
        finding->check_id == 0U ||
        finding->check_id > set->check_count) return 0;
    check = &set->checks[(size_t)(finding->check_id - 1U)];
    return hwa_physical_scored_finding_for_check(
               check, &finding_class, &severity,
               &code, &message, &score) == 1 &&
           finding->finding_class == finding_class &&
           finding->severity ==
               hwa_physical_severity_for_score(finding->score) &&
           hwa_physical_score_equal(
               finding->score, score, max_score_ulps) &&
           strcmp(finding->code, code) == 0 &&
           strcmp(finding->message, message) == 0;
}

static int hwa_physical_missing_finding_matches(
    const HWAPhysicalFinding *finding,
    const HWAPhysicalCheck *check)
{
    return !finding->score_valid && finding->rank == 0U &&
           finding->check_id_valid && finding->check_id == check->id &&
           finding->finding_class == HWA_PHYSICAL_FINDING_UNAVAILABLE &&
           finding->severity == HWA_PHYSICAL_SEVERITY_INFO &&
           strcmp(finding->code, "missing-physical-evidence") == 0 &&
           strcmp(finding->message,
                  "This physical check family has no usable bound evidence.")
               == 0;
}

static int hwa_physical_missing_family_scope(const char *scope)
{
    return strcmp(scope, "body") == 0 || strcmp(scope, "joint") == 0 ||
           strcmp(scope, "render") == 0;
}

static int hwa_physical_findings_derived_valid(
    const HWAPhysicalCheckSet *set,
    unsigned max_score_ulps)
{
    size_t check_index;
    size_t finding_index = 0U;
    size_t expected_scored = 0U;
    uint64_t previous_check_id = 0U;
    for (check_index = 0U; check_index < set->check_count; ++check_index) {
        HWAPhysicalFindingClass finding_class;
        HWAPhysicalSeverity severity;
        const char *code;
        const char *message;
        double score;
        if (hwa_physical_scored_finding_for_check(
                &set->checks[check_index], &finding_class, &severity,
                &code, &message, &score) == 1) expected_scored++;
    }
    while (finding_index < set->finding_count &&
           set->findings[finding_index].score_valid) {
        const HWAPhysicalFinding *finding = &set->findings[finding_index];
        if (!hwa_physical_scored_finding_matches(
                set, finding, max_score_ulps) ||
            finding->check_id == previous_check_id) return 0;
        previous_check_id = finding->check_id;
        finding_index++;
    }
    if (finding_index != expected_scored) return 0;
    check_index = 0U;
    while (check_index < set->check_count) {
        const HWAPhysicalCheck *first = &set->checks[check_index];
        size_t end = check_index + 1U;
        int available = first->availability == HWA_PHYSICAL_AVAILABLE;
        if (!hwa_physical_missing_family_scope(first->scope)) {
            check_index++;
            continue;
        }
        while (end < set->check_count &&
               strcmp(set->checks[end].scope, first->scope) == 0 &&
               strcmp(set->checks[end].case_id, first->case_id) == 0) {
            if (set->checks[end].availability == HWA_PHYSICAL_AVAILABLE) {
                available = 1;
            }
            end++;
        }
        if (!available) {
            if (finding_index >= set->finding_count ||
                !hwa_physical_missing_finding_matches(
                    &set->findings[finding_index], first)) return 0;
            finding_index++;
        }
        check_index = end;
    }
    return finding_index == set->finding_count;
}

static int hwa_physical_finding_qsort_compare(const void *left,
                                              const void *right)
{
    return hwa_physical_finding_canonical_compare(
        (const HWAPhysicalFinding *)left,
        (const HWAPhysicalFinding *)right);
}

static void hwa_physical_normalize_findings(HWAPhysicalCheckSet *set)
{
    size_t index;
    size_t rank = 0U;
    for (index = 0U; index < set->finding_count; ++index) {
        HWAPhysicalFinding *finding = &set->findings[index];
        if (finding->score_valid) {
            HWAPhysicalFindingClass finding_class;
            HWAPhysicalSeverity severity;
            const char *code;
            const char *message;
            double score;
            (void)hwa_physical_scored_finding_for_check(
                &set->checks[(size_t)(finding->check_id - 1U)],
                &finding_class, &severity, &code, &message, &score);
            finding->score = score;
            finding->severity = severity;
        }
    }
    if (set->finding_count > 1U) {
        qsort(set->findings, set->finding_count, sizeof(*set->findings),
              hwa_physical_finding_qsort_compare);
    }
    for (index = 0U; index < set->finding_count; ++index) {
        set->findings[index].id = (uint64_t)index + 1U;
        if (set->findings[index].score_valid) {
            set->findings[index].rank = ++rank;
        } else {
            set->findings[index].rank = 0U;
        }
    }
}

static int hwa_physical_add_bytes(uint64_t *total, uint64_t amount)
{
    if (amount > UINT64_MAX - *total) return -1;
    *total += amount;
    return 0;
}

static int hwa_physical_add_string(uint64_t *total, const char *text)
{
    size_t length;
    if (text == NULL) return -1;
    length = strlen(text);
    return length == SIZE_MAX ||
                   hwa_physical_add_bytes(total, (uint64_t)length + 1U) != 0
               ? -1 : 0;
}

int hwa_physical_check_set_retained_bytes(
    const HWAPhysicalCheckSet *set,
    uint64_t *bytes)
{
    uint64_t total = 0U;
    size_t index;
    if (set == NULL || bytes == NULL ||
#if SIZE_MAX >= UINT64_MAX
        set->source_count > UINT64_MAX / sizeof(HWAPhysicalSource) ||
        set->check_count > UINT64_MAX / sizeof(HWAPhysicalCheck) ||
        set->finding_count > UINT64_MAX / sizeof(HWAPhysicalFinding) ||
        set->warning_count > UINT64_MAX / sizeof(HWAPhysicalWarning) ||
#endif
        hwa_physical_add_bytes(
            &total, (uint64_t)set->source_count *
                        (uint64_t)sizeof(HWAPhysicalSource)) != 0 ||
        hwa_physical_add_bytes(
            &total, (uint64_t)set->check_count *
                        (uint64_t)sizeof(HWAPhysicalCheck)) != 0 ||
        hwa_physical_add_bytes(
            &total, (uint64_t)set->finding_count *
                        (uint64_t)sizeof(HWAPhysicalFinding)) != 0 ||
        hwa_physical_add_bytes(
            &total, (uint64_t)set->warning_count *
                        (uint64_t)sizeof(HWAPhysicalWarning)) != 0 ||
        hwa_physical_add_string(&total, set->reference_measures_path) != 0 ||
        hwa_physical_add_string(&total, set->model_measures_path) != 0) {
        return -1;
    }
    for (index = 0U; index < set->source_count; ++index) {
        if (hwa_physical_add_string(&total, set->sources[index].role) != 0 ||
            hwa_physical_add_string(&total, set->sources[index].path) != 0) {
            return -1;
        }
    }
    for (index = 0U; index < set->check_count; ++index) {
        if (hwa_physical_add_string(&total, set->checks[index].scope) != 0 ||
            hwa_physical_add_string(&total, set->checks[index].case_id) != 0 ||
            hwa_physical_add_string(&total, set->checks[index].element) != 0) {
            return -1;
        }
    }
    for (index = 0U; index < set->finding_count; ++index) {
        if (hwa_physical_add_string(&total, set->findings[index].code) != 0 ||
            hwa_physical_add_string(&total,
                                    set->findings[index].message) != 0) {
            return -1;
        }
    }
    for (index = 0U; index < set->warning_count; ++index) {
        if (hwa_physical_add_string(&total, set->warnings[index].code) != 0 ||
            hwa_physical_add_string(&total,
                                    set->warnings[index].message) != 0) {
            return -1;
        }
    }
    *bytes = total;
    return 0;
}

static int hwa_physical_check_set_validate_with_score_ulps(
    const HWAPhysicalCheckSet *set,
    unsigned max_score_ulps,
    char *error,
    size_t error_size)
{
    size_t index;
    size_t scored = 0U;
    double previous_score = INFINITY;
    uint64_t retained_bytes;
    if (set == NULL || !hwa_physical_options_valid(&set->options) ||
        !hwa_physical_path_valid(set->reference_measures_path) ||
        !hwa_physical_path_valid(set->model_measures_path) ||
        !hwa_physical_sha_valid(set->reference_measures_sha256) ||
        !hwa_physical_sha_valid(set->model_measures_sha256) ||
        strcmp(set->reference_measures_sha256,
               set->model_measures_sha256) == 0 ||
        (set->check_count != 0U && set->checks == NULL) ||
        (set->finding_count != 0U && set->findings == NULL) ||
        (set->warning_count != 0U && set->warnings == NULL) ||
        set->check_count > set->options.max_checks ||
        set->finding_count > set->options.max_findings ||
        set->warning_count > set->options.max_warnings ||
        set->transform_count > set->options.max_transforms ||
        set->pair_evaluations > set->options.max_pair_evaluations ||
        !hwa_physical_sources_valid(set) ||
        hwa_physical_check_set_retained_bytes(set, &retained_bytes) != 0 ||
        retained_bytes != set->retained_work_bytes) {
        hwa_physical_error(error, error_size,
                           "invalid physical-check result metadata");
        return -1;
    }
    for (index = 0U; index < set->check_count; ++index) {
        const HWAPhysicalCheck *check = &set->checks[index];
        if (check->id != (uint64_t)index + 1U ||
            hwa_physical_check_kind_name(check->kind) == NULL ||
            hwa_physical_unit_name(check->unit) == NULL ||
            hwa_physical_expected_unit(check->kind) != check->unit ||
            hwa_physical_availability_name(check->availability) == NULL ||
            !hwa_physical_string_valid(check->scope, 0) ||
            !hwa_physical_case_valid(check->case_id) ||
            !hwa_physical_string_valid(check->element, 1) ||
            !hwa_physical_check_values_valid(check) ||
            !hwa_physical_check_shape_valid(set, check) ||
            (check->evidence_flags & ~HWA_PHYSICAL_EVIDENCE_ALL) != 0U ||
            (check->quality_flags & ~HWA_PHYSICAL_QUALITY_ALL) != 0U) {
            hwa_physical_error(error, error_size,
                               "invalid physical-check row");
            return -1;
        }
    }
    if (!hwa_physical_body_modes_valid(set, set->options.max_modes)) {
        hwa_physical_error(error, error_size,
                           "physical body-mode rows are not canonical");
        return -1;
    }
    if (!hwa_physical_check_order_valid(set)) {
        hwa_physical_error(error, error_size,
                           "physical check rows are not canonical");
        return -1;
    }
    if (!hwa_physical_check_catalog_valid(set)) {
        hwa_physical_error(error, error_size,
                           "physical check catalog is incomplete");
        return -1;
    }
    for (index = 0U; index < set->finding_count; ++index) {
        const HWAPhysicalFinding *finding = &set->findings[index];
        if (finding->id != (uint64_t)index + 1U ||
            hwa_physical_finding_class_name(finding->finding_class) == NULL ||
            hwa_physical_severity_name(finding->severity) == NULL ||
            !hwa_physical_string_valid(finding->code, 0) ||
            !hwa_physical_string_valid(finding->message, 0) ||
            !isfinite(finding->score) ||
            (!finding->score_valid && finding->score != 0.0) ||
            (finding->score_valid &&
             (finding->score < 0.0 || finding->score > 1.0)) ||
            (!finding->check_id_valid && finding->check_id != 0U) ||
            (finding->check_id_valid &&
             (finding->check_id == 0U ||
              finding->check_id > set->check_count))) {
            hwa_physical_error(error, error_size,
                               "invalid physical finding row");
            return -1;
        }
        if (index != 0U && hwa_physical_finding_canonical_compare(
                &set->findings[index - 1U], finding) >= 0) {
            hwa_physical_error(error, error_size,
                               "physical finding order is invalid");
            return -1;
        }
        if (finding->score_valid) {
            scored++;
            if (finding->rank != scored || finding->score > previous_score) {
                hwa_physical_error(error, error_size,
                                   "physical finding rank order is invalid");
                return -1;
            }
            previous_score = finding->score;
        } else if (finding->rank != 0U) {
            hwa_physical_error(error, error_size,
                               "unscored physical finding has a rank");
            return -1;
        }
    }
    if (!hwa_physical_findings_derived_valid(set, max_score_ulps)) {
        hwa_physical_error(error, error_size,
                           "physical findings do not match their checks");
        return -1;
    }
    for (index = 0U; index < set->warning_count; ++index) {
        const HWAPhysicalWarning *warning = &set->warnings[index];
        if (warning->id != (uint64_t)index + 1U ||
            !hwa_physical_string_valid(warning->code, 0) ||
            !hwa_physical_string_valid(warning->message, 0) ||
            (!warning->source_id_valid && warning->source_id != 0U) ||
            (warning->source_id_valid &&
             (warning->source_id == 0U ||
              warning->source_id > set->source_count)) ||
            (!warning->check_id_valid && warning->check_id != 0U) ||
            (warning->check_id_valid &&
             (warning->check_id == 0U ||
              warning->check_id > set->check_count))) {
            hwa_physical_error(error, error_size,
                               "invalid physical warning row");
            return -1;
        }
    }
    return 0;
}

int hwa_physical_check_set_validate(const HWAPhysicalCheckSet *set,
                                    char *error,
                                    size_t error_size)
{
    return hwa_physical_check_set_validate_with_score_ulps(
        set, 0U, error, error_size);
}

static int hwa_physical_csv_field(FILE *stream, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    int quoted = 0;
    while (*cursor != 0U) {
        if (*cursor == (unsigned char)',' || *cursor == (unsigned char)'"' ||
            *cursor == (unsigned char)'\r' || *cursor == (unsigned char)'\n') {
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

static int hwa_physical_path_hex(FILE *stream, const char *path)
{
    const unsigned char *cursor = (const unsigned char *)path;
    while (*cursor != 0U) {
        if (fprintf(stream, "%02x", (unsigned)*cursor++) < 0) return -1;
    }
    return 0;
}

static int hwa_physical_number(FILE *stream, double value)
{
    return !isfinite(value) ||
           fprintf(stream, "%.17g", value == 0.0 ? 0.0 : value) < 0 ? -1 : 0;
}

static int hwa_physical_optional_number(FILE *stream,
                                        double value,
                                        int valid)
{
    return valid ? hwa_physical_number(stream, value) : 0;
}

static int hwa_physical_meta_text(FILE *stream,
                                  const char *key,
                                  const char *value,
                                  const char *unit)
{
    return fputs("META,", stream) == EOF ||
           hwa_physical_csv_field(stream, key) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_physical_csv_field(stream, value) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_physical_csv_field(stream, unit) != 0 ||
           fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_physical_meta_u64(FILE *stream,
                                 const char *key,
                                 uint64_t value,
                                 const char *unit)
{
    char text[32];
    if (snprintf(text, sizeof(text), "%" PRIu64, value) < 0) return -1;
    return hwa_physical_meta_text(stream, key, text, unit);
}

static int hwa_physical_meta_size(FILE *stream,
                                  const char *key,
                                  size_t value,
                                  const char *unit)
{
    char text[32];
    if (snprintf(text, sizeof(text), "%zu", value) < 0) return -1;
    return hwa_physical_meta_text(stream, key, text, unit);
}

static int hwa_physical_meta_double(FILE *stream,
                                    const char *key,
                                    double value,
                                    const char *unit)
{
    char text[64];
    if (!isfinite(value) ||
        snprintf(text, sizeof(text), "%.17g", value == 0.0 ? 0.0 : value) < 0) {
        return -1;
    }
    return hwa_physical_meta_text(stream, key, text, unit);
}

static int hwa_physical_write_meta(FILE *stream,
                                   const HWAPhysicalCheckSet *set)
{
    const HWAPhysicalOptions *options = &set->options;
    const HWAProfileComparisonOptions *profile = &options->profile_limits;
    return hwa_physical_meta_text(stream, "tool_version", HWA_VERSION, "") ||
           hwa_physical_meta_text(stream, "physical_check_method_version",
                                  HWA_PHYSICAL_CHECK_METHOD_VERSION, "") ||
           hwa_physical_meta_text(stream, "build_compiler_family",
                                  hwa_build_compiler_family(), "") ||
           hwa_physical_meta_text(stream, "build_compiler_version",
                                  hwa_build_compiler_version(), "") ||
           hwa_physical_meta_text(stream, "build_c_standard",
                                  hwa_build_c_standard(), "") ||
           hwa_physical_meta_text(stream, "build_target_os",
                                  hwa_build_target_os(), "") ||
           hwa_physical_meta_u64(stream, "build_pointer_bits",
                                 hwa_build_pointer_bits(), "bits") ||
           hwa_physical_meta_text(stream, "build_endianness",
                                  hwa_build_endianness(), "") ||
           hwa_physical_meta_text(stream, "build_mode",
                                  hwa_build_mode(), "") ||
           hwa_physical_meta_size(stream, "decode_block_frames",
                                  options->decode_block_frames, "frames") ||
           hwa_physical_meta_size(stream, "fft_size",
                                  options->fft_size, "samples") ||
           hwa_physical_meta_size(stream, "hop_size",
                                  options->hop_size, "samples") ||
           hwa_physical_meta_double(stream, "spectral_floor_dbfs",
                                    options->spectral_floor_dbfs, "dBFS") ||
           hwa_physical_meta_u64(stream, "max_wave_bytes",
                                 options->max_wave_bytes, "bytes") ||
           hwa_physical_meta_u64(stream, "max_wave_frames",
                                 options->max_wave_frames, "frames") ||
           hwa_physical_meta_u64(stream, "max_work_bytes",
                                 options->max_work_bytes, "bytes") ||
           hwa_physical_meta_u64(stream, "max_pair_evaluations",
                                 options->max_pair_evaluations,
                                 "evaluations") ||
           hwa_physical_meta_size(stream, "max_bindings",
                                  options->max_bindings, "bindings") ||
           hwa_physical_meta_size(stream, "max_transforms",
                                  options->max_transforms, "transforms") ||
           hwa_physical_meta_size(stream, "max_modes",
                                  options->max_modes, "modes") ||
           hwa_physical_meta_size(stream, "max_checks",
                                  options->max_checks, "checks") ||
           hwa_physical_meta_size(stream, "max_findings",
                                  options->max_findings, "findings") ||
           hwa_physical_meta_size(stream, "max_warnings",
                                  options->max_warnings, "warnings") ||
           hwa_physical_meta_u64(stream, "profile_max_input_bytes",
                                 profile->max_input_bytes, "bytes") ||
           hwa_physical_meta_u64(stream, "profile_max_work_bytes",
                                 profile->max_work_bytes, "bytes") ||
           hwa_physical_meta_size(stream, "profile_max_contexts",
                                  profile->max_contexts, "contexts") ||
           hwa_physical_meta_size(stream, "profile_max_measurements",
                                  profile->max_measurements, "measurements") ||
           hwa_physical_meta_size(stream, "profile_max_groups",
                                  profile->max_groups, "groups") ||
           hwa_physical_meta_size(stream, "profile_max_group_members",
                                  profile->max_group_members, "members") ||
           hwa_physical_meta_size(stream, "profile_max_statistics",
                                  profile->max_statistics, "statistics") ||
           hwa_physical_meta_size(stream, "profile_max_warnings",
                                  profile->max_warnings, "warnings") ||
           hwa_physical_meta_size(stream, "profile_max_distributions",
                                  profile->max_distributions,
                                  "distributions") ||
           hwa_physical_meta_size(stream, "profile_max_gaps",
                                  profile->max_gaps, "gaps") ||
           hwa_physical_meta_u64(stream, "retained_work_bytes",
                                 set->retained_work_bytes, "bytes") ||
           hwa_physical_meta_u64(stream, "pair_evaluations",
                                 set->pair_evaluations, "evaluations") ||
           hwa_physical_meta_size(stream, "transform_count",
                                  set->transform_count, "transforms") ||
           hwa_physical_meta_size(stream, "source_count",
                                  set->source_count, "sources") ||
           hwa_physical_meta_size(stream, "check_count",
                                  set->check_count, "checks") ||
           hwa_physical_meta_size(stream, "finding_count",
                                  set->finding_count, "findings") ||
           hwa_physical_meta_size(stream, "warning_count",
                                  set->warning_count, "warnings") ? -1 : 0;
}

static int hwa_physical_write_source(FILE *stream,
                                     const HWAPhysicalSource *source)
{
    const HWAFormat *format = &source->format;
    if (fprintf(stream, "INPUT,%" PRIu64 ",", source->id) < 0 ||
        hwa_physical_csv_field(stream, source->role) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_physical_path_hex(stream, source->path) != 0 ||
        fprintf(stream, ",%s,%d", source->sha256,
                source->is_wave ? 1 : 0) < 0) {
        return -1;
    }
    if (!source->is_wave) {
        return fputs(",,,,,,,,,,,\r\n", stream) == EOF ? -1 : 0;
    }
    return fputc(',', stream) == EOF ||
           hwa_physical_csv_field(stream,
                                  hwa_container_name(format->container)) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_physical_csv_field(stream,
                                  hwa_encoding_name(format->encoding)) != 0 ||
           fprintf(stream,
                   ",%u,%" PRIu32 ",%u,%u,%u,%" PRIu32 ",%" PRIu64
                   ",%" PRIu64 ",",
                   (unsigned)format->channels, format->sample_rate_hz,
                   (unsigned)format->bits_per_sample,
                   (unsigned)format->valid_bits_per_sample,
                   (unsigned)format->block_align, format->channel_mask,
                   format->frames, format->data_bytes) < 0 ||
           hwa_physical_number(stream, format->duration_seconds) != 0 ||
           fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_physical_write_check(FILE *stream,
                                    const HWAPhysicalCheck *check)
{
    if (fprintf(stream, "CHECK,%" PRIu64 ",", check->id) < 0 ||
        hwa_physical_csv_field(stream, check->scope) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_physical_csv_field(stream, check->case_id) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_physical_csv_field(stream, check->element) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_physical_csv_field(
            stream, hwa_physical_check_kind_name(check->kind)) != 0 ||
        fprintf(stream, ",%" PRIu32 ",", check->index) < 0 ||
        hwa_physical_csv_field(stream,
                               hwa_physical_unit_name(check->unit)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_physical_csv_field(
            stream,
            hwa_physical_availability_name(check->availability)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_physical_optional_number(stream, check->reference_value,
                                     check->reference_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_physical_optional_number(stream, check->model_value,
                                     check->model_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_physical_optional_number(stream, check->delta,
                                     check->delta_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_physical_number(stream, check->confidence) != 0 ||
        fprintf(stream, ",%" PRIu32 ",%" PRIu32 ",%d,%d,%d\r\n",
                check->evidence_flags, check->quality_flags,
                check->reference_valid ? 1 : 0,
                check->model_valid ? 1 : 0,
                check->delta_valid ? 1 : 0) < 0) {
        return -1;
    }
    return 0;
}

static int hwa_physical_write_finding(FILE *stream,
                                      const HWAPhysicalFinding *finding)
{
    if (fprintf(stream, "FINDING,%" PRIu64 ",", finding->id) < 0 ||
        hwa_physical_csv_field(
            stream,
            hwa_physical_finding_class_name(finding->finding_class)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_physical_csv_field(
            stream, hwa_physical_severity_name(finding->severity)) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_physical_csv_field(stream, finding->code) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_physical_csv_field(stream, finding->message) != 0 ||
        fputc(',', stream) == EOF) return -1;
    if (finding->check_id_valid &&
        fprintf(stream, "%" PRIu64, finding->check_id) < 0) return -1;
    if (fputc(',', stream) == EOF ||
        hwa_physical_optional_number(stream, finding->score,
                                     finding->score_valid) != 0 ||
        fprintf(stream, ",%zu,%d,%d\r\n", finding->rank,
                finding->check_id_valid ? 1 : 0,
                finding->score_valid ? 1 : 0) < 0) return -1;
    return 0;
}

static int hwa_physical_write_warning(FILE *stream,
                                      const HWAPhysicalWarning *warning)
{
    if (fprintf(stream, "WARNING,%" PRIu64 ",", warning->id) < 0 ||
        hwa_physical_csv_field(stream, warning->code) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_physical_csv_field(stream, warning->message) != 0 ||
        fputc(',', stream) == EOF) return -1;
    if (warning->source_id_valid &&
        fprintf(stream, "%" PRIu64, warning->source_id) < 0) return -1;
    if (fputc(',', stream) == EOF) return -1;
    if (warning->check_id_valid &&
        fprintf(stream, "%" PRIu64, warning->check_id) < 0) return -1;
    return fprintf(stream, ",%d,%d\r\n",
                   warning->source_id_valid ? 1 : 0,
                   warning->check_id_valid ? 1 : 0) < 0 ? -1 : 0;
}

static int hwa_physical_file_write_impl(FILE *stream,
                                        const HWAPhysicalCheckSet *set,
                                        char *error,
                                        size_t error_size)
{
    size_t index;
    if (stream == NULL ||
        hwa_physical_check_set_validate(set, error, error_size) != 0 ||
        (set != NULL &&
         set->retained_work_bytes > set->options.max_work_bytes)) {
        if (stream == NULL) {
            hwa_physical_error(error, error_size,
                               "physical output stream is null");
        } else if (set != NULL &&
                   set->retained_work_bytes > set->options.max_work_bytes) {
            hwa_physical_error(error, error_size,
                               "physical result exceeds its saved work limit");
        }
        return -1;
    }
    if (fprintf(stream, "HWA_PHYSICAL,%u\r\n",
                HWA_PHYSICAL_FILE_SCHEMA_VERSION) < 0 ||
        hwa_physical_write_meta(stream, set) != 0) goto write_error;
    for (index = 0U; index < set->source_count; ++index) {
        if (hwa_physical_write_source(stream, &set->sources[index]) != 0) {
            goto write_error;
        }
    }
    for (index = 0U; index < set->check_count; ++index) {
        if (hwa_physical_write_check(stream, &set->checks[index]) != 0) {
            goto write_error;
        }
    }
    for (index = 0U; index < set->finding_count; ++index) {
        if (hwa_physical_write_finding(stream, &set->findings[index]) != 0) {
            goto write_error;
        }
    }
    for (index = 0U; index < set->warning_count; ++index) {
        if (hwa_physical_write_warning(stream, &set->warnings[index]) != 0) {
            goto write_error;
        }
    }
    return 0;

write_error:
    hwa_physical_error(error, error_size,
                       "cannot write physical-check output");
    return -1;
}

int hwa_physical_file_write(FILE *stream,
                            const HWAPhysicalCheckSet *set,
                            char *error,
                            size_t error_size)
{
    HWANumericLocale locale;
    int status;
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_physical_error(error, error_size,
                           "cannot enter the C numeric locale for physical output");
        return -1;
    }
    status = hwa_physical_file_write_impl(
        stream, set, error, error_size);
    if (hwa_c_numeric_locale_end(&locale) != 0) {
        if (status == 0 || error == NULL || error_size == 0U ||
            error[0] == '\0') {
            hwa_physical_error(
                error, error_size,
                "cannot restore the numeric locale after physical output");
        }
        return -1;
    }
    return status;
}

static int hwa_physical_parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;
    if (text == NULL || text[0] == '\0' || text[0] == '-' ||
        text[0] == '+') return -1;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        parsed > (unsigned long long)UINT64_MAX) return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static int hwa_physical_parse_size(const char *text, size_t *value)
{
    uint64_t parsed;
    if (hwa_physical_parse_u64(text, &parsed) != 0 ||
        parsed > (uint64_t)SIZE_MAX) return -1;
    *value = (size_t)parsed;
    return 0;
}

static int hwa_physical_parse_u32(const char *text, uint32_t *value)
{
    uint64_t parsed;
    if (hwa_physical_parse_u64(text, &parsed) != 0 ||
        parsed > UINT32_MAX) return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static int hwa_physical_parse_u16(const char *text, uint16_t *value)
{
    uint64_t parsed;
    if (hwa_physical_parse_u64(text, &parsed) != 0 ||
        parsed > UINT16_MAX) return -1;
    *value = (uint16_t)parsed;
    return 0;
}

static int hwa_physical_parse_bool(const char *text, int *value)
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

static int hwa_physical_parse_double(const char *text, double *value)
{
    char *end = NULL;
    double parsed;
    if (text == NULL || text[0] == '\0') return -1;
    errno = 0;
    parsed = strtod(text, &end);
    if (end == text || *end != '\0' || !isfinite(parsed) ||
        (errno == ERANGE && fpclassify(parsed) != FP_SUBNORMAL)) return -1;
    *value = parsed == 0.0 ? 0.0 : parsed;
    return 0;
}

static int hwa_physical_hex_nibble(unsigned char byte)
{
    if (byte >= (unsigned char)'0' && byte <= (unsigned char)'9') {
        return (int)(byte - (unsigned char)'0');
    }
    if (byte >= (unsigned char)'a' && byte <= (unsigned char)'f') {
        return (int)(byte - (unsigned char)'a') + 10;
    }
    return -1;
}

static char *hwa_physical_decode_path(const char *hex)
{
    size_t length = strlen(hex);
    size_t index;
    char *path;
    if (length == 0U || (length & 1U) != 0U ||
        length / 2U > HWA_PHYSICAL_FILE_MAX_FIELD_BYTES) return NULL;
    path = (char *)malloc(length / 2U + 1U);
    if (path == NULL) return NULL;
    for (index = 0U; index < length; index += 2U) {
        int high = hwa_physical_hex_nibble((unsigned char)hex[index]);
        int low = hwa_physical_hex_nibble((unsigned char)hex[index + 1U]);
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

typedef enum HWAPhysicalReadSection {
    HWA_PHYSICAL_READ_MAGIC = 1,
    HWA_PHYSICAL_READ_META = 2,
    HWA_PHYSICAL_READ_INPUT = 3,
    HWA_PHYSICAL_READ_CHECK = 4,
    HWA_PHYSICAL_READ_FINDING = 5,
    HWA_PHYSICAL_READ_WARNING = 6,
    HWA_PHYSICAL_READ_DONE = 7
} HWAPhysicalReadSection;

typedef struct HWAPhysicalReadState {
    const HWAPhysicalOptions *limits;
    HWAPhysicalCheckSet *set;
    HWAPhysicalReadSection section;
    size_t meta_index;
    size_t source_index;
    size_t check_index;
    size_t finding_index;
    size_t warning_index;
    uint64_t work_live;
    int arrays_ready;
} HWAPhysicalReadState;

static int hwa_physical_read_charge(HWAPhysicalReadState *state,
                                    uint64_t bytes)
{
    if (state->work_live > state->limits->max_work_bytes ||
        bytes > state->limits->max_work_bytes - state->work_live) return -1;
    state->work_live += bytes;
    return 0;
}

static char *hwa_physical_read_copy(HWAPhysicalReadState *state,
                                    const char *text)
{
    size_t length = strlen(text);
    char *copy;
    if (length > HWA_PHYSICAL_FILE_MAX_FIELD_BYTES ||
        length == SIZE_MAX ||
        hwa_physical_read_charge(state, (uint64_t)length + 1U) != 0) {
        return NULL;
    }
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        state->work_live -= (uint64_t)length + 1U;
        return NULL;
    }
    memcpy(copy, text, length + 1U);
    return copy;
}

static int hwa_physical_read_arrays(HWAPhysicalReadState *state,
                                    char *error,
                                    size_t error_size)
{
    HWAPhysicalCheckSet *set = state->set;
    uint64_t bytes;
    size_t max_sources;
    if (state->arrays_ready) return 0;
    if (state->limits->max_bindings > SIZE_MAX - 2U) {
        hwa_physical_error(error, error_size,
                           "current physical binding limit overflows");
        return -1;
    }
    max_sources = state->limits->max_bindings + 2U;
    if (set->source_count < 2U || set->source_count > max_sources ||
        set->check_count > state->limits->max_checks ||
        set->finding_count > state->limits->max_findings ||
        set->warning_count > state->limits->max_warnings ||
        set->transform_count > state->limits->max_transforms ||
        set->pair_evaluations > state->limits->max_pair_evaluations) {
        hwa_physical_error(error, error_size,
                           "physical result exceeds a current row limit");
        return -1;
    }
#define HWA_PHYSICAL_ALLOC_ROWS(member, count, type)                         \
    do {                                                                     \
        if ((count) > SIZE_MAX / sizeof(type)) {                             \
            hwa_physical_error(error, error_size,                            \
                               "physical result allocation overflows");      \
            return -1;                                                       \
        }                                                                    \
        bytes = (uint64_t)(count) * (uint64_t)sizeof(type);                  \
        if (hwa_physical_read_charge(state, bytes) != 0) {                   \
            hwa_physical_error(error, error_size,                            \
                               "physical result exceeds the work-byte limit");\
            return -1;                                                       \
        }                                                                    \
        if ((count) != 0U) {                                                 \
            set->member = (type *)calloc((count), sizeof(type));             \
            if (set->member == NULL) {                                       \
                state->work_live -= bytes;                                   \
                hwa_physical_error(error, error_size,                        \
                                   "out of memory for physical result");     \
                return -1;                                                   \
            }                                                                \
        }                                                                    \
    } while (0)
    HWA_PHYSICAL_ALLOC_ROWS(sources, set->source_count, HWAPhysicalSource);
    HWA_PHYSICAL_ALLOC_ROWS(checks, set->check_count, HWAPhysicalCheck);
    HWA_PHYSICAL_ALLOC_ROWS(findings, set->finding_count, HWAPhysicalFinding);
    HWA_PHYSICAL_ALLOC_ROWS(warnings, set->warning_count, HWAPhysicalWarning);
#undef HWA_PHYSICAL_ALLOC_ROWS
    state->arrays_ready = 1;
    return 0;
}

static int hwa_physical_read_meta_value(HWAPhysicalReadState *state,
                                        size_t index,
                                        const char *value)
{
    HWAPhysicalCheckSet *set = state->set;
    HWAPhysicalOptions *options = &set->options;
    HWAProfileComparisonOptions *profile = &options->profile_limits;
    uint64_t u64;
    size_t size;
    switch (index) {
    case 0U: return hwa_physical_string_valid(value, 0) ? 0 : -1;
    case 1U:
        return strcmp(value, HWA_PHYSICAL_CHECK_METHOD_VERSION) == 0 ? 0 : -1;
    case 2U:
    case 3U:
    case 4U:
    case 5U:
    case 7U:
    case 8U:
        return hwa_physical_string_valid(value, 0) ? 0 : -1;
    case 6U:
        return hwa_physical_parse_u64(value, &u64) == 0 && u64 != 0U ? 0 : -1;
    case 9U:
        return hwa_physical_parse_size(value,
                                       &options->decode_block_frames);
    case 10U: return hwa_physical_parse_size(value, &options->fft_size);
    case 11U: return hwa_physical_parse_size(value, &options->hop_size);
    case 12U:
        return hwa_physical_parse_double(value,
                                         &options->spectral_floor_dbfs);
    case 13U:
        return hwa_physical_parse_u64(value, &options->max_wave_bytes);
    case 14U:
        return hwa_physical_parse_u64(value, &options->max_wave_frames);
    case 15U:
        return hwa_physical_parse_u64(value, &options->max_work_bytes);
    case 16U:
        return hwa_physical_parse_u64(value,
                                      &options->max_pair_evaluations);
    case 17U:
        return hwa_physical_parse_size(value, &options->max_bindings);
    case 18U:
        return hwa_physical_parse_size(value, &options->max_transforms);
    case 19U: return hwa_physical_parse_size(value, &options->max_modes);
    case 20U: return hwa_physical_parse_size(value, &options->max_checks);
    case 21U: return hwa_physical_parse_size(value, &options->max_findings);
    case 22U: return hwa_physical_parse_size(value, &options->max_warnings);
    case 23U:
        return hwa_physical_parse_u64(value, &profile->max_input_bytes);
    case 24U:
        return hwa_physical_parse_u64(value, &profile->max_work_bytes);
    case 25U:
        return hwa_physical_parse_size(value, &profile->max_contexts);
    case 26U:
        return hwa_physical_parse_size(value, &profile->max_measurements);
    case 27U: return hwa_physical_parse_size(value, &profile->max_groups);
    case 28U:
        return hwa_physical_parse_size(value, &profile->max_group_members);
    case 29U:
        return hwa_physical_parse_size(value, &profile->max_statistics);
    case 30U:
        return hwa_physical_parse_size(value, &profile->max_warnings);
    case 31U:
        return hwa_physical_parse_size(value, &profile->max_distributions);
    case 32U: return hwa_physical_parse_size(value, &profile->max_gaps);
    case 33U:
        return hwa_physical_parse_u64(value, &set->retained_work_bytes);
    case 34U:
        return hwa_physical_parse_u64(value, &set->pair_evaluations);
    case 35U:
        return hwa_physical_parse_size(value, &set->transform_count);
    case 36U:
        return hwa_physical_parse_size(value, &set->source_count);
    case 37U:
        return hwa_physical_parse_size(value, &set->check_count);
    case 38U:
        return hwa_physical_parse_size(value, &set->finding_count);
    case 39U:
        return hwa_physical_parse_size(value, &set->warning_count);
    default:
        (void)size;
        return -1;
    }
}

static int hwa_physical_read_take_path(HWAPhysicalReadState *state,
                                       const char *hex,
                                       char **target)
{
    size_t hex_length = strlen(hex);
    uint64_t bytes;
    char *path;
    if (hex_length == 0U || (hex_length & 1U) != 0U ||
        hex_length / 2U > HWA_PHYSICAL_FILE_MAX_FIELD_BYTES) return -1;
    bytes = (uint64_t)(hex_length / 2U) + 1U;
    if (hwa_physical_read_charge(state, bytes) != 0) return -1;
    path = hwa_physical_decode_path(hex);
    if (path == NULL) {
        state->work_live -= bytes;
        return -1;
    }
    *target = path;
    return 0;
}

static int hwa_physical_read_optional_double(const char *text,
                                             int valid,
                                             double *value)
{
    *value = 0.0;
    if (!valid) return text[0] == '\0' ? 0 : -1;
    return hwa_physical_parse_double(text, value);
}

static int hwa_physical_read_source(HWAPhysicalReadState *state,
                                    char **fields,
                                    size_t field_count)
{
    HWAPhysicalSource *source;
    uint64_t id;
    int is_wave;
    if (field_count != 17U || state->source_index >= state->set->source_count ||
        hwa_physical_parse_u64(fields[1], &id) != 0 ||
        id != (uint64_t)state->source_index + 1U ||
        !hwa_physical_sha_valid(fields[4]) ||
        hwa_physical_parse_bool(fields[5], &is_wave) != 0) return -1;
    source = &state->set->sources[state->source_index];
    source->id = id;
    source->role = hwa_physical_read_copy(state, fields[2]);
    if (source->role == NULL ||
        hwa_physical_read_take_path(state, fields[3], &source->path) != 0) {
        return -1;
    }
    memcpy(source->sha256, fields[4], HWA_SHA256_HEX_SIZE);
    source->is_wave = is_wave;
    if (!is_wave) {
        size_t index;
        if (state->source_index >= 2U) return -1;
        for (index = 6U; index < 17U; ++index) {
            if (fields[index][0] != '\0') return -1;
        }
    } else {
        HWAFormat *format = &source->format;
        uint16_t channels;
        if (state->source_index < 2U ||
            (strcmp(fields[6], "RIFF") != 0 &&
             strcmp(fields[6], "RF64") != 0) ||
            (strcmp(fields[7], "PCM") != 0 &&
             strcmp(fields[7], "IEEE float") != 0) ||
            hwa_physical_parse_u16(fields[8], &channels) != 0 ||
            hwa_physical_parse_u32(fields[9], &format->sample_rate_hz) != 0 ||
            hwa_physical_parse_u16(fields[10],
                                   &format->bits_per_sample) != 0 ||
            hwa_physical_parse_u16(fields[11],
                                   &format->valid_bits_per_sample) != 0 ||
            hwa_physical_parse_u16(fields[12], &format->block_align) != 0 ||
            hwa_physical_parse_u32(fields[13], &format->channel_mask) != 0 ||
            hwa_physical_parse_u64(fields[14], &format->frames) != 0 ||
            hwa_physical_parse_u64(fields[15], &format->data_bytes) != 0 ||
            hwa_physical_parse_double(fields[16],
                                      &format->duration_seconds) != 0 ||
            format->frames > state->limits->max_wave_frames ||
            format->data_bytes > state->limits->max_wave_bytes) {
            return -1;
        }
        format->container = strcmp(fields[6], "RIFF") == 0 ?
                                HWA_CONTAINER_RIFF : HWA_CONTAINER_RF64;
        format->encoding = strcmp(fields[7], "PCM") == 0 ?
                               HWA_ENCODING_PCM : HWA_ENCODING_IEEE_FLOAT;
        format->channels = channels;
    }
    if (state->source_index == 0U) {
        state->set->reference_measures_path =
            hwa_physical_read_copy(state, source->path);
        if (state->set->reference_measures_path == NULL) return -1;
        memcpy(state->set->reference_measures_sha256, source->sha256,
               HWA_SHA256_HEX_SIZE);
    } else if (state->source_index == 1U) {
        state->set->model_measures_path =
            hwa_physical_read_copy(state, source->path);
        if (state->set->model_measures_path == NULL) return -1;
        memcpy(state->set->model_measures_sha256, source->sha256,
               HWA_SHA256_HEX_SIZE);
    }
    state->source_index++;
    return 0;
}

static int hwa_physical_read_check(HWAPhysicalReadState *state,
                                   char **fields,
                                   size_t field_count)
{
    HWAPhysicalCheck *check;
    uint64_t id;
    if (field_count != 18U || state->check_index >= state->set->check_count ||
        hwa_physical_parse_u64(fields[1], &id) != 0 ||
        id != (uint64_t)state->check_index + 1U) return -1;
    check = &state->set->checks[state->check_index];
    check->id = id;
    check->scope = hwa_physical_read_copy(state, fields[2]);
    check->case_id = hwa_physical_read_copy(state, fields[3]);
    check->element = hwa_physical_read_copy(state, fields[4]);
    if (check->scope == NULL || check->case_id == NULL ||
        check->element == NULL ||
        hwa_physical_check_kind_from_name(fields[5], &check->kind) != 0 ||
        hwa_physical_parse_u32(fields[6], &check->index) != 0 ||
        hwa_physical_unit_from_name(fields[7], &check->unit) != 0 ||
        hwa_physical_availability_from_name(
            fields[8], &check->availability) != 0 ||
        hwa_physical_parse_double(fields[12], &check->confidence) != 0 ||
        hwa_physical_parse_u32(fields[13],
                               &check->evidence_flags) != 0 ||
        hwa_physical_parse_u32(fields[14],
                               &check->quality_flags) != 0 ||
        hwa_physical_parse_bool(fields[15],
                                &check->reference_valid) != 0 ||
        hwa_physical_parse_bool(fields[16], &check->model_valid) != 0 ||
        hwa_physical_parse_bool(fields[17], &check->delta_valid) != 0 ||
        hwa_physical_read_optional_double(
            fields[9], check->reference_valid, &check->reference_value) != 0 ||
        hwa_physical_read_optional_double(
            fields[10], check->model_valid, &check->model_value) != 0 ||
        hwa_physical_read_optional_double(
            fields[11], check->delta_valid, &check->delta) != 0) {
        return -1;
    }
    state->check_index++;
    return 0;
}

static int hwa_physical_read_finding(HWAPhysicalReadState *state,
                                     char **fields,
                                     size_t field_count)
{
    HWAPhysicalFinding *finding;
    uint64_t id;
    if (field_count != 11U ||
        state->finding_index >= state->set->finding_count ||
        hwa_physical_parse_u64(fields[1], &id) != 0 ||
        id != (uint64_t)state->finding_index + 1U) return -1;
    finding = &state->set->findings[state->finding_index];
    finding->id = id;
    finding->code = hwa_physical_read_copy(state, fields[4]);
    finding->message = hwa_physical_read_copy(state, fields[5]);
    if (finding->code == NULL || finding->message == NULL ||
        hwa_physical_finding_class_from_name(
            fields[2], &finding->finding_class) != 0 ||
        hwa_physical_severity_from_name(fields[3], &finding->severity) != 0 ||
        hwa_physical_parse_size(fields[8], &finding->rank) != 0 ||
        hwa_physical_parse_bool(fields[9],
                                &finding->check_id_valid) != 0 ||
        hwa_physical_parse_bool(fields[10],
                                &finding->score_valid) != 0) {
        return -1;
    }
    if (finding->check_id_valid) {
        if (hwa_physical_parse_u64(fields[6], &finding->check_id) != 0) {
            return -1;
        }
    } else if (fields[6][0] != '\0') {
        return -1;
    }
    if (hwa_physical_read_optional_double(
            fields[7], finding->score_valid, &finding->score) != 0) return -1;
    state->finding_index++;
    return 0;
}

static int hwa_physical_read_warning(HWAPhysicalReadState *state,
                                     char **fields,
                                     size_t field_count)
{
    HWAPhysicalWarning *warning;
    uint64_t id;
    if (field_count != 8U ||
        state->warning_index >= state->set->warning_count ||
        hwa_physical_parse_u64(fields[1], &id) != 0 ||
        id != (uint64_t)state->warning_index + 1U) return -1;
    warning = &state->set->warnings[state->warning_index];
    warning->id = id;
    warning->code = hwa_physical_read_copy(state, fields[2]);
    warning->message = hwa_physical_read_copy(state, fields[3]);
    if (warning->code == NULL || warning->message == NULL ||
        hwa_physical_parse_bool(fields[6],
                                &warning->source_id_valid) != 0 ||
        hwa_physical_parse_bool(fields[7],
                                &warning->check_id_valid) != 0) return -1;
    if (warning->source_id_valid) {
        if (hwa_physical_parse_u64(fields[4], &warning->source_id) != 0) {
            return -1;
        }
    } else if (fields[4][0] != '\0') {
        return -1;
    }
    if (warning->check_id_valid) {
        if (hwa_physical_parse_u64(fields[5], &warning->check_id) != 0) {
            return -1;
        }
    } else if (fields[5][0] != '\0') {
        return -1;
    }
    state->warning_index++;
    return 0;
}

static int hwa_physical_read_row(char **fields,
                                 size_t field_count,
                                 size_t row,
                                 void *user,
                                 char *error,
                                 size_t error_size)
{
    HWAPhysicalReadState *state = (HWAPhysicalReadState *)user;
    int result = -1;
    if (state->section == HWA_PHYSICAL_READ_MAGIC) {
        uint64_t version;
        if (field_count == 2U && strcmp(fields[0], "HWA_PHYSICAL") == 0 &&
            hwa_physical_parse_u64(fields[1], &version) == 0 &&
            version == HWA_PHYSICAL_FILE_SCHEMA_VERSION) {
            state->section = HWA_PHYSICAL_READ_META;
            return 0;
        }
    } else if (state->section == HWA_PHYSICAL_READ_META) {
        if (field_count == 4U && strcmp(fields[0], "META") == 0 &&
            state->meta_index <
                sizeof(hwa_physical_meta_keys) /
                    sizeof(hwa_physical_meta_keys[0]) &&
            strcmp(fields[1],
                   hwa_physical_meta_keys[state->meta_index].key) == 0 &&
            strcmp(fields[3],
                   hwa_physical_meta_keys[state->meta_index].unit) == 0 &&
            hwa_physical_read_meta_value(state, state->meta_index,
                                         fields[2]) == 0) {
            state->meta_index++;
            if (state->meta_index ==
                sizeof(hwa_physical_meta_keys) /
                    sizeof(hwa_physical_meta_keys[0])) {
                if (!hwa_physical_options_valid(&state->set->options) ||
                    hwa_physical_read_arrays(state, error, error_size) != 0) {
                    return -1;
                }
                state->section = HWA_PHYSICAL_READ_INPUT;
            }
            return 0;
        }
    } else {
        if (state->section == HWA_PHYSICAL_READ_INPUT &&
            state->source_index == state->set->source_count) {
            state->section = HWA_PHYSICAL_READ_CHECK;
        }
        if (state->section == HWA_PHYSICAL_READ_CHECK &&
            state->check_index == state->set->check_count) {
            state->section = HWA_PHYSICAL_READ_FINDING;
        }
        if (state->section == HWA_PHYSICAL_READ_FINDING &&
            state->finding_index == state->set->finding_count) {
            state->section = HWA_PHYSICAL_READ_WARNING;
        }
        if (state->section == HWA_PHYSICAL_READ_WARNING &&
            state->warning_index == state->set->warning_count) {
            state->section = HWA_PHYSICAL_READ_DONE;
        }
        if (state->section == HWA_PHYSICAL_READ_INPUT &&
            strcmp(fields[0], "INPUT") == 0) {
            result = hwa_physical_read_source(state, fields, field_count);
        } else if (state->section == HWA_PHYSICAL_READ_CHECK &&
                   strcmp(fields[0], "CHECK") == 0) {
            result = hwa_physical_read_check(state, fields, field_count);
        } else if (state->section == HWA_PHYSICAL_READ_FINDING &&
                   strcmp(fields[0], "FINDING") == 0) {
            result = hwa_physical_read_finding(state, fields, field_count);
        } else if (state->section == HWA_PHYSICAL_READ_WARNING &&
                   strcmp(fields[0], "WARNING") == 0) {
            result = hwa_physical_read_warning(state, fields, field_count);
        }
        if (result == 0) return 0;
    }
    hwa_set_error(error, error_size,
                  "invalid physical result row %zu", row);
    return -1;
}

typedef int (*HWAPhysicalRowFunction)(char **fields,
                                      size_t field_count,
                                      size_t row,
                                      void *user,
                                      char *error,
                                      size_t error_size);

static int hwa_physical_csv_record(const unsigned char *data,
                                   size_t size,
                                   size_t row,
                                   HWAPhysicalRowFunction function,
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
    fields = (char **)malloc(HWA_PHYSICAL_FILE_MAX_FIELDS * sizeof(*fields));
    storage = (char *)malloc(size + 1U);
    if (fields == NULL || storage == NULL) {
        free(storage);
        free(fields);
        hwa_physical_error(error, error_size,
                           "out of memory while parsing physical result");
        return -1;
    }
    while (!row_done) {
        size_t field_start = output;
        int quoted = 0;
        int closed_quote = 0;
        if (field_count == HWA_PHYSICAL_FILE_MAX_FIELDS) {
            hwa_set_error(error, error_size,
                          "physical result row %zu has too many fields", row);
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
                hwa_set_error(error, error_size,
                              "physical result row %zu contains a NUL byte",
                              row);
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
                        hwa_set_error(error, error_size,
                                      "physical result row %zu has a bare carriage return",
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
                hwa_set_error(error, error_size,
                              "physical result row %zu has an unquoted quote",
                              row);
                goto fail;
            }
            if (byte == (unsigned char)',' ||
                byte == (unsigned char)'\r' ||
                byte == (unsigned char)'\n') break;
            storage[output++] = (char)byte;
            position++;
        }
        if (quoted && !closed_quote) {
            hwa_set_error(error, error_size,
                          "physical result row %zu has an unterminated quote",
                          row);
            goto fail;
        }
        if (output - field_start > HWA_PHYSICAL_FILE_MAX_FIELD_BYTES) {
            hwa_set_error(error, error_size,
                          "physical result row %zu has an oversized field",
                          row);
            goto fail;
        }
        storage[output++] = '\0';
        if (quoted && position < size &&
            data[position] != (unsigned char)',' &&
            data[position] != (unsigned char)'\r' &&
            data[position] != (unsigned char)'\n') {
            hwa_set_error(error, error_size,
                          "physical result row %zu has bytes after a quote",
                          row);
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
                hwa_set_error(error, error_size,
                              "physical result row %zu has bad line ending",
                              row);
                goto fail;
            }
            position += 2U;
            row_done = 1;
        } else {
            if (position + 1U != size) {
                hwa_set_error(error, error_size,
                              "physical result row %zu has trailing bytes",
                              row);
                goto fail;
            }
            position++;
            row_done = 1;
        }
    }
    if (function(fields, field_count, row, user, error, error_size) != 0) {
        goto fail;
    }
    free(storage);
    free(fields);
    return 0;

fail:
    free(storage);
    free(fields);
    return -1;
}

static void hwa_physical_hash_input_byte(
    HWASha256 *context,
    unsigned char buffer[HWA_PHYSICAL_HASH_BUFFER_BYTES],
    size_t *buffer_size,
    unsigned char byte)
{
    buffer[(*buffer_size)++] = byte;
    if (*buffer_size == HWA_PHYSICAL_HASH_BUFFER_BYTES) {
        hwa_sha256_update(context, buffer, *buffer_size);
        *buffer_size = 0U;
    }
}

static int hwa_physical_record_capacity(size_t field_count,
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

static int hwa_physical_stream_rows(const char *path,
                                    const HWAPhysicalOptions *limits,
                                    const HWAPhysicalFileIdentity *expected,
                                    char stream_sha256[HWA_SHA256_HEX_SIZE],
                                    HWAPhysicalReadState *state,
                                    char *error,
                                    size_t error_size)
{
    FILE *stream = NULL;
    HWASha256 hash_context;
    unsigned char hash_buffer[HWA_PHYSICAL_HASH_BUFFER_BYTES];
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
    int result = -1;
    HWAPhysicalFileIdentity before;
    HWAPhysicalFileIdentity opened;
    stream_sha256[0] = '\0';
    if (hwa_physical_record_capacity(HWA_PHYSICAL_FILE_MAX_FIELDS,
                                     HWA_PHYSICAL_FILE_MAX_FIELD_BYTES,
                                     &maximum_record) != 0) {
        hwa_physical_error(error, error_size,
                           "physical result record limit overflows");
        return -1;
    }
    parser_bytes = (uint64_t)maximum_record;
    if (hwa_physical_read_charge(state, parser_bytes) != 0) {
        hwa_physical_error(error, error_size,
                           "physical row buffer exceeds the work-byte limit");
        return -1;
    }
    record = (unsigned char *)malloc(maximum_record);
    if (record == NULL) {
        state->work_live -= parser_bytes;
        hwa_physical_error(error, error_size,
                           "out of memory for physical row buffer");
        return -1;
    }
    if (hwa_physical_path_identity(path, &before, error, error_size) != 0 ||
        !hwa_physical_same_identity(expected, &before)) {
        hwa_physical_error(error, error_size,
                           "physical result changed before it was opened");
        goto cleanup;
    }
    if (before.size > limits->profile_limits.max_input_bytes) {
        hwa_physical_error(error, error_size,
                           "physical result exceeds the current byte limit");
        goto cleanup;
    }
    stream = fopen(path, "rb");
    if (stream == NULL) {
        hwa_set_error(error, error_size,
                      "cannot open physical result '%s': %s",
                      path, strerror(errno));
        goto cleanup;
    }
    if (hwa_physical_stream_identity(stream, &opened) != 0 ||
        !hwa_physical_same_identity(expected, &opened)) {
        hwa_physical_error(error, error_size,
                           "physical result changed before it was opened");
        goto cleanup;
    }
    hwa_sha256_init(&hash_context);
    for (;;) {
        int byte = fgetc(stream);
        int row_done = 0;
        if (byte == EOF) {
            if (ferror(stream)) {
                hwa_physical_error(error, error_size,
                                   "cannot read physical result");
                goto cleanup;
            }
            if (in_quotes) {
                hwa_physical_error(error, error_size,
                                   "physical result has an unterminated quote");
                goto cleanup;
            }
            row_done = record_size != 0U;
        } else {
            hwa_physical_hash_input_byte(
                &hash_context, hash_buffer, &hash_buffer_size,
                (unsigned char)byte);
            if (source_bytes == limits->profile_limits.max_input_bytes ||
                record_size == maximum_record) {
                hwa_physical_error(error, error_size,
                                   "physical result exceeds a byte or row limit");
                goto cleanup;
            }
            source_bytes++;
            record[record_size++] = (unsigned char)byte;
            if (in_quotes && byte == '"') {
                int next = fgetc(stream);
                if (next == '"') {
                    if (source_bytes ==
                            limits->profile_limits.max_input_bytes ||
                        record_size == maximum_record) {
                        hwa_physical_error(error, error_size,
                                           "physical result row exceeds limits");
                        goto cleanup;
                    }
                    source_bytes++;
                    record[record_size++] = (unsigned char)next;
                    hwa_physical_hash_input_byte(
                        &hash_context, hash_buffer, &hash_buffer_size,
                        (unsigned char)next);
                } else {
                    in_quotes = 0;
                    if (next != EOF && ungetc(next, stream) == EOF) {
                        hwa_physical_error(error, error_size,
                                           "cannot parse physical result");
                        goto cleanup;
                    }
                }
            } else if (!in_quotes && field_start && byte == '"') {
                in_quotes = 1;
                field_start = 0;
            } else if (!in_quotes && byte == ',') {
                field_start = 1;
            } else if (!in_quotes && byte == '\n') {
                row_done = 1;
                field_start = 1;
            } else if (!in_quotes && byte == '\r') {
                int next = fgetc(stream);
                if (next != '\n' ||
                    source_bytes == limits->profile_limits.max_input_bytes ||
                    record_size == maximum_record) {
                    hwa_physical_error(error, error_size,
                                       "physical result has a bare carriage return");
                    goto cleanup;
                }
                source_bytes++;
                record[record_size++] = (unsigned char)next;
                hwa_physical_hash_input_byte(
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
                          (uint64_t)HWA_PHYSICAL_FILE_MAX_FIELDS *
                              (uint64_t)sizeof(char *);
            if (hwa_physical_read_charge(state, row_scratch) != 0) {
                hwa_physical_error(error, error_size,
                                   "physical row parse exceeds the work-byte limit");
                goto cleanup;
            }
            if (hwa_physical_csv_record(
                    record, record_size, row, hwa_physical_read_row, state,
                    error, error_size) != 0) {
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
        hwa_physical_error(error, error_size,
                           "physical result size changed while reading");
        goto cleanup;
    }
    if (hash_buffer_size != 0U) {
        hwa_sha256_update(&hash_context, hash_buffer, hash_buffer_size);
    }
    if (hash_context.overflowed) {
        hwa_physical_error(error, error_size,
                           "physical result is too large to hash");
        goto cleanup;
    }
    hwa_sha256_final(&hash_context, hash_digest);
    hwa_sha256_hex(hash_digest, stream_sha256);
    if (fclose(stream) != 0) {
        stream = NULL;
        hwa_physical_error(error, error_size,
                           "cannot close physical result after reading");
        goto cleanup;
    }
    stream = NULL;
    result = 0;

cleanup:
    if (stream != NULL) (void)fclose(stream);
    free(record);
    state->work_live -= parser_bytes;
    return result;
}

static int hwa_physical_read_complete(HWAPhysicalReadState *state)
{
    return state->meta_index ==
               sizeof(hwa_physical_meta_keys) /
                   sizeof(hwa_physical_meta_keys[0]) &&
           state->source_index == state->set->source_count &&
           state->check_index == state->set->check_count &&
           state->finding_index == state->set->finding_count &&
           state->warning_index == state->set->warning_count;
}

static void hwa_physical_use_current_limits(
    HWAPhysicalOptions *options,
    const HWAPhysicalOptions *current)
{
    options->max_wave_bytes = current->max_wave_bytes;
    options->max_wave_frames = current->max_wave_frames;
    options->max_work_bytes = current->max_work_bytes;
    options->max_pair_evaluations = current->max_pair_evaluations;
    options->max_bindings = current->max_bindings;
    options->max_transforms = current->max_transforms;
    options->max_modes = current->max_modes;
    options->max_checks = current->max_checks;
    options->max_findings = current->max_findings;
    options->max_warnings = current->max_warnings;
    options->profile_limits = current->profile_limits;
}

static int hwa_physical_file_read_impl(
    const char *path,
    const HWAPhysicalOptions *limits,
    HWAPhysicalCheckSet *set,
    char file_sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    HWAPhysicalOptions current;
    HWAPhysicalReadState state;
    HWAPhysicalFileIdentity input_identity;
    HWAPhysicalFileIdentity final_identity;
    char parsed[HWA_SHA256_HEX_SIZE];
    char after[HWA_SHA256_HEX_SIZE];
    uint64_t saved_retained_work_bytes;
    int result = -1;
    if (set == NULL) {
        hwa_physical_error(error, error_size,
                           "physical result pointer is null");
        return -1;
    }
    if (limits != NULL) current = *limits;
    memset(set, 0, sizeof(*set));
    if (file_sha256 != NULL) file_sha256[0] = '\0';
    if (path == NULL || path[0] == '\0' || strcmp(path, "-") == 0 ||
        limits == NULL || !hwa_physical_options_valid(&current) ||
        file_sha256 == NULL) {
        hwa_physical_error(error, error_size,
                           "invalid physical result reader arguments");
        return -1;
    }
    if (hwa_physical_path_identity(
            path, &input_identity, error, error_size) != 0) return -1;
    memset(&state, 0, sizeof(state));
    state.limits = &current;
    state.set = set;
    state.section = HWA_PHYSICAL_READ_MAGIC;
    if (hwa_physical_stream_rows(
            path, &current, &input_identity, parsed, &state,
            error, error_size) != 0 ||
        !hwa_physical_read_complete(&state)) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_physical_error(error, error_size,
                               "physical result is incomplete");
        }
        goto cleanup;
    }
    saved_retained_work_bytes = set->retained_work_bytes;
    if (saved_retained_work_bytes == 0U ||
        saved_retained_work_bytes > set->options.max_work_bytes) {
        hwa_physical_error(error, error_size,
                           "invalid saved physical retained-work byte count");
        goto cleanup;
    }
    set->retained_work_bytes = state.work_live;
    if (hwa_physical_check_set_validate_with_score_ulps(
            set, HWA_PHYSICAL_READER_SCORE_MAX_ULPS,
            error, error_size) != 0) {
        goto cleanup;
    }
    if (!hwa_physical_body_modes_valid(set, current.max_modes)) {
        hwa_physical_error(error, error_size,
                           "physical result exceeds the current mode limit");
        goto cleanup;
    }
    hwa_physical_normalize_findings(set);
    hwa_physical_use_current_limits(&set->options, &current);
    if (hwa_physical_check_set_validate(set, error, error_size) != 0) {
        goto cleanup;
    }
    if (hwa_physical_path_identity(
            path, &final_identity, error, error_size) != 0 ||
        !hwa_physical_same_identity(&input_identity, &final_identity) ||
        hwa_sha256_file(path, current.profile_limits.max_input_bytes,
                        after, error, error_size) != 0 ||
        strcmp(parsed, after) != 0) {
        hwa_physical_error(error, error_size,
                           "physical result changed while it was read");
        goto cleanup;
    }
    memcpy(file_sha256, parsed, HWA_SHA256_HEX_SIZE);
    result = 0;

cleanup:
    if (result != 0) {
        if (set->sources == NULL) set->source_count = 0U;
        if (set->checks == NULL) set->check_count = 0U;
        if (set->findings == NULL) set->finding_count = 0U;
        if (set->warnings == NULL) set->warning_count = 0U;
        hwa_physical_check_set_free(set);
        file_sha256[0] = '\0';
    }
    return result;
}

int hwa_physical_file_read(
    const char *path,
    const HWAPhysicalOptions *limits,
    HWAPhysicalCheckSet *set,
    char file_sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    HWANumericLocale locale;
    int status;
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        if (set != NULL) memset(set, 0, sizeof(*set));
        if (file_sha256 != NULL) file_sha256[0] = '\0';
        hwa_physical_error(error, error_size,
                           "cannot enter the C numeric locale for physical input");
        return -1;
    }
    status = hwa_physical_file_read_impl(
        path, limits, set, file_sha256, error, error_size);
    if (hwa_c_numeric_locale_end(&locale) != 0) {
        if (status == 0 && set != NULL) hwa_physical_check_set_free(set);
        if (file_sha256 != NULL) file_sha256[0] = '\0';
        if (status == 0 || error == NULL || error_size == 0U ||
            error[0] == '\0') {
            hwa_physical_error(
                error, error_size,
                "cannot restore the numeric locale after physical input");
        }
        return -1;
    }
    return status;
}

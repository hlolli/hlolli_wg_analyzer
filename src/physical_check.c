#if !defined(_WIN32)
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "hlolli_wg_analyzer.h"
#include "dsp.h"
#include "internal.h"
#include "measure_file.h"
#include "numeric_locale.h"
#include "physical_check.h"
#include "physical_file.h"
#include "sha256.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define HWA_PHYSICAL_PI 3.14159265358979323846264338327950288
#define HWA_PHYSICAL_REPEAT_BLOCK 32U
#define HWA_PHYSICAL_HASH_BLOCK 65536U

typedef struct HWAFileIdentity {
    uint64_t device;
    uint64_t inode;
    uint64_t size;
    int valid;
} HWAFileIdentity;

typedef struct HWAPhysicalMode {
    double frequency_hz;
    double bandwidth_hz;
    double q;
    double prominence_db;
    double decay_seconds;
    double power;
} HWAPhysicalMode;

typedef struct HWAProbe {
    HWAPhysicalRole role;
    HWAFileIdentity identity;
    HWAFormat format;
    double *samples;
    uint64_t frame_count;
    double *spectrum;
    size_t spectrum_bins;
    HWAPhysicalMode *modes;
    size_t mode_count;
    size_t strongest_bin;
    double strongest_frequency_hz;
    uint64_t nonfinite_count;
    double dc_offset;
    double dc_drift;
    double clip_fraction;
    double hard_bound_fraction;
    double repeated_block_fraction;
    double stuck_state_fraction;
    double max_step_dbfs;
    double runaway_slope_db_per_second;
    double return_level_dbfs;
    double high_band_ratio;
    double subharmonic_ratio;
    double fixed_tone_prominence_db;
    double denormal_fraction;
    double rms;
    uint64_t work_bytes;
    int spectrum_valid;
} HWAProbe;

typedef struct HWAPhysicalName {
    int value;
    const char *name;
} HWAPhysicalName;

static const HWAPhysicalName hwa_physical_check_names[] = {
    {HWA_PHYSICAL_ELEMENT_TRAIT_DELTA, "element_trait_delta"},
    {HWA_PHYSICAL_ELEMENT_REFERENCE_DISTANCE,
     "element_reference_distance"},
    {HWA_PHYSICAL_ELEMENT_MODEL_DISTANCE, "element_model_distance"},
    {HWA_PHYSICAL_ELEMENT_DISTINCTNESS_RATIO,
     "element_distinctness_ratio"},
    {HWA_PHYSICAL_ELEMENT_GAIN_ONLY_SCORE, "element_gain_only_score"},
    {HWA_PHYSICAL_ELEMENT_PITCH_ONLY_SCORE, "element_pitch_only_score"},
    {HWA_PHYSICAL_ELEMENT_CARRYOVER_DB, "element_carryover_db"},
    {HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ, "body_mode_frequency_hz"},
    {HWA_PHYSICAL_BODY_MODE_BANDWIDTH_HZ, "body_mode_bandwidth_hz"},
    {HWA_PHYSICAL_BODY_MODE_Q, "body_mode_q"},
    {HWA_PHYSICAL_BODY_MODE_PROMINENCE_DB, "body_mode_prominence_db"},
    {HWA_PHYSICAL_BODY_MODE_DECAY_SECONDS, "body_mode_decay_seconds"},
    {HWA_PHYSICAL_BODY_MODE_PAN, "body_mode_pan"},
    {HWA_PHYSICAL_BODY_MODE_DENSITY_PER_KHZ,
     "body_mode_density_per_khz"},
    {HWA_PHYSICAL_BODY_MODE_DISTANCE_CENTS, "body_mode_distance_cents"},
    {HWA_PHYSICAL_JOINT_RESIDUAL_DB, "joint_residual_db"},
    {HWA_PHYSICAL_SHARED_GAIN_DB, "shared_gain_db"},
    {HWA_PHYSICAL_INTERMODULATION_RATIO, "intermodulation_ratio"},
    {HWA_PHYSICAL_SUM_TONE_DB, "sum_tone_db"},
    {HWA_PHYSICAL_DIFFERENCE_TONE_DB, "difference_tone_db"},
    {HWA_PHYSICAL_BEATING_DEPTH_RATIO, "beating_depth_ratio"},
    {HWA_PHYSICAL_BEATING_RATE_HZ, "beating_rate_hz"},
    {HWA_PHYSICAL_ROUGHNESS_RATIO, "roughness_ratio"},
    {HWA_PHYSICAL_PITCH_PULL_CENTS, "pitch_pull_cents"},
    {HWA_PHYSICAL_RENDER_RMS_ERROR_DB, "render_rms_error_db"},
    {HWA_PHYSICAL_RENDER_MAX_ERROR_DBFS, "render_max_error_dbfs"},
    {HWA_PHYSICAL_RENDER_CORRELATION, "render_correlation"},
    {HWA_PHYSICAL_RENDER_LAG_SAMPLES, "render_lag_samples"},
    {HWA_PHYSICAL_RENDER_PITCH_DELTA_CENTS,
     "render_pitch_delta_cents"},
    {HWA_PHYSICAL_RENDER_ATTACK_DELTA_SECONDS,
     "render_attack_delta_seconds"},
    {HWA_PHYSICAL_RENDER_DECAY_DELTA_SECONDS,
     "render_decay_delta_seconds"},
    {HWA_PHYSICAL_RENDER_SPECTRAL_DISTANCE_DB,
     "render_spectral_distance_db"},
    {HWA_PHYSICAL_DC_OFFSET, "dc_offset"},
    {HWA_PHYSICAL_DC_DRIFT, "dc_drift"},
    {HWA_PHYSICAL_CLIP_FRACTION, "clip_fraction"},
    {HWA_PHYSICAL_HARD_BOUND_FRACTION, "hard_bound_fraction"},
    {HWA_PHYSICAL_REPEATED_BLOCK_FRACTION, "repeated_block_fraction"},
    {HWA_PHYSICAL_STUCK_STATE_FRACTION, "stuck_state_fraction"},
    {HWA_PHYSICAL_MAX_STEP_DBFS, "max_step_dbfs"},
    {HWA_PHYSICAL_RUNAWAY_SLOPE_DB_PER_SECOND,
     "runaway_slope_db_per_second"},
    {HWA_PHYSICAL_RETURN_LEVEL_DBFS, "return_level_dbfs"},
    {HWA_PHYSICAL_HIGH_BAND_RATIO, "high_band_ratio"},
    {HWA_PHYSICAL_SUBHARMONIC_RATIO, "subharmonic_ratio"},
    {HWA_PHYSICAL_FIXED_TONE_PROMINENCE_DB,
     "fixed_tone_prominence_db"},
    {HWA_PHYSICAL_DENORMAL_FRACTION, "denormal_fraction"}
};

static const HWAPhysicalName hwa_physical_unit_names[] = {
    {HWA_PHYSICAL_UNIT_DBFS, "dBFS"},
    {HWA_PHYSICAL_UNIT_DB, "dB"},
    {HWA_PHYSICAL_UNIT_HZ, "Hz"},
    {HWA_PHYSICAL_UNIT_SECONDS, "seconds"},
    {HWA_PHYSICAL_UNIT_RATIO, "ratio"},
    {HWA_PHYSICAL_UNIT_CENTS, "cents"},
    {HWA_PHYSICAL_UNIT_SAMPLES, "samples"},
    {HWA_PHYSICAL_UNIT_HZ_PER_SECOND, "Hz/s"},
    {HWA_PHYSICAL_UNIT_DB_PER_SECOND, "dB/s"},
    {HWA_PHYSICAL_UNIT_COUNT_VALUE, "count"}
};

static const HWAPhysicalName hwa_physical_availability_names[] = {
    {HWA_PHYSICAL_AVAILABLE, "available"},
    {HWA_PHYSICAL_UNAVAILABLE, "unavailable"},
    {HWA_PHYSICAL_INSUFFICIENT, "insufficient"}
};

static const HWAPhysicalName hwa_physical_finding_names[] = {
    {HWA_PHYSICAL_FINDING_NONE, "none"},
    {HWA_PHYSICAL_FINDING_GAP, "gap"},
    {HWA_PHYSICAL_FINDING_FAULT, "fault"},
    {HWA_PHYSICAL_FINDING_UNAVAILABLE, "unavailable"}
};

static const HWAPhysicalName hwa_physical_severity_names[] = {
    {HWA_PHYSICAL_SEVERITY_INFO, "info"},
    {HWA_PHYSICAL_SEVERITY_WARNING, "warning"},
    {HWA_PHYSICAL_SEVERITY_CRITICAL, "critical"}
};

static const char *hwa_physical_name_for_value(
    const HWAPhysicalName *names,
    size_t count,
    int value)
{
    size_t index;
    for (index = 0U; index < count; ++index) {
        if (names[index].value == value) return names[index].name;
    }
    return NULL;
}

static int hwa_physical_value_from_name(const HWAPhysicalName *names,
                                        size_t count,
                                        const char *name,
                                        int *value)
{
    size_t index;
    if (name == NULL || value == NULL) return -1;
    for (index = 0U; index < count; ++index) {
        if (strcmp(names[index].name, name) == 0) {
            *value = names[index].value;
            return 0;
        }
    }
    return -1;
}

#define HWA_PHYSICAL_ARRAY_COUNT(array) \
    (sizeof(array) / sizeof((array)[0]))

const char *hwa_physical_check_kind_name(HWAPhysicalCheckKind kind)
{
    return hwa_physical_name_for_value(
        hwa_physical_check_names,
        HWA_PHYSICAL_ARRAY_COUNT(hwa_physical_check_names), (int)kind);
}

int hwa_physical_check_kind_from_name(const char *name,
                                      HWAPhysicalCheckKind *kind)
{
    int value;
    if (kind == NULL || hwa_physical_value_from_name(
            hwa_physical_check_names,
            HWA_PHYSICAL_ARRAY_COUNT(hwa_physical_check_names),
            name, &value) != 0) return -1;
    *kind = (HWAPhysicalCheckKind)value;
    return 0;
}

const char *hwa_physical_unit_name(HWAPhysicalUnit unit)
{
    return hwa_physical_name_for_value(
        hwa_physical_unit_names,
        HWA_PHYSICAL_ARRAY_COUNT(hwa_physical_unit_names), (int)unit);
}

int hwa_physical_unit_from_name(const char *name, HWAPhysicalUnit *unit)
{
    int value;
    if (unit == NULL || hwa_physical_value_from_name(
            hwa_physical_unit_names,
            HWA_PHYSICAL_ARRAY_COUNT(hwa_physical_unit_names),
            name, &value) != 0) return -1;
    *unit = (HWAPhysicalUnit)value;
    return 0;
}

const char *hwa_physical_availability_name(
    HWAPhysicalAvailability availability)
{
    return hwa_physical_name_for_value(
        hwa_physical_availability_names,
        HWA_PHYSICAL_ARRAY_COUNT(hwa_physical_availability_names),
        (int)availability);
}

int hwa_physical_availability_from_name(
    const char *name,
    HWAPhysicalAvailability *availability)
{
    int value;
    if (availability == NULL || hwa_physical_value_from_name(
            hwa_physical_availability_names,
            HWA_PHYSICAL_ARRAY_COUNT(hwa_physical_availability_names),
            name, &value) != 0) return -1;
    *availability = (HWAPhysicalAvailability)value;
    return 0;
}

const char *hwa_physical_finding_class_name(
    HWAPhysicalFindingClass finding_class)
{
    return hwa_physical_name_for_value(
        hwa_physical_finding_names,
        HWA_PHYSICAL_ARRAY_COUNT(hwa_physical_finding_names),
        (int)finding_class);
}

int hwa_physical_finding_class_from_name(
    const char *name,
    HWAPhysicalFindingClass *finding_class)
{
    int value;
    if (finding_class == NULL || hwa_physical_value_from_name(
            hwa_physical_finding_names,
            HWA_PHYSICAL_ARRAY_COUNT(hwa_physical_finding_names),
            name, &value) != 0) return -1;
    *finding_class = (HWAPhysicalFindingClass)value;
    return 0;
}

const char *hwa_physical_severity_name(HWAPhysicalSeverity severity)
{
    return hwa_physical_name_for_value(
        hwa_physical_severity_names,
        HWA_PHYSICAL_ARRAY_COUNT(hwa_physical_severity_names),
        (int)severity);
}

int hwa_physical_severity_from_name(const char *name,
                                    HWAPhysicalSeverity *severity)
{
    int value;
    if (severity == NULL || hwa_physical_value_from_name(
            hwa_physical_severity_names,
            HWA_PHYSICAL_ARRAY_COUNT(hwa_physical_severity_names),
            name, &value) != 0) return -1;
    *severity = (HWAPhysicalSeverity)value;
    return 0;
}

const char *hwa_physical_role_side_name(HWAPhysicalRoleSide side)
{
    if (side == HWA_PHYSICAL_ROLE_REFERENCE) return "reference";
    if (side == HWA_PHYSICAL_ROLE_MODEL) return "model";
    return NULL;
}

const char *hwa_physical_role_kind_name(HWAPhysicalRoleKind kind)
{
    switch (kind) {
    case HWA_PHYSICAL_ROLE_BODY: return "body";
    case HWA_PHYSICAL_ROLE_JOINT: return "joint";
    case HWA_PHYSICAL_ROLE_ISOLATED_A: return "isolated-a";
    case HWA_PHYSICAL_ROLE_ISOLATED_B: return "isolated-b";
    case HWA_PHYSICAL_ROLE_RENDER_BASELINE: return "render-baseline";
    case HWA_PHYSICAL_ROLE_RENDER_VARIANT: return "render-variant";
    case HWA_PHYSICAL_ROLE_SCAN: return "scan";
    default: return NULL;
    }
}

static int hwa_physical_span_equal(const char *start,
                                   size_t length,
                                   const char *text)
{
    return strlen(text) == length && memcmp(start, text, length) == 0;
}

int hwa_physical_role_parse(const char *text, HWAPhysicalRole *role)
{
    const char *first;
    const char *second;
    const char *extra;
    size_t side_length;
    size_t kind_length;

    if (role != NULL) memset(role, 0, sizeof(*role));
    if (text == NULL || role == NULL) return -1;
    first = strchr(text, ':');
    if (first == NULL) return -1;
    second = strchr(first + 1, ':');
    if (second == NULL || second[1] == '\0') return -1;
    extra = strchr(second + 1, ':');
    if (extra != NULL) return -1;
    side_length = (size_t)(first - text);
    kind_length = (size_t)(second - (first + 1));
    if (hwa_physical_span_equal(text, side_length, "reference")) {
        role->side = HWA_PHYSICAL_ROLE_REFERENCE;
    } else if (hwa_physical_span_equal(text, side_length, "model")) {
        role->side = HWA_PHYSICAL_ROLE_MODEL;
    } else {
        return -1;
    }
    if (hwa_physical_span_equal(first + 1, kind_length, "body")) {
        role->kind = HWA_PHYSICAL_ROLE_BODY;
    } else if (hwa_physical_span_equal(first + 1, kind_length, "joint")) {
        role->kind = HWA_PHYSICAL_ROLE_JOINT;
    } else if (hwa_physical_span_equal(first + 1, kind_length,
                                       "isolated-a")) {
        role->kind = HWA_PHYSICAL_ROLE_ISOLATED_A;
    } else if (hwa_physical_span_equal(first + 1, kind_length,
                                       "isolated-b")) {
        role->kind = HWA_PHYSICAL_ROLE_ISOLATED_B;
    } else if (hwa_physical_span_equal(first + 1, kind_length,
                                       "render-baseline")) {
        role->kind = HWA_PHYSICAL_ROLE_RENDER_BASELINE;
    } else if (hwa_physical_span_equal(first + 1, kind_length,
                                       "render-variant")) {
        role->kind = HWA_PHYSICAL_ROLE_RENDER_VARIANT;
    } else if (hwa_physical_span_equal(first + 1, kind_length, "scan")) {
        role->kind = HWA_PHYSICAL_ROLE_SCAN;
    } else {
        memset(role, 0, sizeof(*role));
        return -1;
    }
    role->case_id = second + 1;
    role->case_id_length = strlen(second + 1);
    return 0;
}

static int hwa_physical_file_identity(const char *path,
                                      HWAFileIdentity *identity,
                                      char *error,
                                      size_t error_size)
{
    if (identity != NULL) memset(identity, 0, sizeof(*identity));
    if (path == NULL || path[0] == '\0' || strcmp(path, "-") == 0 ||
        identity == NULL) {
        hwa_set_error(error, error_size, "invalid Stage 5 file path");
        return -1;
    }
#if defined(_WIN32)
    {
        BY_HANDLE_FILE_INFORMATION information;
        DWORD attributes = GetFileAttributesA(path);
        HANDLE handle;
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
            hwa_set_error(error, error_size,
                          "Stage 5 path is not a named regular file: %s",
                          path);
            return -1;
        }
        handle = CreateFileA(
            path, 0U,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (handle == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandle(handle, &information)) {
            DWORD windows_error = GetLastError();
            if (handle != INVALID_HANDLE_VALUE) (void)CloseHandle(handle);
            hwa_set_error(error, error_size,
                          "cannot inspect Stage 5 file identity: Windows error %lu",
                          (unsigned long)windows_error);
            return -1;
        }
        if ((information.dwFileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
            0U) {
            (void)CloseHandle(handle);
            hwa_set_error(error, error_size,
                          "Stage 5 path is not a named regular file: %s",
                          path);
            return -1;
        }
        (void)CloseHandle(handle);
        identity->device = (uint64_t)information.dwVolumeSerialNumber;
        identity->inode =
            ((uint64_t)information.nFileIndexHigh << 32U) |
            (uint64_t)information.nFileIndexLow;
        identity->size =
            ((uint64_t)information.nFileSizeHigh << 32U) |
            (uint64_t)information.nFileSizeLow;
    }
#else
    {
        struct stat facts;
        if (lstat(path, &facts) != 0 || !S_ISREG(facts.st_mode) ||
            facts.st_size < 0) {
            hwa_set_error(error, error_size,
                          "Stage 5 path is not a named regular file: %s",
                          path);
            return -1;
        }
        identity->device = (uint64_t)facts.st_dev;
        identity->inode = (uint64_t)facts.st_ino;
        identity->size = (uint64_t)facts.st_size;
    }
#endif
    identity->valid = 1;
    return 0;
}

static int hwa_physical_same_identity(const HWAFileIdentity *first,
                                      const HWAFileIdentity *second)
{
    return first->valid && second->valid &&
           first->device == second->device && first->inode == second->inode;
}

static int hwa_physical_identity_unchanged(const HWAFileIdentity *before,
                                           const HWAFileIdentity *after)
{
    return hwa_physical_same_identity(before, after) &&
           before->size == after->size;
}

static int hwa_physical_stream_identity(FILE *stream,
                                        HWAFileIdentity *identity,
                                        char *error,
                                        size_t error_size)
{
    if (identity != NULL) memset(identity, 0, sizeof(*identity));
    if (stream == NULL || identity == NULL) {
        hwa_set_error(error, error_size,
                      "invalid Stage 5 open file identity request");
        return -1;
    }
#if defined(_WIN32)
    {
        BY_HANDLE_FILE_INFORMATION information;
        int descriptor = _fileno(stream);
        intptr_t raw_handle = descriptor >= 0
            ? _get_osfhandle(descriptor) : (intptr_t)-1;
        HANDLE handle = (HANDLE)raw_handle;
        if (raw_handle == (intptr_t)-1 ||
            !GetFileInformationByHandle(handle, &information)) {
            hwa_set_error(error, error_size,
                          "cannot inspect opened Stage 5 input: Windows error %lu",
                          (unsigned long)GetLastError());
            return -1;
        }
        if ((information.dwFileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
            0U) {
            hwa_set_error(error, error_size,
                          "opened Stage 5 input is not a regular file");
            return -1;
        }
        identity->device = (uint64_t)information.dwVolumeSerialNumber;
        identity->inode =
            ((uint64_t)information.nFileIndexHigh << 32U) |
            (uint64_t)information.nFileIndexLow;
        identity->size =
            ((uint64_t)information.nFileSizeHigh << 32U) |
            (uint64_t)information.nFileSizeLow;
    }
#else
    {
        struct stat facts;
        int descriptor = fileno(stream);
        if (descriptor < 0 || fstat(descriptor, &facts) != 0 ||
            !S_ISREG(facts.st_mode) || facts.st_size < 0) {
            hwa_set_error(error, error_size,
                          "opened Stage 5 input is not a regular file");
            return -1;
        }
        identity->device = (uint64_t)facts.st_dev;
        identity->inode = (uint64_t)facts.st_ino;
        identity->size = (uint64_t)facts.st_size;
    }
#endif
    identity->valid = 1;
    return 0;
}

static int hwa_physical_stream_seek(FILE *stream,
                                    uint64_t offset,
                                    char *error,
                                    size_t error_size)
{
    if (stream == NULL || offset > (uint64_t)INT64_MAX) {
        hwa_set_error(error, error_size,
                      "Stage 5 input seek offset is too large");
        return -1;
    }
    clearerr(stream);
#if defined(_WIN32)
    if (_fseeki64(stream, (__int64)offset, SEEK_SET) != 0) {
#else
    if (sizeof(off_t) < 8U && offset > (uint64_t)INT32_MAX) {
        hwa_set_error(error, error_size,
                      "Stage 5 input seek offset is too large");
        return -1;
    }
    if (fseeko(stream, (off_t)offset, SEEK_SET) != 0) {
#endif
        hwa_set_error(error, error_size,
                      "cannot seek in opened Stage 5 input");
        return -1;
    }
    return 0;
}

static int hwa_physical_hash_stream(
    FILE *stream,
    const HWAFileIdentity *expected_identity,
    uint64_t max_bytes,
    uint64_t return_offset,
    unsigned char *buffer,
    size_t buffer_size,
    char hex[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    unsigned char digest[32];
    HWASha256 context;
    HWAFileIdentity before_identity;
    HWAFileIdentity after_identity;
    uint64_t total = 0U;
    int status = -1;

    if (hex != NULL) memset(hex, 0, HWA_SHA256_HEX_SIZE);
    if (stream == NULL || expected_identity == NULL ||
        !expected_identity->valid || buffer == NULL || buffer_size == 0U ||
        hex == NULL || max_bytes == 0U ||
        expected_identity->size > max_bytes ||
        expected_identity->size > UINT64_MAX / UINT64_C(8)) {
        hwa_set_error(error, error_size,
                      "invalid opened Stage 5 input hash request");
        return -1;
    }
    if (hwa_physical_stream_identity(
            stream, &before_identity, error, error_size) != 0 ||
        !hwa_physical_identity_unchanged(
            expected_identity, &before_identity) ||
        hwa_physical_stream_seek(stream, 0U, error, error_size) != 0) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "opened Stage 5 input changed before hashing");
        }
        return -1;
    }
    hwa_sha256_init(&context);
    for (;;) {
        size_t count = fread(buffer, 1U, buffer_size, stream);
        if (count != 0U) {
            if (total > max_bytes || (uint64_t)count > max_bytes - total) {
                hwa_set_error(error, error_size,
                              "opened Stage 5 input exceeds its byte limit");
                goto cleanup;
            }
            total += (uint64_t)count;
            hwa_sha256_update(&context, buffer, count);
        }
        if (count < buffer_size) {
            if (ferror(stream)) {
                hwa_set_error(error, error_size,
                              "cannot read opened Stage 5 input for hashing");
                goto cleanup;
            }
            break;
        }
    }
    if (total != expected_identity->size ||
        hwa_physical_stream_identity(
            stream, &after_identity, error, error_size) != 0 ||
        !hwa_physical_identity_unchanged(
            &before_identity, &after_identity)) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "opened Stage 5 input changed while hashing");
        }
        goto cleanup;
    }
    hwa_sha256_final(&context, digest);
    hwa_sha256_hex(digest, hex);
    if (hwa_physical_stream_seek(
            stream, return_offset, error, error_size) != 0) goto cleanup;
    status = 0;

cleanup:
    if (status != 0) memset(hex, 0, HWA_SHA256_HEX_SIZE);
    return status;
}

static int hwa_physical_hash_named_identity(
    const char *path,
    const HWAFileIdentity *expected_identity,
    uint64_t max_bytes,
    uint64_t live_work_bytes,
    uint64_t max_work_bytes,
    char hex[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    FILE *stream = NULL;
    unsigned char *buffer = NULL;
    uint64_t buffer_bytes;
    int status = -1;

    if (hex != NULL) memset(hex, 0, HWA_SHA256_HEX_SIZE);
    if (path == NULL || path[0] == '\0' || expected_identity == NULL ||
        !expected_identity->valid || hex == NULL) {
        hwa_set_error(error, error_size,
                      "invalid Stage 5 identity-bound hash request");
        return -1;
    }
    buffer_bytes = expected_identity->size < HWA_PHYSICAL_HASH_BLOCK
        ? expected_identity->size : HWA_PHYSICAL_HASH_BLOCK;
    if (buffer_bytes == 0U) buffer_bytes = 1U;
    if (live_work_bytes > max_work_bytes ||
        buffer_bytes > max_work_bytes - live_work_bytes) {
        hwa_set_error(error, error_size,
                      "Stage 5 identity hash work exceeds its byte limit");
        return -1;
    }
    buffer = (unsigned char *)malloc((size_t)buffer_bytes);
    if (buffer == NULL) {
        hwa_set_error(error, error_size,
                      "cannot allocate Stage 5 identity hash work");
        return -1;
    }
    stream = fopen(path, "rb");
    if (stream == NULL) {
        hwa_set_error(error, error_size,
                      "cannot open Stage 5 input '%s' for hashing: %s",
                      path, strerror(errno));
        goto cleanup;
    }
    if (hwa_physical_hash_stream(
            stream, expected_identity, max_bytes, 0U,
            buffer, (size_t)buffer_bytes, hex,
            error, error_size) != 0) goto cleanup;
    status = 0;

cleanup:
    if (stream != NULL && fclose(stream) != 0 && status == 0) {
        hwa_set_error(error, error_size,
                      "cannot close Stage 5 identity hash input '%s'", path);
        status = -1;
    }
    free(buffer);
    if (status != 0) memset(hex, 0, HWA_SHA256_HEX_SIZE);
    return status;
}

static int hwa_physical_verify_identity_digest(
    const char *path,
    const HWAFileIdentity *expected_identity,
    const char expected_hash[HWA_SHA256_HEX_SIZE],
    uint64_t max_bytes,
    uint64_t live_work_bytes,
    uint64_t max_work_bytes,
    char *error,
    size_t error_size)
{
    HWAFileIdentity path_identity;
    char actual_hash[HWA_SHA256_HEX_SIZE];
    if (hwa_physical_file_identity(
            path, &path_identity, error, error_size) != 0 ||
        !hwa_physical_identity_unchanged(
            expected_identity, &path_identity) ||
        hwa_physical_hash_named_identity(
            path, expected_identity, max_bytes,
            live_work_bytes, max_work_bytes, actual_hash,
            error, error_size) != 0 ||
        strcmp(expected_hash, actual_hash) != 0 ||
        hwa_physical_file_identity(
            path, &path_identity, error, error_size) != 0 ||
        !hwa_physical_identity_unchanged(
            expected_identity, &path_identity)) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "Stage 5 input identity and digest do not match: %s",
                          path);
        }
        return -1;
    }
    return 0;
}

void hwa_physical_options_default(HWAPhysicalOptions *options)
{
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    options->decode_block_frames = 4096U;
    options->fft_size = 8192U;
    options->hop_size = 256U;
    options->spectral_floor_dbfs = -100.0;
    options->max_wave_bytes = UINT64_C(17179869184);
    options->max_wave_frames = UINT64_C(2000000000);
    options->max_work_bytes = UINT64_C(536870912);
    options->max_pair_evaluations = UINT64_C(100000000);
    options->max_bindings = 256U;
    options->max_transforms = 1000000U;
    options->max_modes = 512U;
    options->max_checks = 1000000U;
    options->max_findings = 100000U;
    options->max_warnings = 100000U;
    hwa_profile_comparison_options_default(&options->profile_limits);
}

void hwa_physical_check_set_free(HWAPhysicalCheckSet *result)
{
    size_t index;

    if (result == NULL) return;
    free(result->reference_measures_path);
    free(result->model_measures_path);
    if (result->sources != NULL) {
        for (index = 0U; index < result->source_count; ++index) {
            free(result->sources[index].role);
            free(result->sources[index].path);
        }
    }
    if (result->checks != NULL) {
        for (index = 0U; index < result->check_count; ++index) {
            free(result->checks[index].scope);
            free(result->checks[index].case_id);
            free(result->checks[index].element);
        }
    }
    if (result->findings != NULL) {
        for (index = 0U; index < result->finding_count; ++index) {
            free(result->findings[index].code);
            free(result->findings[index].message);
        }
    }
    if (result->warnings != NULL) {
        for (index = 0U; index < result->warning_count; ++index) {
            free(result->warnings[index].code);
            free(result->warnings[index].message);
        }
    }
    free(result->sources);
    free(result->checks);
    free(result->findings);
    free(result->warnings);
    memset(result, 0, sizeof(*result));
}

static int hwa_physical_power_of_two(size_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static int hwa_physical_options_valid(const HWAPhysicalOptions *options,
                                      char *error,
                                      size_t error_size)
{
    if (options->decode_block_frames == 0U ||
        !hwa_physical_power_of_two(options->fft_size) ||
        options->fft_size < 256U || options->fft_size > 1048576U ||
        options->hop_size == 0U || options->hop_size > options->fft_size ||
        !isfinite(options->spectral_floor_dbfs) ||
        options->spectral_floor_dbfs < -300.0 ||
        options->spectral_floor_dbfs > 0.0 ||
        options->max_wave_bytes == 0U || options->max_wave_frames == 0U ||
        options->max_work_bytes == 0U ||
        options->max_pair_evaluations == 0U ||
        options->max_bindings == 0U || options->max_transforms == 0U ||
        options->max_modes == 0U || options->max_checks == 0U ||
        options->max_findings == 0U || options->max_warnings == 0U) {
        hwa_set_error(error, error_size, "invalid Stage 5 options or limits");
        return 0;
    }
    return 1;
}

static int hwa_physical_add_work(HWAPhysicalCheckSet *result,
                                 uint64_t bytes,
                                 char *error,
                                 size_t error_size)
{
    if (bytes > result->options.max_work_bytes -
                    (result->retained_work_bytes >
                             result->options.max_work_bytes
                         ? result->options.max_work_bytes
                         : result->retained_work_bytes)) {
        hwa_set_error(error, error_size,
                      "Stage 5 retained work exceeds its byte limit");
        return -1;
    }
    result->retained_work_bytes += bytes;
    return 0;
}

static char *hwa_physical_copy_text(HWAPhysicalCheckSet *result,
                                    const char *text,
                                    size_t length,
                                    char *error,
                                    size_t error_size)
{
    char *copy;
    uint64_t bytes;

    if (text == NULL || length == SIZE_MAX) {
        hwa_set_error(error, error_size, "invalid Stage 5 text");
        return NULL;
    }
    bytes = (uint64_t)length + 1U;
    if (hwa_physical_add_work(result, bytes, error, error_size) != 0) {
        return NULL;
    }
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        result->retained_work_bytes -= bytes;
        hwa_set_error(error, error_size, "cannot allocate Stage 5 text");
        return NULL;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static char *hwa_physical_copy_cstring(HWAPhysicalCheckSet *result,
                                       const char *text,
                                       char *error,
                                       size_t error_size)
{
    if (text == NULL) {
        hwa_set_error(error, error_size, "missing Stage 5 text");
        return NULL;
    }
    return hwa_physical_copy_text(result, text, strlen(text),
                                  error, error_size);
}

static int hwa_physical_allocate_sources(HWAPhysicalCheckSet *result,
                                         size_t count,
                                         char *error,
                                         size_t error_size)
{
    uint64_t bytes;
    if (count == 0U || count > SIZE_MAX / sizeof(*result->sources)) {
        hwa_set_error(error, error_size, "invalid Stage 5 source count");
        return -1;
    }
    bytes = (uint64_t)count * sizeof(*result->sources);
    if (hwa_physical_add_work(result, bytes, error, error_size) != 0) {
        return -1;
    }
    result->sources = (HWAPhysicalSource *)calloc(
        count, sizeof(*result->sources));
    if (result->sources == NULL) {
        result->retained_work_bytes -= bytes;
        hwa_set_error(error, error_size, "cannot allocate Stage 5 sources");
        return -1;
    }
    result->source_count = count;
    return 0;
}

static int hwa_physical_add_check(HWAPhysicalCheckSet *result,
                                  const char *scope,
                                  const char *case_id,
                                  const char *element,
                                  HWAPhysicalCheckKind kind,
                                  HWAPhysicalUnit unit,
                                  HWAPhysicalAvailability availability,
                                  uint32_t evidence_flags,
                                  uint32_t quality_flags,
                                  char *error,
                                  size_t error_size)
{
    HWAPhysicalCheck *grown;
    HWAPhysicalCheck *check;
    uint64_t bytes = sizeof(*result->checks);

    if (result->check_count >= result->options.max_checks ||
        result->check_count == SIZE_MAX / sizeof(*result->checks) ||
        hwa_physical_add_work(result, bytes, error, error_size) != 0) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "Stage 5 check count exceeds its limit");
        }
        return -1;
    }
    grown = (HWAPhysicalCheck *)realloc(
        result->checks,
        (result->check_count + 1U) * sizeof(*result->checks));
    if (grown == NULL) {
        result->retained_work_bytes -= bytes;
        hwa_set_error(error, error_size, "cannot allocate Stage 5 checks");
        return -1;
    }
    result->checks = grown;
    check = &result->checks[result->check_count];
    memset(check, 0, sizeof(*check));
    check->id = (uint64_t)result->check_count + 1U;
    check->kind = kind;
    check->unit = unit;
    check->availability = availability;
    check->confidence = 0.0;
    check->evidence_flags = evidence_flags;
    check->quality_flags = quality_flags;
    check->scope = hwa_physical_copy_cstring(
        result, scope, error, error_size);
    check->case_id = hwa_physical_copy_cstring(
        result, case_id, error, error_size);
    check->element = hwa_physical_copy_cstring(
        result, element, error, error_size);
    if (check->scope == NULL || check->case_id == NULL ||
        check->element == NULL) {
        free(check->scope);
        free(check->case_id);
        free(check->element);
        memset(check, 0, sizeof(*check));
        result->retained_work_bytes -= bytes;
        return -1;
    }
    result->check_count++;
    return 0;
}

static int hwa_physical_set_profile_source(
    HWAPhysicalCheckSet *result,
    size_t index,
    const char *role,
    const char *path,
    const char sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    HWAPhysicalSource *source = &result->sources[index];
    source->id = (uint64_t)index + 1U;
    source->role = hwa_physical_copy_cstring(
        result, role, error, error_size);
    source->path = hwa_physical_copy_cstring(
        result, path, error, error_size);
    if (source->role == NULL || source->path == NULL) return -1;
    memcpy(source->sha256, sha256, HWA_SHA256_HEX_SIZE);
    source->is_wave = 0;
    return 0;
}

static void hwa_physical_probe_free(HWAProbe *probe)
{
    if (probe == NULL) return;
    free(probe->samples);
    free(probe->spectrum);
    free(probe->modes);
    memset(probe, 0, sizeof(*probe));
}

static int hwa_physical_live_work_fits(const HWAPhysicalCheckSet *result,
                                       uint64_t extra)
{
    return result->retained_work_bytes <= result->options.max_work_bytes &&
           extra <= result->options.max_work_bytes -
                        result->retained_work_bytes;
}

static double hwa_physical_db(double value, double floor_db)
{
    if (!isfinite(value)) return value;
    if (value <= 0.0) return floor_db;
    value = 20.0 * log10(value);
    return value < floor_db ? floor_db : value;
}

static int hwa_physical_wide_fits_double(long double value)
{
    return isfinite(value) &&
           fabsl(value) <= (long double)DBL_MAX;
}

static int hwa_physical_accumulate_value(long double value,
                                         long double *total)
{
    long double next;
    if (total == NULL || !hwa_physical_wide_fits_double(value) ||
        !hwa_physical_wide_fits_double(*total)) return 0;
    next = *total + value;
    if (!hwa_physical_wide_fits_double(next)) return 0;
    *total = next;
    return 1;
}

static int hwa_physical_accumulate_product(long double first,
                                           long double second,
                                           long double *total)
{
    long double product;
    if (total == NULL || !hwa_physical_wide_fits_double(first) ||
        !hwa_physical_wide_fits_double(second) ||
        !hwa_physical_wide_fits_double(*total)) return 0;
    product = first * second;
    return hwa_physical_accumulate_value(product, total);
}

static int hwa_physical_accumulate_square(long double value,
                                          long double *total)
{
    return hwa_physical_accumulate_product(value, value, total);
}

static int hwa_physical_wide_ratio(long double numerator,
                                   long double denominator,
                                   double *ratio)
{
    long double wide;
    if (ratio == NULL || !hwa_physical_wide_fits_double(numerator) ||
        !hwa_physical_wide_fits_double(denominator) ||
        denominator <= 0.0L) return 0;
    wide = numerator / denominator;
    if (!hwa_physical_wide_fits_double(wide)) return 0;
    *ratio = (double)wide;
    return isfinite(*ratio);
}

static int hwa_physical_mode_frequency_order(const void *left,
                                             const void *right)
{
    const HWAPhysicalMode *first = (const HWAPhysicalMode *)left;
    const HWAPhysicalMode *second = (const HWAPhysicalMode *)right;
    if (first->frequency_hz < second->frequency_hz) return -1;
    if (first->frequency_hz > second->frequency_hz) return 1;
    return 0;
}

static double hwa_physical_local_baseline(const HWAProbe *probe, size_t bin)
{
    size_t begin = bin > 16U ? bin - 16U : 1U;
    size_t end = bin + 16U < probe->spectrum_bins
                     ? bin + 16U : probe->spectrum_bins - 1U;
    size_t index;
    size_t count = 0U;
    long double sum = 0.0L;
    for (index = begin; index <= end; ++index) {
        size_t distance = index > bin ? index - bin : bin - index;
        if (distance <= 2U) continue;
        sum += (long double)probe->spectrum[index];
        count++;
    }
    return count > 0U ? (double)(sum / (long double)count) : 0.0;
}

static int hwa_physical_mode_candidate(const HWAProbe *probe,
                                       size_t bin,
                                       double *prominence_db)
{
    double power;
    double baseline;
    double frequency;
    if (bin == 0U || bin + 1U >= probe->spectrum_bins) return 0;
    power = probe->spectrum[bin];
    if (!(power > probe->spectrum[bin - 1U] &&
          power >= probe->spectrum[bin + 1U])) return 0;
    frequency = (double)bin * (double)probe->format.sample_rate_hz /
                (double)((probe->spectrum_bins - 1U) * 2U);
    if (frequency < 40.0 ||
        frequency > 0.49 * (double)probe->format.sample_rate_hz) return 0;
    baseline = hwa_physical_local_baseline(probe, bin);
    if (!(power > 0.0) || !(baseline > 0.0)) return 0;
    *prominence_db = 10.0 * log10(power / baseline);
    return isfinite(*prominence_db) && *prominence_db >= 3.0;
}

static int hwa_physical_extract_modes(HWAPhysicalCheckSet *result,
                                      HWAProbe *probe,
                                      uint64_t prior_probe_bytes,
                                      char *error,
                                      size_t error_size)
{
    size_t bin;
    size_t candidate_count = 0U;
    size_t capacity;
    uint64_t bytes;
    double bin_hz;

    if (probe->spectrum_bins < 3U) return 0;
    for (bin = 1U; bin + 1U < probe->spectrum_bins; ++bin) {
        double prominence;
        if (hwa_physical_mode_candidate(probe, bin, &prominence)) {
            candidate_count++;
        }
    }
    if (candidate_count == 0U) return 0;
    capacity = candidate_count < result->options.max_modes
                   ? candidate_count : result->options.max_modes;
    if (capacity > SIZE_MAX / sizeof(*probe->modes)) {
        hwa_set_error(error, error_size, "Stage 5 mode count overflows");
        return -1;
    }
    bytes = (uint64_t)capacity * sizeof(*probe->modes);
    if (prior_probe_bytes > UINT64_MAX - probe->work_bytes ||
        prior_probe_bytes + probe->work_bytes > UINT64_MAX - bytes ||
        !hwa_physical_live_work_fits(
            result, prior_probe_bytes + probe->work_bytes + bytes)) {
        hwa_set_error(error, error_size,
                      "Stage 5 mode work exceeds its byte limit");
        return -1;
    }
    probe->modes = (HWAPhysicalMode *)calloc(capacity,
                                             sizeof(*probe->modes));
    if (probe->modes == NULL) {
        hwa_set_error(error, error_size, "cannot allocate Stage 5 modes");
        return -1;
    }
    probe->work_bytes += bytes;
    bin_hz = (double)probe->format.sample_rate_hz /
             (double)((probe->spectrum_bins - 1U) * 2U);
    for (bin = 1U; bin + 1U < probe->spectrum_bins; ++bin) {
        double prominence;
        size_t slot;
        if (!hwa_physical_mode_candidate(probe, bin, &prominence)) continue;
        if (probe->mode_count < capacity) {
            slot = probe->mode_count++;
        } else {
            size_t weakest = 0U;
            size_t index;
            for (index = 1U; index < capacity; ++index) {
                if (probe->modes[index].power < probe->modes[weakest].power) {
                    weakest = index;
                }
            }
            if (probe->spectrum[bin] <= probe->modes[weakest].power) continue;
            slot = weakest;
        }
        {
            size_t left = bin;
            size_t right = bin;
            double half_power = probe->spectrum[bin] * 0.5;
            double bandwidth;
            while (left > 1U && probe->spectrum[left] > half_power) left--;
            while (right + 1U < probe->spectrum_bins &&
                   probe->spectrum[right] > half_power) right++;
            bandwidth = (double)(right - left) * bin_hz;
            if (bandwidth < bin_hz) bandwidth = bin_hz;
            probe->modes[slot].frequency_hz = (double)bin * bin_hz;
            probe->modes[slot].bandwidth_hz = bandwidth;
            probe->modes[slot].q = probe->modes[slot].frequency_hz /
                                   bandwidth;
            probe->modes[slot].prominence_db = prominence;
            probe->modes[slot].decay_seconds =
                log(1000.0) / (HWA_PHYSICAL_PI * bandwidth);
            probe->modes[slot].power = probe->spectrum[bin];
        }
    }
    qsort(probe->modes, probe->mode_count, sizeof(*probe->modes),
          hwa_physical_mode_frequency_order);
    return 0;
}

static int hwa_physical_probe_spectrum(HWAPhysicalCheckSet *result,
                                       HWAProbe *probe,
                                       uint64_t prior_probe_bytes,
                                       char *error,
                                       size_t error_size)
{
    size_t fft_size = result->options.fft_size;
    size_t bins = fft_size / 2U + 1U;
    size_t transforms;
    size_t transform;
    size_t bin;
    size_t start = 0U;
    size_t high_start;
    HwaDspComplex *fft = NULL;
    double *window = NULL;
    uint64_t live_bytes;
    long double total_power = 0.0L;
    long double high_power = 0.0L;
    double peak_power = 0.0;
    double baseline = 0.0;

    if (probe->frame_count == 0U ||
        probe->frame_count > (uint64_t)SIZE_MAX) {
        return 0;
    }
    if (!(probe->rms > 0.0) ||
        20.0 * log10(probe->rms) < result->options.spectral_floor_dbfs) {
        return 0;
    }
    if (probe->frame_count <= (uint64_t)fft_size) {
        transforms = 1U;
    } else {
        uint64_t remaining = probe->frame_count - (uint64_t)fft_size;
        uint64_t count = remaining / (uint64_t)result->options.hop_size + 1U;
        if (count > (uint64_t)SIZE_MAX) {
            hwa_set_error(error, error_size,
                          "Stage 5 transform count overflows");
            return -1;
        }
        transforms = (size_t)count;
    }
    if (transforms > result->options.max_transforms -
                         (result->transform_count >
                                  result->options.max_transforms
                              ? result->options.max_transforms
                              : result->transform_count)) {
        hwa_set_error(error, error_size,
                      "Stage 5 transform count exceeds its limit");
        return -1;
    }
    if (fft_size > SIZE_MAX / sizeof(*fft) ||
        fft_size > SIZE_MAX / sizeof(*window) ||
        bins > SIZE_MAX / sizeof(*probe->spectrum)) {
        hwa_set_error(error, error_size, "Stage 5 spectrum size overflows");
        return -1;
    }
    live_bytes = (uint64_t)fft_size * sizeof(*fft) +
                 (uint64_t)fft_size * sizeof(*window) +
                 (uint64_t)bins * sizeof(*probe->spectrum);
    if (prior_probe_bytes > UINT64_MAX - probe->work_bytes ||
        prior_probe_bytes + probe->work_bytes > UINT64_MAX - live_bytes ||
        !hwa_physical_live_work_fits(
            result, prior_probe_bytes + probe->work_bytes + live_bytes)) {
        hwa_set_error(error, error_size,
                      "Stage 5 spectrum work exceeds its byte limit");
        return -1;
    }
    fft = (HwaDspComplex *)calloc(fft_size, sizeof(*fft));
    window = (double *)malloc(fft_size * sizeof(*window));
    probe->spectrum = (double *)calloc(bins, sizeof(*probe->spectrum));
    if (fft == NULL || window == NULL || probe->spectrum == NULL) {
        free(fft);
        free(window);
        free(probe->spectrum);
        probe->spectrum = NULL;
        hwa_set_error(error, error_size,
                      "cannot allocate Stage 5 spectrum work");
        return -1;
    }
    if (hwa_dsp_hann(window, fft_size) != HWA_DSP_OK) {
        hwa_set_error(error, error_size,
                      "cannot build Stage 5 spectrum window");
        free(fft);
        free(window);
        free(probe->spectrum);
        probe->spectrum = NULL;
        return -1;
    }
    for (transform = 0U; transform < transforms; ++transform) {
        size_t index;
        for (index = 0U; index < fft_size; ++index) {
            uint64_t source = (uint64_t)start + (uint64_t)index;
            fft[index].real = source < probe->frame_count
                                  ? probe->samples[(size_t)source] * window[index]
                                  : 0.0;
            fft[index].imag = 0.0;
        }
        if (hwa_dsp_fft(fft, fft_size, 0) != HWA_DSP_OK) {
            hwa_set_error(error, error_size,
                          "Stage 5 spectrum transform failed");
            free(fft);
            free(window);
            free(probe->spectrum);
            probe->spectrum = NULL;
            return -1;
        }
        for (bin = 0U; bin < bins; ++bin) {
            long double real = (long double)fft[bin].real;
            long double imag = (long double)fft[bin].imag;
            long double power = real * real + imag * imag;
            if (!isfinite(fft[bin].real) || !isfinite(fft[bin].imag) ||
                !isfinite(power) || power > (long double)DBL_MAX ||
                probe->spectrum[bin] > DBL_MAX - (double)power) {
                hwa_set_error(error, error_size,
                              "Stage 5 spectrum calculation overflowed");
                free(fft);
                free(window);
                free(probe->spectrum);
                probe->spectrum = NULL;
                return -1;
            }
            probe->spectrum[bin] += (double)power;
        }
        start += result->options.hop_size;
    }
    free(fft);
    free(window);
    for (bin = 0U; bin < bins; ++bin) {
        probe->spectrum[bin] /= (double)transforms;
        if (!isfinite(probe->spectrum[bin])) {
            hwa_set_error(error, error_size,
                          "Stage 5 spectrum result is non-finite");
            free(probe->spectrum);
            probe->spectrum = NULL;
            return -1;
        }
    }
    result->transform_count += transforms;
    probe->spectrum_bins = bins;
    probe->work_bytes += (uint64_t)bins * sizeof(*probe->spectrum);
    high_start = (bins * 4U + 4U) / 5U;
    if (high_start >= bins) high_start = bins - 1U;
    for (bin = 1U; bin < bins; ++bin) {
        double power = probe->spectrum[bin];
        total_power += (long double)power;
        if (bin >= high_start) high_power += (long double)power;
        if (power > peak_power) {
            peak_power = power;
            probe->strongest_bin = bin;
        }
    }
    if (!isfinite(total_power) || !isfinite(high_power) ||
        total_power > (long double)DBL_MAX ||
        high_power > (long double)DBL_MAX) {
        hwa_set_error(error, error_size,
                      "Stage 5 spectrum power sum overflowed");
        return -1;
    }
    if (total_power <= 0.0L || peak_power <= 0.0) return 0;
    probe->strongest_frequency_hz = (double)probe->strongest_bin *
        (double)probe->format.sample_rate_hz / (double)fft_size;
    probe->high_band_ratio = (double)(high_power / total_power);
    if (bins > 2U) {
        baseline = (double)((total_power - (long double)peak_power) /
                            (long double)(bins - 2U));
    }
    probe->fixed_tone_prominence_db = baseline > 0.0
        ? 10.0 * log10(peak_power / baseline)
        : -result->options.spectral_floor_dbfs;
    if (probe->strongest_bin > 2U) {
        double lower_peak = 0.0;
        size_t upper = probe->strongest_bin / 2U;
        for (bin = 1U; bin <= upper; ++bin) {
            if (probe->spectrum[bin] > lower_peak) {
                lower_peak = probe->spectrum[bin];
            }
        }
        probe->subharmonic_ratio = lower_peak / peak_power;
    }
    if (!isfinite(probe->strongest_frequency_hz) ||
        !isfinite(probe->high_band_ratio) ||
        !isfinite(probe->fixed_tone_prominence_db) ||
        !isfinite(probe->subharmonic_ratio)) {
        hwa_set_error(error, error_size,
                      "Stage 5 spectral fact is non-finite");
        return -1;
    }
    probe->spectrum_valid = 1;
    return hwa_physical_extract_modes(result, probe, prior_probe_bytes,
                                      error, error_size);
}

static int hwa_physical_probe_time_facts(
    const HWAPhysicalCheckSet *result,
    HWAProbe *probe,
    uint64_t clipped,
    uint64_t hard_bound,
    uint64_t denormal,
    uint64_t sample_values,
    char *error,
    size_t error_size)
{
    uint64_t frame;
    uint64_t edge = probe->frame_count / 10U;
    uint64_t stuck = 0U;
    uint64_t stuck_pairs = 0U;
    uint64_t repeated = 0U;
    uint64_t repeated_pairs = 0U;
    long double sum = 0.0L;
    long double whole_power = 0.0L;
    long double early = 0.0L;
    long double late = 0.0L;
    long double return_power = 0.0L;
    double max_step = 0.0;
    size_t level_block = probe->format.sample_rate_hz / 10U;
    long double sx = 0.0L;
    long double sy = 0.0L;
    long double sxx = 0.0L;
    long double sxy = 0.0L;
    uint64_t slope_count = 0U;

    if (edge == 0U) edge = 1U;
    for (frame = 0U; frame < probe->frame_count; ++frame) {
        double value = probe->samples[(size_t)frame];
        long double wide = (long double)value;
        long double square = wide * wide;
        if (!isfinite(square)) {
            hwa_set_error(error, error_size,
                          "Stage 5 WAVE level calculation overflowed");
            return -1;
        }
        sum += (long double)value;
        whole_power += square;
        if (frame < edge) early += (long double)value;
        if (frame >= probe->frame_count - edge) {
            late += (long double)value;
            return_power += square;
        }
        if (frame > 0U) {
            double difference = fabs(value - probe->samples[(size_t)frame - 1U]);
            if (difference > max_step) max_step = difference;
            if (value != 0.0 || probe->samples[(size_t)frame - 1U] != 0.0) {
                stuck_pairs++;
                if (value == probe->samples[(size_t)frame - 1U]) stuck++;
            }
        }
    }
    if (!isfinite(sum) || !isfinite(whole_power) || !isfinite(early) ||
        !isfinite(late) || !isfinite(return_power)) {
        hwa_set_error(error, error_size,
                      "Stage 5 WAVE summary calculation overflowed");
        return -1;
    }
    probe->rms = sqrt(
        (double)(whole_power / (long double)probe->frame_count));
    probe->dc_offset = (double)(sum / (long double)probe->frame_count);
    probe->dc_drift = (double)(late / (long double)edge -
                               early / (long double)edge);
    probe->clip_fraction = sample_values > 0U
        ? (double)clipped / (double)sample_values : 0.0;
    probe->hard_bound_fraction = sample_values > 0U
        ? (double)hard_bound / (double)sample_values : 0.0;
    probe->denormal_fraction = sample_values > 0U
        ? (double)denormal / (double)sample_values : 0.0;
    probe->stuck_state_fraction = stuck_pairs > 0U
        ? (double)stuck / (double)stuck_pairs : 0.0;
    probe->max_step_dbfs = hwa_physical_db(
        max_step, result->options.spectral_floor_dbfs);
    probe->return_level_dbfs = hwa_physical_db(
        sqrt((double)(return_power / (long double)edge)),
        result->options.spectral_floor_dbfs);

    if (probe->frame_count >= HWA_PHYSICAL_REPEAT_BLOCK * 2U) {
        uint64_t block;
        uint64_t blocks = probe->frame_count / HWA_PHYSICAL_REPEAT_BLOCK;
        int previous_active = 0;
        size_t sample;
        for (sample = 0U; sample < HWA_PHYSICAL_REPEAT_BLOCK; ++sample) {
            if (probe->samples[sample] != 0.0) {
                previous_active = 1;
                break;
            }
        }
        for (block = 1U; block < blocks; ++block) {
            size_t current = (size_t)(block * HWA_PHYSICAL_REPEAT_BLOCK);
            size_t previous = current - HWA_PHYSICAL_REPEAT_BLOCK;
            int current_active = 0;
            for (sample = 0U; sample < HWA_PHYSICAL_REPEAT_BLOCK; ++sample) {
                if (probe->samples[current + sample] != 0.0) {
                    current_active = 1;
                    break;
                }
            }
            if (previous_active || current_active) {
                repeated_pairs++;
                if (memcmp(probe->samples + current,
                           probe->samples + previous,
                           HWA_PHYSICAL_REPEAT_BLOCK * sizeof(double)) == 0) {
                    repeated++;
                }
            }
            previous_active = current_active;
        }
        probe->repeated_block_fraction = repeated_pairs > 0U
            ? (double)repeated / (double)repeated_pairs : 0.0;
    }

    if (level_block == 0U) level_block = 1U;
    for (frame = 0U; frame + (uint64_t)level_block <= probe->frame_count;
         frame += (uint64_t)level_block) {
        size_t index;
        long double power = 0.0L;
        double level;
        double time;
        for (index = 0U; index < level_block; ++index) {
            double value = probe->samples[(size_t)frame + index];
            long double wide = (long double)value;
            power += wide * wide;
        }
        if (!isfinite(power)) {
            hwa_set_error(error, error_size,
                          "Stage 5 level-slope calculation overflowed");
            return -1;
        }
        level = hwa_physical_db(
            sqrt((double)(power / (long double)level_block)),
            result->options.spectral_floor_dbfs);
        time = ((double)frame + 0.5 * (double)level_block) /
               (double)probe->format.sample_rate_hz;
        sx += (long double)time;
        sy += (long double)level;
        sxx += (long double)time * (long double)time;
        sxy += (long double)time * (long double)level;
        slope_count++;
    }
    if (slope_count >= 2U) {
        long double denominator = (long double)slope_count * sxx - sx * sx;
        if (denominator > 0.0L) {
            probe->runaway_slope_db_per_second =
                (double)(((long double)slope_count * sxy - sx * sy) /
                         denominator);
        }
    }
    if (!isfinite(probe->rms) || !isfinite(probe->dc_offset) ||
        !isfinite(probe->dc_drift) || !isfinite(probe->clip_fraction) ||
        !isfinite(probe->hard_bound_fraction) ||
        !isfinite(probe->denormal_fraction) ||
        !isfinite(probe->stuck_state_fraction) ||
        !isfinite(probe->repeated_block_fraction) ||
        !isfinite(probe->max_step_dbfs) ||
        !isfinite(probe->return_level_dbfs) ||
        !isfinite(probe->runaway_slope_db_per_second)) {
        hwa_set_error(error, error_size,
                      "Stage 5 WAVE summary is non-finite");
        return -1;
    }
    return 0;
}

static int hwa_physical_probe_read(HWAPhysicalCheckSet *result,
                                   const HWAPhysicalInput *input,
                                   HWAProbe *probe,
                                   HWAPhysicalSource *source,
                                   uint64_t prior_probe_bytes,
                                   char *error,
                                   size_t error_size)
{
    HWAWavReader reader;
    unsigned char *buffer = NULL;
    char before[HWA_SHA256_HEX_SIZE];
    char after[HWA_SHA256_HEX_SIZE];
    HWAFileIdentity opened_identity;
    HWAFileIdentity after_identity;
    uint64_t frame_offset = 0U;
    uint64_t clipped = 0U;
    uint64_t hard_bound = 0U;
    uint64_t denormal = 0U;
    uint64_t sample_values = 0U;
    uint64_t sample_bytes;
    uint64_t buffer_bytes;
    uint64_t hash_buffer_bytes;
    int status = -1;

    memset(probe, 0, sizeof(*probe));
    memset(&reader, 0, sizeof(reader));
    if (input->role == NULL || input->path == NULL ||
        input->path[0] == '\0' || strcmp(input->path, "-") == 0 ||
        hwa_physical_role_parse(input->role, &probe->role) != 0) {
        hwa_set_error(error, error_size,
                      "invalid Stage 5 binding role or path");
        return -1;
    }
    if (hwa_physical_file_identity(
            input->path, &probe->identity, error, error_size) != 0 ||
        hwa_wav_reader_open(&reader, input->path,
                            result->options.max_wave_bytes,
                            error, error_size) != 0) {
        goto cleanup;
    }
    if (hwa_physical_stream_identity(
            reader.file, &opened_identity, error, error_size) != 0 ||
        !hwa_physical_identity_unchanged(
            &probe->identity, &opened_identity)) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "opened Stage 5 WAVE does not match its checked path");
        }
        goto cleanup;
    }
    if (reader.format.frames == 0U ||
        reader.format.frames > result->options.max_wave_frames ||
        reader.format.frames > (uint64_t)SIZE_MAX / sizeof(double) ||
        result->options.decode_block_frames >
            SIZE_MAX / reader.format.block_align) {
        hwa_set_error(error, error_size,
                      "Stage 5 WAVE shape exceeds its limit");
        goto cleanup;
    }
    sample_bytes = reader.format.frames * sizeof(double);
    buffer_bytes = (uint64_t)result->options.decode_block_frames *
                   reader.format.block_align;
    if (buffer_bytes > (uint64_t)SIZE_MAX ||
        sample_bytes > UINT64_MAX - buffer_bytes ||
        prior_probe_bytes > UINT64_MAX - sample_bytes ||
        prior_probe_bytes + sample_bytes > UINT64_MAX - buffer_bytes ||
        !hwa_physical_live_work_fits(
            result, prior_probe_bytes + sample_bytes + buffer_bytes)) {
        hwa_set_error(error, error_size,
                      "Stage 5 WAVE work exceeds its byte limit");
        goto cleanup;
    }
    probe->samples = (double *)malloc((size_t)sample_bytes);
    buffer = (unsigned char *)malloc((size_t)buffer_bytes);
    if (probe->samples == NULL || buffer == NULL) {
        hwa_set_error(error, error_size,
                      "cannot allocate Stage 5 WAVE work");
        goto cleanup;
    }
    probe->format = reader.format;
    probe->frame_count = reader.format.frames;
    probe->work_bytes = sample_bytes;
    if (hwa_physical_hash_stream(
            reader.file, &opened_identity, result->options.max_wave_bytes,
            reader.data_offset, buffer,
            (size_t)(buffer_bytes < HWA_PHYSICAL_HASH_BLOCK
                         ? buffer_bytes : HWA_PHYSICAL_HASH_BLOCK),
            before, error, error_size) != 0) {
        goto cleanup;
    }
    while (frame_offset < reader.format.frames) {
        size_t frames_read = 0U;
        size_t frame;
        if (hwa_wav_reader_read_frames(
                &reader, buffer, result->options.decode_block_frames,
                &frames_read, error, error_size) != 0) {
            goto cleanup;
        }
        if (frames_read == 0U) break;
        for (frame = 0U; frame < frames_read; ++frame) {
            long double mix = 0.0L;
            uint16_t channel;
            for (channel = 0U; channel < reader.format.channels; ++channel) {
                size_t offset = frame * reader.format.block_align +
                    (size_t)channel * reader.bytes_per_sample;
                int sample_clipped = 0;
                double value = hwa_wav_decode_sample(
                    &reader, buffer + offset, &sample_clipped);
                sample_values++;
                if (sample_clipped) clipped++;
                if (!isfinite(value)) {
                    probe->nonfinite_count++;
                    value = 0.0;
                } else {
                    double absolute = fabs(value);
                    if (absolute >= 1.0) hard_bound++;
                    if (absolute > 0.0 &&
                        ((reader.format.bits_per_sample == 32U &&
                          reader.format.encoding == HWA_ENCODING_IEEE_FLOAT &&
                          absolute < (double)FLT_MIN) ||
                         (reader.format.bits_per_sample == 64U &&
                          reader.format.encoding == HWA_ENCODING_IEEE_FLOAT &&
                          absolute < DBL_MIN))) {
                        denormal++;
                    }
                }
                mix += (long double)value;
            }
            probe->samples[(size_t)frame_offset + frame] =
                (double)(mix / (long double)reader.format.channels);
            if (!isfinite(probe->samples[(size_t)frame_offset + frame])) {
                hwa_set_error(error, error_size,
                              "Stage 5 channel mix overflowed");
                goto cleanup;
            }
        }
        frame_offset += (uint64_t)frames_read;
    }
    free(buffer);
    buffer = NULL;
    if (frame_offset != reader.format.frames) {
        hwa_set_error(error, error_size, "truncated Stage 5 WAVE input");
        goto cleanup;
    }
    if (probe->nonfinite_count != 0U) {
        hwa_set_error(error, error_size,
                      "Stage 5 WAVE contains non-finite samples");
        goto cleanup;
    }
    if (hwa_physical_probe_time_facts(
            result, probe, clipped, hard_bound, denormal, sample_values,
            error, error_size) != 0) {
        goto cleanup;
    }
    if (hwa_physical_probe_spectrum(
            result, probe, prior_probe_bytes, error, error_size) != 0) {
        goto cleanup;
    }
    hash_buffer_bytes = opened_identity.size < HWA_PHYSICAL_HASH_BLOCK
        ? opened_identity.size : HWA_PHYSICAL_HASH_BLOCK;
    if (hash_buffer_bytes == 0U ||
        prior_probe_bytes > UINT64_MAX - probe->work_bytes ||
        prior_probe_bytes + probe->work_bytes >
            UINT64_MAX - hash_buffer_bytes ||
        !hwa_physical_live_work_fits(
            result, prior_probe_bytes + probe->work_bytes +
                        hash_buffer_bytes)) {
        hwa_set_error(error, error_size,
                      "Stage 5 WAVE hash work exceeds its byte limit");
        goto cleanup;
    }
    buffer = (unsigned char *)malloc((size_t)hash_buffer_bytes);
    if (buffer == NULL) {
        hwa_set_error(error, error_size,
                      "cannot allocate Stage 5 WAVE hash work");
        goto cleanup;
    }
    if (hwa_physical_hash_stream(
            reader.file, &opened_identity, result->options.max_wave_bytes,
            reader.data_offset, buffer, (size_t)hash_buffer_bytes,
            after, error, error_size) != 0 ||
        strcmp(before, after) != 0) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "opened Stage 5 WAVE changed during analysis");
        }
        goto cleanup;
    }
    free(buffer);
    buffer = NULL;
    hwa_wav_reader_close(&reader);
    if (hwa_physical_file_identity(
            input->path, &after_identity, error, error_size) != 0 ||
        !hwa_physical_identity_unchanged(
            &probe->identity, &after_identity)) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "Stage 5 WAVE changed during analysis");
        }
        goto cleanup;
    }
    {
        uint64_t text_bytes = (uint64_t)strlen(input->role) + 1U;
        uint64_t path_bytes = (uint64_t)strlen(input->path) + 1U;
        if (text_bytes > UINT64_MAX - path_bytes ||
            prior_probe_bytes > UINT64_MAX - probe->work_bytes ||
            prior_probe_bytes + probe->work_bytes >
                UINT64_MAX - text_bytes - path_bytes ||
            !hwa_physical_live_work_fits(
                result, prior_probe_bytes + probe->work_bytes +
                            text_bytes + path_bytes)) {
            hwa_set_error(error, error_size,
                          "Stage 5 source text exceeds its byte limit");
            goto cleanup;
        }
    }
    source->role = hwa_physical_copy_cstring(
        result, input->role, error, error_size);
    source->path = hwa_physical_copy_cstring(
        result, input->path, error, error_size);
    if (source->role == NULL || source->path == NULL) goto cleanup;
    memcpy(source->sha256, before, HWA_SHA256_HEX_SIZE);
    source->format = probe->format;
    source->is_wave = 1;
    status = 0;

cleanup:
    hwa_wav_reader_close(&reader);
    free(buffer);
    if (status != 0) hwa_physical_probe_free(probe);
    return status;
}

static HWAPhysicalUnit hwa_physical_scan_unit(HWAPhysicalCheckKind kind)
{
    switch (kind) {
    case HWA_PHYSICAL_MAX_STEP_DBFS:
    case HWA_PHYSICAL_RETURN_LEVEL_DBFS:
        return HWA_PHYSICAL_UNIT_DBFS;
    case HWA_PHYSICAL_RUNAWAY_SLOPE_DB_PER_SECOND:
        return HWA_PHYSICAL_UNIT_DB_PER_SECOND;
    case HWA_PHYSICAL_FIXED_TONE_PROMINENCE_DB:
        return HWA_PHYSICAL_UNIT_DB;
    default:
        return HWA_PHYSICAL_UNIT_RATIO;
    }
}

static double hwa_physical_probe_scan_value(const HWAProbe *probe,
                                            HWAPhysicalCheckKind kind)
{
    switch (kind) {
    case HWA_PHYSICAL_DC_OFFSET: return probe->dc_offset;
    case HWA_PHYSICAL_DC_DRIFT: return probe->dc_drift;
    case HWA_PHYSICAL_CLIP_FRACTION: return probe->clip_fraction;
    case HWA_PHYSICAL_HARD_BOUND_FRACTION:
        return probe->hard_bound_fraction;
    case HWA_PHYSICAL_REPEATED_BLOCK_FRACTION:
        return probe->repeated_block_fraction;
    case HWA_PHYSICAL_STUCK_STATE_FRACTION:
        return probe->stuck_state_fraction;
    case HWA_PHYSICAL_MAX_STEP_DBFS: return probe->max_step_dbfs;
    case HWA_PHYSICAL_RUNAWAY_SLOPE_DB_PER_SECOND:
        return probe->runaway_slope_db_per_second;
    case HWA_PHYSICAL_RETURN_LEVEL_DBFS: return probe->return_level_dbfs;
    case HWA_PHYSICAL_HIGH_BAND_RATIO: return probe->high_band_ratio;
    case HWA_PHYSICAL_SUBHARMONIC_RATIO: return probe->subharmonic_ratio;
    case HWA_PHYSICAL_FIXED_TONE_PROMINENCE_DB:
        return probe->fixed_tone_prominence_db;
    case HWA_PHYSICAL_DENORMAL_FRACTION: return probe->denormal_fraction;
    default: return 0.0;
    }
}

static int hwa_physical_add_scan_checks(HWAPhysicalCheckSet *result,
                                        const HWAProbe *probe,
                                        const char *scope,
                                        char *error,
                                        size_t error_size)
{
    int value;
    for (value = (int)HWA_PHYSICAL_DC_OFFSET;
         value <= (int)HWA_PHYSICAL_DENORMAL_FRACTION; ++value) {
        HWAPhysicalCheckKind kind = (HWAPhysicalCheckKind)value;
        int spectral = kind == HWA_PHYSICAL_HIGH_BAND_RATIO ||
                       kind == HWA_PHYSICAL_SUBHARMONIC_RATIO ||
                       kind == HWA_PHYSICAL_FIXED_TONE_PROMINENCE_DB;
        int fallback = kind == HWA_PHYSICAL_SUBHARMONIC_RATIO ||
                       kind == HWA_PHYSICAL_FIXED_TONE_PROMINENCE_DB;
        HWAPhysicalAvailability availability =
            spectral && !probe->spectrum_valid
                ? HWA_PHYSICAL_INSUFFICIENT : HWA_PHYSICAL_AVAILABLE;
        uint32_t evidence = HWA_PHYSICAL_EVIDENCE_WAVE_SAMPLES;
        uint32_t quality = fallback ? HWA_PHYSICAL_QUALITY_FALLBACK : 0U;
        HWAPhysicalCheck *check;
        if (spectral && probe->spectrum_valid) {
            evidence |= HWA_PHYSICAL_EVIDENCE_SPECTRUM;
        } else if (spectral) {
            quality |= HWA_PHYSICAL_QUALITY_LOW_SIGNAL;
        }
        if (hwa_physical_add_check(
                result, scope, probe->role.case_id, "", kind,
                hwa_physical_scan_unit(kind), availability,
                evidence, quality,
                error, error_size) != 0) {
            return -1;
        }
        check = &result->checks[result->check_count - 1U];
        if (availability == HWA_PHYSICAL_AVAILABLE) {
            double measured = hwa_physical_probe_scan_value(probe, kind);
            check->confidence = 1.0;
            if (probe->role.side == HWA_PHYSICAL_ROLE_REFERENCE) {
                check->reference_value = measured;
                check->reference_valid = 1;
            } else {
                check->model_value = measured;
                check->model_valid = 1;
            }
        }
    }
    return 0;
}

typedef struct HWAPhysicalTraitSpec {
    HWAMeasureKind kind;
    double scale;
} HWAPhysicalTraitSpec;

static const HWAPhysicalTraitSpec hwa_physical_traits[] = {
    {HWA_MEASURE_FLATNESS, 0.10},
    {HWA_MEASURE_FIXED_STATE_FRACTION, 0.10},
    {HWA_MEASURE_ODD_EVEN_BALANCE_DB, 6.0},
    {HWA_MEASURE_HARMONIC_CENTROID, 1.0},
    {HWA_MEASURE_HNR_DB, 6.0},
    {HWA_MEASURE_HARMONIC_DECAY_DB_PER_SECOND, 20.0},
    {HWA_MEASURE_RISE_90_SECONDS, 0.05},
    {HWA_MEASURE_DECAY_DB, 6.0}
};

static int hwa_physical_take_pair_evaluation(
    HWAPhysicalCheckSet *result,
    char *error,
    size_t error_size);

static int hwa_physical_element_group(const HWAMeasureGroup *group)
{
    return group->selector == HWA_MEASURE_GROUP_PHYSICAL_ELEMENT &&
           group->item_kind == HWA_ITEM_BODY && group->item_role != NULL &&
           strcmp(group->item_role, "body") == 0 &&
           group->value != NULL && group->value[0] != '\0';
}

static int hwa_physical_statistic_key_order(
    const HWAMeasureStatistic *candidate,
    uint64_t group_id,
    HWAMeasureKind kind,
    uint32_t index,
    HWAMeasureView view)
{
    if (candidate->group_id != group_id) {
        return candidate->group_id < group_id ? -1 : 1;
    }
    if (candidate->kind != kind) {
        return candidate->kind < kind ? -1 : 1;
    }
    if (candidate->index != index) {
        return candidate->index < index ? -1 : 1;
    }
    if (candidate->view != view) {
        return candidate->view < view ? -1 : 1;
    }
    return 0;
}

static int hwa_physical_statistic_value(HWAPhysicalCheckSet *result,
                                        const HWAMeasurementSet *set,
                                        const HWAMeasureGroup *group,
                                        HWAMeasureKind kind,
                                        double *value,
                                        double *confidence,
                                        char *error,
                                        size_t error_size)
{
    const HWAMeasureStatistic *statistic = NULL;
    size_t low = 0U;
    size_t high = set->statistic_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        const HWAMeasureStatistic *candidate = &set->statistics[middle];
        int order;
        if (hwa_physical_take_pair_evaluation(
                result, error, error_size) != 0) return -1;
        order = hwa_physical_statistic_key_order(
            candidate, group->id, kind, 0U, HWA_MEASURE_VIEW_RAW);
        if (order < 0) {
            low = middle + 1U;
        } else if (order > 0) {
            high = middle;
        } else {
            statistic = candidate;
            break;
        }
    }
    if (statistic == NULL || !statistic->statistics.valid ||
        statistic->statistics.valid_count < 2U ||
        !isfinite(statistic->statistics.q50)) {
        return 0;
    }
    *value = statistic->statistics.q50;
    *confidence = statistic->statistics.confidence;
    return 1;
}

static int hwa_physical_take_pair_evaluation(
    HWAPhysicalCheckSet *result,
    char *error,
    size_t error_size)
{
    if (result->pair_evaluations >= result->options.max_pair_evaluations) {
        hwa_set_error(error, error_size,
                      "Stage 5 pair evaluations exceed their limit");
        return -1;
    }
    result->pair_evaluations++;
    return 0;
}

static int hwa_physical_shape_distance(
    HWAPhysicalCheckSet *result,
    const HWAMeasurementSet *set,
    const HWAMeasureGroup *first,
    const HWAMeasureGroup *second,
    double *distance,
    double *confidence,
    size_t *coverage,
    char *error,
    size_t error_size)
{
    long double squared = 0.0L;
    double minimum_confidence = 1.0;
    size_t trait;
    size_t valid = 0U;

    for (trait = 0U; trait < HWA_PHYSICAL_ARRAY_COUNT(hwa_physical_traits);
         ++trait) {
        double first_value;
        double second_value;
        double first_confidence;
        double second_confidence;
        int first_status;
        int second_status;
        if (hwa_physical_take_pair_evaluation(
                result, error, error_size) != 0) return -1;
        first_status = hwa_physical_statistic_value(
            result, set, first, hwa_physical_traits[trait].kind,
            &first_value, &first_confidence, error, error_size);
        if (first_status < 0) return -1;
        second_status = hwa_physical_statistic_value(
            result, set, second, hwa_physical_traits[trait].kind,
            &second_value, &second_confidence, error, error_size);
        if (second_status < 0) return -1;
        if (first_status > 0 && second_status > 0) {
            double normalized = (second_value - first_value) /
                                hwa_physical_traits[trait].scale;
            squared += (long double)normalized * (long double)normalized;
            if (first_confidence < minimum_confidence) {
                minimum_confidence = first_confidence;
            }
            if (second_confidence < minimum_confidence) {
                minimum_confidence = second_confidence;
            }
            valid++;
        }
    }
    *coverage = valid;
    if (valid < 4U) return 0;
    *distance = sqrt((double)(squared / (long double)valid));
    *confidence = minimum_confidence *
        (double)valid /
        (double)HWA_PHYSICAL_ARRAY_COUNT(hwa_physical_traits);
    return 1;
}

static char *hwa_physical_pair_label(HWAPhysicalCheckSet *result,
                                     const char *first,
                                     const char *second,
                                     uint64_t *work_bytes,
                                     char *error,
                                     size_t error_size)
{
    size_t first_length = strlen(first);
    size_t second_length = strlen(second);
    char first_prefix[32];
    char second_prefix[32];
    int first_prefix_length;
    int second_prefix_length;
    size_t length;
    size_t offset;
    uint64_t label_bytes;
    char *label;
    *work_bytes = 0U;
    first_prefix_length = snprintf(first_prefix, sizeof(first_prefix),
                                   "%zu:", first_length);
    second_prefix_length = snprintf(second_prefix, sizeof(second_prefix),
                                    "%zu:", second_length);
    if (first_prefix_length <= 0 || second_prefix_length <= 0 ||
        (size_t)first_prefix_length >= sizeof(first_prefix) ||
        (size_t)second_prefix_length >= sizeof(second_prefix)) {
        hwa_set_error(error, error_size, "Stage 5 element label overflows");
        return NULL;
    }
    length = (size_t)first_prefix_length;
    if (first_length > SIZE_MAX - length) goto overflow;
    length += first_length;
    if (length == SIZE_MAX) goto overflow;
    length++;
    if ((size_t)second_prefix_length > SIZE_MAX - length) goto overflow;
    length += (size_t)second_prefix_length;
    if (second_length > SIZE_MAX - length ||
        length + second_length == SIZE_MAX) goto overflow;
    length += second_length;
    label_bytes = (uint64_t)length + 1U;
    if ((size_t)(label_bytes - 1U) != length || label_bytes == 0U ||
        !hwa_physical_live_work_fits(result, label_bytes)) {
        hwa_set_error(error, error_size,
                      "Stage 5 element label exceeds its byte limit");
        return NULL;
    }
    label = (char *)malloc(length + 1U);
    if (label == NULL) {
        hwa_set_error(error, error_size,
                      "cannot allocate Stage 5 element label");
        return NULL;
    }
    offset = 0U;
    memcpy(label + offset, first_prefix, (size_t)first_prefix_length);
    offset += (size_t)first_prefix_length;
    memcpy(label + offset, first, first_length);
    offset += first_length;
    label[offset++] = '|';
    memcpy(label + offset, second_prefix, (size_t)second_prefix_length);
    offset += (size_t)second_prefix_length;
    memcpy(label + offset, second, second_length);
    label[length] = '\0';
    *work_bytes = label_bytes;
    return label;

overflow:
    hwa_set_error(error, error_size, "Stage 5 element label overflows");
    return NULL;
}

static void hwa_physical_set_available_pair(HWAPhysicalCheck *check,
                                            double reference,
                                            double model,
                                            double confidence)
{
    check->availability = HWA_PHYSICAL_AVAILABLE;
    check->reference_value = reference;
    check->model_value = model;
    check->delta = model - reference;
    check->confidence = confidence;
    check->reference_valid = 1;
    check->model_valid = 1;
    check->delta_valid = 1;
}

static void hwa_physical_set_available_single(HWAPhysicalCheck *check,
                                              HWAPhysicalRoleSide side,
                                              double value,
                                              double confidence)
{
    check->availability = HWA_PHYSICAL_AVAILABLE;
    check->confidence = confidence;
    if (side == HWA_PHYSICAL_ROLE_REFERENCE) {
        check->reference_value = value;
        check->reference_valid = 1;
    } else {
        check->model_value = value;
        check->model_valid = 1;
    }
}

static int hwa_physical_add_element_pair_checks(
    HWAPhysicalCheckSet *result,
    const HWAMeasurementSet *reference,
    const HWAMeasurementSet *model,
    const HWAMeasureGroup *reference_first,
    const HWAMeasureGroup *reference_second,
    const HWAMeasureGroup *model_first,
    const HWAMeasureGroup *model_second,
    const char *pair_label,
    char *error,
    size_t error_size)
{
    double reference_distance = 0.0;
    double model_distance = 0.0;
    double reference_confidence = 0.0;
    double model_confidence = 0.0;
    double reference_level_first = 0.0;
    double reference_level_second = 0.0;
    double model_level_first = 0.0;
    double model_level_second = 0.0;
    double unused_confidence;
    size_t reference_coverage = 0U;
    size_t model_coverage = 0U;
    int reference_valid;
    int model_valid;
    int level_status[4];
    HWAPhysicalCheck *check;

    reference_valid = hwa_physical_shape_distance(
        result, reference, reference_first, reference_second,
        &reference_distance, &reference_confidence, &reference_coverage,
        error, error_size);
    if (reference_valid < 0) return -1;
    model_valid = hwa_physical_shape_distance(
        result, model, model_first, model_second,
        &model_distance, &model_confidence, &model_coverage,
        error, error_size);
    if (model_valid < 0) return -1;
    if (!reference_valid || !model_valid) return 0;

    if (hwa_physical_add_check(
            result, "reference-profile", "", pair_label,
            HWA_PHYSICAL_ELEMENT_REFERENCE_DISTANCE,
            HWA_PHYSICAL_UNIT_RATIO, HWA_PHYSICAL_AVAILABLE,
            HWA_PHYSICAL_EVIDENCE_REFERENCE_PROFILE |
                HWA_PHYSICAL_EVIDENCE_ELEMENT_LABEL,
            reference_coverage < HWA_PHYSICAL_ARRAY_COUNT(hwa_physical_traits)
                ? HWA_PHYSICAL_QUALITY_LOW_COVERAGE : 0U,
            error, error_size) != 0) return -1;
    check = &result->checks[result->check_count - 1U];
    check->reference_value = reference_distance;
    check->reference_valid = 1;
    check->confidence = reference_confidence;

    if (hwa_physical_add_check(
            result, "model-profile", "", pair_label,
            HWA_PHYSICAL_ELEMENT_MODEL_DISTANCE,
            HWA_PHYSICAL_UNIT_RATIO, HWA_PHYSICAL_AVAILABLE,
            HWA_PHYSICAL_EVIDENCE_MODEL_PROFILE |
                HWA_PHYSICAL_EVIDENCE_ELEMENT_LABEL,
            model_coverage < HWA_PHYSICAL_ARRAY_COUNT(hwa_physical_traits)
                ? HWA_PHYSICAL_QUALITY_LOW_COVERAGE : 0U,
            error, error_size) != 0) return -1;
    check = &result->checks[result->check_count - 1U];
    check->model_value = model_distance;
    check->model_valid = 1;
    check->confidence = model_confidence;

    if (hwa_physical_add_check(
            result, "profiles", "", pair_label,
            HWA_PHYSICAL_ELEMENT_DISTINCTNESS_RATIO,
            HWA_PHYSICAL_UNIT_RATIO, HWA_PHYSICAL_AVAILABLE,
            HWA_PHYSICAL_EVIDENCE_REFERENCE_PROFILE |
                HWA_PHYSICAL_EVIDENCE_MODEL_PROFILE |
                HWA_PHYSICAL_EVIDENCE_ELEMENT_LABEL,
            0U, error, error_size) != 0) return -1;
    check = &result->checks[result->check_count - 1U];
    hwa_physical_set_available_pair(
        check, reference_distance, model_distance,
        fmin(reference_confidence, model_confidence));

    level_status[0] = hwa_physical_statistic_value(
        result, reference, reference_first, HWA_MEASURE_RMS_DBFS,
        &reference_level_first, &unused_confidence, error, error_size);
    level_status[1] = hwa_physical_statistic_value(
        result, reference, reference_second, HWA_MEASURE_RMS_DBFS,
        &reference_level_second, &unused_confidence, error, error_size);
    level_status[2] = hwa_physical_statistic_value(
        result, model, model_first, HWA_MEASURE_RMS_DBFS,
        &model_level_first, &unused_confidence, error, error_size);
    level_status[3] = hwa_physical_statistic_value(
        result, model, model_second, HWA_MEASURE_RMS_DBFS,
        &model_level_second, &unused_confidence, error, error_size);
    if (level_status[0] < 0 || level_status[1] < 0 ||
        level_status[2] < 0 || level_status[3] < 0) return -1;
    if (level_status[0] > 0 && level_status[1] > 0 &&
        level_status[2] > 0 && level_status[3] > 0) {
        double reference_gain = fmin(
            1.0, fabs(reference_level_second - reference_level_first) / 6.0);
        double model_gain = fmin(
            1.0, fabs(model_level_second - model_level_first) / 6.0);
        double reference_score = reference_gain / (1.0 + reference_distance);
        double model_score = model_gain / (1.0 + model_distance);
        if (hwa_physical_add_check(
                result, "profiles", "", pair_label,
                HWA_PHYSICAL_ELEMENT_GAIN_ONLY_SCORE,
                HWA_PHYSICAL_UNIT_RATIO, HWA_PHYSICAL_AVAILABLE,
                HWA_PHYSICAL_EVIDENCE_REFERENCE_PROFILE |
                    HWA_PHYSICAL_EVIDENCE_MODEL_PROFILE |
                    HWA_PHYSICAL_EVIDENCE_ELEMENT_LABEL,
                HWA_PHYSICAL_QUALITY_PITCH_CONFOUNDED,
                error, error_size) != 0) return -1;
        check = &result->checks[result->check_count - 1U];
        hwa_physical_set_available_pair(
            check, reference_score, model_score,
            fmin(reference_confidence, model_confidence));
    }

    if (hwa_physical_add_check(
            result, "profiles", "", pair_label,
            HWA_PHYSICAL_ELEMENT_PITCH_ONLY_SCORE,
            HWA_PHYSICAL_UNIT_RATIO, HWA_PHYSICAL_UNAVAILABLE,
            HWA_PHYSICAL_EVIDENCE_REFERENCE_PROFILE |
                HWA_PHYSICAL_EVIDENCE_MODEL_PROFILE |
                HWA_PHYSICAL_EVIDENCE_ELEMENT_LABEL,
            HWA_PHYSICAL_QUALITY_PITCH_CONFOUNDED,
            error, error_size) != 0) return -1;
    return 1;
}

typedef struct HWAPhysicalElementGroupPair {
    const HWAMeasureGroup *reference;
    const HWAMeasureGroup *model;
} HWAPhysicalElementGroupPair;

static int hwa_physical_match_element_groups(
    HWAPhysicalCheckSet *result,
    const HWAMeasurementSet *reference,
    const HWAMeasurementSet *model,
    HWAPhysicalElementGroupPair *pairs,
    size_t capacity,
    size_t *match_count,
    char *error,
    size_t error_size)
{
    size_t reference_index = 0U;
    size_t model_index = 0U;
    size_t matched = 0U;
    while (reference_index < reference->group_count &&
           model_index < model->group_count) {
        const HWAMeasureGroup *reference_group;
        const HWAMeasureGroup *model_group;
        int order;
        while (reference_index < reference->group_count &&
               !hwa_physical_element_group(
                   &reference->groups[reference_index])) {
            if (hwa_physical_take_pair_evaluation(
                    result, error, error_size) != 0) return -1;
            reference_index++;
        }
        while (model_index < model->group_count &&
               !hwa_physical_element_group(&model->groups[model_index])) {
            if (hwa_physical_take_pair_evaluation(
                    result, error, error_size) != 0) return -1;
            model_index++;
        }
        if (reference_index >= reference->group_count ||
            model_index >= model->group_count) break;
        if (hwa_physical_take_pair_evaluation(
                result, error, error_size) != 0) return -1;
        reference_group = &reference->groups[reference_index];
        model_group = &model->groups[model_index];
        order = strcmp(reference_group->value, model_group->value);
        if (order == 0) {
            if (pairs != NULL) {
                if (matched >= capacity) {
                    hwa_set_error(error, error_size,
                                  "Stage 5 element groups changed while matching");
                    return -1;
                }
                pairs[matched].reference = reference_group;
                pairs[matched].model = model_group;
            }
            if (matched == SIZE_MAX) {
                hwa_set_error(error, error_size,
                              "Stage 5 element group count overflows");
                return -1;
            }
            matched++;
            reference_index++;
            model_index++;
        } else if (order < 0) {
            reference_index++;
        } else {
            model_index++;
        }
    }
    *match_count = matched;
    return 0;
}

static int hwa_physical_add_element_trait_checks(
    HWAPhysicalCheckSet *result,
    const HWAMeasurementSet *reference,
    const HWAMeasurementSet *model,
    char *error,
    size_t error_size)
{
    HWAPhysicalElementGroupPair *groups = NULL;
    uint64_t saved_work_limit = result->options.max_work_bytes;
    uint64_t scratch_bytes = 0U;
    size_t group_count = 0U;
    size_t filled_count = 0U;
    size_t group;
    size_t pair_count = 0U;
    size_t trait_count = 0U;
    int status = -1;

    if (hwa_physical_match_element_groups(
            result, reference, model, NULL, 0U, &group_count,
            error, error_size) != 0) goto cleanup;
    if (group_count > SIZE_MAX / sizeof(*groups)) {
        hwa_set_error(error, error_size,
                      "Stage 5 element group work overflows");
        goto cleanup;
    }
    scratch_bytes = (uint64_t)group_count * sizeof(*groups);
    if (scratch_bytes > 0U) {
        if (!hwa_physical_live_work_fits(result, scratch_bytes)) {
            hwa_set_error(error, error_size,
                          "Stage 5 element group work exceeds its byte limit");
            goto cleanup;
        }
        result->options.max_work_bytes = saved_work_limit - scratch_bytes;
        groups = (HWAPhysicalElementGroupPair *)malloc((size_t)scratch_bytes);
        if (groups == NULL) {
            hwa_set_error(error, error_size,
                          "cannot allocate Stage 5 element group work");
            goto cleanup;
        }
        if (hwa_physical_match_element_groups(
                result, reference, model, groups, group_count, &filled_count,
                error, error_size) != 0) goto cleanup;
        if (filled_count != group_count) {
            hwa_set_error(error, error_size,
                          "Stage 5 element groups changed while matching");
            goto cleanup;
        }
    }

    for (group = 0U; group < group_count; ++group) {
        size_t trait;
        for (trait = 0U;
             trait < HWA_PHYSICAL_ARRAY_COUNT(hwa_physical_traits); ++trait) {
            double reference_value;
            double model_value;
            double reference_confidence;
            double model_confidence;
            int reference_status;
            int model_status;
            HWAPhysicalCheck *check;
            if (hwa_physical_take_pair_evaluation(
                    result, error, error_size) != 0) goto cleanup;
            reference_status = hwa_physical_statistic_value(
                result, reference, groups[group].reference,
                hwa_physical_traits[trait].kind,
                &reference_value, &reference_confidence, error, error_size);
            if (reference_status < 0) goto cleanup;
            model_status = hwa_physical_statistic_value(
                result, model, groups[group].model,
                hwa_physical_traits[trait].kind,
                &model_value, &model_confidence, error, error_size);
            if (model_status < 0) goto cleanup;
            if (reference_status == 0 || model_status == 0) continue;
            if (hwa_physical_add_check(
                    result, "profiles", "", groups[group].reference->value,
                    HWA_PHYSICAL_ELEMENT_TRAIT_DELTA,
                    HWA_PHYSICAL_UNIT_RATIO, HWA_PHYSICAL_AVAILABLE,
                    HWA_PHYSICAL_EVIDENCE_REFERENCE_PROFILE |
                        HWA_PHYSICAL_EVIDENCE_MODEL_PROFILE |
                        HWA_PHYSICAL_EVIDENCE_ELEMENT_LABEL,
                    HWA_PHYSICAL_QUALITY_PITCH_CONFOUNDED,
                    error, error_size) != 0) goto cleanup;
            check = &result->checks[result->check_count - 1U];
            check->index = (uint32_t)hwa_physical_traits[trait].kind;
            hwa_physical_set_available_pair(
                check,
                reference_value / hwa_physical_traits[trait].scale,
                model_value / hwa_physical_traits[trait].scale,
                fmin(reference_confidence, model_confidence));
            trait_count++;
        }
    }

    for (group = 0U; group < group_count; ++group) {
        size_t second;
        for (second = group + 1U; second < group_count; ++second) {
            char *pair_label;
            uint64_t label_work_bytes;
            int pair_result;
            if (hwa_physical_take_pair_evaluation(
                    result, error, error_size) != 0) goto cleanup;
            pair_label = hwa_physical_pair_label(
                result,
                groups[group].reference->value,
                groups[second].reference->value,
                &label_work_bytes,
                error, error_size);
            if (pair_label == NULL) goto cleanup;
            result->options.max_work_bytes -= label_work_bytes;
            pair_result = hwa_physical_add_element_pair_checks(
                result, reference, model,
                groups[group].reference, groups[second].reference,
                groups[group].model, groups[second].model, pair_label,
                error, error_size);
            free(pair_label);
            result->options.max_work_bytes += label_work_bytes;
            if (pair_result < 0) goto cleanup;
            if (pair_result > 0) pair_count++;
        }
    }
    if (trait_count == 0U || pair_count == 0U) {
        if (hwa_physical_add_check(
                result, "profiles", "", "",
                HWA_PHYSICAL_ELEMENT_TRAIT_DELTA, HWA_PHYSICAL_UNIT_RATIO,
                HWA_PHYSICAL_INSUFFICIENT,
                HWA_PHYSICAL_EVIDENCE_REFERENCE_PROFILE |
                    HWA_PHYSICAL_EVIDENCE_MODEL_PROFILE,
                HWA_PHYSICAL_QUALITY_LOW_COVERAGE,
                error, error_size) != 0) goto cleanup;
    }
    status = 0;

cleanup:
    free(groups);
    result->options.max_work_bytes = saved_work_limit;
    return status;
}

static int hwa_physical_find_role_group(
    HWAPhysicalCheckSet *result,
    const HWAMeasurementSet *set,
    HWAItemKind kind,
    const char *role,
    const HWAMeasureGroup **found,
    char *error,
    size_t error_size)
{
    size_t group;
    *found = NULL;
    for (group = 0U; group < set->group_count; ++group) {
        const HWAMeasureGroup *candidate = &set->groups[group];
        if (hwa_physical_take_pair_evaluation(
                result, error, error_size) != 0) return -1;
        if (candidate->item_kind == kind &&
            candidate->selector == HWA_MEASURE_GROUP_ALL &&
            candidate->item_role != NULL &&
            strcmp(candidate->item_role, role) == 0) {
            *found = candidate;
            return 0;
        }
    }
    return 0;
}

static int hwa_physical_add_carryover_check(
    HWAPhysicalCheckSet *result,
    const HWAMeasurementSet *reference,
    const HWAMeasurementSet *model,
    char *error,
    size_t error_size)
{
    const HWAMeasureGroup *reference_group = NULL;
    const HWAMeasureGroup *model_group = NULL;
    double reference_value;
    double model_value;
    double reference_confidence;
    double model_confidence;
    int reference_status = 0;
    int model_status = 0;
    HWAPhysicalCheck *check;

    if (hwa_physical_find_role_group(
            result, reference, HWA_ITEM_TRANSITION,
            "physical-element-change", &reference_group,
            error, error_size) != 0 ||
        hwa_physical_find_role_group(
            result, model, HWA_ITEM_TRANSITION,
            "physical-element-change", &model_group,
            error, error_size) != 0) return -1;
    if (reference_group != NULL) {
        reference_status = hwa_physical_statistic_value(
            result, reference, reference_group, HWA_MEASURE_CARRYOVER_DB,
            &reference_value, &reference_confidence, error, error_size);
        if (reference_status < 0) return -1;
    }
    if (model_group != NULL) {
        model_status = hwa_physical_statistic_value(
            result, model, model_group, HWA_MEASURE_CARRYOVER_DB,
            &model_value, &model_confidence, error, error_size);
        if (model_status < 0) return -1;
    }
    if (reference_status == 0 || model_status == 0) {
        return hwa_physical_add_check(
            result, "profiles", "", "",
            HWA_PHYSICAL_ELEMENT_CARRYOVER_DB, HWA_PHYSICAL_UNIT_DB,
            HWA_PHYSICAL_INSUFFICIENT,
            HWA_PHYSICAL_EVIDENCE_REFERENCE_PROFILE |
                HWA_PHYSICAL_EVIDENCE_MODEL_PROFILE,
            HWA_PHYSICAL_QUALITY_LOW_COVERAGE,
            error, error_size);
    }
    if (hwa_physical_add_check(
            result, "profiles", "", "",
            HWA_PHYSICAL_ELEMENT_CARRYOVER_DB, HWA_PHYSICAL_UNIT_DB,
            HWA_PHYSICAL_AVAILABLE,
            HWA_PHYSICAL_EVIDENCE_REFERENCE_PROFILE |
                HWA_PHYSICAL_EVIDENCE_MODEL_PROFILE |
                HWA_PHYSICAL_EVIDENCE_ELEMENT_LABEL,
            HWA_PHYSICAL_QUALITY_PITCH_CONFOUNDED,
            error, error_size) != 0) return -1;
    check = &result->checks[result->check_count - 1U];
    hwa_physical_set_available_pair(
        check, reference_value, model_value,
        fmin(reference_confidence, model_confidence));
    return 0;
}

static HWAPhysicalUnit hwa_physical_kind_unit(HWAPhysicalCheckKind kind)
{
    switch (kind) {
    case HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ:
    case HWA_PHYSICAL_BODY_MODE_BANDWIDTH_HZ:
    case HWA_PHYSICAL_BEATING_RATE_HZ:
        return HWA_PHYSICAL_UNIT_HZ;
    case HWA_PHYSICAL_BODY_MODE_PROMINENCE_DB:
    case HWA_PHYSICAL_JOINT_RESIDUAL_DB:
    case HWA_PHYSICAL_SHARED_GAIN_DB:
    case HWA_PHYSICAL_SUM_TONE_DB:
    case HWA_PHYSICAL_DIFFERENCE_TONE_DB:
    case HWA_PHYSICAL_RENDER_RMS_ERROR_DB:
    case HWA_PHYSICAL_RENDER_SPECTRAL_DISTANCE_DB:
        return HWA_PHYSICAL_UNIT_DB;
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
        return HWA_PHYSICAL_UNIT_DBFS;
    case HWA_PHYSICAL_RENDER_LAG_SAMPLES:
        return HWA_PHYSICAL_UNIT_SAMPLES;
    default:
        return HWA_PHYSICAL_UNIT_RATIO;
    }
}

static int hwa_physical_add_unavailable_family(
    HWAPhysicalCheckSet *result,
    const char *scope,
    const char *case_id,
    HWAPhysicalCheckKind first,
    HWAPhysicalCheckKind last,
    HWAPhysicalAvailability availability,
    uint32_t evidence,
    uint32_t quality,
    char *error,
    size_t error_size)
{
    int value;
    for (value = (int)first; value <= (int)last; ++value) {
        HWAPhysicalCheckKind kind = (HWAPhysicalCheckKind)value;
        if (hwa_physical_add_check(
                result, scope, case_id, "", kind,
                hwa_physical_kind_unit(kind), availability, evidence, quality,
                error, error_size) != 0) return -1;
    }
    return 0;
}

static int hwa_physical_same_case(const HWAProbe *probe,
                                  HWAPhysicalRoleSide side,
                                  HWAPhysicalRoleKind kind,
                                  const char *case_id)
{
    return probe->role.side == side && probe->role.kind == kind &&
           strcmp(probe->role.case_id, case_id) == 0;
}

static HWAProbe *hwa_physical_find_probe(HWAProbe *probes,
                                        size_t probe_count,
                                        HWAPhysicalRoleSide side,
                                        HWAPhysicalRoleKind kind,
                                        const char *case_id)
{
    size_t index;
    for (index = 0U; index < probe_count; ++index) {
        if (hwa_physical_same_case(&probes[index], side, kind, case_id)) {
            return &probes[index];
        }
    }
    return NULL;
}

static int hwa_physical_case_seen(const HWAProbe *probes,
                                  size_t before,
                                  HWAPhysicalRoleKind kind,
                                  const char *case_id)
{
    size_t index;
    for (index = 0U; index < before; ++index) {
        if (probes[index].role.kind == kind &&
            strcmp(probes[index].role.case_id, case_id) == 0) return 1;
    }
    return 0;
}

static int hwa_physical_add_body_mode_pair(
    HWAPhysicalCheckSet *result,
    const char *case_id,
    uint32_t index,
    const HWAPhysicalMode *reference,
    const HWAPhysicalMode *model,
    char *error,
    size_t error_size)
{
    static const HWAPhysicalCheckKind kinds[5] = {
        HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ,
        HWA_PHYSICAL_BODY_MODE_BANDWIDTH_HZ,
        HWA_PHYSICAL_BODY_MODE_Q,
        HWA_PHYSICAL_BODY_MODE_PROMINENCE_DB,
        HWA_PHYSICAL_BODY_MODE_DECAY_SECONDS
    };
    const double reference_values[5] = {
        reference->frequency_hz, reference->bandwidth_hz, reference->q,
        reference->prominence_db, reference->decay_seconds
    };
    const double model_values[5] = {
        model->frequency_hz, model->bandwidth_hz, model->q,
        model->prominence_db, model->decay_seconds
    };
    size_t fact;
    for (fact = 0U; fact < HWA_PHYSICAL_ARRAY_COUNT(kinds); ++fact) {
        HWAPhysicalCheck *check;
        if (hwa_physical_add_check(
                result, "body", case_id, "", kinds[fact],
                hwa_physical_kind_unit(kinds[fact]), HWA_PHYSICAL_AVAILABLE,
                HWA_PHYSICAL_EVIDENCE_WAVE_SAMPLES |
                    HWA_PHYSICAL_EVIDENCE_SPECTRUM |
                    HWA_PHYSICAL_EVIDENCE_BODY_RESPONSE,
                HWA_PHYSICAL_QUALITY_FALLBACK |
                    HWA_PHYSICAL_QUALITY_ROOM_CONFOUNDED,
                error, error_size) != 0) return -1;
        check = &result->checks[result->check_count - 1U];
        check->index = index;
        hwa_physical_set_available_pair(
            check, reference_values[fact], model_values[fact], 0.70);
    }
    return 0;
}

static int hwa_physical_add_body_single(
    HWAPhysicalCheckSet *result,
    const HWAProbe *probe,
    const char *case_id,
    char *error,
    size_t error_size)
{
    static const HWAPhysicalCheckKind kinds[5] = {
        HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ,
        HWA_PHYSICAL_BODY_MODE_BANDWIDTH_HZ,
        HWA_PHYSICAL_BODY_MODE_Q,
        HWA_PHYSICAL_BODY_MODE_PROMINENCE_DB,
        HWA_PHYSICAL_BODY_MODE_DECAY_SECONDS
    };
    size_t mode;
    uint32_t evidence = HWA_PHYSICAL_EVIDENCE_WAVE_SAMPLES |
                        HWA_PHYSICAL_EVIDENCE_BODY_RESPONSE;
    if (!probe->spectrum_valid) {
        return hwa_physical_add_unavailable_family(
            result, "body", case_id,
            HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ,
            HWA_PHYSICAL_BODY_MODE_DECAY_SECONDS,
            HWA_PHYSICAL_INSUFFICIENT, evidence,
            HWA_PHYSICAL_QUALITY_LOW_SIGNAL |
                HWA_PHYSICAL_QUALITY_ROOM_CONFOUNDED,
            error, error_size);
    }
    evidence |= HWA_PHYSICAL_EVIDENCE_SPECTRUM;
    for (mode = 0U; mode < probe->mode_count; ++mode) {
        const double values[5] = {
            probe->modes[mode].frequency_hz,
            probe->modes[mode].bandwidth_hz,
            probe->modes[mode].q,
            probe->modes[mode].prominence_db,
            probe->modes[mode].decay_seconds
        };
        size_t fact;
        if (mode > (size_t)UINT32_MAX) {
            hwa_set_error(error, error_size,
                          "Stage 5 body mode index overflows");
            return -1;
        }
        for (fact = 0U; fact < HWA_PHYSICAL_ARRAY_COUNT(kinds); ++fact) {
            HWAPhysicalCheck *check;
            if (hwa_physical_add_check(
                    result, "body", case_id, "", kinds[fact],
                    hwa_physical_kind_unit(kinds[fact]),
                    HWA_PHYSICAL_AVAILABLE, evidence,
                    HWA_PHYSICAL_QUALITY_FALLBACK |
                        HWA_PHYSICAL_QUALITY_ROOM_CONFOUNDED,
                    error, error_size) != 0) return -1;
            check = &result->checks[result->check_count - 1U];
            check->index = (uint32_t)mode;
            hwa_physical_set_available_single(
                check, probe->role.side, values[fact], 0.70);
        }
    }
    if (probe->mode_count == 0U &&
        hwa_physical_add_unavailable_family(
            result, "body", case_id,
            HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ,
            HWA_PHYSICAL_BODY_MODE_DECAY_SECONDS,
            HWA_PHYSICAL_INSUFFICIENT, evidence,
            HWA_PHYSICAL_QUALITY_LOW_COVERAGE |
                HWA_PHYSICAL_QUALITY_ROOM_CONFOUNDED,
            error, error_size) != 0) return -1;
    return 0;
}

static int hwa_physical_add_body_case(HWAPhysicalCheckSet *result,
                                      HWAProbe *reference,
                                      HWAProbe *model,
                                      const char *case_id,
                                      char *error,
                                      size_t error_size)
{
    uint32_t evidence = HWA_PHYSICAL_EVIDENCE_WAVE_SAMPLES |
                        HWA_PHYSICAL_EVIDENCE_BODY_RESPONSE;
    size_t reference_index = 0U;
    size_t model_index = 0U;
    size_t matched = 0U;
    long double cents_sum = 0.0L;
    HWAPhysicalCheck *check;

    if (reference == NULL || model == NULL) {
        const HWAProbe *single = reference != NULL ? reference : model;
        if (single == NULL || hwa_physical_add_body_single(
                result, single, case_id, error, error_size) != 0) return -1;
        evidence = HWA_PHYSICAL_EVIDENCE_WAVE_SAMPLES |
                   HWA_PHYSICAL_EVIDENCE_BODY_RESPONSE;
        if (single != NULL && single->spectrum_valid) {
            evidence |= HWA_PHYSICAL_EVIDENCE_SPECTRUM;
        }
        if (hwa_physical_add_check(
                result, "body", case_id, "", HWA_PHYSICAL_BODY_MODE_PAN,
                HWA_PHYSICAL_UNIT_RATIO, HWA_PHYSICAL_UNAVAILABLE,
                evidence, HWA_PHYSICAL_QUALITY_FALLBACK |
                              HWA_PHYSICAL_QUALITY_ROOM_CONFOUNDED,
                error, error_size) != 0) return -1;
        if (hwa_physical_add_check(
                result, "body", case_id, "",
                HWA_PHYSICAL_BODY_MODE_DENSITY_PER_KHZ,
                HWA_PHYSICAL_UNIT_COUNT_VALUE,
                single != NULL && single->spectrum_valid
                    ? HWA_PHYSICAL_AVAILABLE : HWA_PHYSICAL_INSUFFICIENT,
                evidence, HWA_PHYSICAL_QUALITY_FALLBACK |
                              HWA_PHYSICAL_QUALITY_ROOM_CONFOUNDED,
                error, error_size) != 0) return -1;
        if (single != NULL && single->spectrum_valid) {
            check = &result->checks[result->check_count - 1U];
            hwa_physical_set_available_single(
                check, single->role.side,
                (double)single->mode_count /
                    ((double)single->format.sample_rate_hz / 2000.0),
                0.70);
        }
        return hwa_physical_add_check(
            result, "body", case_id, "",
            HWA_PHYSICAL_BODY_MODE_DISTANCE_CENTS,
            HWA_PHYSICAL_UNIT_CENTS, HWA_PHYSICAL_UNAVAILABLE,
            evidence, HWA_PHYSICAL_QUALITY_LOW_COVERAGE |
                          HWA_PHYSICAL_QUALITY_ROOM_CONFOUNDED,
            error, error_size);
    }
    if (!reference->spectrum_valid || !model->spectrum_valid) {
        const HWAProbe *single = reference->spectrum_valid
                                     ? reference
                                     : model->spectrum_valid ? model : NULL;
        if (single != NULL) {
            if (hwa_physical_add_body_single(
                    result, single, case_id, error, error_size) != 0 ||
                hwa_physical_add_check(
                    result, "body", case_id, "", HWA_PHYSICAL_BODY_MODE_PAN,
                    HWA_PHYSICAL_UNIT_RATIO, HWA_PHYSICAL_UNAVAILABLE,
                    HWA_PHYSICAL_EVIDENCE_WAVE_SAMPLES |
                        HWA_PHYSICAL_EVIDENCE_SPECTRUM |
                        HWA_PHYSICAL_EVIDENCE_BODY_RESPONSE,
                    HWA_PHYSICAL_QUALITY_FALLBACK |
                        HWA_PHYSICAL_QUALITY_ROOM_CONFOUNDED,
                    error, error_size) != 0 ||
                hwa_physical_add_check(
                    result, "body", case_id, "",
                    HWA_PHYSICAL_BODY_MODE_DENSITY_PER_KHZ,
                    HWA_PHYSICAL_UNIT_COUNT_VALUE, HWA_PHYSICAL_AVAILABLE,
                    HWA_PHYSICAL_EVIDENCE_WAVE_SAMPLES |
                        HWA_PHYSICAL_EVIDENCE_SPECTRUM |
                        HWA_PHYSICAL_EVIDENCE_BODY_RESPONSE,
                    HWA_PHYSICAL_QUALITY_FALLBACK |
                        HWA_PHYSICAL_QUALITY_ROOM_CONFOUNDED,
                    error, error_size) != 0) return -1;
            check = &result->checks[result->check_count - 1U];
            hwa_physical_set_available_single(
                check, single->role.side,
                (double)single->mode_count /
                    ((double)single->format.sample_rate_hz / 2000.0),
                0.70);
            return hwa_physical_add_check(
                result, "body", case_id, "",
                HWA_PHYSICAL_BODY_MODE_DISTANCE_CENTS,
                HWA_PHYSICAL_UNIT_CENTS, HWA_PHYSICAL_INSUFFICIENT,
                HWA_PHYSICAL_EVIDENCE_WAVE_SAMPLES |
                    HWA_PHYSICAL_EVIDENCE_SPECTRUM |
                    HWA_PHYSICAL_EVIDENCE_BODY_RESPONSE,
                HWA_PHYSICAL_QUALITY_LOW_SIGNAL |
                    HWA_PHYSICAL_QUALITY_ROOM_CONFOUNDED,
                error, error_size);
        }
        return hwa_physical_add_unavailable_family(
            result, "body", case_id,
            HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ,
            HWA_PHYSICAL_BODY_MODE_DISTANCE_CENTS,
            HWA_PHYSICAL_INSUFFICIENT, evidence,
            HWA_PHYSICAL_QUALITY_LOW_SIGNAL |
                HWA_PHYSICAL_QUALITY_ROOM_CONFOUNDED,
            error, error_size);
    }
    evidence |= HWA_PHYSICAL_EVIDENCE_SPECTRUM;
    while (reference_index < reference->mode_count &&
           model_index < model->mode_count) {
        const HWAPhysicalMode *reference_mode =
            &reference->modes[reference_index];
        const HWAPhysicalMode *model_mode = &model->modes[model_index];
        double difference = model_mode->frequency_hz -
                            reference_mode->frequency_hz;
        double tolerance = fmax(100.0, 0.15 * reference_mode->frequency_hz);
        if (hwa_physical_take_pair_evaluation(
                result, error, error_size) != 0) return -1;
        if (fabs(difference) <= tolerance) {
            double cents = 1200.0 * log2(model_mode->frequency_hz /
                                         reference_mode->frequency_hz);
            if (matched >= (size_t)UINT32_MAX ||
                hwa_physical_add_body_mode_pair(
                    result, case_id, (uint32_t)matched,
                    reference_mode, model_mode, error, error_size) != 0) {
                if (error != NULL && error_size > 0U && error[0] == '\0') {
                    hwa_set_error(error, error_size,
                                  "Stage 5 body mode index overflows");
                }
                return -1;
            }
            cents_sum += (long double)fabs(cents);
            matched++;
            reference_index++;
            model_index++;
        } else if (difference < 0.0) {
            model_index++;
        } else {
            reference_index++;
        }
    }
    if (matched == 0U) {
        if (hwa_physical_add_unavailable_family(
                result, "body", case_id,
                HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ,
                HWA_PHYSICAL_BODY_MODE_DECAY_SECONDS,
                HWA_PHYSICAL_INSUFFICIENT, evidence,
                HWA_PHYSICAL_QUALITY_LOW_COVERAGE |
                    HWA_PHYSICAL_QUALITY_ROOM_CONFOUNDED,
                error, error_size) != 0) return -1;
    }
    if (hwa_physical_add_check(
            result, "body", case_id, "", HWA_PHYSICAL_BODY_MODE_PAN,
            HWA_PHYSICAL_UNIT_RATIO, HWA_PHYSICAL_UNAVAILABLE,
            evidence, HWA_PHYSICAL_QUALITY_FALLBACK |
                          HWA_PHYSICAL_QUALITY_ROOM_CONFOUNDED,
            error, error_size) != 0) return -1;

    if (hwa_physical_add_check(
            result, "body", case_id, "",
            HWA_PHYSICAL_BODY_MODE_DENSITY_PER_KHZ,
            HWA_PHYSICAL_UNIT_COUNT_VALUE, HWA_PHYSICAL_AVAILABLE,
            evidence, HWA_PHYSICAL_QUALITY_FALLBACK |
                          HWA_PHYSICAL_QUALITY_ROOM_CONFOUNDED,
            error, error_size) != 0) return -1;
    check = &result->checks[result->check_count - 1U];
    hwa_physical_set_available_pair(
        check,
        (double)reference->mode_count /
            ((double)reference->format.sample_rate_hz / 2000.0),
        (double)model->mode_count /
            ((double)model->format.sample_rate_hz / 2000.0),
        0.70);

    if (hwa_physical_add_check(
            result, "body", case_id, "",
            HWA_PHYSICAL_BODY_MODE_DISTANCE_CENTS,
            HWA_PHYSICAL_UNIT_CENTS,
            matched > 0U ? HWA_PHYSICAL_AVAILABLE :
                           HWA_PHYSICAL_INSUFFICIENT,
            evidence, matched > 0U
                ? HWA_PHYSICAL_QUALITY_FALLBACK |
                      HWA_PHYSICAL_QUALITY_ROOM_CONFOUNDED
                : HWA_PHYSICAL_QUALITY_LOW_COVERAGE |
                      HWA_PHYSICAL_QUALITY_ROOM_CONFOUNDED,
            error, error_size) != 0) return -1;
    if (matched > 0U) {
        check = &result->checks[result->check_count - 1U];
        check->model_value = (double)(cents_sum / (long double)matched);
        check->model_valid = 1;
        check->confidence = 0.70;
    }
    return 0;
}

static int hwa_physical_add_body_checks(HWAPhysicalCheckSet *result,
                                        HWAProbe *probes,
                                        size_t probe_count,
                                        char *error,
                                        size_t error_size)
{
    size_t index;
    size_t case_count = 0U;
    for (index = 0U; index < probe_count; ++index) {
        const char *case_id;
        HWAProbe *reference;
        HWAProbe *model;
        if (probes[index].role.kind != HWA_PHYSICAL_ROLE_BODY) continue;
        case_id = probes[index].role.case_id;
        if (hwa_physical_case_seen(
                probes, index, HWA_PHYSICAL_ROLE_BODY, case_id)) continue;
        reference = hwa_physical_find_probe(
            probes, probe_count, HWA_PHYSICAL_ROLE_REFERENCE,
            HWA_PHYSICAL_ROLE_BODY, case_id);
        model = hwa_physical_find_probe(
            probes, probe_count, HWA_PHYSICAL_ROLE_MODEL,
            HWA_PHYSICAL_ROLE_BODY, case_id);
        if (hwa_physical_add_body_case(
                result, reference, model, case_id,
                error, error_size) != 0) return -1;
        case_count++;
    }
    if (case_count == 0U) {
        return hwa_physical_add_unavailable_family(
            result, "body", "", HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ,
            HWA_PHYSICAL_BODY_MODE_DISTANCE_CENTS,
            HWA_PHYSICAL_UNAVAILABLE, 0U, 0U, error, error_size);
    }
    return 0;
}

static int hwa_physical_take_pair_evaluations(
    HWAPhysicalCheckSet *result,
    uint64_t count,
    char *error,
    size_t error_size)
{
    if (count > result->options.max_pair_evaluations -
                    (result->pair_evaluations >
                             result->options.max_pair_evaluations
                         ? result->options.max_pair_evaluations
                         : result->pair_evaluations)) {
        hwa_set_error(error, error_size,
                      "Stage 5 pair evaluations exceed their limit");
        return -1;
    }
    result->pair_evaluations += count;
    return 0;
}

typedef struct HWAPhysicalJointFacts {
    double values[9];
    int valid[9];
    uint32_t quality_flags[9];
    int complete;
} HWAPhysicalJointFacts;

static int hwa_physical_probe_clock_equal(const HWAProbe *first,
                                          const HWAProbe *second)
{
    return first->format.sample_rate_hz == second->format.sample_rate_hz &&
           first->frame_count == second->frame_count;
}

static double hwa_physical_spectrum_power_at(const HWAProbe *probe,
                                             double frequency_hz)
{
    double bin_value;
    size_t bin;
    if (!probe->spectrum_valid || frequency_hz < 0.0 ||
        frequency_hz > 0.5 * (double)probe->format.sample_rate_hz) return 0.0;
    bin_value = frequency_hz *
        (double)((probe->spectrum_bins - 1U) * 2U) /
        (double)probe->format.sample_rate_hz;
    if (bin_value < 0.0 || bin_value > (double)(probe->spectrum_bins - 1U)) {
        return 0.0;
    }
    bin = (size_t)llround(bin_value);
    return probe->spectrum[bin];
}

static int hwa_physical_compute_joint_facts(
    HWAPhysicalCheckSet *result,
    const HWAProbe *joint,
    const HWAProbe *isolated_a,
    const HWAProbe *isolated_b,
    HWAPhysicalJointFacts *facts,
    char *error,
    size_t error_size)
{
    uint64_t frame;
    long double residual_power = 0.0L;
    long double joint_power = 0.0L;
    long double sum_power = 0.0L;
    long double joint_sum = 0.0L;
    long double carrier_total = 0.0L;
    long double intermodulation_total = 0.0L;
    double first_frequency;
    double second_frequency;
    double carrier_power;
    double sum_power_bin;
    double difference_power_bin;
    double im_power;
    double ratio;
    size_t fact;

    memset(facts, 0, sizeof(*facts));
    if (joint == NULL || isolated_a == NULL || isolated_b == NULL) return 0;
    facts->complete = 1;
    if (!hwa_physical_probe_clock_equal(joint, isolated_a) ||
        !hwa_physical_probe_clock_equal(joint, isolated_b)) {
        for (fact = 0U; fact < HWA_PHYSICAL_ARRAY_COUNT(facts->values);
             ++fact) {
            facts->quality_flags[fact] |=
                HWA_PHYSICAL_QUALITY_INCOMPATIBLE_CLOCK;
        }
        return 0;
    }
    if (hwa_physical_take_pair_evaluations(
            result, joint->frame_count, error, error_size) != 0) return -1;
    for (frame = 0U; frame < joint->frame_count; ++frame) {
        long double sum = (long double)isolated_a->samples[(size_t)frame] +
                          (long double)isolated_b->samples[(size_t)frame];
        long double actual = (long double)joint->samples[(size_t)frame];
        long double residual = actual - sum;
        if (!hwa_physical_wide_fits_double(sum) ||
            !hwa_physical_wide_fits_double(residual) ||
            !hwa_physical_accumulate_square(
                residual, &residual_power) ||
            !hwa_physical_accumulate_square(actual, &joint_power) ||
            !hwa_physical_accumulate_square(sum, &sum_power) ||
            !hwa_physical_accumulate_product(
                actual, sum, &joint_sum)) {
            hwa_set_error(error, error_size,
                          "Stage 5 joint calculation overflowed");
            return -1;
        }
    }
    if (joint_power > 0.0L) {
        if (!hwa_physical_wide_ratio(
                residual_power, joint_power, &ratio) || ratio < 0.0) {
            hwa_set_error(error, error_size,
                          "Stage 5 joint residual ratio overflowed");
            return -1;
        }
        facts->values[0] = hwa_physical_db(
            sqrt(ratio), -300.0);
        facts->valid[0] = 1;
    }
    if (sum_power > 0.0L) {
        if (!hwa_physical_wide_ratio(
                fabsl(joint_sum), sum_power, &ratio)) {
            hwa_set_error(error, error_size,
                          "Stage 5 joint gain ratio overflowed");
            return -1;
        }
        facts->values[1] = hwa_physical_db(ratio, -300.0);
        facts->valid[1] = 1;
    }
    for (fact = 0U; fact < 2U; ++fact) {
        if (facts->valid[fact] && !isfinite(facts->values[fact])) {
            hwa_set_error(error, error_size,
                          "Stage 5 joint calculation is non-finite");
            return -1;
        }
    }
    if (!joint->spectrum_valid || !isolated_a->spectrum_valid ||
        !isolated_b->spectrum_valid ||
        isolated_a->strongest_frequency_hz <= 0.0 ||
        isolated_b->strongest_frequency_hz <= 0.0) {
        for (fact = 2U; fact < HWA_PHYSICAL_ARRAY_COUNT(facts->values);
             ++fact) {
            facts->quality_flags[fact] |= HWA_PHYSICAL_QUALITY_LOW_SIGNAL;
        }
        return 0;
    }
    for (fact = 2U; fact <= 4U; ++fact) {
        facts->quality_flags[fact] |= HWA_PHYSICAL_QUALITY_FALLBACK;
    }
    for (fact = 5U; fact < HWA_PHYSICAL_ARRAY_COUNT(facts->values);
         ++fact) {
        facts->quality_flags[fact] |= HWA_PHYSICAL_QUALITY_PITCH_CONFOUNDED;
    }
    first_frequency = isolated_a->strongest_frequency_hz;
    second_frequency = isolated_b->strongest_frequency_hz;
    if (!hwa_physical_accumulate_value(
            (long double)hwa_physical_spectrum_power_at(
                joint, first_frequency), &carrier_total) ||
        !hwa_physical_accumulate_value(
            (long double)hwa_physical_spectrum_power_at(
                joint, second_frequency), &carrier_total)) {
        hwa_set_error(error, error_size,
                      "Stage 5 joint carrier power overflowed");
        return -1;
    }
    carrier_power = (double)carrier_total;
    sum_power_bin = hwa_physical_spectrum_power_at(
        joint, first_frequency + second_frequency);
    difference_power_bin = hwa_physical_spectrum_power_at(
        joint, fabs(first_frequency - second_frequency));
    if (!hwa_physical_accumulate_value(
            (long double)hwa_physical_spectrum_power_at(
                joint, fabs(2.0 * first_frequency - second_frequency)),
            &intermodulation_total) ||
        !hwa_physical_accumulate_value(
            (long double)hwa_physical_spectrum_power_at(
                joint, fabs(2.0 * second_frequency - first_frequency)),
            &intermodulation_total)) {
        hwa_set_error(error, error_size,
                      "Stage 5 joint intermodulation power overflowed");
        return -1;
    }
    im_power = (double)intermodulation_total;
    if (carrier_power > 0.0) {
        double sum_ratio;
        double difference_ratio;
        if (!hwa_physical_wide_ratio(
                (long double)im_power, (long double)carrier_power, &ratio) ||
            !hwa_physical_wide_ratio(
                (long double)sum_power_bin, (long double)carrier_power,
                &sum_ratio) ||
            !hwa_physical_wide_ratio(
                (long double)difference_power_bin,
                (long double)carrier_power, &difference_ratio) ||
            ratio < 0.0 || sum_ratio < 0.0 || difference_ratio < 0.0) {
            hwa_set_error(error, error_size,
                          "Stage 5 joint spectral ratio overflowed");
            return -1;
        }
        facts->values[2] = sqrt(ratio);
        facts->values[3] = 10.0 * log10(fmax(sum_ratio, 1.0e-30));
        facts->values[4] = 10.0 * log10(
            fmax(difference_ratio, 1.0e-30));
        facts->valid[2] = 1;
        facts->valid[3] = 1;
        facts->valid[4] = 1;
    }
    /* Beating, roughness, and pitch pull need tracked fundamentals and a
       steady-state span. */
    for (fact = 0U; fact < HWA_PHYSICAL_ARRAY_COUNT(facts->values); ++fact) {
        if (facts->valid[fact] && !isfinite(facts->values[fact])) {
            hwa_set_error(error, error_size,
                          "Stage 5 joint calculation is non-finite");
            return -1;
        }
    }
    return 0;
}

static int hwa_physical_joint_role_kind(HWAPhysicalRoleKind kind)
{
    return kind == HWA_PHYSICAL_ROLE_JOINT ||
           kind == HWA_PHYSICAL_ROLE_ISOLATED_A ||
           kind == HWA_PHYSICAL_ROLE_ISOLATED_B;
}

static int hwa_physical_joint_case_seen(const HWAProbe *probes,
                                        size_t before,
                                        const char *case_id)
{
    size_t index;
    for (index = 0U; index < before; ++index) {
        if (hwa_physical_joint_role_kind(probes[index].role.kind) &&
            strcmp(probes[index].role.case_id, case_id) == 0) return 1;
    }
    return 0;
}

static int hwa_physical_add_joint_case(HWAPhysicalCheckSet *result,
                                       HWAProbe *probes,
                                       size_t probe_count,
                                       const char *case_id,
                                       char *error,
                                       size_t error_size)
{
    static const HWAPhysicalCheckKind kinds[9] = {
        HWA_PHYSICAL_JOINT_RESIDUAL_DB,
        HWA_PHYSICAL_SHARED_GAIN_DB,
        HWA_PHYSICAL_INTERMODULATION_RATIO,
        HWA_PHYSICAL_SUM_TONE_DB,
        HWA_PHYSICAL_DIFFERENCE_TONE_DB,
        HWA_PHYSICAL_BEATING_DEPTH_RATIO,
        HWA_PHYSICAL_BEATING_RATE_HZ,
        HWA_PHYSICAL_ROUGHNESS_RATIO,
        HWA_PHYSICAL_PITCH_PULL_CENTS
    };
    HWAPhysicalJointFacts reference;
    HWAPhysicalJointFacts model;
    HWAProbe *reference_joint = hwa_physical_find_probe(
        probes, probe_count, HWA_PHYSICAL_ROLE_REFERENCE,
        HWA_PHYSICAL_ROLE_JOINT, case_id);
    HWAProbe *reference_a = hwa_physical_find_probe(
        probes, probe_count, HWA_PHYSICAL_ROLE_REFERENCE,
        HWA_PHYSICAL_ROLE_ISOLATED_A, case_id);
    HWAProbe *reference_b = hwa_physical_find_probe(
        probes, probe_count, HWA_PHYSICAL_ROLE_REFERENCE,
        HWA_PHYSICAL_ROLE_ISOLATED_B, case_id);
    HWAProbe *model_joint = hwa_physical_find_probe(
        probes, probe_count, HWA_PHYSICAL_ROLE_MODEL,
        HWA_PHYSICAL_ROLE_JOINT, case_id);
    HWAProbe *model_a = hwa_physical_find_probe(
        probes, probe_count, HWA_PHYSICAL_ROLE_MODEL,
        HWA_PHYSICAL_ROLE_ISOLATED_A, case_id);
    HWAProbe *model_b = hwa_physical_find_probe(
        probes, probe_count, HWA_PHYSICAL_ROLE_MODEL,
        HWA_PHYSICAL_ROLE_ISOLATED_B, case_id);
    size_t fact;
    int computed;

    computed = hwa_physical_compute_joint_facts(
        result, reference_joint, reference_a, reference_b,
        &reference, error, error_size);
    if (computed != 0) return -1;
    computed = hwa_physical_compute_joint_facts(
        result, model_joint, model_a, model_b,
        &model, error, error_size);
    if (computed != 0) return -1;
    if (!reference.complete && !model.complete) {
        return hwa_physical_add_unavailable_family(
            result, "joint", case_id, HWA_PHYSICAL_JOINT_RESIDUAL_DB,
            HWA_PHYSICAL_PITCH_PULL_CENTS, HWA_PHYSICAL_UNAVAILABLE,
            0U, 0U, error, error_size);
    }
    for (fact = 0U; fact < HWA_PHYSICAL_ARRAY_COUNT(kinds); ++fact) {
        HWAPhysicalAvailability availability =
            reference.valid[fact] || model.valid[fact]
                ? HWA_PHYSICAL_AVAILABLE : HWA_PHYSICAL_INSUFFICIENT;
        uint32_t quality = reference.quality_flags[fact] |
                           model.quality_flags[fact];
        uint32_t evidence = HWA_PHYSICAL_EVIDENCE_WAVE_SAMPLES |
                            HWA_PHYSICAL_EVIDENCE_JOINT_SUM;
        HWAPhysicalCheck *check;
        if ((fact >= 2U && fact <= 4U) || fact >= 6U) {
            evidence |= HWA_PHYSICAL_EVIDENCE_SPECTRUM;
        }
        if (hwa_physical_add_check(
                result, "joint", case_id, "", kinds[fact],
                hwa_physical_kind_unit(kinds[fact]), availability,
                evidence, quality, error, error_size) != 0) return -1;
        if (availability == HWA_PHYSICAL_AVAILABLE) {
            check = &result->checks[result->check_count - 1U];
            if (reference.valid[fact] && model.valid[fact]) {
                hwa_physical_set_available_pair(
                    check, reference.values[fact], model.values[fact],
                    fact < 2U ? 1.0 : 0.65);
            } else if (reference.valid[fact]) {
                hwa_physical_set_available_single(
                    check, HWA_PHYSICAL_ROLE_REFERENCE,
                    reference.values[fact], fact < 2U ? 1.0 : 0.65);
            } else {
                hwa_physical_set_available_single(
                    check, HWA_PHYSICAL_ROLE_MODEL,
                    model.values[fact], fact < 2U ? 1.0 : 0.65);
            }
        }
    }
    return 0;
}

static int hwa_physical_add_joint_checks(HWAPhysicalCheckSet *result,
                                         HWAProbe *probes,
                                         size_t probe_count,
                                         char *error,
                                         size_t error_size)
{
    size_t index;
    size_t case_count = 0U;
    for (index = 0U; index < probe_count; ++index) {
        const char *case_id;
        if (!hwa_physical_joint_role_kind(probes[index].role.kind)) continue;
        case_id = probes[index].role.case_id;
        if (hwa_physical_joint_case_seen(probes, index, case_id)) continue;
        if (hwa_physical_add_joint_case(
                result, probes, probe_count, case_id,
                error, error_size) != 0) return -1;
        case_count++;
    }
    if (case_count == 0U) {
        return hwa_physical_add_unavailable_family(
            result, "joint", "", HWA_PHYSICAL_JOINT_RESIDUAL_DB,
            HWA_PHYSICAL_PITCH_PULL_CENTS, HWA_PHYSICAL_UNAVAILABLE,
            0U, 0U, error, error_size);
    }
    return 0;
}

typedef struct HWAPhysicalRenderFacts {
    double values[8];
    int valid[8];
    uint32_t quality_flags[8];
    int complete;
} HWAPhysicalRenderFacts;

static int hwa_physical_probe_attack_decay(const HWAProbe *probe,
                                           double *attack,
                                           double *decay)
{
    uint64_t frame;
    uint64_t first = 0U;
    uint64_t last = 0U;
    double peak = 0.0;
    double threshold;
    int found = 0;
    for (frame = 0U; frame < probe->frame_count; ++frame) {
        double absolute = fabs(probe->samples[(size_t)frame]);
        if (absolute > peak) peak = absolute;
    }
    if (!(peak > 0.0)) return 0;
    threshold = peak * 0.10;
    for (frame = 0U; frame < probe->frame_count; ++frame) {
        if (fabs(probe->samples[(size_t)frame]) >= threshold) {
            if (!found) first = frame;
            last = frame;
            found = 1;
        }
    }
    if (!found) return 0;
    *attack = (double)first / (double)probe->format.sample_rate_hz;
    *decay = (double)(last - first) /
             (double)probe->format.sample_rate_hz;
    return 1;
}

static int hwa_physical_compute_render_facts(
    HWAPhysicalCheckSet *result,
    const HWAProbe *baseline,
    const HWAProbe *variant,
    HWAPhysicalRenderFacts *facts,
    char *error,
    size_t error_size)
{
    size_t delay_count;
    size_t max_lag;
    uint64_t delay_evaluations;
    ptrdiff_t delay = 0;
    double correlation = 0.0;
    size_t baseline_start = 0U;
    size_t variant_start = 0U;
    size_t overlap;
    size_t index;
    long double error_power = 0.0L;
    long double baseline_power = 0.0L;
    double max_error = 0.0;
    double ratio;
    double baseline_attack;
    double baseline_decay;
    double variant_attack;
    double variant_decay;

    memset(facts, 0, sizeof(*facts));
    if (baseline == NULL || variant == NULL) return 0;
    facts->complete = 1;
    if (!hwa_physical_probe_clock_equal(baseline, variant)) {
        for (index = 0U; index < HWA_PHYSICAL_ARRAY_COUNT(facts->values);
             ++index) {
            facts->quality_flags[index] |=
                HWA_PHYSICAL_QUALITY_INCOMPATIBLE_CLOCK;
        }
        return 0;
    }
    if (baseline->frame_count > (uint64_t)SIZE_MAX) return 0;
    delay_count = (size_t)baseline->frame_count;
    if (delay_count > 65536U) delay_count = 65536U;
    max_lag = baseline->format.sample_rate_hz / 100U;
    if (max_lag > 256U) max_lag = 256U;
    if (max_lag > delay_count / 8U) max_lag = delay_count / 8U;
    if (delay_count >= 3U) {
        uint64_t width = (uint64_t)max_lag * 2U + 1U;
        if ((uint64_t)delay_count > UINT64_MAX / width) {
            hwa_set_error(error, error_size,
                          "Stage 5 render delay work overflows");
            return -1;
        }
        delay_evaluations = (uint64_t)delay_count * width;
        if (hwa_physical_take_pair_evaluations(
                result, delay_evaluations, error, error_size) != 0) return -1;
        int delay_status = hwa_dsp_estimate_delay(
            baseline->samples, variant->samples, delay_count, max_lag,
            &delay, &correlation);
        if (delay_status == HWA_DSP_OK) {
            facts->values[2] = correlation;
            facts->values[3] = (double)delay;
            facts->valid[2] = 1;
            facts->valid[3] = 1;
        } else if (delay_status == HWA_DSP_NO_DATA) {
            delay = 0;
            facts->quality_flags[2] |= HWA_PHYSICAL_QUALITY_LOW_SIGNAL;
            facts->quality_flags[3] |= HWA_PHYSICAL_QUALITY_LOW_SIGNAL;
        } else {
            hwa_set_error(error, error_size,
                          "Stage 5 render delay calculation failed");
            return -1;
        }
    }
    if (delay > 0) {
        variant_start = (size_t)delay;
    } else if (delay < 0) {
        baseline_start = (size_t)(-delay);
    }
    overlap = (size_t)baseline->frame_count -
        (baseline_start > variant_start ? baseline_start : variant_start);
    if (hwa_physical_take_pair_evaluations(
            result, (uint64_t)overlap, error, error_size) != 0) return -1;
    for (index = 0U; index < overlap; ++index) {
        long double first =
            (long double)baseline->samples[baseline_start + index];
        long double second =
            (long double)variant->samples[variant_start + index];
        long double difference = second - first;
        double absolute;
        if (!hwa_physical_wide_fits_double(difference) ||
            !hwa_physical_accumulate_square(
                difference, &error_power) ||
            !hwa_physical_accumulate_square(first, &baseline_power)) {
            hwa_set_error(error, error_size,
                          "Stage 5 render calculation overflowed");
            return -1;
        }
        absolute = (double)fabsl(difference);
        if (!isfinite(absolute)) {
            hwa_set_error(error, error_size,
                          "Stage 5 render maximum error overflowed");
            return -1;
        }
        if (absolute > max_error) max_error = absolute;
    }
    if (overlap > 0U) {
        long double denominator = baseline_power > 0.0L
            ? baseline_power : (long double)overlap;
        if (!hwa_physical_wide_ratio(
                error_power, denominator, &ratio) || ratio < 0.0) {
            hwa_set_error(error, error_size,
                          "Stage 5 render error ratio overflowed");
            return -1;
        }
        facts->values[0] = hwa_physical_db(sqrt(ratio), -300.0);
        facts->values[1] = hwa_physical_db(max_error, -300.0);
        facts->valid[0] = 1;
        facts->valid[1] = 1;
    }
    if (baseline->strongest_frequency_hz > 0.0 &&
        variant->strongest_frequency_hz > 0.0) {
        if (!hwa_physical_wide_ratio(
                (long double)variant->strongest_frequency_hz,
                (long double)baseline->strongest_frequency_hz, &ratio) ||
            ratio <= 0.0) {
            hwa_set_error(error, error_size,
                          "Stage 5 render pitch ratio overflowed");
            return -1;
        }
        facts->values[4] = 1200.0 * log2(ratio);
        facts->valid[4] = 1;
    }
    if (hwa_physical_probe_attack_decay(
            baseline, &baseline_attack, &baseline_decay) &&
        hwa_physical_probe_attack_decay(
            variant, &variant_attack, &variant_decay)) {
        facts->values[5] = variant_attack - baseline_attack;
        facts->values[6] = variant_decay - baseline_decay;
        facts->valid[5] = 1;
        facts->valid[6] = 1;
    }
    if (baseline->spectrum_valid && variant->spectrum_valid &&
        baseline->spectrum_bins == variant->spectrum_bins) {
        long double squared = 0.0L;
        size_t valid_bins = 0U;
        if (hwa_physical_take_pair_evaluations(
                result, (uint64_t)baseline->spectrum_bins,
                error, error_size) != 0) return -1;
        for (index = 1U; index < baseline->spectrum_bins; ++index) {
            double first = baseline->spectrum[index];
            double second = variant->spectrum[index];
            if (first > 0.0 && second > 0.0) {
                double difference;
                if (!hwa_physical_wide_ratio(
                        (long double)second, (long double)first, &ratio) ||
                    ratio <= 0.0) {
                    hwa_set_error(error, error_size,
                                  "Stage 5 render spectrum ratio overflowed");
                    return -1;
                }
                difference = 10.0 * log10(ratio);
                if (!isfinite(difference) ||
                    !hwa_physical_accumulate_square(
                        (long double)difference, &squared)) {
                    hwa_set_error(error, error_size,
                                  "Stage 5 render spectrum calculation overflowed");
                    return -1;
                }
                valid_bins++;
            }
        }
        if (valid_bins > 0U) {
            if (!hwa_physical_wide_ratio(
                    squared, (long double)valid_bins, &ratio) ||
                ratio < 0.0) {
                hwa_set_error(error, error_size,
                              "Stage 5 render spectrum result overflowed");
                return -1;
            }
            facts->values[7] = sqrt(ratio);
            facts->valid[7] = 1;
        }
    }
    for (index = 0U; index < HWA_PHYSICAL_ARRAY_COUNT(facts->values);
         ++index) {
        if (facts->valid[index] && !isfinite(facts->values[index])) {
            hwa_set_error(error, error_size,
                          "Stage 5 render calculation is non-finite");
            return -1;
        }
    }
    facts->quality_flags[4] |= HWA_PHYSICAL_QUALITY_FALLBACK;
    return 0;
}

static int hwa_physical_render_role_kind(HWAPhysicalRoleKind kind)
{
    return kind == HWA_PHYSICAL_ROLE_RENDER_BASELINE ||
           kind == HWA_PHYSICAL_ROLE_RENDER_VARIANT;
}

static int hwa_physical_render_case_seen(const HWAProbe *probes,
                                         size_t before,
                                         const char *case_id)
{
    size_t index;
    for (index = 0U; index < before; ++index) {
        if (hwa_physical_render_role_kind(probes[index].role.kind) &&
            strcmp(probes[index].role.case_id, case_id) == 0) return 1;
    }
    return 0;
}

static int hwa_physical_add_render_case(HWAPhysicalCheckSet *result,
                                        HWAProbe *probes,
                                        size_t probe_count,
                                        const char *case_id,
                                        char *error,
                                        size_t error_size)
{
    static const HWAPhysicalCheckKind kinds[8] = {
        HWA_PHYSICAL_RENDER_RMS_ERROR_DB,
        HWA_PHYSICAL_RENDER_MAX_ERROR_DBFS,
        HWA_PHYSICAL_RENDER_CORRELATION,
        HWA_PHYSICAL_RENDER_LAG_SAMPLES,
        HWA_PHYSICAL_RENDER_PITCH_DELTA_CENTS,
        HWA_PHYSICAL_RENDER_ATTACK_DELTA_SECONDS,
        HWA_PHYSICAL_RENDER_DECAY_DELTA_SECONDS,
        HWA_PHYSICAL_RENDER_SPECTRAL_DISTANCE_DB
    };
    HWAProbe *reference_baseline = hwa_physical_find_probe(
        probes, probe_count, HWA_PHYSICAL_ROLE_REFERENCE,
        HWA_PHYSICAL_ROLE_RENDER_BASELINE, case_id);
    HWAProbe *reference_variant = hwa_physical_find_probe(
        probes, probe_count, HWA_PHYSICAL_ROLE_REFERENCE,
        HWA_PHYSICAL_ROLE_RENDER_VARIANT, case_id);
    HWAProbe *model_baseline = hwa_physical_find_probe(
        probes, probe_count, HWA_PHYSICAL_ROLE_MODEL,
        HWA_PHYSICAL_ROLE_RENDER_BASELINE, case_id);
    HWAProbe *model_variant = hwa_physical_find_probe(
        probes, probe_count, HWA_PHYSICAL_ROLE_MODEL,
        HWA_PHYSICAL_ROLE_RENDER_VARIANT, case_id);
    HWAPhysicalRenderFacts reference;
    HWAPhysicalRenderFacts model;
    size_t fact;
    int computed;

    computed = hwa_physical_compute_render_facts(
        result, reference_baseline, reference_variant,
        &reference, error, error_size);
    if (computed != 0) return -1;
    computed = hwa_physical_compute_render_facts(
        result, model_baseline, model_variant,
        &model, error, error_size);
    if (computed != 0) return -1;
    if (!reference.complete && !model.complete) {
        return hwa_physical_add_unavailable_family(
            result, "render", case_id,
            HWA_PHYSICAL_RENDER_RMS_ERROR_DB,
            HWA_PHYSICAL_RENDER_SPECTRAL_DISTANCE_DB,
            HWA_PHYSICAL_UNAVAILABLE, 0U, 0U, error, error_size);
    }
    for (fact = 0U; fact < HWA_PHYSICAL_ARRAY_COUNT(kinds); ++fact) {
        HWAPhysicalAvailability availability =
            reference.valid[fact] || model.valid[fact]
                ? HWA_PHYSICAL_AVAILABLE : HWA_PHYSICAL_INSUFFICIENT;
        uint32_t evidence = HWA_PHYSICAL_EVIDENCE_WAVE_SAMPLES |
                            HWA_PHYSICAL_EVIDENCE_RENDER_PAIR;
        HWAPhysicalCheck *check;
        if (fact == 4U || fact == 7U) {
            evidence |= HWA_PHYSICAL_EVIDENCE_SPECTRUM;
        }
        if (fact == 5U || fact == 6U) {
            evidence |= HWA_PHYSICAL_EVIDENCE_ENVELOPE;
        }
        if (hwa_physical_add_check(
                result, "render", case_id, "", kinds[fact],
                hwa_physical_kind_unit(kinds[fact]), availability,
                evidence, reference.quality_flags[fact] |
                              model.quality_flags[fact],
                error, error_size) != 0) return -1;
        if (availability == HWA_PHYSICAL_AVAILABLE) {
            check = &result->checks[result->check_count - 1U];
            if (reference.valid[fact] && model.valid[fact]) {
                hwa_physical_set_available_pair(
                    check, reference.values[fact], model.values[fact],
                    fact < 4U ? 0.90 : 0.70);
            } else if (reference.valid[fact]) {
                hwa_physical_set_available_single(
                    check, HWA_PHYSICAL_ROLE_REFERENCE,
                    reference.values[fact], fact < 4U ? 0.90 : 0.70);
            } else {
                hwa_physical_set_available_single(
                    check, HWA_PHYSICAL_ROLE_MODEL,
                    model.values[fact], fact < 4U ? 0.90 : 0.70);
            }
        }
    }
    return 0;
}

static int hwa_physical_add_render_checks(HWAPhysicalCheckSet *result,
                                          HWAProbe *probes,
                                          size_t probe_count,
                                          char *error,
                                          size_t error_size)
{
    size_t index;
    size_t case_count = 0U;
    for (index = 0U; index < probe_count; ++index) {
        const char *case_id;
        if (!hwa_physical_render_role_kind(probes[index].role.kind)) continue;
        case_id = probes[index].role.case_id;
        if (hwa_physical_render_case_seen(probes, index, case_id)) continue;
        if (hwa_physical_add_render_case(
                result, probes, probe_count, case_id,
                error, error_size) != 0) return -1;
        case_count++;
    }
    if (case_count == 0U) {
        return hwa_physical_add_unavailable_family(
            result, "render", "", HWA_PHYSICAL_RENDER_RMS_ERROR_DB,
            HWA_PHYSICAL_RENDER_SPECTRAL_DISTANCE_DB,
            HWA_PHYSICAL_UNAVAILABLE, 0U, 0U, error, error_size);
    }
    return 0;
}

static int hwa_physical_add_finding(HWAPhysicalCheckSet *result,
                                    HWAPhysicalFindingClass finding_class,
                                    HWAPhysicalSeverity severity,
                                    const char *code,
                                    const char *message,
                                    uint64_t check_id,
                                    int check_id_valid,
                                    double score,
                                    int score_valid,
                                    char *error,
                                    size_t error_size)
{
    HWAPhysicalFinding *grown;
    HWAPhysicalFinding *finding;
    uint64_t bytes = sizeof(*result->findings);
    if (result->finding_count >= result->options.max_findings ||
        result->finding_count == SIZE_MAX / sizeof(*result->findings) ||
        hwa_physical_add_work(result, bytes, error, error_size) != 0) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "Stage 5 finding count exceeds its limit");
        }
        return -1;
    }
    grown = (HWAPhysicalFinding *)realloc(
        result->findings,
        (result->finding_count + 1U) * sizeof(*result->findings));
    if (grown == NULL) {
        result->retained_work_bytes -= bytes;
        hwa_set_error(error, error_size, "cannot allocate Stage 5 findings");
        return -1;
    }
    result->findings = grown;
    finding = &result->findings[result->finding_count];
    memset(finding, 0, sizeof(*finding));
    finding->id = (uint64_t)result->finding_count + 1U;
    finding->finding_class = finding_class;
    finding->severity = severity;
    finding->check_id = check_id;
    finding->check_id_valid = check_id_valid;
    finding->score = score_valid ? fmin(1.0, fmax(0.0, score)) : 0.0;
    finding->score_valid = score_valid;
    finding->code = hwa_physical_copy_cstring(
        result, code, error, error_size);
    finding->message = hwa_physical_copy_cstring(
        result, message, error, error_size);
    if (finding->code == NULL || finding->message == NULL) {
        free(finding->code);
        free(finding->message);
        memset(finding, 0, sizeof(*finding));
        result->retained_work_bytes -= bytes;
        return -1;
    }
    result->finding_count++;
    return 0;
}

static int hwa_physical_finding_order(const void *left, const void *right)
{
    const HWAPhysicalFinding *first = (const HWAPhysicalFinding *)left;
    const HWAPhysicalFinding *second = (const HWAPhysicalFinding *)right;
    if (first->score_valid != second->score_valid) {
        return first->score_valid ? -1 : 1;
    }
    if (first->score_valid && first->score != second->score) {
        return first->score > second->score ? -1 : 1;
    }
    if (first->check_id_valid != second->check_id_valid) {
        return first->check_id_valid ? -1 : 1;
    }
    if (first->check_id < second->check_id) return -1;
    if (first->check_id > second->check_id) return 1;
    return 0;
}

static int hwa_physical_check_order(const void *left, const void *right)
{
    return hwa_physical_check_canonical_compare(
        (const HWAPhysicalCheck *)left,
        (const HWAPhysicalCheck *)right);
}

static void hwa_physical_sort_checks(HWAPhysicalCheckSet *result)
{
    size_t index;
    if (result->check_count > 1U) {
        qsort(result->checks, result->check_count,
              sizeof(*result->checks), hwa_physical_check_order);
    }
    for (index = 0U; index < result->check_count; ++index) {
        result->checks[index].id = (uint64_t)index + 1U;
    }
}

static int hwa_physical_scope_case_has_available(
    const HWAPhysicalCheckSet *result,
    const HWAPhysicalCheck *target)
{
    size_t index;
    for (index = 0U; index < result->check_count; ++index) {
        const HWAPhysicalCheck *check = &result->checks[index];
        if (strcmp(check->scope, target->scope) == 0 &&
            strcmp(check->case_id, target->case_id) == 0 &&
            check->availability == HWA_PHYSICAL_AVAILABLE) return 1;
    }
    return 0;
}

static int hwa_physical_scope_case_unavailable_seen(
    const HWAPhysicalCheckSet *result,
    size_t before,
    const HWAPhysicalCheck *target)
{
    size_t index;
    for (index = 0U; index < before; ++index) {
        const HWAPhysicalCheck *check = &result->checks[index];
        if (strcmp(check->scope, target->scope) == 0 &&
            strcmp(check->case_id, target->case_id) == 0 &&
            check->availability != HWA_PHYSICAL_AVAILABLE) return 1;
    }
    return 0;
}

static int hwa_physical_build_findings(HWAPhysicalCheckSet *result,
                                       char *error,
                                       size_t error_size)
{
    size_t index;
    size_t ranked = 0U;
    for (index = 0U; index < result->check_count; ++index) {
        const HWAPhysicalCheck *check = &result->checks[index];
        double score;
        HWAPhysicalFindingClass finding_class;
        HWAPhysicalSeverity severity;
        const char *code;
        const char *message;
        if (hwa_physical_scored_finding_for_check(
                check, &finding_class, &severity, &code, &message,
                &score)) {
            if (hwa_physical_add_finding(
                    result, finding_class, severity, code, message,
                    check->id, 1, score, 1, error, error_size) != 0) return -1;
        } else if ((strcmp(check->scope, "body") == 0 ||
                    strcmp(check->scope, "joint") == 0 ||
                    strcmp(check->scope, "render") == 0) &&
                   check->availability != HWA_PHYSICAL_AVAILABLE &&
                   !hwa_physical_scope_case_has_available(result, check) &&
                   !hwa_physical_scope_case_unavailable_seen(
                       result, index, check)) {
            if (hwa_physical_add_finding(
                    result, HWA_PHYSICAL_FINDING_UNAVAILABLE,
                    HWA_PHYSICAL_SEVERITY_INFO,
                    "missing-physical-evidence",
                    "This physical check family has no usable bound evidence.",
                    check->id, 1, 0.0, 0, error, error_size) != 0) return -1;
        }
    }
    if (result->finding_count > 1U) {
        qsort(result->findings, result->finding_count,
              sizeof(*result->findings), hwa_physical_finding_order);
    }
    for (index = 0U; index < result->finding_count; ++index) {
        result->findings[index].id = (uint64_t)index + 1U;
        if (result->findings[index].score_valid) {
            ranked++;
            result->findings[index].rank = ranked;
        } else {
            result->findings[index].rank = 0U;
        }
    }
    return 0;
}

static int hwa_physical_verify_unchanged(
    const char *path,
    const HWAFileIdentity *before_identity,
    const char before_hash[HWA_SHA256_HEX_SIZE],
    uint64_t max_bytes,
    uint64_t live_work_bytes,
    uint64_t max_work_bytes,
    char *error,
    size_t error_size)
{
    return hwa_physical_verify_identity_digest(
        path, before_identity, before_hash, max_bytes,
        live_work_bytes, max_work_bytes, error, error_size);
}

static int hwa_physical_verify_named_identity(
    const char *path,
    const HWAFileIdentity *before_identity,
    char *error,
    size_t error_size)
{
    HWAFileIdentity after_identity;
    if (hwa_physical_file_identity(
            path, &after_identity, error, error_size) != 0 ||
        !hwa_physical_identity_unchanged(
            before_identity, &after_identity)) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "Stage 5 input changed during analysis: %s", path);
        }
        return -1;
    }
    return 0;
}

static size_t hwa_physical_ordered_input_index(
    const HWAPhysicalInput *inputs,
    size_t input_count,
    size_t rank)
{
    size_t candidate;
    for (candidate = 0U; candidate < input_count; ++candidate) {
        size_t other;
        size_t before = 0U;
        for (other = 0U; other < input_count; ++other) {
            if (strcmp(inputs[other].role, inputs[candidate].role) < 0) {
                before++;
            }
        }
        if (before == rank) return candidate;
    }
    return input_count;
}

static int hwa_physical_check_files_impl(
    const char *reference_measures_path,
    const char *model_measures_path,
    const HWAPhysicalInput *inputs,
    size_t input_count,
    const HWAPhysicalOptions *options,
    HWAPhysicalCheckSet *result,
    char *error,
    size_t error_size)
{
    HWAPhysicalOptions copied_options;
    HWAMeasurementSet reference;
    HWAMeasurementSet model;
    HWAProfileComparisonOptions read_limits;
    HWAProbe *probes = NULL;
    char reference_hash[HWA_SHA256_HEX_SIZE];
    char model_hash[HWA_SHA256_HEX_SIZE];
    HWAFileIdentity reference_identity;
    HWAFileIdentity model_identity;
    uint64_t active_probe_bytes = 0U;
    uint64_t command_work_limit;
    uint64_t profile_live_bytes = 0U;
    size_t input_index;
    int status = -1;

    if (result == NULL) {
        hwa_set_error(error, error_size, "missing Stage 5 result");
        return -1;
    }
    if (error != NULL && error_size > 0U) error[0] = '\0';
    if (options == NULL) {
        hwa_physical_options_default(&copied_options);
    } else {
        copied_options = *options;
    }
    memset(result, 0, sizeof(*result));
    result->options = copied_options;
    command_work_limit = copied_options.max_work_bytes;
    memset(&reference, 0, sizeof(reference));
    memset(&model, 0, sizeof(model));
    if (reference_measures_path == NULL ||
        reference_measures_path[0] == '\0' || model_measures_path == NULL ||
        model_measures_path[0] == '\0' ||
        (input_count > 0U && inputs == NULL) ||
        !hwa_physical_options_valid(&copied_options, error, error_size) ||
        input_count > copied_options.max_bindings ||
        input_count > SIZE_MAX - 2U) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "invalid Stage 5 paths or binding count");
        }
        goto cleanup;
    }
    if (hwa_physical_file_identity(
            reference_measures_path, &reference_identity,
            error, error_size) != 0 ||
        hwa_physical_file_identity(
            model_measures_path, &model_identity,
            error, error_size) != 0 ||
        strcmp(reference_measures_path, model_measures_path) == 0 ||
        hwa_physical_same_identity(
            &reference_identity, &model_identity)) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "reference and model profiles name the same file");
        }
        goto cleanup;
    }
    for (input_index = 0U; input_index < input_count; ++input_index) {
        HWAPhysicalRole parsed;
        HWAFileIdentity input_identity;
        size_t previous;
        if (inputs[input_index].role == NULL ||
            inputs[input_index].path == NULL ||
            inputs[input_index].path[0] == '\0' ||
            strcmp(inputs[input_index].path, "-") == 0 ||
            hwa_physical_role_parse(inputs[input_index].role, &parsed) != 0) {
            hwa_set_error(error, error_size,
                          "invalid Stage 5 binding at index %zu",
                          input_index);
            goto cleanup;
        }
        if (hwa_physical_file_identity(
                inputs[input_index].path, &input_identity,
                error, error_size) != 0 ||
            strcmp(inputs[input_index].path, reference_measures_path) == 0 ||
            strcmp(inputs[input_index].path, model_measures_path) == 0 ||
            hwa_physical_same_identity(&input_identity,
                                       &reference_identity) ||
            hwa_physical_same_identity(&input_identity, &model_identity)) {
            if (error != NULL && error_size > 0U && error[0] == '\0') {
                hwa_set_error(error, error_size,
                              "Stage 5 binding reuses a profile file");
            }
            goto cleanup;
        }
        for (previous = 0U; previous < input_index; ++previous) {
            HWAFileIdentity previous_identity;
            if (strcmp(inputs[previous].role, inputs[input_index].role) == 0) {
                hwa_set_error(error, error_size,
                              "duplicate Stage 5 binding role '%s'",
                              inputs[input_index].role);
                goto cleanup;
            }
            if (strcmp(inputs[previous].path, inputs[input_index].path) == 0 ||
                hwa_physical_file_identity(
                    inputs[previous].path, &previous_identity,
                    error, error_size) != 0 ||
                hwa_physical_same_identity(
                    &input_identity, &previous_identity)) {
                if (error != NULL && error_size > 0U && error[0] == '\0') {
                    hwa_set_error(error, error_size,
                                  "Stage 5 bindings name the same file");
                }
                goto cleanup;
            }
        }
    }
    result->reference_measures_path = hwa_physical_copy_cstring(
        result, reference_measures_path, error, error_size);
    result->model_measures_path = hwa_physical_copy_cstring(
        result, model_measures_path, error, error_size);
    if (result->reference_measures_path == NULL ||
        result->model_measures_path == NULL) {
        goto cleanup;
    }
    read_limits = copied_options.profile_limits;
    if (result->retained_work_bytes >= command_work_limit) {
        hwa_set_error(error, error_size,
                      "Stage 5 profile paths consume the work limit");
        goto cleanup;
    }
    if (read_limits.max_work_bytes >
        command_work_limit - result->retained_work_bytes) {
        read_limits.max_work_bytes =
            command_work_limit - result->retained_work_bytes;
    }
    if (read_limits.max_work_bytes == 0U ||
        hwa_measure_file_read(
            reference_measures_path, &read_limits,
            &reference, reference_hash, error, error_size) != 0) {
        goto cleanup;
    }
    if (reference.retained_work_bytes > command_work_limit -
            result->retained_work_bytes) {
        hwa_set_error(error, error_size,
                      "reference profile exceeds Stage 5 work limit");
        goto cleanup;
    }
    profile_live_bytes = reference.retained_work_bytes;
    if (hwa_physical_verify_identity_digest(
            reference_measures_path, &reference_identity, reference_hash,
            copied_options.profile_limits.max_input_bytes,
            result->retained_work_bytes + profile_live_bytes,
            command_work_limit, error, error_size) != 0) {
        goto cleanup;
    }
    read_limits = copied_options.profile_limits;
    if (profile_live_bytes >= command_work_limit -
                                  result->retained_work_bytes) {
        hwa_set_error(error, error_size,
                      "reference profile leaves no model work space");
        goto cleanup;
    }
    if (read_limits.max_work_bytes >
        command_work_limit - result->retained_work_bytes -
            profile_live_bytes) {
        read_limits.max_work_bytes =
            command_work_limit - result->retained_work_bytes -
            profile_live_bytes;
    }
    if (read_limits.max_work_bytes == 0U ||
        hwa_measure_file_read(
            model_measures_path, &read_limits,
            &model, model_hash, error, error_size) != 0) {
        goto cleanup;
    }
    if (model.retained_work_bytes > UINT64_MAX - profile_live_bytes) {
        hwa_set_error(error, error_size,
                      "Stage 5 profile work overflows");
        goto cleanup;
    }
    profile_live_bytes += model.retained_work_bytes;
    if (profile_live_bytes > command_work_limit -
                                 result->retained_work_bytes) {
        hwa_set_error(error, error_size,
                      "profiles exceed Stage 5 work limit");
        goto cleanup;
    }
    if (hwa_physical_verify_identity_digest(
            model_measures_path, &model_identity, model_hash,
            copied_options.profile_limits.max_input_bytes,
            result->retained_work_bytes + profile_live_bytes,
            command_work_limit, error, error_size) != 0) {
        goto cleanup;
    }
    if (strcmp(reference_hash, model_hash) == 0) {
        hwa_set_error(error, error_size,
                      "reference and model profiles have identical bytes");
        goto cleanup;
    }
    result->options.max_work_bytes = command_work_limit - profile_live_bytes;
    if (result->options.max_work_bytes < result->retained_work_bytes ||
        hwa_physical_allocate_sources(
            result, input_count + 2U, error, error_size) != 0 ||
        hwa_physical_set_profile_source(
            result, 0U, "reference:profile", reference_measures_path,
            reference_hash, error, error_size) != 0 ||
        hwa_physical_set_profile_source(
            result, 1U, "model:profile", model_measures_path,
            model_hash, error, error_size) != 0) {
        goto cleanup;
    }
    memcpy(result->reference_measures_sha256, reference_hash,
           HWA_SHA256_HEX_SIZE);
    memcpy(result->model_measures_sha256, model_hash,
           HWA_SHA256_HEX_SIZE);

    if (hwa_physical_add_element_trait_checks(
            result, &reference, &model, error, error_size) != 0 ||
        hwa_physical_add_carryover_check(
            result, &reference, &model, error, error_size) != 0) {
        goto cleanup;
    }
    result->options.max_work_bytes = command_work_limit;
    hwa_measurement_set_free(&model);
    hwa_measurement_set_free(&reference);
    profile_live_bytes = 0U;

    if (input_count > 0U) {
        uint64_t bytes;
        if (input_count > SIZE_MAX / sizeof(*probes)) {
            hwa_set_error(error, error_size,
                          "Stage 5 probe count overflows");
            goto cleanup;
        }
        bytes = (uint64_t)input_count * sizeof(*probes);
        if (!hwa_physical_live_work_fits(result, bytes)) {
            hwa_set_error(error, error_size,
                          "Stage 5 probe list exceeds its byte limit");
            goto cleanup;
        }
        probes = (HWAProbe *)calloc(input_count, sizeof(*probes));
        if (probes == NULL) {
            hwa_set_error(error, error_size,
                          "cannot allocate Stage 5 probe list");
            goto cleanup;
        }
        active_probe_bytes = bytes;
        for (input_index = 0U; input_index < input_count; ++input_index) {
            size_t ordered_index = hwa_physical_ordered_input_index(
                inputs, input_count, input_index);
            HWAPhysicalSource *source = &result->sources[input_index + 2U];
            source->id = (uint64_t)input_index + 3U;
            if (ordered_index >= input_count ||
                hwa_physical_probe_read(
                    result, &inputs[ordered_index], &probes[input_index],
                    source, active_probe_bytes,
                    error, error_size) != 0 ||
                active_probe_bytes > UINT64_MAX -
                                         probes[input_index].work_bytes) {
                if (error != NULL && error_size > 0U && error[0] == '\0') {
                    hwa_set_error(error, error_size,
                                  "Stage 5 probe work overflows");
                }
                goto cleanup;
            }
            active_probe_bytes += probes[input_index].work_bytes;
        }
    }

    if (active_probe_bytes > command_work_limit ||
        result->retained_work_bytes >
            command_work_limit - active_probe_bytes) {
        hwa_set_error(error, error_size,
                      "Stage 5 probes leave no result work space");
        goto cleanup;
    }
    result->options.max_work_bytes = command_work_limit - active_probe_bytes;
    for (input_index = 0U; input_index < input_count; ++input_index) {
        if (hwa_physical_add_scan_checks(
                result, &probes[input_index],
                result->sources[input_index + 2U].role,
                error, error_size) != 0) {
            goto cleanup;
        }
    }
    if (hwa_physical_add_body_checks(
            result, probes, input_count, error, error_size) != 0 ||
        hwa_physical_add_joint_checks(
            result, probes, input_count, error, error_size) != 0 ||
        hwa_physical_add_render_checks(
            result, probes, input_count, error, error_size) != 0) {
        goto cleanup;
    }
    hwa_physical_sort_checks(result);
    if (hwa_physical_build_findings(result, error, error_size) != 0) {
        goto cleanup;
    }
    if (hwa_physical_verify_unchanged(
            reference_measures_path, &reference_identity, reference_hash,
            copied_options.profile_limits.max_input_bytes,
            result->retained_work_bytes + active_probe_bytes,
            command_work_limit,
            error, error_size) != 0 ||
        hwa_physical_verify_unchanged(
            model_measures_path, &model_identity, model_hash,
            copied_options.profile_limits.max_input_bytes,
            result->retained_work_bytes + active_probe_bytes,
            command_work_limit,
            error, error_size) != 0) {
        goto cleanup;
    }
    for (input_index = 0U; input_index < input_count; ++input_index) {
        if (hwa_physical_verify_named_identity(
                result->sources[input_index + 2U].path,
                &probes[input_index].identity,
                error, error_size) != 0) goto cleanup;
    }
    result->options.max_work_bytes = command_work_limit;
    if (hwa_physical_check_set_validate(result, error, error_size) != 0) {
        goto cleanup;
    }
    status = 0;

cleanup:
    if (probes != NULL) {
        for (input_index = 0U; input_index < input_count; ++input_index) {
            hwa_physical_probe_free(&probes[input_index]);
        }
    }
    free(probes);
    result->options.max_work_bytes = command_work_limit;
    hwa_measurement_set_free(&model);
    hwa_measurement_set_free(&reference);
    if (status != 0) {
        hwa_physical_check_set_free(result);
    } else if (error != NULL && error_size > 0U) {
        error[0] = '\0';
    }
    return status;
}

int hwa_check_physical_files(
    const char *reference_measures_path,
    const char *model_measures_path,
    const HWAPhysicalInput *inputs,
    size_t input_count,
    const HWAPhysicalOptions *options,
    HWAPhysicalCheckSet *result,
    char *error,
    size_t error_size)
{
    HWAPhysicalOptions copied_options;
    const HWAPhysicalOptions *safe_options = options;
    HWANumericLocale locale;
    int status;
    int locale_status;

    if (result == NULL) {
        hwa_set_error(error, error_size, "missing Stage 5 result");
        return -1;
    }
    if (options != NULL) {
        copied_options = *options;
        safe_options = &copied_options;
    }
    if (error != NULL && error_size > 0U) error[0] = '\0';
    memset(result, 0, sizeof(*result));
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_set_error(error, error_size,
                      "cannot enter the C numeric locale for Stage 5");
        return -1;
    }
    status = hwa_physical_check_files_impl(
        reference_measures_path, model_measures_path, inputs, input_count,
        safe_options, result, error, error_size);
    locale_status = hwa_c_numeric_locale_end(&locale);
    if (locale_status != 0) {
        if (status == 0) hwa_physical_check_set_free(result);
        if (error != NULL && error_size > 0U &&
            (status == 0 || error[0] == '\0')) {
            hwa_set_error(error, error_size,
                          "cannot restore the numeric locale after Stage 5");
        }
        return -1;
    }
    return status;
}

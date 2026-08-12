#if !defined(_WIN32)
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "hlolli_wg_analyzer.h"
#include "internal.h"
#include "measure_file.h"
#include "numeric_locale.h"
#include "production.h"
#include "production_file.h"
#include "sha256.h"

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

typedef struct HWAProductionName {
    int value;
    const char *name;
} HWAProductionName;

static const HWAProductionName hwa_production_availability_names[] = {
    {HWA_PRODUCTION_AVAILABLE, "available"},
    {HWA_PRODUCTION_UNAVAILABLE, "unavailable"},
    {HWA_PRODUCTION_INSUFFICIENT, "insufficient"}
};

static const HWAProductionName hwa_production_split_names[] = {
    {HWA_PRODUCTION_TRAIN, "train"},
    {HWA_PRODUCTION_CHECK, "check"}
};

static const HWAProductionName hwa_production_view_names[] = {
    {HWA_PRODUCTION_VIEW_RAW, "raw"},
    {HWA_PRODUCTION_VIEW_DRY_LIKE, "dry-like"},
    {HWA_PRODUCTION_VIEW_ROOM_MATCHED, "room-matched"}
};

static const HWAProductionName hwa_production_scope_names[] = {
    {HWA_PRODUCTION_SCOPE_CORRECTION, "correction"},
    {HWA_PRODUCTION_SCOPE_REFERENCE, "reference"},
    {HWA_PRODUCTION_SCOPE_MODEL, "model"},
    {HWA_PRODUCTION_SCOPE_ROOM_IR, "room-ir"}
};

static const HWAProductionName hwa_production_unit_names[] = {
    {HWA_PRODUCTION_UNIT_DBFS, "dBFS"},
    {HWA_PRODUCTION_UNIT_DB, "dB"},
    {HWA_PRODUCTION_UNIT_RATIO, "ratio"},
    {HWA_PRODUCTION_UNIT_SECONDS, "seconds"},
    {HWA_PRODUCTION_UNIT_SAMPLES, "samples"}
};

static const HWAProductionName hwa_production_fit_names[] = {
    {HWA_PRODUCTION_FIT_EQ_GAIN_DB, "eq_gain_db"},
    {HWA_PRODUCTION_FIT_THRESHOLD_DBFS, "threshold_dbfs"},
    {HWA_PRODUCTION_FIT_RATIO, "ratio"},
    {HWA_PRODUCTION_FIT_MAKEUP_DB, "makeup_db"},
    {HWA_PRODUCTION_FIT_STEREO_DELAY_SAMPLES, "stereo_delay_samples"},
    {HWA_PRODUCTION_FIT_CHANNEL_POLARITY, "channel_polarity"},
    {HWA_PRODUCTION_FIT_CHANNEL_BALANCE_DB, "channel_balance_db"},
    {HWA_PRODUCTION_FIT_STEREO_WIDTH_RATIO, "stereo_width_ratio"},
    {HWA_PRODUCTION_FIT_STEREO_CORRELATION, "stereo_correlation"},
    {HWA_PRODUCTION_FIT_EARLY_REFLECTION_DB, "early_reflection_db"},
    {HWA_PRODUCTION_FIT_LATE_DECAY_SECONDS, "late_decay_seconds"}
};

static const HWAProductionName hwa_production_metric_names[] = {
    {HWA_PRODUCTION_METRIC_RMS_DBFS, "rms_dbfs"},
    {HWA_PRODUCTION_METRIC_CREST_DB, "crest_db"},
    {HWA_PRODUCTION_METRIC_BAND_LEVEL_DBFS, "band_level_dbfs"},
    {HWA_PRODUCTION_METRIC_LEVEL_SPREAD_DB, "level_spread_db"},
    {HWA_PRODUCTION_METRIC_CHANNEL_BALANCE_DB, "channel_balance_db"},
    {HWA_PRODUCTION_METRIC_STEREO_WIDTH_RATIO, "stereo_width_ratio"},
    {HWA_PRODUCTION_METRIC_STEREO_CORRELATION, "stereo_correlation"},
    {HWA_PRODUCTION_METRIC_STEREO_DELAY_SAMPLES, "stereo_delay_samples"},
    {HWA_PRODUCTION_METRIC_EARLY_REFLECTION_DB, "early_reflection_db"},
    {HWA_PRODUCTION_METRIC_LATE_DECAY_SECONDS, "late_decay_seconds"}
};

#define HWA_PRODUCTION_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define HWA_PRODUCTION_METRICS_PER_VIEW \
    (7U + HWA_BAND_COUNT + 2U * HWA_PRODUCTION_ROOM_BAND_COUNT)
#define HWA_PRODUCTION_EVALUATIONS_PER_SPAN \
    ((HWA_PRODUCTION_VIEW_COUNT - 1U) * HWA_PRODUCTION_METRICS_PER_VIEW)
#define HWA_PRODUCTION_DELAY_MILLISECONDS 2U
#define HWA_PRODUCTION_MIN_TRAIN_SPANS 8U
#define HWA_PRODUCTION_MIN_CHECK_SPANS 2U
#define HWA_PRODUCTION_MAX_TRAIN_PER_FAMILY 64U
#define HWA_PRODUCTION_MAX_CHECK_PER_FAMILY 16U

typedef struct HWAProductionContextRef {
    const HWAMeasureItemContext *context;
} HWAProductionContextRef;

typedef struct HWAProductionMatch {
    const HWAMeasureItemContext *reference;
    const HWAMeasureItemContext *model;
    HWAProductionSplit split;
    uint32_t eligibility_flags;
    uint32_t selected_flags;
} HWAProductionMatch;

typedef struct HWAProductionRankedMatch {
    unsigned char digest[32];
    size_t match_index;
    const char *item_key;
} HWAProductionRankedMatch;

typedef struct HWAProductionMetricValue {
    double value;
    double confidence;
    uint32_t evidence_flags;
    uint32_t quality_flags;
    int valid;
} HWAProductionMetricValue;

typedef struct HWAProductionEnvelope {
    double *points;
    size_t count;
    size_t capacity;
} HWAProductionEnvelope;

typedef struct HWAProductionSpanValues {
    HWAProductionMetricValue reference[HWA_PRODUCTION_METRICS_PER_VIEW];
    HWAProductionMetricValue model[HWA_PRODUCTION_METRICS_PER_VIEW];
    HWAProductionEnvelope reference_envelope;
    HWAProductionEnvelope model_envelope;
} HWAProductionSpanValues;

typedef struct HWAProductionAudioAccum {
    long double sum_squares;
    long double peak;
    long double left_sum;
    long double right_sum;
    long double left_squares;
    long double right_squares;
    long double left_right;
    long double mid_squares;
    long double side_squares;
    long double *delay_sums;
    uint32_t *delay_counts;
    double *delay_anchor_left;
    double *delay_right_ring;
    size_t delay_anchor_count;
    size_t delay_next_anchor;
    size_t delay_max_lag;
    size_t delay_ring_size;
    uint64_t delay_interior_length;
    uint64_t delay_work_bytes;
    double *envelope_ring;
    size_t envelope_window;
    size_t envelope_hop;
    size_t envelope_position;
    size_t envelope_filled;
    long double envelope_sum_squares;
    HWAProductionEnvelope *envelope;
    double low_state[HWA_PRODUCTION_ROOM_BAND_COUNT];
    double high_state[HWA_PRODUCTION_ROOM_BAND_COUNT];
    long double direct_energy[HWA_PRODUCTION_ROOM_BAND_COUNT];
    long double early_energy[HWA_PRODUCTION_ROOM_BAND_COUNT];
    long double decay_energy[HWA_PRODUCTION_ROOM_BAND_COUNT][64U];
    uint64_t count;
    uint64_t direct_count;
    uint64_t early_count;
} HWAProductionAudioAccum;

typedef struct HWAProductionTimeline {
    uint64_t start;
    uint64_t end;
    size_t span_index;
} HWAProductionTimeline;

typedef struct HWAProductionRoomFacts {
    double response_db[HWA_BAND_COUNT];
    double early_db[HWA_PRODUCTION_ROOM_BAND_COUNT];
    double late_seconds[HWA_PRODUCTION_ROOM_BAND_COUNT];
    double power_gain_db;
    size_t point_count;
    int response_valid[HWA_BAND_COUNT];
    int early_valid[HWA_PRODUCTION_ROOM_BAND_COUNT];
    int late_valid[HWA_PRODUCTION_ROOM_BAND_COUNT];
    int supplied;
} HWAProductionRoomFacts;

typedef struct HWAProductionFitState {
    double eq_gain[HWA_PRODUCTION_EQ_NODE_COUNT];
    int eq_valid[HWA_PRODUCTION_EQ_NODE_COUNT];
    double threshold_dbfs;
    double ratio;
    double makeup_db;
    int dynamics_valid;
    double delay_samples;
    double polarity;
    double balance_db;
    double width_ratio;
    double correlation;
    int delay_valid;
    int polarity_valid;
    int balance_valid;
    int width_valid;
    int correlation_valid;
    uint32_t train_sufficient_flags;
    uint32_t check_sufficient_flags;
} HWAProductionFitState;

typedef struct HWAProductionFileIdentity {
    uint64_t device;
    uint64_t file;
    uint64_t size;
} HWAProductionFileIdentity;

static const char *hwa_production_name(const HWAProductionName *names,
                                       size_t count,
                                       int value)
{
    size_t index;
    for (index = 0U; index < count; ++index) {
        if (names[index].value == value) return names[index].name;
    }
    return NULL;
}

static int hwa_production_from_name(const HWAProductionName *names,
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

#define HWA_PRODUCTION_NAME_GETTER(function_name, array_name, enum_type)      \
    const char *function_name(enum_type value)                               \
    {                                                                         \
        return hwa_production_name(                                           \
            array_name, HWA_PRODUCTION_ARRAY_COUNT(array_name), (int)value);  \
    }

#define HWA_PRODUCTION_NAME_PARSER(function_name, array_name, enum_type)      \
    int function_name(const char *name, enum_type *value)                     \
    {                                                                         \
        int parsed;                                                           \
        if (value == NULL || hwa_production_from_name(                        \
                array_name, HWA_PRODUCTION_ARRAY_COUNT(array_name), name,     \
                &parsed) != 0) return -1;                                     \
        *value = (enum_type)parsed;                                           \
        return 0;                                                             \
    }

HWA_PRODUCTION_NAME_GETTER(hwa_production_availability_name,
                           hwa_production_availability_names,
                           HWAProductionAvailability)
HWA_PRODUCTION_NAME_GETTER(hwa_production_split_name,
                           hwa_production_split_names,
                           HWAProductionSplit)
HWA_PRODUCTION_NAME_GETTER(hwa_production_view_name,
                           hwa_production_view_names,
                           HWAProductionView)
HWA_PRODUCTION_NAME_GETTER(hwa_production_scope_name,
                           hwa_production_scope_names,
                           HWAProductionScope)
HWA_PRODUCTION_NAME_GETTER(hwa_production_unit_name,
                           hwa_production_unit_names,
                           HWAProductionUnit)
HWA_PRODUCTION_NAME_GETTER(hwa_production_fit_kind_name,
                           hwa_production_fit_names,
                           HWAProductionFitKind)
HWA_PRODUCTION_NAME_GETTER(hwa_production_metric_kind_name,
                           hwa_production_metric_names,
                           HWAProductionMetricKind)

HWA_PRODUCTION_NAME_PARSER(hwa_production_availability_from_name,
                           hwa_production_availability_names,
                           HWAProductionAvailability)
HWA_PRODUCTION_NAME_PARSER(hwa_production_split_from_name,
                           hwa_production_split_names,
                           HWAProductionSplit)
HWA_PRODUCTION_NAME_PARSER(hwa_production_view_from_name,
                           hwa_production_view_names,
                           HWAProductionView)
HWA_PRODUCTION_NAME_PARSER(hwa_production_scope_from_name,
                           hwa_production_scope_names,
                           HWAProductionScope)
HWA_PRODUCTION_NAME_PARSER(hwa_production_unit_from_name,
                           hwa_production_unit_names,
                           HWAProductionUnit)
HWA_PRODUCTION_NAME_PARSER(hwa_production_fit_kind_from_name,
                           hwa_production_fit_names,
                           HWAProductionFitKind)
HWA_PRODUCTION_NAME_PARSER(hwa_production_metric_kind_from_name,
                           hwa_production_metric_names,
                           HWAProductionMetricKind)

int hwa_production_split_for_item_key(const char *item_key,
                                      HWAProductionSplit *split)
{
    const char *source_event;
    HWASha256 context;
    unsigned char digest[32];
    unsigned remainder = 0U;
    size_t index;
    if (item_key == NULL || split == NULL || item_key[0] == '\0') return -1;
    source_event = strchr(item_key, ':');
    if (source_event == NULL || source_event[1] == '\0') return -1;
    source_event++;
    hwa_sha256_init(&context);
    hwa_sha256_update(&context, (const unsigned char *)source_event,
                      strlen(source_event));
    hwa_sha256_final(&context, digest);
    for (index = 0U; index < sizeof(digest); ++index) {
        remainder = (remainder * 256U + (unsigned)digest[index]) % 5U;
    }
    *split = remainder == 0U ? HWA_PRODUCTION_CHECK : HWA_PRODUCTION_TRAIN;
    return 0;
}

int hwa_production_fit_shape_valid(HWAProductionFitKind kind,
                                   uint32_t index,
                                   HWAProductionUnit unit)
{
    switch (kind) {
    case HWA_PRODUCTION_FIT_EQ_GAIN_DB:
        return index < HWA_PRODUCTION_EQ_NODE_COUNT &&
               unit == HWA_PRODUCTION_UNIT_DB;
    case HWA_PRODUCTION_FIT_THRESHOLD_DBFS:
        return index == 0U && unit == HWA_PRODUCTION_UNIT_DBFS;
    case HWA_PRODUCTION_FIT_RATIO:
    case HWA_PRODUCTION_FIT_CHANNEL_POLARITY:
    case HWA_PRODUCTION_FIT_STEREO_WIDTH_RATIO:
    case HWA_PRODUCTION_FIT_STEREO_CORRELATION:
        return index == 0U && unit == HWA_PRODUCTION_UNIT_RATIO;
    case HWA_PRODUCTION_FIT_MAKEUP_DB:
    case HWA_PRODUCTION_FIT_CHANNEL_BALANCE_DB:
        return index == 0U && unit == HWA_PRODUCTION_UNIT_DB;
    case HWA_PRODUCTION_FIT_STEREO_DELAY_SAMPLES:
        return index == 0U && unit == HWA_PRODUCTION_UNIT_SAMPLES;
    case HWA_PRODUCTION_FIT_EARLY_REFLECTION_DB:
        return index < HWA_PRODUCTION_ROOM_BAND_COUNT &&
               unit == HWA_PRODUCTION_UNIT_DB;
    case HWA_PRODUCTION_FIT_LATE_DECAY_SECONDS:
        return index < HWA_PRODUCTION_ROOM_BAND_COUNT &&
               unit == HWA_PRODUCTION_UNIT_SECONDS;
    default:
        return 0;
    }
}

int hwa_production_metric_shape_valid(HWAProductionMetricKind kind,
                                      uint32_t index,
                                      HWAProductionUnit unit)
{
    switch (kind) {
    case HWA_PRODUCTION_METRIC_RMS_DBFS:
        return index == 0U && unit == HWA_PRODUCTION_UNIT_DBFS;
    case HWA_PRODUCTION_METRIC_CREST_DB:
    case HWA_PRODUCTION_METRIC_LEVEL_SPREAD_DB:
    case HWA_PRODUCTION_METRIC_CHANNEL_BALANCE_DB:
        return index == 0U && unit == HWA_PRODUCTION_UNIT_DB;
    case HWA_PRODUCTION_METRIC_BAND_LEVEL_DBFS:
        return index < HWA_BAND_COUNT && unit == HWA_PRODUCTION_UNIT_DBFS;
    case HWA_PRODUCTION_METRIC_STEREO_WIDTH_RATIO:
    case HWA_PRODUCTION_METRIC_STEREO_CORRELATION:
        return index == 0U && unit == HWA_PRODUCTION_UNIT_RATIO;
    case HWA_PRODUCTION_METRIC_STEREO_DELAY_SAMPLES:
        return index == 0U && unit == HWA_PRODUCTION_UNIT_SAMPLES;
    case HWA_PRODUCTION_METRIC_EARLY_REFLECTION_DB:
        return index < HWA_PRODUCTION_ROOM_BAND_COUNT &&
               unit == HWA_PRODUCTION_UNIT_DB;
    case HWA_PRODUCTION_METRIC_LATE_DECAY_SECONDS:
        return index < HWA_PRODUCTION_ROOM_BAND_COUNT &&
               unit == HWA_PRODUCTION_UNIT_SECONDS;
    default:
        return 0;
    }
}

static int hwa_production_options_valid(const HWAProductionOptions *options,
                                        char *error,
                                        size_t error_size)
{
    if (options == NULL || options->decode_block_frames == 0U ||
        options->decode_block_frames > 1048576U ||
        options->max_input_bytes == 0U ||
        options->max_input_frames == 0U ||
        options->max_ir_frames == 0U || options->max_work_bytes == 0U ||
        options->max_evaluations == 0U ||
        options->max_spans == 0U || options->max_envelope_points == 0U ||
        options->max_fits < 61U ||
        options->max_evaluation_rows < HWA_PRODUCTION_EVALUATIONS_PER_SPAN ||
        options->max_view_rows <
            (HWA_PRODUCTION_SPLIT_COUNT - 1U) *
            HWA_PRODUCTION_EVALUATIONS_PER_SPAN ||
        options->max_warnings == 0U ||
        options->profile_limits.max_input_bytes == 0U ||
        options->profile_limits.max_work_bytes == 0U ||
        options->profile_limits.max_contexts == 0U ||
        options->profile_limits.max_measurements == 0U ||
        options->profile_limits.max_groups == 0U ||
        options->profile_limits.max_group_members == 0U ||
        options->profile_limits.max_statistics == 0U ||
        options->profile_limits.max_warnings == 0U ||
        options->profile_limits.max_distributions == 0U ||
        options->profile_limits.max_gaps == 0U) {
        hwa_set_error(error, error_size, "invalid Stage 6 limits or DSP options");
        return 0;
    }
    return 1;
}

static int hwa_production_add_size(uint64_t *total, uint64_t amount,
                                   uint64_t limit)
{
    if (amount > UINT64_MAX - *total || *total + amount > limit) return -1;
    *total += amount;
    return 0;
}

static int hwa_production_array_bytes(size_t count, size_t element,
                                      uint64_t *bytes)
{
    if (element != 0U &&
        (count > SIZE_MAX / element || count > UINT64_MAX / element)) {
        return -1;
    }
    *bytes = (uint64_t)count * (uint64_t)element;
    return 0;
}

static int hwa_production_charge_evaluations(
    uint64_t *evaluations,
    uint64_t amount,
    uint64_t limit,
    const char *work,
    char *error,
    size_t error_size)
{
    if (evaluations == NULL || *evaluations > limit ||
        amount > limit - *evaluations) {
        hwa_set_error(error, error_size,
                      "%s exceeds the Stage 6 evaluation cap", work);
        return -1;
    }
    *evaluations += amount;
    return 0;
}

static char *hwa_production_copy(const char *text)
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

static int hwa_production_path_identity(
    const char *path,
    HWAProductionFileIdentity *identity,
    char *error,
    size_t error_size)
{
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION information;
    HANDLE handle = CreateFileA(
        path, 0U, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(handle, &information) ||
        (information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        if (handle != INVALID_HANDLE_VALUE) (void)CloseHandle(handle);
        hwa_set_error(error, error_size,
                      "Stage 6 input is not a named regular file: %s", path);
        return -1;
    }
    (void)CloseHandle(handle);
    identity->device = (uint64_t)information.dwVolumeSerialNumber;
    identity->file = ((uint64_t)information.nFileIndexHigh << 32U) |
                     (uint64_t)information.nFileIndexLow;
    identity->size = ((uint64_t)information.nFileSizeHigh << 32U) |
                     (uint64_t)information.nFileSizeLow;
#else
    struct stat facts;
    if (path == NULL || path[0] == '\0' || strcmp(path, "-") == 0 ||
        lstat(path, &facts) != 0 || !S_ISREG(facts.st_mode) ||
        facts.st_size < 0) {
        hwa_set_error(error, error_size,
                      "Stage 6 input is not a named regular file: %s",
                      path == NULL ? "" : path);
        return -1;
    }
    identity->device = (uint64_t)facts.st_dev;
    identity->file = (uint64_t)facts.st_ino;
    identity->size = (uint64_t)facts.st_size;
#endif
    return 0;
}

static int hwa_production_stream_identity(
    FILE *stream,
    HWAProductionFileIdentity *identity)
{
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION information;
    int descriptor = _fileno(stream);
    intptr_t raw_handle = descriptor >= 0 ? _get_osfhandle(descriptor) :
                                            (intptr_t)-1;
    if (raw_handle == (intptr_t)-1 ||
        !GetFileInformationByHandle((HANDLE)raw_handle, &information) ||
        (information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        return -1;
    }
    identity->device = (uint64_t)information.dwVolumeSerialNumber;
    identity->file = ((uint64_t)information.nFileIndexHigh << 32U) |
                     (uint64_t)information.nFileIndexLow;
    identity->size = ((uint64_t)information.nFileSizeHigh << 32U) |
                     (uint64_t)information.nFileSizeLow;
#else
    struct stat facts;
    int descriptor = fileno(stream);
    if (descriptor < 0 || fstat(descriptor, &facts) != 0 ||
        !S_ISREG(facts.st_mode) || facts.st_size < 0) return -1;
    identity->device = (uint64_t)facts.st_dev;
    identity->file = (uint64_t)facts.st_ino;
    identity->size = (uint64_t)facts.st_size;
#endif
    return 0;
}

static int hwa_production_identity_equal(
    const HWAProductionFileIdentity *left,
    const HWAProductionFileIdentity *right)
{
    return left->device == right->device && left->file == right->file &&
           left->size == right->size;
}

static int hwa_production_seek(FILE *stream, uint64_t offset)
{
    if (offset > (uint64_t)INT64_MAX) return -1;
#if defined(_WIN32)
    return _fseeki64(stream, (__int64)offset, SEEK_SET);
#else
    if (sizeof(off_t) < 8U && offset > (uint64_t)INT32_MAX) return -1;
    return fseeko(stream, (off_t)offset, SEEK_SET);
#endif
}

static int hwa_production_hash_stream(
    FILE *stream,
    uint64_t max_bytes,
    char hex[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    unsigned char buffer[65536];
    unsigned char digest[32];
    HWASha256 sha;
    uint64_t total = 0U;
    if (hwa_production_seek(stream, 0U) != 0) {
        hwa_set_error(error, error_size, "cannot rewind Stage 6 input");
        return -1;
    }
    clearerr(stream);
    hwa_sha256_init(&sha);
    for (;;) {
        size_t count = fread(buffer, 1U, sizeof(buffer), stream);
        if ((uint64_t)count > max_bytes - total) {
            hwa_set_error(error, error_size, "Stage 6 input exceeds byte cap");
            return -1;
        }
        total += (uint64_t)count;
        hwa_sha256_update(&sha, buffer, count);
        if (count < sizeof(buffer)) {
            if (ferror(stream)) {
                hwa_set_error(error, error_size, "cannot hash Stage 6 input");
                return -1;
            }
            break;
        }
        if (total == max_bytes) {
            int extra = fgetc(stream);
            if (extra != EOF) {
                hwa_set_error(error, error_size,
                              "Stage 6 input exceeds byte cap");
                return -1;
            }
            if (ferror(stream)) {
                hwa_set_error(error, error_size, "cannot hash Stage 6 input");
                return -1;
            }
            break;
        }
    }
    hwa_sha256_final(&sha, digest);
    hwa_sha256_hex(digest, hex);
    return 0;
}

static int hwa_production_hash_named_file(
    const char *path,
    uint64_t max_bytes,
    HWAProductionFileIdentity *identity,
    char sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    HWAProductionFileIdentity path_before;
    HWAProductionFileIdentity stream_before;
    HWAProductionFileIdentity stream_after;
    HWAProductionFileIdentity path_after;
    FILE *stream = NULL;
    int status = -1;
    if (hwa_production_path_identity(
            path, &path_before, error, error_size) != 0) return -1;
    stream = fopen(path, "rb");
    if (stream == NULL ||
        hwa_production_stream_identity(stream, &stream_before) != 0 ||
        !hwa_production_identity_equal(&path_before, &stream_before) ||
        hwa_production_hash_stream(
            stream, max_bytes, sha256, error, error_size) != 0 ||
        hwa_production_stream_identity(stream, &stream_after) != 0 ||
        hwa_production_path_identity(
            path, &path_after, error, error_size) != 0 ||
        !hwa_production_identity_equal(&stream_before, &stream_after) ||
        !hwa_production_identity_equal(&stream_before, &path_after)) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "Stage 6 input changed while it was hashed: %s",
                          path == NULL ? "" : path);
        }
        goto cleanup;
    }
    if (identity != NULL) *identity = stream_before;
    status = 0;
cleanup:
    if (stream != NULL && fclose(stream) != 0 && status == 0) {
        hwa_set_error(error, error_size,
                      "cannot close Stage 6 input after hashing");
        status = -1;
    }
    return status;
}

size_t hwa_production_metric_catalog_count(void)
{
    return HWA_PRODUCTION_METRICS_PER_VIEW;
}

int hwa_production_metric_catalog_at(
    size_t offset,
    HWAProductionMetricKind *kind,
    uint32_t *index,
    HWAProductionUnit *unit)
{
    if (offset == 0U) {
        *kind = HWA_PRODUCTION_METRIC_RMS_DBFS;
        *index = 0U;
        *unit = HWA_PRODUCTION_UNIT_DBFS;
    } else if (offset == 1U) {
        *kind = HWA_PRODUCTION_METRIC_CREST_DB;
        *index = 0U;
        *unit = HWA_PRODUCTION_UNIT_DB;
    } else if (offset < 2U + HWA_BAND_COUNT) {
        *kind = HWA_PRODUCTION_METRIC_BAND_LEVEL_DBFS;
        *index = (uint32_t)(offset - 2U);
        *unit = HWA_PRODUCTION_UNIT_DBFS;
    } else if (offset == 2U + HWA_BAND_COUNT) {
        *kind = HWA_PRODUCTION_METRIC_LEVEL_SPREAD_DB;
        *index = 0U;
        *unit = HWA_PRODUCTION_UNIT_DB;
    } else if (offset == 3U + HWA_BAND_COUNT) {
        *kind = HWA_PRODUCTION_METRIC_CHANNEL_BALANCE_DB;
        *index = 0U;
        *unit = HWA_PRODUCTION_UNIT_DB;
    } else if (offset == 4U + HWA_BAND_COUNT) {
        *kind = HWA_PRODUCTION_METRIC_STEREO_WIDTH_RATIO;
        *index = 0U;
        *unit = HWA_PRODUCTION_UNIT_RATIO;
    } else if (offset == 5U + HWA_BAND_COUNT) {
        *kind = HWA_PRODUCTION_METRIC_STEREO_CORRELATION;
        *index = 0U;
        *unit = HWA_PRODUCTION_UNIT_RATIO;
    } else if (offset == 6U + HWA_BAND_COUNT) {
        *kind = HWA_PRODUCTION_METRIC_STEREO_DELAY_SAMPLES;
        *index = 0U;
        *unit = HWA_PRODUCTION_UNIT_SAMPLES;
    } else if (offset < 7U + HWA_BAND_COUNT +
                        HWA_PRODUCTION_ROOM_BAND_COUNT) {
        *kind = HWA_PRODUCTION_METRIC_EARLY_REFLECTION_DB;
        *index = (uint32_t)(offset - (7U + HWA_BAND_COUNT));
        *unit = HWA_PRODUCTION_UNIT_DB;
    } else if (offset < HWA_PRODUCTION_METRICS_PER_VIEW) {
        *kind = HWA_PRODUCTION_METRIC_LATE_DECAY_SECONDS;
        *index = (uint32_t)(offset - (7U + HWA_BAND_COUNT +
                                      HWA_PRODUCTION_ROOM_BAND_COUNT));
        *unit = HWA_PRODUCTION_UNIT_SECONDS;
    } else {
        return -1;
    }
    return 0;
}

size_t hwa_production_fit_catalog_count(void)
{
    return 61U;
}

int hwa_production_fit_catalog_at(size_t offset,
                                  HWAProductionScope *scope,
                                  HWAProductionFitKind *kind,
                                  uint32_t *index,
                                  HWAProductionUnit *unit)
{
    size_t local;
    if (scope == NULL || kind == NULL || index == NULL || unit == NULL ||
        offset >= hwa_production_fit_catalog_count()) return -1;
    if (offset < 15U) {
        *scope = HWA_PRODUCTION_SCOPE_CORRECTION;
        if (offset < HWA_PRODUCTION_EQ_NODE_COUNT) {
            *kind = HWA_PRODUCTION_FIT_EQ_GAIN_DB;
            *index = (uint32_t)offset;
            *unit = HWA_PRODUCTION_UNIT_DB;
            return 0;
        }
        local = offset - HWA_PRODUCTION_EQ_NODE_COUNT;
        *index = 0U;
        switch (local) {
        case 0U: *kind = HWA_PRODUCTION_FIT_THRESHOLD_DBFS;
                 *unit = HWA_PRODUCTION_UNIT_DBFS; return 0;
        case 1U: *kind = HWA_PRODUCTION_FIT_RATIO;
                 *unit = HWA_PRODUCTION_UNIT_RATIO; return 0;
        case 2U: *kind = HWA_PRODUCTION_FIT_MAKEUP_DB;
                 *unit = HWA_PRODUCTION_UNIT_DB; return 0;
        case 3U: *kind = HWA_PRODUCTION_FIT_STEREO_DELAY_SAMPLES;
                 *unit = HWA_PRODUCTION_UNIT_SAMPLES; return 0;
        case 4U: *kind = HWA_PRODUCTION_FIT_CHANNEL_POLARITY;
                 *unit = HWA_PRODUCTION_UNIT_RATIO; return 0;
        case 5U: *kind = HWA_PRODUCTION_FIT_CHANNEL_BALANCE_DB;
                 *unit = HWA_PRODUCTION_UNIT_DB; return 0;
        case 6U: *kind = HWA_PRODUCTION_FIT_STEREO_WIDTH_RATIO;
                 *unit = HWA_PRODUCTION_UNIT_RATIO; return 0;
        case 7U: *kind = HWA_PRODUCTION_FIT_STEREO_CORRELATION;
                 *unit = HWA_PRODUCTION_UNIT_RATIO; return 0;
        default: return -1;
        }
    }
    if (offset < 49U) {
        local = (offset - 15U) % 17U;
        *scope = offset < 32U ? HWA_PRODUCTION_SCOPE_REFERENCE :
                                HWA_PRODUCTION_SCOPE_MODEL;
        if (local < 5U) {
            static const HWAProductionFitKind kinds[5] = {
                HWA_PRODUCTION_FIT_STEREO_DELAY_SAMPLES,
                HWA_PRODUCTION_FIT_CHANNEL_POLARITY,
                HWA_PRODUCTION_FIT_CHANNEL_BALANCE_DB,
                HWA_PRODUCTION_FIT_STEREO_WIDTH_RATIO,
                HWA_PRODUCTION_FIT_STEREO_CORRELATION
            };
            static const HWAProductionUnit units[5] = {
                HWA_PRODUCTION_UNIT_SAMPLES,
                HWA_PRODUCTION_UNIT_RATIO,
                HWA_PRODUCTION_UNIT_DB,
                HWA_PRODUCTION_UNIT_RATIO,
                HWA_PRODUCTION_UNIT_RATIO
            };
            *kind = kinds[local];
            *index = 0U;
            *unit = units[local];
            return 0;
        }
        if (local < 11U) {
            *kind = HWA_PRODUCTION_FIT_EARLY_REFLECTION_DB;
            *index = (uint32_t)(local - 5U);
            *unit = HWA_PRODUCTION_UNIT_DB;
        } else {
            *kind = HWA_PRODUCTION_FIT_LATE_DECAY_SECONDS;
            *index = (uint32_t)(local - 11U);
            *unit = HWA_PRODUCTION_UNIT_SECONDS;
        }
        return 0;
    }
    local = offset - 49U;
    *scope = HWA_PRODUCTION_SCOPE_ROOM_IR;
    if (local < HWA_PRODUCTION_ROOM_BAND_COUNT) {
        *kind = HWA_PRODUCTION_FIT_EARLY_REFLECTION_DB;
        *index = (uint32_t)local;
        *unit = HWA_PRODUCTION_UNIT_DB;
    } else {
        *kind = HWA_PRODUCTION_FIT_LATE_DECAY_SECONDS;
        *index = (uint32_t)(local - HWA_PRODUCTION_ROOM_BAND_COUNT);
        *unit = HWA_PRODUCTION_UNIT_SECONDS;
    }
    return 0;
}

double hwa_production_eq_node_frequency_hz(size_t index)
{
    static const double frequencies[HWA_PRODUCTION_EQ_NODE_COUNT] = {
        80.0, 160.0, 320.0, 640.0, 1280.0, 2560.0, 5120.0
    };
    return index < HWA_PRODUCTION_EQ_NODE_COUNT ? frequencies[index] : 0.0;
}

int hwa_production_eq_node_supported(size_t index,
                                     uint32_t reference_rate_hz,
                                     uint32_t model_rate_hz)
{
    double frequency = hwa_production_eq_node_frequency_hz(index);
    return frequency > 0.0 && reference_rate_hz != 0U && model_rate_hz != 0U &&
           frequency < 0.45 * (double)reference_rate_hz &&
           frequency < 0.45 * (double)model_rate_hz;
}

int hwa_production_eq_adjacent_valid(double lower_gain_db,
                                     double upper_gain_db)
{
    return isfinite(lower_gain_db) && isfinite(upper_gain_db) &&
           fabs(upper_gain_db - lower_gain_db) <= 3.0;
}

int hwa_production_room_band_supported(uint32_t index,
                                       uint32_t rate_hz)
{
    static const double upper_edges[HWA_PRODUCTION_ROOM_BAND_COUNT] = {
        250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0
    };
    return index < HWA_PRODUCTION_ROOM_BAND_COUNT && rate_hz != 0U &&
           upper_edges[index] < 0.45 * (double)rate_hz;
}

size_t hwa_production_minimum_train_spans(void)
{
    return HWA_PRODUCTION_MIN_TRAIN_SPANS;
}

size_t hwa_production_minimum_check_spans(void)
{
    return HWA_PRODUCTION_MIN_CHECK_SPANS;
}

size_t hwa_production_max_train_spans_per_family(void)
{
    return HWA_PRODUCTION_MAX_TRAIN_PER_FAMILY;
}

size_t hwa_production_max_check_spans_per_family(void)
{
    return HWA_PRODUCTION_MAX_CHECK_PER_FAMILY;
}

size_t hwa_production_threshold_grid_count(void)
{
    return 43U;
}

double hwa_production_threshold_grid_at(size_t index)
{
    return index < hwa_production_threshold_grid_count() ?
               -48.0 + (double)index : 0.0;
}

size_t hwa_production_ratio_grid_count(void)
{
    return 9U;
}

double hwa_production_ratio_grid_at(size_t index)
{
    static const double ratios[] = {
        1.0, 1.25, 1.5, 2.0, 3.0, 4.0, 6.0, 10.0, 20.0
    };
    return index < HWA_PRODUCTION_ARRAY_COUNT(ratios) ? ratios[index] : 0.0;
}

uint32_t hwa_production_fit_eligibility_flag(HWAProductionFitKind kind)
{
    switch (kind) {
    case HWA_PRODUCTION_FIT_EQ_GAIN_DB:
        return HWA_PRODUCTION_SPAN_EQ;
    case HWA_PRODUCTION_FIT_THRESHOLD_DBFS:
    case HWA_PRODUCTION_FIT_RATIO:
    case HWA_PRODUCTION_FIT_MAKEUP_DB:
        return HWA_PRODUCTION_SPAN_DYNAMICS;
    case HWA_PRODUCTION_FIT_STEREO_DELAY_SAMPLES:
    case HWA_PRODUCTION_FIT_CHANNEL_POLARITY:
    case HWA_PRODUCTION_FIT_CHANNEL_BALANCE_DB:
    case HWA_PRODUCTION_FIT_STEREO_WIDTH_RATIO:
    case HWA_PRODUCTION_FIT_STEREO_CORRELATION:
        return HWA_PRODUCTION_SPAN_STEREO;
    case HWA_PRODUCTION_FIT_EARLY_REFLECTION_DB:
    case HWA_PRODUCTION_FIT_LATE_DECAY_SECONDS:
        return HWA_PRODUCTION_SPAN_DECAY;
    default:
        return 0U;
    }
}

uint32_t hwa_production_metric_eligibility_flags(HWAProductionView view,
                                                 HWAProductionMetricKind kind)
{
    if (view == HWA_PRODUCTION_VIEW_RAW) return 0U;
    switch (kind) {
    case HWA_PRODUCTION_METRIC_RMS_DBFS:
    case HWA_PRODUCTION_METRIC_LEVEL_SPREAD_DB:
        return HWA_PRODUCTION_SPAN_DYNAMICS;
    case HWA_PRODUCTION_METRIC_BAND_LEVEL_DBFS:
        return HWA_PRODUCTION_SPAN_EQ | HWA_PRODUCTION_SPAN_DYNAMICS;
    case HWA_PRODUCTION_METRIC_CHANNEL_BALANCE_DB:
    case HWA_PRODUCTION_METRIC_STEREO_WIDTH_RATIO:
    case HWA_PRODUCTION_METRIC_STEREO_CORRELATION:
    case HWA_PRODUCTION_METRIC_STEREO_DELAY_SAMPLES:
        return HWA_PRODUCTION_SPAN_STEREO;
    case HWA_PRODUCTION_METRIC_EARLY_REFLECTION_DB:
    case HWA_PRODUCTION_METRIC_LATE_DECAY_SECONDS:
        return HWA_PRODUCTION_SPAN_DECAY;
    default:
        return 0U;
    }
}

int hwa_production_raw_metric_applicable(
    const HWAProductionSpan *span,
    HWAProductionMetricKind kind)
{
    uint32_t flags;
    if (span == NULL) return 0;
    flags = span->eligibility_flags;
    switch (kind) {
    case HWA_PRODUCTION_METRIC_RMS_DBFS:
    case HWA_PRODUCTION_METRIC_BAND_LEVEL_DBFS:
        return (flags & (HWA_PRODUCTION_SPAN_EQ |
                         HWA_PRODUCTION_SPAN_DYNAMICS)) != 0U;
    case HWA_PRODUCTION_METRIC_CREST_DB:
        return 1;
    case HWA_PRODUCTION_METRIC_LEVEL_SPREAD_DB:
        return (flags & HWA_PRODUCTION_SPAN_DYNAMICS) != 0U;
    case HWA_PRODUCTION_METRIC_CHANNEL_BALANCE_DB:
    case HWA_PRODUCTION_METRIC_STEREO_WIDTH_RATIO:
    case HWA_PRODUCTION_METRIC_STEREO_CORRELATION:
    case HWA_PRODUCTION_METRIC_STEREO_DELAY_SAMPLES:
        return (flags & HWA_PRODUCTION_SPAN_STEREO) != 0U;
    case HWA_PRODUCTION_METRIC_EARLY_REFLECTION_DB:
    case HWA_PRODUCTION_METRIC_LATE_DECAY_SECONDS:
        return (flags & HWA_PRODUCTION_SPAN_DECAY) != 0U;
    default:
        return 0;
    }
}

int hwa_production_fit_value_valid(HWAProductionScope scope,
                                   HWAProductionFitKind kind,
                                   uint32_t index,
                                   double value,
                                   uint32_t reference_rate_hz,
                                   uint32_t model_rate_hz)
{
    double delay_limit;
    if (!isfinite(value)) return 0;
    switch (kind) {
    case HWA_PRODUCTION_FIT_EQ_GAIN_DB:
        return scope == HWA_PRODUCTION_SCOPE_CORRECTION &&
               hwa_production_eq_node_supported(
                   index, reference_rate_hz, model_rate_hz) &&
               value >= -6.0 && value <= 6.0;
    case HWA_PRODUCTION_FIT_THRESHOLD_DBFS:
        if (scope == HWA_PRODUCTION_SCOPE_CORRECTION && index == 0U) {
            size_t grid;
            for (grid = 0U; grid < hwa_production_threshold_grid_count();
                 ++grid) {
                if (value == hwa_production_threshold_grid_at(grid)) return 1;
            }
        }
        return 0;
    case HWA_PRODUCTION_FIT_RATIO: {
        size_t grid;
        if (scope != HWA_PRODUCTION_SCOPE_CORRECTION || index != 0U) return 0;
        for (grid = 0U; grid < hwa_production_ratio_grid_count(); ++grid) {
            if (value == hwa_production_ratio_grid_at(grid)) return 1;
        }
        return 0;
    }
    case HWA_PRODUCTION_FIT_MAKEUP_DB:
        return scope == HWA_PRODUCTION_SCOPE_CORRECTION && index == 0U &&
               value >= -12.0 && value <= 12.0;
    case HWA_PRODUCTION_FIT_STEREO_DELAY_SAMPLES:
        if (index != 0U || scope == HWA_PRODUCTION_SCOPE_ROOM_IR) return 0;
        delay_limit = 0.002 * (double)(
            scope == HWA_PRODUCTION_SCOPE_REFERENCE ? reference_rate_hz :
                                                      model_rate_hz);
        return value >= -floor(delay_limit) && value <= floor(delay_limit);
    case HWA_PRODUCTION_FIT_CHANNEL_POLARITY:
        return index == 0U && scope != HWA_PRODUCTION_SCOPE_ROOM_IR &&
               (value == -1.0 || value == 1.0);
    case HWA_PRODUCTION_FIT_CHANNEL_BALANCE_DB:
        return index == 0U && scope != HWA_PRODUCTION_SCOPE_ROOM_IR &&
               value >= (scope == HWA_PRODUCTION_SCOPE_CORRECTION ? -12.0 :
                                                                      -24.0) &&
               value <= (scope == HWA_PRODUCTION_SCOPE_CORRECTION ? 12.0 :
                                                                      24.0);
    case HWA_PRODUCTION_FIT_STEREO_WIDTH_RATIO:
        return index == 0U && scope != HWA_PRODUCTION_SCOPE_ROOM_IR &&
               value >= (scope == HWA_PRODUCTION_SCOPE_CORRECTION ? 0.25 :
                                                                      0.0) &&
               value <= 4.0;
    case HWA_PRODUCTION_FIT_STEREO_CORRELATION:
        return index == 0U && scope != HWA_PRODUCTION_SCOPE_ROOM_IR &&
               value >= -1.0 && value <= 1.0;
    case HWA_PRODUCTION_FIT_EARLY_REFLECTION_DB:
        return scope != HWA_PRODUCTION_SCOPE_CORRECTION &&
               index < HWA_PRODUCTION_ROOM_BAND_COUNT &&
               value >= -120.0 && value <= 60.0;
    case HWA_PRODUCTION_FIT_LATE_DECAY_SECONDS:
        return scope != HWA_PRODUCTION_SCOPE_CORRECTION &&
               index < HWA_PRODUCTION_ROOM_BAND_COUNT &&
               value >= 0.0 && value <= 30.0;
    default:
        return 0;
    }
}

int hwa_production_metric_value_valid(HWAProductionMetricKind kind,
                                      uint32_t index,
                                      double value,
                                      uint32_t rate_hz)
{
    if (!isfinite(value) || rate_hz == 0U) return 0;
    switch (kind) {
    case HWA_PRODUCTION_METRIC_RMS_DBFS:
    case HWA_PRODUCTION_METRIC_BAND_LEVEL_DBFS:
        return value >= -300.0 && value <= 300.0;
    case HWA_PRODUCTION_METRIC_CREST_DB:
        return value >= 0.0 && value <= 600.0;
    case HWA_PRODUCTION_METRIC_LEVEL_SPREAD_DB:
        return value >= 0.0 && value <= 300.0;
    case HWA_PRODUCTION_METRIC_CHANNEL_BALANCE_DB:
        return value >= -24.0 && value <= 24.0;
    case HWA_PRODUCTION_METRIC_STEREO_WIDTH_RATIO:
        return value >= 0.0 && value <= 4.0;
    case HWA_PRODUCTION_METRIC_STEREO_CORRELATION:
        return value >= -1.0 && value <= 1.0;
    case HWA_PRODUCTION_METRIC_STEREO_DELAY_SAMPLES:
        return value >= -floor(0.002 * (double)rate_hz) &&
               value <= floor(0.002 * (double)rate_hz);
    case HWA_PRODUCTION_METRIC_EARLY_REFLECTION_DB:
        return index < HWA_PRODUCTION_ROOM_BAND_COUNT &&
               value >= -120.0 && value <= 60.0;
    case HWA_PRODUCTION_METRIC_LATE_DECAY_SECONDS:
        return index < HWA_PRODUCTION_ROOM_BAND_COUNT &&
               value >= 0.0 && value <= 30.0;
    default:
        return 0;
    }
}

static int hwa_production_format_equal(const HWAFormat *left,
                                       const HWAFormat *right)
{
    return left->container == right->container &&
           left->encoding == right->encoding &&
           left->channels == right->channels &&
           left->sample_rate_hz == right->sample_rate_hz &&
           left->bits_per_sample == right->bits_per_sample &&
           left->valid_bits_per_sample == right->valid_bits_per_sample &&
           left->block_align == right->block_align &&
           left->channel_mask == right->channel_mask &&
           left->frames == right->frames &&
           left->data_bytes == right->data_bytes &&
           left->duration_seconds == right->duration_seconds;
}

static int hwa_production_context_ref_compare(const void *left,
                                              const void *right)
{
    const HWAProductionContextRef *a =
        (const HWAProductionContextRef *)left;
    const HWAProductionContextRef *b =
        (const HWAProductionContextRef *)right;
    return strcmp(a->context->item_key, b->context->item_key);
}

static int hwa_production_match_compare(const void *left, const void *right)
{
    const HWAProductionMatch *a = (const HWAProductionMatch *)left;
    const HWAProductionMatch *b = (const HWAProductionMatch *)right;
    int order;
    if (a->split != b->split) return a->split < b->split ? -1 : 1;
    order = strcmp(a->reference->item_key, b->reference->item_key);
    if (order != 0) return order;
    if (a->reference->item_kind != b->reference->item_kind) {
        return a->reference->item_kind < b->reference->item_kind ? -1 : 1;
    }
    order = strcmp(a->reference->item_role, b->reference->item_role);
    if (order != 0) return order;
    if (a->reference->item_id != b->reference->item_id) {
        return a->reference->item_id < b->reference->item_id ? -1 : 1;
    }
    if (a->model->item_id != b->model->item_id) {
        return a->model->item_id < b->model->item_id ? -1 : 1;
    }
    return 0;
}

static int hwa_production_ranked_match_compare(const void *left,
                                               const void *right)
{
    const HWAProductionRankedMatch *a =
        (const HWAProductionRankedMatch *)left;
    const HWAProductionRankedMatch *b =
        (const HWAProductionRankedMatch *)right;
    int order = memcmp(a->digest, b->digest, sizeof(a->digest));
    if (order != 0) return order;
    order = strcmp(a->item_key, b->item_key);
    if (order != 0) return order;
    if (a->match_index != b->match_index) {
        return a->match_index < b->match_index ? -1 : 1;
    }
    return 0;
}

static int hwa_production_select_matches(HWAProductionMatch *matches,
                                         size_t *match_count,
                                         char *error,
                                         size_t error_size)
{
    static const uint32_t families[] = {
        HWA_PRODUCTION_SPAN_EQ,
        HWA_PRODUCTION_SPAN_DYNAMICS,
        HWA_PRODUCTION_SPAN_STEREO,
        HWA_PRODUCTION_SPAN_DECAY
    };
    HWAProductionRankedMatch *ranked;
    size_t family;
    size_t output_count = 0U;
    size_t index;
    if (*match_count > SIZE_MAX / sizeof(*ranked)) {
        hwa_set_error(error, error_size, "Stage 6 sample rank overflows");
        return -1;
    }
    ranked = (HWAProductionRankedMatch *)malloc(
        *match_count * sizeof(*ranked));
    if (ranked == NULL) {
        hwa_set_error(error, error_size, "cannot allocate Stage 6 sample rank");
        return -1;
    }
    for (family = 0U; family < HWA_PRODUCTION_ARRAY_COUNT(families);
         ++family) {
        HWAProductionSplit split;
        for (split = HWA_PRODUCTION_TRAIN;
             split < HWA_PRODUCTION_SPLIT_COUNT;
             split = (HWAProductionSplit)((int)split + 1)) {
            size_t count = 0U;
            size_t keep = split == HWA_PRODUCTION_TRAIN ?
                HWA_PRODUCTION_MAX_TRAIN_PER_FAMILY :
                HWA_PRODUCTION_MAX_CHECK_PER_FAMILY;
            for (index = 0U; index < *match_count; ++index) {
                HWASha256 sha;
                if (matches[index].split != split ||
                    (matches[index].eligibility_flags & families[family]) == 0U) {
                    continue;
                }
                hwa_sha256_init(&sha);
                hwa_sha256_update(
                    &sha,
                    (const unsigned char *)matches[index].reference->item_key,
                    strlen(matches[index].reference->item_key));
                hwa_sha256_final(&sha, ranked[count].digest);
                ranked[count].match_index = index;
                ranked[count].item_key = matches[index].reference->item_key;
                count++;
            }
            qsort(ranked, count, sizeof(*ranked),
                  hwa_production_ranked_match_compare);
            if (keep > count) keep = count;
            for (index = 0U; index < keep; ++index) {
                matches[ranked[index].match_index].selected_flags |=
                    families[family];
            }
        }
    }
    for (index = 0U; index < *match_count; ++index) {
        if (matches[index].selected_flags != 0U) {
            matches[index].eligibility_flags = matches[index].selected_flags;
            matches[output_count++] = matches[index];
        }
    }
    free(ranked);
    if (output_count == 0U) {
        hwa_set_error(error, error_size, "Stage 6 sampling kept no spans");
        return -1;
    }
    *match_count = output_count;
    return 0;
}

static const HWAMeasureObservation *hwa_production_observation(
    const HWAMeasurementSet *set,
    uint64_t item_id,
    HWAMeasureKind kind,
    uint32_t index)
{
    size_t low = 0U;
    size_t high = set->measurement_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        if (set->measurements[middle].item_id < item_id) low = middle + 1U;
        else high = middle;
    }
    while (low < set->measurement_count &&
           set->measurements[low].item_id == item_id) {
        const HWAMeasureObservation *value = &set->measurements[low];
        if (value->view == HWA_MEASURE_VIEW_RAW && value->kind == kind &&
            value->index == index) return value;
        low++;
    }
    return NULL;
}

static int hwa_production_profile_metric(
    const HWAMeasurementSet *set,
    uint64_t item_id,
    HWAMeasureKind kind,
    uint32_t index,
    HWAProductionMetricValue *value)
{
    const HWAMeasureObservation *observation =
        hwa_production_observation(set, item_id, kind, index);
    memset(value, 0, sizeof(*value));
    if (observation == NULL || observation->status != HWA_MEASURE_STATUS_VALID ||
        !isfinite(observation->value) || !isfinite(observation->confidence) ||
        observation->confidence < 0.0 || observation->confidence > 1.0) {
        return 0;
    }
    value->value = observation->value == 0.0 ? 0.0 : observation->value;
    value->confidence = observation->confidence;
    value->evidence_flags = HWA_PRODUCTION_EVIDENCE_PROFILE;
    value->quality_flags =
        (observation->quality_flags & HWA_MEASURE_QUALITY_LOW_CONFIDENCE) != 0U
            ? HWA_PRODUCTION_QUALITY_LOW_COVERAGE : 0U;
    value->valid = 1;
    return 1;
}

static uint32_t hwa_production_match_eligibility(
    const HWAMeasurementSet *reference,
    const HWAMeasureItemContext *reference_context,
    const HWAMeasurementSet *model,
    const HWAMeasureItemContext *model_context)
{
    uint32_t flags = 0U;
    uint64_t reference_frames = reference_context->end_sample -
                                reference_context->start_sample;
    uint64_t model_frames = model_context->end_sample -
                            model_context->start_sample;
    double reference_seconds = (double)reference_frames /
        (double)reference->audio_format.sample_rate_hz;
    double model_seconds = (double)model_frames /
        (double)model->audio_format.sample_rate_hz;
    HWAProductionMetricValue a;
    HWAProductionMetricValue b;
    const HWAMeasureObservation *observation_a;
    const HWAMeasureObservation *observation_b;
    const uint32_t rejected_measure_quality =
        HWA_MEASURE_QUALITY_LOW_CONFIDENCE |
        HWA_MEASURE_QUALITY_NEAR_SPECTRAL_FLOOR;
    size_t band;
    int all_bands = 1;

    if (reference_context->excluded || model_context->excluded ||
        reference_context->item_kind != model_context->item_kind ||
        strcmp(reference_context->item_role,
               model_context->item_role) != 0 ||
        reference_frames == 0U || model_frames == 0U) return 0U;

    observation_a = hwa_production_observation(
        reference, reference_context->item_id, HWA_MEASURE_RMS_DBFS, 0U);
    observation_b = hwa_production_observation(
        model, model_context->item_id, HWA_MEASURE_RMS_DBFS, 0U);
    if (reference_context->item_kind == HWA_ITEM_BODY &&
        reference_seconds >= 0.25 && model_seconds >= 0.25 &&
        (reference_context->item_quality_flags &
         HWA_ITEM_QUALITY_LOW_CONFIDENCE) == 0U &&
        (model_context->item_quality_flags &
         HWA_ITEM_QUALITY_LOW_CONFIDENCE) == 0U &&
        observation_a != NULL && observation_b != NULL &&
        observation_a->status == HWA_MEASURE_STATUS_VALID &&
        observation_b->status == HWA_MEASURE_STATUS_VALID &&
        observation_a->value >= reference->options.spectral_floor_dbfs &&
        observation_b->value >= model->options.spectral_floor_dbfs &&
        (observation_a->quality_flags & rejected_measure_quality) == 0U &&
        (observation_b->quality_flags & rejected_measure_quality) == 0U &&
        hwa_production_profile_metric(
            reference, reference_context->item_id,
            HWA_MEASURE_FIXED_STATE_FRACTION, 0U, &a) &&
        a.value >= 0.70 &&
        hwa_production_profile_metric(
            model, model_context->item_id,
            HWA_MEASURE_FIXED_STATE_FRACTION, 0U, &b) &&
        b.value >= 0.70) {
        for (band = 0U; band < HWA_BAND_COUNT; ++band) {
            observation_a = hwa_production_observation(
                reference, reference_context->item_id,
                HWA_MEASURE_BAND_LEVEL_DBFS, (uint32_t)band);
            observation_b = hwa_production_observation(
                model, model_context->item_id,
                HWA_MEASURE_BAND_LEVEL_DBFS, (uint32_t)band);
            if (!hwa_production_profile_metric(
                    reference, reference_context->item_id,
                    HWA_MEASURE_BAND_LEVEL_DBFS, (uint32_t)band, &a) ||
                !hwa_production_profile_metric(
                    model, model_context->item_id,
                    HWA_MEASURE_BAND_LEVEL_DBFS, (uint32_t)band, &b) ||
                observation_a == NULL || observation_b == NULL ||
                a.value < reference->options.spectral_floor_dbfs ||
                b.value < model->options.spectral_floor_dbfs ||
                (observation_a->quality_flags & rejected_measure_quality) != 0U ||
                (observation_b->quality_flags & rejected_measure_quality) != 0U) {
                all_bands = 0;
                break;
            }
        }
        if (all_bands) flags |= HWA_PRODUCTION_SPAN_EQ;
    }
    if ((reference_context->item_kind == HWA_ITEM_NOTE ||
         reference_context->item_kind == HWA_ITEM_GESTURE) &&
        reference_seconds >= 1.0 && model_seconds >= 1.0 &&
        (reference_context->item_quality_flags &
         HWA_ITEM_QUALITY_LOW_CONFIDENCE) == 0U &&
        (model_context->item_quality_flags &
         HWA_ITEM_QUALITY_LOW_CONFIDENCE) == 0U &&
        hwa_production_profile_metric(
            reference, reference_context->item_id, HWA_MEASURE_RMS_DBFS,
            0U, &a) &&
        hwa_production_profile_metric(
            model, model_context->item_id, HWA_MEASURE_RMS_DBFS,
            0U, &b) && a.value >= reference->options.spectral_floor_dbfs &&
        b.value >= model->options.spectral_floor_dbfs &&
        observation_a != NULL && observation_b != NULL &&
        (observation_a->quality_flags & rejected_measure_quality) == 0U &&
        (observation_b->quality_flags & rejected_measure_quality) == 0U) {
        flags |= HWA_PRODUCTION_SPAN_DYNAMICS;
    }
    if ((reference_context->item_kind == HWA_ITEM_NOTE ||
         reference_context->item_kind == HWA_ITEM_GESTURE) &&
        reference->audio_format.channels >= 2U &&
        model->audio_format.channels >= 2U &&
        reference_seconds >= 1.0 && model_seconds >= 1.0 &&
        (reference_context->item_quality_flags &
         HWA_ITEM_QUALITY_LOW_CONFIDENCE) == 0U &&
        (model_context->item_quality_flags &
         HWA_ITEM_QUALITY_LOW_CONFIDENCE) == 0U &&
        hwa_production_profile_metric(
            reference, reference_context->item_id, HWA_MEASURE_RMS_DBFS,
            0U, &a) &&
        hwa_production_profile_metric(
            model, model_context->item_id, HWA_MEASURE_RMS_DBFS,
            0U, &b) && a.value >= reference->options.spectral_floor_dbfs &&
        b.value >= model->options.spectral_floor_dbfs &&
        observation_a != NULL && observation_b != NULL &&
        (observation_a->quality_flags & rejected_measure_quality) == 0U &&
        (observation_b->quality_flags & rejected_measure_quality) == 0U) {
        flags |= HWA_PRODUCTION_SPAN_STEREO;
    }
    if ((reference_context->item_kind == HWA_ITEM_RELEASE ||
         reference_context->item_kind == HWA_ITEM_RESIDUAL_TAIL) &&
        reference_seconds >= 0.05 && model_seconds >= 0.05 &&
        (reference_context->item_quality_flags &
         HWA_ITEM_QUALITY_LOW_CONFIDENCE) == 0U &&
        (model_context->item_quality_flags &
         HWA_ITEM_QUALITY_LOW_CONFIDENCE) == 0U &&
        hwa_production_profile_metric(
            reference, reference_context->item_id, HWA_MEASURE_RMS_DBFS,
            0U, &a) &&
        hwa_production_profile_metric(
            model, model_context->item_id, HWA_MEASURE_RMS_DBFS,
            0U, &b) && a.value >= reference->options.spectral_floor_dbfs &&
        b.value >= model->options.spectral_floor_dbfs &&
        observation_a != NULL && observation_b != NULL &&
        (observation_a->quality_flags & rejected_measure_quality) == 0U &&
        (observation_b->quality_flags & rejected_measure_quality) == 0U) {
        flags |= HWA_PRODUCTION_SPAN_DECAY;
    }
    return flags;
}

static int hwa_production_build_matches(
    const HWAMeasurementSet *reference,
    const HWAMeasurementSet *model,
    const HWAProductionOptions *options,
    HWAProductionMatch **matches_out,
    size_t *count_out,
    char *error,
    size_t error_size)
{
    HWAProductionContextRef *reference_refs = NULL;
    HWAProductionContextRef *model_refs = NULL;
    HWAProductionMatch *matches = NULL;
    size_t reference_index = 0U;
    size_t model_index = 0U;
    size_t match_count = 0U;
    size_t index;
    size_t train_count = 0U;
    size_t check_count = 0U;

    *matches_out = NULL;
    *count_out = 0U;
    if (reference->context_count > SIZE_MAX / sizeof(*reference_refs) ||
        model->context_count > SIZE_MAX / sizeof(*model_refs)) goto overflow;
    reference_refs = reference->context_count == 0U ? NULL :
        (HWAProductionContextRef *)malloc(reference->context_count *
                                           sizeof(*reference_refs));
    model_refs = model->context_count == 0U ? NULL :
        (HWAProductionContextRef *)malloc(model->context_count *
                                          sizeof(*model_refs));
    if ((reference->context_count != 0U && reference_refs == NULL) ||
        (model->context_count != 0U && model_refs == NULL)) goto memory;
    for (index = 0U; index < reference->context_count; ++index) {
        reference_refs[index].context = &reference->contexts[index];
    }
    for (index = 0U; index < model->context_count; ++index) {
        model_refs[index].context = &model->contexts[index];
    }
    qsort(reference_refs, reference->context_count, sizeof(*reference_refs),
          hwa_production_context_ref_compare);
    qsort(model_refs, model->context_count, sizeof(*model_refs),
          hwa_production_context_ref_compare);

    while (reference_index < reference->context_count &&
           model_index < model->context_count) {
        int order = strcmp(reference_refs[reference_index].context->item_key,
                           model_refs[model_index].context->item_key);
        if (order < 0) reference_index++;
        else if (order > 0) model_index++;
        else {
            uint32_t flags = hwa_production_match_eligibility(
                reference, reference_refs[reference_index].context,
                model, model_refs[model_index].context);
            if (flags != 0U) match_count++;
            reference_index++;
            model_index++;
        }
    }
    if (match_count == 0U || match_count > SIZE_MAX / sizeof(*matches)) {
        hwa_set_error(error, error_size,
                      match_count == 0U ?
                          "no eligible matched Stage 4 item spans" :
                          "matched Stage 6 candidate count overflows");
        goto cleanup;
    }
    matches = (HWAProductionMatch *)calloc(match_count, sizeof(*matches));
    if (matches == NULL) goto memory;
    reference_index = 0U;
    model_index = 0U;
    match_count = 0U;
    while (reference_index < reference->context_count &&
           model_index < model->context_count) {
        const HWAMeasureItemContext *a =
            reference_refs[reference_index].context;
        const HWAMeasureItemContext *b = model_refs[model_index].context;
        int order = strcmp(a->item_key, b->item_key);
        if (order < 0) reference_index++;
        else if (order > 0) model_index++;
        else {
            uint32_t flags = hwa_production_match_eligibility(
                reference, a, model, b);
            if (flags != 0U) {
                HWAProductionMatch *match = &matches[match_count++];
                match->reference = a;
                match->model = b;
                match->eligibility_flags = flags;
                if (hwa_production_split_for_item_key(
                        a->item_key, &match->split) != 0) {
                    hwa_set_error(error, error_size,
                                  "matched item key lacks a source event: %s",
                                  a->item_key);
                    goto cleanup;
                }
                if (match->split == HWA_PRODUCTION_TRAIN) train_count++;
                else check_count++;
            }
            reference_index++;
            model_index++;
        }
    }
    if (train_count == 0U || check_count == 0U) {
        hwa_set_error(error, error_size,
                      "deterministic split needs both train and check source events");
        goto cleanup;
    }
    if (hwa_production_select_matches(
            matches, &match_count, error, error_size) != 0) goto cleanup;
    if (match_count > options->max_spans) {
        hwa_set_error(error, error_size,
                      "sampled Stage 6 span union exceeds its cap");
        goto cleanup;
    }
    qsort(matches, match_count, sizeof(*matches),
          hwa_production_match_compare);
    free(reference_refs);
    free(model_refs);
    *matches_out = matches;
    *count_out = match_count;
    return 0;

overflow:
    hwa_set_error(error, error_size, "Stage 6 context index overflows");
    goto cleanup;
memory:
    hwa_set_error(error, error_size, "cannot allocate Stage 6 context match work");
cleanup:
    free(reference_refs);
    free(model_refs);
    free(matches);
    return -1;
}

static void hwa_production_fill_profile_values(
    const HWAMeasurementSet *set,
    uint64_t item_id,
    HWAProductionMetricValue values[HWA_PRODUCTION_METRICS_PER_VIEW])
{
    size_t band;
    memset(values, 0,
           HWA_PRODUCTION_METRICS_PER_VIEW * sizeof(*values));
    (void)hwa_production_profile_metric(
        set, item_id, HWA_MEASURE_RMS_DBFS, 0U, &values[0]);
    (void)hwa_production_profile_metric(
        set, item_id, HWA_MEASURE_CREST_DB, 0U, &values[1]);
    for (band = 0U; band < HWA_BAND_COUNT; ++band) {
        (void)hwa_production_profile_metric(
            set, item_id, HWA_MEASURE_BAND_LEVEL_DBFS, (uint32_t)band,
            &values[2U + band]);
    }
    (void)hwa_production_profile_metric(
        set, item_id, HWA_MEASURE_LEVEL_MODULATION_SPREAD_DB, 0U,
        &values[2U + HWA_BAND_COUNT]);
}

static int hwa_production_timeline_compare(const void *left,
                                           const void *right)
{
    const HWAProductionTimeline *a = (const HWAProductionTimeline *)left;
    const HWAProductionTimeline *b = (const HWAProductionTimeline *)right;
    if (a->start != b->start) return a->start < b->start ? -1 : 1;
    if (a->end != b->end) return a->end < b->end ? -1 : 1;
    if (a->span_index != b->span_index) {
        return a->span_index < b->span_index ? -1 : 1;
    }
    return 0;
}

static size_t hwa_production_fraction_bin64(uint64_t numerator,
                                            uint64_t denominator)
{
    size_t result = 0U;
    unsigned bit;
    if (denominator == 0U || numerator >= denominator) return 63U;
    for (bit = 0U; bit < 6U; ++bit) {
        result *= 2U;
        if (numerator >= denominator - numerator) {
            numerator -= denominator - numerator;
            result++;
        } else {
            numerator += numerator;
        }
    }
    return result > 63U ? 63U : result;
}

static double hwa_production_clamp(double value, double minimum,
                                   double maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static void hwa_production_set_audio_metric(HWAProductionMetricValue *value,
                                            double number,
                                            double confidence,
                                            uint32_t evidence,
                                            uint32_t quality)
{
    if (!isfinite(number) || !isfinite(confidence)) return;
    memset(value, 0, sizeof(*value));
    value->value = number == 0.0 ? 0.0 : number;
    value->confidence = hwa_production_clamp(confidence, 0.0, 1.0);
    value->evidence_flags = evidence;
    value->quality_flags = quality;
    value->valid = 1;
}

static uint64_t hwa_production_delay_anchor(
    const HWAProductionAudioAccum *accum,
    size_t index)
{
    uint64_t start = (uint64_t)accum->delay_max_lag;
    uint64_t distance;
    uint64_t denominator;
    uint64_t quotient;
    uint64_t remainder;
    if (accum->delay_anchor_count <= 1U) return start;
    distance = accum->delay_interior_length - 1U;
    denominator = (uint64_t)(accum->delay_anchor_count - 1U);
    quotient = distance / denominator;
    remainder = distance % denominator;
    return start + (uint64_t)index * quotient +
        ((uint64_t)index * remainder) / denominator;
}

static size_t hwa_production_delay_anchor_lower_bound(
    const HWAProductionAudioAccum *accum,
    size_t high,
    uint64_t value)
{
    size_t low = 0U;
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        if (hwa_production_delay_anchor(accum, middle) < value) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    return low;
}

static int hwa_production_delay_add_pair(
    HWAProductionAudioAccum *accum,
    ptrdiff_t lag,
    double left,
    double right,
    uint64_t *evaluations,
    uint64_t evaluation_limit,
    char *error,
    size_t error_size)
{
    size_t slot = (size_t)(lag + (ptrdiff_t)accum->delay_max_lag);
    long double *sums = &accum->delay_sums[slot * 5U];
    if (*evaluations == evaluation_limit) {
        hwa_set_error(error, error_size,
                      "stereo delay search exceeds the evaluation cap");
        return -1;
    }
    (*evaluations)++;
    sums[0] += (long double)left;
    sums[1] += (long double)right;
    sums[2] += (long double)left * (long double)left;
    sums[3] += (long double)right * (long double)right;
    sums[4] += (long double)left * (long double)right;
    accum->delay_counts[slot]++;
    return 0;
}

static int hwa_production_delay_observe(
    HWAProductionAudioAccum *accum,
    uint64_t relative,
    double left,
    double right,
    uint64_t *evaluations,
    uint64_t evaluation_limit,
    char *error,
    size_t error_size)
{
    size_t anchor_index;
    size_t low;
    size_t high;
    if (accum->delay_anchor_count == 0U) return 0;
    accum->delay_right_ring[
        (size_t)(relative % (uint64_t)accum->delay_ring_size)] = right;
    if (accum->delay_next_anchor < accum->delay_anchor_count &&
        relative == hwa_production_delay_anchor(
                        accum, accum->delay_next_anchor)) {
        ptrdiff_t lag;
        anchor_index = accum->delay_next_anchor++;
        accum->delay_anchor_left[anchor_index] = left;
        for (lag = -(ptrdiff_t)accum->delay_max_lag; lag <= 0; ++lag) {
            uint64_t right_position = lag < 0 ?
                relative - (uint64_t)(-lag) : relative;
            double paired_right = accum->delay_right_ring[
                (size_t)(right_position %
                         (uint64_t)accum->delay_ring_size)];
            if (hwa_production_delay_add_pair(
                    accum, lag, left, paired_right, evaluations,
                    evaluation_limit, error, error_size) != 0) return -1;
        }
    }
    if (accum->delay_next_anchor == 0U || relative == 0U) return 0;
    low = relative > (uint64_t)accum->delay_max_lag ?
        hwa_production_delay_anchor_lower_bound(
            accum, accum->delay_next_anchor,
            relative - (uint64_t)accum->delay_max_lag) : 0U;
    high = hwa_production_delay_anchor_lower_bound(
        accum, accum->delay_next_anchor, relative);
    for (anchor_index = low; anchor_index < high; ++anchor_index) {
        uint64_t anchor = hwa_production_delay_anchor(accum, anchor_index);
        ptrdiff_t lag = (ptrdiff_t)(relative - anchor);
        if (lag > 0 && lag <= (ptrdiff_t)accum->delay_max_lag &&
            hwa_production_delay_add_pair(
                accum, lag, accum->delay_anchor_left[anchor_index], right,
                evaluations, evaluation_limit, error, error_size) != 0) {
            return -1;
        }
    }
    return 0;
}

static int hwa_production_finish_delay(
    HWAProductionAudioAccum *accum,
    HWAProductionMetricValue *value,
    uint64_t *evaluations,
    uint64_t evaluation_limit,
    char *error,
    size_t error_size)
{
    ptrdiff_t best_lag = 0;
    double best_correlation = 0.0;
    ptrdiff_t lag;
    (void)evaluations;
    (void)evaluation_limit;
    (void)error;
    (void)error_size;
    if (accum->delay_anchor_count < 64U) return 0;
    for (lag = -(ptrdiff_t)accum->delay_max_lag;
         lag <= (ptrdiff_t)accum->delay_max_lag; ++lag) {
        size_t slot = (size_t)(lag + (ptrdiff_t)accum->delay_max_lag);
        size_t count = accum->delay_counts[slot];
        const long double *sums = &accum->delay_sums[slot * 5U];
        long double covariance;
        long double variance_left;
        long double variance_right;
        double correlation;
        if (count < 64U) continue;
        covariance = sums[4] - sums[0] * sums[1] / (long double)count;
        variance_left = sums[2] - sums[0] * sums[0] / (long double)count;
        variance_right = sums[3] - sums[1] * sums[1] / (long double)count;
        if (variance_left <= LDBL_MIN || variance_right <= LDBL_MIN) continue;
        correlation = (double)((covariance / sqrtl(variance_left)) /
                               sqrtl(variance_right));
        if (!isfinite(correlation)) continue;
        if (fabs(correlation) > fabs(best_correlation) ||
            (fabs(correlation) == fabs(best_correlation) &&
             (labs((long)lag) < labs((long)best_lag) ||
              (labs((long)lag) == labs((long)best_lag) && lag < best_lag)))) {
            best_correlation = correlation;
            best_lag = lag;
        }
    }
    if (best_correlation == 0.0) return 0;
    hwa_production_set_audio_metric(
        value, (double)best_lag, fabs(best_correlation),
        HWA_PRODUCTION_EVIDENCE_SAMPLES | HWA_PRODUCTION_EVIDENCE_STEREO,
        0U);
    return 0;
}

static int hwa_production_finish_audio_accum(
    HWAProductionAudioAccum *accum,
    uint32_t sample_rate,
    int stereo,
    int decay,
    HWAProductionMetricValue values[HWA_PRODUCTION_METRICS_PER_VIEW],
    uint64_t *evaluations,
    uint64_t evaluation_limit,
    char *error,
    size_t error_size)
{
    size_t band;
    if (accum->count != 0U && accum->sum_squares > LDBL_MIN) {
        double rms = sqrt((double)(accum->sum_squares /
                                  (long double)accum->count));
        double peak = (double)accum->peak;
        hwa_production_set_audio_metric(
            &values[0], 20.0 * log10(rms), 1.0,
            HWA_PRODUCTION_EVIDENCE_SAMPLES, 0U);
        if (peak > 0.0) {
            hwa_production_set_audio_metric(
                &values[1], 20.0 * log10(peak / rms), 1.0,
                HWA_PRODUCTION_EVIDENCE_SAMPLES, 0U);
        }
    }
    if (stereo && accum->count >= 3U) {
        long double count = (long double)accum->count;
        long double covariance = accum->left_right -
            accum->left_sum * accum->right_sum / count;
        long double left_variance = accum->left_squares -
            accum->left_sum * accum->left_sum / count;
        long double right_variance = accum->right_squares -
            accum->right_sum * accum->right_sum / count;
        if (accum->left_squares > LDBL_MIN &&
            accum->right_squares > LDBL_MIN) {
            double balance = 10.0 * log10(
                (double)(accum->right_squares / accum->left_squares));
            hwa_production_set_audio_metric(
                &values[3U + HWA_BAND_COUNT],
                hwa_production_clamp(balance, -24.0, 24.0), 1.0,
                HWA_PRODUCTION_EVIDENCE_SAMPLES |
                    HWA_PRODUCTION_EVIDENCE_STEREO,
                balance < -24.0 || balance > 24.0 ?
                    HWA_PRODUCTION_QUALITY_FIT_AT_BOUND : 0U);
        }
        if (accum->mid_squares > LDBL_MIN) {
            double width = sqrt((double)(accum->side_squares /
                                         accum->mid_squares));
            hwa_production_set_audio_metric(
                &values[4U + HWA_BAND_COUNT],
                hwa_production_clamp(width, 0.0, 4.0), 1.0,
                HWA_PRODUCTION_EVIDENCE_SAMPLES |
                    HWA_PRODUCTION_EVIDENCE_STEREO,
                width > 4.0 ? HWA_PRODUCTION_QUALITY_FIT_AT_BOUND : 0U);
        }
        if (left_variance > LDBL_MIN && right_variance > LDBL_MIN) {
            double correlation = (double)((covariance /
                sqrtl(left_variance)) / sqrtl(right_variance));
            hwa_production_set_audio_metric(
                &values[5U + HWA_BAND_COUNT],
                hwa_production_clamp(correlation, -1.0, 1.0),
                fabs(correlation),
                HWA_PRODUCTION_EVIDENCE_SAMPLES |
                    HWA_PRODUCTION_EVIDENCE_STEREO,
                0U);
        }
        if (hwa_production_finish_delay(
                accum, &values[6U + HWA_BAND_COUNT], evaluations,
                evaluation_limit, error, error_size) != 0) return -1;
    }
    if (!decay) return 0;
    for (band = 0U; band < HWA_PRODUCTION_ROOM_BAND_COUNT; ++band) {
        double direct;
        double early;
        double duration;
        long double cumulative[64U];
        size_t bin;
        size_t regression_count = 0U;
        long double sum_x = 0.0L;
        long double sum_y = 0.0L;
        long double sum_xx = 0.0L;
        long double sum_xy = 0.0L;
        long double sum_yy = 0.0L;
        if (!hwa_production_room_band_supported(
                (uint32_t)band, sample_rate)) continue;
        direct = (double)accum->direct_energy[band];
        early = (double)accum->early_energy[band];
        if (direct > DBL_MIN && early > DBL_MIN) {
            hwa_production_set_audio_metric(
                &values[7U + HWA_BAND_COUNT + band],
                hwa_production_clamp(10.0 * log10(early / direct),
                                     -120.0, 60.0),
                0.6, HWA_PRODUCTION_EVIDENCE_SAMPLES |
                         HWA_PRODUCTION_EVIDENCE_ENVELOPE |
                         HWA_PRODUCTION_EVIDENCE_SPECTRUM,
                HWA_PRODUCTION_QUALITY_SOURCE_CONFOUNDED);
        }
        duration = (double)accum->count / (double)sample_rate;
        cumulative[63U] = accum->decay_energy[band][63U];
        for (bin = 63U; bin-- > 0U;) {
            cumulative[bin] = cumulative[bin + 1U] +
                              accum->decay_energy[band][bin];
        }
        if (duration > 0.0 && cumulative[0] > LDBL_MIN) {
            for (bin = 0U; bin < 64U; ++bin) {
                double db = 10.0 * log10(
                    (double)(cumulative[bin] / cumulative[0]));
                if (db <= -5.0 && db >= -25.0) {
                    long double x = (long double)duration *
                        ((long double)bin + 0.5L) / 64.0L;
                    long double y = db;
                    sum_x += x;
                    sum_y += y;
                    sum_xx += x * x;
                    sum_xy += x * y;
                    sum_yy += y * y;
                    regression_count++;
                }
            }
        }
        if (regression_count >= 4U) {
            long double n = (long double)regression_count;
            long double denominator = n * sum_xx - sum_x * sum_x;
            long double y_denominator = n * sum_yy - sum_y * sum_y;
            if (denominator > LDBL_MIN && y_denominator > LDBL_MIN) {
                long double numerator = n * sum_xy - sum_x * sum_y;
                double slope = (double)(numerator / denominator);
                double r2 = (double)((numerator * numerator) /
                    (denominator * y_denominator));
                if (slope < 0.0 && r2 >= 0.8) {
                    double decay_seconds = -60.0 / slope;
                    hwa_production_set_audio_metric(
                        &values[7U + HWA_BAND_COUNT +
                                HWA_PRODUCTION_ROOM_BAND_COUNT + band],
                        hwa_production_clamp(decay_seconds, 0.0, 30.0), r2,
                        HWA_PRODUCTION_EVIDENCE_SAMPLES |
                            HWA_PRODUCTION_EVIDENCE_ENVELOPE |
                            HWA_PRODUCTION_EVIDENCE_SPECTRUM,
                        HWA_PRODUCTION_QUALITY_SOURCE_CONFOUNDED);
                }
            }
        }
    }
    return 0;
}

static int hwa_production_stream_audio(
    const char *path,
    const HWAFormat *expected_format,
    const char expected_sha256[HWA_SHA256_HEX_SIZE],
    const HWAProductionOptions *options,
    const HWAProductionSpan *spans,
    size_t span_count,
    int reference_side,
    HWAProductionSpanValues *span_values,
    HWAFormat *format_out,
    char sha256_out[HWA_SHA256_HEX_SIZE],
    uint64_t *evaluations,
    uint64_t live_base,
    uint64_t *temporary_bytes,
    size_t *envelope_point_count,
    char *error,
    size_t error_size)
{
    static const double room_edges[HWA_PRODUCTION_ROOM_BAND_COUNT + 1U] = {
        125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0
    };
    HWAWavReader reader;
    HWAProductionTimeline *timeline = NULL;
    HWAProductionAudioAccum *accums = NULL;
    size_t *active = NULL;
    unsigned char *buffer = NULL;
    size_t buffer_bytes;
    size_t timeline_next = 0U;
    size_t active_count = 0U;
    uint64_t frame = 0U;
    uint64_t live = live_base;
    uint64_t bytes;
    double low_alpha[HWA_PRODUCTION_ROOM_BAND_COUNT];
    double high_alpha[HWA_PRODUCTION_ROOM_BAND_COUNT];
    long double safe_sample;
    char before_hash[HWA_SHA256_HEX_SIZE];
    char after_hash[HWA_SHA256_HEX_SIZE];
    HWAProductionFileIdentity path_identity;
    HWAProductionFileIdentity open_identity;
    HWAProductionFileIdentity final_stream_identity;
    HWAProductionFileIdentity final_path_identity;
    size_t index;
    int status = -1;

    memset(&reader, 0, sizeof(reader));
    if (temporary_bytes == NULL || envelope_point_count == NULL ||
        live > options->max_work_bytes ||
        *temporary_bytes > options->max_work_bytes - live) {
        hwa_set_error(error, error_size, "invalid Stage 6 audio work ledger");
        goto cleanup;
    }
    live += *temporary_bytes;
    if (hwa_production_path_identity(
            path, &path_identity, error, error_size) != 0 ||
        hwa_wav_reader_open(&reader, path, options->max_input_bytes,
                            error, error_size) != 0 ||
        hwa_production_stream_identity(reader.file, &open_identity) != 0 ||
        !hwa_production_identity_equal(&path_identity, &open_identity) ||
        hwa_production_hash_stream(
            reader.file, options->max_input_bytes, before_hash,
            error, error_size) != 0) goto cleanup;
    if (strcmp(before_hash, expected_sha256) != 0) {
        hwa_set_error(error, error_size,
                      "explicit Stage 6 audio hash does not match its profile");
        goto cleanup;
    }
    if (hwa_production_seek(reader.file, reader.data_offset) != 0) {
        hwa_set_error(error, error_size,
                      "cannot seek to Stage 6 WAVE sample data");
        goto cleanup;
    }
    reader.bytes_remaining = reader.format.data_bytes;
    if (reader.format.frames > options->max_input_frames ||
        !hwa_production_format_equal(&reader.format, expected_format)) {
        hwa_set_error(error, error_size,
                      "explicit Stage 6 audio shape does not match its profile");
        goto cleanup;
    }
    *format_out = reader.format;
    memcpy(sha256_out, before_hash, HWA_SHA256_HEX_SIZE);
    safe_sample = sqrtl(
        (long double)DBL_MAX /
        ((long double)options->max_input_frames * 64.0L)) /
        (long double)reader.format.channels;
    if (reader.format.block_align == 0U ||
        options->decode_block_frames > SIZE_MAX / reader.format.block_align) {
        hwa_set_error(error, error_size, "Stage 6 decode buffer overflows");
        goto cleanup;
    }
    buffer_bytes = options->decode_block_frames * reader.format.block_align;
    if (hwa_production_array_bytes(span_count, sizeof(*timeline), &bytes) != 0 ||
        hwa_production_add_size(&live, bytes, options->max_work_bytes) != 0) {
        hwa_set_error(error, error_size, "Stage 6 timeline exceeds work cap");
        goto cleanup;
    }
    timeline = (HWAProductionTimeline *)malloc((size_t)bytes);
    if (hwa_production_array_bytes(span_count, sizeof(*accums), &bytes) != 0 ||
        hwa_production_add_size(&live, bytes, options->max_work_bytes) != 0) {
        hwa_set_error(error, error_size, "Stage 6 audio facts exceed work cap");
        goto cleanup;
    }
    accums = (HWAProductionAudioAccum *)calloc(span_count, sizeof(*accums));
    if (hwa_production_array_bytes(span_count, sizeof(*active), &bytes) != 0 ||
        hwa_production_add_size(&live, bytes, options->max_work_bytes) != 0 ||
        hwa_production_add_size(&live, (uint64_t)buffer_bytes,
                                options->max_work_bytes) != 0) {
        hwa_set_error(error, error_size, "Stage 6 active-span work exceeds cap");
        goto cleanup;
    }
    active = (size_t *)malloc(span_count * sizeof(*active));
    buffer = (unsigned char *)malloc(buffer_bytes);
    if (timeline == NULL || accums == NULL || active == NULL || buffer == NULL) {
        hwa_set_error(error, error_size, "cannot allocate Stage 6 audio work");
        goto cleanup;
    }
    for (index = 0U; index < span_count; ++index) {
        timeline[index].start = reference_side ?
            spans[index].reference_start_sample : spans[index].model_start_sample;
        timeline[index].end = reference_side ?
            spans[index].reference_end_sample : spans[index].model_end_sample;
        timeline[index].span_index = index;
    }
    qsort(timeline, span_count, sizeof(*timeline),
          hwa_production_timeline_compare);
    for (index = 0U; index < HWA_PRODUCTION_ROOM_BAND_COUNT; ++index) {
        low_alpha[index] = 1.0 - exp(
            -2.0 * 3.14159265358979323846 * room_edges[index] /
            (double)reader.format.sample_rate_hz);
        high_alpha[index] = 1.0 - exp(
            -2.0 * 3.14159265358979323846 * room_edges[index + 1U] /
            (double)reader.format.sample_rate_hz);
    }

    while (frame < reader.format.frames) {
        size_t frames_read = 0U;
        size_t local_frame;
        if (hwa_wav_reader_read_frames(&reader, buffer,
                                       options->decode_block_frames,
                                       &frames_read, error, error_size) != 0 ||
            frames_read == 0U) {
            if (error != NULL && error_size > 0U && error[0] == '\0') {
                hwa_set_error(error, error_size,
                              "short read in explicit Stage 6 audio");
            }
            goto cleanup;
        }
        for (local_frame = 0U; local_frame < frames_read;
             ++local_frame, ++frame) {
            const unsigned char *frame_bytes = buffer +
                local_frame * reader.format.block_align;
            long double mono_sum = 0.0L;
            double mono;
            double left = 0.0;
            double right = 0.0;
            size_t channel;
            size_t active_index;
            while (timeline_next < span_count &&
                   timeline[timeline_next].start <= frame) {
                size_t span_index = timeline[timeline_next].span_index;
                HWAProductionAudioAccum *accum = &accums[span_index];
                uint64_t span_frames = timeline[timeline_next].end -
                                       timeline[timeline_next].start;
                if ((spans[span_index].eligibility_flags &
                     HWA_PRODUCTION_SPAN_DYNAMICS) != 0U) {
                    HWAProductionEnvelope *envelope = reference_side ?
                        &span_values[span_index].reference_envelope :
                        &span_values[span_index].model_envelope;
                    size_t window = reader.format.sample_rate_hz / 10U;
                    size_t hop = reader.format.sample_rate_hz / 20U;
                    size_t capacity = 0U;
                    uint64_t ring_bytes;
                    uint64_t point_bytes;
                    if (window == 0U) window = 1U;
                    if (hop == 0U) hop = 1U;
                    if (span_frames >= (uint64_t)window) {
                        uint64_t quotient =
                            (span_frames - (uint64_t)window) /
                            (uint64_t)hop;
                        if (quotient > (uint64_t)SIZE_MAX - 1U ||
                            quotient + 1U >
                                (uint64_t)options->max_envelope_points) {
                            hwa_set_error(
                                error, error_size,
                                "dynamics envelope point count overflows");
                            goto cleanup;
                        }
                        capacity = 1U + (size_t)quotient;
                    }
                    if (*envelope_point_count >
                            options->max_envelope_points ||
                        capacity == 0U || capacity >
                        options->max_envelope_points - *envelope_point_count ||
                        hwa_production_array_bytes(
                            window, sizeof(double), &ring_bytes) != 0 ||
                        hwa_production_array_bytes(
                            capacity, sizeof(double), &point_bytes) != 0 ||
                        hwa_production_add_size(
                            &live, ring_bytes, options->max_work_bytes) != 0 ||
                        hwa_production_add_size(
                            &live, point_bytes, options->max_work_bytes) != 0) {
                        hwa_set_error(error, error_size,
                                      "dynamics envelope exceeds Stage 6 caps");
                        goto cleanup;
                    }
                    accum->envelope_ring =
                        (double *)calloc(window, sizeof(double));
                    envelope->points =
                        (double *)malloc(capacity * sizeof(double));
                    if (accum->envelope_ring == NULL ||
                        envelope->points == NULL) {
                        hwa_set_error(error, error_size,
                                      "cannot allocate dynamics envelope work");
                        goto cleanup;
                    }
                    accum->envelope_window = window;
                    accum->envelope_hop = hop;
                    accum->envelope = envelope;
                    envelope->capacity = capacity;
                    *envelope_point_count += capacity;
                    *temporary_bytes += point_bytes;
                }
                if ((spans[span_index].eligibility_flags &
                     HWA_PRODUCTION_SPAN_STEREO) != 0U) {
                    size_t max_lag =
                        ((size_t)reader.format.sample_rate_hz *
                         HWA_PRODUCTION_DELAY_MILLISECONDS) / 1000U;
                    size_t lag_count = max_lag * 2U + 1U;
                    uint64_t interior = span_frames >
                        (uint64_t)max_lag * 2U ?
                        span_frames - (uint64_t)max_lag * 2U : 0U;
                    size_t anchors = interior < 256U ? (size_t)interior : 256U;
                    uint64_t delay_bytes = 0U;
                    uint64_t part;
                    if (max_lag > 1536U || anchors == 0U ||
                        hwa_production_array_bytes(
                            lag_count, 5U * sizeof(long double), &part) != 0 ||
                        hwa_production_add_size(
                            &delay_bytes, part, UINT64_MAX) != 0 ||
                        hwa_production_array_bytes(
                            lag_count, sizeof(uint32_t), &part) != 0 ||
                        hwa_production_add_size(
                            &delay_bytes, part, UINT64_MAX) != 0 ||
                        hwa_production_array_bytes(
                            anchors, sizeof(double), &part) != 0 ||
                        hwa_production_add_size(
                            &delay_bytes, part, UINT64_MAX) != 0 ||
                        hwa_production_array_bytes(
                            max_lag + 1U, sizeof(double), &part) != 0 ||
                        hwa_production_add_size(
                            &delay_bytes, part, UINT64_MAX) != 0 ||
                        hwa_production_add_size(
                            &live, delay_bytes,
                            options->max_work_bytes) != 0) {
                        hwa_set_error(error, error_size,
                                      "stereo delay work exceeds Stage 6 cap");
                        goto cleanup;
                    }
                    accum->delay_sums = (long double *)calloc(
                        lag_count * 5U, sizeof(long double));
                    accum->delay_counts = (uint32_t *)calloc(
                        lag_count, sizeof(uint32_t));
                    accum->delay_anchor_left = (double *)malloc(
                        anchors * sizeof(double));
                    accum->delay_right_ring = (double *)calloc(
                        max_lag + 1U, sizeof(double));
                    if (accum->delay_sums == NULL ||
                        accum->delay_counts == NULL ||
                        accum->delay_anchor_left == NULL ||
                        accum->delay_right_ring == NULL) {
                        hwa_set_error(error, error_size,
                                      "cannot allocate stereo delay work");
                        goto cleanup;
                    }
                    accum->delay_anchor_count = anchors;
                    accum->delay_max_lag = max_lag;
                    accum->delay_ring_size = max_lag + 1U;
                    accum->delay_interior_length = interior;
                    accum->delay_work_bytes = delay_bytes;
                }
                active[active_count++] = span_index;
                timeline_next++;
            }
            for (channel = 0U; channel < reader.format.channels; ++channel) {
                int clipped;
                double sample = hwa_wav_decode_sample(
                    &reader, frame_bytes + channel * reader.bytes_per_sample,
                    &clipped);
                (void)clipped;
                if (!isfinite(sample) ||
                    fabsl((long double)sample) > safe_sample) {
                    hwa_set_error(error, error_size,
                                  "Stage 6 audio sample exceeds safe numeric range");
                    goto cleanup;
                }
                mono_sum += (long double)sample;
                if (channel == 0U) left = sample;
                if (channel == 1U) right = sample;
            }
            mono_sum /= (long double)reader.format.channels;
            if (!isfinite(mono_sum) || fabsl(mono_sum) > DBL_MAX) {
                hwa_set_error(error, error_size,
                              "Stage 6 channel sum exceeds safe numeric range");
                goto cleanup;
            }
            mono = (double)mono_sum;
            active_index = 0U;
            while (active_index < active_count) {
                size_t span_index = active[active_index];
                HWAProductionAudioAccum *accum = &accums[span_index];
                uint64_t start = reference_side ?
                    spans[span_index].reference_start_sample :
                    spans[span_index].model_start_sample;
                uint64_t end = reference_side ?
                    spans[span_index].reference_end_sample :
                    spans[span_index].model_end_sample;
                uint64_t relative;
                uint64_t length;
                size_t band;
                if (frame >= end) {
                    HWAProductionMetricValue *values = reference_side ?
                        span_values[span_index].reference :
                        span_values[span_index].model;
                    uint64_t delay_bytes = accum->delay_work_bytes;
                    uint64_t ring_bytes =
                        (uint64_t)accum->envelope_window * sizeof(double);
                    if (hwa_production_finish_audio_accum(
                            accum, reader.format.sample_rate_hz,
                            reader.format.channels >= 2U,
                            (spans[span_index].eligibility_flags &
                             HWA_PRODUCTION_SPAN_DECAY) != 0U,
                            values, evaluations,
                            options->max_evaluations, error, error_size) != 0) {
                        goto cleanup;
                    }
                    free(accum->delay_sums);
                    free(accum->delay_counts);
                    free(accum->delay_anchor_left);
                    free(accum->delay_right_ring);
                    free(accum->envelope_ring);
                    accum->delay_sums = NULL;
                    accum->delay_counts = NULL;
                    accum->delay_anchor_left = NULL;
                    accum->delay_right_ring = NULL;
                    accum->envelope_ring = NULL;
                    if (delay_bytes <= live) live -= delay_bytes;
                    if (ring_bytes <= live) live -= ring_bytes;
                    active[active_index] = active[--active_count];
                    continue;
                }
                relative = frame - start;
                length = end - start;
                if (*evaluations == options->max_evaluations) {
                    hwa_set_error(error, error_size,
                                  "Stage 6 sample evaluation cap exceeded");
                    goto cleanup;
                }
                (*evaluations)++;
                accum->sum_squares += (long double)mono * mono;
                if (fabs(mono) > accum->peak) accum->peak = fabs(mono);
                accum->count++;
                if (accum->envelope_ring != NULL) {
                    double square = mono * mono;
                    if (!isfinite(square)) {
                        hwa_set_error(error, error_size,
                                      "finite Stage 6 samples overflow dynamics work");
                        goto cleanup;
                    }
                    if (accum->envelope_filled == accum->envelope_window) {
                        accum->envelope_sum_squares -=
                            accum->envelope_ring[accum->envelope_position];
                    } else {
                        accum->envelope_filled++;
                    }
                    accum->envelope_ring[accum->envelope_position] = square;
                    accum->envelope_sum_squares += square;
                    accum->envelope_position =
                        (accum->envelope_position + 1U) %
                        accum->envelope_window;
                    if (accum->envelope_filled == accum->envelope_window &&
                        (relative + 1U - accum->envelope_window) %
                            accum->envelope_hop == 0U &&
                        accum->envelope->count <
                            accum->envelope->capacity) {
                        double mean_square = (double)(
                            accum->envelope_sum_squares /
                            (long double)accum->envelope_window);
                        accum->envelope->points[accum->envelope->count++] =
                            mean_square > DBL_MIN ?
                                10.0 * log10(mean_square) : -300.0;
                    }
                }
                if (reader.format.channels >= 2U) {
                    long double mid =
                        0.5L * ((long double)left + (long double)right);
                    long double side =
                        0.5L * ((long double)left - (long double)right);
                    accum->left_sum += left;
                    accum->right_sum += right;
                    accum->left_squares += (long double)left * left;
                    accum->right_squares += (long double)right * right;
                    accum->left_right += (long double)left * right;
                    accum->mid_squares += mid * mid;
                    accum->side_squares += side * side;
                    if (hwa_production_delay_observe(
                            accum, relative, left, right, evaluations,
                            options->max_evaluations,
                            error, error_size) != 0) goto cleanup;
                }
                if ((spans[span_index].eligibility_flags &
                     HWA_PRODUCTION_SPAN_DECAY) != 0U) {
                    if ((uint64_t)HWA_PRODUCTION_ROOM_BAND_COUNT >
                        options->max_evaluations - *evaluations) {
                        hwa_set_error(error, error_size,
                                      "Stage 6 room-band evaluation cap exceeded");
                        goto cleanup;
                    }
                    *evaluations += HWA_PRODUCTION_ROOM_BAND_COUNT;
                    for (band = 0U; band < HWA_PRODUCTION_ROOM_BAND_COUNT;
                         ++band) {
                    double band_sample;
                    size_t decay_bin;
                    accum->low_state[band] += low_alpha[band] *
                        (mono - accum->low_state[band]);
                    accum->high_state[band] += high_alpha[band] *
                        (mono - accum->high_state[band]);
                    band_sample = accum->high_state[band] -
                                  accum->low_state[band];
                    if (relative < reader.format.sample_rate_hz / 100U) {
                        accum->direct_energy[band] +=
                            (long double)band_sample * band_sample;
                    } else if (relative <
                               (uint64_t)reader.format.sample_rate_hz * 8U /
                                   100U) {
                        accum->early_energy[band] +=
                            (long double)band_sample * band_sample;
                    }
                    decay_bin = hwa_production_fraction_bin64(
                        relative, length);
                    accum->decay_energy[band][decay_bin] +=
                        (long double)band_sample * band_sample;
                    }
                    if (relative < reader.format.sample_rate_hz / 100U) {
                        accum->direct_count++;
                    } else if (relative <
                               (uint64_t)reader.format.sample_rate_hz * 8U /
                                   100U) {
                        accum->early_count++;
                    }
                }
                active_index++;
            }
        }
    }
    while (active_count != 0U) {
        size_t span_index = active[--active_count];
        HWAProductionAudioAccum *accum = &accums[span_index];
        HWAProductionMetricValue *values = reference_side ?
            span_values[span_index].reference :
            span_values[span_index].model;
        if (hwa_production_finish_audio_accum(
                accum, reader.format.sample_rate_hz,
                reader.format.channels >= 2U,
                (spans[span_index].eligibility_flags &
                 HWA_PRODUCTION_SPAN_DECAY) != 0U,
                values, evaluations,
                options->max_evaluations, error, error_size) != 0) goto cleanup;
        free(accum->delay_sums);
        free(accum->delay_counts);
        free(accum->delay_anchor_left);
        free(accum->delay_right_ring);
        free(accum->envelope_ring);
        accum->delay_sums = NULL;
        accum->delay_counts = NULL;
        accum->delay_anchor_left = NULL;
        accum->delay_right_ring = NULL;
        accum->envelope_ring = NULL;
    }
    if (hwa_production_hash_stream(
            reader.file, options->max_input_bytes, after_hash,
            error, error_size) != 0 ||
        hwa_production_stream_identity(
            reader.file, &final_stream_identity) != 0 ||
        hwa_production_path_identity(
            path, &final_path_identity, error, error_size) != 0 ||
        !hwa_production_identity_equal(&open_identity,
                                       &final_stream_identity) ||
        !hwa_production_identity_equal(&open_identity,
                                       &final_path_identity)) goto cleanup;
    if (strcmp(before_hash, after_hash) != 0 ||
        strcmp(after_hash, expected_sha256) != 0) {
        hwa_set_error(error, error_size,
                      "Stage 6 audio changed while it was read");
        goto cleanup;
    }
    hwa_wav_reader_close(&reader);
    status = 0;

cleanup:
    hwa_wav_reader_close(&reader);
    if (accums != NULL) {
        for (index = 0U; index < span_count; ++index) {
            free(accums[index].delay_sums);
            free(accums[index].delay_counts);
            free(accums[index].delay_anchor_left);
            free(accums[index].delay_right_ring);
            free(accums[index].envelope_ring);
        }
    }
    free(timeline);
    free(accums);
    free(active);
    free(buffer);
    return status;
}

static int hwa_production_analyze_room_ir(
    const char *path,
    const HWAProductionOptions *options,
    HWAProductionRoomFacts *facts,
    HWAFormat *format_out,
    char sha256_out[HWA_SHA256_HEX_SIZE],
    uint64_t *evaluations,
    uint64_t live_base,
    char *error,
    size_t error_size)
{
    HWAWavReader reader;
    HWAProductionSpan span;
    HWAProductionSpanValues values;
    HWAProductionOptions ir_options = *options;
    HWAFormat expected_format;
    char expected_hash[HWA_SHA256_HEX_SIZE];
    uint64_t temporary_bytes = 0U;
    size_t envelope_points = 0U;
    size_t band;

    memset(facts, 0, sizeof(*facts));
    memset(&reader, 0, sizeof(reader));
    if (path == NULL) return 0;
    if (path[0] == '\0' || strcmp(path, "-") == 0) {
        hwa_set_error(error, error_size, "invalid explicit room IR path");
        return -1;
    }
    if (hwa_sha256_file(path, options->max_input_bytes, expected_hash,
                        error, error_size) != 0 ||
        hwa_wav_reader_open(&reader, path, options->max_input_bytes,
                            error, error_size) != 0) {
        hwa_wav_reader_close(&reader);
        return -1;
    }
    expected_format = reader.format;
    hwa_wav_reader_close(&reader);
    if (expected_format.frames == 0U ||
        expected_format.frames > options->max_ir_frames ||
        expected_format.frames > options->max_envelope_points) {
        hwa_set_error(error, error_size, "room IR exceeds its frame/point cap");
        return -1;
    }
    memset(&span, 0, sizeof(span));
    memset(&values, 0, sizeof(values));
    span.id = 1U;
    span.reference_start_sample = 0U;
    span.reference_end_sample = expected_format.frames;
    span.eligibility_flags = HWA_PRODUCTION_SPAN_DECAY;
    ir_options.max_input_frames = options->max_ir_frames;
    if (hwa_production_stream_audio(
            path, &expected_format, expected_hash, &ir_options, &span, 1U, 1,
            &values, format_out, sha256_out, evaluations, live_base,
            &temporary_bytes, &envelope_points, error, error_size) != 0) {
        return -1;
    }
    facts->supplied = 1;
    facts->point_count = (size_t)expected_format.frames;
    for (band = 0U; band < HWA_PRODUCTION_ROOM_BAND_COUNT; ++band) {
        size_t early = 7U + HWA_BAND_COUNT + band;
        size_t late = early + HWA_PRODUCTION_ROOM_BAND_COUNT;
        if (values.reference[early].valid) {
            facts->early_db[band] = values.reference[early].value;
            facts->early_valid[band] = 1;
        }
        if (values.reference[late].valid) {
            facts->late_seconds[band] = values.reference[late].value;
            facts->late_valid[band] = 1;
        }
    }
    return 0;
}

static int hwa_production_double_compare(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static double hwa_production_quantile(double *values, size_t count,
                                      double probability)
{
    double position;
    size_t lower;
    size_t upper;
    double fraction;
    qsort(values, count, sizeof(*values), hwa_production_double_compare);
    if (count == 1U) return values[0];
    position = (double)(count - 1U) * probability;
    lower = (size_t)floor(position);
    upper = (size_t)ceil(position);
    fraction = position - (double)lower;
    return values[lower] + (values[upper] - values[lower]) * fraction;
}

static double hwa_production_median_copy(const double *values,
                                         size_t count,
                                         double *work)
{
    memcpy(work, values, count * sizeof(*work));
    return hwa_production_quantile(work, count, 0.5);
}

static void hwa_production_fit_summary(const double *values,
                                       size_t count,
                                       double *work,
                                       double *estimate,
                                       double *q05,
                                       double *q95)
{
    *estimate = hwa_production_median_copy(values, count, work);
    memcpy(work, values, count * sizeof(*work));
    *q05 = hwa_production_quantile(work, count, 0.05);
    memcpy(work, values, count * sizeof(*work));
    *q95 = hwa_production_quantile(work, count, 0.95);
    if (*q05 > *estimate) *q05 = *estimate;
    if (*q95 < *estimate) *q95 = *estimate;
}

static HWAProductionFit *hwa_production_find_fit(
    HWAProductionResult *result,
    HWAProductionScope scope,
    HWAProductionFitKind kind,
    uint32_t index)
{
    size_t fit_index;
    for (fit_index = 0U; fit_index < result->fit_count; ++fit_index) {
        HWAProductionFit *fit = &result->fits[fit_index];
        if (fit->scope == scope && fit->kind == kind && fit->index == index) {
            return fit;
        }
    }
    return NULL;
}

static void hwa_production_fit_mark(HWAProductionFit *fit,
                                    HWAProductionAvailability availability,
                                    size_t span_count,
                                    size_t point_count,
                                    uint32_t quality_flags)
{
    if (availability == HWA_PRODUCTION_INSUFFICIENT &&
        span_count == 0U && point_count == 0U) {
        availability = HWA_PRODUCTION_UNAVAILABLE;
        quality_flags = 0U;
    }
    fit->availability = availability;
    fit->span_count = span_count;
    fit->point_count = point_count;
    fit->quality_flags = quality_flags;
}

static void hwa_production_fit_set(HWAProductionFit *fit,
                                   double estimate,
                                   double q05,
                                   double q95,
                                   size_t span_count,
                                   size_t point_count,
                                   uint32_t quality_flags)
{
    fit->availability = HWA_PRODUCTION_AVAILABLE;
    fit->estimate = estimate == 0.0 ? 0.0 : estimate;
    fit->q05 = q05 == 0.0 ? 0.0 : q05;
    fit->q95 = q95 == 0.0 ? 0.0 : q95;
    fit->span_count = span_count;
    fit->point_count = point_count;
    fit->quality_flags = quality_flags;
    fit->estimate_valid = 1;
    fit->uncertainty_valid = 1;
}

static void hwa_production_fit_set_without_uncertainty(
    HWAProductionFit *fit,
    double estimate,
    size_t span_count,
    size_t point_count,
    uint32_t quality_flags)
{
    fit->availability = HWA_PRODUCTION_AVAILABLE;
    fit->estimate = estimate == 0.0 ? 0.0 : estimate;
    fit->span_count = span_count;
    fit->point_count = point_count;
    fit->quality_flags = quality_flags;
    fit->estimate_valid = 1;
    fit->uncertainty_valid = 0;
}

static size_t hwa_production_family_count(
    const HWAProductionResult *result,
    HWAProductionSplit split,
    uint32_t flag)
{
    size_t count = 0U;
    size_t index;
    for (index = 0U; index < result->span_count; ++index) {
        if (result->spans[index].split == split &&
            (result->spans[index].eligibility_flags & flag) != 0U) count++;
    }
    return count;
}

typedef struct HWAProductionWarningRule {
    uint32_t family;
    const char *code;
    const char *message;
    uint64_t fit_id;
} HWAProductionWarningRule;

static const HWAProductionWarningRule hwa_production_warning_rules[] = {
    {HWA_PRODUCTION_SPAN_EQ, "low-eq-evidence",
     "EQ correction has fewer than 8 TRAIN spans or 2 CHECK spans.", 1U},
    {HWA_PRODUCTION_SPAN_DYNAMICS, "low-dynamics-evidence",
     "Dynamics correction has fewer than 8 TRAIN spans or 2 CHECK spans.",
     8U},
    {HWA_PRODUCTION_SPAN_STEREO, "low-stereo-evidence",
     "Stereo correction has fewer than 8 TRAIN spans or 2 CHECK spans.",
     11U},
    {HWA_PRODUCTION_SPAN_DECAY, "low-decay-evidence",
     "Decay facts have fewer than 8 TRAIN spans or 2 CHECK spans.", 21U}
};

static int hwa_production_room_ir_facts_available(
    const HWAProductionResult *result)
{
    size_t index;
    for (index = 0U; index < result->fit_count; ++index) {
        if (result->fits[index].scope == HWA_PRODUCTION_SCOPE_ROOM_IR &&
            result->fits[index].availability == HWA_PRODUCTION_AVAILABLE) {
            return 1;
        }
    }
    return 0;
}

size_t hwa_production_warning_spec_count(
    const HWAProductionResult *result)
{
    size_t count = 0U;
    size_t index;
    if (result == NULL) return 0U;
    for (index = 0U;
         index < HWA_PRODUCTION_ARRAY_COUNT(hwa_production_warning_rules);
         ++index) {
        const HWAProductionWarningRule *rule =
            &hwa_production_warning_rules[index];
        if (hwa_production_family_count(
                result, HWA_PRODUCTION_TRAIN, rule->family) <
                hwa_production_minimum_train_spans() ||
            hwa_production_family_count(
                result, HWA_PRODUCTION_CHECK, rule->family) <
                hwa_production_minimum_check_spans()) count++;
    }
    if (result->source_count == 5U &&
        !hwa_production_room_ir_facts_available(result)) count++;
    return count;
}

int hwa_production_warning_spec_at(
    const HWAProductionResult *result,
    size_t requested,
    HWAProductionWarningSpec *spec)
{
    size_t current = 0U;
    size_t index;
    if (result == NULL || spec == NULL) return -1;
    memset(spec, 0, sizeof(*spec));
    for (index = 0U;
         index < HWA_PRODUCTION_ARRAY_COUNT(hwa_production_warning_rules);
         ++index) {
        const HWAProductionWarningRule *rule =
            &hwa_production_warning_rules[index];
        if (hwa_production_family_count(
                result, HWA_PRODUCTION_TRAIN, rule->family) >=
                hwa_production_minimum_train_spans() &&
            hwa_production_family_count(
                result, HWA_PRODUCTION_CHECK, rule->family) >=
                hwa_production_minimum_check_spans()) continue;
        if (current++ == requested) {
            spec->code = rule->code;
            spec->message = rule->message;
            spec->fit_id = rule->fit_id;
            spec->fit_id_valid = rule->fit_id <= result->fit_count;
            if (!spec->fit_id_valid) spec->fit_id = 0U;
            return 0;
        }
    }
    if (result->source_count == 5U &&
        !hwa_production_room_ir_facts_available(result) &&
        current == requested) {
        spec->code = "unusable-room-ir";
        spec->message =
            "The explicit room IR yielded no usable room-band fact.";
        return 0;
    }
    return -1;
}

typedef struct HWAProductionDynamicsPoint {
    double reference_dbfs;
    double model_dbfs;
    size_t span_index;
} HWAProductionDynamicsPoint;

static int hwa_production_fit_dynamics_grid(
    const HWAProductionDynamicsPoint *points,
    size_t point_count,
    size_t omitted_span,
    uint64_t *evaluations,
    uint64_t evaluation_limit,
    double *threshold_out,
    double *ratio_out,
    double *makeup_out,
    double *residuals,
    double *work)
{
    long double best_error = LDBL_MAX;
    double best_threshold = 0.0;
    double best_ratio = 0.0;
    double best_makeup = 0.0;
    size_t threshold_index;
    size_t ratio_index;
    size_t used = 0U;
    size_t point;
    double minimum = 0.0;
    double maximum = 0.0;
    for (point = 0U; point < point_count; ++point) {
        if (points[point].span_index == omitted_span) continue;
        if (used == 0U) minimum = maximum = points[point].model_dbfs;
        else {
            if (points[point].model_dbfs < minimum) {
                minimum = points[point].model_dbfs;
            }
            if (points[point].model_dbfs > maximum) {
                maximum = points[point].model_dbfs;
            }
        }
        used++;
    }
    if (used < hwa_production_minimum_train_spans() ||
        maximum - minimum < 12.0) return -1;
    for (threshold_index = 0U;
         threshold_index < hwa_production_threshold_grid_count();
         ++threshold_index) {
        double threshold = hwa_production_threshold_grid_at(threshold_index);
        for (ratio_index = 0U;
             ratio_index < hwa_production_ratio_grid_count(); ++ratio_index) {
            double ratio = hwa_production_ratio_grid_at(ratio_index);
            double makeup;
            long double squared = 0.0L;
            size_t count = 0U;
            if ((uint64_t)used > evaluation_limit - *evaluations) return -2;
            *evaluations += (uint64_t)used;
            for (point = 0U; point < point_count; ++point) {
                double compressed;
                if (points[point].span_index == omitted_span) continue;
                compressed = points[point].model_dbfs <= threshold ?
                    points[point].model_dbfs : threshold +
                    (points[point].model_dbfs - threshold) / ratio;
                residuals[count++] = points[point].reference_dbfs - compressed;
            }
            makeup = hwa_production_clamp(
                hwa_production_median_copy(residuals, count, work),
                -12.0, 12.0);
            for (point = 0U; point < count; ++point) {
                long double difference = (long double)residuals[point] - makeup;
                squared += difference * difference;
            }
            if (squared < best_error) {
                best_error = squared;
                best_threshold = threshold;
                best_ratio = ratio;
                best_makeup = makeup;
            }
        }
    }
    *threshold_out = best_threshold;
    *ratio_out = best_ratio;
    *makeup_out = best_makeup;
    return 0;
}

static int hwa_production_build_dynamics_fits(
    HWAProductionResult *result,
    const HWAProductionSpanValues *values,
    HWAProductionFitState *state,
    uint64_t *evaluations,
    char *error,
    size_t error_size)
{
    HWAProductionFit *threshold_fit = hwa_production_find_fit(
        result, HWA_PRODUCTION_SCOPE_CORRECTION,
        HWA_PRODUCTION_FIT_THRESHOLD_DBFS, 0U);
    HWAProductionFit *ratio_fit = hwa_production_find_fit(
        result, HWA_PRODUCTION_SCOPE_CORRECTION,
        HWA_PRODUCTION_FIT_RATIO, 0U);
    HWAProductionFit *makeup_fit = hwa_production_find_fit(
        result, HWA_PRODUCTION_SCOPE_CORRECTION,
        HWA_PRODUCTION_FIT_MAKEUP_DB, 0U);
    HWAProductionDynamicsPoint *points = NULL;
    double *residuals = NULL;
    double *work = NULL;
    double *thresholds = NULL;
    double *ratios = NULL;
    double *makeups = NULL;
    size_t eligible = hwa_production_family_count(
        result, HWA_PRODUCTION_TRAIN, HWA_PRODUCTION_SPAN_DYNAMICS);
    size_t point_count = 0U;
    size_t successful = 0U;
    size_t index;
    int status = -1;

    state->dynamics_valid = 0;
    state->threshold_dbfs = 0.0;
    state->ratio = 0.0;
    state->makeup_db = 0.0;

    for (index = 0U; index < result->span_count; ++index) {
        if (result->spans[index].split == HWA_PRODUCTION_TRAIN &&
            (result->spans[index].eligibility_flags &
             HWA_PRODUCTION_SPAN_DYNAMICS) != 0U) {
            size_t paired = values[index].reference_envelope.count <
                values[index].model_envelope.count ?
                values[index].reference_envelope.count :
                values[index].model_envelope.count;
            if (paired > SIZE_MAX - point_count) goto overflow;
            point_count += paired;
        }
    }
    if (point_count > result->options.max_envelope_points ||
        point_count > SIZE_MAX / sizeof(*points) ||
        eligible > SIZE_MAX / sizeof(*thresholds)) goto overflow;
    points = point_count == 0U ? NULL :
        (HWAProductionDynamicsPoint *)malloc(point_count * sizeof(*points));
    residuals = point_count == 0U ? NULL :
        (double *)malloc(point_count * sizeof(*residuals));
    work = point_count == 0U ? NULL :
        (double *)malloc(point_count * sizeof(*work));
    thresholds = eligible == 0U ? NULL :
        (double *)malloc(eligible * sizeof(*thresholds));
    ratios = eligible == 0U ? NULL :
        (double *)malloc(eligible * sizeof(*ratios));
    makeups = eligible == 0U ? NULL :
        (double *)malloc(eligible * sizeof(*makeups));
    if ((point_count != 0U &&
         (points == NULL || residuals == NULL || work == NULL)) ||
        (eligible != 0U &&
         (thresholds == NULL || ratios == NULL || makeups == NULL))) {
        hwa_set_error(error, error_size,
                      "cannot allocate dynamics grid work");
        goto cleanup;
    }
    point_count = 0U;
    for (index = 0U; index < result->span_count; ++index) {
        if (result->spans[index].split == HWA_PRODUCTION_TRAIN &&
            (result->spans[index].eligibility_flags &
             HWA_PRODUCTION_SPAN_DYNAMICS) != 0U) {
            size_t paired = values[index].reference_envelope.count <
                values[index].model_envelope.count ?
                values[index].reference_envelope.count :
                values[index].model_envelope.count;
            size_t local;
            for (local = 0U; local < paired; ++local) {
                points[point_count].reference_dbfs =
                    values[index].reference_envelope.points[local];
                points[point_count].model_dbfs =
                    values[index].model_envelope.points[local];
                points[point_count].span_index = index;
                point_count++;
            }
        }
    }
    if (eligible >= hwa_production_minimum_train_spans() &&
        point_count >= hwa_production_minimum_train_spans()) {
        double base_threshold;
        double base_ratio;
        double base_makeup;
        int fit_status = hwa_production_fit_dynamics_grid(
            points, point_count, SIZE_MAX, evaluations,
            result->options.max_evaluations, &base_threshold, &base_ratio,
            &base_makeup, residuals, work);
        if (fit_status == -2) {
            hwa_set_error(error, error_size,
                          "dynamics grid exceeds the evaluation cap");
            goto cleanup;
        }
        if (fit_status == 0) {
            for (index = 0U; index < result->span_count; ++index) {
                double threshold;
                double ratio;
                double makeup;
                int jack_status;
                if (result->spans[index].split != HWA_PRODUCTION_TRAIN ||
                    (result->spans[index].eligibility_flags &
                     HWA_PRODUCTION_SPAN_DYNAMICS) == 0U) continue;
                jack_status = hwa_production_fit_dynamics_grid(
                    points, point_count, index, evaluations,
                    result->options.max_evaluations, &threshold, &ratio,
                    &makeup, residuals, work);
                if (jack_status == -2) {
                    hwa_set_error(error, error_size,
                                  "dynamics uncertainty exceeds evaluation cap");
                    goto cleanup;
                }
                if (jack_status == 0) {
                    thresholds[successful] = threshold;
                    ratios[successful] = ratio;
                    makeups[successful] = makeup;
                    successful++;
                }
            }
            if (successful >= hwa_production_minimum_train_spans()) {
                size_t low = (size_t)floor((double)(successful - 1U) * 0.05);
                size_t high = (size_t)ceil((double)(successful - 1U) * 0.95);
                double threshold_q05;
                double threshold_q95;
                double ratio_q05;
                double ratio_q95;
                double makeup_q05;
                double makeup_q95;
                qsort(thresholds, successful, sizeof(*thresholds),
                      hwa_production_double_compare);
                qsort(ratios, successful, sizeof(*ratios),
                      hwa_production_double_compare);
                threshold_q05 = thresholds[low] < base_threshold ?
                    thresholds[low] : base_threshold;
                threshold_q95 = thresholds[high] > base_threshold ?
                    thresholds[high] : base_threshold;
                ratio_q05 = ratios[low] < base_ratio ? ratios[low] : base_ratio;
                ratio_q95 = ratios[high] > base_ratio ? ratios[high] : base_ratio;
                memcpy(work, makeups, successful * sizeof(*work));
                makeup_q05 = hwa_production_quantile(work, successful, 0.05);
                memcpy(work, makeups, successful * sizeof(*work));
                makeup_q95 = hwa_production_quantile(work, successful, 0.95);
                if (makeup_q05 > base_makeup) makeup_q05 = base_makeup;
                if (makeup_q95 < base_makeup) makeup_q95 = base_makeup;
                state->threshold_dbfs = base_threshold;
                state->ratio = base_ratio;
                state->makeup_db = base_makeup;
                state->dynamics_valid = 1;
                hwa_production_fit_set(
                    threshold_fit, base_threshold, threshold_q05,
                    threshold_q95, eligible, point_count, 0U);
                hwa_production_fit_set(
                    ratio_fit, base_ratio, ratio_q05, ratio_q95,
                    eligible, point_count, 0U);
                hwa_production_fit_set(
                    makeup_fit, base_makeup,
                    hwa_production_clamp(makeup_q05, -12.0, base_makeup),
                    hwa_production_clamp(makeup_q95, base_makeup, 12.0),
                    eligible, point_count,
                    fabs(base_makeup) == 12.0 ?
                        HWA_PRODUCTION_QUALITY_FIT_AT_BOUND : 0U);
            }
        }
    }
    if (!state->dynamics_valid) {
        hwa_production_fit_mark(
            threshold_fit, HWA_PRODUCTION_INSUFFICIENT, eligible, point_count,
            HWA_PRODUCTION_QUALITY_LOW_COVERAGE);
        hwa_production_fit_mark(
            ratio_fit, HWA_PRODUCTION_INSUFFICIENT, eligible, point_count,
            HWA_PRODUCTION_QUALITY_LOW_COVERAGE);
        hwa_production_fit_mark(
            makeup_fit, HWA_PRODUCTION_INSUFFICIENT, eligible, point_count,
            HWA_PRODUCTION_QUALITY_LOW_COVERAGE);
    }
    status = 0;
    goto cleanup;

overflow:
    hwa_set_error(error, error_size, "dynamics fit work exceeds its caps");
cleanup:
    free(points);
    free(residuals);
    free(work);
    free(thresholds);
    free(ratios);
    free(makeups);
    return status;
}

static int hwa_production_build_fits(
    HWAProductionResult *result,
    const HWAProductionSpanValues *values,
    const HWAProductionRoomFacts *room,
    HWAProductionFitState *state,
    uint32_t reference_rate,
    uint32_t model_rate,
    uint64_t *evaluations,
    char *error,
    size_t error_size)
{
    static const size_t eq_band[HWA_PRODUCTION_EQ_NODE_COUNT] = {
        1U, 2U, 3U, 4U, 5U, 6U, 7U
    };
    uint32_t families[] = {
        HWA_PRODUCTION_SPAN_EQ, HWA_PRODUCTION_SPAN_DYNAMICS,
        HWA_PRODUCTION_SPAN_STEREO, HWA_PRODUCTION_SPAN_DECAY
    };
    double *samples = NULL;
    double *work = NULL;
    double *reference_levels = NULL;
    double *model_levels = NULL;
    size_t capacity = result->span_count;
    size_t offset;
    size_t family;
    size_t index;

    memset(state, 0, sizeof(*state));
    result->fit_count = hwa_production_fit_catalog_count();
    if (result->fit_count > result->options.max_fits ||
        result->fit_count > SIZE_MAX / sizeof(*result->fits)) {
        hwa_set_error(error, error_size, "Stage 6 fit catalog exceeds its cap");
        return -1;
    }
    result->fits = (HWAProductionFit *)calloc(result->fit_count,
                                               sizeof(*result->fits));
    samples = capacity == 0U ? NULL :
        (double *)malloc(capacity * sizeof(*samples));
    work = capacity == 0U ? NULL :
        (double *)malloc(capacity * sizeof(*work));
    reference_levels = capacity == 0U ? NULL :
        (double *)malloc(capacity * sizeof(*reference_levels));
    model_levels = capacity == 0U ? NULL :
        (double *)malloc(capacity * sizeof(*model_levels));
    if (result->fits == NULL || (capacity != 0U &&
        (samples == NULL || work == NULL || reference_levels == NULL ||
         model_levels == NULL))) {
        hwa_set_error(error, error_size, "cannot allocate Stage 6 fit work");
        goto failure;
    }
    for (offset = 0U; offset < result->fit_count; ++offset) {
        HWAProductionFit *fit = &result->fits[offset];
        fit->id = (uint64_t)offset + 1U;
        if (hwa_production_fit_catalog_at(
                offset, &fit->scope, &fit->kind, &fit->index,
                &fit->unit) != 0) {
            hwa_set_error(error, error_size, "invalid Stage 6 fit catalog");
            goto failure;
        }
        fit->availability = HWA_PRODUCTION_UNAVAILABLE;
    }
    for (family = 0U; family < HWA_PRODUCTION_ARRAY_COUNT(families); ++family) {
        size_t train = hwa_production_family_count(
            result, HWA_PRODUCTION_TRAIN, families[family]);
        size_t check = hwa_production_family_count(
            result, HWA_PRODUCTION_CHECK, families[family]);
        if (train >= hwa_production_minimum_train_spans()) {
            state->train_sufficient_flags |= families[family];
        }
        if (check >= hwa_production_minimum_check_spans()) {
            state->check_sufficient_flags |= families[family];
        }
    }
    {
        uint64_t eq_visits;
        uint64_t stereo_visits;
        uint64_t decay_visits;
        uint64_t fit_visits = 0U;
        if (hwa_production_array_bytes(
                hwa_production_family_count(
                    result, HWA_PRODUCTION_TRAIN,
                    HWA_PRODUCTION_SPAN_EQ),
                HWA_PRODUCTION_EQ_NODE_COUNT, &eq_visits) != 0 ||
            hwa_production_array_bytes(
                hwa_production_family_count(
                    result, HWA_PRODUCTION_TRAIN,
                    HWA_PRODUCTION_SPAN_STEREO),
                5U, &stereo_visits) != 0 ||
            hwa_production_array_bytes(
                hwa_production_family_count(
                    result, HWA_PRODUCTION_TRAIN,
                    HWA_PRODUCTION_SPAN_DECAY),
                HWA_PRODUCTION_ROOM_BAND_COUNT * 4U,
                &decay_visits) != 0 ||
            hwa_production_add_size(
                &fit_visits, eq_visits, UINT64_MAX) != 0 ||
            hwa_production_add_size(
                &fit_visits, stereo_visits, UINT64_MAX) != 0 ||
            hwa_production_add_size(
                &fit_visits, decay_visits, UINT64_MAX) != 0 ||
            hwa_production_charge_evaluations(
                evaluations, fit_visits,
                result->options.max_evaluations,
                "production fit evidence", error, error_size) != 0) {
            goto failure;
        }
    }

    for (offset = 0U; offset < HWA_PRODUCTION_EQ_NODE_COUNT; ++offset) {
        HWAProductionFit *fit = hwa_production_find_fit(
            result, HWA_PRODUCTION_SCOPE_CORRECTION,
            HWA_PRODUCTION_FIT_EQ_GAIN_DB, (uint32_t)offset);
        size_t eligible = hwa_production_family_count(
            result, HWA_PRODUCTION_TRAIN, HWA_PRODUCTION_SPAN_EQ);
        size_t count = 0U;
        if (!hwa_production_eq_node_supported(
                offset, reference_rate, model_rate)) {
            hwa_production_fit_mark(fit, HWA_PRODUCTION_UNAVAILABLE,
                                    eligible, 0U, 0U);
            continue;
        }
        for (index = 0U; index < result->span_count; ++index) {
            if (result->spans[index].split != HWA_PRODUCTION_TRAIN ||
                (result->spans[index].eligibility_flags &
                 HWA_PRODUCTION_SPAN_EQ) == 0U) continue;
            if (values[index].reference[2U + eq_band[offset]].valid &&
                values[index].model[2U + eq_band[offset]].valid) {
                samples[count++] =
                    values[index].reference[2U + eq_band[offset]].value -
                    values[index].model[2U + eq_band[offset]].value;
            }
        }
        if (eligible < hwa_production_minimum_train_spans() ||
            count < hwa_production_minimum_train_spans()) {
            hwa_production_fit_mark(fit, HWA_PRODUCTION_INSUFFICIENT,
                                    eligible, count,
                                    HWA_PRODUCTION_QUALITY_LOW_COVERAGE);
        } else {
            double estimate = hwa_production_median_copy(samples, count, work);
            double q05;
            double q95;
            memcpy(work, samples, count * sizeof(*work));
            q05 = hwa_production_quantile(work, count, 0.05);
            memcpy(work, samples, count * sizeof(*work));
            q95 = hwa_production_quantile(work, count, 0.95);
            estimate = hwa_production_clamp(estimate, -6.0, 6.0);
            q05 = hwa_production_clamp(q05, -6.0, estimate);
            q95 = hwa_production_clamp(q95, estimate, 6.0);
            state->eq_gain[offset] = estimate;
            state->eq_valid[offset] = 1;
            hwa_production_fit_set(
                fit, estimate, q05, q95, eligible, count,
                estimate == -6.0 || estimate == 6.0 ?
                    HWA_PRODUCTION_QUALITY_FIT_AT_BOUND : 0U);
        }
    }
    for (offset = 1U; offset < HWA_PRODUCTION_EQ_NODE_COUNT; ++offset) {
        if (state->eq_valid[offset - 1U] && state->eq_valid[offset]) {
            double low = state->eq_gain[offset - 1U] - 3.0;
            double high = state->eq_gain[offset - 1U] + 3.0;
            HWAProductionFit *fit = hwa_production_find_fit(
                result, HWA_PRODUCTION_SCOPE_CORRECTION,
                HWA_PRODUCTION_FIT_EQ_GAIN_DB, (uint32_t)offset);
            HWAProductionFit *previous_fit = hwa_production_find_fit(
                result, HWA_PRODUCTION_SCOPE_CORRECTION,
                HWA_PRODUCTION_FIT_EQ_GAIN_DB, (uint32_t)(offset - 1U));
            state->eq_gain[offset] = hwa_production_clamp(
                state->eq_gain[offset], low, high);
            fit->estimate = state->eq_gain[offset];
            if (fit->q05 > fit->estimate) fit->q05 = fit->estimate;
            if (fit->q95 < fit->estimate) fit->q95 = fit->estimate;
            fit->q05 = hwa_production_clamp(
                fit->q05, previous_fit->q05 - 3.0,
                previous_fit->q05 + 3.0);
            fit->q95 = hwa_production_clamp(
                fit->q95, previous_fit->q95 - 3.0,
                previous_fit->q95 + 3.0);
            if (fit->q05 > fit->estimate) fit->q05 = fit->estimate;
            if (fit->q95 < fit->estimate) fit->q95 = fit->estimate;
            if (fit->estimate == low || fit->estimate == high) {
                fit->quality_flags |= HWA_PRODUCTION_QUALITY_FIT_AT_BOUND;
            }
        }
    }

    if (hwa_production_build_dynamics_fits(
            result, values, state, evaluations, error, error_size) != 0) {
        goto failure;
    }

    /* Stereo correction and side facts share one fixed median reducer. */
    {
        static const HWAProductionFitKind kinds[4] = {
            HWA_PRODUCTION_FIT_STEREO_DELAY_SAMPLES,
            HWA_PRODUCTION_FIT_CHANNEL_BALANCE_DB,
            HWA_PRODUCTION_FIT_STEREO_WIDTH_RATIO,
            HWA_PRODUCTION_FIT_STEREO_CORRELATION
        };
        static const size_t metric_offsets[4] = {
            6U + HWA_BAND_COUNT, 3U + HWA_BAND_COUNT,
            4U + HWA_BAND_COUNT, 5U + HWA_BAND_COUNT
        };
        size_t eligible = hwa_production_family_count(
            result, HWA_PRODUCTION_TRAIN, HWA_PRODUCTION_SPAN_STEREO);
        size_t kind_index;
        double polarity = 1.0;
        double reference_polarity = 1.0;
        double model_polarity = 1.0;
        size_t correlation_count = 0U;
        for (index = 0U; index < result->span_count; ++index) {
            if (result->spans[index].split == HWA_PRODUCTION_TRAIN &&
                (result->spans[index].eligibility_flags &
                 HWA_PRODUCTION_SPAN_STEREO) != 0U &&
                values[index].reference[5U + HWA_BAND_COUNT].valid &&
                values[index].model[5U + HWA_BAND_COUNT].valid) {
                reference_levels[correlation_count] =
                    values[index].reference[5U + HWA_BAND_COUNT].value;
                model_levels[correlation_count] =
                    values[index].model[5U + HWA_BAND_COUNT].value;
                samples[correlation_count++] =
                    values[index].reference[5U + HWA_BAND_COUNT].value *
                    values[index].model[5U + HWA_BAND_COUNT].value;
            }
        }
        if (correlation_count >= hwa_production_minimum_train_spans()) {
            reference_polarity = hwa_production_median_copy(
                reference_levels, correlation_count, work) < 0.0 ? -1.0 : 1.0;
            model_polarity = hwa_production_median_copy(
                model_levels, correlation_count, work) < 0.0 ? -1.0 : 1.0;
            polarity = reference_polarity * model_polarity;
        }
        for (kind_index = 0U; kind_index < 4U; ++kind_index) {
            size_t count = 0U;
            HWAProductionFit *reference_fit = hwa_production_find_fit(
                result, HWA_PRODUCTION_SCOPE_REFERENCE, kinds[kind_index], 0U);
            HWAProductionFit *model_fit = hwa_production_find_fit(
                result, HWA_PRODUCTION_SCOPE_MODEL, kinds[kind_index], 0U);
            HWAProductionFit *correction_fit = hwa_production_find_fit(
                result, HWA_PRODUCTION_SCOPE_CORRECTION, kinds[kind_index], 0U);
            double reference_median;
            double reference_q05;
            double reference_q95;
            double model_median;
            double model_q05;
            double model_q95;
            double correction;
            double correction_q05;
            double correction_q95;
            for (index = 0U; index < result->span_count; ++index) {
                size_t metric = metric_offsets[kind_index];
                if (result->spans[index].split == HWA_PRODUCTION_TRAIN &&
                    (result->spans[index].eligibility_flags &
                     HWA_PRODUCTION_SPAN_STEREO) != 0U &&
                    values[index].reference[metric].valid &&
                    values[index].model[metric].valid) {
                    double reference_value =
                        values[index].reference[metric].value;
                    double model_value = values[index].model[metric].value;
                    reference_levels[count] = reference_value;
                    model_levels[count] = model_value;
                    if (kinds[kind_index] ==
                        HWA_PRODUCTION_FIT_STEREO_WIDTH_RATIO) {
                        samples[count] = hwa_production_clamp(
                            model_value > DBL_MIN ?
                                reference_value / model_value : 4.0,
                            0.25, 4.0);
                    } else if (kinds[kind_index] ==
                               HWA_PRODUCTION_FIT_STEREO_CORRELATION) {
                        samples[count] = hwa_production_clamp(
                            reference_value - polarity * model_value,
                            -1.0, 1.0);
                    } else if (kinds[kind_index] ==
                               HWA_PRODUCTION_FIT_STEREO_DELAY_SAMPLES) {
                        samples[count] = hwa_production_clamp(
                            reference_value - model_value,
                            -floor(0.002 * (double)model_rate),
                            floor(0.002 * (double)model_rate));
                    } else {
                        samples[count] = hwa_production_clamp(
                            reference_value - model_value, -12.0, 12.0);
                    }
                    count++;
                }
            }
            if (eligible < hwa_production_minimum_train_spans() ||
                count < hwa_production_minimum_train_spans()) {
                hwa_production_fit_mark(reference_fit,
                    HWA_PRODUCTION_INSUFFICIENT, eligible, count,
                    HWA_PRODUCTION_QUALITY_LOW_COVERAGE);
                hwa_production_fit_mark(model_fit,
                    HWA_PRODUCTION_INSUFFICIENT, eligible, count,
                    HWA_PRODUCTION_QUALITY_LOW_COVERAGE);
                hwa_production_fit_mark(correction_fit,
                    HWA_PRODUCTION_INSUFFICIENT, eligible, count,
                    HWA_PRODUCTION_QUALITY_LOW_COVERAGE);
                continue;
            }
            hwa_production_fit_summary(
                reference_levels, count, work, &reference_median,
                &reference_q05, &reference_q95);
            hwa_production_fit_summary(
                model_levels, count, work, &model_median,
                &model_q05, &model_q95);
            hwa_production_fit_summary(
                samples, count, work, &correction,
                &correction_q05, &correction_q95);
            if (kinds[kind_index] == HWA_PRODUCTION_FIT_STEREO_WIDTH_RATIO) {
                state->width_ratio = correction;
                state->width_valid = 1;
            } else if (kinds[kind_index] ==
                       HWA_PRODUCTION_FIT_STEREO_CORRELATION) {
                state->correlation = correction;
                state->correlation_valid = 1;
            } else {
                if (kinds[kind_index] ==
                    HWA_PRODUCTION_FIT_STEREO_DELAY_SAMPLES) {
                    state->delay_samples = correction;
                    state->delay_valid = 1;
                } else {
                    state->balance_db = correction;
                    state->balance_valid = 1;
                }
            }
            hwa_production_fit_set(
                reference_fit, reference_median, reference_q05,
                reference_q95, eligible, count, 0U);
            hwa_production_fit_set(
                model_fit, model_median, model_q05,
                model_q95, eligible, count, 0U);
            hwa_production_fit_set(
                correction_fit, correction, correction_q05,
                correction_q95, eligible, count, 0U);
        }
        {
            HWAProductionFit *correction_fit = hwa_production_find_fit(
                result, HWA_PRODUCTION_SCOPE_CORRECTION,
                HWA_PRODUCTION_FIT_CHANNEL_POLARITY, 0U);
            HWAProductionFit *reference_fit = hwa_production_find_fit(
                result, HWA_PRODUCTION_SCOPE_REFERENCE,
                HWA_PRODUCTION_FIT_CHANNEL_POLARITY, 0U);
            HWAProductionFit *model_fit = hwa_production_find_fit(
                result, HWA_PRODUCTION_SCOPE_MODEL,
                HWA_PRODUCTION_FIT_CHANNEL_POLARITY, 0U);
            if (eligible >= hwa_production_minimum_train_spans() &&
                correlation_count >= hwa_production_minimum_train_spans()) {
                state->polarity = polarity;
                state->polarity_valid = 1;
                hwa_production_fit_set_without_uncertainty(
                    correction_fit, polarity, eligible, correlation_count, 0U);
                hwa_production_fit_set_without_uncertainty(
                    reference_fit, reference_polarity, eligible,
                    correlation_count, 0U);
                hwa_production_fit_set_without_uncertainty(
                    model_fit, model_polarity, eligible,
                    correlation_count, 0U);
            } else {
                hwa_production_fit_mark(correction_fit,
                    HWA_PRODUCTION_INSUFFICIENT, eligible, correlation_count,
                    HWA_PRODUCTION_QUALITY_LOW_COVERAGE);
                hwa_production_fit_mark(reference_fit,
                    HWA_PRODUCTION_INSUFFICIENT, eligible, correlation_count,
                    HWA_PRODUCTION_QUALITY_LOW_COVERAGE);
                hwa_production_fit_mark(model_fit,
                    HWA_PRODUCTION_INSUFFICIENT, eligible, correlation_count,
                    HWA_PRODUCTION_QUALITY_LOW_COVERAGE);
            }
        }
    }

    for (offset = 0U; offset < HWA_PRODUCTION_ROOM_BAND_COUNT; ++offset) {
        HWAProductionFitKind kind;
        for (kind = HWA_PRODUCTION_FIT_EARLY_REFLECTION_DB;
             kind <= HWA_PRODUCTION_FIT_LATE_DECAY_SECONDS;
             kind = (HWAProductionFitKind)((int)kind + 1)) {
            size_t metric = 7U + HWA_BAND_COUNT + offset +
                (kind == HWA_PRODUCTION_FIT_LATE_DECAY_SECONDS ?
                    HWA_PRODUCTION_ROOM_BAND_COUNT : 0U);
            size_t eligible = hwa_production_family_count(
                result, HWA_PRODUCTION_TRAIN, HWA_PRODUCTION_SPAN_DECAY);
            HWAProductionScope scope;
            for (scope = HWA_PRODUCTION_SCOPE_REFERENCE;
                 scope <= HWA_PRODUCTION_SCOPE_MODEL;
                 scope = (HWAProductionScope)((int)scope + 1)) {
                HWAProductionFit *fit = hwa_production_find_fit(
                    result, scope, kind, (uint32_t)offset);
                size_t count = 0U;
                if (!hwa_production_room_band_supported(
                        (uint32_t)offset,
                        scope == HWA_PRODUCTION_SCOPE_REFERENCE ?
                            reference_rate : model_rate)) {
                    hwa_production_fit_mark(
                        fit, HWA_PRODUCTION_UNAVAILABLE,
                        eligible, 0U, 0U);
                    continue;
                }
                for (index = 0U; index < result->span_count; ++index) {
                    const HWAProductionMetricValue *metric_value =
                        scope == HWA_PRODUCTION_SCOPE_REFERENCE ?
                            &values[index].reference[metric] :
                            &values[index].model[metric];
                    if (result->spans[index].split == HWA_PRODUCTION_TRAIN &&
                        (result->spans[index].eligibility_flags &
                         HWA_PRODUCTION_SPAN_DECAY) != 0U &&
                        metric_value->valid) samples[count++] = metric_value->value;
                }
                if (eligible >= hwa_production_minimum_train_spans() &&
                    count >= hwa_production_minimum_train_spans()) {
                    double estimate = hwa_production_median_copy(
                        samples, count, work);
                    double q05;
                    double q95;
                    memcpy(work, samples, count * sizeof(*work));
                    q05 = hwa_production_quantile(work, count, 0.05);
                    memcpy(work, samples, count * sizeof(*work));
                    q95 = hwa_production_quantile(work, count, 0.95);
                    if (q05 > estimate) q05 = estimate;
                    if (q95 < estimate) q95 = estimate;
                    hwa_production_fit_set(
                        fit, estimate, q05, q95, eligible, count, 0U);
                } else {
                    hwa_production_fit_mark(fit,
                        HWA_PRODUCTION_INSUFFICIENT, eligible, count,
                        HWA_PRODUCTION_QUALITY_LOW_COVERAGE);
                }
            }
            {
                HWAProductionFit *fit = hwa_production_find_fit(
                    result, HWA_PRODUCTION_SCOPE_ROOM_IR, kind,
                    (uint32_t)offset);
                int valid = kind == HWA_PRODUCTION_FIT_EARLY_REFLECTION_DB ?
                    room->early_valid[offset] : room->late_valid[offset];
                double estimate = kind == HWA_PRODUCTION_FIT_EARLY_REFLECTION_DB ?
                    room->early_db[offset] : room->late_seconds[offset];
                int supported = result->source_count == 5U &&
                    hwa_production_room_band_supported(
                        (uint32_t)offset,
                        result->sources[4].format.sample_rate_hz);
                if (room->supplied && !supported) {
                    hwa_production_fit_mark(
                        fit, HWA_PRODUCTION_UNAVAILABLE, 0U, 0U, 0U);
                } else if (room->supplied && valid) {
                    hwa_production_fit_set_without_uncertainty(
                        fit, estimate, 0U, room->point_count,
                        HWA_PRODUCTION_QUALITY_IR_SUPPLIED);
                } else {
                    hwa_production_fit_mark(
                        fit, room->supplied ? HWA_PRODUCTION_INSUFFICIENT :
                                             HWA_PRODUCTION_UNAVAILABLE,
                        0U, room->supplied ? room->point_count : 0U,
                        room->supplied ? HWA_PRODUCTION_QUALITY_IR_SUPPLIED : 0U);
                }
            }
        }
    }
    free(samples);
    free(work);
    free(reference_levels);
    free(model_levels);
    return 0;

failure:
    free(samples);
    free(work);
    free(reference_levels);
    free(model_levels);
    return -1;
}

static const HWAProductionFit *hwa_production_find_fit_const(
    const HWAProductionResult *result,
    HWAProductionScope scope,
    HWAProductionFitKind kind,
    uint32_t index)
{
    size_t fit_index;
    for (fit_index = 0U; fit_index < result->fit_count; ++fit_index) {
        const HWAProductionFit *fit = &result->fits[fit_index];
        if (fit->scope == scope && fit->kind == kind && fit->index == index) {
            return fit;
        }
    }
    return NULL;
}

static int hwa_production_eq_interpolation(
    uint32_t band,
    uint32_t reference_rate,
    uint32_t model_rate,
    size_t *lower,
    size_t *upper,
    double *fraction)
{
    static const double centers[HWA_BAND_COUNT] = {
        30.0, 84.8528137423857, 173.205080756888,
        353.553390593274, 707.106781186548, 1414.21356237310,
        2828.42712474619, 5656.85424949238, 11313.7084989848,
        20000.0
    };
    size_t node;
    double frequency;
    if (band >= HWA_BAND_COUNT || lower == NULL || upper == NULL ||
        fraction == NULL) return -1;
    frequency = centers[band];
    for (node = 0U; node + 1U < HWA_PRODUCTION_EQ_NODE_COUNT; ++node) {
        double low = hwa_production_eq_node_frequency_hz(node);
        double high = hwa_production_eq_node_frequency_hz(node + 1U);
        if (frequency >= low && frequency <= high &&
            hwa_production_eq_node_supported(
                node, reference_rate, model_rate) &&
            hwa_production_eq_node_supported(
                node + 1U, reference_rate, model_rate)) {
            *lower = node;
            *upper = node + 1U;
            *fraction = log(frequency / low) / log(high / low);
            return 0;
        }
    }
    return -1;
}

static double hwa_production_forward_level(double input,
                                           double threshold,
                                           double ratio,
                                           double makeup)
{
    double compressed = input <= threshold ? input :
        threshold + (input - threshold) / ratio;
    return compressed + makeup;
}

static double hwa_production_inverse_level(double input,
                                           double threshold,
                                           double ratio,
                                           double makeup)
{
    double without_makeup = input - makeup;
    return without_makeup <= threshold ? without_makeup :
        threshold + (without_makeup - threshold) * ratio;
}

static int hwa_production_check_metric_count(
    const HWAProductionResult *result,
    HWAProductionMetricKind kind,
    size_t metric_offset,
    uint32_t family)
{
    size_t count = 0U;
    size_t span_index;
    for (span_index = 0U; span_index < result->span_count; ++span_index) {
        size_t raw_index = span_index * HWA_PRODUCTION_EVALUATIONS_PER_SPAN +
                           metric_offset;
        if (result->spans[span_index].split == HWA_PRODUCTION_CHECK &&
            (result->spans[span_index].eligibility_flags & family) != 0U &&
            result->evaluations[raw_index].kind == kind &&
            result->evaluations[raw_index].availability ==
                HWA_PRODUCTION_AVAILABLE) count++;
    }
    return count >= hwa_production_minimum_check_spans();
}

static int hwa_production_corrected_check_sufficient(
    const HWAProductionResult *result,
    HWAProductionMetricKind kind,
    size_t metric_offset)
{
    if (kind == HWA_PRODUCTION_METRIC_RMS_DBFS) {
        return hwa_production_check_metric_count(
            result, HWA_PRODUCTION_METRIC_RMS_DBFS, 0U,
            HWA_PRODUCTION_SPAN_DYNAMICS);
    }
    if (kind == HWA_PRODUCTION_METRIC_BAND_LEVEL_DBFS) {
        return hwa_production_check_metric_count(
                   result, HWA_PRODUCTION_METRIC_RMS_DBFS, 0U,
                   HWA_PRODUCTION_SPAN_DYNAMICS) &&
               hwa_production_check_metric_count(
                   result, kind, metric_offset, HWA_PRODUCTION_SPAN_EQ);
    }
    if (kind == HWA_PRODUCTION_METRIC_LEVEL_SPREAD_DB) {
        return hwa_production_check_metric_count(
            result, kind, metric_offset, HWA_PRODUCTION_SPAN_DYNAMICS);
    }
    if (kind == HWA_PRODUCTION_METRIC_CHANNEL_BALANCE_DB ||
        kind == HWA_PRODUCTION_METRIC_STEREO_WIDTH_RATIO ||
        kind == HWA_PRODUCTION_METRIC_STEREO_CORRELATION ||
        kind == HWA_PRODUCTION_METRIC_STEREO_DELAY_SAMPLES) {
        return hwa_production_check_metric_count(
            result, kind, metric_offset, HWA_PRODUCTION_SPAN_STEREO);
    }
    if (kind == HWA_PRODUCTION_METRIC_EARLY_REFLECTION_DB ||
        kind == HWA_PRODUCTION_METRIC_LATE_DECAY_SECONDS) {
        return hwa_production_check_metric_count(
            result, kind, metric_offset, HWA_PRODUCTION_SPAN_DECAY);
    }
    return 0;
}

static int hwa_production_fit_available(
    const HWAProductionFit *fit,
    HWAProductionAvailability *missing,
    uint32_t *quality)
{
    if (fit != NULL) *quality |= fit->quality_flags;
    if (fit != NULL && fit->availability == HWA_PRODUCTION_AVAILABLE &&
        fit->estimate_valid) return 1;
    *quality |= HWA_PRODUCTION_QUALITY_CORRECTION_INCOMPLETE;
    if (fit != NULL && fit->availability == HWA_PRODUCTION_INSUFFICIENT) {
        *missing = HWA_PRODUCTION_INSUFFICIENT;
    }
    return 0;
}

static int hwa_production_has_usable_room_ir(
    const HWAProductionResult *result)
{
    size_t index;
    for (index = 0U; index < result->fit_count; ++index) {
        const HWAProductionFit *fit = &result->fits[index];
        if (fit->scope == HWA_PRODUCTION_SCOPE_ROOM_IR &&
            fit->availability == HWA_PRODUCTION_AVAILABLE &&
            fit->estimate_valid) return 1;
    }
    return 0;
}

int hwa_production_evaluation_derive(
    const HWAProductionResult *result,
    size_t span_index,
    HWAProductionView view,
    size_t metric_offset,
    HWAProductionEvaluation *evaluation,
    char *error,
    size_t error_size)
{
    const HWAProductionSpan *span;
    const HWAProductionEvaluation *raw;
    HWAProductionMetricKind kind;
    HWAProductionUnit unit;
    uint32_t metric_index;
    uint32_t required_flags;
    HWAProductionAvailability missing = HWA_PRODUCTION_UNAVAILABLE;
    double reference_value;
    double model_value;
    uint32_t quality;
    const HWAProductionFit *threshold;
    const HWAProductionFit *ratio;
    const HWAProductionFit *makeup;
    size_t row_index;
    if (result == NULL || evaluation == NULL ||
        span_index >= result->span_count ||
        view <= HWA_PRODUCTION_VIEW_RAW ||
        view >= HWA_PRODUCTION_VIEW_COUNT ||
        metric_offset >= hwa_production_metric_catalog_count() ||
        hwa_production_metric_catalog_at(
            metric_offset, &kind, &metric_index, &unit) != 0) {
        hwa_set_error(error, error_size,
                      "invalid Stage 6 corrected evaluation request");
        return -1;
    }
    span = &result->spans[span_index];
    raw = &result->evaluations[
        span_index * HWA_PRODUCTION_EVALUATIONS_PER_SPAN + metric_offset];
    row_index = span_index * HWA_PRODUCTION_EVALUATIONS_PER_SPAN +
        ((size_t)view - 1U) * HWA_PRODUCTION_METRICS_PER_VIEW + metric_offset;
    memset(evaluation, 0, sizeof(*evaluation));
    evaluation->id = (uint64_t)row_index + 1U;
    evaluation->span_id = span->id;
    evaluation->view = view;
    evaluation->kind = kind;
    evaluation->index = metric_index;
    evaluation->unit = unit;
    evaluation->availability = HWA_PRODUCTION_UNAVAILABLE;
    evaluation->evidence_flags = raw->evidence_flags;
    if (span->split == HWA_PRODUCTION_CHECK) {
        evaluation->evidence_flags |= HWA_PRODUCTION_EVIDENCE_HELD_OUT;
    } else {
        evaluation->evidence_flags &=
            (uint32_t)~(uint32_t)HWA_PRODUCTION_EVIDENCE_HELD_OUT;
    }
    quality = raw->quality_flags;
    if ((kind == HWA_PRODUCTION_METRIC_EARLY_REFLECTION_DB ||
         kind == HWA_PRODUCTION_METRIC_LATE_DECAY_SECONDS) &&
        (!hwa_production_room_band_supported(
             metric_index, result->sources[1].format.sample_rate_hz) ||
         !hwa_production_room_band_supported(
             metric_index, result->sources[3].format.sample_rate_hz) ||
         (view == HWA_PRODUCTION_VIEW_ROOM_MATCHED &&
          result->source_count == 5U &&
          !hwa_production_room_band_supported(
              metric_index,
              result->sources[4].format.sample_rate_hz)))) {
        evaluation->quality_flags = quality;
        return 0;
    }
    if (kind == HWA_PRODUCTION_METRIC_BAND_LEVEL_DBFS) {
        size_t lower;
        size_t upper;
        double fraction;
        if (hwa_production_eq_interpolation(
                metric_index,
                result->sources[1].format.sample_rate_hz,
                result->sources[3].format.sample_rate_hz,
                &lower, &upper, &fraction) != 0) {
            evaluation->quality_flags = quality;
            return 0;
        }
    }
    if (view == HWA_PRODUCTION_VIEW_DRY_LIKE) {
        quality |= HWA_PRODUCTION_QUALITY_SOURCE_CONFOUNDED;
    } else {
        if (result->source_count != 5U) {
            evaluation->quality_flags = quality;
            return 0;
        }
        if (!hwa_production_has_usable_room_ir(result)) {
            evaluation->availability = HWA_PRODUCTION_INSUFFICIENT;
            evaluation->quality_flags =
                quality | HWA_PRODUCTION_QUALITY_LOW_COVERAGE;
            return 0;
        }
        if (kind == HWA_PRODUCTION_METRIC_EARLY_REFLECTION_DB ||
            kind == HWA_PRODUCTION_METRIC_LATE_DECAY_SECONDS) {
            quality |= HWA_PRODUCTION_QUALITY_IR_SUPPLIED;
            evaluation->evidence_flags |= HWA_PRODUCTION_EVIDENCE_ROOM_IR;
        }
    }
    evaluation->quality_flags = quality;
    if (kind == HWA_PRODUCTION_METRIC_CREST_DB ||
        (view == HWA_PRODUCTION_VIEW_DRY_LIKE &&
         (kind == HWA_PRODUCTION_METRIC_EARLY_REFLECTION_DB ||
          kind == HWA_PRODUCTION_METRIC_LATE_DECAY_SECONDS))) return 0;
    if (raw->availability != HWA_PRODUCTION_AVAILABLE) {
        evaluation->availability = raw->availability;
        return 0;
    }
    required_flags = hwa_production_metric_eligibility_flags(view, kind);
    if (!hwa_production_raw_metric_applicable(span, kind)) {
        evaluation->availability = HWA_PRODUCTION_INSUFFICIENT;
        return 0;
    }
    if (span->split == HWA_PRODUCTION_CHECK && required_flags != 0U &&
        !hwa_production_corrected_check_sufficient(
            result, kind, metric_offset)) {
        evaluation->availability = HWA_PRODUCTION_INSUFFICIENT;
        evaluation->quality_flags |= HWA_PRODUCTION_QUALITY_LOW_COVERAGE;
        return 0;
    }
    reference_value = raw->reference_value;
    model_value = raw->model_value;
    threshold = hwa_production_find_fit_const(
        result, HWA_PRODUCTION_SCOPE_CORRECTION,
        HWA_PRODUCTION_FIT_THRESHOLD_DBFS, 0U);
    ratio = hwa_production_find_fit_const(
        result, HWA_PRODUCTION_SCOPE_CORRECTION,
        HWA_PRODUCTION_FIT_RATIO, 0U);
    makeup = hwa_production_find_fit_const(
        result, HWA_PRODUCTION_SCOPE_CORRECTION,
        HWA_PRODUCTION_FIT_MAKEUP_DB, 0U);

    if (kind == HWA_PRODUCTION_METRIC_RMS_DBFS ||
        kind == HWA_PRODUCTION_METRIC_BAND_LEVEL_DBFS) {
        double level_change;
        int dynamics_fits_available = 1;
        const HWAProductionEvaluation *raw_rms =
            &result->evaluations[
                span_index * HWA_PRODUCTION_EVALUATIONS_PER_SPAN];
        if (!hwa_production_fit_available(
                threshold, &missing, &quality)) {
            dynamics_fits_available = 0;
        }
        if (!hwa_production_fit_available(ratio, &missing, &quality)) {
            dynamics_fits_available = 0;
        }
        if (!hwa_production_fit_available(makeup, &missing, &quality)) {
            dynamics_fits_available = 0;
        }
        if (!dynamics_fits_available ||
            raw_rms->availability != HWA_PRODUCTION_AVAILABLE) {
            goto unavailable;
        }
        if (view == HWA_PRODUCTION_VIEW_DRY_LIKE) {
            double inverse = hwa_production_inverse_level(
                raw_rms->reference_value, threshold->estimate, ratio->estimate,
                makeup->estimate);
            level_change = inverse - raw_rms->reference_value;
            if (kind == HWA_PRODUCTION_METRIC_RMS_DBFS) {
                reference_value = inverse;
            }
        } else {
            double forward = hwa_production_forward_level(
                raw_rms->model_value, threshold->estimate, ratio->estimate,
                makeup->estimate);
            level_change = forward - raw_rms->model_value;
            if (kind == HWA_PRODUCTION_METRIC_RMS_DBFS) {
                model_value = forward;
            }
        }
        if (kind == HWA_PRODUCTION_METRIC_BAND_LEVEL_DBFS) {
            size_t lower;
            size_t upper;
            double fraction;
            const HWAProductionFit *low_fit;
            const HWAProductionFit *high_fit;
            double eq;
            uint32_t reference_rate = result->sources[1].format.sample_rate_hz;
            uint32_t model_rate = result->sources[3].format.sample_rate_hz;
            if (hwa_production_eq_interpolation(
                    metric_index, reference_rate, model_rate,
                    &lower, &upper, &fraction) != 0) goto unavailable;
            low_fit = hwa_production_find_fit_const(
                result, HWA_PRODUCTION_SCOPE_CORRECTION,
                HWA_PRODUCTION_FIT_EQ_GAIN_DB, (uint32_t)lower);
            high_fit = hwa_production_find_fit_const(
                result, HWA_PRODUCTION_SCOPE_CORRECTION,
                HWA_PRODUCTION_FIT_EQ_GAIN_DB, (uint32_t)upper);
            evaluation->evidence_flags |= HWA_PRODUCTION_EVIDENCE_SAMPLES;
            int eq_fits_available = 1;
            if (!hwa_production_fit_available(
                    low_fit, &missing, &quality)) {
                eq_fits_available = 0;
            }
            if (!hwa_production_fit_available(
                    high_fit, &missing, &quality)) {
                eq_fits_available = 0;
            }
            if (!eq_fits_available) {
                goto unavailable;
            }
            eq = low_fit->estimate +
                 (high_fit->estimate - low_fit->estimate) * fraction;
            if (view == HWA_PRODUCTION_VIEW_DRY_LIKE) {
                reference_value = raw->reference_value + level_change - eq;
            } else {
                model_value = raw->model_value + level_change + eq;
            }
        }
    } else if (kind == HWA_PRODUCTION_METRIC_LEVEL_SPREAD_DB) {
        evaluation->evidence_flags |= HWA_PRODUCTION_EVIDENCE_SAMPLES;
        if (!hwa_production_fit_available(
                ratio, &missing, &quality)) goto unavailable;
        if (view == HWA_PRODUCTION_VIEW_DRY_LIKE) {
            reference_value *= ratio->estimate;
        } else {
            model_value /= ratio->estimate;
        }
    } else if (kind == HWA_PRODUCTION_METRIC_CHANNEL_BALANCE_DB) {
        const HWAProductionFit *fit = hwa_production_find_fit_const(
            result, HWA_PRODUCTION_SCOPE_CORRECTION,
            HWA_PRODUCTION_FIT_CHANNEL_BALANCE_DB, 0U);
        if (!hwa_production_fit_available(
                fit, &missing, &quality)) goto unavailable;
        if (view == HWA_PRODUCTION_VIEW_DRY_LIKE) reference_value -= fit->estimate;
        else model_value += fit->estimate;
    } else if (kind == HWA_PRODUCTION_METRIC_STEREO_WIDTH_RATIO) {
        const HWAProductionFit *fit = hwa_production_find_fit_const(
            result, HWA_PRODUCTION_SCOPE_CORRECTION,
            HWA_PRODUCTION_FIT_STEREO_WIDTH_RATIO, 0U);
        if (!hwa_production_fit_available(
                fit, &missing, &quality)) goto unavailable;
        if (view == HWA_PRODUCTION_VIEW_DRY_LIKE) reference_value /= fit->estimate;
        else model_value *= fit->estimate;
    } else if (kind == HWA_PRODUCTION_METRIC_STEREO_CORRELATION) {
        const HWAProductionFit *polarity = hwa_production_find_fit_const(
            result, HWA_PRODUCTION_SCOPE_CORRECTION,
            HWA_PRODUCTION_FIT_CHANNEL_POLARITY, 0U);
        const HWAProductionFit *delta = hwa_production_find_fit_const(
            result, HWA_PRODUCTION_SCOPE_CORRECTION,
            HWA_PRODUCTION_FIT_STEREO_CORRELATION, 0U);
        int correlation_fits_available = 1;
        if (!hwa_production_fit_available(
                polarity, &missing, &quality)) {
            correlation_fits_available = 0;
        }
        if (!hwa_production_fit_available(
                delta, &missing, &quality)) {
            correlation_fits_available = 0;
        }
        if (!correlation_fits_available) goto unavailable;
        if (view == HWA_PRODUCTION_VIEW_DRY_LIKE) {
            reference_value = polarity->estimate *
                              (reference_value - delta->estimate);
        } else {
            model_value = polarity->estimate * model_value + delta->estimate;
        }
        reference_value = hwa_production_clamp(reference_value, -1.0, 1.0);
        model_value = hwa_production_clamp(model_value, -1.0, 1.0);
    } else if (kind == HWA_PRODUCTION_METRIC_STEREO_DELAY_SAMPLES) {
        const HWAProductionFit *fit = hwa_production_find_fit_const(
            result, HWA_PRODUCTION_SCOPE_CORRECTION,
            HWA_PRODUCTION_FIT_STEREO_DELAY_SAMPLES, 0U);
        if (!hwa_production_fit_available(
                fit, &missing, &quality)) goto unavailable;
        if (view == HWA_PRODUCTION_VIEW_DRY_LIKE) reference_value -= fit->estimate;
        else model_value += fit->estimate;
    } else if (view == HWA_PRODUCTION_VIEW_ROOM_MATCHED &&
               kind == HWA_PRODUCTION_METRIC_EARLY_REFLECTION_DB) {
        const HWAProductionFit *fit = hwa_production_find_fit_const(
            result, HWA_PRODUCTION_SCOPE_ROOM_IR,
            HWA_PRODUCTION_FIT_EARLY_REFLECTION_DB, metric_index);
        if (!hwa_production_fit_available(
                fit, &missing, &quality)) goto unavailable;
        model_value = hwa_production_clamp(model_value + fit->estimate,
                                           -120.0, 60.0);
    } else if (view == HWA_PRODUCTION_VIEW_ROOM_MATCHED &&
               kind == HWA_PRODUCTION_METRIC_LATE_DECAY_SECONDS) {
        const HWAProductionFit *fit = hwa_production_find_fit_const(
            result, HWA_PRODUCTION_SCOPE_ROOM_IR,
            HWA_PRODUCTION_FIT_LATE_DECAY_SECONDS, metric_index);
        if (!hwa_production_fit_available(
                fit, &missing, &quality)) goto unavailable;
        if (fit->estimate > model_value) model_value = fit->estimate;
    } else {
        goto unavailable;
    }
    if (!hwa_production_metric_value_valid(
            kind, metric_index, reference_value,
            result->sources[1].format.sample_rate_hz) ||
        !hwa_production_metric_value_valid(
            kind, metric_index, model_value,
            result->sources[3].format.sample_rate_hz)) {
        missing = HWA_PRODUCTION_INSUFFICIENT;
        goto unavailable;
    }
    evaluation->availability = HWA_PRODUCTION_AVAILABLE;
    evaluation->quality_flags = quality;
    evaluation->reference_value = reference_value == 0.0 ? 0.0 : reference_value;
    evaluation->model_value = model_value == 0.0 ? 0.0 : model_value;
    evaluation->delta = evaluation->model_value - evaluation->reference_value;
    evaluation->confidence = raw->confidence;
    evaluation->reference_valid = 1;
    evaluation->model_valid = 1;
    evaluation->delta_valid = 1;
    return 0;

unavailable:
    evaluation->quality_flags = quality;
    evaluation->availability = missing;
    return 0;
}

static int hwa_production_profile_method_match(
    const HWAMeasurementSet *reference,
    const HWAMeasurementSet *model,
    HWAProductionProfileMethod *method)
{
    if (reference->options.fft_size != model->options.fft_size ||
        reference->options.hop_size != model->options.hop_size ||
        reference->options.pitch_confidence_floor !=
            model->options.pitch_confidence_floor ||
        reference->options.spectral_floor_dbfs !=
            model->options.spectral_floor_dbfs ||
        reference->options.max_partials != model->options.max_partials) {
        return 0;
    }
    method->fft_size = reference->options.fft_size;
    method->hop_size = reference->options.hop_size;
    method->pitch_confidence_floor =
        reference->options.pitch_confidence_floor;
    method->spectral_floor_dbfs = reference->options.spectral_floor_dbfs;
    method->max_partials = reference->options.max_partials;
    return 1;
}

static int hwa_production_set_source(
    HWAProductionSource *source,
    uint64_t id,
    const char *role,
    const char *path,
    const char sha256[HWA_SHA256_HEX_SIZE],
    const HWAFormat *format,
    int is_wave,
    char *error,
    size_t error_size)
{
    source->id = id;
    source->role = hwa_production_copy(role);
    source->path = hwa_production_copy(path);
    if (source->role == NULL || source->path == NULL) {
        hwa_set_error(error, error_size,
                      "cannot allocate Stage 6 source metadata");
        return -1;
    }
    memcpy(source->sha256, sha256, HWA_SHA256_HEX_SIZE);
    source->is_wave = is_wave;
    if (is_wave && format != NULL) source->format = *format;
    return 0;
}

static void hwa_production_span_values_free(
    HWAProductionSpanValues *values,
    size_t count)
{
    size_t index;
    if (values == NULL) return;
    for (index = 0U; index < count; ++index) {
        free(values[index].reference_envelope.points);
        free(values[index].model_envelope.points);
    }
    free(values);
}

static int hwa_production_build_raw_evaluations(
    HWAProductionResult *result,
    const HWAProductionSpanValues *values,
    char *error,
    size_t error_size)
{
    size_t span_index;
    size_t metric_count = hwa_production_metric_catalog_count();
    for (span_index = 0U; span_index < result->span_count; ++span_index) {
        size_t metric_offset;
        const HWAProductionSpan *span = &result->spans[span_index];
        for (metric_offset = 0U; metric_offset < metric_count;
             ++metric_offset) {
            size_t row_index =
                span_index * HWA_PRODUCTION_EVALUATIONS_PER_SPAN +
                metric_offset;
            HWAProductionEvaluation *row = &result->evaluations[row_index];
            const HWAProductionMetricValue *reference =
                &values[span_index].reference[metric_offset];
            const HWAProductionMetricValue *model =
                &values[span_index].model[metric_offset];
            memset(row, 0, sizeof(*row));
            row->id = (uint64_t)row_index + 1U;
            row->span_id = span->id;
            row->view = HWA_PRODUCTION_VIEW_RAW;
            if (hwa_production_metric_catalog_at(
                    metric_offset, &row->kind, &row->index,
                    &row->unit) != 0) {
                hwa_set_error(error, error_size,
                              "invalid Stage 6 metric catalog");
                return -1;
            }
            if (span->split == HWA_PRODUCTION_CHECK) {
                row->evidence_flags |= HWA_PRODUCTION_EVIDENCE_HELD_OUT;
            }
            if (!hwa_production_raw_metric_applicable(span, row->kind)) {
                row->availability = HWA_PRODUCTION_UNAVAILABLE;
                continue;
            }
            if ((row->kind == HWA_PRODUCTION_METRIC_EARLY_REFLECTION_DB ||
                 row->kind == HWA_PRODUCTION_METRIC_LATE_DECAY_SECONDS) &&
                (!hwa_production_room_band_supported(
                     row->index, result->sources[1].format.sample_rate_hz) ||
                 !hwa_production_room_band_supported(
                     row->index, result->sources[3].format.sample_rate_hz))) {
                row->availability = HWA_PRODUCTION_UNAVAILABLE;
                continue;
            }
            row->evidence_flags |=
                reference->evidence_flags | model->evidence_flags;
            row->quality_flags =
                reference->quality_flags | model->quality_flags;
            if (!reference->valid || !model->valid ||
                !hwa_production_metric_value_valid(
                    row->kind, row->index, reference->value,
                    result->sources[1].format.sample_rate_hz) ||
                !hwa_production_metric_value_valid(
                    row->kind, row->index, model->value,
                    result->sources[3].format.sample_rate_hz)) {
                row->availability = HWA_PRODUCTION_INSUFFICIENT;
                row->quality_flags |= HWA_PRODUCTION_QUALITY_LOW_COVERAGE;
                continue;
            }
            row->availability = HWA_PRODUCTION_AVAILABLE;
            row->reference_value =
                reference->value == 0.0 ? 0.0 : reference->value;
            row->model_value = model->value == 0.0 ? 0.0 : model->value;
            row->delta = row->model_value - row->reference_value;
            row->confidence = reference->confidence < model->confidence ?
                reference->confidence : model->confidence;
            row->reference_valid = 1;
            row->model_valid = 1;
            row->delta_valid = 1;
        }
    }
    return 0;
}

static int hwa_production_fill_corrected_evaluations(
    HWAProductionResult *result,
    char *error,
    size_t error_size)
{
    size_t span_index;
    for (span_index = 0U; span_index < result->span_count; ++span_index) {
        HWAProductionView view;
        for (view = HWA_PRODUCTION_VIEW_DRY_LIKE;
             view < HWA_PRODUCTION_VIEW_COUNT;
             view = (HWAProductionView)((int)view + 1)) {
            size_t metric_offset;
            for (metric_offset = 0U;
                 metric_offset < hwa_production_metric_catalog_count();
                 ++metric_offset) {
                size_t row_index =
                    span_index * HWA_PRODUCTION_EVALUATIONS_PER_SPAN +
                    ((size_t)view - 1U) *
                        HWA_PRODUCTION_METRICS_PER_VIEW +
                    metric_offset;
                if (hwa_production_evaluation_derive(
                        result, span_index, view, metric_offset,
                        &result->evaluations[row_index],
                        error, error_size) != 0) return -1;
            }
        }
    }
    return 0;
}

static int hwa_production_add_array_work(
    uint64_t *work,
    size_t count,
    size_t element,
    uint64_t limit)
{
    uint64_t bytes;
    return hwa_production_array_bytes(count, element, &bytes) != 0 ||
           hwa_production_add_size(work, bytes, limit) != 0 ? -1 : 0;
}

static int hwa_production_add_string_work(
    uint64_t *work,
    const char *text,
    uint64_t limit)
{
    size_t length;
    if (text == NULL) return -1;
    length = strlen(text);
    return length == SIZE_MAX ||
           hwa_production_add_size(
               work, (uint64_t)length + 1U, limit) != 0 ? -1 : 0;
}

static int hwa_production_build_warnings(
    HWAProductionResult *result,
    char *error,
    size_t error_size)
{
    size_t count = hwa_production_warning_spec_count(result);
    uint64_t work = result->retained_work_bytes;
    size_t index;
    if (count > result->options.max_warnings ||
        hwa_production_add_array_work(
            &work, count, sizeof(*result->warnings),
            result->options.max_work_bytes) != 0) {
        hwa_set_error(error, error_size,
                      "Stage 6 warnings exceed their caps");
        return -1;
    }
    for (index = 0U; index < count; ++index) {
        HWAProductionWarningSpec spec;
        if (hwa_production_warning_spec_at(result, index, &spec) != 0 ||
            hwa_production_add_string_work(
                &work, spec.code, result->options.max_work_bytes) != 0 ||
            hwa_production_add_string_work(
                &work, spec.message, result->options.max_work_bytes) != 0) {
            hwa_set_error(error, error_size,
                          "Stage 6 warning strings exceed the work cap");
            return -1;
        }
    }
    result->warnings = count == 0U ? NULL :
        (HWAProductionWarning *)calloc(count, sizeof(*result->warnings));
    if (count != 0U && result->warnings == NULL) {
        hwa_set_error(error, error_size,
                      "cannot allocate Stage 6 warnings");
        return -1;
    }
    result->warning_count = count;
    for (index = 0U; index < count; ++index) {
        HWAProductionWarningSpec spec;
        HWAProductionWarning *warning = &result->warnings[index];
        if (hwa_production_warning_spec_at(result, index, &spec) != 0) {
            hwa_set_error(error, error_size,
                          "invalid Stage 6 warning catalog");
            return -1;
        }
        warning->id = (uint64_t)index + 1U;
        warning->code = hwa_production_copy(spec.code);
        warning->message = hwa_production_copy(spec.message);
        if (warning->code == NULL || warning->message == NULL) {
            hwa_set_error(error, error_size,
                          "cannot allocate Stage 6 warning text");
            return -1;
        }
        warning->span_id = spec.span_id;
        warning->fit_id = spec.fit_id;
        warning->span_id_valid = spec.span_id_valid;
        warning->fit_id_valid = spec.fit_id_valid;
    }
    return 0;
}

static int hwa_production_verify_named_unchanged(
    const char *path,
    uint64_t max_bytes,
    const HWAProductionFileIdentity *before_identity,
    const char before_hash[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    HWAProductionFileIdentity after_identity;
    char after_hash[HWA_SHA256_HEX_SIZE];
    if (hwa_production_hash_named_file(
            path, max_bytes, &after_identity, after_hash,
            error, error_size) != 0 ||
        !hwa_production_identity_equal(before_identity, &after_identity) ||
        strcmp(before_hash, after_hash) != 0) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "Stage 6 input changed during analysis: %s", path);
        }
        return -1;
    }
    return 0;
}

void hwa_production_options_default(HWAProductionOptions *options)
{
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    options->decode_block_frames = 4096U;
    options->max_input_bytes = UINT64_C(17179869184);
    options->max_input_frames = UINT64_C(2000000000);
    options->max_ir_frames = UINT64_C(262144);
    options->max_work_bytes = UINT64_C(536870912);
    options->max_evaluations = UINT64_C(100000000);
    options->max_spans = 200000U;
    options->max_envelope_points = 2000000U;
    options->max_fits = 256U;
    options->max_evaluation_rows = 4000000U;
    options->max_view_rows = 256U;
    options->max_warnings = 100000U;
    hwa_profile_comparison_options_default(&options->profile_limits);
}

void hwa_production_result_free(HWAProductionResult *result)
{
    size_t index;
    if (result == NULL) return;
    if (result->sources != NULL) {
        for (index = 0U; index < result->source_count; ++index) {
            free(result->sources[index].role);
            free(result->sources[index].path);
        }
    }
    if (result->spans != NULL) {
        for (index = 0U; index < result->span_count; ++index) {
            free(result->spans[index].item_key);
            free(result->spans[index].item_role);
        }
    }
    if (result->warnings != NULL) {
        for (index = 0U; index < result->warning_count; ++index) {
            free(result->warnings[index].code);
            free(result->warnings[index].message);
        }
    }
    free(result->sources);
    free(result->spans);
    free(result->fits);
    free(result->evaluations);
    free(result->view_rows);
    free(result->warnings);
    memset(result, 0, sizeof(*result));
}

static int hwa_production_account_impl(
    const HWAProductionInputs *inputs,
    const HWAProductionOptions *options,
    HWAProductionResult *result,
    char *error,
    size_t error_size)
{
    HWAMeasurementSet reference;
    HWAMeasurementSet model;
    HWAProfileComparisonOptions read_limits;
    HWAProductionMatch *matches = NULL;
    HWAProductionSpanValues *values = NULL;
    HWAProductionRoomFacts room;
    HWAProductionFitState fit_state;
    HWAProductionFileIdentity reference_identity;
    HWAProductionFileIdentity model_identity;
    HWAProductionFileIdentity reference_audio_identity;
    HWAProductionFileIdentity model_audio_identity;
    HWAProductionFileIdentity room_ir_identity;
    char reference_hash[HWA_SHA256_HEX_SIZE];
    char model_hash[HWA_SHA256_HEX_SIZE];
    char read_hash[HWA_SHA256_HEX_SIZE];
    char empty_hash[HWA_SHA256_HEX_SIZE];
    uint64_t profile_live = 0U;
    uint64_t work;
    uint64_t values_bytes = 0U;
    uint64_t envelope_bytes = 0U;
    uint64_t bytes;
    size_t envelope_points = 0U;
    size_t match_count = 0U;
    size_t candidate_cap;
    size_t source_count;
    size_t index;
    int status = -1;

    memset(&reference, 0, sizeof(reference));
    memset(&model, 0, sizeof(model));
    memset(&room, 0, sizeof(room));
    memset(&fit_state, 0, sizeof(fit_state));
    memset(empty_hash, '0', HWA_SHA256_HEX_SIZE - 1U);
    empty_hash[HWA_SHA256_HEX_SIZE - 1U] = '\0';
    if (inputs == NULL ||
        inputs->reference_measures_path == NULL ||
        inputs->reference_measures_path[0] == '\0' ||
        inputs->reference_audio_path == NULL ||
        inputs->reference_audio_path[0] == '\0' ||
        inputs->model_measures_path == NULL ||
        inputs->model_measures_path[0] == '\0' ||
        inputs->model_audio_path == NULL ||
        inputs->model_audio_path[0] == '\0' ||
        strcmp(inputs->reference_measures_path, "-") == 0 ||
        strcmp(inputs->reference_audio_path, "-") == 0 ||
        strcmp(inputs->model_measures_path, "-") == 0 ||
        strcmp(inputs->model_audio_path, "-") == 0 ||
        (inputs->room_ir_path != NULL &&
         (inputs->room_ir_path[0] == '\0' ||
          strcmp(inputs->room_ir_path, "-") == 0)) ||
        !hwa_production_options_valid(options, error, error_size)) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_set_error(error, error_size, "invalid Stage 6 inputs");
        }
        goto cleanup;
    }
    if (hwa_production_hash_named_file(
            inputs->reference_measures_path,
            options->profile_limits.max_input_bytes,
            &reference_identity, reference_hash,
            error, error_size) != 0) goto cleanup;
    read_limits = options->profile_limits;
    if (read_limits.max_work_bytes > options->max_work_bytes) {
        read_limits.max_work_bytes = options->max_work_bytes;
    }
    if (hwa_measure_file_read(
            inputs->reference_measures_path, &read_limits,
            &reference, read_hash, error, error_size) != 0 ||
        strcmp(reference_hash, read_hash) != 0 ||
        reference.retained_work_bytes > options->max_work_bytes) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "reference Stage 4 profile changed while read");
        }
        goto cleanup;
    }
    profile_live = reference.retained_work_bytes;
    if (profile_live >= options->max_work_bytes) {
        hwa_set_error(error, error_size,
                      "reference profile leaves no model work space");
        goto cleanup;
    }
    if (hwa_production_hash_named_file(
            inputs->model_measures_path,
            options->profile_limits.max_input_bytes,
            &model_identity, model_hash,
            error, error_size) != 0) goto cleanup;
    read_limits = options->profile_limits;
    if (read_limits.max_work_bytes > options->max_work_bytes - profile_live) {
        read_limits.max_work_bytes = options->max_work_bytes - profile_live;
    }
    if (read_limits.max_work_bytes == 0U ||
        hwa_measure_file_read(
            inputs->model_measures_path, &read_limits,
            &model, read_hash, error, error_size) != 0 ||
        strcmp(model_hash, read_hash) != 0 ||
        model.retained_work_bytes > options->max_work_bytes - profile_live) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "model Stage 4 profile exceeds the work cap or changed");
        }
        goto cleanup;
    }
    profile_live += model.retained_work_bytes;
    if (!hwa_production_profile_method_match(
            &reference, &model, &result->profile_method)) {
        hwa_set_error(error, error_size,
                      "Stage 4 profile shaping methods do not match");
        goto cleanup;
    }
    if (reference.audio_format.sample_rate_hz !=
        model.audio_format.sample_rate_hz) {
        hwa_set_error(error, error_size,
                      "Stage 6 requires equal audio sample rates");
        goto cleanup;
    }

    candidate_cap = reference.context_count < model.context_count ?
        reference.context_count : model.context_count;
    work = profile_live;
    if (hwa_production_add_array_work(
            &work, reference.context_count,
            sizeof(HWAProductionContextRef), options->max_work_bytes) != 0 ||
        hwa_production_add_array_work(
            &work, model.context_count,
            sizeof(HWAProductionContextRef), options->max_work_bytes) != 0 ||
        hwa_production_add_array_work(
            &work, candidate_cap, sizeof(HWAProductionMatch),
            options->max_work_bytes) != 0 ||
        hwa_production_add_array_work(
            &work, candidate_cap, sizeof(HWAProductionRankedMatch),
            options->max_work_bytes) != 0) {
        hwa_set_error(error, error_size,
                      "Stage 6 match index exceeds the work cap");
        goto cleanup;
    }
    if (hwa_production_build_matches(
            &reference, &model, options, &matches, &match_count,
            error, error_size) != 0) goto cleanup;

    source_count = inputs->room_ir_path == NULL ? 4U : 5U;
    work = profile_live;
    if (hwa_production_add_array_work(
            &work, candidate_cap, sizeof(HWAProductionMatch),
            options->max_work_bytes) != 0 ||
        hwa_production_add_array_work(
            &work, source_count, sizeof(*result->sources),
            options->max_work_bytes) != 0 ||
        hwa_production_add_array_work(
            &work, match_count, sizeof(*result->spans),
            options->max_work_bytes) != 0 ||
        hwa_production_add_array_work(
            &work, match_count, sizeof(*values),
            options->max_work_bytes) != 0) {
        hwa_set_error(error, error_size,
                      "Stage 6 retained span work exceeds its cap");
        goto cleanup;
    }
    if (hwa_production_add_string_work(
            &work, "reference:profile", options->max_work_bytes) != 0 ||
        hwa_production_add_string_work(
            &work, inputs->reference_measures_path,
            options->max_work_bytes) != 0 ||
        hwa_production_add_string_work(
            &work, "reference:audio", options->max_work_bytes) != 0 ||
        hwa_production_add_string_work(
            &work, inputs->reference_audio_path,
            options->max_work_bytes) != 0 ||
        hwa_production_add_string_work(
            &work, "model:profile", options->max_work_bytes) != 0 ||
        hwa_production_add_string_work(
            &work, inputs->model_measures_path,
            options->max_work_bytes) != 0 ||
        hwa_production_add_string_work(
            &work, "model:audio", options->max_work_bytes) != 0 ||
        hwa_production_add_string_work(
            &work, inputs->model_audio_path,
            options->max_work_bytes) != 0 ||
        (source_count == 5U &&
         (hwa_production_add_string_work(
              &work, "room-ir", options->max_work_bytes) != 0 ||
          hwa_production_add_string_work(
              &work, inputs->room_ir_path,
              options->max_work_bytes) != 0))) {
        hwa_set_error(error, error_size,
                      "Stage 6 source strings exceed the work cap");
        goto cleanup;
    }
    for (index = 0U; index < match_count; ++index) {
        if (hwa_production_add_string_work(
                &work, matches[index].reference->item_key,
                options->max_work_bytes) != 0 ||
            hwa_production_add_string_work(
                &work, matches[index].reference->item_role,
                options->max_work_bytes) != 0) {
            hwa_set_error(error, error_size,
                          "Stage 6 span strings exceed the work cap");
            goto cleanup;
        }
    }
    result->sources = (HWAProductionSource *)calloc(
        source_count, sizeof(*result->sources));
    result->spans = (HWAProductionSpan *)calloc(
        match_count, sizeof(*result->spans));
    values = (HWAProductionSpanValues *)calloc(match_count, sizeof(*values));
    if (result->sources == NULL || result->spans == NULL || values == NULL) {
        hwa_set_error(error, error_size,
                      "cannot allocate Stage 6 retained rows");
        goto cleanup;
    }
    result->source_count = source_count;
    result->span_count = match_count;
    if (hwa_production_set_source(
            &result->sources[0], 1U, "reference:profile",
            inputs->reference_measures_path, reference_hash,
            NULL, 0, error, error_size) != 0 ||
        hwa_production_set_source(
            &result->sources[1], 2U, "reference:audio",
            inputs->reference_audio_path, reference.audio_sha256,
            &reference.audio_format, 1, error, error_size) != 0 ||
        hwa_production_set_source(
            &result->sources[2], 3U, "model:profile",
            inputs->model_measures_path, model_hash,
            NULL, 0, error, error_size) != 0 ||
        hwa_production_set_source(
            &result->sources[3], 4U, "model:audio",
            inputs->model_audio_path, model.audio_sha256,
            &model.audio_format, 1, error, error_size) != 0 ||
        (source_count == 5U && hwa_production_set_source(
            &result->sources[4], 5U, "room-ir", inputs->room_ir_path,
            empty_hash, NULL, 1, error, error_size) != 0)) goto cleanup;
    for (index = 0U; index < match_count; ++index) {
        HWAProductionSpan *span = &result->spans[index];
        span->id = (uint64_t)index + 1U;
        span->item_key = hwa_production_copy(matches[index].reference->item_key);
        span->item_role = hwa_production_copy(
            matches[index].reference->item_role);
        if (span->item_key == NULL || span->item_role == NULL) {
            hwa_set_error(error, error_size,
                          "cannot allocate Stage 6 span metadata");
            goto cleanup;
        }
        span->item_kind = matches[index].reference->item_kind;
        span->split = matches[index].split;
        span->reference_item_id = matches[index].reference->item_id;
        span->reference_start_sample = matches[index].reference->start_sample;
        span->reference_end_sample = matches[index].reference->end_sample;
        span->model_item_id = matches[index].model->item_id;
        span->model_start_sample = matches[index].model->start_sample;
        span->model_end_sample = matches[index].model->end_sample;
        span->eligibility_flags = matches[index].eligibility_flags;
        hwa_production_fill_profile_values(
            &reference, span->reference_item_id, values[index].reference);
        hwa_production_fill_profile_values(
            &model, span->model_item_id, values[index].model);
    }
    if (hwa_production_result_retained_bytes(
            result, &result->retained_work_bytes) != 0 ||
        hwa_production_array_bytes(
            match_count, sizeof(*values), &values_bytes) != 0 ||
        result->retained_work_bytes > options->max_work_bytes - profile_live ||
        values_bytes > options->max_work_bytes - profile_live -
                           result->retained_work_bytes) {
        hwa_set_error(error, error_size,
                      "Stage 6 profile rows exceed the work cap");
        goto cleanup;
    }
    free(matches);
    matches = NULL;
    hwa_measurement_set_free(&model);
    hwa_measurement_set_free(&reference);
    profile_live = 0U;

    if (hwa_production_path_identity(
            inputs->reference_audio_path, &reference_audio_identity,
            error, error_size) != 0 ||
        hwa_production_path_identity(
            inputs->model_audio_path, &model_audio_identity,
            error, error_size) != 0 ||
        (source_count == 5U && hwa_production_path_identity(
            inputs->room_ir_path, &room_ir_identity,
            error, error_size) != 0) ||
        hwa_production_stream_audio(
            inputs->reference_audio_path, &result->sources[1].format,
            result->sources[1].sha256, options, result->spans,
            result->span_count, 1, values, &result->sources[1].format,
            result->sources[1].sha256, &result->evaluation_count,
            result->retained_work_bytes + values_bytes,
            &envelope_bytes, &envelope_points,
            error, error_size) != 0 ||
        hwa_production_stream_audio(
            inputs->model_audio_path, &result->sources[3].format,
            result->sources[3].sha256, options, result->spans,
            result->span_count, 0, values, &result->sources[3].format,
            result->sources[3].sha256, &result->evaluation_count,
            result->retained_work_bytes + values_bytes,
            &envelope_bytes, &envelope_points,
            error, error_size) != 0) goto cleanup;
    if (source_count == 5U) {
        if (envelope_bytes > options->max_work_bytes -
                                 result->retained_work_bytes - values_bytes ||
            hwa_production_analyze_room_ir(
                inputs->room_ir_path, options, &room,
                &result->sources[4].format,
                result->sources[4].sha256, &result->evaluation_count,
                result->retained_work_bytes + values_bytes + envelope_bytes,
                error, error_size) != 0) goto cleanup;
    }

    work = result->retained_work_bytes;
    if (hwa_production_add_size(
            &work, values_bytes, options->max_work_bytes) != 0 ||
        hwa_production_add_size(
            &work, envelope_bytes, options->max_work_bytes) != 0 ||
        hwa_production_add_array_work(
            &work, hwa_production_fit_catalog_count(),
            sizeof(*result->fits), options->max_work_bytes) != 0 ||
        hwa_production_add_array_work(
            &work, match_count, 4U * sizeof(double),
            options->max_work_bytes) != 0 ||
        hwa_production_add_array_work(
            &work, envelope_points,
            sizeof(HWAProductionDynamicsPoint) + 2U * sizeof(double),
            options->max_work_bytes) != 0 ||
        hwa_production_add_array_work(
            &work, match_count, 3U * sizeof(double),
            options->max_work_bytes) != 0) {
        hwa_set_error(error, error_size,
                      "Stage 6 fit scratch exceeds the work cap");
        goto cleanup;
    }
    if (hwa_production_build_fits(
            result, values, &room, &fit_state,
            result->sources[1].format.sample_rate_hz,
            result->sources[3].format.sample_rate_hz,
            &result->evaluation_count, error, error_size) != 0 ||
        hwa_production_result_retained_bytes(
            result, &result->retained_work_bytes) != 0) goto cleanup;

    for (index = 0U; index < match_count; ++index) {
        free(values[index].reference_envelope.points);
        values[index].reference_envelope.points = NULL;
        free(values[index].model_envelope.points);
        values[index].model_envelope.points = NULL;
    }
    envelope_bytes = 0U;
    if (match_count > SIZE_MAX / HWA_PRODUCTION_EVALUATIONS_PER_SPAN) {
        hwa_set_error(error, error_size,
                      "Stage 6 evaluation row count overflows");
        goto cleanup;
    }
    result->evaluation_row_count =
        match_count * HWA_PRODUCTION_EVALUATIONS_PER_SPAN;
    if (result->evaluation_row_count > options->max_evaluation_rows ||
        hwa_production_array_bytes(
            result->evaluation_row_count, sizeof(*result->evaluations),
            &bytes) != 0 ||
        result->retained_work_bytes > options->max_work_bytes - values_bytes ||
        bytes > options->max_work_bytes - values_bytes -
                    result->retained_work_bytes) {
        hwa_set_error(error, error_size,
                      "Stage 6 evaluation rows exceed their caps");
        goto cleanup;
    }
    result->evaluations = (HWAProductionEvaluation *)calloc(
        result->evaluation_row_count, sizeof(*result->evaluations));
    if (result->evaluations == NULL ||
        hwa_production_build_raw_evaluations(
            result, values, error, error_size) != 0 ||
        hwa_production_fill_corrected_evaluations(
            result, error, error_size) != 0) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "cannot allocate Stage 6 evaluation rows");
        }
        goto cleanup;
    }
    hwa_production_span_values_free(values, match_count);
    values = NULL;
    values_bytes = 0U;
    if (hwa_production_result_retained_bytes(
            result, &result->retained_work_bytes) != 0 ||
        hwa_production_build_warnings(result, error, error_size) != 0 ||
        hwa_production_result_retained_bytes(
            result, &result->retained_work_bytes) != 0 ||
        hwa_production_view_rows_rebuild(result, error, error_size) != 0 ||
        hwa_production_verify_named_unchanged(
            inputs->reference_measures_path,
            options->profile_limits.max_input_bytes,
            &reference_identity, reference_hash,
            error, error_size) != 0 ||
        hwa_production_verify_named_unchanged(
            inputs->model_measures_path,
            options->profile_limits.max_input_bytes,
            &model_identity, model_hash,
            error, error_size) != 0 ||
        hwa_production_verify_named_unchanged(
            inputs->reference_audio_path, options->max_input_bytes,
            &reference_audio_identity, result->sources[1].sha256,
            error, error_size) != 0 ||
        hwa_production_verify_named_unchanged(
            inputs->model_audio_path, options->max_input_bytes,
            &model_audio_identity, result->sources[3].sha256,
            error, error_size) != 0 ||
        (source_count == 5U && hwa_production_verify_named_unchanged(
            inputs->room_ir_path, options->max_input_bytes,
            &room_ir_identity, result->sources[4].sha256,
            error, error_size) != 0) ||
        hwa_production_result_validate(result, error, error_size) != 0) {
        goto cleanup;
    }
    status = 0;

cleanup:
    free(matches);
    hwa_production_span_values_free(values, match_count);
    hwa_measurement_set_free(&model);
    hwa_measurement_set_free(&reference);
    if (status != 0) hwa_production_result_free(result);
    else if (error != NULL && error_size > 0U) error[0] = '\0';
    return status;
}

int hwa_account_production_files(const HWAProductionInputs *inputs,
                                 const HWAProductionOptions *options,
                                 HWAProductionResult *result,
                                 char *error,
                                 size_t error_size)
{
    HWAProductionOptions copied_options;
    const HWAProductionOptions *safe_options = options;
    HWANumericLocale locale;
    int status;
    int locale_status;
    if (result == NULL) {
        hwa_set_error(error, error_size, "missing Stage 6 result");
        return -1;
    }
    if (options == NULL) {
        hwa_production_options_default(&copied_options);
        safe_options = &copied_options;
    } else {
        copied_options = *options;
        safe_options = &copied_options;
    }
    if (error != NULL && error_size > 0U) error[0] = '\0';
    memset(result, 0, sizeof(*result));
    result->options = *safe_options;
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_set_error(error, error_size,
                      "cannot enter the C numeric locale for Stage 6");
        return -1;
    }
    status = hwa_production_account_impl(
        inputs, safe_options, result, error, error_size);
    locale_status = hwa_c_numeric_locale_end(&locale);
    if (locale_status != 0) {
        if (status == 0) hwa_production_result_free(result);
        if (error != NULL && error_size > 0U &&
            (status == 0 || error[0] == '\0')) {
            hwa_set_error(error, error_size,
                          "cannot restore the numeric locale after Stage 6");
        }
        return -1;
    }
    return status;
}

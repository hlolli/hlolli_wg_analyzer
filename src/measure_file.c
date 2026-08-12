#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "measure_file.h"

#include "alignment_file.h"
#include "internal.h"
#include "measure_compare.h"
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

#define HWA_MEASURE_FILE_MAX_FIELDS 40U
#define HWA_MEASURE_FILE_MAX_FIELD_BYTES 65536U
#define HWA_MEASURE_EVIDENCE_ALL ((UINT32_C(1) << 11) - 1U)
#define HWA_MEASURE_QUALITY_ALL ((UINT32_C(1) << 7) - 1U)
#define HWA_LABEL_OVERRIDE_ALL ((UINT32_C(1) << 11) - 1U)
/* Accept small libm/compiler drift when checking profiles from older builds. */
#define HWA_MEASURE_READER_STATISTIC_MAX_ULPS 32U

typedef struct HWAMeasureFileIdentity {
    uint64_t size;
#if defined(_WIN32)
    DWORD volume_serial_number;
    DWORD file_index_high;
    DWORD file_index_low;
#else
    dev_t device;
    ino_t inode;
#endif
} HWAMeasureFileIdentity;

typedef int (*HWAMeasureRowFunction)(char **fields,
                                     size_t field_count,
                                     size_t row,
                                     void *user,
                                     char *error,
                                     size_t error_size);

static int hwa_measure_valid_sha256(const char *text);
typedef struct HWAMeasureReadState HWAMeasureReadState;
static int hwa_measure_read_charge(HWAMeasureReadState *state,
                                   uint64_t bytes);
static int hwa_measure_read_row(char **fields,
                                size_t field_count,
                                size_t row,
                                void *user,
                                char *error,
                                size_t error_size);

static int hwa_measure_file_identity_equal(
    const HWAMeasureFileIdentity *first,
    const HWAMeasureFileIdentity *second)
{
    if (first == NULL || second == NULL || first->size != second->size) {
        return 0;
    }
#if defined(_WIN32)
    return first->volume_serial_number == second->volume_serial_number &&
           first->file_index_high == second->file_index_high &&
           first->file_index_low == second->file_index_low;
#else
    return first->device == second->device && first->inode == second->inode;
#endif
}

static int hwa_measure_file_identity_from_stream(
    FILE *stream,
    HWAMeasureFileIdentity *identity)
{
    if (stream == NULL || identity == NULL) return -1;
#if defined(_WIN32)
    {
        int descriptor = _fileno(stream);
        intptr_t raw_handle;
        HANDLE handle;
        BY_HANDLE_FILE_INFORMATION information;
        ULARGE_INTEGER size;
        if (descriptor < 0) return -1;
        raw_handle = _get_osfhandle(descriptor);
        if (raw_handle == (intptr_t)-1) return -1;
        handle = (HANDLE)raw_handle;
        if (GetFileType(handle) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(handle, &information) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
            return -1;
        }
        size.HighPart = information.nFileSizeHigh;
        size.LowPart = information.nFileSizeLow;
        identity->size = size.QuadPart;
        identity->volume_serial_number = information.dwVolumeSerialNumber;
        identity->file_index_high = information.nFileIndexHigh;
        identity->file_index_low = information.nFileIndexLow;
    }
#else
    {
        struct stat status;
        int descriptor = fileno(stream);
        if (descriptor < 0 || fstat(descriptor, &status) != 0 ||
            !S_ISREG(status.st_mode) || status.st_size < 0) return -1;
        identity->size = (uint64_t)status.st_size;
        identity->device = status.st_dev;
        identity->inode = status.st_ino;
    }
#endif
    return 0;
}

static int hwa_measure_file_identity_from_path(
    const char *path,
    uint64_t max_bytes,
    HWAMeasureFileIdentity *identity,
    char *error,
    size_t error_size)
{
    FILE *stream;
    int result = -1;
    if (path == NULL || identity == NULL) {
        hwa_set_error(error, error_size,
                      "invalid measurement profile identity arguments");
        return -1;
    }
    stream = fopen(path, "rb");
    if (stream == NULL) {
        hwa_set_error(error, error_size,
                      "cannot open measurement profile '%s': %s",
                      path, strerror(errno));
        return -1;
    }
    if (hwa_measure_file_identity_from_stream(stream, identity) != 0) {
        hwa_set_error(error, error_size,
                      "cannot inspect measurement profile '%s'", path);
    } else if (identity->size > max_bytes ||
               identity->size > UINT64_MAX / UINT64_C(8)) {
        hwa_set_error(error, error_size,
                      "measurement profile exceeds the current byte limit");
    } else {
        result = 0;
    }
    if (fclose(stream) != 0 && result == 0) {
        hwa_set_error(error, error_size,
                      "cannot close measurement profile '%s'", path);
        result = -1;
    }
    return result;
}

static int hwa_measure_sha256_for_identity(
    const char *path,
    uint64_t max_bytes,
    const HWAMeasureFileIdentity *expected_identity,
    char hex[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    unsigned char buffer[65536];
    unsigned char digest[32];
    HWASha256 context;
    HWAMeasureFileIdentity opened_identity;
    HWAMeasureFileIdentity final_identity;
    uint64_t total = 0U;
    FILE *stream;
    int result = -1;
    if (path == NULL || expected_identity == NULL || hex == NULL ||
        max_bytes == 0U || expected_identity->size > max_bytes ||
        expected_identity->size > UINT64_MAX / UINT64_C(8)) {
        hwa_set_error(error, error_size,
                      "invalid measurement profile hash arguments");
        return -1;
    }
    stream = fopen(path, "rb");
    if (stream == NULL) {
        hwa_set_error(error, error_size,
                      "cannot open measurement profile '%s': %s",
                      path, strerror(errno));
        return -1;
    }
    if (hwa_measure_file_identity_from_stream(stream, &opened_identity) != 0 ||
        !hwa_measure_file_identity_equal(&opened_identity,
                                         expected_identity)) {
        hwa_set_error(error, error_size,
                      "measurement profile changed before it was hashed");
        goto cleanup;
    }
    hwa_sha256_init(&context);
    for (;;) {
        size_t count = fread(buffer, 1U, sizeof(buffer), stream);
        if (count != 0U) {
            if ((uint64_t)count > max_bytes - total) {
                hwa_set_error(error, error_size,
                              "measurement profile exceeds the byte limit");
                goto cleanup;
            }
            total += (uint64_t)count;
            hwa_sha256_update(&context, buffer, count);
        }
        if (count < sizeof(buffer)) {
            if (ferror(stream)) {
                hwa_set_error(error, error_size,
                              "cannot read measurement profile '%s'", path);
                goto cleanup;
            }
            break;
        }
    }
    if (total != expected_identity->size ||
        hwa_measure_file_identity_from_stream(stream, &final_identity) != 0 ||
        !hwa_measure_file_identity_equal(&opened_identity, &final_identity)) {
        hwa_set_error(error, error_size,
                      "measurement profile changed while it was hashed");
        goto cleanup;
    }
    hwa_sha256_final(&context, digest);
    hwa_sha256_hex(digest, hex);
    result = 0;

cleanup:
    if (fclose(stream) != 0 && result == 0) {
        hwa_set_error(error, error_size,
                      "cannot close measurement profile '%s'", path);
        result = -1;
    }
    if (result != 0) memset(hex, 0, HWA_SHA256_HEX_SIZE);
    return result;
}

static int hwa_measure_csv_rows(const unsigned char *data,
                                size_t size,
                                HWAMeasureRowFunction function,
                                void *user,
                                char *error,
                                size_t error_size)
{
    char **fields = NULL;
    char *storage = NULL;
    size_t position = 0U;
    size_t physical_row = 1U;
    int saw_row = 0;
    fields = (char **)malloc(HWA_MEASURE_FILE_MAX_FIELDS * sizeof(*fields));
    storage = (char *)malloc(size + 1U);
    if (fields == NULL || storage == NULL) {
        free(storage);
        free(fields);
        hwa_set_error(error, error_size,
                      "out of memory while parsing measurement profile");
        return -1;
    }
    while (position < size) {
        size_t field_count = 0U;
        size_t output = 0U;
        size_t logical_row = physical_row;
        int row_done = 0;
        while (!row_done) {
            int quoted = 0;
            int closed_quote = 0;
            if (field_count == HWA_MEASURE_FILE_MAX_FIELDS) {
                hwa_set_error(error, error_size,
                              "measurement row %zu has too many fields",
                              logical_row);
                free(storage);
                free(fields);
                return -1;
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
                                  "measurement row %zu contains a NUL byte",
                                  logical_row);
                    free(storage);
                    free(fields);
                    return -1;
                }
                if (quoted) {
                    if (byte == (unsigned char)'"') {
                        if (position + 1U < size &&
                            data[position + 1U] == (unsigned char)'"') {
                            storage[output++] = '"';
                            position += 2U;
                            continue;
                        }
                        closed_quote = 1;
                        position++;
                        break;
                    }
                    if (byte == (unsigned char)'\r') {
                        if (position + 1U >= size ||
                            data[position + 1U] != (unsigned char)'\n') {
                            hwa_set_error(error, error_size,
                                          "measurement row %zu has a bare carriage return",
                                          logical_row);
                            free(storage);
                            free(fields);
                            return -1;
                        }
                        storage[output++] = '\r';
                        storage[output++] = '\n';
                        position += 2U;
                        physical_row++;
                        continue;
                    }
                    storage[output++] = (char)byte;
                    if (byte == (unsigned char)'\n') physical_row++;
                    position++;
                    continue;
                }
                if (byte == (unsigned char)'"') {
                    hwa_set_error(error, error_size,
                                  "measurement row %zu has a quote in an unquoted field",
                                  logical_row);
                    free(storage);
                    free(fields);
                    return -1;
                }
                if (byte == (unsigned char)',' ||
                    byte == (unsigned char)'\r' ||
                    byte == (unsigned char)'\n') {
                    break;
                }
                storage[output++] = (char)byte;
                position++;
            }
            if (quoted && !closed_quote) {
                hwa_set_error(error, error_size,
                              "measurement row %zu has an unterminated quote",
                              logical_row);
                free(storage);
                free(fields);
                return -1;
            }
            storage[output++] = '\0';
            if (quoted && position < size &&
                data[position] != (unsigned char)',' &&
                data[position] != (unsigned char)'\r' &&
                data[position] != (unsigned char)'\n') {
                hwa_set_error(error, error_size,
                              "measurement row %zu has bytes after a quote",
                              logical_row);
                free(storage);
                free(fields);
                return -1;
            }
            if (position >= size) {
                row_done = 1;
            } else if (data[position] == (unsigned char)',') {
                position++;
            } else if (data[position] == (unsigned char)'\r') {
                if (position + 1U >= size ||
                    data[position + 1U] != (unsigned char)'\n') {
                    hwa_set_error(error, error_size,
                                  "measurement row %zu has a bare carriage return",
                                  logical_row);
                    free(storage);
                    free(fields);
                    return -1;
                }
                position += 2U;
                physical_row++;
                row_done = 1;
            } else {
                position++;
                physical_row++;
                row_done = 1;
            }
        }
        saw_row = 1;
        if (function(fields, field_count, logical_row, user,
                     error, error_size) != 0) {
            free(storage);
            free(fields);
            return -1;
        }
    }
    free(storage);
    free(fields);
    if (!saw_row) {
        hwa_set_error(error, error_size, "measurement profile is empty");
        return -1;
    }
    return 0;
}

static int hwa_measure_record_capacity(size_t field_count,
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

static int hwa_measure_csv_stream(const char *path,
                                  const HWAMeasureFileIdentity *expected_identity,
                                  uint64_t max_bytes,
                                  uint64_t max_work_bytes,
                                  uint64_t *work_live,
                                  uint64_t *parser_bytes,
                                  char parsed_sha256[HWA_SHA256_HEX_SIZE],
                                  void *user,
                                  char *error,
                                  size_t error_size)
{
    unsigned char digest[32];
    HWASha256 hash;
    FILE *stream = NULL;
    unsigned char *record = NULL;
    size_t record_size = 0U;
    size_t maximum_record;
    uint64_t source_bytes = 0U;
    int in_quotes = 0;
    int field_start = 1;
    int result = -1;
    HWAMeasureFileIdentity opened_identity;
    HWAMeasureFileIdentity final_identity;
    if (parsed_sha256 != NULL) parsed_sha256[0] = '\0';
    if (hwa_measure_record_capacity(HWA_MEASURE_FILE_MAX_FIELDS,
                                    HWA_MEASURE_FILE_MAX_FIELD_BYTES,
                                    &maximum_record) != 0) {
        hwa_set_error(error, error_size,
                      "measurement record limit overflows");
        return -1;
    }
    if (*work_live > max_work_bytes ||
        (uint64_t)maximum_record > max_work_bytes - *work_live) {
        hwa_set_error(error, error_size,
                      "measurement row buffer exceeds the work-byte limit");
        return -1;
    }
    *work_live += (uint64_t)maximum_record;
    *parser_bytes = (uint64_t)maximum_record;
    record = (unsigned char *)malloc(maximum_record);
    if (record == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for measurement row buffer");
        *work_live -= (uint64_t)maximum_record;
        *parser_bytes = 0U;
        return -1;
    }
    if (expected_identity == NULL || expected_identity->size > max_bytes) {
        hwa_set_error(error, error_size,
                      "measurement profile exceeds the current byte limit");
        goto cleanup;
    }
    stream = fopen(path, "rb");
    if (stream == NULL) {
        hwa_set_error(error, error_size,
                      "cannot open measurement profile '%s': %s",
                      path, strerror(errno));
        goto cleanup;
    }
    if (hwa_measure_file_identity_from_stream(stream, &opened_identity) != 0 ||
        !hwa_measure_file_identity_equal(&opened_identity,
                                         expected_identity)) {
        hwa_set_error(error, error_size,
                      "measurement profile changed before it was opened");
        goto cleanup;
    }
    hwa_sha256_init(&hash);
    for (;;) {
        int byte = fgetc(stream);
        int row_done = 0;
        if (byte == EOF) {
            if (ferror(stream)) {
                hwa_set_error(error, error_size,
                              "cannot read measurement profile '%s'", path);
                goto cleanup;
            }
            if (in_quotes) {
                hwa_set_error(error, error_size,
                              "measurement profile has an unterminated quote");
                goto cleanup;
            }
            row_done = record_size != 0U;
        } else {
            if (source_bytes == max_bytes) {
                hwa_set_error(error, error_size,
                              "measurement profile exceeds the byte limit");
                goto cleanup;
            }
            source_bytes++;
            if (record_size == maximum_record) {
                hwa_set_error(error, error_size,
                              "measurement record exceeds the field limits");
                goto cleanup;
            }
            record[record_size++] = (unsigned char)byte;
            if (in_quotes && byte == '"') {
                int next = fgetc(stream);
                if (next == '"') {
                    if (source_bytes == max_bytes ||
                        record_size == maximum_record) {
                        hwa_set_error(error, error_size,
                                      "measurement record exceeds limits");
                        goto cleanup;
                    }
                    source_bytes++;
                    record[record_size++] = (unsigned char)next;
                } else {
                    in_quotes = 0;
                    if (next != EOF && ungetc(next, stream) == EOF) {
                        hwa_set_error(error, error_size,
                                      "cannot parse measurement profile");
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
                if (next != '\n' || source_bytes == max_bytes ||
                    record_size == maximum_record) {
                    hwa_set_error(error, error_size,
                                  "measurement profile has a bare carriage return");
                    goto cleanup;
                }
                source_bytes++;
                record[record_size++] = (unsigned char)next;
                row_done = 1;
                field_start = 1;
            } else if (!in_quotes) {
                field_start = 0;
            }
        }
        if (row_done) {
            uint64_t row_scratch = (uint64_t)record_size + 1U +
                (uint64_t)HWA_MEASURE_FILE_MAX_FIELDS * sizeof(char *);
            if (*work_live > max_work_bytes ||
                row_scratch > max_work_bytes - *work_live) {
                hwa_set_error(error, error_size,
                              "measurement row parse exceeds the work-byte limit");
                goto cleanup;
            }
            *work_live += row_scratch;
            hwa_sha256_update(&hash, record, record_size);
            if (hwa_measure_csv_rows(record, record_size,
                                     hwa_measure_read_row, user,
                                     error, error_size) != 0) {
                *work_live -= row_scratch;
                goto cleanup;
            }
            *work_live -= row_scratch;
            record_size = 0U;
        }
        if (byte == EOF) break;
    }
    {
        int identity_changed =
            hwa_measure_file_identity_from_stream(stream, &final_identity) != 0 ||
            !hwa_measure_file_identity_equal(&opened_identity,
                                             &final_identity);
        int size_changed = source_bytes != expected_identity->size;
        int close_failed = fclose(stream) != 0;
        stream = NULL;
        if (identity_changed || size_changed || close_failed) {
            hwa_set_error(error, error_size,
                          "measurement profile changed while reading");
            goto cleanup;
        }
    }
    if (parsed_sha256 == NULL) {
        hwa_set_error(error, error_size,
                      "invalid measurement profile hash output");
        goto cleanup;
    }
    hwa_sha256_final(&hash, digest);
    hwa_sha256_hex(digest, parsed_sha256);
    result = 0;

cleanup:
    if (stream != NULL) (void)fclose(stream);
    free(record);
    if (*parser_bytes <= *work_live) {
        *work_live -= *parser_bytes;
    }
    *parser_bytes = 0U;
    if (result != 0 && parsed_sha256 != NULL) {
        memset(parsed_sha256, 0, HWA_SHA256_HEX_SIZE);
    }
    return result;
}

typedef enum HWAMeasureFileSection {
    HWA_MEASURE_SECTION_META = 1,
    HWA_MEASURE_SECTION_INPUT = 2,
    HWA_MEASURE_SECTION_SOURCE = 3,
    HWA_MEASURE_SECTION_CONTEXT = 4,
    HWA_MEASURE_SECTION_MEASURE = 5,
    HWA_MEASURE_SECTION_GROUP = 6,
    HWA_MEASURE_SECTION_GROUP_MEMBER = 7,
    HWA_MEASURE_SECTION_STAT = 8,
    HWA_MEASURE_SECTION_WARNING = 9
} HWAMeasureFileSection;

typedef struct HWAMeasureMetaKey {
    const char *key;
    const char *unit;
} HWAMeasureMetaKey;

static const HWAMeasureMetaKey hwa_measure_meta_keys[] = {
    {"tool_version", ""},
    {"analysis_method_version", ""},
    {"alignment_method_version", ""},
    {"segmentation_method_version", ""},
    {"measurement_method_version", ""},
    {"build_compiler_family", ""},
    {"build_compiler_version", ""},
    {"build_c_standard", ""},
    {"build_target_os", ""},
    {"build_pointer_bits", "bits"},
    {"build_endianness", ""},
    {"build_mode", ""},
    {"sample_rate_hz", "Hz"},
    {"audio_frames", "frames"},
    {"audio_duration_seconds", "seconds"},
    {"audio_channels", "channels"},
    {"audio_container", ""},
    {"audio_encoding", ""},
    {"bits_per_sample", "bits"},
    {"valid_bits_per_sample", "bits"},
    {"block_align", "bytes"},
    {"channel_mask", "bitset"},
    {"data_bytes", "bytes"},
    {"level_reference_dbfs", "dBFS"},
    {"level_reference_item_count", "items"},
    {"capability_flags", "bitset"},
    {"production_corrected_available", "boolean"},
    {"item_frame_evaluations", "evaluations"},
    {"retained_work_bytes", "bytes"},
    {"transform_count", "transforms"},
    {"decode_block_frames", "frames"},
    {"fft_size", "samples"},
    {"hop_size", "samples"},
    {"pitch_confidence_floor", "ratio"},
    {"spectral_floor_dbfs", "dBFS"},
    {"max_partials", "partials"},
    {"max_input_bytes", "bytes"},
    {"max_input_frames", "frames"},
    {"max_work_bytes", "bytes"},
    {"max_transforms", "transforms"},
    {"max_series_points", "points"},
    {"max_item_frame_evaluations", "evaluations"},
    {"max_events", "events"},
    {"max_items", "items"},
    {"max_item_members", "members"},
    {"max_measurements", "measurements"},
    {"max_groups", "groups"},
    {"max_group_members", "members"},
    {"max_statistics", "statistics"},
    {"max_warnings", "warnings"},
    {"context_count", "contexts"},
    {"measurement_count", "measurements"},
    {"group_count", "groups"},
    {"group_member_count", "members"},
    {"statistic_count", "statistics"},
    {"warning_count", "warnings"}
};

struct HWAMeasureReadState {
    HWAProfileComparisonOptions limits;
    HWAMeasurementSet result;
    HWAMeasureFileSection section;
    size_t row_count;
    size_t meta_index;
    size_t input_count;
    size_t source_count;
    size_t expected_contexts;
    size_t expected_measurements;
    size_t expected_groups;
    size_t expected_group_members;
    size_t expected_statistics;
    size_t expected_warnings;
    uint64_t work_live;
    uint64_t parser_bytes;
    uint64_t saved_profile_bytes;
    uint64_t saved_retained_meta;
    int arrays_allocated;
};

static int hwa_measure_parse_u64(const char *text, uint64_t *value)
{
    uint64_t parsed = 0U;
    const unsigned char *cursor = (const unsigned char *)text;
    if (text == NULL || text[0] == '\0') return -1;
    while (*cursor != 0U) {
        unsigned digit;
        if (*cursor < (unsigned char)'0' || *cursor > (unsigned char)'9') {
            return -1;
        }
        digit = (unsigned)(*cursor - (unsigned char)'0');
        if (parsed > (UINT64_MAX - (uint64_t)digit) / 10U) return -1;
        parsed = parsed * 10U + (uint64_t)digit;
        cursor++;
    }
    *value = parsed;
    return 0;
}

static int hwa_measure_parse_size(const char *text, size_t *value)
{
    uint64_t parsed;
    if (hwa_measure_parse_u64(text, &parsed) != 0 ||
        parsed > (uint64_t)SIZE_MAX) return -1;
    *value = (size_t)parsed;
    return 0;
}

static int hwa_measure_parse_u32(const char *text, uint32_t *value)
{
    uint64_t parsed;
    if (hwa_measure_parse_u64(text, &parsed) != 0 || parsed > UINT32_MAX) {
        return -1;
    }
    *value = (uint32_t)parsed;
    return 0;
}

static int hwa_measure_parse_double(const char *text, double *value)
{
    char *end = NULL;
    double parsed;
    if (text == NULL || text[0] == '\0') return -1;
    errno = 0;
    parsed = strtod(text, &end);
    if (end == text || *end != '\0' || !isfinite(parsed) ||
        (errno == ERANGE && fpclassify(parsed) != FP_SUBNORMAL)) {
        return -1;
    }
    *value = parsed == 0.0 ? 0.0 : parsed;
    return 0;
}

static int hwa_measure_read_charge(HWAMeasureReadState *state, uint64_t bytes)
{
    if (state->work_live > state->limits.max_work_bytes ||
        bytes > state->limits.max_work_bytes - state->work_live) return -1;
    state->work_live += bytes;
    return 0;
}

static char *hwa_measure_read_copy(HWAMeasureReadState *state,
                                   const char *text)
{
    size_t size = strlen(text) + 1U;
    char *copy;
    if (hwa_measure_read_charge(state, (uint64_t)size) != 0) return NULL;
    copy = (char *)malloc(size);
    if (copy == NULL) {
        state->work_live -= (uint64_t)size;
        return NULL;
    }
    memcpy(copy, text, size);
    return copy;
}

static int hwa_measure_hex_value(unsigned char byte)
{
    if (byte >= (unsigned char)'0' && byte <= (unsigned char)'9') {
        return (int)(byte - (unsigned char)'0');
    }
    if (byte >= (unsigned char)'a' && byte <= (unsigned char)'f') {
        return 10 + (int)(byte - (unsigned char)'a');
    }
    return -1;
}

static char *hwa_measure_read_path(HWAMeasureReadState *state,
                                   const char *hex)
{
    size_t hex_size = strlen(hex);
    size_t output_size;
    char *path;
    size_t index;
    if (hex_size == 0U || (hex_size & 1U) != 0U) return NULL;
    output_size = hex_size / 2U;
    if (output_size == SIZE_MAX ||
        hwa_measure_read_charge(state, (uint64_t)output_size + 1U) != 0) {
        return NULL;
    }
    path = (char *)malloc(output_size + 1U);
    if (path == NULL) {
        state->work_live -= (uint64_t)output_size + 1U;
        return NULL;
    }
    for (index = 0U; index < output_size; ++index) {
        int high = hwa_measure_hex_value((unsigned char)hex[index * 2U]);
        int low = hwa_measure_hex_value((unsigned char)hex[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            free(path);
            state->work_live -= (uint64_t)output_size + 1U;
            return NULL;
        }
        path[index] = (char)((unsigned)high * 16U + (unsigned)low);
        if (path[index] == '\0') {
            free(path);
            state->work_live -= (uint64_t)output_size + 1U;
            return NULL;
        }
    }
    path[output_size] = '\0';
    return path;
}

static int hwa_measure_read_allocate_arrays(HWAMeasureReadState *state,
                                            char *error,
                                            size_t error_size)
{
    uint64_t bytes = 0U;
#define HWA_ADD_ARRAY(count_, type_)                                         \
    do {                                                                     \
        uint64_t add_;                                                       \
        if ((uint64_t)(count_) > UINT64_MAX / (uint64_t)sizeof(type_)) {    \
            hwa_set_error(error, error_size,                                 \
                          "measurement row storage overflows");             \
            return -1;                                                       \
        }                                                                    \
        add_ = (uint64_t)(count_) * (uint64_t)sizeof(type_);                 \
        if (add_ > UINT64_MAX - bytes) {                                     \
            hwa_set_error(error, error_size,                                 \
                          "measurement row total overflows");               \
            return -1;                                                       \
        }                                                                    \
        bytes += add_;                                                       \
    } while (0)

    if (state->arrays_allocated) return 0;
    if (state->expected_contexts > state->limits.max_contexts ||
        state->expected_measurements > state->limits.max_measurements ||
        state->expected_groups > state->limits.max_groups ||
        state->expected_group_members > state->limits.max_group_members ||
        state->expected_statistics > state->limits.max_statistics ||
        state->expected_warnings > state->limits.max_warnings) {
        hwa_set_error(error, error_size,
                      "measurement rows exceed current caller limits");
        return -1;
    }
    HWA_ADD_ARRAY(state->expected_contexts, HWAMeasureItemContext);
    HWA_ADD_ARRAY(state->expected_measurements, HWAMeasureObservation);
    HWA_ADD_ARRAY(state->expected_groups, HWAMeasureGroup);
    HWA_ADD_ARRAY(state->expected_group_members, HWAMeasureGroupMember);
    HWA_ADD_ARRAY(state->expected_statistics, HWAMeasureStatistic);
    HWA_ADD_ARRAY(state->expected_warnings, HWAMeasureWarning);
#undef HWA_ADD_ARRAY
    if (hwa_measure_read_charge(state, bytes) != 0) {
        hwa_set_error(error, error_size,
                      "measurement rows exceed the current work-byte limit");
        return -1;
    }
    state->result.contexts = state->expected_contexts == 0U ? NULL :
        (HWAMeasureItemContext *)calloc(state->expected_contexts,
                                        sizeof(*state->result.contexts));
    state->result.measurements = state->expected_measurements == 0U ? NULL :
        (HWAMeasureObservation *)calloc(
            state->expected_measurements,
            sizeof(*state->result.measurements));
    state->result.groups = state->expected_groups == 0U ? NULL :
        (HWAMeasureGroup *)calloc(state->expected_groups,
                                  sizeof(*state->result.groups));
    state->result.group_members = state->expected_group_members == 0U ? NULL :
        (HWAMeasureGroupMember *)calloc(
            state->expected_group_members,
            sizeof(*state->result.group_members));
    state->result.statistics = state->expected_statistics == 0U ? NULL :
        (HWAMeasureStatistic *)calloc(
            state->expected_statistics,
            sizeof(*state->result.statistics));
    state->result.warnings = state->expected_warnings == 0U ? NULL :
        (HWAMeasureWarning *)calloc(state->expected_warnings,
                                    sizeof(*state->result.warnings));
    if ((state->expected_contexts != 0U && state->result.contexts == NULL) ||
        (state->expected_measurements != 0U &&
         state->result.measurements == NULL) ||
        (state->expected_groups != 0U && state->result.groups == NULL) ||
        (state->expected_group_members != 0U &&
         state->result.group_members == NULL) ||
        (state->expected_statistics != 0U &&
         state->result.statistics == NULL) ||
        (state->expected_warnings != 0U && state->result.warnings == NULL)) {
        hwa_set_error(error, error_size,
                      "out of memory for measurement rows");
        return -1;
    }
    state->saved_profile_bytes =
        (uint64_t)state->expected_groups * sizeof(*state->result.groups) +
        (uint64_t)state->expected_group_members *
            sizeof(*state->result.group_members) +
        (uint64_t)state->expected_statistics *
            sizeof(*state->result.statistics);
    state->arrays_allocated = 1;
    return 0;
}

static int hwa_measure_read_meta(char **fields,
                                 size_t field_count,
                                 size_t row,
                                 HWAMeasureReadState *state,
                                 char *error,
                                 size_t error_size)
{
    size_t index = state->meta_index;
    uint64_t u64;
    size_t size_value;
    double number;
    if (field_count != 4U || index >= sizeof(hwa_measure_meta_keys) /
                                             sizeof(hwa_measure_meta_keys[0]) ||
        strcmp(fields[1], hwa_measure_meta_keys[index].key) != 0 ||
        strcmp(fields[3], hwa_measure_meta_keys[index].unit) != 0) {
        hwa_set_error(error, error_size,
                      "measurement row %zu has invalid META order", row);
        return -1;
    }
    switch (index) {
    case 0U:
        if (fields[2][0] == '\0') goto invalid;
        break;
    case 1U:
    case 2U:
    case 3U:
        if (fields[2][0] == '\0') goto invalid;
        break;
    case 4U:
        if (strcmp(fields[2], HWA_MEASUREMENT_METHOD_VERSION) != 0) goto invalid;
        break;
    case 5U: case 6U: case 7U: case 8U: case 10U: case 11U:
        if (fields[2][0] == '\0') goto invalid;
        break;
    case 9U:
        if (hwa_measure_parse_u64(fields[2], &u64) != 0 || u64 == 0U) {
            goto invalid;
        }
        break;
    case 12U:
        if (hwa_measure_parse_u64(fields[2], &u64) != 0 ||
            u64 == 0U || u64 > UINT32_MAX) goto invalid;
        state->result.audio_format.sample_rate_hz = (uint32_t)u64;
        break;
    case 13U:
        if (hwa_measure_parse_u64(fields[2],
                                  &state->result.audio_format.frames) != 0) {
            goto invalid;
        }
        break;
    case 14U:
        if (hwa_measure_parse_double(fields[2], &number) != 0 || number < 0.0) {
            goto invalid;
        }
        state->result.audio_format.duration_seconds = number;
        break;
    case 15U:
        if (hwa_measure_parse_u64(fields[2], &u64) != 0 ||
            u64 == 0U || u64 > UINT16_MAX) goto invalid;
        state->result.audio_format.channels = (uint16_t)u64;
        break;
    case 16U:
        if (strcmp(fields[2], "RIFF") == 0) {
            state->result.audio_format.container = HWA_CONTAINER_RIFF;
        } else if (strcmp(fields[2], "RF64") == 0) {
            state->result.audio_format.container = HWA_CONTAINER_RF64;
        } else goto invalid;
        break;
    case 17U:
        if (strcmp(fields[2], "PCM") == 0) {
            state->result.audio_format.encoding = HWA_ENCODING_PCM;
        } else if (strcmp(fields[2], "IEEE float") == 0) {
            state->result.audio_format.encoding = HWA_ENCODING_IEEE_FLOAT;
        } else goto invalid;
        break;
    case 18U:
        if (hwa_measure_parse_u64(fields[2], &u64) != 0 ||
            u64 == 0U || u64 > UINT16_MAX) goto invalid;
        state->result.audio_format.bits_per_sample = (uint16_t)u64;
        break;
    case 19U:
        if (hwa_measure_parse_u64(fields[2], &u64) != 0 ||
            u64 == 0U || u64 > UINT16_MAX) goto invalid;
        state->result.audio_format.valid_bits_per_sample = (uint16_t)u64;
        break;
    case 20U:
        if (hwa_measure_parse_u64(fields[2], &u64) != 0 ||
            u64 == 0U || u64 > UINT16_MAX) goto invalid;
        state->result.audio_format.block_align = (uint16_t)u64;
        break;
    case 21U:
        if (hwa_measure_parse_u64(fields[2], &u64) != 0 || u64 > UINT32_MAX) {
            goto invalid;
        }
        state->result.audio_format.channel_mask = (uint32_t)u64;
        break;
    case 22U:
        if (hwa_measure_parse_u64(fields[2],
                                  &state->result.audio_format.data_bytes) != 0) {
            goto invalid;
        }
        break;
    case 23U:
        if (fields[2][0] == '\0') {
            state->result.level_reference_valid = 0;
        } else {
            if (hwa_measure_parse_double(fields[2], &number) != 0) goto invalid;
            state->result.level_reference_dbfs = number;
            state->result.level_reference_valid = 1;
        }
        break;
    case 24U:
        if (hwa_measure_parse_size(fields[2],
                                   &state->result.level_reference_item_count) != 0) {
            goto invalid;
        }
        break;
    case 25U:
        if (hwa_measure_parse_u64(fields[2], &u64) != 0 ||
            u64 != 0U) {
            goto invalid;
        }
        state->result.capability_flags = (uint32_t)u64;
        break;
    case 26U:
        if (strcmp(fields[2], "0") != 0) goto invalid;
        break;
    case 27U:
        if (hwa_measure_parse_u64(fields[2],
                                  &state->result.item_frame_evaluations) != 0) {
            goto invalid;
        }
        break;
    case 28U:
        if (hwa_measure_parse_u64(fields[2], &state->saved_retained_meta) != 0) {
            goto invalid;
        }
        break;
    case 29U:
        if (hwa_measure_parse_size(fields[2], &state->result.transform_count) != 0) {
            goto invalid;
        }
        break;
    case 30U:
        if (hwa_measure_parse_size(fields[2],
                                   &state->result.options.decode_block_frames) != 0) goto invalid;
        break;
    case 31U:
        if (hwa_measure_parse_size(fields[2], &state->result.options.fft_size) != 0) goto invalid;
        break;
    case 32U:
        if (hwa_measure_parse_size(fields[2], &state->result.options.hop_size) != 0) goto invalid;
        break;
    case 33U:
        if (hwa_measure_parse_double(fields[2], &state->result.options.pitch_confidence_floor) != 0) goto invalid;
        break;
    case 34U:
        if (hwa_measure_parse_double(fields[2], &state->result.options.spectral_floor_dbfs) != 0) goto invalid;
        break;
    case 35U:
        if (hwa_measure_parse_size(fields[2], &state->result.options.max_partials) != 0) goto invalid;
        break;
    case 36U:
        if (hwa_measure_parse_u64(fields[2], &state->result.options.max_input_bytes) != 0) goto invalid;
        break;
    case 37U:
        if (hwa_measure_parse_u64(fields[2], &state->result.options.max_input_frames) != 0) goto invalid;
        break;
    case 38U:
        if (hwa_measure_parse_u64(fields[2], &state->result.options.max_work_bytes) != 0) goto invalid;
        break;
    case 39U:
        if (hwa_measure_parse_size(fields[2], &state->result.options.max_transforms) != 0) goto invalid;
        break;
    case 40U:
        if (hwa_measure_parse_size(fields[2], &state->result.options.max_series_points) != 0) goto invalid;
        break;
    case 41U:
        if (hwa_measure_parse_u64(fields[2], &state->result.options.max_item_frame_evaluations) != 0) goto invalid;
        break;
    case 42U:
        if (hwa_measure_parse_size(fields[2], &state->result.options.max_events) != 0) goto invalid;
        break;
    case 43U:
        if (hwa_measure_parse_size(fields[2], &state->result.options.max_items) != 0) goto invalid;
        break;
    case 44U:
        if (hwa_measure_parse_size(fields[2], &state->result.options.max_item_members) != 0) goto invalid;
        break;
    case 45U:
        if (hwa_measure_parse_size(fields[2], &state->result.options.max_measurements) != 0) goto invalid;
        break;
    case 46U:
        if (hwa_measure_parse_size(fields[2], &state->result.options.max_groups) != 0) goto invalid;
        break;
    case 47U:
        if (hwa_measure_parse_size(fields[2], &state->result.options.max_group_members) != 0) goto invalid;
        break;
    case 48U:
        if (hwa_measure_parse_size(fields[2], &state->result.options.max_statistics) != 0) goto invalid;
        break;
    case 49U:
        if (hwa_measure_parse_size(fields[2], &state->result.options.max_warnings) != 0) goto invalid;
        break;
    case 50U:
        if (hwa_measure_parse_size(fields[2], &state->expected_contexts) != 0) goto invalid;
        break;
    case 51U:
        if (hwa_measure_parse_size(fields[2], &state->expected_measurements) != 0) goto invalid;
        break;
    case 52U:
        if (hwa_measure_parse_size(fields[2], &state->expected_groups) != 0) goto invalid;
        break;
    case 53U:
        if (hwa_measure_parse_size(fields[2], &state->expected_group_members) != 0) goto invalid;
        break;
    case 54U:
        if (hwa_measure_parse_size(fields[2], &state->expected_statistics) != 0) goto invalid;
        break;
    case 55U:
        if (hwa_measure_parse_size(fields[2], &state->expected_warnings) != 0) goto invalid;
        break;
    default:
        goto invalid;
    }
    (void)size_value;
    state->meta_index++;
    return 0;

invalid:
    hwa_set_error(error, error_size,
                  "measurement row %zu has invalid META value", row);
    return -1;
}

static int hwa_measure_saved_options_valid(const HWAMeasurementOptions *options)
{
    return options->decode_block_frames != 0U &&
           options->decode_block_frames <= 1048576U &&
           options->fft_size >= 256U && options->fft_size <= 16384U &&
           (options->fft_size & (options->fft_size - 1U)) == 0U &&
           options->hop_size != 0U && options->hop_size <= options->fft_size &&
           isfinite(options->pitch_confidence_floor) &&
           options->pitch_confidence_floor >= 0.0 &&
           options->pitch_confidence_floor <= 1.0 &&
           isfinite(options->spectral_floor_dbfs) &&
           options->spectral_floor_dbfs >= -300.0 &&
           options->spectral_floor_dbfs <= 0.0 &&
           options->max_partials != 0U && options->max_partials <= 32U &&
           options->max_input_bytes != 0U &&
           options->max_input_frames != 0U && options->max_work_bytes != 0U &&
           options->max_transforms != 0U && options->max_series_points != 0U &&
           options->max_item_frame_evaluations != 0U &&
           options->max_events != 0U && options->max_items != 0U &&
           options->max_item_members != 0U && options->max_measurements != 0U &&
           options->max_groups != 0U && options->max_group_members != 0U &&
           options->max_statistics != 0U && options->max_warnings != 0U;
}

static int hwa_measure_read_input(char **fields,
                                  size_t field_count,
                                  size_t row,
                                  HWAMeasureReadState *state,
                                  char *error,
                                  size_t error_size)
{
    const char *expected_role = state->input_count == 0U ? "items" : "audio";
    char **path = state->input_count == 0U ? &state->result.items_path :
                                            &state->result.audio_path;
    char *sha256 = state->input_count == 0U ? state->result.items_sha256 :
                                              state->result.audio_sha256;
    double duration;
    uint64_t sample_rate;
    uint64_t frames;
    if (field_count != 7U || state->input_count >= 2U ||
        strcmp(fields[1], expected_role) != 0 ||
        !hwa_measure_valid_sha256(fields[3]) ||
        (*path = hwa_measure_read_path(state, fields[2])) == NULL) {
        hwa_set_error(error, error_size,
                      "measurement row %zu has an invalid INPUT", row);
        return -1;
    }
    memcpy(sha256, fields[3], HWA_SHA256_HEX_SIZE);
    if (state->input_count == 0U) {
        if (fields[4][0] != '\0' || fields[5][0] != '\0' ||
            fields[6][0] != '\0') goto invalid;
    } else {
        if (hwa_measure_parse_double(fields[4], &duration) != 0 ||
            hwa_measure_parse_u64(fields[5], &sample_rate) != 0 ||
            hwa_measure_parse_u64(fields[6], &frames) != 0 ||
            duration != state->result.audio_format.duration_seconds ||
            sample_rate != state->result.audio_format.sample_rate_hz ||
            frames != state->result.audio_format.frames) goto invalid;
    }
    state->input_count++;
    return 0;

invalid:
    hwa_set_error(error, error_size,
                  "measurement row %zu has mismatched INPUT facts", row);
    return -1;
}

static int hwa_measure_read_source(char **fields,
                                   size_t field_count,
                                   size_t row,
                                   HWAMeasureReadState *state,
                                   char *error,
                                   size_t error_size)
{
    char **path = NULL;
    char *sha256 = NULL;
    if (field_count != 4U || state->result.source_score_path != NULL ||
        !hwa_measure_valid_sha256(fields[3])) goto invalid;
    if (state->source_count == 0U && strcmp(fields[1], "alignment") == 0) {
        path = &state->result.alignment_path;
        sha256 = state->result.alignment_sha256;
    } else if (strcmp(fields[1], "labels") == 0 &&
               state->result.labels_path == NULL &&
               state->result.amendment_path == NULL) {
        path = &state->result.labels_path;
        sha256 = state->result.labels_sha256;
    } else if (strcmp(fields[1], "amendment") == 0 &&
               state->result.amendment_path == NULL) {
        path = &state->result.amendment_path;
        sha256 = state->result.amendment_sha256;
    } else if (strcmp(fields[1], "score") == 0 &&
               state->result.alignment_path != NULL) {
        path = &state->result.source_score_path;
        sha256 = state->result.source_score_sha256;
    } else {
        goto invalid;
    }
    *path = hwa_measure_read_path(state, fields[2]);
    if (*path == NULL) goto invalid;
    memcpy(sha256, fields[3], HWA_SHA256_HEX_SIZE);
    state->source_count++;
    return 0;

invalid:
    hwa_set_error(error, error_size,
                  "measurement row %zu has an invalid SOURCE", row);
    return -1;
}

static char *hwa_measure_read_optional_copy(HWAMeasureReadState *state,
                                            const char *text)
{
    return text[0] == '\0' ? NULL : hwa_measure_read_copy(state, text);
}

static int hwa_measure_read_context(char **fields,
                                    size_t field_count,
                                    size_t row,
                                    HWAMeasureReadState *state,
                                    char *error,
                                    size_t error_size)
{
    HWAMeasureItemContext *context;
    char **labels[11];
    size_t index;
    uint32_t override_flags;
    uint32_t quality_flags;
    uint64_t item_id;
    int excluded;
    if (field_count != 23U ||
        state->result.context_count >= state->expected_contexts ||
        hwa_measure_parse_u64(fields[1], &item_id) != 0 ||
        item_id != (uint64_t)state->result.context_count + 1U) goto invalid;
    context = &state->result.contexts[state->result.context_count];
    state->result.context_count++;
    context->item_id = item_id;
    context->item_key = hwa_measure_read_copy(state, fields[2]);
    context->item_role = hwa_measure_read_copy(state, fields[4]);
    if (context->item_key == NULL || context->item_key[0] == '\0' ||
        context->item_role == NULL || context->item_role[0] == '\0' ||
        hwa_measure_item_kind_from_name(fields[3], &context->item_kind) != 0 ||
        hwa_measure_parse_u64(fields[5], &context->start_sample) != 0 ||
        hwa_measure_parse_u64(fields[6], &context->end_sample) != 0 ||
        context->start_sample > context->end_sample ||
        context->end_sample > state->result.audio_format.frames) goto invalid;
    labels[0] = &context->labels.pitch;
    labels[1] = &context->labels.register_name;
    labels[2] = &context->labels.dynamic;
    labels[3] = &context->labels.articulation;
    labels[4] = &context->labels.part;
    labels[5] = &context->labels.physical_element;
    labels[6] = &context->labels.controller;
    labels[7] = &context->labels.technique;
    labels[8] = &context->labels.score_section;
    labels[9] = &context->labels.transition;
    labels[10] = &context->labels.gesture;
    for (index = 0U; index < 11U; ++index) {
        *labels[index] = hwa_measure_read_optional_copy(state, fields[7U + index]);
        if (fields[7U + index][0] != '\0' && *labels[index] == NULL) goto invalid;
    }
    if (hwa_measure_parse_u32(fields[18], &override_flags) != 0 ||
        (override_flags & ~HWA_LABEL_OVERRIDE_ALL) != 0U ||
        hwa_measure_parse_size(fields[19], &context->source_event_count) != 0 ||
        hwa_measure_parse_double(fields[20], &context->item_confidence) != 0 ||
        context->item_confidence < 0.0 || context->item_confidence > 1.0 ||
        hwa_measure_parse_u32(fields[21], &quality_flags) != 0 ||
        (quality_flags & ~((UINT32_C(1) << 4) - 1U)) != 0U ||
        hwa_measure_parse_u64(fields[22], &item_id) != 0 || item_id > 1U) {
        goto invalid;
    }
    for (index = 0U; index < 11U; ++index) {
        if ((override_flags & (UINT32_C(1) << index)) != 0U &&
            *labels[index] == NULL) goto invalid;
    }
    excluded = item_id != 0U;
    context->labels.override_flags = override_flags;
    context->item_quality_flags = quality_flags;
    context->excluded = excluded;
    return 0;

invalid:
    hwa_set_error(error, error_size,
                  "measurement row %zu has an invalid CONTEXT", row);
    return -1;
}

static int hwa_measure_read_observation(char **fields,
                                        size_t field_count,
                                        size_t row,
                                        HWAMeasureReadState *state,
                                        char *error,
                                        size_t error_size)
{
    HWAMeasureObservation *observation;
    uint64_t id;
    uint64_t item_id;
    uint32_t index;
    uint32_t evidence;
    uint32_t quality;
    HWAMeasureUnit expected_unit;
    if (field_count != 12U ||
        state->result.measurement_count >= state->expected_measurements ||
        hwa_measure_parse_u64(fields[1], &id) != 0 ||
        id != (uint64_t)state->result.measurement_count + 1U ||
        hwa_measure_parse_u64(fields[2], &item_id) != 0 ||
        item_id == 0U || item_id > (uint64_t)state->result.context_count ||
        hwa_measure_parse_u32(fields[4], &index) != 0 ||
        hwa_measure_parse_u32(fields[10], &evidence) != 0 ||
        hwa_measure_parse_u32(fields[11], &quality) != 0 ||
        (evidence & ~HWA_MEASURE_EVIDENCE_ALL) != 0U ||
        (quality & ~HWA_MEASURE_QUALITY_ALL) != 0U) goto invalid;
    observation = &state->result.measurements[state->result.measurement_count];
    observation->id = id;
    observation->item_id = item_id;
    observation->index = index;
    observation->evidence_flags = evidence;
    observation->quality_flags = quality;
    if (hwa_measure_kind_from_name(fields[3], &observation->kind) != 0 ||
        hwa_measure_unit_from_name(fields[5], &observation->unit) != 0 ||
        hwa_measure_view_from_name(fields[6], &observation->view) != 0 ||
        hwa_measure_status_from_name(fields[7], &observation->status) != 0 ||
        observation->view == HWA_MEASURE_VIEW_PRODUCTION_CORRECTED ||
        !hwa_measure_kind_index_valid(observation->kind, index,
                                      state->result.options.max_partials) ||
        hwa_measure_kind_unit(observation->kind, observation->view,
                              &expected_unit) != 0 ||
        expected_unit != observation->unit ||
        hwa_measure_parse_double(fields[9], &observation->confidence) != 0 ||
        observation->confidence < 0.0 || observation->confidence > 1.0) {
        goto invalid;
    }
    if (observation->status == HWA_MEASURE_STATUS_VALID) {
        if (hwa_measure_parse_double(fields[8], &observation->value) != 0) {
            goto invalid;
        }
    } else if (fields[8][0] != '\0') goto invalid;
    state->result.measurement_count++;
    return 0;

invalid:
    hwa_set_error(error, error_size,
                  "measurement row %zu has an invalid MEASURE", row);
    return -1;
}

static int hwa_measure_read_group(char **fields,
                                  size_t field_count,
                                  size_t row,
                                  HWAMeasureReadState *state,
                                  char *error,
                                  size_t error_size)
{
    HWAMeasureGroup *group;
    uint64_t id;
    if (field_count != 8U ||
        state->result.group_count >= state->expected_groups ||
        hwa_measure_parse_u64(fields[1], &id) != 0 ||
        id != (uint64_t)state->result.group_count + 1U) goto invalid;
    group = &state->result.groups[state->result.group_count];
    state->result.group_count++;
    group->id = id;
    group->key = hwa_measure_read_copy(state, fields[2]);
    group->item_role = hwa_measure_read_copy(state, fields[4]);
    group->value = hwa_measure_read_copy(state, fields[6]);
    if (group->key == NULL || group->key[0] == '\0' ||
        group->item_role == NULL || group->item_role[0] == '\0' ||
        group->value == NULL ||
        hwa_measure_item_kind_from_name(fields[3], &group->item_kind) != 0 ||
        hwa_measure_group_selector_from_name(fields[5], &group->selector) != 0 ||
        (group->selector == HWA_MEASURE_GROUP_ALL && group->value[0] != '\0') ||
        (group->selector != HWA_MEASURE_GROUP_ALL && group->value[0] == '\0') ||
        hwa_measure_parse_size(fields[7], &group->member_count) != 0) {
        goto invalid;
    }
    state->saved_profile_bytes += (uint64_t)strlen(group->key) + 1U +
                                  (uint64_t)strlen(group->item_role) + 1U +
                                  (uint64_t)strlen(group->value) + 1U;
    return 0;

invalid:
    hwa_set_error(error, error_size,
                  "measurement row %zu has an invalid GROUP", row);
    return -1;
}

static int hwa_measure_read_group_member(char **fields,
                                         size_t field_count,
                                         size_t row,
                                         HWAMeasureReadState *state,
                                         char *error,
                                         size_t error_size)
{
    HWAMeasureGroupMember *member;
    if (field_count != 3U ||
        state->result.group_member_count >= state->expected_group_members) {
        goto invalid;
    }
    member = &state->result.group_members[state->result.group_member_count];
    if (hwa_measure_parse_u64(fields[1], &member->group_id) != 0 ||
        hwa_measure_parse_u64(fields[2], &member->item_id) != 0 ||
        member->group_id == 0U ||
        member->group_id > (uint64_t)state->result.group_count ||
        member->item_id == 0U ||
        member->item_id > (uint64_t)state->result.context_count) goto invalid;
    state->result.group_member_count++;
    return 0;

invalid:
    hwa_set_error(error, error_size,
                  "measurement row %zu has an invalid GROUP_MEMBER", row);
    return -1;
}

static int hwa_measure_read_statistics_fields(char **fields,
                                              size_t start,
                                              HWAMeasureStatistics *statistics)
{
    int valid;
    uint64_t bit;
    memset(statistics, 0, sizeof(*statistics));
    if (hwa_measure_parse_size(fields[start], &statistics->total_count) != 0 ||
        hwa_measure_parse_size(fields[start + 1U], &statistics->valid_count) != 0 ||
        hwa_measure_parse_size(fields[start + 2U], &statistics->missing_count) != 0 ||
        statistics->valid_count > statistics->total_count ||
        statistics->missing_count !=
            statistics->total_count - statistics->valid_count ||
        hwa_measure_parse_u64(fields[start + 13U], &bit) != 0 || bit > 1U) {
        return -1;
    }
    valid = bit != 0U;
    if (valid != (statistics->valid_count != 0U)) return -1;
    statistics->valid = valid;
    if (!valid) {
        size_t index;
        for (index = 3U; index <= 12U; ++index) {
            if (fields[start + index][0] != '\0') return -1;
        }
        return 0;
    }
    return hwa_measure_parse_double(fields[start + 3U], &statistics->minimum) ||
           hwa_measure_parse_double(fields[start + 4U], &statistics->q05) ||
           hwa_measure_parse_double(fields[start + 5U], &statistics->q25) ||
           hwa_measure_parse_double(fields[start + 6U], &statistics->q50) ||
           hwa_measure_parse_double(fields[start + 7U], &statistics->q75) ||
           hwa_measure_parse_double(fields[start + 8U], &statistics->q95) ||
           hwa_measure_parse_double(fields[start + 9U], &statistics->maximum) ||
           hwa_measure_parse_double(fields[start + 10U], &statistics->mean) ||
           hwa_measure_parse_double(fields[start + 11U],
                                    &statistics->population_sd) ||
           hwa_measure_parse_double(fields[start + 12U],
                                    &statistics->confidence) ||
           statistics->minimum > statistics->q05 ||
           statistics->q05 > statistics->q25 ||
           statistics->q25 > statistics->q50 ||
           statistics->q50 > statistics->q75 ||
           statistics->q75 > statistics->q95 ||
           statistics->q95 > statistics->maximum ||
           statistics->population_sd < 0.0 ||
           statistics->confidence < 0.0 || statistics->confidence > 1.0
               ? -1 : 0;
}

static int hwa_measure_read_statistic(char **fields,
                                      size_t field_count,
                                      size_t row,
                                      HWAMeasureReadState *state,
                                      char *error,
                                      size_t error_size)
{
    HWAMeasureStatistic *statistic;
    uint64_t id;
    uint32_t quality;
    HWAMeasureUnit expected_unit;
    if (field_count != 22U ||
        state->result.statistic_count >= state->expected_statistics ||
        hwa_measure_parse_u64(fields[1], &id) != 0 ||
        id != (uint64_t)state->result.statistic_count + 1U) goto invalid;
    statistic = &state->result.statistics[state->result.statistic_count];
    statistic->id = id;
    if (hwa_measure_parse_u64(fields[2], &statistic->group_id) != 0 ||
        statistic->group_id == 0U ||
        statistic->group_id > (uint64_t)state->result.group_count ||
        hwa_measure_kind_from_name(fields[3], &statistic->kind) != 0 ||
        hwa_measure_parse_u32(fields[4], &statistic->index) != 0 ||
        hwa_measure_unit_from_name(fields[5], &statistic->unit) != 0 ||
        hwa_measure_view_from_name(fields[6], &statistic->view) != 0 ||
        statistic->view == HWA_MEASURE_VIEW_PRODUCTION_CORRECTED ||
        !hwa_measure_kind_index_valid(statistic->kind, statistic->index,
                                      state->result.options.max_partials) ||
        hwa_measure_kind_unit(statistic->kind, statistic->view,
                              &expected_unit) != 0 ||
        expected_unit != statistic->unit ||
        hwa_measure_read_statistics_fields(fields, 7U,
                                           &statistic->statistics) != 0 ||
        hwa_measure_parse_u32(fields[21], &quality) != 0 ||
        (quality & ~HWA_MEASURE_QUALITY_ALL) != 0U) goto invalid;
    statistic->quality_flags = quality;
    state->result.statistic_count++;
    return 0;

invalid:
    hwa_set_error(error, error_size,
                  "measurement row %zu has an invalid STAT", row);
    return -1;
}

static int hwa_measure_read_warning(char **fields,
                                    size_t field_count,
                                    size_t row,
                                    HWAMeasureReadState *state,
                                    char *error,
                                    size_t error_size)
{
    HWAMeasureWarning *warning;
    uint64_t id;
    if (field_count != 6U ||
        state->result.warning_count >= state->expected_warnings ||
        hwa_measure_parse_u64(fields[1], &id) != 0 ||
        id != (uint64_t)state->result.warning_count + 1U ||
        fields[2][0] == '\0') goto invalid;
    warning = &state->result.warnings[state->result.warning_count];
    state->result.warning_count++;
    warning->id = id;
    warning->code = hwa_measure_read_copy(state, fields[2]);
    warning->message = hwa_measure_read_copy(state, fields[3]);
    if (warning->code == NULL || warning->message == NULL) goto invalid;
    if (fields[4][0] != '\0') {
        if (hwa_measure_parse_u64(fields[4], &warning->item_id) != 0 ||
            warning->item_id == 0U ||
            warning->item_id > (uint64_t)state->result.context_count) {
            goto invalid;
        }
        warning->item_id_valid = 1;
    }
    if (fields[5][0] != '\0') {
        if (hwa_measure_parse_u64(fields[5], &warning->observation_id) != 0 ||
            warning->observation_id == 0U ||
            warning->observation_id >
                (uint64_t)state->result.measurement_count) goto invalid;
        warning->observation_id_valid = 1;
    }
    return 0;

invalid:
    hwa_set_error(error, error_size,
                  "measurement row %zu has an invalid WARNING", row);
    return -1;
}

static int hwa_measure_read_row(char **fields,
                                size_t field_count,
                                size_t row,
                                void *user,
                                char *error,
                                size_t error_size)
{
    HWAMeasureReadState *state = (HWAMeasureReadState *)user;
    HWAMeasureFileSection section;
    size_t index;
    if (field_count == 0U || field_count > HWA_MEASURE_FILE_MAX_FIELDS) {
        hwa_set_error(error, error_size,
                      "measurement row %zu exceeds the field limit", row);
        return -1;
    }
    for (index = 0U; index < field_count; ++index) {
        if (strlen(fields[index]) > HWA_MEASURE_FILE_MAX_FIELD_BYTES) {
            hwa_set_error(error, error_size,
                          "measurement row %zu exceeds the field-byte limit",
                          row);
            return -1;
        }
    }
    if (state->row_count++ == 0U) {
        if (field_count != 2U || strcmp(fields[0], "HWA_MEASURES") != 0 ||
            strcmp(fields[1], "1") != 0) {
            hwa_set_error(error, error_size,
                          "measurement profile has an unsupported header");
            return -1;
        }
        state->section = HWA_MEASURE_SECTION_META;
        return 0;
    }
    if (strcmp(fields[0], "META") == 0) section = HWA_MEASURE_SECTION_META;
    else if (strcmp(fields[0], "INPUT") == 0) section = HWA_MEASURE_SECTION_INPUT;
    else if (strcmp(fields[0], "SOURCE") == 0) section = HWA_MEASURE_SECTION_SOURCE;
    else if (strcmp(fields[0], "CONTEXT") == 0) section = HWA_MEASURE_SECTION_CONTEXT;
    else if (strcmp(fields[0], "MEASURE") == 0) section = HWA_MEASURE_SECTION_MEASURE;
    else if (strcmp(fields[0], "GROUP") == 0) section = HWA_MEASURE_SECTION_GROUP;
    else if (strcmp(fields[0], "GROUP_MEMBER") == 0) section = HWA_MEASURE_SECTION_GROUP_MEMBER;
    else if (strcmp(fields[0], "STAT") == 0) section = HWA_MEASURE_SECTION_STAT;
    else if (strcmp(fields[0], "WARNING") == 0) section = HWA_MEASURE_SECTION_WARNING;
    else {
        hwa_set_error(error, error_size,
                      "measurement row %zu has an unknown record type", row);
        return -1;
    }
    if (section < state->section) {
        hwa_set_error(error, error_size,
                      "measurement row %zu is outside canonical order", row);
        return -1;
    }
    if (section > HWA_MEASURE_SECTION_META) {
        if (state->meta_index != sizeof(hwa_measure_meta_keys) /
                                  sizeof(hwa_measure_meta_keys[0]) ||
            !hwa_measure_saved_options_valid(&state->result.options) ||
            hwa_measure_read_allocate_arrays(state, error, error_size) != 0) {
            if (error != NULL && error_size != 0U && error[0] == '\0') {
                hwa_set_error(error, error_size,
                              "measurement META is incomplete or invalid");
            }
            return -1;
        }
    }
    if (section > HWA_MEASURE_SECTION_INPUT && state->input_count != 2U) {
        hwa_set_error(error, error_size,
                      "measurement row %zu starts before INPUT is complete",
                      row);
        return -1;
    }
    if (section > HWA_MEASURE_SECTION_SOURCE &&
        state->result.source_score_path == NULL) {
        hwa_set_error(error, error_size,
                      "measurement row %zu starts before SOURCE is complete",
                      row);
        return -1;
    }
    state->section = section;
    switch (section) {
    case HWA_MEASURE_SECTION_META:
        return hwa_measure_read_meta(fields, field_count, row, state,
                                     error, error_size);
    case HWA_MEASURE_SECTION_INPUT:
        return hwa_measure_read_input(fields, field_count, row, state,
                                      error, error_size);
    case HWA_MEASURE_SECTION_SOURCE:
        return hwa_measure_read_source(fields, field_count, row, state,
                                       error, error_size);
    case HWA_MEASURE_SECTION_CONTEXT:
        return hwa_measure_read_context(fields, field_count, row, state,
                                        error, error_size);
    case HWA_MEASURE_SECTION_MEASURE:
        return hwa_measure_read_observation(fields, field_count, row, state,
                                            error, error_size);
    case HWA_MEASURE_SECTION_GROUP:
        return hwa_measure_read_group(fields, field_count, row, state,
                                      error, error_size);
    case HWA_MEASURE_SECTION_GROUP_MEMBER:
        return hwa_measure_read_group_member(fields, field_count, row, state,
                                             error, error_size);
    case HWA_MEASURE_SECTION_STAT:
        return hwa_measure_read_statistic(fields, field_count, row, state,
                                          error, error_size);
    case HWA_MEASURE_SECTION_WARNING:
        return hwa_measure_read_warning(fields, field_count, row, state,
                                        error, error_size);
    default:
        return -1;
    }
}

static int hwa_measure_canonical_double_equal(double a,
                                              double b,
                                              unsigned max_ulps)
{
    unsigned step;
    if (a == b) return 1;
    if (!isfinite(a) || !isfinite(b) || signbit(a) != signbit(b)) return 0;
    for (step = 0U; step < max_ulps; ++step) {
        a = nextafter(a, b);
        if (a == b) return 1;
    }
    return 0;
}

static int hwa_measure_statistics_equal(const HWAMeasureStatistics *a,
                                        const HWAMeasureStatistics *b,
                                        unsigned max_ulps)
{
    return a->total_count == b->total_count &&
           a->valid_count == b->valid_count &&
           a->missing_count == b->missing_count && a->valid == b->valid &&
           (!a->valid ||
            (hwa_measure_canonical_double_equal(
                 a->minimum, b->minimum, max_ulps) &&
             hwa_measure_canonical_double_equal(a->q05, b->q05, max_ulps) &&
             hwa_measure_canonical_double_equal(a->q25, b->q25, max_ulps) &&
             hwa_measure_canonical_double_equal(a->q50, b->q50, max_ulps) &&
             hwa_measure_canonical_double_equal(a->q75, b->q75, max_ulps) &&
             hwa_measure_canonical_double_equal(a->q95, b->q95, max_ulps) &&
             hwa_measure_canonical_double_equal(
                 a->maximum, b->maximum, max_ulps) &&
             hwa_measure_canonical_double_equal(a->mean, b->mean, max_ulps) &&
             hwa_measure_canonical_double_equal(
                 a->population_sd, b->population_sd, max_ulps) &&
             hwa_measure_canonical_double_equal(
                 a->confidence, b->confidence, max_ulps)));
}

static int hwa_measure_context_key_compare(const void *left,
                                           const void *right)
{
    const char *const *a = (const char *const *)left;
    const char *const *b = (const char *const *)right;
    return strcmp(*a, *b);
}

static int hwa_measure_validate_context_keys(HWAMeasureReadState *state,
                                             char *error,
                                             size_t error_size)
{
    char **keys;
    uint64_t bytes;
    size_t index;
    if (state->result.context_count < 2U) return 0;
    if (state->result.context_count > SIZE_MAX / sizeof(*keys)) {
        hwa_set_error(error, error_size,
                      "measurement context-key index overflows");
        return -1;
    }
    bytes = (uint64_t)state->result.context_count * sizeof(*keys);
    if (hwa_measure_read_charge(state, bytes) != 0) {
        hwa_set_error(error, error_size,
                      "measurement context-key index exceeds work limit");
        return -1;
    }
    keys = (char **)malloc((size_t)bytes);
    if (keys == NULL) {
        state->work_live -= bytes;
        hwa_set_error(error, error_size,
                      "out of memory for measurement context keys");
        return -1;
    }
    for (index = 0U; index < state->result.context_count; ++index) {
        keys[index] = state->result.contexts[index].item_key;
    }
    qsort(keys, state->result.context_count, sizeof(*keys),
          hwa_measure_context_key_compare);
    for (index = 1U; index < state->result.context_count; ++index) {
        if (strcmp(keys[index - 1U], keys[index]) == 0) {
            hwa_set_error(error, error_size,
                          "measurement profile repeats item key '%s'",
                          keys[index]);
            free(keys);
            state->work_live -= bytes;
            return -1;
        }
    }
    free(keys);
    state->work_live -= bytes;
    return 0;
}

static int hwa_measure_double_compare(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static int hwa_measure_validate_level_reference(HWAMeasureReadState *state,
                                                char *error,
                                                size_t error_size)
{
    double *values = NULL;
    size_t count = 0U;
    size_t index;
    uint64_t bytes = 0U;
    double reference = 0.0;
    int reference_valid;

    for (index = 0U; index < state->result.measurement_count; ++index) {
        const HWAMeasureObservation *observation =
            &state->result.measurements[index];
        const HWAMeasureItemContext *context =
            &state->result.contexts[(size_t)observation->item_id - 1U];
        if (observation->kind == HWA_MEASURE_RMS_DBFS &&
            observation->index == 0U &&
            observation->view == HWA_MEASURE_VIEW_RAW &&
            observation->status == HWA_MEASURE_STATUS_VALID &&
            context->item_kind == HWA_ITEM_BODY && context->excluded == 0 &&
            (context->item_quality_flags &
             HWA_ITEM_QUALITY_LOW_CONFIDENCE) == 0U &&
            observation->value >= state->result.options.spectral_floor_dbfs) {
            count++;
        }
    }
    if (count != 0U) {
        if (count > SIZE_MAX / sizeof(*values)) goto work_error;
        bytes = (uint64_t)count * sizeof(*values);
        if (hwa_measure_read_charge(state, bytes) != 0) goto work_error;
        values = (double *)malloc((size_t)bytes);
        if (values == NULL) {
            state->work_live -= bytes;
            hwa_set_error(error, error_size,
                          "out of memory for the level reference");
            return -1;
        }
        count = 0U;
        for (index = 0U; index < state->result.measurement_count; ++index) {
            const HWAMeasureObservation *observation =
                &state->result.measurements[index];
            const HWAMeasureItemContext *context =
                &state->result.contexts[(size_t)observation->item_id - 1U];
            if (observation->kind == HWA_MEASURE_RMS_DBFS &&
                observation->index == 0U &&
                observation->view == HWA_MEASURE_VIEW_RAW &&
                observation->status == HWA_MEASURE_STATUS_VALID &&
                context->item_kind == HWA_ITEM_BODY &&
                context->excluded == 0 &&
                (context->item_quality_flags &
                 HWA_ITEM_QUALITY_LOW_CONFIDENCE) == 0U &&
                observation->value >=
                    state->result.options.spectral_floor_dbfs) {
                values[count++] = observation->value;
            }
        }
        qsort(values, count, sizeof(*values), hwa_measure_double_compare);
        reference = count % 2U != 0U
                        ? values[count / 2U]
                        : 0.5 * (values[count / 2U - 1U] +
                                 values[count / 2U]);
    }
    reference_valid = count != 0U;
    if (state->result.level_reference_valid != reference_valid ||
        state->result.level_reference_item_count != count ||
        (reference_valid &&
         state->result.level_reference_dbfs != reference)) {
        hwa_set_error(error, error_size,
                      "measurement level reference is not canonical");
        free(values);
        state->work_live -= bytes;
        return -1;
    }
    free(values);
    state->work_live -= bytes;
    return 0;

work_error:
    hwa_set_error(error, error_size,
                  "measurement level-reference work exceeds the limit");
    return -1;
}

static int hwa_measure_validate_level_pairs(HWAMeasureReadState *state,
                                            char *error,
                                            size_t error_size)
{
    size_t index = 0U;
    while (index < state->result.measurement_count) {
        const HWAMeasureObservation *raw = &state->result.measurements[index];
        const HWAMeasureItemContext *context =
            &state->result.contexts[(size_t)raw->item_id - 1U];
        HWAMeasureUnit relative_unit;
        int level_like = hwa_measure_kind_unit(
            raw->kind, HWA_MEASURE_VIEW_LEVEL_RELATIVE,
            &relative_unit) == 0;
        if (context->excluded != 0 || raw->view != HWA_MEASURE_VIEW_RAW ||
            (raw->evidence_flags &
             HWA_MEASURE_EVIDENCE_LEVEL_REFERENCE) != 0U) goto invalid;
        if (!level_like) {
            index++;
            continue;
        }
        if (index + 1U >= state->result.measurement_count) goto invalid;
        {
            const HWAMeasureObservation *relative =
                &state->result.measurements[index + 1U];
            HWAMeasureStatus expected_status = raw->status;
            uint32_t expected_evidence = raw->evidence_flags;
            if (raw->status == HWA_MEASURE_STATUS_VALID) {
                if (state->result.level_reference_valid != 0) {
                    expected_evidence |=
                        HWA_MEASURE_EVIDENCE_LEVEL_REFERENCE;
                } else {
                    expected_status = HWA_MEASURE_STATUS_NO_REFERENCE;
                }
            }
            if (relative->item_id != raw->item_id ||
                relative->kind != raw->kind ||
                relative->index != raw->index ||
                relative->unit != relative_unit ||
                relative->view != HWA_MEASURE_VIEW_LEVEL_RELATIVE ||
                relative->status != expected_status ||
                relative->confidence != raw->confidence ||
                relative->evidence_flags != expected_evidence ||
                relative->quality_flags != raw->quality_flags ||
                (expected_status == HWA_MEASURE_STATUS_VALID &&
                 relative->value !=
                     raw->value - state->result.level_reference_dbfs)) {
                goto invalid;
            }
        }
        index += 2U;
    }
    return 0;

invalid:
    hwa_set_error(error, error_size,
                  "measurement level-relative rows are not canonical");
    return -1;
}

static unsigned hwa_measure_bit_count32(uint32_t value)
{
    unsigned count = 0U;
    while (value != 0U) {
        count += value & 1U;
        value >>= 1U;
    }
    return count;
}

static int hwa_measure_audio_shape_valid(const HWAMeasurementSet *set)
{
    const HWAFormat *format = &set->audio_format;
    uint64_t expected_align;
    int encoding_valid;
    if (format->sample_rate_hz < 8000U ||
        format->sample_rate_hz > 768000U || format->frames == 0U ||
        format->frames > set->options.max_input_frames ||
        format->channels == 0U || format->channels > HWA_MAX_CHANNELS ||
        format->bits_per_sample == 0U ||
        (format->bits_per_sample & 7U) != 0U ||
        format->valid_bits_per_sample == 0U ||
        format->valid_bits_per_sample > format->bits_per_sample) {
        return 0;
    }
    encoding_valid =
        (format->encoding == HWA_ENCODING_PCM &&
         (format->bits_per_sample == 8U ||
          format->bits_per_sample == 16U ||
          format->bits_per_sample == 24U ||
          format->bits_per_sample == 32U)) ||
        (format->encoding == HWA_ENCODING_IEEE_FLOAT &&
         (format->bits_per_sample == 32U ||
          format->bits_per_sample == 64U) &&
         format->valid_bits_per_sample == format->bits_per_sample);
    if (!encoding_valid) return 0;
    expected_align = (uint64_t)format->channels *
                     ((uint64_t)format->bits_per_sample / 8U);
    return expected_align <= UINT16_MAX &&
           format->block_align == (uint16_t)expected_align &&
           (format->channel_mask == 0U ||
            hwa_measure_bit_count32(format->channel_mask) ==
                (unsigned)format->channels) &&
           format->data_bytes <= set->options.max_input_bytes;
}

static int hwa_measure_saved_counts_valid(const HWAMeasurementSet *set)
{
    return set->context_count <= set->options.max_items &&
           set->measurement_count <= set->options.max_measurements &&
           set->group_count <= set->options.max_groups &&
           set->group_member_count <= set->options.max_group_members &&
           set->statistic_count <= set->options.max_statistics &&
           set->warning_count <= set->options.max_warnings &&
           set->transform_count <= set->options.max_transforms &&
           set->item_frame_evaluations <=
               set->options.max_item_frame_evaluations &&
           set->level_reference_item_count <= set->context_count;
}

static int hwa_measure_saved_profile_equal(
    const HWAMeasureGroup *saved_groups,
    size_t saved_group_count,
    const HWAMeasureGroupMember *saved_members,
    size_t saved_member_count,
    const HWAMeasureStatistic *saved_statistics,
    size_t saved_statistic_count,
    const HWAMeasurementSet *rebuilt,
    unsigned max_statistic_ulps)
{
    size_t index;
    if (saved_group_count != rebuilt->group_count ||
        saved_member_count != rebuilt->group_member_count ||
        saved_statistic_count != rebuilt->statistic_count) return 0;
    for (index = 0U; index < saved_group_count; ++index) {
        const HWAMeasureGroup *a = &saved_groups[index];
        const HWAMeasureGroup *b = &rebuilt->groups[index];
        if (a->id != b->id || a->item_kind != b->item_kind ||
            a->selector != b->selector || a->member_count != b->member_count ||
            strcmp(a->key, b->key) != 0 ||
            strcmp(a->item_role, b->item_role) != 0 ||
            strcmp(a->value, b->value) != 0) return 0;
    }
    for (index = 0U; index < saved_member_count; ++index) {
        if (saved_members[index].group_id !=
                rebuilt->group_members[index].group_id ||
            saved_members[index].item_id !=
                rebuilt->group_members[index].item_id) return 0;
    }
    for (index = 0U; index < saved_statistic_count; ++index) {
        const HWAMeasureStatistic *a = &saved_statistics[index];
        const HWAMeasureStatistic *b = &rebuilt->statistics[index];
        if (a->id != b->id || a->group_id != b->group_id ||
            a->kind != b->kind || a->index != b->index ||
            a->unit != b->unit || a->view != b->view ||
            a->quality_flags != b->quality_flags ||
            !hwa_measure_statistics_equal(
                &a->statistics, &b->statistics, max_statistic_ulps)) {
            return 0;
        }
    }
    return 1;
}

static void hwa_measure_free_profile_rows(HWAMeasureGroup *groups,
                                          size_t group_count,
                                          HWAMeasureGroupMember *members,
                                          HWAMeasureStatistic *statistics)
{
    size_t index;
    for (index = 0U; index < group_count; ++index) {
        free(groups[index].key);
        free(groups[index].item_role);
        free(groups[index].value);
    }
    free(groups);
    free(members);
    free(statistics);
}

static int hwa_measure_rebuild_and_validate_profile(
    HWAMeasureReadState *state,
    char *error,
    size_t error_size)
{
    HWAMeasureGroup *saved_groups = state->result.groups;
    HWAMeasureGroupMember *saved_members = state->result.group_members;
    HWAMeasureStatistic *saved_statistics = state->result.statistics;
    size_t saved_group_count = state->result.group_count;
    size_t saved_member_count = state->result.group_member_count;
    size_t saved_statistic_count = state->result.statistic_count;
    HWAMeasurementOptions saved_options = state->result.options;
    int equal;

    state->result.groups = NULL;
    state->result.group_members = NULL;
    state->result.statistics = NULL;
    state->result.group_count = 0U;
    state->result.group_member_count = 0U;
    state->result.statistic_count = 0U;
    state->result.options.max_work_bytes = state->limits.max_work_bytes;
    state->result.options.max_items = state->limits.max_contexts;
    state->result.options.max_measurements = state->limits.max_measurements;
    state->result.options.max_groups = state->limits.max_groups;
    state->result.options.max_group_members = state->limits.max_group_members;
    state->result.options.max_statistics = state->limits.max_statistics;
    if (hwa_measure_build_profile(&state->result, error, error_size) != 0) {
        state->result.groups = saved_groups;
        state->result.group_members = saved_members;
        state->result.statistics = saved_statistics;
        state->result.group_count = saved_group_count;
        state->result.group_member_count = saved_member_count;
        state->result.statistic_count = saved_statistic_count;
        state->result.options = saved_options;
        return -1;
    }
    state->result.options = saved_options;
    equal = hwa_measure_saved_profile_equal(
        saved_groups, saved_group_count, saved_members, saved_member_count,
        saved_statistics, saved_statistic_count, &state->result,
        HWA_MEASURE_READER_STATISTIC_MAX_ULPS);
    if (!equal) {
        hwa_set_error(error, error_size,
                      "saved measurement distributions are not canonical");
        hwa_measure_free_profile_rows(
            state->result.groups, state->result.group_count,
            state->result.group_members, state->result.statistics);
        state->result.groups = saved_groups;
        state->result.group_members = saved_members;
        state->result.statistics = saved_statistics;
        state->result.group_count = saved_group_count;
        state->result.group_member_count = saved_member_count;
        state->result.statistic_count = saved_statistic_count;
        return -1;
    }
    hwa_measure_free_profile_rows(saved_groups, saved_group_count,
                                  saved_members, saved_statistics);
    if (state->saved_profile_bytes > state->result.retained_work_bytes) {
        hwa_set_error(error, error_size,
                      "measurement profile work accounting is invalid");
        return -1;
    }
    state->result.retained_work_bytes -= state->saved_profile_bytes;
    return 0;
}

static int hwa_measure_file_read_impl(
    const char *path,
    const HWAProfileComparisonOptions *limits,
    HWAMeasurementSet *set,
    char file_sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    HWAMeasureReadState state;
    HWAMeasureFileIdentity initial_identity;
    HWAMeasureFileIdentity final_identity;
    char after[HWA_SHA256_HEX_SIZE];
    double expected_duration;
    uint64_t expected_data_bytes;

    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (path == NULL || path[0] == '\0' || strcmp(path, "-") == 0 ||
        set == NULL || file_sha256 == NULL || limits == NULL ||
        limits->max_input_bytes == 0U || limits->max_work_bytes == 0U ||
        limits->max_contexts == 0U || limits->max_measurements == 0U ||
        limits->max_groups == 0U || limits->max_group_members == 0U ||
        limits->max_statistics == 0U || limits->max_warnings == 0U) {
        hwa_set_error(error, error_size,
                      "invalid measurement profile reader arguments");
        return -1;
    }
    memset(set, 0, sizeof(*set));
    file_sha256[0] = '\0';
    if (hwa_measure_file_identity_from_path(
            path, limits->max_input_bytes, &initial_identity,
            error, error_size) != 0) {
        return -1;
    }
    memset(&state, 0, sizeof(state));
    state.limits = *limits;
    if (hwa_measure_csv_stream(path, &initial_identity,
                               limits->max_input_bytes,
                               limits->max_work_bytes,
                               &state.work_live, &state.parser_bytes,
                               file_sha256, &state,
                               error, error_size) != 0 ||
        state.row_count == 0U ||
        state.meta_index != sizeof(hwa_measure_meta_keys) /
                            sizeof(hwa_measure_meta_keys[0]) ||
        !state.arrays_allocated || state.input_count != 2U ||
        state.result.alignment_path == NULL ||
        state.result.source_score_path == NULL ||
        state.result.context_count != state.expected_contexts ||
        state.result.measurement_count != state.expected_measurements ||
        state.result.group_count != state.expected_groups ||
        state.result.group_member_count != state.expected_group_members ||
        state.result.statistic_count != state.expected_statistics ||
        state.result.warning_count != state.expected_warnings) {
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "measurement row counts do not match META");
        }
        hwa_measurement_set_free(&state.result);
        return -1;
    }
    expected_duration = (double)state.result.audio_format.frames /
                        (double)state.result.audio_format.sample_rate_hz;
    if (!hwa_measure_audio_shape_valid(&state.result) ||
        !hwa_measure_saved_counts_valid(&state.result) ||
        fabs(state.result.audio_format.duration_seconds - expected_duration) >
            1e-12 ||
        state.result.audio_format.valid_bits_per_sample >
            state.result.audio_format.bits_per_sample ||
        state.result.audio_format.frames >
            UINT64_MAX / state.result.audio_format.block_align) {
        hwa_set_error(error, error_size,
                      "measurement audio shape is inconsistent");
        hwa_measurement_set_free(&state.result);
        return -1;
    }
    expected_data_bytes = state.result.audio_format.frames *
                          state.result.audio_format.block_align;
    if (expected_data_bytes != state.result.audio_format.data_bytes ||
        state.saved_retained_meta > state.result.options.max_work_bytes) {
        hwa_set_error(error, error_size,
                      "measurement saved work or audio bytes are inconsistent");
        hwa_measurement_set_free(&state.result);
        return -1;
    }
    state.result.retained_work_bytes = state.work_live;
    if (hwa_measure_validate_context_keys(&state, error, error_size) != 0 ||
        hwa_measure_validate_level_reference(&state,
                                             error, error_size) != 0 ||
        hwa_measure_validate_level_pairs(&state, error, error_size) != 0 ||
        hwa_measure_rebuild_and_validate_profile(&state,
                                                 error, error_size) != 0 ||
        state.saved_retained_meta != state.result.retained_work_bytes ||
        hwa_measure_sha256_for_identity(
            path, limits->max_input_bytes, &initial_identity, after,
            error, error_size) != 0 ||
        strcmp(after, file_sha256) != 0 ||
        hwa_measure_file_identity_from_path(
            path, limits->max_input_bytes, &final_identity,
            error, error_size) != 0 ||
        !hwa_measure_file_identity_equal(&initial_identity, &final_identity)) {
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          state.saved_retained_meta !=
                                  state.result.retained_work_bytes
                              ? "measurement retained-work total is not canonical"
                              : "measurement profile changed while reading");
        }
        hwa_measurement_set_free(&state.result);
        return -1;
    }
    *set = state.result;
    memset(&state.result, 0, sizeof(state.result));
    return 0;
}

int hwa_measure_file_read(
    const char *path,
    const HWAProfileComparisonOptions *limits,
    HWAMeasurementSet *set,
    char file_sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    HWANumericLocale locale;
    int result;
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        if (set != NULL) memset(set, 0, sizeof(*set));
        if (file_sha256 != NULL) file_sha256[0] = '\0';
        hwa_set_error(error, error_size,
                      "cannot select the C numeric locale");
        return -1;
    }
    result = hwa_measure_file_read_impl(
        path, limits, set, file_sha256, error, error_size);
    if (hwa_c_numeric_locale_end(&locale) != 0) {
        if (result == 0 && set != NULL) hwa_measurement_set_free(set);
        if (file_sha256 != NULL) file_sha256[0] = '\0';
        hwa_set_error(error, error_size,
                      "cannot restore the caller's numeric locale");
        return -1;
    }
    return result;
}

static int hwa_measure_csv_field(FILE *stream, const char *text)
{
    const unsigned char *cursor;
    int quoted = 0;
    if (text == NULL) text = "";
    cursor = (const unsigned char *)text;
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

static int hwa_measure_csv_number(FILE *stream, double value)
{
    if (!isfinite(value)) return -1;
    return fprintf(stream, "%.17g", value == 0.0 ? 0.0 : value) < 0 ? -1 : 0;
}

static int hwa_measure_csv_optional_number(FILE *stream,
                                           double value,
                                           int valid)
{
    return valid ? hwa_measure_csv_number(stream, value) : 0;
}

static int hwa_measure_csv_optional_u64(FILE *stream,
                                        uint64_t value,
                                        int valid)
{
    return valid && fprintf(stream, "%" PRIu64, value) < 0 ? -1 : 0;
}

static int hwa_measure_meta_text(FILE *stream,
                                 const char *key,
                                 const char *value,
                                 const char *unit)
{
    return fputs("META,", stream) == EOF ||
           hwa_measure_csv_field(stream, key) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_measure_csv_field(stream, value) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_measure_csv_field(stream, unit) != 0 ||
           fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_measure_meta_u64(FILE *stream,
                                const char *key,
                                uint64_t value,
                                const char *unit)
{
    return fprintf(stream, "META,%s,%" PRIu64 ",%s\r\n",
                   key, value, unit) < 0 ? -1 : 0;
}

static int hwa_measure_meta_size(FILE *stream,
                                 const char *key,
                                 size_t value,
                                 const char *unit)
{
    return fprintf(stream, "META,%s,%zu,%s\r\n",
                   key, value, unit) < 0 ? -1 : 0;
}

static int hwa_measure_meta_number(FILE *stream,
                                   const char *key,
                                   double value,
                                   const char *unit)
{
    return fputs("META,", stream) == EOF || fputs(key, stream) == EOF ||
           fputc(',', stream) == EOF ||
           hwa_measure_csv_number(stream, value) != 0 ||
           fputc(',', stream) == EOF || fputs(unit, stream) == EOF ||
           fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_measure_meta_optional_number(FILE *stream,
                                            const char *key,
                                            double value,
                                            const char *unit,
                                            int valid)
{
    return fputs("META,", stream) == EOF || fputs(key, stream) == EOF ||
           fputc(',', stream) == EOF ||
           hwa_measure_csv_optional_number(stream, value, valid) != 0 ||
           fputc(',', stream) == EOF || fputs(unit, stream) == EOF ||
           fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_measure_path_hex(FILE *stream, const char *path)
{
    const unsigned char *cursor = (const unsigned char *)path;
    if (path == NULL) return -1;
    while (*cursor != 0U) {
        if (fprintf(stream, "%02x", (unsigned)*cursor) < 0) return -1;
        cursor++;
    }
    return 0;
}

static int hwa_measure_valid_sha256(const char *text)
{
    size_t index;
    if (text == NULL || strlen(text) != HWA_SHA256_HEX_SIZE - 1U) return 0;
    for (index = 0U; index < HWA_SHA256_HEX_SIZE - 1U; ++index) {
        if (!((text[index] >= '0' && text[index] <= '9') ||
              (text[index] >= 'a' && text[index] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static int hwa_measure_write_input(FILE *stream,
                                   const char *role,
                                   const char *path,
                                   const char *sha256,
                                   const HWAFormat *format)
{
    if (path == NULL || path[0] == '\0' || !hwa_measure_valid_sha256(sha256) ||
        fprintf(stream, "INPUT,%s,", role) < 0 ||
        hwa_measure_path_hex(stream, path) != 0 ||
        fprintf(stream, ",%s,", sha256) < 0) {
        return -1;
    }
    if (format != NULL) {
        if (hwa_measure_csv_number(stream, format->duration_seconds) != 0 ||
            fprintf(stream, ",%" PRIu32 ",%" PRIu64,
                    format->sample_rate_hz, format->frames) < 0) {
            return -1;
        }
    } else if (fputs(",,", stream) == EOF) {
        return -1;
    }
    return fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_measure_write_source(FILE *stream,
                                    const char *role,
                                    const char *path,
                                    const char *sha256)
{
    return path == NULL || path[0] == '\0' ||
           !hwa_measure_valid_sha256(sha256) ||
           fprintf(stream, "SOURCE,%s,", role) < 0 ||
           hwa_measure_path_hex(stream, path) != 0 ||
           fprintf(stream, ",%s\r\n", sha256) < 0 ? -1 : 0;
}

static int hwa_measure_write_build(FILE *stream)
{
    return hwa_measure_meta_text(stream, "tool_version", HWA_VERSION, "") ||
           hwa_measure_meta_text(stream, "analysis_method_version",
                                 HWA_ANALYSIS_METHOD_VERSION, "") ||
           hwa_measure_meta_text(stream, "alignment_method_version",
                                 HWA_ALIGNMENT_METHOD_VERSION, "") ||
           hwa_measure_meta_text(stream, "segmentation_method_version",
                                 HWA_SEGMENTATION_METHOD_VERSION, "") ||
           hwa_measure_meta_text(stream, "measurement_method_version",
                                 HWA_MEASUREMENT_METHOD_VERSION, "") ||
           hwa_measure_meta_text(stream, "build_compiler_family",
                                 hwa_build_compiler_family(), "") ||
           hwa_measure_meta_text(stream, "build_compiler_version",
                                 hwa_build_compiler_version(), "") ||
           hwa_measure_meta_text(stream, "build_c_standard",
                                 hwa_build_c_standard(), "") ||
           hwa_measure_meta_text(stream, "build_target_os",
                                 hwa_build_target_os(), "") ||
           hwa_measure_meta_u64(stream, "build_pointer_bits",
                                hwa_build_pointer_bits(), "bits") ||
           hwa_measure_meta_text(stream, "build_endianness",
                                 hwa_build_endianness(), "") ||
           hwa_measure_meta_text(stream, "build_mode", hwa_build_mode(), "")
               ? -1 : 0;
}

static int hwa_measure_write_meta(FILE *stream,
                                  const HWAMeasurementSet *set)
{
    const HWAMeasurementOptions *options = &set->options;
    return hwa_measure_write_build(stream) ||
           hwa_measure_meta_u64(stream, "sample_rate_hz",
                                set->audio_format.sample_rate_hz, "Hz") ||
           hwa_measure_meta_u64(stream, "audio_frames",
                                set->audio_format.frames, "frames") ||
           hwa_measure_meta_number(stream, "audio_duration_seconds",
                                   set->audio_format.duration_seconds,
                                   "seconds") ||
           hwa_measure_meta_u64(stream, "audio_channels",
                                set->audio_format.channels, "channels") ||
           hwa_measure_meta_text(stream, "audio_container",
                                 hwa_container_name(set->audio_format.container),
                                 "") ||
           hwa_measure_meta_text(stream, "audio_encoding",
                                 hwa_encoding_name(set->audio_format.encoding),
                                 "") ||
           hwa_measure_meta_u64(stream, "bits_per_sample",
                                set->audio_format.bits_per_sample, "bits") ||
           hwa_measure_meta_u64(stream, "valid_bits_per_sample",
                                set->audio_format.valid_bits_per_sample,
                                "bits") ||
           hwa_measure_meta_u64(stream, "block_align",
                                set->audio_format.block_align, "bytes") ||
           hwa_measure_meta_u64(stream, "channel_mask",
                                set->audio_format.channel_mask, "bitset") ||
           hwa_measure_meta_u64(stream, "data_bytes",
                                set->audio_format.data_bytes, "bytes") ||
           hwa_measure_meta_optional_number(stream, "level_reference_dbfs",
                                            set->level_reference_dbfs, "dBFS",
                                            set->level_reference_valid) ||
           hwa_measure_meta_size(stream, "level_reference_item_count",
                                 set->level_reference_item_count, "items") ||
           hwa_measure_meta_u64(stream, "capability_flags",
                                set->capability_flags, "bitset") ||
           hwa_measure_meta_u64(stream, "production_corrected_available",
                                0U, "boolean") ||
           hwa_measure_meta_u64(stream, "item_frame_evaluations",
                                set->item_frame_evaluations, "evaluations") ||
           hwa_measure_meta_u64(stream, "retained_work_bytes",
                                set->retained_work_bytes, "bytes") ||
           hwa_measure_meta_size(stream, "transform_count",
                                 set->transform_count, "transforms") ||
           hwa_measure_meta_size(stream, "decode_block_frames",
                                 options->decode_block_frames, "frames") ||
           hwa_measure_meta_size(stream, "fft_size", options->fft_size,
                                 "samples") ||
           hwa_measure_meta_size(stream, "hop_size", options->hop_size,
                                 "samples") ||
           hwa_measure_meta_number(stream, "pitch_confidence_floor",
                                   options->pitch_confidence_floor, "ratio") ||
           hwa_measure_meta_number(stream, "spectral_floor_dbfs",
                                   options->spectral_floor_dbfs, "dBFS") ||
           hwa_measure_meta_size(stream, "max_partials",
                                 options->max_partials, "partials") ||
           hwa_measure_meta_u64(stream, "max_input_bytes",
                                options->max_input_bytes, "bytes") ||
           hwa_measure_meta_u64(stream, "max_input_frames",
                                options->max_input_frames, "frames") ||
           hwa_measure_meta_u64(stream, "max_work_bytes",
                                options->max_work_bytes, "bytes") ||
           hwa_measure_meta_size(stream, "max_transforms",
                                 options->max_transforms, "transforms") ||
           hwa_measure_meta_size(stream, "max_series_points",
                                 options->max_series_points, "points") ||
           hwa_measure_meta_u64(stream, "max_item_frame_evaluations",
                                options->max_item_frame_evaluations,
                                "evaluations") ||
           hwa_measure_meta_size(stream, "max_events", options->max_events,
                                 "events") ||
           hwa_measure_meta_size(stream, "max_items", options->max_items,
                                 "items") ||
           hwa_measure_meta_size(stream, "max_item_members",
                                 options->max_item_members, "members") ||
           hwa_measure_meta_size(stream, "max_measurements",
                                 options->max_measurements, "measurements") ||
           hwa_measure_meta_size(stream, "max_groups", options->max_groups,
                                 "groups") ||
           hwa_measure_meta_size(stream, "max_group_members",
                                 options->max_group_members, "members") ||
           hwa_measure_meta_size(stream, "max_statistics",
                                 options->max_statistics, "statistics") ||
           hwa_measure_meta_size(stream, "max_warnings",
                                 options->max_warnings, "warnings") ||
           hwa_measure_meta_size(stream, "context_count", set->context_count,
                                 "contexts") ||
           hwa_measure_meta_size(stream, "measurement_count",
                                 set->measurement_count, "measurements") ||
           hwa_measure_meta_size(stream, "group_count", set->group_count,
                                 "groups") ||
           hwa_measure_meta_size(stream, "group_member_count",
                                 set->group_member_count, "members") ||
           hwa_measure_meta_size(stream, "statistic_count",
                                 set->statistic_count, "statistics") ||
           hwa_measure_meta_size(stream, "warning_count", set->warning_count,
                                 "warnings") ? -1 : 0;
}

static int hwa_measure_write_context(FILE *stream,
                                     const HWAMeasureItemContext *context)
{
    const char *values[] = {
        context->labels.pitch, context->labels.register_name,
        context->labels.dynamic, context->labels.articulation,
        context->labels.part, context->labels.physical_element,
        context->labels.controller, context->labels.technique,
        context->labels.score_section, context->labels.transition,
        context->labels.gesture
    };
    const char *kind = hwa_measure_item_kind_name(context->item_kind);
    size_t index;
    if (kind == NULL || context->item_id == 0U ||
        context->item_key == NULL || context->item_key[0] == '\0' ||
        context->item_role == NULL || context->item_role[0] == '\0' ||
        context->start_sample > context->end_sample ||
        !isfinite(context->item_confidence) ||
        context->item_confidence < 0.0 || context->item_confidence > 1.0 ||
        fprintf(stream, "CONTEXT,%" PRIu64 ",", context->item_id) < 0 ||
        hwa_measure_csv_field(stream, context->item_key) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_measure_csv_field(stream, kind) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_measure_csv_field(stream, context->item_role) != 0 ||
        fprintf(stream, ",%" PRIu64 ",%" PRIu64,
                context->start_sample, context->end_sample) < 0) {
        return -1;
    }
    for (index = 0U; index < sizeof(values) / sizeof(values[0]); ++index) {
        if (fputc(',', stream) == EOF ||
            hwa_measure_csv_field(stream, values[index]) != 0) return -1;
    }
    return fprintf(stream, ",%" PRIu32 ",%zu,",
                   context->labels.override_flags,
                   context->source_event_count) < 0 ||
           hwa_measure_csv_number(stream, context->item_confidence) != 0 ||
           fprintf(stream, ",%" PRIu32 ",%d\r\n",
                   context->item_quality_flags,
                   context->excluded ? 1 : 0) < 0 ? -1 : 0;
}

static int hwa_measure_write_observation(
    FILE *stream,
    const HWAMeasureObservation *observation,
    size_t max_partials)
{
    const char *kind = hwa_measure_kind_name(observation->kind);
    const char *unit = hwa_measure_unit_name(observation->unit);
    const char *view = hwa_measure_view_name(observation->view);
    const char *status = hwa_measure_status_name(observation->status);
    HWAMeasureUnit expected_unit;
    int valid = observation->status == HWA_MEASURE_STATUS_VALID;
    return kind == NULL || unit == NULL || view == NULL || status == NULL ||
           observation->id == 0U || observation->item_id == 0U ||
           !hwa_measure_kind_index_valid(observation->kind,
                                         observation->index, max_partials) ||
           hwa_measure_kind_unit(observation->kind, observation->view,
                                 &expected_unit) != 0 ||
           expected_unit != observation->unit ||
           !isfinite(observation->value) ||
           (!valid && observation->value != 0.0) ||
           !isfinite(observation->confidence) ||
           observation->confidence < 0.0 || observation->confidence > 1.0 ||
           fprintf(stream, "MEASURE,%" PRIu64 ",%" PRIu64 ",%s,%" PRIu32
                   ",%s,%s,%s,",
                   observation->id, observation->item_id, kind,
                   observation->index, unit, view, status) < 0 ||
           hwa_measure_csv_optional_number(stream, observation->value,
                                           valid) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_measure_csv_number(stream, observation->confidence) != 0 ||
           fprintf(stream, ",%" PRIu32 ",%" PRIu32 "\r\n",
                   observation->evidence_flags,
                   observation->quality_flags) < 0 ? -1 : 0;
}

static int hwa_measure_write_group(FILE *stream,
                                   const HWAMeasureGroup *group,
                                   int include_member_count)
{
    const char *kind = hwa_measure_item_kind_name(group->item_kind);
    const char *selector = hwa_measure_group_selector_name(group->selector);
    if (kind == NULL || selector == NULL || group->id == 0U ||
        group->key == NULL || group->key[0] == '\0' ||
        group->item_role == NULL || group->item_role[0] == '\0' ||
        group->value == NULL ||
        (group->selector == HWA_MEASURE_GROUP_ALL && group->value[0] != '\0') ||
        fprintf(stream, "GROUP,%" PRIu64 ",", group->id) < 0 ||
        hwa_measure_csv_field(stream, group->key) != 0 ||
        fputc(',', stream) == EOF || hwa_measure_csv_field(stream, kind) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_measure_csv_field(stream, group->item_role) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_measure_csv_field(stream, selector) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_measure_csv_field(stream, group->value) != 0) {
        return -1;
    }
    if (include_member_count && fprintf(stream, ",%zu", group->member_count) < 0) {
        return -1;
    }
    return fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_measure_write_statistics_fields(
    FILE *stream,
    const HWAMeasureStatistics *statistics)
{
    int valid = statistics->valid;
    if (statistics->valid_count > statistics->total_count ||
        statistics->missing_count !=
            statistics->total_count - statistics->valid_count ||
        valid != (statistics->valid_count != 0U) ||
        fprintf(stream, "%zu,%zu,%zu,", statistics->total_count,
                statistics->valid_count, statistics->missing_count) < 0 ||
        hwa_measure_csv_optional_number(stream, statistics->minimum, valid) ||
        fputc(',', stream) == EOF ||
        hwa_measure_csv_optional_number(stream, statistics->q05, valid) ||
        fputc(',', stream) == EOF ||
        hwa_measure_csv_optional_number(stream, statistics->q25, valid) ||
        fputc(',', stream) == EOF ||
        hwa_measure_csv_optional_number(stream, statistics->q50, valid) ||
        fputc(',', stream) == EOF ||
        hwa_measure_csv_optional_number(stream, statistics->q75, valid) ||
        fputc(',', stream) == EOF ||
        hwa_measure_csv_optional_number(stream, statistics->q95, valid) ||
        fputc(',', stream) == EOF ||
        hwa_measure_csv_optional_number(stream, statistics->maximum, valid) ||
        fputc(',', stream) == EOF ||
        hwa_measure_csv_optional_number(stream, statistics->mean, valid) ||
        fputc(',', stream) == EOF ||
        hwa_measure_csv_optional_number(stream, statistics->population_sd,
                                        valid) ||
        fputc(',', stream) == EOF ||
        hwa_measure_csv_optional_number(stream, statistics->confidence,
                                        valid) ||
        fprintf(stream, ",%d", valid ? 1 : 0) < 0) {
        return -1;
    }
    return 0;
}

static int hwa_measure_write_statistic(FILE *stream,
                                       const HWAMeasureStatistic *statistic)
{
    const char *kind = hwa_measure_kind_name(statistic->kind);
    const char *unit = hwa_measure_unit_name(statistic->unit);
    const char *view = hwa_measure_view_name(statistic->view);
    return kind == NULL || unit == NULL || view == NULL ||
           statistic->id == 0U || statistic->group_id == 0U ||
           fprintf(stream, "STAT,%" PRIu64 ",%" PRIu64 ",%s,%" PRIu32
                   ",%s,%s,",
                   statistic->id, statistic->group_id, kind,
                   statistic->index, unit, view) < 0 ||
           hwa_measure_write_statistics_fields(stream,
                                               &statistic->statistics) != 0 ||
           fprintf(stream, ",%" PRIu32 "\r\n",
                   statistic->quality_flags) < 0 ? -1 : 0;
}

static int hwa_measure_write_warning(FILE *stream,
                                     const HWAMeasureWarning *warning)
{
    return warning->id == 0U || warning->code == NULL ||
           warning->code[0] == '\0' || warning->message == NULL ||
           fprintf(stream, "WARNING,%" PRIu64 ",", warning->id) < 0 ||
           hwa_measure_csv_field(stream, warning->code) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_measure_csv_field(stream, warning->message) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_measure_csv_optional_u64(stream, warning->item_id,
                                        warning->item_id_valid) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_measure_csv_optional_u64(stream, warning->observation_id,
                                        warning->observation_id_valid) != 0 ||
           fputs("\r\n", stream) == EOF ? -1 : 0;
}

static int hwa_measure_retained_add(uint64_t *total,
                                    uint64_t bytes)
{
    if (bytes > UINT64_MAX - *total) return -1;
    *total += bytes;
    return 0;
}

static int hwa_measure_retained_text(uint64_t *total, const char *text)
{
    size_t size;
    if (text == NULL) return 0;
    size = strlen(text);
    if (size > HWA_MEASURE_FILE_MAX_FIELD_BYTES) return -1;
    return hwa_measure_retained_add(total, (uint64_t)size + 1U);
}

static int hwa_measure_retained_array(uint64_t *total,
                                      size_t count,
                                      size_t element_size)
{
    if (element_size != 0U && count > SIZE_MAX / element_size) return -1;
    return hwa_measure_retained_add(
        total, (uint64_t)count * (uint64_t)element_size);
}

static int hwa_measure_retained_total(const HWAMeasurementSet *set,
                                      uint64_t *total)
{
    size_t index;
    *total = 0U;
    if (hwa_measure_retained_text(total, set->items_path) != 0 ||
        hwa_measure_retained_text(total, set->audio_path) != 0 ||
        hwa_measure_retained_text(total, set->alignment_path) != 0 ||
        hwa_measure_retained_text(total, set->labels_path) != 0 ||
        hwa_measure_retained_text(total, set->amendment_path) != 0 ||
        hwa_measure_retained_text(total, set->source_score_path) != 0 ||
        hwa_measure_retained_array(total, set->context_count,
                                   sizeof(*set->contexts)) != 0 ||
        hwa_measure_retained_array(total, set->measurement_count,
                                   sizeof(*set->measurements)) != 0 ||
        hwa_measure_retained_array(total, set->group_count,
                                   sizeof(*set->groups)) != 0 ||
        hwa_measure_retained_array(total, set->group_member_count,
                                   sizeof(*set->group_members)) != 0 ||
        hwa_measure_retained_array(total, set->statistic_count,
                                   sizeof(*set->statistics)) != 0 ||
        hwa_measure_retained_array(total, set->warning_count,
                                   sizeof(*set->warnings)) != 0) return -1;
    for (index = 0U; index < set->context_count; ++index) {
        const HWAMeasureItemContext *context = &set->contexts[index];
        const char *labels[] = {
            context->labels.pitch, context->labels.register_name,
            context->labels.dynamic, context->labels.articulation,
            context->labels.part, context->labels.physical_element,
            context->labels.controller, context->labels.technique,
            context->labels.score_section, context->labels.transition,
            context->labels.gesture
        };
        size_t label;
        if (hwa_measure_retained_text(total, context->item_key) != 0 ||
            hwa_measure_retained_text(total, context->item_role) != 0) {
            return -1;
        }
        for (label = 0U; label < 11U; ++label) {
            if (hwa_measure_retained_text(total, labels[label]) != 0) {
                return -1;
            }
        }
    }
    for (index = 0U; index < set->group_count; ++index) {
        if (hwa_measure_retained_text(total, set->groups[index].key) != 0 ||
            hwa_measure_retained_text(total,
                                      set->groups[index].item_role) != 0 ||
            hwa_measure_retained_text(total, set->groups[index].value) != 0) {
            return -1;
        }
    }
    for (index = 0U; index < set->warning_count; ++index) {
        if (hwa_measure_retained_text(total, set->warnings[index].code) != 0 ||
            hwa_measure_retained_text(total,
                                      set->warnings[index].message) != 0) {
            return -1;
        }
    }
    return 0;
}

static int hwa_measure_set_canonical(const HWAMeasurementSet *set,
                                     char *error,
                                     size_t error_size)
{
    HWAMeasureReadState validation;
    HWAMeasurementSet rebuilt;
    double duration;
    uint64_t data_bytes;
    uint64_t retained;
    size_t index;
    int equal;
    memset(&validation, 0, sizeof(validation));
    if (!hwa_measure_saved_options_valid(&set->options) ||
        !hwa_measure_audio_shape_valid(set) ||
        !hwa_measure_saved_counts_valid(set) || set->capability_flags != 0U ||
        set->audio_format.frames >
            UINT64_MAX / set->audio_format.block_align) goto invalid;
    duration = (double)set->audio_format.frames /
               (double)set->audio_format.sample_rate_hz;
    data_bytes = set->audio_format.frames * set->audio_format.block_align;
    if (fabs(set->audio_format.duration_seconds - duration) > 1e-12 ||
        set->audio_format.data_bytes != data_bytes ||
        set->retained_work_bytes > set->options.max_work_bytes ||
        hwa_measure_retained_total(set, &retained) != 0 ||
        retained != set->retained_work_bytes) goto invalid;
    for (index = 0U; index < set->context_count; ++index) {
        const HWAMeasureItemContext *context = &set->contexts[index];
        const char *labels[] = {
            context->labels.pitch, context->labels.register_name,
            context->labels.dynamic, context->labels.articulation,
            context->labels.part, context->labels.physical_element,
            context->labels.controller, context->labels.technique,
            context->labels.score_section, context->labels.transition,
            context->labels.gesture
        };
        size_t other;
        size_t label;
        if (context->item_id != (uint64_t)index + 1U ||
            context->item_key == NULL || context->item_key[0] == '\0' ||
            context->item_role == NULL || context->item_role[0] == '\0' ||
            (context->item_quality_flags &
             ~((UINT32_C(1) << 4) - 1U)) != 0U ||
            (context->labels.override_flags & ~HWA_LABEL_OVERRIDE_ALL) != 0U) {
            goto invalid;
        }
        for (label = 0U; label < 11U; ++label) {
            if ((context->labels.override_flags &
                 (UINT32_C(1) << label)) != 0U && labels[label] == NULL) {
                goto invalid;
            }
        }
        for (other = 0U; other < index; ++other) {
            if (strcmp(context->item_key,
                       set->contexts[other].item_key) == 0) goto invalid;
        }
    }
    for (index = 0U; index < set->warning_count; ++index) {
        const HWAMeasureWarning *warning = &set->warnings[index];
        if (warning->id != (uint64_t)index + 1U || warning->code == NULL ||
            warning->code[0] == '\0' || warning->message == NULL ||
            (warning->item_id_valid &&
             (warning->item_id == 0U ||
              warning->item_id > (uint64_t)set->context_count)) ||
            (warning->observation_id_valid &&
             (warning->observation_id == 0U ||
              warning->observation_id >
                  (uint64_t)set->measurement_count))) goto invalid;
    }
    validation.limits.max_work_bytes = set->options.max_work_bytes;
    validation.result = *set;
    if (hwa_measure_validate_level_reference(&validation,
                                             error, error_size) != 0 ||
        hwa_measure_validate_level_pairs(&validation,
                                         error, error_size) != 0) return -1;
    rebuilt = *set;
    rebuilt.groups = NULL;
    rebuilt.group_count = 0U;
    rebuilt.group_members = NULL;
    rebuilt.group_member_count = 0U;
    rebuilt.statistics = NULL;
    rebuilt.statistic_count = 0U;
    rebuilt.retained_work_bytes = 0U;
    if (hwa_measure_build_profile(&rebuilt, error, error_size) != 0) return -1;
    equal = hwa_measure_saved_profile_equal(
        set->groups, set->group_count, set->group_members,
        set->group_member_count, set->statistics, set->statistic_count,
        &rebuilt, 0U);
    hwa_measure_free_profile_rows(rebuilt.groups, rebuilt.group_count,
                                  rebuilt.group_members,
                                  rebuilt.statistics);
    if (!equal) {
        hwa_set_error(error, error_size,
                      "measurement distributions are not canonical");
        return -1;
    }
    return 0;

invalid:
    hwa_set_error(error, error_size,
                  "measurement set cannot form a canonical profile");
    return -1;
}

static int hwa_measure_file_write_impl(FILE *stream,
                                       const HWAMeasurementSet *set,
                                       char *error,
                                       size_t error_size)
{
    size_t index;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (stream == NULL || set == NULL || set->items_path == NULL ||
        set->audio_path == NULL || set->alignment_path == NULL ||
        set->source_score_path == NULL ||
        (set->context_count != 0U && set->contexts == NULL) ||
        (set->measurement_count != 0U && set->measurements == NULL) ||
        (set->group_count != 0U && set->groups == NULL) ||
        (set->group_member_count != 0U && set->group_members == NULL) ||
        (set->statistic_count != 0U && set->statistics == NULL) ||
        (set->warning_count != 0U && set->warnings == NULL)) {
        hwa_set_error(error, error_size, "invalid measurement writer arguments");
        return -1;
    }
    if (hwa_measure_set_canonical(set, error, error_size) != 0) return -1;
    if (set->items_path == NULL ||
        set->audio_path == NULL || set->alignment_path == NULL ||
        set->source_score_path == NULL ||
        !hwa_measure_valid_sha256(set->items_sha256) ||
        !hwa_measure_valid_sha256(set->audio_sha256) ||
        !hwa_measure_valid_sha256(set->alignment_sha256) ||
        !hwa_measure_valid_sha256(set->source_score_sha256) ||
        set->audio_format.sample_rate_hz == 0U ||
        (set->labels_path == NULL && set->labels_sha256[0] != '\0') ||
        (set->labels_path != NULL &&
         !hwa_measure_valid_sha256(set->labels_sha256)) ||
        (set->amendment_path == NULL && set->amendment_sha256[0] != '\0') ||
        (set->amendment_path != NULL &&
         !hwa_measure_valid_sha256(set->amendment_sha256)) ||
        (set->context_count != 0U && set->contexts == NULL) ||
        (set->measurement_count != 0U && set->measurements == NULL) ||
        (set->group_count != 0U && set->groups == NULL) ||
        (set->group_member_count != 0U && set->group_members == NULL) ||
        (set->statistic_count != 0U && set->statistics == NULL) ||
        (set->warning_count != 0U && set->warnings == NULL) ||
        fputs("HWA_MEASURES,1\r\n", stream) == EOF ||
        hwa_measure_write_meta(stream, set) != 0 ||
        hwa_measure_write_input(stream, "items", set->items_path,
                                set->items_sha256, NULL) != 0 ||
        hwa_measure_write_input(stream, "audio", set->audio_path,
                                set->audio_sha256, &set->audio_format) != 0 ||
        hwa_measure_write_source(stream, "alignment", set->alignment_path,
                                 set->alignment_sha256) != 0 ||
        (set->labels_path != NULL &&
         hwa_measure_write_source(stream, "labels", set->labels_path,
                                  set->labels_sha256) != 0) ||
        (set->amendment_path != NULL &&
         hwa_measure_write_source(stream, "amendment", set->amendment_path,
                                  set->amendment_sha256) != 0) ||
        hwa_measure_write_source(stream, "score", set->source_score_path,
                                 set->source_score_sha256) != 0) {
        hwa_set_error(error, error_size, "cannot write measurement metadata");
        return -1;
    }
    for (index = 0U; index < set->context_count; ++index) {
        if (set->contexts[index].item_id != (uint64_t)index + 1U ||
            hwa_measure_write_context(stream, &set->contexts[index]) != 0) {
            hwa_set_error(error, error_size,
                          "cannot write measurement context %zu", index);
            return -1;
        }
    }
    for (index = 0U; index < set->measurement_count; ++index) {
        if (set->measurements[index].id != (uint64_t)index + 1U ||
            hwa_measure_write_observation(stream, &set->measurements[index],
                                          set->options.max_partials) != 0) {
            hwa_set_error(error, error_size,
                          "cannot write scalar measurement %zu", index);
            return -1;
        }
    }
    for (index = 0U; index < set->group_count; ++index) {
        if (set->groups[index].id != (uint64_t)index + 1U ||
            hwa_measure_write_group(stream, &set->groups[index], 1) != 0) {
            hwa_set_error(error, error_size,
                          "cannot write measurement group %zu", index);
            return -1;
        }
    }
    for (index = 0U; index < set->group_member_count; ++index) {
        const HWAMeasureGroupMember *member = &set->group_members[index];
        if (member->group_id == 0U ||
            member->group_id > (uint64_t)set->group_count ||
            member->item_id == 0U ||
            member->item_id > (uint64_t)set->context_count ||
            fprintf(stream, "GROUP_MEMBER,%" PRIu64 ",%" PRIu64 "\r\n",
                    member->group_id, member->item_id) < 0) {
            hwa_set_error(error, error_size,
                          "cannot write measurement group member %zu", index);
            return -1;
        }
    }
    for (index = 0U; index < set->statistic_count; ++index) {
        if (set->statistics[index].id != (uint64_t)index + 1U ||
            hwa_measure_write_statistic(stream, &set->statistics[index]) != 0) {
            hwa_set_error(error, error_size,
                          "cannot write measurement statistic %zu", index);
            return -1;
        }
    }
    for (index = 0U; index < set->warning_count; ++index) {
        if (set->warnings[index].id != (uint64_t)index + 1U ||
            hwa_measure_write_warning(stream, &set->warnings[index]) != 0) {
            hwa_set_error(error, error_size,
                          "cannot write measurement warning %zu", index);
            return -1;
        }
    }
    return 0;
}

int hwa_measure_file_write(FILE *stream,
                           const HWAMeasurementSet *set,
                           char *error,
                           size_t error_size)
{
    HWANumericLocale locale;
    int result;
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_set_error(error, error_size,
                      "cannot select the C numeric locale");
        return -1;
    }
    result = hwa_measure_file_write_impl(stream, set, error, error_size);
    if (hwa_c_numeric_locale_end(&locale) != 0) {
        hwa_set_error(error, error_size,
                      "cannot restore the caller's numeric locale");
        return -1;
    }
    return result;
}

static int hwa_compare_write_meta(FILE *stream,
                                  const HWAProfileComparisonSet *set)
{
    const HWAProfileComparisonOptions *options = &set->options;
    return hwa_measure_write_build(stream) ||
           hwa_measure_meta_text(stream, "profile_comparison_method_version",
                                 HWA_PROFILE_COMPARISON_METHOD_VERSION, "") ||
           hwa_measure_meta_u64(stream, "retained_work_bytes",
                                set->retained_work_bytes, "bytes") ||
           hwa_measure_meta_u64(stream, "max_input_bytes",
                                options->max_input_bytes, "bytes") ||
           hwa_measure_meta_u64(stream, "max_work_bytes",
                                options->max_work_bytes, "bytes") ||
           hwa_measure_meta_size(stream, "max_contexts",
                                 options->max_contexts, "contexts") ||
           hwa_measure_meta_size(stream, "max_measurements",
                                 options->max_measurements, "measurements") ||
           hwa_measure_meta_size(stream, "max_groups",
                                 options->max_groups, "groups") ||
           hwa_measure_meta_size(stream, "max_group_members",
                                 options->max_group_members, "members") ||
           hwa_measure_meta_size(stream, "max_statistics",
                                 options->max_statistics, "statistics") ||
           hwa_measure_meta_size(stream, "max_warnings",
                                 options->max_warnings, "warnings") ||
           hwa_measure_meta_size(stream, "max_distributions",
                                 options->max_distributions, "distributions") ||
           hwa_measure_meta_size(stream, "max_gaps",
                                 options->max_gaps, "gaps") ||
           hwa_measure_meta_size(stream, "group_count", set->group_count,
                                 "groups") ||
           hwa_measure_meta_size(stream, "distribution_count",
                                 set->distribution_count, "distributions") ||
           hwa_measure_meta_size(stream, "gap_count", set->gap_count,
                                 "gaps") ||
           hwa_measure_meta_size(stream, "warning_count", set->warning_count,
                                 "warnings") ? -1 : 0;
}

static int hwa_compare_write_input(FILE *stream,
                                   const char *role,
                                   const char *path,
                                   const char *sha256)
{
    return path == NULL || path[0] == '\0' ||
           !hwa_measure_valid_sha256(sha256) ||
           fprintf(stream, "INPUT,%s,", role) < 0 ||
           hwa_measure_path_hex(stream, path) != 0 ||
           fprintf(stream, ",%s\r\n", sha256) < 0 ? -1 : 0;
}

static int hwa_compare_write_distribution(
    FILE *stream,
    const HWAProfileDistribution *distribution)
{
    const char *kind = hwa_measure_kind_name(distribution->kind);
    const char *unit = hwa_measure_unit_name(distribution->unit);
    const char *view = hwa_measure_view_name(distribution->view);
    if (kind == NULL || unit == NULL || view == NULL ||
        distribution->id == 0U || distribution->group_id == 0U ||
        fprintf(stream,
                "DISTRIBUTION,%" PRIu64 ",%" PRIu64 ",%s,%" PRIu32
                ",%s,%s,%d,%d,",
                distribution->id, distribution->group_id, kind,
                distribution->index, unit, view,
                distribution->reference_valid ? 1 : 0,
                distribution->model_valid ? 1 : 0) < 0 ||
        hwa_measure_write_statistics_fields(
            stream, &distribution->reference_statistics) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_measure_write_statistics_fields(
            stream, &distribution->model_statistics) != 0 ||
        fputs("\r\n", stream) == EOF) {
        return -1;
    }
    return 0;
}

static int hwa_compare_write_gap(FILE *stream, const HWAProfileGap *gap)
{
    if (gap->id == 0U || gap->distribution_id == 0U ||
        fprintf(stream, "GAP,%" PRIu64 ",%" PRIu64 ",",
                gap->id, gap->distribution_id) < 0 ||
        hwa_measure_csv_optional_number(stream, gap->mean_delta,
                                        gap->mean_delta_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_measure_csv_optional_number(stream, gap->median_delta,
                                        gap->median_delta_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_measure_csv_optional_number(stream, gap->quantile_distance,
                                        gap->quantile_distance_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_measure_csv_optional_number(
            stream, gap->standardized_mean_shift,
            gap->standardized_mean_shift_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_measure_csv_optional_number(stream, gap->valid_coverage,
                                        gap->valid_coverage_valid) != 0 ||
        fputc(',', stream) == EOF ||
        hwa_measure_csv_optional_number(stream, gap->gap_score,
                                        gap->gap_score_valid) != 0 ||
        fprintf(stream,
                ",%zu,%" PRIu32 ",%d,%d,%d,%d,%d,%d\r\n",
                gap->rank, gap->quality_flags,
                gap->mean_delta_valid ? 1 : 0,
                gap->median_delta_valid ? 1 : 0,
                gap->quantile_distance_valid ? 1 : 0,
                gap->standardized_mean_shift_valid ? 1 : 0,
                gap->valid_coverage_valid ? 1 : 0,
                gap->gap_score_valid ? 1 : 0) < 0) {
        return -1;
    }
    return 0;
}

static int hwa_compare_write_warning(FILE *stream,
                                     const HWAProfileWarning *warning)
{
    return warning->id == 0U || warning->code == NULL ||
           warning->code[0] == '\0' || warning->message == NULL ||
           fprintf(stream, "WARNING,%" PRIu64 ",", warning->id) < 0 ||
           hwa_measure_csv_field(stream, warning->code) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_measure_csv_field(stream, warning->message) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_measure_csv_optional_u64(stream, warning->group_id,
                                        warning->group_id_valid) != 0 ||
           fputc(',', stream) == EOF ||
           hwa_measure_csv_optional_u64(stream, warning->distribution_id,
                                        warning->distribution_id_valid) != 0 ||
           fputs("\r\n", stream) == EOF ? -1 : 0;
}

int hwa_profile_comparison_file_write(
    FILE *stream,
    const HWAProfileComparisonSet *set,
    char *error,
    size_t error_size)
{
    size_t index;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (stream == NULL || set == NULL || set->reference_path == NULL ||
        set->model_path == NULL ||
        !hwa_measure_valid_sha256(set->reference_sha256) ||
        !hwa_measure_valid_sha256(set->model_sha256) ||
        (set->group_count != 0U && set->groups == NULL) ||
        (set->distribution_count != 0U && set->distributions == NULL) ||
        (set->gap_count != 0U && set->gaps == NULL) ||
        (set->warning_count != 0U && set->warnings == NULL) ||
        fputs("HWA_COMPARE,1\r\n", stream) == EOF ||
        hwa_compare_write_meta(stream, set) != 0 ||
        hwa_compare_write_input(stream, "reference", set->reference_path,
                                set->reference_sha256) != 0 ||
        hwa_compare_write_input(stream, "model", set->model_path,
                                set->model_sha256) != 0) {
        hwa_set_error(error, error_size,
                      "cannot write profile comparison metadata");
        return -1;
    }
    for (index = 0U; index < set->group_count; ++index) {
        if (set->groups[index].id != (uint64_t)index + 1U ||
            hwa_measure_write_group(stream, &set->groups[index], 0) != 0) {
            hwa_set_error(error, error_size,
                          "cannot write comparison group %zu", index);
            return -1;
        }
    }
    for (index = 0U; index < set->distribution_count; ++index) {
        if (set->distributions[index].id != (uint64_t)index + 1U ||
            hwa_compare_write_distribution(
                stream, &set->distributions[index]) != 0) {
            hwa_set_error(error, error_size,
                          "cannot write comparison distribution %zu", index);
            return -1;
        }
    }
    for (index = 0U; index < set->gap_count; ++index) {
        if (set->gaps[index].id != (uint64_t)index + 1U ||
            hwa_compare_write_gap(stream, &set->gaps[index]) != 0) {
            hwa_set_error(error, error_size,
                          "cannot write profile gap %zu", index);
            return -1;
        }
    }
    for (index = 0U; index < set->warning_count; ++index) {
        if (set->warnings[index].id != (uint64_t)index + 1U ||
            hwa_compare_write_warning(stream, &set->warnings[index]) != 0) {
            hwa_set_error(error, error_size,
                          "cannot write comparison warning %zu", index);
            return -1;
        }
    }
    return 0;
}

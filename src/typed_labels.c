#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "typed_labels.h"

#include "internal.h"
#include "sha256.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <sys/stat.h>
#include "windows_file_identity.h"
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define HWA_TYPED_LABEL_FIELD_COUNT 12U

typedef struct HWATypedLabelParser {
    HWATypedLabelSet result;
    uint64_t max_work_bytes;
    uint64_t work_bytes;
    size_t max_rows;
    size_t max_field_bytes;
    size_t capacity;
    size_t row_count;
} HWATypedLabelParser;

static const char *const hwa_typed_label_header_fields[] = {
    "event_id", "pitch", "register", "dynamic", "articulation", "part",
    "physical_element", "controller", "technique", "score_section",
    "transition", "gesture"
};

static void hwa_typed_label_values_free(HWATypedLabels *labels)
{
    if (labels == NULL) return;
    free(labels->gesture);
    free(labels->transition);
    free(labels->score_section);
    free(labels->technique);
    free(labels->controller);
    free(labels->physical_element);
    free(labels->part);
    free(labels->articulation);
    free(labels->dynamic);
    free(labels->register_name);
    free(labels->pitch);
    memset(labels, 0, sizeof(*labels));
}

void hwa_typed_labels_free(HWATypedLabelSet *labels)
{
    size_t index;

    if (labels == NULL) return;
    for (index = 0U; index < labels->row_count; ++index) {
        free(labels->rows[index].event_id);
        hwa_typed_label_values_free(&labels->rows[index].labels);
    }
    free(labels->rows);
    free(labels->path);
    memset(labels, 0, sizeof(*labels));
}

static int hwa_typed_labels_charge(HWATypedLabelParser *parser,
                                   uint64_t bytes,
                                   char *error,
                                   size_t error_size)
{
    if (parser->work_bytes > parser->max_work_bytes ||
        bytes > parser->max_work_bytes - parser->work_bytes) {
        hwa_set_error(error, error_size,
                      "typed labels exceed the current work-byte limit");
        return -1;
    }
    parser->work_bytes += bytes;
    return 0;
}

static int hwa_typed_labels_valid_utf8(const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;

    while (*cursor != 0U) {
        unsigned char first = *cursor++;
        uint32_t value;
        unsigned needed;
        unsigned index;

        if (first < 0x80U) continue;
        if (first >= 0xc2U && first <= 0xdfU) {
            value = (uint32_t)(first & 0x1fU);
            needed = 1U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            value = (uint32_t)(first & 0x0fU);
            needed = 2U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            value = (uint32_t)(first & 0x07U);
            needed = 3U;
        } else {
            return 0;
        }
        for (index = 0U; index < needed; ++index) {
            unsigned char next = cursor[index];
            if (next == 0U || (next & 0xc0U) != 0x80U) return 0;
            value = (value << 6U) | (uint32_t)(next & 0x3fU);
        }
        if ((needed == 2U && value < UINT32_C(0x800)) ||
            (needed == 3U && value < UINT32_C(0x10000)) ||
            value > UINT32_C(0x10ffff) ||
            (value >= UINT32_C(0xd800) && value <= UINT32_C(0xdfff))) {
            return 0;
        }
        cursor += needed;
    }
    return 1;
}

static char *hwa_typed_labels_copy(HWATypedLabelParser *parser,
                                   const char *text,
                                   int empty_is_null,
                                   char *error,
                                   size_t error_size)
{
    size_t length;
    char *copy;

    if (empty_is_null && text[0] == '\0') return NULL;
    length = strlen(text);
    if (length == SIZE_MAX ||
        hwa_typed_labels_charge(parser, (uint64_t)length + 1U,
                                error, error_size) != 0) {
        return NULL;
    }
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        hwa_set_error(error, error_size, "out of memory for typed labels");
        return NULL;
    }
    memcpy(copy, text, length + 1U);
    return copy;
}

static int hwa_typed_labels_reserve(HWATypedLabelParser *parser,
                                    char *error,
                                    size_t error_size)
{
    size_t next;
    size_t added;
    HWATypedLabelRow *grown;

    if (parser->result.row_count == parser->max_rows) {
        hwa_set_error(error, error_size,
                      "typed labels exceed the current row limit");
        return -1;
    }
    if (parser->result.row_count < parser->capacity) return 0;
    next = parser->capacity == 0U ? 16U : parser->capacity * 2U;
    if (next < parser->capacity || next > parser->max_rows) {
        next = parser->max_rows;
    }
    if (next <= parser->capacity ||
        next > SIZE_MAX / sizeof(*parser->result.rows)) {
        hwa_set_error(error, error_size, "typed-label row storage overflows");
        return -1;
    }
    added = next - parser->capacity;
    if (
#if SIZE_MAX > UINT64_MAX
        added > UINT64_MAX / (uint64_t)sizeof(*parser->result.rows) ||
#endif
        hwa_typed_labels_charge(
            parser,
            (uint64_t)added * (uint64_t)sizeof(*parser->result.rows),
            error, error_size) != 0) {
        return -1;
    }
    grown = (HWATypedLabelRow *)realloc(
        parser->result.rows, next * sizeof(*parser->result.rows));
    if (grown == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for typed-label rows");
        return -1;
    }
    memset(grown + parser->capacity, 0, added * sizeof(*grown));
    parser->result.rows = grown;
    parser->capacity = next;
    return 0;
}

static int hwa_typed_labels_store_row(HWATypedLabelParser *parser,
                                      char **fields,
                                      size_t physical_row,
                                      char *error,
                                      size_t error_size)
{
    HWATypedLabelRow *row;
    char **targets[11];
    uint32_t flags[11] = {
        HWA_LABEL_OVERRIDE_PITCH,
        HWA_LABEL_OVERRIDE_REGISTER,
        HWA_LABEL_OVERRIDE_DYNAMIC,
        HWA_LABEL_OVERRIDE_ARTICULATION,
        HWA_LABEL_OVERRIDE_PART,
        HWA_LABEL_OVERRIDE_PHYSICAL_ELEMENT,
        HWA_LABEL_OVERRIDE_CONTROLLER,
        HWA_LABEL_OVERRIDE_TECHNIQUE,
        HWA_LABEL_OVERRIDE_SCORE_SECTION,
        HWA_LABEL_OVERRIDE_TRANSITION,
        HWA_LABEL_OVERRIDE_GESTURE
    };
    size_t index;

    if (fields[0][0] == '\0') {
        hwa_set_error(error, error_size,
                      "typed-label row %zu has an empty event_id",
                      physical_row);
        return -1;
    }
    if (hwa_typed_labels_reserve(parser, error, error_size) != 0) return -1;
    row = &parser->result.rows[parser->result.row_count];
    row->event_id = hwa_typed_labels_copy(parser, fields[0], 0,
                                          error, error_size);
    if (row->event_id == NULL) return -1;
    targets[0] = &row->labels.pitch;
    targets[1] = &row->labels.register_name;
    targets[2] = &row->labels.dynamic;
    targets[3] = &row->labels.articulation;
    targets[4] = &row->labels.part;
    targets[5] = &row->labels.physical_element;
    targets[6] = &row->labels.controller;
    targets[7] = &row->labels.technique;
    targets[8] = &row->labels.score_section;
    targets[9] = &row->labels.transition;
    targets[10] = &row->labels.gesture;
    for (index = 0U; index < 11U; ++index) {
        if (fields[index + 1U][0] != '\0') {
            *targets[index] = hwa_typed_labels_copy(
                parser, fields[index + 1U], 0, error, error_size);
            if (*targets[index] == NULL) {
                free(row->event_id);
                row->event_id = NULL;
                hwa_typed_label_values_free(&row->labels);
                return -1;
            }
            row->labels.override_flags |= flags[index];
        }
    }
    parser->result.row_count++;
    return 0;
}

static int hwa_typed_label_row_compare(const void *left, const void *right)
{
    const HWATypedLabelRow *a = (const HWATypedLabelRow *)left;
    const HWATypedLabelRow *b = (const HWATypedLabelRow *)right;
    return strcmp(a->event_id, b->event_id);
}

static int hwa_typed_labels_parse(unsigned char *data,
                                  size_t size,
                                  HWATypedLabelParser *parser,
                                  char *error,
                                  size_t error_size)
{
    char *fields[HWA_TYPED_LABEL_FIELD_COUNT];
    size_t position = 0U;
    size_t output = 0U;
    size_t physical_row = 1U;
    int header = 1;

    while (position < size) {
        size_t field_count = 0U;
        size_t logical_row = physical_row;
        int row_done = 0;

        while (!row_done) {
            int quoted = 0;
            int closed_quote = 0;

            if (field_count == HWA_TYPED_LABEL_FIELD_COUNT) {
                hwa_set_error(error, error_size,
                              "typed-label row %zu has too many fields",
                              logical_row);
                return -1;
            }
            fields[field_count++] = (char *)data + output;
            if (position < size && data[position] == (unsigned char)'"') {
                quoted = 1;
                position++;
            }
            while (position < size) {
                unsigned char byte = data[position];
                if (byte == 0U) {
                    hwa_set_error(error, error_size,
                                  "typed-label row %zu contains a NUL byte",
                                  logical_row);
                    return -1;
                }
                if (quoted) {
                    if (byte == (unsigned char)'"') {
                        if (position + 1U < size &&
                            data[position + 1U] == (unsigned char)'"') {
                            data[output++] = (unsigned char)'"';
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
                                          "typed-label row %zu has a bare carriage return",
                                          logical_row);
                            return -1;
                        }
                        data[output++] = (unsigned char)'\r';
                        data[output++] = (unsigned char)'\n';
                        position += 2U;
                        physical_row++;
                        continue;
                    }
                    data[output++] = byte;
                    if (byte == (unsigned char)'\n') physical_row++;
                    position++;
                    continue;
                }
                if (byte == (unsigned char)'"') {
                    hwa_set_error(error, error_size,
                                  "typed-label row %zu has an unescaped quote",
                                  logical_row);
                    return -1;
                }
                if (byte == (unsigned char)',' ||
                    byte == (unsigned char)'\r' ||
                    byte == (unsigned char)'\n') {
                    break;
                }
                data[output++] = byte;
                position++;
            }
            if (quoted && !closed_quote) {
                hwa_set_error(error, error_size,
                              "typed-label row %zu has an unclosed quote",
                              logical_row);
                return -1;
            }
            {
                unsigned char delimiter =
                    position < size ? data[position] : 0U;
                int crlf = delimiter == (unsigned char)'\r' &&
                           position + 1U < size &&
                           data[position + 1U] == (unsigned char)'\n';

                data[output++] = 0U;
            if (position == size) {
                row_done = 1;
            } else if (delimiter == (unsigned char)',') {
                position++;
            } else if (delimiter == (unsigned char)'\n') {
                position++;
                physical_row++;
                row_done = 1;
            } else if (delimiter == (unsigned char)'\r') {
                if (!crlf) {
                    hwa_set_error(error, error_size,
                                  "typed-label row %zu has a bare carriage return",
                                  logical_row);
                    return -1;
                }
                position += 2U;
                physical_row++;
                row_done = 1;
            } else {
                hwa_set_error(error, error_size,
                              "typed-label row %zu has text after a quote",
                              logical_row);
                return -1;
            }
            }
        }
        if (field_count != HWA_TYPED_LABEL_FIELD_COUNT) {
            hwa_set_error(error, error_size,
                          "typed-label row %zu has the wrong field count",
                          logical_row);
            return -1;
        }
        {
            size_t field;
            for (field = 0U; field < field_count; ++field) {
                if (strlen(fields[field]) > parser->max_field_bytes ||
                    !hwa_typed_labels_valid_utf8(fields[field])) {
                    hwa_set_error(error, error_size,
                                  "typed-label row %zu has invalid or oversized text",
                                  logical_row);
                    return -1;
                }
            }
        }
        if (header) {
            size_t field;
            for (field = 0U; field < field_count; ++field) {
                if (strcmp(fields[field],
                           hwa_typed_label_header_fields[field]) != 0) {
                    hwa_set_error(error, error_size,
                                  "typed-label file has the wrong header");
                    return -1;
                }
            }
            header = 0;
        } else if (hwa_typed_labels_store_row(
                       parser, fields, logical_row,
                       error, error_size) != 0) {
            return -1;
        }
    }
    if (header) {
        hwa_set_error(error, error_size, "typed-label file is empty");
        return -1;
    }
    return 0;
}

static int hwa_typed_labels_read_regular(const char *path,
                                         uint64_t max_bytes,
                                         uint64_t max_work_bytes,
                                         unsigned char **data,
                                         size_t *size,
                                         char *error,
                                         size_t error_size)
{
    uint64_t source_size;
    FILE *stream;
    unsigned char *buffer;
#if defined(_WIN32)
    HWAWindowsFileIdentity before;
    HWAWindowsFileIdentity opened;
#else
    struct stat before;
    struct stat opened;
#endif

    *data = NULL;
    *size = 0U;
    if (path == NULL || strcmp(path, "-") == 0) {
        hwa_set_error(error, error_size,
                      "typed labels must be a named regular file");
        return -1;
    }
#if defined(_WIN32)
    if (hwa_windows_identity_from_path(path, &before) != 0) {
#else
    if (stat(path, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size < 0) {
#endif
        hwa_set_error(error, error_size,
                      "cannot inspect typed-label input '%s'", path);
        return -1;
    }
#if defined(_WIN32)
    source_size = before.size;
#else
    source_size = (uint64_t)before.st_size;
#endif
    if (source_size > max_bytes || source_size == UINT64_MAX ||
        source_size + 1U > max_work_bytes ||
        source_size > (uint64_t)(SIZE_MAX - 1U)) {
        hwa_set_error(error, error_size,
                      "typed-label input exceeds the current byte limit");
        return -1;
    }
    stream = fopen(path, "rb");
    if (stream == NULL) {
        hwa_set_error(error, error_size,
                      "cannot open typed-label input '%s': %s",
                      path, strerror(errno));
        return -1;
    }
#if defined(_WIN32)
    if (hwa_windows_identity_from_stream(stream, &opened) != 0 ||
        !hwa_windows_identity_equal(&before, &opened)) {
#else
    if (fstat(fileno(stream), &opened) != 0 ||
        opened.st_dev != before.st_dev || opened.st_ino != before.st_ino ||
        opened.st_size != before.st_size) {
#endif
        hwa_set_error(error, error_size,
                      "typed-label input changed before it was opened");
        (void)fclose(stream);
        return -1;
    }
    buffer = (unsigned char *)malloc((size_t)source_size + 1U);
    if (buffer == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for typed-label input");
        (void)fclose(stream);
        return -1;
    }
    {
        int read_failed = source_size != 0U &&
                          fread(buffer, 1U, (size_t)source_size, stream) !=
                              (size_t)source_size;
        int extra = read_failed ? EOF : fgetc(stream);
        int stream_error = ferror(stream);
        int close_failed = fclose(stream) != 0;

        if (read_failed || extra != EOF || stream_error || close_failed) {
        hwa_set_error(error, error_size,
                      "cannot read typed-label input '%s'", path);
        free(buffer);
        return -1;
        }
    }
    buffer[(size_t)source_size] = 0U;
    *data = buffer;
    *size = (size_t)source_size;
    return 0;
}

int hwa_typed_labels_load(const char *path,
                          uint64_t max_bytes,
                          uint64_t max_work_bytes,
                          size_t max_rows,
                          size_t max_field_bytes,
                          HWATypedLabelSet *labels,
                          char *error,
                          size_t error_size)
{
    HWATypedLabelParser parser;
    unsigned char *data = NULL;
    size_t size = 0U;
    unsigned char digest[32];
    HWASha256 sha;
    char after[HWA_SHA256_HEX_SIZE];
    size_t index;

    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (labels == NULL || max_bytes == 0U || max_work_bytes == 0U ||
        max_rows == 0U || max_field_bytes == 0U) {
        hwa_set_error(error, error_size, "invalid typed-label arguments");
        return -1;
    }
    memset(labels, 0, sizeof(*labels));
    memset(&parser, 0, sizeof(parser));
    parser.max_work_bytes = max_work_bytes;
    parser.max_rows = max_rows;
    parser.max_field_bytes = max_field_bytes;
    if (hwa_typed_labels_read_regular(path, max_bytes, max_work_bytes,
                                      &data, &size, error, error_size) != 0) {
        return -1;
    }
    parser.work_bytes = (uint64_t)size + 1U;
    hwa_sha256_init(&sha);
    hwa_sha256_update(&sha, data, size);
    hwa_sha256_final(&sha, digest);
    hwa_sha256_hex(digest, parser.result.sha256);
    if (hwa_typed_labels_parse(data, size, &parser,
                               error, error_size) != 0) {
        free(data);
        hwa_typed_labels_free(&parser.result);
        return -1;
    }
    free(data);
    if (parser.result.row_count > 1U) {
        qsort(parser.result.rows, parser.result.row_count,
              sizeof(*parser.result.rows), hwa_typed_label_row_compare);
        for (index = 1U; index < parser.result.row_count; ++index) {
            if (strcmp(parser.result.rows[index - 1U].event_id,
                       parser.result.rows[index].event_id) == 0) {
                hwa_set_error(error, error_size,
                              "typed-label event_id '%s' appears more than once",
                              parser.result.rows[index].event_id);
                hwa_typed_labels_free(&parser.result);
                return -1;
            }
        }
    }
    parser.result.path = hwa_typed_labels_copy(
        &parser, path, 0, error, error_size);
    if (parser.result.path == NULL ||
        hwa_sha256_file(path, max_bytes, after,
                        error, error_size) != 0 ||
        strcmp(after, parser.result.sha256) != 0) {
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "typed-label input changed while it was read");
        }
        hwa_typed_labels_free(&parser.result);
        return -1;
    }
    parser.result.retained_work_bytes =
        parser.work_bytes - ((uint64_t)size + 1U);
    *labels = parser.result;
    return 0;
}

const HWATypedLabelRow *hwa_typed_labels_find(
    const HWATypedLabelSet *labels,
    const char *event_id)
{
    size_t low = 0U;
    size_t high;

    if (labels == NULL || event_id == NULL) return NULL;
    high = labels->row_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        int order = strcmp(labels->rows[middle].event_id, event_id);
        if (order < 0) low = middle + 1U;
        else high = middle;
    }
    return low < labels->row_count &&
                   strcmp(labels->rows[low].event_id, event_id) == 0
               ? &labels->rows[low]
               : NULL;
}

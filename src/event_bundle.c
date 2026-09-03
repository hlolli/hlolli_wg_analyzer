#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "event_bundle.h"

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
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#define HWA_EVENT_MKDIR(path) _mkdir(path)
#define HWA_EVENT_RMDIR(path) _rmdir(path)
#define HWA_EVENT_UNLINK(path) _unlink(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define HWA_EVENT_MKDIR(path) mkdir(path, 0755)
#define HWA_EVENT_RMDIR(path) rmdir(path)
#define HWA_EVENT_UNLINK(path) unlink(path)
#endif

#define HWA_EVENT_JSON_SAFE_INTEGER UINT64_C(9007199254740991)
#define HWA_EVENT_PATH_LIMIT 4096U

typedef struct HWAEventJson {
    const unsigned char *data;
    size_t size;
    size_t offset;
    size_t tokens;
    size_t depth;
    const HWAEventBundleLimits *limits;
    const HWANumericLocale *locale;
    char *error;
    size_t error_size;
    const char *name;
} HWAEventJson;

typedef struct HWAEventManifestCounts {
    size_t providers;
    size_t audio;
    size_t events;
    size_t values;
    size_t traces;
    size_t trace_refs;
    size_t warnings;
} HWAEventManifestCounts;

typedef struct HWAEventIndexFile {
    char sha256[HWA_SHA256_HEX_SIZE];
    uint64_t bytes;
    int seen;
} HWAEventIndexFile;

typedef struct HWAEventManifestFiles {
    HWAEventIndexFile events;
    HWAEventIndexFile traces;
    struct HWAEventPayloadFile *payloads;
    size_t payload_count;
    size_t payload_capacity;
} HWAEventManifestFiles;

typedef struct HWAEventPayloadFile {
    char *relative_path;
    char sha256[HWA_SHA256_HEX_SIZE];
    uint64_t bytes;
} HWAEventPayloadFile;

static size_t hwa_event_utf8_size(const unsigned char *text,
                                  size_t remaining);

static int hwa_event_join(char path[HWA_EVENT_PATH_LIMIT],
                          const char *directory,
                          const char *name)
{
    int length;
    if (path == NULL || directory == NULL || name == NULL) return -1;
    length = snprintf(path, HWA_EVENT_PATH_LIMIT, "%s/%s", directory, name);
    return length < 0 || (size_t)length >= HWA_EVENT_PATH_LIMIT ? -1 : 0;
}

static int hwa_event_relative_path(const char *path, const char *prefix)
{
    const char *cursor;
    const unsigned char *utf8;
    size_t remaining;
    size_t path_size;
    size_t prefix_size;
    if (path == NULL || prefix == NULL || path[0] == '\0' ||
        path[0] == '/' || path[0] == '\\') return 0;
    path_size = strlen(path);
    if (path[path_size - 1U] == '/') return 0;
    prefix_size = strlen(prefix);
    if (strncmp(path, prefix, prefix_size) != 0 ||
        path[prefix_size] == '\0') return 0;
    utf8 = (const unsigned char *)path;
    remaining = path_size;
    while (remaining != 0U) {
        size_t count = hwa_event_utf8_size(utf8, remaining);
        if (count == 0U || utf8[0] == 0U) return 0;
        utf8 += count;
        remaining -= count;
    }
    cursor = path;
    while (*cursor != '\0') {
        const char *start = cursor;
        size_t size;
        while (*cursor != '\0' && *cursor != '/') {
            if (*cursor == '\\') return 0;
            cursor++;
        }
        size = (size_t)(cursor - start);
        if (size == 0U || (size == 1U && start[0] == '.') ||
            (size == 2U && start[0] == '.' && start[1] == '.')) return 0;
        if (*cursor == '/') cursor++;
    }
    return 1;
}

static char *hwa_event_copy(const char *text)
{
    size_t size;
    char *copy;
    if (text == NULL) return NULL;
    size = strlen(text);
    if (size == SIZE_MAX) return NULL;
    copy = (char *)malloc(size + 1U);
    if (copy != NULL) memcpy(copy, text, size + 1U);
    return copy;
}

static int hwa_event_array_reserve(void *items,
                                   size_t *capacity,
                                   size_t needed,
                                   size_t maximum,
                                   size_t item_size,
                                   void **result,
                                   char *error,
                                   size_t error_size,
                                   const char *message)
{
    size_t next_capacity;
    void *rows;
    if (needed > maximum) {
        hwa_set_error(error, error_size, "%s", message);
        return -1;
    }
    if (*capacity >= needed) {
        *result = items;
        return 0;
    }
    next_capacity = *capacity == 0U ? 8U : *capacity;
    if (next_capacity > maximum) next_capacity = maximum;
    while (next_capacity < needed) {
        if (next_capacity > maximum / 2U)
            next_capacity = maximum;
        else
            next_capacity *= 2U;
    }
    if (next_capacity == 0U || item_size > SIZE_MAX / next_capacity) {
        hwa_set_error(error, error_size, "%s", message);
        return -1;
    }
    rows = realloc(items, next_capacity * item_size);
    if (rows == NULL) {
        hwa_set_error(error, error_size, "%s", message);
        return -1;
    }
    *result = rows;
    *capacity = next_capacity;
    return 0;
}

static int hwa_event_array_shrink(void *items,
                                  size_t *capacity,
                                  size_t count,
                                  size_t item_size,
                                  void **result,
                                  char *error,
                                  size_t error_size,
                                  const char *message)
{
    void *rows;
    if (*capacity == count) {
        *result = items;
        return 0;
    }
    if (count == 0U) {
        free(items);
        *result = NULL;
        *capacity = 0U;
        return 0;
    }
    if (item_size > SIZE_MAX / count) {
        hwa_set_error(error, error_size, "%s", message);
        return -1;
    }
    rows = realloc(items, count * item_size);
    if (rows == NULL) {
        hwa_set_error(error, error_size, "%s", message);
        return -1;
    }
    *result = rows;
    *capacity = count;
    return 0;
}

static int hwa_event_hex_string(const char *text)
{
    size_t index;
    if (text == NULL) return 0;
    for (index = 0U; index < 64U; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (!((value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
              (value >= (unsigned char)'a' && value <= (unsigned char)'f'))) {
            return 0;
        }
    }
    return text[64] == '\0';
}

static int hwa_event_safe_id(uint64_t value)
{
    return value != 0U && value <= HWA_EVENT_JSON_SAFE_INTEGER;
}

static int hwa_event_value_name(const char *name)
{
    const unsigned char *cursor = (const unsigned char *)name;
    if (cursor == NULL || *cursor < 'a' || *cursor > 'z') return 0;
    cursor++;
    while (*cursor != 0U) {
        if (*cursor == '.') {
            cursor++;
            if (*cursor < 'a' || *cursor > 'z') return 0;
        } else if (!((*cursor >= 'a' && *cursor <= 'z') ||
                     (*cursor >= '0' && *cursor <= '9') ||
                     *cursor == '_' || *cursor == '-')) {
            return 0;
        }
        cursor++;
    }
    return 1;
}

static const char *hwa_event_audio_kind_name(HWAEventAudioKind kind)
{
    switch (kind) {
    case HWA_EVENT_SOURCE_RECORDING: return "source-recording";
    case HWA_EVENT_DERIVED_AUDIO: return "derived-audio";
    case HWA_EVENT_INSTRUMENT_STEM: return "instrument-stem";
    default: return NULL;
    }
}

static HWAEventAudioKind hwa_event_audio_kind_parse(const char *name)
{
    if (strcmp(name, "source-recording") == 0)
        return HWA_EVENT_SOURCE_RECORDING;
    if (strcmp(name, "derived-audio") == 0)
        return HWA_EVENT_DERIVED_AUDIO;
    if (strcmp(name, "instrument-stem") == 0)
        return HWA_EVENT_INSTRUMENT_STEM;
    return 0;
}

static const char *hwa_event_value_kind_name(HWAEventValueKind kind)
{
    switch (kind) {
    case HWA_EVENT_VALUE_TEXT: return "text";
    case HWA_EVENT_VALUE_F64: return "f64";
    case HWA_EVENT_VALUE_I64: return "i64";
    case HWA_EVENT_VALUE_BOOL: return "bool";
    default: return NULL;
    }
}

static HWAEventValueKind hwa_event_value_kind_parse(const char *name)
{
    if (strcmp(name, "text") == 0) return HWA_EVENT_VALUE_TEXT;
    if (strcmp(name, "f64") == 0) return HWA_EVENT_VALUE_F64;
    if (strcmp(name, "i64") == 0) return HWA_EVENT_VALUE_I64;
    if (strcmp(name, "bool") == 0) return HWA_EVENT_VALUE_BOOL;
    return 0;
}

static const char *hwa_event_value_basis_name(HWAEventValueBasis basis)
{
    switch (basis) {
    case HWA_EVENT_OBSERVATION: return "observation";
    case HWA_EVENT_INFERENCE: return "inference";
    case HWA_EVENT_SCORE_VALUE: return "score";
    default: return NULL;
    }
}

static HWAEventValueBasis hwa_event_value_basis_parse(const char *name)
{
    if (strcmp(name, "observation") == 0) return HWA_EVENT_OBSERVATION;
    if (strcmp(name, "inference") == 0) return HWA_EVENT_INFERENCE;
    if (strcmp(name, "score") == 0) return HWA_EVENT_SCORE_VALUE;
    return 0;
}

static const char *hwa_event_trace_format_name(HWAEventTraceFormat format)
{
    switch (format) {
    case HWA_EVENT_TRACE_CSV_F64: return "csv-f64";
    case HWA_EVENT_TRACE_F64LE: return "f64le";
    default: return NULL;
    }
}

static HWAEventTraceFormat hwa_event_trace_format_parse(const char *name)
{
    if (strcmp(name, "csv-f64") == 0) return HWA_EVENT_TRACE_CSV_F64;
    if (strcmp(name, "f64le") == 0) return HWA_EVENT_TRACE_F64LE;
    return 0;
}

static const char *hwa_event_container_name(HWAContainer container)
{
    switch (container) {
    case HWA_CONTAINER_RIFF: return "riff";
    case HWA_CONTAINER_RF64: return "rf64";
    default: return NULL;
    }
}

static HWAContainer hwa_event_container_parse(const char *name)
{
    if (strcmp(name, "riff") == 0) return HWA_CONTAINER_RIFF;
    if (strcmp(name, "rf64") == 0) return HWA_CONTAINER_RF64;
    return 0;
}

static const char *hwa_event_encoding_name(HWAEncoding encoding)
{
    switch (encoding) {
    case HWA_ENCODING_PCM: return "pcm";
    case HWA_ENCODING_IEEE_FLOAT: return "ieee-float";
    default: return NULL;
    }
}

static HWAEncoding hwa_event_encoding_parse(const char *name)
{
    if (strcmp(name, "pcm") == 0) return HWA_ENCODING_PCM;
    if (strcmp(name, "ieee-float") == 0) return HWA_ENCODING_IEEE_FLOAT;
    return 0;
}

static size_t hwa_event_utf8_size(const unsigned char *text, size_t remaining)
{
    unsigned char first;
    if (text == NULL || remaining == 0U) return 0U;
    first = text[0];
    if (first < 0x80U) return 1U;
    if (first >= 0xc2U && first <= 0xdfU) {
        return remaining >= 2U && text[1] >= 0x80U && text[1] <= 0xbfU
                   ? 2U : 0U;
    }
    if (first >= 0xe0U && first <= 0xefU) {
        if (remaining < 3U || text[2] < 0x80U || text[2] > 0xbfU)
            return 0U;
        if (first == 0xe0U)
            return text[1] >= 0xa0U && text[1] <= 0xbfU ? 3U : 0U;
        if (first == 0xedU)
            return text[1] >= 0x80U && text[1] <= 0x9fU ? 3U : 0U;
        return text[1] >= 0x80U && text[1] <= 0xbfU ? 3U : 0U;
    }
    if (first >= 0xf0U && first <= 0xf4U) {
        if (remaining < 4U || text[2] < 0x80U || text[2] > 0xbfU ||
            text[3] < 0x80U || text[3] > 0xbfU) return 0U;
        if (first == 0xf0U)
            return text[1] >= 0x90U && text[1] <= 0xbfU ? 4U : 0U;
        if (first == 0xf4U)
            return text[1] >= 0x80U && text[1] <= 0x8fU ? 4U : 0U;
        return text[1] >= 0x80U && text[1] <= 0xbfU ? 4U : 0U;
    }
    return 0U;
}

static int hwa_event_utf8_text(const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    size_t remaining;
    if (cursor == NULL) return 0;
    remaining = strlen(text);
    while (remaining != 0U) {
        size_t count = hwa_event_utf8_size(cursor, remaining);
        if (count == 0U) return 0;
        cursor += count;
        remaining -= count;
    }
    return 1;
}

static int hwa_event_json_write_string(FILE *stream, const char *text)
{
    const unsigned char *cursor;
    const unsigned char *end;
    if (stream == NULL || text == NULL || fputc('"', stream) == EOF) return -1;
    cursor = (const unsigned char *)text;
    end = cursor + strlen(text);
    while (*cursor != 0U) {
        unsigned char value = *cursor++;
        if (value == '"' || value == '\\') {
            if (fputc('\\', stream) == EOF || fputc((int)value, stream) == EOF)
                return -1;
        } else if (value == '\b') {
            if (fputs("\\b", stream) == EOF) return -1;
        } else if (value == '\f') {
            if (fputs("\\f", stream) == EOF) return -1;
        } else if (value == '\n') {
            if (fputs("\\n", stream) == EOF) return -1;
        } else if (value == '\r') {
            if (fputs("\\r", stream) == EOF) return -1;
        } else if (value == '\t') {
            if (fputs("\\t", stream) == EOF) return -1;
        } else if (value < 0x20U) {
            if (fprintf(stream, "\\u%04x", (unsigned)value) < 0) return -1;
        } else if (value >= 0x80U) {
            const unsigned char *start = cursor - 1U;
            size_t count = hwa_event_utf8_size(start,
                                               (size_t)(end - start));
            if (count == 0U || fwrite(start, 1U, count, stream) != count)
                return -1;
            cursor += count - 1U;
        } else if (fputc((int)value, stream) == EOF) {
            return -1;
        }
    }
    return fputc('"', stream) == EOF ? -1 : 0;
}

static int hwa_event_json_write_double(FILE *stream,
                                       const HWANumericLocale *locale,
                                       double value)
{
    char text[64];
    if (hwa_c_locale_format_double(locale, text, sizeof(text), value) != 0)
        return -1;
    return fputs(text, stream) == EOF ? -1 : 0;
}

static int hwa_event_json_fail(HWAEventJson *json, const char *message)
{
    hwa_set_error(json->error, json->error_size,
                  "invalid %s at byte %zu: %s",
                  json->name, json->offset, message);
    return -1;
}

static void hwa_event_json_space(HWAEventJson *json)
{
    while (json->offset < json->size) {
        unsigned char value = json->data[json->offset];
        if (value != ' ' && value != '\t' && value != '\r' && value != '\n')
            break;
        json->offset++;
    }
}

static int hwa_event_json_token(HWAEventJson *json)
{
    if (json->tokens >= json->limits->max_json_tokens)
        return hwa_event_json_fail(json, "token limit exceeded");
    json->tokens++;
    return 0;
}

static int hwa_event_json_take(HWAEventJson *json, unsigned char wanted)
{
    hwa_event_json_space(json);
    if (json->offset >= json->size || json->data[json->offset] != wanted)
        return hwa_event_json_fail(json, "unexpected token");
    json->offset++;
    return 0;
}

static int hwa_event_json_peek(HWAEventJson *json, unsigned char value)
{
    hwa_event_json_space(json);
    return json->offset < json->size && json->data[json->offset] == value;
}

static int hwa_event_json_depth_begin(HWAEventJson *json)
{
    if (json->depth >= json->limits->max_json_depth)
        return hwa_event_json_fail(json, "nesting limit exceeded");
    json->depth++;
    return 0;
}

static void hwa_event_json_depth_end(HWAEventJson *json)
{
    if (json->depth != 0U) json->depth--;
}

static int hwa_event_json_hex(unsigned char value)
{
    if (value >= '0' && value <= '9') return (int)(value - '0');
    if (value >= 'a' && value <= 'f') return 10 + (int)(value - 'a');
    if (value >= 'A' && value <= 'F') return 10 + (int)(value - 'A');
    return -1;
}

static int hwa_event_json_string(HWAEventJson *json, char **result)
{
    size_t scan;
    size_t capacity;
    size_t write = 0U;
    char *text;
    if (result == NULL) return hwa_event_json_fail(json, "missing string target");
    *result = NULL;
    hwa_event_json_space(json);
    if (hwa_event_json_token(json) != 0 ||
        json->offset >= json->size || json->data[json->offset] != '"')
        return hwa_event_json_fail(json, "string expected");
    scan = ++json->offset;
    while (scan < json->size && json->data[scan] != '"') {
        unsigned char value = json->data[scan++];
        if (value < 0x20U) return hwa_event_json_fail(json, "control byte in string");
        if (value >= 0x80U) {
            size_t count = hwa_event_utf8_size(json->data + scan - 1U,
                                               json->size - scan + 1U);
            if (count == 0U)
                return hwa_event_json_fail(json, "invalid UTF-8 in string");
            scan += count - 1U;
            continue;
        }
        if (value == '\\') {
            if (scan >= json->size) return hwa_event_json_fail(json, "bad escape");
            value = json->data[scan++];
            if (value == 'u') {
                if (scan + 4U > json->size ||
                    hwa_event_json_hex(json->data[scan]) < 0 ||
                    hwa_event_json_hex(json->data[scan + 1U]) < 0 ||
                    hwa_event_json_hex(json->data[scan + 2U]) < 0 ||
                    hwa_event_json_hex(json->data[scan + 3U]) < 0)
                    return hwa_event_json_fail(json, "bad Unicode escape");
                scan += 4U;
            } else if (strchr("\"\\/bfnrt", (int)value) == NULL) {
                return hwa_event_json_fail(json, "bad escape");
            }
        }
    }
    if (scan >= json->size) return hwa_event_json_fail(json, "unterminated string");
    capacity = scan - json->offset;
    if ((uint64_t)capacity + UINT64_C(1) > json->limits->max_work_bytes)
        return hwa_event_json_fail(json, "string exceeds work limit");
    text = (char *)malloc(capacity + 1U);
    if (text == NULL) return hwa_event_json_fail(json, "cannot allocate string");
    while (json->offset < scan) {
        unsigned char value = json->data[json->offset++];
        if (value == '\\') {
            unsigned char escaped = json->data[json->offset++];
            if (escaped == 'u') {
                int a = hwa_event_json_hex(json->data[json->offset]);
                int b = hwa_event_json_hex(json->data[json->offset + 1U]);
                int c = hwa_event_json_hex(json->data[json->offset + 2U]);
                int d = hwa_event_json_hex(json->data[json->offset + 3U]);
                unsigned code = ((unsigned)a << 12U) | ((unsigned)b << 8U) |
                                ((unsigned)c << 4U) | (unsigned)d;
                json->offset += 4U;
                if (code >= 0xd800U && code <= 0xdbffU) {
                    unsigned low;
                    if (json->offset + 6U > scan ||
                        json->data[json->offset] != '\\' ||
                        json->data[json->offset + 1U] != 'u') {
                        free(text);
                        return hwa_event_json_fail(json,
                                                   "bad Unicode surrogate");
                    }
                    a = hwa_event_json_hex(json->data[json->offset + 2U]);
                    b = hwa_event_json_hex(json->data[json->offset + 3U]);
                    c = hwa_event_json_hex(json->data[json->offset + 4U]);
                    d = hwa_event_json_hex(json->data[json->offset + 5U]);
                    if (a < 0 || b < 0 || c < 0 || d < 0) {
                        free(text);
                        return hwa_event_json_fail(json,
                                                   "bad Unicode surrogate");
                    }
                    low = ((unsigned)a << 12U) | ((unsigned)b << 8U) |
                          ((unsigned)c << 4U) | (unsigned)d;
                    if (low < 0xdc00U || low > 0xdfffU) {
                        free(text);
                        return hwa_event_json_fail(json,
                                                   "bad Unicode surrogate");
                    }
                    code = 0x10000U + ((code - 0xd800U) << 10U) +
                           (low - 0xdc00U);
                    json->offset += 6U;
                } else if (code >= 0xdc00U && code <= 0xdfffU) {
                    free(text);
                    return hwa_event_json_fail(
                        json, "bad Unicode surrogate");
                }
                if (code == 0U) {
                    free(text);
                    return hwa_event_json_fail(json,
                                               "null character in string");
                }
                if (code <= 0x7fU) {
                    text[write++] = (char)code;
                } else if (code <= 0x7ffU) {
                    text[write++] = (char)(0xc0U | (code >> 6U));
                    text[write++] = (char)(0x80U | (code & 0x3fU));
                } else if (code <= 0xffffU) {
                    text[write++] = (char)(0xe0U | (code >> 12U));
                    text[write++] =
                        (char)(0x80U | ((code >> 6U) & 0x3fU));
                    text[write++] = (char)(0x80U | (code & 0x3fU));
                } else {
                    text[write++] = (char)(0xf0U | (code >> 18U));
                    text[write++] =
                        (char)(0x80U | ((code >> 12U) & 0x3fU));
                    text[write++] =
                        (char)(0x80U | ((code >> 6U) & 0x3fU));
                    text[write++] = (char)(0x80U | (code & 0x3fU));
                }
                continue;
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
    *result = text;
    return 0;
}

static int hwa_event_json_number_text(HWAEventJson *json,
                                      char text[128],
                                      int integer_only)
{
    size_t start;
    size_t length;
    hwa_event_json_space(json);
    if (hwa_event_json_token(json) != 0) return -1;
    start = json->offset;
    if (json->offset < json->size && json->data[json->offset] == '-')
        json->offset++;
    if (json->offset >= json->size) return hwa_event_json_fail(json, "number expected");
    if (json->data[json->offset] == '0') {
        json->offset++;
        if (json->offset < json->size &&
            json->data[json->offset] >= '0' &&
            json->data[json->offset] <= '9')
            return hwa_event_json_fail(json, "leading zero in number");
    } else {
        if (json->data[json->offset] < '1' || json->data[json->offset] > '9')
            return hwa_event_json_fail(json, "number expected");
        while (json->offset < json->size &&
               json->data[json->offset] >= '0' &&
               json->data[json->offset] <= '9') json->offset++;
    }
    if (!integer_only && json->offset < json->size &&
        json->data[json->offset] == '.') {
        json->offset++;
        if (json->offset >= json->size || json->data[json->offset] < '0' ||
            json->data[json->offset] > '9')
            return hwa_event_json_fail(json, "fraction digit expected");
        while (json->offset < json->size &&
               json->data[json->offset] >= '0' &&
               json->data[json->offset] <= '9') json->offset++;
    }
    if (!integer_only && json->offset < json->size &&
        (json->data[json->offset] == 'e' ||
         json->data[json->offset] == 'E')) {
        json->offset++;
        if (json->offset < json->size &&
            (json->data[json->offset] == '+' ||
             json->data[json->offset] == '-')) json->offset++;
        if (json->offset >= json->size || json->data[json->offset] < '0' ||
            json->data[json->offset] > '9')
            return hwa_event_json_fail(json, "exponent digit expected");
        while (json->offset < json->size &&
               json->data[json->offset] >= '0' &&
               json->data[json->offset] <= '9') json->offset++;
    }
    length = json->offset - start;
    if (length == 0U || length >= 128U)
        return hwa_event_json_fail(json, "number is too long");
    memcpy(text, json->data + start, length);
    text[length] = '\0';
    return 0;
}

static int hwa_event_json_u64(HWAEventJson *json, uint64_t *result)
{
    char text[128];
    uint64_t value = 0U;
    size_t index;
    if (result == NULL || hwa_event_json_number_text(json, text, 1) != 0)
        return -1;
    if (text[0] == '-') return hwa_event_json_fail(json, "unsigned integer expected");
    for (index = 0U; text[index] != '\0'; ++index) {
        unsigned digit = (unsigned)(text[index] - '0');
        if (value > (UINT64_MAX - (uint64_t)digit) / UINT64_C(10))
            return hwa_event_json_fail(json, "integer overflow");
        value = value * UINT64_C(10) + (uint64_t)digit;
    }
    if (value > HWA_EVENT_JSON_SAFE_INTEGER)
        return hwa_event_json_fail(json, "integer exceeds exact JSON range");
    *result = value;
    return 0;
}

static int hwa_event_json_i64(HWAEventJson *json, int64_t *result)
{
    char text[128];
    char *end = NULL;
    long long value;
    if (result == NULL || hwa_event_json_number_text(json, text, 1) != 0)
        return -1;
    errno = 0;
    value = strtoll(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        value < -INT64_C(9007199254740991) ||
        value > INT64_C(9007199254740991))
        return hwa_event_json_fail(json, "integer exceeds exact JSON range");
    *result = (int64_t)value;
    return 0;
}

static int hwa_event_json_double(HWAEventJson *json, double *result)
{
    char text[128];
    if (result == NULL || hwa_event_json_number_text(json, text, 0) != 0)
        return -1;
    if (hwa_c_locale_parse_double(json->locale, text, result) != 0)
        return hwa_event_json_fail(json, "finite number expected");
    return 0;
}

static int hwa_event_json_bool(HWAEventJson *json, int *result)
{
    hwa_event_json_space(json);
    if (hwa_event_json_token(json) != 0) return -1;
    if (json->offset + 4U <= json->size &&
        memcmp(json->data + json->offset, "true", 4U) == 0) {
        json->offset += 4U;
        *result = 1;
        return 0;
    }
    if (json->offset + 5U <= json->size &&
        memcmp(json->data + json->offset, "false", 5U) == 0) {
        json->offset += 5U;
        *result = 0;
        return 0;
    }
    return hwa_event_json_fail(json, "boolean expected");
}

static int hwa_event_json_nullable_u64(HWAEventJson *json,
                                       uint64_t *result,
                                       int *valid)
{
    hwa_event_json_space(json);
    if (json->offset + 4U <= json->size &&
        memcmp(json->data + json->offset, "null", 4U) == 0) {
        if (hwa_event_json_token(json) != 0) return -1;
        json->offset += 4U;
        *result = 0U;
        *valid = 0;
        return 0;
    }
    if (hwa_event_json_u64(json, result) != 0) return -1;
    *valid = 1;
    return 0;
}

static int hwa_event_json_key(HWAEventJson *json, char **key, int *first);

static int hwa_event_json_nullable_double(HWAEventJson *json,
                                          double *result,
                                          int *valid)
{
    hwa_event_json_space(json);
    if (json->offset + 4U <= json->size &&
        memcmp(json->data + json->offset, "null", 4U) == 0) {
        if (hwa_event_json_token(json) != 0) return -1;
        json->offset += 4U;
        *result = 0.0;
        *valid = 0;
        return 0;
    }
    if (hwa_event_json_double(json, result) != 0) return -1;
    *valid = 1;
    return 0;
}

static int hwa_event_json_skip_value(HWAEventJson *json)
{
    hwa_event_json_space(json);
    if (json->offset >= json->size)
        return hwa_event_json_fail(json, "JSON value expected");
    if (json->data[json->offset] == '"') {
        char *text = NULL;
        int result = hwa_event_json_string(json, &text);
        free(text);
        return result;
    }
    if (json->data[json->offset] == '{') {
        int first = 1;
        if (hwa_event_json_take(json, '{') != 0 ||
            hwa_event_json_depth_begin(json) != 0) return -1;
        while (!hwa_event_json_peek(json, '}')) {
            char *key = NULL;
            int state = hwa_event_json_key(json, &key, &first);
            free(key);
            if (state != 0 || hwa_event_json_skip_value(json) != 0)
                return -1;
        }
        if (hwa_event_json_take(json, '}') != 0) return -1;
        hwa_event_json_depth_end(json);
        return 0;
    }
    if (json->data[json->offset] == '[') {
        int first = 1;
        if (hwa_event_json_take(json, '[') != 0 ||
            hwa_event_json_depth_begin(json) != 0) return -1;
        while (!hwa_event_json_peek(json, ']')) {
            if (!first && hwa_event_json_take(json, ',') != 0) return -1;
            first = 0;
            if (hwa_event_json_skip_value(json) != 0) return -1;
        }
        if (hwa_event_json_take(json, ']') != 0) return -1;
        hwa_event_json_depth_end(json);
        return 0;
    }
    if (json->offset + 4U <= json->size &&
        memcmp(json->data + json->offset, "null", 4U) == 0) {
        if (hwa_event_json_token(json) != 0) return -1;
        json->offset += 4U;
        return 0;
    }
    if ((json->offset + 4U <= json->size &&
         memcmp(json->data + json->offset, "true", 4U) == 0) ||
        (json->offset + 5U <= json->size &&
         memcmp(json->data + json->offset, "false", 5U) == 0)) {
        int value;
        return hwa_event_json_bool(json, &value);
    }
    {
        double value;
        return hwa_event_json_double(json, &value);
    }
}

static int hwa_event_json_raw_value(HWAEventJson *json, char **result)
{
    size_t start;
    size_t length;
    char *copy;
    if (result == NULL) return hwa_event_json_fail(json, "missing value target");
    *result = NULL;
    hwa_event_json_space(json);
    start = json->offset;
    if (hwa_event_json_skip_value(json) != 0) return -1;
    length = json->offset - start;
    if ((uint64_t)length + 1U > json->limits->max_work_bytes)
        return hwa_event_json_fail(json, "object exceeds work limit");
    copy = (char *)malloc(length + 1U);
    if (copy == NULL)
        return hwa_event_json_fail(json, "cannot allocate object");
    memcpy(copy, json->data + start, length);
    copy[length] = '\0';
    *result = copy;
    return 0;
}

static int hwa_event_json_raw_object(HWAEventJson *json, char **result)
{
    hwa_event_json_space(json);
    if (json->offset >= json->size || json->data[json->offset] != '{')
        return hwa_event_json_fail(json, "object expected");
    return hwa_event_json_raw_value(json, result);
}

static int hwa_event_validate_settings_json(
    const char *settings_json,
    const HWAEventBundleLimits *limits,
    const HWANumericLocale *locale,
    char *error,
    size_t error_size)
{
    HWAEventJson json;
    char *copy = NULL;
    int result = -1;
    if (settings_json == NULL || strchr(settings_json, '\r') != NULL) {
        hwa_set_error(error, error_size,
                      "provider settings must use line-feed line endings");
        return -1;
    }
    memset(&json, 0, sizeof(json));
    json.data = (const unsigned char *)settings_json;
    json.size = strlen(settings_json);
    json.limits = limits;
    json.locale = locale;
    json.error = error;
    json.error_size = error_size;
    json.name = "provider settings";
    if (hwa_event_json_raw_object(&json, &copy) != 0) goto cleanup;
    hwa_event_json_space(&json);
    if (json.offset != json.size) {
        hwa_event_json_fail(&json, "text follows settings object");
        goto cleanup;
    }
    result = 0;
cleanup:
    free(copy);
    return result;
}

static int hwa_event_json_key(HWAEventJson *json, char **key, int *first)
{
    hwa_event_json_space(json);
    if (hwa_event_json_peek(json, '}')) return 1;
    if (!*first && hwa_event_json_take(json, ',') != 0) return -1;
    *first = 0;
    if (hwa_event_json_string(json, key) != 0 ||
        hwa_event_json_take(json, ':') != 0) {
        free(*key);
        *key = NULL;
        return -1;
    }
    return 0;
}

static int hwa_event_read_file(const char *path,
                               uint64_t maximum,
                               unsigned char **data,
                               size_t *size,
                               char *error,
                               size_t error_size)
{
    FILE *stream;
    long length;
    unsigned char *bytes;
    if (data == NULL || size == NULL) return -1;
    *data = NULL;
    *size = 0U;
    stream = fopen(path, "rb");
    if (stream == NULL) {
        hwa_set_error(error, error_size, "cannot open event bundle member");
        return -1;
    }
    if (fseek(stream, 0L, SEEK_END) != 0 || (length = ftell(stream)) < 0L ||
        fseek(stream, 0L, SEEK_SET) != 0) {
        (void)fclose(stream);
        hwa_set_error(error, error_size, "cannot size event bundle member");
        return -1;
    }
    if ((uint64_t)length > maximum || (uintmax_t)length > SIZE_MAX - 1U) {
        (void)fclose(stream);
        hwa_set_error(error, error_size, "event bundle member exceeds limit");
        return -1;
    }
    bytes = (unsigned char *)malloc((size_t)length + 1U);
    if (bytes == NULL) {
        (void)fclose(stream);
        hwa_set_error(error, error_size, "cannot allocate event bundle member");
        return -1;
    }
    if ((size_t)length != 0U &&
        fread(bytes, 1U, (size_t)length, stream) != (size_t)length) {
        free(bytes);
        (void)fclose(stream);
        hwa_set_error(error, error_size, "cannot read event bundle member");
        return -1;
    }
    if (fclose(stream) != 0) {
        free(bytes);
        hwa_set_error(error, error_size, "cannot close event bundle member");
        return -1;
    }
    bytes[(size_t)length] = 0U;
    *data = bytes;
    *size = (size_t)length;
    return 0;
}

static int hwa_event_file_size(const char *path, uint64_t *size)
{
#if defined(_WIN32)
    struct _stat64 status;
    if (_stat64(path, &status) != 0 || status.st_size < 0 ||
        (status.st_mode & _S_IFMT) != _S_IFREG) return -1;
#else
    struct stat status;
    if (lstat(path, &status) != 0 || status.st_size < 0 ||
        !S_ISREG(status.st_mode)) return -1;
#endif
    *size = (uint64_t)status.st_size;
    return 0;
}

static int hwa_event_copy_file(const char *source_path,
                               const char *target_path,
                               uint64_t maximum,
                               uint64_t *written,
                               char *error,
                               size_t error_size)
{
    unsigned char buffer[65536];
    FILE *source = NULL;
    FILE *target = NULL;
    uint64_t total = 0U;
    int result = -1;
    source = fopen(source_path, "rb");
    if (source == NULL) {
        hwa_set_error(error, error_size, "cannot open event payload binding");
        return -1;
    }
    target = fopen(target_path, "wb");
    if (target == NULL) {
        (void)fclose(source);
        hwa_set_error(error, error_size, "cannot create event payload");
        return -1;
    }
    for (;;) {
        size_t count = fread(buffer, 1U, sizeof(buffer), source);
        if (count != 0U) {
            if (total > maximum ||
                (uint64_t)count > maximum - total ||
                fwrite(buffer, 1U, count, target) != count) {
                hwa_set_error(error, error_size,
                              "event payload exceeds limit or cannot be written");
                goto cleanup;
            }
            total += (uint64_t)count;
        }
        if (count < sizeof(buffer)) {
            if (ferror(source)) {
                hwa_set_error(error, error_size, "cannot read event payload");
                goto cleanup;
            }
            break;
        }
    }
    if (fflush(target) != 0) {
        hwa_set_error(error, error_size, "cannot flush event payload");
        goto cleanup;
    }
    result = 0;
cleanup:
    if (fclose(source) != 0 && result == 0) result = -1;
    if (fclose(target) != 0 && result == 0) result = -1;
    if (result != 0) {
        (void)HWA_EVENT_UNLINK(target_path);
        return -1;
    }
    *written = total;
    return 0;
}

static int hwa_event_copy_source(const HWAByteSource *source,
                                 const char *target_path,
                                 uint64_t maximum,
                                 uint64_t *written,
                                 char *error,
                                 size_t error_size)
{
    unsigned char buffer[65536];
    FILE *target = NULL;
    uint64_t offset = 0U;
    int result = -1;
    if (source == NULL || source->name == NULL || source->name[0] == '\0' ||
        source->read_at == NULL || source->size > maximum) {
        hwa_set_error(error, error_size,
                      "invalid or oversized event byte-source binding");
        return -1;
    }
    target = fopen(target_path, "wb");
    if (target == NULL) {
        hwa_set_error(error, error_size, "cannot create event payload");
        return -1;
    }
    while (offset < source->size) {
        uint64_t remaining = source->size - offset;
        size_t count = remaining > (uint64_t)sizeof(buffer)
                           ? sizeof(buffer)
                           : (size_t)remaining;
        if (source->read_at(source->context, offset, buffer, count) != 0) {
            hwa_set_error(error, error_size,
                          "cannot read event byte-source binding");
            goto cleanup;
        }
        if (fwrite(buffer, 1U, count, target) != count) {
            hwa_set_error(error, error_size, "cannot write event payload");
            goto cleanup;
        }
        offset += (uint64_t)count;
    }
    if (fflush(target) != 0) {
        hwa_set_error(error, error_size, "cannot flush event payload");
        goto cleanup;
    }
    result = 0;
cleanup:
    if (fclose(target) != 0 && result == 0) {
        hwa_set_error(error, error_size, "cannot close event payload");
        result = -1;
    }
    if (result != 0) {
        (void)HWA_EVENT_UNLINK(target_path);
        return -1;
    }
    *written = offset;
    return 0;
}

static void hwa_event_manifest_files_free(HWAEventManifestFiles *files)
{
    size_t index;
    if (files == NULL) return;
    for (index = 0U; index < files->payload_count; ++index)
        free(files->payloads[index].relative_path);
    free(files->payloads);
    memset(files, 0, sizeof(*files));
}

static int hwa_event_append_payload_file(HWAEventManifestFiles *files,
                                         const char *relative_path,
                                         uint64_t bytes,
                                         const char *sha256,
                                         char *error,
                                         size_t error_size)
{
    void *storage;
    size_t next;
    char *path_copy;
    if (files->payload_count == SIZE_MAX) {
        hwa_set_error(error, error_size,
                      "event payload inventory count overflow");
        return -1;
    }
    path_copy = hwa_event_copy(relative_path);
    if (path_copy == NULL) {
        hwa_set_error(error, error_size,
                      "cannot allocate event payload inventory path");
        return -1;
    }
    next = files->payload_count + 1U;
    if (hwa_event_array_reserve(
            files->payloads, &files->payload_capacity, next, SIZE_MAX,
            sizeof(*files->payloads), &storage, error, error_size,
            "cannot allocate event payload inventory") != 0) {
        free(path_copy);
        return -1;
    }
    files->payloads = (HWAEventPayloadFile *)storage;
    files->payloads[files->payload_count].relative_path = path_copy;
    files->payloads[files->payload_count].bytes = bytes;
    memcpy(files->payloads[files->payload_count].sha256, sha256,
           HWA_SHA256_HEX_SIZE);
    files->payload_count = next;
    return 0;
}

static int hwa_event_payload_file_compare(const void *left, const void *right)
{
    const HWAEventPayloadFile *left_file =
        (const HWAEventPayloadFile *)left;
    const HWAEventPayloadFile *right_file =
        (const HWAEventPayloadFile *)right;
    return strcmp(left_file->relative_path, right_file->relative_path);
}

static int hwa_event_finalize_payload_files(HWAEventManifestFiles *files,
                                            char *error,
                                            size_t error_size)
{
    void *storage;
    size_t index;
    if (files->payload_count > 1U)
        qsort(files->payloads, files->payload_count, sizeof(*files->payloads),
              hwa_event_payload_file_compare);
    for (index = 1U; index < files->payload_count; ++index) {
        if (strcmp(files->payloads[index - 1U].relative_path,
                   files->payloads[index].relative_path) == 0) {
            hwa_set_error(error, error_size,
                          "duplicate event payload inventory path");
            return -1;
        }
    }
    if (hwa_event_array_shrink(
            files->payloads, &files->payload_capacity, files->payload_count,
            sizeof(*files->payloads), &storage, error, error_size,
            "cannot finalize event payload inventory") != 0)
        return -1;
    files->payloads = (HWAEventPayloadFile *)storage;
    return 0;
}

static int hwa_event_make_payload_parent(const char *directory,
                                         const char *relative_path,
                                         char target[HWA_EVENT_PATH_LIMIT],
                                         char *error,
                                         size_t error_size)
{
    size_t root_size;
    char *cursor;
    if (hwa_event_join(target, directory, relative_path) != 0) {
        hwa_set_error(error, error_size, "event payload path is too long");
        return -1;
    }
    root_size = strlen(directory);
    cursor = target + root_size + 1U;
    while ((cursor = strchr(cursor, '/')) != NULL) {
        *cursor = '\0';
        if (HWA_EVENT_MKDIR(target) != 0 && errno != EEXIST) {
            *cursor = '/';
            hwa_set_error(error, error_size,
                          "cannot create event payload directory");
            return -1;
        }
#if defined(_WIN32)
        {
            struct _stat64 status;
            if (_stat64(target, &status) != 0 ||
                (status.st_mode & _S_IFMT) != _S_IFDIR) {
                *cursor = '/';
                hwa_set_error(error, error_size,
                              "event payload parent is not a directory");
                return -1;
            }
        }
#else
        {
            struct stat status;
            if (lstat(target, &status) != 0 || !S_ISDIR(status.st_mode)) {
                *cursor = '/';
                hwa_set_error(error, error_size,
                              "event payload parent is not a plain directory");
                return -1;
            }
        }
#endif
        *cursor = '/';
        cursor++;
    }
    return 0;
}

static int hwa_event_check_payload_parent(const char *directory,
                                          const char *relative_path,
                                          char target[HWA_EVENT_PATH_LIMIT],
                                          char *error,
                                          size_t error_size)
{
    size_t root_size;
    char *cursor;
    if (hwa_event_join(target, directory, relative_path) != 0) {
        hwa_set_error(error, error_size, "event payload path is too long");
        return -1;
    }
    root_size = strlen(directory);
    cursor = target + root_size + 1U;
    while ((cursor = strchr(cursor, '/')) != NULL) {
        *cursor = '\0';
#if defined(_WIN32)
        {
            struct _stat64 status;
            if (_stat64(target, &status) != 0 ||
                (status.st_mode & _S_IFMT) != _S_IFDIR) {
                *cursor = '/';
                hwa_set_error(error, error_size,
                              "event payload parent is not a directory");
                return -1;
            }
        }
#else
        {
            struct stat status;
            if (lstat(target, &status) != 0 || !S_ISDIR(status.st_mode)) {
                *cursor = '/';
                hwa_set_error(error, error_size,
                              "event payload parent is not a plain directory");
                return -1;
            }
        }
#endif
        *cursor = '/';
        cursor++;
    }
    return 0;
}

static void hwa_event_remove_payload_parent(const char *directory,
                                            const char *relative_path)
{
    char path[HWA_EVENT_PATH_LIMIT];
    size_t root_size;
    char *slash;
    if (hwa_event_join(path, directory, relative_path) != 0) return;
    root_size = strlen(directory);
    slash = strrchr(path, '/');
    while (slash != NULL && (size_t)(slash - path) > root_size) {
        *slash = '\0';
        (void)HWA_EVENT_RMDIR(path);
        slash = strrchr(path, '/');
    }
}

static int hwa_event_validate_trace_payload(
    const char *path,
    const HWAEventTrace *trace,
    const HWANumericLocale *locale,
    char *error,
    size_t error_size);

typedef struct HWAEventBindingIndexRow {
    const char *path;
    size_t row;
} HWAEventBindingIndexRow;

static int hwa_event_binding_index_compare(const void *left,
                                           const void *right)
{
    const HWAEventBindingIndexRow *left_row =
        (const HWAEventBindingIndexRow *)left;
    const HWAEventBindingIndexRow *right_row =
        (const HWAEventBindingIndexRow *)right;
    return strcmp(left_row->path, right_row->path);
}

static int hwa_event_binding_index_find(
    const HWAEventBindingIndexRow *rows,
    size_t count,
    const char *path,
    size_t *result)
{
    size_t first = 0U;
    size_t last = count;
    while (first < last) {
        size_t middle = first + (last - first) / 2U;
        int order = strcmp(rows[middle].path, path);
        if (order < 0)
            first = middle + 1U;
        else
            last = middle;
    }
    if (first == count || strcmp(rows[first].path, path) != 0) return 0;
    *result = rows[first].row;
    return 1;
}

static int hwa_event_copy_bound_payload(
    const char *directory,
    const char *relative_path,
    uint64_t expected_bytes,
    const char *expected_sha256,
    const HWAEventFileBinding *bindings,
    const HWAEventBindingIndexRow *binding_index,
    size_t binding_count,
    unsigned char *used,
    const HWAEventBundleLimits *limits,
    HWAEventManifestFiles *files,
    char *error,
    size_t error_size)
{
    char target[HWA_EVENT_PATH_LIMIT];
    char sha256[HWA_SHA256_HEX_SIZE];
    uint64_t copied = 0U;
    uint64_t source_bytes = 0U;
    size_t match = SIZE_MAX;
    if (!hwa_event_binding_index_find(binding_index, binding_count,
                                      relative_path, &match) ||
        bindings[match].source_path == NULL ||
        bindings[match].source_path[0] == '\0') {
        hwa_set_error(error, error_size,
                      "event payload has no source binding");
        return -1;
    }
    if (hwa_event_file_size(bindings[match].source_path, &source_bytes) != 0) {
        hwa_set_error(error, error_size,
                      "event payload source is not a regular file");
        return -1;
    }
    if (source_bytes > limits->max_payload_file_bytes) {
        hwa_set_error(error, error_size,
                      "event payload source size exceeds limit");
        return -1;
    }
    if (source_bytes != expected_bytes) {
        hwa_set_error(error, error_size,
                      "event payload source size does not match its descriptor");
        return -1;
    }
    if (hwa_event_make_payload_parent(directory, relative_path, target,
                                      error, error_size) != 0 ||
        hwa_event_copy_file(bindings[match].source_path, target,
                            expected_bytes, &copied,
                            error, error_size) != 0)
        return -1;
    if (copied != expected_bytes ||
        hwa_sha256_file(target, limits->max_payload_file_bytes,
                        sha256, error, error_size) != 0 ||
        strcmp(sha256, expected_sha256) != 0) {
        (void)HWA_EVENT_UNLINK(target);
        hwa_set_error(error, error_size,
                      "event payload does not match its descriptor");
        return -1;
    }
    if (hwa_event_append_payload_file(files, relative_path, copied, sha256,
                                      error, error_size) != 0) {
        (void)HWA_EVENT_UNLINK(target);
        return -1;
    }
    used[match] = 1U;
    return 0;
}

static int hwa_event_copy_payloads(const char *directory,
                                   const HWAEventBundle *bundle,
                                   const HWAEventFileBinding *bindings,
                                   size_t binding_count,
                                   const HWAEventBundleLimits *limits,
                                   const HWANumericLocale *locale,
                                   HWAEventManifestFiles *files,
                                   char *error,
                                   size_t error_size)
{
    unsigned char *used = NULL;
    HWAEventBindingIndexRow *binding_index = NULL;
    size_t expected = bundle->trace_count;
    size_t index;
    int result = -1;
    for (index = 0U; index < bundle->audio_count; ++index) {
        const char *path = bundle->audio[index].relative_path;
        if (path != NULL && path[0] != '\0') {
            if (expected == SIZE_MAX) {
                hwa_set_error(error, error_size,
                              "event payload count overflow");
                return -1;
            }
            expected++;
        }
    }
    if (binding_count != expected) {
        hwa_set_error(error, error_size,
                      "event payload binding count does not match payloads");
        return -1;
    }
    if (binding_count != 0U) {
        if (sizeof(*binding_index) > SIZE_MAX / binding_count) {
            hwa_set_error(error, error_size,
                          "event payload binding index overflows");
            return -1;
        }
        used = (unsigned char *)calloc(binding_count, sizeof(*used));
        binding_index = (HWAEventBindingIndexRow *)malloc(
            binding_count * sizeof(*binding_index));
        if (used == NULL || binding_index == NULL) {
            free(used);
            free(binding_index);
            hwa_set_error(error, error_size,
                          "cannot allocate event payload binding state");
            return -1;
        }
        for (index = 0U; index < binding_count; ++index) {
            if (bindings[index].relative_path == NULL) {
                hwa_set_error(error, error_size,
                              "event payload binding has no path");
                goto cleanup;
            }
            binding_index[index].path = bindings[index].relative_path;
            binding_index[index].row = index;
        }
        if (binding_count > 1U)
            qsort(binding_index, binding_count, sizeof(*binding_index),
                  hwa_event_binding_index_compare);
        for (index = 1U; index < binding_count; ++index) {
            if (strcmp(binding_index[index - 1U].path,
                       binding_index[index].path) == 0) {
                hwa_set_error(error, error_size,
                              "event payload has repeated bindings");
                goto cleanup;
            }
        }
    }
    for (index = 0U; index < bundle->audio_count; ++index) {
        const HWAEventAudio *audio = &bundle->audio[index];
        if (audio->relative_path == NULL || audio->relative_path[0] == '\0')
            continue;
        if (hwa_event_copy_bound_payload(directory, audio->relative_path,
                                         audio->file_bytes, audio->sha256,
                                         bindings, binding_index,
                                         binding_count, used, limits, files,
                                         error, error_size) != 0)
            goto cleanup;
    }
    for (index = 0U; index < bundle->trace_count; ++index) {
        const HWAEventTrace *trace = &bundle->traces[index];
        char target[HWA_EVENT_PATH_LIMIT];
        if (hwa_event_copy_bound_payload(directory, trace->relative_path,
                                         trace->file_bytes, trace->sha256,
                                         bindings, binding_index,
                                         binding_count, used, limits, files,
                                         error, error_size) != 0)
            goto cleanup;
        if (hwa_event_join(target, directory, trace->relative_path) != 0 ||
            hwa_event_validate_trace_payload(target, trace, locale,
                                             error, error_size) != 0)
            goto cleanup;
    }
    for (index = 0U; index < binding_count; ++index) {
        if (!used[index]) {
            hwa_set_error(error, error_size,
                          "event payload binding does not name a payload");
            goto cleanup;
        }
    }
    if (hwa_event_finalize_payload_files(files, error, error_size) == 0)
        result = 0;
cleanup:
    free(used);
    free(binding_index);
    return result;
}

static int hwa_event_copy_bound_source(
    const char *directory,
    const char *relative_path,
    uint64_t expected_bytes,
    const char *expected_sha256,
    const HWAEventSourceBinding *bindings,
    const HWAEventBindingIndexRow *binding_index,
    size_t binding_count,
    unsigned char *used,
    const HWAEventBundleLimits *limits,
    HWAEventManifestFiles *files,
    char *error,
    size_t error_size)
{
    char target[HWA_EVENT_PATH_LIMIT];
    char sha256[HWA_SHA256_HEX_SIZE];
    uint64_t copied = 0U;
    size_t match = SIZE_MAX;
    if (!hwa_event_binding_index_find(binding_index, binding_count,
                                      relative_path, &match) ||
        bindings[match].source.name == NULL ||
        bindings[match].source.name[0] == '\0' ||
        bindings[match].source.read_at == NULL) {
        hwa_set_error(error, error_size,
                      "event payload has no byte-source binding");
        return -1;
    }
    if (bindings[match].source.size > limits->max_payload_file_bytes) {
        hwa_set_error(error, error_size,
                      "event byte-source size exceeds limit");
        return -1;
    }
    if (bindings[match].source.size != expected_bytes) {
        hwa_set_error(error, error_size,
                      "event byte-source size does not match its descriptor");
        return -1;
    }
    if (hwa_event_make_payload_parent(directory, relative_path, target,
                                      error, error_size) != 0 ||
        hwa_event_copy_source(&bindings[match].source, target,
                              expected_bytes, &copied,
                              error, error_size) != 0)
        return -1;
    if (copied != expected_bytes ||
        hwa_sha256_file(target, limits->max_payload_file_bytes,
                        sha256, error, error_size) != 0 ||
        strcmp(sha256, expected_sha256) != 0) {
        (void)HWA_EVENT_UNLINK(target);
        hwa_set_error(error, error_size,
                      "event byte-source does not match its descriptor");
        return -1;
    }
    if (hwa_event_append_payload_file(files, relative_path, copied, sha256,
                                      error, error_size) != 0) {
        (void)HWA_EVENT_UNLINK(target);
        return -1;
    }
    used[match] = 1U;
    return 0;
}

static int hwa_event_copy_source_payloads(
    const char *directory,
    const HWAEventBundle *bundle,
    const HWAEventSourceBinding *bindings,
    size_t binding_count,
    const HWAEventBundleLimits *limits,
    const HWANumericLocale *locale,
    HWAEventManifestFiles *files,
    char *error,
    size_t error_size)
{
    unsigned char *used = NULL;
    HWAEventBindingIndexRow *binding_index = NULL;
    size_t expected = bundle->trace_count;
    size_t index;
    int result = -1;
    for (index = 0U; index < bundle->audio_count; ++index) {
        const char *path = bundle->audio[index].relative_path;
        if (path != NULL && path[0] != '\0') {
            if (expected == SIZE_MAX) {
                hwa_set_error(error, error_size,
                              "event payload count overflow");
                return -1;
            }
            expected++;
        }
    }
    if (binding_count != expected) {
        hwa_set_error(error, error_size,
                      "event payload binding count does not match payloads");
        return -1;
    }
    if (binding_count != 0U) {
        if (sizeof(*binding_index) > SIZE_MAX / binding_count) {
            hwa_set_error(error, error_size,
                          "event payload binding index overflows");
            return -1;
        }
        used = (unsigned char *)calloc(binding_count, sizeof(*used));
        binding_index = (HWAEventBindingIndexRow *)malloc(
            binding_count * sizeof(*binding_index));
        if (used == NULL || binding_index == NULL) {
            free(used);
            free(binding_index);
            hwa_set_error(error, error_size,
                          "cannot allocate event payload binding state");
            return -1;
        }
        for (index = 0U; index < binding_count; ++index) {
            if (bindings[index].relative_path == NULL) {
                hwa_set_error(error, error_size,
                              "event payload binding has no path");
                goto cleanup;
            }
            binding_index[index].path = bindings[index].relative_path;
            binding_index[index].row = index;
        }
        if (binding_count > 1U)
            qsort(binding_index, binding_count, sizeof(*binding_index),
                  hwa_event_binding_index_compare);
        for (index = 1U; index < binding_count; ++index) {
            if (strcmp(binding_index[index - 1U].path,
                       binding_index[index].path) == 0) {
                hwa_set_error(error, error_size,
                              "event payload has repeated bindings");
                goto cleanup;
            }
        }
    }
    for (index = 0U; index < bundle->audio_count; ++index) {
        const HWAEventAudio *audio = &bundle->audio[index];
        if (audio->relative_path == NULL || audio->relative_path[0] == '\0')
            continue;
        if (hwa_event_copy_bound_source(
                directory, audio->relative_path, audio->file_bytes,
                audio->sha256, bindings, binding_index, binding_count,
                used, limits, files, error, error_size) != 0)
            goto cleanup;
    }
    for (index = 0U; index < bundle->trace_count; ++index) {
        const HWAEventTrace *trace = &bundle->traces[index];
        char target[HWA_EVENT_PATH_LIMIT];
        if (hwa_event_copy_bound_source(
                directory, trace->relative_path, trace->file_bytes,
                trace->sha256, bindings, binding_index, binding_count,
                used, limits, files, error, error_size) != 0)
            goto cleanup;
        if (hwa_event_join(target, directory, trace->relative_path) != 0 ||
            hwa_event_validate_trace_payload(target, trace, locale,
                                             error, error_size) != 0)
            goto cleanup;
    }
    for (index = 0U; index < binding_count; ++index) {
        if (!used[index]) {
            hwa_set_error(error, error_size,
                          "event payload binding does not name a payload");
            goto cleanup;
        }
    }
    if (hwa_event_finalize_payload_files(files, error, error_size) == 0)
        result = 0;
cleanup:
    free(used);
    free(binding_index);
    return result;
}

typedef struct HWAEventIdIndexRow {
    uint64_t id;
    size_t row;
} HWAEventIdIndexRow;

typedef struct HWAEventPathIndexRow {
    const char *path;
    size_t row;
} HWAEventPathIndexRow;

static void *hwa_event_index_allocate(size_t count,
                                      size_t item_size,
                                      char *error,
                                      size_t error_size)
{
    void *rows;
    if (count == 0U) return NULL;
    if (item_size > SIZE_MAX / count) {
        hwa_set_error(error, error_size, "event validation index overflows");
        return NULL;
    }
    rows = malloc(count * item_size);
    if (rows == NULL)
        hwa_set_error(error, error_size,
                      "cannot allocate event validation index");
    return rows;
}

static int hwa_event_id_index_compare(const void *left, const void *right)
{
    const HWAEventIdIndexRow *left_row =
        (const HWAEventIdIndexRow *)left;
    const HWAEventIdIndexRow *right_row =
        (const HWAEventIdIndexRow *)right;
    if (left_row->id < right_row->id) return -1;
    if (left_row->id > right_row->id) return 1;
    return 0;
}

static int hwa_event_path_index_compare(const void *left, const void *right)
{
    const HWAEventPathIndexRow *left_row =
        (const HWAEventPathIndexRow *)left;
    const HWAEventPathIndexRow *right_row =
        (const HWAEventPathIndexRow *)right;
    return strcmp(left_row->path, right_row->path);
}

static int hwa_event_value_pointer_compare(const void *left,
                                           const void *right)
{
    const HWAEventValue *const *left_value =
        (const HWAEventValue *const *)left;
    const HWAEventValue *const *right_value =
        (const HWAEventValue *const *)right;
    return strcmp((*left_value)->name, (*right_value)->name);
}

static int hwa_event_id_index_find(const HWAEventIdIndexRow *rows,
                                   size_t count,
                                   uint64_t id,
                                   size_t *result)
{
    size_t first = 0U;
    size_t last = count;
    while (first < last) {
        size_t middle = first + (last - first) / 2U;
        if (rows[middle].id < id)
            first = middle + 1U;
        else
            last = middle;
    }
    if (first == count || rows[first].id != id) return 0;
    if (result != NULL) *result = rows[first].row;
    return 1;
}

static int hwa_event_id_index_unique(const HWAEventIdIndexRow *rows,
                                     size_t count)
{
    size_t index;
    for (index = 1U; index < count; ++index) {
        if (rows[index - 1U].id == rows[index].id) return 0;
    }
    return 1;
}

static int hwa_event_path_index_unique(const HWAEventPathIndexRow *rows,
                                       size_t count)
{
    size_t index;
    for (index = 1U; index < count; ++index) {
        if (strcmp(rows[index - 1U].path, rows[index].path) == 0) return 0;
    }
    return 1;
}

static int hwa_event_validate_value(const HWAEventValue *value,
                                    char *error,
                                    size_t error_size)
{
    if (!hwa_event_value_name(value->name) ||
        value->kind <= 0 || value->kind >= HWA_EVENT_VALUE_KIND_COUNT ||
        value->basis <= 0 || value->basis >= HWA_EVENT_VALUE_BASIS_COUNT ||
        (value->unit != NULL && !hwa_event_utf8_text(value->unit))) {
        hwa_set_error(error, error_size, "invalid event value");
        return -1;
    }
    if ((value->kind == HWA_EVENT_VALUE_TEXT &&
         (value->text == NULL || !hwa_event_utf8_text(value->text))) ||
        (value->kind == HWA_EVENT_VALUE_F64 && !isfinite(value->number)) ||
        (value->kind == HWA_EVENT_VALUE_I64 &&
         (value->integer < -INT64_C(9007199254740991) ||
          value->integer > INT64_C(9007199254740991))) ||
        (value->kind == HWA_EVENT_VALUE_BOOL &&
         value->boolean != 0 && value->boolean != 1) ||
        (value->score_valid &&
         (!isfinite(value->score) || value->score < 0.0 || value->score > 1.0)) ||
        (value->provider_id_valid && !hwa_event_safe_id(value->provider_id))) {
        hwa_set_error(error, error_size, "invalid event value payload");
        return -1;
    }
    return 0;
}

static int hwa_event_validate_bundle(const HWAEventBundle *bundle,
                                     const HWAEventBundleLimits *limits,
                                     char *error,
                                     size_t error_size)
{
    HWAEventIdIndexRow *provider_ids = NULL;
    HWAEventIdIndexRow *audio_ids = NULL;
    HWAEventIdIndexRow *trace_ids = NULL;
    HWAEventIdIndexRow *event_ids = NULL;
    HWAEventIdIndexRow *warning_ids = NULL;
    HWAEventPathIndexRow *audio_paths = NULL;
    HWAEventPathIndexRow *trace_paths = NULL;
    const HWAEventValue **value_order = NULL;
    size_t value_order_capacity = 0U;
    size_t *parent_rows = NULL;
    unsigned char *parent_state = NULL;
    size_t audio_path_count = 0U;
    size_t trace_path_count = 0U;
    size_t index;
    size_t total_values = 0U;
    size_t total_trace_refs = 0U;
    int result = -1;
    if (bundle == NULL || limits == NULL ||
        bundle->audio_count == 0U || bundle->audio == NULL ||
        bundle->audio_count > limits->max_audio_files ||
        bundle->event_count > limits->max_events ||
        (bundle->event_count != 0U && bundle->events == NULL) ||
        bundle->trace_count > limits->max_traces ||
        (bundle->trace_count != 0U && bundle->traces == NULL) ||
        bundle->provider_count > limits->max_providers ||
        (bundle->provider_count != 0U && bundle->providers == NULL) ||
        bundle->warning_count > limits->max_warnings ||
        (bundle->warning_count != 0U && bundle->warnings == NULL)) {
        hwa_set_error(error, error_size, "invalid event bundle counts");
        return -1;
    }
    provider_ids = (HWAEventIdIndexRow *)hwa_event_index_allocate(
        bundle->provider_count, sizeof(*provider_ids), error, error_size);
    audio_ids = (HWAEventIdIndexRow *)hwa_event_index_allocate(
        bundle->audio_count, sizeof(*audio_ids), error, error_size);
    trace_ids = (HWAEventIdIndexRow *)hwa_event_index_allocate(
        bundle->trace_count, sizeof(*trace_ids), error, error_size);
    event_ids = (HWAEventIdIndexRow *)hwa_event_index_allocate(
        bundle->event_count, sizeof(*event_ids), error, error_size);
    warning_ids = (HWAEventIdIndexRow *)hwa_event_index_allocate(
        bundle->warning_count, sizeof(*warning_ids), error, error_size);
    audio_paths = (HWAEventPathIndexRow *)hwa_event_index_allocate(
        bundle->audio_count, sizeof(*audio_paths), error, error_size);
    trace_paths = (HWAEventPathIndexRow *)hwa_event_index_allocate(
        bundle->trace_count, sizeof(*trace_paths), error, error_size);
    parent_rows = (size_t *)hwa_event_index_allocate(
        bundle->event_count, sizeof(*parent_rows), error, error_size);
    parent_state = (unsigned char *)hwa_event_index_allocate(
        bundle->event_count, sizeof(*parent_state), error, error_size);
    if ((bundle->provider_count != 0U && provider_ids == NULL) ||
        audio_ids == NULL ||
        (bundle->trace_count != 0U &&
         (trace_ids == NULL || trace_paths == NULL)) ||
        (bundle->event_count != 0U &&
         (event_ids == NULL || parent_rows == NULL || parent_state == NULL)) ||
        (bundle->warning_count != 0U && warning_ids == NULL) ||
        audio_paths == NULL)
        goto cleanup;
    if (bundle->event_count != 0U)
        memset(parent_state, 0, bundle->event_count * sizeof(*parent_state));
    for (index = 0U; index < bundle->provider_count; ++index) {
        const HWAEventProvider *provider = &bundle->providers[index];
        if (!hwa_event_safe_id(provider->id) || provider->name == NULL ||
            provider->name[0] == '\0' || provider->version == NULL ||
            provider->version[0] == '\0' ||
            !hwa_event_utf8_text(provider->name) ||
            !hwa_event_utf8_text(provider->version) ||
            (provider->model_sha256[0] != '\0' &&
             !hwa_event_hex_string(provider->model_sha256)) ||
            provider->settings_json == NULL ||
            provider->settings_json[0] != '{') {
            hwa_set_error(error, error_size, "invalid event provider row");
            goto cleanup;
        }
        provider_ids[index].id = provider->id;
        provider_ids[index].row = index;
    }
    if (bundle->provider_count > 1U)
        qsort(provider_ids, bundle->provider_count, sizeof(*provider_ids),
              hwa_event_id_index_compare);
    if (!hwa_event_id_index_unique(provider_ids, bundle->provider_count)) {
        hwa_set_error(error, error_size, "duplicate event provider id");
        goto cleanup;
    }
    for (index = 0U; index < bundle->audio_count; ++index) {
        const HWAEventAudio *audio = &bundle->audio[index];
        const char *kind_name = hwa_event_audio_kind_name(audio->kind);
        if (!hwa_event_safe_id(audio->id) || kind_name == NULL ||
            audio->name == NULL || audio->name[0] == '\0' ||
            !hwa_event_utf8_text(audio->name) ||
            (audio->path_hint != NULL &&
             !hwa_event_utf8_text(audio->path_hint)) ||
            !hwa_event_hex_string(audio->sha256) ||
            hwa_event_container_name(audio->format.container) == NULL ||
            hwa_event_encoding_name(audio->format.encoding) == NULL ||
            audio->format.channels == 0U ||
            audio->format.sample_rate_hz == 0U ||
            audio->format.bits_per_sample == 0U ||
            audio->format.valid_bits_per_sample == 0U ||
            audio->format.valid_bits_per_sample >
                audio->format.bits_per_sample ||
            audio->format.block_align == 0U ||
            audio->format.frames > HWA_EVENT_JSON_SAFE_INTEGER ||
            audio->format.data_bytes > HWA_EVENT_JSON_SAFE_INTEGER ||
            audio->file_bytes > HWA_EVENT_JSON_SAFE_INTEGER ||
            audio->file_bytes > limits->max_payload_file_bytes ||
            !isfinite(audio->format.duration_seconds) ||
            audio->format.duration_seconds < 0.0 ||
            audio->format.duration_seconds !=
                (double)audio->format.frames /
                    (double)audio->format.sample_rate_hz ||
            ((audio->relative_path != NULL &&
              audio->relative_path[0] != '\0') &&
             (!hwa_event_relative_path(audio->relative_path, "audio/") ||
             audio->file_bytes == 0U))) {
            hwa_set_error(error, error_size, "invalid event audio row");
            goto cleanup;
        }
        audio_ids[index].id = audio->id;
        audio_ids[index].row = index;
        if (audio->relative_path != NULL && audio->relative_path[0] != '\0') {
            audio_paths[audio_path_count].path = audio->relative_path;
            audio_paths[audio_path_count].row = index;
            audio_path_count++;
        }
    }
    if (bundle->audio_count > 1U)
        qsort(audio_ids, bundle->audio_count, sizeof(*audio_ids),
              hwa_event_id_index_compare);
    if (!hwa_event_id_index_unique(audio_ids, bundle->audio_count)) {
        hwa_set_error(error, error_size, "duplicate event audio id");
        goto cleanup;
    }
    if (audio_path_count > 1U)
        qsort(audio_paths, audio_path_count, sizeof(*audio_paths),
              hwa_event_path_index_compare);
    if (!hwa_event_path_index_unique(audio_paths, audio_path_count)) {
        hwa_set_error(error, error_size, "duplicate event audio path");
        goto cleanup;
    }
    for (index = 0U; index < bundle->audio_count; ++index) {
        const HWAEventAudio *audio = &bundle->audio[index];
        if (audio->kind == HWA_EVENT_SOURCE_RECORDING) {
            if (audio->source_recording_id_valid) {
                hwa_set_error(error, error_size,
                              "source recording cannot name a source recording");
                goto cleanup;
            }
        } else {
            size_t source_index;
            if (!audio->source_recording_id_valid ||
                !hwa_event_id_index_find(audio_ids, bundle->audio_count,
                                         audio->source_recording_id,
                                         &source_index) ||
                bundle->audio[source_index].kind !=
                    HWA_EVENT_SOURCE_RECORDING ||
                !hwa_event_relative_path(audio->relative_path, "audio/") ||
                audio->format.sample_rate_hz !=
                    bundle->audio[source_index].format.sample_rate_hz ||
                audio->format.frames !=
                    bundle->audio[source_index].format.frames) {
                hwa_set_error(error, error_size,
                              "derived audio has an invalid source clock");
                goto cleanup;
            }
        }
    }
    for (index = 0U; index < bundle->trace_count; ++index) {
        const HWAEventTrace *trace = &bundle->traces[index];
        size_t source_index;
        uint64_t final_start;
        uint64_t step_count;
        const char *format_name = hwa_event_trace_format_name(trace->format);
        if (!hwa_event_safe_id(trace->id) ||
            !hwa_event_value_name(trace->name) || trace->unit == NULL ||
            !hwa_event_utf8_text(trace->unit) ||
            !hwa_event_relative_path(trace->relative_path, "traces/") ||
            !hwa_event_hex_string(trace->sha256) || format_name == NULL ||
            !hwa_event_id_index_find(audio_ids, bundle->audio_count,
                                     trace->source_recording_id,
                                     &source_index) ||
            bundle->audio[source_index].kind != HWA_EVENT_SOURCE_RECORDING ||
            trace->hop_samples == 0U || trace->window_samples == 0U ||
            trace->point_count == 0U || trace->value_width == 0U ||
            trace->first_sample > HWA_EVENT_JSON_SAFE_INTEGER ||
            trace->hop_samples > HWA_EVENT_JSON_SAFE_INTEGER ||
            trace->window_samples > HWA_EVENT_JSON_SAFE_INTEGER ||
            trace->point_count > HWA_EVENT_JSON_SAFE_INTEGER ||
            trace->file_bytes > limits->max_payload_file_bytes ||
            trace->file_bytes > HWA_EVENT_JSON_SAFE_INTEGER) {
            hwa_set_error(error, error_size, "invalid event trace row");
            goto cleanup;
        }
        trace_ids[index].id = trace->id;
        trace_ids[index].row = index;
        trace_paths[trace_path_count].path = trace->relative_path;
        trace_paths[trace_path_count].row = index;
        trace_path_count++;
        step_count = trace->point_count - 1U;
        if (step_count > (UINT64_MAX - trace->first_sample) /
                             trace->hop_samples) {
            hwa_set_error(error, error_size, "event trace grid overflows");
            goto cleanup;
        }
        final_start = trace->first_sample + step_count * trace->hop_samples;
        if (final_start > bundle->audio[source_index].format.frames ||
            trace->window_samples >
                bundle->audio[source_index].format.frames - final_start) {
            hwa_set_error(error, error_size,
                          "event trace extends past its source recording");
            goto cleanup;
        }
        if (trace->format == HWA_EVENT_TRACE_F64LE) {
            uint64_t expected;
            if ((uint64_t)trace->value_width >
                    UINT64_MAX / trace->point_count ||
                trace->point_count * (uint64_t)trace->value_width >
                    UINT64_MAX / 8U) {
                hwa_set_error(error, error_size,
                              "event trace payload size overflows");
                goto cleanup;
            }
            expected = trace->point_count * (uint64_t)trace->value_width * 8U;
            if (trace->file_bytes != expected) {
                hwa_set_error(error, error_size,
                              "event trace payload size is wrong");
                goto cleanup;
            }
        } else if (trace->file_bytes == 0U) {
            hwa_set_error(error, error_size, "event trace payload is empty");
            goto cleanup;
        }
    }
    if (bundle->trace_count > 1U) {
        qsort(trace_ids, bundle->trace_count, sizeof(*trace_ids),
              hwa_event_id_index_compare);
        qsort(trace_paths, trace_path_count, sizeof(*trace_paths),
              hwa_event_path_index_compare);
    }
    if (!hwa_event_id_index_unique(trace_ids, bundle->trace_count)) {
        hwa_set_error(error, error_size, "duplicate event trace id");
        goto cleanup;
    }
    if (!hwa_event_path_index_unique(trace_paths, trace_path_count)) {
        hwa_set_error(error, error_size, "duplicate event trace path");
        goto cleanup;
    }
    for (index = 0U; index < bundle->event_count; ++index) {
        const HWAPerformanceEvent *event = &bundle->events[index];
        if (!hwa_event_safe_id(event->id) || event->kind == NULL ||
            event->kind[0] == '\0' ||
            !hwa_event_utf8_text(event->kind) ||
            (event->voice != NULL && !hwa_event_utf8_text(event->voice)) ||
            (event->part != NULL && !hwa_event_utf8_text(event->part)) ||
            (event->score_event_id != NULL &&
             !hwa_event_utf8_text(event->score_event_id)) ||
            event->start_sample >= event->end_sample ||
            event->end_sample > HWA_EVENT_JSON_SAFE_INTEGER ||
            (event->value_count != 0U && event->values == NULL) ||
            (event->trace_ref_count != 0U && event->trace_refs == NULL)) {
            hwa_set_error(error, error_size, "invalid performance event");
            goto cleanup;
        }
        event_ids[index].id = event->id;
        event_ids[index].row = index;
        if (SIZE_MAX - total_values < event->value_count) {
            hwa_set_error(error, error_size, "event value count overflow");
            goto cleanup;
        }
        total_values += event->value_count;
        if (total_values > limits->max_values) {
            hwa_set_error(error, error_size, "event value limit exceeded");
            goto cleanup;
        }
        if (SIZE_MAX - total_trace_refs < event->trace_ref_count) {
            hwa_set_error(error, error_size,
                          "event trace reference count overflow");
            goto cleanup;
        }
        total_trace_refs += event->trace_ref_count;
        if (total_trace_refs > limits->max_trace_refs) {
            hwa_set_error(error, error_size,
                          "event trace reference limit exceeded");
            goto cleanup;
        }
    }
    if (bundle->event_count > 1U)
        qsort(event_ids, bundle->event_count, sizeof(*event_ids),
              hwa_event_id_index_compare);
    if (!hwa_event_id_index_unique(event_ids, bundle->event_count)) {
        hwa_set_error(error, error_size, "duplicate performance event id");
        goto cleanup;
    }
    for (index = 0U; index < bundle->event_count; ++index) {
        const HWAPerformanceEvent *event = &bundle->events[index];
        size_t source_index;
        size_t value_index;
        if (!hwa_event_id_index_find(audio_ids, bundle->audio_count,
                                     event->source_recording_id,
                                     &source_index) ||
            bundle->audio[source_index].kind != HWA_EVENT_SOURCE_RECORDING ||
            event->end_sample > bundle->audio[source_index].format.frames) {
            hwa_set_error(error, error_size, "invalid performance event");
            goto cleanup;
        }
        if (event->evidence_audio_id_valid) {
            size_t evidence_index;
            const HWAEventAudio *evidence;
            if (!hwa_event_id_index_find(audio_ids, bundle->audio_count,
                                         event->evidence_audio_id,
                                         &evidence_index)) {
                hwa_set_error(error, error_size,
                              "event evidence audio is missing");
                goto cleanup;
            }
            evidence = &bundle->audio[evidence_index];
            if ((evidence->kind == HWA_EVENT_SOURCE_RECORDING &&
                 evidence->id != event->source_recording_id) ||
                (evidence->kind != HWA_EVENT_SOURCE_RECORDING &&
                 (!evidence->source_recording_id_valid ||
                  evidence->source_recording_id !=
                      event->source_recording_id))) {
                hwa_set_error(error, error_size,
                              "event evidence uses another source clock");
                goto cleanup;
            }
        }
        if (event->parent_id_valid) {
            size_t parent_index;
            const HWAPerformanceEvent *parent;
            if (!hwa_event_safe_id(event->parent_id) ||
                !hwa_event_id_index_find(event_ids, bundle->event_count,
                                         event->parent_id, &parent_index)) {
                hwa_set_error(error, error_size, "event parent is missing");
                goto cleanup;
            }
            parent = &bundle->events[parent_index];
            if (parent->id == event->id ||
                parent->source_recording_id != event->source_recording_id ||
                parent->start_sample > event->start_sample ||
                parent->end_sample < event->end_sample) {
                hwa_set_error(error, error_size,
                              "event does not fit within its parent");
                goto cleanup;
            }
            parent_rows[index] = parent_index;
        } else {
            parent_rows[index] = SIZE_MAX;
        }
        if (event->value_count > value_order_capacity) {
            const HWAEventValue **rows;
            if (event->value_count > SIZE_MAX / sizeof(*rows)) {
                hwa_set_error(error, error_size,
                              "event value order index overflows");
                goto cleanup;
            }
            rows = (const HWAEventValue **)realloc(
                value_order, event->value_count * sizeof(*rows));
            if (rows == NULL) {
                hwa_set_error(error, error_size,
                              "cannot allocate event value order index");
                goto cleanup;
            }
            value_order = rows;
            value_order_capacity = event->value_count;
        }
        for (value_index = 0U; value_index < event->value_count;
             ++value_index) {
            const HWAEventValue *value = &event->values[value_index];
            if (hwa_event_validate_value(value, error, error_size) != 0)
                goto cleanup;
            if (value->provider_id_valid &&
                !hwa_event_id_index_find(provider_ids,
                                         bundle->provider_count,
                                         value->provider_id, NULL)) {
                hwa_set_error(error, error_size,
                              "event value provider is missing");
                goto cleanup;
            }
            value_order[value_index] = value;
        }
        if (event->value_count > 1U)
            qsort(value_order, event->value_count, sizeof(*value_order),
                  hwa_event_value_pointer_compare);
        value_index = 0U;
        while (value_index < event->value_count) {
            const HWAEventValue *first_value = value_order[value_index];
            const char *first_unit = first_value->unit == NULL
                                         ? ""
                                         : first_value->unit;
            int selected = 0;
            size_t candidate = value_index;
            while (candidate < event->value_count &&
                   strcmp(value_order[candidate]->name,
                          first_value->name) == 0) {
                const HWAEventValue *value = value_order[candidate];
                const char *unit = value->unit == NULL ? "" : value->unit;
                if (value->kind != first_value->kind ||
                    strcmp(unit, first_unit) != 0) {
                    hwa_set_error(
                        error, error_size,
                        "competing event values disagree on type or unit");
                    goto cleanup;
                }
                if (value->selected && selected) {
                    hwa_set_error(
                        error, error_size,
                        "event value has more than one selected candidate");
                    goto cleanup;
                }
                if (value->selected) selected = 1;
                candidate++;
            }
            value_index = candidate;
        }
        for (value_index = 0U; value_index < event->trace_ref_count;
             ++value_index) {
            const HWAEventTraceRef *ref = &event->trace_refs[value_index];
            size_t trace_index;
            if (!hwa_event_id_index_find(trace_ids, bundle->trace_count,
                                         ref->trace_id, &trace_index) ||
                ref->role == NULL || ref->role[0] == '\0' ||
                !hwa_event_utf8_text(ref->role) ||
                ref->point_count == 0U ||
                ref->first_point > bundle->traces[trace_index].point_count ||
                ref->point_count >
                    bundle->traces[trace_index].point_count -
                        ref->first_point) {
                hwa_set_error(error, error_size,
                              "invalid event trace reference");
                goto cleanup;
            }
        }
    }
    for (index = 0U; index < bundle->event_count; ++index) {
        size_t cursor = index;
        size_t path_count = 0U;
        size_t depth;
        if (parent_state[index] == 2U) continue;
        while (cursor != SIZE_MAX && parent_state[cursor] == 0U) {
            parent_state[cursor] = 1U;
            path_count++;
            cursor = parent_rows[cursor];
        }
        if (cursor != SIZE_MAX && parent_state[cursor] == 1U) {
            hwa_set_error(error, error_size,
                          "event parent nesting is invalid");
            goto cleanup;
        }
        if (cursor == SIZE_MAX) {
            depth = path_count - 1U;
        } else {
            if (parent_rows[cursor] > SIZE_MAX - path_count) {
                hwa_set_error(error, error_size,
                              "event parent nesting is invalid");
                goto cleanup;
            }
            depth = parent_rows[cursor] + path_count;
        }
        if (depth > limits->max_nesting_depth) {
            hwa_set_error(error, error_size,
                          "event parent nesting is invalid");
            goto cleanup;
        }
        cursor = index;
        while (cursor != SIZE_MAX && parent_state[cursor] == 1U) {
            size_t parent_index = parent_rows[cursor];
            parent_rows[cursor] = depth;
            parent_state[cursor] = 2U;
            if (depth != 0U) depth--;
            cursor = parent_index;
        }
    }
    for (index = 0U; index < bundle->warning_count; ++index) {
        const HWAEventWarning *warning = &bundle->warnings[index];
        if (!hwa_event_safe_id(warning->id) || warning->code == NULL ||
            warning->code[0] == '\0' || warning->message == NULL ||
            warning->message[0] == '\0' ||
            !hwa_event_utf8_text(warning->code) ||
            !hwa_event_utf8_text(warning->message) ||
            (warning->event_id_valid &&
             !hwa_event_id_index_find(event_ids, bundle->event_count,
                                      warning->event_id, NULL))) {
            hwa_set_error(error, error_size, "invalid event warning row");
            goto cleanup;
        }
        warning_ids[index].id = warning->id;
        warning_ids[index].row = index;
    }
    if (bundle->warning_count > 1U)
        qsort(warning_ids, bundle->warning_count, sizeof(*warning_ids),
              hwa_event_id_index_compare);
    if (!hwa_event_id_index_unique(warning_ids, bundle->warning_count)) {
        hwa_set_error(error, error_size, "duplicate event warning id");
        goto cleanup;
    }
    result = 0;
cleanup:
    free(provider_ids);
    free(audio_ids);
    free(trace_ids);
    free(event_ids);
    free(warning_ids);
    free(audio_paths);
    free(trace_paths);
    free(value_order);
    free(parent_rows);
    free(parent_state);
    return result;
}

static int hwa_event_write_format(FILE *stream,
                                  const HWAFormat *format,
                                  const HWANumericLocale *locale)
{
    const char *container = hwa_event_container_name(format->container);
    const char *encoding = hwa_event_encoding_name(format->encoding);
    if (container == NULL || encoding == NULL ||
        fputs("{\"container\":", stream) == EOF ||
        hwa_event_json_write_string(stream, container) != 0 ||
        fputs(",\"encoding\":", stream) == EOF ||
        hwa_event_json_write_string(stream, encoding) != 0 ||
        fprintf(stream,
                ",\"channels\":%u,\"sample_rate_hz\":%" PRIu32
                ",\"bits_per_sample\":%u,\"valid_bits_per_sample\":%u"
                ",\"block_align\":%u,\"channel_mask\":%" PRIu32
                ",\"frames\":%" PRIu64 ",\"data_bytes\":%" PRIu64
                ",\"duration_seconds\":",
                (unsigned)format->channels, format->sample_rate_hz,
                (unsigned)format->bits_per_sample,
                (unsigned)format->valid_bits_per_sample,
                (unsigned)format->block_align, format->channel_mask,
                format->frames, format->data_bytes) < 0 ||
        hwa_event_json_write_double(stream, locale,
                                    format->duration_seconds) != 0 ||
        fputc('}', stream) == EOF) return -1;
    return 0;
}

static int hwa_event_write_audio(FILE *stream,
                                 const HWAEventAudio *audio,
                                 const HWANumericLocale *locale)
{
    const char *kind = hwa_event_audio_kind_name(audio->kind);
    if (kind == NULL ||
        fprintf(stream, "{\"id\":%" PRIu64 ",\"kind\":", audio->id) < 0 ||
        hwa_event_json_write_string(stream, kind) != 0 ||
        fputs(",\"name\":", stream) == EOF ||
        hwa_event_json_write_string(stream, audio->name) != 0 ||
        fputs(",\"relative_path\":", stream) == EOF ||
        hwa_event_json_write_string(
            stream, audio->relative_path == NULL ? "" : audio->relative_path) != 0 ||
        fputs(",\"path_hint\":", stream) == EOF ||
        hwa_event_json_write_string(
            stream, audio->path_hint == NULL ? "" : audio->path_hint) != 0 ||
        fputs(",\"sha256\":", stream) == EOF ||
        hwa_event_json_write_string(stream, audio->sha256) != 0 ||
        fprintf(stream, ",\"file_bytes\":%" PRIu64 ",\"format\":",
                audio->file_bytes) < 0 ||
        hwa_event_write_format(stream, &audio->format, locale) != 0 ||
        fputs(",\"source_recording_id\":", stream) == EOF) return -1;
    if (audio->source_recording_id_valid) {
        if (fprintf(stream, "%" PRIu64, audio->source_recording_id) < 0)
            return -1;
    } else if (fputs("null", stream) == EOF) {
        return -1;
    }
    return fputc('}', stream) == EOF ? -1 : 0;
}

static int hwa_event_write_provider(FILE *stream,
                                    const HWAEventProvider *provider,
                                    const HWAEventBundleLimits *limits,
                                    const HWANumericLocale *locale,
                                    char *error,
                                    size_t error_size)
{
    if (hwa_event_validate_settings_json(provider->settings_json, limits,
                                         locale, error, error_size) != 0)
        return -1;
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"name\":", provider->id) < 0 ||
        hwa_event_json_write_string(stream, provider->name) != 0 ||
        fputs(",\"version\":", stream) == EOF ||
        hwa_event_json_write_string(stream, provider->version) != 0 ||
        fputs(",\"model_sha256\":", stream) == EOF ||
        hwa_event_json_write_string(stream, provider->model_sha256) != 0 ||
        fputs(",\"settings\":", stream) == EOF ||
        fputs(provider->settings_json, stream) == EOF ||
        fputc('}', stream) == EOF) return -1;
    return 0;
}

static int hwa_event_write_warning(FILE *stream,
                                   const HWAEventWarning *warning)
{
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"code\":", warning->id) < 0 ||
        hwa_event_json_write_string(stream, warning->code) != 0 ||
        fputs(",\"message\":", stream) == EOF ||
        hwa_event_json_write_string(stream, warning->message) != 0 ||
        fputs(",\"event_id\":", stream) == EOF) return -1;
    if (warning->event_id_valid) {
        if (fprintf(stream, "%" PRIu64, warning->event_id) < 0) return -1;
    } else if (fputs("null", stream) == EOF) {
        return -1;
    }
    return fputc('}', stream) == EOF ? -1 : 0;
}

static int hwa_event_write_value(FILE *stream,
                                 const HWAEventValue *value,
                                 const HWANumericLocale *locale)
{
    const char *kind = hwa_event_value_kind_name(value->kind);
    const char *basis = hwa_event_value_basis_name(value->basis);
    if (kind == NULL || basis == NULL || fputs("{\"name\":", stream) == EOF ||
        hwa_event_json_write_string(stream, value->name) != 0 ||
        fputs(",\"kind\":", stream) == EOF ||
        hwa_event_json_write_string(stream, kind) != 0 ||
        fputs(",\"basis\":", stream) == EOF ||
        hwa_event_json_write_string(stream, basis) != 0 ||
        fputs(",\"value\":", stream) == EOF) return -1;
    if (value->kind == HWA_EVENT_VALUE_TEXT) {
        if (hwa_event_json_write_string(stream, value->text) != 0) return -1;
    } else if (value->kind == HWA_EVENT_VALUE_F64) {
        if (hwa_event_json_write_double(stream, locale, value->number) != 0)
            return -1;
    } else if (value->kind == HWA_EVENT_VALUE_I64) {
        if (fprintf(stream, "%" PRId64, value->integer) < 0) return -1;
    } else if (fputs(value->boolean ? "true" : "false", stream) == EOF) {
        return -1;
    }
    if (fputs(",\"unit\":", stream) == EOF ||
        hwa_event_json_write_string(
            stream, value->unit == NULL ? "" : value->unit) != 0 ||
        fputs(",\"score\":", stream) == EOF) return -1;
    if (value->score_valid) {
        if (hwa_event_json_write_double(stream, locale, value->score) != 0)
            return -1;
    } else if (fputs("null", stream) == EOF) {
        return -1;
    }
    if (fputs(",\"provider_id\":", stream) == EOF) return -1;
    if (value->provider_id_valid) {
        if (fprintf(stream, "%" PRIu64, value->provider_id) < 0) return -1;
    } else if (fputs("null", stream) == EOF) {
        return -1;
    }
    return fprintf(stream, ",\"selected\":%s}",
                   value->selected ? "true" : "false") < 0 ? -1 : 0;
}

static int hwa_event_write_trace_ref(FILE *stream,
                                     const HWAEventTraceRef *ref)
{
    if (fprintf(stream, "{\"trace_id\":%" PRIu64 ",\"role\":",
                ref->trace_id) < 0 ||
        hwa_event_json_write_string(stream, ref->role) != 0 ||
        fprintf(stream,
                ",\"first_point\":%" PRIu64
                ",\"point_count\":%" PRIu64 "}",
                ref->first_point, ref->point_count) < 0) return -1;
    return 0;
}

static int hwa_event_write_trace(FILE *stream, const HWAEventTrace *trace)
{
    const char *format = hwa_event_trace_format_name(trace->format);
    if (format == NULL ||
        fprintf(stream, "{\"id\":%" PRIu64 ",\"name\":", trace->id) < 0 ||
        hwa_event_json_write_string(stream, trace->name) != 0 ||
        fputs(",\"unit\":", stream) == EOF ||
        hwa_event_json_write_string(stream, trace->unit) != 0 ||
        fputs(",\"relative_path\":", stream) == EOF ||
        hwa_event_json_write_string(stream, trace->relative_path) != 0 ||
        fputs(",\"sha256\":", stream) == EOF ||
        hwa_event_json_write_string(stream, trace->sha256) != 0 ||
        fputs(",\"format\":", stream) == EOF ||
        hwa_event_json_write_string(stream, format) != 0 ||
        fprintf(stream,
                ",\"source_recording_id\":%" PRIu64
                ",\"first_sample\":%" PRIu64
                ",\"hop_samples\":%" PRIu64
                ",\"window_samples\":%" PRIu64
                ",\"point_count\":%" PRIu64
                ",\"value_width\":%" PRIu32
                ",\"file_bytes\":%" PRIu64 "}\n",
                trace->source_recording_id, trace->first_sample,
                trace->hop_samples, trace->window_samples,
                trace->point_count, trace->value_width,
                trace->file_bytes) < 0) return -1;
    return 0;
}

static int hwa_event_write_event(FILE *stream,
                                 const HWAPerformanceEvent *event,
                                 const HWANumericLocale *locale)
{
    size_t index;
    if (fprintf(stream, "{\"id\":%" PRIu64 ",\"kind\":", event->id) < 0 ||
        hwa_event_json_write_string(stream, event->kind) != 0 ||
        fprintf(stream,
                ",\"source_recording_id\":%" PRIu64
                ",\"evidence_audio_id\":",
                event->source_recording_id) < 0) return -1;
    if (event->evidence_audio_id_valid) {
        if (fprintf(stream, "%" PRIu64, event->evidence_audio_id) < 0)
            return -1;
    } else if (fputs("null", stream) == EOF) {
        return -1;
    }
    if (fputs(",\"parent_id\":", stream) == EOF) return -1;
    if (event->parent_id_valid) {
        if (fprintf(stream, "%" PRIu64, event->parent_id) < 0) return -1;
    } else if (fputs("null", stream) == EOF) {
        return -1;
    }
    if (fprintf(stream,
                ",\"start_sample\":%" PRIu64
                ",\"end_sample\":%" PRIu64 ",\"voice\":",
                event->start_sample, event->end_sample) < 0 ||
        hwa_event_json_write_string(
            stream, event->voice == NULL ? "" : event->voice) != 0 ||
        fputs(",\"part\":", stream) == EOF ||
        hwa_event_json_write_string(
            stream, event->part == NULL ? "" : event->part) != 0 ||
        fputs(",\"score_event_id\":", stream) == EOF ||
        hwa_event_json_write_string(
            stream,
            event->score_event_id == NULL ? "" : event->score_event_id) != 0 ||
        fputs(",\"values\":[", stream) == EOF) return -1;
    for (index = 0U; index < event->value_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_event_write_value(stream, &event->values[index], locale) != 0)
            return -1;
    }
    if (fputs("],\"trace_refs\":[", stream) == EOF) return -1;
    for (index = 0U; index < event->trace_ref_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_event_write_trace_ref(stream, &event->trace_refs[index]) != 0)
            return -1;
    }
    return fputs("]}\n", stream) == EOF ? -1 : 0;
}

static int hwa_event_write_indexes(const char *directory,
                                   const HWAEventBundle *bundle,
                                   const HWANumericLocale *locale,
                                   HWAEventManifestFiles *files,
                                   char *error,
                                   size_t error_size)
{
    char path[HWA_EVENT_PATH_LIMIT];
    FILE *stream;
    size_t index;
    if (hwa_event_join(path, directory, "events.jsonl") != 0 ||
        (stream = fopen(path, "wb")) == NULL) {
        hwa_set_error(error, error_size, "cannot create events index");
        return -1;
    }
    for (index = 0U; index < bundle->event_count; ++index) {
        if (hwa_event_write_event(stream, &bundle->events[index], locale) != 0) {
            (void)fclose(stream);
            hwa_set_error(error, error_size, "cannot write events index");
            return -1;
        }
    }
    if (fflush(stream) != 0) {
        (void)fclose(stream);
        hwa_set_error(error, error_size, "cannot finish events index");
        return -1;
    }
    if (fclose(stream) != 0 ||
        hwa_event_file_size(path, &files->events.bytes) != 0 ||
        hwa_sha256_file(path, UINT64_MAX, files->events.sha256,
                        error, error_size) != 0) {
        hwa_set_error(error, error_size, "cannot finish events index");
        return -1;
    }
    if (hwa_event_join(path, directory, "traces.jsonl") != 0 ||
        (stream = fopen(path, "wb")) == NULL) {
        hwa_set_error(error, error_size, "cannot create traces index");
        return -1;
    }
    for (index = 0U; index < bundle->trace_count; ++index) {
        if (hwa_event_write_trace(stream, &bundle->traces[index]) != 0) {
            (void)fclose(stream);
            hwa_set_error(error, error_size, "cannot write traces index");
            return -1;
        }
    }
    if (fflush(stream) != 0) {
        (void)fclose(stream);
        hwa_set_error(error, error_size, "cannot finish traces index");
        return -1;
    }
    if (fclose(stream) != 0 ||
        hwa_event_file_size(path, &files->traces.bytes) != 0 ||
        hwa_sha256_file(path, UINT64_MAX, files->traces.sha256,
                        error, error_size) != 0) {
        hwa_set_error(error, error_size, "cannot finish traces index");
        return -1;
    }
    files->events.seen = 1;
    files->traces.seen = 1;
    return 0;
}

static int hwa_event_write_manifest(const char *directory,
                                    const HWAEventBundle *bundle,
                                    const HWAEventManifestFiles *files,
                                    const HWAEventBundleLimits *limits,
                                    const HWANumericLocale *locale,
                                    char *error,
                                    size_t error_size)
{
    char path[HWA_EVENT_PATH_LIMIT];
    FILE *stream;
    size_t index;
    size_t value_count = 0U;
    size_t trace_ref_count = 0U;
    for (index = 0U; index < bundle->event_count; ++index) {
        value_count += bundle->events[index].value_count;
        trace_ref_count += bundle->events[index].trace_ref_count;
    }
    if (hwa_event_join(path, directory, "manifest.json") != 0 ||
        (stream = fopen(path, "wb")) == NULL) {
        hwa_set_error(error, error_size, "cannot create event manifest");
        return -1;
    }
    if (fprintf(stream,
                "{\n\"schema\":\"%s\",\n\"schema_version\":%u,\n"
                "\"tool_version\":\"%s\",\n"
                "\"counts\":{\"providers\":%zu,\"audio\":%zu,"
                "\"events\":%zu,\"values\":%zu,\"traces\":%zu,"
                "\"trace_refs\":%zu,\"warnings\":%zu},\n"
                "\"providers\":[",
                HWA_EVENT_BUNDLE_SCHEMA, HWA_EVENT_BUNDLE_SCHEMA_VERSION,
                HWA_VERSION, bundle->provider_count, bundle->audio_count,
                bundle->event_count, value_count, bundle->trace_count,
                trace_ref_count,
                bundle->warning_count) < 0) goto write_error;
    for (index = 0U; index < bundle->provider_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_event_write_provider(stream, &bundle->providers[index],
                                     limits, locale,
                                     error, error_size) != 0)
            goto write_error;
    }
    if (fputs("],\n\"audio\":[", stream) == EOF) goto write_error;
    for (index = 0U; index < bundle->audio_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_event_write_audio(stream, &bundle->audio[index], locale) != 0)
            goto write_error;
    }
    if (fputs("],\n\"warnings\":[", stream) == EOF) goto write_error;
    for (index = 0U; index < bundle->warning_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_event_write_warning(stream, &bundle->warnings[index]) != 0)
            goto write_error;
    }
    if (fprintf(stream,
                "],\n\"files\":["
                "{\"relative_path\":\"events.jsonl\","
                "\"file_bytes\":%" PRIu64 ",\"sha256\":\"%s\"},"
                "{\"relative_path\":\"traces.jsonl\","
                "\"file_bytes\":%" PRIu64 ",\"sha256\":\"%s\"}",
                files->events.bytes, files->events.sha256,
                files->traces.bytes, files->traces.sha256) < 0)
        goto write_error;
    for (index = 0U; index < files->payload_count; ++index) {
        if (fputs(",{\"relative_path\":", stream) == EOF ||
            hwa_event_json_write_string(
                stream, files->payloads[index].relative_path) != 0 ||
            fprintf(stream,
                    ",\"file_bytes\":%" PRIu64 ",\"sha256\":\"%s\"}",
                    files->payloads[index].bytes,
                    files->payloads[index].sha256) < 0)
            goto write_error;
    }
    if (fputs("]\n}\n", stream) == EOF) goto write_error;
    if (fflush(stream) != 0) {
        (void)fclose(stream);
        hwa_set_error(error, error_size, "cannot finish event manifest");
        return -1;
    }
    if (fclose(stream) != 0) {
        hwa_set_error(error, error_size, "cannot finish event manifest");
        return -1;
    }
    return 0;
write_error:
    (void)fclose(stream);
    if (error == NULL || error_size == 0U || error[0] == '\0')
        hwa_set_error(error, error_size, "cannot write event manifest");
    return -1;
}

static int hwa_event_parse_format(HWAEventJson *json, HWAFormat *format)
{
    unsigned seen = 0U;
    int first = 1;
    if (hwa_event_json_take(json, '{') != 0 ||
        hwa_event_json_depth_begin(json) != 0) return -1;
    while (!hwa_event_json_peek(json, '}')) {
        char *key = NULL;
        int state = hwa_event_json_key(json, &key, &first);
        if (state != 0) return state < 0 ? -1 : hwa_event_json_fail(json, "bad format");
        if (strcmp(key, "container") == 0 && !(seen & 1U)) {
            char *value = NULL;
            seen |= 1U;
            if (hwa_event_json_string(json, &value) != 0) { free(key); return -1; }
            format->container = hwa_event_container_parse(value);
            free(value);
        } else if (strcmp(key, "encoding") == 0 && !(seen & 2U)) {
            char *value = NULL;
            seen |= 2U;
            if (hwa_event_json_string(json, &value) != 0) { free(key); return -1; }
            format->encoding = hwa_event_encoding_parse(value);
            free(value);
        } else if (strcmp(key, "channels") == 0 && !(seen & 4U)) {
            uint64_t value;
            seen |= 4U;
            if (hwa_event_json_u64(json, &value) != 0 || value > UINT16_MAX) {
                free(key); return hwa_event_json_fail(json, "invalid channel count");
            }
            format->channels = (uint16_t)value;
        } else if (strcmp(key, "sample_rate_hz") == 0 && !(seen & 8U)) {
            uint64_t value;
            seen |= 8U;
            if (hwa_event_json_u64(json, &value) != 0 || value > UINT32_MAX) {
                free(key); return hwa_event_json_fail(json, "invalid sample rate");
            }
            format->sample_rate_hz = (uint32_t)value;
        } else if (strcmp(key, "bits_per_sample") == 0 && !(seen & 16U)) {
            uint64_t value;
            seen |= 16U;
            if (hwa_event_json_u64(json, &value) != 0 || value > UINT16_MAX) {
                free(key); return hwa_event_json_fail(json, "invalid bit depth");
            }
            format->bits_per_sample = (uint16_t)value;
        } else if (strcmp(key, "valid_bits_per_sample") == 0 && !(seen & 32U)) {
            uint64_t value;
            seen |= 32U;
            if (hwa_event_json_u64(json, &value) != 0 || value > UINT16_MAX) {
                free(key); return hwa_event_json_fail(json, "invalid valid bit depth");
            }
            format->valid_bits_per_sample = (uint16_t)value;
        } else if (strcmp(key, "block_align") == 0 && !(seen & 64U)) {
            uint64_t value;
            seen |= 64U;
            if (hwa_event_json_u64(json, &value) != 0 || value > UINT16_MAX) {
                free(key); return hwa_event_json_fail(json, "invalid block alignment");
            }
            format->block_align = (uint16_t)value;
        } else if (strcmp(key, "channel_mask") == 0 && !(seen & 128U)) {
            uint64_t value;
            seen |= 128U;
            if (hwa_event_json_u64(json, &value) != 0 || value > UINT32_MAX) {
                free(key); return hwa_event_json_fail(json, "invalid channel mask");
            }
            format->channel_mask = (uint32_t)value;
        } else if (strcmp(key, "frames") == 0 && !(seen & 256U)) {
            seen |= 256U;
            if (hwa_event_json_u64(json, &format->frames) != 0) { free(key); return -1; }
        } else if (strcmp(key, "data_bytes") == 0 && !(seen & 512U)) {
            seen |= 512U;
            if (hwa_event_json_u64(json, &format->data_bytes) != 0) { free(key); return -1; }
        } else if (strcmp(key, "duration_seconds") == 0 && !(seen & 1024U)) {
            seen |= 1024U;
            if (hwa_event_json_double(json, &format->duration_seconds) != 0) { free(key); return -1; }
        } else {
            free(key);
            return hwa_event_json_fail(json, "unknown or repeated format field");
        }
        free(key);
    }
    if (hwa_event_json_take(json, '}') != 0) return -1;
    hwa_event_json_depth_end(json);
    if (seen != 2047U || format->container == 0 || format->encoding == 0)
        return hwa_event_json_fail(json, "incomplete audio format");
    return 0;
}

static int hwa_event_parse_audio(HWAEventJson *json, HWAEventAudio *audio)
{
    unsigned seen = 0U;
    int first = 1;
    memset(audio, 0, sizeof(*audio));
    if (hwa_event_json_take(json, '{') != 0 ||
        hwa_event_json_depth_begin(json) != 0) return -1;
    while (!hwa_event_json_peek(json, '}')) {
        char *key = NULL;
        int state = hwa_event_json_key(json, &key, &first);
        if (state != 0) return state < 0 ? -1 : hwa_event_json_fail(json, "bad audio row");
        if (strcmp(key, "id") == 0 && !(seen & 1U)) {
            seen |= 1U;
            if (hwa_event_json_u64(json, &audio->id) != 0) { free(key); return -1; }
        } else if (strcmp(key, "kind") == 0 && !(seen & 2U)) {
            char *value = NULL;
            seen |= 2U;
            if (hwa_event_json_string(json, &value) != 0) { free(key); return -1; }
            audio->kind = hwa_event_audio_kind_parse(value);
            free(value);
        } else if (strcmp(key, "name") == 0 && !(seen & 4U)) {
            seen |= 4U;
            if (hwa_event_json_string(json, &audio->name) != 0) { free(key); return -1; }
        } else if (strcmp(key, "relative_path") == 0 && !(seen & 8U)) {
            seen |= 8U;
            if (hwa_event_json_string(json, &audio->relative_path) != 0) { free(key); return -1; }
        } else if (strcmp(key, "path_hint") == 0 && !(seen & 16U)) {
            seen |= 16U;
            if (hwa_event_json_string(json, &audio->path_hint) != 0) { free(key); return -1; }
        } else if (strcmp(key, "sha256") == 0 && !(seen & 32U)) {
            char *value = NULL;
            seen |= 32U;
            if (hwa_event_json_string(json, &value) != 0) { free(key); return -1; }
            if (strlen(value) != 64U) { free(value); free(key); return hwa_event_json_fail(json, "bad audio hash"); }
            memcpy(audio->sha256, value, 65U);
            free(value);
        } else if (strcmp(key, "file_bytes") == 0 && !(seen & 64U)) {
            seen |= 64U;
            if (hwa_event_json_u64(json, &audio->file_bytes) != 0) { free(key); return -1; }
        } else if (strcmp(key, "format") == 0 && !(seen & 128U)) {
            seen |= 128U;
            if (hwa_event_parse_format(json, &audio->format) != 0) { free(key); return -1; }
        } else if (strcmp(key, "source_recording_id") == 0 && !(seen & 256U)) {
            seen |= 256U;
            if (hwa_event_json_nullable_u64(json, &audio->source_recording_id,
                                            &audio->source_recording_id_valid) != 0) {
                free(key); return -1;
            }
        } else {
            free(key);
            return hwa_event_json_fail(json, "unknown or repeated audio field");
        }
        free(key);
    }
    if (hwa_event_json_take(json, '}') != 0) return -1;
    hwa_event_json_depth_end(json);
    if (seen != 511U || audio->kind == 0)
        return hwa_event_json_fail(json, "incomplete audio row");
    return 0;
}

static int hwa_event_append_audio(HWAEventBundle *bundle,
                                  const HWAEventBundleLimits *limits,
                                  HWAEventAudio *audio,
                                  size_t *capacity,
                                  HWAEventJson *json)
{
    void *storage;
    size_t next;
    if (bundle->audio_count >= limits->max_audio_files ||
        bundle->audio_count == SIZE_MAX)
        return hwa_event_json_fail(json, "audio row limit exceeded");
    next = bundle->audio_count + 1U;
    if (hwa_event_array_reserve(
            bundle->audio, capacity, next, limits->max_audio_files,
            sizeof(*bundle->audio), &storage, json->error, json->error_size,
            "cannot allocate audio rows") != 0)
        return -1;
    bundle->audio = (HWAEventAudio *)storage;
    bundle->audio[bundle->audio_count] = *audio;
    memset(audio, 0, sizeof(*audio));
    bundle->audio_count = next;
    return 0;
}

static int hwa_event_parse_audio_array(HWAEventJson *json,
                                       HWAEventBundle *bundle)
{
    void *storage;
    size_t capacity = 0U;
    int first = 1;
    if (hwa_event_json_take(json, '[') != 0 ||
        hwa_event_json_depth_begin(json) != 0) return -1;
    while (!hwa_event_json_peek(json, ']')) {
        HWAEventAudio audio;
        memset(&audio, 0, sizeof(audio));
        if (!first && hwa_event_json_take(json, ',') != 0) return -1;
        first = 0;
        if (hwa_event_parse_audio(json, &audio) != 0) {
            free(audio.name);
            free(audio.relative_path);
            free(audio.path_hint);
            return -1;
        }
        if (hwa_event_append_audio(bundle, json->limits, &audio, &capacity,
                                   json) != 0) {
            free(audio.name);
            free(audio.relative_path);
            free(audio.path_hint);
            return -1;
        }
    }
    if (hwa_event_json_take(json, ']') != 0) return -1;
    hwa_event_json_depth_end(json);
    if (hwa_event_array_shrink(
            bundle->audio, &capacity, bundle->audio_count,
            sizeof(*bundle->audio), &storage, json->error, json->error_size,
            "cannot finalize audio rows") != 0)
        return -1;
    bundle->audio = (HWAEventAudio *)storage;
    return 0;
}

static int hwa_event_parse_provider(HWAEventJson *json,
                                    HWAEventProvider *provider)
{
    unsigned seen = 0U;
    int first = 1;
    memset(provider, 0, sizeof(*provider));
    if (hwa_event_json_take(json, '{') != 0 ||
        hwa_event_json_depth_begin(json) != 0) return -1;
    while (!hwa_event_json_peek(json, '}')) {
        char *key = NULL;
        int state = hwa_event_json_key(json, &key, &first);
        if (state != 0) goto parse_error;
        if (strcmp(key, "id") == 0 && !(seen & 1U)) {
            seen |= 1U;
            if (hwa_event_json_u64(json, &provider->id) != 0) {
                free(key); goto parse_error;
            }
        } else if (strcmp(key, "name") == 0 && !(seen & 2U)) {
            seen |= 2U;
            if (hwa_event_json_string(json, &provider->name) != 0) {
                free(key); goto parse_error;
            }
        } else if (strcmp(key, "version") == 0 && !(seen & 4U)) {
            seen |= 4U;
            if (hwa_event_json_string(json, &provider->version) != 0) {
                free(key); goto parse_error;
            }
        } else if (strcmp(key, "model_sha256") == 0 && !(seen & 8U)) {
            char *value = NULL;
            seen |= 8U;
            if (hwa_event_json_string(json, &value) != 0) {
                free(key); goto parse_error;
            }
            if (strlen(value) >= HWA_SHA256_HEX_SIZE) {
                free(value); free(key);
                hwa_event_json_fail(json, "bad provider model hash");
                goto parse_error;
            }
            memcpy(provider->model_sha256, value, strlen(value) + 1U);
            free(value);
        } else if (strcmp(key, "settings") == 0 && !(seen & 16U)) {
            seen |= 16U;
            if (hwa_event_json_raw_object(json,
                                          &provider->settings_json) != 0) {
                free(key); goto parse_error;
            }
        } else {
            free(key);
            hwa_event_json_fail(json, "unknown or repeated provider field");
            goto parse_error;
        }
        free(key);
    }
    if (hwa_event_json_take(json, '}') != 0 || seen != 31U)
        goto parse_error;
    hwa_event_json_depth_end(json);
    return 0;
parse_error:
    free(provider->name);
    free(provider->version);
    free(provider->settings_json);
    memset(provider, 0, sizeof(*provider));
    return -1;
}

static int hwa_event_parse_providers(HWAEventJson *json,
                                     HWAEventBundle *bundle)
{
    void *storage;
    size_t capacity = 0U;
    int first = 1;
    if (hwa_event_json_take(json, '[') != 0 ||
        hwa_event_json_depth_begin(json) != 0) return -1;
    while (!hwa_event_json_peek(json, ']')) {
        HWAEventProvider provider;
        size_t next;
        if (!first && hwa_event_json_take(json, ',') != 0) return -1;
        first = 0;
        if (bundle->provider_count >= json->limits->max_providers ||
            bundle->provider_count == SIZE_MAX)
            return hwa_event_json_fail(json, "provider row limit exceeded");
        if (hwa_event_parse_provider(json, &provider) != 0) return -1;
        next = bundle->provider_count + 1U;
        if (hwa_event_array_reserve(
                bundle->providers, &capacity, next,
                json->limits->max_providers, sizeof(*bundle->providers),
                &storage, json->error, json->error_size,
                "cannot allocate providers") != 0) {
            free(provider.name);
            free(provider.version);
            free(provider.settings_json);
            return -1;
        }
        bundle->providers = (HWAEventProvider *)storage;
        bundle->providers[bundle->provider_count] = provider;
        bundle->provider_count = next;
    }
    if (hwa_event_json_take(json, ']') != 0) return -1;
    hwa_event_json_depth_end(json);
    if (hwa_event_array_shrink(
            bundle->providers, &capacity, bundle->provider_count,
            sizeof(*bundle->providers), &storage, json->error,
            json->error_size, "cannot finalize providers") != 0)
        return -1;
    bundle->providers = (HWAEventProvider *)storage;
    return 0;
}

static int hwa_event_parse_warning(HWAEventJson *json,
                                   HWAEventWarning *warning)
{
    unsigned seen = 0U;
    int first = 1;
    memset(warning, 0, sizeof(*warning));
    if (hwa_event_json_take(json, '{') != 0 ||
        hwa_event_json_depth_begin(json) != 0) return -1;
    while (!hwa_event_json_peek(json, '}')) {
        char *key = NULL;
        int state = hwa_event_json_key(json, &key, &first);
        if (state != 0) goto parse_error;
        if (strcmp(key, "id") == 0 && !(seen & 1U)) {
            seen |= 1U;
            if (hwa_event_json_u64(json, &warning->id) != 0) {
                free(key); goto parse_error;
            }
        } else if (strcmp(key, "code") == 0 && !(seen & 2U)) {
            seen |= 2U;
            if (hwa_event_json_string(json, &warning->code) != 0) {
                free(key); goto parse_error;
            }
        } else if (strcmp(key, "message") == 0 && !(seen & 4U)) {
            seen |= 4U;
            if (hwa_event_json_string(json, &warning->message) != 0) {
                free(key); goto parse_error;
            }
        } else if (strcmp(key, "event_id") == 0 && !(seen & 8U)) {
            seen |= 8U;
            if (hwa_event_json_nullable_u64(json, &warning->event_id,
                                            &warning->event_id_valid) != 0) {
                free(key); goto parse_error;
            }
        } else {
            free(key);
            hwa_event_json_fail(json, "unknown or repeated warning field");
            goto parse_error;
        }
        free(key);
    }
    if (hwa_event_json_take(json, '}') != 0 || seen != 15U)
        goto parse_error;
    hwa_event_json_depth_end(json);
    return 0;
parse_error:
    free(warning->code);
    free(warning->message);
    memset(warning, 0, sizeof(*warning));
    return -1;
}

static int hwa_event_parse_warnings(HWAEventJson *json,
                                    HWAEventBundle *bundle)
{
    void *storage;
    size_t capacity = 0U;
    int first = 1;
    if (hwa_event_json_take(json, '[') != 0 ||
        hwa_event_json_depth_begin(json) != 0) return -1;
    while (!hwa_event_json_peek(json, ']')) {
        HWAEventWarning warning;
        size_t next;
        if (!first && hwa_event_json_take(json, ',') != 0) return -1;
        first = 0;
        if (bundle->warning_count >= json->limits->max_warnings ||
            bundle->warning_count == SIZE_MAX)
            return hwa_event_json_fail(json, "warning row limit exceeded");
        if (hwa_event_parse_warning(json, &warning) != 0) return -1;
        next = bundle->warning_count + 1U;
        if (hwa_event_array_reserve(
                bundle->warnings, &capacity, next,
                json->limits->max_warnings, sizeof(*bundle->warnings),
                &storage, json->error, json->error_size,
                "cannot allocate warnings") != 0) {
            free(warning.code);
            free(warning.message);
            return -1;
        }
        bundle->warnings = (HWAEventWarning *)storage;
        bundle->warnings[bundle->warning_count] = warning;
        bundle->warning_count = next;
    }
    if (hwa_event_json_take(json, ']') != 0) return -1;
    hwa_event_json_depth_end(json);
    if (hwa_event_array_shrink(
            bundle->warnings, &capacity, bundle->warning_count,
            sizeof(*bundle->warnings), &storage, json->error,
            json->error_size, "cannot finalize warnings") != 0)
        return -1;
    bundle->warnings = (HWAEventWarning *)storage;
    return 0;
}

static int hwa_event_parse_counts(HWAEventJson *json,
                                  HWAEventManifestCounts *counts)
{
    unsigned seen = 0U;
    int first = 1;
    uint64_t value;
    if (hwa_event_json_take(json, '{') != 0 ||
        hwa_event_json_depth_begin(json) != 0) return -1;
    while (!hwa_event_json_peek(json, '}')) {
        char *key = NULL;
        int state = hwa_event_json_key(json, &key, &first);
        if (state != 0) return state < 0 ? -1 : hwa_event_json_fail(json, "bad counts");
        if (hwa_event_json_u64(json, &value) != 0) { free(key); return -1; }
        if (value > SIZE_MAX) { free(key); return hwa_event_json_fail(json, "count overflow"); }
        if (strcmp(key, "providers") == 0 && !(seen & 1U)) {
            seen |= 1U; counts->providers = (size_t)value;
        } else if (strcmp(key, "audio") == 0 && !(seen & 2U)) {
            seen |= 2U; counts->audio = (size_t)value;
        } else if (strcmp(key, "events") == 0 && !(seen & 4U)) {
            seen |= 4U; counts->events = (size_t)value;
        } else if (strcmp(key, "values") == 0 && !(seen & 8U)) {
            seen |= 8U; counts->values = (size_t)value;
        } else if (strcmp(key, "traces") == 0 && !(seen & 16U)) {
            seen |= 16U; counts->traces = (size_t)value;
        } else if (strcmp(key, "trace_refs") == 0 && !(seen & 32U)) {
            seen |= 32U; counts->trace_refs = (size_t)value;
        } else if (strcmp(key, "warnings") == 0 && !(seen & 64U)) {
            seen |= 64U; counts->warnings = (size_t)value;
        } else {
            free(key);
            return hwa_event_json_fail(json, "unknown or repeated count");
        }
        free(key);
    }
    if (hwa_event_json_take(json, '}') != 0) return -1;
    hwa_event_json_depth_end(json);
    return seen == 127U ? 0 : hwa_event_json_fail(json, "incomplete counts");
}

static int hwa_event_parse_file(HWAEventJson *json,
                                HWAEventManifestFiles *files)
{
    char *path = NULL;
    char *sha = NULL;
    uint64_t bytes = 0U;
    unsigned seen = 0U;
    int first = 1;
    HWAEventIndexFile *file = NULL;
    if (hwa_event_json_take(json, '{') != 0 ||
        hwa_event_json_depth_begin(json) != 0) return -1;
    while (!hwa_event_json_peek(json, '}')) {
        char *key = NULL;
        int state = hwa_event_json_key(json, &key, &first);
        if (state != 0) goto parse_error;
        if (strcmp(key, "relative_path") == 0 && !(seen & 1U)) {
            seen |= 1U;
            if (hwa_event_json_string(json, &path) != 0) { free(key); goto parse_error; }
        } else if (strcmp(key, "file_bytes") == 0 && !(seen & 2U)) {
            seen |= 2U;
            if (hwa_event_json_u64(json, &bytes) != 0) { free(key); goto parse_error; }
        } else if (strcmp(key, "sha256") == 0 && !(seen & 4U)) {
            seen |= 4U;
            if (hwa_event_json_string(json, &sha) != 0) { free(key); goto parse_error; }
        } else {
            free(key);
            hwa_event_json_fail(json, "unknown or repeated file field");
            goto parse_error;
        }
        free(key);
    }
    if (hwa_event_json_take(json, '}') != 0 || seen != 7U ||
        !hwa_event_hex_string(sha)) goto parse_error;
    hwa_event_json_depth_end(json);
    if (strcmp(path, "events.jsonl") == 0)
        file = &files->events;
    else if (strcmp(path, "traces.jsonl") == 0)
        file = &files->traces;
    if (file != NULL) {
        if (file->seen) {
            hwa_event_json_fail(json, "repeated index file");
            goto parse_error;
        }
        file->seen = 1;
        file->bytes = bytes;
        memcpy(file->sha256, sha, 65U);
    } else {
        if (!hwa_event_relative_path(path, "audio/") &&
            !hwa_event_relative_path(path, "traces/")) {
            hwa_event_json_fail(json, "unsafe event payload path");
            goto parse_error;
        }
        if (hwa_event_append_payload_file(files, path, bytes, sha,
                                          json->error,
                                          json->error_size) != 0)
            goto parse_error;
    }
    free(path); free(sha);
    return 0;
parse_error:
    free(path); free(sha);
    return -1;
}

static int hwa_event_parse_files(HWAEventJson *json,
                                 HWAEventManifestFiles *files)
{
    size_t maximum;
    int first = 1;
    if (json->limits->max_audio_files >
        SIZE_MAX - json->limits->max_traces)
        return hwa_event_json_fail(json, "file inventory limit overflows");
    maximum = json->limits->max_audio_files + json->limits->max_traces;
    if (hwa_event_json_take(json, '[') != 0 ||
        hwa_event_json_depth_begin(json) != 0) return -1;
    while (!hwa_event_json_peek(json, ']')) {
        if (!first && hwa_event_json_take(json, ',') != 0) return -1;
        first = 0;
        if (files->payload_count >= maximum)
            return hwa_event_json_fail(json,
                                       "file inventory limit exceeded");
        if (hwa_event_parse_file(json, files) != 0) return -1;
    }
    if (hwa_event_json_take(json, ']') != 0) return -1;
    hwa_event_json_depth_end(json);
    if (!files->events.seen || !files->traces.seen)
        return hwa_event_json_fail(json, "missing index file");
    if (hwa_event_finalize_payload_files(files, json->error,
                                         json->error_size) != 0)
        return -1;
    return 0;
}

static int hwa_event_parse_manifest(const unsigned char *data,
                                    size_t size,
                                    const HWAEventBundleLimits *limits,
                                    const HWANumericLocale *locale,
                                    HWAEventBundle *bundle,
                                    HWAEventManifestCounts *counts,
                                    HWAEventManifestFiles *files,
                                    char *error,
                                    size_t error_size)
{
    HWAEventJson json;
    unsigned seen = 0U;
    int first = 1;
    memset(&json, 0, sizeof(json));
    json.data = data; json.size = size; json.limits = limits;
    json.locale = locale; json.error = error; json.error_size = error_size;
    json.name = "event manifest";
    if (memchr(data, '\r', size) != NULL)
        return hwa_event_json_fail(&json,
                                   "manifest must use line-feed line endings");
    if (hwa_event_json_take(&json, '{') != 0 ||
        hwa_event_json_depth_begin(&json) != 0) return -1;
    while (!hwa_event_json_peek(&json, '}')) {
        char *key = NULL;
        int state = hwa_event_json_key(&json, &key, &first);
        if (state != 0) return -1;
        if (strcmp(key, "schema") == 0 && !(seen & 1U)) {
            char *value = NULL;
            seen |= 1U;
            if (hwa_event_json_string(&json, &value) != 0) { free(key); return -1; }
            if (strcmp(value, HWA_EVENT_BUNDLE_SCHEMA) != 0) {
                free(value); free(key);
                return hwa_event_json_fail(&json, "unsupported schema");
            }
            free(value);
        } else if (strcmp(key, "schema_version") == 0 && !(seen & 2U)) {
            uint64_t version;
            seen |= 2U;
            if (hwa_event_json_u64(&json, &version) != 0) { free(key); return -1; }
            if (version != HWA_EVENT_BUNDLE_SCHEMA_VERSION) {
                free(key);
                return hwa_event_json_fail(&json, "unsupported schema version");
            }
        } else if (strcmp(key, "tool_version") == 0 && !(seen & 4U)) {
            char *value = NULL;
            seen |= 4U;
            if (hwa_event_json_string(&json, &value) != 0) { free(key); return -1; }
            free(value);
        } else if (strcmp(key, "counts") == 0 && !(seen & 8U)) {
            seen |= 8U;
            if (hwa_event_parse_counts(&json, counts) != 0) { free(key); return -1; }
        } else if (strcmp(key, "providers") == 0 && !(seen & 16U)) {
            seen |= 16U;
            if (hwa_event_parse_providers(&json, bundle) != 0) {
                free(key); return -1;
            }
        } else if (strcmp(key, "audio") == 0 && !(seen & 32U)) {
            seen |= 32U;
            if (hwa_event_parse_audio_array(&json, bundle) != 0) { free(key); return -1; }
        } else if (strcmp(key, "warnings") == 0 && !(seen & 64U)) {
            seen |= 64U;
            if (hwa_event_parse_warnings(&json, bundle) != 0) {
                free(key); return -1;
            }
        } else if (strcmp(key, "files") == 0 && !(seen & 128U)) {
            seen |= 128U;
            if (hwa_event_parse_files(&json, files) != 0) { free(key); return -1; }
        } else {
            free(key);
            return hwa_event_json_fail(&json,
                                       "unknown or repeated manifest field");
        }
        free(key);
    }
    if (hwa_event_json_take(&json, '}') != 0) return -1;
    hwa_event_json_depth_end(&json);
    hwa_event_json_space(&json);
    if (json.offset != json.size || seen != 255U)
        return hwa_event_json_fail(&json, "incomplete manifest");
    return 0;
}

static int hwa_event_parse_value(HWAEventJson *json, HWAEventValue *value)
{
    unsigned seen = 0U;
    int first = 1;
    char *kind = NULL;
    char *basis = NULL;
    char *payload = NULL;
    memset(value, 0, sizeof(*value));
    if (hwa_event_json_take(json, '{') != 0 ||
        hwa_event_json_depth_begin(json) != 0) return -1;
    while (!hwa_event_json_peek(json, '}')) {
        char *key = NULL;
        int state = hwa_event_json_key(json, &key, &first);
        if (state != 0) goto parse_error;
        if (strcmp(key, "name") == 0 && !(seen & 1U)) {
            seen |= 1U;
            if (hwa_event_json_string(json, &value->name) != 0) { free(key); goto parse_error; }
        } else if (strcmp(key, "kind") == 0 && !(seen & 2U)) {
            seen |= 2U;
            if (hwa_event_json_string(json, &kind) != 0) { free(key); goto parse_error; }
        } else if (strcmp(key, "basis") == 0 && !(seen & 4U)) {
            seen |= 4U;
            if (hwa_event_json_string(json, &basis) != 0) { free(key); goto parse_error; }
        } else if (strcmp(key, "value") == 0 && !(seen & 8U)) {
            seen |= 8U;
            if (hwa_event_json_raw_value(json, &payload) != 0) {
                free(key); goto parse_error;
            }
        } else if (strcmp(key, "unit") == 0 && !(seen & 16U)) {
            seen |= 16U;
            if (hwa_event_json_string(json, &value->unit) != 0) { free(key); goto parse_error; }
        } else if (strcmp(key, "score") == 0 && !(seen & 32U)) {
            seen |= 32U;
            if (hwa_event_json_nullable_double(json, &value->score,
                                               &value->score_valid) != 0) {
                free(key); goto parse_error;
            }
        } else if (strcmp(key, "provider_id") == 0 && !(seen & 64U)) {
            seen |= 64U;
            if (hwa_event_json_nullable_u64(json, &value->provider_id,
                                            &value->provider_id_valid) != 0) {
                free(key); goto parse_error;
            }
        } else if (strcmp(key, "selected") == 0 && !(seen & 128U)) {
            seen |= 128U;
            if (hwa_event_json_bool(json, &value->selected) != 0) { free(key); goto parse_error; }
        } else {
            free(key); hwa_event_json_fail(json, "unknown or repeated value field"); goto parse_error;
        }
        free(key);
    }
    if (hwa_event_json_take(json, '}') != 0 || seen != 255U) goto parse_error;
    hwa_event_json_depth_end(json);
    value->kind = hwa_event_value_kind_parse(kind == NULL ? "" : kind);
    value->basis = hwa_event_value_basis_parse(basis == NULL ? "" : basis);
    if (value->kind == 0 || value->basis == 0) {
        hwa_event_json_fail(json, "unknown value kind or basis");
        goto parse_error;
    }
    {
        HWAEventJson payload_json;
        memset(&payload_json, 0, sizeof(payload_json));
        payload_json.data = (const unsigned char *)payload;
        payload_json.size = strlen(payload);
        payload_json.limits = json->limits;
        payload_json.locale = json->locale;
        payload_json.error = json->error;
        payload_json.error_size = json->error_size;
        payload_json.name = "event value payload";
        if ((value->kind == HWA_EVENT_VALUE_TEXT &&
             hwa_event_json_string(&payload_json, &value->text) != 0) ||
            (value->kind == HWA_EVENT_VALUE_F64 &&
             hwa_event_json_double(&payload_json, &value->number) != 0) ||
            (value->kind == HWA_EVENT_VALUE_I64 &&
             hwa_event_json_i64(&payload_json, &value->integer) != 0) ||
            (value->kind == HWA_EVENT_VALUE_BOOL &&
             hwa_event_json_bool(&payload_json, &value->boolean) != 0))
            goto parse_error;
        hwa_event_json_space(&payload_json);
        if (payload_json.offset != payload_json.size) {
            hwa_event_json_fail(&payload_json, "text follows typed value");
            goto parse_error;
        }
    }
    free(kind); free(basis); free(payload);
    return 0;
parse_error:
    free(kind); free(basis); free(payload);
    free(value->name); free(value->text); free(value->unit);
    memset(value, 0, sizeof(*value));
    return -1;
}

static int hwa_event_parse_values(HWAEventJson *json,
                                  HWAPerformanceEvent *event)
{
    void *storage;
    size_t capacity = 0U;
    int first = 1;
    if (hwa_event_json_take(json, '[') != 0 ||
        hwa_event_json_depth_begin(json) != 0) return -1;
    while (!hwa_event_json_peek(json, ']')) {
        HWAEventValue value;
        size_t next;
        if (!first && hwa_event_json_take(json, ',') != 0) return -1;
        first = 0;
        if (event->value_count >= json->limits->max_values ||
            event->value_count == SIZE_MAX)
            return hwa_event_json_fail(json, "value row limit exceeded");
        if (hwa_event_parse_value(json, &value) != 0) return -1;
        next = event->value_count + 1U;
        if (hwa_event_array_reserve(
                event->values, &capacity, next, json->limits->max_values,
                sizeof(*event->values), &storage, json->error,
                json->error_size,
                "cannot allocate event values") != 0) {
            free(value.name); free(value.text); free(value.unit);
            return -1;
        }
        event->values = (HWAEventValue *)storage;
        event->values[event->value_count] = value;
        event->value_count = next;
    }
    if (hwa_event_json_take(json, ']') != 0) return -1;
    hwa_event_json_depth_end(json);
    if (hwa_event_array_shrink(
            event->values, &capacity, event->value_count,
            sizeof(*event->values), &storage, json->error, json->error_size,
            "cannot finalize event values") != 0)
        return -1;
    event->values = (HWAEventValue *)storage;
    return 0;
}

static int hwa_event_parse_trace_ref(HWAEventJson *json,
                                     HWAEventTraceRef *ref)
{
    unsigned seen = 0U;
    int first = 1;
    memset(ref, 0, sizeof(*ref));
    if (hwa_event_json_take(json, '{') != 0 ||
        hwa_event_json_depth_begin(json) != 0) return -1;
    while (!hwa_event_json_peek(json, '}')) {
        char *key = NULL;
        int state = hwa_event_json_key(json, &key, &first);
        if (state != 0) goto parse_error;
        if (strcmp(key, "trace_id") == 0 && !(seen & 1U)) {
            seen |= 1U;
            if (hwa_event_json_u64(json, &ref->trace_id) != 0) {
                free(key); goto parse_error;
            }
        } else if (strcmp(key, "role") == 0 && !(seen & 2U)) {
            seen |= 2U;
            if (hwa_event_json_string(json, &ref->role) != 0) {
                free(key); goto parse_error;
            }
        } else if (strcmp(key, "first_point") == 0 && !(seen & 4U)) {
            seen |= 4U;
            if (hwa_event_json_u64(json, &ref->first_point) != 0) {
                free(key); goto parse_error;
            }
        } else if (strcmp(key, "point_count") == 0 && !(seen & 8U)) {
            seen |= 8U;
            if (hwa_event_json_u64(json, &ref->point_count) != 0) {
                free(key); goto parse_error;
            }
        } else {
            free(key);
            hwa_event_json_fail(json,
                                "unknown or repeated trace reference field");
            goto parse_error;
        }
        free(key);
    }
    if (hwa_event_json_take(json, '}') != 0 || seen != 15U)
        goto parse_error;
    hwa_event_json_depth_end(json);
    return 0;
parse_error:
    free(ref->role);
    memset(ref, 0, sizeof(*ref));
    return -1;
}

static int hwa_event_parse_trace_refs(HWAEventJson *json,
                                      HWAPerformanceEvent *event)
{
    void *storage;
    size_t capacity = 0U;
    int first = 1;
    if (hwa_event_json_take(json, '[') != 0 ||
        hwa_event_json_depth_begin(json) != 0) return -1;
    while (!hwa_event_json_peek(json, ']')) {
        HWAEventTraceRef ref;
        size_t next;
        if (!first && hwa_event_json_take(json, ',') != 0) return -1;
        first = 0;
        if (event->trace_ref_count >= json->limits->max_trace_refs ||
            event->trace_ref_count == SIZE_MAX)
            return hwa_event_json_fail(json,
                                       "trace reference row limit exceeded");
        if (hwa_event_parse_trace_ref(json, &ref) != 0) return -1;
        next = event->trace_ref_count + 1U;
        if (hwa_event_array_reserve(
                event->trace_refs, &capacity, next,
                json->limits->max_trace_refs, sizeof(*event->trace_refs),
                &storage, json->error, json->error_size,
                "cannot allocate trace references") != 0) {
            free(ref.role);
            return -1;
        }
        event->trace_refs = (HWAEventTraceRef *)storage;
        event->trace_refs[event->trace_ref_count] = ref;
        event->trace_ref_count = next;
    }
    if (hwa_event_json_take(json, ']') != 0) return -1;
    hwa_event_json_depth_end(json);
    if (hwa_event_array_shrink(
            event->trace_refs, &capacity, event->trace_ref_count,
            sizeof(*event->trace_refs), &storage, json->error,
            json->error_size, "cannot finalize trace references") != 0)
        return -1;
    event->trace_refs = (HWAEventTraceRef *)storage;
    return 0;
}

static int hwa_event_parse_event(HWAEventJson *json,
                                 HWAPerformanceEvent *event)
{
    unsigned seen = 0U;
    int first = 1;
    memset(event, 0, sizeof(*event));
    if (hwa_event_json_take(json, '{') != 0 ||
        hwa_event_json_depth_begin(json) != 0) return -1;
    while (!hwa_event_json_peek(json, '}')) {
        char *key = NULL;
        int state = hwa_event_json_key(json, &key, &first);
        if (state != 0) return -1;
        if (strcmp(key, "id") == 0 && !(seen & 1U)) {
            seen |= 1U;
            if (hwa_event_json_u64(json, &event->id) != 0) { free(key); return -1; }
        } else if (strcmp(key, "kind") == 0 && !(seen & 2U)) {
            seen |= 2U;
            if (hwa_event_json_string(json, &event->kind) != 0) { free(key); return -1; }
        } else if (strcmp(key, "source_recording_id") == 0 && !(seen & 4U)) {
            seen |= 4U;
            if (hwa_event_json_u64(json, &event->source_recording_id) != 0) { free(key); return -1; }
        } else if (strcmp(key, "evidence_audio_id") == 0 && !(seen & 8U)) {
            seen |= 8U;
            if (hwa_event_json_nullable_u64(json, &event->evidence_audio_id,
                                            &event->evidence_audio_id_valid) != 0) {
                free(key); return -1;
            }
        } else if (strcmp(key, "parent_id") == 0 && !(seen & 16U)) {
            seen |= 16U;
            if (hwa_event_json_nullable_u64(json, &event->parent_id,
                                            &event->parent_id_valid) != 0) {
                free(key); return -1;
            }
        } else if (strcmp(key, "start_sample") == 0 && !(seen & 32U)) {
            seen |= 32U;
            if (hwa_event_json_u64(json, &event->start_sample) != 0) { free(key); return -1; }
        } else if (strcmp(key, "end_sample") == 0 && !(seen & 64U)) {
            seen |= 64U;
            if (hwa_event_json_u64(json, &event->end_sample) != 0) { free(key); return -1; }
        } else if (strcmp(key, "voice") == 0 && !(seen & 128U)) {
            seen |= 128U;
            if (hwa_event_json_string(json, &event->voice) != 0) { free(key); return -1; }
        } else if (strcmp(key, "part") == 0 && !(seen & 256U)) {
            seen |= 256U;
            if (hwa_event_json_string(json, &event->part) != 0) { free(key); return -1; }
        } else if (strcmp(key, "score_event_id") == 0 && !(seen & 512U)) {
            seen |= 512U;
            if (hwa_event_json_string(json, &event->score_event_id) != 0) { free(key); return -1; }
        } else if (strcmp(key, "values") == 0 && !(seen & 1024U)) {
            seen |= 1024U;
            if (hwa_event_parse_values(json, event) != 0) { free(key); return -1; }
        } else if (strcmp(key, "trace_refs") == 0 && !(seen & 2048U)) {
            seen |= 2048U;
            if (hwa_event_parse_trace_refs(json, event) != 0) {
                free(key); return -1;
            }
        } else {
            free(key);
            return hwa_event_json_fail(json, "unknown or repeated event field");
        }
        free(key);
    }
    if (hwa_event_json_take(json, '}') != 0) return -1;
    hwa_event_json_depth_end(json);
    return seen == 4095U ? 0 : hwa_event_json_fail(json, "incomplete event row");
}

static int hwa_event_append_event(HWAEventBundle *bundle,
                                  const HWAEventBundleLimits *limits,
                                  HWAPerformanceEvent *event,
                                  size_t *capacity,
                                  HWAEventJson *json)
{
    void *storage;
    size_t next;
    if (bundle->event_count >= limits->max_events ||
        bundle->event_count == SIZE_MAX)
        return hwa_event_json_fail(json, "event row limit exceeded");
    next = bundle->event_count + 1U;
    if (hwa_event_array_reserve(
            bundle->events, capacity, next, limits->max_events,
            sizeof(*bundle->events), &storage, json->error, json->error_size,
            "cannot allocate events") != 0)
        return -1;
    bundle->events = (HWAPerformanceEvent *)storage;
    bundle->events[bundle->event_count] = *event;
    memset(event, 0, sizeof(*event));
    bundle->event_count = next;
    return 0;
}

static void hwa_event_clear_event(HWAPerformanceEvent *event)
{
    size_t index;
    if (event == NULL) return;
    free(event->kind);
    free(event->voice);
    free(event->part);
    free(event->score_event_id);
    for (index = 0U; index < event->value_count; ++index) {
        free(event->values[index].name);
        free(event->values[index].text);
        free(event->values[index].unit);
    }
    for (index = 0U; index < event->trace_ref_count; ++index)
        free(event->trace_refs[index].role);
    free(event->values);
    free(event->trace_refs);
    memset(event, 0, sizeof(*event));
}

static int hwa_event_parse_events(const unsigned char *data,
                                  size_t size,
                                  const HWAEventBundleLimits *limits,
                                  const HWANumericLocale *locale,
                                  HWAEventBundle *bundle,
                                  char *error,
                                  size_t error_size)
{
    size_t offset = 0U;
    size_t tokens = 0U;
    size_t capacity = 0U;
    void *storage;
    while (offset < size) {
        const unsigned char *newline =
            (const unsigned char *)memchr(data + offset, '\n', size - offset);
        size_t line_size;
        HWAEventJson json;
        HWAPerformanceEvent event;
        if (newline == NULL) {
            hwa_set_error(error, error_size,
                          "invalid events index: final line has no line feed");
            return -1;
        }
        line_size = (size_t)(newline - (data + offset));
        if (line_size == 0U || memchr(data + offset, '\r', line_size) != NULL ||
            (offset == 0U && line_size >= 3U && data[0] == 0xefU &&
             data[1] == 0xbbU && data[2] == 0xbfU)) {
            hwa_set_error(error, error_size,
                          "invalid events index line at byte %zu", offset);
            return -1;
        }
        memset(&json, 0, sizeof(json));
        json.data = data + offset; json.size = line_size;
        json.tokens = tokens; json.limits = limits; json.locale = locale;
        json.error = error; json.error_size = error_size;
        json.name = "events index line";
        if (hwa_event_parse_event(&json, &event) != 0) {
            hwa_event_clear_event(&event);
            return -1;
        }
        hwa_event_json_space(&json);
        if (json.offset != json.size) {
            hwa_event_clear_event(&event);
            return hwa_event_json_fail(&json,
                                       "text follows performance event");
        }
        if (hwa_event_append_event(bundle, limits, &event, &capacity,
                                   &json) != 0) {
            hwa_event_clear_event(&event);
            return -1;
        }
        tokens = json.tokens;
        offset += line_size + 1U;
    }
    if (hwa_event_array_shrink(
            bundle->events, &capacity, bundle->event_count,
            sizeof(*bundle->events), &storage, error, error_size,
            "cannot finalize events") != 0)
        return -1;
    bundle->events = (HWAPerformanceEvent *)storage;
    return 0;
}

static int hwa_event_parse_trace(HWAEventJson *json, HWAEventTrace *trace)
{
    unsigned seen = 0U;
    int first = 1;
    memset(trace, 0, sizeof(*trace));
    if (hwa_event_json_take(json, '{') != 0 ||
        hwa_event_json_depth_begin(json) != 0) return -1;
    while (!hwa_event_json_peek(json, '}')) {
        char *key = NULL;
        int state = hwa_event_json_key(json, &key, &first);
        if (state != 0) goto parse_error;
        if (strcmp(key, "id") == 0 && !(seen & 1U)) {
            seen |= 1U;
            if (hwa_event_json_u64(json, &trace->id) != 0) {
                free(key); goto parse_error;
            }
        } else if (strcmp(key, "name") == 0 && !(seen & 2U)) {
            seen |= 2U;
            if (hwa_event_json_string(json, &trace->name) != 0) {
                free(key); goto parse_error;
            }
        } else if (strcmp(key, "unit") == 0 && !(seen & 4U)) {
            seen |= 4U;
            if (hwa_event_json_string(json, &trace->unit) != 0) {
                free(key); goto parse_error;
            }
        } else if (strcmp(key, "relative_path") == 0 && !(seen & 8U)) {
            seen |= 8U;
            if (hwa_event_json_string(json, &trace->relative_path) != 0) {
                free(key); goto parse_error;
            }
        } else if (strcmp(key, "sha256") == 0 && !(seen & 16U)) {
            char *value = NULL;
            seen |= 16U;
            if (hwa_event_json_string(json, &value) != 0) {
                free(key); goto parse_error;
            }
            if (strlen(value) != 64U) {
                free(value); free(key);
                hwa_event_json_fail(json, "bad trace hash");
                goto parse_error;
            }
            memcpy(trace->sha256, value, 65U);
            free(value);
        } else if (strcmp(key, "format") == 0 && !(seen & 32U)) {
            char *value = NULL;
            seen |= 32U;
            if (hwa_event_json_string(json, &value) != 0) {
                free(key); goto parse_error;
            }
            trace->format = hwa_event_trace_format_parse(value);
            free(value);
        } else if (strcmp(key, "source_recording_id") == 0 &&
                   !(seen & 64U)) {
            seen |= 64U;
            if (hwa_event_json_u64(json,
                                   &trace->source_recording_id) != 0) {
                free(key); goto parse_error;
            }
        } else if (strcmp(key, "first_sample") == 0 && !(seen & 128U)) {
            seen |= 128U;
            if (hwa_event_json_u64(json, &trace->first_sample) != 0) {
                free(key); goto parse_error;
            }
        } else if (strcmp(key, "hop_samples") == 0 && !(seen & 256U)) {
            seen |= 256U;
            if (hwa_event_json_u64(json, &trace->hop_samples) != 0) {
                free(key); goto parse_error;
            }
        } else if (strcmp(key, "window_samples") == 0 &&
                   !(seen & 512U)) {
            seen |= 512U;
            if (hwa_event_json_u64(json, &trace->window_samples) != 0) {
                free(key); goto parse_error;
            }
        } else if (strcmp(key, "point_count") == 0 && !(seen & 1024U)) {
            seen |= 1024U;
            if (hwa_event_json_u64(json, &trace->point_count) != 0) {
                free(key); goto parse_error;
            }
        } else if (strcmp(key, "value_width") == 0 && !(seen & 2048U)) {
            uint64_t value;
            seen |= 2048U;
            if (hwa_event_json_u64(json, &value) != 0 ||
                value > UINT32_MAX) {
                free(key);
                hwa_event_json_fail(json, "invalid trace value width");
                goto parse_error;
            }
            trace->value_width = (uint32_t)value;
        } else if (strcmp(key, "file_bytes") == 0 && !(seen & 4096U)) {
            seen |= 4096U;
            if (hwa_event_json_u64(json, &trace->file_bytes) != 0) {
                free(key); goto parse_error;
            }
        } else {
            free(key);
            hwa_event_json_fail(json, "unknown or repeated trace field");
            goto parse_error;
        }
        free(key);
    }
    if (hwa_event_json_take(json, '}') != 0 || seen != 8191U ||
        trace->format == 0) goto parse_error;
    hwa_event_json_depth_end(json);
    return 0;
parse_error:
    free(trace->name);
    free(trace->unit);
    free(trace->relative_path);
    memset(trace, 0, sizeof(*trace));
    return -1;
}

static int hwa_event_append_trace(HWAEventBundle *bundle,
                                  const HWAEventBundleLimits *limits,
                                  HWAEventTrace *trace,
                                  size_t *capacity,
                                  HWAEventJson *json)
{
    void *storage;
    size_t next;
    if (bundle->trace_count >= limits->max_traces ||
        bundle->trace_count == SIZE_MAX)
        return hwa_event_json_fail(json, "trace row limit exceeded");
    next = bundle->trace_count + 1U;
    if (hwa_event_array_reserve(
            bundle->traces, capacity, next, limits->max_traces,
            sizeof(*bundle->traces), &storage, json->error, json->error_size,
            "cannot allocate traces") != 0)
        return -1;
    bundle->traces = (HWAEventTrace *)storage;
    bundle->traces[bundle->trace_count] = *trace;
    memset(trace, 0, sizeof(*trace));
    bundle->trace_count = next;
    return 0;
}

static int hwa_event_parse_traces(const unsigned char *data,
                                  size_t size,
                                  const HWAEventBundleLimits *limits,
                                  const HWANumericLocale *locale,
                                  HWAEventBundle *bundle,
                                  char *error,
                                  size_t error_size)
{
    size_t offset = 0U;
    size_t tokens = 0U;
    size_t capacity = 0U;
    void *storage;
    while (offset < size) {
        const unsigned char *newline =
            (const unsigned char *)memchr(data + offset, '\n', size - offset);
        size_t line_size;
        HWAEventJson json;
        HWAEventTrace trace;
        if (newline == NULL) {
            hwa_set_error(error, error_size,
                          "invalid traces index: final line has no line feed");
            return -1;
        }
        line_size = (size_t)(newline - (data + offset));
        if (line_size == 0U || memchr(data + offset, '\r', line_size) != NULL ||
            (offset == 0U && line_size >= 3U && data[0] == 0xefU &&
             data[1] == 0xbbU && data[2] == 0xbfU)) {
            hwa_set_error(error, error_size,
                          "invalid traces index line at byte %zu", offset);
            return -1;
        }
        memset(&json, 0, sizeof(json));
        json.data = data + offset; json.size = line_size;
        json.tokens = tokens; json.limits = limits; json.locale = locale;
        json.error = error; json.error_size = error_size;
        json.name = "traces index line";
        if (hwa_event_parse_trace(&json, &trace) != 0) return -1;
        hwa_event_json_space(&json);
        if (json.offset != json.size) {
            free(trace.name);
            free(trace.unit);
            free(trace.relative_path);
            return hwa_event_json_fail(&json,
                                       "text follows trace row");
        }
        if (hwa_event_append_trace(bundle, limits, &trace, &capacity,
                                   &json) != 0) {
            free(trace.name);
            free(trace.unit);
            free(trace.relative_path);
            return -1;
        }
        tokens = json.tokens;
        offset += line_size + 1U;
    }
    if (hwa_event_array_shrink(
            bundle->traces, &capacity, bundle->trace_count,
            sizeof(*bundle->traces), &storage, error, error_size,
            "cannot finalize traces") != 0)
        return -1;
    bundle->traces = (HWAEventTrace *)storage;
    return 0;
}

static int hwa_event_check_index(const char *path,
                                 const HWAEventIndexFile *file,
                                 uint64_t maximum,
                                 char *error,
                                 size_t error_size)
{
    uint64_t size;
    char sha256[HWA_SHA256_HEX_SIZE];
    if (hwa_event_file_size(path, &size) != 0 || size != file->bytes ||
        size > maximum ||
        hwa_sha256_file(path, maximum, sha256, error, error_size) != 0 ||
        strcmp(sha256, file->sha256) != 0) {
        hwa_set_error(error, error_size, "event index inventory mismatch");
        return -1;
    }
    return 0;
}

static void hwa_event_hash_data(const unsigned char *data,
                                size_t size,
                                char sha256[HWA_SHA256_HEX_SIZE])
{
    HWASha256 context;
    unsigned char digest[32];
    hwa_sha256_init(&context);
    hwa_sha256_update(&context, data, size);
    hwa_sha256_final(&context, digest);
    hwa_sha256_hex(digest, sha256);
}

static int hwa_event_check_index_data(const unsigned char *data,
                                      size_t size,
                                      const HWAEventIndexFile *file,
                                      char *error,
                                      size_t error_size)
{
    char sha256[HWA_SHA256_HEX_SIZE];
    if ((uint64_t)size != file->bytes) {
        hwa_set_error(error, error_size, "event index size changed while read");
        return -1;
    }
    hwa_event_hash_data(data, size, sha256);
    if (strcmp(sha256, file->sha256) != 0) {
        hwa_set_error(error, error_size, "event index changed while read");
        return -1;
    }
    return 0;
}

static int hwa_event_validate_trace_payload(
    const char *path,
    const HWAEventTrace *trace,
    const HWANumericLocale *locale,
    char *error,
    size_t error_size)
{
    FILE *stream = fopen(path, "rb");
    uint64_t point;
    if (stream == NULL) {
        hwa_set_error(error, error_size, "cannot open event trace payload");
        return -1;
    }
    if (trace->format == HWA_EVENT_TRACE_F64LE && sizeof(double) != 8U) {
        (void)fclose(stream);
        hwa_set_error(error, error_size,
                      "f64le event traces need 64-bit doubles");
        return -1;
    }
    if (trace->format == HWA_EVENT_TRACE_CSV_F64) {
        for (point = 0U; point < trace->point_count; ++point) {
            uint32_t column;
            for (column = 0U; column < trace->value_width; ++column) {
                char number[128];
                size_t length = 0U;
                int delimiter = column + 1U == trace->value_width ? '\n' : ',';
                int byte;
                double value;
                for (;;) {
                    byte = fgetc(stream);
                    if (byte == EOF || byte == ',' || byte == '\n' ||
                        byte == '\r') break;
                    if (length + 1U >= sizeof(number)) {
                        (void)fclose(stream);
                        hwa_set_error(error, error_size,
                                      "event trace number is too long");
                        return -1;
                    }
                    number[length++] = (char)byte;
                }
                number[length] = '\0';
                if (byte != delimiter ||
                    hwa_c_locale_parse_double(locale, number, &value) != 0) {
                    (void)fclose(stream);
                    hwa_set_error(error, error_size,
                                  "invalid csv-f64 event trace payload");
                    return -1;
                }
            }
        }
        if (fgetc(stream) != EOF || ferror(stream)) {
            (void)fclose(stream);
            hwa_set_error(error, error_size,
                          "csv-f64 event trace has extra data");
            return -1;
        }
    } else {
        uint64_t value_count = trace->point_count *
                               (uint64_t)trace->value_width;
        uint64_t index;
        for (index = 0U; index < value_count; ++index) {
            unsigned char bytes[8];
            uint64_t bits = 0U;
            double value;
            size_t byte_index;
            if (fread(bytes, 1U, sizeof(bytes), stream) != sizeof(bytes)) {
                (void)fclose(stream);
                hwa_set_error(error, error_size,
                              "short f64le event trace payload");
                return -1;
            }
            for (byte_index = 0U; byte_index < sizeof(bytes); ++byte_index)
                bits |= (uint64_t)bytes[byte_index] << (byte_index * 8U);
            memcpy(&value, &bits, sizeof(value));
            if (!isfinite(value)) {
                (void)fclose(stream);
                hwa_set_error(error, error_size,
                              "non-finite f64le event trace value");
                return -1;
            }
        }
        if (fgetc(stream) != EOF || ferror(stream)) {
            (void)fclose(stream);
            hwa_set_error(error, error_size,
                          "f64le event trace has extra data");
            return -1;
        }
    }
    if (fclose(stream) != 0) {
        hwa_set_error(error, error_size, "cannot close event trace payload");
        return -1;
    }
    return 0;
}

typedef struct HWAEventPayloadDescriptor {
    const char *relative_path;
    const char *sha256;
    uint64_t bytes;
    const HWAEventTrace *trace;
} HWAEventPayloadDescriptor;

static int hwa_event_payload_descriptor_compare(const void *left,
                                                const void *right)
{
    const HWAEventPayloadDescriptor *left_descriptor =
        (const HWAEventPayloadDescriptor *)left;
    const HWAEventPayloadDescriptor *right_descriptor =
        (const HWAEventPayloadDescriptor *)right;
    return strcmp(left_descriptor->relative_path,
                  right_descriptor->relative_path);
}

static int hwa_event_check_payloads(const char *directory,
                                    const HWAEventBundle *bundle,
                                    const HWAEventManifestFiles *files,
                                    const HWAEventBundleLimits *limits,
                                    const HWANumericLocale *locale,
                                    uint64_t *total_file_bytes,
                                    char *error,
                                    size_t error_size)
{
    HWAEventPayloadDescriptor *descriptors = NULL;
    size_t expected = bundle->trace_count;
    size_t index;
    size_t descriptor_index = 0U;
    int result = -1;
    for (index = 0U; index < bundle->audio_count; ++index) {
        const char *path = bundle->audio[index].relative_path;
        if (path != NULL && path[0] != '\0') {
            if (expected == SIZE_MAX) {
                hwa_set_error(error, error_size,
                              "event payload count overflow");
                goto cleanup;
            }
            expected++;
        }
    }
    if (files->payload_count != expected) {
        hwa_set_error(error, error_size,
                      "event payload inventory count mismatch");
        goto cleanup;
    }
    descriptors = (HWAEventPayloadDescriptor *)hwa_event_index_allocate(
        expected, sizeof(*descriptors), error, error_size);
    if (expected != 0U && descriptors == NULL) goto cleanup;
    for (index = 0U; index < bundle->audio_count; ++index) {
        const HWAEventAudio *audio = &bundle->audio[index];
        if (audio->relative_path == NULL || audio->relative_path[0] == '\0')
            continue;
        descriptors[descriptor_index].relative_path = audio->relative_path;
        descriptors[descriptor_index].sha256 = audio->sha256;
        descriptors[descriptor_index].bytes = audio->file_bytes;
        descriptors[descriptor_index].trace = NULL;
        descriptor_index++;
    }
    for (index = 0U; index < bundle->trace_count; ++index) {
        const HWAEventTrace *trace = &bundle->traces[index];
        descriptors[descriptor_index].relative_path = trace->relative_path;
        descriptors[descriptor_index].sha256 = trace->sha256;
        descriptors[descriptor_index].bytes = trace->file_bytes;
        descriptors[descriptor_index].trace = trace;
        descriptor_index++;
    }
    if (expected > 1U)
        qsort(descriptors, expected, sizeof(*descriptors),
              hwa_event_payload_descriptor_compare);
    for (index = 0U; index < expected; ++index) {
        const HWAEventPayloadFile *file = &files->payloads[index];
        const HWAEventPayloadDescriptor *descriptor = &descriptors[index];
        char path[HWA_EVENT_PATH_LIMIT];
        char sha256[HWA_SHA256_HEX_SIZE];
        uint64_t bytes;
        if (strcmp(descriptor->relative_path, file->relative_path) != 0 ||
            descriptor->bytes != file->bytes ||
            strcmp(descriptor->sha256, file->sha256) != 0 ||
            hwa_event_check_payload_parent(directory, file->relative_path,
                                           path, error, error_size) != 0 ||
            hwa_event_file_size(path, &bytes) != 0 || bytes != file->bytes ||
            hwa_sha256_file(path, limits->max_payload_file_bytes,
                            sha256, error, error_size) != 0 ||
            strcmp(sha256, file->sha256) != 0) {
            hwa_set_error(error, error_size,
                          "event payload inventory mismatch");
            goto cleanup;
        }
        if (descriptor->trace != NULL &&
            hwa_event_validate_trace_payload(path, descriptor->trace, locale,
                                             error, error_size) != 0)
            goto cleanup;
        if (*total_file_bytes > limits->max_bundle_bytes ||
            bytes > limits->max_bundle_bytes - *total_file_bytes) {
            hwa_set_error(error, error_size, "event bundle exceeds limit");
            goto cleanup;
        }
        *total_file_bytes += bytes;
    }
    result = 0;
cleanup:
    free(descriptors);
    return result;
}

static int hwa_event_work_add(uint64_t *total,
                              size_t count,
                              size_t item_size,
                              uint64_t maximum)
{
    uint64_t bytes;
    if (count != 0U && item_size > SIZE_MAX / count) return -1;
    if ((uintmax_t)(count * item_size) > UINT64_MAX) return -1;
    bytes = (uint64_t)(count * item_size);
    if (*total > maximum || bytes > maximum - *total) return -1;
    *total += bytes;
    return 0;
}

static int hwa_event_work_add_string(uint64_t *total,
                                     const char *text,
                                     uint64_t maximum)
{
    return text == NULL
               ? 0
               : hwa_event_work_add(total, strlen(text) + 1U, 1U, maximum);
}

static int hwa_event_bundle_measure_work(const HWAEventBundle *bundle,
                                         const HWAEventBundleLimits *limits,
                                         uint64_t *result,
                                         char *error,
                                         size_t error_size)
{
    uint64_t total = 0U;
    size_t index;
    if (hwa_event_work_add(&total, bundle->provider_count,
                           sizeof(*bundle->providers),
                           limits->max_work_bytes) != 0 ||
        hwa_event_work_add(&total, bundle->audio_count,
                           sizeof(*bundle->audio),
                           limits->max_work_bytes) != 0 ||
        hwa_event_work_add(&total, bundle->trace_count,
                           sizeof(*bundle->traces),
                           limits->max_work_bytes) != 0 ||
        hwa_event_work_add(&total, bundle->event_count,
                           sizeof(*bundle->events),
                           limits->max_work_bytes) != 0 ||
        hwa_event_work_add(&total, bundle->warning_count,
                           sizeof(*bundle->warnings),
                           limits->max_work_bytes) != 0)
        goto too_large;
    for (index = 0U; index < bundle->provider_count; ++index) {
        const HWAEventProvider *row = &bundle->providers[index];
        if (hwa_event_work_add_string(&total, row->name,
                                      limits->max_work_bytes) != 0 ||
            hwa_event_work_add_string(&total, row->version,
                                      limits->max_work_bytes) != 0 ||
            hwa_event_work_add_string(&total, row->settings_json,
                                      limits->max_work_bytes) != 0)
            goto too_large;
    }
    for (index = 0U; index < bundle->audio_count; ++index) {
        const HWAEventAudio *row = &bundle->audio[index];
        if (hwa_event_work_add_string(&total, row->name,
                                      limits->max_work_bytes) != 0 ||
            hwa_event_work_add_string(&total, row->relative_path,
                                      limits->max_work_bytes) != 0 ||
            hwa_event_work_add_string(&total, row->path_hint,
                                      limits->max_work_bytes) != 0)
            goto too_large;
    }
    for (index = 0U; index < bundle->trace_count; ++index) {
        const HWAEventTrace *row = &bundle->traces[index];
        if (hwa_event_work_add_string(&total, row->name,
                                      limits->max_work_bytes) != 0 ||
            hwa_event_work_add_string(&total, row->unit,
                                      limits->max_work_bytes) != 0 ||
            hwa_event_work_add_string(&total, row->relative_path,
                                      limits->max_work_bytes) != 0)
            goto too_large;
    }
    for (index = 0U; index < bundle->event_count; ++index) {
        const HWAPerformanceEvent *row = &bundle->events[index];
        size_t child;
        if (hwa_event_work_add_string(&total, row->kind,
                                      limits->max_work_bytes) != 0 ||
            hwa_event_work_add_string(&total, row->voice,
                                      limits->max_work_bytes) != 0 ||
            hwa_event_work_add_string(&total, row->part,
                                      limits->max_work_bytes) != 0 ||
            hwa_event_work_add_string(&total, row->score_event_id,
                                      limits->max_work_bytes) != 0 ||
            hwa_event_work_add(&total, row->value_count,
                               sizeof(*row->values),
                               limits->max_work_bytes) != 0 ||
            hwa_event_work_add(&total, row->trace_ref_count,
                               sizeof(*row->trace_refs),
                               limits->max_work_bytes) != 0)
            goto too_large;
        for (child = 0U; child < row->value_count; ++child) {
            const HWAEventValue *value = &row->values[child];
            if (hwa_event_work_add_string(&total, value->name,
                                          limits->max_work_bytes) != 0 ||
                hwa_event_work_add_string(&total, value->text,
                                          limits->max_work_bytes) != 0 ||
                hwa_event_work_add_string(&total, value->unit,
                                          limits->max_work_bytes) != 0)
                goto too_large;
        }
        for (child = 0U; child < row->trace_ref_count; ++child) {
            if (hwa_event_work_add_string(&total,
                                          row->trace_refs[child].role,
                                          limits->max_work_bytes) != 0)
                goto too_large;
        }
    }
    for (index = 0U; index < bundle->warning_count; ++index) {
        const HWAEventWarning *row = &bundle->warnings[index];
        if (hwa_event_work_add_string(&total, row->code,
                                      limits->max_work_bytes) != 0 ||
            hwa_event_work_add_string(&total, row->message,
                                      limits->max_work_bytes) != 0)
            goto too_large;
    }
    if (hwa_event_work_add_string(&total, bundle->directory,
                                  limits->max_work_bytes) != 0)
        goto too_large;
    *result = total;
    return 0;
too_large:
    hwa_set_error(error, error_size, "event bundle work exceeds limit");
    return -1;
}

void hwa_event_bundle_limits_default(HWAEventBundleLimits *limits)
{
    if (limits == NULL) return;
    memset(limits, 0, sizeof(*limits));
    limits->max_manifest_bytes = UINT64_C(8) * 1024U * 1024U;
    limits->max_index_bytes = UINT64_C(1024) * 1024U * 1024U;
    limits->max_payload_file_bytes = UINT64_C(16) * 1024U * 1024U * 1024U;
    limits->max_bundle_bytes = UINT64_C(64) * 1024U * 1024U * 1024U;
    limits->max_work_bytes = UINT64_C(1024) * 1024U * 1024U;
    limits->max_audio_files = 4096U;
    limits->max_events = 10000000U;
    limits->max_values = 100000000U;
    limits->max_traces = 100000U;
    limits->max_trace_refs = 100000000U;
    limits->max_providers = 4096U;
    limits->max_warnings = 100000U;
    limits->max_nesting_depth = 256U;
    limits->max_json_depth = 64U;
    limits->max_json_tokens = 100000000U;
}

void hwa_event_bundle_free(HWAEventBundle *bundle)
{
    size_t index;
    if (bundle == NULL) return;
    for (index = 0U; index < bundle->provider_count; ++index) {
        free(bundle->providers[index].name);
        free(bundle->providers[index].version);
        free(bundle->providers[index].settings_json);
    }
    for (index = 0U; index < bundle->audio_count; ++index) {
        free(bundle->audio[index].name);
        free(bundle->audio[index].relative_path);
        free(bundle->audio[index].path_hint);
    }
    for (index = 0U; index < bundle->trace_count; ++index) {
        free(bundle->traces[index].name);
        free(bundle->traces[index].unit);
        free(bundle->traces[index].relative_path);
    }
    for (index = 0U; index < bundle->event_count; ++index) {
        size_t value_index;
        size_t ref_index;
        HWAPerformanceEvent *event = &bundle->events[index];
        free(event->kind);
        free(event->voice);
        free(event->part);
        free(event->score_event_id);
        for (value_index = 0U; value_index < event->value_count; ++value_index) {
            free(event->values[value_index].name);
            free(event->values[value_index].text);
            free(event->values[value_index].unit);
        }
        for (ref_index = 0U; ref_index < event->trace_ref_count; ++ref_index)
            free(event->trace_refs[ref_index].role);
        free(event->values);
        free(event->trace_refs);
    }
    for (index = 0U; index < bundle->warning_count; ++index) {
        free(bundle->warnings[index].code);
        free(bundle->warnings[index].message);
    }
    free(bundle->directory);
    free(bundle->providers);
    free(bundle->audio);
    free(bundle->traces);
    free(bundle->events);
    free(bundle->warnings);
    memset(bundle, 0, sizeof(*bundle));
}

int hwa_event_bundle_validate(const HWAEventBundle *bundle,
                              const HWAEventBundleLimits *limits,
                              char *error,
                              size_t error_size)
{
    HWANumericLocale locale;
    uint64_t retained_work_bytes;
    size_t index;
    int result = -1;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (hwa_event_validate_bundle(bundle, limits, error, error_size) != 0)
        return -1;
    if (hwa_event_bundle_measure_work(bundle, limits,
                                      &retained_work_bytes,
                                      error, error_size) != 0)
        return -1;
    if (bundle->provider_count == 0U) return 0;
    memset(&locale, 0, sizeof(locale));
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_set_error(error, error_size, "cannot enter C numeric locale");
        return -1;
    }
    for (index = 0U; index < bundle->provider_count; ++index) {
        if (hwa_event_validate_settings_json(
                bundle->providers[index].settings_json, limits, &locale,
                error, error_size) != 0)
            goto cleanup;
    }
    result = 0;
cleanup:
    if (hwa_c_numeric_locale_end(&locale) != 0 && result == 0) {
        hwa_set_error(error, error_size, "cannot restore numeric locale");
        result = -1;
    }
    return result;
}

static int hwa_event_bundle_write_common(
    const char *output_directory,
    const HWAEventBundle *bundle,
    const HWAEventFileBinding *file_bindings,
    const HWAEventSourceBinding *source_bindings,
    size_t binding_count,
    int use_sources,
    const HWAEventBundleLimits *limits,
    char *error,
    size_t error_size)
{
    HWAEventManifestFiles files;
    HWAEventBundle verified;
    HWANumericLocale locale;
    char path[HWA_EVENT_PATH_LIMIT];
    int locale_active = 0;
    int result = -1;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    memset(&files, 0, sizeof(files));
    memset(&verified, 0, sizeof(verified));
    memset(&locale, 0, sizeof(locale));
    if (output_directory == NULL || output_directory[0] == '\0' ||
        limits == NULL ||
        (binding_count != 0U &&
         ((use_sources && source_bindings == NULL) ||
          (!use_sources && file_bindings == NULL)))) {
        hwa_set_error(error, error_size, "invalid event bundle write arguments");
        return -1;
    }
    if (hwa_event_bundle_validate(bundle, limits, error, error_size) != 0)
        return -1;
    if (HWA_EVENT_MKDIR(output_directory) != 0) {
        hwa_set_error(error, error_size,
                      errno == EEXIST ? "event bundle output already exists"
                                     : "cannot create event bundle directory");
        return -1;
    }
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_set_error(error, error_size, "cannot enter C numeric locale");
        goto cleanup;
    }
    locale_active = 1;
    if ((use_sources
             ? hwa_event_copy_source_payloads(
                   output_directory, bundle, source_bindings, binding_count,
                   limits, &locale, &files, error, error_size)
             : hwa_event_copy_payloads(
                   output_directory, bundle, file_bindings, binding_count,
                   limits, &locale, &files, error, error_size)) != 0 ||
        hwa_event_write_indexes(output_directory, bundle, &locale, &files,
                                error, error_size) != 0 ||
        hwa_event_write_manifest(output_directory, bundle, &files, limits,
                                 &locale, error, error_size) != 0)
        goto cleanup;
    if (hwa_event_bundle_read(output_directory, limits, &verified,
                              error, error_size) != 0)
        goto cleanup;
    hwa_event_bundle_free(&verified);
    result = 0;
cleanup:
    hwa_event_bundle_free(&verified);
    if (locale_active && hwa_c_numeric_locale_end(&locale) != 0 && result == 0) {
        hwa_set_error(error, error_size, "cannot restore numeric locale");
        result = -1;
    }
    if (result != 0) {
        size_t index;
        for (index = 0U; index < files.payload_count; ++index) {
            if (hwa_event_join(path, output_directory,
                               files.payloads[index].relative_path) == 0)
                (void)HWA_EVENT_UNLINK(path);
        }
        for (index = files.payload_count; index > 0U; --index)
            hwa_event_remove_payload_parent(
                output_directory, files.payloads[index - 1U].relative_path);
        for (index = bundle->trace_count; index > 0U; --index)
            hwa_event_remove_payload_parent(
                output_directory, bundle->traces[index - 1U].relative_path);
        for (index = bundle->audio_count; index > 0U; --index) {
            const char *relative_path =
                bundle->audio[index - 1U].relative_path;
            if (relative_path != NULL && relative_path[0] != '\0')
                hwa_event_remove_payload_parent(output_directory,
                                                relative_path);
        }
        if (hwa_event_join(path, output_directory, "manifest.json") == 0)
            (void)HWA_EVENT_UNLINK(path);
        if (hwa_event_join(path, output_directory, "events.jsonl") == 0)
            (void)HWA_EVENT_UNLINK(path);
        if (hwa_event_join(path, output_directory, "traces.jsonl") == 0)
            (void)HWA_EVENT_UNLINK(path);
        (void)HWA_EVENT_RMDIR(output_directory);
    }
    hwa_event_manifest_files_free(&files);
    return result;
}

int hwa_event_bundle_write(const char *output_directory,
                           const HWAEventBundle *bundle,
                           const HWAEventFileBinding *bindings,
                           size_t binding_count,
                           const HWAEventBundleLimits *limits,
                           char *error,
                           size_t error_size)
{
    return hwa_event_bundle_write_common(
        output_directory, bundle, bindings, NULL, binding_count, 0,
        limits, error, error_size);
}

int hwa_event_bundle_write_sources(
    const char *output_directory,
    const HWAEventBundle *bundle,
    const HWAEventSourceBinding *bindings,
    size_t binding_count,
    const HWAEventBundleLimits *limits,
    char *error,
    size_t error_size)
{
    return hwa_event_bundle_write_common(
        output_directory, bundle, NULL, bindings, binding_count, 1,
        limits, error, error_size);
}

int hwa_event_bundle_read(const char *directory,
                          const HWAEventBundleLimits *limits,
                          HWAEventBundle *bundle,
                          char *error,
                          size_t error_size)
{
    unsigned char *manifest_data = NULL;
    unsigned char *events_data = NULL;
    unsigned char *traces_data = NULL;
    size_t manifest_size = 0U;
    size_t events_size = 0U;
    size_t traces_size = 0U;
    HWAEventManifestCounts counts;
    HWAEventManifestFiles files;
    HWANumericLocale locale;
    char path[HWA_EVENT_PATH_LIMIT];
    int locale_active = 0;
    int result = -1;
    if (bundle == NULL) return -1;
    memset(bundle, 0, sizeof(*bundle));
    if (error != NULL && error_size != 0U) error[0] = '\0';
    memset(&counts, 0, sizeof(counts));
    memset(&files, 0, sizeof(files));
    memset(&locale, 0, sizeof(locale));
    if (directory == NULL || directory[0] == '\0' || limits == NULL) {
        hwa_set_error(error, error_size, "invalid event bundle read arguments");
        return -1;
    }
    if (hwa_event_join(path, directory, "manifest.json") != 0 ||
        hwa_event_read_file(path, limits->max_manifest_bytes,
                            &manifest_data, &manifest_size,
                            error, error_size) != 0)
        goto cleanup;
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_set_error(error, error_size, "cannot enter C numeric locale");
        goto cleanup;
    }
    locale_active = 1;
    if (hwa_event_parse_manifest(manifest_data, manifest_size, limits, &locale,
                                 bundle, &counts, &files,
                                 error, error_size) != 0)
        goto cleanup;
    if (counts.providers != bundle->provider_count ||
        counts.audio != bundle->audio_count ||
        counts.warnings != bundle->warning_count) {
        hwa_set_error(error, error_size, "event manifest count mismatch");
        goto cleanup;
    }
    if (hwa_event_join(path, directory, "events.jsonl") != 0 ||
        hwa_event_check_index(path, &files.events, limits->max_index_bytes,
                              error, error_size) != 0 ||
        hwa_event_read_file(path, limits->max_index_bytes,
                            &events_data, &events_size,
                            error, error_size) != 0 ||
        hwa_event_check_index_data(events_data, events_size, &files.events,
                                   error, error_size) != 0 ||
        hwa_event_parse_events(events_data, events_size, limits, &locale,
                               bundle, error, error_size) != 0)
        goto cleanup;
    if (hwa_event_join(path, directory, "traces.jsonl") != 0 ||
        hwa_event_check_index(path, &files.traces, limits->max_index_bytes,
                              error, error_size) != 0 ||
        hwa_event_read_file(path, limits->max_index_bytes,
                            &traces_data, &traces_size,
                            error, error_size) != 0 ||
        hwa_event_check_index_data(traces_data, traces_size, &files.traces,
                                   error, error_size) != 0 ||
        hwa_event_parse_traces(traces_data, traces_size, limits, &locale,
                               bundle, error, error_size) != 0)
        goto cleanup;
    {
        size_t value_count = 0U;
        size_t trace_ref_count = 0U;
        size_t index;
        for (index = 0U; index < bundle->event_count; ++index) {
            if (SIZE_MAX - value_count < bundle->events[index].value_count ||
                SIZE_MAX - trace_ref_count <
                    bundle->events[index].trace_ref_count) {
                hwa_set_error(error, error_size,
                              "event child count overflow");
                goto cleanup;
            }
            value_count += bundle->events[index].value_count;
            trace_ref_count += bundle->events[index].trace_ref_count;
        }
        if (counts.events != bundle->event_count ||
            counts.values != value_count ||
            counts.traces != bundle->trace_count ||
            counts.trace_refs != trace_ref_count) {
            hwa_set_error(error, error_size, "event row count mismatch");
            goto cleanup;
        }
    }
    if (hwa_event_bundle_validate(bundle, limits, error, error_size) != 0)
        goto cleanup;
    bundle->directory = hwa_event_copy(directory);
    if (bundle->directory == NULL) {
        hwa_set_error(error, error_size, "cannot copy event bundle path");
        goto cleanup;
    }
    if (hwa_event_bundle_measure_work(bundle, limits,
                                      &bundle->retained_work_bytes,
                                      error, error_size) != 0)
        goto cleanup;
    hwa_event_hash_data(manifest_data, manifest_size,
                        bundle->manifest_sha256);
    bundle->total_file_bytes = (uint64_t)manifest_size;
    if (bundle->total_file_bytes > limits->max_bundle_bytes ||
        files.events.bytes >
            limits->max_bundle_bytes - bundle->total_file_bytes) {
        hwa_set_error(error, error_size, "event bundle exceeds limit");
        goto cleanup;
    }
    bundle->total_file_bytes += files.events.bytes;
    if (files.traces.bytes >
        limits->max_bundle_bytes - bundle->total_file_bytes) {
        hwa_set_error(error, error_size, "event bundle exceeds limit");
        goto cleanup;
    }
    bundle->total_file_bytes += files.traces.bytes;
    if (hwa_event_check_payloads(directory, bundle, &files, limits, &locale,
                                 &bundle->total_file_bytes,
                                 error, error_size) != 0)
        goto cleanup;
    result = 0;
cleanup:
    free(manifest_data);
    free(events_data);
    free(traces_data);
    hwa_event_manifest_files_free(&files);
    if (locale_active && hwa_c_numeric_locale_end(&locale) != 0 && result == 0) {
        hwa_set_error(error, error_size, "cannot restore numeric locale");
        result = -1;
    }
    if (result != 0) hwa_event_bundle_free(bundle);
    return result;
}

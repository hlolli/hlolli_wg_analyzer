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

#include "experiment.h"

#include "experiment_file.h"
#include "internal.h"
#include "numeric_locale.h"
#include "run.h"
#include "run_file.h"
#include "sha256.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#define HWA_PATH_SEPARATOR '\\'
#else
#include <dirent.h>
#include <fcntl.h>
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define HWA_PATH_SEPARATOR '/'
#endif

#if defined(__APPLE__)
extern int renameatx_np(int from_directory, const char *from,
                        int to_directory, const char *to,
                        unsigned int flags);
#ifndef RENAME_EXCL
#define RENAME_EXCL 0x00000004U
#endif
#endif

#if !defined(O_NOFOLLOW)
#define O_NOFOLLOW 0
#endif

#define HWA_EXPERIMENT_MAX_JSON_DEPTH 12U
#define HWA_EXPERIMENT_MAX_JSON_TOKENS 262144U
#define HWA_EXPERIMENT_FLAT_THRESHOLD 0.000001
#define HWA_EXPERIMENT_NOISE_THRESHOLD 0.02
#define HWA_EXPERIMENT_HARM_THRESHOLD 0.02
#define HWA_EXPERIMENT_MAX_RESOURCES_PER_CASE 144U

typedef struct HWAExperimentStemTemplate {
    char *id;
    HWARunSide side;
    HWARunStemRole role;
    char *input_id;
    char *output;
    int64_t start_sample;
    double gain_db;
    uint32_t rate_hz;
    uint16_t channels;
} HWAExperimentStemTemplate;

typedef struct HWAExperimentProbeTemplate {
    char *id;
    HWARunSide side;
    char *name;
    char *unit;
    HWARunProbeFormat format;
    char *input_id;
    char *output;
    int64_t start_sample;
    uint64_t rate_numerator;
    uint64_t rate_denominator;
    uint64_t value_count;
} HWAExperimentProbeTemplate;

typedef struct HWAExperimentLinkTemplate {
    char *stem_id;
    char *probe_id;
    HWARunFeatureKind feature;
    uint32_t feature_index;
} HWAExperimentLinkTemplate;

typedef struct HWAExperimentCaseTemplate {
    HWAExperimentStemTemplate *stems;
    size_t stem_count;
    size_t stem_capacity;
    HWAExperimentProbeTemplate *probes;
    size_t probe_count;
    size_t probe_capacity;
    HWAExperimentLinkTemplate *links;
    size_t link_count;
    size_t link_capacity;
} HWAExperimentCaseTemplate;

typedef struct HWAExperimentManifest {
    uint32_t clock_rate_hz;
    HWAExperimentInput *inputs;
    size_t input_count;
    size_t input_capacity;
    HWAExperimentParameter *parameters;
    size_t parameter_count;
    size_t parameter_capacity;
    HWAExperimentLevel *levels;
    size_t level_count;
    size_t level_capacity;
    HWAExperimentCase *cases;
    HWAExperimentCaseTemplate *case_templates;
    size_t case_count;
    size_t case_capacity;
    HWAExperimentResponse *responses;
    size_t response_count;
    size_t response_capacity;
    HWAExperimentPlanKind plan_kind;
    uint64_t plan_seed;
    size_t sample_count;
    size_t replicates;
} HWAExperimentManifest;

static size_t hwa_experiment_case_output_count(
    const HWAExperimentCaseTemplate *item);

typedef struct HWAExperimentJson {
    const unsigned char *data;
    size_t size;
    size_t offset;
    size_t tokens;
    size_t depth;
    uint64_t max_work;
    uint64_t *live_work;
    const HWANumericLocale *locale;
    char *error;
    size_t error_size;
} HWAExperimentJson;

typedef struct HWAExperimentBlob {
    unsigned char *data;
    size_t size;
    char sha256[HWA_SHA256_HEX_SIZE];
} HWAExperimentBlob;

typedef struct HWAExperimentFileIdentity {
    uint64_t device;
    uint64_t inode;
    uint64_t size;
    int valid;
} HWAExperimentFileIdentity;

typedef struct HWAExperimentName {
    int value;
    const char *name;
} HWAExperimentName;

static const HWAExperimentName hwa_experiment_plan_names[] = {
    {HWA_EXPERIMENT_ONE_AT_A_TIME, "one-at-a-time"},
    {HWA_EXPERIMENT_GRID, "grid"},
    {HWA_EXPERIMENT_RANDOM, "random"}
};

static const HWAExperimentName hwa_experiment_split_names[] = {
    {HWA_EXPERIMENT_FIT, "fit"},
    {HWA_EXPERIMENT_CHECK, "check"}
};

static const HWAExperimentName hwa_experiment_monotonicity_names[] = {
    {HWA_EXPERIMENT_MONOTONIC_NONE, "none"},
    {HWA_EXPERIMENT_MONOTONIC_FLAT, "flat"},
    {HWA_EXPERIMENT_MONOTONIC_INCREASING, "increasing"},
    {HWA_EXPERIMENT_MONOTONIC_DECREASING, "decreasing"},
    {HWA_EXPERIMENT_MONOTONIC_MIXED, "mixed"}
};

static const char *hwa_experiment_name_for(const HWAExperimentName *names,
                                           size_t count,
                                           int value)
{
    size_t index;
    for (index = 0U; index < count; ++index)
        if (names[index].value == value) return names[index].name;
    return "unknown";
}

const char *hwa_experiment_plan_name(HWAExperimentPlanKind value)
{
    return hwa_experiment_name_for(
        hwa_experiment_plan_names,
        sizeof(hwa_experiment_plan_names) / sizeof(hwa_experiment_plan_names[0]),
        (int)value);
}

const char *hwa_experiment_split_name(HWAExperimentSplit value)
{
    return hwa_experiment_name_for(
        hwa_experiment_split_names,
        sizeof(hwa_experiment_split_names) /
            sizeof(hwa_experiment_split_names[0]),
        (int)value);
}

const char *hwa_experiment_monotonicity_name(
    HWAExperimentMonotonicity value)
{
    return hwa_experiment_name_for(
        hwa_experiment_monotonicity_names,
        sizeof(hwa_experiment_monotonicity_names) /
            sizeof(hwa_experiment_monotonicity_names[0]),
        (int)value);
}

static char *hwa_experiment_copy_string(const char *text)
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

static int hwa_experiment_u64_add(uint64_t left,
                                  uint64_t right,
                                  uint64_t *sum)
{
    if (sum == NULL || left > UINT64_MAX - right) return -1;
    *sum = left + right;
    return 0;
}

static int hwa_experiment_u64_multiply(uint64_t left,
                                       uint64_t right,
                                       uint64_t *product)
{
    if (product == NULL || (left != 0U && right > UINT64_MAX / left))
        return -1;
    *product = left * right;
    return 0;
}

static uint64_t hwa_experiment_gcd_u64(uint64_t left, uint64_t right)
{
    while (right != 0U) {
        uint64_t remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

static int hwa_experiment_work_add(uint64_t *live,
                                   uint64_t amount,
                                   uint64_t cap,
                                   char *error,
                                   size_t error_size)
{
    if (live == NULL || amount > cap || *live > cap - amount) {
        hwa_set_error(error, error_size, "Stage 8 work cap exceeded");
        return -1;
    }
    *live += amount;
    return 0;
}

static int hwa_experiment_string_bytes(const char *text, uint64_t *bytes)
{
    uint64_t amount;
    if (text == NULL) return 0;
    amount = (uint64_t)strlen(text) + UINT64_C(1);
    return hwa_experiment_u64_add(*bytes, amount, bytes);
}

static int hwa_experiment_array_bytes(size_t count,
                                      size_t size,
                                      uint64_t *bytes)
{
    uint64_t amount;
    if (hwa_experiment_u64_multiply((uint64_t)count, (uint64_t)size,
                                    &amount) != 0)
        return -1;
    return hwa_experiment_u64_add(*bytes, amount, bytes);
}

static int hwa_experiment_boolean(int value)
{
    return value == 0 || value == 1;
}

static int hwa_experiment_hash_valid(const char *text)
{
    size_t index;
    if (text == NULL || strlen(text) != 64U) return 0;
    for (index = 0U; index < 64U; ++index)
        if (!((text[index] >= '0' && text[index] <= '9') ||
              (text[index] >= 'a' && text[index] <= 'f'))) return 0;
    return 1;
}

static int hwa_experiment_token_valid(const char *text, int namespaced)
{
    size_t index;
    size_t length;
    int has_namespace = 0;
    unsigned char first;
    if (text == NULL || text[0] == '\0') return 0;
    length = strlen(text);
    if (length > 127U) return 0;
    first = (unsigned char)text[0];
    if (!((first >= '0' && first <= '9') ||
          (first >= 'A' && first <= 'Z') ||
          (first >= 'a' && first <= 'z'))) return 0;
    for (index = 0U; index < length; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'A' && value <= 'Z') ||
              (value >= 'a' && value <= 'z') || value == '.' ||
              value == '_' || value == ':' || value == '/' || value == '-'))
            return 0;
        if (value == '.' || value == ':' || value == '/') has_namespace = 1;
    }
    return !namespaced || has_namespace;
}

static int hwa_experiment_unit_valid(const char *text)
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

static int hwa_experiment_component_valid(const char *text)
{
    size_t index;
    size_t length;
    size_t stem_length;
    static const char *const devices[] = {
        "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4",
        "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
        "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    if (text == NULL || text[0] == '\0' || strcmp(text, ".") == 0 ||
        strcmp(text, "..") == 0) return 0;
    length = strlen(text);
    if (length > 127U || text[length - 1U] == '.' ||
        text[length - 1U] == ' ') return 0;
    for (index = 0U; index < length; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'A' && value <= 'Z') ||
              (value >= 'a' && value <= 'z') || value == '.' ||
              value == '_' || value == '-')) return 0;
    }
    stem_length = strcspn(text, ".");
    for (index = 0U; index < sizeof(devices) / sizeof(devices[0]); ++index) {
        size_t scan;
        size_t device_length = strlen(devices[index]);
        if (stem_length != device_length) continue;
        for (scan = 0U; scan < stem_length; ++scan) {
            unsigned char value = (unsigned char)text[scan];
            if (value >= 'a' && value <= 'z') value -= 'a' - 'A';
            if (value != (unsigned char)devices[index][scan])
                break;
        }
        if (scan == stem_length) return 0;
    }
    return 1;
}

static int hwa_experiment_ascii_case_equal(const char *left,
                                            const char *right)
{
    size_t index;
    if (left == NULL || right == NULL) return 0;
    for (index = 0U; left[index] != '\0' && right[index] != '\0'; ++index) {
        unsigned char left_value = (unsigned char)left[index];
        unsigned char right_value = (unsigned char)right[index];
        if (left_value >= 'A' && left_value <= 'Z') left_value += 'a' - 'A';
        if (right_value >= 'A' && right_value <= 'Z') right_value += 'a' - 'A';
        if (left_value != right_value) return 0;
    }
    return left[index] == right[index];
}

static int hwa_experiment_output_name_valid(const char *text)
{
    static const char *const reserved[] = {
        "request.json", "stdout.txt", "stderr.txt", "run.json",
        "result.hwa-run"
    };
    size_t index;
    if (!hwa_experiment_component_valid(text)) return 0;
    for (index = 0U; index < sizeof(reserved) / sizeof(reserved[0]); ++index)
        if (hwa_experiment_ascii_case_equal(text, reserved[index])) return 0;
    return 1;
}

static int hwa_experiment_json_fail(HWAExperimentJson *json,
                                    const char *message)
{
    hwa_set_error(json->error, json->error_size,
                  "invalid Stage 8 manifest at byte %llu: %s",
                  (unsigned long long)json->offset, message);
    return -1;
}

static void hwa_experiment_json_space(HWAExperimentJson *json)
{
    while (json->offset < json->size) {
        unsigned char value = json->data[json->offset];
        if (value != ' ' && value != '\t' && value != '\r' && value != '\n')
            break;
        json->offset++;
    }
}

static int hwa_experiment_json_token(HWAExperimentJson *json)
{
    if (json->tokens >= HWA_EXPERIMENT_MAX_JSON_TOKENS)
        return hwa_experiment_json_fail(json, "token limit exceeded");
    json->tokens++;
    return 0;
}

static int hwa_experiment_json_take(HWAExperimentJson *json,
                                    unsigned char wanted)
{
    hwa_experiment_json_space(json);
    if (json->offset >= json->size || json->data[json->offset] != wanted)
        return hwa_experiment_json_fail(json, "unexpected token");
    json->offset++;
    return 0;
}

static int hwa_experiment_json_depth_begin(HWAExperimentJson *json)
{
    if (json->depth >= HWA_EXPERIMENT_MAX_JSON_DEPTH)
        return hwa_experiment_json_fail(json, "nesting limit exceeded");
    json->depth++;
    return 0;
}

static int hwa_experiment_json_hex(unsigned char value)
{
    if (value >= '0' && value <= '9') return (int)(value - '0');
    if (value >= 'a' && value <= 'f') return 10 + (int)(value - 'a');
    if (value >= 'A' && value <= 'F') return 10 + (int)(value - 'A');
    return -1;
}

static int hwa_experiment_json_string(HWAExperimentJson *json, char **out)
{
    size_t scan;
    size_t decoded_size = 0U;
    size_t write = 0U;
    char *decoded;
    if (out == NULL) return hwa_experiment_json_fail(json, "missing string target");
    *out = NULL;
    hwa_experiment_json_space(json);
    if (hwa_experiment_json_token(json) != 0 ||
        json->offset >= json->size || json->data[json->offset] != '"')
        return hwa_experiment_json_fail(json, "string expected");
    scan = ++json->offset;
    while (scan < json->size && json->data[scan] != '"') {
        unsigned char value = json->data[scan++];
        if (value < 0x20U || value >= 0x80U)
            return hwa_experiment_json_fail(json, "strings must be ASCII");
        if (value == '\\') {
            unsigned char escaped;
            if (scan >= json->size)
                return hwa_experiment_json_fail(json, "bad escape");
            escaped = json->data[scan++];
            if (escaped == 'u') {
                int a;
                int b;
                int c;
                int d;
                unsigned code;
                if (scan + 4U > json->size ||
                    (a = hwa_experiment_json_hex(json->data[scan])) < 0 ||
                    (b = hwa_experiment_json_hex(json->data[scan + 1U])) < 0 ||
                    (c = hwa_experiment_json_hex(json->data[scan + 2U])) < 0 ||
                    (d = hwa_experiment_json_hex(json->data[scan + 3U])) < 0)
                    return hwa_experiment_json_fail(json, "bad Unicode escape");
                code = ((unsigned)a << 12U) | ((unsigned)b << 8U) |
                       ((unsigned)c << 4U) | (unsigned)d;
                if (code == 0U || code >= 0x80U)
                    return hwa_experiment_json_fail(json, "non-ASCII escape");
                scan += 4U;
            } else if (strchr("\"\\/bfnrt", (int)escaped) == NULL) {
                return hwa_experiment_json_fail(json, "bad escape");
            }
        }
        if (decoded_size == SIZE_MAX)
            return hwa_experiment_json_fail(json, "string too long");
        decoded_size++;
    }
    if (scan >= json->size)
        return hwa_experiment_json_fail(json, "unterminated string");
    if (hwa_experiment_work_add(json->live_work,
                                (uint64_t)decoded_size + UINT64_C(1),
                                json->max_work, json->error,
                                json->error_size) != 0) return -1;
    decoded = (char *)malloc(decoded_size + 1U);
    if (decoded == NULL) {
        *json->live_work -= (uint64_t)decoded_size + UINT64_C(1);
        return hwa_experiment_json_fail(json, "cannot allocate string");
    }
    while (json->offset < scan) {
        unsigned char value = json->data[json->offset++];
        if (value == '\\') {
            unsigned char escaped = json->data[json->offset++];
            if (escaped == 'u') {
                int a = hwa_experiment_json_hex(json->data[json->offset]);
                int b = hwa_experiment_json_hex(json->data[json->offset + 1U]);
                int c = hwa_experiment_json_hex(json->data[json->offset + 2U]);
                int d = hwa_experiment_json_hex(json->data[json->offset + 3U]);
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

static void hwa_experiment_json_string_free(HWAExperimentJson *json,
                                            char **text)
{
    uint64_t bytes;
    if (json == NULL || text == NULL || *text == NULL) return;
    bytes = (uint64_t)strlen(*text) + UINT64_C(1);
    free(*text);
    *text = NULL;
    if (bytes <= *json->live_work) *json->live_work -= bytes;
}

static int hwa_experiment_json_number_text(HWAExperimentJson *json,
                                           char text[128],
                                           int integer_only)
{
    size_t start;
    size_t length;
    hwa_experiment_json_space(json);
    if (hwa_experiment_json_token(json) != 0) return -1;
    start = json->offset;
    if (json->offset < json->size && json->data[json->offset] == '-')
        json->offset++;
    if (json->offset >= json->size)
        return hwa_experiment_json_fail(json, "number expected");
    if (json->data[json->offset] == '0') {
        json->offset++;
        if (json->offset < json->size &&
            json->data[json->offset] >= '0' &&
            json->data[json->offset] <= '9')
            return hwa_experiment_json_fail(json, "leading zero");
    } else {
        if (json->data[json->offset] < '0' ||
            json->data[json->offset] > '9')
            return hwa_experiment_json_fail(json, "number expected");
        while (json->offset < json->size &&
               json->data[json->offset] >= '0' &&
               json->data[json->offset] <= '9') json->offset++;
    }
    if (!integer_only && json->offset < json->size &&
        json->data[json->offset] == '.') {
        json->offset++;
        if (json->offset >= json->size ||
            (json->data[json->offset] < '0' ||
             json->data[json->offset] > '9'))
            return hwa_experiment_json_fail(json, "fraction digit expected");
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
        if (json->offset >= json->size ||
            (json->data[json->offset] < '0' ||
             json->data[json->offset] > '9'))
            return hwa_experiment_json_fail(json, "exponent digit expected");
        while (json->offset < json->size &&
               json->data[json->offset] >= '0' &&
               json->data[json->offset] <= '9') json->offset++;
    }
    length = json->offset - start;
    if (length == 0U || length >= 128U)
        return hwa_experiment_json_fail(json, "number too long");
    memcpy(text, json->data + start, length);
    text[length] = '\0';
    return 0;
}

static int hwa_experiment_json_u64(HWAExperimentJson *json, uint64_t *value)
{
    char text[128];
    size_t index;
    uint64_t parsed = 0U;
    if (value == NULL || hwa_experiment_json_number_text(json, text, 1) != 0)
        return -1;
    if (text[0] == '-')
        return hwa_experiment_json_fail(json, "unsigned integer expected");
    for (index = 0U; text[index] != '\0'; ++index) {
        unsigned digit = (unsigned)(text[index] - '0');
        if (parsed > (UINT64_MAX - digit) / UINT64_C(10))
            return hwa_experiment_json_fail(json, "integer overflow");
        parsed = parsed * UINT64_C(10) + digit;
    }
    *value = parsed;
    return 0;
}

static int hwa_experiment_json_i64(HWAExperimentJson *json, int64_t *value)
{
    char text[128];
    char *end = NULL;
    long long parsed;
    if (value == NULL || hwa_experiment_json_number_text(json, text, 1) != 0)
        return -1;
    errno = 0;
    parsed = strtoll(text, &end, 10);
    if (errno != 0 || end == NULL || *end != '\0')
        return hwa_experiment_json_fail(json, "integer overflow");
    *value = (int64_t)parsed;
    return 0;
}

static int hwa_experiment_json_double(HWAExperimentJson *json, double *value)
{
    char text[128];
    if (value == NULL || hwa_experiment_json_number_text(json, text, 0) != 0)
        return -1;
    if (hwa_c_locale_parse_double(json->locale, text, value) != 0 ||
        !isfinite(*value))
        return hwa_experiment_json_fail(json, "finite number expected");
    if (*value == 0.0) *value = 0.0;
    return 0;
}

static int hwa_experiment_json_null(HWAExperimentJson *json)
{
    hwa_experiment_json_space(json);
    if (hwa_experiment_json_token(json) != 0 ||
        json->offset + 4U > json->size ||
        memcmp(json->data + json->offset, "null", 4U) != 0)
        return hwa_experiment_json_fail(json, "null expected");
    json->offset += 4U;
    return 0;
}

static int hwa_experiment_json_key(HWAExperimentJson *json,
                                   char **key,
                                   uint64_t *seen,
                                   unsigned bit)
{
    uint64_t mask;
    if (bit >= 64U) return -1;
    mask = UINT64_C(1) << bit;
    if ((*seen & mask) != 0U) {
        hwa_experiment_json_string_free(json, key);
        return hwa_experiment_json_fail(json, "duplicate object key");
    }
    *seen |= mask;
    if (hwa_experiment_json_take(json, ':') != 0) {
        hwa_experiment_json_string_free(json, key);
        return -1;
    }
    return 0;
}

static int hwa_experiment_grow(HWAExperimentJson *json,
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
    if (count >= maximum) return hwa_experiment_json_fail(json, "row limit exceeded");
    next = *capacity == 0U ? 4U : *capacity * 2U;
    if (next < *capacity || next > maximum) next = maximum;
    if (element_size != 0U && next > SIZE_MAX / element_size)
        return hwa_experiment_json_fail(json, "array too large");
    old_bytes = (uint64_t)(*capacity * element_size);
    new_bytes = (uint64_t)(next * element_size);
    if (hwa_experiment_work_add(json->live_work, new_bytes, json->max_work,
                                json->error, json->error_size) != 0) return -1;
    grown = calloc(next, element_size);
    if (grown == NULL) {
        *json->live_work -= new_bytes;
        return hwa_experiment_json_fail(json, "cannot allocate array");
    }
    if (old_bytes != 0U) memcpy(grown, *array, (size_t)old_bytes);
    free(*array);
    if (old_bytes <= *json->live_work) *json->live_work -= old_bytes;
    *array = grown;
    *capacity = next;
    return 0;
}

static int hwa_experiment_parse_side(const char *text, HWARunSide *side)
{
    if (strcmp(text, "reference") == 0) *side = HWA_RUN_REFERENCE;
    else if (strcmp(text, "model") == 0) *side = HWA_RUN_MODEL;
    else return -1;
    return 0;
}

static int hwa_experiment_parse_role(const char *text, HWARunStemRole *role)
{
    size_t index;
    for (index = 1U; index < (size_t)HWA_RUN_STEM_ROLE_COUNT; ++index) {
        if (strcmp(text, hwa_run_stem_role_name((HWARunStemRole)index)) == 0) {
            *role = (HWARunStemRole)index;
            return 0;
        }
    }
    return -1;
}

static int hwa_experiment_parse_feature(const char *text,
                                        HWARunFeatureKind *feature)
{
    size_t index;
    for (index = 1U; index < (size_t)HWA_RUN_FEATURE_KIND_COUNT; ++index) {
        if (strcmp(text,
                   hwa_run_feature_kind_name((HWARunFeatureKind)index)) == 0) {
            *feature = (HWARunFeatureKind)index;
            return 0;
        }
    }
    return -1;
}

static int hwa_experiment_parse_format(const char *text,
                                       HWARunProbeFormat *format)
{
    if (strcmp(text, "csv-f64") == 0) *format = HWA_RUN_PROBE_CSV_F64;
    else if (strcmp(text, "binary-f64le") == 0)
        *format = HWA_RUN_PROBE_BINARY_F64LE;
    else return -1;
    return 0;
}

static int hwa_experiment_parse_split(const char *text,
                                      HWAExperimentSplit *split)
{
    if (strcmp(text, "fit") == 0) *split = HWA_EXPERIMENT_FIT;
    else if (strcmp(text, "check") == 0) *split = HWA_EXPERIMENT_CHECK;
    else return -1;
    return 0;
}

static int hwa_experiment_parse_plan_kind(const char *text,
                                          HWAExperimentPlanKind *kind)
{
    if (strcmp(text, "one-at-a-time") == 0)
        *kind = HWA_EXPERIMENT_ONE_AT_A_TIME;
    else if (strcmp(text, "grid") == 0) *kind = HWA_EXPERIMENT_GRID;
    else if (strcmp(text, "random") == 0) *kind = HWA_EXPERIMENT_RANDOM;
    else return -1;
    return 0;
}

static void hwa_experiment_case_free(HWAExperimentCase *record,
                                     HWAExperimentCaseTemplate *item)
{
    size_t nested;
    if (record != NULL) {
        free(record->name);
        memset(record, 0, sizeof(*record));
    }
    if (item == NULL) return;
    for (nested = 0U; nested < item->stem_count; ++nested) {
        free(item->stems[nested].id);
        free(item->stems[nested].input_id);
        free(item->stems[nested].output);
    }
    for (nested = 0U; nested < item->probe_count; ++nested) {
        free(item->probes[nested].id);
        free(item->probes[nested].name);
        free(item->probes[nested].unit);
        free(item->probes[nested].input_id);
        free(item->probes[nested].output);
    }
    for (nested = 0U; nested < item->link_count; ++nested) {
        free(item->links[nested].stem_id);
        free(item->links[nested].probe_id);
    }
    free(item->stems);
    free(item->probes);
    free(item->links);
    memset(item, 0, sizeof(*item));
}

static void hwa_experiment_manifest_free(HWAExperimentManifest *manifest)
{
    size_t index;
    if (manifest == NULL) return;
    if (manifest->inputs != NULL)
        for (index = 0U; index < manifest->input_count; ++index)
            free(manifest->inputs[index].binding_id);
    if (manifest->parameters != NULL)
        for (index = 0U; index < manifest->parameter_count; ++index) {
            free(manifest->parameters[index].name);
            free(manifest->parameters[index].unit);
        }
    for (index = 0U; index < manifest->case_count; ++index)
        hwa_experiment_case_free(
            manifest->cases == NULL ? NULL : &manifest->cases[index],
            manifest->case_templates == NULL ? NULL :
                &manifest->case_templates[index]);
    if (manifest->responses != NULL)
        for (index = 0U; index < manifest->response_count; ++index)
            free(manifest->responses[index].name);
    free(manifest->inputs);
    free(manifest->parameters);
    free(manifest->levels);
    free(manifest->cases);
    free(manifest->case_templates);
    free(manifest->responses);
    memset(manifest, 0, sizeof(*manifest));
}

static int hwa_experiment_parse_string_or_null(HWAExperimentJson *json,
                                               char **value)
{
    hwa_experiment_json_space(json);
    if (json->offset < json->size && json->data[json->offset] == 'n') {
        *value = NULL;
        return hwa_experiment_json_null(json);
    }
    return hwa_experiment_json_string(json, value);
}

static int hwa_experiment_parse_input(HWAExperimentJson *json,
                                      HWAExperimentInput *input)
{
    uint64_t seen = 0U;
    char *key = NULL;
    char *hash = NULL;
    int first = 1;
    memset(input, 0, sizeof(*input));
    if (hwa_experiment_json_take(json, '{') != 0 ||
        hwa_experiment_json_depth_begin(json) != 0) return -1;
    for (;;) {
        hwa_experiment_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == '}') {
            json->offset++;
            break;
        }
        if (!first && hwa_experiment_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_experiment_json_string(json, &key) != 0) goto failed;
        if (strcmp(key, "id") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 0U) != 0 ||
                hwa_experiment_json_string(json, &input->binding_id) != 0)
                goto failed;
        } else if (strcmp(key, "sha256") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 1U) != 0 ||
                hwa_experiment_json_string(json, &hash) != 0) goto failed;
        } else {
            hwa_experiment_json_fail(json, "unknown input key");
            goto failed;
        }
        hwa_experiment_json_string_free(json, &key);
    }
    json->depth--;
    if (seen != UINT64_C(3) || !hwa_experiment_token_valid(input->binding_id, 0) ||
        !hwa_experiment_hash_valid(hash)) {
        hwa_experiment_json_fail(json, "incomplete or invalid input");
        goto failed_no_depth;
    }
    memcpy(input->sha256, hash, HWA_SHA256_HEX_SIZE);
    hwa_experiment_json_string_free(json, &hash);
    return 0;
failed:
    hwa_experiment_json_string_free(json, &key);
    json->depth--;
failed_no_depth:
    hwa_experiment_json_string_free(json, &hash);
    hwa_experiment_json_string_free(json, &input->binding_id);
    memset(input, 0, sizeof(*input));
    return -1;
}

static int hwa_experiment_parse_levels(HWAExperimentJson *json,
                                       HWAExperimentManifest *manifest,
                                       HWAExperimentParameter *parameter)
{
    int first = 1;
    parameter->first_level = manifest->level_count;
    if (hwa_experiment_json_take(json, '[') != 0 ||
        hwa_experiment_json_depth_begin(json) != 0) return -1;
    for (;;) {
        double value;
        hwa_experiment_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == ']') {
            json->offset++;
            break;
        }
        if (!first && hwa_experiment_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_experiment_grow(json, (void **)&manifest->levels,
                                &manifest->level_capacity,
                                manifest->level_count, SIZE_MAX,
                                sizeof(*manifest->levels)) != 0 ||
            hwa_experiment_json_double(json, &value) != 0) goto failed;
        manifest->levels[manifest->level_count].value = value;
        manifest->level_count++;
        parameter->level_count++;
    }
    json->depth--;
    return 0;
failed:
    json->depth--;
    return -1;
}

static int hwa_experiment_parse_parameter(HWAExperimentJson *json,
                                          HWAExperimentManifest *manifest,
                                          HWAExperimentParameter *parameter)
{
    uint64_t seen = 0U;
    char *key = NULL;
    int first = 1;
    memset(parameter, 0, sizeof(*parameter));
    if (hwa_experiment_json_take(json, '{') != 0 ||
        hwa_experiment_json_depth_begin(json) != 0) return -1;
    for (;;) {
        hwa_experiment_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == '}') {
            json->offset++;
            break;
        }
        if (!first && hwa_experiment_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_experiment_json_string(json, &key) != 0) goto failed;
        if (strcmp(key, "id") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 0U) != 0 ||
                hwa_experiment_json_string(json, &parameter->name) != 0)
                goto failed;
        } else if (strcmp(key, "unit") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 1U) != 0 ||
                hwa_experiment_json_string(json, &parameter->unit) != 0)
                goto failed;
        } else if (strcmp(key, "minimum") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 2U) != 0 ||
                hwa_experiment_json_double(json, &parameter->minimum) != 0)
                goto failed;
        } else if (strcmp(key, "maximum") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 3U) != 0 ||
                hwa_experiment_json_double(json, &parameter->maximum) != 0)
                goto failed;
        } else if (strcmp(key, "baseline") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 4U) != 0 ||
                hwa_experiment_json_double(json, &parameter->baseline) != 0)
                goto failed;
        } else if (strcmp(key, "levels") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 5U) != 0 ||
                hwa_experiment_parse_levels(json, manifest, parameter) != 0)
                goto failed;
        } else {
            hwa_experiment_json_fail(json, "unknown parameter key");
            goto failed;
        }
        hwa_experiment_json_string_free(json, &key);
    }
    json->depth--;
    if (seen != UINT64_C(0x3f) ||
        !hwa_experiment_token_valid(parameter->name, 0) ||
        !hwa_experiment_unit_valid(parameter->unit) ||
        parameter->minimum > parameter->baseline ||
        parameter->baseline > parameter->maximum) {
        hwa_experiment_json_fail(json, "incomplete or invalid parameter");
        goto failed_no_depth;
    }
    return 0;
failed:
    hwa_experiment_json_string_free(json, &key);
    json->depth--;
failed_no_depth:
    hwa_experiment_json_string_free(json, &parameter->name);
    hwa_experiment_json_string_free(json, &parameter->unit);
    memset(parameter, 0, sizeof(*parameter));
    return -1;
}

static int hwa_experiment_parse_stem(HWAExperimentJson *json,
                                     HWAExperimentStemTemplate *stem)
{
    uint64_t seen = 0U;
    char *key = NULL;
    int first = 1;
    memset(stem, 0, sizeof(*stem));
    if (hwa_experiment_json_take(json, '{') != 0 ||
        hwa_experiment_json_depth_begin(json) != 0) return -1;
    for (;;) {
        hwa_experiment_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == '}') {
            json->offset++;
            break;
        }
        if (!first && hwa_experiment_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_experiment_json_string(json, &key) != 0) goto failed;
        if (strcmp(key, "id") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 0U) != 0 ||
                hwa_experiment_json_string(json, &stem->id) != 0) goto failed;
        } else if (strcmp(key, "side") == 0) {
            char *value = NULL;
            if (hwa_experiment_json_key(json, &key, &seen, 1U) != 0 ||
                hwa_experiment_json_string(json, &value) != 0 ||
                hwa_experiment_parse_side(value, &stem->side) != 0) {
                hwa_experiment_json_string_free(json, &value);
                goto failed;
            }
            hwa_experiment_json_string_free(json, &value);
        } else if (strcmp(key, "role") == 0) {
            char *value = NULL;
            if (hwa_experiment_json_key(json, &key, &seen, 2U) != 0 ||
                hwa_experiment_json_string(json, &value) != 0 ||
                hwa_experiment_parse_role(value, &stem->role) != 0) {
                hwa_experiment_json_string_free(json, &value);
                goto failed;
            }
            hwa_experiment_json_string_free(json, &value);
        } else if (strcmp(key, "input_id") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 3U) != 0 ||
                hwa_experiment_parse_string_or_null(json, &stem->input_id) != 0)
                goto failed;
        } else if (strcmp(key, "output") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 4U) != 0 ||
                hwa_experiment_parse_string_or_null(json, &stem->output) != 0)
                goto failed;
        } else if (strcmp(key, "start_sample") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 5U) != 0 ||
                hwa_experiment_json_i64(json, &stem->start_sample) != 0)
                goto failed;
        } else if (strcmp(key, "gain_db") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 6U) != 0 ||
                hwa_experiment_json_double(json, &stem->gain_db) != 0)
                goto failed;
        } else if (strcmp(key, "rate_hz") == 0) {
            uint64_t value;
            if (hwa_experiment_json_key(json, &key, &seen, 7U) != 0 ||
                hwa_experiment_json_u64(json, &value) != 0 ||
                value > UINT32_MAX) goto failed;
            stem->rate_hz = (uint32_t)value;
        } else if (strcmp(key, "channels") == 0) {
            uint64_t value;
            if (hwa_experiment_json_key(json, &key, &seen, 8U) != 0 ||
                hwa_experiment_json_u64(json, &value) != 0 ||
                value > UINT16_MAX) goto failed;
            stem->channels = (uint16_t)value;
        } else {
            hwa_experiment_json_fail(json, "unknown case stem key");
            goto failed;
        }
        hwa_experiment_json_string_free(json, &key);
    }
    json->depth--;
    if (seen != UINT64_C(0x1ff) || !hwa_experiment_token_valid(stem->id, 0) ||
        ((stem->input_id != NULL) == (stem->output != NULL)) ||
        (stem->input_id != NULL &&
         !hwa_experiment_token_valid(stem->input_id, 0)) ||
        (stem->output != NULL &&
         !hwa_experiment_output_name_valid(stem->output)) ||
        stem->rate_hz < 8000U || stem->channels == 0U) {
        hwa_experiment_json_fail(json, "incomplete or invalid case stem");
        goto failed_no_depth;
    }
    return 0;
failed:
    hwa_experiment_json_string_free(json, &key);
    json->depth--;
failed_no_depth:
    hwa_experiment_json_string_free(json, &stem->id);
    hwa_experiment_json_string_free(json, &stem->input_id);
    hwa_experiment_json_string_free(json, &stem->output);
    memset(stem, 0, sizeof(*stem));
    return -1;
}

static int hwa_experiment_parse_probe(HWAExperimentJson *json,
                                      HWAExperimentProbeTemplate *probe)
{
    uint64_t seen = 0U;
    char *key = NULL;
    int first = 1;
    memset(probe, 0, sizeof(*probe));
    if (hwa_experiment_json_take(json, '{') != 0 ||
        hwa_experiment_json_depth_begin(json) != 0) return -1;
    for (;;) {
        hwa_experiment_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == '}') {
            json->offset++;
            break;
        }
        if (!first && hwa_experiment_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_experiment_json_string(json, &key) != 0) goto failed;
        if (strcmp(key, "id") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 0U) != 0 ||
                hwa_experiment_json_string(json, &probe->id) != 0) goto failed;
        } else if (strcmp(key, "side") == 0) {
            char *value = NULL;
            if (hwa_experiment_json_key(json, &key, &seen, 1U) != 0 ||
                hwa_experiment_json_string(json, &value) != 0 ||
                hwa_experiment_parse_side(value, &probe->side) != 0) {
                hwa_experiment_json_string_free(json, &value);
                goto failed;
            }
            hwa_experiment_json_string_free(json, &value);
        } else if (strcmp(key, "name") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 2U) != 0 ||
                hwa_experiment_json_string(json, &probe->name) != 0) goto failed;
        } else if (strcmp(key, "unit") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 3U) != 0 ||
                hwa_experiment_json_string(json, &probe->unit) != 0) goto failed;
        } else if (strcmp(key, "format") == 0) {
            char *value = NULL;
            if (hwa_experiment_json_key(json, &key, &seen, 4U) != 0 ||
                hwa_experiment_json_string(json, &value) != 0 ||
                hwa_experiment_parse_format(value, &probe->format) != 0) {
                hwa_experiment_json_string_free(json, &value);
                goto failed;
            }
            hwa_experiment_json_string_free(json, &value);
        } else if (strcmp(key, "input_id") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 5U) != 0 ||
                hwa_experiment_parse_string_or_null(json, &probe->input_id) != 0)
                goto failed;
        } else if (strcmp(key, "output") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 6U) != 0 ||
                hwa_experiment_parse_string_or_null(json, &probe->output) != 0)
                goto failed;
        } else if (strcmp(key, "start_sample") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 7U) != 0 ||
                hwa_experiment_json_i64(json, &probe->start_sample) != 0)
                goto failed;
        } else if (strcmp(key, "rate_numerator") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 8U) != 0 ||
                hwa_experiment_json_u64(json, &probe->rate_numerator) != 0)
                goto failed;
        } else if (strcmp(key, "rate_denominator") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 9U) != 0 ||
                hwa_experiment_json_u64(json, &probe->rate_denominator) != 0)
                goto failed;
        } else if (strcmp(key, "value_count") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 10U) != 0 ||
                hwa_experiment_json_u64(json, &probe->value_count) != 0)
                goto failed;
        } else {
            hwa_experiment_json_fail(json, "unknown case probe key");
            goto failed;
        }
        hwa_experiment_json_string_free(json, &key);
    }
    json->depth--;
    if (seen != UINT64_C(0x7ff) || !hwa_experiment_token_valid(probe->id, 0) ||
        !hwa_experiment_token_valid(probe->name, 1) ||
        !hwa_experiment_unit_valid(probe->unit) ||
        ((probe->input_id != NULL) == (probe->output != NULL)) ||
        (probe->input_id != NULL &&
         !hwa_experiment_token_valid(probe->input_id, 0)) ||
        (probe->output != NULL &&
         !hwa_experiment_output_name_valid(probe->output)) ||
        probe->rate_numerator == 0U || probe->rate_denominator == 0U ||
        probe->value_count == 0U) {
        hwa_experiment_json_fail(json, "incomplete or invalid case probe");
        goto failed_no_depth;
    }
    return 0;
failed:
    hwa_experiment_json_string_free(json, &key);
    json->depth--;
failed_no_depth:
    hwa_experiment_json_string_free(json, &probe->id);
    hwa_experiment_json_string_free(json, &probe->name);
    hwa_experiment_json_string_free(json, &probe->unit);
    hwa_experiment_json_string_free(json, &probe->input_id);
    hwa_experiment_json_string_free(json, &probe->output);
    memset(probe, 0, sizeof(*probe));
    return -1;
}

static int hwa_experiment_parse_link(HWAExperimentJson *json,
                                     HWAExperimentLinkTemplate *link)
{
    uint64_t seen = 0U;
    char *key = NULL;
    int first = 1;
    memset(link, 0, sizeof(*link));
    if (hwa_experiment_json_take(json, '{') != 0 ||
        hwa_experiment_json_depth_begin(json) != 0) return -1;
    for (;;) {
        hwa_experiment_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == '}') {
            json->offset++;
            break;
        }
        if (!first && hwa_experiment_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_experiment_json_string(json, &key) != 0) goto failed;
        if (strcmp(key, "stem") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 0U) != 0 ||
                hwa_experiment_json_string(json, &link->stem_id) != 0)
                goto failed;
        } else if (strcmp(key, "probe") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 1U) != 0 ||
                hwa_experiment_json_string(json, &link->probe_id) != 0)
                goto failed;
        } else if (strcmp(key, "feature") == 0) {
            char *value = NULL;
            if (hwa_experiment_json_key(json, &key, &seen, 2U) != 0 ||
                hwa_experiment_json_string(json, &value) != 0 ||
                strcmp(value, "rms_dbfs") != 0) {
                hwa_experiment_json_string_free(json, &value);
                goto failed;
            }
            link->feature = HWA_RUN_FEATURE_RMS_DBFS;
            link->feature_index = 0U;
            hwa_experiment_json_string_free(json, &value);
        } else {
            hwa_experiment_json_fail(json, "unknown case link key");
            goto failed;
        }
        hwa_experiment_json_string_free(json, &key);
    }
    json->depth--;
    if (seen != UINT64_C(7) ||
        !hwa_experiment_token_valid(link->stem_id, 0) ||
        !hwa_experiment_token_valid(link->probe_id, 0)) {
        hwa_experiment_json_fail(json, "incomplete or invalid case link");
        goto failed_no_depth;
    }
    return 0;
failed:
    hwa_experiment_json_string_free(json, &key);
    json->depth--;
failed_no_depth:
    hwa_experiment_json_string_free(json, &link->stem_id);
    hwa_experiment_json_string_free(json, &link->probe_id);
    memset(link, 0, sizeof(*link));
    return -1;
}

static int hwa_experiment_parse_stems(HWAExperimentJson *json,
                                      HWAExperimentCaseTemplate *item)
{
    int first = 1;
    if (hwa_experiment_json_take(json, '[') != 0 ||
        hwa_experiment_json_depth_begin(json) != 0) return -1;
    for (;;) {
        hwa_experiment_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == ']') {
            json->offset++;
            break;
        }
        if (!first && hwa_experiment_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_experiment_grow(json, (void **)&item->stems,
                                &item->stem_capacity, item->stem_count,
                                HWA_EXPERIMENT_MAX_RESOURCES_PER_CASE,
                                sizeof(*item->stems)) != 0 ||
            hwa_experiment_parse_stem(json, &item->stems[item->stem_count]) != 0)
            goto failed;
        item->stem_count++;
    }
    json->depth--;
    return 0;
failed:
    json->depth--;
    return -1;
}

static int hwa_experiment_parse_probes(HWAExperimentJson *json,
                                       HWAExperimentCaseTemplate *item)
{
    int first = 1;
    if (hwa_experiment_json_take(json, '[') != 0 ||
        hwa_experiment_json_depth_begin(json) != 0) return -1;
    for (;;) {
        hwa_experiment_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == ']') {
            json->offset++;
            break;
        }
        if (!first && hwa_experiment_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_experiment_grow(json, (void **)&item->probes,
                                &item->probe_capacity, item->probe_count,
                                HWA_EXPERIMENT_MAX_RESOURCES_PER_CASE,
                                sizeof(*item->probes)) != 0 ||
            hwa_experiment_parse_probe(
                json, &item->probes[item->probe_count]) != 0) goto failed;
        item->probe_count++;
    }
    json->depth--;
    return 0;
failed:
    json->depth--;
    return -1;
}

static int hwa_experiment_parse_links(HWAExperimentJson *json,
                                      HWAExperimentCaseTemplate *item)
{
    int first = 1;
    if (hwa_experiment_json_take(json, '[') != 0 ||
        hwa_experiment_json_depth_begin(json) != 0) return -1;
    for (;;) {
        hwa_experiment_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == ']') {
            json->offset++;
            break;
        }
        if (!first && hwa_experiment_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_experiment_grow(json, (void **)&item->links,
                                &item->link_capacity, item->link_count,
                                HWA_EXPERIMENT_MAX_RESOURCES_PER_CASE,
                                sizeof(*item->links)) != 0 ||
            hwa_experiment_parse_link(json, &item->links[item->link_count]) != 0)
            goto failed;
        item->link_count++;
    }
    json->depth--;
    return 0;
failed:
    json->depth--;
    return -1;
}

static int hwa_experiment_parse_case(HWAExperimentJson *json,
                                     HWAExperimentCase *record,
                                     HWAExperimentCaseTemplate *item)
{
    uint64_t seen = 0U;
    char *key = NULL;
    int first = 1;
    memset(record, 0, sizeof(*record));
    memset(item, 0, sizeof(*item));
    if (hwa_experiment_json_take(json, '{') != 0 ||
        hwa_experiment_json_depth_begin(json) != 0) return -1;
    for (;;) {
        hwa_experiment_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == '}') {
            json->offset++;
            break;
        }
        if (!first && hwa_experiment_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_experiment_json_string(json, &key) != 0) goto failed;
        if (strcmp(key, "id") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 0U) != 0 ||
                hwa_experiment_json_string(json, &record->name) != 0)
                goto failed;
        } else if (strcmp(key, "split") == 0) {
            char *value = NULL;
            if (hwa_experiment_json_key(json, &key, &seen, 1U) != 0 ||
                hwa_experiment_json_string(json, &value) != 0 ||
                hwa_experiment_parse_split(value, &record->split) != 0) {
                hwa_experiment_json_string_free(json, &value);
                goto failed;
            }
            hwa_experiment_json_string_free(json, &value);
        } else if (strcmp(key, "weight") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 2U) != 0 ||
                hwa_experiment_json_double(json, &record->weight) != 0)
                goto failed;
        } else if (strcmp(key, "stems") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 3U) != 0 ||
                hwa_experiment_parse_stems(json, item) != 0) goto failed;
        } else if (strcmp(key, "probes") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 4U) != 0 ||
                hwa_experiment_parse_probes(json, item) != 0) goto failed;
        } else if (strcmp(key, "links") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 5U) != 0 ||
                hwa_experiment_parse_links(json, item) != 0) goto failed;
        } else {
            hwa_experiment_json_fail(json, "unknown case key");
            goto failed;
        }
        hwa_experiment_json_string_free(json, &key);
    }
    json->depth--;
    if (seen != UINT64_C(0x3f) || !hwa_experiment_token_valid(record->name, 0) ||
        !(record->weight > 0.0) || item->stem_count == 0U) {
        hwa_experiment_json_fail(json, "incomplete or invalid case");
        goto failed_no_depth;
    }
    return 0;
failed:
    hwa_experiment_json_string_free(json, &key);
    json->depth--;
failed_no_depth:
    hwa_experiment_case_free(record, item);
    return -1;
}

static int hwa_experiment_parse_response(HWAExperimentJson *json,
                                         HWAExperimentResponse *response)
{
    uint64_t seen = 0U;
    char *key = NULL;
    int first = 1;
    memset(response, 0, sizeof(*response));
    if (hwa_experiment_json_take(json, '{') != 0 ||
        hwa_experiment_json_depth_begin(json) != 0) return -1;
    for (;;) {
        hwa_experiment_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == '}') {
            json->offset++;
            break;
        }
        if (!first && hwa_experiment_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_experiment_json_string(json, &key) != 0) goto failed;
        if (strcmp(key, "id") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 0U) != 0 ||
                hwa_experiment_json_string(json, &response->name) != 0)
                goto failed;
        } else if (strcmp(key, "role") == 0) {
            char *value = NULL;
            if (hwa_experiment_json_key(json, &key, &seen, 1U) != 0 ||
                hwa_experiment_json_string(json, &value) != 0 ||
                hwa_experiment_parse_role(value, &response->role) != 0) {
                hwa_experiment_json_string_free(json, &value);
                goto failed;
            }
            hwa_experiment_json_string_free(json, &value);
        } else if (strcmp(key, "feature") == 0) {
            char *value = NULL;
            if (hwa_experiment_json_key(json, &key, &seen, 2U) != 0 ||
                hwa_experiment_json_string(json, &value) != 0 ||
                hwa_experiment_parse_feature(value, &response->feature) != 0) {
                hwa_experiment_json_string_free(json, &value);
                goto failed;
            }
            hwa_experiment_json_string_free(json, &value);
        } else if (strcmp(key, "index") == 0) {
            uint64_t value;
            if (hwa_experiment_json_key(json, &key, &seen, 3U) != 0 ||
                hwa_experiment_json_u64(json, &value) != 0 ||
                value > UINT32_MAX) goto failed;
            response->feature_index = (uint32_t)value;
        } else {
            hwa_experiment_json_fail(json, "unknown response key");
            goto failed;
        }
        hwa_experiment_json_string_free(json, &key);
    }
    json->depth--;
    if (seen != UINT64_C(0xf) || !hwa_experiment_token_valid(response->name, 0) ||
        ((response->feature != HWA_RUN_FEATURE_BAND_LEVEL_DBFS &&
          response->feature_index != 0U) ||
         (response->feature == HWA_RUN_FEATURE_BAND_LEVEL_DBFS &&
          response->feature_index >= HWA_BAND_COUNT))) {
        hwa_experiment_json_fail(json, "incomplete or invalid response");
        goto failed_no_depth;
    }
    return 0;
failed:
    hwa_experiment_json_string_free(json, &key);
    json->depth--;
failed_no_depth:
    hwa_experiment_json_string_free(json, &response->name);
    memset(response, 0, sizeof(*response));
    return -1;
}

static int hwa_experiment_parse_plan(HWAExperimentJson *json,
                                     HWAExperimentManifest *manifest)
{
    uint64_t seen = 0U;
    char *key = NULL;
    int first = 1;
    uint64_t samples = 0U;
    uint64_t replicates = 0U;
    if (hwa_experiment_json_take(json, '{') != 0 ||
        hwa_experiment_json_depth_begin(json) != 0) return -1;
    for (;;) {
        hwa_experiment_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == '}') {
            json->offset++;
            break;
        }
        if (!first && hwa_experiment_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_experiment_json_string(json, &key) != 0) goto failed;
        if (strcmp(key, "kind") == 0) {
            char *value = NULL;
            if (hwa_experiment_json_key(json, &key, &seen, 0U) != 0 ||
                hwa_experiment_json_string(json, &value) != 0 ||
                hwa_experiment_parse_plan_kind(value, &manifest->plan_kind) != 0) {
                hwa_experiment_json_string_free(json, &value);
                goto failed;
            }
            hwa_experiment_json_string_free(json, &value);
        } else if (strcmp(key, "seed") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 1U) != 0 ||
                hwa_experiment_json_u64(json, &manifest->plan_seed) != 0)
                goto failed;
        } else if (strcmp(key, "sample_count") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 2U) != 0 ||
                hwa_experiment_json_u64(json, &samples) != 0) goto failed;
        } else if (strcmp(key, "replicates") == 0) {
            if (hwa_experiment_json_key(json, &key, &seen, 3U) != 0 ||
                hwa_experiment_json_u64(json, &replicates) != 0) goto failed;
        } else {
            hwa_experiment_json_fail(json, "unknown plan key");
            goto failed;
        }
        hwa_experiment_json_string_free(json, &key);
    }
    json->depth--;
    if (seen != UINT64_C(0xf) || replicates == 0U || replicates > SIZE_MAX ||
        samples > SIZE_MAX ||
        ((manifest->plan_kind == HWA_EXPERIMENT_RANDOM) != (samples > 0U))) {
        hwa_experiment_json_fail(json, "incomplete or invalid plan");
        goto failed_no_depth;
    }
    manifest->sample_count = (size_t)samples;
    manifest->replicates = (size_t)replicates;
    return 0;
failed:
    hwa_experiment_json_string_free(json, &key);
    json->depth--;
failed_no_depth:
    return -1;
}

static int hwa_experiment_parse_input_array(HWAExperimentJson *json,
                                            HWAExperimentManifest *manifest,
                                            size_t maximum)
{
    int first = 1;
    if (hwa_experiment_json_take(json, '[') != 0 ||
        hwa_experiment_json_depth_begin(json) != 0) return -1;
    for (;;) {
        hwa_experiment_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == ']') {
            json->offset++;
            break;
        }
        if (!first && hwa_experiment_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_experiment_grow(json, (void **)&manifest->inputs,
                                &manifest->input_capacity,
                                manifest->input_count, maximum,
                                sizeof(*manifest->inputs)) != 0 ||
            hwa_experiment_parse_input(
                json, &manifest->inputs[manifest->input_count]) != 0)
            goto failed;
        manifest->input_count++;
    }
    json->depth--;
    return 0;
failed:
    json->depth--;
    return -1;
}

static int hwa_experiment_parse_parameter_array(
    HWAExperimentJson *json,
    HWAExperimentManifest *manifest,
    size_t maximum_parameters,
    size_t maximum_levels)
{
    int first = 1;
    if (hwa_experiment_json_take(json, '[') != 0 ||
        hwa_experiment_json_depth_begin(json) != 0) return -1;
    for (;;) {
        hwa_experiment_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == ']') {
            json->offset++;
            break;
        }
        if (!first && hwa_experiment_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (manifest->level_count > maximum_levels ||
            hwa_experiment_grow(json, (void **)&manifest->parameters,
                                &manifest->parameter_capacity,
                                manifest->parameter_count,
                                maximum_parameters,
                                sizeof(*manifest->parameters)) != 0 ||
            hwa_experiment_parse_parameter(
                json, manifest,
                &manifest->parameters[manifest->parameter_count]) != 0 ||
            manifest->level_count > maximum_levels) goto failed;
        manifest->parameter_count++;
    }
    json->depth--;
    return 0;
failed:
    json->depth--;
    return -1;
}

static int hwa_experiment_case_arrays_grow(HWAExperimentJson *json,
                                           HWAExperimentManifest *manifest,
                                           size_t maximum)
{
    size_t next;
    uint64_t old_case_bytes;
    uint64_t old_template_bytes;
    uint64_t new_case_bytes;
    uint64_t new_template_bytes;
    HWAExperimentCase *cases;
    HWAExperimentCaseTemplate *templates;
    if (manifest->case_count < manifest->case_capacity) return 0;
    if (manifest->case_count >= maximum)
        return hwa_experiment_json_fail(json, "case limit exceeded");
    next = manifest->case_capacity == 0U ? 4U :
           manifest->case_capacity * 2U;
    if (next < manifest->case_capacity || next > maximum) next = maximum;
    old_case_bytes = (uint64_t)(manifest->case_capacity * sizeof(*cases));
    old_template_bytes =
        (uint64_t)(manifest->case_capacity * sizeof(*templates));
    new_case_bytes = (uint64_t)(next * sizeof(*cases));
    new_template_bytes = (uint64_t)(next * sizeof(*templates));
    if (hwa_experiment_work_add(json->live_work, new_case_bytes,
                                json->max_work, json->error,
                                json->error_size) != 0 ||
        hwa_experiment_work_add(json->live_work, new_template_bytes,
                                json->max_work, json->error,
                                json->error_size) != 0) return -1;
    cases = (HWAExperimentCase *)calloc(next, sizeof(*cases));
    templates = (HWAExperimentCaseTemplate *)calloc(next, sizeof(*templates));
    if (cases == NULL || templates == NULL) {
        free(cases);
        free(templates);
        return hwa_experiment_json_fail(json, "cannot allocate cases");
    }
    if (old_case_bytes != 0U)
        memcpy(cases, manifest->cases, (size_t)old_case_bytes);
    if (old_template_bytes != 0U)
        memcpy(templates, manifest->case_templates,
               (size_t)old_template_bytes);
    free(manifest->cases);
    free(manifest->case_templates);
    *json->live_work -= old_case_bytes + old_template_bytes;
    manifest->cases = cases;
    manifest->case_templates = templates;
    manifest->case_capacity = next;
    return 0;
}

static int hwa_experiment_parse_case_array(HWAExperimentJson *json,
                                           HWAExperimentManifest *manifest,
                                           size_t maximum)
{
    int first = 1;
    if (hwa_experiment_json_take(json, '[') != 0 ||
        hwa_experiment_json_depth_begin(json) != 0) return -1;
    for (;;) {
        hwa_experiment_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == ']') {
            json->offset++;
            break;
        }
        if (!first && hwa_experiment_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_experiment_case_arrays_grow(json, manifest, maximum) != 0 ||
            hwa_experiment_parse_case(
                json, &manifest->cases[manifest->case_count],
                &manifest->case_templates[manifest->case_count]) != 0)
            goto failed;
        manifest->case_count++;
    }
    json->depth--;
    return 0;
failed:
    json->depth--;
    return -1;
}

static int hwa_experiment_parse_response_array(HWAExperimentJson *json,
                                               HWAExperimentManifest *manifest,
                                               size_t maximum)
{
    int first = 1;
    if (hwa_experiment_json_take(json, '[') != 0 ||
        hwa_experiment_json_depth_begin(json) != 0) return -1;
    for (;;) {
        hwa_experiment_json_space(json);
        if (json->offset < json->size && json->data[json->offset] == ']') {
            json->offset++;
            break;
        }
        if (!first && hwa_experiment_json_take(json, ',') != 0) goto failed;
        first = 0;
        if (hwa_experiment_grow(json, (void **)&manifest->responses,
                                &manifest->response_capacity,
                                manifest->response_count, maximum,
                                sizeof(*manifest->responses)) != 0 ||
            hwa_experiment_parse_response(
                json, &manifest->responses[manifest->response_count]) != 0)
            goto failed;
        manifest->response_count++;
    }
    json->depth--;
    return 0;
failed:
    json->depth--;
    return -1;
}

static const HWAExperimentInput *hwa_experiment_manifest_input_find(
    const HWAExperimentManifest *manifest,
    const char *id)
{
    size_t index;
    for (index = 0U; index < manifest->input_count; ++index)
        if (strcmp(manifest->inputs[index].binding_id, id) == 0)
            return &manifest->inputs[index];
    return NULL;
}

static const HWAExperimentStemTemplate *hwa_experiment_case_stem_find(
    const HWAExperimentCaseTemplate *item,
    const char *id)
{
    size_t index;
    for (index = 0U; index < item->stem_count; ++index)
        if (strcmp(item->stems[index].id, id) == 0) return &item->stems[index];
    return NULL;
}

static const HWAExperimentProbeTemplate *hwa_experiment_case_probe_find(
    const HWAExperimentCaseTemplate *item,
    const char *id)
{
    size_t index;
    for (index = 0U; index < item->probe_count; ++index)
        if (strcmp(item->probes[index].id, id) == 0) return &item->probes[index];
    return NULL;
}

static int hwa_experiment_manifest_semantics(HWAExperimentJson *json,
                                             HWAExperimentManifest *manifest,
                                             const HWAExperimentOptions *options)
{
    size_t index;
    size_t other;
    size_t fit_count = 0U;
    size_t check_count = 0U;
    double fit_weight = 0.0;
    double check_weight = 0.0;
    size_t expected_levels = 0U;
    if (manifest->input_count == 0U || manifest->parameter_count == 0U ||
        manifest->case_count == 0U || manifest->response_count == 0U ||
        manifest->replicates > options->max_replicates)
        return hwa_experiment_json_fail(json, "empty or over-limit manifest");
    for (index = 0U; index < manifest->input_count; ++index) {
        manifest->inputs[index].id = (uint64_t)index + UINT64_C(1);
        if (index != 0U && strcmp(manifest->inputs[index - 1U].binding_id,
                                  manifest->inputs[index].binding_id) >= 0)
            return hwa_experiment_json_fail(json, "inputs are not sorted");
    }
    for (index = 0U; index < manifest->parameter_count; ++index) {
        HWAExperimentParameter *parameter = &manifest->parameters[index];
        int baseline_seen = 0;
        parameter->id = (uint64_t)index + UINT64_C(1);
        if (index != 0U && strcmp(manifest->parameters[index - 1U].name,
                                  parameter->name) >= 0)
            return hwa_experiment_json_fail(json, "parameters are not sorted");
        if (parameter->first_level != expected_levels)
            return hwa_experiment_json_fail(json, "bad level span");
        if (!isfinite(parameter->maximum - parameter->minimum))
            return hwa_experiment_json_fail(json,
                                             "parameter span is too large");
        expected_levels += parameter->level_count;
        if (manifest->plan_kind == HWA_EXPERIMENT_RANDOM) {
            if (!(parameter->minimum < parameter->maximum) ||
                !isfinite(parameter->maximum - parameter->minimum) ||
                parameter->level_count != 0U)
                return hwa_experiment_json_fail(
                    json, "random parameters need a range and no levels");
        } else {
            if (parameter->level_count == 0U)
                return hwa_experiment_json_fail(json, "parameter levels empty");
            for (other = 0U; other < parameter->level_count; ++other) {
                HWAExperimentLevel *level =
                    &manifest->levels[parameter->first_level + other];
                level->id = (uint64_t)(parameter->first_level + other) +
                            UINT64_C(1);
                level->parameter_id = parameter->id;
                if (level->value < parameter->minimum ||
                    level->value > parameter->maximum ||
                    (other != 0U &&
                     manifest->levels[parameter->first_level + other - 1U]
                             .value >= level->value))
                    return hwa_experiment_json_fail(json,
                                                    "invalid sorted levels");
                if (level->value == parameter->baseline) baseline_seen = 1;
            }
            if (!baseline_seen)
                return hwa_experiment_json_fail(json,
                                                "levels omit baseline");
        }
    }
    for (index = 0U; index < manifest->case_count; ++index) {
        HWAExperimentCase *record = &manifest->cases[index];
        HWAExperimentCaseTemplate *item = &manifest->case_templates[index];
        size_t reference_final = 0U;
        size_t model_final = 0U;
        unsigned model_roles = 0U;
        record->id = (uint64_t)index + UINT64_C(1);
        if (index != 0U && strcmp(manifest->cases[index - 1U].name,
                                  record->name) >= 0)
            return hwa_experiment_json_fail(json, "cases are not sorted");
        if (record->split == HWA_EXPERIMENT_FIT) {
            if (!isfinite(fit_weight + record->weight))
                return hwa_experiment_json_fail(json,
                                                 "fit weights are too large");
            fit_weight += record->weight;
            fit_count++;
        } else {
            if (!isfinite(check_weight + record->weight))
                return hwa_experiment_json_fail(
                    json, "check weights are too large");
            check_weight += record->weight;
            check_count++;
        }
        if (item->stem_count > options->run.max_stems ||
            item->probe_count > options->run.max_probes ||
            item->link_count > options->run.max_links ||
            item->probe_count > options->run.max_result_rows ||
            item->stem_count >
                options->run.max_result_rows - item->probe_count)
            return hwa_experiment_json_fail(json,
                                             "case exceeds Stage 7 row caps");
        if (hwa_experiment_case_output_count(item) == 0U)
            return hwa_experiment_json_fail(
                json, "each case needs a renderer output");
        for (other = 0U; other < item->stem_count; ++other) {
            HWAExperimentStemTemplate *stem = &item->stems[other];
            size_t scan;
            if (stem->rate_hz != manifest->clock_rate_hz ||
                stem->channels > HWA_MAX_CHANNELS ||
                stem->gain_db < -120.0 || stem->gain_db > 120.0 ||
                (stem->input_id != NULL &&
                 hwa_experiment_manifest_input_find(manifest,
                                                    stem->input_id) == NULL))
                return hwa_experiment_json_fail(json, "invalid case stem input");
            for (scan = other + 1U; scan < item->stem_count; ++scan)
                if (strcmp(stem->id, item->stems[scan].id) == 0)
                    return hwa_experiment_json_fail(json, "duplicate stream ID");
            if (stem->input_id != NULL) {
                for (scan = other + 1U; scan < item->stem_count; ++scan)
                    if (item->stems[scan].input_id != NULL &&
                        strcmp(stem->input_id,
                               item->stems[scan].input_id) == 0)
                        return hwa_experiment_json_fail(
                            json, "case reuses a fixed input");
                for (scan = 0U; scan < item->probe_count; ++scan)
                    if (item->probes[scan].input_id != NULL &&
                        strcmp(stem->input_id,
                               item->probes[scan].input_id) == 0)
                        return hwa_experiment_json_fail(
                            json, "case reuses a fixed input");
            }
            for (scan = 0U; scan < item->probe_count; ++scan)
                if (strcmp(stem->id, item->probes[scan].id) == 0)
                    return hwa_experiment_json_fail(json, "duplicate stream ID");
            if (stem->output != NULL) {
                for (scan = other + 1U; scan < item->stem_count; ++scan)
                    if (item->stems[scan].output != NULL &&
                        hwa_experiment_ascii_case_equal(
                            stem->output, item->stems[scan].output))
                        return hwa_experiment_json_fail(
                            json, "duplicate output name");
                for (scan = 0U; scan < item->probe_count; ++scan)
                    if (item->probes[scan].output != NULL &&
                        hwa_experiment_ascii_case_equal(
                            stem->output, item->probes[scan].output))
                        return hwa_experiment_json_fail(
                            json, "duplicate output name");
            }
            if (stem->side == HWA_RUN_REFERENCE) {
                if (stem->role != HWA_RUN_STEM_FINAL)
                    return hwa_experiment_json_fail(
                        json, "only reference.final is accepted");
                reference_final++;
            } else {
                unsigned bit = 1U << (unsigned)stem->role;
                if ((model_roles & bit) != 0U)
                    return hwa_experiment_json_fail(json,
                                                    "duplicate model role");
                model_roles |= bit;
                if (stem->role == HWA_RUN_STEM_FINAL) model_final++;
            }
        }
        if (reference_final != 1U || model_final != 1U)
            return hwa_experiment_json_fail(
                json, "each case needs reference.final and model.final");
        for (other = 0U; other < item->probe_count; ++other) {
            HWAExperimentProbeTemplate *probe = &item->probes[other];
            size_t scan;
            if (probe->input_id != NULL &&
                hwa_experiment_manifest_input_find(manifest,
                                                   probe->input_id) == NULL)
                return hwa_experiment_json_fail(json, "invalid probe input");
            if (probe->value_count > options->run.max_probe_values ||
                hwa_experiment_gcd_u64(probe->rate_numerator,
                                       probe->rate_denominator) != 1U)
                return hwa_experiment_json_fail(json,
                                                 "invalid probe limits");
            if (probe->format == HWA_RUN_PROBE_BINARY_F64LE &&
                (probe->value_count >
                     (UINT64_MAX - UINT64_C(16)) / UINT64_C(8) ||
                 UINT64_C(16) + UINT64_C(8) * probe->value_count >
                     options->run.max_probe_bytes))
                return hwa_experiment_json_fail(json,
                                                 "binary probe is too large");
            for (scan = other + 1U; scan < item->probe_count; ++scan)
                if (strcmp(probe->id, item->probes[scan].id) == 0)
                    return hwa_experiment_json_fail(json, "duplicate stream ID");
            if (probe->input_id != NULL)
                for (scan = other + 1U; scan < item->probe_count; ++scan)
                    if (item->probes[scan].input_id != NULL &&
                        strcmp(probe->input_id,
                               item->probes[scan].input_id) == 0)
                        return hwa_experiment_json_fail(
                            json, "case reuses a fixed input");
            if (probe->output != NULL)
                for (scan = other + 1U; scan < item->probe_count; ++scan)
                    if (item->probes[scan].output != NULL &&
                        hwa_experiment_ascii_case_equal(
                            probe->output, item->probes[scan].output))
                        return hwa_experiment_json_fail(
                            json, "duplicate output name");
        }
        for (other = 0U; other < item->link_count; ++other) {
            const HWAExperimentLinkTemplate *link = &item->links[other];
            const HWAExperimentStemTemplate *stem =
                hwa_experiment_case_stem_find(item, link->stem_id);
            const HWAExperimentProbeTemplate *probe =
                hwa_experiment_case_probe_find(item, link->probe_id);
            size_t scan;
            if (stem == NULL || probe == NULL || stem->side != probe->side ||
                (stem->side == HWA_RUN_REFERENCE &&
                 stem->role != HWA_RUN_STEM_FINAL))
                return hwa_experiment_json_fail(json, "invalid same-side link");
            for (scan = other + 1U; scan < item->link_count; ++scan)
                if (strcmp(link->stem_id, item->links[scan].stem_id) == 0 &&
                    strcmp(link->probe_id, item->links[scan].probe_id) == 0)
                    return hwa_experiment_json_fail(json, "duplicate link");
        }
    }
    if (fit_count == 0U || check_count == 0U)
        return hwa_experiment_json_fail(json, "fit and check cases required");
    for (index = 0U; index < manifest->response_count; ++index) {
        HWAExperimentResponse *response = &manifest->responses[index];
        response->id = (uint64_t)index + UINT64_C(1);
        if (index != 0U && strcmp(manifest->responses[index - 1U].name,
                                  response->name) >= 0)
            return hwa_experiment_json_fail(json, "responses are not sorted");
        for (other = 0U; other < manifest->case_count; ++other) {
            const HWAExperimentCaseTemplate *item =
                &manifest->case_templates[other];
            size_t stem_index;
            int found = 0;
            for (stem_index = 0U; stem_index < item->stem_count; ++stem_index)
                if (item->stems[stem_index].side == HWA_RUN_MODEL &&
                    item->stems[stem_index].role == response->role) found = 1;
            if (!found)
                return hwa_experiment_json_fail(
                    json, "response role missing from a case");
        }
    }
    return 0;
}

static int hwa_experiment_manifest_parse(
    const unsigned char *data,
    size_t size,
    const HWAExperimentOptions *options,
    const HWANumericLocale *locale,
    uint64_t *live_work,
    HWAExperimentManifest *manifest,
    char *error,
    size_t error_size)
{
    HWAExperimentJson json;
    uint64_t seen = 0U;
    int first = 1;
    int status = -1;
    memset(manifest, 0, sizeof(*manifest));
    memset(&json, 0, sizeof(json));
    json.data = data;
    json.size = size;
    json.max_work = options->max_work_bytes;
    json.live_work = live_work;
    json.locale = locale;
    json.error = error;
    json.error_size = error_size;
    if (hwa_experiment_json_take(&json, '{') != 0 ||
        hwa_experiment_json_depth_begin(&json) != 0) goto cleanup;
    for (;;) {
        char *key = NULL;
        hwa_experiment_json_space(&json);
        if (json.offset < json.size && json.data[json.offset] == '}') {
            json.offset++;
            break;
        }
        if (!first && hwa_experiment_json_take(&json, ',') != 0)
            goto cleanup_depth;
        first = 0;
        if (hwa_experiment_json_string(&json, &key) != 0) goto cleanup_depth;
        if (strcmp(key, "schema") == 0) {
            char *value = NULL;
            if (hwa_experiment_json_key(&json, &key, &seen, 0U) != 0 ||
                hwa_experiment_json_string(&json, &value) != 0 ||
                strcmp(value, "hwa-experiment") != 0) {
                hwa_experiment_json_string_free(&json, &value);
                goto cleanup_depth;
            }
            hwa_experiment_json_string_free(&json, &value);
        } else if (strcmp(key, "schema_version") == 0) {
            uint64_t value;
            if (hwa_experiment_json_key(&json, &key, &seen, 1U) != 0 ||
                hwa_experiment_json_u64(&json, &value) != 0 || value != 1U)
                goto cleanup_depth;
        } else if (strcmp(key, "method_version") == 0) {
            char *value = NULL;
            if (hwa_experiment_json_key(&json, &key, &seen, 2U) != 0 ||
                hwa_experiment_json_string(&json, &value) != 0 ||
                strcmp(value, HWA_EXPERIMENT_METHOD_VERSION) != 0) {
                hwa_experiment_json_string_free(&json, &value);
                goto cleanup_depth;
            }
            hwa_experiment_json_string_free(&json, &value);
        } else if (strcmp(key, "clock_rate_hz") == 0) {
            uint64_t value;
            if (hwa_experiment_json_key(&json, &key, &seen, 3U) != 0 ||
                hwa_experiment_json_u64(&json, &value) != 0 ||
                value < 8000U || value > 768000U) goto cleanup_depth;
            manifest->clock_rate_hz = (uint32_t)value;
        } else if (strcmp(key, "inputs") == 0) {
            if (hwa_experiment_json_key(&json, &key, &seen, 4U) != 0 ||
                hwa_experiment_parse_input_array(
                    &json, manifest, options->max_artifacts) != 0)
                goto cleanup_depth;
        } else if (strcmp(key, "parameters") == 0) {
            if (hwa_experiment_json_key(&json, &key, &seen, 5U) != 0 ||
                hwa_experiment_parse_parameter_array(
                    &json, manifest, options->max_parameters,
                    options->max_levels) != 0) goto cleanup_depth;
        } else if (strcmp(key, "plan") == 0) {
            if (hwa_experiment_json_key(&json, &key, &seen, 6U) != 0 ||
                hwa_experiment_parse_plan(&json, manifest) != 0)
                goto cleanup_depth;
        } else if (strcmp(key, "cases") == 0) {
            if (hwa_experiment_json_key(&json, &key, &seen, 7U) != 0 ||
                hwa_experiment_parse_case_array(
                    &json, manifest, options->max_cases) != 0)
                goto cleanup_depth;
        } else if (strcmp(key, "responses") == 0) {
            if (hwa_experiment_json_key(&json, &key, &seen, 8U) != 0 ||
                hwa_experiment_parse_response_array(
                    &json, manifest, options->max_responses) != 0)
                goto cleanup_depth;
        } else {
            hwa_experiment_json_fail(&json, "unknown top-level key");
            goto cleanup_depth;
        }
        hwa_experiment_json_string_free(&json, &key);
    }
    json.depth--;
    hwa_experiment_json_space(&json);
    if (seen != UINT64_C(0x1ff) || json.offset != json.size ||
        hwa_experiment_manifest_semantics(&json, manifest, options) != 0) {
        if (error != NULL && error_size != 0U && error[0] == '\0')
            hwa_experiment_json_fail(&json,
                                     "incomplete manifest or trailing data");
        goto cleanup;
    }
    status = 0;
    goto cleanup;
cleanup_depth:
    json.depth--;
cleanup:
    if (status != 0) hwa_experiment_manifest_free(manifest);
    return status;
}

static int hwa_experiment_blob_read(const char *path,
                                    uint64_t maximum,
                                    HWAExperimentBlob *blob,
                                    char *error,
                                    size_t error_size)
{
    FILE *stream = NULL;
    HWAExperimentFileIdentity before;
    HWAExperimentFileIdentity opened;
    HWAExperimentFileIdentity after;
    HWASha256 hash;
    unsigned char digest[32];
    size_t size;
    int status = -1;
    memset(blob, 0, sizeof(*blob));
    memset(&before, 0, sizeof(before));
    memset(&opened, 0, sizeof(opened));
    memset(&after, 0, sizeof(after));
#if defined(_WIN32)
    {
        BY_HANDLE_FILE_INFORMATION information;
        HANDLE handle = CreateFileA(path, 0U,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (handle == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandle(handle, &information)) {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            goto failed;
        }
        CloseHandle(handle);
        if ((information.dwFileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U)
            goto failed;
        before.device = information.dwVolumeSerialNumber;
        before.inode = ((uint64_t)information.nFileIndexHigh << 32U) |
            information.nFileIndexLow;
        before.size = ((uint64_t)information.nFileSizeHigh << 32U) |
            information.nFileSizeLow;
        before.valid = 1;
    }
#else
    {
        struct stat info;
        if (path == NULL || path[0] == '\0' || strcmp(path, "-") == 0 ||
            lstat(path, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_size < 0) goto failed;
        before.device = (uint64_t)info.st_dev;
        before.inode = (uint64_t)info.st_ino;
        before.size = (uint64_t)info.st_size;
        before.valid = 1;
    }
#endif
    if (!before.valid || before.size == 0U || before.size > maximum ||
        before.size > SIZE_MAX) goto failed;
    size = (size_t)before.size;
    blob->data = (unsigned char *)malloc(size);
    if (blob->data == NULL) goto failed;
    stream = fopen(path, "rb");
    if (stream == NULL) goto failed;
#if defined(_WIN32)
    {
        BY_HANDLE_FILE_INFORMATION information;
        intptr_t raw = _get_osfhandle(_fileno(stream));
        if (raw == (intptr_t)-1 ||
            !GetFileInformationByHandle((HANDLE)raw, &information)) goto failed;
        opened.device = information.dwVolumeSerialNumber;
        opened.inode = ((uint64_t)information.nFileIndexHigh << 32U) |
            information.nFileIndexLow;
        opened.size = ((uint64_t)information.nFileSizeHigh << 32U) |
            information.nFileSizeLow;
        opened.valid = 1;
    }
#else
    {
        struct stat info;
        if (fstat(fileno(stream), &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_size < 0) goto failed;
        opened.device = (uint64_t)info.st_dev;
        opened.inode = (uint64_t)info.st_ino;
        opened.size = (uint64_t)info.st_size;
        opened.valid = 1;
    }
#endif
    if (!opened.valid || opened.device != before.device ||
        opened.inode != before.inode || opened.size != before.size ||
        fread(blob->data, 1U, size, stream) != size || fgetc(stream) != EOF ||
        ferror(stream) || fclose(stream) != 0) {
        stream = NULL;
        goto failed;
    }
    stream = NULL;
#if defined(_WIN32)
    {
        BY_HANDLE_FILE_INFORMATION information;
        HANDLE handle = CreateFileA(path, 0U,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (handle == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandle(handle, &information)) {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            goto failed;
        }
        CloseHandle(handle);
        after.device = information.dwVolumeSerialNumber;
        after.inode = ((uint64_t)information.nFileIndexHigh << 32U) |
            information.nFileIndexLow;
        after.size = ((uint64_t)information.nFileSizeHigh << 32U) |
            information.nFileSizeLow;
        after.valid = 1;
    }
#else
    {
        struct stat info;
        if (lstat(path, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_size < 0) goto failed;
        after.device = (uint64_t)info.st_dev;
        after.inode = (uint64_t)info.st_ino;
        after.size = (uint64_t)info.st_size;
        after.valid = 1;
    }
#endif
    if (!after.valid || after.device != before.device ||
        after.inode != before.inode || after.size != before.size) goto failed;
    blob->size = size;
    hwa_sha256_init(&hash);
    hwa_sha256_update(&hash, blob->data, blob->size);
    hwa_sha256_final(&hash, digest);
    hwa_sha256_hex(digest, blob->sha256);
    status = 0;
failed:
    if (stream != NULL) (void)fclose(stream);
    if (status != 0) {
        hwa_set_error(error, error_size,
                      "cannot read named regular Stage 8 manifest");
        free(blob->data);
        memset(blob, 0, sizeof(*blob));
    }
    return status;
}

static uint64_t hwa_experiment_splitmix64(uint64_t *state)
{
    uint64_t value;
    *state += UINT64_C(0x9e3779b97f4a7c15);
    value = *state;
    value = (value ^ (value >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27U)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

static double hwa_experiment_random_unit(uint64_t *state)
{
    return (double)(hwa_experiment_splitmix64(state) >> 11U) /
           9007199254740992.0;
}

static int hwa_experiment_hash_point(const HWAExperimentResult *result,
                                     size_t point_index,
                                     const HWANumericLocale *locale,
                                     char hash[HWA_SHA256_HEX_SIZE])
{
    HWASha256 context;
    unsigned char digest[32];
    size_t parameter;
    hwa_sha256_init(&context);
    hwa_sha256_update(&context, (const unsigned char *)"{\"parameters\":[",
                      strlen("{\"parameters\":["));
    for (parameter = 0U; parameter < result->parameter_count; ++parameter) {
        const HWAExperimentValue *value =
            &result->values[point_index * result->parameter_count + parameter];
        char number[128];
        const char *name = result->parameters[parameter].name;
        if (hwa_c_locale_format_double(locale, number, sizeof(number),
                                       value->value) != 0) return -1;
        if (parameter != 0U)
            hwa_sha256_update(&context, (const unsigned char *)",", 1U);
        hwa_sha256_update(&context, (const unsigned char *)"{\"id\":\"", 7U);
        hwa_sha256_update(&context, (const unsigned char *)name, strlen(name));
        hwa_sha256_update(&context,
                          (const unsigned char *)"\",\"value\":", 10U);
        hwa_sha256_update(&context, (const unsigned char *)number,
                          strlen(number));
        hwa_sha256_update(&context, (const unsigned char *)"}", 1U);
    }
    hwa_sha256_update(&context, (const unsigned char *)"]}", 2U);
    hwa_sha256_final(&context, digest);
    hwa_sha256_hex(digest, hash);
    return 0;
}

static int hwa_experiment_keys_unique(const char *const *keys, size_t count);

static int hwa_experiment_points_allocate(HWAExperimentResult *result,
                                          size_t point_count,
                                          char *error,
                                          size_t error_size)
{
    uint64_t value_count;
    if (point_count == 0U || point_count > result->options.max_points ||
        hwa_experiment_u64_multiply((uint64_t)point_count,
                                    (uint64_t)result->parameter_count,
                                    &value_count) != 0 ||
        value_count > SIZE_MAX) {
        hwa_set_error(error, error_size, "Stage 8 point cap exceeded");
        return -1;
    }
    result->points = (HWAExperimentPoint *)calloc(point_count,
                                                   sizeof(*result->points));
    result->values = (HWAExperimentValue *)calloc(
        (size_t)value_count, sizeof(*result->values));
    if (result->points == NULL || result->values == NULL) {
        hwa_set_error(error, error_size, "cannot allocate Stage 8 points");
        return -1;
    }
    result->point_count = point_count;
    result->value_count = (size_t)value_count;
    return 0;
}

static int hwa_experiment_plan_point_count(
    const HWAExperimentManifest *manifest,
    size_t *count)
{
    size_t points = 1U;
    size_t parameter;
    if (manifest == NULL || count == NULL) return -1;
    if (manifest->plan_kind == HWA_EXPERIMENT_ONE_AT_A_TIME) {
        for (parameter = 0U; parameter < manifest->parameter_count;
             ++parameter) {
            size_t add;
            if (manifest->parameters[parameter].level_count == 0U) return -1;
            add = manifest->parameters[parameter].level_count - 1U;
            if (points > SIZE_MAX - add) return -1;
            points += add;
        }
    } else if (manifest->plan_kind == HWA_EXPERIMENT_GRID) {
        for (parameter = 0U; parameter < manifest->parameter_count;
             ++parameter) {
            size_t levels = manifest->parameters[parameter].level_count;
            if (levels == 0U || points > SIZE_MAX / levels) return -1;
            points *= levels;
        }
    } else if (manifest->plan_kind == HWA_EXPERIMENT_RANDOM) {
        if (manifest->sample_count > SIZE_MAX - 1U) return -1;
        points = manifest->sample_count + 1U;
    } else return -1;
    if (points == 0U) return -1;
    *count = points;
    return 0;
}

static int hwa_experiment_points_build(const HWAExperimentManifest *manifest,
                                       const HWANumericLocale *locale,
                                       HWAExperimentResult *result,
                                       char *error,
                                       size_t error_size)
{
    size_t point_count = 1U;
    size_t point = 0U;
    size_t parameter;
    size_t value_index = 0U;
    uint64_t random_state = manifest->plan_seed;
    size_t grid_baseline_linear = 0U;
    if (hwa_experiment_plan_point_count(manifest, &point_count) != 0)
        goto too_many;
    if (manifest->plan_kind == HWA_EXPERIMENT_ONE_AT_A_TIME) {
        /* The shared counter above fixed the OAT catalog size. */
    } else if (manifest->plan_kind == HWA_EXPERIMENT_GRID) {
        for (parameter = 0U; parameter < manifest->parameter_count; ++parameter) {
            size_t level;
            size_t baseline_level = SIZE_MAX;
            for (level = 0U;
                 level < manifest->parameters[parameter].level_count; ++level)
                if (manifest->levels[
                        manifest->parameters[parameter].first_level + level]
                        .value == manifest->parameters[parameter].baseline)
                    baseline_level = level;
            if (baseline_level == SIZE_MAX ||
                grid_baseline_linear >
                    (SIZE_MAX - baseline_level) /
                        manifest->parameters[parameter].level_count)
                goto too_many;
            grid_baseline_linear = grid_baseline_linear *
                manifest->parameters[parameter].level_count + baseline_level;
        }
    }
    if (hwa_experiment_points_allocate(result, point_count,
                                       error, error_size) != 0) return -1;
    for (point = 0U; point < point_count; ++point) {
        result->points[point].id = (uint64_t)point + UINT64_C(1);
        result->points[point].baseline = point == 0U;
        for (parameter = 0U; parameter < manifest->parameter_count; ++parameter) {
            double value = manifest->parameters[parameter].baseline;
            if (point != 0U &&
                manifest->plan_kind == HWA_EXPERIMENT_ONE_AT_A_TIME) {
                size_t remaining = point - 1U;
                size_t scan;
                for (scan = 0U; scan < manifest->parameter_count; ++scan) {
                    size_t choices = manifest->parameters[scan].level_count - 1U;
                    if (remaining < choices) {
                        if (scan == parameter) {
                            size_t level;
                            size_t choice = 0U;
                            for (level = 0U;
                                 level < manifest->parameters[scan].level_count;
                                 ++level) {
                                double candidate = manifest->levels[
                                    manifest->parameters[scan].first_level + level]
                                                       .value;
                                if (candidate == manifest->parameters[scan].baseline)
                                    continue;
                                if (choice == remaining) value = candidate;
                                choice++;
                            }
                        }
                        break;
                    }
                    remaining -= choices;
                }
            } else if (point != 0U &&
                       manifest->plan_kind == HWA_EXPERIMENT_GRID) {
                size_t linear = point - 1U;
                size_t scan = manifest->parameter_count;
                size_t chosen = 0U;
                if (linear >= grid_baseline_linear) linear++;
                while (scan != 0U) {
                    size_t current = scan - 1U;
                    size_t count = manifest->parameters[current].level_count;
                    size_t digit = linear % count;
                    linear /= count;
                    if (current == parameter) chosen = digit;
                    scan--;
                }
                value = manifest->levels[
                    manifest->parameters[parameter].first_level + chosen].value;
            } else if (point != 0U &&
                       manifest->plan_kind == HWA_EXPERIMENT_RANDOM) {
                double unit = hwa_experiment_random_unit(&random_state);
                value = manifest->parameters[parameter].minimum +
                    (manifest->parameters[parameter].maximum -
                     manifest->parameters[parameter].minimum) * unit;
            }
            result->values[value_index].id =
                (uint64_t)value_index + UINT64_C(1);
            result->values[value_index].point_id = (uint64_t)point + UINT64_C(1);
            result->values[value_index].parameter_id =
                (uint64_t)parameter + UINT64_C(1);
            result->values[value_index].value = value == 0.0 ? 0.0 : value;
            value_index++;
        }
        if (hwa_experiment_hash_point(result, point, locale,
                                      result->points[point].key) != 0) {
            hwa_set_error(error, error_size, "cannot hash Stage 8 point");
            return -1;
        }
    }
    {
        const char **keys;
        if (point_count > SIZE_MAX / sizeof(*keys)) goto too_many;
        keys = (const char **)malloc(point_count * sizeof(*keys));
        if (keys == NULL) {
            hwa_set_error(error, error_size,
                          "cannot validate unique Stage 8 points");
            return -1;
        }
        for (point = 0U; point < point_count; ++point)
            keys[point] = result->points[point].key;
        if (!hwa_experiment_keys_unique(keys, point_count)) {
            free(keys);
            hwa_set_error(error, error_size,
                          "Stage 8 plan contains duplicate points");
            return -1;
        }
        free(keys);
    }
    return 0;
too_many:
    hwa_set_error(error, error_size, "Stage 8 point count overflow");
    return -1;
}

static int hwa_experiment_path_join(char **out,
                                    const char *left,
                                    const char *right)
{
    size_t left_size;
    size_t right_size;
    int separator;
    char *path;
    if (out == NULL || left == NULL || right == NULL) return -1;
    left_size = strlen(left);
    right_size = strlen(right);
    separator = left_size != 0U && left[left_size - 1U] != HWA_PATH_SEPARATOR;
    if (left_size > SIZE_MAX - right_size - (separator ? 2U : 1U)) return -1;
    path = (char *)malloc(left_size + right_size + (separator ? 2U : 1U));
    if (path == NULL) return -1;
    memcpy(path, left, left_size);
    if (separator) path[left_size++] = HWA_PATH_SEPARATOR;
    memcpy(path + left_size, right, right_size + 1U);
    *out = path;
    return 0;
}

static int hwa_experiment_absolute_parent(const char *path, char **absolute)
{
    char *copy;
    char *slash;
    char *base;
#if defined(_WIN32)
    char resolved[_MAX_PATH];
#else
    char resolved[PATH_MAX];
#endif
    if (path == NULL || path[0] == '\0') return -1;
    copy = hwa_experiment_copy_string(path);
    if (copy == NULL) return -1;
#if defined(_WIN32)
    {
        char *forward = strrchr(copy, '/');
        slash = strrchr(copy, '\\');
        if (forward != NULL && (slash == NULL || forward > slash))
            slash = forward;
    }
#else
    slash = strrchr(copy, HWA_PATH_SEPARATOR);
#endif
    if (slash == NULL) {
        base = copy;
#if defined(_WIN32)
        if (_getcwd(resolved, sizeof(resolved)) == NULL) {
#else
        if (getcwd(resolved, sizeof(resolved)) == NULL) {
#endif
            free(copy);
            return -1;
        }
    } else {
        char *parent;
        *slash = '\0';
        base = slash + 1U;
#if defined(_WIN32)
        if (slash == copy + 2U && copy[1] == ':') {
            slash[0] = '\\';
            slash[1] = '\0';
        }
        parent = copy[0] == '\0' ? (char *)"\\" : copy;
        if (_fullpath(resolved, parent, sizeof(resolved)) == NULL) {
#else
        parent = copy[0] == '\0' ? (char *)"/" : copy;
        if (realpath(parent, resolved) == NULL) {
#endif
            free(copy);
            return -1;
        }
    }
    if (!hwa_experiment_component_valid(base)) {
        free(copy);
        return -1;
    }
    if (hwa_experiment_path_join(absolute, resolved, base) != 0) {
        free(copy);
        return -1;
    }
    free(copy);
    return 0;
}

static int hwa_experiment_mkdir_new(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

static int hwa_experiment_file_regular_size(const char *path,
                                            uint64_t maximum,
                                            uint64_t *size)
{
#if defined(_WIN32)
    struct _stat64 info;
    if (_stat64(path, &info) != 0 || (info.st_mode & _S_IFREG) == 0 ||
        info.st_size < 0 || (uint64_t)info.st_size > maximum) return -1;
    *size = (uint64_t)info.st_size;
#else
    struct stat info;
    if (lstat(path, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0 || (uint64_t)info.st_size > maximum) return -1;
    *size = (uint64_t)info.st_size;
#endif
    return 0;
}

static int hwa_experiment_file_identity_read(
    const char *path,
    uint64_t maximum,
    HWAExperimentFileIdentity *identity)
{
    if (path == NULL || identity == NULL) return -1;
    memset(identity, 0, sizeof(*identity));
#if defined(_WIN32)
    {
        BY_HANDLE_FILE_INFORMATION information;
        HANDLE handle = CreateFileA(
            path, 0U, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (handle == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandle(handle, &information)) {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            return -1;
        }
        CloseHandle(handle);
        if ((information.dwFileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
              FILE_ATTRIBUTE_DEVICE)) != 0U)
            return -1;
        identity->device = information.dwVolumeSerialNumber;
        identity->inode = ((uint64_t)information.nFileIndexHigh << 32U) |
            information.nFileIndexLow;
        identity->size = ((uint64_t)information.nFileSizeHigh << 32U) |
            information.nFileSizeLow;
    }
#else
    {
        struct stat information;
        if (lstat(path, &information) != 0 ||
            !S_ISREG(information.st_mode) || information.st_size < 0)
            return -1;
        identity->device = (uint64_t)information.st_dev;
        identity->inode = (uint64_t)information.st_ino;
        identity->size = (uint64_t)information.st_size;
    }
#endif
    if (identity->size > maximum) return -1;
    identity->valid = 1;
    return 0;
}

static int hwa_experiment_file_identity_equal(
    const HWAExperimentFileIdentity *left,
    const HWAExperimentFileIdentity *right)
{
    return left != NULL && right != NULL && left->valid && right->valid &&
        left->device == right->device && left->inode == right->inode &&
        left->size == right->size;
}

static int hwa_experiment_hash_file_identity(
    const char *path,
    uint64_t maximum,
    const HWAExperimentFileIdentity *expected,
    HWAExperimentFileIdentity *accepted,
    char hash[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size)
{
    HWAExperimentFileIdentity before;
    HWAExperimentFileIdentity after;
    if (hwa_experiment_file_identity_read(path, maximum, &before) != 0 ||
        (expected != NULL &&
         !hwa_experiment_file_identity_equal(expected, &before)) ||
        hwa_sha256_file(path, maximum, hash, error, error_size) != 0 ||
        hwa_experiment_file_identity_read(path, maximum, &after) != 0 ||
        !hwa_experiment_file_identity_equal(&before, &after))
        return -1;
    if (accepted != NULL) *accepted = before;
    return 0;
}

static int hwa_experiment_json_quoted(FILE *stream, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    if (fputc('"', stream) == EOF) return -1;
    while (*cursor != '\0') {
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
        } else if (value < 0x20U || value >= 0x80U) {
            if (fprintf(stream, "\\u%04x", (unsigned)value) < 0) return -1;
        } else if (fputc((int)value, stream) == EOF) return -1;
    }
    return fputc('"', stream) == EOF ? -1 : 0;
}

static int hwa_experiment_write_number(FILE *stream,
                                       const HWANumericLocale *locale,
                                       double value)
{
    char text[128];
    if (hwa_c_locale_format_double(locale, text, sizeof(text), value) != 0)
        return -1;
    return fputs(text, stream) == EOF ? -1 : 0;
}

static uint64_t hwa_experiment_job_seed(uint64_t seed,
                                        size_t point,
                                        size_t case_index,
                                        size_t replicate)
{
    uint64_t state = seed;
    state ^= (uint64_t)point * UINT64_C(0x9e3779b97f4a7c15);
    state ^= (uint64_t)case_index * UINT64_C(0xbf58476d1ce4e5b9);
    state ^= (uint64_t)replicate * UINT64_C(0x94d049bb133111eb);
    return hwa_experiment_splitmix64(&state);
}

static int hwa_experiment_hash_job(const HWAExperimentResult *result,
                                   size_t point,
                                   size_t case_index,
                                   size_t replicate,
                                   uint64_t seed,
                                   char key[HWA_SHA256_HEX_SIZE])
{
    HWASha256 context;
    unsigned char digest[32];
    char text[1024];
    int length;
    size_t index;
    const HWARunOptions *run = &result->options.run;
    hwa_sha256_init(&context);
#define HWA_JOB_HASH_TEXT(value)                                           \
    hwa_sha256_update(&context, (const unsigned char *)(value), strlen(value))
    HWA_JOB_HASH_TEXT("{\"manifest_sha256\":\"");
    HWA_JOB_HASH_TEXT(result->manifest_sha256);
    HWA_JOB_HASH_TEXT("\",\"inputs\":[");
    for (index = 0U; index < result->input_count; ++index) {
        if (index != 0U) HWA_JOB_HASH_TEXT(",");
        HWA_JOB_HASH_TEXT("{\"id\":\"");
        HWA_JOB_HASH_TEXT(result->inputs[index].binding_id);
        HWA_JOB_HASH_TEXT("\",\"sha256\":\"");
        HWA_JOB_HASH_TEXT(result->inputs[index].sha256);
        HWA_JOB_HASH_TEXT("\"}");
    }
    HWA_JOB_HASH_TEXT("],\"renderer\":{\"id\":\"");
    HWA_JOB_HASH_TEXT(result->renderer_id);
    HWA_JOB_HASH_TEXT("\",\"sha256\":\"");
    HWA_JOB_HASH_TEXT(result->renderer_sha256);
    HWA_JOB_HASH_TEXT("\"},\"stage7\":{\"method_version\":\"");
    HWA_JOB_HASH_TEXT(HWA_RUN_METHOD_VERSION);
    length = snprintf(text, sizeof(text),
                      "\",\"decode_block_frames\":%zu},",
                      run->decode_block_frames);
    if (length < 0 || (size_t)length >= sizeof(text)) return -1;
    hwa_sha256_update(&context, (const unsigned char *)text, (size_t)length);
    length = snprintf(
        text, sizeof(text),
        "\"case\":\"%s\",\"point\":\"%s\",\"replicate\":%zu,"
        "\"seed\":%" PRIu64 "}\n",
        result->cases[case_index].name, result->points[point].key,
        replicate, seed);
    if (length < 0 || (size_t)length >= sizeof(text)) return -1;
    hwa_sha256_update(&context, (const unsigned char *)text, (size_t)length);
    hwa_sha256_final(&context, digest);
    hwa_sha256_hex(digest, key);
#undef HWA_JOB_HASH_TEXT
    return 0;
}

static size_t hwa_experiment_case_output_count(
    const HWAExperimentCaseTemplate *item)
{
    size_t count = 0U;
    size_t index;
    for (index = 0U; index < item->stem_count; ++index)
        if (item->stems[index].output != NULL) count++;
    for (index = 0U; index < item->probe_count; ++index)
        if (item->probes[index].output != NULL) count++;
    return count;
}

static int hwa_experiment_case_output_at(
    const HWAExperimentCaseTemplate *item,
    size_t output_index,
    const char **resource_id,
    const char **name,
    HWARunSourceKind *kind)
{
    size_t index;
    size_t current = 0U;
    if (item == NULL || resource_id == NULL || name == NULL || kind == NULL)
        return -1;
    for (index = 0U; index < item->stem_count; ++index) {
        if (item->stems[index].output == NULL) continue;
        if (current++ == output_index) {
            *resource_id = item->stems[index].id;
            *name = item->stems[index].output;
            *kind = HWA_RUN_SOURCE_STEM;
            return 0;
        }
    }
    for (index = 0U; index < item->probe_count; ++index) {
        if (item->probes[index].output == NULL) continue;
        if (current++ == output_index) {
            *resource_id = item->probes[index].id;
            *name = item->probes[index].output;
            *kind = HWA_RUN_SOURCE_PROBE;
            return 0;
        }
    }
    return -1;
}

static int hwa_experiment_request_write(
    const char *path,
    const HWAExperimentResult *result,
    const HWAExperimentJob *job,
    const HWAExperimentCase *record,
    const HWAExperimentRenderParameter *parameters,
    size_t parameter_count,
    const HWAExperimentRenderInput *inputs,
    size_t input_count,
    const HWAExperimentRenderOutput *outputs,
    size_t output_count,
    const HWANumericLocale *locale,
    char *error,
    size_t error_size)
{
    FILE *stream;
    size_t index;
#if defined(_WIN32)
    int descriptor = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                           _S_IREAD | _S_IWRITE);
#else
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
#endif
    if (descriptor < 0) goto failed;
#if defined(_WIN32)
    stream = _fdopen(descriptor, "wb");
#else
    stream = fdopen(descriptor, "wb");
#endif
    if (stream == NULL) goto failed_descriptor;
    if (fputs("{\"schema\":\"hwa-render-job\",\"schema_version\":1,"
              "\"method_version\":\"stage8-1\",\"case_id\":",
              stream) == EOF ||
        hwa_experiment_json_quoted(stream, record->name) != 0 ||
        fputs(",\"job_id\":", stream) == EOF ||
        fprintf(stream, "%" PRIu64, job->id) < 0 ||
        fputs(",\"job_key\":", stream) == EOF ||
        hwa_experiment_json_quoted(stream, job->key) != 0 ||
        fputs(",\"inputs\":[", stream) == EOF) goto failed_stream;
    for (index = 0U; index < input_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            fputs("{\"binding_id\":", stream) == EOF ||
            hwa_experiment_json_quoted(stream, inputs[index].binding_id) != 0 ||
            fprintf(stream,
                    ",\"channels\":%u,\"gain_db\":",
                    (unsigned)inputs[index].channels) < 0 ||
            hwa_experiment_write_number(stream, locale,
                                        inputs[index].gain_db) != 0 ||
            fputs(",\"kind\":", stream) == EOF ||
            hwa_experiment_json_quoted(
                stream, hwa_run_source_kind_name(inputs[index].kind)) != 0 ||
            fputs(",\"path\":", stream) == EOF ||
            hwa_experiment_json_quoted(stream, inputs[index].path) != 0 ||
            fputs(",\"probe_format\":", stream) == EOF ||
            (inputs[index].kind == HWA_RUN_SOURCE_PROBE
                 ? hwa_experiment_json_quoted(
                       stream, hwa_run_probe_format_name(
                                   inputs[index].probe_format))
                 : fputs("null", stream) == EOF ? -1 : 0) != 0 ||
            fputs(",\"probe_name\":", stream) == EOF ||
            (inputs[index].probe_name != NULL
                 ? hwa_experiment_json_quoted(stream,
                                              inputs[index].probe_name)
                 : fputs("null", stream) == EOF ? -1 : 0) != 0 ||
            fprintf(stream,
                    ",\"rate_denominator\":%" PRIu64
                    ",\"rate_hz\":%u,\"rate_numerator\":%" PRIu64,
                    inputs[index].rate_denominator,
                    inputs[index].rate_hz,
                    inputs[index].rate_numerator) < 0 ||
            fputs(",\"resource_id\":", stream) == EOF ||
            hwa_experiment_json_quoted(stream, inputs[index].resource_id) != 0 ||
            fputs(",\"role\":", stream) == EOF ||
            (inputs[index].kind == HWA_RUN_SOURCE_STEM
                 ? hwa_experiment_json_quoted(
                       stream, hwa_run_stem_role_name(inputs[index].role))
                 : fputs("null", stream) == EOF ? -1 : 0) != 0 ||
            fputs(",\"sha256\":", stream) == EOF ||
            hwa_experiment_json_quoted(stream, inputs[index].sha256) != 0 ||
            fputs(",\"side\":", stream) == EOF ||
            hwa_experiment_json_quoted(
                stream, hwa_run_side_name(inputs[index].side)) != 0 ||
            fprintf(stream, ",\"start_sample\":%" PRId64,
                    inputs[index].start_sample) < 0 ||
            fputs(",\"unit\":", stream) == EOF ||
            (inputs[index].unit != NULL
                 ? hwa_experiment_json_quoted(stream, inputs[index].unit)
                 : fputs("null", stream) == EOF ? -1 : 0) != 0 ||
            fprintf(stream, ",\"value_count\":%" PRIu64 "}",
                    inputs[index].value_count) < 0) goto failed_stream;
    }
    if (fputs("],\"outputs\":[", stream) == EOF) goto failed_stream;
    for (index = 0U; index < output_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            fputs("{\"id\":", stream) == EOF ||
            hwa_experiment_json_quoted(stream, outputs[index].id) != 0 ||
            fputs(",\"kind\":", stream) == EOF ||
            hwa_experiment_json_quoted(
                stream, hwa_run_source_kind_name(outputs[index].kind)) != 0 ||
            fputs(",\"path\":", stream) == EOF ||
            hwa_experiment_json_quoted(stream, outputs[index].path) != 0 ||
            fputs(",\"side\":", stream) == EOF ||
            hwa_experiment_json_quoted(
                stream, hwa_run_side_name(outputs[index].side)) != 0 ||
            fputs(",\"role\":", stream) == EOF ||
            (outputs[index].kind == HWA_RUN_SOURCE_STEM
                 ? hwa_experiment_json_quoted(
                       stream, hwa_run_stem_role_name(outputs[index].role))
                 : fputs("null", stream) == EOF ? -1 : 0) != 0 ||
            fputs(",\"probe_format\":", stream) == EOF ||
            (outputs[index].kind == HWA_RUN_SOURCE_PROBE
                 ? hwa_experiment_json_quoted(
                       stream, hwa_run_probe_format_name(
                                   outputs[index].probe_format))
                 : fputs("null", stream) == EOF ? -1 : 0) != 0 ||
            fputs(",\"probe_name\":", stream) == EOF ||
            (outputs[index].probe_name != NULL
                 ? hwa_experiment_json_quoted(stream,
                                              outputs[index].probe_name)
                 : fputs("null", stream) == EOF ? -1 : 0) != 0 ||
            fputs(",\"unit\":", stream) == EOF ||
            (outputs[index].unit != NULL
                 ? hwa_experiment_json_quoted(stream, outputs[index].unit)
                 : fputs("null", stream) == EOF ? -1 : 0) != 0 ||
            fprintf(stream,
                    ",\"start_sample\":%" PRId64 ",\"gain_db\":",
                    outputs[index].start_sample) < 0 ||
            hwa_experiment_write_number(stream, locale,
                                        outputs[index].gain_db) != 0 ||
            fprintf(stream,
                    ",\"rate_hz\":%u,\"channels\":%u,"
                    "\"rate_numerator\":%" PRIu64
                    ",\"rate_denominator\":%" PRIu64
                    ",\"value_count\":%" PRIu64 "}",
                    outputs[index].rate_hz,
                    (unsigned)outputs[index].channels,
                    outputs[index].rate_numerator,
                    outputs[index].rate_denominator,
                    outputs[index].value_count) < 0) goto failed_stream;
    }
    if (fputs("],\"parameters\":[", stream) == EOF) goto failed_stream;
    for (index = 0U; index < parameter_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            fputs("{\"id\":", stream) == EOF ||
            hwa_experiment_json_quoted(stream, parameters[index].id) != 0 ||
            fputs(",\"unit\":", stream) == EOF ||
            hwa_experiment_json_quoted(stream, parameters[index].unit) != 0 ||
            fputs(",\"value\":", stream) == EOF ||
            hwa_experiment_write_number(stream, locale,
                                        parameters[index].value) != 0 ||
            fputc('}', stream) == EOF) goto failed_stream;
    }
    if (fprintf(stream,
                "],\"replicate\":%zu,\"seed\":%" PRIu64 ",\"split\":",
                job->replicate, job->seed) < 0 ||
        hwa_experiment_json_quoted(
            stream, hwa_experiment_split_name(record->split)) != 0 ||
        fputs("}\n", stream) == EOF || fflush(stream) != 0)
        goto failed_stream;
    if (fclose(stream) != 0) goto failed_after_close;
    (void)result;
    return 0;
failed_stream:
    (void)fclose(stream);
    goto failed_after_close;
failed_descriptor:
#if defined(_WIN32)
    _close(descriptor);
#else
    close(descriptor);
#endif
failed_after_close:
    (void)remove(path);
failed:
    hwa_set_error(error, error_size, "cannot write Stage 8 request");
    return -1;
}

static const HWAExperimentInput *hwa_experiment_result_input_find(
    const HWAExperimentResult *result,
    const char *id)
{
    size_t index;
    for (index = 0U; index < result->input_count; ++index)
        if (strcmp(result->inputs[index].binding_id, id) == 0)
            return &result->inputs[index];
    return NULL;
}

static int hwa_experiment_run_manifest_write(
    const char *path,
    const HWAExperimentResult *result,
    const HWAExperimentCaseTemplate *item,
    const HWAExperimentArtifact *artifacts,
    size_t artifact_count,
    const HWANumericLocale *locale,
    HWARunBinding *bindings,
    char *error,
    size_t error_size)
{
    FILE *stream;
    size_t index;
    size_t binding_index = 0U;
#if defined(_WIN32)
    int descriptor = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                           _S_IREAD | _S_IWRITE);
#else
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
#endif
    if (descriptor < 0) goto failed;
#if defined(_WIN32)
    stream = _fdopen(descriptor, "wb");
#else
    stream = fdopen(descriptor, "wb");
#endif
    if (stream == NULL) goto failed_descriptor;
    if (fprintf(stream,
                "{\"schema\":\"hwa-run\",\"schema_version\":1,"
                "\"method_version\":\"%s\",\"clock_rate_hz\":%u,"
                "\"stems\":[",
                HWA_RUN_METHOD_VERSION, result->options.run.max_input_frames != 0U
                    ? item->stems[0].rate_hz : 0U) < 0) goto failed_stream;
    for (index = 0U; index < item->stem_count; ++index) {
        const HWAExperimentStemTemplate *stem = &item->stems[index];
        const char *hash = NULL;
        const char *source_path = NULL;
        size_t artifact_index;
        if (stem->input_id != NULL) {
            const HWAExperimentInput *input =
                hwa_experiment_result_input_find(result, stem->input_id);
            if (input == NULL) goto failed_stream;
            hash = input->sha256;
            source_path = input->path;
        } else {
            for (artifact_index = 0U; artifact_index < artifact_count;
                 ++artifact_index)
                if (strcmp(artifacts[artifact_index].resource_id, stem->id) == 0) {
                    hash = artifacts[artifact_index].sha256;
                    source_path = artifacts[artifact_index].path;
                }
        }
        if (hash == NULL || source_path == NULL ||
            (index != 0U && fputc(',', stream) == EOF) ||
            fputs("{\"id\":", stream) == EOF ||
            hwa_experiment_json_quoted(stream, stem->id) != 0 ||
            fputs(",\"side\":", stream) == EOF ||
            hwa_experiment_json_quoted(stream,
                                       hwa_run_side_name(stem->side)) != 0 ||
            fputs(",\"role\":", stream) == EOF ||
            hwa_experiment_json_quoted(stream,
                                       hwa_run_stem_role_name(stem->role)) != 0 ||
            fputs(",\"sha256\":", stream) == EOF ||
            hwa_experiment_json_quoted(stream, hash) != 0 ||
            fprintf(stream, ",\"start_sample\":%" PRId64 ",\"gain_db\":",
                    stem->start_sample) < 0 ||
            hwa_experiment_write_number(stream, locale, stem->gain_db) != 0 ||
            fprintf(stream, ",\"rate_hz\":%u,\"channels\":%u}",
                    stem->rate_hz, (unsigned)stem->channels) < 0)
            goto failed_stream;
        bindings[binding_index].id = stem->id;
        bindings[binding_index].path = source_path;
        binding_index++;
    }
    if (fputs("],\"probes\":[", stream) == EOF) goto failed_stream;
    for (index = 0U; index < item->probe_count; ++index) {
        const HWAExperimentProbeTemplate *probe = &item->probes[index];
        const char *hash = NULL;
        const char *source_path = NULL;
        size_t artifact_index;
        if (probe->input_id != NULL) {
            const HWAExperimentInput *input =
                hwa_experiment_result_input_find(result, probe->input_id);
            if (input == NULL) goto failed_stream;
            hash = input->sha256;
            source_path = input->path;
        } else {
            for (artifact_index = 0U; artifact_index < artifact_count;
                 ++artifact_index)
                if (strcmp(artifacts[artifact_index].resource_id, probe->id) == 0) {
                    hash = artifacts[artifact_index].sha256;
                    source_path = artifacts[artifact_index].path;
                }
        }
        if (hash == NULL || source_path == NULL ||
            (index != 0U && fputc(',', stream) == EOF) ||
            fputs("{\"id\":", stream) == EOF ||
            hwa_experiment_json_quoted(stream, probe->id) != 0 ||
            fputs(",\"side\":", stream) == EOF ||
            hwa_experiment_json_quoted(stream,
                                       hwa_run_side_name(probe->side)) != 0 ||
            fputs(",\"name\":", stream) == EOF ||
            hwa_experiment_json_quoted(stream, probe->name) != 0 ||
            fputs(",\"unit\":", stream) == EOF ||
            hwa_experiment_json_quoted(stream, probe->unit) != 0 ||
            fputs(",\"format\":", stream) == EOF ||
            hwa_experiment_json_quoted(
                stream, hwa_run_probe_format_name(probe->format)) != 0 ||
            fputs(",\"sha256\":", stream) == EOF ||
            hwa_experiment_json_quoted(stream, hash) != 0 ||
            fprintf(stream,
                    ",\"start_sample\":%" PRId64
                    ",\"rate_numerator\":%" PRIu64
                    ",\"rate_denominator\":%" PRIu64
                    ",\"value_count\":%" PRIu64 "}",
                    probe->start_sample, probe->rate_numerator,
                    probe->rate_denominator, probe->value_count) < 0)
            goto failed_stream;
        bindings[binding_index].id = probe->id;
        bindings[binding_index].path = source_path;
        binding_index++;
    }
    if (fputs("],\"links\":[", stream) == EOF) goto failed_stream;
    for (index = 0U; index < item->link_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            fputs("{\"stem\":", stream) == EOF ||
            hwa_experiment_json_quoted(stream, item->links[index].stem_id) != 0 ||
            fputs(",\"probe\":", stream) == EOF ||
            hwa_experiment_json_quoted(stream, item->links[index].probe_id) != 0 ||
            fputs(",\"feature\":\"rms_dbfs\"}", stream) == EOF)
            goto failed_stream;
    }
    if (fputs("]}\n", stream) == EOF || fflush(stream) != 0)
        goto failed_stream;
    if (fclose(stream) != 0) goto failed_after_close;
    return 0;
failed_stream:
    (void)fclose(stream);
    goto failed_after_close;
failed_descriptor:
#if defined(_WIN32)
    _close(descriptor);
#else
    close(descriptor);
#endif
failed_after_close:
    (void)remove(path);
failed:
    hwa_set_error(error, error_size, "cannot write Stage 8 run manifest");
    return -1;
}

static const HWARunFeature *hwa_experiment_run_feature_find(
    const HWARunResult *run,
    const HWAExperimentResponse *response)
{
    size_t index;
    for (index = 0U; index < run->feature_count; ++index) {
        const HWARunFeature *feature = &run->features[index];
        if (feature->role == response->role &&
            feature->kind == response->feature &&
            feature->index == response->feature_index) return feature;
    }
    return NULL;
}

static int hwa_experiment_run_result_write(const char *path,
                                           const HWARunResult *run,
                                           char *error,
                                           size_t error_size)
{
    FILE *stream;
#if defined(_WIN32)
    int descriptor = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                           _S_IREAD | _S_IWRITE);
#else
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
#endif
    if (descriptor < 0) goto failed;
#if defined(_WIN32)
    stream = _fdopen(descriptor, "wb");
#else
    stream = fdopen(descriptor, "wb");
#endif
    if (stream == NULL) goto failed_descriptor;
    if (hwa_run_file_write(stream, run, error, error_size) != 0 ||
        fflush(stream) != 0) {
        (void)fclose(stream);
        goto failed_after_close;
    }
    if (fclose(stream) != 0) goto failed_after_close;
    return 0;
failed_descriptor:
#if defined(_WIN32)
    _close(descriptor);
#else
    close(descriptor);
#endif
failed_after_close:
    (void)remove(path);
failed:
    if (error == NULL || error_size == 0U || error[0] == '\0')
        hwa_set_error(error, error_size, "cannot write Stage 8 run result");
    return -1;
}

static int hwa_experiment_run_canonicalize_paths(
    HWARunResult *run,
    char *error,
    size_t error_size)
{
    size_t index;
    uint64_t retained;
    char *manifest = hwa_experiment_copy_string("run.json");
    if (manifest == NULL) goto allocation;
    free(run->manifest_path);
    run->manifest_path = manifest;
    for (index = 0U; index < run->source_count; ++index) {
        HWARunSource *source = &run->sources[index];
        const char *prefix;
        size_t prefix_size;
        size_t id_size;
        char *path;
        prefix = "resources/";
        prefix_size = strlen(prefix);
        id_size = strlen(source->binding_id);
        if (prefix_size > SIZE_MAX - id_size - 1U) goto allocation;
        path = (char *)malloc(prefix_size + id_size + 1U);
        if (path == NULL) goto allocation;
        memcpy(path, prefix, prefix_size);
        memcpy(path + prefix_size, source->binding_id, id_size + 1U);
        free(source->path);
        source->path = path;
    }
    if (hwa_run_result_retained_bytes(run, &retained) != 0 ||
        retained > run->options.max_work_bytes) goto allocation;
    run->retained_work_bytes = retained;
    if (hwa_run_result_validate(run, error, error_size) != 0) return -1;
    return 0;
allocation:
    hwa_set_error(error, error_size,
                  "cannot canonicalize Stage 8 run paths");
    return -1;
}

static int hwa_experiment_job_artifacts(
    const HWAExperimentCaseTemplate *item,
    uint64_t job_id,
    const HWAExperimentRenderOutput *outputs,
    size_t output_count,
    HWAExperimentResult *result,
    char *error,
    size_t error_size)
{
    size_t index;
    if (output_count > result->options.max_artifacts ||
        result->artifact_count >
            result->options.max_artifacts - output_count) {
        hwa_set_error(error, error_size, "Stage 8 artifact cap exceeded");
        return -1;
    }
    for (index = 0U; index < output_count; ++index) {
        HWAExperimentArtifact *artifact =
            &result->artifacts[result->artifact_count];
        uint64_t size;
        const char *expected_id = NULL;
        HWARunSourceKind expected_kind = HWA_RUN_SOURCE_STEM;
        size_t scan;
        for (scan = 0U; scan < item->stem_count; ++scan)
            if (item->stems[scan].output != NULL &&
                strcmp(item->stems[scan].id, outputs[index].id) == 0) {
                expected_id = item->stems[scan].id;
                expected_kind = HWA_RUN_SOURCE_STEM;
            }
        for (scan = 0U; scan < item->probe_count; ++scan)
            if (item->probes[scan].output != NULL &&
                strcmp(item->probes[scan].id, outputs[index].id) == 0) {
                expected_id = item->probes[scan].id;
                expected_kind = HWA_RUN_SOURCE_PROBE;
            }
        if (expected_id == NULL || expected_kind != outputs[index].kind ||
            hwa_experiment_file_regular_size(
                outputs[index].path, result->options.max_output_file_bytes,
                &size) != 0 || size == 0U ||
            size > result->options.max_bundle_bytes ||
            result->total_output_bytes >
                result->options.max_bundle_bytes - size ||
            hwa_sha256_file(outputs[index].path,
                            result->options.max_output_file_bytes,
                            artifact->sha256, error, error_size) != 0) {
            if (error == NULL || error_size == 0U || error[0] == '\0')
                hwa_set_error(error, error_size,
                              "invalid Stage 8 renderer output");
            return -1;
        }
        artifact->id = (uint64_t)result->artifact_count + UINT64_C(1);
        artifact->job_id = job_id;
        artifact->resource_id = hwa_experiment_copy_string(expected_id);
        artifact->path = hwa_experiment_copy_string(outputs[index].path);
        artifact->file_bytes = size;
        artifact->kind = expected_kind;
        if (artifact->resource_id == NULL || artifact->path == NULL) {
            hwa_set_error(error, error_size,
                          "cannot retain Stage 8 artifact");
            return -1;
        }
        result->artifact_count++;
        result->total_output_bytes += size;
    }
    return 0;
}

static int hwa_experiment_now_milliseconds(uint64_t *milliseconds)
{
#if defined(_WIN32)
    *milliseconds = (uint64_t)GetTickCount64();
    return 0;
#else
    struct timespec now;
    if (milliseconds == NULL || clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
        now.tv_sec < 0 || now.tv_nsec < 0) return -1;
    if ((uint64_t)now.tv_sec > UINT64_MAX / UINT64_C(1000)) return -1;
    *milliseconds = (uint64_t)now.tv_sec * UINT64_C(1000) +
        (uint64_t)now.tv_nsec / UINT64_C(1000000);
    return 0;
#endif
}

static int hwa_experiment_deadline_check(uint64_t started,
                                         uint64_t maximum,
                                         char *error,
                                         size_t error_size)
{
    uint64_t now;
    if (hwa_experiment_now_milliseconds(&now) != 0 || now < started ||
        now - started > maximum) {
        hwa_set_error(error, error_size, "Stage 8 time cap exceeded");
        return -1;
    }
    return 0;
}

typedef struct HWAExperimentDeadline {
    uint64_t started;
    uint64_t maximum;
    int active;
} HWAExperimentDeadline;

#if defined(_MSC_VER)
#define HWA_EXPERIMENT_THREAD_LOCAL __declspec(thread)
#else
#define HWA_EXPERIMENT_THREAD_LOCAL _Thread_local
#endif

#define HWA_EXPERIMENT_DEADLINE_DEPTH 32U
static HWA_EXPERIMENT_THREAD_LOCAL HWAExperimentDeadline
    hwa_experiment_deadlines[HWA_EXPERIMENT_DEADLINE_DEPTH];
static HWA_EXPERIMENT_THREAD_LOCAL size_t hwa_experiment_deadline_depth;
static HWA_EXPERIMENT_THREAD_LOCAL size_t hwa_experiment_deadline_overflow;

void hwa_experiment_deadline_enter(uint64_t started, uint64_t maximum)
{
    HWAExperimentDeadline *deadline;
    if (hwa_experiment_deadline_depth >= HWA_EXPERIMENT_DEADLINE_DEPTH) {
        hwa_experiment_deadline_overflow++;
        return;
    }
    deadline = &hwa_experiment_deadlines[hwa_experiment_deadline_depth++];
    deadline->started = started;
    deadline->maximum = maximum;
    deadline->active = 1;
}

void hwa_experiment_deadline_leave(void)
{
    if (hwa_experiment_deadline_overflow != 0U) {
        hwa_experiment_deadline_overflow--;
        return;
    }
    if (hwa_experiment_deadline_depth == 0U) return;
    hwa_experiment_deadline_depth--;
    memset(&hwa_experiment_deadlines[hwa_experiment_deadline_depth], 0,
           sizeof(hwa_experiment_deadlines[hwa_experiment_deadline_depth]));
}

int hwa_experiment_deadline_poll(char *error, size_t error_size)
{
    const HWAExperimentDeadline *deadline;
    if (hwa_experiment_deadline_overflow != 0U) {
        hwa_set_error(error, error_size, "Stage 8 deadline nesting exceeded");
        return -1;
    }
    if (hwa_experiment_deadline_depth == 0U) return 0;
    deadline = &hwa_experiment_deadlines[hwa_experiment_deadline_depth - 1U];
    return hwa_experiment_deadline_check(
        deadline->started, deadline->maximum,
        error, error_size);
}

static int hwa_experiment_jobs_allocate(HWAExperimentResult *result,
                                        const HWAExperimentManifest *manifest,
                                        size_t *artifact_capacity,
                                        char *error,
                                        size_t error_size)
{
    uint64_t job_count;
    uint64_t observation_count;
    uint64_t max_outputs = 0U;
    size_t case_index;
    if (hwa_experiment_u64_multiply((uint64_t)result->point_count,
                                    (uint64_t)result->case_count,
                                    &job_count) != 0 ||
        hwa_experiment_u64_multiply(job_count,
                                    (uint64_t)result->plan_replicates,
                                    &job_count) != 0 ||
        job_count == 0U || job_count > result->options.max_jobs ||
        job_count > SIZE_MAX ||
        hwa_experiment_u64_multiply(job_count,
                                    (uint64_t)result->response_count,
                                    &observation_count) != 0 ||
        observation_count > result->options.max_observations ||
        observation_count > SIZE_MAX) {
        hwa_set_error(error, error_size, "Stage 8 job or observation cap exceeded");
        return -1;
    }
    for (case_index = 0U; case_index < manifest->case_count; ++case_index) {
        uint64_t count = (uint64_t)hwa_experiment_case_output_count(
            &manifest->case_templates[case_index]);
        if (hwa_experiment_u64_add(max_outputs, count, &max_outputs) != 0)
            return -1;
    }
    {
        uint64_t point_replicates;
        if (hwa_experiment_u64_multiply(
                (uint64_t)result->point_count,
                (uint64_t)result->plan_replicates,
                &point_replicates) != 0 ||
            hwa_experiment_u64_multiply(
                max_outputs, point_replicates, &max_outputs) != 0)
            max_outputs = UINT64_MAX;
    }
    if (
        max_outputs > result->options.max_artifacts || max_outputs > SIZE_MAX) {
        hwa_set_error(error, error_size, "Stage 8 artifact cap exceeded");
        return -1;
    }
    result->jobs = (HWAExperimentJob *)calloc((size_t)job_count,
                                               sizeof(*result->jobs));
    result->observations = (HWAExperimentObservation *)calloc(
        (size_t)observation_count, sizeof(*result->observations));
    result->artifacts = (HWAExperimentArtifact *)calloc(
        (size_t)max_outputs, sizeof(*result->artifacts));
    if (result->jobs == NULL || result->observations == NULL ||
        (max_outputs != 0U && result->artifacts == NULL)) {
        hwa_set_error(error, error_size, "cannot allocate Stage 8 jobs");
        return -1;
    }
    result->job_count = (size_t)job_count;
    result->observation_count = (size_t)observation_count;
    if (artifact_capacity != NULL) *artifact_capacity = (size_t)max_outputs;
    return 0;
}

static int hwa_experiment_remove_tree(const char *path);
static int hwa_experiment_job_file_expected(
    const HWAExperimentCaseTemplate *item,
    const char *name);

static int hwa_experiment_remove_scratch_file(const char *path,
                                              int required)
{
#if defined(_WIN32)
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return !required && GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY |
                       FILE_ATTRIBUTE_REPARSE_POINT |
                       FILE_ATTRIBUTE_DEVICE)) != 0U) return -1;
#else
    struct stat info;
    if (lstat(path, &info) != 0)
        return !required && errno == ENOENT ? 0 : -1;
    if (!S_ISREG(info.st_mode)) return -1;
#endif
    return remove(path);
}

static int hwa_experiment_job_file_expected(
    const HWAExperimentCaseTemplate *item,
    const char *name);

static int hwa_experiment_callback_directory_check(
    const char *job_directory,
    const HWAExperimentCaseTemplate *item,
    uint64_t maximum,
    char *error,
    size_t error_size)
{
    uint64_t total = 0U;
#if defined(_WIN32)
    WIN32_FIND_DATAA entry;
    HANDLE search;
    char *pattern = NULL;
    if (hwa_experiment_path_join(&pattern, job_directory, "*") != 0)
        goto failed;
    search = FindFirstFileA(pattern, &entry);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) goto failed;
    for (;;) {
        if (strcmp(entry.cFileName, ".") != 0 &&
            strcmp(entry.cFileName, "..") != 0) {
            uint64_t size = ((uint64_t)entry.nFileSizeHigh << 32U) |
                entry.nFileSizeLow;
            int allowed = hwa_experiment_job_file_expected(item,
                entry.cFileName) ||
                strcmp(entry.cFileName, "request.json") == 0 ||
                strcmp(entry.cFileName, "stdout.txt") == 0 ||
                strcmp(entry.cFileName, "stderr.txt") == 0;
            if (!allowed ||
                (entry.dwFileAttributes &
                 (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                  FILE_ATTRIBUTE_DEVICE)) != 0U ||
                total > maximum || size > maximum - total) {
                FindClose(search);
                goto failed;
            }
            total += size;
        }
        if (!FindNextFileA(search, &entry)) break;
    }
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        FindClose(search);
        goto failed;
    }
    FindClose(search);
#else
    DIR *directory = opendir(job_directory);
    struct dirent *entry;
    if (directory == NULL) goto failed;
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        char *path = NULL;
        struct stat information;
        int allowed;
        uint64_t size;
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            errno = 0;
            continue;
        }
        allowed = hwa_experiment_job_file_expected(item, entry->d_name) ||
            strcmp(entry->d_name, "request.json") == 0 ||
            strcmp(entry->d_name, "stdout.txt") == 0 ||
            strcmp(entry->d_name, "stderr.txt") == 0;
        if (!allowed || hwa_experiment_path_join(&path, job_directory,
                                                  entry->d_name) != 0 ||
            lstat(path, &information) != 0 ||
            !S_ISREG(information.st_mode) || information.st_size < 0) {
            free(path);
            (void)closedir(directory);
            goto failed;
        }
        free(path);
        size = (uint64_t)information.st_size;
        if (total > maximum || size > maximum - total) {
            (void)closedir(directory);
            goto failed;
        }
        total += size;
        errno = 0;
    }
    if (errno != 0 || closedir(directory) != 0) goto failed;
#endif
    return 0;
failed:
    hwa_set_error(error, error_size,
                  "renderer exceeded its Stage 8 job file authority");
    return -1;
}

static int hwa_experiment_job_file_expected(
    const HWAExperimentCaseTemplate *item,
    const char *name)
{
    size_t index;
    if (strcmp(name, "run.json") == 0 ||
        strcmp(name, "result.hwa-run") == 0) return 1;
    for (index = 0U; index < item->stem_count; ++index)
        if (item->stems[index].output != NULL &&
            strcmp(name, item->stems[index].output) == 0) return 1;
    for (index = 0U; index < item->probe_count; ++index)
        if (item->probes[index].output != NULL &&
            strcmp(name, item->probes[index].output) == 0) return 1;
    return 0;
}

static int hwa_experiment_job_directory_exact(
    const char *job_directory,
    const HWAExperimentCaseTemplate *item,
    const char *run_manifest_path,
    const char run_manifest_sha256[HWA_SHA256_HEX_SIZE],
    const char *run_result_path,
    const char run_result_sha256[HWA_SHA256_HEX_SIZE],
    const HWAExperimentArtifact *artifacts,
    size_t artifact_count,
    const HWAExperimentOptions *options,
    HWAExperimentFileIdentity *run_manifest_identity,
    HWAExperimentFileIdentity *run_result_identity,
    HWAExperimentFileIdentity *artifact_identities,
    uint64_t *bundle_bytes,
    char *error,
    size_t error_size)
{
    size_t seen = 0U;
    size_t expected = artifact_count + 2U;
    uint64_t total = 0U;
    uint64_t manifest_maximum = options->max_manifest_bytes;
    if (manifest_maximum > options->max_output_file_bytes)
        manifest_maximum = options->max_output_file_bytes;
    if (manifest_maximum > options->run.max_manifest_bytes)
        manifest_maximum = options->run.max_manifest_bytes;
    size_t index;
#if defined(_WIN32)
    WIN32_FIND_DATAA entry;
    HANDLE search;
    char *pattern = NULL;
    if (hwa_experiment_path_join(&pattern, job_directory, "*") != 0)
        goto failed;
    search = FindFirstFileA(pattern, &entry);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) goto failed;
    for (;;) {
        if (strcmp(entry.cFileName, ".") == 0 ||
            strcmp(entry.cFileName, "..") == 0) {
            if (!FindNextFileA(search, &entry)) break;
            continue;
        }
        if (!hwa_experiment_job_file_expected(item, entry.cFileName) ||
            (entry.dwFileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
              FILE_ATTRIBUTE_DEVICE)) != 0U) {
            FindClose(search);
            goto failed;
        }
        seen++;
        if (!FindNextFileA(search, &entry)) break;
    }
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        FindClose(search);
        goto failed;
    }
    FindClose(search);
#else
    DIR *directory = opendir(job_directory);
    struct dirent *entry;
    if (directory == NULL) goto failed;
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        char *path = NULL;
        struct stat info;
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;
        if (!hwa_experiment_job_file_expected(item, entry->d_name) ||
            hwa_experiment_path_join(&path, job_directory,
                                     entry->d_name) != 0 ||
            lstat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
            free(path);
            (void)closedir(directory);
            goto failed;
        }
        free(path);
        seen++;
        errno = 0;
    }
    if (errno != 0) {
        (void)closedir(directory);
        goto failed;
    }
    if (closedir(directory) != 0) goto failed;
#endif
    if (seen != expected) goto failed;
    {
        uint64_t size;
        char hash[HWA_SHA256_HEX_SIZE];
        if (hwa_experiment_file_regular_size(
                run_manifest_path, manifest_maximum, &size) != 0 ||
            hwa_experiment_hash_file_identity(
                run_manifest_path, manifest_maximum,
                run_manifest_identity != NULL && run_manifest_identity->valid
                    ? run_manifest_identity : NULL,
                run_manifest_identity, hash, error, error_size) != 0 ||
            strcmp(hash, run_manifest_sha256) != 0 ||
            hwa_experiment_u64_add(total, size, &total) != 0 ||
            hwa_experiment_file_regular_size(
                run_result_path, options->max_output_file_bytes, &size) != 0 ||
            hwa_experiment_hash_file_identity(
                run_result_path, options->max_output_file_bytes,
                run_result_identity != NULL && run_result_identity->valid
                    ? run_result_identity : NULL,
                run_result_identity, hash, error, error_size) != 0 ||
            strcmp(hash, run_result_sha256) != 0 ||
            hwa_experiment_u64_add(total, size, &total) != 0)
            goto failed;
    }
    for (index = 0U; index < artifact_count; ++index) {
        uint64_t size;
        char hash[HWA_SHA256_HEX_SIZE];
        if (hwa_experiment_file_regular_size(
                artifacts[index].path, options->max_output_file_bytes,
                &size) != 0 || size != artifacts[index].file_bytes ||
            hwa_experiment_hash_file_identity(
                artifacts[index].path, options->max_output_file_bytes,
                artifact_identities != NULL && artifact_identities[index].valid
                    ? &artifact_identities[index] : NULL,
                artifact_identities == NULL ? NULL : &artifact_identities[index],
                hash, error, error_size) != 0 ||
            strcmp(hash, artifacts[index].sha256) != 0 ||
            hwa_experiment_u64_add(total, size, &total) != 0)
            goto failed;
    }
    if (total > options->max_bundle_bytes ||
        *bundle_bytes > options->max_bundle_bytes - total) goto failed;
    *bundle_bytes += total;
    return 0;
failed:
    hwa_set_error(error, error_size,
                  "invalid Stage 8 job directory contents");
    return -1;
}

static int hwa_experiment_copy_file_checked(
    const char *source,
    const char *destination,
    uint64_t maximum,
    const char expected_sha256[HWA_SHA256_HEX_SIZE],
    uint64_t expected_size,
    char *error,
    size_t error_size)
{
    FILE *input = NULL;
    FILE *output = NULL;
    unsigned char buffer[65536];
    uint64_t total = 0U;
    char hash[HWA_SHA256_HEX_SIZE];
    HWAExperimentFileIdentity source_before;
    HWAExperimentFileIdentity source_after;
    int status = -1;
#if defined(_WIN32)
    int descriptor = _open(destination,
        _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
        _S_IREAD | _S_IWRITE);
#else
    int descriptor = open(destination,
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
#endif
    if (descriptor < 0) goto cleanup;
#if defined(_WIN32)
    output = _fdopen(descriptor, "wb");
#else
    output = fdopen(descriptor, "wb");
#endif
    if (output == NULL) {
#if defined(_WIN32)
        _close(descriptor);
#else
        close(descriptor);
#endif
        goto cleanup;
    }
    if (hwa_experiment_file_identity_read(source, maximum,
                                          &source_before) != 0 ||
        source_before.size != expected_size)
        goto cleanup;
#if defined(_WIN32)
    {
        HANDLE source_handle = CreateFileA(
            source, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        int source_descriptor;
        if (source_handle == INVALID_HANDLE_VALUE) goto cleanup;
        source_descriptor = _open_osfhandle(
            (intptr_t)source_handle, _O_RDONLY | _O_BINARY);
        if (source_descriptor < 0) {
            CloseHandle(source_handle);
            goto cleanup;
        }
        input = _fdopen(source_descriptor, "rb");
        if (input == NULL) {
            _close(source_descriptor);
            goto cleanup;
        }
    }
#else
    {
        int source_descriptor = open(source, O_RDONLY | O_NOFOLLOW);
        if (source_descriptor < 0) goto cleanup;
        input = fdopen(source_descriptor, "rb");
        if (input == NULL) {
            close(source_descriptor);
            goto cleanup;
        }
    }
#endif
    for (;;) {
        size_t count = fread(buffer, 1U, sizeof(buffer), input);
        if (count != 0U) {
            if (total > maximum || (uint64_t)count > maximum - total ||
                fwrite(buffer, 1U, count, output) != count) goto cleanup;
            total += (uint64_t)count;
        }
        if (count < sizeof(buffer)) {
            if (ferror(input)) goto cleanup;
            break;
        }
    }
    if (fclose(input) != 0) goto cleanup;
    input = NULL;
    if (fflush(output) != 0 || fclose(output) != 0) {
        output = NULL;
        goto cleanup;
    }
    output = NULL;
    if (total != expected_size ||
        hwa_experiment_file_identity_read(source, maximum,
                                          &source_after) != 0 ||
        !hwa_experiment_file_identity_equal(&source_before, &source_after) ||
        hwa_sha256_file(destination, maximum, hash, error, error_size) != 0 ||
        strcmp(hash, expected_sha256) != 0) goto cleanup;
    status = 0;
cleanup:
    if (input != NULL) (void)fclose(input);
    if (output != NULL) (void)fclose(output);
    if (status != 0) {
        (void)remove(destination);
        hwa_set_error(error, error_size,
                      "cannot copy checked Stage 8 resume file");
    }
    return status;
}

static int hwa_experiment_resume_job(
    const char *resume_directory,
    const HWAExperimentCaseTemplate *item,
    size_t job_index,
    size_t artifact_start,
    size_t *resume_artifact_cursor,
    const HWAExperimentResult *resume,
    const char *jobs_directory,
    const HWANumericLocale *locale,
    uint64_t experiment_started,
    HWAExperimentFileIdentity *run_manifest_identities,
    HWAExperimentFileIdentity *run_result_identities,
    HWAExperimentFileIdentity *artifact_identities,
    uint64_t *bundle_bytes,
    HWAExperimentResult *result,
    char *error,
    size_t error_size)
{
    const HWAExperimentJob *source_job;
    HWAExperimentJob *job = &result->jobs[job_index];
    HWARunBinding *run_bindings = NULL;
    HWARunResult run;
    size_t source_index;
    size_t source_first;
    size_t source_count = 0U;
    char *source_job_directory = NULL;
    char *destination_job_directory = NULL;
    char *run_source = NULL;
    char *run_destination = NULL;
    char *manifest_source = NULL;
    char *manifest_destination = NULL;
    size_t binding_count = item->stem_count + item->probe_count;
    char source_manifest_hash[HWA_SHA256_HEX_SIZE];
    char source_run_hash[HWA_SHA256_HEX_SIZE];
    HWAExperimentFileIdentity source_manifest_identity;
    uint64_t manifest_maximum = result->options.max_manifest_bytes;
    uint64_t now;
    int status = -1;
    memset(&run, 0, sizeof(run));
    memset(&source_manifest_identity, 0, sizeof(source_manifest_identity));
    if (manifest_maximum > result->options.max_output_file_bytes)
        manifest_maximum = result->options.max_output_file_bytes;
    if (manifest_maximum > result->options.run.max_manifest_bytes)
        manifest_maximum = result->options.run.max_manifest_bytes;
    if (resume == NULL || job_index >= resume->job_count ||
        strcmp(resume->jobs[job_index].key, job->key) != 0) return 0;
    source_job = &resume->jobs[job_index];
    if (resume_artifact_cursor == NULL) return -1;
    source_first = *resume_artifact_cursor;
    while (*resume_artifact_cursor < resume->artifact_count &&
           resume->artifacts[*resume_artifact_cursor].job_id == source_job->id) {
        (*resume_artifact_cursor)++;
        source_count++;
    }
    if (source_count != hwa_experiment_case_output_count(item)) return 0;
    if (source_job->run_evaluations > result->options.run.max_evaluations ||
        result->total_run_evaluations >
            result->options.max_total_run_evaluations ||
        source_job->run_evaluations >
            result->options.max_total_run_evaluations -
                result->total_run_evaluations ||
        result->total_output_bytes > result->options.max_bundle_bytes ||
        source_job->output_bytes > result->options.max_bundle_bytes -
            result->total_output_bytes) {
        hwa_set_error(error, error_size,
                      "resumed Stage 8 job exceeds current caps");
        return -1;
    }
    if (hwa_experiment_path_join(&source_job_directory, resume_directory,
                                 "jobs") != 0 ||
        hwa_experiment_path_join(&run_source, source_job_directory,
                                 source_job->key) != 0) goto cleanup;
    free(source_job_directory);
    source_job_directory = run_source;
    run_source = NULL;
    if (hwa_experiment_path_join(&destination_job_directory, jobs_directory,
                                 job->key) != 0 ||
        hwa_experiment_mkdir_new(destination_job_directory) != 0 ||
        hwa_experiment_path_join(&manifest_source, source_job_directory,
                                 "run.json") != 0 ||
        hwa_experiment_path_join(&manifest_destination,
                                 destination_job_directory,
                                 "run.json") != 0 ||
        hwa_experiment_path_join(&run_source, source_job_directory,
                                 "result.hwa-run") != 0 ||
        hwa_experiment_path_join(&run_destination,
                                 destination_job_directory,
                                 "result.hwa-run") != 0 ||
        hwa_experiment_hash_file_identity(
            manifest_source, manifest_maximum, NULL,
            &source_manifest_identity, source_manifest_hash,
            error, error_size) != 0 ||
        strcmp(source_manifest_hash,
               source_job->run_manifest_sha256) != 0)
        goto cleanup;
    for (source_index = 0U; source_index < source_count; ++source_index) {
        const HWAExperimentArtifact *source_artifact =
            &resume->artifacts[source_first + source_index];
        HWAExperimentArtifact *artifact =
            &result->artifacts[result->artifact_count];
        const char *expected_id = NULL;
        const char *expected_name = NULL;
        HWARunSourceKind expected_kind = HWA_RUN_SOURCE_STEM;
        const char *base = strrchr(source_artifact->path, '/');
        char *source_path = NULL;
        char *destination_path = NULL;
        if (hwa_experiment_case_output_at(
                item, source_index, &expected_id, &expected_name,
                &expected_kind) != 0 ||
            base == NULL || strcmp(base + 1U, expected_name) != 0 ||
            strcmp(source_artifact->resource_id, expected_id) != 0 ||
            source_artifact->kind != expected_kind ||
            hwa_experiment_path_join(&source_path, source_job_directory,
                                     expected_name) != 0 ||
            hwa_experiment_path_join(&destination_path,
                                     destination_job_directory,
                                     expected_name) != 0 ||
            hwa_experiment_copy_file_checked(
                source_path, destination_path,
                result->options.max_output_file_bytes,
                source_artifact->sha256, source_artifact->file_bytes,
                error, error_size) != 0) {
            free(source_path);
            free(destination_path);
            goto cleanup;
        }
        artifact->id = (uint64_t)result->artifact_count + UINT64_C(1);
        artifact->job_id = job->id;
        artifact->resource_id = hwa_experiment_copy_string(expected_id);
        artifact->path = destination_path;
        artifact->file_bytes = source_artifact->file_bytes;
        artifact->kind = expected_kind;
        memcpy(artifact->sha256, source_artifact->sha256,
               HWA_SHA256_HEX_SIZE);
        free(source_path);
        if (artifact->resource_id == NULL) goto cleanup;
        result->artifact_count++;
    }
    run_bindings = (HWARunBinding *)calloc(binding_count,
                                            sizeof(*run_bindings));
    if (run_bindings == NULL ||
        hwa_experiment_run_manifest_write(
            manifest_destination, result, item,
            result->artifacts + artifact_start,
            result->artifact_count - artifact_start, locale, run_bindings,
            error, error_size) != 0 ||
        hwa_experiment_hash_file_identity(
            manifest_destination, manifest_maximum, NULL,
            &run_manifest_identities[job_index], job->run_manifest_sha256,
            error, error_size) != 0 ||
        strcmp(job->run_manifest_sha256,
               source_job->run_manifest_sha256) != 0 ||
        hwa_run_file_read_locale(
            run_source, &result->options.run, &run, source_run_hash,
            locale, error, error_size) != 0 ||
        strcmp(source_run_hash, source_job->run_result_sha256) != 0 ||
        strcmp(run.manifest_sha256, job->run_manifest_sha256) != 0 ||
        run.evaluation_count != source_job->run_evaluations)
        goto cleanup;
    run.options = result->options.run;
    if (hwa_experiment_run_canonicalize_paths(&run, error, error_size) != 0 ||
        hwa_experiment_run_result_write(run_destination, &run,
                                        error, error_size) != 0 ||
        hwa_experiment_hash_file_identity(
            run_destination, result->options.max_output_file_bytes, NULL,
            &run_result_identities[job_index], job->run_result_sha256,
            error, error_size) != 0)
        goto cleanup;
    hwa_run_result_free(&run);
    if (hwa_run_file_read_locale(
            run_destination, &result->options.run, &run, source_run_hash,
            locale, error, error_size) != 0 ||
        strcmp(source_run_hash, job->run_result_sha256) != 0 ||
        strcmp(run.manifest_sha256, job->run_manifest_sha256) != 0 ||
        run.evaluation_count != source_job->run_evaluations)
        goto cleanup;
    *job = *source_job;
    job->id = (uint64_t)job_index + UINT64_C(1);
    memcpy(job->run_manifest_sha256,
           run.manifest_sha256, HWA_SHA256_HEX_SIZE);
    if (hwa_sha256_file(run_destination,
                        result->options.max_output_file_bytes,
                        job->run_result_sha256,
                        error, error_size) != 0)
        goto cleanup;
    job->run_result_path = hwa_experiment_copy_string(run_destination);
    job->duration_milliseconds = 0U;
    job->reused = 1;
    if (job->run_result_path == NULL) goto cleanup;
    result->total_run_evaluations += job->run_evaluations;
    result->total_output_bytes += job->output_bytes;
    result->reused_job_count++;
    for (source_index = 0U; source_index < result->response_count;
         ++source_index) {
        size_t observation_index = job_index * result->response_count +
            source_index;
        HWAExperimentObservation observation;
        const HWAExperimentObservation *saved =
            &resume->observations[observation_index];
        const HWARunFeature *feature = hwa_experiment_run_feature_find(
            &run, &result->responses[source_index]);
        memset(&observation, 0, sizeof(observation));
        observation.id = (uint64_t)observation_index + UINT64_C(1);
        observation.job_id = job->id;
        observation.response_id = result->responses[source_index].id;
        if (feature != NULL && feature->gap_valid) {
            observation.availability = feature->availability;
            observation.value = feature->normalized_gap;
            observation.quality_flags = feature->quality_flags;
            observation.value_valid = 1;
        } else {
            observation.availability = feature == NULL
                ? HWA_RUN_UNAVAILABLE : feature->availability;
        }
        if (observation.id != saved->id ||
            observation.job_id != saved->job_id ||
            observation.response_id != saved->response_id ||
            observation.availability != saved->availability ||
            observation.value != saved->value ||
            observation.quality_flags != saved->quality_flags ||
            observation.value_valid != saved->value_valid) {
            hwa_set_error(error, error_size,
                          "resume observations do not match Stage 7 result");
            goto cleanup;
        }
        result->observations[observation_index] = observation;
    }
    if (hwa_experiment_job_directory_exact(
            destination_job_directory, item, manifest_destination,
            job->run_manifest_sha256, run_destination,
            job->run_result_sha256, result->artifacts + artifact_start,
            result->artifact_count - artifact_start, &result->options,
            &run_manifest_identities[job_index],
            &run_result_identities[job_index],
            artifact_identities + artifact_start,
            bundle_bytes, error, error_size) != 0 ||
        hwa_experiment_now_milliseconds(&now) != 0 ||
        now < experiment_started ||
        now - experiment_started > result->options.max_total_milliseconds)
        goto cleanup;
    status = 1;
cleanup:
    if (status <= 0 && destination_job_directory != NULL)
        (void)hwa_experiment_remove_tree(destination_job_directory);
    free(source_job_directory);
    free(destination_job_directory);
    free(run_source);
    free(run_destination);
    free(manifest_source);
    free(manifest_destination);
    free(run_bindings);
    hwa_run_result_free(&run);
    (void)source_manifest_identity;
    return status;
}

static int hwa_experiment_job_execute(
    const HWAExperimentManifest *manifest,
    size_t point_index,
    size_t case_index,
    size_t replicate,
    const char *jobs_directory,
    const HWAExperimentRenderer *renderer,
    const HWANumericLocale *locale,
    uint64_t experiment_started,
    HWAExperimentFileIdentity *run_manifest_identities,
    HWAExperimentFileIdentity *run_result_identities,
    HWAExperimentFileIdentity *artifact_identities,
    uint64_t *bundle_bytes,
    HWAExperimentResult *result,
    char *error,
    size_t error_size)
{
    size_t job_index =
        (point_index * result->case_count + case_index) *
            result->plan_replicates + replicate;
    HWAExperimentJob *job = &result->jobs[job_index];
    const HWAExperimentCase *record = &result->cases[case_index];
    const HWAExperimentCaseTemplate *item =
        &manifest->case_templates[case_index];
    HWAExperimentRenderParameter *parameters = NULL;
    HWAExperimentRenderInput *inputs = NULL;
    HWAExperimentRenderOutput *outputs = NULL;
    HWARunBinding *run_bindings = NULL;
    char **owned_output_paths = NULL;
    char *job_directory = NULL;
    char *request_path = NULL;
    char *stdout_path = NULL;
    char *stderr_path = NULL;
    char *run_manifest_path = NULL;
    char *run_result_path = NULL;
    size_t output_count = hwa_experiment_case_output_count(item);
    size_t binding_count = item->stem_count + item->probe_count;
    size_t input_count = binding_count - output_count;
    size_t parameter_index;
    size_t output_index = 0U;
    size_t input_index = 0U;
    size_t artifact_start = result->artifact_count;
    uint64_t start = 0U;
    uint64_t finish = 0U;
    HWAExperimentRenderRequest request;
    HWARunResult run;
    uint64_t manifest_size;
    uint64_t request_size;
    uint64_t manifest_maximum = result->options.max_manifest_bytes;
    int status = -1;
    memset(&request, 0, sizeof(request));
    memset(&run, 0, sizeof(run));
    if (manifest_maximum > result->options.max_output_file_bytes)
        manifest_maximum = result->options.max_output_file_bytes;
    if (manifest_maximum > result->options.run.max_manifest_bytes)
        manifest_maximum = result->options.run.max_manifest_bytes;
    job->id = (uint64_t)job_index + UINT64_C(1);
    job->point_id = (uint64_t)point_index + UINT64_C(1);
    job->case_id = (uint64_t)case_index + UINT64_C(1);
    job->replicate = replicate;
    job->seed = hwa_experiment_job_seed(result->plan_seed, point_index,
                                        case_index, replicate);
    if (hwa_experiment_hash_job(result, point_index, case_index, replicate,
                                job->seed, job->key) != 0 ||
        hwa_experiment_path_join(&job_directory, jobs_directory, job->key) != 0 ||
        hwa_experiment_mkdir_new(job_directory) != 0 ||
        hwa_experiment_path_join(&request_path, job_directory, "request.json") != 0 ||
        hwa_experiment_path_join(&stdout_path, job_directory, "stdout.txt") != 0 ||
        hwa_experiment_path_join(&stderr_path, job_directory, "stderr.txt") != 0 ||
        hwa_experiment_path_join(&run_manifest_path, job_directory,
                                 "run.json") != 0 ||
        hwa_experiment_path_join(&run_result_path, job_directory,
                                 "result.hwa-run") != 0) {
        hwa_set_error(error, error_size, "cannot create Stage 8 job paths");
        goto cleanup;
    }
    parameters = (HWAExperimentRenderParameter *)calloc(
        result->parameter_count, sizeof(*parameters));
    inputs = (HWAExperimentRenderInput *)calloc(input_count, sizeof(*inputs));
    outputs = (HWAExperimentRenderOutput *)calloc(output_count,
                                                   sizeof(*outputs));
    owned_output_paths = (char **)calloc(output_count, sizeof(*owned_output_paths));
    run_bindings = (HWARunBinding *)calloc(binding_count,
                                            sizeof(*run_bindings));
    if (parameters == NULL || (input_count != 0U && inputs == NULL) ||
        (output_count != 0U &&
        (outputs == NULL || owned_output_paths == NULL)) ||
        run_bindings == NULL) {
        hwa_set_error(error, error_size, "cannot allocate Stage 8 request");
        goto cleanup;
    }
    for (parameter_index = 0U; parameter_index < result->parameter_count;
         ++parameter_index) {
        const HWAExperimentValue *value = &result->values[
            point_index * result->parameter_count + parameter_index];
        parameters[parameter_index].id = result->parameters[parameter_index].name;
        parameters[parameter_index].unit = result->parameters[parameter_index].unit;
        parameters[parameter_index].value = value->value;
    }
    for (parameter_index = 0U; parameter_index < item->stem_count;
         ++parameter_index) {
        const HWAExperimentStemTemplate *stem = &item->stems[parameter_index];
        if (stem->output == NULL) {
            const HWAExperimentInput *input =
                hwa_experiment_result_input_find(result, stem->input_id);
            if (input == NULL) goto cleanup;
            inputs[input_index].resource_id = stem->id;
            inputs[input_index].binding_id = stem->input_id;
            inputs[input_index].path = input->path;
            inputs[input_index].sha256 = input->sha256;
            inputs[input_index].kind = HWA_RUN_SOURCE_STEM;
            inputs[input_index].side = stem->side;
            inputs[input_index].role = stem->role;
            inputs[input_index].start_sample = stem->start_sample;
            inputs[input_index].gain_db = stem->gain_db;
            inputs[input_index].rate_hz = stem->rate_hz;
            inputs[input_index].channels = stem->channels;
            input_index++;
            continue;
        }
        if (hwa_experiment_path_join(&owned_output_paths[output_index],
                                     job_directory,
                                     stem->output) != 0)
            goto cleanup;
        outputs[output_index].id = stem->id;
        outputs[output_index].path = owned_output_paths[output_index];
        outputs[output_index].kind = HWA_RUN_SOURCE_STEM;
        outputs[output_index].side = stem->side;
        outputs[output_index].role = stem->role;
        outputs[output_index].start_sample = stem->start_sample;
        outputs[output_index].gain_db = stem->gain_db;
        outputs[output_index].rate_hz = stem->rate_hz;
        outputs[output_index].channels = stem->channels;
        output_index++;
    }
    for (parameter_index = 0U; parameter_index < item->probe_count;
         ++parameter_index) {
        const HWAExperimentProbeTemplate *probe = &item->probes[parameter_index];
        if (probe->output == NULL) {
            const HWAExperimentInput *input =
                hwa_experiment_result_input_find(result, probe->input_id);
            if (input == NULL) goto cleanup;
            inputs[input_index].resource_id = probe->id;
            inputs[input_index].binding_id = probe->input_id;
            inputs[input_index].path = input->path;
            inputs[input_index].sha256 = input->sha256;
            inputs[input_index].kind = HWA_RUN_SOURCE_PROBE;
            inputs[input_index].side = probe->side;
            inputs[input_index].probe_format = probe->format;
            inputs[input_index].probe_name = probe->name;
            inputs[input_index].unit = probe->unit;
            inputs[input_index].start_sample = probe->start_sample;
            inputs[input_index].rate_numerator = probe->rate_numerator;
            inputs[input_index].rate_denominator = probe->rate_denominator;
            inputs[input_index].value_count = probe->value_count;
            input_index++;
            continue;
        }
        if (hwa_experiment_path_join(&owned_output_paths[output_index],
                                     job_directory,
                                     probe->output) != 0)
            goto cleanup;
        outputs[output_index].id = probe->id;
        outputs[output_index].path = owned_output_paths[output_index];
        outputs[output_index].kind = HWA_RUN_SOURCE_PROBE;
        outputs[output_index].side = probe->side;
        outputs[output_index].probe_format = probe->format;
        outputs[output_index].probe_name = probe->name;
        outputs[output_index].unit = probe->unit;
        outputs[output_index].start_sample = probe->start_sample;
        outputs[output_index].rate_numerator = probe->rate_numerator;
        outputs[output_index].rate_denominator = probe->rate_denominator;
        outputs[output_index].value_count = probe->value_count;
        output_index++;
    }
    if (input_index != input_count || output_index != output_count) goto cleanup;
    if (hwa_experiment_request_write(
            request_path, result, job, record, parameters,
            result->parameter_count, inputs, input_count,
            outputs, output_count, locale,
            error, error_size) != 0 ||
        hwa_experiment_file_regular_size(
            request_path, result->options.max_output_file_bytes,
            &request_size) != 0)
        goto cleanup;
    request.job_id = job->id;
    request.job_key = job->key;
    request.case_id = record->name;
    request.split = record->split;
    request.replicate = replicate;
    request.seed = job->seed;
    request.job_directory = job_directory;
    request.request_path = request_path;
    request.stdout_path = stdout_path;
    request.stderr_path = stderr_path;
    request.parameters = parameters;
    request.parameter_count = result->parameter_count;
    request.inputs = inputs;
    request.input_count = input_count;
    request.outputs = outputs;
    request.output_count = output_count;
    if (*bundle_bytes >= result->options.max_bundle_bytes) {
        hwa_set_error(error, error_size, "Stage 8 bundle cap exceeded");
        goto cleanup;
    }
    request.max_output_file_bytes = result->options.max_output_file_bytes;
    request.max_output_bytes =
        result->options.max_bundle_bytes - *bundle_bytes;
    if (hwa_experiment_now_milliseconds(&start) != 0 ||
        start < experiment_started ||
        start - experiment_started >=
            result->options.max_total_milliseconds) {
        hwa_set_error(error, error_size, "Stage 8 time cap exceeded");
        goto cleanup;
    }
    request.timeout_milliseconds = result->options.max_job_milliseconds;
    if (request.timeout_milliseconds >
        result->options.max_total_milliseconds -
            (start - experiment_started))
        request.timeout_milliseconds =
            result->options.max_total_milliseconds -
                (start - experiment_started);
    if (renderer->render(renderer->context, &request, error, error_size) != 0) {
        if (error == NULL || error_size == 0U || error[0] == '\0')
            hwa_set_error(error, error_size, "Stage 8 renderer failed");
        goto cleanup;
    }
    if (hwa_experiment_now_milliseconds(&finish) != 0 || finish < start ||
        finish < experiment_started || finish - experiment_started >
            result->options.max_total_milliseconds) {
        hwa_set_error(error, error_size, "Stage 8 time cap exceeded");
        goto cleanup;
    }
    if (hwa_experiment_callback_directory_check(
            job_directory, item, request.max_output_bytes,
            error, error_size) != 0)
        goto cleanup;
    job->duration_milliseconds = finish - start;
    if (job->duration_milliseconds > result->options.max_job_milliseconds ||
        job->duration_milliseconds >
            result->options.max_total_milliseconds ||
        result->total_duration_milliseconds >
            result->options.max_total_milliseconds -
                job->duration_milliseconds) {
        hwa_set_error(error, error_size, "Stage 8 time cap exceeded");
        goto cleanup;
    }
    result->total_duration_milliseconds += job->duration_milliseconds;
    if (hwa_experiment_job_artifacts(item, job->id, outputs, output_count,
                                     result, error, error_size) != 0 ||
        hwa_experiment_run_manifest_write(
            run_manifest_path, result, item,
            result->artifacts + artifact_start,
            result->artifact_count - artifact_start, locale, run_bindings,
            error, error_size) != 0 ||
        hwa_experiment_file_regular_size(
            run_manifest_path, manifest_maximum,
            &manifest_size) != 0 ||
        hwa_sha256_file(run_manifest_path, manifest_maximum,
                        job->run_manifest_sha256, error, error_size) != 0 ||
        result->total_run_evaluations >=
            result->options.max_total_run_evaluations)
        goto cleanup;
    {
        HWAExperimentOptions bounded = result->options;
        uint64_t remaining = result->options.max_total_run_evaluations -
            result->total_run_evaluations;
        if (bounded.run.max_evaluations > remaining)
            bounded.run.max_evaluations = remaining;
        if (hwa_analyze_run_files(run_manifest_path, run_bindings,
                                  binding_count, &bounded.run, &run,
                                  error, error_size) != 0)
            goto cleanup;
    }
    /* Limits do not affect canonical science facts or job-key authority. */
    run.options = result->options.run;
    if (
        hwa_experiment_run_canonicalize_paths(&run,
                                              error, error_size) != 0 ||
        hwa_experiment_run_result_write(run_result_path, &run,
                                        error, error_size) != 0 ||
        hwa_sha256_file(run_result_path, result->options.max_output_file_bytes,
                        job->run_result_sha256, error, error_size) != 0)
        goto cleanup;
    /* Prove that this fresh nested result is reusable under the same caps. */
    hwa_run_result_free(&run);
    {
        char persisted_hash[HWA_SHA256_HEX_SIZE];
        if (hwa_run_file_read_locale(
                run_result_path, &result->options.run, &run,
                persisted_hash, locale, error, error_size) != 0 ||
            strcmp(persisted_hash, job->run_result_sha256) != 0 ||
            strcmp(run.manifest_sha256, job->run_manifest_sha256) != 0) {
            if (error == NULL || error_size == 0U || error[0] == '\0')
                hwa_set_error(error, error_size,
                              "fresh Stage 7 result fails current caps");
            goto cleanup;
        }
    }
    job->run_result_path = hwa_experiment_copy_string(run_result_path);
    job->run_evaluations = run.evaluation_count;
    if (job->run_result_path == NULL ||
        job->run_evaluations >
            result->options.max_total_run_evaluations ||
        result->total_run_evaluations >
            result->options.max_total_run_evaluations - job->run_evaluations) {
        hwa_set_error(error, error_size,
                      "Stage 8 run evaluation cap exceeded");
        goto cleanup;
    }
    result->total_run_evaluations += job->run_evaluations;
    job->output_bytes = 0U;
    for (parameter_index = artifact_start;
         parameter_index < result->artifact_count; ++parameter_index)
        job->output_bytes += result->artifacts[parameter_index].file_bytes;
    for (parameter_index = 0U; parameter_index < result->response_count;
         ++parameter_index) {
        size_t observation_index = job_index * result->response_count +
                                   parameter_index;
        HWAExperimentObservation *observation =
            &result->observations[observation_index];
        const HWARunFeature *feature = hwa_experiment_run_feature_find(
            &run, &result->responses[parameter_index]);
        observation->id = (uint64_t)observation_index + UINT64_C(1);
        observation->job_id = job->id;
        observation->response_id =
            result->responses[parameter_index].id;
        if (feature != NULL && feature->gap_valid) {
            observation->availability = feature->availability;
            observation->value = feature->normalized_gap;
            observation->quality_flags = feature->quality_flags;
            observation->value_valid = 1;
        } else {
            observation->availability = feature == NULL
                ? HWA_RUN_UNAVAILABLE : feature->availability;
        }
    }
    result->rendered_job_count++;
    if (hwa_experiment_remove_scratch_file(request_path, 1) != 0 ||
        hwa_experiment_remove_scratch_file(stdout_path, 0) != 0 ||
        hwa_experiment_remove_scratch_file(stderr_path, 0) != 0 ||
        hwa_experiment_job_directory_exact(
            job_directory, item, run_manifest_path,
            job->run_manifest_sha256, run_result_path,
            job->run_result_sha256, result->artifacts + artifact_start,
            result->artifact_count - artifact_start, &result->options,
            &run_manifest_identities[job_index],
            &run_result_identities[job_index],
            artifact_identities + artifact_start,
            bundle_bytes, error, error_size) != 0)
        goto cleanup;
    if (hwa_experiment_now_milliseconds(&finish) != 0 ||
        finish < experiment_started ||
        finish - experiment_started >
            result->options.max_total_milliseconds) {
        hwa_set_error(error, error_size, "Stage 8 time cap exceeded");
        goto cleanup;
    }
    hwa_run_result_free(&run);
    status = 0;
cleanup:
    if (status != 0) hwa_run_result_free(&run);
    for (parameter_index = 0U; parameter_index < output_count;
         ++parameter_index) free(owned_output_paths == NULL
                                  ? NULL : owned_output_paths[parameter_index]);
    free(parameters);
    free(inputs);
    free(outputs);
    free(owned_output_paths);
    free(run_bindings);
    free(job_directory);
    free(request_path);
    free(stdout_path);
    free(stderr_path);
    free(run_manifest_path);
    free(run_result_path);
    (void)manifest_size;
    return status;
}

static const HWAExperimentObservation *hwa_experiment_observation_at(
    const HWAExperimentResult *result,
    size_t job_index,
    size_t response_index)
{
    size_t index = job_index * result->response_count + response_index;
    if (job_index >= result->job_count || response_index >= result->response_count ||
        index >= result->observation_count) return NULL;
    return &result->observations[index];
}

static int hwa_experiment_candidate_value(
    const HWAExperimentResult *result,
    size_t point_index,
    size_t response_index,
    HWAExperimentSplit split,
    double *mean,
    double *worst_harm,
    size_t *case_count)
{
    double weighted_sum = 0.0;
    double weight_sum = 0.0;
    double worst = 0.0;
    size_t count = 0U;
    size_t case_index;
    for (case_index = 0U; case_index < result->case_count; ++case_index) {
        const HWAExperimentCase *record = &result->cases[case_index];
        double sum = 0.0;
        size_t valid = 0U;
        size_t replicate;
        if (record->split != split) continue;
        for (replicate = 0U; replicate < result->plan_replicates; ++replicate) {
            size_t job_index =
                (point_index * result->case_count + case_index) *
                    result->plan_replicates + replicate;
            const HWAExperimentObservation *observation =
                hwa_experiment_observation_at(result, job_index,
                                              response_index);
            if (observation != NULL && observation->value_valid) {
                sum += observation->value;
                valid++;
            }
        }
        if (valid != result->plan_replicates) return -1;
        {
            double cell = sum / (double)valid;
            weighted_sum += record->weight * cell;
            weight_sum += record->weight;
            if (point_index != 0U) {
                double baseline_sum = 0.0;
                size_t baseline_valid = 0U;
                for (replicate = 0U; replicate < result->plan_replicates;
                     ++replicate) {
                    size_t baseline_job = case_index * result->plan_replicates +
                                          replicate;
                    const HWAExperimentObservation *baseline =
                        hwa_experiment_observation_at(
                            result, baseline_job, response_index);
                    if (baseline != NULL && baseline->value_valid) {
                        baseline_sum += baseline->value;
                        baseline_valid++;
                    }
                }
                if (baseline_valid != result->plan_replicates) return -1;
                {
                    double harm = cell - baseline_sum / (double)baseline_valid;
                    if (count == 0U || harm > worst) worst = harm;
                }
            }
        }
        count++;
    }
    if (count == 0U || !(weight_sum > 0.0)) return -1;
    *mean = weighted_sum / weight_sum;
    *worst_harm = point_index == 0U ? 0.0 : worst;
    *case_count = count;
    return isfinite(*mean) && isfinite(*worst_harm) ? 0 : -1;
}

int hwa_experiment_candidates_rebuild(HWAExperimentResult *result,
                                      char *error,
                                      size_t error_size)
{
    uint64_t count;
    uint64_t cap;
    size_t point;
    size_t response;
    size_t split_offset;
    size_t index = 0U;
    double *baseline = NULL;
    free(result == NULL ? NULL : result->candidates);
    if (result != NULL) {
        result->candidates = NULL;
        result->candidate_count = 0U;
    }
    if (result == NULL ||
        hwa_experiment_u64_multiply((uint64_t)result->point_count,
                                    (uint64_t)result->response_count,
                                    &count) != 0 ||
        hwa_experiment_u64_multiply(count, UINT64_C(2), &count) != 0 ||
        hwa_experiment_u64_multiply(
            (uint64_t)result->options.max_points,
            (uint64_t)result->options.max_responses, &cap) != 0 ||
        hwa_experiment_u64_multiply(cap, UINT64_C(2), &cap) != 0 ||
        count > cap ||
        count > SIZE_MAX) {
        hwa_set_error(error, error_size, "invalid Stage 8 candidate rebuild");
        return -1;
    }
    result->candidate_count = (size_t)count;
    result->candidates = (HWAExperimentCandidate *)calloc(
        result->candidate_count, sizeof(*result->candidates));
    baseline = (double *)calloc(result->response_count * 2U,
                                sizeof(*baseline));
    if ((result->candidate_count != 0U && result->candidates == NULL) ||
        (result->response_count != 0U && baseline == NULL)) goto allocation;
    for (response = 0U; response < result->response_count; ++response) {
        for (split_offset = 0U; split_offset < 2U; ++split_offset) {
            double worst;
            size_t cases;
            if (hwa_experiment_candidate_value(
                    result, 0U, response,
                    split_offset == 0U ? HWA_EXPERIMENT_FIT :
                                         HWA_EXPERIMENT_CHECK,
                    &baseline[response * 2U + split_offset], &worst,
                    &cases) != 0)
                baseline[response * 2U + split_offset] = NAN;
        }
    }
    for (point = 0U; point < result->point_count; ++point) {
        if ((point & 255U) == 0U &&
            hwa_experiment_deadline_poll(error, error_size) != 0)
            goto allocation;
        for (response = 0U; response < result->response_count; ++response) {
            if ((response & 31U) == 0U &&
                hwa_experiment_deadline_poll(error, error_size) != 0)
                goto allocation;
            for (split_offset = 0U; split_offset < 2U; ++split_offset) {
                HWAExperimentCandidate *candidate = &result->candidates[index];
                double mean;
                double worst;
                size_t cases;
                HWAExperimentSplit split = split_offset == 0U
                    ? HWA_EXPERIMENT_FIT : HWA_EXPERIMENT_CHECK;
                candidate->id = (uint64_t)index + UINT64_C(1);
                candidate->point_id = (uint64_t)point + UINT64_C(1);
                candidate->response_id = (uint64_t)response + UINT64_C(1);
                candidate->split = split;
                if (hwa_experiment_candidate_value(
                        result, point, response, split,
                        &mean, &worst, &cases) == 0 &&
                    isfinite(baseline[response * 2U + split_offset])) {
                    candidate->availability = HWA_RUN_AVAILABLE;
                    candidate->mean_gap = mean;
                    candidate->improvement =
                        baseline[response * 2U + split_offset] - mean;
                    candidate->worst_harm = worst;
                    candidate->case_count = cases;
                    candidate->values_valid = 1;
                    if (split == HWA_EXPERIMENT_CHECK &&
                        worst > HWA_EXPERIMENT_HARM_THRESHOLD)
                        candidate->quality_flags |=
                            HWA_EXPERIMENT_QUALITY_CHECK_HARM;
                } else {
                    candidate->availability = HWA_RUN_UNAVAILABLE;
                }
                index++;
            }
        }
    }
    free(baseline);
    return 0;
allocation:
    free(baseline);
    free(result->candidates);
    result->candidates = NULL;
    result->candidate_count = 0U;
    hwa_set_error(error, error_size, "cannot allocate Stage 8 candidates");
    return -1;
}

static const HWAExperimentCandidate *hwa_experiment_candidate_find(
    const HWAExperimentResult *result,
    uint64_t point_id,
    uint64_t response_id,
    HWAExperimentSplit split)
{
    size_t point;
    size_t response;
    size_t split_offset;
    size_t index;
    if (result == NULL || point_id == 0U ||
        point_id > result->point_count || response_id == 0U ||
        response_id > result->response_count ||
        (split != HWA_EXPERIMENT_FIT && split != HWA_EXPERIMENT_CHECK))
        return NULL;
    point = (size_t)(point_id - UINT64_C(1));
    response = (size_t)(response_id - UINT64_C(1));
    split_offset = split == HWA_EXPERIMENT_FIT ? 0U : 1U;
    if (point > (SIZE_MAX / result->response_count) - 1U) return NULL;
    index = (point * result->response_count + response) * 2U + split_offset;
    if (index >= result->candidate_count) return NULL;
    return &result->candidates[index];
}

typedef struct HWAExperimentXY {
    double x;
    double y;
} HWAExperimentXY;

static int hwa_experiment_xy_compare(const void *left, const void *right)
{
    const HWAExperimentXY *left_xy = (const HWAExperimentXY *)left;
    const HWAExperimentXY *right_xy = (const HWAExperimentXY *)right;
    if (left_xy->x < right_xy->x) return -1;
    if (left_xy->x > right_xy->x) return 1;
    return 0;
}

static int hwa_experiment_point_for_parameter(
    const HWAExperimentResult *result,
    size_t point,
    size_t parameter)
{
    size_t scan;
    if (result->plan_kind != HWA_EXPERIMENT_ONE_AT_A_TIME || point == 0U)
        return 1;
    for (scan = 0U; scan < result->parameter_count; ++scan) {
        const HWAExperimentValue *value =
            &result->values[point * result->parameter_count + scan];
        if (scan != parameter &&
            value->value != result->parameters[scan].baseline) return 0;
    }
    return 1;
}

typedef struct HWAExperimentStats {
    double mean_x;
    double mean_y;
    double m2_x;
    double m2_y;
    double covariance_sum;
    double minimum_y;
    double maximum_y;
    size_t count;
} HWAExperimentStats;

static void hwa_experiment_stats_push(HWAExperimentStats *stats,
                                      double x,
                                      double y)
{
    double count = (double)stats->count + 1.0;
    double delta_x = x - stats->mean_x;
    double delta_y = y - stats->mean_y;
    stats->mean_x += delta_x / count;
    stats->mean_y += delta_y / count;
    stats->m2_x += delta_x * (x - stats->mean_x);
    stats->m2_y += delta_y * (y - stats->mean_y);
    stats->covariance_sum += delta_x * (y - stats->mean_y);
    if (stats->count == 0U || y < stats->minimum_y) stats->minimum_y = y;
    if (stats->count == 0U || y > stats->maximum_y) stats->maximum_y = y;
    stats->count++;
}

static int hwa_experiment_stats_fit(const HWAExperimentStats *stats,
                                    double *slope,
                                    double *pearson,
                                    double *r_squared)
{
    if (stats->count < 2U) return -1;
    if (!(stats->m2_x > 0.0) || !(stats->m2_y > 0.0) ||
        !isfinite(stats->m2_x) || !isfinite(stats->m2_y) ||
        !isfinite(stats->covariance_sum)) return -1;
    *slope = stats->covariance_sum / stats->m2_x;
    *pearson = stats->covariance_sum /
        sqrt(stats->m2_x * stats->m2_y);
    if (*pearson > 1.0) *pearson = 1.0;
    if (*pearson < -1.0) *pearson = -1.0;
    *r_squared = *pearson * *pearson;
    return isfinite(*slope) && isfinite(*pearson) && isfinite(*r_squared)
        ? 0 : -1;
}

static int hwa_experiment_sensitivity_monotonicity(
    const HWAExperimentResult *result,
    size_t parameter,
    size_t response,
    HWAExperimentSplit split,
    HWAExperimentMonotonicity *monotonicity,
    char *error,
    size_t error_size)
{
    size_t point;
    double last_x = 0.0;
    double group_sum = 0.0;
    size_t group_count = 0U;
    double last_mean = 0.0;
    int have_group = 0;
    int increasing = 0;
    int decreasing = 0;
    /* Manifest order gives sorted x for OAT/grid only after grouping by scan. */
    HWAExperimentXY *rows = (HWAExperimentXY *)malloc(
        result->point_count * sizeof(*rows));
    size_t count = 0U;
    if (rows == NULL) return -1;
    for (point = 0U; point < result->point_count; ++point) {
        const HWAExperimentCandidate *candidate;
        if ((point & 255U) == 0U &&
            hwa_experiment_deadline_poll(error, error_size) != 0) {
            free(rows);
            return -1;
        }
        if (!hwa_experiment_point_for_parameter(result, point, parameter))
            continue;
        candidate = hwa_experiment_candidate_find(
            result, (uint64_t)point + UINT64_C(1),
            (uint64_t)response + UINT64_C(1), split);
        if (candidate != NULL && candidate->values_valid) {
            rows[count].x = result->values[
                point * result->parameter_count + parameter].value;
            rows[count].y = candidate->mean_gap;
            count++;
        }
    }
    qsort(rows, count, sizeof(*rows), hwa_experiment_xy_compare);
    if (count == 0U) {
        free(rows);
        *monotonicity = HWA_EXPERIMENT_MONOTONIC_NONE;
        return 0;
    }
    for (point = 0U; point <= count; ++point) {
        if ((point & 255U) == 0U &&
            hwa_experiment_deadline_poll(error, error_size) != 0) {
            free(rows);
            return -1;
        }
        if (point == count ||
            (group_count != 0U && rows[point].x != last_x)) {
            double mean = group_sum / (double)group_count;
            if (have_group) {
                if (mean > last_mean + HWA_EXPERIMENT_FLAT_THRESHOLD)
                    increasing = 1;
                else if (mean < last_mean - HWA_EXPERIMENT_FLAT_THRESHOLD)
                    decreasing = 1;
            }
            last_mean = mean;
            have_group = 1;
            group_sum = 0.0;
            group_count = 0U;
        }
        if (point < count) {
            if (group_count == 0U) last_x = rows[point].x;
            group_sum += rows[point].y;
            group_count++;
        }
    }
    free(rows);
    if (!have_group) *monotonicity = HWA_EXPERIMENT_MONOTONIC_NONE;
    else if (!increasing && !decreasing)
        *monotonicity = HWA_EXPERIMENT_MONOTONIC_FLAT;
    else if (increasing && !decreasing)
        *monotonicity = HWA_EXPERIMENT_MONOTONIC_INCREASING;
    else if (!increasing && decreasing)
        *monotonicity = HWA_EXPERIMENT_MONOTONIC_DECREASING;
    else *monotonicity = HWA_EXPERIMENT_MONOTONIC_MIXED;
    return 0;
}

static int hwa_experiment_grid_effect(const HWAExperimentResult *result,
                                      size_t parameter,
                                      size_t response,
                                      HWAExperimentSplit split,
                                      double *effect,
                                      char *error,
                                      size_t error_size)
{
    HWAExperimentStats total_stats;
    size_t point;
    double between = 0.0;
    memset(&total_stats, 0, sizeof(total_stats));
    if (result->plan_kind != HWA_EXPERIMENT_GRID) return 1;
    for (point = 0U; point < result->point_count; ++point) {
        const HWAExperimentCandidate *candidate = hwa_experiment_candidate_find(
            result, (uint64_t)point + UINT64_C(1),
            (uint64_t)response + UINT64_C(1), split);
        if ((point & 255U) == 0U &&
            hwa_experiment_deadline_poll(error, error_size) != 0) return -1;
        if (candidate == NULL || !candidate->values_valid) continue;
        hwa_experiment_stats_push(&total_stats, 0.0, candidate->mean_gap);
    }
    if (total_stats.count < 2U || !(total_stats.m2_y > 0.0) ||
        !isfinite(total_stats.m2_y)) return 1;
    {
        double grand = total_stats.mean_y;
        double total = total_stats.m2_y / (double)total_stats.count;
        size_t level;
        if (!(total > 0.0) || !isfinite(total)) return 1;
        for (level = 0U; level < result->parameters[parameter].level_count;
             ++level) {
            double x = result->levels[
                result->parameters[parameter].first_level + level].value;
            double sum = 0.0;
            size_t count = 0U;
            for (point = 0U; point < result->point_count; ++point) {
                const HWAExperimentCandidate *candidate;
                if ((point & 255U) == 0U &&
                    hwa_experiment_deadline_poll(error, error_size) != 0)
                    return -1;
                if (result->values[point * result->parameter_count + parameter]
                        .value != x) continue;
                candidate = hwa_experiment_candidate_find(
                    result, (uint64_t)point + UINT64_C(1),
                    (uint64_t)response + UINT64_C(1), split);
                if (candidate != NULL && candidate->values_valid) {
                    sum += candidate->mean_gap;
                    count++;
                }
            }
            if (count != 0U) {
                double delta = sum / (double)count - grand;
                between += (double)count * delta * delta;
            }
        }
        between /= (double)total_stats.count;
        *effect = between / total;
    }
    return isfinite(*effect) ? 0 : 1;
}

static int hwa_experiment_replicate_noise(
    const HWAExperimentResult *result,
    size_t response,
    HWAExperimentSplit split,
    double *noise,
    char *error,
    size_t error_size)
{
    double sse = 0.0;
    size_t residual_count = 0U;
    size_t point;
    size_t case_index;
    if (result->plan_replicates < 2U) return 1;
    for (point = 0U; point < result->point_count; ++point) {
        if ((point & 255U) == 0U &&
            hwa_experiment_deadline_poll(error, error_size) != 0) return -1;
        for (case_index = 0U; case_index < result->case_count; ++case_index) {
            double sum = 0.0;
            size_t replicate;
            if (result->cases[case_index].split != split) continue;
            for (replicate = 0U; replicate < result->plan_replicates;
                 ++replicate) {
                size_t job = (point * result->case_count + case_index) *
                    result->plan_replicates + replicate;
                const HWAExperimentObservation *observation =
                    hwa_experiment_observation_at(result, job, response);
                if (observation == NULL || !observation->value_valid) break;
                sum += observation->value;
            }
            if (replicate != result->plan_replicates) continue;
            {
                double mean = sum / (double)result->plan_replicates;
                for (replicate = 0U; replicate < result->plan_replicates;
                     ++replicate) {
                    size_t job = (point * result->case_count + case_index) *
                        result->plan_replicates + replicate;
                    const HWAExperimentObservation *observation =
                        hwa_experiment_observation_at(result, job, response);
                    double delta = observation->value - mean;
                    sse += delta * delta;
                    residual_count++;
                }
            }
        }
    }
    if (residual_count == 0U) return 1;
    *noise = sqrt(sse / (double)residual_count);
    return isfinite(*noise) ? 0 : 1;
}

int hwa_experiment_sensitivities_rebuild(HWAExperimentResult *result,
                                         char *error,
                                         size_t error_size)
{
    uint64_t count;
    size_t parameter;
    size_t response;
    size_t split_offset;
    size_t index = 0U;
    free(result == NULL ? NULL : result->sensitivities);
    if (result != NULL) {
        result->sensitivities = NULL;
        result->sensitivity_count = 0U;
    }
    if (result == NULL ||
        hwa_experiment_u64_multiply((uint64_t)result->parameter_count,
                                    (uint64_t)result->response_count,
                                    &count) != 0 ||
        hwa_experiment_u64_multiply(count, UINT64_C(2), &count) != 0 ||
        count > result->options.max_sensitivities || count > SIZE_MAX) {
        hwa_set_error(error, error_size,
                      "invalid Stage 8 sensitivity rebuild");
        return -1;
    }
    result->sensitivity_count = (size_t)count;
    result->sensitivities = (HWAExperimentSensitivity *)calloc(
        result->sensitivity_count, sizeof(*result->sensitivities));
    if (result->sensitivity_count != 0U && result->sensitivities == NULL) {
        hwa_set_error(error, error_size,
                      "cannot allocate Stage 8 sensitivities");
        return -1;
    }
    for (parameter = 0U; parameter < result->parameter_count; ++parameter) {
        if (hwa_experiment_deadline_poll(error, error_size) != 0) return -1;
        for (response = 0U; response < result->response_count; ++response) {
            for (split_offset = 0U; split_offset < 2U; ++split_offset) {
                HWAExperimentSensitivity *sensitivity =
                    &result->sensitivities[index];
                HWAExperimentStats stats;
                HWAExperimentSplit split = split_offset == 0U
                    ? HWA_EXPERIMENT_FIT : HWA_EXPERIMENT_CHECK;
                size_t point;
                double x_scale = fmax(
                    1.0, fmax(fabs(result->parameters[parameter].minimum),
                              fabs(result->parameters[parameter].maximum)));
                memset(&stats, 0, sizeof(stats));
                sensitivity->id = (uint64_t)index + UINT64_C(1);
                sensitivity->parameter_id =
                    (uint64_t)parameter + UINT64_C(1);
                sensitivity->response_id =
                    (uint64_t)response + UINT64_C(1);
                sensitivity->split = split;
                for (point = 0U; point < result->point_count; ++point) {
                    const HWAExperimentCandidate *candidate;
                    if ((point & 255U) == 0U &&
                        hwa_experiment_deadline_poll(
                            error, error_size) != 0) return -1;
                    if (!hwa_experiment_point_for_parameter(
                            result, point, parameter)) continue;
                    candidate = hwa_experiment_candidate_find(
                        result, (uint64_t)point + UINT64_C(1),
                        (uint64_t)response + UINT64_C(1), split);
                    if (candidate != NULL && candidate->values_valid) {
                        double x = result->values[
                            point * result->parameter_count + parameter].value;
                        hwa_experiment_stats_push(&stats, x / x_scale,
                                                  candidate->mean_gap);
                    }
                }
                sensitivity->point_count = stats.count;
                sensitivity->availability = stats.count >= 2U
                    ? HWA_RUN_AVAILABLE : HWA_RUN_INSUFFICIENT;
                sensitivity->response_range = stats.count == 0U ? 0.0 :
                    stats.maximum_y - stats.minimum_y;
                if (hwa_experiment_stats_fit(
                        &stats, &sensitivity->slope, &sensitivity->pearson,
                        &sensitivity->linear_r_squared) == 0) {
                    sensitivity->slope /= x_scale;
                    sensitivity->linear_valid = 1;
                }
                {
                    int effect_status = hwa_experiment_grid_effect(
                        result, parameter, response, split,
                        &sensitivity->effect_fraction, error, error_size);
                    if (effect_status < 0) return -1;
                    if (effect_status == 0) sensitivity->effect_valid = 1;
                }
                {
                    int noise_status = hwa_experiment_replicate_noise(
                        result, response, split, &sensitivity->noise_sd,
                        error, error_size);
                    if (noise_status < 0) return -1;
                    if (noise_status == 0) sensitivity->noise_valid = 1;
                }
                if (hwa_experiment_sensitivity_monotonicity(
                        result, parameter, response, split,
                        &sensitivity->monotonicity,
                        error, error_size) != 0) return -1;
                if (stats.count != 0U && sensitivity->response_range <=
                    HWA_EXPERIMENT_FLAT_THRESHOLD)
                    sensitivity->quality_flags |= HWA_EXPERIMENT_QUALITY_FLAT;
                if (sensitivity->monotonicity ==
                    HWA_EXPERIMENT_MONOTONIC_MIXED)
                    sensitivity->quality_flags |=
                        HWA_EXPERIMENT_QUALITY_NONMONOTONIC;
                if (sensitivity->noise_valid &&
                    sensitivity->noise_sd > HWA_EXPERIMENT_NOISE_THRESHOLD)
                    sensitivity->quality_flags |=
                        HWA_EXPERIMENT_QUALITY_REPLICATE_NOISE;
                if (result->plan_kind == HWA_EXPERIMENT_RANDOM)
                    sensitivity->quality_flags |=
                        HWA_EXPERIMENT_QUALITY_RANDOM_LINEAR_ONLY;
                index++;
            }
        }
    }
    return 0;
}

static int hwa_experiment_interaction_effect(
    const HWAExperimentResult *result,
    size_t left_parameter,
    size_t right_parameter,
    size_t response,
    HWAExperimentSplit split,
    double *effect,
    size_t *point_count,
    char *error,
    size_t error_size)
{
    size_t left_levels = result->parameters[left_parameter].level_count;
    size_t right_levels = result->parameters[right_parameter].level_count;
    double *left_sum = NULL;
    double *right_sum = NULL;
    double *cell_sum = NULL;
    size_t *left_count = NULL;
    size_t *right_count = NULL;
    size_t *cell_count = NULL;
    HWAExperimentStats total_stats;
    size_t count = 0U;
    size_t point;
    double interaction_sum = 0.0;
    int status = -1;
    memset(&total_stats, 0, sizeof(total_stats));
    if (result->plan_kind != HWA_EXPERIMENT_GRID || left_levels == 0U ||
        right_levels == 0U || left_levels > SIZE_MAX / right_levels)
        return 1;
    left_sum = (double *)calloc(left_levels, sizeof(*left_sum));
    right_sum = (double *)calloc(right_levels, sizeof(*right_sum));
    cell_sum = (double *)calloc(left_levels * right_levels,
                                sizeof(*cell_sum));
    left_count = (size_t *)calloc(left_levels, sizeof(*left_count));
    right_count = (size_t *)calloc(right_levels, sizeof(*right_count));
    cell_count = (size_t *)calloc(left_levels * right_levels,
                                  sizeof(*cell_count));
    if (left_sum == NULL || right_sum == NULL || cell_sum == NULL ||
        left_count == NULL || right_count == NULL || cell_count == NULL)
        goto cleanup;
    for (point = 0U; point < result->point_count; ++point) {
        const HWAExperimentCandidate *candidate = hwa_experiment_candidate_find(
            result, (uint64_t)point + UINT64_C(1),
            (uint64_t)response + UINT64_C(1), split);
        double left_value;
        double right_value;
        size_t left_index;
        size_t right_index;
        if ((point & 255U) == 0U &&
            hwa_experiment_deadline_poll(error, error_size) != 0) {
            status = -1;
            goto cleanup;
        }
        if (candidate == NULL || !candidate->values_valid) continue;
        left_value = result->values[
            point * result->parameter_count + left_parameter].value;
        right_value = result->values[
            point * result->parameter_count + right_parameter].value;
        for (left_index = 0U; left_index < left_levels; ++left_index)
            if (result->levels[
                    result->parameters[left_parameter].first_level + left_index]
                    .value == left_value) break;
        for (right_index = 0U; right_index < right_levels; ++right_index)
            if (result->levels[
                    result->parameters[right_parameter].first_level + right_index]
                    .value == right_value) break;
        if (left_index == left_levels || right_index == right_levels)
            goto cleanup;
        hwa_experiment_stats_push(&total_stats, 0.0, candidate->mean_gap);
        left_sum[left_index] += candidate->mean_gap;
        right_sum[right_index] += candidate->mean_gap;
        cell_sum[left_index * right_levels + right_index] +=
            candidate->mean_gap;
        left_count[left_index]++;
        right_count[right_index]++;
        cell_count[left_index * right_levels + right_index]++;
        count++;
    }
    if (count < 2U) {
        status = 1;
        goto cleanup;
    }
    {
        double grand = total_stats.mean_y;
        double total = total_stats.m2_y / (double)count;
        size_t left_index;
        if (!(total > 0.0) || !isfinite(total)) {
            status = 1;
            goto cleanup;
        }
        for (left_index = 0U; left_index < left_levels; ++left_index) {
            size_t right_index;
            if ((left_index & 63U) == 0U &&
                hwa_experiment_deadline_poll(error, error_size) != 0) {
                status = -1;
                goto cleanup;
            }
            if (left_count[left_index] == 0U) continue;
            for (right_index = 0U; right_index < right_levels; ++right_index) {
                size_t cell = left_index * right_levels + right_index;
                double residual;
                if (cell_count[cell] == 0U || right_count[right_index] == 0U)
                    continue;
                residual = cell_sum[cell] / (double)cell_count[cell] -
                    left_sum[left_index] / (double)left_count[left_index] -
                    right_sum[right_index] /
                        (double)right_count[right_index] + grand;
                interaction_sum += (double)cell_count[cell] *
                                   residual * residual;
            }
        }
        interaction_sum /= (double)count;
        *effect = interaction_sum / total;
    }
    *point_count = count;
    status = isfinite(*effect) ? 0 : 1;
cleanup:
    free(left_sum);
    free(right_sum);
    free(cell_sum);
    free(left_count);
    free(right_count);
    free(cell_count);
    return status;
}

int hwa_experiment_interactions_rebuild(HWAExperimentResult *result,
                                        char *error,
                                        size_t error_size)
{
    uint64_t pairs;
    uint64_t count;
    size_t left;
    size_t right;
    size_t response;
    size_t split_offset;
    size_t index = 0U;
    free(result == NULL ? NULL : result->interactions);
    if (result != NULL) {
        result->interactions = NULL;
        result->interaction_count = 0U;
    }
    if (result == NULL) goto invalid;
    if (result->parameter_count < 2U) pairs = 0U;
    else if (hwa_experiment_u64_multiply(
                 (uint64_t)result->parameter_count,
                 (uint64_t)(result->parameter_count - 1U),
                 &pairs) != 0)
        goto invalid;
    else pairs /= UINT64_C(2);
    if (hwa_experiment_u64_multiply(pairs,
                                    (uint64_t)result->response_count,
                                    &count) != 0 ||
        hwa_experiment_u64_multiply(count, UINT64_C(2), &count) != 0 ||
        count > result->options.max_interactions || count > SIZE_MAX)
        goto invalid;
    result->interaction_count = (size_t)count;
    result->interactions = (HWAExperimentInteraction *)calloc(
        result->interaction_count, sizeof(*result->interactions));
    if (result->interaction_count != 0U && result->interactions == NULL) {
        hwa_set_error(error, error_size,
                      "cannot allocate Stage 8 interactions");
        return -1;
    }
    for (left = 0U; left < result->parameter_count; ++left) {
        if (hwa_experiment_deadline_poll(error, error_size) != 0) return -1;
        for (right = left + 1U; right < result->parameter_count; ++right) {
            for (response = 0U; response < result->response_count; ++response) {
                for (split_offset = 0U; split_offset < 2U; ++split_offset) {
                    HWAExperimentInteraction *interaction =
                        &result->interactions[index];
                    HWAExperimentSplit split = split_offset == 0U
                        ? HWA_EXPERIMENT_FIT : HWA_EXPERIMENT_CHECK;
                    interaction->id = (uint64_t)index + UINT64_C(1);
                    interaction->left_parameter_id =
                        (uint64_t)left + UINT64_C(1);
                    interaction->right_parameter_id =
                        (uint64_t)right + UINT64_C(1);
                    interaction->response_id =
                        (uint64_t)response + UINT64_C(1);
                    interaction->split = split;
                    {
                        int effect_status = hwa_experiment_interaction_effect(
                            result, left, right, response, split,
                            &interaction->effect_fraction,
                            &interaction->point_count, error, error_size);
                        if (effect_status < 0) return -1;
                        if (effect_status == 0) {
                        interaction->availability = HWA_RUN_AVAILABLE;
                        interaction->effect_valid = 1;
                        } else {
                            interaction->availability =
                                result->plan_kind == HWA_EXPERIMENT_GRID
                                    ? HWA_RUN_INSUFFICIENT : HWA_RUN_UNAVAILABLE;
                        }
                    }
                    index++;
                }
            }
        }
    }
    return 0;
invalid:
    hwa_set_error(error, error_size,
                  "invalid Stage 8 interaction rebuild");
    return -1;
}

static int hwa_experiment_warning_add(HWAExperimentResult *result,
                                      const char *code,
                                      const char *message,
                                      uint64_t point_id,
                                      uint64_t parameter_id,
                                      uint64_t response_id,
                                      int point_valid,
                                      int parameter_valid,
                                      int response_valid,
                                      char *error,
                                      size_t error_size)
{
    HWAExperimentWarning *warning;
    if (result->warning_count >= result->options.max_warnings) {
        hwa_set_error(error, error_size, "Stage 8 warning cap exceeded");
        return -1;
    }
    warning = &result->warnings[result->warning_count];
    warning->id = (uint64_t)result->warning_count + UINT64_C(1);
    warning->code = hwa_experiment_copy_string(code);
    warning->message = hwa_experiment_copy_string(message);
    warning->point_id = point_id;
    warning->parameter_id = parameter_id;
    warning->response_id = response_id;
    warning->point_id_valid = point_valid;
    warning->parameter_id_valid = parameter_valid;
    warning->response_id_valid = response_valid;
    if (warning->code == NULL || warning->message == NULL) {
        hwa_set_error(error, error_size,
                      "cannot allocate Stage 8 warning");
        return -1;
    }
    result->warning_count++;
    return 0;
}

int hwa_experiment_warnings_rebuild(HWAExperimentResult *result,
                                    char *error,
                                    size_t error_size)
{
    size_t index;
    size_t expected = 0U;
    if (result == NULL) {
        hwa_set_error(error, error_size, "invalid Stage 8 warning rebuild");
        return -1;
    }
    for (index = 0U; index < result->warning_count; ++index) {
        free(result->warnings[index].code);
        free(result->warnings[index].message);
    }
    free(result->warnings);
    for (index = 0U; index < result->candidate_count; ++index) {
        if ((index & 1023U) == 0U &&
            hwa_experiment_deadline_poll(error, error_size) != 0)
            return -1;
        if ((result->candidates[index].quality_flags &
             HWA_EXPERIMENT_QUALITY_CHECK_HARM) != 0U) expected++;
    }
    for (index = 0U; index < result->sensitivity_count; ++index) {
        if ((index & 1023U) == 0U &&
            hwa_experiment_deadline_poll(error, error_size) != 0)
            return -1;
        if ((result->sensitivities[index].quality_flags &
             HWA_EXPERIMENT_QUALITY_FLAT) != 0U) expected++;
        if ((result->sensitivities[index].quality_flags &
             HWA_EXPERIMENT_QUALITY_REPLICATE_NOISE) != 0U) expected++;
    }
    if (expected > result->options.max_warnings) {
        result->warnings = NULL;
        result->warning_count = 0U;
        hwa_set_error(error, error_size, "Stage 8 warning cap exceeded");
        return -1;
    }
    result->warnings = (HWAExperimentWarning *)calloc(
        expected, sizeof(*result->warnings));
    result->warning_count = 0U;
    if (expected != 0U && result->warnings == NULL) {
        hwa_set_error(error, error_size,
                      "cannot allocate Stage 8 warnings");
        return -1;
    }
    for (index = 0U; index < result->candidate_count; ++index) {
        const HWAExperimentCandidate *candidate = &result->candidates[index];
        if ((candidate->quality_flags & HWA_EXPERIMENT_QUALITY_CHECK_HARM) != 0U &&
            hwa_experiment_warning_add(
                result, "CHECK_HARM",
                "candidate harms at least one check case by more than 0.02",
                candidate->point_id, 0U, candidate->response_id,
                1, 0, 1, error, error_size) != 0) return -1;
    }
    for (index = 0U; index < result->sensitivity_count; ++index) {
        const HWAExperimentSensitivity *sensitivity =
            &result->sensitivities[index];
        if ((sensitivity->quality_flags & HWA_EXPERIMENT_QUALITY_FLAT) != 0U &&
            hwa_experiment_warning_add(
                result, "SENSITIVITY_FLAT",
                "parameter response range is at most 1e-6",
                0U, sensitivity->parameter_id, sensitivity->response_id,
                0, 1, 1, error, error_size) != 0) return -1;
    }
    for (index = 0U; index < result->sensitivity_count; ++index) {
        const HWAExperimentSensitivity *sensitivity =
            &result->sensitivities[index];
        if ((sensitivity->quality_flags &
             HWA_EXPERIMENT_QUALITY_REPLICATE_NOISE) != 0U &&
            hwa_experiment_warning_add(
                result, "REPLICATE_NOISE",
                "pooled replicate noise exceeds 0.02",
                0U, sensitivity->parameter_id, sensitivity->response_id,
                0, 1, 1, error, error_size) != 0) return -1;
    }
    return 0;
}

int hwa_experiment_derived_rebuild(HWAExperimentResult *result,
                                   char *error,
                                   size_t error_size)
{
    if (result == NULL ||
        hwa_experiment_candidates_rebuild(result, error, error_size) != 0 ||
        hwa_experiment_sensitivities_rebuild(result, error, error_size) != 0 ||
        hwa_experiment_interactions_rebuild(result, error, error_size) != 0 ||
        hwa_experiment_warnings_rebuild(result, error, error_size) != 0)
        return -1;
    return 0;
}

int hwa_experiment_total_run_evaluations_expected(
    const HWAExperimentResult *result,
    uint64_t *expected)
{
    uint64_t sum = 0U;
    size_t index;
    if (result == NULL || expected == NULL) return -1;
    for (index = 0U; index < result->job_count; ++index)
        if (hwa_experiment_u64_add(sum, result->jobs[index].run_evaluations,
                                   &sum) != 0) return -1;
    *expected = sum;
    return 0;
}

int hwa_experiment_total_output_bytes_expected(
    const HWAExperimentResult *result,
    uint64_t *expected)
{
    uint64_t sum = 0U;
    size_t index;
    if (result == NULL || expected == NULL) return -1;
    for (index = 0U; index < result->artifact_count; ++index)
        if (hwa_experiment_u64_add(sum, result->artifacts[index].file_bytes,
                                   &sum) != 0) return -1;
    *expected = sum;
    return 0;
}

static int hwa_experiment_result_retained_bytes_impl(
    const HWAExperimentResult *result,
    int canonical,
    uint64_t *bytes)
{
    uint64_t total = 0U;
    size_t index;
    if (result == NULL || bytes == NULL ||
        hwa_experiment_array_bytes(result->input_count,
                                   sizeof(*result->inputs), &total) != 0 ||
        hwa_experiment_array_bytes(result->parameter_count,
                                   sizeof(*result->parameters), &total) != 0 ||
        hwa_experiment_array_bytes(result->level_count,
                                   sizeof(*result->levels), &total) != 0 ||
        hwa_experiment_array_bytes(result->case_count,
                                   sizeof(*result->cases), &total) != 0 ||
        hwa_experiment_array_bytes(result->response_count,
                                   sizeof(*result->responses), &total) != 0 ||
        hwa_experiment_array_bytes(result->point_count,
                                   sizeof(*result->points), &total) != 0 ||
        hwa_experiment_array_bytes(result->value_count,
                                   sizeof(*result->values), &total) != 0 ||
        hwa_experiment_array_bytes(result->job_count,
                                   sizeof(*result->jobs), &total) != 0 ||
        hwa_experiment_array_bytes(result->artifact_count,
                                   sizeof(*result->artifacts), &total) != 0 ||
        hwa_experiment_array_bytes(result->observation_count,
                                   sizeof(*result->observations), &total) != 0 ||
        hwa_experiment_array_bytes(result->candidate_count,
                                   sizeof(*result->candidates), &total) != 0 ||
        hwa_experiment_array_bytes(result->sensitivity_count,
                                   sizeof(*result->sensitivities), &total) != 0 ||
        hwa_experiment_array_bytes(result->interaction_count,
                                   sizeof(*result->interactions), &total) != 0 ||
        hwa_experiment_array_bytes(result->warning_count,
                                   sizeof(*result->warnings), &total) != 0 ||
        (canonical
            ? hwa_experiment_u64_add(total, UINT64_C(4), &total)
            : (hwa_experiment_string_bytes(result->manifest_path, &total) != 0 ||
               hwa_experiment_string_bytes(result->output_directory, &total) != 0 ||
               hwa_experiment_string_bytes(result->resume_directory, &total) != 0)) ||
        hwa_experiment_string_bytes(result->renderer_id, &total) != 0)
        return -1;
    for (index = 0U; index < result->input_count; ++index)
        if (hwa_experiment_string_bytes(result->inputs[index].binding_id,
                                        &total) != 0 ||
            (canonical
                ? hwa_experiment_u64_add(total, UINT64_C(2), &total)
                : hwa_experiment_string_bytes(result->inputs[index].path,
                                              &total) != 0)) return -1;
    for (index = 0U; index < result->parameter_count; ++index)
        if (hwa_experiment_string_bytes(result->parameters[index].name,
                                        &total) != 0 ||
            hwa_experiment_string_bytes(result->parameters[index].unit,
                                        &total) != 0) return -1;
    for (index = 0U; index < result->case_count; ++index)
        if (hwa_experiment_string_bytes(result->cases[index].name,
                                        &total) != 0) return -1;
    for (index = 0U; index < result->response_count; ++index)
        if (hwa_experiment_string_bytes(result->responses[index].name,
                                        &total) != 0) return -1;
    for (index = 0U; index < result->job_count; ++index)
        if (hwa_experiment_string_bytes(result->jobs[index].run_result_path,
                                        &total) != 0) return -1;
    for (index = 0U; index < result->artifact_count; ++index)
        if (hwa_experiment_string_bytes(result->artifacts[index].resource_id,
                                        &total) != 0 ||
            hwa_experiment_string_bytes(result->artifacts[index].path,
                                        &total) != 0) return -1;
    for (index = 0U; index < result->warning_count; ++index)
        if (hwa_experiment_string_bytes(result->warnings[index].code,
                                        &total) != 0 ||
            hwa_experiment_string_bytes(result->warnings[index].message,
                                        &total) != 0) return -1;
    *bytes = total;
    return 0;
}

int hwa_experiment_result_retained_bytes(const HWAExperimentResult *result,
                                         uint64_t *bytes)
{
    return hwa_experiment_result_retained_bytes_impl(result, 0, bytes);
}

int hwa_experiment_result_canonical_retained_bytes(
    const HWAExperimentResult *result,
    uint64_t *bytes)
{
    return hwa_experiment_result_retained_bytes_impl(result, 1, bytes);
}

int hwa_experiment_result_peak_work_bytes(
    const HWAExperimentResult *result,
    uint64_t extra_bytes,
    uint64_t *bytes)
{
    uint64_t retained;
    uint64_t derived = 0U;
    uint64_t scratch = 0U;
    uint64_t interaction_cells = 0U;
    uint64_t candidate_count;
    uint64_t sensitivity_count;
    uint64_t interaction_count = 0U;
    uint64_t warning_count;
    uint64_t response_splits;
    uint64_t pair_count = 0U;
    size_t max_levels = 0U;
    size_t max_artifacts_per_job = 0U;
    size_t index;
    if (result == NULL || bytes == NULL ||
        hwa_experiment_result_retained_bytes(result, &retained) != 0 ||
        hwa_experiment_u64_multiply((uint64_t)result->point_count,
                                    (uint64_t)result->response_count,
                                    &candidate_count) != 0 ||
        hwa_experiment_u64_multiply(candidate_count, UINT64_C(2),
                                    &candidate_count) != 0 ||
        hwa_experiment_u64_multiply((uint64_t)result->parameter_count,
                                    (uint64_t)result->response_count,
                                    &sensitivity_count) != 0 ||
        hwa_experiment_u64_multiply(sensitivity_count, UINT64_C(2),
                                    &sensitivity_count) != 0 ||
        hwa_experiment_u64_multiply((uint64_t)result->response_count,
                                    UINT64_C(2), &response_splits) != 0)
        return -1;
    {
        uint64_t parameters = (uint64_t)result->parameter_count;
        if (parameters > 1U) {
            pair_count = parameters - UINT64_C(1);
            if ((parameters & UINT64_C(1)) == 0U)
                parameters /= UINT64_C(2);
            else
                pair_count /= UINT64_C(2);
            if (hwa_experiment_u64_multiply(parameters, pair_count,
                                             &pair_count) != 0 ||
                hwa_experiment_u64_multiply(pair_count, response_splits,
                                             &interaction_count) != 0)
                return -1;
        }
    }
    if (hwa_experiment_u64_multiply(sensitivity_count, UINT64_C(2),
                                    &warning_count) != 0 ||
        hwa_experiment_u64_add(warning_count, candidate_count,
                               &warning_count) != 0)
        return -1;
    if (warning_count > (uint64_t)result->options.max_warnings)
        warning_count = (uint64_t)result->options.max_warnings;
    if (candidate_count > SIZE_MAX || sensitivity_count > SIZE_MAX ||
        interaction_count > SIZE_MAX || warning_count > SIZE_MAX ||
        response_splits > SIZE_MAX ||
        hwa_experiment_array_bytes((size_t)candidate_count,
                                   sizeof(*result->candidates), &derived) != 0 ||
        hwa_experiment_array_bytes((size_t)sensitivity_count,
                                   sizeof(*result->sensitivities), &derived) != 0 ||
        hwa_experiment_array_bytes((size_t)interaction_count,
                                   sizeof(*result->interactions), &derived) != 0 ||
        hwa_experiment_array_bytes((size_t)warning_count,
                                   sizeof(*result->warnings), &derived) != 0 ||
        hwa_experiment_u64_multiply(warning_count, UINT64_C(128),
                                    &warning_count) != 0 ||
        hwa_experiment_u64_add(derived, warning_count, &derived) != 0 ||
        hwa_experiment_u64_multiply(derived, UINT64_C(2), &derived) != 0)
        return -1;
    for (index = 0U; index < result->parameter_count; ++index) {
        size_t levels = result->parameters[index].level_count;
        if (levels > max_levels) max_levels = levels;
    }
    if (result->plan_kind == HWA_EXPERIMENT_GRID &&
        (hwa_experiment_u64_multiply((uint64_t)max_levels,
                                    (uint64_t)max_levels,
                                    &interaction_cells) != 0 ||
         hwa_experiment_u64_multiply(interaction_cells,
            (uint64_t)(sizeof(double) + sizeof(size_t)),
            &interaction_cells) != 0 ||
         hwa_experiment_u64_multiply(interaction_cells, UINT64_C(3),
                                     &interaction_cells) != 0))
        return -1;
    {
        size_t artifact = 0U;
        for (index = 0U; index < result->job_count; ++index) {
            size_t first = artifact;
            while (artifact < result->artifact_count &&
                   result->artifacts[artifact].job_id ==
                       (uint64_t)index + UINT64_C(1))
                artifact++;
            if (artifact - first > max_artifacts_per_job)
                max_artifacts_per_job = artifact - first;
        }
    }
    if (hwa_experiment_array_bytes(result->point_count, sizeof(HWAExperimentXY),
                                   &scratch) != 0 ||
        hwa_experiment_array_bytes((size_t)response_splits,
                                   sizeof(double), &scratch) != 0 ||
        hwa_experiment_array_bytes(result->point_count, sizeof(char *) * 2U,
                                   &scratch) != 0 ||
        hwa_experiment_array_bytes(result->job_count, sizeof(char *) * 2U,
                                   &scratch) != 0 ||
        hwa_experiment_array_bytes(max_artifacts_per_job, sizeof(char *) * 3U,
                                   &scratch) != 0 ||
        hwa_experiment_u64_add(scratch, interaction_cells, &scratch) != 0 ||
        hwa_experiment_u64_add(retained, derived, &retained) != 0 ||
        hwa_experiment_u64_add(retained, scratch, &retained) != 0 ||
        hwa_experiment_u64_add(retained, extra_bytes, &retained) != 0)
        return -1;
    *bytes = retained;
    return 0;
}

int hwa_experiment_derived_double_matches(double saved, double expected)
{
    size_t step;
    double value;
    if (!isfinite(saved) || !isfinite(expected)) return 0;
    if (saved == 0.0) saved = 0.0;
    if (expected == 0.0) expected = 0.0;
    if (saved == expected) return 1;
    value = expected;
    for (step = 0U; step < 32U; ++step) {
        value = nextafter(value, saved);
        if (value == saved) return 1;
    }
    return 0;
}

double hwa_experiment_derived_double_normalize(double saved, double expected)
{
    return hwa_experiment_derived_double_matches(saved, expected)
        ? (expected == 0.0 ? 0.0 : expected) : saved;
}

static int hwa_experiment_options_valid(const HWAExperimentOptions *options)
{
    return options != NULL && options->max_manifest_bytes != 0U &&
        options->max_input_bytes != 0U && options->max_work_bytes != 0U &&
        options->max_bundle_bytes != 0U &&
        options->max_output_file_bytes != 0U &&
        options->max_total_run_evaluations != 0U &&
        options->max_job_milliseconds != 0U &&
        options->max_total_milliseconds != 0U &&
        options->max_parameters != 0U && options->max_levels != 0U &&
        options->max_cases != 0U && options->max_responses != 0U &&
        options->max_points != 0U && options->max_jobs != 0U &&
        options->max_replicates != 0U && options->max_artifacts != 0U &&
        options->max_observations != 0U &&
        options->max_sensitivities != 0U &&
        options->max_interactions != 0U && options->max_warnings != 0U &&
        options->run.decode_block_frames != 0U &&
        options->run.decode_block_frames <= 1048576U &&
        options->run.max_manifest_bytes != 0U &&
        options->run.max_input_bytes != 0U &&
        options->run.max_input_frames != 0U &&
        options->run.max_probe_bytes != 0U &&
        options->run.max_probe_values != 0U &&
        options->run.max_work_bytes != 0U &&
        options->run.max_evaluations != 0U &&
        options->run.max_stems != 0U && options->run.max_probes != 0U &&
        options->run.max_links != 0U &&
        options->run.max_json_depth != 0U &&
        options->run.max_json_tokens != 0U &&
        options->run.max_result_rows != 0U &&
        options->run.max_warnings != 0U;
}

static void hwa_experiment_derived_free(HWAExperimentResult *result)
{
    size_t index;
    if (result == NULL) return;
    free(result->candidates);
    free(result->sensitivities);
    free(result->interactions);
    for (index = 0U; index < result->warning_count; ++index) {
        free(result->warnings[index].code);
        free(result->warnings[index].message);
    }
    free(result->warnings);
    result->candidates = NULL;
    result->candidate_count = 0U;
    result->sensitivities = NULL;
    result->sensitivity_count = 0U;
    result->interactions = NULL;
    result->interaction_count = 0U;
    result->warnings = NULL;
    result->warning_count = 0U;
}

static int hwa_experiment_candidate_equal(const HWAExperimentCandidate *left,
                                          const HWAExperimentCandidate *right)
{
    return left->id == right->id && left->point_id == right->point_id &&
        left->response_id == right->response_id && left->split == right->split &&
        left->availability == right->availability &&
        left->case_count == right->case_count &&
        left->quality_flags == right->quality_flags &&
        left->values_valid == right->values_valid &&
        (!left->values_valid ||
         (hwa_experiment_derived_double_matches(left->mean_gap,
                                                right->mean_gap) &&
          hwa_experiment_derived_double_matches(left->improvement,
                                                right->improvement) &&
          hwa_experiment_derived_double_matches(left->worst_harm,
                                                right->worst_harm)));
}

static int hwa_experiment_sensitivity_equal(
    const HWAExperimentSensitivity *left,
    const HWAExperimentSensitivity *right)
{
    return left->id == right->id &&
        left->parameter_id == right->parameter_id &&
        left->response_id == right->response_id && left->split == right->split &&
        left->availability == right->availability &&
        left->point_count == right->point_count &&
        left->monotonicity == right->monotonicity &&
        left->quality_flags == right->quality_flags &&
        left->linear_valid == right->linear_valid &&
        left->effect_valid == right->effect_valid &&
        left->noise_valid == right->noise_valid &&
        hwa_experiment_derived_double_matches(left->response_range,
                                              right->response_range) &&
        (!left->linear_valid ||
         (hwa_experiment_derived_double_matches(left->slope, right->slope) &&
          hwa_experiment_derived_double_matches(left->pearson, right->pearson) &&
          hwa_experiment_derived_double_matches(left->linear_r_squared,
                                                right->linear_r_squared))) &&
        (!left->effect_valid ||
         hwa_experiment_derived_double_matches(left->effect_fraction,
                                               right->effect_fraction)) &&
        (!left->noise_valid ||
         hwa_experiment_derived_double_matches(left->noise_sd,
                                               right->noise_sd));
}

static int hwa_experiment_interaction_equal(
    const HWAExperimentInteraction *left,
    const HWAExperimentInteraction *right)
{
    return left->id == right->id &&
        left->left_parameter_id == right->left_parameter_id &&
        left->right_parameter_id == right->right_parameter_id &&
        left->response_id == right->response_id && left->split == right->split &&
        left->availability == right->availability &&
        left->point_count == right->point_count &&
        left->effect_valid == right->effect_valid &&
        (!left->effect_valid ||
         hwa_experiment_derived_double_matches(left->effect_fraction,
                                               right->effect_fraction));
}

static int hwa_experiment_plan_replay_validate(
    const HWAExperimentResult *result,
    const HWANumericLocale *locale)
{
    size_t expected_points = 1U;
    size_t baseline_linear = 0U;
    size_t point;
    size_t parameter;
    uint64_t random_state = result->plan_seed;
    if (locale == NULL || !locale->active) return -1;
    if (result->plan_kind == HWA_EXPERIMENT_ONE_AT_A_TIME) {
        for (parameter = 0U; parameter < result->parameter_count; ++parameter) {
            size_t add;
            if (result->parameters[parameter].level_count == 0U) return -1;
            add = result->parameters[parameter].level_count - 1U;
            if (expected_points > SIZE_MAX - add) return -1;
            expected_points += add;
        }
    } else if (result->plan_kind == HWA_EXPERIMENT_GRID) {
        for (parameter = 0U; parameter < result->parameter_count; ++parameter) {
            const HWAExperimentParameter *row = &result->parameters[parameter];
            size_t level;
            size_t baseline = SIZE_MAX;
            if (row->level_count == 0U ||
                expected_points > SIZE_MAX / row->level_count) return -1;
            expected_points *= row->level_count;
            for (level = 0U; level < row->level_count; ++level)
                if (result->levels[row->first_level + level].value ==
                    row->baseline) baseline = level;
            if (baseline == SIZE_MAX ||
                baseline_linear > (SIZE_MAX - baseline) / row->level_count)
                return -1;
            baseline_linear = baseline_linear * row->level_count + baseline;
        }
    }
    /* Random sample_count is stored canonically as point_count minus baseline. */
    if (result->point_count != expected_points &&
        result->plan_kind != HWA_EXPERIMENT_RANDOM) return -1;
    if (result->plan_kind == HWA_EXPERIMENT_RANDOM &&
        result->point_count < 2U) return -1;
    for (point = 0U; point < result->point_count; ++point) {
        char key[HWA_SHA256_HEX_SIZE];
        for (parameter = 0U; parameter < result->parameter_count; ++parameter) {
            const HWAExperimentParameter *row = &result->parameters[parameter];
            double expected = row->baseline;
            double actual = result->values[
                point * result->parameter_count + parameter].value;
            if (point != 0U &&
                result->plan_kind == HWA_EXPERIMENT_ONE_AT_A_TIME) {
                size_t remaining = point - 1U;
                size_t scan;
                for (scan = 0U; scan < result->parameter_count; ++scan) {
                    const HWAExperimentParameter *current =
                        &result->parameters[scan];
                    size_t choices = current->level_count - 1U;
                    if (remaining < choices) {
                        if (scan == parameter) {
                            size_t level;
                            size_t choice = 0U;
                            for (level = 0U; level < current->level_count;
                                 ++level) {
                                double candidate = result->levels[
                                    current->first_level + level].value;
                                if (candidate == current->baseline) continue;
                                if (choice == remaining) expected = candidate;
                                choice++;
                            }
                        }
                        break;
                    }
                    remaining -= choices;
                }
            } else if (point != 0U &&
                       result->plan_kind == HWA_EXPERIMENT_GRID) {
                size_t linear = point - 1U;
                size_t scan = result->parameter_count;
                size_t chosen = 0U;
                if (linear >= baseline_linear) linear++;
                while (scan != 0U) {
                    size_t current = scan - 1U;
                    size_t count = result->parameters[current].level_count;
                    size_t digit = linear % count;
                    linear /= count;
                    if (current == parameter) chosen = digit;
                    scan--;
                }
                expected = result->levels[row->first_level + chosen].value;
            } else if (point != 0U &&
                       result->plan_kind == HWA_EXPERIMENT_RANDOM) {
                double unit = hwa_experiment_random_unit(&random_state);
                expected = row->minimum +
                    (row->maximum - row->minimum) * unit;
                if (!isfinite(expected)) return -1;
            }
            if (expected == 0.0) expected = 0.0;
            if (actual != expected || signbit(actual) != signbit(expected))
                return -1;
        }
        if (hwa_experiment_hash_point(result, point, locale, key) != 0 ||
            strcmp(key, result->points[point].key) != 0) return -1;
    }
    for (point = 0U; point < result->job_count; ++point) {
        const HWAExperimentJob *job = &result->jobs[point];
        size_t replicate = point % result->plan_replicates;
        size_t cell = point / result->plan_replicates;
        size_t case_index = cell % result->case_count;
        size_t point_index = cell / result->case_count;
        uint64_t seed = hwa_experiment_job_seed(
            result->plan_seed, point_index, case_index, replicate);
        char key[HWA_SHA256_HEX_SIZE];
        if (job->seed != seed ||
            hwa_experiment_hash_job(result, point_index, case_index,
                                    replicate, seed, key) != 0 ||
            strcmp(key, job->key) != 0) return -1;
    }
    return 0;
}

static int hwa_experiment_string_pointer_compare(const void *left,
                                                 const void *right)
{
    const char *const *left_text = (const char *const *)left;
    const char *const *right_text = (const char *const *)right;
    return strcmp(*left_text, *right_text);
}

static int hwa_experiment_string_pointer_casefold_compare(
    const void *left,
    const void *right)
{
    const unsigned char *left_text =
        (const unsigned char *)*(const char *const *)left;
    const unsigned char *right_text =
        (const unsigned char *)*(const char *const *)right;
    while (*left_text != '\0' && *right_text != '\0') {
        int left_value = *left_text;
        int right_value = *right_text;
        if (left_value >= 'A' && left_value <= 'Z') left_value += 'a' - 'A';
        if (right_value >= 'A' && right_value <= 'Z') right_value += 'a' - 'A';
        if (left_value != right_value) return left_value - right_value;
        left_text++;
        right_text++;
    }
    return (int)*left_text - (int)*right_text;
}

static int hwa_experiment_keys_unique(const char *const *keys, size_t count)
{
    const char **sorted;
    size_t index;
    int unique = 1;
    if (count < 2U) return 1;
    if (count > SIZE_MAX / sizeof(*sorted)) return 0;
    sorted = (const char **)malloc(count * sizeof(*sorted));
    if (sorted == NULL) return 0;
    memcpy(sorted, keys, count * sizeof(*sorted));
    qsort(sorted, count, sizeof(*sorted),
          hwa_experiment_string_pointer_compare);
    for (index = 1U; index < count; ++index)
        if (strcmp(sorted[index - 1U], sorted[index]) == 0) {
            unique = 0;
            break;
        }
    free(sorted);
    return unique;
}

static int hwa_experiment_keys_unique_casefold(const char *const *keys,
                                               size_t count)
{
    const char **sorted;
    size_t index;
    int unique = 1;
    if (count < 2U) return 1;
    if (count > SIZE_MAX / sizeof(*sorted)) return 0;
    sorted = (const char **)malloc(count * sizeof(*sorted));
    if (sorted == NULL) return 0;
    memcpy(sorted, keys, count * sizeof(*sorted));
    qsort(sorted, count, sizeof(*sorted),
          hwa_experiment_string_pointer_casefold_compare);
    for (index = 1U; index < count; ++index)
        if (hwa_experiment_ascii_case_equal(sorted[index - 1U],
                                            sorted[index])) {
            unique = 0;
            break;
        }
    free(sorted);
    return unique;
}

static int hwa_experiment_job_path_valid(const HWAExperimentJob *job)
{
    char expected[96];
    int length;
    if (job == NULL || job->run_result_path == NULL) return 0;
    length = snprintf(expected, sizeof(expected), "jobs/%s/result.hwa-run",
                      job->key);
    return length >= 0 && (size_t)length < sizeof(expected) &&
        strcmp(job->run_result_path, expected) == 0;
}

static int hwa_experiment_artifact_path_valid(
    const HWAExperimentResult *result,
    const HWAExperimentArtifact *artifact)
{
    char prefix[80];
    int length;
    const char *base;
    if (result == NULL || artifact == NULL || artifact->path == NULL ||
        artifact->job_id == 0U || artifact->job_id > result->job_count)
        return 0;
    length = snprintf(prefix, sizeof(prefix), "jobs/%s/",
                      result->jobs[artifact->job_id - 1U].key);
    if (length < 0 || (size_t)length >= sizeof(prefix) ||
        strncmp(artifact->path, prefix, (size_t)length) != 0) return 0;
    base = artifact->path + (size_t)length;
    return hwa_experiment_output_name_valid(base);
}

int hwa_experiment_result_validate_locale(
    const HWAExperimentResult *result,
    const HWANumericLocale *locale,
    char *error,
    size_t error_size)
{
    uint64_t expected_values;
    uint64_t expected_jobs;
    uint64_t expected_observations;
    uint64_t expected_evaluations;
    uint64_t expected_output;
    uint64_t expected_duration = 0U;
    uint64_t retained;
    uint64_t artifact_sum = 0U;
    uint64_t candidate_cap;
    uint64_t peak_work;
    size_t expected_level = 0U;
    size_t index;
    size_t actual_reused = 0U;
    const char **point_keys = NULL;
    const char **job_keys = NULL;
    HWAExperimentResult rebuilt;
    int valid = 0;
    memset(&rebuilt, 0, sizeof(rebuilt));
    if (result == NULL || !hwa_experiment_options_valid(&result->options) ||
        result->plan_kind <= 0 ||
        result->plan_kind >= HWA_EXPERIMENT_PLAN_KIND_COUNT ||
        result->plan_replicates == 0U ||
        result->plan_replicates > result->options.max_replicates ||
        result->manifest_path == NULL || result->manifest_path[0] == '\0' ||
        !hwa_experiment_hash_valid(result->manifest_sha256) ||
        result->output_directory == NULL || result->output_directory[0] == '\0' ||
        result->renderer_id == NULL ||
        !hwa_experiment_token_valid(result->renderer_id, 0) ||
        !hwa_experiment_hash_valid(result->renderer_sha256) ||
        result->input_count == 0U ||
        result->input_count > result->options.max_artifacts ||
        result->parameter_count == 0U ||
        result->parameter_count > result->options.max_parameters ||
        result->level_count > result->options.max_levels ||
        result->case_count < 2U ||
        result->case_count > result->options.max_cases ||
        result->response_count == 0U ||
        result->response_count > result->options.max_responses ||
        result->point_count == 0U ||
        result->point_count > result->options.max_points ||
        (result->input_count != 0U && result->inputs == NULL) ||
        result->parameters == NULL ||
        (result->level_count != 0U && result->levels == NULL) ||
        result->cases == NULL ||
        result->responses == NULL || result->points == NULL ||
        hwa_experiment_u64_multiply((uint64_t)result->point_count,
                                    (uint64_t)result->parameter_count,
                                    &expected_values) != 0 ||
        expected_values != result->value_count ||
        result->values == NULL ||
        hwa_experiment_u64_multiply((uint64_t)result->point_count,
                                    (uint64_t)result->case_count,
                                    &expected_jobs) != 0 ||
        hwa_experiment_u64_multiply(expected_jobs,
                                    (uint64_t)result->plan_replicates,
                                    &expected_jobs) != 0 ||
        expected_jobs != result->job_count || result->jobs == NULL ||
        result->job_count > result->options.max_jobs ||
        hwa_experiment_u64_multiply(expected_jobs,
                                    (uint64_t)result->response_count,
                                    &expected_observations) != 0 ||
        expected_observations != result->observation_count ||
        result->observation_count > result->options.max_observations ||
        result->observations == NULL ||
        result->artifact_count > result->options.max_artifacts ||
        (result->artifact_count != 0U && result->artifacts == NULL) ||
        hwa_experiment_u64_multiply(
            (uint64_t)result->options.max_points,
            (uint64_t)result->options.max_responses, &candidate_cap) != 0 ||
        hwa_experiment_u64_multiply(candidate_cap, UINT64_C(2),
                                    &candidate_cap) != 0 ||
        result->candidate_count > candidate_cap ||
        (result->candidate_count != 0U && result->candidates == NULL) ||
        result->sensitivity_count > result->options.max_sensitivities ||
        (result->sensitivity_count != 0U && result->sensitivities == NULL) ||
        result->interaction_count > result->options.max_interactions ||
        (result->interaction_count != 0U && result->interactions == NULL) ||
        result->warning_count > result->options.max_warnings ||
        (result->warning_count != 0U && result->warnings == NULL) ||
        result->rendered_job_count > result->job_count ||
        result->reused_job_count > result->job_count ||
        result->rendered_job_count >
            result->job_count - result->reused_job_count) goto failed;
    if (hwa_experiment_result_peak_work_bytes(
            result, 0U, &peak_work) != 0 ||
        peak_work > result->options.max_work_bytes)
        goto failed;
    for (index = 0U; index < result->input_count; ++index) {
        const HWAExperimentInput *input = &result->inputs[index];
        if (input->id != (uint64_t)index + UINT64_C(1) ||
            !hwa_experiment_token_valid(input->binding_id, 0) ||
            input->path == NULL || input->path[0] == '\0' ||
            !hwa_experiment_hash_valid(input->sha256) || input->file_bytes == 0U ||
            input->file_bytes > result->options.max_input_bytes ||
            (index != 0U && strcmp(result->inputs[index - 1U].binding_id,
                                   input->binding_id) >= 0)) goto failed;
    }
    for (index = 0U; index < result->parameter_count; ++index) {
        const HWAExperimentParameter *parameter = &result->parameters[index];
        size_t level;
        if (parameter->id != (uint64_t)index + UINT64_C(1) ||
            !hwa_experiment_token_valid(parameter->name, 0) ||
            !hwa_experiment_unit_valid(parameter->unit) ||
            !isfinite(parameter->minimum) || !isfinite(parameter->maximum) ||
            !isfinite(parameter->baseline) ||
            !isfinite(parameter->maximum - parameter->minimum) ||
            parameter->minimum > parameter->baseline ||
            parameter->baseline > parameter->maximum ||
            parameter->first_level != expected_level ||
            parameter->level_count > result->level_count - expected_level ||
            (result->plan_kind == HWA_EXPERIMENT_RANDOM &&
             (parameter->level_count != 0U ||
              !(parameter->minimum < parameter->maximum) ||
              !isfinite(parameter->maximum - parameter->minimum))) ||
            (result->plan_kind != HWA_EXPERIMENT_RANDOM &&
             parameter->level_count == 0U) ||
            (index != 0U && strcmp(result->parameters[index - 1U].name,
                                   parameter->name) >= 0)) goto failed;
        {
            int baseline_seen = 0;
        for (level = 0U; level < parameter->level_count; ++level) {
            size_t level_index = parameter->first_level + level;
            const HWAExperimentLevel *row = &result->levels[level_index];
            if (row->id != (uint64_t)level_index + UINT64_C(1) ||
                row->parameter_id != parameter->id || !isfinite(row->value) ||
                row->value < parameter->minimum || row->value > parameter->maximum ||
                (level != 0U &&
                 result->levels[level_index - 1U].value >= row->value))
                goto failed;
            if (row->value == parameter->baseline) baseline_seen = 1;
        }
        if (result->plan_kind != HWA_EXPERIMENT_RANDOM && !baseline_seen)
            goto failed;
        }
        expected_level += parameter->level_count;
    }
    if (expected_level != result->level_count) goto failed;
    {
        size_t fit = 0U;
        size_t check = 0U;
        double fit_weight = 0.0;
        double check_weight = 0.0;
        for (index = 0U; index < result->case_count; ++index) {
            const HWAExperimentCase *record = &result->cases[index];
            if (record->id != (uint64_t)index + UINT64_C(1) ||
                !hwa_experiment_token_valid(record->name, 0) ||
                (record->split != HWA_EXPERIMENT_FIT &&
                 record->split != HWA_EXPERIMENT_CHECK) ||
                !isfinite(record->weight) || !(record->weight > 0.0) ||
                (index != 0U && strcmp(result->cases[index - 1U].name,
                                       record->name) >= 0)) goto failed;
            if (record->split == HWA_EXPERIMENT_FIT) {
                if (!isfinite(fit_weight + record->weight)) goto failed;
                fit_weight += record->weight;
                fit++;
            } else {
                if (!isfinite(check_weight + record->weight)) goto failed;
                check_weight += record->weight;
                check++;
            }
        }
        if (fit == 0U || check == 0U) goto failed;
    }
    for (index = 0U; index < result->response_count; ++index) {
        const HWAExperimentResponse *response = &result->responses[index];
        if (response->id != (uint64_t)index + UINT64_C(1) ||
            !hwa_experiment_token_valid(response->name, 0) ||
            response->role <= 0 || response->role >= HWA_RUN_STEM_ROLE_COUNT ||
            response->feature <= 0 ||
            response->feature >= HWA_RUN_FEATURE_KIND_COUNT ||
            (response->feature != HWA_RUN_FEATURE_BAND_LEVEL_DBFS &&
             response->feature_index != 0U) ||
            (response->feature == HWA_RUN_FEATURE_BAND_LEVEL_DBFS &&
             response->feature_index >= HWA_BAND_COUNT) ||
            (index != 0U && strcmp(result->responses[index - 1U].name,
                                   response->name) >= 0)) goto failed;
    }
    for (index = 0U; index < result->point_count; ++index) {
        const HWAExperimentPoint *point = &result->points[index];
        if (point->id != (uint64_t)index + UINT64_C(1) ||
            !hwa_experiment_hash_valid(point->key) ||
            !hwa_experiment_boolean(point->baseline) ||
            point->baseline != (index == 0U)) goto failed;
    }
    point_keys = (const char **)malloc(result->point_count * sizeof(*point_keys));
    if (point_keys == NULL) goto failed;
    for (index = 0U; index < result->point_count; ++index)
        point_keys[index] = result->points[index].key;
    if (!hwa_experiment_keys_unique(point_keys, result->point_count)) goto failed;
    free(point_keys);
    point_keys = NULL;
    for (index = 0U; index < result->value_count; ++index) {
        const HWAExperimentValue *value = &result->values[index];
        size_t point = index / result->parameter_count;
        size_t parameter = index % result->parameter_count;
        if (value->id != (uint64_t)index + UINT64_C(1) ||
            value->point_id != (uint64_t)point + UINT64_C(1) ||
            value->parameter_id != (uint64_t)parameter + UINT64_C(1) ||
            !isfinite(value->value) ||
            value->value < result->parameters[parameter].minimum ||
            value->value > result->parameters[parameter].maximum) goto failed;
    }
    for (index = 0U; index < result->job_count; ++index) {
        const HWAExperimentJob *job = &result->jobs[index];
        size_t replicate = index % result->plan_replicates;
        size_t cell = index / result->plan_replicates;
        size_t case_index = cell % result->case_count;
        size_t point = cell / result->case_count;
        if (job->id != (uint64_t)index + UINT64_C(1) ||
            !hwa_experiment_hash_valid(job->key) ||
            job->point_id != (uint64_t)point + UINT64_C(1) ||
            job->case_id != (uint64_t)case_index + UINT64_C(1) ||
            job->replicate != replicate ||
            !hwa_experiment_job_path_valid(job) ||
            !hwa_experiment_hash_valid(job->run_manifest_sha256) ||
            !hwa_experiment_hash_valid(job->run_result_sha256) ||
            !hwa_experiment_boolean(job->reused) ||
            job->run_evaluations > result->options.run.max_evaluations ||
            job->duration_milliseconds >
                result->options.max_job_milliseconds ||
            hwa_experiment_u64_add(expected_duration,
                                   job->duration_milliseconds,
                                   &expected_duration) != 0) goto failed;
        if (job->reused) actual_reused++;
    }
    if (actual_reused != result->reused_job_count ||
        result->rendered_job_count != result->job_count - actual_reused)
        goto failed;
    job_keys = (const char **)malloc(result->job_count * sizeof(*job_keys));
    if (job_keys == NULL) goto failed;
    for (index = 0U; index < result->job_count; ++index)
        job_keys[index] = result->jobs[index].key;
    if (!hwa_experiment_keys_unique(job_keys, result->job_count)) goto failed;
    free(job_keys);
    job_keys = NULL;
    {
        size_t artifact_index = 0U;
        for (index = 0U; index < result->job_count; ++index) {
            size_t first = artifact_index;
            size_t count;
            const char **resource_keys = NULL;
            const char **path_keys = NULL;
            artifact_sum = 0U;
            while (artifact_index < result->artifact_count &&
                   result->artifacts[artifact_index].job_id ==
                       (uint64_t)index + UINT64_C(1)) {
                const HWAExperimentArtifact *artifact =
                    &result->artifacts[artifact_index];
                if (artifact->id !=
                        (uint64_t)artifact_index + UINT64_C(1) ||
                    !hwa_experiment_token_valid(artifact->resource_id, 0) ||
                    !hwa_experiment_artifact_path_valid(result, artifact) ||
                    !hwa_experiment_hash_valid(artifact->sha256) ||
                    artifact->file_bytes == 0U ||
                    artifact->file_bytes >
                        result->options.max_output_file_bytes ||
                    (artifact->kind != HWA_RUN_SOURCE_STEM &&
                     artifact->kind != HWA_RUN_SOURCE_PROBE) ||
                    hwa_experiment_u64_add(artifact_sum,
                                           artifact->file_bytes,
                                           &artifact_sum) != 0) goto failed;
                artifact_index++;
            }
            if (artifact_sum != result->jobs[index].output_bytes) goto failed;
            count = artifact_index - first;
            if (count != 0U) {
                size_t scan;
                if (count > SIZE_MAX / sizeof(*resource_keys) ||
                    count > SIZE_MAX / sizeof(*path_keys)) goto failed;
                resource_keys = (const char **)malloc(
                    count * sizeof(*resource_keys));
                path_keys = (const char **)malloc(count * sizeof(*path_keys));
                if (resource_keys == NULL || path_keys == NULL) {
                    free(resource_keys);
                    free(path_keys);
                    goto failed;
                }
                for (scan = 0U; scan < count; ++scan) {
                    resource_keys[scan] =
                        result->artifacts[first + scan].resource_id;
                    path_keys[scan] = result->artifacts[first + scan].path;
                }
                if (!hwa_experiment_keys_unique(resource_keys, count) ||
                    !hwa_experiment_keys_unique_casefold(path_keys, count)) {
                    free(resource_keys);
                    free(path_keys);
                    goto failed;
                }
                free(resource_keys);
                free(path_keys);
            }
        }
        if (artifact_index != result->artifact_count) goto failed;
    }
    for (index = 0U; index < result->candidate_count; ++index) {
        const HWAExperimentCandidate *candidate = &result->candidates[index];
        if (!hwa_experiment_boolean(candidate->values_valid) ||
            (candidate->quality_flags &
             ~(uint32_t)(HWA_EXPERIMENT_QUALITY_FLAT |
                         HWA_EXPERIMENT_QUALITY_NONMONOTONIC |
                         HWA_EXPERIMENT_QUALITY_REPLICATE_NOISE |
                         HWA_EXPERIMENT_QUALITY_CHECK_HARM |
                         HWA_EXPERIMENT_QUALITY_RANDOM_LINEAR_ONLY)) != 0U)
            goto failed;
    }
    for (index = 0U; index < result->sensitivity_count; ++index) {
        const HWAExperimentSensitivity *sensitivity =
            &result->sensitivities[index];
        if (!hwa_experiment_boolean(sensitivity->linear_valid) ||
            !hwa_experiment_boolean(sensitivity->effect_valid) ||
            !hwa_experiment_boolean(sensitivity->noise_valid) ||
            (sensitivity->quality_flags &
             ~(uint32_t)(HWA_EXPERIMENT_QUALITY_FLAT |
                         HWA_EXPERIMENT_QUALITY_NONMONOTONIC |
                         HWA_EXPERIMENT_QUALITY_REPLICATE_NOISE |
                         HWA_EXPERIMENT_QUALITY_CHECK_HARM |
                         HWA_EXPERIMENT_QUALITY_RANDOM_LINEAR_ONLY)) != 0U)
            goto failed;
    }
    for (index = 0U; index < result->interaction_count; ++index)
        if (!hwa_experiment_boolean(result->interactions[index].effect_valid))
            goto failed;
    for (index = 0U; index < result->observation_count; ++index) {
        const HWAExperimentObservation *observation =
            &result->observations[index];
        size_t job = index / result->response_count;
        size_t response = index % result->response_count;
        if (observation->id != (uint64_t)index + UINT64_C(1) ||
            observation->job_id != (uint64_t)job + UINT64_C(1) ||
            observation->response_id !=
                (uint64_t)response + UINT64_C(1) ||
            observation->availability <= 0 ||
            observation->availability >= HWA_RUN_AVAILABILITY_COUNT ||
            !hwa_experiment_boolean(observation->value_valid) ||
            (observation->quality_flags &
             ~(uint32_t)(HWA_RUN_QUALITY_CLOCK_OFFSET |
                         HWA_RUN_QUALITY_CLOCK_DRIFT |
                         HWA_RUN_QUALITY_LOW_OVERLAP |
                         HWA_RUN_QUALITY_LOW_VARIANCE |
                         HWA_RUN_QUALITY_STAGE_CONFOUNDED)) != 0U ||
            (observation->value_valid &&
             (observation->availability != HWA_RUN_AVAILABLE ||
              !isfinite(observation->value) || observation->value < 0.0 ||
              observation->value > 1.0)) ||
            (!observation->value_valid &&
             (observation->availability == HWA_RUN_AVAILABLE ||
              observation->value != 0.0 || observation->quality_flags != 0U)))
            goto failed;
    }
    if (hwa_experiment_plan_replay_validate(result, locale) != 0) goto failed;
    if (hwa_experiment_total_run_evaluations_expected(
            result, &expected_evaluations) != 0 ||
        expected_evaluations != result->total_run_evaluations ||
        result->total_run_evaluations >
            result->options.max_total_run_evaluations ||
        hwa_experiment_total_output_bytes_expected(
            result, &expected_output) != 0 ||
        expected_output != result->total_output_bytes ||
        result->total_output_bytes > result->options.max_bundle_bytes ||
        expected_duration != result->total_duration_milliseconds ||
        result->total_duration_milliseconds >
            result->options.max_total_milliseconds) goto failed;
    rebuilt = *result;
    rebuilt.candidates = NULL;
    rebuilt.candidate_count = 0U;
    rebuilt.sensitivities = NULL;
    rebuilt.sensitivity_count = 0U;
    rebuilt.interactions = NULL;
    rebuilt.interaction_count = 0U;
    rebuilt.warnings = NULL;
    rebuilt.warning_count = 0U;
    if (hwa_experiment_derived_rebuild(&rebuilt, NULL, 0U) != 0 ||
        rebuilt.candidate_count != result->candidate_count ||
        rebuilt.sensitivity_count != result->sensitivity_count ||
        rebuilt.interaction_count != result->interaction_count ||
        rebuilt.warning_count != result->warning_count) goto failed;
    for (index = 0U; index < result->candidate_count; ++index)
        if (!hwa_experiment_candidate_equal(&result->candidates[index],
                                            &rebuilt.candidates[index]))
            goto failed;
    for (index = 0U; index < result->sensitivity_count; ++index)
        if (!hwa_experiment_sensitivity_equal(&result->sensitivities[index],
                                              &rebuilt.sensitivities[index]))
            goto failed;
    for (index = 0U; index < result->interaction_count; ++index)
        if (!hwa_experiment_interaction_equal(&result->interactions[index],
                                              &rebuilt.interactions[index]))
            goto failed;
    for (index = 0U; index < result->warning_count; ++index) {
        const HWAExperimentWarning *left = &result->warnings[index];
        const HWAExperimentWarning *right = &rebuilt.warnings[index];
        if (left->id != right->id || left->code == NULL ||
            strcmp(left->code, right->code) != 0 || left->message == NULL ||
            strcmp(left->message, right->message) != 0 ||
            left->job_id != right->job_id || left->point_id != right->point_id ||
            left->parameter_id != right->parameter_id ||
            left->response_id != right->response_id ||
            left->job_id_valid != right->job_id_valid ||
            left->point_id_valid != right->point_id_valid ||
            left->parameter_id_valid != right->parameter_id_valid ||
            left->response_id_valid != right->response_id_valid) goto failed;
    }
    if (hwa_experiment_result_retained_bytes(result, &retained) != 0 ||
        retained != result->retained_work_bytes ||
        retained > result->options.max_work_bytes) goto failed;
    valid = 1;
failed:
    free(point_keys);
    free(job_keys);
    hwa_experiment_derived_free(&rebuilt);
    if (!valid) {
        hwa_set_error(error, error_size, "invalid Stage 8 result");
        return -1;
    }
    return 0;
}

int hwa_experiment_result_validate(const HWAExperimentResult *result,
                                   char *error,
                                   size_t error_size)
{
    HWANumericLocale locale;
    int status;
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_set_error(error, error_size,
                      "cannot enter the C numeric locale for Stage 8 validation");
        return -1;
    }
    /* Public validation is independent of a caller's active execution cap. */
    hwa_experiment_deadline_enter(UINT64_C(0), UINT64_MAX);
    status = hwa_experiment_result_validate_locale(
        result, &locale, error, error_size);
    hwa_experiment_deadline_leave();
    if (hwa_c_numeric_locale_end(&locale) != 0) {
        hwa_set_error(error, error_size,
                      "cannot restore the numeric locale after Stage 8 validation");
        return -1;
    }
    return status;
}

void hwa_experiment_options_default(HWAExperimentOptions *options)
{
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    hwa_run_options_default(&options->run);
    options->max_manifest_bytes = UINT64_C(8388608);
    options->max_input_bytes = UINT64_C(17179869184);
    options->max_work_bytes = UINT64_C(1073741824);
    options->max_bundle_bytes = UINT64_C(17179869184);
    options->max_output_file_bytes = UINT64_C(4294967296);
    options->max_total_run_evaluations = UINT64_C(2000000000);
    options->max_job_milliseconds = UINT64_C(3600000);
    options->max_total_milliseconds = UINT64_C(86400000);
    options->max_parameters = 64U;
    options->max_levels = 4096U;
    options->max_cases = 256U;
    options->max_responses = 256U;
    options->max_points = 100000U;
    options->max_jobs = 100000U;
    options->max_replicates = 64U;
    options->max_artifacts = 1000000U;
    options->max_observations = 1000000U;
    options->max_sensitivities = 100000U;
    options->max_interactions = 1000000U;
    options->max_warnings = 100000U;
}

void hwa_experiment_result_free(HWAExperimentResult *result)
{
    size_t index;
    if (result == NULL) return;
    free(result->manifest_path);
    free(result->output_directory);
    free(result->resume_directory);
    free(result->renderer_id);
    for (index = 0U; index < result->input_count; ++index) {
        free(result->inputs[index].binding_id);
        free(result->inputs[index].path);
    }
    for (index = 0U; index < result->parameter_count; ++index) {
        free(result->parameters[index].name);
        free(result->parameters[index].unit);
    }
    for (index = 0U; index < result->case_count; ++index)
        free(result->cases[index].name);
    for (index = 0U; index < result->response_count; ++index)
        free(result->responses[index].name);
    for (index = 0U; index < result->job_count; ++index)
        free(result->jobs[index].run_result_path);
    for (index = 0U; index < result->artifact_count; ++index) {
        free(result->artifacts[index].resource_id);
        free(result->artifacts[index].path);
    }
    free(result->inputs);
    free(result->parameters);
    free(result->levels);
    free(result->cases);
    free(result->responses);
    free(result->points);
    free(result->values);
    free(result->jobs);
    free(result->artifacts);
    free(result->observations);
    hwa_experiment_derived_free(result);
    memset(result, 0, sizeof(*result));
}

static const HWARunBinding *hwa_experiment_binding_find(
    const HWARunBinding *bindings,
    size_t binding_count,
    const char *id)
{
    size_t index;
    for (index = 0U; index < binding_count; ++index)
        if (bindings[index].id != NULL && strcmp(bindings[index].id, id) == 0)
            return &bindings[index];
    return NULL;
}

static int hwa_experiment_inputs_bind(HWAExperimentManifest *manifest,
                                      const HWARunBinding *bindings,
                                      size_t binding_count,
                                      const HWAExperimentOptions *options,
                                      HWAExperimentFileIdentity *identities,
                                      char *error,
                                      size_t error_size)
{
    size_t index;
    if (binding_count != manifest->input_count ||
        (binding_count != 0U && bindings == NULL)) {
        hwa_set_error(error, error_size,
                      "Stage 8 requires one binding per fixed input");
        return -1;
    }
    for (index = 0U; index < binding_count; ++index) {
        size_t other;
        if (bindings[index].id == NULL || bindings[index].path == NULL ||
            bindings[index].id[0] == '\0' || bindings[index].path[0] == '\0') {
            hwa_set_error(error, error_size,
                          "invalid Stage 8 binding");
            return -1;
        }
        for (other = index + 1U; other < binding_count; ++other)
            if (bindings[other].id == NULL ||
                strcmp(bindings[index].id, bindings[other].id) == 0) {
                hwa_set_error(error, error_size,
                              "duplicate or invalid Stage 8 binding");
                return -1;
            }
    }
    for (index = 0U; index < manifest->input_count; ++index) {
        HWAExperimentInput *input = &manifest->inputs[index];
        const HWARunBinding *binding = hwa_experiment_binding_find(
            bindings, binding_count, input->binding_id);
        char actual[HWA_SHA256_HEX_SIZE];
        if (binding == NULL || binding->path == NULL ||
            binding->path[0] == '\0' || strcmp(binding->path, "-") == 0 ||
            identities == NULL ||
            hwa_experiment_hash_file_identity(
                binding->path, options->max_input_bytes, NULL,
                &identities[index], actual, error, error_size) != 0 ||
            strcmp(actual, input->sha256) != 0) {
            if (error == NULL || error_size == 0U || error[0] == '\0')
                hwa_set_error(error, error_size,
                              "invalid Stage 8 fixed input binding");
            return -1;
        }
        {
            size_t other;
            for (other = 0U; other < index; ++other)
                if (hwa_experiment_file_identity_equal(
                        &identities[other], &identities[index])) {
                    hwa_set_error(error, error_size,
                                  "fixed Stage 8 inputs alias one file");
                    return -1;
                }
        }
        input->path = hwa_experiment_copy_string(binding->path);
        input->file_bytes = identities[index].size;
        if (input->path == NULL) return -1;
    }
    return 0;
}

static int hwa_experiment_manifest_transfer(HWAExperimentManifest *manifest,
                                            HWAExperimentResult *result)
{
    result->plan_kind = manifest->plan_kind;
    result->plan_seed = manifest->plan_seed;
    result->plan_replicates = manifest->replicates;
    result->inputs = manifest->inputs;
    result->input_count = manifest->input_count;
    result->parameters = manifest->parameters;
    result->parameter_count = manifest->parameter_count;
    result->levels = manifest->levels;
    result->level_count = manifest->level_count;
    result->cases = manifest->cases;
    result->case_count = manifest->case_count;
    result->responses = manifest->responses;
    result->response_count = manifest->response_count;
    return 0;
}

static int hwa_experiment_array_compact(void **array,
                                        size_t count,
                                        size_t element_size)
{
    void *exact;
    if (count == 0U) {
        free(*array);
        *array = NULL;
        return 0;
    }
    if (element_size != 0U && count > SIZE_MAX / element_size) return -1;
    exact = realloc(*array, count * element_size);
    if (exact == NULL) return -1;
    *array = exact;
    return 0;
}

static int hwa_experiment_manifest_compact(HWAExperimentManifest *manifest)
{
    size_t index;
    if (hwa_experiment_array_compact(
            (void **)&manifest->inputs, manifest->input_count,
            sizeof(*manifest->inputs)) != 0 ||
        hwa_experiment_array_compact(
            (void **)&manifest->parameters, manifest->parameter_count,
            sizeof(*manifest->parameters)) != 0 ||
        hwa_experiment_array_compact(
            (void **)&manifest->levels, manifest->level_count,
            sizeof(*manifest->levels)) != 0 ||
        hwa_experiment_array_compact(
            (void **)&manifest->cases, manifest->case_count,
            sizeof(*manifest->cases)) != 0 ||
        hwa_experiment_array_compact(
            (void **)&manifest->case_templates, manifest->case_count,
            sizeof(*manifest->case_templates)) != 0 ||
        hwa_experiment_array_compact(
            (void **)&manifest->responses, manifest->response_count,
            sizeof(*manifest->responses)) != 0)
        return -1;
    for (index = 0U; index < manifest->case_count; ++index) {
        HWAExperimentCaseTemplate *item = &manifest->case_templates[index];
        if (hwa_experiment_array_compact(
                (void **)&item->stems, item->stem_count,
                sizeof(*item->stems)) != 0 ||
            hwa_experiment_array_compact(
                (void **)&item->probes, item->probe_count,
                sizeof(*item->probes)) != 0 ||
            hwa_experiment_array_compact(
                (void **)&item->links, item->link_count,
                sizeof(*item->links)) != 0)
            return -1;
        item->stem_capacity = item->stem_count;
        item->probe_capacity = item->probe_count;
        item->link_capacity = item->link_count;
    }
    manifest->input_capacity = manifest->input_count;
    manifest->parameter_capacity = manifest->parameter_count;
    manifest->level_capacity = manifest->level_count;
    manifest->case_capacity = manifest->case_count;
    manifest->response_capacity = manifest->response_count;
    return 0;
}

/*
 * Fail before plan/job allocation when the declared catalog cannot fit the
 * live Stage 8 ledger. The bound includes the parsed manifest, all raw result
 * rows, output identity rows, derived rebuild/validator storage, the largest
 * per-job request, and one complete Stage 7 work allowance. String and path
 * storage gets a manifest-sized allowance plus fixed canonical job paths.
 */
static int hwa_experiment_execute_work_preflight(
    const HWAExperimentManifest *manifest,
    const HWAExperimentOptions *options,
    uint64_t parsed_work,
    size_t output_root_length,
    size_t resume_root_length,
    uint64_t resume_file_bytes,
    int with_resume,
    char *error,
    size_t error_size)
{
    uint64_t work = parsed_work;
    uint64_t points;
    uint64_t values;
    uint64_t jobs;
    uint64_t observations;
    uint64_t artifacts = 0U;
    uint64_t candidates;
    uint64_t sensitivities;
    uint64_t interactions;
    uint64_t pairs = 0U;
    uint64_t response_splits;
    uint64_t warnings;
    uint64_t bytes;
    uint64_t request_rows = 0U;
    uint64_t request_bindings = 0U;
    uint64_t max_outputs_per_job = 0U;
    uint64_t max_levels = 0U;
    size_t point_count;
    size_t index;
#define HWA_EXEC_WORK_ARRAY(rows, type)                                    \
    do {                                                                   \
        if (hwa_experiment_u64_multiply((rows), (uint64_t)sizeof(type),    \
                                        &bytes) != 0 ||                     \
            hwa_experiment_work_add(&work, bytes, options->max_work_bytes, \
                                    error, error_size) != 0)                \
            goto failed;                                                   \
    } while (0)
    if (manifest == NULL || options == NULL ||
        hwa_experiment_plan_point_count(manifest, &point_count) != 0)
        goto failed;
    points = (uint64_t)point_count;
    if (hwa_experiment_u64_multiply(points,
                                    (uint64_t)manifest->parameter_count,
                                    &values) != 0 ||
        hwa_experiment_u64_multiply(points,
                                    (uint64_t)manifest->case_count,
                                    &jobs) != 0 ||
        hwa_experiment_u64_multiply(jobs,
                                    (uint64_t)manifest->replicates,
                                    &jobs) != 0 ||
        hwa_experiment_u64_multiply(jobs,
                                    (uint64_t)manifest->response_count,
                                    &observations) != 0 ||
        hwa_experiment_u64_multiply((uint64_t)manifest->response_count,
                                    UINT64_C(2), &response_splits) != 0)
        goto failed;
    for (index = 0U; index < manifest->case_count; ++index) {
        uint64_t outputs = (uint64_t)hwa_experiment_case_output_count(
            &manifest->case_templates[index]);
        uint64_t per_case;
        uint64_t inputs = (uint64_t)(
            manifest->case_templates[index].stem_count +
            manifest->case_templates[index].probe_count) - outputs;
        uint64_t rows = outputs > inputs ? outputs : inputs;
        uint64_t bindings_for_case = outputs + inputs;
        if (hwa_experiment_u64_multiply(outputs, points, &per_case) != 0 ||
            hwa_experiment_u64_multiply(
                per_case, (uint64_t)manifest->replicates, &per_case) != 0 ||
            hwa_experiment_u64_add(artifacts, per_case, &artifacts) != 0)
            goto failed;
        if (rows > request_rows) request_rows = rows;
        if (bindings_for_case > request_bindings)
            request_bindings = bindings_for_case;
        if (outputs > max_outputs_per_job) max_outputs_per_job = outputs;
    }
    for (index = 0U; index < manifest->parameter_count; ++index)
        if ((uint64_t)manifest->parameters[index].level_count > max_levels)
            max_levels = (uint64_t)manifest->parameters[index].level_count;
    if (hwa_experiment_u64_multiply(points,
                                    (uint64_t)manifest->response_count,
                                    &candidates) != 0 ||
        hwa_experiment_u64_multiply(candidates, UINT64_C(2),
                                    &candidates) != 0 ||
        hwa_experiment_u64_multiply((uint64_t)manifest->parameter_count,
                                    response_splits, &sensitivities) != 0)
        goto failed;
    if (manifest->parameter_count > 1U) {
        uint64_t parameters = (uint64_t)manifest->parameter_count;
        uint64_t other = parameters - UINT64_C(1);
        if ((parameters & UINT64_C(1)) == 0U) parameters /= UINT64_C(2);
        else other /= UINT64_C(2);
        if (hwa_experiment_u64_multiply(parameters, other, &pairs) != 0)
            goto failed;
    }
    if (hwa_experiment_u64_multiply(pairs, response_splits,
                                    &interactions) != 0 ||
        sensitivities > (uint64_t)options->max_sensitivities ||
        interactions > (uint64_t)options->max_interactions ||
        hwa_experiment_u64_multiply(sensitivities, UINT64_C(2),
                                    &warnings) != 0 ||
        hwa_experiment_u64_add(warnings, candidates, &warnings) != 0)
        goto failed;
    if (warnings > (uint64_t)options->max_warnings)
        warnings = (uint64_t)options->max_warnings;
    HWA_EXEC_WORK_ARRAY(points, HWAExperimentPoint);
    HWA_EXEC_WORK_ARRAY(values, HWAExperimentValue);
    HWA_EXEC_WORK_ARRAY(jobs, HWAExperimentJob);
    HWA_EXEC_WORK_ARRAY(observations, HWAExperimentObservation);
    HWA_EXEC_WORK_ARRAY(artifacts, HWAExperimentArtifact);
    HWA_EXEC_WORK_ARRAY(jobs, HWAExperimentFileIdentity);
    HWA_EXEC_WORK_ARRAY(jobs, HWAExperimentFileIdentity);
    HWA_EXEC_WORK_ARRAY(artifacts, HWAExperimentFileIdentity);
    HWA_EXEC_WORK_ARRAY((uint64_t)manifest->input_count,
                        HWAExperimentFileIdentity);
    /* One retained derived set plus the validator's rebuilt set. */
    HWA_EXEC_WORK_ARRAY(candidates, HWAExperimentCandidate);
    HWA_EXEC_WORK_ARRAY(candidates, HWAExperimentCandidate);
    HWA_EXEC_WORK_ARRAY(sensitivities, HWAExperimentSensitivity);
    HWA_EXEC_WORK_ARRAY(sensitivities, HWAExperimentSensitivity);
    HWA_EXEC_WORK_ARRAY(interactions, HWAExperimentInteraction);
    HWA_EXEC_WORK_ARRAY(interactions, HWAExperimentInteraction);
    HWA_EXEC_WORK_ARRAY(warnings, HWAExperimentWarning);
    HWA_EXEC_WORK_ARRAY(warnings, HWAExperimentWarning);
    if (hwa_experiment_u64_multiply(warnings, UINT64_C(256), &bytes) != 0 ||
        hwa_experiment_work_add(&work, bytes, options->max_work_bytes,
                                error, error_size) != 0)
        goto failed;
    /* Point/job uniqueness copies and the largest request descriptor set. */
    HWA_EXEC_WORK_ARRAY(points, const char *);
    HWA_EXEC_WORK_ARRAY(points, const char *);
    HWA_EXEC_WORK_ARRAY(jobs, const char *);
    HWA_EXEC_WORK_ARRAY(jobs, const char *);
    HWA_EXEC_WORK_ARRAY(request_rows, HWAExperimentRenderInput);
    HWA_EXEC_WORK_ARRAY(request_rows, HWAExperimentRenderOutput);
    HWA_EXEC_WORK_ARRAY(request_bindings, HWARunBinding);
    HWA_EXEC_WORK_ARRAY(request_rows, char *);
    HWA_EXEC_WORK_ARRAY((uint64_t)manifest->parameter_count,
                        HWAExperimentRenderParameter);
    HWA_EXEC_WORK_ARRAY(points, HWAExperimentXY);
    HWA_EXEC_WORK_ARRAY(response_splits, double);
    HWA_EXEC_WORK_ARRAY(max_outputs_per_job, const char *);
    HWA_EXEC_WORK_ARRAY(max_outputs_per_job, const char *);
    HWA_EXEC_WORK_ARRAY(max_outputs_per_job, const char *);
    if (manifest->plan_kind == HWA_EXPERIMENT_GRID) {
        uint64_t cells;
        if (hwa_experiment_u64_multiply(max_levels, max_levels,
                                        &cells) != 0)
            goto failed;
        HWA_EXEC_WORK_ARRAY(cells, double);
        HWA_EXEC_WORK_ARRAY(cells, double);
        HWA_EXEC_WORK_ARRAY(cells, double);
        HWA_EXEC_WORK_ARRAY(cells, size_t);
        HWA_EXEC_WORK_ARRAY(cells, size_t);
        HWA_EXEC_WORK_ARRAY(cells, size_t);
    }
    /* Absolute live paths include the full output root for each job/artifact. */
    if ((uint64_t)output_root_length > UINT64_MAX - UINT64_C(384) ||
        hwa_experiment_u64_multiply(
            jobs, (uint64_t)output_root_length + UINT64_C(384),
            &bytes) != 0 ||
        hwa_experiment_work_add(&work, bytes, options->max_work_bytes,
                                error, error_size) != 0 ||
        (uint64_t)output_root_length > UINT64_MAX - UINT64_C(384) ||
        hwa_experiment_u64_multiply(
            artifacts, (uint64_t)output_root_length + UINT64_C(384),
            &bytes) != 0 ||
        hwa_experiment_work_add(&work, bytes, options->max_work_bytes,
                                error, error_size) != 0 ||
        (uint64_t)output_root_length > UINT64_MAX - UINT64_C(192) ||
        max_outputs_per_job > UINT64_MAX - UINT64_C(8) ||
        hwa_experiment_u64_multiply(
            max_outputs_per_job + UINT64_C(8),
            (uint64_t)output_root_length + UINT64_C(192),
            &bytes) != 0 ||
        hwa_experiment_work_add(&work, bytes, options->max_work_bytes,
                                error, error_size) != 0 ||
        hwa_experiment_work_add(&work, options->run.max_work_bytes,
                                options->max_work_bytes,
                                error, error_size) != 0)
        goto failed;
    if (with_resume) {
        /* The read-only saved result coexists with the new raw result. */
        uint64_t duplicate = work - parsed_work -
            options->run.max_work_bytes;
        if ((uint64_t)resume_root_length > UINT64_MAX - UINT64_C(192) ||
            hwa_experiment_u64_multiply(
                max_outputs_per_job + UINT64_C(8),
                (uint64_t)resume_root_length + UINT64_C(192),
                &bytes) != 0 ||
            hwa_experiment_work_add(&work, bytes,
                                    options->max_work_bytes,
                                    error, error_size) != 0)
            goto failed;
        if (hwa_experiment_work_add(&work, duplicate,
                                    options->max_work_bytes,
                                    error, error_size) != 0 ||
            hwa_experiment_work_add(&work, resume_file_bytes,
                                    options->max_work_bytes,
                                    error, error_size) != 0)
            goto failed;
    }
#undef HWA_EXEC_WORK_ARRAY
    return 0;
failed:
#undef HWA_EXEC_WORK_ARRAY
    if (error == NULL || error_size == 0U || error[0] == '\0')
        hwa_set_error(error, error_size,
                      "Stage 8 peak work cap exceeded");
    return -1;
}

static int hwa_experiment_temp_root(const char *output,
                                    char **temporary,
                                    char *error,
                                    size_t error_size)
{
    unsigned attempt;
    for (attempt = 0U; attempt < 100U; ++attempt) {
        char suffix[96];
        size_t output_size = strlen(output);
        unsigned long process_id;
        int length;
#if defined(_WIN32)
        process_id = (unsigned long)GetCurrentProcessId();
#else
        process_id = (unsigned long)getpid();
#endif
        length = snprintf(suffix, sizeof(suffix), ".tmp-stage8-%lu-%u",
                          process_id, attempt);
        char *path;
        if (length < 0 || (size_t)length >= sizeof(suffix) ||
            output_size > SIZE_MAX - (size_t)length - 1U) break;
        path = (char *)malloc(output_size + (size_t)length + 1U);
        if (path == NULL) break;
        memcpy(path, output, output_size);
        memcpy(path + output_size, suffix, (size_t)length + 1U);
        if (hwa_experiment_mkdir_new(path) == 0) {
            *temporary = path;
            return 0;
        }
        free(path);
        if (errno != EEXIST) break;
    }
    hwa_set_error(error, error_size,
                  "cannot create private Stage 8 output directory");
    return -1;
}

static int hwa_experiment_rename_no_replace(const char *from, const char *to)
{
#if defined(_WIN32)
    return MoveFileA(from, to) ? 0 : -1;
#elif defined(__APPLE__)
    return renameatx_np(AT_FDCWD, from, AT_FDCWD, to, RENAME_EXCL);
#elif defined(__linux__) && defined(SYS_renameat2)
    return (int)syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to,
                        RENAME_NOREPLACE);
#else
    (void)from;
    (void)to;
    errno = ENOTSUP;
    return -1;
#endif
}

#if defined(_WIN32)
static int hwa_experiment_windows_delete(const char *path, int directory)
{
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return -1;
    if ((attributes & FILE_ATTRIBUTE_READONLY) != 0U &&
        !SetFileAttributesA(path, attributes & ~FILE_ATTRIBUTE_READONLY))
        return -1;
    return directory ? (RemoveDirectoryA(path) ? 0 : -1)
                     : (DeleteFileA(path) ? 0 : -1);
}
#endif

static int hwa_experiment_remove_tree(const char *path)
{
#if defined(_WIN32)
    WIN32_FIND_DATAA data;
    HANDLE search;
    char *pattern = NULL;
    int status = 0;
    DWORD root_attributes = GetFileAttributesA(path);
    if (root_attributes == INVALID_FILE_ATTRIBUTES) return -1;
    if ((root_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
        return hwa_experiment_windows_delete(
            path, (root_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U);
    if ((root_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U)
        return hwa_experiment_windows_delete(path, 0);
    if (hwa_experiment_path_join(&pattern, path, "*") != 0) return -1;
    search = FindFirstFileA(pattern, &data);
    free(pattern);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            char *child = NULL;
            if (strcmp(data.cFileName, ".") == 0 ||
                strcmp(data.cFileName, "..") == 0) continue;
            if (hwa_experiment_path_join(&child, path, data.cFileName) != 0) {
                status = -1;
                break;
            }
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
                status |= hwa_experiment_windows_delete(
                    child, (data.dwFileAttributes &
                            FILE_ATTRIBUTE_DIRECTORY) != 0U);
            else if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U)
                status |= hwa_experiment_remove_tree(child);
            else status |= hwa_experiment_windows_delete(child, 0);
            free(child);
        } while (FindNextFileA(search, &data));
        FindClose(search);
    }
    if (hwa_experiment_windows_delete(path, 1) != 0) status = -1;
    return status;
#else
    struct stat root_information;
    DIR *directory;
    struct dirent *entry;
    int status = 0;
    if (lstat(path, &root_information) != 0) return -1;
    if (!S_ISDIR(root_information.st_mode)) return unlink(path);
    if (chmod(path, root_information.st_mode | S_IRUSR | S_IWUSR | S_IXUSR) != 0)
        return -1;
    directory = opendir(path);
    if (directory == NULL) return -1;
    while ((entry = readdir(directory)) != NULL) {
        char *child = NULL;
        struct stat info;
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;
        if (hwa_experiment_path_join(&child, path, entry->d_name) != 0) {
            status = -1;
            break;
        }
        if (lstat(child, &info) != 0) status = -1;
        else if (S_ISDIR(info.st_mode)) status |= hwa_experiment_remove_tree(child);
        else status |= unlink(child);
        free(child);
    }
    closedir(directory);
    if (rmdir(path) != 0) status = -1;
    return status;
#endif
}

static int hwa_experiment_job_relative_paths(HWAExperimentResult *result,
                                             char *error,
                                             size_t error_size)
{
    size_t index;
    for (index = 0U; index < result->job_count; ++index) {
        HWAExperimentJob *job = &result->jobs[index];
        char path[96];
        int length = snprintf(path, sizeof(path), "jobs/%s/result.hwa-run",
                              job->key);
        char *copy;
        if (length < 0 || (size_t)length >= sizeof(path) ||
            (copy = hwa_experiment_copy_string(path)) == NULL) goto failed;
        free(job->run_result_path);
        job->run_result_path = copy;
    }
    for (index = 0U; index < result->artifact_count; ++index) {
        HWAExperimentArtifact *artifact = &result->artifacts[index];
        const char *base = strrchr(artifact->path, HWA_PATH_SEPARATOR);
        char path[512];
        int length;
        char *copy;
        if (base == NULL || artifact->job_id == 0U ||
            artifact->job_id > result->job_count) goto failed;
        base++;
        if (!hwa_experiment_component_valid(base)) goto failed;
        length = snprintf(path, sizeof(path), "jobs/%s/%s",
                          result->jobs[artifact->job_id - 1U].key, base);
        if (length < 0 || (size_t)length >= sizeof(path) ||
            (copy = hwa_experiment_copy_string(path)) == NULL) goto failed;
        free(artifact->path);
        artifact->path = copy;
    }
    return 0;
failed:
    hwa_set_error(error, error_size,
                  "cannot make Stage 8 bundle paths relative");
    return -1;
}

static int hwa_experiment_directory_regular(const char *path)
{
#if defined(_WIN32)
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
        (attributes & (FILE_ATTRIBUTE_REPARSE_POINT |
                       FILE_ATTRIBUTE_DEVICE)) == 0U;
#else
    struct stat information;
    return lstat(path, &information) == 0 && S_ISDIR(information.st_mode);
#endif
}

static void hwa_experiment_string_array_free(char **values, size_t count)
{
    size_t index;
    for (index = 0U; index < count; ++index) free(values[index]);
    free(values);
}

static int hwa_experiment_jobs_directory_exact(
    const char *jobs_directory,
    const HWAExperimentResult *result)
{
    char **found = NULL;
    const char **expected = NULL;
    size_t count = 0U;
    size_t index;
    int status = -1;
    if (!hwa_experiment_directory_regular(jobs_directory) ||
        result->job_count > SIZE_MAX / sizeof(*found))
        return -1;
    found = (char **)calloc(result->job_count, sizeof(*found));
    expected = (const char **)malloc(result->job_count * sizeof(*expected));
    if ((result->job_count != 0U && found == NULL) ||
        (result->job_count != 0U && expected == NULL))
        goto cleanup;
#if defined(_WIN32)
    {
        WIN32_FIND_DATAA entry;
        HANDLE search;
        char *pattern = NULL;
        if (hwa_experiment_path_join(&pattern, jobs_directory, "*") != 0)
            goto cleanup;
        search = FindFirstFileA(pattern, &entry);
        free(pattern);
        if (search == INVALID_HANDLE_VALUE) goto cleanup;
        for (;;) {
            if (strcmp(entry.cFileName, ".") != 0 &&
                strcmp(entry.cFileName, "..") != 0) {
                if (count == result->job_count ||
                    (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
                    (entry.dwFileAttributes &
                     (FILE_ATTRIBUTE_REPARSE_POINT |
                      FILE_ATTRIBUTE_DEVICE)) != 0U ||
                    (found[count] = hwa_experiment_copy_string(
                        entry.cFileName)) == NULL) {
                    FindClose(search);
                    goto cleanup;
                }
                count++;
            }
            if (!FindNextFileA(search, &entry)) break;
        }
        if (GetLastError() != ERROR_NO_MORE_FILES) {
            FindClose(search);
            goto cleanup;
        }
        FindClose(search);
    }
#else
    {
        DIR *directory = opendir(jobs_directory);
        struct dirent *entry;
        if (directory == NULL) goto cleanup;
        errno = 0;
        while ((entry = readdir(directory)) != NULL) {
            char *path = NULL;
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                errno = 0;
                continue;
            }
            if (count == result->job_count ||
                hwa_experiment_path_join(&path, jobs_directory,
                                         entry->d_name) != 0 ||
                !hwa_experiment_directory_regular(path) ||
                (found[count] = hwa_experiment_copy_string(
                    entry->d_name)) == NULL) {
                free(path);
                (void)closedir(directory);
                goto cleanup;
            }
            free(path);
            count++;
            errno = 0;
        }
        if (errno != 0 || closedir(directory) != 0) goto cleanup;
    }
#endif
    if (count != result->job_count) goto cleanup;
    for (index = 0U; index < count; ++index)
        expected[index] = result->jobs[index].key;
    qsort(found, count, sizeof(*found), hwa_experiment_string_pointer_compare);
    qsort(expected, count, sizeof(*expected),
          hwa_experiment_string_pointer_compare);
    for (index = 0U; index < count; ++index)
        if (strcmp(found[index], expected[index]) != 0) goto cleanup;
    status = 0;
cleanup:
    hwa_experiment_string_array_free(found, count);
    free(expected);
    return status;
}

static int hwa_experiment_bundle_job_name_expected(
    const HWAExperimentResult *result,
    size_t artifact_first,
    size_t artifact_count,
    const char *name)
{
    size_t index;
    if (strcmp(name, "run.json") == 0 ||
        strcmp(name, "result.hwa-run") == 0)
        return 1;
    for (index = 0U; index < artifact_count; ++index) {
        const char *base = strrchr(
            result->artifacts[artifact_first + index].path, '/');
        if (base != NULL && strcmp(base + 1U, name) == 0) return 1;
    }
    return 0;
}

static int hwa_experiment_bundle_job_exact(
    const char *job_directory,
    const HWAExperimentResult *result,
    size_t job_index,
    size_t artifact_first,
    size_t artifact_count,
    HWAExperimentFileIdentity *run_manifest_identities,
    HWAExperimentFileIdentity *run_result_identities,
    HWAExperimentFileIdentity *artifact_identities,
    uint64_t *bundle_bytes,
    char *error,
    size_t error_size)
{
    size_t seen = 0U;
    size_t index;
    char *run_manifest = NULL;
    char *run_result = NULL;
    int status = -1;
    if (!hwa_experiment_directory_regular(job_directory)) goto cleanup;
#if defined(_WIN32)
    {
        WIN32_FIND_DATAA entry;
        HANDLE search;
        char *pattern = NULL;
        if (hwa_experiment_path_join(&pattern, job_directory, "*") != 0)
            goto cleanup;
        search = FindFirstFileA(pattern, &entry);
        free(pattern);
        if (search == INVALID_HANDLE_VALUE) goto cleanup;
        for (;;) {
            if (strcmp(entry.cFileName, ".") != 0 &&
                strcmp(entry.cFileName, "..") != 0) {
                if (!hwa_experiment_bundle_job_name_expected(
                        result, artifact_first, artifact_count,
                        entry.cFileName) ||
                    (entry.dwFileAttributes &
                     (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                      FILE_ATTRIBUTE_DEVICE)) != 0U) {
                    FindClose(search);
                    goto cleanup;
                }
                seen++;
            }
            if (!FindNextFileA(search, &entry)) break;
        }
        if (GetLastError() != ERROR_NO_MORE_FILES) {
            FindClose(search);
            goto cleanup;
        }
        FindClose(search);
    }
#else
    {
        DIR *directory = opendir(job_directory);
        struct dirent *entry;
        if (directory == NULL) goto cleanup;
        errno = 0;
        while ((entry = readdir(directory)) != NULL) {
            char *path = NULL;
            struct stat information;
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                errno = 0;
                continue;
            }
            if (!hwa_experiment_bundle_job_name_expected(
                    result, artifact_first, artifact_count, entry->d_name) ||
                hwa_experiment_path_join(&path, job_directory,
                                         entry->d_name) != 0 ||
                lstat(path, &information) != 0 ||
                !S_ISREG(information.st_mode)) {
                free(path);
                (void)closedir(directory);
                goto cleanup;
            }
            free(path);
            seen++;
            errno = 0;
        }
        if (errno != 0 || closedir(directory) != 0) goto cleanup;
    }
#endif
    if (seen != artifact_count + 2U ||
        hwa_experiment_path_join(&run_manifest, job_directory,
                                 "run.json") != 0 ||
        hwa_experiment_path_join(&run_result, job_directory,
                                 "result.hwa-run") != 0)
        goto cleanup;
    {
        HWAExperimentFileIdentity accepted;
        char hash[HWA_SHA256_HEX_SIZE];
        HWAExperimentFileIdentity *identity = run_manifest_identities == NULL
            ? NULL : &run_manifest_identities[job_index];
        uint64_t manifest_maximum = result->options.max_manifest_bytes;
        if (manifest_maximum > result->options.max_output_file_bytes)
            manifest_maximum = result->options.max_output_file_bytes;
        if (manifest_maximum > result->options.run.max_manifest_bytes)
            manifest_maximum = result->options.run.max_manifest_bytes;
        if (hwa_experiment_hash_file_identity(
                run_manifest, manifest_maximum,
                identity != NULL && identity->valid ? identity : NULL,
                &accepted, hash, error, error_size) != 0 ||
            strcmp(hash, result->jobs[job_index].run_manifest_sha256) != 0 ||
            hwa_experiment_u64_add(*bundle_bytes, accepted.size,
                                   bundle_bytes) != 0)
            goto cleanup;
        if (identity != NULL) *identity = accepted;
        identity = run_result_identities == NULL
            ? NULL : &run_result_identities[job_index];
        if (hwa_experiment_hash_file_identity(
                run_result, result->options.max_output_file_bytes,
                identity != NULL && identity->valid ? identity : NULL,
                &accepted, hash, error, error_size) != 0 ||
            strcmp(hash, result->jobs[job_index].run_result_sha256) != 0 ||
            hwa_experiment_u64_add(*bundle_bytes, accepted.size,
                                   bundle_bytes) != 0)
            goto cleanup;
        if (identity != NULL) *identity = accepted;
    }
    for (index = 0U; index < artifact_count; ++index) {
        size_t artifact_index = artifact_first + index;
        const HWAExperimentArtifact *artifact =
            &result->artifacts[artifact_index];
        const char *base = strrchr(artifact->path, '/');
        HWAExperimentFileIdentity accepted;
        HWAExperimentFileIdentity *identity = artifact_identities == NULL
            ? NULL : &artifact_identities[artifact_index];
        char hash[HWA_SHA256_HEX_SIZE];
        char *path = NULL;
        if (base == NULL ||
            hwa_experiment_path_join(&path, job_directory, base + 1U) != 0 ||
            hwa_experiment_hash_file_identity(
                path, result->options.max_output_file_bytes,
                identity != NULL && identity->valid ? identity : NULL,
                &accepted, hash, error, error_size) != 0 ||
            accepted.size != artifact->file_bytes ||
            strcmp(hash, artifact->sha256) != 0 ||
            hwa_experiment_u64_add(*bundle_bytes, accepted.size,
                                   bundle_bytes) != 0) {
            free(path);
            goto cleanup;
        }
        free(path);
        if (identity != NULL) *identity = accepted;
    }
    status = 0;
cleanup:
    free(run_manifest);
    free(run_result);
    return status;
}

static int hwa_experiment_bundle_exact(
    const char *root,
    const HWAExperimentResult *result,
    const char *expected_bundle_sha256,
    HWAExperimentFileIdentity *run_manifest_identities,
    HWAExperimentFileIdentity *run_result_identities,
    HWAExperimentFileIdentity *artifact_identities,
    HWAExperimentFileIdentity *bundle_identity,
    uint64_t *exact_bytes,
    char *error,
    size_t error_size)
{
    char *jobs_directory = NULL;
    char *bundle = NULL;
    uint64_t bytes = 0U;
    size_t artifact_index = 0U;
    size_t job_index;
    size_t root_seen = 0U;
    int status = -1;
    if (!hwa_experiment_directory_regular(root) || result == NULL ||
        hwa_experiment_path_join(&jobs_directory, root, "jobs") != 0 ||
        hwa_experiment_path_join(&bundle, root,
                                 "result.hwa-experiment") != 0)
        goto cleanup;
#if defined(_WIN32)
    {
        WIN32_FIND_DATAA entry;
        HANDLE search;
        char *pattern = NULL;
        if (hwa_experiment_path_join(&pattern, root, "*") != 0) goto cleanup;
        search = FindFirstFileA(pattern, &entry);
        free(pattern);
        if (search == INVALID_HANDLE_VALUE) goto cleanup;
        for (;;) {
            if (strcmp(entry.cFileName, ".") != 0 &&
                strcmp(entry.cFileName, "..") != 0) {
                int jobs = strcmp(entry.cFileName, "jobs") == 0;
                int top = strcmp(entry.cFileName,
                                 "result.hwa-experiment") == 0;
                if ((!jobs && !top) ||
                    (entry.dwFileAttributes &
                     (FILE_ATTRIBUTE_REPARSE_POINT |
                      FILE_ATTRIBUTE_DEVICE)) != 0U ||
                    (jobs && (entry.dwFileAttributes &
                              FILE_ATTRIBUTE_DIRECTORY) == 0U) ||
                    (top && (entry.dwFileAttributes &
                             FILE_ATTRIBUTE_DIRECTORY) != 0U)) {
                    FindClose(search);
                    goto cleanup;
                }
                root_seen++;
            }
            if (!FindNextFileA(search, &entry)) break;
        }
        if (GetLastError() != ERROR_NO_MORE_FILES) {
            FindClose(search);
            goto cleanup;
        }
        FindClose(search);
    }
#else
    {
        DIR *directory = opendir(root);
        struct dirent *entry;
        if (directory == NULL) goto cleanup;
        errno = 0;
        while ((entry = readdir(directory)) != NULL) {
            char *path = NULL;
            struct stat information;
            int jobs;
            int top;
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                errno = 0;
                continue;
            }
            jobs = strcmp(entry->d_name, "jobs") == 0;
            top = strcmp(entry->d_name, "result.hwa-experiment") == 0;
            if ((!jobs && !top) ||
                hwa_experiment_path_join(&path, root, entry->d_name) != 0 ||
                lstat(path, &information) != 0 ||
                (jobs && !S_ISDIR(information.st_mode)) ||
                (top && !S_ISREG(information.st_mode))) {
                free(path);
                (void)closedir(directory);
                goto cleanup;
            }
            free(path);
            root_seen++;
            errno = 0;
        }
        if (errno != 0 || closedir(directory) != 0) goto cleanup;
    }
#endif
    if (root_seen != 2U ||
        hwa_experiment_jobs_directory_exact(jobs_directory, result) != 0)
        goto cleanup;
    for (job_index = 0U; job_index < result->job_count; ++job_index) {
        size_t first = artifact_index;
        char *job_directory = NULL;
        while (artifact_index < result->artifact_count &&
               result->artifacts[artifact_index].job_id ==
                   (uint64_t)job_index + UINT64_C(1))
            artifact_index++;
        if (hwa_experiment_path_join(&job_directory, jobs_directory,
                                     result->jobs[job_index].key) != 0 ||
            hwa_experiment_bundle_job_exact(
                job_directory, result, job_index, first,
                artifact_index - first, run_manifest_identities,
                run_result_identities, artifact_identities, &bytes,
                error, error_size) != 0) {
            free(job_directory);
            goto cleanup;
        }
        free(job_directory);
    }
    if (artifact_index != result->artifact_count) goto cleanup;
    {
        HWAExperimentFileIdentity accepted;
        char hash[HWA_SHA256_HEX_SIZE];
        if (hwa_experiment_hash_file_identity(
                bundle, result->options.max_output_file_bytes,
                bundle_identity != NULL && bundle_identity->valid
                    ? bundle_identity : NULL,
                &accepted, hash, error, error_size) != 0 ||
            (expected_bundle_sha256 != NULL &&
             strcmp(hash, expected_bundle_sha256) != 0) ||
            hwa_experiment_u64_add(bytes, accepted.size, &bytes) != 0 ||
            bytes > result->options.max_bundle_bytes)
            goto cleanup;
        if (bundle_identity != NULL) *bundle_identity = accepted;
    }
    *exact_bytes = bytes;
    status = 0;
cleanup:
    if (status != 0 && (error == NULL || error_size == 0U ||
                        error[0] == '\0'))
        hwa_set_error(error, error_size,
                      "invalid Stage 8 bundle contents");
    free(jobs_directory);
    free(bundle);
    return status;
}

static int hwa_experiment_verify_inputs(
                                        const HWAExperimentResult *result,
                                        const HWAExperimentFileIdentity *identities,
                                        char *error,
                                        size_t error_size)
{
    size_t index;
    for (index = 0U; index < result->input_count; ++index) {
        char hash[HWA_SHA256_HEX_SIZE];
        HWAExperimentFileIdentity checked;
        if (identities == NULL ||
            hwa_experiment_hash_file_identity(
                result->inputs[index].path, result->options.max_input_bytes,
                &identities[index], &checked, hash,
                error, error_size) != 0 ||
            checked.size != result->inputs[index].file_bytes ||
            strcmp(hash, result->inputs[index].sha256) != 0) {
            hwa_set_error(error, error_size,
                          "Stage 8 fixed input changed during execution");
            return -1;
        }
    }
    return 0;
}

static int hwa_experiment_execute_impl(
    const char *manifest_path,
    const HWARunBinding *bindings,
    size_t binding_count,
    const char *output_directory,
    const char *resume_directory,
    const HWAExperimentRenderer *renderer,
    const HWAExperimentOptions *options,
    const HWANumericLocale *locale,
    HWAExperimentResult *result,
    char *error,
    size_t error_size)
{
    HWAExperimentBlob blob;
    HWAExperimentManifest manifest;
    HWAExperimentResult resume;
    char *absolute_output = NULL;
    char *temporary_root = NULL;
    char *jobs_directory = NULL;
    char *bundle_path = NULL;
    HWAExperimentFileIdentity *input_identities = NULL;
    HWAExperimentFileIdentity *run_manifest_identities = NULL;
    HWAExperimentFileIdentity *run_result_identities = NULL;
    HWAExperimentFileIdentity *artifact_identities = NULL;
    uint64_t live_work = 0U;
    uint64_t retained;
    uint64_t experiment_started = 0U;
    uint64_t bundle_bytes = 0U;
    uint64_t exact_bundle_bytes = 0U;
    uint64_t resume_file_bytes = 0U;
    HWAExperimentFileIdentity bundle_identity;
    HWAExperimentFileIdentity resume_bundle_identity;
    size_t point;
    size_t case_index;
    size_t replicate;
    size_t artifact_capacity = 0U;
    size_t resume_artifact_cursor = 0U;
    int status = -1;
    int manifest_rows_transferred = 0;
    int resume_loaded = 0;
    memset(&blob, 0, sizeof(blob));
    memset(&manifest, 0, sizeof(manifest));
    memset(&resume, 0, sizeof(resume));
    memset(&bundle_identity, 0, sizeof(bundle_identity));
    memset(&resume_bundle_identity, 0, sizeof(resume_bundle_identity));
    if (hwa_experiment_now_milliseconds(&experiment_started) != 0) {
        hwa_set_error(error, error_size, "cannot read Stage 8 time clock");
        goto cleanup;
    }
    hwa_experiment_deadline_enter(
        experiment_started, options->max_total_milliseconds);
    if (manifest_path == NULL || manifest_path[0] == '\0' ||
        strcmp(manifest_path, "-") == 0 || output_directory == NULL ||
        output_directory[0] == '\0' ||
        renderer == NULL || renderer->id == NULL ||
        !hwa_experiment_token_valid(renderer->id, 0) ||
        !hwa_experiment_hash_valid(renderer->sha256) ||
        renderer->render == NULL ||
        hwa_experiment_absolute_parent(output_directory,
                                       &absolute_output) != 0 ||
        hwa_experiment_blob_read(
                                 manifest_path,
                                 options->max_manifest_bytes <
                                     options->max_work_bytes
                                     ? options->max_manifest_bytes
                                     : options->max_work_bytes,
                                 &blob, error, error_size) != 0) {
        if (error == NULL || error_size == 0U || error[0] == '\0')
            hwa_set_error(error, error_size, "invalid Stage 8 arguments");
        goto cleanup;
    }
    if (resume_directory != NULL) {
        char *resume_size_path = NULL;
        if (resume_directory[0] == '\0' || strcmp(resume_directory, "-") == 0 ||
            hwa_experiment_path_join(&resume_size_path, resume_directory,
                                     "result.hwa-experiment") != 0 ||
            hwa_experiment_file_regular_size(
                resume_size_path,
                options->max_output_file_bytes < options->max_bundle_bytes
                    ? options->max_output_file_bytes
                    : options->max_bundle_bytes,
                &resume_file_bytes) != 0 ||
            resume_file_bytes == UINT64_MAX) {
            free(resume_size_path);
            hwa_set_error(error, error_size,
                          "invalid Stage 8 resume result file");
            goto cleanup;
        }
        resume_file_bytes++;
        free(resume_size_path);
    }
    memcpy(result->manifest_sha256, blob.sha256, HWA_SHA256_HEX_SIZE);
    live_work = (uint64_t)blob.size;
    if (live_work > options->max_work_bytes ||
        hwa_experiment_manifest_parse(blob.data, blob.size, options, locale,
                                      &live_work, &manifest,
                                      error, error_size) != 0)
        goto cleanup;
    {
        uint64_t projected_work = live_work;
        size_t index;
        for (index = 0U; index < binding_count; ++index) {
            if (bindings[index].id != NULL &&
                hwa_experiment_work_add(
                    &projected_work,
                    (uint64_t)strlen(bindings[index].id) + UINT64_C(1),
                    options->max_work_bytes, error, error_size) != 0)
                goto cleanup;
            if (bindings[index].path != NULL &&
                hwa_experiment_work_add(
                    &projected_work,
                    (uint64_t)strlen(bindings[index].path) + UINT64_C(1),
                    options->max_work_bytes, error, error_size) != 0)
                goto cleanup;
        }
        if (hwa_experiment_work_add(
                &projected_work,
                (uint64_t)strlen(manifest_path) + UINT64_C(1),
                options->max_work_bytes, error, error_size) != 0 ||
            hwa_experiment_work_add(
                &projected_work,
                (uint64_t)strlen(absolute_output) + UINT64_C(1),
                options->max_work_bytes, error, error_size) != 0 ||
            hwa_experiment_work_add(
                &projected_work,
                (uint64_t)strlen(renderer->id) + UINT64_C(1),
                options->max_work_bytes, error, error_size) != 0 ||
            (resume_directory != NULL &&
             hwa_experiment_work_add(
                 &projected_work,
                 (uint64_t)strlen(resume_directory) + UINT64_C(1),
                 options->max_work_bytes, error, error_size) != 0) ||
            hwa_experiment_execute_work_preflight(
                &manifest, options, projected_work, strlen(absolute_output),
                resume_directory == NULL ? 0U : strlen(resume_directory),
                resume_file_bytes,
                resume_directory != NULL, error, error_size) != 0)
            goto cleanup;
    }
    input_identities = (HWAExperimentFileIdentity *)calloc(
        manifest.input_count, sizeof(*input_identities));
    if ((manifest.input_count != 0U && input_identities == NULL) ||
        hwa_experiment_inputs_bind(&manifest, bindings, binding_count,
                                   options, input_identities,
                                   error, error_size) != 0 ||
        hwa_experiment_manifest_compact(&manifest) != 0)
        goto cleanup;
    if (hwa_experiment_deadline_check(
            experiment_started, options->max_total_milliseconds,
            error, error_size) != 0)
        goto cleanup;
    (void)hwa_experiment_manifest_transfer(&manifest, result);
    manifest_rows_transferred = 1;
    free(blob.data);
    memset(&blob, 0, sizeof(blob));
    result->manifest_path = hwa_experiment_copy_string(manifest_path);
    result->output_directory = hwa_experiment_copy_string(absolute_output);
    result->resume_directory = resume_directory == NULL ? NULL :
        hwa_experiment_copy_string(resume_directory);
    result->renderer_id = hwa_experiment_copy_string(renderer->id);
    memcpy(result->renderer_sha256, renderer->sha256, HWA_SHA256_HEX_SIZE);
    if (result->manifest_path == NULL || result->output_directory == NULL ||
        (resume_directory != NULL && result->resume_directory == NULL) ||
        result->renderer_id == NULL ||
        hwa_experiment_points_build(&manifest, locale, result,
                                    error, error_size) != 0 ||
        hwa_experiment_jobs_allocate(result, &manifest, &artifact_capacity,
                                     error, error_size) != 0 ||
        hwa_experiment_temp_root(absolute_output, &temporary_root,
                                 error, error_size) != 0 ||
        hwa_experiment_path_join(&jobs_directory, temporary_root, "jobs") != 0 ||
        hwa_experiment_mkdir_new(jobs_directory) != 0)
        goto cleanup;
    run_manifest_identities = (HWAExperimentFileIdentity *)calloc(
        result->job_count, sizeof(*run_manifest_identities));
    run_result_identities = (HWAExperimentFileIdentity *)calloc(
        result->job_count, sizeof(*run_result_identities));
    artifact_identities = (HWAExperimentFileIdentity *)calloc(
        artifact_capacity, sizeof(*artifact_identities));
    if (run_manifest_identities == NULL || run_result_identities == NULL ||
        (artifact_capacity != 0U && artifact_identities == NULL))
        goto cleanup;
    if (hwa_experiment_deadline_check(
            experiment_started, options->max_total_milliseconds,
            error, error_size) != 0)
        goto cleanup;
    if (resume_directory != NULL) {
        char *resume_path = NULL;
        char resume_hash[HWA_SHA256_HEX_SIZE];
        if (resume_directory[0] == '\0' || strcmp(resume_directory, "-") == 0 ||
            hwa_experiment_path_join(&resume_path, resume_directory,
                                     "result.hwa-experiment") != 0 ||
            hwa_experiment_file_read_locale(resume_path, options, &resume,
                                            resume_hash, locale,
                                            error, error_size) != 0 ||
            strcmp(resume.manifest_sha256, result->manifest_sha256) != 0 ||
            strcmp(resume.renderer_id, result->renderer_id) != 0 ||
            strcmp(resume.renderer_sha256, result->renderer_sha256) != 0 ||
            resume.plan_kind != result->plan_kind ||
            resume.plan_seed != result->plan_seed ||
            resume.plan_replicates != result->plan_replicates ||
            resume.point_count != result->point_count ||
            resume.job_count != result->job_count ||
            resume.artifact_count > result->options.max_artifacts ||
            resume.observation_count != result->observation_count) {
            free(resume_path);
            if (error == NULL || error_size == 0U || error[0] == '\0')
                hwa_set_error(error, error_size,
                              "invalid Stage 8 resume bundle");
            goto cleanup;
        }
        free(resume_path);
        resume_loaded = 1;
        {
            uint64_t resume_bytes = 0U;
            if (hwa_experiment_bundle_exact(
                    resume_directory, &resume, resume_hash,
                    NULL, NULL, NULL, &resume_bundle_identity,
                    &resume_bytes, error, error_size) != 0)
                goto cleanup;
        }
        if (hwa_experiment_deadline_check(
                experiment_started, options->max_total_milliseconds,
                error, error_size) != 0)
            goto cleanup;
    }
    for (point = 0U; point < result->point_count; ++point) {
        for (case_index = 0U; case_index < result->case_count; ++case_index) {
            for (replicate = 0U; replicate < result->plan_replicates;
                 ++replicate) {
                size_t job_index =
                    (point * result->case_count + case_index) *
                        result->plan_replicates + replicate;
                size_t artifact_start = result->artifact_count;
                int reused = 0;
                HWAExperimentJob *job = &result->jobs[job_index];
                job->id = (uint64_t)job_index + UINT64_C(1);
                job->point_id = (uint64_t)point + UINT64_C(1);
                job->case_id = (uint64_t)case_index + UINT64_C(1);
                job->replicate = replicate;
                job->seed = hwa_experiment_job_seed(
                    result->plan_seed, point, case_index, replicate);
                if (hwa_experiment_hash_job(
                        result, point, case_index, replicate,
                        job->seed, job->key) != 0) goto cleanup;
                if (resume_loaded)
                    reused = hwa_experiment_resume_job(
                        resume_directory,
                        &manifest.case_templates[case_index], job_index,
                        artifact_start, &resume_artifact_cursor,
                        &resume, jobs_directory, locale,
                        experiment_started, run_manifest_identities,
                        run_result_identities, artifact_identities,
                        &bundle_bytes, result,
                        error, error_size);
                if (reused < 0) goto cleanup;
                if (reused == 0 && hwa_experiment_job_execute(
                        &manifest, point, case_index, replicate,
                        jobs_directory, renderer, locale, experiment_started,
                        run_manifest_identities, run_result_identities,
                        artifact_identities, &bundle_bytes, result,
                        error, error_size) != 0) goto cleanup;
            }
        }
    }
    if (hwa_experiment_verify_inputs(result, input_identities,
                                     error, error_size) != 0 ||
        hwa_experiment_job_relative_paths(result, error, error_size) != 0)
        goto cleanup;
    {
        uint64_t peak_work;
        if (hwa_experiment_result_peak_work_bytes(
                result, 0U, &peak_work) != 0 ||
            peak_work > options->max_work_bytes) {
            hwa_set_error(error, error_size,
                          "Stage 8 peak work cap exceeded");
            goto cleanup;
        }
    }
    if (hwa_experiment_derived_rebuild(result, error, error_size) != 0 ||
        hwa_experiment_result_retained_bytes(result, &retained) != 0 ||
        retained > options->max_work_bytes)
        goto cleanup;
    result->retained_work_bytes = retained;
    if (hwa_experiment_result_validate_locale(
            result, locale, error, error_size) != 0 ||
        hwa_experiment_deadline_check(
            experiment_started, options->max_total_milliseconds,
            error, error_size) != 0 ||
        hwa_experiment_path_join(&bundle_path, temporary_root,
                                 "result.hwa-experiment") != 0 ||
        hwa_experiment_file_write_path_locale(bundle_path, result, locale,
                                              error, error_size) != 0 ||
        hwa_experiment_bundle_exact(
            temporary_root, result, NULL, run_manifest_identities,
            run_result_identities, artifact_identities, &bundle_identity,
            &exact_bundle_bytes, error, error_size) != 0 ||
        hwa_experiment_deadline_check(
            experiment_started, options->max_total_milliseconds,
            error, error_size) != 0 ||
        hwa_experiment_rename_no_replace(temporary_root,
                                         absolute_output) != 0) {
        if (error == NULL || error_size == 0U || error[0] == '\0')
            hwa_set_error(error, error_size,
                          "cannot commit Stage 8 output directory");
        goto cleanup;
    }
    free(temporary_root);
    temporary_root = NULL;
    status = 0;
cleanup:
    hwa_experiment_deadline_leave();
    free(blob.data);
    if (manifest_rows_transferred) {
        manifest.inputs = NULL;
        manifest.parameters = NULL;
        manifest.levels = NULL;
        manifest.cases = NULL;
        manifest.responses = NULL;
    }
    hwa_experiment_manifest_free(&manifest);
    hwa_experiment_result_free(&resume);
    if (temporary_root != NULL) (void)hwa_experiment_remove_tree(temporary_root);
    free(absolute_output);
    free(temporary_root);
    free(jobs_directory);
    free(bundle_path);
    free(input_identities);
    free(run_manifest_identities);
    free(run_result_identities);
    free(artifact_identities);
    (void)bundle_bytes;
    (void)exact_bundle_bytes;
    if (status != 0) hwa_experiment_result_free(result);
    return status;
}

int hwa_execute_experiment_files(
    const char *manifest_path,
    const HWARunBinding *bindings,
    size_t binding_count,
    const char *output_directory,
    const char *resume_directory,
    const HWAExperimentRenderer *renderer,
    const HWAExperimentOptions *options,
    HWAExperimentResult *result,
    char *error,
    size_t error_size)
{
    HWAExperimentOptions copied;
    HWANumericLocale locale;
    int status;
    int locale_status;
    if (result == NULL) {
        hwa_set_error(error, error_size, "missing Stage 8 result");
        return -1;
    }
    if (options == NULL) hwa_experiment_options_default(&copied);
    else copied = *options;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    memset(result, 0, sizeof(*result));
    result->options = copied;
    if (!hwa_experiment_options_valid(&copied) ||
        (binding_count != 0U && bindings == NULL)) {
        hwa_set_error(error, error_size, "invalid Stage 8 options or bindings");
        return -1;
    }
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_set_error(error, error_size,
                      "cannot enter the C numeric locale for Stage 8");
        return -1;
    }
    status = hwa_experiment_execute_impl(
        manifest_path, bindings, binding_count, output_directory,
        resume_directory, renderer, &copied, &locale,
        result, error, error_size);
    locale_status = hwa_c_numeric_locale_end(&locale);
    if (locale_status != 0) {
        if (status == 0) {
            (void)hwa_experiment_output_remove(result, NULL, 0U);
            hwa_experiment_result_free(result);
        }
        hwa_set_error(error, error_size,
                      "cannot restore numeric locale after Stage 8");
        return -1;
    }
    return status;
}

int hwa_experiment_manifest_validate_bytes(const unsigned char *data,
                                           size_t size,
                                           const HWAExperimentOptions *options,
                                           char *error,
                                           size_t error_size)
{
    HWAExperimentOptions copied;
    HWAExperimentManifest manifest;
    HWANumericLocale locale;
    uint64_t live_work = 0U;
    int status = -1;
    int locale_status;
    memset(&manifest, 0, sizeof(manifest));
    if (options == NULL) hwa_experiment_options_default(&copied);
    else copied = *options;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (data == NULL || size > copied.max_manifest_bytes ||
        (uint64_t)size == UINT64_MAX || !hwa_experiment_options_valid(&copied) ||
        hwa_experiment_work_add(&live_work,
                                (uint64_t)size + UINT64_C(1),
                                copied.max_work_bytes,
                                error, error_size) != 0) {
        if (error != NULL && error_size != 0U && error[0] == '\0')
            hwa_set_error(error, error_size,
                          "invalid Stage 8 manifest bytes");
        return -1;
    }
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_set_error(error, error_size,
                      "cannot enter the C numeric locale for Stage 8");
        return -1;
    }
    if (hwa_experiment_manifest_parse(data, size, &copied, &locale,
                                      &live_work, &manifest,
                                      error, error_size) == 0)
        status = 0;
    hwa_experiment_manifest_free(&manifest);
    locale_status = hwa_c_numeric_locale_end(&locale);
    if (locale_status != 0) {
        hwa_set_error(error, error_size,
                      "cannot restore numeric locale after Stage 8");
        return -1;
    }
    return status;
}

int hwa_experiment_output_remove(const HWAExperimentResult *result,
                                 char *error,
                                 size_t error_size)
{
    char expected[HWA_SHA256_HEX_SIZE];
    uint64_t exact_bytes = 0U;
    int status = -1;
    if (result == NULL || result->output_directory == NULL ||
        result->output_directory[0] == '\0') goto failed;
    /* Re-render the canonical file to a private stream so removal is identity-bound. */
    {
        FILE *stream = tmpfile();
        HWASha256 hash;
        unsigned char digest[32];
        unsigned char buffer[65536];
        size_t count;
        if (stream == NULL ||
            hwa_experiment_file_write(stream, result, error, error_size) != 0 ||
            fflush(stream) != 0 || fseek(stream, 0L, SEEK_SET) != 0) {
            if (stream != NULL) fclose(stream);
            goto failed;
        }
        hwa_sha256_init(&hash);
        while ((count = fread(buffer, 1U, sizeof(buffer), stream)) != 0U)
            hwa_sha256_update(&hash, buffer, count);
        if (ferror(stream) || fclose(stream) != 0) goto failed;
        hwa_sha256_final(&hash, digest);
        hwa_sha256_hex(digest, expected);
    }
    if (hwa_experiment_bundle_exact(
            result->output_directory, result, expected,
            NULL, NULL, NULL, NULL, &exact_bytes,
            error, error_size) != 0) {
        hwa_set_error(error, error_size,
                      "Stage 8 output changed; refusing removal");
        goto failed;
    }
    status = hwa_experiment_remove_tree(result->output_directory);
    if (status != 0)
        hwa_set_error(error, error_size, "cannot remove Stage 8 output");
failed:
    (void)exact_bytes;
    return status;
}

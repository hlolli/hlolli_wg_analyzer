#include "physical_report.h"
#include "physical_file.h"

#include <locale.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TestJson {
    const unsigned char *cursor;
    const unsigned char *end;
} TestJson;

static int failures;

#define CHECK(condition, ...)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "FAIL: ");                                \
            (void)fprintf(stderr, __VA_ARGS__);                              \
            (void)fputc('\n', stderr);                                      \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static int test_set_comma_numeric_locale(void)
{
    static const char *const candidates[] = {
        "de_DE.UTF-8", "fr_FR.UTF-8", "de_DE",
        "German_Germany.1252", "de-DE"
    };
    size_t index;
    for (index = 0U;
         index < sizeof(candidates) / sizeof(candidates[0]); ++index) {
        if (setlocale(LC_NUMERIC, candidates[index]) != NULL &&
            strcmp(localeconv()->decimal_point, ",") == 0) return 1;
    }
    return 0;
}

static char *test_copy(const char *text)
{
    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1U);
    if (copy != NULL) memcpy(copy, text, length + 1U);
    return copy;
}

static void test_hash(char target[HWA_SHA256_HEX_SIZE], char byte)
{
    size_t index;
    for (index = 0U; index < HWA_SHA256_HEX_SIZE - 1U; ++index) {
        target[index] = byte;
    }
    target[HWA_SHA256_HEX_SIZE - 1U] = '\0';
}

static int test_scored_finding(HWAPhysicalFinding *finding,
                               uint64_t id,
                               size_t rank,
                               const HWAPhysicalCheck *check)
{
    const char *code;
    const char *message;
    memset(finding, 0, sizeof(*finding));
    if (hwa_physical_scored_finding_for_check(
            check, &finding->finding_class, &finding->severity,
            &code, &message, &finding->score) != 1) return 0;
    finding->id = id;
    finding->rank = rank;
    finding->check_id = check->id;
    finding->check_id_valid = 1;
    finding->score_valid = 1;
    finding->code = test_copy(code);
    finding->message = test_copy(message);
    return finding->code != NULL && finding->message != NULL;
}

static HWAPhysicalUnit test_kind_unit(HWAPhysicalCheckKind kind)
{
    switch (kind) {
    case HWA_PHYSICAL_ELEMENT_CARRYOVER_DB:
    case HWA_PHYSICAL_BODY_MODE_PROMINENCE_DB:
    case HWA_PHYSICAL_JOINT_RESIDUAL_DB:
    case HWA_PHYSICAL_SHARED_GAIN_DB:
    case HWA_PHYSICAL_SUM_TONE_DB:
    case HWA_PHYSICAL_DIFFERENCE_TONE_DB:
    case HWA_PHYSICAL_RENDER_RMS_ERROR_DB:
    case HWA_PHYSICAL_RENDER_SPECTRAL_DISTANCE_DB:
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
        return HWA_PHYSICAL_UNIT_DBFS;
    case HWA_PHYSICAL_RENDER_LAG_SAMPLES:
        return HWA_PHYSICAL_UNIT_SAMPLES;
    default:
        return HWA_PHYSICAL_UNIT_RATIO;
    }
}

static int test_check(HWAPhysicalCheck *check,
                      uint64_t id,
                      const char *scope,
                      HWAPhysicalCheckKind kind,
                      HWAPhysicalAvailability availability)
{
    memset(check, 0, sizeof(*check));
    check->id = id;
    check->scope = test_copy(scope);
    check->case_id = test_copy("");
    check->element = test_copy("");
    check->kind = kind;
    check->unit = test_kind_unit(kind);
    check->availability = availability;
    return check->scope != NULL && check->case_id != NULL &&
           check->element != NULL;
}

static int test_missing_finding(HWAPhysicalFinding *finding,
                                uint64_t id,
                                const HWAPhysicalCheck *check)
{
    memset(finding, 0, sizeof(*finding));
    finding->id = id;
    finding->finding_class = HWA_PHYSICAL_FINDING_UNAVAILABLE;
    finding->severity = HWA_PHYSICAL_SEVERITY_INFO;
    finding->code = test_copy("missing-physical-evidence");
    finding->message = test_copy(
        "This physical check family has no usable bound evidence.");
    finding->check_id = check->id;
    finding->check_id_valid = 1;
    return finding->code != NULL && finding->message != NULL;
}

static int test_make_set(HWAPhysicalCheckSet *set)
{
    size_t index;
    int kind;
    memset(set, 0, sizeof(*set));
    hwa_physical_options_default(&set->options);
    set->reference_measures_path = test_copy("/reference-\xff\n.hwa");
    set->model_measures_path = test_copy("/model.hwa");
    test_hash(set->reference_measures_sha256, 'a');
    test_hash(set->model_measures_sha256, 'b');
    set->retained_work_bytes = 0U;
    set->pair_evaluations = 8U;
    set->transform_count = 2U;
    set->source_count = 2U;
    set->check_count = 28U;
    set->finding_count = 4U;
    set->warning_count = 1U;
    set->sources = (HWAPhysicalSource *)calloc(2U, sizeof(*set->sources));
    set->checks = (HWAPhysicalCheck *)calloc(
        set->check_count, sizeof(*set->checks));
    set->findings = (HWAPhysicalFinding *)calloc(
        set->finding_count, sizeof(*set->findings));
    set->warnings = (HWAPhysicalWarning *)calloc(
        1U, sizeof(*set->warnings));
    if (set->reference_measures_path == NULL ||
        set->model_measures_path == NULL || set->sources == NULL ||
        set->checks == NULL || set->findings == NULL ||
        set->warnings == NULL) return 0;
    set->sources[0].id = 1U;
    set->sources[0].role = test_copy("reference:profile");
    set->sources[0].path = test_copy(set->reference_measures_path);
    test_hash(set->sources[0].sha256, 'a');
    set->sources[1].id = 2U;
    set->sources[1].role = test_copy("model:profile");
    set->sources[1].path = test_copy(set->model_measures_path);
    test_hash(set->sources[1].sha256, 'b');
    if (!test_check(&set->checks[0], 1U, "profiles",
                    HWA_PHYSICAL_ELEMENT_TRAIT_DELTA,
                    HWA_PHYSICAL_INSUFFICIENT) ||
        !test_check(&set->checks[1], 2U, "profiles",
                    HWA_PHYSICAL_ELEMENT_CARRYOVER_DB,
                    HWA_PHYSICAL_UNAVAILABLE) ||
        !test_check(&set->checks[2], 3U, "profiles",
                    HWA_PHYSICAL_ELEMENT_DISTINCTNESS_RATIO,
                    HWA_PHYSICAL_AVAILABLE)) return 0;
    free(set->checks[2].element);
    set->checks[2].element = test_copy("element\none");
    set->checks[2].reference_value = 0.2;
    set->checks[2].model_value = 0.4;
    set->checks[2].delta = set->checks[2].model_value -
                           set->checks[2].reference_value;
    set->checks[2].confidence = 0.75;
    set->checks[2].reference_valid = 1;
    set->checks[2].model_valid = 1;
    set->checks[2].delta_valid = 1;
    set->checks[2].evidence_flags =
        HWA_PHYSICAL_EVIDENCE_REFERENCE_PROFILE |
        HWA_PHYSICAL_EVIDENCE_MODEL_PROFILE;
    index = 3U;
    for (kind = (int)HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ;
         kind <= (int)HWA_PHYSICAL_BODY_MODE_DISTANCE_CENTS; ++kind) {
        if (!test_check(&set->checks[index], (uint64_t)index + 1U, "body",
                        (HWAPhysicalCheckKind)kind,
                        HWA_PHYSICAL_UNAVAILABLE)) return 0;
        index++;
    }
    for (kind = (int)HWA_PHYSICAL_JOINT_RESIDUAL_DB;
         kind <= (int)HWA_PHYSICAL_PITCH_PULL_CENTS; ++kind) {
        if (!test_check(&set->checks[index], (uint64_t)index + 1U, "joint",
                        (HWAPhysicalCheckKind)kind,
                        HWA_PHYSICAL_UNAVAILABLE)) return 0;
        index++;
    }
    for (kind = (int)HWA_PHYSICAL_RENDER_RMS_ERROR_DB;
         kind <= (int)HWA_PHYSICAL_RENDER_SPECTRAL_DISTANCE_DB; ++kind) {
        if (!test_check(&set->checks[index], (uint64_t)index + 1U, "render",
                        (HWAPhysicalCheckKind)kind,
                        HWA_PHYSICAL_UNAVAILABLE)) return 0;
        index++;
    }
    if (index != set->check_count || set->checks[2].element == NULL ||
        !test_scored_finding(
            &set->findings[0], 1U, 1U, &set->checks[2]) ||
        !test_missing_finding(&set->findings[1], 2U, &set->checks[3]) ||
        !test_missing_finding(&set->findings[2], 3U, &set->checks[11]) ||
        !test_missing_finding(&set->findings[3], 4U, &set->checks[20])) {
        return 0;
    }
    set->warnings[0].id = 1U;
    set->warnings[0].code = test_copy("missing-body");
    set->warnings[0].message = test_copy("body evidence missing");
    set->warnings[0].check_id = 21U;
    set->warnings[0].check_id_valid = 1;
    if (!(set->sources[0].role != NULL && set->sources[0].path != NULL &&
           set->sources[1].role != NULL && set->sources[1].path != NULL &&
           set->checks[0].scope != NULL && set->checks[2].element != NULL &&
           set->findings[0].code != NULL &&
           set->findings[0].message != NULL &&
           set->findings[1].code != NULL &&
           set->findings[1].message != NULL &&
           set->findings[2].code != NULL &&
           set->findings[2].message != NULL &&
           set->findings[3].code != NULL &&
           set->findings[3].message != NULL &&
           set->warnings[0].code != NULL &&
           set->warnings[0].message != NULL)) return 0;
    return hwa_physical_check_set_retained_bytes(
               set, &set->retained_work_bytes) == 0;
}

static void test_json_space(TestJson *json)
{
    while (json->cursor != json->end &&
           (*json->cursor == (unsigned char)' ' ||
            *json->cursor == (unsigned char)'\t' ||
            *json->cursor == (unsigned char)'\r' ||
            *json->cursor == (unsigned char)'\n')) json->cursor++;
}

static int test_json_hex(unsigned char value)
{
    return (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           (value >= (unsigned char)'a' && value <= (unsigned char)'f') ||
           (value >= (unsigned char)'A' && value <= (unsigned char)'F');
}

static int test_json_string(TestJson *json)
{
    if (json->cursor == json->end ||
        *json->cursor != (unsigned char)'"') return 0;
    json->cursor++;
    while (json->cursor != json->end) {
        unsigned char value = *json->cursor++;
        if (value == (unsigned char)'"') return 1;
        if (value < 0x20U) return 0;
        if (value == (unsigned char)'\\') {
            size_t index;
            if (json->cursor == json->end) return 0;
            value = *json->cursor++;
            if (strchr("\"\\/bfnrt", (int)value) != NULL) continue;
            if (value != (unsigned char)'u' ||
                (size_t)(json->end - json->cursor) < 4U) return 0;
            for (index = 0U; index < 4U; ++index) {
                if (!test_json_hex(json->cursor[index])) return 0;
            }
            json->cursor += 4U;
        }
    }
    return 0;
}

static int test_json_number(TestJson *json)
{
    const unsigned char *start = json->cursor;
    if (json->cursor != json->end &&
        *json->cursor == (unsigned char)'-') json->cursor++;
    if (json->cursor == json->end) return 0;
    if (*json->cursor == (unsigned char)'0') {
        json->cursor++;
    } else {
        if (*json->cursor < (unsigned char)'1' ||
            *json->cursor > (unsigned char)'9') return 0;
        while (json->cursor != json->end &&
               *json->cursor >= (unsigned char)'0' &&
               *json->cursor <= (unsigned char)'9') json->cursor++;
    }
    if (json->cursor != json->end &&
        *json->cursor == (unsigned char)'.') {
        json->cursor++;
        if (json->cursor == json->end ||
            *json->cursor < (unsigned char)'0' ||
            *json->cursor > (unsigned char)'9') return 0;
        while (json->cursor != json->end &&
               *json->cursor >= (unsigned char)'0' &&
               *json->cursor <= (unsigned char)'9') json->cursor++;
    }
    if (json->cursor != json->end &&
        (*json->cursor == (unsigned char)'e' ||
         *json->cursor == (unsigned char)'E')) {
        json->cursor++;
        if (json->cursor != json->end &&
            (*json->cursor == (unsigned char)'+' ||
             *json->cursor == (unsigned char)'-')) json->cursor++;
        if (json->cursor == json->end ||
            *json->cursor < (unsigned char)'0' ||
            *json->cursor > (unsigned char)'9') return 0;
        while (json->cursor != json->end &&
               *json->cursor >= (unsigned char)'0' &&
               *json->cursor <= (unsigned char)'9') json->cursor++;
    }
    return json->cursor != start;
}

static int test_json_value(TestJson *json, unsigned depth);

static int test_json_array(TestJson *json, unsigned depth)
{
    json->cursor++;
    test_json_space(json);
    if (json->cursor != json->end &&
        *json->cursor == (unsigned char)']') {
        json->cursor++;
        return 1;
    }
    for (;;) {
        if (!test_json_value(json, depth + 1U)) return 0;
        test_json_space(json);
        if (json->cursor == json->end) return 0;
        if (*json->cursor == (unsigned char)']') {
            json->cursor++;
            return 1;
        }
        if (*json->cursor++ != (unsigned char)',') return 0;
        test_json_space(json);
    }
}

static int test_json_object(TestJson *json, unsigned depth)
{
    json->cursor++;
    test_json_space(json);
    if (json->cursor != json->end &&
        *json->cursor == (unsigned char)'}') {
        json->cursor++;
        return 1;
    }
    for (;;) {
        if (!test_json_string(json)) return 0;
        test_json_space(json);
        if (json->cursor == json->end ||
            *json->cursor++ != (unsigned char)':') return 0;
        if (!test_json_value(json, depth + 1U)) return 0;
        test_json_space(json);
        if (json->cursor == json->end) return 0;
        if (*json->cursor == (unsigned char)'}') {
            json->cursor++;
            return 1;
        }
        if (*json->cursor++ != (unsigned char)',') return 0;
        test_json_space(json);
    }
}

static int test_json_literal(TestJson *json, const char *text)
{
    size_t length = strlen(text);
    if ((size_t)(json->end - json->cursor) < length ||
        memcmp(json->cursor, text, length) != 0) return 0;
    json->cursor += length;
    return 1;
}

static int test_json_value(TestJson *json, unsigned depth)
{
    test_json_space(json);
    if (depth > 64U || json->cursor == json->end) return 0;
    if (*json->cursor == (unsigned char)'{') {
        return test_json_object(json, depth);
    }
    if (*json->cursor == (unsigned char)'[') {
        return test_json_array(json, depth);
    }
    if (*json->cursor == (unsigned char)'"') return test_json_string(json);
    if (*json->cursor == (unsigned char)'t') {
        return test_json_literal(json, "true");
    }
    if (*json->cursor == (unsigned char)'f') {
        return test_json_literal(json, "false");
    }
    if (*json->cursor == (unsigned char)'n') {
        return test_json_literal(json, "null");
    }
    return test_json_number(json);
}

static int test_json_document(const char *text, size_t size)
{
    TestJson json;
    json.cursor = (const unsigned char *)text;
    json.end = json.cursor + size;
    return test_json_value(&json, 0U) &&
           (test_json_space(&json), json.cursor == json.end);
}

static char *test_capture(
    int (*writer)(FILE *, const HWAPhysicalCheckSet *),
    const HWAPhysicalCheckSet *set,
    size_t *size)
{
    FILE *stream = tmpfile();
    long length;
    char *text;
    if (stream == NULL || writer(stream, set) != 0 ||
        fflush(stream) != 0 || fseek(stream, 0L, SEEK_END) != 0 ||
        (length = ftell(stream)) < 0 || fseek(stream, 0L, SEEK_SET) != 0) {
        if (stream != NULL) (void)fclose(stream);
        return NULL;
    }
    text = (char *)malloc((size_t)length + 1U);
    if (text == NULL ||
        fread(text, 1U, (size_t)length, stream) != (size_t)length ||
        fclose(stream) != 0) {
        free(text);
        return NULL;
    }
    text[length] = '\0';
    *size = (size_t)length;
    return text;
}

static size_t test_occurrences(const char *text, const char *needle)
{
    size_t count = 0U;
    size_t length = strlen(needle);
    const char *cursor = text;
    while (cursor != NULL && (cursor = strstr(cursor, needle)) != NULL) {
        count++;
        cursor += length;
    }
    return count;
}

static void test_reports(void)
{
    HWAPhysicalCheckSet set;
    char *json;
    char *text;
    size_t json_size;
    size_t text_size;
    const char *current = setlocale(LC_NUMERIC, NULL);
    char saved[128];
    int comma_locale = 0;
    if (current != NULL && strlen(current) < sizeof(saved)) {
        memcpy(saved, current, strlen(current) + 1U);
        comma_locale = test_set_comma_numeric_locale();
    } else {
        saved[0] = '\0';
    }
    if (!test_make_set(&set)) {
        CHECK(0, "report fixture failed");
        if (saved[0] != '\0') (void)setlocale(LC_NUMERIC, saved);
        return;
    }
    json = test_capture(hwa_report_physical_json, &set, &json_size);
    text = test_capture(hwa_report_physical_text, &set, &text_size);
    CHECK(json != NULL && test_json_document(json, json_size),
          "schema 7 output is not one valid JSON document");
    if (comma_locale) {
        CHECK(strcmp(localeconv()->decimal_point, ",") == 0,
              "physical reports changed the caller locale");
    }
    CHECK(json != NULL &&
              strstr(json, "\"schema_version\":7") != NULL &&
              strstr(json, "\"command\":\"check-physical\"") != NULL &&
              strstr(json, "\"checks\":[") != NULL &&
              strstr(json, "\"findings\":[") != NULL &&
              strstr(json, "\"warnings\":[") != NULL,
          "schema 7 output lost full result sections");
    CHECK(json != NULL && strstr(json, "\"reference_value\":null") != NULL &&
              strstr(json, "\"model_value\":null") != NULL,
          "invalid values were not JSON null");
    CHECK(json != NULL && strstr(json, "\\u00ff\\n") != NULL &&
              strstr(json, "2f7265666572656e63652dff0a2e687761") != NULL,
          "byte-safe JSON path fields are missing");
    CHECK(json != NULL && strstr(json, ":nan") == NULL &&
              strstr(json, ":inf") == NULL &&
              strstr(json, ":-inf") == NULL &&
              strstr(json, "NaN") == NULL &&
              strstr(json, "Infinity") == NULL,
          "nonfinite spelling leaked into JSON");
    CHECK(text != NULL && text_size != 0U &&
              strstr(text, "Checks: 28 (available 1, unavailable 26, "
                           "insufficient 1)") != NULL &&
              strstr(text, "Top findings:") != NULL &&
              strstr(text, "\\xff\\x0a") != NULL &&
              strstr(text, "A physical check exceeds its fixed review "
                           "threshold.") != NULL &&
              strstr(text, "check #3 kind=element_distinctness_ratio "
                           "index=0") != NULL &&
              strstr(text, "scope=profiles case= element=element\\x0aone") != NULL &&
              strstr(text, "reference=0.20000000000000001 "
                           "model=0.40000000000000002 "
                           "delta=0.20000000000000001") != NULL &&
              strstr(text, "missing-physical-evidence") != NULL &&
              strstr(text, "(score n/a)") != NULL &&
              strstr(text, "check #21 kind=render_rms_error_db index=0") != NULL &&
              strstr(text, "reference=n/a model=n/a delta=n/a") != NULL,
          "short text report lost counts or byte escaping");
    CHECK(text != NULL &&
              test_occurrences(text, "/reference-\\xff\\x0a.hwa") == 1U,
          "text report did not print the reference path exactly once");
    free(json);
    free(text);
    hwa_physical_check_set_free(&set);
    if (saved[0] != '\0') {
        CHECK(setlocale(LC_NUMERIC, saved) != NULL,
              "cannot restore numeric locale after report test");
    }
}

static void test_preflight_and_fault(void)
{
    HWAPhysicalCheckSet set;
    FILE *stream;
    if (!test_make_set(&set)) {
        CHECK(0, "fault fixture failed");
        return;
    }
    stream = tmpfile();
    CHECK(stream != NULL, "fault tmpfile failed");
    if (stream != NULL) {
        set.checks[0].unit = HWA_PHYSICAL_UNIT_DB;
        CHECK(hwa_report_physical_json(stream, &set) != 0,
              "invalid unit passed JSON report");
        CHECK(ftell(stream) == 0L,
              "report wrote bytes before validating the result");
        set.checks[0].unit = HWA_PHYSICAL_UNIT_RATIO;
        (void)fclose(stream);
    }
    stream = fopen("/dev/null", "rb");
    if (stream != NULL) {
        CHECK(hwa_report_physical_text(stream, &set) != 0,
              "text write fault was not reported");
        CHECK(hwa_report_physical_json(stream, &set) != 0,
              "JSON write fault was not reported");
        (void)fclose(stream);
    }
    hwa_physical_check_set_free(&set);
}

int main(void)
{
    test_reports();
    test_preflight_and_fault();
    if (failures != 0) {
        (void)fprintf(stderr, "%d Stage 5 report test(s) failed\n", failures);
        return 1;
    }
    (void)puts("Stage 5 report tests passed");
    return 0;
}

#include "measure_report.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_OUTPUT_CAPACITY 262144U

typedef struct TestJson {
    const unsigned char *cursor;
    const unsigned char *end;
} TestJson;

typedef int (*TestMeasurementWriter)(FILE *, const HWAMeasurementSet *);
typedef int (*TestComparisonWriter)(FILE *, const HWAProfileComparisonSet *);

static int failures;

#define CHECK(condition, ...)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            (void)fprintf(stderr, __VA_ARGS__);                              \
            (void)fputc('\n', stderr);                                       \
            failures++;                                                      \
        }                                                                    \
    } while (0)

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
    if (json->cursor == json->end || *json->cursor != (unsigned char)'"') {
        return 0;
    }
    json->cursor++;
    while (json->cursor != json->end) {
        unsigned char value = *json->cursor++;
        if (value == (unsigned char)'"') return 1;
        if (value < 0x20U) return 0;
        if (value == (unsigned char)'\\') {
            size_t index;
            if (json->cursor == json->end) return 0;
            value = *json->cursor++;
            if (value == (unsigned char)'"' ||
                value == (unsigned char)'\\' ||
                value == (unsigned char)'/' ||
                value == (unsigned char)'b' ||
                value == (unsigned char)'f' ||
                value == (unsigned char)'n' ||
                value == (unsigned char)'r' ||
                value == (unsigned char)'t') continue;
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
        do {
            json->cursor++;
        } while (json->cursor != json->end &&
                 *json->cursor >= (unsigned char)'0' &&
                 *json->cursor <= (unsigned char)'9');
    }
    if (json->cursor != json->end &&
        *json->cursor == (unsigned char)'.') {
        json->cursor++;
        if (json->cursor == json->end ||
            *json->cursor < (unsigned char)'0' ||
            *json->cursor > (unsigned char)'9') return 0;
        do {
            json->cursor++;
        } while (json->cursor != json->end &&
                 *json->cursor >= (unsigned char)'0' &&
                 *json->cursor <= (unsigned char)'9');
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
        do {
            json->cursor++;
        } while (json->cursor != json->end &&
                 *json->cursor >= (unsigned char)'0' &&
                 *json->cursor <= (unsigned char)'9');
    }
    return json->cursor != start;
}

static int test_json_value(TestJson *json, unsigned depth);

static int test_json_array(TestJson *json, unsigned depth)
{
    if (json->cursor == json->end || *json->cursor != (unsigned char)'[') {
        return 0;
    }
    json->cursor++;
    test_json_space(json);
    if (json->cursor != json->end && *json->cursor == (unsigned char)']') {
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
        if (*json->cursor != (unsigned char)',') return 0;
        json->cursor++;
        test_json_space(json);
    }
}

static int test_json_object(TestJson *json, unsigned depth)
{
    if (json->cursor == json->end || *json->cursor != (unsigned char)'{') {
        return 0;
    }
    json->cursor++;
    test_json_space(json);
    if (json->cursor != json->end && *json->cursor == (unsigned char)'}') {
        json->cursor++;
        return 1;
    }
    for (;;) {
        if (!test_json_string(json)) return 0;
        test_json_space(json);
        if (json->cursor == json->end ||
            *json->cursor != (unsigned char)':') return 0;
        json->cursor++;
        if (!test_json_value(json, depth + 1U)) return 0;
        test_json_space(json);
        if (json->cursor == json->end) return 0;
        if (*json->cursor == (unsigned char)'}') {
            json->cursor++;
            return 1;
        }
        if (*json->cursor != (unsigned char)',') return 0;
        json->cursor++;
        test_json_space(json);
    }
}

static int test_json_literal(TestJson *json, const char *literal)
{
    size_t length = strlen(literal);
    if ((size_t)(json->end - json->cursor) < length ||
        memcmp(json->cursor, literal, length) != 0) return 0;
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

static int test_capture_measurement(TestMeasurementWriter writer,
                                    const HWAMeasurementSet *set,
                                    char output[TEST_OUTPUT_CAPACITY],
                                    size_t *size)
{
    FILE *stream = tmpfile();
    long length;
    int okay = stream != NULL;
    *size = 0U;
    if (!okay) return 0;
    if (writer(stream, set) != 0 || fflush(stream) != 0 ||
        fseek(stream, 0L, SEEK_END) != 0) okay = 0;
    length = okay ? ftell(stream) : -1L;
    if (length < 0L || (uint64_t)length >= TEST_OUTPUT_CAPACITY ||
        fseek(stream, 0L, SEEK_SET) != 0) okay = 0;
    if (okay && fread(output, 1U, (size_t)length, stream) !=
                      (size_t)length) okay = 0;
    if (fclose(stream) != 0) okay = 0;
    if (!okay) return 0;
    output[(size_t)length] = '\0';
    *size = (size_t)length;
    return 1;
}

static int test_capture_comparison(TestComparisonWriter writer,
                                   const HWAProfileComparisonSet *set,
                                   char output[TEST_OUTPUT_CAPACITY],
                                   size_t *size)
{
    FILE *stream = tmpfile();
    long length;
    int okay = stream != NULL;
    *size = 0U;
    if (!okay) return 0;
    if (writer(stream, set) != 0 || fflush(stream) != 0 ||
        fseek(stream, 0L, SEEK_END) != 0) okay = 0;
    length = okay ? ftell(stream) : -1L;
    if (length < 0L || (uint64_t)length >= TEST_OUTPUT_CAPACITY ||
        fseek(stream, 0L, SEEK_SET) != 0) okay = 0;
    if (okay && fread(output, 1U, (size_t)length, stream) !=
                      (size_t)length) okay = 0;
    if (fclose(stream) != 0) okay = 0;
    if (!okay) return 0;
    output[(size_t)length] = '\0';
    *size = (size_t)length;
    return 1;
}

static void test_hash(char target[HWA_SHA256_HEX_SIZE], char byte)
{
    size_t index;
    for (index = 0U; index < HWA_SHA256_HEX_SIZE - 1U; ++index) {
        target[index] = byte;
    }
    target[HWA_SHA256_HEX_SIZE - 1U] = '\0';
}

static void test_measurement_json(void)
{
    char unsafe_path[] = "i\xffm";
    HWAMeasureItemContext context;
    HWAMeasureObservation observation;
    HWAMeasureGroup group;
    HWAMeasureGroupMember member;
    HWAMeasureStatistic statistic;
    HWAMeasureWarning warning;
    HWAMeasurementSet set;
    char output[TEST_OUTPUT_CAPACITY];
    size_t size;
    memset(&set, 0, sizeof(set));
    memset(&context, 0, sizeof(context));
    memset(&observation, 0, sizeof(observation));
    memset(&group, 0, sizeof(group));
    memset(&member, 0, sizeof(member));
    memset(&statistic, 0, sizeof(statistic));
    memset(&warning, 0, sizeof(warning));
    hwa_measurement_options_default(&set.options);
    set.items_path = unsafe_path;
    set.audio_path = (char *)"audio.wav";
    set.alignment_path = (char *)"alignment.hwa-align";
    set.amendment_path = (char *)"amend.hwa-items";
    set.source_score_path = (char *)"score.csv";
    test_hash(set.items_sha256, 'a');
    test_hash(set.audio_sha256, 'b');
    test_hash(set.alignment_sha256, 'c');
    test_hash(set.amendment_sha256, 'd');
    test_hash(set.source_score_sha256, 'e');
    set.audio_format.container = HWA_CONTAINER_RIFF;
    set.audio_format.encoding = HWA_ENCODING_PCM;
    set.audio_format.channels = 1U;
    set.audio_format.sample_rate_hz = 48000U;
    set.audio_format.bits_per_sample = 24U;
    set.audio_format.valid_bits_per_sample = 24U;
    set.audio_format.block_align = 3U;
    set.audio_format.frames = 48000U;
    set.audio_format.data_bytes = 144000U;
    set.audio_format.duration_seconds = 1.0;
    set.level_reference_dbfs = -18.0;
    set.level_reference_item_count = 1U;
    set.level_reference_valid = 1;
    set.capability_flags = 3U;
    set.item_frame_evaluations = 17U;
    set.transform_count = 4U;
    set.retained_work_bytes = 12345U;
    context.item_id = 1U;
    context.item_key = (char *)"body:1";
    context.item_kind = HWA_ITEM_BODY;
    context.item_role = (char *)"body";
    context.end_sample = 48000U;
    context.labels.pitch = (char *)"A4";
    context.labels.override_flags = HWA_LABEL_OVERRIDE_PITCH;
    context.source_event_count = 1U;
    context.item_confidence = 0.75;
    set.contexts = &context;
    set.context_count = 1U;
    observation.id = 1U;
    observation.item_id = 1U;
    observation.kind = HWA_MEASURE_RMS_DBFS;
    observation.unit = HWA_MEASURE_UNIT_DBFS;
    observation.view = HWA_MEASURE_VIEW_RAW;
    observation.status = HWA_MEASURE_STATUS_VALID;
    observation.value = -18.0;
    observation.confidence = 0.75;
    set.measurements = &observation;
    set.measurement_count = 1U;
    group.id = 1U;
    group.key = (char *)"g/body/626f6479/all/";
    group.item_kind = HWA_ITEM_BODY;
    group.item_role = (char *)"body";
    group.selector = HWA_MEASURE_GROUP_ALL;
    group.value = (char *)"";
    group.member_count = 1U;
    set.groups = &group;
    set.group_count = 1U;
    member.group_id = 1U;
    member.item_id = 1U;
    set.group_members = &member;
    set.group_member_count = 1U;
    statistic.id = 1U;
    statistic.group_id = 1U;
    statistic.kind = observation.kind;
    statistic.unit = observation.unit;
    statistic.view = observation.view;
    statistic.statistics.total_count = 1U;
    statistic.statistics.valid_count = 1U;
    statistic.statistics.minimum = -18.0;
    statistic.statistics.q05 = -18.0;
    statistic.statistics.q25 = -18.0;
    statistic.statistics.q50 = -18.0;
    statistic.statistics.q75 = -18.0;
    statistic.statistics.q95 = -18.0;
    statistic.statistics.maximum = -18.0;
    statistic.statistics.mean = -18.0;
    statistic.statistics.confidence = 0.75;
    statistic.statistics.valid = 1;
    set.statistics = &statistic;
    set.statistic_count = 1U;
    warning.id = 1U;
    warning.code = (char *)"linked";
    warning.message = (char *)"linked warning";
    warning.item_id = 1U;
    warning.observation_id = 1U;
    warning.item_id_valid = 1;
    warning.observation_id_valid = 1;
    set.warnings = &warning;
    set.warning_count = 1U;
    CHECK(test_capture_measurement(hwa_report_measurement_json, &set,
                                   output, &size),
          "cannot capture measurement JSON");
    CHECK(test_json_document(output, size), "measurement JSON is invalid");
    CHECK(strstr(output, "\"schema_version\":5") != NULL &&
              strstr(output, "\"options\":{") != NULL &&
              strstr(output, "\"contexts\":[{") != NULL &&
              strstr(output, "\"measurements\":[{") != NULL &&
              strstr(output, "\"groups\":[{") != NULL &&
              strstr(output, "\"group_members\":[{") != NULL &&
              strstr(output, "\"statistics\":[{") != NULL,
          "measurement JSON omits full profile data");
    CHECK(strstr(output, "\"warnings\":[{\"id\":1") != NULL &&
              strstr(output, "\"item_id\":1,\"observation_id\":1") != NULL,
          "measurement JSON omits linked warnings");
    CHECK(strstr(output, "\"level_reference_dbfs\":-18") != NULL &&
              strstr(output, "\"level_reference_valid\":true") != NULL &&
              strstr(output, "\"level_reference_item_count\":1") != NULL &&
              strstr(output, "\"capability_flags\":3") != NULL &&
              strstr(output, "\"transform_count\":4") != NULL &&
              strstr(output, "\"retained_work_bytes\":12345") != NULL,
          "measurement summary omits saved facts");
    CHECK(strstr(output, "\"path_bytes_hex\":\"69ff6d\"") != NULL &&
              strstr(output, "\\u00ff") != NULL,
          "byte-unsafe path did not keep text and bytes");
    set.level_reference_valid = 0;
    set.level_reference_item_count = 0U;
    CHECK(test_capture_measurement(hwa_report_measurement_json, &set,
                                   output, &size) &&
              test_json_document(output, size) &&
              strstr(output, "\"level_reference_dbfs\":null") != NULL &&
              strstr(output, "\"level_reference_valid\":false") != NULL,
          "missing level reference is not a JSON null");
    set.items_path = NULL;
    {
        FILE *invalid_stream = tmpfile();
        CHECK(invalid_stream != NULL &&
                  hwa_report_measurement_json(invalid_stream, &set) != 0,
              "measurement JSON accepted a null required path");
        if (invalid_stream != NULL) (void)fclose(invalid_stream);
    }
}

static void test_comparison_json(void)
{
    HWAProfileComparisonSet set;
    HWAMeasureGroup group;
    HWAProfileDistribution distribution;
    HWAProfileGap gap;
    HWAProfileWarning warning;
    char output[TEST_OUTPUT_CAPACITY];
    size_t size;
    memset(&set, 0, sizeof(set));
    memset(&group, 0, sizeof(group));
    memset(&distribution, 0, sizeof(distribution));
    memset(&gap, 0, sizeof(gap));
    memset(&warning, 0, sizeof(warning));
    hwa_profile_comparison_options_default(&set.options);
    set.reference_path = (char *)"reference.hwa-measures";
    set.model_path = (char *)"model.hwa-measures";
    test_hash(set.reference_sha256, 'a');
    test_hash(set.model_sha256, 'b');
    set.retained_work_bytes = 9876U;
    group.id = 1U;
    group.key = (char *)"g/body/626f6479/all/";
    group.item_kind = HWA_ITEM_BODY;
    group.item_role = (char *)"body";
    group.selector = HWA_MEASURE_GROUP_ALL;
    group.value = (char *)"";
    group.member_count = 2U;
    set.groups = &group;
    set.group_count = 1U;
    distribution.id = 1U;
    distribution.group_id = 1U;
    distribution.kind = HWA_MEASURE_RMS_DBFS;
    distribution.unit = HWA_MEASURE_UNIT_DBFS;
    distribution.view = HWA_MEASURE_VIEW_RAW;
    distribution.reference_statistics.total_count = 2U;
    distribution.reference_statistics.valid_count = 2U;
    distribution.reference_statistics.minimum = -20.0;
    distribution.reference_statistics.q05 = -19.5;
    distribution.reference_statistics.q25 = -17.5;
    distribution.reference_statistics.q50 = -15.0;
    distribution.reference_statistics.q75 = -12.5;
    distribution.reference_statistics.q95 = -10.5;
    distribution.reference_statistics.maximum = -10.0;
    distribution.reference_statistics.mean = -15.0;
    distribution.reference_statistics.population_sd = 5.0;
    distribution.reference_statistics.confidence = 0.8;
    distribution.reference_statistics.valid = 1;
    distribution.model_statistics = distribution.reference_statistics;
    distribution.reference_valid = 1;
    distribution.model_valid = 1;
    set.distributions = &distribution;
    set.distribution_count = 1U;
    gap.id = 1U;
    gap.distribution_id = 1U;
    gap.mean_delta_valid = 1;
    gap.median_delta_valid = 1;
    gap.quantile_distance = 1.0;
    gap.quantile_distance_valid = 1;
    gap.standardized_mean_shift_valid = 1;
    gap.valid_coverage = 1.0;
    gap.valid_coverage_valid = 1;
    gap.gap_score = 0.25;
    gap.gap_score_valid = 1;
    gap.rank = 1U;
    set.gaps = &gap;
    set.gap_count = 1U;
    warning.id = 1U;
    warning.code = (char *)"unmatched";
    warning.message = (char *)"one side is missing";
    warning.group_id = 1U;
    warning.distribution_id = 1U;
    warning.group_id_valid = 1;
    warning.distribution_id_valid = 1;
    set.warnings = &warning;
    set.warning_count = 1U;
    CHECK(test_capture_comparison(hwa_report_profile_comparison_json, &set,
                                  output, &size),
          "cannot capture comparison JSON");
    CHECK(test_json_document(output, size), "comparison JSON is invalid");
    CHECK(strstr(output, "\"schema_version\":6") != NULL &&
              strstr(output, "\"options\":{") != NULL &&
              strstr(output, "\"groups\":[{") != NULL &&
              strstr(output, "\"distributions\":[{") != NULL &&
              strstr(output, "\"gaps\":[{") != NULL,
          "comparison JSON omits full data");
    CHECK(strstr(output, "\"warnings\":[{\"id\":1") != NULL &&
              strstr(output, "\"group_id\":1,\"distribution_id\":1") != NULL,
          "comparison JSON omits linked warnings");
    CHECK(strstr(output, "\"retained_work_bytes\":9876") != NULL,
          "comparison JSON omits retained work");
    set.reference_path = NULL;
    {
        FILE *invalid_stream = tmpfile();
        CHECK(invalid_stream != NULL &&
                  hwa_report_profile_comparison_json(invalid_stream, &set) != 0,
              "comparison JSON accepted a null required path");
        if (invalid_stream != NULL) (void)fclose(invalid_stream);
    }
}

int main(void)
{
    test_measurement_json();
    test_comparison_json();
    if (failures != 0) {
        (void)fprintf(stderr, "%d Stage 4 report test(s) failed\n", failures);
        return 1;
    }
    return 0;
}

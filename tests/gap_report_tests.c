#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "gap_report.h"
#include "production.h"
#include "run.h"
#include "sha256.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#define CHECK(condition, message)                                           \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "FAIL: %s\n", message);                        \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static char *copy_text(const char *text)
{
    size_t size = strlen(text) + 1U;
    char *copy = (char *)malloc(size);
    if (copy != NULL) memcpy(copy, text, size);
    return copy;
}

static void set_all_work_caps(HWAGapReportOptions *options, uint64_t bytes);

static int make_temp_file(char path[256], const char *bytes)
{
#if defined(_WIN32)
    (void)path;
    (void)bytes;
    return -1;
#else
    int descriptor;
    size_t size = strlen(bytes);
    (void)snprintf(path, 256U, "/tmp/hwa-gap-core-XXXXXX");
    descriptor = mkstemp(path);
    if (descriptor < 0 || write(descriptor, bytes, size) != (ssize_t)size ||
        close(descriptor) != 0) {
        if (descriptor >= 0) (void)close(descriptor);
        return -1;
    }
    return 0;
#endif
}

static int test_names_and_defaults(void)
{
    HWAGapReportOptions options;
    HWAGapReportSourceKind kind = HWA_GAP_REPORT_SOURCE_KIND_COUNT;
    char error[HWA_ERROR_SIZE];
    int failures = 0;
    hwa_gap_report_options_default(&options);
    CHECK(strcmp(hwa_gap_report_mode_name(HWA_GAP_REPORT_FULL), "full") == 0,
          "full mode name");
    CHECK(hwa_gap_report_source_kind_from_name("run", &kind) == 0 &&
              kind == HWA_GAP_REPORT_SOURCE_RUN,
          "run source parser");
    CHECK(hwa_gap_report_options_validate(&options, error, sizeof(error)) == 0,
          "default options validate");
    options.experiment.run.max_links = 0U;
    CHECK(hwa_gap_report_options_validate(&options, error, sizeof(error)) != 0,
          "nested zero option fails");
    hwa_gap_report_options_default(&options);
    options.production.max_spans = 1U;
    options.production.max_evaluation_rows =
        hwa_production_metric_catalog_count() * 3U;
    CHECK(hwa_gap_report_options_validate(&options, error, sizeof(error)) == 0,
          "source-free Stage 6 option ceilings validate");
    return failures;
}

static int test_rebuild_score_and_link(void)
{
    HWAGapReportResult result;
    char error[HWA_ERROR_SIZE];
    int failures = 0;
    memset(&result, 0, sizeof(result));
    hwa_gap_report_options_default(&result.options);
    result.candidates = (HWAGapReportCandidate *)calloc(
        2U, sizeof(*result.candidates));
    result.cases = (HWAGapReportCase *)calloc(2U, sizeof(*result.cases));
    result.candidate_count = 2U;
    result.case_count = 2U;
    result.candidates[0].id = 1U;
    result.candidates[0].source_id = 1U;
    result.candidates[0].source_row = 1U;
    result.candidates[0].case_id = copy_text("one");
    result.candidates[0].metric = copy_text("level");
    result.candidates[0].family_key = copy_text("same");
    result.candidates[0].reason = copy_text("");
    result.candidates[0].kind = HWA_GAP_REPORT_CANDIDATE_RUN_FEATURE;
    result.candidates[0].availability = HWA_GAP_REPORT_AVAILABLE;
    result.candidates[0].raw_value = 0.5;
    result.candidates[0].raw_value_valid = 1;
    result.candidates[1] = result.candidates[0];
    result.candidates[1].id = 2U;
    result.candidates[1].source_row = 2U;
    result.candidates[1].case_id = copy_text("two");
    result.candidates[1].metric = copy_text("level");
    result.candidates[1].family_key = copy_text("same");
    result.candidates[1].reason = copy_text("");
    result.candidates[1].raw_value = 0.25;
    result.cases[0].id = 1U;
    result.cases[0].candidate_id = 1U;
    result.cases[0].case_id = copy_text("one");
    result.cases[0].reason = copy_text("");
    result.cases[0].availability = HWA_GAP_REPORT_AVAILABLE;
    result.cases[0].value = 0.5;
    result.cases[0].value_valid = 1;
    result.cases[0].confidence = 1.0;
    result.cases[0].confidence_valid = 1;
    result.cases[1].id = 2U;
    result.cases[1].candidate_id = 2U;
    result.cases[1].case_id = copy_text("two");
    result.cases[1].reason = copy_text("");
    result.cases[1].availability = HWA_GAP_REPORT_AVAILABLE;
    result.cases[1].value = 0.25;
    result.cases[1].value_valid = 1;
    result.cases[1].confidence = 1.0;
    result.cases[1].confidence_valid = 1;
    CHECK(hwa_gap_report_result_rebuild(&result, error, sizeof(error)) == 0,
          "rebuild succeeds");
    CHECK(result.family_count == 1U, "linked family collapsed");
    CHECK(result.candidates[0].primary == 1 &&
              result.candidates[0].rank == 1U,
          "strongest linked member ranks");
    CHECK(fabs(result.candidates[0].score - 0.425) < 1e-12,
          "factor product exact");
    CHECK((result.candidates[1].quality_flags &
           HWA_GAP_REPORT_QUALITY_LINKED_SECONDARY) != 0U,
          "secondary remains explicit");
    CHECK(result.cases[0].score_valid && result.cases[0].score >
              result.cases[1].score,
          "case scores rebuild from case evidence");
    CHECK(result.evaluation_count != 0U,
          "saved evaluation ledger is derived");
    {
        uint64_t peak = 0U;
        uint64_t rebuild_peak = 0U;
        uint64_t retained = 0U;
        CHECK(hwa_gap_report_result_retained_bytes(&result, &retained) == 0 &&
              hwa_gap_report_result_peak_work_bytes(
                  &result, 257U, &peak) == 0 &&
              hwa_gap_report_result_peak_work_bytes(
                  &result, 0U, &rebuild_peak) == 0 &&
              peak == rebuild_peak + 257U && peak > retained + 257U,
              "peak includes reader, snapshots, derived rows, and strings");
        result.options.max_work_bytes = rebuild_peak - 1U;
        result.options.measurement.max_work_bytes = rebuild_peak - 1U;
        result.options.production.max_work_bytes = rebuild_peak - 1U;
        result.options.production.profile_limits.max_work_bytes =
            rebuild_peak - 1U;
        result.options.run.max_work_bytes = rebuild_peak - 1U;
        result.options.experiment.max_work_bytes = rebuild_peak - 1U;
        result.options.experiment.run.max_work_bytes = rebuild_peak - 1U;
        CHECK(hwa_gap_report_result_rebuild(
                  &result, error, sizeof(error)) != 0,
              "one-under peak work cap fails before rebuild");
        result.options.max_work_bytes = rebuild_peak;
        result.options.measurement.max_work_bytes = rebuild_peak;
        result.options.production.max_work_bytes = rebuild_peak;
        result.options.production.profile_limits.max_work_bytes = rebuild_peak;
        result.options.run.max_work_bytes = rebuild_peak;
        result.options.experiment.max_work_bytes = rebuild_peak;
        result.options.experiment.run.max_work_bytes = rebuild_peak;
        CHECK(hwa_gap_report_result_rebuild(
                  &result, error, sizeof(error)) == 0,
              "exact peak work cap permits rebuild");
    }
    {
        uint64_t exact = result.evaluation_count;
        result.evaluation_count = exact - 1U;
        result.manifest_path = copy_text(".");
        result.title = copy_text("test");
        result.audibility_method = copy_text(HWA_GAP_REPORT_AUDIBILITY_METHOD);
        memset(result.manifest_sha256, 'a', HWA_SHA256_HEX_SIZE - 1U);
        result.manifest_sha256[HWA_SHA256_HEX_SIZE - 1U] = '\0';
        CHECK(hwa_gap_report_result_retained_bytes(
                  &result, &result.retained_work_bytes) == 0 &&
              hwa_gap_report_result_validate(
                  &result, error, sizeof(error)) != 0,
              "one-under evaluation ledger is rejected");
        result.evaluation_count = exact;
        {
            uint64_t validation_peak = 0U;
            HWAGapReportResult projected = result;
            projected.options.max_work_bytes = UINT64_MAX;
            CHECK(hwa_gap_report_result_peak_work_bytes(
                      &projected, 0U, &validation_peak) == 0 &&
                      validation_peak > result.retained_work_bytes,
                  "validator peak work projection");
            set_all_work_caps(&result.options, validation_peak - 1U);
            CHECK(hwa_gap_report_result_validate(
                      &result, error, sizeof(error)) != 0,
                  "one-under validator peak is rejected before rederive");
        }
    }
    hwa_gap_report_result_free(&result);
    return failures;
}

static int test_malformed_measurement_prefix(void)
{
#if defined(_WIN32)
    return 0;
#else
    char first[256];
    char second[256];
    char manifest[256];
    char first_hash[HWA_SHA256_HEX_SIZE];
    char second_hash[HWA_SHA256_HEX_SIZE];
    char json[2048];
    char error[HWA_ERROR_SIZE];
    HWARunBinding bindings[2];
    HWAGapReportResult result;
    FILE *stream;
    int failures = 0;
    memset(&result, 0, sizeof(result));
    if (make_temp_file(first, "one") != 0 ||
        make_temp_file(second, "two") != 0 ||
        make_temp_file(manifest, "") != 0 ||
        hwa_sha256_file(first, 16U, first_hash, error, sizeof(error)) != 0 ||
        hwa_sha256_file(second, 16U, second_hash, error, sizeof(error)) != 0) {
        CHECK(0, "malformed prefix fixture setup");
        return failures;
    }
    (void)snprintf(json, sizeof(json),
        "{\"schema\":\"hwa-gap-report\",\"schema_version\":1,"
        "\"method_version\":\"stage9-1\","
        "\"audibility_method\":\"hwa-audibility-1\","
        "\"title\":\"prefix\",\"sources\":["
        "{\"id\":\"pair.model.extra\",\"kind\":\"measurement\","
        "\"sha256\":\"%s\"},"
        "{\"id\":\"pair.reference.extra\",\"kind\":\"measurement\","
        "\"sha256\":\"%s\"}],\"case_labels\":[],\"excerpts\":[]}",
        first_hash, second_hash);
    stream = fopen(manifest, "wb");
    if (stream == NULL || fputs(json, stream) == EOF || fclose(stream) != 0) {
        if (stream != NULL) (void)fclose(stream);
        CHECK(0, "malformed prefix manifest write");
    } else {
        bindings[0].id = "pair.model.extra";
        bindings[0].path = first;
        bindings[1].id = "pair.reference.extra";
        bindings[1].path = second;
        CHECK(hwa_build_gap_report_files(
                  manifest, bindings, 2U, NULL, HWA_GAP_REPORT_RANK,
                  NULL, &result, error, sizeof(error)) != 0 &&
              strstr(error, "measurement cohort") != NULL &&
              result.source_count == 0U,
              "malformed measurement prefix fails without a partial result");
    }
    (void)unlink(first);
    (void)unlink(second);
    (void)unlink(manifest);
    hwa_gap_report_result_free(&result);
    return failures;
#endif
}

static int test_source_scoped_families(void)
{
    HWAGapReportResult result;
    char error[HWA_ERROR_SIZE];
    size_t index;
    int failures = 0;
    memset(&result, 0, sizeof(result));
    hwa_gap_report_options_default(&result.options);
    result.candidates = (HWAGapReportCandidate *)calloc(
        2U, sizeof(*result.candidates));
    result.cases = (HWAGapReportCase *)calloc(2U, sizeof(*result.cases));
    result.candidate_count = 2U;
    result.case_count = 2U;
    for (index = 0U; index < 2U; ++index) {
        HWAGapReportCandidate *candidate = &result.candidates[index];
        HWAGapReportCase *row = &result.cases[index];
        candidate->id = (uint64_t)index + UINT64_C(1);
        candidate->source_id = (uint64_t)index + UINT64_C(1);
        candidate->source_row = 1U;
        candidate->case_id = copy_text("case");
        candidate->metric = copy_text("level");
        candidate->family_key = copy_text("same-metric");
        candidate->kind = HWA_GAP_REPORT_CANDIDATE_RUN_FEATURE;
        candidate->availability = HWA_GAP_REPORT_AVAILABLE;
        candidate->raw_value = 0.5;
        candidate->raw_value_valid = 1;
        candidate->reason = copy_text("");
        row->id = (uint64_t)index + UINT64_C(1);
        row->candidate_id = candidate->id;
        row->case_id = copy_text("case");
        row->availability = HWA_GAP_REPORT_AVAILABLE;
        row->value = 0.5;
        row->confidence = 1.0;
        row->value_valid = 1;
        row->confidence_valid = 1;
        row->reason = copy_text("");
    }
    CHECK(hwa_gap_report_result_rebuild(
              &result, error, sizeof(error)) == 0,
          "source-scoped family rebuild succeeds");
    CHECK(result.family_count == 2U &&
              result.candidates[0].primary && result.candidates[1].primary,
          "same metric from two sources collapsed into one family");
    hwa_gap_report_result_free(&result);
    return failures;
}

static int test_measurement_catalog_truth(void)
{
    static const char group[] = "g/body/626f6479/all/";
    static const char family[] =
        "source:2:measurement:g/body/626f6479/all/:level";
    HWAGapReportResult result;
    size_t index;
    int failures = 0;
    memset(&result, 0, sizeof(result));
    hwa_gap_report_options_default(&result.options);
    result.source_count = 2U;
    result.sources = (HWAGapReportSource *)calloc(
        2U, sizeof(*result.sources));
    result.candidate_count = 2U;
    result.candidates = (HWAGapReportCandidate *)calloc(
        2U, sizeof(*result.candidates));
    result.case_count = 4U;
    result.cases = (HWAGapReportCase *)calloc(4U, sizeof(*result.cases));
    CHECK(result.sources != NULL && result.candidates != NULL &&
              result.cases != NULL, "measurement catalog allocation");
    if (result.sources == NULL || result.candidates == NULL ||
        result.cases == NULL) {
        hwa_gap_report_result_free(&result);
        return failures;
    }
    result.sources[0].id = 1U;
    result.sources[0].name = copy_text("pair.model");
    result.sources[0].kind = HWA_GAP_REPORT_SOURCE_MEASUREMENT;
    result.sources[1].id = 2U;
    result.sources[1].name = copy_text("pair.reference");
    result.sources[1].kind = HWA_GAP_REPORT_SOURCE_MEASUREMENT;
    result.sources[1].candidate_count = 2U;
    for (index = 0U; index < 2U; ++index) {
        HWAGapReportCandidate *candidate = &result.candidates[index];
        size_t case_index;
        candidate->id = (uint64_t)index + UINT64_C(1);
        candidate->source_id = 2U;
        candidate->source_row = (uint64_t)index + UINT64_C(1);
        candidate->case_id = copy_text(group);
        candidate->metric = copy_text("level");
        candidate->family_key = copy_text(family);
        candidate->kind = HWA_GAP_REPORT_CANDIDATE_MEASUREMENT;
        for (case_index = 0U; case_index < 2U; ++case_index) {
            HWAGapReportCase *record =
                &result.cases[index * 2U + case_index];
            record->id = (uint64_t)(index * 2U + case_index) + UINT64_C(1);
            record->candidate_id = candidate->id;
            record->case_id = copy_text(case_index == 0U ? "a" : "z");
            record->reason = copy_text("");
            record->availability = HWA_GAP_REPORT_AVAILABLE;
            record->value_valid = 1;
            record->confidence_valid = 1;
        }
    }
    CHECK(hwa_gap_report_candidate_catalog_fits(
              &result, &result.options) &&
              hwa_gap_report_case_catalog_fits(&result, &result.options),
          "valid measurement catalog rejected");
    free(result.candidates[0].metric);
    free(result.candidates[0].family_key);
    result.candidates[0].metric = copy_text("other");
    result.candidates[0].family_key = copy_text(
        "source:2:measurement:g/body/626f6479/all/:other");
    free(result.candidates[1].metric);
    free(result.candidates[1].family_key);
    result.candidates[1].metric = copy_text("crest");
    result.candidates[1].family_key = copy_text(
        "source:2:measurement:g/body/626f6479/all/:crest");
    CHECK(!hwa_gap_report_candidate_catalog_fits(&result, &result.options),
          "impossible measurement metric order accepted");
    free(result.candidates[0].metric);
    free(result.candidates[0].family_key);
    result.candidates[0].metric = copy_text("level");
    result.candidates[0].family_key = copy_text(family);
    free(result.candidates[1].metric);
    free(result.candidates[1].family_key);
    result.candidates[1].metric = copy_text("level");
    result.candidates[1].family_key = copy_text(family);
    free(result.cases[3].case_id);
    result.cases[3].case_id = copy_text("y");
    CHECK(!hwa_gap_report_case_catalog_fits(&result, &result.options),
          "divergent measurement case set accepted");
    hwa_gap_report_result_free(&result);
    {
        HWAGapReportResult limit;
        memset(&limit, 0, sizeof(limit));
        hwa_gap_report_options_default(&limit.options);
        limit.source_count = 2U;
        limit.sources = (HWAGapReportSource *)calloc(
            2U, sizeof(*limit.sources));
        limit.candidate_count = 83U;
        limit.candidates = (HWAGapReportCandidate *)calloc(
            83U, sizeof(*limit.candidates));
        CHECK(limit.sources != NULL && limit.candidates != NULL,
              "measurement maximum catalog allocation");
        if (limit.sources != NULL && limit.candidates != NULL) {
            limit.sources[0].id = 1U;
            limit.sources[0].name = copy_text("pair.model");
            limit.sources[0].kind = HWA_GAP_REPORT_SOURCE_MEASUREMENT;
            limit.sources[1].id = 2U;
            limit.sources[1].name = copy_text("pair.reference");
            limit.sources[1].kind = HWA_GAP_REPORT_SOURCE_MEASUREMENT;
            for (index = 0U; index < 83U; ++index) {
                HWAGapReportCandidate *candidate = &limit.candidates[index];
                candidate->source_id = 2U;
                candidate->source_row = (uint64_t)index + UINT64_C(1);
                candidate->case_id = copy_text(group);
                candidate->metric = copy_text("level");
                candidate->family_key = copy_text(family);
                candidate->kind = HWA_GAP_REPORT_CANDIDATE_MEASUREMENT;
            }
            limit.candidate_count = 82U;
            limit.sources[1].candidate_count = 82U;
            CHECK(hwa_gap_report_candidate_catalog_fits(
                      &limit, &limit.options),
                  "valid 82-row measurement level catalog rejected");
            limit.candidate_count = 83U;
            limit.sources[1].candidate_count = 83U;
            CHECK(!hwa_gap_report_candidate_catalog_fits(
                       &limit, &limit.options),
                  "83-row measurement level catalog accepted");
        }
        hwa_gap_report_result_free(&limit);
    }
    return failures;
}

static int test_production_check_key(char key[32])
{
    unsigned value;
    for (value = 0U; value < 10000U; ++value) {
        HWAProductionSplit split;
        int written = snprintf(key, 32U, "tail:%04u", value);
        if (written < 0 || written >= 32) return 0;
        if (hwa_production_split_for_item_key(key, &split) == 0 &&
            split == HWA_PRODUCTION_CHECK) return 1;
    }
    return 0;
}

static const char *test_run_metric(HWARunFeatureKind kind,
                                   uint32_t index,
                                   char buffer[32])
{
    if (kind == HWA_RUN_FEATURE_RMS_DBFS) return "level";
    if (kind == HWA_RUN_FEATURE_CREST_DB) return "crest";
    if (kind == HWA_RUN_FEATURE_BAND_LEVEL_DBFS) {
        (void)snprintf(buffer, 32U, "band:%" PRIu32, index);
        return buffer;
    }
    return "other";
}

static int test_catalog_evaluation_minima(void)
{
    HWAGapReportResult result;
    HWAGapReportSource source;
    HWAGapReportCandidate candidate;
    HWAGapReportCase row;
    char check_key[32];
    int failures = 0;
    memset(&result, 0, sizeof(result));
    memset(&source, 0, sizeof(source));
    memset(&candidate, 0, sizeof(candidate));
    memset(&row, 0, sizeof(row));
    hwa_gap_report_options_default(&result.options);
    CHECK(test_production_check_key(check_key),
          "production CHECK key fixture");
    source.id = 1U;
    source.kind = HWA_GAP_REPORT_SOURCE_PRODUCTION;
    source.candidate_count = 1U;
    candidate.id = 1U;
    row.candidate_id = 1U;
    row.case_id = check_key;
    row.reason = (char *)"";
    result.sources = &source;
    result.source_count = 1U;
    result.candidates = &candidate;
    result.candidate_count = 1U;
    result.cases = &row;
    result.case_count = 1U;
    result.options.production.max_evaluations = 4U;
    CHECK(hwa_gap_report_case_catalog_fits(&result, &result.options),
          "exact production evaluation minimum rejected");
    result.options.production.max_evaluations = 3U;
    CHECK(!hwa_gap_report_case_catalog_fits(&result, &result.options),
          "one-under production evaluation minimum accepted");

    source.kind = HWA_GAP_REPORT_SOURCE_EXPERIMENT;
    candidate.metric = (char *)"level";
    candidate.family_key = (char *)"source:1:experiment:role:4:level";
    row.case_id = (char *)"case-one";
    hwa_gap_report_options_default(&result.options);
    result.options.experiment.run.max_evaluations = 2U;
    result.options.experiment.max_total_run_evaluations = 4U;
    CHECK(hwa_gap_report_case_catalog_fits(&result, &result.options),
          "exact experiment evaluation minima rejected");
    result.options.experiment.run.max_evaluations = 1U;
    CHECK(!hwa_gap_report_case_catalog_fits(&result, &result.options),
          "one-under experiment stem evaluation minimum accepted");
    result.options.experiment.run.max_evaluations = 2U;
    result.options.experiment.max_total_run_evaluations = 3U;
    CHECK(!hwa_gap_report_case_catalog_fits(&result, &result.options),
          "one-under experiment total evaluation minimum accepted");

    {
        HWAGapReportCandidate candidates[15];
        char cases[15][32];
        char metrics[15][32];
        char families[15][160];
        size_t feature_count = hwa_run_feature_catalog_count();
        size_t stage_count = hwa_run_stage_catalog_count();
        size_t index;
        memset(candidates, 0, sizeof(candidates));
        CHECK(feature_count == 12U && stage_count == 3U,
              "fixed run catalog size");
        source.kind = HWA_GAP_REPORT_SOURCE_RUN;
        source.candidate_count = feature_count + stage_count;
        result.candidates = candidates;
        result.candidate_count = source.candidate_count;
        result.cases = NULL;
        result.case_count = 0U;
        for (index = 0U; index < feature_count; ++index) {
            HWARunFeatureKind kind;
            uint32_t feature_index;
            HWARunUnit unit;
            const char *metric;
            CHECK(hwa_run_feature_catalog_at(index, &kind, &feature_index,
                      &unit) == 0, "run feature fixture");
            metric = test_run_metric(kind, feature_index, metrics[index]);
            if (metric != metrics[index])
                (void)snprintf(metrics[index], sizeof(metrics[index]),
                               "%s", metric);
            (void)snprintf(cases[index], sizeof(cases[index]),
                           "feature:%zu", index + 1U);
            (void)snprintf(families[index], sizeof(families[index]),
                           "source:1:run:role:4:%.31s",
                           strncmp(metrics[index], "band:", 5U) == 0
                               ? "band" : metrics[index]);
            candidates[index].source_id = 1U;
            candidates[index].source_row = (uint64_t)index + UINT64_C(1);
            candidates[index].case_id = cases[index];
            candidates[index].metric = metrics[index];
            candidates[index].family_key = families[index];
            candidates[index].kind = HWA_GAP_REPORT_CANDIDATE_RUN_FEATURE;
        }
        for (index = 0U; index < stage_count; ++index) {
            size_t offset = feature_count + index;
            HWARunStemRole from_role;
            HWARunStemRole to_role;
            CHECK(hwa_run_stage_catalog_at(index, &from_role, &to_role) == 0,
                  "run stage fixture");
            (void)snprintf(metrics[offset], sizeof(metrics[offset]), "other");
            (void)snprintf(cases[offset], sizeof(cases[offset]),
                           "stage:%zu", index + 1U);
            (void)snprintf(families[offset], sizeof(families[offset]),
                           "source:1:run:stage:%d:%d",
                           (int)from_role, (int)to_role);
            candidates[offset].source_id = 1U;
            candidates[offset].source_row = (uint64_t)offset + UINT64_C(1);
            candidates[offset].case_id = cases[offset];
            candidates[offset].metric = metrics[offset];
            candidates[offset].family_key = families[offset];
            candidates[offset].kind = HWA_GAP_REPORT_CANDIDATE_RUN_STAGE;
        }
        hwa_gap_report_options_default(&result.options);
        result.options.run.max_evaluations = 2U;
        CHECK(hwa_gap_report_candidate_catalog_fits(&result, &result.options),
              "exact run evaluation minimum rejected");
        result.options.run.max_evaluations = 1U;
        CHECK(!hwa_gap_report_candidate_catalog_fits(&result, &result.options),
              "one-under run evaluation minimum accepted");
    }
    return failures;
}

static void set_all_work_caps(HWAGapReportOptions *options, uint64_t bytes)
{
    options->max_work_bytes = bytes;
    options->measurement.max_work_bytes = bytes;
    options->production.max_work_bytes = bytes;
    options->production.profile_limits.max_work_bytes = bytes;
    options->run.max_work_bytes = bytes;
    options->experiment.max_work_bytes = bytes;
    options->experiment.run.max_work_bytes = bytes;
}

static int test_public_build_peak(void)
{
#if defined(_WIN32)
    return 0;
#else
    char source[256];
    char manifest[256];
    char hash[HWA_SHA256_HEX_SIZE];
    char json[1024];
    char error[HWA_ERROR_SIZE];
    HWARunBinding binding;
    HWAGapReportOptions options;
    HWAGapReportResult result;
    FILE *stream;
    uint64_t low = 1U;
    uint64_t high = UINT64_C(1) << 20U;
    int failures = 0;
    if (make_temp_file(source, "bound source") != 0 ||
        make_temp_file(manifest, "") != 0 ||
        hwa_sha256_file(source, 32U, hash, error, sizeof(error)) != 0) {
        CHECK(0, "public peak fixture setup");
        return failures;
    }
    (void)snprintf(json, sizeof(json),
        "{\"schema\":\"hwa-gap-report\",\"schema_version\":1,"
        "\"method_version\":\"stage9-1\","
        "\"audibility_method\":\"hwa-audibility-1\","
        "\"title\":\"peak\",\"sources\":["
        "{\"id\":\"wave\",\"kind\":\"wave\",\"sha256\":\"%s\"}],"
        "\"case_labels\":[],\"excerpts\":[]}", hash);
    stream = fopen(manifest, "wb");
    if (stream == NULL || fputs(json, stream) == EOF || fclose(stream) != 0) {
        if (stream != NULL) (void)fclose(stream);
        CHECK(0, "public peak manifest write");
        goto cleanup;
    }
    binding.id = "wave";
    binding.path = source;
    while (low < high) {
        uint64_t middle = low + (high - low) / UINT64_C(2);
        memset(&result, 0, sizeof(result));
        hwa_gap_report_options_default(&options);
        set_all_work_caps(&options, middle);
        if (hwa_build_gap_report_files(
                manifest, &binding, 1U, NULL, HWA_GAP_REPORT_RANK,
                &options, &result, error, sizeof(error)) == 0) high = middle;
        else low = middle + UINT64_C(1);
        hwa_gap_report_result_free(&result);
    }
    memset(&result, 0, sizeof(result));
    hwa_gap_report_options_default(&options);
    set_all_work_caps(&options, low);
    CHECK(hwa_build_gap_report_files(
              manifest, &binding, 1U, NULL, HWA_GAP_REPORT_RANK,
              &options, &result, error, sizeof(error)) == 0,
          "exact public peak work cap failed");
    hwa_gap_report_result_free(&result);
    if (low > 1U) {
        hwa_gap_report_options_default(&options);
        set_all_work_caps(&options, low - UINT64_C(1));
        CHECK(hwa_build_gap_report_files(
                  manifest, &binding, 1U, NULL, HWA_GAP_REPORT_RANK,
                  &options, &result, error, sizeof(error)) != 0 &&
                  result.source_count == 0U,
              "one-under public peak work cap succeeded");
        hwa_gap_report_result_free(&result);
    }
cleanup:
    (void)unlink(source);
    (void)unlink(manifest);
    return failures;
#endif
}

int main(void)
{
    int failures = 0;
    failures += test_names_and_defaults();
    failures += test_rebuild_score_and_link();
    failures += test_malformed_measurement_prefix();
    failures += test_source_scoped_families();
    failures += test_measurement_catalog_truth();
    failures += test_catalog_evaluation_minima();
    failures += test_public_build_peak();
    if (failures != 0) return EXIT_FAILURE;
    puts("gap report tests passed");
    return EXIT_SUCCESS;
}

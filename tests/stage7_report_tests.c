#include "hlolli_wg_analyzer.h"
#include "run.h"
#include "run_report.h"

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition, ...)                                               \
    do {                                                                    \
        if (!(condition)) {                                                 \
            (void)fprintf(stderr, "FAIL: ");                               \
            (void)fprintf(stderr, __VA_ARGS__);                             \
            (void)fputc('\n', stderr);                                      \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static char *test_copy(const char *text)
{
    size_t size = strlen(text) + 1U;
    char *copy = (char *)malloc(size);
    if (copy != NULL) memcpy(copy, text, size);
    return copy;
}

static void test_hash(char hash[HWA_SHA256_HEX_SIZE], char digit)
{
    memset(hash, digit, HWA_SHA256_HEX_SIZE - 1U);
    hash[HWA_SHA256_HEX_SIZE - 1U] = '\0';
}

static int test_stem(HWARunSource *source,
                     uint64_t id,
                     const char *binding,
                     HWARunSide side)
{
    memset(source, 0, sizeof(*source));
    source->id = id;
    source->binding_id = test_copy(binding);
    source->path = test_copy(id == 1U ? "/stem-\xff.wav" : "/reference.wav");
    test_hash(source->sha256, id == 1U ? 'a' : 'c');
    source->kind = HWA_RUN_SOURCE_STEM;
    source->side = side;
    source->role = HWA_RUN_STEM_FINAL;
    source->format.container = HWA_CONTAINER_RIFF;
    source->format.encoding = HWA_ENCODING_PCM;
    source->format.channels = 1U;
    source->format.sample_rate_hz = 48000U;
    source->format.bits_per_sample = 16U;
    source->format.valid_bits_per_sample = 16U;
    source->format.block_align = 2U;
    source->format.frames = 48000U;
    source->format.data_bytes = 96000U;
    source->format.duration_seconds = 1.0;
    source->rate_numerator = 48000U;
    source->rate_denominator = 1U;
    source->file_bytes = 96044U;
    source->value_count = 48000U;
    return source->binding_id != NULL && source->path != NULL;
}

static int test_make_result(HWARunResult *result)
{
    size_t index;
    memset(result, 0, sizeof(*result));
    hwa_run_options_default(&result->options);
    result->manifest_path = test_copy("/manifest-\xfe.json");
    test_hash(result->manifest_sha256, 'f');
    result->clock_rate_hz = 48000U;
    result->source_count = 3U;
    result->clock_count = 1U;
    result->feature_count = hwa_run_feature_catalog_count();
    result->stage_count = hwa_run_stage_catalog_count();
    result->probe_count = 1U;
    result->link_count = 1U;
    result->sources = (HWARunSource *)calloc(3U, sizeof(*result->sources));
    result->clocks = (HWARunClock *)calloc(1U, sizeof(*result->clocks));
    result->features = (HWARunFeature *)calloc(
        result->feature_count, sizeof(*result->features));
    result->stages = (HWARunStage *)calloc(
        result->stage_count, sizeof(*result->stages));
    result->probes = (HWARunProbe *)calloc(1U, sizeof(*result->probes));
    result->links = (HWARunLink *)calloc(1U, sizeof(*result->links));
    if (result->manifest_path == NULL || result->sources == NULL ||
        result->clocks == NULL || result->features == NULL ||
        result->stages == NULL || result->probes == NULL ||
        result->links == NULL ||
        !test_stem(&result->sources[0], 1U, "model.final", HWA_RUN_MODEL)) {
        return 0;
    }
    result->sources[1].id = 2U;
    result->sources[1].binding_id = test_copy("model.force");
    result->sources[1].path = test_copy("/probe.csv");
    test_hash(result->sources[1].sha256, 'b');
    result->sources[1].kind = HWA_RUN_SOURCE_PROBE;
    result->sources[1].side = HWA_RUN_MODEL;
    result->sources[1].probe_format = HWA_RUN_PROBE_CSV_F64;
    result->sources[1].probe_name = test_copy("pm.excitation.force");
    result->sources[1].unit = test_copy("si.N");
    result->sources[1].rate_numerator = 100U;
    result->sources[1].rate_denominator = 1U;
    result->sources[1].file_bytes = 800U;
    result->sources[1].value_count = 64U;
    if (result->sources[1].binding_id == NULL ||
        result->sources[1].path == NULL ||
        result->sources[1].probe_name == NULL ||
        result->sources[1].unit == NULL ||
        !test_stem(&result->sources[2], 3U, "reference.final",
                   HWA_RUN_REFERENCE)) return 0;
    result->clocks[0].id = 1U;
    result->clocks[0].role = HWA_RUN_STEM_FINAL;
    result->clocks[0].reference_source_id = 3U;
    result->clocks[0].model_source_id = 1U;
    result->clocks[0].availability = HWA_RUN_AVAILABLE;
    result->clocks[0].overlap_frames = 48000U;
    for (index = 0U; index < result->feature_count; ++index) {
        HWARunFeature *feature = &result->features[index];
        feature->id = (uint64_t)index + 1U;
        feature->clock_id = 1U;
        feature->role = HWA_RUN_STEM_FINAL;
        if (hwa_run_feature_catalog_at(index, &feature->kind,
                                       &feature->index,
                                       &feature->unit) != 0) return 0;
        feature->availability = HWA_RUN_AVAILABLE;
        feature->reference_value = -20.0;
        feature->model_value = -18.0;
        feature->delta = 2.0;
        feature->normalized_gap = 2.0 / 12.0;
        feature->reference_valid = 1;
        feature->model_valid = 1;
        feature->delta_valid = 1;
        feature->gap_valid = 1;
    }
    result->features[result->feature_count - 1U].availability =
        HWA_RUN_INSUFFICIENT;
    result->features[result->feature_count - 1U].model_value = 0.0;
    result->features[result->feature_count - 1U].delta = 0.0;
    result->features[result->feature_count - 1U].normalized_gap = 0.0;
    result->features[result->feature_count - 1U].model_valid = 0;
    result->features[result->feature_count - 1U].delta_valid = 0;
    result->features[result->feature_count - 1U].gap_valid = 0;
    result->probes[0].id = 1U;
    result->probes[0].source_id = 2U;
    result->probes[0].availability = HWA_RUN_AVAILABLE;
    result->probes[0].value_count = 64U;
    result->probes[0].minimum = 0.0;
    result->probes[0].maximum = 1.0;
    result->probes[0].mean = 0.5;
    result->probes[0].population_sd = 0.25;
    result->probes[0].statistics_valid = 1;
    result->links[0].id = 1U;
    result->links[0].stem_source_id = 1U;
    result->links[0].probe_source_id = 2U;
    result->links[0].feature = HWA_RUN_FEATURE_RMS_DBFS;
    result->links[0].availability = HWA_RUN_INSUFFICIENT;
    result->links[0].quality_flags = HWA_RUN_QUALITY_LOW_OVERLAP;
    if (hwa_run_stage_rows_rebuild(result, NULL, 0U) != 0 ||
        hwa_run_warnings_rebuild(result, NULL, 0U) != 0 ||
        hwa_run_evaluations_expected(
            result, &result->evaluation_count) != 0 ||
        hwa_run_result_retained_bytes(
            result, &result->retained_work_bytes) != 0) return 0;
    return hwa_run_result_validate(result, NULL, 0U) == 0;
}

static char *test_stream_text(FILE *stream)
{
    long end;
    char *text;
    if (fflush(stream) != 0 || fseek(stream, 0L, SEEK_END) != 0 ||
        (end = ftell(stream)) < 0L || fseek(stream, 0L, SEEK_SET) != 0) {
        return NULL;
    }
    text = (char *)malloc((size_t)end + 1U);
    if (text == NULL || fread(text, 1U, (size_t)end, stream) != (size_t)end) {
        free(text);
        return NULL;
    }
    text[(size_t)end] = '\0';
    return text;
}

static void test_reports(void)
{
    HWARunResult result;
    FILE *stream;
    char *text;
    CHECK(test_make_result(&result), "cannot make Stage 7 report fixture");
    stream = tmpfile();
    CHECK(stream != NULL && hwa_report_run_text(stream, &result) == 0,
          "Stage 7 text report failed");
    text = stream != NULL ? test_stream_text(stream) : NULL;
    CHECK(text != NULL && strstr(text, "Run analysis (stage7-1)") != NULL &&
              strstr(text, "link-insufficient") != NULL &&
              strstr(text, "Probe links:") != NULL &&
              strstr(text, "link 1 [insufficient] stem=model.final; "
                           "probe=model.force: fit unavailable") != NULL &&
              strstr(text, "Stage changes: unavailable") != NULL,
          "Stage 7 text report lacks method, link context, warning, or claim limit");
    free(text);
    if (stream != NULL) (void)fclose(stream);
    stream = tmpfile();
    CHECK(stream != NULL && hwa_report_run_json(stream, &result) == 0,
          "Stage 7 JSON report failed");
    text = stream != NULL ? test_stream_text(stream) : NULL;
    CHECK(text != NULL &&
              strstr(text, "\"schema_version\":9") != NULL &&
              strstr(text, "\"sources\":[") != NULL &&
              strstr(text, "\"clocks\":[") != NULL &&
              strstr(text, "\"features\":[") != NULL &&
              strstr(text, "\"stages\":[") != NULL &&
              strstr(text, "\"probes\":[") != NULL &&
              strstr(text, "\"links\":[") != NULL &&
              strstr(text, "\"warnings\":[") != NULL &&
              strstr(text, "\"correlation\":null") != NULL &&
              strstr(text, "\"reference_value\":-20,\"model_value\":null,"
                           "\"delta\":null,\"normalized_gap\":null") != NULL &&
              strstr(text, "\"file_bytes\":96044") != NULL &&
              strstr(text, "\\u00ff") != NULL &&
              strstr(text, "ff2e776176") != NULL,
          "Stage 7 JSON report lacks full arrays, nulls, or byte-safe path");
    free(text);
    if (stream != NULL) (void)fclose(stream);
    result.warnings[0].code[0] = 'X';
    stream = tmpfile();
    CHECK(stream != NULL && hwa_report_run_json(stream, &result) != 0,
          "Stage 7 JSON report accepted a forged fixed warning");
    if (stream != NULL) (void)fclose(stream);
    result.warnings[0].code[0] = 'l';
    result.stages[0].rank = 1U;
    stream = tmpfile();
    CHECK(stream != NULL && hwa_report_run_text(stream, &result) != 0,
          "Stage 7 text report accepted a forged unavailable stage");
    if (stream != NULL) (void)fclose(stream);
    result.stages[0].rank = 0U;
    result.links[0].correlation = NAN;
    stream = tmpfile();
    CHECK(stream != NULL && hwa_report_run_json(stream, &result) != 0,
          "Stage 7 JSON report accepted a hidden nonfinite link value");
    if (stream != NULL) (void)fclose(stream);
    result.links[0].correlation = 0.0;
    result.links[0].slope = 1.0;
    stream = tmpfile();
    CHECK(stream != NULL && hwa_report_run_text(stream, &result) != 0,
          "Stage 7 text report accepted a nonzero invalid link value");
    if (stream != NULL) (void)fclose(stream);
    result.links[0].slope = 0.0;
    result.links[0].coverage = NAN;
    stream = tmpfile();
    CHECK(stream != NULL && hwa_report_run_json(stream, &result) != 0,
          "Stage 7 JSON report accepted nonfinite link coverage");
    if (stream != NULL) (void)fclose(stream);
    result.links[0].coverage = 0.0;
    result.links[0].quality_flags = HWA_RUN_QUALITY_LOW_OVERLAP |
                                      HWA_RUN_QUALITY_LOW_VARIANCE;
    stream = tmpfile();
    CHECK(stream != NULL && hwa_report_run_text(stream, &result) != 0,
          "Stage 7 text report accepted a false low-variance flag");
    if (stream != NULL) (void)fclose(stream);
    result.links[0].quality_flags = HWA_RUN_QUALITY_LOW_OVERLAP;
    result.links[0].availability = HWA_RUN_AVAILABLE;
    result.links[0].lag_hops = 2;
    result.links[0].lag_samples = 960;
    result.links[0].correlation = 0.75;
    result.links[0].slope = 2.0;
    result.links[0].intercept = -30.0;
    result.links[0].r_squared = 0.5625;
    result.links[0].point_count = 64U;
    result.links[0].coverage = 64.0 / 99.0;
    result.links[0].quality_flags = 0U;
    result.links[0].fit_valid = 1;
    CHECK(hwa_run_warnings_rebuild(&result, NULL, 0U) == 0 &&
              hwa_run_evaluations_expected(
                  &result, &result.evaluation_count) == 0 &&
              hwa_run_result_retained_bytes(
                  &result, &result.retained_work_bytes) == 0,
          "cannot make available Stage 7 report link");
    stream = tmpfile();
    CHECK(stream != NULL && hwa_report_run_text(stream, &result) == 0,
          "Stage 7 available-link text report failed");
    text = stream != NULL ? test_stream_text(stream) : NULL;
    CHECK(text != NULL &&
              strstr(text, "link 1 [available] stem=model.final; "
                           "probe=model.force") != NULL &&
              strstr(text, "lag=2 hops / 960 samples") != NULL &&
              strstr(text, "r=+0.75; slope=+2; intercept=-30; "
                           "R2=0.5625; points=64; coverage=0.646") != NULL,
          "Stage 7 text report lacks available link facts");
    free(text);
    if (stream != NULL) (void)fclose(stream);
    result.clock_rate_hz = 0U;
    stream = tmpfile();
    CHECK(stream != NULL && hwa_report_run_json(stream, &result) != 0,
          "Stage 7 JSON report accepted an invalid result");
    if (stream != NULL) (void)fclose(stream);
    result.clock_rate_hz = 48000U;
#if !defined(_WIN32)
    stream = fopen("/dev/full", "wb");
    if (stream != NULL) {
        CHECK(hwa_report_run_json(stream, &result) != 0 ||
                  fflush(stream) != 0,
              "Stage 7 JSON report ignored a write fault");
        (void)fclose(stream);
    }
#endif
    hwa_run_result_free(&result);
}

static void test_locale(void)
{
    static const char *const candidates[] = {
        "de_DE.UTF-8", "fr_FR.UTF-8", "el_GR.UTF-8"
    };
    HWARunResult result;
    const char *original = setlocale(LC_NUMERIC, NULL);
    char *saved = original != NULL ? test_copy(original) : NULL;
    size_t index;
    for (index = 0U; index < sizeof(candidates) / sizeof(candidates[0]); ++index) {
        if (setlocale(LC_NUMERIC, candidates[index]) != NULL) break;
    }
    if (index < sizeof(candidates) / sizeof(candidates[0]) &&
        test_make_result(&result)) {
        FILE *stream = tmpfile();
        char *text;
        CHECK(stream != NULL && hwa_report_run_json(stream, &result) == 0,
              "Stage 7 locale JSON report failed");
        text = stream != NULL ? test_stream_text(stream) : NULL;
        CHECK(text != NULL && strstr(text, "\"mean\":0.5") != NULL,
              "Stage 7 JSON report used the process decimal separator");
        free(text);
        if (stream != NULL) (void)fclose(stream);
        hwa_run_result_free(&result);
    }
    if (saved != NULL) {
        (void)setlocale(LC_NUMERIC, saved);
        free(saved);
    }
}

int main(void)
{
    test_reports();
    test_locale();
    if (failures != 0) {
        (void)fprintf(stderr, "%d Stage 7 report test(s) failed\n", failures);
        return 1;
    }
    (void)puts("Stage 7 report tests passed");
    return 0;
}

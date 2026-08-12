#include "hlolli_wg_analyzer.h"
#include "production.h"
#include "production_file.h"
#include "production_report.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int test_source(HWAProductionSource *source,
                       uint64_t id,
                       const char *role,
                       const char *path,
                       char hash_byte,
                       int wave)
{
    source->id = id;
    source->role = test_copy(role);
    source->path = test_copy(path);
    test_hash(source->sha256, hash_byte);
    source->is_wave = wave;
    if (wave) {
        source->format.container = HWA_CONTAINER_RIFF;
        source->format.encoding = HWA_ENCODING_PCM;
        source->format.channels = 2U;
        source->format.sample_rate_hz = 48000U;
        source->format.bits_per_sample = 16U;
        source->format.valid_bits_per_sample = 16U;
        source->format.block_align = 4U;
        source->format.frames = 48000U;
        source->format.data_bytes = 192000U;
        source->format.duration_seconds = 1.0;
    }
    return source->role != NULL && source->path != NULL;
}

static int test_make_warnings(HWAProductionResult *result)
{
    size_t index;
    result->warning_count =
        hwa_production_warning_spec_count(result);
    result->warnings = result->warning_count == 0U ? NULL :
        (HWAProductionWarning *)calloc(
            result->warning_count, sizeof(*result->warnings));
    if (result->warning_count != 0U && result->warnings == NULL) return 0;
    for (index = 0U; index < result->warning_count; ++index) {
        HWAProductionWarningSpec spec;
        HWAProductionWarning *warning = &result->warnings[index];
        if (hwa_production_warning_spec_at(
                result, index, &spec) != 0) return 0;
        warning->id = (uint64_t)index + 1U;
        warning->code = test_copy(spec.code);
        warning->message = test_copy(spec.message);
        warning->span_id = spec.span_id;
        warning->fit_id = spec.fit_id;
        warning->span_id_valid = spec.span_id_valid;
        warning->fit_id_valid = spec.fit_id_valid;
        if (warning->code == NULL || warning->message == NULL) return 0;
    }
    return 1;
}

static char *test_key(HWAProductionSplit wanted)
{
    unsigned candidate;
    for (candidate = 0U; candidate < 10000U; ++candidate) {
        char text[32];
        HWAProductionSplit split;
        int length = snprintf(
            text, sizeof(text), "report:%04u", candidate);
        if (length < 0 || (size_t)length >= sizeof(text)) return NULL;
        if (hwa_production_split_for_item_key(text, &split) == 0 &&
            split == wanted) return test_copy(text);
    }
    return NULL;
}

static int test_make_result(HWAProductionResult *result)
{
    size_t metric_count = hwa_production_metric_catalog_count();
    size_t per_span =
        ((size_t)HWA_PRODUCTION_VIEW_COUNT - 1U) * metric_count;
    size_t index;
    memset(result, 0, sizeof(*result));
    hwa_production_options_default(&result->options);
    result->profile_method.fft_size = 8192U;
    result->profile_method.hop_size = 2048U;
    result->profile_method.pitch_confidence_floor = 0.8;
    result->profile_method.spectral_floor_dbfs = -100.0;
    result->profile_method.max_partials = 16U;
    result->source_count = 4U;
    result->span_count = 2U;
    result->fit_count = hwa_production_fit_catalog_count();
    result->evaluation_row_count = result->span_count * per_span;
    result->sources = (HWAProductionSource *)calloc(
        result->source_count, sizeof(*result->sources));
    result->spans = (HWAProductionSpan *)calloc(
        result->span_count, sizeof(*result->spans));
    result->fits = (HWAProductionFit *)calloc(
        result->fit_count, sizeof(*result->fits));
    result->evaluations = (HWAProductionEvaluation *)calloc(
        result->evaluation_row_count, sizeof(*result->evaluations));
    if (result->sources == NULL || result->spans == NULL ||
        result->fits == NULL || result->evaluations == NULL ||
        !test_source(&result->sources[0], 1U, "reference:profile",
                     "/reference-profile.hwa", 'a', 0) ||
        !test_source(&result->sources[1], 2U, "reference:audio",
                     "/reference-\xff\n.wav", 'b', 1) ||
        !test_source(&result->sources[2], 3U, "model:profile",
                     "/model-profile.hwa", 'c', 0) ||
        !test_source(&result->sources[3], 4U, "model:audio",
                     "/model.wav", 'd', 1)) return 0;
    for (index = 0U; index < 2U; ++index) {
        HWAProductionSpan *span = &result->spans[index];
        span->id = (uint64_t)index + 1U;
        span->split = index == 0U ?
            HWA_PRODUCTION_TRAIN : HWA_PRODUCTION_CHECK;
        span->item_key = test_key(span->split);
        span->item_role = test_copy("tail");
        span->item_kind = HWA_ITEM_RELEASE;
        span->reference_item_id = (uint64_t)index + 1U;
        span->reference_end_sample = 2400U;
        span->model_item_id = (uint64_t)index + 101U;
        span->model_end_sample = 2400U;
        span->eligibility_flags = HWA_PRODUCTION_SPAN_DECAY;
        if (span->item_key == NULL || span->item_role == NULL) return 0;
    }
    for (index = 0U; index < result->fit_count; ++index) {
        HWAProductionFit *fit = &result->fits[index];
        fit->id = (uint64_t)index + 1U;
        if (hwa_production_fit_catalog_at(
                index, &fit->scope, &fit->kind,
                &fit->index, &fit->unit) != 0) return 0;
        fit->availability = HWA_PRODUCTION_UNAVAILABLE;
        if (fit->scope != HWA_PRODUCTION_SCOPE_ROOM_IR &&
            hwa_production_fit_eligibility_flag(fit->kind) ==
                HWA_PRODUCTION_SPAN_DECAY) fit->span_count = 1U;
    }
    if (!test_make_warnings(result)) return 0;
    for (index = 0U; index < result->evaluation_row_count; ++index) {
        size_t span_index = index / per_span;
        size_t within = index % per_span;
        size_t metric = within % metric_count;
        HWAProductionEvaluation *row = &result->evaluations[index];
        HWAProductionView view = (HWAProductionView)(
            (int)HWA_PRODUCTION_VIEW_RAW +
            (int)(within / metric_count));
        if (view == HWA_PRODUCTION_VIEW_RAW) {
            row->id = (uint64_t)index + 1U;
            row->span_id = (uint64_t)span_index + 1U;
            row->view = view;
            if (hwa_production_metric_catalog_at(
                    metric, &row->kind, &row->index,
                    &row->unit) != 0) return 0;
            row->availability = HWA_PRODUCTION_UNAVAILABLE;
            row->evidence_flags = span_index == 1U ?
                HWA_PRODUCTION_EVIDENCE_HELD_OUT : 0U;
        } else if (hwa_production_evaluation_derive(
                       result, span_index, view, metric,
                       row, NULL, 0U) != 0) {
            return 0;
        }
    }
    return hwa_production_result_retained_bytes(
               result, &result->retained_work_bytes) == 0 &&
           hwa_production_view_rows_rebuild(result, NULL, 0U) == 0;
}

static char *test_capture(
    int (*report)(FILE *, const HWAProductionResult *),
    const HWAProductionResult *result)
{
    FILE *stream = tmpfile();
    long length;
    char *text;
    if (stream == NULL || report(stream, result) != 0 ||
        fflush(stream) != 0 || fseek(stream, 0L, SEEK_END) != 0) {
        if (stream != NULL) (void)fclose(stream);
        return NULL;
    }
    length = ftell(stream);
    if (length < 0 || fseek(stream, 0L, SEEK_SET) != 0) {
        (void)fclose(stream);
        return NULL;
    }
    text = (char *)malloc((size_t)length + 1U);
    if (text == NULL ||
        fread(text, 1U, (size_t)length, stream) != (size_t)length) {
        free(text);
        (void)fclose(stream);
        return NULL;
    }
    text[length] = '\0';
    (void)fclose(stream);
    return text;
}

static void test_reports(void)
{
    HWAProductionResult result;
    char expected[2048];
    char *text;
    char *json;
    int length;
    FILE *read_only;
    CHECK(test_make_result(&result), "make report fixture");
    text = test_capture(hwa_report_production_text, &result);
    json = test_capture(hwa_report_production_json, &result);
    length = snprintf(
        expected, sizeof(expected),
        "Production account\n"
        "Reference audio: /reference-\\xff\\x0a.wav\n"
        "Model audio: /model.wav\n"
        "Room IR: none\n"
        "Source profile method: stage4-1 (FFT 8192, hop 2048)\n"
        "Spans: 2 (train 1, check 1)\n"
        "Fits: 61 (available 0, unavailable 61, insufficient 0)\n"
        "Evaluation rows: 174\n"
        "Check dry-like: available 0, survives 0\n"
        "Check room-matched: available 0, survives 0\n"
        "Warnings: 4\n"
        "Work: 0 evaluations, %" PRIu64 " retained bytes\n"
        "Warning details:\n"
        "  low-eq-evidence: EQ correction has fewer than 8 TRAIN spans "
        "or 2 CHECK spans.\n"
        "  low-dynamics-evidence: Dynamics correction has fewer than 8 "
        "TRAIN spans or 2 CHECK spans.\n"
        "  low-stereo-evidence: Stereo correction has fewer than 8 TRAIN "
        "spans or 2 CHECK spans.\n"
        "  low-decay-evidence: Decay facts have fewer than 8 TRAIN spans "
        "or 2 CHECK spans.\n",
        result.retained_work_bytes);
    CHECK(text != NULL && length > 0 &&
          (size_t)length < sizeof(expected) &&
          strcmp(text, expected) == 0, "exact short text report");
    CHECK(json != NULL &&
          strncmp(json,
                  "{\"schema_version\":8,"
                  "\"command\":\"account-production\",",
                  strlen("{\"schema_version\":8,"
                         "\"command\":\"account-production\",")) == 0,
          "JSON schema 8 prefix");
    CHECK(json != NULL &&
          strstr(json, "\"source_profile_method\":{") != NULL &&
          strstr(json, "\"sources\":[") != NULL &&
          strstr(json, "\"spans\":[") != NULL &&
          strstr(json, "\"fits\":[") != NULL &&
          strstr(json, "\"evaluations\":[") != NULL &&
          strstr(json, "\"views\":[") != NULL &&
          strstr(json, "\"warnings\":[") != NULL,
          "JSON includes every result section");
    CHECK(json != NULL &&
          strstr(json, "\\u00ff\\n.wav") != NULL &&
          strstr(json, "\"path_bytes_hex\":"
                       "\"2f7265666572656e63652dff0a2e776176\"") != NULL,
          "JSON preserves unsafe path bytes");
    CHECK(json != NULL && strlen(json) > 2U &&
          json[strlen(json) - 2U] == '}' &&
          json[strlen(json) - 1U] == '\n',
          "JSON closes with one newline");
    CHECK(hwa_report_production_text(NULL, &result) != 0,
          "text report rejects null stream");
    CHECK(hwa_report_production_json(NULL, &result) != 0,
          "JSON report rejects null stream");
    CHECK(hwa_report_production_text(stdout, NULL) != 0,
          "text report rejects null result");
    read_only = fopen(__FILE__, "rb");
    if (read_only != NULL) {
        CHECK(hwa_report_production_json(read_only, &result) != 0,
              "JSON report returns broken-stream error");
        (void)fclose(read_only);
    }
    free(json);
    free(text);
    hwa_production_result_free(&result);
}

int main(void)
{
    test_reports();
    if (failures != 0) {
        (void)fprintf(stderr, "%d Stage 6 report tests failed\n", failures);
        return 1;
    }
    (void)puts("Stage 6 report tests passed");
    return 0;
}

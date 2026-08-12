#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "experiment.h"
#include "experiment_report.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#define CHECK(condition, message)                                          \
    do { if (!(condition)) {                                               \
        (void)fprintf(stderr, "FAIL: %s\n", message); return 1;            \
    } } while (0)

static char *read_stream(FILE *stream)
{
    long size;
    char *text;
    if (fflush(stream) != 0 || fseek(stream, 0L, SEEK_END) != 0 ||
        (size = ftell(stream)) < 0 || fseek(stream, 0L, SEEK_SET) != 0) {
        return NULL;
    }
    text = (char *)malloc((size_t)size + 1U);
    if (text == NULL ||
        fread(text, 1U, (size_t)size, stream) != (size_t)size) {
        free(text);
        return NULL;
    }
    text[size] = '\0';
    return text;
}

static char *copy_text(const char *text)
{
    size_t size = strlen(text) + 1U;
    char *copy = (char *)malloc(size);
    if (copy != NULL) memcpy(copy, text, size);
    return copy;
}

static char *job_result_path(const char key[HWA_SHA256_HEX_SIZE])
{
    char text[96];
    int length = snprintf(text, sizeof(text), "jobs/%s/result.hwa-run", key);
    return length < 0 || (size_t)length >= sizeof(text) ? NULL : copy_text(text);
}

static char *job_artifact_path(const char key[HWA_SHA256_HEX_SIZE],
                               const char *name)
{
    char text[256];
    int length = snprintf(text, sizeof(text), "jobs/%s/%s", key, name);
    return length < 0 || (size_t)length >= sizeof(text) ? NULL : copy_text(text);
}

static void fill_hash(char hash[HWA_SHA256_HEX_SIZE], char digit)
{
    memset(hash, digit, HWA_SHA256_HEX_SIZE - 1U);
    hash[HWA_SHA256_HEX_SIZE - 1U] = '\0';
}

static void copy_hash(char hash[HWA_SHA256_HEX_SIZE], const char *text)
{
    memcpy(hash, text, HWA_SHA256_HEX_SIZE);
}

static int make_result(HWAExperimentResult *r)
{
    char error[128];
    memset(r, 0, sizeof(*r));
    hwa_experiment_options_default(&r->options);
    r->plan_kind = HWA_EXPERIMENT_ONE_AT_A_TIME;
    r->plan_seed = 17U;
    r->plan_replicates = 1U;
    r->manifest_path = copy_text(".");
    r->output_directory = copy_text(".");
    r->renderer_id = copy_text("process");
    fill_hash(r->manifest_sha256, 'a');
    fill_hash(r->renderer_sha256, 'b');
    r->input_count = 1U;
    r->parameter_count = 1U;
    r->level_count = 1U;
    r->case_count = 2U;
    r->response_count = 1U;
    r->point_count = 1U;
    r->value_count = 1U;
    r->job_count = 2U;
    r->artifact_count = 2U;
    r->observation_count = 2U;
    r->inputs = (HWAExperimentInput *)calloc(1U, sizeof(*r->inputs));
    r->parameters = (HWAExperimentParameter *)calloc(
        1U, sizeof(*r->parameters));
    r->levels = (HWAExperimentLevel *)calloc(1U, sizeof(*r->levels));
    r->cases = (HWAExperimentCase *)calloc(2U, sizeof(*r->cases));
    r->responses = (HWAExperimentResponse *)calloc(
        1U, sizeof(*r->responses));
    r->points = (HWAExperimentPoint *)calloc(1U, sizeof(*r->points));
    r->values = (HWAExperimentValue *)calloc(1U, sizeof(*r->values));
    r->jobs = (HWAExperimentJob *)calloc(2U, sizeof(*r->jobs));
    r->artifacts = (HWAExperimentArtifact *)calloc(2U, sizeof(*r->artifacts));
    r->observations = (HWAExperimentObservation *)calloc(
        2U, sizeof(*r->observations));
    if (r->manifest_path == NULL || r->output_directory == NULL ||
        r->renderer_id == NULL || r->inputs == NULL ||
        r->parameters == NULL || r->levels == NULL || r->cases == NULL ||
        r->responses == NULL || r->points == NULL || r->values == NULL ||
        r->jobs == NULL || r->artifacts == NULL ||
        r->observations == NULL) return -1;
    r->inputs[0].id = 1U;
    r->inputs[0].binding_id = copy_text("artist");
    r->inputs[0].path = copy_text(".");
    fill_hash(r->inputs[0].sha256, 'c');
    r->inputs[0].file_bytes = 2U;
    r->parameters[0].id = 1U;
    r->parameters[0].name = copy_text("gain");
    r->parameters[0].unit = copy_text("ratio");
    r->parameters[0].maximum = 1.0;
    r->parameters[0].level_count = 1U;
    r->levels[0].id = 1U;
    r->levels[0].parameter_id = 1U;
    r->cases[0].id = 1U;
    r->cases[0].name = copy_text("check-case");
    r->cases[0].split = HWA_EXPERIMENT_CHECK;
    r->cases[0].weight = 1.0;
    r->cases[1].id = 2U;
    r->cases[1].name = copy_text("fit-case");
    r->cases[1].split = HWA_EXPERIMENT_FIT;
    r->cases[1].weight = 1.0;
    r->responses[0].id = 1U;
    r->responses[0].name = copy_text("final.rms");
    r->responses[0].role = HWA_RUN_STEM_FINAL;
    r->responses[0].feature = HWA_RUN_FEATURE_RMS_DBFS;
    r->points[0].id = 1U;
    copy_hash(r->points[0].key,
              "a553129766af4ff435d4d170333bdf2da0d4c5974b1f4c301e035e0ce183099e");
    r->points[0].baseline = 1;
    r->values[0].id = 1U;
    r->values[0].point_id = 1U;
    r->values[0].parameter_id = 1U;
    r->jobs[0].id = 1U;
    copy_hash(r->jobs[0].key,
              "69a9e07731560edfe8f8d303a772615c8b489b8e69111b1434e2352bbcc49026");
    r->jobs[0].point_id = 1U;
    r->jobs[0].case_id = 1U;
    r->jobs[0].seed = UINT64_C(9260656408219841379);
    r->jobs[0].run_result_path = job_result_path(r->jobs[0].key);
    fill_hash(r->jobs[0].run_manifest_sha256, 'f');
    fill_hash(r->jobs[0].run_result_sha256, '1');
    r->jobs[0].output_bytes = 3U;
    r->jobs[0].run_evaluations = 11U;
    r->jobs[1].id = 2U;
    copy_hash(r->jobs[1].key,
              "88900cf0cfd714727aeeeb2ecfa3a2173e796c1a1c687f2c146f23906bd09f5e");
    r->jobs[1].point_id = 1U;
    r->jobs[1].case_id = 2U;
    r->jobs[1].seed = UINT64_C(10495416878414257626);
    r->jobs[1].run_result_path = job_result_path(r->jobs[1].key);
    fill_hash(r->jobs[1].run_manifest_sha256, '3');
    fill_hash(r->jobs[1].run_result_sha256, '4');
    r->jobs[1].output_bytes = 5U;
    r->jobs[1].run_evaluations = 13U;
    r->artifacts[0].id = 1U;
    r->artifacts[0].job_id = 1U;
    r->artifacts[0].resource_id = copy_text("model-final");
    r->artifacts[0].path = job_artifact_path(r->jobs[0].key, "model.wav");
    fill_hash(r->artifacts[0].sha256, '5');
    r->artifacts[0].file_bytes = 3U;
    r->artifacts[0].kind = HWA_RUN_SOURCE_STEM;
    r->artifacts[1].id = 2U;
    r->artifacts[1].job_id = 2U;
    r->artifacts[1].resource_id = copy_text("model-final");
    r->artifacts[1].path = job_artifact_path(r->jobs[1].key, "model.wav");
    fill_hash(r->artifacts[1].sha256, '6');
    r->artifacts[1].file_bytes = 5U;
    r->artifacts[1].kind = HWA_RUN_SOURCE_STEM;
    r->rendered_job_count = 2U;
    r->total_run_evaluations = 24U;
    r->total_output_bytes = 8U;
    r->observations[0].id = 1U;
    r->observations[0].job_id = 1U;
    r->observations[0].response_id = 1U;
    r->observations[0].availability = HWA_RUN_AVAILABLE;
    r->observations[0].value = 0.6;
    r->observations[0].value_valid = 1;
    r->observations[1].id = 2U;
    r->observations[1].job_id = 2U;
    r->observations[1].response_id = 1U;
    r->observations[1].availability = HWA_RUN_AVAILABLE;
    r->observations[1].value = 0.5;
    r->observations[1].value_valid = 1;
    if (r->inputs[0].binding_id == NULL || r->inputs[0].path == NULL ||
        r->parameters[0].name == NULL || r->parameters[0].unit == NULL ||
        r->cases[0].name == NULL || r->cases[1].name == NULL ||
        r->responses[0].name == NULL ||
        r->jobs[0].run_result_path == NULL ||
        r->jobs[1].run_result_path == NULL ||
        r->artifacts[0].resource_id == NULL ||
        r->artifacts[0].path == NULL ||
        r->artifacts[1].resource_id == NULL ||
        r->artifacts[1].path == NULL ||
        hwa_experiment_derived_rebuild(r, error, sizeof(error)) != 0 ||
        hwa_experiment_result_retained_bytes(
            r, &r->retained_work_bytes) != 0 ||
        hwa_experiment_result_validate(r, error, sizeof(error)) != 0) {
        return -1;
    }
    return 0;
}

/*
 * The engine test owns the full valid fixture. This test checks the report
 * failure contract without forging a result that could skip engine rules.
 */
int main(void)
{
    HWAExperimentResult result;
    FILE *stream = tmpfile();
    char *text;
    char *canonical_json = NULL;
    memset(&result, 0, sizeof(result));
    CHECK(stream != NULL, "tmpfile");
    CHECK(hwa_report_experiment_json(stream, &result) != 0,
          "JSON accepted an invalid result");
    CHECK(hwa_report_experiment_text(stream, &result) != 0,
          "text accepted an invalid result");
    text = read_stream(stream);
    CHECK(text != NULL && text[0] == '\0',
          "invalid result wrote partial report bytes");
    free(text);
    CHECK(fclose(stream) == 0, "close");
    CHECK(make_result(&result) == 0, "valid fixture");
    stream = tmpfile();
    CHECK(stream != NULL &&
              hwa_report_experiment_json(stream, &result) == 0,
          "valid JSON report");
    text = read_stream(stream);
    CHECK(text != NULL &&
              strstr(text, "\"schema_version\":10") != NULL &&
              strstr(text, "\"command\":\"experiment\"") != NULL &&
              strstr(text, "\"candidates\":[") != NULL &&
              strstr(text, "\"sensitivities\":[") != NULL &&
              strstr(text, "\"interactions\":[") != NULL &&
              strstr(text, "\"warnings\":[") != NULL &&
              strstr(text, "\"artifacts\":[{") != NULL &&
              strstr(text, "\"response_range\":") != NULL &&
              strstr(text, "\"effect_fraction\":null") != NULL &&
              strstr(text, "manifest_path") == NULL &&
              strstr(text, "output_directory") == NULL &&
              strstr(text, "duration_milliseconds") == NULL &&
              strstr(text, "\"reused\"") == NULL,
          "JSON report schema or canonical projection");
    canonical_json = text;
    CHECK(fclose(stream) == 0, "close JSON");
    {
        static const char *const locales[] = {
            "de_DE.UTF-8", "de_DE", "fr_FR.UTF-8", "fr_FR"
        };
        const char *saved = setlocale(LC_NUMERIC, NULL);
        char *saved_copy = saved == NULL ? NULL : copy_text(saved);
        size_t index;
        for (index = 0U; index < sizeof(locales) / sizeof(locales[0]);
             ++index) {
            if (setlocale(LC_NUMERIC, locales[index]) != NULL) break;
        }
        if (index < sizeof(locales) / sizeof(locales[0])) {
            stream = tmpfile();
            CHECK(stream != NULL &&
                      hwa_report_experiment_json(stream, &result) == 0,
                  "JSON report under non-C locale");
            text = read_stream(stream);
            CHECK(text != NULL && canonical_json != NULL &&
                      strcmp(text, canonical_json) == 0,
                  "numeric locale changed JSON");
            free(text);
            CHECK(fclose(stream) == 0, "close locale JSON");
        }
        if (saved_copy != NULL) {
            CHECK(setlocale(LC_NUMERIC, saved_copy) != NULL,
                  "restore locale");
        }
        free(saved_copy);
    }
    free(result.manifest_path);
    free(result.output_directory);
    free(result.inputs[0].path);
    result.manifest_path = copy_text("/host/a/experiment.json");
    result.output_directory = copy_text("/host/a/result-bundle");
    result.resume_directory = copy_text("/host/b/old-bundle");
    result.inputs[0].path = copy_text("/host/c/artist.wav");
    CHECK(result.manifest_path != NULL && result.output_directory != NULL &&
              result.resume_directory != NULL &&
              result.inputs[0].path != NULL &&
              hwa_experiment_result_retained_bytes(
                  &result, &result.retained_work_bytes) == 0 &&
              hwa_experiment_result_validate(
                  &result, NULL, 0U) == 0,
          "runtime path fixture");
    stream = tmpfile();
    CHECK(stream != NULL &&
              hwa_report_experiment_json(stream, &result) == 0,
          "runtime-path JSON report");
    text = read_stream(stream);
    CHECK(text != NULL && canonical_json != NULL &&
              strcmp(text, canonical_json) == 0,
          "runtime paths changed canonical JSON");
    free(text);
    CHECK(fclose(stream) == 0, "close runtime-path JSON");
    stream = tmpfile();
    CHECK(stream != NULL &&
              hwa_report_experiment_text(stream, &result) == 0,
          "valid text report");
    text = read_stream(stream);
    CHECK(text != NULL &&
              strstr(text, "Candidate ") != NULL &&
              strstr(text, "Sensitivity ") != NULL &&
              strstr(text, ", range ") != NULL &&
              strstr(text, "causal") != NULL,
          "text report omitted core facts or causal warning");
    free(text);
    CHECK(fclose(stream) == 0, "close text");
#if !defined(_WIN32)
    stream = tmpfile();
    CHECK(stream != NULL && setvbuf(stream, NULL, _IONBF, 0U) == 0,
          "fault stream");
    CHECK(close(fileno(stream)) == 0 &&
              hwa_report_experiment_json(stream, &result) != 0,
          "JSON report ignored an output fault");
    (void)fclose(stream);
#endif
    free(canonical_json);
    hwa_experiment_result_free(&result);
    return 0;
}

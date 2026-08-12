#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "experiment.h"
#include "experiment_file.h"

#include <inttypes.h>
#include <math.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

#define CHECK(condition, message)                                          \
    do { if (!(condition)) {                                               \
        (void)fprintf(stderr, "FAIL: %s\n", message); return 1;            \
    } } while (0)

static int make_path(char path[256])
{
#if defined(_WIN32)
    static unsigned serial;
    const char *temporary = getenv("TEMP");
    unsigned attempt;
    if (temporary == NULL || temporary[0] == '\0') temporary = ".";
    for (attempt = 0U; attempt < 100U; ++attempt) {
        int descriptor;
        int written = snprintf(
            path, 256U, "%s/hwa-stage8-persistence-%lu-%u.tmp", temporary,
            (unsigned long)_getpid(), serial++);
        if (written < 0 || written >= 256) return -1;
        descriptor = _open(path, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY,
                           _S_IREAD | _S_IWRITE);
        if (descriptor >= 0) {
            if (_close(descriptor) != 0 || _unlink(path) != 0) return -1;
            return 0;
        }
    }
    return -1;
#else
    int descriptor;
    (void)snprintf(path, 256U, "/tmp/hwa-stage8-persistence-XXXXXX");
    descriptor = mkstemp(path);
    if (descriptor < 0) return -1;
    if (close(descriptor) != 0 || unlink(path) != 0) return -1;
    return 0;
#endif
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

static int make_result(HWAExperimentResult *result)
{
    char error[256];
    memset(result, 0, sizeof(*result));
    hwa_experiment_options_default(&result->options);
    result->plan_kind = HWA_EXPERIMENT_ONE_AT_A_TIME;
    result->plan_seed = 17U;
    result->plan_replicates = 1U;
    result->manifest_path = copy_text(".");
    result->output_directory = copy_text(".");
    result->renderer_id = copy_text("process");
    fill_hash(result->manifest_sha256, 'a');
    fill_hash(result->renderer_sha256, 'b');

    result->input_count = 1U;
    result->inputs = (HWAExperimentInput *)calloc(
        result->input_count, sizeof(*result->inputs));
    result->parameter_count = 1U;
    result->parameters = (HWAExperimentParameter *)calloc(
        result->parameter_count, sizeof(*result->parameters));
    result->level_count = 1U;
    result->levels = (HWAExperimentLevel *)calloc(
        result->level_count, sizeof(*result->levels));
    result->case_count = 2U;
    result->cases = (HWAExperimentCase *)calloc(
        result->case_count, sizeof(*result->cases));
    result->response_count = 1U;
    result->responses = (HWAExperimentResponse *)calloc(
        result->response_count, sizeof(*result->responses));
    result->point_count = 1U;
    result->points = (HWAExperimentPoint *)calloc(
        result->point_count, sizeof(*result->points));
    result->value_count = 1U;
    result->values = (HWAExperimentValue *)calloc(
        result->value_count, sizeof(*result->values));
    result->job_count = 2U;
    result->jobs = (HWAExperimentJob *)calloc(
        result->job_count, sizeof(*result->jobs));
    result->artifact_count = 2U;
    result->artifacts = (HWAExperimentArtifact *)calloc(
        result->artifact_count, sizeof(*result->artifacts));
    result->observation_count = 2U;
    result->observations = (HWAExperimentObservation *)calloc(
        result->observation_count, sizeof(*result->observations));
    if (result->manifest_path == NULL || result->output_directory == NULL ||
        result->renderer_id == NULL || result->inputs == NULL ||
        result->parameters == NULL || result->levels == NULL ||
        result->cases == NULL || result->responses == NULL ||
        result->points == NULL || result->values == NULL ||
        result->jobs == NULL || result->artifacts == NULL ||
        result->observations == NULL) return -1;

    result->inputs[0].id = 1U;
    result->inputs[0].binding_id = copy_text("artist");
    result->inputs[0].path = copy_text(".");
    fill_hash(result->inputs[0].sha256, 'c');
    result->inputs[0].file_bytes = 2U;
    result->parameters[0].id = 1U;
    result->parameters[0].name = copy_text("gain");
    result->parameters[0].unit = copy_text("ratio");
    result->parameters[0].minimum = 0.0;
    result->parameters[0].maximum = 1.0;
    result->parameters[0].baseline = 0.0;
    result->parameters[0].first_level = 0U;
    result->parameters[0].level_count = 1U;
    result->levels[0].id = 1U;
    result->levels[0].parameter_id = 1U;
    result->levels[0].value = 0.0;
    result->cases[0].id = 1U;
    result->cases[0].name = copy_text("check-case");
    result->cases[0].split = HWA_EXPERIMENT_CHECK;
    result->cases[0].weight = 1.0;
    result->cases[1].id = 2U;
    result->cases[1].name = copy_text("fit-case");
    result->cases[1].split = HWA_EXPERIMENT_FIT;
    result->cases[1].weight = 1.0;
    result->responses[0].id = 1U;
    result->responses[0].name = copy_text("final.rms");
    result->responses[0].role = HWA_RUN_STEM_FINAL;
    result->responses[0].feature = HWA_RUN_FEATURE_RMS_DBFS;
    result->points[0].id = 1U;
    copy_hash(result->points[0].key,
              "a553129766af4ff435d4d170333bdf2da0d4c5974b1f4c301e035e0ce183099e");
    result->points[0].baseline = 1;
    result->values[0].id = 1U;
    result->values[0].point_id = 1U;
    result->values[0].parameter_id = 1U;
    result->values[0].value = 0.0;
    result->jobs[0].id = 1U;
    copy_hash(result->jobs[0].key,
              "69a9e07731560edfe8f8d303a772615c8b489b8e69111b1434e2352bbcc49026");
    result->jobs[0].point_id = 1U;
    result->jobs[0].case_id = 1U;
    result->jobs[0].seed = UINT64_C(9260656408219841379);
    result->jobs[0].run_result_path = job_result_path(result->jobs[0].key);
    fill_hash(result->jobs[0].run_manifest_sha256, 'f');
    fill_hash(result->jobs[0].run_result_sha256, '1');
    result->jobs[0].output_bytes = 3U;
    result->jobs[0].run_evaluations = 11U;
    result->jobs[1].id = 2U;
    copy_hash(result->jobs[1].key,
              "88900cf0cfd714727aeeeb2ecfa3a2173e796c1a1c687f2c146f23906bd09f5e");
    result->jobs[1].point_id = 1U;
    result->jobs[1].case_id = 2U;
    result->jobs[1].seed = UINT64_C(10495416878414257626);
    result->jobs[1].run_result_path = job_result_path(result->jobs[1].key);
    fill_hash(result->jobs[1].run_manifest_sha256, '3');
    fill_hash(result->jobs[1].run_result_sha256, '4');
    result->jobs[1].output_bytes = 5U;
    result->jobs[1].run_evaluations = 13U;
    result->artifacts[0].id = 1U;
    result->artifacts[0].job_id = 1U;
    result->artifacts[0].resource_id = copy_text("model-final");
    result->artifacts[0].path =
        job_artifact_path(result->jobs[0].key, "model.wav");
    fill_hash(result->artifacts[0].sha256, '5');
    result->artifacts[0].file_bytes = 3U;
    result->artifacts[0].kind = HWA_RUN_SOURCE_STEM;
    result->artifacts[1].id = 2U;
    result->artifacts[1].job_id = 2U;
    result->artifacts[1].resource_id = copy_text("model-final");
    result->artifacts[1].path =
        job_artifact_path(result->jobs[1].key, "model.wav");
    fill_hash(result->artifacts[1].sha256, '6');
    result->artifacts[1].file_bytes = 5U;
    result->artifacts[1].kind = HWA_RUN_SOURCE_STEM;
    result->rendered_job_count = 2U;
    result->total_run_evaluations = 24U;
    result->total_output_bytes = 8U;
    result->observations[0].id = 1U;
    result->observations[0].job_id = 1U;
    result->observations[0].response_id = 1U;
    result->observations[0].availability = HWA_RUN_AVAILABLE;
    result->observations[0].value = 0.6;
    result->observations[0].value_valid = 1;
    result->observations[1].id = 2U;
    result->observations[1].job_id = 2U;
    result->observations[1].response_id = 1U;
    result->observations[1].availability = HWA_RUN_AVAILABLE;
    result->observations[1].value = 0.5;
    result->observations[1].value_valid = 1;
    if (result->inputs[0].binding_id == NULL ||
        result->inputs[0].path == NULL ||
        result->parameters[0].name == NULL ||
        result->parameters[0].unit == NULL ||
        result->cases[0].name == NULL || result->cases[1].name == NULL ||
        result->responses[0].name == NULL ||
        result->jobs[0].run_result_path == NULL ||
        result->jobs[1].run_result_path == NULL ||
        result->artifacts[0].resource_id == NULL ||
        result->artifacts[0].path == NULL ||
        result->artifacts[1].resource_id == NULL ||
        result->artifacts[1].path == NULL ||
        hwa_experiment_derived_rebuild(result, error, sizeof(error)) != 0 ||
        hwa_experiment_result_retained_bytes(
            result, &result->retained_work_bytes) != 0 ||
        hwa_experiment_result_validate(result, error, sizeof(error)) != 0) {
        return -1;
    }
    return 0;
}

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *stream = fopen(path, "rb");
    long length;
    unsigned char *data;
    if (stream == NULL || fseek(stream, 0L, SEEK_END) != 0 ||
        (length = ftell(stream)) < 0 || fseek(stream, 0L, SEEK_SET) != 0) {
        if (stream != NULL) (void)fclose(stream);
        return NULL;
    }
    data = (unsigned char *)malloc((size_t)length + 1U);
    if (data == NULL ||
        fread(data, 1U, (size_t)length, stream) != (size_t)length ||
        fclose(stream) != 0) {
        free(data);
        return NULL;
    }
    data[length] = 0U;
    *size = (size_t)length;
    return data;
}

static int write_file(const char *path,
                      const unsigned char *data,
                      size_t size)
{
    FILE *stream = fopen(path, "wb");
    if (stream == NULL) return -1;
    if (fwrite(data, 1U, size, stream) != size ||
        fflush(stream) != 0 || fclose(stream) != 0) {
        return -1;
    }
    return 0;
}

static unsigned char *replace_span(const unsigned char *data,
                                   size_t size,
                                   size_t start,
                                   size_t end,
                                   const char *replacement,
                                   size_t *new_size)
{
    size_t replacement_size = strlen(replacement);
    size_t removed;
    unsigned char *copy;
    if (start > end || end > size) return NULL;
    removed = end - start;
    if (replacement_size > SIZE_MAX - (size - removed)) return NULL;
    *new_size = size - removed + replacement_size;
    copy = (unsigned char *)malloc(*new_size + 1U);
    if (copy == NULL) return NULL;
    memcpy(copy, data, start);
    memcpy(copy + start, replacement, replacement_size);
    memcpy(copy + start + replacement_size, data + end, size - end);
    copy[*new_size] = 0U;
    return copy;
}

static unsigned char *replace_row_field(const unsigned char *data,
                                        size_t size,
                                        const char *row_prefix,
                                        size_t comma_before,
                                        const char *replacement,
                                        size_t *new_size)
{
    const char *row = strstr((const char *)data, row_prefix);
    const char *cursor;
    const char *start = NULL;
    const char *end = NULL;
    size_t commas = 0U;
    if (row == NULL) return NULL;
    for (cursor = row; *cursor != '\0' && *cursor != '\r'; ++cursor) {
        if (*cursor == ',') {
            commas++;
            if (commas == comma_before) start = cursor + 1;
            else if (commas == comma_before + 1U) {
                end = cursor;
                break;
            }
        }
    }
    if (start == NULL) return NULL;
    if (end == NULL) end = cursor;
    return replace_span(data, size, (size_t)(start - (const char *)data),
                        (size_t)(end - (const char *)data), replacement,
                        new_size);
}

static unsigned char *replace_prefixed_field(const unsigned char *data,
                                             size_t size,
                                             const char *prefix,
                                             const char *replacement,
                                             size_t *new_size)
{
    const char *start = strstr((const char *)data, prefix);
    const char *end;
    if (start == NULL) return NULL;
    start += strlen(prefix);
    end = strchr(start, ',');
    if (end == NULL) return NULL;
    return replace_span(data, size, (size_t)(start - (const char *)data),
                        (size_t)(end - (const char *)data), replacement,
                        new_size);
}

static int corrupt_row_field_byte(unsigned char *data,
                                  const char *row_prefix,
                                  size_t comma_before)
{
    char *row = strstr((char *)data, row_prefix);
    char *cursor;
    size_t commas = 0U;
    if (row == NULL) return -1;
    for (cursor = row; *cursor != '\0' && *cursor != '\r'; ++cursor) {
        if (*cursor == ',') {
            commas++;
            if (commas == comma_before) {
                if (cursor[1] == '\0' || cursor[1] == ',' ||
                    cursor[1] == '\r') return -1;
                cursor[1] = cursor[1] == '7' ? '6' : '7';
                return 0;
            }
        }
    }
    return -1;
}

static int files_equal(const char *left, const char *right)
{
    size_t left_size = 0U;
    size_t right_size = 0U;
    unsigned char *left_bytes = read_file(left, &left_size);
    unsigned char *right_bytes = read_file(right, &right_size);
    int equal = left_bytes != NULL && right_bytes != NULL &&
                left_size == right_size &&
                memcmp(left_bytes, right_bytes, left_size) == 0;
    free(left_bytes);
    free(right_bytes);
    return equal;
}

static int corrupt_candidate(unsigned char *data)
{
    char *row = strstr((char *)data, "CANDIDATE,");
    size_t comma = 0U;
    char *cursor;
    if (row == NULL) return -1;
    for (cursor = row; *cursor != '\0' && *cursor != '\r'; ++cursor) {
        if (*cursor == ',') {
            comma++;
            if (comma == 6U) {
                if (cursor[1] != '0' || cursor[2] != '.') return -1;
                cursor[3] = cursor[3] == '9' ? '8' : '9';
                return 0;
            }
        }
    }
    return -1;
}

int main(void)
{
    HWAExperimentOptions limits;
    HWAExperimentResult result;
    HWAExperimentResult loaded;
    char path[256];
    char second[256];
    char third[256];
    char hash[HWA_SHA256_HEX_SIZE];
    char error[256];
    FILE *stream;
    hwa_experiment_options_default(&limits);
    memset(&result, 0, sizeof(result));
    memset(&loaded, 0, sizeof(loaded));
    CHECK(make_path(path) == 0, "path");
    stream = tmpfile();
    CHECK(stream != NULL, "tmpfile");
    CHECK(hwa_experiment_file_write(stream, &result,
                                    error, sizeof(error)) != 0,
          "writer accepted an invalid result");
    CHECK(ftell(stream) == 0L, "invalid result wrote partial bytes");
    CHECK(fclose(stream) == 0, "close");
    CHECK(hwa_experiment_file_write_path(path, &result,
                                         error, sizeof(error)) != 0,
          "path writer accepted an invalid result");
    CHECK(fopen(path, "rb") == NULL, "failed writer left a file");
    CHECK(hwa_experiment_file_read(path, &limits, &result, hash,
                                   error, sizeof(error)) != 0,
          "reader accepted a missing result");
    CHECK(hash[0] == '\0', "failed reader kept a hash");
    CHECK(result.inputs == NULL && result.input_count == 0U,
          "failed reader kept rows");
    CHECK(make_result(&result) == 0, "valid fixture");
    CHECK(make_path(path) == 0 && make_path(second) == 0, "roundtrip paths");
    CHECK(hwa_experiment_file_write_path(path, &result,
                                         error, sizeof(error)) == 0,
          "write valid result");
    {
        HWAExperimentOptions saved = result.options;
        FILE *too_small = tmpfile();
        CHECK(too_small != NULL, "small-cap stream");
        result.options.max_bundle_bytes = result.total_output_bytes + 1U;
        result.options.max_output_file_bytes = result.total_output_bytes + 1U;
        CHECK(hwa_experiment_file_write(too_small, &result,
                                        error, sizeof(error)) != 0 &&
                  ftell(too_small) == 0L,
              "capped FILE writer emitted partial bytes");
        CHECK(fclose(too_small) == 0, "close small-cap stream");
        CHECK(make_path(third) == 0 &&
                  hwa_experiment_file_write_path(
                      third, &result, error, sizeof(error)) != 0 &&
                  fopen(third, "rb") == NULL,
              "capped path writer left a file");
        result.options = saved;
    }
    CHECK(hwa_experiment_file_read(path, &limits, &loaded, hash,
                                   error, sizeof(error)) == 0,
          "read valid result");
    CHECK(strcmp(loaded.manifest_path, ".") == 0 &&
              strcmp(loaded.output_directory, ".") == 0 &&
              loaded.resume_directory == NULL &&
              strcmp(loaded.inputs[0].path, ".") == 0 &&
              loaded.total_duration_milliseconds == 0U &&
              loaded.reused_job_count == 0U &&
              loaded.rendered_job_count == loaded.job_count,
          "reader did not return canonical operational fields");
    CHECK(hwa_experiment_file_write_path(second, &loaded,
                                         error, sizeof(error)) == 0,
          "rewrite loaded result");
    {
        size_t first_size = 0U;
        size_t second_size = 0U;
        unsigned char *first_bytes = read_file(path, &first_size);
        unsigned char *second_bytes = read_file(second, &second_size);
        CHECK(first_bytes != NULL && second_bytes != NULL &&
                  first_size == second_size &&
                  memcmp(first_bytes, second_bytes, first_size) == 0,
              "reader/writer roundtrip changed canonical bytes");
        CHECK(strstr((const char *)first_bytes, "HWA_EXPERIMENT,1\r\n") != NULL &&
                  strstr((const char *)first_bytes, "manifest_path") == NULL &&
                  strstr((const char *)first_bytes, "output_directory") == NULL &&
                  strstr((const char *)first_bytes, "resume_directory") == NULL &&
                  strstr((const char *)first_bytes,
                         "duration_milliseconds") == NULL,
              "canonical bytes retained operational facts");
        free(first_bytes);
        free(second_bytes);
    }
    {
        HWAExperimentOptions wider = limits;
        HWAExperimentResult wider_loaded;
        HWAExperimentResult wider_roundtrip;
        memset(&wider_loaded, 0, sizeof(wider_loaded));
        memset(&wider_roundtrip, 0, sizeof(wider_roundtrip));
        wider.run.max_evaluations++;
        wider.max_input_bytes++;
        wider.max_jobs++;
        CHECK(remove(second) == 0 &&
                  hwa_experiment_file_read(
                      path, &wider, &wider_loaded, hash,
                      error, sizeof(error)) == 0 &&
                  wider_loaded.options.run.max_evaluations ==
                      wider.run.max_evaluations &&
                  wider_loaded.options.max_input_bytes ==
                      wider.max_input_bytes &&
                  wider_loaded.options.max_jobs == wider.max_jobs,
              "reader did not apply wider current caps");
        CHECK(hwa_experiment_file_write_path(
                  second, &wider_loaded, error, sizeof(error)) == 0 &&
                  hwa_experiment_file_read(
                      second, &wider, &wider_roundtrip, hash,
                      error, sizeof(error)) == 0 &&
                  make_path(third) == 0 &&
                  hwa_experiment_file_write_path(
                      third, &wider_roundtrip, error, sizeof(error)) == 0 &&
                  files_equal(second, third),
              "wider current caps did not reach a canonical roundtrip");
        CHECK(remove(third) == 0, "remove wider-cap roundtrip");
        hwa_experiment_result_free(&wider_roundtrip);
        hwa_experiment_result_free(&wider_loaded);
    }
    {
        uint64_t retained;
        CHECK(hwa_experiment_result_retained_bytes(&loaded, &retained) == 0 &&
                  retained == loaded.retained_work_bytes,
              "reader retained ledger mismatch");
    }
    {
        HWAExperimentOptions boundary = limits;
        HWAExperimentResult boundary_loaded;
        uint64_t low = UINT64_C(1);
        uint64_t high = limits.max_work_bytes;
        while (low < high) {
            uint64_t middle = low + (high - low) / UINT64_C(2);
            memset(&boundary_loaded, 0, sizeof(boundary_loaded));
            boundary.max_work_bytes = middle;
            if (hwa_experiment_file_read(
                    path, &boundary, &boundary_loaded, hash,
                    error, sizeof(error)) == 0) {
                hwa_experiment_result_free(&boundary_loaded);
                high = middle;
            } else {
                low = middle + UINT64_C(1);
            }
        }
        memset(&boundary_loaded, 0, sizeof(boundary_loaded));
        boundary.max_work_bytes = low;
        CHECK(hwa_experiment_file_read(
                  path, &boundary, &boundary_loaded, hash,
                  error, sizeof(error)) == 0,
              "reader rejected its exact peak work cap");
        hwa_experiment_result_free(&boundary_loaded);
        CHECK(low > UINT64_C(1), "reader peak work cap is not bounded");
        memset(&boundary_loaded, 0, sizeof(boundary_loaded));
        boundary.max_work_bytes = low - UINT64_C(1);
        CHECK(hwa_experiment_file_read(
                  path, &boundary, &boundary_loaded, hash,
                  error, sizeof(error)) != 0,
              "reader accepted one byte below its peak work cap");
    }
    {
        HWAExperimentOptions boundary = limits;
        HWAExperimentResult boundary_loaded;
        size_t size = 0U;
        unsigned char *bytes = read_file(path, &size);
        uint64_t exact;
        memset(&boundary_loaded, 0, sizeof(boundary_loaded));
        CHECK(bytes != NULL &&
                  (uint64_t)size <=
                      UINT64_MAX - result.total_output_bytes,
              "make bundle lower-bound cap");
        exact = (uint64_t)size + result.total_output_bytes;
        boundary.max_bundle_bytes = exact;
        CHECK(hwa_experiment_file_read(
                  path, &boundary, &boundary_loaded, hash,
                  error, sizeof(error)) == 0,
              "reader rejected the exact bundle lower bound");
        hwa_experiment_result_free(&boundary_loaded);
        boundary.max_bundle_bytes = exact - UINT64_C(1);
        CHECK(hwa_experiment_file_read(
                  path, &boundary, &boundary_loaded, hash,
                  error, sizeof(error)) != 0,
              "reader accepted one byte below the bundle lower bound");
        free(bytes);
    }
    {
        HWAExperimentOptions overflow_limits = limits;
        HWAExperimentResult overflow_loaded;
        size_t size = 0U;
        size_t first_size = 0U;
        size_t changed_size = 0U;
        unsigned char *bytes = read_file(path, &size);
        unsigned char *first = bytes == NULL ? NULL : replace_prefixed_field(
            bytes, size, "META,max_bundle_bytes,",
            "18446744073709551615", &first_size);
        unsigned char *changed = first == NULL ? NULL : replace_prefixed_field(
            first, first_size, "META,total_output_bytes,",
            "18446744073709551615", &changed_size);
        memset(&overflow_loaded, 0, sizeof(overflow_loaded));
        overflow_limits.max_bundle_bytes = UINT64_MAX;
        CHECK(changed != NULL && make_path(third) == 0 &&
                  write_file(third, changed, changed_size) == 0,
              "write overflowing bundle lower bound");
        free(bytes);
        free(first);
        free(changed);
        CHECK(hwa_experiment_file_read(
                  third, &overflow_limits, &overflow_loaded, hash,
                  error, sizeof(error)) != 0,
              "reader accepted an overflowing bundle lower bound");
        CHECK(remove(third) == 0,
              "remove overflowing bundle lower bound");
    }
    {
        size_t size = 0U;
        size_t changed_size = 0U;
        unsigned char *bytes = read_file(path, &size);
        unsigned char *changed;
        char retained[32];
        int length = snprintf(retained, sizeof(retained), "%" PRIu64,
                              loaded.retained_work_bytes + UINT64_C(1));
        CHECK(bytes != NULL && length > 0 &&
                  (size_t)length < sizeof(retained),
              "make ABI ledger value");
        changed = replace_prefixed_field(
            bytes, size, "META,retained_work_bytes,", retained,
            &changed_size);
        CHECK(changed != NULL && make_path(third) == 0 &&
                  write_file(third, changed, changed_size) == 0,
              "write producer ABI ledger");
        free(bytes);
        free(changed);
        hwa_experiment_result_free(&loaded);
        CHECK(hwa_experiment_file_read(third, &limits, &loaded, hash,
                                       error, sizeof(error)) == 0 &&
                  loaded.retained_work_bytes + UINT64_C(1) ==
                      (uint64_t)strtoull(retained, NULL, 10),
              "reader did not replace the producer ABI ledger");
        CHECK(remove(third) == 0 &&
                  hwa_experiment_file_write_path(
                      third, &loaded, error, sizeof(error)) == 0 &&
                  files_equal(path, third),
              "ABI ledger did not normalize to canonical bytes");
        CHECK(remove(third) == 0, "remove ABI ledger file");
    }
    {
        size_t before_size = 0U;
        size_t after_size = 0U;
        unsigned char *before = read_file(path, &before_size);
        unsigned char *after;
        CHECK(before != NULL &&
                  hwa_experiment_file_write_path(path, &result,
                                                 error, sizeof(error)) != 0,
              "exclusive writer replaced an existing result");
        after = read_file(path, &after_size);
        CHECK(after != NULL && before_size == after_size &&
                  memcmp(before, after, before_size) == 0,
              "failed exclusive write changed existing bytes");
        free(before);
        free(after);
    }
    hwa_experiment_result_free(&loaded);
    hwa_experiment_options_default(&limits);
    limits.max_jobs = 1U;
    CHECK(hwa_experiment_file_read(path, &limits, &loaded, hash,
                                   error, sizeof(error)) != 0 &&
              hash[0] == '\0',
          "reader ignored the current job cap");
    hwa_experiment_options_default(&limits);
    limits.max_input_bytes = 1U;
    CHECK(hwa_experiment_file_read(path, &limits, &loaded, hash,
                                   error, sizeof(error)) != 0 &&
              hash[0] == '\0',
          "reader ignored the current input byte cap");
    hwa_experiment_options_default(&limits);
    limits.max_output_file_bytes = 4U;
    CHECK(hwa_experiment_file_read(path, &limits, &loaded, hash,
                                   error, sizeof(error)) != 0 &&
              hash[0] == '\0',
          "reader ignored the current artifact byte cap");
    hwa_experiment_options_default(&limits);
    limits.run.max_evaluations = 12U;
    CHECK(hwa_experiment_file_read(path, &limits, &loaded, hash,
                                   error, sizeof(error)) != 0 &&
              hash[0] == '\0',
          "reader ignored the current per-run evaluation cap");
    hwa_experiment_options_default(&limits);
    {
        size_t size = 0U;
        size_t changed_size = 0U;
        unsigned char *bytes = read_file(path, &size);
        unsigned char *changed = bytes == NULL ? NULL :
            replace_prefixed_field(bytes, size, "META,max_input_bytes,", "1",
                                   &changed_size);
        CHECK(changed != NULL && make_path(third) == 0 &&
                  write_file(third, changed, changed_size) == 0,
              "write saved-cap result");
        free(bytes);
        free(changed);
    }
    CHECK(hwa_experiment_file_read(third, &limits, &loaded, hash,
                                   error, sizeof(error)) != 0 &&
              hash[0] == '\0',
          "reader ignored the saved producer input cap");
    CHECK(remove(third) == 0, "remove saved-cap result");
    {
        size_t size = 0U;
        unsigned char *bytes = read_file(path, &size);
        unsigned char *crlf = bytes == NULL ? NULL :
            (unsigned char *)strstr((char *)bytes, "\r\n");
        CHECK(crlf != NULL && make_path(third) == 0,
              "make line-ending corruption");
        crlf[0] = '\n';
        CHECK(write_file(third, bytes, size) == 0,
              "write line-ending corruption");
        free(bytes);
    }
    CHECK(hwa_experiment_file_read(third, &limits, &loaded, hash,
                                   error, sizeof(error)) != 0,
          "reader accepted noncanonical line endings");
    CHECK(remove(third) == 0, "remove line-ending corruption");
    {
        size_t size = 0U;
        size_t changed_size = 0U;
        unsigned char *bytes = read_file(path, &size);
        unsigned char *changed = bytes == NULL ? NULL : replace_row_field(
            bytes, size, "POINT,", 2U,
            "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
            &changed_size);
        CHECK(changed != NULL && make_path(third) == 0 &&
                  write_file(third, changed, changed_size) == 0,
              "write hostile point key");
        free(bytes);
        free(changed);
    }
    CHECK(hwa_experiment_file_read(third, &limits, &loaded, hash,
                                   error, sizeof(error)) != 0,
          "reader accepted a false point key");
    CHECK(remove(third) == 0, "remove hostile point key");
    {
        size_t size = 0U;
        size_t changed_size = 0U;
        unsigned char *bytes = read_file(path, &size);
        unsigned char *changed = bytes == NULL ? NULL : replace_row_field(
            bytes, size, "JOB,", 6U, "1", &changed_size);
        CHECK(changed != NULL && make_path(third) == 0 &&
                  write_file(third, changed, changed_size) == 0,
              "write hostile job seed");
        free(bytes);
        free(changed);
    }
    CHECK(hwa_experiment_file_read(third, &limits, &loaded, hash,
                                   error, sizeof(error)) != 0,
          "reader accepted a false job seed");
    CHECK(remove(third) == 0, "remove hostile job seed");
    {
        size_t size = 0U;
        unsigned char *bytes = read_file(path, &size);
        CHECK(bytes != NULL &&
                  corrupt_row_field_byte(bytes, "JOB,", 7U) == 0 &&
                  make_path(third) == 0 && write_file(third, bytes, size) == 0,
              "write hostile job path");
        free(bytes);
    }
    CHECK(hwa_experiment_file_read(third, &limits, &loaded, hash,
                                   error, sizeof(error)) != 0,
          "reader accepted a noncanonical job path");
    CHECK(remove(third) == 0, "remove hostile job path");
    {
        size_t size = 0U;
        unsigned char *bytes = read_file(path, &size);
        CHECK(bytes != NULL &&
                  corrupt_row_field_byte(bytes, "ARTIFACT,", 4U) == 0 &&
                  make_path(third) == 0 && write_file(third, bytes, size) == 0,
              "write hostile artifact path");
        free(bytes);
    }
    CHECK(hwa_experiment_file_read(third, &limits, &loaded, hash,
                                   error, sizeof(error)) != 0,
          "reader accepted a noncanonical artifact path");
    CHECK(remove(third) == 0, "remove hostile artifact path");
    {
        size_t size = 0U;
        size_t changed_size = 0U;
        unsigned char *bytes = read_file(path, &size);
        unsigned char *changed = bytes == NULL ? NULL : replace_row_field(
            bytes, size, "OBSERVATION,", 5U, "1.1", &changed_size);
        CHECK(changed != NULL && make_path(third) == 0 &&
                  write_file(third, changed, changed_size) == 0,
              "write hostile normalized gap");
        free(bytes);
        free(changed);
    }
    CHECK(hwa_experiment_file_read(third, &limits, &loaded, hash,
                                   error, sizeof(error)) != 0,
          "reader accepted a normalized gap above one");
    CHECK(remove(third) == 0, "remove hostile normalized gap");
    {
        size_t size = 0U;
        size_t changed_size = 0U;
        unsigned char *bytes = read_file(path, &size);
        unsigned char *changed = bytes == NULL ? NULL : replace_row_field(
            bytes, size, "OBSERVATION,", 6U, "32", &changed_size);
        CHECK(changed != NULL && make_path(third) == 0 &&
                  write_file(third, changed, changed_size) == 0,
              "write hostile observation flags");
        free(bytes);
        free(changed);
    }
    CHECK(hwa_experiment_file_read(third, &limits, &loaded, hash,
                                   error, sizeof(error)) != 0,
          "reader accepted unknown observation quality flags");
    CHECK(remove(third) == 0, "remove hostile observation flags");
    {
        size_t size = 0U;
        size_t changed_size = 0U;
        unsigned char *bytes = read_file(path, &size);
        unsigned char *changed = bytes == NULL ? NULL : replace_row_field(
            bytes, size, "JOB,", 10U, "4", &changed_size);
        CHECK(changed != NULL && make_path(third) == 0 &&
                  write_file(third, changed, changed_size) == 0,
              "write hostile job output ledger");
        free(bytes);
        free(changed);
    }
    CHECK(hwa_experiment_file_read(third, &limits, &loaded, hash,
                                   error, sizeof(error)) != 0,
          "reader accepted a false per-job output ledger");
    CHECK(remove(third) == 0, "remove hostile job output ledger");
    {
        size_t size = 0U;
        size_t changed_size = 0U;
        unsigned char *bytes = read_file(path, &size);
        unsigned char *changed;
        char near_value[64];
        int length = snprintf(near_value, sizeof(near_value), "%.17g",
                              nextafter(0.5, 1.0));
        CHECK(bytes != NULL && length > 0 &&
                  (size_t)length < sizeof(near_value),
              "make nearby derived value");
        changed = replace_row_field(bytes, size, "CANDIDATE,", 6U,
                                    near_value, &changed_size);
        CHECK(changed != NULL && make_path(third) == 0 &&
                  write_file(third, changed, changed_size) == 0,
              "write nearby derived value");
        free(bytes);
        free(changed);
    }
    CHECK(hwa_experiment_file_read(third, &limits, &loaded, hash,
                                   error, sizeof(error)) == 0,
          "reader rejected allowed derived drift");
    CHECK(remove(third) == 0 &&
              hwa_experiment_file_write_path(
                  third, &loaded, error, sizeof(error)) == 0 &&
              files_equal(path, third),
          "reader did not normalize allowed derived drift");
    CHECK(remove(third) == 0, "remove normalized derived file");
    hwa_experiment_result_free(&loaded);
    CHECK(make_path(third) == 0, "hostile path");
    {
        size_t size = 0U;
        unsigned char *bytes = read_file(path, &size);
        CHECK(bytes != NULL && corrupt_candidate(bytes) == 0 &&
                  write_file(third, bytes, size) == 0,
              "make hostile derived result");
        free(bytes);
    }
    CHECK(hwa_experiment_file_read(third, &limits, &loaded, hash,
                                   error, sizeof(error)) != 0 &&
              hash[0] == '\0',
          "reader accepted a hostile derived value");
    CHECK(remove(third) == 0, "remove hostile file");
    free(result.manifest_path);
    free(result.output_directory);
    free(result.inputs[0].path);
    result.manifest_path = copy_text("/moved/manifest.json");
    result.output_directory = copy_text("/moved/bundle");
    result.inputs[0].path = copy_text("/moved/artist.wav");
    result.jobs[0].duration_milliseconds = 9U;
    result.jobs[0].reused = 1;
    result.total_duration_milliseconds = 9U;
    result.rendered_job_count = 1U;
    result.reused_job_count = 1U;
    CHECK(result.manifest_path != NULL && result.output_directory != NULL &&
              result.inputs[0].path != NULL &&
              hwa_experiment_result_retained_bytes(
                  &result, &result.retained_work_bytes) == 0 &&
              hwa_experiment_result_validate(
                  &result, error, sizeof(error)) == 0 &&
              make_path(third) == 0 &&
              hwa_experiment_file_write_path(
                  third, &result, error, sizeof(error)) == 0,
          "write moved/resumed result");
    {
        size_t first_size = 0U;
        size_t moved_size = 0U;
        unsigned char *first = read_file(path, &first_size);
        unsigned char *moved = read_file(third, &moved_size);
        CHECK(first != NULL && moved != NULL && first_size == moved_size &&
                  memcmp(first, moved, first_size) == 0,
              "host paths or run logistics changed canonical bytes");
        free(first);
        free(moved);
    }
    CHECK(remove(third) == 0, "remove moved file");
    {
        static const char *const locales[] = {
            "de_DE.UTF-8", "de_DE", "fr_FR.UTF-8", "fr_FR"
        };
        const char *saved = setlocale(LC_NUMERIC, NULL);
        char *saved_copy = saved == NULL ? NULL : copy_text(saved);
        size_t locale_index;
        for (locale_index = 0U;
             locale_index < sizeof(locales) / sizeof(locales[0]);
             ++locale_index) {
            if (setlocale(LC_NUMERIC, locales[locale_index]) != NULL) break;
        }
        if (locale_index < sizeof(locales) / sizeof(locales[0])) {
            CHECK(make_path(third) == 0 &&
                      hwa_experiment_file_write_path(
                          third, &result, error, sizeof(error)) == 0,
                  "write under a non-C locale");
            {
                size_t first_size = 0U;
                size_t locale_size = 0U;
                unsigned char *first = read_file(path, &first_size);
                unsigned char *locale = read_file(third, &locale_size);
                CHECK(first != NULL && locale != NULL &&
                          first_size == locale_size &&
                          memcmp(first, locale, first_size) == 0,
                      "numeric locale changed canonical bytes");
                free(first);
                free(locale);
            }
            CHECK(remove(third) == 0, "remove locale file");
        }
        if (saved_copy != NULL) {
            CHECK(setlocale(LC_NUMERIC, saved_copy) != NULL,
                  "restore locale");
        }
        free(saved_copy);
    }
    CHECK(remove(path) == 0 && remove(second) == 0, "remove roundtrip files");
    hwa_experiment_result_free(&loaded);
    hwa_experiment_result_free(&result);
    return 0;
}

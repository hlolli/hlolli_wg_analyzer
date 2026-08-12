#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "hlolli_wg_analyzer.h"
#include "run.h"
#include "run_file.h"
#include "sha256.h"

#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#define HWA_TEST_PID _getpid
#define HWA_TEST_UNLINK _unlink
#else
#include <unistd.h>
#define HWA_TEST_PID getpid
#define HWA_TEST_UNLINK unlink
#endif

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
                     HWARunSide side,
                     HWARunStemRole role,
                     char hash_digit)
{
    memset(source, 0, sizeof(*source));
    source->id = id;
    source->binding_id = test_copy(binding);
    source->path = test_copy(
        id == 1U ? "/not-opened/body-\xff\n.wav" : "/not-opened/stem.wav");
    test_hash(source->sha256, hash_digit);
    source->kind = HWA_RUN_SOURCE_STEM;
    source->side = side;
    source->role = role;
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
    source->rate_numerator = 48000U;
    source->rate_denominator = 1U;
    source->file_bytes = 192044U;
    source->value_count = 48000U;
    return source->binding_id != NULL && source->path != NULL;
}

static int test_probe_source(HWARunSource *source)
{
    memset(source, 0, sizeof(*source));
    source->id = 3U;
    source->binding_id = test_copy("model.force");
    source->path = test_copy("/not-opened/probe.csv");
    test_hash(source->sha256, 'c');
    source->kind = HWA_RUN_SOURCE_PROBE;
    source->side = HWA_RUN_MODEL;
    source->probe_format = HWA_RUN_PROBE_CSV_F64;
    source->probe_name = test_copy("pm.excitation.force");
    source->unit = test_copy("si.N");
    source->rate_numerator = 100U;
    source->rate_denominator = 1U;
    source->file_bytes = 1200U;
    source->value_count = 100U;
    return source->binding_id != NULL && source->path != NULL &&
           source->probe_name != NULL && source->unit != NULL;
}

static int test_make_result(HWARunResult *result)
{
    static const double gaps[] = {0.0, 0.2, 0.4, 0.3, 0.1, 0.05};
    size_t clock_index;
    size_t feature_offset;
    memset(result, 0, sizeof(*result));
    hwa_run_options_default(&result->options);
    result->manifest_path = test_copy("/not-opened/run-\xfe.json");
    test_hash(result->manifest_sha256, 'f');
    result->clock_rate_hz = 48000U;
    result->source_count = 8U;
    result->clock_count = 6U;
    result->feature_count = result->clock_count *
                            hwa_run_feature_catalog_count();
    result->stage_count = hwa_run_stage_catalog_count();
    result->probe_count = 1U;
    result->link_count = 1U;
    result->sources = (HWARunSource *)calloc(
        result->source_count, sizeof(*result->sources));
    result->clocks = (HWARunClock *)calloc(
        result->clock_count, sizeof(*result->clocks));
    result->features = (HWARunFeature *)calloc(
        result->feature_count, sizeof(*result->features));
    result->stages = (HWARunStage *)calloc(
        result->stage_count, sizeof(*result->stages));
    result->probes = (HWARunProbe *)calloc(
        result->probe_count, sizeof(*result->probes));
    result->links = (HWARunLink *)calloc(
        result->link_count, sizeof(*result->links));
    if (result->manifest_path == NULL || result->sources == NULL ||
        result->clocks == NULL || result->features == NULL ||
        result->stages == NULL || result->probes == NULL ||
        result->links == NULL ||
        !test_stem(&result->sources[0], 1U, "model.body",
                   HWA_RUN_MODEL, HWA_RUN_STEM_BODY, 'a') ||
        !test_stem(&result->sources[1], 2U, "model.final",
                   HWA_RUN_MODEL, HWA_RUN_STEM_FINAL, 'b') ||
        !test_probe_source(&result->sources[2]) ||
        !test_stem(&result->sources[3], 4U, "model.noise",
                   HWA_RUN_MODEL, HWA_RUN_STEM_NOISE, 'd') ||
        !test_stem(&result->sources[4], 5U, "model.room",
                   HWA_RUN_MODEL, HWA_RUN_STEM_ROOM, 'e') ||
        !test_stem(&result->sources[5], 6U, "model.source",
                   HWA_RUN_MODEL, HWA_RUN_STEM_SOURCE, 'f') ||
        !test_stem(&result->sources[6], 7U, "model.wet",
                   HWA_RUN_MODEL, HWA_RUN_STEM_WET, 'a') ||
        !test_stem(&result->sources[7], 8U, "reference.final",
                   HWA_RUN_REFERENCE, HWA_RUN_STEM_FINAL, 'b')) return 0;
    for (clock_index = 0U; clock_index < result->clock_count; ++clock_index) {
        static const uint64_t model_ids[] = {6U, 1U, 7U, 2U, 5U, 4U};
        HWARunClock *clock = &result->clocks[clock_index];
        clock->id = (uint64_t)clock_index + 1U;
        clock->role = (HWARunStemRole)(clock_index + 1U);
        clock->reference_source_id = 8U;
        clock->model_source_id = model_ids[clock_index];
        clock->availability = HWA_RUN_AVAILABLE;
        clock->overlap_frames = 48000U;
    }
    for (clock_index = 0U; clock_index < result->clock_count; ++clock_index) {
        for (feature_offset = 0U;
             feature_offset < hwa_run_feature_catalog_count();
             ++feature_offset) {
            size_t offset = clock_index * hwa_run_feature_catalog_count() +
                            feature_offset;
            HWARunFeature *feature = &result->features[offset];
            feature->id = (uint64_t)offset + 1U;
            feature->clock_id = (uint64_t)clock_index + 1U;
            feature->role = (HWARunStemRole)(clock_index + 1U);
            if (hwa_run_feature_catalog_at(feature_offset, &feature->kind,
                                           &feature->index,
                                           &feature->unit) != 0) return 0;
            feature->availability = HWA_RUN_AVAILABLE;
            feature->reference_value = -20.0;
            feature->model_value = -20.0 + gaps[clock_index] * 12.0;
            feature->delta = feature->model_value - feature->reference_value;
            feature->normalized_gap = fmin(fabs(feature->delta) / 12.0, 1.0);
            feature->reference_valid = 1;
            feature->model_valid = 1;
            feature->delta_valid = 1;
            feature->gap_valid = 1;
        }
    }
    result->probes[0].id = 1U;
    result->probes[0].source_id = 3U;
    result->probes[0].availability = HWA_RUN_AVAILABLE;
    result->probes[0].value_count = 100U;
    result->probes[0].minimum = -1.0;
    result->probes[0].maximum = 2.0;
    result->probes[0].mean = 0.25;
    result->probes[0].population_sd = 0.5;
    result->probes[0].statistics_valid = 1;
    result->links[0].id = 1U;
    result->links[0].stem_source_id = 2U;
    result->links[0].probe_source_id = 3U;
    result->links[0].feature = HWA_RUN_FEATURE_RMS_DBFS;
    result->links[0].availability = HWA_RUN_AVAILABLE;
    result->links[0].lag_hops = 2;
    result->links[0].lag_samples = 960;
    result->links[0].correlation = 0.75;
    result->links[0].slope = 2.0;
    result->links[0].intercept = -30.0;
    result->links[0].r_squared = 0.5625;
    result->links[0].point_count = 64U;
    result->links[0].coverage = 64.0 / 99.0;
    result->links[0].fit_valid = 1;
    if (hwa_run_stage_rows_rebuild(result, NULL, 0U) != 0 ||
        hwa_run_warnings_rebuild(result, NULL, 0U) != 0 ||
        hwa_run_evaluations_expected(
            result, &result->evaluation_count) != 0 ||
        hwa_run_result_retained_bytes(
            result, &result->retained_work_bytes) != 0) return 0;
    return hwa_run_result_validate(result, NULL, 0U) == 0;
}

static unsigned char *test_read_file(const char *path, size_t *size)
{
    FILE *stream = fopen(path, "rb");
    long end;
    unsigned char *data;
    if (stream == NULL || fseek(stream, 0L, SEEK_END) != 0 ||
        (end = ftell(stream)) < 0L || fseek(stream, 0L, SEEK_SET) != 0) {
        if (stream != NULL) (void)fclose(stream);
        return NULL;
    }
    data = (unsigned char *)malloc((size_t)end + 1U);
    if (data == NULL ||
        fread(data, 1U, (size_t)end, stream) != (size_t)end ||
        fclose(stream) != 0) {
        free(data);
        return NULL;
    }
    data[(size_t)end] = 0U;
    *size = (size_t)end;
    return data;
}

static int test_write_bytes(const char *path,
                            const unsigned char *data,
                            size_t size)
{
    FILE *stream = fopen(path, "wb");
    return stream != NULL && fwrite(data, 1U, size, stream) == size &&
           fclose(stream) == 0;
}

static int test_write_result(const char *path, const HWARunResult *result)
{
    FILE *stream = fopen(path, "wb");
    char error[HWA_ERROR_SIZE];
    int status;
    if (stream == NULL) return 0;
    status = hwa_run_file_write(stream, result, error, sizeof(error));
    return fclose(stream) == 0 && status == 0;
}

static unsigned char *test_replace_once(const unsigned char *data,
                                        size_t size,
                                        const char *from,
                                        const char *to,
                                        size_t *new_size)
{
    const unsigned char *where = (const unsigned char *)strstr(
        (const char *)data, from);
    size_t prefix;
    size_t from_size = strlen(from);
    size_t to_size = strlen(to);
    unsigned char *changed;
    if (where == NULL || size < from_size ||
        size - from_size > SIZE_MAX - to_size) return NULL;
    prefix = (size_t)(where - data);
    *new_size = size - from_size + to_size;
    changed = (unsigned char *)malloc(*new_size + 1U);
    if (changed == NULL) return NULL;
    memcpy(changed, data, prefix);
    memcpy(changed + prefix, to, to_size);
    memcpy(changed + prefix + to_size, where + from_size,
           size - prefix - from_size);
    changed[*new_size] = 0U;
    return changed;
}

static unsigned char *test_replace_meta_value(const unsigned char *data,
                                              size_t size,
                                              const char *key,
                                              const char *value,
                                              size_t *new_size)
{
    char marker[128];
    int marker_size = snprintf(marker, sizeof(marker), "META,%s,", key);
    const unsigned char *start;
    const unsigned char *end;
    size_t prefix;
    size_t old_size;
    size_t value_size = strlen(value);
    unsigned char *changed;
    if (marker_size < 0 || (size_t)marker_size >= sizeof(marker)) return NULL;
    start = (const unsigned char *)strstr((const char *)data, marker);
    if (start == NULL) return NULL;
    start += (size_t)marker_size;
    end = (const unsigned char *)strchr((const char *)start, ',');
    if (end == NULL) return NULL;
    prefix = (size_t)(start - data);
    old_size = (size_t)(end - start);
    if (size < old_size || size - old_size > SIZE_MAX - value_size) return NULL;
    *new_size = size - old_size + value_size;
    changed = (unsigned char *)malloc(*new_size + 1U);
    if (changed == NULL) return NULL;
    memcpy(changed, data, prefix);
    memcpy(changed + prefix, value, value_size);
    memcpy(changed + prefix + value_size, end, size - prefix - old_size);
    changed[*new_size] = 0U;
    return changed;
}

static unsigned char *test_replace_row_field(const unsigned char *data,
                                             size_t size,
                                             const char *row_prefix,
                                             size_t field_index,
                                             const char *value,
                                             size_t *new_size)
{
    const unsigned char *start = (const unsigned char *)strstr(
        (const char *)data, row_prefix);
    const unsigned char *end;
    size_t index;
    size_t prefix;
    size_t old_size;
    size_t value_size = strlen(value);
    unsigned char *changed;
    if (start == NULL) return NULL;
    for (index = 0U; index < field_index; ++index) {
        start = (const unsigned char *)strchr((const char *)start, ',');
        if (start == NULL) return NULL;
        start++;
    }
    end = start;
    while ((size_t)(end - data) < size && *end != ',' && *end != '\r') end++;
    if ((size_t)(end - data) >= size) return NULL;
    prefix = (size_t)(start - data);
    old_size = (size_t)(end - start);
    if (size < old_size || size - old_size > SIZE_MAX - value_size) return NULL;
    *new_size = size - old_size + value_size;
    changed = (unsigned char *)malloc(*new_size + 1U);
    if (changed == NULL) return NULL;
    memcpy(changed, data, prefix);
    memcpy(changed + prefix, value, value_size);
    memcpy(changed + prefix + value_size, end, size - prefix - old_size);
    changed[*new_size] = 0U;
    return changed;
}

static int test_reject_row_field(const unsigned char *data,
                                 size_t size,
                                 const char *bad_path,
                                 const char *row_prefix,
                                 size_t field_index,
                                 const char *value,
                                 const HWARunOptions *limits,
                                 const char *label)
{
    HWARunResult loaded;
    char hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    size_t changed_size = 0U;
    unsigned char *changed = test_replace_row_field(
        data, size, row_prefix, field_index, value, &changed_size);
    int rejected = 0;
    memset(&loaded, 0, sizeof(loaded));
    if (changed != NULL && test_write_bytes(bad_path, changed, changed_size)) {
        rejected = hwa_run_file_read(bad_path, limits, &loaded, hash,
                                     error, sizeof(error)) != 0;
    }
    CHECK(changed != NULL, "cannot make hostile Stage 7 %s fixture", label);
    CHECK(rejected, "hostile Stage 7 %s fixture passed", label);
    hwa_run_result_free(&loaded);
    free(changed);
    return rejected;
}

static void test_accept_row_field(const unsigned char *data,
                                  size_t size,
                                  const char *path,
                                  const char *row_prefix,
                                  size_t field_index,
                                  const char *value,
                                  const HWARunOptions *limits,
                                  const char *label)
{
    HWARunResult loaded;
    char hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    unsigned char *changed;
    size_t changed_size = 0U;
    int status = -1;
    memset(&loaded, 0, sizeof(loaded));
    changed = test_replace_row_field(
        data, size, row_prefix, field_index, value, &changed_size);
    if (changed != NULL && test_write_bytes(path, changed, changed_size)) {
        status = hwa_run_file_read(path, limits, &loaded, hash,
                                   error, sizeof(error));
    }
    CHECK(changed != NULL, "cannot make valid Stage 7 %s fixture", label);
    CHECK(status == 0, "valid Stage 7 %s fixture was rejected: %s",
          label, error);
    hwa_run_result_free(&loaded);
    free(changed);
}

static unsigned char *test_replace_row_double_steps(
    const unsigned char *data,
    size_t size,
    const char *row_prefix,
    size_t field_index,
    unsigned int steps,
    size_t *new_size)
{
    const unsigned char *start = (const unsigned char *)strstr(
        (const char *)data, row_prefix);
    const unsigned char *end;
    char original[128];
    char replacement[128];
    char *parse_end;
    size_t index;
    size_t length;
    double value;
    if (start == NULL) return NULL;
    for (index = 0U; index < field_index; ++index) {
        start = (const unsigned char *)strchr((const char *)start, ',');
        if (start == NULL) return NULL;
        start++;
    }
    end = start;
    while ((size_t)(end - data) < size && *end != ',' && *end != '\r') end++;
    length = (size_t)(end - start);
    if ((size_t)(end - data) >= size || length == 0U ||
        length >= sizeof(original)) return NULL;
    memcpy(original, start, length);
    original[length] = '\0';
    value = strtod(original, &parse_end);
    if (parse_end != original + length || !isfinite(value)) return NULL;
    for (index = 0U; index < (size_t)steps; ++index) {
        value = nextafter(value, INFINITY);
    }
    if (!isfinite(value) ||
        snprintf(replacement, sizeof(replacement), "%.17g",
                 value == 0.0 ? 0.0 : value) < 0) return NULL;
    return test_replace_row_field(data, size, row_prefix, field_index,
                                  replacement, new_size);
}

static void test_ulp_case(const unsigned char *data,
                          size_t size,
                          const char *path,
                          const char *row_prefix,
                          size_t field_index,
                          unsigned int steps,
                          int should_pass,
                          const HWARunOptions *limits,
                          const char *label)
{
    HWARunResult loaded;
    char hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    unsigned char *changed;
    unsigned char *normalized = NULL;
    size_t changed_size = 0U;
    size_t normalized_size = 0U;
    int status = -1;
    memset(&loaded, 0, sizeof(loaded));
    changed = test_replace_row_double_steps(
        data, size, row_prefix, field_index, steps, &changed_size);
    if (changed != NULL && test_write_bytes(path, changed, changed_size)) {
        status = hwa_run_file_read(path, limits, &loaded, hash,
                                   error, sizeof(error));
    }
    CHECK(changed != NULL, "cannot make Stage 7 %s ULP fixture", label);
    CHECK(should_pass ? status == 0 : status != 0,
          "Stage 7 %s ULP fixture was %s", label,
          should_pass ? "rejected" : "accepted");
    if (should_pass && status == 0) {
        CHECK(test_write_result(path, &loaded),
              "cannot write normalized Stage 7 %s fixture", label);
        normalized = test_read_file(path, &normalized_size);
        CHECK(normalized != NULL && normalized_size == size &&
                  memcmp(normalized, data, size) == 0,
              "Stage 7 %s was not normalized to canonical bytes", label);
    }
    free(normalized);
    free(changed);
    hwa_run_result_free(&loaded);
}

static void test_preserve_valid_mutation(const unsigned char *data,
                                         size_t size,
                                         const char *path,
                                         const HWARunOptions *limits,
                                         const char *label)
{
    HWARunResult loaded;
    char hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    unsigned char *rewritten = NULL;
    size_t rewritten_size = 0U;
    int status;
    memset(&loaded, 0, sizeof(loaded));
    CHECK(test_write_bytes(path, data, size),
          "cannot write Stage 7 %s fixture", label);
    status = hwa_run_file_read(path, limits, &loaded, hash,
                               error, sizeof(error));
    CHECK(status == 0, "valid Stage 7 %s fixture was rejected: %s",
          label, error);
    if (status == 0) {
        CHECK(test_write_result(path, &loaded),
              "cannot rewrite Stage 7 %s fixture", label);
        rewritten = test_read_file(path, &rewritten_size);
        CHECK(rewritten != NULL && rewritten_size == size &&
                  memcmp(rewritten, data, size) == 0,
              "Stage 7 reader changed raw %s", label);
    }
    free(rewritten);
    hwa_run_result_free(&loaded);
}

static void test_reject_replacement(const unsigned char *data,
                                    size_t size,
                                    const char *bad_path,
                                    const char *from,
                                    const char *to,
                                    const HWARunOptions *limits,
                                    const char *label)
{
    HWARunResult loaded;
    char hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    size_t changed_size = 0U;
    unsigned char *changed = test_replace_once(
        data, size, from, to, &changed_size);
    memset(&loaded, 0, sizeof(loaded));
    CHECK(changed != NULL && test_write_bytes(bad_path, changed, changed_size),
          "cannot write hostile Stage 7 %s fixture", label);
    CHECK(changed != NULL &&
              hwa_run_file_read(bad_path, limits, &loaded, hash,
                                error, sizeof(error)) != 0,
          "hostile Stage 7 %s fixture passed", label);
    hwa_run_result_free(&loaded);
    free(changed);
}

static void test_round_trip(const char *first, const char *second)
{
    HWARunResult source;
    HWARunResult loaded;
    HWARunOptions limits;
    char hash[HWA_SHA256_HEX_SIZE];
    char expected_hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    unsigned char *a;
    unsigned char *b;
    size_t a_size = 0U;
    size_t b_size = 0U;
    CHECK(test_make_result(&source), "cannot make Stage 7 fixture");
    CHECK(test_write_result(first, &source), "cannot write Stage 7 fixture");
    a = test_read_file(first, &a_size);
    CHECK(a != NULL && a_size > 10U &&
              memcmp(a, "HWA_RUN,1\r\n", 11U) == 0,
          "canonical Stage 7 header is wrong");
    CHECK(a != NULL && a_size >= 2U && a[a_size - 2U] == '\r' &&
              a[a_size - 1U] == '\n',
          "canonical Stage 7 file lacks final CRLF");
    hwa_run_options_default(&limits);
    memset(&loaded, 0, sizeof(loaded));
    CHECK(hwa_run_file_read(first, &limits, &loaded, hash,
                            error, sizeof(error)) == 0,
          "cannot read canonical Stage 7 fixture: %s", error);
    CHECK(hwa_sha256_file(first, UINT64_MAX, expected_hash,
                          error, sizeof(error)) == 0 &&
              strcmp(hash, expected_hash) == 0,
          "Stage 7 reader hash does not name its parsed byte stream");
    CHECK(strlen(hash) == 64U && loaded.source_count == 8U &&
              loaded.clock_count == 6U && loaded.feature_count == 72U &&
              loaded.stage_count == 3U &&
              loaded.warning_count == 2U,
          "loaded Stage 7 row counts are wrong");
    {
        uint64_t retained = 0U;
        CHECK(hwa_run_result_retained_bytes(&loaded, &retained) == 0 &&
                  retained == loaded.retained_work_bytes,
              "Stage 7 reader did not store the current retained ledger");
    }
    CHECK(test_write_result(second, &loaded),
          "cannot rewrite Stage 7 fixture");
    b = test_read_file(second, &b_size);
    CHECK(a != NULL && b != NULL && a_size == b_size &&
              memcmp(a, b, a_size) == 0,
          "Stage 7 reader/writer round trip changed bytes");
#if !defined(_WIN32)
    {
        FILE *fault = fopen("/dev/full", "wb");
        if (fault != NULL) {
            CHECK(hwa_run_file_write(fault, &source,
                                     error, sizeof(error)) != 0 ||
                      fflush(fault) != 0,
                  "Stage 7 writer ignored an output fault");
            (void)fclose(fault);
        }
    }
#endif
    free(a);
    free(b);
    hwa_run_result_free(&loaded);
    hwa_run_result_free(&source);
}

static void test_caps_and_hostile(const char *good, const char *bad)
{
    HWARunOptions limits;
    HWARunResult loaded;
    char hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    unsigned char *data;
    unsigned char *changed;
    unsigned char *changed_again;
    size_t size = 0U;
    size_t changed_size = 0U;
    size_t changed_again_size = 0U;
    hwa_run_options_default(&limits);
    data = test_read_file(good, &size);
    CHECK(data != NULL, "cannot read good Stage 7 bytes");
    if (data == NULL) return;
    CHECK(hwa_run_file_read("-", &limits, &loaded, hash,
                            error, sizeof(error)) != 0,
          "Stage 7 reader accepted standard input authority");
#if !defined(_WIN32)
    (void)HWA_TEST_UNLINK(bad);
    CHECK(symlink(good, bad) == 0, "cannot make Stage 7 symlink fixture");
    CHECK(hwa_run_file_read(bad, &limits, &loaded, hash,
                            error, sizeof(error)) != 0,
          "Stage 7 reader followed a result symlink");
    (void)HWA_TEST_UNLINK(bad);
#endif
    limits.max_input_bytes = 192044U;
    memset(&loaded, 0, sizeof(loaded));
    CHECK(hwa_run_file_read(good, &limits, &loaded, hash,
                            error, sizeof(error)) == 0,
          "exact Stage 7 stem-byte cap failed: %s", error);
    hwa_run_result_free(&loaded);
    limits.max_input_bytes = 192043U;
    CHECK(hwa_run_file_read(good, &limits, &loaded, hash,
                            error, sizeof(error)) != 0,
          "one-under Stage 7 stem-byte cap passed");
    hwa_run_options_default(&limits);
    limits.max_input_bytes = 192044U;
    limits.max_work_bytes =
        ((uint64_t)size + UINT64_C(1)) * UINT64_C(3) - UINT64_C(1);
    CHECK(hwa_run_file_read(good, &limits, &loaded, hash,
                            error, sizeof(error)) != 0,
          "Stage 7 reader allocated a file past the live-work preflight");
    hwa_run_options_default(&limits);
    limits.max_input_bytes = 192044U;
    limits.max_work_bytes =
        ((uint64_t)size + UINT64_C(1)) * UINT64_C(3) +
        UINT64_C(8) * (uint64_t)sizeof(HWARunSource) +
        UINT64_C(6) * (uint64_t)sizeof(HWARunClock) +
        UINT64_C(72) * (uint64_t)sizeof(HWARunFeature) +
        UINT64_C(3) * (uint64_t)sizeof(HWARunStage) +
        (uint64_t)sizeof(HWARunProbe) +
        (uint64_t)sizeof(HWARunLink) +
        UINT64_C(2) * (uint64_t)sizeof(HWARunWarning);
    CHECK(hwa_run_file_read(good, &limits, &loaded, hash,
                            error, sizeof(error)) == 0,
          "exact Stage 7 reader-work cap failed: %s", error);
    hwa_run_result_free(&loaded);
    limits.max_work_bytes--;
    CHECK(hwa_run_file_read(good, &limits, &loaded, hash,
                            error, sizeof(error)) != 0,
          "one-under Stage 7 reader-work cap passed");
    hwa_run_options_default(&limits);
    limits.max_result_rows = 93U;
    CHECK(hwa_run_file_read(good, &limits, &loaded, hash,
                            error, sizeof(error)) == 0,
          "exact Stage 7 result-row cap failed: %s", error);
    hwa_run_result_free(&loaded);
    limits.max_result_rows = 92U;
    CHECK(hwa_run_file_read(good, &limits, &loaded, hash,
                            error, sizeof(error)) != 0,
          "one-under Stage 7 result-row cap passed");
    hwa_run_options_default(&limits);
    limits.max_input_bytes = 192044U;
    limits.max_input_frames = 48000U;
    limits.max_probe_bytes = 1200U;
    limits.max_probe_values = 100U;
    CHECK(hwa_run_file_read(good, &limits, &loaded, hash,
                            error, sizeof(error)) == 0,
          "exact Stage 7 source caps failed: %s", error);
    hwa_run_result_free(&loaded);
    limits.max_input_frames = 47999U;
    CHECK(hwa_run_file_read(good, &limits, &loaded, hash,
                            error, sizeof(error)) != 0,
          "one-under Stage 7 frame cap passed");
    limits.max_input_frames = 48000U;
    limits.max_probe_bytes = 1199U;
    CHECK(hwa_run_file_read(good, &limits, &loaded, hash,
                            error, sizeof(error)) != 0,
          "one-under Stage 7 probe-byte cap passed");
    limits.max_probe_bytes = 1200U;
    limits.max_probe_values = 99U;
    CHECK(hwa_run_file_read(good, &limits, &loaded, hash,
                            error, sizeof(error)) != 0,
          "one-under Stage 7 probe-value cap passed");
    limits.max_probe_values = 100U;
    limits.max_input_bytes = 192043U;
    CHECK(hwa_run_file_read(good, &limits, &loaded, hash,
                            error, sizeof(error)) != 0,
          "one-under Stage 7 stem-byte cap passed");
    hwa_run_options_default(&limits);
    limits.max_stems = 7U;
    limits.max_probes = 1U;
    CHECK(hwa_run_file_read(good, &limits, &loaded, hash,
                            error, sizeof(error)) == 0,
          "exact Stage 7 source-kind caps failed: %s", error);
    hwa_run_result_free(&loaded);
    limits.max_stems = 6U;
    CHECK(hwa_run_file_read(good, &limits, &loaded, hash,
                            error, sizeof(error)) != 0,
          "one-under Stage 7 stem-count cap passed");
    hwa_run_options_default(&limits);
    changed = test_replace_once(data, size, "HWA_RUN,1\r\n",
                                "HWA_RUN,01\r\n", &changed_size);
    CHECK(changed != NULL && test_write_bytes(bad, changed, changed_size),
          "cannot write noncanonical numeric fixture");
    CHECK(changed != NULL &&
              hwa_run_file_read(bad, &limits, &loaded, hash,
                                error, sizeof(error)) != 0,
          "noncanonical Stage 7 header passed");
    free(changed);
    changed = test_replace_once(data, size, "META,max_stems,16,stems\r\n",
                                "META,max_stems,1,stems\r\n",
                                &changed_size);
    CHECK(changed != NULL && test_write_bytes(bad, changed, changed_size),
          "cannot write false saved-cap fixture");
    CHECK(changed != NULL &&
              hwa_run_file_read(bad, &limits, &loaded, hash,
                                error, sizeof(error)) != 0,
          "Stage 7 reader trusted false saved caps");
    free(changed);
    changed = test_replace_meta_value(data, size, "retained_work_bytes", "0",
                                      &changed_size);
    CHECK(changed != NULL && test_write_bytes(bad, changed, changed_size),
          "cannot write zero saved-ledger fixture");
    CHECK(changed != NULL &&
              hwa_run_file_read(bad, &limits, &loaded, hash,
                                error, sizeof(error)) != 0,
          "Stage 7 reader accepted a zero saved retained ledger");
    free(changed);
    changed = test_replace_meta_value(data, size, "retained_work_bytes",
                                      "536870913", &changed_size);
    CHECK(changed != NULL && test_write_bytes(bad, changed, changed_size),
          "cannot write over-cap saved-ledger fixture");
    CHECK(changed != NULL &&
              hwa_run_file_read(bad, &limits, &loaded, hash,
                                error, sizeof(error)) != 0,
          "Stage 7 reader accepted an over-cap saved retained ledger");
    free(changed);
    changed = test_replace_once(data, size, "STAGE,1,source,body,available,",
                                "STAGE,1,source,body,available,0.1",
                                &changed_size);
    CHECK(changed != NULL && test_write_bytes(bad, changed, changed_size),
          "cannot write derived-stage corruption");
    CHECK(changed != NULL &&
              hwa_run_file_read(bad, &limits, &loaded, hash,
                                error, sizeof(error)) != 0,
          "corrupt derived Stage 7 stage passed");
    free(changed);
    changed = test_replace_once(data, size, "stage-cause-unproven",
                                "stage-cause-unproved", &changed_size);
    CHECK(changed != NULL && test_write_bytes(bad, changed, changed_size),
          "cannot write warning corruption");
    CHECK(changed != NULL &&
              hwa_run_file_read(bad, &limits, &loaded, hash,
                                error, sizeof(error)) != 0,
          "corrupt fixed Stage 7 warning passed");
    free(changed);
    changed = test_replace_once(data, size, "\r\nMETA", "\nMETA",
                                &changed_size);
    CHECK(changed != NULL && test_write_bytes(bad, changed, changed_size),
          "cannot write LF fixture");
    CHECK(changed != NULL &&
              hwa_run_file_read(bad, &limits, &loaded, hash,
                                error, sizeof(error)) != 0,
          "bare-LF Stage 7 file passed");
    free(changed);
    test_reject_replacement(
        data, size, bad,
        "META,feature_count,72,features\r\n",
        "META,feature_count,71,features\r\n",
        &limits, "count");
    test_reject_replacement(
        data, size, bad, "CLOCK,1,source,",
        "FEATURE,1,source,", &limits, "section");
    test_reject_replacement(
        data, size, bad, "SOURCE,2,model.final,",
        "SOURCE,1,model.final,", &limits, "duplicate-id");
    test_reject_replacement(
        data, size, bad, "LINK,1,2,3,rms_dbfs,",
        "LINK,1,99,3,rms_dbfs,", &limits, "broken-link");
    test_reject_replacement(
        data, size, bad, ",available,0,0,0,48000,0,",
        ",invalid,0,0,0,48000,0,", &limits, "enum");
    test_reject_replacement(
        data, size, bad, "META,max_stems,16,stems\r\n",
        "META,\"max_stems\",16,stems\r\n",
        &limits, "needless-quote");
    test_reject_replacement(
        data, size, bad, "META,max_probes,128,probes\r\n",
        "META,max_stems,128,probes\r\n",
        &limits, "duplicate-key");
    test_reject_replacement(
        data, size, bad, "SOURCE,1,model.body,",
        "SOURCE,1,zz.model.body,", &limits, "row-order");
    test_accept_row_field(data, size, bad, "META,tool_version,", 2U,
                          "0.7.9", &limits,
                          "cross-version tool provenance");
    test_accept_row_field(data, size, bad,
                          "META,build_compiler_family,", 2U,
                          "\"old,compiler\"", &limits,
                          "quoted cross-version build provenance");
    test_accept_row_field(data, size, bad, "META,max_evaluations,", 2U,
                          "100000000", &limits,
                          "older saved evaluation cap");
    (void)test_reject_row_field(data, size, bad,
                                "META,run_method_version,", 2U,
                                "stage7-0", &limits,
                                "run method version");
    (void)test_reject_row_field(data, size, bad,
                                "META,build_compiler_family,", 2U,
                                "", &limits,
                                "empty build provenance");
    (void)test_reject_row_field(data, size, bad, "SOURCE,1,model.body,",
                                2U, "bad binding", &limits,
                                "source binding grammar");
    (void)test_reject_row_field(data, size, bad, "SOURCE,1,model.body,",
                                7U, "final", &limits,
                                "source role catalog");
    (void)test_reject_row_field(data, size, bad, "SOURCE,1,model.body,",
                                13U, "47999", &limits,
                                "stem rate numerator");
    (void)test_reject_row_field(data, size, bad, "SOURCE,1,model.body,",
                                14U, "0", &limits,
                                "stem rate denominator");
    (void)test_reject_row_field(data, size, bad, "SOURCE,1,model.body,",
                                15U, "0", &limits,
                                "source file bytes");
    (void)test_reject_row_field(data, size, bad, "SOURCE,3,model.force,",
                                9U, "plain", &limits,
                                "probe name grammar");
    (void)test_reject_row_field(data, size, bad, "SOURCE,3,model.force,",
                                10U, "\"si,N\"", &limits,
                                "probe unit grammar");
    (void)test_reject_row_field(data, size, bad, "CLOCK,1,source,",
                                10U, "1", &limits,
                                "clock drift derivation");
    (void)test_reject_row_field(data, size, bad, "CLOCK,1,source,",
                                11U, "1", &limits,
                                "clock quality flags");
    (void)test_reject_row_field(data, size, bad, "FEATURE,1,1,source,",
                                10U, "1", &limits,
                                "feature delta derivation");
    (void)test_reject_row_field(data, size, bad, "FEATURE,1,1,source,",
                                11U, "0.5", &limits,
                                "feature gap derivation");
    (void)test_reject_row_field(data, size, bad, "FEATURE,1,1,source,",
                                12U, "1", &limits,
                                "feature quality flags");
    (void)test_reject_row_field(data, size, bad, "PROBE,1,3,available,",
                                4U, "99", &limits,
                                "probe summary count");
    (void)test_reject_row_field(data, size, bad, "PROBE,1,3,available,",
                                7U, "3", &limits,
                                "probe mean domain");
    (void)test_reject_row_field(data, size, bad, "PROBE,1,3,available,",
                                8U, "-1", &limits,
                                "probe standard deviation domain");
    (void)test_reject_row_field(data, size, bad, "LINK,1,2,3,rms_dbfs,",
                                8U, "959", &limits,
                                "link lag relation");
    (void)test_reject_row_field(data, size, bad, "LINK,1,2,3,rms_dbfs,",
                                9U, "nan", &limits,
                                "nonfinite link value");
    (void)test_reject_row_field(data, size, bad, "LINK,1,2,3,rms_dbfs,",
                                12U, "0.5", &limits,
                                "link R-squared derivation");
    (void)test_reject_row_field(data, size, bad, "LINK,1,2,3,rms_dbfs,",
                                15U, "4", &limits,
                                "link quality flags");
    (void)test_reject_row_field(data, size, bad, "LINK,1,2,3,rms_dbfs,",
                                14U, "1.1", &limits,
                                "link coverage domain");
    test_ulp_case(data, size, bad, "CLOCK,1,source,", 10U, 1U, 1,
                  &limits, "one-step clock drift");
    test_ulp_case(data, size, bad, "FEATURE,13,2,body,", 10U, 32U, 1,
                  &limits, "32-step feature delta");
    test_ulp_case(data, size, bad, "FEATURE,13,2,body,", 11U, 32U, 1,
                  &limits, "32-step feature gap");
    test_ulp_case(data, size, bad, "LINK,1,2,3,rms_dbfs,", 12U, 32U, 1,
                  &limits, "32-step link R-squared");
    test_ulp_case(data, size, bad, "LINK,1,2,3,rms_dbfs,", 14U, 32U, 1,
                  &limits, "32-step link coverage");
    test_ulp_case(data, size, bad, "LINK,1,2,3,rms_dbfs,", 14U, 33U, 0,
                  &limits, "33-step link coverage");
    test_ulp_case(data, size, bad, "STAGE,1,source,body,", 6U, 32U, 1,
                  &limits, "32-step stage gap");
    test_ulp_case(data, size, bad, "STAGE,1,source,body,", 6U, 33U, 0,
                  &limits, "33-step stage gap");
    changed = test_replace_row_double_steps(
        data, size, "FEATURE,1,1,source,", 8U, 1U, &changed_size);
    changed_again = changed != NULL
                        ? test_replace_row_double_steps(
                              changed, changed_size, "FEATURE,1,1,source,",
                              9U, 1U, &changed_again_size)
                        : NULL;
    CHECK(changed_again != NULL,
          "cannot make raw Stage 7 feature-value fixture");
    if (changed_again != NULL) {
        test_preserve_valid_mutation(changed_again, changed_again_size,
                                     bad, &limits, "feature values");
    }
    free(changed_again);
    free(changed);
    changed = test_replace_row_double_steps(
        data, size, "LINK,1,2,3,rms_dbfs,", 10U, 1U, &changed_size);
    CHECK(changed != NULL, "cannot make raw Stage 7 link-slope fixture");
    if (changed != NULL) {
        test_preserve_valid_mutation(changed, changed_size, bad, &limits,
                                     "link slope");
    }
    free(changed);
    free(data);
}

static void test_overlap_boundary(const char *path)
{
    HWARunResult source;
    HWARunResult loaded;
    HWARunOptions limits;
    char hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    unsigned char *data;
    size_t size = 0U;
    size_t index;
    CHECK(test_make_result(&source),
          "cannot make Stage 7 overlap boundary fixture");
    source.sources[5].start_sample = 24000;
    source.clocks[0].start_offset_samples = 24000;
    source.clocks[0].end_offset_samples = 24000;
    source.clocks[0].overlap_frames = 24000U;
    source.clocks[0].quality_flags = HWA_RUN_QUALITY_CLOCK_OFFSET;
    for (index = 0U; index < hwa_run_feature_catalog_count(); ++index) {
        source.features[index].quality_flags = HWA_RUN_QUALITY_CLOCK_OFFSET;
    }
    CHECK(hwa_run_evaluations_expected(
              &source, &source.evaluation_count) == 0 &&
              hwa_run_result_validate(&source, error, sizeof(error)) == 0,
          "exact-half Stage 7 overlap fixture is invalid: %s", error);
    CHECK(test_write_result(path, &source),
          "cannot write exact-half Stage 7 overlap fixture");
    data = test_read_file(path, &size);
    hwa_run_options_default(&limits);
    memset(&loaded, 0, sizeof(loaded));
    CHECK(data != NULL &&
              hwa_run_file_read(path, &limits, &loaded, hash,
                                error, sizeof(error)) == 0,
          "exact-half Stage 7 overlap did not load: %s", error);
    hwa_run_result_free(&loaded);
    if (data != NULL) {
        (void)test_reject_row_field(data, size, path, "CLOCK,1,source,",
                                    9U, "23999", &limits,
                                    "one-under half overlap");
        (void)test_reject_row_field(data, size, path, "CLOCK,1,source,",
                                    11U, "5", &limits,
                                    "exact-half low-overlap flag");
    }
    free(data);
    hwa_run_result_free(&source);
}

static void test_one_sided_feature(const char *path)
{
    HWARunResult source;
    HWARunResult loaded;
    HWARunOptions limits;
    HWARunFeature *feature;
    char hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    unsigned char *data;
    size_t size = 0U;
    CHECK(test_make_result(&source),
          "cannot make Stage 7 one-sided feature fixture");
    feature = &source.features[hwa_run_feature_catalog_count()];
    feature->availability = HWA_RUN_INSUFFICIENT;
    feature->model_value = 0.0;
    feature->delta = 0.0;
    feature->normalized_gap = 0.0;
    feature->model_valid = 0;
    feature->delta_valid = 0;
    feature->gap_valid = 0;
    CHECK(hwa_run_stage_rows_rebuild(&source, error, sizeof(error)) == 0 &&
              hwa_run_warnings_rebuild(&source, error, sizeof(error)) == 0 &&
              hwa_run_result_retained_bytes(
                  &source, &source.retained_work_bytes) == 0 &&
              hwa_run_result_validate(&source, error, sizeof(error)) == 0,
          "valid Stage 7 one-sided feature was rejected: %s", error);
    CHECK(test_write_result(path, &source),
          "cannot write Stage 7 one-sided feature fixture");
    data = test_read_file(path, &size);
    hwa_run_options_default(&limits);
    memset(&loaded, 0, sizeof(loaded));
    CHECK(data != NULL &&
              hwa_run_file_read(path, &limits, &loaded, hash,
                                error, sizeof(error)) == 0,
          "cannot read Stage 7 one-sided feature: %s", error);
    CHECK(loaded.feature_count > 12U &&
              loaded.features[12].availability == HWA_RUN_INSUFFICIENT &&
              loaded.features[12].reference_valid &&
              loaded.features[12].reference_value == -20.0 &&
              !loaded.features[12].model_valid &&
              !loaded.features[12].delta_valid &&
              !loaded.features[12].gap_valid &&
              !loaded.stages[0].gap_valid &&
              !loaded.stages[1].gap_valid &&
              loaded.stages[2].gap_valid,
          "Stage 7 reader did not preserve the one-sided feature");
    hwa_run_result_free(&loaded);
    if (data != NULL) {
        (void)test_reject_row_field(data, size, path,
                                    "FEATURE,13,2,body,", 7U,
                                    "unavailable", &limits,
                                    "present-clock unavailable feature");
        (void)test_reject_row_field(data, size, path,
                                    "FEATURE,13,2,body,", 7U,
                                    "available", &limits,
                                    "available one-sided feature");
        (void)test_reject_row_field(data, size, path,
                                    "FEATURE,13,2,body,", 9U,
                                    "0", &limits,
                                    "hidden invalid feature value");
    }
    free(data);
    hwa_run_result_free(&source);
}

static void test_binary_probe_source(const char *path)
{
    HWARunResult source;
    HWARunResult loaded;
    HWARunOptions limits;
    char hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    unsigned char *data;
    size_t size = 0U;
    CHECK(test_make_result(&source),
          "cannot make Stage 7 binary probe result");
    source.sources[2].probe_format = HWA_RUN_PROBE_BINARY_F64LE;
    source.sources[2].file_bytes = 816U;
    free(source.sources[2].path);
    source.sources[2].path = test_copy("/not-opened/probe.bin");
    CHECK(source.sources[2].path != NULL &&
              hwa_run_result_retained_bytes(
                  &source, &source.retained_work_bytes) == 0 &&
              test_write_result(path, &source),
          "cannot write Stage 7 binary probe result");
    hwa_run_options_default(&limits);
    memset(&loaded, 0, sizeof(loaded));
    CHECK(hwa_run_file_read(path, &limits, &loaded, hash,
                            error, sizeof(error)) == 0 &&
              loaded.source_count > 2U &&
              loaded.sources[2].probe_format ==
                  HWA_RUN_PROBE_BINARY_F64LE &&
              loaded.sources[2].file_bytes == 816U,
          "cannot round-trip Stage 7 binary probe source: %s", error);
    hwa_run_result_free(&loaded);
    data = test_read_file(path, &size);
    if (data != NULL) {
        (void)test_reject_row_field(data, size, path,
                                    "SOURCE,3,model.force,", 15U,
                                    "815", &limits,
                                    "binary probe file byte shape");
    }
    free(data);
    hwa_run_result_free(&source);
}

static void test_low_variance_link(const char *path)
{
    HWARunResult source;
    HWARunResult loaded;
    HWARunOptions limits;
    HWARunLink *link;
    char hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    unsigned char *data;
    size_t size = 0U;
    CHECK(test_make_result(&source),
          "cannot make Stage 7 low-variance link fixture");
    link = &source.links[0];
    link->availability = HWA_RUN_INSUFFICIENT;
    link->lag_hops = 0;
    link->lag_samples = 0;
    link->correlation = 0.0;
    link->slope = 0.0;
    link->intercept = 0.0;
    link->r_squared = 0.0;
    link->point_count = 64U;
    link->coverage = 64.0 / 99.0;
    link->quality_flags = HWA_RUN_QUALITY_LOW_VARIANCE;
    link->fit_valid = 0;
    CHECK(hwa_run_warnings_rebuild(&source, error, sizeof(error)) == 0 &&
              hwa_run_evaluations_expected(
                  &source, &source.evaluation_count) == 0 &&
              hwa_run_result_retained_bytes(
                  &source, &source.retained_work_bytes) == 0 &&
              hwa_run_result_validate(&source, error, sizeof(error)) == 0,
          "valid Stage 7 low-variance link was rejected: %s", error);
    CHECK(test_write_result(path, &source),
          "cannot write Stage 7 low-variance link fixture");
    data = test_read_file(path, &size);
    hwa_run_options_default(&limits);
    memset(&loaded, 0, sizeof(loaded));
    CHECK(data != NULL &&
              hwa_run_file_read(path, &limits, &loaded, hash,
                                error, sizeof(error)) == 0 &&
              loaded.link_count == 1U &&
              loaded.links[0].availability == HWA_RUN_INSUFFICIENT &&
              loaded.links[0].point_count == 64U &&
              loaded.links[0].quality_flags ==
                  HWA_RUN_QUALITY_LOW_VARIANCE,
          "cannot round-trip Stage 7 low-variance link: %s", error);
    hwa_run_result_free(&loaded);
    if (data != NULL) {
        (void)test_reject_row_field(data, size, path,
                                    "LINK,1,2,3,rms_dbfs,", 15U,
                                    "0", &limits,
                                    "missing low-variance link flag");
    }
    free(data);
    hwa_run_result_free(&source);
}

static void test_locale(const char *path)
{
    static const char *const candidates[] = {
        "de_DE.UTF-8", "fr_FR.UTF-8", "el_GR.UTF-8"
    };
    HWARunResult result;
    const char *original = setlocale(LC_NUMERIC, NULL);
    char *saved = original != NULL ? test_copy(original) : NULL;
    size_t index;
    int changed = 0;
    for (index = 0U; index < sizeof(candidates) / sizeof(candidates[0]); ++index) {
        if (setlocale(LC_NUMERIC, candidates[index]) != NULL) {
            changed = 1;
            break;
        }
    }
    if (changed && test_make_result(&result)) {
        unsigned char *data;
        HWARunOptions limits;
        HWARunResult loaded;
        char hash[HWA_SHA256_HEX_SIZE];
        char error[HWA_ERROR_SIZE];
        size_t size = 0U;
        CHECK(test_write_result(path, &result),
              "Stage 7 locale write failed");
        data = test_read_file(path, &size);
        CHECK(data != NULL && strstr((const char *)data, ",0.75,") != NULL,
              "Stage 7 writer used the process decimal separator");
        hwa_run_options_default(&limits);
        memset(&loaded, 0, sizeof(loaded));
        CHECK(hwa_run_file_read(path, &limits, &loaded, hash,
                                error, sizeof(error)) == 0,
              "Stage 7 locale read failed: %s", error);
        hwa_run_result_free(&loaded);
        free(data);
        hwa_run_result_free(&result);
    }
    if (saved != NULL) {
        (void)setlocale(LC_NUMERIC, saved);
        free(saved);
    }
}

int main(void)
{
    char first[256];
    char second[256];
    char bad[256];
    char locale_path[256];
    int pid = (int)HWA_TEST_PID();
    (void)snprintf(first, sizeof(first), "/tmp/hwa-run-%d-a.hwa", pid);
    (void)snprintf(second, sizeof(second), "/tmp/hwa-run-%d-b.hwa", pid);
    (void)snprintf(bad, sizeof(bad), "/tmp/hwa-run-%d-bad.hwa", pid);
    (void)snprintf(locale_path, sizeof(locale_path),
                   "/tmp/hwa-run-%d-locale.hwa", pid);
    test_round_trip(first, second);
    test_caps_and_hostile(first, bad);
    test_one_sided_feature(bad);
    test_binary_probe_source(bad);
    test_low_variance_link(bad);
    test_overlap_boundary(bad);
    test_locale(locale_path);
    (void)HWA_TEST_UNLINK(first);
    (void)HWA_TEST_UNLINK(second);
    (void)HWA_TEST_UNLINK(bad);
    (void)HWA_TEST_UNLINK(locale_path);
    if (failures != 0) {
        (void)fprintf(stderr, "%d Stage 7 persistence test(s) failed\n",
                      failures);
        return 1;
    }
    (void)puts("Stage 7 persistence tests passed");
    return 0;
}

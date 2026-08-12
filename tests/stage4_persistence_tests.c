#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "measure_compare.h"
#include "measure_file.h"
#include "sha256.h"

#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

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

static int test_set_comma_numeric_locale(void)
{
    static const char *const candidates[] = {
        "de_DE.UTF-8",
        "fr_FR.UTF-8",
        "de_DE",
        "German_Germany.1252",
        "de-DE"
    };
    size_t index;
    for (index = 0U;
         index < sizeof(candidates) / sizeof(candidates[0]);
         ++index) {
        if (setlocale(LC_NUMERIC, candidates[index]) != NULL &&
            strcmp(localeconv()->decimal_point, ",") == 0) return 1;
    }
    return 0;
}

static long test_process_id(void)
{
#if defined(_WIN32)
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}

static int test_make_directory(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

static int test_remove_directory(const char *path)
{
#if defined(_WIN32)
    return _rmdir(path);
#else
    return rmdir(path);
#endif
}

static int test_remove_file(const char *path)
{
#if defined(_WIN32)
    return _unlink(path);
#else
    return unlink(path);
#endif
}

static int test_workspace(char path[PATH_MAX])
{
    unsigned attempt;
#if defined(_WIN32)
    const char *root = getenv("TEMP");
    if (root == NULL || root[0] == '\0') root = ".";
#else
    const char *root = "/tmp";
#endif
    for (attempt = 0U; attempt < 100U; ++attempt) {
        int length = snprintf(path, PATH_MAX, "%s/hwa-stage4-profile-%ld-%u",
                              root, test_process_id(), attempt);
        if (length < 0 || length >= PATH_MAX) return 0;
        if (test_make_directory(path) == 0) return 1;
        if (errno != EEXIST) return 0;
    }
    return 0;
}

static int test_path(char output[PATH_MAX],
                     const char *directory,
                     const char *name)
{
    int length = snprintf(output, PATH_MAX, "%s/%s", directory, name);
    return length >= 0 && length < PATH_MAX;
}

static char *test_copy(HWAMeasurementSet *set, const char *text)
{
    size_t size = strlen(text) + 1U;
    char *copy = (char *)malloc(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
        set->retained_work_bytes += (uint64_t)size;
    }
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

static int test_make_profile(HWAMeasurementSet *set,
                             double first,
                             double second)
{
    static const double confidence[3] = {0.9, 0.6, 0.3};
    double values[3];
    size_t item;
    char error[HWA_ERROR_SIZE];
    memset(set, 0, sizeof(*set));
    hwa_measurement_options_default(&set->options);
    set->options.fft_size = 1024U;
    set->options.hop_size = 128U;
    set->options.max_partials = 4U;
    set->options.max_work_bytes = UINT64_C(67108864);
    set->items_path = test_copy(set, "items.hwa-items");
    set->audio_path = test_copy(set, "audio.wav");
    set->alignment_path = test_copy(set, "take.hwa-align");
    set->source_score_path = test_copy(set, "score.csv");
    test_hash(set->items_sha256, 'a');
    test_hash(set->audio_sha256, 'b');
    test_hash(set->alignment_sha256, 'c');
    test_hash(set->source_score_sha256, 'd');
    set->audio_format.container = HWA_CONTAINER_RIFF;
    set->audio_format.encoding = HWA_ENCODING_PCM;
    set->audio_format.channels = 1U;
    set->audio_format.sample_rate_hz = 16000U;
    set->audio_format.bits_per_sample = 16U;
    set->audio_format.valid_bits_per_sample = 16U;
    set->audio_format.block_align = 2U;
    set->audio_format.frames = 16000U;
    set->audio_format.data_bytes = 32000U;
    set->audio_format.duration_seconds = 1.0;
    set->level_reference_dbfs = 0.5 * (first + second);
    set->level_reference_item_count = 2U;
    set->level_reference_valid = 1;
    set->item_frame_evaluations = 6U;
    set->transform_count = 2U;
    set->context_count = 3U;
    set->contexts = (HWAMeasureItemContext *)calloc(
        set->context_count, sizeof(*set->contexts));
    set->measurement_count = 6U;
    set->measurements = (HWAMeasureObservation *)calloc(
        set->measurement_count, sizeof(*set->measurements));
    set->warning_count = 1U;
    set->warnings = (HWAMeasureWarning *)calloc(
        set->warning_count, sizeof(*set->warnings));
    if (set->contexts == NULL || set->measurements == NULL ||
        set->warnings == NULL || set->items_path == NULL ||
        set->audio_path == NULL || set->alignment_path == NULL ||
        set->source_score_path == NULL) {
        hwa_measurement_set_free(set);
        return 0;
    }
    set->retained_work_bytes +=
        (uint64_t)set->context_count * sizeof(*set->contexts) +
        (uint64_t)set->measurement_count * sizeof(*set->measurements) +
        (uint64_t)set->warning_count * sizeof(*set->warnings);
    values[0] = first;
    values[1] = second;
    values[2] = 0.0;
    for (item = 0U; item < set->context_count; ++item) {
        HWAMeasureItemContext *context = &set->contexts[item];
        HWAMeasureObservation *raw = &set->measurements[item * 2U];
        HWAMeasureObservation *relative = raw + 1;
        char key[32];
        (void)snprintf(key, sizeof(key), "body:%zu", item + 1U);
        context->item_id = (uint64_t)item + 1U;
        context->item_key = test_copy(set, key);
        context->item_kind = HWA_ITEM_BODY;
        context->item_role = test_copy(set, "body");
        context->start_sample = (uint64_t)item * 4000U;
        context->end_sample = context->start_sample + 3000U;
        context->source_event_count = 1U;
        context->item_confidence = confidence[item];
        raw->id = (uint64_t)item * 2U + 1U;
        raw->item_id = context->item_id;
        raw->kind = HWA_MEASURE_RMS_DBFS;
        raw->unit = HWA_MEASURE_UNIT_DBFS;
        raw->view = HWA_MEASURE_VIEW_RAW;
        raw->status = item < 2U ? HWA_MEASURE_STATUS_VALID :
                                 HWA_MEASURE_STATUS_NO_SIGNAL;
        raw->value = values[item];
        raw->confidence = confidence[item];
        relative->id = raw->id + 1U;
        relative->item_id = raw->item_id;
        relative->kind = raw->kind;
        relative->unit = HWA_MEASURE_UNIT_DB;
        relative->view = HWA_MEASURE_VIEW_LEVEL_RELATIVE;
        relative->status = raw->status;
        relative->value = item < 2U
                              ? values[item] - set->level_reference_dbfs
                              : 0.0;
        relative->confidence = raw->confidence;
        relative->evidence_flags = item < 2U
            ? HWA_MEASURE_EVIDENCE_LEVEL_REFERENCE : 0U;
        if (context->item_key == NULL || context->item_role == NULL) {
            hwa_measurement_set_free(set);
            return 0;
        }
    }
    set->warnings[0].id = 1U;
    set->warnings[0].code = test_copy(set, "stage4-capability");
    set->warnings[0].message = test_copy(
        set, "Production-corrected measurements are not available.");
    set->warnings[0].item_id = 1U;
    set->warnings[0].observation_id = 1U;
    set->warnings[0].item_id_valid = 1;
    set->warnings[0].observation_id_valid = 1;
    if (set->warnings[0].code == NULL || set->warnings[0].message == NULL ||
        hwa_measure_build_profile(set, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "fixture build failed: %s\n", error);
        hwa_measurement_set_free(set);
        return 0;
    }
    return 1;
}

static int test_write_profile(const char *path, const HWAMeasurementSet *set)
{
    FILE *stream = fopen(path, "wb");
    char error[HWA_ERROR_SIZE];
    int result;
    if (stream == NULL) return 0;
    result = hwa_measure_file_write(stream, set, error, sizeof(error));
    if (fclose(stream) != 0) result = -1;
    if (result != 0) (void)fprintf(stderr, "profile write failed: %s\n", error);
    return result == 0;
}

static char *test_read_bytes(const char *path, size_t *size)
{
    FILE *stream = fopen(path, "rb");
    long length;
    char *bytes;
    *size = 0U;
    if (stream == NULL || fseek(stream, 0L, SEEK_END) != 0) {
        if (stream != NULL) (void)fclose(stream);
        return NULL;
    }
    length = ftell(stream);
    if (length < 0 || fseek(stream, 0L, SEEK_SET) != 0 ||
        (uintmax_t)length > (uintmax_t)(SIZE_MAX - 1U)) {
        (void)fclose(stream);
        return NULL;
    }
    bytes = (char *)malloc((size_t)length + 1U);
    if (bytes == NULL) {
        (void)fclose(stream);
        return NULL;
    }
    if (fread(bytes, 1U, (size_t)length, stream) != (size_t)length ||
        fclose(stream) != 0) {
        free(bytes);
        return NULL;
    }
    bytes[(size_t)length] = '\0';
    *size = (size_t)length;
    return bytes;
}

static int test_write_bytes(const char *path, const char *bytes, size_t size)
{
    FILE *stream = fopen(path, "wb");
    int okay = stream != NULL && fwrite(bytes, 1U, size, stream) == size;
    if (stream != NULL && fclose(stream) != 0) okay = 0;
    return okay;
}

static char *test_replace_once(const char *source,
                               const char *needle,
                               const char *replacement,
                               size_t *new_size)
{
    const char *found = strstr(source, needle);
    size_t source_size = strlen(source);
    size_t before;
    size_t needle_size = strlen(needle);
    size_t replacement_size = strlen(replacement);
    char *result;
    if (found == NULL || source_size - needle_size >
                         SIZE_MAX - replacement_size) return NULL;
    before = (size_t)(found - source);
    *new_size = source_size - needle_size + replacement_size;
    result = (char *)malloc(*new_size + 1U);
    if (result == NULL) return NULL;
    memcpy(result, source, before);
    memcpy(result + before, replacement, replacement_size);
    memcpy(result + before + replacement_size, found + needle_size,
           source_size - before - needle_size + 1U);
    return result;
}

static HWAProfileComparisonOptions test_limits(void)
{
    HWAProfileComparisonOptions limits;
    hwa_profile_comparison_options_default(&limits);
    limits.max_input_bytes = UINT64_C(67108864);
    limits.max_work_bytes = UINT64_C(67108864);
    limits.max_contexts = 100U;
    limits.max_measurements = 100U;
    limits.max_groups = 100U;
    limits.max_group_members = 1000U;
    limits.max_statistics = 1000U;
    limits.max_warnings = 100U;
    limits.max_distributions = 1000U;
    limits.max_gaps = 1000U;
    return limits;
}

static int test_read_profile(const char *path,
                             const HWAProfileComparisonOptions *limits,
                             HWAMeasurementSet *set,
                             char *error,
                             size_t error_size)
{
    char sha256[HWA_SHA256_HEX_SIZE];
    memset(set, 0, sizeof(*set));
    return hwa_measure_file_read(path, limits, set, sha256,
                                 error, error_size);
}

static void test_round_trip_and_distributions(const char *directory)
{
    HWAMeasurementSet source;
    HWAMeasurementSet loaded;
    HWAProfileComparisonOptions limits = test_limits();
    char first_path[PATH_MAX];
    char second_path[PATH_MAX];
    char hash[HWA_SHA256_HEX_SIZE];
    char read_hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    char *first_bytes;
    char *second_bytes;
    size_t first_size;
    size_t second_size;
    FILE *stream;
    const HWAMeasureStatistics *statistics;

    CHECK(test_path(first_path, directory, "profile.hwa-measures"),
          "profile path failed");
    CHECK(test_path(second_path, directory, "roundtrip.hwa-measures"),
          "round-trip path failed");
    CHECK(test_make_profile(&source, -20.0, -10.0),
          "profile fixture failed");
    if (source.contexts == NULL) return;
    CHECK(source.statistic_count == 2U, "expected raw and relative stats");
    statistics = &source.statistics[0].statistics;
    CHECK(statistics->total_count == 3U && statistics->valid_count == 2U &&
              statistics->missing_count == 1U,
          "missing observations were not kept in distribution coverage");
    CHECK(statistics->mean == -15.0 && statistics->q05 == -19.5 &&
              statistics->q50 == -15.0 && statistics->q95 == -10.5,
          "fixed quantiles or mean are wrong");
    CHECK(statistics->confidence == 0.5,
          "stat confidence must include missing-row coverage");
    CHECK(test_write_profile(first_path, &source), "profile write failed");
    CHECK(hwa_sha256_file(first_path, limits.max_input_bytes, hash,
                          error, sizeof(error)) == 0,
          "profile hash failed: %s", error);
    memset(&loaded, 0, sizeof(loaded));
    CHECK(hwa_measure_file_read(first_path, &limits, &loaded, read_hash,
                                error, sizeof(error)) == 0,
          "profile read failed: %s", error);
    CHECK(strcmp(hash, read_hash) == 0, "reader returned the wrong hash");
    CHECK(loaded.retained_work_bytes == source.retained_work_bytes,
          "retained work did not round-trip exactly");
    stream = fopen(second_path, "wb");
    CHECK(stream != NULL, "cannot open round-trip output");
    if (stream != NULL) {
        CHECK(hwa_measure_file_write(stream, &loaded,
                                     error, sizeof(error)) == 0,
              "round-trip write failed: %s", error);
        CHECK(fclose(stream) == 0, "round-trip close failed");
    }
    first_bytes = test_read_bytes(first_path, &first_size);
    second_bytes = test_read_bytes(second_path, &second_size);
    CHECK(first_bytes != NULL && second_bytes != NULL &&
              first_size == second_size &&
              memcmp(first_bytes, second_bytes, first_size) == 0,
          "profile parser/writer round-trip changed canonical bytes");
    free(second_bytes);
    free(first_bytes);
    hwa_measurement_set_free(&loaded);
    hwa_measurement_set_free(&source);
    (void)test_remove_file(second_path);
    (void)test_remove_file(first_path);
}

static void test_distribution_gap(const char *directory)
{
    HWAMeasurementSet reference;
    HWAMeasurementSet model;
    HWAProfileComparisonSet comparison;
    HWAProfileComparisonOptions limits = test_limits();
    char reference_path[PATH_MAX];
    char model_path[PATH_MAX];
    char error[HWA_ERROR_SIZE];
    size_t index;
    int found_shape_gap = 0;
    CHECK(test_path(reference_path, directory, "reference.hwa-measures") &&
              test_path(model_path, directory, "model.hwa-measures"),
          "comparison paths failed");
    CHECK(test_make_profile(&reference, -20.0, -10.0) &&
              test_make_profile(&model, -25.0, -5.0),
          "comparison fixtures failed");
    if (reference.contexts == NULL || model.contexts == NULL) return;
    CHECK(test_write_profile(reference_path, &reference) &&
              test_write_profile(model_path, &model),
          "comparison profile write failed");
    memset(&comparison, 0, sizeof(comparison));
    CHECK(hwa_compare_measure_files(reference_path, model_path, &limits,
                                    &comparison, error, sizeof(error)) == 0,
          "saved profile comparison failed: %s", error);
    for (index = 0U; index < comparison.distribution_count; ++index) {
        const HWAProfileDistribution *distribution =
            &comparison.distributions[index];
        const HWAProfileGap *gap = &comparison.gaps[index];
        if (distribution->kind == HWA_MEASURE_RMS_DBFS &&
            distribution->view == HWA_MEASURE_VIEW_RAW &&
            distribution->reference_statistics.mean ==
                distribution->model_statistics.mean) {
            CHECK(gap->mean_delta_valid && gap->mean_delta == 0.0,
                  "equal means should have zero mean delta");
            CHECK(gap->quantile_distance_valid &&
                      gap->quantile_distance > 0.0 && gap->rank != 0U,
                  "comparison lost a distribution-shape gap");
            found_shape_gap = 1;
        }
    }
    CHECK(found_shape_gap, "no equal-mean distribution-shape gap found");
    hwa_profile_comparison_set_free(&comparison);
    hwa_measurement_set_free(&model);
    hwa_measurement_set_free(&reference);
    (void)test_remove_file(model_path);
    (void)test_remove_file(reference_path);
}

static void test_cross_build_statistic_rounding(const char *directory)
{
    HWAMeasurementSet source;
    HWAMeasurementSet loaded;
    HWAProfileComparisonOptions limits = test_limits();
    char base_path[PATH_MAX];
    char near_path[PATH_MAX];
    char far_path[PATH_MAX];
    char normalized_path[PATH_MAX];
    char error[HWA_ERROR_SIZE];
    char needle[96];
    char near_text[128];
    char far_text[128];
    char *base_bytes = NULL;
    char *near_bytes = NULL;
    char *far_bytes = NULL;
    char *normalized_bytes = NULL;
    size_t base_size = 0U;
    size_t near_size = 0U;
    size_t far_size = 0U;
    size_t normalized_size = 0U;
    double near_value = nextafter(5.0, INFINITY);
    double far_value = 5.0;
    unsigned step;
    FILE *stream;

    for (step = 0U; step < 33U; ++step)
        far_value = nextafter(far_value, INFINITY);
    CHECK(test_path(base_path, directory, "rounding-base.hwa-measures") &&
              test_path(near_path, directory, "rounding-near.hwa-measures") &&
              test_path(far_path, directory, "rounding-far.hwa-measures") &&
              test_path(normalized_path, directory,
                        "rounding-normalized.hwa-measures"),
          "rounding paths failed");
    CHECK(test_make_profile(&source, -20.0, -10.0),
          "rounding fixture failed");
    if (source.contexts == NULL) return;
    CHECK(test_write_profile(base_path, &source),
          "rounding base write failed");
    base_bytes = test_read_bytes(base_path, &base_size);
    CHECK(base_bytes != NULL, "cannot read rounding fixture");
    CHECK(snprintf(needle, sizeof(needle),
                   ",5,0.5,1,0\r\nSTAT,2") > 0 &&
              snprintf(near_text, sizeof(near_text),
                       ",%.17g,0.5,1,0\r\nSTAT,2", near_value) > 0 &&
              snprintf(far_text, sizeof(far_text),
                       ",%.17g,0.5,1,0\r\nSTAT,2", far_value) > 0,
          "rounding text formatting failed");
    near_bytes = base_bytes == NULL ? NULL :
        test_replace_once(base_bytes, needle, near_text, &near_size);
    far_bytes = base_bytes == NULL ? NULL :
        test_replace_once(base_bytes, needle, far_text, &far_size);
    CHECK(near_bytes != NULL && far_bytes != NULL,
          "rounding statistic field was not found");
    if (near_bytes != NULL) {
        CHECK(test_write_bytes(near_path, near_bytes, near_size),
              "near-rounding profile write failed");
        CHECK(test_read_profile(near_path, &limits, &loaded,
                                error, sizeof(error)) == 0,
              "one-ULP producer drift was rejected: %s", error);
        if (loaded.contexts != NULL) {
            stream = fopen(normalized_path, "wb");
            CHECK(stream != NULL, "cannot open normalized profile");
            if (stream != NULL) {
                CHECK(hwa_measure_file_write(stream, &loaded,
                                             error, sizeof(error)) == 0,
                      "normalized profile write failed: %s", error);
                CHECK(fclose(stream) == 0,
                      "normalized profile close failed");
            }
            normalized_bytes = test_read_bytes(normalized_path,
                                               &normalized_size);
            CHECK(normalized_bytes != NULL &&
                      normalized_size == base_size &&
                      memcmp(normalized_bytes, base_bytes, base_size) == 0,
                  "reader did not normalize producer rounding drift");
        }
        hwa_measurement_set_free(&loaded);
    }
    if (far_bytes != NULL) {
        CHECK(test_write_bytes(far_path, far_bytes, far_size),
              "far-rounding profile write failed");
        CHECK(test_read_profile(far_path, &limits, &loaded,
                                error, sizeof(error)) != 0,
              "33-ULP statistic change passed canonical validation");
        hwa_measurement_set_free(&loaded);
    }
    free(normalized_bytes);
    free(far_bytes);
    free(near_bytes);
    free(base_bytes);
    hwa_measurement_set_free(&source);
    (void)test_remove_file(normalized_path);
    (void)test_remove_file(far_path);
    (void)test_remove_file(near_path);
    (void)test_remove_file(base_path);
}

static void test_numeric_locale_and_subnormal(const char *directory)
{
    static const char prefix[] = "META,pitch_confidence_floor,";
    HWAMeasurementSet source;
    HWAMeasurementSet loaded;
    HWAProfileComparisonOptions limits = test_limits();
    char path[PATH_MAX];
    char error[HWA_ERROR_SIZE];
    char saved_locale[128];
    const char *current = setlocale(LC_NUMERIC, NULL);
    char *bytes = NULL;
    char *field = NULL;
    char *field_end = NULL;
    size_t size = 0U;
    double true_min;
    int comma_locale = 0;

#ifdef DBL_TRUE_MIN
    true_min = DBL_TRUE_MIN;
#else
    true_min = nextafter(0.0, 1.0);
#endif
    CHECK(current != NULL &&
              (current == NULL || strlen(current) < sizeof(saved_locale)),
          "cannot save the caller's numeric locale");
    if (current == NULL || strlen(current) >= sizeof(saved_locale)) return;
    memcpy(saved_locale, current, strlen(current) + 1U);
    CHECK(true_min != 0.0 && isfinite(true_min) &&
              fpclassify(true_min) == FP_SUBNORMAL,
          "platform did not provide a finite subnormal double");
    if (true_min == 0.0 || !isfinite(true_min) ||
        fpclassify(true_min) != FP_SUBNORMAL) return;
    CHECK(test_path(path, directory, "subnormal.hwa-measures"),
          "subnormal profile path failed");
    CHECK(test_make_profile(&source, -20.0, -10.0),
          "subnormal profile fixture failed");
    if (source.contexts == NULL) return;
    source.options.pitch_confidence_floor = true_min;
    comma_locale = test_set_comma_numeric_locale();
    CHECK(test_write_profile(path, &source),
          "subnormal profile write failed");
    if (comma_locale) {
        CHECK(strcmp(localeconv()->decimal_point, ",") == 0,
              "measurement writer changed the caller's numeric locale");
    }
    bytes = test_read_bytes(path, &size);
    CHECK(bytes != NULL && size != 0U,
          "cannot read the subnormal profile bytes");
    if (bytes != NULL) {
        field = strstr(bytes, prefix);
        if (field != NULL) field += strlen(prefix);
        field_end = field == NULL ? NULL : strchr(field, ',');
        CHECK(field != NULL && field_end != NULL && field_end > field &&
                  memchr(field, '.', (size_t)(field_end - field)) != NULL,
              "subnormal profile did not use a dot decimal field");
    }
    CHECK(test_read_profile(path, &limits, &loaded,
                            error, sizeof(error)) == 0,
          "subnormal profile read failed: %s", error);
    if (loaded.contexts != NULL) {
        CHECK(loaded.options.pitch_confidence_floor == true_min,
              "subnormal measurement option did not round trip");
    }
    if (comma_locale) {
        CHECK(strcmp(localeconv()->decimal_point, ",") == 0,
              "measurement reader changed the caller's numeric locale");
    }
    hwa_measurement_set_free(&loaded);
    free(bytes);
    hwa_measurement_set_free(&source);
    (void)test_remove_file(path);
    CHECK(setlocale(LC_NUMERIC, saved_locale) != NULL,
          "cannot restore the saved numeric locale");
}

static void test_reader_caps(const char *directory)
{
    HWAMeasurementSet source;
    HWAMeasurementSet model;
    HWAMeasurementSet loaded;
    HWAProfileComparisonSet comparison;
    HWAProfileComparisonOptions limits = test_limits();
    char path[PATH_MAX];
    char model_path[PATH_MAX];
    char error[HWA_ERROR_SIZE];
    char *bytes;
    size_t size;
    uint64_t low;
    uint64_t high;
    uint64_t minimum;
    CHECK(test_path(path, directory, "caps.hwa-measures") &&
              test_path(model_path, directory, "caps-model.hwa-measures"),
          "cap path failed");
    CHECK(test_make_profile(&source, -20.0, -10.0), "cap fixture failed");
    CHECK(test_make_profile(&model, -21.0, -9.0),
          "comparison cap fixture failed");
    if (source.contexts == NULL) return;
    CHECK(test_write_profile(path, &source) &&
              test_write_profile(model_path, &model),
          "cap profile write failed");
    bytes = test_read_bytes(path, &size);
    CHECK(bytes != NULL, "cannot read cap fixture");
    if (bytes != NULL) {
        limits.max_input_bytes = (uint64_t)size;
        CHECK(test_read_profile(path, &limits, &loaded,
                                error, sizeof(error)) == 0,
              "exact input-byte cap failed: %s", error);
        hwa_measurement_set_free(&loaded);
        limits.max_input_bytes = (uint64_t)size - 1U;
        CHECK(test_read_profile(path, &limits, &loaded,
                                error, sizeof(error)) != 0,
              "one-byte-under input cap passed");
        free(bytes);
    }
    limits = test_limits();
    low = 0U;
    high = limits.max_work_bytes;
    while (low + 1U < high) {
        uint64_t middle = low + (high - low) / 2U;
        limits.max_work_bytes = middle;
        if (test_read_profile(path, &limits, &loaded,
                              error, sizeof(error)) == 0) {
            hwa_measurement_set_free(&loaded);
            high = middle;
        } else {
            low = middle;
        }
    }
    minimum = high;
    limits.max_work_bytes = minimum;
    CHECK(test_read_profile(path, &limits, &loaded,
                            error, sizeof(error)) == 0,
          "exact reader work cap failed: %s", error);
    hwa_measurement_set_free(&loaded);
    limits.max_work_bytes = minimum - 1U;
    CHECK(test_read_profile(path, &limits, &loaded,
                            error, sizeof(error)) != 0,
          "one-byte-under reader work cap passed");

    limits = test_limits();
    low = 0U;
    high = limits.max_work_bytes;
    while (low + 1U < high) {
        uint64_t middle = low + (high - low) / 2U;
        limits.max_work_bytes = middle;
        memset(&comparison, 0, sizeof(comparison));
        if (hwa_compare_measure_files(path, model_path, &limits, &comparison,
                                      error, sizeof(error)) == 0) {
            hwa_profile_comparison_set_free(&comparison);
            high = middle;
        } else {
            low = middle;
        }
    }
    minimum = high;
    limits.max_work_bytes = minimum;
    memset(&comparison, 0, sizeof(comparison));
    CHECK(hwa_compare_measure_files(path, model_path, &limits, &comparison,
                                    error, sizeof(error)) == 0,
          "exact comparison work cap failed: %s", error);
    hwa_profile_comparison_set_free(&comparison);
    limits.max_work_bytes = minimum - 1U;
    memset(&comparison, 0, sizeof(comparison));
    CHECK(hwa_compare_measure_files(path, model_path, &limits, &comparison,
                                    error, sizeof(error)) != 0,
          "one-byte-under comparison work cap passed");
    hwa_profile_comparison_set_free(&comparison);
    hwa_measurement_set_free(&model);
    hwa_measurement_set_free(&source);
    (void)test_remove_file(model_path);
    (void)test_remove_file(path);
}

static void test_tamper(const char *directory,
                        const char *name,
                        const char *needle,
                        const char *replacement)
{
    HWAMeasurementSet source;
    HWAMeasurementSet loaded;
    HWAProfileComparisonOptions limits = test_limits();
    char base_path[PATH_MAX];
    char bad_path[PATH_MAX];
    char error[HWA_ERROR_SIZE];
    char *bytes;
    char *changed;
    size_t size;
    size_t changed_size;
    CHECK(test_path(base_path, directory, "tamper-base.hwa-measures") &&
              test_path(bad_path, directory, name),
          "tamper paths failed");
    CHECK(test_make_profile(&source, -20.0, -10.0),
          "tamper fixture failed");
    if (source.contexts == NULL) return;
    CHECK(test_write_profile(base_path, &source), "tamper base write failed");
    bytes = test_read_bytes(base_path, &size);
    changed = bytes == NULL ? NULL :
        test_replace_once(bytes, needle, replacement, &changed_size);
    CHECK(changed != NULL, "tamper needle was not found for %s", name);
    if (changed != NULL) {
        CHECK(test_write_bytes(bad_path, changed, changed_size),
              "tampered file write failed");
        CHECK(test_read_profile(bad_path, &limits, &loaded,
                                error, sizeof(error)) != 0,
              "tampered profile passed: %s", name);
        hwa_measurement_set_free(&loaded);
    }
    free(changed);
    free(bytes);
    hwa_measurement_set_free(&source);
    (void)test_remove_file(bad_path);
    (void)test_remove_file(base_path);
}

static void test_hostile_profiles(const char *directory)
{
    test_tamper(directory, "capability.hwa-measures",
                "META,capability_flags,0,bitset",
                "META,capability_flags,1,bitset");
    test_tamper(directory, "duplicate-key.hwa-measures",
                "CONTEXT,2,body:2,", "CONTEXT,2,body:1,");
    test_tamper(directory, "reference.hwa-measures",
                "META,level_reference_dbfs,-15,dBFS",
                "META,level_reference_dbfs,-14,dBFS");
    test_tamper(directory, "relative.hwa-measures",
                "MEASURE,2,1,rms_dbfs,0,dB,level-relative,valid,-5,",
                "MEASURE,2,1,rms_dbfs,0,dB,level-relative,valid,-4,");
    test_tamper(directory, "audio-shape.hwa-measures",
                "META,sample_rate_hz,16000,Hz",
                "META,sample_rate_hz,7999,Hz");
    test_tamper(directory, "retained.hwa-measures",
                "META,retained_work_bytes,", "META,retained_work_bytes,1");
    test_tamper(directory, "context-unwind.hwa-measures",
                ",0.29999999999999999,0,0\r\nMEASURE,1,1",
                ",0.29999999999999999,0,2\r\nMEASURE,1,1");
    test_tamper(directory, "group-unwind.hwa-measures",
                ",all,,3\r\nGROUP_MEMBER", ",all,,x\r\nGROUP_MEMBER");
    test_tamper(directory, "warning-unwind.hwa-measures",
                "WARNING,1,stage4-capability,Production-corrected measurements are not available.,1,1\r\n",
                "WARNING,1,stage4-capability,Production-corrected measurements are not available.,1,999\r\n");
}

int main(void)
{
    char directory[PATH_MAX];
    if (!test_workspace(directory)) {
        (void)fprintf(stderr, "cannot create test workspace\n");
        return 1;
    }
    test_round_trip_and_distributions(directory);
    test_distribution_gap(directory);
    test_cross_build_statistic_rounding(directory);
    test_numeric_locale_and_subnormal(directory);
    test_reader_caps(directory);
    test_hostile_profiles(directory);
    CHECK(test_remove_directory(directory) == 0,
          "cannot remove test workspace");
    if (failures != 0) {
        (void)fprintf(stderr, "%d Stage 4 persistence test(s) failed\n",
                      failures);
        return 1;
    }
    return 0;
}

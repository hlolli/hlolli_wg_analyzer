#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "hlolli_wg_analyzer.h"
#include "alignment_file.h"
#include "physical_file.h"
#include "sha256.h"

#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
#define HWA_TEST_PID _getpid
#else
#include <unistd.h>
#define HWA_TEST_PID getpid
#endif

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

static int test_source(HWAPhysicalSource *source,
                       uint64_t id,
                       const char *role,
                       const char *path,
                       char hash_byte,
                       int wave)
{
    memset(source, 0, sizeof(*source));
    source->id = id;
    source->role = test_copy(role);
    source->path = test_copy(path);
    test_hash(source->sha256, hash_byte);
    source->is_wave = wave;
    if (wave) {
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
    }
    return source->role != NULL && source->path != NULL;
}

static int test_check(HWAPhysicalCheck *check,
                      uint64_t id,
                      const char *scope,
                      const char *case_id,
                      HWAPhysicalCheckKind kind,
                      HWAPhysicalUnit unit,
                      HWAPhysicalAvailability availability)
{
    memset(check, 0, sizeof(*check));
    check->id = id;
    check->scope = test_copy(scope);
    check->case_id = test_copy(case_id);
    check->element = test_copy("");
    check->kind = kind;
    check->unit = unit;
    check->availability = availability;
    check->confidence = availability == HWA_PHYSICAL_AVAILABLE ? 0.8 : 0.0;
    return check->scope != NULL && check->case_id != NULL &&
           check->element != NULL;
}

static int test_body_mode(HWAPhysicalCheck *checks,
                          uint64_t first_id,
                          uint32_t mode_index)
{
    static const HWAPhysicalCheckKind kinds[] = {
        HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ,
        HWA_PHYSICAL_BODY_MODE_BANDWIDTH_HZ,
        HWA_PHYSICAL_BODY_MODE_Q,
        HWA_PHYSICAL_BODY_MODE_PROMINENCE_DB,
        HWA_PHYSICAL_BODY_MODE_DECAY_SECONDS
    };
    static const HWAPhysicalUnit units[] = {
        HWA_PHYSICAL_UNIT_HZ,
        HWA_PHYSICAL_UNIT_HZ,
        HWA_PHYSICAL_UNIT_RATIO,
        HWA_PHYSICAL_UNIT_DB,
        HWA_PHYSICAL_UNIT_SECONDS
    };
    size_t fact;
    for (fact = 0U; fact < sizeof(kinds) / sizeof(kinds[0]); ++fact) {
        HWAPhysicalCheck *check = &checks[fact];
        if (!test_check(check, first_id + (uint64_t)fact, "body", "body-01",
                        kinds[fact], units[fact], HWA_PHYSICAL_AVAILABLE)) {
            return 0;
        }
        check->index = mode_index;
        check->reference_value = 1.0 + (double)mode_index;
        check->reference_valid = 1;
        check->evidence_flags = HWA_PHYSICAL_EVIDENCE_WAVE_SAMPLES |
                                HWA_PHYSICAL_EVIDENCE_SPECTRUM |
                                HWA_PHYSICAL_EVIDENCE_BODY_RESPONSE;
    }
    return 1;
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
    case HWA_PHYSICAL_FIXED_TONE_PROMINENCE_DB:
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
    case HWA_PHYSICAL_MAX_STEP_DBFS:
    case HWA_PHYSICAL_RETURN_LEVEL_DBFS:
        return HWA_PHYSICAL_UNIT_DBFS;
    case HWA_PHYSICAL_RENDER_LAG_SAMPLES:
        return HWA_PHYSICAL_UNIT_SAMPLES;
    case HWA_PHYSICAL_RUNAWAY_SLOPE_DB_PER_SECOND:
        return HWA_PHYSICAL_UNIT_DB_PER_SECOND;
    default:
        return HWA_PHYSICAL_UNIT_RATIO;
    }
}

static int test_unavailable_family(HWAPhysicalCheck *checks,
                                   size_t first_index,
                                   const char *scope,
                                   const char *case_id,
                                   HWAPhysicalCheckKind first,
                                   HWAPhysicalCheckKind last)
{
    int value;
    for (value = (int)first; value <= (int)last; ++value) {
        size_t index = first_index + (size_t)(value - (int)first);
        if (!test_check(&checks[index], (uint64_t)index + 1U,
                        scope, case_id, (HWAPhysicalCheckKind)value,
                        test_kind_unit((HWAPhysicalCheckKind)value),
                        HWA_PHYSICAL_UNAVAILABLE)) return 0;
    }
    return 1;
}

static int test_scan_family(HWAPhysicalCheck *checks,
                            size_t first_index,
                            const char *role,
                            int reference_dc)
{
    const char *case_id = strrchr(role, ':');
    int value;
    if (case_id == NULL) return 0;
    case_id++;
    for (value = (int)HWA_PHYSICAL_DC_OFFSET;
         value <= (int)HWA_PHYSICAL_DENORMAL_FRACTION; ++value) {
        HWAPhysicalCheckKind kind = (HWAPhysicalCheckKind)value;
        size_t index = first_index +
                       (size_t)(value - (int)HWA_PHYSICAL_DC_OFFSET);
        HWAPhysicalCheck *check = &checks[index];
        double measured = kind == HWA_PHYSICAL_MAX_STEP_DBFS ||
                                  kind == HWA_PHYSICAL_RETURN_LEVEL_DBFS
                              ? -100.0 : 0.0;
        int reference = strncmp(role, "reference:", 10U) == 0;
        if (!test_check(check, (uint64_t)index + 1U, role, case_id,
                        kind, test_kind_unit(kind),
                        HWA_PHYSICAL_AVAILABLE)) return 0;
        if (reference_dc && kind == HWA_PHYSICAL_DC_OFFSET) measured = 0.01;
        if (reference) {
            check->reference_value = measured;
            check->reference_valid = 1;
        } else {
            check->model_value = measured;
            check->model_valid = 1;
        }
        check->evidence_flags = HWA_PHYSICAL_EVIDENCE_WAVE_SAMPLES;
    }
    return 1;
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

static void test_score_extremes(void)
{
    HWAPhysicalCheck check;
    HWAPhysicalFindingClass finding_class;
    HWAPhysicalSeverity severity;
    const char *code;
    const char *message;
    double score = 0.0;
    memset(&check, 0, sizeof(check));
    check.availability = HWA_PHYSICAL_AVAILABLE;
    check.kind = HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ;
    check.reference_value = DBL_MIN;
    check.model_value = DBL_MAX;
    check.delta = check.model_value - check.reference_value;
    check.reference_valid = 1;
    check.model_valid = 1;
    check.delta_valid = 1;
    CHECK(hwa_physical_scored_finding_for_check(
              &check, &finding_class, &severity, &code, &message,
              &score) == 1 && score == 1.0 &&
              severity == HWA_PHYSICAL_SEVERITY_CRITICAL,
          "extreme finite frequency gap was hidden");
    check.kind = HWA_PHYSICAL_BODY_MODE_BANDWIDTH_HZ;
    CHECK(hwa_physical_scored_finding_for_check(
              &check, &finding_class, &severity, &code, &message,
              &score) == 1 && score == 1.0,
          "positive score overflow did not saturate");
    check.kind = HWA_PHYSICAL_ELEMENT_GAIN_ONLY_SCORE;
    check.reference_value = 0.9;
    check.model_value = 0.9;
    check.delta = 0.0;
    CHECK(hwa_physical_scored_finding_for_check(
              &check, &finding_class, &severity, &code, &message,
              &score) == 0,
          "equal gain-only scores formed a gap");
}

static int test_make_set(HWAPhysicalCheckSet *set)
{
    memset(set, 0, sizeof(*set));
    hwa_physical_options_default(&set->options);
    set->reference_measures_path = test_copy("/not-opened/reference.hwa-measures");
    set->model_measures_path = test_copy("/not-opened/model.hwa-measures");
    test_hash(set->reference_measures_sha256, 'a');
    test_hash(set->model_measures_sha256, 'b');
    set->retained_work_bytes = 0U;
    set->pair_evaluations = 22U;
    set->transform_count = 3U;
    set->source_count = 5U;
    set->check_count = 71U;
    set->finding_count = 4U;
    set->warning_count = 1U;
    set->sources = (HWAPhysicalSource *)calloc(
        set->source_count, sizeof(*set->sources));
    set->checks = (HWAPhysicalCheck *)calloc(
        set->check_count, sizeof(*set->checks));
    set->findings = (HWAPhysicalFinding *)calloc(
        set->finding_count, sizeof(*set->findings));
    set->warnings = (HWAPhysicalWarning *)calloc(
        set->warning_count, sizeof(*set->warnings));
    if (set->reference_measures_path == NULL ||
        set->model_measures_path == NULL || set->sources == NULL ||
        set->checks == NULL || set->findings == NULL ||
        set->warnings == NULL ||
        !test_source(&set->sources[0], 1U, "reference:profile",
                     set->reference_measures_path, 'a', 0) ||
        !test_source(&set->sources[1], 2U, "model:profile",
                     set->model_measures_path, 'b', 0) ||
        !test_source(&set->sources[2], 3U, "model:scan:scale",
                     "/not-opened/raw-model.wav", 'd', 1) ||
        !test_source(&set->sources[3], 4U, "reference:body:body-01",
                     "/not-opened/body.wav", 'c', 1) ||
        !test_source(&set->sources[4], 5U, "reference:scan:scale",
                     "/not-opened/raw-\xff.wav", 'e', 1) ||
        !test_check(&set->checks[0], 1U, "profiles", "",
                    HWA_PHYSICAL_ELEMENT_CARRYOVER_DB,
                    HWA_PHYSICAL_UNIT_DB, HWA_PHYSICAL_UNAVAILABLE) ||
        !test_check(&set->checks[1], 2U, "profiles", "",
                    HWA_PHYSICAL_ELEMENT_TRAIT_DELTA,
                    HWA_PHYSICAL_UNIT_RATIO, HWA_PHYSICAL_AVAILABLE) ||
        !test_scan_family(set->checks, 2U,
                          "model:scan:scale", 0) ||
        !test_scan_family(set->checks, 15U,
                          "reference:body:body-01", 0) ||
        !test_scan_family(set->checks, 28U,
                          "reference:scan:scale", 1) ||
        !test_body_mode(&set->checks[41], 42U, 0U) ||
        !test_body_mode(&set->checks[46], 47U, 1U) ||
        !test_check(&set->checks[51], 52U, "body", "body-01",
                    HWA_PHYSICAL_BODY_MODE_PAN,
                    HWA_PHYSICAL_UNIT_RATIO, HWA_PHYSICAL_UNAVAILABLE) ||
        !test_check(&set->checks[52], 53U, "body", "body-01",
                    HWA_PHYSICAL_BODY_MODE_DENSITY_PER_KHZ,
                    HWA_PHYSICAL_UNIT_COUNT_VALUE,
                    HWA_PHYSICAL_AVAILABLE) ||
        !test_check(&set->checks[53], 54U, "body", "body-01",
                    HWA_PHYSICAL_BODY_MODE_DISTANCE_CENTS,
                    HWA_PHYSICAL_UNIT_CENTS, HWA_PHYSICAL_UNAVAILABLE) ||
        !test_unavailable_family(
            set->checks, 54U, "joint", "",
            HWA_PHYSICAL_JOINT_RESIDUAL_DB,
            HWA_PHYSICAL_PITCH_PULL_CENTS) ||
        !test_unavailable_family(
            set->checks, 63U, "render", "",
            HWA_PHYSICAL_RENDER_RMS_ERROR_DB,
            HWA_PHYSICAL_RENDER_SPECTRAL_DISTANCE_DB)) {
        hwa_physical_check_set_free(set);
        return 0;
    }
    set->checks[1].reference_value = 0.1;
    set->checks[1].model_value = 0.5;
    set->checks[1].delta = set->checks[1].model_value -
                           set->checks[1].reference_value;
    set->checks[1].reference_valid = 1;
    set->checks[1].model_valid = 1;
    set->checks[1].delta_valid = 1;
    set->checks[1].evidence_flags =
        HWA_PHYSICAL_EVIDENCE_REFERENCE_PROFILE |
        HWA_PHYSICAL_EVIDENCE_MODEL_PROFILE |
        HWA_PHYSICAL_EVIDENCE_ELEMENT_LABEL;
    set->checks[1].element = (free(set->checks[1].element),
                              test_copy("element,A"));
    if (set->checks[1].element == NULL) {
        hwa_physical_check_set_free(set);
        return 0;
    }
    set->checks[52].reference_value = 1.0;
    set->checks[52].reference_valid = 1;
    if (!test_scored_finding(&set->findings[0], 1U, 1U,
                             &set->checks[28]) ||
        !test_scored_finding(&set->findings[1], 2U, 2U,
                             &set->checks[1]) ||
        !test_missing_finding(&set->findings[2], 3U,
                              &set->checks[54]) ||
        !test_missing_finding(&set->findings[3], 4U,
                              &set->checks[63])) {
        hwa_physical_check_set_free(set);
        return 0;
    }
    set->warnings[0].id = 1U;
    set->warnings[0].code = test_copy("weak-evidence");
    set->warnings[0].message = test_copy("weak \"raw\",\r\nevidence");
    set->warnings[0].source_id = 5U;
    set->warnings[0].check_id = 29U;
    set->warnings[0].source_id_valid = 1;
    set->warnings[0].check_id_valid = 1;
    if (set->findings[0].code == NULL || set->findings[0].message == NULL ||
        set->findings[1].code == NULL || set->findings[1].message == NULL ||
        set->warnings[0].code == NULL || set->warnings[0].message == NULL) {
        hwa_physical_check_set_free(set);
        return 0;
    }
    return hwa_physical_check_set_retained_bytes(
               set, &set->retained_work_bytes) == 0;
}

static void test_mode_stress_and_retained_work(void)
{
    const size_t mode_count = 1024U;
    const size_t body_first = 15U;
    const size_t scalar_first = body_first + mode_count * 5U;
    const size_t joint_first = scalar_first + 3U;
    const size_t render_first = joint_first + 9U;
    HWAPhysicalCheckSet set;
    FILE *stream;
    char error[HWA_ERROR_SIZE] = {0};
    size_t mode;
    memset(&set, 0, sizeof(set));
    hwa_physical_options_default(&set.options);
    set.options.max_modes = mode_count;
    set.reference_measures_path = test_copy("/not-opened/stress-reference");
    set.model_measures_path = test_copy("/not-opened/stress-model");
    test_hash(set.reference_measures_sha256, '1');
    test_hash(set.model_measures_sha256, '2');
    set.source_count = 3U;
    set.check_count = render_first + 8U;
    set.finding_count = 2U;
    set.sources = (HWAPhysicalSource *)calloc(
        set.source_count, sizeof(*set.sources));
    set.checks = (HWAPhysicalCheck *)calloc(
        set.check_count, sizeof(*set.checks));
    set.findings = (HWAPhysicalFinding *)calloc(
        set.finding_count, sizeof(*set.findings));
    if (set.reference_measures_path == NULL ||
        set.model_measures_path == NULL || set.sources == NULL ||
        set.checks == NULL || set.findings == NULL ||
        !test_source(&set.sources[0], 1U, "reference:profile",
                     set.reference_measures_path, '1', 0) ||
        !test_source(&set.sources[1], 2U, "model:profile",
                     set.model_measures_path, '2', 0) ||
        !test_source(&set.sources[2], 3U, "reference:body:body-01",
                     "/not-opened/stress-body.wav", '3', 1) ||
        !test_check(&set.checks[0], 1U, "profiles", "",
                    HWA_PHYSICAL_ELEMENT_TRAIT_DELTA,
                    HWA_PHYSICAL_UNIT_RATIO, HWA_PHYSICAL_INSUFFICIENT) ||
        !test_check(&set.checks[1], 2U, "profiles", "",
                    HWA_PHYSICAL_ELEMENT_CARRYOVER_DB,
                    HWA_PHYSICAL_UNIT_DB, HWA_PHYSICAL_UNAVAILABLE) ||
        !test_scan_family(set.checks, 2U,
                          "reference:body:body-01", 0)) {
        CHECK(0, "mode stress fixture allocation failed");
        hwa_physical_check_set_free(&set);
        return;
    }
    for (mode = 0U; mode < mode_count; ++mode) {
        size_t first = body_first + mode * 5U;
        if (!test_body_mode(&set.checks[first],
                            (uint64_t)first + 1U,
                            (uint32_t)mode)) {
            CHECK(0, "mode stress row allocation failed");
            hwa_physical_check_set_free(&set);
            return;
        }
    }
    if (!test_check(&set.checks[scalar_first],
                    (uint64_t)scalar_first + 1U, "body", "body-01",
                    HWA_PHYSICAL_BODY_MODE_PAN,
                    HWA_PHYSICAL_UNIT_RATIO, HWA_PHYSICAL_UNAVAILABLE) ||
        !test_check(&set.checks[scalar_first + 1U],
                    (uint64_t)scalar_first + 2U, "body", "body-01",
                    HWA_PHYSICAL_BODY_MODE_DENSITY_PER_KHZ,
                    HWA_PHYSICAL_UNIT_COUNT_VALUE,
                    HWA_PHYSICAL_AVAILABLE) ||
        !test_check(&set.checks[scalar_first + 2U],
                    (uint64_t)scalar_first + 3U, "body", "body-01",
                    HWA_PHYSICAL_BODY_MODE_DISTANCE_CENTS,
                    HWA_PHYSICAL_UNIT_CENTS, HWA_PHYSICAL_UNAVAILABLE) ||
        !test_unavailable_family(
            set.checks, joint_first, "joint", "",
            HWA_PHYSICAL_JOINT_RESIDUAL_DB,
            HWA_PHYSICAL_PITCH_PULL_CENTS) ||
        !test_unavailable_family(
            set.checks, render_first, "render", "",
            HWA_PHYSICAL_RENDER_RMS_ERROR_DB,
            HWA_PHYSICAL_RENDER_SPECTRAL_DISTANCE_DB) ||
        !test_missing_finding(&set.findings[0], 1U,
                              &set.checks[joint_first]) ||
        !test_missing_finding(&set.findings[1], 2U,
                              &set.checks[render_first])) {
        CHECK(0, "mode stress catalog allocation failed");
        hwa_physical_check_set_free(&set);
        return;
    }
    set.checks[scalar_first + 1U].reference_value = 1.0;
    set.checks[scalar_first + 1U].reference_valid = 1;
    if (hwa_physical_check_set_retained_bytes(
            &set, &set.retained_work_bytes) != 0) {
        CHECK(0, "mode stress retained-byte count failed");
        hwa_physical_check_set_free(&set);
        return;
    }
    set.options.max_work_bytes = set.retained_work_bytes - 1U;
    stream = tmpfile();
    CHECK(stream != NULL && hwa_physical_file_write(
              stream, &set, error, sizeof(error)) != 0 &&
              ftell(stream) == 0L,
          "one-under retained-work writer cap passed");
    if (stream != NULL) (void)fclose(stream);
    set.options.max_work_bytes++;
    stream = tmpfile();
    CHECK(stream != NULL && hwa_physical_file_write(
              stream, &set, error, sizeof(error)) == 0,
          "exact retained-work writer cap failed: %s", error);
    if (stream != NULL) (void)fclose(stream);
    hwa_physical_check_set_free(&set);
}

static int test_path(char *path, size_t path_size, const char *name)
{
    static unsigned serial;
    int written = snprintf(path, path_size,
                           "/tmp/hwa-stage5-%ld-%u-%s",
                           (long)HWA_TEST_PID(), serial++, name);
    return written > 0 && (size_t)written < path_size;
}

static int test_write(const char *path, const HWAPhysicalCheckSet *set)
{
    FILE *stream = fopen(path, "wb");
    char error[HWA_ERROR_SIZE] = {0};
    int result;
    if (stream == NULL) return 0;
    result = hwa_physical_file_write(stream, set, error, sizeof(error));
    if (fclose(stream) != 0) result = -1;
    if (result != 0) (void)fprintf(stderr, "write: %s\n", error);
    return result == 0;
}

static unsigned char *test_read_bytes(const char *path, size_t *size)
{
    FILE *stream = fopen(path, "rb");
    long length;
    unsigned char *bytes;
    if (stream == NULL || fseek(stream, 0L, SEEK_END) != 0 ||
        (length = ftell(stream)) < 0 || fseek(stream, 0L, SEEK_SET) != 0) {
        if (stream != NULL) (void)fclose(stream);
        return NULL;
    }
    bytes = (unsigned char *)malloc((size_t)length + 1U);
    if (bytes == NULL ||
        fread(bytes, 1U, (size_t)length, stream) != (size_t)length ||
        fclose(stream) != 0) {
        free(bytes);
        return NULL;
    }
    bytes[length] = 0U;
    *size = (size_t)length;
    return bytes;
}

static int test_same_file(const char *left, const char *right)
{
    size_t left_size;
    size_t right_size;
    unsigned char *left_bytes = test_read_bytes(left, &left_size);
    unsigned char *right_bytes = test_read_bytes(right, &right_size);
    int same = left_bytes != NULL && right_bytes != NULL &&
               left_size == right_size &&
               memcmp(left_bytes, right_bytes, left_size) == 0;
    free(left_bytes);
    free(right_bytes);
    return same;
}

static void test_round_trip(void)
{
    HWAPhysicalCheckSet source;
    HWAPhysicalCheckSet loaded;
    HWAPhysicalOptions limits;
    char first[512];
    char second[512];
    char hash[HWA_SHA256_HEX_SIZE];
    char expected[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE] = {0};
    int read_ok;
    CHECK(test_path(first, sizeof(first), "first.hwa-physical") &&
              test_path(second, sizeof(second), "second.hwa-physical"),
          "temporary paths failed");
    if (!test_make_set(&source)) {
        CHECK(0, "fixture allocation failed");
        return;
    }
    if (!test_write(first, &source)) {
        CHECK(0, "canonical write failed");
        hwa_physical_check_set_free(&source);
        return;
    }
    hwa_physical_options_default(&limits);
    memset(&loaded, 0, sizeof(loaded));
    CHECK(hwa_sha256_file(first, UINT64_MAX, expected,
                          error, sizeof(error)) == 0,
          "fixture hash failed: %s", error);
    read_ok = hwa_physical_file_read(first, &limits, &loaded, hash,
                                     error, sizeof(error)) == 0;
    CHECK(read_ok,
          "canonical read failed: %s", error);
    if (!read_ok) {
        hwa_physical_check_set_free(&source);
        hwa_physical_check_set_free(&loaded);
        (void)remove(first);
        return;
    }
    CHECK(strcmp(hash, expected) == 0, "reader returned wrong hash");
    CHECK(loaded.source_count == 5U && loaded.check_count == 71U &&
              loaded.finding_count == 4U && loaded.warning_count == 1U,
          "reader lost rows");
    CHECK(strcmp(loaded.sources[4].path, "/not-opened/raw-\xff.wav") == 0,
          "byte path did not round trip");
    CHECK(strcmp(loaded.warnings[0].message,
                 "weak \"raw\",\r\nevidence") == 0,
          "quoted text did not round trip");
    CHECK(test_write(second, &loaded), "second canonical write failed");
    CHECK(test_same_file(first, second),
          "reader/writer round trip changed canonical bytes");
    hwa_physical_check_set_free(&loaded);
    hwa_physical_check_set_free(&loaded);
    hwa_physical_check_set_free(&source);
    (void)remove(first);
    (void)remove(second);
}

static void test_numeric_locale_round_trip(void)
{
    HWAPhysicalCheckSet source;
    HWAPhysicalCheckSet loaded;
    HWAPhysicalOptions limits;
    const char *current = setlocale(LC_NUMERIC, NULL);
    char saved[128];
    char path[512];
    char hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE] = {0};
    unsigned char *bytes = NULL;
    size_t byte_count = 0U;
    int locale_ready = 0;
    if (current == NULL || strlen(current) >= sizeof(saved)) return;
    memcpy(saved, current, strlen(current) + 1U);
    CHECK(test_path(path, sizeof(path), "locale.hwa-physical"),
          "numeric-locale path failed");
    if (failures != 0 || !test_make_set(&source)) {
        CHECK(0, "numeric-locale fixture failed");
        return;
    }
    memset(&loaded, 0, sizeof(loaded));
    locale_ready = test_set_comma_numeric_locale();
    if (locale_ready) {
        hwa_physical_options_default(&limits);
        CHECK(test_write(path, &source),
              "comma-locale physical write failed");
        CHECK(strcmp(localeconv()->decimal_point, ",") == 0,
              "physical writer changed the caller locale");
        bytes = test_read_bytes(path, &byte_count);
        CHECK(bytes != NULL && byte_count != 0U &&
                  strstr((const char *)bytes,
                         "0.10000000000000001,0.5") != NULL,
              "physical writer used the caller's decimal separator");
        free(bytes);
        CHECK(hwa_physical_file_read(
                  path, &limits, &loaded, hash, error, sizeof(error)) == 0,
              "comma-locale physical read failed: %s", error);
        CHECK(strcmp(localeconv()->decimal_point, ",") == 0,
              "physical reader changed the caller locale");
    }
    CHECK(setlocale(LC_NUMERIC, saved) != NULL,
          "cannot restore numeric locale after persistence test");
    hwa_physical_check_set_free(&loaded);
    hwa_physical_check_set_free(&source);
    (void)remove(path);
}

static void test_subnormal_round_trip(void)
{
    HWAPhysicalCheckSet source;
    HWAPhysicalCheckSet loaded;
    HWAPhysicalOptions limits;
    char path[512];
    char hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE] = {0};
    CHECK(test_path(path, sizeof(path), "subnormal.hwa-physical"),
          "subnormal path failed");
    if (!test_make_set(&source)) {
        CHECK(0, "subnormal fixture failed");
        return;
    }
    memset(&loaded, 0, sizeof(loaded));
    source.checks[1].confidence = DBL_TRUE_MIN;
    CHECK(test_write(path, &source),
          "DBL_TRUE_MIN physical write failed");
    hwa_physical_options_default(&limits);
    CHECK(hwa_physical_file_read(
              path, &limits, &loaded, hash, error, sizeof(error)) == 0,
          "DBL_TRUE_MIN physical read failed: %s", error);
    CHECK(loaded.check_count > 1U &&
              loaded.checks[1].confidence == DBL_TRUE_MIN,
          "DBL_TRUE_MIN did not round trip");
    hwa_physical_check_set_free(&loaded);
    hwa_physical_check_set_free(&source);
    (void)remove(path);
}

static int test_write_bytes(const char *path,
                            const unsigned char *bytes,
                            size_t size)
{
    FILE *stream = fopen(path, "wb");
    int okay = stream != NULL && fwrite(bytes, 1U, size, stream) == size;
    if (stream != NULL && fclose(stream) != 0) okay = 0;
    return okay;
}

static unsigned char *test_replace_once(const unsigned char *bytes,
                                        size_t size,
                                        const char *needle,
                                        const char *replacement,
                                        size_t *changed_size)
{
    const char *found = strstr((const char *)bytes, needle);
    size_t needle_size = strlen(needle);
    size_t replacement_size = strlen(replacement);
    size_t before;
    unsigned char *changed;
    if (found == NULL) return NULL;
    before = (size_t)(found - (const char *)bytes);
    *changed_size = size - needle_size + replacement_size;
    changed = (unsigned char *)malloc(*changed_size);
    if (changed == NULL) return NULL;
    memcpy(changed, bytes, before);
    memcpy(changed + before, replacement, replacement_size);
    memcpy(changed + before + replacement_size,
           bytes + before + needle_size, size - before - needle_size);
    return changed;
}

static void test_compatible_replacement(const char *base,
                                        const char *needle,
                                        const char *replacement,
                                        const char *name)
{
    HWAPhysicalCheckSet loaded;
    HWAPhysicalOptions limits;
    unsigned char *bytes;
    unsigned char *changed;
    char changed_path[512];
    char normalized_path[512];
    char normalized_name[192];
    char hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE] = {0};
    size_t size;
    size_t changed_size = 0U;
    int written = snprintf(normalized_name, sizeof(normalized_name),
                           "%s-normalized", name);
    CHECK(written > 0 && (size_t)written < sizeof(normalized_name) &&
              test_path(changed_path, sizeof(changed_path), name) &&
              test_path(normalized_path, sizeof(normalized_path),
                        normalized_name),
          "compatible replacement paths failed: %s", name);
    if (written <= 0 || (size_t)written >= sizeof(normalized_name)) return;
    bytes = test_read_bytes(base, &size);
    changed = bytes == NULL ? NULL : test_replace_once(
        bytes, size, needle, replacement, &changed_size);
    CHECK(changed != NULL, "compatible replacement needle missing: %s",
          needle);
    if (changed == NULL) {
        free(bytes);
        return;
    }
    CHECK(test_write_bytes(changed_path, changed, changed_size),
          "compatible replacement write failed: %s", name);
    hwa_physical_options_default(&limits);
    memset(&loaded, 0, sizeof(loaded));
    CHECK(hwa_physical_file_read(changed_path, &limits, &loaded, hash,
                                 error, sizeof(error)) == 0,
          "compatible replacement was rejected (%s): %s", name, error);
    if (loaded.sources != NULL) {
        CHECK(test_write(normalized_path, &loaded),
              "compatible replacement did not rewrite: %s", name);
        CHECK(test_same_file(base, normalized_path),
              "compatible replacement did not normalize: %s", name);
    }
    hwa_physical_check_set_free(&loaded);
    free(changed);
    free(bytes);
    (void)remove(changed_path);
    (void)remove(normalized_path);
}

static void test_bad_replacement(const char *base,
                                 const char *needle,
                                 const char *replacement,
                                 const char *name)
{
    HWAPhysicalCheckSet loaded;
    HWAPhysicalOptions limits;
    unsigned char *bytes;
    unsigned char *changed;
    char path[512];
    char hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE] = {0};
    char *found;
    size_t size;
    size_t before;
    size_t needle_size = strlen(needle);
    size_t replacement_size = strlen(replacement);
    CHECK(test_path(path, sizeof(path), name), "bad path failed");
    bytes = test_read_bytes(base, &size);
    found = bytes == NULL ? NULL : strstr((char *)bytes, needle);
    CHECK(found != NULL, "tamper needle missing: %s", needle);
    if (found == NULL) {
        free(bytes);
        return;
    }
    before = (size_t)(found - (char *)bytes);
    changed = (unsigned char *)malloc(
        size - needle_size + replacement_size);
    CHECK(changed != NULL, "tamper allocation failed");
    if (changed != NULL) {
        memcpy(changed, bytes, before);
        memcpy(changed + before, replacement, replacement_size);
        memcpy(changed + before + replacement_size,
               bytes + before + needle_size,
               size - before - needle_size);
        CHECK(test_write_bytes(path, changed,
                               size - needle_size + replacement_size),
              "tamper write failed");
        hwa_physical_options_default(&limits);
        memset(&loaded, 0, sizeof(loaded));
        CHECK(hwa_physical_file_read(path, &limits, &loaded, hash,
                                     error, sizeof(error)) != 0,
              "hostile physical row passed: %s", name);
        hwa_physical_check_set_free(&loaded);
        (void)remove(path);
    }
    free(changed);
    free(bytes);
}

static void test_compatible_meta(const char *base,
                                 const char *key,
                                 const char *current_value,
                                 const char *saved_value)
{
    HWAPhysicalCheckSet loaded;
    HWAPhysicalOptions limits;
    unsigned char *bytes;
    unsigned char *changed;
    char changed_path[512];
    char rewritten_path[512];
    char changed_name[160];
    char rewritten_name[160];
    char needle[1024];
    char replacement[1024];
    char hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE] = {0};
    char *found;
    size_t size;
    size_t before;
    size_t needle_size;
    size_t replacement_size;
    size_t changed_size;
    int written;
    int read_ok;
    written = snprintf(changed_name, sizeof(changed_name),
                       "compatible-%s.hwa-physical", key);
    CHECK(written > 0 && (size_t)written < sizeof(changed_name),
          "compatible metadata name failed");
    if (written <= 0 || (size_t)written >= sizeof(changed_name)) return;
    written = snprintf(rewritten_name, sizeof(rewritten_name),
                       "compatible-%s-rewritten.hwa-physical", key);
    CHECK(written > 0 && (size_t)written < sizeof(rewritten_name),
          "compatible metadata rewrite name failed");
    if (written <= 0 || (size_t)written >= sizeof(rewritten_name)) return;
    written = snprintf(needle, sizeof(needle), "META,%s,%s,",
                       key, current_value);
    CHECK(written > 0 && (size_t)written < sizeof(needle),
          "compatible metadata needle failed");
    if (written <= 0 || (size_t)written >= sizeof(needle)) return;
    written = snprintf(replacement, sizeof(replacement), "META,%s,%s,",
                       key, saved_value);
    CHECK(written > 0 && (size_t)written < sizeof(replacement),
          "compatible metadata replacement failed");
    if (written <= 0 || (size_t)written >= sizeof(replacement)) return;
    if (!test_path(changed_path, sizeof(changed_path), changed_name) ||
        !test_path(rewritten_path, sizeof(rewritten_path), rewritten_name)) {
        CHECK(0, "compatible metadata paths failed");
        return;
    }
    bytes = test_read_bytes(base, &size);
    found = bytes == NULL ? NULL : strstr((char *)bytes, needle);
    CHECK(found != NULL, "compatible metadata row missing: %s", key);
    if (found == NULL) {
        free(bytes);
        return;
    }
    before = (size_t)(found - (char *)bytes);
    needle_size = strlen(needle);
    replacement_size = strlen(replacement);
    changed_size = size - needle_size + replacement_size;
    changed = (unsigned char *)malloc(changed_size);
    CHECK(changed != NULL, "compatible metadata allocation failed");
    if (changed == NULL) {
        free(bytes);
        return;
    }
    memcpy(changed, bytes, before);
    memcpy(changed + before, replacement, replacement_size);
    memcpy(changed + before + replacement_size,
           bytes + before + needle_size, size - before - needle_size);
    CHECK(test_write_bytes(changed_path, changed, changed_size),
          "compatible metadata write failed: %s", key);
    free(changed);
    free(bytes);
    hwa_physical_options_default(&limits);
    memset(&loaded, 0, sizeof(loaded));
    read_ok = hwa_physical_file_read(changed_path, &limits, &loaded, hash,
                                     error, sizeof(error)) == 0;
    CHECK(read_ok, "compatible producer metadata was rejected (%s): %s",
          key, error);
    if (read_ok) {
        CHECK(test_write(rewritten_path, &loaded),
              "compatible producer result did not rewrite: %s", key);
        bytes = test_read_bytes(rewritten_path, &size);
        CHECK(bytes != NULL && strstr((char *)bytes, needle) != NULL,
              "rewritten result did not use current metadata: %s", key);
        free(bytes);
    }
    hwa_physical_check_set_free(&loaded);
    (void)remove(changed_path);
    (void)remove(rewritten_path);
}

static void test_producer_compatibility(const char *base)
{
    char pointer_bits[32];
    (void)snprintf(pointer_bits, sizeof(pointer_bits), "%u",
                   hwa_build_pointer_bits());
    test_compatible_meta(base, "tool_version", HWA_VERSION, "0.6.99");
    test_compatible_meta(base, "build_compiler_family",
                         hwa_build_compiler_family(), "other-compiler");
    test_compatible_meta(base, "build_compiler_version",
                         hwa_build_compiler_version(), "999");
    test_compatible_meta(base, "build_c_standard",
                         hwa_build_c_standard(), "C-other");
    test_compatible_meta(base, "build_target_os",
                         hwa_build_target_os(), "other-os");
    test_compatible_meta(base, "build_pointer_bits", pointer_bits, "7");
    test_compatible_meta(base, "build_endianness",
                         hwa_build_endianness(), "mixed");
    test_compatible_meta(base, "build_mode", hwa_build_mode(), "Other");
}

static void test_hostile_and_caps(void)
{
    HWAPhysicalCheckSet source;
    HWAPhysicalCheckSet loaded;
    HWAPhysicalOptions limits;
    char path[512];
    char hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE] = {0};
    char retained[96];
    char retained_less[96];
    char saved_work[96];
    char saved_work_too_small[96];
    char pointer_bits[96];
    char score_needle[160];
    char score_near[160];
    char score_far[160];
    double near_score = nextafter(0.2, INFINITY);
    double far_score = 0.2;
    unsigned step;
    CHECK(test_path(path, sizeof(path), "hostile.hwa-physical"),
          "hostile path failed");
    if (!test_make_set(&source)) {
        CHECK(0, "hostile fixture allocation failed");
        return;
    }
    if (!test_write(path, &source)) {
        CHECK(0, "hostile fixture write failed");
        hwa_physical_check_set_free(&source);
        return;
    }
    test_bad_replacement(path, "HWA_PHYSICAL,1",
                         "HWA_PHYSICAL,2", "bad-magic");
    test_bad_replacement(path, "physical_check_method_version,stage5-1",
                         "physical_check_method_version,stage5-x",
                         "bad-method");
    test_producer_compatibility(path);
    test_bad_replacement(path, ",aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                         ",Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                         "bad-hash");
    test_bad_replacement(path, "WARNING,1,weak-evidence",
                         "CHECK,1,weak-evidence", "bad-section");
    test_bad_replacement(path, "model:scan:scale",
                         "reference:scan:zeta", "bad-source-order");
    test_bad_replacement(
        path, "INPUT,4,reference:body:body-01,",
        "INPUT,4,reference:body:body-02,",
        "bound-case-without-family");
    test_bad_replacement(
        path, "CHECK,42,body,body-01,,body_mode_frequency_hz,",
        "CHECK,42,body,forged,,body_mode_frequency_hz,",
        "family-case-without-binding");
    test_bad_replacement(
        path,
        "CHECK,29,reference:scan:scale,scale,,dc_offset,0,ratio,",
        "CHECK,29,profiles,,\"element,A\",element_trait_delta,0,ratio,",
        "duplicate-check-key");
    test_bad_replacement(
        path,
        "CHECK,42,body,body-01,,body_mode_frequency_hz,0,Hz,",
        "CHECK,42,body,body-01,,body_mode_q,0,ratio,",
        "bad-check-order");
    test_bad_replacement(
        path,
        "CHECK,29,reference:scan:scale,scale,,dc_offset,0,ratio,"
        "available,0.01,,,0.80000000000000004,8,0,1,0,0",
        "CHECK,29,reference:scan:scale,scale,,clip_fraction,0,ratio,"
        "available,2,,,0.80000000000000004,8,0,1,0,0",
        "fraction-out-of-range");
    test_bad_replacement(
        path,
        "available,0.01,,,0.80000000000000004,8,0,1,0,0",
        "available,,0.01,,0.80000000000000004,8,0,0,1,0",
        "scan-value-on-wrong-side");
    test_bad_replacement(path, "spectral_floor_dbfs,-100,dBFS",
                         "spectral_floor_dbfs,nan,dBFS", "nonfinite");
    (void)snprintf(pointer_bits, sizeof(pointer_bits),
                   "build_pointer_bits,%u,bits", hwa_build_pointer_bits());
    test_bad_replacement(path, pointer_bits,
                         "build_pointer_bits,0,bits",
                         "zero-pointer-bits");
    test_bad_replacement(
        path,
        ",0.10000000000000001,0.5,0.40000000000000002,",
        ",0.10000000000000001,0.5,0.41,",
        "wrong-delta");
    test_bad_replacement(
        path,
        "available,0.01,,,0.80000000000000004,8,0,1,0,0",
        "available,,,,0.80000000000000004,8,0,0,0,0",
        "available-without-value");
    test_bad_replacement(
        path,
        "available,0.01,,,0.80000000000000004,8,0,1,0,0",
        "unavailable,,,,0.10000000000000001,8,0,0,0,0",
        "unavailable-with-confidence");
    test_bad_replacement(
        path,
        "FINDING,2,gap,info,physical-gap,"
        "A physical check exceeds its fixed review threshold.,2,"
        "0.20000000000000001,2,1,1",
        "FINDING,2,gap,info,physical-gap,"
        "A physical check exceeds its fixed review threshold.,2,"
        "1,2,1,1",
        "equal-score-check-order");
    test_bad_replacement(
        path, "FINDING,2,gap,info,physical-gap,",
        "FINDING,2,fault,info,physical-gap,",
        "forged-finding-class");
    test_bad_replacement(
        path, "FINDING,2,gap,info,physical-gap,",
        "FINDING,2,gap,warning,physical-gap,",
        "forged-finding-severity");
    test_bad_replacement(
        path, "FINDING,2,gap,info,physical-gap,",
        "FINDING,2,gap,info,forged-gap,",
        "forged-finding-code");
    test_bad_replacement(
        path,
        "threshold.,2,0.20000000000000001,2,1,1",
        "threshold.,2,0.20999999999999999,2,1,1",
        "forged-finding-score");
    for (step = 0U; step < 33U; ++step) {
        far_score = nextafter(far_score, INFINITY);
    }
    (void)snprintf(score_needle, sizeof(score_needle),
                   "threshold.,2,%.17g,2,1,1", 0.2);
    (void)snprintf(score_near, sizeof(score_near),
                   "threshold.,2,%.17g,2,1,1", near_score);
    (void)snprintf(score_far, sizeof(score_far),
                   "threshold.,2,%.17g,2,1,1", far_score);
    test_compatible_replacement(path, score_needle, score_near,
                                "compatible-score-rounding");
    test_bad_replacement(path, score_needle, score_far,
                         "far-score-rounding");
    (void)snprintf(retained, sizeof(retained),
                   "retained_work_bytes,%" PRIu64 ",bytes",
                   source.retained_work_bytes);
    (void)snprintf(retained_less, sizeof(retained_less),
                   "retained_work_bytes,%" PRIu64 ",bytes",
                   source.retained_work_bytes - 1U);
    test_compatible_replacement(path, retained, retained_less,
                                "compatible-layout-count");
    (void)snprintf(saved_work, sizeof(saved_work),
                   "max_work_bytes,%" PRIu64 ",bytes",
                   source.options.max_work_bytes);
    (void)snprintf(saved_work_too_small, sizeof(saved_work_too_small),
                   "max_work_bytes,%" PRIu64 ",bytes",
                   source.retained_work_bytes - 1U);
    test_bad_replacement(path, saved_work, saved_work_too_small,
                         "saved-work-below-provenance");
    hwa_physical_options_default(&limits);
    limits.max_wave_frames = 47999U;
    memset(&loaded, 0, sizeof(loaded));
    CHECK(hwa_physical_file_read(path, &limits, &loaded, hash,
                                 error, sizeof(error)) != 0,
          "current WAVE frame cap was not enforced");
    hwa_physical_check_set_free(&loaded);
    hwa_physical_options_default(&limits);
    limits.max_wave_bytes = 95999U;
    memset(&loaded, 0, sizeof(loaded));
    CHECK(hwa_physical_file_read(path, &limits, &loaded, hash,
                                 error, sizeof(error)) != 0,
          "current WAVE byte cap was not enforced");
    hwa_physical_check_set_free(&loaded);
    hwa_physical_options_default(&limits);
    limits.max_modes = 1U;
    memset(&loaded, 0, sizeof(loaded));
    CHECK(hwa_physical_file_read(path, &limits, &loaded, hash,
                                 error, sizeof(error)) != 0,
          "current body-mode cap was not enforced");
    hwa_physical_check_set_free(&loaded);
    hwa_physical_options_default(&limits);
    limits.max_checks = 2U;
    memset(&loaded, 0, sizeof(loaded));
    CHECK(hwa_physical_file_read(path, &limits, &loaded, hash,
                                 error, sizeof(error)) != 0,
          "check cap was not enforced");
    hwa_physical_check_set_free(&loaded);
    hwa_physical_options_default(&limits);
    limits.max_bindings = 1U;
    CHECK(hwa_physical_file_read(path, &limits, &loaded, hash,
                                 error, sizeof(error)) != 0,
          "binding cap was not enforced");
    hwa_physical_check_set_free(&loaded);
    hwa_physical_options_default(&limits);
    limits.max_work_bytes = 1024U;
    CHECK(hwa_physical_file_read(path, &limits, &loaded, hash,
                                 error, sizeof(error)) != 0,
          "work cap was not enforced");
    hwa_physical_check_set_free(&loaded);
    hwa_physical_options_default(&limits);
    limits.max_work_bytes--;
    limits.max_checks--;
    limits.profile_limits.max_work_bytes--;
    memset(&loaded, 0, sizeof(loaded));
    CHECK(hwa_physical_file_read(path, &limits, &loaded, hash,
                                 error, sizeof(error)) == 0,
          "compatible current caps were rejected: %s", error);
    if (loaded.sources != NULL) {
        FILE *rewrite = tmpfile();
        CHECK(loaded.options.max_work_bytes == limits.max_work_bytes &&
                  loaded.options.max_checks == limits.max_checks &&
                  loaded.options.profile_limits.max_work_bytes ==
                      limits.profile_limits.max_work_bytes,
              "reader did not normalize result caps");
        CHECK(rewrite != NULL && hwa_physical_file_write(
                  rewrite, &loaded, error, sizeof(error)) == 0,
              "normalized result was not writable: %s", error);
        if (rewrite != NULL) (void)fclose(rewrite);
    }
    hwa_physical_check_set_free(&loaded);
    hwa_physical_check_set_free(&source);
    (void)remove(path);
}

static void test_preflight_and_write_fault(void)
{
    HWAPhysicalCheckSet source;
    FILE *stream;
    long position;
    char *old_path;
    char *large_path;
    char *duplicate_scope;
    char *duplicate_case;
    char *duplicate_element;
    char *old_scope;
    char *old_case;
    char *old_element;
    HWAPhysicalCheckKind old_kind;
    HWAPhysicalUnit old_unit;
    size_t large_size = 32769U;
    size_t saved_max_modes;
    char error[HWA_ERROR_SIZE] = {0};
    if (!test_make_set(&source)) {
        CHECK(0, "preflight fixture failed");
        return;
    }
    stream = tmpfile();
    CHECK(stream != NULL, "tmpfile failed");
    if (stream != NULL) {
        source.findings[0].rank = 2U;
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) != 0,
              "invalid rank passed writer");
        position = ftell(stream);
        CHECK(position == 0L, "writer emitted bytes before full preflight");
        source.findings[0].rank = 1U;
        source.checks[1].confidence = NAN;
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) != 0,
              "nonfinite check passed writer");
        CHECK(ftell(stream) == 0L,
              "nonfinite preflight emitted partial output");
        source.checks[1].confidence = 0.8;
        source.checks[1].delta = 0.31;
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) != 0,
              "incorrect check delta passed writer");
        CHECK(ftell(stream) == 0L,
              "delta preflight emitted partial output");
        source.checks[1].delta = source.checks[1].model_value -
                                 source.checks[1].reference_value;
        source.checks[28].reference_value = 0.0;
        source.checks[28].reference_valid = 0;
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) != 0,
              "available check without a value passed writer");
        CHECK(ftell(stream) == 0L,
              "empty available preflight emitted partial output");
        source.checks[28].reference_value = 0.01;
        source.checks[28].reference_valid = 1;
        source.checks[28].availability = HWA_PHYSICAL_UNAVAILABLE;
        source.checks[28].reference_value = 0.0;
        source.checks[28].reference_valid = 0;
        source.checks[28].confidence = 0.1;
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) != 0,
              "unavailable check with confidence passed writer");
        CHECK(ftell(stream) == 0L,
              "unavailable confidence preflight emitted partial output");
        source.checks[28].availability = HWA_PHYSICAL_AVAILABLE;
        source.checks[28].confidence = 0.8;
        source.checks[28].reference_value = 0.01;
        source.checks[28].reference_valid = 1;
        source.checks[1].model_value = 2.1;
        source.checks[1].delta = 2.0;
        source.findings[1].score = 1.0;
        source.findings[1].severity = HWA_PHYSICAL_SEVERITY_CRITICAL;
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) != 0,
              "equal-score findings ignored check-ID order");
        CHECK(ftell(stream) == 0L,
              "finding-tie preflight emitted partial output");
        source.checks[1].model_value = 0.5;
        source.checks[1].delta = 0.4;
        source.findings[1].score = 0.2;
        source.findings[1].severity = HWA_PHYSICAL_SEVERITY_INFO;
        source.findings[1].score = nextafter(0.2, INFINITY);
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) != 0,
              "noncanonical one-ULP finding score passed writer");
        CHECK(ftell(stream) == 0L,
              "score preflight emitted partial output");
        source.findings[1].score = 0.2;
        old_kind = source.checks[0].kind;
        old_unit = source.checks[0].unit;
        source.checks[0].kind = HWA_PHYSICAL_ELEMENT_PITCH_ONLY_SCORE;
        source.checks[0].unit = HWA_PHYSICAL_UNIT_RATIO;
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) != 0,
              "incomplete physical check catalog passed writer");
        CHECK(ftell(stream) == 0L,
              "catalog preflight emitted partial output");
        source.checks[0].kind = old_kind;
        source.checks[0].unit = old_unit;
        old_scope = source.checks[1].scope;
        old_case = source.checks[1].case_id;
        old_element = source.checks[1].element;
        old_kind = source.checks[1].kind;
        old_unit = source.checks[1].unit;
        duplicate_scope = test_copy(source.checks[0].scope);
        duplicate_case = test_copy(source.checks[0].case_id);
        duplicate_element = test_copy(source.checks[0].element);
        CHECK(duplicate_scope != NULL && duplicate_case != NULL &&
                  duplicate_element != NULL,
              "duplicate-key fixture allocation failed");
        if (duplicate_scope != NULL && duplicate_case != NULL &&
            duplicate_element != NULL) {
            source.checks[1].scope = duplicate_scope;
            source.checks[1].case_id = duplicate_case;
            source.checks[1].element = duplicate_element;
            source.checks[1].kind = source.checks[0].kind;
            source.checks[1].unit = source.checks[0].unit;
            CHECK(hwa_physical_check_set_retained_bytes(
                      &source, &source.retained_work_bytes) == 0,
                  "duplicate-key retained-byte count failed");
            CHECK(hwa_physical_file_write(stream, &source,
                                          error, sizeof(error)) != 0,
                  "duplicate physical check key passed writer");
            CHECK(ftell(stream) == 0L,
                  "duplicate-key preflight emitted partial output");
            source.checks[1].scope = old_scope;
            source.checks[1].case_id = old_case;
            source.checks[1].element = old_element;
            source.checks[1].kind = old_kind;
            source.checks[1].unit = old_unit;
        }
        free(duplicate_scope);
        free(duplicate_case);
        free(duplicate_element);
        CHECK(hwa_physical_check_set_retained_bytes(
                  &source, &source.retained_work_bytes) == 0,
              "restored duplicate-key retained-byte count failed");
        old_kind = source.checks[2].kind;
        old_unit = source.checks[2].unit;
        source.checks[2].kind = source.checks[3].kind;
        source.checks[2].unit = source.checks[3].unit;
        source.checks[3].kind = old_kind;
        source.checks[3].unit = old_unit;
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) != 0,
              "reordered physical check rows passed writer");
        CHECK(ftell(stream) == 0L,
              "check-order preflight emitted partial output");
        old_kind = source.checks[2].kind;
        old_unit = source.checks[2].unit;
        source.checks[2].kind = source.checks[3].kind;
        source.checks[2].unit = source.checks[3].unit;
        source.checks[3].kind = old_kind;
        source.checks[3].unit = old_unit;
        saved_max_modes = source.options.max_modes;
        source.options.max_modes = 1U;
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) != 0,
              "saved body-mode cap was not enforced");
        CHECK(ftell(stream) == 0L,
              "mode-cap preflight emitted partial output");
        source.options.max_modes = saved_max_modes;
        source.checks[46].index = 0U;
        source.checks[47].index = 0U;
        source.checks[48].index = 0U;
        source.checks[49].index = 0U;
        source.checks[50].index = 0U;
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) != 0,
              "duplicate body-mode indices passed writer");
        CHECK(ftell(stream) == 0L,
              "mode-index preflight emitted partial output");
        source.checks[46].index = 1U;
        source.checks[47].index = 1U;
        source.checks[48].index = 1U;
        source.checks[49].index = 1U;
        source.checks[50].index = 1U;
        source.checks[41].reference_value = -1.0;
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) != 0,
              "negative body-mode value passed writer");
        CHECK(ftell(stream) == 0L,
              "value-domain preflight emitted partial output");
        source.checks[41].reference_value = 1.0;
        source.checks[41].model_value = 1.0;
        source.checks[41].model_valid = 1;
        source.checks[41].delta_valid = 1;
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) != 0,
              "body value on an unbound side passed writer");
        CHECK(ftell(stream) == 0L,
              "side-shape preflight emitted partial output");
        source.checks[41].model_value = 0.0;
        source.checks[41].model_valid = 0;
        source.checks[41].delta_valid = 0;
        source.checks[51].availability = HWA_PHYSICAL_AVAILABLE;
        source.checks[51].reference_value = 0.5;
        source.checks[51].reference_valid = 1;
        source.checks[51].confidence = 0.8;
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) != 0,
              "unsupported body-pan fact passed writer");
        CHECK(ftell(stream) == 0L,
              "unsupported-fact preflight emitted partial output");
        source.checks[51].availability = HWA_PHYSICAL_UNAVAILABLE;
        source.checks[51].reference_value = 0.0;
        source.checks[51].reference_valid = 0;
        source.checks[51].confidence = 0.0;
        source.checks[51].availability = HWA_PHYSICAL_INSUFFICIENT;
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) != 0,
              "body pan accepted insufficient instead of unavailable");
        CHECK(ftell(stream) == 0L,
              "body-pan availability emitted partial output");
        source.checks[51].availability = HWA_PHYSICAL_UNAVAILABLE;
        source.checks[59].availability = HWA_PHYSICAL_INSUFFICIENT;
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) != 0,
              "unbound beating fact accepted insufficient evidence");
        CHECK(ftell(stream) == 0L,
              "joint availability emitted partial output");
        source.checks[59].availability = HWA_PHYSICAL_UNAVAILABLE;
        source.checks[12].model_value = 2.0;
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) != 0,
              "subharmonic ratio above one passed writer");
        CHECK(ftell(stream) == 0L,
              "subharmonic-domain preflight emitted partial output");
        source.checks[12].model_value = 0.0;
        source.checks[13].model_value = -1.0;
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) != 0,
              "negative fixed-tone prominence passed writer");
        CHECK(ftell(stream) == 0L,
              "fixed-tone-domain preflight emitted partial output");
        source.checks[13].model_value = 0.0;
        source.retained_work_bytes--;
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) != 0,
              "wrong retained-work count passed writer");
        CHECK(ftell(stream) == 0L,
              "retained-work preflight emitted partial output");
        source.retained_work_bytes++;
        old_path = source.sources[2].path;
        large_path = (char *)malloc(large_size + 1U);
        CHECK(large_path != NULL, "large path allocation failed");
        if (large_path != NULL) {
            memset(large_path, 'x', large_size);
            large_path[large_size] = '\0';
            source.sources[2].path = large_path;
            CHECK(hwa_physical_check_set_retained_bytes(
                      &source, &source.retained_work_bytes) == 0,
                  "large path retained-byte count failed");
            CHECK(hwa_physical_file_write(stream, &source,
                                          error, sizeof(error)) != 0,
                  "oversized encoded path passed writer");
            CHECK(ftell(stream) == 0L,
                  "field-cap preflight emitted partial output");
            source.sources[2].path = old_path;
            free(large_path);
            CHECK(hwa_physical_check_set_retained_bytes(
                      &source, &source.retained_work_bytes) == 0,
                  "restored retained-byte count failed");
        }
        source.sources[2].format.block_align = 4U;
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) != 0,
              "inconsistent WAVE shape passed writer");
        CHECK(ftell(stream) == 0L,
              "format preflight emitted partial output");
        source.sources[2].format.block_align = 2U;
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) == 0,
              "restored preflight fixture is invalid: %s", error);
        (void)fclose(stream);
    }
    stream = fopen("/dev/null", "rb");
    if (stream != NULL) {
        CHECK(hwa_physical_file_write(stream, &source,
                                      error, sizeof(error)) != 0,
              "write fault was not reported");
        (void)fclose(stream);
    }
    hwa_physical_check_set_free(&source);
}

int main(void)
{
    test_score_extremes();
    test_round_trip();
    test_numeric_locale_round_trip();
    test_subnormal_round_trip();
    test_mode_stress_and_retained_work();
    test_hostile_and_caps();
    test_preflight_and_write_fault();
    if (failures != 0) {
        (void)fprintf(stderr, "%d Stage 5 persistence test(s) failed\n",
                      failures);
        return 1;
    }
    (void)puts("Stage 5 persistence tests passed");
    return 0;
}

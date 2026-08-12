#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "hlolli_wg_analyzer.h"
#include "measure_compare.h"
#include "measure_file.h"
#include "physical_check.h"
#include "physical_file.h"
#include "sha256.h"

#include <errno.h>
#include <float.h>
#include <locale.h>
#include <math.h>
#include <limits.h>
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

#define TEST_PI 3.14159265358979323846264338327950288

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
        int length = snprintf(path, PATH_MAX,
                              "%s/hwa-stage5-physical-%ld-%u",
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

static int test_set_comma_numeric_locale(void)
{
    static const char *const candidates[] = {
        "de_DE.UTF-8", "fr_FR.UTF-8", "de_DE",
        "German_Germany.1252", "de-DE"
    };
    size_t index;
    for (index = 0U; index < sizeof(candidates) / sizeof(candidates[0]);
         ++index) {
        if (setlocale(LC_NUMERIC, candidates[index]) != NULL &&
            strcmp(localeconv()->decimal_point, ",") == 0) return 1;
    }
    return 0;
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
    for (index = 0U; index + 1U < HWA_SHA256_HEX_SIZE; ++index) {
        target[index] = byte;
    }
    target[HWA_SHA256_HEX_SIZE - 1U] = '\0';
}

static int test_make_profile(HWAMeasurementSet *set,
                             double first,
                             double second)
{
    double values[2];
    size_t item;
    char error[HWA_ERROR_SIZE];

    memset(set, 0, sizeof(*set));
    hwa_measurement_options_default(&set->options);
    set->options.fft_size = 1024U;
    set->options.hop_size = 128U;
    set->options.max_partials = 4U;
    set->options.max_work_bytes = UINT64_C(67108864);
    set->items_path = test_copy(set, "items.hwa-items");
    set->audio_path = test_copy(set, "stored-audio-must-not-open.wav");
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
    set->item_frame_evaluations = 4U;
    set->transform_count = 2U;
    set->context_count = 2U;
    set->contexts = (HWAMeasureItemContext *)calloc(
        set->context_count, sizeof(*set->contexts));
    set->measurement_count = 4U;
    set->measurements = (HWAMeasureObservation *)calloc(
        set->measurement_count, sizeof(*set->measurements));
    if (set->contexts == NULL || set->measurements == NULL ||
        set->items_path == NULL || set->audio_path == NULL ||
        set->alignment_path == NULL || set->source_score_path == NULL) {
        hwa_measurement_set_free(set);
        return 0;
    }
    set->retained_work_bytes +=
        (uint64_t)set->context_count * sizeof(*set->contexts) +
        (uint64_t)set->measurement_count * sizeof(*set->measurements);
    values[0] = first;
    values[1] = second;
    for (item = 0U; item < set->context_count; ++item) {
        HWAMeasureItemContext *context = &set->contexts[item];
        HWAMeasureObservation *raw = &set->measurements[item * 2U];
        HWAMeasureObservation *relative = raw + 1;
        char key[64];
        (void)snprintf(key, sizeof(key), "body:%zu", item + 1U);
        context->item_id = (uint64_t)item + 1U;
        context->item_key = test_copy(set, key);
        context->item_kind = HWA_ITEM_BODY;
        context->item_role = test_copy(set, "body");
        context->start_sample = (uint64_t)item * 8000U;
        context->end_sample = context->start_sample + 7000U;
        context->source_event_count = 1U;
        context->item_confidence = 0.9;
        raw->id = (uint64_t)item * 2U + 1U;
        raw->item_id = context->item_id;
        raw->kind = HWA_MEASURE_RMS_DBFS;
        raw->unit = HWA_MEASURE_UNIT_DBFS;
        raw->view = HWA_MEASURE_VIEW_RAW;
        raw->status = HWA_MEASURE_STATUS_VALID;
        raw->value = values[item];
        raw->confidence = 0.9;
        relative->id = raw->id + 1U;
        relative->item_id = raw->item_id;
        relative->kind = raw->kind;
        relative->unit = HWA_MEASURE_UNIT_DB;
        relative->view = HWA_MEASURE_VIEW_LEVEL_RELATIVE;
        relative->status = HWA_MEASURE_STATUS_VALID;
        relative->value = values[item] - set->level_reference_dbfs;
        relative->confidence = 0.9;
        relative->evidence_flags = HWA_MEASURE_EVIDENCE_LEVEL_REFERENCE;
        if (context->item_key == NULL || context->item_role == NULL) {
            hwa_measurement_set_free(set);
            return 0;
        }
    }
    if (hwa_measure_build_profile(set, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "profile fixture failed: %s\n", error);
        hwa_measurement_set_free(set);
        return 0;
    }
    return 1;
}

static void test_set_observation(HWAMeasureObservation *observation,
                                 uint64_t id,
                                 uint64_t item_id,
                                 HWAMeasureKind kind,
                                 HWAMeasureUnit unit,
                                 HWAMeasureView view,
                                 double value)
{
    memset(observation, 0, sizeof(*observation));
    observation->id = id;
    observation->item_id = item_id;
    observation->kind = kind;
    observation->unit = unit;
    observation->view = view;
    observation->status = HWA_MEASURE_STATUS_VALID;
    observation->value = value;
    observation->confidence = 0.95;
}

static int test_make_element_profile(HWAMeasurementSet *set, int collapsed)
{
    static const HWAMeasureKind kinds[8] = {
        HWA_MEASURE_FLATNESS,
        HWA_MEASURE_FIXED_STATE_FRACTION,
        HWA_MEASURE_ODD_EVEN_BALANCE_DB,
        HWA_MEASURE_HARMONIC_CENTROID,
        HWA_MEASURE_HNR_DB,
        HWA_MEASURE_HARMONIC_DECAY_DB_PER_SECOND,
        HWA_MEASURE_RISE_90_SECONDS,
        HWA_MEASURE_DECAY_DB
    };
    static const HWAMeasureUnit units[8] = {
        HWA_MEASURE_UNIT_RATIO,
        HWA_MEASURE_UNIT_RATIO,
        HWA_MEASURE_UNIT_DB,
        HWA_MEASURE_UNIT_HARMONIC_INDEX,
        HWA_MEASURE_UNIT_DB,
        HWA_MEASURE_UNIT_DB_PER_SECOND,
        HWA_MEASURE_UNIT_SECONDS,
        HWA_MEASURE_UNIT_DB
    };
    static const double distinct[2][8] = {
        {0.10, 0.20, -6.0, 2.0, 15.0, -30.0, 0.05, -12.0},
        {0.30, 0.60, 6.0, 4.0, 8.0, -10.0, 0.15, -5.0}
    };
    static const double same[2][8] = {
        {0.20, 0.40, 0.0, 3.0, 12.0, -20.0, 0.10, -8.0},
        {0.20, 0.40, 0.0, 3.0, 12.0, -20.0, 0.10, -8.0}
    };
    const double (*traits)[8] = collapsed ? same : distinct;
    size_t item;
    char error[HWA_ERROR_SIZE];

    memset(set, 0, sizeof(*set));
    hwa_measurement_options_default(&set->options);
    set->options.fft_size = 1024U;
    set->options.hop_size = 128U;
    set->options.max_partials = 4U;
    set->options.max_work_bytes = UINT64_C(67108864);
    set->items_path = test_copy(set, "elements.hwa-items");
    set->audio_path = test_copy(set, "stored-elements.wav");
    set->alignment_path = test_copy(set, "elements.hwa-align");
    set->source_score_path = test_copy(set, "elements.csv");
    test_hash(set->items_sha256, '1');
    test_hash(set->audio_sha256, '2');
    test_hash(set->alignment_sha256, '3');
    test_hash(set->source_score_sha256, '4');
    set->audio_format.container = HWA_CONTAINER_RIFF;
    set->audio_format.encoding = HWA_ENCODING_PCM;
    set->audio_format.channels = 1U;
    set->audio_format.sample_rate_hz = 16000U;
    set->audio_format.bits_per_sample = 16U;
    set->audio_format.valid_bits_per_sample = 16U;
    set->audio_format.block_align = 2U;
    set->audio_format.frames = 32000U;
    set->audio_format.data_bytes = 64000U;
    set->audio_format.duration_seconds = 2.0;
    set->level_reference_dbfs = -15.0;
    set->level_reference_item_count = 4U;
    set->level_reference_valid = 1;
    set->item_frame_evaluations = 40U;
    set->transform_count = 16U;
    set->context_count = 4U;
    set->contexts = (HWAMeasureItemContext *)calloc(
        set->context_count, sizeof(*set->contexts));
    set->measurement_count = 40U;
    set->measurements = (HWAMeasureObservation *)calloc(
        set->measurement_count, sizeof(*set->measurements));
    if (set->contexts == NULL || set->measurements == NULL ||
        set->items_path == NULL || set->audio_path == NULL ||
        set->alignment_path == NULL || set->source_score_path == NULL) {
        hwa_measurement_set_free(set);
        return 0;
    }
    set->retained_work_bytes +=
        (uint64_t)set->context_count * sizeof(*set->contexts) +
        (uint64_t)set->measurement_count * sizeof(*set->measurements);
    for (item = 0U; item < set->context_count; ++item) {
        size_t element = item / 2U;
        HWAMeasureItemContext *context = &set->contexts[item];
        HWAMeasureObservation *observations =
            &set->measurements[item * 10U];
        double rms = element == 0U ? -20.0 : -10.0;
        char key[64];
        size_t trait;

        (void)snprintf(key, sizeof(key), "body:element:%zu", item + 1U);
        context->item_id = (uint64_t)item + 1U;
        context->item_key = test_copy(set, key);
        context->item_kind = HWA_ITEM_BODY;
        context->item_role = test_copy(set, "body");
        context->start_sample = (uint64_t)item * 8000U;
        context->end_sample = context->start_sample + 7000U;
        context->labels.physical_element = test_copy(
            set, element == 0U ? "element|a" : "element:b");
        context->source_event_count = 1U;
        context->item_confidence = 0.95;
        test_set_observation(&observations[0],
                             (uint64_t)item * 10U + 1U,
                             context->item_id, HWA_MEASURE_RMS_DBFS,
                             HWA_MEASURE_UNIT_DBFS,
                             HWA_MEASURE_VIEW_RAW, rms);
        test_set_observation(&observations[1],
                             (uint64_t)item * 10U + 2U,
                             context->item_id, HWA_MEASURE_RMS_DBFS,
                             HWA_MEASURE_UNIT_DB,
                             HWA_MEASURE_VIEW_LEVEL_RELATIVE, rms + 15.0);
        observations[1].evidence_flags =
            HWA_MEASURE_EVIDENCE_LEVEL_REFERENCE;
        for (trait = 0U; trait < 8U; ++trait) {
            test_set_observation(&observations[trait + 2U],
                                 (uint64_t)item * 10U + trait + 3U,
                                 context->item_id, kinds[trait], units[trait],
                                 HWA_MEASURE_VIEW_RAW,
                                 traits[element][trait] +
                                     (item % 2U == 0U ? -0.001 : 0.001));
        }
        if (context->item_key == NULL || context->item_role == NULL ||
            context->labels.physical_element == NULL) {
            hwa_measurement_set_free(set);
            return 0;
        }
    }
    if (hwa_measure_build_profile(set, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "element fixture failed: %s\n", error);
        hwa_measurement_set_free(set);
        return 0;
    }
    return 1;
}

static int test_make_many_element_profile(HWAMeasurementSet *set,
                                          const char *prefix,
                                          size_t element_count)
{
    size_t context_count;
    size_t item;
    char error[HWA_ERROR_SIZE];
    if (element_count == 0U || element_count > SIZE_MAX / 2U) return 0;
    context_count = element_count * 2U;
    memset(set, 0, sizeof(*set));
    hwa_measurement_options_default(&set->options);
    set->options.fft_size = 1024U;
    set->options.hop_size = 128U;
    set->options.max_partials = 4U;
    set->options.max_work_bytes = UINT64_C(67108864);
    set->items_path = test_copy(set, "many-elements.hwa-items");
    set->audio_path = test_copy(set, "stored-many-elements.wav");
    set->alignment_path = test_copy(set, "many-elements.hwa-align");
    set->source_score_path = test_copy(set, "many-elements.csv");
    test_hash(set->items_sha256, prefix[0] == 'r' ? '1' : '2');
    test_hash(set->audio_sha256, 'a');
    test_hash(set->alignment_sha256, 'b');
    test_hash(set->source_score_sha256, 'c');
    set->audio_format.container = HWA_CONTAINER_RIFF;
    set->audio_format.encoding = HWA_ENCODING_PCM;
    set->audio_format.channels = 1U;
    set->audio_format.sample_rate_hz = 16000U;
    set->audio_format.bits_per_sample = 16U;
    set->audio_format.valid_bits_per_sample = 16U;
    set->audio_format.block_align = 2U;
    set->audio_format.frames = 32000U;
    set->audio_format.data_bytes = 64000U;
    set->audio_format.duration_seconds = 2.0;
    set->level_reference_dbfs = -15.0;
    set->level_reference_item_count = context_count;
    set->level_reference_valid = 1;
    set->context_count = context_count;
    set->measurement_count = context_count * 2U;
    set->contexts = (HWAMeasureItemContext *)calloc(
        set->context_count, sizeof(*set->contexts));
    set->measurements = (HWAMeasureObservation *)calloc(
        set->measurement_count, sizeof(*set->measurements));
    if (set->contexts == NULL || set->measurements == NULL ||
        set->items_path == NULL || set->audio_path == NULL ||
        set->alignment_path == NULL || set->source_score_path == NULL) {
        hwa_measurement_set_free(set);
        return 0;
    }
    set->retained_work_bytes +=
        (uint64_t)set->context_count * sizeof(*set->contexts) +
        (uint64_t)set->measurement_count * sizeof(*set->measurements);
    for (item = 0U; item < context_count; ++item) {
        HWAMeasureItemContext *context = &set->contexts[item];
        HWAMeasureObservation *raw = &set->measurements[item * 2U];
        HWAMeasureObservation *relative = raw + 1U;
        double value = item % 2U == 0U ? -20.0 : -10.0;
        char key[64];
        char element[64];
        (void)snprintf(key, sizeof(key), "many:%s:%zu", prefix, item);
        (void)snprintf(element, sizeof(element), "%s%zu", prefix, item / 2U);
        context->item_id = (uint64_t)item + 1U;
        context->item_key = test_copy(set, key);
        context->item_kind = HWA_ITEM_BODY;
        context->item_role = test_copy(set, "body");
        context->labels.physical_element = test_copy(set, element);
        context->start_sample = (uint64_t)item * 1000U;
        context->end_sample = context->start_sample + 900U;
        context->source_event_count = 1U;
        context->item_confidence = 0.9;
        test_set_observation(raw, (uint64_t)item * 2U + 1U,
                             context->item_id, HWA_MEASURE_RMS_DBFS,
                             HWA_MEASURE_UNIT_DBFS,
                             HWA_MEASURE_VIEW_RAW, value);
        test_set_observation(relative, raw->id + 1U, context->item_id,
                             HWA_MEASURE_RMS_DBFS, HWA_MEASURE_UNIT_DB,
                             HWA_MEASURE_VIEW_LEVEL_RELATIVE, value + 15.0);
        relative->evidence_flags = HWA_MEASURE_EVIDENCE_LEVEL_REFERENCE;
        if (context->item_key == NULL || context->item_role == NULL ||
            context->labels.physical_element == NULL) {
            hwa_measurement_set_free(set);
            return 0;
        }
    }
    if (hwa_measure_build_profile(set, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "many-element fixture failed: %s\n", error);
        hwa_measurement_set_free(set);
        return 0;
    }
    return 1;
}

static int test_write_profile(const char *path,
                              const HWAMeasurementSet *set)
{
    FILE *stream = fopen(path, "wb");
    char error[HWA_ERROR_SIZE];
    int result;
    if (stream == NULL) return 0;
    result = hwa_measure_file_write(stream, set, error, sizeof(error));
    if (fclose(stream) != 0) result = -1;
    if (result != 0) {
        (void)fprintf(stderr, "profile write failed: %s\n", error);
    }
    return result == 0;
}

static int test_write_stream_bytes(FILE *stream,
                                   const void *bytes,
                                   size_t size)
{
    return fwrite(bytes, 1U, size, stream) == size;
}

static int test_write_u16(FILE *stream, uint16_t value)
{
    unsigned char bytes[2];
    bytes[0] = (unsigned char)(value & UINT16_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT16_C(0xff));
    return test_write_stream_bytes(stream, bytes, sizeof(bytes));
}

static int test_write_u32(FILE *stream, uint32_t value)
{
    unsigned char bytes[4];
    bytes[0] = (unsigned char)(value & UINT32_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT32_C(0xff));
    bytes[2] = (unsigned char)((value >> 16U) & UINT32_C(0xff));
    bytes[3] = (unsigned char)((value >> 24U) & UINT32_C(0xff));
    return test_write_stream_bytes(stream, bytes, sizeof(bytes));
}

static int test_write_u64(FILE *stream, uint64_t value)
{
    unsigned char bytes[8];
    unsigned shift;
    for (shift = 0U; shift < 8U; ++shift) {
        bytes[shift] = (unsigned char)(value >> (shift * 8U));
    }
    return test_write_stream_bytes(stream, bytes, sizeof(bytes));
}

static int test_write_short_float64_wav(const char *path,
                                        const double *samples,
                                        uint32_t frames)
{
    const uint32_t sample_rate = 8000U;
    const uint16_t block_align = 8U;
    uint32_t data_size;
    FILE *stream;
    uint32_t frame;
    int okay;
    if (path == NULL || samples == NULL || frames == 0U ||
        frames > UINT32_MAX / (uint32_t)block_align) return 0;
    data_size = frames * (uint32_t)block_align;
    stream = fopen(path, "wb");
    if (stream == NULL) return 0;
    okay = test_write_stream_bytes(stream, "RIFF", 4U) &&
           test_write_u32(stream, 36U + data_size) &&
           test_write_stream_bytes(stream, "WAVE", 4U) &&
           test_write_stream_bytes(stream, "fmt ", 4U) &&
           test_write_u32(stream, 16U) && test_write_u16(stream, 3U) &&
           test_write_u16(stream, 1U) &&
           test_write_u32(stream, sample_rate) &&
           test_write_u32(stream, sample_rate * (uint32_t)block_align) &&
           test_write_u16(stream, block_align) &&
           test_write_u16(stream, 64U) &&
           test_write_stream_bytes(stream, "data", 4U) &&
           test_write_u32(stream, data_size);
    for (frame = 0U; okay && frame < frames; ++frame) {
        uint64_t bits;
        memcpy(&bits, &samples[frame], sizeof(bits));
        okay = test_write_u64(stream, bits);
    }
    if (fclose(stream) != 0) okay = 0;
    return okay;
}

static double test_scan_sample(uint32_t frame, uint32_t sample_rate)
{
    double value;
    if (frame >= 7200U) return 0.0;
    if (frame >= 2048U && frame < 3072U) {
        uint32_t repeated = (frame - 2048U) % 32U;
        return 0.15 * sin(2.0 * TEST_PI * (double)repeated / 32.0);
    }
    value = 0.08 + 0.20 * sin(
        2.0 * TEST_PI * 440.0 * (double)frame / (double)sample_rate);
    if (frame >= 4096U && frame < 7000U) {
        value *= 1.0 + 2.0 * (double)(frame - 4096U) / 2904.0;
    }
    if (frame == 3500U) value = 1.0;
    return value;
}

static int test_write_scan_wav(const char *path)
{
    const uint32_t sample_rate = 8000U;
    const uint32_t frames = 8192U;
    const uint16_t channels = 1U;
    const uint16_t block_align = 2U;
    uint32_t data_size = frames * (uint32_t)block_align;
    FILE *stream = fopen(path, "wb");
    uint32_t frame;
    int okay;

    if (stream == NULL) return 0;
    okay = test_write_stream_bytes(stream, "RIFF", 4U) &&
           test_write_u32(stream, 36U + data_size) &&
           test_write_stream_bytes(stream, "WAVE", 4U) &&
           test_write_stream_bytes(stream, "fmt ", 4U) &&
           test_write_u32(stream, 16U) && test_write_u16(stream, 1U) &&
           test_write_u16(stream, channels) &&
           test_write_u32(stream, sample_rate) &&
           test_write_u32(stream, sample_rate * (uint32_t)block_align) &&
           test_write_u16(stream, block_align) &&
           test_write_u16(stream, 16U) &&
           test_write_stream_bytes(stream, "data", 4U) &&
           test_write_u32(stream, data_size);
    for (frame = 0U; okay && frame < frames; ++frame) {
        double value = test_scan_sample(frame, sample_rate);
        int16_t sample = value >= 1.0
                             ? INT16_MAX
                             : value <= -1.0
                                   ? INT16_MIN
                                   : (int16_t)lrint(value * 32767.0);
        okay = test_write_u16(stream, (uint16_t)sample);
    }
    if (fclose(stream) != 0) okay = 0;
    return okay;
}

typedef enum TestSignalKind {
    TEST_SIGNAL_BODY = 1,
    TEST_SIGNAL_ISOLATED_A = 2,
    TEST_SIGNAL_ISOLATED_B = 3,
    TEST_SIGNAL_JOINT = 4,
    TEST_SIGNAL_RENDER_BASELINE = 5,
    TEST_SIGNAL_RENDER_VARIANT = 6,
    TEST_SIGNAL_SILENT = 7,
    TEST_SIGNAL_QUIET = 8,
    TEST_SIGNAL_SUBSONIC = 9
} TestSignalKind;

static double test_signal_value(TestSignalKind kind,
                                uint32_t frame,
                                uint32_t sample_rate)
{
    double time = (double)frame / (double)sample_rate;
    double first = 0.22 * sin(2.0 * TEST_PI * 300.0 * time);
    double second = 0.18 * sin(2.0 * TEST_PI * 450.0 * time);
    switch (kind) {
    case TEST_SIGNAL_BODY:
        return exp(-2.0 * time) *
               (0.45 * sin(2.0 * TEST_PI * 440.0 * time) +
                0.22 * sin(2.0 * TEST_PI * 920.0 * time));
    case TEST_SIGNAL_ISOLATED_A: return first;
    case TEST_SIGNAL_ISOLATED_B: return second;
    case TEST_SIGNAL_JOINT:
        return first + second + 0.60 * first * second;
    case TEST_SIGNAL_RENDER_BASELINE:
        return frame < 7200U
            ? 0.30 * sin(2.0 * TEST_PI * 330.0 * time) : 0.0;
    case TEST_SIGNAL_RENDER_VARIANT:
        if (frame < 12U || frame >= 7212U) return 0.0;
        time = (double)(frame - 12U) / (double)sample_rate;
        return 0.27 * sin(2.0 * TEST_PI * 330.0 * time);
    case TEST_SIGNAL_SILENT: return 0.0;
    case TEST_SIGNAL_QUIET:
        return 0.0001 * sin(2.0 * TEST_PI * 330.0 * time);
    case TEST_SIGNAL_SUBSONIC:
        return 0.40 * sin(2.0 * TEST_PI * 5.0 * time) +
               0.02 * sin(2.0 * TEST_PI * 440.0 * time);
    default: return 0.0;
    }
}

static int test_write_signal_wav(const char *path, TestSignalKind kind)
{
    const uint32_t sample_rate = 8000U;
    const uint32_t frames = 8192U;
    const uint16_t block_align = 2U;
    uint32_t data_size = frames * (uint32_t)block_align;
    FILE *stream = fopen(path, "wb");
    uint32_t frame;
    int okay;
    if (stream == NULL) return 0;
    okay = test_write_stream_bytes(stream, "RIFF", 4U) &&
           test_write_u32(stream, 36U + data_size) &&
           test_write_stream_bytes(stream, "WAVE", 4U) &&
           test_write_stream_bytes(stream, "fmt ", 4U) &&
           test_write_u32(stream, 16U) && test_write_u16(stream, 1U) &&
           test_write_u16(stream, 1U) &&
           test_write_u32(stream, sample_rate) &&
           test_write_u32(stream, sample_rate * (uint32_t)block_align) &&
           test_write_u16(stream, block_align) &&
           test_write_u16(stream, 16U) &&
           test_write_stream_bytes(stream, "data", 4U) &&
           test_write_u32(stream, data_size);
    for (frame = 0U; okay && frame < frames; ++frame) {
        double value = test_signal_value(kind, frame, sample_rate);
        int16_t sample = (int16_t)lrint(
            fmax(-1.0, fmin(1.0, value)) * 32767.0);
        okay = test_write_u16(stream, (uint16_t)sample);
    }
    if (fclose(stream) != 0) okay = 0;
    return okay;
}

static int test_write_nonfinite_wav(const char *path)
{
    const uint32_t sample_rate = 8000U;
    const uint32_t frames = 256U;
    const uint16_t block_align = 4U;
    uint32_t data_size = frames * (uint32_t)block_align;
    FILE *stream = fopen(path, "wb");
    uint32_t frame;
    int okay;
    if (stream == NULL) return 0;
    okay = test_write_stream_bytes(stream, "RIFF", 4U) &&
           test_write_u32(stream, 36U + data_size) &&
           test_write_stream_bytes(stream, "WAVE", 4U) &&
           test_write_stream_bytes(stream, "fmt ", 4U) &&
           test_write_u32(stream, 16U) && test_write_u16(stream, 3U) &&
           test_write_u16(stream, 1U) &&
           test_write_u32(stream, sample_rate) &&
           test_write_u32(stream, sample_rate * (uint32_t)block_align) &&
           test_write_u16(stream, block_align) &&
           test_write_u16(stream, 32U) &&
           test_write_stream_bytes(stream, "data", 4U) &&
           test_write_u32(stream, data_size);
    for (frame = 0U; okay && frame < frames; ++frame) {
        okay = test_write_u32(
            stream, frame == 10U ? UINT32_C(0x7fc00000) : UINT32_C(0));
    }
    if (fclose(stream) != 0) okay = 0;
    return okay;
}

static int test_write_huge_finite_wav(const char *path)
{
    const uint32_t sample_rate = 8000U;
    const uint32_t frames = 256U;
    const uint16_t block_align = 8U;
    uint32_t data_size = frames * (uint32_t)block_align;
    FILE *stream = fopen(path, "wb");
    uint32_t frame;
    int okay;
    if (stream == NULL) return 0;
    okay = test_write_stream_bytes(stream, "RIFF", 4U) &&
           test_write_u32(stream, 36U + data_size) &&
           test_write_stream_bytes(stream, "WAVE", 4U) &&
           test_write_stream_bytes(stream, "fmt ", 4U) &&
           test_write_u32(stream, 16U) && test_write_u16(stream, 3U) &&
           test_write_u16(stream, 1U) &&
           test_write_u32(stream, sample_rate) &&
           test_write_u32(stream, sample_rate * (uint32_t)block_align) &&
           test_write_u16(stream, block_align) &&
           test_write_u16(stream, 64U) &&
           test_write_stream_bytes(stream, "data", 4U) &&
           test_write_u32(stream, data_size);
    for (frame = 0U; okay && frame < frames; ++frame) {
        okay = test_write_u64(
            stream, frame == 10U ? UINT64_C(0x7fefffffffffffff) : 0U);
    }
    if (fclose(stream) != 0) okay = 0;
    return okay;
}

static void test_defaults_and_empty_free(void)
{
    HWAPhysicalOptions options;
    HWAPhysicalCheckSet set;

    memset(&options, 0, sizeof(options));
    hwa_physical_options_default(&options);
    CHECK(options.decode_block_frames > 0U,
          "default decode block is empty");
    CHECK(options.fft_size >= 256U,
          "default FFT is too small");
    CHECK(options.hop_size > 0U && options.hop_size <= options.fft_size,
          "default hop is outside the FFT");
    CHECK(isfinite(options.spectral_floor_dbfs) &&
              options.spectral_floor_dbfs < 0.0,
          "default spectral floor is invalid");
    CHECK(options.max_wave_bytes > 0U && options.max_wave_frames > 0U &&
              options.max_work_bytes > 0U &&
              options.max_pair_evaluations > 0U,
          "default hard limits are empty");
    CHECK(options.max_bindings > 0U && options.max_transforms > 0U &&
              options.max_modes > 0U && options.max_checks > 0U &&
              options.max_findings > 0U && options.max_warnings > 0U,
          "default count limits are empty");
    CHECK(options.profile_limits.max_input_bytes > 0U &&
              options.profile_limits.max_work_bytes > 0U,
          "default profile limits are empty");

    memset(&set, 0, sizeof(set));
    hwa_physical_check_set_free(&set);
    CHECK(set.sources == NULL && set.checks == NULL &&
              set.findings == NULL && set.warnings == NULL,
          "free changed an empty result");
    set.source_count = 9U;
    set.check_count = 8U;
    set.finding_count = 7U;
    set.warning_count = 6U;
    hwa_physical_check_set_free(&set);
    CHECK(set.source_count == 0U && set.check_count == 0U &&
              set.finding_count == 0U && set.warning_count == 0U,
          "free did not tolerate parsed counts without arrays");
}

static void test_saved_names_and_role_grammar(void)
{
    HWAPhysicalRole role;
    HWAPhysicalCheckKind kind = HWA_PHYSICAL_CHECK_KIND_COUNT;
    HWAPhysicalUnit unit = HWA_PHYSICAL_UNIT_COUNT;
    HWAPhysicalAvailability availability =
        HWA_PHYSICAL_AVAILABILITY_COUNT;
    HWAPhysicalFindingClass finding_class =
        HWA_PHYSICAL_FINDING_CLASS_COUNT;
    HWAPhysicalSeverity severity = HWA_PHYSICAL_SEVERITY_COUNT;

    CHECK(strcmp(hwa_physical_check_kind_name(
                     HWA_PHYSICAL_JOINT_RESIDUAL_DB),
                 "joint_residual_db") == 0,
          "check kind saved name changed");
    CHECK(hwa_physical_check_kind_from_name("joint_residual_db", &kind) == 0 &&
              kind == HWA_PHYSICAL_JOINT_RESIDUAL_DB,
          "check kind saved name did not parse");
    CHECK(hwa_physical_unit_from_name("dB/s", &unit) == 0 &&
              unit == HWA_PHYSICAL_UNIT_DB_PER_SECOND,
          "physical unit did not parse");
    CHECK(hwa_physical_availability_from_name("insufficient", &availability) ==
                  0 &&
              availability == HWA_PHYSICAL_INSUFFICIENT,
          "availability did not parse");
    CHECK(hwa_physical_finding_class_from_name("fault", &finding_class) == 0 &&
              finding_class == HWA_PHYSICAL_FINDING_FAULT,
          "finding class did not parse");
    CHECK(hwa_physical_severity_from_name("critical", &severity) == 0 &&
              severity == HWA_PHYSICAL_SEVERITY_CRITICAL,
          "severity did not parse");
    CHECK(hwa_physical_check_kind_from_name("JOINT_RESIDUAL_DB", &kind) != 0,
          "saved names became case-insensitive");
    CHECK(hwa_physical_check_kind_name(HWA_PHYSICAL_CHECK_KIND_COUNT) == NULL,
          "count sentinel has a saved name");

    CHECK(hwa_physical_role_parse("reference:body:resonator-a", &role) == 0,
          "valid body role did not parse");
    CHECK(role.side == HWA_PHYSICAL_ROLE_REFERENCE &&
              role.kind == HWA_PHYSICAL_ROLE_BODY &&
              role.case_id_length == 11U &&
              strncmp(role.case_id, "resonator-a", role.case_id_length) == 0,
          "valid body role parsed to the wrong fields");
    CHECK(hwa_physical_role_parse("model:isolated-a:dual-tone-1", &role) == 0 &&
              role.side == HWA_PHYSICAL_ROLE_MODEL &&
              role.kind == HWA_PHYSICAL_ROLE_ISOLATED_A,
          "valid isolated role did not parse");
    CHECK(hwa_physical_role_parse("reference:body:", &role) != 0 &&
              hwa_physical_role_parse("reference:body:a:b", &role) != 0 &&
              hwa_physical_role_parse("other:body:a", &role) != 0 &&
              hwa_physical_role_parse("model:other:a", &role) != 0,
          "invalid role grammar was accepted");
}

static const HWAPhysicalCheck *test_find_check(
    const HWAPhysicalCheckSet *set,
    HWAPhysicalCheckKind kind)
{
    size_t index;
    for (index = 0U; index < set->check_count; ++index) {
        if (set->checks[index].kind == kind) return &set->checks[index];
    }
    return NULL;
}

static const HWAPhysicalCheck *test_find_scope_check(
    const HWAPhysicalCheckSet *set,
    const char *scope,
    HWAPhysicalCheckKind kind)
{
    size_t index;
    for (index = 0U; index < set->check_count; ++index) {
        if (set->checks[index].kind == kind &&
            strcmp(set->checks[index].scope, scope) == 0) {
            return &set->checks[index];
        }
    }
    return NULL;
}

static const HWAPhysicalCheck *test_find_case_check(
    const HWAPhysicalCheckSet *set,
    const char *scope,
    const char *case_id,
    HWAPhysicalCheckKind kind)
{
    size_t index;
    for (index = 0U; index < set->check_count; ++index) {
        if (set->checks[index].kind == kind &&
            strcmp(set->checks[index].scope, scope) == 0 &&
            strcmp(set->checks[index].case_id, case_id) == 0) {
            return &set->checks[index];
        }
    }
    return NULL;
}

static int test_check_has_scored_finding(const HWAPhysicalCheckSet *set,
                                         const HWAPhysicalCheck *check)
{
    size_t index;
    if (check == NULL) return 0;
    for (index = 0U; index < set->finding_count; ++index) {
        if (set->findings[index].check_id_valid &&
            set->findings[index].score_valid &&
            set->findings[index].check_id == check->id) return 1;
    }
    return 0;
}

static void test_physical_options(HWAPhysicalOptions *options)
{
    hwa_physical_options_default(options);
    options->fft_size = 1024U;
    options->hop_size = 128U;
    options->profile_limits.max_input_bytes = UINT64_C(67108864);
    options->profile_limits.max_work_bytes = UINT64_C(67108864);
    options->profile_limits.max_contexts = 100U;
    options->profile_limits.max_measurements = 1000U;
    options->profile_limits.max_groups = 1000U;
    options->profile_limits.max_group_members = 10000U;
    options->profile_limits.max_statistics = 10000U;
    options->profile_limits.max_warnings = 100U;
    options->profile_limits.max_distributions = 10000U;
    options->profile_limits.max_gaps = 10000U;
}

static uint64_t test_physical_retained_bytes(
    const HWAPhysicalCheckSet *set)
{
    uint64_t bytes = (uint64_t)strlen(set->reference_measures_path) + 1U +
                     (uint64_t)strlen(set->model_measures_path) + 1U;
    size_t index;
    bytes += (uint64_t)set->source_count * sizeof(*set->sources);
    bytes += (uint64_t)set->check_count * sizeof(*set->checks);
    bytes += (uint64_t)set->finding_count * sizeof(*set->findings);
    bytes += (uint64_t)set->warning_count * sizeof(*set->warnings);
    for (index = 0U; index < set->source_count; ++index) {
        bytes += (uint64_t)strlen(set->sources[index].role) + 1U;
        bytes += (uint64_t)strlen(set->sources[index].path) + 1U;
    }
    for (index = 0U; index < set->check_count; ++index) {
        bytes += (uint64_t)strlen(set->checks[index].scope) + 1U;
        bytes += (uint64_t)strlen(set->checks[index].case_id) + 1U;
        bytes += (uint64_t)strlen(set->checks[index].element) + 1U;
    }
    for (index = 0U; index < set->finding_count; ++index) {
        bytes += (uint64_t)strlen(set->findings[index].code) + 1U;
        bytes += (uint64_t)strlen(set->findings[index].message) + 1U;
    }
    for (index = 0U; index < set->warning_count; ++index) {
        bytes += (uint64_t)strlen(set->warnings[index].code) + 1U;
        bytes += (uint64_t)strlen(set->warnings[index].message) + 1U;
    }
    return bytes;
}

static void test_profile_only_provenance_and_unavailable(void)
{
    HWAMeasurementSet reference;
    HWAMeasurementSet model;
    HWAPhysicalOptions options;
    HWAPhysicalCheckSet result;
    const HWAPhysicalCheck *check;
    char directory[PATH_MAX];
    char reference_path[PATH_MAX];
    char model_path[PATH_MAX];
    char error[HWA_ERROR_SIZE];
    int call_result;

    CHECK(test_workspace(directory), "cannot make test workspace");
    if (failures != 0 ||
        !test_path(reference_path, directory, "reference.hwa-measures") ||
        !test_path(model_path, directory, "model.hwa-measures")) return;
    CHECK(test_make_profile(&reference, -20.0, -10.0),
          "cannot build reference profile");
    CHECK(test_make_profile(&model, -18.0, -8.0),
          "cannot build model profile");
    CHECK(test_write_profile(reference_path, &reference) &&
              test_write_profile(model_path, &model),
          "cannot write profile fixtures");
    hwa_physical_options_default(&options);
    options.profile_limits.max_input_bytes = UINT64_C(67108864);
    options.profile_limits.max_work_bytes = UINT64_C(67108864);
    options.profile_limits.max_contexts = 100U;
    options.profile_limits.max_measurements = 100U;
    options.profile_limits.max_groups = 100U;
    options.profile_limits.max_group_members = 1000U;
    options.profile_limits.max_statistics = 1000U;
    options.profile_limits.max_warnings = 100U;
    options.profile_limits.max_distributions = 1000U;
    options.profile_limits.max_gaps = 1000U;
    memset(&result, 0xa5, sizeof(result));
    call_result = hwa_check_physical_files(
        reference_path, model_path, NULL, 0U, &options,
        &result, error, sizeof(error));
    CHECK(call_result == 0, "profile-only check failed: %s", error);
    if (call_result == 0) {
        CHECK(hwa_physical_check_set_validate(
                  &result, error, sizeof(error)) == 0,
              "profile-only result fails persistence validation: %s", error);
        CHECK(result.source_count == 2U,
              "profile-only result has %zu sources", result.source_count);
        CHECK(result.sources[0].id == 1U &&
                  strcmp(result.sources[0].role, "reference:profile") == 0 &&
                  result.sources[0].is_wave == 0,
              "reference profile source is wrong");
        CHECK(result.sources[1].id == 2U &&
                  strcmp(result.sources[1].role, "model:profile") == 0 &&
                  result.sources[1].is_wave == 0,
              "model profile source is wrong");
        CHECK(strcmp(result.reference_measures_path, reference_path) == 0 &&
                  strcmp(result.model_measures_path, model_path) == 0,
              "profile paths were not retained");
        CHECK(strlen(result.reference_measures_sha256) == 64U &&
                  strlen(result.model_measures_sha256) == 64U,
              "profile hashes were not retained");
        check = test_find_check(&result,
                                HWA_PHYSICAL_ELEMENT_TRAIT_DELTA);
        CHECK(check != NULL &&
                  check->availability == HWA_PHYSICAL_INSUFFICIENT &&
                  !check->reference_valid && !check->model_valid &&
                  !check->delta_valid,
              "missing element labels did not stay insufficient");
        check = test_find_check(&result,
                                HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ);
        CHECK(check != NULL &&
                  check->availability == HWA_PHYSICAL_UNAVAILABLE,
              "missing body probes did not stay unavailable");
        check = test_find_check(&result, HWA_PHYSICAL_JOINT_RESIDUAL_DB);
        CHECK(check != NULL &&
                  check->availability == HWA_PHYSICAL_UNAVAILABLE,
              "missing joint probes did not stay unavailable");
        check = test_find_check(&result,
                                HWA_PHYSICAL_RENDER_RMS_ERROR_DB);
        CHECK(check != NULL &&
                  check->availability == HWA_PHYSICAL_UNAVAILABLE,
              "missing render probes did not stay unavailable");
        hwa_physical_check_set_free(&result);
    }
    hwa_measurement_set_free(&model);
    hwa_measurement_set_free(&reference);
    CHECK(test_remove_file(model_path) == 0 &&
              test_remove_file(reference_path) == 0 &&
              test_remove_directory(directory) == 0,
          "cannot clean profile-only workspace");
}

static void test_public_call_uses_c_numeric_locale(void)
{
    HWAMeasurementSet reference;
    HWAMeasurementSet model;
    HWAPhysicalOptions options;
    HWAPhysicalCheckSet result;
    const char *current = setlocale(LC_NUMERIC, NULL);
    char saved[128];
    char directory[PATH_MAX];
    char reference_path[PATH_MAX];
    char model_path[PATH_MAX];
    char error[HWA_ERROR_SIZE];
    int call_result;

    if (current == NULL || strlen(current) >= sizeof(saved)) return;
    memcpy(saved, current, strlen(current) + 1U);
    CHECK(test_workspace(directory), "cannot make locale workspace");
    if (failures != 0 ||
        !test_path(reference_path, directory, "reference.hwa-measures") ||
        !test_path(model_path, directory, "model.hwa-measures")) return;
    CHECK(test_make_profile(&reference, -20.0, -10.0) &&
              test_make_profile(&model, -18.0, -8.0) &&
              test_write_profile(reference_path, &reference) &&
              test_write_profile(model_path, &model),
          "cannot build locale profiles");
    if (!test_set_comma_numeric_locale()) {
        CHECK(setlocale(LC_NUMERIC, saved) != NULL,
              "cannot restore numeric locale after skip");
    } else {
        test_physical_options(&options);
        memset(&result, 0xa5, sizeof(result));
        call_result = hwa_check_physical_files(
            reference_path, model_path, NULL, 0U, &options,
            &result, error, sizeof(error));
        CHECK(call_result == 0,
              "comma-locale physical check failed: %s", error);
        CHECK(strcmp(localeconv()->decimal_point, ",") == 0,
              "physical check changed the caller's numeric locale");
        if (call_result == 0) hwa_physical_check_set_free(&result);
        test_physical_options(&result.options);
        call_result = hwa_check_physical_files(
            reference_path, model_path, NULL, 0U, &result.options,
            &result, error, sizeof(error));
        CHECK(call_result == 0,
              "result-options alias physical check failed: %s", error);
        if (call_result == 0) hwa_physical_check_set_free(&result);
        CHECK(setlocale(LC_NUMERIC, saved) != NULL,
              "cannot restore numeric locale after Stage 5 call");
    }
    hwa_measurement_set_free(&model);
    hwa_measurement_set_free(&reference);
    CHECK(test_remove_file(model_path) == 0 &&
              test_remove_file(reference_path) == 0 &&
              test_remove_directory(directory) == 0,
          "cannot clean locale workspace");
}

static void test_scan_binding_and_block_invariance(void)
{
    HWAMeasurementSet reference;
    HWAMeasurementSet model;
    HWAPhysicalInput input;
    HWAPhysicalInput invalid;
    HWAPhysicalOptions first_options;
    HWAPhysicalOptions second_options;
    HWAPhysicalCheckSet first;
    HWAPhysicalCheckSet second;
    HWAPhysicalCheckSet rejected;
    const HWAPhysicalCheck *first_check;
    const HWAPhysicalCheck *second_check;
    char directory[PATH_MAX];
    char reference_path[PATH_MAX];
    char model_path[PATH_MAX];
    char wave_path[PATH_MAX];
    char error[HWA_ERROR_SIZE];
    char expected_wave_hash[HWA_SHA256_HEX_SIZE];
    size_t kind;
    int first_result;
    int second_result;
    int hash_result;

    CHECK(test_workspace(directory), "cannot make scan workspace");
    if (failures != 0 ||
        !test_path(reference_path, directory, "reference.hwa-measures") ||
        !test_path(model_path, directory, "model.hwa-measures") ||
        !test_path(wave_path, directory, "fault-scan.wav")) return;
    CHECK(test_make_profile(&reference, -20.0, -10.0) &&
              test_make_profile(&model, -18.0, -8.0) &&
              test_write_profile(reference_path, &reference) &&
              test_write_profile(model_path, &model) &&
              test_write_scan_wav(wave_path),
          "cannot build scan fixtures");
    hwa_physical_options_default(&first_options);
    first_options.decode_block_frames = 17U;
    first_options.fft_size = 1024U;
    first_options.hop_size = 128U;
    first_options.profile_limits.max_input_bytes = UINT64_C(67108864);
    first_options.profile_limits.max_work_bytes = UINT64_C(67108864);
    first_options.profile_limits.max_contexts = 100U;
    first_options.profile_limits.max_measurements = 100U;
    first_options.profile_limits.max_groups = 100U;
    first_options.profile_limits.max_group_members = 1000U;
    first_options.profile_limits.max_statistics = 1000U;
    first_options.profile_limits.max_warnings = 100U;
    first_options.profile_limits.max_distributions = 1000U;
    first_options.profile_limits.max_gaps = 1000U;
    second_options = first_options;
    second_options.decode_block_frames = 257U;
    hash_result = hwa_sha256_file(
        wave_path, first_options.max_wave_bytes, expected_wave_hash,
        error, sizeof(error));
    CHECK(hash_result == 0,
          "cannot hash scan fixture: %s", error);
    input.role = "model:scan:fault";
    input.path = wave_path;
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    first_result = hwa_check_physical_files(
        reference_path, model_path, &input, 1U, &first_options,
        &first, error, sizeof(error));
    CHECK(first_result == 0, "first scan failed: %s", error);
    second_result = hwa_check_physical_files(
        reference_path, model_path, &input, 1U, &second_options,
        &second, error, sizeof(error));
    CHECK(second_result == 0, "second scan failed: %s", error);
    if (first_result == 0 && second_result == 0) {
        CHECK(first.source_count == 3U && first.sources[2].is_wave == 1 &&
                  strcmp(first.sources[2].role, input.role) == 0 &&
                  strcmp(first.sources[2].path, input.path) == 0 &&
                  first.sources[2].format.frames == 8192U,
              "scan source provenance is wrong");
        if (hash_result == 0) {
            CHECK(strcmp(first.sources[2].sha256, expected_wave_hash) == 0 &&
                      strcmp(second.sources[2].sha256,
                             expected_wave_hash) == 0,
                  "scan source hash is not bound to the WAVE bytes");
        }
        for (kind = (size_t)HWA_PHYSICAL_DC_OFFSET;
             kind <= (size_t)HWA_PHYSICAL_DENORMAL_FRACTION; ++kind) {
            first_check = test_find_scope_check(
                &first, input.role, (HWAPhysicalCheckKind)kind);
            second_check = test_find_scope_check(
                &second, input.role, (HWAPhysicalCheckKind)kind);
            CHECK(first_check != NULL && second_check != NULL,
                  "scan omitted kind %zu", kind);
            if (first_check != NULL && second_check != NULL &&
                first_check->availability == HWA_PHYSICAL_AVAILABLE &&
                second_check->availability == HWA_PHYSICAL_AVAILABLE) {
                CHECK(first_check->model_valid && second_check->model_valid &&
                          fabs(first_check->model_value -
                               second_check->model_value) <= 1.0e-12,
                      "scan kind %zu depends on decode blocks", kind);
            }
        }
        first_check = test_find_scope_check(
            &first, input.role, HWA_PHYSICAL_DC_OFFSET);
        CHECK(first_check != NULL &&
                  first_check->availability == HWA_PHYSICAL_AVAILABLE &&
                  first_check->model_valid &&
                  first_check->model_value > 0.01,
              "known DC offset was not measured");
        first_check = test_find_scope_check(
            &first, input.role, HWA_PHYSICAL_CLIP_FRACTION);
        CHECK(first_check != NULL && first_check->model_value > 0.0,
              "known clipped sample was not measured");
        first_check = test_find_scope_check(
            &first, input.role, HWA_PHYSICAL_REPEATED_BLOCK_FRACTION);
        CHECK(first_check != NULL && first_check->model_value > 0.05,
              "known repeated blocks were not measured");
    }

    invalid.role = "model:scan:bad:case";
    invalid.path = wave_path;
    memset(&rejected, 0xa5, sizeof(rejected));
    CHECK(hwa_check_physical_files(
              reference_path, model_path, &invalid, 1U, &first_options,
              &rejected, error, sizeof(error)) != 0 &&
              rejected.sources == NULL && rejected.checks == NULL,
          "invalid role did not fail with an empty result");

    hwa_physical_check_set_free(&second);
    hwa_physical_check_set_free(&first);
    hwa_measurement_set_free(&model);
    hwa_measurement_set_free(&reference);
    CHECK(test_remove_file(wave_path) == 0 &&
              test_remove_file(model_path) == 0 &&
              test_remove_file(reference_path) == 0 &&
              test_remove_directory(directory) == 0,
          "cannot clean scan workspace");
}

static void test_element_identity_uses_shape_not_level(void)
{
    HWAMeasurementSet reference;
    HWAMeasurementSet model;
    HWAPhysicalOptions options;
    HWAPhysicalCheckSet result;
    const HWAPhysicalCheck *reference_distance;
    const HWAPhysicalCheck *model_distance;
    const HWAPhysicalCheck *gain_only;
    const HWAPhysicalCheck *pitch_only;
    char directory[PATH_MAX];
    char reference_path[PATH_MAX];
    char model_path[PATH_MAX];
    char error[HWA_ERROR_SIZE];
    int call_result;

    CHECK(test_workspace(directory), "cannot make element workspace");
    if (failures != 0 ||
        !test_path(reference_path, directory, "reference.hwa-measures") ||
        !test_path(model_path, directory, "model.hwa-measures")) return;
    CHECK(test_make_element_profile(&reference, 0) &&
              test_make_element_profile(&model, 1) &&
              test_write_profile(reference_path, &reference) &&
              test_write_profile(model_path, &model),
          "cannot build element profile fixtures");
    hwa_physical_options_default(&options);
    options.profile_limits.max_input_bytes = UINT64_C(67108864);
    options.profile_limits.max_work_bytes = UINT64_C(67108864);
    options.profile_limits.max_contexts = 100U;
    options.profile_limits.max_measurements = 1000U;
    options.profile_limits.max_groups = 1000U;
    options.profile_limits.max_group_members = 10000U;
    options.profile_limits.max_statistics = 10000U;
    options.profile_limits.max_warnings = 100U;
    options.profile_limits.max_distributions = 10000U;
    options.profile_limits.max_gaps = 10000U;
    memset(&result, 0, sizeof(result));
    call_result = hwa_check_physical_files(
        reference_path, model_path, NULL, 0U, &options,
        &result, error, sizeof(error));
    CHECK(call_result == 0, "element check failed: %s", error);
    if (call_result == 0) {
        reference_distance = test_find_check(
            &result, HWA_PHYSICAL_ELEMENT_REFERENCE_DISTANCE);
        model_distance = test_find_check(
            &result, HWA_PHYSICAL_ELEMENT_MODEL_DISTANCE);
        gain_only = test_find_check(
            &result, HWA_PHYSICAL_ELEMENT_GAIN_ONLY_SCORE);
        pitch_only = test_find_check(
            &result, HWA_PHYSICAL_ELEMENT_PITCH_ONLY_SCORE);
        CHECK(reference_distance != NULL &&
                  reference_distance->availability == HWA_PHYSICAL_AVAILABLE &&
                  reference_distance->reference_valid,
              "reference element distance is unavailable");
        CHECK(model_distance != NULL &&
                  model_distance->availability == HWA_PHYSICAL_AVAILABLE &&
                  model_distance->model_valid,
              "model element distance is unavailable");
        if (reference_distance != NULL && model_distance != NULL) {
            CHECK(reference_distance->reference_value >
                      model_distance->model_value + 0.5,
                  "shape-collapsed model did not reduce element distance");
            CHECK(strcmp(reference_distance->element,
                         "9:element:b|9:element|a") == 0 &&
                      strcmp(model_distance->element,
                             "9:element:b|9:element|a") == 0,
                  "element pair key is not length-prefixed");
        }
        CHECK(gain_only != NULL && gain_only->model_valid &&
                  gain_only->model_value > 0.8,
              "gain-only element collapse was not flagged");
        CHECK(pitch_only != NULL &&
                  pitch_only->availability == HWA_PHYSICAL_UNAVAILABLE &&
                  (pitch_only->quality_flags &
                   HWA_PHYSICAL_QUALITY_PITCH_CONFOUNDED) != 0U,
              "pitch-only check ignored profile pitch confounding");
        CHECK(hwa_physical_check_set_validate(
                  &result, error, sizeof(error)) == 0,
              "multi-element result is not canonical: %s", error);
        {
            size_t check_index;
            for (check_index = 1U; check_index < result.check_count;
                 ++check_index) {
                CHECK(hwa_physical_check_canonical_compare(
                          &result.checks[check_index - 1U],
                          &result.checks[check_index]) < 0,
                      "multi-element checks are not in canonical order");
            }
        }
        hwa_physical_check_set_free(&result);
    }
    hwa_measurement_set_free(&model);
    hwa_measurement_set_free(&reference);
    CHECK(test_remove_file(model_path) == 0 &&
              test_remove_file(reference_path) == 0 &&
              test_remove_directory(directory) == 0,
          "cannot clean element workspace");
}

static void test_one_sided_body_joint_and_render(void)
{
    static const char *wave_names[9] = {
        "body.wav", "isolated-a.wav", "isolated-b.wav", "joint.wav",
        "render-baseline.wav", "render-variant.wav", "silent.wav",
        "quiet.wav", "subsonic.wav"
    };
    static const char *roles[9] = {
        "model:body:resonator",
        "model:isolated-a:dual-tone",
        "model:isolated-b:dual-tone",
        "model:joint:dual-tone",
        "model:render-baseline:render",
        "model:render-variant:render",
        "model:scan:silence",
        "model:scan:quiet",
        "model:scan:subsonic"
    };
    static const TestSignalKind signal_kinds[9] = {
        TEST_SIGNAL_BODY, TEST_SIGNAL_ISOLATED_A, TEST_SIGNAL_ISOLATED_B,
        TEST_SIGNAL_JOINT, TEST_SIGNAL_RENDER_BASELINE,
        TEST_SIGNAL_RENDER_VARIANT, TEST_SIGNAL_SILENT, TEST_SIGNAL_QUIET,
        TEST_SIGNAL_SUBSONIC
    };
    HWAMeasurementSet reference;
    HWAMeasurementSet model;
    HWAPhysicalOptions options;
    HWAPhysicalCheckSet result;
    HWAPhysicalCheckSet reordered;
    HWAPhysicalInput inputs[9];
    HWAPhysicalInput reversed[9];
    char paths[9][PATH_MAX];
    char directory[PATH_MAX];
    char reference_path[PATH_MAX];
    char model_path[PATH_MAX];
    char error[HWA_ERROR_SIZE];
    const HWAPhysicalCheck *check;
    size_t index;
    size_t expected_rank = 0U;
    int call_result;

    CHECK(test_workspace(directory), "cannot make relation workspace");
    if (failures != 0 ||
        !test_path(reference_path, directory, "reference.hwa-measures") ||
        !test_path(model_path, directory, "model.hwa-measures")) return;
    CHECK(test_make_profile(&reference, -20.0, -10.0) &&
              test_make_profile(&model, -18.0, -8.0) &&
              test_write_profile(reference_path, &reference) &&
              test_write_profile(model_path, &model),
          "cannot build relation profiles");
    for (index = 0U; index < 9U; ++index) {
        CHECK(test_path(paths[index], directory, wave_names[index]) &&
                  test_write_signal_wav(paths[index], signal_kinds[index]),
              "cannot write relation WAVE %zu", index);
        inputs[index].role = roles[index];
        inputs[index].path = paths[index];
        reversed[8U - index] = inputs[index];
    }
    test_physical_options(&options);
    options.spectral_floor_dbfs = -60.0;
    memset(&result, 0, sizeof(result));
    call_result = hwa_check_physical_files(
        reference_path, model_path, inputs, 9U, &options,
        &result, error, sizeof(error));
    CHECK(call_result == 0, "relation check failed: %s", error);
    if (call_result == 0) {
        int reordered_result;
        memset(&reordered, 0, sizeof(reordered));
        reordered_result = hwa_check_physical_files(
            reference_path, model_path, reversed, 9U, &options,
            &reordered, error, sizeof(error));
        CHECK(reordered_result == 0,
              "reordered relation check failed: %s", error);
        if (reordered_result == 0) {
            CHECK(reordered.source_count == result.source_count &&
                      reordered.check_count == result.check_count &&
                      reordered.finding_count == result.finding_count,
                  "binding order changed result counts");
            if (reordered.source_count == result.source_count) {
                for (index = 0U; index < result.source_count; ++index) {
                    CHECK(strcmp(result.sources[index].role,
                                 reordered.sources[index].role) == 0 &&
                              strcmp(result.sources[index].sha256,
                                     reordered.sources[index].sha256) == 0,
                          "binding order changed source %zu", index);
                }
            }
            if (reordered.check_count == result.check_count) {
                for (index = 0U; index < result.check_count; ++index) {
                    const HWAPhysicalCheck *first = &result.checks[index];
                    const HWAPhysicalCheck *second = &reordered.checks[index];
                    CHECK(first->kind == second->kind &&
                              first->index == second->index &&
                              strcmp(first->scope, second->scope) == 0 &&
                              strcmp(first->case_id, second->case_id) == 0 &&
                              first->availability == second->availability &&
                              first->reference_valid == second->reference_valid &&
                              first->model_valid == second->model_valid &&
                              first->delta_valid == second->delta_valid &&
                              fabs(first->reference_value -
                                   second->reference_value) <= 1.0e-12 &&
                              fabs(first->model_value -
                                   second->model_value) <= 1.0e-12 &&
                              fabs(first->delta - second->delta) <= 1.0e-12,
                          "binding order changed check %zu", index);
                }
            }
            hwa_physical_check_set_free(&reordered);
        }
        check = test_find_case_check(
            &result, "body", "resonator",
            HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ);
        CHECK(check != NULL && check->availability == HWA_PHYSICAL_AVAILABLE &&
                  check->model_valid && !check->reference_valid &&
                  check->model_value > 400.0 && check->model_value < 1000.0,
              "one-sided body modes were not retained");
        check = test_find_case_check(
            &result, "body", "resonator", HWA_PHYSICAL_BODY_MODE_PAN);
        CHECK(check != NULL &&
                  check->availability == HWA_PHYSICAL_UNAVAILABLE &&
                  !check->reference_valid && !check->model_valid,
              "mono body pan did not stay unavailable");
        check = test_find_case_check(
            &result, "body", "resonator",
            HWA_PHYSICAL_BODY_MODE_DENSITY_PER_KHZ);
        CHECK(check != NULL && check->model_valid &&
                  check->model_value > 0.0,
              "one-sided body density was not retained");
        check = test_find_case_check(
            &result, "joint", "dual-tone",
            HWA_PHYSICAL_JOINT_RESIDUAL_DB);
        CHECK(check != NULL && check->availability == HWA_PHYSICAL_AVAILABLE &&
                  check->model_valid && !check->reference_valid &&
                  check->model_value > -40.0 &&
                  (check->quality_flags & HWA_PHYSICAL_QUALITY_FALLBACK) ==
                      0U &&
                  (check->evidence_flags & HWA_PHYSICAL_EVIDENCE_SPECTRUM) ==
                      0U,
              "known one-sided nonlinear residual was not measured");
        check = test_find_case_check(
            &result, "joint", "dual-tone",
            HWA_PHYSICAL_INTERMODULATION_RATIO);
        CHECK(check != NULL && check->model_valid &&
                  check->model_value > 0.0 &&
                  (check->quality_flags & HWA_PHYSICAL_QUALITY_FALLBACK) !=
                      0U &&
                  (check->evidence_flags & HWA_PHYSICAL_EVIDENCE_SPECTRUM) !=
                      0U,
              "known intermodulation was not measured");
        check = test_find_case_check(
            &result, "joint", "dual-tone",
            HWA_PHYSICAL_BEATING_DEPTH_RATIO);
        CHECK(check != NULL &&
                  check->availability == HWA_PHYSICAL_INSUFFICIENT &&
                  !check->model_valid &&
                  (check->quality_flags &
                   HWA_PHYSICAL_QUALITY_PITCH_CONFOUNDED) != 0U &&
                  (check->evidence_flags & HWA_PHYSICAL_EVIDENCE_ENVELOPE) ==
                      0U,
              "untracked beating depth was reported as measured");
        check = test_find_case_check(
            &result, "joint", "dual-tone",
            HWA_PHYSICAL_BEATING_RATE_HZ);
        CHECK(check != NULL &&
                  check->availability == HWA_PHYSICAL_INSUFFICIENT &&
                  !check->model_valid &&
                  (check->quality_flags &
                   HWA_PHYSICAL_QUALITY_PITCH_CONFOUNDED) != 0U,
              "untracked beating rate was reported as measured");
        check = test_find_case_check(
            &result, "render", "render", HWA_PHYSICAL_RENDER_LAG_SAMPLES);
        CHECK(check != NULL && check->availability == HWA_PHYSICAL_AVAILABLE &&
                  check->model_valid && !check->reference_valid &&
                  fabs(check->model_value - 12.0) <= 1.0 &&
                  (check->quality_flags & HWA_PHYSICAL_QUALITY_FALLBACK) ==
                      0U &&
                  (check->evidence_flags & HWA_PHYSICAL_EVIDENCE_SPECTRUM) ==
                      0U,
              "known render lag was not measured");
        check = test_find_case_check(
            &result, "render", "render",
            HWA_PHYSICAL_RENDER_CORRELATION);
        CHECK(check != NULL && check->model_valid &&
                  check->model_value > 0.99,
              "known render correlation was not measured");
        check = test_find_scope_check(
            &result, "model:render-baseline:render",
            HWA_PHYSICAL_REPEATED_BLOCK_FRACTION);
        CHECK(check != NULL && check->model_valid &&
                  check->model_value < 0.01,
              "silent tail was counted as repeated active blocks");
        check = test_find_scope_check(
            &result, "model:render-baseline:render",
            HWA_PHYSICAL_STUCK_STATE_FRACTION);
        CHECK(check != NULL && check->model_valid &&
                  check->model_value < 0.01,
              "silent tail was counted as a stuck active state");
        check = test_find_scope_check(
            &result, "model:scan:silence",
            HWA_PHYSICAL_REPEATED_BLOCK_FRACTION);
        CHECK(check != NULL && check->model_valid &&
                  check->model_value == 0.0,
              "digital silence was counted as repeated active blocks");
        check = test_find_scope_check(
            &result, "model:scan:silence",
            HWA_PHYSICAL_STUCK_STATE_FRACTION);
        CHECK(check != NULL && check->model_valid &&
                  check->model_value == 0.0,
              "digital silence was counted as a stuck active state");
        check = test_find_scope_check(
            &result, "model:scan:quiet", HWA_PHYSICAL_HIGH_BAND_RATIO);
        CHECK(check != NULL &&
                  check->availability == HWA_PHYSICAL_INSUFFICIENT &&
                  !check->model_valid &&
                  (check->quality_flags & HWA_PHYSICAL_QUALITY_LOW_SIGNAL) !=
                      0U,
              "below-floor spectrum was treated as usable evidence");
        check = test_find_scope_check(
            &result, "model:scan:subsonic",
            HWA_PHYSICAL_SUBHARMONIC_RATIO);
        CHECK(check != NULL && check->availability == HWA_PHYSICAL_AVAILABLE &&
                  check->model_valid && check->model_value >= 0.0 &&
                  check->model_value <= 1.0,
              "subsonic-heavy scan produced an invalid harmonic ratio");
        check = test_find_scope_check(
            &result, "model:render-baseline:render",
            HWA_PHYSICAL_FIXED_TONE_PROMINENCE_DB);
        CHECK(check != NULL && !test_check_has_scored_finding(&result, check),
              "an untargeted steady tone produced a numeric fault");
        check = test_find_scope_check(
            &result, "model:joint:dual-tone",
            HWA_PHYSICAL_SUBHARMONIC_RATIO);
        CHECK(check != NULL && !test_check_has_scored_finding(&result, check),
              "an untargeted harmonic ratio produced a numeric fault");
        CHECK(result.retained_work_bytes ==
                  test_physical_retained_bytes(&result),
              "retained byte ledger is not exact");
        CHECK(hwa_physical_check_set_validate(
                  &result, error, sizeof(error)) == 0,
              "public result fails persistence validation: %s", error);
        for (index = 0U; index < result.check_count; ++index) {
            const HWAPhysicalCheck *item = &result.checks[index];
            CHECK(item->id == (uint64_t)index + 1U &&
                      item->scope != NULL && item->case_id != NULL &&
                      item->element != NULL,
                  "check identity/text invariant failed at %zu", index);
            if (item->availability != HWA_PHYSICAL_AVAILABLE) {
                CHECK(!item->reference_valid && !item->model_valid &&
                          !item->delta_valid && item->reference_value == 0.0 &&
                          item->model_value == 0.0 && item->delta == 0.0,
                      "unavailable check retained numeric data at %zu", index);
            }
        }
        for (index = 0U; index < result.finding_count; ++index) {
            const HWAPhysicalFinding *finding = &result.findings[index];
            CHECK(finding->id == (uint64_t)index + 1U &&
                      finding->code != NULL && finding->message != NULL,
                  "finding identity/text invariant failed at %zu", index);
            if (finding->score_valid) {
                expected_rank++;
                CHECK(finding->rank == expected_rank &&
                          isfinite(finding->score) && finding->score >= 0.0 &&
                          finding->score <= 1.0,
                      "ranked finding invariant failed at %zu", index);
            } else {
                CHECK(finding->rank == 0U && finding->score == 0.0,
                      "unranked finding stored a rank or score");
            }
        }
        CHECK(expected_rank > 0U, "known faults produced no ranked findings");
        hwa_physical_check_set_free(&result);
    }
    hwa_measurement_set_free(&model);
    hwa_measurement_set_free(&reference);
    for (index = 0U; index < 9U; ++index) {
        CHECK(test_remove_file(paths[index]) == 0,
              "cannot remove relation WAVE %zu", index);
    }
    CHECK(test_remove_file(model_path) == 0 &&
              test_remove_file(reference_path) == 0 &&
              test_remove_directory(directory) == 0,
          "cannot clean relation workspace");
}

static void test_rejects_nonfinite_and_special_files(void)
{
    HWAMeasurementSet reference;
    HWAMeasurementSet model;
    HWAPhysicalOptions options;
    HWAPhysicalCheckSet result;
    HWAPhysicalInput input;
    char directory[PATH_MAX];
    char reference_path[PATH_MAX];
    char model_path[PATH_MAX];
    char identical_path[PATH_MAX];
    char nonfinite_path[PATH_MAX];
    char huge_path[PATH_MAX];
    char error[HWA_ERROR_SIZE];

    CHECK(test_workspace(directory), "cannot make rejection workspace");
    if (failures != 0 ||
        !test_path(reference_path, directory, "reference.hwa-measures") ||
        !test_path(model_path, directory, "model.hwa-measures") ||
        !test_path(identical_path, directory, "identical.hwa-measures") ||
        !test_path(nonfinite_path, directory, "nonfinite.wav") ||
        !test_path(huge_path, directory, "huge.wav")) return;
    CHECK(test_make_profile(&reference, -20.0, -10.0) &&
              test_make_profile(&model, -18.0, -8.0) &&
              test_write_profile(reference_path, &reference) &&
              test_write_profile(model_path, &model) &&
              test_write_profile(identical_path, &reference) &&
              test_write_nonfinite_wav(nonfinite_path) &&
              test_write_huge_finite_wav(huge_path),
          "cannot build rejection fixtures");
    test_physical_options(&options);
    memset(&result, 0xa5, sizeof(result));
    CHECK(hwa_check_physical_files(
              reference_path, identical_path, NULL, 0U, &options,
              &result, error, sizeof(error)) != 0 &&
              result.sources == NULL && result.checks == NULL,
          "byte-identical profiles were accepted");

    input.role = "model:scan:nonfinite";
    input.path = nonfinite_path;
    memset(&result, 0xa5, sizeof(result));
    CHECK(hwa_check_physical_files(
              reference_path, model_path, &input, 1U, &options,
              &result, error, sizeof(error)) != 0 &&
              result.sources == NULL && result.checks == NULL,
          "non-finite WAVE input was accepted");
    input.role = "model:scan:huge";
    input.path = huge_path;
    memset(&result, 0xa5, sizeof(result));
    CHECK(hwa_check_physical_files(
              reference_path, model_path, &input, 1U, &options,
              &result, error, sizeof(error)) != 0 &&
              result.sources == NULL && result.checks == NULL,
          "huge finite WAVE input was accepted");
#if !defined(_WIN32)
    {
        char symlink_path[PATH_MAX];
        char fifo_path[PATH_MAX];
        CHECK(test_path(symlink_path, directory, "linked.wav") &&
                  test_path(fifo_path, directory, "pipe.wav") &&
                  symlink(nonfinite_path, symlink_path) == 0 &&
                  mkfifo(fifo_path, 0600) == 0,
              "cannot build special-file fixtures");
        input.role = "model:scan:linked";
        input.path = symlink_path;
        memset(&result, 0xa5, sizeof(result));
        CHECK(hwa_check_physical_files(
                  reference_path, model_path, &input, 1U, &options,
                  &result, error, sizeof(error)) != 0 &&
                  result.sources == NULL,
              "symbolic-link binding was accepted");
        input.role = "model:scan:pipe";
        input.path = fifo_path;
        memset(&result, 0xa5, sizeof(result));
        CHECK(hwa_check_physical_files(
                  reference_path, model_path, &input, 1U, &options,
                  &result, error, sizeof(error)) != 0 &&
                  result.sources == NULL,
              "FIFO binding was accepted");
        CHECK(test_remove_file(fifo_path) == 0 &&
                  test_remove_file(symlink_path) == 0,
              "cannot remove special-file fixtures");
    }
#endif
    hwa_measurement_set_free(&model);
    hwa_measurement_set_free(&reference);
    CHECK(test_remove_file(huge_path) == 0 &&
              test_remove_file(nonfinite_path) == 0 &&
              test_remove_file(identical_path) == 0 &&
              test_remove_file(model_path) == 0 &&
              test_remove_file(reference_path) == 0 &&
              test_remove_directory(directory) == 0,
          "cannot clean rejection workspace");
}

static void test_rejects_joint_and_render_overflow(void)
{
    static const char *names[5] = {
        "isolated-a.wav", "isolated-b.wav", "joint.wav",
        "render-baseline.wav", "render-variant.wav"
    };
    HWAMeasurementSet reference;
    HWAMeasurementSet model;
    HWAPhysicalOptions options;
    HWAPhysicalCheckSet result;
    HWAPhysicalInput joint_inputs[3];
    HWAPhysicalInput render_inputs[2];
    char directory[PATH_MAX];
    char reference_path[PATH_MAX];
    char model_path[PATH_MAX];
    char paths[5][PATH_MAX];
    char error[HWA_ERROR_SIZE];
    double positive[1];
    double negative[1];
    size_t index;

    positive[0] = 0.75 * sqrt(DBL_MAX);
    negative[0] = -positive[0];
    CHECK(isfinite(positive[0]) &&
              positive[0] * positive[0] < DBL_MAX,
          "cannot build finite overflow samples");
    CHECK(test_workspace(directory), "cannot make overflow workspace");
    if (failures != 0 ||
        !test_path(reference_path, directory, "reference.hwa-measures") ||
        !test_path(model_path, directory, "model.hwa-measures")) return;
    CHECK(test_make_profile(&reference, -20.0, -10.0) &&
              test_make_profile(&model, -18.0, -8.0) &&
              test_write_profile(reference_path, &reference) &&
              test_write_profile(model_path, &model),
          "cannot build overflow profiles");
    for (index = 0U; index < 5U; ++index) {
        const double *samples = index == 4U ? negative : positive;
        CHECK(test_path(paths[index], directory, names[index]) &&
                  test_write_short_float64_wav(paths[index], samples, 1U),
              "cannot write overflow WAVE %zu", index);
    }
    test_physical_options(&options);
    joint_inputs[0].role = "model:isolated-a:overflow";
    joint_inputs[0].path = paths[0];
    joint_inputs[1].role = "model:isolated-b:overflow";
    joint_inputs[1].path = paths[1];
    joint_inputs[2].role = "model:joint:overflow";
    joint_inputs[2].path = paths[2];
    memset(&result, 0xa5, sizeof(result));
    CHECK(hwa_check_physical_files(
              reference_path, model_path, joint_inputs, 3U, &options,
              &result, error, sizeof(error)) != 0 &&
              result.sources == NULL && result.checks == NULL &&
              result.findings == NULL && strstr(error, "joint") != NULL &&
              strstr(error, "overflow") != NULL,
          "joint overflow did not fail clearly: %s", error);

    render_inputs[0].role = "model:render-baseline:overflow";
    render_inputs[0].path = paths[3];
    render_inputs[1].role = "model:render-variant:overflow";
    render_inputs[1].path = paths[4];
    memset(&result, 0xa5, sizeof(result));
    CHECK(hwa_check_physical_files(
              reference_path, model_path, render_inputs, 2U, &options,
              &result, error, sizeof(error)) != 0 &&
              result.sources == NULL && result.checks == NULL &&
              result.findings == NULL && strstr(error, "render") != NULL &&
              strstr(error, "overflow") != NULL,
          "render overflow did not fail clearly: %s", error);

    hwa_measurement_set_free(&model);
    hwa_measurement_set_free(&reference);
    for (index = 0U; index < 5U; ++index) {
        CHECK(test_remove_file(paths[index]) == 0,
              "cannot remove overflow WAVE %zu", index);
    }
    CHECK(test_remove_file(model_path) == 0 &&
              test_remove_file(reference_path) == 0 &&
              test_remove_directory(directory) == 0,
          "cannot clean overflow workspace");
}

static void test_profile_lookup_evaluation_cap(void)
{
    HWAMeasurementSet reference;
    HWAMeasurementSet model;
    HWAPhysicalOptions options;
    HWAPhysicalCheckSet result;
    char directory[PATH_MAX];
    char reference_path[PATH_MAX];
    char model_path[PATH_MAX];
    char error[HWA_ERROR_SIZE];

    CHECK(test_workspace(directory), "cannot make profile-cap workspace");
    if (failures != 0 ||
        !test_path(reference_path, directory, "reference.hwa-measures") ||
        !test_path(model_path, directory, "model.hwa-measures")) return;
    CHECK(test_make_many_element_profile(&reference, "reference-", 8U) &&
              test_make_many_element_profile(&model, "model-", 8U) &&
              test_write_profile(reference_path, &reference) &&
              test_write_profile(model_path, &model),
          "cannot build profile-cap fixtures");
    test_physical_options(&options);
    options.max_pair_evaluations = 8U;
    memset(&result, 0xa5, sizeof(result));
    CHECK(hwa_check_physical_files(
              reference_path, model_path, NULL, 0U, &options,
              &result, error, sizeof(error)) != 0 &&
              result.sources == NULL && result.checks == NULL &&
              result.findings == NULL,
          "profile group lookups bypassed the evaluation cap");
    hwa_measurement_set_free(&model);
    hwa_measurement_set_free(&reference);
    CHECK(test_remove_file(model_path) == 0 &&
              test_remove_file(reference_path) == 0 &&
              test_remove_directory(directory) == 0,
          "cannot clean profile-cap workspace");
}

static int test_call_with_work_limit(const char *reference_path,
                                     const char *model_path,
                                     const HWAPhysicalInput *input,
                                     HWAPhysicalOptions *options,
                                     uint64_t limit)
{
    HWAPhysicalCheckSet result;
    char error[HWA_ERROR_SIZE];
    int status;
    options->max_work_bytes = limit;
    memset(&result, 0xa5, sizeof(result));
    status = hwa_check_physical_files(
        reference_path, model_path, input, 1U, options,
        &result, error, sizeof(error));
    if (status == 0) hwa_physical_check_set_free(&result);
    return status == 0;
}

static void test_exact_work_cap_boundary(void)
{
    HWAMeasurementSet reference;
    HWAMeasurementSet model;
    HWAPhysicalOptions options;
    HWAPhysicalInput input;
    HWAPhysicalCheckSet exact;
    HWAPhysicalCheckSet rejected;
    char directory[PATH_MAX];
    char reference_path[PATH_MAX];
    char model_path[PATH_MAX];
    char wave_path[PATH_MAX];
    char error[HWA_ERROR_SIZE];
    uint64_t lower = 1U;
    uint64_t upper = UINT64_C(16777216);
    uint64_t exact_limit;
    int exact_status;

    CHECK(test_workspace(directory), "cannot make cap workspace");
    if (failures != 0 ||
        !test_path(reference_path, directory, "reference.hwa-measures") ||
        !test_path(model_path, directory, "model.hwa-measures") ||
        !test_path(wave_path, directory, "scan.wav")) return;
    CHECK(test_make_profile(&reference, -20.0, -10.0) &&
              test_make_profile(&model, -18.0, -8.0) &&
              test_write_profile(reference_path, &reference) &&
              test_write_profile(model_path, &model) &&
              test_write_scan_wav(wave_path),
          "cannot build cap fixtures");
    test_physical_options(&options);
    input.role = "model:scan:cap";
    input.path = wave_path;
    CHECK(test_call_with_work_limit(
              reference_path, model_path, &input, &options, upper),
          "cap search upper bound is too small");
    while (lower < upper) {
        uint64_t middle = lower + (upper - lower) / 2U;
        if (test_call_with_work_limit(
                reference_path, model_path, &input, &options, middle)) {
            upper = middle;
        } else {
            lower = middle + 1U;
        }
    }
    exact_limit = lower;
    options.max_work_bytes = exact_limit;
    memset(&exact, 0, sizeof(exact));
    exact_status = hwa_check_physical_files(
        reference_path, model_path, &input, 1U, &options,
        &exact, error, sizeof(error));
    CHECK(exact_status == 0,
          "exact work cap failed at %llu bytes: %s",
          (unsigned long long)exact_limit, error);
    if (exact_status == 0) hwa_physical_check_set_free(&exact);
    CHECK(exact_limit > 1U, "exact cap search returned an invalid bound");
    if (exact_limit > 1U) {
        options.max_work_bytes = exact_limit - 1U;
        memset(&rejected, 0xa5, sizeof(rejected));
        CHECK(hwa_check_physical_files(
                  reference_path, model_path, &input, 1U, &options,
                  &rejected, error, sizeof(error)) != 0 &&
                  rejected.sources == NULL && rejected.checks == NULL &&
                  rejected.findings == NULL,
              "one byte below the exact work cap did not fail cleanly");
    }
    hwa_measurement_set_free(&model);
    hwa_measurement_set_free(&reference);
    CHECK(test_remove_file(wave_path) == 0 &&
              test_remove_file(model_path) == 0 &&
              test_remove_file(reference_path) == 0 &&
              test_remove_directory(directory) == 0,
          "cannot clean cap workspace");
}

int main(void)
{
    test_defaults_and_empty_free();
    test_saved_names_and_role_grammar();
    test_profile_only_provenance_and_unavailable();
    test_public_call_uses_c_numeric_locale();
    test_scan_binding_and_block_invariance();
    test_element_identity_uses_shape_not_level();
    test_one_sided_body_joint_and_render();
    test_rejects_nonfinite_and_special_files();
    test_rejects_joint_and_render_overflow();
    test_profile_lookup_evaluation_cap();
    test_exact_work_cap_boundary();
    if (failures != 0) {
        (void)fprintf(stderr, "%d physical-check test(s) failed\n", failures);
        return 1;
    }
    (void)puts("physical-check tests passed");
    return 0;
}

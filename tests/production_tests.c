#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "hlolli_wg_analyzer.h"
#include "measure_compare.h"
#include "measure_file.h"
#include "production.h"
#include "production_file.h"
#include "sha256.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#define TEST_MKDIR(path) _mkdir(path)
#define TEST_RMDIR(path) _rmdir(path)
#define TEST_UNLINK(path) _unlink(path)
#define TEST_PID() ((long)_getpid())
#else
#include <sys/stat.h>
#include <unistd.h>
#define TEST_MKDIR(path) mkdir(path, 0700)
#define TEST_RMDIR(path) rmdir(path)
#define TEST_UNLINK(path) unlink(path)
#define TEST_PID() ((long)getpid())
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TEST_RATE 8000U
#define TEST_FRAMES 8000U
#define TEST_PI 3.14159265358979323846264338327950288

typedef struct TestFiles {
    char directory[PATH_MAX];
    char reference_profile[PATH_MAX];
    char reference_audio[PATH_MAX];
    char model_profile[PATH_MAX];
    char model_audio[PATH_MAX];
    char room_ir[PATH_MAX];
} TestFiles;

typedef double (*TestSampleFunction)(uint32_t frame,
                                     uint16_t channel,
                                     void *context);

static int failures;

#define CHECK(condition, ...)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "FAIL: ");                                \
            (void)fprintf(stderr, __VA_ARGS__);                              \
            (void)fputc('\n', stderr);                                       \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static int test_join(char path[PATH_MAX],
                     const char *directory,
                     const char *name)
{
    int written = snprintf(path, PATH_MAX, "%s/%s", directory, name);
    return written > 0 && written < PATH_MAX;
}

static int test_files_open(TestFiles *files)
{
    unsigned attempt;
#if defined(_WIN32)
    const char *root = getenv("TEMP");
    if (root == NULL || root[0] == '\0') root = ".";
#else
    const char *root = "/tmp";
#endif
    memset(files, 0, sizeof(*files));
    for (attempt = 0U; attempt < 100U; ++attempt) {
        int written = snprintf(
            files->directory, sizeof(files->directory),
            "%s/hwa-production-%ld-%u", root, TEST_PID(), attempt);
        if (written < 0 ||
            (size_t)written >= sizeof(files->directory)) return 0;
        if (TEST_MKDIR(files->directory) == 0) break;
        if (errno != EEXIST) return 0;
    }
    return attempt < 100U &&
        test_join(files->reference_profile, files->directory,
                  "reference.hwa-measures") &&
        test_join(files->reference_audio, files->directory,
                  "reference.wav") &&
        test_join(files->model_profile, files->directory,
                  "model.hwa-measures") &&
        test_join(files->model_audio, files->directory, "model.wav") &&
        test_join(files->room_ir, files->directory, "room-ir.wav");
}

static void test_files_close(const TestFiles *files)
{
    (void)TEST_UNLINK(files->reference_profile);
    (void)TEST_UNLINK(files->reference_audio);
    (void)TEST_UNLINK(files->model_profile);
    (void)TEST_UNLINK(files->model_audio);
    (void)TEST_UNLINK(files->room_ir);
    (void)TEST_RMDIR(files->directory);
}

static int test_write_bytes(FILE *stream, const void *bytes, size_t count)
{
    return fwrite(bytes, 1U, count, stream) == count;
}

static int test_write_u16(FILE *stream, uint16_t value)
{
    unsigned char bytes[2];
    bytes[0] = (unsigned char)(value & UINT16_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT16_C(0xff));
    return test_write_bytes(stream, bytes, sizeof(bytes));
}

static int test_write_u32(FILE *stream, uint32_t value)
{
    unsigned char bytes[4];
    bytes[0] = (unsigned char)(value & UINT32_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT32_C(0xff));
    bytes[2] = (unsigned char)((value >> 16U) & UINT32_C(0xff));
    bytes[3] = (unsigned char)((value >> 24U) & UINT32_C(0xff));
    return test_write_bytes(stream, bytes, sizeof(bytes));
}

static int test_write_u64(FILE *stream, uint64_t value)
{
    unsigned char bytes[8];
    unsigned shift;
    for (shift = 0U; shift < 8U; ++shift) {
        bytes[shift] = (unsigned char)(value >> (shift * 8U));
    }
    return test_write_bytes(stream, bytes, sizeof(bytes));
}

static int test_write_pcm16_wave(const char *path,
                                 uint32_t sample_rate,
                                 uint16_t channels,
                                 uint32_t frames,
                                 TestSampleFunction sample_function,
                                 void *context)
{
    FILE *stream = fopen(path, "wb");
    uint64_t block_align = (uint64_t)channels * 2U;
    uint64_t data_bytes = (uint64_t)frames * block_align;
    uint64_t byte_rate = (uint64_t)sample_rate * block_align;
    uint32_t frame;
    int okay;
    if (stream == NULL || channels == 0U || sample_function == NULL ||
        data_bytes > UINT32_MAX || byte_rate > UINT32_MAX ||
        data_bytes > UINT32_MAX - 36U) {
        if (stream != NULL) (void)fclose(stream);
        return 0;
    }
    okay = test_write_bytes(stream, "RIFF", 4U) &&
        test_write_u32(stream, 36U + (uint32_t)data_bytes) &&
        test_write_bytes(stream, "WAVE", 4U) &&
        test_write_bytes(stream, "fmt ", 4U) &&
        test_write_u32(stream, 16U) && test_write_u16(stream, 1U) &&
        test_write_u16(stream, channels) &&
        test_write_u32(stream, sample_rate) &&
        test_write_u32(stream, (uint32_t)byte_rate) &&
        test_write_u16(stream, (uint16_t)block_align) &&
        test_write_u16(stream, 16U) &&
        test_write_bytes(stream, "data", 4U) &&
        test_write_u32(stream, (uint32_t)data_bytes);
    for (frame = 0U; okay && frame < frames; ++frame) {
        uint16_t channel;
        for (channel = 0U; okay && channel < channels; ++channel) {
            double sample = sample_function(frame, channel, context);
            if (sample > 1.0) sample = 1.0;
            if (sample < -1.0) sample = -1.0;
            okay = test_write_u16(
                stream, (uint16_t)(int16_t)lrint(sample * 32767.0));
        }
    }
    if (fclose(stream) != 0) okay = 0;
    return okay;
}

static int test_write_huge_float64_wave(const char *path,
                                        uint32_t sample_rate,
                                        uint32_t frames)
{
    FILE *stream = fopen(path, "wb");
    uint64_t data_bytes = (uint64_t)frames * 8U;
    uint32_t frame;
    int okay;
    if (stream == NULL || data_bytes > UINT32_MAX ||
        data_bytes > UINT32_MAX - 36U || sample_rate > UINT32_MAX / 8U) {
        if (stream != NULL) (void)fclose(stream);
        return 0;
    }
    okay = test_write_bytes(stream, "RIFF", 4U) &&
        test_write_u32(stream, 36U + (uint32_t)data_bytes) &&
        test_write_bytes(stream, "WAVE", 4U) &&
        test_write_bytes(stream, "fmt ", 4U) &&
        test_write_u32(stream, 16U) && test_write_u16(stream, 3U) &&
        test_write_u16(stream, 1U) && test_write_u32(stream, sample_rate) &&
        test_write_u32(stream, sample_rate * 8U) &&
        test_write_u16(stream, 8U) && test_write_u16(stream, 64U) &&
        test_write_bytes(stream, "data", 4U) &&
        test_write_u32(stream, (uint32_t)data_bytes);
    for (frame = 0U; okay && frame < frames; ++frame) {
        okay = test_write_u64(stream, UINT64_C(0x7fd0000000000000));
    }
    if (fclose(stream) != 0) okay = 0;
    return okay;
}

static int test_write_wave(const char *path, double amplitude)
{
    FILE *stream = fopen(path, "wb");
    uint32_t frame;
    int okay;
    if (stream == NULL) return 0;
    okay = test_write_bytes(stream, "RIFF", 4U) &&
        test_write_u32(stream, 36U + TEST_FRAMES * 2U) &&
        test_write_bytes(stream, "WAVE", 4U) &&
        test_write_bytes(stream, "fmt ", 4U) &&
        test_write_u32(stream, 16U) && test_write_u16(stream, 1U) &&
        test_write_u16(stream, 1U) && test_write_u32(stream, TEST_RATE) &&
        test_write_u32(stream, TEST_RATE * 2U) &&
        test_write_u16(stream, 2U) && test_write_u16(stream, 16U) &&
        test_write_bytes(stream, "data", 4U) &&
        test_write_u32(stream, TEST_FRAMES * 2U);
    for (frame = 0U; okay && frame < TEST_FRAMES; ++frame) {
        double time = (double)frame / (double)TEST_RATE;
        double sample = amplitude * sin(2.0 * TEST_PI * 220.0 * time);
        okay = test_write_u16(
            stream, (uint16_t)(int16_t)lrint(sample * 32767.0));
    }
    if (fclose(stream) != 0) okay = 0;
    return okay;
}

static char *test_profile_copy(HWAMeasurementSet *set, const char *text)
{
    size_t bytes = strlen(text) + 1U;
    char *copy = (char *)malloc(bytes);
    if (copy != NULL) {
        memcpy(copy, text, bytes);
        set->retained_work_bytes += (uint64_t)bytes;
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

static int test_key_for_split(HWAProductionSplit wanted, char key[32])
{
    unsigned candidate;
    for (candidate = 0U; candidate < 10000U; ++candidate) {
        HWAProductionSplit actual;
        int written = snprintf(key, 32U, "tail:%04u", candidate);
        if (written < 0 || written >= 32) return 0;
        if (hwa_production_split_for_item_key(key, &actual) == 0 &&
            actual == wanted) return 1;
    }
    return 0;
}

static int test_key_for_split_ordinal(HWAProductionSplit wanted,
                                      size_t ordinal,
                                      const char *prefix,
                                      char key[32])
{
    unsigned candidate;
    size_t found = 0U;
    for (candidate = 0U; candidate < 100000U; ++candidate) {
        HWAProductionSplit actual;
        int written = snprintf(key, 32U, "%s:%05u", prefix, candidate);
        if (written < 0 || written >= 32) return 0;
        if (hwa_production_split_for_item_key(key, &actual) == 0 &&
            actual == wanted) {
            if (found == ordinal) return 1;
            found++;
        }
    }
    return 0;
}

typedef struct TestProfileSpec {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    HWAEncoding encoding;
    uint32_t frames;
    uint32_t span_frames;
    size_t span_count;
    size_t train_count;
    HWAItemKind item_kind;
    const char *item_role;
    const double *levels;
    const size_t *logical_order;
    const uint32_t *item_quality_flags;
    const uint32_t *rms_quality_flags;
    const uint32_t *band_quality_flags;
    const double *band_levels;
    int include_eq_facts;
} TestProfileSpec;

static int test_make_profile_spec(
    HWAMeasurementSet *set,
    const char *stored_audio,
    const char audio_hash[HWA_SHA256_HEX_SIZE],
    const TestProfileSpec *spec)
{
    size_t item;
    uint64_t bytes_per_sample;
    uint64_t block_align;
    uint64_t data_bytes;
    size_t rows_per_item;
    char error[HWA_ERROR_SIZE] = {0};
    memset(set, 0, sizeof(*set));
    if (spec == NULL || spec->span_count == 0U ||
        spec->train_count > spec->span_count || spec->span_frames == 0U ||
        (uint64_t)spec->span_count * spec->span_frames > spec->frames) {
        return 0;
    }
    bytes_per_sample = spec->bits_per_sample / 8U;
    block_align = bytes_per_sample * spec->channels;
    data_bytes = block_align * spec->frames;
    rows_per_item = spec->include_eq_facts ? 23U : 2U;
    if (bytes_per_sample == 0U || block_align > UINT16_MAX) return 0;
    hwa_measurement_options_default(&set->options);
    set->options.fft_size = 1024U;
    set->options.hop_size = 128U;
    set->options.max_partials = 4U;
    set->options.max_work_bytes = UINT64_C(268435456);
    set->items_path = test_profile_copy(set, "/not-opened/items.hwa-items");
    set->audio_path = test_profile_copy(set, stored_audio);
    set->alignment_path = test_profile_copy(
        set, "/not-opened/take.hwa-align");
    set->source_score_path = test_profile_copy(
        set, "/not-opened/score.csv");
    test_hash(set->items_sha256, 'a');
    memcpy(set->audio_sha256, audio_hash, HWA_SHA256_HEX_SIZE);
    test_hash(set->alignment_sha256, 'c');
    test_hash(set->source_score_sha256, 'd');
    set->audio_format.container = HWA_CONTAINER_RIFF;
    set->audio_format.encoding = spec->encoding;
    set->audio_format.channels = spec->channels;
    set->audio_format.sample_rate_hz = spec->sample_rate;
    set->audio_format.bits_per_sample = spec->bits_per_sample;
    set->audio_format.valid_bits_per_sample = spec->bits_per_sample;
    set->audio_format.block_align = (uint16_t)block_align;
    set->audio_format.frames = spec->frames;
    set->audio_format.data_bytes = data_bytes;
    set->audio_format.duration_seconds =
        (double)spec->frames / (double)spec->sample_rate;
    if (spec->item_kind == HWA_ITEM_BODY) {
        double *reference_values = (double *)malloc(
            spec->span_count * sizeof(*reference_values));
        size_t count = 0U;
        size_t outer;
        if (reference_values == NULL) {
            hwa_measurement_set_free(set);
            return 0;
        }
        for (outer = 0U; outer < spec->span_count; ++outer) {
            double level = spec->levels == NULL ? -24.0 :
                spec->levels[outer];
            uint32_t quality = spec->item_quality_flags == NULL ? 0U :
                spec->item_quality_flags[outer];
            if ((quality & HWA_ITEM_QUALITY_LOW_CONFIDENCE) == 0U &&
                level >= set->options.spectral_floor_dbfs) {
                reference_values[count++] = level;
            }
        }
        for (outer = 1U; outer < count; ++outer) {
            double value = reference_values[outer];
            size_t inner = outer;
            while (inner > 0U && reference_values[inner - 1U] > value) {
                reference_values[inner] = reference_values[inner - 1U];
                inner--;
            }
            reference_values[inner] = value;
        }
        if (count != 0U) {
            set->level_reference_valid = 1;
            set->level_reference_item_count = count;
            set->level_reference_dbfs = count % 2U != 0U ?
                reference_values[count / 2U] :
                0.5 * (reference_values[count / 2U - 1U] +
                       reference_values[count / 2U]);
        }
        free(reference_values);
    }
    set->item_frame_evaluations =
        (uint64_t)spec->span_count * (uint64_t)rows_per_item;
    set->transform_count = spec->span_count;
    set->context_count = spec->span_count;
    set->measurement_count = spec->span_count * rows_per_item;
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
    for (item = 0U; item < spec->span_count; ++item) {
        HWAMeasureItemContext *context = &set->contexts[item];
        HWAMeasureObservation *raw =
            &set->measurements[item * rows_per_item];
        HWAMeasureObservation *relative = raw + 1;
        size_t logical = spec->logical_order == NULL ? item :
            spec->logical_order[item];
        HWAProductionSplit split;
        size_t split_ordinal;
        if (logical >= spec->span_count) {
            hwa_measurement_set_free(set);
            return 0;
        }
        split = logical < spec->train_count ?
            HWA_PRODUCTION_TRAIN : HWA_PRODUCTION_CHECK;
        split_ordinal = logical < spec->train_count ? logical :
            logical - spec->train_count;
        char key[32];
        if (!test_key_for_split_ordinal(
                split, split_ordinal, spec->item_role, key)) {
            hwa_measurement_set_free(set);
            return 0;
        }
        context->item_id = (uint64_t)item + 1U;
        context->item_key = test_profile_copy(set, key);
        context->item_kind = spec->item_kind;
        context->item_role = test_profile_copy(set, spec->item_role);
        context->start_sample = (uint64_t)item * spec->span_frames;
        context->end_sample = context->start_sample + spec->span_frames;
        context->source_event_count = 1U;
        context->item_confidence = 0.9;
        context->item_quality_flags = spec->item_quality_flags == NULL ? 0U :
            spec->item_quality_flags[logical];
        raw->id = (uint64_t)(item * rows_per_item) + 1U;
        raw->item_id = context->item_id;
        raw->kind = HWA_MEASURE_RMS_DBFS;
        raw->unit = HWA_MEASURE_UNIT_DBFS;
        raw->view = HWA_MEASURE_VIEW_RAW;
        raw->status = HWA_MEASURE_STATUS_VALID;
        raw->value = spec->levels == NULL ? -24.0 : spec->levels[logical];
        raw->confidence = 0.9;
        raw->quality_flags = spec->rms_quality_flags == NULL ? 0U :
            spec->rms_quality_flags[logical];
        relative->id = raw->id + 1U;
        relative->item_id = raw->item_id;
        relative->kind = raw->kind;
        relative->unit = HWA_MEASURE_UNIT_DB;
        relative->view = HWA_MEASURE_VIEW_LEVEL_RELATIVE;
        relative->confidence = raw->confidence;
        relative->quality_flags = raw->quality_flags;
        if (set->level_reference_valid) {
            relative->status = HWA_MEASURE_STATUS_VALID;
            relative->value = raw->value - set->level_reference_dbfs;
            relative->evidence_flags = HWA_MEASURE_EVIDENCE_LEVEL_REFERENCE;
        } else {
            relative->status = HWA_MEASURE_STATUS_NO_REFERENCE;
        }
        if (spec->include_eq_facts) {
            size_t band;
            for (band = 0U; band < HWA_BAND_COUNT; ++band) {
                HWAMeasureObservation *observation =
                    raw + 2U + band * 2U;
                HWAMeasureObservation *band_relative = observation + 1U;
                observation->id = raw->id + 2U + (uint64_t)band * 2U;
                observation->item_id = raw->item_id;
                observation->kind = HWA_MEASURE_BAND_LEVEL_DBFS;
                observation->index = (uint32_t)band;
                observation->unit = HWA_MEASURE_UNIT_DBFS;
                observation->view = HWA_MEASURE_VIEW_RAW;
                observation->status = HWA_MEASURE_STATUS_VALID;
                observation->value = spec->band_levels == NULL ? -30.0 :
                    spec->band_levels[logical];
                observation->confidence = 0.9;
                observation->quality_flags =
                    spec->band_quality_flags == NULL ? 0U :
                    spec->band_quality_flags[logical];
                band_relative->id = observation->id + 1U;
                band_relative->item_id = observation->item_id;
                band_relative->kind = observation->kind;
                band_relative->index = observation->index;
                band_relative->unit = HWA_MEASURE_UNIT_DB;
                band_relative->view = HWA_MEASURE_VIEW_LEVEL_RELATIVE;
                band_relative->confidence = observation->confidence;
                band_relative->quality_flags = observation->quality_flags;
                if (set->level_reference_valid) {
                    band_relative->status = HWA_MEASURE_STATUS_VALID;
                    band_relative->value = observation->value -
                        set->level_reference_dbfs;
                    band_relative->evidence_flags =
                        HWA_MEASURE_EVIDENCE_LEVEL_REFERENCE;
                } else {
                    band_relative->status =
                        HWA_MEASURE_STATUS_NO_REFERENCE;
                }
            }
            {
                HWAMeasureObservation *fixed =
                    raw + 2U + HWA_BAND_COUNT * 2U;
                fixed->id = raw->id + 2U + HWA_BAND_COUNT * 2U;
                fixed->item_id = raw->item_id;
                fixed->kind = HWA_MEASURE_FIXED_STATE_FRACTION;
                fixed->unit = HWA_MEASURE_UNIT_RATIO;
                fixed->view = HWA_MEASURE_VIEW_RAW;
                fixed->status = HWA_MEASURE_STATUS_VALID;
                fixed->value = 0.8;
                fixed->confidence = 0.9;
            }
        }
        if (context->item_key == NULL || context->item_role == NULL) {
            hwa_measurement_set_free(set);
            return 0;
        }
    }
    if (hwa_measure_build_profile(set, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "profile fixture: %s\n", error);
        hwa_measurement_set_free(set);
        return 0;
    }
    return 1;
}

static int test_make_profile(HWAMeasurementSet *set,
                             const char *stored_audio,
                             const char audio_hash[HWA_SHA256_HEX_SIZE],
                             double first,
                             double second)
{
    char keys[2][32];
    double levels[2];
    size_t item;
    char error[HWA_ERROR_SIZE] = {0};
    memset(set, 0, sizeof(*set));
    if (!test_key_for_split(HWA_PRODUCTION_TRAIN, keys[0]) ||
        !test_key_for_split(HWA_PRODUCTION_CHECK, keys[1])) return 0;
    hwa_measurement_options_default(&set->options);
    set->options.fft_size = 1024U;
    set->options.hop_size = 128U;
    set->options.max_partials = 4U;
    set->options.max_work_bytes = UINT64_C(67108864);
    set->items_path = test_profile_copy(set, "/not-opened/items.hwa-items");
    set->audio_path = test_profile_copy(set, stored_audio);
    set->alignment_path = test_profile_copy(set, "/not-opened/take.hwa-align");
    set->source_score_path = test_profile_copy(set, "/not-opened/score.csv");
    test_hash(set->items_sha256, 'a');
    memcpy(set->audio_sha256, audio_hash, HWA_SHA256_HEX_SIZE);
    test_hash(set->alignment_sha256, 'c');
    test_hash(set->source_score_sha256, 'd');
    set->audio_format.container = HWA_CONTAINER_RIFF;
    set->audio_format.encoding = HWA_ENCODING_PCM;
    set->audio_format.channels = 1U;
    set->audio_format.sample_rate_hz = TEST_RATE;
    set->audio_format.bits_per_sample = 16U;
    set->audio_format.valid_bits_per_sample = 16U;
    set->audio_format.block_align = 2U;
    set->audio_format.frames = TEST_FRAMES;
    set->audio_format.data_bytes = TEST_FRAMES * 2U;
    set->audio_format.duration_seconds = 1.0;
    set->item_frame_evaluations = 4U;
    set->transform_count = 2U;
    set->context_count = 2U;
    set->measurement_count = 4U;
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
    levels[0] = first;
    levels[1] = second;
    for (item = 0U; item < 2U; ++item) {
        HWAMeasureItemContext *context = &set->contexts[item];
        HWAMeasureObservation *raw = &set->measurements[item * 2U];
        HWAMeasureObservation *relative = raw + 1;
        context->item_id = (uint64_t)item + 1U;
        context->item_key = test_profile_copy(set, keys[item]);
        context->item_kind = HWA_ITEM_RELEASE;
        context->item_role = test_profile_copy(set, "tail");
        context->start_sample = (uint64_t)item * 800U;
        context->end_sample = context->start_sample + 800U;
        context->source_event_count = 1U;
        context->item_confidence = 0.9;
        raw->id = (uint64_t)item * 2U + 1U;
        raw->item_id = context->item_id;
        raw->kind = HWA_MEASURE_RMS_DBFS;
        raw->unit = HWA_MEASURE_UNIT_DBFS;
        raw->view = HWA_MEASURE_VIEW_RAW;
        raw->status = HWA_MEASURE_STATUS_VALID;
        raw->value = levels[item];
        raw->confidence = 0.9;
        relative->id = raw->id + 1U;
        relative->item_id = raw->item_id;
        relative->kind = raw->kind;
        relative->unit = HWA_MEASURE_UNIT_DB;
        relative->view = HWA_MEASURE_VIEW_LEVEL_RELATIVE;
        relative->status = HWA_MEASURE_STATUS_NO_REFERENCE;
        relative->confidence = raw->confidence;
        if (context->item_key == NULL || context->item_role == NULL) {
            hwa_measurement_set_free(set);
            return 0;
        }
    }
    if (hwa_measure_build_profile(set, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "profile fixture: %s\n", error);
        hwa_measurement_set_free(set);
        return 0;
    }
    return 1;
}

static int test_write_profile(const char *path,
                              const HWAMeasurementSet *set)
{
    FILE *stream = fopen(path, "wb");
    char error[HWA_ERROR_SIZE] = {0};
    int status;
    if (stream == NULL) return 0;
    status = hwa_measure_file_write(stream, set, error, sizeof(error));
    if (fclose(stream) != 0) status = -1;
    if (status != 0) (void)fprintf(stderr, "profile write: %s\n", error);
    return status == 0;
}

static int test_write_profile_pair(const TestFiles *files,
                                   const TestProfileSpec *reference_spec,
                                   const TestProfileSpec *model_spec)
{
    HWAMeasurementSet reference;
    HWAMeasurementSet model;
    char reference_hash[HWA_SHA256_HEX_SIZE];
    char model_hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE] = {0};
    int okay;
    memset(&reference, 0, sizeof(reference));
    memset(&model, 0, sizeof(model));
    if (hwa_sha256_file(files->reference_audio, UINT64_C(16777216),
                        reference_hash, error, sizeof(error)) != 0 ||
        hwa_sha256_file(files->model_audio, UINT64_C(16777216),
                        model_hash, error, sizeof(error)) != 0 ||
        !test_make_profile_spec(
            &reference, "/not-opened/reference.wav", reference_hash,
            reference_spec) ||
        !test_make_profile_spec(
            &model, "/not-opened/model.wav", model_hash, model_spec)) {
        hwa_measurement_set_free(&reference);
        hwa_measurement_set_free(&model);
        return 0;
    }
    okay = test_write_profile(files->reference_profile, &reference) &&
        test_write_profile(files->model_profile, &model);
    hwa_measurement_set_free(&reference);
    hwa_measurement_set_free(&model);
    return okay;
}

static int test_make_files(TestFiles *files)
{
    HWAMeasurementSet reference;
    HWAMeasurementSet model;
    char reference_hash[HWA_SHA256_HEX_SIZE];
    char model_hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE] = {0};
    int okay;
    memset(&reference, 0, sizeof(reference));
    memset(&model, 0, sizeof(model));
    if (!test_files_open(files) ||
        !test_write_wave(files->reference_audio, 0.10) ||
        !test_write_wave(files->model_audio, 0.07) ||
        hwa_sha256_file(files->reference_audio, UINT64_C(1048576),
                        reference_hash, error, sizeof(error)) != 0 ||
        hwa_sha256_file(files->model_audio, UINT64_C(1048576),
                        model_hash, error, sizeof(error)) != 0 ||
        !test_make_profile(&reference, "/not-opened/reference.wav",
                           reference_hash, -20.0, -22.0) ||
        !test_make_profile(&model, "/not-opened/model.wav",
                           model_hash, -23.0, -25.0)) {
        hwa_measurement_set_free(&reference);
        hwa_measurement_set_free(&model);
        return 0;
    }
    okay = test_write_profile(files->reference_profile, &reference) &&
        test_write_profile(files->model_profile, &model);
    hwa_measurement_set_free(&reference);
    hwa_measurement_set_free(&model);
    return okay;
}

static HWAProductionInputs test_inputs(const TestFiles *files)
{
    HWAProductionInputs inputs;
    inputs.reference_measures_path = files->reference_profile;
    inputs.reference_audio_path = files->reference_audio;
    inputs.model_measures_path = files->model_profile;
    inputs.model_audio_path = files->model_audio;
    inputs.room_ir_path = NULL;
    return inputs;
}

static double test_noise(uint64_t position)
{
    uint32_t value = (uint32_t)position ^ UINT32_C(0x9e3779b9);
    value ^= value >> 16U;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15U;
    value *= UINT32_C(0x846ca68b);
    value ^= value >> 16U;
    return (double)(value & UINT32_C(0xffff)) / 32767.5 - 1.0;
}

static double test_plain_sample(uint32_t frame,
                                uint16_t channel,
                                void *context)
{
    double amplitude = *(const double *)context;
    return amplitude * test_noise(
        (uint64_t)frame + (uint64_t)channel * UINT64_C(1000003));
}

static void test_run_eligibility_case(const char *label,
                                      const TestProfileSpec *spec,
                                      uint32_t expected_flags)
{
    TestFiles files;
    HWAProductionInputs inputs;
    HWAProductionResult result;
    double amplitude = 0.1;
    char error[HWA_ERROR_SIZE] = {0};
    memset(&result, 0, sizeof(result));
    CHECK(test_files_open(&files), "cannot open %s fixture directory", label);
    if (failures != 0) return;
    CHECK(test_write_pcm16_wave(
              files.reference_audio, spec->sample_rate, spec->channels,
              spec->frames, test_plain_sample, &amplitude) &&
          test_write_pcm16_wave(
              files.model_audio, spec->sample_rate, spec->channels,
              spec->frames, test_plain_sample, &amplitude) &&
          test_write_profile_pair(&files, spec, spec),
          "cannot build %s eligibility fixture", label);
    if (failures == 0) {
        inputs = test_inputs(&files);
        CHECK(hwa_account_production_files(
                  &inputs, NULL, &result, error, sizeof(error)) == 0,
              "%s eligibility account failed: %s", label, error);
    }
    CHECK(result.span_count == 2U &&
              result.spans != NULL &&
              result.spans[0].split == HWA_PRODUCTION_TRAIN &&
              result.spans[1].split == HWA_PRODUCTION_CHECK &&
              result.spans[0].eligibility_flags == expected_flags &&
              result.spans[1].eligibility_flags == expected_flags,
          "%s gates kept %zu spans or the wrong family mask", label,
          result.span_count);
    hwa_production_result_free(&result);
    test_files_close(&files);
}

static void test_low_confidence_and_floor_gates(void)
{
    static const double body_levels[7] = {
        -24.0, -24.0, -24.0, -120.0, -24.0, -24.0, -24.0
    };
    static const double body_band_levels[7] = {
        -30.0, -30.0, -30.0, -30.0, -30.0, -120.0, -30.0
    };
    static const uint32_t body_item_quality[7] = {
        0U, HWA_ITEM_QUALITY_LOW_CONFIDENCE, 0U, 0U, 0U, 0U, 0U
    };
    static const uint32_t body_rms_quality[7] = {
        0U, 0U, HWA_MEASURE_QUALITY_LOW_CONFIDENCE,
        HWA_MEASURE_QUALITY_NEAR_SPECTRAL_FLOOR, 0U, 0U, 0U
    };
    static const uint32_t body_band_quality[7] = {
        0U, 0U, 0U, 0U, HWA_MEASURE_QUALITY_LOW_CONFIDENCE,
        HWA_MEASURE_QUALITY_NEAR_SPECTRAL_FLOOR, 0U
    };
    static const double other_levels[5] = {
        -24.0, -24.0, -24.0, -120.0, -24.0
    };
    static const uint32_t other_item_quality[5] = {
        0U, HWA_ITEM_QUALITY_LOW_CONFIDENCE, 0U, 0U, 0U
    };
    static const uint32_t other_rms_quality[5] = {
        0U, 0U, HWA_MEASURE_QUALITY_LOW_CONFIDENCE,
        HWA_MEASURE_QUALITY_NEAR_SPECTRAL_FLOOR, 0U
    };
    TestProfileSpec body;
    TestProfileSpec note;
    TestProfileSpec decay;
    memset(&body, 0, sizeof(body));
    body.sample_rate = 8000U;
    body.channels = 1U;
    body.bits_per_sample = 16U;
    body.encoding = HWA_ENCODING_PCM;
    body.frames = 56000U;
    body.span_frames = 8000U;
    body.span_count = 7U;
    body.train_count = 6U;
    body.item_kind = HWA_ITEM_BODY;
    body.item_role = "body";
    body.levels = body_levels;
    body.item_quality_flags = body_item_quality;
    body.rms_quality_flags = body_rms_quality;
    body.band_quality_flags = body_band_quality;
    body.band_levels = body_band_levels;
    body.include_eq_facts = 1;
    note = body;
    note.channels = 2U;
    note.frames = 40000U;
    note.span_count = 5U;
    note.train_count = 4U;
    note.item_kind = HWA_ITEM_NOTE;
    note.item_role = "note";
    note.levels = other_levels;
    note.item_quality_flags = other_item_quality;
    note.rms_quality_flags = other_rms_quality;
    note.band_quality_flags = NULL;
    note.band_levels = NULL;
    note.include_eq_facts = 0;
    decay = note;
    decay.channels = 1U;
    decay.item_kind = HWA_ITEM_RELEASE;
    decay.item_role = "tail";
    test_run_eligibility_case(
        "EQ", &body, HWA_PRODUCTION_SPAN_EQ);
    test_run_eligibility_case(
        "dynamics/stereo", &note,
        HWA_PRODUCTION_SPAN_DYNAMICS | HWA_PRODUCTION_SPAN_STEREO);
    test_run_eligibility_case(
        "decay", &decay, HWA_PRODUCTION_SPAN_DECAY);
}

typedef struct TestRankedKey {
    char key[32];
    unsigned char digest[32];
} TestRankedKey;

static int test_ranked_key_compare(const void *left, const void *right)
{
    const TestRankedKey *a = (const TestRankedKey *)left;
    const TestRankedKey *b = (const TestRankedKey *)right;
    int order = memcmp(a->digest, b->digest, sizeof(a->digest));
    return order != 0 ? order : strcmp(a->key, b->key);
}

static int test_key_in_rank_prefix(const char *key,
                                   const TestRankedKey *ranked,
                                   size_t keep)
{
    size_t index;
    for (index = 0U; index < keep; ++index) {
        if (strcmp(key, ranked[index].key) == 0) return 1;
    }
    return 0;
}

static void test_sampling_case(const char *label,
                               TestProfileSpec *spec,
                               uint32_t expected_flags)
{
    enum { TOTAL = 100, TRAIN = 80, CHECK = 20 };
    TestFiles files;
    HWAProductionInputs inputs;
    HWAProductionResult forward;
    HWAProductionResult reverse_result;
    TestRankedKey train_ranked[TRAIN];
    TestRankedKey check_ranked[CHECK];
    size_t reverse_order[TOTAL];
    double levels[TOTAL];
    double band_levels[TOTAL];
    double amplitude = 0.1;
    char error[HWA_ERROR_SIZE] = {0};
    size_t logical;
    size_t span_index;
    size_t train_count = 0U;
    size_t check_count = 0U;
    memset(&forward, 0, sizeof(forward));
    memset(&reverse_result, 0, sizeof(reverse_result));
    for (logical = 0U; logical < TOTAL; ++logical) {
        HWAProductionSplit split = logical < TRAIN ?
            HWA_PRODUCTION_TRAIN : HWA_PRODUCTION_CHECK;
        size_t ordinal = logical < TRAIN ? logical : logical - TRAIN;
        TestRankedKey *ranked = split == HWA_PRODUCTION_TRAIN ?
            &train_ranked[ordinal] : &check_ranked[ordinal];
        HWASha256 sha;
        reverse_order[logical] = TOTAL - logical - 1U;
        levels[logical] = -24.0;
        band_levels[logical] = -30.0;
        CHECK(test_key_for_split_ordinal(
                  split, ordinal, spec->item_role, ranked->key),
              "cannot build %s ranked key %zu", label, logical);
        hwa_sha256_init(&sha);
        hwa_sha256_update(
            &sha, (const unsigned char *)ranked->key, strlen(ranked->key));
        hwa_sha256_final(&sha, ranked->digest);
    }
    qsort(train_ranked, TRAIN, sizeof(train_ranked[0]),
          test_ranked_key_compare);
    qsort(check_ranked, CHECK, sizeof(check_ranked[0]),
          test_ranked_key_compare);
    spec->span_count = TOTAL;
    spec->train_count = TRAIN;
    spec->frames = (uint32_t)(TOTAL * spec->span_frames);
    spec->levels = levels;
    if (spec->include_eq_facts) spec->band_levels = band_levels;
    CHECK(test_files_open(&files), "cannot open %s sampling fixture", label);
    if (failures != 0) return;
    CHECK(test_write_pcm16_wave(
              files.reference_audio, spec->sample_rate, spec->channels,
              spec->frames, test_plain_sample, &amplitude) &&
          test_write_pcm16_wave(
              files.model_audio, spec->sample_rate, spec->channels,
              spec->frames, test_plain_sample, &amplitude) &&
          test_write_profile_pair(&files, spec, spec),
          "cannot build forward %s sampling fixture", label);
    inputs = test_inputs(&files);
    if (failures == 0) {
        CHECK(hwa_account_production_files(
                  &inputs, NULL, &forward, error, sizeof(error)) == 0,
              "forward %s sampling account failed: %s", label, error);
    }
    spec->logical_order = reverse_order;
    CHECK(test_write_profile_pair(&files, spec, spec),
          "cannot build reversed %s sampling fixture", label);
    memset(error, 0, sizeof(error));
    if (failures == 0) {
        CHECK(hwa_account_production_files(
                  &inputs, NULL, &reverse_result, error, sizeof(error)) == 0,
              "reversed %s sampling account failed: %s", label, error);
    }
    CHECK(forward.span_count == 80U && reverse_result.span_count == 80U,
          "%s sampling did not retain a 64/16 union", label);
    if (forward.span_count == 80U && reverse_result.span_count == 80U) {
        for (span_index = 0U; span_index < forward.span_count; ++span_index) {
            const HWAProductionSpan *a = &forward.spans[span_index];
            const HWAProductionSpan *b = &reverse_result.spans[span_index];
            int expected_key = a->split == HWA_PRODUCTION_TRAIN ?
                test_key_in_rank_prefix(a->item_key, train_ranked, 64U) :
                test_key_in_rank_prefix(a->item_key, check_ranked, 16U);
            CHECK(expected_key,
                  "%s sampling kept a key outside the SHA rank prefix", label);
            CHECK(a->eligibility_flags == expected_flags,
                  "%s sampling leaked an unselected family flag", label);
            CHECK(a->split == b->split &&
                      a->eligibility_flags == b->eligibility_flags &&
                      strcmp(a->item_key, b->item_key) == 0,
                  "%s selected keys changed with input order", label);
            if (a->split == HWA_PRODUCTION_TRAIN) train_count++;
            else check_count++;
        }
        CHECK(train_count == 64U && check_count == 16U,
              "%s sampling retained %zu TRAIN/%zu CHECK", label,
              train_count, check_count);
        CHECK(hwa_production_result_validate(
                  &forward, error, sizeof(error)) == 0 &&
              hwa_production_result_validate(
                  &reverse_result, error, sizeof(error)) == 0,
              "%s sampled result failed validation: %s", label, error);
    }
    hwa_production_result_free(&reverse_result);
    hwa_production_result_free(&forward);
    test_files_close(&files);
}

static void test_family_sampling_and_order_invariance(void)
{
    TestProfileSpec eq;
    TestProfileSpec note;
    TestProfileSpec decay;
    memset(&eq, 0, sizeof(eq));
    eq.sample_rate = 8000U;
    eq.channels = 1U;
    eq.bits_per_sample = 16U;
    eq.encoding = HWA_ENCODING_PCM;
    eq.span_frames = 2000U;
    eq.item_kind = HWA_ITEM_BODY;
    eq.item_role = "body";
    eq.include_eq_facts = 1;
    note = eq;
    note.channels = 2U;
    note.span_frames = 8000U;
    note.item_kind = HWA_ITEM_NOTE;
    note.item_role = "note";
    note.include_eq_facts = 0;
    decay = note;
    decay.channels = 1U;
    decay.span_frames = 400U;
    decay.item_kind = HWA_ITEM_RELEASE;
    decay.item_role = "tail";
    test_sampling_case("EQ", &eq, HWA_PRODUCTION_SPAN_EQ);
    test_sampling_case(
        "dynamics/stereo", &note,
        HWA_PRODUCTION_SPAN_DYNAMICS | HWA_PRODUCTION_SPAN_STEREO);
    test_sampling_case("decay", &decay, HWA_PRODUCTION_SPAN_DECAY);
}

typedef struct TestStereoSignal {
    uint32_t span_frames;
    int model;
} TestStereoSignal;

static double test_stereo_sample(uint32_t frame,
                                 uint16_t channel,
                                 void *context)
{
    static const double model_amplitudes[10] = {
        0.010, 0.016, 0.026, 0.043, 0.071,
        0.116, 0.190, 0.310, 0.080, 0.210
    };
    static const unsigned reference_delays[10] = {
        0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U
    };
    static const unsigned model_delays[10] = {
        5U, 4U, 3U, 2U, 1U, 0U, 7U, 6U, 9U, 8U
    };
    static const double reference_gains[10] = {
        0.55, 0.62, 0.70, 0.78, 0.86,
        0.94, 1.02, 1.10, 0.67, 0.98
    };
    static const double model_gains[10] = {
        1.10, 1.02, 0.94, 0.86, 0.78,
        0.70, 0.62, 0.55, 0.91, 0.73
    };
    TestStereoSignal *signal = (TestStereoSignal *)context;
    size_t span = frame / signal->span_frames;
    uint32_t local = frame % signal->span_frames;
    double model_amplitude = model_amplitudes[span];
    double model_db = 20.0 * log10(model_amplitude);
    double reference_db = model_db <= -30.0 ? model_db + 1.0 :
        -29.0 + (model_db + 30.0) / 2.0;
    double amplitude = signal->model ? model_amplitude :
        pow(10.0, reference_db / 20.0);
    unsigned delay = signal->model ? model_delays[span] :
        reference_delays[span];
    double gain = signal->model ? model_gains[span] :
        reference_gains[span];
    uint64_t seed = (uint64_t)span * UINT64_C(1000003);
    if (channel >= 2U) return 0.0;
    if (channel == 0U) return amplitude * test_noise(seed + local);
    if (local < signal->span_frames / 2U) {
        return amplitude * gain *
            test_noise(seed + UINT64_C(500000) + local);
    }
    if (local < delay) return 0.0;
    return amplitude * gain * test_noise(seed + local - delay);
}

static const HWAProductionFit *test_find_fit(
    const HWAProductionResult *result,
    HWAProductionScope scope,
    HWAProductionFitKind kind,
    uint32_t index)
{
    size_t fit_index;
    for (fit_index = 0U; fit_index < result->fit_count; ++fit_index) {
        const HWAProductionFit *fit = &result->fits[fit_index];
        if (fit->scope == scope && fit->kind == kind &&
            fit->index == index) return fit;
    }
    return NULL;
}

static int test_fit_has_interval(const HWAProductionFit *fit)
{
    return fit != NULL && fit->availability == HWA_PRODUCTION_AVAILABLE &&
        fit->estimate_valid && fit->uncertainty_valid &&
        fit->q05 <= fit->estimate && fit->estimate <= fit->q95;
}

static const HWAProductionEvaluation *test_find_evaluation(
    const HWAProductionResult *result,
    uint64_t span_id,
    HWAProductionView view,
    HWAProductionMetricKind kind,
    uint32_t index)
{
    size_t row_index;
    for (row_index = 0U; row_index < result->evaluation_row_count;
         ++row_index) {
        const HWAProductionEvaluation *row = &result->evaluations[row_index];
        if (row->span_id == span_id && row->view == view &&
            row->kind == kind && row->index == index) return row;
    }
    return NULL;
}

static void test_dynamics_and_stereo(void)
{
    static const double profile_levels[10] = {
        -42.0, -38.0, -34.0, -30.0, -26.0,
        -22.0, -18.0, -14.0, -28.0, -20.0
    };
    TestFiles files;
    TestStereoSignal reference_signal = {8000U, 0};
    TestStereoSignal model_signal = {8000U, 1};
    TestProfileSpec reference_spec = {
        8000U, 2U, 16U, HWA_ENCODING_PCM, 80000U, 8000U,
        10U, 8U, HWA_ITEM_NOTE, "note", profile_levels,
        NULL, NULL, NULL, NULL, NULL, 0
    };
    TestProfileSpec model_spec = reference_spec;
    HWAProductionInputs inputs;
    HWAProductionResult result;
    const HWAProductionFit *fit;
    char error[HWA_ERROR_SIZE] = {0};
    size_t index;
    int varied_dynamics = 0;
    int varied_stereo = 0;
    memset(&result, 0, sizeof(result));
    model_spec.channels = 4U;
    CHECK(test_files_open(&files), "cannot open stereo fixture directory");
    if (failures != 0) return;
    CHECK(test_write_pcm16_wave(
              files.reference_audio, 8000U, 2U, 80000U,
              test_stereo_sample, &reference_signal) &&
          test_write_pcm16_wave(
              files.model_audio, 8000U, 4U, 80000U,
              test_stereo_sample, &model_signal) &&
          test_write_profile_pair(&files, &reference_spec, &model_spec),
          "cannot build stereo/dynamics fixture");
    if (failures == 0) {
        inputs = test_inputs(&files);
        CHECK(hwa_account_production_files(
                  &inputs, NULL, &result, error, sizeof(error)) == 0,
              "2ch-vs-4ch production account failed: %s", error);
    }
    CHECK(result.span_count == 10U,
          "stereo fixture retained %zu spans", result.span_count);
    if (result.span_count == 10U) {
        static const double expected_reference_delays[10] = {
            0.0, 1.0, 2.0, 3.0, 4.0,
            5.0, 6.0, 7.0, 8.0, 9.0
        };
        static const double expected_model_delays[10] = {
            5.0, 4.0, 3.0, 2.0, 1.0,
            0.0, 7.0, 6.0, 9.0, 8.0
        };
        for (index = 0U; index < result.span_count; ++index) {
            const HWAProductionSpan *span = &result.spans[index];
            const HWAProductionEvaluation *delay = test_find_evaluation(
                &result, span->id, HWA_PRODUCTION_VIEW_RAW,
                HWA_PRODUCTION_METRIC_STEREO_DELAY_SAMPLES, 0U);
            CHECK((result.spans[index].eligibility_flags &
                   (HWA_PRODUCTION_SPAN_DYNAMICS |
                    HWA_PRODUCTION_SPAN_STEREO)) ==
                  (HWA_PRODUCTION_SPAN_DYNAMICS |
                   HWA_PRODUCTION_SPAN_STEREO),
                  "note span %zu lost dynamics/stereo eligibility", index);
            CHECK(delay != NULL &&
                      delay->availability == HWA_PRODUCTION_AVAILABLE &&
                      delay->reference_value ==
                          expected_reference_delays[
                              span->reference_item_id - 1U] &&
                      delay->model_value ==
                          expected_model_delays[span->model_item_id - 1U],
                  "full-span delay probe got the wrong signed lag for item %llu",
                  (unsigned long long)span->reference_item_id);
        }
        for (index = (size_t)HWA_PRODUCTION_FIT_THRESHOLD_DBFS;
             index <= (size_t)HWA_PRODUCTION_FIT_MAKEUP_DB; ++index) {
            fit = test_find_fit(
                &result, HWA_PRODUCTION_SCOPE_CORRECTION,
                (HWAProductionFitKind)index, 0U);
            CHECK(test_fit_has_interval(fit),
                  "dynamics fit %zu lacks a valid interval", index);
            if (fit != NULL && fit->uncertainty_valid &&
                fit->q05 < fit->q95) varied_dynamics = 1;
        }
        for (index = (size_t)HWA_PRODUCTION_FIT_STEREO_DELAY_SAMPLES;
             index <= (size_t)HWA_PRODUCTION_FIT_STEREO_CORRELATION;
             ++index) {
            HWAProductionFitKind kind = (HWAProductionFitKind)index;
            if (kind == HWA_PRODUCTION_FIT_CHANNEL_POLARITY) continue;
            fit = test_find_fit(
                &result, HWA_PRODUCTION_SCOPE_CORRECTION, kind, 0U);
            CHECK(test_fit_has_interval(fit),
                  "stereo fit %zu lacks a valid interval", index);
            if (fit != NULL && fit->uncertainty_valid &&
                fit->q05 < fit->q95) varied_stereo = 1;
        }
        fit = test_find_fit(
            &result, HWA_PRODUCTION_SCOPE_CORRECTION,
            HWA_PRODUCTION_FIT_CHANNEL_POLARITY, 0U);
        CHECK(fit != NULL && fit->availability == HWA_PRODUCTION_AVAILABLE &&
                  fit->estimate_valid && !fit->uncertainty_valid,
              "relative channel polarity has false uncertainty");
        CHECK(varied_dynamics,
              "varied dynamics fixture produced copied fit bounds");
        CHECK(varied_stereo,
              "varied stereo fixture produced copied fit bounds");
        CHECK(hwa_production_result_validate(
                  &result, error, sizeof(error)) == 0,
              "2ch-vs-4ch result does not validate: %s", error);
    }
    hwa_production_result_free(&result);
    test_files_close(&files);
}

typedef struct TestDecaySignal {
    uint32_t sample_rate;
    uint32_t span_frames;
    int model;
    int room_ir;
} TestDecaySignal;

static double test_decay_sample(uint32_t frame,
                                uint16_t channel,
                                void *context)
{
    static const double reference_tau[10] = {
        0.030, 0.034, 0.038, 0.043, 0.049,
        0.056, 0.064, 0.073, 0.045, 0.061
    };
    static const double model_tau[10] = {
        0.026, 0.031, 0.036, 0.041, 0.047,
        0.054, 0.062, 0.071, 0.039, 0.058
    };
    TestDecaySignal *signal = (TestDecaySignal *)context;
    size_t span = signal->room_ir ? 0U : frame / signal->span_frames;
    uint32_t local = signal->room_ir ? frame :
        frame % signal->span_frames;
    double tau = signal->room_ir ? 0.055 :
        (signal->model ? model_tau[span] : reference_tau[span]);
    double time = (double)local / (double)signal->sample_rate;
    uint64_t seed = (uint64_t)span * UINT64_C(2000003) + local;
    (void)channel;
    return 0.55 * exp(-time / tau) * test_noise(seed);
}

static void test_decay_quantiles_and_low_rate_ir(void)
{
    static const double profile_levels[10] = {
        -20.0, -21.0, -22.0, -23.0, -24.0,
        -25.0, -26.0, -27.0, -23.5, -25.5
    };
    const uint64_t expected_evaluations = UINT64_C(3416192);
    TestFiles files;
    TestDecaySignal reference_signal = {48000U, 24000U, 0, 0};
    TestDecaySignal model_signal = {48000U, 24000U, 1, 0};
    TestDecaySignal ir_signal = {8000U, 8000U, 0, 1};
    TestProfileSpec reference_spec = {
        48000U, 1U, 16U, HWA_ENCODING_PCM, 240000U, 24000U,
        10U, 8U, HWA_ITEM_RELEASE, "tail", profile_levels,
        NULL, NULL, NULL, NULL, NULL, 0
    };
    TestProfileSpec model_spec = reference_spec;
    HWAProductionInputs inputs;
    HWAProductionOptions options;
    HWAProductionResult result;
    char error[HWA_ERROR_SIZE] = {0};
    size_t kind_index;
    size_t band;
    int early_available = 0;
    int late_available = 0;
    int varied_room = 0;
    memset(&result, 0, sizeof(result));
    CHECK(test_files_open(&files), "cannot open decay fixture directory");
    if (failures != 0) return;
    CHECK(test_write_pcm16_wave(
              files.reference_audio, 48000U, 1U, 240000U,
              test_decay_sample, &reference_signal) &&
          test_write_pcm16_wave(
              files.model_audio, 48000U, 1U, 240000U,
              test_decay_sample, &model_signal) &&
          test_write_pcm16_wave(
              files.room_ir, 8000U, 1U, 8000U,
              test_decay_sample, &ir_signal) &&
          test_write_profile_pair(&files, &reference_spec, &model_spec),
          "cannot build decay/IR fixture");
    inputs = test_inputs(&files);
    inputs.room_ir_path = files.room_ir;
    hwa_production_options_default(&options);
    options.max_evaluations = expected_evaluations;
    if (failures == 0) {
        CHECK(hwa_account_production_files(
                  &inputs, &options, &result, error, sizeof(error)) == 0,
              "decay/IR production account failed at exact cap: %s", error);
    }
    CHECK(result.span_count == 10U,
          "decay fixture retained %zu spans", result.span_count);
    if (result.span_count == 10U) {
        CHECK(result.evaluation_count == expected_evaluations,
              "decay evaluation ledger is %llu, expected %llu",
              (unsigned long long)result.evaluation_count,
              (unsigned long long)expected_evaluations);
        for (kind_index =
                 (size_t)HWA_PRODUCTION_FIT_EARLY_REFLECTION_DB;
             kind_index <=
                 (size_t)HWA_PRODUCTION_FIT_LATE_DECAY_SECONDS;
             ++kind_index) {
            HWAProductionFitKind kind = (HWAProductionFitKind)kind_index;
            for (band = 0U; band < 6U; ++band) {
                const HWAProductionFit *reference_fit = test_find_fit(
                    &result, HWA_PRODUCTION_SCOPE_REFERENCE,
                    kind, (uint32_t)band);
                const HWAProductionFit *model_fit = test_find_fit(
                    &result, HWA_PRODUCTION_SCOPE_MODEL,
                    kind, (uint32_t)band);
                const HWAProductionFit *ir_fit = test_find_fit(
                    &result, HWA_PRODUCTION_SCOPE_ROOM_IR,
                    kind, (uint32_t)band);
                if (reference_fit != NULL &&
                    reference_fit->availability == HWA_PRODUCTION_AVAILABLE) {
                    CHECK(test_fit_has_interval(reference_fit),
                          "reference room fit lacks a true interval");
                    CHECK(test_fit_has_interval(model_fit),
                          "model room fit lacks a true interval");
                    if (reference_fit->q05 < reference_fit->q95 ||
                        (model_fit != NULL &&
                         model_fit->q05 < model_fit->q95)) varied_room = 1;
                    if (kind == HWA_PRODUCTION_FIT_EARLY_REFLECTION_DB) {
                        early_available = 1;
                    } else {
                        late_available = 1;
                    }
                }
                if (band >= 4U) {
                    CHECK(ir_fit != NULL &&
                              ir_fit->availability ==
                                  HWA_PRODUCTION_UNAVAILABLE &&
                              !ir_fit->uncertainty_valid &&
                              ir_fit->point_count == 0U,
                          "unsupported low-rate IR fit is not unavailable");
                } else if (ir_fit != NULL &&
                           ir_fit->availability == HWA_PRODUCTION_AVAILABLE) {
                    CHECK(ir_fit->estimate_valid &&
                              !ir_fit->uncertainty_valid,
                          "one supplied IR claims an uncertainty interval");
                }
            }
        }
        CHECK(early_available, "decay fixture has no early-energy fit");
        CHECK(late_available, "decay fixture has no T20 fit");
        CHECK(varied_room, "varied room facts produced copied fit bounds");
        for (band = 0U; band < result.evaluation_row_count; ++band) {
            const HWAProductionEvaluation *row = &result.evaluations[band];
            if (row->view == HWA_PRODUCTION_VIEW_ROOM_MATCHED &&
                (row->kind ==
                     HWA_PRODUCTION_METRIC_EARLY_REFLECTION_DB ||
                 row->kind ==
                     HWA_PRODUCTION_METRIC_LATE_DECAY_SECONDS) &&
                row->index >= 4U) {
                CHECK(row->availability == HWA_PRODUCTION_UNAVAILABLE,
                      "unsupported room-matched IR row is not unavailable");
            }
        }
        CHECK(hwa_production_result_validate(
                  &result, error, sizeof(error)) == 0,
              "decay/low-rate-IR result does not validate: %s", error);
    }
    hwa_production_result_free(&result);
    options.max_evaluations = expected_evaluations - 1U;
    memset(error, 0, sizeof(error));
    CHECK(hwa_account_production_files(
              &inputs, &options, &result, error, sizeof(error)) != 0 &&
              result.sources == NULL && result.spans == NULL,
          "one-under decay evaluation cap did not fail transactionally");
    hwa_production_result_free(&result);
    test_files_close(&files);
}

static void test_huge_finite_float_rejected(void)
{
    static const double profile_levels[2] = {-20.0, -21.0};
    TestFiles files;
    TestProfileSpec spec = {
        8000U, 1U, 64U, HWA_ENCODING_IEEE_FLOAT, 8000U, 800U,
        2U, 1U, HWA_ITEM_RELEASE, "tail", profile_levels,
        NULL, NULL, NULL, NULL, NULL, 0
    };
    HWAProductionInputs inputs;
    HWAProductionResult result;
    char error[HWA_ERROR_SIZE] = {0};
    memset(&result, 0xa5, sizeof(result));
    CHECK(test_files_open(&files), "cannot open huge-float fixture directory");
    if (failures != 0) return;
    CHECK(test_write_huge_float64_wave(
              files.reference_audio, 8000U, 8000U) &&
          test_write_huge_float64_wave(
              files.model_audio, 8000U, 8000U) &&
          test_write_profile_pair(&files, &spec, &spec),
          "cannot build huge finite float64 fixture");
    if (failures == 0) {
        inputs = test_inputs(&files);
        CHECK(hwa_account_production_files(
                  &inputs, NULL, &result, error, sizeof(error)) != 0 &&
                  result.sources == NULL && result.spans == NULL &&
                  strstr(error, "numeric range") != NULL,
              "huge finite float64 rejection was not numeric and transactional: %s",
              error);
    }
    hwa_production_result_free(&result);
    test_files_close(&files);
}

static void test_changed_audio_rejected(void)
{
    TestFiles files;
    HWAProductionInputs inputs;
    HWAProductionResult result;
    FILE *stream;
    char error[HWA_ERROR_SIZE] = {0};
    memset(&result, 0xa5, sizeof(result));
    CHECK(test_make_files(&files), "cannot make changed-audio fixture");
    if (failures != 0) return;
    stream = fopen(files.model_audio, "r+b");
    CHECK(stream != NULL && fseek(stream, 44L, SEEK_SET) == 0 &&
              fputc(1, stream) != EOF && fclose(stream) == 0,
          "cannot mutate explicit model audio fixture");
    if (failures == 0) {
        inputs = test_inputs(&files);
        CHECK(hwa_account_production_files(
                  &inputs, NULL, &result, error, sizeof(error)) != 0 &&
                  result.sources == NULL && result.spans == NULL &&
                  strstr(error, "hash") != NULL,
              "changed explicit model audio was not rejected by hash: %s",
              error);
    }
    hwa_production_result_free(&result);
    test_files_close(&files);
}

static void test_public_account(const TestFiles *files)
{
    HWAProductionInputs inputs = test_inputs(files);
    HWAProductionOptions one;
    HWAProductionOptions many;
    HWAProductionResult a;
    HWAProductionResult b;
    char error[HWA_ERROR_SIZE] = {0};
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    hwa_production_options_default(&one);
    many = one;
    one.decode_block_frames = 1U;
    many.decode_block_frames = 4096U;
    CHECK(hwa_account_production_files(
              &inputs, &one, &a, error, sizeof(error)) == 0,
          "public account failed: %s", error);
    CHECK(hwa_account_production_files(
              &inputs, &many, &b, error, sizeof(error)) == 0,
          "second public account failed: %s", error);
    CHECK(a.span_count == 2U && b.span_count == 2U,
          "public account kept the wrong span count");
    if (a.span_count == 2U && b.span_count == 2U) {
        CHECK(a.fit_count == hwa_production_fit_catalog_count() &&
                  a.evaluation_row_count ==
                      a.span_count *
                      ((size_t)HWA_PRODUCTION_VIEW_COUNT - 1U) *
                      hwa_production_metric_catalog_count(),
              "public account omitted canonical rows");
        CHECK(a.evaluation_count == b.evaluation_count &&
                  a.evaluation_row_count == b.evaluation_row_count &&
                  memcmp(a.evaluations, b.evaluations,
                         a.evaluation_row_count * sizeof(*a.evaluations)) == 0,
              "analysis changed with decode block size");
        CHECK(hwa_production_result_validate(
                  &a, error, sizeof(error)) == 0,
              "public result does not validate: %s", error);
        CHECK(a.warning_count ==
                  hwa_production_warning_spec_count(&a),
              "public warnings do not match the method catalog");
        {
            size_t index;
            for (index = 0U; index < a.fit_count; ++index) {
                const HWAProductionFit *fit = &a.fits[index];
                if ((fit->kind ==
                         HWA_PRODUCTION_FIT_EARLY_REFLECTION_DB ||
                     fit->kind ==
                         HWA_PRODUCTION_FIT_LATE_DECAY_SECONDS) &&
                    fit->index >= 4U) {
                    CHECK(fit->availability ==
                              HWA_PRODUCTION_UNAVAILABLE &&
                              fit->point_count == 0U,
                          "unsupported low-rate room fit is not unavailable");
                }
            }
            for (index = 0U; index < a.evaluation_row_count; ++index) {
                const HWAProductionEvaluation *row = &a.evaluations[index];
                if ((row->kind ==
                         HWA_PRODUCTION_METRIC_EARLY_REFLECTION_DB ||
                     row->kind ==
                         HWA_PRODUCTION_METRIC_LATE_DECAY_SECONDS) &&
                    row->index >= 4U) {
                    CHECK(row->availability ==
                              HWA_PRODUCTION_UNAVAILABLE,
                          "unsupported low-rate room evaluation is not unavailable");
                }
            }
        }
    }
    hwa_production_result_free(&b);
    hwa_production_result_free(&a);
}

static void test_self_account_and_ir(const TestFiles *files)
{
    HWAProductionInputs inputs = test_inputs(files);
    HWAProductionResult result;
    char error[HWA_ERROR_SIZE] = {0};
    memset(&result, 0, sizeof(result));
    inputs.model_measures_path = files->reference_profile;
    inputs.model_audio_path = files->reference_audio;
    CHECK(hwa_account_production_files(
              &inputs, NULL, &result, error, sizeof(error)) == 0,
          "byte-identical self account failed: %s", error);
    hwa_production_result_free(&result);
    inputs = test_inputs(files);
    inputs.room_ir_path = files->reference_audio;
    CHECK(hwa_account_production_files(
              &inputs, NULL, &result, error, sizeof(error)) == 0 &&
              result.source_count == 5U,
          "explicit room IR account failed: %s", error);
    hwa_production_result_free(&result);
}

static void test_caps_and_free(const TestFiles *files)
{
    HWAProductionInputs inputs = test_inputs(files);
    HWAProductionOptions options;
    HWAProductionResult result;
    char error[HWA_ERROR_SIZE] = {0};
    uint64_t low;
    uint64_t high;
    memset(&result, 0xa5, sizeof(result));
    hwa_production_options_default(&options);
    options.max_spans = 1U;
    CHECK(hwa_account_production_files(
              &inputs, &options, &result, error, sizeof(error)) != 0 &&
              result.sources == NULL && result.spans == NULL,
          "one-under span cap did not fail transactionally");
    hwa_production_options_default(&options);
    low = 1U;
    high = options.max_work_bytes;
    while (low < high) {
        uint64_t middle = low + (high - low) / 2U;
        int status;
        options.max_work_bytes = middle;
        memset(error, 0, sizeof(error));
        status = hwa_account_production_files(
            &inputs, &options, &result, error, sizeof(error));
        hwa_production_result_free(&result);
        if (status == 0) high = middle;
        else low = middle + 1U;
    }
    options.max_work_bytes = low;
    memset(error, 0, sizeof(error));
    CHECK(hwa_account_production_files(
              &inputs, &options, &result, error, sizeof(error)) == 0,
          "exact Stage 6 work cap failed at %llu bytes: %s",
          (unsigned long long)low, error);
    hwa_production_result_free(&result);
    options.max_work_bytes = low - 1U;
    memset(error, 0, sizeof(error));
    CHECK(hwa_account_production_files(
              &inputs, &options, &result, error, sizeof(error)) != 0 &&
              result.sources == NULL && result.spans == NULL,
          "one byte below the exact Stage 6 work cap passed");
    hwa_production_result_free(&result);
    memset(&result, 0, sizeof(result));
    result.source_count = 1U;
    result.sources = (HWAProductionSource *)calloc(
        1U, sizeof(*result.sources));
    if (result.sources != NULL) {
        result.sources[0].role = (char *)malloc(2U);
        if (result.sources[0].role != NULL) {
            memcpy(result.sources[0].role, "x", 2U);
        }
    }
    hwa_production_result_free(&result);
    CHECK(result.sources == NULL && result.source_count == 0U,
          "partial Stage 6 free did not clear ownership");
}

static void test_method_helpers(void)
{
    CHECK(hwa_production_fit_catalog_count() == 61U &&
              hwa_production_metric_catalog_count() == 29U,
          "Stage 6 method catalogs changed size");
    CHECK(hwa_production_fit_value_valid(
              HWA_PRODUCTION_SCOPE_CORRECTION,
              HWA_PRODUCTION_FIT_THRESHOLD_DBFS, 0U, -47.0,
              48000U, 48000U) &&
              !hwa_production_fit_value_valid(
                  HWA_PRODUCTION_SCOPE_CORRECTION,
                  HWA_PRODUCTION_FIT_THRESHOLD_DBFS, 0U, -47.5,
                  48000U, 48000U),
          "dynamics threshold grid is not exact");
    CHECK(hwa_production_metric_value_valid(
              HWA_PRODUCTION_METRIC_STEREO_DELAY_SAMPLES, 0U,
              97.0, 48500U) &&
              !hwa_production_metric_value_valid(
                  HWA_PRODUCTION_METRIC_STEREO_DELAY_SAMPLES, 0U,
                  98.0, 48500U),
          "delay domain does not use floor(2 ms * rate)");
    CHECK(hwa_production_room_band_supported(4U, 48000U) &&
              !hwa_production_room_band_supported(5U, 16000U),
          "room-band sample-rate support rule changed");
}

static void test_unbracketed_check_band_is_unavailable(void)
{
    HWAProductionResult result;
    HWAProductionSource sources[5];
    HWAProductionSpan span;
    HWAProductionEvaluation *rows;
    HWAProductionEvaluation derived;
    size_t metric_count = hwa_production_metric_catalog_count();
    size_t metric_offset;
    char error[HWA_ERROR_SIZE] = {0};
    memset(&result, 0, sizeof(result));
    memset(sources, 0, sizeof(sources));
    memset(&span, 0, sizeof(span));
    rows = (HWAProductionEvaluation *)calloc(
        metric_count * ((size_t)HWA_PRODUCTION_VIEW_COUNT - 1U),
        sizeof(*rows));
    CHECK(rows != NULL, "cannot allocate unbracketed-band fixture");
    if (rows == NULL) return;
    for (metric_offset = 0U; metric_offset < metric_count; ++metric_offset) {
        HWAProductionMetricKind kind;
        uint32_t index;
        HWAProductionUnit unit;
        if (hwa_production_metric_catalog_at(
                metric_offset, &kind, &index, &unit) == 0 &&
            kind == HWA_PRODUCTION_METRIC_BAND_LEVEL_DBFS && index == 0U) {
            break;
        }
    }
    CHECK(metric_offset < metric_count,
          "band-0 metric is absent from the method catalog");
    result.sources = sources;
    result.source_count = 4U;
    result.spans = &span;
    result.span_count = 1U;
    result.evaluations = rows;
    result.evaluation_row_count =
        metric_count * ((size_t)HWA_PRODUCTION_VIEW_COUNT - 1U);
    sources[1].format.sample_rate_hz = 48000U;
    sources[3].format.sample_rate_hz = 48000U;
    span.id = 1U;
    span.split = HWA_PRODUCTION_CHECK;
    span.eligibility_flags =
        HWA_PRODUCTION_SPAN_EQ | HWA_PRODUCTION_SPAN_DYNAMICS;
    rows[metric_offset].availability = HWA_PRODUCTION_AVAILABLE;
    rows[metric_offset].reference_value = -24.0;
    rows[metric_offset].model_value = -25.0;
    rows[metric_offset].reference_valid = 1;
    rows[metric_offset].model_valid = 1;
    rows[metric_offset].delta_valid = 1;
    CHECK(hwa_production_evaluation_derive(
              &result, 0U, HWA_PRODUCTION_VIEW_DRY_LIKE,
              metric_offset, &derived, error, sizeof(error)) == 0 &&
              derived.availability == HWA_PRODUCTION_UNAVAILABLE,
          "unbracketed CHECK band became %d instead of unavailable: %s",
          (int)derived.availability, error);
    rows[metric_offset].availability = HWA_PRODUCTION_INSUFFICIENT;
    memset(&derived, 0, sizeof(derived));
    CHECK(hwa_production_evaluation_derive(
              &result, 0U, HWA_PRODUCTION_VIEW_DRY_LIKE,
              metric_offset, &derived, error, sizeof(error)) == 0 &&
              derived.availability == HWA_PRODUCTION_UNAVAILABLE,
          "missing raw evidence overrode unbracketed band support: %d",
          (int)derived.availability);
    result.source_count = 5U;
    sources[4].format.sample_rate_hz = 48000U;
    memset(&derived, 0, sizeof(derived));
    CHECK(hwa_production_evaluation_derive(
              &result, 0U, HWA_PRODUCTION_VIEW_ROOM_MATCHED,
              metric_offset, &derived, error, sizeof(error)) == 0 &&
              derived.availability == HWA_PRODUCTION_UNAVAILABLE,
          "unusable IR overrode unbracketed band support: %d",
          (int)derived.availability);
    free(rows);
}

int main(void)
{
    TestFiles files;
    test_method_helpers();
    test_unbracketed_check_band_is_unavailable();
    test_low_confidence_and_floor_gates();
    test_family_sampling_and_order_invariance();
    test_dynamics_and_stereo();
    test_decay_quantiles_and_low_rate_ir();
    test_huge_finite_float_rejected();
    test_changed_audio_rejected();
    CHECK(test_make_files(&files), "cannot make Stage 6 public fixture");
    if (failures == 0) {
        test_public_account(&files);
        test_self_account_and_ir(&files);
        test_caps_and_free(&files);
    }
    test_files_close(&files);
    if (failures != 0) {
        (void)fprintf(stderr, "%d production test(s) failed\n", failures);
        return 1;
    }
    (void)puts("Production tests passed");
    return 0;
}

#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "hlolli_wg_analyzer.h"
#include "numeric_locale.h"
#include "production.h"
#include "production_file.h"

#include <inttypes.h>
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
    memset(source, 0, sizeof(*source));
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

static char *test_key_for_split(HWAProductionSplit wanted)
{
    unsigned candidate;
    for (candidate = 0U; candidate < 10000U; ++candidate) {
        char text[32];
        HWAProductionSplit actual;
        int length = snprintf(
            text, sizeof(text), "event:%04u", candidate);
        if (length < 0 || (size_t)length >= sizeof(text)) return NULL;
        if (hwa_production_split_for_item_key(text, &actual) == 0 &&
            actual == wanted) return test_copy(text);
    }
    return NULL;
}

static char *test_next_key_for_split(HWAProductionSplit wanted,
                                     unsigned *candidate)
{
    while (*candidate < 100000U) {
        char text[32];
        HWAProductionSplit actual;
        int length = snprintf(
            text, sizeof(text), "tolerance:%05u", *candidate);
        (*candidate)++;
        if (length < 0 || (size_t)length >= sizeof(text)) return NULL;
        if (hwa_production_split_for_item_key(text, &actual) == 0 &&
            actual == wanted) return test_copy(text);
    }
    return NULL;
}

static int test_make_result(HWAProductionResult *result)
{
    size_t metric_count = hwa_production_metric_catalog_count();
    size_t per_span =
        ((size_t)HWA_PRODUCTION_VIEW_COUNT - 1U) * metric_count;
    size_t span_index;
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
    for (span_index = 0U; span_index < result->span_count; ++span_index) {
        HWAProductionSpan *span = &result->spans[span_index];
        span->id = (uint64_t)span_index + 1U;
        span->split = span_index == 0U ?
            HWA_PRODUCTION_TRAIN : HWA_PRODUCTION_CHECK;
        span->item_key = test_key_for_split(span->split);
        span->item_role = test_copy(
            span_index == 0U ? "tail,\n\"quoted\"" : "tail");
        span->item_kind = HWA_ITEM_RELEASE;
        span->reference_item_id = (uint64_t)span_index + 1U;
        span->reference_end_sample = 2400U;
        span->model_item_id = (uint64_t)span_index + 101U;
        span->model_end_sample = 2400U;
        span->eligibility_flags = HWA_PRODUCTION_SPAN_DECAY;
        if (span->item_key == NULL || span->item_role == NULL) return 0;
    }
    for (span_index = 0U; span_index < result->fit_count; ++span_index) {
        HWAProductionFit *fit = &result->fits[span_index];
        fit->id = (uint64_t)span_index + 1U;
        if (hwa_production_fit_catalog_at(
                span_index, &fit->scope, &fit->kind,
                &fit->index, &fit->unit) != 0) return 0;
        fit->availability = HWA_PRODUCTION_UNAVAILABLE;
        if (fit->scope != HWA_PRODUCTION_SCOPE_ROOM_IR &&
            hwa_production_fit_eligibility_flag(fit->kind) ==
                HWA_PRODUCTION_SPAN_DECAY) fit->span_count = 1U;
    }
    if (!test_make_warnings(result)) return 0;
    for (span_index = 0U; span_index < result->span_count; ++span_index) {
        HWAProductionView view;
        for (view = HWA_PRODUCTION_VIEW_RAW;
             view < HWA_PRODUCTION_VIEW_COUNT;
             view = (HWAProductionView)((int)view + 1)) {
            size_t metric;
            for (metric = 0U; metric < metric_count; ++metric) {
                size_t offset = span_index * per_span +
                    ((size_t)view - 1U) * metric_count + metric;
                HWAProductionEvaluation *row =
                    &result->evaluations[offset];
                if (view == HWA_PRODUCTION_VIEW_RAW) {
                    row->id = (uint64_t)offset + 1U;
                    row->span_id = (uint64_t)span_index + 1U;
                    row->view = view;
                    if (hwa_production_metric_catalog_at(
                            metric, &row->kind, &row->index,
                            &row->unit) != 0) return 0;
                    row->availability = HWA_PRODUCTION_UNAVAILABLE;
                    row->evidence_flags =
                        span_index == 1U ?
                            HWA_PRODUCTION_EVIDENCE_HELD_OUT : 0U;
                } else if (hwa_production_evaluation_derive(
                               result, span_index, view, metric,
                               row, NULL, 0U) != 0) {
                    return 0;
                }
            }
        }
    }
    if (hwa_production_result_retained_bytes(
            result, &result->retained_work_bytes) != 0 ||
        hwa_production_view_rows_rebuild(
            result, NULL, 0U) != 0) return 0;
    return 1;
}

static int test_derive_corrected(HWAProductionResult *result,
                                 char *error,
                                 size_t error_size)
{
    size_t metric_count = hwa_production_metric_catalog_count();
    size_t span_index;
    for (span_index = 0U; span_index < result->span_count; ++span_index) {
        HWAProductionView view;
        for (view = HWA_PRODUCTION_VIEW_DRY_LIKE;
             view < HWA_PRODUCTION_VIEW_COUNT;
             view = (HWAProductionView)((int)view + 1)) {
            size_t metric;
            for (metric = 0U; metric < metric_count; ++metric) {
                size_t offset =
                    span_index *
                        ((size_t)HWA_PRODUCTION_VIEW_COUNT - 1U) *
                        metric_count +
                    ((size_t)view - 1U) * metric_count + metric;
                if (hwa_production_evaluation_derive(
                        result, span_index, view, metric,
                        &result->evaluations[offset],
                        error, error_size) != 0) return 0;
            }
        }
    }
    return hwa_production_view_rows_rebuild(
               result, error, error_size) == 0;
}

static int test_make_tolerance_result(HWAProductionResult *result)
{
    const size_t train_count = 8U;
    const size_t check_count = 2U;
    size_t metric_count = hwa_production_metric_catalog_count();
    size_t per_span =
        ((size_t)HWA_PRODUCTION_VIEW_COUNT - 1U) * metric_count;
    size_t balance_metric = 3U + HWA_BAND_COUNT;
    unsigned train_candidate = 0U;
    unsigned check_candidate = 0U;
    size_t span_index;
    char error[HWA_ERROR_SIZE] = {0};
    memset(result, 0, sizeof(*result));
    hwa_production_options_default(&result->options);
    result->profile_method.fft_size = 8192U;
    result->profile_method.hop_size = 2048U;
    result->profile_method.pitch_confidence_floor = 0.8;
    result->profile_method.spectral_floor_dbfs = -100.0;
    result->profile_method.max_partials = 16U;
    result->source_count = 4U;
    result->span_count = train_count + check_count;
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
                     "/tolerance-reference.hwa", '1', 0) ||
        !test_source(&result->sources[1], 2U, "reference:audio",
                     "/tolerance-reference.wav", '2', 1) ||
        !test_source(&result->sources[2], 3U, "model:profile",
                     "/tolerance-model.hwa", '3', 0) ||
        !test_source(&result->sources[3], 4U, "model:audio",
                     "/tolerance-model.wav", '4', 1)) return 0;
    for (span_index = 0U; span_index < result->span_count; ++span_index) {
        HWAProductionSpan *span = &result->spans[span_index];
        int train = span_index < train_count;
        span->id = (uint64_t)span_index + 1U;
        span->split = train ? HWA_PRODUCTION_TRAIN :
                              HWA_PRODUCTION_CHECK;
        span->item_key = test_next_key_for_split(
            span->split, train ? &train_candidate : &check_candidate);
        span->item_role = test_copy("note");
        span->item_kind = HWA_ITEM_NOTE;
        span->reference_item_id = (uint64_t)span_index + 1U;
        span->reference_end_sample = 48000U;
        span->model_item_id = (uint64_t)span_index + 101U;
        span->model_end_sample = 48000U;
        span->eligibility_flags = HWA_PRODUCTION_SPAN_STEREO;
        if (span->item_key == NULL || span->item_role == NULL) return 0;
    }
    for (span_index = 0U; span_index < result->fit_count; ++span_index) {
        HWAProductionFit *fit = &result->fits[span_index];
        fit->id = (uint64_t)span_index + 1U;
        if (hwa_production_fit_catalog_at(
                span_index, &fit->scope, &fit->kind,
                &fit->index, &fit->unit) != 0) return 0;
        fit->availability = HWA_PRODUCTION_UNAVAILABLE;
        if (fit->scope != HWA_PRODUCTION_SCOPE_ROOM_IR &&
            hwa_production_fit_eligibility_flag(fit->kind) ==
                HWA_PRODUCTION_SPAN_STEREO) {
            fit->span_count = train_count;
        }
        if (fit->scope == HWA_PRODUCTION_SCOPE_CORRECTION &&
            fit->kind == HWA_PRODUCTION_FIT_CHANNEL_BALANCE_DB) {
            fit->availability = HWA_PRODUCTION_AVAILABLE;
            fit->estimate = 1.0;
            fit->q05 = 0.5;
            fit->q95 = 1.5;
            fit->point_count = train_count;
            fit->estimate_valid = 1;
            fit->uncertainty_valid = 1;
        }
    }
    if (!test_make_warnings(result)) return 0;
    for (span_index = 0U; span_index < result->span_count; ++span_index) {
        size_t metric;
        for (metric = 0U; metric < metric_count; ++metric) {
            size_t offset = span_index * per_span + metric;
            HWAProductionEvaluation *row = &result->evaluations[offset];
            row->id = (uint64_t)offset + 1U;
            row->span_id = (uint64_t)span_index + 1U;
            row->view = HWA_PRODUCTION_VIEW_RAW;
            if (hwa_production_metric_catalog_at(
                    metric, &row->kind, &row->index,
                    &row->unit) != 0) return 0;
            row->availability = HWA_PRODUCTION_UNAVAILABLE;
            if (result->spans[span_index].split ==
                HWA_PRODUCTION_CHECK) {
                row->evidence_flags =
                    HWA_PRODUCTION_EVIDENCE_HELD_OUT;
            }
            if (metric == balance_metric) {
                double reference = (double)span_index * 0.25;
                row->availability = HWA_PRODUCTION_AVAILABLE;
                row->reference_value = reference;
                row->model_value = reference + 3.0;
                row->delta = 3.0;
                row->confidence = 0.9;
                row->evidence_flags |=
                    HWA_PRODUCTION_EVIDENCE_SAMPLES |
                    HWA_PRODUCTION_EVIDENCE_STEREO;
                row->reference_valid = 1;
                row->model_valid = 1;
                row->delta_valid = 1;
            }
        }
    }
    if (!test_derive_corrected(result, error, sizeof(error)) ||
        hwa_production_result_validate(
            result, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "tolerance fixture: %s\n", error);
        return 0;
    }
    return 1;
}

static int test_file_equal(const char *left, const char *right)
{
    FILE *a = fopen(left, "rb");
    FILE *b = fopen(right, "rb");
    int equal = a != NULL && b != NULL;
    while (equal) {
        int x = fgetc(a);
        int y = fgetc(b);
        if (x != y) equal = 0;
        if (x == EOF || y == EOF) {
            if (x != EOF || y != EOF ||
                ferror(a) || ferror(b)) equal = 0;
            break;
        }
    }
    if (a != NULL) (void)fclose(a);
    if (b != NULL) (void)fclose(b);
    return equal;
}

static int test_change_pointer_bits(const char *path)
{
    static const char marker[] = "META,build_pointer_bits,";
    FILE *stream = fopen(path, "r+b");
    size_t matched = 0U;
    int byte;
    if (stream == NULL) return 0;
    while ((byte = fgetc(stream)) != EOF) {
        if ((unsigned char)byte == (unsigned char)marker[matched]) {
            matched++;
            if (matched == sizeof(marker) - 1U) {
                long position = ftell(stream);
                int digit = fgetc(stream);
                unsigned char replacement;
                if (position < 0 || digit < '1' || digit > '9') break;
                replacement = digit == '9' ? (unsigned char)'8' :
                                             (unsigned char)(digit + 1);
                if (fseek(stream, position, SEEK_SET) != 0 ||
                    fwrite(&replacement, 1U, 1U, stream) != 1U) break;
                return fclose(stream) == 0;
            }
        } else {
            matched = (unsigned char)byte == (unsigned char)marker[0] ?
                          1U : 0U;
        }
    }
    (void)fclose(stream);
    return 0;
}

static int test_replace_text(const char *path,
                             const char *needle,
                             const char *replacement)
{
    FILE *stream = fopen(path, "rb");
    unsigned char *input = NULL;
    unsigned char *output = NULL;
    char *found;
    long end;
    size_t input_size;
    size_t prefix;
    size_t needle_size = strlen(needle);
    size_t replacement_size = strlen(replacement);
    size_t output_size;
    int okay = 0;
    if (stream == NULL || fseek(stream, 0L, SEEK_END) != 0 ||
        (end = ftell(stream)) < 0 || fseek(stream, 0L, SEEK_SET) != 0) {
        if (stream != NULL) (void)fclose(stream);
        return 0;
    }
    input_size = (size_t)end;
    input = (unsigned char *)malloc(input_size + 1U);
    if (input == NULL ||
        fread(input, 1U, input_size, stream) != input_size ||
        fclose(stream) != 0) {
        free(input);
        return 0;
    }
    input[input_size] = 0U;
    found = strstr((char *)input, needle);
    if (found == NULL || replacement_size > SIZE_MAX - input_size ||
        needle_size > input_size ||
        replacement_size + input_size - needle_size == SIZE_MAX) {
        free(input);
        return 0;
    }
    prefix = (size_t)(found - (char *)input);
    output_size = input_size - needle_size + replacement_size;
    output = (unsigned char *)malloc(output_size);
    if (output == NULL) {
        free(input);
        return 0;
    }
    memcpy(output, input, prefix);
    memcpy(output + prefix, replacement, replacement_size);
    memcpy(output + prefix + replacement_size,
           input + prefix + needle_size,
           input_size - prefix - needle_size);
    stream = fopen(path, "wb");
    if (stream != NULL) {
        okay = fwrite(output, 1U, output_size, stream) == output_size &&
                fclose(stream) == 0;
    }
    free(output);
    free(input);
    return okay;
}

static int test_drift_numeric_field(const char *path,
                                    const char *section,
                                    uint64_t id,
                                    size_t field_index,
                                    unsigned ulps)
{
    FILE *stream = fopen(path, "rb");
    unsigned char *input = NULL;
    unsigned char *output = NULL;
    char prefix[64];
    char number[64];
    char replacement[64];
    char *row;
    char *field;
    char *end;
    char *parsed_end;
    long file_end;
    size_t input_size;
    size_t old_size;
    size_t new_size;
    size_t output_size;
    size_t before_size;
    size_t index;
    double value;
    HWANumericLocale locale;
    int locale_active = 0;
    int okay = 0;
    int prefix_size = snprintf(
        prefix, sizeof(prefix), "%s,%" PRIu64 ",", section, id);
    if (prefix_size < 0 || (size_t)prefix_size >= sizeof(prefix) ||
        stream == NULL || fseek(stream, 0L, SEEK_END) != 0 ||
        (file_end = ftell(stream)) < 0 ||
        fseek(stream, 0L, SEEK_SET) != 0) {
        if (stream != NULL) (void)fclose(stream);
        return 0;
    }
    input_size = (size_t)file_end;
    input = (unsigned char *)malloc(input_size + 1U);
    if (input == NULL ||
        fread(input, 1U, input_size, stream) != input_size ||
        fclose(stream) != 0) {
        free(input);
        return 0;
    }
    input[input_size] = 0U;
    row = strstr((char *)input, prefix);
    if (row == NULL ||
        (row != (char *)input && row[-1] != '\n')) goto cleanup;
    field = row;
    for (index = 0U; index < field_index; ++index) {
        field = strchr(field, ',');
        if (field == NULL) goto cleanup;
        field++;
    }
    end = strchr(field, ',');
    if (end == NULL || end <= field ||
        (size_t)(end - field) >= sizeof(number)) goto cleanup;
    old_size = (size_t)(end - field);
    memcpy(number, field, old_size);
    number[old_size] = '\0';
    if (hwa_c_numeric_locale_begin(&locale) != 0) goto cleanup;
    locale_active = 1;
    value = strtod(number, &parsed_end);
    if (parsed_end == number || *parsed_end != '\0' || !isfinite(value)) {
        goto cleanup;
    }
    for (index = 0U; index < (size_t)ulps; ++index) {
        value = nextafter(value, INFINITY);
    }
    {
        int length = snprintf(
            replacement, sizeof(replacement), "%.17g",
            value == 0.0 ? 0.0 : value);
        if (length < 0 || (size_t)length >= sizeof(replacement)) {
            goto cleanup;
        }
        new_size = (size_t)length;
    }
    locale_active = 0;
    if (hwa_c_numeric_locale_end(&locale) != 0) goto cleanup;
    if (new_size > SIZE_MAX - input_size ||
        old_size > input_size) goto cleanup;
    output_size = input_size - old_size + new_size;
    output = (unsigned char *)malloc(output_size);
    if (output == NULL) goto cleanup;
    before_size = (size_t)(field - (char *)input);
    memcpy(output, input, before_size);
    memcpy(output + before_size, replacement, new_size);
    memcpy(output + before_size + new_size, end,
           input_size - before_size - old_size);
    stream = fopen(path, "wb");
    if (stream != NULL) {
        int write_ok = fwrite(output, 1U, output_size, stream) == output_size;
        int close_ok = fclose(stream) == 0;
        okay = write_ok && close_ok;
    }

cleanup:
    if (locale_active) (void)hwa_c_numeric_locale_end(&locale);
    free(output);
    free(input);
    return okay;
}

static int test_truncate_final_crlf(const char *path)
{
    FILE *stream = fopen(path, "rb");
    unsigned char *bytes;
    long end;
    size_t size;
    int okay = 0;
    if (stream == NULL || fseek(stream, 0L, SEEK_END) != 0 ||
        (end = ftell(stream)) < 2L || fseek(stream, 0L, SEEK_SET) != 0) {
        if (stream != NULL) (void)fclose(stream);
        return 0;
    }
    size = (size_t)end;
    bytes = (unsigned char *)malloc(size);
    if (bytes == NULL || fread(bytes, 1U, size, stream) != size ||
        fclose(stream) != 0) {
        free(bytes);
        return 0;
    }
    if (bytes[size - 2U] == (unsigned char)'\r' &&
        bytes[size - 1U] == (unsigned char)'\n') {
        stream = fopen(path, "wb");
        if (stream != NULL) {
            okay = fwrite(bytes, 1U, size - 2U, stream) == size - 2U &&
                    fclose(stream) == 0;
        }
    }
    free(bytes);
    return okay;
}

static int test_write_result(const char *path,
                             const HWAProductionResult *result)
{
    FILE *stream = fopen(path, "wb");
    char error[HWA_ERROR_SIZE] = {0};
    int status;
    if (stream == NULL) return 0;
    status = hwa_production_file_write(
        stream, result, error, sizeof(error));
    if (fclose(stream) != 0) status = -1;
    return status == 0;
}

static void test_round_trip(const char *first, const char *second)
{
    HWAProductionResult original;
    HWAProductionResult read_back;
    char sha[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    HWAProductionOptions read_limits;
    FILE *stream;
    memset(&read_back, 0, sizeof(read_back));
    CHECK(test_make_result(&original), "make canonical fixture");
    CHECK(original.view_rows[0].reference_statistics.total_count == 0U &&
              original.view_rows[7U + HWA_BAND_COUNT]
                      .reference_statistics.total_count == 1U,
          "VIEW totals use only the metric-applicable span cohort");
    CHECK(hwa_production_result_validate(
              &original, error, sizeof(error)) == 0,
          "fixture validates: %s", error);
    stream = fopen(first, "wb");
    CHECK(stream != NULL, "open first output");
    if (stream != NULL) {
        CHECK(hwa_production_file_write(
                  stream, &original, error, sizeof(error)) == 0,
              "write fixture: %s", error);
        CHECK(fclose(stream) == 0, "close first output");
    }
    read_limits = original.options;
    read_limits.profile_limits.max_input_bytes = 1U;
    CHECK(hwa_production_file_read(
              first, &read_limits, &read_back,
              sha, error, sizeof(error)) == 0,
          "read fixture under distinct profile byte cap: %s", error);
    hwa_production_result_free(&read_back);
    memset(&read_back, 0, sizeof(read_back));
    CHECK(hwa_production_file_read(
              first, &original.options, &read_back,
              sha, error, sizeof(error)) == 0,
          "read fixture under matching current limits: %s", error);
    CHECK(strlen(sha) == HWA_SHA256_HEX_SIZE - 1U,
          "reader returns file hash");
    CHECK(read_back.source_count == 4U &&
          strcmp(read_back.sources[1].path,
                 original.sources[1].path) == 0,
          "reader preserves path bytes");
    stream = fopen(second, "wb");
    CHECK(stream != NULL, "open normalized output");
    if (stream != NULL) {
        CHECK(hwa_production_file_write(
                  stream, &read_back, error, sizeof(error)) == 0,
              "write normalized result: %s", error);
        CHECK(fclose(stream) == 0, "close normalized output");
    }
    CHECK(test_file_equal(first, second), "canonical byte round trip");
    hwa_production_result_free(&read_back);
    CHECK(test_change_pointer_bits(first),
          "change saved producer pointer width");
    memset(&read_back, 0, sizeof(read_back));
    CHECK(hwa_production_file_read(
              first, &original.options, &read_back,
              sha, error, sizeof(error)) == 0,
          "read cross-build provenance: %s", error);
    stream = fopen(first, "wb");
    CHECK(stream != NULL, "open cross-build rewrite");
    if (stream != NULL) {
        CHECK(hwa_production_file_write(
                  stream, &read_back, error, sizeof(error)) == 0,
              "normalize cross-build provenance: %s", error);
        CHECK(fclose(stream) == 0, "close cross-build rewrite");
    }
    CHECK(test_file_equal(first, second),
          "reader normalizes producer build provenance");
    hwa_production_result_free(&read_back);
    hwa_production_result_free(&original);
}

static void test_rejections(const char *path)
{
    HWAProductionResult result;
    HWAProductionResult read_back;
    HWAProductionOptions limits;
    char sha[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    FILE *stream;
    size_t metric_count = hwa_production_metric_catalog_count();
    HWAProductionEvaluation saved_evaluation;
    HWAProductionFit saved_room_fit;
    HWAProductionFit *room_fit = NULL;
    HWAProductionSource *expanded_sources;
    size_t fit_index;
    size_t view_offset;
    int unsupported_totals_zero;
    size_t room_metric = 7U + HWA_BAND_COUNT +
                         HWA_PRODUCTION_ROOM_BAND_COUNT - 1U;
    CHECK(test_make_result(&result), "make rejection fixture");
    stream = tmpfile();
    CHECK(stream != NULL, "open validation stream");
    if (stream == NULL) {
        hwa_production_result_free(&result);
        return;
    }
    saved_evaluation = result.evaluations[room_metric];
    result.evaluations[room_metric].availability =
        HWA_PRODUCTION_AVAILABLE;
    result.evaluations[room_metric].evidence_flags =
        HWA_PRODUCTION_EVIDENCE_SAMPLES |
        HWA_PRODUCTION_EVIDENCE_ENVELOPE |
        HWA_PRODUCTION_EVIDENCE_SPECTRUM;
    result.evaluations[room_metric].reference_value = -30.0;
    result.evaluations[room_metric].model_value = -33.0;
    result.evaluations[room_metric].delta = -3.0;
    result.evaluations[room_metric].confidence = 0.9;
    result.evaluations[room_metric].reference_valid = 1;
    result.evaluations[room_metric].model_valid = 1;
    result.evaluations[room_metric].delta_valid = 1;
    CHECK(hwa_production_evaluation_derive(
              &result, 0U, HWA_PRODUCTION_VIEW_DRY_LIKE,
              room_metric,
              &result.evaluations[metric_count + room_metric],
              error, sizeof(error)) == 0 &&
              hwa_production_evaluation_derive(
                  &result, 0U, HWA_PRODUCTION_VIEW_ROOM_MATCHED,
                  room_metric,
                  &result.evaluations[2U * metric_count + room_metric],
                  error, sizeof(error)) == 0 &&
              hwa_production_view_rows_rebuild(
                  &result, error, sizeof(error)) == 0 &&
              hwa_production_result_validate(
                  &result, error, sizeof(error)) == 0,
          "make supported high room-band evaluation: %s", error);
    result.evaluations[room_metric].evidence_flags =
        HWA_PRODUCTION_EVIDENCE_PROFILE;
    CHECK(hwa_production_file_write(
              stream, &result, error, sizeof(error)) != 0,
          "reject forged RAW evidence authority");
    result.evaluations[room_metric].evidence_flags =
        HWA_PRODUCTION_EVIDENCE_SAMPLES |
        HWA_PRODUCTION_EVIDENCE_ENVELOPE |
        HWA_PRODUCTION_EVIDENCE_SPECTRUM;
    result.evaluations[room_metric].quality_flags |=
        HWA_PRODUCTION_QUALITY_CORRECTION_INCOMPLETE;
    CHECK(hwa_production_file_write(
              stream, &result, error, sizeof(error)) != 0,
          "reject correction-only quality on RAW evidence");
    result.evaluations[room_metric].quality_flags &=
        (uint32_t)~(uint32_t)
            HWA_PRODUCTION_QUALITY_CORRECTION_INCOMPLETE;
    result.sources[1].format.sample_rate_hz = 16000U;
    result.sources[1].format.duration_seconds = 3.0;
    result.sources[3].format.sample_rate_hz = 16000U;
    result.sources[3].format.duration_seconds = 3.0;
    CHECK(hwa_production_file_write(
              stream, &result, error, sizeof(error)) != 0,
          "reject available room metric above the saved source rate");
    result.sources[1].format.sample_rate_hz = 48000U;
    result.sources[1].format.duration_seconds = 1.0;
    result.sources[3].format.sample_rate_hz = 48000U;
    result.sources[3].format.duration_seconds = 1.0;
    result.evaluations[room_metric] = saved_evaluation;
    CHECK(hwa_production_evaluation_derive(
              &result, 0U, HWA_PRODUCTION_VIEW_DRY_LIKE,
              room_metric,
              &result.evaluations[metric_count + room_metric],
              error, sizeof(error)) == 0 &&
              hwa_production_evaluation_derive(
                  &result, 0U, HWA_PRODUCTION_VIEW_ROOM_MATCHED,
                  room_metric,
                  &result.evaluations[2U * metric_count + room_metric],
                  error, sizeof(error)) == 0 &&
              hwa_production_view_rows_rebuild(
                  &result, error, sizeof(error)) == 0,
          "restore room-band fixture: %s", error);
    result.sources[1].format.sample_rate_hz = 16000U;
    result.sources[1].format.duration_seconds = 3.0;
    result.sources[3].format.sample_rate_hz = 16000U;
    result.sources[3].format.duration_seconds = 3.0;
    unsupported_totals_zero =
        test_derive_corrected(&result, error, sizeof(error));
    for (view_offset = room_metric;
         unsupported_totals_zero &&
         view_offset < result.view_row_count;
         view_offset += metric_count) {
        unsupported_totals_zero =
            result.view_rows[view_offset]
                    .reference_statistics.total_count == 0U &&
            result.view_rows[view_offset]
                    .model_statistics.total_count == 0U;
    }
    CHECK(unsupported_totals_zero &&
              hwa_production_result_validate(
                  &result, error, sizeof(error)) == 0,
          "unsupported room bands have zero VIEW population: %s", error);
    result.sources[1].format.sample_rate_hz = 48000U;
    result.sources[1].format.duration_seconds = 1.0;
    result.sources[3].format.sample_rate_hz = 48000U;
    result.sources[3].format.duration_seconds = 1.0;
    CHECK(test_derive_corrected(&result, error, sizeof(error)),
          "restore supported room-band population: %s", error);
    expanded_sources = (HWAProductionSource *)realloc(
        result.sources, 5U * sizeof(*result.sources));
    CHECK(expanded_sources != NULL,
          "grow source fixture for low-rate room IR");
    if (expanded_sources != NULL) {
        result.sources = expanded_sources;
        memset(&result.sources[4], 0, sizeof(result.sources[4]));
        result.source_count = 5U;
        CHECK(test_source(
                  &result.sources[4], 5U, "room-ir",
                  "/room.wav", 'e', 1),
              "make low-rate room IR source");
        for (fit_index = 0U; fit_index < result.fit_count; ++fit_index) {
            HWAProductionFit *candidate = &result.fits[fit_index];
            if (candidate->scope == HWA_PRODUCTION_SCOPE_ROOM_IR &&
                candidate->kind ==
                    HWA_PRODUCTION_FIT_EARLY_REFLECTION_DB &&
                candidate->index ==
                    HWA_PRODUCTION_ROOM_BAND_COUNT - 1U) {
                room_fit = candidate;
                break;
            }
        }
        CHECK(room_fit != NULL, "find highest room-IR band fit");
        if (room_fit != NULL) {
            saved_room_fit = *room_fit;
            room_fit->availability = HWA_PRODUCTION_AVAILABLE;
            room_fit->estimate = -20.0;
            room_fit->estimate_valid = 1;
            room_fit->point_count = 1U;
            room_fit->quality_flags =
                HWA_PRODUCTION_QUALITY_IR_SUPPLIED;
            CHECK(test_derive_corrected(
                      &result, error, sizeof(error)) &&
                      hwa_production_result_validate(
                          &result, error, sizeof(error)) == 0,
                  "make supported explicit room fit: %s", error);
            result.sources[4].format.sample_rate_hz = 16000U;
            result.sources[4].format.duration_seconds = 3.0;
            CHECK(hwa_production_file_write(
                      stream, &result, error, sizeof(error)) != 0,
                  "reject available room-IR fit above the IR rate");
            result.sources[4].format.sample_rate_hz = 48000U;
            result.sources[4].format.duration_seconds = 1.0;
            *room_fit = saved_room_fit;
        }
        free(result.sources[4].role);
        free(result.sources[4].path);
        memset(&result.sources[4], 0, sizeof(result.sources[4]));
        result.source_count = 4U;
        CHECK(test_derive_corrected(&result, error, sizeof(error)),
              "restore no-IR corrected fixture: %s", error);
    }
    result.sources[1].format.data_bytes++;
    CHECK(hwa_production_file_write(
              stream, &result, error, sizeof(error)) != 0,
          "reject inconsistent WAVE bytes");
    result.sources[1].format.data_bytes--;
    result.sources[3].format.sample_rate_hz = 44100U;
    CHECK(hwa_production_file_write(
              stream, &result, error, sizeof(error)) != 0,
          "reject unequal paired sample rates");
    result.sources[3].format.sample_rate_hz = 48000U;
    result.evaluations[0].reference_value = 1.0;
    CHECK(hwa_production_file_write(
              stream, &result, error, sizeof(error)) != 0,
          "reject nonzero unavailable evaluation");
    result.evaluations[0].reference_value = 0.0;
    saved_evaluation = result.evaluations[metric_count];
    result.evaluations[metric_count].availability =
        HWA_PRODUCTION_AVAILABLE;
    result.evaluations[metric_count].reference_valid = 1;
    result.evaluations[metric_count].model_valid = 1;
    result.evaluations[metric_count].delta_valid = 1;
    result.evaluations[metric_count].confidence = 1.0;
    CHECK(hwa_production_file_write(
              stream, &result, error, sizeof(error)) != 0,
          "reject forged corrected evaluation");
    result.evaluations[metric_count] = saved_evaluation;
    result.spans[0].split = HWA_PRODUCTION_CHECK;
    CHECK(hwa_production_file_write(
              stream, &result, error, sizeof(error)) != 0,
          "reject saved split relabel");
    result.spans[0].split = HWA_PRODUCTION_TRAIN;
    result.spans[0].eligibility_flags |= HWA_PRODUCTION_SPAN_DYNAMICS;
    CHECK(hwa_production_file_write(
              stream, &result, error, sizeof(error)) != 0,
          "reject wrong kind or duration for a span family");
    result.spans[0].eligibility_flags = HWA_PRODUCTION_SPAN_DECAY;
    result.sources[3].format.channels = 4U;
    result.sources[3].format.block_align = 8U;
    result.sources[3].format.data_bytes =
        result.sources[3].format.frames * 8U;
    CHECK(hwa_production_result_validate(
              &result, error, sizeof(error)) == 0,
          "allow two-channel versus four-channel sources: %s", error);
    result.sources[3].format.channels = 2U;
    result.sources[3].format.block_align = 4U;
    result.sources[3].format.data_bytes =
        result.sources[3].format.frames * 4U;
    result.options.max_fits = result.fit_count - 1U;
    CHECK(hwa_production_file_write(
              stream, &result, error, sizeof(error)) != 0,
          "reject saved row cap");
    result.options.max_fits++;
    CHECK(hwa_production_file_write(
              NULL, &result, error, sizeof(error)) != 0,
          "reject null output stream");
    CHECK(fclose(stream) == 0, "close validation stream");
    stream = fopen(path, "wb");
    CHECK(stream != NULL, "open reader cap fixture");
    if (stream != NULL) {
        CHECK(hwa_production_file_write(
                  stream, &result, error, sizeof(error)) == 0,
              "write reader cap fixture");
        (void)fclose(stream);
    }
    memset(&read_back, 0, sizeof(read_back));
    CHECK(test_replace_text(
              path,
              "META,profile_fft_size,8192,samples\r\n",
              "META,profile_fft_size,08192,samples\r\n") &&
              hwa_production_file_read(
                  path, &result.options, &read_back, sha,
                  error, sizeof(error)) != 0,
          "reject noncanonical integer spelling");
    CHECK(test_write_result(path, &result),
          "restore after integer spelling test");
    memset(&read_back, 0, sizeof(read_back));
    CHECK(test_replace_text(
              path,
              "META,profile_pitch_confidence_floor,"
              "0.80000000000000004,ratio\r\n",
              "META,profile_pitch_confidence_floor,8e-1,ratio\r\n") &&
              hwa_production_file_read(
                  path, &result.options, &read_back, sha,
                  error, sizeof(error)) != 0,
          "reject noncanonical double spelling");
    CHECK(test_write_result(path, &result),
          "restore after double spelling test");
    memset(&read_back, 0, sizeof(read_back));
    CHECK(test_replace_text(
              path,
              "META,production_method_version,stage6-1,\r\n",
              "META,production_method_version,\"stage6-1\",\r\n") &&
              hwa_production_file_read(
                  path, &result.options, &read_back, sha,
                  error, sizeof(error)) != 0,
          "reject needless CSV quoting");
    CHECK(test_write_result(path, &result),
          "restore after quoting test");
    memset(&read_back, 0, sizeof(read_back));
    CHECK(test_replace_text(
              path, "HWA_PRODUCTION,1\r\n", "HWA_PRODUCTION,1\n") &&
              hwa_production_file_read(
                  path, &result.options, &read_back, sha,
                  error, sizeof(error)) != 0,
          "reject a bare record line feed");
    CHECK(test_write_result(path, &result),
          "restore after line-ending test");
    memset(&read_back, 0, sizeof(read_back));
    CHECK(test_truncate_final_crlf(path) &&
              hwa_production_file_read(
                  path, &result.options, &read_back, sha,
                  error, sizeof(error)) != 0,
          "reject a final record without CRLF");
    CHECK(test_write_result(path, &result),
          "restore after final-record test");
    memset(&read_back, 0, sizeof(read_back));
    CHECK(test_replace_text(
              path, "low-eq-evidence", "bad-eq-evidence") &&
              hwa_production_file_read(
                  path, &result.options, &read_back, sha,
                  error, sizeof(error)) != 0,
          "reject a forged canonical warning");
    CHECK(test_write_result(path, &result),
          "restore after warning test");
    limits = result.options;
    limits.max_input_bytes = 1U;
    memset(&read_back, 0, sizeof(read_back));
    CHECK(hwa_production_file_read(
              path, &limits, &read_back, sha,
              error, sizeof(error)) != 0,
          "reject current production-file byte cap");
    CHECK(read_back.sources == NULL && sha[0] == '\0',
          "reader unwinds on production-file byte cap");
    limits = result.options;
    limits.max_spans = 1U;
    memset(&read_back, 0, sizeof(read_back));
    CHECK(hwa_production_file_read(
              path, &limits, &read_back, sha,
              error, sizeof(error)) != 0,
          "reject current span cap");
    CHECK(read_back.sources == NULL && sha[0] == '\0',
          "reader fully unwinds on cap failure");
    CHECK(hwa_production_file_read(
              "-", &result.options, &read_back, sha,
              error, sizeof(error)) != 0,
          "reject standard-input sentinel");
    CHECK(hwa_production_file_read(
              NULL, &result.options, &read_back, sha,
              error, sizeof(error)) != 0,
          "reject null path");
    CHECK(hwa_production_file_read(
              path, &result.options, &read_back, NULL,
              error, sizeof(error)) != 0,
          "reject null hash output");
    hwa_production_result_free(&result);
}

static void test_derived_float_tolerance(const char *drifted_path,
                                         const char *canonical_path)
{
    HWAProductionResult original;
    HWAProductionResult read_back;
    char sha[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE] = {0};
    static const unsigned accepted_ulps[] = {1U, 32U};
    const uint64_t corrected_evaluation_id = 43U;
    const uint64_t corrected_view_id = 43U;
    size_t index;
    int fixture_ok = test_make_tolerance_result(&original);
    CHECK(fixture_ok, "make derived-float tolerance fixture");
    if (!fixture_ok) {
        hwa_production_result_free(&original);
        return;
    }
    CHECK(original.evaluations[corrected_evaluation_id - 1U].view ==
              HWA_PRODUCTION_VIEW_DRY_LIKE &&
              original.evaluations[corrected_evaluation_id - 1U]
                      .availability == HWA_PRODUCTION_AVAILABLE &&
              original.view_rows[corrected_view_id - 1U].view ==
                  HWA_PRODUCTION_VIEW_DRY_LIKE &&
              original.view_rows[corrected_view_id - 1U].gap_valid,
          "tolerance targets are derived and numeric");
    CHECK(test_write_result(canonical_path, &original),
          "write float-tolerance canonical fixture");
    for (index = 0U;
         index < sizeof(accepted_ulps) / sizeof(accepted_ulps[0]);
         ++index) {
        FILE *stream;
        memset(&read_back, 0, sizeof(read_back));
        CHECK(test_write_result(drifted_path, &original),
              "restore float-tolerance drift input");
        CHECK(test_drift_numeric_field(
                  drifted_path, "EVALUATION",
                  corrected_evaluation_id, 11U,
                  accepted_ulps[index]) &&
                  test_drift_numeric_field(
                      drifted_path, "VIEW",
                      corrected_view_id, 39U,
                      accepted_ulps[index]),
              "write %u-ULP corrected EVALUATION/VIEW drift",
              accepted_ulps[index]);
        CHECK(hwa_production_file_read(
                  drifted_path, &original.options, &read_back,
                  sha, error, sizeof(error)) == 0,
              "accept %u-ULP derived drift: %s",
              accepted_ulps[index], error);
        stream = fopen(drifted_path, "wb");
        CHECK(stream != NULL, "open normalized float rewrite");
        if (stream != NULL) {
            CHECK(hwa_production_file_write(
                      stream, &read_back, error, sizeof(error)) == 0,
                  "write normalized %u-ULP result: %s",
                  accepted_ulps[index], error);
            CHECK(fclose(stream) == 0,
                  "close normalized float rewrite");
        }
        CHECK(test_file_equal(drifted_path, canonical_path),
              "%u-ULP reader rewrite is exact",
              accepted_ulps[index]);
        hwa_production_result_free(&read_back);
    }
    memset(&read_back, 0, sizeof(read_back));
    CHECK(test_write_result(drifted_path, &original) &&
              test_drift_numeric_field(
                  drifted_path, "EVALUATION",
                  corrected_evaluation_id, 11U, 33U) &&
              hwa_production_file_read(
                  drifted_path, &original.options, &read_back,
                  sha, error, sizeof(error)) != 0,
          "reject 33-ULP corrected EVALUATION drift");
    CHECK(read_back.sources == NULL && sha[0] == '\0',
          "unwind rejected corrected EVALUATION drift");
    memset(&read_back, 0, sizeof(read_back));
    CHECK(test_write_result(drifted_path, &original) &&
              test_drift_numeric_field(
                  drifted_path, "VIEW",
                  corrected_view_id, 39U, 33U) &&
              hwa_production_file_read(
                  drifted_path, &original.options, &read_back,
                  sha, error, sizeof(error)) != 0,
          "reject 33-ULP derived VIEW drift");
    CHECK(read_back.sources == NULL && sha[0] == '\0',
          "unwind rejected derived VIEW drift");
    hwa_production_result_free(&original);
}

int main(void)
{
    char first[256];
    char second[256];
    int a = snprintf(first, sizeof(first),
                     "/tmp/hwa-stage6-persistence-%ld-a.hwa",
                     (long)HWA_TEST_PID());
    int b = snprintf(second, sizeof(second),
                     "/tmp/hwa-stage6-persistence-%ld-b.hwa",
                     (long)HWA_TEST_PID());
    if (a < 0 || b < 0 || (size_t)a >= sizeof(first) ||
        (size_t)b >= sizeof(second)) return 1;
    test_round_trip(first, second);
    test_rejections(first);
    test_derived_float_tolerance(first, second);
    (void)HWA_TEST_UNLINK(first);
    (void)HWA_TEST_UNLINK(second);
    if (failures != 0) {
        (void)fprintf(stderr, "%d Stage 6 persistence tests failed\n",
                      failures);
        return 1;
    }
    (void)puts("Stage 6 persistence tests passed");
    return 0;
}

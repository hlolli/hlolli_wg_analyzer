#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "gap_report.h"
#include "gap_report_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

#define CHECK(condition, message)                                          \
    do {                                                                   \
        if (!(condition)) {                                                \
            (void)fprintf(stderr, "FAIL: %s\n", message);                \
            return 1;                                                      \
        }                                                                  \
    } while (0)

static char *copy_text(const char *text)
{
    size_t size = strlen(text) + 1U;
    char *copy = (char *)malloc(size);
    if (copy != NULL) memcpy(copy, text, size);
    return copy;
}

static void fill_hash(char hash[HWA_SHA256_HEX_SIZE], char digit)
{
    memset(hash, digit, HWA_SHA256_HEX_SIZE - 1U);
    hash[HWA_SHA256_HEX_SIZE - 1U] = '\0';
}

typedef struct HWAStage9SourceIdentityLedger {
    uint64_t size;
#if defined(_WIN32)
    uint64_t device;
    uint64_t inode;
#else
    dev_t device;
    ino_t inode;
#endif
} HWAStage9SourceIdentityLedger;

static uint64_t test_text_bytes(const char *text)
{
    return text == NULL ? 0U : (uint64_t)strlen(text) + UINT64_C(1);
}

static uint64_t test_pre_adapter_live_work(
    const HWAGapReportResult *result)
{
    uint64_t total = (uint64_t)sizeof(*result) +
        (uint64_t)result->source_count * (uint64_t)sizeof(*result->sources) +
        (uint64_t)result->label_count * (uint64_t)sizeof(*result->labels) +
        (uint64_t)result->excerpt_count * (uint64_t)sizeof(*result->excerpts) +
        (uint64_t)result->source_count *
            (uint64_t)sizeof(HWAStage9SourceIdentityLedger) +
        test_text_bytes(".") +
        (result->mode == HWA_GAP_REPORT_RANK ? 0U : test_text_bytes(".")) +
        test_text_bytes(result->title) +
        test_text_bytes(result->audibility_method);
    size_t index;
    const char *excerpt_reason = result->mode == HWA_GAP_REPORT_RANK
        ? "rank-mode-does-not-render-excerpts" : "excerpt-not-rendered";
    for (index = 0U; index < result->source_count; ++index)
        total += test_text_bytes(result->sources[index].name) +
                 test_text_bytes(".");
    for (index = 0U; index < result->label_count; ++index) {
        const HWAGapReportLabel *row = &result->labels[index];
        total += test_text_bytes(row->case_id) + test_text_bytes(row->pitch) +
                 test_text_bytes(row->register_name) +
                 test_text_bytes(row->dynamic) +
                 test_text_bytes(row->gesture) +
                 test_text_bytes(row->physical_element) +
                 test_text_bytes(row->section);
    }
    for (index = 0U; index < result->excerpt_count; ++index)
        total += test_text_bytes(result->excerpts[index].name) +
                 UINT64_C(3) + test_text_bytes(excerpt_reason);
    return total;
}

static void set_nested_work_limit(HWAGapReportOptions *options,
                                  uint64_t maximum)
{
    options->measurement.max_work_bytes = maximum;
    options->production.max_work_bytes = maximum;
    options->production.profile_limits.max_work_bytes = maximum;
    options->run.max_work_bytes = maximum;
    options->experiment.max_work_bytes = maximum;
    options->experiment.run.max_work_bytes = maximum;
}

static int make_path(char path[256])
{
#if defined(_WIN32)
    int written = snprintf(path, 256U, "hwa-stage9-persistence-%lu.tmp",
                           (unsigned long)_getpid());
    return written < 0 || written >= 256 ? -1 : 0;
#else
    int descriptor;
    (void)snprintf(path, 256U, "/tmp/hwa-stage9-persistence-XXXXXX");
    descriptor = mkstemp(path);
    if (descriptor < 0) return -1;
    return close(descriptor) == 0 && unlink(path) == 0 ? 0 : -1;
#endif
}

static int make_result(HWAGapReportResult *result)
{
    HWAGapReportSource *source;
    HWAGapReportLabel *label;
    HWAGapReportCandidate *candidate;
    HWAGapReportCase *record;
    char error[HWA_ERROR_SIZE];
    memset(result, 0, sizeof(*result));
    hwa_gap_report_options_default(&result->options);
    result->mode = HWA_GAP_REPORT_RANK;
    result->manifest_path = copy_text("/host/one/manifest.json");
    result->title = copy_text("Gap, report \"one\"");
    result->audibility_method = copy_text(HWA_GAP_REPORT_AUDIBILITY_METHOD);
    fill_hash(result->manifest_sha256, 'a');
    result->source_count = 1U;
    result->sources = (HWAGapReportSource *)calloc(
        1U, sizeof(*result->sources));
    result->label_count = 1U;
    result->labels = (HWAGapReportLabel *)calloc(
        1U, sizeof(*result->labels));
    result->candidate_count = 1U;
    result->candidates = (HWAGapReportCandidate *)calloc(
        1U, sizeof(*result->candidates));
    result->case_count = 1U;
    result->cases = (HWAGapReportCase *)calloc(
        1U, sizeof(*result->cases));
    if (result->manifest_path == NULL || result->title == NULL ||
        result->audibility_method == NULL || result->sources == NULL ||
        result->labels == NULL || result->candidates == NULL ||
        result->cases == NULL) return -1;
    source = &result->sources[0];
    source->id = 1U;
    source->name = copy_text("model-run");
    source->kind = HWA_GAP_REPORT_SOURCE_EXPERIMENT;
    source->path = copy_text("/host/one/result.hwa-experiment");
    fill_hash(source->sha256, 'b');
    source->file_bytes = 0U;
    source->candidate_count = 1U;
    result->total_input_bytes = 0U;
    label = &result->labels[0];
    label->id = 1U;
    label->source_id = 1U;
    label->case_id = copy_text("case-one");
    label->pitch = copy_text("A4");
    label->register_name = copy_text("middle");
    label->dynamic = copy_text("mf");
    label->gesture = copy_text("sustain");
    label->physical_element = copy_text("body");
    label->section = copy_text("attack");
    candidate = &result->candidates[0];
    candidate->id = 1U;
    candidate->source_id = 1U;
    candidate->source_row = 2U;
    candidate->case_id = copy_text("baseline-check");
    candidate->metric = copy_text("level");
    candidate->family_key = copy_text(
        "source:1:experiment:role:4:level");
    candidate->kind = HWA_GAP_REPORT_CANDIDATE_EXPERIMENT;
    candidate->availability = HWA_GAP_REPORT_AVAILABLE;
    candidate->raw_value = 0.8;
    candidate->raw_value_valid = 1;
    candidate->reason = copy_text("");
    record = &result->cases[0];
    record->id = 1U;
    record->candidate_id = 1U;
    record->case_id = copy_text("case-one");
    record->availability = HWA_GAP_REPORT_AVAILABLE;
    record->value = 0.7;
    record->confidence = 0.9;
    record->value_valid = 1;
    record->confidence_valid = 1;
    record->reason = copy_text("");
    if (source->name == NULL || source->path == NULL ||
        label->case_id == NULL || label->pitch == NULL ||
        label->register_name == NULL || label->dynamic == NULL ||
        label->gesture == NULL || label->physical_element == NULL ||
        label->section == NULL || candidate->case_id == NULL ||
        candidate->metric == NULL || candidate->family_key == NULL ||
        candidate->reason == NULL || record->case_id == NULL ||
        record->reason == NULL ||
        hwa_gap_report_result_rebuild(result, error, sizeof(error)) != 0 ||
        hwa_gap_report_result_retained_bytes(
            result, &result->retained_work_bytes) != 0 ||
        hwa_gap_report_result_validate(result, error, sizeof(error)) != 0)
        return -1;
    return 0;
}

static int refresh_result(HWAGapReportResult *result)
{
    char error[HWA_ERROR_SIZE];
    return hwa_gap_report_result_rebuild(result, error, sizeof(error)) == 0 &&
           hwa_gap_report_result_retained_bytes(
               result, &result->retained_work_bytes) == 0 &&
           hwa_gap_report_result_validate(
               result, error, sizeof(error)) == 0
        ? 0 : -1;
}

static int make_excerpt_result(HWAGapReportResult *result)
{
    HWAGapReportSource *sources;
    HWAGapReportExcerpt *excerpt;
    if (make_result(result) != 0) return -1;
    result->mode = HWA_GAP_REPORT_EXCERPTS;
    result->output_directory = copy_text("/host/one/bundle");
    sources = (HWAGapReportSource *)realloc(
        result->sources, 3U * sizeof(*result->sources));
    if (result->output_directory == NULL || sources == NULL) return -1;
    result->sources = sources;
    memset(&result->sources[1], 0, 2U * sizeof(*result->sources));
    result->source_count = 3U;
    result->sources[1].id = 2U;
    result->sources[1].name = copy_text("model-wave");
    result->sources[1].kind = HWA_GAP_REPORT_SOURCE_WAVE;
    result->sources[1].path = copy_text("/host/one/model.wav");
    fill_hash(result->sources[1].sha256, 'c');
    result->sources[2].id = 3U;
    result->sources[2].name = copy_text("reference-wave");
    result->sources[2].kind = HWA_GAP_REPORT_SOURCE_WAVE;
    result->sources[2].path = copy_text("/host/one/reference.wav");
    fill_hash(result->sources[2].sha256, 'd');
    result->excerpt_count = 1U;
    result->excerpts = (HWAGapReportExcerpt *)calloc(
        1U, sizeof(*result->excerpts));
    if (result->sources[1].name == NULL ||
        result->sources[1].path == NULL ||
        result->sources[2].name == NULL ||
        result->sources[2].path == NULL || result->excerpts == NULL)
        return -1;
    excerpt = &result->excerpts[0];
    excerpt->id = 1U;
    excerpt->name = copy_text("clip");
    excerpt->candidate_source_id = 1U;
    excerpt->candidate_row = 2U;
    excerpt->view = HWA_GAP_REPORT_VIEW_RAW;
    excerpt->reference_source_id = 3U;
    excerpt->model_source_id = 2U;
    excerpt->reference_start_sample = 8U;
    excerpt->model_start_sample = 9U;
    excerpt->frame_count = 4U;
    excerpt->make_x = 1;
    excerpt->availability = HWA_GAP_REPORT_AVAILABLE;
    excerpt->reference_path = copy_text("audio/clip-a.wav");
    excerpt->model_path = copy_text("audio/clip-b.wav");
    excerpt->x_path = copy_text("audio/clip-x.wav");
    fill_hash(excerpt->reference_sha256, 'e');
    fill_hash(excerpt->model_sha256, 'f');
    excerpt->x_is_reference =
        hwa_gap_report_excerpt_x_is_reference(result, excerpt);
    memcpy(excerpt->x_sha256, excerpt->x_is_reference
               ? excerpt->reference_sha256 : excerpt->model_sha256,
           HWA_SHA256_HEX_SIZE);
    excerpt->reference_file_bytes = 52U;
    excerpt->model_file_bytes = 52U;
    excerpt->x_file_bytes = 52U;
    excerpt->reason = copy_text("");
    result->total_output_bytes = 156U;
    if (excerpt->name == NULL || excerpt->reference_path == NULL ||
        excerpt->model_path == NULL || excerpt->x_path == NULL ||
        excerpt->reason == NULL) return -1;
    return refresh_result(result);
}

static int add_second_excerpt(HWAGapReportResult *result)
{
    HWAGapReportExcerpt *rows = (HWAGapReportExcerpt *)realloc(
        result->excerpts, 2U * sizeof(*result->excerpts));
    HWAGapReportExcerpt *row;
    if (rows == NULL) return -1;
    result->excerpts = rows;
    memset(&result->excerpts[1], 0, sizeof(result->excerpts[1]));
    result->excerpt_count = 2U;
    row = &result->excerpts[1];
    row->id = 2U;
    row->name = copy_text("clip");
    row->candidate_source_id = 1U;
    row->candidate_row = 2U;
    row->view = HWA_GAP_REPORT_VIEW_RAW;
    row->reference_source_id = 3U;
    row->model_source_id = 2U;
    row->reference_start_sample = 8U;
    row->model_start_sample = 9U;
    row->frame_count = 4U;
    row->availability = HWA_GAP_REPORT_UNAVAILABLE;
    row->reference_path = copy_text("");
    row->model_path = copy_text("");
    row->x_path = copy_text("");
    row->reason = copy_text("excerpt-not-rendered");
    if (row->name == NULL || row->reference_path == NULL ||
        row->model_path == NULL || row->x_path == NULL || row->reason == NULL)
        return -1;
    return refresh_result(result);
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
    data[(size_t)length] = 0U;
    *size = (size_t)length;
    return data;
}

static int write_bytes(const char *path,
                       const unsigned char *bytes,
                       size_t size)
{
    FILE *stream = fopen(path, "wb");
    if (stream == NULL) return -1;
    return fwrite(bytes, 1U, size, stream) == size && fclose(stream) == 0
        ? 0 : -1;
}

static int write_result(const char *path, const HWAGapReportResult *result)
{
    FILE *stream = fopen(path, "wb");
    char error[HWA_ERROR_SIZE];
    int status;
    if (stream == NULL) return -1;
    status = hwa_gap_report_file_write(
        stream, result, error, sizeof(error));
    if (fclose(stream) != 0) status = -1;
    return status;
}

static unsigned char *replace_meta(const unsigned char *bytes,
                                   size_t size,
                                   const char *key,
                                   const char *value,
                                   size_t *new_size)
{
    char prefix[128];
    const char *row;
    const char *start;
    const char *end;
    size_t before;
    size_t after;
    size_t value_size = strlen(value);
    unsigned char *copy;
    int written = snprintf(prefix, sizeof(prefix), "META,%s,", key);
    if (written < 0 || (size_t)written >= sizeof(prefix)) return NULL;
    row = strstr((const char *)bytes, prefix);
    if (row == NULL) return NULL;
    start = row + strlen(prefix);
    end = strchr(start, ',');
    if (end == NULL) return NULL;
    before = (size_t)(start - (const char *)bytes);
    after = size - (size_t)(end - (const char *)bytes);
    if (value_size > SIZE_MAX - before - after) return NULL;
    *new_size = before + value_size + after;
    copy = (unsigned char *)malloc(*new_size + 1U);
    if (copy == NULL) return NULL;
    memcpy(copy, bytes, before);
    memcpy(copy + before, value, value_size);
    memcpy(copy + before + value_size, end, after);
    copy[*new_size] = 0U;
    return copy;
}

static unsigned char *replace_same_size(const unsigned char *bytes,
                                        size_t size,
                                        const char *from,
                                        const char *to)
{
    const char *found;
    unsigned char *copy;
    size_t length = strlen(from);
    if (length != strlen(to)) return NULL;
    found = strstr((const char *)bytes, from);
    if (found == NULL) return NULL;
    copy = (unsigned char *)malloc(size + 1U);
    if (copy == NULL) return NULL;
    memcpy(copy, bytes, size + 1U);
    memcpy(copy + (size_t)(found - (const char *)bytes), to, length);
    return copy;
}

int main(void)
{
    HWAGapReportResult result;
    HWAGapReportResult source_result;
    HWAGapReportResult excerpt_result;
    HWAGapReportResult duplicate_result;
    HWAGapReportResult loaded;
    HWAGapReportOptions limits;
    char first[256];
    char second[256];
    char corrupt[256];
    char hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    unsigned char *first_bytes;
    unsigned char *second_bytes;
    unsigned char *changed;
    size_t first_size;
    size_t second_size;
    size_t changed_size;
    char count_text[32];
    char *long_field;
    memset(&result, 0, sizeof(result));
    memset(&source_result, 0, sizeof(source_result));
    memset(&excerpt_result, 0, sizeof(excerpt_result));
    memset(&duplicate_result, 0, sizeof(duplicate_result));
    memset(&loaded, 0, sizeof(loaded));
    CHECK(make_path(first) == 0 && make_path(second) == 0 &&
              make_path(corrupt) == 0,
          "temporary paths");
    CHECK(make_excerpt_result(&source_result) == 0,
          "rank unavailable X fixture");
    if (source_result.excerpt_count == 1U) {
        static const char hex[] = "0123456789abcdef";
        HWAGapReportExcerpt *rank_excerpt = &source_result.excerpts[0];
        size_t hex_index;
        int selected_reference = 0;
        source_result.mode = HWA_GAP_REPORT_RANK;
        free(source_result.output_directory);
        source_result.output_directory = NULL;
        rank_excerpt->availability = HWA_GAP_REPORT_UNAVAILABLE;
        rank_excerpt->reference_path[0] = '\0';
        rank_excerpt->model_path[0] = '\0';
        rank_excerpt->x_path[0] = '\0';
        rank_excerpt->reference_sha256[0] = '\0';
        rank_excerpt->model_sha256[0] = '\0';
        rank_excerpt->x_sha256[0] = '\0';
        rank_excerpt->reference_file_bytes = 0U;
        rank_excerpt->model_file_bytes = 0U;
        rank_excerpt->x_file_bytes = 0U;
        rank_excerpt->reference_gain_db = 0.0;
        rank_excerpt->model_gain_db = 0.0;
        rank_excerpt->x_is_reference = 0;
        free(rank_excerpt->reason);
        rank_excerpt->reason = copy_text("rank-mode-does-not-render-excerpts");
        source_result.total_output_bytes = 0U;
        for (hex_index = 0U; hex_index < sizeof(hex) - 1U; ++hex_index) {
            source_result.manifest_sha256[0] = hex[hex_index];
            if (hwa_gap_report_excerpt_x_is_reference(
                    &source_result, rank_excerpt)) {
                selected_reference = 1;
                break;
            }
        }
        CHECK(selected_reference && rank_excerpt->reason != NULL &&
                  refresh_result(&source_result) == 0 &&
                  write_result(corrupt, &source_result) == 0,
              "rank unavailable X side was treated as rendered clip state");
    }
    hwa_gap_report_result_free(&source_result);
    CHECK(make_result(&result) == 0, "valid fixture");
    result.candidates[0].raw_value = 2.0;
    CHECK(refresh_result(&result) != 0,
          "candidate source value above one was accepted");
    result.candidates[0].raw_value = 0.8;
    CHECK(refresh_result(&result) == 0, "restore candidate source value");
    result.candidates[0].raw_value_valid = 0;
    CHECK(refresh_result(&result) != 0,
          "nonzero invalid candidate source value was accepted");
    result.candidates[0].raw_value_valid = 1;
    CHECK(refresh_result(&result) == 0, "restore candidate validity");
    result.cases[0].value = -1.0;
    CHECK(refresh_result(&result) != 0,
          "negative case source value was accepted");
    result.cases[0].value = 0.7;
    CHECK(refresh_result(&result) == 0, "restore case source value");
    free(result.candidates[0].family_key);
    result.candidates[0].family_key = copy_text("forged-family");
    CHECK(result.candidates[0].family_key != NULL &&
              write_result(first, &result) != 0,
          "forged source-derived family key was accepted");
    free(result.candidates[0].family_key);
    result.candidates[0].family_key = copy_text(
        "source:1:experiment:role:4:level");
    CHECK(result.candidates[0].family_key != NULL &&
              refresh_result(&result) == 0,
          "restore candidate family key");
    free(result.candidates[0].metric);
    result.candidates[0].metric = copy_text("");
    CHECK(result.candidates[0].metric != NULL && refresh_result(&result) != 0,
          "empty candidate metric was accepted");
    free(result.candidates[0].metric);
    result.candidates[0].metric = copy_text("level");
    CHECK(result.candidates[0].metric != NULL && refresh_result(&result) == 0,
          "restore candidate metric");
    CHECK(write_result(first, &result) == 0, "canonical write");
    first_bytes = read_file(first, &first_size);
    CHECK(first_bytes != NULL && first_size > 16U &&
              memcmp(first_bytes, "HWA_REPORT,1\r\n", 14U) == 0 &&
              strstr((const char *)first_bytes,
                     "\nMETA,tool_version,1.1.0,\r\n") != NULL &&
              strstr((const char *)first_bytes, "\nSOURCE,") != NULL &&
              strstr((const char *)first_bytes, "\nLABEL,") != NULL &&
              strstr((const char *)first_bytes, "\nCANDIDATE,") != NULL &&
              strstr((const char *)first_bytes, "\nOCCURRENCE,") != NULL &&
              strstr((const char *)first_bytes, "\nFAMILY,") != NULL &&
              strstr((const char *)first_bytes, "\nGROUP,") != NULL &&
              strstr((const char *)first_bytes, "\nWORST,") != NULL &&
              strstr((const char *)first_bytes, "manifest.json") == NULL &&
              strstr((const char *)first_bytes, "result.hwa-run") == NULL,
          "header, sections, or inert path projection");
    hwa_gap_report_options_default(&limits);
    {
        int read_status = hwa_gap_report_file_read(
            first, &limits, &loaded, hash, error, sizeof(error));
        if (read_status != 0)
            (void)fprintf(stderr, "reader error: %s\n", error);
        CHECK(read_status == 0 &&
              strcmp(loaded.manifest_path, ".") == 0 &&
              loaded.output_directory == NULL &&
              strcmp(loaded.sources[0].path, ".") == 0 &&
              loaded.candidate_count == 1U && loaded.family_count == 1U &&
              loaded.group_count == 6U && loaded.case_count == 1U &&
              loaded.candidates[0].score_valid &&
              loaded.warning_count == 1U &&
              strcmp(loaded.warnings[0].code,
                     "source-unavailable") == 0 &&
              loaded.evaluation_count == result.evaluation_count,
              "round trip read");
    }
    CHECK(write_result(second, &loaded) == 0, "round trip write");
    second_bytes = read_file(second, &second_size);
    CHECK(second_bytes != NULL && first_size == second_size &&
              memcmp(first_bytes, second_bytes, first_size) == 0,
          "round trip bytes changed");
    free(second_bytes);
    hwa_gap_report_result_free(&loaded);

    changed = replace_meta(first_bytes, first_size,
                           "tool_version", "1.0.0", &changed_size);
    CHECK(changed != NULL && write_bytes(corrupt, changed, changed_size) == 0,
          "write version 1.0.0 compatibility fixture");
    free(changed);
    CHECK(hwa_gap_report_file_read(
              corrupt, &limits, &loaded, hash, error, sizeof(error)) == 0 &&
              loaded.candidate_count == result.candidate_count &&
              loaded.evaluation_count == result.evaluation_count,
          "version 1.0.0 report did not remain readable");
    hwa_gap_report_result_free(&loaded);
    changed = replace_meta(first_bytes, first_size,
                           "tool_version", "", &changed_size);
    CHECK(changed != NULL && write_bytes(corrupt, changed, changed_size) == 0,
          "write empty tool version fixture");
    free(changed);
    CHECK(hwa_gap_report_file_read(
              corrupt, &limits, &loaded, hash, error, sizeof(error)) != 0,
          "empty tool version provenance accepted");

    changed = replace_meta(first_bytes, first_size, "evaluation_count", "0",
                           &changed_size);
    CHECK(changed != NULL && write_bytes(corrupt, changed, changed_size) == 0,
          "write corrupt derived count");
    free(changed);
    CHECK(hwa_gap_report_file_read(
              corrupt, &limits, &loaded, hash, error, sizeof(error)) != 0,
          "tampered derived count accepted");
    (void)snprintf(count_text, sizeof(count_text), "%llu",
                   (unsigned long long)(result.evaluation_count + 1U));
    changed = replace_meta(first_bytes, first_size, "evaluation_count",
                           count_text, &changed_size);
    CHECK(changed != NULL && write_bytes(corrupt, changed, changed_size) == 0,
          "write inflated derived count");
    free(changed);
    CHECK(hwa_gap_report_file_read(
              corrupt, &limits, &loaded, hash, error, sizeof(error)) != 0,
          "inflated derived count accepted");
    changed = replace_same_size(first_bytes, first_size,
                                "WORST,1,1,case-one,",
                                "WORST,1,2,case-one,");
    CHECK(changed != NULL && write_bytes(corrupt, changed, first_size) == 0,
          "write parsed case catalog mismatch");
    free(changed);
    CHECK(hwa_gap_report_file_read(
              corrupt, &limits, &loaded, hash, error, sizeof(error)) != 0,
          "parsed case catalog mismatch accepted");
    {
        const char *warning = strstr(
            (const char *)first_bytes, "source-unavailable");
        unsigned char *copy;
        size_t offset;
        CHECK(warning != NULL, "saved warning row");
        copy = (unsigned char *)malloc(first_size);
        CHECK(copy != NULL, "warning corruption allocation");
        memcpy(copy, first_bytes, first_size);
        offset = (size_t)(warning - (const char *)first_bytes);
        copy[offset] = 'S';
        CHECK(write_bytes(corrupt, copy, first_size) == 0,
              "write corrupt saved warning");
        free(copy);
        CHECK(hwa_gap_report_file_read(
                  corrupt, &limits, &loaded, hash,
                  error, sizeof(error)) != 0,
              "tampered saved warning accepted");
    }
    changed = replace_meta(first_bytes, first_size,
                           "measurement_max_gaps", "0", &changed_size);
    CHECK(changed != NULL && write_bytes(corrupt, changed, changed_size) == 0,
          "write corrupt nested cap");
    free(changed);
    CHECK(hwa_gap_report_file_read(
              corrupt, &limits, &loaded, hash, error, sizeof(error)) != 0,
          "invalid nested option accepted");
    limits.measurement.max_gaps = 1U;
    changed = replace_meta(first_bytes, first_size,
                           "measurement_max_gaps", "2", &changed_size);
    CHECK(changed != NULL && write_bytes(corrupt, changed, changed_size) == 0,
          "write over-current nested cap");
    free(changed);
    CHECK(hwa_gap_report_file_read(
              corrupt, &limits, &loaded, hash, error, sizeof(error)) == 0 &&
              loaded.options.measurement.max_gaps == 1U,
          "saved nested producer cap blocked current limits");
    hwa_gap_report_result_free(&loaded);
    limits.max_sources = 1U;
    limits.max_labels = 1U;
    limits.max_candidates = 1U;
    limits.max_families = 1U;
    limits.max_groups = 6U;
    limits.max_cases = 1U;
    limits.max_excerpts = 1U;
    limits.max_warnings = 1U;
    CHECK(hwa_gap_report_file_read(
              first, &limits, &loaded, hash, error, sizeof(error)) == 0 &&
              loaded.options.max_sources == 1U &&
              loaded.options.max_groups == 6U &&
              loaded.options.measurement.max_gaps == 1U,
          "saved Stage 9 ceilings blocked fitting current facts");
    hwa_gap_report_result_free(&loaded);
    hwa_gap_report_options_default(&limits);
    changed = replace_meta(first_bytes, first_size,
                           "max_output_file_bytes", "1", &changed_size);
    CHECK(changed != NULL && write_bytes(corrupt, changed, changed_size) == 0,
          "write underreported saved report cap");
    free(changed);
    CHECK(hwa_gap_report_file_read(
              corrupt, &limits, &loaded, hash, error, sizeof(error)) != 0,
          "saved report byte cap below actual bytes accepted");
    changed = replace_same_size(
        first_bytes, first_size, ",0,1\r\nLABEL,", ",0,0\r\nLABEL,");
    changed_size = first_size;
    CHECK(changed != NULL && write_bytes(corrupt, changed, changed_size) == 0,
          "write truncated source candidate catalog");
    free(changed);
    CHECK(hwa_gap_report_file_read(
              corrupt, &limits, &loaded, hash, error, sizeof(error)) != 0,
          "truncated source candidate catalog accepted");
    changed = replace_same_size(
        first_bytes, first_size, "model-run", "../bad.xx");
    CHECK(changed != NULL && write_bytes(corrupt, changed, first_size) == 0,
          "write invalid saved SOURCE name");
    free(changed);
    CHECK(hwa_gap_report_file_read(
              corrupt, &limits, &loaded, hash, error, sizeof(error)) != 0,
          "saved SOURCE name with a path separator accepted");
    hwa_gap_report_options_default(&limits);
    {
        const char *candidate = strstr(
            (const char *)first_bytes, "\r\nCANDIDATE,");
        size_t offset;
        unsigned char *copy;
        CHECK(candidate != NULL, "candidate row for corruption");
        offset = (size_t)(candidate - (const char *)first_bytes) + 2U;
        copy = (unsigned char *)malloc(first_size);
        CHECK(copy != NULL, "corruption allocation");
        memcpy(copy, first_bytes, first_size);
        copy[offset] = 'X';
        CHECK(write_bytes(corrupt, copy, first_size) == 0,
              "write corrupt section order");
        free(copy);
        CHECK(hwa_gap_report_file_read(
                  corrupt, &limits, &loaded, hash,
                  error, sizeof(error)) != 0,
              "unknown section accepted");
    }
    CHECK(first_size > 2U &&
              write_bytes(corrupt, first_bytes, first_size - 1U) == 0 &&
              hwa_gap_report_file_read(
                  corrupt, &limits, &loaded, hash,
                  error, sizeof(error)) != 0,
          "truncated canonical file accepted");

    CHECK(make_result(&source_result) == 0, "source limit fixture");
    source_result.sources[0].file_bytes = 2U;
    source_result.total_input_bytes = 2U;
    CHECK(refresh_result(&source_result) == 0 &&
              write_result(second, &source_result) == 0,
          "source limit fixture write");
    second_bytes = read_file(second, &second_size);
    CHECK(second_bytes != NULL, "source limit bytes");
    hwa_gap_report_options_default(&limits);
    limits.experiment.max_output_file_bytes = 1U;
    CHECK(hwa_gap_report_file_read(
              second, &limits, &loaded, hash, error, sizeof(error)) != 0,
          "experiment source bytes above current nested cap accepted");
    hwa_gap_report_options_default(&limits);
    changed = replace_meta(second_bytes, second_size,
                           "experiment_max_output_file_bytes", "1",
                           &changed_size);
    CHECK(changed != NULL && write_bytes(corrupt, changed, changed_size) == 0,
          "write saved experiment byte cap");
    free(changed);
    CHECK(hwa_gap_report_file_read(
              corrupt, &limits, &loaded, hash, error, sizeof(error)) != 0,
          "experiment source bytes above saved nested cap accepted");
    free(second_bytes);
    hwa_gap_report_result_free(&source_result);

    CHECK(make_result(&source_result) == 0, "maximum saved field fixture");
    long_field = (char *)malloc(65538U);
    CHECK(long_field != NULL, "maximum saved field allocation");
    memset(long_field, 'k', 65537U);
    long_field[65537U] = '\0';
    free(source_result.title);
    source_result.title = long_field;
    long_field[65536U] = '\0';
    CHECK(refresh_result(&source_result) == 0 &&
              write_result(second, &source_result) == 0,
          "exact maximum decoded field did not round trip");
    hwa_gap_report_options_default(&limits);
    CHECK(hwa_gap_report_file_read(
              second, &limits, &loaded, hash, error, sizeof(error)) == 0,
          "reader rejected the exact maximum decoded field");
    hwa_gap_report_result_free(&loaded);
    long_field[65536U] = 'k';
    long_field[65537U] = '\0';
    CHECK(refresh_result(&source_result) == 0 &&
              write_result(corrupt, &source_result) != 0,
          "writer accepted a decoded field above its reader limit");
    hwa_gap_report_result_free(&source_result);

    CHECK(make_excerpt_result(&excerpt_result) == 0,
          "excerpt persistence fixture");
    CHECK(write_result(second, &excerpt_result) == 0,
          "canonical excerpt write");
    second_bytes = read_file(second, &second_size);
    CHECK(second_bytes != NULL &&
              strstr((const char *)second_bytes,
                     "audio/clip-a.wav") != NULL,
          "canonical excerpt bytes");
    excerpt_result.excerpts[0].x_is_reference =
        !excerpt_result.excerpts[0].x_is_reference;
    memcpy(excerpt_result.excerpts[0].x_sha256,
           excerpt_result.excerpts[0].x_is_reference
               ? excerpt_result.excerpts[0].reference_sha256
               : excerpt_result.excerpts[0].model_sha256,
           HWA_SHA256_HEX_SIZE);
    CHECK(write_result(corrupt, &excerpt_result) != 0,
          "writer accepted a forged deterministic X side");
    excerpt_result.excerpts[0].x_is_reference =
        hwa_gap_report_excerpt_x_is_reference(
            &excerpt_result, &excerpt_result.excerpts[0]);
    memcpy(excerpt_result.excerpts[0].x_sha256,
           excerpt_result.excerpts[0].x_is_reference
               ? excerpt_result.excerpts[0].reference_sha256
               : excerpt_result.excerpts[0].model_sha256,
           HWA_SHA256_HEX_SIZE);
    changed = replace_meta(second_bytes, second_size,
                           "max_input_frames", "10", &changed_size);
    CHECK(changed != NULL && write_bytes(corrupt, changed, changed_size) == 0,
          "write saved excerpt frame cap");
    free(changed);
    hwa_gap_report_options_default(&limits);
    CHECK(hwa_gap_report_file_read(
              corrupt, &limits, &loaded, hash, error, sizeof(error)) != 0,
          "excerpt span above saved frame cap accepted");
    changed = replace_same_size(
        second_bytes, second_size,
        "audio/clip-a.wav", "audio/clip-z.wav");
    CHECK(changed != NULL && write_bytes(corrupt, changed, second_size) == 0,
          "write noncanonical FILE path");
    free(changed);
    CHECK(hwa_gap_report_file_read(
              corrupt, &limits, &loaded, hash, error, sizeof(error)) != 0,
          "noncanonical saved FILE path accepted");
    changed = replace_same_size(
        second_bytes, second_size, "audio/clip-a.wav", "audio/../x-a.wav");
    CHECK(changed != NULL, "invalid excerpt name path copy");
    {
        unsigned char *name_changed = changed == NULL ? NULL :
            replace_same_size(changed, second_size,
                              "audio/clip-b.wav", "audio/../x-b.wav");
        free(changed);
        changed = name_changed == NULL ? NULL :
            replace_same_size(name_changed, second_size,
                              "CLIP,1,clip,", "CLIP,1,../x,");
        free(name_changed);
    }
    CHECK(changed != NULL && write_bytes(corrupt, changed, second_size) == 0,
          "write invalid saved excerpt name");
    free(changed);
    CHECK(hwa_gap_report_file_read(
              corrupt, &limits, &loaded, hash, error, sizeof(error)) != 0,
          "saved excerpt name with a path separator accepted");
    CHECK(make_excerpt_result(&duplicate_result) == 0 &&
              add_second_excerpt(&duplicate_result) != 0,
          "duplicate excerpt names passed result validation");
    hwa_gap_report_result_free(&duplicate_result);

    excerpt_result.mode = HWA_GAP_REPORT_FULL;
    CHECK(refresh_result(&excerpt_result) == 0 &&
              write_result(second, &excerpt_result) == 0,
          "FULL saved bundle fixture write");
    free(second_bytes);
    second_bytes = read_file(second, &second_size);
    CHECK(second_bytes != NULL, "FULL saved bundle bytes");
    (void)snprintf(count_text, sizeof(count_text), "%llu",
                   (unsigned long long)(excerpt_result.total_output_bytes +
                                        (uint64_t)second_size * 2U));
    changed = replace_meta(second_bytes, second_size,
                           "max_bundle_bytes", count_text, &changed_size);
    CHECK(changed != NULL && write_bytes(corrupt, changed, changed_size) == 0,
          "write incomplete FULL saved bundle cap");
    free(changed);
    CHECK(hwa_gap_report_file_read(
              corrupt, &limits, &loaded, hash, error, sizeof(error)) != 0,
          "FULL saved cap omitted mandatory JSON and HTML reports");
    free(excerpt_result.excerpts[0].reference_path);
    excerpt_result.excerpts[0].reference_path =
        copy_text("audio/clip-z.wav");
    CHECK(excerpt_result.excerpts[0].reference_path != NULL &&
              hwa_gap_report_result_retained_bytes(
                  &excerpt_result,
                  &excerpt_result.retained_work_bytes) == 0 &&
              write_result(corrupt, &excerpt_result) != 0,
          "writer accepted a noncanonical clip path");
    free(second_bytes);
    hwa_gap_report_result_free(&excerpt_result);

    CHECK(make_result(&source_result) == 0, "saved nested work fixture");
    source_result.sources[0].file_bytes = 2U;
    source_result.total_input_bytes = 2U;
    CHECK(refresh_result(&source_result) == 0 &&
              write_result(second, &source_result) == 0,
          "saved nested work fixture write");
    second_bytes = read_file(second, &second_size);
    CHECK(second_bytes != NULL, "saved nested work fixture bytes");
    changed = replace_meta(second_bytes, second_size,
                           "experiment_max_work_bytes", "8", &changed_size);
    CHECK(changed != NULL && write_bytes(corrupt, changed, changed_size) == 0,
          "write saved experiment work cap");
    free(changed);
    hwa_gap_report_options_default(&limits);
    CHECK(hwa_gap_report_file_read(
              corrupt, &limits, &loaded, hash, error, sizeof(error)) != 0,
          "experiment source above saved reader work cap accepted");
    limits.experiment.max_work_bytes = 8U;
    CHECK(hwa_gap_report_file_read(
              second, &limits, &loaded, hash, error, sizeof(error)) != 0,
          "experiment source above current reader work cap accepted");
    free(second_bytes);
    hwa_gap_report_result_free(&source_result);

    {
        uint64_t nested_work;
        uint64_t exact_outer_work;
        char under_text[32];
        CHECK(make_result(&source_result) == 0,
              "combined producer work fixture");
        source_result.sources[0].file_bytes = UINT64_C(1000000);
        source_result.total_input_bytes = UINT64_C(1000000);
        nested_work = (source_result.sources[0].file_bytes + UINT64_C(1)) *
                      UINT64_C(3);
        exact_outer_work = test_pre_adapter_live_work(&source_result) +
                           nested_work;
        source_result.options.max_work_bytes = exact_outer_work;
        set_nested_work_limit(&source_result.options, nested_work);
        CHECK(refresh_result(&source_result) == 0 &&
                  write_result(second, &source_result) == 0,
              "exact combined producer work cap");
        second_bytes = read_file(second, &second_size);
        CHECK(second_bytes != NULL, "combined producer work bytes");
        limits = source_result.options;
        CHECK(hwa_gap_report_file_read(
                  second, &limits, &loaded, hash, error, sizeof(error)) == 0,
              "reader rejected exact combined producer work cap");
        hwa_gap_report_result_free(&loaded);
        limits.max_work_bytes = exact_outer_work - UINT64_C(1);
        CHECK(hwa_gap_report_file_read(
                  second, &limits, &loaded, hash, error, sizeof(error)) != 0,
              "current outer cap omitted retained pre-adapter work");
        (void)snprintf(under_text, sizeof(under_text), "%llu",
                       (unsigned long long)(exact_outer_work - UINT64_C(1)));
        changed = replace_meta(second_bytes, second_size,
                               "max_work_bytes", under_text, &changed_size);
        CHECK(changed != NULL &&
                  write_bytes(corrupt, changed, changed_size) == 0,
              "write one-under saved outer work cap");
        free(changed);
        limits.max_work_bytes = exact_outer_work;
        CHECK(hwa_gap_report_file_read(
                  corrupt, &limits, &loaded, hash, error, sizeof(error)) != 0,
              "saved outer cap omitted retained pre-adapter work");
        source_result.options.max_work_bytes = exact_outer_work - UINT64_C(1);
        CHECK(write_result(corrupt, &source_result) != 0,
              "writer accepted one-under combined producer work cap");
        free(second_bytes);
        hwa_gap_report_result_free(&source_result);
    }

    CHECK(make_result(&source_result) == 0, "small saved input cap fixture");
    source_result.options.max_input_bytes = 1024U;
    source_result.options.measurement.max_input_bytes = 1024U;
    source_result.options.production.max_input_bytes = 1024U;
    source_result.options.production.profile_limits.max_input_bytes = 1024U;
    source_result.options.run.max_input_bytes = 1024U;
    source_result.options.experiment.max_input_bytes = 1024U;
    source_result.options.experiment.max_bundle_bytes = 1024U;
    source_result.options.experiment.run.max_input_bytes = 1024U;
    CHECK(write_result(second, &source_result) == 0,
          "report larger than its saved input cap write");
    second_bytes = read_file(second, &second_size);
    CHECK(second_bytes != NULL && second_size > 1024U,
          "saved report did not exceed the saved input cap");
    hwa_gap_report_options_default(&limits);
    CHECK(hwa_gap_report_file_read(
              second, &limits, &loaded, hash, error, sizeof(error)) == 0,
          "saved input cap was wrongly applied to the report file");
    hwa_gap_report_result_free(&loaded);
    free(second_bytes);
    hwa_gap_report_result_free(&source_result);

    free(result.manifest_path);
    free(result.sources[0].path);
    result.manifest_path = copy_text("/different/host/manifest.json");
    result.sources[0].path = copy_text("/different/host/result.hwa-run");
    CHECK(result.manifest_path != NULL && result.sources[0].path != NULL &&
              hwa_gap_report_result_retained_bytes(
                  &result, &result.retained_work_bytes) == 0 &&
              write_result(second, &result) == 0,
          "runtime path rewrite");
    second_bytes = read_file(second, &second_size);
    CHECK(second_bytes != NULL && first_size == second_size &&
              memcmp(first_bytes, second_bytes, first_size) == 0,
          "runtime paths changed saved bytes");
    free(second_bytes);
    result.options.max_output_file_bytes = 1U;
    CHECK(write_result(second, &result) != 0,
          "tiny output cap rendered a persistence scratch file");
    free(first_bytes);
    hwa_gap_report_result_free(&result);
    (void)remove(first);
    (void)remove(second);
    (void)remove(corrupt);
    return 0;
}

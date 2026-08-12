#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#endif

#include "gap_report.h"
#include "gap_report_output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#define HWA_TEST_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#define HWA_TEST_MKDIR(path) mkdir((path), 0700)
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

static char *read_stream(FILE *stream)
{
    long length;
    char *text;
    if (fflush(stream) != 0 || fseek(stream, 0L, SEEK_END) != 0 ||
        (length = ftell(stream)) < 0 || fseek(stream, 0L, SEEK_SET) != 0)
        return NULL;
    text = (char *)malloc((size_t)length + 1U);
    if (text == NULL ||
        fread(text, 1U, (size_t)length, stream) != (size_t)length) {
        free(text);
        return NULL;
    }
    text[(size_t)length] = '\0';
    return text;
}

static char *read_file(const char *path)
{
    FILE *stream = fopen(path, "rb");
    char *text;
    if (stream == NULL) return NULL;
    text = read_stream(stream);
    if (fclose(stream) != 0) {
        free(text);
        return NULL;
    }
    return text;
}

static int write_text(const char *path, const char *text)
{
    FILE *stream = fopen(path, "wb");
    size_t size = strlen(text);
    if (stream == NULL) return -1;
    return fwrite(text, 1U, size, stream) == size && fclose(stream) == 0
        ? 0 : -1;
}

static int exists(const char *path)
{
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) return 0;
    (void)fclose(stream);
    return 1;
}

static int make_directory(char path[256])
{
#if defined(_WIN32)
    static unsigned serial;
    char temporary[256];
    DWORD temporary_size = GetTempPathA((DWORD)sizeof(temporary), temporary);
    unsigned attempt;
    if (temporary_size == 0U || temporary_size >= sizeof(temporary)) return -1;
    for (attempt = 0U; attempt < 100U; ++attempt) {
        int written = snprintf(path, 256U,
                               "%s/hwa-stage9-output-%lu-%u", temporary,
                               (unsigned long)_getpid(), serial++);
        if (written < 0 || written >= 256) return -1;
        if (HWA_TEST_MKDIR(path) == 0) return 0;
    }
    return -1;
#else
    (void)snprintf(path, 256U, "/tmp/hwa-stage9-output-XXXXXX");
    return mkdtemp(path) == NULL ? -1 : 0;
#endif
}

static int join(char path[512], const char *directory, const char *name)
{
    int written = snprintf(path, 512U, "%s/%s", directory, name);
    return written < 0 || written >= 512 ? -1 : 0;
}

static int make_result(HWAGapReportResult *result,
                       HWAGapReportMode mode,
                       const char *output)
{
    static const char *case_ids[3] = {
        "case-a", "case-b", "case-c"
    };
    static const char *metrics[3] = {
        "crest", "band:0", "level"
    };
    static const char *families[3] = {
        "source:1:experiment:role:1:crest",
        "source:1:experiment:role:2:band",
        "source:1:experiment:role:3:level"
    };
    static const double raw_values[3] = {0.2, 0.9, 0.5};
    static const double case_values[3] = {0.95, 0.2, 0.7};
    static const double confidences[3] = {0.9, 1.0, 0.8};
    char error[HWA_ERROR_SIZE];
    size_t index;
    memset(result, 0, sizeof(*result));
    hwa_gap_report_options_default(&result->options);
    result->mode = mode;
    result->manifest_path = copy_text("/host/manifest.json");
    result->output_directory = mode == HWA_GAP_REPORT_RANK
        ? NULL : copy_text(output);
    result->title = copy_text("Hostile <script>alert(1)</script>\n\033 title");
    result->audibility_method = copy_text(HWA_GAP_REPORT_AUDIBILITY_METHOD);
    fill_hash(result->manifest_sha256, 'a');
    result->source_count = 1U;
    result->sources = (HWAGapReportSource *)calloc(
        result->source_count, sizeof(*result->sources));
    result->label_count = 3U;
    result->labels = (HWAGapReportLabel *)calloc(
        result->label_count, sizeof(*result->labels));
    result->candidate_count = 3U;
    result->candidates = (HWAGapReportCandidate *)calloc(
        result->candidate_count, sizeof(*result->candidates));
    result->case_count = 9U;
    result->cases = (HWAGapReportCase *)calloc(
        result->case_count, sizeof(*result->cases));
    if (result->manifest_path == NULL ||
        (mode != HWA_GAP_REPORT_RANK && result->output_directory == NULL) ||
        result->title == NULL || result->audibility_method == NULL ||
        result->sources == NULL || result->labels == NULL ||
        result->candidates == NULL || result->cases == NULL) return -1;
    result->sources[0].id = 1U;
    result->sources[0].name = copy_text("rank-source");
    result->sources[0].kind = HWA_GAP_REPORT_SOURCE_EXPERIMENT;
    result->sources[0].path = copy_text("/host/result.hwa-run");
    fill_hash(result->sources[0].sha256, 'b');
    result->sources[0].file_bytes = 1U;
    result->sources[0].candidate_count = 3U;
    result->total_input_bytes = 1U;
    for (index = 0U; index < 3U; ++index) {
        HWAGapReportLabel *label = &result->labels[index];
        HWAGapReportCandidate *candidate = &result->candidates[index];
        size_t case_index;
        label->id = (uint64_t)index + 1U;
        label->source_id = 1U;
        label->case_id = copy_text(case_ids[index]);
        label->pitch = copy_text("A4");
        label->register_name = copy_text("middle");
        label->dynamic = copy_text("mf");
        label->gesture = copy_text("sustain");
        label->physical_element = copy_text("body");
        label->section = copy_text("attack");
        candidate->id = (uint64_t)index + 1U;
        candidate->source_id = 1U;
        candidate->source_row = ((uint64_t)index + 1U) * UINT64_C(2);
        candidate->case_id = copy_text("baseline-check");
        candidate->metric = copy_text(metrics[index]);
        candidate->family_key = copy_text(families[index]);
        candidate->kind = HWA_GAP_REPORT_CANDIDATE_EXPERIMENT;
        candidate->availability = HWA_GAP_REPORT_AVAILABLE;
        candidate->raw_value = raw_values[index];
        candidate->raw_value_valid = 1;
        candidate->reason = copy_text("");
        if (label->case_id == NULL || label->pitch == NULL ||
            label->register_name == NULL || label->dynamic == NULL ||
            label->gesture == NULL || label->physical_element == NULL ||
            label->section == NULL || candidate->case_id == NULL ||
            candidate->metric == NULL || candidate->family_key == NULL ||
            candidate->reason == NULL) return -1;
        for (case_index = 0U; case_index < 3U; ++case_index) {
            HWAGapReportCase *record =
                &result->cases[index * 3U + case_index];
            int available = case_index == index;
            record->id = (uint64_t)(index * 3U + case_index) + UINT64_C(1);
            record->candidate_id = (uint64_t)index + UINT64_C(1);
            record->case_id = copy_text(case_ids[case_index]);
            record->availability = available ? HWA_GAP_REPORT_AVAILABLE :
                HWA_GAP_REPORT_UNAVAILABLE;
            record->value = available ? case_values[index] : 0.0;
            record->confidence = available ? confidences[index] : 0.0;
            record->value_valid = available;
            record->confidence_valid = available;
            record->reason = copy_text(available ? "" : "case-unavailable");
            if (record->case_id == NULL || record->reason == NULL) return -1;
        }
    }
    if (result->sources[0].name == NULL ||
        result->sources[0].path == NULL ||
        hwa_gap_report_result_rebuild(result, error, sizeof(error)) != 0 ||
        hwa_gap_report_result_retained_bytes(
            result, &result->retained_work_bytes) != 0 ||
        hwa_gap_report_result_validate(result, error, sizeof(error)) != 0)
        return -1;
    return 0;
}

int main(void)
{
    HWAGapReportResult result;
    HWAGapReportResult full;
    FILE *stream;
    char directory[256];
    char audio[512];
    char path[512];
    char error[HWA_ERROR_SIZE];
    char *json;
    char *text;
    char *html;
    char *csv;
    char *saved;
    char *canonical_csv = NULL;
    memset(&result, 0, sizeof(result));
    memset(&full, 0, sizeof(full));
    CHECK(make_result(&result, HWA_GAP_REPORT_RANK, NULL) == 0,
          "rank fixture");
    stream = tmpfile();
    CHECK(stream != NULL && hwa_gap_report_json(stream, &result) == 0,
          "schema 11 JSON");
    json = read_stream(stream);
    CHECK(json != NULL &&
              strstr(json, "\"schema_version\":11") != NULL &&
              strstr(json, "\"settings\":{") != NULL &&
              strstr(json, "\"measurement\":{") != NULL &&
              strstr(json, "\"production\":{") != NULL &&
              strstr(json, "\"experiment\":{") != NULL &&
              strstr(json, "\"manifest_sha256\":") != NULL &&
              strstr(json, "manifest_path") == NULL &&
              strstr(json, "output_directory") == NULL,
          "JSON schema or canonical projection");
    {
        const char *candidates = strstr(json, "\"candidates\":[");
        const char *rank_one = candidates == NULL ? NULL :
            strstr(candidates, "\"rank\":1,\"quality_flags\"");
        const char *rank_two = candidates == NULL ? NULL :
            strstr(candidates, "\"rank\":2,\"quality_flags\"");
        const char *rank_three = candidates == NULL ? NULL :
            strstr(candidates, "\"rank\":3,\"quality_flags\"");
        const char *candidate_end = candidates == NULL ? NULL :
            strstr(candidates, "],\"families\":[");
        const char *worst = strstr(json, "\"worst_cases\":[");
        const char *worst_one = worst == NULL ? NULL :
            strstr(worst, "\"case_id\":\"case-a\"");
        const char *worst_two = worst == NULL ? NULL :
            strstr(worst, "\"case_id\":\"case-c\"");
        const char *worst_three = worst == NULL ? NULL :
            strstr(worst, "\"case_id\":\"case-b\"");
        const char *worst_end = worst == NULL ? NULL :
            strstr(worst, "],\"clips\":[");
        CHECK(candidates != NULL && rank_one != NULL && rank_two != NULL &&
                  rank_three != NULL && candidate_end != NULL &&
                  rank_one < rank_two && rank_two < rank_three &&
                  rank_three < candidate_end,
              "JSON candidates are not in rank order");
        CHECK(worst != NULL && worst_one != NULL && worst_two != NULL &&
                  worst_three != NULL && worst_end != NULL &&
                  worst_one < worst_two && worst_two < worst_three &&
                  worst_three < worst_end,
              "JSON worst cases are not selected by case score");
    }
    free(json);
    CHECK(fclose(stream) == 0, "close JSON");
    stream = tmpfile();
    CHECK(stream != NULL && hwa_gap_report_text(stream, &result) == 0,
          "rank text");
    text = read_stream(stream);
    CHECK(text != NULL && strstr(text, "\\x0a") != NULL &&
              strstr(text, "\\x1b") != NULL &&
              strchr(text, '\033') == NULL,
          "text control escaping");
    CHECK(strstr(text, "\tband:0\t") != NULL &&
              strstr(text, "\tlevel\t") != NULL &&
              strstr(text, "\tcrest\t") != NULL &&
              strstr(text, "\tlevel\t") < strstr(text, "\tband:0\t") &&
              strstr(text, "\tband:0\t") < strstr(text, "\tcrest\t"),
          "text candidates are not in rank order");
    free(text);
    CHECK(fclose(stream) == 0, "close text");
    result.options.max_output_file_bytes = 1U;
    stream = tmpfile();
    CHECK(stream != NULL && hwa_gap_report_json(stream, &result) != 0,
          "tiny JSON cap was rendered");
    json = read_stream(stream);
    CHECK(json != NULL && json[0] == '\0',
          "tiny JSON cap wrote partial output");
    free(json);
    CHECK(fclose(stream) == 0, "close bounded JSON");
    hwa_gap_report_result_free(&result);

    CHECK(make_directory(directory) == 0 &&
              join(audio, directory, "audio") == 0 &&
              HWA_TEST_MKDIR(audio) == 0 &&
              make_result(&full, HWA_GAP_REPORT_FULL, directory) == 0,
          "full fixture");
    CHECK(hwa_gap_report_output_write_tree(
              directory, HWA_GAP_REPORT_FULL, &full,
              0U,
              error, sizeof(error)) == 0,
          "full report tree");
    CHECK(join(path, directory, "report.html") == 0 &&
              (html = read_file(path)) != NULL &&
              strstr(html, "&lt;script&gt;alert(1)&lt;/script&gt;") != NULL &&
              strstr(html, "<script>alert(1)</script>") == NULL &&
              strstr(html, "http://") == NULL &&
              strstr(html, "https://") == NULL &&
              strstr(html, "All settings") != NULL &&
              strstr(html, full.manifest_sha256) != NULL,
          "self-contained escaped HTML");
    {
        const char *rank_one = strstr(html, "<rect data-rank=\"1\"");
        const char *rank_two = strstr(html, "<rect data-rank=\"2\"");
        const char *rank_three = strstr(html, "<rect data-rank=\"3\"");
        const char *groups = strstr(html, "Six-axis group statistics");
        const char *worst = strstr(html, "Worst matched cases");
        const char *worst_one = worst == NULL ? NULL :
            strstr(worst, "case-a");
        const char *worst_two = worst == NULL ? NULL :
            strstr(worst, "case-c");
        const char *worst_three = worst == NULL ? NULL :
            strstr(worst, "case-b");
        CHECK(rank_one != NULL && rank_two != NULL && rank_three != NULL &&
                  rank_one < rank_two && rank_two < rank_three,
              "HTML chart did not use exact rank order");
        CHECK(groups != NULL && worst != NULL && groups < worst &&
                  strstr(groups, "<th>q05</th>") != NULL &&
                  strstr(groups, "<th>q25</th>") != NULL &&
                  strstr(groups, "<th>Median</th>") != NULL &&
                  strstr(groups, "<th>q75</th>") != NULL &&
                  strstr(groups, "<th>q95</th>") != NULL &&
                  strstr(groups, "<th>Spread</th>") != NULL &&
                  strstr(groups, "<th>Confidence</th>") != NULL &&
                  strstr(groups, "<td>pitch</td>") != NULL &&
                  strstr(groups, "<td>register</td>") != NULL &&
                  strstr(groups, "<td>dynamic</td>") != NULL &&
                  strstr(groups, "<td>gesture</td>") != NULL &&
                  strstr(groups, "<td>physical-element</td>") != NULL &&
                  strstr(groups, "<td>section</td>") != NULL,
              "HTML omitted six-axis group tails, counts, or confidence");
        CHECK(worst_one != NULL && worst_two != NULL &&
                  worst_three != NULL && worst_one < worst_two &&
                  worst_two < worst_three,
              "HTML worst cases are not in case-score order");
    }
    free(html);
    CHECK(join(path, directory, "report.csv") == 0 &&
              (csv = read_file(path)) != NULL &&
              join(path, directory, "result.hwa-report") == 0 &&
              (saved = read_file(path)) != NULL && strcmp(csv, saved) == 0,
          "canonical CSV equality");
    canonical_csv = copy_text(csv);
    free(csv);
    free(saved);
    CHECK(hwa_gap_report_output_remove(&full, error, sizeof(error)) == 0,
          "exact tree removal");
    hwa_gap_report_result_free(&full);

    CHECK(make_directory(directory) == 0 &&
              join(audio, directory, "audio") == 0 &&
              HWA_TEST_MKDIR(audio) == 0 &&
              make_result(&full, HWA_GAP_REPORT_FULL, directory) == 0 &&
              hwa_gap_report_output_write_tree(
                  directory, HWA_GAP_REPORT_FULL, &full,
                  0U,
                  error, sizeof(error)) == 0 &&
              join(path, directory, "report.csv") == 0 &&
              write_text(path, "same tree, changed report\n") == 0,
          "tampered report fixture");
    CHECK(hwa_gap_report_output_remove(&full, error, sizeof(error)) != 0 &&
              exists(path),
          "tampered report caused partial removal");
    CHECK(write_text(path, canonical_csv) == 0,
          "restore canonical report");
    CHECK(join(path, directory, "unexpected.txt") == 0 &&
              write_text(path, "extra\n") == 0 &&
              hwa_gap_report_output_remove(
                  &full, error, sizeof(error)) != 0 && exists(path),
          "extra file caused partial removal");
    CHECK(remove(path) == 0 &&
              hwa_gap_report_output_remove(
                  &full, error, sizeof(error)) == 0,
          "remove restored exact tree");
    hwa_gap_report_result_free(&full);
    free(canonical_csv);

    CHECK(make_result(&full, HWA_GAP_REPORT_FULL, ".") == 0 &&
              hwa_gap_report_output_remove(
                  &full, error, sizeof(error)) != 0,
          "loaded inert path removal rejection");
    hwa_gap_report_result_free(&full);
    return 0;
}

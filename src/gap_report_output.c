#if !defined(_WIN32)
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "gap_report_output.h"

#include "gap_report_file.h"
#include "numeric_locale.h"
#include "sha256.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

typedef struct HWAGROutIdentity {
    uint64_t device;
    uint64_t file;
    uint64_t size;
    int64_t modified_seconds;
    int64_t modified_nanoseconds;
    int64_t changed_seconds;
    int64_t changed_nanoseconds;
} HWAGROutIdentity;

typedef enum HWAGRRenderKind {
    HWA_GR_RENDER_TEXT = 1,
    HWA_GR_RENDER_JSON = 2,
    HWA_GR_RENDER_CSV = 3,
    HWA_GR_RENDER_HTML = 4
} HWAGRRenderKind;

static void hwa_gro_error(char *error,
                          size_t error_size,
                          const char *message)
{
    if (error != NULL && error_size != 0U)
        (void)snprintf(error, error_size, "%s", message);
}

static int hwa_gro_bound_add(uint64_t *total, uint64_t value)
{
    if (value > UINT64_MAX - *total) return -1;
    *total += value;
    return 0;
}

static int hwa_gro_bound_rows(uint64_t *total,
                              size_t count,
                              uint64_t per_row)
{
    if ((uint64_t)count > UINT64_MAX / per_row) return -1;
    return hwa_gro_bound_add(total, (uint64_t)count * per_row);
}

static int hwa_gro_bound_text(uint64_t *total, const char *text)
{
    size_t length = text == NULL ? 0U : strlen(text);
#if SIZE_MAX > (UINT64_MAX - UINT64_C(6)) / UINT64_C(6)
    if (length >
        (size_t)((UINT64_MAX - UINT64_C(6)) / UINT64_C(6)))
        return -1;
#endif
    return hwa_gro_bound_add(
        total, (uint64_t)length * UINT64_C(6) + UINT64_C(6));
}

static int hwa_gro_render_minimum(const HWAGapReportResult *result,
                                  uint64_t *bytes)
{
    uint64_t total = UINT64_C(2);
    if (hwa_gro_bound_rows(&total, result->source_count, 2U) != 0 ||
        hwa_gro_bound_rows(&total, result->label_count, 2U) != 0 ||
        hwa_gro_bound_rows(&total, result->candidate_count, 2U) != 0 ||
        hwa_gro_bound_rows(&total, result->family_count, 2U) != 0 ||
        hwa_gro_bound_rows(&total, result->group_count, 2U) != 0 ||
        hwa_gro_bound_rows(&total, result->case_count, 2U) != 0 ||
        hwa_gro_bound_rows(&total, result->excerpt_count, 2U) != 0 ||
        hwa_gro_bound_rows(&total, result->warning_count, 2U) != 0)
        return -1;
    *bytes = total;
    return 0;
}

/*
 * HTML entities and JSON escapes use at most six bytes per input byte. Fixed
 * row allowances cover field names plus 63-byte numeric renderings. The bound
 * lets all scratch-file paths reject small caps before tmpfile() is opened.
 */
static int hwa_gro_render_upper_for_audio(
    const HWAGapReportResult *result,
    HWAGRRenderKind kind,
    uint64_t html_audio_bytes,
    uint64_t *bytes)
{
    uint64_t total = kind == HWA_GR_RENDER_TEXT
        ? UINT64_C(4096) : UINT64_C(32768);
    size_t index;
    if (hwa_gro_bound_text(&total, result->title) != 0 ||
        (kind == HWA_GR_RENDER_HTML &&
         hwa_gro_bound_text(&total, result->title) != 0) ||
        hwa_gro_bound_text(&total, result->audibility_method) != 0 ||
        hwa_gro_bound_rows(&total, result->source_count, 512U) != 0 ||
        hwa_gro_bound_rows(&total, result->label_count, 512U) != 0 ||
        hwa_gro_bound_rows(&total, result->candidate_count, 1536U) != 0 ||
        hwa_gro_bound_rows(&total, result->family_count, 512U) != 0 ||
        hwa_gro_bound_rows(&total, result->group_count, 1536U) != 0 ||
        hwa_gro_bound_rows(&total, result->case_count, 768U) != 0 ||
        hwa_gro_bound_rows(&total, result->excerpt_count, 2048U) != 0 ||
        hwa_gro_bound_rows(&total, result->warning_count, 768U) != 0)
        return -1;
    for (index = 0U; index < result->source_count; ++index)
        if (hwa_gro_bound_text(&total, result->sources[index].name) != 0)
            return -1;
    for (index = 0U; index < result->label_count; ++index) {
        const HWAGapReportLabel *r = &result->labels[index];
        if (hwa_gro_bound_text(&total, r->case_id) != 0 ||
            hwa_gro_bound_text(&total, r->pitch) != 0 ||
            hwa_gro_bound_text(&total, r->register_name) != 0 ||
            hwa_gro_bound_text(&total, r->dynamic) != 0 ||
            hwa_gro_bound_text(&total, r->gesture) != 0 ||
            hwa_gro_bound_text(&total, r->physical_element) != 0 ||
            hwa_gro_bound_text(&total, r->section) != 0) return -1;
    }
    for (index = 0U; index < result->candidate_count; ++index) {
        const HWAGapReportCandidate *r = &result->candidates[index];
        if (hwa_gro_bound_text(&total, r->case_id) != 0 ||
            hwa_gro_bound_text(&total, r->metric) != 0 ||
            hwa_gro_bound_text(&total, r->metric) != 0 ||
            hwa_gro_bound_text(&total, r->metric) != 0 ||
            hwa_gro_bound_text(&total, r->family_key) != 0 ||
            hwa_gro_bound_text(&total, r->reason) != 0 ||
            hwa_gro_bound_text(&total, r->reason) != 0) return -1;
    }
    for (index = 0U; index < result->family_count; ++index)
        if (hwa_gro_bound_text(&total, result->families[index].key) != 0)
            return -1;
    for (index = 0U; index < result->group_count; ++index)
        if (hwa_gro_bound_text(&total, result->groups[index].value) != 0)
            return -1;
    for (index = 0U; index < result->case_count; ++index) {
        const HWAGapReportCase *r = &result->cases[index];
        if (hwa_gro_bound_text(&total, r->case_id) != 0 ||
            hwa_gro_bound_text(&total, r->reason) != 0) return -1;
        if ((kind == HWA_GR_RENDER_JSON || kind == HWA_GR_RENDER_HTML) &&
            r->availability == HWA_GAP_REPORT_AVAILABLE && r->score_valid) {
            if (hwa_gro_bound_text(&total, r->case_id) != 0 ||
                hwa_gro_bound_text(&total, r->reason) != 0)
                return -1;
            if (kind == HWA_GR_RENDER_HTML) {
                if (r->candidate_id == 0U ||
                    r->candidate_id > (uint64_t)result->candidate_count ||
                    hwa_gro_bound_text(
                        &total,
                        result->candidates[r->candidate_id - 1U].metric) != 0)
                    return -1;
            }
        }
    }
    for (index = 0U; index < result->excerpt_count; ++index) {
        const HWAGapReportExcerpt *r = &result->excerpts[index];
        if (hwa_gro_bound_text(&total, r->name) != 0 ||
            hwa_gro_bound_text(&total, r->reference_path) != 0 ||
            hwa_gro_bound_text(&total, r->model_path) != 0 ||
            hwa_gro_bound_text(&total, r->x_path) != 0 ||
            hwa_gro_bound_text(&total, r->reason) != 0 ||
            hwa_gro_bound_text(&total, r->reason) != 0) return -1;
    }
    for (index = 0U; index < result->warning_count; ++index) {
        const HWAGapReportWarning *r = &result->warnings[index];
        if (hwa_gro_bound_text(&total, r->code) != 0 ||
            hwa_gro_bound_text(&total, r->message) != 0) return -1;
    }
    if (kind == HWA_GR_RENDER_HTML) {
        uint64_t groups;
        uint64_t encoded;
        if (html_audio_bytes > UINT64_MAX - UINT64_C(2))
            return -1;
        groups = (html_audio_bytes + UINT64_C(2)) / UINT64_C(3);
        if (groups > UINT64_MAX / UINT64_C(4)) return -1;
        encoded = groups * UINT64_C(4);
        if (hwa_gro_bound_add(&total, encoded) != 0 ||
            hwa_gro_bound_rows(
                &total, result->excerpt_count, UINT64_C(12)) != 0)
            return -1;
    }
    *bytes = total;
    return 0;
}

static int hwa_gro_render_upper(const HWAGapReportResult *result,
                                HWAGRRenderKind kind,
                                uint64_t *bytes)
{
    return hwa_gro_render_upper_for_audio(
        result, kind, result->total_output_bytes, bytes);
}

static int hwa_gro_preflight(const HWAGapReportResult *result,
                             HWAGRRenderKind kind,
                             uint64_t maximum)
{
    uint64_t minimum;
    uint64_t upper;
    return result != NULL &&
           hwa_gro_render_minimum(result, &minimum) == 0 &&
           minimum <= maximum &&
           hwa_gro_render_upper(result, kind, &upper) == 0 &&
           upper <= maximum
               ? 0 : -1;
}

static int hwa_gro_work_preflight(const HWAGapReportResult *result,
                                  uint64_t live_work)
{
    uint64_t extra;
    uint64_t peak;
    if (result == NULL || live_work < result->retained_work_bytes ||
        live_work > result->options.max_work_bytes)
        return -1;
    extra = live_work - result->retained_work_bytes;
    return hwa_gap_report_result_peak_work_bytes(result, extra, &peak) == 0 &&
           peak <= result->options.max_work_bytes
        ? 0 : -1;
}

static int hwa_gro_arrays_present(const HWAGapReportResult *result)
{
    return result != NULL &&
           (result->source_count == 0U || result->sources != NULL) &&
           (result->label_count == 0U || result->labels != NULL) &&
           (result->candidate_count == 0U || result->candidates != NULL) &&
           (result->family_count == 0U || result->families != NULL) &&
           (result->group_count == 0U || result->groups != NULL) &&
           (result->case_count == 0U || result->cases != NULL) &&
           (result->excerpt_count == 0U || result->excerpts != NULL) &&
           (result->warning_count == 0U || result->warnings != NULL);
}

static int hwa_gro_projected_path_extra(
    const HWAGapReportResult *result,
    uint64_t *bytes)
{
    uint64_t total = 0U;
    size_t index;
    for (index = 0U; index < result->excerpt_count; ++index) {
        const HWAGapReportExcerpt *r = &result->excerpts[index];
        size_t length = r->name == NULL ? 0U : strlen(r->name);
        uint64_t path_bytes;
        uint64_t roles = r->make_x ? UINT64_C(3) : UINT64_C(2);
#if SIZE_MAX >= UINT64_MAX
        if (length > (size_t)(UINT64_MAX - UINT64_C(12))) return -1;
#endif
        path_bytes = (uint64_t)length + UINT64_C(12);
        if (path_bytes > UINT64_MAX / UINT64_C(6) ||
            path_bytes * UINT64_C(6) > UINT64_MAX / roles ||
            hwa_gro_bound_add(
                &total, path_bytes * UINT64_C(6) * roles) != 0)
            return -1;
    }
    *bytes = total;
    return 0;
}

int hwa_gap_report_output_projected_upper(
    const HWAGapReportResult *result,
    HWAGapReportMode mode,
    uint64_t projected_audio_bytes,
    uint64_t *tree_bytes)
{
    uint64_t paths;
    uint64_t csv;
    uint64_t json = 0U;
    uint64_t html = 0U;
    uint64_t total = projected_audio_bytes;
    if (result == NULL || tree_bytes == NULL ||
        (mode != HWA_GAP_REPORT_EXCERPTS && mode != HWA_GAP_REPORT_FULL) ||
        hwa_gro_projected_path_extra(result, &paths) != 0 ||
        hwa_gro_render_upper_for_audio(
            result, HWA_GR_RENDER_CSV, projected_audio_bytes, &csv) != 0 ||
        hwa_gro_bound_add(&csv, paths) != 0 ||
        csv > result->options.max_output_file_bytes ||
        hwa_gro_bound_add(&total, csv) != 0)
        return -1;
    if (mode == HWA_GAP_REPORT_FULL) {
        if (hwa_gro_render_upper_for_audio(
                result, HWA_GR_RENDER_JSON,
                projected_audio_bytes, &json) != 0 ||
            hwa_gro_render_upper_for_audio(
                result, HWA_GR_RENDER_HTML,
                projected_audio_bytes, &html) != 0 ||
            hwa_gro_bound_add(&json, paths) != 0 ||
            hwa_gro_bound_add(&html, paths) != 0 ||
            json > result->options.max_output_file_bytes ||
            html > result->options.max_output_file_bytes ||
            hwa_gro_bound_add(&total, csv) != 0 ||
            hwa_gro_bound_add(&total, json) != 0 ||
            hwa_gro_bound_add(&total, html) != 0)
            return -1;
    }
    *tree_bytes = total;
    return 0;
}

static int hwa_gro_json_string(FILE *stream, const char *text)
{
    const unsigned char *bytes =
        (const unsigned char *)(text != NULL ? text : "");
    size_t index;
    if (fputc('"', stream) == EOF) return -1;
    for (index = 0U; bytes[index] != 0U; ++index) {
        unsigned char c = bytes[index];
        if (c == '"' || c == '\\') {
            if (fputc('\\', stream) == EOF || fputc(c, stream) == EOF)
                return -1;
        } else if (c == '\b') {
            if (fputs("\\b", stream) == EOF) return -1;
        } else if (c == '\f') {
            if (fputs("\\f", stream) == EOF) return -1;
        } else if (c == '\n') {
            if (fputs("\\n", stream) == EOF) return -1;
        } else if (c == '\r') {
            if (fputs("\\r", stream) == EOF) return -1;
        } else if (c == '\t') {
            if (fputs("\\t", stream) == EOF) return -1;
        } else if (c < 0x20U || c >= 0x80U) {
            if (fprintf(stream, "\\u%04x", (unsigned)c) < 0) return -1;
        } else if (fputc(c, stream) == EOF) return -1;
    }
    return fputc('"', stream) == EOF ? -1 : 0;
}

static int hwa_gro_html_text(FILE *stream, const char *text)
{
    const unsigned char *bytes =
        (const unsigned char *)(text != NULL ? text : "");
    size_t index;
    for (index = 0U; bytes[index] != 0U; ++index) {
        unsigned char c = bytes[index];
        if (c == '&') {
            if (fputs("&amp;", stream) == EOF) return -1;
        } else if (c == '<') {
            if (fputs("&lt;", stream) == EOF) return -1;
        } else if (c == '>') {
            if (fputs("&gt;", stream) == EOF) return -1;
        } else if (c == '"') {
            if (fputs("&quot;", stream) == EOF) return -1;
        } else if (c == '\'') {
            if (fputs("&#39;", stream) == EOF) return -1;
        } else if (c < 0x20U || c >= 0x7fU) {
            if (fprintf(stream, "&#%u;", (unsigned)c) < 0) return -1;
        } else if (fputc(c, stream) == EOF) return -1;
    }
    return 0;
}

static int hwa_gro_text(FILE *stream, const char *text)
{
    const unsigned char *bytes =
        (const unsigned char *)(text != NULL ? text : "");
    size_t index;
    static const char digits[] = "0123456789abcdef";
    for (index = 0U; bytes[index] != 0U; ++index) {
        unsigned char c = bytes[index];
        if (c >= 0x20U && c <= 0x7eU && c != '\\') {
            if (fputc(c, stream) == EOF) return -1;
        } else {
            if (fputc('\\', stream) == EOF) return -1;
            if (c == '\\') {
                if (fputc('\\', stream) == EOF) return -1;
            } else if (fputc('x', stream) == EOF ||
                       fputc(digits[c >> 4U], stream) == EOF ||
                       fputc(digits[c & 15U], stream) == EOF) {
                return -1;
            }
        }
    }
    return 0;
}

static int hwa_gro_number(FILE *stream,
                          const HWANumericLocale *locale,
                          double value)
{
    char text[64];
    if (!isfinite(value) ||
        hwa_c_locale_format_double(
            locale, text, sizeof(text),
            value == 0.0 ? 0.0 : value) != 0) return -1;
    return fputs(text, stream) == EOF ? -1 : 0;
}

static int hwa_gro_optional_number(FILE *stream,
                                   const HWANumericLocale *locale,
                                   double value,
                                   int valid)
{
    return valid ? hwa_gro_number(stream, locale, value)
                 : (fputs("null", stream) == EOF ? -1 : 0);
}

static int hwa_gro_candidate_order(
    const HWAGapReportResult *result,
    const HWAGapReportCandidate ***ordered)
{
    const HWAGapReportCandidate **rows = NULL;
    size_t ranked_count = 0U;
    size_t next;
    size_t index;
    uint64_t bytes;
    *ordered = NULL;
    if (result->candidate_count == 0U) return 0;
    if (result->candidate_count > SIZE_MAX / sizeof(*rows)) return -1;
    bytes = (uint64_t)(result->candidate_count * sizeof(*rows));
    if (result->retained_work_bytes > result->options.max_work_bytes ||
        bytes > result->options.max_work_bytes -
                    result->retained_work_bytes) return -1;
    rows = (const HWAGapReportCandidate **)calloc(
        result->candidate_count, sizeof(*rows));
    if (rows == NULL) return -1;
    for (index = 0U; index < result->candidate_count; ++index) {
        const HWAGapReportCandidate *r = &result->candidates[index];
        if (r->primary && r->score_valid && r->rank != 0U) {
            if (r->rank > result->candidate_count ||
                rows[r->rank - 1U] != NULL) goto failed;
            rows[r->rank - 1U] = r;
            if (r->rank > ranked_count) ranked_count = r->rank;
        }
    }
    for (index = 0U; index < ranked_count; ++index)
        if (rows[index] == NULL) goto failed;
    next = ranked_count;
    for (index = 0U; index < result->candidate_count; ++index) {
        const HWAGapReportCandidate *r = &result->candidates[index];
        if (!(r->primary && r->score_valid && r->rank != 0U))
            rows[next++] = r;
    }
    if (next != result->candidate_count) goto failed;
    *ordered = rows;
    return 0;
failed:
    free(rows);
    return -1;
}

static int hwa_gro_case_pointer_compare(const void *left,
                                        const void *right)
{
    const HWAGapReportCase *a =
        *(const HWAGapReportCase *const *)left;
    const HWAGapReportCase *b =
        *(const HWAGapReportCase *const *)right;
    double av = fabs(a->value);
    double bv = fabs(b->value);
    if (a->score != b->score) return a->score > b->score ? -1 : 1;
    if (av != bv) return av > bv ? -1 : 1;
    if (a->confidence != b->confidence)
        return a->confidence > b->confidence ? -1 : 1;
    if (a->candidate_id != b->candidate_id)
        return a->candidate_id < b->candidate_id ? -1 : 1;
    {
        int order = strcmp(a->case_id, b->case_id);
        if (order != 0) return order;
    }
    return a->id < b->id ? -1 : a->id > b->id ? 1 : 0;
}

static int hwa_gro_worst_cases(
    const HWAGapReportResult *result,
    const HWAGapReportCase ***ordered,
    size_t *ordered_count)
{
    const HWAGapReportCase **rows = NULL;
    size_t count = 0U;
    size_t index;
    uint64_t bytes;
    *ordered = NULL;
    *ordered_count = 0U;
    if (result->case_count == 0U) return 0;
    if (result->case_count > SIZE_MAX / sizeof(*rows)) return -1;
    bytes = (uint64_t)(result->case_count * sizeof(*rows));
    if (result->retained_work_bytes > result->options.max_work_bytes ||
        bytes > result->options.max_work_bytes -
                    result->retained_work_bytes) return -1;
    rows = (const HWAGapReportCase **)malloc(
        result->case_count * sizeof(*rows));
    if (rows == NULL) return -1;
    for (index = 0U; index < result->case_count; ++index) {
        const HWAGapReportCase *r = &result->cases[index];
        if (r->availability == HWA_GAP_REPORT_AVAILABLE && r->score_valid)
            rows[count++] = r;
    }
    if (count != 0U)
        qsort(rows, count, sizeof(*rows), hwa_gro_case_pointer_compare);
    *ordered = rows;
    *ordered_count = count < 20U ? count : 20U;
    return 0;
}

static int hwa_gro_json_source(FILE *s, const HWAGapReportSource *r)
{
    if (fprintf(s, "{\"id\":%" PRIu64 ",\"name\":", r->id) < 0 ||
        hwa_gro_json_string(s, r->name) != 0 ||
        fputs(",\"kind\":", s) == EOF ||
        hwa_gro_json_string(
            s, hwa_gap_report_source_kind_name(r->kind)) != 0 ||
        fputs(",\"sha256\":", s) == EOF ||
        hwa_gro_json_string(s, r->sha256) != 0 ||
        fprintf(s, ",\"file_bytes\":%" PRIu64
                ",\"candidate_count\":%zu}",
                r->file_bytes, r->candidate_count) < 0) {
        return -1;
    }
    return 0;
}

static int hwa_gro_json_label(FILE *s, const HWAGapReportLabel *r)
{
    if (fprintf(s, "{\"id\":%" PRIu64 ",\"source_id\":%" PRIu64
                ",\"case_id\":", r->id, r->source_id) < 0 ||
        hwa_gro_json_string(s, r->case_id) != 0 ||
        fputs(",\"pitch\":", s) == EOF ||
        hwa_gro_json_string(s, r->pitch) != 0 ||
        fputs(",\"register\":", s) == EOF ||
        hwa_gro_json_string(s, r->register_name) != 0 ||
        fputs(",\"dynamic\":", s) == EOF ||
        hwa_gro_json_string(s, r->dynamic) != 0 ||
        fputs(",\"gesture\":", s) == EOF ||
        hwa_gro_json_string(s, r->gesture) != 0 ||
        fputs(",\"physical_element\":", s) == EOF ||
        hwa_gro_json_string(s, r->physical_element) != 0 ||
        fputs(",\"section\":", s) == EOF ||
        hwa_gro_json_string(s, r->section) != 0 ||
        fputc('}', s) == EOF) return -1;
    return 0;
}

static int hwa_gro_json_candidate(
    FILE *s,
    const HWANumericLocale *locale,
    const HWAGapReportCandidate *r)
{
    if (fprintf(s, "{\"id\":%" PRIu64 ",\"source_id\":%" PRIu64
                ",\"source_row\":%" PRIu64 ",\"case_id\":",
                r->id, r->source_id, r->source_row) < 0 ||
        hwa_gro_json_string(s, r->case_id) != 0 ||
        fputs(",\"metric\":", s) == EOF ||
        hwa_gro_json_string(s, r->metric) != 0 ||
        fputs(",\"family_key\":", s) == EOF ||
        hwa_gro_json_string(s, r->family_key) != 0 ||
        fputs(",\"kind\":", s) == EOF ||
        hwa_gro_json_string(
            s, hwa_gap_report_candidate_kind_name(r->kind)) != 0 ||
        fputs(",\"availability\":", s) == EOF ||
        hwa_gro_json_string(
            s, hwa_gap_report_availability_name(r->availability)) != 0 ||
        fputs(",\"raw_value\":", s) == EOF ||
        hwa_gro_optional_number(
            s, locale, r->raw_value, r->raw_value_valid) != 0 ||
        fputs(",\"size_factor\":", s) == EOF ||
        hwa_gro_optional_number(
            s, locale, r->size_factor, r->size_valid) != 0 ||
        fputs(",\"audibility_factor\":", s) == EOF ||
        hwa_gro_optional_number(
            s, locale, r->audibility_factor, r->audibility_valid) != 0 ||
        fprintf(s, ",\"occurrence_count\":%" PRIu64
                ",\"eligible_count\":%" PRIu64 ",\"occurrence_factor\":",
                r->occurrence_count, r->eligible_count) < 0 ||
        hwa_gro_optional_number(
            s, locale, r->occurrence_factor, r->occurrence_valid) != 0 ||
        fputs(",\"confidence_factor\":", s) == EOF ||
        hwa_gro_optional_number(
            s, locale, r->confidence_factor, r->confidence_valid) != 0 ||
        fputs(",\"score\":", s) == EOF ||
        hwa_gro_optional_number(s, locale, r->score, r->score_valid) != 0 ||
        fprintf(s, ",\"linked_family_id\":%" PRIu64
                ",\"rank\":%zu,\"quality_flags\":%" PRIu32
                ",\"primary\":%s,\"reason\":",
                r->linked_family_id, r->rank, r->quality_flags,
                r->primary ? "true" : "false") < 0 ||
        hwa_gro_json_string(s, r->reason) != 0 ||
        fputc('}', s) == EOF) return -1;
    return 0;
}

static int hwa_gro_json_candidates(FILE *stream,
                                   const HWANumericLocale *locale,
                                   const HWAGapReportResult *result)
{
    const HWAGapReportCandidate **ordered = NULL;
    size_t index;
    int status = -1;
    if (hwa_gro_candidate_order(result, &ordered) != 0) return -1;
    for (index = 0U; index < result->candidate_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_gro_json_candidate(stream, locale, ordered[index]) != 0)
            goto cleanup;
    }
    status = 0;
cleanup:
    free(ordered);
    return status;
}

static int hwa_gro_json_family(FILE *s, const HWAGapReportFamily *r)
{
    if (fprintf(s, "{\"id\":%" PRIu64 ",\"key\":", r->id) < 0 ||
        hwa_gro_json_string(s, r->key) != 0 ||
        fprintf(s, ",\"primary_candidate_id\":%" PRIu64
                ",\"member_count\":%zu,\"rank\":%zu}",
                r->primary_candidate_id, r->member_count, r->rank) < 0) {
        return -1;
    }
    return 0;
}

static int hwa_gro_json_group(FILE *s,
                              const HWANumericLocale *locale,
                              const HWAGapReportGroup *r)
{
    if (fprintf(s, "{\"id\":%" PRIu64 ",\"axis\":", r->id) < 0 ||
        hwa_gro_json_string(s, hwa_gap_report_axis_name(r->axis)) != 0 ||
        fputs(",\"value\":", s) == EOF ||
        hwa_gro_json_string(s, r->value) != 0 ||
        fprintf(s, ",\"family_count\":%zu,\"candidate_count\":%zu"
                ",\"available_count\":%zu,\"missing_count\":%zu"
                ",\"excluded_count\":%zu,\"q05\":",
                r->family_count, r->candidate_count, r->available_count,
                r->missing_count, r->excluded_count) < 0 ||
        hwa_gro_optional_number(
            s, locale, r->q05, r->statistics_valid) != 0 ||
        fputs(",\"q25\":", s) == EOF ||
        hwa_gro_optional_number(
            s, locale, r->q25, r->statistics_valid) != 0 ||
        fputs(",\"median\":", s) == EOF ||
        hwa_gro_optional_number(
            s, locale, r->median, r->statistics_valid) != 0 ||
        fputs(",\"q75\":", s) == EOF ||
        hwa_gro_optional_number(
            s, locale, r->q75, r->statistics_valid) != 0 ||
        fputs(",\"q95\":", s) == EOF ||
        hwa_gro_optional_number(
            s, locale, r->q95, r->statistics_valid) != 0 ||
        fputs(",\"spread\":", s) == EOF ||
        hwa_gro_optional_number(
            s, locale, r->spread, r->statistics_valid) != 0 ||
        fputs(",\"confidence\":", s) == EOF ||
        hwa_gro_optional_number(
            s, locale, r->confidence, r->confidence_valid) != 0 ||
        fputc('}', s) == EOF) return -1;
    return 0;
}

static int hwa_gro_json_case(FILE *s,
                             const HWANumericLocale *locale,
                             const HWAGapReportCase *r)
{
    if (fprintf(s, "{\"id\":%" PRIu64 ",\"candidate_id\":%" PRIu64
                ",\"case_id\":", r->id, r->candidate_id) < 0 ||
        hwa_gro_json_string(s, r->case_id) != 0 ||
        fputs(",\"availability\":", s) == EOF ||
        hwa_gro_json_string(
            s, hwa_gap_report_availability_name(r->availability)) != 0 ||
        fputs(",\"value\":", s) == EOF ||
        hwa_gro_optional_number(s, locale, r->value, r->value_valid) != 0 ||
        fputs(",\"confidence\":", s) == EOF ||
        hwa_gro_optional_number(
            s, locale, r->confidence, r->confidence_valid) != 0 ||
        fputs(",\"score\":", s) == EOF ||
        hwa_gro_optional_number(s, locale, r->score, r->score_valid) != 0 ||
        fputs(",\"reason\":", s) == EOF ||
        hwa_gro_json_string(s, r->reason) != 0 ||
        fputc('}', s) == EOF) return -1;
    return 0;
}

static int hwa_gro_json_worst_cases(FILE *stream,
                                    const HWANumericLocale *locale,
                                    const HWAGapReportResult *result)
{
    const HWAGapReportCase **ordered = NULL;
    size_t count = 0U;
    size_t index;
    int status = -1;
    if (hwa_gro_worst_cases(result, &ordered, &count) != 0) return -1;
    for (index = 0U; index < count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_gro_json_case(stream, locale, ordered[index]) != 0)
            goto cleanup;
    }
    status = 0;
cleanup:
    free(ordered);
    return status;
}

static int hwa_gro_json_excerpt(FILE *s,
                                const HWANumericLocale *locale,
                                const HWAGapReportExcerpt *r)
{
    if (fprintf(s, "{\"id\":%" PRIu64 ",\"name\":", r->id) < 0 ||
        hwa_gro_json_string(s, r->name) != 0 ||
        fprintf(s, ",\"candidate_source_id\":%" PRIu64
                ",\"candidate_row\":%" PRIu64 ",\"view\":",
                r->candidate_source_id, r->candidate_row) < 0 ||
        hwa_gro_json_string(s, hwa_gap_report_view_name(r->view)) != 0 ||
        fprintf(s, ",\"reference_source_id\":%" PRIu64
                ",\"model_source_id\":%" PRIu64
                ",\"reference_start_sample\":%" PRIu64
                ",\"model_start_sample\":%" PRIu64
                ",\"frame_count\":%" PRIu64 ",\"make_x\":%s"
                ",\"availability\":",
                r->reference_source_id, r->model_source_id,
                r->reference_start_sample, r->model_start_sample,
                r->frame_count, r->make_x ? "true" : "false") < 0 ||
        hwa_gro_json_string(
            s, hwa_gap_report_availability_name(r->availability)) != 0 ||
        fputs(",\"reference_path\":", s) == EOF ||
        hwa_gro_json_string(s, r->reference_path) != 0 ||
        fputs(",\"model_path\":", s) == EOF ||
        hwa_gro_json_string(s, r->model_path) != 0 ||
        fputs(",\"x_path\":", s) == EOF ||
        hwa_gro_json_string(s, r->x_path) != 0 ||
        fputs(",\"reference_sha256\":", s) == EOF ||
        hwa_gro_json_string(s, r->reference_sha256) != 0 ||
        fputs(",\"model_sha256\":", s) == EOF ||
        hwa_gro_json_string(s, r->model_sha256) != 0 ||
        fputs(",\"x_sha256\":", s) == EOF ||
        hwa_gro_json_string(s, r->x_sha256) != 0 ||
        fprintf(s, ",\"reference_file_bytes\":%" PRIu64
                ",\"model_file_bytes\":%" PRIu64
                ",\"x_file_bytes\":%" PRIu64 ",\"reference_gain_db\":",
                r->reference_file_bytes, r->model_file_bytes,
                r->x_file_bytes) < 0 ||
        hwa_gro_number(s, locale, r->reference_gain_db) != 0 ||
        fputs(",\"model_gain_db\":", s) == EOF ||
        hwa_gro_number(s, locale, r->model_gain_db) != 0 ||
        fprintf(s, ",\"x_is_reference\":%s,\"reason\":",
                r->x_is_reference ? "true" : "false") < 0 ||
        hwa_gro_json_string(s, r->reason) != 0 ||
        fputc('}', s) == EOF) return -1;
    return 0;
}

static int hwa_gro_json_warning(FILE *s, const HWAGapReportWarning *r)
{
    if (fprintf(s, "{\"id\":%" PRIu64 ",\"code\":", r->id) < 0 ||
        hwa_gro_json_string(s, r->code) != 0 ||
        fputs(",\"message\":", s) == EOF ||
        hwa_gro_json_string(s, r->message) != 0 ||
        fprintf(s, ",\"source_id\":%" PRIu64
                ",\"candidate_id\":%" PRIu64
                ",\"excerpt_id\":%" PRIu64
                ",\"source_id_valid\":%s,\"candidate_id_valid\":%s"
                ",\"excerpt_id_valid\":%s}",
                r->source_id, r->candidate_id, r->excerpt_id,
                r->source_id_valid ? "true" : "false",
                r->candidate_id_valid ? "true" : "false",
                r->excerpt_id_valid ? "true" : "false") < 0) return -1;
    return 0;
}

static int hwa_gro_json_profile_options(
    FILE *s,
    const HWAProfileComparisonOptions *o)
{
    return fprintf(s,
        "{\"max_input_bytes\":%" PRIu64
        ",\"max_work_bytes\":%" PRIu64
        ",\"max_contexts\":%zu,\"max_measurements\":%zu"
        ",\"max_groups\":%zu,\"max_group_members\":%zu"
        ",\"max_statistics\":%zu,\"max_warnings\":%zu"
        ",\"max_distributions\":%zu,\"max_gaps\":%zu}",
        o->max_input_bytes, o->max_work_bytes, o->max_contexts,
        o->max_measurements, o->max_groups, o->max_group_members,
        o->max_statistics, o->max_warnings, o->max_distributions,
        o->max_gaps) < 0 ? -1 : 0;
}

static int hwa_gro_json_run_options(FILE *s, const HWARunOptions *o)
{
    return fprintf(s,
        "{\"decode_block_frames\":%zu,\"max_manifest_bytes\":%" PRIu64
        ",\"max_input_bytes\":%" PRIu64
        ",\"max_input_frames\":%" PRIu64
        ",\"max_probe_bytes\":%" PRIu64
        ",\"max_probe_values\":%" PRIu64
        ",\"max_work_bytes\":%" PRIu64
        ",\"max_evaluations\":%" PRIu64
        ",\"max_stems\":%zu,\"max_probes\":%zu,\"max_links\":%zu"
        ",\"max_json_depth\":%zu,\"max_json_tokens\":%zu"
        ",\"max_result_rows\":%zu,\"max_warnings\":%zu}",
        o->decode_block_frames, o->max_manifest_bytes, o->max_input_bytes,
        o->max_input_frames, o->max_probe_bytes, o->max_probe_values,
        o->max_work_bytes, o->max_evaluations, o->max_stems, o->max_probes,
        o->max_links, o->max_json_depth, o->max_json_tokens,
        o->max_result_rows, o->max_warnings) < 0 ? -1 : 0;
}

static int hwa_gro_json_production_options(
    FILE *s,
    const HWAProductionOptions *o)
{
    if (fprintf(s,
            "{\"decode_block_frames\":%zu,\"max_input_bytes\":%" PRIu64
            ",\"max_input_frames\":%" PRIu64
            ",\"max_ir_frames\":%" PRIu64
            ",\"max_work_bytes\":%" PRIu64
            ",\"max_evaluations\":%" PRIu64
            ",\"max_spans\":%zu,\"max_envelope_points\":%zu"
            ",\"max_fits\":%zu,\"max_evaluation_rows\":%zu"
            ",\"max_view_rows\":%zu,\"max_warnings\":%zu"
            ",\"profile_limits\":",
            o->decode_block_frames, o->max_input_bytes, o->max_input_frames,
            o->max_ir_frames, o->max_work_bytes, o->max_evaluations,
            o->max_spans, o->max_envelope_points, o->max_fits,
            o->max_evaluation_rows, o->max_view_rows,
            o->max_warnings) < 0 ||
        hwa_gro_json_profile_options(s, &o->profile_limits) != 0 ||
        fputc('}', s) == EOF) return -1;
    return 0;
}

static int hwa_gro_json_experiment_options(
    FILE *s,
    const HWAExperimentOptions *o)
{
    if (fputs("{\"run\":", s) == EOF ||
        hwa_gro_json_run_options(s, &o->run) != 0 ||
        fprintf(s,
            ",\"max_manifest_bytes\":%" PRIu64
            ",\"max_input_bytes\":%" PRIu64
            ",\"max_work_bytes\":%" PRIu64
            ",\"max_bundle_bytes\":%" PRIu64
            ",\"max_output_file_bytes\":%" PRIu64
            ",\"max_total_run_evaluations\":%" PRIu64
            ",\"max_job_milliseconds\":%" PRIu64
            ",\"max_total_milliseconds\":%" PRIu64
            ",\"max_parameters\":%zu,\"max_levels\":%zu"
            ",\"max_cases\":%zu,\"max_responses\":%zu"
            ",\"max_points\":%zu,\"max_jobs\":%zu"
            ",\"max_replicates\":%zu,\"max_artifacts\":%zu"
            ",\"max_observations\":%zu,\"max_sensitivities\":%zu"
            ",\"max_interactions\":%zu,\"max_warnings\":%zu}",
            o->max_manifest_bytes, o->max_input_bytes, o->max_work_bytes,
            o->max_bundle_bytes, o->max_output_file_bytes,
            o->max_total_run_evaluations, o->max_job_milliseconds,
            o->max_total_milliseconds, o->max_parameters, o->max_levels,
            o->max_cases, o->max_responses, o->max_points, o->max_jobs,
            o->max_replicates, o->max_artifacts, o->max_observations,
            o->max_sensitivities, o->max_interactions,
            o->max_warnings) < 0) return -1;
    return 0;
}

static int hwa_gro_json_options(FILE *s, const HWAGapReportOptions *o)
{
    if (fprintf(s,
            "{\"decode_block_frames\":%zu,\"max_manifest_bytes\":%" PRIu64
            ",\"max_input_bytes\":%" PRIu64
            ",\"max_input_frames\":%" PRIu64
            ",\"max_work_bytes\":%" PRIu64
            ",\"max_evaluations\":%" PRIu64
            ",\"max_output_file_bytes\":%" PRIu64
            ",\"max_bundle_bytes\":%" PRIu64
            ",\"max_excerpt_frames\":%" PRIu64
            ",\"max_total_excerpt_frames\":%" PRIu64
            ",\"max_sources\":%zu,\"max_labels\":%zu"
            ",\"max_candidates\":%zu,\"max_families\":%zu"
            ",\"max_groups\":%zu,\"max_cases\":%zu"
            ",\"max_excerpts\":%zu,\"max_warnings\":%zu"
            ",\"max_json_depth\":%zu,\"max_json_tokens\":%zu"
            ",\"measurement\":",
            o->decode_block_frames, o->max_manifest_bytes,
            o->max_input_bytes, o->max_input_frames, o->max_work_bytes,
            o->max_evaluations, o->max_output_file_bytes,
            o->max_bundle_bytes, o->max_excerpt_frames,
            o->max_total_excerpt_frames, o->max_sources, o->max_labels,
            o->max_candidates, o->max_families, o->max_groups, o->max_cases,
            o->max_excerpts, o->max_warnings, o->max_json_depth,
            o->max_json_tokens) < 0 ||
        hwa_gro_json_profile_options(s, &o->measurement) != 0 ||
        fputs(",\"production\":", s) == EOF ||
        hwa_gro_json_production_options(s, &o->production) != 0 ||
        fputs(",\"run\":", s) == EOF ||
        hwa_gro_json_run_options(s, &o->run) != 0 ||
        fputs(",\"experiment\":", s) == EOF ||
        hwa_gro_json_experiment_options(s, &o->experiment) != 0 ||
        fputc('}', s) == EOF) return -1;
    return 0;
}

static int hwa_gro_json_impl(FILE *stream,
                             const HWAGapReportResult *result,
                             const HWANumericLocale *locale)
{
    size_t index;
    char error[HWA_ERROR_SIZE];
    if (stream == NULL || result == NULL ||
        hwa_gap_report_result_validate(
            result, error, sizeof(error)) != 0) return -1;
    if (fputs("{\"schema\":\"hwa-gap-report\",\"schema_version\":11,"
              "\"method_version\":", stream) == EOF ||
        hwa_gro_json_string(stream, HWA_GAP_REPORT_METHOD_VERSION) != 0 ||
        fputs(",\"command\":", stream) == EOF ||
        hwa_gro_json_string(
            stream, hwa_gap_report_mode_name(result->mode)) != 0 ||
        fputs(",\"title\":", stream) == EOF ||
        hwa_gro_json_string(stream, result->title) != 0 ||
        fputs(",\"audibility_method\":", stream) == EOF ||
        hwa_gro_json_string(stream, result->audibility_method) != 0 ||
        fputs(",\"manifest_sha256\":", stream) == EOF ||
        hwa_gro_json_string(stream, result->manifest_sha256) != 0 ||
        fputs(",\"settings\":", stream) == EOF ||
        hwa_gro_json_options(stream, &result->options) != 0 ||
        fprintf(stream, ",\"total_input_bytes\":%" PRIu64
                ",\"total_output_bytes\":%" PRIu64
                ",\"evaluation_count\":%" PRIu64 ",\"sources\":[",
                result->total_input_bytes, result->total_output_bytes,
                result->evaluation_count) < 0) return -1;
#define ROWS(field, count, writer)                                         \
    do {                                                                   \
        for (index = 0U; index < result->count; ++index) {                 \
            if (index != 0U && fputc(',', stream) == EOF) return -1;       \
            if (writer(stream, &result->field[index]) != 0) return -1;     \
        }                                                                  \
    } while (0)
    ROWS(sources, source_count, hwa_gro_json_source);
    if (fputs("],\"labels\":[", stream) == EOF) return -1;
    ROWS(labels, label_count, hwa_gro_json_label);
    if (fputs("],\"candidates\":[", stream) == EOF) return -1;
    if (hwa_gro_json_candidates(stream, locale, result) != 0) return -1;
    if (fputs("],\"families\":[", stream) == EOF) return -1;
    ROWS(families, family_count, hwa_gro_json_family);
    if (fputs("],\"groups\":[", stream) == EOF) return -1;
    for (index = 0U; index < result->group_count; ++index) {
        if (index != 0U && fputc(',', stream) == EOF) return -1;
        if (hwa_gro_json_group(
                stream, locale, &result->groups[index]) != 0) return -1;
    }
    if (fputs("],\"worst_cases\":[", stream) == EOF) return -1;
    if (hwa_gro_json_worst_cases(stream, locale, result) != 0) return -1;
    if (fputs("],\"clips\":[", stream) == EOF) return -1;
    for (index = 0U; index < result->excerpt_count; ++index) {
        if (index != 0U && fputc(',', stream) == EOF) return -1;
        if (hwa_gro_json_excerpt(
                stream, locale, &result->excerpts[index]) != 0) return -1;
    }
    if (fputs("],\"exclusions\":[", stream) == EOF) return -1;
    {
        int first = 1;
        for (index = 0U; index < result->candidate_count; ++index) {
            const HWAGapReportCandidate *r = &result->candidates[index];
            if (r->availability == HWA_GAP_REPORT_AVAILABLE) continue;
            if (!first && fputc(',', stream) == EOF) return -1;
            if (fprintf(stream, "{\"candidate_id\":%" PRIu64
                        ",\"availability\":", r->id) < 0 ||
                hwa_gro_json_string(
                    stream,
                    hwa_gap_report_availability_name(r->availability)) != 0 ||
                fputs(",\"reason\":", stream) == EOF ||
                hwa_gro_json_string(stream, r->reason) != 0 ||
                fputc('}', stream) == EOF) return -1;
            first = 0;
        }
    }
    if (fputs("],\"failures\":[", stream) == EOF) return -1;
    {
        int first = 1;
        for (index = 0U; index < result->excerpt_count; ++index) {
            const HWAGapReportExcerpt *r = &result->excerpts[index];
            if (r->availability == HWA_GAP_REPORT_AVAILABLE) continue;
            if (!first && fputc(',', stream) == EOF) return -1;
            if (fprintf(stream, "{\"excerpt_id\":%" PRIu64
                        ",\"availability\":", r->id) < 0 ||
                hwa_gro_json_string(
                    stream,
                    hwa_gap_report_availability_name(r->availability)) != 0 ||
                fputs(",\"reason\":", stream) == EOF ||
                hwa_gro_json_string(stream, r->reason) != 0 ||
                fputc('}', stream) == EOF) return -1;
            first = 0;
        }
    }
    if (fputs("],\"warnings\":[", stream) == EOF) return -1;
    ROWS(warnings, warning_count, hwa_gro_json_warning);
#undef ROWS
    return fputs("]}\n", stream) == EOF ? -1 : 0;
}

int hwa_gap_report_json(FILE *stream, const HWAGapReportResult *result)
{
    HWANumericLocale locale;
    char error[HWA_ERROR_SIZE];
    uint64_t maximum;
    int status;
    if (stream == NULL || result == NULL ||
        hwa_gap_report_result_validate(result, error, sizeof(error)) != 0 ||
        result->total_output_bytes > result->options.max_bundle_bytes)
        return -1;
    maximum = result->options.max_bundle_bytes - result->total_output_bytes;
    if (maximum > result->options.max_output_file_bytes)
        maximum = result->options.max_output_file_bytes;
    if (hwa_gro_preflight(result, HWA_GR_RENDER_JSON, maximum) != 0)
        return -1;
    if (hwa_c_numeric_locale_begin(&locale) != 0) return -1;
    status = hwa_gro_json_impl(stream, result, &locale);
    if (hwa_c_numeric_locale_end(&locale) != 0) return -1;
    return status;
}

static int hwa_gro_text_candidates(FILE *stream,
                                   const HWANumericLocale *locale,
                                   const HWAGapReportResult *result)
{
    const HWAGapReportCandidate **ordered = NULL;
    size_t index;
    int status = -1;
    if (hwa_gro_candidate_order(result, &ordered) != 0) return -1;
    for (index = 0U; index < result->candidate_count; ++index) {
        const HWAGapReportCandidate *r = ordered[index];
        if (!r->primary || !r->score_valid) continue;
        if (fprintf(stream, "%zu\t", r->rank) < 0 ||
            hwa_gro_number(stream, locale, r->score) != 0 ||
            fputc('\t', stream) == EOF ||
            hwa_gro_text(stream, r->metric) != 0 ||
            fputc('\t', stream) == EOF ||
            hwa_gro_text(stream, r->case_id) != 0 ||
            fputc('\t', stream) == EOF ||
            hwa_gro_text(stream, r->reason) != 0 ||
            fputc('\n', stream) == EOF) goto cleanup;
    }
    status = 0;
cleanup:
    free(ordered);
    return status;
}

int hwa_gap_report_text(FILE *stream, const HWAGapReportResult *result)
{
    size_t index;
    char error[HWA_ERROR_SIZE];
    HWANumericLocale locale;
    int status = -1;
    if (stream == NULL || result == NULL ||
        hwa_gap_report_result_validate(
            result, error, sizeof(error)) != 0 ||
        hwa_gro_preflight(result, HWA_GR_RENDER_TEXT,
                          result->options.max_output_file_bytes) != 0)
        return -1;
    if (hwa_c_numeric_locale_begin(&locale) != 0) return -1;
    if (fputs("Stage 9 gap report: ", stream) == EOF ||
        hwa_gro_text(stream, result->title) != 0 ||
        fprintf(stream, "\nMode: %s\nCandidates: %zu; linked families: %zu; "
                "groups: %zu\n", hwa_gap_report_mode_name(result->mode),
                result->candidate_count, result->family_count,
                result->group_count) < 0) goto cleanup;
    if (hwa_gro_text_candidates(stream, &locale, result) != 0) goto cleanup;
    if (fprintf(stream, "Exclusions: ") < 0) goto cleanup;
    {
        size_t count = 0U;
        for (index = 0U; index < result->candidate_count; ++index)
            if (result->candidates[index].availability !=
                HWA_GAP_REPORT_AVAILABLE) count++;
        if (fprintf(stream, "%zu\nFailures: ", count) < 0) goto cleanup;
    }
    {
        size_t count = 0U;
        for (index = 0U; index < result->excerpt_count; ++index)
            if (result->excerpts[index].availability !=
                HWA_GAP_REPORT_AVAILABLE) count++;
        if (fprintf(stream, "%zu\nWarnings: %zu\n",
                    count, result->warning_count) < 0) goto cleanup;
    }
    status = 0;
cleanup:
    if (hwa_c_numeric_locale_end(&locale) != 0) return -1;
    return status;
}

static int hwa_gro_relative_audio_path(const char *path)
{
    const char *cursor;
    const char *component;
    size_t length;
    if (path == NULL || strncmp(path, "audio/", 6U) != 0 ||
        path[6] == '\0' || strchr(path, '\\') != NULL) return 0;
    cursor = path;
    while (*cursor != '\0') {
        component = cursor;
        while (*cursor != '\0' && *cursor != '/') cursor++;
        length = (size_t)(cursor - component);
        if (length == 0U ||
            (length == 1U && component[0] == '.') ||
            (length == 2U && component[0] == '.' &&
             component[1] == '.')) return 0;
        if (*cursor == '/') cursor++;
    }
    return 1;
}

static int hwa_gro_excerpt_name_valid(const char *name)
{
    size_t name_size;
    size_t index;
    if (name == NULL) return 0;
    name_size = strlen(name);
    if (name_size == 0U || name_size > 255U) return 0;
    for (index = 0U; index < name_size; ++index) {
        unsigned char c = (unsigned char)name[index];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) return 0;
    }
    return 1;
}

static int hwa_gro_exact_audio_path(const char *path,
                                    const char *name,
                                    char suffix)
{
    size_t name_size;
    size_t path_size;
    if (path == NULL || !hwa_gro_excerpt_name_valid(name)) return 0;
    name_size = strlen(name);
    path_size = strlen(path);
    return name_size <= SIZE_MAX - 13U &&
           path_size == name_size + 12U &&
           memcmp(path, "audio/", 6U) == 0 &&
           memcmp(path + 6U, name, name_size) == 0 &&
           path[6U + name_size] == '-' &&
           path[7U + name_size] == suffix &&
           memcmp(path + 8U + name_size, ".wav", 5U) == 0;
}

static int hwa_gro_exact_excerpt_paths(const HWAGapReportResult *result)
{
    size_t index;
    for (index = 0U; index < result->excerpt_count; ++index) {
        const HWAGapReportExcerpt *r = &result->excerpts[index];
        if (!hwa_gro_excerpt_name_valid(r->name)) return 0;
        if (r->availability == HWA_GAP_REPORT_AVAILABLE) {
            if (!hwa_gro_exact_audio_path(
                    r->reference_path, r->name, 'a') ||
                !hwa_gro_exact_audio_path(r->model_path, r->name, 'b') ||
                (r->make_x
                    ? !hwa_gro_exact_audio_path(r->x_path, r->name, 'x')
                    : (r->x_path == NULL || r->x_path[0] != '\0')))
                return 0;
        } else if (r->reference_path == NULL ||
                   r->reference_path[0] != '\0' ||
                   r->model_path == NULL || r->model_path[0] != '\0' ||
                   r->x_path == NULL || r->x_path[0] != '\0') {
            return 0;
        }
    }
    return 1;
}

static int hwa_gro_absolute_path(const char *path)
{
    if (path == NULL || path[0] == '\0') return 0;
#if defined(_WIN32)
    return (path[0] == '\\' && path[1] == '\\') ||
           (((path[0] >= 'A' && path[0] <= 'Z') ||
             (path[0] >= 'a' && path[0] <= 'z')) &&
            path[1] == ':' && (path[2] == '\\' || path[2] == '/'));
#else
    return path[0] == '/';
#endif
}

static const char *hwa_gro_audio_name(const char *path)
{
    const char *name;
    if (!hwa_gro_relative_audio_path(path)) return NULL;
    name = path + 6U;
    return strchr(name, '/') == NULL ? name : NULL;
}

static int hwa_gro_expected_audio_name(
    const HWAGapReportResult *result,
    const char *name)
{
    size_t index;
    size_t matches = 0U;
    for (index = 0U; index < result->excerpt_count; ++index) {
        const HWAGapReportExcerpt *r = &result->excerpts[index];
        const char *expected;
        if (r->availability != HWA_GAP_REPORT_AVAILABLE) continue;
        expected = hwa_gro_audio_name(r->reference_path);
        if (expected != NULL && strcmp(name, expected) == 0) matches++;
        expected = hwa_gro_audio_name(r->model_path);
        if (expected != NULL && strcmp(name, expected) == 0) matches++;
        if (r->make_x) {
            expected = hwa_gro_audio_name(r->x_path);
            if (expected != NULL && strcmp(name, expected) == 0) matches++;
        }
    }
    return matches == 1U;
}

static int hwa_gro_expected_root_name(HWAGapReportMode mode,
                                      const char *name,
                                      int *directory)
{
    *directory = strcmp(name, "audio") == 0;
    if (*directory || strcmp(name, "result.hwa-report") == 0) return 1;
    return mode == HWA_GAP_REPORT_FULL &&
           (strcmp(name, "report.csv") == 0 ||
            strcmp(name, "report.json") == 0 ||
            strcmp(name, "report.html") == 0);
}

static int hwa_gro_join(char **out,
                        const char *directory,
                        const char *relative,
                        uint64_t *live_work,
                        uint64_t max_work)
{
    size_t a;
    size_t b;
    int separator;
    char *path;
    uint64_t bytes;
    if (out == NULL || live_work == NULL ||
        directory == NULL || directory[0] == '\0' ||
        relative == NULL || relative[0] == '\0') return -1;
    a = strlen(directory);
    b = strlen(relative);
    separator = directory[a - 1U] != '/' && directory[a - 1U] != '\\';
    if (a > SIZE_MAX - b - (size_t)separator - 1U) return -1;
    bytes = (uint64_t)(a + b + (size_t)separator + 1U);
    if (*live_work > max_work || bytes > max_work - *live_work) return -1;
    *live_work += bytes;
    path = (char *)malloc(a + b + (size_t)separator + 1U);
    if (path == NULL) {
        *live_work -= bytes;
        return -1;
    }
    memcpy(path, directory, a);
    if (separator) path[a++] = '/';
    memcpy(path + a, relative, b + 1U);
    *out = path;
    return 0;
}

static void hwa_gro_join_free(char **path, uint64_t *live_work)
{
    uint64_t bytes;
    if (path == NULL || *path == NULL) return;
    bytes = (uint64_t)strlen(*path) + UINT64_C(1);
    free(*path);
    *path = NULL;
    if (live_work != NULL && bytes <= *live_work) *live_work -= bytes;
}

#if defined(_WIN32)
static void hwa_gro_windows_identity(
    const BY_HANDLE_FILE_INFORMATION *info,
    HANDLE handle,
    HWAGROutIdentity *identity)
{
    FILE_BASIC_INFO basic;
    identity->device = (uint64_t)info->dwVolumeSerialNumber;
    identity->file = ((uint64_t)info->nFileIndexHigh << 32U) |
                     (uint64_t)info->nFileIndexLow;
    identity->size = ((uint64_t)info->nFileSizeHigh << 32U) |
                     (uint64_t)info->nFileSizeLow;
    identity->modified_seconds =
        ((int64_t)info->ftLastWriteTime.dwHighDateTime << 32U) |
        (int64_t)info->ftLastWriteTime.dwLowDateTime;
    identity->modified_nanoseconds = 0;
    identity->changed_seconds =
        GetFileInformationByHandleEx(
            handle, FileBasicInfo, &basic, (DWORD)sizeof(basic))
            ? (int64_t)basic.ChangeTime.QuadPart
            : identity->modified_seconds;
    identity->changed_nanoseconds = 0;
}

static int hwa_gro_path_identity(const char *path,
                                 int directory,
                                 HWAGROutIdentity *identity)
{
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE handle = CreateFileA(
        path, 0U, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT |
        (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0U), NULL);
    int is_directory;
    if (handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(handle, &info)) {
        if (handle != INVALID_HANDLE_VALUE) (void)CloseHandle(handle);
        return -1;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        (void)CloseHandle(handle); return -1;
    }
    is_directory =
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    if (is_directory != directory) { (void)CloseHandle(handle); return -1; }
    hwa_gro_windows_identity(&info, handle, identity);
    (void)CloseHandle(handle);
    return 0;
}

static int hwa_gro_descriptor_identity(int descriptor,
                                       HWAGROutIdentity *identity)
{
    BY_HANDLE_FILE_INFORMATION info;
    intptr_t raw = descriptor >= 0
        ? _get_osfhandle(descriptor) : (intptr_t)-1;
    if (raw == (intptr_t)-1 ||
        !GetFileInformationByHandle((HANDLE)raw, &info) ||
        (info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U)
        return -1;
    hwa_gro_windows_identity(&info, (HANDLE)raw, identity);
    return 0;
}

static int hwa_gro_stream_identity(FILE *stream,
                                   HWAGROutIdentity *identity)
{
    return hwa_gro_descriptor_identity(_fileno(stream), identity);
}
#else
static int hwa_gro_path_identity(const char *path,
                                 int directory,
                                 HWAGROutIdentity *identity)
{
    struct stat facts;
    if (lstat(path, &facts) != 0 ||
        (directory ? !S_ISDIR(facts.st_mode) :
                     !S_ISREG(facts.st_mode)) ||
        facts.st_size < 0) return -1;
    identity->device = (uint64_t)facts.st_dev;
    identity->file = (uint64_t)facts.st_ino;
    identity->size = (uint64_t)facts.st_size;
#if defined(__APPLE__)
    identity->modified_seconds = (int64_t)facts.st_mtimespec.tv_sec;
    identity->modified_nanoseconds = (int64_t)facts.st_mtimespec.tv_nsec;
    identity->changed_seconds = (int64_t)facts.st_ctimespec.tv_sec;
    identity->changed_nanoseconds = (int64_t)facts.st_ctimespec.tv_nsec;
#else
    identity->modified_seconds = (int64_t)facts.st_mtim.tv_sec;
    identity->modified_nanoseconds = (int64_t)facts.st_mtim.tv_nsec;
    identity->changed_seconds = (int64_t)facts.st_ctim.tv_sec;
    identity->changed_nanoseconds = (int64_t)facts.st_ctim.tv_nsec;
#endif
    return 0;
}

static int hwa_gro_descriptor_identity(int descriptor,
                                       HWAGROutIdentity *identity)
{
    struct stat facts;
    if (descriptor < 0 || fstat(descriptor, &facts) != 0 ||
        !S_ISREG(facts.st_mode) || facts.st_size < 0) return -1;
    identity->device = (uint64_t)facts.st_dev;
    identity->file = (uint64_t)facts.st_ino;
    identity->size = (uint64_t)facts.st_size;
#if defined(__APPLE__)
    identity->modified_seconds = (int64_t)facts.st_mtimespec.tv_sec;
    identity->modified_nanoseconds = (int64_t)facts.st_mtimespec.tv_nsec;
    identity->changed_seconds = (int64_t)facts.st_ctimespec.tv_sec;
    identity->changed_nanoseconds = (int64_t)facts.st_ctimespec.tv_nsec;
#else
    identity->modified_seconds = (int64_t)facts.st_mtim.tv_sec;
    identity->modified_nanoseconds = (int64_t)facts.st_mtim.tv_nsec;
    identity->changed_seconds = (int64_t)facts.st_ctim.tv_sec;
    identity->changed_nanoseconds = (int64_t)facts.st_ctim.tv_nsec;
#endif
    return 0;
}

static int hwa_gro_stream_identity(FILE *stream,
                                   HWAGROutIdentity *identity)
{
    return hwa_gro_descriptor_identity(fileno(stream), identity);
}
#endif

static FILE *hwa_gro_open_read(const char *path)
{
#if defined(_WIN32)
    HANDLE handle = CreateFileA(
        path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BY_HANDLE_FILE_INFORMATION info;
    int descriptor;
    FILE *stream;
    if (handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        if (handle != INVALID_HANDLE_VALUE) (void)CloseHandle(handle);
        return NULL;
    }
    descriptor = _open_osfhandle(
        (intptr_t)handle, _O_RDONLY | _O_BINARY);
    if (descriptor < 0) {
        (void)CloseHandle(handle);
        return NULL;
    }
    stream = _fdopen(descriptor, "rb");
    if (stream == NULL) (void)_close(descriptor);
    return stream;
#else
    int flags = O_RDONLY;
    int descriptor;
    FILE *stream;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    descriptor = open(path, flags);
    if (descriptor < 0) return NULL;
    stream = fdopen(descriptor, "rb");
    if (stream == NULL) (void)close(descriptor);
    return stream;
#endif
}

static int hwa_gro_identity_equal(const HWAGROutIdentity *a,
                                  const HWAGROutIdentity *b)
{
    return a->device == b->device && a->file == b->file &&
           a->size == b->size &&
           a->modified_seconds == b->modified_seconds &&
           a->modified_nanoseconds == b->modified_nanoseconds &&
           a->changed_seconds == b->changed_seconds &&
           a->changed_nanoseconds == b->changed_nanoseconds;
}

static int hwa_gro_expected_audio_count(
    const HWAGapReportResult *result,
    size_t *expected)
{
    size_t index;
    size_t count = 0U;
    for (index = 0U; index < result->excerpt_count; ++index) {
        const HWAGapReportExcerpt *r = &result->excerpts[index];
        size_t add;
        if (r->availability != HWA_GAP_REPORT_AVAILABLE) continue;
        add = r->make_x ? 3U : 2U;
        if (add > SIZE_MAX - count) return -1;
        count += add;
    }
    *expected = count;
    return 0;
}

static int hwa_gro_directory_exact(const char *path,
                                   const HWAGapReportResult *result,
                                   int audio,
                                   uint64_t *live_work)
{
    size_t seen = 0U;
    size_t expected_audio = 0U;
    if (audio && hwa_gro_expected_audio_count(
            result, &expected_audio) != 0) return -1;
#if defined(_WIN32)
    WIN32_FIND_DATAA entry;
    HANDLE search;
    char *pattern = NULL;
    if (hwa_gro_join(&pattern, path, "*", live_work,
                     result->options.max_work_bytes) != 0) return -1;
    search = FindFirstFileA(pattern, &entry);
    hwa_gro_join_free(&pattern, live_work);
    if (search == INVALID_HANDLE_VALUE) {
        DWORD failure = GetLastError();
        return audio && expected_audio == 0U &&
            failure == ERROR_FILE_NOT_FOUND ? 0 : -1;
    }
    for (;;) {
        if (strcmp(entry.cFileName, ".") != 0 &&
            strcmp(entry.cFileName, "..") != 0) {
            int expected_directory = 0;
            int expected = audio
                ? hwa_gro_expected_audio_name(result, entry.cFileName)
                : hwa_gro_expected_root_name(
                      result->mode, entry.cFileName, &expected_directory);
            int is_directory =
                (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
            if (!expected || is_directory != expected_directory ||
                (entry.dwFileAttributes &
                 (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) !=
                    0U) {
                (void)FindClose(search);
                return -1;
            }
            seen++;
        }
        if (!FindNextFileA(search, &entry)) break;
    }
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        (void)FindClose(search);
        return -1;
    }
    (void)FindClose(search);
#else
    DIR *directory = opendir(path);
    struct dirent *entry;
    if (directory == NULL) return -1;
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        char *child = NULL;
        struct stat facts;
        int expected_directory = 0;
        int expected;
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            errno = 0;
            continue;
        }
        expected = audio
            ? hwa_gro_expected_audio_name(result, entry->d_name)
            : hwa_gro_expected_root_name(
                  result->mode, entry->d_name, &expected_directory);
        if (!expected || hwa_gro_join(
                &child, path, entry->d_name, live_work,
                result->options.max_work_bytes) != 0 ||
            lstat(child, &facts) != 0 ||
            (expected_directory ? !S_ISDIR(facts.st_mode)
                                : !S_ISREG(facts.st_mode))) {
            hwa_gro_join_free(&child, live_work);
            (void)closedir(directory);
            return -1;
        }
        hwa_gro_join_free(&child, live_work);
        seen++;
        errno = 0;
    }
    if (errno != 0 || closedir(directory) != 0) return -1;
#endif
    return seen == (audio ? expected_audio
                          : (result->mode == HWA_GAP_REPORT_FULL ? 5U : 2U))
        ? 0 : -1;
}

static void hwa_gro_digest_hex(const unsigned char digest[32],
                               char text[HWA_SHA256_HEX_SIZE])
{
    static const char digits[] = "0123456789abcdef";
    size_t index;
    for (index = 0U; index < 32U; ++index) {
        text[index * 2U] = digits[digest[index] >> 4U];
        text[index * 2U + 1U] = digits[digest[index] & 15U];
    }
    text[64] = '\0';
}

static int hwa_gro_hash_file(
    const char *path,
    uint64_t maximum,
    uint64_t expected_bytes,
    const char expected_sha[HWA_SHA256_HEX_SIZE],
    HWAGROutIdentity *accepted)
{
    unsigned char buffer[65536];
    unsigned char digest[32];
    char hash_text[HWA_SHA256_HEX_SIZE];
    HWAGROutIdentity before;
    HWAGROutIdentity opened;
    HWAGROutIdentity after;
    HWASha256 hash;
    FILE *stream = NULL;
    uint64_t total = 0U;
    int status = -1;
    if (hwa_gro_path_identity(path, 0, &before) != 0 ||
        before.size != expected_bytes || before.size > maximum) return -1;
    stream = hwa_gro_open_read(path);
    if (stream == NULL || hwa_gro_stream_identity(stream, &opened) != 0 ||
        !hwa_gro_identity_equal(&before, &opened)) goto cleanup;
    hwa_sha256_init(&hash);
    for (;;) {
        size_t count = fread(buffer, 1U, sizeof(buffer), stream);
        if (count == 0U) break;
        if ((uint64_t)count > expected_bytes - total) goto cleanup;
        total += (uint64_t)count;
        hwa_sha256_update(&hash, buffer, count);
    }
    {
        int read_error = ferror(stream);
        int close_error = fclose(stream);
        stream = NULL;
        if (read_error || total != expected_bytes || close_error != 0)
            goto cleanup;
    }
    if (hwa_gro_path_identity(path, 0, &after) != 0 ||
        !hwa_gro_identity_equal(&before, &after)) goto cleanup;
    hwa_sha256_final(&hash, digest);
    hwa_gro_digest_hex(digest, hash_text);
    if (strcmp(hash_text, expected_sha) != 0) goto cleanup;
    if (accepted != NULL) *accepted = after;
    status = 0;
cleanup:
    if (stream != NULL) (void)fclose(stream);
    return status;
}

static int hwa_gro_base64_file(FILE *output,
                               const char *bundle_directory,
                               const char *relative,
                               const char expected_sha[HWA_SHA256_HEX_SIZE],
                               uint64_t expected_bytes,
                               uint64_t maximum,
                               uint64_t retained_work,
                               uint64_t max_work)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *path = NULL;
    FILE *input = NULL;
    HWAGROutIdentity before;
    HWAGROutIdentity opened;
    HWAGROutIdentity after;
    HWASha256 hash;
    unsigned char digest[32];
    char hash_text[HWA_SHA256_HEX_SIZE];
    unsigned char in[3];
    uint64_t total = 0U;
    uint64_t live_work = retained_work;
    int status = -1;
    if (!hwa_gro_relative_audio_path(relative) ||
        hwa_gro_join(&path, bundle_directory, relative,
                     &live_work, max_work) != 0 ||
        hwa_gro_path_identity(path, 0, &before) != 0 ||
        before.size != expected_bytes || before.size > maximum) goto cleanup;
    input = hwa_gro_open_read(path);
    if (input == NULL || hwa_gro_stream_identity(input, &opened) != 0 ||
        !hwa_gro_identity_equal(&before, &opened)) goto cleanup;
    hwa_sha256_init(&hash);
    for (;;) {
        size_t count = fread(in, 1U, sizeof(in), input);
        if (count == 0U) break;
        if ((uint64_t)count > expected_bytes - total) goto cleanup;
        total += (uint64_t)count;
        hwa_sha256_update(&hash, in, count);
        if (fputc(alphabet[in[0] >> 2U], output) == EOF ||
            fputc(alphabet[((in[0] & 3U) << 4U) |
                           (count > 1U ? in[1] >> 4U : 0U)],
                  output) == EOF ||
            fputc(count > 1U
                      ? alphabet[((in[1] & 15U) << 2U) |
                                 (count > 2U ? in[2] >> 6U : 0U)]
                      : '=',
                  output) == EOF ||
            fputc(count > 2U ? alphabet[in[2] & 63U] : '=',
                  output) == EOF) goto cleanup;
        if (count != sizeof(in)) break;
    }
    {
        int read_error = ferror(input);
        int close_error = fclose(input);
        input = NULL;
        if (read_error || total != expected_bytes || close_error != 0)
            goto cleanup;
    }
    if (hwa_gro_path_identity(path, 0, &after) != 0 ||
        !hwa_gro_identity_equal(&before, &after)) goto cleanup;
    hwa_sha256_final(&hash, digest);
    hwa_gro_digest_hex(digest, hash_text);
    if (strcmp(hash_text, expected_sha) != 0) goto cleanup;
    status = 0;
cleanup:
    if (input != NULL) (void)fclose(input);
    hwa_gro_join_free(&path, &live_work);
    return status;
}

static int hwa_gro_html_audio(FILE *stream,
                              const char *bundle_directory,
                              const char *label,
                              const char *path,
                              const char sha[HWA_SHA256_HEX_SIZE],
                              uint64_t bytes,
                              uint64_t maximum,
                              uint64_t retained_work,
                              uint64_t max_work)
{
    return fputs("<label>", stream) == EOF ||
                   hwa_gro_html_text(stream, label) != 0 ||
                   fputs("<audio controls preload=\"none\" src=\"data:audio/"
                         "wav;base64,",
                         stream) == EOF ||
                   hwa_gro_base64_file(stream, bundle_directory, path,
                                       sha, bytes, maximum,
                                       retained_work, max_work) != 0 ||
                   fputs("\"></audio></label>", stream) == EOF
               ? -1
               : 0;
}

static int hwa_gro_html_optional_number(FILE *stream,
                                        const HWANumericLocale *locale,
                                        double value,
                                        int valid)
{
    return valid ? hwa_gro_number(stream, locale, value)
                 : (fputs("n/a", stream) == EOF ? -1 : 0);
}

static int hwa_gro_html_ranked(FILE *stream,
                               const HWANumericLocale *locale,
                               const HWAGapReportResult *result)
{
    const HWAGapReportCandidate **ordered = NULL;
    char number[64];
    size_t index;
    int status = -1;
    if (hwa_gro_candidate_order(result, &ordered) != 0 ||
        fputs("<svg viewBox=\"0 0 1000 180\" role=\"img\" "
              "aria-label=\"Candidate scores for exact ranks 1 through 20\">",
              stream) == EOF) goto cleanup;
    for (index = 0U; index < result->candidate_count; ++index) {
        const HWAGapReportCandidate *r = ordered[index];
        double width;
        if (!r->primary || !r->score_valid) continue;
        if (r->rank > 20U) break;
        width = r->score * 900.0;
        if (width < 0.0) width = 0.0;
        if (width > 900.0) width = 900.0;
        if (hwa_c_locale_format_double(
                locale, number, sizeof(number), width) != 0 ||
            fprintf(stream, "<rect data-rank=\"%zu\" x=\"80\" y=\"%zu\" "
                    "width=\"%s\" height=\"6\" fill=\"#245b78\"><title>"
                    "Rank %zu: ",
                    r->rank, 8U + (r->rank - 1U) * 8U, number, r->rank) < 0 ||
            hwa_gro_html_text(stream, r->metric) != 0 ||
            fputc(' ', stream) == EOF ||
            hwa_gro_number(stream, locale, r->score) != 0 ||
            fputs("</title></rect>", stream) == EOF) goto cleanup;
    }
    if (fputs("</svg><table><thead><tr><th>Rank</th><th>Metric</th>"
              "<th>Case</th><th>Size</th><th>Audible</th><th>Occurrence</th>"
              "<th>Confidence</th><th>Score</th></tr></thead><tbody>",
              stream) == EOF) goto cleanup;
    for (index = 0U; index < result->candidate_count; ++index) {
        const HWAGapReportCandidate *r = ordered[index];
        if (!r->primary || !r->score_valid) continue;
        if (fprintf(stream, "<tr data-rank=\"%zu\"><td>%zu</td><td>",
                    r->rank, r->rank) < 0 ||
            hwa_gro_html_text(stream, r->metric) != 0 ||
            fputs("</td><td>", stream) == EOF ||
            hwa_gro_html_text(stream, r->case_id) != 0 ||
            fputs("</td><td>", stream) == EOF ||
            hwa_gro_number(stream, locale, r->size_factor) != 0 ||
            fputs("</td><td>", stream) == EOF ||
            hwa_gro_number(stream, locale, r->audibility_factor) != 0 ||
            fputs("</td><td>", stream) == EOF ||
            hwa_gro_number(stream, locale, r->occurrence_factor) != 0 ||
            fputs("</td><td>", stream) == EOF ||
            hwa_gro_number(stream, locale, r->confidence_factor) != 0 ||
            fputs("</td><td>", stream) == EOF ||
            hwa_gro_number(stream, locale, r->score) != 0 ||
            fputs("</td></tr>", stream) == EOF) goto cleanup;
    }
    if (fputs("</tbody></table>", stream) == EOF) goto cleanup;
    status = 0;
cleanup:
    free(ordered);
    return status;
}

static int hwa_gro_html_groups(FILE *stream,
                               const HWANumericLocale *locale,
                               const HWAGapReportResult *result)
{
    size_t index;
    if (fputs("<h2>Six-axis group statistics</h2><table><thead><tr>"
              "<th>Axis</th><th>Value</th><th>Families</th>"
              "<th>Candidates</th><th>Available</th><th>Missing</th>"
              "<th>Excluded</th><th>q05</th><th>q25</th><th>Median</th>"
              "<th>q75</th><th>q95</th><th>Spread</th><th>Confidence</th>"
              "</tr></thead><tbody>", stream) == EOF) return -1;
    for (index = 0U; index < result->group_count; ++index) {
        const HWAGapReportGroup *r = &result->groups[index];
        if (fputs("<tr><td>", stream) == EOF ||
            hwa_gro_html_text(
                stream, hwa_gap_report_axis_name(r->axis)) != 0 ||
            fputs("</td><td>", stream) == EOF ||
            hwa_gro_html_text(stream, r->value) != 0 ||
            fprintf(stream, "</td><td>%zu</td><td>%zu</td><td>%zu</td>"
                    "<td>%zu</td><td>%zu</td><td>",
                    r->family_count, r->candidate_count, r->available_count,
                    r->missing_count, r->excluded_count) < 0 ||
            hwa_gro_html_optional_number(
                stream, locale, r->q05, r->statistics_valid) != 0 ||
            fputs("</td><td>", stream) == EOF ||
            hwa_gro_html_optional_number(
                stream, locale, r->q25, r->statistics_valid) != 0 ||
            fputs("</td><td>", stream) == EOF ||
            hwa_gro_html_optional_number(
                stream, locale, r->median, r->statistics_valid) != 0 ||
            fputs("</td><td>", stream) == EOF ||
            hwa_gro_html_optional_number(
                stream, locale, r->q75, r->statistics_valid) != 0 ||
            fputs("</td><td>", stream) == EOF ||
            hwa_gro_html_optional_number(
                stream, locale, r->q95, r->statistics_valid) != 0 ||
            fputs("</td><td>", stream) == EOF ||
            hwa_gro_html_optional_number(
                stream, locale, r->spread, r->statistics_valid) != 0 ||
            fputs("</td><td>", stream) == EOF ||
            hwa_gro_html_optional_number(
                stream, locale, r->confidence, r->confidence_valid) != 0 ||
            fputs("</td></tr>", stream) == EOF) return -1;
    }
    return fputs("</tbody></table>", stream) == EOF ? -1 : 0;
}

static int hwa_gro_html_worst_cases(FILE *stream,
                                    const HWANumericLocale *locale,
                                    const HWAGapReportResult *result)
{
    const HWAGapReportCase **ordered = NULL;
    size_t count = 0U;
    size_t index;
    int status = -1;
    if (hwa_gro_worst_cases(result, &ordered, &count) != 0 ||
        fputs("<h2>Worst matched cases</h2><table><thead><tr>"
              "<th>Candidate rank</th><th>Metric</th><th>Case</th>"
              "<th>Value</th><th>Confidence</th><th>Score</th><th>Reason</th>"
              "</tr></thead><tbody>", stream) == EOF) goto cleanup;
    for (index = 0U; index < count; ++index) {
        const HWAGapReportCase *r = ordered[index];
        const HWAGapReportCandidate *candidate =
            &result->candidates[r->candidate_id - 1U];
        if (fprintf(stream, "<tr data-case-rank=\"%zu\"><td>%zu</td><td>",
                    index + 1U, candidate->rank) < 0 ||
            hwa_gro_html_text(stream, candidate->metric) != 0 ||
            fputs("</td><td>", stream) == EOF ||
            hwa_gro_html_text(stream, r->case_id) != 0 ||
            fputs("</td><td>", stream) == EOF ||
            hwa_gro_html_optional_number(
                stream, locale, r->value, r->value_valid) != 0 ||
            fputs("</td><td>", stream) == EOF ||
            hwa_gro_html_optional_number(
                stream, locale, r->confidence, r->confidence_valid) != 0 ||
            fputs("</td><td>", stream) == EOF ||
            hwa_gro_html_optional_number(
                stream, locale, r->score, r->score_valid) != 0 ||
            fputs("</td><td>", stream) == EOF ||
            hwa_gro_html_text(stream, r->reason) != 0 ||
            fputs("</td></tr>", stream) == EOF) goto cleanup;
    }
    if (fputs("</tbody></table>", stream) == EOF) goto cleanup;
    status = 0;
cleanup:
    free(ordered);
    return status;
}

static int hwa_gro_html_with_work(FILE *stream,
                                  const char *bundle_directory,
                                  const HWAGapReportResult *result,
                                  uint64_t live_work)
{
    size_t index;
    char error[HWA_ERROR_SIZE];
    HWANumericLocale locale;
    uint64_t maximum;
    int status = -1;
    if (stream == NULL || bundle_directory == NULL ||
        result == NULL || result->mode != HWA_GAP_REPORT_FULL ||
        hwa_gap_report_result_validate(
            result, error, sizeof(error)) != 0 ||
        hwa_gro_work_preflight(result, live_work) != 0 ||
        !hwa_gro_exact_excerpt_paths(result) ||
        result->total_output_bytes > result->options.max_bundle_bytes)
        return -1;
    maximum = result->options.max_bundle_bytes - result->total_output_bytes;
    if (maximum > result->options.max_output_file_bytes)
        maximum = result->options.max_output_file_bytes;
    if (hwa_gro_preflight(result, HWA_GR_RENDER_HTML, maximum) != 0)
        return -1;
    if (hwa_c_numeric_locale_begin(&locale) != 0) return -1;
    if (fputs("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
              "<meta name=\"viewport\" content=\"width=device-width,"
              "initial-scale=1\"><meta http-equiv=\"Content-Security-Policy\" "
              "content=\"default-src 'none'; style-src 'unsafe-inline'; "
              "script-src 'unsafe-inline'; media-src data:\"><title>",
              stream) == EOF ||
        hwa_gro_html_text(stream, result->title) != 0 ||
        fputs("</title><style>body{font:16px system-ui;max-width:76rem;"
              "margin:auto;padding:2rem;color:#17202a;background:#faf9f6}"
              "table{border-collapse:collapse;width:100%}th,td{padding:.45rem;"
              "border-bottom:1px solid #ccc;text-align:left}svg{width:100%;"
              "height:12rem}audio{display:block;width:100%}.muted{color:#666}"
              "</style></head><body><h1>",
              stream) == EOF ||
        hwa_gro_html_text(stream, result->title) != 0 ||
        fprintf(stream, "</h1><p class=\"muted\">Method %s; audibility %s."
                " All failures and exclusions remain below.</p>"
                "<h2>Ranked linked gaps</h2>",
                HWA_GAP_REPORT_METHOD_VERSION,
                HWA_GAP_REPORT_AUDIBILITY_METHOD) < 0) goto cleanup;
    if (hwa_gro_html_ranked(stream, &locale, result) != 0 ||
        hwa_gro_html_groups(stream, &locale, result) != 0 ||
        hwa_gro_html_worst_cases(stream, &locale, result) != 0 ||
        fputs("<h2>Listening clips</h2>", stream) == EOF)
        goto cleanup;
    for (index = 0U; index < result->excerpt_count; ++index) {
        const HWAGapReportExcerpt *r = &result->excerpts[index];
        if (fputs("<section><h3>", stream) == EOF ||
            hwa_gro_html_text(stream, r->name) != 0 ||
            fputs("</h3>", stream) == EOF) goto cleanup;
        if (r->availability == HWA_GAP_REPORT_AVAILABLE) {
            if (hwa_gro_html_audio(
                    stream, bundle_directory, "A", r->reference_path,
                    r->reference_sha256, r->reference_file_bytes,
                    result->options.max_output_file_bytes,
                    live_work,
                    result->options.max_work_bytes) != 0 ||
                hwa_gro_html_audio(
                    stream, bundle_directory, "B", r->model_path,
                    r->model_sha256, r->model_file_bytes,
                    result->options.max_output_file_bytes,
                    live_work,
                    result->options.max_work_bytes) != 0 ||
                (r->make_x &&
                 hwa_gro_html_audio(
                     stream, bundle_directory, "X", r->x_path,
                     r->x_sha256, r->x_file_bytes,
                     result->options.max_output_file_bytes,
                     live_work,
                     result->options.max_work_bytes) != 0)) goto cleanup;
            if (r->make_x &&
                fprintf(stream, "<button type=\"button\" data-answer=\"%s\" "
                        "onclick=\"this.textContent='X is '+this.dataset.answer\">"
                        "Reveal X</button>",
                        r->x_is_reference ? "A" : "B") < 0) goto cleanup;
        } else {
            if (fputs("<p>Failed: ", stream) == EOF ||
                hwa_gro_html_text(stream, r->reason) != 0 ||
                fputs("</p>", stream) == EOF) goto cleanup;
        }
        if (fputs("</section>", stream) == EOF) goto cleanup;
    }
    if (fputs("<h2>Sources and settings</h2><p>Manifest SHA-256: <code>",
              stream) == EOF ||
        hwa_gro_html_text(stream, result->manifest_sha256) != 0 ||
        fputs("</code></p><ul>", stream) == EOF) goto cleanup;
    for (index = 0U; index < result->source_count; ++index) {
        if (fputs("<li>", stream) == EOF ||
            hwa_gro_html_text(stream, result->sources[index].name) != 0 ||
            fputs(": <code>", stream) == EOF ||
            hwa_gro_html_text(stream, result->sources[index].sha256) != 0 ||
            fputs("</code></li>", stream) == EOF) goto cleanup;
    }
    if (fprintf(stream, "</ul><p>Decode block: %zu frames; work cap: %" PRIu64
                " bytes; evaluation cap: %" PRIu64
                "; output-file cap: %" PRIu64
                " bytes; bundle cap: %" PRIu64 " bytes.</p>"
                "<details><summary>All settings</summary><pre>",
                result->options.decode_block_frames,
                result->options.max_work_bytes,
                result->options.max_evaluations,
                result->options.max_output_file_bytes,
                result->options.max_bundle_bytes) < 0 ||
        hwa_gro_json_options(stream, &result->options) != 0 ||
        fputs("</pre></details>", stream) == EOF) goto cleanup;
    if (fputs("<h2>Exclusions</h2><ul>", stream) == EOF) goto cleanup;
    for (index = 0U; index < result->candidate_count; ++index) {
        const HWAGapReportCandidate *r = &result->candidates[index];
        if (r->availability == HWA_GAP_REPORT_AVAILABLE) continue;
        if (fputs("<li>", stream) == EOF ||
            hwa_gro_html_text(stream, r->metric) != 0 ||
            fputs(": ", stream) == EOF ||
            hwa_gro_html_text(stream, r->reason) != 0 ||
            fputs("</li>", stream) == EOF) goto cleanup;
    }
    if (fputs("</ul><h2>Warnings</h2><ul>", stream) == EOF) goto cleanup;
    for (index = 0U; index < result->warning_count; ++index) {
        if (fputs("<li>", stream) == EOF ||
            hwa_gro_html_text(stream, result->warnings[index].code) != 0 ||
            fputs(": ", stream) == EOF ||
            hwa_gro_html_text(stream, result->warnings[index].message) != 0 ||
            fputs("</li>", stream) == EOF) goto cleanup;
    }
    if (fputs("</ul><script>'use strict';</script></body></html>\n",
              stream) == EOF) goto cleanup;
    status = 0;
cleanup:
    if (hwa_c_numeric_locale_end(&locale) != 0) return -1;
    return status;
}

int hwa_gap_report_html(FILE *stream,
                        const char *bundle_directory,
                        const HWAGapReportResult *result)
{
    return result == NULL ? -1 : hwa_gro_html_with_work(
        stream, bundle_directory, result, result->retained_work_bytes);
}

typedef int (*HWAGROutWriter)(FILE *, const HWAGapReportResult *);

static int hwa_gro_write_json_adapter(
    FILE *stream,
    const HWAGapReportResult *result)
{
    return hwa_gap_report_json(stream, result);
}

static int hwa_gro_write_csv_adapter(
    FILE *stream,
    const HWAGapReportResult *result)
{
    char error[HWA_ERROR_SIZE];
    return hwa_gap_report_file_write(
        stream, result, error, sizeof(error));
}

static int hwa_gro_seek_start(FILE *stream)
{
#if defined(_WIN32)
    return _fseeki64(stream, 0, SEEK_SET);
#else
    return fseeko(stream, 0, SEEK_SET);
#endif
}

static int hwa_gro_stream_size(FILE *stream, uint64_t *bytes)
{
#if defined(_WIN32)
    __int64 position;
    if (_fseeki64(stream, 0, SEEK_END) != 0 ||
        (position = _ftelli64(stream)) < 0) return -1;
#else
    off_t position;
    if (fseeko(stream, 0, SEEK_END) != 0 ||
        (position = ftello(stream)) < 0) return -1;
#endif
    *bytes = (uint64_t)position;
    return hwa_gro_seek_start(stream);
}

static int hwa_gro_same_node(const HWAGROutIdentity *a,
                             const HWAGROutIdentity *b)
{
    return a->device == b->device && a->file == b->file;
}

static int hwa_gro_after_close_match(const HWAGROutIdentity *before,
                                     const HWAGROutIdentity *after)
{
#if defined(_WIN32)
    /* Windows can finalize write/change times when the last handle closes. */
    return hwa_gro_same_node(before, after) && before->size == after->size;
#else
    return hwa_gro_identity_equal(before, after);
#endif
}

static int hwa_gro_commit_scratch(const char *path,
                                  FILE *scratch,
                                  uint64_t maximum,
                                  uint64_t *bytes,
                                  HWAGROutIdentity *accepted,
                                  char *error,
                                  size_t error_size)
{
    unsigned char buffer[65536];
    FILE *stream = NULL;
    HWAGROutIdentity owned;
    HWAGROutIdentity current;
    uint64_t length = 0U;
    uint64_t written = 0U;
    size_t count;
    int descriptor = -1;
    int created = 0;
    int status = -1;
    memset(&owned, 0, sizeof(owned));
    *bytes = 0U;
    if (fflush(scratch) != 0 ||
        hwa_gro_stream_size(scratch, &length) != 0 ||
        length > maximum) goto cleanup;
#if defined(_WIN32)
    descriptor = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                       _S_IREAD | _S_IWRITE);
#else
    descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL
#ifdef O_NOFOLLOW
                      | O_NOFOLLOW
#endif
                      , S_IRUSR | S_IWUSR);
#endif
    if (descriptor < 0 ||
        hwa_gro_descriptor_identity(descriptor, &owned) != 0 ||
        owned.size != 0U) {
        if (descriptor >= 0) {
#if defined(_WIN32)
            (void)_close(descriptor);
#else
            (void)close(descriptor);
#endif
        }
        goto cleanup;
    }
    created = 1;
#if defined(_WIN32)
    stream = _fdopen(descriptor, "wb");
#else
    stream = fdopen(descriptor, "wb");
#endif
    if (stream == NULL) {
#if defined(_WIN32)
        (void)_close(descriptor);
#else
        (void)close(descriptor);
#endif
        goto cleanup;
    }
    if (hwa_gro_stream_identity(stream, &owned) != 0 || owned.size != 0U)
        goto cleanup;
    while ((count = fread(buffer, 1U, sizeof(buffer), scratch)) != 0U) {
        if ((uint64_t)count > maximum - written ||
            fwrite(buffer, 1U, count, stream) != count) goto cleanup;
        written += (uint64_t)count;
    }
    if (ferror(scratch) || written != length || fflush(stream) != 0)
        goto cleanup;
#if defined(_WIN32)
    if (_commit(descriptor) != 0) goto cleanup;
#else
    if (fsync(descriptor) != 0) goto cleanup;
#endif
    {
        int identity_error = hwa_gro_stream_identity(stream, &owned);
        int size_error = owned.size != length;
        int close_error = fclose(stream);
        stream = NULL;
        if (identity_error != 0 || size_error || close_error != 0)
            goto cleanup;
    }
    if (hwa_gro_path_identity(path, 0, &current) != 0 ||
        !hwa_gro_after_close_match(&owned, &current)) goto cleanup;
    owned = current;
    *bytes = length;
    if (accepted != NULL) *accepted = current;
    status = 0;
cleanup:
    if (stream != NULL) {
        (void)hwa_gro_stream_identity(stream, &owned);
        (void)fclose(stream);
    }
    if (status != 0 && created &&
        hwa_gro_path_identity(path, 0, &current) == 0 &&
        hwa_gro_after_close_match(&owned, &current)) {
        owned = current;
        if (hwa_gro_path_identity(path, 0, &current) == 0 &&
            hwa_gro_identity_equal(&owned, &current)) (void)remove(path);
    }
    if (status != 0)
        hwa_gro_error(error, error_size,
                      "cannot create a bounded Stage 9 report file");
    return status;
}

static int hwa_gro_create_file(const char *path,
                               const HWAGapReportResult *result,
                               HWAGROutWriter writer,
                               uint64_t maximum,
                               uint64_t live_work,
                               uint64_t *bytes,
                               HWAGROutIdentity *accepted,
                               char *error,
                               size_t error_size)
{
    FILE *scratch = NULL;
    HWAGROutIdentity owned = {0};
    HWAGROutIdentity current;
    int committed = 0;
    int status = -1;
    *bytes = 0U;
    if (maximum > result->options.max_output_file_bytes)
        maximum = result->options.max_output_file_bytes;
    if (hwa_gro_work_preflight(result, live_work) != 0 ||
        hwa_gro_preflight(
            result,
            writer == hwa_gro_write_json_adapter
                ? HWA_GR_RENDER_JSON : HWA_GR_RENDER_CSV,
            maximum) != 0) {
        hwa_gro_error(error, error_size,
                      "Stage 9 report cannot fit its output limits");
        return -1;
    }
    scratch = tmpfile();
    if (scratch != NULL && writer(scratch, result) == 0 &&
        hwa_gro_commit_scratch(path, scratch, maximum, bytes,
                               &owned, error, error_size) == 0) {
        committed = 1;
        status = 0;
    }
    if (scratch != NULL && fclose(scratch) != 0) status = -1;
    if (status != 0 && committed &&
        hwa_gro_path_identity(path, 0, &current) == 0 &&
        hwa_gro_identity_equal(&owned, &current)) (void)remove(path);
    if (status == 0 && accepted != NULL) *accepted = owned;
    if (status != 0 &&
        (error == NULL || error_size == 0U || error[0] == '\0')) {
        hwa_gro_error(error, error_size,
                      "cannot create a Stage 9 report file");
    }
    return status;
}

static int hwa_gro_create_html(const char *path,
                               const char *directory,
                               const HWAGapReportResult *result,
                               uint64_t maximum,
                               uint64_t live_work,
                               uint64_t *bytes,
                               HWAGROutIdentity *accepted,
                               char *error,
                               size_t error_size)
{
    FILE *scratch = NULL;
    HWAGROutIdentity owned = {0};
    HWAGROutIdentity current;
    int committed = 0;
    int status = -1;
    *bytes = 0U;
    if (maximum > result->options.max_output_file_bytes)
        maximum = result->options.max_output_file_bytes;
    if (hwa_gro_work_preflight(result, live_work) != 0 ||
        hwa_gro_preflight(result, HWA_GR_RENDER_HTML, maximum) != 0) {
        hwa_gro_error(error, error_size,
                      "Stage 9 HTML cannot fit its output limits");
        return -1;
    }
    scratch = tmpfile();
    if (scratch != NULL &&
        hwa_gro_html_with_work(
            scratch, directory, result, live_work) == 0 &&
        hwa_gro_commit_scratch(path, scratch, maximum, bytes,
                               &owned, error, error_size) == 0) {
        committed = 1;
        status = 0;
    }
    if (scratch != NULL && fclose(scratch) != 0) status = -1;
    if (status != 0 && committed &&
        hwa_gro_path_identity(path, 0, &current) == 0 &&
        hwa_gro_identity_equal(&owned, &current)) (void)remove(path);
    if (status == 0 && accepted != NULL) *accepted = owned;
    if (status != 0 &&
        (error == NULL || error_size == 0U || error[0] == '\0')) {
        hwa_gro_error(error, error_size,
                      "cannot create the Stage 9 HTML report");
    }
    return status;
}

static int hwa_gro_files_equal(const char *left, const char *right)
{
    unsigned char a[65536];
    unsigned char b[65536];
    FILE *first = NULL;
    FILE *second = NULL;
    HWAGROutIdentity before_first;
    HWAGROutIdentity before_second;
    HWAGROutIdentity opened_first;
    HWAGROutIdentity opened_second;
    HWAGROutIdentity after_first;
    HWAGROutIdentity after_second;
    int status = -1;
    if (hwa_gro_path_identity(left, 0, &before_first) != 0 ||
        hwa_gro_path_identity(right, 0, &before_second) != 0 ||
        before_first.size != before_second.size) goto cleanup;
    first = hwa_gro_open_read(left);
    second = hwa_gro_open_read(right);
    if (first == NULL || second == NULL ||
        hwa_gro_stream_identity(first, &opened_first) != 0 ||
        hwa_gro_stream_identity(second, &opened_second) != 0 ||
        !hwa_gro_identity_equal(&before_first, &opened_first) ||
        !hwa_gro_identity_equal(&before_second, &opened_second)) goto cleanup;
    for (;;) {
        size_t ac = fread(a, 1U, sizeof(a), first);
        size_t bc = fread(b, 1U, sizeof(b), second);
        if (ac != bc || (ac != 0U && memcmp(a, b, ac) != 0)) goto cleanup;
        if (ac == 0U) break;
    }
    if (ferror(first) || ferror(second)) goto cleanup;
    {
        int first_status = fclose(first);
        int second_status = fclose(second);
        first = NULL;
        second = NULL;
        if (first_status != 0 || second_status != 0) goto cleanup;
    }
    first = NULL;
    second = NULL;
    if (hwa_gro_path_identity(left, 0, &after_first) != 0 ||
        hwa_gro_path_identity(right, 0, &after_second) != 0 ||
        !hwa_gro_identity_equal(&before_first, &after_first) ||
        !hwa_gro_identity_equal(&before_second, &after_second)) goto cleanup;
    status = 0;
cleanup:
    if (first != NULL) (void)fclose(first);
    if (second != NULL) (void)fclose(second);
    return status;
}

static int hwa_gro_file_equals_stream(const char *path,
                                      FILE *expected,
                                      uint64_t maximum,
                                      uint64_t *bytes)
{
    unsigned char a[65536];
    unsigned char b[65536];
    FILE *actual = NULL;
    HWAGROutIdentity before;
    HWAGROutIdentity opened;
    HWAGROutIdentity after;
    uint64_t length = 0U;
    int status = -1;
    if (fflush(expected) != 0 ||
        hwa_gro_stream_size(expected, &length) != 0 || length > maximum ||
        hwa_gro_path_identity(path, 0, &before) != 0 ||
        before.size != length) goto cleanup;
    actual = hwa_gro_open_read(path);
    if (actual == NULL || hwa_gro_stream_identity(actual, &opened) != 0 ||
        !hwa_gro_identity_equal(&before, &opened)) goto cleanup;
    for (;;) {
        size_t ac = fread(a, 1U, sizeof(a), actual);
        size_t bc = fread(b, 1U, sizeof(b), expected);
        if (ac != bc || (ac != 0U && memcmp(a, b, ac) != 0)) goto cleanup;
        if (ac == 0U) break;
    }
    {
        int actual_error = ferror(actual);
        int expected_error = ferror(expected);
        int close_error = fclose(actual);
        actual = NULL;
        if (actual_error || expected_error || close_error != 0)
            goto cleanup;
    }
    if (hwa_gro_path_identity(path, 0, &after) != 0 ||
        !hwa_gro_identity_equal(&before, &after)) goto cleanup;
    *bytes = length;
    status = 0;
cleanup:
    if (actual != NULL) (void)fclose(actual);
    return status;
}

static int hwa_gro_add_bytes(uint64_t *total, uint64_t value)
{
    if (value > UINT64_MAX - *total) return -1;
    *total += value;
    return 0;
}

static int hwa_gro_compare_report(
    const char *path,
    const char *bundle_directory,
    const HWAGapReportResult *result,
    HWAGROutWriter writer,
    int html,
    uint64_t live_work,
    uint64_t *bytes)
{
    FILE *scratch = NULL;
    uint64_t maximum;
    int status = -1;
    if (result->total_output_bytes > result->options.max_bundle_bytes)
        return -1;
    maximum = result->options.max_bundle_bytes -
              result->total_output_bytes;
    if (maximum > result->options.max_output_file_bytes)
        maximum = result->options.max_output_file_bytes;
    if (hwa_gro_work_preflight(result, live_work) != 0 ||
        hwa_gro_preflight(
            result, html ? HWA_GR_RENDER_HTML :
                (writer == hwa_gro_write_json_adapter
                    ? HWA_GR_RENDER_JSON : HWA_GR_RENDER_CSV),
            maximum) != 0) return -1;
    scratch = tmpfile();
    if (scratch != NULL &&
        (html ? hwa_gro_html_with_work(
                    scratch, bundle_directory, result, live_work)
              : writer(scratch, result)) == 0 &&
        hwa_gro_file_equals_stream(
            path, scratch, maximum, bytes) == 0) status = 0;
    if (scratch != NULL && fclose(scratch) != 0) status = -1;
    return status;
}

static int hwa_gro_bundle_exact(const char *root,
                                const HWAGapReportResult *result,
                                uint64_t initial_live_work,
                                HWAGROutIdentity *root_accepted,
                                HWAGROutIdentity *audio_accepted,
                                uint64_t *exact_bytes)
{
    char *audio = NULL;
    char *path = NULL;
    HWAGROutIdentity root_before;
    HWAGROutIdentity root_after;
    HWAGROutIdentity audio_before;
    HWAGROutIdentity audio_after;
    uint64_t bytes = 0U;
    uint64_t audio_bytes = 0U;
    uint64_t file_bytes = 0U;
    uint64_t live_work = initial_live_work;
    size_t index;
    int status = -1;
    if (root == NULL || result == NULL || exact_bytes == NULL ||
        hwa_gro_work_preflight(result, live_work) != 0 ||
        hwa_gro_path_identity(root, 1, &root_before) != 0 ||
        hwa_gro_join(&audio, root, "audio", &live_work,
                     result->options.max_work_bytes) != 0 ||
        hwa_gro_path_identity(audio, 1, &audio_before) != 0 ||
        hwa_gro_directory_exact(root, result, 0, &live_work) != 0 ||
        hwa_gro_directory_exact(audio, result, 1, &live_work) != 0)
        goto cleanup;
    for (index = 0U; index < result->excerpt_count; ++index) {
        const HWAGapReportExcerpt *r = &result->excerpts[index];
#define CLIP(field, size_field, sha_field)                                 \
        do {                                                               \
            if (hwa_gro_join(&path, root, r->field, &live_work,            \
                             result->options.max_work_bytes) != 0 ||       \
                hwa_gro_hash_file(path,                                    \
                    result->options.max_output_file_bytes,                 \
                    r->size_field, r->sha_field, NULL) != 0 ||              \
                hwa_gro_add_bytes(&audio_bytes, r->size_field) != 0 ||      \
                hwa_gro_add_bytes(&bytes, r->size_field) != 0)             \
                goto cleanup;                                              \
            hwa_gro_join_free(&path, &live_work);                          \
        } while (0)
        if (r->availability != HWA_GAP_REPORT_AVAILABLE) continue;
        CLIP(reference_path, reference_file_bytes, reference_sha256);
        CLIP(model_path, model_file_bytes, model_sha256);
        if (r->make_x) CLIP(x_path, x_file_bytes, x_sha256);
#undef CLIP
    }
    if (audio_bytes != result->total_output_bytes ||
        hwa_gro_join(&path, root, "result.hwa-report", &live_work,
                     result->options.max_work_bytes) != 0 ||
        hwa_gro_compare_report(
            path, root, result, hwa_gro_write_csv_adapter, 0,
            live_work,
            &file_bytes) != 0 ||
        hwa_gro_add_bytes(&bytes, file_bytes) != 0) goto cleanup;
    hwa_gro_join_free(&path, &live_work);
    if (result->mode == HWA_GAP_REPORT_FULL) {
        if (hwa_gro_join(&path, root, "report.csv", &live_work,
                         result->options.max_work_bytes) != 0 ||
            hwa_gro_compare_report(
                path, root, result, hwa_gro_write_csv_adapter, 0,
                live_work,
                &file_bytes) != 0 ||
            hwa_gro_add_bytes(&bytes, file_bytes) != 0) goto cleanup;
        hwa_gro_join_free(&path, &live_work);
        if (hwa_gro_join(&path, root, "report.json", &live_work,
                         result->options.max_work_bytes) != 0 ||
            hwa_gro_compare_report(
                path, root, result, hwa_gro_write_json_adapter, 0,
                live_work,
                &file_bytes) != 0 ||
            hwa_gro_add_bytes(&bytes, file_bytes) != 0) goto cleanup;
        hwa_gro_join_free(&path, &live_work);
        if (hwa_gro_join(&path, root, "report.html", &live_work,
                         result->options.max_work_bytes) != 0 ||
            hwa_gro_compare_report(
                path, root, result, NULL, 1, live_work,
                &file_bytes) != 0 ||
            hwa_gro_add_bytes(&bytes, file_bytes) != 0) goto cleanup;
        hwa_gro_join_free(&path, &live_work);
    }
    if (bytes > result->options.max_bundle_bytes ||
        hwa_gro_path_identity(root, 1, &root_after) != 0 ||
        hwa_gro_path_identity(audio, 1, &audio_after) != 0 ||
        !hwa_gro_identity_equal(&root_before, &root_after) ||
        !hwa_gro_identity_equal(&audio_before, &audio_after)) goto cleanup;
    if (root_accepted != NULL) *root_accepted = root_after;
    if (audio_accepted != NULL) *audio_accepted = audio_after;
    *exact_bytes = bytes;
    status = 0;
cleanup:
    hwa_gro_join_free(&path, &live_work);
    hwa_gro_join_free(&audio, &live_work);
    return status;
}

static void hwa_gro_remove_regular(const char *path,
                                   const HWAGROutIdentity *owned)
{
    HWAGROutIdentity current;
    if (path != NULL && owned != NULL &&
        hwa_gro_path_identity(path, 0, &current) == 0 &&
        hwa_gro_identity_equal(owned, &current))
        (void)remove(path);
}

int hwa_gap_report_output_write_tree(const char *private_directory,
                                     HWAGapReportMode mode,
                                     HWAGapReportResult *result,
                                     uint64_t outer_live_work,
                                     char *error,
                                     size_t error_size)
{
    char *result_path = NULL;
    char *csv_path = NULL;
    char *json_path = NULL;
    char *html_path = NULL;
    uint64_t result_bytes = 0U;
    uint64_t csv_bytes = 0U;
    uint64_t json_bytes = 0U;
    uint64_t html_bytes = 0U;
    HWAGROutIdentity result_identity = {0};
    HWAGROutIdentity csv_identity = {0};
    HWAGROutIdentity json_identity = {0};
    HWAGROutIdentity html_identity = {0};
    uint64_t tree_bytes;
    uint64_t remaining;
    uint64_t live_work;
    int result_owned = 0;
    int csv_owned = 0;
    int json_owned = 0;
    int html_owned = 0;
    int status = -1;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (private_directory == NULL || private_directory[0] == '\0' ||
        result == NULL || result->mode != mode ||
        mode == HWA_GAP_REPORT_RANK ||
        !hwa_gro_arrays_present(result) ||
        result->retained_work_bytes > result->options.max_work_bytes ||
        outer_live_work > result->options.max_work_bytes -
                              result->retained_work_bytes) {
        hwa_gro_error(error, error_size,
                      "invalid Stage 9 report tree arguments");
        goto cleanup;
    }
    live_work = result->retained_work_bytes + outer_live_work;
    if (hwa_gro_work_preflight(result, live_work) != 0 ||
        hwa_gap_report_result_validate(
            result, error, error_size) != 0) {
        hwa_gro_error(error, error_size,
                      "invalid Stage 9 report tree arguments");
        goto cleanup;
    }
    if (hwa_gro_join(
            &result_path, private_directory, "result.hwa-report",
            &live_work, result->options.max_work_bytes) != 0) {
        hwa_gro_error(error, error_size,
                      "Stage 9 report paths exceed the work limit");
        goto cleanup;
    }
    if (result->total_output_bytes > result->options.max_bundle_bytes)
        goto cleanup;
    remaining = result->options.max_bundle_bytes -
                result->total_output_bytes;
    if (hwa_gro_create_file(
            result_path, result, hwa_gro_write_csv_adapter,
            remaining, live_work, &result_bytes, &result_identity,
            error, error_size) != 0) goto cleanup;
    result_owned = 1;
    remaining -= result_bytes;
    if (mode == HWA_GAP_REPORT_FULL) {
        if (hwa_gro_join(&csv_path, private_directory, "report.csv",
                         &live_work, result->options.max_work_bytes) != 0 ||
            hwa_gro_join(&json_path, private_directory, "report.json",
                         &live_work, result->options.max_work_bytes) != 0 ||
            hwa_gro_join(&html_path, private_directory, "report.html",
                         &live_work, result->options.max_work_bytes) != 0)
            goto cleanup;
        if (hwa_gro_create_file(
                csv_path, result, hwa_gro_write_csv_adapter,
                remaining, live_work, &csv_bytes, &csv_identity,
                error, error_size) != 0) goto cleanup;
        csv_owned = 1;
        if (csv_bytes != result_bytes ||
            hwa_gro_files_equal(result_path, csv_path) != 0) goto cleanup;
        remaining -= csv_bytes;
        if (hwa_gro_create_file(
                json_path, result, hwa_gro_write_json_adapter,
                remaining, live_work, &json_bytes, &json_identity,
                error, error_size) != 0)
            goto cleanup;
        json_owned = 1;
        remaining -= json_bytes;
        if (hwa_gro_create_html(
                html_path, private_directory, result,
                remaining, live_work, &html_bytes, &html_identity,
                error, error_size) != 0)
            goto cleanup;
        html_owned = 1;
        remaining -= html_bytes;
    }
    tree_bytes = result->total_output_bytes;
#define ADD(value)                                                         \
    do { if ((value) > UINT64_MAX - tree_bytes) goto cleanup;              \
         tree_bytes += (value); } while (0)
    ADD(result_bytes);
    ADD(csv_bytes);
    ADD(json_bytes);
    ADD(html_bytes);
#undef ADD
    if (tree_bytes > result->options.max_bundle_bytes ||
        hwa_gro_bundle_exact(private_directory, result, live_work,
                             NULL, NULL,
                             &tree_bytes) != 0) {
        hwa_gro_error(error, error_size,
                      "Stage 9 report tree exceeds its bundle limit");
        goto cleanup;
    }
    status = 0;
cleanup:
    if (status != 0) {
        hwa_gro_remove_regular(
            html_path, html_owned ? &html_identity : NULL);
        hwa_gro_remove_regular(
            json_path, json_owned ? &json_identity : NULL);
        hwa_gro_remove_regular(
            csv_path, csv_owned ? &csv_identity : NULL);
        hwa_gro_remove_regular(
            result_path, result_owned ? &result_identity : NULL);
    }
    hwa_gro_join_free(&result_path, &live_work);
    hwa_gro_join_free(&csv_path, &live_work);
    hwa_gro_join_free(&json_path, &live_work);
    hwa_gro_join_free(&html_path, &live_work);
    return status;
}

typedef struct HWAGROwnedFile {
    char *path;
    HWAGROutIdentity identity;
} HWAGROwnedFile;

static int hwa_gro_owned_add(HWAGROwnedFile *files,
                             size_t capacity,
                             size_t *count,
                             const char *directory,
                             const char *relative,
                             uint64_t *live_work,
                             uint64_t max_work)
{
    HWAGROwnedFile *file;
    if (*count >= capacity) return -1;
    file = &files[*count];
    if (hwa_gro_join(&file->path, directory, relative,
                     live_work, max_work) != 0)
        return -1;
    if (hwa_gro_path_identity(file->path, 0, &file->identity) != 0) {
        hwa_gro_join_free(&file->path, live_work);
        return -1;
    }
    (*count)++;
    return 0;
}

int hwa_gap_report_output_remove(const HWAGapReportResult *result,
                                 char *error,
                                 size_t error_size)
{
    char *audio = NULL;
    HWAGROutIdentity root_first;
    HWAGROutIdentity audio_first;
    HWAGROutIdentity root_second;
    HWAGROutIdentity audio_second;
    HWAGROutIdentity current;
    HWAGROwnedFile *files = NULL;
    uint64_t exact_bytes = 0U;
    uint64_t live_work = 0U;
    uint64_t array_bytes = 0U;
    size_t capacity;
    size_t count = 0U;
    size_t index;
    int status = -1;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (result == NULL || result->mode == HWA_GAP_REPORT_RANK ||
        result->output_directory == NULL ||
        !hwa_gro_absolute_path(result->output_directory) ||
        hwa_gap_report_result_validate(
            result, error, error_size) != 0 ||
        hwa_gro_bundle_exact(
            result->output_directory, result,
            result->retained_work_bytes, &root_first, &audio_first,
            &exact_bytes) != 0) {
        hwa_gro_error(error, error_size,
                      "invalid Stage 9 output removal request");
        return -1;
    }
    if (hwa_gro_expected_audio_count(result, &capacity) != 0 ||
        (result->mode == HWA_GAP_REPORT_FULL ? 4U : 1U) >
            SIZE_MAX - capacity)
        goto cleanup;
    capacity += result->mode == HWA_GAP_REPORT_FULL ? 4U : 1U;
    live_work = result->retained_work_bytes;
    if (capacity > SIZE_MAX / sizeof(*files)) goto cleanup;
    array_bytes = (uint64_t)(capacity * sizeof(*files));
    if (live_work > result->options.max_work_bytes ||
        array_bytes > result->options.max_work_bytes - live_work)
        goto cleanup;
    live_work += array_bytes;
    if (
        (files = (HWAGROwnedFile *)calloc(
            capacity, sizeof(*files))) == NULL ||
        hwa_gro_join(&audio, result->output_directory, "audio",
                     &live_work, result->options.max_work_bytes) != 0)
        goto cleanup;
    for (index = 0U; index < result->excerpt_count; ++index) {
        const HWAGapReportExcerpt *r = &result->excerpts[index];
        if (r->availability != HWA_GAP_REPORT_AVAILABLE) continue;
        if (hwa_gro_owned_add(
                files, capacity, &count, result->output_directory,
                r->reference_path, &live_work,
                result->options.max_work_bytes) != 0 ||
            hwa_gro_owned_add(
                files, capacity, &count, result->output_directory,
                r->model_path, &live_work,
                result->options.max_work_bytes) != 0 ||
            (r->make_x &&
             hwa_gro_owned_add(
                 files, capacity, &count, result->output_directory,
                 r->x_path, &live_work,
                 result->options.max_work_bytes) != 0)) goto cleanup;
    }
    if (result->mode == HWA_GAP_REPORT_FULL) {
        if (hwa_gro_owned_add(files, capacity, &count,
                              result->output_directory, "report.html",
                              &live_work,
                              result->options.max_work_bytes) != 0 ||
            hwa_gro_owned_add(files, capacity, &count,
                              result->output_directory, "report.json",
                              &live_work,
                              result->options.max_work_bytes) != 0 ||
            hwa_gro_owned_add(files, capacity, &count,
                              result->output_directory, "report.csv",
                              &live_work,
                              result->options.max_work_bytes) != 0)
            goto cleanup;
    }
    if (hwa_gro_owned_add(files, capacity, &count,
                          result->output_directory,
                          "result.hwa-report", &live_work,
                          result->options.max_work_bytes) != 0 ||
        count != capacity ||
        hwa_gro_bundle_exact(
            result->output_directory, result, live_work,
            &root_second, &audio_second,
            &exact_bytes) != 0 ||
        !hwa_gro_identity_equal(&root_first, &root_second) ||
        !hwa_gro_identity_equal(&audio_first, &audio_second) ||
        hwa_gro_path_identity(
            result->output_directory, 1, &current) != 0 ||
        !hwa_gro_identity_equal(&root_second, &current) ||
        hwa_gro_path_identity(audio, 1, &current) != 0 ||
        !hwa_gro_identity_equal(&audio_second, &current)) goto cleanup;
    for (index = 0U; index < count; ++index) {
        if (hwa_gro_path_identity(files[index].path, 0, &current) != 0 ||
            !hwa_gro_identity_equal(&files[index].identity, &current))
            goto cleanup;
    }
    for (index = 0U; index < count; ++index) {
        if (hwa_gro_path_identity(files[index].path, 0, &current) != 0 ||
            !hwa_gro_identity_equal(&files[index].identity, &current) ||
            remove(files[index].path) != 0) goto cleanup;
    }
    if (hwa_gro_path_identity(audio, 1, &current) != 0 ||
        !hwa_gro_same_node(&audio_second, &current)) goto cleanup;
#if defined(_WIN32)
    if (!RemoveDirectoryA(audio)) goto cleanup;
#else
    if (rmdir(audio) != 0) goto cleanup;
#endif
    if (hwa_gro_path_identity(
            result->output_directory, 1, &current) != 0 ||
        !hwa_gro_same_node(&root_second, &current)) goto cleanup;
#if defined(_WIN32)
    if (!RemoveDirectoryA(result->output_directory)) goto cleanup;
#else
    if (rmdir(result->output_directory) != 0) goto cleanup;
#endif
    status = 0;
cleanup:
    for (index = 0U; index < count; ++index)
        hwa_gro_join_free(&files[index].path, &live_work);
    free(files);
    if (files != NULL && array_bytes <= live_work) live_work -= array_bytes;
    hwa_gro_join_free(&audio, &live_work);
    (void)exact_bytes;
    if (status != 0) {
        hwa_gro_error(error, error_size,
                      "Stage 9 output changed; refusing removal");
    }
    return status;
}

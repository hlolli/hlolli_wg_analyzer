#include "paired_spectrum_spans.h"

#include "alignment_file.h"
#include "inference_provider.h"
#include "numeric_locale.h"
#include "sha256.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct PairedMap {
    const HWAAlignment *alignment;
    const HWAFormat *reference;
    const HWAFormat *target;
    double *minimum_confidence;
    double *gap_ends;
    size_t target_gap_start;
    double held_fraction;
    double min_confidence;
} PairedMap;

typedef struct PairedBound {
    double time;
    size_t left;
    size_t right;
} PairedBound;

static int paired_map_init(PairedMap *map, uint64_t *remaining,
                           char *error, size_t error_size)
{
    const HWAAlignment *alignment = map->alignment;
    size_t anchors = alignment->anchor_count;
    size_t gap_count = alignment->unmatched_span_count;
    uint64_t bytes = ((uint64_t)anchors * 2U + (uint64_t)gap_count) *
                     sizeof(double);
    size_t index;
    double gap_end = 0.0;

    if (bytes > *remaining) {
        hwa_set_error(error, error_size, "alignment index exceeds the work limit");
        return -1;
    }
    if (bytes != 0U) {
        map->minimum_confidence = malloc((size_t)bytes);
        if (map->minimum_confidence == NULL) {
            hwa_set_error(error, error_size, "cannot allocate alignment index");
            return -1;
        }
        map->gap_ends = map->minimum_confidence + anchors * 2U;
    }
    *remaining -= bytes;

    /* Leaves hold anchor confidence. Each parent holds the minimum of its children. */
    for (index = 0U; index < anchors; ++index)
        map->minimum_confidence[anchors + index] = alignment->anchors[index].confidence;
    for (index = anchors; index > 1U;) {
        --index;
        map->minimum_confidence[index] = fmin(map->minimum_confidence[2U * index],
                                               map->minimum_confidence[2U * index + 1U]);
    }

    /* Within each recording, keep a prefix maximum so overlaps take one lookup. */
    map->target_gap_start = gap_count;
    for (index = 0U; index < gap_count; ++index) {
        const HWAUnmatchedSpan *span = &alignment->unmatched_spans[index];
        if (span->side == HWA_ALIGNMENT_TARGET && map->target_gap_start == gap_count) {
            map->target_gap_start = index;
            gap_end = 0.0;
        }
        if (span->reason != HWA_UNMATCHED_LOW_CONFIDENCE &&
            span->reason != HWA_UNMATCHED_NO_EVIDENCE &&
            span->start_seconds < span->end_seconds)
            gap_end = fmax(gap_end, span->end_seconds);
        map->gap_ends[index] = gap_end;
    }
    return 0;
}

static double paired_anchor_time(const HWAAlignmentAnchor *anchor, int target)
{
    return target ? anchor->target_seconds : anchor->reference_seconds;
}

static int paired_map_bound(const PairedMap *map, double time, int reverse,
                            PairedBound *bound)
{
    const HWAAlignmentAnchor *anchors = map->alignment->anchors;
    size_t count = map->alignment->anchor_count;
    size_t first = 0U;
    size_t last = count;

    while (first < last) {
        size_t middle = first + (last - first) / 2U;
        if (paired_anchor_time(&anchors[middle], reverse) < time)
            first = middle + 1U;
        else
            last = middle;
    }
    if (first == count) return -1;
    bound->right = first;
    bound->time = paired_anchor_time(&anchors[first], !reverse);
    if (paired_anchor_time(&anchors[first], reverse) == time) {
        bound->left = first;
    } else {
        double source_left, source_right, destination_left, fraction;
        if (first == 0U) return -1;
        bound->left = first - 1U;
        source_left = paired_anchor_time(&anchors[bound->left], reverse);
        source_right = paired_anchor_time(&anchors[first], reverse);
        destination_left = paired_anchor_time(&anchors[bound->left], !reverse);
        fraction = (time - source_left) / (source_right - source_left);
        bound->time = destination_left + fraction * (bound->time - destination_left);
    }
    return 0;
}

static double paired_confidence(const PairedMap *map, size_t start, size_t end)
{
    size_t left = start + map->alignment->anchor_count;
    size_t right = end + map->alignment->anchor_count + 1U;
    double result = 1.0;

    while (left < right) {
        if (left % 2U != 0U) result = fmin(result, map->minimum_confidence[left++]);
        if (right % 2U != 0U) result = fmin(result, map->minimum_confidence[--right]);
        left /= 2U;
        right /= 2U;
    }
    return result;
}

static int paired_intersects_gap(const PairedMap *map, int target,
                                 double start, double end)
{
    size_t side_start = target ? map->target_gap_start : 0U;
    size_t first = side_start;
    size_t last = target ? map->alignment->unmatched_span_count : map->target_gap_start;

    while (first < last) {
        size_t middle = first + (last - first) / 2U;
        if (map->alignment->unmatched_spans[middle].start_seconds < end)
            first = middle + 1U;
        else
            last = middle;
    }
    return first > side_start && map->gap_ends[first - 1U] > start;
}

static uint64_t paired_sample(double seconds, const HWAFormat *format)
{
    double sample = floor(seconds * (double)format->sample_rate_hz + 0.5);
    return sample >= (double)format->frames ? format->frames : (uint64_t)sample;
}

static HwaPairedSampleSpan paired_sample_span(double start, double end,
                                              const HWAFormat *format)
{
    HwaPairedSampleSpan span;
    span.start_sample = paired_sample(start, format);
    span.end_sample = paired_sample(end, format);
    span.present = 1;
    span.valid = span.end_sample > span.start_sample;
    return span;
}

static void paired_skip(HwaPairedSpan *row, const char *reason)
{
    if (row->skip_reason == NULL) row->skip_reason = reason;
}

static void paired_map_span(const PairedMap *map, HwaPairedSpan *row,
                            double start, double end, int reverse)
{
    const HWAFormat *source = reverse ? map->target : map->reference;
    const HWAFormat *destination = reverse ? map->reference : map->target;
    HwaPairedSampleSpan *source_span = reverse ? &row->target : &row->reference;
    HwaPairedSampleSpan *target_span = reverse ? &row->reference : &row->target;
    HwaPairedSampleSpan *source_held = reverse ? &row->held_target : &row->held_reference;
    HwaPairedSampleSpan *target_held = reverse ? &row->held_reference : &row->held_target;
    double margin = 0.5 * (1.0 - map->held_fraction) * (end - start);
    double held_start = start + margin;
    double held_end = end - margin;
    PairedBound left, right;

    if (!source_span->valid) {
        paired_skip(row, "invalid-span");
        return;
    }
    *source_held = paired_sample_span(held_start, held_end, source);
    if (paired_map_bound(map, start, reverse, &left) != 0 ||
        paired_map_bound(map, end, reverse, &right) != 0) {
        paired_skip(row, "outside-alignment");
    } else {
        *target_span = paired_sample_span(left.time, right.time, destination);
        if (!target_span->valid) paired_skip(row, "collapsed-span");
    }
    if (paired_map_bound(map, held_start, reverse, &left) != 0 ||
        paired_map_bound(map, held_end, reverse, &right) != 0) {
        paired_skip(row, "outside-alignment");
        return;
    }
    *target_held = paired_sample_span(left.time, right.time, destination);
    if (!source_held->valid || !target_held->valid) {
        paired_skip(row, "collapsed-span");
        return;
    }
    if (!target_span->valid) return;
    row->confidence = fmin(row->confidence, paired_confidence(map, left.left, right.right));
    row->confidence_valid = 1;
    if (paired_intersects_gap(map, reverse, held_start, held_end) ||
        paired_intersects_gap(map, !reverse, left.time, right.time)) {
        paired_skip(row, "unmatched-alignment");
    } else if (row->confidence < map->min_confidence) {
        paired_skip(row, "low-alignment-confidence");
    }
}

enum PairedColumn {
    COLUMN_ID,
    COLUMN_START_SAMPLE,
    COLUMN_END_SAMPLE,
    COLUMN_EXPECTED_HZ,
    COLUMN_MIDI_NOTE,
    COLUMN_CONFIDENCE,
    COLUMN_COUNT
};

#define PAIRED_CSV_COLUMNS (COLUMN_COUNT + HWA_PAIRED_LABEL_MAX)

typedef struct PairedCsv {
    char *input;
    char *output;
    const char *end;
} PairedCsv;

static int paired_csv_row(PairedCsv *csv, char **fields, size_t *count)
{
    *count = 0U;
    do {
        char separator = '\0';

        if (*count == PAIRED_CSV_COLUMNS) return -1;
        fields[(*count)++] = csv->output;
        if (csv->input != csv->end && *csv->input == '"') {
            ++csv->input;
            for (;;) {
                char byte;
                if (csv->input == csv->end) return -1;
                byte = *csv->input++;
                if (byte == '\0') return -1;
                if (byte == '"') {
                    if (csv->input == csv->end || *csv->input != '"') break;
                    ++csv->input;
                }
                *csv->output++ = byte;
            }
        } else {
            while (csv->input != csv->end && *csv->input != ',' &&
                   *csv->input != '\r' && *csv->input != '\n') {
                if (*csv->input == '\0' || *csv->input == '"') return -1;
                *csv->output++ = *csv->input++;
            }
        }
        if (csv->input != csv->end) {
            separator = *csv->input++;
            if (separator == '\r') {
                if (csv->input == csv->end || *csv->input++ != '\n') return -1;
            } else if (separator != ',' && separator != '\n') {
                return -1;
            }
        }
        *csv->output++ = '\0';
        if (separator != ',') break;
    } while (1);
    return 0;
}

static int paired_csv_header(char *const *fields, size_t count, size_t *slots,
                             size_t *label_slots, HwaPairedSpans *result,
                             char *error, size_t error_size)
{
    static const char *const reserved[] = {
        "id", "start_sample", "end_sample", "expected_hz", "midi_note", "confidence"
    };
    size_t index, slot;

    for (slot = 0U; slot < COLUMN_COUNT; ++slot) slots[slot] = count;
    for (index = 0U; index < count; ++index) {
        size_t previous;
        for (previous = 0U; previous < index; ++previous) {
            if (strcmp(fields[index], fields[previous]) == 0) break;
        }
        if (previous < index || fields[index][0] == '\0') {
            hwa_set_error(error, error_size, "note CSV has a blank or duplicate column name");
            return -1;
        }
        for (slot = 0U; slot < COLUMN_COUNT; ++slot) {
            if (strcmp(fields[index], reserved[slot]) == 0) break;
        }
        if (slot < COLUMN_COUNT) {
            slots[slot] = index;
        } else {
            if (result->label_count == HWA_PAIRED_LABEL_MAX) {
                hwa_set_error(error, error_size, "note CSV has too many label columns");
                return -1;
            }
            label_slots[result->label_count] = index;
            result->label_names[result->label_count++] = fields[index];
        }
    }
    if (slots[COLUMN_ID] == count || slots[COLUMN_START_SAMPLE] == count ||
        slots[COLUMN_END_SAMPLE] == count) {
        hwa_set_error(error, error_size, "note CSV needs id, start_sample and end_sample columns");
        return -1;
    }
    return 0;
}

static int paired_u64(const char *value, uint64_t *result)
{
    uint64_t number = 0U;
    if (*value == '\0') return -1;
    for (; *value != '\0'; ++value) {
        uint64_t digit;
        if (*value < '0' || *value > '9') return -1;
        digit = (uint64_t)(*value - '0');
        if (number > (UINT64_MAX - digit) / 10U) return -1;
        number = number * 10U + digit;
    }
    *result = number;
    return 0;
}

static int paired_midi_hz(const char *text, double *expected_hz)
{
    uint64_t midi;
    int negative = *text == '-';
    if (*text == '+' || negative) ++text;
    if (paired_u64(text, &midi) != 0 || midi > 127U) return -1;
    if (negative && midi != 0U) return -1;
    *expected_hz = 440.0 * exp2(((double)midi - 69.0) / 12.0);
    return 0;
}

static int paired_csv_note(const PairedMap *map, char *const *fields,
                           const size_t *slots, const size_t *label_slots,
                           const HwaPairedSpans *result, const HWANumericLocale *locale,
                           int reverse, HwaPairedSpan *row)
{
    const char *hz = fields[slots[COLUMN_EXPECTED_HZ]];
    const char *midi = fields[slots[COLUMN_MIDI_NOTE]];
    const char *confidence = fields[slots[COLUMN_CONFIDENCE]];
    const HWAFormat *source = reverse ? map->target : map->reference;
    HwaPairedSampleSpan *span = reverse ? &row->target : &row->reference;
    size_t index;

    memset(row, 0, sizeof(*row));
    row->id = fields[slots[COLUMN_ID]];
    row->confidence = 1.0;
    if (*row->id == '\0' ||
        paired_u64(fields[slots[COLUMN_START_SAMPLE]], &span->start_sample) != 0 ||
        paired_u64(fields[slots[COLUMN_END_SAMPLE]], &span->end_sample) != 0 ||
        (*hz != '\0' && (*midi != '\0' ||
            hwa_c_locale_parse_double(locale, hz, &row->expected_hz) != 0 ||
            row->expected_hz <= 0.0)) ||
        (*midi != '\0' && paired_midi_hz(midi, &row->expected_hz) != 0) ||
        (*confidence != '\0' &&
         (hwa_c_locale_parse_double(locale, confidence, &row->confidence) != 0 ||
          row->confidence < 0.0 || row->confidence > 1.0))) return -1;

    for (index = 0U; index < result->label_count; ++index) {
        const char *value = fields[label_slots[index]];
        if (*value != '\0') {
            HwaPairedLabel *label = &row->labels[row->label_count++];
            label->name = result->label_names[index];
            label->value = value;
        }
    }
    span->present = 1;
    span->valid = span->start_sample < span->end_sample && span->end_sample <= source->frames;
    paired_map_span(map, row, (double)span->start_sample / source->sample_rate_hz,
                   (double)span->end_sample / source->sample_rate_hz, reverse);
    return 0;
}

static int paired_load_csv(const char *path, const HWAAlignmentFileLimits *limits,
                           uint64_t remaining, const PairedMap *map, int reverse,
                           const HWANumericLocale *locale,
                           HwaPairedSpans *result, char *error, size_t error_size)
{
    size_t size, field_count, count, slots[COLUMN_COUNT];
    size_t label_slots[HWA_PAIRED_LABEL_MAX];
    size_t row_capacity = 0U;
    size_t maximum_rows;
    char *fields[PAIRED_CSV_COLUMNS + 1U];
    unsigned char *storage;
    PairedCsv csv;
    uint64_t input_limit;

    if (remaining < 2U) {
        hwa_set_error(error, error_size, "insufficient work limit for note CSV");
        return -1;
    }
    input_limit = remaining - 1U < limits->max_bytes ? remaining - 1U : limits->max_bytes;
    if (hwa_sha256_read_file(path, input_limit, &result->csv_data, &size,
                            result->notes_sha256, error, error_size) != 0) return -1;
    storage = realloc(result->csv_data, size + 1U);
    if (storage == NULL) {
        hwa_set_error(error, error_size, "cannot allocate note CSV storage");
        return -1;
    }
    result->csv_data = storage;
    csv.input = csv.output = (char *)storage;
    csv.end = csv.input + size;
    if (size >= 3U && memcmp(storage, "\xef\xbb\xbf", 3U) == 0) csv.input += 3U;
    if (csv.input == csv.end || paired_csv_row(&csv, fields, &field_count) != 0) {
        hwa_set_error(error, error_size, "missing or malformed note CSV header");
        return -1;
    }
    if (paired_csv_header(fields, field_count, slots, label_slots, result,
                          error, error_size) != 0) return -1;

    remaining -= (uint64_t)size + 1U;
    maximum_rows = limits->max_matches;
    if (remaining / sizeof(*result->rows) < maximum_rows)
        maximum_rows = (size_t)(remaining / sizeof(*result->rows));
    while (csv.input != csv.end) {
        if (paired_csv_row(&csv, fields, &count) != 0 || count != field_count) {
            hwa_set_error(error, error_size, "malformed note CSV record %zu", result->count + 2U);
            return -1;
        }
        if (result->count == row_capacity) {
            HwaPairedSpan *rows;
            size_t requested = row_capacity == 0U ? 16U : row_capacity * 2U;
            if (requested > maximum_rows) requested = maximum_rows;
            if (requested == row_capacity) {
                hwa_set_error(error, error_size, "note CSV exceeds the row or work limit");
                return -1;
            }
            rows = realloc(result->rows, requested * sizeof(*rows));
            if (rows == NULL) {
                hwa_set_error(error, error_size, "cannot allocate note CSV rows");
                return -1;
            }
            result->rows = rows;
            row_capacity = requested;
        }
        fields[count] = "";
        if (paired_csv_note(map, fields, slots, label_slots, result, locale,
                            reverse, &result->rows[result->count]) != 0) {
            hwa_set_error(error, error_size, "invalid note CSV values in record %zu", result->count + 2U);
            return -1;
        }
        ++result->count;
    }
    result->retained_work_bytes = (uint64_t)size + 1U +
                                  (uint64_t)row_capacity * sizeof(*result->rows);
    return 0;
}

static int paired_read_alignment(const char *path, const HWAAlignmentFileLimits *limits,
                                 HWAAlignment *alignment, char hash[HWA_SHA256_HEX_SIZE],
                                 char *error, size_t error_size)
{
    char after[HWA_SHA256_HEX_SIZE];
    uint64_t file_limit = limits->max_work_bytes / 2U;

    if (file_limit > limits->max_bytes) file_limit = limits->max_bytes;
    if (file_limit == 0U) {
        hwa_set_error(error, error_size, "insufficient work limit for an alignment file");
        return -1;
    }
    if (hwa_sha256_file(path, file_limit, hash, error, error_size) != 0 ||
        hwa_alignment_file_read(path, limits, alignment, error, error_size) != 0 ||
        hwa_sha256_file(path, file_limit, after, error, error_size) != 0)
        return -1;
    if (strcmp(hash, after) != 0) {
        hwa_set_error(error, error_size, "alignment changed while it was read");
        return -1;
    }
    return 0;
}

static int paired_load_score(const char *path, HWAAlignmentFileLimits limits,
                             uint64_t remaining, const PairedMap *map,
                             HwaPairedSpans *result, char *error, size_t error_size)
{
    static const char *const keys[] = {
        "voice", "midi_note", "velocity", "tie", "dynamic", "mark", "score_position"
    };
    HWAAlignment *score = &result->score_alignment;
    const HWAFormat *source;
    unsigned present = 0U;
    size_t index, count = 0U;
    int reverse;

    limits.max_work_bytes = remaining;
    if (paired_read_alignment(path, &limits, score, result->notes_sha256, error, error_size) != 0)
        return -1;
    if (score->mode != HWA_ALIGNMENT_SCORE_TO_AUDIO) {
        hwa_set_error(error, error_size, "score alignment must be a score-to-audio alignment");
        return -1;
    }
    reverse = strcmp(score->target_sha256, result->reference_sha256) != 0;
    if (reverse && strcmp(score->target_sha256, result->target_sha256) != 0) {
        hwa_set_error(error, error_size, "score alignment WAVE SHA-256 matches neither recording");
        return -1;
    }
    result->notes_on_target = reverse;
    source = reverse ? map->target : map->reference;
    if (source->duration_seconds != score->target_duration_seconds) {
        hwa_set_error(error, error_size, "WAVE duration differs from the score alignment target");
        return -1;
    }

    for (index = 0U; index < score->match_count; ++index) {
        if (strcmp(score->matches[index].kind, "note") == 0) ++count;
    }
    if ((uint64_t)count > (remaining - score->retained_work_bytes) / sizeof(*result->rows)) {
        hwa_set_error(error, error_size, "score note rows exceed the work limit");
        return -1;
    }
    if (count != 0U) {
        result->rows = calloc(count, sizeof(*result->rows));
        if (result->rows == NULL) {
            hwa_set_error(error, error_size, "cannot allocate score note rows");
            return -1;
        }
    }
    for (index = 0U; index < score->match_count; ++index) {
        const HWAAlignmentMatch *match = &score->matches[index];
        const char *const values[] = {
            match->voice, match->midi_note, match->velocity, match->tie,
            match->dynamic, match->mark, match->score_position
        };
        HwaPairedSpan *row;
        HwaPairedSampleSpan *source_span;
        size_t label_index;

        if (strcmp(match->kind, "note") != 0) continue;
        row = &result->rows[result->count++];
        row->id = match->event_id;
        row->confidence = match->confidence;
        if (match->midi_note != NULL && paired_midi_hz(match->midi_note, &row->expected_hz) != 0) {
            hwa_set_error(error, error_size, "score note '%s' has an invalid MIDI pitch", row->id);
            return -1;
        }
        for (label_index = 0U; label_index < sizeof(keys) / sizeof(*keys); ++label_index) {
            if (values[label_index] != NULL) {
                HwaPairedLabel *label = &row->labels[row->label_count++];
                label->name = keys[label_index];
                label->value = values[label_index];
                present |= 1U << label_index;
            }
        }
        if (match->status == HWA_ALIGNMENT_LOW_CONFIDENCE)
            row->skip_reason = "low-alignment-confidence";
        else if (match->status != HWA_ALIGNMENT_MATCHED)
            row->skip_reason = "score-unmatched";
        source_span = reverse ? &row->target : &row->reference;
        *source_span = paired_sample_span(match->target_start_seconds,
                                          match->target_end_seconds, source);
        paired_map_span(map, row, match->target_start_seconds, match->target_end_seconds, reverse);
    }
    for (index = 0U; index < sizeof(keys) / sizeof(*keys); ++index) {
        if ((present & (1U << index)) != 0U)
            result->label_names[result->label_count++] = keys[index];
    }
    result->retained_work_bytes = score->retained_work_bytes + (uint64_t)count * sizeof(*result->rows);
    return 0;
}

static int paired_duration_agrees(const HWAFormat *recording, const HWAFormat *proxy)
{
    long double recording_rate = (long double)recording->sample_rate_hz;
    long double proxy_rate = (long double)proxy->sample_rate_hz;
    long double scaled_frames = (long double)recording->frames * proxy_rate;
    long double scaled_proxy = (long double)proxy->frames * recording_rate;

    /* Compare durations within one sample on each clock before dividing. */
    return fabsl(scaled_frames - scaled_proxy) <= recording_rate + proxy_rate;
}

static int paired_open_wave(const char *path, const char *proxy_path, const char *side,
                             const char *expected_sha256, double expected_duration,
                             uint64_t max_bytes, HWAWavReader *reader,
                             char hash[HWA_SHA256_HEX_SIZE], char alignment_hash[HWA_SHA256_HEX_SIZE],
                             char *error, size_t error_size)
{
    HWAWavReader proxy = {0};
    HWAWavReader *alignment_reader = proxy_path == NULL ? reader : &proxy;
    int status = -1;

    if (hwa_wav_reader_open(reader, path, max_bytes, error, error_size) != 0) return -1;
    reader->source.name = path;
    if (proxy_path != NULL) {
        if (hwa_wav_reader_open(&proxy, proxy_path, max_bytes, error, error_size) != 0) goto cleanup;
        proxy.source.name = proxy_path;
    }
    if (alignment_reader->format.duration_seconds != expected_duration) {
        hwa_set_error(error, error_size, "%s WAVE duration differs from the alignment", side);
        goto cleanup;
    }
    if (proxy_path != NULL && !paired_duration_agrees(&reader->format, &proxy.format)) {
        hwa_set_error(error, error_size, "%s recording duration differs from its alignment WAVE", side);
        goto cleanup;
    }
    if (hwa_inference_byte_source_sha256(&alignment_reader->source, max_bytes,
                                         alignment_hash, error, error_size) != 0) goto cleanup;
    if (strcmp(alignment_hash, expected_sha256) != 0) {
        hwa_set_error(error, error_size, "%s WAVE SHA-256 differs from the alignment", side);
        goto cleanup;
    }
    if (proxy_path != NULL) {
        if (hwa_inference_byte_source_sha256(&reader->source, max_bytes, hash,
                                             error, error_size) != 0) goto cleanup;
    } else {
        memcpy(hash, alignment_hash, HWA_SHA256_HEX_SIZE);
    }
    status = 0;
cleanup:
    hwa_wav_reader_close(&proxy);
    return status;
}

int hwa_paired_spans_open(const char *reference_path, const char *target_path,
                          const char *alignment_path,
                          const char *alignment_reference_path, const char *alignment_target_path,
                          const char *notes_path, const char *score_alignment_path,
                          int notes_on_target,
                          const HWAAnalysisOptions *limits, double held_fraction,
                          double min_confidence, HwaPairedSpans *result,
                          char *error, size_t error_size)
{
    HWAAlignmentFileLimits file_limits;
    HWAAlignment alignment = {0};
    PairedMap map = {0};
    HWANumericLocale locale;
    uint64_t remaining;
    int status = -1;

    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (result != NULL) memset(result, 0, sizeof(*result));
    if (result == NULL || limits == NULL || reference_path == NULL || target_path == NULL ||
        alignment_path == NULL || (notes_path == NULL) == (score_alignment_path == NULL) ||
        strcmp(reference_path, "-") == 0 || strcmp(target_path, "-") == 0 ||
        (alignment_reference_path != NULL && strcmp(alignment_reference_path, "-") == 0) ||
        (alignment_target_path != NULL && strcmp(alignment_target_path, "-") == 0) ||
        (score_alignment_path != NULL && notes_on_target) ||
        !isfinite(held_fraction) || held_fraction <= 0.0 || held_fraction > 1.0 ||
        !isfinite(min_confidence) || min_confidence < 0.0 || min_confidence > 1.0) {
        hwa_set_error(error, error_size, "invalid paired-note span inputs");
        return -1;
    }
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_set_error(error, error_size, "cannot set the note input numeric locale");
        return -1;
    }
    hwa_alignment_file_limits_default(&file_limits);
    if (limits->max_work_bytes < file_limits.max_work_bytes)
        file_limits.max_work_bytes = limits->max_work_bytes;
    if (paired_read_alignment(alignment_path, &file_limits, &alignment,
                               result->alignment_sha256, error, error_size) != 0)
        goto cleanup;
    if (alignment.mode != HWA_ALIGNMENT_AUDIO_TO_AUDIO) {
        hwa_set_error(error, error_size, "comparison alignment must be an audio-to-audio alignment");
        goto cleanup;
    }
    if (paired_open_wave(reference_path, alignment_reference_path, "reference",
                          alignment.reference_sha256, alignment.reference_duration_seconds,
                          limits->max_input_bytes, &result->reference, result->reference_sha256,
                          result->alignment_reference_sha256, error, error_size) != 0 ||
        paired_open_wave(target_path, alignment_target_path, "target",
                          alignment.target_sha256, alignment.target_duration_seconds,
                          limits->max_input_bytes, &result->target, result->target_sha256,
                          result->alignment_target_sha256, error, error_size) != 0)
        goto cleanup;

    remaining = file_limits.max_work_bytes - alignment.retained_work_bytes;
    map.alignment = &alignment;
    map.reference = &result->reference.format;
    map.target = &result->target.format;
    map.held_fraction = held_fraction;
    map.min_confidence = min_confidence;
    if (paired_map_init(&map, &remaining, error, error_size) != 0) goto cleanup;
    if (notes_path != NULL) {
        result->notes_on_target = notes_on_target;
        status = paired_load_csv(notes_path, &file_limits, remaining, &map, notes_on_target,
                                 &locale, result, error, error_size);
    } else {
        status = paired_load_score(score_alignment_path, file_limits, remaining, &map, result,
                                   error, error_size);
    }

cleanup:
    free(map.minimum_confidence);
    hwa_alignment_free(&alignment);
    if (hwa_c_numeric_locale_end(&locale) != 0 && status == 0) {
        hwa_set_error(error, error_size, "cannot restore the note input numeric locale");
        status = -1;
    }
    if (status != 0) hwa_paired_spans_close(result);
    return status;
}

void hwa_paired_spans_close(HwaPairedSpans *result)
{
    if (result == NULL) return;
    free(result->rows);
    free(result->csv_data);
    hwa_alignment_free(&result->score_alignment);
    hwa_wav_reader_close(&result->reference);
    hwa_wav_reader_close(&result->target);
    memset(result, 0, sizeof(*result));
}

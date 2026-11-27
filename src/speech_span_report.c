#include "speech_span_report.h"

#include "file_output.h"
#include "numeric_locale.h"
#include "output.h"
#include "speech_span_audio.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SPEECH_MIN_CHANGE_DB 3.0
#define SPEECH_TRANSITION_MS 20U
#define SPEECH_CONTEXT_MS 150U
#define SPEECH_TIMING_SEARCH_MS 55U
#define SPEECH_EXCERPT_RMS 0.125

enum SpeechSide { SPEECH_REFERENCE, SPEECH_CANDIDATE, SPEECH_SIDE_COUNT };
enum SpeechKind { SPEECH_VOWEL, SPEECH_CONSONANT };

typedef struct SpeechBuzzSide {
    const char *status;
    const char *reason;
    double pitch_min_hz;
    double pitch_max_hz;
    double pitch_coverage;
    HWASpeechPeak fixed;
    HWASpeechPeak harmonic;
    HWASpeechPeak fixed_probe;
    HWASpeechPeak harmonic_probe;
} SpeechBuzzSide;

typedef struct SpeechExcerpt {
    HwaPairedSampleSpan bounds[SPEECH_SIDE_COUNT];
    HWAFileOutput output[SPEECH_SIDE_COUNT];
    double gain[SPEECH_SIDE_COUNT];
} SpeechExcerpt;

typedef struct SpeechRow {
    const HwaPairedSpan *phone;
    const struct SpeechRow *next_phone;
    const char *word;
    const char *phone_label;
    const char *vowel_id;
    const char *status;
    const char *reason;
    const char *classification;
    size_t index;
    size_t rank;
    enum SpeechKind kind;
    int release;
    double confidence;
    double score;
    HwaPairedSampleSpan transition[SPEECH_SIDE_COUNT];
    HwaPairedSampleSpan vowel[SPEECH_SIDE_COUNT];
    uint64_t transition_edge[SPEECH_SIDE_COUNT];
    int transition_edge_valid;
    uint64_t candidate_nearby_cue;
    union {
        SpeechBuzzSide buzz[SPEECH_SIDE_COUNT];
        HWASpeechContrast contrast[SPEECH_SIDE_COUNT];
    } measure;
    SpeechExcerpt excerpt;
} SpeechRow;

typedef struct SpeechReport {
    HwaPairedSpans spans;
    SpeechRow *rows;
    SpeechRow **ordered;
    HWASpeechWork work;
    HWASpeechAudioLevel levels[SPEECH_SIDE_COUNT];
    const char *protected_paths[6];
    size_t protected_count;
    double shared_nyquist;
    double min_confidence;
} SpeechReport;

static const HWAWavReader *speech_reader(const SpeechReport *report, size_t side)
{
    return side == SPEECH_REFERENCE ? &report->spans.reference : &report->spans.target;
}

static const HwaPairedSampleSpan *speech_phone_bounds(const HwaPairedSpan *row, size_t side)
{
    return side == SPEECH_REFERENCE ? &row->reference : &row->target;
}

static const char *speech_label(const HwaPairedSpan *row, const char *name)
{
    size_t index;

    for (index = 0U; index < row->label_count; ++index) {
        if (strcmp(row->labels[index].name, name) == 0) return row->labels[index].value;
    }
    return NULL;
}

static int speech_time_order(const void *left, const void *right)
{
    const SpeechRow *const *first = left;
    const SpeechRow *const *second = right;
    HwaPairedSampleSpan a = (*first)->phone->reference;
    HwaPairedSampleSpan b = (*second)->phone->reference;

    if (a.valid != b.valid) return a.valid ? -1 : 1;
    if (a.start_sample != b.start_sample) return a.start_sample < b.start_sample ? -1 : 1;
    if (a.end_sample != b.end_sample) return a.end_sample < b.end_sample ? -1 : 1;
    return (*first)->index < (*second)->index ? -1 : ((*first)->index != (*second)->index);
}

static int speech_id_order(const void *left, const void *right)
{
    const SpeechRow *const *first = left;
    const SpeechRow *const *second = right;
    return strcmp((*first)->phone->id, (*second)->phone->id);
}

static const SpeechRow *speech_find_row(const SpeechReport *report, const char *id)
{
    size_t first = 0U, last = report->spans.count;
    while (first < last) {
        size_t middle = first + (last - first) / 2U;
        int order = strcmp(id, report->ordered[middle]->phone->id);
        if (order == 0) return report->ordered[middle];
        if (order < 0) last = middle;
        else first = middle + 1U;
    }
    return NULL;
}

static int speech_load_labels(SpeechReport *report, char *error, size_t error_size)
{
    size_t index;

    for (index = 0U; index < report->spans.count; ++index) {
        SpeechRow *row = &report->rows[index];
        const HwaPairedSpan *phone = &report->spans.rows[index];
        const char *kind = speech_label(phone, "kind");
        const char *edge = speech_label(phone, "edge");

        row->phone = phone;
        row->index = index;
        row->word = speech_label(phone, "word");
        row->phone_label = speech_label(phone, "phone");
        row->vowel_id = speech_label(phone, "vowel_id");
        row->confidence = phone->confidence_valid ? phone->confidence : NAN;
        row->status = phone->skip_reason;
        report->ordered[index] = row;
        if (row->word == NULL || row->phone_label == NULL || kind == NULL ||
            (strcmp(kind, "vowel") != 0 && strcmp(kind, "consonant") != 0)) {
            hwa_set_error(error, error_size, "speech span '%s' needs word, phone and vowel/consonant kind",
                          phone->id);
            return -1;
        }
        row->kind = strcmp(kind, "vowel") == 0 ? SPEECH_VOWEL : SPEECH_CONSONANT;
        if (row->kind == SPEECH_CONSONANT) {
            if (row->vowel_id == NULL ||
                (edge != NULL && strcmp(edge, "start") != 0 && strcmp(edge, "end") != 0)) {
                hwa_set_error(error, error_size, "consonant '%s' needs a vowel_id and a start or end edge",
                              phone->id);
                return -1;
            }
            row->release = edge == NULL || strcmp(edge, "end") == 0;
        }
    }
    qsort(report->ordered, report->spans.count, sizeof(*report->ordered), speech_time_order);
    for (index = 1U; index < report->spans.count &&
         report->ordered[index]->phone->reference.valid; ++index) {
        report->ordered[index - 1U]->next_phone = report->ordered[index];
    }
    qsort(report->ordered, report->spans.count, sizeof(*report->ordered), speech_id_order);
    for (index = 1U; index < report->spans.count; ++index) {
        if (speech_id_order(&report->ordered[index - 1U], &report->ordered[index]) == 0) {
            hwa_set_error(error, error_size, "duplicate speech span id '%s'",
                          report->ordered[index]->phone->id);
            return -1;
        }
    }
    return 0;
}

static HwaPairedSpan speech_map(const SpeechReport *report, uint64_t start, uint64_t end)
{
    HwaPairedSpan row = {.reference = {.start_sample = start, .end_sample = end},
                         .confidence = 1.0};
    hwa_paired_spans_map_interval(&report->spans, &row, 0);
    return row;
}

static void speech_check_support(SpeechRow *row, const HwaPairedSpan *support)
{
    if (support->confidence_valid) row->confidence = fmin(row->confidence, support->confidence);
    if (row->status == NULL) row->status = support->skip_reason;
}

static uint64_t speech_padding(const SpeechReport *report, unsigned ms)
{
    return ((uint64_t)report->spans.reference.format.sample_rate_hz * ms + 500U) / 1000U;
}

static uint64_t speech_lower(uint64_t edge, uint64_t pad)
{
    return edge - (edge < pad ? edge : pad);
}

static uint64_t speech_upper(uint64_t edge, uint64_t pad, uint64_t frames)
{
    return edge + (frames - edge < pad ? frames - edge : pad);
}

static double *speech_read_mono(SpeechReport *report, size_t side, HwaPairedSampleSpan first,
                                const HwaPairedSampleSpan *second, char *error, size_t error_size)
{
    uint64_t first_count = first.end_sample - first.start_sample;
    uint64_t second_count = second != NULL ? second->end_sample - second->start_sample : 0U;
    uint64_t capacity = report->work.available_bytes / sizeof(double);
    uint64_t bytes;
    double *samples;

    if (capacity > SIZE_MAX / sizeof(double)) capacity = SIZE_MAX / sizeof(double);
    if (first_count > capacity || second_count > capacity - first_count) {
        hwa_set_error(error, error_size, "speech samples exceed the work limit");
        return NULL;
    }
    bytes = (first_count + second_count) * sizeof(double);
    samples = malloc((size_t)bytes);
    if (samples == NULL) {
        hwa_set_error(error, error_size, "cannot allocate speech samples");
        return NULL;
    }
    report->work.available_bytes -= bytes;
    if (hwa_speech_audio_mono(speech_reader(report, side), first, &report->work,
                              samples, error, error_size) != 0 ||
        (second != NULL &&
         hwa_speech_audio_mono(speech_reader(report, side), *second, &report->work,
                               samples + first_count, error, error_size) != 0)) {
        report->work.available_bytes += bytes;
        free(samples);
        return NULL;
    }
    return samples;
}

static double speech_peak_change(const HWASpeechPeak *reference, const HWASpeechPeak *candidate)
{
    return candidate->present && reference->present
        ? candidate->relative_level_db - reference->relative_level_db : NAN;
}

static const HWASpeechPeak *speech_find_fixed(const HWASpeechBuzz *reference, double frequency,
                                               double tolerance)
{
    const HWASpeechPeak *best = NULL;
    double distance = tolerance;
    size_t index;

    for (index = 0U; index < HWA_SPEECH_FIXED_PEAKS; ++index) {
        const HWASpeechPeak *peak = &reference->fixed_peaks[index];
        double error_hz = fabs(peak->frequency_hz - frequency);
        if (peak->present && error_hz <= distance) {
            distance = error_hz;
            best = peak;
        }
    }
    return best;
}

static int speech_probe_buzz(SpeechReport *report, const SpeechRow *row,
                            const HWASpeechPeak *const *targets, size_t target_count,
                            HWASpeechPeak peaks[SPEECH_SIDE_COUNT][HWA_SPEECH_BUZZ_PROBE_LIMIT],
                            char *error, size_t error_size)
{
    size_t side;

    for (side = 0U; side < SPEECH_SIDE_COUNT; ++side) {
        HwaPairedSampleSpan span = *speech_phone_bounds(row->phone, side);
        const HWAWavReader *reader = speech_reader(report, side);
        double *samples = speech_read_mono(report, side, span, NULL, error, error_size);
        size_t count = (size_t)(span.end_sample - span.start_sample);
        int status;
        if (samples == NULL) return -1;
        status = hwa_speech_buzz_probe(samples, count, reader->format.sample_rate_hz,
            report->shared_nyquist, targets, target_count, &report->work, peaks[side],
            error, error_size);
        free(samples);
        report->work.available_bytes += (uint64_t)count * sizeof(double);
        if (status != 0) return -1;
    }
    return 0;
}

typedef struct SpeechBuzzChoice {
    const HWASpeechPeak *reference;
    const HWASpeechPeak *candidate;
    const HWASpeechPeak *reference_probe;
    const HWASpeechPeak *candidate_probe;
    double change_db;
} SpeechBuzzChoice;

static void speech_consider_peak(SpeechBuzzChoice *choice, const HWASpeechPeak *reference,
                                  const HWASpeechPeak *candidate,
                                  const HWASpeechPeak **targets, size_t *target_count)
{
    double change;

    if (!candidate->present) return;
    if (reference == NULL || !reference->present) {
        targets[(*target_count)++] = candidate;
        return;
    }
    change = speech_peak_change(reference, candidate);
    if (change > choice->change_db) {
        choice->change_db = change;
        choice->reference = reference;
        choice->candidate = candidate;
    }
}

static void speech_export_peak(const SpeechBuzzChoice *choice, HWASpeechPeak *reference,
                               HWASpeechPeak *candidate, HWASpeechPeak *reference_probe,
                               HWASpeechPeak *candidate_probe)
{
    if (choice->candidate != NULL) {
        *reference = choice->reference != NULL ? *choice->reference : (HWASpeechPeak){0};
        *candidate = *choice->candidate;
    }
    if (choice->reference_probe != NULL) {
        *reference_probe = *choice->reference_probe;
        *candidate_probe = *choice->candidate_probe;
    }
}

static int speech_compare_buzz(SpeechReport *report, SpeechRow *row, const HWASpeechBuzz *reference,
                                const HWASpeechBuzz *candidate, char *error, size_t error_size)
{
    double tolerance = fmax(reference->frequency_resolution_hz, candidate->frequency_resolution_hz);
    SpeechBuzzChoice fixed = {.change_db = -INFINITY};
    SpeechBuzzChoice harmonic = {.change_db = -INFINITY};
    SpeechBuzzSide *ref = &row->measure.buzz[SPEECH_REFERENCE];
    SpeechBuzzSide *cand = &row->measure.buzz[SPEECH_CANDIDATE];
    const HWASpeechPeak *targets[HWA_SPEECH_BUZZ_PROBE_LIMIT];
    HWASpeechPeak probes[SPEECH_SIDE_COUNT][HWA_SPEECH_BUZZ_PROBE_LIMIT];
    int unavailable = 0;
    size_t target_count = 0U;
    size_t fixed_count;
    size_t index;

    for (index = 0U; index < HWA_SPEECH_FIXED_PEAKS; ++index) {
        const HWASpeechPeak *peak = &candidate->fixed_peaks[index];
        if (peak->present) {
            speech_consider_peak(&fixed, speech_find_fixed(reference, peak->frequency_hz, tolerance),
                                 peak, targets, &target_count);
        }
    }
    fixed_count = target_count;
    for (index = 0U; index < HWA_SPEECH_HARMONICS; ++index) {
        speech_consider_peak(&harmonic, &reference->harmonics[index], &candidate->harmonics[index],
                             targets, &target_count);
    }
    if (target_count != 0U &&
        speech_probe_buzz(report, row, targets, target_count, probes, error, error_size) != 0)
        return -1;
    for (index = 0U; index < target_count; ++index) {
        SpeechBuzzChoice *choice = index < fixed_count ? &fixed : &harmonic;
        const HWASpeechPeak *reference_probe = &probes[SPEECH_REFERENCE][index];
        const HWASpeechPeak *candidate_probe = &probes[SPEECH_CANDIDATE][index];
        double change = speech_peak_change(reference_probe, candidate_probe);

        if (!isfinite(change)) {
            unavailable = 1;
        } else if (change > choice->change_db) {
            *choice = (SpeechBuzzChoice){NULL, targets[index], reference_probe, candidate_probe, change};
        }
    }
    speech_export_peak(&fixed, &ref->fixed, &cand->fixed, &ref->fixed_probe, &cand->fixed_probe);
    speech_export_peak(&harmonic, &ref->harmonic, &cand->harmonic,
                       &ref->harmonic_probe, &cand->harmonic_probe);
    row->status = "no-finding";
    if (fmax(fixed.change_db, harmonic.change_db) >= SPEECH_MIN_CHANGE_DB) {
        row->status = "finding";
        row->score = fmax(fixed.change_db, harmonic.change_db);
        row->classification = fixed.change_db >= harmonic.change_db ? "fixed" : "harmonic";
    } else if (unavailable) {
        row->status = "indeterminate";
        row->reason = "no-shared-peak-measure";
    }
    return 0;
}

static int speech_measure_buzz(SpeechReport *report, SpeechRow *row, char *error, size_t error_size)
{
    HWASpeechBuzz result[SPEECH_SIDE_COUNT] = {0};
    size_t side;

    for (side = 0U; side < SPEECH_SIDE_COUNT; ++side) {
        HwaPairedSampleSpan span = *speech_phone_bounds(row->phone, side);
        size_t count = (size_t)(span.end_sample - span.start_sample);
        const HWAWavReader *reader = speech_reader(report, side);
        HWASpeechBuzz *buzz = &result[side];
        double *samples = speech_read_mono(report, side, span, NULL, error, error_size);
        int status;
        if (samples == NULL) return -1;
        status = hwa_speech_buzz(samples, count, reader->format.sample_rate_hz,
                                report->shared_nyquist, &report->work, buzz, error, error_size);
        free(samples);
        report->work.available_bytes += (uint64_t)count * sizeof(double);
        if (status != 0) return -1;
        row->measure.buzz[side] = (SpeechBuzzSide){
            .status = buzz->status, .reason = buzz->reason, .pitch_min_hz = buzz->pitch_min_hz,
            .pitch_max_hz = buzz->pitch_max_hz, .pitch_coverage = buzz->pitch_coverage,
            .fixed = buzz->fixed, .harmonic = buzz->harmonic
        };
        if (strcmp(buzz->status, "indeterminate") == 0) {
            row->status = "indeterminate";
            if (row->reason == NULL) row->reason = buzz->reason;
        }
    }
    if (row->status == NULL) {
        return speech_compare_buzz(report, row, &result[SPEECH_REFERENCE], &result[SPEECH_CANDIDATE],
                                   error, error_size);
    }
    return 0;
}

typedef struct SpeechCue {
    double strength;
    double roughness;
    uint64_t sample;
} SpeechCue;

static SpeechCue speech_edge_cue(const double *samples, HwaPairedSampleSpan span, unsigned rate,
                                  double vowel_power)
{
    SpeechCue cue = {0};
    size_t count = (size_t)(span.end_sample - span.start_sample);
    size_t window = rate / 250U;
    size_t hop = rate / 1000U;
    size_t start;

    if (hop == 0U || count < window || vowel_power <= 0.0) return cue;
    for (start = 0U; start <= count - window; start += hop) {
        double power = 0.0, difference = 0.0;
        size_t index;
        for (index = start + 1U; index < start + window; ++index) {
            double sample = samples[index];
            double delta = sample - samples[index - 1U];
            power += sample * sample;
            difference += delta * delta;
        }
        double strength = difference / ((double)(window - 1U) * vowel_power) *
                          ((double)rate * (double)rate);
        if (strength > cue.strength && power > 0.0) {
            cue.strength = strength;
            cue.roughness = difference / power * ((double)rate * (double)rate);
            cue.sample = span.start_sample + start + window / 2U;
        }
    }
    return cue;
}

static int speech_edge_locked(const SpeechReport *report, uint64_t edge)
{
    const HWAAlignment *alignment = &report->spans.audio_alignment;
    double time = (double)edge / report->spans.reference.format.sample_rate_hz;
    size_t first = 0U, last = alignment->anchor_count;

    while (first < last) {
        size_t middle = first + (last - first) / 2U;
        const HWAAlignmentAnchor *anchor = &alignment->anchors[middle];
        if (anchor->reference_seconds == time) return anchor->locked;
        if (anchor->reference_seconds < time) first = middle + 1U;
        else last = middle;
    }
    return 0;
}

static double speech_vowel_power(const double *samples, size_t count)
{
    double power = 0.0;
    size_t index;

    for (index = 0U; index < count; ++index) power += samples[index] * samples[index];
    return power / (double)count;
}

static int speech_read_edge_cue(SpeechReport *report, size_t side, HwaPairedSampleSpan bounds,
                                 double vowel_power, SpeechCue *cue, char *error, size_t error_size)
{
    double *samples = speech_read_mono(report, side, bounds, NULL, error, error_size);
    if (samples == NULL) return -1;
    *cue = speech_edge_cue(samples, bounds, speech_reader(report, side)->format.sample_rate_hz,
                           vowel_power);
    free(samples);
    report->work.available_bytes += (bounds.end_sample - bounds.start_sample) * sizeof(double);
    return 0;
}

static int speech_similar_cue(SpeechCue observed, SpeechCue expected)
{
    return observed.strength >= 0.25 * expected.strength &&
           observed.roughness >= 0.5 * expected.roughness &&
           observed.roughness <= 2.0 * expected.roughness;
}

static int speech_contains_cue(HwaPairedSampleSpan bounds, SpeechCue cue)
{
    return cue.sample >= bounds.start_sample && cue.sample < bounds.end_sample;
}

/* Corroborating a neighboring transient can disqualify the saved edge. Leave
 * the supplied mapping intact since the other phone boundaries remain unknown. */
static int speech_verify_edge(SpeechReport *report, SpeechRow *row, const SpeechRow *vowel,
                               const SpeechCue inside[SPEECH_SIDE_COUNT],
                               const double vowel_power[SPEECH_SIDE_COUNT], char *error, size_t error_size)
{
    const HwaPairedSampleSpan *phone = &row->phone->target;
    const HwaPairedSampleSpan *neighbor = &vowel->phone->target;
    HwaPairedSampleSpan measured = row->transition[SPEECH_CANDIDATE];
    unsigned rate = report->spans.target.format.sample_rate_hz;
    uint64_t edge = row->transition_edge[SPEECH_CANDIDATE];
    uint64_t pad = ((uint64_t)rate * SPEECH_TIMING_SEARCH_MS + 500U) / 1000U;
    HwaPairedSampleSpan probe = {
        .start_sample = speech_lower(edge, pad),
        .end_sample = speech_upper(edge, pad, report->spans.target.format.frames)
    };
    uint64_t start = row->release ? phone->start_sample : neighbor->start_sample;
    uint64_t end = row->release ? neighbor->end_sample : phone->end_sample;
    SpeechCue cue;
    HwaPairedSpan peer = {.confidence = 1.0};

    if (!neighbor->valid || inside[SPEECH_REFERENCE].strength <= 0.0 ||
        vowel_power[SPEECH_CANDIDATE] <= 0.0) return 0;
    if (probe.start_sample < start) probe.start_sample = start;
    if (probe.end_sample > end) probe.end_sample = end;
    if (probe.start_sample >= probe.end_sample ||
        (probe.start_sample >= measured.start_sample && probe.end_sample <= measured.end_sample)) return 0;
    if (speech_read_edge_cue(report, SPEECH_CANDIDATE, probe, vowel_power[SPEECH_CANDIDATE],
                             &cue, error, error_size) != 0) return -1;
    if (speech_contains_cue(measured, cue) ||
        cue.strength <= 2.0 * inside[SPEECH_CANDIDATE].strength ||
        !speech_similar_cue(cue, inside[SPEECH_REFERENCE])) return 0;

    /* A counterpart at the saved offset supports treating the nearby
     * sound as a separate event. */
    pad = ((uint64_t)rate * 8U + 500U) / 1000U;
    peer.target.start_sample = speech_lower(cue.sample, pad);
    peer.target.end_sample = speech_upper(cue.sample, pad, report->spans.target.format.frames);
    hwa_paired_spans_map_interval(&report->spans, &peer, 1);
    if (peer.skip_reason == NULL) {
        SpeechCue reference;
        if (speech_read_edge_cue(report, SPEECH_REFERENCE, peer.reference, vowel_power[SPEECH_REFERENCE],
                                 &reference, error, error_size) != 0) return -1;
        if (!speech_contains_cue(row->transition[SPEECH_REFERENCE], reference) &&
            speech_similar_cue(reference, cue)) return 0;
    }

    row->status = "indeterminate";
    row->reason = "transition-timing-disagreement";
    row->candidate_nearby_cue = cue.sample;
    return 0;
}

static int speech_measure_consonant(SpeechReport *report, SpeechRow *row,
                                    char *error, size_t error_size)
{
    const SpeechRow *vowel = speech_find_row(report, row->vowel_id);
    const HwaPairedSampleSpan *phone = &row->phone->reference;
    const HwaPairedSampleSpan *neighbor;
    uint64_t edge = row->release ? phone->end_sample : phone->start_sample;
    uint64_t frames = report->spans.reference.format.frames;
    uint64_t pad = speech_padding(report, SPEECH_TRANSITION_MS);
    HwaPairedSpan transition, left, interior;
    uint64_t edges[SPEECH_SIDE_COUNT], margin, start, end;
    SpeechCue cues[SPEECH_SIDE_COUNT] = {0};
    double vowel_power[SPEECH_SIDE_COUNT] = {0};
    int verify;
    size_t side;
    const HWASpeechContrast *ref = &row->measure.contrast[SPEECH_REFERENCE];
    const HWASpeechContrast *cand = &row->measure.contrast[SPEECH_CANDIDATE];

    if (vowel == NULL || vowel->kind != SPEECH_VOWEL) {
        hwa_set_error(error, error_size, "consonant '%s' has unknown vowel_id '%s'",
                      row->phone->id, row->vowel_id);
        return -1;
    }
    if (!phone->valid) return 0;
    neighbor = &vowel->phone->reference;
    if (!neighbor->valid) {
        if (row->status == NULL) {
            row->status = "indeterminate";
            row->reason = "invalid-neighboring-vowel";
        }
        return 0;
    }
    if ((row->release && neighbor->start_sample < edge) ||
        (!row->release && neighbor->end_sample > edge)) {
        hwa_set_error(error, error_size, "vowel '%s' is on the wrong side of consonant '%s'",
                      vowel->phone->id, row->phone->id);
        return -1;
    }
    /* A short unlabeled bridge can contain the transition. It cannot include
     * an intervening annotated phone. Touching labels need no gap check. */
    if ((row->release && neighbor->start_sample > edge && row->next_phone != vowel) ||
        (!row->release && neighbor->end_sample < edge && vowel->next_phone != row)) {
        if (row->status == NULL) {
            row->status = "indeterminate";
            row->reason = "non-neighboring-vowel";
        }
        return 0;
    }
    start = speech_lower(edge, pad);
    end = speech_upper(edge, pad, frames);
    if (start < (row->release ? phone->start_sample : neighbor->start_sample)) {
        start = row->release ? phone->start_sample : neighbor->start_sample;
    }
    if (end > (row->release ? neighbor->end_sample : phone->end_sample)) {
        end = row->release ? neighbor->end_sample : phone->end_sample;
    }
    if ((row->release && neighbor->start_sample >= end) ||
        (!row->release && neighbor->end_sample <= start)) {
        if (row->status == NULL) {
            row->status = "indeterminate";
            row->reason = "vowel-outside-transition";
        }
        return 0;
    }
    transition = speech_map(report, start, end);
    row->transition[SPEECH_REFERENCE] = transition.reference;
    row->transition[SPEECH_CANDIDATE] = transition.target;
    speech_check_support(row, &transition);
    if (row->status != NULL) return 0;
    if (edge == transition.reference.start_sample || edge == transition.reference.end_sample) {
        row->status = "indeterminate";
        row->reason = "transition-outside-recording";
        return 0;
    }
    left = speech_map(report, transition.reference.start_sample, edge);
    speech_check_support(row, &left);
    edges[SPEECH_REFERENCE] = edge;
    edges[SPEECH_CANDIDATE] = left.target.end_sample;
    row->transition_edge[SPEECH_REFERENCE] = edges[SPEECH_REFERENCE];
    row->transition_edge[SPEECH_CANDIDATE] = edges[SPEECH_CANDIDATE];
    row->transition_edge_valid = left.target.present;
    margin = (vowel->phone->reference.end_sample - vowel->phone->reference.start_sample) / 5U;
    interior = speech_map(report, vowel->phone->reference.start_sample + margin,
                          vowel->phone->reference.end_sample - margin);
    row->vowel[SPEECH_REFERENCE] = interior.reference;
    row->vowel[SPEECH_CANDIDATE] = interior.target;
    /* Use annotation confidence together with the anchors supporting the vowel interior. */
    interior.confidence = fmin(interior.confidence, vowel->phone->confidence);
    speech_check_support(row, &interior);
    if (row->status != NULL) return 0;
    if (row->confidence < report->min_confidence) {
        row->status = "low-alignment-confidence";
        return 0;
    }
    verify = !speech_edge_locked(report, edge);
    for (side = 0U; side < SPEECH_SIDE_COUNT; ++side) {
        HwaPairedSampleSpan trans = row->transition[side];
        HwaPairedSampleSpan baseline = *speech_phone_bounds(&interior, side);
        const HWAWavReader *reader = speech_reader(report, side);
        size_t count = (size_t)(trans.end_sample - trans.start_sample);
        size_t vowel_count = (size_t)(baseline.end_sample - baseline.start_sample);
        double *samples;
        int status;
        if (edges[side] <= trans.start_sample || edges[side] >= trans.end_sample) {
            row->status = "indeterminate";
            row->reason = "collapsed-transition";
            return 0;
        }
        samples = speech_read_mono(report, side, trans, &baseline, error, error_size);
        if (samples == NULL) return -1;
        status = hwa_speech_contrast(samples, count, samples + count, vowel_count,
            reader->format.sample_rate_hz, report->shared_nyquist, &report->work,
            &row->measure.contrast[side], error, error_size);
        if (verify && status == 0) {
            vowel_power[side] = speech_vowel_power(samples + count, vowel_count);
            cues[side] = speech_edge_cue(samples, trans, reader->format.sample_rate_hz, vowel_power[side]);
        }
        free(samples);
        report->work.available_bytes += ((uint64_t)count + vowel_count) * sizeof(double);
        if (status != 0) return -1;
    }
    row->score = fmax(ref->level_contrast_db - cand->level_contrast_db,
                      fmax(ref->high_band_contrast_db - cand->high_band_contrast_db,
                           ref->attack_contrast_db - cand->attack_contrast_db));
    if (!isfinite(row->score)) {
        row->status = "indeterminate";
        row->reason = "no-shared-transition-measure";
    } else {
        row->status = row->score >= SPEECH_MIN_CHANGE_DB ? "finding" : "no-finding";
    }
    if (verify && row->score >= SPEECH_MIN_CHANGE_DB) {
        return speech_verify_edge(report, row, vowel, cues, vowel_power,
                                  error, error_size);
    }
    return 0;
}

static int speech_rank_order(const void *left, const void *right)
{
    const SpeechRow *const *first = left;
    const SpeechRow *const *second = right;
    double a = strcmp((*first)->status, "finding") == 0 ? (*first)->score : 0.0;
    double b = strcmp((*second)->status, "finding") == 0 ? (*second)->score : 0.0;

    if ((*first)->kind != (*second)->kind) return (*first)->kind < (*second)->kind ? -1 : 1;
    if (a != b) return a > b ? -1 : 1;
    return (*first)->index < (*second)->index ? -1 : ((*first)->index != (*second)->index);
}

static int speech_make_excerpt(const HWASpeechReportOptions *options, SpeechReport *report,
                                SpeechRow *row, char *error, size_t error_size)
{
    uint64_t start = row->phone->reference.start_sample;
    uint64_t end = row->phone->reference.end_sample;
    uint64_t pad = speech_padding(report, SPEECH_CONTEXT_MS);
    uint64_t frames = report->spans.reference.format.frames;
    HWASpeechAudioLevel level[SPEECH_SIDE_COUNT];
    HwaPairedSpan excerpt;
    double rms;
    size_t side;

    if (row->kind == SPEECH_CONSONANT) {
        HwaPairedSampleSpan transition = row->transition[SPEECH_REFERENCE];
        if (start > transition.start_sample) start = transition.start_sample;
        if (end < transition.end_sample) end = transition.end_sample;
    }
    {
        uint64_t padded_start = speech_lower(start, pad);
        uint64_t padded_end = speech_upper(end, pad, frames);
        excerpt = speech_map(report, padded_start, padded_end);
        if (excerpt.skip_reason != NULL) {
            HwaPairedSpan base = speech_map(report, start, end);
            if (base.skip_reason != NULL) return 0;
            excerpt = base;
            /* Grow each side only as far as the saved alignment supports. */
            while (start > padded_start) {
                uint64_t test = padded_start + (start - padded_start) / 2U;
                HwaPairedSpan mapped = speech_map(report, test, end);
                if (mapped.skip_reason == NULL) {
                    start = test;
                    excerpt = mapped;
                } else {
                    padded_start = test + 1U;
                }
            }
            while (padded_end > end) {
                uint64_t test = padded_end - (padded_end - end) / 2U;
                HwaPairedSpan mapped = speech_map(report, start, test);
                if (mapped.skip_reason == NULL) {
                    end = test;
                    excerpt = mapped;
                } else {
                    padded_end = test - 1U;
                }
            }
        }
    }
    row->excerpt.bounds[SPEECH_REFERENCE] = excerpt.reference;
    row->excerpt.bounds[SPEECH_CANDIDATE] = excerpt.target;

    for (side = 0U; side < SPEECH_SIDE_COUNT; ++side) {
        if (hwa_speech_audio_level(speech_reader(report, side), row->excerpt.bounds[side],
                                   &report->work, &level[side], error, error_size) != 0) return -1;
    }
    rms = SPEECH_EXCERPT_RMS;
    for (side = 0U; side < SPEECH_SIDE_COUNT; ++side) {
        if (level[side].rms <= 0.0) return 0;
        rms = fmin(rms, 0.99 * (level[side].rms / level[side].peak));
    }
    for (side = 0U; side < SPEECH_SIDE_COUNT; ++side) {
        size_t length = strlen(options->excerpt_dir) + 64U;
        char *path;
        HWAFileOutput *output = &row->excerpt.output[side];
        const char *name = side == SPEECH_REFERENCE ? "reference" : "candidate";
        int opened;

        /* Charge each retained path and the scratch copy while opening it. */
        if (length > report->work.available_bytes / 2U) {
            hwa_set_error(error, error_size, "speech excerpt paths exceed the work limit");
            return -1;
        }
        path = malloc(length);
        if (path == NULL) {
            hwa_set_error(error, error_size, "cannot allocate speech excerpt path");
            return -1;
        }
        (void)snprintf(path, length, "%s/span-%zu-%s.wav", options->excerpt_dir, row->index + 1U, name);
        opened = hwa_file_output_open(output, path, report->protected_paths, report->protected_count,
                                      0, error, error_size);
        free(path);
        if (opened != 0) return -1;
        report->work.available_bytes -= length;
        row->excerpt.gain[side] = rms / level[side].rms;
        if (hwa_speech_audio_write(hwa_file_output_stream(output), speech_reader(report, side),
                row->excerpt.bounds[side], row->excerpt.gain[side], &report->work,
                error, error_size) != 0 ||
            hwa_file_output_seal_new(output, "speech excerpt", error, error_size) != 0) return -1;
    }
    return 0;
}

static int speech_string(FILE *stream, const char *key, const char *value)
{
    if (fprintf(stream, "\"%s\":", key) < 0) return -1;
    return value != NULL ? hwa_json_write_string(stream, value) : (fputs("null", stream) == EOF ? -1 : 0);
}

static int speech_number(FILE *stream, const char *key, double value)
{
    if (fprintf(stream, "\"%s\":", key) < 0) return -1;
    return (isfinite(value) ? fprintf(stream, "%.17g", value) : fputs("null", stream)) < 0 ? -1 : 0;
}

static int speech_bounds(FILE *stream, const char *key, HwaPairedSampleSpan span, unsigned rate)
{
    if (fprintf(stream, "\"%s\":", key) < 0) return -1;
    if (!span.present) return fputs("null", stream) == EOF ? -1 : 0;
    return fprintf(stream, "{\"start_sample\":%" PRIu64 ",\"end_sample\":%" PRIu64
                   ",\"start_seconds\":%.17g,\"end_seconds\":%.17g}",
                   span.start_sample, span.end_sample,
                   (double)span.start_sample / rate, (double)span.end_sample / rate) < 0 ? -1 : 0;
}

static int speech_peak_json(FILE *stream, const char *key, HWASpeechPeak peak)
{
    if (fprintf(stream, "\"%s\":", key) < 0) return -1;
    if (!peak.present) return fputs("null", stream) == EOF ? -1 : 0;
    return fputc('{', stream) == EOF ||
        speech_number(stream, "frequency_hz", peak.frequency_hz) != 0 ||
        fprintf(stream, ",\"persistence\":%.17g,\"strength_db\":%.17g,\"relative_level_db\":%.17g,",
                peak.persistence, peak.strength_db, peak.relative_level_db) < 0 ||
        speech_number(stream, "harmonic_number", peak.harmonic_number > 0.0 ? peak.harmonic_number : NAN) != 0 ||
        fputc('}', stream) == EOF ? -1 : 0;
}

static int speech_buzz_json(FILE *stream, const char *key, const SpeechBuzzSide *side)
{
    if (fprintf(stream, "\"%s\":", key) < 0) return -1;
    if (side->status == NULL) return fputs("null", stream) == EOF ? -1 : 0;
    return fputc('{', stream) == EOF || speech_string(stream, "status", side->status) != 0 ||
        fputc(',', stream) == EOF || speech_string(stream, "reason", side->reason) != 0 ||
        fputc(',', stream) == EOF || speech_number(stream, "pitch_min_hz", side->pitch_min_hz) != 0 ||
        fputc(',', stream) == EOF || speech_number(stream, "pitch_max_hz", side->pitch_max_hz) != 0 ||
        fputc(',', stream) == EOF || speech_number(stream, "pitch_coverage", side->pitch_coverage) != 0 ||
        fputc(',', stream) == EOF || speech_peak_json(stream, "fixed", side->fixed) != 0 ||
        fputc(',', stream) == EOF || speech_peak_json(stream, "harmonic", side->harmonic) != 0 ||
        fputc(',', stream) == EOF || speech_peak_json(stream, "fixed_probe", side->fixed_probe) != 0 ||
        fputc(',', stream) == EOF || speech_peak_json(stream, "harmonic_probe", side->harmonic_probe) != 0 ||
        fputc('}', stream) == EOF ? -1 : 0;
}

static int speech_peak_difference(FILE *stream, const char *key,
                                   HWASpeechPeak reference, HWASpeechPeak candidate)
{
    HWASpeechPeak diff = {0};
    if (reference.present && candidate.present) {
        diff.present = 1;
        diff.frequency_hz = candidate.frequency_hz - reference.frequency_hz;
        diff.persistence = candidate.persistence - reference.persistence;
        diff.strength_db = candidate.strength_db - reference.strength_db;
        diff.relative_level_db = candidate.relative_level_db - reference.relative_level_db;
    }
    return speech_peak_json(stream, key, diff);
}

static int speech_contrast_metrics(FILE *stream, const HWASpeechContrast *contrast)
{
    return speech_number(stream, "level_contrast_db", contrast->level_contrast_db) != 0 ||
        fputc(',', stream) == EOF ||
        speech_number(stream, "high_band_contrast_db", contrast->high_band_contrast_db) != 0 ||
        fputc(',', stream) == EOF ||
        speech_number(stream, "attack_contrast_db", contrast->attack_contrast_db) != 0 ? -1 : 0;
}

static int speech_contrast_json(FILE *stream, const char *key, const HWASpeechContrast *contrast)
{
    if (fprintf(stream, "\"%s\":", key) < 0) return -1;
    if (contrast->status == NULL) return fputs("null", stream) == EOF ? -1 : 0;
    return fputc('{', stream) == EOF || speech_string(stream, "status", contrast->status) != 0 ||
        fputc(',', stream) == EOF || speech_string(stream, "reason", contrast->reason) != 0 ||
        fputc(',', stream) == EOF || speech_contrast_metrics(stream, contrast) != 0 ||
        fputc('}', stream) == EOF ? -1 : 0;
}

static int speech_excerpt_json(FILE *stream, const SpeechRow *row, unsigned reference_rate,
                                unsigned candidate_rate)
{
    const SpeechExcerpt *excerpt = &row->excerpt;

    if (excerpt->output[SPEECH_CANDIDATE].path == NULL) {
        return fputs("\"excerpt\":null", stream) == EOF ? -1 : 0;
    }
    return fputs("\"excerpt\":{", stream) == EOF ||
        speech_string(stream, "reference_path", excerpt->output[SPEECH_REFERENCE].path) != 0 ||
        fputc(',', stream) == EOF ||
        speech_string(stream, "candidate_path", excerpt->output[SPEECH_CANDIDATE].path) != 0 ||
        fputc(',', stream) == EOF ||
        speech_bounds(stream, "reference_span", excerpt->bounds[SPEECH_REFERENCE], reference_rate) != 0 ||
        fputc(',', stream) == EOF ||
        speech_bounds(stream, "candidate_span", excerpt->bounds[SPEECH_CANDIDATE], candidate_rate) != 0 ||
        fprintf(stream, ",\"reference_gain_db\":%.17g,\"candidate_gain_db\":%.17g}",
                20.0 * log10(excerpt->gain[SPEECH_REFERENCE]),
                20.0 * log10(excerpt->gain[SPEECH_CANDIDATE])) < 0 ? -1 : 0;
}

static int speech_row_json(FILE *stream, const SpeechRow *row, unsigned reference_rate,
                            unsigned candidate_rate)
{
    if (fputc('{', stream) == EOF || speech_string(stream, "id", row->phone->id) != 0 ||
        fputc(',', stream) == EOF || speech_string(stream, "word", row->word) != 0 ||
        fputc(',', stream) == EOF || speech_string(stream, "phone", row->phone_label) != 0 ||
        fputc(',', stream) == EOF || speech_string(stream, "status", row->status) != 0 ||
        fputc(',', stream) == EOF || speech_string(stream, "reason", row->reason) != 0 ||
        fputc(',', stream) == EOF || speech_number(stream, "alignment_confidence", row->confidence) != 0 ||
        fputs(",\"rank\":", stream) == EOF ||
        (row->rank == 0U ? fputs("null", stream) : fprintf(stream, "%zu", row->rank)) < 0 ||
        fputc(',', stream) == EOF || speech_number(stream, "rank_change_db", row->rank ? row->score : NAN) != 0 ||
        fputc(',', stream) == EOF ||
        speech_bounds(stream, "reference_span", row->phone->reference, reference_rate) != 0 ||
        fputc(',', stream) == EOF ||
        speech_bounds(stream, "candidate_span", row->phone->target, candidate_rate) != 0 ||
        fputc(',', stream) == EOF || speech_excerpt_json(stream, row, reference_rate, candidate_rate) != 0 ||
        fputc(',', stream) == EOF) return -1;

    if (row->kind == SPEECH_VOWEL) {
        const SpeechBuzzSide *ref = &row->measure.buzz[SPEECH_REFERENCE];
        const SpeechBuzzSide *cand = &row->measure.buzz[SPEECH_CANDIDATE];
        int comparable = ref->status != NULL && cand->status != NULL &&
                         strcmp(ref->status, "ok") == 0 && strcmp(cand->status, "ok") == 0;
        if (speech_string(stream, "classification", row->classification) != 0 ||
            fputc(',', stream) == EOF || speech_buzz_json(stream, "reference", ref) != 0 ||
            fputc(',', stream) == EOF || speech_buzz_json(stream, "candidate", cand) != 0 ||
            fputs(",\"difference\":", stream) == EOF) return -1;
        if (!comparable) return fputs("null}", stream) == EOF ? -1 : 0;
        return fputc('{', stream) == EOF ||
            speech_peak_difference(stream, "fixed",
                ref->fixed_probe.present ? ref->fixed_probe : ref->fixed,
                ref->fixed_probe.present ? cand->fixed_probe : cand->fixed) != 0 ||
            fputc(',', stream) == EOF ||
            speech_peak_difference(stream, "harmonic",
                ref->harmonic_probe.present ? ref->harmonic_probe : ref->harmonic,
                ref->harmonic_probe.present ? cand->harmonic_probe : cand->harmonic) != 0 ||
            fputs("}}", stream) == EOF ? -1 : 0;
    }
    const HWASpeechContrast *ref = &row->measure.contrast[SPEECH_REFERENCE];
    const HWASpeechContrast *cand = &row->measure.contrast[SPEECH_CANDIDATE];
    if (speech_string(stream, "vowel_id", row->vowel_id) != 0 ||
        fputc(',', stream) == EOF || speech_string(stream, "edge", row->release ? "end" : "start") != 0 ||
        fputc(',', stream) == EOF ||
        speech_bounds(stream, "reference_transition", row->transition[SPEECH_REFERENCE], reference_rate) != 0 ||
        fputc(',', stream) == EOF ||
        speech_bounds(stream, "candidate_transition", row->transition[SPEECH_CANDIDATE], candidate_rate) != 0 ||
        fputc(',', stream) == EOF ||
        speech_number(stream, "reference_edge_seconds", row->transition_edge_valid ?
                      (double)row->transition_edge[SPEECH_REFERENCE] / reference_rate : NAN) != 0 ||
        fputc(',', stream) == EOF ||
        speech_number(stream, "candidate_edge_seconds", row->transition_edge_valid ?
                      (double)row->transition_edge[SPEECH_CANDIDATE] / candidate_rate : NAN) != 0 ||
        fputc(',', stream) == EOF ||
        speech_number(stream, "candidate_nearby_cue_seconds", row->candidate_nearby_cue != 0U ?
                      (double)row->candidate_nearby_cue / candidate_rate : NAN) != 0 ||
        fputc(',', stream) == EOF ||
        speech_bounds(stream, "reference_vowel", row->vowel[SPEECH_REFERENCE], reference_rate) != 0 ||
        fputc(',', stream) == EOF ||
        speech_bounds(stream, "candidate_vowel", row->vowel[SPEECH_CANDIDATE], candidate_rate) != 0 ||
        fputc(',', stream) == EOF || speech_contrast_json(stream, "reference", ref) != 0 ||
        fputc(',', stream) == EOF || speech_contrast_json(stream, "candidate", cand) != 0 ||
        fputs(",\"difference\":", stream) == EOF) return -1;
    if (ref->status == NULL || cand->status == NULL) return fputs("null}", stream) == EOF ? -1 : 0;
    HWASpeechContrast difference = {
        .level_contrast_db = cand->level_contrast_db - ref->level_contrast_db,
        .high_band_contrast_db = cand->high_band_contrast_db - ref->high_band_contrast_db,
        .attack_contrast_db = cand->attack_contrast_db - ref->attack_contrast_db
    };
    return fputc('{', stream) == EOF || speech_contrast_metrics(stream, &difference) != 0 ||
        fputs("}}", stream) == EOF ? -1 : 0;
}

static int speech_write(FILE *stream, const HWASpeechReportOptions *options, const SpeechReport *report)
{
    double ref_rms = report->levels[SPEECH_REFERENCE].rms;
    double cand_rms = report->levels[SPEECH_CANDIDATE].rms;
    unsigned ref_rate = report->spans.reference.format.sample_rate_hz;
    unsigned cand_rate = report->spans.target.format.sample_rate_hz;
    double ref_db = ref_rms > 0.0 ? 20.0 * log10(ref_rms) : NAN;
    double cand_db = cand_rms > 0.0 ? 20.0 * log10(cand_rms) : NAN;
    const char *const arrays[] = {"buzz", "consonant_loss"};
    size_t kind, index = 0U;

    if (fputs("{\"format\":\"hwa-speech-span-comparison\",\"schema_version\":1,", stream) == EOF ||
        speech_string(stream, "reference_path", options->reference_path) != 0 ||
        fputc(',', stream) == EOF || speech_string(stream, "candidate_path", options->candidate_path) != 0 ||
        fputc(',', stream) == EOF || speech_string(stream, "reference_sha256", report->spans.reference_sha256) != 0 ||
        fputc(',', stream) == EOF || speech_string(stream, "candidate_sha256", report->spans.target_sha256) != 0 ||
        fputc(',', stream) == EOF || speech_string(stream, "spans_sha256", report->spans.notes_sha256) != 0 ||
        fputc(',', stream) == EOF || speech_string(stream, "alignment_sha256", report->spans.alignment_sha256) != 0 ||
        fputc(',', stream) == EOF ||
        speech_string(stream, "alignment_reference_sha256", report->spans.alignment_reference_sha256) != 0 ||
        fputc(',', stream) == EOF ||
        speech_string(stream, "alignment_candidate_sha256", report->spans.alignment_target_sha256) != 0 ||
        fprintf(stream, ",\"reference_sample_rate_hz\":%u,\"candidate_sample_rate_hz\":%u"
                ",\"min_pair_confidence\":%.17g,\"min_change_db\":%.17g,\"buzz_level_floor_db\":%.17g"
                ",\"work\":{\"decoded_frames\":%" PRIu64 ",\"transforms\":%" PRIu64 "},\"levels\":{",
                ref_rate, cand_rate, options->min_confidence, SPEECH_MIN_CHANGE_DB,
                HWA_SPEECH_BUZZ_LEVEL_FLOOR_DB, report->work.decoded_frames, report->work.transforms) < 0 ||
        speech_number(stream, "reference_rms_dbfs", ref_db) != 0 || fputc(',', stream) == EOF ||
        speech_number(stream, "candidate_rms_dbfs", cand_db) != 0 || fputc(',', stream) == EOF ||
        speech_number(stream, "candidate_minus_reference_db", cand_db - ref_db) != 0 ||
        fputc('}', stream) == EOF) return -1;

    for (kind = 0U; kind < sizeof(arrays) / sizeof(*arrays); ++kind) {
        size_t start = index;
        if (fprintf(stream, ",\"%s\":[", arrays[kind]) < 0) return -1;
        while (index < report->spans.count && (size_t)report->ordered[index]->kind == kind) {
            if ((index != start && fputc(',', stream) == EOF) ||
                speech_row_json(stream, report->ordered[index], ref_rate, cand_rate) != 0) return -1;
            ++index;
        }
        if (fputc(']', stream) == EOF) return -1;
    }
    return fputs("}\n", stream) == EOF ? -1 : 0;
}

int hwa_speech_span_report(const HWASpeechReportOptions *options,
                           const HWAAnalysisOptions *limits, char *error, size_t error_size)
{
    SpeechReport report = {0};
    HWAFileOutput output = {0};
    HWANumericLocale locale;
    size_t index, side;
    size_t ranks[2] = {0};
    int status = -1, written, restored;
    uint64_t per_row = sizeof(*report.rows) + sizeof(*report.ordered);

    if (options->excerpt_dir != NULL) {
        struct stat info;
        int result = stat(options->excerpt_dir, &info);
#if defined(_MSC_VER)
        int directory = result == 0 && (info.st_mode & _S_IFMT) == _S_IFDIR;
#else
        int directory = result == 0 && S_ISDIR(info.st_mode);
#endif
        if (!directory) {
            hwa_set_error(error, error_size, "speech excerpt directory must already exist");
            return -1;
        }
    }
    if (hwa_paired_spans_open(options->reference_path, options->candidate_path, options->alignment_path,
            options->alignment_reference_path, options->alignment_candidate_path, options->spans_path,
            NULL, 0, 1, limits, 1.0, options->min_confidence, &report.spans, error, error_size) != 0) {
        return -1;
    }
    report.min_confidence = options->min_confidence;
    report.shared_nyquist = 0.5 * fmin(report.spans.reference.format.sample_rate_hz,
                                       report.spans.target.format.sample_rate_hz);
    if (report.spans.count == 0U || report.spans.retained_work_bytes > limits->max_work_bytes ||
        report.spans.count > (limits->max_work_bytes - report.spans.retained_work_bytes) / per_row) {
        hwa_set_error(error, error_size, "no speech spans or speech report exceeds the work limit");
        goto done;
    }
    report.work = (HWASpeechWork){
        .max_frames = limits->max_input_frames, .max_transforms = limits->max_transforms,
        .available_bytes = limits->max_work_bytes - report.spans.retained_work_bytes -
                           report.spans.count * per_row
    };
    report.rows = calloc(report.spans.count, sizeof(*report.rows));
    report.ordered = calloc(report.spans.count, sizeof(*report.ordered));
    if (report.rows == NULL || report.ordered == NULL) {
        hwa_set_error(error, error_size, "cannot allocate speech report rows");
        goto done;
    }
    if (speech_load_labels(&report, error, error_size) != 0) goto done;
    report.protected_paths[report.protected_count++] = options->reference_path;
    report.protected_paths[report.protected_count++] = options->candidate_path;
    report.protected_paths[report.protected_count++] = options->alignment_path;
    report.protected_paths[report.protected_count++] = options->spans_path;
    if (options->alignment_reference_path != NULL) {
        report.protected_paths[report.protected_count++] = options->alignment_reference_path;
    }
    if (options->alignment_candidate_path != NULL) {
        report.protected_paths[report.protected_count++] = options->alignment_candidate_path;
    }
    for (side = 0U; side < SPEECH_SIDE_COUNT; ++side) {
        const HWAWavReader *reader = speech_reader(&report, side);
        HwaPairedSampleSpan span = {.end_sample = reader->format.frames};
        if (span.end_sample != 0U && hwa_speech_audio_level(reader, span, &report.work,
                &report.levels[side], error, error_size) != 0) goto done;
    }
    for (index = 0U; index < report.spans.count; ++index) {
        SpeechRow *row = &report.rows[index];
        if (row->kind == SPEECH_CONSONANT) {
            if (speech_measure_consonant(&report, row, error, error_size) != 0) goto done;
        } else if (row->status == NULL && speech_measure_buzz(&report, row, error, error_size) != 0) goto done;
    }
    qsort(report.ordered, report.spans.count, sizeof(*report.ordered), speech_rank_order);
    if (hwa_file_output_open(&output, options->output_path, report.protected_paths, report.protected_count,
                             options->replace, error, error_size) != 0) goto done;
    for (index = 0U; index < report.spans.count; ++index) {
        SpeechRow *row = report.ordered[index];
        if (strcmp(row->status, "finding") != 0) continue;
        row->rank = ++ranks[row->kind];
        if (options->excerpt_dir != NULL &&
            speech_make_excerpt(options, &report, row, error, error_size) != 0) goto done;
    }
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_set_error(error, error_size, "cannot set speech report numeric locale");
        goto done;
    }
    written = speech_write(hwa_file_output_stream(&output), options, &report);
    restored = hwa_c_numeric_locale_end(&locale);
    if (written != 0 || restored != 0) {
        hwa_set_error(error, error_size, written != 0 ? "cannot write speech report"
                                                       : "cannot restore numeric locale");
        goto done;
    }
    status = hwa_file_output_finish(&output, "speech report", error, error_size);
done:
    if (status != 0) (void)hwa_file_output_abort(&output);
    if (report.rows != NULL) {
        for (index = 0U; index < report.spans.count; ++index) {
            for (side = 0U; side < SPEECH_SIDE_COUNT; ++side) {
                HWAFileOutput *excerpt = &report.rows[index].excerpt.output[side];
                if (status == 0) hwa_file_output_keep_sealed(excerpt);
                else (void)hwa_file_output_abort(excerpt);
            }
        }
    }
    free(report.ordered);
    free(report.rows);
    hwa_paired_spans_close(&report.spans);
    return status;
}

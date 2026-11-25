#include "paired_spectrum_report.h"
#include "paired_spectrum_spans.h"
#include "file_output.h"
#include "numeric_locale.h"
#include "output.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct PairedMeasurement {
    HWAPairedSpectrumSide reference;
    HWAPairedSpectrumSide target;
    const char *reason;
    char side_reason[48];
    int measured;
} PairedMeasurement;

typedef struct PairedReport {
    const HwaPairedSpans *spans;
    const HWAPairedSpectrumReportOptions *options;
    PairedMeasurement *measurements;
    double *powers;
    HWAPairedSpectrumWork work;
    uint64_t resident_bytes;
    HWAPairedSpectrumDspOptions dsp;
} PairedReport;

typedef struct PairedBandMean {
    size_t power_count, absolute_count, normalized_count;
    double reference, target, absolute_db, normalized_db;
} PairedBandMean;

typedef struct PairedSummary {
    size_t usable, skipped;
    uint64_t reference_windows, target_windows;
    PairedBandMean bands[HWA_PAIRED_SPECTRUM_MAX_BANDS];
} PairedSummary;

typedef struct PairedSortEntry {
    const char *value;
    size_t index;
} PairedSortEntry;

static int paired_entry_order(const void *left, const void *right)
{
    const PairedSortEntry *a = left, *b = right;
    int order = strcmp(a->value, b->value);
    if (order != 0) return order;
    return (a->index > b->index) - (a->index < b->index);
}

static const char *paired_label(const HwaPairedSpan *span, const char *label)
{
    size_t index;
    for (index = 0U; index < span->label_count; ++index) {
        if (strcmp(span->labels[index].name, label) == 0) return span->labels[index].value;
    }
    return NULL;
}

static int paired_check_options(const PairedReport *report, double nyquist,
                                char *error, size_t error_size)
{
    const HWAPairedSpectrumReportOptions *options = report->options;
    size_t index;

    for (index = 0U; index < options->group_count; ++index) {
        const char *label = options->group_labels[index];
        size_t column = 0U;
        while (column < report->spans->label_count &&
               strcmp(label, report->spans->label_names[column]) != 0) ++column;
        if (column == report->spans->label_count) {
            hwa_set_error(error, error_size, "unknown note label: %s", label);
            return -1;
        }
    }
    if (report->dsp.normalization_max_hz > nyquist) {
        hwa_set_error(error, error_size, "normalization band exceeds the shared Nyquist frequency");
        return -1;
    }
    for (index = 0U; index < options->band_count; ++index) {
        if (options->bands[index].high_hz > nyquist) {
            hwa_set_error(error, error_size, "band %zu exceeds the shared Nyquist frequency", index + 1U);
            return -1;
        }
    }
    return 0;
}

static int paired_measure(PairedReport *report, HwaPairedSpans *spans,
                          char *error, size_t error_size)
{
    const HWAPairedSpectrumReportOptions *options = report->options;
    size_t index;

    for (index = 0U; index < spans->count; ++index) {
        const HwaPairedSpan *span = &spans->rows[index];
        PairedMeasurement *measurement = &report->measurements[index];
        double *powers = &report->powers[index * 2U * options->band_count];
        const char *side;
        HWAPairedSpectrumSideStatus status;

        measurement->reason = span->skip_reason;
        if (measurement->reason != NULL) continue;
        if (hwa_paired_spectrum_measure(&spans->reference,
                span->held_reference.start_sample, span->held_reference.end_sample,
                span->expected_hz, options->bands, options->band_count,
                &report->dsp, &report->work, &measurement->reference, powers,
                error, error_size) != 0 ||
            hwa_paired_spectrum_measure(&spans->target,
                span->held_target.start_sample, span->held_target.end_sample,
                span->expected_hz, options->bands, options->band_count,
                &report->dsp, &report->work, &measurement->target,
                powers + options->band_count, error, error_size) != 0) return -1;
        measurement->measured = 1;
        side = "reference";
        status = measurement->reference.status;
        if (status == HWA_PAIRED_SPECTRUM_SIDE_OK) {
            side = "target";
            status = measurement->target.status;
        }
        if (status != HWA_PAIRED_SPECTRUM_SIDE_OK) {
            (void)snprintf(measurement->side_reason, sizeof(measurement->side_reason),
                           "%s-%s", side, hwa_paired_spectrum_side_status_text(status));
            measurement->reason = measurement->side_reason;
        }
    }
    return 0;
}

static int paired_number(FILE *stream, double value)
{
    return (isfinite(value) ? fprintf(stream, "%.17g", value) : fputs("null", stream)) < 0 ? -1 : 0;
}

static int paired_number_field(FILE *stream, const char *key, double value)
{
    return fprintf(stream, ",\"%s\":", key) < 0 || paired_number(stream, value) != 0 ? -1 : 0;
}

static int paired_sample_fields(FILE *stream, const HwaPairedSampleSpan *bounds, int held)
{
    const char *prefix = held ? "held_" : "";
    if (bounds->present) {
        return fprintf(stream, ",\"%sstart_sample\":%" PRIu64 ",\"%send_sample\":%" PRIu64,
                       prefix, bounds->start_sample, prefix, bounds->end_sample) < 0 ? -1 : 0;
    }
    return fprintf(stream, ",\"%sstart_sample\":null,\"%send_sample\":null", prefix, prefix) < 0 ? -1 : 0;
}

static int paired_side_json(FILE *stream, const HwaPairedSampleSpan *bounds,
                            const HwaPairedSampleSpan *held,
                            const HWAPairedSpectrumSide *side, int measured,
                            int check_pitch)
{
    if (fprintf(stream, "{\"status\":\"%s\"", measured ?
                hwa_paired_spectrum_side_status_text(side->status) : "not-measured") < 0 ||
        paired_sample_fields(stream, bounds, 0) != 0 ||
        paired_sample_fields(stream, held, 1) != 0 ||
        paired_number_field(stream, "pitch_hz",
                            side->measured_pitch_hz > 0.0 ? side->measured_pitch_hz : NAN) != 0 ||
        paired_number_field(stream, "pitch_confidence",
                            check_pitch && side->window_count > 0U ? side->pitch_confidence : NAN) != 0 ||
        paired_number_field(stream, "total_power",
                            side->usable_windows > 0U ? side->total_power : NAN) != 0 ||
        fprintf(stream, ",\"window_count\":%" PRIu64 ",\"usable_windows\":%" PRIu64
                ",\"pitch_windows\":%" PRIu64 ",\"fft_size\":%zu,\"hop_size\":%zu}",
                side->window_count, side->usable_windows, side->pitch_windows,
                side->fft_size, side->hop_size) < 0) return -1;
    return 0;
}

static double paired_difference_db(double reference, double target)
{
    return reference > 0.0 && target > 0.0 ? 10.0 * (log10(target) - log10(reference)) : NAN;
}

static int paired_band_begin(FILE *stream, const HWAPairedSpectrumBand *band, size_t index)
{
    return fprintf(stream, "%s{\"low_hz\":%.17g,\"high_hz\":%.17g",
                   index == 0U ? "" : ",", band->low_hz, band->high_hz) < 0 ? -1 : 0;
}

static void paired_summary_add(PairedSummary *summary, const PairedReport *report, size_t index)
{
    size_t band;
    size_t count = report->options->band_count;
    const PairedMeasurement *measurement = &report->measurements[index];
    const double *powers = &report->powers[index * 2U * count];
    double total_db;

    if (measurement->reason != NULL) {
        ++summary->skipped;
        return;
    }
    ++summary->usable;
    summary->reference_windows += measurement->reference.usable_windows;
    summary->target_windows += measurement->target.usable_windows;
    total_db = paired_difference_db(measurement->reference.total_power, measurement->target.total_power);
    for (band = 0U; band < count; ++band) {
        PairedBandMean *mean = &summary->bands[band];
        double delta = paired_difference_db(powers[band], powers[band + count]);
        ++mean->power_count;
        mean->reference += (powers[band] - mean->reference) / (double)mean->power_count;
        mean->target += (powers[band + count] - mean->target) / (double)mean->power_count;
        if (isfinite(delta)) {
            ++mean->absolute_count;
            mean->absolute_db += (delta - mean->absolute_db) / (double)mean->absolute_count;
        }
        if (isfinite(delta) && isfinite(total_db)) {
            ++mean->normalized_count;
            mean->normalized_db += (delta - total_db - mean->normalized_db) /
                                   (double)mean->normalized_count;
        }
    }
}

static int paired_note_json(FILE *stream, const PairedReport *report, size_t index)
{
    const HwaPairedSpan *span = &report->spans->rows[index];
    const PairedMeasurement *measurement = &report->measurements[index];
    size_t count = report->options->band_count;
    const double *powers = &report->powers[index * 2U * count];
    double total_db = paired_difference_db(measurement->reference.total_power,
                                           measurement->target.total_power);
    size_t label, band;

    if (fprintf(stream, "%s\n{\"id\":", index == 0U ? "" : ",") < 0 ||
        hwa_json_write_string(stream, span->id) != 0 ||
        fprintf(stream, ",\"status\":\"%s\",\"skip_reason\":",
                measurement->reason != NULL ? "skipped" : "usable") < 0 ||
        (measurement->reason != NULL ? hwa_json_write_string(stream, measurement->reason)
                                       : (fputs("null", stream) == EOF ? -1 : 0)) != 0 ||
        paired_number_field(stream, "expected_hz", span->expected_hz > 0.0 ? span->expected_hz : NAN) != 0 ||
        paired_number_field(stream, "confidence", span->confidence_valid ? span->confidence : NAN) != 0 ||
        fputs(",\"labels\":{", stream) == EOF) return -1;
    for (label = 0U; label < span->label_count; ++label) {
        if ((label != 0U && fputc(',', stream) == EOF) ||
            hwa_json_write_string(stream, span->labels[label].name) != 0 ||
            fputc(':', stream) == EOF ||
            hwa_json_write_string(stream, span->labels[label].value) != 0) return -1;
    }
    if (fputs("},\"reference\":", stream) == EOF ||
        paired_side_json(stream, &span->reference, &span->held_reference,
                         &measurement->reference, measurement->measured, span->expected_hz > 0.0) != 0 ||
        fputs(",\"target\":", stream) == EOF ||
        paired_side_json(stream, &span->target, &span->held_target,
                         &measurement->target, measurement->measured, span->expected_hz > 0.0) != 0 ||
        fputs(",\"bands\":[", stream) == EOF) return -1;
    for (band = 0U; band < count; ++band) {
        double reference = measurement->reference.usable_windows > 0U ? powers[band] : NAN;
        double target = measurement->target.usable_windows > 0U ? powers[band + count] : NAN;
        double absolute = measurement->reason == NULL ? paired_difference_db(reference, target) : NAN;
        if (paired_band_begin(stream, &report->options->bands[band], band) != 0 ||
            paired_number_field(stream, "reference_power", reference) != 0 ||
            paired_number_field(stream, "target_power", target) != 0 ||
            paired_number_field(stream, "absolute_difference_db", absolute) != 0 ||
            paired_number_field(stream, "normalized_difference_db", absolute - total_db) != 0 ||
            fputc('}', stream) == EOF) return -1;
    }
    return fputs("]}", stream) == EOF ? -1 : 0;
}

static int paired_summary_json(FILE *stream, const PairedSummary *summary,
                               const HWAPairedSpectrumReportOptions *options)
{
    size_t band;
    if (fprintf(stream, "\"usable_pairs\":%zu,\"skipped_pairs\":%zu,"
                "\"reference_window_count\":%" PRIu64 ",\"target_window_count\":%" PRIu64
                ",\"bands\":[", summary->usable, summary->skipped,
                summary->reference_windows, summary->target_windows) < 0) return -1;
    for (band = 0U; band < options->band_count; ++band) {
        const PairedBandMean *mean = &summary->bands[band];
        if (paired_band_begin(stream, &options->bands[band], band) != 0 ||
            fprintf(stream, ",\"power_pairs\":%zu,\"absolute_difference_pairs\":%zu,"
                    "\"normalized_difference_pairs\":%zu",
                    mean->power_count, mean->absolute_count, mean->normalized_count) < 0 ||
            paired_number_field(stream, "mean_reference_power", mean->power_count ? mean->reference : NAN) != 0 ||
            paired_number_field(stream, "mean_target_power", mean->power_count ? mean->target : NAN) != 0 ||
            paired_number_field(stream, "mean_absolute_difference_db",
                                mean->absolute_count ? mean->absolute_db : NAN) != 0 ||
            paired_number_field(stream, "mean_normalized_difference_db",
                                mean->normalized_count ? mean->normalized_db : NAN) != 0 ||
            fputc('}', stream) == EOF) return -1;
    }
    return fputc(']', stream) == EOF ? -1 : 0;
}

static int paired_skip_reasons_json(FILE *stream, PairedSortEntry *entries, size_t count)
{
    size_t start, end;
    qsort(entries, count, sizeof(*entries), paired_entry_order);
    if (fputs(",\"skip_reasons\":{", stream) == EOF) return -1;
    for (start = 0U; start < count; start = end) {
        for (end = start + 1U; end < count && strcmp(entries[start].value, entries[end].value) == 0;
             ++end) {}
        if ((start != 0U && fputc(',', stream) == EOF) ||
            hwa_json_write_string(stream, entries[start].value) != 0 ||
            fprintf(stream, ":%zu", end - start) < 0) return -1;
    }
    return fputc('}', stream) == EOF ? -1 : 0;
}

static int paired_groups_json(FILE *stream, const PairedReport *report, PairedSortEntry *entries)
{
    size_t group;
    int comma = 0;
    if (fputs(",\"groups\":[", stream) == EOF) return -1;
    for (group = 0U; group < report->options->group_count; ++group) {
        const char *label = report->options->group_labels[group];
        size_t index, count = 0U, end;
        for (index = 0U; index < report->spans->count; ++index) {
            const char *value = paired_label(&report->spans->rows[index], label);
            if (value != NULL && value[0] != '\0') {
                entries[count++] = (PairedSortEntry){value, index};
            }
        }
        qsort(entries, count, sizeof(*entries), paired_entry_order);
        for (index = 0U; index < count; index = end) {
            PairedSummary summary = {0};
            const char *value = entries[index].value;
            for (end = index; end < count && strcmp(value, entries[end].value) == 0; ++end) {
                paired_summary_add(&summary, report, entries[end].index);
            }
            if ((comma && fputc(',', stream) == EOF) ||
                fputs("{\"label\":", stream) == EOF ||
                hwa_json_write_string(stream, label) != 0 ||
                fputs(",\"value\":", stream) == EOF ||
                hwa_json_write_string(stream, value) != 0 ||
                fputc(',', stream) == EOF ||
                paired_summary_json(stream, &summary, report->options) != 0 ||
                fputc('}', stream) == EOF) return -1;
            comma = 1;
        }
    }
    return fputc(']', stream) == EOF ? -1 : 0;
}

static int paired_write(FILE *stream, const PairedReport *report, PairedSortEntry *scratch, int score)
{
    const HwaPairedSpans *spans = report->spans;
    const HWAPairedSpectrumReportOptions *options = report->options;
    PairedSummary summary = {0};
    size_t index, skipped = 0U;
    if (fprintf(stream,
                "{\"schema\":\"hwa-paired-note-spectra\",\"schema_version\":1,"
                "\"method_version\":\"paired-note-hann-v1\",\"power_unit\":\"full-scale-squared\","
                "\"alignment_sha256\":\"%s\",\"notes_sha256\":\"%s\",\"notes_kind\":\"%s\","
                "\"notes_clock\":\"%s\","
                "\"alignment_reference_sha256\":\"%s\",\"alignment_target_sha256\":\"%s\","
                "\"reference\":{\"sha256\":\"%s\",\"rate_hz\":%" PRIu32 ",\"frames\":%" PRIu64 "},"
                "\"target\":{\"sha256\":\"%s\",\"rate_hz\":%" PRIu32 ",\"frames\":%" PRIu64 "},"
                "\"settings\":{\"held_fraction\":%.17g,\"min_pair_confidence\":%.17g,"
                "\"pitch_tolerance_cents\":%.17g,\"min_pitch_confidence\":%.17g,"
                "\"normalization_min_hz\":%.17g,\"normalization_max_hz\":%.17g,"
                "\"fft_size\":%zu,\"hop_size\":%zu},"
                "\"work\":{\"decoded_frames\":%" PRIu64 ",\"transforms\":%" PRIu64
                ",\"peak_bytes\":%" PRIu64 "},\"notes\":[",
                spans->alignment_sha256, spans->notes_sha256, score ? "score-alignment" : "csv",
                spans->notes_on_target ? "target" : "reference",
                spans->alignment_reference_sha256, spans->alignment_target_sha256,
                spans->reference_sha256, spans->reference.format.sample_rate_hz, spans->reference.format.frames,
                spans->target_sha256, spans->target.format.sample_rate_hz, spans->target.format.frames,
                options->held_fraction, options->min_pair_confidence,
                report->dsp.pitch_tolerance_cents, report->dsp.min_pitch_confidence,
                report->dsp.normalization_min_hz, report->dsp.normalization_max_hz,
                report->dsp.fft_size, report->dsp.hop_size,
                report->work.decoded_frames, report->work.transforms,
                report->resident_bytes + report->work.peak_bytes) < 0) return -1;
    for (index = 0U; index < spans->count; ++index) {
        const char *reason = report->measurements[index].reason;
        if (paired_note_json(stream, report, index) != 0) return -1;
        paired_summary_add(&summary, report, index);
        if (reason != NULL) scratch[skipped++] = (PairedSortEntry){reason, index};
    }
    if (fputs("],\"summary\":{", stream) == EOF ||
        paired_summary_json(stream, &summary, options) != 0 ||
        paired_skip_reasons_json(stream, scratch, skipped) != 0 ||
        fputc('}', stream) == EOF || paired_groups_json(stream, report, scratch) != 0 ||
        fputs("}\n", stream) == EOF) return -1;
    return 0;
}

int hwa_paired_spectrum_report(const char *reference_path, const char *target_path,
                               const char *alignment_path,
                               const char *alignment_reference_path, const char *alignment_target_path,
                               const char *notes_path, const char *score_alignment_path,
                               int notes_on_target, const char *output_path, int replace,
                               const HWAAnalysisOptions *limits,
                               const HWAPairedSpectrumReportOptions *options,
                               char *error, size_t error_size)
{
    HwaPairedSpans spans = {0};
    PairedReport report = {0};
    PairedSortEntry *scratch = NULL;
    HWAFileOutput output;
    HWANumericLocale locale;
    const char *paths[6] = {reference_path, target_path, alignment_path,
                            notes_path != NULL ? notes_path : score_alignment_path};
    size_t path_count = 4U;
    size_t count, per_note;
    double nyquist;
    int status = -1, written, restored;

    if (hwa_paired_spectrum_dsp_options_validate(&options->dsp, error, error_size) != 0 ||
        hwa_paired_spans_open(reference_path, target_path, alignment_path,
            alignment_reference_path, alignment_target_path, notes_path, score_alignment_path,
            notes_on_target, limits, options->held_fraction, options->min_pair_confidence,
            &spans, error, error_size) != 0) return -1;
    report.spans = &spans;
    report.options = options;
    report.dsp = options->dsp;
    nyquist = 0.5 * fmin(spans.reference.format.sample_rate_hz, spans.target.format.sample_rate_hz);
    report.dsp.normalization_min_hz = options->normalization_band.low_hz;
    report.dsp.normalization_max_hz = options->normalization_band.high_hz > 0.0
                                        ? options->normalization_band.high_hz : nyquist;
    if (paired_check_options(&report, nyquist, error, error_size) != 0) goto done;

    count = spans.count != 0U ? spans.count : 1U;
    per_note = sizeof(*report.measurements) + sizeof(*scratch) +
               2U * options->band_count * sizeof(*report.powers);
    if (count > SIZE_MAX / per_note || spans.retained_work_bytes >= limits->max_work_bytes ||
        (uint64_t)(count * per_note) >= limits->max_work_bytes - spans.retained_work_bytes) {
        hwa_set_error(error, error_size, "paired spectrum report exceeds work byte limit");
        goto done;
    }
    report.resident_bytes = spans.retained_work_bytes + (uint64_t)(count * per_note);
    report.dsp.max_work_bytes = limits->max_work_bytes - report.resident_bytes;
    report.measurements = calloc(count, sizeof(*report.measurements));
    report.powers = calloc(count * 2U * options->band_count, sizeof(*report.powers));
    scratch = malloc(count * sizeof(*scratch));
    if (report.measurements == NULL || report.powers == NULL || scratch == NULL) {
        hwa_set_error(error, error_size, "cannot allocate paired spectrum report");
        goto done;
    }
    if (alignment_reference_path != NULL) paths[path_count++] = alignment_reference_path;
    if (alignment_target_path != NULL) paths[path_count++] = alignment_target_path;
    if (paired_measure(&report, &spans, error, error_size) != 0 ||
        hwa_file_output_open(&output, output_path, paths, path_count,
                             replace, error, error_size) != 0) goto done;
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        (void)hwa_file_output_abort(&output);
        hwa_set_error(error, error_size, "cannot set numeric output locale");
        goto done;
    }
    written = paired_write(hwa_file_output_stream(&output), &report, scratch, score_alignment_path != NULL);
    restored = hwa_c_numeric_locale_end(&locale);
    if (written != 0 || restored != 0) {
        (void)hwa_file_output_abort(&output);
        hwa_set_error(error, error_size, written != 0 ? "cannot write paired spectrum report" :
                      "cannot restore numeric output locale");
        goto done;
    }
    status = hwa_file_output_finish(&output, "paired spectrum report", error, error_size);
done:
    free(scratch);
    free(report.powers);
    free(report.measurements);
    hwa_paired_spans_close(&spans);
    return status;
}

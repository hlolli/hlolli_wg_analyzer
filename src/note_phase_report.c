#include "note_phase_report.h"
#include "measure_compare.h"
#include "numeric_locale.h"
#include "output.h"
#include <inttypes.h>

static int write_metric(FILE *stream, const HWAMeasureObservation *observation, int comma)
{
    if (fprintf(stream, "%s\"%s\":{\"unit\":\"%s\",\"status\":\"%s\",\"value\":",
                comma ? "," : "", hwa_measure_kind_name(observation->kind),
                hwa_measure_unit_name(observation->unit),
                hwa_measure_status_name(observation->status)) < 0) return -1;
    if (observation->status == HWA_MEASURE_STATUS_VALID) {
        if (fprintf(stream, "%.17g", observation->value) < 0) return -1;
    } else if (fputs("null", stream) == EOF) return -1;
    return fprintf(stream, ",\"confidence\":%.17g,\"quality_flags\":%" PRIu32 "}",
                   observation->confidence, observation->quality_flags) < 0 ? -1 : 0;
}

static int write_report(FILE *stream, const HWANotePhaseResult *result,
                        const HWANotePhaseEnvelopeResult *envelope,
                        const HWANotePhaseFramesResult *frames)
{
    const char *names[4] = {"attack", "sustain", "release", "clean-tail"};
    const char *statuses[6] = {
        "invalid", "valid", "no-signal", "too-short", "interrupted", "truncated"
    };
    HWANumericLocale locale;
    size_t index;
    int status = -1;
    if (stream == NULL || result == NULL || hwa_c_numeric_locale_begin(&locale) != 0) return -1;
    if (fputs("{\"schema\":\"hwa-note-phases\",\"schema_version\":1,"
              "\"command\":\"note-phases\",\"method\":\"" HWA_NOTE_PHASE_METHOD_VERSION
              "\",\"path\":", stream) == EOF ||
        hwa_json_write_string(stream, result->path) != 0 ||
        fprintf(stream, ",\"audio_sha256\":\"%s\",\"sample_rate_hz\":%" PRIu32
                ",\"frames\":%" PRIu64 ",\"note_start_sample\":%" PRIu64
                ",\"note_end_sample\":%" PRIu64
                ",\"boundary_frame_size\":%zu,\"boundary_hop_size\":%zu"
                ",\"measurement_fft_size\":%zu,\"measurement_hop_size\":%zu"
                ",\"boundary_search_seconds\":%.17g,\"tail_limit_seconds\":%.17g"
                ",\"min_phase_seconds\":%.17g,\"min_body_seconds\":%.17g"
                ",\"silence_threshold_dbfs\":%.17g"
                ",\"next_onset_sample\":",
                result->audio_sha256, result->format.sample_rate_hz,
                result->format.frames, result->options.note_start_sample,
                result->options.note_end_sample, result->options.analysis.frame_size,
                result->options.analysis.hop_size, result->options.measurement.fft_size,
                result->options.measurement.hop_size,
                result->options.segmentation.boundary_search_seconds,
                result->options.segmentation.tail_limit_seconds,
                result->options.segmentation.min_phase_seconds,
                result->options.segmentation.min_body_seconds,
                result->options.analysis.silence_threshold_dbfs) < 0) goto done;
    if (result->next_onset_valid) {
        if (fprintf(stream, "%" PRIu64, result->next_onset_sample) < 0) goto done;
    } else if (fputs("null", stream) == EOF) goto done;
    if (envelope != NULL && fprintf(stream,
            ",\"envelope_method\":\"" HWA_NOTE_PHASE_ENVELOPE_METHOD_VERSION
            "\",\"attack_envelope_bins\":%" PRIu32,
            envelope->attack_envelope_bins) < 0) goto done;
    if (fputs(",\"phases\":[", stream) == EOF) goto done;
    for (index = 0U; index < HWA_NOTE_PHASE_COUNT; ++index) {
        const HWANotePhase *phase = &result->phases[index];
        size_t metric;
        if (fprintf(stream, "%s{\"phase\":\"%s\",\"start_sample\":%" PRIu64
                    ",\"end_sample\":%" PRIu64 ",\"duration_seconds\":%.17g"
                    ",\"boundary_confidence\":%.17g,\"status\":\"%s\",\"metrics\":{",
                    index != 0U ? "," : "", names[index], phase->start_sample,
                    phase->end_sample,
                    (double)(phase->end_sample - phase->start_sample) / result->format.sample_rate_hz,
                    phase->boundary_confidence, statuses[phase->status]) < 0) goto done;
        for (metric = 0U; metric < HWA_NOTE_PHASE_METRIC_COUNT; ++metric) {
            if (write_metric(stream, &phase->metrics[metric], metric != 0U) != 0) goto done;
        }
        if (envelope != NULL) {
            size_t count = index == 0U ? HWA_NOTE_PHASE_ENVELOPE_METRIC_COUNT : 3U;
            for (metric = 0U; metric < count; ++metric) {
                if (write_metric(stream, &envelope->metrics[index][metric], 1) != 0) goto done;
            }
        }
        if (fputs("}}", stream) == EOF) goto done;
    }
    if (fputs("]", stream) == EOF) goto done;
    if (frames != NULL) {
        if (fprintf(stream, ",\"frame_series\":{\"method\":\""
                HWA_NOTE_PHASE_FRAMES_METHOD_VERSION "\",\"window\":\"symmetric-hann\","
                "\"spectrum_weighting\":\"one-sided-power\",\"flatness_excludes_dc\":true,"
                "\"channel_mix\":\"arithmetic-mean\",\"level_weighting\":\"window-energy-normalized\","
                "\"level_floor_dbfs\":-300,\"spectral_floor_dbfs\":%.17g,"
                "\"units\":{\"level_dbfs\":\"dBFS\",\"centroid_hz\":\"Hz\",\"flatness\":\"ratio\"},"
                "\"rows\":[", result->options.measurement.spectral_floor_dbfs) < 0) goto done;
        for (index = 0U; index < frames->frame_count; ++index) {
            const HWANotePhaseFrame *frame = &frames->frames[index];
            if (fprintf(stream, "%s{\"phase\":\"%s\",\"start_sample\":%" PRIu64
                    ",\"end_sample\":%" PRIu64 ",\"center_sample\":%" PRIu64
                    ",\"crosses_phase_bounds\":%s,\"zero_padded\":%s,"
                    "\"level_dbfs\":%.17g,\"spectral_status\":\"%s\",\"centroid_hz\":",
                    index != 0U ? "," : "", names[frame->phase_index], frame->start_sample,
                    frame->end_sample, frame->center_sample,
                    frame->crosses_phase_bounds ? "true" : "false",
                    frame->zero_padded ? "true" : "false", frame->level_dbfs,
                    hwa_measure_status_name(frame->spectral_status)) < 0) goto done;
            if (frame->spectral_status == HWA_MEASURE_STATUS_VALID) {
                if (fprintf(stream, "%.17g,\"flatness\":%.17g}",
                            frame->centroid_hz, frame->flatness) < 0) goto done;
            } else if (fputs("null,\"flatness\":null}", stream) == EOF) goto done;
        }
        if (fputs("]}", stream) == EOF) goto done;
    }
    if (fputs("}\n", stream) != EOF) status = 0;
done:
    if (hwa_c_numeric_locale_end(&locale) != 0) status = -1;
    return status;
}

int hwa_note_phase_report_json(FILE *stream, const HWANotePhaseResult *result)
{
    return write_report(stream, result, NULL, NULL);
}

int hwa_note_phase_envelope_report_json(FILE *stream, const HWANotePhaseEnvelopeResult *result)
{
    return result != NULL ? write_report(stream, &result->summary, result, NULL) : -1;
}

int hwa_note_phase_frames_report_json(FILE *stream, const HWANotePhaseFramesResult *result,
                                     int include_envelope)
{
    return result != NULL ? write_report(stream, &result->envelope.summary,
        include_envelope ? &result->envelope : NULL, result) : -1;
}

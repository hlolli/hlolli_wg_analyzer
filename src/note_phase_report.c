#include "note_phase_report.h"
#include "measure_compare.h"
#include "numeric_locale.h"
#include "output.h"
#include <inttypes.h>

int hwa_note_phase_report_json(FILE *stream, const HWANotePhaseResult *result)
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
            const HWAMeasureObservation *observation = &phase->metrics[metric];
            if (fprintf(stream, "%s\"%s\":{\"unit\":\"%s\",\"status\":\"%s\",\"value\":",
                        metric != 0U ? "," : "", hwa_measure_kind_name(observation->kind),
                        hwa_measure_unit_name(observation->unit),
                        hwa_measure_status_name(observation->status)) < 0) goto done;
            if (observation->status == HWA_MEASURE_STATUS_VALID) {
                if (fprintf(stream, "%.17g", observation->value) < 0) goto done;
            } else if (fputs("null", stream) == EOF) goto done;
            if (fprintf(stream, ",\"confidence\":%.17g,\"quality_flags\":%" PRIu32 "}",
                        observation->confidence, observation->quality_flags) < 0) goto done;
        }
        if (fputs("}}", stream) == EOF) goto done;
    }
    if (fputs("]}\n", stream) != EOF) status = 0;
done:
    if (hwa_c_numeric_locale_end(&locale) != 0) status = -1;
    return status;
}

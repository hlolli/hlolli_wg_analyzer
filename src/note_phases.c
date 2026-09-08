#include "internal.h"
#include "measure_engine.h"
#include "segmentation.h"
#include "sha256.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const HWAMeasureKind phase_metrics[HWA_NOTE_PHASE_METRIC_COUNT] = {
    HWA_MEASURE_RMS_DBFS, HWA_MEASURE_PEAK_DBFS,
    HWA_MEASURE_LEVEL_SLOPE_DB_PER_SECOND, HWA_MEASURE_CENTROID_HZ,
    HWA_MEASURE_CENTROID_SLOPE_HZ_PER_SECOND, HWA_MEASURE_DURATION_SECONDS
};
static const HWAMeasureUnit phase_units[HWA_NOTE_PHASE_METRIC_COUNT] = {
    HWA_MEASURE_UNIT_DBFS, HWA_MEASURE_UNIT_DBFS,
    HWA_MEASURE_UNIT_DB_PER_SECOND, HWA_MEASURE_UNIT_HZ,
    HWA_MEASURE_UNIT_HZ_PER_SECOND, HWA_MEASURE_UNIT_SECONDS
};

void hwa_note_phase_options_default(HWANotePhaseOptions *options)
{
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    hwa_analysis_options_default(&options->analysis);
    hwa_segmentation_options_default(&options->segmentation);
    hwa_measurement_options_default(&options->measurement);
    options->analysis.collect_tracks = 1;
}

void hwa_note_phase_result_free(HWANotePhaseResult *result)
{
    if (result == NULL) return;
    free(result->path);
    memset(result, 0, sizeof(*result));
}

static void check_tail(const HWAAnalysis *analysis, HWANotePhaseResult *result)
{
    const uint64_t half = (uint64_t)analysis->options.frame_size / 2U;
    const uint64_t hop = (uint64_t)analysis->options.hop_size;
    const uint64_t end = result->phases[3].end_sample;
    uint64_t guard = result->options.note_end_sample;
    double peak = -300.0;
    double last_level = 0.0;
    int have_last = 0;
    size_t index;

    for (index = 0U; index < analysis->track_count; ++index) {
        uint64_t center = (uint64_t)index * hop + half;
        const HWAFrameMetrics *frame = &analysis->tracks[index];
        if (center >= result->phases[0].start_sample &&
            center < result->phases[2].start_sample && frame->rms_dbfs > peak) {
            peak = frame->rms_dbfs;
        }
        if (center > end) break;
        if (center >= guard) {
            last_level = frame->rms_dbfs;
            have_last = 1;
            if (center > guard + hop &&
                frame->rms_dbfs > analysis->options.silence_threshold_dbfs &&
                frame->combined_onset_strength >= 0.35 &&
                (frame->energy_onset_strength >= 0.12 ||
                 (frame->pitch_change_valid && frame->pitch_change_strength >= 0.5))) {
                result->next_onset_valid = 1;
                result->next_onset_sample = center - half;
                break;
            }
        }
    }
    for (index = 2U; index < HWA_NOTE_PHASE_COUNT; ++index) {
        HWANotePhase *phase = &result->phases[index];
        if (result->next_onset_valid &&
            phase->end_sample > result->next_onset_sample) {
            phase->status = HWA_NOTE_PHASE_INTERRUPTED;
        } else if (index == 3U && phase->status == HWA_NOTE_PHASE_VALID &&
                   (!have_last || last_level > fmax(
                       analysis->options.silence_threshold_dbfs, peak - 30.0))) {
            phase->status = HWA_NOTE_PHASE_TRUNCATED;
        }
    }
}

int hwa_analyze_note_phases_wav(
    const char *path, const HWANotePhaseOptions *options,
    HWANotePhaseResult *result, char *error, size_t error_size)
{
    HWANotePhaseOptions copied;
    HWAAnalysis analysis;
    HWAMeasurementSet measures;
    HWAItemSet items;
    HWAItem item[HWA_NOTE_PHASE_COUNT];
    const HWAItemKind kinds[HWA_NOTE_PHASE_COUNT] = {
        HWA_ITEM_ATTACK, HWA_ITEM_BODY, HWA_ITEM_RELEASE, HWA_ITEM_RESIDUAL_TAIL
    };
    const char *names[HWA_NOTE_PHASE_COUNT] = {
        "attack", "sustain", "release", "clean-tail"
    };
    uint64_t bounds[5];
    double confidence[4];
    char digest[HWA_SHA256_HEX_SIZE];
    size_t index;
    size_t metric;
    int status = -1;

    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (result == NULL) return -1;
    if (options != NULL) copied = *options;
    else hwa_note_phase_options_default(&copied);
    memset(result, 0, sizeof(*result));
    memset(&analysis, 0, sizeof(analysis));
    memset(&measures, 0, sizeof(measures));
    memset(&items, 0, sizeof(items));
    memset(item, 0, sizeof(item));
    copied.analysis.collect_tracks = 1;
    if (path == NULL || path[0] == '\0' || strcmp(path, "-") == 0 ||
        copied.note_start_sample >= copied.note_end_sample ||
        copied.analysis.channel_mode != HWA_CHANNEL_KEEP ||
        copied.analysis.collect_spectrogram) {
        hwa_set_error(error, error_size, "invalid note-phases input or span");
        return -1;
    }
    result->options = copied;
    if (hwa_sha256_file(path, copied.analysis.max_input_bytes,
                        result->audio_sha256, error, error_size) != 0 ||
        hwa_analyze_wav_with_options(path, &copied.analysis, &analysis,
                                     error, error_size) != 0) goto cleanup;
    if (hwa_segmentation_note_bounds(
            &analysis, &copied.segmentation, copied.note_start_sample,
            copied.note_end_sample, bounds, confidence, error, error_size) != 0) {
        goto cleanup;
    }
    result->format = analysis.format;
    result->path = analysis.path;
    analysis.path = NULL;
    items.audio_format = analysis.format;
    items.items = item;
    items.item_count = HWA_NOTE_PHASE_COUNT;
    for (index = 0U; index < HWA_NOTE_PHASE_COUNT; ++index) {
        HWANotePhase *phase = &result->phases[index];
        phase->start_sample = bounds[index];
        phase->end_sample = bounds[index + 1U];
        phase->boundary_confidence = confidence[index];
        phase->status = bounds[index] == bounds[index + 1U]
                            ? HWA_NOTE_PHASE_TOO_SHORT : HWA_NOTE_PHASE_VALID;
        if (!analysis.activity.active_span_valid) {
            phase->status = HWA_NOTE_PHASE_NO_SIGNAL;
        }
    }
    check_tail(&analysis, result);
    hwa_analysis_free(&analysis);
    for (index = 0U; index < HWA_NOTE_PHASE_COUNT; ++index) {
        const HWANotePhase *phase = &result->phases[index];
        item[index].id = (uint64_t)index + 1U;
        item[index].kind = kinds[index];
        item[index].key = (char *)names[index];
        item[index].role = (char *)names[index];
        item[index].start_sample = phase->start_sample;
        item[index].end_sample = phase->end_sample;
        item[index].confidence = phase->boundary_confidence;
        item[index].excluded = phase->status != HWA_NOTE_PHASE_VALID;
        for (metric = 0U; metric < HWA_NOTE_PHASE_METRIC_COUNT; ++metric) {
            result->phases[index].metrics[metric].kind = phase_metrics[metric];
            result->phases[index].metrics[metric].unit = phase_units[metric];
            result->phases[index].metrics[metric].status = HWA_MEASURE_STATUS_NO_DATA;
        }
    }
    if (hwa_measure_engine_wav(&items, path, &copied.measurement,
                                (uint64_t)strlen(result->path) + 1U,
                                &measures, error, error_size) != 0) goto cleanup;
    for (index = 0U; index < measures.measurement_count; ++index) {
        const HWAMeasureObservation *observation = &measures.measurements[index];
        if (observation->view != HWA_MEASURE_VIEW_RAW || observation->index != 0U ||
            observation->item_id == 0U || observation->item_id > HWA_NOTE_PHASE_COUNT) {
            continue;
        }
        for (metric = 0U; metric < HWA_NOTE_PHASE_METRIC_COUNT; ++metric) {
            if (observation->kind == phase_metrics[metric]) {
                result->phases[observation->item_id - 1U].metrics[metric] = *observation;
            }
        }
    }
    if (hwa_sha256_file(path, copied.analysis.max_input_bytes, digest,
                        error, error_size) != 0) goto cleanup;
    if (strcmp(digest, result->audio_sha256) != 0) {
        hwa_set_error(error, error_size, "note-phases source changed during analysis");
        goto cleanup;
    }
    status = 0;
cleanup:
    hwa_analysis_free(&analysis);
    hwa_measurement_set_free(&measures);
    if (status != 0) hwa_note_phase_result_free(result);
    return status;
}

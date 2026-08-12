#include "alignment.h"

#include "internal.h"
#include "score_manifest.h"
#include "sha256.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HWA_ALIGN_PI 3.14159265358979323846264338327950288
#define HWA_ALIGN_INFINITY (DBL_MAX / 16.0)
#define HWA_ALIGN_NO_EVENT SIZE_MAX
#define HWA_ALIGN_SHORT_GAP_EXTEND_RATIO 0.10
#define HWA_ALIGN_LONG_GAP_EXTEND_RATIO 1.00
#define HWA_ALIGN_SHORT_GAP_SECONDS 0.40

typedef struct HWAAlignAccum {
    long double chroma[HWA_CHROMA_BIN_COUNT];
    long double energy_sum;
    long double activity_sum;
    long double pitch_x;
    long double pitch_y;
    long double pitch_weight;
    long double score_beat_sum;
    double spectral_onset;
    double energy_onset;
    double phase_onset;
    double combined_onset;
    uint32_t evidence_flags;
    uint32_t score_flags;
    size_t event_index;
    size_t count;
    size_t score_beat_count;
} HWAAlignAccum;

typedef struct HWACorridor {
    size_t rows;
    size_t columns;
    size_t *minimum;
    size_t *maximum;
    size_t *offset;
    unsigned char *force_match;
    uint64_t cell_count;
} HWACorridor;

typedef struct HWAPathPoint {
    size_t reference_index;
    size_t target_index;
    unsigned char step;
    double cost;
    double confidence;
    uint32_t evidence_flags;
} HWAPathPoint;

typedef struct HWADtwResult {
    HWAPathPoint *points;
    size_t count;
    size_t capacity;
    double total_cost;
    uint64_t cells;
} HWADtwResult;

typedef struct HWAAlignCostContext {
    const HWAAlignTrack *reference;
    const HWAAlignTrack *target;
    const HWAAlignmentOptions *options;
    double reference_energy_center;
    double target_energy_center;
} HWAAlignCostContext;

static int hwa_align_size_multiply(size_t first,
                                   size_t second,
                                   size_t *product)
{
    if (product == NULL ||
        (second != 0U && first > SIZE_MAX / second)) {
        return -1;
    }
    *product = first * second;
    return 0;
}

static int hwa_align_add_bytes(size_t count,
                               size_t element_size,
                               uint64_t *total)
{
    size_t bytes;

    if (total == NULL ||
        hwa_align_size_multiply(count, element_size, &bytes) != 0 ||
        (uint64_t)bytes > UINT64_MAX - *total) {
        return -1;
    }
    *total += (uint64_t)bytes;
    return 0;
}

static double hwa_align_clamp(double value, double low, double high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static uint8_t hwa_align_next_gap_run(uint8_t run)
{
    return run < UINT8_MAX ? (uint8_t)(run + 1U) : UINT8_MAX;
}

static double hwa_align_gap_extend_ratio(uint8_t run, double step_seconds)
{
    return (double)run * step_seconds <= HWA_ALIGN_SHORT_GAP_SECONDS
               ? HWA_ALIGN_SHORT_GAP_EXTEND_RATIO
               : HWA_ALIGN_LONG_GAP_EXTEND_RATIO;
}

static char *hwa_align_copy_string(const char *source)
{
    size_t length;
    char *copy;

    if (source == NULL) {
        return NULL;
    }
    length = strlen(source);
    if (length == SIZE_MAX) {
        return NULL;
    }
    copy = (char *)malloc(length + 1U);
    if (copy != NULL) {
        memcpy(copy, source, length + 1U);
    }
    return copy;
}

static void hwa_align_free_match_strings(HWAAlignmentMatch *match)
{
    if (match == NULL) {
        return;
    }
    free(match->event_id);
    free(match->kind);
    free(match->voice);
    free(match->midi_note);
    free(match->velocity);
    free(match->tie);
    free(match->dynamic);
    free(match->mark);
    free(match->score_position);
}

void hwa_alignment_free(HWAAlignment *alignment)
{
    size_t index;

    if (alignment == NULL) {
        return;
    }
    free(alignment->reference_path);
    free(alignment->target_path);
    free(alignment->score_path);
    for (index = 0U; index < alignment->match_count; ++index) {
        hwa_align_free_match_strings(&alignment->matches[index]);
    }
    free(alignment->matches);
    free(alignment->anchors);
    free(alignment->unmatched_spans);
    for (index = 0U; index < alignment->warning_count; ++index) {
        free(alignment->warnings[index].code);
        free(alignment->warnings[index].message);
    }
    free(alignment->warnings);
    memset(alignment, 0, sizeof(*alignment));
}

void hwa_alignment_options_default(HWAAlignmentOptions *options)
{
    if (options == NULL) {
        return;
    }
    memset(options, 0, sizeof(*options));
    hwa_analysis_options_default(&options->analysis);
    options->analysis.collect_tracks = 1;
    options->analysis.collect_spectrogram = 0;
    options->analysis.true_peak_oversample = 1U;
    options->alignment_step_seconds = 0.05;
    options->coarse_step_seconds = 0.20;
    options->dtw_band_seconds = 45.0;
    options->fine_radius_seconds = 1.5;
    options->refine_radius_seconds = 0.15;
    options->match_threshold = 0.45;
    options->chroma_weight = 0.50;
    options->onset_weight = 0.20;
    options->pitch_weight = 0.10;
    options->envelope_weight = 0.10;
    options->activity_weight = 0.10;
    options->skip_cost = 0.45;
    options->repeat_cost = 0.45;
    options->ornament_cost = 0.18;
    options->rest_cost = 0.15;
    options->cadenza_cost = 0.12;
    options->max_dtw_cells = UINT64_C(8000000);
    options->max_alignment_work_bytes = UINT64_C(268435456);
    options->max_alignment_points = 200000U;
    options->max_score_events = 200000U;
    options->max_manual_anchors = 4096U;
}

static int hwa_alignment_options_valid(const HWAAlignmentOptions *options,
                                       char *error,
                                       size_t error_size)
{
    double weight_sum;

    if (options == NULL) {
        hwa_set_error(error, error_size, "alignment options are null");
        return -1;
    }
    if (!isfinite(options->alignment_step_seconds) ||
        options->alignment_step_seconds < 0.005 ||
        options->alignment_step_seconds > 1.0 ||
        !isfinite(options->coarse_step_seconds) ||
        options->coarse_step_seconds < options->alignment_step_seconds ||
        options->coarse_step_seconds > 5.0 ||
        !isfinite(options->dtw_band_seconds) ||
        options->dtw_band_seconds < 0.0 ||
        !isfinite(options->fine_radius_seconds) ||
        options->fine_radius_seconds < options->alignment_step_seconds ||
        (options->fine_radius_seconds > options->dtw_band_seconds &&
         options->fine_radius_seconds - options->dtw_band_seconds >
             options->coarse_step_seconds) ||
        !isfinite(options->refine_radius_seconds) ||
        options->refine_radius_seconds < 0.0 ||
        options->refine_radius_seconds > options->fine_radius_seconds) {
        hwa_set_error(error, error_size,
                      "invalid alignment step or radius option");
        return -1;
    }
    if (!isfinite(options->match_threshold) ||
        options->match_threshold < 0.0 || options->match_threshold > 1.0) {
        hwa_set_error(error, error_size,
                      "match threshold must be in 0..1");
        return -1;
    }
    weight_sum = options->chroma_weight + options->onset_weight +
                 options->pitch_weight + options->envelope_weight +
                 options->activity_weight;
    if (!isfinite(weight_sum) || !(weight_sum > 0.0) ||
        options->chroma_weight < 0.0 || options->onset_weight < 0.0 ||
        options->pitch_weight < 0.0 || options->envelope_weight < 0.0 ||
        options->activity_weight < 0.0 ||
        !isfinite(options->skip_cost) || options->skip_cost < 0.0 ||
        !isfinite(options->repeat_cost) || options->repeat_cost < 0.0 ||
        !isfinite(options->ornament_cost) || options->ornament_cost < 0.0 ||
        !isfinite(options->rest_cost) || options->rest_cost < 0.0 ||
        !isfinite(options->cadenza_cost) || options->cadenza_cost < 0.0) {
        hwa_set_error(error, error_size,
                      "alignment weights and costs must be finite and nonnegative");
        return -1;
    }
    if (options->max_dtw_cells == 0U ||
        options->max_alignment_work_bytes == 0U ||
        options->max_alignment_points == 0U ||
        options->max_score_events == 0U ||
        options->max_manual_anchors == 0U) {
        hwa_set_error(error, error_size,
                      "alignment limits must be nonzero");
        return -1;
    }
    return 0;
}

static double hwa_align_pitch_class(double pitch_hz, double tuning_cents)
{
    double pitch = 69.0 + 12.0 * log2(pitch_hz / 440.0) -
                   tuning_cents / 100.0;
    double pitch_class = fmod(pitch, 12.0);

    return pitch_class < 0.0 ? pitch_class + 12.0 : pitch_class;
}

static void hwa_align_rotate_chroma(double chroma[HWA_CHROMA_BIN_COUNT],
                                    double shift)
{
    double input[HWA_CHROMA_BIN_COUNT];
    double output[HWA_CHROMA_BIN_COUNT] = {0.0};
    double norm = 0.0;
    size_t index;

    memcpy(input, chroma, sizeof(input));
    for (index = 0U; index < HWA_CHROMA_BIN_COUNT; ++index) {
        double position = (double)index + shift;
        double base_value;
        double fraction;
        long base;
        size_t first;
        size_t second;

        while (position < 0.0) {
            position += (double)HWA_CHROMA_BIN_COUNT;
        }
        while (position >= (double)HWA_CHROMA_BIN_COUNT) {
            position -= (double)HWA_CHROMA_BIN_COUNT;
        }
        base_value = floor(position);
        base = (long)base_value;
        fraction = position - base_value;
        first = (size_t)base;
        second = (first + 1U) % HWA_CHROMA_BIN_COUNT;
        output[first] += input[index] * (1.0 - fraction);
        output[second] += input[index] * fraction;
    }
    for (index = 0U; index < HWA_CHROMA_BIN_COUNT; ++index) {
        norm += output[index] * output[index];
    }
    if (norm > 0.0) {
        norm = sqrt(norm);
        for (index = 0U; index < HWA_CHROMA_BIN_COUNT; ++index) {
            chroma[index] = output[index] / norm;
        }
    }
}

static void hwa_align_estimate_tuning(const HWAAnalysis *analysis,
                                      double *offset_cents,
                                      double *confidence)
{
    long double x = 0.0L;
    long double y = 0.0L;
    long double weight_sum = 0.0L;
    size_t index;

    *offset_cents = 0.0;
    *confidence = 0.0;
    for (index = 0U; index < analysis->track_count; ++index) {
        const HWAFrameMetrics *frame = &analysis->tracks[index];
        double midi;
        double cents;
        double angle;
        double weight;

        if (frame->pitch_valid == 0 || frame->pitch_hz <= 0.0 ||
            frame->pitch_confidence < 0.35 ||
            frame->rms_dbfs < analysis->options.silence_threshold_dbfs) {
            continue;
        }
        midi = 69.0 + 12.0 * log2(frame->pitch_hz / 440.0);
        cents = 100.0 * (midi - floor(midi + 0.5));
        angle = 2.0 * HWA_ALIGN_PI * cents / 100.0;
        weight = frame->pitch_confidence;
        x += (long double)weight * (long double)cos(angle);
        y += (long double)weight * (long double)sin(angle);
        weight_sum += (long double)weight;
    }
    if (weight_sum > 0.0L) {
        long double length = sqrtl(x * x + y * y);

        *offset_cents = 100.0 * atan2((double)y, (double)x) /
                        (2.0 * HWA_ALIGN_PI);
        *confidence = hwa_align_clamp((double)(length / weight_sum), 0.0, 1.0);
    }
}

int hwa_align_track_from_analysis(const HWAAnalysis *analysis,
                                  double step_seconds,
                                  uint64_t max_work_bytes,
                                  size_t max_points,
                                  HWAAlignFrame **owned_frames,
                                  HWAAlignTrack *track,
                                  char *error,
                                  size_t error_size)
{
    HWAAlignFrame *frames = NULL;
    HWAAlignAccum *accumulators = NULL;
    size_t point_count;
    size_t bytes;
    uint64_t work_bytes = 0U;
    double tuning_offset;
    double tuning_confidence;
    size_t index;
    int result = -1;

    if (error != NULL && error_size > 0U) {
        error[0] = '\0';
    }
    if (analysis == NULL || owned_frames == NULL || track == NULL ||
        analysis->tracks == NULL || analysis->track_count == 0U ||
        !isfinite(step_seconds) || !(step_seconds > 0.0) ||
        !isfinite(analysis->format.duration_seconds) ||
        !(analysis->format.duration_seconds > 0.0)) {
        hwa_set_error(error, error_size, "invalid audio alignment track");
        return -1;
    }
    *owned_frames = NULL;
    memset(track, 0, sizeof(*track));
    if (analysis->format.duration_seconds / step_seconds >=
        (double)SIZE_MAX - 1.0) {
        hwa_set_error(error, error_size,
                      "alignment track point count overflows this host");
        return -1;
    }
    point_count = (size_t)ceil(analysis->format.duration_seconds /
                               step_seconds);
    if (point_count == 0U) {
        point_count = 1U;
    }
    if (point_count > max_points) {
        hwa_set_error(error, error_size,
                      "alignment track point limit exceeded");
        return -1;
    }
    if (hwa_align_add_bytes(point_count, sizeof(*frames), &work_bytes) != 0 ||
        hwa_align_add_bytes(point_count, sizeof(*accumulators),
                            &work_bytes) != 0 ||
        work_bytes > max_work_bytes ||
        hwa_align_size_multiply(point_count, sizeof(*frames), &bytes) != 0) {
        hwa_set_error(error, error_size,
                      "alignment work byte limit exceeded");
        return -1;
    }
    (void)bytes;
    frames = (HWAAlignFrame *)calloc(point_count, sizeof(*frames));
    accumulators = (HWAAlignAccum *)calloc(point_count,
                                            sizeof(*accumulators));
    if (frames == NULL || accumulators == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for audio alignment track");
        goto cleanup;
    }
    for (index = 0U; index < point_count; ++index) {
        accumulators[index].event_index = HWA_ALIGN_NO_EVENT;
    }
    hwa_align_estimate_tuning(analysis, &tuning_offset, &tuning_confidence);
    if (tuning_confidence < 0.25) {
        tuning_offset = 0.0;
    }
    for (index = 0U; index < analysis->track_count; ++index) {
        const HWAFrameMetrics *source = &analysis->tracks[index];
        size_t bucket;
        HWAAlignAccum *target;
        double activity;
        size_t chroma;

        if (source->time_seconds < 0.0) {
            continue;
        }
        bucket = (size_t)floor(source->time_seconds / step_seconds);
        if (bucket >= point_count) {
            bucket = point_count - 1U;
        }
        target = &accumulators[bucket];
        activity = source->rms_dbfs >= analysis->options.silence_threshold_dbfs
                       ? 1.0
                       : 0.0;
        target->energy_sum += (long double)source->rms_dbfs;
        target->activity_sum += (long double)activity;
        if (source->onset_strength > target->spectral_onset) {
            target->spectral_onset = source->onset_strength;
        }
        if (source->energy_onset_strength > target->energy_onset) {
            target->energy_onset = source->energy_onset_strength;
        }
        if (source->phase_onset_valid != 0 &&
            source->phase_onset_strength > target->phase_onset) {
            target->phase_onset = source->phase_onset_strength;
        }
        if (source->combined_onset_strength > target->combined_onset) {
            target->combined_onset = source->combined_onset_strength;
        }
        if (source->chroma_valid != 0) {
            for (chroma = 0U; chroma < HWA_CHROMA_BIN_COUNT; ++chroma) {
                target->chroma[chroma] +=
                    (long double)source->chroma[chroma];
            }
            target->evidence_flags |= HWA_ALIGNMENT_EVIDENCE_CHROMA;
        }
        if (source->pitch_valid != 0 && source->pitch_hz > 0.0) {
            double pitch_class = hwa_align_pitch_class(
                source->pitch_hz, tuning_offset);
            double angle = 2.0 * HWA_ALIGN_PI * pitch_class / 12.0;
            double weight = source->pitch_confidence;

            target->pitch_x += (long double)weight * (long double)cos(angle);
            target->pitch_y += (long double)weight * (long double)sin(angle);
            target->pitch_weight += (long double)weight;
            target->evidence_flags |= HWA_ALIGNMENT_EVIDENCE_PITCH;
        }
        if (source->onset_strength > 0.0) {
            target->evidence_flags |= HWA_ALIGNMENT_EVIDENCE_SPECTRAL_ONSET;
        }
        if (source->energy_onset_strength > 0.0) {
            target->evidence_flags |= HWA_ALIGNMENT_EVIDENCE_ENERGY_ONSET;
        }
        if (source->phase_onset_valid != 0) {
            target->evidence_flags |= HWA_ALIGNMENT_EVIDENCE_PHASE_ONSET;
        }
        target->evidence_flags |= HWA_ALIGNMENT_EVIDENCE_ENVELOPE;
        target->count++;
    }
    for (index = 0U; index < point_count; ++index) {
        HWAAlignAccum *source = &accumulators[index];
        HWAAlignFrame *target = &frames[index];
        double chroma_norm = 0.0;
        size_t chroma;

        target->time_seconds = ((double)index + 0.5) * step_seconds;
        if (target->time_seconds > analysis->format.duration_seconds) {
            target->time_seconds = analysis->format.duration_seconds;
        }
        target->event_index = HWA_ALIGN_NO_EVENT;
        if (source->count == 0U) {
            target->log_energy = -300.0;
            continue;
        }
        target->log_energy =
            (double)(source->energy_sum / (long double)source->count);
        target->activity = hwa_align_clamp(
            (double)(source->activity_sum / (long double)source->count),
            0.0, 1.0);
        target->spectral_onset = hwa_align_clamp(source->spectral_onset,
                                                 0.0, 1.0);
        target->energy_onset = hwa_align_clamp(source->energy_onset,
                                               0.0, 1.0);
        target->phase_onset = hwa_align_clamp(source->phase_onset,
                                              0.0, 1.0);
        target->combined_onset = hwa_align_clamp(source->combined_onset,
                                                 0.0, 1.0);
        target->evidence_flags = source->evidence_flags;
        for (chroma = 0U; chroma < HWA_CHROMA_BIN_COUNT; ++chroma) {
            target->chroma[chroma] = (double)source->chroma[chroma];
            chroma_norm += target->chroma[chroma] * target->chroma[chroma];
        }
        if (chroma_norm > 0.0) {
            chroma_norm = sqrt(chroma_norm);
            for (chroma = 0U; chroma < HWA_CHROMA_BIN_COUNT; ++chroma) {
                target->chroma[chroma] /= chroma_norm;
            }
            hwa_align_rotate_chroma(target->chroma,
                                    -tuning_offset / 100.0);
        }
        if (source->pitch_weight > 0.0L) {
            double angle = atan2((double)source->pitch_y,
                                 (double)source->pitch_x);

            if (angle < 0.0) {
                angle += 2.0 * HWA_ALIGN_PI;
            }
            target->pitch_class = 12.0 * angle / (2.0 * HWA_ALIGN_PI);
            target->pitch_confidence = hwa_align_clamp(
                (double)(sqrtl(source->pitch_x * source->pitch_x +
                               source->pitch_y * source->pitch_y) /
                         source->pitch_weight),
                0.0, 1.0);
        }
    }
    track->frames = frames;
    track->frame_count = point_count;
    track->step_seconds = step_seconds;
    track->duration_seconds = analysis->format.duration_seconds;
    track->tuning_offset_cents = tuning_offset;
    track->tuning_confidence = tuning_confidence;
    *owned_frames = frames;
    frames = NULL;
    result = 0;

cleanup:
    free(accumulators);
    free(frames);
    return result;
}

void hwa_align_track_release(HWAAlignFrame *frames,
                             HWAAlignTrack *track)
{
    free(frames);
    if (track != NULL) {
        memset(track, 0, sizeof(*track));
    }
}

static int hwa_align_resample_track(const HWAAlignTrack *input,
                                    double step_seconds,
                                    uint64_t max_work_bytes,
                                    HWAAlignFrame **owned_frames,
                                    HWAAlignTrack *output,
                                    char *error,
                                    size_t error_size)
{
    HWAAlignFrame *frames = NULL;
    HWAAlignAccum *accumulators = NULL;
    size_t point_count;
    uint64_t work_bytes = 0U;
    size_t index;
    int result = -1;

    *owned_frames = NULL;
    memset(output, 0, sizeof(*output));
    if (input == NULL || input->frames == NULL || input->frame_count == 0U ||
        !isfinite(input->duration_seconds) ||
        !(input->duration_seconds > 0.0) ||
        !isfinite(step_seconds) || !(step_seconds > 0.0) ||
        input->duration_seconds / step_seconds >= (double)SIZE_MAX - 1.0) {
        hwa_set_error(error, error_size, "invalid alignment track resample");
        return -1;
    }
    point_count = (size_t)ceil(input->duration_seconds / step_seconds);
    if (point_count == 0U) {
        point_count = 1U;
    }
    if (hwa_align_add_bytes(point_count, sizeof(*frames), &work_bytes) != 0 ||
        hwa_align_add_bytes(point_count, sizeof(*accumulators),
                            &work_bytes) != 0 ||
        work_bytes > max_work_bytes) {
        hwa_set_error(error, error_size,
                      "alignment work byte limit exceeded");
        return -1;
    }
    frames = (HWAAlignFrame *)calloc(point_count, sizeof(*frames));
    accumulators = (HWAAlignAccum *)calloc(point_count,
                                            sizeof(*accumulators));
    if (frames == NULL || accumulators == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for coarse alignment track");
        goto cleanup;
    }
    for (index = 0U; index < point_count; ++index) {
        accumulators[index].event_index = HWA_ALIGN_NO_EVENT;
    }
    for (index = 0U; index < input->frame_count; ++index) {
        const HWAAlignFrame *source = &input->frames[index];
        size_t bucket = source->time_seconds > 0.0
                            ? (size_t)floor(source->time_seconds /
                                            step_seconds)
                            : 0U;
        HWAAlignAccum *target;
        size_t chroma;

        if (bucket >= point_count) {
            bucket = point_count - 1U;
        }
        target = &accumulators[bucket];
        for (chroma = 0U; chroma < HWA_CHROMA_BIN_COUNT; ++chroma) {
            target->chroma[chroma] += (long double)source->chroma[chroma];
        }
        target->energy_sum += (long double)source->log_energy;
        target->activity_sum += (long double)source->activity;
        if (source->spectral_onset > target->spectral_onset) {
            target->spectral_onset = source->spectral_onset;
        }
        if (source->energy_onset > target->energy_onset) {
            target->energy_onset = source->energy_onset;
        }
        if (source->phase_onset > target->phase_onset) {
            target->phase_onset = source->phase_onset;
        }
        if (source->combined_onset > target->combined_onset) {
            target->combined_onset = source->combined_onset;
        }
        if ((source->evidence_flags & HWA_ALIGNMENT_EVIDENCE_PITCH) != 0U) {
            double angle = 2.0 * HWA_ALIGN_PI * source->pitch_class / 12.0;
            double weight = source->pitch_confidence;

            target->pitch_x += (long double)weight * (long double)cos(angle);
            target->pitch_y += (long double)weight * (long double)sin(angle);
            target->pitch_weight += (long double)weight;
        }
        if (source->score_beat_valid != 0) {
            target->score_beat_sum += (long double)source->score_beat;
            target->score_beat_count++;
        }
        target->evidence_flags |= source->evidence_flags;
        target->score_flags |= source->score_flags;
        if (target->event_index == HWA_ALIGN_NO_EVENT &&
            source->event_index != HWA_ALIGN_NO_EVENT) {
            target->event_index = source->event_index;
        }
        target->count++;
    }
    for (index = 0U; index < point_count; ++index) {
        HWAAlignFrame *target = &frames[index];
        HWAAlignAccum *source = &accumulators[index];
        double chroma_norm = 0.0;
        size_t chroma;

        target->time_seconds = ((double)index + 0.5) * step_seconds;
        if (target->time_seconds > input->duration_seconds) {
            target->time_seconds = input->duration_seconds;
        }
        target->event_index = source->event_index;
        target->score_flags = source->score_flags;
        target->evidence_flags = source->evidence_flags;
        if (source->count == 0U) {
            target->log_energy = -300.0;
            continue;
        }
        target->log_energy =
            (double)(source->energy_sum / (long double)source->count);
        target->activity = hwa_align_clamp(
            (double)(source->activity_sum / (long double)source->count),
            0.0, 1.0);
        target->spectral_onset = hwa_align_clamp(source->spectral_onset,
                                                 0.0, 1.0);
        target->energy_onset = hwa_align_clamp(source->energy_onset,
                                               0.0, 1.0);
        target->phase_onset = hwa_align_clamp(source->phase_onset,
                                              0.0, 1.0);
        target->combined_onset = hwa_align_clamp(source->combined_onset,
                                                 0.0, 1.0);
        for (chroma = 0U; chroma < HWA_CHROMA_BIN_COUNT; ++chroma) {
            target->chroma[chroma] = (double)source->chroma[chroma];
            chroma_norm += target->chroma[chroma] * target->chroma[chroma];
        }
        if (chroma_norm > 0.0) {
            chroma_norm = sqrt(chroma_norm);
            for (chroma = 0U; chroma < HWA_CHROMA_BIN_COUNT; ++chroma) {
                target->chroma[chroma] /= chroma_norm;
            }
        }
        if (source->pitch_weight > 0.0L) {
            double angle = atan2((double)source->pitch_y,
                                 (double)source->pitch_x);

            if (angle < 0.0) {
                angle += 2.0 * HWA_ALIGN_PI;
            }
            target->pitch_class = 12.0 * angle / (2.0 * HWA_ALIGN_PI);
            target->pitch_confidence = hwa_align_clamp(
                (double)(sqrtl(source->pitch_x * source->pitch_x +
                               source->pitch_y * source->pitch_y) /
                         source->pitch_weight),
                0.0, 1.0);
        }
        if (source->score_beat_count != 0U) {
            target->score_beat =
                (double)(source->score_beat_sum /
                         (long double)source->score_beat_count);
            target->score_beat_valid = 1;
        }
    }
    output->frames = frames;
    output->frame_count = point_count;
    output->events = input->events;
    output->event_count = input->event_count;
    output->step_seconds = step_seconds;
    output->duration_seconds = input->duration_seconds;
    output->tuning_offset_cents = input->tuning_offset_cents;
    output->tuning_confidence = input->tuning_confidence;
    output->is_score = input->is_score;
    *owned_frames = frames;
    frames = NULL;
    result = 0;

cleanup:
    free(accumulators);
    free(frames);
    return result;
}

static double hwa_align_energy_center(const HWAAlignTrack *track)
{
    long double sum = 0.0L;
    size_t count = 0U;
    size_t index;

    for (index = 0U; index < track->frame_count; ++index) {
        if (track->frames[index].activity >= 0.25 &&
            isfinite(track->frames[index].log_energy)) {
            sum += (long double)track->frames[index].log_energy;
            count++;
        }
    }
    return count != 0U ? (double)(sum / (long double)count) : -120.0;
}

static double hwa_align_frame_cost(const HWAAlignCostContext *context,
                                   size_t reference_index,
                                   size_t target_index,
                                   uint32_t *evidence_flags)
{
    const HWAAlignFrame *reference =
        &context->reference->frames[reference_index];
    const HWAAlignFrame *target = &context->target->frames[target_index];
    const HWAAlignmentOptions *options = context->options;
    double weighted_cost = 0.0;
    double weight = 0.0;
    uint32_t evidence = 0U;

    if ((reference->evidence_flags & HWA_ALIGNMENT_EVIDENCE_CHROMA) != 0U &&
        (target->evidence_flags & HWA_ALIGNMENT_EVIDENCE_CHROMA) != 0U &&
        options->chroma_weight > 0.0) {
        double dot = 0.0;
        size_t bin;

        for (bin = 0U; bin < HWA_CHROMA_BIN_COUNT; ++bin) {
            dot += reference->chroma[bin] * target->chroma[bin];
        }
        weighted_cost += options->chroma_weight *
                         (1.0 - hwa_align_clamp(dot, 0.0, 1.0));
        weight += options->chroma_weight;
        evidence |= HWA_ALIGNMENT_EVIDENCE_CHROMA;
    }
    if (((reference->evidence_flags | target->evidence_flags) &
         (HWA_ALIGNMENT_EVIDENCE_SPECTRAL_ONSET |
          HWA_ALIGNMENT_EVIDENCE_ENERGY_ONSET |
          HWA_ALIGNMENT_EVIDENCE_PHASE_ONSET)) != 0U &&
        options->onset_weight > 0.0) {
        weighted_cost += options->onset_weight *
                         fabs(reference->combined_onset -
                              target->combined_onset);
        weight += options->onset_weight;
        evidence |= (reference->evidence_flags & target->evidence_flags) &
                    (HWA_ALIGNMENT_EVIDENCE_SPECTRAL_ONSET |
                     HWA_ALIGNMENT_EVIDENCE_ENERGY_ONSET |
                     HWA_ALIGNMENT_EVIDENCE_PHASE_ONSET);
    }
    if ((reference->evidence_flags & HWA_ALIGNMENT_EVIDENCE_PITCH) != 0U &&
        (target->evidence_flags & HWA_ALIGNMENT_EVIDENCE_PITCH) != 0U &&
        reference->pitch_confidence > 0.0 && target->pitch_confidence > 0.0 &&
        options->pitch_weight > 0.0) {
        double difference = fabs(reference->pitch_class - target->pitch_class);
        double confidence = sqrt(reference->pitch_confidence *
                                 target->pitch_confidence);

        if (difference > 6.0) {
            difference = 12.0 - difference;
        }
        weighted_cost += options->pitch_weight * confidence *
                         hwa_align_clamp(difference / 3.0, 0.0, 1.0);
        weight += options->pitch_weight * confidence;
        evidence |= HWA_ALIGNMENT_EVIDENCE_PITCH;
    }
    if ((reference->evidence_flags & HWA_ALIGNMENT_EVIDENCE_ENVELOPE) != 0U &&
        (target->evidence_flags & HWA_ALIGNMENT_EVIDENCE_ENVELOPE) != 0U &&
        options->envelope_weight > 0.0) {
        double reference_level = reference->log_energy -
                                 context->reference_energy_center;
        double target_level = target->log_energy -
                              context->target_energy_center;

        weighted_cost += options->envelope_weight *
                         hwa_align_clamp(fabs(reference_level - target_level) /
                                             24.0,
                                         0.0, 1.0);
        weight += options->envelope_weight;
        evidence |= HWA_ALIGNMENT_EVIDENCE_ENVELOPE;
    }
    if (options->activity_weight > 0.0) {
        double activity_cost;

        if ((reference->score_flags & HWA_ALIGN_FRAME_REST) != 0U) {
            activity_cost = target->activity;
        } else {
            activity_cost = fabs(reference->activity - target->activity);
        }
        weighted_cost += options->activity_weight *
                         hwa_align_clamp(activity_cost, 0.0, 1.0);
        weight += options->activity_weight;
    }
    if (evidence_flags != NULL) {
        *evidence_flags = evidence;
    }
    return weight > 0.0 ? hwa_align_clamp(weighted_cost / weight, 0.0, 1.0)
                        : 1.0;
}

static double hwa_align_skip_cost(const HWAAlignFrame *frame,
                                  const HWAAlignmentOptions *options)
{
    if ((frame->score_flags & HWA_ALIGN_FRAME_CADENZA) != 0U) {
        return options->cadenza_cost;
    }
    if ((frame->score_flags & HWA_ALIGN_FRAME_ORNAMENT) != 0U) {
        return options->ornament_cost;
    }
    if ((frame->score_flags & HWA_ALIGN_FRAME_REST) != 0U) {
        return options->rest_cost;
    }
    if ((frame->score_flags & HWA_ALIGN_FRAME_REPEAT) != 0U) {
        return options->repeat_cost;
    }
    return options->skip_cost;
}

static double hwa_align_repeat_cost(const HWAAlignFrame *reference,
                                    const HWAAlignmentOptions *options)
{
    if ((reference->score_flags & HWA_ALIGN_FRAME_CADENZA) != 0U) {
        return options->cadenza_cost;
    }
    if ((reference->score_flags & HWA_ALIGN_FRAME_ORNAMENT) != 0U) {
        return options->ornament_cost;
    }
    if ((reference->score_flags & HWA_ALIGN_FRAME_REST) != 0U) {
        return options->rest_cost;
    }
    return options->repeat_cost;
}

static size_t hwa_align_time_index(const HWAAlignTrack *track, double seconds)
{
    double quotient;

    if (seconds <= 0.0 || track->frame_count <= 1U) {
        return 0U;
    }
    if (seconds >= track->duration_seconds) {
        return track->frame_count - 1U;
    }
    quotient = seconds / track->step_seconds;
    if (!isfinite(quotient) ||
        quotient >= (double)(track->frame_count - 1U)) {
        return track->frame_count - 1U;
    }
    return (size_t)floor(quotient);
}

static int hwa_align_validate_locked(
    const HWAAlignTrack *reference,
    const HWAAlignTrack *target,
    const HWAAlignmentAnchor *anchors,
    size_t anchor_count,
    size_t max_anchors,
    char *error,
    size_t error_size)
{
    size_t index;

    if (anchor_count > max_anchors) {
        hwa_set_error(error, error_size,
                      "manual anchor limit exceeded");
        return -1;
    }
    if (anchor_count != 0U && anchors == NULL) {
        hwa_set_error(error, error_size,
                      "locked anchor array is null");
        return -1;
    }
    for (index = 0U; index < anchor_count; ++index) {
        const HWAAlignmentAnchor *anchor = &anchors[index];
        size_t reference_cell;
        size_t target_cell;

        if (!isfinite(anchor->reference_seconds) ||
            !isfinite(anchor->target_seconds) ||
            anchor->reference_seconds < 0.0 ||
            anchor->target_seconds < 0.0 ||
            anchor->reference_seconds > reference->duration_seconds ||
            anchor->target_seconds > target->duration_seconds) {
            hwa_set_error(error, error_size,
                          "locked anchor is outside the input duration");
            return -1;
        }
        if ((anchor->reference_seconds == 0.0) !=
                (anchor->target_seconds == 0.0) ||
            (anchor->reference_seconds == reference->duration_seconds) !=
                (anchor->target_seconds == target->duration_seconds)) {
            hwa_set_error(error, error_size,
                          "locked anchors must pair exact timeline endpoints");
            return -1;
        }
        reference_cell = hwa_align_time_index(
            reference, anchor->reference_seconds);
        target_cell = hwa_align_time_index(target, anchor->target_seconds);
        if ((reference_cell == 0U) != (target_cell == 0U) ||
            (reference_cell + 1U == reference->frame_count) !=
                (target_cell + 1U == target->frame_count)) {
            hwa_set_error(error, error_size,
                          "locked endpoint cells must map to matching endpoints");
            return -1;
        }
        if (index != 0U &&
            (anchor->reference_seconds <=
                 anchors[index - 1U].reference_seconds ||
             anchor->target_seconds <= anchors[index - 1U].target_seconds)) {
            hwa_set_error(error, error_size,
                          "locked anchors must increase on both axes");
            return -1;
        }
        if (index != 0U &&
            (reference_cell ==
                 hwa_align_time_index(reference,
                                      anchors[index - 1U].reference_seconds) ||
             target_cell ==
                 hwa_align_time_index(target,
                                      anchors[index - 1U].target_seconds))) {
            hwa_set_error(error, error_size,
                          "locked anchors collapse on the alignment grid");
            return -1;
        }
    }
    return 0;
}

static double hwa_align_expected_target(
    double reference_seconds,
    double reference_duration,
    double target_duration,
    const HWAAlignmentAnchor *anchors,
    size_t anchor_count)
{
    double previous_reference = 0.0;
    double previous_target = 0.0;
    size_t index;

    for (index = 0U; index <= anchor_count; ++index) {
        double next_reference = index < anchor_count
                                    ? anchors[index].reference_seconds
                                    : reference_duration;
        double next_target = index < anchor_count
                                 ? anchors[index].target_seconds
                                 : target_duration;

        if (reference_seconds <= next_reference || index == anchor_count) {
            double range = next_reference - previous_reference;

            if (!(range > 0.0)) {
                return next_target;
            }
            return previous_target +
                   (reference_seconds - previous_reference) *
                       (next_target - previous_target) / range;
        }
        previous_reference = next_reference;
        previous_target = next_target;
    }
    return target_duration;
}

static void hwa_align_corridor_free(HWACorridor *corridor)
{
    if (corridor == NULL) {
        return;
    }
    free(corridor->minimum);
    free(corridor->maximum);
    free(corridor->offset);
    free(corridor->force_match);
    memset(corridor, 0, sizeof(*corridor));
}

static int hwa_align_corridor_allocate(size_t rows,
                                       size_t columns,
                                       uint64_t max_work_bytes,
                                       HWACorridor *corridor,
                                       char *error,
                                       size_t error_size)
{
    uint64_t bytes = 0U;

    memset(corridor, 0, sizeof(*corridor));
    if (rows == 0U || columns == 0U ||
        hwa_align_add_bytes(rows, sizeof(*corridor->minimum), &bytes) != 0 ||
        hwa_align_add_bytes(rows, sizeof(*corridor->maximum), &bytes) != 0 ||
        hwa_align_add_bytes(rows, sizeof(*corridor->offset), &bytes) != 0 ||
        hwa_align_add_bytes(rows, sizeof(*corridor->force_match), &bytes) != 0 ||
        bytes > max_work_bytes) {
        hwa_set_error(error, error_size,
                      "alignment work byte limit exceeded");
        return -1;
    }
    corridor->minimum = (size_t *)calloc(rows, sizeof(*corridor->minimum));
    corridor->maximum = (size_t *)calloc(rows, sizeof(*corridor->maximum));
    corridor->offset = (size_t *)calloc(rows, sizeof(*corridor->offset));
    corridor->force_match = (unsigned char *)calloc(
        rows, sizeof(*corridor->force_match));
    if (corridor->minimum == NULL || corridor->maximum == NULL ||
        corridor->offset == NULL || corridor->force_match == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for DTW corridor");
        hwa_align_corridor_free(corridor);
        return -1;
    }
    corridor->rows = rows;
    corridor->columns = columns;
    return 0;
}

static int hwa_align_corridor_finish(HWACorridor *corridor,
                                     char *error,
                                     size_t error_size)
{
    uint64_t total = 0U;
    size_t row;

    for (row = 0U; row < corridor->rows; ++row) {
        uint64_t width;

        if (corridor->minimum[row] > corridor->maximum[row] ||
            corridor->maximum[row] >= corridor->columns ||
            total > (uint64_t)SIZE_MAX) {
            hwa_set_error(error, error_size, "invalid DTW corridor");
            return -1;
        }
        corridor->offset[row] = (size_t)total;
        width = (uint64_t)(corridor->maximum[row] -
                           corridor->minimum[row]) + 1U;
        if (width > UINT64_MAX - total) {
            hwa_set_error(error, error_size,
                          "DTW cell count overflows");
            return -1;
        }
        total += width;
    }
    if (total > (uint64_t)SIZE_MAX) {
        hwa_set_error(error, error_size,
                      "DTW cell count does not fit this host");
        return -1;
    }
    corridor->cell_count = total;
    return 0;
}

static int hwa_align_apply_locked(HWACorridor *corridor,
                                  const HWAAlignTrack *reference,
                                  const HWAAlignTrack *target,
                                  const HWAAlignmentAnchor *anchors,
                                  size_t anchor_count,
                                  int strict_subsequence,
                                  char *error,
                                  size_t error_size)
{
    size_t previous_row = 0U;
    size_t previous_column = 0U;
    int have_previous = 0;
    size_t index;

    (void)error;
    (void)error_size;

    for (index = 0U; index < anchor_count; ++index) {
        size_t row = hwa_align_time_index(reference,
                                          anchors[index].reference_seconds);
        size_t column = hwa_align_time_index(target,
                                             anchors[index].target_seconds);

        if (strict_subsequence && have_previous &&
            (row <= previous_row || column <= previous_column)) {
            continue;
        }
        if (column > corridor->maximum[row]) {
            corridor->maximum[row] = column;
        }
        corridor->minimum[row] = column;
        if (row != 0U && column != 0U) {
            if (column - 1U < corridor->minimum[row - 1U]) {
                corridor->minimum[row - 1U] = column - 1U;
            }
            if (column - 1U > corridor->maximum[row - 1U]) {
                corridor->maximum[row - 1U] = column - 1U;
            }
        }
        if (row + 1U < corridor->rows) {
            if (column < corridor->minimum[row + 1U]) {
                corridor->minimum[row + 1U] = column;
            }
            if (column > corridor->maximum[row + 1U]) {
                corridor->maximum[row + 1U] = column;
            }
        }
        corridor->force_match[row] = 1U;
        previous_row = row;
        previous_column = column;
        have_previous = 1;
    }
    return 0;
}

static size_t hwa_align_radius_points(double seconds,
                                      const HWAAlignTrack *track)
{
    double points;

    if (!(seconds > 0.0) || track->frame_count <= 1U) {
        return 0U;
    }
    points = seconds / track->step_seconds;
    if (!isfinite(points) ||
        points >= (double)(track->frame_count - 1U)) {
        return track->frame_count - 1U;
    }
    return (size_t)ceil(points);
}

static int hwa_align_build_coarse_corridor(
    const HWAAlignTrack *reference,
    const HWAAlignTrack *target,
    double radius_seconds,
    const HWAAlignmentAnchor *anchors,
    size_t anchor_count,
    uint64_t max_work_bytes,
    HWACorridor *corridor,
    char *error,
    size_t error_size)
{
    size_t radius = hwa_align_radius_points(radius_seconds, target);
    size_t row;

    if (hwa_align_corridor_allocate(reference->frame_count,
                                    target->frame_count,
                                    max_work_bytes, corridor,
                                    error, error_size) != 0) {
        return -1;
    }
    for (row = 0U; row < reference->frame_count; ++row) {
        double expected_seconds = hwa_align_expected_target(
            reference->frames[row].time_seconds,
            reference->duration_seconds, target->duration_seconds,
            anchors, anchor_count);
        size_t center = hwa_align_time_index(target, expected_seconds);

        corridor->minimum[row] = center > radius ? center - radius : 0U;
        corridor->maximum[row] =
            radius < target->frame_count - 1U - center
                ? center + radius
                : target->frame_count - 1U;
    }
    corridor->minimum[0U] = 0U;
    corridor->maximum[0U] =
        corridor->maximum[0U] < target->frame_count - 1U
            ? corridor->maximum[0U]
            : target->frame_count - 1U;
    corridor->maximum[reference->frame_count - 1U] =
        target->frame_count - 1U;
    if (hwa_align_apply_locked(corridor, reference, target,
                               anchors, anchor_count, 1,
                               error, error_size) != 0 ||
        hwa_align_corridor_finish(corridor, error, error_size) != 0) {
        hwa_align_corridor_free(corridor);
        return -1;
    }
    return 0;
}

static double hwa_align_map_from_path(const HWAAlignTrack *reference,
                                      const HWAAlignTrack *target,
                                      const HWADtwResult *path,
                                      double reference_seconds,
                                      size_t *cursor)
{
    const HWAPathPoint *left;
    const HWAPathPoint *right;
    double left_time;
    double right_time;
    double fraction;

    if (reference_seconds <= 0.0) {
        return 0.0;
    }
    if (reference_seconds >= reference->duration_seconds) {
        return target->duration_seconds;
    }
    while (*cursor + 1U < path->count &&
           reference->frames[path->points[*cursor + 1U].reference_index]
                   .time_seconds < reference_seconds) {
        (*cursor)++;
    }
    left = &path->points[*cursor];
    right = *cursor + 1U < path->count
                ? &path->points[*cursor + 1U]
                : left;
    left_time = reference->frames[left->reference_index].time_seconds;
    right_time = reference->frames[right->reference_index].time_seconds;
    if (!(right_time > left_time)) {
        return target->frames[right->target_index].time_seconds;
    }
    fraction = (reference_seconds - left_time) / (right_time - left_time);
    return target->frames[left->target_index].time_seconds +
           fraction *
               (target->frames[right->target_index].time_seconds -
                target->frames[left->target_index].time_seconds);
}

static int hwa_align_build_fine_corridor(
    const HWAAlignTrack *reference,
    const HWAAlignTrack *target,
    const HWAAlignTrack *coarse_reference,
    const HWAAlignTrack *coarse_target,
    const HWADtwResult *coarse_path,
    double radius_seconds,
    const HWAAlignmentAnchor *anchors,
    size_t anchor_count,
    uint64_t max_work_bytes,
    HWACorridor *corridor,
    char *error,
    size_t error_size)
{
    size_t radius = hwa_align_radius_points(radius_seconds, target);
    size_t cursor = 0U;
    size_t row;

    if (hwa_align_corridor_allocate(reference->frame_count,
                                    target->frame_count,
                                    max_work_bytes, corridor,
                                    error, error_size) != 0) {
        return -1;
    }
    for (row = 0U; row < reference->frame_count; ++row) {
        double mapped_seconds = hwa_align_map_from_path(
            coarse_reference, coarse_target, coarse_path,
            reference->frames[row].time_seconds, &cursor);
        size_t center = hwa_align_time_index(target, mapped_seconds);

        corridor->minimum[row] = center > radius ? center - radius : 0U;
        corridor->maximum[row] =
            radius < target->frame_count - 1U - center
                ? center + radius
                : target->frame_count - 1U;
        if (anchor_count != 0U) {
            double locked_seconds = hwa_align_expected_target(
                reference->frames[row].time_seconds,
                reference->duration_seconds, target->duration_seconds,
                anchors, anchor_count);
            size_t locked_center = hwa_align_time_index(target,
                                                        locked_seconds);
            size_t locked_minimum = locked_center > radius
                                        ? locked_center - radius
                                        : 0U;
            size_t locked_maximum =
                radius < target->frame_count - 1U - locked_center
                    ? locked_center + radius
                    : target->frame_count - 1U;

            if (locked_minimum < corridor->minimum[row]) {
                corridor->minimum[row] = locked_minimum;
            }
            if (locked_maximum > corridor->maximum[row]) {
                corridor->maximum[row] = locked_maximum;
            }
        }
    }
    for (row = 0U; row < coarse_path->count; ++row) {
        const HWAPathPoint *point = &coarse_path->points[row];
        size_t fine_row = hwa_align_time_index(
            reference,
            coarse_reference->frames[point->reference_index].time_seconds);
        size_t fine_column = hwa_align_time_index(
            target,
            coarse_target->frames[point->target_index].time_seconds);

        if (fine_column < corridor->minimum[fine_row]) {
            corridor->minimum[fine_row] = fine_column;
        }
        if (fine_column > corridor->maximum[fine_row]) {
            corridor->maximum[fine_row] = fine_column;
        }
    }
    corridor->minimum[0U] = 0U;
    corridor->maximum[reference->frame_count - 1U] =
        target->frame_count - 1U;
    if (hwa_align_apply_locked(corridor, reference, target,
                               anchors, anchor_count, 0,
                               error, error_size) != 0 ||
        hwa_align_corridor_finish(corridor, error, error_size) != 0) {
        hwa_align_corridor_free(corridor);
        return -1;
    }
    return 0;
}

static void hwa_align_dtw_free(HWADtwResult *result)
{
    if (result == NULL) {
        return;
    }
    free(result->points);
    memset(result, 0, sizeof(*result));
}

static int hwa_align_run_dtw(const HWAAlignTrack *reference,
                             const HWAAlignTrack *target,
                             const HWAAlignmentOptions *options,
                             const HWACorridor *corridor,
                             uint64_t max_work_bytes,
                             HWADtwResult *result,
                             char *error,
                             size_t error_size)
{
    unsigned char *predecessor = NULL;
    uint8_t *previous_gap_run = NULL;
    uint8_t *current_gap_run = NULL;
    double *older = NULL;
    double *previous = NULL;
    double *current = NULL;
    HWAPathPoint *reverse_path = NULL;
    size_t path_capacity;
    uint64_t work_bytes = 0U;
    HWAAlignCostContext context;
    size_t row;
    int status = -1;

    memset(result, 0, sizeof(*result));
    if (reference->frame_count > SIZE_MAX - target->frame_count) {
        hwa_set_error(error, error_size, "DTW path size overflows");
        return -1;
    }
    path_capacity = reference->frame_count + target->frame_count;
    if (hwa_align_add_bytes((size_t)corridor->cell_count,
                            sizeof(*predecessor), &work_bytes) != 0 ||
        hwa_align_add_bytes(target->frame_count,
                            sizeof(*previous_gap_run), &work_bytes) != 0 ||
        hwa_align_add_bytes(target->frame_count,
                            sizeof(*current_gap_run), &work_bytes) != 0 ||
        hwa_align_add_bytes(target->frame_count,
                            sizeof(*older), &work_bytes) != 0 ||
        hwa_align_add_bytes(target->frame_count,
                            sizeof(*previous), &work_bytes) != 0 ||
        hwa_align_add_bytes(target->frame_count,
                            sizeof(*current), &work_bytes) != 0 ||
        hwa_align_add_bytes(path_capacity,
                            sizeof(*reverse_path), &work_bytes) != 0 ||
        work_bytes > max_work_bytes) {
        hwa_set_error(error, error_size,
                      "alignment work byte limit exceeded");
        return -1;
    }
    predecessor = (unsigned char *)calloc(
        (size_t)corridor->cell_count, sizeof(*predecessor));
    previous_gap_run = (uint8_t *)calloc(
        target->frame_count, sizeof(*previous_gap_run));
    current_gap_run = (uint8_t *)calloc(
        target->frame_count, sizeof(*current_gap_run));
    older = (double *)malloc(target->frame_count * sizeof(*older));
    previous = (double *)malloc(target->frame_count * sizeof(*previous));
    current = (double *)malloc(target->frame_count * sizeof(*current));
    reverse_path = (HWAPathPoint *)calloc(path_capacity,
                                          sizeof(*reverse_path));
    if (predecessor == NULL || previous_gap_run == NULL ||
        current_gap_run == NULL || older == NULL || previous == NULL ||
        current == NULL ||
        reverse_path == NULL) {
        hwa_set_error(error, error_size, "out of memory for bounded DTW");
        goto cleanup;
    }
    for (row = 0U; row < target->frame_count; ++row) {
        older[row] = HWA_ALIGN_INFINITY;
        previous[row] = HWA_ALIGN_INFINITY;
        current[row] = HWA_ALIGN_INFINITY;
    }
    context.reference = reference;
    context.target = target;
    context.options = options;
    context.reference_energy_center = hwa_align_energy_center(reference);
    context.target_energy_center = hwa_align_energy_center(target);

    for (row = 0U; row < reference->frame_count; ++row) {
        size_t column;

        for (column = corridor->minimum[row];
             column <= corridor->maximum[row]; ++column) {
            current[column] = HWA_ALIGN_INFINITY;
            current_gap_run[column] = 0U;
        }
        for (column = corridor->minimum[row];
             column <= corridor->maximum[row]; ++column) {
            size_t cell = corridor->offset[row] +
                          column - corridor->minimum[row];
            double best = HWA_ALIGN_INFINITY;
            unsigned char step = 0U;
            uint8_t selected_gap_run = 0U;
            double local_cost = hwa_align_frame_cost(
                &context, row, column, NULL);

            if (row == 0U && column == 0U) {
                best = local_cost;
            } else if (corridor->force_match[row] != 0U &&
                       column == corridor->minimum[row]) {
                if (row != 0U && column != 0U &&
                    column - 1U >= corridor->minimum[row - 1U] &&
                    column - 1U <= corridor->maximum[row - 1U] &&
                    previous[column - 1U] < HWA_ALIGN_INFINITY) {
                    best = previous[column - 1U] + local_cost;
                    step = 1U;
                }
            } else if (corridor->force_match[row] != 0U) {
                if (column != 0U &&
                    current[column - 1U] < HWA_ALIGN_INFINITY) {
                    size_t previous_cell = cell - 1U;
                    double gap_cost = hwa_align_repeat_cost(
                        &reference->frames[row], options);
                    uint8_t gap_run = 1U;

                    if (predecessor[previous_cell] == 3U) {
                        gap_run = hwa_align_next_gap_run(
                            current_gap_run[column - 1U]);
                        gap_cost *= hwa_align_gap_extend_ratio(
                            gap_run, target->step_seconds);
                    }
                    best = current[column - 1U] + gap_cost;
                    step = 3U;
                    selected_gap_run = gap_run;
                }
            } else {
                if (row != 0U && column != 0U &&
                    column - 1U >= corridor->minimum[row - 1U] &&
                    column - 1U <= corridor->maximum[row - 1U] &&
                    previous[column - 1U] < HWA_ALIGN_INFINITY) {
                    best = previous[column - 1U] + local_cost;
                    step = 1U;
                }
                if (row != 0U &&
                    column >= corridor->minimum[row - 1U] &&
                    column <= corridor->maximum[row - 1U] &&
                    previous[column] < HWA_ALIGN_INFINITY) {
                    size_t previous_cell = corridor->offset[row - 1U] +
                                           column -
                                           corridor->minimum[row - 1U];
                    double gap_cost = hwa_align_skip_cost(
                        &reference->frames[row], options);
                    double candidate;
                    uint8_t gap_run = 1U;

                    if (predecessor[previous_cell] == 2U) {
                        gap_run = hwa_align_next_gap_run(
                            previous_gap_run[column]);
                        gap_cost *= hwa_align_gap_extend_ratio(
                            gap_run, reference->step_seconds);
                    }
                    candidate = previous[column] + gap_cost;

                    if (candidate < best) {
                        best = candidate;
                        step = 2U;
                        selected_gap_run = gap_run;
                    }
                }
                if (row != 0U && column >= 2U &&
                    column - 2U >= corridor->minimum[row - 1U] &&
                    column - 2U <= corridor->maximum[row - 1U] &&
                    previous[column - 2U] < HWA_ALIGN_INFINITY &&
                    !(target->frames[column - 1U].combined_onset > 0.75 &&
                      reference->frames[row].combined_onset < 0.25)) {
                    double paired_cost = hwa_align_frame_cost(
                        &context, row, column - 1U, NULL);
                    double candidate = previous[column - 2U] +
                                       0.5 * (local_cost + paired_cost) +
                                       0.05;

                    if (candidate < best) {
                        best = candidate;
                        step = 4U;
                        selected_gap_run = 0U;
                    }
                }
                if (row >= 2U &&
                    corridor->force_match[row - 1U] == 0U &&
                    column != 0U &&
                    column - 1U >= corridor->minimum[row - 2U] &&
                    column - 1U <= corridor->maximum[row - 2U] &&
                    older[column - 1U] < HWA_ALIGN_INFINITY &&
                    !(reference->frames[row - 1U].combined_onset > 0.75 &&
                      target->frames[column].combined_onset < 0.25)) {
                    double paired_cost = hwa_align_frame_cost(
                        &context, row - 1U, column, NULL);
                    double candidate = older[column - 1U] +
                                       0.5 * (local_cost + paired_cost) +
                                       0.05;

                    if (candidate < best) {
                        best = candidate;
                        step = 5U;
                        selected_gap_run = 0U;
                    }
                }
                if (column != 0U &&
                    column - 1U >= corridor->minimum[row] &&
                    current[column - 1U] < HWA_ALIGN_INFINITY) {
                    size_t previous_cell = cell - 1U;
                    double gap_cost = hwa_align_repeat_cost(
                        &reference->frames[row], options);
                    double candidate;
                    uint8_t gap_run = 1U;

                    if (predecessor[previous_cell] == 3U) {
                        gap_run = hwa_align_next_gap_run(
                            current_gap_run[column - 1U]);
                        gap_cost *= hwa_align_gap_extend_ratio(
                            gap_run, target->step_seconds);
                    }
                    candidate = current[column - 1U] + gap_cost;

                    if (candidate < best) {
                        best = candidate;
                        step = 3U;
                        selected_gap_run = gap_run;
                    }
                }
            }
            current[column] = best;
            predecessor[cell] = step;
            current_gap_run[column] = selected_gap_run;
        }
        {
            double *temporary = older;
            older = previous;
            previous = current;
            current = temporary;
            {
                uint8_t *gap_temporary = previous_gap_run;
                previous_gap_run = current_gap_run;
                current_gap_run = gap_temporary;
            }
        }
    }
    if (!(previous[target->frame_count - 1U] < HWA_ALIGN_INFINITY)) {
        hwa_set_error(error, error_size,
                      "DTW corridor has no endpoint path");
        goto cleanup;
    }
    result->total_cost = previous[target->frame_count - 1U];
    result->cells = corridor->cell_count;
    {
        size_t reference_index = reference->frame_count - 1U;
        size_t target_index = target->frame_count - 1U;

        for (;;) {
            HWAPathPoint *point;
            size_t cell;
            unsigned char step;
            uint32_t evidence = 0U;

            if (result->count >= path_capacity ||
                target_index < corridor->minimum[reference_index] ||
                target_index > corridor->maximum[reference_index]) {
                hwa_set_error(error, error_size,
                              "DTW backtrace left its corridor");
                goto cleanup;
            }
            cell = corridor->offset[reference_index] +
                   target_index - corridor->minimum[reference_index];
            step = predecessor[cell];
            point = &reverse_path[result->count++];
            point->reference_index = reference_index;
            point->target_index = target_index;
            point->step = step;
            point->cost = hwa_align_frame_cost(
                &context, reference_index, target_index, &evidence);
            if (step == 4U) {
                uint32_t paired_evidence = 0U;
                double paired_cost = hwa_align_frame_cost(
                    &context, reference_index, target_index - 1U,
                    &paired_evidence);

                point->cost = 0.5 * (point->cost + paired_cost) + 0.05;
                evidence |= paired_evidence;
            } else if (step == 5U) {
                uint32_t paired_evidence = 0U;
                double paired_cost = hwa_align_frame_cost(
                    &context, reference_index - 1U, target_index,
                    &paired_evidence);

                point->cost = 0.5 * (point->cost + paired_cost) + 0.05;
                evidence |= paired_evidence;
            }
            point->evidence_flags = evidence;
            point->confidence = step == 2U || step == 3U
                                    ? 0.0
                                    : exp(-2.5 * point->cost) *
                                          (evidence != 0U ? 1.0 : 0.5);
            if (reference_index == 0U && target_index == 0U) {
                break;
            }
            if (step == 1U) {
                if (reference_index == 0U || target_index == 0U) {
                    hwa_set_error(error, error_size,
                                  "invalid diagonal DTW backtrace");
                    goto cleanup;
                }
                reference_index--;
                target_index--;
            } else if (step == 2U) {
                if (reference_index == 0U) {
                    hwa_set_error(error, error_size,
                                  "invalid reference DTW backtrace");
                    goto cleanup;
                }
                reference_index--;
            } else if (step == 3U) {
                if (target_index == 0U) {
                    hwa_set_error(error, error_size,
                                  "invalid target DTW backtrace");
                    goto cleanup;
                }
                target_index--;
            } else if (step == 4U) {
                if (reference_index == 0U || target_index < 2U) {
                    hwa_set_error(error, error_size,
                                  "invalid fast-target DTW backtrace");
                    goto cleanup;
                }
                reference_index--;
                target_index -= 2U;
            } else if (step == 5U) {
                if (reference_index < 2U || target_index == 0U) {
                    hwa_set_error(error, error_size,
                                  "invalid fast-reference DTW backtrace");
                    goto cleanup;
                }
                reference_index -= 2U;
                target_index--;
            } else {
                hwa_set_error(error, error_size,
                              "incomplete DTW backtrace");
                goto cleanup;
            }
        }
    }
    {
        size_t first = 0U;
        size_t last = result->count - 1U;

        while (first < last) {
            HWAPathPoint temporary = reverse_path[first];
            reverse_path[first] = reverse_path[last];
            reverse_path[last] = temporary;
            first++;
            last--;
        }
    }
    result->points = reverse_path;
    result->capacity = path_capacity;
    reverse_path = NULL;
    status = 0;

cleanup:
    free(current_gap_run);
    free(previous_gap_run);
    free(reverse_path);
    free(current);
    free(previous);
    free(older);
    free(predecessor);
    if (status != 0) {
        hwa_align_dtw_free(result);
    }
    return status;
}

static int hwa_align_path_contains_locked(
    const HWAAlignTrack *reference,
    const HWAAlignTrack *target,
    const HWADtwResult *path,
    const HWAAlignmentAnchor *anchors,
    size_t anchor_count,
    char *error,
    size_t error_size)
{
    size_t cursor = 0U;
    size_t index;

    for (index = 0U; index < anchor_count; ++index) {
        size_t row = hwa_align_time_index(reference,
                                          anchors[index].reference_seconds);
        size_t column = hwa_align_time_index(target,
                                             anchors[index].target_seconds);

        while (cursor < path->count &&
               (path->points[cursor].reference_index < row ||
                (path->points[cursor].reference_index == row &&
                 path->points[cursor].target_index < column))) {
            cursor++;
        }
        if (cursor >= path->count ||
            path->points[cursor].reference_index != row ||
            path->points[cursor].target_index != column) {
            hwa_set_error(error, error_size,
                          "DTW path missed a locked anchor");
            return -1;
        }
    }
    return 0;
}

static int hwa_align_anchor_compare(const void *first, const void *second)
{
    const HWAAlignmentAnchor *left = (const HWAAlignmentAnchor *)first;
    const HWAAlignmentAnchor *right = (const HWAAlignmentAnchor *)second;

    if (left->reference_seconds < right->reference_seconds) {
        return -1;
    }
    if (left->reference_seconds > right->reference_seconds) {
        return 1;
    }
    if (left->origin == HWA_ALIGNMENT_ORIGIN_MANUAL &&
        right->origin != HWA_ALIGNMENT_ORIGIN_MANUAL) {
        return -1;
    }
    if (right->origin == HWA_ALIGNMENT_ORIGIN_MANUAL &&
        left->origin != HWA_ALIGNMENT_ORIGIN_MANUAL) {
        return 1;
    }
    return 0;
}

static int hwa_align_near_locked(double reference_seconds,
                                 double tolerance,
                                 const HWAAlignmentAnchor *locked,
                                 size_t locked_count)
{
    size_t index;

    for (index = 0U; index < locked_count; ++index) {
        if (fabs(reference_seconds - locked[index].reference_seconds) <=
            tolerance) {
            return 1;
        }
    }
    return 0;
}

static int hwa_align_exact_locked(double reference_seconds,
                                  double target_seconds,
                                  const HWAAlignmentAnchor *locked,
                                  size_t locked_count)
{
    size_t index;

    for (index = 0U; index < locked_count; ++index) {
        if (locked[index].reference_seconds == reference_seconds &&
            locked[index].target_seconds == target_seconds) {
            return 1;
        }
    }
    return 0;
}

static int hwa_align_score_beat_at_time(const HWAAlignTrack *track,
                                        double seconds,
                                        double *beat)
{
    size_t right = 0U;
    double left_time;
    double left_beat;
    double right_time;
    double right_beat;

    if (track == NULL || beat == NULL || track->is_score == 0 ||
        track->frames == NULL || track->frame_count == 0U ||
        !isfinite(seconds) || seconds < 0.0 ||
        seconds > track->duration_seconds) {
        return -1;
    }
    if (seconds == 0.0) {
        *beat = 0.0;
        return 0;
    }
    while (right < track->frame_count &&
           track->frames[right].time_seconds < seconds) {
        right++;
    }
    if (right < track->frame_count &&
        track->frames[right].time_seconds == seconds) {
        *beat = track->frames[right].score_beat;
        return track->frames[right].score_beat_valid != 0 ? 0 : -1;
    }
    if (right == 0U) {
        left_time = 0.0;
        left_beat = 0.0;
        right_time = track->frames[0U].time_seconds;
        right_beat = track->frames[0U].score_beat;
        if (track->frames[0U].score_beat_valid == 0) {
            return -1;
        }
    } else {
        const HWAAlignFrame *left = &track->frames[right - 1U];

        if (left->score_beat_valid == 0) {
            return -1;
        }
        left_time = left->time_seconds;
        left_beat = left->score_beat;
        if (right < track->frame_count) {
            const HWAAlignFrame *next = &track->frames[right];

            if (next->score_beat_valid == 0) {
                return -1;
            }
            right_time = next->time_seconds;
            right_beat = next->score_beat;
        } else {
            size_t event;
            int have_end = 0;

            right_time = track->duration_seconds;
            right_beat = left_beat;
            for (event = 0U; event < track->event_count; ++event) {
                if (!have_end || track->events[event].end_beat > right_beat) {
                    right_beat = track->events[event].end_beat;
                    have_end = 1;
                }
            }
            if (!have_end && track->frame_count >= 2U) {
                const HWAAlignFrame *prior =
                    &track->frames[track->frame_count - 2U];
                double time_range = left_time - prior->time_seconds;

                if (prior->score_beat_valid == 0 || !(time_range > 0.0)) {
                    return -1;
                }
                right_beat = left_beat +
                    (right_time - left_time) *
                    (left_beat - prior->score_beat) / time_range;
            } else if (!have_end && left_time > 0.0) {
                right_beat = left_beat * right_time / left_time;
            }
        }
    }
    if (!(right_time > left_time) || !isfinite(right_beat) ||
        right_beat < left_beat) {
        return -1;
    }
    *beat = left_beat + (seconds - left_time) *
                            (right_beat - left_beat) /
                            (right_time - left_time);
    return isfinite(*beat) && *beat >= 0.0 ? 0 : -1;
}

static int hwa_align_make_anchors(
    const HWAAlignTrack *reference,
    const HWAAlignTrack *target,
    const HWAAlignmentOptions *options,
    const HWADtwResult *path,
    const HWAAlignmentAnchor *locked,
    size_t locked_count,
    HWAAlignment *result,
    char *error,
    size_t error_size)
{
    HWAAlignmentAnchor *anchors;
    HWAAlignCostContext context;
    double last_auto = -DBL_MAX;
    size_t auto_count = 0U;
    size_t index;
    size_t output = 0U;

    for (index = 0U; index < path->count; ++index) {
        const HWAPathPoint *point = &path->points[index];
        double time = reference->frames[point->reference_index].time_seconds;
        int endpoint = index == 0U || index + 1U == path->count;
        int landmark = reference->frames[point->reference_index]
                           .combined_onset >= 0.35;
        double output_reference = index == 0U
                                      ? 0.0
                                      : (index + 1U == path->count
                                             ? reference->duration_seconds
                                             : time);
        double output_target = index == 0U
                                   ? 0.0
                                   : (index + 1U == path->count
                                          ? target->duration_seconds
                                          : target->frames[point->target_index]
                                                .time_seconds);
        int suppress = endpoint
                           ? hwa_align_exact_locked(
                                 output_reference, output_target,
                                 locked, locked_count)
                           : hwa_align_near_locked(
                                 time, reference->step_seconds * 0.51,
                                 locked, locked_count);

        if ((endpoint ||
             (point->step == 1U &&
              (landmark || time - last_auto >= 1.0))) &&
            !suppress) {
            auto_count++;
            last_auto = time;
        }
    }
    if (path->count == 1U &&
        !hwa_align_exact_locked(reference->duration_seconds,
                                target->duration_seconds,
                                locked, locked_count)) {
        auto_count++;
    }
    if (auto_count > SIZE_MAX - locked_count ||
        auto_count + locked_count > options->max_alignment_points) {
        hwa_set_error(error, error_size,
                      "alignment point limit exceeded");
        return -1;
    }
    anchors = (HWAAlignmentAnchor *)calloc(
        auto_count + locked_count, sizeof(*anchors));
    if (anchors == NULL && auto_count + locked_count != 0U) {
        hwa_set_error(error, error_size,
                      "out of memory for alignment anchors");
        return -1;
    }
    context.reference = reference;
    context.target = target;
    context.options = options;
    context.reference_energy_center = hwa_align_energy_center(reference);
    context.target_energy_center = hwa_align_energy_center(target);
    last_auto = -DBL_MAX;
    for (index = 0U; index < path->count; ++index) {
        const HWAPathPoint *point = &path->points[index];
        const HWAAlignFrame *frame =
            &reference->frames[point->reference_index];
        double time = frame->time_seconds;
        int endpoint = index == 0U || index + 1U == path->count;
        int landmark = frame->combined_onset >= 0.35;
        double output_reference = index == 0U
                                      ? 0.0
                                      : (index + 1U == path->count
                                             ? reference->duration_seconds
                                             : time);
        double output_target = index == 0U
                                   ? 0.0
                                   : (index + 1U == path->count
                                          ? target->duration_seconds
                                          : target->frames[point->target_index]
                                                .time_seconds);
        int suppress = endpoint
                           ? hwa_align_exact_locked(
                                 output_reference, output_target,
                                 locked, locked_count)
                           : hwa_align_near_locked(
                                 time, reference->step_seconds * 0.51,
                                 locked, locked_count);

        if (!(endpoint ||
              (point->step == 1U &&
               (landmark || time - last_auto >= 1.0))) ||
            suppress) {
            continue;
        }
        anchors[output].reference_seconds = output_reference;
        anchors[output].target_seconds = output_target;
        anchors[output].confidence = point->confidence;
        anchors[output].evidence_flags = point->evidence_flags;
        anchors[output].origin = HWA_ALIGNMENT_ORIGIN_AUTO;
        if (frame->score_beat_valid != 0) {
            anchors[output].score_beat = frame->score_beat;
            anchors[output].score_beat_valid = 1;
        }
        if (landmark && options->refine_radius_seconds > 0.0 && !endpoint) {
            size_t radius = hwa_align_radius_points(
                options->refine_radius_seconds, target);
            size_t low = point->target_index > radius
                             ? point->target_index - radius
                             : 0U;
            size_t high = radius < target->frame_count - 1U -
                                       point->target_index
                              ? point->target_index + radius
                              : target->frame_count - 1U;
            size_t candidate;
            size_t best = point->target_index;
            double best_cost = hwa_align_frame_cost(
                &context, point->reference_index, best, NULL);

            for (candidate = low; candidate <= high; ++candidate) {
                double candidate_cost = hwa_align_frame_cost(
                    &context, point->reference_index, candidate, NULL);

                if (candidate_cost < best_cost) {
                    best_cost = candidate_cost;
                    best = candidate;
                }
            }
            anchors[output].target_seconds =
                target->frames[best].time_seconds;
            anchors[output].confidence = exp(-2.5 * best_cost);
        }
        output++;
        last_auto = time;
    }
    if (path->count == 1U &&
        !hwa_align_exact_locked(reference->duration_seconds,
                                target->duration_seconds,
                                locked, locked_count)) {
        const HWAPathPoint *point = &path->points[0U];

        anchors[output].reference_seconds = reference->duration_seconds;
        anchors[output].target_seconds = target->duration_seconds;
        anchors[output].confidence = point->confidence;
        anchors[output].evidence_flags = point->evidence_flags;
        anchors[output].origin = HWA_ALIGNMENT_ORIGIN_AUTO;
        output++;
    }
    for (index = 0U; index < locked_count; ++index) {
        anchors[output] = locked[index];
        anchors[output].origin = HWA_ALIGNMENT_ORIGIN_MANUAL;
        anchors[output].locked = 1;
        output++;
    }
    for (index = 0U; index < output; ++index) {
        if (reference->is_score != 0) {
            if (hwa_align_score_beat_at_time(
                    reference, anchors[index].reference_seconds,
                    &anchors[index].score_beat) != 0) {
                free(anchors);
                hwa_set_error(error, error_size,
                              "cannot map an alignment anchor to score beat");
                return -1;
            }
            anchors[index].score_beat_valid = 1;
        } else {
            anchors[index].score_beat = 0.0;
            anchors[index].score_beat_valid = 0;
        }
    }
    qsort(anchors, output, sizeof(*anchors), hwa_align_anchor_compare);
    {
        size_t compact = 0U;

        for (index = 0U; index < output; ++index) {
            HWAAlignmentAnchor current = anchors[index];

            if (current.origin == HWA_ALIGNMENT_ORIGIN_MANUAL) {
                while (compact != 0U &&
                       (current.reference_seconds <=
                            anchors[compact - 1U].reference_seconds ||
                        current.target_seconds <=
                            anchors[compact - 1U].target_seconds) &&
                       anchors[compact - 1U].origin ==
                           HWA_ALIGNMENT_ORIGIN_AUTO) {
                    compact--;
                }
                if (compact != 0U &&
                    (current.reference_seconds <=
                         anchors[compact - 1U].reference_seconds ||
                     current.target_seconds <=
                         anchors[compact - 1U].target_seconds)) {
                    free(anchors);
                    hwa_set_error(error, error_size,
                                  "locked anchors do not remain strictly ordered");
                    return -1;
                }
                anchors[compact++] = current;
            } else if (compact == 0U ||
                       (current.reference_seconds >
                            anchors[compact - 1U].reference_seconds &&
                        current.target_seconds >
                            anchors[compact - 1U].target_seconds)) {
                anchors[compact++] = current;
            }
        }
        output = compact;
    }
    for (index = 0U; index < output; ++index) {
        anchors[index].id = (uint64_t)index + 1U;
    }
    result->anchors = anchors;
    result->anchor_count = output;
    return 0;
}

static double hwa_align_path_target_time(const HWAAlignTrack *reference,
                                         const HWAAlignTrack *target,
                                         const HWADtwResult *path,
                                         double reference_seconds,
                                         double *confidence,
                                         uint32_t *evidence_flags)
{
    size_t low = 0U;
    size_t high = path->count;
    size_t right;
    size_t left;
    double left_time;
    double right_time;

    if (reference_seconds <= 0.0) {
        if (confidence != NULL) {
            *confidence = path->points[0U].confidence;
        }
        if (evidence_flags != NULL) {
            *evidence_flags = path->points[0U].evidence_flags;
        }
        return 0.0;
    }
    if (reference_seconds >= reference->duration_seconds) {
        if (confidence != NULL) {
            *confidence = path->points[path->count - 1U].confidence;
        }
        if (evidence_flags != NULL) {
            *evidence_flags = path->points[path->count - 1U].evidence_flags;
        }
        return target->duration_seconds;
    }
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        double middle_time = reference->frames[
            path->points[middle].reference_index].time_seconds;

        if (middle_time < reference_seconds) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    right = low < path->count ? low : path->count - 1U;
    left = right != 0U ? right - 1U : right;
    left_time = reference->frames[
        path->points[left].reference_index].time_seconds;
    right_time = reference->frames[
        path->points[right].reference_index].time_seconds;
    if (confidence != NULL) {
        *confidence = 0.5 * (path->points[left].confidence +
                             path->points[right].confidence);
    }
    if (evidence_flags != NULL) {
        *evidence_flags = path->points[left].evidence_flags |
                          path->points[right].evidence_flags;
    }
    if (!(right_time > left_time)) {
        return target->frames[path->points[right].target_index].time_seconds;
    }
    return target->frames[path->points[left].target_index].time_seconds +
           (reference_seconds - left_time) /
               (right_time - left_time) *
               (target->frames[path->points[right].target_index].time_seconds -
                target->frames[path->points[left].target_index].time_seconds);
}

static int hwa_align_copy_event_strings(HWAAlignmentMatch *target,
                                        const HWAAlignEvent *source)
{
#define HWA_COPY_EVENT_FIELD(field)                                           \
    do {                                                                      \
        target->field = hwa_align_copy_string(source->field);                 \
        if (source->field != NULL && target->field == NULL) {                 \
            return -1;                                                        \
        }                                                                     \
    } while (0)

    HWA_COPY_EVENT_FIELD(event_id);
    HWA_COPY_EVENT_FIELD(kind);
    HWA_COPY_EVENT_FIELD(voice);
    HWA_COPY_EVENT_FIELD(midi_note);
    HWA_COPY_EVENT_FIELD(velocity);
    HWA_COPY_EVENT_FIELD(tie);
    HWA_COPY_EVENT_FIELD(dynamic);
    HWA_COPY_EVENT_FIELD(mark);
    HWA_COPY_EVENT_FIELD(score_position);
#undef HWA_COPY_EVENT_FIELD
    return 0;
}

static size_t hwa_align_path_boundary(const HWAAlignTrack *reference,
                                      const HWAAlignTrack *target,
                                      const HWADtwResult *path,
                                      size_t start,
                                      double reference_seconds,
                                      double target_seconds,
                                      uint64_t *work)
{
    size_t low = start;
    size_t high = path->count;

    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        const HWAPathPoint *point = &path->points[middle];
        double reference_time =
            reference->frames[point->reference_index].time_seconds;
        double target_time =
            target->frames[point->target_index].time_seconds;

        if (work != NULL && *work != UINT64_MAX) {
            (*work)++;
        }
        if (reference_time >= reference_seconds &&
            target_time >= target_seconds) {
            high = middle;
        } else {
            low = middle + 1U;
        }
    }
    return low < path->count ? low : path->count - 1U;
}

static double hwa_align_path_interval_confidence(
    const HWAAlignTrack *reference,
    const HWAAlignTrack *target,
    const HWADtwResult *path,
    size_t start,
    size_t end,
    double fallback,
    uint32_t fallback_evidence,
    uint32_t *evidence_flags)
{
    long double weighted_confidence = 0.0L;
    long double duration = 0.0L;
    uint32_t evidence = 0U;
    size_t index;

    if (start >= end) {
        if (evidence_flags != NULL) {
            *evidence_flags = fallback_evidence;
        }
        return fallback;
    }
    for (index = start + 1U; index <= end; ++index) {
        const HWAPathPoint *previous = &path->points[index - 1U];
        const HWAPathPoint *point = &path->points[index];
        double reference_duration =
            (double)(point->reference_index - previous->reference_index) *
            reference->step_seconds;
        double target_duration =
            (double)(point->target_index - previous->target_index) *
            target->step_seconds;
        double transition_duration = fmax(reference_duration,
                                          target_duration);

        if (transition_duration > 0.0) {
            weighted_confidence += (long double)transition_duration *
                                   (long double)point->confidence;
            duration += (long double)transition_duration;
        }
        if (point->step != 2U && point->step != 3U) {
            evidence |= point->evidence_flags;
        }
    }
    if (duration == 0.0L) {
        evidence = fallback_evidence;
    }
    if (evidence_flags != NULL) {
        *evidence_flags = evidence;
    }
    return duration != 0.0L
               ? hwa_align_clamp((double)(weighted_confidence / duration),
                                 0.0, 1.0)
               : fallback;
}

static int hwa_align_make_matches(const HWAAlignTrack *reference,
                                  const HWAAlignTrack *target,
                                  const HWAAlignmentOptions *options,
                                  const HWADtwResult *path,
                                  HWAAlignment *result,
                                  char *error,
                                  size_t error_size)
{
    size_t count = reference->is_score != 0
                       ? reference->event_count
                       : (result->anchor_count > 1U
                              ? result->anchor_count - 1U
                              : 0U);
    size_t index;
    size_t path_boundary = 0U;
    uint64_t boundary_work = 0U;

    if (count > options->max_alignment_points) {
        hwa_set_error(error, error_size,
                      "alignment match limit exceeded");
        return -1;
    }
    result->matches = (HWAAlignmentMatch *)calloc(
        count, sizeof(*result->matches));
    if (result->matches == NULL && count != 0U) {
        hwa_set_error(error, error_size,
                      "out of memory for alignment matches");
        return -1;
    }
    result->match_count = count;
    for (index = 0U; index < count; ++index) {
        HWAAlignmentMatch *match = &result->matches[index];

        match->id = (uint64_t)index + 1U;
        if (reference->is_score != 0) {
            const HWAAlignEvent *event = &reference->events[index];
            double start_confidence;
            double end_confidence;
            uint32_t start_evidence;
            uint32_t end_evidence;

            match->reference_start_seconds = event->start_seconds;
            match->reference_end_seconds = event->end_seconds;
            match->target_start_seconds = hwa_align_path_target_time(
                reference, target, path, event->start_seconds,
                &start_confidence, &start_evidence);
            match->target_end_seconds = hwa_align_path_target_time(
                reference, target, path, event->end_seconds,
                &end_confidence, &end_evidence);
            match->score_start_beat = event->start_beat;
            match->score_end_beat = event->end_beat;
            match->score_span_valid = 1;
            {
                size_t start_boundary = hwa_align_path_boundary(
                    reference, target, path, 0U,
                    event->start_seconds, match->target_start_seconds,
                    &boundary_work);
                size_t end_boundary = hwa_align_path_boundary(
                    reference, target, path, start_boundary,
                    event->end_seconds, match->target_end_seconds,
                    &boundary_work);

                match->confidence = hwa_align_path_interval_confidence(
                    reference, target, path, start_boundary, end_boundary,
                    0.5 * (start_confidence + end_confidence),
                    start_evidence | end_evidence,
                    &match->evidence_flags);
            }
            match->status = event->status;
            if (match->status == HWA_ALIGNMENT_MATCHED &&
                match->confidence < options->match_threshold) {
                match->status = HWA_ALIGNMENT_LOW_CONFIDENCE;
            }
            match->tempo_bpm = event->tempo_bpm;
            match->tempo_valid = event->tempo_valid;
            if (hwa_align_copy_event_strings(match, event) != 0) {
                hwa_set_error(error, error_size,
                              "out of memory for score match metadata");
                return -1;
            }
        } else {
            const HWAAlignmentAnchor *first = &result->anchors[index];
            const HWAAlignmentAnchor *second = &result->anchors[index + 1U];
            size_t next_boundary = hwa_align_path_boundary(
                reference, target, path, path_boundary,
                second->reference_seconds, second->target_seconds,
                &boundary_work);
            double endpoint_confidence = 0.5 * (first->confidence +
                                                 second->confidence);
            uint32_t endpoint_evidence = first->evidence_flags |
                                         second->evidence_flags;

            match->reference_start_seconds = first->reference_seconds;
            match->reference_end_seconds = second->reference_seconds;
            match->target_start_seconds = first->target_seconds;
            match->target_end_seconds = second->target_seconds;
            match->confidence = hwa_align_path_interval_confidence(
                reference, target, path, path_boundary, next_boundary,
                endpoint_confidence, endpoint_evidence,
                &match->evidence_flags);
            match->status = match->confidence >= options->match_threshold
                                ? HWA_ALIGNMENT_MATCHED
                                : HWA_ALIGNMENT_LOW_CONFIDENCE;
            path_boundary = next_boundary;
        }
    }
    {
        uint64_t calls = (uint64_t)count;
        uint64_t depth = 0U;
        size_t remaining = path->count;
        uint64_t allowed;

        while (remaining != 0U) {
            depth++;
            remaining /= 2U;
        }
        if (reference->is_score != 0) {
            calls = calls <= UINT64_MAX / 2U ? calls * 2U : UINT64_MAX;
        }
        allowed = depth != 0U && calls <= UINT64_MAX / depth
                      ? calls * depth
                      : UINT64_MAX;
        if (boundary_work > allowed) {
            hwa_set_error(error, error_size,
                          "alignment path-boundary work limit exceeded");
            return -1;
        }
    }
    return 0;
}

static double hwa_align_frame_start(const HWAAlignTrack *track, size_t index)
{
    double start = track->frames[index].time_seconds -
                   0.5 * track->step_seconds;

    return start > 0.0 ? start : 0.0;
}

static double hwa_align_frame_end(const HWAAlignTrack *track, size_t index)
{
    double end = track->frames[index].time_seconds +
                 0.5 * track->step_seconds;

    return end < track->duration_seconds ? end : track->duration_seconds;
}

static HWAUnmatchedReason hwa_align_gap_reason(
    const HWAAlignTrack *reference,
    const HWADtwResult *path,
    size_t start,
    size_t end,
    unsigned char step)
{
    uint32_t flags = 0U;
    size_t index;
    int have_match_before = 0;
    int have_match_after = 0;

    for (index = 1U; index < start; ++index) {
        if (path->points[index].step == 1U ||
            path->points[index].step == 4U ||
            path->points[index].step == 5U) {
            have_match_before = 1;
            break;
        }
    }
    for (index = end; index < path->count; ++index) {
        if (path->points[index].step == 1U ||
            path->points[index].step == 4U ||
            path->points[index].step == 5U) {
            have_match_after = 1;
            break;
        }
    }
    if (have_match_before == 0) {
        return HWA_UNMATCHED_PREFIX;
    }
    if (have_match_after == 0) {
        return HWA_UNMATCHED_SUFFIX;
    }
    for (index = start; index < end; ++index) {
        flags |= reference->frames[path->points[index].reference_index]
                     .score_flags;
    }
    if ((flags & HWA_ALIGN_FRAME_CADENZA) != 0U) {
        return HWA_UNMATCHED_CADENZA;
    }
    if ((flags & HWA_ALIGN_FRAME_REST) != 0U) {
        return HWA_UNMATCHED_REST;
    }
    return step == 2U ? HWA_UNMATCHED_SKIP : HWA_UNMATCHED_REPEAT;
}

static int hwa_align_next_gap_interval(const HWAUnmatchedSpan *spans,
                                       size_t count,
                                       HWAAlignmentSide side,
                                       size_t *index,
                                       double *start,
                                       double *end)
{
    while (*index < count) {
        const HWAUnmatchedSpan *span = &spans[(*index)++];

        if (span->side == side && span->end_seconds > span->start_seconds) {
            *start = span->start_seconds;
            *end = span->end_seconds;
            return 1;
        }
    }
    return 0;
}

static int hwa_align_next_low_interval(const HWAAlignmentMatch *matches,
                                       size_t count,
                                       HWAAlignmentSide side,
                                       double threshold,
                                       size_t *index,
                                       double *start,
                                       double *end)
{
    while (*index < count) {
        const HWAAlignmentMatch *match = &matches[(*index)++];

        if (match->confidence >= threshold) {
            continue;
        }
        if (side == HWA_ALIGNMENT_REFERENCE) {
            *start = match->reference_start_seconds;
            *end = match->reference_end_seconds;
        } else {
            *start = match->target_start_seconds;
            *end = match->target_end_seconds;
        }
        if (*end > *start) {
            return 1;
        }
    }
    return 0;
}

static double hwa_align_unstable_union(const HWAUnmatchedSpan *gap_spans,
                                       size_t gap_count,
                                       const HWAAlignmentMatch *matches,
                                       size_t match_count,
                                       HWAAlignmentSide side,
                                       double threshold)
{
    size_t gap_index = 0U;
    size_t match_index = 0U;
    double gap_start = 0.0;
    double gap_end = 0.0;
    double match_start = 0.0;
    double match_end = 0.0;
    double union_start = 0.0;
    double union_end = 0.0;
    double total = 0.0;
    int have_gap = hwa_align_next_gap_interval(
        gap_spans, gap_count, side, &gap_index, &gap_start, &gap_end);
    int have_match = hwa_align_next_low_interval(
        matches, match_count, side, threshold, &match_index,
        &match_start, &match_end);
    int have_union = 0;

    while (have_gap != 0 || have_match != 0) {
        double start;
        double end;

        if (have_gap != 0 &&
            (have_match == 0 || gap_start <= match_start)) {
            start = gap_start;
            end = gap_end;
            have_gap = hwa_align_next_gap_interval(
                gap_spans, gap_count, side, &gap_index,
                &gap_start, &gap_end);
        } else {
            start = match_start;
            end = match_end;
            have_match = hwa_align_next_low_interval(
                matches, match_count, side, threshold, &match_index,
                &match_start, &match_end);
        }
        if (have_union == 0) {
            union_start = start;
            union_end = end;
            have_union = 1;
        } else if (start > union_end) {
            total += union_end - union_start;
            union_start = start;
            union_end = end;
        } else if (end > union_end) {
            union_end = end;
        }
    }
    if (have_union != 0) {
        total += union_end - union_start;
    }
    return total;
}

static int hwa_align_make_unmatched(const HWAAlignTrack *reference,
                                    const HWAAlignTrack *target,
                                    const HWAAlignmentOptions *options,
                                    const HWADtwResult *path,
                                    HWAAlignment *result,
                                    char *error,
                                    size_t error_size)
{
    HWAUnmatchedSpan *spans;
    size_t capacity;
    size_t count = 0U;
    size_t index = 1U;
    size_t gap_count;
    double unmatched_reference = 0.0;
    double unmatched_target = 0.0;

    if (path->count > SIZE_MAX - result->match_count) {
        hwa_set_error(error, error_size,
                      "unmatched span capacity overflows");
        return -1;
    }
    capacity = path->count + result->match_count;
    if (capacity > options->max_alignment_points) {
        capacity = options->max_alignment_points;
    }
    spans = (HWAUnmatchedSpan *)calloc(capacity, sizeof(*spans));
    if (spans == NULL && capacity != 0U) {
        hwa_set_error(error, error_size,
                      "out of memory for unmatched spans");
        return -1;
    }
    while (index < path->count) {
        unsigned char step = path->points[index].step;
        size_t start = index;

        if (step != 2U && step != 3U) {
            index++;
            continue;
        }
        while (index < path->count && path->points[index].step == step) {
            index++;
        }
        {
            const HWAAlignTrack *side_track = step == 2U ? reference : target;
            size_t first_index = step == 2U
                                     ? path->points[start].reference_index
                                     : path->points[start].target_index;
            size_t last_index = step == 2U
                                    ? path->points[index - 1U]
                                          .reference_index
                                    : path->points[index - 1U].target_index;
            HWAUnmatchedSpan *span;

            if (count >= capacity) {
                free(spans);
                hwa_set_error(error, error_size,
                              "alignment point limit exceeded");
                return -1;
            }
            span = &spans[count++];
            span->side = step == 2U ? HWA_ALIGNMENT_REFERENCE
                                    : HWA_ALIGNMENT_TARGET;
            span->reason = hwa_align_gap_reason(reference, path,
                                                start, index, step);
            span->start_seconds = hwa_align_frame_start(side_track,
                                                        first_index);
            span->end_seconds = hwa_align_frame_end(side_track, last_index);
            if (step == 2U && reference->is_score != 0) {
                const HWAAlignFrame *first =
                    &reference->frames[first_index];
                const HWAAlignFrame *last =
                    &reference->frames[last_index];

                if (first->score_beat_valid != 0 &&
                    last->score_beat_valid != 0) {
                    span->start_beat = first->score_beat;
                    span->end_beat = last->score_beat;
                    span->score_span_valid = 1;
                }
            }
        }
    }
    gap_count = count;
    for (index = 0U; index < result->match_count; ++index) {
        const HWAAlignmentMatch *match = &result->matches[index];

        if (match->confidence < options->match_threshold) {
            HWAUnmatchedSpan *span;

            if (count >= capacity) {
                free(spans);
                hwa_set_error(error, error_size,
                              "alignment point limit exceeded");
                return -1;
            }
            span = &spans[count++];
            span->side = HWA_ALIGNMENT_REFERENCE;
            span->reason = match->evidence_flags != 0U
                               ? HWA_UNMATCHED_LOW_CONFIDENCE
                               : HWA_UNMATCHED_NO_EVIDENCE;
            span->start_seconds = match->reference_start_seconds;
            span->end_seconds = match->reference_end_seconds;
            span->start_beat = match->score_start_beat;
            span->end_beat = match->score_end_beat;
            span->score_span_valid = match->score_span_valid;
            span->confidence = match->confidence;
        }
    }
    unmatched_reference = hwa_align_unstable_union(
        spans, gap_count, result->matches, result->match_count,
        HWA_ALIGNMENT_REFERENCE, options->match_threshold);
    unmatched_target = hwa_align_unstable_union(
        spans, gap_count, result->matches, result->match_count,
        HWA_ALIGNMENT_TARGET, options->match_threshold);
    for (index = 0U; index < count; ++index) {
        spans[index].id = (uint64_t)index + 1U;
    }
    result->unmatched_spans = spans;
    result->unmatched_span_count = count;
    result->matched_coverage = 1.0 - fmax(
        reference->duration_seconds > 0.0
            ? unmatched_reference / reference->duration_seconds
            : 1.0,
        target->duration_seconds > 0.0
            ? unmatched_target / target->duration_seconds
            : 1.0);
    result->matched_coverage = hwa_align_clamp(result->matched_coverage,
                                               0.0, 1.0);
    return 0;
}

static int hwa_align_add_warning(HWAAlignment *result,
                                 size_t capacity,
                                 const char *code,
                                 const char *message)
{
    HWAAlignmentWarning *warning;

    if (result->warning_count >= capacity) {
        return -1;
    }
    warning = &result->warnings[result->warning_count];
    warning->code = hwa_align_copy_string(code);
    warning->message = hwa_align_copy_string(message);
    if (warning->code == NULL || warning->message == NULL) {
        free(warning->code);
        free(warning->message);
        warning->code = NULL;
        warning->message = NULL;
        return -1;
    }
    warning->id = (uint64_t)result->warning_count + 1U;
    result->warning_count++;
    return 0;
}

static int hwa_align_make_warnings(const HWAAlignTrack *reference,
                                   const HWAAlignTrack *target,
                                   HWAAlignment *result,
                                   char *error,
                                   size_t error_size)
{
    const size_t capacity = 3U;
    size_t evidence_count = 0U;
    size_t index;

    result->warnings = (HWAAlignmentWarning *)calloc(
        capacity, sizeof(*result->warnings));
    if (result->warnings == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for alignment warnings");
        return -1;
    }
    for (index = 0U; index < result->anchor_count; ++index) {
        if (result->anchors[index].evidence_flags != 0U) {
            evidence_count++;
        }
    }
    if (result->matched_coverage < 0.60 &&
        hwa_align_add_warning(
            result, capacity, "low_coverage",
            "Less than 60 percent of both timelines has a stable match.") !=
            0) {
        goto failure;
    }
    if (fmin(reference->tuning_confidence, target->tuning_confidence) < 0.20 &&
        hwa_align_add_warning(
            result, capacity, "low_tuning_confidence",
            "The pitch track gives weak evidence for a tuning offset.") != 0) {
        goto failure;
    }
    if (evidence_count == 0U &&
        hwa_align_add_warning(
            result, capacity, "no_alignment_evidence",
            "The alignment contains no shared chroma, onset, pitch, or envelope evidence.") != 0) {
        goto failure;
    }
    return 0;

failure:
    hwa_set_error(error, error_size,
                  "out of memory for alignment warning text");
    return -1;
}

static int hwa_align_track_valid(const HWAAlignTrack *track,
                                 size_t max_points,
                                 char *error,
                                 size_t error_size)
{
    size_t index;

    if (track == NULL || track->frames == NULL || track->frame_count == 0U ||
        track->frame_count > max_points ||
        !isfinite(track->step_seconds) || !(track->step_seconds > 0.0) ||
        !isfinite(track->duration_seconds) ||
        !(track->duration_seconds > 0.0) ||
        !isfinite(track->tuning_offset_cents) ||
        !isfinite(track->tuning_confidence) ||
        track->tuning_confidence < 0.0 ||
        track->tuning_confidence > 1.0 ||
        (track->event_count != 0U && track->events == NULL)) {
        hwa_set_error(error, error_size, "invalid alignment track");
        return -1;
    }
    for (index = 0U; index < track->frame_count; ++index) {
        const HWAAlignFrame *frame = &track->frames[index];
        size_t chroma;

        if (!isfinite(frame->time_seconds) ||
            !isfinite(frame->spectral_onset) ||
            !isfinite(frame->energy_onset) ||
            !isfinite(frame->phase_onset) ||
            !isfinite(frame->combined_onset) ||
            !isfinite(frame->log_energy) ||
            !isfinite(frame->pitch_class) ||
            !isfinite(frame->pitch_confidence) ||
            !isfinite(frame->activity) ||
            frame->time_seconds < 0.0 ||
            frame->time_seconds > track->duration_seconds ||
            (frame->score_beat_valid != 0 &&
             (!isfinite(frame->score_beat) || frame->score_beat < 0.0)) ||
            (track->is_score != 0 && frame->score_beat_valid == 0) ||
            (index != 0U &&
             (frame->time_seconds <
                  track->frames[index - 1U].time_seconds ||
              (track->is_score != 0 &&
               frame->score_beat <
                   track->frames[index - 1U].score_beat)))) {
            hwa_set_error(error, error_size,
                          "alignment track contains a non-finite or unordered frame");
            return -1;
        }
        for (chroma = 0U; chroma < HWA_CHROMA_BIN_COUNT; ++chroma) {
            if (!isfinite(frame->chroma[chroma]) ||
                frame->chroma[chroma] < 0.0) {
                hwa_set_error(error, error_size,
                              "alignment track contains invalid chroma");
                return -1;
            }
        }
    }
    return 0;
}

static uint64_t hwa_align_frame_array_bytes(size_t count)
{
    return count <= SIZE_MAX / sizeof(HWAAlignFrame)
               ? (uint64_t)(count * sizeof(HWAAlignFrame))
               : UINT64_MAX;
}

static uint64_t hwa_align_corridor_bytes(size_t rows)
{
    uint64_t bytes = 0U;

    if (hwa_align_add_bytes(rows, sizeof(size_t), &bytes) != 0 ||
        hwa_align_add_bytes(rows, sizeof(size_t), &bytes) != 0 ||
        hwa_align_add_bytes(rows, sizeof(size_t), &bytes) != 0 ||
        hwa_align_add_bytes(rows, sizeof(unsigned char), &bytes) != 0) {
        return UINT64_MAX;
    }
    return bytes;
}

static size_t hwa_align_auto_anchor_capacity(
    const HWAAlignTrack *reference,
    const HWAAlignTrack *target,
    const HWADtwResult *path,
    const HWAAlignmentAnchor *locked,
    size_t locked_count)
{
    double last_auto = -DBL_MAX;
    size_t count = 0U;
    size_t index;

    for (index = 0U; index < path->count; ++index) {
        const HWAPathPoint *point = &path->points[index];
        double time = reference->frames[point->reference_index].time_seconds;
        int endpoint = index == 0U || index + 1U == path->count;
        int landmark = reference->frames[point->reference_index]
                           .combined_onset >= 0.35;
        double output_reference = index == 0U
                                      ? 0.0
                                      : (index + 1U == path->count
                                             ? reference->duration_seconds
                                             : time);
        double output_target = index == 0U
                                   ? 0.0
                                   : (index + 1U == path->count
                                          ? target->duration_seconds
                                          : target->frames[point->target_index]
                                                .time_seconds);
        int suppress = endpoint
                           ? hwa_align_exact_locked(
                                 output_reference, output_target,
                                 locked, locked_count)
                           : hwa_align_near_locked(
                                 time, reference->step_seconds * 0.51,
                                 locked, locked_count);

        if ((endpoint ||
             (point->step == 1U &&
              (landmark || time - last_auto >= 1.0))) &&
            !suppress) {
            count++;
            last_auto = time;
        }
    }
    if (path->count == 1U &&
        !hwa_align_exact_locked(reference->duration_seconds,
                                target->duration_seconds,
                                locked, locked_count)) {
        count++;
    }
    return count;
}

static int hwa_align_result_preflight(
    const HWAAlignTrack *reference,
    const HWAAlignTrack *target,
    const HWAAlignmentOptions *options,
    const HWADtwResult *path,
    const HWACorridor *corridor,
    const HWAAlignmentAnchor *locked,
    size_t locked_count,
    char *error,
    size_t error_size)
{
    size_t anchor_capacity = hwa_align_auto_anchor_capacity(
        reference, target, path, locked, locked_count);
    size_t match_capacity;
    size_t unmatched_capacity;
    uint64_t bytes = hwa_align_corridor_bytes(corridor->rows);
    const char *warning_text[] = {
        "low_coverage",
        "Less than 60 percent of both timelines has a stable match.",
        "low_tuning_confidence",
        "The pitch track gives weak evidence for a tuning offset.",
        "no_alignment_evidence",
        "The alignment contains no shared chroma, onset, pitch, or envelope evidence."
    };
    size_t index;

    if (anchor_capacity > SIZE_MAX - locked_count) {
        goto overflow;
    }
    anchor_capacity += locked_count;
    match_capacity = reference->is_score != 0
                         ? reference->event_count
                         : (anchor_capacity > 1U
                                ? anchor_capacity - 1U
                                : 0U);
    if (path->count > SIZE_MAX - match_capacity) {
        goto overflow;
    }
    unmatched_capacity = path->count + match_capacity;
    if (unmatched_capacity > options->max_alignment_points) {
        unmatched_capacity = options->max_alignment_points;
    }
    if (bytes == UINT64_MAX ||
        hwa_align_add_bytes(path->capacity,
                            sizeof(*path->points), &bytes) != 0 ||
        hwa_align_add_bytes(anchor_capacity,
                            sizeof(HWAAlignmentAnchor), &bytes) != 0 ||
        hwa_align_add_bytes(match_capacity,
                            sizeof(HWAAlignmentMatch), &bytes) != 0 ||
        hwa_align_add_bytes(unmatched_capacity,
                            sizeof(HWAUnmatchedSpan), &bytes) != 0 ||
        hwa_align_add_bytes(3U, sizeof(HWAAlignmentWarning), &bytes) != 0) {
        goto overflow;
    }
    if (reference->is_score != 0) {
        for (index = 0U; index < reference->event_count; ++index) {
            const HWAAlignEvent *event = &reference->events[index];
            const char *fields[] = {
                event->event_id, event->kind, event->voice,
                event->midi_note, event->velocity, event->tie,
                event->dynamic, event->mark, event->score_position
            };
            size_t field;

            for (field = 0U;
                 field < sizeof(fields) / sizeof(fields[0]); ++field) {
                if (fields[field] != NULL) {
                    size_t length = strlen(fields[field]);

                    if (length == SIZE_MAX ||
                        (uint64_t)(length + 1U) > UINT64_MAX - bytes) {
                        goto overflow;
                    }
                    bytes += (uint64_t)(length + 1U);
                }
            }
        }
    }
    for (index = 0U;
         index < sizeof(warning_text) / sizeof(warning_text[0]); ++index) {
        size_t length = strlen(warning_text[index]);

        if ((uint64_t)(length + 1U) > UINT64_MAX - bytes) {
            goto overflow;
        }
        bytes += (uint64_t)(length + 1U);
    }
    if (bytes > options->max_alignment_work_bytes) {
        hwa_set_error(error, error_size,
                      "alignment work byte limit exceeded");
        return -1;
    }
    return 0;

overflow:
    hwa_set_error(error, error_size,
                  "alignment work byte limit exceeded");
    return -1;
}

int hwa_align_tracks(const HWAAlignTrack *reference,
                     const HWAAlignTrack *target,
                     const HWAAlignmentOptions *options,
                     const HWAAlignmentAnchor *locked_anchors,
                     size_t locked_anchor_count,
                     HWAAlignment *result,
                     char *error,
                     size_t error_size)
{
    HWAAlignFrame *coarse_reference_frames = NULL;
    HWAAlignFrame *coarse_target_frames = NULL;
    HWAAlignTrack coarse_reference;
    HWAAlignTrack coarse_target;
    HWACorridor coarse_corridor;
    HWACorridor fine_corridor;
    HWADtwResult coarse_path;
    HWADtwResult fine_path;
    uint64_t coarse_cells = 0U;
    uint64_t persistent_bytes;
    uint64_t available_bytes;
    long double confidence_sum = 0.0L;
    size_t confidence_count = 0U;
    size_t index;
    int status = -1;

    memset(&coarse_reference, 0, sizeof(coarse_reference));
    memset(&coarse_target, 0, sizeof(coarse_target));
    memset(&coarse_corridor, 0, sizeof(coarse_corridor));
    memset(&fine_corridor, 0, sizeof(fine_corridor));
    memset(&coarse_path, 0, sizeof(coarse_path));
    memset(&fine_path, 0, sizeof(fine_path));
    if (error != NULL && error_size > 0U) {
        error[0] = '\0';
    }
    if (result == NULL) {
        hwa_set_error(error, error_size, "alignment result is null");
        return -1;
    }
    memset(result, 0, sizeof(*result));
    if (hwa_alignment_options_valid(options, error, error_size) != 0 ||
        hwa_align_track_valid(reference, options->max_alignment_points,
                              error, error_size) != 0 ||
        hwa_align_track_valid(target, options->max_alignment_points,
                              error, error_size) != 0) {
        return -1;
    }
    if (reference->is_score != 0 &&
        reference->event_count > options->max_score_events) {
        hwa_set_error(error, error_size, "score event limit exceeded");
        return -1;
    }
    if (reference->frame_count > SIZE_MAX - target->frame_count ||
        reference->frame_count + target->frame_count >
            options->max_alignment_points) {
        hwa_set_error(error, error_size,
                      "alignment point limit exceeded");
        return -1;
    }
    if (hwa_align_validate_locked(reference, target,
                                  locked_anchors, locked_anchor_count,
                                  options->max_manual_anchors,
                                  error, error_size) != 0) {
        return -1;
    }
    if (hwa_align_resample_track(reference, options->coarse_step_seconds,
                                 options->max_alignment_work_bytes,
                                 &coarse_reference_frames,
                                 &coarse_reference,
                                 error, error_size) != 0) {
        goto cleanup;
    }
    persistent_bytes = hwa_align_frame_array_bytes(
        coarse_reference.frame_count);
    if (persistent_bytes >= options->max_alignment_work_bytes) {
        hwa_set_error(error, error_size,
                      "alignment work byte limit exceeded");
        goto cleanup;
    }
    if (hwa_align_resample_track(
            target, options->coarse_step_seconds,
            options->max_alignment_work_bytes - persistent_bytes,
            &coarse_target_frames, &coarse_target,
            error, error_size) != 0) {
        goto cleanup;
    }
    {
        uint64_t target_bytes = hwa_align_frame_array_bytes(
            coarse_target.frame_count);

        if (target_bytes == UINT64_MAX ||
            target_bytes > UINT64_MAX - persistent_bytes) {
            hwa_set_error(error, error_size,
                          "alignment work byte limit exceeded");
            goto cleanup;
        }
        persistent_bytes += target_bytes;
    }
    if (persistent_bytes >= options->max_alignment_work_bytes) {
        hwa_set_error(error, error_size,
                      "alignment work byte limit exceeded");
        goto cleanup;
    }
    available_bytes = options->max_alignment_work_bytes - persistent_bytes;
    if (hwa_align_build_coarse_corridor(
            &coarse_reference, &coarse_target,
            options->dtw_band_seconds,
            locked_anchors, locked_anchor_count,
            available_bytes, &coarse_corridor,
            error, error_size) != 0) {
        goto cleanup;
    }
    if (coarse_corridor.cell_count > options->max_dtw_cells) {
        hwa_set_error(error, error_size, "DTW cell limit exceeded");
        goto cleanup;
    }
    {
        uint64_t corridor_bytes = hwa_align_corridor_bytes(
            coarse_corridor.rows);

        if (corridor_bytes == UINT64_MAX ||
            corridor_bytes >= available_bytes) {
            hwa_set_error(error, error_size,
                          "alignment work byte limit exceeded");
            goto cleanup;
        }
        available_bytes -= corridor_bytes;
    }
    if (hwa_align_run_dtw(&coarse_reference, &coarse_target, options,
                          &coarse_corridor, available_bytes,
                          &coarse_path, error, error_size) != 0) {
        goto cleanup;
    }
    coarse_cells = coarse_path.cells;
    hwa_align_corridor_free(&coarse_corridor);
    persistent_bytes = hwa_align_frame_array_bytes(
        coarse_reference.frame_count);
    {
        uint64_t amount = hwa_align_frame_array_bytes(
            coarse_target.frame_count);
        uint64_t path_bytes =
            coarse_path.capacity <= SIZE_MAX / sizeof(*coarse_path.points)
                ? (uint64_t)(coarse_path.capacity *
                             sizeof(*coarse_path.points))
                : UINT64_MAX;

        if (amount == UINT64_MAX || path_bytes == UINT64_MAX ||
            amount > UINT64_MAX - persistent_bytes ||
            path_bytes > UINT64_MAX - persistent_bytes - amount) {
            hwa_set_error(error, error_size,
                          "alignment work byte limit exceeded");
            goto cleanup;
        }
        persistent_bytes += amount + path_bytes;
    }
    if (persistent_bytes >= options->max_alignment_work_bytes) {
        hwa_set_error(error, error_size,
                      "alignment work byte limit exceeded");
        goto cleanup;
    }
    available_bytes = options->max_alignment_work_bytes - persistent_bytes;
    if (hwa_align_build_fine_corridor(
            reference, target, &coarse_reference, &coarse_target,
            &coarse_path, options->fine_radius_seconds,
            locked_anchors, locked_anchor_count,
            available_bytes, &fine_corridor,
            error, error_size) != 0) {
        goto cleanup;
    }
    if (fine_corridor.cell_count >
        options->max_dtw_cells - coarse_cells) {
        hwa_set_error(error, error_size, "DTW cell limit exceeded");
        goto cleanup;
    }
    hwa_align_dtw_free(&coarse_path);
    hwa_align_track_release(coarse_reference_frames, &coarse_reference);
    coarse_reference_frames = NULL;
    hwa_align_track_release(coarse_target_frames, &coarse_target);
    coarse_target_frames = NULL;
    persistent_bytes = hwa_align_corridor_bytes(fine_corridor.rows);
    if (persistent_bytes >= options->max_alignment_work_bytes) {
        hwa_set_error(error, error_size,
                      "alignment work byte limit exceeded");
        goto cleanup;
    }
    available_bytes = options->max_alignment_work_bytes - persistent_bytes;
    if (hwa_align_run_dtw(reference, target, options,
                          &fine_corridor, available_bytes,
                          &fine_path, error, error_size) != 0) {
        goto cleanup;
    }
    if (hwa_align_path_contains_locked(reference, target, &fine_path,
                                       locked_anchors, locked_anchor_count,
                                       error, error_size) != 0) {
        goto cleanup;
    }
    result->options = *options;
    result->reference_duration_seconds = reference->duration_seconds;
    result->target_duration_seconds = target->duration_seconds;
    result->tuning_offset_cents = target->tuning_offset_cents -
                                  reference->tuning_offset_cents;
    result->tuning_confidence = fmin(reference->tuning_confidence,
                                     target->tuning_confidence);
    result->total_cost = fine_path.total_cost;
    result->normalized_cost = fine_path.count != 0U
                                  ? fine_path.total_cost /
                                        (double)fine_path.count
                                  : 0.0;
    result->dtw_cells = coarse_cells + fine_path.cells;
    result->path_points = (uint64_t)fine_path.count;
    for (index = 0U; index < fine_path.count; ++index) {
        if (fine_path.points[index].step != 2U &&
            fine_path.points[index].step != 3U) {
            confidence_sum +=
                (long double)fine_path.points[index].confidence;
            confidence_count++;
        }
    }
    if (hwa_align_result_preflight(reference, target, options, &fine_path,
                                   &fine_corridor,
                                   locked_anchors, locked_anchor_count,
                                   error, error_size) != 0) {
        goto cleanup;
    }
    if (hwa_align_make_anchors(reference, target, options, &fine_path,
                               locked_anchors, locked_anchor_count,
                               result, error, error_size) != 0 ||
        hwa_align_make_matches(reference, target, options, &fine_path,
                               result, error, error_size) != 0 ||
        hwa_align_make_unmatched(reference, target, options, &fine_path,
                                 result, error, error_size) != 0) {
        goto cleanup;
    }
    result->global_confidence = confidence_count != 0U
                                    ? (double)(confidence_sum /
                                               (long double)confidence_count) *
                                          result->matched_coverage
                                    : 0.0;
    result->global_confidence = hwa_align_clamp(result->global_confidence,
                                                0.0, 1.0);
    if (hwa_align_make_warnings(reference, target, result,
                                error, error_size) != 0) {
        goto cleanup;
    }
    status = 0;

cleanup:
    hwa_align_corridor_free(&fine_corridor);
    hwa_align_corridor_free(&coarse_corridor);
    hwa_align_dtw_free(&fine_path);
    hwa_align_dtw_free(&coarse_path);
    hwa_align_track_release(coarse_reference_frames, &coarse_reference);
    hwa_align_track_release(coarse_target_frames, &coarse_target);
    if (status != 0) {
        hwa_alignment_free(result);
    }
    return status;
}

static int hwa_align_hash_stable(const char *path,
                                 uint64_t max_bytes,
                                 const char before[HWA_SHA256_HEX_SIZE],
                                 char after[HWA_SHA256_HEX_SIZE],
                                 char *error,
                                 size_t error_size)
{
    if (hwa_sha256_file(path, max_bytes, after, error, error_size) != 0) {
        return -1;
    }
    if (strcmp(before, after) != 0) {
        hwa_set_error(error, error_size,
                      "input changed while it was analyzed");
        return -1;
    }
    return 0;
}

int hwa_align_audio_wav(const char *reference_path,
                        const char *target_path,
                        const HWAAlignmentOptions *provided_options,
                        const HWAAlignmentAnchor *locked_anchors,
                        size_t locked_anchor_count,
                        HWAAlignment *alignment,
                        char *error,
                        size_t error_size)
{
    HWAAlignmentOptions selected_options;
    HWAAlignmentOptions engine_options;
    HWAAnalysis reference_analysis;
    HWAAnalysis target_analysis;
    HWAAlignFrame *reference_frames = NULL;
    HWAAlignFrame *target_frames = NULL;
    HWAAlignTrack reference_track;
    HWAAlignTrack target_track;
    uint64_t retained_track_bytes = 0U;
    char reference_before[HWA_SHA256_HEX_SIZE];
    char reference_after[HWA_SHA256_HEX_SIZE];
    char target_before[HWA_SHA256_HEX_SIZE];
    char target_after[HWA_SHA256_HEX_SIZE];
    int status = -1;

    memset(&reference_analysis, 0, sizeof(reference_analysis));
    memset(&target_analysis, 0, sizeof(target_analysis));
    memset(&reference_track, 0, sizeof(reference_track));
    memset(&target_track, 0, sizeof(target_track));
    if (error != NULL && error_size > 0U) {
        error[0] = '\0';
    }
    if (alignment == NULL) {
        hwa_set_error(error, error_size,
                      "invalid audio alignment arguments");
        return -1;
    }
    if (provided_options == NULL) {
        hwa_alignment_options_default(&selected_options);
    } else {
        selected_options = *provided_options;
    }
    memset(alignment, 0, sizeof(*alignment));
    if (reference_path == NULL || target_path == NULL) {
        hwa_set_error(error, error_size,
                      "invalid audio alignment arguments");
        return -1;
    }
    if (strcmp(reference_path, "-") == 0 || strcmp(target_path, "-") == 0) {
        hwa_set_error(error, error_size,
                      "alignment needs named regular input files");
        return -1;
    }
    selected_options.analysis.collect_tracks = 1;
    selected_options.analysis.collect_spectrogram = 0;
    selected_options.analysis.true_peak_oversample = 1U;
    if (hwa_alignment_options_valid(&selected_options,
                                    error, error_size) != 0) {
        return -1;
    }
    if (hwa_sha256_file(reference_path,
                        selected_options.analysis.max_input_bytes,
                        reference_before, error, error_size) != 0 ||
        hwa_sha256_file(target_path,
                        selected_options.analysis.max_input_bytes,
                        target_before, error, error_size) != 0) {
        goto cleanup;
    }
    if (hwa_analyze_wav_with_options(reference_path,
                                     &selected_options.analysis,
                                     &reference_analysis,
                                     error, error_size) != 0 ||
        hwa_align_hash_stable(reference_path,
                              selected_options.analysis.max_input_bytes,
                              reference_before, reference_after,
                              error, error_size) != 0) {
        goto cleanup;
    }
    if (hwa_analyze_wav_with_options(target_path,
                                     &selected_options.analysis,
                                     &target_analysis,
                                     error, error_size) != 0 ||
        hwa_align_hash_stable(target_path,
                              selected_options.analysis.max_input_bytes,
                              target_before, target_after,
                              error, error_size) != 0) {
        goto cleanup;
    }
    if (hwa_align_track_from_analysis(
            &reference_analysis, selected_options.alignment_step_seconds,
            selected_options.max_alignment_work_bytes,
            selected_options.max_alignment_points,
            &reference_frames, &reference_track,
            error, error_size) != 0) {
        goto cleanup;
    }
    {
        uint64_t reference_bytes = hwa_align_frame_array_bytes(
            reference_track.frame_count);

        if (reference_bytes >= selected_options.max_alignment_work_bytes ||
            hwa_align_track_from_analysis(
                &target_analysis, selected_options.alignment_step_seconds,
                selected_options.max_alignment_work_bytes - reference_bytes,
                selected_options.max_alignment_points,
                &target_frames, &target_track,
                error, error_size) != 0) {
            if (error != NULL && error_size > 0U && error[0] == '\0') {
                hwa_set_error(error, error_size,
                              "alignment work byte limit exceeded");
            }
            goto cleanup;
        }
        retained_track_bytes = reference_bytes;
    }
    {
        uint64_t target_bytes = hwa_align_frame_array_bytes(
            target_track.frame_count);
        size_t reference_path_size = strlen(reference_path) + 1U;
        size_t target_path_size = strlen(target_path) + 1U;

        if (target_bytes == UINT64_MAX ||
            target_bytes > UINT64_MAX - retained_track_bytes ||
            (uint64_t)reference_path_size >
                UINT64_MAX - retained_track_bytes - target_bytes ||
            (uint64_t)target_path_size >
                UINT64_MAX - retained_track_bytes - target_bytes -
                    (uint64_t)reference_path_size ||
            retained_track_bytes + target_bytes +
                    (uint64_t)reference_path_size +
                    (uint64_t)target_path_size >=
                selected_options.max_alignment_work_bytes) {
            hwa_set_error(error, error_size,
                          "alignment work byte limit exceeded");
            goto cleanup;
        }
        retained_track_bytes += target_bytes +
                                (uint64_t)reference_path_size +
                                (uint64_t)target_path_size;
    }
    engine_options = selected_options;
    engine_options.max_alignment_work_bytes =
        selected_options.max_alignment_work_bytes - retained_track_bytes;
    if (hwa_align_tracks(&reference_track, &target_track,
                         &engine_options,
                         locked_anchors, locked_anchor_count,
                         alignment, error, error_size) != 0) {
        goto cleanup;
    }
    alignment->options = selected_options;
    alignment->mode = HWA_ALIGNMENT_AUDIO_TO_AUDIO;
    alignment->reference_path = hwa_align_copy_string(reference_path);
    alignment->target_path = hwa_align_copy_string(target_path);
    if (alignment->reference_path == NULL || alignment->target_path == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for alignment paths");
        hwa_alignment_free(alignment);
        goto cleanup;
    }
    memcpy(alignment->reference_sha256, reference_after,
           sizeof(alignment->reference_sha256));
    memcpy(alignment->target_sha256, target_after,
           sizeof(alignment->target_sha256));
    status = 0;

cleanup:
    hwa_align_track_release(target_frames, &target_track);
    hwa_align_track_release(reference_frames, &reference_track);
    hwa_analysis_free(&target_analysis);
    hwa_analysis_free(&reference_analysis);
    return status;
}

int hwa_align_score_manifest_wav(
    const char *score_path,
    const char *audio_path,
    const HWAAlignmentOptions *provided_options,
    const HWAAlignmentAnchor *locked_anchors,
    size_t locked_anchor_count,
    HWAAlignment *alignment,
    char *error,
    size_t error_size)
{
    HWAAlignmentOptions selected_options;
    HWAAlignmentOptions engine_options;
    HWAScoreManifest manifest;
    HWAAnalysis audio_analysis;
    HWAAlignFrame *score_frames = NULL;
    HWAAlignEvent *score_events = NULL;
    HWAAlignFrame *audio_frames = NULL;
    HWAAlignTrack score_track;
    HWAAlignTrack audio_track;
    uint64_t retained_track_bytes = 0U;
    char score_before[HWA_SHA256_HEX_SIZE];
    char score_after[HWA_SHA256_HEX_SIZE];
    char audio_before[HWA_SHA256_HEX_SIZE];
    char audio_after[HWA_SHA256_HEX_SIZE];
    uint64_t score_byte_cap;
    int status = -1;

    memset(&manifest, 0, sizeof(manifest));
    memset(&audio_analysis, 0, sizeof(audio_analysis));
    memset(&score_track, 0, sizeof(score_track));
    memset(&audio_track, 0, sizeof(audio_track));
    if (error != NULL && error_size > 0U) {
        error[0] = '\0';
    }
    if (alignment == NULL) {
        hwa_set_error(error, error_size,
                      "invalid score alignment arguments");
        return -1;
    }
    if (provided_options == NULL) {
        hwa_alignment_options_default(&selected_options);
    } else {
        selected_options = *provided_options;
    }
    memset(alignment, 0, sizeof(*alignment));
    if (score_path == NULL || audio_path == NULL) {
        hwa_set_error(error, error_size,
                      "invalid score alignment arguments");
        return -1;
    }
    if (strcmp(score_path, "-") == 0 || strcmp(audio_path, "-") == 0) {
        hwa_set_error(error, error_size,
                      "alignment needs named regular input files");
        return -1;
    }
    selected_options.analysis.collect_tracks = 1;
    selected_options.analysis.collect_spectrogram = 0;
    selected_options.analysis.true_peak_oversample = 1U;
    if (hwa_alignment_options_valid(&selected_options,
                                    error, error_size) != 0) {
        return -1;
    }
    score_byte_cap = selected_options.max_alignment_work_bytes / 2U;
    if (score_byte_cap == 0U) {
        hwa_set_error(error, error_size,
                      "alignment work byte limit exceeded");
        return -1;
    }
    if (score_byte_cap > selected_options.analysis.max_input_bytes) {
        score_byte_cap = selected_options.analysis.max_input_bytes;
    }
    if (hwa_sha256_file(score_path,
                        score_byte_cap,
                        score_before, error, error_size) != 0 ||
        hwa_sha256_file(audio_path,
                        selected_options.analysis.max_input_bytes,
                        audio_before, error, error_size) != 0) {
        goto cleanup;
    }
    if (hwa_score_manifest_load(
            score_path, score_byte_cap,
            selected_options.max_score_events,
            &manifest, error, error_size) != 0 ||
        hwa_align_hash_stable(score_path,
                              score_byte_cap,
                              score_before, score_after,
                              error, error_size) != 0 ||
        strcmp(score_after, manifest.sha256) != 0) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "score input changed while it was parsed");
        }
        goto cleanup;
    }
    if (hwa_analyze_wav_with_options(audio_path,
                                     &selected_options.analysis,
                                     &audio_analysis,
                                     error, error_size) != 0 ||
        hwa_align_hash_stable(audio_path,
                              selected_options.analysis.max_input_bytes,
                              audio_before, audio_after,
                              error, error_size) != 0) {
        goto cleanup;
    }
    if (hwa_score_manifest_build_track(
            &manifest, selected_options.alignment_step_seconds,
            selected_options.max_alignment_work_bytes,
            selected_options.max_alignment_points,
            &score_frames, &score_events, &score_track,
            error, error_size) != 0) {
        goto cleanup;
    }
    {
        uint64_t score_bytes = hwa_align_frame_array_bytes(
            score_track.frame_count);
        uint64_t event_bytes;

        if (score_track.event_count > SIZE_MAX / sizeof(*score_events)) {
            hwa_set_error(error, error_size,
                          "alignment work byte limit exceeded");
            goto cleanup;
        }
        event_bytes = (uint64_t)(score_track.event_count *
                                 sizeof(*score_events));
        if (score_bytes == UINT64_MAX ||
            event_bytes > UINT64_MAX - score_bytes ||
            score_bytes + event_bytes >=
                selected_options.max_alignment_work_bytes) {
            hwa_set_error(error, error_size,
                          "alignment work byte limit exceeded");
            goto cleanup;
        }
        score_bytes += event_bytes;
        retained_track_bytes = score_bytes;
        if (hwa_align_track_from_analysis(
                &audio_analysis, selected_options.alignment_step_seconds,
                selected_options.max_alignment_work_bytes - score_bytes,
                selected_options.max_alignment_points,
                &audio_frames, &audio_track,
                error, error_size) != 0) {
            goto cleanup;
        }
    }
    {
        uint64_t audio_bytes = hwa_align_frame_array_bytes(
            audio_track.frame_count);
        size_t score_path_size = strlen(score_path) + 1U;
        size_t audio_path_size = strlen(audio_path) + 1U;

        if (audio_bytes == UINT64_MAX ||
            audio_bytes > UINT64_MAX - retained_track_bytes ||
            (uint64_t)score_path_size >
                UINT64_MAX - retained_track_bytes - audio_bytes ||
            (uint64_t)audio_path_size >
                UINT64_MAX - retained_track_bytes - audio_bytes -
                    (uint64_t)score_path_size ||
            retained_track_bytes + audio_bytes +
                    (uint64_t)score_path_size +
                    (uint64_t)audio_path_size >=
                selected_options.max_alignment_work_bytes) {
            hwa_set_error(error, error_size,
                          "alignment work byte limit exceeded");
            goto cleanup;
        }
        retained_track_bytes += audio_bytes +
                                (uint64_t)score_path_size +
                                (uint64_t)audio_path_size;
    }
    engine_options = selected_options;
    engine_options.max_alignment_work_bytes =
        selected_options.max_alignment_work_bytes - retained_track_bytes;
    if (hwa_align_tracks(&score_track, &audio_track,
                         &engine_options,
                         locked_anchors, locked_anchor_count,
                         alignment, error, error_size) != 0) {
        goto cleanup;
    }
    alignment->options = selected_options;
    alignment->mode = HWA_ALIGNMENT_SCORE_TO_AUDIO;
    alignment->score_path = hwa_align_copy_string(score_path);
    alignment->target_path = hwa_align_copy_string(audio_path);
    if (alignment->score_path == NULL || alignment->target_path == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for alignment paths");
        hwa_alignment_free(alignment);
        goto cleanup;
    }
    memcpy(alignment->score_sha256, score_after,
           sizeof(alignment->score_sha256));
    memcpy(alignment->target_sha256, audio_after,
           sizeof(alignment->target_sha256));
    status = 0;

cleanup:
    hwa_align_track_release(audio_frames, &audio_track);
    hwa_score_manifest_release_track(score_frames, score_events,
                                     &score_track);
    hwa_analysis_free(&audio_analysis);
    hwa_score_manifest_free(&manifest);
    return status;
}

#include "body_envelope.h"

#include "internal.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HWA_BODY_DEFAULT_MIN_HZ 120.0
#define HWA_BODY_DEFAULT_MAX_HZ 12000.0
#define HWA_BODY_DEFAULT_PITCH_CONFIDENCE 0.55
#define HWA_BODY_DEFAULT_MAX_HARMONICS 32U
#define HWA_BODY_DEFAULT_BINS_PER_OCTAVE 24U
#define HWA_BODY_DEFAULT_FIT_PASSES 8U
#define HWA_BODY_DEFAULT_MAX_OBSERVATIONS 2000000U
#define HWA_BODY_DEFAULT_MAX_POINTS 512U
#define HWA_BODY_DEFAULT_MAX_FIT_EVALUATIONS UINT64_C(100000000)
#define HWA_BODY_PITCH_CELLS 256U
#define HWA_BODY_HUBER_DB 6.0

typedef struct HWABodyObservation {
    size_t frame;
    size_t point;
    size_t harmonic;
    unsigned pitch_cell;
    double level_db;
    double weight;
} HWABodyObservation;

static int hwa_body_u64_add(uint64_t left, uint64_t right, uint64_t *result)
{
    if (result == NULL || left > UINT64_MAX - right) return -1;
    *result = left + right;
    return 0;
}

static int hwa_body_u64_multiply(uint64_t left,
                                 uint64_t right,
                                 uint64_t *result)
{
    if (result == NULL || (left != 0U && right > UINT64_MAX / left)) {
        return -1;
    }
    *result = left * right;
    return 0;
}

static int hwa_body_size_multiply(size_t left, size_t right, size_t *result)
{
    if (result == NULL || (left != 0U && right > SIZE_MAX / left)) return -1;
    *result = left * right;
    return 0;
}

static char *hwa_body_copy_string(const char *text)
{
    size_t length;
    char *copy;

    if (text == NULL || strlen(text) == SIZE_MAX) return NULL;
    length = strlen(text) + 1U;
    copy = (char *)malloc(length);
    if (copy != NULL) memcpy(copy, text, length);
    return copy;
}

static void hwa_body_estimate_free(HWABodyEnvelopeEstimate *estimate)
{
    if (estimate == NULL) return;
    free(estimate->path);
    free(estimate->points);
    memset(estimate, 0, sizeof(*estimate));
}

void hwa_body_envelope_result_free(HWABodyEnvelopeResult *result)
{
    if (result == NULL) return;
    hwa_body_estimate_free(&result->reference);
    hwa_body_estimate_free(&result->model);
    free(result->gaps);
    memset(result, 0, sizeof(*result));
}

void hwa_body_envelope_options_default(HWABodyEnvelopeOptions *options)
{
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    hwa_analysis_options_default(&options->analysis);
    options->analysis.frame_size = 4096U;
    options->analysis.hop_size = 1024U;
    options->analysis.max_work_bytes = UINT64_C(536870912);
    options->analysis.max_transforms = 1000000U;
    options->analysis.max_track_points = 500000U;
    options->analysis.max_spectrum_values = 64000000U;
    options->min_frequency_hz = HWA_BODY_DEFAULT_MIN_HZ;
    options->max_frequency_hz = HWA_BODY_DEFAULT_MAX_HZ;
    options->min_pitch_confidence = HWA_BODY_DEFAULT_PITCH_CONFIDENCE;
    options->max_harmonics = HWA_BODY_DEFAULT_MAX_HARMONICS;
    options->bins_per_octave = HWA_BODY_DEFAULT_BINS_PER_OCTAVE;
    options->fit_passes = HWA_BODY_DEFAULT_FIT_PASSES;
    options->max_observations = HWA_BODY_DEFAULT_MAX_OBSERVATIONS;
    options->max_points = HWA_BODY_DEFAULT_MAX_POINTS;
    options->max_fit_evaluations =
        HWA_BODY_DEFAULT_MAX_FIT_EVALUATIONS;
}

static int hwa_body_options_validate(const HWABodyEnvelopeOptions *options,
                                     size_t *point_count,
                                     char *error,
                                     size_t error_size)
{
    double span;
    double points;

    if (options == NULL || point_count == NULL) {
        hwa_set_error(error, error_size, "invalid body-envelope options");
        return -1;
    }
    if (hwa_analysis_options_validate(&options->analysis,
                                      error, error_size) != 0) {
        return -1;
    }
    if (!isfinite(options->min_frequency_hz) ||
        !isfinite(options->max_frequency_hz) ||
        options->min_frequency_hz < 20.0 ||
        options->max_frequency_hz <= options->min_frequency_hz ||
        options->max_frequency_hz > 96000.0) {
        hwa_set_error(error, error_size,
                      "body-envelope frequency range is invalid");
        return -1;
    }
    if (!isfinite(options->min_pitch_confidence) ||
        options->min_pitch_confidence < 0.0 ||
        options->min_pitch_confidence > 1.0) {
        hwa_set_error(error, error_size,
                      "body-envelope pitch confidence is invalid");
        return -1;
    }
    if (options->max_harmonics == 0U || options->max_harmonics > 64U ||
        options->bins_per_octave == 0U ||
        options->bins_per_octave > 96U || options->fit_passes == 0U ||
        options->fit_passes > 32U || options->max_observations == 0U ||
        options->max_points == 0U ||
        options->max_fit_evaluations == 0U) {
        hwa_set_error(error, error_size,
                      "body-envelope method or limit option is invalid");
        return -1;
    }
    span = log2(options->max_frequency_hz /
                options->min_frequency_hz);
    points = floor(span * (double)options->bins_per_octave) + 1.0;
    if (!isfinite(points) || points < 1.0 || points > (double)SIZE_MAX ||
        points > (double)options->max_points) {
        hwa_set_error(error, error_size,
                      "body-envelope point limit exceeded");
        return -1;
    }
    *point_count = (size_t)points;
    return 0;
}

static int hwa_body_frame_usable(const HWAFrameMetrics *frame,
                                 const HWABodyEnvelopeOptions *options)
{
    return frame != NULL && frame->pitch_valid != 0 &&
           frame->spectrum_valid != 0 && isfinite(frame->pitch_hz) &&
           frame->pitch_hz > 0.0 && isfinite(frame->pitch_confidence) &&
           frame->pitch_confidence >= options->min_pitch_confidence &&
           isfinite(frame->rms_dbfs) &&
           frame->rms_dbfs > options->analysis.silence_threshold_dbfs;
}

static size_t hwa_body_frequency_point(double frequency,
                                       const HWABodyEnvelopeOptions *options,
                                       size_t point_count)
{
    double point = floor(log2(frequency / options->min_frequency_hz) *
                         (double)options->bins_per_octave + 0.5);

    if (point <= 0.0) return 0U;
    if (point >= (double)(point_count - 1U)) return point_count - 1U;
    return (size_t)point;
}

static unsigned hwa_body_pitch_cell(double pitch)
{
    double cell = floor(24.0 * log2(pitch / 20.0) + 0.5);

    if (!isfinite(cell) || cell <= 0.0) return 0U;
    if (cell >= (double)(HWA_BODY_PITCH_CELLS - 1U)) {
        return HWA_BODY_PITCH_CELLS - 1U;
    }
    return (unsigned)cell;
}

static double hwa_body_peak_level(const HWAAnalysis *analysis,
                                  size_t frame,
                                  double frequency)
{
    double bin_hz = (double)analysis->format.sample_rate_hz /
                    (double)analysis->options.frame_size;
    double exact_bin = frequency / bin_hz;
    size_t center = (size_t)floor(exact_bin + 0.5);
    size_t first = center > 1U ? center - 1U : center;
    size_t last = center + 1U < analysis->spectrum_bins
                      ? center + 1U
                      : center;
    size_t bin;
    double peak = -300.0;

    for (bin = first; bin <= last; ++bin) {
        double value = analysis->spectrogram_db[
            frame * analysis->spectrum_bins + bin];
        if (isfinite(value) && value > peak) peak = value;
    }
    return peak;
}

static double hwa_body_robust_weight(double base_weight, double residual)
{
    double absolute = fabs(residual);

    if (absolute > HWA_BODY_HUBER_DB) {
        return base_weight * HWA_BODY_HUBER_DB / absolute;
    }
    return base_weight;
}

static int hwa_body_analysis_work(const HWAAnalysis *analysis,
                                  uint64_t *bytes)
{
    uint64_t total = sizeof(*analysis);
    uint64_t value;

    if (analysis == NULL || bytes == NULL) return -1;
    if (analysis->path != NULL &&
        hwa_body_u64_add(total, (uint64_t)strlen(analysis->path) + 1U,
                         &total) != 0) {
        return -1;
    }
    if (hwa_body_u64_multiply((uint64_t)analysis->analyzed_channels,
                              sizeof(*analysis->channels), &value) != 0 ||
        hwa_body_u64_add(total, value, &total) != 0 ||
        hwa_body_u64_multiply((uint64_t)analysis->track_count,
                              sizeof(*analysis->tracks), &value) != 0 ||
        hwa_body_u64_add(total, value, &total) != 0 ||
        hwa_body_u64_multiply((uint64_t)analysis->track_count,
                              (uint64_t)analysis->spectrum_bins, &value) != 0 ||
        hwa_body_u64_multiply(value, sizeof(*analysis->spectrogram_db),
                              &value) != 0 ||
        hwa_body_u64_add(total, value, &total) != 0) {
        return -1;
    }
    *bytes = total;
    return 0;
}

static int hwa_body_fit_work(const HWAAnalysis *analysis,
                             const HWABodyEnvelopeOptions *options,
                             size_t point_count,
                             size_t observation_count,
                             uint64_t outer_retained_work_bytes,
                             uint64_t *bytes)
{
    uint64_t total;
    uint64_t value;
    uint64_t frame_arrays;
    uint64_t harmonic_arrays;
    uint64_t point_arrays;

    if (hwa_body_analysis_work(analysis, &total) != 0 ||
        hwa_body_u64_add(total, outer_retained_work_bytes, &total) != 0 ||
        hwa_body_u64_multiply((uint64_t)observation_count,
                              sizeof(HWABodyObservation), &value) != 0 ||
        hwa_body_u64_add(total, value, &total) != 0 ||
        hwa_body_u64_multiply((uint64_t)analysis->track_count,
                              3U * sizeof(double), &frame_arrays) != 0 ||
        hwa_body_u64_add(total, frame_arrays, &total) != 0 ||
        hwa_body_u64_multiply((uint64_t)(options->max_harmonics + 1U),
                              4U * sizeof(double), &harmonic_arrays) != 0 ||
        hwa_body_u64_add(total, harmonic_arrays, &total) != 0 ||
        hwa_body_u64_multiply((uint64_t)point_count,
                              7U * sizeof(double) + sizeof(uint64_t) +
                                  sizeof(uint64_t),
                              &point_arrays) != 0 ||
        hwa_body_u64_add(total, point_arrays, &total) != 0 ||
        hwa_body_u64_multiply((uint64_t)point_count,
                              HWA_BODY_PITCH_CELLS, &value) != 0 ||
        hwa_body_u64_add(total, value, &total) != 0 ||
        hwa_body_u64_multiply((uint64_t)point_count,
                              sizeof(HWABodyEnvelopePoint), &value) != 0 ||
        hwa_body_u64_add(total, value, &total) != 0) {
        return -1;
    }
    *bytes = total;
    return 0;
}

static int hwa_body_count_observations(
    const HWAAnalysis *analysis,
    const HWABodyEnvelopeOptions *options,
    size_t *observation_count,
    uint64_t *frames_used,
    uint64_t *frames_rejected,
    double *pitch_min,
    double *pitch_max,
    char *error,
    size_t error_size)
{
    size_t frame;
    size_t count = 0U;

    *frames_used = 0U;
    *frames_rejected = 0U;
    *pitch_min = 0.0;
    *pitch_max = 0.0;
    for (frame = 0U; frame < analysis->track_count; ++frame) {
        const HWAFrameMetrics *track = &analysis->tracks[frame];
        size_t harmonic;
        size_t frame_count = 0U;

        if (!hwa_body_frame_usable(track, options)) {
            (*frames_rejected)++;
            continue;
        }
        for (harmonic = 1U; harmonic <= options->max_harmonics; ++harmonic) {
            double frequency = track->pitch_hz * (double)harmonic;
            double nyquist = (double)analysis->format.sample_rate_hz * 0.5;

            if (frequency > options->max_frequency_hz ||
                frequency >= nyquist) {
                break;
            }
            if (frequency < options->min_frequency_hz) continue;
            if (count == options->max_observations) {
                hwa_set_error(error, error_size,
                              "body-envelope observation limit exceeded");
                return -1;
            }
            count++;
            frame_count++;
        }
        if (frame_count != 0U) {
            (*frames_used)++;
            if (*pitch_min == 0.0 || track->pitch_hz < *pitch_min) {
                *pitch_min = track->pitch_hz;
            }
            if (track->pitch_hz > *pitch_max) *pitch_max = track->pitch_hz;
        }
    }
    *observation_count = count;
    return 0;
}

static void hwa_body_center(double *values,
                            const double *weights,
                            size_t count,
                            double *frame_gain,
                            const double *frame_weight,
                            size_t frame_count)
{
    size_t index;
    long double sum = 0.0L;
    long double weight_sum = 0.0L;
    double mean;

    for (index = 0U; index < count; ++index) {
        if (weights[index] > 0.0) {
            sum += (long double)values[index] *
                   (long double)weights[index];
            weight_sum += (long double)weights[index];
        }
    }
    if (weight_sum <= 0.0L) return;
    mean = (double)(sum / weight_sum);
    for (index = 0U; index < count; ++index) values[index] -= mean;
    for (index = 0U; index < frame_count; ++index) {
        if (frame_weight[index] > 0.0) frame_gain[index] += mean;
    }
}

static void hwa_body_remove_log_trend(double *values,
                                      const double *weights,
                                      size_t count,
                                      size_t bins_per_octave)
{
    size_t index;
    long double weight_sum = 0.0L;
    long double x_sum = 0.0L;
    long double y_sum = 0.0L;
    long double xx_sum = 0.0L;
    long double xy_sum = 0.0L;
    long double denominator;
    double intercept;
    double slope;

    for (index = 0U; index < count; ++index) {
        if (weights[index] > 0.0) {
            long double weight = (long double)weights[index];
            long double x = (long double)index /
                            (long double)bins_per_octave;
            long double y = (long double)values[index];

            weight_sum += weight;
            x_sum += weight * x;
            y_sum += weight * y;
            xx_sum += weight * x * x;
            xy_sum += weight * x * y;
        }
    }
    denominator = weight_sum * xx_sum - x_sum * x_sum;
    if (weight_sum <= 0.0L || fabsl(denominator) <= LDBL_EPSILON) return;
    slope = (double)((weight_sum * xy_sum - x_sum * y_sum) /
                     denominator);
    intercept = (double)((y_sum - (long double)slope * x_sum) /
                         weight_sum);
    for (index = 0U; index < count; ++index) {
        double x = (double)index / (double)bins_per_octave;
        values[index] -= intercept + slope * x;
    }
}

int hwa_body_envelope_fit_analysis(
    const HWAAnalysis *analysis,
    const HWABodyEnvelopeOptions *options,
    uint64_t outer_retained_work_bytes,
    HWABodyEnvelopeEstimate *estimate,
    uint64_t *fit_evaluations,
    char *error,
    size_t error_size)
{
    HWABodyObservation *observations = NULL;
    double *frame_gain = NULL;
    double *frame_sum = NULL;
    double *frame_weight = NULL;
    double *harmonic_curve = NULL;
    double *harmonic_sum = NULL;
    double *harmonic_weight = NULL;
    double *harmonic_support = NULL;
    double *body_curve = NULL;
    double *body_smooth = NULL;
    double *body_sum = NULL;
    double *body_weight = NULL;
    double *residual_sum = NULL;
    uint64_t *point_observations = NULL;
    uint64_t *harmonic_masks = NULL;
    unsigned char *pitch_seen = NULL;
    size_t point_count = 0U;
    size_t observation_count = 0U;
    size_t observation_bytes = 0U;
    size_t point_pitch_bytes = 0U;
    size_t index;
    size_t pass;
    uint64_t frames_used;
    uint64_t frames_rejected;
    uint64_t maximum_scan;
    uint64_t evaluation_factor;
    uint64_t evaluation_need;
    uint64_t peak_work;
    double pitch_min;
    double pitch_max;
    int result = -1;

    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (estimate != NULL) memset(estimate, 0, sizeof(*estimate));
    if (fit_evaluations != NULL) *fit_evaluations = 0U;
    if (analysis == NULL || options == NULL || estimate == NULL ||
        fit_evaluations == NULL || analysis->tracks == NULL ||
        analysis->spectrogram_db == NULL || analysis->track_count == 0U ||
        analysis->spectrum_bins < 3U ||
        hwa_body_options_validate(options, &point_count,
                                  error, error_size) != 0) {
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "analysis lacks body-envelope frame data");
        }
        return -1;
    }
    if (hwa_body_u64_multiply((uint64_t)analysis->track_count,
                              (uint64_t)options->max_harmonics,
                              &maximum_scan) != 0 ||
        maximum_scan > options->max_fit_evaluations) {
        hwa_set_error(error, error_size,
                      "body-envelope fit evaluation limit exceeded");
        return -1;
    }
    if (hwa_body_count_observations(
            analysis, options, &observation_count, &frames_used,
            &frames_rejected, &pitch_min, &pitch_max,
            error, error_size) != 0) {
        return -1;
    }
    if (hwa_body_u64_multiply((uint64_t)options->fit_passes, 3U,
                              &evaluation_factor) != 0 ||
        hwa_body_u64_add(evaluation_factor, 4U,
                         &evaluation_factor) != 0 ||
        hwa_body_u64_multiply((uint64_t)observation_count,
                              evaluation_factor, &evaluation_need) != 0 ||
        hwa_body_u64_add(maximum_scan, evaluation_need,
                         &evaluation_need) != 0 ||
        evaluation_need > options->max_fit_evaluations) {
        hwa_set_error(error, error_size,
                      "body-envelope fit evaluation limit exceeded");
        return -1;
    }
    if (hwa_body_fit_work(analysis, options, point_count,
                          observation_count, outer_retained_work_bytes,
                          &peak_work) != 0 ||
        peak_work > options->analysis.max_work_bytes) {
        hwa_set_error(error, error_size,
                      "body-envelope work limit exceeded");
        return -1;
    }
    if (hwa_body_size_multiply(observation_count, sizeof(*observations),
                               &observation_bytes) != 0 ||
        hwa_body_size_multiply(point_count, HWA_BODY_PITCH_CELLS,
                               &point_pitch_bytes) != 0) {
        hwa_set_error(error, error_size,
                      "body-envelope allocation size overflow");
        return -1;
    }
    observations = (HWABodyObservation *)malloc(observation_bytes);
    frame_gain = (double *)calloc(analysis->track_count, sizeof(*frame_gain));
    frame_sum = (double *)calloc(analysis->track_count, sizeof(*frame_sum));
    frame_weight =
        (double *)calloc(analysis->track_count, sizeof(*frame_weight));
    harmonic_curve = (double *)calloc(options->max_harmonics + 1U,
                                      sizeof(*harmonic_curve));
    harmonic_sum = (double *)calloc(options->max_harmonics + 1U,
                                    sizeof(*harmonic_sum));
    harmonic_weight = (double *)calloc(options->max_harmonics + 1U,
                                       sizeof(*harmonic_weight));
    harmonic_support = (double *)calloc(options->max_harmonics + 1U,
                                        sizeof(*harmonic_support));
    body_curve = (double *)calloc(point_count, sizeof(*body_curve));
    body_smooth = (double *)calloc(point_count, sizeof(*body_smooth));
    body_sum = (double *)calloc(point_count, sizeof(*body_sum));
    body_weight = (double *)calloc(point_count, sizeof(*body_weight));
    residual_sum = (double *)calloc(point_count, sizeof(*residual_sum));
    point_observations =
        (uint64_t *)calloc(point_count, sizeof(*point_observations));
    harmonic_masks =
        (uint64_t *)calloc(point_count, sizeof(*harmonic_masks));
    pitch_seen = (unsigned char *)calloc(point_pitch_bytes, 1U);
    estimate->points = (HWABodyEnvelopePoint *)calloc(
        point_count, sizeof(*estimate->points));
    estimate->path = hwa_body_copy_string(
        analysis->path != NULL ? analysis->path : "<memory>");
    if ((observation_count != 0U && observations == NULL) ||
        frame_gain == NULL || frame_sum == NULL || frame_weight == NULL ||
        harmonic_curve == NULL || harmonic_sum == NULL ||
        harmonic_weight == NULL || harmonic_support == NULL ||
        body_curve == NULL || body_smooth == NULL || body_sum == NULL ||
        body_weight == NULL || residual_sum == NULL ||
        point_observations == NULL || harmonic_masks == NULL ||
        pitch_seen == NULL || estimate->points == NULL ||
        estimate->path == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for body-envelope fit");
        goto cleanup;
    }
    index = 0U;
    {
        size_t frame;
        for (frame = 0U; frame < analysis->track_count; ++frame) {
            const HWAFrameMetrics *track = &analysis->tracks[frame];
            size_t harmonic;

            if (!hwa_body_frame_usable(track, options)) continue;
            for (harmonic = 1U;
                 harmonic <= options->max_harmonics; ++harmonic) {
                double frequency = track->pitch_hz * (double)harmonic;
                double nyquist =
                    (double)analysis->format.sample_rate_hz * 0.5;
                double onset;
                double weight;

                if (frequency > options->max_frequency_hz ||
                    frequency >= nyquist) {
                    break;
                }
                if (frequency < options->min_frequency_hz) continue;
                onset = isfinite(track->combined_onset_strength)
                            ? fmax(track->combined_onset_strength, 0.0)
                            : 0.0;
                weight = track->pitch_confidence / (1.0 + 4.0 * onset);
                if (weight < 0.05) weight = 0.05;
                observations[index].frame = frame;
                observations[index].point = hwa_body_frequency_point(
                    frequency, options, point_count);
                observations[index].harmonic = harmonic;
                observations[index].pitch_cell =
                    hwa_body_pitch_cell(track->pitch_hz);
                observations[index].level_db =
                    hwa_body_peak_level(analysis, frame, frequency);
                observations[index].weight = weight;
                index++;
            }
        }
    }
    if (index != observation_count) {
        hwa_set_error(error, error_size,
                      "body-envelope observation count changed");
        goto cleanup;
    }
    for (index = 0U; index < observation_count; ++index) {
        const HWABodyObservation *observation = &observations[index];
        frame_sum[observation->frame] +=
            observation->level_db * observation->weight;
        frame_weight[observation->frame] += observation->weight;
    }
    for (index = 0U; index < analysis->track_count; ++index) {
        if (frame_weight[index] > 0.0) {
            frame_gain[index] = frame_sum[index] / frame_weight[index];
        }
    }
    for (pass = 0U; pass < options->fit_passes; ++pass) {
        memset(frame_sum, 0,
               analysis->track_count * sizeof(*frame_sum));
        memset(frame_weight, 0,
               analysis->track_count * sizeof(*frame_weight));
        for (index = 0U; index < observation_count; ++index) {
            const HWABodyObservation *observation = &observations[index];
            double predicted = frame_gain[observation->frame] +
                               harmonic_curve[observation->harmonic] +
                               body_curve[observation->point];
            double weight = hwa_body_robust_weight(
                observation->weight, observation->level_db - predicted);
            frame_sum[observation->frame] +=
                weight * (observation->level_db -
                          harmonic_curve[observation->harmonic] -
                          body_curve[observation->point]);
            frame_weight[observation->frame] += weight;
        }
        for (index = 0U; index < analysis->track_count; ++index) {
            if (frame_weight[index] > 0.0) {
                frame_gain[index] = frame_sum[index] / frame_weight[index];
            }
        }
        memset(harmonic_sum, 0,
               (options->max_harmonics + 1U) * sizeof(*harmonic_sum));
        memset(harmonic_weight, 0,
               (options->max_harmonics + 1U) * sizeof(*harmonic_weight));
        for (index = 0U; index < observation_count; ++index) {
            const HWABodyObservation *observation = &observations[index];
            double predicted = frame_gain[observation->frame] +
                               harmonic_curve[observation->harmonic] +
                               body_curve[observation->point];
            double weight = hwa_body_robust_weight(
                observation->weight, observation->level_db - predicted);
            harmonic_sum[observation->harmonic] +=
                weight * (observation->level_db -
                          frame_gain[observation->frame] -
                          body_curve[observation->point]);
            harmonic_weight[observation->harmonic] += weight;
        }
        for (index = 1U; index <= options->max_harmonics; ++index) {
            if (harmonic_weight[index] > 0.0) {
                harmonic_curve[index] =
                    harmonic_sum[index] / harmonic_weight[index];
            }
        }
        hwa_body_center(harmonic_curve, harmonic_weight,
                        options->max_harmonics + 1U, frame_gain,
                        frame_weight, analysis->track_count);
        memset(body_sum, 0, point_count * sizeof(*body_sum));
        memset(body_weight, 0, point_count * sizeof(*body_weight));
        for (index = 0U; index < observation_count; ++index) {
            const HWABodyObservation *observation = &observations[index];
            double predicted = frame_gain[observation->frame] +
                               harmonic_curve[observation->harmonic] +
                               body_curve[observation->point];
            double weight = hwa_body_robust_weight(
                observation->weight, observation->level_db - predicted);
            body_sum[observation->point] +=
                weight * (observation->level_db -
                          frame_gain[observation->frame] -
                          harmonic_curve[observation->harmonic]);
            body_weight[observation->point] += weight;
        }
        for (index = 0U; index < point_count; ++index) {
            if (body_weight[index] > 0.0) {
                body_curve[index] = body_sum[index] / body_weight[index];
            }
        }
        for (index = 0U; index < point_count; ++index) {
            double sum = 2.0 * body_curve[index];
            double weight = 2.0;
            if (index > 0U && body_weight[index - 1U] > 0.0) {
                sum += body_curve[index - 1U];
                weight += 1.0;
            }
            if (index + 1U < point_count &&
                body_weight[index + 1U] > 0.0) {
                sum += body_curve[index + 1U];
                weight += 1.0;
            }
            body_smooth[index] = sum / weight;
        }
        memcpy(body_curve, body_smooth,
               point_count * sizeof(*body_curve));
        hwa_body_center(body_curve, body_weight, point_count,
                        frame_gain, frame_weight, analysis->track_count);
    }
    memset(harmonic_support, 0,
           (options->max_harmonics + 1U) * sizeof(*harmonic_support));
    for (index = 0U; index < observation_count; ++index) {
        const HWABodyObservation *observation = &observations[index];
        double residual = observation->level_db -
                          frame_gain[observation->frame] -
                          harmonic_curve[observation->harmonic] -
                          body_curve[observation->point];
        size_t seen_index =
            observation->point * HWA_BODY_PITCH_CELLS +
            (size_t)observation->pitch_cell;

        residual_sum[observation->point] += fabs(residual);
        point_observations[observation->point]++;
        pitch_seen[seen_index] = 1U;
        harmonic_masks[observation->point] |=
            UINT64_C(1) << (observation->harmonic - 1U);
        harmonic_support[observation->harmonic] += observation->weight;
    }
    hwa_body_remove_log_trend(body_curve, body_weight, point_count,
                              options->bins_per_octave);
    {
        size_t valid_count = 0U;
        long double confidence_sum = 0.0L;

        for (index = 0U; index < point_count; ++index) {
            HWABodyEnvelopePoint *point = &estimate->points[index];
            size_t cell;
            size_t pitch_cells = 0U;
            size_t harmonics = 0U;
            uint64_t mask = harmonic_masks[index];
            double support_factor;
            double pitch_factor;
            double harmonic_factor;
            double residual_factor;

            point->frequency_hz = options->min_frequency_hz *
                pow(2.0, (double)index /
                          (double)options->bins_per_octave);
            point->relative_db = body_curve[index];
            point->observation_count = point_observations[index];
            for (cell = 0U; cell < HWA_BODY_PITCH_CELLS; ++cell) {
                if (pitch_seen[index * HWA_BODY_PITCH_CELLS + cell] != 0U) {
                    pitch_cells++;
                }
            }
            while (mask != 0U) {
                harmonics += (size_t)(mask & UINT64_C(1));
                mask >>= 1U;
            }
            point->pitch_cell_count = pitch_cells;
            point->harmonic_count = harmonics;
            if (point->observation_count != 0U) {
                point->residual_spread_db =
                    residual_sum[index] /
                    (double)point->observation_count;
            }
            if (point->observation_count < 6U) {
                point->quality_flags |= HWA_BODY_ENVELOPE_LOW_POINT_SUPPORT;
            }
            if (pitch_cells < 2U) {
                point->quality_flags |= HWA_BODY_ENVELOPE_LOW_PITCH_SPREAD;
            }
            if (harmonics < 2U) {
                point->quality_flags |= HWA_BODY_ENVELOPE_LOW_PARTIAL_SPREAD;
            }
            if (point->residual_spread_db > 12.0) {
                point->quality_flags |= HWA_BODY_ENVELOPE_HIGH_RESIDUAL;
            }
            support_factor = fmin((double)point->observation_count / 20.0, 1.0);
            pitch_factor = fmin((double)pitch_cells / 4.0, 1.0);
            harmonic_factor = fmin((double)harmonics / 4.0, 1.0);
            residual_factor =
                exp(-point->residual_spread_db / 12.0);
            point->confidence = support_factor * pitch_factor *
                                harmonic_factor * residual_factor;
            point->valid = point->observation_count >= 6U &&
                           pitch_cells >= 2U && harmonics >= 2U;
            if (point->valid) {
                valid_count++;
                confidence_sum += (long double)point->confidence;
            }
        }
        if (valid_count >= 8U) {
            estimate->status = HWA_BODY_ENVELOPE_VALID;
        } else if (valid_count != 0U || observation_count != 0U) {
            estimate->status = HWA_BODY_ENVELOPE_LOW_SUPPORT;
        } else {
            estimate->status = HWA_BODY_ENVELOPE_NO_SUPPORT;
        }
        estimate->confidence = valid_count != 0U
                                   ? (double)(confidence_sum /
                                              (long double)valid_count)
                                   : 0.0;
    }
    estimate->pitch_min_hz = pitch_min;
    estimate->pitch_max_hz = pitch_max;
    estimate->frames_seen = (uint64_t)analysis->track_count;
    estimate->frames_used = frames_used;
    estimate->frames_rejected_pitch = frames_rejected;
    estimate->observation_count = observation_count;
    estimate->point_count = point_count;
    *fit_evaluations = evaluation_need;
    result = 0;

cleanup:
    free(observations);
    free(frame_gain);
    free(frame_sum);
    free(frame_weight);
    free(harmonic_curve);
    free(harmonic_sum);
    free(harmonic_weight);
    free(harmonic_support);
    free(body_curve);
    free(body_smooth);
    free(body_sum);
    free(body_weight);
    free(residual_sum);
    free(point_observations);
    free(harmonic_masks);
    free(pitch_seen);
    if (result != 0) hwa_body_estimate_free(estimate);
    return result;
}

static uint64_t hwa_body_estimate_retained(
    const HWABodyEnvelopeEstimate *estimate)
{
    uint64_t retained = sizeof(*estimate);
    uint64_t points;

    if (estimate->path != NULL) {
        retained += (uint64_t)strlen(estimate->path) + 1U;
    }
    if (hwa_body_u64_multiply((uint64_t)estimate->point_count,
                              sizeof(*estimate->points), &points) == 0 &&
        retained <= UINT64_MAX - points) {
        retained += points;
    } else {
        return UINT64_MAX;
    }
    return retained;
}

static int hwa_body_compare(HWABodyEnvelopeResult *result,
                            char *error,
                            size_t error_size)
{
    size_t index;
    size_t common = 0U;
    long double delta_sum = 0.0L;
    long double delta_weight = 0.0L;
    long double reference_sum = 0.0L;
    long double model_sum = 0.0L;
    long double confidence_sum = 0.0L;
    double delta_mean;

    if (result->reference.point_count != result->model.point_count) {
        hwa_set_error(error, error_size,
                      "body-envelope grids do not match");
        return -1;
    }
    result->gap_count = result->reference.point_count;
    result->gaps = (HWABodyEnvelopeGap *)calloc(
        result->gap_count, sizeof(*result->gaps));
    if (result->gaps == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for body-envelope comparison");
        return -1;
    }
    for (index = 0U; index < result->gap_count; ++index) {
        const HWABodyEnvelopePoint *reference =
            &result->reference.points[index];
        const HWABodyEnvelopePoint *model = &result->model.points[index];
        double confidence = fmin(reference->confidence, model->confidence);

        result->gaps[index].frequency_hz = reference->frequency_hz;
        if (reference->valid && model->valid && confidence > 0.0) {
            double delta = model->relative_db - reference->relative_db;
            delta_sum += (long double)delta * (long double)confidence;
            delta_weight += (long double)confidence;
            reference_sum += (long double)reference->relative_db;
            model_sum += (long double)model->relative_db;
            common++;
        }
    }
    if (common < 3U || delta_weight <= 0.0L) return 0;
    delta_mean = (double)(delta_sum / delta_weight);
    {
        double reference_mean = (double)(reference_sum / (long double)common);
        double model_mean = (double)(model_sum / (long double)common);
        long double square_sum = 0.0L;
        long double covariance = 0.0L;
        long double reference_square = 0.0L;
        long double model_square = 0.0L;

        for (index = 0U; index < result->gap_count; ++index) {
            const HWABodyEnvelopePoint *reference =
                &result->reference.points[index];
            const HWABodyEnvelopePoint *model = &result->model.points[index];
            double confidence = fmin(reference->confidence,
                                     model->confidence);
            if (reference->valid && model->valid && confidence > 0.0) {
                double delta = model->relative_db - reference->relative_db -
                               delta_mean;
                double reference_centered =
                    reference->relative_db - reference_mean;
                double model_centered = model->relative_db - model_mean;

                result->gaps[index].model_minus_reference_db = delta;
                result->gaps[index].confidence = confidence;
                result->gaps[index].valid = 1;
                square_sum += (long double)delta * (long double)delta;
                covariance += (long double)reference_centered *
                              (long double)model_centered;
                reference_square += (long double)reference_centered *
                                    (long double)reference_centered;
                model_square += (long double)model_centered *
                                (long double)model_centered;
                confidence_sum += (long double)confidence;
            }
        }
        result->shape_rmse_db = sqrt((double)(square_sum /
                                               (long double)common));
        if (reference_square > 0.0L && model_square > 0.0L) {
            result->shape_correlation =
                (double)(covariance /
                         sqrtl(reference_square * model_square));
        }
        result->comparison_confidence =
            (double)(confidence_sum / (long double)common);
        result->comparison_valid = 1;
    }
    return 0;
}

int hwa_body_envelope_wavs(const char *reference_path,
                           const char *model_path,
                           const HWABodyEnvelopeOptions *provided_options,
                           HWABodyEnvelopeResult *result,
                           char *error,
                           size_t error_size)
{
    HWABodyEnvelopeOptions options;
    HWAAnalysisOptions analysis_options;
    HWAAnalysis analysis;
    uint64_t reference_evaluations = 0U;
    uint64_t model_evaluations = 0U;
    uint64_t reference_retained;
    uint64_t gaps_retained = 0U;
    size_t point_count;

    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (result != NULL) memset(result, 0, sizeof(*result));
    if (reference_path == NULL || result == NULL) {
        hwa_set_error(error, error_size,
                      "invalid body-envelope input");
        return -1;
    }
    if (provided_options == NULL) {
        hwa_body_envelope_options_default(&options);
    } else {
        options = *provided_options;
    }
    if (hwa_body_options_validate(&options, &point_count,
                                  error, error_size) != 0) {
        return -1;
    }
    (void)point_count;
    result->options = options;
    analysis_options = options.analysis;
    analysis_options.collect_tracks = 1;
    analysis_options.collect_spectrogram = 1;
    memset(&analysis, 0, sizeof(analysis));
    if (hwa_analyze_wav_with_options(reference_path, &analysis_options,
                                     &analysis, error, error_size) != 0) {
        goto failure;
    }
    if (hwa_body_envelope_fit_analysis(
            &analysis, &options, 0U, &result->reference,
            &reference_evaluations, error, error_size) != 0) {
        hwa_analysis_free(&analysis);
        goto failure;
    }
    hwa_analysis_free(&analysis);
    reference_retained = hwa_body_estimate_retained(&result->reference);
    if (reference_retained == UINT64_MAX) {
        hwa_set_error(error, error_size,
                      "body-envelope retained size overflow");
        goto failure;
    }
    if (model_path != NULL) {
        HWABodyEnvelopeOptions model_options = options;

        if (reference_retained >= options.analysis.max_work_bytes) {
            hwa_set_error(error, error_size,
                          "body-envelope work limit exceeded before model");
            goto failure;
        }
        analysis_options = options.analysis;
        analysis_options.collect_tracks = 1;
        analysis_options.collect_spectrogram = 1;
        analysis_options.max_work_bytes =
            options.analysis.max_work_bytes - reference_retained;
        if (reference_evaluations >= options.max_fit_evaluations) {
            hwa_set_error(error, error_size,
                          "body-envelope fit evaluation limit exhausted");
            goto failure;
        }
        model_options.max_fit_evaluations =
            options.max_fit_evaluations - reference_evaluations;
        memset(&analysis, 0, sizeof(analysis));
        if (hwa_analyze_wav_with_options(model_path, &analysis_options,
                                         &analysis, error, error_size) != 0) {
            goto failure;
        }
        if (hwa_body_envelope_fit_analysis(
                &analysis, &model_options, reference_retained,
                &result->model, &model_evaluations,
                error, error_size) != 0) {
            hwa_analysis_free(&analysis);
            goto failure;
        }
        hwa_analysis_free(&analysis);
        result->model_present = 1;
        if (hwa_body_compare(result, error, error_size) != 0) goto failure;
    }
    if (hwa_body_u64_add(reference_evaluations, model_evaluations,
                         &result->fit_evaluations) != 0 ||
        result->fit_evaluations > options.max_fit_evaluations) {
        hwa_set_error(error, error_size,
                      "body-envelope total fit evaluation limit exceeded");
        goto failure;
    }
    result->retained_work_bytes = sizeof(*result);
    if (hwa_body_u64_add(result->retained_work_bytes, reference_retained,
                         &result->retained_work_bytes) != 0 ||
        hwa_body_u64_add(result->retained_work_bytes,
                         hwa_body_estimate_retained(&result->model),
                         &result->retained_work_bytes) != 0 ||
        hwa_body_u64_multiply((uint64_t)result->gap_count,
                              sizeof(*result->gaps), &gaps_retained) != 0 ||
        hwa_body_u64_add(result->retained_work_bytes, gaps_retained,
                         &result->retained_work_bytes) != 0 ||
        result->retained_work_bytes > options.analysis.max_work_bytes) {
        hwa_set_error(error, error_size,
                      "body-envelope retained work limit exceeded");
        goto failure;
    }
    return 0;

failure:
    hwa_body_envelope_result_free(result);
    return -1;
}

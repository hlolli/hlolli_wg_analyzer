#include "internal.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HWA_NOTE_ALL_METRICS                                             \
    ((uint32_t)HWA_ISOLATED_NOTE_PITCH |                                 \
     (uint32_t)HWA_ISOLATED_NOTE_PASSIVE_DECAY)
#define HWA_NOTE_MIN_PITCH_CONFIDENCE 0.65
#define HWA_NOTE_MIN_PITCH_COVERAGE 0.35

typedef struct HWANoteWork {
    uint64_t live;
    uint64_t peak;
    uint64_t evaluations;
    uint64_t max_work;
    uint64_t max_evaluations;
} HWANoteWork;

static int hwa_note_work_add(HWANoteWork *work,
                             uint64_t bytes,
                             char *error,
                             size_t error_size)
{
    if (work->live > work->max_work || bytes > work->max_work - work->live) {
        hwa_set_error(error, error_size,
                      "isolated-note work limit exceeded");
        return -1;
    }
    work->live += bytes;
    if (work->live > work->peak) work->peak = work->live;
    return 0;
}

static void hwa_note_work_remove(HWANoteWork *work, uint64_t bytes)
{
    if (bytes <= work->live) work->live -= bytes;
}

static int hwa_note_evaluate(HWANoteWork *work,
                             uint64_t count,
                             char *error,
                             size_t error_size)
{
    if (work->evaluations > work->max_evaluations ||
        count > work->max_evaluations - work->evaluations) {
        hwa_set_error(error, error_size,
                      "isolated-note evaluation limit exceeded");
        return -1;
    }
    work->evaluations += count;
    return 0;
}

static int hwa_note_size_bytes(size_t count,
                               size_t item_size,
                               uint64_t *bytes)
{
    if (item_size != 0U && count > SIZE_MAX / item_size) return -1;
    *bytes = (uint64_t)(count * item_size);
    return 0;
}

static char *hwa_note_copy_path(const char *path)
{
    size_t length;
    char *copy;

    if (path == NULL) return NULL;
    length = strlen(path);
    if (length == SIZE_MAX) return NULL;
    copy = (char *)malloc(length + 1U);
    if (copy != NULL) memcpy(copy, path, length + 1U);
    return copy;
}

void hwa_isolated_note_options_default(HWAIsolatedNoteOptions *options)
{
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    options->metric_mask = HWA_NOTE_ALL_METRICS;
    options->decode_block_frames = 4096U;
    options->max_input_bytes = UINT64_C(17179869184);
    options->max_input_frames = UINT64_C(2000000000);
    options->max_work_bytes = UINT64_C(268435456);
    options->max_evaluations = UINT64_C(250000000);
}

void hwa_isolated_note_result_free(HWAIsolatedNoteResult *result)
{
    if (result == NULL) return;
    free(result->path);
    memset(result, 0, sizeof(*result));
}

static int hwa_note_options_validate(const HWAIsolatedNoteOptions *options,
                                     char *error,
                                     size_t error_size)
{
    if (options == NULL || !isfinite(options->expected_hz) ||
        options->expected_hz < 20.0 || options->expected_hz > 5000.0) {
        hwa_set_error(error, error_size,
                      "isolated-note expected frequency is invalid");
        return -1;
    }
    if (options->metric_mask == 0U ||
        (options->metric_mask & ~HWA_NOTE_ALL_METRICS) != 0U) {
        hwa_set_error(error, error_size,
                      "isolated-note metric mask is invalid");
        return -1;
    }
    if (options->decode_block_frames == 0U ||
        options->decode_block_frames > 1048576U ||
        options->max_input_bytes == 0U ||
        options->max_input_frames == 0U ||
        options->max_work_bytes == 0U || options->max_evaluations == 0U) {
        hwa_set_error(error, error_size,
                      "isolated-note work limit is invalid");
        return -1;
    }
    return 0;
}

static int hwa_note_decode(HWAWavReader *reader,
                           const HWAIsolatedNoteOptions *options,
                           double **samples,
                           HWANoteWork *work,
                           uint64_t *sample_bytes,
                           char *error,
                           size_t error_size)
{
    unsigned char *buffer = NULL;
    uint64_t buffer_bytes = 0U;
    size_t frame_offset = 0U;
    int result = -1;

    if (reader->format.encoding != HWA_ENCODING_PCM ||
        (reader->format.bits_per_sample != 16U &&
         reader->format.bits_per_sample != 24U) ||
        (reader->format.channels != 1U && reader->format.channels != 2U)) {
        hwa_set_error(error, error_size,
                      "isolated-note needs mono or stereo 16-bit or 24-bit PCM WAVE audio");
        return -1;
    }
    if (reader->format.frames == 0U ||
        reader->format.frames > options->max_input_frames ||
        reader->format.frames > (uint64_t)SIZE_MAX) {
        hwa_set_error(error, error_size,
                      "isolated-note input frame limit exceeded");
        return -1;
    }
    if (hwa_note_size_bytes((size_t)reader->format.frames, sizeof(double),
                            sample_bytes) != 0 ||
        hwa_note_size_bytes(options->decode_block_frames,
                            reader->format.block_align,
                            &buffer_bytes) != 0 ||
        hwa_note_work_add(work, *sample_bytes, error, error_size) != 0 ||
        hwa_note_work_add(work, buffer_bytes, error, error_size) != 0) {
        return -1;
    }
    *samples = (double *)malloc((size_t)*sample_bytes);
    buffer = (unsigned char *)malloc((size_t)buffer_bytes);
    if (*samples == NULL || buffer == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for isolated-note audio");
        goto cleanup;
    }
    for (;;) {
        size_t frames_read = 0U;
        size_t frame;
        if (hwa_wav_reader_read_frames(reader, buffer,
                                       options->decode_block_frames,
                                       &frames_read, error, error_size) != 0) {
            goto cleanup;
        }
        if (frames_read == 0U) break;
        for (frame = 0U; frame < frames_read; ++frame) {
            size_t channel;
            double mixed = 0.0;
            const unsigned char *frame_bytes =
                buffer + frame * reader->format.block_align;
            for (channel = 0U;
                 channel < (size_t)reader->format.channels; ++channel) {
                int clipped = 0;
                mixed += hwa_wav_decode_sample(
                    reader,
                    frame_bytes + channel * reader->bytes_per_sample,
                    &clipped);
            }
            (*samples)[frame_offset++] =
                mixed / (double)reader->format.channels;
        }
    }
    if ((uint64_t)frame_offset != reader->format.frames) {
        hwa_set_error(error, error_size,
                      "isolated-note decoded frame count changed");
        goto cleanup;
    }
    result = 0;

cleanup:
    free(buffer);
    hwa_note_work_remove(work, buffer_bytes);
    if (result != 0) {
        free(*samples);
        *samples = NULL;
        hwa_note_work_remove(work, *sample_bytes);
        *sample_bytes = 0U;
    }
    return result;
}

static double hwa_note_rms(const double *samples, size_t start, size_t count)
{
    size_t index;
    double sum = 0.0;
    for (index = 0U; index < count; ++index) {
        double value = samples[start + index];
        sum += value * value;
    }
    return sqrt(sum / (double)count);
}

static double hwa_note_correlation(const double *samples,
                                   size_t start,
                                   size_t count,
                                   size_t lag)
{
    size_t index;
    size_t pairs = count - lag;
    double product = 0.0;
    double left_energy = 0.0;
    double right_energy = 0.0;

    for (index = 0U; index < pairs; ++index) {
        double left = samples[start + index];
        double right = samples[start + index + lag];
        product += left * right;
        left_energy += left * left;
        right_energy += right * right;
    }
    if (left_energy <= 1.0e-30 || right_energy <= 1.0e-30) return 0.0;
    return product / sqrt(left_energy * right_energy);
}

static int hwa_note_compare_double(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static double hwa_note_median(double *values, size_t count)
{
    qsort(values, count, sizeof(*values), hwa_note_compare_double);
    if ((count & 1U) != 0U) return values[count / 2U];
    return 0.5 * (values[count / 2U - 1U] + values[count / 2U]);
}

static int hwa_note_pitch(const double *samples,
                          size_t frame_count,
                          uint32_t rate,
                          double expected,
                          HWAIsolatedNoteResult *result,
                          HWANoteWork *work,
                          char *error,
                          size_t error_size)
{
    double ratio = pow(2.0, 100.0 / 1200.0);
    size_t window = (size_t)ceil(6.0 * (double)rate / expected);
    size_t hop;
    size_t first_lag;
    size_t last_lag;
    size_t window_count;
    double *pitches = NULL;
    double *confidences = NULL;
    uint64_t array_bytes = 0U;
    double max_rms = 0.0;
    double active_floor;
    size_t active_count = 0U;
    size_t accepted = 0U;
    size_t first_sample = 0U;
    size_t last_sample = 0U;
    size_t window_index;
    int status = -1;

    if (window < 2048U) window = 2048U;
    if (window > frame_count) {
        result->rejection_mask |= HWA_ISOLATED_NOTE_REJECT_LOW_SUPPORT;
        return 0;
    }
    hop = window / 2U;
    first_lag = (size_t)floor((double)rate / (expected * ratio));
    last_lag = (size_t)ceil((double)rate * ratio / expected);
    if (first_lag < 2U || last_lag + 1U >= window ||
        first_lag >= last_lag) {
        hwa_set_error(error, error_size,
                      "isolated-note pitch bounds are not usable");
        return -1;
    }
    window_count = 1U + (frame_count - window) / hop;
    if (hwa_note_size_bytes(window_count, sizeof(double), &array_bytes) != 0 ||
        array_bytes > UINT64_MAX / 2U ||
        hwa_note_work_add(work, array_bytes * 2U,
                          error, error_size) != 0) {
        return -1;
    }
    pitches = (double *)malloc((size_t)array_bytes);
    confidences = (double *)malloc((size_t)array_bytes);
    if (pitches == NULL || confidences == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for isolated-note pitch");
        goto cleanup;
    }
    for (window_index = 0U; window_index < window_count; ++window_index) {
        size_t start = window_index * hop;
        double rms;
        if (hwa_note_evaluate(work, (uint64_t)window,
                              error, error_size) != 0) {
            goto cleanup;
        }
        rms = hwa_note_rms(samples, start, window);
        if (rms > max_rms) max_rms = rms;
    }
    if (max_rms < 1.0e-5) {
        result->rejection_mask |= HWA_ISOLATED_NOTE_REJECT_SILENCE;
        status = 0;
        goto cleanup;
    }
    active_floor = fmax(1.0e-4, max_rms * pow(10.0, -35.0 / 20.0));
    for (window_index = 0U; window_index < window_count; ++window_index) {
        size_t start = window_index * hop;
        size_t lag;
        size_t best_lag = first_lag;
        double rms;
        double best = -1.0;
        double refined_lag;
        double pitch;
        double cents;

        if (hwa_note_evaluate(work, (uint64_t)window,
                              error, error_size) != 0) {
            goto cleanup;
        }
        rms = hwa_note_rms(samples, start, window);
        if (rms < active_floor) continue;
        active_count++;
        for (lag = first_lag; lag <= last_lag; ++lag) {
            double correlation;
            if (hwa_note_evaluate(work, (uint64_t)(window - lag),
                                  error, error_size) != 0) {
                goto cleanup;
            }
            correlation = hwa_note_correlation(samples, start, window, lag);
            if (correlation > best) {
                best = correlation;
                best_lag = lag;
            }
        }
        if (best_lag == first_lag || best_lag == last_lag) {
            result->rejection_mask |= HWA_ISOLATED_NOTE_REJECT_BOUNDARY;
            continue;
        }
        {
            double left;
            double right;
            double divisor;
            double offset;
            uint64_t refinement_evaluations =
                (uint64_t)(window - (best_lag - 1U)) +
                (uint64_t)(window - (best_lag + 1U));
            if (hwa_note_evaluate(work, refinement_evaluations,
                                  error, error_size) != 0) {
                goto cleanup;
            }
            left = hwa_note_correlation(
                samples, start, window, best_lag - 1U);
            right = hwa_note_correlation(
                samples, start, window, best_lag + 1U);
            divisor = left - 2.0 * best + right;
            offset = fabs(divisor) > 1.0e-12
                         ? 0.5 * (left - right) / divisor : 0.0;
            if (offset < -1.0 || offset > 1.0) offset = 0.0;
            refined_lag = (double)best_lag + offset;
        }
        if (best_lag >= 4U) {
            size_t half_lag = (size_t)floor(refined_lag * 0.5 + 0.5);
            double half;
            if (hwa_note_evaluate(work, (uint64_t)(window - half_lag),
                                  error, error_size) != 0) {
                goto cleanup;
            }
            half = hwa_note_correlation(samples, start, window, half_lag);
            if (half > 0.96 && half >= best - 0.015) {
                result->rejection_mask |= HWA_ISOLATED_NOTE_REJECT_OCTAVE;
                continue;
            }
        }
        pitch = (double)rate / refined_lag;
        cents = 1200.0 * log2(pitch / expected);
        if (best < HWA_NOTE_MIN_PITCH_CONFIDENCE || fabs(cents) > 80.0) {
            result->rejection_mask |= HWA_ISOLATED_NOTE_REJECT_NOISE;
            continue;
        }
        pitches[accepted] = pitch;
        confidences[accepted] = best;
        if (accepted == 0U) first_sample = start;
        last_sample = start + window;
        accepted++;
    }
    result->pitch.window_count = (uint64_t)active_count;
    result->pitch.accepted_window_count = (uint64_t)accepted;
    result->pitch.coverage = active_count == 0U
                                 ? 0.0
                                 : (double)accepted / (double)active_count;
    if (accepted >= 2U) {
        size_t index;
        double confidence_sum = 0.0;
        result->pitch.hz = hwa_note_median(pitches, accepted);
        for (index = 0U; index < accepted; ++index) {
            confidence_sum += confidences[index];
        }
        result->pitch.cents = 1200.0 * log2(result->pitch.hz / expected);
        result->pitch.confidence = confidence_sum / (double)accepted;
        result->pitch.start_sample = (uint64_t)first_sample;
        result->pitch.end_sample = (uint64_t)last_sample;
        if (result->pitch.confidence >= HWA_NOTE_MIN_PITCH_CONFIDENCE &&
            result->pitch.coverage >= HWA_NOTE_MIN_PITCH_COVERAGE) {
            result->pitch.valid = 1;
            result->valid_mask |= HWA_ISOLATED_NOTE_PITCH;
        } else {
            result->rejection_mask |= HWA_ISOLATED_NOTE_REJECT_LOW_SUPPORT;
        }
    } else {
        result->rejection_mask |= HWA_ISOLATED_NOTE_REJECT_LOW_SUPPORT;
    }
    status = 0;

cleanup:
    free(confidences);
    free(pitches);
    hwa_note_work_remove(work, array_bytes * 2U);
    return status;
}

static int hwa_note_decay(const double *samples,
                          size_t frame_count,
                          uint32_t rate,
                          double expected,
                          HWAIsolatedNoteResult *result,
                          HWANoteWork *work,
                          char *error,
                          size_t error_size)
{
    size_t window = (size_t)ceil(4.0 * (double)rate / expected);
    size_t hop;
    size_t point_count;
    double *levels = NULL;
    uint64_t level_bytes = 0U;
    size_t index;
    size_t peak_index = 0U;
    double peak_level = -200.0;
    size_t start;
    size_t end;
    size_t floor_points;
    double *floor_levels = NULL;
    uint64_t floor_bytes = 0U;
    double floor_level;
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;
    double slope;
    double intercept;
    double residual_sum = 0.0;
    double divisor;
    size_t fit_count;
    int status = -1;

    if (window < (size_t)rate / 25U) window = (size_t)rate / 25U;
    if (window > (size_t)rate * 3U / 25U) {
        window = (size_t)rate * 3U / 25U;
    }
    if (window < 16U || window > frame_count) {
        result->rejection_mask |= HWA_ISOLATED_NOTE_REJECT_LOW_SUPPORT;
        return 0;
    }
    hop = window / 4U;
    point_count = 1U + (frame_count - window) / hop;
    if (point_count < 8U) {
        result->rejection_mask |= HWA_ISOLATED_NOTE_REJECT_LOW_SUPPORT;
        return 0;
    }
    if (hwa_note_size_bytes(point_count, sizeof(double), &level_bytes) != 0 ||
        hwa_note_work_add(work, level_bytes, error, error_size) != 0) {
        return -1;
    }
    levels = (double *)malloc((size_t)level_bytes);
    if (levels == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for isolated-note decay");
        goto cleanup;
    }
    for (index = 0U; index < point_count; ++index) {
        double rms;
        if (hwa_note_evaluate(work, (uint64_t)window,
                              error, error_size) != 0) {
            goto cleanup;
        }
        rms = hwa_note_rms(samples, index * hop, window);
        levels[index] = rms > 1.0e-10 ? 20.0 * log10(rms) : -200.0;
        if (levels[index] > peak_level) {
            peak_level = levels[index];
            peak_index = index;
        }
    }
    if (peak_level < -90.0) {
        result->rejection_mask |= HWA_ISOLATED_NOTE_REJECT_SILENCE;
        status = 0;
        goto cleanup;
    }
    floor_points = point_count / 10U;
    if (floor_points < 5U) floor_points = 5U;
    if (floor_points > point_count) floor_points = point_count;
    if (hwa_note_size_bytes(floor_points, sizeof(double), &floor_bytes) != 0 ||
        hwa_note_work_add(work, floor_bytes, error, error_size) != 0) {
        goto cleanup;
    }
    floor_levels = (double *)malloc((size_t)floor_bytes);
    if (floor_levels == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for isolated-note decay floor");
        goto cleanup;
    }
    memcpy(floor_levels, levels + point_count - floor_points,
           (size_t)floor_bytes);
    floor_level = hwa_note_median(floor_levels, floor_points);
    free(floor_levels);
    floor_levels = NULL;
    hwa_note_work_remove(work, floor_bytes);
    floor_bytes = 0U;
    start = peak_index + 1U;
    end = point_count;
    for (index = start + 2U; index < point_count; ++index) {
        if (levels[index] > levels[index - 2U] + 5.0 &&
            levels[index] > floor_level + 8.0) {
            end = index - 1U;
            result->rejection_mask |= HWA_ISOLATED_NOTE_REJECT_LATE_PULSE;
            break;
        }
        if (index > start + 6U && levels[index] <= floor_level + 3.0) {
            end = index + 1U;
            break;
        }
    }
    if (end <= start + 3U) {
        result->rejection_mask |= HWA_ISOLATED_NOTE_REJECT_LOW_SUPPORT;
        status = 0;
        goto cleanup;
    }
    fit_count = end - start;
    for (index = 0U; index < fit_count; ++index) {
        double x = (double)index * (double)hop / (double)rate;
        double y = levels[start + index];
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_xy += x * y;
    }
    divisor = (double)fit_count * sum_xx - sum_x * sum_x;
    if (fabs(divisor) < 1.0e-20) {
        result->rejection_mask |= HWA_ISOLATED_NOTE_REJECT_LOW_SUPPORT;
        status = 0;
        goto cleanup;
    }
    slope = ((double)fit_count * sum_xy - sum_x * sum_y) / divisor;
    intercept = (sum_y - slope * sum_x) / (double)fit_count;
    for (index = 0U; index < fit_count; ++index) {
        double x = (double)index * (double)hop / (double)rate;
        double residual = levels[start + index] - (intercept + slope * x);
        residual_sum += residual * residual;
    }
    result->decay.slope_db_per_second = slope;
    result->decay.t60_seconds = slope < 0.0 ? -60.0 / slope : 0.0;
    result->decay.support_seconds =
        (double)((end - 1U) - start) * (double)hop / (double)rate;
    result->decay.dynamic_range_db = levels[start] - levels[end - 1U];
    result->decay.residual_db = sqrt(residual_sum / (double)fit_count);
    result->decay.floor_dbfs = floor_level;
    result->decay.point_count = (uint64_t)fit_count;
    result->decay.start_sample = (uint64_t)(start * hop);
    result->decay.end_sample = (uint64_t)((end - 1U) * hop + window);
    if (result->decay.end_sample > (uint64_t)frame_count) {
        result->decay.end_sample = (uint64_t)frame_count;
    }
    if (result->decay.support_seconds < 0.30 || slope >= -1.0) {
        result->rejection_mask |= HWA_ISOLATED_NOTE_REJECT_LOW_SUPPORT;
    } else if (result->decay.dynamic_range_db < 10.0) {
        result->rejection_mask |=
            HWA_ISOLATED_NOTE_REJECT_LOW_DYNAMIC_RANGE;
    } else if (result->decay.residual_db > 2.5) {
        result->rejection_mask |= HWA_ISOLATED_NOTE_REJECT_HIGH_RESIDUAL;
    } else {
        result->decay.valid = 1;
        result->valid_mask |= HWA_ISOLATED_NOTE_PASSIVE_DECAY;
    }
    status = 0;

cleanup:
    free(floor_levels);
    hwa_note_work_remove(work, floor_bytes);
    free(levels);
    hwa_note_work_remove(work, level_bytes);
    return status;
}

int hwa_analyze_isolated_note_wav(const char *path,
                                  const HWAIsolatedNoteOptions *options,
                                  HWAIsolatedNoteResult *result,
                                  char *error,
                                  size_t error_size)
{
    HWAIsolatedNoteOptions copied;
    HWAIsolatedNoteResult computed;
    HWAWavReader reader;
    HWANoteWork work;
    double *samples = NULL;
    uint64_t sample_bytes = 0U;
    int status = -1;

    if (error != NULL && error_size > 0U) error[0] = '\0';
    if (result != NULL) memset(result, 0, sizeof(*result));
    if (path == NULL || options == NULL || result == NULL) {
        hwa_set_error(error, error_size,
                      "invalid isolated-note arguments");
        return -1;
    }
    copied = *options;
    if (hwa_note_options_validate(&copied, error, error_size) != 0) return -1;
    memset(&computed, 0, sizeof(computed));
    memset(&reader, 0, sizeof(reader));
    memset(&work, 0, sizeof(work));
    work.max_work = copied.max_work_bytes;
    work.max_evaluations = copied.max_evaluations;
    computed.expected_hz = copied.expected_hz;
    computed.requested_mask = copied.metric_mask;
    computed.path = hwa_note_copy_path(path);
    if (computed.path == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for isolated-note path");
        goto cleanup;
    }
    if (hwa_wav_reader_open(&reader, path, copied.max_input_bytes,
                            error, error_size) != 0) {
        goto cleanup;
    }
    computed.format = reader.format;
    if (hwa_note_decode(&reader, &copied, &samples, &work, &sample_bytes,
                        error, error_size) != 0) {
        goto cleanup;
    }
    if ((copied.metric_mask & HWA_ISOLATED_NOTE_PITCH) != 0U &&
        hwa_note_pitch(samples, (size_t)computed.format.frames,
                       computed.format.sample_rate_hz, copied.expected_hz,
                       &computed, &work, error, error_size) != 0) {
        goto cleanup;
    }
    if ((copied.metric_mask & HWA_ISOLATED_NOTE_PASSIVE_DECAY) != 0U &&
        hwa_note_decay(samples, (size_t)computed.format.frames,
                       computed.format.sample_rate_hz, copied.expected_hz,
                       &computed, &work, error, error_size) != 0) {
        goto cleanup;
    }
    computed.peak_work_bytes = work.peak;
    computed.evaluation_count = work.evaluations;
    *result = computed;
    memset(&computed, 0, sizeof(computed));
    status = 0;

cleanup:
    free(samples);
    hwa_note_work_remove(&work, sample_bytes);
    hwa_wav_reader_close(&reader);
    hwa_isolated_note_result_free(&computed);
    return status;
}

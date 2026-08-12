#include "features.h"

#include "dsp.h"
#include "internal.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HWA_FEATURE_MAX_CHANNELS 1024U
#define HWA_LOUDNESS_BIN_COUNT 1601U
#define HWA_LOUDNESS_BIN_MIN (-120.0)
#define HWA_LOUDNESS_BIN_STEP 0.1
#define HWA_LOUDNESS_RING_SIZE 30U
#define HWA_MIN_PITCH_HZ 40.0
#define HWA_MAX_PITCH_HZ 2000.0
#define HWA_PI 3.14159265358979323846264338327950288

typedef struct HWAChannelAccumulator {
    long double sum;
    long double sum_squares;
    double peak;
    uint64_t clipped_samples;
    uint64_t zero_crossings;
    int previous_sign;
} HWAChannelAccumulator;

typedef struct HWALoudnessHistogram {
    uint64_t count[HWA_LOUDNESS_BIN_COUNT];
    long double energy[HWA_LOUDNESS_BIN_COUNT];
} HWALoudnessHistogram;

struct HWAFeatureProcessor {
    uint32_t sample_rate_hz;
    uint16_t channels;
    uint32_t channel_mask;
    uint64_t expected_frames;
    uint64_t total_frames;
    uint64_t next_frame_start;
    HWAAnalysisOptions options;
    int finished;

    size_t fft_size;
    size_t pitch_fft_size;
    size_t spectrum_bins;
    size_t estimated_feature_frames;
    size_t work_transform_count;
    size_t spectral_transform_count;
    size_t feature_frame_count;

    HWAChannelAccumulator *channel_accumulators;
    HWAChannelMetrics *result_channels;
    double *frame_ring;
    double *k_energy_ring;
    double *window;
    double window_sum_squares;
    HwaDspComplex *fft_work;
    HwaDspComplex *first_fft;
    HwaDspComplex *phase_fft;
    double *power;
    double *previous_magnitude;
    double *previous_phase;
    double *previous_phase_delta;
    int have_previous_spectrum;
    unsigned phase_history;
    double previous_rms_dbfs;
    double previous_pitch_hz;
    int have_previous_rms;
    int have_previous_pitch;

    HwaDspBiquad *k_shelf;
    HwaDspBiquad *k_highpass;
    HwaDspTruePeak4x *true_peak;
    double *loudness_channel_weight;

    HwaDspRunningCovariance stereo_covariance;
    long double left_sum_squares;
    long double right_sum_squares;
    long double mid_sum_squares;
    long double side_sum_squares;

    size_t activity_block_samples;
    size_t activity_block_count;
    long double activity_block_energy;
    uint64_t activity_block_start;
    uint64_t activity_silent_samples;
    uint64_t activity_classified_samples;
    uint64_t activity_first_active;
    uint64_t activity_last_active;
    int activity_found;

    size_t loudness_step_samples;
    size_t loudness_step_count;
    long double loudness_step_energy;
    double loudness_ring[HWA_LOUDNESS_RING_SIZE];
    size_t loudness_ring_count;
    size_t loudness_ring_position;
    uint64_t loudness_step_index;
    HWALoudnessHistogram momentary_histogram;
    HWALoudnessHistogram short_term_histogram;
    double momentary_max_lufs;
    double short_term_max_lufs;
    int have_momentary;
    int have_short_term;

    long double descriptor_centroid_sum;
    long double descriptor_spread_sum;
    long double descriptor_rolloff_sum;
    long double descriptor_flatness_sum;
    long double descriptor_slope_sum;
    long double flux_sum;
    size_t flux_count;
    long double band_power_sum[HWA_BAND_COUNT];
    long double mid_band_power_sum[HWA_BAND_COUNT];
    long double side_band_power_sum[HWA_BAND_COUNT];
    size_t descriptor_count;

    HWAFrameMetrics *tracks;
    size_t track_capacity;
    double *spectrogram;
    size_t spectrogram_capacity;
    size_t spectrogram_count;

    double *delay_left;
    double *delay_right;
    size_t delay_capacity;
    size_t delay_count;
    int delay_started;
};

static const double hwa_band_edges[HWA_BAND_COUNT] = {
    60.0, 120.0, 250.0, 500.0, 1000.0,
    2000.0, 4000.0, 8000.0, 16000.0, DBL_MAX
};

static int hwa_size_multiply(size_t first, size_t second, size_t *product)
{
    if (product == NULL || (second != 0U && first > SIZE_MAX / second)) {
        return -1;
    }
    *product = first * second;
    return 0;
}

static int hwa_add_work_bytes(size_t count,
                              size_t element_size,
                              uint64_t *total)
{
    size_t bytes;

    if (total == NULL ||
        hwa_size_multiply(count, element_size, &bytes) != 0 ||
        (uintmax_t)bytes > (uintmax_t)(UINT64_MAX - *total)) {
        return -1;
    }
    *total += (uint64_t)bytes;
    return 0;
}

static int hwa_uint64_to_size(uint64_t value, size_t *result)
{
    if (result == NULL || value > (uint64_t)SIZE_MAX) {
        return -1;
    }
    *result = (size_t)value;
    return 0;
}

static int hwa_next_power_of_two(size_t value, size_t *result)
{
    size_t power = 1U;

    if (value == 0U || result == NULL) {
        return -1;
    }
    while (power < value) {
        if (power > SIZE_MAX / 2U) {
            return -1;
        }
        power *= 2U;
    }
    *result = power;
    return 0;
}

static double hwa_power_to_db(double power)
{
    return power > 1.0e-30 ? 10.0 * log10(power) : -300.0;
}

static double hwa_power_to_lufs(double power)
{
    return -0.691 + hwa_power_to_db(power);
}

static double hwa_clamp_unit(double value)
{
    if (value < 0.0) {
        return 0.0;
    }
    if (value > 1.0) {
        return 1.0;
    }
    return value;
}

static double hwa_wrap_phase(double phase)
{
    while (phase > HWA_PI) {
        phase -= 2.0 * HWA_PI;
    }
    while (phase < -HWA_PI) {
        phase += 2.0 * HWA_PI;
    }
    return phase;
}

static void hwa_frame_chroma(const HWAFeatureProcessor *processor,
                             double chroma[HWA_CHROMA_BIN_COUNT],
                             int *valid)
{
    double norm = 0.0;
    size_t index;
    size_t chroma_index;

    memset(chroma, 0, HWA_CHROMA_BIN_COUNT * sizeof(*chroma));
    *valid = 0;
    for (index = 1U; index < processor->spectrum_bins; ++index) {
        double frequency = (double)index *
                           (double)processor->sample_rate_hz /
                           (double)processor->fft_size;
        double pitch;
        double pitch_class;
        double fraction;
        double magnitude;
        size_t lower;
        size_t upper;

        if (frequency < 55.0 || frequency > 5000.0) {
            continue;
        }
        magnitude = sqrt(fmax(processor->power[index], 0.0));
        if (!(magnitude > 0.0)) {
            continue;
        }
        /* A mild low-frequency weight keeps upper partials from hiding roots. */
        magnitude *= sqrt(440.0 / frequency);
        pitch = 69.0 + 12.0 * log2(frequency / 440.0);
        pitch_class = fmod(pitch, 12.0);
        if (pitch_class < 0.0) {
            pitch_class += 12.0;
        }
        lower = (size_t)floor(pitch_class);
        fraction = pitch_class - (double)lower;
        upper = (lower + 1U) % HWA_CHROMA_BIN_COUNT;
        chroma[lower] += magnitude * (1.0 - fraction);
        chroma[upper] += magnitude * fraction;
    }
    for (chroma_index = 0U;
         chroma_index < HWA_CHROMA_BIN_COUNT;
         ++chroma_index) {
        norm += chroma[chroma_index] * chroma[chroma_index];
    }
    if (norm > 1.0e-24) {
        norm = sqrt(norm);
        for (chroma_index = 0U;
             chroma_index < HWA_CHROMA_BIN_COUNT;
             ++chroma_index) {
            chroma[chroma_index] /= norm;
        }
        *valid = 1;
    }
}

static size_t hwa_band_for_frequency(double frequency)
{
    size_t band;

    for (band = 0U; band < HWA_BAND_COUNT; ++band) {
        if (frequency < hwa_band_edges[band]) {
            return band;
        }
    }
    return HWA_BAND_COUNT - 1U;
}

static size_t hwa_loudness_bin(double lufs)
{
    double raw;

    if (lufs <= HWA_LOUDNESS_BIN_MIN) {
        return 0U;
    }
    raw = (lufs - HWA_LOUDNESS_BIN_MIN) / HWA_LOUDNESS_BIN_STEP;
    if (raw >= (double)(HWA_LOUDNESS_BIN_COUNT - 1U)) {
        return HWA_LOUDNESS_BIN_COUNT - 1U;
    }
    return (size_t)floor(raw);
}

static double hwa_loudness_bin_center(size_t bin)
{
    return HWA_LOUDNESS_BIN_MIN +
           ((double)bin + 0.5) * HWA_LOUDNESS_BIN_STEP;
}

static void hwa_histogram_add(HWALoudnessHistogram *histogram,
                              double lufs,
                              double energy)
{
    size_t bin = hwa_loudness_bin(lufs);

    histogram->count[bin]++;
    histogram->energy[bin] += (long double)energy;
}

static unsigned hwa_popcount32(uint32_t value)
{
    unsigned count = 0U;

    while (value != 0U) {
        count += value & 1U;
        value >>= 1U;
    }
    return count;
}

static unsigned hwa_mask_bit_for_channel(uint32_t mask, uint16_t channel)
{
    unsigned bit;
    uint16_t seen = 0U;

    for (bit = 0U; bit < 32U; ++bit) {
        if ((mask & ((uint32_t)1U << bit)) != 0U) {
            if (seen == channel) {
                return bit;
            }
            seen++;
        }
    }
    return 32U;
}

static double hwa_bs1770_channel_weight(uint32_t mask,
                                        uint16_t channel,
                                        uint16_t channels)
{
    unsigned bit;

    if (mask == 0U || hwa_popcount32(mask) != (unsigned)channels) {
        return 1.0;
    }
    bit = hwa_mask_bit_for_channel(mask, channel);
    if (bit == 3U) {
        return 0.0; /* LFE */
    }
    if (bit == 4U || bit == 5U || bit == 9U || bit == 10U) {
        return 1.41;
    }
    return 1.0;
}

static double hwa_frame_sample(const HWAFeatureProcessor *processor,
                               uint64_t absolute_frame,
                               uint16_t channel)
{
    size_t ring_frame;

    if (absolute_frame >= processor->total_frames ||
        processor->total_frames - absolute_frame >
            (uint64_t)processor->options.frame_size) {
        return 0.0;
    }
    ring_frame = (size_t)(absolute_frame %
                          (uint64_t)processor->options.frame_size);
    return processor->frame_ring[
        ring_frame * (size_t)processor->channels + (size_t)channel];
}

static double hwa_frame_k_energy(const HWAFeatureProcessor *processor,
                                 uint64_t absolute_frame)
{
    size_t ring_frame;

    if (absolute_frame >= processor->total_frames ||
        processor->total_frames - absolute_frame >
            (uint64_t)processor->options.frame_size) {
        return 0.0;
    }
    ring_frame = (size_t)(absolute_frame %
                          (uint64_t)processor->options.frame_size);
    return processor->k_energy_ring[ring_frame];
}

static void hwa_accumulate_channel(HWAChannelAccumulator *accumulator,
                                   double sample,
                                   int clipped)
{
    double magnitude = fabs(sample);
    int sign = sample > 0.0 ? 1 : (sample < 0.0 ? -1 : 0);

    accumulator->sum += (long double)sample;
    accumulator->sum_squares += (long double)sample * (long double)sample;
    if (magnitude > accumulator->peak) {
        accumulator->peak = magnitude;
    }
    if (clipped != 0) {
        accumulator->clipped_samples++;
    }
    if (sign != 0) {
        if (accumulator->previous_sign != 0 &&
            accumulator->previous_sign != sign) {
            accumulator->zero_crossings++;
        }
        accumulator->previous_sign = sign;
    }
}

static void hwa_classify_activity_block(HWAFeatureProcessor *processor)
{
    double mean_power;
    double threshold_power;
    uint64_t block_end;

    if (processor->activity_block_count == 0U) {
        return;
    }
    mean_power = (double)(processor->activity_block_energy /
                          (long double)processor->activity_block_count);
    threshold_power = pow(10.0,
                          processor->options.silence_threshold_dbfs / 10.0);
    block_end = processor->activity_block_start +
                (uint64_t)processor->activity_block_count;
    if (mean_power < threshold_power) {
        processor->activity_silent_samples +=
            (uint64_t)processor->activity_block_count;
    } else {
        if (!processor->activity_found) {
            processor->activity_first_active =
                processor->activity_block_start;
            processor->activity_found = 1;
        }
        processor->activity_last_active = block_end;
    }
    processor->activity_classified_samples +=
        (uint64_t)processor->activity_block_count;
    processor->activity_block_start = block_end;
    processor->activity_block_count = 0U;
    processor->activity_block_energy = 0.0L;
}

static double hwa_loudness_ring_sum(const HWAFeatureProcessor *processor,
                                    size_t count)
{
    long double sum = 0.0L;
    size_t offset;

    for (offset = 0U; offset < count; ++offset) {
        size_t position =
            (processor->loudness_ring_position + HWA_LOUDNESS_RING_SIZE - 1U -
             offset) % HWA_LOUDNESS_RING_SIZE;
        sum += (long double)processor->loudness_ring[position];
    }
    return (double)sum;
}

static void hwa_finish_loudness_step(HWAFeatureProcessor *processor)
{
    double step_energy;

    if (processor->loudness_step_count == 0U) {
        return;
    }
    step_energy = (double)(processor->loudness_step_energy /
                           (long double)processor->loudness_step_count);
    processor->loudness_ring[processor->loudness_ring_position] = step_energy;
    processor->loudness_ring_position =
        (processor->loudness_ring_position + 1U) % HWA_LOUDNESS_RING_SIZE;
    if (processor->loudness_ring_count < HWA_LOUDNESS_RING_SIZE) {
        processor->loudness_ring_count++;
    }
    processor->loudness_step_index++;

    if (processor->loudness_ring_count >= 4U) {
        double energy = hwa_loudness_ring_sum(processor, 4U) / 4.0;

        if (energy > 0.0) {
            double lufs = hwa_power_to_lufs(energy);

            hwa_histogram_add(&processor->momentary_histogram, lufs, energy);
            if (!processor->have_momentary ||
                lufs > processor->momentary_max_lufs) {
                processor->momentary_max_lufs = lufs;
                processor->have_momentary = 1;
            }
        }
    }
    if (processor->loudness_ring_count >= HWA_LOUDNESS_RING_SIZE &&
        (processor->loudness_step_index - HWA_LOUDNESS_RING_SIZE) % 10U == 0U) {
        double energy = hwa_loudness_ring_sum(
                            processor, HWA_LOUDNESS_RING_SIZE) /
                        (double)HWA_LOUDNESS_RING_SIZE;

        if (energy > 0.0) {
            double lufs = hwa_power_to_lufs(energy);

            hwa_histogram_add(&processor->short_term_histogram, lufs, energy);
            if (!processor->have_short_term ||
                lufs > processor->short_term_max_lufs) {
                processor->short_term_max_lufs = lufs;
                processor->have_short_term = 1;
            }
        }
    }
    processor->loudness_step_count = 0U;
    processor->loudness_step_energy = 0.0L;
}

static int hwa_estimate_pitch(HWAFeatureProcessor *processor,
                              uint64_t frame_start,
                              uint16_t channel,
                              double frame_mean,
                              double frame_power,
                              double *pitch_hz,
                              double *confidence)
{
    size_t index;
    size_t min_lag;
    size_t max_lag;
    size_t best_lag = 0U;
    double best_value = -DBL_MAX;
    double zero_lag;
    double silence_power = pow(
        10.0, processor->options.silence_threshold_dbfs / 10.0);

    *pitch_hz = 0.0;
    *confidence = 0.0;
    if (frame_power <= silence_power) {
        return 0;
    }
    for (index = 0U; index < processor->pitch_fft_size; ++index) {
        processor->fft_work[index].real = 0.0;
        processor->fft_work[index].imag = 0.0;
    }
    for (index = 0U; index < processor->options.frame_size; ++index) {
        double sample = hwa_frame_sample(
                            processor, frame_start + (uint64_t)index,
                            channel) -
                        frame_mean;
        processor->fft_work[index].real = sample * processor->window[index];
    }
    if (hwa_dsp_fft(processor->fft_work,
                    processor->pitch_fft_size, 0) != HWA_DSP_OK) {
        return -1;
    }
    processor->work_transform_count++;
    for (index = 0U; index < processor->pitch_fft_size; ++index) {
        double real = processor->fft_work[index].real;
        double imag = processor->fft_work[index].imag;

        processor->fft_work[index].real = real * real + imag * imag;
        processor->fft_work[index].imag = 0.0;
    }
    if (hwa_dsp_fft(processor->fft_work,
                    processor->pitch_fft_size, 1) != HWA_DSP_OK) {
        return -1;
    }
    processor->work_transform_count++;
    zero_lag = processor->fft_work[0U].real;
    if (!(zero_lag > 0.0)) {
        return 0;
    }

    min_lag = (size_t)ceil((double)processor->sample_rate_hz /
                           HWA_MAX_PITCH_HZ);
    if (min_lag < 1U) {
        min_lag = 1U;
    }
    max_lag = (size_t)floor((double)processor->sample_rate_hz /
                            HWA_MIN_PITCH_HZ);
    if (max_lag >= processor->options.frame_size / 2U) {
        max_lag = processor->options.frame_size / 2U;
    }
    if (min_lag > max_lag) {
        return 0;
    }
    for (index = min_lag; index <= max_lag; ++index) {
        double value = processor->fft_work[index].real;

        if (value > best_value) {
            best_value = value;
            best_lag = index;
        }
    }
    if (best_lag == 0U) {
        return 0;
    }
    *confidence = best_value / zero_lag;
    if (*confidence < 0.0) {
        *confidence = 0.0;
    } else if (*confidence > 1.0) {
        *confidence = 1.0;
    }
    if (*confidence >= 0.30) {
        double lag = (double)best_lag;

        if (best_lag > min_lag && best_lag < max_lag) {
            double left = processor->fft_work[best_lag - 1U].real;
            double center = processor->fft_work[best_lag].real;
            double right = processor->fft_work[best_lag + 1U].real;
            double denominator = left - 2.0 * center + right;

            if (fabs(denominator) > DBL_EPSILON) {
                double offset = 0.5 * (left - right) / denominator;

                if (offset > -1.0 && offset < 1.0) {
                    lag += offset;
                }
            }
        }
        if (lag > 0.0) {
            *pitch_hz = (double)processor->sample_rate_hz / lag;
            return 1;
        }
    }
    return 0;
}

static int hwa_process_feature_frame(HWAFeatureProcessor *processor,
                                     uint64_t frame_start,
                                     char *error,
                                     size_t error_size)
{
    size_t frame_size = processor->options.frame_size;
    size_t bin_count = processor->spectrum_bins;
    size_t channel;
    size_t index;
    size_t needed_transforms = (size_t)processor->channels;
    uint16_t pitch_channel = 0U;
    double pitch_channel_power = -1.0;
    double pitch_channel_mean = 0.0;
    long double frame_sum_squares = 0.0L;
    long double frame_k_energy = 0.0L;
    double normalization;
    double total_power = 0.0;
    double centroid = 0.0;
    double spread = 0.0;
    double rolloff = 0.0;
    double flatness = 0.0;
    double slope = 0.0;
    double flux = 0.0;
    double phase_onset = 0.0;
    double frame_chroma[HWA_CHROMA_BIN_COUNT] = {0.0};
    double band_power[HWA_BAND_COUNT] = {0.0};
    int descriptor_valid = 0;
    int phase_onset_valid = 0;
    int chroma_valid = 0;

    if (processor->options.collect_tracks != 0) {
        needed_transforms += 2U;
    }
    if (needed_transforms > processor->options.max_transforms -
                                processor->work_transform_count) {
        hwa_set_error(error, error_size,
                      "feature transform limit exceeded at frame %llu",
                      (unsigned long long)frame_start);
        return -1;
    }
    memset(processor->power, 0, bin_count * sizeof(*processor->power));
    normalization = 1.0 /
                    ((double)processor->fft_size *
                     processor->window_sum_squares);

    for (channel = 0U; channel < (size_t)processor->channels; ++channel) {
        long double channel_sum = 0.0L;
        long double channel_sum_squares = 0.0L;

        for (index = 0U; index < processor->pitch_fft_size; ++index) {
            processor->fft_work[index].real = 0.0;
            processor->fft_work[index].imag = 0.0;
        }
        for (index = 0U; index < frame_size; ++index) {
            double sample = hwa_frame_sample(
                processor, frame_start + (uint64_t)index,
                (uint16_t)channel);

            channel_sum += (long double)sample;
            channel_sum_squares += (long double)sample * (long double)sample;
            processor->fft_work[index].real =
                sample * processor->window[index];
        }
        if (hwa_dsp_fft(processor->fft_work,
                        processor->fft_size, 0) != HWA_DSP_OK) {
            hwa_set_error(error, error_size,
                          "FFT failed at feature frame %llu",
                          (unsigned long long)frame_start);
            return -1;
        }
        processor->work_transform_count++;
        processor->spectral_transform_count++;
        if (processor->options.collect_tracks != 0 && channel == 0U) {
            memcpy(processor->phase_fft, processor->fft_work,
                   bin_count * sizeof(*processor->phase_fft));
        }
        for (index = 0U; index < bin_count; ++index) {
            double real = processor->fft_work[index].real;
            double imag = processor->fft_work[index].imag;
            double factor = (index == 0U ||
                             (processor->fft_size % 2U == 0U &&
                              index + 1U == bin_count))
                                ? normalization
                                : 2.0 * normalization;
            double bin_power = (real * real + imag * imag) * factor;

            processor->power[index] +=
                bin_power / (double)processor->channels;
            if (channel == 0U) {
                processor->first_fft[index] = processor->fft_work[index];
            } else if (channel == 1U) {
                double left_real = processor->first_fft[index].real;
                double left_imag = processor->first_fft[index].imag;
                double mid_real = (left_real + real) * 0.5;
                double mid_imag = (left_imag + imag) * 0.5;
                double side_real = (left_real - real) * 0.5;
                double side_imag = (left_imag - imag) * 0.5;
                double frequency = (double)index *
                                   (double)processor->sample_rate_hz /
                                   (double)processor->fft_size;
                size_t band = hwa_band_for_frequency(frequency);

                processor->mid_band_power_sum[band] +=
                    (long double)((mid_real * mid_real + mid_imag * mid_imag) *
                                  factor);
                processor->side_band_power_sum[band] +=
                    (long double)((side_real * side_real +
                                   side_imag * side_imag) * factor);
            }
        }
        frame_sum_squares += channel_sum_squares;
        if ((double)(channel_sum_squares / (long double)frame_size) >
            pitch_channel_power) {
            pitch_channel_power =
                (double)(channel_sum_squares / (long double)frame_size);
            pitch_channel_mean =
                (double)(channel_sum / (long double)frame_size);
            pitch_channel = (uint16_t)channel;
        }
    }
    for (index = 0U; index < frame_size; ++index) {
        frame_k_energy += (long double)hwa_frame_k_energy(
            processor, frame_start + (uint64_t)index);
    }

    for (index = 0U; index < bin_count; ++index) {
        double frequency = (double)index *
                           (double)processor->sample_rate_hz /
                           (double)processor->fft_size;
        double bin_power_value = processor->power[index];
        size_t band = hwa_band_for_frequency(frequency);

        total_power += bin_power_value;
        centroid += frequency * bin_power_value;
        band_power[band] += bin_power_value;
    }
    if (total_power > 1.0e-30) {
        double cumulative = 0.0;
        double logarithm_sum = 0.0;
        double arithmetic_sum = 0.0;
        double slope_x_sum = 0.0;
        double slope_y_sum = 0.0;
        double slope_xx_sum = 0.0;
        double slope_xy_sum = 0.0;
        size_t flatness_count = 0U;
        size_t slope_count = 0U;
        int rolloff_found = 0;

        centroid /= total_power;
        for (index = 0U; index < bin_count; ++index) {
            double frequency = (double)index *
                               (double)processor->sample_rate_hz /
                               (double)processor->fft_size;
            double bin_power_value = processor->power[index];
            double delta = frequency - centroid;

            spread += delta * delta * bin_power_value;
            cumulative += bin_power_value;
            if (!rolloff_found && cumulative >= total_power * 0.85) {
                rolloff = frequency;
                rolloff_found = 1;
            }
            if (index > 0U) {
                double bounded = fmax(bin_power_value, 1.0e-30);

                logarithm_sum += log(bounded);
                arithmetic_sum += bounded;
                flatness_count++;
                if (frequency >= 20.0 && bin_power_value > 1.0e-20) {
                    double x = log2(frequency / 1000.0);
                    double y = 10.0 * log10(bin_power_value);

                    slope_x_sum += x;
                    slope_y_sum += y;
                    slope_xx_sum += x * x;
                    slope_xy_sum += x * y;
                    slope_count++;
                }
            }
        }
        spread = sqrt(spread / total_power);
        if (flatness_count != 0U && arithmetic_sum > 0.0) {
            flatness = exp(logarithm_sum / (double)flatness_count) /
                       (arithmetic_sum / (double)flatness_count);
        }
        if (slope_count >= 2U) {
            double count = (double)slope_count;
            double denominator = count * slope_xx_sum -
                                 slope_x_sum * slope_x_sum;

            if (fabs(denominator) > DBL_EPSILON) {
                slope = (count * slope_xy_sum -
                         slope_x_sum * slope_y_sum) /
                        denominator;
            }
        }
        descriptor_valid = 1;
    }

    {
        double magnitude_sum = 0.0;
        double positive_change = 0.0;

        for (index = 0U; index < bin_count; ++index) {
            double magnitude = sqrt(fmax(processor->power[index], 0.0));

            magnitude_sum += magnitude;
            if (processor->have_previous_spectrum != 0 &&
                magnitude > processor->previous_magnitude[index]) {
                positive_change +=
                    magnitude - processor->previous_magnitude[index];
            }
            processor->previous_magnitude[index] = magnitude;
        }
        if (processor->have_previous_spectrum != 0 && magnitude_sum > 0.0) {
            flux = positive_change / magnitude_sum;
            processor->flux_sum += (long double)flux;
            processor->flux_count++;
        }
        processor->have_previous_spectrum = 1;
    }

    if (processor->options.collect_tracks != 0) {
        long double weighted_deviation = 0.0L;
        long double phase_weight = 0.0L;

        for (index = 1U; index < bin_count; ++index) {
            double real = processor->phase_fft[index].real;
            double imag = processor->phase_fft[index].imag;
            double magnitude = hypot(real, imag);
            double phase = atan2(imag, real);
            double delta = hwa_wrap_phase(
                phase - processor->previous_phase[index]);

            if (processor->phase_history >= 2U && magnitude > 1.0e-12) {
                double deviation = hwa_wrap_phase(
                    delta - processor->previous_phase_delta[index]);

                weighted_deviation +=
                    (long double)magnitude *
                    (long double)fabs(sin(0.5 * deviation));
                phase_weight += (long double)magnitude;
            }
            processor->previous_phase[index] = phase;
            processor->previous_phase_delta[index] = delta;
        }
        if (processor->phase_history >= 2U && phase_weight > 0.0L) {
            phase_onset = hwa_clamp_unit(
                (double)(weighted_deviation / phase_weight));
            phase_onset_valid = 1;
        }
        if (processor->phase_history < 2U) {
            processor->phase_history++;
        }
        hwa_frame_chroma(processor, frame_chroma, &chroma_valid);
    }

    if (descriptor_valid != 0) {
        size_t band;

        processor->descriptor_centroid_sum += (long double)centroid;
        processor->descriptor_spread_sum += (long double)spread;
        processor->descriptor_rolloff_sum += (long double)rolloff;
        processor->descriptor_flatness_sum += (long double)flatness;
        processor->descriptor_slope_sum += (long double)slope;
        for (band = 0U; band < HWA_BAND_COUNT; ++band) {
            processor->band_power_sum[band] +=
                (long double)band_power[band];
        }
        processor->descriptor_count++;
    }

    if (processor->options.collect_spectrogram != 0) {
        if (bin_count > processor->spectrogram_capacity -
                            processor->spectrogram_count) {
            hwa_set_error(error, error_size,
                          "spectrogram value limit exceeded at frame %llu",
                          (unsigned long long)frame_start);
            return -1;
        }
        for (index = 0U; index < bin_count; ++index) {
            processor->spectrogram[processor->spectrogram_count++] =
                hwa_power_to_db(processor->power[index]);
        }
    }
    if (processor->options.collect_tracks != 0) {
        HWAFrameMetrics *track;
        double pitch_hz = 0.0;
        double pitch_confidence = 0.0;
        double frame_rms_dbfs;
        double energy_onset = 0.0;
        double pitch_change = 0.0;
        int pitch_result;
        size_t band;

        if (processor->feature_frame_count >= processor->track_capacity) {
            hwa_set_error(error, error_size,
                          "track point limit exceeded at frame %llu",
                          (unsigned long long)frame_start);
            return -1;
        }
        pitch_result = hwa_estimate_pitch(
            processor, frame_start, pitch_channel, pitch_channel_mean,
            pitch_channel_power, &pitch_hz, &pitch_confidence);
        if (pitch_result < 0) {
            hwa_set_error(error, error_size,
                          "pitch transform failed at frame %llu",
                          (unsigned long long)frame_start);
            return -1;
        }
        track = &processor->tracks[processor->feature_frame_count];
        memset(track, 0, sizeof(*track));
        track->time_seconds =
            ((double)frame_start + (double)frame_size * 0.5) /
            (double)processor->sample_rate_hz;
        frame_rms_dbfs = hwa_power_to_db(
            (double)(frame_sum_squares /
                     ((long double)frame_size *
                      (long double)processor->channels)));
        track->rms_dbfs = frame_rms_dbfs;
        if (processor->have_previous_rms != 0) {
            energy_onset = hwa_clamp_unit(
                (frame_rms_dbfs - processor->previous_rms_dbfs) / 18.0);
        }
        processor->previous_rms_dbfs = frame_rms_dbfs;
        processor->have_previous_rms = 1;
        track->frame_lufs = hwa_power_to_lufs(
            (double)(frame_k_energy / (long double)frame_size));
        track->loudness_valid = frame_k_energy > 0.0L;
        track->pitch_hz = pitch_hz;
        track->pitch_confidence = pitch_confidence;
        track->pitch_valid = pitch_result > 0;
        if (pitch_result > 0 && processor->have_previous_pitch != 0 &&
            processor->previous_pitch_hz > 0.0) {
            pitch_change = hwa_clamp_unit(
                fabs(12.0 * log2(pitch_hz /
                                  processor->previous_pitch_hz)) /
                2.0);
            track->pitch_change_valid = 1;
        }
        if (pitch_result > 0) {
            processor->previous_pitch_hz = pitch_hz;
            processor->have_previous_pitch = 1;
        } else {
            processor->have_previous_pitch = 0;
        }
        track->onset_strength = flux;
        track->energy_onset_strength = energy_onset;
        track->phase_onset_strength = phase_onset;
        track->phase_onset_valid = phase_onset_valid;
        track->pitch_change_strength = pitch_change;
        track->combined_onset_strength = hwa_clamp_unit(
            0.40 * hwa_clamp_unit(2.0 * flux) +
            0.25 * energy_onset +
            0.20 * phase_onset +
            0.15 * pitch_change);
        if (chroma_valid != 0 &&
            frame_rms_dbfs >= processor->options.silence_threshold_dbfs) {
            memcpy(track->chroma, frame_chroma, sizeof(track->chroma));
            track->chroma_valid = 1;
        }
        track->spectral_centroid_hz = centroid;
        track->spectral_rolloff_85_hz = rolloff;
        track->spectral_flatness = flatness;
        track->spectrum_valid = descriptor_valid;
        for (band = 0U; band < HWA_BAND_COUNT; ++band) {
            track->band_power_db[band] = hwa_power_to_db(band_power[band]);
        }
    }
    processor->feature_frame_count++;
    return 0;
}

int hwa_features_create(HWAFeatureProcessor **processor,
                        uint32_t sample_rate_hz,
                        uint16_t analyzed_channels,
                        uint32_t channel_mask,
                        uint64_t expected_frames,
                        const HWAAnalysisOptions *options,
                        char *error,
                        size_t error_size)
{
    HWAFeatureProcessor *created = NULL;
    size_t estimated_frames = 0U;
    size_t expected_size = 0U;
    size_t ring_values;
    size_t pitch_input_size;
    size_t pitch_fft_size;
    size_t spectrum_bins;
    size_t transform_per_frame;
    size_t estimated_transforms;
    size_t spectrogram_values = 0U;
    size_t track_capacity;
    size_t phase_storage_count;
    size_t delay_capacity = 0U;
    size_t channel;
    uint64_t work_bytes = 0U;
    int result = -1;

    if (error != NULL && error_size > 0U) {
        error[0] = '\0';
    }
    if (processor == NULL) {
        hwa_set_error(error, error_size,
                      "feature processor output is null");
        return -1;
    }
    *processor = NULL;
    if (options == NULL) {
        hwa_set_error(error, error_size, "feature options are null");
        return -1;
    }
    if (sample_rate_hz < 8000U || sample_rate_hz > 768000U) {
        hwa_set_error(error, error_size,
                      "sample rate %u is outside the feature range 8000-768000 Hz",
                      (unsigned)sample_rate_hz);
        return -1;
    }
    if (analyzed_channels == 0U ||
        analyzed_channels > HWA_FEATURE_MAX_CHANNELS) {
        hwa_set_error(error, error_size,
                      "analyzed channel count %u is outside 1-%u",
                      (unsigned)analyzed_channels,
                      (unsigned)HWA_FEATURE_MAX_CHANNELS);
        return -1;
    }
    if (options->frame_size < 16U ||
        options->frame_size > 1048576U || options->hop_size == 0U ||
        options->hop_size > options->frame_size) {
        hwa_set_error(error, error_size,
                      "feature frame size must be in 16..1048576 and hop must be in 1..frame-size");
        return -1;
    }
    if (!isfinite(options->silence_threshold_dbfs) ||
        options->silence_threshold_dbfs > 0.0 ||
        options->silence_threshold_dbfs < -300.0) {
        hwa_set_error(error, error_size,
                      "silence threshold must be finite and in -300..0 dBFS");
        return -1;
    }
    if (options->true_peak_oversample != 1U &&
        options->true_peak_oversample != 4U) {
        hwa_set_error(error, error_size,
                      "true-peak oversample must be 1 or 4");
        return -1;
    }
    if (options->max_input_frames != 0U &&
        expected_frames > options->max_input_frames) {
        hwa_set_error(error, error_size,
                      "input frame limit exceeded: %llu > %llu",
                      (unsigned long long)expected_frames,
                      (unsigned long long)options->max_input_frames);
        return -1;
    }
    if (hwa_uint64_to_size(expected_frames, &expected_size) != 0) {
        hwa_set_error(error, error_size,
                      "input frame count does not fit this host");
        return -1;
    }
    if (expected_frames != 0U) {
        uint64_t estimated64 =
            (expected_frames - 1U) / (uint64_t)options->hop_size + 1U;

        if (hwa_uint64_to_size(estimated64, &estimated_frames) != 0) {
            hwa_set_error(error, error_size,
                          "feature frame count does not fit this host");
            return -1;
        }
    }
    transform_per_frame = (size_t)analyzed_channels +
                          (options->collect_tracks != 0 ? 2U : 0U);
    if (hwa_size_multiply(estimated_frames, transform_per_frame,
                          &estimated_transforms) != 0) {
        hwa_set_error(error, error_size,
                      "required transform count overflows this host");
        return -1;
    }
    if (estimated_transforms > options->max_transforms) {
        hwa_set_error(error, error_size,
                      "transform limit exceeded: need %llu, limit is %llu",
                      (unsigned long long)estimated_transforms,
                      (unsigned long long)options->max_transforms);
        return -1;
    }
    if (options->collect_tracks != 0 &&
        estimated_frames > options->max_track_points) {
        hwa_set_error(error, error_size,
                      "track point limit exceeded: need %llu, limit is %llu",
                      (unsigned long long)estimated_frames,
                      (unsigned long long)options->max_track_points);
        return -1;
    }
    if (hwa_next_power_of_two(options->frame_size, &pitch_input_size) != 0 ||
        pitch_input_size > SIZE_MAX / 2U) {
        hwa_set_error(error, error_size,
                      "feature FFT size overflows this host");
        return -1;
    }
    pitch_fft_size = pitch_input_size * 2U;
    spectrum_bins = pitch_input_size / 2U + 1U;
    track_capacity = options->collect_tracks != 0 ? estimated_frames : 0U;
    phase_storage_count = options->collect_tracks != 0 ? spectrum_bins : 0U;
    if (options->collect_spectrogram != 0) {
        if (hwa_size_multiply(estimated_frames, spectrum_bins,
                              &spectrogram_values) != 0) {
            hwa_set_error(error, error_size,
                          "required spectrogram size overflows this host");
            return -1;
        }
        if (spectrogram_values > options->max_spectrum_values) {
            hwa_set_error(error, error_size,
                          "spectrogram value limit exceeded: need %llu, limit is %llu",
                          (unsigned long long)spectrogram_values,
                          (unsigned long long)options->max_spectrum_values);
            return -1;
        }
    }
    if (hwa_size_multiply(options->frame_size,
                          (size_t)analyzed_channels, &ring_values) != 0) {
        hwa_set_error(error, error_size,
                      "feature sample ring size overflows this host");
        return -1;
    }
    if (analyzed_channels >= 2U && expected_size != 0U) {
        size_t delay_minimum;

        if (options->max_lag_samples > (SIZE_MAX - 3U) / 4U ||
            options->max_lag_samples > (size_t)PTRDIFF_MAX) {
            hwa_set_error(error, error_size,
                          "delay lag limit overflows this host");
            return -1;
        }
        delay_minimum = options->max_lag_samples * 4U + 3U;
        delay_capacity = (size_t)sample_rate_hz;
        if (delay_capacity < delay_minimum) {
            delay_capacity = delay_minimum;
        }
        if (delay_capacity > expected_size) {
            delay_capacity = expected_size;
        }
    }

    if (hwa_add_work_bytes(1U, sizeof(*created), &work_bytes) != 0 ||
        hwa_add_work_bytes((size_t)analyzed_channels,
                           sizeof(*created->channel_accumulators),
                           &work_bytes) != 0 ||
        hwa_add_work_bytes((size_t)analyzed_channels,
                           sizeof(*created->result_channels),
                           &work_bytes) != 0 ||
        hwa_add_work_bytes(ring_values, sizeof(*created->frame_ring),
                           &work_bytes) != 0 ||
        hwa_add_work_bytes(options->frame_size,
                           sizeof(*created->k_energy_ring),
                           &work_bytes) != 0 ||
        hwa_add_work_bytes(options->frame_size, sizeof(*created->window),
                           &work_bytes) != 0 ||
        hwa_add_work_bytes(pitch_fft_size, sizeof(*created->fft_work),
                           &work_bytes) != 0 ||
        hwa_add_work_bytes(spectrum_bins, sizeof(*created->first_fft),
                           &work_bytes) != 0 ||
        hwa_add_work_bytes(phase_storage_count,
                           sizeof(*created->phase_fft), &work_bytes) != 0 ||
        hwa_add_work_bytes(spectrum_bins, sizeof(*created->power),
                           &work_bytes) != 0 ||
        hwa_add_work_bytes(spectrum_bins,
                           sizeof(*created->previous_magnitude),
                           &work_bytes) != 0 ||
        hwa_add_work_bytes(phase_storage_count,
                           sizeof(*created->previous_phase),
                           &work_bytes) != 0 ||
        hwa_add_work_bytes(phase_storage_count,
                           sizeof(*created->previous_phase_delta),
                           &work_bytes) != 0 ||
        hwa_add_work_bytes((size_t)analyzed_channels,
                           sizeof(*created->k_shelf), &work_bytes) != 0 ||
        hwa_add_work_bytes((size_t)analyzed_channels,
                           sizeof(*created->k_highpass), &work_bytes) != 0 ||
        hwa_add_work_bytes((size_t)analyzed_channels,
                           sizeof(*created->true_peak), &work_bytes) != 0 ||
        hwa_add_work_bytes((size_t)analyzed_channels,
                           sizeof(*created->loudness_channel_weight),
                           &work_bytes) != 0 ||
        hwa_add_work_bytes(track_capacity, sizeof(*created->tracks),
                           &work_bytes) != 0 ||
        hwa_add_work_bytes(spectrogram_values,
                           sizeof(*created->spectrogram),
                           &work_bytes) != 0 ||
        hwa_add_work_bytes(delay_capacity, sizeof(*created->delay_left),
                           &work_bytes) != 0 ||
        hwa_add_work_bytes(delay_capacity, sizeof(*created->delay_right),
                           &work_bytes) != 0) {
        hwa_set_error(error, error_size,
                      "feature work allocation size overflows");
        return -1;
    }
    if (work_bytes > options->max_work_bytes) {
        hwa_set_error(error, error_size,
                      "feature work byte limit exceeded: need %llu, limit is %llu",
                      (unsigned long long)work_bytes,
                      (unsigned long long)options->max_work_bytes);
        return -1;
    }

    created = (HWAFeatureProcessor *)calloc(1U, sizeof(*created));
    if (created == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for feature processor");
        return -1;
    }
    created->sample_rate_hz = sample_rate_hz;
    created->channels = analyzed_channels;
    created->channel_mask = channel_mask;
    created->expected_frames = expected_frames;
    created->options = *options;
    created->fft_size = pitch_input_size;
    created->pitch_fft_size = pitch_fft_size;
    created->spectrum_bins = spectrum_bins;
    created->estimated_feature_frames = estimated_frames;
    created->track_capacity = track_capacity;
    created->spectrogram_capacity = spectrogram_values;
    created->delay_capacity = delay_capacity;

    created->channel_accumulators = (HWAChannelAccumulator *)calloc(
        analyzed_channels, sizeof(*created->channel_accumulators));
    created->result_channels = (HWAChannelMetrics *)calloc(
        analyzed_channels, sizeof(*created->result_channels));
    created->frame_ring = (double *)calloc(ring_values,
                                           sizeof(*created->frame_ring));
    created->k_energy_ring = (double *)calloc(
        options->frame_size, sizeof(*created->k_energy_ring));
    created->window = (double *)malloc(options->frame_size *
                                        sizeof(*created->window));
    created->fft_work = (HwaDspComplex *)calloc(
        created->pitch_fft_size, sizeof(*created->fft_work));
    created->first_fft = (HwaDspComplex *)calloc(
        created->spectrum_bins, sizeof(*created->first_fft));
    created->power = (double *)calloc(created->spectrum_bins,
                                      sizeof(*created->power));
    created->previous_magnitude = (double *)calloc(
        created->spectrum_bins, sizeof(*created->previous_magnitude));
    if (phase_storage_count != 0U) {
        created->phase_fft = (HwaDspComplex *)calloc(
            phase_storage_count, sizeof(*created->phase_fft));
        created->previous_phase = (double *)calloc(
            phase_storage_count, sizeof(*created->previous_phase));
        created->previous_phase_delta = (double *)calloc(
            phase_storage_count, sizeof(*created->previous_phase_delta));
    }
    created->k_shelf = (HwaDspBiquad *)calloc(
        analyzed_channels, sizeof(*created->k_shelf));
    created->k_highpass = (HwaDspBiquad *)calloc(
        analyzed_channels, sizeof(*created->k_highpass));
    created->true_peak = (HwaDspTruePeak4x *)calloc(
        analyzed_channels, sizeof(*created->true_peak));
    created->loudness_channel_weight = (double *)calloc(
        analyzed_channels, sizeof(*created->loudness_channel_weight));
    if (created->channel_accumulators == NULL ||
        created->result_channels == NULL || created->frame_ring == NULL ||
        created->k_energy_ring == NULL || created->window == NULL ||
        created->fft_work == NULL || created->first_fft == NULL ||
        created->power == NULL || created->previous_magnitude == NULL ||
        (phase_storage_count != 0U &&
         (created->phase_fft == NULL || created->previous_phase == NULL ||
          created->previous_phase_delta == NULL)) ||
        created->k_shelf == NULL || created->k_highpass == NULL ||
        created->true_peak == NULL ||
        created->loudness_channel_weight == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for feature work storage");
        goto cleanup;
    }
    if (created->track_capacity != 0U) {
        created->tracks = (HWAFrameMetrics *)calloc(
            created->track_capacity, sizeof(*created->tracks));
        if (created->tracks == NULL) {
            hwa_set_error(error, error_size,
                          "out of memory for feature tracks");
            goto cleanup;
        }
    }
    if (created->spectrogram_capacity != 0U) {
        created->spectrogram = (double *)calloc(
            created->spectrogram_capacity, sizeof(*created->spectrogram));
        if (created->spectrogram == NULL) {
            hwa_set_error(error, error_size,
                          "out of memory for spectrogram");
            goto cleanup;
        }
    }
    if (hwa_dsp_hann(created->window,
                     options->frame_size) != HWA_DSP_OK) {
        hwa_set_error(error, error_size, "could not make Hann window");
        goto cleanup;
    }
    for (channel = 0U; channel < options->frame_size; ++channel) {
        created->window_sum_squares +=
            created->window[channel] * created->window[channel];
    }
    if (!(created->window_sum_squares > 0.0)) {
        hwa_set_error(error, error_size, "Hann window has no energy");
        goto cleanup;
    }

    for (channel = 0U; channel < (size_t)analyzed_channels; ++channel) {
        double shelf_frequency = 1681.974450955533;
        double highpass_frequency = 38.13547087602444;

        if (shelf_frequency >= (double)sample_rate_hz * 0.45) {
            shelf_frequency = (double)sample_rate_hz * 0.40;
        }
        if (hwa_dsp_biquad_design_highshelf(
                &created->k_shelf[channel], (double)sample_rate_hz,
                shelf_frequency, 3.99984385397, 1.0) != HWA_DSP_OK ||
            hwa_dsp_biquad_design_highpass(
                &created->k_highpass[channel], (double)sample_rate_hz,
                highpass_frequency, 0.5003270373) != HWA_DSP_OK) {
            hwa_set_error(error, error_size,
                          "could not design K-weighting filters");
            goto cleanup;
        }
        hwa_dsp_true_peak_4x_reset(&created->true_peak[channel]);
        created->loudness_channel_weight[channel] =
            hwa_bs1770_channel_weight(channel_mask, (uint16_t)channel,
                                      analyzed_channels);
    }
    hwa_dsp_covariance_reset(&created->stereo_covariance);
    created->activity_block_samples =
        ((size_t)sample_rate_hz + 50U) / 100U;
    if (created->activity_block_samples == 0U) {
        created->activity_block_samples = 1U;
    }
    created->loudness_step_samples =
        ((size_t)sample_rate_hz + 5U) / 10U;
    if (created->loudness_step_samples == 0U) {
        created->loudness_step_samples = 1U;
    }

    if (created->delay_capacity != 0U) {
        created->delay_left = (double *)calloc(
            created->delay_capacity, sizeof(*created->delay_left));
        created->delay_right = (double *)calloc(
            created->delay_capacity, sizeof(*created->delay_right));
        if (created->delay_left == NULL || created->delay_right == NULL) {
            hwa_set_error(error, error_size,
                          "out of memory for bounded delay window");
            goto cleanup;
        }
    }
    result = 0;

cleanup:
    if (result != 0) {
        hwa_features_destroy(created);
        return -1;
    }
    *processor = created;
    return 0;
}

int hwa_features_push(HWAFeatureProcessor *processor,
                      const double *samples,
                      const unsigned char *clipped,
                      size_t frame_count,
                      char *error,
                      size_t error_size)
{
    size_t sample_count;
    size_t index;
    size_t frame;

    if (error != NULL && error_size > 0U) {
        error[0] = '\0';
    }
    if (processor == NULL || (samples == NULL && frame_count != 0U)) {
        hwa_set_error(error, error_size, "invalid feature push arguments");
        return -1;
    }
    if (processor->finished != 0) {
        hwa_set_error(error, error_size,
                      "cannot push after feature finish");
        return -1;
    }
    if ((uint64_t)frame_count >
        processor->expected_frames - processor->total_frames) {
        hwa_set_error(error, error_size,
                      "feature input exceeds declared frame count");
        return -1;
    }
    if (hwa_size_multiply(frame_count, (size_t)processor->channels,
                          &sample_count) != 0) {
        hwa_set_error(error, error_size,
                      "feature input sample count overflows this host");
        return -1;
    }
    for (index = 0U; index < sample_count; ++index) {
        if (!isfinite(samples[index])) {
            hwa_set_error(error, error_size,
                          "non-finite analyzed sample at block sample %llu",
                          (unsigned long long)index);
            return -1;
        }
    }

    for (frame = 0U; frame < frame_count; ++frame) {
        size_t channel;
        size_t ring_frame = (size_t)(
            processor->total_frames %
            (uint64_t)processor->options.frame_size);
        long double activity_energy = 0.0L;
        long double k_energy = 0.0L;
        double left = 0.0;
        double right = 0.0;
        double largest_stereo_sample = 0.0;

        for (channel = 0U; channel < (size_t)processor->channels; ++channel) {
            size_t input_index = frame * (size_t)processor->channels + channel;
            double sample = samples[input_index];
            double shelf_output;
            double weighted_output;
            int clipped_value = clipped != NULL ? clipped[input_index] != 0U
                                                 : 0;

            processor->frame_ring[
                ring_frame * (size_t)processor->channels + channel] = sample;
            hwa_accumulate_channel(&processor->channel_accumulators[channel],
                                   sample, clipped_value);
            activity_energy += (long double)sample * (long double)sample /
                               (long double)processor->channels;
            if (processor->options.true_peak_oversample == 4U &&
                hwa_dsp_true_peak_4x_push(
                    &processor->true_peak[channel], &sample, 1U) !=
                    HWA_DSP_OK) {
                hwa_set_error(error, error_size,
                              "true-peak filter failed at frame %llu",
                              (unsigned long long)processor->total_frames);
                return -1;
            }
            if (hwa_dsp_biquad_process_sample(
                    &processor->k_shelf[channel], sample,
                    &shelf_output) != HWA_DSP_OK ||
                hwa_dsp_biquad_process_sample(
                    &processor->k_highpass[channel], shelf_output,
                    &weighted_output) != HWA_DSP_OK ||
                !isfinite(weighted_output)) {
                hwa_set_error(error, error_size,
                              "K-weighting filter failed at frame %llu",
                              (unsigned long long)processor->total_frames);
                return -1;
            }
            k_energy += (long double)processor->loudness_channel_weight[channel] *
                        (long double)weighted_output *
                        (long double)weighted_output;
            if (channel == 0U) {
                left = sample;
            } else if (channel == 1U) {
                right = sample;
            }
        }
        processor->k_energy_ring[ring_frame] = (double)k_energy;
        processor->activity_block_energy += activity_energy;
        processor->activity_block_count++;
        if (processor->activity_block_count ==
            processor->activity_block_samples) {
            hwa_classify_activity_block(processor);
        }
        processor->loudness_step_energy += k_energy;
        processor->loudness_step_count++;
        if (processor->loudness_step_count ==
            processor->loudness_step_samples) {
            hwa_finish_loudness_step(processor);
        }

        if (processor->channels >= 2U) {
            long double mid = ((long double)left + (long double)right) * 0.5L;
            long double side = ((long double)left - (long double)right) * 0.5L;

            (void)hwa_dsp_covariance_push(&processor->stereo_covariance,
                                          left, right);
            processor->left_sum_squares +=
                (long double)left * (long double)left;
            processor->right_sum_squares +=
                (long double)right * (long double)right;
            processor->mid_sum_squares += mid * mid;
            processor->side_sum_squares += side * side;
            largest_stereo_sample = fmax(fabs(left), fabs(right));
            if (!processor->delay_started &&
                largest_stereo_sample >=
                    pow(10.0,
                        processor->options.silence_threshold_dbfs / 20.0)) {
                processor->delay_started = 1;
            }
            if (processor->delay_started &&
                processor->delay_count < processor->delay_capacity) {
                processor->delay_left[processor->delay_count] = left;
                processor->delay_right[processor->delay_count] = right;
                processor->delay_count++;
            }
        }
        processor->total_frames++;
        while (processor->next_frame_start <= processor->total_frames &&
               processor->total_frames - processor->next_frame_start >=
                   (uint64_t)processor->options.frame_size) {
            if (hwa_process_feature_frame(processor,
                                          processor->next_frame_start,
                                          error, error_size) != 0) {
                return -1;
            }
            processor->next_frame_start +=
                (uint64_t)processor->options.hop_size;
        }
    }
    return 0;
}

static int hwa_finish_channel_metrics(HWAFeatureProcessor *processor,
                                      char *error,
                                      size_t error_size)
{
    size_t channel;

    for (channel = 0U; channel < (size_t)processor->channels; ++channel) {
        const HWAChannelAccumulator *source =
            &processor->channel_accumulators[channel];
        HWAChannelMetrics *target = &processor->result_channels[channel];

        target->peak = source->peak;
        target->clipped_samples = source->clipped_samples;
        if (processor->total_frames != 0U) {
            long double count = (long double)processor->total_frames;

            target->rms = (double)sqrtl(source->sum_squares / count);
            target->dc_offset = (double)(source->sum / count);
            target->zero_crossing_rate = processor->total_frames > 1U
                                             ? (double)source->zero_crossings /
                                                   (double)(processor->total_frames -
                                                            1U)
                                             : 0.0;
            target->true_peak_valid = 1;
        }
        if (target->rms > 0.0) {
            target->crest_factor = target->peak / target->rms;
            target->crest_factor_valid = 1;
        }
        if (processor->options.true_peak_oversample == 4U) {
            double peak = 0.0;

            int peak_status = hwa_dsp_true_peak_4x_finish(
                &processor->true_peak[channel], &peak);

            if (peak_status == HWA_DSP_OK) {
                target->true_peak = peak;
            } else if (processor->total_frames == 0U &&
                       peak_status == HWA_DSP_NO_DATA) {
                target->true_peak = target->peak;
                target->true_peak_valid = 0;
            } else {
                hwa_set_error(error, error_size,
                              "true-peak finish failed for channel %u",
                              (unsigned)channel + 1U);
                return -1;
            }
        } else {
            target->true_peak = target->peak;
        }
    }
    return 0;
}

static void hwa_finish_loudness_metrics(
    const HWAFeatureProcessor *processor,
    HWALoudnessMetrics *metrics)
{
    uint64_t absolute_count = 0U;
    uint64_t relative_count = 0U;
    long double absolute_energy = 0.0L;
    long double relative_energy = 0.0L;
    double relative_gate = -70.0;
    size_t bin;

    memset(metrics, 0, sizeof(*metrics));
    for (bin = 0U; bin < HWA_LOUDNESS_BIN_COUNT; ++bin) {
        if (hwa_loudness_bin_center(bin) >= -70.0) {
            absolute_count += processor->momentary_histogram.count[bin];
            absolute_energy += processor->momentary_histogram.energy[bin];
        }
    }
    metrics->blocks_above_absolute_gate = absolute_count;
    if (absolute_count != 0U && absolute_energy > 0.0L) {
        double preliminary = hwa_power_to_lufs(
            (double)(absolute_energy / (long double)absolute_count));

        relative_gate = preliminary - 10.0;
        if (relative_gate < -70.0) {
            relative_gate = -70.0;
        }
        for (bin = 0U; bin < HWA_LOUDNESS_BIN_COUNT; ++bin) {
            if (hwa_loudness_bin_center(bin) >= relative_gate) {
                relative_count += processor->momentary_histogram.count[bin];
                relative_energy += processor->momentary_histogram.energy[bin];
            }
        }
        if (relative_count != 0U && relative_energy > 0.0L) {
            metrics->integrated_lufs = hwa_power_to_lufs(
                (double)(relative_energy / (long double)relative_count));
            metrics->integrated_valid = 1;
        }
    }
    metrics->blocks_above_relative_gate = relative_count;
    if (processor->have_momentary) {
        metrics->momentary_max_lufs = processor->momentary_max_lufs;
        metrics->momentary_valid = 1;
    }
    if (processor->have_short_term) {
        metrics->short_term_max_lufs = processor->short_term_max_lufs;
        metrics->short_term_valid = 1;
    }

    {
        double range_gate = -70.0;
        uint64_t range_count = 0U;
        uint64_t tenth_rank;
        uint64_t ninety_fifth_rank;
        uint64_t cumulative = 0U;
        double low = 0.0;
        double high = 0.0;
        int have_low = 0;
        int have_high = 0;

        if (metrics->integrated_valid != 0 &&
            metrics->integrated_lufs - 20.0 > range_gate) {
            range_gate = metrics->integrated_lufs - 20.0;
        }
        for (bin = 0U; bin < HWA_LOUDNESS_BIN_COUNT; ++bin) {
            if (hwa_loudness_bin_center(bin) >= range_gate) {
                range_count += processor->short_term_histogram.count[bin];
            }
        }
        if (range_count >= 2U) {
            tenth_rank = range_count / 10U;
            if (range_count % 10U != 0U) {
                tenth_rank++;
            }
            ninety_fifth_rank = (range_count / 100U) * 95U;
            ninety_fifth_rank +=
                ((range_count % 100U) * 95U + 99U) / 100U;
            if (tenth_rank == 0U) {
                tenth_rank = 1U;
            }
            if (ninety_fifth_rank == 0U) {
                ninety_fifth_rank = 1U;
            }
            for (bin = 0U; bin < HWA_LOUDNESS_BIN_COUNT; ++bin) {
                if (hwa_loudness_bin_center(bin) < range_gate) {
                    continue;
                }
                cumulative += processor->short_term_histogram.count[bin];
                if (!have_low && cumulative >= tenth_rank) {
                    low = hwa_loudness_bin_center(bin);
                    have_low = 1;
                }
                if (!have_high && cumulative >= ninety_fifth_rank) {
                    high = hwa_loudness_bin_center(bin);
                    have_high = 1;
                    break;
                }
            }
            if (have_low && have_high) {
                metrics->loudness_range_lu = high - low;
                metrics->range_valid = 1;
            }
        }
    }
}

static void hwa_finish_spectral_metrics(
    const HWAFeatureProcessor *processor,
    HWASpectralMetrics *metrics)
{
    size_t band;

    memset(metrics, 0, sizeof(*metrics));
    metrics->transform_count =
        (uint64_t)processor->spectral_transform_count;
    if (processor->descriptor_count == 0U) {
        return;
    }
    metrics->centroid_hz =
        (double)(processor->descriptor_centroid_sum /
                 (long double)processor->descriptor_count);
    metrics->spread_hz =
        (double)(processor->descriptor_spread_sum /
                 (long double)processor->descriptor_count);
    metrics->rolloff_85_hz =
        (double)(processor->descriptor_rolloff_sum /
                 (long double)processor->descriptor_count);
    metrics->flatness =
        (double)(processor->descriptor_flatness_sum /
                 (long double)processor->descriptor_count);
    metrics->slope_db_per_octave =
        (double)(processor->descriptor_slope_sum /
                 (long double)processor->descriptor_count);
    if (processor->flux_count != 0U) {
        metrics->mean_flux =
            (double)(processor->flux_sum /
                     (long double)processor->flux_count);
    }
    for (band = 0U; band < HWA_BAND_COUNT; ++band) {
        metrics->band_power[band] =
            (double)(processor->band_power_sum[band] /
                     (long double)processor->descriptor_count);
    }
    metrics->valid = 1;
}

static void hwa_finish_activity_metrics(
    const HWAFeatureProcessor *processor,
    HWAActivityMetrics *metrics)
{
    memset(metrics, 0, sizeof(*metrics));
    metrics->threshold_dbfs = processor->options.silence_threshold_dbfs;
    if (processor->activity_classified_samples != 0U) {
        metrics->silence_fraction =
            (double)processor->activity_silent_samples /
            (double)processor->activity_classified_samples;
        metrics->classified_valid = 1;
    }
    if (processor->activity_found) {
        metrics->active_start_seconds =
            (double)processor->activity_first_active /
            (double)processor->sample_rate_hz;
        metrics->active_end_seconds =
            (double)processor->activity_last_active /
            (double)processor->sample_rate_hz;
        metrics->active_span_valid = 1;
    }
}

static void hwa_finish_stereo_metrics(
    const HWAFeatureProcessor *processor,
    HWAStereoMetrics *metrics)
{
    size_t band;

    memset(metrics, 0, sizeof(*metrics));
    if (processor->channels < 2U) {
        return;
    }
    metrics->available = 1;
    if (processor->total_frames != 0U) {
        long double count = (long double)processor->total_frames;
        double left_rms = (double)sqrtl(processor->left_sum_squares / count);
        double right_rms =
            (double)sqrtl(processor->right_sum_squares / count);

        metrics->mid_rms =
            (double)sqrtl(processor->mid_sum_squares / count);
        metrics->side_rms =
            (double)sqrtl(processor->side_sum_squares / count);
        metrics->level_valid = 1;
        if (left_rms > 0.0 && right_rms > 0.0) {
            metrics->balance_db = 20.0 * log10(right_rms / left_rms);
        } else if (right_rms > 0.0) {
            metrics->balance_db = 300.0;
        } else if (left_rms > 0.0) {
            metrics->balance_db = -300.0;
        }
        if (metrics->mid_rms > 0.0) {
            metrics->width_ratio = metrics->side_rms / metrics->mid_rms;
            metrics->width_valid = 1;
        }
    }
    if (hwa_dsp_covariance_correlation(&processor->stereo_covariance,
                                       &metrics->correlation) == HWA_DSP_OK) {
        metrics->correlation_valid = 1;
    }
    for (band = 0U; band < HWA_BAND_COUNT; ++band) {
        if (processor->mid_band_power_sum[band] > 0.0L) {
            metrics->band_width[band] = (double)sqrtl(
                processor->side_band_power_sum[band] /
                processor->mid_band_power_sum[band]);
            metrics->band_width_valid_mask |=
                (uint16_t)((uint16_t)1U << band);
        }
    }
    if (processor->delay_count >= 3U) {
        size_t max_lag = processor->options.max_lag_samples;
        ptrdiff_t delay;
        double correlation;

        if (max_lag > processor->delay_count - 3U) {
            max_lag = processor->delay_count - 3U;
        }
        if (hwa_dsp_estimate_delay(processor->delay_left,
                                   processor->delay_right,
                                   processor->delay_count, max_lag,
                                   &delay, &correlation) == HWA_DSP_OK) {
            metrics->interchannel_delay_samples = (double)delay;
            metrics->interchannel_delay_seconds =
                (double)delay / (double)processor->sample_rate_hz;
            metrics->interchannel_delay_confidence = fabs(correlation);
            metrics->delay_valid = 1;
        }
    }
}

int hwa_features_finish(HWAFeatureProcessor *processor,
                        HWAAnalysis *analysis,
                        char *error,
                        size_t error_size)
{
    if (error != NULL && error_size > 0U) {
        error[0] = '\0';
    }
    if (processor == NULL || analysis == NULL) {
        hwa_set_error(error, error_size,
                      "invalid feature finish arguments");
        return -1;
    }
    if (processor->finished != 0) {
        hwa_set_error(error, error_size,
                      "feature processor was already finished");
        return -1;
    }
    if (processor->total_frames != processor->expected_frames) {
        hwa_set_error(error, error_size,
                      "feature stream ended at %llu of %llu declared frames",
                      (unsigned long long)processor->total_frames,
                      (unsigned long long)processor->expected_frames);
        return -1;
    }
    if (analysis->channels != NULL || analysis->tracks != NULL ||
        analysis->spectrogram_db != NULL) {
        hwa_set_error(error, error_size,
                      "analysis already owns feature result arrays");
        return -1;
    }
    while (processor->next_frame_start < processor->total_frames) {
        if (hwa_process_feature_frame(processor,
                                      processor->next_frame_start,
                                      error, error_size) != 0) {
            return -1;
        }
        processor->next_frame_start +=
            (uint64_t)processor->options.hop_size;
    }
    hwa_classify_activity_block(processor);
    if (hwa_finish_channel_metrics(processor, error, error_size) != 0) {
        return -1;
    }

    analysis->analyzed_channels = processor->channels;
    analysis->channels = processor->result_channels;
    processor->result_channels = NULL;
    hwa_finish_loudness_metrics(processor, &analysis->loudness);
    hwa_finish_spectral_metrics(processor, &analysis->spectrum);
    hwa_finish_activity_metrics(processor, &analysis->activity);
    hwa_finish_stereo_metrics(processor, &analysis->stereo);
    if (processor->options.collect_tracks != 0) {
        analysis->tracks = processor->tracks;
        analysis->track_count = processor->feature_frame_count;
        processor->tracks = NULL;
    }
    if (processor->options.collect_spectrogram != 0) {
        analysis->spectrogram_db = processor->spectrogram;
        analysis->spectrum_bins = processor->spectrum_bins;
        processor->spectrogram = NULL;
    }
    processor->finished = 1;
    return 0;
}

void hwa_features_destroy(HWAFeatureProcessor *processor)
{
    if (processor == NULL) {
        return;
    }
    free(processor->channel_accumulators);
    free(processor->result_channels);
    free(processor->frame_ring);
    free(processor->k_energy_ring);
    free(processor->window);
    free(processor->fft_work);
    free(processor->first_fft);
    free(processor->phase_fft);
    free(processor->power);
    free(processor->previous_magnitude);
    free(processor->previous_phase);
    free(processor->previous_phase_delta);
    free(processor->k_shelf);
    free(processor->k_highpass);
    free(processor->true_peak);
    free(processor->loudness_channel_weight);
    free(processor->tracks);
    free(processor->spectrogram);
    free(processor->delay_left);
    free(processor->delay_right);
    free(processor);
}

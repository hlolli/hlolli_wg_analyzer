#include "dsp.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define HWA_DSP_PI 3.14159265358979323846264338327950288
#define HWA_DSP_TRUE_PEAK_FACTOR 4U
#define HWA_DSP_TRUE_PEAK_RADIUS 12

static int hwa_dsp_is_power_of_two(size_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static int hwa_dsp_values_are_finite(const double *values, size_t count)
{
    size_t index;

    if (values == NULL) {
        return 0;
    }
    for (index = 0U; index < count; ++index) {
        if (!isfinite(values[index])) {
            return 0;
        }
    }
    return 1;
}

int hwa_dsp_hann(double *window, size_t count)
{
    size_t index;
    double denominator;

    if (window == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (count < 2U) {
        return HWA_DSP_BAD_SIZE;
    }

    denominator = (double)(count - 1U);
    for (index = 0U; index < count; ++index) {
        double phase = (2.0 * HWA_DSP_PI * (double)index) / denominator;
        window[index] = 0.5 - (0.5 * cos(phase));
    }
    window[0] = 0.0;
    window[count - 1U] = 0.0;
    return HWA_DSP_OK;
}

int hwa_dsp_fft(HwaDspComplex *values, size_t count, int inverse)
{
    size_t index;
    size_t reversed;
    size_t length;

    if (values == NULL || (inverse != 0 && inverse != 1)) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (!hwa_dsp_is_power_of_two(count)) {
        return HWA_DSP_BAD_SIZE;
    }
    for (index = 0U; index < count; ++index) {
        if (!isfinite(values[index].real) || !isfinite(values[index].imag)) {
            return HWA_DSP_NONFINITE;
        }
    }

    reversed = 0U;
    for (index = 1U; index < count; ++index) {
        size_t bit = count >> 1U;

        while ((reversed & bit) != 0U) {
            reversed ^= bit;
            bit >>= 1U;
        }
        reversed ^= bit;
        if (index < reversed) {
            HwaDspComplex temporary = values[index];
            values[index] = values[reversed];
            values[reversed] = temporary;
        }
    }

    length = 2U;
    while (length <= count && length != 0U) {
        size_t half = length / 2U;
        double direction = inverse != 0 ? 1.0 : -1.0;
        double angle = direction * 2.0 * HWA_DSP_PI / (double)length;
        double step_real = cos(angle);
        double step_imag = sin(angle);
        size_t start;

        for (start = 0U; start < count; start += length) {
            double twiddle_real = 1.0;
            double twiddle_imag = 0.0;
            size_t offset;

            for (offset = 0U; offset < half; ++offset) {
                size_t even_index = start + offset;
                size_t odd_index = even_index + half;
                double odd_real = (values[odd_index].real * twiddle_real) -
                                  (values[odd_index].imag * twiddle_imag);
                double odd_imag = (values[odd_index].real * twiddle_imag) +
                                  (values[odd_index].imag * twiddle_real);
                double even_real = values[even_index].real;
                double even_imag = values[even_index].imag;
                double next_twiddle_real;
                double next_twiddle_imag;

                values[even_index].real = even_real + odd_real;
                values[even_index].imag = even_imag + odd_imag;
                values[odd_index].real = even_real - odd_real;
                values[odd_index].imag = even_imag - odd_imag;
                if (!isfinite(values[even_index].real) ||
                    !isfinite(values[even_index].imag) ||
                    !isfinite(values[odd_index].real) ||
                    !isfinite(values[odd_index].imag)) {
                    return HWA_DSP_NUMERIC_ERROR;
                }

                next_twiddle_real = (twiddle_real * step_real) -
                                     (twiddle_imag * step_imag);
                next_twiddle_imag = (twiddle_real * step_imag) +
                                     (twiddle_imag * step_real);
                twiddle_real = next_twiddle_real;
                twiddle_imag = next_twiddle_imag;
            }
        }
        if (length == count) {
            break;
        }
        if (length > SIZE_MAX / 2U) {
            return HWA_DSP_BAD_SIZE;
        }
        length *= 2U;
    }

    if (inverse != 0) {
        double scale = 1.0 / (double)count;
        for (index = 0U; index < count; ++index) {
            values[index].real *= scale;
            values[index].imag *= scale;
        }
    }
    return HWA_DSP_OK;
}

void hwa_dsp_stats_reset(HwaDspRunningStats *stats)
{
    if (stats != NULL) {
        stats->count = 0U;
        stats->mean = 0.0L;
        stats->m2 = 0.0L;
    }
}

int hwa_dsp_stats_push(HwaDspRunningStats *stats, double value)
{
    uint64_t next_count;
    long double delta;
    long double next_mean;
    long double next_m2;

    if (stats == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (!isfinite(value)) {
        return HWA_DSP_NONFINITE;
    }
    if (stats->count == UINT64_MAX || !isfinite(stats->mean) ||
        !isfinite(stats->m2) || stats->m2 < 0.0L) {
        return HWA_DSP_NUMERIC_ERROR;
    }

    next_count = stats->count + 1U;
    delta = (long double)value - stats->mean;
    next_mean = stats->mean + (delta / (long double)next_count);
    next_m2 = stats->m2 +
              (delta * ((long double)value - next_mean));
    if (!isfinite(next_mean) || !isfinite(next_m2) || next_m2 < 0.0L) {
        return HWA_DSP_NUMERIC_ERROR;
    }
    stats->count = next_count;
    stats->mean = next_mean;
    stats->m2 = next_m2;
    return HWA_DSP_OK;
}

static int hwa_dsp_stats_variance(const HwaDspRunningStats *stats,
                                  uint64_t divisor,
                                  double *variance)
{
    long double result;

    if (stats == NULL || variance == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (divisor == 0U || !isfinite(stats->m2) || stats->m2 < 0.0L) {
        return HWA_DSP_NO_DATA;
    }
    result = stats->m2 / (long double)divisor;
    if (!isfinite(result) || result > (long double)DBL_MAX) {
        return HWA_DSP_NUMERIC_ERROR;
    }
    *variance = (double)result;
    return HWA_DSP_OK;
}

int hwa_dsp_stats_mean(const HwaDspRunningStats *stats, double *mean)
{
    if (stats == NULL || mean == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (stats->count == 0U) {
        return HWA_DSP_NO_DATA;
    }
    if (!isfinite(stats->mean) || fabsl(stats->mean) > (long double)DBL_MAX) {
        return HWA_DSP_NUMERIC_ERROR;
    }
    *mean = (double)stats->mean;
    return HWA_DSP_OK;
}

int hwa_dsp_stats_population_variance(const HwaDspRunningStats *stats,
                                      double *variance)
{
    if (stats == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    return hwa_dsp_stats_variance(stats, stats->count, variance);
}

int hwa_dsp_stats_sample_variance(const HwaDspRunningStats *stats,
                                  double *variance)
{
    if (stats == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (stats->count < 2U) {
        return HWA_DSP_NO_DATA;
    }
    return hwa_dsp_stats_variance(stats, stats->count - 1U, variance);
}

void hwa_dsp_covariance_reset(HwaDspRunningCovariance *stats)
{
    if (stats != NULL) {
        stats->count = 0U;
        stats->mean_x = 0.0L;
        stats->mean_y = 0.0L;
        stats->m2_x = 0.0L;
        stats->m2_y = 0.0L;
        stats->c2 = 0.0L;
    }
}

int hwa_dsp_covariance_push(HwaDspRunningCovariance *stats,
                            double x,
                            double y)
{
    uint64_t next_count;
    long double delta_x;
    long double delta_y;
    long double next_mean_x;
    long double next_mean_y;
    long double next_m2_x;
    long double next_m2_y;
    long double next_c2;

    if (stats == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (!isfinite(x) || !isfinite(y)) {
        return HWA_DSP_NONFINITE;
    }
    if (stats->count == UINT64_MAX || !isfinite(stats->mean_x) ||
        !isfinite(stats->mean_y) || !isfinite(stats->m2_x) ||
        !isfinite(stats->m2_y) || !isfinite(stats->c2) ||
        stats->m2_x < 0.0L || stats->m2_y < 0.0L) {
        return HWA_DSP_NUMERIC_ERROR;
    }

    next_count = stats->count + 1U;
    delta_x = (long double)x - stats->mean_x;
    delta_y = (long double)y - stats->mean_y;
    next_mean_x = stats->mean_x + delta_x / (long double)next_count;
    next_mean_y = stats->mean_y + delta_y / (long double)next_count;
    next_m2_x = stats->m2_x +
                delta_x * ((long double)x - next_mean_x);
    next_m2_y = stats->m2_y +
                delta_y * ((long double)y - next_mean_y);
    next_c2 = stats->c2 +
              delta_x * ((long double)y - next_mean_y);
    if (!isfinite(next_mean_x) || !isfinite(next_mean_y) ||
        !isfinite(next_m2_x) || !isfinite(next_m2_y) ||
        !isfinite(next_c2) || next_m2_x < 0.0L || next_m2_y < 0.0L) {
        return HWA_DSP_NUMERIC_ERROR;
    }

    stats->count = next_count;
    stats->mean_x = next_mean_x;
    stats->mean_y = next_mean_y;
    stats->m2_x = next_m2_x;
    stats->m2_y = next_m2_y;
    stats->c2 = next_c2;
    return HWA_DSP_OK;
}

static int hwa_dsp_covariance_value(const HwaDspRunningCovariance *stats,
                                    uint64_t divisor,
                                    double *covariance)
{
    long double result;

    if (stats == NULL || covariance == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (divisor == 0U || !isfinite(stats->c2)) {
        return HWA_DSP_NO_DATA;
    }
    result = stats->c2 / (long double)divisor;
    if (!isfinite(result) || fabsl(result) > (long double)DBL_MAX) {
        return HWA_DSP_NUMERIC_ERROR;
    }
    *covariance = (double)result;
    return HWA_DSP_OK;
}

int hwa_dsp_covariance_population(const HwaDspRunningCovariance *stats,
                                  double *covariance)
{
    if (stats == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    return hwa_dsp_covariance_value(stats, stats->count, covariance);
}

int hwa_dsp_covariance_sample(const HwaDspRunningCovariance *stats,
                              double *covariance)
{
    if (stats == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (stats->count < 2U) {
        return HWA_DSP_NO_DATA;
    }
    return hwa_dsp_covariance_value(stats, stats->count - 1U, covariance);
}

int hwa_dsp_covariance_correlation(const HwaDspRunningCovariance *stats,
                                   double *correlation)
{
    long double result;
    long double denominator_x;
    long double denominator_y;

    if (stats == NULL || correlation == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (stats->count < 2U || stats->m2_x <= 0.0L || stats->m2_y <= 0.0L) {
        return HWA_DSP_NO_DATA;
    }
    if (!isfinite(stats->c2) || !isfinite(stats->m2_x) ||
        !isfinite(stats->m2_y)) {
        return HWA_DSP_NUMERIC_ERROR;
    }
    denominator_x = sqrtl(stats->m2_x);
    denominator_y = sqrtl(stats->m2_y);
    result = (stats->c2 / denominator_x) / denominator_y;
    if (!isfinite(result)) {
        return HWA_DSP_NUMERIC_ERROR;
    }
    if (result > 1.0L &&
        result < 1.0L + 16.0L * (long double)DBL_EPSILON) {
        result = 1.0L;
    } else if (result < -1.0L &&
               result > -1.0L - 16.0L * (long double)DBL_EPSILON) {
        result = -1.0L;
    }
    if (result < -1.0L || result > 1.0L) {
        return HWA_DSP_NUMERIC_ERROR;
    }
    *correlation = (double)result;
    return HWA_DSP_OK;
}

static int hwa_dsp_biquad_set(HwaDspBiquad *filter,
                              double b0,
                              double b1,
                              double b2,
                              double a0,
                              double a1,
                              double a2)
{
    if (filter == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (!isfinite(b0) || !isfinite(b1) || !isfinite(b2) ||
        !isfinite(a0) || !isfinite(a1) || !isfinite(a2) || a0 == 0.0) {
        return HWA_DSP_NUMERIC_ERROR;
    }
    filter->b0 = b0 / a0;
    filter->b1 = b1 / a0;
    filter->b2 = b2 / a0;
    filter->a1 = a1 / a0;
    filter->a2 = a2 / a0;
    filter->z1 = 0.0;
    filter->z2 = 0.0;
    if (!isfinite(filter->b0) || !isfinite(filter->b1) ||
        !isfinite(filter->b2) || !isfinite(filter->a1) ||
        !isfinite(filter->a2)) {
        return HWA_DSP_NUMERIC_ERROR;
    }
    return HWA_DSP_OK;
}

static int hwa_dsp_biquad_frequency_is_valid(double sample_rate,
                                              double frequency)
{
    return isfinite(sample_rate) && isfinite(frequency) &&
           sample_rate > 0.0 && frequency > 0.0 &&
           frequency < sample_rate * 0.5;
}

int hwa_dsp_biquad_design_highpass(HwaDspBiquad *filter,
                                   double sample_rate,
                                   double frequency,
                                   double q)
{
    double omega;
    double cosine;
    double alpha;

    if (filter == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (!hwa_dsp_biquad_frequency_is_valid(sample_rate, frequency) ||
        !isfinite(q) || q <= 0.0) {
        return HWA_DSP_INVALID_ARGUMENT;
    }

    omega = 2.0 * HWA_DSP_PI * frequency / sample_rate;
    cosine = cos(omega);
    alpha = sin(omega) / (2.0 * q);
    return hwa_dsp_biquad_set(filter,
                              (1.0 + cosine) * 0.5,
                              -(1.0 + cosine),
                              (1.0 + cosine) * 0.5,
                              1.0 + alpha,
                              -2.0 * cosine,
                              1.0 - alpha);
}

int hwa_dsp_biquad_design_highshelf(HwaDspBiquad *filter,
                                    double sample_rate,
                                    double frequency,
                                    double gain_db,
                                    double slope)
{
    double amplitude;
    double omega;
    double cosine;
    double sine;
    double radicand;
    double alpha;
    double root_amplitude;
    double alpha_term;

    if (filter == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (!hwa_dsp_biquad_frequency_is_valid(sample_rate, frequency) ||
        !isfinite(gain_db) || !isfinite(slope) || slope <= 0.0) {
        return HWA_DSP_INVALID_ARGUMENT;
    }

    amplitude = pow(10.0, gain_db / 40.0);
    omega = 2.0 * HWA_DSP_PI * frequency / sample_rate;
    cosine = cos(omega);
    sine = sin(omega);
    if (!isfinite(amplitude) || amplitude <= 0.0) {
        return HWA_DSP_NUMERIC_ERROR;
    }
    radicand = (amplitude + (1.0 / amplitude)) *
               ((1.0 / slope) - 1.0) + 2.0;
    if (!isfinite(radicand) || radicand < 0.0) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    alpha = (sine * 0.5) * sqrt(radicand);
    root_amplitude = sqrt(amplitude);
    alpha_term = 2.0 * root_amplitude * alpha;

    return hwa_dsp_biquad_set(
        filter,
        amplitude * ((amplitude + 1.0) +
                     ((amplitude - 1.0) * cosine) + alpha_term),
        -2.0 * amplitude * ((amplitude - 1.0) +
                            ((amplitude + 1.0) * cosine)),
        amplitude * ((amplitude + 1.0) +
                     ((amplitude - 1.0) * cosine) - alpha_term),
        (amplitude + 1.0) - ((amplitude - 1.0) * cosine) + alpha_term,
        2.0 * ((amplitude - 1.0) - ((amplitude + 1.0) * cosine)),
        (amplitude + 1.0) - ((amplitude - 1.0) * cosine) - alpha_term);
}

int hwa_dsp_biquad_reset(HwaDspBiquad *filter)
{
    if (filter == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (!isfinite(filter->b0) || !isfinite(filter->b1) ||
        !isfinite(filter->b2) || !isfinite(filter->a1) ||
        !isfinite(filter->a2)) {
        return HWA_DSP_NONFINITE;
    }
    filter->z1 = 0.0;
    filter->z2 = 0.0;
    return HWA_DSP_OK;
}

int hwa_dsp_biquad_process_sample(HwaDspBiquad *filter,
                                  double input,
                                  double *output)
{
    double result;
    double next_z1;
    double next_z2;

    if (filter == NULL || output == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (!isfinite(input) || !isfinite(filter->b0) ||
        !isfinite(filter->b1) || !isfinite(filter->b2) ||
        !isfinite(filter->a1) || !isfinite(filter->a2) ||
        !isfinite(filter->z1) || !isfinite(filter->z2)) {
        return HWA_DSP_NONFINITE;
    }

    result = (filter->b0 * input) + filter->z1;
    next_z1 = (filter->b1 * input) - (filter->a1 * result) + filter->z2;
    next_z2 = (filter->b2 * input) - (filter->a2 * result);
    if (!isfinite(result) || !isfinite(next_z1) || !isfinite(next_z2)) {
        return HWA_DSP_NUMERIC_ERROR;
    }
    filter->z1 = next_z1;
    filter->z2 = next_z2;
    *output = result;
    return HWA_DSP_OK;
}

int hwa_dsp_biquad_process(HwaDspBiquad *filter,
                           const double *input,
                           double *output,
                           size_t count)
{
    size_t index;
    int status;

    if (filter == NULL || input == NULL || output == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (count == 0U) {
        return HWA_DSP_BAD_SIZE;
    }
    if (!hwa_dsp_values_are_finite(input, count)) {
        return HWA_DSP_NONFINITE;
    }
    for (index = 0U; index < count; ++index) {
        status = hwa_dsp_biquad_process_sample(filter, input[index],
                                               &output[index]);
        if (status != HWA_DSP_OK) {
            return status;
        }
    }
    return HWA_DSP_OK;
}

int hwa_dsp_resample_linear(const double *input,
                            size_t input_count,
                            double *output,
                            size_t output_count)
{
    size_t index;

    if (input == NULL || output == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (input_count == 0U || output_count == 0U) {
        return HWA_DSP_BAD_SIZE;
    }
    if (!hwa_dsp_values_are_finite(input, input_count)) {
        return HWA_DSP_NONFINITE;
    }
    if (input_count == 1U) {
        for (index = 0U; index < output_count; ++index) {
            output[index] = input[0];
        }
        return HWA_DSP_OK;
    }
    if (output_count == 1U) {
        output[0] = input[0];
        return HWA_DSP_OK;
    }

    for (index = 0U; index < output_count; ++index) {
        double position = ((double)index * (double)(input_count - 1U)) /
                          (double)(output_count - 1U);
        size_t left = (size_t)floor(position);

        if (left >= input_count - 1U) {
            output[index] = input[input_count - 1U];
        } else {
            long double fraction = (long double)position - (long double)left;
            long double value = ((1.0L - fraction) *
                                 (long double)input[left]) +
                                (fraction * (long double)input[left + 1U]);
            if (!isfinite(value) || fabsl(value) > (long double)DBL_MAX) {
                return HWA_DSP_NUMERIC_ERROR;
            }
            output[index] = (double)value;
        }
    }
    return HWA_DSP_OK;
}

int hwa_dsp_true_peak_4x_output_count(size_t input_count,
                                      size_t *output_count)
{
    if (output_count == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (input_count == 0U) {
        return HWA_DSP_BAD_SIZE;
    }
    if (input_count - 1U > (SIZE_MAX - 1U) / HWA_DSP_TRUE_PEAK_FACTOR) {
        return HWA_DSP_BAD_SIZE;
    }
    *output_count = ((input_count - 1U) * HWA_DSP_TRUE_PEAK_FACTOR) + 1U;
    return HWA_DSP_OK;
}

static long double hwa_dsp_sinc(long double value)
{
    long double angle;

    if (fabsl(value) < 8.0L * LDBL_EPSILON) {
        return 1.0L;
    }
    angle = (long double)HWA_DSP_PI * value;
    return sinl(angle) / angle;
}

static long double hwa_dsp_lanczos_weight(long double distance)
{
    if (fabsl(distance) >= (long double)HWA_DSP_TRUE_PEAK_RADIUS) {
        return 0.0L;
    }
    return hwa_dsp_sinc(distance) *
           hwa_dsp_sinc(distance / (long double)HWA_DSP_TRUE_PEAK_RADIUS);
}

static void hwa_dsp_true_peak_weights(
    long double weights[3U][HWA_DSP_TRUE_PEAK_HISTORY])
{
    size_t phase;

    for (phase = 1U; phase < HWA_DSP_TRUE_PEAK_FACTOR; ++phase) {
        int offset;

        for (offset = -HWA_DSP_TRUE_PEAK_RADIUS + 1;
             offset <= HWA_DSP_TRUE_PEAK_RADIUS;
             ++offset) {
            size_t index = (size_t)(offset + HWA_DSP_TRUE_PEAK_RADIUS - 1);
            long double distance =
                (long double)phase /
                    (long double)HWA_DSP_TRUE_PEAK_FACTOR -
                (long double)offset;

            weights[phase - 1U][index] =
                hwa_dsp_lanczos_weight(distance);
        }
    }
}

int hwa_dsp_true_peak_4x(const double *input,
                         size_t input_count,
                         double *workspace,
                         size_t workspace_count,
                         double *peak)
{
    size_t required_count;
    size_t output_index;
    long double weights[3U][HWA_DSP_TRUE_PEAK_HISTORY];
    double maximum = 0.0;
    int status;

    if (input == NULL || workspace == NULL || peak == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    status = hwa_dsp_true_peak_4x_output_count(input_count, &required_count);
    if (status != HWA_DSP_OK) {
        return status;
    }
    if (workspace_count < required_count || input_count > (size_t)PTRDIFF_MAX) {
        return HWA_DSP_BAD_SIZE;
    }
    if (!hwa_dsp_values_are_finite(input, input_count)) {
        return HWA_DSP_NONFINITE;
    }
    hwa_dsp_true_peak_weights(weights);

    for (output_index = 0U; output_index < required_count; ++output_index) {
        size_t phase = output_index % HWA_DSP_TRUE_PEAK_FACTOR;
        size_t base_index = output_index / HWA_DSP_TRUE_PEAK_FACTOR;
        long double value;

        if (phase == 0U) {
            value = (long double)input[base_index];
        } else {
            ptrdiff_t base = (ptrdiff_t)base_index;
            int offset;
            long double weighted_sum = 0.0L;
            long double weight_sum = 0.0L;

            for (offset = -HWA_DSP_TRUE_PEAK_RADIUS + 1;
                 offset <= HWA_DSP_TRUE_PEAK_RADIUS;
                 ++offset) {
                ptrdiff_t sample_index = base + (ptrdiff_t)offset;

                if (sample_index >= 0 &&
                    (size_t)sample_index < input_count) {
                    size_t weight_index = (size_t)(
                        offset + HWA_DSP_TRUE_PEAK_RADIUS - 1);
                    long double weight =
                        weights[phase - 1U][weight_index];
                    weighted_sum += (long double)input[(size_t)sample_index] *
                                    weight;
                    weight_sum += weight;
                }
            }
            if (!isfinite(weighted_sum) || !isfinite(weight_sum) ||
                fabsl(weight_sum) <= LDBL_EPSILON) {
                return HWA_DSP_NUMERIC_ERROR;
            }
            value = weighted_sum / weight_sum;
        }
        if (!isfinite(value) || fabsl(value) > (long double)DBL_MAX) {
            return HWA_DSP_NUMERIC_ERROR;
        }
        workspace[output_index] = (double)value;
        if (fabs(workspace[output_index]) > maximum) {
            maximum = fabs(workspace[output_index]);
        }
    }
    *peak = maximum;
    return HWA_DSP_OK;
}

void hwa_dsp_true_peak_4x_reset(HwaDspTruePeak4x *state)
{
    size_t index;

    if (state != NULL) {
        for (index = 0U; index < HWA_DSP_TRUE_PEAK_HISTORY; ++index) {
            state->history[index] = 0.0;
        }
        hwa_dsp_true_peak_weights(state->weights);
        state->sample_count = 0U;
        state->next_base = 0U;
        state->peak = 0.0;
        state->finished = 0;
    }
}

static int hwa_dsp_true_peak_stream_sample(
    const HwaDspTruePeak4x *state,
    uint64_t sample_index,
    double *sample)
{
    uint64_t age;

    if (sample_index >= state->sample_count) {
        return HWA_DSP_NO_DATA;
    }
    age = state->sample_count - sample_index;
    if (age > (uint64_t)HWA_DSP_TRUE_PEAK_HISTORY) {
        return HWA_DSP_NUMERIC_ERROR;
    }
    *sample = state->history[(size_t)(sample_index %
                                      HWA_DSP_TRUE_PEAK_HISTORY)];
    return HWA_DSP_OK;
}

static int hwa_dsp_true_peak_stream_value(const HwaDspTruePeak4x *state,
                                          uint64_t base,
                                          size_t phase,
                                          double *output)
{
    int offset;
    long double weighted_sum = 0.0L;
    long double weight_sum = 0.0L;

    if (phase == 0U) {
        return hwa_dsp_true_peak_stream_sample(state, base, output);
    }
    for (offset = -HWA_DSP_TRUE_PEAK_RADIUS + 1;
         offset <= HWA_DSP_TRUE_PEAK_RADIUS;
         ++offset) {
        uint64_t sample_index;
        double sample;
        long double weight;
        int status;

        if (offset < 0) {
            uint64_t magnitude = (uint64_t)(-(int64_t)offset);
            if (base < magnitude) {
                continue;
            }
            sample_index = base - magnitude;
        } else {
            uint64_t magnitude = (uint64_t)offset;
            if (base > UINT64_MAX - magnitude) {
                continue;
            }
            sample_index = base + magnitude;
        }
        if (sample_index >= state->sample_count) {
            continue;
        }
        status = hwa_dsp_true_peak_stream_sample(state, sample_index, &sample);
        if (status != HWA_DSP_OK) {
            return status;
        }
        weight = state->weights[phase - 1U][
            (size_t)(offset + HWA_DSP_TRUE_PEAK_RADIUS - 1)];
        weighted_sum += (long double)sample * weight;
        weight_sum += weight;
    }
    if (!isfinite(weighted_sum) || !isfinite(weight_sum) ||
        fabsl(weight_sum) <= LDBL_EPSILON) {
        return HWA_DSP_NUMERIC_ERROR;
    }
    weighted_sum /= weight_sum;
    if (!isfinite(weighted_sum) ||
        fabsl(weighted_sum) > (long double)DBL_MAX) {
        return HWA_DSP_NUMERIC_ERROR;
    }
    *output = (double)weighted_sum;
    return HWA_DSP_OK;
}

static int hwa_dsp_true_peak_stream_interval(HwaDspTruePeak4x *state,
                                             uint64_t base)
{
    size_t phase;

    for (phase = 0U; phase < HWA_DSP_TRUE_PEAK_FACTOR; ++phase) {
        double value;
        int status = hwa_dsp_true_peak_stream_value(state, base, phase,
                                                    &value);
        if (status != HWA_DSP_OK) {
            return status;
        }
        if (fabs(value) > state->peak) {
            state->peak = fabs(value);
        }
    }
    return HWA_DSP_OK;
}

int hwa_dsp_true_peak_4x_push(HwaDspTruePeak4x *state,
                              const double *input,
                              size_t count)
{
    size_t index;

    if (state == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (state->finished != 0 || state->next_base > state->sample_count ||
        !isfinite(state->peak) || state->peak < 0.0) {
        return HWA_DSP_NUMERIC_ERROR;
    }
    if (count == 0U) {
        return HWA_DSP_OK;
    }
    if (input == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (!hwa_dsp_values_are_finite(input, count)) {
        return HWA_DSP_NONFINITE;
    }
    if ((uintmax_t)count >
        (uintmax_t)(UINT64_MAX - state->sample_count)) {
        return HWA_DSP_BAD_SIZE;
    }

    for (index = 0U; index < count; ++index) {
        int status;

        state->history[(size_t)(state->sample_count %
                                HWA_DSP_TRUE_PEAK_HISTORY)] = input[index];
        ++state->sample_count;
        while (state->sample_count >
                   (uint64_t)HWA_DSP_TRUE_PEAK_RADIUS &&
               state->next_base <=
                   state->sample_count -
                       (uint64_t)HWA_DSP_TRUE_PEAK_RADIUS - 1U) {
            status = hwa_dsp_true_peak_stream_interval(state,
                                                       state->next_base);
            if (status != HWA_DSP_OK) {
                return status;
            }
            ++state->next_base;
        }
    }
    return HWA_DSP_OK;
}

int hwa_dsp_true_peak_4x_finish(HwaDspTruePeak4x *state, double *peak)
{
    double last_sample;
    int status;

    if (state == NULL || peak == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (state->sample_count == 0U) {
        return HWA_DSP_NO_DATA;
    }
    if (state->finished != 0) {
        *peak = state->peak;
        return HWA_DSP_OK;
    }
    while (state->next_base + 1U < state->sample_count) {
        status = hwa_dsp_true_peak_stream_interval(state, state->next_base);
        if (status != HWA_DSP_OK) {
            return status;
        }
        ++state->next_base;
    }
    status = hwa_dsp_true_peak_stream_sample(state,
                                             state->sample_count - 1U,
                                             &last_sample);
    if (status != HWA_DSP_OK) {
        return status;
    }
    if (fabs(last_sample) > state->peak) {
        state->peak = fabs(last_sample);
    }
    state->finished = 1;
    *peak = state->peak;
    return HWA_DSP_OK;
}

static int hwa_dsp_delay_candidate(const double *first,
                                   const double *second,
                                   size_t count,
                                   ptrdiff_t lag,
                                   double *correlation,
                                   size_t *overlap)
{
    size_t first_start;
    size_t second_start;
    size_t index;
    HwaDspRunningCovariance stats;
    int status;

    if (lag >= 0) {
        first_start = 0U;
        second_start = (size_t)lag;
    } else {
        first_start = (size_t)(-lag);
        second_start = 0U;
    }
    *overlap = count - (first_start + second_start);
    if (*overlap < 3U) {
        return HWA_DSP_NO_DATA;
    }

    hwa_dsp_covariance_reset(&stats);
    for (index = 0U; index < *overlap; ++index) {
        status = hwa_dsp_covariance_push(&stats,
                                         first[first_start + index],
                                         second[second_start + index]);
        if (status != HWA_DSP_OK) {
            return status;
        }
    }
    return hwa_dsp_covariance_correlation(&stats, correlation);
}

int hwa_dsp_estimate_delay(const double *first,
                           const double *second,
                           size_t count,
                           size_t max_lag,
                           ptrdiff_t *delay,
                           double *correlation)
{
    size_t lag_magnitude;
    double best_correlation = 0.0;
    long double best_score = -1.0L;
    ptrdiff_t best_lag = 0;
    int found = 0;

    if (first == NULL || second == NULL || delay == NULL ||
        correlation == NULL) {
        return HWA_DSP_INVALID_ARGUMENT;
    }
    if (count < 3U || max_lag >= count - 2U ||
        max_lag > (size_t)PTRDIFF_MAX) {
        return HWA_DSP_BAD_SIZE;
    }
    if (!hwa_dsp_values_are_finite(first, count) ||
        !hwa_dsp_values_are_finite(second, count)) {
        return HWA_DSP_NONFINITE;
    }

    for (lag_magnitude = 0U; lag_magnitude <= max_lag; ++lag_magnitude) {
        int sign_count = lag_magnitude == 0U ? 1 : 2;
        int sign_index;

        for (sign_index = 0; sign_index < sign_count; ++sign_index) {
            ptrdiff_t lag = (ptrdiff_t)lag_magnitude;
            double candidate_correlation;
            size_t overlap;
            int status;
            long double score;

            if (sign_index == 1) {
                lag = -lag;
            }
            status = hwa_dsp_delay_candidate(first, second, count, lag,
                                             &candidate_correlation, &overlap);
            if (status == HWA_DSP_NO_DATA) {
                continue;
            }
            if (status != HWA_DSP_OK) {
                return status;
            }
            score = fabsl((long double)candidate_correlation) *
                    ((long double)overlap / (long double)count);
            if (!found || score > best_score) {
                found = 1;
                best_score = score;
                best_correlation = candidate_correlation;
                best_lag = lag;
            }
        }
    }
    if (!found) {
        return HWA_DSP_NO_DATA;
    }
    *delay = best_lag;
    *correlation = best_correlation;
    return HWA_DSP_OK;
}

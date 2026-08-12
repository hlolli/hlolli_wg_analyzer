#include "dsp.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_PI 3.14159265358979323846264338327950288

static int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",               \
                          __FILE__, __LINE__, #condition);                     \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static int close_enough(double actual, double expected, double tolerance)
{
    return isfinite(actual) && fabs(actual - expected) <= tolerance;
}

static void test_hann(void)
{
    double window[5];

    CHECK(hwa_dsp_hann(window, 5U) == HWA_DSP_OK);
    CHECK(close_enough(window[0], 0.0, 0.0));
    CHECK(close_enough(window[1], 0.5, 1.0e-15));
    CHECK(close_enough(window[2], 1.0, 1.0e-15));
    CHECK(close_enough(window[3], 0.5, 1.0e-15));
    CHECK(close_enough(window[4], 0.0, 0.0));
    CHECK(hwa_dsp_hann(window, 1U) == HWA_DSP_BAD_SIZE);
    CHECK(hwa_dsp_hann(NULL, 5U) == HWA_DSP_INVALID_ARGUMENT);
}

static void test_fft_impulse_round_trip(void)
{
    HwaDspComplex values[8];
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        values[index].real = index == 0U ? 1.0 : 0.0;
        values[index].imag = 0.0;
    }
    CHECK(hwa_dsp_fft(values, 8U, 0) == HWA_DSP_OK);
    for (index = 0U; index < 8U; ++index) {
        CHECK(close_enough(values[index].real, 1.0, 1.0e-14));
        CHECK(close_enough(values[index].imag, 0.0, 1.0e-14));
    }
    CHECK(hwa_dsp_fft(values, 8U, 1) == HWA_DSP_OK);
    CHECK(close_enough(values[0].real, 1.0, 1.0e-14));
    for (index = 1U; index < 8U; ++index) {
        CHECK(close_enough(values[index].real, 0.0, 1.0e-14));
        CHECK(close_enough(values[index].imag, 0.0, 1.0e-14));
    }
}

static void test_fft_tone(void)
{
    HwaDspComplex values[16];
    size_t index;

    for (index = 0U; index < 16U; ++index) {
        values[index].real = cos(2.0 * TEST_PI * 3.0 *
                                 (double)index / 16.0);
        values[index].imag = 0.0;
    }
    CHECK(hwa_dsp_fft(values, 16U, 0) == HWA_DSP_OK);
    for (index = 0U; index < 16U; ++index) {
        double expected = (index == 3U || index == 13U) ? 8.0 : 0.0;
        CHECK(close_enough(values[index].real, expected, 2.0e-13));
        CHECK(close_enough(values[index].imag, 0.0, 2.0e-13));
    }
    CHECK(hwa_dsp_fft(values, 6U, 0) == HWA_DSP_BAD_SIZE);
    CHECK(hwa_dsp_fft(values, 16U, 2) == HWA_DSP_INVALID_ARGUMENT);
    values[0].real = NAN;
    CHECK(hwa_dsp_fft(values, 16U, 0) == HWA_DSP_NONFINITE);
}

static void test_fft_general_round_trip(void)
{
    HwaDspComplex values[32];
    HwaDspComplex original[32];
    size_t index;

    for (index = 0U; index < 32U; ++index) {
        values[index].real = sin((double)index * 0.37) +
                             cos((double)index * 0.11) * 0.25;
        values[index].imag = cos((double)index * 0.23) * 0.1;
        original[index] = values[index];
    }
    CHECK(hwa_dsp_fft(values, 32U, 0) == HWA_DSP_OK);
    CHECK(hwa_dsp_fft(values, 32U, 1) == HWA_DSP_OK);
    for (index = 0U; index < 32U; ++index) {
        CHECK(close_enough(values[index].real, original[index].real,
                           1.0e-13));
        CHECK(close_enough(values[index].imag, original[index].imag,
                           1.0e-13));
    }
    CHECK(hwa_dsp_fft(values, 0U, 0) == HWA_DSP_BAD_SIZE);
    CHECK(hwa_dsp_fft(values, 1U, 0) == HWA_DSP_OK);
    CHECK(hwa_dsp_fft(values, 1U, 1) == HWA_DSP_OK);
}

static void test_running_stats(void)
{
    static const double values[] = {1.0, 2.0, 3.0, 4.0};
    HwaDspRunningStats stats;
    HwaDspRunningCovariance covariance;
    double result = 0.0;
    size_t index;

    hwa_dsp_stats_reset(&stats);
    CHECK(hwa_dsp_stats_mean(&stats, &result) == HWA_DSP_NO_DATA);
    for (index = 0U; index < 4U; ++index) {
        CHECK(hwa_dsp_stats_push(&stats, values[index]) == HWA_DSP_OK);
    }
    CHECK(hwa_dsp_stats_mean(&stats, &result) == HWA_DSP_OK);
    CHECK(close_enough(result, 2.5, 1.0e-15));
    CHECK(hwa_dsp_stats_population_variance(&stats, &result) == HWA_DSP_OK);
    CHECK(close_enough(result, 1.25, 1.0e-15));
    CHECK(hwa_dsp_stats_sample_variance(&stats, &result) == HWA_DSP_OK);
    CHECK(close_enough(result, 5.0 / 3.0, 1.0e-15));
    CHECK(hwa_dsp_stats_push(&stats, INFINITY) == HWA_DSP_NONFINITE);
    CHECK(stats.count == 4U);

    hwa_dsp_stats_reset(&stats);
    for (index = 0U; index < 4U; ++index) {
        CHECK(hwa_dsp_stats_push(&stats, 1.0e12 + values[index]) ==
              HWA_DSP_OK);
    }
    CHECK(hwa_dsp_stats_population_variance(&stats, &result) == HWA_DSP_OK);
    CHECK(close_enough(result, 1.25, 1.0e-9));

    hwa_dsp_covariance_reset(&covariance);
    for (index = 0U; index < 4U; ++index) {
        CHECK(hwa_dsp_covariance_push(&covariance, values[index],
                                      values[index] * 2.0) == HWA_DSP_OK);
    }
    CHECK(hwa_dsp_covariance_population(&covariance, &result) == HWA_DSP_OK);
    CHECK(close_enough(result, 2.5, 1.0e-15));
    CHECK(hwa_dsp_covariance_sample(&covariance, &result) == HWA_DSP_OK);
    CHECK(close_enough(result, 10.0 / 3.0, 1.0e-15));
    CHECK(hwa_dsp_covariance_correlation(&covariance, &result) == HWA_DSP_OK);
    CHECK(close_enough(result, 1.0, 1.0e-15));
}

static void test_long_perfect_correlation(void)
{
    HwaDspRunningCovariance covariance;
    uint32_t state = UINT32_C(0x91e10da5);
    double result = 0.0;
    size_t index;

    hwa_dsp_covariance_reset(&covariance);
    for (index = 0U; index < 7993U; ++index) {
        double sample;

        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        sample = ((double)(state & UINT32_C(0xffff)) / 32768.0 - 1.0) * 0.2;
        CHECK(hwa_dsp_covariance_push(&covariance, sample, sample) ==
              HWA_DSP_OK);
    }
    CHECK(hwa_dsp_covariance_correlation(&covariance, &result) == HWA_DSP_OK);
    CHECK(close_enough(result, 1.0, 0.0));
}

static double biquad_magnitude(const HwaDspBiquad *filter, double omega)
{
    double c1 = cos(omega);
    double s1 = -sin(omega);
    double c2 = cos(2.0 * omega);
    double s2 = -sin(2.0 * omega);
    double numerator_real = filter->b0 + filter->b1 * c1 +
                            filter->b2 * c2;
    double numerator_imag = filter->b1 * s1 + filter->b2 * s2;
    double denominator_real = 1.0 + filter->a1 * c1 +
                              filter->a2 * c2;
    double denominator_imag = filter->a1 * s1 + filter->a2 * s2;

    return hypot(numerator_real, numerator_imag) /
           hypot(denominator_real, denominator_imag);
}

static void test_biquads(void)
{
    HwaDspBiquad filter;
    double input[4096];
    double output[4096];
    double sample = 0.0;
    double high_gain = pow(10.0, 4.0 / 20.0);
    size_t index;

    CHECK(hwa_dsp_biquad_design_highpass(&filter, 48000.0, 100.0,
                                         0.7071067811865476) == HWA_DSP_OK);
    CHECK(biquad_magnitude(&filter, 0.0) < 1.0e-10);
    CHECK(biquad_magnitude(&filter, TEST_PI) > 0.999);
    for (index = 0U; index < 4096U; ++index) {
        input[index] = 1.0;
    }
    CHECK(hwa_dsp_biquad_process(&filter, input, output, 4096U) ==
          HWA_DSP_OK);
    CHECK(fabs(output[4095]) < 1.0e-8);
    CHECK(hwa_dsp_biquad_reset(&filter) == HWA_DSP_OK);
    CHECK(hwa_dsp_biquad_process_sample(&filter, 1.0, &sample) ==
          HWA_DSP_OK);
    CHECK(isfinite(sample));

    CHECK(hwa_dsp_biquad_design_highshelf(&filter, 48000.0, 1681.974,
                                          4.0, 1.0) == HWA_DSP_OK);
    CHECK(close_enough(biquad_magnitude(&filter, 0.0), 1.0, 1.0e-10));
    CHECK(close_enough(biquad_magnitude(&filter, TEST_PI), high_gain,
                       1.0e-10));
    CHECK(hwa_dsp_biquad_design_highpass(&filter, 48000.0, 24000.0,
                                         0.7) == HWA_DSP_INVALID_ARGUMENT);
    CHECK(hwa_dsp_biquad_design_highshelf(&filter, 48000.0, 1000.0,
                                          NAN, 1.0) ==
          HWA_DSP_INVALID_ARGUMENT);
}

static void test_linear_resampling(void)
{
    static const double input[] = {0.0, 10.0};
    static const double expected[] = {0.0, 2.5, 5.0, 7.5, 10.0};
    double output[5];
    double held[4];
    double one = -3.25;
    double invalid[] = {0.0, NAN};
    size_t index;

    CHECK(hwa_dsp_resample_linear(input, 2U, output, 5U) == HWA_DSP_OK);
    for (index = 0U; index < 5U; ++index) {
        CHECK(close_enough(output[index], expected[index], 1.0e-15));
    }
    CHECK(hwa_dsp_resample_linear(&one, 1U, held, 4U) == HWA_DSP_OK);
    for (index = 0U; index < 4U; ++index) {
        CHECK(close_enough(held[index], one, 0.0));
    }
    CHECK(hwa_dsp_resample_linear(invalid, 2U, output, 5U) ==
          HWA_DSP_NONFINITE);
}

static void run_streaming_true_peak(const double *input,
                                    size_t count,
                                    size_t block_size,
                                    double *peak)
{
    HwaDspTruePeak4x state;
    size_t offset = 0U;

    hwa_dsp_true_peak_4x_reset(&state);
    while (offset < count) {
        size_t block = count - offset;
        if (block > block_size) {
            block = block_size;
        }
        CHECK(hwa_dsp_true_peak_4x_push(&state, input + offset, block) ==
              HWA_DSP_OK);
        offset += block;
    }
    CHECK(hwa_dsp_true_peak_4x_finish(&state, peak) == HWA_DSP_OK);
}

static void test_true_peak(void)
{
    double input[64];
    double workspace[253];
    size_t required = 0U;
    size_t index;
    double sample_peak = 0.0;
    double whole_peak = 0.0;
    double one_sample_peak = 0.0;
    double seven_sample_peak = 0.0;
    double split_peak = 0.0;
    double constant[31];
    double constant_workspace[121];
    double constant_peak = 0.0;
    size_t block_size;

    for (index = 0U; index < 64U; ++index) {
        input[index] = 0.8 * sin((2.0 * TEST_PI * 0.4 * (double)index) +
                                0.37);
        if (fabs(input[index]) > sample_peak) {
            sample_peak = fabs(input[index]);
        }
    }
    CHECK(hwa_dsp_true_peak_4x_output_count(64U, &required) == HWA_DSP_OK);
    CHECK(required == 253U);
    CHECK(hwa_dsp_true_peak_4x(input, 64U, workspace, 253U,
                               &whole_peak) == HWA_DSP_OK);
    CHECK(whole_peak >= sample_peak);
    CHECK(whole_peak > sample_peak + 0.005);
    CHECK(whole_peak < 0.9);

    run_streaming_true_peak(input, 64U, 1U, &one_sample_peak);
    run_streaming_true_peak(input, 64U, 7U, &seven_sample_peak);
    CHECK(close_enough(one_sample_peak, whole_peak, 1.0e-14));
    CHECK(close_enough(seven_sample_peak, whole_peak, 1.0e-14));
    for (block_size = 1U; block_size <= 64U; ++block_size) {
        run_streaming_true_peak(input, 64U, block_size, &split_peak);
        CHECK(close_enough(split_peak, whole_peak, 1.0e-14));
    }

    for (index = 0U; index < 31U; ++index) {
        constant[index] = -0.625;
    }
    CHECK(hwa_dsp_true_peak_4x(constant, 31U, constant_workspace, 121U,
                               &constant_peak) == HWA_DSP_OK);
    CHECK(close_enough(constant_peak, 0.625, 1.0e-14));
    run_streaming_true_peak(constant, 31U, 9U, &split_peak);
    CHECK(close_enough(split_peak, constant_peak, 1.0e-14));

    CHECK(hwa_dsp_true_peak_4x(input, 1U, workspace, 1U,
                               &split_peak) == HWA_DSP_OK);
    CHECK(close_enough(split_peak, fabs(input[0]), 0.0));

    CHECK(hwa_dsp_true_peak_4x(input, 64U, workspace, 252U,
                               &whole_peak) == HWA_DSP_BAD_SIZE);
}

static void make_test_signal(double *values, size_t count)
{
    uint32_t state = UINT32_C(0x12345678);
    size_t index;

    for (index = 0U; index < count; ++index) {
        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
        values[index] = ((double)(state >> 8U) / 8388608.0) - 1.0;
    }
}

static void test_delay(void)
{
    double first[128];
    double second[128];
    ptrdiff_t delay = 0;
    double correlation = 0.0;
    size_t index;

    make_test_signal(first, 128U);
    for (index = 0U; index < 128U; ++index) {
        second[index] = 0.0;
    }
    for (index = 0U; index + 7U < 128U; ++index) {
        second[index + 7U] = first[index];
    }
    CHECK(hwa_dsp_estimate_delay(first, second, 128U, 16U, &delay,
                                 &correlation) == HWA_DSP_OK);
    CHECK(delay == 7);
    CHECK(close_enough(correlation, 1.0, 1.0e-14));

    CHECK(hwa_dsp_estimate_delay(second, first, 128U, 16U, &delay,
                                 &correlation) == HWA_DSP_OK);
    CHECK(delay == -7);
    CHECK(close_enough(correlation, 1.0, 1.0e-14));

    for (index = 0U; index < 128U; ++index) {
        second[index] = 0.0;
    }
    for (index = 0U; index + 7U < 128U; ++index) {
        second[index + 7U] = -first[index];
    }
    CHECK(hwa_dsp_estimate_delay(first, second, 128U, 16U, &delay,
                                 &correlation) == HWA_DSP_OK);
    CHECK(delay == 7);
    CHECK(close_enough(correlation, -1.0, 1.0e-14));

    for (index = 0U; index < 128U; ++index) {
        second[index] = 1.0;
    }
    CHECK(hwa_dsp_estimate_delay(second, second, 128U, 16U, &delay,
                                 &correlation) == HWA_DSP_NO_DATA);
    CHECK(hwa_dsp_estimate_delay(first, first, 2U, 0U, &delay,
                                 &correlation) == HWA_DSP_BAD_SIZE);
}

int main(void)
{
    test_hann();
    test_fft_impulse_round_trip();
    test_fft_tone();
    test_fft_general_round_trip();
    test_running_stats();
    test_long_perfect_correlation();
    test_biquads();
    test_linear_resampling();
    test_true_peak();
    test_delay();

    if (failures != 0) {
        (void)fprintf(stderr, "%d DSP test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    (void)puts("DSP tests passed");
    return EXIT_SUCCESS;
}

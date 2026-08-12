#ifndef HWA_DSP_H
#define HWA_DSP_H

#include <stddef.h>
#include <stdint.h>

/*
 * These routines allocate no memory. Array functions write only to the
 * output buffers named by the caller. Input and output arrays must not
 * overlap unless a function says otherwise.
 */

typedef enum HwaDspStatus {
    HWA_DSP_OK = 0,
    HWA_DSP_INVALID_ARGUMENT = -1,
    HWA_DSP_BAD_SIZE = -2,
    HWA_DSP_NONFINITE = -3,
    HWA_DSP_NO_DATA = -4,
    HWA_DSP_NUMERIC_ERROR = -5
} HwaDspStatus;

typedef struct HwaDspComplex {
    double real;
    double imag;
} HwaDspComplex;

/* Write a symmetric Hann window. count must be at least two. */
int hwa_dsp_hann(double *window, size_t count);

/*
 * Transform values in place. count must be a non-zero power of two.
 * inverse must be zero or one. The inverse transform includes 1/count gain.
 */
int hwa_dsp_fft(HwaDspComplex *values, size_t count, int inverse);

typedef struct HwaDspRunningStats {
    uint64_t count;
    long double mean;
    long double m2;
} HwaDspRunningStats;

void hwa_dsp_stats_reset(HwaDspRunningStats *stats);
int hwa_dsp_stats_push(HwaDspRunningStats *stats, double value);
int hwa_dsp_stats_mean(const HwaDspRunningStats *stats, double *mean);
int hwa_dsp_stats_population_variance(const HwaDspRunningStats *stats,
                                      double *variance);
int hwa_dsp_stats_sample_variance(const HwaDspRunningStats *stats,
                                  double *variance);

typedef struct HwaDspRunningCovariance {
    uint64_t count;
    long double mean_x;
    long double mean_y;
    long double m2_x;
    long double m2_y;
    long double c2;
} HwaDspRunningCovariance;

void hwa_dsp_covariance_reset(HwaDspRunningCovariance *stats);
int hwa_dsp_covariance_push(HwaDspRunningCovariance *stats,
                            double x,
                            double y);
int hwa_dsp_covariance_population(const HwaDspRunningCovariance *stats,
                                  double *covariance);
int hwa_dsp_covariance_sample(const HwaDspRunningCovariance *stats,
                              double *covariance);
int hwa_dsp_covariance_correlation(const HwaDspRunningCovariance *stats,
                                   double *correlation);

/* Direct-form II transposed biquad. Design calls also clear its state. */
typedef struct HwaDspBiquad {
    double b0;
    double b1;
    double b2;
    double a1;
    double a2;
    double z1;
    double z2;
} HwaDspBiquad;

int hwa_dsp_biquad_design_highpass(HwaDspBiquad *filter,
                                   double sample_rate,
                                   double frequency,
                                   double q);
int hwa_dsp_biquad_design_highshelf(HwaDspBiquad *filter,
                                    double sample_rate,
                                    double frequency,
                                    double gain_db,
                                    double slope);
int hwa_dsp_biquad_reset(HwaDspBiquad *filter);
int hwa_dsp_biquad_process_sample(HwaDspBiquad *filter,
                                  double input,
                                  double *output);
int hwa_dsp_biquad_process(HwaDspBiquad *filter,
                           const double *input,
                           double *output,
                           size_t count);

/*
 * Map the first and last input samples to the first and last output samples.
 * A one-sample input is held across the output.
 */
int hwa_dsp_resample_linear(const double *input,
                            size_t input_count,
                            double *output,
                            size_t output_count);

/*
 * The 4x estimator uses a fixed band-limited interpolator. Ask for its exact
 * workspace size first. workspace receives the oversampled signal and must
 * not overlap input. peak is the greatest absolute oversampled value.
 */
int hwa_dsp_true_peak_4x_output_count(size_t input_count,
                                      size_t *output_count);
int hwa_dsp_true_peak_4x(const double *input,
                         size_t input_count,
                         double *workspace,
                         size_t workspace_count,
                         double *peak);

/*
 * Streaming form of the same estimator. It holds a fixed 24-sample ring and
 * gives the same result for every split of the same sample stream. Call finish
 * once after the last block. A reset permits reuse for another stream.
 */
#define HWA_DSP_TRUE_PEAK_HISTORY 24U
typedef struct HwaDspTruePeak4x {
    double history[HWA_DSP_TRUE_PEAK_HISTORY];
    long double weights[3U][HWA_DSP_TRUE_PEAK_HISTORY];
    uint64_t sample_count;
    uint64_t next_base;
    double peak;
    int finished;
} HwaDspTruePeak4x;

void hwa_dsp_true_peak_4x_reset(HwaDspTruePeak4x *state);
int hwa_dsp_true_peak_4x_push(HwaDspTruePeak4x *state,
                              const double *input,
                              size_t count);
int hwa_dsp_true_peak_4x_finish(HwaDspTruePeak4x *state, double *peak);

/*
 * Find a lag in [-max_lag, max_lag] by normalized correlation. A positive lag
 * means second arrives after first. At least three samples must overlap.
 */
int hwa_dsp_estimate_delay(const double *first,
                           const double *second,
                           size_t count,
                           size_t max_lag,
                           ptrdiff_t *delay,
                           double *correlation);

#endif

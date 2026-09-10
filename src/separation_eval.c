#include "internal.h"

#include <float.h>
#include <math.h>
#include <string.h>

void hwa_separation_eval_options_default(HWASeparationEvalOptions *options)
{
    if (options == NULL) return;
    options->max_frames = UINT64_C(100000000);
    options->max_sample_values = UINT64_C(200000000);
    options->max_input_bytes = UINT64_C(536870912);
    options->max_work_bytes = UINT64_C(268435456);
    options->remove_channel_mean = 1;
}

static HWASeparationRatio ratio(double numerator_log, double denominator_log,
                                 int numerator_zero, int denominator_zero)
{
    HWASeparationRatio value;
    value.db = 0.0;
    value.status = numerator_zero && denominator_zero
        ? HWA_SEPARATION_RATIO_UNDEFINED
        : denominator_zero ? HWA_SEPARATION_RATIO_POSITIVE_INFINITY
        : numerator_zero ? HWA_SEPARATION_RATIO_NEGATIVE_INFINITY
        : HWA_SEPARATION_RATIO_FINITE;
    if (value.status == HWA_SEPARATION_RATIO_FINITE)
        value.db = 10.0 * (numerator_log - denominator_log);
    return value;
}

static void evaluate_pair(const double *reference, const double *estimate,
                           size_t frames, uint32_t channels,
                           const long double *reference_mean,
                           const long double *estimate_mean,
                           double reference_peak, double estimate_peak,
                           HWASeparationPair *pair)
{
    long double target_energy = 0.0L;
    long double estimate_energy = 0.0L;
    long double dot = 0.0L;
    long double residual_energy = 0.0L;
    long double error_energy = 0.0L;
    long double alpha;
    double common_peak;
    double reference_factor;
    double estimate_factor;
    double reference_power_log;
    double estimate_power_log;
    double error_power_log;
    size_t index;
    size_t count = frames * channels;
    for (index = 0U; index < count; ++index) {
        uint32_t channel = (uint32_t)(index % channels);
        long double target = (long double)reference[index] / reference_peak - reference_mean[channel];
        long double predicted = (long double)estimate[index] / estimate_peak - estimate_mean[channel];
        target_energy += target * target;
        estimate_energy += predicted * predicted;
        dot += target * predicted;
    }
    pair->reference_silent = target_energy == 0.0L;
    pair->estimate_silent = estimate_energy == 0.0L;
    common_peak = fmax(pair->reference_silent ? 0.0 : reference_peak,
                        pair->estimate_silent ? 0.0 : estimate_peak);
    if (common_peak == 0.0) common_peak = 1.0;
    reference_factor = pair->reference_silent ? 0.0 : reference_peak / common_peak;
    estimate_factor = pair->estimate_silent ? 0.0 : estimate_peak / common_peak;
    alpha = target_energy > 0.0L ? dot / target_energy : 0.0L;
    for (index = 0U; index < count; ++index) {
        uint32_t channel = (uint32_t)(index % channels);
        long double target = (long double)reference[index] / reference_peak - reference_mean[channel];
        long double predicted = (long double)estimate[index] / estimate_peak - estimate_mean[channel];
        long double residual = predicted - alpha * target;
        long double error = target * reference_factor - predicted * estimate_factor;
        residual_energy += residual * residual;
        error_energy += error * error;
    }
    reference_power_log = target_energy > 0.0L
        ? (double)log10l(target_energy) + 2.0 * log10(reference_peak) - log10((double)count) : 0.0;
    estimate_power_log = estimate_energy > 0.0L
        ? (double)log10l(estimate_energy) + 2.0 * log10(estimate_peak) - log10((double)count) : 0.0;
    error_power_log = error_energy > 0.0L
        ? (double)log10l(error_energy) + 2.0 * log10(common_peak) - log10((double)count) : 0.0;
    pair->reference_level_dbfs = ratio(reference_power_log, 0.0, target_energy == 0.0L, 0);
    pair->estimate_level_dbfs = ratio(estimate_power_log, 0.0, estimate_energy == 0.0L, 0);
    pair->error_level_dbfs = ratio(error_power_log, 0.0, error_energy == 0.0L, 0);
    pair->snr = ratio(reference_power_log, error_power_log,
                      target_energy == 0.0L, error_energy == 0.0L);
    if (target_energy == 0.0L || estimate_energy == 0.0L) {
        pair->si_sdr = ratio(0.0, 0.0, 1, 1);
        return;
    }
    pair->si_sdr = ratio(
        dot != 0.0L ? 2.0 * (double)log10l(fabsl(dot)) - (double)log10l(target_energy) : 0.0,
        residual_energy > 0.0L ? (double)log10l(residual_energy) : 0.0,
        dot == 0.0L, residual_energy == 0.0L);
    if (alpha == 0.0L) {
        pair->projection_gain_valid = 1;
    } else {
        long double gain_log = logl(fabsl(alpha)) + logl(estimate_peak) - logl(reference_peak);
        if (gain_log <= logl(DBL_MAX) && gain_log >= logl(nextafter(0.0, 1.0))) {
            pair->projection_gain = (double)copysignl(expl(gain_log), alpha);
            pair->projection_gain_valid = 1;
        }
    }
}

int hwa_evaluate_separation_samples(const double *reference,
                                     const double *estimate,
                                     const double *mixture,
                                     size_t frames, uint32_t channels,
                                     const HWASeparationEvalOptions *options,
                                     HWASeparationEvaluation *result,
                                     char *error, size_t error_size)
{
    HWASeparationEvalOptions defaults;
    HWASeparationEvalOptions checked;
    const double *signals[3] = {reference, estimate, mixture};
    long double means[3][HWA_MAX_CHANNELS];
    double peaks[3] = {0.0, 0.0, 0.0};
    size_t count;
    size_t index;
    unsigned signal;
    unsigned signal_count = mixture == NULL ? 2U : 3U;
    hwa_separation_eval_options_default(&defaults);
    checked = options == NULL ? defaults : *options;
    if (result == NULL) {
        hwa_set_error(error, error_size, "separation evaluation needs a result");
        return -1;
    }
    memset(result, 0, sizeof(*result));
    memset(means, 0, sizeof(means));
    if (reference == NULL || estimate == NULL || frames == 0U ||
        channels == 0U || channels > HWA_MAX_CHANNELS ||
        frames > SIZE_MAX / channels ||
        (uint64_t)frames > checked.max_frames ||
        checked.max_sample_values == 0U ||
        frames * channels > checked.max_sample_values / signal_count ||
        (checked.remove_channel_mean != 0 && checked.remove_channel_mean != 1)) {
        hwa_set_error(error, error_size, "invalid or over-limit separation sample layout");
        return -1;
    }
    count = frames * channels;
    for (signal = 0U; signal < signal_count; ++signal) {
        for (index = 0U; index < count; ++index) {
            double value = signals[signal][index];
            if (!isfinite(value)) {
                hwa_set_error(error, error_size, "separation samples must be finite");
                return -1;
            }
            peaks[signal] = fmax(peaks[signal], fabs(value));
        }
        if (peaks[signal] == 0.0) peaks[signal] = 1.0;
        if (checked.remove_channel_mean) {
            uint32_t channel;
            for (channel = 0U; channel < channels; ++channel) {
                long double sum = 0.0L;
                for (index = channel; index < count; index += channels)
                    sum += (long double)signals[signal][index] / peaks[signal];
                means[signal][channel] = sum / (long double)frames;
            }
        }
    }
    result->options = checked;
    result->frames = frames;
    result->channels = channels;
    result->mixture_present = mixture != NULL;
    evaluate_pair(reference, estimate, frames, channels, means[0], means[1],
                   peaks[0], peaks[1], &result->estimate);
    if (mixture != NULL) {
        evaluate_pair(reference, mixture, frames, channels, means[0], means[2],
                       peaks[0], peaks[2], &result->mixture);
        if (result->estimate.si_sdr.status == HWA_SEPARATION_RATIO_FINITE &&
            result->mixture.si_sdr.status == HWA_SEPARATION_RATIO_FINITE) {
            result->si_sdr_improvement_db = result->estimate.si_sdr.db - result->mixture.si_sdr.db;
            result->si_sdr_improvement_valid = 1;
        }
    }
    return 0;
}

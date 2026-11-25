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

typedef struct SeparationScale {
    double peak;
    double origin[HWA_MAX_CHANNELS];
    long double mean[HWA_MAX_CHANNELS];
} SeparationScale;

static long double scaled_sample(double value, uint32_t channel,
                                  const SeparationScale *scale)
{
    return ((long double)value - scale->origin[channel]) / scale->peak;
}

static int prepare_scale(const double *samples, size_t count, size_t frames,
                         uint32_t channels, int remove_mean, SeparationScale *scale)
{
    double low[HWA_MAX_CHANNELS];
    double high[HWA_MAX_CHANNELS];
    size_t index;
    uint32_t channel;
    memset(scale, 0, sizeof(*scale));
    for (channel = 0U; channel < channels; ++channel) {
        low[channel] = DBL_MAX;
        high[channel] = -DBL_MAX;
    }
    for (index = 0U; index < count; ++index) {
        double value = samples[index];
        if (!isfinite(value)) return -1;
        channel = (uint32_t)(index % channels);
        if (value < low[channel]) low[channel] = value;
        if (value > high[channel]) high[channel] = value;
    }
    for (channel = 0U; channel < channels; ++channel) {
        double origin = 0.0;
        if (remove_mean) {
            /* Subtract the endpoint nearest zero for single-sign channels.
             * The difference cannot overflow and constant offsets vanish. */
            if (low[channel] > 0.0) origin = low[channel];
            else if (high[channel] < 0.0) origin = high[channel];
        }
        scale->origin[channel] = origin;
        scale->peak = fmax(scale->peak, fmax(high[channel] - origin, origin - low[channel]));
    }
    if (scale->peak == 0.0) scale->peak = 1.0;
    if (remove_mean) {
        for (index = 0U; index < count; ++index) {
            channel = (uint32_t)(index % channels);
            scale->mean[channel] += scaled_sample(samples[index], channel, scale);
        }
        for (channel = 0U; channel < channels; ++channel) {
            scale->mean[channel] /= (long double)frames;
        }
    }
    return 0;
}

static void evaluate_pair(const double *reference, const double *estimate,
                          size_t count, uint32_t channels,
                          const SeparationScale *reference_scale,
                          const SeparationScale *estimate_scale,
                          HWASeparationPair *pair)
{
    long double target_energy = 0.0L;
    long double estimate_energy = 0.0L;
    long double dot = 0.0L;
    long double residual_energy = 0.0L;
    long double error_energy = 0.0L;
    long double alpha;
    double reference_peak = reference_scale->peak;
    double estimate_peak = estimate_scale->peak;
    double common_peak;
    double reference_factor;
    double estimate_factor;
    double reference_power_log;
    double estimate_power_log;
    double error_power_log;
    size_t index;
    for (index = 0U; index < count; ++index) {
        uint32_t channel = (uint32_t)(index % channels);
        long double target = scaled_sample(reference[index], channel, reference_scale) - reference_scale->mean[channel];
        long double predicted = scaled_sample(estimate[index], channel, estimate_scale) - estimate_scale->mean[channel];
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
        long double target = scaled_sample(reference[index], channel, reference_scale) - reference_scale->mean[channel];
        long double predicted = scaled_sample(estimate[index], channel, estimate_scale) - estimate_scale->mean[channel];
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
    SeparationScale scales[3];
    size_t count;
    unsigned signal;
    unsigned signal_count = mixture == NULL ? 2U : 3U;
    hwa_separation_eval_options_default(&defaults);
    checked = options == NULL ? defaults : *options;
    if (result == NULL) {
        hwa_set_error(error, error_size, "separation evaluation needs a result");
        return -1;
    }
    memset(result, 0, sizeof(*result));
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
        if (prepare_scale(signals[signal], count, frames, channels,
                          checked.remove_channel_mean, &scales[signal]) != 0) {
            hwa_set_error(error, error_size, "separation samples must be finite");
            return -1;
        }
    }
    result->options = checked;
    result->frames = frames;
    result->channels = channels;
    result->mixture_present = mixture != NULL;
    evaluate_pair(reference, estimate, count, channels, &scales[0], &scales[1],
                  &result->estimate);
    if (mixture != NULL) {
        evaluate_pair(reference, mixture, count, channels, &scales[0], &scales[2],
                      &result->mixture);
        if (result->estimate.si_sdr.status == HWA_SEPARATION_RATIO_FINITE &&
            result->mixture.si_sdr.status == HWA_SEPARATION_RATIO_FINITE) {
            result->si_sdr_improvement_db = result->estimate.si_sdr.db - result->mixture.si_sdr.db;
            result->si_sdr_improvement_valid = 1;
        }
    }
    return 0;
}

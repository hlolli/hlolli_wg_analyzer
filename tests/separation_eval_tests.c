#include "hlolli_wg_analyzer.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    ++failures; } } while (0)

int main(void)
{
    const double reference[4] = {1.0, -1.0, 1.0, -1.0};
    const double mixture[4] = {2.0, 0.0, 0.0, -2.0};
    const double estimate[4] = {1.5, -0.5, 0.5, -1.5};
    const double scaled[4] = {-3.0, 3.0, -3.0, 3.0};
    const double orthogonal[4] = {1.0, 1.0, -1.0, -1.0};
    const double silent[4] = {0.0, 0.0, 0.0, 0.0};
    const double constant[4] = {2.0, 2.0, 2.0, 2.0};
    const double nonfinite[4] = {0.0, INFINITY, 0.0, 0.0};
    double shifted[4];
    double tiny[4];
    double huge[4];
    HWASeparationEvaluation result;
    HWASeparationEvalOptions options;
    char error[HWA_ERROR_SIZE];
    size_t index;
    hwa_separation_eval_options_default(&options);
    CHECK(hwa_evaluate_separation_samples(reference, estimate, mixture,
        4U, 1U, &options, &result, error, sizeof(error)) == 0);
    CHECK(result.estimate.si_sdr.status == HWA_SEPARATION_RATIO_FINITE);
    CHECK(fabs(result.estimate.si_sdr.db - 10.0 * log10(4.0)) < 1e-12);
    CHECK(fabs(result.mixture.si_sdr.db) < 1e-12);
    CHECK(result.si_sdr_improvement_valid &&
        fabs(result.si_sdr_improvement_db - 10.0 * log10(4.0)) < 1e-12);
    CHECK(result.estimate.projection_gain_valid &&
        fabs(result.estimate.projection_gain - 1.0) < 1e-12);
    CHECK(result.estimate.reference_level_dbfs.status == HWA_SEPARATION_RATIO_FINITE &&
        fabs(result.estimate.reference_level_dbfs.db) < 1e-12);
    CHECK(fabs(result.estimate.estimate_level_dbfs.db - 10.0 * log10(1.25)) < 1e-12);
    CHECK(fabs(result.estimate.error_level_dbfs.db - 10.0 * log10(0.25)) < 1e-12);
    CHECK(fabs(result.mixture.estimate_level_dbfs.db - 10.0 * log10(2.0)) < 1e-12);
    CHECK(hwa_evaluate_separation_samples(reference, scaled, NULL,
        4U, 1U, &result.options, &result, error, sizeof(error)) == 0);
    CHECK(result.estimate.si_sdr.status == HWA_SEPARATION_RATIO_POSITIVE_INFINITY);
    CHECK(result.estimate.snr.status == HWA_SEPARATION_RATIO_FINITE &&
        fabs(result.estimate.snr.db + 10.0 * log10(16.0)) < 1e-12);
    CHECK(fabs(result.estimate.projection_gain + 3.0) < 1e-12);
    CHECK(!result.mixture_present && !result.si_sdr_improvement_valid);
    CHECK(hwa_evaluate_separation_samples(reference, orthogonal, NULL,
        4U, 1U, NULL, &result, error, sizeof(error)) == 0);
    CHECK(result.estimate.si_sdr.status == HWA_SEPARATION_RATIO_NEGATIVE_INFINITY);
    CHECK(hwa_evaluate_separation_samples(reference, silent, NULL,
        4U, 1U, NULL, &result, error, sizeof(error)) == 0);
    CHECK(result.estimate.estimate_silent && !result.estimate.reference_silent);
    CHECK(result.estimate.snr.status == HWA_SEPARATION_RATIO_FINITE &&
        result.estimate.snr.db == 0.0);
    CHECK(result.estimate.si_sdr.status == HWA_SEPARATION_RATIO_UNDEFINED);
    CHECK(result.estimate.estimate_level_dbfs.status == HWA_SEPARATION_RATIO_NEGATIVE_INFINITY);
    CHECK(result.estimate.error_level_dbfs.status == HWA_SEPARATION_RATIO_FINITE &&
        fabs(result.estimate.error_level_dbfs.db) < 1e-12);
    CHECK(hwa_evaluate_separation_samples(silent, estimate, NULL,
        4U, 1U, NULL, &result, error, sizeof(error)) == 0);
    CHECK(result.estimate.reference_silent && !result.estimate.estimate_silent);
    CHECK(result.estimate.reference_level_dbfs.status == HWA_SEPARATION_RATIO_NEGATIVE_INFINITY);
    CHECK(fabs(result.estimate.error_level_dbfs.db - 10.0 * log10(1.25)) < 1e-12);
    CHECK(result.estimate.error_level_dbfs.db == result.estimate.estimate_level_dbfs.db);
    CHECK(hwa_evaluate_separation_samples(constant, constant, NULL,
        4U, 1U, NULL, &result, error, sizeof(error)) == 0);
    CHECK(result.estimate.reference_silent && result.estimate.estimate_silent);
    CHECK(result.estimate.snr.status == HWA_SEPARATION_RATIO_UNDEFINED);
    CHECK(result.estimate.reference_level_dbfs.status == HWA_SEPARATION_RATIO_NEGATIVE_INFINITY &&
        result.estimate.estimate_level_dbfs.status == HWA_SEPARATION_RATIO_NEGATIVE_INFINITY &&
        result.estimate.error_level_dbfs.status == HWA_SEPARATION_RATIO_NEGATIVE_INFINITY);
    options.remove_channel_mean = 0;
    CHECK(hwa_evaluate_separation_samples(constant, constant, NULL,
        4U, 1U, &options, &result, error, sizeof(error)) == 0);
    CHECK(!result.estimate.reference_silent &&
        result.estimate.si_sdr.status == HWA_SEPARATION_RATIO_POSITIVE_INFINITY);
    CHECK(fabs(result.estimate.reference_level_dbfs.db - 20.0 * log10(2.0)) < 1e-12);
    CHECK(result.estimate.error_level_dbfs.status == HWA_SEPARATION_RATIO_NEGATIVE_INFINITY);
    hwa_separation_eval_options_default(&options);
    for (index = 0U; index < 4U; ++index) {
        shifted[index] = estimate[index] + 100.0;
        tiny[index] = reference[index] * 1e-300;
        huge[index] = estimate[index] * 1e300;
    }
    CHECK(hwa_evaluate_separation_samples(reference, shifted, NULL,
        4U, 1U, &options, &result, error, sizeof(error)) == 0);
    CHECK(fabs(result.estimate.si_sdr.db - 10.0 * log10(4.0)) < 1e-9);
    CHECK(fabs(result.estimate.estimate_level_dbfs.db - 10.0 * log10(1.25)) < 1e-12);
    CHECK(hwa_evaluate_separation_samples(tiny, huge, NULL,
        4U, 1U, &options, &result, error, sizeof(error)) == 0);
    CHECK(fabs(result.estimate.si_sdr.db - 10.0 * log10(4.0)) < 1e-12);
    CHECK(!result.estimate.projection_gain_valid);
    CHECK(result.estimate.snr.status == HWA_SEPARATION_RATIO_FINITE &&
        fabs(result.estimate.snr.db + 12000.0 + 10.0 * log10(1.25)) < 1e-9);
    CHECK(fabs(result.estimate.reference_level_dbfs.db + 6000.0) < 1e-9);
    CHECK(fabs(result.estimate.estimate_level_dbfs.db - 6000.0 - 10.0 * log10(1.25)) < 1e-9);
    CHECK(hwa_evaluate_separation_samples(tiny, silent, NULL,
        4U, 1U, &options, &result, error, sizeof(error)) == 0);
    CHECK(result.estimate.snr.status == HWA_SEPARATION_RATIO_FINITE &&
        fabs(result.estimate.snr.db) < 1e-12);
    CHECK(fabs(result.estimate.error_level_dbfs.db + 6000.0) < 1e-9);
    CHECK(hwa_evaluate_separation_samples(tiny, constant, NULL,
        4U, 1U, &options, &result, error, sizeof(error)) == 0);
    CHECK(result.estimate.snr.status == HWA_SEPARATION_RATIO_FINITE &&
        fabs(result.estimate.snr.db) < 1e-12);
    CHECK(hwa_evaluate_separation_samples(silent, tiny, NULL,
        4U, 1U, &options, &result, error, sizeof(error)) == 0);
    CHECK(fabs(result.estimate.error_level_dbfs.db + 6000.0) < 1e-9);
    {
        const double high[2] = {DBL_MAX, -DBL_MAX};
        const double inverse[2] = {-DBL_MAX, DBL_MAX};
        CHECK(hwa_evaluate_separation_samples(high, inverse, NULL,
            2U, 1U, &options, &result, error, sizeof(error)) == 0);
        CHECK(result.estimate.error_level_dbfs.status == HWA_SEPARATION_RATIO_FINITE);
        CHECK(fabs(result.estimate.error_level_dbfs.db -
            20.0 * (log10(DBL_MAX) + log10(2.0))) < 1e-9);
    }
    {
        const double stereo[8] = {1, 1, -1, -1, 1, 1, -1, -1};
        const double unbalanced[8] = {1, 2, -1, -2, 1, 2, -1, -2};
        CHECK(hwa_evaluate_separation_samples(stereo, unbalanced, NULL,
            4U, 2U, &options, &result, error, sizeof(error)) == 0);
        CHECK(fabs(result.estimate.si_sdr.db - 10.0 * log10(9.0)) < 1e-12);
        CHECK(fabs(result.estimate.estimate_level_dbfs.db - 10.0 * log10(2.5)) < 1e-12);
        CHECK(fabs(result.estimate.error_level_dbfs.db - 10.0 * log10(0.5)) < 1e-12);
    }
    options.max_sample_values = 11U;
    CHECK(hwa_evaluate_separation_samples(reference, estimate, mixture,
        4U, 1U, &options, &result, error, sizeof(error)) != 0);
    CHECK(result.frames == 0U && result.channels == 0U);
    options.max_sample_values = 12U;
    CHECK(hwa_evaluate_separation_samples(reference, estimate, mixture,
        4U, 1U, &options, &result, error, sizeof(error)) == 0);
    CHECK(hwa_evaluate_separation_samples(reference, nonfinite, NULL,
        4U, 1U, &options, &result, error, sizeof(error)) != 0);
    CHECK(result.frames == 0U);
    CHECK(hwa_evaluate_separation_samples(reference, estimate, NULL,
        0U, 1U, NULL, &result, error, sizeof(error)) != 0);
    CHECK(hwa_evaluate_separation_samples(reference, estimate, NULL,
        4U, 0U, NULL, &result, error, sizeof(error)) != 0);
    if (failures) return 1;
    puts("separation evaluation tests passed");
    return 0;
}

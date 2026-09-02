#include "harmonic_decay_report.h"

#include "numeric_locale.h"
#include "output.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>

static int hwa_hd_json_number(FILE *stream, double value)
{
    if (!isfinite(value)) {
        return fputs("null", stream) == EOF ? -1 : 0;
    }
    return fprintf(stream, "%.17g", value == 0.0 ? 0.0 : value) < 0
               ? -1 : 0;
}

static int hwa_hd_json_rejections(FILE *stream, uint32_t mask)
{
    static const struct HWAHDRejectionName {
        uint32_t flag;
        const char *name;
    } names[] = {
        {HWA_HARMONIC_DECAY_REJECT_NO_ONSET, "no-onset"},
        {HWA_HARMONIC_DECAY_REJECT_LATE_PULSE, "late-pulse"},
        {HWA_HARMONIC_DECAY_REJECT_LOW_ANCHOR_SNR, "low-anchor-snr"},
        {HWA_HARMONIC_DECAY_REJECT_BAND_OUT_OF_RANGE,
         "band-out-of-range"},
        {HWA_HARMONIC_DECAY_REJECT_LOW_SUPPORT, "low-support"},
        {HWA_HARMONIC_DECAY_REJECT_LOW_DYNAMIC_RANGE,
         "low-dynamic-range"},
        {HWA_HARMONIC_DECAY_REJECT_NON_DECAY, "non-decay"},
        {HWA_HARMONIC_DECAY_REJECT_HIGH_RESIDUAL, "high-residual"},
        {HWA_HARMONIC_DECAY_REJECT_TRUNCATED_FIT, "truncated-fit"},
        {HWA_HARMONIC_DECAY_REJECT_LOW_HARMONIC_COVERAGE,
         "low-harmonic-coverage"}
    };
    size_t index;
    int wrote = 0;

    if (fputc('[', stream) == EOF) return -1;
    for (index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
        if ((mask & names[index].flag) == 0U) continue;
        if ((wrote && fputc(',', stream) == EOF) ||
            hwa_json_write_string(stream, names[index].name) != 0) {
            return -1;
        }
        wrote = 1;
    }
    return fputc(']', stream) == EOF ? -1 : 0;
}

static int hwa_hd_json_format(FILE *stream, const HWAFormat *format)
{
    if (fputs("{\"container\":", stream) == EOF ||
        hwa_json_write_string(stream,
                              hwa_container_name(format->container)) != 0 ||
        fputs(",\"encoding\":", stream) == EOF ||
        hwa_json_write_string(stream,
                              hwa_encoding_name(format->encoding)) != 0 ||
        fprintf(stream,
                ",\"channels\":%u,\"sample_rate_hz\":%" PRIu32
                ",\"bits_per_sample\":%u,\"valid_bits_per_sample\":%u,"
                "\"block_align\":%u,\"channel_mask\":%" PRIu32
                ",\"frames\":%" PRIu64 ",\"data_bytes\":%" PRIu64
                ",\"duration_seconds\":",
                (unsigned)format->channels, format->sample_rate_hz,
                (unsigned)format->bits_per_sample,
                (unsigned)format->valid_bits_per_sample,
                (unsigned)format->block_align, format->channel_mask,
                format->frames, format->data_bytes) < 0 ||
        hwa_hd_json_number(stream, format->duration_seconds) != 0 ||
        fputc('}', stream) == EOF) {
        return -1;
    }
    return 0;
}

static int hwa_hd_json_band(FILE *stream,
                            const HWAHarmonicDecayBand *band)
{
    if (fprintf(stream,
                "{\"harmonic_number\":%" PRIu32 ",\"target_hz\":",
                band->harmonic_number) < 0 ||
        hwa_hd_json_number(stream, band->target_hz) != 0 ||
        fputs(",\"selected_hz\":", stream) == EOF ||
        hwa_hd_json_number(stream, band->selected_hz) != 0 ||
        fprintf(stream,
                ",\"selected_bin\":%zu,\"signal_first_bin\":%zu,"
                "\"signal_last_bin\":%zu,\"lower_noise_first_bin\":%zu,"
                "\"lower_noise_last_bin\":%zu,"
                "\"upper_noise_first_bin\":%zu,"
                "\"upper_noise_last_bin\":%zu,\"anchor_snr_db\":",
                band->selected_bin, band->signal_first_bin,
                band->signal_last_bin, band->lower_noise_first_bin,
                band->lower_noise_last_bin, band->upper_noise_first_bin,
                band->upper_noise_last_bin) < 0 ||
        hwa_hd_json_number(stream, band->anchor_snr_db) != 0 ||
        fputs(",\"slope_db_per_second\":", stream) == EOF ||
        hwa_hd_json_number(stream, band->slope_db_per_second) != 0 ||
        fputs(",\"t60_seconds\":", stream) == EOF ||
        hwa_hd_json_number(stream, band->t60_seconds) != 0 ||
        fputs(",\"support_seconds\":", stream) == EOF ||
        hwa_hd_json_number(stream, band->support_seconds) != 0 ||
        fputs(",\"fit_dynamic_range_db\":", stream) == EOF ||
        hwa_hd_json_number(stream, band->fit_dynamic_range_db) != 0 ||
        fputs(",\"residual_db\":", stream) == EOF ||
        hwa_hd_json_number(stream, band->residual_db) != 0 ||
        fprintf(stream,
                ",\"fit_point_count\":%" PRIu64
                ",\"fit_start_sample\":%" PRIu64
                ",\"fit_end_sample\":%" PRIu64
                ",\"tail_boundary_sample\":%" PRIu64
                ",\"valid\":%s,\"rejection_mask\":%" PRIu32
                ",\"rejections\":",
                band->fit_point_count, band->fit_start_sample,
                band->fit_end_sample, band->tail_boundary_sample,
                band->valid ? "true" : "false", band->rejection_mask) < 0 ||
        hwa_hd_json_rejections(stream, band->rejection_mask) != 0 ||
        fputc('}', stream) == EOF) {
        return -1;
    }
    return 0;
}

static int hwa_hd_json_profile(FILE *stream,
                               const HWAHarmonicDecayProfile *profile)
{
    size_t index;

    if (fputs("{\"path\":", stream) == EOF ||
        hwa_json_write_string(stream,
                              profile->path != NULL ? profile->path : "") != 0 ||
        fputs(",\"format\":", stream) == EOF ||
        hwa_hd_json_format(stream, &profile->format) != 0 ||
        fprintf(stream,
                ",\"fft_size\":%zu,\"hop_samples\":%zu,"
                "\"onset_sample\":%" PRIu64
                ",\"broad_peak_sample\":%" PRIu64
                ",\"analysis_start_sample\":%" PRIu64
                ",\"analysis_end_sample\":%" PRIu64
                ",\"band_count\":%zu,\"valid_band_count\":%zu,"
                "\"valid\":%s,\"rejection_mask\":%" PRIu32
                ",\"rejections\":",
                profile->fft_size, profile->hop_samples,
                profile->onset_sample, profile->broad_peak_sample,
                profile->analysis_start_sample, profile->analysis_end_sample,
                profile->band_count, profile->valid_band_count,
                profile->valid ? "true" : "false",
                profile->rejection_mask) < 0 ||
        hwa_hd_json_rejections(stream, profile->rejection_mask) != 0 ||
        fputs(",\"bands\":[", stream) == EOF) {
        return -1;
    }
    for (index = 0U; index < profile->band_count; ++index) {
        if ((index != 0U && fputc(',', stream) == EOF) ||
            hwa_hd_json_band(stream, &profile->bands[index]) != 0) {
            return -1;
        }
    }
    return fprintf(stream,
                   "],\"work\":{\"peak_bytes\":%" PRIu64
                   ",\"evaluations\":%" PRIu64 "}}",
                   profile->peak_work_bytes,
                   profile->evaluation_count) < 0 ? -1 : 0;
}

static int hwa_hd_json_comparison(FILE *stream,
                                  const HWAHarmonicDecayResult *result)
{
    size_t index;

    if (fprintf(stream,
                "{\"valid\":%s,\"shared_valid_band_count\":%zu,"
                "\"shared_reference_coverage\":",
                result->comparison_valid ? "true" : "false",
                result->shared_valid_band_count) < 0 ||
        hwa_hd_json_number(stream, result->shared_reference_coverage) != 0 ||
        fputs(",\"t60_log_rmse_db\":", stream) == EOF ||
        hwa_hd_json_number(stream, result->t60_log_rmse_db) != 0 ||
        fputs(",\"median_t60_log_bias_db\":", stream) == EOF ||
        hwa_hd_json_number(stream, result->median_t60_log_bias_db) != 0 ||
        fprintf(stream, ",\"band_count\":%zu,\"bands\":[",
                result->comparison_count) < 0) {
        return -1;
    }
    for (index = 0U; index < result->comparison_count; ++index) {
        const HWAHarmonicDecayComparison *comparison =
            &result->comparisons[index];
        if (index != 0U && fputc(',', stream) == EOF) return -1;
        if (fprintf(stream,
                    "{\"harmonic_number\":%" PRIu32
                    ",\"reference_valid\":%s,\"model_valid\":%s,"
                    "\"valid\":%s,\"t60_log_error_db\":",
                    comparison->harmonic_number,
                    comparison->reference_valid ? "true" : "false",
                    comparison->model_valid ? "true" : "false",
                    comparison->valid ? "true" : "false") < 0 ||
            hwa_hd_json_number(stream,
                               comparison->t60_log_error_db) != 0 ||
            fputc('}', stream) == EOF) {
            return -1;
        }
    }
    return fputs("]}", stream) == EOF ? -1 : 0;
}

int hwa_harmonic_decay_report_json(
    FILE *stream,
    const HWAHarmonicDecayResult *result)
{
    HWANumericLocale locale;
    int status = -1;

    if (stream == NULL || result == NULL ||
        hwa_c_numeric_locale_begin(&locale) != 0) {
        return -1;
    }
    if (fputs("{\"schema\":\"hwa-harmonic-decay\","
              "\"schema_version\":1,\"command\":\"harmonic-decay\","
              "\"method\":\"" HWA_HARMONIC_DECAY_METHOD_VERSION
              "\",\"expected_hz\":",
              stream) == EOF ||
        hwa_hd_json_number(stream, result->expected_hz) != 0 ||
        fputs(",\"options\":{\"expected_hz\":", stream) == EOF ||
        hwa_hd_json_number(stream, result->options.expected_hz) != 0 ||
        fprintf(stream,
                ",\"decode_block_frames\":%zu,"
                "\"max_input_bytes\":%" PRIu64
                ",\"max_input_frames\":%" PRIu64
                ",\"max_work_bytes\":%" PRIu64
                ",\"max_evaluations\":%" PRIu64 "},\"reference\":",
                result->options.decode_block_frames,
                result->options.max_input_bytes,
                result->options.max_input_frames,
                result->options.max_work_bytes,
                result->options.max_evaluations) < 0 ||
        hwa_hd_json_profile(stream, &result->reference) != 0 ||
        fputs(",\"model\":", stream) == EOF) {
        goto cleanup;
    }
    if (result->model_present) {
        if (hwa_hd_json_profile(stream, &result->model) != 0 ||
            fputs(",\"comparison\":", stream) == EOF ||
            hwa_hd_json_comparison(stream, result) != 0) {
            goto cleanup;
        }
    } else if (fputs("null,\"comparison\":null", stream) == EOF) {
        goto cleanup;
    }
    if (fprintf(stream,
                ",\"work\":{\"peak_bytes\":%" PRIu64
                ",\"evaluations\":%" PRIu64 "}}\n",
                result->peak_work_bytes,
                result->evaluation_count) < 0) {
        goto cleanup;
    }
    status = 0;

cleanup:
    if (hwa_c_numeric_locale_end(&locale) != 0) status = -1;
    return status;
}

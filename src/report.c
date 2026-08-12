#include "report.h"

#include "output.h"

#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>

static int hwa_put_json_number(FILE *stream, double value)
{
    if (!isfinite(value)) {
        return fputs("null", stream) == EOF ? -1 : 0;
    }
    if (value == 0.0) {
        return fputc('0', stream) == EOF ? -1 : 0;
    }
    return fprintf(stream, "%.17g", value) < 0 ? -1 : 0;
}

static int hwa_put_json_u64_delta(FILE *stream,
                                  uint64_t reference,
                                  uint64_t model)
{
    if (model >= reference) {
        return fprintf(stream, "%" PRIu64, model - reference) < 0 ? -1 : 0;
    }
    return fprintf(stream, "-%" PRIu64, reference - model) < 0 ? -1 : 0;
}

static int hwa_put_json_optional_number(FILE *stream,
                                        double value,
                                        int valid)
{
    return valid ? hwa_put_json_number(stream, value)
                 : (fputs("null", stream) == EOF ? -1 : 0);
}

static int hwa_put_csv_number(FILE *stream, double value)
{
    if (!isfinite(value)) {
        return 0;
    }
    return fprintf(stream, "%.17g", value) < 0 ? -1 : 0;
}

static const char *hwa_channel_mode_name(HWAChannelMode mode)
{
    switch (mode) {
    case HWA_CHANNEL_KEEP:
        return "keep";
    case HWA_CHANNEL_SELECT:
        return "select";
    case HWA_CHANNEL_MIX:
        return "mix";
    default:
        return "unknown";
    }
}

static const char *hwa_json_encoding_name(HWAEncoding encoding)
{
    return encoding == HWA_ENCODING_PCM ? "pcm" : "ieee_float";
}

static const char *hwa_json_container_name(HWAContainer container)
{
    return container == HWA_CONTAINER_RF64 ? "rf64" : "riff";
}

static int hwa_json_band_values(FILE *stream,
                                const double *values,
                                uint16_t valid_mask)
{
    size_t band;

    if (fputc('[', stream) == EOF) {
        return -1;
    }
    for (band = 0U; band < HWA_BAND_COUNT; ++band) {
        if ((band != 0U && fputc(',', stream) == EOF) ||
            fputs("{\"name\":", stream) == EOF ||
            hwa_json_write_string(stream, hwa_band_name(band)) != 0 ||
            fputs(",\"value\":", stream) == EOF ||
            hwa_put_json_optional_number(
                stream, values[band],
                (valid_mask & ((uint16_t)1U << band)) != 0U) != 0 ||
            fputc('}', stream) == EOF) {
            return -1;
        }
    }
    return fputc(']', stream) == EOF ? -1 : 0;
}

static int hwa_json_channel(FILE *stream,
                            const HWAChannelMetrics *metrics,
                            uint16_t index)
{
    if (fprintf(stream, "{\"index\":%u,\"peak\":", (unsigned)index + 1U) < 0 ||
        hwa_put_json_number(stream, metrics->peak) != 0 ||
        fputs(",\"true_peak\":", stream) == EOF ||
        (metrics->true_peak_valid
             ? hwa_put_json_number(stream, metrics->true_peak)
             : (fputs("null", stream) == EOF ? -1 : 0)) != 0 ||
        fputs(",\"rms\":", stream) == EOF ||
        hwa_put_json_number(stream, metrics->rms) != 0 ||
        fputs(",\"dc_offset\":", stream) == EOF ||
        hwa_put_json_number(stream, metrics->dc_offset) != 0 ||
        fputs(",\"crest_factor\":", stream) == EOF ||
        (metrics->crest_factor_valid
             ? hwa_put_json_number(stream, metrics->crest_factor)
             : (fputs("null", stream) == EOF ? -1 : 0)) != 0 ||
        fputs(",\"zero_crossing_rate\":", stream) == EOF ||
        hwa_put_json_number(stream, metrics->zero_crossing_rate) != 0 ||
        fprintf(stream, ",\"clipped_samples\":%" PRIu64 "}",
                metrics->clipped_samples) < 0) {
        return -1;
    }
    return 0;
}

static int hwa_json_options(FILE *stream, const HWAAnalysis *analysis)
{
    const HWAAnalysisOptions *options = &analysis->options;

    if (fputs("{\"channel_mode\":", stream) == EOF ||
        hwa_json_write_string(stream,
                              hwa_channel_mode_name(options->channel_mode)) != 0 ||
        fprintf(stream,
                ",\"selected_channel\":%u,\"decode_block_frames\":%zu"
                ",\"frame_size\":%zu,\"hop_size\":%zu"
                ",\"silence_threshold_dbfs\":",
                (unsigned)options->selected_channel,
                options->decode_block_frames,
                options->frame_size,
                options->hop_size) < 0 ||
        hwa_put_json_number(stream, options->silence_threshold_dbfs) != 0 ||
        fprintf(stream,
                ",\"max_input_bytes\":%" PRIu64
                ",\"max_input_frames\":%" PRIu64
                ",\"max_work_bytes\":%" PRIu64
                ",\"max_transforms\":%zu,\"max_track_points\":%zu"
                ",\"max_spectrum_values\":%zu,\"max_lag_samples\":%zu"
                ",\"true_peak_oversample\":%u}",
                options->max_input_bytes,
                options->max_input_frames,
                options->max_work_bytes,
                options->max_transforms,
                options->max_track_points,
                options->max_spectrum_values,
                options->max_lag_samples,
                options->true_peak_oversample) < 0) {
        return -1;
    }
    return 0;
}

int hwa_report_analysis_json(FILE *stream, const HWAAnalysis *analysis)
{
    uint16_t channel;

    if (stream == NULL || analysis == NULL ||
        fputs("{\"path\":", stream) == EOF ||
        hwa_json_write_string(stream, analysis->path) != 0 ||
        fputs(",\"path_encoding\":"
              "\"utf8_with_invalid_bytes_as_u00xx\",\"path_bytes_hex\":",
              stream) == EOF ||
        hwa_json_write_byte_hex(stream, analysis->path) != 0 ||
        fputs(",\"method_version\":", stream) == EOF ||
        hwa_json_write_string(stream, HWA_METHOD_VERSION) != 0 ||
        fputs(",\"format\":{\"container\":", stream) == EOF ||
        hwa_json_write_string(
            stream, hwa_json_container_name(analysis->format.container)) != 0 ||
        fputs(",\"encoding\":", stream) == EOF ||
        hwa_json_write_string(
            stream, hwa_json_encoding_name(analysis->format.encoding)) != 0 ||
        fprintf(stream,
                ",\"channels\":%u,\"sample_rate_hz\":%" PRIu32
                ",\"bits_per_sample\":%u,\"valid_bits_per_sample\":%u"
                ",\"block_align\":%u,\"channel_mask\":%" PRIu32
                ",\"frames\":%" PRIu64 ",\"data_bytes\":%" PRIu64
                ",\"duration_seconds\":",
                (unsigned)analysis->format.channels,
                analysis->format.sample_rate_hz,
                (unsigned)analysis->format.bits_per_sample,
                (unsigned)analysis->format.valid_bits_per_sample,
                (unsigned)analysis->format.block_align,
                analysis->format.channel_mask,
                analysis->format.frames,
                analysis->format.data_bytes) < 0 ||
        hwa_put_json_number(stream, analysis->format.duration_seconds) != 0 ||
        fputs("},\"options\":", stream) == EOF ||
        hwa_json_options(stream, analysis) != 0 ||
        fputs(",\"true_peak_method\":", stream) == EOF ||
        hwa_json_write_string(
            stream,
            analysis->options.true_peak_oversample == 4U
                ? "4x 24-tap Lanczos windowed-sinc estimate"
                : "sample peak (1x)") != 0 ||
        fprintf(stream, ",\"analyzed_channels\":%u,\"channels\":[",
                (unsigned)analysis->analyzed_channels) < 0) {
        return -1;
    }
    for (channel = 0U; channel < analysis->analyzed_channels; ++channel) {
        if ((channel != 0U && fputc(',', stream) == EOF) ||
            hwa_json_channel(stream, &analysis->channels[channel], channel) != 0) {
            return -1;
        }
    }
    if (fputs("],\"loudness\":{\"method\":"
              "\"BS.1770-4 and EBU R128 style; RBJ K-weighting, 0.1 LU bins\",\"integrated_lufs\":",
              stream) == EOF ||
        (analysis->loudness.integrated_valid
             ? hwa_put_json_number(stream, analysis->loudness.integrated_lufs)
             : (fputs("null", stream) == EOF ? -1 : 0)) != 0 ||
        fputs(",\"loudness_range_lu\":", stream) == EOF ||
        (analysis->loudness.range_valid
             ? hwa_put_json_number(stream,
                                   analysis->loudness.loudness_range_lu)
             : (fputs("null", stream) == EOF ? -1 : 0)) != 0 ||
        fputs(",\"momentary_max_lufs\":", stream) == EOF ||
        hwa_put_json_optional_number(
            stream, analysis->loudness.momentary_max_lufs,
            analysis->loudness.momentary_valid) != 0 ||
        fputs(",\"short_term_max_lufs\":", stream) == EOF ||
        hwa_put_json_optional_number(
            stream, analysis->loudness.short_term_max_lufs,
            analysis->loudness.short_term_valid) != 0 ||
        fprintf(stream,
                ",\"blocks_above_absolute_gate\":%" PRIu64
                ",\"blocks_above_relative_gate\":%" PRIu64 "}",
                analysis->loudness.blocks_above_absolute_gate,
                analysis->loudness.blocks_above_relative_gate) < 0 ||
        fputs(",\"spectrum\":{\"centroid_hz\":", stream) == EOF ||
        hwa_put_json_optional_number(stream, analysis->spectrum.centroid_hz,
                                     analysis->spectrum.valid) != 0 ||
        fputs(",\"spread_hz\":", stream) == EOF ||
        hwa_put_json_optional_number(stream, analysis->spectrum.spread_hz,
                                     analysis->spectrum.valid) != 0 ||
        fputs(",\"rolloff_85_hz\":", stream) == EOF ||
        hwa_put_json_optional_number(stream, analysis->spectrum.rolloff_85_hz,
                                     analysis->spectrum.valid) != 0 ||
        fputs(",\"flatness\":", stream) == EOF ||
        hwa_put_json_optional_number(stream, analysis->spectrum.flatness,
                                     analysis->spectrum.valid) != 0 ||
        fputs(",\"slope_db_per_octave\":", stream) == EOF ||
        hwa_put_json_optional_number(
            stream, analysis->spectrum.slope_db_per_octave,
            analysis->spectrum.valid) != 0 ||
        fputs(",\"mean_flux\":", stream) == EOF ||
        hwa_put_json_optional_number(stream, analysis->spectrum.mean_flux,
                                     analysis->spectrum.valid) != 0 ||
        fprintf(stream, ",\"transform_count\":%" PRIu64
                        ",\"band_power\":",
                analysis->spectrum.transform_count) < 0 ||
        hwa_json_band_values(
            stream, analysis->spectrum.band_power,
            analysis->spectrum.valid
                ? (uint16_t)(((uint16_t)1U << HWA_BAND_COUNT) - 1U)
                : 0U) != 0 ||
        fputs("},\"activity\":{\"threshold_dbfs\":", stream) == EOF ||
        hwa_put_json_number(stream, analysis->activity.threshold_dbfs) != 0 ||
        fputs(",\"silence_fraction\":", stream) == EOF ||
        hwa_put_json_optional_number(
            stream, analysis->activity.silence_fraction,
            analysis->activity.classified_valid) != 0 ||
        fputs(",\"active_start_seconds\":", stream) == EOF ||
        (analysis->activity.active_span_valid
             ? hwa_put_json_number(stream,
                                   analysis->activity.active_start_seconds)
             : (fputs("null", stream) == EOF ? -1 : 0)) != 0 ||
        fputs(",\"active_end_seconds\":", stream) == EOF ||
        (analysis->activity.active_span_valid
             ? hwa_put_json_number(stream,
                                   analysis->activity.active_end_seconds)
             : (fputs("null", stream) == EOF ? -1 : 0)) != 0 ||
        fputs("},\"stereo\":", stream) == EOF) {
        return -1;
    }
    if (!analysis->stereo.available) {
        if (fputs("null", stream) == EOF) {
            return -1;
        }
    } else if (fputs("{\"correlation\":", stream) == EOF ||
               (analysis->stereo.correlation_valid
                    ? hwa_put_json_number(stream,
                                          analysis->stereo.correlation)
                    : (fputs("null", stream) == EOF ? -1 : 0)) != 0 ||
               fputs(",\"mid_rms\":", stream) == EOF ||
               hwa_put_json_optional_number(
                   stream, analysis->stereo.mid_rms,
                   analysis->stereo.level_valid) != 0 ||
               fputs(",\"side_rms\":", stream) == EOF ||
               hwa_put_json_optional_number(
                   stream, analysis->stereo.side_rms,
                   analysis->stereo.level_valid) != 0 ||
               fputs(",\"balance_db\":", stream) == EOF ||
               hwa_put_json_optional_number(
                   stream, analysis->stereo.balance_db,
                   analysis->stereo.level_valid) != 0 ||
               fputs(",\"width_ratio\":", stream) == EOF ||
               hwa_put_json_optional_number(
                   stream, analysis->stereo.width_ratio,
                   analysis->stereo.width_valid) != 0 ||
               fputs(",\"interchannel_delay_samples\":", stream) == EOF ||
               (analysis->stereo.delay_valid
                    ? hwa_put_json_number(
                          stream, analysis->stereo.interchannel_delay_samples)
                    : (fputs("null", stream) == EOF ? -1 : 0)) != 0 ||
               fputs(",\"interchannel_delay_seconds\":", stream) == EOF ||
               (analysis->stereo.delay_valid
                    ? hwa_put_json_number(
                          stream, analysis->stereo.interchannel_delay_seconds)
                    : (fputs("null", stream) == EOF ? -1 : 0)) != 0 ||
               fputs(",\"interchannel_delay_confidence\":", stream) == EOF ||
               (analysis->stereo.delay_valid
                    ? hwa_put_json_number(
                          stream, analysis->stereo.interchannel_delay_confidence)
                    : (fputs("null", stream) == EOF ? -1 : 0)) != 0 ||
               fputs(",\"band_width\":", stream) == EOF ||
               hwa_json_band_values(
                   stream, analysis->stereo.band_width,
                   analysis->stereo.band_width_valid_mask) != 0 ||
               fputc('}', stream) == EOF) {
        return -1;
    }
    return fputc('}', stream) == EOF ? -1 : 0;
}

static int hwa_text_number(FILE *stream, double value)
{
    return isfinite(value)
               ? (fprintf(stream, "%.9f", value == 0.0 ? 0.0 : value) < 0
                      ? -1
                      : 0)
               : (fputs("n/a", stream) == EOF ? -1 : 0);
}

static int hwa_text_optional_number(FILE *stream, double value, int valid)
{
    return valid ? hwa_text_number(stream, value)
                 : (fputs("n/a", stream) == EOF ? -1 : 0);
}

static int hwa_text_path(FILE *stream, const char *path)
{
    const unsigned char *byte = (const unsigned char *)path;

    while (*byte != 0U) {
        if (*byte >= 0x20U && *byte <= 0x7eU && *byte != (unsigned char)'\\') {
            if (fputc((int)*byte, stream) == EOF) {
                return -1;
            }
        } else if (*byte == (unsigned char)'\\') {
            if (fputs("\\\\", stream) == EOF) {
                return -1;
            }
        } else if (fprintf(stream, "\\x%02x", (unsigned)*byte) < 0) {
            return -1;
        }
        ++byte;
    }
    return 0;
}

int hwa_report_analysis_text(FILE *stream, const HWAAnalysis *analysis)
{
    uint16_t channel;
    size_t band;

    if (stream == NULL || analysis == NULL ||
        fputs("File: ", stream) == EOF ||
        hwa_text_path(stream, analysis->path) != 0 ||
        fprintf(stream,
                "\nContainer: %s\nEncoding: %s\n"
                "Channels: %u (analyzed: %u)\nSample rate: %" PRIu32
                " Hz\nBits per sample: %u (valid: %u)\nFrames: %" PRIu64
                "\nDuration: %.9f s\nData bytes: %" PRIu64
                "\nTrue-peak method: %s\n",
                hwa_container_name(analysis->format.container),
                hwa_encoding_name(analysis->format.encoding),
                (unsigned)analysis->format.channels,
                (unsigned)analysis->analyzed_channels,
                analysis->format.sample_rate_hz,
                (unsigned)analysis->format.bits_per_sample,
                (unsigned)analysis->format.valid_bits_per_sample,
                analysis->format.frames,
                analysis->format.duration_seconds,
                analysis->format.data_bytes,
                analysis->options.true_peak_oversample == 4U
                    ? "4x 24-tap Lanczos windowed-sinc estimate"
                    : "sample peak (1x)") < 0) {
        return -1;
    }
    for (channel = 0U; channel < analysis->analyzed_channels; ++channel) {
        const HWAChannelMetrics *metrics = &analysis->channels[channel];
        if (fprintf(stream, "Channel %u:\n  Peak: ", (unsigned)channel + 1U) < 0 ||
            hwa_text_number(stream, metrics->peak) != 0 ||
            fputs("\n  True peak: ", stream) == EOF ||
            hwa_text_optional_number(stream, metrics->true_peak,
                                     metrics->true_peak_valid) != 0 ||
            fputs("\n  RMS: ", stream) == EOF ||
            hwa_text_number(stream, metrics->rms) != 0 ||
            fputs("\n  DC offset: ", stream) == EOF ||
            hwa_text_number(stream, metrics->dc_offset) != 0 ||
            fputs("\n  Crest factor: ", stream) == EOF ||
            (metrics->crest_factor_valid
                 ? hwa_text_number(stream, metrics->crest_factor)
                 : (fputs("n/a", stream) == EOF ? -1 : 0)) != 0 ||
            fputs("\n  Zero-crossing rate: ", stream) == EOF ||
            hwa_text_number(stream, metrics->zero_crossing_rate) != 0 ||
            fprintf(stream, "\n  Clipped samples: %" PRIu64 "\n",
                    metrics->clipped_samples) < 0) {
            return -1;
        }
    }
    if (fputs("Loudness (BS.1770-4 and EBU R128 style):\n  Integrated: ",
              stream) == EOF ||
        (analysis->loudness.integrated_valid
             ? hwa_text_number(stream, analysis->loudness.integrated_lufs)
             : (fputs("n/a", stream) == EOF ? -1 : 0)) != 0 ||
        fputs(" LUFS\n  Range: ", stream) == EOF ||
        (analysis->loudness.range_valid
             ? hwa_text_number(stream, analysis->loudness.loudness_range_lu)
             : (fputs("n/a", stream) == EOF ? -1 : 0)) != 0 ||
        fputs(" LU\n  Momentary max: ", stream) == EOF ||
        hwa_text_optional_number(stream,
                                 analysis->loudness.momentary_max_lufs,
                                 analysis->loudness.momentary_valid) != 0 ||
        fputs(" LUFS\n  Short-term max: ", stream) == EOF ||
        hwa_text_optional_number(stream,
                                 analysis->loudness.short_term_max_lufs,
                                 analysis->loudness.short_term_valid) != 0 ||
        fputs(" LUFS\nSpectrum:\n  Centroid: ", stream) == EOF ||
        hwa_text_optional_number(stream, analysis->spectrum.centroid_hz,
                                 analysis->spectrum.valid) != 0 ||
        fputs(" Hz\n  Spread: ", stream) == EOF ||
        hwa_text_optional_number(stream, analysis->spectrum.spread_hz,
                                 analysis->spectrum.valid) != 0 ||
        fputs(" Hz\n  85% rolloff: ", stream) == EOF ||
        hwa_text_optional_number(stream, analysis->spectrum.rolloff_85_hz,
                                 analysis->spectrum.valid) != 0 ||
        fputs(" Hz\n  Flatness: ", stream) == EOF ||
        hwa_text_optional_number(stream, analysis->spectrum.flatness,
                                 analysis->spectrum.valid) != 0 ||
        fputs("\n  Slope: ", stream) == EOF ||
        hwa_text_optional_number(stream,
                                 analysis->spectrum.slope_db_per_octave,
                                 analysis->spectrum.valid) != 0 ||
        fputs(" dB/octave", stream) == EOF ||
        fputs("\n  Mean flux: ", stream) == EOF ||
        hwa_text_optional_number(stream, analysis->spectrum.mean_flux,
                                 analysis->spectrum.valid) != 0 ||
        fputs("\n  Bands:\n", stream) == EOF) {
        return -1;
    }
    for (band = 0U; band < HWA_BAND_COUNT; ++band) {
        if (fprintf(stream, "    %s: ", hwa_band_name(band)) < 0 ||
            hwa_text_optional_number(stream,
                                     analysis->spectrum.band_power[band],
                                     analysis->spectrum.valid) != 0 ||
            fputs("\n", stream) == EOF) {
            return -1;
        }
    }
    if (fputs("Activity:\n  Silence: ", stream) == EOF ||
        hwa_text_optional_number(stream, analysis->activity.silence_fraction,
                                 analysis->activity.classified_valid) != 0 ||
        fputs("\n", stream) == EOF) {
        return -1;
    }
    if (fputs("  Active span: ", stream) == EOF) {
        return -1;
    }
    if (analysis->activity.active_span_valid) {
        if (hwa_text_number(stream,
                            analysis->activity.active_start_seconds) != 0 ||
            fputs(" to ", stream) == EOF ||
            hwa_text_number(stream,
                            analysis->activity.active_end_seconds) != 0 ||
            fputs(" s\n", stream) == EOF) {
            return -1;
        }
    } else if (fputs("n/a\n", stream) == EOF) {
        return -1;
    }
    if (analysis->stereo.available &&
        (fputs("Stereo:\n  Correlation: ", stream) == EOF ||
         hwa_text_optional_number(stream, analysis->stereo.correlation,
                                  analysis->stereo.correlation_valid) != 0 ||
         fputs("\n  Balance: ", stream) == EOF ||
         hwa_text_optional_number(stream, analysis->stereo.balance_db,
                                  analysis->stereo.level_valid) != 0 ||
         fputs(" dB\n  Width ratio: ", stream) == EOF ||
         hwa_text_optional_number(stream, analysis->stereo.width_ratio,
                                  analysis->stereo.width_valid) != 0 ||
         fputs("\n  Delay: ", stream) == EOF ||
         (analysis->stereo.delay_valid
              ? hwa_text_number(stream,
                                analysis->stereo.interchannel_delay_samples)
              : (fputs("n/a", stream) == EOF ? -1 : 0)) != 0 ||
         fputs(" samples\n", stream) == EOF)) {
        return -1;
    }
    return 0;
}

static int hwa_json_delta_number(FILE *stream,
                                 const char *name,
                                 double reference,
                                 double model,
                                 int valid,
                                 int comma)
{
    if ((comma && fputc(',', stream) == EOF) ||
        hwa_json_write_string(stream, name) != 0 ||
        fputc(':', stream) == EOF ||
        hwa_put_json_optional_number(stream, model - reference, valid) != 0) {
        return -1;
    }
    return 0;
}

int hwa_report_compare_json(FILE *stream,
                            const HWAAnalysis *reference,
                            const HWAAnalysis *model)
{
    size_t band;

    if (stream == NULL || reference == NULL || model == NULL ||
        fputs("{\"duration_seconds\":", stream) == EOF ||
        hwa_put_json_number(stream, model->format.duration_seconds -
                                        reference->format.duration_seconds) != 0 ||
        fputs(",\"frames\":", stream) == EOF ||
        hwa_put_json_u64_delta(stream, reference->format.frames,
                              model->format.frames) != 0 ||
        hwa_json_delta_number(stream, "integrated_lufs",
                              reference->loudness.integrated_lufs,
                              model->loudness.integrated_lufs,
                              reference->loudness.integrated_valid &&
                                  model->loudness.integrated_valid,
                              1) != 0 ||
        hwa_json_delta_number(stream, "spectral_centroid_hz",
                              reference->spectrum.centroid_hz,
                              model->spectrum.centroid_hz,
                              reference->spectrum.valid &&
                                  model->spectrum.valid,
                              1) != 0 ||
        hwa_json_delta_number(stream, "spectral_rolloff_85_hz",
                              reference->spectrum.rolloff_85_hz,
                              model->spectrum.rolloff_85_hz,
                              reference->spectrum.valid &&
                                  model->spectrum.valid,
                              1) != 0 ||
        hwa_json_delta_number(stream, "spectral_flatness",
                              reference->spectrum.flatness,
                              model->spectrum.flatness,
                              reference->spectrum.valid &&
                                  model->spectrum.valid,
                              1) != 0 ||
        hwa_json_delta_number(stream, "silence_fraction",
                              reference->activity.silence_fraction,
                              model->activity.silence_fraction,
                              reference->activity.classified_valid &&
                                  model->activity.classified_valid,
                              1) != 0 ||
        hwa_json_delta_number(stream, "stereo_correlation",
                              reference->stereo.correlation,
                              model->stereo.correlation,
                              reference->stereo.available &&
                                  model->stereo.available &&
                                  reference->stereo.correlation_valid &&
                                  model->stereo.correlation_valid,
                              1) != 0 ||
        hwa_json_delta_number(stream, "stereo_width_ratio",
                              reference->stereo.width_ratio,
                              model->stereo.width_ratio,
                              reference->stereo.available &&
                                  model->stereo.available &&
                                  reference->stereo.width_valid &&
                                  model->stereo.width_valid,
                              1) != 0 ||
        fputs(",\"band_power\":[", stream) == EOF) {
        return -1;
    }
    for (band = 0U; band < HWA_BAND_COUNT; ++band) {
        if ((band != 0U && fputc(',', stream) == EOF) ||
            fputs("{\"name\":", stream) == EOF ||
            hwa_json_write_string(stream, hwa_band_name(band)) != 0 ||
            fputs(",\"value\":", stream) == EOF ||
            hwa_put_json_optional_number(
                stream,
                model->spectrum.band_power[band] -
                    reference->spectrum.band_power[band],
                reference->spectrum.valid && model->spectrum.valid) != 0 ||
            fputc('}', stream) == EOF) {
            return -1;
        }
    }
    return fputs("]}", stream) == EOF ? -1 : 0;
}

int hwa_report_compare_text(FILE *stream,
                            const HWAAnalysis *reference,
                            const HWAAnalysis *model)
{
    if (stream == NULL || reference == NULL || model == NULL ||
        fputs("Delta (model - reference)\n  Duration: ", stream) == EOF ||
        hwa_text_number(stream, model->format.duration_seconds -
                                    reference->format.duration_seconds) != 0 ||
        fputs(" s\n  Integrated loudness: ", stream) == EOF ||
        hwa_text_optional_number(
            stream,
            model->loudness.integrated_lufs -
                reference->loudness.integrated_lufs,
            reference->loudness.integrated_valid &&
                model->loudness.integrated_valid) != 0 ||
        fputs(" LU\n  Spectral centroid: ", stream) == EOF ||
        hwa_text_optional_number(
            stream,
            model->spectrum.centroid_hz -
                reference->spectrum.centroid_hz,
            reference->spectrum.valid && model->spectrum.valid) != 0 ||
        fputs(" Hz\n  85% rolloff: ", stream) == EOF ||
        hwa_text_optional_number(
            stream,
            model->spectrum.rolloff_85_hz -
                reference->spectrum.rolloff_85_hz,
            reference->spectrum.valid && model->spectrum.valid) != 0 ||
        fputs(" Hz\n  Silence fraction: ", stream) == EOF ||
        hwa_text_optional_number(
            stream,
            model->activity.silence_fraction -
                reference->activity.silence_fraction,
            reference->activity.classified_valid &&
                model->activity.classified_valid) != 0 ||
        fputs("\n  Stereo width ratio: ", stream) == EOF ||
        hwa_text_optional_number(
            stream,
            model->stereo.width_ratio - reference->stereo.width_ratio,
            reference->stereo.available && model->stereo.available &&
                reference->stereo.width_valid && model->stereo.width_valid) != 0 ||
        fputc('\n', stream) == EOF) {
        return -1;
    }
    return 0;
}

int hwa_report_frames_csv(FILE *stream, const HWAAnalysis *analysis)
{
    size_t frame;
    size_t band;

    if (stream == NULL || analysis == NULL ||
        fputs("time_seconds,rms_dbfs,frame_lufs,pitch_hz,"
              "pitch_confidence,onset_strength,spectral_centroid_hz,"
              "spectral_rolloff_85_hz,spectral_flatness",
              stream) == EOF) {
        return -1;
    }
    for (band = 0U; band < HWA_BAND_COUNT; ++band) {
        if (fprintf(stream, ",band_%zu_db", band) < 0) {
            return -1;
        }
    }
    if (fputc('\n', stream) == EOF) {
        return -1;
    }
    for (frame = 0U; frame < analysis->track_count; ++frame) {
        const HWAFrameMetrics *item = &analysis->tracks[frame];
        if (hwa_put_csv_number(stream, item->time_seconds) != 0 ||
            fputc(',', stream) == EOF ||
            hwa_put_csv_number(stream, item->rms_dbfs) != 0 ||
            fputc(',', stream) == EOF ||
            (item->loudness_valid
                 ? hwa_put_csv_number(stream, item->frame_lufs)
                 : 0) != 0 ||
            fputc(',', stream) == EOF ||
            (item->pitch_valid ? hwa_put_csv_number(stream, item->pitch_hz) : 0) !=
                0 ||
            fputc(',', stream) == EOF ||
            (item->pitch_valid
                 ? hwa_put_csv_number(stream, item->pitch_confidence)
                 : 0) != 0 ||
            fputc(',', stream) == EOF ||
            hwa_put_csv_number(stream, item->onset_strength) != 0 ||
            fputc(',', stream) == EOF ||
            (item->spectrum_valid
                 ? hwa_put_csv_number(stream, item->spectral_centroid_hz)
                 : 0) != 0 ||
            fputc(',', stream) == EOF ||
            (item->spectrum_valid
                 ? hwa_put_csv_number(stream, item->spectral_rolloff_85_hz)
                 : 0) != 0 ||
            fputc(',', stream) == EOF ||
            (item->spectrum_valid
                 ? hwa_put_csv_number(stream, item->spectral_flatness)
                 : 0) != 0) {
            return -1;
        }
        for (band = 0U; band < HWA_BAND_COUNT; ++band) {
            if (fputc(',', stream) == EOF ||
                (item->spectrum_valid
                     ? hwa_put_csv_number(stream, item->band_power_db[band])
                     : 0) != 0) {
                return -1;
            }
        }
        if (fputc('\n', stream) == EOF) {
            return -1;
        }
    }
    return 0;
}

int hwa_report_spectrogram_csv(FILE *stream, const HWAAnalysis *analysis)
{
    size_t frame;
    size_t bin;
    double bin_hz;

    if (stream == NULL || analysis == NULL ||
        (analysis->track_count != 0U && analysis->spectrogram_db == NULL) ||
        analysis->spectrum_bins == 0U ||
        fputs("time_seconds,frequency_hz,power_db\n", stream) == EOF) {
        return -1;
    }
    bin_hz = (double)analysis->format.sample_rate_hz /
             (double)analysis->options.frame_size;
    for (frame = 0U; frame < analysis->track_count; ++frame) {
        for (bin = 0U; bin < analysis->spectrum_bins; ++bin) {
            if (hwa_put_csv_number(stream,
                                   analysis->tracks[frame].time_seconds) != 0 ||
                fputc(',', stream) == EOF ||
                hwa_put_csv_number(stream, (double)bin * bin_hz) != 0 ||
                fputc(',', stream) == EOF ||
                hwa_put_csv_number(
                    stream,
                    analysis->spectrogram_db[
                        frame * analysis->spectrum_bins + bin]) != 0 ||
                fputc('\n', stream) == EOF) {
                return -1;
            }
        }
    }
    return 0;
}

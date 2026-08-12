#include "internal.h"

#include "features.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static char *hwa_copy_string(const char *source)
{
    size_t length = strlen(source);
    char *copy;

    if (length == SIZE_MAX) {
        return NULL;
    }
    copy = (char *)malloc(length + 1U);
    if (copy != NULL) {
        memcpy(copy, source, length + 1U);
    }
    return copy;
}

static int hwa_is_power_of_two(size_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

void hwa_analysis_options_default(HWAAnalysisOptions *options)
{
    if (options == NULL) {
        return;
    }
    memset(options, 0, sizeof(*options));
    options->channel_mode = HWA_CHANNEL_KEEP;
    options->decode_block_frames = 4096U;
    options->frame_size = 2048U;
    options->hop_size = 512U;
    options->silence_threshold_dbfs = -60.0;
    options->max_input_bytes = UINT64_C(17179869184);
    options->max_input_frames = UINT64_C(2000000000);
    options->max_work_bytes = UINT64_C(268435456);
    options->max_transforms = 200000U;
    options->max_track_points = 200000U;
    options->max_spectrum_values = 8000000U;
    options->max_lag_samples = 2048U;
    options->true_peak_oversample = 4U;
}

int hwa_analysis_options_validate(const HWAAnalysisOptions *options,
                                  char *error,
                                  size_t error_size)
{
    if (options->channel_mode != HWA_CHANNEL_KEEP &&
        options->channel_mode != HWA_CHANNEL_SELECT &&
        options->channel_mode != HWA_CHANNEL_MIX) {
        hwa_set_error(error, error_size, "invalid channel mode");
        return -1;
    }
    if (options->channel_mode == HWA_CHANNEL_SELECT &&
        options->selected_channel == 0U) {
        hwa_set_error(error, error_size,
                      "selected channel is one-based and must be nonzero");
        return -1;
    }
    if (options->decode_block_frames == 0U ||
        options->decode_block_frames > 1048576U) {
        hwa_set_error(error, error_size,
                      "decode block must contain 1 to 1048576 frames");
        return -1;
    }
    if (!hwa_is_power_of_two(options->frame_size) ||
        options->frame_size < 256U || options->frame_size > 16384U) {
        hwa_set_error(error, error_size,
                      "analysis frame size must be a power of two from 256 to 16384");
        return -1;
    }
    if (options->hop_size == 0U ||
        options->hop_size > options->frame_size) {
        hwa_set_error(error, error_size,
                      "hop size must be from 1 through the frame size");
        return -1;
    }
    if (!isfinite(options->silence_threshold_dbfs) ||
        options->silence_threshold_dbfs < -200.0 ||
        options->silence_threshold_dbfs > 0.0) {
        hwa_set_error(error, error_size,
                      "silence threshold must be between -200 and 0 dBFS");
        return -1;
    }
    if (options->max_input_bytes == 0U ||
        options->max_input_frames == 0U ||
        options->max_work_bytes == 0U ||
        options->max_transforms == 0U ||
        options->max_track_points == 0U ||
        options->max_spectrum_values == 0U) {
        hwa_set_error(error, error_size,
                      "analysis work limits must be nonzero");
        return -1;
    }
    if (options->true_peak_oversample != 1U &&
        options->true_peak_oversample != 4U) {
        hwa_set_error(error, error_size,
                      "true-peak oversampling must be 1 or 4");
        return -1;
    }
    if (options->collect_spectrogram && !options->collect_tracks) {
        hwa_set_error(error, error_size,
                      "spectrogram collection also needs track collection");
        return -1;
    }
    return 0;
}

static uint32_t hwa_selected_channel_mask(uint32_t mask,
                                          uint16_t selected_channel)
{
    uint16_t seen = 0U;
    unsigned bit;

    if (mask == 0U || selected_channel == 0U) {
        return 0U;
    }
    for (bit = 0U; bit < 32U; ++bit) {
        uint32_t value = UINT32_C(1) << bit;
        if ((mask & value) != 0U) {
            seen++;
            if (seen == selected_channel) {
                return value;
            }
        }
    }
    return 0U;
}

static int hwa_decode_block(const HWAWavReader *reader,
                            const HWAAnalysisOptions *options,
                            const unsigned char *source,
                            size_t frame_count,
                            double *decoded,
                            unsigned char *clipped,
                            uint16_t analyzed_channels,
                            uint64_t first_frame,
                            char *error,
                            size_t error_size)
{
    size_t frame;

    for (frame = 0U; frame < frame_count; ++frame) {
        const unsigned char *frame_data =
            source + frame * reader->format.block_align;

        if (options->channel_mode == HWA_CHANNEL_KEEP) {
            uint16_t channel;
            for (channel = 0U; channel < analyzed_channels; ++channel) {
                int sample_clipped;
                double sample = hwa_wav_decode_sample(
                    reader,
                    frame_data + (size_t)channel * reader->bytes_per_sample,
                    &sample_clipped);
                if (!isfinite(sample)) {
                    hwa_set_error(error, error_size,
                                  "non-finite sample at frame %llu, channel %u",
                                  (unsigned long long)(first_frame + frame),
                                  (unsigned)channel + 1U);
                    return -1;
                }
                decoded[frame * analyzed_channels + channel] = sample;
                clipped[frame * analyzed_channels + channel] =
                    (unsigned char)(sample_clipped != 0);
            }
        } else if (options->channel_mode == HWA_CHANNEL_SELECT) {
            size_t channel = (size_t)options->selected_channel - 1U;
            int sample_clipped;
            double sample = hwa_wav_decode_sample(
                reader,
                frame_data + channel * reader->bytes_per_sample,
                &sample_clipped);
            if (!isfinite(sample)) {
                hwa_set_error(error, error_size,
                              "non-finite sample at frame %llu, channel %u",
                              (unsigned long long)(first_frame + frame),
                              (unsigned)options->selected_channel);
                return -1;
            }
            decoded[frame] = sample;
            clipped[frame] = (unsigned char)(sample_clipped != 0);
        } else {
            long double sum = 0.0L;
            uint16_t channel;
            double mixed;

            for (channel = 0U; channel < reader->format.channels; ++channel) {
                int sample_clipped;
                double sample = hwa_wav_decode_sample(
                    reader,
                    frame_data + (size_t)channel * reader->bytes_per_sample,
                    &sample_clipped);
                (void)sample_clipped;
                if (!isfinite(sample)) {
                    hwa_set_error(error, error_size,
                                  "non-finite sample at frame %llu, channel %u",
                                  (unsigned long long)(first_frame + frame),
                                  (unsigned)channel + 1U);
                    return -1;
                }
                sum += (long double)sample;
            }
            mixed = (double)(sum / (long double)reader->format.channels);
            decoded[frame] = mixed;
            clipped[frame] =
                (unsigned char)(mixed <= -1.0 || mixed >= 1.0);
        }
    }
    return 0;
}

int hwa_analyze_wav_reader(HWAWavReader *reader,
                           const char *name,
                           const HWAAnalysisOptions *options,
                           HWAAnalysis *analysis,
                           char *error,
                           size_t error_size)
{
    HWAAnalysisOptions feature_options;
    HWAFeatureProcessor *features = NULL;
    unsigned char *raw = NULL;
    double *decoded = NULL;
    unsigned char *clipped = NULL;
    uint16_t analyzed_channels;
    uint32_t analyzed_channel_mask;
    size_t raw_size;
    size_t sample_count;
    size_t decoded_size;
    size_t name_length;
    size_t name_size;
    uint64_t analysis_work_bytes;
    uint64_t frames_processed = 0U;
    int result = -1;

    if (reader->format.frames > options->max_input_frames) {
        hwa_set_error(error, error_size,
                      "input has %llu frames, above the %llu-frame limit",
                      (unsigned long long)reader->format.frames,
                      (unsigned long long)options->max_input_frames);
        goto cleanup;
    }
    if (options->channel_mode == HWA_CHANNEL_SELECT &&
        options->selected_channel > reader->format.channels) {
        hwa_set_error(error, error_size,
                      "selected channel %u exceeds the %u-channel input",
                      (unsigned)options->selected_channel,
                      (unsigned)reader->format.channels);
        goto cleanup;
    }
    analyzed_channels = options->channel_mode == HWA_CHANNEL_KEEP
                            ? reader->format.channels
                            : 1U;
    analyzed_channel_mask =
        options->channel_mode == HWA_CHANNEL_KEEP
            ? reader->format.channel_mask
            : (options->channel_mode == HWA_CHANNEL_SELECT
                   ? hwa_selected_channel_mask(reader->format.channel_mask,
                                               options->selected_channel)
                   : 0U);

    if (options->decode_block_frames >
            SIZE_MAX / reader->format.block_align ||
        options->decode_block_frames > SIZE_MAX / analyzed_channels ||
        options->decode_block_frames * analyzed_channels >
            SIZE_MAX / sizeof(*decoded)) {
        hwa_set_error(error, error_size, "decode work buffer size overflows");
        goto cleanup;
    }
    raw_size = options->decode_block_frames * reader->format.block_align;
    sample_count = options->decode_block_frames * analyzed_channels;
    decoded_size = sample_count * sizeof(*decoded);
    name_length = strlen(name);
    if (name_length == SIZE_MAX) {
        hwa_set_error(error, error_size, "input name size overflows");
        goto cleanup;
    }
    name_size = name_length + 1U;
    analysis_work_bytes = (uint64_t)raw_size;
    if ((uint64_t)decoded_size > UINT64_MAX - analysis_work_bytes) {
        hwa_set_error(error, error_size, "analysis work size overflows");
        goto cleanup;
    }
    analysis_work_bytes += (uint64_t)decoded_size;
    if ((uint64_t)sample_count > UINT64_MAX - analysis_work_bytes) {
        hwa_set_error(error, error_size, "analysis work size overflows");
        goto cleanup;
    }
    analysis_work_bytes += (uint64_t)sample_count;
    if ((uint64_t)name_size > UINT64_MAX - analysis_work_bytes) {
        hwa_set_error(error, error_size, "analysis work size overflows");
        goto cleanup;
    }
    analysis_work_bytes += (uint64_t)name_size;
    if (analysis_work_bytes >= options->max_work_bytes) {
        hwa_set_error(error, error_size,
                      "analysis work limit exceeded: %llu bytes leave no feature budget under the %llu-byte limit",
                      (unsigned long long)analysis_work_bytes,
                      (unsigned long long)options->max_work_bytes);
        goto cleanup;
    }
    feature_options = *options;
    feature_options.max_work_bytes =
        options->max_work_bytes - analysis_work_bytes;
    raw = (unsigned char *)malloc(raw_size);
    decoded = (double *)malloc(decoded_size);
    clipped = (unsigned char *)malloc(sample_count);
    analysis->path = hwa_copy_string(name);
    if (raw == NULL || decoded == NULL || clipped == NULL ||
        analysis->path == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory while creating analysis buffers");
        goto cleanup;
    }
    analysis->format = reader->format;
    analysis->options = *options;
    analysis->analyzed_channels = analyzed_channels;
    if (hwa_features_create(&features,
                            reader->format.sample_rate_hz,
                            analyzed_channels,
                            analyzed_channel_mask,
                            reader->format.frames,
                            &feature_options,
                            error,
                            error_size) != 0) {
        goto cleanup;
    }

    for (;;) {
        size_t frames_read;

        if (hwa_wav_reader_read_frames(reader, raw,
                                       options->decode_block_frames,
                                       &frames_read, error, error_size) != 0) {
            goto cleanup;
        }
        if (frames_read == 0U) {
            break;
        }
        if (hwa_decode_block(reader, options, raw, frames_read,
                             decoded, clipped, analyzed_channels,
                             frames_processed, error, error_size) != 0 ||
            hwa_features_push(features, decoded, clipped, frames_read,
                              error, error_size) != 0) {
            goto cleanup;
        }
        frames_processed += frames_read;
    }
    if (frames_processed != reader->format.frames) {
        hwa_set_error(error, error_size, "decoded frame count changed");
        goto cleanup;
    }
    if (hwa_features_finish(features, analysis, error, error_size) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    hwa_features_destroy(features);
    free(clipped);
    free(decoded);
    free(raw);
    if (result != 0) {
        hwa_analysis_free(analysis);
    }
    return result;
}

int hwa_analyze_wav_source(const HWAByteSource *source,
                           const HWAAnalysisOptions *provided_options,
                           HWAAnalysis *analysis,
                           char *error,
                           size_t error_size)
{
    HWAAnalysisOptions selected_options;
    const HWAAnalysisOptions *options = &selected_options;
    HWAWavReader reader;
    int status;
    if (error != NULL && error_size > 0U) error[0] = '\0';
    if (analysis == NULL) {
        hwa_set_error(error, error_size, "invalid analysis arguments");
        return -1;
    }
    if (provided_options == NULL) hwa_analysis_options_default(&selected_options);
    else selected_options = *provided_options;
    memset(analysis, 0, sizeof(*analysis));
    if (source == NULL || source->name == NULL || source->read_at == NULL) {
        hwa_set_error(error, error_size, "invalid WAVE byte source");
        return -1;
    }
    if (hwa_analysis_options_validate(options, error, error_size) != 0) return -1;
    memset(&reader, 0, sizeof(reader));
    if (hwa_wav_reader_open_source(&reader, source, options->max_input_bytes,
                                   error, error_size) != 0) return -1;
    status = hwa_analyze_wav_reader(&reader, source->name, options,
                                    analysis, error, error_size);
    hwa_wav_reader_close(&reader);
    return status;
}

void hwa_analysis_free(HWAAnalysis *analysis)
{
    if (analysis != NULL) {
        free(analysis->path);
        free(analysis->channels);
        free(analysis->tracks);
        free(analysis->spectrogram_db);
        memset(analysis, 0, sizeof(*analysis));
    }
}

const char *hwa_container_name(HWAContainer container)
{
    switch (container) {
    case HWA_CONTAINER_RIFF:
        return "RIFF";
    case HWA_CONTAINER_RF64:
        return "RF64";
    default:
        return "unknown";
    }
}

const char *hwa_encoding_name(HWAEncoding encoding)
{
    switch (encoding) {
    case HWA_ENCODING_PCM:
        return "PCM";
    case HWA_ENCODING_IEEE_FLOAT:
        return "IEEE float";
    default:
        return "unknown";
    }
}

const char *hwa_band_name(size_t band)
{
    static const char *const names[HWA_BAND_COUNT] = {
        "0-60 Hz",
        "60-120 Hz",
        "120-250 Hz",
        "250-500 Hz",
        "500-1000 Hz",
        "1-2 kHz",
        "2-4 kHz",
        "4-8 kHz",
        "8-16 kHz",
        "16 kHz-Nyquist"
    };

    return band < HWA_BAND_COUNT ? names[band] : "invalid";
}

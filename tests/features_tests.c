#include "features.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PI 3.14159265358979323846264338327950288

static int failures = 0;

#define CHECK(condition, message)                                             \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, (message));   \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static HWAAnalysisOptions test_options(void)
{
    HWAAnalysisOptions options;

    memset(&options, 0, sizeof(options));
    options.channel_mode = HWA_CHANNEL_KEEP;
    options.decode_block_frames = 257U;
    options.frame_size = 1024U;
    options.hop_size = 256U;
    options.silence_threshold_dbfs = -60.0;
    options.max_input_bytes = UINT64_MAX;
    options.max_input_frames = UINT64_MAX;
    options.max_work_bytes = UINT64_MAX;
    options.max_transforms = 100000U;
    options.max_track_points = 100000U;
    options.max_spectrum_values = 10000000U;
    options.max_lag_samples = 64U;
    options.true_peak_oversample = 4U;
    options.collect_tracks = 1;
    options.collect_spectrogram = 1;
    return options;
}

static void release_analysis(HWAAnalysis *analysis)
{
    free(analysis->channels);
    free(analysis->tracks);
    free(analysis->spectrogram_db);
    memset(analysis, 0, sizeof(*analysis));
}

static int analyze_samples(const double *samples,
                           const unsigned char *clipped,
                           size_t frames,
                           uint16_t channels,
                           uint32_t sample_rate,
                           const HWAAnalysisOptions *options,
                           int split,
                           HWAAnalysis *analysis,
                           char *error,
                           size_t error_size)
{
    static const size_t blocks[] = {1U, 7U, 113U, 2U, 509U, 31U};
    HWAFeatureProcessor *processor = NULL;
    size_t position = 0U;
    size_t block_index = 0U;
    int result = -1;

    memset(analysis, 0, sizeof(*analysis));
    if (hwa_features_create(&processor, sample_rate, channels, 0U,
                            (uint64_t)frames, options,
                            error, error_size) != 0) {
        return -1;
    }
    while (position < frames) {
        size_t count = split != 0
                           ? blocks[block_index %
                                    (sizeof(blocks) / sizeof(blocks[0]))]
                           : frames;

        if (count > frames - position) {
            count = frames - position;
        }
        if (hwa_features_push(
                processor, samples + position * (size_t)channels,
                clipped != NULL
                    ? clipped + position * (size_t)channels
                    : NULL,
                count, error, error_size) != 0) {
            goto cleanup;
        }
        position += count;
        block_index++;
    }
    if (hwa_features_finish(processor, analysis,
                            error, error_size) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    hwa_features_destroy(processor);
    return result;
}

static int near(double first, double second, double tolerance)
{
    return fabs(first - second) <= tolerance;
}

static int average_chroma(const HWAAnalysis *analysis,
                          double start,
                          double end,
                          double output[HWA_CHROMA_BIN_COUNT])
{
    size_t count = 0U;
    size_t frame;
    size_t bin;
    double norm = 0.0;

    memset(output, 0, HWA_CHROMA_BIN_COUNT * sizeof(*output));
    for (frame = 0U; frame < analysis->track_count; ++frame) {
        const HWAFrameMetrics *source = &analysis->tracks[frame];

        if (source->time_seconds < start || source->time_seconds >= end ||
            source->chroma_valid == 0) {
            continue;
        }
        for (bin = 0U; bin < HWA_CHROMA_BIN_COUNT; ++bin) {
            output[bin] += source->chroma[bin];
        }
        count++;
    }
    if (count == 0U) {
        return 0;
    }
    for (bin = 0U; bin < HWA_CHROMA_BIN_COUNT; ++bin) {
        output[bin] /= (double)count;
        norm += output[bin] * output[bin];
    }
    if (!(norm > 0.0)) {
        return 0;
    }
    norm = sqrt(norm);
    for (bin = 0U; bin < HWA_CHROMA_BIN_COUNT; ++bin) {
        output[bin] /= norm;
    }
    return 1;
}

static double chroma_similarity(const double left[HWA_CHROMA_BIN_COUNT],
                                const double right[HWA_CHROMA_BIN_COUNT])
{
    double result = 0.0;
    size_t bin;

    for (bin = 0U; bin < HWA_CHROMA_BIN_COUNT; ++bin) {
        result += left[bin] * right[bin];
    }
    return result;
}

static void test_alignment_features(void)
{
    const uint32_t sample_rate = 24000U;
    const size_t frames = 60000U;
    const double shifted_a = 440.0 * 1.020405;
    HWAAnalysisOptions options = test_options();
    HWAAnalysis analysis;
    double *samples = (double *)calloc(frames, sizeof(*samples));
    double a4[HWA_CHROMA_BIN_COUNT];
    double a5[HWA_CHROMA_BIN_COUNT];
    double chord[HWA_CHROMA_BIN_COUNT];
    double shifted[HWA_CHROMA_BIN_COUNT];
    char error[HWA_ERROR_SIZE];
    double attack_max = 0.0;
    double pitch_change_max = 0.0;
    size_t index;

    options.frame_size = 1024U;
    options.hop_size = 240U;
    options.collect_spectrogram = 0;
    CHECK(samples != NULL, "alignment-feature allocation failed");
    if (samples == NULL) {
        return;
    }
    for (index = 6000U; index < 18000U; ++index) {
        samples[index] = 0.3 * sin(2.0 * TEST_PI * 440.0 *
                                    (double)index / (double)sample_rate);
    }
    for (index = 18000U; index < 30000U; ++index) {
        samples[index] = 0.3 * sin(2.0 * TEST_PI * 880.0 *
                                    (double)index / (double)sample_rate);
    }
    for (index = 30000U; index < 42000U; ++index) {
        samples[index] =
            0.20 * sin(2.0 * TEST_PI * 523.2511306011972 *
                       (double)index / (double)sample_rate) +
            0.20 * sin(2.0 * TEST_PI * 659.2551138257398 *
                       (double)index / (double)sample_rate);
    }
    for (index = 42000U; index < 54000U; ++index) {
        samples[index] = 0.3 * sin(2.0 * TEST_PI * shifted_a *
                                    (double)index / (double)sample_rate);
    }
    CHECK(analyze_samples(samples, NULL, frames, 1U, sample_rate,
                          &options, 1, &analysis,
                          error, sizeof(error)) == 0,
          error);
    if (analysis.tracks != NULL) {
        CHECK(average_chroma(&analysis, 0.35, 0.65, a4),
              "A4 chroma is unavailable");
        CHECK(average_chroma(&analysis, 0.85, 1.15, a5),
              "A5 chroma is unavailable");
        CHECK(average_chroma(&analysis, 1.35, 1.65, chord),
              "chord chroma is unavailable");
        CHECK(average_chroma(&analysis, 1.85, 2.15, shifted),
              "shifted A chroma is unavailable");
        CHECK(chroma_similarity(a4, a5) > 0.90,
              "octave-equivalent chroma similarity is below 0.90");
        CHECK(chroma_similarity(a4, shifted) > 0.90,
              "35-cent tuning shift changed chroma too much");
        CHECK(chord[0] > 0.05 && chord[4] > 0.05,
              "C/E chord lacks both pitch classes");
        for (index = 0U; index < analysis.track_count; ++index) {
            const HWAFrameMetrics *track = &analysis.tracks[index];

            CHECK(isfinite(track->combined_onset_strength) &&
                      track->combined_onset_strength >= 0.0 &&
                      track->combined_onset_strength <= 1.0,
                  "combined onset is outside 0..1");
            if (track->time_seconds >= 0.18 &&
                track->time_seconds <= 0.32 &&
                track->combined_onset_strength > attack_max) {
                attack_max = track->combined_onset_strength;
            }
            if (track->time_seconds >= 0.68 &&
                track->time_seconds <= 0.82 &&
                track->pitch_change_strength > pitch_change_max) {
                pitch_change_max = track->pitch_change_strength;
            }
        }
        CHECK(attack_max > 0.15,
              "energy/spectral/phase evidence missed a known attack");
        CHECK(pitch_change_max > 0.25,
              "pitch-change evidence missed an octave change");
    }
    release_analysis(&analysis);
    free(samples);
}

static void test_sine_and_exports(void)
{
    const uint32_t sample_rate = 48000U;
    const size_t frames = 48000U;
    const double amplitude = 0.5;
    HWAAnalysisOptions options = test_options();
    HWAAnalysis analysis;
    double *samples = (double *)malloc(frames * sizeof(*samples));
    char error[HWA_ERROR_SIZE];
    size_t index;
    size_t expected_tracks;
    int found_pitch = 0;

    CHECK(samples != NULL, "sine allocation failed");
    if (samples == NULL) {
        return;
    }
    for (index = 0U; index < frames; ++index) {
        samples[index] = amplitude *
                         sin(2.0 * TEST_PI * 1000.0 *
                             (double)index / (double)sample_rate);
    }
    CHECK(analyze_samples(samples, NULL, frames, 1U, sample_rate,
                          &options, 1, &analysis,
                          error, sizeof(error)) == 0,
          error);
    if (failures == 0 || analysis.channels != NULL) {
        expected_tracks = (frames - 1U) / options.hop_size + 1U;
        CHECK(analysis.analyzed_channels == 1U,
              "mono analysis channel count is wrong");
        CHECK(near(analysis.channels[0].peak, amplitude, 1.0e-12),
              "sine sample peak is wrong");
        CHECK(analysis.channels[0].true_peak_valid != 0,
              "true peak should be valid");
        CHECK(analysis.channels[0].true_peak >= analysis.channels[0].peak,
              "true peak fell below sample peak");
        CHECK(near(analysis.channels[0].rms,
                   amplitude / sqrt(2.0), 1.0e-10),
              "sine RMS is wrong");
        CHECK(fabs(analysis.channels[0].dc_offset) < 1.0e-12,
              "sine DC is wrong");
        CHECK(analysis.spectrum.valid != 0,
              "sine spectrum should be valid");
        CHECK(analysis.spectrum.centroid_hz > 900.0 &&
                  analysis.spectrum.centroid_hz < 1100.0,
              "sine spectral centroid is not near 1 kHz");
        CHECK(analysis.loudness.integrated_valid != 0,
              "one-second sine integrated loudness should be valid");
        CHECK(analysis.loudness.momentary_valid != 0,
              "one-second sine momentary maximum should be valid");
        CHECK(analysis.activity.active_span_valid != 0,
              "sine active span should be valid");
        CHECK(analysis.activity.classified_valid != 0,
              "sine activity classification should be valid");
        CHECK(analysis.activity.silence_fraction < 0.01,
              "sine should not be classed as silence");
        CHECK(analysis.track_count == expected_tracks,
              "track count does not match the fixed hop grid");
        CHECK(analysis.spectrum_bins == options.frame_size / 2U + 1U,
              "spectrogram bin count is wrong");
        CHECK(analysis.spectrogram_db != NULL,
              "spectrogram was not collected");
        for (index = 2U; index + 2U < analysis.track_count; ++index) {
            if (analysis.tracks[index].pitch_valid != 0 &&
                analysis.tracks[index].pitch_hz > 990.0 &&
                analysis.tracks[index].pitch_hz < 1010.0 &&
                analysis.tracks[index].pitch_confidence > 0.7) {
                found_pitch = 1;
                break;
            }
        }
        CHECK(found_pitch, "pitch track did not find the 1 kHz sine");
    }
    release_analysis(&analysis);
    free(samples);
}

static void test_block_split_invariance(void)
{
    const uint32_t sample_rate = 16000U;
    const size_t frames = 5003U;
    HWAAnalysisOptions options = test_options();
    HWAAnalysis whole;
    HWAAnalysis split;
    double *samples = (double *)malloc(frames * 2U * sizeof(*samples));
    char error[HWA_ERROR_SIZE];
    uint32_t state = 0x12345678U;
    size_t index;

    options.frame_size = 256U;
    options.hop_size = 97U;
    CHECK(samples != NULL, "split-invariance allocation failed");
    if (samples == NULL) {
        return;
    }
    for (index = 0U; index < frames; ++index) {
        double noise;

        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        noise = ((double)(state & 0xffffU) / 32768.0 - 1.0) * 0.15;
        samples[index * 2U] =
            noise + 0.2 * sin(2.0 * TEST_PI * 440.0 *
                              (double)index / (double)sample_rate);
        samples[index * 2U + 1U] =
            noise * 0.7 + 0.1 * sin(2.0 * TEST_PI * 660.0 *
                                    (double)index / (double)sample_rate);
    }
    CHECK(analyze_samples(samples, NULL, frames, 2U, sample_rate,
                          &options, 0, &whole,
                          error, sizeof(error)) == 0,
          error);
    CHECK(analyze_samples(samples, NULL, frames, 2U, sample_rate,
                          &options, 1, &split,
                          error, sizeof(error)) == 0,
          error);
    if (whole.channels != NULL && split.channels != NULL) {
        CHECK(memcmp(whole.channels, split.channels,
                     2U * sizeof(*whole.channels)) == 0,
              "channel results depend on push block splits");
        CHECK(memcmp(&whole.loudness, &split.loudness,
                     sizeof(whole.loudness)) == 0,
              "loudness depends on push block splits");
        CHECK(memcmp(&whole.spectrum, &split.spectrum,
                     sizeof(whole.spectrum)) == 0,
              "spectrum depends on push block splits");
        CHECK(memcmp(&whole.activity, &split.activity,
                     sizeof(whole.activity)) == 0,
              "activity depends on push block splits");
        CHECK(memcmp(&whole.stereo, &split.stereo,
                     sizeof(whole.stereo)) == 0,
              "stereo results depend on push block splits");
        CHECK(whole.track_count == split.track_count,
              "track counts depend on push block splits");
        CHECK(whole.track_count == 0U ||
                  memcmp(whole.tracks, split.tracks,
                         whole.track_count * sizeof(*whole.tracks)) == 0,
              "tracks depend on push block splits");
        CHECK(whole.spectrum_bins == split.spectrum_bins,
              "spectrum bin counts depend on push block splits");
        CHECK(whole.track_count == 0U ||
                  memcmp(whole.spectrogram_db, split.spectrogram_db,
                         whole.track_count * whole.spectrum_bins *
                             sizeof(*whole.spectrogram_db)) == 0,
              "spectrogram depends on push block splits");
    }
    release_analysis(&whole);
    release_analysis(&split);
    free(samples);
}

static void test_activity_and_clipping(void)
{
    const uint32_t sample_rate = 8000U;
    const size_t frames = 6400U;
    HWAAnalysisOptions options = test_options();
    HWAAnalysis analysis;
    double *samples = (double *)calloc(frames, sizeof(*samples));
    unsigned char *clipped = (unsigned char *)calloc(frames,
                                                      sizeof(*clipped));
    char error[HWA_ERROR_SIZE];
    size_t index;

    options.frame_size = 256U;
    options.hop_size = 128U;
    options.silence_threshold_dbfs = -40.0;
    options.collect_tracks = 0;
    options.collect_spectrogram = 0;
    CHECK(samples != NULL && clipped != NULL,
          "activity allocation failed");
    if (samples == NULL || clipped == NULL) {
        free(samples);
        free(clipped);
        return;
    }
    for (index = 1600U; index < 4800U; ++index) {
        samples[index] = 0.3 *
                         sin(2.0 * TEST_PI * 400.0 *
                             (double)index / (double)sample_rate);
    }
    clipped[2000U] = 1U;
    clipped[3000U] = 1U;
    CHECK(analyze_samples(samples, clipped, frames, 1U, sample_rate,
                          &options, 1, &analysis,
                          error, sizeof(error)) == 0,
          error);
    if (analysis.channels != NULL) {
        CHECK(analysis.channels[0].clipped_samples == 2U,
              "clipped sample count is wrong");
        CHECK(near(analysis.activity.silence_fraction, 0.5, 1.0e-12),
              "silence fraction is wrong");
        CHECK(analysis.activity.classified_valid != 0,
              "activity classification should be marked valid");
        CHECK(near(analysis.activity.active_start_seconds, 0.2, 1.0e-12),
              "active start is wrong");
        CHECK(near(analysis.activity.active_end_seconds, 0.6, 1.0e-12),
              "active end is wrong");
    }
    release_analysis(&analysis);
    free(samples);
    free(clipped);
}

static void test_stereo_delay(void)
{
    const uint32_t sample_rate = 8000U;
    const size_t frames = 16000U;
    const size_t known_delay = 7U;
    HWAAnalysisOptions options = test_options();
    HWAAnalysis analysis;
    double *samples = (double *)calloc(frames * 2U, sizeof(*samples));
    double *source = (double *)malloc(frames * sizeof(*source));
    char error[HWA_ERROR_SIZE];
    uint32_t state = 0x91e10da5U;
    size_t index;

    options.frame_size = 256U;
    options.hop_size = 128U;
    options.max_lag_samples = 20U;
    options.silence_threshold_dbfs = -80.0;
    options.collect_tracks = 0;
    options.collect_spectrogram = 0;
    CHECK(samples != NULL && source != NULL,
          "delay allocation failed");
    if (samples == NULL || source == NULL) {
        free(samples);
        free(source);
        return;
    }
    for (index = 0U; index < frames; ++index) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        source[index] = ((double)(state & 0xffffU) / 32768.0 - 1.0) * 0.2;
        samples[index * 2U] = source[index];
        if (index >= known_delay) {
            samples[index * 2U + 1U] = source[index - known_delay];
        }
    }
    CHECK(analyze_samples(samples, NULL, frames, 2U, sample_rate,
                          &options, 1, &analysis,
                          error, sizeof(error)) == 0,
          error);
    CHECK(analysis.stereo.available != 0,
          "stereo metrics should be available");
    CHECK(analysis.stereo.level_valid != 0,
          "stereo levels should be valid");
    CHECK(analysis.stereo.width_valid != 0,
          "stereo width should be valid");
    CHECK(analysis.stereo.band_width_valid_mask != 0U,
          "at least one stereo band width should be valid");
    CHECK(analysis.stereo.delay_valid != 0,
          "known delay should be valid");
    CHECK(near(analysis.stereo.interchannel_delay_samples,
               (double)known_delay, 1.0e-12),
          "known stereo delay was not recovered");
    CHECK(analysis.stereo.interchannel_delay_confidence > 0.99,
          "known stereo delay confidence is too low");
    release_analysis(&analysis);
    free(samples);
    free(source);
}

static void test_stereo_phase_history(void)
{
    const uint32_t sample_rate = 16000U;
    const size_t frames = 32000U;
    HWAAnalysisOptions options = test_options();
    HWAAnalysis analysis;
    double *samples = (double *)malloc(frames * 2U * sizeof(*samples));
    char error[HWA_ERROR_SIZE];
    double maximum = 0.0;
    size_t valid_count = 0U;
    size_t index;

    options.frame_size = 256U;
    options.hop_size = 64U;
    options.collect_spectrogram = 0;
    CHECK(samples != NULL, "stereo phase-history allocation failed");
    if (samples == NULL) {
        return;
    }
    for (index = 0U; index < frames; ++index) {
        double time = (double)index / (double)sample_rate;
        int second_is_louder = ((index / 4000U) & 1U) != 0U;
        double second_level = second_is_louder ? 0.30 : 0.10;
        double carrier = 2.0 * TEST_PI * 440.0 * time;

        samples[index * 2U] = 0.20 * sin(carrier);
        samples[index * 2U + 1U] = second_level * cos(carrier);
    }
    CHECK(analyze_samples(samples, NULL, frames, 2U, sample_rate,
                          &options, 1, &analysis,
                          error, sizeof(error)) == 0,
          error);
    for (index = 0U; index < analysis.track_count; ++index) {
        const HWAFrameMetrics *track = &analysis.tracks[index];

        if (track->time_seconds >= 0.15 && track->time_seconds <= 1.85 &&
            track->phase_onset_valid != 0) {
            if (track->phase_onset_strength > maximum) {
                maximum = track->phase_onset_strength;
            }
            valid_count++;
        }
    }
    CHECK(valid_count != 0U,
          "steady stereo tones made no phase-onset track");
    CHECK(maximum < 0.08,
          "phase history jumped when the louder stereo channel changed");
    release_analysis(&analysis);
    free(samples);
}

static void test_loudness_range(void)
{
    const uint32_t sample_rate = 8000U;
    const size_t frames = 48000U;
    HWAAnalysisOptions options = test_options();
    HWAAnalysis analysis;
    double *samples = (double *)malloc(frames * sizeof(*samples));
    char error[HWA_ERROR_SIZE];
    size_t index;

    options.frame_size = 1024U;
    options.hop_size = 512U;
    options.collect_tracks = 0;
    options.collect_spectrogram = 0;
    CHECK(samples != NULL, "loudness allocation failed");
    if (samples == NULL) {
        return;
    }
    for (index = 0U; index < frames; ++index) {
        samples[index] = 0.1 *
                         sin(2.0 * TEST_PI * 1000.0 *
                             (double)index / (double)sample_rate);
    }
    CHECK(analyze_samples(samples, NULL, frames, 1U, sample_rate,
                          &options, 1, &analysis,
                          error, sizeof(error)) == 0,
          error);
    CHECK(analysis.loudness.integrated_valid != 0,
          "constant tone integrated loudness should be valid");
    CHECK(analysis.loudness.range_valid != 0,
          "six-second tone loudness range should be valid");
    CHECK(analysis.loudness.short_term_valid != 0,
          "six-second tone short-term maximum should be valid");
    CHECK(analysis.loudness.loudness_range_lu <= 0.2,
          "constant tone loudness range should be near zero");
    CHECK(analysis.loudness.blocks_above_relative_gate > 0U,
          "relative loudness gate should retain blocks");
    release_analysis(&analysis);
    free(samples);
}

static void test_limits_and_nonfinite(void)
{
    HWAAnalysisOptions options = test_options();
    HWAFeatureProcessor *processor = NULL;
    char error[HWA_ERROR_SIZE];
    double samples[1000] = {0.0};
    double bad[2] = {0.0, NAN};

    options.frame_size = 256U;
    options.hop_size = 64U;
    options.collect_spectrogram = 0;
    options.max_transforms = 47U;
    CHECK(hwa_features_create(&processor, 8000U, 1U, 0U, 1000U,
                              &options, error, sizeof(error)) != 0,
          "transform cap should reject known excess work");
    CHECK(strstr(error, "transform limit") != NULL,
          "transform cap error is not clear");

    options.max_transforms = 1000U;
    options.max_track_points = 15U;
    CHECK(hwa_features_create(&processor, 8000U, 1U, 0U, 1000U,
                              &options, error, sizeof(error)) != 0,
          "track cap should reject known excess points");
    CHECK(strstr(error, "track point limit") != NULL,
          "track cap error is not clear");

    options.max_track_points = 1000U;
    options.collect_tracks = 0;
    options.collect_spectrogram = 1;
    options.max_spectrum_values = 2063U;
    CHECK(hwa_features_create(&processor, 8000U, 1U, 0U, 1000U,
                              &options, error, sizeof(error)) != 0,
          "spectrogram cap should reject known excess values");
    CHECK(strstr(error, "spectrogram value limit") != NULL,
          "spectrogram cap error is not clear");

    options.collect_spectrogram = 0;
    options.max_spectrum_values = 100000U;
    CHECK(hwa_features_create(&processor, 8000U, 1U, 0U, 1000U,
                              &options, error, sizeof(error)) == 0,
          error);
    if (processor != NULL) {
        CHECK(hwa_features_push(processor, bad, NULL, 2U,
                                error, sizeof(error)) != 0,
              "non-finite push should fail");
        CHECK(strstr(error, "non-finite") != NULL,
              "non-finite push error is not clear");
        CHECK(hwa_features_push(processor, samples, NULL, 1000U,
                                error, sizeof(error)) == 0,
              "failed push should not consume part of its block");
    }
    hwa_features_destroy(processor);
    processor = NULL;

    {
        uint64_t low = 0U;
        uint64_t high = UINT64_C(1073741824);

        while (low < high) {
            uint64_t middle = low + (high - low) / 2U;

            options.max_work_bytes = middle;
            if (hwa_features_create(&processor, 8000U, 1U, 0U, 1000U,
                                    &options, error, sizeof(error)) == 0) {
                hwa_features_destroy(processor);
                processor = NULL;
                high = middle;
            } else if (strstr(error, "work byte limit") != NULL) {
                low = middle + 1U;
            } else {
                CHECK(0, error);
                break;
            }
        }
        CHECK(low < UINT64_C(1073741824),
              "could not find the feature work byte boundary");
        options.max_work_bytes = low;
        CHECK(hwa_features_create(&processor, 8000U, 1U, 0U, 1000U,
                                  &options, error, sizeof(error)) == 0,
              "exact feature work byte cap should pass");
        hwa_features_destroy(processor);
        processor = NULL;
        CHECK(low > 0U, "feature work byte count cannot be zero");
        if (low > 0U) {
            options.max_work_bytes = low - 1U;
            CHECK(hwa_features_create(&processor, 8000U, 1U, 0U, 1000U,
                                      &options, error, sizeof(error)) != 0,
                  "one byte below the feature work cap should fail");
            CHECK(strstr(error, "work byte limit") != NULL,
                  "work byte cap error is not clear");
            hwa_features_destroy(processor);
        }
    }
}

int main(void)
{
    test_sine_and_exports();
    test_alignment_features();
    test_block_split_invariance();
    test_activity_and_clipping();
    test_stereo_delay();
    test_stereo_phase_history();
    test_loudness_range();
    test_limits_and_nonfinite();

    if (failures != 0) {
        fprintf(stderr, "%d feature test(s) failed\n", failures);
        return 1;
    }
    return 0;
}

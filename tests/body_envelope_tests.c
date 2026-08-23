#include "body_envelope.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_FRAMES 96U
#define TEST_RATE 48000U
#define TEST_FRAME_SIZE 4096U
#define TEST_BINS (TEST_FRAME_SIZE / 2U + 1U)

static int failures;

#define CHECK(condition, message)                                            \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "FAIL: %s\n", message);                  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static double known_response(double frequency)
{
    double peak_x = log2(frequency / 2200.0) / 0.18;
    double dip_x = log2(frequency / 3600.0) / 0.22;

    return 8.0 * exp(-0.5 * peak_x * peak_x) -
           5.0 * exp(-0.5 * dip_x * dip_x);
}

static void make_analysis(HWAAnalysis *analysis)
{
    size_t frame;

    memset(analysis, 0, sizeof(*analysis));
    analysis->path = (char *)malloc(15U);
    if (analysis->path != NULL) memcpy(analysis->path, "synthetic.wav", 14U);
    analysis->format.sample_rate_hz = TEST_RATE;
    analysis->options.frame_size = TEST_FRAME_SIZE;
    analysis->track_count = TEST_FRAMES;
    analysis->spectrum_bins = TEST_BINS;
    analysis->tracks = (HWAFrameMetrics *)calloc(
        TEST_FRAMES, sizeof(*analysis->tracks));
    analysis->spectrogram_db = (double *)malloc(
        TEST_FRAMES * TEST_BINS * sizeof(*analysis->spectrogram_db));
    if (analysis->tracks == NULL || analysis->spectrogram_db == NULL) return;
    for (frame = 0U; frame < TEST_FRAMES * TEST_BINS; ++frame) {
        analysis->spectrogram_db[frame] = -180.0;
    }
    for (frame = 0U; frame < TEST_FRAMES; ++frame) {
        HWAFrameMetrics *track = &analysis->tracks[frame];
        double pitch = 110.0 * pow(2.0, (double)(frame % 24U) / 24.0);
        double frame_gain = -28.0 + 4.0 * sin((double)frame * 0.37);
        size_t harmonic;

        track->pitch_valid = 1;
        track->spectrum_valid = 1;
        track->pitch_hz = pitch;
        track->pitch_confidence = 0.95;
        track->rms_dbfs = -20.0;
        track->combined_onset_strength = 0.02;
        for (harmonic = 1U; harmonic <= 32U; ++harmonic) {
            double frequency = pitch * (double)harmonic;
            double harmonic_shape = -7.0 * log2((double)harmonic + 1.0);
            double level = frame_gain + harmonic_shape +
                           known_response(frequency);
            size_t bin;
            size_t center;

            if (frequency >= 12000.0) break;
            center = (size_t)floor(frequency *
                                   (double)TEST_FRAME_SIZE /
                                   (double)TEST_RATE + 0.5);
            for (bin = center > 0U ? center - 1U : center;
                 bin <= center + 1U && bin < TEST_BINS; ++bin) {
                analysis->spectrogram_db[frame * TEST_BINS + bin] = level;
            }
        }
    }
}

static const HWABodyEnvelopePoint *nearest_point(
    const HWABodyEnvelopeEstimate *estimate,
    double frequency)
{
    size_t index;
    const HWABodyEnvelopePoint *best = NULL;
    double best_distance = 1.0e30;

    for (index = 0U; index < estimate->point_count; ++index) {
        const HWABodyEnvelopePoint *point = &estimate->points[index];
        double distance = fabs(log2(point->frequency_hz / frequency));

        if (point->valid && distance < best_distance) {
            best = point;
            best_distance = distance;
        }
    }
    return best;
}

static void test_known_response(void)
{
    HWAAnalysis analysis;
    HWABodyEnvelopeOptions options;
    HWABodyEnvelopeEstimate estimate;
    const HWABodyEnvelopePoint *peak;
    const HWABodyEnvelopePoint *low;
    const HWABodyEnvelopePoint *dip;
    uint64_t evaluations;
    char error[256] = {0};

    make_analysis(&analysis);
    hwa_body_envelope_options_default(&options);
    memset(&estimate, 0, sizeof(estimate));
    CHECK(analysis.path != NULL && analysis.tracks != NULL &&
              analysis.spectrogram_db != NULL,
          "synthetic allocation");
    CHECK(hwa_body_envelope_fit_analysis(
              &analysis, &options, 0U, &estimate, &evaluations,
              error, sizeof(error)) == 0,
          error[0] != '\0' ? error : "known response fit");
    CHECK(estimate.status == HWA_BODY_ENVELOPE_VALID,
          "known response has valid support");
    peak = nearest_point(&estimate, 2200.0);
    low = nearest_point(&estimate, 1000.0);
    dip = nearest_point(&estimate, 3600.0);
    CHECK(peak != NULL && low != NULL && dip != NULL,
          "known response comparison points exist");
    if (peak != NULL && low != NULL && dip != NULL) {
        CHECK(peak->relative_db > low->relative_db + 2.0,
              "known 2.2 kHz hill recovered");
        CHECK(peak->relative_db > dip->relative_db + 4.0,
              "known 3.6 kHz dip recovered");
    }
    CHECK(evaluations > 0U, "fit evaluation ledger is nonzero");
    free(estimate.path);
    free(estimate.points);
    hwa_analysis_free(&analysis);
}

static void test_cap_before_fit(void)
{
    HWAAnalysis analysis;
    HWABodyEnvelopeOptions options;
    HWABodyEnvelopeEstimate estimate;
    uint64_t evaluations = 99U;
    char error[256] = {0};

    make_analysis(&analysis);
    hwa_body_envelope_options_default(&options);
    options.max_fit_evaluations = 1U;
    memset(&estimate, 0x5a, sizeof(estimate));
    CHECK(hwa_body_envelope_fit_analysis(
              &analysis, &options, 0U, &estimate, &evaluations,
              error, sizeof(error)) != 0,
          "one-evaluation cap rejects fit");
    CHECK(estimate.path == NULL && estimate.points == NULL &&
              estimate.point_count == 0U && evaluations == 0U,
          "cap failure returns an empty result");
    hwa_analysis_free(&analysis);
}

int main(void)
{
    test_known_response();
    test_cap_before_fit();
    if (failures != 0) return 1;
    (void)puts("PASS: pitch-conditioned body envelope");
    return 0;
}

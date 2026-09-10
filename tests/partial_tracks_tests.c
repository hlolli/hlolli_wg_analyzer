#include "hlolli_wg_analyzer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static void fixture(HWANotePhaseSpectraResult *source, HWANotePhaseFrame frames[3], double powers[387])
{
    size_t i;
    memset(source, 0, sizeof(*source));
    memset(frames, 0, 3U * sizeof(*frames));
    memset(powers, 0, 387U * sizeof(*powers));
    hwa_note_phase_options_default(&source->series.envelope.summary.options);
    source->series.envelope.summary.options.measurement.fft_size = 256U;
    source->series.envelope.summary.options.measurement.hop_size = 64U;
    source->series.envelope.summary.format.sample_rate_hz = 16000U;
    source->series.envelope.summary.format.frames = 1024U;
    source->series.envelope.summary.phases[0].status = HWA_NOTE_PHASE_VALID;
    source->series.envelope.summary.phases[0].end_sample = 1024U;
    source->series.frames = frames;
    source->series.frame_count = 3U;
    source->bin_count = 129U;
    source->bin_powers = powers;
    for (i = 0U; i < 3U; ++i) {
        frames[i].start_sample = (uint64_t)i * 64U;
        frames[i].center_sample = frames[i].start_sample + 128U;
        frames[i].end_sample = frames[i].start_sample + 256U;
        powers[i*129U+15U] = 0.1;
        powers[i*129U+16U] = 1.0;
        powers[i*129U+17U] = 0.1;
        powers[i*129U+31U] = 0.025;
        powers[i*129U+32U] = 0.25;
        powers[i*129U+33U] = 0.025;
    }
}

int main(void)
{
    HWANotePhaseSpectraResult source;
    HWANotePhaseFrame frames[3];
    double powers[387];
    HWAPartialTracks tracks;
    HWAPartialTrackingOptions options;
    char error[HWA_ERROR_SIZE];
    size_t i;
    fixture(&source, frames, powers);
    hwa_partial_tracking_options_default(&options);
    CHECK(hwa_track_note_phase_partials(&source, &options, &tracks, error, sizeof(error)) == 0);
    CHECK(tracks.point_count == 6U && tracks.track_count == 2U && tracks.omitted_peak_count == 0U);
    for (i = 0U; i < 6U; ++i) {
        CHECK(tracks.points[i].frame_index == i/2U);
        CHECK(tracks.points[i].track_id == i%2U+1U);
        CHECK(tracks.points[i].continued == (i >= 2U));
        CHECK(fabs(tracks.points[i].frequency_hz - (i%2U == 0U ? 1000.0 : 2000.0)) < 1e-9);
    }
    hwa_partial_tracks_free(&tracks);
    hwa_partial_tracks_free(&tracks);
    options.max_peaks = 1U;
    CHECK(hwa_track_note_phase_partials(&source, &options, &tracks, error, sizeof(error)) == 0);
    CHECK(tracks.point_count == 3U && tracks.omitted_peak_count == 3U && tracks.track_count == 1U);
    hwa_partial_tracks_free(&tracks);
    hwa_partial_tracking_options_default(&options);
    frames[2].center_sample += 64U;
    CHECK(hwa_track_note_phase_partials(&source, &options, &tracks, error, sizeof(error)) == 0);
    CHECK(tracks.track_count == 4U && tracks.points[4].continued == 0);
    hwa_partial_tracks_free(&tracks);
    fixture(&source, frames, powers);
    memset(powers + 129U, 0, 129U * sizeof(*powers));
    CHECK(hwa_track_note_phase_partials(&source, &options, &tracks, error, sizeof(error)) == 0);
    CHECK(tracks.track_count == 4U && tracks.point_count == 4U);
    hwa_partial_tracks_free(&tracks);
    fixture(&source, frames, powers);
    options.max_evaluations = 1U;
    CHECK(hwa_track_note_phase_partials(&source, &options, &tracks, error, sizeof(error)) != 0);
    CHECK(tracks.points == NULL && tracks.point_count == 0U && error[0] != '\0');
    hwa_partial_tracking_options_default(&options);
    options.max_work_bytes = 1U;
    CHECK(hwa_track_note_phase_partials(&source, &options, &tracks, error, sizeof(error)) != 0);
    hwa_partial_tracking_options_default(&options);
    options.max_points = 1U;
    CHECK(hwa_track_note_phase_partials(&source, &options, &tracks, error, sizeof(error)) != 0);
    hwa_partial_tracking_options_default(&options);
    options.max_step_cents = NAN;
    CHECK(hwa_track_note_phase_partials(&source, &options, &tracks, error, sizeof(error)) != 0);
    hwa_partial_tracking_options_default(&options);
    powers[20] = NAN;
    CHECK(hwa_track_note_phase_partials(&source, &options, &tracks, error, sizeof(error)) != 0);
    CHECK(tracks.points == NULL);
    fixture(&source, frames, powers);
    frames[1].center_sample = frames[0].center_sample;
    CHECK(hwa_track_note_phase_partials(&source, &options, &tracks, error, sizeof(error)) != 0);
    fixture(&source, frames, powers);
    memset(&tracks, 0, sizeof(tracks));
    hwa_partial_tracking_options_default(&tracks.options);
    CHECK(hwa_track_note_phase_partials(&source, &tracks.options, &tracks, error, sizeof(error)) == 0);
    CHECK(tracks.point_count == 6U);
    hwa_partial_tracks_free(&tracks);
    CHECK(hwa_track_note_phase_partials(NULL, NULL, &tracks, error, sizeof(error)) != 0);
    CHECK(hwa_track_note_phase_partials(&source, NULL, NULL, error, sizeof(error)) != 0);
    hwa_partial_tracks_free(NULL);
    return 0;
}

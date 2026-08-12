#include "alignment.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned note_classes[10] = {
    0U, 4U, 7U, 2U, 9U, 5U, 11U, 3U, 8U, 1U
};

static int failures;

#define CHECK(condition, ...)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);       \
            (void)fprintf(stderr, __VA_ARGS__);                              \
            (void)fputc('\n', stderr);                                       \
            failures++;                                                      \
        }                                                                    \
    } while (0)

typedef struct OwnedTrack {
    HWAAlignFrame *frames;
    HWAAlignTrack track;
} OwnedTrack;

static void owned_track_free(OwnedTrack *owned)
{
    free(owned->frames);
    memset(owned, 0, sizeof(*owned));
}

static int make_note_track(const unsigned *notes,
                           const double *durations,
                           size_t note_count,
                           double step,
                           OwnedTrack *owned)
{
    double duration = 0.0;
    size_t frame_count;
    size_t frame;
    size_t note = 0U;
    double note_start = 0.0;
    double note_end;

    memset(owned, 0, sizeof(*owned));
    if (notes == NULL || durations == NULL || note_count == 0U ||
        !(step > 0.0)) {
        return 0;
    }
    for (frame = 0U; frame < note_count; ++frame) {
        if (!isfinite(durations[frame]) || !(durations[frame] > 0.0)) {
            return 0;
        }
        duration += durations[frame];
    }
    frame_count = (size_t)ceil(duration / step);
    owned->frames = (HWAAlignFrame *)calloc(
        frame_count, sizeof(*owned->frames));
    if (owned->frames == NULL) {
        return 0;
    }
    note_end = durations[0];
    for (frame = 0U; frame < frame_count; ++frame) {
        HWAAlignFrame *target = &owned->frames[frame];
        double time = ((double)frame + 0.5) * step;

        if (time > duration) {
            time = duration;
        }
        while (note + 1U < note_count && time >= note_end) {
            note_start = note_end;
            note++;
            note_end += durations[note];
        }
        target->time_seconds = time;
        target->chroma[notes[note] % HWA_CHROMA_BIN_COUNT] = 1.0;
        target->pitch_class = (double)(notes[note] % HWA_CHROMA_BIN_COUNT);
        target->pitch_confidence = 1.0;
        target->activity = 1.0;
        target->log_energy = -12.0 + (double)(note % 3U);
        target->evidence_flags =
            HWA_ALIGNMENT_EVIDENCE_CHROMA |
            HWA_ALIGNMENT_EVIDENCE_PITCH |
            HWA_ALIGNMENT_EVIDENCE_ENVELOPE |
            HWA_ALIGNMENT_EVIDENCE_SPECTRAL_ONSET |
            HWA_ALIGNMENT_EVIDENCE_ENERGY_ONSET;
        if (time - 0.5 * step <= note_start + 1e-12) {
            target->spectral_onset = 1.0;
            target->energy_onset = 1.0;
            target->combined_onset = 1.0;
        }
        target->event_index = SIZE_MAX;
    }
    owned->track.frames = owned->frames;
    owned->track.frame_count = frame_count;
    owned->track.step_seconds = step;
    owned->track.duration_seconds = duration;
    owned->track.tuning_confidence = 1.0;
    return 1;
}

static HWAAlignmentOptions test_options(void)
{
    HWAAlignmentOptions options;

    hwa_alignment_options_default(&options);
    options.alignment_step_seconds = 0.05;
    options.coarse_step_seconds = 0.20;
    options.dtw_band_seconds = 2.0;
    options.fine_radius_seconds = 0.40;
    options.refine_radius_seconds = 0.20;
    options.max_dtw_cells = UINT64_C(1000000);
    options.max_alignment_work_bytes = UINT64_C(16777216);
    options.max_alignment_points = 10000U;
    return options;
}

static int alignment_is_finite_and_ordered(const HWAAlignment *alignment)
{
    size_t index;

    if (!isfinite(alignment->total_cost) ||
        !isfinite(alignment->normalized_cost) ||
        !isfinite(alignment->matched_coverage) ||
        !isfinite(alignment->global_confidence) ||
        alignment->matched_coverage < 0.0 ||
        alignment->matched_coverage > 1.0 ||
        alignment->global_confidence < 0.0 ||
        alignment->global_confidence > 1.0 ||
        alignment->anchor_count < 2U) {
        return 0;
    }
    for (index = 0U; index < alignment->anchor_count; ++index) {
        const HWAAlignmentAnchor *anchor = &alignment->anchors[index];

        if (!isfinite(anchor->reference_seconds) ||
            !isfinite(anchor->target_seconds) ||
            !isfinite(anchor->confidence) ||
            anchor->confidence < 0.0 || anchor->confidence > 1.0 ||
            (index != 0U &&
             (anchor->reference_seconds <=
                  alignment->anchors[index - 1U].reference_seconds ||
              anchor->target_seconds <=
                  alignment->anchors[index - 1U].target_seconds))) {
            return 0;
        }
    }
    for (index = 0U; index < alignment->unmatched_span_count; ++index) {
        const HWAUnmatchedSpan *span = &alignment->unmatched_spans[index];

        if (!isfinite(span->start_seconds) ||
            !isfinite(span->end_seconds) ||
            span->start_seconds < 0.0 ||
            span->end_seconds < span->start_seconds) {
            return 0;
        }
    }
    return 1;
}

static int find_anchor_near(const HWAAlignment *alignment,
                            double reference,
                            double tolerance,
                            double *target)
{
    size_t index;
    double best = tolerance + 1.0;
    int found = 0;

    for (index = 0U; index < alignment->anchor_count; ++index) {
        double difference = fabs(
            alignment->anchors[index].reference_seconds - reference);

        if (difference <= tolerance && difference < best) {
            best = difference;
            *target = alignment->anchors[index].target_seconds;
            found = 1;
        }
    }
    return found;
}

static void test_identity_and_rubato(void)
{
    static const double equal[10] = {
        0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4
    };
    static const double rubato[10] = {
        0.25, 0.55, 0.35, 0.50, 0.30,
        0.45, 0.40, 0.35, 0.50, 0.35
    };
    OwnedTrack reference;
    OwnedTrack identity;
    OwnedTrack warped;
    HWAAlignmentOptions options = test_options();
    HWAAlignment result;
    char error[HWA_ERROR_SIZE];
    double expected_target = 0.0;
    double target = 0.0;
    size_t note;

    CHECK(make_note_track(note_classes, equal, 10U, 0.05, &reference) &&
              make_note_track(note_classes, equal, 10U, 0.05, &identity) &&
              make_note_track(note_classes, rubato, 10U, 0.05, &warped),
          "could not make identity/rubato tracks");
    CHECK(hwa_align_tracks(&reference.track, &identity.track, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "identity alignment failed: %s", error);
    CHECK(alignment_is_finite_and_ordered(&result),
          "identity alignment is not finite and strictly ordered");
    CHECK(result.matched_coverage > 0.99 &&
              result.global_confidence > 0.80,
          "identity alignment has low coverage or confidence");
    for (note = 1U; note < 10U; ++note) {
        double reference_time = (double)note * 0.4;

        CHECK(find_anchor_near(&result, reference_time, 0.08, &target) &&
                  fabs(target - reference_time) <= 0.08,
              "identity onset %zu missed by more than 80 ms", note);
    }
    hwa_alignment_free(&result);

    CHECK(hwa_align_tracks(&reference.track, &warped.track, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "rubato alignment failed: %s", error);
    CHECK(alignment_is_finite_and_ordered(&result),
          "rubato alignment is not finite and strictly ordered");
    for (note = 1U; note < 10U; ++note) {
        int found;

        expected_target += rubato[note - 1U];
        found = find_anchor_near(&result, (double)note * 0.4,
                                 0.08, &target);
        CHECK(found &&
                  fabs(target - expected_target) <= 0.12,
              "rubato onset %zu missed by more than 120 ms "
              "(found=%d target=%.3f expected=%.3f)",
              note, found, target, expected_target);
    }
    CHECK(result.matched_coverage > 0.90,
          "rubato alignment coverage is below 0.90");
    hwa_alignment_free(&result);
    owned_track_free(&warped);
    owned_track_free(&identity);
    owned_track_free(&reference);
}

static int has_unmatched(const HWAAlignment *alignment,
                         HWAAlignmentSide side,
                         double minimum_duration)
{
    size_t index;

    for (index = 0U; index < alignment->unmatched_span_count; ++index) {
        const HWAUnmatchedSpan *span = &alignment->unmatched_spans[index];

        if (span->side == side &&
            span->end_seconds - span->start_seconds >= minimum_duration) {
            return 1;
        }
    }
    return 0;
}

static double explicit_gap_duration(const HWAAlignment *alignment,
                                    HWAAlignmentSide side)
{
    double duration = 0.0;
    size_t index;

    for (index = 0U; index < alignment->unmatched_span_count; ++index) {
        const HWAUnmatchedSpan *span = &alignment->unmatched_spans[index];

        if (span->side == side &&
            span->reason != HWA_UNMATCHED_LOW_CONFIDENCE &&
            span->reason != HWA_UNMATCHED_NO_EVIDENCE &&
            span->end_seconds > span->start_seconds) {
            duration += span->end_seconds - span->start_seconds;
        }
    }
    return duration;
}

static void test_pickup_missing_and_repeat(void)
{
    static const double reference_duration[10] = {
        0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4
    };
    static const double pickup_duration[11] = {
        0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4
    };
    static const unsigned pickup_notes[11] = {
        6U, 0U, 4U, 7U, 2U, 9U, 5U, 11U, 3U, 8U, 1U
    };
    static const unsigned missing_notes[9] = {
        0U, 4U, 7U, 2U, 5U, 11U, 3U, 8U, 1U
    };
    static const double missing_duration[9] = {
        0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4
    };
    static const unsigned repeat_notes[11] = {
        0U, 4U, 7U, 2U, 9U, 9U, 5U, 11U, 3U, 8U, 1U
    };
    OwnedTrack reference;
    OwnedTrack changed;
    HWAAlignmentOptions options = test_options();
    HWAAlignment result;
    char error[HWA_ERROR_SIZE];

    CHECK(make_note_track(note_classes, reference_duration,
                          10U, 0.05, &reference),
          "could not make gap reference track");
    CHECK(make_note_track(pickup_notes, pickup_duration,
                          11U, 0.05, &changed),
          "could not make pickup track");
    CHECK(hwa_align_tracks(&reference.track, &changed.track, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "pickup alignment failed: %s", error);
    CHECK(has_unmatched(&result, HWA_ALIGNMENT_TARGET, 0.20),
          "pickup was not reported as a target unmatched span");
    hwa_alignment_free(&result);
    owned_track_free(&changed);

    CHECK(make_note_track(missing_notes, missing_duration,
                          9U, 0.05, &changed),
          "could not make missing-note track");
    CHECK(hwa_align_tracks(&reference.track, &changed.track, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "missing-note alignment failed: %s", error);
    CHECK(has_unmatched(&result, HWA_ALIGNMENT_REFERENCE, 0.20),
          "missing note was not reported on the reference side");
    hwa_alignment_free(&result);
    owned_track_free(&changed);

    CHECK(make_note_track(repeat_notes, pickup_duration,
                          11U, 0.05, &changed),
          "could not make repeated-note track");
    CHECK(hwa_align_tracks(&reference.track, &changed.track, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "repeat alignment failed: %s", error);
    CHECK(has_unmatched(&result, HWA_ALIGNMENT_TARGET, 0.20),
          "repeat was not reported on the target side");
    hwa_alignment_free(&result);
    owned_track_free(&changed);
    owned_track_free(&reference);
}

static void test_coarse_horizontal_run_corridor(void)
{
    static const unsigned reference_notes[5] = {
        0U, 4U, 7U, 2U, 9U
    };
    static const double reference_durations[5] = {
        0.4, 0.4, 0.4, 0.4, 0.4
    };
    static const unsigned target_notes[6] = {
        6U, 0U, 4U, 7U, 2U, 9U
    };
    static const double target_durations[6] = {
        2.0, 0.4, 0.4, 0.4, 0.4, 0.4
    };
    OwnedTrack reference;
    OwnedTrack target;
    HWAAlignmentOptions options = test_options();
    HWAAlignment result;
    char error[HWA_ERROR_SIZE];

    CHECK(make_note_track(reference_notes, reference_durations,
                          5U, 0.05, &reference) &&
              make_note_track(target_notes, target_durations,
                              6U, 0.05, &target),
          "could not make coarse horizontal-run tracks");
    options.dtw_band_seconds = 4.0;
    options.repeat_cost = 0.01;
    CHECK(hwa_align_tracks(&reference.track, &target.track, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "coarse horizontal run disconnected the fine corridor: %s",
          error);
    CHECK(alignment_is_finite_and_ordered(&result),
          "coarse horizontal run made an invalid alignment");
    hwa_alignment_free(&result);
    owned_track_free(&target);
    owned_track_free(&reference);
}

static void test_locked_anchors(void)
{
    static const double equal[10] = {
        0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4
    };
    OwnedTrack reference;
    OwnedTrack target;
    HWAAlignmentOptions options = test_options();
    HWAAlignmentAnchor locked[2];
    HWAAlignment result;
    char error[HWA_ERROR_SIZE];
    size_t index;
    int found = 0;

    CHECK(make_note_track(note_classes, equal, 10U, 0.05, &reference) &&
              make_note_track(note_classes, equal, 10U, 0.05, &target),
          "could not make locked-anchor tracks");
    memset(locked, 0, sizeof(locked));
    locked[0].reference_seconds = 1.2;
    locked[0].target_seconds = 1.35;
    locked[0].confidence = 1.0;
    locked[0].origin = HWA_ALIGNMENT_ORIGIN_MANUAL;
    locked[0].locked = 1;
    CHECK(hwa_align_tracks(&reference.track, &target.track, &options,
                           locked, 1U, &result,
                           error, sizeof(error)) == 0,
          "valid locked anchor failed: %s", error);
    for (index = 0U; index < result.anchor_count; ++index) {
        if (result.anchors[index].origin == HWA_ALIGNMENT_ORIGIN_MANUAL &&
            fabs(result.anchors[index].reference_seconds - 1.2) < 1e-12 &&
            fabs(result.anchors[index].target_seconds - 1.35) < 1e-12 &&
            result.anchors[index].locked != 0) {
            found = 1;
        }
    }
    CHECK(found, "manual locked anchor was not kept exactly");
    hwa_alignment_free(&result);

    locked[1] = locked[0];
    locked[1].reference_seconds = 1.21;
    locked[1].target_seconds = 1.36;
    CHECK(hwa_align_tracks(&reference.track, &target.track, &options,
                           locked, 2U, &result,
                           error, sizeof(error)) != 0 &&
              strstr(error, "collapse") != NULL,
          "anchors in one fine cell were accepted: %s", error);
    locked[1].reference_seconds = 1.1;
    locked[1].target_seconds = 1.5;
    CHECK(hwa_align_tracks(&reference.track, &target.track, &options,
                           locked, 2U, &result,
                           error, sizeof(error)) != 0 &&
              strstr(error, "increase") != NULL,
          "non-monotone locked anchors were accepted: %s", error);
    locked[0].reference_seconds = NAN;
    CHECK(hwa_align_tracks(&reference.track, &target.track, &options,
                           locked, 1U, &result,
                           error, sizeof(error)) != 0 &&
              strstr(error, "duration") != NULL,
          "non-finite locked anchor was accepted: %s", error);
    owned_track_free(&target);
    owned_track_free(&reference);
}

static void test_exact_limits(void)
{
    static const double equal[10] = {
        0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4, 0.4
    };
    OwnedTrack reference;
    OwnedTrack target;
    HWAAlignmentOptions options = test_options();
    HWAAlignment result;
    char error[HWA_ERROR_SIZE];
    uint64_t exact_cells;
    uint64_t low = 1U;
    uint64_t high = UINT64_C(16777216);

    CHECK(make_note_track(note_classes, equal, 10U, 0.05, &reference) &&
              make_note_track(note_classes, equal, 10U, 0.05, &target),
          "could not make limit tracks");
    CHECK(hwa_align_tracks(&reference.track, &target.track, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "baseline limit alignment failed: %s", error);
    exact_cells = result.dtw_cells;
    hwa_alignment_free(&result);
    options.max_dtw_cells = exact_cells;
    CHECK(hwa_align_tracks(&reference.track, &target.track, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "exact DTW cell cap failed: %s", error);
    hwa_alignment_free(&result);
    options.max_dtw_cells = exact_cells - 1U;
    CHECK(hwa_align_tracks(&reference.track, &target.track, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) != 0 &&
              strstr(error, "DTW cell limit exceeded") != NULL,
          "one cell below the DTW cap passed: %s", error);
    options.max_dtw_cells = UINT64_C(1000000);

    while (low < high) {
        uint64_t middle = low + (high - low) / 2U;

        options.max_alignment_work_bytes = middle;
        if (hwa_align_tracks(&reference.track, &target.track, &options,
                             NULL, 0U, &result,
                             error, sizeof(error)) == 0) {
            hwa_alignment_free(&result);
            high = middle;
        } else if (strstr(error, "alignment work byte limit exceeded") !=
                   NULL) {
            low = middle + 1U;
        } else {
            CHECK(0, "unexpected work-cap error: %s", error);
            break;
        }
    }
    options.max_alignment_work_bytes = low;
    CHECK(hwa_align_tracks(&reference.track, &target.track, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "exact alignment work cap failed: %s", error);
    hwa_alignment_free(&result);
    options.max_alignment_work_bytes = low - 1U;
    CHECK(hwa_align_tracks(&reference.track, &target.track, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) != 0 &&
              strstr(error, "alignment work byte limit exceeded") != NULL,
          "one byte below the alignment work cap passed: %s", error);

    options = test_options();
    options.max_alignment_points =
        reference.track.frame_count + target.track.frame_count - 1U;
    CHECK(hwa_align_tracks(&reference.track, &target.track, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) != 0 &&
              strstr(error, "alignment point limit") != NULL,
          "one point below the input sum passed: %s", error);
    owned_track_free(&target);
    owned_track_free(&reference);
}

static void test_no_evidence_and_bad_track(void)
{
    HWAAlignFrame reference_frames[8];
    HWAAlignFrame target_frames[8];
    HWAAlignTrack reference;
    HWAAlignTrack target;
    HWAAlignmentOptions options = test_options();
    HWAAlignment result;
    char error[HWA_ERROR_SIZE];
    size_t index;
    int warned = 0;

    memset(reference_frames, 0, sizeof(reference_frames));
    memset(target_frames, 0, sizeof(target_frames));
    memset(&reference, 0, sizeof(reference));
    memset(&target, 0, sizeof(target));
    for (index = 0U; index < 8U; ++index) {
        reference_frames[index].time_seconds = ((double)index + 0.5) * 0.05;
        target_frames[index] = reference_frames[index];
        reference_frames[index].log_energy = -120.0;
        target_frames[index].log_energy = -120.0;
    }
    reference.frames = reference_frames;
    reference.frame_count = 8U;
    reference.step_seconds = 0.05;
    reference.duration_seconds = 0.4;
    target.frames = target_frames;
    target.frame_count = 8U;
    target.step_seconds = 0.05;
    target.duration_seconds = 0.4;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "no-evidence alignment failed: %s", error);
    for (index = 0U; index < result.warning_count; ++index) {
        if (strcmp(result.warnings[index].code,
                   "no_alignment_evidence") == 0) {
            warned = 1;
        }
    }
    CHECK(warned, "no-evidence alignment lacks its warning");
    hwa_alignment_free(&result);

    target_frames[3].chroma[0] = NAN;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) != 0 &&
              strstr(error, "invalid chroma") != NULL,
          "non-finite chroma was accepted: %s", error);
    target_frames[3].chroma[0] = 0.0;
    target.tuning_offset_cents = NAN;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) != 0 &&
              strstr(error, "invalid alignment track") != NULL,
          "non-finite tuning offset was accepted: %s", error);
    target.tuning_offset_cents = 0.0;
    target.tuning_confidence = 1.01;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) != 0 &&
              strstr(error, "invalid alignment track") != NULL,
          "out-of-range tuning confidence was accepted: %s", error);
}

static void fill_frame(HWAAlignFrame *frame,
                       size_t index,
                       double step,
                       unsigned note)
{
    memset(frame, 0, sizeof(*frame));
    frame->time_seconds = ((double)index + 0.5) * step;
    frame->chroma[note % HWA_CHROMA_BIN_COUNT] = 1.0;
    frame->pitch_class = (double)(note % HWA_CHROMA_BIN_COUNT);
    frame->pitch_confidence = 1.0;
    frame->activity = 1.0;
    frame->log_energy = -12.0;
    frame->event_index = SIZE_MAX;
    frame->evidence_flags =
        HWA_ALIGNMENT_EVIDENCE_CHROMA |
        HWA_ALIGNMENT_EVIDENCE_PITCH |
        HWA_ALIGNMENT_EVIDENCE_ENVELOPE |
        HWA_ALIGNMENT_EVIDENCE_SPECTRAL_ONSET |
        HWA_ALIGNMENT_EVIDENCE_ENERGY_ONSET;
}

static void make_constant_track(HWAAlignFrame *frames,
                                size_t count,
                                double step,
                                int is_score,
                                HWAAlignTrack *track)
{
    size_t index;

    memset(track, 0, sizeof(*track));
    for (index = 0U; index < count; ++index) {
        fill_frame(&frames[index], index, step, 0U);
        if (is_score) {
            frames[index].score_beat =
                frames[index].time_seconds * 2.0;
            frames[index].score_beat_valid = 1;
        }
    }
    track->frames = frames;
    track->frame_count = count;
    track->step_seconds = step;
    track->duration_seconds = (double)count * step;
    track->tuning_confidence = 1.0;
    track->is_score = is_score;
}

static size_t manual_anchor_count(const HWAAlignment *alignment,
                                  double reference_seconds,
                                  double target_seconds)
{
    size_t found = 0U;
    size_t index;

    for (index = 0U; index < alignment->anchor_count; ++index) {
        const HWAAlignmentAnchor *anchor = &alignment->anchors[index];

        if (anchor->origin == HWA_ALIGNMENT_ORIGIN_MANUAL &&
            anchor->locked != 0 &&
            anchor->reference_seconds == reference_seconds &&
            anchor->target_seconds == target_seconds) {
            found++;
        }
    }
    return found;
}

static void test_large_and_collapsed_locked_anchors(void)
{
    HWAAlignFrame reference_frames[80];
    HWAAlignFrame target_frames[80];
    HWAAlignTrack reference;
    HWAAlignTrack target;
    HWAAlignmentOptions options = test_options();
    HWAAlignmentAnchor locked[2];
    HWAAlignment result;
    char error[HWA_ERROR_SIZE];

    make_constant_track(reference_frames, 80U, 0.05, 0, &reference);
    make_constant_track(target_frames, 80U, 0.05, 0, &target);
    options.dtw_band_seconds = 4.0;
    options.fine_radius_seconds = 0.10;
    options.refine_radius_seconds = 0.05;
    memset(locked, 0, sizeof(locked));
    locked[0].reference_seconds = 2.0;
    locked[0].target_seconds = 3.0;
    locked[0].confidence = 1.0;
    locked[0].origin = HWA_ALIGNMENT_ORIGIN_MANUAL;
    locked[0].locked = 1;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           locked, 1U, &result,
                           error, sizeof(error)) == 0,
          "large manual correction failed: %s", error);
    CHECK(manual_anchor_count(&result, 2.0, 3.0) == 1U,
          "large manual correction was not kept exactly");
    hwa_alignment_free(&result);

    locked[0].reference_seconds = 1.01;
    locked[0].target_seconds = 2.01;
    locked[1] = locked[0];
    locked[1].reference_seconds = 1.08;
    locked[1].target_seconds = 2.15;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           locked, 2U, &result,
                           error, sizeof(error)) == 0,
          "locks collapsed in one coarse row failed: %s", error);
    CHECK(manual_anchor_count(&result, 1.01, 2.01) == 1U &&
              manual_anchor_count(&result, 1.08, 2.15) == 1U,
          "a fine lock collapsed in one coarse row was lost");
    hwa_alignment_free(&result);

    locked[0].reference_seconds = 1.01;
    locked[0].target_seconds = 2.01;
    locked[1] = locked[0];
    locked[1].reference_seconds = 1.25;
    locked[1].target_seconds = 2.08;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           locked, 2U, &result,
                           error, sizeof(error)) == 0,
          "locks collapsed in one coarse column failed: %s", error);
    CHECK(manual_anchor_count(&result, 1.01, 2.01) == 1U &&
              manual_anchor_count(&result, 1.25, 2.08) == 1U,
          "a fine lock collapsed in one coarse column was lost");
    hwa_alignment_free(&result);
}

static void test_locked_endpoint_and_score_rules(void)
{
    HWAAlignFrame reference_frames[40];
    HWAAlignFrame target_frames[40];
    HWAAlignTrack reference;
    HWAAlignTrack target;
    HWAAlignmentOptions options = test_options();
    HWAAlignmentAnchor locked;
    HWAAlignment result;
    char error[HWA_ERROR_SIZE];
    size_t index;
    int found_score = 0;

    make_constant_track(reference_frames, 40U, 0.05, 0, &reference);
    make_constant_track(target_frames, 40U, 0.05, 0, &target);
    options.dtw_band_seconds = 2.0;
    memset(&locked, 0, sizeof(locked));
    locked.reference_seconds = 0.01;
    locked.target_seconds = 0.01;
    locked.confidence = 1.0;
    locked.origin = HWA_ALIGNMENT_ORIGIN_MANUAL;
    locked.locked = 1;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           &locked, 1U, &result,
                           error, sizeof(error)) == 0,
          "near-endpoint lock failed: %s", error);
    CHECK(result.anchor_count >= 3U &&
              result.anchors[0].reference_seconds == 0.0 &&
              result.anchors[0].target_seconds == 0.0 &&
              result.anchors[result.anchor_count - 1U].reference_seconds ==
                  reference.duration_seconds &&
              result.anchors[result.anchor_count - 1U].target_seconds ==
                  target.duration_seconds,
          "a near-endpoint lock removed full timeline coverage");
    hwa_alignment_free(&result);

    locked.reference_seconds = 0.0;
    locked.target_seconds = 0.01;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           &locked, 1U, &result,
                           error, sizeof(error)) != 0 &&
              strstr(error, "exact timeline endpoints") != NULL,
          "a one-sided endpoint lock was accepted: %s", error);

    make_constant_track(reference_frames, 40U, 0.05, 1, &reference);
    locked.reference_seconds = 1.0;
    locked.target_seconds = 1.1;
    locked.score_beat_valid = 0;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           &locked, 1U, &result,
                           error, sizeof(error)) == 0,
          "score lock without a supplied beat failed: %s", error);
    for (index = 0U; index < result.anchor_count; ++index) {
        const HWAAlignmentAnchor *anchor = &result.anchors[index];

        if (anchor->origin == HWA_ALIGNMENT_ORIGIN_MANUAL &&
            anchor->reference_seconds == 1.0 &&
            anchor->score_beat_valid != 0 &&
            fabs(anchor->score_beat - 2.0) <= 1e-12) {
            found_score = 1;
        }
    }
    CHECK(found_score,
          "score alignment did not derive a manual anchor beat");
    hwa_alignment_free(&result);

    make_constant_track(reference_frames, 40U, 0.05, 0, &reference);
    locked.score_beat = 9.0;
    locked.score_beat_valid = 1;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           &locked, 1U, &result,
                           error, sizeof(error)) == 0,
          "audio lock with stray score beat failed: %s", error);
    for (index = 0U; index < result.anchor_count; ++index) {
        if (result.anchors[index].origin == HWA_ALIGNMENT_ORIGIN_MANUAL) {
            CHECK(result.anchors[index].score_beat_valid == 0,
                  "audio alignment kept a score-only anchor field");
        }
    }
    hwa_alignment_free(&result);
}

static void test_one_frame_and_huge_radii(void)
{
    HWAAlignFrame reference_frames[20];
    HWAAlignFrame target_frames[20];
    HWAAlignTrack reference;
    HWAAlignTrack target;
    HWAAlignmentOptions options = test_options();
    HWAAlignmentAnchor locked;
    HWAAlignment result;
    char error[HWA_ERROR_SIZE];

    make_constant_track(reference_frames, 1U, 0.03, 0, &reference);
    make_constant_track(target_frames, 1U, 0.03, 0, &target);
    options.alignment_step_seconds = 0.03;
    options.coarse_step_seconds = 0.20;
    options.fine_radius_seconds = 0.20;
    options.refine_radius_seconds = 0.03;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "one-frame alignment failed: %s", error);
    CHECK(result.anchor_count == 2U &&
              result.anchors[0].reference_seconds == 0.0 &&
              result.anchors[1].reference_seconds ==
                  reference.duration_seconds,
          "one-frame alignment did not keep both endpoints");
    hwa_alignment_free(&result);

    make_constant_track(reference_frames, 20U, 0.05, 0, &reference);
    make_constant_track(target_frames, 20U, 0.05, 0, &target);
    reference_frames[10].combined_onset = 1.0;
    options = test_options();
    options.dtw_band_seconds = DBL_MAX;
    options.fine_radius_seconds = DBL_MAX;
    options.refine_radius_seconds = DBL_MAX;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "huge finite radii failed: %s", error);
    CHECK(alignment_is_finite_and_ordered(&result),
          "huge finite radii made an invalid result");
    hwa_alignment_free(&result);

    make_constant_track(reference_frames, 1U, 0.05, 0, &reference);
    make_constant_track(target_frames, 1U, 0.05, 0, &target);
    reference.duration_seconds = DBL_MAX;
    target.duration_seconds = DBL_MAX;
    memset(&locked, 0, sizeof(locked));
    locked.reference_seconds = DBL_MAX / 2.0;
    locked.target_seconds = DBL_MAX / 2.0;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           &locked, 1U, &result,
                           error, sizeof(error)) != 0 &&
              strstr(error, "resample") != NULL,
          "huge finite anchor time did not fail without a cast: %s", error);
}

static int zero_alignment(const HWAAlignment *alignment)
{
    HWAAlignment zero;

    memset(&zero, 0, sizeof(zero));
    return memcmp(alignment, &zero, sizeof(zero)) == 0;
}

static void test_public_result_initialization(void)
{
    HWAAlignment alignment;
    HWAAlignmentOptions options;
    char error[HWA_ERROR_SIZE];

    memset(&alignment, 0, sizeof(alignment));
    hwa_alignment_options_default(&alignment.options);
    CHECK(hwa_align_audio_wav("", "", &alignment.options,
                              NULL, 0U, &alignment,
                              error, sizeof(error)) != 0 &&
              strstr(error, "hash input") != NULL &&
              zero_alignment(&alignment),
          "audio wrapper did not copy aliased options before init: %s",
          error);

    hwa_alignment_options_default(&alignment.options);
    CHECK(hwa_align_score_manifest_wav("", "", &alignment.options,
                                       NULL, 0U, &alignment,
                                       error, sizeof(error)) != 0 &&
              strstr(error, "hash input") != NULL &&
              zero_alignment(&alignment),
          "score wrapper did not copy aliased options before init: %s",
          error);

    hwa_alignment_options_default(&options);
    memset(&alignment, 0xa5, sizeof(alignment));
    CHECK(hwa_align_audio_wav(NULL, "unused", &options,
                              NULL, 0U, &alignment,
                              error, sizeof(error)) != 0 &&
              zero_alignment(&alignment),
          "audio wrapper left a non-null failed result uninitialized");
}

static void test_refinement_anchor_collision(void)
{
    HWAAlignFrame reference_frames[20];
    HWAAlignFrame target_frames[20];
    HWAAlignTrack reference;
    HWAAlignTrack target;
    HWAAlignmentOptions options = test_options();
    HWAAlignment result;
    char error[HWA_ERROR_SIZE];
    size_t index;

    memset(&reference, 0, sizeof(reference));
    memset(&target, 0, sizeof(target));
    for (index = 0U; index < 20U; ++index) {
        fill_frame(&reference_frames[index], index, 0.05, 0U);
        fill_frame(&target_frames[index], index, 0.05, 0U);
    }
    reference_frames[5].combined_onset = 1.0;
    reference_frames[5].spectral_onset = 1.0;
    reference_frames[5].energy_onset = 1.0;
    reference_frames[7].combined_onset = 1.0;
    reference_frames[7].spectral_onset = 1.0;
    reference_frames[7].energy_onset = 1.0;
    target_frames[6].combined_onset = 1.0;
    target_frames[6].spectral_onset = 1.0;
    target_frames[6].energy_onset = 1.0;
    reference.frames = reference_frames;
    reference.frame_count = 20U;
    reference.step_seconds = 0.05;
    reference.duration_seconds = 1.0;
    reference.tuning_confidence = 1.0;
    target.frames = target_frames;
    target.frame_count = 20U;
    target.step_seconds = 0.05;
    target.duration_seconds = 1.0;
    target.tuning_confidence = 1.0;
    options.refine_radius_seconds = 0.20;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "nearby-onset refinement failed: %s", error);
    CHECK(alignment_is_finite_and_ordered(&result),
          "two refined landmarks made duplicate or reversed anchors");
    CHECK(result.anchor_count >= 3U && result.anchor_count <= 4U,
          "nearby onsets made an unexpected anchor count: %zu",
          result.anchor_count);
    hwa_alignment_free(&result);
}

static void test_one_frame_unmatched_span(void)
{
    static const unsigned reference_notes[12] = {
        0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 0U
    };
    static const unsigned target_notes[13] = {
        0U, 1U, 2U, 3U, 4U, 5U, 11U, 6U, 7U, 8U, 9U, 10U, 0U
    };
    HWAAlignFrame reference_frames[12];
    HWAAlignFrame target_frames[13];
    HWAAlignTrack reference;
    HWAAlignTrack target;
    HWAAlignmentOptions options = test_options();
    HWAAlignment result;
    char error[HWA_ERROR_SIZE];
    size_t index;

    memset(&reference, 0, sizeof(reference));
    memset(&target, 0, sizeof(target));
    for (index = 0U; index < 12U; ++index) {
        fill_frame(&reference_frames[index], index, 0.05,
                   reference_notes[index]);
    }
    for (index = 0U; index < 13U; ++index) {
        fill_frame(&target_frames[index], index, 0.05,
                   target_notes[index]);
    }
    reference.frames = reference_frames;
    reference.frame_count = 12U;
    reference.step_seconds = 0.05;
    reference.duration_seconds = 0.60;
    reference.tuning_confidence = 1.0;
    target.frames = target_frames;
    target.frame_count = 13U;
    target.step_seconds = 0.05;
    target.duration_seconds = 0.65;
    target.tuning_confidence = 1.0;
    options.skip_cost = 0.05;
    options.repeat_cost = 0.05;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "one-frame gap alignment failed: %s", error);
    CHECK(has_unmatched(&result, HWA_ALIGNMENT_TARGET, 0.049),
          "one fine-frame target gap was suppressed");
    CHECK(alignment_is_finite_and_ordered(&result),
          "one-frame gap made an invalid alignment");
    hwa_alignment_free(&result);
}

static void test_room_tail(void)
{
    static const unsigned notes[5] = {0U, 4U, 7U, 2U, 9U};
    static const double durations[5] = {0.4, 0.4, 0.4, 0.4, 0.4};
    OwnedTrack reference;
    OwnedTrack target;
    HWAAlignmentOptions options = test_options();
    HWAAlignment result;
    HWAAlignFrame *grown;
    char error[HWA_ERROR_SIZE];
    size_t original_count;
    size_t index;

    CHECK(make_note_track(notes, durations, 5U, 0.05, &reference) &&
              make_note_track(notes, durations, 5U, 0.05, &target),
          "could not make room-tail tracks");
    original_count = target.track.frame_count;
    grown = (HWAAlignFrame *)realloc(
        target.frames, (original_count + 8U) * sizeof(*grown));
    CHECK(grown != NULL, "could not grow the room-tail track");
    if (grown == NULL) {
        owned_track_free(&target);
        owned_track_free(&reference);
        return;
    }
    target.frames = grown;
    target.track.frames = grown;
    for (index = 0U; index < 8U; ++index) {
        HWAAlignFrame *frame = &grown[original_count + index];

        memset(frame, 0, sizeof(*frame));
        frame->time_seconds =
            ((double)(original_count + index) + 0.5) * 0.05;
        frame->log_energy = -80.0;
        frame->event_index = SIZE_MAX;
    }
    target.track.frame_count = original_count + 8U;
    target.track.duration_seconds = 2.4;
    options.repeat_cost = 0.02;
    CHECK(hwa_align_tracks(&reference.track, &target.track, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "room-tail alignment failed: %s", error);
    CHECK(alignment_is_finite_and_ordered(&result),
          "room tail made an invalid alignment");
    CHECK(has_unmatched(&result, HWA_ALIGNMENT_TARGET, 0.30),
          "room tail was not kept as a target unmatched span");
    hwa_alignment_free(&result);
    owned_track_free(&target);
    owned_track_free(&reference);
}

static void test_noise_and_tuning_offset(void)
{
    static const double durations[10] = {
        0.3, 0.3, 0.3, 0.3, 0.3, 0.3, 0.3, 0.3, 0.3, 0.3
    };
    OwnedTrack reference;
    OwnedTrack target;
    HWAAlignmentOptions options = test_options();
    HWAAlignment result;
    char error[HWA_ERROR_SIZE];
    size_t frame;

    CHECK(make_note_track(note_classes, durations, 10U, 0.05, &reference) &&
              make_note_track(note_classes, durations, 10U, 0.05, &target),
          "could not make noise/tuning tracks");
    for (frame = 0U; frame < target.track.frame_count; ++frame) {
        HWAAlignFrame *value = &target.frames[frame];
        double norm = 0.0;
        size_t bin;

        for (bin = 0U; bin < HWA_CHROMA_BIN_COUNT; ++bin) {
            value->chroma[bin] += 0.025;
            norm += value->chroma[bin] * value->chroma[bin];
        }
        norm = sqrt(norm);
        for (bin = 0U; bin < HWA_CHROMA_BIN_COUNT; ++bin) {
            value->chroma[bin] /= norm;
        }
        value->log_energy += (double)((int)(frame % 5U) - 2) * 0.15;
        value->combined_onset *= 0.85;
        value->spectral_onset *= 0.85;
        value->energy_onset *= 0.85;
    }
    reference.track.tuning_offset_cents = 0.0;
    reference.track.tuning_confidence = 1.0;
    target.track.tuning_offset_cents = 37.5;
    target.track.tuning_confidence = 0.9;
    CHECK(hwa_align_tracks(&reference.track, &target.track, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "noisy/tuned alignment failed: %s", error);
    CHECK(alignment_is_finite_and_ordered(&result) &&
              result.matched_coverage > 0.90,
          "small chroma noise broke alignment coverage");
    CHECK(fabs(result.tuning_offset_cents - 37.5) <= 1e-12 &&
              fabs(result.tuning_confidence - 0.9) <= 1e-12,
          "known tuning offset or confidence changed");
    hwa_alignment_free(&result);
    owned_track_free(&target);
    owned_track_free(&reference);
}

static void test_trill(void)
{
    static const unsigned reference_notes[5] = {0U, 4U, 7U, 2U, 9U};
    static const double reference_durations[5] = {
        0.4, 0.4, 0.4, 0.4, 0.4
    };
    static const unsigned target_notes[8] = {
        0U, 4U, 7U, 9U, 7U, 9U, 2U, 9U
    };
    static const double target_durations[8] = {
        0.4, 0.4, 0.1, 0.1, 0.1, 0.1, 0.4, 0.4
    };
    OwnedTrack reference;
    OwnedTrack target;
    HWAAlignmentOptions options = test_options();
    HWAAlignment result;
    char error[HWA_ERROR_SIZE];
    double mapped = 0.0;

    CHECK(make_note_track(reference_notes, reference_durations,
                          5U, 0.05, &reference) &&
              make_note_track(target_notes, target_durations,
                              8U, 0.05, &target),
          "could not make trill tracks");
    CHECK(hwa_align_tracks(&reference.track, &target.track, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "trill alignment failed: %s", error);
    CHECK(alignment_is_finite_and_ordered(&result) &&
              result.matched_coverage > 0.70,
          "trill made an invalid or low-coverage alignment");
    CHECK(find_anchor_near(&result, 1.2, 0.08, &mapped) &&
              fabs(mapped - 1.2) <= 0.15,
          "trill shifted the following note by more than 150 ms");
    hwa_alignment_free(&result);
    owned_track_free(&target);
    owned_track_free(&reference);
}

static void test_audio_match_uses_interior_path(void)
{
    static const unsigned base[12] = {
        0U, 7U, 2U, 10U, 4U, 9U, 1U, 8U, 3U, 11U, 5U, 6U
    };
    static const unsigned shift[5] = {0U, 1U, 4U, 2U, 7U};
    const size_t frame_count = 600U;
    const double step = 0.05;
    HWAAlignFrame *reference_frames = (HWAAlignFrame *)calloc(
        frame_count, sizeof(*reference_frames));
    HWAAlignFrame *target_frames = (HWAAlignFrame *)calloc(
        frame_count, sizeof(*target_frames));
    HWAAlignTrack reference;
    HWAAlignTrack target;
    HWAAlignmentOptions options = test_options();
    HWAAlignment result;
    char error[HWA_ERROR_SIZE];
    size_t index;
    int low_interior = 0;

    CHECK(reference_frames != NULL && target_frames != NULL,
          "could not allocate the interior-confidence tracks");
    if (reference_frames == NULL || target_frames == NULL) {
        free(target_frames);
        free(reference_frames);
        return;
    }
    memset(&reference, 0, sizeof(reference));
    memset(&target, 0, sizeof(target));
    for (index = 0U; index < frame_count; ++index) {
        size_t block = index / 10U;
        unsigned note = (base[block % 12U] + shift[block / 12U]) % 12U;

        fill_frame(&reference_frames[index], index, step, note);
        fill_frame(&target_frames[index], index, step, note);
        if (index >= 200U && index < 400U) {
            double other = sqrt((1.0 - 0.55 * 0.55) / 11.0);
            size_t bin;

            for (bin = 0U; bin < HWA_CHROMA_BIN_COUNT; ++bin) {
                target_frames[index].chroma[bin] = bin == note ? 0.55
                                                                : other;
            }
        }
    }
    reference.frames = reference_frames;
    reference.frame_count = frame_count;
    reference.step_seconds = step;
    reference.duration_seconds = (double)frame_count * step;
    reference.tuning_confidence = 1.0;
    target.frames = target_frames;
    target.frame_count = frame_count;
    target.step_seconds = step;
    target.duration_seconds = (double)frame_count * step;
    target.tuning_confidence = 1.0;
    options.dtw_band_seconds = 15.0;
    options.fine_radius_seconds = 1.5;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "regularized interior alignment failed: %s", error);
    CHECK(explicit_gap_duration(&result, HWA_ALIGNMENT_REFERENCE) < 5.0 &&
              explicit_gap_duration(&result, HWA_ALIGNMENT_TARGET) < 5.0,
          "default gap costs discarded the long uncertain interval");
    hwa_alignment_free(&result);
    options.skip_cost = 0.01;
    options.repeat_cost = 0.01;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "interior-confidence alignment failed: %s", error);
    CHECK(explicit_gap_duration(&result, HWA_ALIGNMENT_REFERENCE) >= 5.0 &&
              explicit_gap_duration(&result, HWA_ALIGNMENT_TARGET) >= 5.0,
          "the forced cheap-gap path did not contain both gap directions");
    for (index = 0U; index < result.match_count; ++index) {
        const HWAAlignmentMatch *match = &result.matches[index];
        double reference_duration = match->reference_end_seconds -
                                    match->reference_start_seconds;
        double target_duration = match->target_end_seconds -
                                 match->target_start_seconds;

        if (match->status == HWA_ALIGNMENT_LOW_CONFIDENCE &&
            match->confidence < 0.20 &&
            (reference_duration > 1.0 || target_duration > 1.0)) {
            low_interior = 1;
            break;
        }
    }
    CHECK(low_interior,
          "an audio match inherited good endpoints across long path gaps");
    hwa_alignment_free(&result);
    free(target_frames);
    free(reference_frames);
}

static void test_low_confidence_reduces_stable_coverage(void)
{
    HWAAlignFrame reference_frames[40];
    HWAAlignFrame target_frames[40];
    HWAAlignTrack reference;
    HWAAlignTrack target;
    HWAAlignmentOptions options = test_options();
    HWAAlignment result;
    char error[HWA_ERROR_SIZE];
    size_t index;
    int warned = 0;

    memset(&reference, 0, sizeof(reference));
    memset(&target, 0, sizeof(target));
    for (index = 0U; index < 40U; ++index) {
        fill_frame(&reference_frames[index], index, 0.05, 0U);
        fill_frame(&target_frames[index], index, 0.05, 6U);
    }
    reference.frames = reference_frames;
    reference.frame_count = 40U;
    reference.step_seconds = 0.05;
    reference.duration_seconds = 2.0;
    reference.tuning_confidence = 1.0;
    target.frames = target_frames;
    target.frame_count = 40U;
    target.step_seconds = 0.05;
    target.duration_seconds = 2.0;
    target.tuning_confidence = 1.0;
    options.skip_cost = 2.0;
    options.repeat_cost = 2.0;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "low-confidence coverage alignment failed: %s", error);
    CHECK(result.matched_coverage < 0.05,
          "low-confidence spans were omitted from stable coverage: %.6f",
          result.matched_coverage);
    for (index = 0U; index < result.warning_count; ++index) {
        if (strcmp(result.warnings[index].code, "low_coverage") == 0) {
            warned = 1;
        }
    }
    CHECK(warned, "low stable coverage did not produce a warning");
    hwa_alignment_free(&result);
}

static void test_accelerated_pair_confidence(void)
{
    HWAAlignFrame reference_frames[20];
    HWAAlignFrame target_frames[39];
    HWAAlignTrack reference;
    HWAAlignTrack target;
    HWAAlignmentOptions options = test_options();
    HWAAlignment result;
    char error[HWA_ERROR_SIZE];
    size_t row;
    size_t output = 0U;

    memset(&reference, 0, sizeof(reference));
    memset(&target, 0, sizeof(target));
    for (row = 0U; row < 20U; ++row) {
        fill_frame(&reference_frames[row], row, 0.05,
                   note_classes[row % 10U]);
    }
    fill_frame(&target_frames[output], output, 0.05, note_classes[0]);
    output++;
    for (row = 1U; row < 20U; ++row) {
        unsigned note = note_classes[row % 10U];

        fill_frame(&target_frames[output], output, 0.05,
                   (note + 6U) % HWA_CHROMA_BIN_COUNT);
        output++;
        fill_frame(&target_frames[output], output, 0.05, note);
        output++;
    }
    reference.frames = reference_frames;
    reference.frame_count = 20U;
    reference.step_seconds = 0.05;
    reference.duration_seconds = 1.0;
    reference.tuning_confidence = 1.0;
    target.frames = target_frames;
    target.frame_count = output;
    target.step_seconds = 0.05;
    target.duration_seconds = (double)output * 0.05;
    target.tuning_confidence = 1.0;
    options.dtw_band_seconds = 2.0;
    options.fine_radius_seconds = 0.40;
    options.skip_cost = 5.0;
    options.repeat_cost = 5.0;
    options.match_threshold = 0.80;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "accelerated-pair alignment failed: %s", error);
    CHECK(result.path_points == 20U && result.match_count != 0U,
          "accelerated-pair fixture did not use the expected 1:2 path");
    CHECK(result.matches != NULL &&
              result.matches[0].confidence < options.match_threshold &&
              result.matches[0].status == HWA_ALIGNMENT_LOW_CONFIDENCE,
          "1:2 confidence ignored the paired target frame");
    hwa_alignment_free(&result);
    CHECK(hwa_align_tracks(&target, &reference, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "accelerated-pair reverse alignment failed: %s", error);
    CHECK(result.path_points == 20U && result.match_count != 0U &&
              result.matches != NULL &&
              result.matches[0].confidence < options.match_threshold &&
              result.matches[0].status == HWA_ALIGNMENT_LOW_CONFIDENCE,
          "2:1 confidence ignored the paired reference frame");
    hwa_alignment_free(&result);
}

static void test_accelerated_context_marks_interior_gap(void)
{
    HWAAlignFrame reference_frames[20];
    HWAAlignFrame target_frames[40];
    HWAAlignTrack reference;
    HWAAlignTrack target;
    HWAAlignmentOptions options = test_options();
    HWAAlignment result;
    char error[HWA_ERROR_SIZE];
    size_t row;
    size_t output = 0U;
    int found_repeat = 0;
    int found_edge = 0;

    memset(&reference, 0, sizeof(reference));
    memset(&target, 0, sizeof(target));
    for (row = 0U; row < 20U; ++row) {
        fill_frame(&reference_frames[row], row, 0.05,
                   note_classes[row % 10U]);
    }
    fill_frame(&target_frames[output], output, 0.05, note_classes[0]);
    output++;
    for (row = 1U; row < 20U; ++row) {
        unsigned note = note_classes[row % 10U];

        if (row == 10U) {
            fill_frame(&target_frames[output], output, 0.05,
                       (note + 6U) % HWA_CHROMA_BIN_COUNT);
            output++;
        }
        fill_frame(&target_frames[output], output, 0.05, note);
        output++;
        fill_frame(&target_frames[output], output, 0.05, note);
        output++;
    }
    reference.frames = reference_frames;
    reference.frame_count = 20U;
    reference.step_seconds = 0.05;
    reference.duration_seconds = 1.0;
    reference.tuning_confidence = 1.0;
    target.frames = target_frames;
    target.frame_count = output;
    target.step_seconds = 0.05;
    target.duration_seconds = (double)output * 0.05;
    target.tuning_confidence = 1.0;
    options.dtw_band_seconds = 2.0;
    options.fine_radius_seconds = 0.40;
    options.skip_cost = 2.0;
    options.repeat_cost = 0.45;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "accelerated-gap alignment failed: %s", error);
    for (row = 0U; row < result.unmatched_span_count; ++row) {
        const HWAUnmatchedSpan *span = &result.unmatched_spans[row];

        if (span->side != HWA_ALIGNMENT_TARGET) {
            continue;
        }
        if (span->reason == HWA_UNMATCHED_REPEAT) {
            found_repeat = 1;
        } else if (span->reason == HWA_UNMATCHED_PREFIX ||
                   span->reason == HWA_UNMATCHED_SUFFIX) {
            found_edge = 1;
        }
    }
    CHECK(found_repeat && !found_edge,
          "a gap between 1:2 moves was mislabeled as a timeline edge");
    hwa_alignment_free(&result);
}

static void test_score_event_uses_interior_path(void)
{
    const size_t event_count = 128U;
    HWAAlignFrame reference_frames[80];
    HWAAlignFrame target_frames[80];
    HWAAlignEvent *events = (HWAAlignEvent *)calloc(event_count,
                                                    sizeof(*events));
    HWAAlignTrack reference;
    HWAAlignTrack target;
    HWAAlignmentOptions options = test_options();
    HWAAlignment result;
    char error[HWA_ERROR_SIZE];
    size_t index;

    CHECK(events != NULL, "could not allocate overlapping score events");
    if (events == NULL) {
        return;
    }
    make_constant_track(reference_frames, 80U, 0.05, 1, &reference);
    make_constant_track(target_frames, 80U, 0.05, 0, &target);
    for (index = 20U; index < 60U; ++index) {
        fill_frame(&target_frames[index], index, 0.05, 6U);
    }
    for (index = 0U; index < event_count; ++index) {
        events[index].start_seconds = 0.0;
        events[index].end_seconds = 4.0;
        events[index].start_beat = 0.0;
        events[index].end_beat = 8.0;
        events[index].status = index == 0U ? HWA_ALIGNMENT_REST
                                           : HWA_ALIGNMENT_MATCHED;
        events[index].tempo_bpm = 120.0;
        events[index].tempo_valid = 1;
    }
    reference.events = events;
    reference.event_count = event_count;
    options.dtw_band_seconds = 2.0;
    options.fine_radius_seconds = 0.40;
    options.skip_cost = 5.0;
    options.repeat_cost = 5.0;
    options.match_threshold = 0.80;
    CHECK(hwa_align_tracks(&reference, &target, &options,
                           NULL, 0U, &result,
                           error, sizeof(error)) == 0,
          "overlapping score-event alignment failed: %s", error);
    CHECK(result.match_count == event_count && result.matches != NULL &&
              result.matches[0].status == HWA_ALIGNMENT_REST &&
              result.matches[0].confidence < options.match_threshold &&
              result.matches[1].status == HWA_ALIGNMENT_LOW_CONFIDENCE,
          "score event confidence ignored its interior or lost semantic status");
    CHECK(result.matched_coverage < 0.05 &&
              result.unmatched_span_count >= event_count,
          "low-confidence semantic score events stayed in stable coverage");
    hwa_alignment_free(&result);
    free(events);
}

int main(void)
{
    test_identity_and_rubato();
    test_pickup_missing_and_repeat();
    test_coarse_horizontal_run_corridor();
    test_locked_anchors();
    test_large_and_collapsed_locked_anchors();
    test_locked_endpoint_and_score_rules();
    test_exact_limits();
    test_no_evidence_and_bad_track();
    test_refinement_anchor_collision();
    test_one_frame_and_huge_radii();
    test_public_result_initialization();
    test_one_frame_unmatched_span();
    test_room_tail();
    test_noise_and_tuning_offset();
    test_trill();
    test_audio_match_uses_interior_path();
    test_low_confidence_reduces_stable_coverage();
    test_accelerated_pair_confidence();
    test_accelerated_context_marks_interior_gap();
    test_score_event_uses_interior_path();

    if (failures != 0) {
        (void)fprintf(stderr, "%d alignment test(s) failed\n", failures);
        return 1;
    }
    return 0;
}

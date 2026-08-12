#ifndef HWA_ALIGNMENT_H
#define HWA_ALIGNMENT_H

#include "hlolli_wg_analyzer.h"

#include <stddef.h>
#include <stdint.h>

enum {
    HWA_ALIGN_FRAME_REST = 1U << 0,
    HWA_ALIGN_FRAME_ORNAMENT = 1U << 1,
    HWA_ALIGN_FRAME_CADENZA = 1U << 2,
    HWA_ALIGN_FRAME_REPEAT = 1U << 3,
    HWA_ALIGN_FRAME_TIED = 1U << 4
};

typedef struct HWAAlignFrame {
    double time_seconds;
    double score_beat;
    double chroma[HWA_CHROMA_BIN_COUNT];
    double spectral_onset;
    double energy_onset;
    double phase_onset;
    double combined_onset;
    double log_energy;
    double pitch_class;
    double pitch_confidence;
    double activity;
    uint32_t evidence_flags;
    uint32_t score_flags;
    size_t event_index;
    int score_beat_valid;
} HWAAlignFrame;

typedef struct HWAAlignEvent {
    double start_seconds;
    double end_seconds;
    double start_beat;
    double end_beat;
    HWAAlignmentStatus status;
    const char *event_id;
    const char *kind;
    const char *voice;
    const char *midi_note;
    const char *velocity;
    const char *tie;
    const char *dynamic;
    const char *mark;
    const char *score_position;
    double tempo_bpm;
    int tempo_valid;
} HWAAlignEvent;

/* The owner of a track also owns its frame and event arrays and strings. */
typedef struct HWAAlignTrack {
    const HWAAlignFrame *frames;
    size_t frame_count;
    const HWAAlignEvent *events;
    size_t event_count;
    double step_seconds;
    double duration_seconds;
    double tuning_offset_cents;
    double tuning_confidence;
    int is_score;
} HWAAlignTrack;

/*
 * Align two already-built tracks. The function initializes result fields that
 * describe the alignment but does not set its paths, hashes, or mode. Locked
 * anchors use reference_seconds and target_seconds and must increase on both
 * axes. Caller-owned track and anchor memory remains unchanged.
 */
int hwa_align_tracks(const HWAAlignTrack *reference,
                     const HWAAlignTrack *target,
                     const HWAAlignmentOptions *options,
                     const HWAAlignmentAnchor *locked_anchors,
                     size_t locked_anchor_count,
                     HWAAlignment *result,
                     char *error,
                     size_t error_size);

/* Build a uniform track by aggregating native analysis frames. */
int hwa_align_track_from_analysis(const HWAAnalysis *analysis,
                                  double step_seconds,
                                  uint64_t max_work_bytes,
                                  size_t max_points,
                                  HWAAlignFrame **owned_frames,
                                  HWAAlignTrack *track,
                                  char *error,
                                  size_t error_size);

void hwa_align_track_release(HWAAlignFrame *frames,
                             HWAAlignTrack *track);

#endif

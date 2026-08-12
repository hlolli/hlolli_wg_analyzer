#ifndef HWA_SCORE_MANIFEST_H
#define HWA_SCORE_MANIFEST_H

#include "alignment.h"

#include <stddef.h>
#include <stdint.h>

typedef enum HWAScoreEventKind {
    HWA_SCORE_NOTE = 1,
    HWA_SCORE_REST = 2,
    HWA_SCORE_TEMPO = 3,
    HWA_SCORE_ORNAMENT = 4,
    HWA_SCORE_CADENZA = 5
} HWAScoreEventKind;

typedef enum HWAScoreTie {
    HWA_SCORE_TIE_NONE = 0,
    HWA_SCORE_TIE_START = 1,
    HWA_SCORE_TIE_CONTINUE = 2,
    HWA_SCORE_TIE_STOP = 3
} HWAScoreTie;

typedef struct HWAScoreEvent {
    char *event_id;
    char *kind_text;
    char *midi_note_text;
    char *velocity_text;
    char *voice;
    char *tie_text;
    char *dynamic;
    char *mark;
    char *score_position;
    HWAScoreEventKind kind;
    HWAScoreTie tie;
    double start_beats;
    double duration_beats;
    double end_beats;
    double start_seconds;
    double end_seconds;
    double tempo_bpm;
    int midi_note;
    int velocity;
    int midi_note_valid;
    int velocity_valid;
    int tempo_valid;
    size_t source_row;
} HWAScoreEvent;

typedef struct HWAScoreTempoPoint {
    double beat;
    double seconds;
    double bpm;
    size_t event_index;
} HWAScoreTempoPoint;

typedef struct HWAScoreManifest {
    char *path;
    char sha256[HWA_SHA256_HEX_SIZE];
    HWAScoreEvent *events;
    size_t event_count;
    HWAScoreTempoPoint *tempo_points;
    size_t tempo_count;
    uint64_t tempo_lookup_steps;
    double duration_beats;
    double duration_seconds;
} HWAScoreManifest;

int hwa_score_manifest_load(const char *path,
                            uint64_t max_bytes,
                            size_t max_events,
                            HWAScoreManifest *manifest,
                            char *error,
                            size_t error_size);

void hwa_score_manifest_free(HWAScoreManifest *manifest);

int hwa_score_manifest_beat_to_seconds(const HWAScoreManifest *manifest,
                                       double beat,
                                       double *seconds);

int hwa_score_manifest_seconds_to_beat(const HWAScoreManifest *manifest,
                                       double seconds,
                                       double *beat);

/*
 * Build the uniform score track consumed by hwa_align_tracks(). Event strings
 * remain owned by manifest, which must outlive the alignment call.
 */
int hwa_score_manifest_build_track(const HWAScoreManifest *manifest,
                                   double step_seconds,
                                   uint64_t max_work_bytes,
                                   size_t max_points,
                                   HWAAlignFrame **owned_frames,
                                   HWAAlignEvent **owned_events,
                                   HWAAlignTrack *track,
                                   char *error,
                                   size_t error_size);

void hwa_score_manifest_release_track(HWAAlignFrame *frames,
                                      HWAAlignEvent *events,
                                      HWAAlignTrack *track);

#endif

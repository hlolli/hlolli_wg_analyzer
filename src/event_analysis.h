#ifndef HWA_EVENT_ANALYSIS_H
#define HWA_EVENT_ANALYSIS_H

#include "hlolli_wg_analyzer.h"

typedef struct HWAEventAnalysisOptions {
    HWAAnalysisOptions analysis;
    double min_pitch_confidence;
    double pitch_split_semitones;
    double onset_split_strength;
    size_t min_note_points;
    size_t max_gap_points;
    HWAEventBundleLimits bundle_limits;
} HWAEventAnalysisOptions;

void hwa_event_analysis_options_default(HWAEventAnalysisOptions *options);

int hwa_event_analysis_options_validate(
    const HWAEventAnalysisOptions *options,
    char *error,
    size_t error_size);

/* Analyze one named WAVE file and create one new hwa-events directory. */
int hwa_analyze_events_wav(const char *input_path,
                           const char *output_directory,
                           const HWAEventAnalysisOptions *options,
                           char *error,
                           size_t error_size);

#endif

#ifndef HWA_SPEECH_TEMPORAL_H
#define HWA_SPEECH_TEMPORAL_H

#include "speech_span_audio.h"

typedef struct HWASpeechTemporalFrame {
    uint64_t start_sample;
    uint64_t end_sample;
    /* Reference first, candidate second. Unavailable measurements are NaN. */
    double level[2];
    double spectral_change[2];
    double spectral_acceleration[2];
    double spectral_distance;
    int edited;
} HWASpeechTemporalFrame;

typedef struct HWASpeechTemporal {
    HWASpeechTemporalFrame *frames;
    size_t count;
    size_t window_samples;
    size_t hop_samples;
    uint64_t retained_bytes;
} HWASpeechTemporal;

/* Measure the original shared clock. This call charges decoded frames and
 * transforms. The caller accounts for retained_bytes while keeping the result.
 * The reader cursors remain untouched. */
int hwa_speech_temporal_measure(const HWAWavReader *reference, const HWAWavReader *candidate,
                                double reference_rms, double candidate_rms, HWASpeechWork *work,
                                HWASpeechTemporal *result, char *error, size_t error_size);

void hwa_speech_temporal_free(HWASpeechTemporal *result);

#endif

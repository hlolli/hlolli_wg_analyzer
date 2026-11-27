#ifndef HWA_SPEECH_SPAN_DSP_H
#define HWA_SPEECH_SPAN_DSP_H

#include "internal.h"

#define HWA_SPEECH_HARMONICS 64U
#define HWA_SPEECH_FIXED_PEAKS 8U
#define HWA_SPEECH_BUZZ_PROBE_LIMIT (HWA_SPEECH_FIXED_PEAKS + HWA_SPEECH_HARMONICS)
#define HWA_SPEECH_BUZZ_LEVEL_FLOOR_DB (-40.0)

typedef struct HWASpeechWork {
    uint64_t decoded_frames;
    uint64_t transforms;
    uint64_t max_frames;
    uint64_t max_transforms;
    uint64_t available_bytes;
} HWASpeechWork;

typedef struct HWASpeechPeak {
    int present;
    double frequency_hz;
    double persistence;
    double strength_db;
    double relative_level_db;
    double harmonic_number;
} HWASpeechPeak;

typedef struct HWASpeechBuzz {
    const char *status;
    const char *reason;
    double pitch_min_hz;
    double pitch_max_hz;
    double pitch_coverage;
    double frequency_resolution_hz;
    HWASpeechPeak fixed;
    HWASpeechPeak harmonic;
    HWASpeechPeak fixed_peaks[HWA_SPEECH_FIXED_PEAKS];
    HWASpeechPeak harmonics[HWA_SPEECH_HARMONICS];
} HWASpeechBuzz;

typedef struct HWASpeechContrast {
    const char *status;
    const char *reason;
    double level_contrast_db;
    double high_band_contrast_db;
    double attack_contrast_db;
} HWASpeechContrast;

int hwa_speech_charge_transforms(HWASpeechWork *work, uint64_t count,
                                 char *error, size_t error_size);

int hwa_speech_buzz(const double *samples, size_t count, unsigned rate,
                    double max_hz, HWASpeechWork *work, HWASpeechBuzz *result,
                    char *error, size_t error_size);

/* Measure the candidate peak frequencies or harmonic numbers in one pass
 * over the signal. Results use the same order as the targets. */
int hwa_speech_buzz_probe(const double *samples, size_t count, unsigned rate, double max_hz,
                          const HWASpeechPeak *const *targets, size_t target_count,
                          HWASpeechWork *work, HWASpeechPeak *results, char *error, size_t error_size);

int hwa_speech_contrast(const double *transition, size_t transition_count,
                        const double *vowel, size_t vowel_count,
                        unsigned rate, double max_hz,
                        HWASpeechWork *work, HWASpeechContrast *result,
                        char *error, size_t error_size);

#endif

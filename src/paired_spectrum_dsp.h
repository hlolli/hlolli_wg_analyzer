#ifndef HWA_PAIRED_SPECTRUM_DSP_H
#define HWA_PAIRED_SPECTRUM_DSP_H

#include "internal.h"

typedef struct HWAPairedSpectrumBand {
    double low_hz;
    double high_hz;
} HWAPairedSpectrumBand;

typedef struct HWAPairedSpectrumDspOptions {
    size_t fft_size;
    size_t hop_size;
    double pitch_tolerance_cents;
    double min_pitch_confidence;
    double normalization_min_hz;
    double normalization_max_hz;
    uint64_t max_decoded_frames;
    uint64_t max_transforms;
    uint64_t max_work_bytes;
} HWAPairedSpectrumDspOptions;

typedef struct HWAPairedSpectrumWork {
    uint64_t decoded_frames;
    uint64_t transforms;
    uint64_t peak_bytes;
} HWAPairedSpectrumWork;

typedef enum HWAPairedSpectrumSideStatus {
    HWA_PAIRED_SPECTRUM_SIDE_OK,
    HWA_PAIRED_SPECTRUM_SIDE_TOO_SHORT,
    HWA_PAIRED_SPECTRUM_SIDE_NO_SIGNAL,
    HWA_PAIRED_SPECTRUM_SIDE_PITCH_UNVERIFIED,
    HWA_PAIRED_SPECTRUM_SIDE_PITCH_MISMATCH,
    HWA_PAIRED_SPECTRUM_SIDE_UNSUPPORTED_BAND
} HWAPairedSpectrumSideStatus;

typedef struct HWAPairedSpectrumSide {
    HWAPairedSpectrumSideStatus status;
    size_t fft_size;
    size_t hop_size;
    uint64_t window_count;
    uint64_t usable_windows;
    uint64_t pitch_windows;
    double total_power;
    double measured_pitch_hz;
    double pitch_confidence;
} HWAPairedSpectrumSide;

void hwa_paired_spectrum_dsp_options_default(HWAPairedSpectrumDspOptions *options);
int hwa_paired_spectrum_dsp_options_validate(const HWAPairedSpectrumDspOptions *options,
                                             char *error,
                                             size_t error_size);
const char *hwa_paired_spectrum_side_status_text(HWAPairedSpectrumSideStatus status);

/* Measure complete windows inside [start, end) without changing the reader
 * cursor. Zero FFT and hop sizes select an adaptive frame and half-frame hop.
 * A zero expected pitch disables pitch verification. Power averages accepted
 * windows and channels in squared full-scale sample units. total_power spans
 * the normalization band. A zero normalization maximum means Nyquist.
 * Work accumulates across calls, with peak_bytes tracking the largest FFT
 * workspace. */
int hwa_paired_spectrum_measure(HWAWavReader *reader,
                                uint64_t start,
                                uint64_t end,
                                double expected_pitch_hz,
                                const HWAPairedSpectrumBand *bands,
                                size_t band_count,
                                const HWAPairedSpectrumDspOptions *options,
                                HWAPairedSpectrumWork *work,
                                HWAPairedSpectrumSide *result,
                                double *band_powers,
                                char *error,
                                size_t error_size);

#endif

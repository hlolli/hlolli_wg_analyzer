#ifndef HWA_PAIRED_SPECTRUM_REPORT_H
#define HWA_PAIRED_SPECTRUM_REPORT_H

#include "paired_spectrum_dsp.h"

#define HWA_PAIRED_SPECTRUM_MAX_BANDS 32U
#define HWA_PAIRED_SPECTRUM_MAX_GROUP_LABELS 8U

typedef struct HWAPairedSpectrumReportOptions {
    HWAPairedSpectrumDspOptions dsp;
    HWAPairedSpectrumBand normalization_band;
    HWAPairedSpectrumBand bands[HWA_PAIRED_SPECTRUM_MAX_BANDS];
    const char *group_labels[HWA_PAIRED_SPECTRUM_MAX_GROUP_LABELS];
    size_t band_count;
    size_t group_count;
    double held_fraction;
    double min_pair_confidence;
} HWAPairedSpectrumReportOptions;

int hwa_paired_spectrum_report(const char *reference_path,
                               const char *target_path,
                               const char *alignment_path,
                               const char *alignment_reference_path,
                               const char *alignment_target_path,
                               const char *notes_path,
                               const char *score_alignment_path,
                               int notes_on_target,
                               const char *output_path,
                               int replace,
                               const HWAAnalysisOptions *limits,
                               const HWAPairedSpectrumReportOptions *options,
                               char *error,
                               size_t error_size);

#endif

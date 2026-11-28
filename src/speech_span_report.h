#ifndef HWA_SPEECH_SPAN_REPORT_H
#define HWA_SPEECH_SPAN_REPORT_H

#include "internal.h"

typedef struct HWASpeechReportOptions {
    const char *reference_path;
    const char *candidate_path;
    const char *alignment_path;
    const char *alignment_reference_path;
    const char *alignment_candidate_path;
    const char *spans_path;
    const char *output_path;
    const char *excerpt_dir;
    double min_confidence;
    int shared_timing;
    int replace;
} HWASpeechReportOptions;

int hwa_speech_span_report(const HWASpeechReportOptions *options,
                           const HWAAnalysisOptions *limits, char *error,
                           size_t error_size);

#endif

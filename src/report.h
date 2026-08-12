#ifndef HWA_REPORT_H
#define HWA_REPORT_H

#include "hlolli_wg_analyzer.h"

#include <stdio.h>

int hwa_report_analysis_json(FILE *stream, const HWAAnalysis *analysis);
int hwa_report_analysis_text(FILE *stream, const HWAAnalysis *analysis);
int hwa_report_compare_json(FILE *stream,
                            const HWAAnalysis *reference,
                            const HWAAnalysis *model);
int hwa_report_compare_text(FILE *stream,
                            const HWAAnalysis *reference,
                            const HWAAnalysis *model);
int hwa_report_frames_csv(FILE *stream, const HWAAnalysis *analysis);
int hwa_report_spectrogram_csv(FILE *stream, const HWAAnalysis *analysis);

#endif

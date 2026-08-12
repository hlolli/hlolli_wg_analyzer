#ifndef HWA_EXPERIMENT_REPORT_H
#define HWA_EXPERIMENT_REPORT_H

#include "hlolli_wg_analyzer.h"

#include <stdio.h>

int hwa_report_experiment_text(FILE *stream,
                               const HWAExperimentResult *result);
int hwa_report_experiment_json(FILE *stream,
                               const HWAExperimentResult *result);

#endif

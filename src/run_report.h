#ifndef HWA_RUN_REPORT_H
#define HWA_RUN_REPORT_H

#include "hlolli_wg_analyzer.h"

#include <stdio.h>

int hwa_report_run_text(FILE *stream, const HWARunResult *result);
int hwa_report_run_json(FILE *stream, const HWARunResult *result);

#endif

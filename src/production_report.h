#ifndef HWA_PRODUCTION_REPORT_H
#define HWA_PRODUCTION_REPORT_H

#include "hlolli_wg_analyzer.h"

#include <stdio.h>

int hwa_report_production_text(FILE *stream,
                               const HWAProductionResult *result);
int hwa_report_production_json(FILE *stream,
                               const HWAProductionResult *result);

#endif

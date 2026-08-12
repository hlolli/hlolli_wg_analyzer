#ifndef HWA_PHYSICAL_REPORT_H
#define HWA_PHYSICAL_REPORT_H

#include "hlolli_wg_analyzer.h"

#include <stdio.h>

int hwa_report_physical_text(FILE *stream,
                             const HWAPhysicalCheckSet *set);
int hwa_report_physical_json(FILE *stream,
                             const HWAPhysicalCheckSet *set);

#endif

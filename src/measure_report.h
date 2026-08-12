#ifndef HWA_MEASURE_REPORT_H
#define HWA_MEASURE_REPORT_H

#include "hlolli_wg_analyzer.h"

#include <stdio.h>

int hwa_report_measurement_text(FILE *stream,
                                const HWAMeasurementSet *set);
int hwa_report_measurement_json(FILE *stream,
                                const HWAMeasurementSet *set);
int hwa_report_profile_comparison_text(
    FILE *stream,
    const HWAProfileComparisonSet *set);
int hwa_report_profile_comparison_json(
    FILE *stream,
    const HWAProfileComparisonSet *set);

#endif

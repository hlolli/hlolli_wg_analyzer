#ifndef HWA_HARMONIC_DECAY_REPORT_H
#define HWA_HARMONIC_DECAY_REPORT_H

#include "hlolli_wg_analyzer.h"

#include <stdio.h>

int hwa_harmonic_decay_report_json(
    FILE *stream,
    const HWAHarmonicDecayResult *result);

#endif

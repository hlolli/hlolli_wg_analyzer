#ifndef HWA_BODY_ENVELOPE_H
#define HWA_BODY_ENVELOPE_H

#include "hlolli_wg_analyzer.h"

#include <stddef.h>
#include <stdint.h>

int hwa_body_envelope_fit_analysis(
    const HWAAnalysis *analysis,
    const HWABodyEnvelopeOptions *options,
    uint64_t outer_retained_work_bytes,
    HWABodyEnvelopeEstimate *estimate,
    uint64_t *fit_evaluations,
    char *error,
    size_t error_size);

#endif

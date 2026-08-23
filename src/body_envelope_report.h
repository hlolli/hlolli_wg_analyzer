#ifndef HWA_BODY_ENVELOPE_REPORT_H
#define HWA_BODY_ENVELOPE_REPORT_H

#include "hlolli_wg_analyzer.h"

#include <stdio.h>

int hwa_body_envelope_report_text(FILE *stream,
                                  const HWABodyEnvelopeResult *result);
int hwa_body_envelope_report_json(FILE *stream,
                                  const HWABodyEnvelopeResult *result);

#endif

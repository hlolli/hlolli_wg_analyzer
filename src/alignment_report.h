#ifndef HWA_ALIGNMENT_REPORT_H
#define HWA_ALIGNMENT_REPORT_H

#include "hlolli_wg_analyzer.h"

#include <stdio.h>

int hwa_report_alignment_json(FILE *stream,
                              const HWAAlignment *alignment);
int hwa_report_alignment_text(FILE *stream,
                              const HWAAlignment *alignment);

#endif

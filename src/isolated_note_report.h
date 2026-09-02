#ifndef HWA_ISOLATED_NOTE_REPORT_H
#define HWA_ISOLATED_NOTE_REPORT_H

#include "hlolli_wg_analyzer.h"

#include <stdio.h>

int hwa_isolated_note_report_json(FILE *stream,
                                  const HWAIsolatedNoteResult *result);

#endif

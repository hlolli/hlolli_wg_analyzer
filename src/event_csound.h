#ifndef HWA_EVENT_CSOUND_H
#define HWA_EVENT_CSOUND_H

#include "hlolli_wg_analyzer.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Write a numeric Csound score for note events on one source clock.
 * Instrument 1 receives pitch Hz in p4, a fixed generic level in p5, the
 * event ID in p6, and the exact half-open sample bounds in p7 and p8.
 */
int hwa_event_csound_score_write(FILE *stream,
                                 const HWAEventBundle *bundle,
                                 uint64_t source_recording_id,
                                 char *error,
                                 size_t error_size);

#endif

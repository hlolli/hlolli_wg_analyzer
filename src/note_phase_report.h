#ifndef HWA_NOTE_PHASE_REPORT_H
#define HWA_NOTE_PHASE_REPORT_H
#include "hlolli_wg_analyzer.h"
#include <stdio.h>
int hwa_note_phase_report_json(FILE *stream, const HWANotePhaseResult *result);
int hwa_note_phase_envelope_report_json(FILE *stream, const HWANotePhaseEnvelopeResult *result);
int hwa_note_phase_frames_report_json(FILE *stream, const HWANotePhaseFramesResult *result,
                                     int include_envelope);
int hwa_note_phase_spectra_report_json(FILE *stream, const HWANotePhaseSpectraResult *result,
                                      int include_envelope);
int hwa_note_phase_partials_report_json(FILE *stream, const HWANotePhaseSpectraResult *result,
    const HWAPartialTracks *tracks, int include_envelope, int include_spectra);
#endif

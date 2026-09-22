#ifndef HWA_MUSICXML_INTERNAL_H
#define HWA_MUSICXML_INTERNAL_H

#include "hlolli_wg_analyzer.h"

/* These bits are part of the MusicXML report format. Keep their values stable. */
#define HWA_MUSICXML_BASELINE_POLICY 1U
#define HWA_MUSICXML_PLAYBACK_DATA 2U
#define HWA_MUSICXML_TUPLET_REPAIR 4U
#define HWA_MUSICXML_TEMPO_CHOICE 8U
#define HWA_MUSICXML_OVERFULL_MEASURE 16U
#define HWA_MUSICXML_CUE_NOTATION 32U

#define HWA_MUSICXML_STACCATO 1U
#define HWA_MUSICXML_STACCATISSIMO 2U
#define HWA_MUSICXML_TENUTO 4U
#define HWA_MUSICXML_ACCENT 8U
#define HWA_MUSICXML_STRONG_ACCENT 16U

/* Beat order, then tempo before other events, then source order. */
int hwa_musicxml_event_order(const void *left, const void *right);

int hwa_musicxml_perform(HWAMusicXMLScore *score, const HWAMusicXMLOptions *options,
                         uint64_t work_bytes, char *error, size_t error_size);

#endif

#ifndef HWA_STEM_NOTE_DERIVATION_H
#define HWA_STEM_NOTE_DERIVATION_H

#include "inference_provider.h"

#include <stddef.h>
#include <stdint.h>

/* One caller-owned byte source, keyed by its instrument-stem audio ID. */
typedef struct HWAStemNoteAudioSource {
    uint64_t audio_id;
    HWAByteSource bytes;
} HWAStemNoteAudioSource;

typedef struct HWAStemNoteDerivationOptions {
    const char *note_task;
    const char *note_settings_json;
    uint64_t seed;
    uint64_t max_input_file_bytes;
    uint64_t max_input_bytes;
    uint64_t timeout_milliseconds;
    size_t max_note_events;
    HWAEventBundleLimits output_limits;
} HWAStemNoteDerivationOptions;

typedef struct HWAStemNoteDerivation HWAStemNoteDerivation;

void hwa_stem_note_derivation_options_default(
    HWAStemNoteDerivationOptions *options);

/*
 * Run one raw-audio note task against each instrument stem and merge its
 * notes into a copy of stem_bundle. The bundle must have the exact logical
 * shape of org.hlolli.instrument-stems-v1. Each stem needs one byte-source
 * binding.
 *
 * The note provider receives one source-recording WAVE at a time. Its output
 * must contain one matching source row, one provider row, zero or more note
 * events, and no payloads, traces, or warnings. Event bounds must use the
 * stem clock. Merged notes use the stem as evidence and its region as parent;
 * the region owns the stem ID, so each note keeps empty voice, score-part,
 * and score-event fields. The provider is borrowed and used in stem-ID order.
 *
 * Success gives the caller one owned result. The returned output stays valid
 * until hwa_stem_note_derivation_free. Byte-source contexts and their bytes
 * remain caller-owned and must stay valid for that lifetime.
 */
int hwa_stem_note_derivation_run(
    const HWAEventBundle *stem_bundle,
    const HWAStemNoteAudioSource *stem_sources,
    size_t stem_source_count,
    HWAInferenceProvider *note_provider,
    const HWAStemNoteDerivationOptions *options,
    HWAStemNoteDerivation **result,
    char *error,
    size_t error_size);

const HWAInferenceOutput *hwa_stem_note_derivation_output(
    const HWAStemNoteDerivation *result);

void hwa_stem_note_derivation_free(HWAStemNoteDerivation *result);

#endif

#ifndef HWA_BASIC_PITCH_H
#define HWA_BASIC_PITCH_H

#include <stddef.h>
#include <stdint.h>

#define HWA_BASIC_PITCH_SAMPLE_RATE 22050U
#define HWA_BASIC_PITCH_INPUT_SAMPLES 43844U
#define HWA_BASIC_PITCH_OUTPUT_FRAMES 172U
#define HWA_BASIC_PITCH_NOTE_BINS 88U
#define HWA_BASIC_PITCH_CONTOUR_BINS 264U
#define HWA_BASIC_PITCH_CROP_FRAMES 15U
#define HWA_BASIC_PITCH_KEPT_FRAMES 142U
#define HWA_BASIC_PITCH_FRAME_SAMPLES 256U
#define HWA_BASIC_PITCH_WINDOW_STEP_SAMPLES 36352U
#define HWA_BASIC_PITCH_MIDI_OFFSET 21U

typedef struct HWABasicPitchDecoderOptions {
    double onset_threshold;
    double frame_threshold;
    size_t minimum_note_frames;
    size_t energy_tolerance_frames;
    int infer_onsets;
    int melodia;
} HWABasicPitchDecoderOptions;

typedef struct HWABasicPitchNote {
    uint64_t start_frame;
    uint64_t end_frame;
    uint16_t midi_note;
    double score;
} HWABasicPitchNote;

void hwa_basic_pitch_decoder_options_default(
    HWABasicPitchDecoderOptions *options);

int hwa_basic_pitch_decoder_options_validate(
    const HWABasicPitchDecoderOptions *options,
    char *error,
    size_t error_size);

/*
 * Decode row-major [frame_count, 88] note and onset activations.
 * The caller owns both input arrays. The function owns *notes on success.
 */
int hwa_basic_pitch_decode(
    const float *note_activations,
    const float *onset_activations,
    size_t frame_count,
    const HWABasicPitchDecoderOptions *options,
    size_t max_notes,
    uint64_t max_work_bytes,
    HWABasicPitchNote **notes,
    size_t *note_count,
    char *error,
    size_t error_size);

#endif

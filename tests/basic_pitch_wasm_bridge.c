#include "basic_pitch.h"
#include <stdlib.h>

/* Fixed-window test bridge. Both browser backends use the production decoder. */
static float note_input[HWA_BASIC_PITCH_OUTPUT_FRAMES * HWA_BASIC_PITCH_NOTE_BINS];
static float onset_input[HWA_BASIC_PITCH_OUTPUT_FRAMES * HWA_BASIC_PITCH_NOTE_BINS];
static HWABasicPitchNote *decoded;
static size_t decoded_count;

float *hwa_test_note_buffer(void) { return note_input; }
float *hwa_test_onset_buffer(void) { return onset_input; }

int hwa_test_decode(void)
{
    HWABasicPitchDecoderOptions options;
    char error[512];
    free(decoded);
    decoded = NULL;
    decoded_count = 0U;
    hwa_basic_pitch_decoder_options_default(&options);
    return hwa_basic_pitch_decode(note_input, onset_input,
        HWA_BASIC_PITCH_OUTPUT_FRAMES, &options, 1000U, UINT64_C(16777216),
        &decoded, &decoded_count, error, sizeof(error));
}

int hwa_test_note_count(void) { return (int)decoded_count; }

double hwa_test_note_field(int index, int field)
{
    if (index < 0 || (size_t)index >= decoded_count) return -1.0;
    if (field == 0) return (double)decoded[index].start_frame;
    if (field == 1) return (double)decoded[index].end_frame;
    if (field == 2) return decoded[index].midi_note;
    if (field == 3) return decoded[index].score;
    return -1.0;
}

#include "basic_pitch.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_FRAMES 40U

static int test_overlapping_notes(void)
{
    float notes[TEST_FRAMES * HWA_BASIC_PITCH_NOTE_BINS] = {0.0f};
    float onsets[TEST_FRAMES * HWA_BASIC_PITCH_NOTE_BINS] = {0.0f};
    HWABasicPitchDecoderOptions options;
    HWABasicPitchNote *decoded = NULL;
    size_t decoded_count = 0U;
    char error[256] = {0};
    size_t frame;
    hwa_basic_pitch_decoder_options_default(&options);
    options.infer_onsets = 0;
    options.melodia = 0;
    for (frame = 5U; frame < 25U; ++frame)
        notes[frame * HWA_BASIC_PITCH_NOTE_BINS + 48U] = 0.8f;
    for (frame = 8U; frame < 30U; ++frame)
        notes[frame * HWA_BASIC_PITCH_NOTE_BINS + 52U] = 0.7f;
    onsets[5U * HWA_BASIC_PITCH_NOTE_BINS + 48U] = 0.9f;
    onsets[8U * HWA_BASIC_PITCH_NOTE_BINS + 52U] = 0.85f;
    if (hwa_basic_pitch_decode(
            notes, onsets, TEST_FRAMES, &options, 8U, UINT64_C(1048576),
            &decoded, &decoded_count, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "overlap decode failed: %s\n", error);
        return -1;
    }
    if (decoded_count != 2U ||
        decoded[0].start_frame != UINT64_C(5) ||
        decoded[0].end_frame != UINT64_C(25) ||
        decoded[0].midi_note != 69U ||
        fabs(decoded[0].score - 0.8) > 1e-6 ||
        decoded[1].start_frame != UINT64_C(8) ||
        decoded[1].end_frame != UINT64_C(30) ||
        decoded[1].midi_note != 73U ||
        fabs(decoded[1].score - 0.7) > 1e-6) {
        (void)fputs("overlap decode returned the wrong notes\n", stderr);
        free(decoded);
        return -1;
    }
    free(decoded);
    return 0;
}

static int test_melodia_note(void)
{
    float notes[TEST_FRAMES * HWA_BASIC_PITCH_NOTE_BINS] = {0.0f};
    float onsets[TEST_FRAMES * HWA_BASIC_PITCH_NOTE_BINS] = {0.0f};
    HWABasicPitchDecoderOptions options;
    HWABasicPitchNote *decoded = NULL;
    size_t decoded_count = 0U;
    char error[256] = {0};
    size_t frame;
    hwa_basic_pitch_decoder_options_default(&options);
    options.infer_onsets = 0;
    for (frame = 4U; frame < 28U; ++frame)
        notes[frame * HWA_BASIC_PITCH_NOTE_BINS + 12U] = 0.75f;
    if (hwa_basic_pitch_decode(
            notes, onsets, TEST_FRAMES, &options, 8U, UINT64_C(1048576),
            &decoded, &decoded_count, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "Melodia decode failed: %s\n", error);
        return -1;
    }
    if (decoded_count != 1U || decoded[0].midi_note != 33U ||
        decoded[0].start_frame != UINT64_C(4) ||
        decoded[0].end_frame != UINT64_C(28)) {
        (void)fputs("Melodia decode returned the wrong note\n", stderr);
        free(decoded);
        return -1;
    }
    free(decoded);
    return 0;
}

static int test_melodia_seed_keeps_adjacent_bin(void)
{
    float notes[TEST_FRAMES * HWA_BASIC_PITCH_NOTE_BINS] = {0.0f};
    float onsets[TEST_FRAMES * HWA_BASIC_PITCH_NOTE_BINS] = {0.0f};
    HWABasicPitchDecoderOptions options;
    HWABasicPitchNote *decoded = NULL;
    size_t decoded_count = 0U;
    char error[256] = {0};
    size_t frame;
    hwa_basic_pitch_decoder_options_default(&options);
    options.infer_onsets = 0;
    notes[20U * HWA_BASIC_PITCH_NOTE_BINS + 20U] = 0.9f;
    notes[20U * HWA_BASIC_PITCH_NOTE_BINS + 21U] = 0.8f;
    for (frame = 0U; frame < TEST_FRAMES; ++frame)
        notes[frame * HWA_BASIC_PITCH_NOTE_BINS + 22U] = 0.7f;
    if (hwa_basic_pitch_decode(
            notes, onsets, TEST_FRAMES, &options, 8U, UINT64_C(1048576),
            &decoded, &decoded_count, error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "adjacent-seed decode failed: %s\n", error);
        return -1;
    }
    /*
     * The seed clears only its own bin. The adjacent seed must remain for
     * the next pass, whose scan clears and splits bin 22 into short spans.
     */
    if (decoded_count != 0U) {
        (void)fputs("Melodia seed cleared an adjacent candidate\n", stderr);
        free(decoded);
        return -1;
    }
    free(decoded);
    return 0;
}

static int test_zero_and_invalid(void)
{
    float notes[3U * HWA_BASIC_PITCH_NOTE_BINS] = {0.0f};
    float onsets[3U * HWA_BASIC_PITCH_NOTE_BINS] = {0.0f};
    HWABasicPitchDecoderOptions options;
    HWABasicPitchNote *decoded = (HWABasicPitchNote *)1;
    size_t decoded_count = 99U;
    char error[256] = {0};
    hwa_basic_pitch_decoder_options_default(&options);
    if (hwa_basic_pitch_decode(
            notes, onsets, 3U, &options, 4U, UINT64_C(1048576),
            &decoded, &decoded_count, error, sizeof(error)) != 0 ||
        decoded != NULL || decoded_count != 0U) {
        (void)fputs("zero-note decode failed\n", stderr);
        free(decoded);
        return -1;
    }
    notes[0] = NAN;
    decoded = (HWABasicPitchNote *)1;
    decoded_count = 99U;
    if (hwa_basic_pitch_decode(
            notes, onsets, 3U, &options, 4U, UINT64_C(1048576),
            &decoded, &decoded_count, error, sizeof(error)) == 0 ||
        decoded != NULL || decoded_count != 0U) {
        (void)fputs("NaN activation was accepted\n", stderr);
        return -1;
    }
    notes[0] = 0.0f;
    if (hwa_basic_pitch_decode(
            notes, onsets, 3U, &options, 4U, UINT64_C(1),
            &decoded, &decoded_count, error, sizeof(error)) == 0) {
        (void)fputs("tiny work limit was accepted\n", stderr);
        free(decoded);
        return -1;
    }
    return 0;
}

int main(void)
{
    if (test_overlapping_notes() != 0 ||
        test_melodia_note() != 0 ||
        test_melodia_seed_keeps_adjacent_bin() != 0 ||
        test_zero_and_invalid() != 0)
        return 1;
    (void)puts("Basic Pitch decoder tests passed");
    return 0;
}

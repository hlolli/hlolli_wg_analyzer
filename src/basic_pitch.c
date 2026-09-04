/*
 * Derived from Spotify Basic Pitch's note_creation.py.
 * Copyright 2022 Spotify AB.
 * Modified for this project in 2026.
 * Licensed under the Apache License, Version 2.0. See
 * third_party/basic-pitch/LICENSE and third_party/basic-pitch/NOTICE.
 */

#include "basic_pitch.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void hwa_basic_pitch_error(char *error,
                                  size_t error_size,
                                  const char *message)
{
    if (error == NULL || error_size == 0U) return;
    (void)snprintf(error, error_size, "%s", message);
    error[error_size - 1U] = '\0';
}

void hwa_basic_pitch_decoder_options_default(
    HWABasicPitchDecoderOptions *options)
{
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    options->onset_threshold = 0.5;
    options->frame_threshold = 0.3;
    options->minimum_note_frames = 11U;
    options->energy_tolerance_frames = 11U;
    options->infer_onsets = 1;
    options->melodia = 1;
}

int hwa_basic_pitch_decoder_options_validate(
    const HWABasicPitchDecoderOptions *options,
    char *error,
    size_t error_size)
{
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (options == NULL || !isfinite(options->onset_threshold) ||
        options->onset_threshold < 0.0 ||
        options->onset_threshold > 1.0 ||
        !isfinite(options->frame_threshold) ||
        options->frame_threshold < 0.0 ||
        options->frame_threshold > 1.0 ||
        options->minimum_note_frames == 0U ||
        options->energy_tolerance_frames == 0U ||
        (options->infer_onsets != 0 && options->infer_onsets != 1) ||
        (options->melodia != 0 && options->melodia != 1)) {
        hwa_basic_pitch_error(error, error_size,
                              "invalid Basic Pitch decoder options");
        return -1;
    }
    return 0;
}

static int hwa_basic_pitch_matrix_size(size_t frame_count,
                                       size_t *cell_count,
                                       size_t *byte_count)
{
    size_t cells;
    if (cell_count == NULL || byte_count == NULL ||
        frame_count > SIZE_MAX / HWA_BASIC_PITCH_NOTE_BINS)
        return -1;
    cells = frame_count * HWA_BASIC_PITCH_NOTE_BINS;
    if (cells > SIZE_MAX / sizeof(float)) return -1;
    *cell_count = cells;
    *byte_count = cells * sizeof(float);
    return 0;
}

static int hwa_basic_pitch_activations_valid(const float *values,
                                              size_t count)
{
    size_t index;
    if (values == NULL && count != 0U) return 0;
    for (index = 0U; index < count; ++index) {
        double value = (double)values[index];
        if (!isfinite(value) || value < 0.0 || value > 1.0) return 0;
    }
    return 1;
}

static int hwa_basic_pitch_note_append(HWABasicPitchNote **notes,
                                       size_t *count,
                                       size_t *capacity,
                                       size_t maximum,
                                       uint64_t base_work_bytes,
                                       uint64_t max_work_bytes,
                                       uint64_t start,
                                       uint64_t end,
                                       size_t pitch,
                                       double score,
                                       char *error,
                                       size_t error_size)
{
    HWABasicPitchNote *grown;
    size_t next;
    uint64_t note_bytes;
    if (*count >= maximum) {
        hwa_basic_pitch_error(error, error_size,
                              "Basic Pitch note count exceeds its limit");
        return -1;
    }
    if (*count == *capacity) {
        next = *capacity == 0U ? 64U : *capacity * 2U;
        if (next < *capacity || next > maximum) next = maximum;
        if (next == 0U || next > SIZE_MAX / sizeof(**notes)) {
            hwa_basic_pitch_error(error, error_size,
                                  "Basic Pitch note storage overflows");
            return -1;
        }
        note_bytes = (uint64_t)next * (uint64_t)sizeof(**notes);
        if (note_bytes > max_work_bytes ||
            base_work_bytes > max_work_bytes - note_bytes) {
            hwa_basic_pitch_error(error, error_size,
                                  "Basic Pitch decode exceeds its work limit");
            return -1;
        }
        grown = (HWABasicPitchNote *)realloc(
            *notes, next * sizeof(**notes));
        if (grown == NULL) {
            hwa_basic_pitch_error(error, error_size,
                                  "cannot allocate Basic Pitch notes");
            return -1;
        }
        *notes = grown;
        *capacity = next;
    }
    (*notes)[*count].start_frame = start;
    (*notes)[*count].end_frame = end;
    (*notes)[*count].midi_note =
        (uint16_t)(pitch + HWA_BASIC_PITCH_MIDI_OFFSET);
    (*notes)[*count].score = score;
    (*count)++;
    return 0;
}

static void hwa_basic_pitch_clear_pitch(float *remaining,
                                        size_t frame,
                                        size_t pitch)
{
    size_t base = frame * HWA_BASIC_PITCH_NOTE_BINS;
    remaining[base + pitch] = 0.0f;
    if (pitch != 0U) remaining[base + pitch - 1U] = 0.0f;
    if (pitch + 1U < HWA_BASIC_PITCH_NOTE_BINS)
        remaining[base + pitch + 1U] = 0.0f;
}

static double hwa_basic_pitch_mean(const float *frames,
                                   size_t start,
                                   size_t end,
                                   size_t pitch)
{
    long double sum = 0.0L;
    size_t frame;
    for (frame = start; frame < end; ++frame)
        sum += (long double)frames[
            frame * HWA_BASIC_PITCH_NOTE_BINS + pitch];
    return (double)(sum / (long double)(end - start));
}

static int hwa_basic_pitch_note_compare(const void *left_value,
                                        const void *right_value)
{
    const HWABasicPitchNote *left =
        (const HWABasicPitchNote *)left_value;
    const HWABasicPitchNote *right =
        (const HWABasicPitchNote *)right_value;
    if (left->start_frame < right->start_frame) return -1;
    if (left->start_frame > right->start_frame) return 1;
    if (left->midi_note < right->midi_note) return -1;
    if (left->midi_note > right->midi_note) return 1;
    if (left->end_frame < right->end_frame) return -1;
    if (left->end_frame > right->end_frame) return 1;
    return 0;
}

static void hwa_basic_pitch_infer_onsets(const float *frames,
                                         const float *onsets,
                                         size_t frame_count,
                                         float *result)
{
    size_t pitch;
    size_t frame;
    size_t cell_count = frame_count * HWA_BASIC_PITCH_NOTE_BINS;
    float max_onset = 0.0f;
    float max_difference = 0.0f;
    size_t cell;
    for (cell = 0U; cell < cell_count; ++cell)
        if (onsets[cell] > max_onset) max_onset = onsets[cell];
    for (frame = 0U; frame < frame_count; ++frame) {
        for (pitch = 0U; pitch < HWA_BASIC_PITCH_NOTE_BINS; ++pitch) {
            size_t index = frame * HWA_BASIC_PITCH_NOTE_BINS + pitch;
            float current = frames[index];
            float previous_one = frame >= 1U
                                     ? frames[index - HWA_BASIC_PITCH_NOTE_BINS]
                                     : 0.0f;
            float previous_two = frame >= 2U
                                     ? frames[index -
                                              2U * HWA_BASIC_PITCH_NOTE_BINS]
                                     : 0.0f;
            float difference_one = current - previous_one;
            float difference_two = current - previous_two;
            float difference = difference_one < difference_two
                                   ? difference_one : difference_two;
            if (frame < 2U || difference < 0.0f) difference = 0.0f;
            result[index] = difference;
            if (difference > max_difference) max_difference = difference;
        }
    }
    if (max_difference > 0.0f) {
        float scale = max_onset / max_difference;
        for (cell = 0U; cell < cell_count; ++cell) {
            float inferred = result[cell] * scale;
            result[cell] = inferred > onsets[cell]
                               ? inferred : onsets[cell];
        }
    } else {
        memcpy(result, onsets, cell_count * sizeof(*result));
    }
}

static int hwa_basic_pitch_decode_onsets(
    const float *frames,
    const float *onsets,
    float *remaining,
    size_t frame_count,
    const HWABasicPitchDecoderOptions *options,
    HWABasicPitchNote **notes,
    size_t *note_count,
    size_t *note_capacity,
    size_t max_notes,
    uint64_t base_work_bytes,
    uint64_t max_work_bytes,
    char *error,
    size_t error_size)
{
    size_t time;
    if (frame_count < 3U) return 0;
    for (time = frame_count - 2U; time > 0U; --time) {
        size_t pitch = HWA_BASIC_PITCH_NOTE_BINS;
        while (pitch != 0U) {
            size_t index;
            size_t end;
            size_t below = 0U;
            double score;
            pitch--;
            index = time * HWA_BASIC_PITCH_NOTE_BINS + pitch;
            if ((double)onsets[index] < options->onset_threshold ||
                !(onsets[index] >
                      onsets[index - HWA_BASIC_PITCH_NOTE_BINS] &&
                  onsets[index] >
                      onsets[index + HWA_BASIC_PITCH_NOTE_BINS]))
                continue;
            end = time + 1U;
            while (end < frame_count - 1U &&
                   below < options->energy_tolerance_frames) {
                if ((double)remaining[
                        end * HWA_BASIC_PITCH_NOTE_BINS + pitch] <
                    options->frame_threshold)
                    below++;
                else
                    below = 0U;
                end++;
            }
            end -= below;
            if (end - time <= options->minimum_note_frames) continue;
            for (index = time; index < end; ++index)
                hwa_basic_pitch_clear_pitch(remaining, index, pitch);
            score = hwa_basic_pitch_mean(frames, time, end, pitch);
            if (hwa_basic_pitch_note_append(
                    notes, note_count, note_capacity, max_notes,
                    base_work_bytes, max_work_bytes,
                    (uint64_t)time, (uint64_t)end, pitch, score,
                    error, error_size) != 0)
                return -1;
        }
    }
    return 0;
}

static int hwa_basic_pitch_decode_melodia(
    const float *frames,
    float *remaining,
    size_t frame_count,
    const HWABasicPitchDecoderOptions *options,
    HWABasicPitchNote **notes,
    size_t *note_count,
    size_t *note_capacity,
    size_t max_notes,
    uint64_t base_work_bytes,
    uint64_t max_work_bytes,
    char *error,
    size_t error_size)
{
    size_t cell_count = frame_count * HWA_BASIC_PITCH_NOTE_BINS;
    for (;;) {
        float maximum = 0.0f;
        size_t maximum_index = 0U;
        size_t cell;
        size_t middle;
        size_t pitch;
        size_t end;
        size_t start;
        size_t below;
        double score;
        for (cell = 0U; cell < cell_count; ++cell) {
            if (remaining[cell] > maximum) {
                maximum = remaining[cell];
                maximum_index = cell;
            }
        }
        if ((double)maximum <= options->frame_threshold) break;
        middle = maximum_index / HWA_BASIC_PITCH_NOTE_BINS;
        pitch = maximum_index % HWA_BASIC_PITCH_NOTE_BINS;
        remaining[middle * HWA_BASIC_PITCH_NOTE_BINS + pitch] = 0.0f;

        end = middle + 1U;
        below = 0U;
        while (end < frame_count - 1U &&
               below < options->energy_tolerance_frames) {
            if ((double)remaining[
                    end * HWA_BASIC_PITCH_NOTE_BINS + pitch] <
                options->frame_threshold)
                below++;
            else
                below = 0U;
            hwa_basic_pitch_clear_pitch(remaining, end, pitch);
            end++;
        }
        /* `end` is an exclusive boundary; retain the last active frame. */
        end -= below;

        if (middle == 0U) {
            start = 0U;
        } else {
            size_t cursor = middle - 1U;
            below = 0U;
            while (cursor > 0U &&
                   below < options->energy_tolerance_frames) {
                if ((double)remaining[
                        cursor * HWA_BASIC_PITCH_NOTE_BINS + pitch] <
                    options->frame_threshold)
                    below++;
                else
                    below = 0U;
                hwa_basic_pitch_clear_pitch(remaining, cursor, pitch);
                cursor--;
            }
            start = cursor + 1U + below;
        }
        if (end <= start ||
            end - start <= options->minimum_note_frames)
            continue;
        score = hwa_basic_pitch_mean(frames, start, end, pitch);
        if (hwa_basic_pitch_note_append(
                notes, note_count, note_capacity, max_notes,
                base_work_bytes, max_work_bytes,
                (uint64_t)start, (uint64_t)end, pitch, score,
                error, error_size) != 0)
            return -1;
    }
    return 0;
}

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
    size_t error_size)
{
    float *remaining = NULL;
    float *inferred_onsets = NULL;
    const float *selected_onsets = onset_activations;
    HWABasicPitchNote *result = NULL;
    size_t result_count = 0U;
    size_t result_capacity = 0U;
    size_t cell_count;
    size_t matrix_bytes;
    uint64_t base_work_bytes;
    int status = -1;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (notes == NULL || note_count == NULL) {
        hwa_basic_pitch_error(error, error_size,
                              "Basic Pitch note output is null");
        return -1;
    }
    *notes = NULL;
    *note_count = 0U;
    if (hwa_basic_pitch_decoder_options_validate(
            options, error, error_size) != 0 ||
        max_notes == 0U || max_work_bytes == 0U ||
        hwa_basic_pitch_matrix_size(
            frame_count, &cell_count, &matrix_bytes) != 0 ||
        !hwa_basic_pitch_activations_valid(
            note_activations, cell_count) ||
        !hwa_basic_pitch_activations_valid(
            onset_activations, cell_count)) {
        if (error != NULL && error_size != 0U && error[0] == '\0')
            hwa_basic_pitch_error(error, error_size,
                                  "invalid Basic Pitch activations");
        return -1;
    }
    if (frame_count == 0U) return 0;
    base_work_bytes = (uint64_t)matrix_bytes;
    if (options->infer_onsets) {
        if (base_work_bytes > UINT64_MAX - (uint64_t)matrix_bytes)
            goto work_too_large;
        base_work_bytes += (uint64_t)matrix_bytes;
    }
    if (base_work_bytes > max_work_bytes) goto work_too_large;
    remaining = (float *)malloc(matrix_bytes);
    if (remaining == NULL) goto allocation_failed;
    memcpy(remaining, note_activations, matrix_bytes);
    if (options->infer_onsets) {
        inferred_onsets = (float *)malloc(matrix_bytes);
        if (inferred_onsets == NULL) goto allocation_failed;
        hwa_basic_pitch_infer_onsets(
            note_activations, onset_activations,
            frame_count, inferred_onsets);
        selected_onsets = inferred_onsets;
    }
    if (hwa_basic_pitch_decode_onsets(
            note_activations, selected_onsets, remaining, frame_count,
            options, &result, &result_count, &result_capacity,
            max_notes, base_work_bytes, max_work_bytes,
            error, error_size) != 0)
        goto cleanup;
    if (options->melodia && hwa_basic_pitch_decode_melodia(
            note_activations, remaining, frame_count, options,
            &result, &result_count, &result_capacity,
            max_notes, base_work_bytes, max_work_bytes,
            error, error_size) != 0)
        goto cleanup;
    if (result_count > 1U)
        qsort(result, result_count, sizeof(*result),
              hwa_basic_pitch_note_compare);
    *notes = result;
    *note_count = result_count;
    result = NULL;
    status = 0;
    goto cleanup;

work_too_large:
    hwa_basic_pitch_error(error, error_size,
                          "Basic Pitch decode exceeds its work limit");
    goto cleanup;
allocation_failed:
    hwa_basic_pitch_error(error, error_size,
                          "cannot allocate Basic Pitch decode work");
cleanup:
    free(result);
    free(inferred_onsets);
    free(remaining);
    return status;
}

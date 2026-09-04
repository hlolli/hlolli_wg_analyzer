#ifndef HWA_BASIC_PITCH_AUDIO_PROVIDER_H
#define HWA_BASIC_PITCH_AUDIO_PROVIDER_H

#include "inference_provider.h"

#include <stddef.h>
#include <stdint.h>

#define HWA_BASIC_PITCH_AUDIO_PROVIDER_NAME \
    "org.hlolli.basic-pitch-audio-provider"
#define HWA_BASIC_PITCH_AUDIO_PROVIDER_VERSION "1"
#define HWA_BASIC_PITCH_AUDIO_TASK_NAME \
    "org.hlolli.note-events-on-audio-v1"
#define HWA_BASIC_PITCH_AUDIO_TRANSFORM_NAME \
    "stereo-average-blackman-sinc-127-decimate-2-v1"

/*
 * Adapt stereo IEEE-float32 44100 Hz WAVE audio to Basic Pitch's mono
 * 22050 Hz input without changing the event clock seen by the caller.
 *
 * The fixed transform first takes (left + right) / 2. It then applies a
 * symmetric 127-tap, DC-normalized Blackman-windowed sinc whose cutoff is
 * 0.225 cycles per 44100 Hz input sample. Samples outside the input are zero.
 * Output sample m is centered on input sample 2*m, and the output has
 * ceil(input_frames / 2) frames. A child half-open span [a, b) maps to
 * [2*a, min(input_frames, 2*b)).
 *
 * basic_pitch is borrowed. Its context must stay alive until this provider
 * and all of its tasks have been destroyed. This provider never calls the
 * child's destroy function. It accepts the child's task settings unchanged.
 */
int hwa_basic_pitch_audio_provider_init(
    HWAInferenceProvider *provider,
    const HWAInferenceProvider *basic_pitch,
    const char *adapter_sha256,
    uint64_t max_work_bytes,
    char *error,
    size_t error_size);

#endif

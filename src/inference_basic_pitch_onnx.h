#ifndef HWA_INFERENCE_BASIC_PITCH_ONNX_H
#define HWA_INFERENCE_BASIC_PITCH_ONNX_H

#include "basic_pitch_provider.h"

#include <stddef.h>
#include <stdint.h>

int hwa_inference_basic_pitch_onnx_available(void);

/*
 * Open a bounded, self-contained Basic Pitch ONNX model. On success the
 * runner owns its context, and the caller must either move it into a provider
 * or call runner.destroy(runner.context).
 */
int hwa_inference_basic_pitch_onnx_runner_open(
    const char *model_path,
    const char *expected_model_sha256,
    uint64_t max_model_bytes,
    HWABasicPitchRunner *runner,
    char model_sha256[65],
    char *error,
    size_t error_size);

#endif

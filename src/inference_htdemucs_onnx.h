#ifndef HWA_INFERENCE_HTDEMUCS_ONNX_H
#define HWA_INFERENCE_HTDEMUCS_ONNX_H

#include "htdemucs.h"

#include <stddef.h>
#include <stdint.h>

#define HWA_HTDEMUCS_6S_MODEL_SHA256 \
    "7ce55792e2231c93fbf92de95f5fd5b3a5e6c89f7db690dfd693e8f1dce56869"
#define HWA_HTDEMUCS_6S_MODEL_BYTES UINT64_C(136428532)

int hwa_inference_htdemucs_onnx_available(void);

/*
 * Open the pinned, self-contained HTDemucs six-stem ONNX graph. On success
 * the runner owns its context. The caller must move it into the portable
 * adapter or call runner.destroy(runner.context).
 */
int hwa_inference_htdemucs_onnx_runner_open(
    const char *model_path,
    const char *expected_model_sha256,
    uint64_t max_model_bytes,
    HWAHTDemucsModelRunner *runner,
    char model_sha256[65],
    char *error,
    size_t error_size);

#endif

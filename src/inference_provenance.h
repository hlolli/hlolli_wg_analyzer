#ifndef HWA_INFERENCE_PROVENANCE_H
#define HWA_INFERENCE_PROVENANCE_H

#include "inference_provider.h"

typedef struct HWAInferenceRuntimeProvenance {
    const char *name;
    const char *version;
    const char *backend;
    const char *fallback;
    const char *adapter_sha256;
} HWAInferenceRuntimeProvenance;

/*
 * Build provider settings that retain one inference request. Inputs are
 * sorted by ID. Seed and byte sizes use fixed-width decimal strings so JSON
 * number limits cannot change them. The request must already have passed
 * hwa_inference_request_validate. The caller owns *settings_json.
 */
int hwa_inference_provenance_settings_build(
    const HWAInferenceRequest *request,
    const HWAInferenceRuntimeProvenance *runtime,
    uint64_t max_settings_bytes,
    char **settings_json,
    char *error,
    size_t error_size);

#endif

#include "inference_onnx.h"

#include <onnxruntime_c_api.h>

#include <stdio.h>
#include <string.h>

static const char *test_runtime_version(void)
{
    return "test-runtime";
}

const OrtApiBase *OrtGetApiBase(void)
{
    static const OrtApiBase api = {NULL, test_runtime_version};
    return &api;
}

int main(void)
{
    const char *version = hwa_inference_onnx_runtime_version();
    if (!hwa_inference_onnx_available() || version == NULL ||
        strcmp(version, "test-runtime") != 0) {
        (void)fprintf(stderr, "ONNX capability branch returned the wrong state\n");
        return 1;
    }
    (void)puts("ONNX capability test passed");
    return 0;
}

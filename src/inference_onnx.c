#include "inference_onnx.h"

#ifndef HWA_HAVE_ONNX_RUNTIME
#define HWA_HAVE_ONNX_RUNTIME 0
#endif

#if HWA_HAVE_ONNX_RUNTIME
#include <onnxruntime_c_api.h>
#endif

int hwa_inference_onnx_available(void)
{
#if HWA_HAVE_ONNX_RUNTIME
    return 1;
#else
    return 0;
#endif
}

const char *hwa_inference_onnx_runtime_version(void)
{
#if HWA_HAVE_ONNX_RUNTIME
    return OrtGetApiBase()->GetVersionString();
#else
    return "";
#endif
}

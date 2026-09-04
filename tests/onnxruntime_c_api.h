#ifndef HWA_TEST_ONNXRUNTIME_C_API_H
#define HWA_TEST_ONNXRUNTIME_C_API_H

#include <stdint.h>

typedef struct OrtApiBase {
    const void *(*GetApi)(uint32_t version);
    const char *(*GetVersionString)(void);
} OrtApiBase;

const OrtApiBase *OrtGetApiBase(void);

#endif

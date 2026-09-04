#ifndef HWA_HTDEMUCS_H
#define HWA_HTDEMUCS_H

#include "instrument_stem_provider.h"

#include <stddef.h>
#include <stdint.h>

#define HWA_HTDEMUCS_ADAPTER_NAME "org.hlolli.htdemucs-6s-onnx"
#define HWA_HTDEMUCS_ADAPTER_VERSION "1"
#define HWA_HTDEMUCS_SAMPLE_RATE 44100U
#define HWA_HTDEMUCS_CHANNELS 2U
#define HWA_HTDEMUCS_STEM_COUNT 6U
#define HWA_HTDEMUCS_INPUT_SAMPLES 343980U
#define HWA_HTDEMUCS_OVERLAP_SAMPLES 85995U
#define HWA_HTDEMUCS_STRIDE_SAMPLES 257985U
#define HWA_HTDEMUCS_INPUT_CELLS \
    (HWA_HTDEMUCS_CHANNELS * HWA_HTDEMUCS_INPUT_SAMPLES)
#define HWA_HTDEMUCS_OUTPUT_CELLS \
    (HWA_HTDEMUCS_STEM_COUNT * HWA_HTDEMUCS_CHANNELS * \
     HWA_HTDEMUCS_INPUT_SAMPLES)

/*
 * Run one fixed HTDemucs window. Arrays use planar row-major float32:
 * input is [1, 2, 343980], and output is [1, 6, 2, 343980]. The six
 * output rows are drums, bass, other, vocals, guitar, and piano. The
 * runner must fill every output cell with a finite value.
 */
typedef int (*HWAHTDemucsRunWindowFunction)(
    void *context,
    const float input[HWA_HTDEMUCS_INPUT_CELLS],
    float output[HWA_HTDEMUCS_OUTPUT_CELLS],
    char *error,
    size_t error_size);

typedef void (*HWAHTDemucsModelRunnerDestroyFunction)(void *context);

typedef struct HWAHTDemucsModelRunner {
    void *context;
    const char *runtime_name;
    const char *runtime_version;
    const char *backend;
    const char *fallback;
    HWAHTDemucsRunWindowFunction run_window;
    HWAHTDemucsModelRunnerDestroyFunction destroy;
} HWAHTDemucsModelRunner;

/*
 * Wrap a fixed-window model runner as an instrument-stem runner. The adapter
 * accepts mono or stereo 44100 Hz WAVE input and emits six stereo float32
 * WAVE byte sources. It keeps output audio in temporary files. max_work_bytes
 * covers the adapter's explicit heap allocations, not the model runtime.
 *
 * On success, out owns model_runner->context and destroys it through
 * model_runner->destroy. On failure, the caller still owns that context.
 * Runtime strings are copied.
 */
int hwa_htdemucs_instrument_runner_init(
    HWAInstrumentStemRunner *out,
    uint64_t max_work_bytes,
    const HWAHTDemucsModelRunner *model_runner,
    char *error,
    size_t error_size);

#endif

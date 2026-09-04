#ifndef HWA_BASIC_PITCH_PROVIDER_H
#define HWA_BASIC_PITCH_PROVIDER_H

#include "basic_pitch.h"
#include "inference_provider.h"

#include <stddef.h>
#include <stdint.h>

#define HWA_BASIC_PITCH_PROVIDER_NAME "org.hlolli.basic-pitch-onnx"
#define HWA_BASIC_PITCH_PROVIDER_VERSION "1"
#define HWA_BASIC_PITCH_TASK_NAME \
    "org.hlolli.polyphonic-note-events-v1"
#define HWA_BASIC_PITCH_LEFT_PAD_SAMPLES 3840U

/*
 * Run one fixed Basic Pitch window. Arrays use row-major float32 storage:
 * input is [43844], and each output is [172, 88]. The runner must fill both
 * outputs with finite values in [0, 1].
 */
typedef int (*HWABasicPitchRunWindowFunction)(
    void *context,
    const float input[HWA_BASIC_PITCH_INPUT_SAMPLES],
    float note_output[HWA_BASIC_PITCH_OUTPUT_FRAMES *
                      HWA_BASIC_PITCH_NOTE_BINS],
    float onset_output[HWA_BASIC_PITCH_OUTPUT_FRAMES *
                       HWA_BASIC_PITCH_NOTE_BINS],
    char *error,
    size_t error_size);

typedef void (*HWABasicPitchRunnerDestroyFunction)(void *context);

typedef struct HWABasicPitchRunner {
    void *context;
    const char *runtime_name;
    const char *runtime_version;
    const char *backend;
    const char *fallback;
    HWABasicPitchRunWindowFunction run_window;
    HWABasicPitchRunnerDestroyFunction destroy;
} HWABasicPitchRunner;

/*
 * Build the one accepted task-settings object. The caller owns
 * *settings_json on success and must free it with free().
 */
int hwa_basic_pitch_task_settings_build(
    const HWABasicPitchDecoderOptions *decoder_options,
    char **settings_json,
    char *error,
    size_t error_size);

/*
 * Initialize a synchronous provider. On success, the provider owns the
 * runner context and destroys it through runner->destroy. On failure, the
 * caller still owns that context. Runtime strings and hashes are copied.
 */
int hwa_basic_pitch_provider_init(
    HWAInferenceProvider *provider,
    const char *model_sha256,
    const char *adapter_sha256,
    const HWABasicPitchDecoderOptions *decoder_options,
    uint64_t max_work_bytes,
    const HWABasicPitchRunner *runner,
    char *error,
    size_t error_size);

#endif

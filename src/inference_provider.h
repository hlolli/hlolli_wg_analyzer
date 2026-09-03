#ifndef HWA_INFERENCE_PROVIDER_H
#define HWA_INFERENCE_PROVIDER_H

#include "hlolli_wg_analyzer.h"

#include <stddef.h>
#include <stdint.h>

#define HWA_INFERENCE_MAX_INPUTS 4096U

/* One caller-owned, path-free input. Its ID must be unique in a request. */
typedef struct HWAInferenceInput {
    const char *id;
    const char *role;
    const char *media_type;
    const char *sha256;
    HWAByteSource bytes;
} HWAInferenceInput;

/*
 * The request, its input rows, all pointed-to strings, byte-source metadata,
 * and byte content stay caller-owned, valid, and unchanged until task_free.
 * source_input_id picks the sole sample clock from the named inputs.
 * Expected identity fields must match the provider descriptor byte for byte.
 */
typedef struct HWAInferenceRequest {
    const char *task;
    const char *settings_json;
    const char *expected_provider_name;
    const char *expected_provider_version;
    const char *expected_model_sha256;
    uint64_t seed;
    uint64_t source_recording_id;
    const char *source_input_id;
    const HWAInferenceInput *inputs;
    size_t input_count;
    HWAFormat source_format;
    HWAEventBundleLimits output_limits;
} HWAInferenceRequest;

typedef struct HWAInferencePayload {
    const char *relative_path;
    HWAByteSource bytes;
} HWAInferencePayload;

/* Provider-owned rows and byte sources stay unchanged until task_free. */
typedef struct HWAInferenceOutput {
    const HWAEventBundle *bundle;
    const HWAInferencePayload *payloads;
    size_t payload_count;
} HWAInferenceOutput;

typedef enum HWAInferencePollState {
    HWA_INFERENCE_PENDING = 1,
    HWA_INFERENCE_READY = 2
} HWAInferencePollState;

/*
 * start sets *task to null before any work. On success, the caller owns one
 * task and must free it once. On failure, the provider keeps no request data
 * and the caller has no task to free.
 */
typedef int (*HWAInferenceStartFunction)(
    void *context,
    const HWAInferenceRequest *request,
    void **task,
    char *error,
    size_t error_size);

/*
 * Poll must return at once. Pending success sets output to null. Ready success
 * returns provider-owned data that stays valid until task_free; later polls
 * may return the same data. Poll failure sets pending and null output. That
 * failure ends polling, but the caller must still free the task once.
 */
typedef int (*HWAInferencePollFunction)(
    void *context,
    void *task,
    HWAInferencePollState *state,
    const HWAInferenceOutput **output,
    char *error,
    size_t error_size);

/*
 * task_free accepts null. It cancels pending work and releases all task data
 * before it returns. The task keeps no borrowed pointer after it.
 */
typedef void (*HWAInferenceTaskFreeFunction)(void *context, void *task);

/*
 * The caller frees every task before destroy. Destroy releases the provider
 * context and descriptor data. It accepts a null context.
 */
typedef void (*HWAInferenceDestroyFunction)(void *context);

typedef struct HWAInferenceProvider {
    /* These three descriptor strings stay fixed for the provider's lifetime. */
    const char *name;
    const char *version;
    const char *model_sha256;
    void *context;
    HWAInferenceStartFunction start;
    HWAInferencePollFunction poll;
    HWAInferenceTaskFreeFunction task_free;
    HWAInferenceDestroyFunction destroy;
} HWAInferenceProvider;

/*
 * Validate the bundle and every payload binding. This reads each bound byte
 * source in full to check its size and SHA-256 value.
 */
int hwa_inference_output_validate(const HWAInferenceOutput *output,
                                  const HWAEventBundleLimits *limits,
                                  char *error,
                                  size_t error_size);

/* Hash one bounded byte source without treating its display name as a path. */
int hwa_inference_byte_source_sha256(
    const HWAByteSource *source,
    uint64_t max_bytes,
    char sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size);

/* Validate and save one provider output as a new event-bundle directory. */
int hwa_inference_output_write(const char *output_directory,
                               const HWAInferenceOutput *output,
                               const HWAEventBundleLimits *limits,
                               char *error,
                               size_t error_size);

/* Destroy the provider context once, then clear the descriptor. */
void hwa_inference_provider_destroy(HWAInferenceProvider *provider);

/* Test adapter for the fixed org.hlolli.fixed-note contract. */
void hwa_inference_fixed_provider_init(HWAInferenceProvider *provider);

#endif

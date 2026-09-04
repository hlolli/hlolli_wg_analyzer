#ifndef HWA_INSTRUMENT_STEM_PROVIDER_H
#define HWA_INSTRUMENT_STEM_PROVIDER_H

#include "inference_provider.h"

#include <stddef.h>
#include <stdint.h>

#define HWA_INSTRUMENT_STEM_PROVIDER_NAME \
    "org.hlolli.instrument-stem-provider"
#define HWA_INSTRUMENT_STEM_PROVIDER_VERSION "1"
#define HWA_INSTRUMENT_STEM_TASK_NAME \
    "org.hlolli.instrument-stems-v1"

/*
 * One separated WAVE returned by a runner. stem_id is a unique lower-case
 * machine name. instrument is the runner's instrument label. A score is an
 * optional label confidence, not a claim about separation quality.
 */
typedef struct HWAInstrumentStemResult {
    const char *stem_id;
    const char *instrument;
    double score;
    int score_valid;
    HWAByteSource wave;
} HWAInstrumentStemResult;

/*
 * Run one separation task. The source and its bytes stay caller-owned. On
 * return, stems, their strings, byte-source fields, and bytes belong to the
 * runner and stay valid and unchanged until results_destroy. The provider
 * calls results_destroy once, including after a failed run that returned
 * rows. A successful run must return at least one stem. The provider checks
 * and hashes every returned WAVE.
 */
typedef int (*HWAInstrumentStemRunFunction)(
    void *context,
    const HWAByteSource *source,
    const HWAFormat *source_format,
    uint64_t seed,
    uint64_t timeout_milliseconds,
    HWAInstrumentStemResult **stems,
    size_t *stem_count,
    char *error,
    size_t error_size);

typedef void (*HWAInstrumentStemResultsDestroyFunction)(
    void *context,
    HWAInstrumentStemResult *stems,
    size_t stem_count);

typedef void (*HWAInstrumentStemRunnerDestroyFunction)(void *context);

typedef struct HWAInstrumentStemRunner {
    void *context;
    const char *runtime_name;
    const char *runtime_version;
    const char *backend;
    const char *fallback;
    HWAInstrumentStemRunFunction run;
    HWAInstrumentStemResultsDestroyFunction results_destroy;
    HWAInstrumentStemRunnerDestroyFunction destroy;
} HWAInstrumentStemRunner;

/* Check the exact row order, IDs, links, and paths of a valid v1 result. */
int hwa_instrument_stem_bundle_validate_v1(
    const HWAEventBundle *bundle,
    char *error,
    size_t error_size);

/*
 * Initialize the portable synchronous provider. On success, the provider
 * owns the runner context and destroys it through runner->destroy. On
 * failure, the caller still owns that context. Runtime strings and hashes
 * are copied.
 */
int hwa_instrument_stem_provider_init(
    HWAInferenceProvider *provider,
    const char *model_sha256,
    const char *adapter_sha256,
    uint64_t max_work_bytes,
    const HWAInstrumentStemRunner *runner,
    char *error,
    size_t error_size);

#endif

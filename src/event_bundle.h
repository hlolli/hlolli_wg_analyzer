#ifndef HWA_EVENT_BUNDLE_H
#define HWA_EVENT_BUNDLE_H

#include "hlolli_wg_analyzer.h"

typedef struct HWAEventSourceBinding {
    const char *relative_path;
    HWAByteSource source;
} HWAEventSourceBinding;

/* Reject a new output path that is the source bundle or lies below it. */
int hwa_event_bundle_output_path_validate(
    const char *output_directory,
    const char *source_directory,
    char *error,
    size_t error_size);

/* Measure the bundle-owned rows and strings under the given work cap. */
int hwa_event_bundle_measure_work(
    const HWAEventBundle *bundle,
    const HWAEventBundleLimits *limits,
    uint64_t *result,
    char *error,
    size_t error_size);

/* Private byte-source form of the public path-backed bundle writer. */
int hwa_event_bundle_write_sources(
    const char *output_directory,
    const HWAEventBundle *bundle,
    const HWAEventSourceBinding *bindings,
    size_t binding_count,
    const HWAEventBundleLimits *limits,
    char *error,
    size_t error_size);

#endif

#ifndef HWA_EVENT_BUNDLE_H
#define HWA_EVENT_BUNDLE_H

#include "hlolli_wg_analyzer.h"

typedef struct HWAEventSourceBinding {
    const char *relative_path;
    HWAByteSource source;
} HWAEventSourceBinding;

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

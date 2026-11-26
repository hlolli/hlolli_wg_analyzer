#ifndef HWA_INSTRUMENT_ISOLATION_FILE_H
#define HWA_INSTRUMENT_ISOLATION_FILE_H

#include "hlolli_wg_analyzer.h"
#include "instrument_isolation.h"

int hwa_isolation_instrument_from_name(const char *name,
                                       HwaIsolationInstrument *instrument);

/* Write the instrument estimate and removed audio into a new event bundle. */
int hwa_isolate_instrument_wav(const char *source_path,
                               const char *output_directory,
                               HwaIsolationInstrument instrument,
                               const HwaIsolationRange *ranges,
                               size_t range_count,
                               int deconv,
                               const HWAAnalysisOptions *input_limits,
                               const HWAEventBundleLimits *bundle_limits,
                               char *error,
                               size_t error_size);

#endif

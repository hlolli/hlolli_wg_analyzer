#ifndef HWA_FEATURES_H
#define HWA_FEATURES_H

#include "hlolli_wg_analyzer.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Broad spectral bands use these half-open ranges, in hertz. The last band
 * ends at Nyquist:
 *
 *   0-60, 60-120, 120-250, 250-500, 500-1000,
 *   1000-2000, 2000-4000, 4000-8000, 8000-16000, 16000-Nyquist.
 *
 * Band power is mean-square full-scale power. Band width is side RMS divided
 * by mid RMS in that band.
 */

typedef struct HWAFeatureProcessor HWAFeatureProcessor;

/*
 * Create all work storage for a stream with exactly expected_frames frames.
 * A successful create performs every allocation needed by push. Finish moves
 * result arrays to analysis; the caller then owns them through
 * hwa_analysis_free().
 */
int hwa_features_create(HWAFeatureProcessor **processor,
                        uint32_t sample_rate_hz,
                        uint16_t analyzed_channels,
                        uint32_t channel_mask,
                        uint64_t expected_frames,
                        const HWAAnalysisOptions *options,
                        char *error,
                        size_t error_size);

/*
 * Samples and optional clipped flags are interleaved by channel. Each clipped
 * flag is zero or nonzero. Push accepts any block split and does not allocate.
 */
int hwa_features_push(HWAFeatureProcessor *processor,
                      const double *samples,
                      const unsigned char *clipped,
                      size_t frame_count,
                      char *error,
                      size_t error_size);

/* Fill feature fields in analysis and transfer all result array ownership. */
int hwa_features_finish(HWAFeatureProcessor *processor,
                        HWAAnalysis *analysis,
                        char *error,
                        size_t error_size);

void hwa_features_destroy(HWAFeatureProcessor *processor);

#endif

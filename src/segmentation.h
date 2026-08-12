#ifndef HWA_SEGMENTATION_H
#define HWA_SEGMENTATION_H

#include "hlolli_wg_analyzer.h"
#include "typed_labels.h"

#include <stddef.h>

/*
 * Build items from inputs that a caller has already loaded and checked. This
 * entry point exists so the bounded role and boundary engine can be tested
 * without file I/O. It copies every value retained in the result.
 */
int hwa_segmentation_build(const HWAAlignment *alignment,
                           const HWAAnalysis *analysis,
                           const HWATypedLabelSet *labels,
                           const HWASegmentationOptions *options,
                           const HWAItemEdit *edits,
                           size_t edit_count,
                           HWAItemSet *items,
                           char *error,
                           size_t error_size);

#endif

#ifndef HWA_TYPED_LABELS_H
#define HWA_TYPED_LABELS_H

#include "hlolli_wg_analyzer.h"

#include <stddef.h>
#include <stdint.h>

#define HWA_TYPED_LABEL_HEADER                                      \
    "event_id,pitch,register,dynamic,articulation,part,"            \
    "physical_element,controller,technique,score_section,"          \
    "transition,gesture"

typedef struct HWATypedLabelRow {
    char *event_id;
    HWATypedLabels labels;
} HWATypedLabelRow;

typedef struct HWATypedLabelSet {
    char *path;
    char sha256[HWA_SHA256_HEX_SIZE];
    HWATypedLabelRow *rows;
    size_t row_count;
    uint64_t retained_work_bytes;
} HWATypedLabelSet;

int hwa_typed_labels_load(const char *path,
                          uint64_t max_bytes,
                          uint64_t max_work_bytes,
                          size_t max_rows,
                          size_t max_field_bytes,
                          HWATypedLabelSet *labels,
                          char *error,
                          size_t error_size);

const HWATypedLabelRow *hwa_typed_labels_find(
    const HWATypedLabelSet *labels,
    const char *event_id);

void hwa_typed_labels_free(HWATypedLabelSet *labels);

#endif

#ifndef HWA_MEASURE_COMPARE_H
#define HWA_MEASURE_COMPARE_H

#include "hlolli_wg_analyzer.h"

#include <stddef.h>

/*
 * Build the canonical one-factor role groups and their saved distributions.
 * The measurement engine owns contexts and scalar observations; this step
 * fills only groups, group members, and statistics. Those arrays must be
 * empty on entry.
 */
int hwa_measure_build_profile(HWAMeasurementSet *set,
                              char *error,
                              size_t error_size);

/* Names below are part of the saved Stage 4 forms. */
const char *hwa_measure_kind_name(HWAMeasureKind kind);
const char *hwa_measure_unit_name(HWAMeasureUnit unit);
const char *hwa_measure_view_name(HWAMeasureView view);
const char *hwa_measure_status_name(HWAMeasureStatus status);
const char *hwa_measure_group_selector_name(HWAMeasureGroupSelector selector);
const char *hwa_measure_item_kind_name(HWAItemKind kind);

int hwa_measure_kind_from_name(const char *name, HWAMeasureKind *kind);
int hwa_measure_unit_from_name(const char *name, HWAMeasureUnit *unit);
int hwa_measure_view_from_name(const char *name, HWAMeasureView *view);
int hwa_measure_status_from_name(const char *name, HWAMeasureStatus *status);
int hwa_measure_group_selector_from_name(
    const char *name,
    HWAMeasureGroupSelector *selector);
int hwa_measure_item_kind_from_name(const char *name, HWAItemKind *kind);

int hwa_measure_kind_index_valid(HWAMeasureKind kind,
                                 uint32_t index,
                                 size_t max_partials);
int hwa_measure_kind_unit(HWAMeasureKind kind,
                          HWAMeasureView view,
                          HWAMeasureUnit *unit);

#endif

#ifndef HWA_PRODUCTION_H
#define HWA_PRODUCTION_H

#include "hlolli_wg_analyzer.h"

#include <stddef.h>

const char *hwa_production_availability_name(
    HWAProductionAvailability availability);
const char *hwa_production_split_name(HWAProductionSplit split);
const char *hwa_production_view_name(HWAProductionView view);
const char *hwa_production_scope_name(HWAProductionScope scope);
const char *hwa_production_unit_name(HWAProductionUnit unit);
const char *hwa_production_fit_kind_name(HWAProductionFitKind kind);
const char *hwa_production_metric_kind_name(HWAProductionMetricKind kind);

int hwa_production_availability_from_name(
    const char *name,
    HWAProductionAvailability *availability);
int hwa_production_split_from_name(const char *name,
                                   HWAProductionSplit *split);
int hwa_production_view_from_name(const char *name,
                                  HWAProductionView *view);
int hwa_production_scope_from_name(const char *name,
                                   HWAProductionScope *scope);
int hwa_production_unit_from_name(const char *name,
                                  HWAProductionUnit *unit);
int hwa_production_fit_kind_from_name(const char *name,
                                      HWAProductionFitKind *kind);
int hwa_production_metric_kind_from_name(const char *name,
                                         HWAProductionMetricKind *kind);

int hwa_production_fit_shape_valid(HWAProductionFitKind kind,
                                   uint32_t index,
                                   HWAProductionUnit unit);
int hwa_production_metric_shape_valid(HWAProductionMetricKind kind,
                                      uint32_t index,
                                      HWAProductionUnit unit);

size_t hwa_production_metric_catalog_count(void);
int hwa_production_metric_catalog_at(size_t offset,
                                     HWAProductionMetricKind *kind,
                                     uint32_t *index,
                                     HWAProductionUnit *unit);
size_t hwa_production_fit_catalog_count(void);
int hwa_production_fit_catalog_at(size_t offset,
                                  HWAProductionScope *scope,
                                  HWAProductionFitKind *kind,
                                  uint32_t *index,
                                  HWAProductionUnit *unit);

double hwa_production_eq_node_frequency_hz(size_t index);
int hwa_production_eq_node_supported(size_t index,
                                     uint32_t reference_rate_hz,
                                     uint32_t model_rate_hz);
int hwa_production_eq_adjacent_valid(double lower_gain_db,
                                     double upper_gain_db);
int hwa_production_room_band_supported(uint32_t index,
                                       uint32_t rate_hz);
int hwa_production_fit_value_valid(HWAProductionScope scope,
                                   HWAProductionFitKind kind,
                                   uint32_t index,
                                   double value,
                                   uint32_t reference_rate_hz,
                                   uint32_t model_rate_hz);
int hwa_production_metric_value_valid(HWAProductionMetricKind kind,
                                      uint32_t index,
                                      double value,
                                      uint32_t rate_hz);

size_t hwa_production_minimum_train_spans(void);
size_t hwa_production_minimum_check_spans(void);
size_t hwa_production_max_train_spans_per_family(void);
size_t hwa_production_max_check_spans_per_family(void);
size_t hwa_production_threshold_grid_count(void);
double hwa_production_threshold_grid_at(size_t index);
size_t hwa_production_ratio_grid_count(void);
double hwa_production_ratio_grid_at(size_t index);
uint32_t hwa_production_fit_eligibility_flag(HWAProductionFitKind kind);
uint32_t hwa_production_metric_eligibility_flags(HWAProductionView view,
                                                 HWAProductionMetricKind kind);
int hwa_production_raw_metric_applicable(
    const HWAProductionSpan *span,
    HWAProductionMetricKind kind);

typedef struct HWAProductionWarningSpec {
    const char *code;
    const char *message;
    uint64_t span_id;
    uint64_t fit_id;
    int span_id_valid;
    int fit_id_valid;
} HWAProductionWarningSpec;

size_t hwa_production_warning_spec_count(
    const HWAProductionResult *result);
int hwa_production_warning_spec_at(
    const HWAProductionResult *result,
    size_t index,
    HWAProductionWarningSpec *spec);

int hwa_production_evaluation_derive(
    const HWAProductionResult *result,
    size_t span_index,
    HWAProductionView view,
    size_t metric_offset,
    HWAProductionEvaluation *evaluation,
    char *error,
    size_t error_size);

/* Hash the source-event bytes after the first item-key colon. */
int hwa_production_split_for_item_key(const char *item_key,
                                      HWAProductionSplit *split);

#endif

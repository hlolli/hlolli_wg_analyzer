#include "measure_compare.h"

#include "internal.h"
#include "measure_file.h"
#include "sha256.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HWA_MEASURE_EVIDENCE_ALL ((UINT32_C(1) << 11) - 1U)
#define HWA_MEASURE_QUALITY_ALL ((UINT32_C(1) << 7) - 1U)

typedef enum HWAIndexRule {
    HWA_INDEX_SCALAR = 0,
    HWA_INDEX_BAND = 1,
    HWA_INDEX_PARTIAL = 2
} HWAIndexRule;

typedef struct HWAMeasureCatalogEntry {
    HWAMeasureKind kind;
    const char *name;
    HWAMeasureUnit unit;
    HWAIndexRule index_rule;
    int level_like;
} HWAMeasureCatalogEntry;

#define HWA_SCALAR(kind_, name_, unit_) \
    { kind_, name_, unit_, HWA_INDEX_SCALAR, 0 }
#define HWA_LEVEL(kind_, name_) \
    { kind_, name_, HWA_MEASURE_UNIT_DBFS, HWA_INDEX_SCALAR, 1 }
#define HWA_BAND(kind_, name_, unit_, level_) \
    { kind_, name_, unit_, HWA_INDEX_BAND, level_ }
#define HWA_PARTIAL(kind_, name_, unit_, level_) \
    { kind_, name_, unit_, HWA_INDEX_PARTIAL, level_ }

static const HWAMeasureCatalogEntry hwa_measure_catalog[] = {
    HWA_LEVEL(HWA_MEASURE_RMS_DBFS, "rms_dbfs"),
    HWA_LEVEL(HWA_MEASURE_PEAK_DBFS, "peak_dbfs"),
    HWA_SCALAR(HWA_MEASURE_CREST_DB, "crest_db", HWA_MEASURE_UNIT_DB),
    HWA_BAND(HWA_MEASURE_BAND_LEVEL_DBFS, "band_level_dbfs",
             HWA_MEASURE_UNIT_DBFS, 1),
    HWA_BAND(HWA_MEASURE_BAND_BALANCE_DB, "band_balance_db",
             HWA_MEASURE_UNIT_DB, 0),
    HWA_SCALAR(HWA_MEASURE_CENTROID_HZ, "centroid_hz",
               HWA_MEASURE_UNIT_HZ),
    HWA_SCALAR(HWA_MEASURE_FLATNESS, "flatness", HWA_MEASURE_UNIT_RATIO),
    HWA_SCALAR(HWA_MEASURE_LEVEL_SLOPE_DB_PER_SECOND,
               "level_slope_db_per_second", HWA_MEASURE_UNIT_DB_PER_SECOND),
    HWA_SCALAR(HWA_MEASURE_CENTROID_SLOPE_HZ_PER_SECOND,
               "centroid_slope_hz_per_second",
               HWA_MEASURE_UNIT_HZ_PER_SECOND),
    HWA_BAND(HWA_MEASURE_BAND_SLOPE_DB_PER_SECOND,
             "band_slope_db_per_second", HWA_MEASURE_UNIT_DB_PER_SECOND, 0),
    HWA_SCALAR(HWA_MEASURE_TRANSIENT_RATE_HZ, "transient_rate_hz",
               HWA_MEASURE_UNIT_HZ),
    HWA_SCALAR(HWA_MEASURE_FIXED_STATE_FRACTION, "fixed_state_fraction",
               HWA_MEASURE_UNIT_RATIO),
    HWA_SCALAR(HWA_MEASURE_LEVEL_MODULATION_SPREAD_DB,
               "level_modulation_spread_db", HWA_MEASURE_UNIT_DB),
    HWA_SCALAR(HWA_MEASURE_CENTROID_MODULATION_SPREAD_HZ,
               "centroid_modulation_spread_hz", HWA_MEASURE_UNIT_HZ),
    HWA_BAND(HWA_MEASURE_BAND_MODULATION_SPREAD_DB,
             "band_modulation_spread_db", HWA_MEASURE_UNIT_DB, 0),
    HWA_SCALAR(HWA_MEASURE_PITCH_HZ, "pitch_hz", HWA_MEASURE_UNIT_HZ),
    HWA_SCALAR(HWA_MEASURE_TUNING_OFFSET_CENTS, "tuning_offset_cents",
               HWA_MEASURE_UNIT_CENTS),
    HWA_SCALAR(HWA_MEASURE_PITCH_SPREAD_CENTS, "pitch_spread_cents",
               HWA_MEASURE_UNIT_CENTS),
    HWA_SCALAR(HWA_MEASURE_PITCH_OVERSHOOT_CENTS,
               "pitch_overshoot_cents", HWA_MEASURE_UNIT_CENTS),
    HWA_SCALAR(HWA_MEASURE_PITCH_UNDERSHOOT_CENTS,
               "pitch_undershoot_cents", HWA_MEASURE_UNIT_CENTS),
    HWA_SCALAR(HWA_MEASURE_PITCH_DRIFT_CENTS_PER_SECOND,
               "pitch_drift_cents_per_second",
               HWA_MEASURE_UNIT_CENTS_PER_SECOND),
    HWA_SCALAR(HWA_MEASURE_PITCH_SETTLE_SECONDS, "pitch_settle_seconds",
               HWA_MEASURE_UNIT_SECONDS),
    HWA_SCALAR(HWA_MEASURE_OCTAVE_FAULT_FRACTION, "octave_fault_fraction",
               HWA_MEASURE_UNIT_RATIO),
    HWA_SCALAR(HWA_MEASURE_GLIDE_TIME_SECONDS, "glide_time_seconds",
               HWA_MEASURE_UNIT_SECONDS),
    HWA_SCALAR(HWA_MEASURE_PORTAMENTO_LINEARITY, "portamento_linearity",
               HWA_MEASURE_UNIT_RATIO),
    HWA_SCALAR(HWA_MEASURE_INHARMONICITY_B, "inharmonicity_b",
               HWA_MEASURE_UNIT_RATIO),
    HWA_SCALAR(HWA_MEASURE_ODD_EVEN_BALANCE_DB, "odd_even_balance_db",
               HWA_MEASURE_UNIT_DB),
    HWA_SCALAR(HWA_MEASURE_HARMONIC_CENTROID, "harmonic_centroid",
               HWA_MEASURE_UNIT_HARMONIC_INDEX),
    HWA_LEVEL(HWA_MEASURE_HARMONIC_LEVEL_DBFS, "harmonic_level_dbfs"),
    HWA_LEVEL(HWA_MEASURE_RESIDUAL_LEVEL_DBFS, "residual_level_dbfs"),
    HWA_SCALAR(HWA_MEASURE_HNR_DB, "hnr_db", HWA_MEASURE_UNIT_DB),
    HWA_PARTIAL(HWA_MEASURE_PARTIAL_FREQUENCY_ERROR_CENTS,
                "partial_frequency_error_cents", HWA_MEASURE_UNIT_CENTS, 0),
    HWA_PARTIAL(HWA_MEASURE_PARTIAL_LEVEL_DBFS, "partial_level_dbfs",
                HWA_MEASURE_UNIT_DBFS, 1),
    HWA_PARTIAL(HWA_MEASURE_PARTIAL_BALANCE_DB, "partial_balance_db",
                HWA_MEASURE_UNIT_DB, 0),
    HWA_PARTIAL(HWA_MEASURE_PARTIAL_PRESENCE_FRACTION,
                "partial_presence_fraction", HWA_MEASURE_UNIT_RATIO, 0),
    HWA_PARTIAL(HWA_MEASURE_PARTIAL_BIRTH_SECONDS, "partial_birth_seconds",
                HWA_MEASURE_UNIT_SECONDS, 0),
    HWA_PARTIAL(HWA_MEASURE_PARTIAL_LOSS_SECONDS, "partial_loss_seconds",
                HWA_MEASURE_UNIT_SECONDS, 0),
    HWA_PARTIAL(HWA_MEASURE_PARTIAL_LEVEL_SLOPE_DB_PER_SECOND,
                "partial_level_slope_db_per_second",
                HWA_MEASURE_UNIT_DB_PER_SECOND, 0),
    HWA_PARTIAL(HWA_MEASURE_PARTIAL_ONSET_ORDER, "partial_onset_order",
                HWA_MEASURE_UNIT_ORDER, 0),
    HWA_SCALAR(HWA_MEASURE_PARTIAL_ONSET_SPREAD_SECONDS,
               "partial_onset_spread_seconds", HWA_MEASURE_UNIT_SECONDS),
    HWA_BAND(HWA_MEASURE_RESIDUAL_BAND_LEVEL_DBFS,
             "residual_band_level_dbfs", HWA_MEASURE_UNIT_DBFS, 1),
    HWA_BAND(HWA_MEASURE_RESIDUAL_BAND_BALANCE_DB,
             "residual_band_balance_db", HWA_MEASURE_UNIT_DB, 0),
    HWA_SCALAR(HWA_MEASURE_HARMONIC_DECAY_DB_PER_SECOND,
               "harmonic_decay_db_per_second",
               HWA_MEASURE_UNIT_DB_PER_SECOND),
    HWA_SCALAR(HWA_MEASURE_VIBRATO_DELAY_SECONDS, "vibrato_delay_seconds",
               HWA_MEASURE_UNIT_SECONDS),
    HWA_SCALAR(HWA_MEASURE_VIBRATO_RATE_HZ, "vibrato_rate_hz",
               HWA_MEASURE_UNIT_HZ),
    HWA_SCALAR(HWA_MEASURE_VIBRATO_DEPTH_CENTS, "vibrato_depth_cents",
               HWA_MEASURE_UNIT_CENTS),
    HWA_SCALAR(HWA_MEASURE_VIBRATO_WAVEFORM_RESIDUAL_RATIO,
               "vibrato_waveform_residual_ratio", HWA_MEASURE_UNIT_RATIO),
    HWA_SCALAR(HWA_MEASURE_VIBRATO_RATE_DRIFT_HZ_PER_SECOND,
               "vibrato_rate_drift_hz_per_second",
               HWA_MEASURE_UNIT_HZ_PER_SECOND),
    HWA_SCALAR(HWA_MEASURE_VIBRATO_DEPTH_DRIFT_CENTS_PER_SECOND,
               "vibrato_depth_drift_cents_per_second",
               HWA_MEASURE_UNIT_CENTS_PER_SECOND),
    HWA_SCALAR(HWA_MEASURE_PITCH_LEVEL_CORRELATION,
               "pitch_level_correlation", HWA_MEASURE_UNIT_RATIO),
    HWA_SCALAR(HWA_MEASURE_PITCH_TONE_CORRELATION,
               "pitch_tone_correlation", HWA_MEASURE_UNIT_RATIO),
    HWA_SCALAR(HWA_MEASURE_ATTACK_DELAY_SECONDS, "attack_delay_seconds",
               HWA_MEASURE_UNIT_SECONDS),
    HWA_SCALAR(HWA_MEASURE_RISE_10_SECONDS, "rise_10_seconds",
               HWA_MEASURE_UNIT_SECONDS),
    HWA_SCALAR(HWA_MEASURE_RISE_50_SECONDS, "rise_50_seconds",
               HWA_MEASURE_UNIT_SECONDS),
    HWA_SCALAR(HWA_MEASURE_RISE_90_SECONDS, "rise_90_seconds",
               HWA_MEASURE_UNIT_SECONDS),
    HWA_SCALAR(HWA_MEASURE_ATTACK_SLOPE_DB_PER_SECOND,
               "attack_slope_db_per_second", HWA_MEASURE_UNIT_DB_PER_SECOND),
    HWA_SCALAR(HWA_MEASURE_ATTACK_OVERSHOOT_DB, "attack_overshoot_db",
               HWA_MEASURE_UNIT_DB),
    HWA_SCALAR(HWA_MEASURE_NOISE_BURST_SECONDS, "noise_burst_seconds",
               HWA_MEASURE_UNIT_SECONDS),
    HWA_LEVEL(HWA_MEASURE_PRE_NOTE_RESIDUAL_DBFS,
              "pre_note_residual_dbfs"),
    HWA_SCALAR(HWA_MEASURE_EARLY_DAMPING_DB_PER_SECOND,
               "early_damping_db_per_second",
               HWA_MEASURE_UNIT_DB_PER_SECOND),
    HWA_SCALAR(HWA_MEASURE_PITCH_FALL_CENTS, "pitch_fall_cents",
               HWA_MEASURE_UNIT_CENTS),
    HWA_SCALAR(HWA_MEASURE_DECAY_DB, "decay_db", HWA_MEASURE_UNIT_DB),
    HWA_LEVEL(HWA_MEASURE_RESIDUAL_EXCITATION_DBFS,
              "residual_excitation_dbfs"),
    HWA_SCALAR(HWA_MEASURE_GAP_OVERLAP_SECONDS, "gap_overlap_seconds",
               HWA_MEASURE_UNIT_SECONDS),
    HWA_SCALAR(HWA_MEASURE_CARRYOVER_DB, "carryover_db",
               HWA_MEASURE_UNIT_DB),
    HWA_SCALAR(HWA_MEASURE_TRANSITION_PITCH_CHANGE_CENTS,
               "transition_pitch_change_cents", HWA_MEASURE_UNIT_CENTS),
    HWA_SCALAR(HWA_MEASURE_TRANSITION_TONE_CHANGE_HZ,
               "transition_tone_change_hz", HWA_MEASURE_UNIT_HZ),
    HWA_SCALAR(HWA_MEASURE_REPEATED_ATTACK_SIMILARITY,
               "repeated_attack_similarity", HWA_MEASURE_UNIT_RATIO),
    HWA_SCALAR(HWA_MEASURE_REPEATED_PITCH_CURVE_SIMILARITY,
               "repeated_pitch_curve_similarity", HWA_MEASURE_UNIT_RATIO),
    HWA_SCALAR(HWA_MEASURE_LOCAL_CONTRAST_DB, "local_contrast_db",
               HWA_MEASURE_UNIT_DB),
    HWA_SCALAR(HWA_MEASURE_ACCENT_SIZE_DB, "accent_size_db",
               HWA_MEASURE_UNIT_DB),
    HWA_SCALAR(HWA_MEASURE_DURATION_SECONDS, "duration_seconds",
               HWA_MEASURE_UNIT_SECONDS)
};

#undef HWA_SCALAR
#undef HWA_LEVEL
#undef HWA_BAND
#undef HWA_PARTIAL

typedef struct HWANameValue {
    int value;
    const char *name;
} HWANameValue;

static const HWANameValue hwa_measure_units[] = {
    {HWA_MEASURE_UNIT_DBFS, "dBFS"},
    {HWA_MEASURE_UNIT_DB, "dB"},
    {HWA_MEASURE_UNIT_HZ, "Hz"},
    {HWA_MEASURE_UNIT_HZ_PER_SECOND, "Hz/s"},
    {HWA_MEASURE_UNIT_SECONDS, "seconds"},
    {HWA_MEASURE_UNIT_RATIO, "ratio"},
    {HWA_MEASURE_UNIT_CENTS, "cents"},
    {HWA_MEASURE_UNIT_CENTS_PER_SECOND, "cents/s"},
    {HWA_MEASURE_UNIT_DB_PER_SECOND, "dB/s"},
    {HWA_MEASURE_UNIT_HARMONIC_INDEX, "harmonic-index"},
    {HWA_MEASURE_UNIT_ORDER, "order"}
};

static const HWANameValue hwa_measure_views[] = {
    {HWA_MEASURE_VIEW_RAW, "raw"},
    {HWA_MEASURE_VIEW_LEVEL_RELATIVE, "level-relative"},
    {HWA_MEASURE_VIEW_PRODUCTION_CORRECTED, "production-corrected"}
};

static const HWANameValue hwa_measure_statuses[] = {
    {HWA_MEASURE_STATUS_VALID, "valid"},
    {HWA_MEASURE_STATUS_NO_DATA, "no-data"},
    {HWA_MEASURE_STATUS_UNSUPPORTED_ITEM, "unsupported-item"},
    {HWA_MEASURE_STATUS_EMPTY_SPAN, "empty-span"},
    {HWA_MEASURE_STATUS_TOO_SHORT, "too-short"},
    {HWA_MEASURE_STATUS_NO_SIGNAL, "no-signal"},
    {HWA_MEASURE_STATUS_BELOW_FLOOR, "below-floor"},
    {HWA_MEASURE_STATUS_NO_PITCH, "no-pitch"},
    {HWA_MEASURE_STATUS_MULTI_PITCH, "multi-pitch"},
    {HWA_MEASURE_STATUS_NO_REFERENCE, "no-reference"}
};

static const HWANameValue hwa_measure_selectors[] = {
    {HWA_MEASURE_GROUP_ALL, "all"},
    {HWA_MEASURE_GROUP_PITCH, "pitch"},
    {HWA_MEASURE_GROUP_REGISTER, "register"},
    {HWA_MEASURE_GROUP_DYNAMIC, "dynamic"},
    {HWA_MEASURE_GROUP_ARTICULATION, "articulation"},
    {HWA_MEASURE_GROUP_PART, "part"},
    {HWA_MEASURE_GROUP_PHYSICAL_ELEMENT, "physical_element"},
    {HWA_MEASURE_GROUP_CONTROLLER, "controller"},
    {HWA_MEASURE_GROUP_TECHNIQUE, "technique"},
    {HWA_MEASURE_GROUP_SCORE_SECTION, "score_section"},
    {HWA_MEASURE_GROUP_TRANSITION, "transition"},
    {HWA_MEASURE_GROUP_GESTURE, "gesture"}
};

static const HWANameValue hwa_measure_item_kinds[] = {
    {HWA_ITEM_NOTE, "note"},
    {HWA_ITEM_ATTACK, "attack"},
    {HWA_ITEM_BODY, "body"},
    {HWA_ITEM_RELEASE, "release"},
    {HWA_ITEM_RESIDUAL_TAIL, "residual-tail"},
    {HWA_ITEM_REST, "rest"},
    {HWA_ITEM_TRANSITION, "transition"},
    {HWA_ITEM_GESTURE, "gesture"},
    {HWA_ITEM_MULTI_NOTE, "multi-note"}
};

typedef struct HWAGroupCandidate {
    uint64_t item_id;
    HWAItemKind item_kind;
    const char *item_role;
    HWAMeasureGroupSelector selector;
    const char *value;
} HWAGroupCandidate;

typedef struct HWAMeasureSignature {
    HWAMeasureKind kind;
    uint32_t index;
    HWAMeasureUnit unit;
    HWAMeasureView view;
} HWAMeasureSignature;

static int hwa_measure_work_add(uint64_t *live,
                                uint64_t limit,
                                uint64_t bytes)
{
    if (live == NULL || *live > limit || bytes > limit - *live) return -1;
    *live += bytes;
    return 0;
}

static void hwa_measure_work_release(uint64_t *live, uint64_t bytes)
{
    if (live != NULL && bytes <= *live) *live -= bytes;
}

static const HWAMeasureCatalogEntry *hwa_measure_catalog_find(
    HWAMeasureKind kind)
{
    size_t index;
    for (index = 0U;
         index < sizeof(hwa_measure_catalog) / sizeof(hwa_measure_catalog[0]);
         ++index) {
        if (hwa_measure_catalog[index].kind == kind) {
            return &hwa_measure_catalog[index];
        }
    }
    return NULL;
}

static const char *hwa_name_for_value(const HWANameValue *values,
                                      size_t count,
                                      int value)
{
    size_t index;
    for (index = 0U; index < count; ++index) {
        if (values[index].value == value) return values[index].name;
    }
    return NULL;
}

static int hwa_value_for_name(const HWANameValue *values,
                              size_t count,
                              const char *name,
                              int *value)
{
    size_t index;
    if (name == NULL || value == NULL) return -1;
    for (index = 0U; index < count; ++index) {
        if (strcmp(values[index].name, name) == 0) {
            *value = values[index].value;
            return 0;
        }
    }
    return -1;
}

const char *hwa_measure_kind_name(HWAMeasureKind kind)
{
    const HWAMeasureCatalogEntry *entry = hwa_measure_catalog_find(kind);
    return entry != NULL ? entry->name : NULL;
}

const char *hwa_measure_unit_name(HWAMeasureUnit unit)
{
    return hwa_name_for_value(hwa_measure_units,
                              sizeof(hwa_measure_units) /
                                  sizeof(hwa_measure_units[0]),
                              (int)unit);
}

const char *hwa_measure_view_name(HWAMeasureView view)
{
    return hwa_name_for_value(hwa_measure_views,
                              sizeof(hwa_measure_views) /
                                  sizeof(hwa_measure_views[0]),
                              (int)view);
}

const char *hwa_measure_status_name(HWAMeasureStatus status)
{
    return hwa_name_for_value(hwa_measure_statuses,
                              sizeof(hwa_measure_statuses) /
                                  sizeof(hwa_measure_statuses[0]),
                              (int)status);
}

const char *hwa_measure_group_selector_name(HWAMeasureGroupSelector selector)
{
    return hwa_name_for_value(hwa_measure_selectors,
                              sizeof(hwa_measure_selectors) /
                                  sizeof(hwa_measure_selectors[0]),
                              (int)selector);
}

const char *hwa_measure_item_kind_name(HWAItemKind kind)
{
    return hwa_name_for_value(hwa_measure_item_kinds,
                              sizeof(hwa_measure_item_kinds) /
                                  sizeof(hwa_measure_item_kinds[0]),
                              (int)kind);
}

int hwa_measure_kind_from_name(const char *name, HWAMeasureKind *kind)
{
    size_t index;
    if (name == NULL || kind == NULL) return -1;
    for (index = 0U;
         index < sizeof(hwa_measure_catalog) / sizeof(hwa_measure_catalog[0]);
         ++index) {
        if (strcmp(name, hwa_measure_catalog[index].name) == 0) {
            *kind = hwa_measure_catalog[index].kind;
            return 0;
        }
    }
    return -1;
}

#define HWA_DEFINE_NAME_PARSER(function_, array_, type_)                    \
    int function_(const char *name, type_ *value)                          \
    {                                                                       \
        int parsed;                                                         \
        if (hwa_value_for_name(array_,                                      \
                               sizeof(array_) / sizeof((array_)[0]),        \
                               name, &parsed) != 0) {                        \
            return -1;                                                      \
        }                                                                   \
        *value = (type_)parsed;                                              \
        return 0;                                                           \
    }

HWA_DEFINE_NAME_PARSER(hwa_measure_unit_from_name,
                       hwa_measure_units, HWAMeasureUnit)
HWA_DEFINE_NAME_PARSER(hwa_measure_view_from_name,
                       hwa_measure_views, HWAMeasureView)
HWA_DEFINE_NAME_PARSER(hwa_measure_status_from_name,
                       hwa_measure_statuses, HWAMeasureStatus)
HWA_DEFINE_NAME_PARSER(hwa_measure_group_selector_from_name,
                       hwa_measure_selectors, HWAMeasureGroupSelector)
HWA_DEFINE_NAME_PARSER(hwa_measure_item_kind_from_name,
                       hwa_measure_item_kinds, HWAItemKind)

#undef HWA_DEFINE_NAME_PARSER

int hwa_measure_kind_index_valid(HWAMeasureKind kind,
                                 uint32_t index,
                                 size_t max_partials)
{
    const HWAMeasureCatalogEntry *entry = hwa_measure_catalog_find(kind);
    if (entry == NULL) return 0;
    if (entry->index_rule == HWA_INDEX_SCALAR) return index == 0U;
    if (entry->index_rule == HWA_INDEX_BAND) return index < HWA_BAND_COUNT;
    return index != 0U && (uint64_t)index <= (uint64_t)max_partials;
}

int hwa_measure_kind_unit(HWAMeasureKind kind,
                          HWAMeasureView view,
                          HWAMeasureUnit *unit)
{
    const HWAMeasureCatalogEntry *entry = hwa_measure_catalog_find(kind);
    if (entry == NULL || unit == NULL ||
        (view != HWA_MEASURE_VIEW_RAW &&
         view != HWA_MEASURE_VIEW_LEVEL_RELATIVE) ||
        (view == HWA_MEASURE_VIEW_LEVEL_RELATIVE && !entry->level_like)) {
        return -1;
    }
    *unit = entry->level_like && view == HWA_MEASURE_VIEW_LEVEL_RELATIVE
                ? HWA_MEASURE_UNIT_DB
                : entry->unit;
    return 0;
}

static char *hwa_measure_strdup(const char *text)
{
    size_t size;
    char *copy;
    if (text == NULL) text = "";
    size = strlen(text) + 1U;
    copy = (char *)malloc(size);
    if (copy != NULL) memcpy(copy, text, size);
    return copy;
}

static const char *hwa_measure_context_value(
    const HWAMeasureItemContext *context,
    HWAMeasureGroupSelector selector)
{
    switch (selector) {
    case HWA_MEASURE_GROUP_PITCH: return context->labels.pitch;
    case HWA_MEASURE_GROUP_REGISTER: return context->labels.register_name;
    case HWA_MEASURE_GROUP_DYNAMIC: return context->labels.dynamic;
    case HWA_MEASURE_GROUP_ARTICULATION:
        return context->labels.articulation;
    case HWA_MEASURE_GROUP_PART: return context->labels.part;
    case HWA_MEASURE_GROUP_PHYSICAL_ELEMENT:
        return context->labels.physical_element;
    case HWA_MEASURE_GROUP_CONTROLLER: return context->labels.controller;
    case HWA_MEASURE_GROUP_TECHNIQUE: return context->labels.technique;
    case HWA_MEASURE_GROUP_SCORE_SECTION:
        return context->labels.score_section;
    case HWA_MEASURE_GROUP_TRANSITION: return context->labels.transition;
    case HWA_MEASURE_GROUP_GESTURE: return context->labels.gesture;
    default: return "";
    }
}

static int hwa_measure_group_candidate_compare(const void *left,
                                               const void *right)
{
    const HWAGroupCandidate *a = (const HWAGroupCandidate *)left;
    const HWAGroupCandidate *b = (const HWAGroupCandidate *)right;
    int order;
    if (a->item_kind != b->item_kind) return a->item_kind < b->item_kind ? -1 : 1;
    order = strcmp(a->item_role, b->item_role);
    if (order != 0) return order;
    if (a->selector != b->selector) return a->selector < b->selector ? -1 : 1;
    order = strcmp(a->value, b->value);
    if (order != 0) return order;
    return a->item_id < b->item_id ? -1 : a->item_id > b->item_id ? 1 : 0;
}

static int hwa_measure_group_descriptor_equal(const HWAGroupCandidate *left,
                                              const HWAGroupCandidate *right)
{
    return left->item_kind == right->item_kind &&
           left->selector == right->selector &&
           strcmp(left->item_role, right->item_role) == 0 &&
           strcmp(left->value, right->value) == 0;
}

static size_t hwa_measure_hex_size(const char *text)
{
    size_t size = strlen(text);
    return size > (SIZE_MAX - 1U) / 2U ? SIZE_MAX : size * 2U;
}

static size_t hwa_measure_group_key_size(const HWAGroupCandidate *candidate)
{
    const char *kind = hwa_measure_item_kind_name(candidate->item_kind);
    const char *selector = hwa_measure_group_selector_name(candidate->selector);
    size_t kind_size;
    size_t selector_size;
    size_t role_hex_size;
    size_t value_hex_size;
    if (kind == NULL || selector == NULL) return SIZE_MAX;
    kind_size = strlen(kind);
    selector_size = strlen(selector);
    role_hex_size = hwa_measure_hex_size(candidate->item_role);
    value_hex_size = hwa_measure_hex_size(candidate->value);
    if (role_hex_size == SIZE_MAX || value_hex_size == SIZE_MAX ||
        kind_size > SIZE_MAX - selector_size - 6U ||
        role_hex_size > SIZE_MAX - kind_size - selector_size - 6U ||
        value_hex_size > SIZE_MAX - kind_size - selector_size -
                             role_hex_size - 6U) {
        return SIZE_MAX;
    }
    return kind_size + selector_size + role_hex_size + value_hex_size + 6U;
}

static char *hwa_measure_group_key(const HWAGroupCandidate *candidate)
{
    static const char digits[] = "0123456789abcdef";
    const char *kind = hwa_measure_item_kind_name(candidate->item_kind);
    const char *selector = hwa_measure_group_selector_name(candidate->selector);
    size_t total;
    char *key;
    char *cursor;
    const unsigned char *source;

    if (kind == NULL || selector == NULL) return NULL;
    total = hwa_measure_group_key_size(candidate);
    if (total == SIZE_MAX) return NULL;
    key = (char *)malloc(total);
    if (key == NULL) return NULL;
    cursor = key;
    *cursor++ = 'g';
    *cursor++ = '/';
    memcpy(cursor, kind, strlen(kind)); cursor += strlen(kind); *cursor++ = '/';
    source = (const unsigned char *)candidate->item_role;
    while (*source != 0U) {
        *cursor++ = digits[*source >> 4U];
        *cursor++ = digits[*source & 0x0fU];
        source++;
    }
    *cursor++ = '/';
    memcpy(cursor, selector, strlen(selector)); cursor += strlen(selector);
    *cursor++ = '/';
    source = (const unsigned char *)candidate->value;
    while (*source != 0U) {
        *cursor++ = digits[*source >> 4U];
        *cursor++ = digits[*source & 0x0fU];
        source++;
    }
    *cursor = '\0';
    return key;
}

static void hwa_measure_profile_arrays_free(HWAMeasurementSet *set)
{
    size_t index;
    if (set == NULL) return;
    for (index = 0U; index < set->group_count; ++index) {
        free(set->groups[index].key);
        free(set->groups[index].item_role);
        free(set->groups[index].value);
    }
    free(set->statistics);
    free(set->group_members);
    free(set->groups);
    set->statistics = NULL;
    set->group_members = NULL;
    set->groups = NULL;
    set->statistic_count = 0U;
    set->group_member_count = 0U;
    set->group_count = 0U;
}

static int hwa_measure_signature_compare(const void *left, const void *right)
{
    const HWAMeasureSignature *a = (const HWAMeasureSignature *)left;
    const HWAMeasureSignature *b = (const HWAMeasureSignature *)right;
    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
    if (a->index != b->index) return a->index < b->index ? -1 : 1;
    if (a->view != b->view) return a->view < b->view ? -1 : 1;
    return a->unit < b->unit ? -1 : a->unit > b->unit ? 1 : 0;
}

static int hwa_measure_observation_signature_compare(
    const HWAMeasureObservation *observation,
    const HWAMeasureSignature *signature)
{
    if (observation->kind != signature->kind) {
        return observation->kind < signature->kind ? -1 : 1;
    }
    if (observation->index != signature->index) {
        return observation->index < signature->index ? -1 : 1;
    }
    if (observation->view != signature->view) {
        return observation->view < signature->view ? -1 : 1;
    }
    return observation->unit < signature->unit ? -1 :
           observation->unit > signature->unit ? 1 : 0;
}

static const HWAMeasureObservation *hwa_measure_find_observation(
    const HWAMeasurementSet *set,
    const size_t *offsets,
    uint64_t item_id,
    const HWAMeasureSignature *signature)
{
    size_t low = offsets[item_id - 1U];
    size_t high = offsets[item_id];
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        int order = hwa_measure_observation_signature_compare(
            &set->measurements[middle], signature);
        if (order < 0) low = middle + 1U;
        else high = middle;
    }
    if (low < offsets[item_id] &&
        hwa_measure_observation_signature_compare(
            &set->measurements[low], signature) == 0) {
        return &set->measurements[low];
    }
    return NULL;
}

static int hwa_measure_double_compare(const void *left, const void *right)
{
    const double a = *(const double *)left;
    const double b = *(const double *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static double hwa_measure_quantile(const double *values,
                                   size_t count,
                                   double probability)
{
    double position;
    size_t lower;
    size_t upper;
    double fraction;
    if (count == 1U) return values[0];
    position = (double)(count - 1U) * probability;
    lower = (size_t)floor(position);
    upper = (size_t)ceil(position);
    fraction = position - (double)lower;
    return values[lower] + (values[upper] - values[lower]) * fraction;
}

static void hwa_measure_calculate_statistics(double *values,
                                             const double *confidences,
                                             size_t valid_count,
                                             size_t total_count,
                                             HWAMeasureStatistics *statistics)
{
    size_t index;
    long double sum = 0.0L;
    long double confidence_sum = 0.0L;
    long double squared_sum = 0.0L;
    double mean;

    memset(statistics, 0, sizeof(*statistics));
    statistics->total_count = total_count;
    statistics->valid_count = valid_count;
    statistics->missing_count = total_count - valid_count;
    if (valid_count == 0U) return;
    qsort(values, valid_count, sizeof(*values), hwa_measure_double_compare);
    for (index = 0U; index < valid_count; ++index) {
        sum += (long double)values[index];
        confidence_sum += (long double)confidences[index];
    }
    mean = (double)(sum / (long double)valid_count);
    for (index = 0U; index < valid_count; ++index) {
        long double difference = (long double)values[index] -
                                 (long double)mean;
        squared_sum += difference * difference;
    }
    statistics->minimum = values[0];
    statistics->q05 = hwa_measure_quantile(values, valid_count, 0.05);
    statistics->q25 = hwa_measure_quantile(values, valid_count, 0.25);
    statistics->q50 = hwa_measure_quantile(values, valid_count, 0.50);
    statistics->q75 = hwa_measure_quantile(values, valid_count, 0.75);
    statistics->q95 = hwa_measure_quantile(values, valid_count, 0.95);
    statistics->maximum = values[valid_count - 1U];
    statistics->mean = mean == 0.0 ? 0.0 : mean;
    statistics->population_sd =
        sqrt((double)(squared_sum / (long double)valid_count));
    statistics->confidence = total_count == 0U
                                 ? 0.0
                                 : (double)(confidence_sum /
                                            (long double)total_count);
    statistics->valid = 1;
}

static int hwa_measure_validate_source(const HWAMeasurementSet *set,
                                       char *error,
                                       size_t error_size)
{
    size_t index;
    uint64_t previous_item = 0U;
    HWAMeasureSignature previous_signature = {0};
    int previous_valid = 0;

    if (set->context_count > set->options.max_items ||
        set->measurement_count > set->options.max_measurements ||
        (set->context_count != 0U && set->contexts == NULL) ||
        (set->measurement_count != 0U && set->measurements == NULL) ||
        set->groups != NULL || set->group_count != 0U ||
        set->group_members != NULL || set->group_member_count != 0U ||
        set->statistics != NULL || set->statistic_count != 0U) {
        hwa_set_error(error, error_size,
                      "invalid measurement profile build arguments");
        return -1;
    }
    for (index = 0U; index < set->context_count; ++index) {
        const HWAMeasureItemContext *context = &set->contexts[index];
        if (context->item_id != (uint64_t)index + 1U ||
            context->item_key == NULL || context->item_key[0] == '\0' ||
            context->item_role == NULL || context->item_role[0] == '\0' ||
            hwa_measure_item_kind_name(context->item_kind) == NULL ||
            context->start_sample > context->end_sample ||
            context->end_sample > set->audio_format.frames ||
            !isfinite(context->item_confidence) ||
            context->item_confidence < 0.0 || context->item_confidence > 1.0) {
            hwa_set_error(error, error_size,
                          "invalid measurement item context %zu", index);
            return -1;
        }
    }
    for (index = 0U; index < set->measurement_count; ++index) {
        const HWAMeasureObservation *observation = &set->measurements[index];
        HWAMeasureUnit expected_unit;
        HWAMeasureSignature signature;
        if (observation->id != (uint64_t)index + 1U ||
            observation->item_id == 0U ||
            observation->item_id > (uint64_t)set->context_count ||
            set->contexts[observation->item_id - 1U].excluded ||
            !hwa_measure_kind_index_valid(observation->kind,
                                          observation->index,
                                          set->options.max_partials) ||
            hwa_measure_kind_unit(observation->kind, observation->view,
                                  &expected_unit) != 0 ||
            observation->unit != expected_unit ||
            hwa_measure_status_name(observation->status) == NULL ||
            !isfinite(observation->value) ||
            !isfinite(observation->confidence) ||
            observation->confidence < 0.0 || observation->confidence > 1.0 ||
            (observation->status != HWA_MEASURE_STATUS_VALID &&
             observation->value != 0.0) ||
            (observation->evidence_flags & ~HWA_MEASURE_EVIDENCE_ALL) != 0U ||
            (observation->quality_flags & ~HWA_MEASURE_QUALITY_ALL) != 0U) {
            hwa_set_error(error, error_size,
                          "invalid scalar measurement %zu", index);
            return -1;
        }
        signature.kind = observation->kind;
        signature.index = observation->index;
        signature.unit = observation->unit;
        signature.view = observation->view;
        if (previous_valid && observation->item_id == previous_item &&
            hwa_measure_signature_compare(&previous_signature, &signature) >= 0) {
            hwa_set_error(error, error_size,
                          "scalar measurements are not in canonical order");
            return -1;
        }
        if (observation->item_id < previous_item) {
            hwa_set_error(error, error_size,
                          "scalar measurement item order decreases");
            return -1;
        }
        previous_item = observation->item_id;
        previous_signature = signature;
        previous_valid = 1;
    }
    return 0;
}

int hwa_measure_build_profile(HWAMeasurementSet *set,
                              char *error,
                              size_t error_size)
{
    HWAGroupCandidate *candidates = NULL;
    size_t candidate_count = 0U;
    size_t candidate_capacity;
    size_t unique_group_count = 0U;
    size_t *offsets = NULL;
    unsigned char *seen = NULL;
    size_t seen_slots;
    size_t index_stride;
    size_t maximum_group_members = 0U;
    double *values = NULL;
    double *confidences = NULL;
    size_t statistic_count = 0U;
    uint64_t retained_add = 0U;
    uint64_t work_live;
    uint64_t candidate_bytes = 0U;
    uint64_t group_array_bytes = 0U;
    uint64_t member_array_bytes = 0U;
    uint64_t group_string_bytes = 0U;
    uint64_t offset_bytes = 0U;
    uint64_t seen_bytes = 0U;
    uint64_t value_bytes = 0U;
    uint64_t statistic_bytes = 0U;
    size_t index;
    int result = -1;

    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (set == NULL || hwa_measure_validate_source(set, error, error_size) != 0) {
        return -1;
    }
    work_live = set->retained_work_bytes;
    if (work_live > set->options.max_work_bytes) {
        hwa_set_error(error, error_size,
                      "measurement source exceeds the work-byte limit");
        return -1;
    }
    if (set->context_count > SIZE_MAX / HWA_MEASURE_GROUP_SELECTOR_COUNT) {
        hwa_set_error(error, error_size, "measurement group count overflows");
        return -1;
    }
    candidate_capacity = set->context_count *
                         (HWA_MEASURE_GROUP_SELECTOR_COUNT - 1U);
    if (candidate_capacity > set->options.max_group_members ||
        (candidate_capacity != 0U &&
         candidate_capacity > SIZE_MAX / sizeof(*candidates))) {
        hwa_set_error(error, error_size,
                      "measurement group-member limit exceeded");
        return -1;
    }
    candidate_bytes = (uint64_t)candidate_capacity * sizeof(*candidates);
    if (hwa_measure_work_add(&work_live, set->options.max_work_bytes,
                             candidate_bytes) != 0) {
        hwa_set_error(error, error_size,
                      "measurement group scratch exceeds the work-byte limit");
        return -1;
    }
    if (candidate_capacity != 0U) {
        candidates = (HWAGroupCandidate *)malloc(candidate_capacity *
                                                  sizeof(*candidates));
        if (candidates == NULL) {
            hwa_set_error(error, error_size,
                          "out of memory for measurement role groups");
            return -1;
        }
    }
    for (index = 0U; index < set->context_count; ++index) {
        const HWAMeasureItemContext *context = &set->contexts[index];
        HWAMeasureGroupSelector selector;
        if (context->excluded) continue;
        candidates[candidate_count].item_id = context->item_id;
        candidates[candidate_count].item_kind = context->item_kind;
        candidates[candidate_count].item_role = context->item_role;
        candidates[candidate_count].selector = HWA_MEASURE_GROUP_ALL;
        candidates[candidate_count].value = "";
        candidate_count++;
        for (selector = HWA_MEASURE_GROUP_PITCH;
             selector < HWA_MEASURE_GROUP_SELECTOR_COUNT;
             selector = (HWAMeasureGroupSelector)((int)selector + 1)) {
            const char *value = hwa_measure_context_value(context, selector);
            if (value != NULL && value[0] != '\0') {
                candidates[candidate_count].item_id = context->item_id;
                candidates[candidate_count].item_kind = context->item_kind;
                candidates[candidate_count].item_role = context->item_role;
                candidates[candidate_count].selector = selector;
                candidates[candidate_count].value = value;
                candidate_count++;
            }
        }
    }
    qsort(candidates, candidate_count, sizeof(*candidates),
          hwa_measure_group_candidate_compare);
    for (index = 0U; index < candidate_count; ++index) {
        if (index == 0U ||
            !hwa_measure_group_descriptor_equal(&candidates[index - 1U],
                                                &candidates[index])) {
            unique_group_count++;
        } else if (candidates[index - 1U].item_id == candidates[index].item_id) {
            hwa_set_error(error, error_size,
                          "duplicate measurement group membership");
            goto cleanup;
        }
    }
    if (unique_group_count > set->options.max_groups ||
        unique_group_count > SIZE_MAX / sizeof(*set->groups) ||
        candidate_count > SIZE_MAX / sizeof(*set->group_members)) {
        hwa_set_error(error, error_size, "measurement group limit exceeded");
        goto cleanup;
    }
    group_array_bytes = (uint64_t)unique_group_count * sizeof(*set->groups);
    member_array_bytes = (uint64_t)candidate_count *
                         sizeof(*set->group_members);
    for (index = 0U; index < candidate_count; ++index) {
        if (index == 0U ||
            !hwa_measure_group_descriptor_equal(&candidates[index - 1U],
                                                &candidates[index])) {
            size_t key_size = hwa_measure_group_key_size(&candidates[index]);
            uint64_t string_bytes;
            if (key_size == SIZE_MAX
#if SIZE_MAX >= UINT64_MAX
                || strlen(candidates[index].item_role) >
                    (size_t)(UINT64_MAX - 1U)
                || strlen(candidates[index].value) >
                    (size_t)(UINT64_MAX - 1U)
#endif
            ) {
                hwa_set_error(error, error_size,
                              "measurement group string size overflows");
                goto cleanup;
            }
            string_bytes = (uint64_t)strlen(candidates[index].item_role) + 1U;
            if ((uint64_t)strlen(candidates[index].value) + 1U >
                    UINT64_MAX - string_bytes ||
                (uint64_t)key_size >
                    UINT64_MAX - string_bytes -
                        ((uint64_t)strlen(candidates[index].value) + 1U)) {
                hwa_set_error(error, error_size,
                              "measurement group string total overflows");
                goto cleanup;
            }
            string_bytes += (uint64_t)strlen(candidates[index].value) + 1U;
            string_bytes += (uint64_t)key_size;
            if (string_bytes > UINT64_MAX - group_string_bytes) {
                hwa_set_error(error, error_size,
                              "measurement group storage overflows");
                goto cleanup;
            }
            group_string_bytes += string_bytes;
        }
    }
    if (group_array_bytes > UINT64_MAX - member_array_bytes ||
        group_string_bytes > UINT64_MAX - group_array_bytes -
                                 member_array_bytes ||
        hwa_measure_work_add(&work_live, set->options.max_work_bytes,
                             group_array_bytes + member_array_bytes +
                                 group_string_bytes) != 0) {
        hwa_set_error(error, error_size,
                      "measurement groups exceed the work-byte limit");
        goto cleanup;
    }
    set->groups = unique_group_count == 0U ? NULL :
        (HWAMeasureGroup *)calloc(unique_group_count, sizeof(*set->groups));
    set->group_members = candidate_count == 0U ? NULL :
        (HWAMeasureGroupMember *)calloc(candidate_count,
                                        sizeof(*set->group_members));
    if ((unique_group_count != 0U && set->groups == NULL) ||
        (candidate_count != 0U && set->group_members == NULL)) {
        hwa_set_error(error, error_size,
                      "out of memory for retained measurement groups");
        goto cleanup;
    }
    set->group_count = unique_group_count;
    set->group_member_count = candidate_count;
    unique_group_count = 0U;
    for (index = 0U; index < candidate_count; ++index) {
        HWAMeasureGroup *group;
        if (index == 0U ||
            !hwa_measure_group_descriptor_equal(&candidates[index - 1U],
                                                &candidates[index])) {
            group = &set->groups[unique_group_count];
            group->id = (uint64_t)unique_group_count + 1U;
            group->item_kind = candidates[index].item_kind;
            group->selector = candidates[index].selector;
            group->item_role = hwa_measure_strdup(candidates[index].item_role);
            group->value = hwa_measure_strdup(candidates[index].value);
            group->key = hwa_measure_group_key(&candidates[index]);
            if (group->item_role == NULL || group->value == NULL ||
                group->key == NULL) {
                hwa_set_error(error, error_size,
                              "out of memory for measurement group strings");
                goto cleanup;
            }
            unique_group_count++;
        }
        group = &set->groups[unique_group_count - 1U];
        set->group_members[index].group_id = group->id;
        set->group_members[index].item_id = candidates[index].item_id;
        group->member_count++;
        if (group->member_count > maximum_group_members) {
            maximum_group_members = group->member_count;
        }
    }
    retained_add = group_array_bytes + member_array_bytes + group_string_bytes;
    free(candidates);
    candidates = NULL;
    hwa_measure_work_release(&work_live, candidate_bytes);
    candidate_bytes = 0U;

    if (set->context_count == SIZE_MAX ||
        set->context_count + 1U > SIZE_MAX / sizeof(*offsets)) {
        hwa_set_error(error, error_size, "measurement item index overflows");
        goto cleanup;
    }
    offset_bytes = (uint64_t)(set->context_count + 1U) * sizeof(*offsets);
    if (hwa_measure_work_add(&work_live, set->options.max_work_bytes,
                             offset_bytes) != 0) {
        hwa_set_error(error, error_size,
                      "measurement item index exceeds the work-byte limit");
        goto cleanup;
    }
    offsets = (size_t *)malloc((set->context_count + 1U) * sizeof(*offsets));
    if (offsets == NULL && set->context_count != SIZE_MAX) {
        hwa_set_error(error, error_size,
                      "out of memory for measurement item index");
        goto cleanup;
    }
    {
        size_t measurement = 0U;
        size_t item;
        for (item = 0U; item < set->context_count; ++item) {
            offsets[item] = measurement;
            while (measurement < set->measurement_count &&
                   set->measurements[measurement].item_id ==
                       (uint64_t)item + 1U) {
                measurement++;
            }
        }
        offsets[set->context_count] = measurement;
        if (measurement != set->measurement_count) {
            hwa_set_error(error, error_size,
                          "measurement item index is incomplete");
            goto cleanup;
        }
    }
    index_stride = set->options.max_partials > HWA_BAND_COUNT
                       ? set->options.max_partials + 1U
                       : HWA_BAND_COUNT + 1U;
    if (index_stride == 0U ||
        (size_t)HWA_MEASURE_KIND_COUNT > SIZE_MAX / index_stride ||
        (size_t)HWA_MEASURE_KIND_COUNT * index_stride >
            SIZE_MAX / (size_t)HWA_MEASURE_VIEW_COUNT) {
        hwa_set_error(error, error_size,
                      "measurement statistic signature space overflows");
        goto cleanup;
    }
    seen_slots = (size_t)HWA_MEASURE_KIND_COUNT * index_stride *
                 (size_t)HWA_MEASURE_VIEW_COUNT;
    seen_bytes = (uint64_t)seen_slots;
    value_bytes = (uint64_t)maximum_group_members * sizeof(*values);
    if (value_bytes > UINT64_MAX / 2U ||
        seen_bytes > UINT64_MAX - value_bytes * 2U ||
        hwa_measure_work_add(&work_live, set->options.max_work_bytes,
                             seen_bytes + value_bytes * 2U) != 0) {
        hwa_set_error(error, error_size,
                      "measurement distribution scratch exceeds the work-byte limit");
        goto cleanup;
    }
    seen = (unsigned char *)calloc(seen_slots, 1U);
    values = maximum_group_members == 0U ? NULL :
        (double *)malloc(maximum_group_members * sizeof(*values));
    confidences = maximum_group_members == 0U ? NULL :
        (double *)malloc(maximum_group_members * sizeof(*confidences));
    if (seen == NULL || (maximum_group_members != 0U &&
                         (values == NULL || confidences == NULL))) {
        hwa_set_error(error, error_size,
                      "out of memory for measurement distributions");
        goto cleanup;
    }
    {
        size_t member_offset = 0U;
        size_t group_index;
        for (group_index = 0U; group_index < set->group_count; ++group_index) {
            size_t member_end = member_offset +
                                set->groups[group_index].member_count;
            size_t member;
            memset(seen, 0, seen_slots);
            for (member = member_offset; member < member_end; ++member) {
                uint64_t item_id = set->group_members[member].item_id;
                size_t measurement;
                for (measurement = offsets[item_id - 1U];
                     measurement < offsets[item_id]; ++measurement) {
                    const HWAMeasureObservation *observation =
                        &set->measurements[measurement];
                    size_t slot = ((size_t)observation->kind * index_stride +
                                   (size_t)observation->index) *
                                      (size_t)HWA_MEASURE_VIEW_COUNT +
                                  (size_t)observation->view;
                    if (slot >= seen_slots) {
                        hwa_set_error(error, error_size,
                                      "measurement signature exceeds bounds");
                        goto cleanup;
                    }
                    seen[slot] = 1U;
                }
            }
            {
                size_t slot;
                for (slot = 0U; slot < seen_slots; ++slot) {
                    if (seen[slot]) statistic_count++;
                }
            }
            member_offset = member_end;
        }
    }
    if (statistic_count > set->options.max_statistics ||
        statistic_count > SIZE_MAX / sizeof(*set->statistics)) {
        hwa_set_error(error, error_size,
                      "measurement statistic limit exceeded");
        goto cleanup;
    }
    statistic_bytes = (uint64_t)statistic_count * sizeof(*set->statistics);
    if (hwa_measure_work_add(&work_live, set->options.max_work_bytes,
                             statistic_bytes) != 0) {
        hwa_set_error(error, error_size,
                      "measurement statistics exceed the work-byte limit");
        goto cleanup;
    }
    set->statistics = statistic_count == 0U ? NULL :
        (HWAMeasureStatistic *)calloc(statistic_count,
                                      sizeof(*set->statistics));
    if (statistic_count != 0U && set->statistics == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for measurement statistics");
        goto cleanup;
    }
    set->statistic_count = statistic_count;
    retained_add += statistic_bytes;
    statistic_count = 0U;
    {
        size_t member_offset = 0U;
        size_t group_index;
        for (group_index = 0U; group_index < set->group_count; ++group_index) {
            size_t member_end = member_offset +
                                set->groups[group_index].member_count;
            size_t member;
            memset(seen, 0, seen_slots);
            for (member = member_offset; member < member_end; ++member) {
                uint64_t item_id = set->group_members[member].item_id;
                size_t measurement;
                for (measurement = offsets[item_id - 1U];
                     measurement < offsets[item_id]; ++measurement) {
                    const HWAMeasureObservation *observation =
                        &set->measurements[measurement];
                    size_t slot = ((size_t)observation->kind * index_stride +
                                   (size_t)observation->index) *
                                      (size_t)HWA_MEASURE_VIEW_COUNT +
                                  (size_t)observation->view;
                    seen[slot] = 1U;
                }
            }
            {
                size_t slot;
                for (slot = 0U; slot < seen_slots; ++slot) {
                    HWAMeasureStatistic *statistic;
                    HWAMeasureSignature signature;
                    size_t valid_count = 0U;
                    uint32_t quality_flags = 0U;
                    if (!seen[slot]) continue;
                    signature.view = (HWAMeasureView)(slot %
                        (size_t)HWA_MEASURE_VIEW_COUNT);
                    signature.index = (uint32_t)((slot /
                        (size_t)HWA_MEASURE_VIEW_COUNT) % index_stride);
                    signature.kind = (HWAMeasureKind)((slot /
                        (size_t)HWA_MEASURE_VIEW_COUNT) / index_stride);
                    if (hwa_measure_kind_unit(signature.kind, signature.view,
                                              &signature.unit) != 0) {
                        hwa_set_error(error, error_size,
                                      "invalid measurement statistic key");
                        goto cleanup;
                    }
                    for (member = member_offset; member < member_end; ++member) {
                        const HWAMeasureObservation *observation =
                            hwa_measure_find_observation(
                                set, offsets,
                                set->group_members[member].item_id,
                                &signature);
                        if (observation != NULL) {
                            quality_flags |= observation->quality_flags;
                            if (observation->status == HWA_MEASURE_STATUS_VALID) {
                                values[valid_count] = observation->value;
                                confidences[valid_count] =
                                    observation->confidence;
                                valid_count++;
                            }
                        } else {
                            quality_flags |=
                                HWA_MEASURE_QUALITY_INCOMPLETE_COVERAGE;
                        }
                    }
                    statistic = &set->statistics[statistic_count];
                    statistic->id = (uint64_t)statistic_count + 1U;
                    statistic->group_id = (uint64_t)group_index + 1U;
                    statistic->kind = signature.kind;
                    statistic->index = signature.index;
                    statistic->unit = signature.unit;
                    statistic->view = signature.view;
                    statistic->quality_flags = quality_flags;
                    hwa_measure_calculate_statistics(
                        values, confidences, valid_count,
                        set->groups[group_index].member_count,
                        &statistic->statistics);
                    statistic_count++;
                }
            }
            member_offset = member_end;
        }
    }
    if (statistic_count != set->statistic_count) {
        hwa_set_error(error, error_size,
                      "measurement statistic count changed while building");
        goto cleanup;
    }
    hwa_measure_work_release(&work_live, value_bytes * 2U);
    value_bytes = 0U;
    hwa_measure_work_release(&work_live, seen_bytes);
    seen_bytes = 0U;
    hwa_measure_work_release(&work_live, offset_bytes);
    offset_bytes = 0U;
    if (work_live != set->retained_work_bytes + retained_add) {
        hwa_set_error(error, error_size,
                      "measurement profile work ledger mismatch");
        goto cleanup;
    }
    set->retained_work_bytes += retained_add;
    result = 0;

cleanup:
    hwa_measure_work_release(&work_live, value_bytes * 2U);
    hwa_measure_work_release(&work_live, seen_bytes);
    hwa_measure_work_release(&work_live, offset_bytes);
    hwa_measure_work_release(&work_live, candidate_bytes);
    free(confidences);
    free(values);
    free(seen);
    free(offsets);
    free(candidates);
    if (result != 0) hwa_measure_profile_arrays_free(set);
    return result;
}

static int hwa_measure_group_compare(const HWAMeasureGroup *a,
                                     const HWAMeasureGroup *b)
{
    int order;
    if (a->item_kind != b->item_kind) return a->item_kind < b->item_kind ? -1 : 1;
    order = strcmp(a->item_role, b->item_role);
    if (order != 0) return order;
    if (a->selector != b->selector) return a->selector < b->selector ? -1 : 1;
    return strcmp(a->value, b->value);
}

static int hwa_measure_statistic_key_compare(const HWAMeasureStatistic *a,
                                             const HWAMeasureStatistic *b)
{
    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
    if (a->index != b->index) return a->index < b->index ? -1 : 1;
    if (a->view != b->view) return a->view < b->view ? -1 : 1;
    return a->unit < b->unit ? -1 : a->unit > b->unit ? 1 : 0;
}

static int hwa_measure_copy_group(const HWAMeasureGroup *source,
                                  uint64_t id,
                                  HWAMeasureGroup *target,
                                  uint64_t *retained)
{
    memset(target, 0, sizeof(*target));
    target->id = id;
    target->item_kind = source->item_kind;
    target->selector = source->selector;
    target->key = hwa_measure_strdup(source->key);
    target->item_role = hwa_measure_strdup(source->item_role);
    target->value = hwa_measure_strdup(source->value);
    if (target->key == NULL || target->item_role == NULL ||
        target->value == NULL) {
        free(target->key);
        free(target->item_role);
        free(target->value);
        memset(target, 0, sizeof(*target));
        return -1;
    }
    *retained += (uint64_t)strlen(target->key) + 1U;
    *retained += (uint64_t)strlen(target->item_role) + 1U;
    *retained += (uint64_t)strlen(target->value) + 1U;
    return 0;
}

void hwa_profile_comparison_options_default(
    HWAProfileComparisonOptions *options)
{
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    options->max_input_bytes = UINT64_C(17179869184);
    options->max_work_bytes = UINT64_C(536870912);
    options->max_contexts = 1000000U;
    options->max_measurements = 4000000U;
    options->max_groups = 1000000U;
    options->max_group_members = 8000000U;
    options->max_statistics = 4000000U;
    options->max_warnings = 100000U;
    options->max_distributions = 4000000U;
    options->max_gaps = 4000000U;
}

static int hwa_profile_options_valid(const HWAProfileComparisonOptions *options)
{
    return options != NULL && options->max_input_bytes != 0U &&
           options->max_work_bytes != 0U && options->max_contexts != 0U &&
           options->max_measurements != 0U && options->max_groups != 0U &&
           options->max_group_members != 0U &&
           options->max_statistics != 0U && options->max_warnings != 0U &&
           options->max_distributions != 0U && options->max_gaps != 0U;
}

void hwa_profile_comparison_set_free(HWAProfileComparisonSet *result)
{
    size_t index;
    if (result == NULL) return;
    free(result->reference_path);
    free(result->model_path);
    for (index = 0U; index < result->group_count; ++index) {
        free(result->groups[index].key);
        free(result->groups[index].item_role);
        free(result->groups[index].value);
    }
    for (index = 0U; index < result->warning_count; ++index) {
        free(result->warnings[index].code);
        free(result->warnings[index].message);
    }
    free(result->groups);
    free(result->distributions);
    free(result->gaps);
    free(result->warnings);
    memset(result, 0, sizeof(*result));
}

static int hwa_profile_shaping_options_match(
    const HWAMeasurementOptions *reference,
    const HWAMeasurementOptions *model)
{
    return reference->fft_size == model->fft_size &&
           reference->hop_size == model->hop_size &&
           reference->pitch_confidence_floor ==
               model->pitch_confidence_floor &&
           reference->spectral_floor_dbfs == model->spectral_floor_dbfs &&
           reference->max_partials == model->max_partials;
}

static uint64_t hwa_profile_string_bytes(const char *text)
{
    return text == NULL ? 0U : (uint64_t)strlen(text) + 1U;
}

static uint64_t hwa_profile_label_bytes(const HWATypedLabels *labels)
{
    return hwa_profile_string_bytes(labels->pitch) +
           hwa_profile_string_bytes(labels->register_name) +
           hwa_profile_string_bytes(labels->dynamic) +
           hwa_profile_string_bytes(labels->articulation) +
           hwa_profile_string_bytes(labels->part) +
           hwa_profile_string_bytes(labels->physical_element) +
           hwa_profile_string_bytes(labels->controller) +
           hwa_profile_string_bytes(labels->technique) +
           hwa_profile_string_bytes(labels->score_section) +
           hwa_profile_string_bytes(labels->transition) +
           hwa_profile_string_bytes(labels->gesture);
}

static void hwa_profile_labels_free(HWATypedLabels *labels)
{
    free(labels->pitch);
    free(labels->register_name);
    free(labels->dynamic);
    free(labels->articulation);
    free(labels->part);
    free(labels->physical_element);
    free(labels->controller);
    free(labels->technique);
    free(labels->score_section);
    free(labels->transition);
    free(labels->gesture);
    memset(labels, 0, sizeof(*labels));
}

static int hwa_profile_compact(HWAMeasurementSet *set,
                               char *error,
                               size_t error_size)
{
    uint64_t freed = 0U;
    size_t index;
    freed += hwa_profile_string_bytes(set->items_path);
    freed += hwa_profile_string_bytes(set->audio_path);
    freed += hwa_profile_string_bytes(set->alignment_path);
    freed += hwa_profile_string_bytes(set->labels_path);
    freed += hwa_profile_string_bytes(set->amendment_path);
    freed += hwa_profile_string_bytes(set->source_score_path);
    freed += (uint64_t)set->context_count * sizeof(*set->contexts);
    freed += (uint64_t)set->measurement_count * sizeof(*set->measurements);
    freed += (uint64_t)set->group_member_count * sizeof(*set->group_members);
    freed += (uint64_t)set->warning_count * sizeof(*set->warnings);
    for (index = 0U; index < set->context_count; ++index) {
        freed += hwa_profile_string_bytes(set->contexts[index].item_key);
        freed += hwa_profile_string_bytes(set->contexts[index].item_role);
        freed += hwa_profile_label_bytes(&set->contexts[index].labels);
    }
    for (index = 0U; index < set->warning_count; ++index) {
        freed += hwa_profile_string_bytes(set->warnings[index].code);
        freed += hwa_profile_string_bytes(set->warnings[index].message);
    }
    if (freed > set->retained_work_bytes) {
        hwa_set_error(error, error_size,
                      "measurement profile compact ledger mismatch");
        return -1;
    }
    free(set->items_path); set->items_path = NULL;
    free(set->audio_path); set->audio_path = NULL;
    free(set->alignment_path); set->alignment_path = NULL;
    free(set->labels_path); set->labels_path = NULL;
    free(set->amendment_path); set->amendment_path = NULL;
    free(set->source_score_path); set->source_score_path = NULL;
    for (index = 0U; index < set->context_count; ++index) {
        free(set->contexts[index].item_key);
        free(set->contexts[index].item_role);
        hwa_profile_labels_free(&set->contexts[index].labels);
    }
    for (index = 0U; index < set->warning_count; ++index) {
        free(set->warnings[index].code);
        free(set->warnings[index].message);
    }
    free(set->contexts); set->contexts = NULL; set->context_count = 0U;
    free(set->measurements); set->measurements = NULL;
    set->measurement_count = 0U;
    free(set->group_members); set->group_members = NULL;
    set->group_member_count = 0U;
    free(set->warnings); set->warnings = NULL; set->warning_count = 0U;
    set->retained_work_bytes -= freed;
    return 0;
}

static void hwa_profile_statistic_range(const HWAMeasurementSet *set,
                                        uint64_t group_id,
                                        size_t *begin,
                                        size_t *end)
{
    size_t low = 0U;
    size_t high = set->statistic_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        if (set->statistics[middle].group_id < group_id) low = middle + 1U;
        else high = middle;
    }
    *begin = low;
    high = set->statistic_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        if (set->statistics[middle].group_id <= group_id) low = middle + 1U;
        else high = middle;
    }
    *end = low;
}

static int hwa_profile_count_union(const HWAMeasurementSet *reference,
                                   const HWAMeasurementSet *model,
                                   size_t *group_count,
                                   size_t *distribution_count)
{
    size_t reference_group = 0U;
    size_t model_group = 0U;
    size_t groups = 0U;
    size_t distributions = 0U;
    while (reference_group < reference->group_count ||
           model_group < model->group_count) {
        const HWAMeasureGroup *reference_value =
            reference_group < reference->group_count
                ? &reference->groups[reference_group] : NULL;
        const HWAMeasureGroup *model_value =
            model_group < model->group_count
                ? &model->groups[model_group] : NULL;
        int order = reference_value == NULL ? 1 :
                    model_value == NULL ? -1 :
                    hwa_measure_group_compare(reference_value, model_value);
        size_t reference_begin = 0U;
        size_t reference_end = 0U;
        size_t model_begin = 0U;
        size_t model_end = 0U;
        if (groups == SIZE_MAX) return -1;
        groups++;
        if (order <= 0) {
            hwa_profile_statistic_range(reference, reference_value->id,
                                        &reference_begin, &reference_end);
        }
        if (order >= 0) {
            hwa_profile_statistic_range(model, model_value->id,
                                        &model_begin, &model_end);
        }
        while (reference_begin < reference_end || model_begin < model_end) {
            const HWAMeasureStatistic *a =
                reference_begin < reference_end
                    ? &reference->statistics[reference_begin] : NULL;
            const HWAMeasureStatistic *b =
                model_begin < model_end
                    ? &model->statistics[model_begin] : NULL;
            int statistic_order = a == NULL ? 1 : b == NULL ? -1 :
                                  hwa_measure_statistic_key_compare(a, b);
            if (distributions == SIZE_MAX) return -1;
            distributions++;
            if (statistic_order <= 0) reference_begin++;
            if (statistic_order >= 0) model_begin++;
        }
        if (order <= 0) reference_group++;
        if (order >= 0) model_group++;
    }
    *group_count = groups;
    *distribution_count = distributions;
    return 0;
}

static void hwa_profile_build_gap(HWAProfileGap *gap,
                                  uint64_t id,
                                  const HWAProfileDistribution *distribution,
                                  uint32_t quality_flags)
{
    const HWAMeasureStatistics *reference =
        &distribution->reference_statistics;
    const HWAMeasureStatistics *model = &distribution->model_statistics;
    double pooled_variance;
    double pooled_sd;
    double robust_iqr_scale;
    double scale;
    double mean_component;
    double quantile_component;
    double scaled_quantile;

    memset(gap, 0, sizeof(*gap));
    gap->id = id;
    gap->distribution_id = distribution->id;
    gap->quality_flags = quality_flags;
    if (!distribution->reference_valid || !distribution->model_valid ||
        !reference->valid || !model->valid) {
        gap->quality_flags |= HWA_MEASURE_QUALITY_INCOMPLETE_COVERAGE;
        return;
    }
    gap->mean_delta = model->mean - reference->mean;
    gap->median_delta = model->q50 - reference->q50;
    gap->quantile_distance =
        (fabs(model->q05 - reference->q05) +
         fabs(model->q25 - reference->q25) +
         fabs(model->q50 - reference->q50) +
         fabs(model->q75 - reference->q75) +
         fabs(model->q95 - reference->q95)) / 5.0;
    gap->mean_delta_valid = 1;
    gap->median_delta_valid = 1;
    gap->quantile_distance_valid = 1;
    pooled_variance =
        ((double)reference->valid_count *
             reference->population_sd * reference->population_sd +
         (double)model->valid_count *
             model->population_sd * model->population_sd) /
        (double)(reference->valid_count + model->valid_count);
    pooled_sd = sqrt(pooled_variance);
    if (pooled_sd > 0.0) {
        gap->standardized_mean_shift = gap->mean_delta / pooled_sd;
        gap->standardized_mean_shift_valid = 1;
        mean_component = fabs(gap->standardized_mean_shift) /
                         (1.0 + fabs(gap->standardized_mean_shift));
    } else {
        mean_component = gap->mean_delta == 0.0 ? 0.0 : 1.0;
    }
    if (reference->total_count != 0U && model->total_count != 0U) {
        double reference_coverage = (double)reference->valid_count /
                                    (double)reference->total_count;
        double model_coverage = (double)model->valid_count /
                                (double)model->total_count;
        gap->valid_coverage = reference_coverage < model_coverage
                                  ? reference_coverage : model_coverage;
        gap->valid_coverage_valid = 1;
    }
    robust_iqr_scale =
        ((reference->q75 - reference->q25) +
         (model->q75 - model->q25)) / (2.0 * 1.349);
    scale = pooled_sd > robust_iqr_scale ? pooled_sd : robust_iqr_scale;
    if (scale > 0.0) {
        scaled_quantile = gap->quantile_distance / scale;
        quantile_component = scaled_quantile / (1.0 + scaled_quantile);
    } else {
        quantile_component = gap->quantile_distance == 0.0 ? 0.0 : 1.0;
    }
    if (gap->valid_coverage_valid) {
        gap->gap_score = gap->valid_coverage *
                         (mean_component + quantile_component) / 2.0;
        gap->gap_score_valid = 1;
    }
}

typedef struct HWAGapRankEntry {
    HWAProfileGap *gap;
    const HWAProfileDistribution *distribution;
    const HWAMeasureGroup *group;
} HWAGapRankEntry;

static int hwa_profile_gap_rank_compare(const void *left, const void *right)
{
    const HWAGapRankEntry *a = (const HWAGapRankEntry *)left;
    const HWAGapRankEntry *b = (const HWAGapRankEntry *)right;
    int order;
    double a_median = fabs(a->gap->median_delta);
    double b_median = fabs(b->gap->median_delta);
    if (a->gap->gap_score != b->gap->gap_score) {
        return a->gap->gap_score > b->gap->gap_score ? -1 : 1;
    }
    if (a->gap->quantile_distance != b->gap->quantile_distance) {
        return a->gap->quantile_distance > b->gap->quantile_distance ? -1 : 1;
    }
    if (a_median != b_median) return a_median > b_median ? -1 : 1;
    order = hwa_measure_group_compare(a->group, b->group);
    if (order != 0) return order;
    if (a->distribution->kind != b->distribution->kind) {
        return a->distribution->kind < b->distribution->kind ? -1 : 1;
    }
    if (a->distribution->index != b->distribution->index) {
        return a->distribution->index < b->distribution->index ? -1 : 1;
    }
    if (a->distribution->view != b->distribution->view) {
        return a->distribution->view < b->distribution->view ? -1 : 1;
    }
    return a->distribution->unit < b->distribution->unit ? -1 :
           a->distribution->unit > b->distribution->unit ? 1 : 0;
}

static int hwa_profile_rank_gaps(HWAProfileComparisonSet *result,
                                 uint64_t max_result_work,
                                 char *error,
                                 size_t error_size)
{
    HWAGapRankEntry *entries;
    size_t count = 0U;
    size_t index;
    if (result->gap_count == 0U) return 0;
    if (result->gap_count > SIZE_MAX / sizeof(*entries)) {
        hwa_set_error(error, error_size, "profile gap rank storage overflows");
        return -1;
    }
    if ((uint64_t)result->gap_count * sizeof(*entries) >
            max_result_work - result->retained_work_bytes) {
        hwa_set_error(error, error_size,
                      "profile gap ranks exceed the work-byte limit");
        return -1;
    }
    entries = (HWAGapRankEntry *)malloc(result->gap_count * sizeof(*entries));
    if (entries == NULL) {
        hwa_set_error(error, error_size, "out of memory for profile gap ranks");
        return -1;
    }
    for (index = 0U; index < result->gap_count; ++index) {
        HWAProfileGap *gap = &result->gaps[index];
        const HWAProfileDistribution *distribution =
            &result->distributions[gap->distribution_id - 1U];
        if (!gap->gap_score_valid) continue;
        entries[count].gap = gap;
        entries[count].distribution = distribution;
        entries[count].group = &result->groups[distribution->group_id - 1U];
        count++;
    }
    qsort(entries, count, sizeof(*entries), hwa_profile_gap_rank_compare);
    for (index = 0U; index < count; ++index) {
        entries[index].gap->rank = index + 1U;
    }
    free(entries);
    return 0;
}

static int hwa_profile_build_comparison(
    const HWAMeasurementSet *reference,
    const HWAMeasurementSet *model,
    HWAProfileComparisonSet *result,
    uint64_t max_result_work,
    char *error,
    size_t error_size)
{
    size_t group_count;
    size_t distribution_count;
    size_t reference_group = 0U;
    size_t model_group = 0U;
    size_t output_group = 0U;
    size_t output_distribution = 0U;
    uint64_t retained = 0U;
    uint64_t array_bytes;
    uint64_t string_bytes = 0U;
    size_t string_reference_group = 0U;
    size_t string_model_group = 0U;
    if (hwa_profile_count_union(reference, model, &group_count,
                                &distribution_count) != 0 ||
        group_count > result->options.max_groups ||
        distribution_count > result->options.max_distributions ||
        distribution_count > result->options.max_gaps ||
        group_count > SIZE_MAX / sizeof(*result->groups) ||
        distribution_count > SIZE_MAX / sizeof(*result->distributions) ||
        distribution_count > SIZE_MAX / sizeof(*result->gaps)) {
        hwa_set_error(error, error_size,
                      "profile comparison row limit exceeded");
        return -1;
    }
#if SIZE_MAX >= UINT64_MAX
    if ((uint64_t)group_count >
            UINT64_MAX / (uint64_t)sizeof(*result->groups) ||
        (uint64_t)distribution_count >
            UINT64_MAX /
                ((uint64_t)sizeof(*result->distributions) +
                 (uint64_t)sizeof(*result->gaps))) {
        hwa_set_error(error, error_size,
                      "profile comparison storage overflows");
        return -1;
    }
#endif
    array_bytes = (uint64_t)group_count * sizeof(*result->groups) +
                  (uint64_t)distribution_count *
                      (sizeof(*result->distributions) + sizeof(*result->gaps));
    while (string_reference_group < reference->group_count ||
           string_model_group < model->group_count) {
        const HWAMeasureGroup *a =
            string_reference_group < reference->group_count
                ? &reference->groups[string_reference_group] : NULL;
        const HWAMeasureGroup *b =
            string_model_group < model->group_count
                ? &model->groups[string_model_group] : NULL;
        int order = a == NULL ? 1 : b == NULL ? -1 :
                    hwa_measure_group_compare(a, b);
        const HWAMeasureGroup *source = order <= 0 ? a : b;
        uint64_t next = (uint64_t)strlen(source->key) + 1U +
                        (uint64_t)strlen(source->item_role) + 1U +
                        (uint64_t)strlen(source->value) + 1U;
        if (next > UINT64_MAX - string_bytes) {
            hwa_set_error(error, error_size,
                          "profile comparison strings overflow");
            return -1;
        }
        string_bytes += next;
        if (order <= 0) string_reference_group++;
        if (order >= 0) string_model_group++;
    }
    if (array_bytes > max_result_work ||
        string_bytes > max_result_work - array_bytes) {
        hwa_set_error(error, error_size,
                      "profile comparison exceeds the work-byte limit");
        return -1;
    }
    result->groups = group_count == 0U ? NULL :
        (HWAMeasureGroup *)calloc(group_count, sizeof(*result->groups));
    result->distributions = distribution_count == 0U ? NULL :
        (HWAProfileDistribution *)calloc(distribution_count,
                                         sizeof(*result->distributions));
    result->gaps = distribution_count == 0U ? NULL :
        (HWAProfileGap *)calloc(distribution_count, sizeof(*result->gaps));
    if ((group_count != 0U && result->groups == NULL) ||
        (distribution_count != 0U &&
         (result->distributions == NULL || result->gaps == NULL))) {
        hwa_set_error(error, error_size,
                      "out of memory for profile comparison");
        return -1;
    }
    result->group_count = group_count;
    result->distribution_count = distribution_count;
    result->gap_count = distribution_count;
    retained = array_bytes;
    while (reference_group < reference->group_count ||
           model_group < model->group_count) {
        const HWAMeasureGroup *reference_value =
            reference_group < reference->group_count
                ? &reference->groups[reference_group] : NULL;
        const HWAMeasureGroup *model_value =
            model_group < model->group_count
                ? &model->groups[model_group] : NULL;
        int order = reference_value == NULL ? 1 :
                    model_value == NULL ? -1 :
                    hwa_measure_group_compare(reference_value, model_value);
        const HWAMeasureGroup *source = order <= 0 ? reference_value : model_value;
        size_t reference_begin = 0U;
        size_t reference_end = 0U;
        size_t model_begin = 0U;
        size_t model_end = 0U;
        if (hwa_measure_copy_group(source, (uint64_t)output_group + 1U,
                                   &result->groups[output_group],
                                   &retained) != 0) {
            hwa_set_error(error, error_size,
                          "out of memory for comparison group strings");
            return -1;
        }
        if (order <= 0) {
            hwa_profile_statistic_range(reference, reference_value->id,
                                        &reference_begin, &reference_end);
        }
        if (order >= 0) {
            hwa_profile_statistic_range(model, model_value->id,
                                        &model_begin, &model_end);
        }
        while (reference_begin < reference_end || model_begin < model_end) {
            const HWAMeasureStatistic *a =
                reference_begin < reference_end
                    ? &reference->statistics[reference_begin] : NULL;
            const HWAMeasureStatistic *b =
                model_begin < model_end
                    ? &model->statistics[model_begin] : NULL;
            int statistic_order = a == NULL ? 1 : b == NULL ? -1 :
                                  hwa_measure_statistic_key_compare(a, b);
            const HWAMeasureStatistic *statistic_source =
                statistic_order <= 0 ? a : b;
            HWAProfileDistribution *distribution =
                &result->distributions[output_distribution];
            uint32_t quality_flags = 0U;
            distribution->id = (uint64_t)output_distribution + 1U;
            distribution->group_id = (uint64_t)output_group + 1U;
            distribution->kind = statistic_source->kind;
            distribution->index = statistic_source->index;
            distribution->unit = statistic_source->unit;
            distribution->view = statistic_source->view;
            if (statistic_order <= 0) {
                distribution->reference_statistics = a->statistics;
                distribution->reference_valid = a->statistics.valid;
                quality_flags |= a->quality_flags;
                reference_begin++;
            }
            if (statistic_order >= 0) {
                distribution->model_statistics = b->statistics;
                distribution->model_valid = b->statistics.valid;
                quality_flags |= b->quality_flags;
                model_begin++;
            }
            hwa_profile_build_gap(
                &result->gaps[output_distribution],
                (uint64_t)output_distribution + 1U,
                distribution, quality_flags);
            output_distribution++;
        }
        output_group++;
        if (order <= 0) reference_group++;
        if (order >= 0) model_group++;
    }
    if (output_group != group_count ||
        output_distribution != distribution_count) {
        hwa_set_error(error, error_size,
                      "profile comparison counts changed while building");
        return -1;
    }
    if (retained != array_bytes + string_bytes ||
        retained > max_result_work) {
        hwa_set_error(error, error_size,
                      "profile comparison exceeds the work-byte limit");
        return -1;
    }
    result->retained_work_bytes = retained;
    return hwa_profile_rank_gaps(result, max_result_work,
                                 error, error_size);
}

int hwa_compare_measure_files(
    const char *reference_path,
    const char *model_path,
    const HWAProfileComparisonOptions *options,
    HWAProfileComparisonSet *result,
    char *error,
    size_t error_size)
{
    HWAProfileComparisonOptions copied_options;
    HWAProfileComparisonOptions read_limits;
    HWAMeasurementSet reference;
    HWAMeasurementSet model;
    char reference_hash[HWA_SHA256_HEX_SIZE];
    char model_hash[HWA_SHA256_HEX_SIZE];
    char check_hash[HWA_SHA256_HEX_SIZE];
    uint64_t command_remaining;
    uint64_t path_bytes;
    int build_result;

    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (result == NULL) {
        hwa_set_error(error, error_size, "missing profile comparison result");
        return -1;
    }
    if (options != NULL) copied_options = *options;
    else hwa_profile_comparison_options_default(&copied_options);
    memset(result, 0, sizeof(*result));
    result->options = copied_options;
    memset(&reference, 0, sizeof(reference));
    memset(&model, 0, sizeof(model));
    if (!hwa_profile_options_valid(&copied_options) ||
        reference_path == NULL || reference_path[0] == '\0' ||
        model_path == NULL || model_path[0] == '\0' ||
        strcmp(reference_path, "-") == 0 || strcmp(model_path, "-") == 0) {
        hwa_set_error(error, error_size,
                      "invalid profile comparison paths or options");
        return -1;
    }
    read_limits = copied_options;
    if (hwa_measure_file_read(reference_path, &read_limits, &reference,
                              reference_hash, error, error_size) != 0 ||
        reference.retained_work_bytes > copied_options.max_work_bytes) {
        hwa_measurement_set_free(&reference);
        return -1;
    }
    if (hwa_profile_compact(&reference, error, error_size) != 0) {
        hwa_measurement_set_free(&reference);
        return -1;
    }
    command_remaining = copied_options.max_work_bytes -
                        reference.retained_work_bytes;
    if (command_remaining == 0U) {
        hwa_set_error(error, error_size,
                      "reference profile leaves no comparison work");
        hwa_measurement_set_free(&reference);
        return -1;
    }
    read_limits.max_work_bytes = command_remaining;
    if (hwa_measure_file_read(model_path, &read_limits, &model,
                              model_hash, error, error_size) != 0) {
        hwa_measurement_set_free(&model);
        hwa_measurement_set_free(&reference);
        return -1;
    }
    if (model.retained_work_bytes > command_remaining) {
        hwa_set_error(error, error_size,
                      "model profile exceeds remaining comparison work");
        hwa_measurement_set_free(&model);
        hwa_measurement_set_free(&reference);
        return -1;
    }
    if (hwa_profile_compact(&model, error, error_size) != 0) {
        hwa_measurement_set_free(&model);
        hwa_measurement_set_free(&reference);
        return -1;
    }
    command_remaining -= model.retained_work_bytes;
    if (strcmp(reference_hash, model_hash) == 0) {
        hwa_set_error(error, error_size,
                      "reference and model measurement files are identical");
        hwa_measurement_set_free(&model);
        hwa_measurement_set_free(&reference);
        return -1;
    }
    if (!hwa_profile_shaping_options_match(&reference.options,
                                           &model.options)) {
        hwa_set_error(error, error_size,
                      "measurement profiles use incompatible methods");
        hwa_measurement_set_free(&model);
        hwa_measurement_set_free(&reference);
        return -1;
    }
    path_bytes = (uint64_t)strlen(reference_path) + 1U +
                 (uint64_t)strlen(model_path) + 1U;
    if (path_bytes > command_remaining) {
        hwa_set_error(error, error_size,
                      "profile paths exceed remaining comparison work");
        hwa_measurement_set_free(&model);
        hwa_measurement_set_free(&reference);
        return -1;
    }
    command_remaining -= path_bytes;
    result->reference_path = hwa_measure_strdup(reference_path);
    result->model_path = hwa_measure_strdup(model_path);
    if (result->reference_path == NULL || result->model_path == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for profile input paths");
        hwa_measurement_set_free(&model);
        hwa_measurement_set_free(&reference);
        hwa_profile_comparison_set_free(result);
        return -1;
    }
    memcpy(result->reference_sha256, reference_hash, sizeof(reference_hash));
    memcpy(result->model_sha256, model_hash, sizeof(model_hash));
    build_result = hwa_profile_build_comparison(&reference, &model, result,
                                                command_remaining,
                                                error, error_size);
    if (build_result == 0 &&
        (hwa_sha256_file(reference_path, copied_options.max_input_bytes,
                         check_hash, error, error_size) != 0 ||
         strcmp(check_hash, reference_hash) != 0 ||
         hwa_sha256_file(model_path, copied_options.max_input_bytes,
                         check_hash, error, error_size) != 0 ||
         strcmp(check_hash, model_hash) != 0)) {
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "measurement input changed during comparison");
        }
        build_result = -1;
    }
    hwa_measurement_set_free(&model);
    hwa_measurement_set_free(&reference);
    if (build_result != 0) {
        hwa_profile_comparison_set_free(result);
        result->options = copied_options;
        return -1;
    }
    result->retained_work_bytes += path_bytes;
    return 0;
}

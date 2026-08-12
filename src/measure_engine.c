#include "measure_engine.h"

#include "dsp.h"
#include "internal.h"
#include "item_file.h"
#include "measure_compare.h"
#include "sha256.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HWA_MEASURE_MAX_PARTIALS 32U
#define HWA_MEASURE_SERIES_PER_ITEM 512U
#define HWA_MEASURE_ATTACK_SHAPE_BINS 16U
#define HWA_MEASURE_RANGE_BLOCK_FRAMES 256U
#define HWA_MEASURE_PI 3.14159265358979323846264338327950288
#define HWA_MEASURE_DB_FLOOR (-300.0)

typedef struct HWAMeasureWork {
    uint64_t limit;
    uint64_t live;
    uint64_t retained;
} HWAMeasureWork;

typedef struct HWAMeasureRunning {
    uint64_t count;
    long double sum;
    long double sum_squares;
    long double sum_time;
    long double sum_time_squares;
    long double sum_time_value;
    double minimum;
    double maximum;
    double first;
    double last;
} HWAMeasureRunning;

typedef struct HWAMeasureSeriesPoint {
    double time_seconds;
    double pitch_cents;
    double level_db;
    double centroid_hz;
    int pitch_valid;
} HWAMeasureSeriesPoint;

typedef struct HWAMeasurePartialAccumulator {
    HWAMeasureRunning frequency_error;
    HWAMeasureRunning level;
    uint64_t present_count;
    double birth_seconds;
    double loss_seconds;
    int born;
} HWAMeasurePartialAccumulator;

typedef struct HWAMeasureItemAccumulator {
    const HWAItem *item;
    long double sample_energy;
    uint64_t sample_count;
    double sample_peak;
    long double context_energy;
    uint64_t context_count;
    long double attack_shape_energy[HWA_MEASURE_ATTACK_SHAPE_BINS];
    uint64_t attack_shape_count[HWA_MEASURE_ATTACK_SHAPE_BINS];

    HWAMeasureRunning level;
    HWAMeasureRunning centroid;
    HWAMeasureRunning flatness;
    HWAMeasureRunning band[HWA_BAND_COUNT];
    HWAMeasureRunning pitch_hz;
    HWAMeasureRunning pitch_cents;
    HWAMeasureRunning harmonic_level;
    HWAMeasureRunning residual_level;
    HWAMeasureRunning hnr;
    HWAMeasureRunning residual_band[HWA_BAND_COUNT];
    HWAMeasurePartialAccumulator partial[HWA_MEASURE_MAX_PARTIALS];

    uint64_t frame_count;
    uint64_t pitch_frame_count;
    long double pitch_confidence_sum;
    uint64_t octave_fault_count;
    uint64_t transient_count;
    uint64_t fixed_state_count;
    double noise_peak_time;
    double noise_peak_level;
    int have_noise_peak;
    double prior_level;
    double prior_centroid;
    int have_prior_frame;

    size_t series_offset;
    size_t series_capacity;
    size_t series_count;
    size_t series_stride;
    size_t series_seen;

    double expected_pitch_hz;
    int midi_note;
    int target_status;
    int transition_from_midi;
    int transition_to_midi;
    int transition_target_valid;
    double transition_tracking_hz;
    int transition_tracking_valid;
    double local_contrast;
    double accent_size;
    double repeated_attack_similarity;
    double repeated_pitch_similarity;
    int local_contrast_valid;
    int accent_size_valid;
    int repeated_attack_valid;
    int repeated_pitch_valid;
} HWAMeasureItemAccumulator;

typedef struct HWAMeasurePitchFrame {
    double pitch_hz;
    double confidence;
    double harmonic_power;
    double residual_power;
    double hnr_db;
    double partial_frequency_error[HWA_MEASURE_MAX_PARTIALS];
    double partial_power[HWA_MEASURE_MAX_PARTIALS];
    unsigned char partial_present[HWA_MEASURE_MAX_PARTIALS];
    double residual_band_power[HWA_BAND_COUNT];
    int octave_fault;
    int valid;
} HWAMeasurePitchFrame;

typedef struct HWAMeasureItemOrder {
    uint64_t sample;
    size_t index;
} HWAMeasureItemOrder;

typedef struct HWAMeasureEngine {
    const HWAItemSet *items;
    const HWAMeasurementOptions *options;
    HWAMeasurementSet *result;
    HWAMeasureWork work;
    uint32_t sample_rate;
    uint64_t total_frames;
    uint64_t pushed_frames;
    uint64_t next_frame_start;
    size_t spectrum_bins;
    size_t observation_capacity;
    size_t transform_count;
    uint64_t evaluations;

    HWAMeasureItemAccumulator *accumulators;
    HWAMeasureItemOrder *start_order;
    HWAMeasureItemOrder *sample_start_order;
    size_t *active_items;
    size_t active_count;
    size_t next_start;
    size_t *sample_active_items;
    size_t sample_active_count;
    size_t sample_next_start;
    unsigned char *event_multi_pitch;
    HWAMeasureSeriesPoint *series;
    size_t series_capacity;

    double *sample_ring;
    double *window;
    double window_energy;
    HwaDspComplex *fft;
    double *power;
    double *previous_magnitude;
    int have_previous_spectrum;

    double *block_mono;
    double *range_block;
    size_t range_block_count;
    uint64_t range_block_start;
    long double *block_energy_prefix;
    double *peak_tree;
    size_t peak_tree_leaf_count;
} HWAMeasureEngine;

static int hwa_measure_size_multiply(size_t count,
                                     size_t size,
                                     size_t *bytes)
{
    if (bytes == NULL || (size != 0U && count > SIZE_MAX / size)) return -1;
    *bytes = count * size;
    return 0;
}

static void *hwa_measure_allocate(HWAMeasureWork *work,
                                  size_t count,
                                  size_t size,
                                  int retained)
{
    size_t bytes;
    void *memory;

    if (work == NULL || hwa_measure_size_multiply(count, size, &bytes) != 0 ||
        (uint64_t)bytes > work->limit - work->live) {
        return NULL;
    }
    memory = calloc(count, size);
    if (memory == NULL && bytes != 0U) return NULL;
    work->live += (uint64_t)bytes;
    if (retained != 0) work->retained += (uint64_t)bytes;
    return memory;
}

static void hwa_measure_release(HWAMeasureWork *work,
                                void *memory,
                                size_t count,
                                size_t size,
                                int retained)
{
    size_t bytes = 0U;

    if (memory == NULL) return;
    if (hwa_measure_size_multiply(count, size, &bytes) == 0 && work != NULL) {
        if ((uint64_t)bytes <= work->live) work->live -= (uint64_t)bytes;
        if (retained != 0 && (uint64_t)bytes <= work->retained) {
            work->retained -= (uint64_t)bytes;
        }
    }
    free(memory);
}

static char *hwa_measure_copy(HWAMeasureWork *work, const char *text)
{
    size_t length;
    char *copy;

    if (text == NULL) return NULL;
    length = strlen(text);
    if (length == SIZE_MAX) return NULL;
    copy = (char *)hwa_measure_allocate(work, length + 1U, 1U, 1);
    if (copy != NULL) memcpy(copy, text, length + 1U);
    return copy;
}

static double hwa_measure_power_db(double power)
{
    return power > 1.0e-30 ? 10.0 * log10(power) : HWA_MEASURE_DB_FLOOR;
}

static double hwa_measure_amplitude_db(double amplitude)
{
    return amplitude > 1.0e-15 ? 20.0 * log10(amplitude)
                               : HWA_MEASURE_DB_FLOOR;
}

static double hwa_measure_clamp(double value, double low, double high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static int hwa_measure_power_of_two(size_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static void hwa_measure_running_push(HWAMeasureRunning *running,
                                     double time,
                                     double value)
{
    if (running->count == 0U) {
        running->minimum = value;
        running->maximum = value;
        running->first = value;
    } else {
        if (value < running->minimum) running->minimum = value;
        if (value > running->maximum) running->maximum = value;
    }
    running->last = value;
    running->count++;
    running->sum += (long double)value;
    running->sum_squares += (long double)value * (long double)value;
    running->sum_time += (long double)time;
    running->sum_time_squares += (long double)time * (long double)time;
    running->sum_time_value += (long double)time * (long double)value;
}

static int hwa_measure_running_mean(const HWAMeasureRunning *running,
                                    double *value)
{
    if (running == NULL || value == NULL || running->count == 0U) return 0;
    *value = (double)(running->sum / (long double)running->count);
    return isfinite(*value) != 0;
}

static int hwa_measure_running_spread(const HWAMeasureRunning *running,
                                      double *value)
{
    long double mean;
    long double variance;

    if (running == NULL || value == NULL || running->count < 2U) return 0;
    mean = running->sum / (long double)running->count;
    variance = running->sum_squares / (long double)running->count -
               mean * mean;
    if (variance < 0.0L && variance > -1.0e-18L) variance = 0.0L;
    if (variance < 0.0L) return 0;
    *value = sqrt((double)variance);
    return isfinite(*value) != 0;
}

static int hwa_measure_running_slope(const HWAMeasureRunning *running,
                                     double *value)
{
    long double count;
    long double denominator;

    if (running == NULL || value == NULL || running->count < 2U) return 0;
    count = (long double)running->count;
    denominator = count * running->sum_time_squares -
                  running->sum_time * running->sum_time;
    if (!(fabsl(denominator) > LDBL_EPSILON)) return 0;
    *value = (double)((count * running->sum_time_value -
                       running->sum_time * running->sum) /
                      denominator);
    return isfinite(*value) != 0;
}

static int hwa_measure_parse_midi(const char *text, int *midi)
{
    char *end = NULL;
    long value;

    if (text == NULL || text[0] == '\0' || midi == NULL) return -1;
    value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 0L || value > 127L) return -1;
    *midi = (int)value;
    return 0;
}

static double hwa_measure_midi_hz(int midi)
{
    return 440.0 * pow(2.0, ((double)midi - 69.0) / 12.0);
}

void hwa_measurement_options_default(HWAMeasurementOptions *options)
{
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    options->decode_block_frames = 4096U;
    options->fft_size = 4096U;
    options->hop_size = 256U;
    options->pitch_confidence_floor = 0.30;
    options->spectral_floor_dbfs = -100.0;
    options->max_partials = 12U;
    options->max_input_bytes = UINT64_C(17179869184);
    options->max_input_frames = UINT64_C(2000000000);
    options->max_work_bytes = UINT64_C(536870912);
    options->max_transforms = 1000000U;
    options->max_series_points = 2000000U;
    options->max_item_frame_evaluations = UINT64_C(100000000);
    options->max_events = 200000U;
    options->max_items = 1000000U;
    options->max_item_members = 2000000U;
    options->max_measurements = 4000000U;
    options->max_groups = 1000000U;
    options->max_group_members = 8000000U;
    options->max_statistics = 4000000U;
    options->max_warnings = 100000U;
}

static int hwa_measure_options_valid(const HWAMeasurementOptions *options)
{
    return options != NULL && options->decode_block_frames != 0U &&
           options->decode_block_frames <= 1048576U &&
           hwa_measure_power_of_two(options->fft_size) &&
           options->fft_size >= 256U && options->fft_size <= 16384U &&
           options->hop_size != 0U && options->hop_size <= options->fft_size &&
           isfinite(options->pitch_confidence_floor) &&
           options->pitch_confidence_floor >= 0.0 &&
           options->pitch_confidence_floor <= 1.0 &&
           isfinite(options->spectral_floor_dbfs) &&
           options->spectral_floor_dbfs >= -300.0 &&
           options->spectral_floor_dbfs <= 0.0 &&
           options->max_partials != 0U &&
           options->max_partials <= HWA_MEASURE_MAX_PARTIALS &&
           options->max_input_bytes != 0U &&
           options->max_input_frames != 0U &&
           options->max_work_bytes != 0U && options->max_transforms != 0U &&
           options->max_series_points != 0U &&
           options->max_item_frame_evaluations != 0U &&
           options->max_events != 0U && options->max_items != 0U &&
           options->max_item_members != 0U &&
           options->max_measurements != 0U && options->max_groups != 0U &&
           options->max_group_members != 0U &&
           options->max_statistics != 0U && options->max_warnings != 0U;
}

static void hwa_measure_labels_free(HWATypedLabels *labels)
{
    if (labels == NULL) return;
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

void hwa_measurement_set_free(HWAMeasurementSet *result)
{
    size_t index;

    if (result == NULL) return;
    free(result->items_path);
    free(result->audio_path);
    free(result->alignment_path);
    free(result->labels_path);
    free(result->amendment_path);
    free(result->source_score_path);
    for (index = 0U; index < result->context_count; ++index) {
        free(result->contexts[index].item_key);
        free(result->contexts[index].item_role);
        hwa_measure_labels_free(&result->contexts[index].labels);
    }
    for (index = 0U; index < result->group_count; ++index) {
        free(result->groups[index].key);
        free(result->groups[index].item_role);
        free(result->groups[index].value);
    }
    for (index = 0U; index < result->warning_count; ++index) {
        free(result->warnings[index].code);
        free(result->warnings[index].message);
    }
    free(result->contexts);
    free(result->measurements);
    free(result->groups);
    free(result->group_members);
    free(result->statistics);
    free(result->warnings);
    memset(result, 0, sizeof(*result));
}

static const HWAItemEvent *hwa_measure_event(const HWAItemSet *items,
                                             uint64_t event_id)
{
    if (event_id == 0U || event_id > (uint64_t)items->event_count) return NULL;
    if (items->events[event_id - 1U].id != event_id) return NULL;
    return &items->events[event_id - 1U];
}

static void hwa_measure_target_for_item(const HWAItemSet *items,
                                        uint64_t item_id,
                                        const unsigned char *event_multi_pitch,
                                        HWAMeasureItemAccumulator *accumulator,
                                        size_t *source_count)
{
    size_t index;
    int midi = -1;
    int found = 0;
    int multiple = 0;

    *source_count = 0U;
    for (index = 0U; index < items->member_count; ++index) {
        const HWAItemMember *member = &items->members[index];
        const HWAItemEvent *event;
        int event_midi;

        if (member->item_id != item_id ||
            member->role != HWA_ITEM_MEMBER_SOURCE) {
            continue;
        }
        event = hwa_measure_event(items, member->event_id);
        if (event == NULL) continue;
        (*source_count)++;
        if (event_multi_pitch != NULL &&
            event_multi_pitch[member->event_id - 1U] != 0U) {
            multiple = 1;
        }
        if (hwa_measure_parse_midi(event->midi_note, &event_midi) != 0) {
            continue;
        }
        if (!found) {
            midi = event_midi;
            found = 1;
        } else if (midi != event_midi) {
            multiple = 1;
        }
    }
    if (multiple != 0) {
        accumulator->target_status = HWA_MEASURE_STATUS_MULTI_PITCH;
    } else if (found != 0) {
        accumulator->midi_note = midi;
        accumulator->expected_pitch_hz = hwa_measure_midi_hz(midi);
        accumulator->target_status = HWA_MEASURE_STATUS_VALID;
    } else {
        accumulator->target_status = HWA_MEASURE_STATUS_NO_PITCH;
    }
}

static const char *hwa_measure_label_field(const HWATypedLabels *labels,
                                           size_t field)
{
    switch (field) {
    case 0U: return labels->pitch;
    case 1U: return labels->register_name;
    case 2U: return labels->dynamic;
    case 3U: return labels->articulation;
    case 4U: return labels->part;
    case 5U: return labels->physical_element;
    case 6U: return labels->controller;
    case 7U: return labels->technique;
    case 8U: return labels->score_section;
    case 9U: return labels->transition;
    case 10U: return labels->gesture;
    default: return NULL;
    }
}

static void hwa_measure_transition_target(const HWAItemSet *items,
                                          uint64_t item_id,
                                          const unsigned char *event_multi_pitch,
                                          HWAMeasureItemAccumulator *accumulator)
{
    const HWAItemEvent *from = NULL;
    const HWAItemEvent *to = NULL;
    size_t index;

    for (index = 0U; index < items->member_count; ++index) {
        const HWAItemMember *member = &items->members[index];
        if (member->item_id != item_id) continue;
        if (member->role == HWA_ITEM_MEMBER_FROM && from == NULL) {
            from = hwa_measure_event(items, member->event_id);
        } else if (member->role == HWA_ITEM_MEMBER_TO && to == NULL) {
            to = hwa_measure_event(items, member->event_id);
        }
    }
    if (from != NULL && to != NULL && event_multi_pitch != NULL &&
        (event_multi_pitch[from->id - 1U] != 0U ||
         event_multi_pitch[to->id - 1U] != 0U)) {
        accumulator->target_status = HWA_MEASURE_STATUS_MULTI_PITCH;
        return;
    }
    if (from != NULL && to != NULL &&
        hwa_measure_parse_midi(from->midi_note,
                               &accumulator->transition_from_midi) == 0 &&
        hwa_measure_parse_midi(to->midi_note,
                               &accumulator->transition_to_midi) == 0) {
        accumulator->transition_target_valid = 1;
        accumulator->target_status = HWA_MEASURE_STATUS_VALID;
        accumulator->expected_pitch_hz =
            hwa_measure_midi_hz(accumulator->transition_from_midi);
    }
}

static int hwa_measure_context_labels(HWAMeasureWork *work,
                                      const HWAItemSet *items,
                                      uint64_t item_id,
                                      HWATypedLabels *labels)
{
    const char *candidate[11] = {NULL};
    unsigned char agreed[11];
    uint32_t override_flags = UINT32_MAX;
    size_t event_count = 0U;
    size_t member_index;
    size_t field;

    memset(labels, 0, sizeof(*labels));
    memset(agreed, 1, sizeof(agreed));
    for (member_index = 0U; member_index < items->member_count;
         ++member_index) {
        const HWAItemMember *member = &items->members[member_index];
        const HWAItemEvent *event;
        if (member->item_id != item_id ||
            (member->role != HWA_ITEM_MEMBER_SOURCE &&
             member->role != HWA_ITEM_MEMBER_ACTIVE)) {
            continue;
        }
        event = hwa_measure_event(items, member->event_id);
        if (event == NULL) continue;
        event_count++;
        override_flags &= event->labels.override_flags;
        for (field = 0U; field < 11U; ++field) {
            const char *value = hwa_measure_label_field(&event->labels, field);
            if (value == NULL || value[0] == '\0') {
                agreed[field] = 0U;
            } else if (candidate[field] == NULL) {
                candidate[field] = value;
            } else if (strcmp(candidate[field], value) != 0) {
                agreed[field] = 0U;
            }
        }
    }
    if (event_count == 0U) return 0;
#define HWA_MEASURE_COPY_CONSENSUS(number, field_name)                        \
    do {                                                                      \
        if (agreed[number] != 0U && candidate[number] != NULL) {              \
            labels->field_name = hwa_measure_copy(work, candidate[number]);   \
            if (labels->field_name == NULL) return -1;                        \
        }                                                                     \
    } while (0)
    HWA_MEASURE_COPY_CONSENSUS(0U, pitch);
    HWA_MEASURE_COPY_CONSENSUS(1U, register_name);
    HWA_MEASURE_COPY_CONSENSUS(2U, dynamic);
    HWA_MEASURE_COPY_CONSENSUS(3U, articulation);
    HWA_MEASURE_COPY_CONSENSUS(4U, part);
    HWA_MEASURE_COPY_CONSENSUS(5U, physical_element);
    HWA_MEASURE_COPY_CONSENSUS(6U, controller);
    HWA_MEASURE_COPY_CONSENSUS(7U, technique);
    HWA_MEASURE_COPY_CONSENSUS(8U, score_section);
    HWA_MEASURE_COPY_CONSENSUS(9U, transition);
    HWA_MEASURE_COPY_CONSENSUS(10U, gesture);
#undef HWA_MEASURE_COPY_CONSENSUS
    {
        static const uint32_t bits[11] = {
            HWA_LABEL_OVERRIDE_PITCH,
            HWA_LABEL_OVERRIDE_REGISTER,
            HWA_LABEL_OVERRIDE_DYNAMIC,
            HWA_LABEL_OVERRIDE_ARTICULATION,
            HWA_LABEL_OVERRIDE_PART,
            HWA_LABEL_OVERRIDE_PHYSICAL_ELEMENT,
            HWA_LABEL_OVERRIDE_CONTROLLER,
            HWA_LABEL_OVERRIDE_TECHNIQUE,
            HWA_LABEL_OVERRIDE_SCORE_SECTION,
            HWA_LABEL_OVERRIDE_TRANSITION,
            HWA_LABEL_OVERRIDE_GESTURE
        };
        for (field = 0U; field < 11U; ++field) {
            if (agreed[field] == 0U || candidate[field] == NULL) {
                override_flags &= ~bits[field];
            }
        }
    }
    labels->override_flags = override_flags;
    return 0;
}

static int hwa_measure_order_compare(const void *left, const void *right)
{
    const HWAMeasureItemOrder *first =
        (const HWAMeasureItemOrder *)left;
    const HWAMeasureItemOrder *second =
        (const HWAMeasureItemOrder *)right;

    if (first->sample < second->sample) return -1;
    if (first->sample > second->sample) return 1;
    if (first->index < second->index) return -1;
    if (first->index > second->index) return 1;
    return 0;
}

static int hwa_measure_append_observation(HWAMeasureEngine *engine,
                                          uint64_t item_id,
                                          HWAMeasureKind kind,
                                          uint32_t index,
                                          HWAMeasureUnit unit,
                                          HWAMeasureView view,
                                          HWAMeasureStatus status,
                                          double value,
                                          double confidence,
                                          uint32_t evidence,
                                          uint32_t quality,
                                          char *error,
                                          size_t error_size)
{
    HWAMeasureObservation *grown;
    HWAMeasureObservation *observation;
    size_t new_capacity;
    size_t old_bytes;
    size_t new_bytes;

    if (engine->result->measurement_count ==
        engine->options->max_measurements) {
        hwa_set_error(error, error_size,
                      "measurement count exceeds the configured limit");
        return -1;
    }
    if (status == HWA_MEASURE_STATUS_VALID && !isfinite(value)) {
        hwa_set_error(error, error_size,
                      "measurement engine produced a non-finite value");
        return -1;
    }
    if (!isfinite(confidence) || confidence < 0.0 || confidence > 1.0) {
        hwa_set_error(error, error_size,
                      "measurement engine produced invalid confidence");
        return -1;
    }
    if (!hwa_measure_kind_index_valid(kind, index,
                                      engine->options->max_partials)) {
        hwa_set_error(error, error_size,
                      "measurement engine produced an invalid kind index");
        return -1;
    }
    if (engine->result->measurement_count == engine->observation_capacity) {
        new_capacity = engine->observation_capacity == 0U
                           ? 256U
                           : engine->observation_capacity * 2U;
        if (new_capacity < engine->observation_capacity ||
            new_capacity > engine->options->max_measurements) {
            new_capacity = engine->options->max_measurements;
        }
        if (hwa_measure_size_multiply(engine->observation_capacity,
                                      sizeof(*grown), &old_bytes) != 0 ||
            hwa_measure_size_multiply(new_capacity, sizeof(*grown),
                                      &new_bytes) != 0 ||
            (uint64_t)(new_bytes - old_bytes) >
                engine->work.limit - engine->work.live) {
            hwa_set_error(error, error_size,
                          "measurement output exceeds the work limit");
            return -1;
        }
        grown = (HWAMeasureObservation *)realloc(
            engine->result->measurements, new_bytes);
        if (grown == NULL) {
            hwa_set_error(error, error_size,
                          "out of memory for measurement output");
            return -1;
        }
        memset(grown + engine->observation_capacity, 0,
               (new_capacity - engine->observation_capacity) *
                   sizeof(*grown));
        engine->result->measurements = grown;
        engine->work.live += (uint64_t)(new_bytes - old_bytes);
        engine->work.retained += (uint64_t)(new_bytes - old_bytes);
        engine->observation_capacity = new_capacity;
    }
    observation = &engine->result->measurements[
        engine->result->measurement_count];
    observation->id = (uint64_t)engine->result->measurement_count + 1U;
    observation->item_id = item_id;
    observation->kind = kind;
    observation->index = index;
    observation->unit = unit;
    observation->view = view;
    observation->status = status;
    observation->value = status == HWA_MEASURE_STATUS_VALID ? value : 0.0;
    observation->confidence = confidence;
    observation->evidence_flags = evidence;
    observation->quality_flags = quality;
    engine->result->measurement_count++;
    return 0;
}

static uint32_t hwa_measure_item_quality(const HWAItem *item)
{
    uint32_t quality = 0U;

    if ((item->quality_flags & HWA_ITEM_QUALITY_LOW_CONFIDENCE) != 0U) {
        quality |= HWA_MEASURE_QUALITY_LOW_CONFIDENCE |
                   HWA_MEASURE_QUALITY_INHERITED_ITEM;
    }
    if ((item->quality_flags &
         (HWA_ITEM_QUALITY_COLLAPSED | HWA_ITEM_QUALITY_TRUNCATED)) != 0U) {
        quality |= HWA_MEASURE_QUALITY_BOUNDARY_LIMITED |
                   HWA_MEASURE_QUALITY_INHERITED_ITEM;
    }
    return quality;
}

static int hwa_measure_prepare_items(HWAMeasureEngine *engine,
                                     char *error,
                                     size_t error_size)
{
    const HWAItemSet *items = engine->items;
    size_t item_count = items->item_count;
    size_t total_series = 0U;
    size_t index;

    if (items->event_count > engine->options->max_events ||
        item_count > engine->options->max_items ||
        items->member_count > engine->options->max_item_members) {
        hwa_set_error(error, error_size,
                      "item data exceeds a Stage 4 count limit");
        return -1;
    }
    engine->accumulators = (HWAMeasureItemAccumulator *)hwa_measure_allocate(
        &engine->work, item_count, sizeof(*engine->accumulators), 0);
    engine->start_order = (HWAMeasureItemOrder *)hwa_measure_allocate(
        &engine->work, item_count, sizeof(*engine->start_order), 0);
    engine->sample_start_order =
        (HWAMeasureItemOrder *)hwa_measure_allocate(
            &engine->work, item_count, sizeof(*engine->sample_start_order), 0);
    engine->active_items = (size_t *)hwa_measure_allocate(
        &engine->work, item_count, sizeof(*engine->active_items), 0);
    engine->sample_active_items = (size_t *)hwa_measure_allocate(
        &engine->work, item_count, sizeof(*engine->sample_active_items), 0);
    engine->result->contexts = (HWAMeasureItemContext *)hwa_measure_allocate(
        &engine->work, item_count, sizeof(*engine->result->contexts), 1);
    engine->event_multi_pitch = (unsigned char *)hwa_measure_allocate(
        &engine->work, items->event_count, 1U, 0);
    if ((item_count != 0U) &&
        (engine->accumulators == NULL || engine->start_order == NULL ||
         engine->sample_start_order == NULL ||
         engine->active_items == NULL ||
         engine->sample_active_items == NULL ||
         engine->result->contexts == NULL ||
         (items->event_count != 0U && engine->event_multi_pitch == NULL))) {
        hwa_set_error(error, error_size,
                      "item measurement state exceeds the work limit");
        return -1;
    }
    engine->result->context_count = item_count;
    for (index = 0U; index < items->member_count; ++index) {
        const HWAItemMember *member = &items->members[index];
        if (member->role == HWA_ITEM_MEMBER_ACTIVE && member->item_id != 0U &&
            member->item_id <= (uint64_t)item_count &&
            items->items[member->item_id - 1U].kind == HWA_ITEM_MULTI_NOTE &&
            strcmp(items->items[member->item_id - 1U].role, "chord") == 0 &&
            member->event_id != 0U &&
            member->event_id <= (uint64_t)items->event_count) {
            engine->event_multi_pitch[member->event_id - 1U] = 1U;
        }
    }
    for (index = 0U; index < item_count; ++index) {
        const HWAItem *item = &items->items[index];
        HWAMeasureItemAccumulator *accumulator =
            &engine->accumulators[index];
        HWAMeasureItemContext *context = &engine->result->contexts[index];
        size_t source_count;
        uint64_t span;
        size_t planned;

        if (item->id != (uint64_t)index + 1U || item->start_sample >
                item->end_sample || item->end_sample > engine->total_frames) {
            hwa_set_error(error, error_size,
                          "item IDs or sample bounds are not canonical");
            return -1;
        }
        accumulator->item = item;
        hwa_measure_target_for_item(items, item->id,
                                    engine->event_multi_pitch, accumulator,
                                    &source_count);
        if (item->kind == HWA_ITEM_MULTI_NOTE) {
            accumulator->target_status = HWA_MEASURE_STATUS_MULTI_PITCH;
        }
        if (item->kind == HWA_ITEM_TRANSITION) {
            hwa_measure_transition_target(items, item->id,
                                          engine->event_multi_pitch,
                                          accumulator);
        }
        context->item_id = item->id;
        context->item_kind = item->kind;
        context->source_event_count = source_count;
        context->item_confidence = item->confidence;
        context->item_quality_flags = item->quality_flags;
        context->start_sample = item->start_sample;
        context->end_sample = item->end_sample;
        context->excluded = item->excluded;
        context->item_key = hwa_measure_copy(&engine->work, item->key);
        context->item_role = hwa_measure_copy(&engine->work, item->role);
        if (context->item_key == NULL || context->item_role == NULL ||
            hwa_measure_context_labels(&engine->work, items, item->id,
                                       &context->labels) != 0) {
            hwa_set_error(error, error_size,
                          "item context exceeds the measurement work limit");
            return -1;
        }
        engine->start_order[index].sample = item->start_sample;
        engine->start_order[index].index = index;
        engine->sample_start_order[index].sample = item->start_sample;
        engine->sample_start_order[index].index = index;
        if ((item->kind == HWA_ITEM_ATTACK || item->kind == HWA_ITEM_NOTE) &&
            item->start_sample > (uint64_t)engine->sample_rate / 20U) {
            engine->sample_start_order[index].sample =
                item->start_sample - (uint64_t)engine->sample_rate / 20U;
        } else if (item->kind == HWA_ITEM_ATTACK ||
                   item->kind == HWA_ITEM_NOTE) {
            engine->sample_start_order[index].sample = 0U;
        }
        span = item->end_sample - item->start_sample;
        planned = span == 0U
                      ? 0U
                      : (size_t)((span - 1U) /
                                     (uint64_t)engine->options->hop_size +
                                 1U);
        if (planned > HWA_MEASURE_SERIES_PER_ITEM) {
            accumulator->series_stride =
                (planned + HWA_MEASURE_SERIES_PER_ITEM - 1U) /
                HWA_MEASURE_SERIES_PER_ITEM;
            planned = (planned + accumulator->series_stride - 1U) /
                      accumulator->series_stride;
        } else {
            accumulator->series_stride = 1U;
        }
        if (accumulator->target_status != HWA_MEASURE_STATUS_VALID ||
            (item->kind != HWA_ITEM_NOTE && item->kind != HWA_ITEM_BODY &&
             item->kind != HWA_ITEM_ATTACK &&
             item->kind != HWA_ITEM_RELEASE &&
             item->kind != HWA_ITEM_TRANSITION)) {
            planned = 0U;
        }
        if (planned > engine->options->max_series_points - total_series) {
            hwa_set_error(error, error_size,
                          "measurement pitch series exceeds its point limit");
            return -1;
        }
        accumulator->series_offset = total_series;
        accumulator->series_capacity = planned;
        total_series += planned;
    }
    qsort(engine->start_order, item_count, sizeof(*engine->start_order),
          hwa_measure_order_compare);
    qsort(engine->sample_start_order, item_count,
          sizeof(*engine->sample_start_order), hwa_measure_order_compare);
    engine->series = (HWAMeasureSeriesPoint *)hwa_measure_allocate(
        &engine->work, total_series, sizeof(*engine->series), 0);
    if (total_series != 0U && engine->series == NULL) {
        hwa_set_error(error, error_size,
                      "measurement pitch series exceeds the work limit");
        return -1;
    }
    engine->series_capacity = total_series;
    return 0;
}

static size_t hwa_measure_band_for_frequency(double frequency)
{
    static const double edges[HWA_BAND_COUNT - 1U] = {
        60.0, 120.0, 250.0, 500.0, 1000.0,
        2000.0, 4000.0, 8000.0, 16000.0
    };
    size_t band;

    for (band = 0U; band + 1U < HWA_BAND_COUNT; ++band) {
        if (frequency < edges[band]) return band;
    }
    return HWA_BAND_COUNT - 1U;
}

static int hwa_measure_prepare_buffers(HWAMeasureEngine *engine,
                                       char *error,
                                       size_t error_size)
{
    size_t fft_size = engine->options->fft_size;
    size_t block_size = HWA_MEASURE_RANGE_BLOCK_FRAMES;
    size_t leaf_count = 1U;
    size_t index;

    while (leaf_count < block_size) {
        if (leaf_count > SIZE_MAX / 2U) {
            hwa_set_error(error, error_size,
                          "measurement peak tree size overflows");
            return -1;
        }
        leaf_count *= 2U;
    }
    engine->spectrum_bins = fft_size / 2U + 1U;
    engine->peak_tree_leaf_count = leaf_count;
    engine->sample_ring = (double *)hwa_measure_allocate(
        &engine->work, fft_size, sizeof(*engine->sample_ring), 0);
    engine->window = (double *)hwa_measure_allocate(
        &engine->work, fft_size, sizeof(*engine->window), 0);
    engine->fft = (HwaDspComplex *)hwa_measure_allocate(
        &engine->work, fft_size, sizeof(*engine->fft), 0);
    engine->power = (double *)hwa_measure_allocate(
        &engine->work, engine->spectrum_bins, sizeof(*engine->power), 0);
    engine->previous_magnitude = (double *)hwa_measure_allocate(
        &engine->work, engine->spectrum_bins,
        sizeof(*engine->previous_magnitude), 0);
    engine->block_mono = (double *)hwa_measure_allocate(
        &engine->work, engine->options->decode_block_frames,
        sizeof(*engine->block_mono), 0);
    engine->range_block = (double *)hwa_measure_allocate(
        &engine->work, block_size, sizeof(*engine->range_block), 0);
    engine->block_energy_prefix = (long double *)hwa_measure_allocate(
        &engine->work, block_size + 1U,
        sizeof(*engine->block_energy_prefix), 0);
    engine->peak_tree = (double *)hwa_measure_allocate(
        &engine->work, leaf_count * 2U, sizeof(*engine->peak_tree), 0);
    if (engine->sample_ring == NULL || engine->window == NULL ||
        engine->fft == NULL || engine->power == NULL ||
        engine->previous_magnitude == NULL || engine->block_mono == NULL ||
        engine->range_block == NULL ||
        engine->block_energy_prefix == NULL || engine->peak_tree == NULL) {
        hwa_set_error(error, error_size,
                      "measurement DSP buffers exceed the work limit");
        return -1;
    }
    if (hwa_dsp_hann(engine->window, fft_size) != HWA_DSP_OK) {
        hwa_set_error(error, error_size,
                      "could not create the measurement Hann window");
        return -1;
    }
    for (index = 0U; index < fft_size; ++index) {
        engine->window_energy += engine->window[index] * engine->window[index];
    }
    if (!(engine->window_energy > 0.0)) {
        hwa_set_error(error, error_size,
                      "measurement window has no energy");
        return -1;
    }
    return 0;
}

static double hwa_measure_peak_query(const HWAMeasureEngine *engine,
                                     size_t start,
                                     size_t end)
{
    size_t left = start + engine->peak_tree_leaf_count;
    size_t right = end + engine->peak_tree_leaf_count;
    double peak = 0.0;

    while (left < right) {
        if ((left & 1U) != 0U) {
            if (engine->peak_tree[left] > peak) peak = engine->peak_tree[left];
            left++;
        }
        if ((right & 1U) != 0U) {
            right--;
            if (engine->peak_tree[right] > peak) {
                peak = engine->peak_tree[right];
            }
        }
        left /= 2U;
        right /= 2U;
    }
    return peak;
}

static int hwa_measure_charge_evaluation(HWAMeasureEngine *engine,
                                         char *error,
                                         size_t error_size)
{
    if (engine->evaluations ==
        engine->options->max_item_frame_evaluations) {
        hwa_set_error(error, error_size,
                      "item-frame evaluation limit exceeded");
        return -1;
    }
    engine->evaluations++;
    return 0;
}

static int hwa_measure_accumulate_sample_block(HWAMeasureEngine *engine,
                                               const double *samples,
                                               size_t count,
                                               uint64_t block_start,
                                               char *error,
                                               size_t error_size)
{
    uint64_t block_end = block_start + (uint64_t)count;
    size_t index;

    engine->block_energy_prefix[0U] = 0.0L;
    for (index = 0U; index < count; ++index) {
        double sample = samples[index];
        if (!isfinite(sample)) {
            hwa_set_error(error, error_size,
                          "non-finite sample at frame %llu",
                          (unsigned long long)(block_start + index));
            return -1;
        }
        engine->block_energy_prefix[index + 1U] =
            engine->block_energy_prefix[index] +
            (long double)sample * (long double)sample;
        engine->peak_tree[engine->peak_tree_leaf_count + index] = fabs(sample);
    }
    for (; index < engine->peak_tree_leaf_count; ++index) {
        engine->peak_tree[engine->peak_tree_leaf_count + index] = 0.0;
    }
    for (index = engine->peak_tree_leaf_count; index-- > 1U;) {
        double left = engine->peak_tree[index * 2U];
        double right = engine->peak_tree[index * 2U + 1U];
        engine->peak_tree[index] = left > right ? left : right;
    }

    while (engine->sample_next_start < engine->items->item_count &&
           engine->sample_start_order[engine->sample_next_start].sample <
               block_end) {
        engine->sample_active_items[engine->sample_active_count++] =
            engine->sample_start_order[engine->sample_next_start].index;
        engine->sample_next_start++;
    }
    index = 0U;
    while (index < engine->sample_active_count) {
        size_t item_index = engine->sample_active_items[index];
        HWAMeasureItemAccumulator *accumulator =
            &engine->accumulators[item_index];
        const HWAItem *item = accumulator->item;
        uint64_t effective_start = item->start_sample;
        uint64_t effective_end = item->end_sample;
        uint64_t start;
        uint64_t end;

        if (item->kind == HWA_ITEM_ATTACK || item->kind == HWA_ITEM_NOTE) {
            uint64_t context = (uint64_t)engine->sample_rate / 20U;
            effective_start = item->start_sample > context
                                  ? item->start_sample - context
                                  : 0U;
        }
        if (effective_end <= block_start) {
            engine->sample_active_items[index] =
                engine->sample_active_items[--engine->sample_active_count];
            continue;
        }
        if (item->excluded != 0) {
            index++;
            continue;
        }
        if (hwa_measure_charge_evaluation(engine, error, error_size) != 0) {
            return -1;
        }
        start = item->start_sample > block_start
                    ? item->start_sample
                    : block_start;
        end = item->end_sample < block_end ? item->end_sample : block_end;
        if (start < end) {
            size_t local_start = (size_t)(start - block_start);
            size_t local_end = (size_t)(end - block_start);
            double peak = hwa_measure_peak_query(engine, local_start, local_end);

            accumulator->sample_energy +=
                engine->block_energy_prefix[local_end] -
                engine->block_energy_prefix[local_start];
            accumulator->sample_count += end - start;
            if (peak > accumulator->sample_peak) accumulator->sample_peak = peak;
            if (item->kind == HWA_ITEM_ATTACK && item->end_sample >
                    item->start_sample) {
                size_t bin;
                uint64_t span = item->end_sample - item->start_sample;

                for (bin = 0U; bin < HWA_MEASURE_ATTACK_SHAPE_BINS; ++bin) {
                    uint64_t bin_start = item->start_sample +
                        (span * (uint64_t)bin) /
                            HWA_MEASURE_ATTACK_SHAPE_BINS;
                    uint64_t bin_end = item->start_sample +
                        (span * (uint64_t)(bin + 1U)) /
                            HWA_MEASURE_ATTACK_SHAPE_BINS;
                    uint64_t part_start = bin_start > start ? bin_start : start;
                    uint64_t part_end = bin_end < end ? bin_end : end;
                    if (part_start < part_end) {
                        size_t first = (size_t)(part_start - block_start);
                        size_t last = (size_t)(part_end - block_start);
                        accumulator->attack_shape_energy[bin] +=
                            engine->block_energy_prefix[last] -
                            engine->block_energy_prefix[first];
                        accumulator->attack_shape_count[bin] +=
                            part_end - part_start;
                    }
                }
            }
        }
        start = effective_start > block_start ? effective_start : block_start;
        end = item->start_sample < block_end ? item->start_sample : block_end;
        if (start < end) {
            size_t local_start = (size_t)(start - block_start);
            size_t local_end = (size_t)(end - block_start);
            accumulator->context_energy +=
                engine->block_energy_prefix[local_end] -
                engine->block_energy_prefix[local_start];
            accumulator->context_count += end - start;
        }
        index++;
    }
    return 0;
}

static double hwa_measure_interpolated_power(const HWAMeasureEngine *engine,
                                             double bin)
{
    size_t lower;
    size_t upper;
    double fraction;

    if (!(bin >= 0.0) || bin >= (double)(engine->spectrum_bins - 1U)) {
        return 0.0;
    }
    lower = (size_t)floor(bin);
    upper = lower + 1U;
    fraction = bin - (double)lower;
    return engine->power[lower] * (1.0 - fraction) +
           engine->power[upper] * fraction;
}

static int hwa_measure_local_peak(const HWAMeasureEngine *engine,
                                  double predicted_hz,
                                  double *frequency,
                                  double *power,
                                  double *prominence)
{
    double bin_hz = (double)engine->sample_rate /
                    (double)engine->options->fft_size;
    double predicted_bin = predicted_hz / bin_hz;
    double cents_bins = predicted_bin * (pow(2.0, 35.0 / 1200.0) - 1.0);
    size_t radius = (size_t)ceil(cents_bins);
    size_t center;
    size_t start;
    size_t end;
    size_t best;
    size_t index;
    double neighborhood = 0.0;
    size_t neighborhood_count = 0U;
    double offset = 0.0;

    if (radius < 2U) radius = 2U;
    if (!(predicted_bin >= 1.0) ||
        predicted_bin >= (double)(engine->spectrum_bins - 1U)) {
        return 0;
    }
    center = (size_t)floor(predicted_bin + 0.5);
    start = center > radius ? center - radius : 1U;
    end = center + radius;
    if (end + 1U >= engine->spectrum_bins) end = engine->spectrum_bins - 2U;
    if (start > end) return 0;
    best = start;
    for (index = start; index <= end; ++index) {
        if (engine->power[index] > engine->power[best]) best = index;
    }
    if (best > 0U && best + 1U < engine->spectrum_bins) {
        double left = log(fmax(engine->power[best - 1U], 1.0e-30));
        double middle = log(fmax(engine->power[best], 1.0e-30));
        double right = log(fmax(engine->power[best + 1U], 1.0e-30));
        double denominator = left - 2.0 * middle + right;
        if (fabs(denominator) > DBL_EPSILON) {
            double candidate = 0.5 * (left - right) / denominator;
            if (candidate > -1.0 && candidate < 1.0) offset = candidate;
        }
    }
    for (index = start > 3U ? start - 3U : 1U;
         index <= end + 3U && index < engine->spectrum_bins; ++index) {
        if (index + 1U < best || index > best + 1U) {
            neighborhood += engine->power[index];
            neighborhood_count++;
        }
    }
    *frequency = ((double)best + offset) * bin_hz;
    *power = engine->power[best];
    *prominence = neighborhood_count != 0U
                      ? *power / fmax(neighborhood /
                                          (double)neighborhood_count,
                                      1.0e-30)
                      : 0.0;
    return 1;
}

static double hwa_measure_harmonic_score(const HWAMeasureEngine *engine,
                                         double fundamental)
{
    size_t partial;
    size_t count = engine->options->max_partials < 8U
                       ? engine->options->max_partials
                       : 8U;
    double score = 0.0;

    for (partial = 1U; partial <= count; ++partial) {
        double frequency = fundamental * (double)partial;
        double bin;
        if (frequency >= 0.98 * (double)engine->sample_rate * 0.5) break;
        bin = frequency * (double)engine->options->fft_size /
              (double)engine->sample_rate;
        score += hwa_measure_interpolated_power(engine, bin) /
                 sqrt((double)partial);
    }
    return score;
}

static int hwa_measure_pitch_frame(const HWAMeasureEngine *engine,
                                   double expected_hz,
                                   double total_power,
                                   const double band_power[HWA_BAND_COUNT],
                                   HWAMeasurePitchFrame *frame)
{
    double nyquist = (double)engine->sample_rate * 0.5;
    double weighted_frequency = 0.0;
    double frequency_weight = 0.0;
    double fundamental;
    double harmonic_power = 0.0;
    double residual;
    double score;
    double half_score;
    double double_score;
    size_t partial;
    size_t band;

    memset(frame, 0, sizeof(*frame));
    memcpy(frame->residual_band_power, band_power,
           sizeof(frame->residual_band_power));
    if (!(expected_hz > 0.0) || expected_hz >= nyquist * 0.95 ||
        (double)engine->options->fft_size * expected_hz /
                (double)engine->sample_rate <
            3.0 ||
        hwa_measure_power_db(total_power) <
            engine->options->spectral_floor_dbfs) {
        return 0;
    }

    for (partial = 1U;
         partial <= engine->options->max_partials; ++partial) {
        double predicted = expected_hz * (double)partial;
        double found_frequency;
        double peak_power;
        double prominence;
        if (predicted >= nyquist * 0.98) break;
        if (hwa_measure_local_peak(engine, predicted, &found_frequency,
                                   &peak_power, &prominence) != 0 &&
            prominence >= 2.5 &&
            hwa_measure_power_db(peak_power) >=
                engine->options->spectral_floor_dbfs) {
            double weight = peak_power / sqrt((double)partial);
            weighted_frequency += weight *
                                  (found_frequency / (double)partial);
            frequency_weight += weight;
        }
    }
    if (!(frequency_weight > 0.0)) return 0;
    fundamental = weighted_frequency / frequency_weight;
    if (!(fundamental > 0.0) ||
        fabs(1200.0 * log2(fundamental / expected_hz)) > 220.0) {
        return 0;
    }

    for (partial = 1U;
         partial <= engine->options->max_partials; ++partial) {
        double predicted = fundamental * (double)partial;
        double found_frequency;
        double peak_power;
        double prominence;
        double integrated = 0.0;
        double peak_bin;
        size_t center;
        size_t first;
        size_t last;
        size_t bin;

        if (predicted >= nyquist * 0.98) break;
        if (hwa_measure_local_peak(engine, predicted, &found_frequency,
                                   &peak_power, &prominence) == 0) {
            continue;
        }
        peak_bin = found_frequency * (double)engine->options->fft_size /
                   (double)engine->sample_rate;
        center = (size_t)floor(peak_bin + 0.5);
        first = center > 1U ? center - 1U : 1U;
        last = center + 1U;
        if (last >= engine->spectrum_bins) last = engine->spectrum_bins - 1U;
        for (bin = first; bin <= last; ++bin) integrated += engine->power[bin];
        if (prominence < 2.5 ||
            hwa_measure_power_db(integrated) <
                engine->options->spectral_floor_dbfs) {
            continue;
        }
        frame->partial_present[partial - 1U] = 1U;
        frame->partial_power[partial - 1U] = integrated;
        frame->partial_frequency_error[partial - 1U] =
            1200.0 * log2(found_frequency / predicted);
        harmonic_power += integrated;
        band = hwa_measure_band_for_frequency(found_frequency);
        frame->residual_band_power[band] =
            fmax(0.0, frame->residual_band_power[band] - integrated);
    }
    if (!(harmonic_power > 0.0)) return 0;
    if (harmonic_power > total_power) harmonic_power = total_power;
    residual = fmax(total_power - harmonic_power, 1.0e-30);
    frame->pitch_hz = fundamental;
    frame->harmonic_power = harmonic_power;
    frame->residual_power = residual;
    frame->confidence = harmonic_power / fmax(total_power, 1.0e-30);
    frame->hnr_db = 10.0 * log10(harmonic_power / residual);
    score = hwa_measure_harmonic_score(engine, expected_hz);
    half_score = expected_hz * 0.5 >= 20.0
                     ? hwa_measure_harmonic_score(engine, expected_hz * 0.5)
                     : 0.0;
    double_score = expected_hz * 2.0 < nyquist * 0.95
                       ? hwa_measure_harmonic_score(engine, expected_hz * 2.0)
                       : 0.0;
    frame->octave_fault = (half_score > score * 1.35 ||
                           double_score > score * 1.35);
    frame->valid = frame->confidence >=
                   engine->options->pitch_confidence_floor;
    return frame->valid;
}

static int hwa_measure_transition_pitch_frame(
    const HWAMeasureEngine *engine,
    HWAMeasureItemAccumulator *accumulator,
    double guide_hz,
    double total_power,
    const double band_power[HWA_BAND_COUNT],
    HWAMeasurePitchFrame *frame)
{
    HWAMeasurePitchFrame candidate;
    double references[6];
    double direction = accumulator->transition_to_midi >=
                               accumulator->transition_from_midi
                           ? 1.0
                           : -1.0;
    double best_score = -DBL_MAX;
    size_t reference_count = 0U;
    size_t index;
    int found = 0;

    memset(frame, 0, sizeof(*frame));
    if (accumulator->transition_tracking_valid != 0) {
        references[reference_count++] = accumulator->transition_tracking_hz;
        references[reference_count++] =
            accumulator->transition_tracking_hz *
            pow(2.0, direction * 70.0 / 1200.0);
        references[reference_count++] =
            accumulator->transition_tracking_hz *
            pow(2.0, direction * 140.0 / 1200.0);
    }
    references[reference_count++] = guide_hz;
    references[reference_count++] =
        hwa_measure_midi_hz(accumulator->transition_from_midi);
    references[reference_count++] =
        hwa_measure_midi_hz(accumulator->transition_to_midi);

    for (index = 0U; index < reference_count; ++index) {
        size_t prior;
        int duplicate = 0;
        double score;

        for (prior = 0U; prior < index; ++prior) {
            if (fabs(1200.0 * log2(references[index] / references[prior])) <
                5.0) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate != 0 ||
            hwa_measure_pitch_frame(engine, references[index], total_power,
                                    band_power, &candidate) == 0) {
            continue;
        }
        score = candidate.confidence;
        if (accumulator->transition_tracking_valid != 0) {
            double distance = fabs(1200.0 * log2(
                candidate.pitch_hz / accumulator->transition_tracking_hz));
            score *= exp(-distance / 400.0);
        }
        if (!found || score > best_score) {
            *frame = candidate;
            best_score = score;
            found = 1;
        }
    }
    if (found != 0) {
        accumulator->transition_tracking_hz = frame->pitch_hz;
        accumulator->transition_tracking_valid = 1;
    }
    return found;
}

static void hwa_measure_add_frame_to_item(
    HWAMeasureEngine *engine,
    size_t item_index,
    double absolute_time,
    double level_db,
    double centroid_hz,
    double flatness,
    double flux,
    const double band_power[HWA_BAND_COUNT],
    const HWAMeasurePitchFrame *pitch,
    double pitch_reference_hz)
{
    HWAMeasureItemAccumulator *accumulator =
        &engine->accumulators[item_index];
    double item_start = (double)accumulator->item->start_sample /
                        (double)engine->sample_rate;
    double time = absolute_time - item_start;
    size_t band;
    size_t partial;
    HWAMeasureSeriesPoint *series_point = NULL;

    hwa_measure_running_push(&accumulator->level, time, level_db);
    hwa_measure_running_push(&accumulator->centroid, time, centroid_hz);
    hwa_measure_running_push(&accumulator->flatness, time, flatness);
    for (band = 0U; band < HWA_BAND_COUNT; ++band) {
        hwa_measure_running_push(&accumulator->band[band], time,
                                 hwa_measure_power_db(band_power[band]));
    }
    if (flux >= 0.20) accumulator->transient_count++;
    if (accumulator->have_prior_frame != 0 &&
        fabs(level_db - accumulator->prior_level) <= 0.35 &&
        fabs(centroid_hz - accumulator->prior_centroid) <= 20.0) {
        accumulator->fixed_state_count++;
    }
    accumulator->prior_level = level_db;
    accumulator->prior_centroid = centroid_hz;
    accumulator->have_prior_frame = 1;
    accumulator->frame_count++;

    if (accumulator->series_capacity != 0U) {
        size_t seen = accumulator->series_seen++;
        if (seen % accumulator->series_stride == 0U &&
            accumulator->series_count < accumulator->series_capacity) {
            series_point = &engine->series[
                accumulator->series_offset + accumulator->series_count++];
            series_point->time_seconds = time;
            series_point->level_db = level_db;
            series_point->centroid_hz = centroid_hz;
        }
    }

    if (pitch == NULL || pitch->valid == 0) return;
    {
        double cents = 1200.0 * log2(
            pitch->pitch_hz / pitch_reference_hz);
        hwa_measure_running_push(&accumulator->pitch_hz, time,
                                 pitch->pitch_hz);
        hwa_measure_running_push(&accumulator->pitch_cents, time, cents);
        hwa_measure_running_push(&accumulator->harmonic_level, time,
                                 hwa_measure_power_db(
                                     pitch->harmonic_power));
        hwa_measure_running_push(&accumulator->residual_level, time,
                                 hwa_measure_power_db(
                                     pitch->residual_power));
        hwa_measure_running_push(&accumulator->hnr, time, pitch->hnr_db);
        if (accumulator->have_noise_peak == 0 ||
            hwa_measure_power_db(pitch->residual_power) >
                accumulator->noise_peak_level) {
            accumulator->noise_peak_level =
                hwa_measure_power_db(pitch->residual_power);
            accumulator->noise_peak_time = time;
            accumulator->have_noise_peak = 1;
        }
        for (band = 0U; band < HWA_BAND_COUNT; ++band) {
            hwa_measure_running_push(
                &accumulator->residual_band[band], time,
                hwa_measure_power_db(pitch->residual_band_power[band]));
        }
        for (partial = 0U; partial < engine->options->max_partials;
             ++partial) {
            HWAMeasurePartialAccumulator *part =
                &accumulator->partial[partial];
            if (pitch->partial_present[partial] == 0U) continue;
            hwa_measure_running_push(&part->frequency_error, time,
                                     pitch->partial_frequency_error[partial]);
            hwa_measure_running_push(&part->level, time,
                                     hwa_measure_power_db(
                                         pitch->partial_power[partial]));
            part->present_count++;
            if (part->born == 0) {
                part->birth_seconds = time;
                part->born = 1;
            }
            part->loss_seconds = time;
        }
        accumulator->pitch_frame_count++;
        accumulator->pitch_confidence_sum += pitch->confidence;
        if (pitch->octave_fault != 0) accumulator->octave_fault_count++;
        if (series_point != NULL) {
            series_point->pitch_cents =
                accumulator->item->kind == HWA_ITEM_TRANSITION
                    ? 1200.0 * log2(pitch->pitch_hz / 440.0)
                    : cents;
            series_point->pitch_valid = 1;
        }
    }
}

static int hwa_measure_process_frame(HWAMeasureEngine *engine,
                                     uint64_t frame_start,
                                     char *error,
                                     size_t error_size)
{
    HWAMeasurePitchFrame pitch_cache[128];
    unsigned char pitch_cache_state[128] = {0U};
    double band_power[HWA_BAND_COUNT] = {0.0};
    double normalization = 1.0 /
        ((double)engine->options->fft_size * engine->window_energy);
    double total_power = 0.0;
    double centroid = 0.0;
    double log_sum = 0.0;
    double arithmetic_sum = 0.0;
    double flatness = 0.0;
    double flux_numerator = 0.0;
    double flux_denominator = 0.0;
    double level_db;
    double center_time;
    uint64_t center_sample = frame_start +
        (uint64_t)engine->options->fft_size / 2U;
    size_t index;

    if (engine->transform_count == engine->options->max_transforms) {
        hwa_set_error(error, error_size,
                      "measurement transform limit exceeded");
        return -1;
    }
    for (index = 0U; index < engine->options->fft_size; ++index) {
        uint64_t absolute = frame_start + (uint64_t)index;
        double sample = absolute < engine->pushed_frames
                            ? engine->sample_ring[
                                  absolute % engine->options->fft_size]
                            : 0.0;
        engine->fft[index].real = sample * engine->window[index];
        engine->fft[index].imag = 0.0;
    }
    if (hwa_dsp_fft(engine->fft, engine->options->fft_size, 0) != HWA_DSP_OK) {
        hwa_set_error(error, error_size,
                      "measurement FFT failed at sample %llu",
                      (unsigned long long)frame_start);
        return -1;
    }
    engine->transform_count++;
    for (index = 0U; index < engine->spectrum_bins; ++index) {
        double real = engine->fft[index].real;
        double imag = engine->fft[index].imag;
        double factor = (index == 0U ||
                         index + 1U == engine->spectrum_bins)
                            ? normalization
                            : 2.0 * normalization;
        double power = (real * real + imag * imag) * factor;
        double frequency = (double)index * (double)engine->sample_rate /
                           (double)engine->options->fft_size;
        double magnitude = sqrt(fmax(power, 0.0));
        size_t band = hwa_measure_band_for_frequency(frequency);

        engine->power[index] = power;
        total_power += power;
        centroid += frequency * power;
        band_power[band] += power;
        if (index != 0U) {
            log_sum += log(fmax(power, 1.0e-30));
            arithmetic_sum += power;
        }
        if (engine->have_previous_spectrum != 0) {
            double change = magnitude - engine->previous_magnitude[index];
            if (change > 0.0) flux_numerator += change;
            flux_denominator += engine->previous_magnitude[index];
        }
        engine->previous_magnitude[index] = magnitude;
    }
    engine->have_previous_spectrum = 1;
    if (total_power > 0.0) centroid /= total_power;
    if (arithmetic_sum > 0.0 && engine->spectrum_bins > 1U) {
        size_t count = engine->spectrum_bins - 1U;
        flatness = exp(log_sum / (double)count) /
                   (arithmetic_sum / (double)count);
    }
    level_db = hwa_measure_power_db(total_power);
    center_time = (double)center_sample / (double)engine->sample_rate;

    while (engine->next_start < engine->items->item_count &&
           engine->start_order[engine->next_start].sample <= center_sample) {
        size_t item_index = engine->start_order[engine->next_start].index;
        if (engine->items->items[item_index].end_sample > center_sample) {
            engine->active_items[engine->active_count++] = item_index;
        }
        engine->next_start++;
    }
    index = 0U;
    while (index < engine->active_count) {
        size_t item_index = engine->active_items[index];
        HWAMeasureItemAccumulator *accumulator =
            &engine->accumulators[item_index];
        HWAMeasurePitchFrame *pitch = NULL;
        HWAMeasurePitchFrame transition_pitch;
        double pitch_reference = accumulator->expected_pitch_hz;

        if (accumulator->item->end_sample <= center_sample) {
            engine->active_items[index] =
                engine->active_items[--engine->active_count];
            continue;
        }
        if (accumulator->item->excluded != 0) {
            index++;
            continue;
        }
        if (hwa_measure_charge_evaluation(engine, error, error_size) != 0) {
            return -1;
        }
        if (accumulator->transition_target_valid != 0) {
            double progress = accumulator->item->end_sample >
                                      accumulator->item->start_sample
                                  ? ((double)center_sample -
                                     (double)accumulator->item->start_sample) /
                                        ((double)accumulator->item->end_sample -
                                         (double)accumulator->item->start_sample)
                                  : 0.0;
            double from_hz = hwa_measure_midi_hz(
                accumulator->transition_from_midi);
            double to_hz = hwa_measure_midi_hz(
                accumulator->transition_to_midi);
            progress = hwa_measure_clamp(progress, 0.0, 1.0);
            pitch_reference = from_hz * pow(to_hz / from_hz, progress);
            (void)hwa_measure_transition_pitch_frame(
                engine, accumulator, pitch_reference, total_power,
                band_power, &transition_pitch);
            pitch = &transition_pitch;
        } else if (accumulator->target_status == HWA_MEASURE_STATUS_VALID) {
            int midi = accumulator->midi_note;
            if (pitch_cache_state[midi] == 0U) {
                pitch_cache_state[midi] = 1U;
                (void)hwa_measure_pitch_frame(
                    engine, accumulator->expected_pitch_hz, total_power,
                    band_power, &pitch_cache[midi]);
            }
            pitch = &pitch_cache[midi];
        }
        hwa_measure_add_frame_to_item(
            engine, item_index, center_time, level_db, centroid, flatness,
            flux_denominator > 0.0
                ? flux_numerator / flux_denominator
                : 0.0,
            band_power, pitch, pitch_reference);
        index++;
    }
    return 0;
}

static int hwa_measure_double_compare(const void *left, const void *right)
{
    double first = *(const double *)left;
    double second = *(const double *)right;
    return first < second ? -1 : first > second ? 1 : 0;
}

static int hwa_measure_emit(HWAMeasureEngine *engine,
                            const HWAMeasureItemAccumulator *accumulator,
                            HWAMeasureKind kind,
                            uint32_t index,
                            HWAMeasureUnit unit,
                            HWAMeasureStatus status,
                            double value,
                            double confidence,
                            uint32_t evidence,
                            uint32_t quality,
                            char *error,
                            size_t error_size)
{
    return hwa_measure_append_observation(
        engine, accumulator->item->id, kind, index, unit,
        HWA_MEASURE_VIEW_RAW, status, value, confidence, evidence, quality,
        error, error_size);
}

static HWAMeasureStatus hwa_measure_pitch_dependent_status(
    HWAMeasureStatus pitch_status,
    int have_value,
    HWAMeasureStatus missing_status)
{
    if (pitch_status != HWA_MEASURE_STATUS_VALID) return pitch_status;
    return have_value != 0 ? HWA_MEASURE_STATUS_VALID : missing_status;
}

static int hwa_measure_emit_level(HWAMeasureEngine *engine,
                                  const HWAMeasureItemAccumulator *accumulator,
                                  HWAMeasureKind kind,
                                  uint32_t index,
                                  HWAMeasureStatus status,
                                  double value,
                                  double confidence,
                                  uint32_t evidence,
                                  uint32_t quality,
                                  char *error,
                                  size_t error_size)
{
    HWAMeasureStatus relative_status = status;
    double relative_value = 0.0;

    if (hwa_measure_append_observation(
            engine, accumulator->item->id, kind, index,
            HWA_MEASURE_UNIT_DBFS, HWA_MEASURE_VIEW_RAW, status, value,
            confidence, evidence, quality, error, error_size) != 0) {
        return -1;
    }
    if (status == HWA_MEASURE_STATUS_VALID) {
        if (engine->result->level_reference_valid != 0) {
            relative_value = value - engine->result->level_reference_dbfs;
            evidence |= HWA_MEASURE_EVIDENCE_LEVEL_REFERENCE;
        } else {
            relative_status = HWA_MEASURE_STATUS_NO_REFERENCE;
        }
    }
    return hwa_measure_append_observation(
        engine, accumulator->item->id, kind, index, HWA_MEASURE_UNIT_DB,
        HWA_MEASURE_VIEW_LEVEL_RELATIVE, relative_status, relative_value,
        confidence, evidence, quality, error, error_size);
}

static double hwa_measure_item_rms(
    const HWAMeasureItemAccumulator *accumulator)
{
    return accumulator->sample_count != 0U
               ? sqrt((double)(accumulator->sample_energy /
                               (long double)accumulator->sample_count))
               : 0.0;
}

static int hwa_measure_make_level_reference(HWAMeasureEngine *engine,
                                            char *error,
                                            size_t error_size)
{
    double *values;
    size_t count = 0U;
    size_t index;

    values = (double *)hwa_measure_allocate(
        &engine->work, engine->items->item_count, sizeof(*values), 0);
    if (engine->items->item_count != 0U && values == NULL) {
        hwa_set_error(error, error_size,
                      "level-reference work exceeds the measurement limit");
        return -1;
    }
    for (index = 0U; index < engine->items->item_count; ++index) {
        const HWAMeasureItemAccumulator *accumulator =
            &engine->accumulators[index];
        double rms;
        if (accumulator->item->excluded != 0 ||
            accumulator->item->kind != HWA_ITEM_BODY ||
            accumulator->sample_count == 0U ||
            (accumulator->item->quality_flags &
             HWA_ITEM_QUALITY_LOW_CONFIDENCE) != 0U) {
            continue;
        }
        rms = hwa_measure_item_rms(accumulator);
        if (hwa_measure_amplitude_db(rms) <
            engine->options->spectral_floor_dbfs) {
            continue;
        }
        values[count++] = hwa_measure_amplitude_db(rms);
    }
    if (count != 0U) {
        qsort(values, count, sizeof(*values), hwa_measure_double_compare);
        engine->result->level_reference_dbfs =
            count % 2U != 0U
                ? values[count / 2U]
                : 0.5 * (values[count / 2U - 1U] + values[count / 2U]);
        engine->result->level_reference_item_count = count;
        engine->result->level_reference_valid = 1;
    }
    hwa_measure_release(&engine->work, values, engine->items->item_count,
                        sizeof(*values), 0);
    return 0;
}

typedef struct HWAMeasureVibratoFacts {
    double delay;
    double rate;
    double depth;
    double waveform_residual;
    double rate_drift;
    double depth_drift;
    double level_correlation;
    double tone_correlation;
    double confidence;
    HWAMeasureStatus drift_status;
    HWAMeasureStatus level_correlation_status;
    HWAMeasureStatus tone_correlation_status;
    int valid;
} HWAMeasureVibratoFacts;

static int hwa_measure_series_rate(const HWAMeasureSeriesPoint *points,
                                   size_t count,
                                   size_t first,
                                   size_t end,
                                   double *rate,
                                   double *depth,
                                   double *correlation)
{
    double values[HWA_MEASURE_SERIES_PER_ITEM];
    double mean_time = 0.0;
    double mean_pitch = 0.0;
    double slope_numerator = 0.0;
    double slope_denominator = 0.0;
    double dt;
    size_t length = end > first ? end - first : 0U;
    size_t index;
    size_t min_lag;
    size_t max_lag;
    size_t best_lag = 0U;
    double best = -1.0;
    double variance = 0.0;
    double minimum_dt = DBL_MAX;
    double maximum_dt = 0.0;

    if (end > count || length < 8U ||
        length > HWA_MEASURE_SERIES_PER_ITEM) return 0;
    dt = (points[end - 1U].time_seconds - points[first].time_seconds) /
         (double)(length - 1U);
    if (!(dt > 0.0)) return 0;
    for (index = first; index < end; ++index) {
        if (points[index].pitch_valid == 0) return 0;
        if (index > first) {
            double step = points[index].time_seconds -
                          points[index - 1U].time_seconds;
            if (!(step > 0.0)) return 0;
            if (step < minimum_dt) minimum_dt = step;
            if (step > maximum_dt) maximum_dt = step;
        }
    }
    if (minimum_dt == DBL_MAX || maximum_dt > minimum_dt * 1.05) return 0;
    for (index = first; index < end; ++index) {
        mean_time += points[index].time_seconds;
        mean_pitch += points[index].pitch_cents;
    }
    mean_time /= (double)length;
    mean_pitch /= (double)length;
    for (index = first; index < end; ++index) {
        double time = points[index].time_seconds - mean_time;
        double pitch = points[index].pitch_cents - mean_pitch;
        slope_numerator += time * pitch;
        slope_denominator += time * time;
    }
    for (index = 0U; index < length; ++index) {
        double time = points[first + index].time_seconds - mean_time;
        values[index] = points[first + index].pitch_cents - mean_pitch -
                        (slope_denominator > 0.0
                             ? slope_numerator / slope_denominator * time
                             : 0.0);
        variance += values[index] * values[index];
    }
    if (!(variance > 0.0)) return 0;
    min_lag = (size_t)floor(1.0 / (9.0 * dt));
    max_lag = (size_t)ceil(1.0 / (3.0 * dt));
    if (min_lag < 1U) min_lag = 1U;
    if (max_lag >= length / 2U) max_lag = length / 2U;
    for (index = min_lag; index <= max_lag; ++index) {
        double numerator = 0.0;
        double first_power = 0.0;
        double second_power = 0.0;
        size_t point;
        for (point = 0U; point + index < length; ++point) {
            numerator += values[point] * values[point + index];
            first_power += values[point] * values[point];
            second_power += values[point + index] * values[point + index];
        }
        if (first_power > 0.0 && second_power > 0.0) {
            double candidate = numerator / sqrt(first_power * second_power);
            if (candidate > best) {
                best = candidate;
                best_lag = index;
            }
        }
    }
    if (best_lag == 0U || best < 0.25) return 0;
    *rate = 1.0 / ((double)best_lag * dt);
    *depth = sqrt(2.0 * variance / (double)length);
    *correlation = best;
    return isfinite(*rate) && isfinite(*depth);
}

static HWAMeasureStatus hwa_measure_series_correlation(
    const HWAMeasureSeriesPoint *points,
    size_t count,
    int tone,
    double *correlation)
{
    double mean_x = 0.0;
    double mean_y = 0.0;
    double xx = 0.0;
    double yy = 0.0;
    double xy = 0.0;
    size_t index;

    *correlation = 0.0;
    if (count < 3U) return HWA_MEASURE_STATUS_TOO_SHORT;
    for (index = 0U; index < count; ++index) {
        if (points[index].pitch_valid == 0) {
            return HWA_MEASURE_STATUS_NO_PITCH;
        }
        mean_x += points[index].pitch_cents;
        mean_y += tone != 0 ? points[index].centroid_hz
                            : points[index].level_db;
    }
    mean_x /= (double)count;
    mean_y /= (double)count;
    for (index = 0U; index < count; ++index) {
        double x = points[index].pitch_cents - mean_x;
        double y = (tone != 0 ? points[index].centroid_hz
                              : points[index].level_db) - mean_y;
        xx += x * x;
        yy += y * y;
        xy += x * y;
    }
    if (!(xx > 0.0) || !(yy > 0.0)) return HWA_MEASURE_STATUS_NO_DATA;
    *correlation = hwa_measure_clamp(xy / sqrt(xx * yy), -1.0, 1.0);
    return HWA_MEASURE_STATUS_VALID;
}

static void hwa_measure_vibrato(const HWAMeasureEngine *engine,
                                const HWAMeasureItemAccumulator *accumulator,
                                HWAMeasureVibratoFacts *facts)
{
    const HWAMeasureSeriesPoint *points =
        engine->series + accumulator->series_offset;
    size_t count = accumulator->series_count;
    size_t half = count / 2U;
    double first_rate = 0.0;
    double first_depth = 0.0;
    double first_confidence = 0.0;
    double second_rate = 0.0;
    double second_depth = 0.0;
    double second_confidence = 0.0;
    size_t index;

    memset(facts, 0, sizeof(*facts));
    facts->drift_status = HWA_MEASURE_STATUS_TOO_SHORT;
    facts->level_correlation_status = hwa_measure_series_correlation(
        points, count, 0, &facts->level_correlation);
    facts->tone_correlation_status = hwa_measure_series_correlation(
        points, count, 1, &facts->tone_correlation);
    if (count < 12U ||
        points[count - 1U].time_seconds - points[0U].time_seconds < 0.35 ||
        !hwa_measure_series_rate(points, count, 0U, count, &facts->rate,
                                 &facts->depth, &facts->confidence)) {
        return;
    }
    facts->delay = points[0U].time_seconds;
    {
        double mean_time = 0.0;
        double mean_pitch = 0.0;
        double numerator = 0.0;
        double denominator = 0.0;
        double dt = (points[count - 1U].time_seconds -
                     points[0U].time_seconds) /
                    (double)(count - 1U);
        size_t window = (size_t)floor(1.0 / (facts->rate * dt) + 0.5);
        int found = 0;
        if (window < 4U) window = 4U;
        if (window > count) window = count;
        for (index = 0U; index < count; ++index) {
            mean_time += points[index].time_seconds;
            mean_pitch += points[index].pitch_cents;
        }
        mean_time /= (double)count;
        mean_pitch /= (double)count;
        for (index = 0U; index < count; ++index) {
            double x = points[index].time_seconds - mean_time;
            numerator += x * (points[index].pitch_cents - mean_pitch);
            denominator += x * x;
        }
        for (index = 0U; index + window <= count; ++index) {
            double energy = 0.0;
            size_t sign_changes = 0U;
            size_t point;
            double prior = 0.0;
            for (point = index; point < index + window; ++point) {
                double residual = points[point].pitch_cents - mean_pitch -
                    (denominator > 0.0
                         ? numerator / denominator *
                               (points[point].time_seconds - mean_time)
                         : 0.0);
                energy += residual * residual;
                if (point > index &&
                    ((residual > 0.0 && prior < 0.0) ||
                     (residual < 0.0 && prior > 0.0))) {
                    sign_changes++;
                }
                prior = residual;
            }
            if (sqrt(energy / (double)window) >=
                    fmax(2.0, facts->depth * 0.30) &&
                sign_changes != 0U) {
                facts->delay = points[index].time_seconds;
                found = 1;
                break;
            }
        }
        if (!found) facts->delay = points[0U].time_seconds;
    }
    facts->waveform_residual = 1.0 - hwa_measure_clamp(
        facts->confidence, 0.0, 1.0);
    if (half >= 6U && count - half >= 6U &&
        hwa_measure_series_rate(points, count, 0U, half, &first_rate,
                                &first_depth, &first_confidence) &&
        hwa_measure_series_rate(points, count, half, count, &second_rate,
                                &second_depth, &second_confidence)) {
        double duration = points[count - 1U].time_seconds -
                          points[0U].time_seconds;
        if (duration > 0.0) {
            facts->rate_drift = (second_rate - first_rate) / duration;
            facts->depth_drift = (second_depth - first_depth) / duration;
            facts->drift_status = HWA_MEASURE_STATUS_VALID;
        }
    }
    facts->valid = 1;
}

static double hwa_measure_attack_similarity(
    const HWAMeasureItemAccumulator *first,
    const HWAMeasureItemAccumulator *second,
    int *valid)
{
    double dot = 0.0;
    double first_power = 0.0;
    double second_power = 0.0;
    size_t bin;

    *valid = 0;
    for (bin = 0U; bin < HWA_MEASURE_ATTACK_SHAPE_BINS; ++bin) {
        double left = first->attack_shape_count[bin] != 0U
                          ? sqrt((double)(first->attack_shape_energy[bin] /
                                          (long double)
                                              first->attack_shape_count[bin]))
                          : 0.0;
        double right = second->attack_shape_count[bin] != 0U
                           ? sqrt((double)(second->attack_shape_energy[bin] /
                                           (long double)second->
                                               attack_shape_count[bin]))
                           : 0.0;
        dot += left * right;
        first_power += left * left;
        second_power += right * right;
    }
    if (first_power > 0.0 && second_power > 0.0) {
        *valid = 1;
        return hwa_measure_clamp(dot / sqrt(first_power * second_power),
                                 0.0, 1.0);
    }
    return 0.0;
}

static double hwa_measure_pitch_similarity(
    const HWAMeasureEngine *engine,
    const HWAMeasureItemAccumulator *first,
    const HWAMeasureItemAccumulator *second,
    int *valid)
{
    const HWAMeasureSeriesPoint *left =
        engine->series + first->series_offset;
    const HWAMeasureSeriesPoint *right =
        engine->series + second->series_offset;
    size_t count = first->series_count < second->series_count
                       ? first->series_count
                       : second->series_count;
    double mean_left = 0.0;
    double mean_right = 0.0;
    double dot = 0.0;
    double left_power = 0.0;
    double right_power = 0.0;
    size_t index;

    *valid = 0;
    if (count < 4U) return 0.0;
    if (count > 64U) count = 64U;
    for (index = 0U; index < count; ++index) {
        size_t left_index = count == 1U
                                ? 0U
                                : index * (first->series_count - 1U) /
                                      (count - 1U);
        size_t right_index = count == 1U
                                 ? 0U
                                 : index * (second->series_count - 1U) /
                                       (count - 1U);
        if (left[left_index].pitch_valid == 0 ||
            right[right_index].pitch_valid == 0) {
            return 0.0;
        }
        mean_left += left[left_index].pitch_cents;
        mean_right += right[right_index].pitch_cents;
    }
    mean_left /= (double)count;
    mean_right /= (double)count;
    for (index = 0U; index < count; ++index) {
        size_t left_index = index * (first->series_count - 1U) /
                            (count - 1U);
        size_t right_index = index * (second->series_count - 1U) /
                             (count - 1U);
        double x = left[left_index].pitch_cents - mean_left;
        double y = right[right_index].pitch_cents - mean_right;
        dot += x * y;
        left_power += x * x;
        right_power += y * y;
    }
    if (left_power > 0.0 && right_power > 0.0) {
        *valid = 1;
        return hwa_measure_clamp(dot / sqrt(left_power * right_power),
                                 -1.0, 1.0);
    }
    return 0.0;
}

static int hwa_measure_is_accent(const HWATypedLabels *labels)
{
    const char *text = labels->articulation;
    return text != NULL &&
           (strcmp(text, "accent") == 0 || strcmp(text, "marcato") == 0 ||
            strcmp(text, "sfz") == 0 || strcmp(text, "sforzando") == 0);
}

static int hwa_measure_prepare_relations(HWAMeasureEngine *engine,
                                         char *error,
                                         size_t error_size)
{
    size_t *attack_by_note;
    size_t *note_history;
    size_t note_count = 0U;
    size_t index;

    attack_by_note = (size_t *)hwa_measure_allocate(
        &engine->work, engine->items->item_count, sizeof(*attack_by_note), 0);
    note_history = (size_t *)hwa_measure_allocate(
        &engine->work, engine->items->item_count, sizeof(*note_history), 0);
    if (engine->items->item_count != 0U &&
        (attack_by_note == NULL || note_history == NULL)) {
        hwa_measure_release(&engine->work, attack_by_note,
                            engine->items->item_count,
                            sizeof(*attack_by_note), 0);
        hwa_measure_release(&engine->work, note_history,
                            engine->items->item_count,
                            sizeof(*note_history), 0);
        hwa_set_error(error, error_size,
                      "repeat-relation work exceeds the measurement limit");
        return -1;
    }
    for (index = 0U; index < engine->items->item_count; ++index) {
        attack_by_note[index] = SIZE_MAX;
    }
    for (index = 0U; index < engine->items->item_count; ++index) {
        const HWAItem *item = &engine->items->items[index];
        if (item->kind == HWA_ITEM_ATTACK && item->parent_valid != 0 &&
            item->parent_id != 0U &&
            item->parent_id <= (uint64_t)engine->items->item_count) {
            attack_by_note[item->parent_id - 1U] = index;
        }
    }
    for (index = 0U; index < engine->items->item_count; ++index) {
        size_t item_index = engine->start_order[index].index;
        HWAMeasureItemAccumulator *current =
            &engine->accumulators[item_index];
        const char *part;
        double current_db;
        size_t prior_part = SIZE_MAX;
        size_t prior_repeat = SIZE_MAX;
        size_t history;

        if (current->item->kind != HWA_ITEM_NOTE ||
            current->item->excluded != 0 || current->sample_count == 0U) {
            continue;
        }
        part = engine->result->contexts[item_index].labels.part;
        current_db = hwa_measure_amplitude_db(hwa_measure_item_rms(current));
        if (part != NULL && part[0] != '\0') {
            for (history = note_count; history-- > 0U;) {
                size_t prior_index = note_history[history];
                HWAMeasureItemAccumulator *prior =
                    &engine->accumulators[prior_index];
                const char *prior_part_name =
                    engine->result->contexts[prior_index].labels.part;
                if (prior_part_name == NULL ||
                    strcmp(part, prior_part_name) != 0) {
                    continue;
                }
                if (hwa_measure_amplitude_db(hwa_measure_item_rms(prior)) <
                    engine->options->spectral_floor_dbfs) {
                    continue;
                }
                if (prior_part == SIZE_MAX) prior_part = prior_index;
                if (current->target_status == HWA_MEASURE_STATUS_VALID &&
                    prior->target_status == HWA_MEASURE_STATUS_VALID &&
                    current->midi_note ==
                        prior->midi_note) {
                    prior_repeat = prior_index;
                    break;
                }
            }
        }
        if (prior_part != SIZE_MAX) {
            HWAMeasureItemAccumulator *previous =
                &engine->accumulators[prior_part];
            double previous_db = hwa_measure_amplitude_db(
                hwa_measure_item_rms(previous));
            if (current_db >= engine->options->spectral_floor_dbfs &&
                previous_db >= engine->options->spectral_floor_dbfs) {
                current->local_contrast = current_db - previous_db;
                current->local_contrast_valid = 1;
                if (hwa_measure_is_accent(
                        &engine->result->contexts[item_index].labels)) {
                    current->accent_size = current->local_contrast;
                    current->accent_size_valid = 1;
                }
            }
        }
        if (prior_repeat != SIZE_MAX) {
            size_t previous_index = prior_repeat;
            HWAMeasureItemAccumulator *previous =
                &engine->accumulators[previous_index];
            size_t current_attack = attack_by_note[item_index];
            size_t previous_attack = attack_by_note[previous_index];

            if (current_attack != SIZE_MAX && previous_attack != SIZE_MAX) {
                current->repeated_attack_similarity =
                    hwa_measure_attack_similarity(
                        &engine->accumulators[previous_attack],
                        &engine->accumulators[current_attack],
                        &current->repeated_attack_valid);
            }
            current->repeated_pitch_similarity =
                hwa_measure_pitch_similarity(
                    engine, previous, current,
                    &current->repeated_pitch_valid);
        }
        note_history[note_count++] = item_index;
    }
    hwa_measure_release(&engine->work, attack_by_note,
                        engine->items->item_count, sizeof(*attack_by_note), 0);
    hwa_measure_release(&engine->work, note_history,
                        engine->items->item_count, sizeof(*note_history), 0);
    return 0;
}

static const HWAItemEvent *hwa_measure_first_source_event(
    const HWAItemSet *items,
    uint64_t item_id)
{
    size_t index;
    for (index = 0U; index < items->member_count; ++index) {
        if (items->members[index].item_id == item_id &&
            items->members[index].role == HWA_ITEM_MEMBER_SOURCE) {
            return hwa_measure_event(items, items->members[index].event_id);
        }
    }
    return NULL;
}

static int hwa_measure_transition_events(const HWAItemSet *items,
                                         uint64_t item_id,
                                         const HWAItemEvent **from,
                                         const HWAItemEvent **to)
{
    size_t index;
    *from = NULL;
    *to = NULL;
    for (index = 0U; index < items->member_count; ++index) {
        const HWAItemMember *member = &items->members[index];
        if (member->item_id != item_id) continue;
        if (member->role == HWA_ITEM_MEMBER_FROM && *from == NULL) {
            *from = hwa_measure_event(items, member->event_id);
        } else if (member->role == HWA_ITEM_MEMBER_TO && *to == NULL) {
            *to = hwa_measure_event(items, member->event_id);
        }
    }
    return *from != NULL && *to != NULL;
}

typedef struct HWAMeasureAttackFacts {
    double rise10;
    double rise50;
    double rise90;
    double slope;
    double overshoot;
    int valid;
} HWAMeasureAttackFacts;

typedef struct HWAMeasureTransitionFacts {
    double glide_time;
    double linearity;
    double pitch_change;
    double confidence;
    HWAMeasureStatus pitch_status;
    HWAMeasureStatus glide_status;
} HWAMeasureTransitionFacts;

static void hwa_measure_transition_facts(
    const HWAMeasureEngine *engine,
    const HWAMeasureItemAccumulator *accumulator,
    HWAMeasureTransitionFacts *facts)
{
    const HWAMeasureSeriesPoint *points =
        engine->series + accumulator->series_offset;
    size_t count = accumulator->series_count;
    size_t valid_count = 0U;
    size_t first_valid = SIZE_MAX;
    size_t last_valid = SIZE_MAX;
    size_t first_count = 0U;
    size_t last_count = 0U;
    double first_sum = 0.0;
    double last_sum = 0.0;
    double from_cents;
    double to_cents;
    double expected_change;
    size_t index;
    size_t crossing10 = SIZE_MAX;
    size_t crossing90 = SIZE_MAX;

    memset(facts, 0, sizeof(*facts));
    facts->pitch_status =
        accumulator->target_status == HWA_MEASURE_STATUS_MULTI_PITCH
            ? HWA_MEASURE_STATUS_MULTI_PITCH
            : HWA_MEASURE_STATUS_NO_PITCH;
    facts->glide_status = facts->pitch_status;
    if (accumulator->transition_target_valid == 0) return;
    if (accumulator->transition_from_midi ==
        accumulator->transition_to_midi) {
        facts->glide_status = HWA_MEASURE_STATUS_UNSUPPORTED_ITEM;
    }
    if (count < 5U) {
        facts->pitch_status = HWA_MEASURE_STATUS_TOO_SHORT;
        if (facts->glide_status != HWA_MEASURE_STATUS_UNSUPPORTED_ITEM) {
            facts->glide_status = HWA_MEASURE_STATUS_TOO_SHORT;
        }
        return;
    }
    for (index = 0U; index < count; ++index) {
        if (points[index].pitch_valid == 0) continue;
        if (first_valid == SIZE_MAX) first_valid = index;
        last_valid = index;
        valid_count++;
        if (first_count < 3U) {
            first_sum += points[index].pitch_cents;
            first_count++;
        }
    }
    for (index = count; index-- > 0U && last_count < 3U;) {
        if (points[index].pitch_valid == 0) continue;
        last_sum += points[index].pitch_cents;
        last_count++;
    }
    if (valid_count < 5U || valid_count * 10U < count * 7U ||
        first_valid > count / 4U || last_valid < (count * 3U) / 4U ||
        first_count == 0U || last_count == 0U) {
        return;
    }
    from_cents =
        100.0 * ((double)accumulator->transition_from_midi - 69.0);
    to_cents = 100.0 * ((double)accumulator->transition_to_midi - 69.0);
    expected_change = to_cents - from_cents;
    first_sum /= (double)first_count;
    last_sum /= (double)last_count;
    if (fabs(first_sum - from_cents) > 120.0 ||
        fabs(last_sum - to_cents) > 120.0) {
        return;
    }
    facts->pitch_change = last_sum - first_sum;
    facts->pitch_status = HWA_MEASURE_STATUS_VALID;
    facts->confidence =
        accumulator->pitch_frame_count != 0U
            ? hwa_measure_clamp(
                  (double)(accumulator->pitch_confidence_sum /
                           (long double)accumulator->pitch_frame_count) *
                      (double)valid_count / (double)count,
                  0.0, 1.0)
            : 0.0;
    if (fabs(expected_change) < 25.0) {
        facts->glide_status = HWA_MEASURE_STATUS_UNSUPPORTED_ITEM;
        return;
    }

    for (index = 0U; index < count; ++index) {
        double progress;
        if (points[index].pitch_valid == 0) continue;
        progress = (points[index].pitch_cents - from_cents) /
                   expected_change;
        if (crossing10 == SIZE_MAX && progress >= 0.10) {
            crossing10 = index;
        }
        if (crossing10 != SIZE_MAX && index > crossing10 &&
            progress >= 0.90) {
            crossing90 = index;
            break;
        }
    }
    if (crossing10 != SIZE_MAX && crossing90 != SIZE_MAX) {
        double mean_time = 0.0;
        double mean_pitch = 0.0;
        double time_power = 0.0;
        double pitch_power = 0.0;
        double covariance = 0.0;
        size_t path_count = 0U;

        for (index = crossing10; index <= crossing90; ++index) {
            if (points[index].pitch_valid == 0) continue;
            mean_time += points[index].time_seconds;
            mean_pitch += points[index].pitch_cents;
            path_count++;
        }
        if (path_count < 3U) return;
        mean_time /= (double)path_count;
        mean_pitch /= (double)path_count;
        for (index = crossing10; index <= crossing90; ++index) {
            double time;
            double pitch;
            if (points[index].pitch_valid == 0) continue;
            time = points[index].time_seconds - mean_time;
            pitch = points[index].pitch_cents - mean_pitch;
            time_power += time * time;
            pitch_power += pitch * pitch;
            covariance += time * pitch;
        }
        if (!(time_power > 0.0) || !(pitch_power > 0.0)) return;
        facts->glide_time = points[crossing90].time_seconds -
                            points[crossing10].time_seconds;
        if (!(facts->glide_time > 0.0)) return;
        facts->linearity = hwa_measure_clamp(
            covariance * covariance / (time_power * pitch_power),
            0.0, 1.0);
        facts->glide_status = HWA_MEASURE_STATUS_VALID;
    }
}

static void hwa_measure_attack_facts(
    const HWAMeasureItemAccumulator *accumulator,
    uint32_t sample_rate,
    HWAMeasureAttackFacts *facts)
{
    double levels[HWA_MEASURE_ATTACK_SHAPE_BINS];
    double maximum = -DBL_MAX;
    double tail = 0.0;
    uint64_t span = accumulator->item->end_sample -
                    accumulator->item->start_sample;
    size_t valid_count = 0U;
    size_t bin;
    int found10 = 0;
    int found50 = 0;
    int found90 = 0;

    memset(facts, 0, sizeof(*facts));
    if (span == 0U) return;
    for (bin = 0U; bin < HWA_MEASURE_ATTACK_SHAPE_BINS; ++bin) {
        if (accumulator->attack_shape_count[bin] == 0U) {
            levels[bin] = HWA_MEASURE_DB_FLOOR;
            continue;
        }
        levels[bin] = hwa_measure_power_db(
            (double)(accumulator->attack_shape_energy[bin] /
                     (long double)accumulator->attack_shape_count[bin]));
        if (levels[bin] > maximum) maximum = levels[bin];
        valid_count++;
    }
    if (valid_count < 4U || maximum <= HWA_MEASURE_DB_FLOOR) return;
    for (bin = HWA_MEASURE_ATTACK_SHAPE_BINS - 4U;
         bin < HWA_MEASURE_ATTACK_SHAPE_BINS; ++bin) {
        tail += levels[bin];
    }
    tail /= 4.0;
    for (bin = 0U; bin < HWA_MEASURE_ATTACK_SHAPE_BINS; ++bin) {
        double seconds = ((double)bin + 0.5) * (double)span /
                         ((double)HWA_MEASURE_ATTACK_SHAPE_BINS *
                          (double)sample_rate);
        double relative_amplitude = pow(10.0, (levels[bin] - maximum) / 20.0);
        if (!found10 && relative_amplitude >= 0.10) {
            facts->rise10 = seconds;
            found10 = 1;
        }
        if (!found50 && relative_amplitude >= 0.50) {
            facts->rise50 = seconds;
            found50 = 1;
        }
        if (!found90 && relative_amplitude >= 0.90) {
            facts->rise90 = seconds;
            found90 = 1;
        }
    }
    if (found10 && found90 && facts->rise90 > facts->rise10) {
        facts->slope = 20.0 * log10(0.9 / 0.1) /
                       (facts->rise90 - facts->rise10);
    }
    facts->overshoot = maximum - tail;
    facts->valid = found10 && found50 && found90;
}

static double hwa_measure_pitch_settle(
    const HWAMeasureEngine *engine,
    const HWAMeasureItemAccumulator *accumulator,
    int *valid)
{
    const HWAMeasureSeriesPoint *points =
        engine->series + accumulator->series_offset;
    double center = 0.0;
    size_t index;

    *valid = 0;
    if (accumulator->series_count < 3U ||
        !hwa_measure_running_mean(&accumulator->pitch_cents, &center)) {
        return 0.0;
    }
    for (index = 0U; index + 2U < accumulator->series_count; ++index) {
        if (points[index].pitch_valid != 0 &&
            points[index + 1U].pitch_valid != 0 &&
            points[index + 2U].pitch_valid != 0 &&
            fabs(points[index].pitch_cents - center) <= 15.0 &&
            fabs(points[index + 1U].pitch_cents - center) <= 15.0 &&
            fabs(points[index + 2U].pitch_cents - center) <= 15.0) {
            *valid = 1;
            return points[index].time_seconds;
        }
    }
    return 0.0;
}

static int hwa_measure_finalize_item(HWAMeasureEngine *engine,
                                     size_t item_index,
                                     char *error,
                                     size_t error_size)
{
    const HWAMeasureItemAccumulator *accumulator =
        &engine->accumulators[item_index];
    const HWAItem *item = accumulator->item;
    double duration = (double)(item->end_sample - item->start_sample) /
                      (double)engine->sample_rate;
    double rms = hwa_measure_item_rms(accumulator);
    double rms_db = hwa_measure_amplitude_db(rms);
    double confidence = hwa_measure_clamp(item->confidence, 0.0, 1.0);
    uint32_t sample_evidence = HWA_MEASURE_EVIDENCE_SAMPLES |
                               HWA_MEASURE_EVIDENCE_ITEM_BOUNDS;
    uint32_t spectrum_evidence = HWA_MEASURE_EVIDENCE_SPECTRUM |
                                 HWA_MEASURE_EVIDENCE_ITEM_BOUNDS;
    uint32_t pitch_evidence = spectrum_evidence |
                              HWA_MEASURE_EVIDENCE_PITCH |
                              HWA_MEASURE_EVIDENCE_TARGET_PITCH |
                              HWA_MEASURE_EVIDENCE_MEMBERS;
    uint32_t quality = hwa_measure_item_quality(item);
    HWAMeasureStatus sample_status;
    HWAMeasureStatus frame_status;
    HWAMeasureStatus pitch_status;
    double value = 0.0;
    double spread = 0.0;
    double slope = 0.0;
    HWAMeasureTransitionFacts transition_facts;
    size_t band;
    size_t partial;

#define HWA_EMIT(call)                                                        \
    do {                                                                      \
        if ((call) != 0) return -1;                                           \
    } while (0)

    if (item->excluded != 0) return 0;
    memset(&transition_facts, 0, sizeof(transition_facts));
    if (item->kind == HWA_ITEM_TRANSITION) {
        hwa_measure_transition_facts(engine, accumulator,
                                     &transition_facts);
    }
    sample_status = accumulator->sample_count == 0U
                        ? (duration == 0.0 ? HWA_MEASURE_STATUS_EMPTY_SPAN
                                           : HWA_MEASURE_STATUS_NO_DATA)
                        : (rms_db < engine->options->spectral_floor_dbfs
                               ? HWA_MEASURE_STATUS_BELOW_FLOOR
                               : HWA_MEASURE_STATUS_VALID);
    frame_status = sample_status != HWA_MEASURE_STATUS_VALID
                       ? sample_status
                       : (accumulator->frame_count == 0U
                              ? HWA_MEASURE_STATUS_TOO_SHORT
                              : HWA_MEASURE_STATUS_VALID);
    if (sample_status != HWA_MEASURE_STATUS_VALID) {
        pitch_status = sample_status;
    } else if (accumulator->frame_count < 3U) {
        pitch_status = HWA_MEASURE_STATUS_TOO_SHORT;
    } else if (accumulator->target_status != HWA_MEASURE_STATUS_VALID) {
        pitch_status = (HWAMeasureStatus)accumulator->target_status;
    } else if (accumulator->pitch_frame_count < 3U) {
        pitch_status = HWA_MEASURE_STATUS_NO_PITCH;
    } else {
        pitch_status = HWA_MEASURE_STATUS_VALID;
    }
    if (accumulator->frame_count != 0U &&
        accumulator->pitch_frame_count * 10U <
            accumulator->frame_count * 7U) {
        quality |= HWA_MEASURE_QUALITY_INCOMPLETE_COVERAGE;
    }

    HWA_EMIT(hwa_measure_emit_level(
        engine, accumulator, HWA_MEASURE_RMS_DBFS, 0U, sample_status,
        rms_db, confidence, sample_evidence, quality, error, error_size));
    HWA_EMIT(hwa_measure_emit_level(
        engine, accumulator, HWA_MEASURE_PEAK_DBFS, 0U, sample_status,
        hwa_measure_amplitude_db(accumulator->sample_peak), confidence,
        sample_evidence, quality, error, error_size));
    HWA_EMIT(hwa_measure_emit(
        engine, accumulator, HWA_MEASURE_CREST_DB, 0U,
        HWA_MEASURE_UNIT_DB, sample_status,
        sample_status == HWA_MEASURE_STATUS_VALID
            ? hwa_measure_amplitude_db(accumulator->sample_peak) - rms_db
            : 0.0,
        confidence, sample_evidence, quality, error, error_size));

    for (band = 0U; band < HWA_BAND_COUNT; ++band) {
        int have = hwa_measure_running_mean(&accumulator->band[band], &value);
        HWAMeasureStatus status = hwa_measure_pitch_dependent_status(
            frame_status, have, HWA_MEASURE_STATUS_TOO_SHORT);
        HWA_EMIT(hwa_measure_emit_level(
            engine, accumulator, HWA_MEASURE_BAND_LEVEL_DBFS,
            (uint32_t)band, status, have ? value : 0.0, confidence,
            spectrum_evidence,
            quality, error, error_size));
    }
    for (band = 0U; band < HWA_BAND_COUNT; ++band) {
        int have = hwa_measure_running_mean(&accumulator->band[band], &value);
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_BAND_BALANCE_DB,
            (uint32_t)band, HWA_MEASURE_UNIT_DB,
            hwa_measure_pitch_dependent_status(
                frame_status, have, HWA_MEASURE_STATUS_TOO_SHORT),
            have ? value - rms_db : 0.0, confidence, spectrum_evidence,
            quality, error, error_size));
    }
    {
        int have_centroid = hwa_measure_running_mean(
            &accumulator->centroid, &value);
        double centroid_mean = have_centroid ? value : 0.0;
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_CENTROID_HZ, 0U,
            HWA_MEASURE_UNIT_HZ,
            hwa_measure_pitch_dependent_status(
                frame_status, have_centroid, HWA_MEASURE_STATUS_TOO_SHORT),
            centroid_mean, confidence, spectrum_evidence, quality, error,
            error_size));
    }
    {
        int have_flatness = hwa_measure_running_mean(
            &accumulator->flatness, &value);
        double flatness_mean = have_flatness ? value : 0.0;
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_FLATNESS, 0U,
            HWA_MEASURE_UNIT_RATIO,
            hwa_measure_pitch_dependent_status(
                frame_status, have_flatness, HWA_MEASURE_STATUS_TOO_SHORT),
            flatness_mean, confidence, spectrum_evidence, quality, error,
            error_size));
    }
    {
        int have_level_slope = hwa_measure_running_slope(
            &accumulator->level, &slope);
        double level_slope = have_level_slope ? slope : 0.0;
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_LEVEL_SLOPE_DB_PER_SECOND, 0U,
            HWA_MEASURE_UNIT_DB_PER_SECOND,
            hwa_measure_pitch_dependent_status(
                frame_status, have_level_slope,
                HWA_MEASURE_STATUS_TOO_SHORT),
            level_slope, confidence, spectrum_evidence, quality, error,
            error_size));
    }
    {
        int have_centroid_slope = hwa_measure_running_slope(
            &accumulator->centroid, &slope);
        double centroid_slope = have_centroid_slope ? slope : 0.0;
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_CENTROID_SLOPE_HZ_PER_SECOND,
            0U, HWA_MEASURE_UNIT_HZ_PER_SECOND,
            hwa_measure_pitch_dependent_status(
                frame_status, have_centroid_slope,
                HWA_MEASURE_STATUS_TOO_SHORT),
            centroid_slope, confidence, spectrum_evidence, quality, error,
            error_size));
    }
    for (band = 0U; band < HWA_BAND_COUNT; ++band) {
        int have = hwa_measure_running_slope(&accumulator->band[band], &slope);
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_BAND_SLOPE_DB_PER_SECOND,
            (uint32_t)band, HWA_MEASURE_UNIT_DB_PER_SECOND,
            hwa_measure_pitch_dependent_status(
                frame_status, have, HWA_MEASURE_STATUS_TOO_SHORT),
            have ? slope : 0.0,
            confidence, spectrum_evidence, quality, error, error_size));
    }
    HWA_EMIT(hwa_measure_emit(
        engine, accumulator, HWA_MEASURE_TRANSIENT_RATE_HZ, 0U,
        HWA_MEASURE_UNIT_HZ,
        hwa_measure_pitch_dependent_status(
            frame_status,
            duration > 0.0 && accumulator->frame_count != 0U,
            HWA_MEASURE_STATUS_TOO_SHORT),
        duration > 0.0 ? (double)accumulator->transient_count / duration : 0.0,
        confidence, spectrum_evidence, quality, error, error_size));
    HWA_EMIT(hwa_measure_emit(
        engine, accumulator, HWA_MEASURE_FIXED_STATE_FRACTION, 0U,
        HWA_MEASURE_UNIT_RATIO,
        hwa_measure_pitch_dependent_status(
            frame_status, accumulator->frame_count > 1U,
            HWA_MEASURE_STATUS_TOO_SHORT),
        accumulator->frame_count > 1U
            ? (double)accumulator->fixed_state_count /
                  (double)(accumulator->frame_count - 1U)
            : 0.0,
        confidence, spectrum_evidence, quality, error, error_size));
    {
        int have_level_spread = hwa_measure_running_spread(
            &accumulator->level, &spread);
        double level_spread = have_level_spread ? spread : 0.0;
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_LEVEL_MODULATION_SPREAD_DB,
            0U, HWA_MEASURE_UNIT_DB,
            hwa_measure_pitch_dependent_status(
                frame_status, have_level_spread,
                HWA_MEASURE_STATUS_TOO_SHORT),
            level_spread, confidence, spectrum_evidence, quality, error,
            error_size));
    }
    {
        int have_centroid_spread = hwa_measure_running_spread(
            &accumulator->centroid, &spread);
        double centroid_spread = have_centroid_spread ? spread : 0.0;
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator,
            HWA_MEASURE_CENTROID_MODULATION_SPREAD_HZ, 0U,
            HWA_MEASURE_UNIT_HZ,
            hwa_measure_pitch_dependent_status(
                frame_status, have_centroid_spread,
                HWA_MEASURE_STATUS_TOO_SHORT),
            centroid_spread, confidence, spectrum_evidence, quality, error,
            error_size));
    }
    for (band = 0U; band < HWA_BAND_COUNT; ++band) {
        int have = hwa_measure_running_spread(&accumulator->band[band],
                                              &spread);
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_BAND_MODULATION_SPREAD_DB,
            (uint32_t)band, HWA_MEASURE_UNIT_DB,
            hwa_measure_pitch_dependent_status(
                frame_status, have, HWA_MEASURE_STATUS_TOO_SHORT),
            have ? spread : 0.0,
            confidence, spectrum_evidence, quality, error, error_size));
    }

    {
        double pitch_confidence = accumulator->pitch_frame_count != 0U
            ? confidence * (double)(accumulator->pitch_confidence_sum /
                                    (long double)
                                        accumulator->pitch_frame_count)
            : 0.0;
        int settle_valid = 0;
        double settle = hwa_measure_pitch_settle(engine, accumulator,
                                                 &settle_valid);
        double mean_cents = 0.0;
        (void)hwa_measure_running_mean(&accumulator->pitch_cents, &mean_cents);
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_PITCH_HZ, 0U,
            HWA_MEASURE_UNIT_HZ, pitch_status,
            hwa_measure_running_mean(&accumulator->pitch_hz, &value)
                ? value
                : 0.0,
            pitch_confidence, pitch_evidence, quality, error, error_size));
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_TUNING_OFFSET_CENTS, 0U,
            HWA_MEASURE_UNIT_CENTS, pitch_status, mean_cents,
            pitch_confidence, pitch_evidence, quality, error, error_size));
        {
            int have_pitch_spread = hwa_measure_running_spread(
                &accumulator->pitch_cents, &spread);
            double pitch_spread = have_pitch_spread ? spread : 0.0;
            HWA_EMIT(hwa_measure_emit(
                engine, accumulator, HWA_MEASURE_PITCH_SPREAD_CENTS, 0U,
                HWA_MEASURE_UNIT_CENTS,
                hwa_measure_pitch_dependent_status(
                    pitch_status, have_pitch_spread,
                    HWA_MEASURE_STATUS_TOO_SHORT),
                pitch_spread, pitch_confidence, pitch_evidence, quality,
                error, error_size));
        }
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_PITCH_OVERSHOOT_CENTS, 0U,
            HWA_MEASURE_UNIT_CENTS, pitch_status,
            accumulator->pitch_cents.count != 0U
                ? accumulator->pitch_cents.maximum - mean_cents
                : 0.0,
            pitch_confidence, pitch_evidence, quality, error, error_size));
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_PITCH_UNDERSHOOT_CENTS, 0U,
            HWA_MEASURE_UNIT_CENTS, pitch_status,
            accumulator->pitch_cents.count != 0U
                ? mean_cents - accumulator->pitch_cents.minimum
                : 0.0,
            pitch_confidence, pitch_evidence, quality, error, error_size));
        {
            int have_pitch_slope = hwa_measure_running_slope(
                &accumulator->pitch_cents, &slope);
            double pitch_slope = have_pitch_slope ? slope : 0.0;
            HWA_EMIT(hwa_measure_emit(
                engine, accumulator,
                HWA_MEASURE_PITCH_DRIFT_CENTS_PER_SECOND, 0U,
                HWA_MEASURE_UNIT_CENTS_PER_SECOND,
                hwa_measure_pitch_dependent_status(
                    pitch_status, have_pitch_slope,
                    HWA_MEASURE_STATUS_TOO_SHORT),
                pitch_slope, pitch_confidence, pitch_evidence, quality,
                error, error_size));
        }
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_PITCH_SETTLE_SECONDS, 0U,
            HWA_MEASURE_UNIT_SECONDS,
            item->kind == HWA_ITEM_TRANSITION
                ? HWA_MEASURE_STATUS_UNSUPPORTED_ITEM
                : hwa_measure_pitch_dependent_status(
                      pitch_status, settle_valid,
                      HWA_MEASURE_STATUS_TOO_SHORT),
            settle, pitch_confidence, pitch_evidence, quality, error,
            error_size));
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_OCTAVE_FAULT_FRACTION, 0U,
            HWA_MEASURE_UNIT_RATIO, pitch_status,
            accumulator->pitch_frame_count != 0U
                ? (double)accumulator->octave_fault_count /
                      (double)accumulator->pitch_frame_count
                : 0.0,
            pitch_confidence, pitch_evidence, quality, error, error_size));
    }

    if (item->kind == HWA_ITEM_TRANSITION) {
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_GLIDE_TIME_SECONDS, 0U,
            HWA_MEASURE_UNIT_SECONDS,
            sample_status != HWA_MEASURE_STATUS_VALID
                ? sample_status
                : transition_facts.glide_status,
            transition_facts.glide_time,
            confidence * transition_facts.confidence,
            pitch_evidence,
            quality, error, error_size));
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_PORTAMENTO_LINEARITY, 0U,
            HWA_MEASURE_UNIT_RATIO,
            sample_status != HWA_MEASURE_STATUS_VALID
                ? sample_status
                : transition_facts.glide_status,
            transition_facts.linearity,
            confidence * transition_facts.confidence,
            pitch_evidence, quality, error, error_size));
    }

    {
        double inharmonicity_sum = 0.0;
        double inharmonicity_weight = 0.0;
        double odd_power = 0.0;
        double even_power = 0.0;
        double harmonic_weight = 0.0;
        double harmonic_index_sum = 0.0;
        for (partial = 0U; partial < engine->options->max_partials;
             ++partial) {
            double partial_level;
            if (hwa_measure_running_mean(
                    &accumulator->partial[partial].frequency_error,
                    &value) && partial != 0U) {
                double ratio = pow(2.0, value / 1200.0);
                double order = (double)(partial + 1U);
                inharmonicity_sum += 2.0 * (ratio - 1.0) /
                                     (order * order);
                inharmonicity_weight += 1.0;
            }
            if (hwa_measure_running_mean(
                    &accumulator->partial[partial].level,
                    &partial_level)) {
                double power = pow(10.0, partial_level / 10.0);
                if ((partial & 1U) == 0U) odd_power += power;
                else even_power += power;
                harmonic_weight += power;
                harmonic_index_sum += power * (double)(partial + 1U);
            }
        }
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_INHARMONICITY_B, 0U,
            HWA_MEASURE_UNIT_RATIO,
            hwa_measure_pitch_dependent_status(
                pitch_status, inharmonicity_weight > 0.0,
                HWA_MEASURE_STATUS_NO_DATA),
            inharmonicity_weight > 0.0
                ? inharmonicity_sum / inharmonicity_weight
                : 0.0,
            confidence, pitch_evidence | HWA_MEASURE_EVIDENCE_HARMONICS,
            quality, error, error_size));
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_ODD_EVEN_BALANCE_DB, 0U,
            HWA_MEASURE_UNIT_DB,
            hwa_measure_pitch_dependent_status(
                pitch_status, odd_power > 0.0 && even_power > 0.0,
                HWA_MEASURE_STATUS_NO_DATA),
            odd_power > 0.0 && even_power > 0.0
                ? 10.0 * log10(odd_power / even_power)
                : 0.0,
            confidence, pitch_evidence | HWA_MEASURE_EVIDENCE_HARMONICS,
            quality, error, error_size));
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_HARMONIC_CENTROID, 0U,
            HWA_MEASURE_UNIT_HARMONIC_INDEX,
            hwa_measure_pitch_dependent_status(
                pitch_status, harmonic_weight > 0.0,
                HWA_MEASURE_STATUS_NO_DATA),
            harmonic_weight > 0.0
                ? harmonic_index_sum / harmonic_weight
                : 0.0,
            confidence, pitch_evidence | HWA_MEASURE_EVIDENCE_HARMONICS,
            quality, error, error_size));
    }
    {
        int have_harmonic_level = hwa_measure_running_mean(
            &accumulator->harmonic_level, &value);
        double harmonic_level = have_harmonic_level ? value : 0.0;
        HWA_EMIT(hwa_measure_emit_level(
            engine, accumulator, HWA_MEASURE_HARMONIC_LEVEL_DBFS, 0U,
            hwa_measure_pitch_dependent_status(
                pitch_status, have_harmonic_level,
                HWA_MEASURE_STATUS_NO_DATA),
            harmonic_level, confidence,
            pitch_evidence | HWA_MEASURE_EVIDENCE_HARMONICS, quality, error,
            error_size));
    }
    {
        int have_residual_level = hwa_measure_running_mean(
            &accumulator->residual_level, &value);
        double residual_level = have_residual_level ? value : 0.0;
        HWA_EMIT(hwa_measure_emit_level(
            engine, accumulator, HWA_MEASURE_RESIDUAL_LEVEL_DBFS, 0U,
            hwa_measure_pitch_dependent_status(
                pitch_status, have_residual_level,
                HWA_MEASURE_STATUS_NO_DATA),
            residual_level, confidence,
            pitch_evidence | HWA_MEASURE_EVIDENCE_RESIDUAL, quality, error,
            error_size));
    }
    {
        int have_hnr = hwa_measure_running_mean(&accumulator->hnr, &value);
        double hnr = have_hnr ? value : 0.0;
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_HNR_DB, 0U,
            HWA_MEASURE_UNIT_DB,
            hwa_measure_pitch_dependent_status(
                pitch_status, have_hnr, HWA_MEASURE_STATUS_NO_DATA), hnr,
            confidence, pitch_evidence | HWA_MEASURE_EVIDENCE_HARMONICS |
                            HWA_MEASURE_EVIDENCE_RESIDUAL,
            quality, error, error_size));
    }

    for (partial = 0U; partial < engine->options->max_partials; ++partial) {
        const HWAMeasurePartialAccumulator *part =
            &accumulator->partial[partial];
        int have = hwa_measure_running_mean(&part->frequency_error, &value);
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_PARTIAL_FREQUENCY_ERROR_CENTS,
            (uint32_t)partial + 1U, HWA_MEASURE_UNIT_CENTS,
            hwa_measure_pitch_dependent_status(
                pitch_status, have, HWA_MEASURE_STATUS_NO_DATA),
            have ? value : 0.0,
            confidence, pitch_evidence | HWA_MEASURE_EVIDENCE_HARMONICS,
            quality, error, error_size));
    }
    for (partial = 0U; partial < engine->options->max_partials; ++partial) {
        const HWAMeasurePartialAccumulator *part =
            &accumulator->partial[partial];
        int have = hwa_measure_running_mean(&part->level, &value);
        HWA_EMIT(hwa_measure_emit_level(
            engine, accumulator, HWA_MEASURE_PARTIAL_LEVEL_DBFS,
            (uint32_t)partial + 1U,
            hwa_measure_pitch_dependent_status(
                pitch_status, have, HWA_MEASURE_STATUS_NO_DATA),
            have ? value : 0.0,
            confidence, pitch_evidence | HWA_MEASURE_EVIDENCE_HARMONICS,
            quality, error, error_size));
    }
    for (partial = 0U; partial < engine->options->max_partials; ++partial) {
        const HWAMeasurePartialAccumulator *part =
            &accumulator->partial[partial];
        int have = hwa_measure_running_mean(&part->level, &value);
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_PARTIAL_BALANCE_DB,
            (uint32_t)partial + 1U, HWA_MEASURE_UNIT_DB,
            hwa_measure_pitch_dependent_status(
                pitch_status,
                have && sample_status == HWA_MEASURE_STATUS_VALID,
                HWA_MEASURE_STATUS_NO_DATA),
            have ? value - rms_db : 0.0, confidence,
            pitch_evidence | HWA_MEASURE_EVIDENCE_HARMONICS, quality, error,
            error_size));
    }
    for (partial = 0U; partial < engine->options->max_partials; ++partial) {
        const HWAMeasurePartialAccumulator *part =
            &accumulator->partial[partial];
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_PARTIAL_PRESENCE_FRACTION,
            (uint32_t)partial + 1U, HWA_MEASURE_UNIT_RATIO,
            hwa_measure_pitch_dependent_status(
                pitch_status, accumulator->pitch_frame_count != 0U,
                HWA_MEASURE_STATUS_NO_DATA),
            accumulator->pitch_frame_count != 0U
                ? (double)part->present_count /
                      (double)accumulator->pitch_frame_count
                : 0.0,
            confidence, pitch_evidence | HWA_MEASURE_EVIDENCE_HARMONICS,
            quality, error, error_size));
    }
    for (partial = 0U; partial < engine->options->max_partials; ++partial) {
        const HWAMeasurePartialAccumulator *part =
            &accumulator->partial[partial];
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_PARTIAL_BIRTH_SECONDS,
            (uint32_t)partial + 1U, HWA_MEASURE_UNIT_SECONDS,
            hwa_measure_pitch_dependent_status(
                pitch_status, part->born, HWA_MEASURE_STATUS_NO_DATA),
            part->birth_seconds, confidence,
            pitch_evidence | HWA_MEASURE_EVIDENCE_HARMONICS, quality, error,
            error_size));
    }
    for (partial = 0U; partial < engine->options->max_partials; ++partial) {
        const HWAMeasurePartialAccumulator *part =
            &accumulator->partial[partial];
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_PARTIAL_LOSS_SECONDS,
            (uint32_t)partial + 1U, HWA_MEASURE_UNIT_SECONDS,
            hwa_measure_pitch_dependent_status(
                pitch_status, part->born, HWA_MEASURE_STATUS_NO_DATA),
            part->loss_seconds, confidence,
            pitch_evidence | HWA_MEASURE_EVIDENCE_HARMONICS, quality, error,
            error_size));
    }
    for (partial = 0U; partial < engine->options->max_partials; ++partial) {
        const HWAMeasurePartialAccumulator *part =
            &accumulator->partial[partial];
        int have = hwa_measure_running_slope(&part->level, &slope);
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator,
            HWA_MEASURE_PARTIAL_LEVEL_SLOPE_DB_PER_SECOND,
            (uint32_t)partial + 1U, HWA_MEASURE_UNIT_DB_PER_SECOND,
            hwa_measure_pitch_dependent_status(
                pitch_status, have, HWA_MEASURE_STATUS_TOO_SHORT),
            have ? slope : 0.0,
            confidence, pitch_evidence | HWA_MEASURE_EVIDENCE_HARMONICS,
            quality, error, error_size));
    }
    {
        size_t born_count = 0U;
        double minimum_birth = DBL_MAX;
        double maximum_birth = -DBL_MAX;
        for (partial = 0U; partial < engine->options->max_partials;
             ++partial) {
            if (accumulator->partial[partial].born != 0) {
                born_count++;
                if (accumulator->partial[partial].birth_seconds < minimum_birth) {
                    minimum_birth = accumulator->partial[partial].birth_seconds;
                }
                if (accumulator->partial[partial].birth_seconds > maximum_birth) {
                    maximum_birth = accumulator->partial[partial].birth_seconds;
                }
            }
        }
        for (partial = 0U; partial < engine->options->max_partials;
             ++partial) {
            size_t order = 1U;
            size_t other;
            const HWAMeasurePartialAccumulator *part =
                &accumulator->partial[partial];
            if (part->born != 0) {
                for (other = 0U; other < engine->options->max_partials;
                     ++other) {
                    if (accumulator->partial[other].born != 0 &&
                        (accumulator->partial[other].birth_seconds <
                             part->birth_seconds ||
                         (accumulator->partial[other].birth_seconds ==
                              part->birth_seconds &&
                          other < partial))) {
                        order++;
                    }
                }
            }
            HWA_EMIT(hwa_measure_emit(
                engine, accumulator, HWA_MEASURE_PARTIAL_ONSET_ORDER,
                (uint32_t)partial + 1U, HWA_MEASURE_UNIT_ORDER,
                hwa_measure_pitch_dependent_status(
                    pitch_status, part->born, HWA_MEASURE_STATUS_NO_DATA),
                part->born != 0 ? (double)order : 0.0, confidence,
                pitch_evidence | HWA_MEASURE_EVIDENCE_HARMONICS, quality,
                error, error_size));
        }
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_PARTIAL_ONSET_SPREAD_SECONDS,
            0U, HWA_MEASURE_UNIT_SECONDS,
            hwa_measure_pitch_dependent_status(
                pitch_status, born_count >= 2U,
                HWA_MEASURE_STATUS_NO_DATA),
            born_count >= 2U ? maximum_birth - minimum_birth : 0.0,
            confidence, pitch_evidence | HWA_MEASURE_EVIDENCE_HARMONICS,
            quality, error, error_size));
    }
    for (band = 0U; band < HWA_BAND_COUNT; ++band) {
        int have = hwa_measure_running_mean(&accumulator->residual_band[band],
                                            &value);
        HWA_EMIT(hwa_measure_emit_level(
            engine, accumulator, HWA_MEASURE_RESIDUAL_BAND_LEVEL_DBFS,
            (uint32_t)band,
            hwa_measure_pitch_dependent_status(
                pitch_status, have, HWA_MEASURE_STATUS_NO_DATA),
            have ? value : 0.0,
            confidence, pitch_evidence | HWA_MEASURE_EVIDENCE_RESIDUAL,
            quality, error, error_size));
    }
    for (band = 0U; band < HWA_BAND_COUNT; ++band) {
        int have = hwa_measure_running_mean(&accumulator->residual_band[band],
                                            &value);
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_RESIDUAL_BAND_BALANCE_DB,
            (uint32_t)band, HWA_MEASURE_UNIT_DB,
            hwa_measure_pitch_dependent_status(
                pitch_status,
                have && sample_status == HWA_MEASURE_STATUS_VALID,
                HWA_MEASURE_STATUS_NO_DATA),
            have ? value - rms_db : 0.0, confidence,
            pitch_evidence | HWA_MEASURE_EVIDENCE_RESIDUAL, quality, error,
            error_size));
    }
    {
        int have_harmonic_decay =
            item->kind == HWA_ITEM_RELEASE &&
            hwa_measure_running_slope(&accumulator->harmonic_level, &slope);
        double harmonic_decay = have_harmonic_decay ? slope : 0.0;
        HWA_EMIT(hwa_measure_emit(
            engine, accumulator, HWA_MEASURE_HARMONIC_DECAY_DB_PER_SECOND,
            0U, HWA_MEASURE_UNIT_DB_PER_SECOND,
            item->kind == HWA_ITEM_RELEASE
                ? hwa_measure_pitch_dependent_status(
                      pitch_status, have_harmonic_decay,
                      HWA_MEASURE_STATUS_TOO_SHORT)
                : HWA_MEASURE_STATUS_UNSUPPORTED_ITEM,
            harmonic_decay, confidence,
            pitch_evidence | HWA_MEASURE_EVIDENCE_HARMONICS, quality, error,
            error_size));
    }

    if (item->kind == HWA_ITEM_NOTE || item->kind == HWA_ITEM_BODY) {
        HWAMeasureVibratoFacts facts;
        HWAMeasureStatus vibrato_status;
        HWAMeasureStatus drift_status;
        HWAMeasureStatus level_correlation_status;
        HWAMeasureStatus tone_correlation_status;
        double coupling_confidence;
        const HWAMeasureSeriesPoint *points =
            engine->series + accumulator->series_offset;
        size_t valid_points = 0U;
        size_t point;
        for (point = 0U; point < accumulator->series_count; ++point) {
            if (points[point].pitch_valid != 0) valid_points++;
        }
        hwa_measure_vibrato(engine, accumulator, &facts);
        if (pitch_status != HWA_MEASURE_STATUS_VALID) {
            vibrato_status = pitch_status;
        } else {
            vibrato_status = facts.valid != 0
                                 ? HWA_MEASURE_STATUS_VALID
                                 : (accumulator->series_count >= 12U &&
                                    valid_points != accumulator->series_count
                                        ? HWA_MEASURE_STATUS_NO_PITCH
                                        : HWA_MEASURE_STATUS_TOO_SHORT);
        }
        drift_status = vibrato_status != HWA_MEASURE_STATUS_VALID
                           ? vibrato_status
                           : facts.drift_status;
        level_correlation_status =
            pitch_status != HWA_MEASURE_STATUS_VALID
                ? pitch_status
                : facts.level_correlation_status;
        tone_correlation_status =
            pitch_status != HWA_MEASURE_STATUS_VALID
                ? pitch_status
                : facts.tone_correlation_status;
        coupling_confidence =
            accumulator->pitch_frame_count != 0U
                ? confidence *
                      (double)(accumulator->pitch_confidence_sum /
                               (long double)accumulator->pitch_frame_count)
                : 0.0;
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_VIBRATO_DELAY_SECONDS, 0U,
            HWA_MEASURE_UNIT_SECONDS, vibrato_status, facts.delay,
            facts.confidence * confidence, pitch_evidence, quality, error,
            error_size));
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_VIBRATO_RATE_HZ, 0U, HWA_MEASURE_UNIT_HZ,
            vibrato_status, facts.rate, facts.confidence * confidence,
            pitch_evidence, quality, error, error_size));
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_VIBRATO_DEPTH_CENTS, 0U, HWA_MEASURE_UNIT_CENTS,
            vibrato_status, facts.depth, facts.confidence * confidence,
            pitch_evidence, quality, error, error_size));
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_VIBRATO_WAVEFORM_RESIDUAL_RATIO, 0U,
            HWA_MEASURE_UNIT_RATIO, vibrato_status, facts.waveform_residual,
            facts.confidence * confidence, pitch_evidence, quality, error,
            error_size));
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_VIBRATO_RATE_DRIFT_HZ_PER_SECOND, 0U,
            HWA_MEASURE_UNIT_HZ_PER_SECOND, drift_status, facts.rate_drift,
            facts.confidence * confidence, pitch_evidence, quality, error,
            error_size));
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_VIBRATO_DEPTH_DRIFT_CENTS_PER_SECOND, 0U,
            HWA_MEASURE_UNIT_CENTS_PER_SECOND, drift_status,
            facts.depth_drift, facts.confidence * confidence, pitch_evidence,
            quality, error, error_size));
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_PITCH_LEVEL_CORRELATION, 0U,
            HWA_MEASURE_UNIT_RATIO, level_correlation_status,
            facts.level_correlation, coupling_confidence, pitch_evidence,
            quality, error,
            error_size));
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_PITCH_TONE_CORRELATION, 0U,
            HWA_MEASURE_UNIT_RATIO, tone_correlation_status,
            facts.tone_correlation, coupling_confidence, pitch_evidence,
            quality, error,
            error_size));
    }

    if (item->kind == HWA_ITEM_ATTACK) {
        const HWAItemEvent *source = hwa_measure_first_source_event(
            engine->items, item->id);
        HWAMeasureAttackFacts facts;
        hwa_measure_attack_facts(accumulator, engine->sample_rate, &facts);
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_ATTACK_DELAY_SECONDS, 0U,
            HWA_MEASURE_UNIT_SECONDS,
            source != NULL ? HWA_MEASURE_STATUS_VALID
                           : HWA_MEASURE_STATUS_NO_REFERENCE,
            source != NULL
                ? ((double)item->start_sample -
                   (double)source->audio_start_sample) /
                      (double)engine->sample_rate
                : 0.0,
            confidence, HWA_MEASURE_EVIDENCE_MEMBERS |
                            HWA_MEASURE_EVIDENCE_ITEM_BOUNDS,
            quality, error, error_size));
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_RISE_10_SECONDS, 0U, HWA_MEASURE_UNIT_SECONDS,
            hwa_measure_pitch_dependent_status(
                sample_status, facts.valid, HWA_MEASURE_STATUS_TOO_SHORT),
            facts.rise10, confidence, HWA_MEASURE_EVIDENCE_ENVELOPE,
            quality, error, error_size));
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_RISE_50_SECONDS, 0U, HWA_MEASURE_UNIT_SECONDS,
            hwa_measure_pitch_dependent_status(
                sample_status, facts.valid, HWA_MEASURE_STATUS_TOO_SHORT),
            facts.rise50, confidence, HWA_MEASURE_EVIDENCE_ENVELOPE,
            quality, error, error_size));
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_RISE_90_SECONDS, 0U, HWA_MEASURE_UNIT_SECONDS,
            hwa_measure_pitch_dependent_status(
                sample_status, facts.valid, HWA_MEASURE_STATUS_TOO_SHORT),
            facts.rise90, confidence, HWA_MEASURE_EVIDENCE_ENVELOPE,
            quality, error, error_size));
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_ATTACK_SLOPE_DB_PER_SECOND, 0U,
            HWA_MEASURE_UNIT_DB_PER_SECOND,
            hwa_measure_pitch_dependent_status(
                sample_status, facts.valid, HWA_MEASURE_STATUS_TOO_SHORT),
            facts.slope, confidence, HWA_MEASURE_EVIDENCE_ENVELOPE,
            quality, error, error_size));
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_ATTACK_OVERSHOOT_DB, 0U, HWA_MEASURE_UNIT_DB,
            hwa_measure_pitch_dependent_status(
                sample_status, facts.valid, HWA_MEASURE_STATUS_TOO_SHORT),
            facts.overshoot, confidence, HWA_MEASURE_EVIDENCE_ENVELOPE,
            quality, error, error_size));
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_NOISE_BURST_SECONDS, 0U,
            HWA_MEASURE_UNIT_SECONDS,
            hwa_measure_pitch_dependent_status(
                pitch_status, accumulator->have_noise_peak,
                HWA_MEASURE_STATUS_NO_DATA),
            accumulator->noise_peak_time, confidence,
            pitch_evidence | HWA_MEASURE_EVIDENCE_RESIDUAL, quality, error,
            error_size));
        HWA_EMIT(hwa_measure_emit_level(engine, accumulator,
            HWA_MEASURE_PRE_NOTE_RESIDUAL_DBFS, 0U,
            accumulator->context_count != 0U
                ? HWA_MEASURE_STATUS_VALID
                : HWA_MEASURE_STATUS_NO_DATA,
            accumulator->context_count != 0U
                ? hwa_measure_power_db(
                      (double)(accumulator->context_energy /
                               (long double)accumulator->context_count))
                : 0.0,
            confidence, HWA_MEASURE_EVIDENCE_SAMPLES |
                            HWA_MEASURE_EVIDENCE_PREVIOUS_ITEM,
            quality, error, error_size));
    }
    if (item->kind == HWA_ITEM_RELEASE) {
        int level_slope = hwa_measure_running_slope(&accumulator->level,
                                                    &slope);
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_EARLY_DAMPING_DB_PER_SECOND, 0U,
            HWA_MEASURE_UNIT_DB_PER_SECOND,
            hwa_measure_pitch_dependent_status(
                frame_status, level_slope, HWA_MEASURE_STATUS_TOO_SHORT),
            level_slope ? slope : 0.0, confidence, spectrum_evidence,
            quality, error, error_size));
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_PITCH_FALL_CENTS, 0U, HWA_MEASURE_UNIT_CENTS,
            pitch_status,
            accumulator->pitch_cents.count != 0U
                ? accumulator->pitch_cents.last -
                      accumulator->pitch_cents.first
                : 0.0,
            confidence, pitch_evidence, quality, error, error_size));
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_DECAY_DB, 0U, HWA_MEASURE_UNIT_DB,
            hwa_measure_pitch_dependent_status(
                frame_status, accumulator->level.count >= 2U,
                HWA_MEASURE_STATUS_TOO_SHORT),
            accumulator->level.count >= 2U
                ? accumulator->level.first - accumulator->level.last
                : 0.0,
            confidence, spectrum_evidence, quality, error, error_size));
        HWA_EMIT(hwa_measure_emit_level(engine, accumulator,
            HWA_MEASURE_RESIDUAL_EXCITATION_DBFS, 0U,
            hwa_measure_pitch_dependent_status(
                pitch_status, accumulator->residual_level.count != 0U,
                HWA_MEASURE_STATUS_NO_DATA),
            accumulator->residual_level.count != 0U
                ? accumulator->residual_level.last
                : 0.0,
            confidence,
            pitch_evidence | HWA_MEASURE_EVIDENCE_RESIDUAL, quality, error,
            error_size));
    }
    if (item->kind == HWA_ITEM_TRANSITION) {
        const HWAItemEvent *from;
        const HWAItemEvent *to;
        int have = hwa_measure_transition_events(engine->items, item->id,
                                                  &from, &to);
        double gap = have
            ? ((double)to->audio_start_sample -
               (double)from->audio_end_sample) /
                  (double)engine->sample_rate
            : 0.0;
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_GAP_OVERLAP_SECONDS, 0U,
            HWA_MEASURE_UNIT_SECONDS,
            have ? HWA_MEASURE_STATUS_VALID
                 : HWA_MEASURE_STATUS_NO_REFERENCE,
            gap, confidence, HWA_MEASURE_EVIDENCE_MEMBERS, quality, error,
            error_size));
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_CARRYOVER_DB, 0U, HWA_MEASURE_UNIT_DB,
            hwa_measure_pitch_dependent_status(
                frame_status, accumulator->level.count >= 2U,
                HWA_MEASURE_STATUS_TOO_SHORT),
            accumulator->level.count >= 2U
                ? accumulator->level.first - accumulator->level.last
                : 0.0,
            confidence, spectrum_evidence, quality, error, error_size));
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_TRANSITION_PITCH_CHANGE_CENTS, 0U,
            HWA_MEASURE_UNIT_CENTS,
            sample_status != HWA_MEASURE_STATUS_VALID
                ? sample_status
                : transition_facts.pitch_status,
            transition_facts.pitch_change,
            confidence * transition_facts.confidence,
            pitch_evidence, quality, error, error_size));
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_TRANSITION_TONE_CHANGE_HZ, 0U,
            HWA_MEASURE_UNIT_HZ,
            hwa_measure_pitch_dependent_status(
                frame_status, accumulator->centroid.count >= 2U,
                HWA_MEASURE_STATUS_TOO_SHORT),
            accumulator->centroid.count >= 2U
                ? accumulator->centroid.last - accumulator->centroid.first
                : 0.0,
            confidence, spectrum_evidence, quality, error, error_size));
    }
    if (item->kind == HWA_ITEM_NOTE) {
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_REPEATED_ATTACK_SIMILARITY, 0U,
            HWA_MEASURE_UNIT_RATIO,
            hwa_measure_pitch_dependent_status(
                sample_status, accumulator->repeated_attack_valid,
                HWA_MEASURE_STATUS_NO_REFERENCE),
            accumulator->repeated_attack_similarity, confidence,
            HWA_MEASURE_EVIDENCE_PREVIOUS_ITEM |
                HWA_MEASURE_EVIDENCE_ENVELOPE,
            quality, error, error_size));
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_REPEATED_PITCH_CURVE_SIMILARITY, 0U,
            HWA_MEASURE_UNIT_RATIO,
            hwa_measure_pitch_dependent_status(
                pitch_status, accumulator->repeated_pitch_valid,
                HWA_MEASURE_STATUS_NO_REFERENCE),
            accumulator->repeated_pitch_similarity, confidence,
            HWA_MEASURE_EVIDENCE_PREVIOUS_ITEM |
                HWA_MEASURE_EVIDENCE_PITCH,
            quality, error, error_size));
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_LOCAL_CONTRAST_DB, 0U, HWA_MEASURE_UNIT_DB,
            hwa_measure_pitch_dependent_status(
                sample_status, accumulator->local_contrast_valid,
                HWA_MEASURE_STATUS_NO_REFERENCE),
            accumulator->local_contrast, confidence,
            HWA_MEASURE_EVIDENCE_PREVIOUS_ITEM |
                HWA_MEASURE_EVIDENCE_SAMPLES,
            quality, error, error_size));
        HWA_EMIT(hwa_measure_emit(engine, accumulator,
            HWA_MEASURE_ACCENT_SIZE_DB, 0U, HWA_MEASURE_UNIT_DB,
            !hwa_measure_is_accent(
                &engine->result->contexts[item_index].labels)
                ? HWA_MEASURE_STATUS_UNSUPPORTED_ITEM
                : hwa_measure_pitch_dependent_status(
                      sample_status, accumulator->accent_size_valid,
                      HWA_MEASURE_STATUS_NO_REFERENCE),
            accumulator->accent_size, confidence,
            HWA_MEASURE_EVIDENCE_PREVIOUS_ITEM |
                HWA_MEASURE_EVIDENCE_MEMBERS,
            quality, error, error_size));
    }
    HWA_EMIT(hwa_measure_emit(
        engine, accumulator, HWA_MEASURE_DURATION_SECONDS, 0U,
        HWA_MEASURE_UNIT_SECONDS, HWA_MEASURE_STATUS_VALID, duration,
        confidence, HWA_MEASURE_EVIDENCE_ITEM_BOUNDS, quality, error,
        error_size));

#undef HWA_EMIT
    return 0;
}

static int hwa_measure_feed(HWAMeasureEngine *engine,
                            const double *samples,
                            size_t count,
                            char *error,
                            size_t error_size)
{
    size_t index;
    if (count > engine->options->decode_block_frames ||
        (uint64_t)count > engine->total_frames - engine->pushed_frames) {
        hwa_set_error(error, error_size,
                      "invalid measurement sample block");
        return -1;
    }
    for (index = 0U; index < count; ++index) {
        if (engine->range_block_count == 0U) {
            engine->range_block_start = engine->pushed_frames;
        }
        engine->range_block[engine->range_block_count++] = samples[index];
        if (engine->range_block_count == HWA_MEASURE_RANGE_BLOCK_FRAMES) {
            if (hwa_measure_accumulate_sample_block(
                    engine, engine->range_block, engine->range_block_count,
                    engine->range_block_start, error, error_size) != 0) {
                return -1;
            }
            engine->range_block_count = 0U;
        }
        engine->sample_ring[engine->pushed_frames %
                            engine->options->fft_size] = samples[index];
        engine->pushed_frames++;
        while (engine->next_frame_start <= engine->pushed_frames &&
               engine->options->fft_size <=
                   engine->pushed_frames - engine->next_frame_start) {
            if (engine->next_frame_start +
                    (uint64_t)engine->options->fft_size / 2U <
                engine->total_frames) {
                if (hwa_measure_process_frame(
                        engine, engine->next_frame_start,
                        error, error_size) != 0) {
                    return -1;
                }
            }
            engine->next_frame_start +=
                (uint64_t)engine->options->hop_size;
        }
    }
    return 0;
}

static int hwa_measure_add_capability_warning(HWAMeasureEngine *engine,
                                              char *error,
                                              size_t error_size)
{
    static const char code[] = "stage4-capabilities-unavailable";
    static const char message[] =
        "Production correction and numeric control probes are unavailable in stage4-1.";
    HWAMeasureWarning *warning;

    if (engine->options->max_warnings == 0U) {
        hwa_set_error(error, error_size,
                      "measurement warning limit is zero");
        return -1;
    }
    engine->result->warnings = (HWAMeasureWarning *)hwa_measure_allocate(
        &engine->work, 1U, sizeof(*engine->result->warnings), 1);
    if (engine->result->warnings == NULL) {
        hwa_set_error(error, error_size,
                      "capability warning exceeds the work limit");
        return -1;
    }
    warning = &engine->result->warnings[0U];
    engine->result->warning_count = 1U;
    warning->id = 1U;
    warning->code = hwa_measure_copy(&engine->work, code);
    warning->message = hwa_measure_copy(&engine->work, message);
    if (warning->code == NULL || warning->message == NULL) {
        hwa_set_error(error, error_size,
                      "capability warning strings exceed the work limit");
        return -1;
    }
    return 0;
}

static int hwa_measure_shrink_observations(HWAMeasureEngine *engine,
                                           char *error,
                                           size_t error_size)
{
    size_t count = engine->result->measurement_count;
    size_t old_bytes;
    size_t new_bytes;
    HWAMeasureObservation *shrunk;

    if (count == engine->observation_capacity) return 0;
    if (hwa_measure_size_multiply(engine->observation_capacity,
                                  sizeof(*shrunk), &old_bytes) != 0 ||
        hwa_measure_size_multiply(count, sizeof(*shrunk), &new_bytes) != 0) {
        hwa_set_error(error, error_size,
                      "measurement output size overflows");
        return -1;
    }
    if (count == 0U) {
        free(engine->result->measurements);
        engine->result->measurements = NULL;
    } else {
        shrunk = (HWAMeasureObservation *)realloc(
            engine->result->measurements, new_bytes);
        if (shrunk == NULL) {
            hwa_set_error(error, error_size,
                          "could not trim measurement output");
            return -1;
        }
        engine->result->measurements = shrunk;
    }
    engine->work.live -= (uint64_t)(old_bytes - new_bytes);
    engine->work.retained -= (uint64_t)(old_bytes - new_bytes);
    engine->observation_capacity = count;
    return 0;
}

static int hwa_measure_engine_begin(HWAMeasureEngine *engine,
                                    const HWAItemSet *items,
                                    uint64_t total_frames,
                                    uint32_t sample_rate,
                                    const HWAMeasurementOptions *options,
                                    uint64_t retained_input_bytes,
                                    HWAMeasurementSet *result,
                                    char *error,
                                    size_t error_size)
{
    memset(engine, 0, sizeof(*engine));
    engine->items = items;
    engine->options = options;
    engine->result = result;
    engine->sample_rate = sample_rate;
    engine->total_frames = total_frames;
    engine->work.limit = options->max_work_bytes;
    engine->work.live = retained_input_bytes;
    if (retained_input_bytes > options->max_work_bytes) {
        hwa_set_error(error, error_size,
                      "retained item data exceeds the measurement work limit");
        return -1;
    }
    if (items == NULL || sample_rate < 8000U || sample_rate > 768000U ||
        total_frames == 0U || total_frames > options->max_input_frames ||
        items->audio_format.frames != total_frames ||
        items->audio_format.sample_rate_hz != sample_rate) {
        hwa_set_error(error, error_size,
                      "item and audio shapes do not match");
        return -1;
    }
    if (hwa_measure_prepare_items(engine, error, error_size) != 0 ||
        hwa_measure_prepare_buffers(engine, error, error_size) != 0) {
        return -1;
    }
    return 0;
}

static int hwa_measure_engine_finish(HWAMeasureEngine *engine,
                                     char *error,
                                     size_t error_size)
{
    size_t index;

    if (engine->pushed_frames != engine->total_frames) {
        hwa_set_error(error, error_size,
                      "decoded measurement frame count changed");
        return -1;
    }
    if (engine->range_block_count != 0U) {
        if (hwa_measure_accumulate_sample_block(
                engine, engine->range_block, engine->range_block_count,
                engine->range_block_start, error, error_size) != 0) {
            return -1;
        }
        engine->range_block_count = 0U;
    }
    while (engine->next_frame_start +
               (uint64_t)engine->options->fft_size / 2U <
           engine->total_frames) {
        if (hwa_measure_process_frame(engine, engine->next_frame_start,
                                      error, error_size) != 0) {
            return -1;
        }
        engine->next_frame_start +=
            (uint64_t)engine->options->hop_size;
    }
    if (hwa_measure_make_level_reference(engine, error, error_size) != 0 ||
        hwa_measure_prepare_relations(engine, error, error_size) != 0) {
        return -1;
    }
    for (index = 0U; index < engine->items->item_count; ++index) {
        if (hwa_measure_finalize_item(engine, index, error, error_size) != 0) {
            return -1;
        }
    }
    if (hwa_measure_add_capability_warning(engine, error, error_size) != 0 ||
        hwa_measure_shrink_observations(engine, error, error_size) != 0) {
        return -1;
    }
    engine->result->item_frame_evaluations = engine->evaluations;
    engine->result->transform_count = engine->transform_count;
    engine->result->capability_flags = 0U;
    engine->result->retained_work_bytes = engine->work.retained;
    return 0;
}

static void hwa_measure_engine_release_scratch(HWAMeasureEngine *engine)
{
    size_t item_count = engine->items != NULL ? engine->items->item_count : 0U;
    size_t event_count = engine->items != NULL ? engine->items->event_count : 0U;

    hwa_measure_release(&engine->work, engine->peak_tree,
                        engine->peak_tree_leaf_count * 2U,
                        sizeof(*engine->peak_tree), 0);
    hwa_measure_release(&engine->work, engine->block_energy_prefix,
                        HWA_MEASURE_RANGE_BLOCK_FRAMES + 1U,
                        sizeof(*engine->block_energy_prefix), 0);
    hwa_measure_release(&engine->work, engine->range_block,
                        HWA_MEASURE_RANGE_BLOCK_FRAMES,
                        sizeof(*engine->range_block), 0);
    hwa_measure_release(&engine->work, engine->block_mono,
                        engine->options->decode_block_frames,
                        sizeof(*engine->block_mono), 0);
    hwa_measure_release(&engine->work, engine->previous_magnitude,
                        engine->spectrum_bins,
                        sizeof(*engine->previous_magnitude), 0);
    hwa_measure_release(&engine->work, engine->power, engine->spectrum_bins,
                        sizeof(*engine->power), 0);
    hwa_measure_release(&engine->work, engine->fft,
                        engine->options->fft_size, sizeof(*engine->fft), 0);
    hwa_measure_release(&engine->work, engine->window,
                        engine->options->fft_size, sizeof(*engine->window), 0);
    hwa_measure_release(&engine->work, engine->sample_ring,
                        engine->options->fft_size,
                        sizeof(*engine->sample_ring), 0);
    hwa_measure_release(&engine->work, engine->series,
                        engine->series_capacity, sizeof(*engine->series), 0);
    hwa_measure_release(&engine->work, engine->event_multi_pitch,
                        event_count, 1U, 0);
    hwa_measure_release(&engine->work, engine->sample_active_items,
                        item_count, sizeof(*engine->sample_active_items), 0);
    hwa_measure_release(&engine->work, engine->active_items,
                        item_count, sizeof(*engine->active_items), 0);
    hwa_measure_release(&engine->work, engine->sample_start_order,
                        item_count, sizeof(*engine->sample_start_order), 0);
    hwa_measure_release(&engine->work, engine->start_order, item_count,
                        sizeof(*engine->start_order), 0);
    hwa_measure_release(&engine->work, engine->accumulators, item_count,
                        sizeof(*engine->accumulators), 0);
}

int hwa_measure_engine_samples(const HWAItemSet *items,
                               const double *samples,
                               uint64_t frame_count,
                               uint32_t sample_rate_hz,
                               const HWAMeasurementOptions *provided_options,
                               uint64_t retained_input_bytes,
                               HWAMeasurementSet *result,
                               char *error,
                               size_t error_size)
{
    HWAMeasurementOptions copied_options;
    HWAMeasureEngine engine;
    uint64_t position = 0U;
    int begun = 0;
    int status = -1;

    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (result == NULL) {
        hwa_set_error(error, error_size, "missing measurement result");
        return -1;
    }
    if (provided_options != NULL) copied_options = *provided_options;
    else hwa_measurement_options_default(&copied_options);
    memset(result, 0, sizeof(*result));
    result->options = copied_options;
    if (!hwa_measure_options_valid(&copied_options) || items == NULL ||
        samples == NULL || frame_count == 0U) {
        hwa_set_error(error, error_size,
                      "invalid measurement samples or options");
        return -1;
    }
    memset(&engine, 0, sizeof(engine));
    begun = 1;
    if (hwa_measure_engine_begin(&engine, items, frame_count, sample_rate_hz,
                                 &result->options, retained_input_bytes,
                                 result, error, error_size) != 0) {
        goto cleanup;
    }
    result->audio_format = items->audio_format;
    while (position < frame_count) {
        size_t count = copied_options.decode_block_frames;
        if ((uint64_t)count > frame_count - position) {
            count = (size_t)(frame_count - position);
        }
        if (hwa_measure_feed(&engine, samples + (size_t)position, count,
                             error, error_size) != 0) {
            goto cleanup;
        }
        position += count;
    }
    if (hwa_measure_engine_finish(&engine, error, error_size) != 0) {
        goto cleanup;
    }
    status = 0;

cleanup:
    if (begun) hwa_measure_engine_release_scratch(&engine);
    if (status != 0) {
        hwa_measurement_set_free(result);
        result->options = copied_options;
    }
    return status;
}

int hwa_measure_engine_wav(const HWAItemSet *items,
                           const char *explicit_audio_path,
                           const HWAMeasurementOptions *provided_options,
                           uint64_t retained_input_bytes,
                           HWAMeasurementSet *result,
                           char *error,
                           size_t error_size)
{
    HWAMeasurementOptions copied_options;
    HWAMeasureEngine engine;
    HWAWavReader reader;
    unsigned char *raw = NULL;
    size_t raw_size = 0U;
    int reader_open = 0;
    int begun = 0;
    int status = -1;

    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (result == NULL) {
        hwa_set_error(error, error_size, "missing measurement result");
        return -1;
    }
    if (provided_options != NULL) copied_options = *provided_options;
    else hwa_measurement_options_default(&copied_options);
    memset(result, 0, sizeof(*result));
    result->options = copied_options;
    memset(&engine, 0, sizeof(engine));
    memset(&reader, 0, sizeof(reader));
    if (!hwa_measure_options_valid(&copied_options) || items == NULL ||
        explicit_audio_path == NULL || explicit_audio_path[0] == '\0' ||
        strcmp(explicit_audio_path, "-") == 0) {
        hwa_set_error(error, error_size,
                      "invalid measurement audio path or options");
        return -1;
    }
    if (hwa_wav_reader_open(&reader, explicit_audio_path,
                            copied_options.max_input_bytes,
                            error, error_size) != 0) {
        goto cleanup;
    }
    reader_open = 1;
    if (reader.format.frames > copied_options.max_input_frames ||
        reader.format.frames != items->audio_format.frames ||
        reader.format.sample_rate_hz != items->audio_format.sample_rate_hz ||
        fabs(reader.format.duration_seconds -
             items->audio_format.duration_seconds) > 1.0e-12) {
        hwa_set_error(error, error_size,
                      "explicit audio format does not match the item file");
        goto cleanup;
    }
    begun = 1;
    if (hwa_measure_engine_begin(
            &engine, items, reader.format.frames,
            reader.format.sample_rate_hz, &result->options,
            retained_input_bytes, result, error, error_size) != 0) {
        goto cleanup;
    }
    result->audio_format = reader.format;
    if (copied_options.decode_block_frames >
        SIZE_MAX / reader.format.block_align) {
        hwa_set_error(error, error_size,
                      "measurement decode buffer size overflows");
        goto cleanup;
    }
    raw_size = copied_options.decode_block_frames * reader.format.block_align;
    raw = (unsigned char *)hwa_measure_allocate(
        &engine.work, raw_size, 1U, 0);
    if (raw == NULL) {
        hwa_set_error(error, error_size,
                      "measurement decode buffer exceeds the work limit");
        goto cleanup;
    }
    for (;;) {
        size_t frames_read;
        size_t frame;
        if (hwa_wav_reader_read_frames(
                &reader, raw, copied_options.decode_block_frames,
                &frames_read, error, error_size) != 0) {
            goto cleanup;
        }
        if (frames_read == 0U) break;
        for (frame = 0U; frame < frames_read; ++frame) {
            const unsigned char *frame_data =
                raw + frame * reader.format.block_align;
            long double sum = 0.0L;
            uint16_t channel;
            for (channel = 0U; channel < reader.format.channels; ++channel) {
                int clipped;
                double sample = hwa_wav_decode_sample(
                    &reader,
                    frame_data + (size_t)channel * reader.bytes_per_sample,
                    &clipped);
                (void)clipped;
                if (!isfinite(sample)) {
                    hwa_set_error(error, error_size,
                                  "non-finite sample at frame %llu, channel %u",
                                  (unsigned long long)(engine.pushed_frames +
                                                       (uint64_t)frame),
                                  (unsigned)channel + 1U);
                    goto cleanup;
                }
                sum += (long double)sample;
            }
            engine.block_mono[frame] =
                (double)(sum / (long double)reader.format.channels);
        }
        if (hwa_measure_feed(&engine, engine.block_mono, frames_read,
                             error, error_size) != 0) {
            goto cleanup;
        }
    }
    hwa_measure_release(&engine.work, raw, raw_size, 1U, 0);
    raw = NULL;
    raw_size = 0U;
    if (hwa_measure_engine_finish(&engine, error, error_size) != 0) {
        goto cleanup;
    }
    status = 0;

cleanup:
    if (begun) {
        hwa_measure_release(&engine.work, raw, raw_size, 1U, 0);
        hwa_measure_engine_release_scratch(&engine);
    }
    if (reader_open) hwa_wav_reader_close(&reader);
    if (status != 0) {
        hwa_measurement_set_free(result);
        result->options = copied_options;
    }
    return status;
}

static char *hwa_measure_result_copy(const char *text,
                                     HWAMeasurementSet *result,
                                     uint64_t live_input_bytes,
                                     char *error,
                                     size_t error_size)
{
    size_t length;
    char *copy;

    if (text == NULL) return NULL;
    length = strlen(text);
    if (length == SIZE_MAX ||
        result->retained_work_bytes > result->options.max_work_bytes ||
        (uint64_t)length + 1U > result->options.max_work_bytes -
            result->retained_work_bytes ||
        live_input_bytes > result->options.max_work_bytes -
            result->retained_work_bytes - ((uint64_t)length + 1U)) {
        hwa_set_error(error, error_size,
                      "measurement provenance exceeds the work limit");
        return NULL;
    }
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for measurement provenance");
        return NULL;
    }
    memcpy(copy, text, length + 1U);
    result->retained_work_bytes += (uint64_t)length + 1U;
    return copy;
}

int hwa_measure_item_file_wav(const char *items_path,
                              const char *audio_path,
                              const HWAMeasurementOptions *provided_options,
                              HWAMeasurementSet *result,
                              char *error,
                              size_t error_size)
{
    HWAMeasurementOptions copied_options;
    HWAItemFileLimits limits;
    HWAItemFileData data;
    char audio_hash[HWA_SHA256_HEX_SIZE];
    char check_hash[HWA_SHA256_HEX_SIZE];
    int have_data = 0;
    int measured = 0;
    int status = -1;

    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (result == NULL) {
        hwa_set_error(error, error_size, "missing measurement result");
        return -1;
    }
    if (provided_options != NULL) copied_options = *provided_options;
    else hwa_measurement_options_default(&copied_options);
    memset(result, 0, sizeof(*result));
    result->options = copied_options;
    memset(&data, 0, sizeof(data));
    if (!hwa_measure_options_valid(&copied_options) ||
        items_path == NULL || items_path[0] == '\0' ||
        strcmp(items_path, "-") == 0 || audio_path == NULL ||
        audio_path[0] == '\0' || strcmp(audio_path, "-") == 0) {
        hwa_set_error(error, error_size,
                      "invalid measurement paths or options");
        return -1;
    }
    if (hwa_sha256_file(audio_path, copied_options.max_input_bytes,
                        audio_hash, error, error_size) != 0) {
        goto cleanup;
    }
    hwa_item_file_limits_default(&limits);
    limits.max_bytes = copied_options.max_input_bytes;
    limits.max_work_bytes = copied_options.max_work_bytes;
    limits.max_events = copied_options.max_events;
    limits.max_items = copied_options.max_items;
    limits.max_manual_items = copied_options.max_items;
    limits.max_members = copied_options.max_item_members;
    limits.max_warnings = copied_options.max_warnings;
    if (hwa_item_file_read_full(items_path, &limits, &data,
                                error, error_size) != 0) {
        goto cleanup;
    }
    have_data = 1;
    if (strcmp(audio_hash, data.items.audio_sha256) != 0) {
        hwa_set_error(error, error_size,
                      "audio SHA-256 does not match the item file");
        goto cleanup;
    }
    if (hwa_measure_engine_wav(
            &data.items, audio_path, &copied_options,
            data.retained_work_bytes, result, error, error_size) != 0) {
        goto cleanup;
    }
    measured = 1;
    if (hwa_sha256_file(audio_path, copied_options.max_input_bytes,
                        check_hash, error, error_size) != 0 ||
        strcmp(check_hash, audio_hash) != 0) {
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "audio changed while measurement ran");
        }
        goto cleanup;
    }
    if (hwa_sha256_file(items_path, copied_options.max_input_bytes,
                        check_hash, error, error_size) != 0 ||
        strcmp(check_hash, data.sha256) != 0) {
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            hwa_set_error(error, error_size,
                          "item file changed while measurement ran");
        }
        goto cleanup;
    }
    result->items_path = hwa_measure_result_copy(
        items_path, result, data.retained_work_bytes, error, error_size);
    result->audio_path = hwa_measure_result_copy(
        audio_path, result, data.retained_work_bytes, error, error_size);
    result->alignment_path = hwa_measure_result_copy(
        data.items.alignment_path, result, data.retained_work_bytes,
        error, error_size);
    result->labels_path = hwa_measure_result_copy(
        data.items.labels_path, result, data.retained_work_bytes,
        error, error_size);
    result->amendment_path = hwa_measure_result_copy(
        data.items.amendment_path, result, data.retained_work_bytes,
        error, error_size);
    result->source_score_path = hwa_measure_result_copy(
        data.items.source_score_path, result, data.retained_work_bytes,
        error, error_size);
    if (result->items_path == NULL || result->audio_path == NULL ||
        (data.items.alignment_path != NULL && result->alignment_path == NULL) ||
        (data.items.labels_path != NULL && result->labels_path == NULL) ||
        (data.items.amendment_path != NULL && result->amendment_path == NULL) ||
        (data.items.source_score_path != NULL &&
         result->source_score_path == NULL)) {
        goto cleanup;
    }
    memcpy(result->items_sha256, data.sha256, HWA_SHA256_HEX_SIZE);
    memcpy(result->audio_sha256, audio_hash, HWA_SHA256_HEX_SIZE);
    memcpy(result->alignment_sha256, data.items.alignment_sha256,
           HWA_SHA256_HEX_SIZE);
    memcpy(result->labels_sha256, data.items.labels_sha256,
           HWA_SHA256_HEX_SIZE);
    memcpy(result->amendment_sha256, data.items.amendment_sha256,
           HWA_SHA256_HEX_SIZE);
    memcpy(result->source_score_sha256, data.items.source_score_sha256,
           HWA_SHA256_HEX_SIZE);
    hwa_item_file_data_free(&data);
    have_data = 0;
    if (hwa_measure_build_profile(result, error, error_size) != 0) {
        goto cleanup;
    }
    status = 0;

cleanup:
    if (have_data) hwa_item_file_data_free(&data);
    if (status != 0 && measured) {
        hwa_measurement_set_free(result);
        result->options = copied_options;
    }
    return status;
}

#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "measure_engine.h"

#include "item_file.h"
#include "sha256.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TEST_PI 3.14159265358979323846264338327950288

static int failures = 0;

#define CHECK(condition, message)                                             \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, (message));   \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static long test_process_id(void)
{
#if defined(_WIN32)
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}

static int test_make_directory(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

static int test_remove_directory(const char *path)
{
#if defined(_WIN32)
    return _rmdir(path);
#else
    return rmdir(path);
#endif
}

static int test_remove_file(const char *path)
{
#if defined(_WIN32)
    return _unlink(path);
#else
    return unlink(path);
#endif
}

static int test_workspace(char path[PATH_MAX])
{
    unsigned attempt;
#if defined(_WIN32)
    const char *root = getenv("TEMP");
    if (root == NULL || root[0] == '\0') root = ".";
#else
    const char *root = "/tmp";
#endif

    for (attempt = 0U; attempt < 100U; ++attempt) {
        int length = snprintf(path, PATH_MAX,
                              "%s/hwa-measure-engine-%ld-%u", root,
                              test_process_id(), attempt);
        if (length < 0 || length >= PATH_MAX) return 0;
        if (test_make_directory(path) == 0) return 1;
        if (errno != EEXIST) return 0;
    }
    return 0;
}

static int test_path(char output[PATH_MAX],
                     const char *directory,
                     const char *name)
{
    int length = snprintf(output, PATH_MAX, "%s/%s", directory, name);
    return length >= 0 && length < PATH_MAX;
}

static int test_write_stream_bytes(FILE *stream,
                                   const void *bytes,
                                   size_t size)
{
    return fwrite(bytes, 1U, size, stream) == size;
}

static int test_write_u16(FILE *stream, uint16_t value)
{
    unsigned char bytes[2];
    bytes[0] = (unsigned char)(value & UINT16_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT16_C(0xff));
    return test_write_stream_bytes(stream, bytes, sizeof(bytes));
}

static int test_write_u32(FILE *stream, uint32_t value)
{
    unsigned char bytes[4];
    bytes[0] = (unsigned char)(value & UINT32_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT32_C(0xff));
    bytes[2] = (unsigned char)((value >> 16U) & UINT32_C(0xff));
    bytes[3] = (unsigned char)((value >> 24U) & UINT32_C(0xff));
    return test_write_stream_bytes(stream, bytes, sizeof(bytes));
}

static int test_write_stereo_wav(const char *path,
                                 uint32_t sample_rate,
                                 uint32_t frames)
{
    const uint16_t channels = 2U;
    const uint16_t block_align = 4U;
    uint32_t data_size = frames * (uint32_t)block_align;
    FILE *stream = fopen(path, "wb");
    uint32_t frame;
    int okay;

    if (stream == NULL) return 0;
    okay = test_write_stream_bytes(stream, "RIFF", 4U) &&
           test_write_u32(stream, 36U + data_size) &&
           test_write_stream_bytes(stream, "WAVE", 4U) &&
           test_write_stream_bytes(stream, "fmt ", 4U) &&
           test_write_u32(stream, 16U) && test_write_u16(stream, 1U) &&
           test_write_u16(stream, channels) &&
           test_write_u32(stream, sample_rate) &&
           test_write_u32(stream,
                          sample_rate * (uint32_t)block_align) &&
           test_write_u16(stream, block_align) &&
           test_write_u16(stream, 16U) &&
           test_write_stream_bytes(stream, "data", 4U) &&
           test_write_u32(stream, data_size);
    for (frame = 0U; okay && frame < frames; ++frame) {
        double value = 0.2 * sin(2.0 * TEST_PI * 440.0 *
                                 (double)frame / (double)sample_rate);
        int16_t sample = (int16_t)lrint(value * 32767.0);
        okay = test_write_u16(stream, (uint16_t)sample) &&
               test_write_u16(stream, (uint16_t)sample);
    }
    if (fclose(stream) != 0) okay = 0;
    return okay;
}

static HWAMeasurementOptions test_options(void)
{
    HWAMeasurementOptions options;
    hwa_measurement_options_default(&options);
    options.decode_block_frames = 257U;
    options.fft_size = 1024U;
    options.hop_size = 64U;
    options.max_input_bytes = UINT64_MAX;
    options.max_input_frames = UINT64_MAX;
    options.max_work_bytes = UINT64_C(268435456);
    options.max_transforms = 100000U;
    options.max_series_points = 100000U;
    options.max_item_frame_evaluations = UINT64_C(10000000);
    options.max_partials = 8U;
    options.max_events = 100U;
    options.max_items = 100U;
    options.max_item_members = 500U;
    options.max_measurements = 100000U;
    options.max_groups = 1000U;
    options.max_group_members = 10000U;
    options.max_statistics = 100000U;
    options.max_warnings = 10U;
    return options;
}

static void set_event(HWAItemEvent *event,
                      uint64_t id,
                      char *event_id,
                      char *midi,
                      char *part,
                      char *dynamic)
{
    memset(event, 0, sizeof(*event));
    event->id = id;
    event->event_id = event_id;
    event->kind = (char *)"note";
    event->midi_note = midi;
    event->labels.pitch = midi;
    event->labels.part = part;
    event->labels.dynamic = dynamic;
    event->alignment_confidence = 1.0;
}

static void set_item(HWAItem *item,
                     uint64_t id,
                     HWAItemKind kind,
                     char *key,
                     char *role,
                     uint64_t start,
                     uint64_t end,
                     uint64_t parent)
{
    memset(item, 0, sizeof(*item));
    item->id = id;
    item->kind = kind;
    item->key = key;
    item->role = role;
    item->start_sample = start;
    item->end_sample = end;
    item->confidence = 1.0;
    if (parent != 0U) {
        item->parent_id = parent;
        item->parent_valid = 1;
    }
}

static void set_source_member(HWAItemMember *member,
                              uint64_t item_id,
                              uint64_t event_id)
{
    member->item_id = item_id;
    member->event_id = event_id;
    member->role = HWA_ITEM_MEMBER_SOURCE;
}

static void set_member(HWAItemMember *member,
                       uint64_t item_id,
                       uint64_t event_id,
                       HWAItemMemberRole role)
{
    member->item_id = item_id;
    member->event_id = event_id;
    member->role = role;
}

static const HWAMeasureObservation *find_measure(
    const HWAMeasurementSet *result,
    uint64_t item_id,
    HWAMeasureKind kind,
    uint32_t index,
    HWAMeasureView view)
{
    size_t offset;
    for (offset = 0U; offset < result->measurement_count; ++offset) {
        const HWAMeasureObservation *measure = &result->measurements[offset];
        if (measure->item_id == item_id && measure->kind == kind &&
            measure->index == index && measure->view == view) {
            return measure;
        }
    }
    return NULL;
}

static void make_note_samples(double *samples,
                              size_t count,
                              uint32_t sample_rate,
                              double amplitude,
                              double vibrato_depth_cents,
                              double vibrato_rate_hz)
{
    uint32_t noise = UINT32_C(0x12345678);
    size_t index;
    double phase = 0.0;

    for (index = 0U; index < count; ++index) {
        double time = (double)index / (double)sample_rate;
        double envelope;
        double cents = time >= 0.20
                           ? vibrato_depth_cents *
                                 sin(2.0 * TEST_PI * vibrato_rate_hz *
                                     (time - 0.20))
                           : 0.0;
        double frequency = 440.0 * pow(2.0, cents / 1200.0);
        double value;

        if (time < 0.12) envelope = time / 0.12;
        else if (time < 0.80) envelope = 1.0;
        else envelope = fmax(0.0, (1.0 - time) / 0.20);
        phase += 2.0 * TEST_PI * frequency / (double)sample_rate;
        noise = noise * UINT32_C(1664525) + UINT32_C(1013904223);
        value = sin(phase) + 0.45 * sin(2.0 * phase + 0.2) +
                0.20 * sin(3.0 * phase - 0.1) +
                0.004 * ((double)((noise >> 8U) & UINT32_C(0xffff)) /
                             32767.5 -
                         1.0);
        samples[index] = amplitude * envelope * value;
    }
}

static void make_delayed_vibrato_samples(double *samples,
                                         size_t count,
                                         uint32_t sample_rate,
                                         double tuning_offset_cents,
                                         double vibrato_delay,
                                         double vibrato_depth_cents,
                                         double vibrato_rate_hz,
                                         double gap_start,
                                         double gap_end)
{
    size_t index;
    double phase = 0.0;

    for (index = 0U; index < count; ++index) {
        double time = (double)index / (double)sample_rate;
        double cents = tuning_offset_cents;
        double envelope = 1.0;
        double frequency;

        if (time >= vibrato_delay) {
            cents += vibrato_depth_cents *
                     sin(2.0 * TEST_PI * vibrato_rate_hz *
                         (time - vibrato_delay));
        }
        if (time < 0.03) envelope = time / 0.03;
        if (time > (double)count / (double)sample_rate - 0.03) {
            envelope = fmax(
                0.0,
                ((double)count / (double)sample_rate - time) / 0.03);
        }
        if (time >= gap_start && time < gap_end) envelope = 0.0;
        frequency = 440.0 * pow(2.0, cents / 1200.0);
        phase += 2.0 * TEST_PI * frequency / (double)sample_rate;
        samples[index] = 0.20 * envelope *
                         (sin(phase) + 0.35 * sin(2.0 * phase + 0.1) +
                          0.12 * sin(3.0 * phase - 0.2));
    }
}

static void make_glide_samples(double *samples,
                               size_t count,
                               uint32_t sample_rate,
                               double from_hz,
                               double to_hz)
{
    size_t index;
    double phase = 0.0;

    for (index = 0U; index < count; ++index) {
        double progress = count > 1U
                              ? (double)index / (double)(count - 1U)
                              : 0.0;
        double frequency = from_hz * pow(to_hz / from_hz, progress);
        double envelope = 1.0;
        if (progress < 0.02) envelope = progress / 0.02;
        if (progress > 0.98) envelope = (1.0 - progress) / 0.02;
        phase += 2.0 * TEST_PI * frequency / (double)sample_rate;
        samples[index] = 0.20 * envelope *
                         (sin(phase) + 0.40 * sin(2.0 * phase + 0.15) +
                          0.15 * sin(3.0 * phase - 0.2));
    }
}

static void test_known_note_and_block_invariance(void)
{
    const uint32_t sample_rate = 16000U;
    const size_t frame_count = 16000U;
    HWAItemEvent events[1];
    HWAItem items[4];
    HWAItemMember members[4];
    HWAItemSet set;
    HWAMeasurementOptions first_options = test_options();
    HWAMeasurementOptions second_options = first_options;
    HWAMeasurementSet first;
    HWAMeasurementSet second;
    double *samples = (double *)calloc(frame_count, sizeof(*samples));
    char error[HWA_ERROR_SIZE];
    const HWAMeasureObservation *pitch;
    const HWAMeasureObservation *hnr;
    const HWAMeasureObservation *partial;
    const HWAMeasureObservation *vibrato_rate;
    const HWAMeasureObservation *rise90;
    const HWAMeasureObservation *decay;
    const HWAMeasureObservation *relative;
    size_t index;

    CHECK(samples != NULL, "known-note sample allocation failed");
    if (samples == NULL) return;
    make_note_samples(samples, frame_count, sample_rate, 0.25, 22.0, 5.0);
    memset(&set, 0, sizeof(set));
    set.audio_format.frames = frame_count;
    set.audio_format.sample_rate_hz = sample_rate;
    set.event_count = 1U;
    set.events = events;
    set.item_count = 4U;
    set.items = items;
    set.member_count = 4U;
    set.members = members;
    set_event(&events[0], 1U, (char *)"n1", (char *)"69",
              (char *)"solo", (char *)"mf");
    events[0].audio_start_sample = 0U;
    events[0].audio_end_sample = frame_count;
    set_item(&items[0], 1U, HWA_ITEM_NOTE, (char *)"note:1",
             (char *)"note", 0U, frame_count, 0U);
    set_item(&items[1], 2U, HWA_ITEM_ATTACK, (char *)"attack:1",
             (char *)"attack", 0U, 2400U, 1U);
    set_item(&items[2], 3U, HWA_ITEM_BODY, (char *)"body:1",
             (char *)"body", 2400U, 12800U, 1U);
    set_item(&items[3], 4U, HWA_ITEM_RELEASE, (char *)"release:1",
             (char *)"release", 12800U, frame_count, 1U);
    for (index = 0U; index < 4U; ++index) {
        set_source_member(&members[index], (uint64_t)index + 1U, 1U);
    }
    CHECK(hwa_measure_engine_samples(
              &set, samples, frame_count, sample_rate, &first_options, 0U,
              &first, error, sizeof(error)) == 0,
          error);
    pitch = find_measure(&first, 3U, HWA_MEASURE_PITCH_HZ, 0U,
                         HWA_MEASURE_VIEW_RAW);
    hnr = find_measure(&first, 3U, HWA_MEASURE_HNR_DB, 0U,
                       HWA_MEASURE_VIEW_RAW);
    partial = find_measure(&first, 3U, HWA_MEASURE_PARTIAL_LEVEL_DBFS, 2U,
                           HWA_MEASURE_VIEW_RAW);
    vibrato_rate = find_measure(&first, 3U, HWA_MEASURE_VIBRATO_RATE_HZ, 0U,
                                HWA_MEASURE_VIEW_RAW);
    rise90 = find_measure(&first, 2U, HWA_MEASURE_RISE_90_SECONDS, 0U,
                          HWA_MEASURE_VIEW_RAW);
    decay = find_measure(&first, 4U, HWA_MEASURE_DECAY_DB, 0U,
                         HWA_MEASURE_VIEW_RAW);
    relative = find_measure(&first, 3U, HWA_MEASURE_RMS_DBFS, 0U,
                            HWA_MEASURE_VIEW_LEVEL_RELATIVE);
    CHECK(pitch != NULL && pitch->status == HWA_MEASURE_STATUS_VALID &&
              fabs(pitch->value - 440.0) < 8.0,
          "known note pitch is not close to 440 Hz");
    CHECK(hnr != NULL && hnr->status == HWA_MEASURE_STATUS_VALID &&
              isfinite(hnr->value),
          "known note HNR is unavailable");
    CHECK(partial != NULL && partial->status == HWA_MEASURE_STATUS_VALID,
          "known second partial is unavailable");
    CHECK(vibrato_rate != NULL &&
              vibrato_rate->status == HWA_MEASURE_STATUS_VALID &&
              fabs(vibrato_rate->value - 5.0) < 1.2,
          "known 5 Hz vibrato was not recovered");
    CHECK(rise90 != NULL && rise90->status == HWA_MEASURE_STATUS_VALID &&
              rise90->value > 0.04 && rise90->value < 0.16,
          "attack rise time is outside the synthetic ramp");
    CHECK(decay != NULL && decay->status == HWA_MEASURE_STATUS_VALID &&
              decay->value > 3.0,
          "release decay was not measured");
    CHECK(relative != NULL && relative->status == HWA_MEASURE_STATUS_VALID &&
              fabs(relative->value) < 0.1 &&
              relative->unit == HWA_MEASURE_UNIT_DB,
          "body level-relative RMS did not use its performance reference");
    CHECK(first.capability_flags == 0U && first.warning_count == 1U,
          "Stage 6/probe capability warning is missing");

    second_options.decode_block_frames = 509U;
    CHECK(hwa_measure_engine_samples(
              &set, samples, frame_count, sample_rate, &second_options, 0U,
              &second, error, sizeof(error)) == 0,
          error);
    CHECK(first.measurement_count == second.measurement_count,
          "decode block changed the measurement count");
    if (first.measurement_count == second.measurement_count) {
        for (index = 0U; index < first.measurement_count; ++index) {
            const HWAMeasureObservation *a = &first.measurements[index];
            const HWAMeasureObservation *b = &second.measurements[index];
            CHECK(a->item_id == b->item_id && a->kind == b->kind &&
                      a->index == b->index && a->view == b->view &&
                      a->status == b->status && a->value == b->value &&
                      a->confidence == b->confidence,
                  "decode block changed a scalar observation");
        }
    }
    hwa_measurement_set_free(&second);
    hwa_measurement_set_free(&first);
    free(samples);
}

static void test_consensus_and_multi_pitch(void)
{
    const uint32_t sample_rate = 16000U;
    const size_t frame_count = 8000U;
    HWAItemEvent events[2];
    HWAItem items[3];
    HWAItemMember members[6];
    HWAItemSet set;
    HWAMeasurementOptions options = test_options();
    HWAMeasurementSet result;
    double *samples = (double *)calloc(frame_count, sizeof(*samples));
    char error[HWA_ERROR_SIZE];
    const HWAMeasureObservation *pitch;
    const HWAMeasureObservation *harmonic;
    const HWAMeasureObservation *composite_pitch;
    const HWAMeasureObservation *transition_pitch;

    CHECK(samples != NULL, "multi-pitch sample allocation failed");
    if (samples == NULL) return;
    make_note_samples(samples, frame_count, sample_rate, 0.2, 0.0, 5.0);
    memset(&set, 0, sizeof(set));
    set.audio_format.frames = frame_count;
    set.audio_format.sample_rate_hz = sample_rate;
    set.events = events;
    set.event_count = 2U;
    set.items = items;
    set.item_count = 3U;
    set.members = members;
    set.member_count = 6U;
    set_event(&events[0], 1U, (char *)"a", (char *)"69",
              (char *)"upper", (char *)"mf");
    set_event(&events[1], 2U, (char *)"b", (char *)"76",
              (char *)"lower", (char *)"ff");
    set_item(&items[0], 1U, HWA_ITEM_NOTE, (char *)"note:a",
             (char *)"note", 0U, frame_count, 0U);
    set_item(&items[1], 2U, HWA_ITEM_MULTI_NOTE, (char *)"multi:a:b",
             (char *)"chord", 0U, frame_count, 0U);
    set_item(&items[2], 3U, HWA_ITEM_TRANSITION,
             (char *)"transition:a:b", (char *)"transition", 0U,
             frame_count, 0U);
    set_source_member(&members[0], 1U, 1U);
    members[1].item_id = 2U;
    members[1].event_id = 1U;
    members[1].role = HWA_ITEM_MEMBER_ACTIVE;
    members[2].item_id = 2U;
    members[2].event_id = 2U;
    members[2].role = HWA_ITEM_MEMBER_ACTIVE;
    members[3].item_id = 2U;
    members[3].event_id = 1U;
    members[3].role = HWA_ITEM_MEMBER_SOURCE;
    set_member(&members[4], 3U, 1U, HWA_ITEM_MEMBER_FROM);
    set_member(&members[5], 3U, 2U, HWA_ITEM_MEMBER_TO);
    CHECK(hwa_measure_engine_samples(
              &set, samples, frame_count, sample_rate, &options, 0U,
              &result, error, sizeof(error)) == 0,
          error);
    pitch = find_measure(&result, 1U, HWA_MEASURE_PITCH_HZ, 0U,
                         HWA_MEASURE_VIEW_RAW);
    CHECK(pitch != NULL && pitch->status == HWA_MEASURE_STATUS_MULTI_PITCH,
          "active chord membership did not block per-note pitch facts");
    harmonic = find_measure(&result, 1U, HWA_MEASURE_HARMONIC_LEVEL_DBFS,
                            0U, HWA_MEASURE_VIEW_RAW);
    CHECK(harmonic != NULL &&
              harmonic->status == HWA_MEASURE_STATUS_MULTI_PITCH,
          "active chord did not mark harmonic rows as multi-pitch");
    transition_pitch = find_measure(
        &result, 3U, HWA_MEASURE_TRANSITION_PITCH_CHANGE_CENTS, 0U,
        HWA_MEASURE_VIEW_RAW);
    CHECK(transition_pitch != NULL &&
              transition_pitch->status == HWA_MEASURE_STATUS_MULTI_PITCH,
          "a chord endpoint enabled isolated transition pitch facts");
    CHECK(result.contexts[1].labels.part == NULL &&
              result.contexts[1].labels.dynamic == NULL &&
              result.contexts[1].labels.pitch == NULL,
          "mixed member labels leaked into a grouped item context");
    hwa_measurement_set_free(&result);

    items[1].role = (char *)"multi-note";
    CHECK(hwa_measure_engine_samples(
              &set, samples, frame_count, sample_rate, &options, 0U,
              &result, error, sizeof(error)) == 0,
          error);
    pitch = find_measure(&result, 1U, HWA_MEASURE_PITCH_HZ, 0U,
                         HWA_MEASURE_VIEW_RAW);
    composite_pitch = find_measure(&result, 2U, HWA_MEASURE_PITCH_HZ, 0U,
                                   HWA_MEASURE_VIEW_RAW);
    CHECK(pitch != NULL && pitch->status == HWA_MEASURE_STATUS_VALID,
          "ordinary overlap blocked a single-source note pitch");
    CHECK(composite_pitch != NULL &&
              composite_pitch->status == HWA_MEASURE_STATUS_MULTI_PITCH,
          "multi-note composite exposed an isolated pitch fact");
    hwa_measurement_set_free(&result);
    free(samples);
}

static void test_interleaved_parts(void)
{
    const uint32_t sample_rate = 16000U;
    const size_t note_frames = 4800U;
    const size_t frame_count = note_frames * 3U;
    HWAItemEvent events[3];
    HWAItem items[6];
    HWAItemMember members[6];
    HWAItemSet set;
    HWAMeasurementOptions options = test_options();
    HWAMeasurementSet result;
    double *samples = (double *)calloc(frame_count, sizeof(*samples));
    char error[HWA_ERROR_SIZE];
    const HWAMeasureObservation *contrast;
    size_t index;

    CHECK(samples != NULL, "interleaved-parts sample allocation failed");
    if (samples == NULL) return;
    make_note_samples(samples, note_frames, sample_rate, 0.10, 0.0, 5.0);
    make_note_samples(samples + note_frames, note_frames, sample_rate,
                      0.70, 0.0, 5.0);
    make_note_samples(samples + note_frames * 2U, note_frames, sample_rate,
                      0.20, 0.0, 5.0);
    memset(&set, 0, sizeof(set));
    set.audio_format.frames = frame_count;
    set.audio_format.sample_rate_hz = sample_rate;
    set.events = events;
    set.event_count = 3U;
    set.items = items;
    set.item_count = 6U;
    set.members = members;
    set.member_count = 6U;
    set_event(&events[0], 1U, (char *)"a1", (char *)"69",
              (char *)"part-a", (char *)"mf");
    set_event(&events[1], 2U, (char *)"b1", (char *)"69",
              (char *)"part-b", (char *)"mf");
    set_event(&events[2], 3U, (char *)"a2", (char *)"69",
              (char *)"part-a", (char *)"mf");
    for (index = 0U; index < 3U; ++index) {
        uint64_t start = (uint64_t)index * note_frames;
        uint64_t end = start + note_frames;
        set_item(&items[index], (uint64_t)index + 1U, HWA_ITEM_NOTE,
                 index == 0U ? (char *)"note:a1"
                             : index == 1U ? (char *)"note:b1"
                                           : (char *)"note:a2",
                 (char *)"note", start, end, 0U);
        set_item(&items[index + 3U], (uint64_t)index + 4U, HWA_ITEM_ATTACK,
                 index == 0U ? (char *)"attack:a1"
                             : index == 1U ? (char *)"attack:b1"
                                           : (char *)"attack:a2",
                 (char *)"attack", start, start + 1600U,
                 (uint64_t)index + 1U);
        set_source_member(&members[index], (uint64_t)index + 1U,
                          (uint64_t)index + 1U);
        set_source_member(&members[index + 3U], (uint64_t)index + 4U,
                          (uint64_t)index + 1U);
    }
    CHECK(hwa_measure_engine_samples(
              &set, samples, frame_count, sample_rate, &options, 0U,
              &result, error, sizeof(error)) == 0,
          error);
    contrast = find_measure(&result, 3U, HWA_MEASURE_LOCAL_CONTRAST_DB, 0U,
                            HWA_MEASURE_VIEW_RAW);
    CHECK(contrast != NULL && contrast->status == HWA_MEASURE_STATUS_VALID &&
              contrast->value > 4.0 && contrast->value < 8.0,
          "interleaved part changed the part-a local contrast reference");
    hwa_measurement_set_free(&result);
    free(samples);
}

static void test_no_body_level_reference(void)
{
    const uint32_t sample_rate = 16000U;
    const size_t frame_count = 8000U;
    HWAItemEvent event;
    HWAItem item;
    HWAItemMember member;
    HWAItemSet set;
    HWAMeasurementOptions options = test_options();
    HWAMeasurementSet result;
    double *samples = (double *)calloc(frame_count, sizeof(*samples));
    char error[HWA_ERROR_SIZE];
    const HWAMeasureObservation *raw;
    const HWAMeasureObservation *relative;

    CHECK(samples != NULL, "no-body sample allocation failed");
    if (samples == NULL) return;
    make_note_samples(samples, frame_count, sample_rate, 0.2, 0.0, 5.0);
    memset(&set, 0, sizeof(set));
    set.audio_format.frames = frame_count;
    set.audio_format.sample_rate_hz = sample_rate;
    set.events = &event;
    set.event_count = 1U;
    set.items = &item;
    set.item_count = 1U;
    set.members = &member;
    set.member_count = 1U;
    set_event(&event, 1U, (char *)"n", (char *)"69", (char *)"solo",
              (char *)"mf");
    set_item(&item, 1U, HWA_ITEM_NOTE, (char *)"note:n", (char *)"note",
             0U, frame_count, 0U);
    set_source_member(&member, 1U, 1U);
    CHECK(hwa_measure_engine_samples(
              &set, samples, frame_count, sample_rate, &options, 0U,
              &result, error, sizeof(error)) == 0,
          error);
    raw = find_measure(&result, 1U, HWA_MEASURE_RMS_DBFS, 0U,
                       HWA_MEASURE_VIEW_RAW);
    relative = find_measure(&result, 1U, HWA_MEASURE_RMS_DBFS, 0U,
                            HWA_MEASURE_VIEW_LEVEL_RELATIVE);
    CHECK(result.level_reference_valid == 0 &&
              result.level_reference_item_count == 0U,
          "a NOTE row was used as a fallback level reference");
    CHECK(raw != NULL && raw->status == HWA_MEASURE_STATUS_VALID,
          "raw note RMS was lost without a BODY reference");
    CHECK(relative != NULL &&
              relative->status == HWA_MEASURE_STATUS_NO_REFERENCE,
          "relative note RMS did not report a missing BODY reference");
    hwa_measurement_set_free(&result);
    free(samples);
}

static void test_level_reference_skips_low_confidence_body(void)
{
    const uint32_t sample_rate = 16000U;
    const size_t span = 4000U;
    const size_t frame_count = span * 4U;
    const double amplitudes[4] = {0.10, 0.20, 0.40, 0.90};
    HWAItemEvent events[4];
    HWAItem items[4];
    HWAItemMember members[4];
    HWAItemSet set;
    HWAMeasurementOptions options = test_options();
    HWAMeasurementSet result;
    double *samples = (double *)calloc(frame_count, sizeof(*samples));
    char error[HWA_ERROR_SIZE];
    size_t item_index;

    CHECK(samples != NULL, "level-reference sample allocation failed");
    if (samples == NULL) return;
    for (item_index = 0U; item_index < 4U; ++item_index) {
        size_t sample;
        for (sample = 0U; sample < span; ++sample) {
            samples[item_index * span + sample] =
                amplitudes[item_index] *
                sin(2.0 * TEST_PI * 440.0 * (double)sample /
                    (double)sample_rate);
        }
    }
    memset(&set, 0, sizeof(set));
    set.audio_format.frames = frame_count;
    set.audio_format.sample_rate_hz = sample_rate;
    set.events = events;
    set.event_count = 4U;
    set.items = items;
    set.item_count = 4U;
    set.members = members;
    set.member_count = 4U;
    for (item_index = 0U; item_index < 4U; ++item_index) {
        uint64_t start = (uint64_t)item_index * span;
        set_event(&events[item_index], (uint64_t)item_index + 1U,
                  (char *)"n", (char *)"69", (char *)"solo",
                  (char *)"mf");
        set_item(&items[item_index], (uint64_t)item_index + 1U,
                 HWA_ITEM_BODY, (char *)"body:n", (char *)"body", start,
                 start + span, 0U);
        set_source_member(&members[item_index],
                          (uint64_t)item_index + 1U,
                          (uint64_t)item_index + 1U);
    }
    items[3].quality_flags |= HWA_ITEM_QUALITY_LOW_CONFIDENCE;
    CHECK(hwa_measure_engine_samples(
              &set, samples, frame_count, sample_rate, &options, 0U,
              &result, error, sizeof(error)) == 0,
          error);
    CHECK(result.level_reference_valid != 0 &&
              result.level_reference_item_count == 3U,
          "low-confidence BODY entered the level reference");
    CHECK(fabs(result.level_reference_dbfs -
                   20.0 * log10(0.20 / sqrt(2.0))) < 0.05,
          "BODY level reference is not the odd median in dBFS");
    hwa_measurement_set_free(&result);
    free(samples);
}

static void test_vibrato_delay_and_missing_pitch(void)
{
    const uint32_t sample_rate = 16000U;
    const size_t frame_count = 19200U;
    HWAItemEvent event;
    HWAItem item;
    HWAItemMember member;
    HWAItemSet set;
    HWAMeasurementOptions options = test_options();
    HWAMeasurementSet result;
    double *samples = (double *)calloc(frame_count, sizeof(*samples));
    char error[HWA_ERROR_SIZE];
    const HWAMeasureObservation *delay;
    const HWAMeasureObservation *rate;

    CHECK(samples != NULL, "vibrato sample allocation failed");
    if (samples == NULL) return;
    memset(&set, 0, sizeof(set));
    set.audio_format.frames = frame_count;
    set.audio_format.sample_rate_hz = sample_rate;
    set.events = &event;
    set.event_count = 1U;
    set.items = &item;
    set.item_count = 1U;
    set.members = &member;
    set.member_count = 1U;
    set_event(&event, 1U, (char *)"n", (char *)"69", (char *)"solo",
              (char *)"mf");
    set_item(&item, 1U, HWA_ITEM_BODY, (char *)"body:n", (char *)"body",
             0U, frame_count, 0U);
    set_source_member(&member, 1U, 1U);

    make_delayed_vibrato_samples(samples, frame_count, sample_rate, 20.0,
                                 0.45, 22.0, 5.0, 2.0, 2.0);
    CHECK(hwa_measure_engine_samples(
              &set, samples, frame_count, sample_rate, &options, 0U,
              &result, error, sizeof(error)) == 0,
          error);
    delay = find_measure(&result, 1U, HWA_MEASURE_VIBRATO_DELAY_SECONDS,
                         0U, HWA_MEASURE_VIEW_RAW);
    rate = find_measure(&result, 1U, HWA_MEASURE_VIBRATO_RATE_HZ, 0U,
                        HWA_MEASURE_VIEW_RAW);
    CHECK(rate != NULL && rate->status == HWA_MEASURE_STATUS_VALID &&
              fabs(rate->value - 5.0) < 1.2,
          "delayed 5 Hz vibrato was not recovered");
    CHECK(delay != NULL && delay->status == HWA_MEASURE_STATUS_VALID &&
              delay->value > 0.15 && delay->value < 0.60,
          "constant tuning offset caused an early vibrato onset");
    hwa_measurement_set_free(&result);

    memset(samples, 0, frame_count * sizeof(*samples));
    make_delayed_vibrato_samples(samples, frame_count, sample_rate, 20.0,
                                 0.20, 22.0, 5.0, 0.50, 0.64);
    CHECK(hwa_measure_engine_samples(
              &set, samples, frame_count, sample_rate, &options, 0U,
              &result, error, sizeof(error)) == 0,
          error);
    rate = find_measure(&result, 1U, HWA_MEASURE_VIBRATO_RATE_HZ, 0U,
                        HWA_MEASURE_VIEW_RAW);
    CHECK(rate != NULL && rate->status == HWA_MEASURE_STATUS_NO_PITCH,
          "a pitch gap was treated as a uniform vibrato series");
    hwa_measurement_set_free(&result);
    free(samples);
}

static void test_measured_transition_pitch(void)
{
    const uint32_t sample_rate = 16000U;
    const size_t frame_count = 16000U;
    HWAItemEvent events[2];
    HWAItem item;
    HWAItemMember members[2];
    HWAItemSet set;
    HWAMeasurementOptions options = test_options();
    HWAMeasurementSet result;
    double *samples = (double *)calloc(frame_count, sizeof(*samples));
    char error[HWA_ERROR_SIZE];
    const HWAMeasureObservation *glide;
    const HWAMeasureObservation *linearity;
    const HWAMeasureObservation *change;
    const HWAMeasureObservation *settle;

    CHECK(samples != NULL, "transition sample allocation failed");
    if (samples == NULL) return;
    make_glide_samples(samples, frame_count, sample_rate, 440.0,
                       440.0 * pow(2.0, 2.0 / 12.0));
    memset(&set, 0, sizeof(set));
    set.audio_format.frames = frame_count;
    set.audio_format.sample_rate_hz = sample_rate;
    set.events = events;
    set.event_count = 2U;
    set.items = &item;
    set.item_count = 1U;
    set.members = members;
    set.member_count = 2U;
    set_event(&events[0], 1U, (char *)"from", (char *)"69",
              (char *)"solo", (char *)"mf");
    set_event(&events[1], 2U, (char *)"to", (char *)"71",
              (char *)"solo", (char *)"mf");
    events[0].audio_start_sample = 0U;
    events[0].audio_end_sample = frame_count / 2U;
    events[1].audio_start_sample = frame_count / 2U;
    events[1].audio_end_sample = frame_count;
    set_item(&item, 1U, HWA_ITEM_TRANSITION, (char *)"transition:from:to",
             (char *)"transition", 0U, frame_count, 0U);
    set_member(&members[0], 1U, 1U, HWA_ITEM_MEMBER_FROM);
    set_member(&members[1], 1U, 2U, HWA_ITEM_MEMBER_TO);
    CHECK(hwa_measure_engine_samples(
              &set, samples, frame_count, sample_rate, &options, 0U,
              &result, error, sizeof(error)) == 0,
          error);
    glide = find_measure(&result, 1U, HWA_MEASURE_GLIDE_TIME_SECONDS, 0U,
                         HWA_MEASURE_VIEW_RAW);
    linearity = find_measure(&result, 1U, HWA_MEASURE_PORTAMENTO_LINEARITY,
                             0U, HWA_MEASURE_VIEW_RAW);
    change = find_measure(
        &result, 1U, HWA_MEASURE_TRANSITION_PITCH_CHANGE_CENTS, 0U,
        HWA_MEASURE_VIEW_RAW);
    CHECK(change != NULL && change->status == HWA_MEASURE_STATUS_VALID &&
              change->value > 150.0 && change->value < 250.0,
          "transition pitch change was not measured from the audio path");
    CHECK(glide != NULL && glide->status == HWA_MEASURE_STATUS_VALID &&
              glide->value > 0.60 && glide->value < 0.95,
          "transition 10-90 percent glide time is wrong");
    CHECK(linearity != NULL &&
              linearity->status == HWA_MEASURE_STATUS_VALID &&
              linearity->value > 0.85,
          "linear transition did not yield a high path R-squared value");
    CHECK(result.contexts[0].labels.part == NULL &&
              result.contexts[0].labels.pitch == NULL,
          "FROM/TO members introduced typed group labels");
    hwa_measurement_set_free(&result);

    events[1].midi_note = (char *)"69";
    events[1].labels.pitch = (char *)"69";
    make_glide_samples(samples, frame_count, sample_rate, 440.0, 440.0);
    CHECK(hwa_measure_engine_samples(
              &set, samples, frame_count, sample_rate, &options, 0U,
              &result, error, sizeof(error)) == 0,
          error);
    glide = find_measure(&result, 1U, HWA_MEASURE_GLIDE_TIME_SECONDS, 0U,
                         HWA_MEASURE_VIEW_RAW);
    linearity = find_measure(&result, 1U, HWA_MEASURE_PORTAMENTO_LINEARITY,
                             0U, HWA_MEASURE_VIEW_RAW);
    settle = find_measure(&result, 1U, HWA_MEASURE_PITCH_SETTLE_SECONDS, 0U,
                          HWA_MEASURE_VIEW_RAW);
    CHECK(glide != NULL &&
              glide->status == HWA_MEASURE_STATUS_UNSUPPORTED_ITEM &&
              linearity != NULL &&
              linearity->status == HWA_MEASURE_STATUS_UNSUPPORTED_ITEM,
          "same-pitch transition exposed a false glide fact");
    CHECK(settle != NULL &&
              settle->status == HWA_MEASURE_STATUS_UNSUPPORTED_ITEM,
          "moving-guide transition exposed a false generic settle fact");
    hwa_measurement_set_free(&result);
    free(samples);
}

static void test_silent_status_precedence(void)
{
    const uint32_t sample_rate = 16000U;
    const size_t frame_count = 4096U;
    HWAItemEvent event;
    HWAItem item;
    HWAItemMember member;
    HWAItemSet set;
    HWAMeasurementOptions options = test_options();
    HWAMeasurementSet result;
    double samples[frame_count];
    char error[HWA_ERROR_SIZE];
    const HWAMeasureObservation *rms;
    const HWAMeasureObservation *relative;
    const HWAMeasureObservation *centroid;
    const HWAMeasureObservation *pitch;
    const HWAMeasureObservation *coupling;

    memset(samples, 0, sizeof(samples));
    memset(&set, 0, sizeof(set));
    set.audio_format.frames = frame_count;
    set.audio_format.sample_rate_hz = sample_rate;
    set.events = &event;
    set.event_count = 1U;
    set.items = &item;
    set.item_count = 1U;
    set.members = &member;
    set.member_count = 1U;
    set_event(&event, 1U, (char *)"silent", (char *)"69",
              (char *)"solo", (char *)"pp");
    set_item(&item, 1U, HWA_ITEM_BODY, (char *)"body:silent",
             (char *)"body", 0U, frame_count, 0U);
    set_source_member(&member, 1U, 1U);
    CHECK(hwa_measure_engine_samples(
              &set, samples, frame_count, sample_rate, &options, 0U,
              &result, error, sizeof(error)) == 0,
          error);
    rms = find_measure(&result, 1U, HWA_MEASURE_RMS_DBFS, 0U,
                       HWA_MEASURE_VIEW_RAW);
    relative = find_measure(&result, 1U, HWA_MEASURE_RMS_DBFS, 0U,
                            HWA_MEASURE_VIEW_LEVEL_RELATIVE);
    centroid = find_measure(&result, 1U, HWA_MEASURE_CENTROID_HZ, 0U,
                            HWA_MEASURE_VIEW_RAW);
    pitch = find_measure(&result, 1U, HWA_MEASURE_PITCH_HZ, 0U,
                         HWA_MEASURE_VIEW_RAW);
    coupling = find_measure(&result, 1U,
                            HWA_MEASURE_PITCH_LEVEL_CORRELATION, 0U,
                            HWA_MEASURE_VIEW_RAW);
    CHECK(rms != NULL && rms->status == HWA_MEASURE_STATUS_BELOW_FLOOR &&
              relative != NULL &&
              relative->status == HWA_MEASURE_STATUS_BELOW_FLOOR,
          "missing BODY reference hid the raw silence status");
    CHECK(centroid != NULL &&
              centroid->status == HWA_MEASURE_STATUS_BELOW_FLOOR &&
              pitch != NULL &&
              pitch->status == HWA_MEASURE_STATUS_BELOW_FLOOR &&
              coupling != NULL &&
              coupling->status == HWA_MEASURE_STATUS_BELOW_FLOOR,
          "frame or pitch faults took precedence over silence");
    hwa_measurement_set_free(&result);
}

static void test_canonical_item_file_and_stereo_wav(void)
{
    const uint32_t sample_rate = 16000U;
    const uint32_t frame_count = 4096U;
    char directory[PATH_MAX];
    char audio_path[PATH_MAX];
    char items_path[PATH_MAX];
    HWAItemEvent event;
    HWAItem items[2];
    HWAItemMember members[2];
    HWAItemSet set;
    HWAMeasurementOptions options = test_options();
    HWAMeasurementSet result;
    FILE *stream = NULL;
    char error[HWA_ERROR_SIZE];
    int workspace_ready = 0;
    int audio_ready = 0;
    int items_ready = 0;
    int okay;

    okay = test_workspace(directory);
    CHECK(okay, "cannot make API test workspace");
    if (!okay) return;
    workspace_ready = 1;
    okay = test_path(audio_path, directory, "played.wav") &&
           test_path(items_path, directory, "played.hwa-items");
    CHECK(okay, "cannot make API test paths");
    if (!okay) {
        goto cleanup;
    }
    okay = test_write_stereo_wav(audio_path, sample_rate, frame_count);
    CHECK(okay, "cannot write stereo API fixture");
    if (!okay) {
        goto cleanup;
    }
    audio_ready = 1;

    memset(&set, 0, sizeof(set));
    hwa_segmentation_options_default(&set.options);
    set.alignment_path = (char *)"fixture.hwa-align";
    set.audio_path = audio_path;
    set.source_score_path = (char *)"fixture.csv";
    (void)strcpy(
        set.alignment_sha256,
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    (void)strcpy(
        set.source_score_sha256,
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    okay = hwa_sha256_file(audio_path, UINT64_MAX, set.audio_sha256,
                           error, sizeof(error)) == 0;
    CHECK(okay, "cannot hash stereo API fixture");
    if (!okay) goto cleanup;
    set.audio_format.sample_rate_hz = sample_rate;
    set.audio_format.frames = frame_count;
    set.audio_format.duration_seconds =
        (double)frame_count / (double)sample_rate;
    set.audio_format.channels = 2U;
    set.audio_format.encoding = HWA_ENCODING_PCM;
    set.audio_format.bits_per_sample = 16U;
    set.audio_format.valid_bits_per_sample = 16U;
    set.source_score_duration_seconds = set.audio_format.duration_seconds;
    set.alignment_confidence = 1.0;
    set.events = &event;
    set.event_count = 1U;
    set.items = items;
    set.item_count = 2U;
    set.members = members;
    set.member_count = 2U;
    set_event(&event, 1U, (char *)"n", (char *)"69", (char *)"solo",
              (char *)"mf");
    event.score_end_beat = 1.0;
    event.score_end_seconds = set.audio_format.duration_seconds;
    event.audio_start_sample = 0U;
    event.audio_end_sample = frame_count;
    event.audio_start_seconds = 0.0;
    event.audio_end_seconds = set.audio_format.duration_seconds;
    event.alignment_status = HWA_ALIGNMENT_MATCHED;
    event.alignment_evidence_flags = HWA_ALIGNMENT_EVIDENCE_PITCH;
    set_item(&items[0], 1U, HWA_ITEM_NOTE, (char *)"note:n",
             (char *)"note", 0U, frame_count, 0U);
    items[0].start_seconds = 0.0;
    items[0].end_seconds = set.audio_format.duration_seconds;
    items[0].score_end_beat = 1.0;
    items[0].evidence_flags = HWA_ITEM_EVIDENCE_ALIGNMENT;
    items[0].origin = HWA_ITEM_ORIGIN_AUTO;
    set_item(&items[1], 2U, HWA_ITEM_BODY, (char *)"body:n",
             (char *)"body", 512U, frame_count, 1U);
    items[1].start_seconds = 512.0 / (double)sample_rate;
    items[1].end_seconds = set.audio_format.duration_seconds;
    items[1].score_end_beat = 1.0;
    items[1].evidence_flags = HWA_ITEM_EVIDENCE_ENERGY;
    items[1].origin = HWA_ITEM_ORIGIN_AUTO;
    set_source_member(&members[0], 1U, 1U);
    set_source_member(&members[1], 2U, 1U);

    stream = fopen(items_path, "wb");
    if (stream != NULL) items_ready = 1;
    okay = stream != NULL &&
           hwa_item_file_write(stream, &set, error, sizeof(error)) == 0;
    CHECK(okay, "cannot write canonical API item fixture");
    if (!okay) goto cleanup;
    okay = fclose(stream) == 0;
    if (!okay) {
        stream = NULL;
        goto cleanup;
    }
    stream = NULL;
    okay = hwa_measure_item_file_wav(
                items_path, audio_path, &options, &result,
                error, sizeof(error)) == 0;
    CHECK(okay, error);
    if (okay) {
        const HWAMeasureObservation *rms = find_measure(
            &result, 2U, HWA_MEASURE_RMS_DBFS, 0U, HWA_MEASURE_VIEW_RAW);
        CHECK(result.audio_format.channels == 2U,
              "real WAV channel facts were not kept in the result");
        CHECK(rms != NULL && rms->status == HWA_MEASURE_STATUS_VALID,
              "canonical item-file API did not measure its stereo WAV");
        hwa_measurement_set_free(&result);
    }

cleanup:
    if (stream != NULL) (void)fclose(stream);
    if (items_ready) (void)test_remove_file(items_path);
    if (audio_ready) (void)test_remove_file(audio_path);
    if (workspace_ready) (void)test_remove_directory(directory);
}

static void test_limits_fail_transactionally(void)
{
    const uint32_t sample_rate = 16000U;
    const size_t frame_count = 4096U;
    HWAItemEvent event;
    HWAItem item;
    HWAItemMember member;
    HWAItemSet set;
    HWAMeasurementOptions options = test_options();
    HWAMeasurementSet result;
    double samples[frame_count];
    char error[HWA_ERROR_SIZE];
    size_t index;
    uint64_t low;
    uint64_t high;

    memset(samples, 0, sizeof(samples));
    for (index = 0U; index < frame_count; ++index) {
        samples[index] = 0.2 * sin(2.0 * TEST_PI * 440.0 *
                                   (double)index / (double)sample_rate);
    }
    memset(&set, 0, sizeof(set));
    set.audio_format.frames = frame_count;
    set.audio_format.sample_rate_hz = sample_rate;
    set.events = &event;
    set.event_count = 1U;
    set.items = &item;
    set.item_count = 1U;
    set.members = &member;
    set.member_count = 1U;
    set_event(&event, 1U, (char *)"n", (char *)"69", (char *)"p",
              (char *)"mf");
    set_item(&item, 1U, HWA_ITEM_NOTE, (char *)"note:n", (char *)"note",
             0U, frame_count, 0U);
    set_source_member(&member, 1U, 1U);

    options.max_transforms = 1U;
    CHECK(hwa_measure_engine_samples(
              &set, samples, frame_count, sample_rate, &options, 0U,
              &result, error, sizeof(error)) != 0 &&
              result.measurements == NULL && result.contexts == NULL,
          "transform limit failure retained partial output");
    options = test_options();
    options.max_item_frame_evaluations = 1U;
    CHECK(hwa_measure_engine_samples(
              &set, samples, frame_count, sample_rate, &options, 0U,
              &result, error, sizeof(error)) != 0 &&
              result.measurements == NULL && result.contexts == NULL,
          "evaluation limit failure retained partial output");
    options = test_options();
    options.max_work_bytes = 1024U;
    CHECK(hwa_measure_engine_samples(
              &set, samples, frame_count, sample_rate, &options, 0U,
              &result, error, sizeof(error)) != 0 &&
              result.measurements == NULL && result.contexts == NULL,
          "work limit failure retained partial output");

    options = test_options();
    high = UINT64_C(4194304);
    options.max_work_bytes = high;
    CHECK(hwa_measure_engine_samples(
              &set, samples, frame_count, sample_rate, &options, 0U,
              &result, error, sizeof(error)) == 0,
          "cap search upper bound did not succeed");
    hwa_measurement_set_free(&result);
    low = 1U;
    while (low < high) {
        uint64_t middle = low + (high - low) / 2U;
        options.max_work_bytes = middle;
        if (hwa_measure_engine_samples(
                &set, samples, frame_count, sample_rate, &options, 0U,
                &result, error, sizeof(error)) == 0) {
            hwa_measurement_set_free(&result);
            high = middle;
        } else {
            CHECK(result.measurements == NULL && result.contexts == NULL &&
                      result.warnings == NULL,
                  "near-success work cap retained partial output");
            low = middle + 1U;
        }
    }
    CHECK(low > 1U, "cap search did not find a failure boundary");
    if (low > 1U) {
        options.max_work_bytes = low - 1U;
        CHECK(hwa_measure_engine_samples(
                  &set, samples, frame_count, sample_rate, &options, 0U,
                  &result, error, sizeof(error)) != 0 &&
                  result.measurements == NULL && result.contexts == NULL &&
                  result.warnings == NULL,
              "last allocation failure retained a partial warning");
    }
}

int main(void)
{
    test_known_note_and_block_invariance();
    test_consensus_and_multi_pitch();
    test_interleaved_parts();
    test_no_body_level_reference();
    test_level_reference_skips_low_confidence_body();
    test_vibrato_delay_and_missing_pitch();
    test_measured_transition_pitch();
    test_silent_status_precedence();
    test_canonical_item_file_and_stereo_wav();
    test_limits_fail_transactionally();
    if (failures != 0) {
        fprintf(stderr, "%d measurement-engine test(s) failed\n", failures);
        return 1;
    }
    puts("measurement engine tests passed");
    return 0;
}

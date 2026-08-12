#include "segmentation.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition, ...)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);       \
            (void)fprintf(stderr, __VA_ARGS__);                              \
            (void)fputc('\n', stderr);                                       \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void make_match(HWAAlignmentMatch *match,
                       uint64_t id,
                       const char *event_id,
                       const char *kind,
                       const char *voice,
                       const char *midi,
                       const char *tie,
                       double start,
                       double end)
{
    memset(match, 0, sizeof(*match));
    match->id = id;
    match->reference_start_seconds = start;
    match->reference_end_seconds = end;
    match->target_start_seconds = start;
    match->target_end_seconds = end;
    match->score_start_beat = start * 2.0;
    match->score_end_beat = end * 2.0;
    match->confidence = 0.90;
    match->evidence_flags = HWA_ALIGNMENT_EVIDENCE_CHROMA |
                            HWA_ALIGNMENT_EVIDENCE_ENVELOPE;
    match->status = strcmp(kind, "rest") == 0
                        ? HWA_ALIGNMENT_REST
                        : strcmp(kind, "ornament") == 0
                              ? HWA_ALIGNMENT_ORNAMENT
                              : strcmp(kind, "cadenza") == 0
                                    ? HWA_ALIGNMENT_CADENZA
                                    : HWA_ALIGNMENT_MATCHED;
    match->score_span_valid = 1;
    match->event_id = (char *)event_id;
    match->kind = (char *)kind;
    match->voice = (char *)voice;
    match->midi_note = (char *)midi;
    match->velocity = (char *)"80";
    match->tie = (char *)tie;
    match->dynamic = (char *)"mf";
    match->mark = (char *)"";
    match->score_position = (char *)"m1";
    match->tempo_bpm = 120.0;
    match->tempo_valid = 1;
}

static void make_fixture(HWAAlignment *alignment,
                         HWAAnalysis *analysis,
                         HWAAlignmentMatch matches[7],
                         HWAFrameMetrics tracks[80])
{
    size_t index;

    memset(alignment, 0, sizeof(*alignment));
    memset(analysis, 0, sizeof(*analysis));
    memset(tracks, 0, 80U * sizeof(*tracks));
    make_match(&matches[0], 1U, "n1", "note", "v1", "60", "start",
               0.20, 0.80);
    make_match(&matches[1], 2U, "n2", "note", "v1", "60", "stop",
               0.80, 1.20);
    make_match(&matches[2], 3U, "n3", "note", "v1", "64", "none",
               1.20, 2.00);
    make_match(&matches[3], 4U, "n4", "note", "v1", "67", "none",
               1.20, 1.80);
    make_match(&matches[4], 5U, "r1", "rest", "v1", "", "none",
               1.30, 1.60);
    make_match(&matches[5], 6U, "o1", "ornament", "v1", "69", "none",
               2.50, 3.00);
    make_match(&matches[6], 7U, "c1", "cadenza", "v1", "", "none",
               3.00, 3.50);
    alignment->mode = HWA_ALIGNMENT_SCORE_TO_AUDIO;
    alignment->matches = matches;
    alignment->match_count = 7U;
    alignment->reference_duration_seconds = 3.5;
    alignment->target_duration_seconds = 4.0;
    alignment->global_confidence = 0.85;
    alignment->score_path = (char *)"score.csv";
    (void)strcpy(alignment->score_sha256,
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

    analysis->format.sample_rate_hz = 1000U;
    analysis->format.frames = 4000U;
    analysis->format.duration_seconds = 4.0;
    analysis->options.frame_size = 100U;
    analysis->options.hop_size = 50U;
    analysis->options.silence_threshold_dbfs = -60.0;
    analysis->tracks = tracks;
    analysis->track_count = 80U;
    for (index = 0U; index < 80U; ++index) {
        HWAFrameMetrics *frame = &tracks[index];
        uint64_t sample = (uint64_t)index * 50U + 50U;
        frame->time_seconds = (double)sample / 1000.0;
        frame->rms_dbfs = sample < 200U || sample >= 3500U ? -80.0 : -12.0;
        frame->pitch_hz = sample < 1200U ? 261.625565 :
                          sample < 2000U ? 329.627557 : 440.0;
        frame->pitch_confidence = 0.90;
        frame->pitch_valid = 1;
        frame->pitch_change_valid = 1;
    }
    tracks[3].combined_onset_strength = 1.0;  /* sample 200 */
    tracks[3].energy_onset_strength = 1.0;
    tracks[23].combined_onset_strength = 0.90; /* sample 1200 */
    tracks[23].energy_onset_strength = 0.80;
    tracks[23].pitch_change_strength = 1.0;
    tracks[49].combined_onset_strength = 0.80; /* ornament */
    tracks[59].combined_onset_strength = 0.75; /* cadenza */
    for (index = 38U; index < 42U; ++index) {
        tracks[index].rms_dbfs = -30.0 - (double)(index - 38U) * 10.0;
    }
}

static const HWAItem *find_key(const HWAItemSet *items, const char *key)
{
    size_t index;
    for (index = 0U; index < items->item_count; ++index) {
        if (strcmp(items->items[index].key, key) == 0) {
            return &items->items[index];
        }
    }
    return NULL;
}

static size_t count_kind(const HWAItemSet *items, HWAItemKind kind)
{
    size_t index;
    size_t count = 0U;
    for (index = 0U; index < items->item_count; ++index) {
        if (items->items[index].kind == kind) count++;
    }
    return count;
}

static size_t count_role(const HWAItemSet *items, const char *role)
{
    size_t index;
    size_t count = 0U;
    for (index = 0U; index < items->item_count; ++index) {
        if (strcmp(items->items[index].role, role) == 0) count++;
    }
    return count;
}

static const HWAItem *find_role(const HWAItemSet *items, const char *role)
{
    size_t index;

    for (index = 0U; index < items->item_count; ++index) {
        if (strcmp(items->items[index].role, role) == 0) {
            return &items->items[index];
        }
    }
    return NULL;
}

static uint64_t string_bytes(const char *text)
{
    return text != NULL ? (uint64_t)strlen(text) + 1U : 0U;
}

static size_t grown_capacity(size_t count, size_t first, size_t limit)
{
    size_t capacity = 0U;

    while (capacity < count) {
        size_t next = capacity == 0U ? first : capacity * 2U;
        if (next < capacity || next > limit) next = limit;
        capacity = next;
    }
    return capacity;
}

static uint64_t expected_retained_work(const HWAItemSet *items)
{
    uint64_t bytes = (uint64_t)items->event_count * sizeof(*items->events);
    size_t index;

    bytes += (uint64_t)grown_capacity(
        items->item_count, 64U, items->options.max_items) *
        sizeof(*items->items);
    bytes += (uint64_t)grown_capacity(
        items->member_count, 128U, items->options.max_item_members) *
        sizeof(*items->members);
    bytes += (uint64_t)grown_capacity(
        items->warning_count, 16U, items->options.max_items) *
        sizeof(*items->warnings);
    bytes += string_bytes(items->alignment_path) +
             string_bytes(items->audio_path) +
             string_bytes(items->labels_path) +
             string_bytes(items->amendment_path) +
             string_bytes(items->source_score_path);
    for (index = 0U; index < items->event_count; ++index) {
        const HWAItemEvent *event = &items->events[index];
        const HWATypedLabels *labels = &event->labels;
        bytes += string_bytes(event->event_id) + string_bytes(event->kind) +
                 string_bytes(event->voice) + string_bytes(event->midi_note) +
                 string_bytes(event->velocity) + string_bytes(event->tie) +
                 string_bytes(event->dynamic) + string_bytes(event->mark) +
                 string_bytes(event->score_position) +
                 string_bytes(labels->pitch) +
                 string_bytes(labels->register_name) +
                 string_bytes(labels->dynamic) +
                 string_bytes(labels->articulation) +
                 string_bytes(labels->part) +
                 string_bytes(labels->physical_element) +
                 string_bytes(labels->controller) +
                 string_bytes(labels->technique) +
                 string_bytes(labels->score_section) +
                 string_bytes(labels->transition) +
                 string_bytes(labels->gesture);
    }
    for (index = 0U; index < items->item_count; ++index) {
        bytes += string_bytes(items->items[index].key) +
                 string_bytes(items->items[index].role) +
                 string_bytes(items->items[index].exclusion_reason);
    }
    for (index = 0U; index < items->warning_count; ++index) {
        bytes += string_bytes(items->warnings[index].code) +
                 string_bytes(items->warnings[index].message);
    }
    return bytes;
}

static void check_item_invariants(const HWAItemSet *items)
{
    size_t index;
    size_t other;

    for (index = 0U; index < items->item_count; ++index) {
        const HWAItem *item = &items->items[index];
        CHECK(item->id == (uint64_t)index + 1U,
              "item IDs are not sequential");
        CHECK(item->key != NULL && item->key[0] != '\0',
              "item has no stable key");
        CHECK(item->start_sample <= item->end_sample &&
                  item->end_sample <= items->audio_format.frames,
              "item sample span is invalid");
        CHECK(fabs(item->start_seconds -
                   (double)item->start_sample /
                       (double)items->audio_format.sample_rate_hz) < 1e-12,
              "item seconds do not derive from its exact sample");
        CHECK(isfinite(item->confidence) && item->confidence >= 0.0 &&
                  item->confidence <= 1.0,
              "item confidence is invalid");
        for (other = index + 1U; other < items->item_count; ++other) {
            CHECK(strcmp(item->key, items->items[other].key) != 0,
                  "duplicate stable item key '%s'", item->key);
        }
    }
    for (index = 0U; index < items->member_count; ++index) {
        const HWAItemMember *member = &items->members[index];
        CHECK(items->members[index].item_id >= 1U &&
                  items->members[index].item_id <= items->item_count,
              "member names an invalid item ID");
        CHECK(items->members[index].event_id >= 1U &&
                  items->members[index].event_id <= items->event_count,
              "member names an invalid event ID");
        if (member->item_id >= 1U && member->item_id <= items->item_count &&
            member->role == HWA_ITEM_MEMBER_ACTIVE) {
            const HWAItem *multi = &items->items[member->item_id - 1U];
            size_t note_index;
            int found_note = 0;

            if (multi->kind != HWA_ITEM_MULTI_NOTE) continue;
            for (note_index = 0U; note_index < items->item_count;
                 ++note_index) {
                const HWAItem *note = &items->items[note_index];
                size_t source_index;

                if (note->kind != HWA_ITEM_NOTE) continue;
                for (source_index = 0U;
                     source_index < items->member_count; ++source_index) {
                    const HWAItemMember *source =
                        &items->members[source_index];
                    if (source->item_id == note->id &&
                        source->event_id == member->event_id &&
                        source->role == HWA_ITEM_MEMBER_SOURCE) {
                        found_note = 1;
                        CHECK(multi->start_sample >= note->start_sample &&
                                  multi->end_sample <= note->end_sample,
                              "multi-note member is not active for its exact span");
                        break;
                    }
                }
                if (found_note != 0) break;
            }
            CHECK(found_note != 0,
                  "multi-note member has no logical source note");
        }
    }
}

static void test_score_roles_and_phases(void)
{
    HWAAlignment alignment;
    HWAAnalysis analysis;
    HWAAlignmentMatch matches[7];
    HWAFrameMetrics tracks[80];
    HWASegmentationOptions options;
    HWAItemSet items;
    char error[HWA_ERROR_SIZE];
    const HWAItem *note;
    const HWAItem *rest;
    const HWAItem *release;
    const HWAItem *tail;
    const HWAItem *next_attack;
    size_t index;
    size_t note_members = 0U;
    size_t chord_members = 0U;
    int build_result;

    make_fixture(&alignment, &analysis, matches, tracks);
    hwa_segmentation_options_default(&options);
    build_result = hwa_segmentation_build(
        &alignment, &analysis, NULL, &options, NULL, 0U,
        &items, error, sizeof(error));
    CHECK(build_result == 0,
          "segmentation failed: %s", error);
    if (build_result != 0) return;
    CHECK(items.event_count == 7U, "wrong normalized event count");
    CHECK(count_kind(&items, HWA_ITEM_NOTE) == 3U,
          "tie chain did not produce three logical notes");
    CHECK(count_kind(&items, HWA_ITEM_ATTACK) == 3U &&
              count_kind(&items, HWA_ITEM_BODY) == 3U &&
              count_kind(&items, HWA_ITEM_RELEASE) == 3U &&
              count_kind(&items, HWA_ITEM_RESIDUAL_TAIL) == 3U,
          "note phases were not emitted once per logical note");
    CHECK(count_kind(&items, HWA_ITEM_REST) == 1U,
          "score rest was not preserved");
    CHECK(count_role(&items, "ornament") == 1U &&
              count_role(&items, "cadenza") == 1U,
          "opaque score gestures were not preserved");
    CHECK(count_role(&items, "acoustic-event-candidate") >= 2U,
          "ornament/cadenza audio onsets were not kept as candidates");
    CHECK(count_role(&items, "chord") >= 1U,
          "simultaneous notes did not produce a chord item");
    CHECK(count_role(&items, "slur") == 0U,
          "engine inferred a slur without a typed label");
    CHECK(count_role(&items, "switch-event") == 0U &&
              count_role(&items, "physical-element-change") == 0U,
          "engine named a switch event without a typed label");
    note = find_key(&items, "note:6e31");
    CHECK(note != NULL, "tied note has no deterministic key");
    if (note != NULL) {
        for (index = 0U; index < items.member_count; ++index) {
            if (items.members[index].item_id == note->id &&
                items.members[index].role == HWA_ITEM_MEMBER_SOURCE) {
                note_members++;
            }
        }
        CHECK(note_members == 2U,
              "tied logical note did not retain both source events");
    }
    for (index = 0U; index < items.item_count; ++index) {
        if (strcmp(items.items[index].role, "chord") == 0) {
            size_t member;
            for (member = 0U; member < items.member_count; ++member) {
                if (items.members[member].item_id == items.items[index].id &&
                    items.members[member].role == HWA_ITEM_MEMBER_ACTIVE) {
                    chord_members++;
                }
            }
            break;
        }
    }
    CHECK(chord_members == 2U,
          "chord did not retain its two active note members");
    rest = find_key(&items, "rest:7231");
    CHECK(rest != NULL && rest->start_sample == 1300U &&
              rest->end_sample == 1600U,
          "voice rest overlapping sounding notes was changed or dropped");
    release = find_key(&items, "release:6e31");
    tail = find_key(&items, "tail:6e31");
    next_attack = find_key(&items, "attack:6e33");
    CHECK(release != NULL && next_attack != NULL &&
              release->end_sample > next_attack->start_sample,
          "release did not retain overlap with the next attack");
    CHECK(tail != NULL && next_attack != NULL &&
              tail->end_sample > next_attack->start_sample,
          "residual tail did not retain overlap with the next attack");
    CHECK(count_role(&items, "continuous-pitch-transition") == 0U,
          "polyphonic destination produced monophonic pitch-glide evidence");
    CHECK(items.boundary_evaluations > 0U &&
              items.boundary_evaluations <= options.max_boundary_evaluations,
          "boundary work was not recorded and bounded");
    check_item_invariants(&items);
    hwa_item_set_free(&items);
}

static void test_typed_gestures(void)
{
    HWAAlignment alignment;
    HWAAnalysis analysis;
    HWAAlignmentMatch matches[7];
    HWAFrameMetrics tracks[80];
    HWASegmentationOptions options;
    HWAItemSet items;
    HWATypedLabelSet labels;
    HWATypedLabelRow rows[3];
    char error[HWA_ERROR_SIZE];
    int build_result;
    const HWAItem *first_gesture;
    const HWAItem *ornament_gesture;

    make_fixture(&alignment, &analysis, matches, tracks);
    hwa_segmentation_options_default(&options);
    memset(&labels, 0, sizeof(labels));
    memset(rows, 0, sizeof(rows));
    rows[0].event_id = (char *)"n1";
    rows[0].labels.physical_element = (char *)"element-a";
    rows[0].labels.gesture = (char *)"first-note-gesture";
    rows[0].labels.override_flags = HWA_LABEL_OVERRIDE_PHYSICAL_ELEMENT |
                                    HWA_LABEL_OVERRIDE_GESTURE;
    rows[1].event_id = (char *)"n3";
    rows[1].labels.articulation = (char *)"slur";
    rows[1].labels.physical_element = (char *)"element-b";
    rows[1].labels.transition = (char *)"legato";
    rows[1].labels.gesture = (char *)"switch-event";
    rows[1].labels.override_flags = HWA_LABEL_OVERRIDE_ARTICULATION |
                                    HWA_LABEL_OVERRIDE_PHYSICAL_ELEMENT |
                                    HWA_LABEL_OVERRIDE_TRANSITION |
                                    HWA_LABEL_OVERRIDE_GESTURE;
    rows[2].event_id = (char *)"o1";
    rows[2].labels.gesture = (char *)"ornament-mark";
    rows[2].labels.override_flags = HWA_LABEL_OVERRIDE_GESTURE;
    labels.rows = rows;
    labels.row_count = 3U;
    build_result = hwa_segmentation_build(
        &alignment, &analysis, &labels, &options, NULL, 0U,
        &items, error, sizeof(error));
    CHECK(build_result == 0,
          "typed gesture segmentation failed: %s", error);
    if (build_result != 0) return;
    CHECK(count_role(&items, "slur") == 1U,
          "explicit typed slur was not emitted exactly once");
    CHECK(count_role(&items, "physical-element-change") == 1U,
          "explicit physical-element change was not emitted");
    CHECK(count_role(&items, "legato") == 1U,
          "typed transition label was not copied to the transition item");
    CHECK(count_role(&items, "switch-event") == 1U,
          "explicit typed switch event was not emitted exactly once");
    CHECK(count_role(&items, "first-note-gesture") == 1U,
          "first note lost its event-owned typed gesture");
    CHECK(count_role(&items, "ornament-mark") == 1U,
          "score ornament lost its event-owned typed gesture");
    first_gesture = find_role(&items, "first-note-gesture");
    ornament_gesture = find_role(&items, "ornament-mark");
    CHECK(first_gesture != NULL && first_gesture->parent_valid,
          "first-note typed gesture is not a child item");
    CHECK(ornament_gesture != NULL && ornament_gesture->parent_valid,
          "ornament typed gesture is not a child item");
    hwa_item_set_free(&items);
}

static void test_isolated_score_chord_once(void)
{
    HWAAlignment alignment;
    HWAAnalysis analysis;
    HWAAlignmentMatch matches[7];
    HWAFrameMetrics tracks[80];
    HWASegmentationOptions options;
    HWAItemSet items;
    char error[HWA_ERROR_SIZE];
    size_t index;
    size_t chord_members = 0U;
    int build_result;

    make_fixture(&alignment, &analysis, matches, tracks);
    make_match(&matches[0], 1U, "a", "note", "v1", "64", "none",
               1.20, 2.00);
    make_match(&matches[1], 2U, "b", "note", "v1", "67", "none",
               1.20, 1.80);
    alignment.match_count = 2U;
    hwa_segmentation_options_default(&options);
    build_result = hwa_segmentation_build(
        &alignment, &analysis, NULL, &options, NULL, 0U,
        &items, error, sizeof(error));
    CHECK(build_result == 0, "isolated chord fixture failed: %s", error);
    if (build_result != 0) return;
    CHECK(count_role(&items, "chord") == 1U,
          "isolated score chord was duplicated or lost");
    CHECK(count_role(&items, "multi-note") == 0U,
          "isolated score chord was duplicated as an acoustic overlap");
    for (index = 0U; index < items.member_count; ++index) {
        if (items.members[index].item_id != 0U &&
            items.members[index].item_id <= items.item_count &&
            strcmp(items.items[items.members[index].item_id - 1U].role,
                   "chord") == 0) {
            chord_members++;
        }
    }
    CHECK(chord_members == 2U,
          "isolated score chord has the wrong member set");
    hwa_item_set_free(&items);

    make_match(&matches[0], 1U, "a", "note", "v1", "60", "none",
               1.20, 1.30);
    make_match(&matches[1], 2U, "b", "note", "v1", "64", "none",
               1.20, 2.00);
    make_match(&matches[2], 3U, "c", "note", "v1", "67", "none",
               1.20, 2.20);
    alignment.match_count = 3U;
    build_result = hwa_segmentation_build(
        &alignment, &analysis, NULL, &options, NULL, 0U,
        &items, error, sizeof(error));
    CHECK(build_result == 0, "staggered chord fixture failed: %s", error);
    if (build_result != 0) return;
    CHECK(count_role(&items, "chord") == 1U,
          "staggered score chord was duplicated or lost");
    CHECK(count_role(&items, "multi-note") >= 1U,
          "sustaining chord subset lost its later multi-note span");
    hwa_item_set_free(&items);
}

static void test_exact_rounding_and_short_body(void)
{
    HWAAlignment alignment;
    HWAAnalysis analysis;
    HWAAlignmentMatch matches[7];
    HWAFrameMetrics tracks[80];
    HWASegmentationOptions options;
    HWAItemSet items;
    const HWAItem *body;
    char error[HWA_ERROR_SIZE];
    int build_result;

    make_fixture(&alignment, &analysis, matches, tracks);
    alignment.match_count = 1U;
    matches[0].tie = (char *)"none";
    matches[0].reference_start_seconds = 0.0;
    matches[0].reference_end_seconds = 0.01;
    matches[0].score_start_beat = 0.0;
    matches[0].score_end_beat = 0.02;
    matches[0].target_start_seconds = 0.0005;
    matches[0].target_end_seconds = 3.9995;
    hwa_segmentation_options_default(&options);
    build_result = hwa_segmentation_build(
        &alignment, &analysis, NULL, &options, NULL, 0U,
        &items, error, sizeof(error));
    CHECK(build_result == 0,
          "half-sample/EOF fixture failed: %s", error);
    if (build_result != 0) return;
    CHECK(items.events[0].audio_start_sample == 1U &&
              items.events[0].audio_end_sample == 4000U,
          "half-up sample rounding or EOF clamp changed: %llu..%llu",
          (unsigned long long)items.events[0].audio_start_sample,
          (unsigned long long)items.events[0].audio_end_sample);
    hwa_item_set_free(&items);

    matches[0].target_start_seconds = 0.20;
    matches[0].target_end_seconds = 0.21;
    build_result = hwa_segmentation_build(
        &alignment, &analysis, NULL, &options, NULL, 0U,
        &items, error, sizeof(error));
    CHECK(build_result == 0,
          "short-note fixture failed: %s", error);
    if (build_result != 0) return;
    body = find_key(&items, "body:6e31");
    CHECK(body != NULL && body->start_sample == body->end_sample &&
              (body->quality_flags & HWA_ITEM_QUALITY_COLLAPSED) != 0U,
          "short note invented a stable body");
    hwa_item_set_free(&items);

    matches[0].target_end_seconds = 4.0006;
    CHECK(hwa_segmentation_build(&alignment, &analysis, NULL, &options,
                                 NULL, 0U, &items,
                                 error, sizeof(error)) != 0,
          "out-of-file rounded sample was silently clamped");
}

static void test_determinism_and_resource_edges(void)
{
    HWAAlignment alignment;
    HWAAnalysis analysis;
    HWAAlignmentMatch matches[7];
    HWAFrameMetrics tracks[80];
    HWASegmentationOptions options;
    HWAItemSet first;
    HWAItemSet second;
    HWAItemSet probe;
    char error[HWA_ERROR_SIZE];
    size_t index;
    uint64_t low;
    uint64_t high;
    int first_result;
    int second_result;

    make_fixture(&alignment, &analysis, matches, tracks);
    hwa_segmentation_options_default(&options);
    first_result = hwa_segmentation_build(
        &alignment, &analysis, NULL, &options, NULL, 0U,
        &first, error, sizeof(error));
    second_result = first_result == 0
                        ? hwa_segmentation_build(
                              &alignment, &analysis, NULL, &options,
                              NULL, 0U, &second, error, sizeof(error))
                        : -1;
    CHECK(first_result == 0 && second_result == 0,
          "determinism fixtures failed: %s", error);
    if (first_result != 0 || second_result != 0) {
        if (first_result == 0) hwa_item_set_free(&first);
        if (second_result == 0) hwa_item_set_free(&second);
        return;
    }
    CHECK(first.item_count == second.item_count &&
              first.member_count == second.member_count,
          "repeat run changed result counts");
    for (index = 0U;
         index < first.item_count && index < second.item_count; ++index) {
        CHECK(strcmp(first.items[index].key, second.items[index].key) == 0 &&
                  first.items[index].start_sample ==
                      second.items[index].start_sample &&
                  first.items[index].end_sample ==
                      second.items[index].end_sample,
              "repeat run changed item %zu", index);
    }
    CHECK(first.retained_work_bytes == second.retained_work_bytes,
          "repeat run changed retained work proof");
    CHECK(first.retained_work_bytes == expected_retained_work(&first),
          "retained work ledger does not match owned result storage");
    hwa_item_set_free(&second);
    hwa_item_set_free(&first);

    options.max_items = 1U;
    CHECK(hwa_segmentation_build(&alignment, &analysis, NULL, &options,
                                 NULL, 0U, &probe,
                                 error, sizeof(error)) != 0,
          "item one-over fixture did not fail");
    hwa_segmentation_options_default(&options);
    options.max_item_members = 1U;
    CHECK(hwa_segmentation_build(&alignment, &analysis, NULL, &options,
                                 NULL, 0U, &probe,
                                 error, sizeof(error)) != 0,
          "member one-over fixture did not fail");
    hwa_segmentation_options_default(&options);
    options.max_events = 6U;
    CHECK(hwa_segmentation_build(&alignment, &analysis, NULL, &options,
                                 NULL, 0U, &probe,
                                 error, sizeof(error)) != 0,
          "event one-over fixture did not fail");

    /* Find and prove the exact cumulative work-cap boundary. */
    hwa_segmentation_options_default(&options);
    low = 1U;
    high = options.max_segmentation_work_bytes;
    while (low < high) {
        uint64_t middle = low + (high - low) / 2U;
        options.max_segmentation_work_bytes = middle;
        if (hwa_segmentation_build(&alignment, &analysis, NULL, &options,
                                   NULL, 0U, &probe,
                                   error, sizeof(error)) == 0) {
            hwa_item_set_free(&probe);
            high = middle;
        } else {
            low = middle + 1U;
        }
    }
    options.max_segmentation_work_bytes = low;
    CHECK(hwa_segmentation_build(&alignment, &analysis, NULL, &options,
                                 NULL, 0U, &probe,
                                 error, sizeof(error)) == 0,
          "exact work limit did not pass: %s", error);
    if (probe.item_count != 0U) hwa_item_set_free(&probe);
    if (low > 1U) {
        options.max_segmentation_work_bytes = low - 1U;
        CHECK(hwa_segmentation_build(&alignment, &analysis, NULL, &options,
                                     NULL, 0U, &probe,
                                     error, sizeof(error)) != 0,
              "one byte below the work boundary did not fail");
    }
}

static void test_manual_edit_and_limits(void)
{
    HWAAlignment alignment;
    HWAAnalysis analysis;
    HWAAlignmentMatch matches[7];
    HWAFrameMetrics tracks[80];
    HWASegmentationOptions options;
    HWAItemSet items;
    HWAItemEdit edit;
    char error[HWA_ERROR_SIZE];
    const HWAItem *note;
    int build_result;

    make_fixture(&alignment, &analysis, matches, tracks);
    hwa_segmentation_options_default(&options);
    memset(&edit, 0, sizeof(edit));
    edit.key = "note:6e31";
    edit.start_sample = 100U;
    edit.end_sample = 900U;
    edit.locked = 1;
    edit.exclusion_set = 1;
    edit.excluded = 1;
    edit.exclusion_reason = "bad take";
    build_result = hwa_segmentation_build(
        &alignment, &analysis, NULL, &options, &edit, 1U,
        &items, error, sizeof(error));
    CHECK(build_result == 0,
          "manual edit failed: %s", error);
    if (build_result != 0) return;
    note = find_key(&items, edit.key);
    CHECK(note != NULL && note->start_sample == 100U &&
              note->end_sample == 900U && note->locked && note->excluded &&
              note->origin == HWA_ITEM_ORIGIN_MANUAL &&
              strcmp(note->exclusion_reason, "bad take") == 0,
          "manual bound or exclusion was not applied exactly");
    CHECK(items.locked_item_count == 1U &&
              items.excluded_item_count == 1U,
          "manual result counts are wrong");
    hwa_item_set_free(&items);

    options.max_boundary_evaluations = 1U;
    memset(&items, 0x5a, sizeof(items));
    CHECK(hwa_segmentation_build(&alignment, &analysis, NULL, &options,
                                 NULL, 0U, &items,
                                 error, sizeof(error)) != 0 &&
              strstr(error, "evaluation") != NULL,
          "one-over boundary work did not fail clearly: %s", error);
    CHECK(items.item_count == 0U &&
              items.options.max_boundary_evaluations == 1U,
          "failed call was not transactional or lost copied options");
    hwa_item_set_free(&items);
}

static void test_relation_evaluation_limit(void)
{
    HWAAlignment alignment;
    HWAAnalysis analysis;
    HWAAlignmentMatch matches[7];
    HWAFrameMetrics tracks[80];
    HWASegmentationOptions options;
    HWAItemSet items;
    char error[HWA_ERROR_SIZE];
    uint64_t relation_checks = 0U;
    uint64_t full_checks;
    size_t item_index;
    size_t member_index;
    int build_result;

    make_fixture(&alignment, &analysis, matches, tracks);
    make_match(&matches[0], 1U, "a", "note", "v1", "60", "none",
               0.20, 2.20);
    make_match(&matches[1], 2U, "b", "note", "v2", "64", "none",
               0.40, 2.00);
    make_match(&matches[2], 3U, "c", "note", "v3", "67", "none",
               0.60, 1.80);
    alignment.match_count = 3U;
    hwa_segmentation_options_default(&options);
    build_result = hwa_segmentation_build(
        &alignment, &analysis, NULL, &options, NULL, 0U,
        &items, error, sizeof(error));
    CHECK(build_result == 0, "nested-overlap fixture failed: %s", error);
    if (build_result != 0) return;
    full_checks = items.boundary_evaluations;
    for (item_index = 0U; item_index < items.item_count; ++item_index) {
        const HWAItem *item = &items.items[item_index];
        if (strcmp(item->role, "multi-note") != 0) continue;
        relation_checks++;
        for (member_index = 0U; member_index < items.member_count;
             ++member_index) {
            if (items.members[member_index].item_id == item->id &&
                items.members[member_index].role == HWA_ITEM_MEMBER_ACTIVE) {
                relation_checks++;
            }
        }
    }
    CHECK(relation_checks > 0U && full_checks > relation_checks,
          "nested overlap did not expose charged relation work");
    hwa_item_set_free(&items);
    if (relation_checks == 0U || full_checks <= relation_checks) return;

    options.max_boundary_evaluations = full_checks - relation_checks;
    build_result = hwa_segmentation_build(
        &alignment, &analysis, NULL, &options, NULL, 0U,
        &items, error, sizeof(error));
    CHECK(build_result != 0 && strstr(error, "boundary/relation") != NULL,
          "exact nested-overlap relation cap did not fail before copy/sort: %s",
          error);
}

static void test_weak_tie_quality(void)
{
    HWAAlignment alignment;
    HWAAnalysis analysis;
    HWAAlignmentMatch matches[7];
    HWAFrameMetrics tracks[80];
    HWASegmentationOptions options;
    HWAItemSet items;
    static const char *const keys[] = {
        "note:6e31", "attack:6e31", "body:6e31",
        "release:6e31", "tail:6e31"
    };
    char error[HWA_ERROR_SIZE];
    size_t index;
    int build_result;

    make_fixture(&alignment, &analysis, matches, tracks);
    matches[1].status = HWA_ALIGNMENT_LOW_CONFIDENCE;
    hwa_segmentation_options_default(&options);
    options.item_confidence_threshold = 0.10;
    build_result = hwa_segmentation_build(
        &alignment, &analysis, NULL, &options, NULL, 0U,
        &items, error, sizeof(error));
    CHECK(build_result == 0, "weak tie fixture failed: %s", error);
    if (build_result != 0) return;
    for (index = 0U; index < sizeof(keys) / sizeof(keys[0]); ++index) {
        const HWAItem *item = find_key(&items, keys[index]);
        CHECK(item != NULL &&
                  (item->quality_flags &
                   HWA_ITEM_QUALITY_LOW_CONFIDENCE) != 0U,
              "weak tie continuation did not flag '%s'", keys[index]);
    }
    hwa_item_set_free(&items);
}

static void test_composite_quality_and_transition_buckets(void)
{
    HWAAlignment alignment;
    HWAAnalysis analysis;
    HWAAlignmentMatch matches[7];
    HWAFrameMetrics tracks[80];
    HWASegmentationOptions options;
    HWAItemSet items;
    HWATypedLabelSet labels;
    HWATypedLabelRow rows[2];
    const HWAItem *chord;
    const HWAItem *transition;
    const HWAItem *slur;
    const HWAItem *multi;
    char error[HWA_ERROR_SIZE];
    int build_result;

    make_fixture(&alignment, &analysis, matches, tracks);
    make_match(&matches[0], 1U, "p", "note", "v1", "60", "none",
               0.20, 1.20);
    make_match(&matches[1], 2U, "a", "note", "v1", "64", "none",
               1.20, 2.00);
    make_match(&matches[2], 3U, "b", "note", "v1", "67", "none",
               1.20, 2.00);
    matches[2].status = HWA_ALIGNMENT_LOW_CONFIDENCE;
    matches[1].evidence_flags = 0U;
    alignment.match_count = 3U;
    hwa_segmentation_options_default(&options);
    options.item_confidence_threshold = 0.10;
    memset(&labels, 0, sizeof(labels));
    memset(rows, 0, sizeof(rows));
    rows[0].event_id = (char *)"a";
    rows[0].labels.articulation = (char *)"accent";
    rows[0].labels.transition = (char *)"legato";
    rows[0].labels.override_flags = HWA_LABEL_OVERRIDE_ARTICULATION |
                                    HWA_LABEL_OVERRIDE_TRANSITION;
    rows[1].event_id = (char *)"b";
    rows[1].labels.articulation = (char *)"slur";
    rows[1].labels.transition = (char *)"legato";
    rows[1].labels.override_flags = HWA_LABEL_OVERRIDE_ARTICULATION |
                                    HWA_LABEL_OVERRIDE_TRANSITION;
    labels.rows = rows;
    labels.row_count = 2U;
    build_result = hwa_segmentation_build(
        &alignment, &analysis, &labels, &options, NULL, 0U,
        &items, error, sizeof(error));
    CHECK(build_result == 0,
          "composite-quality fixture failed: %s", error);
    if (build_result != 0) return;
    chord = find_role(&items, "chord");
    transition = find_role(&items, "legato");
    slur = find_role(&items, "slur");
    CHECK(chord != NULL && chord->confidence >
              options.item_confidence_threshold &&
              (chord->quality_flags &
               HWA_ITEM_QUALITY_LOW_CONFIDENCE) != 0U,
          "high-confidence chord lost a member's LOW status");
    CHECK(transition != NULL && transition->confidence >
              options.item_confidence_threshold &&
              (transition->quality_flags &
               HWA_ITEM_QUALITY_LOW_CONFIDENCE) != 0U,
          "high-confidence transition lost a member's LOW status");
    CHECK(slur != NULL &&
              (slur->quality_flags &
               HWA_ITEM_QUALITY_LOW_CONFIDENCE) != 0U,
          "transition child lost its source quality");
    CHECK(slur != NULL,
          "later chord-member slur was hidden by an earlier accent");
    CHECK(chord != NULL &&
              (chord->quality_flags &
               HWA_ITEM_QUALITY_NO_EVIDENCE) == 0U,
          "one no-evidence member tainted the whole chord");
    hwa_item_set_free(&items);

    matches[2].evidence_flags = 0U;
    build_result = hwa_segmentation_build(
        &alignment, &analysis, &labels, &options, NULL, 0U,
        &items, error, sizeof(error));
    CHECK(build_result == 0,
          "all-no-evidence chord fixture failed: %s", error);
    if (build_result != 0) return;
    chord = find_role(&items, "chord");
    CHECK(chord != NULL &&
              (chord->quality_flags &
               HWA_ITEM_QUALITY_NO_EVIDENCE) != 0U,
          "all-no-evidence chord lost its quality flag");
    hwa_item_set_free(&items);

    rows[1].labels.transition = (char *)"portamento";
    build_result = hwa_segmentation_build(
        &alignment, &analysis, &labels, &options, NULL, 0U,
        &items, error, sizeof(error));
    CHECK(build_result != 0 && strstr(error, "conflict") != NULL &&
              strstr(error, "'a'") != NULL &&
              strstr(error, "'b'") != NULL,
          "conflicting chord transitions were not rejected with IDs: %s",
          error);

    make_match(&matches[0], 1U, "x", "note", "v1", "60", "none",
               0.20, 2.20);
    make_match(&matches[1], 2U, "y", "note", "v2", "64", "none",
               0.40, 2.00);
    matches[0].status = HWA_ALIGNMENT_LOW_CONFIDENCE;
    alignment.match_count = 2U;
    build_result = hwa_segmentation_build(
        &alignment, &analysis, NULL, &options, NULL, 0U,
        &items, error, sizeof(error));
    CHECK(build_result == 0,
          "overlap-quality fixture failed: %s", error);
    if (build_result != 0) return;
    multi = find_role(&items, "multi-note");
    CHECK(multi != NULL && multi->confidence >
              options.item_confidence_threshold &&
              (multi->quality_flags &
               HWA_ITEM_QUALITY_LOW_CONFIDENCE) != 0U,
          "high-confidence overlap lost a member's LOW status");
    hwa_item_set_free(&items);
}

static void test_mode_and_bad_edit_rejection(void)
{
    HWAAlignment alignment;
    HWAAnalysis analysis;
    HWAAlignmentMatch matches[7];
    HWAFrameMetrics tracks[80];
    HWASegmentationOptions options;
    HWAItemSet items;
    HWAItemEdit edit;
    HWATypedLabelSet huge_labels;
    HWATypedLabelRow huge_row;
    char error[HWA_ERROR_SIZE];

    make_fixture(&alignment, &analysis, matches, tracks);
    hwa_segmentation_options_default(&options);
    alignment.mode = HWA_ALIGNMENT_AUDIO_TO_AUDIO;
    CHECK(hwa_segmentation_build(&alignment, &analysis, NULL, &options,
                                 NULL, 0U, &items,
                                 error, sizeof(error)) != 0,
          "audio-to-audio alignment was accepted as a score");
    alignment.mode = HWA_ALIGNMENT_SCORE_TO_AUDIO;
    memset(&edit, 0, sizeof(edit));
    edit.key = "missing:key";
    edit.start_sample = 1U;
    edit.end_sample = 2U;
    edit.locked = 1;
    CHECK(hwa_segmentation_build(&alignment, &analysis, NULL, &options,
                                 &edit, 1U, &items,
                                 error, sizeof(error)) != 0 &&
              strstr(error, "unknown key") != NULL,
          "unknown manual key was not rejected: %s", error);

    matches[1].event_id = (char *)"n1";
    CHECK(hwa_segmentation_build(&alignment, &analysis, NULL, &options,
                                 NULL, 0U, &items,
                                 error, sizeof(error)) != 0 &&
              strstr(error, "repeats event ID") != NULL,
          "duplicate alignment event ID was not rejected: %s", error);
    matches[1].event_id = (char *)"n2";

    if (SIZE_MAX > (size_t)UINT32_MAX) {
        alignment.match_count = (size_t)UINT32_MAX + 1U;
        options.max_events = SIZE_MAX;
        CHECK(hwa_segmentation_build(&alignment, &analysis, NULL, &options,
                                     NULL, 0U, &items,
                                     error, sizeof(error)) != 0,
              "event member-order overflow was accepted");
        alignment.match_count = 7U;
    }
    hwa_segmentation_options_default(&options);
    memset(&huge_labels, 0, sizeof(huge_labels));
    memset(&huge_row, 0, sizeof(huge_row));
    huge_labels.rows = &huge_row;
    huge_labels.row_count = SIZE_MAX;
    options.max_label_rows = SIZE_MAX;
    CHECK(hwa_segmentation_build(&alignment, &analysis, &huge_labels,
                                 &options, NULL, 0U, &items,
                                 error, sizeof(error)) != 0,
          "label work-size overflow was accepted");
}

int main(void)
{
    test_score_roles_and_phases();
    test_typed_gestures();
    test_isolated_score_chord_once();
    test_exact_rounding_and_short_body();
    test_determinism_and_resource_edges();
    test_manual_edit_and_limits();
    test_relation_evaluation_limit();
    test_weak_tie_quality();
    test_composite_quality_and_transition_buckets();
    test_mode_and_bad_edit_rejection();
    if (failures != 0) {
        (void)fprintf(stderr, "%d segmentation test(s) failed\n", failures);
        return 1;
    }
    (void)puts("segmentation tests passed");
    return 0;
}

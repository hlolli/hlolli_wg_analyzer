#include "internal.h"
#include "musicxml_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct ScopedVelocity {
    const char *voice, *staff;
    double value;
    size_t order;
    unsigned interpretation;
} ScopedVelocity;

int hwa_musicxml_event_order(const void *left, const void *right)
{
    const HWAMusicXMLEvent *a = left, *b = right;
    if (a->start_beats != b->start_beats) {
        return a->start_beats < b->start_beats ? -1 : 1;
    }
    if ((a->kind == HWA_MUSICXML_TEMPO) != (b->kind == HWA_MUSICXML_TEMPO)) {
        return a->kind == HWA_MUSICXML_TEMPO ? -1 : 1;
    }
    if (a->source_offset != b->source_offset) {
        return a->source_offset < b->source_offset ? -1 : 1;
    }
    return a->sequence < b->sequence ? -1 : a->sequence > b->sequence ? 1 : 0;
}

static int dynamics_order(const void *left, const void *right)
{
    const HWAMusicXMLEvent *a = *(const HWAMusicXMLEvent *const *)left;
    const HWAMusicXMLEvent *b = *(const HWAMusicXMLEvent *const *)right;
    int order = strcmp(a->part, b->part);
    if (order == 0 && a->start_beats == b->start_beats &&
        (a->kind == HWA_MUSICXML_MARK) != (b->kind == HWA_MUSICXML_MARK)) {
        /* A note's attached dynamic also applies to that note. */
        return a->kind == HWA_MUSICXML_MARK ? -1 : 1;
    }
    return order != 0 ? order : hwa_musicxml_event_order(a, b);
}

static int voice_order(const void *left, const void *right)
{
    const HWAMusicXMLEvent *a = *(const HWAMusicXMLEvent *const *)left;
    const HWAMusicXMLEvent *b = *(const HWAMusicXMLEvent *const *)right;
    int order = strcmp(a->part, b->part);
    if (order == 0) {
        order = strcmp(a->voice, b->voice);
    }
    return order != 0 ? order : hwa_musicxml_event_order(a, b);
}

static int same_voice(const HWAMusicXMLEvent *a, const HWAMusicXMLEvent *b)
{
    return strcmp(a->part, b->part) == 0 && strcmp(a->voice, b->voice) == 0;
}

static double dynamic_velocity(const char *name)
{
    static const struct {
        const char *name;
        double velocity;
    } dynamics[] = {{"pppp", 16}, {"ppp", 24}, {"pp", 33},  {"p", 45},    {"mp", 56},
                    {"mf", 68},   {"f", 90},   {"ff", 104}, {"fff", 116}, {"ffff", 127}};
    size_t i;
    for (i = 0U; i < sizeof(dynamics) / sizeof(dynamics[0]); i++) {
        if (strcmp(name, dynamics[i].name) == 0) {
            return dynamics[i].velocity;
        }
    }
    return -1.0;
}

static const char *apply_dynamics(HWAMusicXMLEvent **refs, size_t count,
                                  const HWAMusicXMLOptions *options)
{
    ScopedVelocity scopes[256];
    size_t i, scope_count = 0U, global_order = 0U;
    const char *part = NULL;
    double global_velocity = -1.0;
    unsigned global_interpretation = 0U;
    qsort(refs, count, sizeof(*refs), dynamics_order);
    for (i = 0U; i < count; i++) {
        HWAMusicXMLEvent *event = refs[i];
        size_t scope_index;
        double velocity = -1.0;
        unsigned interpretation = 0U;
        if (part == NULL || strcmp(part, event->part) != 0) {
            part = event->part;
            scope_count = 0U;
            global_order = 0U;
            global_velocity = options->performance ? options->default_velocity : -1.0;
            global_interpretation = options->performance ? HWA_MUSICXML_BASELINE_POLICY : 0U;
        }
        if (event->kind == HWA_MUSICXML_MARK &&
            !(event->interpretation & HWA_MUSICXML_CUE_NOTATION)) {
            if (strcmp(event->mark, "velocity") == 0) {
                velocity = event->value;
                interpretation = HWA_MUSICXML_PLAYBACK_DATA;
            } else if (options->performance) {
                velocity = dynamic_velocity(event->mark);
                interpretation = HWA_MUSICXML_BASELINE_POLICY;
            }
        }
        if (velocity >= 0.0) {
            if (*event->voice == '\0' && *event->staff == '\0') {
                global_velocity = velocity;
                global_order = i + 1U;
                global_interpretation = interpretation;
            } else {
                for (scope_index = 0U; scope_index < scope_count; scope_index++) {
                    if (strcmp(scopes[scope_index].voice, event->voice) == 0 &&
                        strcmp(scopes[scope_index].staff, event->staff) == 0) {
                        break;
                    }
                }
                if (scope_index == scope_count) {
                    if (scope_count == sizeof(scopes) / sizeof(scopes[0])) {
                        return "scoped dynamics limit exceeded";
                    }
                    scopes[scope_index].voice = event->voice;
                    scopes[scope_index].staff = event->staff;
                    scope_count++;
                }
                scopes[scope_index].value = velocity;
                scopes[scope_index].order = i + 1U;
                scopes[scope_index].interpretation = interpretation;
            }
        }
        if ((event->kind == HWA_MUSICXML_NOTE || event->kind == HWA_MUSICXML_GRACE) &&
            !event->velocity_valid) {
            size_t selected_order = global_order;
            velocity = global_velocity;
            interpretation = global_interpretation;
            /* The latest matching mark wins, including a later part-wide mark. */
            for (scope_index = 0U; scope_index < scope_count; scope_index++) {
                const ScopedVelocity *scope = &scopes[scope_index];
                if ((*scope->voice == '\0' || strcmp(scope->voice, event->voice) == 0) &&
                    (*scope->staff == '\0' || strcmp(scope->staff, event->staff) == 0) &&
                    scope->order > selected_order) {
                    velocity = scope->value;
                    interpretation = scope->interpretation;
                    selected_order = scope->order;
                }
            }
            if (velocity >= 0.0) {
                event->velocity = velocity;
                event->velocity_valid = 1;
                event->interpretation |= interpretation;
            }
        }
        if (options->performance && event->velocity_valid &&
            (event->articulations & (HWA_MUSICXML_ACCENT | HWA_MUSICXML_STRONG_ACCENT))) {
            double scale = (event->articulations & HWA_MUSICXML_STRONG_ACCENT) ? 1.3 : 1.15;
            event->velocity = fmin(127.0, event->velocity * scale);
            event->interpretation |= HWA_MUSICXML_BASELINE_POLICY;
        }
    }
    return NULL;
}

static const char *apply_grace_timing(HWAMusicXMLScore *score, HWAMusicXMLEvent **refs,
                                      const HWAMusicXMLOptions *options)
{
    size_t i, notes = 0U;
    for (i = 0U; i < score->event_count; i++) {
        if (score->events[i].kind == HWA_MUSICXML_NOTE ||
            score->events[i].kind == HWA_MUSICXML_GRACE) {
            refs[notes++] = &score->events[i];
        }
    }
    qsort(refs, notes, sizeof(*refs), voice_order);
    for (i = 0U; i < notes;) {
        size_t end, k, attacks = 0U, attack = 0U;
        HWAMusicXMLEvent *grace = refs[i], *following = NULL, *previous = NULL;
        double previous_beats = 0.0, following_beats = 0.0, start_beat, attack_duration;
        unsigned interpretation = HWA_MUSICXML_PLAYBACK_DATA;
        if (grace->kind != HWA_MUSICXML_GRACE) {
            i++;
            continue;
        }
        for (end = i + 1U;
             end < notes && refs[end]->kind == HWA_MUSICXML_GRACE &&
             refs[end]->start_beats == grace->start_beats && same_voice(refs[end], grace);
             end++) {
        }
        if (end < notes && same_voice(refs[end], grace)) {
            following = refs[end];
        }
        if (i > 0U && same_voice(refs[i - 1U], grace)) {
            previous = refs[i - 1U];
        }
        if (grace->grace_make >= 0.0) {
            score->unrendered_marks++;
            i = end;
            continue;
        }
        if (grace->grace_previous >= 0.0) {
            if (previous == NULL) {
                return "grace timing has no preceding note";
            }
            previous_beats = previous->duration_beats * grace->grace_previous / 100.0;
        }
        if (grace->grace_following >= 0.0) {
            if (following == NULL) {
                return "grace timing has no following note";
            }
            following_beats = following->duration_beats * grace->grace_following / 100.0;
        }
        if (grace->grace_previous < 0.0 && grace->grace_following < 0.0) {
            interpretation = HWA_MUSICXML_BASELINE_POLICY;
            if (following != NULL && following->start_beats == grace->start_beats) {
                following_beats = following->duration_beats * options->grace_fraction;
            } else if (previous != NULL) {
                previous_beats = previous->duration_beats * options->grace_fraction;
            } else {
                score->unrendered_marks++;
                i = end;
                continue;
            }
        }
        for (k = i + 1U; k < end; k++) {
            if (refs[k]->grace_previous != grace->grace_previous ||
                refs[k]->grace_following != grace->grace_following ||
                refs[k]->grace_make != grace->grace_make) {
                return "grace group has conflicting timing attributes";
            }
        }
        start_beat = grace->start_beats - previous_beats;
        if (start_beat < 0.0 || previous_beats + following_beats <= 0.0) {
            return "invalid grace playback interval";
        }
        if (following_beats > 0.0 && following != NULL &&
            following->start_beats != grace->start_beats) {
            return "grace borrowing crosses a gap before the following note";
        }
        if (previous_beats > 0.0 && previous != NULL) {
            double start = previous->start_beats;
            k = i;
            while (k > 0U && refs[k - 1U]->start_beats == start &&
                   same_voice(refs[k - 1U], grace)) {
                HWAMusicXMLEvent *p = refs[--k];
                p->duration_beats = fmin(p->duration_beats, fmax(0.0, start_beat - p->start_beats));
                p->interpretation |= interpretation;
            }
        }
        if (following_beats > 0.0 && following != NULL) {
            double start = following->start_beats;
            for (k = end; k < notes && refs[k]->start_beats == start && same_voice(refs[k], grace);
                 k++) {
                refs[k]->start_beats += following_beats;
                refs[k]->duration_beats = fmax(0.0, refs[k]->duration_beats - following_beats);
                refs[k]->interpretation |= interpretation;
            }
        }
        for (k = i; k < end; k++) {
            if (!refs[k]->chord) {
                attacks++;
            }
        }
        if (attacks == 0U) {
            return "grace chord lacks a leading note";
        }
        attack_duration = (previous_beats + following_beats) / (double)attacks;
        for (k = i; k < end; k++) {
            if (k != i && !refs[k]->chord) {
                attack++;
            }
            refs[k]->kind = HWA_MUSICXML_NOTE;
            refs[k]->start_beats = start_beat + (double)attack * attack_duration;
            refs[k]->duration_beats = attack_duration;
            refs[k]->interpretation |= interpretation;
        }
        i = end;
    }
    return NULL;
}

int hwa_musicxml_perform(HWAMusicXMLScore *score, const HWAMusicXMLOptions *options,
                         uint64_t work_bytes, char *error, size_t error_size)
{
    HWAMusicXMLEvent **refs;
    size_t i, count = score->event_count;
    const char *failure;
    if (count > SIZE_MAX / sizeof(*refs) || (uint64_t)count * sizeof(*refs) > work_bytes ||
        (refs = malloc(count * sizeof(*refs))) == NULL) {
        hwa_set_error(error, error_size,
                      "MusicXML expression work byte limit or allocation failure");
        return -1;
    }
    for (i = 0U; i < count; i++) {
        refs[i] = &score->events[i];
    }
    failure = apply_dynamics(refs, count, options);
    if (failure == NULL && options->performance) {
        failure = apply_grace_timing(score, refs, options);
    }
    free(refs);
    if (failure != NULL) {
        hwa_set_error(error, error_size, "MusicXML playback: %s", failure);
        return -1;
    }
    for (i = 0U; i < count; i++) {
        if (score->events[i].interpretation & HWA_MUSICXML_BASELINE_POLICY) {
            score->interpreted_events++;
        }
    }
    qsort(score->events, count, sizeof(*score->events), hwa_musicxml_event_order);
    return 0;
}

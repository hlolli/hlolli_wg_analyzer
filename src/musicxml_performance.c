#include "internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int time_order(const void *left, const void *right)
{
    const HWAMusicXMLEvent *a = left, *b = right;
    if (a->start_beats != b->start_beats) return a->start_beats < b->start_beats ? -1 : 1;
    if ((a->kind == HWA_MUSICXML_TEMPO) != (b->kind == HWA_MUSICXML_TEMPO)) return a->kind == HWA_MUSICXML_TEMPO ? -1 : 1;
    if (a->source_offset != b->source_offset) return a->source_offset < b->source_offset ? -1 : 1;
    return a->sequence < b->sequence ? -1 : a->sequence > b->sequence ? 1 : 0;
}

static int part_order(const void *left, const void *right)
{
    const HWAMusicXMLEvent *a = *(const HWAMusicXMLEvent *const *)left;
    const HWAMusicXMLEvent *b = *(const HWAMusicXMLEvent *const *)right;
    int order = strcmp(a->part, b->part);
    return order != 0 ? order : time_order(a, b);
}

static int voice_order(const void *left, const void *right)
{
    const HWAMusicXMLEvent *a = *(const HWAMusicXMLEvent *const *)left;
    const HWAMusicXMLEvent *b = *(const HWAMusicXMLEvent *const *)right;
    int order = strcmp(a->part, b->part);
    if (order == 0) order = strcmp(a->voice, b->voice);
    return order != 0 ? order : time_order(a, b);
}

static int same_voice(const HWAMusicXMLEvent *a, const HWAMusicXMLEvent *b)
{
    return strcmp(a->part, b->part) == 0 && strcmp(a->voice, b->voice) == 0;
}

static double dynamic_velocity(const char *name)
{
    static const char *const names[] = {"pppp", "ppp", "pp", "p", "mp", "mf", "f", "ff", "fff", "ffff"};
    static const double velocities[] = {16,24,33,45,56,68,90,104,116,127};
    size_t i;
    for (i = 0U; i < sizeof(names)/sizeof(names[0]); i++) if (strcmp(name, names[i]) == 0) return velocities[i];
    return -1.0;
}

int hwa_musicxml_perform(HWAMusicXMLScore *score, const HWAMusicXMLOptions *options,
                        uint64_t work_bytes, char *error, size_t error_size)
{
    struct VoiceVelocity { const char *voice; double value; size_t order; unsigned interpretation; } voices[256];
    HWAMusicXMLEvent **refs;
    size_t i, count = score->event_count, notes = 0U, voice_count = 0U, global_order = 0U;
    const char *part = NULL, *failure = NULL;
    double global_velocity = -1.0;
    unsigned global_interpretation = 0U;
    if (count > SIZE_MAX/sizeof(*refs) || (uint64_t)count*sizeof(*refs) > work_bytes ||
        (refs = malloc(count*sizeof(*refs))) == NULL) {
        hwa_set_error(error, error_size, "MusicXML expression work byte limit or allocation failure"); return -1;
    }
    for (i = 0U; i < count; i++) refs[i] = &score->events[i];
    qsort(refs, count, sizeof(*refs), part_order);
    for (i = 0U; i < count; i++) {
        HWAMusicXMLEvent *e = refs[i];
        size_t v;
        double velocity = -1.0;
        unsigned interpretation = 0U;
        if (part == NULL || strcmp(part, e->part) != 0) {
            part = e->part; voice_count = 0U; global_order = 0U;
            global_velocity = options->performance ? options->default_velocity : -1.0;
            global_interpretation = options->performance ? 1U : 0U;
        }
        if (e->kind == HWA_MUSICXML_MARK) {
            if (strcmp(e->mark, "velocity") == 0) { velocity = e->value; interpretation = 2U; }
            else if (options->performance) { velocity = dynamic_velocity(e->mark); interpretation = 1U; }
        }
        if (velocity >= 0.0) {
            if (*e->voice == '\0') { global_velocity = velocity; global_order = i+1U; global_interpretation = interpretation; }
            else {
                for (v = 0U; v < voice_count; v++) if (strcmp(voices[v].voice, e->voice) == 0) break;
                if (v == voice_count) {
                    if (voice_count == 256U) { failure = "voice dynamics limit exceeded"; goto done; }
                    voices[v].voice = e->voice; voice_count++;
                }
                voices[v].value = velocity; voices[v].order = i+1U; voices[v].interpretation = interpretation;
            }
        }
        if ((e->kind == HWA_MUSICXML_NOTE || e->kind == HWA_MUSICXML_GRACE) && !e->velocity_valid) {
            velocity = global_velocity; interpretation = global_interpretation;
            for (v = 0U; v < voice_count; v++) if (strcmp(voices[v].voice, e->voice) == 0 && voices[v].order > global_order) {
                velocity = voices[v].value; interpretation = voices[v].interpretation;
            }
            if (velocity >= 0.0) { e->velocity = velocity; e->velocity_valid = 1; e->interpretation |= interpretation; }
        }
        if (options->performance && e->velocity_valid && (e->articulations & 24U)) {
            e->velocity = fmin(127.0, e->velocity*((e->articulations & 16U) ? 1.3 : 1.15)); e->interpretation |= 1U;
        }
    }
    if (options->performance) {
        for (i = 0U; i < count; i++) if (score->events[i].kind == HWA_MUSICXML_NOTE || score->events[i].kind == HWA_MUSICXML_GRACE)
            refs[notes++] = &score->events[i];
        qsort(refs, notes, sizeof(*refs), voice_order);
        for (i = 0U; i < notes; ) {
            size_t end, k, attacks = 0U, attack = 0U;
            HWAMusicXMLEvent *g = refs[i], *following = NULL, *previous = NULL;
            double before = 0.0, after = 0.0, at_beat, each;
            unsigned interpretation = 2U;
            if (g->kind != HWA_MUSICXML_GRACE) { i++; continue; }
            for (end = i+1U; end < notes && refs[end]->kind == HWA_MUSICXML_GRACE &&
                refs[end]->start_beats == g->start_beats && same_voice(refs[end], g); end++) {}
            if (end < notes && same_voice(refs[end], g)) following = refs[end];
            if (i > 0U && same_voice(refs[i-1U], g)) previous = refs[i-1U];
            if (g->grace_make >= 0.0) { score->unrendered_marks++; i = end; continue; }
            if (g->grace_previous >= 0.0) {
                if (previous == NULL) { failure = "grace timing has no preceding note"; goto done; }
                before = previous->duration_beats*g->grace_previous/100.0;
            }
            if (g->grace_following >= 0.0) {
                if (following == NULL) { failure = "grace timing has no following note"; goto done; }
                after = following->duration_beats*g->grace_following/100.0;
            }
            if (g->grace_previous < 0.0 && g->grace_following < 0.0) {
                interpretation = 1U;
                if (following != NULL && following->start_beats == g->start_beats) after = following->duration_beats*options->grace_fraction;
                else if (previous != NULL) before = previous->duration_beats*options->grace_fraction;
                else { score->unrendered_marks++; i = end; continue; }
            }
            for (k = i+1U; k < end; k++) if (refs[k]->grace_previous != g->grace_previous ||
                refs[k]->grace_following != g->grace_following || refs[k]->grace_make != g->grace_make) {
                failure = "grace group has conflicting timing attributes"; goto done;
            }
            at_beat = g->start_beats-before;
            if (at_beat < 0.0 || before+after <= 0.0) { failure = "invalid grace playback interval"; goto done; }
            if (after > 0.0 && following != NULL && following->start_beats != g->start_beats) {
                failure = "grace borrowing crosses a gap before the following note"; goto done;
            }
            if (before > 0.0 && previous != NULL) {
                double start = previous->start_beats;
                k = i;
                while (k > 0U && refs[k-1U]->start_beats == start && same_voice(refs[k-1U], g)) {
                    HWAMusicXMLEvent *p = refs[--k];
                    p->duration_beats = fmin(p->duration_beats, fmax(0.0, at_beat-p->start_beats)); p->interpretation |= interpretation;
                }
            }
            if (after > 0.0 && following != NULL) {
                double start = following->start_beats;
                for (k = end; k < notes && refs[k]->start_beats == start && same_voice(refs[k], g); k++) {
                    refs[k]->start_beats += after; refs[k]->duration_beats = fmax(0.0, refs[k]->duration_beats-after); refs[k]->interpretation |= interpretation;
                }
            }
            for (k = i; k < end; k++) if (!refs[k]->chord) attacks++;
            if (attacks == 0U) { failure = "grace chord lacks a leading note"; goto done; }
            each = (before+after)/(double)attacks;
            for (k = i; k < end; k++) {
                if (k != i && !refs[k]->chord) attack++;
                refs[k]->kind = HWA_MUSICXML_NOTE; refs[k]->start_beats = at_beat+(double)attack*each;
                refs[k]->duration_beats = each; refs[k]->interpretation |= interpretation;
            }
            i = end;
        }
    }
    for (i = 0U; i < count; i++) if (score->events[i].interpretation & 1U) score->interpreted_events++;
    qsort(score->events, count, sizeof(*score->events), time_order);
done:
    free(refs);
    if (failure != NULL) { hwa_set_error(error, error_size, "MusicXML playback: %s", failure); return -1; }
    return 0;
}

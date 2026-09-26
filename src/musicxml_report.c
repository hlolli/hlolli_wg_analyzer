#include "musicxml_report.h"
#include "output.h"
#include "numeric_locale.h"
#include <string.h>

static int optional_number(FILE *stream, const char *name, double value, int valid)
{
    if (fprintf(stream, ",\"%s\":", name) < 0) {
        return -1;
    }
    return valid ? (fprintf(stream, "%.17g", value) < 0 ? -1 : 0)
                 : (fputs("null", stream) == EOF ? -1 : 0);
}

static int write_tempo_details(FILE *stream, const HWAMusicXMLEvent *event)
{
    const HWAMusicXMLMetronome *mark = &event->metronome;
    if (fputs(",\"tempo_source\":", stream) == EOF) {
        return -1;
    }
    if (event->kind == HWA_MUSICXML_TEMPO) {
        if (hwa_json_write_string(stream, event->tempo_source ? event->tempo_source : "unknown") != 0) {
            return -1;
        }
    } else if (fputs("null", stream) == EOF) {
        return -1;
    }
    if (fputs(",\"metronome\":", stream) == EOF) {
        return -1;
    }
    if (event->kind != HWA_MUSICXML_MARK || mark->beat_unit == NULL) {
        return fputs("null", stream) == EOF ? -1 : 0;
    }
    if (fputs("{\"beat_unit\":", stream) == EOF ||
        hwa_json_write_string(stream, mark->beat_unit) != 0 ||
        fprintf(stream, ",\"dots\":%u,\"per_minute\":", mark->dots) < 0 ||
        hwa_json_write_string(stream, mark->per_minute) != 0 ||
        optional_number(stream, "quarter_bpm", mark->quarter_bpm, mark->quarter_bpm > 0.0) != 0 ||
        fprintf(stream, ",\"visible\":%s}", mark->visible ? "true" : "false") < 0) {
        return -1;
    }
    return 0;
}

static int write_event(FILE *stream, const HWAMusicXMLEvent *event, size_t index)
{
    static const char *const kinds[] = {"invalid", "note",      "rest",    "grace",
                                        "tempo",   "direction", "control", "mark", "cue"};
    const struct {
        const char *key;
        const char *value;
    } strings[] = {
        {"id", event->id},       {"part", event->part},   {"part_name", event->part_name},
        {"voice", event->voice}, {"staff", event->staff}, {"measure", event->measure},
        {"mark", event->mark}, {"mark_tag", event->mark_tag}, {"mark_text", event->mark_text},
        {"mark_type", event->mark_type}, {"mark_number", event->mark_number}};
    int note = event->kind == HWA_MUSICXML_NOTE || event->kind == HWA_MUSICXML_GRACE;
    int pitch = note || (event->kind == HWA_MUSICXML_CUE && event->midi_pitch >= 0.0);
    int grace = event->kind == HWA_MUSICXML_GRACE ||
                ((event->kind == HWA_MUSICXML_NOTE || event->kind == HWA_MUSICXML_CUE) &&
                 event->written_duration_beats == 0.0);
    int has_value = event->kind == HWA_MUSICXML_CONTROL ||
                    (event->kind == HWA_MUSICXML_MARK && event->mark != NULL &&
                     strcmp(event->mark, "velocity") == 0);
    size_t field;
    const char *kind = event->kind >= HWA_MUSICXML_NOTE && event->kind <= HWA_MUSICXML_CUE
                           ? kinds[event->kind]
                           : "invalid";
    if (fprintf(stream, "%s{\"index\":%zu,\"kind\":\"%s\"", index ? "," : "", index, kind) < 0) {
        return -1;
    }
    for (field = 0U; field < sizeof(strings) / sizeof(strings[0]); field++) {
        if (fprintf(stream, ",\"%s\":", strings[field].key) < 0 ||
            hwa_json_write_string(stream, strings[field].value ? strings[field].value : "") != 0) {
            return -1;
        }
    }
    if (fprintf(
            stream,
            ",\"start_beats\":%.17g,\"duration_beats\":%.17g,\"written_start_beats\":%.17g,"
            "\"written_duration_beats\":%.17g,\"measure_index\":%zu,\"occurrence\":%u,\"tie\":%u,"
            "\"source_offset\":%zu,\"source_size\":%zu,\"sequence\":%u,\"interpretation\":%u,"
            "\"articulations\":%u,\"chord\":%s",
            event->start_beats, event->duration_beats, event->written_start_beats,
            event->written_duration_beats, event->measure_index, event->occurrence, event->tie,
            event->source_offset, event->source_size, event->sequence, event->interpretation,
            event->articulations, event->chord ? "true" : "false") < 0) {
        return -1;
    }
    if (optional_number(stream, "midi_pitch", event->midi_pitch, pitch) != 0 ||
        optional_number(stream, "velocity", event->velocity, note && event->velocity_valid) != 0 ||
        optional_number(stream, "tempo_bpm", event->tempo_bpm, event->kind == HWA_MUSICXML_TEMPO) !=
            0 ||
        optional_number(stream, "controller", (double)event->controller,
                        event->kind == HWA_MUSICXML_CONTROL) != 0 ||
        optional_number(stream, "value", event->value, has_value) != 0 ||
        optional_number(stream, "grace_previous_percent", event->grace_previous,
                        grace && event->grace_previous >= 0.0) != 0 ||
        optional_number(stream, "grace_following_percent", event->grace_following,
                        grace && event->grace_following >= 0.0) != 0 ||
        optional_number(stream, "grace_make_beats", event->grace_make,
                        grace && event->grace_make >= 0.0) != 0) {
        return -1;
    }
    if (write_tempo_details(stream, event) != 0) {
        return -1;
    }
    return fputs("}", stream) == EOF ? -1 : 0;
}

static int write_report(FILE *stream, const HWAMusicXMLScore *score,
                        const HWAMusicXMLOptions *options)
{
    size_t i;
    if (fprintf(stream,
                "{\"schema\":\"hwa-musicxml-score\",\"schema_version\":1,"
                "\"time_unit\":\"quarter_note\",\"mode\":\"%s\","
                "\"default_tempo_bpm\":%.17g,\"used_default_tempo\":%s,\"duration_beats\":%.17g,"
                "\"part_count\":%zu,\"measure_visits\":%zu,\"unfolded\":%s,"
                "\"interpreted_events\":%zu,\"unrendered_marks\":%zu,"
                "\"repaired_tuplets\":%zu,\"tempo_conflicts\":%zu,"
                "\"repaired_tuplet_forwards\":%zu,\"repaired_tuplet_backups\":%zu,\"overfull_measures\":%zu,"
                "\"policy\":{\"grace_fraction\":%.17g,\"trill_notes_per_beat\":%.17g,"
                "\"staccato_ratio\":%.17g,\"default_velocity\":%.17g,"
                "\"repair_tuplets\":%s,\"tempo_conflicts\":\"%s\","
                "\"overfull_measures\":\"%s\"},\"events\":[",
                options->performance ? "baseline-performance" : "written",
                options->default_tempo_bpm, score->used_default_tempo ? "true" : "false",
                score->duration_beats, score->part_count, score->measure_visits,
                score->unfolded ? "true" : "false", score->interpreted_events,
                score->unrendered_marks, score->repaired_tuplets, score->tempo_conflicts,
                score->repaired_tuplet_forwards, score->repaired_tuplet_backups, score->overfull_measures,
                options->grace_fraction, options->trill_notes_per_beat, options->staccato_ratio,
                options->default_velocity, options->repair_tuplets ? "true" : "false",
                options->last_tempo_wins ? "last" : "error",
                options->preserve_overfull_measures ? "preserve" : "error") < 0) {
        return -1;
    }
    for (i = 0U; i < score->event_count; i++) {
        if (write_event(stream, &score->events[i], i) != 0) {
            return -1;
        }
    }
    return fputs("]}\n", stream) == EOF || ferror(stream) ? -1 : 0;
}

int hwa_musicxml_report(FILE *stream, const HWAMusicXMLScore *score,
                        const HWAMusicXMLOptions *options)
{
    HWANumericLocale locale;
    int result;
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        return -1;
    }
    result = write_report(stream, score, options);
    if (hwa_c_numeric_locale_end(&locale) != 0) {
        result = -1;
    }
    return result;
}

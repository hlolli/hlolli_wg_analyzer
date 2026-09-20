#include "musicxml_report.h"
#include "output.h"
#include "numeric_locale.h"
#include <string.h>

static int optional_number(FILE *stream, const char *name, double value, int valid)
{
    if (fprintf(stream, ",\"%s\":", name) < 0) return -1;
    return valid ? (fprintf(stream, "%.17g", value) < 0 ? -1 : 0) : (fputs("null", stream) == EOF ? -1 : 0);
}

static int write_report(FILE *stream, const HWAMusicXMLScore *score, const HWAMusicXMLOptions *options)
{
    static const char *const kinds[] = {"invalid", "note", "rest", "grace", "tempo", "direction", "control", "mark"};
    size_t i;
    if (fprintf(stream, "{\"schema\":\"hwa-musicxml-score\",\"schema_version\":1,\"time_unit\":\"quarter_note\","
        "\"mode\":\"%s\",\"default_tempo_bpm\":%.17g,\"used_default_tempo\":%s,\"duration_beats\":%.17g,"
        "\"part_count\":%zu,\"measure_visits\":%zu,\"unfolded\":%s,\"interpreted_events\":%zu,\"unrendered_marks\":%zu,"
        "\"policy\":{\"grace_fraction\":%.17g,\"trill_notes_per_beat\":%.17g,\"staccato_ratio\":%.17g,\"default_velocity\":%.17g},\"events\":[",
        options->performance ? "baseline-performance" : "written", options->default_tempo_bpm,
        score->used_default_tempo ? "true" : "false", score->duration_beats, score->part_count,
        score->measure_visits, score->unfolded ? "true" : "false", score->interpreted_events, score->unrendered_marks,
        options->grace_fraction, options->trill_notes_per_beat, options->staccato_ratio, options->default_velocity) < 0) return -1;
    for (i = 0U; i < score->event_count; i++) {
        const HWAMusicXMLEvent *e = &score->events[i];
        const char *const values[] = {e->id, e->part, e->part_name, e->voice, e->staff, e->measure, e->mark};
        static const char *const keys[] = {"id", "part", "part_name", "voice", "staff", "measure", "mark"};
        size_t s;
        if (fprintf(stream, "%s{\"index\":%zu,\"kind\":\"%s\"", i ? "," : "", i,
            e->kind >= HWA_MUSICXML_NOTE && e->kind <= HWA_MUSICXML_MARK ? kinds[e->kind] : "invalid") < 0) return -1;
        for (s = 0U; s < sizeof(keys)/sizeof(keys[0]); s++) {
            if (fprintf(stream, ",\"%s\":", keys[s]) < 0 || hwa_json_write_string(stream, values[s] ? values[s] : "") != 0) return -1;
        }
        if (fprintf(stream, ",\"start_beats\":%.17g,\"duration_beats\":%.17g,\"written_start_beats\":%.17g,"
            "\"written_duration_beats\":%.17g,\"measure_index\":%zu,\"occurrence\":%u,\"tie\":%u,"
            "\"source_offset\":%zu,\"source_size\":%zu,\"sequence\":%u,\"interpretation\":%u,\"articulations\":%u,\"chord\":%s,\"midi_pitch\":",
            e->start_beats, e->duration_beats, e->written_start_beats, e->written_duration_beats, e->measure_index, e->occurrence,
            e->tie, e->source_offset, e->source_size, e->sequence, e->interpretation, e->articulations, e->chord ? "true" : "false") < 0) return -1;
        if (e->kind == HWA_MUSICXML_NOTE || e->kind == HWA_MUSICXML_GRACE) { if (fprintf(stream, "%.17g", e->midi_pitch) < 0) return -1; }
        else if (fputs("null", stream) == EOF) return -1;
        if (fputs(",\"velocity\":", stream) == EOF) return -1;
        if (e->velocity_valid && (e->kind == HWA_MUSICXML_NOTE || e->kind == HWA_MUSICXML_GRACE)) { if (fprintf(stream, "%.17g", e->velocity) < 0) return -1; }
        else if (fputs("null", stream) == EOF) return -1;
        {
            int grace = e->kind == HWA_MUSICXML_GRACE || (e->kind == HWA_MUSICXML_NOTE && e->written_duration_beats == 0.0);
            if (optional_number(stream, "tempo_bpm", e->tempo_bpm, e->kind == HWA_MUSICXML_TEMPO) != 0 ||
                optional_number(stream, "controller", (double)e->controller, e->kind == HWA_MUSICXML_CONTROL) != 0 ||
                optional_number(stream, "value", e->value, e->kind == HWA_MUSICXML_CONTROL ||
                    (e->kind == HWA_MUSICXML_MARK && e->mark && strcmp(e->mark, "velocity") == 0)) != 0 ||
                optional_number(stream, "grace_previous_percent", e->grace_previous, grace && e->grace_previous >= 0.0) != 0 ||
                optional_number(stream, "grace_following_percent", e->grace_following, grace && e->grace_following >= 0.0) != 0 ||
                optional_number(stream, "grace_make_beats", e->grace_make, grace && e->grace_make >= 0.0) != 0 || fputs("}", stream) == EOF) return -1;
        }
    }
    return fputs("]}\n", stream) == EOF || ferror(stream) ? -1 : 0;
}

int hwa_musicxml_report(FILE *stream, const HWAMusicXMLScore *score, const HWAMusicXMLOptions *options)
{
    HWANumericLocale locale;
    int result;
    if (hwa_c_numeric_locale_begin(&locale) != 0) return -1;
    result = write_report(stream, score, options);
    if (hwa_c_numeric_locale_end(&locale) != 0) result = -1;
    return result;
}

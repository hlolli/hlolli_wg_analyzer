#include "internal.h"
#include "numeric_locale.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HWA_SCORE_SAFE_INTEGER UINT64_C(9007199254740991)

typedef struct ScoreNote {
    const HWAPerformanceEvent *event;
    double pitch;
    uint64_t start_tick;
    uint64_t end_tick;
    size_t track;
    size_t lane;
    int midi;
} ScoreNote;

static const char *label(const char *text) { return text == NULL ? "" : text; }

static int track_order(const HWAPerformanceEvent *a, const HWAPerformanceEvent *b)
{
    int result = strcmp(label(a->part), label(b->part));
    return result != 0 ? result : strcmp(label(a->voice), label(b->voice));
}

static int note_order(const void *left, const void *right)
{
    const HWAPerformanceEvent *a = ((const ScoreNote *)left)->event;
    const HWAPerformanceEvent *b = ((const ScoreNote *)right)->event;
    int result = track_order(a, b);
    if (result != 0) return result;
    if (a->start_sample != b->start_sample) return a->start_sample < b->start_sample ? -1 : 1;
    if (a->end_sample != b->end_sample) return a->end_sample < b->end_sample ? -1 : 1;
    return a->id < b->id ? -1 : a->id > b->id ? 1 : 0;
}

void hwa_event_score_options_default(HWAEventScoreOptions *options)
{
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    options->kind = HWA_EVENT_SCORE_CSOUND;
    options->max_notes = 1000000U;
    options->max_tracks = 256U;
    options->max_lanes_per_track = 64U;
    options->max_work_bytes = UINT64_C(536870912);
}

static int pitch_value(const HWAPerformanceEvent *event, double nyquist, double *pitch)
{
    size_t i;
    unsigned count = 0U;
    for (i = 0U; i < event->value_count; ++i) {
        const HWAEventValue *value = &event->values[i];
        if (!value->selected || strcmp(value->name, "pitch-hz") != 0) continue;
        if (++count > 1U || value->kind != HWA_EVENT_VALUE_F64 ||
            strcmp(label(value->unit), "Hz") != 0 || !isfinite(value->number) ||
            value->number <= 0.0 || value->number >= nyquist) return -1;
        *pitch = value->number;
    }
    return count != 0U ? 1 : 0;
}

/* Round a sample position without converting the integer clock to double. */
static int tick_at(uint64_t sample, uint32_t rate, uint32_t tempo, uint64_t *tick)
{
    uint64_t divisor = (uint64_t)rate * 60U;
    uint64_t factor = (uint64_t)tempo * 32U;
    uint64_t whole = sample / divisor;
    uint64_t fraction = ((sample % divisor) * factor + divisor / 2U) / divisor;
    if (whole > (HWA_SCORE_SAFE_INTEGER - fraction) / factor) return -1;
    *tick = whole * factor + fraction;
    return 0;
}

static int number(FILE *stream, const HWANumericLocale *locale, double value)
{
    char text[64];
    return hwa_c_locale_format_double(locale, text, sizeof(text), value) != 0 ||
           fputs(text, stream) == EOF ? -1 : 0;
}

static int hex_label(FILE *stream, const char *text)
{
    const unsigned char *p = (const unsigned char *)label(text);
    if (*p == 0U) return fputc('-', stream) == EOF ? -1 : 0;
    for (; *p != 0U; ++p) if (fprintf(stream, "%02x", (unsigned)*p) < 0) return -1;
    return 0;
}

static int track_comment(FILE *stream, const char *prefix, size_t track,
                         const HWAPerformanceEvent *event)
{
    return fprintf(stream, "%s track=%zu part_utf8_hex=", prefix, track) < 0 ||
        hex_label(stream, event->part) != 0 || fputs(" voice_utf8_hex=", stream) == EOF ||
        hex_label(stream, event->voice) != 0 || fputc('\n', stream) == EOF ? -1 : 0;
}

static int lily_label(FILE *stream, const char *text)
{
    const unsigned char *p = (const unsigned char *)label(text);
    for (; *p != 0U; ++p) {
        if ((*p == '"' || *p == '\\') && fputc('\\', stream) == EOF) return -1;
        if (fputc(*p < 32U || *p == 127U ? ' ' : *p, stream) == EOF) return -1;
    }
    return 0;
}

static int csound(FILE *stream, const ScoreNote *notes, size_t count, size_t tracks,
                   size_t omitted, const HWAEventAudio *source, const HWANumericLocale *locale)
{
    size_t i;
    if (fprintf(stream, "; HWA_CSOUND_SCORE 2\n; source_id=%" PRIu64 " sample_rate_hz=%u notes=%zu tracks=%zu omitted_unpitched_notes=%zu\n"
        "; p1=track_instrument p4=pitch_hz p5=level p6=event_id p7=start_sample p8=end_sample p9=track_id\n",
        source->id, source->format.sample_rate_hz, count, tracks, omitted) < 0) return -1;
    for (i = 0U; i < count; ++i) {
        const HWAPerformanceEvent *event = notes[i].event;
        if ((i == 0U || notes[i].track != notes[i-1U].track) &&
            track_comment(stream, ";", notes[i].track, event) != 0) return -1;
        if (fprintf(stream, "i %zu ", notes[i].track) < 0 ||
            number(stream, locale, (double)event->start_sample / source->format.sample_rate_hz) != 0 ||
            fputc(' ', stream) == EOF ||
            number(stream, locale, (double)(event->end_sample-event->start_sample) / source->format.sample_rate_hz) != 0 ||
            fputc(' ', stream) == EOF || number(stream, locale, notes[i].pitch) != 0 ||
            fprintf(stream, " 0.2 %" PRIu64 " %" PRIu64 " %" PRIu64 " %zu\n",
                event->id, event->start_sample, event->end_sample, notes[i].track) < 0) return -1;
    }
    return fputs("f 0 ", stream) == EOF ||
        number(stream, locale, (double)source->format.frames / source->format.sample_rate_hz) != 0 ||
        fputs("\ne\n", stream) == EOF ? -1 : 0;
}

static void pitch_name(int midi, char text[24])
{
    static const char *const names[] = {"c", "cis", "d", "dis", "e", "f", "fis", "g", "gis", "a", "ais", "b"};
    int octave = midi / 12 - 4; /* Unmarked c is MIDI 48. */
    size_t length;
    strcpy(text, names[midi % 12]);
    length = strlen(text);
    while (octave != 0) {
        text[length++] = octave > 0 ? '\'' : ',';
        octave += octave > 0 ? -1 : 1;
    }
    text[length] = '\0';
}

static int duration(FILE *stream, const char *pitch, uint64_t ticks)
{
    unsigned piece = 128U;
    uint64_t whole = ticks / 128U;
    ticks %= 128U;
    /* Whole-note scaling keeps long silence/sustain output bounded. */
    if (whole != 0U && (fprintf(stream, "%s1*%" PRIu64 "%s ", pitch, whole,
            ticks != 0U && strcmp(pitch, "r") != 0 ? "~" : "") < 0)) return -1;
    while (ticks != 0U) {
        while ((uint64_t)piece > ticks) piece /= 2U;
        ticks -= piece;
        if (fprintf(stream, "%s%u%s ", pitch, 128U/piece,
                ticks != 0U && strcmp(pitch, "r") != 0 ? "~" : "") < 0) return -1;
    }
    return 0;
}

static int lilypond(FILE *stream, const ScoreNote *notes, size_t count, size_t tracks,
                    size_t omitted, const HWAEventAudio *source,
                    const HWAEventScoreOptions *options, const HWANumericLocale *locale)
{
    size_t first = 0U;
    if (fprintf(stream, "\\version \"2.24.0\"\n\\language \"nederlands\"\n"
        "%% HWA_LILYPOND_SCORE 1\n%% source_id=%" PRIu64 " sample_rate_hz=%u notes=%zu tracks=%zu omitted_unpitched_notes=%zu\n"
        "%% pitch=nearest-semitone-A440 rhythm=nearest-128th ties=up minimum_note=1-tick tempo_source=caller\n"
        "%% No key, meter, articulation, ornament, or instrument was inferred.\n"
        "\\score { <<\n", source->id, source->format.sample_rate_hz, count, tracks, omitted) < 0) return -1;
    while (first < count) {
        size_t last = first;
        size_t lanes = 0U;
        size_t lane;
        while (last < count && notes[last].track == notes[first].track) {
            if (notes[last].lane >= lanes) lanes = notes[last].lane+1U;
            last++;
        }
        if (track_comment(stream, "%", notes[first].track, notes[first].event) != 0 ||
            fprintf(stream, "\\new Staff = \"track%zu\" \\with { instrumentName = \"", notes[first].track) < 0 ||
            lily_label(stream, notes[first].event->part) != 0 || fputs(" / ", stream) == EOF ||
            lily_label(stream, notes[first].event->voice) != 0 || fputs("\" } <<\n", stream) == EOF) return -1;
        for (lane = 0U; lane < lanes; ++lane) {
            size_t i;
            uint64_t cursor = 0U;
            if (fprintf(stream, "\\new Voice { \\absolute { \\cadenzaOn \\omit Staff.TimeSignature \\autoBeamOff \\tempo 4 = %u\n",
                        options->tempo_bpm) < 0) return -1;
            for (i = first; i < last; ++i) {
                const HWAPerformanceEvent *event = notes[i].event;
                char pitch[24];
                if (notes[i].lane != lane) continue;
                if (duration(stream, "r", notes[i].start_tick-cursor) != 0 ||
                    fprintf(stream, "\n%% event_id=%" PRIu64 " start_sample=%" PRIu64 " end_sample=%" PRIu64 " pitch_hz=",
                        event->id, event->start_sample, event->end_sample) < 0 ||
                    number(stream, locale, notes[i].pitch) != 0 ||
                    fprintf(stream, " start_tick=%" PRIu64 " end_tick=%" PRIu64 " lane=%zu\n",
                        notes[i].start_tick, notes[i].end_tick, lane+1U) < 0) return -1;
                pitch_name(notes[i].midi, pitch);
                if (duration(stream, pitch, notes[i].end_tick-notes[i].start_tick) != 0) return -1;
                cursor = notes[i].end_tick;
            }
            if (fputs("\n} }\n", stream) == EOF) return -1;
        }
        if (fputs(">>\n", stream) == EOF) return -1;
        first = last;
    }
    return fputs(">> \\layout { } \\midi { } }\n", stream) == EOF ? -1 : 0;
}

int hwa_event_score_write(FILE *stream, const HWAEventBundle *bundle,
                          uint64_t source_recording_id, const HWAEventScoreOptions *options,
                          char *error, size_t error_size)
{
    HWAEventScoreOptions copied;
    HWAEventBundleLimits limits;
    const HWAEventAudio *source = NULL;
    ScoreNote *notes = NULL;
    uint64_t *lane_ends = NULL;
    size_t count = 0U, omitted = 0U, tracks = 0U, i;
    uint64_t bytes;
    HWANumericLocale locale;
    int result = -1;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (options != NULL) copied = *options;
    else hwa_event_score_options_default(&copied);
    if (stream == NULL || bundle == NULL ||
        (copied.kind != HWA_EVENT_SCORE_CSOUND && copied.kind != HWA_EVENT_SCORE_LILYPOND) ||
        (copied.kind == HWA_EVENT_SCORE_LILYPOND && (copied.tempo_bpm < 10U || copied.tempo_bpm > 1000U)) ||
        (copied.kind == HWA_EVENT_SCORE_CSOUND && copied.tempo_bpm != 0U) ||
        copied.max_notes == 0U || copied.max_tracks == 0U || copied.max_tracks > 65535U ||
        copied.max_lanes_per_track == 0U || copied.max_lanes_per_track > 256U || copied.max_work_bytes == 0U) {
        hwa_set_error(error, error_size, "invalid event score arguments or options");
        return -1;
    }
    hwa_event_bundle_limits_default(&limits);
    if (hwa_event_bundle_validate(bundle, &limits, error, error_size) != 0) return -1;
    for (i = 0U; i < bundle->audio_count; ++i) {
        if (bundle->audio[i].id == source_recording_id && bundle->audio[i].kind == HWA_EVENT_SOURCE_RECORDING)
            source = &bundle->audio[i];
    }
    if (source == NULL || source->format.sample_rate_hz == 0U || source->format.frames == 0U ||
        (copied.kind == HWA_EVENT_SCORE_CSOUND && source->format.frames > HWA_SCORE_SAFE_INTEGER)) {
        hwa_set_error(error, error_size, "event score needs a valid source recording clock");
        return -1;
    }
    for (i = 0U; i < bundle->event_count; ++i) {
        const HWAPerformanceEvent *event = &bundle->events[i];
        if (event->source_recording_id == source_recording_id && strcmp(event->kind, "note") == 0) count++;
    }
    if (count == 0U || count > copied.max_notes || count > SIZE_MAX / sizeof(*notes)) {
        hwa_set_error(error, error_size, "event score has no notes or exceeds the note limit");
        return -1;
    }
    bytes = (uint64_t)copied.max_lanes_per_track * sizeof(*lane_ends);
    if (bytes > copied.max_work_bytes || (uint64_t)count > (copied.max_work_bytes-bytes)/sizeof(*notes)) {
        hwa_set_error(error, error_size, "event score work limit exceeded");
        return -1;
    }
    notes = calloc(count, sizeof(*notes));
    lane_ends = calloc(copied.max_lanes_per_track, sizeof(*lane_ends));
    if (notes == NULL || lane_ends == NULL) {
        hwa_set_error(error, error_size, "cannot allocate event score");
        goto cleanup;
    }
    count = 0U;
    for (i = 0U; i < bundle->event_count; ++i) {
        const HWAPerformanceEvent *event = &bundle->events[i];
        double pitch = 0.0;
        int present;
        if (event->source_recording_id != source_recording_id || strcmp(event->kind, "note") != 0) continue;
        if (event->start_sample >= event->end_sample) {
            hwa_set_error(error, error_size, "event score note has an empty span");
            goto cleanup;
        }
        present = pitch_value(event, 0.5*(double)source->format.sample_rate_hz, &pitch);
        if (present < 0) {
            hwa_set_error(error, error_size, "event score has an invalid or ambiguous selected pitch");
            goto cleanup;
        }
        if (present == 0) { omitted++; continue; }
        notes[count].event = event;
        notes[count].pitch = pitch;
        if (copied.kind == HWA_EVENT_SCORE_LILYPOND) {
            double midi = floor(69.0 + 12.0*log2(pitch/440.0) + 0.5);
            if (midi < 0.0 || midi > 127.0 ||
                tick_at(event->start_sample, source->format.sample_rate_hz, copied.tempo_bpm, &notes[count].start_tick) != 0 ||
                tick_at(event->end_sample, source->format.sample_rate_hz, copied.tempo_bpm, &notes[count].end_tick) != 0) {
                hwa_set_error(error, error_size, "note is outside the LilyPond export pitch or time range");
                goto cleanup;
            }
            notes[count].midi = (int)midi;
            if (notes[count].end_tick <= notes[count].start_tick) notes[count].end_tick = notes[count].start_tick+1U;
        } else if (event->id > HWA_SCORE_SAFE_INTEGER) {
            hwa_set_error(error, error_size, "Csound event ID exceeds exact numeric range");
            goto cleanup;
        }
        count++;
    }
    if (count == 0U) {
        hwa_set_error(error, error_size, "event score has no selected pitched notes");
        goto cleanup;
    }
    qsort(notes, count, sizeof(*notes), note_order);
    for (i = 0U; i < count; ++i) {
        size_t lane = 0U;
        if (i == 0U || track_order(notes[i-1U].event, notes[i].event) != 0) {
            tracks++;
            memset(lane_ends, 0, copied.max_lanes_per_track*sizeof(*lane_ends));
        }
        if (tracks > copied.max_tracks) {
            hwa_set_error(error, error_size, "event score track limit exceeded");
            goto cleanup;
        }
        notes[i].track = tracks;
        if (copied.kind != HWA_EVENT_SCORE_LILYPOND) continue;
        while (lane < copied.max_lanes_per_track && lane_ends[lane] > notes[i].start_tick) lane++;
        if (lane == copied.max_lanes_per_track) {
            hwa_set_error(error, error_size, "LilyPond overlap lane limit exceeded");
            goto cleanup;
        }
        notes[i].lane = lane;
        lane_ends[lane] = notes[i].end_tick;
    }
    memset(&locale, 0, sizeof(locale));
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_set_error(error, error_size, "cannot enter C numeric locale");
        goto cleanup;
    }
    result = copied.kind == HWA_EVENT_SCORE_CSOUND ?
        csound(stream, notes, count, tracks, omitted, source, &locale) :
        lilypond(stream, notes, count, tracks, omitted, source, &copied, &locale);
    if (hwa_c_numeric_locale_end(&locale) != 0 || ferror(stream)) result = -1;
    if (result != 0) hwa_set_error(error, error_size, "cannot write event score");
cleanup:
    free(notes);
    free(lane_ends);
    return result;
}

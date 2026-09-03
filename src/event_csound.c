#include "event_csound.h"

#include "internal.h"
#include "numeric_locale.h"

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct HWAEventCsoundNote {
    const HWAPerformanceEvent *event;
    double pitch_hz;
} HWAEventCsoundNote;

#define HWA_EVENT_CSOUND_SAFE_INTEGER UINT64_C(9007199254740991)

static const HWAEventAudio *hwa_event_csound_source(
    const HWAEventBundle *bundle,
    uint64_t source_recording_id)
{
    size_t index;
    if (bundle == NULL) return NULL;
    for (index = 0U; index < bundle->audio_count; ++index) {
        const HWAEventAudio *audio = &bundle->audio[index];
        if (audio->id == source_recording_id &&
            audio->kind == HWA_EVENT_SOURCE_RECORDING) return audio;
    }
    return NULL;
}

static int hwa_event_csound_pitch(const HWAPerformanceEvent *event,
                                  double nyquist_hz,
                                  double *pitch_hz)
{
    size_t index;
    size_t selected = 0U;
    double pitch = 0.0;
    if (event->value_count != 0U && event->values == NULL) return 0;
    for (index = 0U; index < event->value_count; ++index) {
        const HWAEventValue *value = &event->values[index];
        if (value->name == NULL || strcmp(value->name, "pitch-hz") != 0 ||
            !value->selected) continue;
        if (value->kind != HWA_EVENT_VALUE_F64 || value->unit == NULL ||
            strcmp(value->unit, "Hz") != 0 || !isfinite(value->number) ||
            value->number <= 0.0 || value->number >= nyquist_hz) return 0;
        pitch = value->number;
        selected++;
    }
    if (selected != 1U) return 0;
    *pitch_hz = pitch;
    return 1;
}

static int hwa_event_csound_note_compare(const void *left, const void *right)
{
    const HWAEventCsoundNote *first = (const HWAEventCsoundNote *)left;
    const HWAEventCsoundNote *second = (const HWAEventCsoundNote *)right;
    if (first->event->start_sample < second->event->start_sample) return -1;
    if (first->event->start_sample > second->event->start_sample) return 1;
    if (first->event->end_sample < second->event->end_sample) return -1;
    if (first->event->end_sample > second->event->end_sample) return 1;
    if (first->event->id < second->event->id) return -1;
    if (first->event->id > second->event->id) return 1;
    return 0;
}

static int hwa_event_csound_number(FILE *stream,
                                   const HWANumericLocale *locale,
                                   double value)
{
    char text[64];
    return hwa_c_locale_format_double(locale, text, sizeof(text), value) != 0 ||
                   fputs(text, stream) == EOF
               ? -1
               : 0;
}

int hwa_event_csound_score_write(FILE *stream,
                                 const HWAEventBundle *bundle,
                                 uint64_t source_recording_id,
                                 char *error,
                                 size_t error_size)
{
    const HWAEventAudio *source;
    HWAEventBundleLimits limits;
    HWAEventCsoundNote *notes = NULL;
    HWANumericLocale locale;
    size_t note_count = 0U;
    size_t index;
    int locale_active = 0;
    int result = -1;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    memset(&locale, 0, sizeof(locale));
    hwa_event_bundle_limits_default(&limits);
    if (hwa_event_bundle_validate(bundle, &limits,
                                  error, error_size) != 0)
        return -1;
    source = hwa_event_csound_source(bundle, source_recording_id);
    if (stream == NULL || source == NULL ||
        source->format.sample_rate_hz == 0U || source->format.frames == 0U ||
        (bundle->event_count != 0U && bundle->events == NULL)) {
        hwa_set_error(error, error_size,
                      "invalid event Csound score arguments");
        return -1;
    }
    if (bundle->event_count > SIZE_MAX / sizeof(*notes)) {
        hwa_set_error(error, error_size, "too many Csound score events");
        return -1;
    }
    notes = (HWAEventCsoundNote *)calloc(bundle->event_count, sizeof(*notes));
    if (notes == NULL && bundle->event_count != 0U) {
        hwa_set_error(error, error_size,
                      "out of memory for Csound score events");
        return -1;
    }
    for (index = 0U; index < bundle->event_count; ++index) {
        const HWAPerformanceEvent *event = &bundle->events[index];
        double pitch_hz;
        if (event->source_recording_id != source_recording_id ||
            event->kind == NULL || strcmp(event->kind, "note") != 0) continue;
        if (event->id == 0U ||
            event->id > HWA_EVENT_CSOUND_SAFE_INTEGER ||
            event->start_sample > HWA_EVENT_CSOUND_SAFE_INTEGER ||
            event->end_sample > HWA_EVENT_CSOUND_SAFE_INTEGER ||
            event->start_sample >= event->end_sample ||
            event->end_sample > source->format.frames) {
            hwa_set_error(error, error_size,
                          "invalid note event for Csound score");
            goto cleanup;
        }
        if (!hwa_event_csound_pitch(
                event, 0.5 * (double)source->format.sample_rate_hz,
                &pitch_hz)) continue;
        notes[note_count].event = event;
        notes[note_count].pitch_hz = pitch_hz;
        note_count++;
    }
    if (note_count == 0U) {
        hwa_set_error(error, error_size,
                      "event source has no renderable note events");
        goto cleanup;
    }
    qsort(notes, note_count, sizeof(*notes), hwa_event_csound_note_compare);
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_set_error(error, error_size, "cannot enter C numeric locale");
        goto cleanup;
    }
    locale_active = 1;
    if (fputs("; HWA_CSOUND_SCORE 1\n"
              "; p4=pitch_hz p5=level p6=event_id "
              "p7=start_sample p8=end_sample\n",
              stream) == EOF) goto write_error;
    for (index = 0U; index < note_count; ++index) {
        const HWAPerformanceEvent *event = notes[index].event;
        double start_seconds = (double)event->start_sample /
                               (double)source->format.sample_rate_hz;
        double duration_seconds =
            (double)(event->end_sample - event->start_sample) /
            (double)source->format.sample_rate_hz;
        if (fputs("i 1 ", stream) == EOF ||
            hwa_event_csound_number(stream, &locale, start_seconds) != 0 ||
            fputc(' ', stream) == EOF ||
            hwa_event_csound_number(stream, &locale, duration_seconds) != 0 ||
            fputc(' ', stream) == EOF ||
            hwa_event_csound_number(stream, &locale,
                                    notes[index].pitch_hz) != 0 ||
            fprintf(stream, " 0.2 %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
                    event->id, event->start_sample, event->end_sample) < 0)
            goto write_error;
    }
    if (fputs("f 0 ", stream) == EOF ||
        hwa_event_csound_number(
            stream, &locale,
            (double)source->format.frames /
                (double)source->format.sample_rate_hz) != 0 ||
        fputs("\ne\n", stream) == EOF || ferror(stream)) goto write_error;
    result = 0;
    goto cleanup;

write_error:
    hwa_set_error(error, error_size, "cannot write Csound score");
cleanup:
    if (locale_active && hwa_c_numeric_locale_end(&locale) != 0 &&
        result == 0) {
        hwa_set_error(error, error_size, "cannot restore numeric locale");
        result = -1;
    }
    free(notes);
    return result;
}

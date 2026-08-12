#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "score_manifest.h"

#include "internal.h"
#include "sha256.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define HWA_SCORE_COLUMN_COUNT 12U

typedef int (*HWAScoreRowFunction)(char **fields,
                                   size_t field_count,
                                   size_t row,
                                   void *user,
                                   char *error,
                                   size_t error_size);

typedef struct HWAScoreParseState {
    HWAScoreManifest *manifest;
    size_t capacity;
    size_t max_events;
    double previous_start;
    int have_previous_start;
} HWAScoreParseState;

static char *hwa_score_copy_text(const char *text)
{
    size_t length = strlen(text);
    char *copy;

    if (length == SIZE_MAX) {
        return NULL;
    }
    copy = (char *)malloc(length + 1U);
    if (copy != NULL) {
        memcpy(copy, text, length + 1U);
    }
    return copy;
}

static int hwa_score_read_file(const char *path,
                               uint64_t max_bytes,
                               unsigned char **data,
                               size_t *data_size,
                               char sha256[HWA_SHA256_HEX_SIZE],
                               char *error,
                               size_t error_size)
{
    FILE *stream;
    uint64_t size_u64;
    size_t size;
    unsigned char *buffer;
    unsigned char digest[32];
    HWASha256 hash;
#if defined(_WIN32)
    _dev_t expected_device;
    _ino_t expected_inode;
#else
    dev_t expected_device;
    ino_t expected_inode;
#endif

    *data = NULL;
    *data_size = 0U;
    if (path == NULL || strcmp(path, "-") == 0 || max_bytes == 0U) {
        hwa_set_error(error, error_size,
                      "score manifest must be a named regular file");
        return -1;
    }
#if defined(_WIN32)
    {
        struct _stat64 status;
        if (_stat64(path, &status) != 0) {
            hwa_set_error(error, error_size,
                          "cannot inspect score manifest '%s': %s", path,
                          strerror(errno));
            return -1;
        }
        if ((status.st_mode & _S_IFMT) != _S_IFREG || status.st_size < 0) {
            hwa_set_error(error, error_size,
                          "score manifest is not a regular file");
            return -1;
        }
        size_u64 = (uint64_t)status.st_size;
        expected_device = status.st_dev;
        expected_inode = status.st_ino;
    }
#else
    {
        struct stat status;
        if (stat(path, &status) != 0) {
            hwa_set_error(error, error_size,
                          "cannot inspect score manifest '%s': %s", path,
                          strerror(errno));
            return -1;
        }
        if (!S_ISREG(status.st_mode) || status.st_size < 0) {
            hwa_set_error(error, error_size,
                          "score manifest is not a regular file");
            return -1;
        }
        size_u64 = (uint64_t)status.st_size;
        expected_device = status.st_dev;
        expected_inode = status.st_ino;
    }
#endif
    if (size_u64 > max_bytes || size_u64 > (uint64_t)(SIZE_MAX - 1U)) {
        hwa_set_error(error, error_size,
                      "score manifest exceeds the byte limit");
        return -1;
    }
    stream = fopen(path, "rb");
    if (stream == NULL) {
        hwa_set_error(error, error_size,
                      "cannot open score manifest '%s': %s",
                      path, strerror(errno));
        return -1;
    }
#if defined(_WIN32)
    {
        struct _stat64 opened;
        int descriptor = _fileno(stream);
        if (descriptor < 0 || _fstat64(descriptor, &opened) != 0 ||
            (opened.st_mode & _S_IFMT) != _S_IFREG || opened.st_size < 0 ||
            opened.st_dev != expected_device || opened.st_ino != expected_inode ||
            (uint64_t)opened.st_size != size_u64) {
            hwa_set_error(error, error_size,
                          "score manifest changed before it was opened");
            (void)fclose(stream);
            return -1;
        }
    }
#else
    {
        struct stat opened;
        int descriptor = fileno(stream);
        if (descriptor < 0 || fstat(descriptor, &opened) != 0 ||
            !S_ISREG(opened.st_mode) || opened.st_size < 0 ||
            opened.st_dev != expected_device || opened.st_ino != expected_inode ||
            (uint64_t)opened.st_size != size_u64) {
            hwa_set_error(error, error_size,
                          "score manifest changed before it was opened");
            (void)fclose(stream);
            return -1;
        }
    }
#endif
    size = (size_t)size_u64;
    buffer = (unsigned char *)malloc(size + 1U);
    if (buffer == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for the score manifest");
        (void)fclose(stream);
        return -1;
    }
    if (size != 0U && fread(buffer, 1U, size, stream) != size) {
        hwa_set_error(error, error_size,
                      "cannot read score manifest '%s'", path);
        free(buffer);
        (void)fclose(stream);
        return -1;
    }
    if (fgetc(stream) != EOF) {
        hwa_set_error(error, error_size,
                      "score manifest changed while it was read");
        free(buffer);
        (void)fclose(stream);
        return -1;
    }
    if (ferror(stream) || fclose(stream) != 0) {
        hwa_set_error(error, error_size,
                      "cannot finish reading score manifest '%s'", path);
        free(buffer);
        return -1;
    }
    buffer[size] = 0U;
    hwa_sha256_init(&hash);
    hwa_sha256_update(&hash, buffer, size);
    hwa_sha256_final(&hash, digest);
    hwa_sha256_hex(digest, sha256);
    *data = buffer;
    *data_size = size;
    return 0;
}

static int hwa_score_csv_rows(const unsigned char *data,
                              size_t size,
                              HWAScoreRowFunction function,
                              void *user,
                              char *error,
                              size_t error_size)
{
    char **fields;
    char *storage;
    size_t position = 0U;
    size_t physical_row = 1U;
    int saw_row = 0;

    if (size > (SIZE_MAX - 1U) / sizeof(char *)) {
        hwa_set_error(error, error_size, "score manifest is too large");
        return -1;
    }
    fields = (char **)malloc(HWA_SCORE_COLUMN_COUNT * sizeof(*fields));
    storage = (char *)malloc(size + 1U);
    if (fields == NULL || storage == NULL) {
        free(storage);
        free(fields);
        hwa_set_error(error, error_size,
                      "out of memory while parsing the score manifest");
        return -1;
    }
    while (position < size) {
        size_t field_count = 0U;
        size_t output = 0U;
        size_t logical_row = physical_row;
        int row_done = 0;

        while (!row_done) {
            int quoted = 0;
            int closed_quote = 0;

            if (field_count == HWA_SCORE_COLUMN_COUNT) {
                hwa_set_error(error, error_size,
                              "score row %zu has too many fields", logical_row);
                free(storage);
                free(fields);
                return -1;
            }
            fields[field_count++] = storage + output;
            if (position < size && data[position] == (unsigned char)'\"') {
                quoted = 1;
                position++;
            }
            while (position < size) {
                unsigned char byte = data[position];
                if (byte == 0U) {
                    hwa_set_error(error, error_size,
                                  "score row %zu contains a NUL byte",
                                  logical_row);
                    free(storage);
                    free(fields);
                    return -1;
                }
                if (quoted) {
                    if (byte == (unsigned char)'\"') {
                        if (position + 1U < size &&
                            data[position + 1U] == (unsigned char)'\"') {
                            storage[output++] = '\"';
                            position += 2U;
                            continue;
                        }
                        closed_quote = 1;
                        position++;
                        break;
                    }
                    if (byte == (unsigned char)'\r') {
                        if (position + 1U >= size ||
                            data[position + 1U] != (unsigned char)'\n') {
                            hwa_set_error(error, error_size,
                                          "score row %zu has a bare carriage return",
                                          logical_row);
                            free(storage);
                            free(fields);
                            return -1;
                        }
                        storage[output++] = '\r';
                        storage[output++] = '\n';
                        position += 2U;
                        physical_row++;
                        continue;
                    }
                    storage[output++] = (char)byte;
                    if (byte == (unsigned char)'\n') {
                        physical_row++;
                    }
                    position++;
                    continue;
                }
                if (byte == (unsigned char)'\"') {
                    hwa_set_error(error, error_size,
                                  "score row %zu has an unescaped quote",
                                  logical_row);
                    free(storage);
                    free(fields);
                    return -1;
                }
                if (byte == (unsigned char)',' ||
                    byte == (unsigned char)'\r' ||
                    byte == (unsigned char)'\n') {
                    break;
                }
                storage[output++] = (char)byte;
                position++;
            }
            if (quoted && !closed_quote) {
                hwa_set_error(error, error_size,
                              "score row %zu has an unclosed quote", logical_row);
                free(storage);
                free(fields);
                return -1;
            }
            storage[output++] = '\0';
            if (position == size) {
                row_done = 1;
            } else if (data[position] == (unsigned char)',') {
                position++;
            } else if (data[position] == (unsigned char)'\n') {
                position++;
                physical_row++;
                row_done = 1;
            } else if (data[position] == (unsigned char)'\r') {
                if (position + 1U >= size ||
                    data[position + 1U] != (unsigned char)'\n') {
                    hwa_set_error(error, error_size,
                                  "score row %zu has a bare carriage return",
                                  logical_row);
                    free(storage);
                    free(fields);
                    return -1;
                }
                position += 2U;
                physical_row++;
                row_done = 1;
            } else {
                hwa_set_error(error, error_size,
                              "score row %zu has text after a closing quote",
                              logical_row);
                free(storage);
                free(fields);
                return -1;
            }
        }
        if (field_count == 1U && fields[0][0] == '\0') {
            hwa_set_error(error, error_size,
                          "score manifest contains an empty row at %zu",
                          logical_row);
            free(storage);
            free(fields);
            return -1;
        }
        if (function(fields, field_count, logical_row, user,
                     error, error_size) != 0) {
            free(storage);
            free(fields);
            return -1;
        }
        saw_row = 1;
    }
    free(storage);
    free(fields);
    if (!saw_row) {
        hwa_set_error(error, error_size, "score manifest is empty");
        return -1;
    }
    return 0;
}

static int hwa_score_parse_double(const char *text,
                                  int allow_blank,
                                  double *value)
{
    char *end = NULL;
    double parsed;

    if (text[0] == '\0') {
        return allow_blank ? 1 : -1;
    }
    if (isspace((unsigned char)text[0])) {
        return -1;
    }
    errno = 0;
    parsed = strtod(text, &end);
    if (errno == ERANGE || end == text || *end != '\0' || !isfinite(parsed)) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static int hwa_score_parse_integer(const char *text,
                                   int allow_blank,
                                   int *value)
{
    char *end = NULL;
    long parsed;

    if (text[0] == '\0') {
        return allow_blank ? 1 : -1;
    }
    if (isspace((unsigned char)text[0])) {
        return -1;
    }
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        parsed < 0L || parsed > 127L) {
        return -1;
    }
    *value = (int)parsed;
    return 0;
}

static int hwa_score_header_row(char **fields,
                                size_t field_count,
                                size_t row,
                                char *error,
                                size_t error_size)
{
    static const char *const expected[HWA_SCORE_COLUMN_COUNT] = {
        "event_id", "kind", "start_beats", "duration_beats",
        "midi_note", "velocity", "voice", "tie", "dynamic", "mark",
        "score_position", "tempo_bpm"
    };
    size_t column;

    if (field_count != HWA_SCORE_COLUMN_COUNT) {
        hwa_set_error(error, error_size,
                      "score header row %zu must have 12 fields", row);
        return -1;
    }
    for (column = 0U; column < HWA_SCORE_COLUMN_COUNT; ++column) {
        if (strcmp(fields[column], expected[column]) != 0) {
            hwa_set_error(error, error_size,
                          "score header column %zu must be '%s'",
                          column + 1U, expected[column]);
            return -1;
        }
    }
    return 0;
}

static int hwa_score_kind(const char *text, HWAScoreEventKind *kind)
{
    if (strcmp(text, "note") == 0) {
        *kind = HWA_SCORE_NOTE;
    } else if (strcmp(text, "rest") == 0) {
        *kind = HWA_SCORE_REST;
    } else if (strcmp(text, "tempo") == 0) {
        *kind = HWA_SCORE_TEMPO;
    } else if (strcmp(text, "ornament") == 0) {
        *kind = HWA_SCORE_ORNAMENT;
    } else if (strcmp(text, "cadenza") == 0) {
        *kind = HWA_SCORE_CADENZA;
    } else {
        return -1;
    }
    return 0;
}

static int hwa_score_tie(const char *text, HWAScoreTie *tie)
{
    if (text[0] == '\0' || strcmp(text, "none") == 0) {
        *tie = HWA_SCORE_TIE_NONE;
    } else if (strcmp(text, "start") == 0) {
        *tie = HWA_SCORE_TIE_START;
    } else if (strcmp(text, "continue") == 0) {
        *tie = HWA_SCORE_TIE_CONTINUE;
    } else if (strcmp(text, "stop") == 0) {
        *tie = HWA_SCORE_TIE_STOP;
    } else {
        return -1;
    }
    return 0;
}

static void hwa_score_event_free(HWAScoreEvent *event)
{
    free(event->event_id);
    free(event->kind_text);
    free(event->midi_note_text);
    free(event->velocity_text);
    free(event->voice);
    free(event->tie_text);
    free(event->dynamic);
    free(event->mark);
    free(event->score_position);
    memset(event, 0, sizeof(*event));
}

static int hwa_score_append_event(HWAScoreParseState *state,
                                  HWAScoreEvent *event,
                                  char *error,
                                  size_t error_size)
{
    HWAScoreEvent *grown;
    size_t capacity;

    if (state->manifest->event_count == state->max_events) {
        hwa_set_error(error, error_size,
                      "score manifest exceeds the event limit");
        return -1;
    }
    if (state->manifest->event_count == state->capacity) {
        capacity = state->capacity == 0U ? 64U : state->capacity * 2U;
        if (capacity < state->capacity || capacity > state->max_events) {
            capacity = state->max_events;
        }
        if (capacity > SIZE_MAX / sizeof(*grown)) {
            hwa_set_error(error, error_size, "too many score events");
            return -1;
        }
        grown = (HWAScoreEvent *)realloc(
            state->manifest->events, capacity * sizeof(*grown));
        if (grown == NULL) {
            hwa_set_error(error, error_size,
                          "out of memory for score events");
            return -1;
        }
        state->manifest->events = grown;
        state->capacity = capacity;
    }
    state->manifest->events[state->manifest->event_count++] = *event;
    memset(event, 0, sizeof(*event));
    return 0;
}

static int hwa_score_data_row(char **fields,
                              size_t field_count,
                              size_t row,
                              HWAScoreParseState *state,
                              char *error,
                              size_t error_size)
{
    HWAScoreEvent event;
    int numeric_result;

    memset(&event, 0, sizeof(event));
    event.source_row = row;
    if (field_count != HWA_SCORE_COLUMN_COUNT) {
        hwa_set_error(error, error_size,
                      "score row %zu must have 12 fields", row);
        return -1;
    }
    if (fields[0][0] == '\0') {
        hwa_set_error(error, error_size,
                      "score row %zu has an empty event_id", row);
        return -1;
    }
    if (hwa_score_kind(fields[1], &event.kind) != 0) {
        hwa_set_error(error, error_size,
                      "score row %zu has an invalid kind", row);
        return -1;
    }
    if (hwa_score_parse_double(fields[2], 0, &event.start_beats) != 0 ||
        event.start_beats < 0.0) {
        hwa_set_error(error, error_size,
                      "score row %zu has an invalid start_beats", row);
        return -1;
    }
    numeric_result = hwa_score_parse_double(fields[3], 1,
                                            &event.duration_beats);
    if (numeric_result < 0 ||
        (event.kind == HWA_SCORE_TEMPO &&
         numeric_result == 0 && event.duration_beats != 0.0) ||
        (event.kind != HWA_SCORE_TEMPO &&
         (numeric_result != 0 || event.duration_beats <= 0.0))) {
        hwa_set_error(error, error_size,
                      "score row %zu has an invalid duration_beats", row);
        return -1;
    }
    numeric_result = hwa_score_parse_integer(fields[4], 1, &event.midi_note);
    if (numeric_result < 0 ||
        ((event.kind == HWA_SCORE_NOTE || event.kind == HWA_SCORE_ORNAMENT) &&
         numeric_result != 0) ||
        (event.kind != HWA_SCORE_NOTE && event.kind != HWA_SCORE_ORNAMENT &&
         numeric_result == 0)) {
        hwa_set_error(error, error_size,
                      "score row %zu has an invalid midi_note", row);
        return -1;
    }
    event.midi_note_valid = numeric_result == 0;
    numeric_result = hwa_score_parse_integer(fields[5], 1, &event.velocity);
    if (numeric_result < 0 ||
        (event.kind == HWA_SCORE_TEMPO && numeric_result == 0)) {
        hwa_set_error(error, error_size,
                      "score row %zu has an invalid velocity", row);
        return -1;
    }
    event.velocity_valid = numeric_result == 0;
    if (hwa_score_tie(fields[7], &event.tie) != 0 ||
        (event.kind != HWA_SCORE_NOTE && event.kind != HWA_SCORE_ORNAMENT &&
         event.tie != HWA_SCORE_TIE_NONE)) {
        hwa_set_error(error, error_size,
                      "score row %zu has an invalid tie", row);
        return -1;
    }
    numeric_result = hwa_score_parse_double(fields[11], 1, &event.tempo_bpm);
    if (numeric_result < 0 ||
        (event.kind == HWA_SCORE_TEMPO &&
         (numeric_result != 0 || event.tempo_bpm <= 0.0)) ||
        (event.kind != HWA_SCORE_TEMPO && numeric_result == 0)) {
        hwa_set_error(error, error_size,
                      "score row %zu has an invalid tempo_bpm", row);
        return -1;
    }
    event.tempo_valid = numeric_result == 0;
    if (state->have_previous_start &&
        event.start_beats < state->previous_start) {
        hwa_set_error(error, error_size,
                      "score row %zu moves backward; unfold the timeline",
                      row);
        return -1;
    }
    state->previous_start = event.start_beats;
    state->have_previous_start = 1;
    event.end_beats = event.start_beats + event.duration_beats;
    if (!isfinite(event.end_beats)) {
        hwa_set_error(error, error_size,
                      "score row %zu ends outside the numeric range", row);
        return -1;
    }
    event.event_id = hwa_score_copy_text(fields[0]);
    event.kind_text = hwa_score_copy_text(fields[1]);
    event.midi_note_text = hwa_score_copy_text(fields[4]);
    event.velocity_text = hwa_score_copy_text(fields[5]);
    event.voice = hwa_score_copy_text(fields[6]);
    event.tie_text = hwa_score_copy_text(fields[7]);
    event.dynamic = hwa_score_copy_text(fields[8]);
    event.mark = hwa_score_copy_text(fields[9]);
    event.score_position = hwa_score_copy_text(fields[10]);
    if (event.event_id == NULL || event.kind_text == NULL ||
        event.midi_note_text == NULL || event.velocity_text == NULL ||
        event.voice == NULL || event.tie_text == NULL ||
        event.dynamic == NULL || event.mark == NULL ||
        event.score_position == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for score row %zu", row);
        hwa_score_event_free(&event);
        return -1;
    }
    if (hwa_score_append_event(state, &event, error, error_size) != 0) {
        hwa_score_event_free(&event);
        return -1;
    }
    return 0;
}

static int hwa_score_parse_row(char **fields,
                               size_t field_count,
                               size_t row,
                               void *user,
                               char *error,
                               size_t error_size)
{
    HWAScoreParseState *state = (HWAScoreParseState *)user;

    if (state->manifest->event_count == 0U && row == 1U) {
        return hwa_score_header_row(fields, field_count, row,
                                    error, error_size);
    }
    return hwa_score_data_row(fields, field_count, row, state,
                              error, error_size);
}

static int hwa_score_id_pointer_compare(const void *left, const void *right)
{
    const char *const *left_id = (const char *const *)left;
    const char *const *right_id = (const char *const *)right;
    return strcmp(*left_id, *right_id);
}

static int hwa_score_tie_event_pointer_compare(const void *left,
                                               const void *right)
{
    const HWAScoreEvent *const *first =
        (const HWAScoreEvent *const *)left;
    const HWAScoreEvent *const *second =
        (const HWAScoreEvent *const *)right;
    int voice_order = strcmp((*first)->voice, (*second)->voice);

    if (voice_order != 0) {
        return voice_order;
    }
    if ((*first)->midi_note < (*second)->midi_note) return -1;
    if ((*first)->midi_note > (*second)->midi_note) return 1;
    if ((*first)->start_beats < (*second)->start_beats) return -1;
    if ((*first)->start_beats > (*second)->start_beats) return 1;
    return (*first)->source_row < (*second)->source_row ? -1 :
           (*first)->source_row > (*second)->source_row ? 1 : 0;
}

static int hwa_score_beats_touch(double first, double second)
{
    double scale = fmax(1.0, fmax(fabs(first), fabs(second)));
    return fabs(first - second) <= 1e-9 * scale;
}

static int hwa_score_validate_ties(HWAScoreManifest *manifest,
                                   char *error,
                                   size_t error_size)
{
    HWAScoreEvent **notes;
    size_t note_count = 0U;
    size_t index;
    size_t group_start;

    for (index = 0U; index < manifest->event_count; ++index) {
        if (manifest->events[index].midi_note_valid) {
            note_count++;
        }
    }
    if (note_count == 0U) {
        return 0;
    }
    if (note_count > SIZE_MAX / sizeof(*notes)) {
        hwa_set_error(error, error_size, "too many score tie events");
        return -1;
    }
    notes = (HWAScoreEvent **)malloc(note_count * sizeof(*notes));
    if (notes == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory while checking score ties");
        return -1;
    }
    note_count = 0U;
    for (index = 0U; index < manifest->event_count; ++index) {
        if (manifest->events[index].midi_note_valid) {
            notes[note_count++] = &manifest->events[index];
        }
    }
    qsort(notes, note_count, sizeof(*notes),
          hwa_score_tie_event_pointer_compare);
    group_start = 0U;
    while (group_start < note_count) {
        const char *voice = notes[group_start]->voice;
        int midi_note = notes[group_start]->midi_note;
        size_t group_end = group_start;
        int open = 0;
        double expected_beat = 0.0;
        size_t open_row = 0U;

        while (group_end < note_count &&
               notes[group_end]->midi_note == midi_note &&
               strcmp(notes[group_end]->voice, voice) == 0) {
            HWAScoreEvent *event = notes[group_end];

            if (event->tie == HWA_SCORE_TIE_START) {
                if (open) {
                    hwa_set_error(error, error_size,
                                  "score row %zu tie starts before row %zu closes",
                                  event->source_row, open_row);
                    free(notes);
                    return -1;
                }
                open = 1;
                open_row = event->source_row;
                expected_beat = event->end_beats;
            } else if (event->tie == HWA_SCORE_TIE_CONTINUE ||
                       event->tie == HWA_SCORE_TIE_STOP) {
                if (!open ||
                    !hwa_score_beats_touch(event->start_beats,
                                           expected_beat)) {
                    hwa_set_error(error, error_size,
                                  "score row %zu tie '%s' has no contiguous start",
                                  event->source_row, event->tie_text);
                    free(notes);
                    return -1;
                }
                expected_beat = event->end_beats;
                if (event->tie == HWA_SCORE_TIE_STOP) {
                    open = 0;
                }
            } else if (open) {
                hwa_set_error(error, error_size,
                              "score row %zu interrupts the tie from row %zu",
                              event->source_row, open_row);
                free(notes);
                return -1;
            }
            group_end++;
        }
        if (open) {
            hwa_set_error(error, error_size,
                          "score row %zu starts a tie with no stop",
                          open_row);
            free(notes);
            return -1;
        }
        group_start = group_end;
    }
    free(notes);
    return 0;
}

static int hwa_score_finish_manifest(HWAScoreManifest *manifest,
                                     char *error,
                                     size_t error_size)
{
    char **ids;
    size_t index;
    size_t tempo_index = 0U;
    size_t tempo_count = 0U;
    double duration_beats = 0.0;

    if (manifest->event_count == 0U) {
        hwa_set_error(error, error_size,
                      "score manifest has a header but no events");
        return -1;
    }
    if (manifest->event_count > SIZE_MAX / sizeof(*ids)) {
        hwa_set_error(error, error_size, "too many score event IDs");
        return -1;
    }
    ids = (char **)malloc(manifest->event_count * sizeof(*ids));
    if (ids == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory while checking score event IDs");
        return -1;
    }
    for (index = 0U; index < manifest->event_count; ++index) {
        ids[index] = manifest->events[index].event_id;
        if (manifest->events[index].kind == HWA_SCORE_TEMPO) {
            tempo_count++;
        } else if (manifest->events[index].end_beats > duration_beats) {
            duration_beats = manifest->events[index].end_beats;
        }
    }
    qsort(ids, manifest->event_count, sizeof(*ids),
          hwa_score_id_pointer_compare);
    for (index = 1U; index < manifest->event_count; ++index) {
        if (strcmp(ids[index - 1U], ids[index]) == 0) {
            hwa_set_error(error, error_size,
                          "score manifest repeats event_id '%s'", ids[index]);
            free(ids);
            return -1;
        }
    }
    free(ids);
    if (hwa_score_validate_ties(manifest, error, error_size) != 0) {
        return -1;
    }
    if (tempo_count == 0U || tempo_count > SIZE_MAX / sizeof(*manifest->tempo_points)) {
        hwa_set_error(error, error_size,
                      "score manifest needs a tempo row at beat 0");
        return -1;
    }
    manifest->tempo_points = (HWAScoreTempoPoint *)calloc(
        tempo_count, sizeof(*manifest->tempo_points));
    if (manifest->tempo_points == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for score tempo points");
        return -1;
    }
    for (index = 0U; index < manifest->event_count; ++index) {
        HWAScoreEvent *event = &manifest->events[index];
        if (event->kind == HWA_SCORE_TEMPO) {
            HWAScoreTempoPoint *point =
                &manifest->tempo_points[manifest->tempo_count];
            if (manifest->tempo_count != 0U &&
                event->start_beats <=
                    manifest->tempo_points[manifest->tempo_count - 1U].beat) {
                hwa_set_error(error, error_size,
                              "score row %zu repeats or reverses a tempo beat",
                              event->source_row);
                return -1;
            }
            point->beat = event->start_beats;
            point->bpm = event->tempo_bpm;
            point->event_index = index;
            if (manifest->tempo_count != 0U) {
                const HWAScoreTempoPoint *prior = point - 1;
                point->seconds = prior->seconds +
                    (point->beat - prior->beat) * 60.0 / prior->bpm;
            }
            manifest->tempo_count++;
        }
    }
    if (manifest->tempo_points[0].beat != 0.0) {
        hwa_set_error(error, error_size,
                      "the first score tempo row must start at beat 0");
        return -1;
    }
    manifest->duration_beats = duration_beats;
    if (hwa_score_manifest_beat_to_seconds(
            manifest, duration_beats, &manifest->duration_seconds) != 0) {
        hwa_set_error(error, error_size,
                      "cannot map score duration through the tempo map");
        return -1;
    }
    for (index = 0U; index < manifest->event_count; ++index) {
        HWAScoreEvent *event = &manifest->events[index];

        if (hwa_score_manifest_beat_to_seconds(
                manifest, event->start_beats, &event->start_seconds) != 0 ||
            hwa_score_manifest_beat_to_seconds(
                manifest, event->end_beats, &event->end_seconds) != 0) {
            hwa_set_error(error, error_size,
                          "cannot map score row %zu through the tempo map",
                          event->source_row);
            return -1;
        }
        while (tempo_index + 1U < manifest->tempo_count &&
               manifest->tempo_points[tempo_index + 1U].beat <=
                   event->start_beats) {
            tempo_index++;
            manifest->tempo_lookup_steps++;
        }
        if (event->kind != HWA_SCORE_TEMPO) {
            event->tempo_bpm = manifest->tempo_points[tempo_index].bpm;
            event->tempo_valid = 1;
        }
    }
    return 0;
}

int hwa_score_manifest_load(const char *path,
                            uint64_t max_bytes,
                            size_t max_events,
                            HWAScoreManifest *manifest,
                            char *error,
                            size_t error_size)
{
    unsigned char *data = NULL;
    size_t data_size = 0U;
    HWAScoreParseState state;
    HWAScoreManifest temporary;

    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (manifest == NULL || max_events == 0U) {
        hwa_set_error(error, error_size,
                      "invalid score manifest arguments");
        return -1;
    }
    memset(&temporary, 0, sizeof(temporary));
    if (hwa_score_read_file(path, max_bytes, &data, &data_size,
                            temporary.sha256, error, error_size) != 0) {
        return -1;
    }
    temporary.path = hwa_score_copy_text(path);
    if (temporary.path == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for the score path");
        free(data);
        return -1;
    }
    memset(&state, 0, sizeof(state));
    state.manifest = &temporary;
    state.max_events = max_events;
    if (hwa_score_csv_rows(data, data_size, hwa_score_parse_row, &state,
                           error, error_size) != 0 ||
        hwa_score_finish_manifest(&temporary, error, error_size) != 0) {
        free(data);
        hwa_score_manifest_free(&temporary);
        return -1;
    }
    free(data);
    *manifest = temporary;
    return 0;
}

void hwa_score_manifest_free(HWAScoreManifest *manifest)
{
    size_t index;

    if (manifest == NULL) {
        return;
    }
    for (index = 0U; index < manifest->event_count; ++index) {
        hwa_score_event_free(&manifest->events[index]);
    }
    free(manifest->events);
    free(manifest->tempo_points);
    free(manifest->path);
    memset(manifest, 0, sizeof(*manifest));
}

int hwa_score_manifest_beat_to_seconds(const HWAScoreManifest *manifest,
                                       double beat,
                                       double *seconds)
{
    size_t low = 0U;
    size_t high;

    if (manifest == NULL || seconds == NULL || !isfinite(beat) || beat < 0.0 ||
        manifest->tempo_count == 0U) {
        return -1;
    }
    high = manifest->tempo_count;
    while (low + 1U < high) {
        size_t middle = low + (high - low) / 2U;
        if (manifest->tempo_points[middle].beat <= beat) {
            low = middle;
        } else {
            high = middle;
        }
    }
    *seconds = manifest->tempo_points[low].seconds +
               (beat - manifest->tempo_points[low].beat) * 60.0 /
                   manifest->tempo_points[low].bpm;
    return isfinite(*seconds) ? 0 : -1;
}

int hwa_score_manifest_seconds_to_beat(const HWAScoreManifest *manifest,
                                       double seconds,
                                       double *beat)
{
    size_t low = 0U;
    size_t high;

    if (manifest == NULL || beat == NULL || !isfinite(seconds) || seconds < 0.0 ||
        manifest->tempo_count == 0U) {
        return -1;
    }
    high = manifest->tempo_count;
    while (low + 1U < high) {
        size_t middle = low + (high - low) / 2U;
        if (manifest->tempo_points[middle].seconds <= seconds) {
            low = middle;
        } else {
            high = middle;
        }
    }
    *beat = manifest->tempo_points[low].beat +
            (seconds - manifest->tempo_points[low].seconds) *
                manifest->tempo_points[low].bpm / 60.0;
    return isfinite(*beat) ? 0 : -1;
}

static HWAAlignmentStatus hwa_score_alignment_status(HWAScoreEventKind kind)
{
    switch (kind) {
    case HWA_SCORE_REST:
        return HWA_ALIGNMENT_REST;
    case HWA_SCORE_ORNAMENT:
        return HWA_ALIGNMENT_ORNAMENT;
    case HWA_SCORE_CADENZA:
        return HWA_ALIGNMENT_CADENZA;
    default:
        return HWA_ALIGNMENT_MATCHED;
    }
}

static uint32_t hwa_score_frame_flag(const HWAScoreEvent *event)
{
    uint32_t flags = 0U;

    if (event->kind == HWA_SCORE_REST) {
        flags |= HWA_ALIGN_FRAME_REST;
    } else if (event->kind == HWA_SCORE_ORNAMENT) {
        flags |= HWA_ALIGN_FRAME_ORNAMENT;
    } else if (event->kind == HWA_SCORE_CADENZA) {
        flags |= HWA_ALIGN_FRAME_CADENZA;
    }
    if (event->tie != HWA_SCORE_TIE_NONE) {
        flags |= HWA_ALIGN_FRAME_TIED;
    }
    return flags;
}

static void hwa_score_add_chroma(double chroma[HWA_CHROMA_BIN_COUNT],
                                 int midi_note,
                                 double effort)
{
    static const unsigned harmonic_offsets[6] = {0U, 0U, 7U, 0U, 4U, 7U};
    static const double harmonic_weights[6] = {1.0, 0.50, 0.33, 0.25, 0.20, 0.17};
    size_t harmonic;
    unsigned pitch_class = (unsigned)midi_note % HWA_CHROMA_BIN_COUNT;

    for (harmonic = 0U; harmonic < 6U; ++harmonic) {
        unsigned bin = (pitch_class + harmonic_offsets[harmonic]) %
                       HWA_CHROMA_BIN_COUNT;
        chroma[bin] += effort * harmonic_weights[harmonic];
    }
}

int hwa_score_manifest_build_track(const HWAScoreManifest *manifest,
                                   double step_seconds,
                                   uint64_t max_work_bytes,
                                   size_t max_points,
                                   HWAAlignFrame **owned_frames,
                                   HWAAlignEvent **owned_events,
                                   HWAAlignTrack *track,
                                   char *error,
                                   size_t error_size)
{
    HWAAlignFrame *frames = NULL;
    HWAAlignEvent *events = NULL;
    size_t frame_count;
    size_t event_count = 0U;
    size_t event_output = 0U;
    size_t index;
    uint64_t bytes;

    if (error != NULL && error_size != 0U) {
        error[0] = '\0';
    }
    if (manifest == NULL || owned_frames == NULL || owned_events == NULL ||
        track == NULL || !isfinite(step_seconds) || step_seconds <= 0.0 ||
        max_work_bytes == 0U || max_points == 0U ||
        !isfinite(manifest->duration_seconds) ||
        manifest->duration_seconds <= 0.0) {
        hwa_set_error(error, error_size,
                      "invalid score-track arguments");
        return -1;
    }
    if (manifest->duration_seconds / step_seconds >= (double)SIZE_MAX) {
        hwa_set_error(error, error_size, "score track is too long");
        return -1;
    }
    frame_count = (size_t)ceil(manifest->duration_seconds / step_seconds);
    if (frame_count == 0U) {
        frame_count = 1U;
    }
    if (frame_count > max_points) {
        hwa_set_error(error, error_size,
                      "score track exceeds the alignment-point limit");
        return -1;
    }
    for (index = 0U; index < manifest->event_count; ++index) {
        if (manifest->events[index].kind != HWA_SCORE_TEMPO) {
            event_count++;
        }
    }
    if (frame_count > SIZE_MAX / sizeof(*frames) ||
        event_count > SIZE_MAX / sizeof(*events)) {
        hwa_set_error(error, error_size, "score track allocation overflows");
        return -1;
    }
    bytes = (uint64_t)frame_count * (uint64_t)sizeof(*frames);
    if ((uint64_t)event_count >
        (UINT64_MAX - bytes) / (uint64_t)sizeof(*events)) {
        hwa_set_error(error, error_size, "score track work size overflows");
        return -1;
    }
    bytes += (uint64_t)event_count * (uint64_t)sizeof(*events);
    if (bytes > max_work_bytes) {
        hwa_set_error(error, error_size,
                      "score track exceeds the alignment work-byte limit");
        return -1;
    }
    frames = (HWAAlignFrame *)calloc(frame_count, sizeof(*frames));
    events = event_count == 0U ? NULL :
        (HWAAlignEvent *)calloc(event_count, sizeof(*events));
    if (frames == NULL || (event_count != 0U && events == NULL)) {
        free(events);
        free(frames);
        hwa_set_error(error, error_size,
                      "out of memory for the score alignment track");
        return -1;
    }
    for (index = 0U; index < frame_count; ++index) {
        HWAAlignFrame *frame = &frames[index];
        frame->time_seconds = ((double)index + 0.5) * step_seconds;
        if (frame->time_seconds > manifest->duration_seconds) {
            frame->time_seconds = manifest->duration_seconds;
        }
        (void)hwa_score_manifest_seconds_to_beat(
            manifest, frame->time_seconds, &frame->score_beat);
        frame->score_beat_valid = 1;
        frame->event_index = SIZE_MAX;
        frame->log_energy = -120.0;
        frame->score_flags = HWA_ALIGN_FRAME_REST;
    }
    for (index = 0U; index < manifest->event_count; ++index) {
        const HWAScoreEvent *event = &manifest->events[index];
        HWAAlignEvent *align_event;
        size_t first_frame;
        size_t end_frame;
        size_t frame_index;
        double effort;

        if (event->kind == HWA_SCORE_TEMPO) {
            continue;
        }
        align_event = &events[event_output];
        align_event->start_seconds = event->start_seconds;
        align_event->end_seconds = event->end_seconds;
        align_event->start_beat = event->start_beats;
        align_event->end_beat = event->end_beats;
        align_event->status = hwa_score_alignment_status(event->kind);
        align_event->event_id = event->event_id;
        align_event->kind = event->kind_text;
        align_event->voice = event->voice;
        align_event->midi_note = event->midi_note_text;
        align_event->velocity = event->velocity_text;
        align_event->tie = event->tie_text;
        align_event->dynamic = event->dynamic;
        align_event->mark = event->mark;
        align_event->score_position = event->score_position;
        align_event->tempo_bpm = event->tempo_bpm;
        align_event->tempo_valid = event->tempo_valid;
        first_frame = (size_t)floor(event->start_seconds / step_seconds);
        end_frame = (size_t)ceil(event->end_seconds / step_seconds);
        if (first_frame >= frame_count) {
            first_frame = frame_count - 1U;
        }
        if (end_frame > frame_count) {
            end_frame = frame_count;
        }
        if (end_frame <= first_frame) {
            end_frame = first_frame + 1U;
        }
        effort = event->velocity_valid ? (double)event->velocity / 127.0 : 0.75;
        if (effort < 1e-6) {
            effort = 1e-6;
        }
        for (frame_index = first_frame; frame_index < end_frame; ++frame_index) {
            HWAAlignFrame *frame = &frames[frame_index];
            double energy_db = 20.0 * log10(effort);
            uint32_t flags = hwa_score_frame_flag(event);

            if (frame->event_index == SIZE_MAX) {
                frame->event_index = event_output;
            }
            frame->score_flags |= flags;
            if (event->kind != HWA_SCORE_REST) {
                frame->score_flags &= ~(uint32_t)HWA_ALIGN_FRAME_REST;
                frame->activity = 1.0;
                frame->evidence_flags |= HWA_ALIGNMENT_EVIDENCE_ENVELOPE;
                if (energy_db > frame->log_energy) {
                    frame->log_energy = energy_db;
                }
            }
            if (event->midi_note_valid) {
                hwa_score_add_chroma(frame->chroma, event->midi_note, effort);
                frame->evidence_flags |= HWA_ALIGNMENT_EVIDENCE_CHROMA |
                                         HWA_ALIGNMENT_EVIDENCE_PITCH;
                if (frame->pitch_confidence == 0.0 ||
                    effort > frame->pitch_confidence) {
                    frame->pitch_class = (double)(event->midi_note % 12);
                    frame->pitch_confidence = effort;
                }
            }
        }
        if (event->tie != HWA_SCORE_TIE_CONTINUE &&
            event->tie != HWA_SCORE_TIE_STOP) {
            HWAAlignFrame *onset = &frames[first_frame];
            onset->spectral_onset = 1.0;
            onset->energy_onset = 1.0;
            onset->combined_onset = 1.0;
            onset->evidence_flags |= HWA_ALIGNMENT_EVIDENCE_SPECTRAL_ONSET |
                                     HWA_ALIGNMENT_EVIDENCE_ENERGY_ONSET;
        }
        event_output++;
    }
    for (index = 0U; index < frame_count; ++index) {
        HWAAlignFrame *frame = &frames[index];
        double norm = 0.0;
        size_t bin;

        /* A rest in one voice does not make a sounding voice a rest. */
        if (frame->activity > 0.0) {
            frame->score_flags &= ~(uint32_t)HWA_ALIGN_FRAME_REST;
        }

        for (bin = 0U; bin < HWA_CHROMA_BIN_COUNT; ++bin) {
            norm += frame->chroma[bin] * frame->chroma[bin];
        }
        if (norm > 0.0) {
            norm = sqrt(norm);
            for (bin = 0U; bin < HWA_CHROMA_BIN_COUNT; ++bin) {
                frame->chroma[bin] /= norm;
            }
        }
        if (frame->pitch_confidence > 0.0) {
            frame->pitch_confidence = 1.0;
        }
    }
    memset(track, 0, sizeof(*track));
    track->frames = frames;
    track->frame_count = frame_count;
    track->events = events;
    track->event_count = event_count;
    track->step_seconds = step_seconds;
    track->duration_seconds = manifest->duration_seconds;
    track->tuning_offset_cents = 0.0;
    track->tuning_confidence = 1.0;
    track->is_score = 1;
    *owned_frames = frames;
    *owned_events = events;
    return 0;
}

void hwa_score_manifest_release_track(HWAAlignFrame *frames,
                                      HWAAlignEvent *events,
                                      HWAAlignTrack *track)
{
    free(events);
    free(frames);
    if (track != NULL) {
        memset(track, 0, sizeof(*track));
    }
}

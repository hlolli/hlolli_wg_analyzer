#include "item_report.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_OUTPUT_CAPACITY 65536U

typedef struct TestJson {
    const unsigned char *cursor;
    const unsigned char *end;
} TestJson;

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

static void test_json_space(TestJson *json)
{
    while (json->cursor != json->end &&
           (*json->cursor == (unsigned char)' ' ||
            *json->cursor == (unsigned char)'\t' ||
            *json->cursor == (unsigned char)'\r' ||
            *json->cursor == (unsigned char)'\n')) {
        json->cursor++;
    }
}

static int test_json_hex(unsigned char value)
{
    return (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           (value >= (unsigned char)'a' && value <= (unsigned char)'f') ||
           (value >= (unsigned char)'A' && value <= (unsigned char)'F');
}

static int test_json_string(TestJson *json)
{
    if (json->cursor == json->end ||
        *json->cursor != (unsigned char)'"') {
        return 0;
    }
    json->cursor++;
    while (json->cursor != json->end) {
        unsigned char value = *json->cursor++;
        if (value == (unsigned char)'"') return 1;
        if (value < 0x20U) return 0;
        if (value == (unsigned char)'\\') {
            size_t index;
            if (json->cursor == json->end) return 0;
            value = *json->cursor++;
            if (value == (unsigned char)'"' ||
                value == (unsigned char)'\\' ||
                value == (unsigned char)'/' ||
                value == (unsigned char)'b' ||
                value == (unsigned char)'f' ||
                value == (unsigned char)'n' ||
                value == (unsigned char)'r' ||
                value == (unsigned char)'t') {
                continue;
            }
            if (value != (unsigned char)'u' ||
                (size_t)(json->end - json->cursor) < 4U) {
                return 0;
            }
            for (index = 0U; index < 4U; ++index) {
                if (!test_json_hex(json->cursor[index])) return 0;
            }
            json->cursor += 4U;
        }
    }
    return 0;
}

static int test_json_number(TestJson *json)
{
    const unsigned char *start = json->cursor;
    if (json->cursor != json->end &&
        *json->cursor == (unsigned char)'-') {
        json->cursor++;
    }
    if (json->cursor == json->end) return 0;
    if (*json->cursor == (unsigned char)'0') {
        json->cursor++;
    } else {
        if (*json->cursor < (unsigned char)'1' ||
            *json->cursor > (unsigned char)'9') {
            return 0;
        }
        do {
            json->cursor++;
        } while (json->cursor != json->end &&
                 *json->cursor >= (unsigned char)'0' &&
                 *json->cursor <= (unsigned char)'9');
    }
    if (json->cursor != json->end &&
        *json->cursor == (unsigned char)'.') {
        json->cursor++;
        if (json->cursor == json->end ||
            *json->cursor < (unsigned char)'0' ||
            *json->cursor > (unsigned char)'9') {
            return 0;
        }
        do {
            json->cursor++;
        } while (json->cursor != json->end &&
                 *json->cursor >= (unsigned char)'0' &&
                 *json->cursor <= (unsigned char)'9');
    }
    if (json->cursor != json->end &&
        (*json->cursor == (unsigned char)'e' ||
         *json->cursor == (unsigned char)'E')) {
        json->cursor++;
        if (json->cursor != json->end &&
            (*json->cursor == (unsigned char)'+' ||
             *json->cursor == (unsigned char)'-')) {
            json->cursor++;
        }
        if (json->cursor == json->end ||
            *json->cursor < (unsigned char)'0' ||
            *json->cursor > (unsigned char)'9') {
            return 0;
        }
        do {
            json->cursor++;
        } while (json->cursor != json->end &&
                 *json->cursor >= (unsigned char)'0' &&
                 *json->cursor <= (unsigned char)'9');
    }
    return json->cursor != start;
}

static int test_json_value(TestJson *json, unsigned depth);

static int test_json_array(TestJson *json, unsigned depth)
{
    if (json->cursor == json->end ||
        *json->cursor != (unsigned char)'[') {
        return 0;
    }
    json->cursor++;
    test_json_space(json);
    if (json->cursor != json->end &&
        *json->cursor == (unsigned char)']') {
        json->cursor++;
        return 1;
    }
    for (;;) {
        if (!test_json_value(json, depth + 1U)) return 0;
        test_json_space(json);
        if (json->cursor == json->end) return 0;
        if (*json->cursor == (unsigned char)']') {
            json->cursor++;
            return 1;
        }
        if (*json->cursor != (unsigned char)',') return 0;
        json->cursor++;
        test_json_space(json);
    }
}

static int test_json_object(TestJson *json, unsigned depth)
{
    if (json->cursor == json->end ||
        *json->cursor != (unsigned char)'{') {
        return 0;
    }
    json->cursor++;
    test_json_space(json);
    if (json->cursor != json->end &&
        *json->cursor == (unsigned char)'}') {
        json->cursor++;
        return 1;
    }
    for (;;) {
        if (!test_json_string(json)) return 0;
        test_json_space(json);
        if (json->cursor == json->end ||
            *json->cursor != (unsigned char)':') {
            return 0;
        }
        json->cursor++;
        if (!test_json_value(json, depth + 1U)) return 0;
        test_json_space(json);
        if (json->cursor == json->end) return 0;
        if (*json->cursor == (unsigned char)'}') {
            json->cursor++;
            return 1;
        }
        if (*json->cursor != (unsigned char)',') return 0;
        json->cursor++;
        test_json_space(json);
    }
}

static int test_json_literal(TestJson *json, const char *literal)
{
    size_t length = strlen(literal);
    if ((size_t)(json->end - json->cursor) < length ||
        memcmp(json->cursor, literal, length) != 0) {
        return 0;
    }
    json->cursor += length;
    return 1;
}

static int test_json_value(TestJson *json, unsigned depth)
{
    test_json_space(json);
    if (depth > 64U || json->cursor == json->end) return 0;
    if (*json->cursor == (unsigned char)'{') {
        return test_json_object(json, depth);
    }
    if (*json->cursor == (unsigned char)'[') {
        return test_json_array(json, depth);
    }
    if (*json->cursor == (unsigned char)'"') return test_json_string(json);
    if (*json->cursor == (unsigned char)'t') {
        return test_json_literal(json, "true");
    }
    if (*json->cursor == (unsigned char)'f') {
        return test_json_literal(json, "false");
    }
    if (*json->cursor == (unsigned char)'n') {
        return test_json_literal(json, "null");
    }
    return test_json_number(json);
}

static int test_json_document(const char *text, size_t size)
{
    TestJson json;
    json.cursor = (const unsigned char *)text;
    json.end = json.cursor + size;
    return test_json_value(&json, 0U) &&
           (test_json_space(&json), json.cursor == json.end);
}

typedef int (*TestItemWriter)(FILE *, const HWAItemSet *);

static int test_capture(TestItemWriter writer,
                        const HWAItemSet *items,
                        char output[TEST_OUTPUT_CAPACITY],
                        size_t *size)
{
    FILE *stream = tmpfile();
    long length;
    int ok = 1;
    *size = 0U;
    if (stream == NULL) return 0;
    if (writer(stream, items) != 0 || fflush(stream) != 0 ||
        fseek(stream, 0L, SEEK_END) != 0) {
        ok = 0;
    }
    length = ok ? ftell(stream) : -1L;
    if (length < 0L || (uint64_t)length >= TEST_OUTPUT_CAPACITY ||
        fseek(stream, 0L, SEEK_SET) != 0) {
        ok = 0;
    }
    if (ok && fread(output, 1U, (size_t)length, stream) !=
                  (size_t)length) {
        ok = 0;
    }
    if (fclose(stream) != 0) ok = 0;
    if (!ok) return 0;
    output[length] = '\0';
    *size = (size_t)length;
    return 1;
}

static void test_make_items(HWAItemSet *set,
                            HWAItemEvent *event,
                            HWAItem *item,
                            HWAItemMember *member,
                            HWAItemWarning *warning,
                            char alignment_path[5])
{
    memset(set, 0, sizeof(*set));
    memset(event, 0, sizeof(*event));
    memset(item, 0, sizeof(*item));
    memset(member, 0, sizeof(*member));
    memset(warning, 0, sizeof(*warning));
    hwa_segmentation_options_default(&set->options);
    set->alignment_path = alignment_path;
    set->audio_path = (char *)"audio.wav";
    set->labels_path = (char *)"labels.csv";
    set->amendment_path = (char *)"edits.hwa-items";
    set->source_score_path = (char *)"score.csv";
    (void)strcpy(set->alignment_sha256,
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    (void)strcpy(set->audio_sha256,
                 "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    (void)strcpy(set->labels_sha256,
                 "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
    (void)strcpy(set->amendment_sha256,
                 "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
    (void)strcpy(set->source_score_sha256,
                 "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
    set->audio_format.sample_rate_hz = 1000U;
    set->audio_format.frames = 1000U;
    set->audio_format.duration_seconds = 1.0;
    set->source_score_duration_seconds = 1.0;
    set->alignment_confidence = 0.875;
    set->boundary_evaluations = 9U;
    set->retained_work_bytes = 1234U;
    set->events = event;
    set->event_count = 1U;
    set->items = item;
    set->item_count = 1U;
    set->members = member;
    set->member_count = 1U;
    set->warnings = warning;
    set->warning_count = 1U;
    set->locked_item_count = 1U;
    set->low_confidence_item_count = 1U;

    event->id = 1U;
    event->event_id = (char *)"n1";
    event->kind = (char *)"note";
    event->voice = (char *)"v1";
    event->midi_note = (char *)"69";
    event->velocity = (char *)"96";
    event->tie = (char *)"none";
    event->dynamic = (char *)"ff";
    event->mark = (char *)"accent";
    event->score_position = (char *)"m1";
    event->labels.pitch = (char *)"A4";
    event->labels.gesture = (char *)"shake\"\n";
    event->labels.override_flags = HWA_LABEL_OVERRIDE_PITCH |
                                   HWA_LABEL_OVERRIDE_GESTURE;
    event->score_end_beat = 1.0;
    event->score_end_seconds = 0.2;
    event->audio_start_sample = 100U;
    event->audio_end_sample = 300U;
    event->audio_start_seconds = 0.1;
    event->audio_end_seconds = 0.3;
    event->tempo_bpm = 120.0;
    event->tempo_valid = 1;
    event->alignment_confidence = 0.8;
    event->alignment_evidence_flags = HWA_ITEM_EVIDENCE_ALIGNMENT;
    event->alignment_status = HWA_ALIGNMENT_MATCHED;

    item->id = 1U;
    item->key = (char *)"note-1";
    item->role = (char *)"note";
    item->kind = HWA_ITEM_NOTE;
    item->start_sample = 100U;
    item->end_sample = 300U;
    item->start_seconds = 0.1;
    item->end_seconds = 0.3;
    item->score_end_beat = 1.0;
    item->confidence = 0.7;
    item->evidence_flags = HWA_ITEM_EVIDENCE_ALIGNMENT |
                           HWA_ITEM_EVIDENCE_MANUAL;
    item->quality_flags = HWA_ITEM_QUALITY_LOW_CONFIDENCE;
    item->origin = HWA_ITEM_ORIGIN_MANUAL;
    item->locked = 1;

    member->item_id = 1U;
    member->event_id = 1U;
    member->role = HWA_ITEM_MEMBER_SOURCE;

    warning->id = 1U;
    warning->code = (char *)"low-confidence";
    warning->message = (char *)"check\ncontroller";
    warning->item_id = 1U;
    warning->event_id = 1U;
    warning->item_id_valid = 1;
    warning->event_id_valid = 1;
}

int main(void)
{
    char alignment_path[] = "a\xff\nx";
    char output[TEST_OUTPUT_CAPACITY];
    size_t size;
    HWAItemSet set;
    HWAItemEvent event;
    HWAItem item;
    HWAItemMember member;
    HWAItemWarning warning;

    test_make_items(&set, &event, &item, &member, &warning,
                    alignment_path);
    CHECK(test_capture(hwa_report_items_json, &set, output, &size),
          "cannot capture item JSON report");
    CHECK(test_json_document(output, size),
          "item report is not one valid JSON document");
    CHECK(strstr(output,
                 "{\"schema_version\":4,\"command\":\"segment\"") ==
              output,
          "item JSON report lost its schema identity");
    CHECK(strstr(output, "\"method_version\":\"stage3-1\"") != NULL,
          "item JSON report lost its method version");
    CHECK(strstr(output, "\"path\":\"a\\u00ff\\nx\"") != NULL &&
              strstr(output, "\"path_bytes_hex\":\"61ff0a78\"") != NULL,
          "item JSON report did not preserve invalid path bytes");
    CHECK(strstr(output, "\"boundary_evaluations\":9") != NULL &&
              strstr(output, "\"retained_work_bytes\":1234") != NULL,
          "item JSON report lost cap provenance");
    CHECK(strstr(output, "\"events\":[{\"id\":1") != NULL &&
              strstr(output, "\"items\":[{\"id\":1") != NULL &&
              strstr(output, "\"members\":[{\"item_id\":1") != NULL &&
              strstr(output, "\"warnings\":[{\"id\":1") != NULL,
          "item JSON report lost result rows");

    CHECK(test_capture(hwa_report_items_text, &set, output, &size),
          "cannot capture item text report");
    CHECK(strstr(output, "Alignment: a\\xff\\x0ax\nAudio: audio.wav") !=
              NULL,
          "text report did not escape unsafe path bytes");
    CHECK(strstr(output, "Boundary evaluations: 9\n") != NULL,
          "text report lost evaluation provenance");
    return failures == 0 ? 0 : 1;
}

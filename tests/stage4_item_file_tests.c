#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "item_file.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
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
                              "%s/hwa-stage4-items-%ld-%u",
                              root, test_process_id(), attempt);
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

static int test_write_bytes(const char *path, const void *bytes, size_t size)
{
    FILE *stream = fopen(path, "wb");
    int ok = stream != NULL && fwrite(bytes, 1U, size, stream) == size;
    if (stream != NULL && fclose(stream) != 0) ok = 0;
    return ok;
}

static char *test_read_bytes(const char *path, size_t *size)
{
    FILE *stream = fopen(path, "rb");
    long length;
    char *bytes;

    *size = 0U;
    if (stream == NULL || fseek(stream, 0L, SEEK_END) != 0) {
        if (stream != NULL) (void)fclose(stream);
        return NULL;
    }
    length = ftell(stream);
    if (length < 0 || fseek(stream, 0L, SEEK_SET) != 0 ||
        (uintmax_t)length > (uintmax_t)(SIZE_MAX - 1U)) {
        (void)fclose(stream);
        return NULL;
    }
    bytes = (char *)malloc((size_t)length + 1U);
    if (bytes == NULL) {
        (void)fclose(stream);
        return NULL;
    }
    if (fread(bytes, 1U, (size_t)length, stream) != (size_t)length ||
        fclose(stream) != 0) {
        free(bytes);
        return NULL;
    }
    bytes[length] = '\0';
    *size = (size_t)length;
    return bytes;
}

static void test_make_items(HWAItemSet *set,
                            HWAItemEvent events[2],
                            HWAItem items[3],
                            HWAItemMember members[3],
                            HWAItemWarning warnings[1],
                            char score_path[10])
{
    memset(set, 0, sizeof(*set));
    memset(events, 0, 2U * sizeof(*events));
    memset(items, 0, 3U * sizeof(*items));
    memset(members, 0, 3U * sizeof(*members));
    memset(warnings, 0, sizeof(*warnings));
    hwa_segmentation_options_default(&set->options);
    set->alignment_path = (char *)"map.hwa-align";
    set->audio_path = (char *)"played.wav";
    set->labels_path = (char *)"labels.csv";
    set->amendment_path = (char *)"prior.hwa-items";
    set->source_score_path = score_path;
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
    set->source_score_duration_seconds = 1.25;
    set->alignment_confidence = 0.75;
    set->boundary_evaluations = 123U;

    set->events = events;
    set->event_count = 2U;
    events[0].id = 1U;
    events[0].event_id = (char *)"event-a";
    events[0].kind = (char *)"note";
    events[0].voice = (char *)"solo";
    events[0].midi_note = (char *)"69";
    events[0].dynamic = (char *)"mf";
    events[0].score_position = (char *)"A";
    events[0].labels.pitch = (char *)"A4";
    events[0].labels.dynamic = (char *)"mf";
    events[0].labels.articulation = (char *)"detach";
    events[0].labels.physical_element = (char *)"element-a";
    events[0].labels.override_flags =
        HWA_LABEL_OVERRIDE_PITCH | HWA_LABEL_OVERRIDE_ARTICULATION |
        HWA_LABEL_OVERRIDE_PHYSICAL_ELEMENT;
    events[0].score_end_beat = 1.0;
    events[0].score_end_seconds = 0.5;
    events[0].audio_start_sample = 100U;
    events[0].audio_end_sample = 500U;
    events[0].audio_start_seconds = 0.1;
    events[0].audio_end_seconds = 0.5;
    events[0].tempo_bpm = 120.0;
    events[0].alignment_confidence = 0.8;
    events[0].alignment_evidence_flags = HWA_ALIGNMENT_EVIDENCE_PITCH;
    events[0].alignment_status = HWA_ALIGNMENT_MATCHED;
    events[0].tempo_valid = 1;
    events[1] = events[0];
    events[1].id = 2U;
    events[1].event_id = (char *)"event-b";
    events[1].midi_note = (char *)"71";
    events[1].labels.pitch = (char *)"B4";
    events[1].score_start_beat = 1.0;
    events[1].score_end_beat = 2.0;
    events[1].score_start_seconds = 0.5;
    events[1].score_end_seconds = 1.0;
    events[1].audio_start_sample = 500U;
    events[1].audio_end_sample = 900U;
    events[1].audio_start_seconds = 0.5;
    events[1].audio_end_seconds = 0.9;

    set->items = items;
    set->item_count = 3U;
    items[0].id = 1U;
    items[0].key = (char *)"note-a";
    items[0].role = (char *)"note";
    items[0].kind = HWA_ITEM_NOTE;
    items[0].start_sample = 100U;
    items[0].end_sample = 500U;
    items[0].start_seconds = 0.1;
    items[0].end_seconds = 0.5;
    items[0].score_end_beat = 1.0;
    items[0].confidence = 0.8;
    items[0].evidence_flags = HWA_ITEM_EVIDENCE_ALIGNMENT;
    items[0].origin = HWA_ITEM_ORIGIN_AUTO;
    items[1] = items[0];
    items[1].id = 2U;
    items[1].key = (char *)"body-a";
    items[1].role = (char *)"body";
    items[1].kind = HWA_ITEM_BODY;
    items[1].parent_id = 1U;
    items[1].parent_valid = 1;
    items[1].start_sample = 150U;
    items[1].end_sample = 450U;
    items[1].start_seconds = 0.15;
    items[1].end_seconds = 0.45;
    items[1].locked = 1;
    items[1].origin = HWA_ITEM_ORIGIN_MANUAL;
    items[2] = items[0];
    items[2].id = 3U;
    items[2].key = (char *)"note-b";
    items[2].start_sample = 500U;
    items[2].end_sample = 900U;
    items[2].start_seconds = 0.5;
    items[2].end_seconds = 0.9;
    items[2].score_start_beat = 1.0;
    items[2].score_end_beat = 2.0;
    items[2].quality_flags = HWA_ITEM_QUALITY_LOW_CONFIDENCE;
    items[2].excluded = 1;
    items[2].exclusion_reason = (char *)"bad take";
    set->locked_item_count = 1U;
    set->excluded_item_count = 1U;
    set->low_confidence_item_count = 1U;

    set->members = members;
    set->member_count = 3U;
    members[0].item_id = 1U;
    members[0].event_id = 1U;
    members[0].role = HWA_ITEM_MEMBER_SOURCE;
    members[1].item_id = 2U;
    members[1].event_id = 1U;
    members[1].role = HWA_ITEM_MEMBER_SOURCE;
    members[2].item_id = 3U;
    members[2].event_id = 2U;
    members[2].role = HWA_ITEM_MEMBER_SOURCE;

    set->warnings = warnings;
    set->warning_count = 1U;
    warnings[0].id = 1U;
    warnings[0].code = (char *)"fixture";
    warnings[0].message = (char *)"fixture warning";
    warnings[0].item_id = 3U;
    warnings[0].item_id_valid = 1;
}

static int test_write_items(const char *path, const HWAItemSet *items)
{
    FILE *stream = fopen(path, "wb");
    char error[HWA_ERROR_SIZE];
    int result;

    if (stream == NULL) return 0;
    result = hwa_item_file_write(stream, items, error, sizeof(error));
    if (fclose(stream) != 0) result = -1;
    return result == 0;
}

static void test_full_reader(const char *directory)
{
    char path[PATH_MAX];
    char roundtrip[PATH_MAX];
    char bad_key[PATH_MAX];
    char bad_member[PATH_MAX];
    char stale_meta[PATH_MAX];
    char score_path[10] = {'s', 'c', 'o', 'r', (char)0xff,
                           '.', 'c', 's', 'v', '\0'};
    HWAItemSet source;
    HWAItemEvent events[2];
    HWAItem items[3];
    HWAItemMember members[3];
    HWAItemWarning warnings[1];
    HWAItemFileLimits limits;
    HWAItemFileData loaded;
    char error[HWA_ERROR_SIZE];
    char *bytes;
    char *field;
    size_t size;

    CHECK(test_path(path, directory, "source.hwa-items") &&
              test_path(roundtrip, directory, "roundtrip.hwa-items") &&
              test_path(bad_key, directory, "bad-key.hwa-items") &&
              test_path(bad_member, directory, "bad-member.hwa-items") &&
              test_path(stale_meta, directory, "stale-meta.hwa-items"),
          "cannot make item test paths");
    test_make_items(&source, events, items, members, warnings, score_path);
    CHECK(test_write_items(path, &source), "cannot write full item fixture");

    hwa_item_file_limits_default(&limits);
    memset(&loaded, 0xa5, sizeof(loaded));
    CHECK(hwa_item_file_read_full(path, &limits, &loaded,
                                  error, sizeof(error)) == 0,
          "full item reader failed: %s", error);
    CHECK(loaded.path != NULL && strcmp(loaded.path, path) == 0 &&
              strlen(loaded.sha256) == 64U &&
              loaded.retained_work_bytes != 0U,
          "full item reader lost its own provenance");
    CHECK(loaded.items.event_count == 2U &&
              loaded.items.item_count == 3U &&
              loaded.items.member_count == 3U &&
              loaded.items.warning_count == 1U,
          "full item reader lost row counts");
    CHECK(strcmp(loaded.items.events[0].event_id, "event-a") == 0 &&
              strcmp(loaded.items.events[0].labels.articulation,
                     "detach") == 0 &&
              loaded.items.events[1].audio_start_sample == 500U,
          "full item reader lost event data");
    CHECK(strcmp(loaded.items.items[1].key, "body-a") == 0 &&
              loaded.items.items[1].parent_valid &&
              loaded.items.items[1].parent_id == 1U &&
              loaded.items.items[2].excluded &&
              strcmp(loaded.items.items[2].exclusion_reason,
                     "bad take") == 0,
          "full item reader lost item data");
    CHECK(loaded.items.members[2].event_id == 2U &&
              loaded.items.warnings[0].item_id_valid &&
              loaded.items.locked_item_count == 1U &&
              loaded.items.excluded_item_count == 1U &&
              loaded.items.low_confidence_item_count == 1U,
          "full item reader lost relations or derived counts");
    CHECK(loaded.items.source_score_path != NULL &&
              (unsigned char)loaded.items.source_score_path[4] == 0xffU &&
              strcmp(loaded.items.labels_sha256,
                     source.labels_sha256) == 0 &&
              strcmp(loaded.items.amendment_sha256,
                     source.amendment_sha256) == 0,
          "full item reader lost stored provenance");
    CHECK(test_write_items(roundtrip, &loaded.items),
          "cannot rewrite the full item result");
    hwa_item_file_data_free(&loaded);
    hwa_item_file_data_free(&loaded);

    memset(&loaded, 0, sizeof(loaded));
    CHECK(hwa_item_file_read_full(roundtrip, &limits, &loaded,
                                  error, sizeof(error)) == 0,
          "cannot read full item roundtrip: %s", error);
    hwa_item_file_data_free(&loaded);

    bytes = test_read_bytes(path, &size);
    CHECK(bytes != NULL, "cannot read full item fixture bytes");
    field = bytes != NULL ? strstr(bytes, "ITEM,3,note-b,") : NULL;
    CHECK(field != NULL, "cannot find item key to duplicate");
    if (field != NULL) {
        field += strlen("ITEM,3,note-");
        *field = 'a';
        CHECK(test_write_bytes(bad_key, bytes, size),
              "cannot write duplicate-key fixture");
        memset(&loaded, 0xa5, sizeof(loaded));
        CHECK(hwa_item_file_read_full(bad_key, &limits, &loaded,
                                      error, sizeof(error)) != 0 &&
                  strstr(error, "unique") != NULL &&
                  loaded.path == NULL && loaded.items.items == NULL,
              "full reader accepted a duplicate key or kept a failed result: %s",
              error);
        hwa_item_file_data_free(&loaded);
        *field = 'b';
    }
    field = bytes != NULL ? strstr(bytes, "MEMBER,2,1,0,source") : NULL;
    CHECK(field != NULL, "cannot find member row to duplicate");
    if (field != NULL) {
        field[strlen("MEMBER,")] = '1';
        CHECK(test_write_bytes(bad_member, bytes, size),
              "cannot write duplicate-member fixture");
        memset(&loaded, 0, sizeof(loaded));
        CHECK(hwa_item_file_read_full(bad_member, &limits, &loaded,
                                      error, sizeof(error)) != 0 &&
                  strstr(error, "canonical") != NULL,
              "full reader accepted duplicate MEMBER rows: %s", error);
        hwa_item_file_data_free(&loaded);
        field[strlen("MEMBER,")] = '2';
    }
    field = bytes != NULL
                ? strstr(bytes, "META,excluded_item_count,1,items") : NULL;
    CHECK(field != NULL, "cannot find derived excluded count");
    if (field != NULL) {
        field[strlen("META,excluded_item_count,")] = '0';
        CHECK(test_write_bytes(stale_meta, bytes, size),
              "cannot write stale derived-count fixture");
        memset(&loaded, 0, sizeof(loaded));
        CHECK(hwa_item_file_read_full(stale_meta, &limits, &loaded,
                                      error, sizeof(error)) == 0 &&
                  loaded.items.excluded_item_count == 1U,
              "full reader trusted stale derived META: %s", error);
        hwa_item_file_data_free(&loaded);
    }
    free(bytes);

    limits.max_items = 2U;
    memset(&loaded, 0, sizeof(loaded));
    CHECK(hwa_item_file_read_full(path, &limits, &loaded,
                                  error, sizeof(error)) != 0,
          "full item reader ignored the current item cap: %s", error);
    hwa_item_file_data_free(&loaded);

    (void)test_remove_file(stale_meta);
    (void)test_remove_file(bad_member);
    (void)test_remove_file(bad_key);
    (void)test_remove_file(roundtrip);
    (void)test_remove_file(path);
}

int main(void)
{
    char directory[PATH_MAX];

    if (!test_workspace(directory)) {
        (void)fputs("cannot make Stage 4 item test workspace\n", stderr);
        return 1;
    }
    test_full_reader(directory);
    (void)test_remove_directory(directory);
    if (failures != 0) {
        (void)fprintf(stderr, "%d Stage 4 item-file test(s) failed\n",
                      failures);
        return 1;
    }
    (void)puts("Stage 4 full item-file tests passed");
    return 0;
}

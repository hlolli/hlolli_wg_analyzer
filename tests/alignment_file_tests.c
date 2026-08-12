#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "alignment_file.h"

#include <inttypes.h>
#include <limits.h>
#include <math.h>
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

typedef struct AlignmentFixture {
    HWAAlignment alignment;
    HWAAlignmentAnchor anchors[3];
    HWAAlignmentMatch matches[2];
    HWAUnmatchedSpan spans[2];
    HWAAlignmentWarning warnings[2];
} AlignmentFixture;

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

static long process_id(void)
{
#if defined(_WIN32)
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}

static int make_directory(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

static int remove_directory(const char *path)
{
#if defined(_WIN32)
    return _rmdir(path);
#else
    return rmdir(path);
#endif
}

static int remove_file(const char *path)
{
#if defined(_WIN32)
    return _unlink(path);
#else
    return unlink(path);
#endif
}

static int make_workspace(char directory[PATH_MAX])
{
    unsigned attempt;
#if defined(_WIN32)
    const char *root = getenv("TEMP");
    if (root == NULL || root[0] == '\0') {
        root = ".";
    }
#else
    const char *root = "/tmp";
#endif

    for (attempt = 0U; attempt < 100U; ++attempt) {
        int length = snprintf(directory, PATH_MAX,
                              "%s/hwa-align-file-test-%ld-%u",
                              root, process_id(), attempt);
        if (length < 0 || length >= PATH_MAX) {
            return 0;
        }
        if (make_directory(directory) == 0) {
            return 1;
        }
    }
    return 0;
}

static int make_path(char path[PATH_MAX],
                     const char *directory,
                     const char *name)
{
    int length = snprintf(path, PATH_MAX, "%s/%s", directory, name);
    return length >= 0 && length < PATH_MAX;
}

static int write_bytes(const char *path, const void *data, size_t size)
{
    FILE *stream = fopen(path, "wb");
    int ok = stream != NULL && fwrite(data, 1U, size, stream) == size;

    if (stream != NULL && fclose(stream) != 0) {
        ok = 0;
    }
    return ok;
}

static char *read_bytes(const char *path, size_t *size)
{
    FILE *stream = fopen(path, "rb");
    long length;
    char *text;

    *size = 0U;
    if (stream == NULL || fseek(stream, 0L, SEEK_END) != 0) {
        if (stream != NULL) {
            (void)fclose(stream);
        }
        return NULL;
    }
    length = ftell(stream);
    if (length < 0 || fseek(stream, 0L, SEEK_SET) != 0 ||
        (uintmax_t)length > (uintmax_t)(SIZE_MAX - 1U)) {
        (void)fclose(stream);
        return NULL;
    }
    text = (char *)malloc((size_t)length + 1U);
    if (text == NULL) {
        (void)fclose(stream);
        return NULL;
    }
    if (fread(text, 1U, (size_t)length, stream) != (size_t)length ||
        fclose(stream) != 0) {
        free(text);
        return NULL;
    }
    text[length] = '\0';
    *size = (size_t)length;
    return text;
}

static int write_alignment(const char *path,
                           const HWAAlignment *alignment,
                           char *error,
                           size_t error_size)
{
    FILE *stream = fopen(path, "wb");
    int result;

    if (stream == NULL) {
        (void)snprintf(error, error_size, "cannot open test output");
        return -1;
    }
    result = hwa_alignment_file_write(stream, alignment, error, error_size);
    if (fclose(stream) != 0 && result == 0) {
        (void)snprintf(error, error_size, "cannot close test output");
        result = -1;
    }
    return result;
}

static char *replace_once(const char *source,
                          const char *needle,
                          const char *replacement)
{
    const char *found = strstr(source, needle);
    size_t prefix;
    size_t source_size;
    size_t needle_size;
    size_t replacement_size;
    size_t result_size;
    char *result;

    if (found == NULL) {
        return NULL;
    }
    prefix = (size_t)(found - source);
    source_size = strlen(source);
    needle_size = strlen(needle);
    replacement_size = strlen(replacement);
    if (source_size - needle_size > SIZE_MAX - replacement_size) {
        return NULL;
    }
    result_size = source_size - needle_size + replacement_size;
    result = (char *)malloc(result_size + 1U);
    if (result == NULL) {
        return NULL;
    }
    memcpy(result, source, prefix);
    memcpy(result + prefix, replacement, replacement_size);
    memcpy(result + prefix + replacement_size,
           found + needle_size,
           source_size - prefix - needle_size + 1U);
    return result;
}

static char *append_text(const char *source, const char *suffix)
{
    size_t source_size = strlen(source);
    size_t suffix_size = strlen(suffix);
    char *result;

    if (source_size > SIZE_MAX - suffix_size - 1U) {
        return NULL;
    }
    result = (char *)malloc(source_size + suffix_size + 1U);
    if (result == NULL) {
        return NULL;
    }
    memcpy(result, source, source_size);
    memcpy(result + source_size, suffix, suffix_size + 1U);
    return result;
}

static void set_hash(char destination[HWA_SHA256_HEX_SIZE], char digit)
{
    size_t index;
    for (index = 0U; index < HWA_SHA256_HEX_SIZE - 1U; ++index) {
        destination[index] = digit;
    }
    destination[HWA_SHA256_HEX_SIZE - 1U] = '\0';
}

static void fixture_init(AlignmentFixture *fixture)
{
    HWAAlignment *alignment;

    memset(fixture, 0, sizeof(*fixture));
    alignment = &fixture->alignment;
    hwa_alignment_options_default(&alignment->options);
    alignment->mode = HWA_ALIGNMENT_AUDIO_TO_AUDIO;
    alignment->reference_path = "reference, take \"A\".wav";
    alignment->target_path = "target\r\ntake.wav";
    set_hash(alignment->reference_sha256, 'a');
    set_hash(alignment->target_sha256, 'b');
    alignment->reference_duration_seconds = 4.0;
    alignment->target_duration_seconds = 5.0;
    alignment->tuning_offset_cents = -17.5;
    alignment->tuning_confidence = 0.5;
    alignment->total_cost = 4.0;
    alignment->normalized_cost = 0.25;
    alignment->matched_coverage = 0.5;
    alignment->global_confidence = 0.75;
    alignment->dtw_cells = 100U;
    alignment->path_points = 20U;

    fixture->anchors[0].id = 3U;
    fixture->anchors[0].reference_seconds = 4.0;
    fixture->anchors[0].target_seconds = 5.0;
    fixture->anchors[0].confidence = 1.0;
    fixture->anchors[0].origin = HWA_ALIGNMENT_ORIGIN_AUTO;
    fixture->anchors[0].evidence_flags = HWA_ALIGNMENT_EVIDENCE_CHROMA;
    fixture->anchors[1].id = 1U;
    fixture->anchors[1].reference_seconds = 0.0;
    fixture->anchors[1].target_seconds = 0.0;
    fixture->anchors[1].confidence = 1.0;
    fixture->anchors[1].origin = HWA_ALIGNMENT_ORIGIN_AUTO;
    fixture->anchors[1].evidence_flags = HWA_ALIGNMENT_EVIDENCE_CHROMA;
    fixture->anchors[2].id = 2U;
    fixture->anchors[2].reference_seconds = 2.0;
    fixture->anchors[2].target_seconds = 2.5;
    fixture->anchors[2].confidence = 1.0;
    fixture->anchors[2].origin = HWA_ALIGNMENT_ORIGIN_MANUAL;
    fixture->anchors[2].locked = 1;
    fixture->anchors[2].evidence_flags = HWA_ALIGNMENT_EVIDENCE_ENERGY_ONSET;
    alignment->anchors = fixture->anchors;
    alignment->anchor_count = 3U;

    fixture->matches[0].id = 2U;
    fixture->matches[0].reference_start_seconds = 2.0;
    fixture->matches[0].reference_end_seconds = 4.0;
    fixture->matches[0].target_start_seconds = 2.5;
    fixture->matches[0].target_end_seconds = 5.0;
    fixture->matches[0].confidence = 0.5;
    fixture->matches[0].status = HWA_ALIGNMENT_LOW_CONFIDENCE;
    fixture->matches[0].evidence_flags = HWA_ALIGNMENT_EVIDENCE_CHROMA;
    fixture->matches[1].id = 1U;
    fixture->matches[1].reference_start_seconds = 0.0;
    fixture->matches[1].reference_end_seconds = 2.0;
    fixture->matches[1].target_start_seconds = 0.0;
    fixture->matches[1].target_end_seconds = 2.5;
    fixture->matches[1].confidence = 1.0;
    fixture->matches[1].status = HWA_ALIGNMENT_MATCHED;
    fixture->matches[1].evidence_flags =
        HWA_ALIGNMENT_EVIDENCE_CHROMA |
        HWA_ALIGNMENT_EVIDENCE_SPECTRAL_ONSET;
    alignment->matches = fixture->matches;
    alignment->match_count = 2U;

    fixture->spans[0].id = 2U;
    fixture->spans[0].side = HWA_ALIGNMENT_TARGET;
    fixture->spans[0].reason = HWA_UNMATCHED_SUFFIX;
    fixture->spans[0].start_seconds = 4.5;
    fixture->spans[0].end_seconds = 5.0;
    fixture->spans[0].confidence = 0.5;
    fixture->spans[1].id = 1U;
    fixture->spans[1].side = HWA_ALIGNMENT_REFERENCE;
    fixture->spans[1].reason = HWA_UNMATCHED_PREFIX;
    fixture->spans[1].start_seconds = 0.0;
    fixture->spans[1].end_seconds = 0.25;
    fixture->spans[1].confidence = 1.0;
    alignment->unmatched_spans = fixture->spans;
    alignment->unmatched_span_count = 2U;

    fixture->warnings[0].id = 2U;
    fixture->warnings[0].code = "near,\"limit\"";
    fixture->warnings[0].message = "line one\r\nline \"two\"";
    fixture->warnings[1].id = 1U;
    fixture->warnings[1].code = "low-confidence";
    fixture->warnings[1].message = "review this match";
    alignment->warnings = fixture->warnings;
    alignment->warning_count = 2U;
}

static void fixture_init_score(AlignmentFixture *fixture)
{
    size_t index;

    fixture_init(fixture);
    fixture->alignment.mode = HWA_ALIGNMENT_SCORE_TO_AUDIO;
    fixture->alignment.score_path = "score.csv";
    set_hash(fixture->alignment.score_sha256, 'c');
    for (index = 0U; index < 3U; ++index) {
        fixture->anchors[index].score_beat_valid = 1;
    }
    fixture->anchors[0].score_beat = 8.0;
    fixture->anchors[1].score_beat = 0.0;
    fixture->anchors[2].score_beat = 4.0;
    for (index = 0U; index < 2U; ++index) {
        fixture->matches[index].score_span_valid = 1;
        fixture->matches[index].tempo_valid = 1;
        fixture->matches[index].tempo_bpm = 120.0;
        fixture->matches[index].event_id = "event";
        fixture->matches[index].kind = "note";
    }
    fixture->matches[0].score_start_beat = 4.0;
    fixture->matches[0].score_end_beat = 8.0;
    fixture->matches[1].score_start_beat = 0.0;
    fixture->matches[1].score_end_beat = 4.0;
    fixture->spans[1].score_span_valid = 1;
    fixture->spans[1].start_beat = 0.0;
    fixture->spans[1].end_beat = 0.5;
}

static int only_crlf_rows(const char *text)
{
    size_t index;
    for (index = 0U; text[index] != '\0'; ++index) {
        if (text[index] == '\n' && (index == 0U || text[index - 1U] != '\r')) {
            return 0;
        }
        if (text[index] == '\r' && text[index + 1U] != '\n') {
            return 0;
        }
    }
    return 1;
}

static void check_build_facts(const char *text)
{
    static const char *const keys[] = {
        "META,build_compiler_family,",
        "META,build_compiler_version,",
        "META,build_c_standard,",
        "META,build_target_os,",
        "META,build_pointer_bits,",
        "META,build_endianness,",
        "META,build_mode,"
    };
    const char *cursor = text;
    char pointer_line[96];
    size_t index;

    CHECK(hwa_build_compiler_family()[0] != '\0' &&
              hwa_build_compiler_version()[0] != '\0' &&
              hwa_build_c_standard()[0] != '\0' &&
              hwa_build_target_os()[0] != '\0' &&
              hwa_build_endianness()[0] != '\0' &&
              hwa_build_pointer_bits() == sizeof(void *) * (size_t)CHAR_BIT,
          "build-fact helpers returned invalid data");
#if defined(NDEBUG)
    CHECK(strcmp(hwa_build_mode(), "release") == 0,
          "NDEBUG build was not marked release");
#else
    CHECK(strcmp(hwa_build_mode(), "debug") == 0,
          "non-NDEBUG build was not marked debug");
#endif
    for (index = 0U; index < sizeof(keys) / sizeof(keys[0]); ++index) {
        const char *found = strstr(cursor, keys[index]);
        CHECK(found != NULL, "canonical output lacks build fact %s", keys[index]);
        if (found != NULL) {
            cursor = found + strlen(keys[index]);
        }
    }
    (void)snprintf(pointer_line, sizeof(pointer_line),
                   "META,build_pointer_bits,%u,bits\r\n",
                   hwa_build_pointer_bits());
    CHECK(strstr(text, pointer_line) != NULL &&
              strstr(text, "build_timestamp") == NULL,
          "canonical build facts changed shape or added a timestamp");
}

static void expect_bad_text(const char *path,
                            const char *text,
                            const char *wanted)
{
    HWAAlignmentLockedSet locked;
    char error[HWA_ERROR_SIZE];

    memset(&locked, 0, sizeof(locked));
    CHECK(write_bytes(path, text, strlen(text)),
          "could not write bad alignment fixture");
    CHECK(hwa_alignment_file_read_locked(
              path, UINT64_C(1000000), 100U, &locked,
              error, sizeof(error)) != 0 && strstr(error, wanted) != NULL,
          "bad alignment did not fail with '%s': %s", wanted, error);
    hwa_alignment_locked_set_free(&locked);
}

static void test_round_trip(const char *directory,
                            const char *first_path,
                            char **canonical_out,
                            size_t *canonical_size_out)
{
    AlignmentFixture fixture;
    HWAAlignmentAnchor anchors_before[3];
    HWAAlignmentMatch matches_before[2];
    HWAAlignmentLockedSet locked;
    char second_path[PATH_MAX];
    char error[HWA_ERROR_SIZE];
    char *first;
    char *second;
    size_t first_size;
    size_t second_size;

    fixture_init(&fixture);
    memcpy(anchors_before, fixture.anchors, sizeof(anchors_before));
    memcpy(matches_before, fixture.matches, sizeof(matches_before));
    CHECK(make_path(second_path, directory, "second.hwa-align"),
          "second alignment path is too long");
    CHECK(write_alignment(first_path, &fixture.alignment,
                          error, sizeof(error)) == 0,
          "canonical alignment write failed: %s", error);
    CHECK(memcmp(anchors_before, fixture.anchors, sizeof(anchors_before)) == 0 &&
              memcmp(matches_before, fixture.matches, sizeof(matches_before)) == 0,
          "canonical sorting changed the alignment result arrays");
    first = read_bytes(first_path, &first_size);
    CHECK(first != NULL, "could not read canonical alignment");
    if (first == NULL) {
        return;
    }
    CHECK(strncmp(first, "HWA_ALIGNMENT,1\r\n", 17U) == 0 &&
              only_crlf_rows(first),
          "alignment file is not canonical CRLF CSV");
    check_build_facts(first);
    CHECK(strstr(first, "ANCHOR,1,0,0,,1,auto,0,1\r\n") != NULL &&
              strstr(first, "ANCHOR,2,2,2.5,,1,manual,1,4\r\n") != NULL &&
              strstr(first, "ANCHOR,3,4,5,,1,auto,0,1\r\n") != NULL,
          "canonical output did not sort or preserve anchors");
    CHECK(strstr(first, "\"near,\"\"limit\"\"\"") != NULL &&
              strstr(first, "\"line one\r\nline \"\"two\"\"\"") != NULL,
          "canonical CSV did not quote warnings");

    CHECK(write_alignment(second_path, &fixture.alignment,
                          error, sizeof(error)) == 0,
          "second canonical write failed: %s", error);
    second = read_bytes(second_path, &second_size);
    CHECK(second != NULL && second_size == first_size &&
              memcmp(second, first, first_size) == 0,
          "the same alignment did not produce identical bytes");
    free(second);

    memset(&locked, 0, sizeof(locked));
    CHECK(hwa_alignment_file_read_locked(
              first_path, (uint64_t)first_size, 1U, &locked,
              error, sizeof(error)) == 0,
          "canonical alignment read failed: %s", error);
    CHECK(locked.mode == HWA_ALIGNMENT_AUDIO_TO_AUDIO &&
              locked.anchor_count == 1U &&
              locked.anchors != NULL && locked.anchors[0].id == 2U &&
              locked.anchors[0].locked != 0 &&
              locked.anchors[0].origin == HWA_ALIGNMENT_ORIGIN_MANUAL &&
              locked.anchors[0].reference_seconds == 2.0 &&
              locked.anchors[0].target_seconds == 2.5 &&
              locked.reference_duration_seconds == 4.0 &&
              locked.target_duration_seconds == 5.0,
          "locked-anchor round trip changed data");
    CHECK(hwa_alignment_locked_set_matches(
              &locked, HWA_ALIGNMENT_AUDIO_TO_AUDIO,
              fixture.alignment.reference_sha256,
              fixture.alignment.target_sha256, NULL,
              error, sizeof(error)) == 0,
          "matching alignment identities failed: %s", error);
    {
        char bad_hash[HWA_SHA256_HEX_SIZE];
        set_hash(bad_hash, 'c');
        CHECK(hwa_alignment_locked_set_matches(
                  &locked, HWA_ALIGNMENT_AUDIO_TO_AUDIO,
                  bad_hash, fixture.alignment.target_sha256, NULL,
                  error, sizeof(error)) != 0 &&
                  strstr(error, "hashes") != NULL,
              "reference hash mismatch passed: %s", error);
        CHECK(hwa_alignment_locked_set_matches(
                  &locked, HWA_ALIGNMENT_SCORE_TO_AUDIO,
                  NULL, fixture.alignment.target_sha256, bad_hash,
                  error, sizeof(error)) != 0 &&
                  strstr(error, "mode") != NULL,
              "alignment mode mismatch passed: %s", error);
    }
    hwa_alignment_locked_set_free(&locked);
    memset(&locked, 0, sizeof(locked));
    CHECK(first_size != 0U && hwa_alignment_file_read_locked(
              first_path, (uint64_t)first_size - 1U, 1U, &locked,
              error, sizeof(error)) != 0 &&
              strstr(error, "byte limit") != NULL,
          "one byte below the alignment-file cap passed: %s", error);
    hwa_alignment_locked_set_free(&locked);
    (void)remove_file(second_path);
    *canonical_out = first;
    *canonical_size_out = first_size;
}

static void test_bad_records(const char *path, const char *canonical)
{
    char *changed;

#define REPLACE_BAD(needle, replacement, wanted)                              \
    do {                                                                      \
        changed = replace_once(canonical, (needle), (replacement));            \
        CHECK(changed != NULL, "could not form bad alignment fixture");        \
        if (changed != NULL) {                                                  \
            expect_bad_text(path, changed, (wanted));                           \
            free(changed);                                                      \
        }                                                                       \
    } while (0)

    REPLACE_BAD("HWA_ALIGNMENT,1", "HWA_ALIGNMENT,2", "header");
    REPLACE_BAD("META,tool_version", "INPUT,tool_version", "META");
    REPLACE_BAD("INPUT,reference", "ANCHOR,reference", "INPUT");
    REPLACE_BAD("META,matched_coverage,0.5,ratio",
                "META,matched_coverage,1.5,ratio", "META");
    REPLACE_BAD("META,dtw_cells,100,cells",
                "META,dtw_cells,1.5,cells", "META");
    REPLACE_BAD("ANCHOR,2,2,2.5,,1,manual,1,4",
                "ANCHOR,2,2,2.5,,1.5,manual,1,4", "anchor");
    REPLACE_BAD("ANCHOR,2,2,2.5,,1,manual,1,4",
                "ANCHOR,2,2,6,,1,manual,1,4", "anchor");
    REPLACE_BAD("ANCHOR,2,2,2.5,,1,manual,1,4",
                "ANCHOR,2,0,2.5,,1,manual,1,4", "anchor");
    REPLACE_BAD("MATCH,2,2,4,2.5,5,,,0.5",
                "MATCH,2,2,4,2.5,6,,,0.5", "MATCH");
    REPLACE_BAD("MATCH,2,2,4,2.5,5,,,0.5",
                "MATCH,2,2,4,2.5,5,,,1.5", "MATCH");
    REPLACE_BAD("UNMATCHED,2,target,4.5,5,,,suffix,0.5",
                "UNMATCHED,2,target,4.5,6,,,suffix,0.5", "UNMATCHED");
    REPLACE_BAD("UNMATCHED,2,target,4.5,5,,,suffix,0.5",
                "UNMATCHED,2,target,4.5,5,,,suffix,1.5", "UNMATCHED");
    changed = append_text(canonical,
                          "ANCHOR,99,3,4,,1,manual,1,1\r\n");
    CHECK(changed != NULL, "could not form record-order fixture");
    if (changed != NULL) {
        expect_bad_text(path, changed, "canonical record order");
        free(changed);
    }
    changed = replace_once(canonical,
                           "ANCHOR,3,4,5,,1,auto,0,1",
                           "ANCHOR,3,4,5,,1,manual,1,1");
    CHECK(changed != NULL, "could not form locked-limit fixture");
    if (changed != NULL) {
        HWAAlignmentLockedSet locked;
        char error[HWA_ERROR_SIZE];
        memset(&locked, 0, sizeof(locked));
        CHECK(write_bytes(path, changed, strlen(changed)),
              "could not write locked-limit fixture");
        CHECK(hwa_alignment_file_read_locked(
                  path, UINT64_C(1000000), 1U, &locked,
                  error, sizeof(error)) != 0 &&
                  strstr(error, "manual-anchor limit") != NULL,
              "locked-anchor limit passed: %s", error);
        hwa_alignment_locked_set_free(&locked);
        free(changed);
    }
    REPLACE_BAD("\"line one\r\nline \"\"two\"\"\"",
                "\"line one\r\nline two", "unclosed quote");
    changed = replace_once(canonical, "HWA_ALIGNMENT,1\r\n",
                           "HWA_ALIGNMENT,1\r");
    CHECK(changed != NULL, "could not form bare-CR fixture");
    if (changed != NULL) {
        expect_bad_text(path, changed, "bare carriage return");
        free(changed);
    }
#undef REPLACE_BAD
}

static void test_score_metadata_round_trip(const char *directory)
{
    AlignmentFixture fixture;
    HWAAlignmentLockedSet locked;
    char path[PATH_MAX];
    char error[HWA_ERROR_SIZE];
    char *text;
    size_t size;
    size_t index;

    fixture_init(&fixture);
    fixture.alignment.mode = HWA_ALIGNMENT_SCORE_TO_AUDIO;
    fixture.alignment.score_path = "score, \"clean\".csv";
    set_hash(fixture.alignment.score_sha256, 'c');
    for (index = 0U; index < 3U; ++index) {
        fixture.anchors[index].score_beat_valid = 1;
    }
    fixture.anchors[0].score_beat = 8.0;
    fixture.anchors[1].score_beat = 0.0;
    fixture.anchors[2].score_beat = 4.0;
    for (index = 0U; index < 2U; ++index) {
        fixture.matches[index].score_span_valid = 1;
        fixture.matches[index].tempo_valid = 1;
        fixture.matches[index].tempo_bpm = 120.0;
        fixture.matches[index].kind = "note";
        fixture.matches[index].voice = "solo";
        fixture.matches[index].midi_note = "69";
        fixture.matches[index].velocity = "88";
        fixture.matches[index].tie = "none";
        fixture.matches[index].dynamic = "mf";
    }
    fixture.matches[0].score_start_beat = 4.0;
    fixture.matches[0].score_end_beat = 8.0;
    fixture.matches[0].event_id = "event-two";
    fixture.matches[0].mark = "plain";
    fixture.matches[0].score_position = "m2";
    fixture.matches[1].score_start_beat = 0.0;
    fixture.matches[1].score_end_beat = 4.0;
    fixture.matches[1].event_id = "event,\"one\"\r\nnext";
    fixture.matches[1].mark = "dolce, \"warm\"";
    fixture.matches[1].score_position = "m1\r\nb1";
    fixture.spans[1].score_span_valid = 1;
    fixture.spans[1].start_beat = 0.0;
    fixture.spans[1].end_beat = 0.5;
    CHECK(make_path(path, directory, "score.hwa-align"),
          "score alignment path is too long");
    CHECK(write_alignment(path, &fixture.alignment,
                          error, sizeof(error)) == 0,
          "score alignment write failed: %s", error);
    text = read_bytes(path, &size);
    CHECK(text != NULL &&
              strstr(text, "\"event,\"\"one\"\"\r\nnext\"") != NULL &&
              strstr(text, "\"dolce, \"\"warm\"\"\"") != NULL &&
              strstr(text, "\"m1\r\nb1\"") != NULL,
          "score match metadata was not quoted canonically");
    memset(&locked, 0, sizeof(locked));
    CHECK(text != NULL && hwa_alignment_file_read_locked(
              path, (uint64_t)size, 1U, &locked,
              error, sizeof(error)) == 0 &&
              locked.mode == HWA_ALIGNMENT_SCORE_TO_AUDIO &&
              locked.anchor_count == 1U &&
              locked.anchors[0].score_beat_valid != 0 &&
              locked.anchors[0].score_beat == 4.0,
          "score alignment read failed: %s", error);
    CHECK(hwa_alignment_locked_set_matches(
              &locked, HWA_ALIGNMENT_SCORE_TO_AUDIO, NULL,
              fixture.alignment.target_sha256,
              fixture.alignment.score_sha256,
              error, sizeof(error)) == 0,
          "score alignment identity match failed: %s", error);
    hwa_alignment_locked_set_free(&locked);
    free(text);
    (void)remove_file(path);
}

static void test_writer_checks(const char *path)
{
    AlignmentFixture fixture;
    char error[HWA_ERROR_SIZE];
    uint64_t exact_pointer_bytes =
        UINT64_C(9) * (uint64_t)sizeof(void *);

    fixture_init(&fixture);
    fixture.alignment.options.max_alignment_work_bytes = exact_pointer_bytes;
    CHECK(write_alignment(path, &fixture.alignment,
                          error, sizeof(error)) == 0,
          "exact canonical pointer-work cap failed: %s", error);
    fixture.alignment.options.max_alignment_work_bytes = exact_pointer_bytes - 1U;
    CHECK(write_alignment(path, &fixture.alignment,
                          error, sizeof(error)) != 0 &&
              strstr(error, "work-byte limit") != NULL,
          "one byte below canonical pointer-work cap passed: %s", error);
    fixture_init(&fixture);
    fixture.matches[0].target_end_seconds = 6.0;
    CHECK(write_alignment(path, &fixture.alignment,
                          error, sizeof(error)) != 0,
          "writer accepted a match past the target duration");
    fixture_init(&fixture);
    fixture.spans[0].end_seconds = 6.0;
    CHECK(write_alignment(path, &fixture.alignment,
                          error, sizeof(error)) != 0,
          "writer accepted an unmatched span past the target duration");
    fixture_init(&fixture);
    fixture.anchors[0].score_beat_valid = 1;
    fixture.anchors[0].score_beat = 1.0;
    CHECK(write_alignment(path, &fixture.alignment,
                          error, sizeof(error)) != 0,
          "writer made an audio alignment the reader cannot reopen");

    fixture_init(&fixture);
    fixture.matches[0].score_span_valid = 1;
    fixture.matches[0].score_start_beat = 0.0;
    fixture.matches[0].score_end_beat = 1.0;
    CHECK(write_alignment(path, &fixture.alignment,
                          error, sizeof(error)) != 0,
          "writer accepted score beats in an audio match");
    fixture_init(&fixture);
    fixture.matches[0].tempo_valid = 1;
    fixture.matches[0].tempo_bpm = 120.0;
    CHECK(write_alignment(path, &fixture.alignment,
                          error, sizeof(error)) != 0,
          "writer accepted score tempo in an audio match");
    fixture_init(&fixture);
    fixture.spans[0].score_span_valid = 1;
    fixture.spans[0].start_beat = 0.0;
    fixture.spans[0].end_beat = 1.0;
    CHECK(write_alignment(path, &fixture.alignment,
                          error, sizeof(error)) != 0,
          "writer accepted score beats in an audio unmatched span");

    fixture_init_score(&fixture);
    CHECK(write_alignment(path, &fixture.alignment,
                          error, sizeof(error)) == 0,
          "valid score writer fixture failed: %s", error);
    fixture.matches[0].score_span_valid = 0;
    CHECK(write_alignment(path, &fixture.alignment,
                          error, sizeof(error)) != 0,
          "writer accepted a score match without score beats");
    fixture_init_score(&fixture);
    fixture.matches[0].tempo_valid = 0;
    CHECK(write_alignment(path, &fixture.alignment,
                          error, sizeof(error)) != 0,
          "writer accepted a score match without tempo");
    fixture_init_score(&fixture);
    fixture.matches[0].event_id = NULL;
    CHECK(write_alignment(path, &fixture.alignment,
                          error, sizeof(error)) != 0,
          "writer accepted a score match without an event ID");
    fixture_init_score(&fixture);
    fixture.matches[0].kind = "";
    CHECK(write_alignment(path, &fixture.alignment,
                          error, sizeof(error)) != 0,
          "writer accepted a score match without a kind");
    fixture_init_score(&fixture);
    fixture.spans[1].score_span_valid = 0;
    CHECK(write_alignment(path, &fixture.alignment,
                          error, sizeof(error)) != 0,
          "writer accepted a score reference span without score beats");
    fixture_init_score(&fixture);
    fixture.spans[0].score_span_valid = 1;
    fixture.spans[0].start_beat = 0.0;
    fixture.spans[0].end_beat = 1.0;
    CHECK(write_alignment(path, &fixture.alignment,
                          error, sizeof(error)) != 0,
          "writer accepted score beats on a score target span");
}

#if !defined(_WIN32)
static void test_named_file_types(const char *directory,
                                  const char *canonical_path,
                                  size_t canonical_size)
{
    HWAAlignmentLockedSet locked;
    char link_path[PATH_MAX];
    char fifo_path[PATH_MAX];
    char error[HWA_ERROR_SIZE];

    CHECK(make_path(link_path, directory, "prior-link.hwa-align") &&
              make_path(fifo_path, directory, "prior.fifo"),
          "alignment special-file paths are too long");
    CHECK(symlink(canonical_path, link_path) == 0,
          "could not make alignment symlink fixture");
    memset(&locked, 0, sizeof(locked));
    CHECK(hwa_alignment_file_read_locked(
              link_path, (uint64_t)canonical_size, 1U, &locked,
              error, sizeof(error)) == 0 && locked.anchor_count == 1U,
          "symlink to a regular prior alignment failed: %s", error);
    hwa_alignment_locked_set_free(&locked);
    CHECK(mkfifo(fifo_path, 0600) == 0,
          "could not make prior-alignment FIFO fixture");
    memset(&locked, 0, sizeof(locked));
    CHECK(hwa_alignment_file_read_locked(
              fifo_path, UINT64_C(1000000), 1U, &locked,
              error, sizeof(error)) != 0 &&
              strstr(error, "regular file") != NULL,
          "prior-alignment FIFO was not rejected before open: %s", error);
    hwa_alignment_locked_set_free(&locked);
    (void)remove_file(fifo_path);
    (void)remove_file(link_path);
}
#endif

int main(void)
{
    char directory[PATH_MAX];
    char canonical_path[PATH_MAX];
    char bad_path[PATH_MAX];
    char writer_path[PATH_MAX];
    char *canonical = NULL;
    size_t canonical_size = 0U;

    CHECK(make_workspace(directory),
          "could not make alignment-file test workspace");
    CHECK(make_path(canonical_path, directory, "first.hwa-align") &&
              make_path(bad_path, directory, "bad.hwa-align") &&
              make_path(writer_path, directory, "writer.hwa-align"),
          "alignment-file test path is too long");
    if (failures == 0) {
        test_round_trip(directory, canonical_path,
                        &canonical, &canonical_size);
        if (canonical != NULL) {
            test_bad_records(bad_path, canonical);
#if !defined(_WIN32)
            test_named_file_types(directory, canonical_path, canonical_size);
#endif
        }
        test_score_metadata_round_trip(directory);
        test_writer_checks(writer_path);
    }
    free(canonical);
    (void)remove_file(writer_path);
    (void)remove_file(bad_path);
    (void)remove_file(canonical_path);
    (void)remove_directory(directory);
    if (failures != 0) {
        (void)fprintf(stderr, "%d alignment-file test(s) failed\n", failures);
        return 1;
    }
    return 0;
}

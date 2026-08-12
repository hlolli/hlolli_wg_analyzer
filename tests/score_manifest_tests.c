#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "score_manifest.h"

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

static const char valid_manifest[] =
    "event_id,kind,start_beats,duration_beats,midi_note,velocity,voice,tie,dynamic,mark,score_position,tempo_bpm\n"
    "tempo-1,tempo,0,0,,,,,,,m1,120\n"
    "n1,note,0,1,60,90,one,start,mf,\"dolce, warm\",m1b1,\n"
    "n2,note,0,1,64,80,two,none,p,\"say \"\"yes\"\"\",m1b1,\n"
    "n3,note,1,1,60,88,one,stop,mf,,m1b2,\n"
    "tempo-2,tempo,2,0,,,,,,,m2,60\n"
    "r1,rest,2,1,,,one,none,,,m2b1,\n"
    "o1,ornament,3,0.5,67,75,one,none,f,trill,m2b2,\n"
    "c1,cadenza,3.5,1,,,one,none,pp,free,m2b3,";

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

static int make_workspace(char directory[PATH_MAX], char path[PATH_MAX])
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
                              "%s/hwa-score-test-%ld-%u",
                              root, process_id(), attempt);
        if (length < 0 || length >= PATH_MAX) {
            return 0;
        }
        if (make_directory(directory) == 0) {
            length = snprintf(path, PATH_MAX, "%s/score.csv", directory);
            return length >= 0 && length < PATH_MAX;
        }
    }
    return 0;
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

static int write_text(const char *path, const char *text)
{
    return write_bytes(path, text, strlen(text));
}

static void test_tempo_lookup_is_linear(const char *path)
{
    const size_t tempo_count = 64U;
    HWAScoreManifest manifest;
    FILE *stream = fopen(path, "wb");
    char error[HWA_ERROR_SIZE];
    size_t index;
    int write_ok = stream != NULL;

    if (write_ok) {
        write_ok = fputs(
            "event_id,kind,start_beats,duration_beats,midi_note,velocity,"
            "voice,tie,dynamic,mark,score_position,tempo_bpm\n",
            stream) != EOF;
    }
    for (index = 0U; write_ok && index < tempo_count; ++index) {
        write_ok = fprintf(stream,
                           "tempo-%zu,tempo,%zu,0,,,,,,,m%zu,120\n"
                           "note-%zu,note,%zu,0.5,60,80,v,none,mf,,m%zu,\n",
                           index, index, index, index, index, index) >= 0;
    }
    if (stream != NULL && fclose(stream) != 0) {
        write_ok = 0;
    }
    CHECK(write_ok, "could not write many-tempo fixture");
    if (!write_ok) {
        return;
    }
    memset(&manifest, 0, sizeof(manifest));
    CHECK(hwa_score_manifest_load(path, UINT64_C(1000000),
                                  tempo_count * 2U, &manifest,
                                  error, sizeof(error)) == 0,
          "many-tempo manifest failed: %s", error);
    CHECK(manifest.tempo_count == tempo_count &&
              manifest.tempo_lookup_steps == tempo_count - 1U,
          "tempo lookup did not use one monotone scan: %llu steps",
          (unsigned long long)manifest.tempo_lookup_steps);
    hwa_score_manifest_free(&manifest);
}

static char *read_text(const char *path)
{
    FILE *stream = fopen(path, "rb");
    long length;
    char *text;

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
    return text;
}

static int near(double left, double right, double tolerance)
{
    return fabs(left - right) <= tolerance;
}

static void test_valid_manifest(const char *path)
{
    HWAScoreManifest manifest;
    HWAAlignFrame *frames = NULL;
    HWAAlignEvent *events = NULL;
    HWAAlignTrack track;
    char error[HWA_ERROR_SIZE];
    double mapped;
    size_t rest_frame;
    size_t ornament_frame;
    size_t cadenza_frame;
    char *after;

    memset(&manifest, 0, sizeof(manifest));
    CHECK(write_text(path, valid_manifest),
          "could not write valid score fixture");
    CHECK(hwa_score_manifest_load(path, 100000U, 100U, &manifest,
                                  error, sizeof(error)) == 0,
          "valid score failed: %s", error);
    after = read_text(path);
    CHECK(after != NULL && strcmp(after, valid_manifest) == 0,
          "score loading changed its source file");
    free(after);
    CHECK(manifest.event_count == 8U && manifest.tempo_count == 2U,
          "valid score counts are wrong");
    CHECK(near(manifest.duration_beats, 4.5, 1e-12) &&
              near(manifest.duration_seconds, 3.5, 1e-12),
          "score tempo map gave the wrong duration");
    CHECK(strlen(manifest.sha256) == 64U,
          "score hash is not a SHA-256 hex string");
    CHECK(strcmp(manifest.events[1].mark, "dolce, warm") == 0 &&
              strcmp(manifest.events[2].mark, "say \"yes\"") == 0,
          "quoted score metadata was not preserved");
    CHECK(manifest.events[1].tie == HWA_SCORE_TIE_START &&
              manifest.events[3].tie == HWA_SCORE_TIE_STOP &&
              strcmp(manifest.events[1].dynamic, "mf") == 0 &&
              strcmp(manifest.events[2].voice, "two") == 0 &&
              strcmp(manifest.events[6].score_position, "m2b2") == 0,
          "score gesture metadata was not preserved");
    CHECK(hwa_score_manifest_beat_to_seconds(&manifest, 3.0, &mapped) == 0 &&
              near(mapped, 2.0, 1e-12),
          "beat-to-seconds mapping is wrong");
    CHECK(hwa_score_manifest_seconds_to_beat(&manifest, 2.5, &mapped) == 0 &&
              near(mapped, 3.5, 1e-12),
          "seconds-to-beat mapping is wrong");
    CHECK(hwa_score_manifest_build_track(
              &manifest, 0.05, UINT64_C(1000000), 1000U,
              &frames, &events, &track, error, sizeof(error)) == 0,
          "could not build score alignment track: %s", error);
    if (frames != NULL) {
        double norm = 0.0;
        size_t bin;

        CHECK(track.frame_count == 70U && track.event_count == 6U &&
                  track.is_score != 0,
              "score alignment track counts are wrong");
        for (bin = 0U; bin < HWA_CHROMA_BIN_COUNT; ++bin) {
            norm += frames[0].chroma[bin] * frames[0].chroma[bin];
        }
        CHECK(near(norm, 1.0, 1e-12) &&
                  frames[0].chroma[0] > 0.0 &&
                  frames[0].chroma[4] > 0.0,
              "chord chroma is missing or not normalized");
        CHECK(frames[10].combined_onset == 0.0,
              "a tied continuation made a second onset");
        rest_frame = (size_t)floor(1.25 / 0.05);
        ornament_frame = (size_t)floor(2.25 / 0.05);
        cadenza_frame = (size_t)floor(3.0 / 0.05);
        CHECK((frames[rest_frame].score_flags & HWA_ALIGN_FRAME_REST) != 0U &&
                  frames[rest_frame].activity == 0.0,
              "rest did not survive score-track building");
        CHECK((frames[ornament_frame].score_flags &
               HWA_ALIGN_FRAME_ORNAMENT) != 0U,
              "ornament flag did not survive score-track building");
        CHECK((frames[cadenza_frame].score_flags &
               HWA_ALIGN_FRAME_CADENZA) != 0U,
              "cadenza flag did not survive score-track building");
        CHECK(strcmp(events[0].event_id, "n1") == 0 &&
                  strcmp(events[0].velocity, "90") == 0 &&
                  strcmp(events[0].tie, "start") == 0 &&
                  events[0].tempo_valid != 0 &&
                  near(events[0].tempo_bpm, 120.0, 1e-12),
              "alignment events lost score fields");
    }
    hwa_score_manifest_release_track(frames, events, &track);
    hwa_score_manifest_free(&manifest);
}

static void test_limits(const char *path)
{
    HWAScoreManifest manifest;
    HWAAlignFrame *frames = NULL;
    HWAAlignEvent *events = NULL;
    HWAAlignTrack track;
    char error[HWA_ERROR_SIZE];
    size_t exact_bytes;

    memset(&manifest, 0, sizeof(manifest));
    CHECK(write_text(path, valid_manifest),
          "could not write score limit fixture");
    CHECK(hwa_score_manifest_load(path,
                                  (uint64_t)strlen(valid_manifest) - 1U,
                                  100U, &manifest,
                                  error, sizeof(error)) != 0 &&
              strstr(error, "byte limit") != NULL,
          "one byte below the score limit passed: %s", error);
    CHECK(hwa_score_manifest_load(path, 100000U, 7U, &manifest,
                                  error, sizeof(error)) != 0 &&
              strstr(error, "event limit") != NULL,
          "one event below the score limit passed: %s", error);
    CHECK(hwa_score_manifest_load(path, 100000U, 8U, &manifest,
                                  error, sizeof(error)) == 0,
          "exact score event limit failed: %s", error);
    exact_bytes = 70U * sizeof(HWAAlignFrame) +
                  6U * sizeof(HWAAlignEvent);
    CHECK(hwa_score_manifest_build_track(
              &manifest, 0.05, (uint64_t)exact_bytes, 70U,
              &frames, &events, &track, error, sizeof(error)) == 0,
          "exact score-track limits failed: %s", error);
    hwa_score_manifest_release_track(frames, events, &track);
    frames = NULL;
    events = NULL;
    CHECK(hwa_score_manifest_build_track(
              &manifest, 0.05, (uint64_t)exact_bytes - 1U, 70U,
              &frames, &events, &track, error, sizeof(error)) != 0 &&
              strstr(error, "work-byte limit") != NULL,
          "one byte below the score-track cap passed: %s", error);
    CHECK(hwa_score_manifest_build_track(
              &manifest, 0.05, (uint64_t)exact_bytes, 69U,
              &frames, &events, &track, error, sizeof(error)) != 0 &&
              strstr(error, "alignment-point limit") != NULL,
          "one point below the score-track cap passed: %s", error);
    hwa_score_manifest_free(&manifest);
}

static void expect_bad(const char *path,
                       const char *text,
                       const char *wanted)
{
    HWAScoreManifest manifest;
    char error[HWA_ERROR_SIZE];

    memset(&manifest, 0, sizeof(manifest));
    CHECK(write_text(path, text), "could not write bad score fixture");
    CHECK(hwa_score_manifest_load(path, 100000U, 100U, &manifest,
                                  error, sizeof(error)) != 0 &&
              strstr(error, wanted) != NULL,
          "bad score did not fail with '%s': %s", wanted, error);
    hwa_score_manifest_free(&manifest);
}

static void test_bad_manifests(const char *path)
{
    static const char header[] =
        "event_id,kind,start_beats,duration_beats,midi_note,velocity,voice,tie,dynamic,mark,score_position,tempo_bpm\n";
    char text[2048];
    unsigned char nul_data[512];
    size_t header_size = strlen(header);
    HWAScoreManifest manifest;
    char error[HWA_ERROR_SIZE];

    expect_bad(path, "wrong,header\n", "header");
    (void)snprintf(text, sizeof(text),
                   "%st,tempo,0,0,,,,,,,m1\n", header);
    expect_bad(path, text, "12 fields");
    (void)snprintf(text, sizeof(text),
                   "%st,tempo,0,0,,,,,,,m1,120\n"
                   "t,note,0,1,60,90,v,none,mf,,m1,\n",
                   header);
    expect_bad(path, text, "repeats event_id");
    (void)snprintf(text, sizeof(text),
                   "%sn,note,0,1,60,90,v,none,mf,,m1,\n", header);
    expect_bad(path, text, "tempo row");
    (void)snprintf(text, sizeof(text),
                   "%st,tempo,1,0,,,,,,,m1,120\n"
                   "n,note,1,1,60,90,v,none,mf,,m1,\n",
                   header);
    expect_bad(path, text, "beat 0");
    (void)snprintf(text, sizeof(text),
                   "%st,tempo,0,0,,,,,,,m1,120\n"
                   "n,note,1,-1,60,90,v,none,mf,,m1,\n",
                   header);
    expect_bad(path, text, "duration_beats");
    (void)snprintf(text, sizeof(text),
                   "%st,tempo,0,0,,,,,,,m1,120\n"
                   "n,note,1,1,128,90,v,none,mf,,m1,\n",
                   header);
    expect_bad(path, text, "midi_note");
    (void)snprintf(text, sizeof(text),
                   "%st,tempo,0,0,,,,,,,m1,120\n"
                   "r,rest,1,1,,,v,start,,,m1,\n",
                   header);
    expect_bad(path, text, "invalid tie");
    (void)snprintf(text, sizeof(text),
                   "%st,tempo,0,0,,,,,,,m1,0\n"
                   "n,note,0,1,60,90,v,none,mf,,m1,\n",
                   header);
    expect_bad(path, text, "tempo_bpm");
    (void)snprintf(text, sizeof(text),
                   "%st,tempo,0,0,,,,,,,m1,120\n"
                   "u,tempo,0,0,,,,,,,m1,100\n"
                   "n,note,0,1,60,90,v,none,mf,,m1,\n",
                   header);
    expect_bad(path, text, "tempo beat");
    (void)snprintf(text, sizeof(text),
                   "%st,tempo,0,0,,,,,,,m1,120\n"
                   "n,note,nan,1,60,90,v,none,mf,,m1,\n",
                   header);
    expect_bad(path, text, "start_beats");
    (void)snprintf(text, sizeof(text),
                   "%st,tempo,0,0,,,,,,,m1,120\n"
                   "n,note,2,1,60,90,v,none,mf,,m1,\n"
                   "m,note,1,1,62,90,v,none,mf,,m1,\n",
                   header);
    expect_bad(path, text, "moves backward");
    (void)snprintf(text, sizeof(text),
                   "%st,tempo,0,0,,,,,,,m1,120\n"
                   "n,note,0,1,60,90,v,none,mf,\"open,m1,\n",
                   header);
    expect_bad(path, text, "unclosed quote");
    (void)snprintf(text, sizeof(text),
                   "%st,tempo,0,0,,,,,,,m1,120\r"
                   "n,note,0,1,60,90,v,none,mf,,m1,\n",
                   header);
    expect_bad(path, text, "bare carriage return");

    CHECK(header_size + 2U < sizeof(nul_data),
          "NUL score fixture buffer is too small");
    memcpy(nul_data, header, header_size);
    nul_data[header_size] = 0U;
    nul_data[header_size + 1U] = (unsigned char)'\n';
    CHECK(write_bytes(path, nul_data, header_size + 2U),
          "could not write NUL score fixture");
    memset(&manifest, 0, sizeof(manifest));
    CHECK(hwa_score_manifest_load(path, 100000U, 100U, &manifest,
                                  error, sizeof(error)) != 0 &&
              strstr(error, "NUL byte") != NULL,
          "NUL score did not fail clearly: %s", error);
    hwa_score_manifest_free(&manifest);
}

static void test_csv_line_forms(const char *path)
{
    static const char crlf_manifest[] =
        "event_id,kind,start_beats,duration_beats,midi_note,velocity,voice,tie,dynamic,mark,score_position,tempo_bpm\r\n"
        "t,tempo,0,0,,,,,,,m1,120\r\n"
        "n,note,0,1,60,90,v,none,mf,\"first\r\nsecond\",m1,";
    HWAScoreManifest manifest;
    char error[HWA_ERROR_SIZE];

    memset(&manifest, 0, sizeof(manifest));
    CHECK(write_text(path, crlf_manifest),
          "could not write CRLF score fixture");
    CHECK(hwa_score_manifest_load(path, 100000U, 100U, &manifest,
                                  error, sizeof(error)) == 0,
          "valid CRLF/quoted-newline score failed: %s", error);
    CHECK(manifest.event_count == 2U &&
              strcmp(manifest.events[1].mark, "first\r\nsecond") == 0,
          "quoted CRLF field was not preserved");
    hwa_score_manifest_free(&manifest);
}

static void expect_good_ties(const char *path,
                             const char *text,
                             size_t event_count)
{
    HWAScoreManifest manifest;
    char error[HWA_ERROR_SIZE];

    memset(&manifest, 0, sizeof(manifest));
    CHECK(write_text(path, text), "could not write valid tie fixture");
    CHECK(hwa_score_manifest_load(path, 100000U, 100U, &manifest,
                                  error, sizeof(error)) == 0 &&
              manifest.event_count == event_count,
          "valid tie fixture failed: %s", error);
    hwa_score_manifest_free(&manifest);
}

static void test_tie_chains(const char *path)
{
    static const char header[] =
        "event_id,kind,start_beats,duration_beats,midi_note,velocity,voice,tie,dynamic,mark,score_position,tempo_bpm\n";
    char text[4096];

    (void)snprintf(text, sizeof(text),
                   "%st,tempo,0,0,,,,,,,m1,120\n"
                   "n,note,0,1,60,90,v,continue,mf,,m1,\n", header);
    expect_bad(path, text, "tie 'continue'");
    (void)snprintf(text, sizeof(text),
                   "%st,tempo,0,0,,,,,,,m1,120\n"
                   "n,note,0,1,60,90,v,stop,mf,,m1,\n", header);
    expect_bad(path, text, "tie 'stop'");
    (void)snprintf(text, sizeof(text),
                   "%st,tempo,0,0,,,,,,,m1,120\n"
                   "a,note,0,1,60,90,v,start,mf,,m1,\n"
                   "b,note,1.5,1,60,90,v,stop,mf,,m2,\n", header);
    expect_bad(path, text, "contiguous");
    (void)snprintf(text, sizeof(text),
                   "%st,tempo,0,0,,,,,,,m1,120\n"
                   "a,note,0,1,60,90,v,start,mf,,m1,\n"
                   "b,note,1,1,60,90,v,start,mf,,m2,\n", header);
    expect_bad(path, text, "before row");
    (void)snprintf(text, sizeof(text),
                   "%st,tempo,0,0,,,,,,,m1,120\n"
                   "a,note,0,1,60,90,v,start,mf,,m1,\n"
                   "b,note,1,1,60,90,v,none,mf,,m2,\n", header);
    expect_bad(path, text, "interrupts");
    (void)snprintf(text, sizeof(text),
                   "%st,tempo,0,0,,,,,,,m1,120\n"
                   "a,note,0,1,60,90,v,start,mf,,m1,\n", header);
    expect_bad(path, text, "no stop");

    (void)snprintf(text, sizeof(text),
                   "%st,tempo,0,0,,,,,,,m1,120\n"
                   "a,note,0,1,60,90,v,start,mf,,m1,\n"
                   "b,note,1,1,60,90,v,continue,mf,,m2,\n"
                   "c,note,2,1,60,90,v,stop,mf,,m3,\n", header);
    expect_good_ties(path, text, 4U);
    (void)snprintf(text, sizeof(text),
                   "%st,tempo,0,0,,,,,,,m1,120\n"
                   "a0,note,0,1,60,90,a,start,mf,,m1,\n"
                   "b0,note,0,1,60,90,b,start,mf,,m1,\n"
                   "c0,note,0,1,64,90,a,start,mf,,m1,\n"
                   "a1,note,1,1,60,90,a,continue,mf,,m2,\n"
                   "b1,note,1,1,60,90,b,stop,mf,,m2,\n"
                   "c1,note,1,1,64,90,a,stop,mf,,m2,\n"
                   "a2,note,2,1,60,90,a,stop,mf,,m3,\n", header);
    expect_good_ties(path, text, 8U);
}

static void test_overlapping_rest_voice(const char *path)
{
    static const char manifest_text[] =
        "event_id,kind,start_beats,duration_beats,midi_note,velocity,voice,tie,dynamic,mark,score_position,tempo_bpm\n"
        "t,tempo,0,0,,,,,,,m1,60\n"
        "n,note,0,2,69,100,one,none,mf,,m1,\n"
        "r,rest,1,1,,,two,none,,,m2,\n";
    HWAScoreManifest manifest;
    HWAAlignFrame *frames = NULL;
    HWAAlignEvent *events = NULL;
    HWAAlignTrack track;
    char error[HWA_ERROR_SIZE];
    size_t overlap_frame;

    memset(&manifest, 0, sizeof(manifest));
    memset(&track, 0, sizeof(track));
    CHECK(write_text(path, manifest_text),
          "could not write overlapping-rest fixture");
    CHECK(hwa_score_manifest_load(path, 100000U, 100U, &manifest,
                                  error, sizeof(error)) == 0,
          "overlapping-rest manifest failed: %s", error);
    CHECK(hwa_score_manifest_build_track(
              &manifest, 0.25, UINT64_C(100000), 100U,
              &frames, &events, &track, error, sizeof(error)) == 0,
          "overlapping-rest track failed: %s", error);
    overlap_frame = (size_t)floor(1.25 / 0.25);
    if (frames != NULL && overlap_frame < track.frame_count) {
        CHECK(frames[overlap_frame].activity == 1.0 &&
                  (frames[overlap_frame].score_flags &
                   HWA_ALIGN_FRAME_REST) == 0U &&
                  (frames[overlap_frame].evidence_flags &
                   HWA_ALIGNMENT_EVIDENCE_CHROMA) != 0U,
              "a rest in another voice hid a sounding note");
    }
    hwa_score_manifest_release_track(frames, events, &track);
    hwa_score_manifest_free(&manifest);
}

#if !defined(_WIN32)
static void test_symlink_regular(const char *directory, const char *path)
{
    HWAScoreManifest manifest;
    char link_path[PATH_MAX];
    char error[HWA_ERROR_SIZE];
    int length = snprintf(link_path, sizeof(link_path),
                          "%s/score-link.csv", directory);

    CHECK(length >= 0 && (size_t)length < sizeof(link_path),
          "score symlink path is too long");
    CHECK(write_text(path, valid_manifest),
          "could not write score symlink target");
    CHECK(symlink(path, link_path) == 0,
          "could not make score symlink fixture");
    memset(&manifest, 0, sizeof(manifest));
    CHECK(hwa_score_manifest_load(link_path, 100000U, 100U, &manifest,
                                  error, sizeof(error)) == 0 &&
              manifest.event_count == 8U,
          "symlink to a regular score failed: %s", error);
    hwa_score_manifest_free(&manifest);
    (void)remove_file(link_path);
}

static void test_fifo_preflight(const char *path)
{
    HWAScoreManifest manifest;
    char error[HWA_ERROR_SIZE];

    (void)remove_file(path);
    CHECK(mkfifo(path, 0600) == 0,
          "could not make score FIFO fixture");
    memset(&manifest, 0, sizeof(manifest));
    CHECK(hwa_score_manifest_load(path, 100000U, 100U, &manifest,
                                  error, sizeof(error)) != 0 &&
              strstr(error, "regular file") != NULL,
          "score FIFO was not rejected before open: %s", error);
    hwa_score_manifest_free(&manifest);
    (void)remove_file(path);
}
#endif

int main(void)
{
    char directory[PATH_MAX];
    char path[PATH_MAX];

    CHECK(make_workspace(directory, path),
          "could not make score test workspace");
    if (failures == 0) {
        test_valid_manifest(path);
        test_limits(path);
        test_bad_manifests(path);
        test_csv_line_forms(path);
        test_tie_chains(path);
        test_overlapping_rest_voice(path);
        test_tempo_lookup_is_linear(path);
#if !defined(_WIN32)
        test_symlink_regular(directory, path);
        test_fifo_preflight(path);
#endif
    }
    (void)remove_file(path);
    (void)remove_directory(directory);
    if (failures != 0) {
        (void)fprintf(stderr, "%d score-manifest test(s) failed\n",
                      failures);
        return 1;
    }
    return 0;
}

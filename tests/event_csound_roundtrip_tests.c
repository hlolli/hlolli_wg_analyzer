#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "alignment.h"
#include "event_csound.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#define TEST_MKDIR(path) _mkdir(path)
#define TEST_RMDIR(path) _rmdir(path)
#define TEST_UNLINK(path) _unlink(path)
#define TEST_PID() ((long)_getpid())
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define TEST_MKDIR(path) mkdir((path), 0700)
#define TEST_RMDIR(path) rmdir(path)
#define TEST_UNLINK(path) unlink(path)
#define TEST_PID() ((long)getpid())
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TEST_SAMPLE_RATE 8000U
#define TEST_NOTE_FRAMES 4000U
#define TEST_NOTE_COUNT 4U

static int failures;

#define CHECK(condition, ...)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            (void)fprintf(stderr, __VA_ARGS__);                              \
            (void)fputc('\n', stderr);                                       \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static int join_path(char path[PATH_MAX],
                     const char *directory,
                     const char *name)
{
    int length = snprintf(path, PATH_MAX, "%s/%s", directory, name);
    return length >= 0 && (size_t)length < PATH_MAX;
}

static int make_workspace(char directory[PATH_MAX])
{
    unsigned attempt;
#if defined(_WIN32)
    const char *root = getenv("TEMP");
    if (root == NULL || root[0] == '\0') root = ".";
#else
    const char *root = "/tmp";
#endif
    for (attempt = 0U; attempt < 100U; ++attempt) {
        int length = snprintf(directory, PATH_MAX,
                              "%s/hwa-event-csound-%ld-%u",
                              root, TEST_PID(), attempt);
        if (length < 0 || (size_t)length >= PATH_MAX) return 0;
        if (TEST_MKDIR(directory) == 0) return 1;
        if (errno != EEXIST) return 0;
    }
    return 0;
}

static int write_text(const char *path, const char *text)
{
    FILE *stream = fopen(path, "wb");
    size_t size = strlen(text);
    int result = stream != NULL && fwrite(text, 1U, size, stream) == size;
    if (stream != NULL && fclose(stream) != 0) result = 0;
    return result;
}

static void remove_event_bundle(const char *directory)
{
    static const char *const names[] = {
        "manifest.json", "events.jsonl", "traces.jsonl"
    };
    char path[PATH_MAX];
    size_t index;
    for (index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (join_path(path, directory, names[index])) (void)TEST_UNLINK(path);
    }
    (void)TEST_RMDIR(directory);
}

static char *read_text(const char *path)
{
    FILE *stream = fopen(path, "rb");
    long length;
    char *text;
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
    text = (char *)malloc((size_t)length + 1U);
    if (text == NULL) {
        (void)fclose(stream);
        return NULL;
    }
    {
        size_t bytes_read = fread(text, 1U, (size_t)length, stream);
        int close_result = fclose(stream);
        if (bytes_read != (size_t)length || close_result != 0) {
            free(text);
            return NULL;
        }
    }
    text[length] = '\0';
    return text;
}

static int read_stream_text(FILE *stream, char *text, size_t capacity)
{
    size_t bytes_read;
    if (stream == NULL || text == NULL || capacity < 2U) return 0;
    if (fseek(stream, 0L, SEEK_SET) != 0) return 0;
    bytes_read = fread(text, 1U, capacity - 1U, stream);
    if (ferror(stream)) return 0;
    text[bytes_read] = '\0';
    return 1;
}

static size_t score_note_count(const char *text)
{
    size_t count = 0U;
    const char *line = text;
    if (text == NULL) return 0U;
    while (*line != '\0') {
        const char *next;
        if (strncmp(line, "i 1 ", 4U) == 0) count++;
        next = strchr(line, '\n');
        if (next == NULL) break;
        line = next + 1;
    }
    return count;
}

static void make_bundle(HWAEventBundle *bundle,
                        HWAEventAudio *audio,
                        HWAPerformanceEvent events[TEST_NOTE_COUNT],
                        HWAEventValue values[TEST_NOTE_COUNT])
{
    static const double pitches[TEST_NOTE_COUNT] = {
        261.6255653005986, 329.6275569128699,
        391.9954359817493, 293.6647679174076
    };
    static char *const score_ids[TEST_NOTE_COUNT] = {
        "n1", "n2", "n3", "n4"
    };
    size_t index;
    memset(bundle, 0, sizeof(*bundle));
    memset(audio, 0, sizeof(*audio));
    memset(events, 0, TEST_NOTE_COUNT * sizeof(*events));
    memset(values, 0, TEST_NOTE_COUNT * sizeof(*values));
    audio->id = 1U;
    audio->kind = HWA_EVENT_SOURCE_RECORDING;
    audio->name = (char *)"round-trip source";
    memset(audio->sha256, '0', HWA_SHA256_HEX_SIZE - 1U);
    audio->sha256[HWA_SHA256_HEX_SIZE - 1U] = '\0';
    audio->format.container = HWA_CONTAINER_RIFF;
    audio->format.encoding = HWA_ENCODING_PCM;
    audio->format.channels = 1U;
    audio->format.sample_rate_hz = TEST_SAMPLE_RATE;
    audio->format.bits_per_sample = 16U;
    audio->format.valid_bits_per_sample = 16U;
    audio->format.block_align = 2U;
    audio->format.frames = TEST_NOTE_COUNT * TEST_NOTE_FRAMES;
    audio->format.data_bytes = audio->format.frames * 2U;
    audio->format.duration_seconds =
        (double)audio->format.frames / (double)TEST_SAMPLE_RATE;
    for (index = 0U; index < TEST_NOTE_COUNT; ++index) {
        values[index].name = (char *)"pitch-hz";
        values[index].kind = HWA_EVENT_VALUE_F64;
        values[index].basis = HWA_EVENT_INFERENCE;
        values[index].number = pitches[index];
        values[index].unit = (char *)"Hz";
        values[index].selected = 1;
        events[index].id = (uint64_t)index + 1U;
        events[index].kind = (char *)"note";
        events[index].source_recording_id = 1U;
        events[index].start_sample = (uint64_t)index * TEST_NOTE_FRAMES;
        events[index].end_sample = events[index].start_sample +
                                   TEST_NOTE_FRAMES;
        events[index].score_event_id = score_ids[index];
        events[index].values = &values[index];
        events[index].value_count = 1U;
    }
    bundle->audio = audio;
    bundle->audio_count = 1U;
    bundle->events = events;
    bundle->event_count = TEST_NOTE_COUNT;
}

static int run_csound(const char *csound,
                      const char *orchestra,
                      const char *score,
                      const char *output)
{
    const char *arguments[] = {
        csound, "-d", "-m0", "-j", "1", "--sample-accurate",
        "--nopeaks", "-W", "-s", "-o", output, orchestra, score, NULL
    };
#if defined(_WIN32)
    intptr_t status = _spawnv(_P_WAIT, csound, arguments);
    return status >= 0 && status <= INT_MAX ? (int)status : -1;
#else
    pid_t child = fork();
    int status;
    if (child < 0) return -1;
    if (child == 0) {
        (void)execv(csound, (char *const *)arguments);
        _exit(127);
    }
    if (waitpid(child, &status, 0) != child) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static int match_event(const HWAAlignment *alignment,
                       const HWAPerformanceEvent *event,
                       double tolerance)
{
    size_t index;
    double expected_start = (double)event->start_sample /
                            (double)TEST_SAMPLE_RATE;
    double expected_end = (double)event->end_sample /
                          (double)TEST_SAMPLE_RATE;
    for (index = 0U; index < alignment->match_count; ++index) {
        const HWAAlignmentMatch *match = &alignment->matches[index];
        if (match->event_id != NULL && event->score_event_id != NULL &&
            strcmp(match->event_id, event->score_event_id) == 0) {
            return match->status == HWA_ALIGNMENT_MATCHED &&
                   fabs(match->reference_start_seconds - expected_start) <=
                       1e-12 &&
                   fabs(match->reference_end_seconds - expected_end) <=
                       1e-12 &&
                   fabs(match->target_start_seconds - expected_start) <=
                       tolerance &&
                   fabs(match->target_end_seconds - expected_end) <= tolerance;
        }
    }
    return 0;
}

static void test_score_text_preserves_samples(void)
{
    static const char expected[] =
        "; HWA_CSOUND_SCORE 1\n"
        "; p4=pitch_hz p5=level p6=event_id "
        "p7=start_sample p8=end_sample\n"
        "i 1 0 0.5 261.62556530059862 0.2 1 0 4000\n"
        "i 1 0.5 0.5 329.62755691286992 0.2 2 4000 8000\n"
        "i 1 1 0.5 391.99543598174932 0.2 3 8000 12000\n"
        "i 1 1.5 0.5 293.66476791740757 0.2 4 12000 16000\n"
        "f 0 2\n"
        "e\n";
    HWAEventBundle bundle;
    HWAEventAudio audio;
    HWAPerformanceEvent events[TEST_NOTE_COUNT];
    HWAEventValue values[TEST_NOTE_COUNT];
    HWAPerformanceEvent swap;
    char directory[PATH_MAX];
    char score[PATH_MAX];
    char error[HWA_ERROR_SIZE] = {0};
    char *text;
    FILE *stream;
    int score_result = -1;
    make_bundle(&bundle, &audio, events, values);
    swap = events[0];
    events[0] = events[3];
    events[3] = swap;
    CHECK(make_workspace(directory) &&
              join_path(score, directory, "events.sco"),
          "cannot make score-text workspace");
    if (failures != 0) return;
    stream = fopen(score, "wb");
    if (stream != NULL) {
        score_result = hwa_event_csound_score_write(
            stream, &bundle, 1U, error, sizeof(error));
        if (fclose(stream) != 0) score_result = -1;
    }
    CHECK(score_result == 0,
          "cannot write Csound score: %s", error);
    text = read_text(score);
    CHECK(text != NULL && strcmp(text, expected) == 0,
          "Csound score lost exact event fields:\n%s",
          text != NULL ? text : "<unreadable>");
    free(text);
    (void)TEST_UNLINK(score);
    (void)TEST_RMDIR(directory);
}

static void test_score_skips_note_without_selected_pitch(void)
{
    HWAEventBundle bundle;
    HWAEventAudio audio;
    HWAPerformanceEvent events[TEST_NOTE_COUNT];
    HWAEventValue values[TEST_NOTE_COUNT];
    char error[HWA_ERROR_SIZE] = {0};
    char score_text[2048] = {0};
    size_t bytes_read;
    FILE *stream = tmpfile();
    make_bundle(&bundle, &audio, events, values);
    values[0].selected = 0;
    CHECK(stream != NULL, "cannot open temporary score stream");
    if (stream == NULL) return;
    CHECK(hwa_event_csound_score_write(stream, &bundle, 1U,
                                       error, sizeof(error)) == 0,
          "one incomplete note stopped score export: %s", error);
    CHECK(read_stream_text(stream, score_text, sizeof(score_text)),
          "cannot read partial score text");
    bytes_read = strlen(score_text);
    CHECK(bytes_read < sizeof(score_text) - 1U,
          "partial score text filled its test buffer");
    CHECK(strstr(score_text, " 0.2 1 0 4000\n") == NULL &&
              strstr(score_text, " 0.2 2 4000 8000\n") != NULL &&
              strstr(score_text, " 0.2 3 8000 12000\n") != NULL &&
              strstr(score_text, " 0.2 4 12000 16000\n") != NULL &&
              score_note_count(score_text) == 3U,
          "partial score did not contain exactly the renderable notes:\n%s",
          score_text);
    (void)fclose(stream);
}

static void test_score_rejects_no_renderable_notes(void)
{
    HWAEventBundle bundle;
    HWAEventAudio audio;
    HWAPerformanceEvent events[TEST_NOTE_COUNT];
    HWAEventValue values[TEST_NOTE_COUNT];
    char error[HWA_ERROR_SIZE] = {0};
    FILE *stream = tmpfile();
    size_t index;
    make_bundle(&bundle, &audio, events, values);
    for (index = 0U; index < TEST_NOTE_COUNT; ++index)
        values[index].selected = 0;
    CHECK(stream != NULL, "cannot open empty-score stream");
    if (stream == NULL) return;
    CHECK(hwa_event_csound_score_write(stream, &bundle, 1U,
                                       error, sizeof(error)) != 0 &&
              strstr(error, "no renderable note") != NULL,
          "source with no renderable notes gave the wrong result: %s", error);
    (void)fclose(stream);
}

static void test_score_sorts_by_start_end_and_id(void)
{
    HWAEventBundle bundle;
    HWAEventAudio audio;
    HWAPerformanceEvent events[TEST_NOTE_COUNT];
    HWAEventValue values[TEST_NOTE_COUNT];
    char error[HWA_ERROR_SIZE] = {0};
    char score_text[2048] = {0};
    const char *id1;
    const char *id2;
    const char *id3;
    const char *id4;
    FILE *stream = tmpfile();
    make_bundle(&bundle, &audio, events, values);
    events[0].id = 4U;
    events[0].start_sample = 100U;
    events[0].end_sample = 400U;
    events[1].id = 3U;
    events[1].start_sample = 100U;
    events[1].end_sample = 300U;
    events[2].id = 2U;
    events[2].start_sample = 100U;
    events[2].end_sample = 300U;
    events[3].id = 1U;
    events[3].start_sample = 50U;
    events[3].end_sample = 400U;
    CHECK(stream != NULL, "cannot open ordering-test stream");
    if (stream == NULL) return;
    CHECK(hwa_event_csound_score_write(stream, &bundle, 1U,
                                       error, sizeof(error)) == 0,
          "cannot write ordering-test score: %s", error);
    CHECK(read_stream_text(stream, score_text, sizeof(score_text)),
          "cannot read ordering-test score");
    id1 = strstr(score_text, " 0.2 1 50 400\n");
    id2 = strstr(score_text, " 0.2 2 100 300\n");
    id3 = strstr(score_text, " 0.2 3 100 300\n");
    id4 = strstr(score_text, " 0.2 4 100 400\n");
    CHECK(id1 != NULL && id2 != NULL && id3 != NULL && id4 != NULL &&
              id1 < id2 && id2 < id3 && id3 < id4 &&
              score_note_count(score_text) == TEST_NOTE_COUNT,
          "score did not sort by start, end, and ID:\n%s", score_text);
    (void)fclose(stream);
}

static void test_csound_round_trip(const char *csound)
{
    static const char orchestra_text[] =
        "sr = 8000\n"
        "ksmps = 1\n"
        "nchnls = 1\n"
        "0dbfs = 1\n"
        "giSine ftgen 1, 0, 16384, 10, 1\n"
        "instr 1\n"
        "  iHz = p4\n"
        "  iAmp = p5 / 2.45\n"
        "  aEnv linseg 0, 0.01, 1, p3 - 0.02, 1, 0.01, 0\n"
        "  a1 oscili iAmp, iHz, giSine\n"
        "  a2 oscili iAmp * 0.50, iHz * 2, giSine\n"
        "  a3 oscili iAmp * 0.33, iHz * 3, giSine\n"
        "  a4 oscili iAmp * 0.25, iHz * 4, giSine\n"
        "  a5 oscili iAmp * 0.20, iHz * 5, giSine\n"
        "  a6 oscili iAmp * 0.17, iHz * 6, giSine\n"
        "  out aEnv * (a1 + a2 + a3 + a4 + a5 + a6)\n"
        "endin\n";
    static const char score_manifest[] =
        "event_id,kind,start_beats,duration_beats,midi_note,velocity,voice,"
        "tie,dynamic,mark,score_position,tempo_bpm\n"
        "tempo,tempo,0,0,,,,,,,m1,120\n"
        "n1,note,0,1,60,88,solo,none,mf,,m1b1,\n"
        "n2,note,1,1,64,88,solo,none,mf,,m1b2,\n"
        "n3,note,2,1,67,88,solo,none,mf,,m1b3,\n"
        "n4,note,3,1,62,88,solo,none,mf,,m1b4,";
    HWAEventBundle bundle;
    HWAEventAudio audio;
    HWAPerformanceEvent events[TEST_NOTE_COUNT];
    HWAEventValue values[TEST_NOTE_COUNT];
    HWAEventBundle loaded;
    HWAEventBundleLimits limits;
    HWAAlignmentOptions options;
    HWAAlignment alignment;
    char directory[PATH_MAX];
    char orchestra[PATH_MAX];
    char bundle_path[PATH_MAX];
    char score[PATH_MAX];
    char manifest[PATH_MAX];
    char rendered[PATH_MAX];
    char error[HWA_ERROR_SIZE] = {0};
    FILE *stream;
    size_t index;
    int score_result = -1;
    make_bundle(&bundle, &audio, events, values);
    memset(&loaded, 0, sizeof(loaded));
    memset(&alignment, 0, sizeof(alignment));
    CHECK(make_workspace(directory) &&
              join_path(bundle_path, directory, "events.hwa-events") &&
              join_path(orchestra, directory, "generic.orc") &&
              join_path(score, directory, "events.sco") &&
              join_path(manifest, directory, "score.csv") &&
              join_path(rendered, directory, "rendered.wav"),
          "cannot make round-trip workspace");
    if (failures != 0) return;
    hwa_event_bundle_limits_default(&limits);
    CHECK(hwa_event_bundle_write(bundle_path, &bundle, NULL, 0U, &limits,
                                 error, sizeof(error)) == 0 &&
              hwa_event_bundle_read(bundle_path, &limits, &loaded,
                                    error, sizeof(error)) == 0,
          "cannot save and reload event bundle: %s", error);
    stream = fopen(score, "wb");
    if (stream != NULL) {
        score_result = hwa_event_csound_score_write(
            stream, &loaded, 1U, error, sizeof(error));
        if (fclose(stream) != 0) score_result = -1;
    }
    CHECK(score_result == 0 && write_text(orchestra, orchestra_text) &&
              write_text(manifest, score_manifest),
          "cannot write round-trip inputs: %s", error);
    if (failures == 0) {
        CHECK(run_csound(csound, orchestra, score, rendered) == 0,
              "Csound render failed");
    }
    if (failures == 0) {
        hwa_alignment_options_default(&options);
        options.analysis.frame_size = 512U;
        options.analysis.hop_size = 128U;
        options.alignment_step_seconds = 0.05;
        options.coarse_step_seconds = 0.20;
        options.dtw_band_seconds = 2.0;
        options.fine_radius_seconds = 0.40;
        options.refine_radius_seconds = 0.10;
        CHECK(hwa_align_score_manifest_wav(
                  manifest, rendered, &options, NULL, 0U, &alignment,
                  error, sizeof(error)) == 0,
              "rendered score alignment failed: %s", error);
    }
    CHECK(alignment.match_count == TEST_NOTE_COUNT,
          "alignment returned %zu matches", alignment.match_count);
    CHECK(alignment.matched_coverage > 0.90,
          "alignment coverage is %.6f", alignment.matched_coverage);
    for (index = 0U; index < TEST_NOTE_COUNT; ++index) {
        CHECK(index < loaded.event_count &&
                  match_event(&alignment, &loaded.events[index], 0.10),
              "event %zu did not round trip within 100 ms", index + 1U);
    }
    hwa_alignment_free(&alignment);
    hwa_event_bundle_free(&loaded);
    (void)TEST_UNLINK(rendered);
    (void)TEST_UNLINK(manifest);
    (void)TEST_UNLINK(score);
    (void)TEST_UNLINK(orchestra);
    remove_event_bundle(bundle_path);
    (void)TEST_RMDIR(directory);
}

int main(int argc, char **argv)
{
    test_score_text_preserves_samples();
    test_score_skips_note_without_selected_pitch();
    test_score_rejects_no_renderable_notes();
    test_score_sorts_by_start_end_and_id();
    if (argc == 2 && argv[1][0] != '\0') {
        test_csound_round_trip(argv[1]);
    } else {
        (void)puts("Csound round-trip skipped");
    }
    if (failures != 0) {
        (void)fprintf(stderr, "%d event Csound test(s) failed\n", failures);
        return 1;
    }
    (void)puts("event Csound tests passed");
    return 0;
}

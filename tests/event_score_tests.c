#include "hlolli_wg_analyzer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(test) do { if (!(test)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #test); failures++; } } while (0)

static void fixture(HWAEventBundle *bundle, HWAEventAudio *audio,
                    HWAPerformanceEvent events[8], HWAEventValue values[8])
{
    static char *const parts[] = {"violin-1", "violin-2", "viola", "cello", "violin-1", "violin-1", "viola", "cello"};
    static const double pitches[] = {440.0, 659.2551138257398, 261.6255653005986, 130.8127826502993, 523.2511306011972, 330.0, 220.0, 196.0};
    static const uint64_t starts[] = {0, 4000, 0, 0, 4000, 12000, 16000, 16000};
    static const uint64_t ends[] = {8000, 12000, 16000, 16000, 12000, 16000, 20000, 20000};
    size_t i;
    memset(bundle, 0, sizeof(*bundle));
    memset(audio, 0, sizeof(*audio));
    memset(events, 0, 8U*sizeof(*events));
    memset(values, 0, 8U*sizeof(*values));
    audio->id = 1U;
    audio->kind = HWA_EVENT_SOURCE_RECORDING;
    audio->name = (char *)"quartet";
    memset(audio->sha256, '0', 64U);
    audio->format.container = HWA_CONTAINER_RIFF;
    audio->format.encoding = HWA_ENCODING_PCM;
    audio->format.channels = 1U;
    audio->format.sample_rate_hz = 8000U;
    audio->format.bits_per_sample = 16U;
    audio->format.valid_bits_per_sample = 16U;
    audio->format.block_align = 2U;
    audio->format.frames = 24000U;
    audio->format.data_bytes = 48000U;
    audio->format.duration_seconds = 3.0;
    for (i = 0U; i < 8U; ++i) {
        values[i].name = (char *)"pitch-hz";
        values[i].kind = HWA_EVENT_VALUE_F64;
        values[i].basis = HWA_EVENT_INFERENCE;
        values[i].number = pitches[i];
        values[i].unit = (char *)"Hz";
        values[i].selected = 1;
        events[i].id = (uint64_t)i+1U;
        events[i].kind = (char *)"note";
        events[i].source_recording_id = 1U;
        events[i].start_sample = starts[i];
        events[i].end_sample = ends[i];
        events[i].part = parts[i];
        events[i].voice = (char *)"1";
        events[i].values = &values[i];
        events[i].value_count = 1U;
    }
    values[5].selected = 0;
    events[6].kind = (char *)"ornament";
    events[7].kind = (char *)"rest";
    bundle->audio = audio;
    bundle->audio_count = 1U;
    bundle->events = events;
    bundle->event_count = 8U;
}

static int render(const HWAEventBundle *bundle, uint64_t source,
                   const HWAEventScoreOptions *options, char text[16384])
{
    char error[HWA_ERROR_SIZE];
    FILE *stream = tmpfile();
    int result;
    size_t bytes;
    CHECK(stream != NULL);
    if (stream == NULL) return -1;
    result = hwa_event_score_write(stream, bundle, source, options, error, sizeof(error));
    CHECK(result == 0 || error[0] != '\0');
    CHECK(fflush(stream) == 0);
    CHECK(fseek(stream, 0L, SEEK_SET) == 0);
    bytes = fread(text, 1U, 16383U, stream);
    text[bytes] = '\0';
    CHECK(!ferror(stream));
    CHECK(fclose(stream) == 0);
    if (result != 0) CHECK(bytes == 0U);
    return result;
}

int main(int argc, char **argv)
{
    HWAEventBundle bundle;
    HWAEventAudio audio;
    HWAPerformanceEvent events[8];
    HWAEventValue values[8];
    HWAEventScoreOptions options;
    char text[16384], other[16384], error[HWA_ERROR_SIZE];
    fixture(&bundle, &audio, events, values);
    if (argc == 2) {
        HWAEventBundleLimits limits;
        hwa_event_bundle_limits_default(&limits);
        if (hwa_event_bundle_write(argv[1], &bundle, NULL, 0U, &limits, error, sizeof(error)) != 0) {
            fprintf(stderr, "%s\n", error);
            return 1;
        }
        return 0;
    }
    hwa_event_score_options_default(&options);
    CHECK(render(&bundle, 1U, &options, text) == 0);
    CHECK(strstr(text, "; HWA_CSOUND_SCORE 2\n") != NULL);
    CHECK(strstr(text, "notes=5 tracks=4 omitted_unpitched_notes=1") != NULL);
    CHECK(strstr(text, "i 3 0 1 440 0.2 1 0 8000 3\n") != NULL);
    CHECK(strstr(text, "i 3 0.5 1 ") != NULL);
    CHECK(strstr(text, "i 4 0.5 1 ") != NULL);
    CHECK(strstr(text, "f 0 3\ne\n") != NULL);
    { HWAPerformanceEvent saved = events[0]; events[0] = events[4]; events[4] = saved; }
    CHECK(render(&bundle, 1U, &options, other) == 0);
    CHECK(strcmp(text, other) == 0);
    fixture(&bundle, &audio, events, values);
    options.kind = HWA_EVENT_SCORE_LILYPOND;
    CHECK(render(&bundle, 1U, &options, text) != 0);
    options.tempo_bpm = 120U;
    CHECK(render(&bundle, 1U, &options, text) == 0);
    CHECK(strstr(text, "a'2 ") != NULL);
    CHECK(strstr(text, "start_tick=32 end_tick=96 lane=2") != NULL);
    CHECK(strstr(text, "\\tempo 4 = 120") != NULL);
    CHECK(strstr(text, "\\time ") == NULL);
    options.max_lanes_per_track = 1U;
    CHECK(render(&bundle, 1U, &options, text) != 0);
    options.max_lanes_per_track = 64U;
    events[0].start_sample = 1U;
    events[0].end_sample = 2U;
    CHECK(render(&bundle, 1U, &options, text) == 0);
    CHECK(strstr(text, "start_tick=0 end_tick=1 lane=1") != NULL);
    CHECK(strstr(text, "a'128 ") != NULL);
    fixture(&bundle, &audio, events, values);
    options.max_notes = 1U;
    CHECK(render(&bundle, 1U, &options, text) != 0);
    options.max_notes = 100U;
    options.max_tracks = 1U;
    CHECK(render(&bundle, 1U, &options, text) != 0);
    options.max_tracks = 256U;
    options.max_work_bytes = 1U;
    CHECK(render(&bundle, 1U, &options, text) != 0);
    hwa_event_score_options_default(&options);
    CHECK(render(&bundle, 9U, &options, text) != 0);
    values[0].number = NAN;
    CHECK(render(&bundle, 1U, &options, text) != 0);
    values[0].number = 440.0;
    values[0].unit = (char *)"wrong";
    CHECK(render(&bundle, 1U, &options, text) != 0);
    values[0].unit = (char *)"Hz";
    { HWAEventValue duplicate[2]; duplicate[0] = values[0]; duplicate[1] = values[0];
      events[0].values = duplicate; events[0].value_count = 2U;
      CHECK(render(&bundle, 1U, &options, text) != 0); }
    fixture(&bundle, &audio, events, values);
    events[0].id = UINT64_C(9007199254740992);
    CHECK(render(&bundle, 1U, &options, text) != 0);
    fixture(&bundle, &audio, events, values);
    CHECK(render(NULL, 1U, &options, text) != 0);
    CHECK(hwa_event_score_write(NULL, &bundle, 1U, NULL, error, sizeof(error)) != 0);
    hwa_event_score_options_default(NULL);
    return failures == 0 ? 0 : 1;
}

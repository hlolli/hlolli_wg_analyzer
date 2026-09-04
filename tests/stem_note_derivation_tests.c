#include "stem_note_derivation.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_FRAMES UINT64_C(32)
#define TEST_SAMPLE_RATE UINT32_C(44100)

static const char test_stem_model_sha256[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char test_note_model_sha256[] =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static const char test_source_sha256[] =
    "cccccccccccccccccccccccccccccccc"
    "cccccccccccccccccccccccccccccccc";
static const char test_stem_settings[] =
    "{\"task\":\"org.hlolli.instrument-stems-v1\","
    "\"seed\":\"00000000000000000000\",\"inputs\":[{"
    "\"id\":\"source\",\"role\":\"source-recording\","
    "\"media_type\":\"audio/wav\",\"name\":\"mix.wav\","
    "\"bytes\":\"00000000000000000172\","
    "\"sha256\":\"cccccccccccccccccccccccccccccccc"
    "cccccccccccccccccccccccccccccccc\"}],"
    "\"runtime\":{\"name\":\"fixture\",\"version\":\"1\","
    "\"backend\":\"cpu\",\"fallback\":\"\","
    "\"adapter_sha256\":\"dddddddddddddddddddddddddddddddd"
    "dddddddddddddddddddddddddddddddd\"},\"task_settings\":{}}";

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

typedef struct TestBytes {
    unsigned char *data;
    size_t size;
} TestBytes;

typedef struct TestWave {
    TestBytes bytes;
    HWAFormat format;
} TestWave;

typedef struct TestFixture {
    HWAEventBundle bundle;
    HWAEventProvider provider;
    HWAEventAudio audio[3];
    HWAPerformanceEvent regions[2];
    HWAEventValue region_values[2];
    TestWave stem_waves[2];
    HWAStemNoteAudioSource sources[2];
} TestFixture;

typedef enum TestProviderMode {
    TEST_PROVIDER_NOTES = 0,
    TEST_PROVIDER_ZERO_NOTES,
    TEST_PROVIDER_NON_NOTE
} TestProviderMode;

typedef struct TestProviderState {
    TestProviderMode mode;
    size_t starts;
    size_t frees;
    uint64_t source_ids[4];
    char source_names[4][64];
} TestProviderState;

typedef struct TestProviderTask {
    HWAInferenceOutput output;
    HWAEventBundle bundle;
    HWAEventProvider provider;
    HWAEventAudio audio;
    HWAPerformanceEvent *events;
    HWAEventValue *values;
} TestProviderTask;

static void test_le16(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char)(value & UINT16_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT16_C(0xff));
}

static void test_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value & UINT32_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT32_C(0xff));
    bytes[2] = (unsigned char)((value >> 16U) & UINT32_C(0xff));
    bytes[3] = (unsigned char)((value >> 24U) & UINT32_C(0xff));
}

static int test_wave_build(TestWave *wave, int16_t bias)
{
    const uint16_t channels = UINT16_C(2);
    const uint16_t block_align = UINT16_C(4);
    const uint32_t data_bytes =
        (uint32_t)(TEST_FRAMES * (uint64_t)block_align);
    const size_t file_bytes = (size_t)data_bytes + 44U;
    uint64_t frame;
    memset(wave, 0, sizeof(*wave));
    wave->bytes.data = (unsigned char *)calloc(file_bytes, 1U);
    if (wave->bytes.data == NULL) return -1;
    wave->bytes.size = file_bytes;
    memcpy(wave->bytes.data, "RIFF", 4U);
    test_le32(wave->bytes.data + 4U, data_bytes + UINT32_C(36));
    memcpy(wave->bytes.data + 8U, "WAVEfmt ", 8U);
    test_le32(wave->bytes.data + 16U, UINT32_C(16));
    test_le16(wave->bytes.data + 20U, UINT16_C(1));
    test_le16(wave->bytes.data + 22U, channels);
    test_le32(wave->bytes.data + 24U, TEST_SAMPLE_RATE);
    test_le32(wave->bytes.data + 28U,
              TEST_SAMPLE_RATE * (uint32_t)block_align);
    test_le16(wave->bytes.data + 32U, block_align);
    test_le16(wave->bytes.data + 34U, UINT16_C(16));
    memcpy(wave->bytes.data + 36U, "data", 4U);
    test_le32(wave->bytes.data + 40U, data_bytes);
    for (frame = 0U; frame < TEST_FRAMES; ++frame) {
        int32_t signed_sample = (int32_t)frame - 16 + (int32_t)bias;
        uint16_t sample = signed_sample < 0
                              ? (uint16_t)(UINT32_C(65536) +
                                           (uint32_t)signed_sample)
                              : (uint16_t)signed_sample;
        size_t offset = 44U + (size_t)frame * (size_t)block_align;
        test_le16(wave->bytes.data + offset, sample);
        test_le16(wave->bytes.data + offset + 2U, sample);
    }
    wave->format.container = HWA_CONTAINER_RIFF;
    wave->format.encoding = HWA_ENCODING_PCM;
    wave->format.channels = channels;
    wave->format.sample_rate_hz = TEST_SAMPLE_RATE;
    wave->format.bits_per_sample = UINT16_C(16);
    wave->format.valid_bits_per_sample = UINT16_C(16);
    wave->format.block_align = block_align;
    wave->format.frames = TEST_FRAMES;
    wave->format.data_bytes = data_bytes;
    wave->format.duration_seconds =
        (double)TEST_FRAMES / (double)TEST_SAMPLE_RATE;
    return 0;
}

static int test_read_at(void *context,
                        uint64_t offset,
                        unsigned char *destination,
                        size_t size)
{
    const TestBytes *bytes = (const TestBytes *)context;
    if (bytes == NULL || destination == NULL ||
        offset > (uint64_t)bytes->size ||
        (uint64_t)size > (uint64_t)bytes->size - offset)
        return -1;
    memcpy(destination, bytes->data + (size_t)offset, size);
    return 0;
}

static void test_set_audio(HWAEventAudio *audio,
                           uint64_t id,
                           HWAEventAudioKind kind,
                           char *name,
                           char *relative_path,
                           const char *sha256,
                           uint64_t file_bytes,
                           const HWAFormat *format,
                           uint64_t source_id,
                           int source_id_valid)
{
    memset(audio, 0, sizeof(*audio));
    audio->id = id;
    audio->kind = kind;
    audio->name = name;
    audio->relative_path = relative_path;
    audio->path_hint = "";
    memcpy(audio->sha256, sha256, HWA_SHA256_HEX_SIZE);
    audio->file_bytes = file_bytes;
    audio->format = *format;
    audio->source_recording_id = source_id;
    audio->source_recording_id_valid = source_id_valid;
}

static void test_set_region(TestFixture *fixture,
                            size_t row,
                            uint64_t event_id,
                            uint64_t stem_audio_id,
                            char *stem_id,
                            char *instrument)
{
    HWAPerformanceEvent *event = &fixture->regions[row];
    HWAEventValue *value = &fixture->region_values[row];
    memset(event, 0, sizeof(*event));
    memset(value, 0, sizeof(*value));
    value->name = "instrument";
    value->kind = HWA_EVENT_VALUE_TEXT;
    value->basis = HWA_EVENT_INFERENCE;
    value->text = instrument;
    value->unit = "";
    value->provider_id = UINT64_C(1);
    value->provider_id_valid = 1;
    value->selected = 1;
    event->id = event_id;
    event->kind = "instrument-region";
    event->source_recording_id = UINT64_C(9);
    event->evidence_audio_id = stem_audio_id;
    event->evidence_audio_id_valid = 1;
    event->start_sample = 0U;
    event->end_sample = TEST_FRAMES;
    event->voice = "";
    event->part = stem_id;
    event->score_event_id = "";
    event->values = value;
    event->value_count = 1U;
}

static int test_fixture_init(TestFixture *fixture)
{
    char error[HWA_ERROR_SIZE] = {0};
    char stem_sha256[2][HWA_SHA256_HEX_SIZE];
    memset(fixture, 0, sizeof(*fixture));
    if (test_wave_build(&fixture->stem_waves[0], INT16_C(100)) != 0 ||
        test_wave_build(&fixture->stem_waves[1], INT16_C(-100)) != 0)
        goto failure;
    fixture->provider.id = UINT64_C(1);
    fixture->provider.name = "org.hlolli.instrument-stem-provider";
    fixture->provider.version = "1";
    memcpy(fixture->provider.model_sha256, test_stem_model_sha256,
           HWA_SHA256_HEX_SIZE);
    fixture->provider.settings_json = (char *)test_stem_settings;
    if (hwa_inference_byte_source_sha256(
            &(HWAByteSource){&fixture->stem_waves[0].bytes,
                             "zeta.wav",
                             (uint64_t)fixture->stem_waves[0].bytes.size,
                             test_read_at},
            (uint64_t)fixture->stem_waves[0].bytes.size,
            stem_sha256[0], error, sizeof(error)) != 0 ||
        hwa_inference_byte_source_sha256(
            &(HWAByteSource){&fixture->stem_waves[1].bytes,
                             "alpha.wav",
                             (uint64_t)fixture->stem_waves[1].bytes.size,
                             test_read_at},
            (uint64_t)fixture->stem_waves[1].bytes.size,
            stem_sha256[1], error, sizeof(error)) != 0) {
        CHECK(0, "cannot hash stem test WAVE: %s", error);
        goto failure;
    }
    test_set_audio(&fixture->audio[0], UINT64_C(9),
                   HWA_EVENT_SOURCE_RECORDING, "mix.wav", "",
                   test_source_sha256,
                   (uint64_t)fixture->stem_waves[0].bytes.size,
                   &fixture->stem_waves[0].format, 0U, 0);
    test_set_audio(&fixture->audio[1], UINT64_C(1),
                   HWA_EVENT_INSTRUMENT_STEM, "alpha", "audio/alpha.wav",
                   stem_sha256[1],
                   (uint64_t)fixture->stem_waves[1].bytes.size,
                   &fixture->stem_waves[1].format, UINT64_C(9), 1);
    test_set_audio(&fixture->audio[2], UINT64_C(2),
                   HWA_EVENT_INSTRUMENT_STEM, "zeta", "audio/zeta.wav",
                   stem_sha256[0],
                   (uint64_t)fixture->stem_waves[0].bytes.size,
                   &fixture->stem_waves[0].format, UINT64_C(9), 1);
    test_set_region(fixture, 0U, UINT64_C(1), UINT64_C(1),
                    "alpha", "piano");
    test_set_region(fixture, 1U, UINT64_C(2), UINT64_C(2),
                    "zeta", "other");
    fixture->sources[0].audio_id = UINT64_C(2);
    fixture->sources[0].bytes.context = &fixture->stem_waves[0].bytes;
    fixture->sources[0].bytes.name = "ignored-zeta-name.wav";
    fixture->sources[0].bytes.size =
        (uint64_t)fixture->stem_waves[0].bytes.size;
    fixture->sources[0].bytes.read_at = test_read_at;
    fixture->sources[1].audio_id = UINT64_C(1);
    fixture->sources[1].bytes.context = &fixture->stem_waves[1].bytes;
    fixture->sources[1].bytes.name = "ignored-alpha-name.wav";
    fixture->sources[1].bytes.size =
        (uint64_t)fixture->stem_waves[1].bytes.size;
    fixture->sources[1].bytes.read_at = test_read_at;
    fixture->bundle.providers = &fixture->provider;
    fixture->bundle.provider_count = 1U;
    fixture->bundle.audio = fixture->audio;
    fixture->bundle.audio_count = 3U;
    fixture->bundle.events = fixture->regions;
    fixture->bundle.event_count = 2U;
    return 1;
failure:
    free(fixture->stem_waves[0].bytes.data);
    free(fixture->stem_waves[1].bytes.data);
    memset(fixture, 0, sizeof(*fixture));
    return 0;
}

static void test_fixture_free(TestFixture *fixture)
{
    free(fixture->stem_waves[0].bytes.data);
    free(fixture->stem_waves[1].bytes.data);
    memset(fixture, 0, sizeof(*fixture));
}

static void test_task_free(void *context, void *task_value)
{
    TestProviderState *state = (TestProviderState *)context;
    TestProviderTask *task = (TestProviderTask *)task_value;
    if (state != NULL) state->frees++;
    if (task == NULL) return;
    free(task->events);
    free(task->values);
    free(task);
}

static int test_provider_poll(void *context,
                              void *task_value,
                              HWAInferencePollState *poll_state,
                              const HWAInferenceOutput **output,
                              char *error,
                              size_t error_size);

static void test_provider_destroy(void *context)
{
    (void)context;
}

static int test_provider_start(void *context,
                               const HWAInferenceRequest *request,
                               void **task_value,
                               char *error,
                               size_t error_size)
{
    TestProviderState *state = (TestProviderState *)context;
    TestProviderTask *task = NULL;
    size_t call;
    size_t note_count;
    size_t index;
    if (task_value != NULL) *task_value = NULL;
    if (state == NULL || request == NULL || task_value == NULL ||
        state->starts >= 4U ||
        strcmp(request->task, "org.hlolli.note-events-on-audio-v1") != 0 ||
        request->input_count != 1U || request->inputs == NULL ||
        hwa_inference_request_validate(
            &(HWAInferenceProvider){"test.note-provider", "1",
                                    test_note_model_sha256, state,
                                    test_provider_start, test_provider_poll,
                                    test_task_free, test_provider_destroy},
            request, error, error_size) != 0)
        return -1;
    call = state->starts++;
    state->source_ids[call] = request->source_recording_id;
    (void)snprintf(state->source_names[call],
                   sizeof(state->source_names[call]), "%s",
                   request->inputs[0].bytes.name);
    note_count = state->mode == TEST_PROVIDER_ZERO_NOTES ? 0U : 1U;
    task = (TestProviderTask *)calloc(1U, sizeof(*task));
    if (task == NULL) return -1;
    if (note_count != 0U) {
        task->events = (HWAPerformanceEvent *)calloc(
            note_count, sizeof(*task->events));
        task->values = (HWAEventValue *)calloc(
            note_count, sizeof(*task->values));
        if (task->events == NULL || task->values == NULL) {
            test_task_free(NULL, task);
            return -1;
        }
    }
    task->provider.id = UINT64_C(41);
    task->provider.name = "test.note-provider";
    task->provider.version = "1";
    memcpy(task->provider.model_sha256, test_note_model_sha256,
           HWA_SHA256_HEX_SIZE);
    task->provider.settings_json = "{\"fixture\":true}";
    test_set_audio(&task->audio, request->source_recording_id,
                   HWA_EVENT_SOURCE_RECORDING,
                   (char *)request->inputs[0].bytes.name, "",
                   request->inputs[0].sha256,
                   request->inputs[0].bytes.size,
                   &request->source_format, 0U, 0);
    for (index = 0U; index < note_count; ++index) {
        HWAEventValue *value = &task->values[index];
        HWAPerformanceEvent *event = &task->events[index];
        value->name = "pitch-hz";
        value->kind = HWA_EVENT_VALUE_F64;
        value->basis = HWA_EVENT_INFERENCE;
        value->number = call == 0U ? 440.0 : 660.0;
        value->unit = "Hz";
        value->score = 0.75;
        value->score_valid = 1;
        value->provider_id = UINT64_C(41);
        value->provider_id_valid = 1;
        value->selected = 1;
        event->id = UINT64_C(51) + (uint64_t)index;
        event->kind = state->mode == TEST_PROVIDER_NON_NOTE
                          ? "rest"
                          : "note";
        event->source_recording_id = request->source_recording_id;
        event->evidence_audio_id = request->source_recording_id;
        event->evidence_audio_id_valid = 1;
        event->start_sample = call == 0U ? UINT64_C(3) : UINT64_C(5);
        event->end_sample = call == 0U ? UINT64_C(8) : UINT64_C(11);
        event->voice = "child-voice";
        event->part = "child-part";
        event->score_event_id = "child-score-event";
        event->values = value;
        event->value_count = 1U;
    }
    task->bundle.providers = &task->provider;
    task->bundle.provider_count = 1U;
    task->bundle.audio = &task->audio;
    task->bundle.audio_count = 1U;
    task->bundle.events = task->events;
    task->bundle.event_count = note_count;
    task->output.bundle = &task->bundle;
    *task_value = task;
    return 0;
}

static int test_provider_poll(void *context,
                              void *task_value,
                              HWAInferencePollState *poll_state,
                              const HWAInferenceOutput **output,
                              char *error,
                              size_t error_size)
{
    TestProviderTask *task = (TestProviderTask *)task_value;
    (void)context;
    (void)error;
    (void)error_size;
    if (task == NULL || poll_state == NULL || output == NULL) return -1;
    *poll_state = HWA_INFERENCE_READY;
    *output = &task->output;
    return 0;
}

static void test_provider_init(HWAInferenceProvider *provider,
                               TestProviderState *state)
{
    memset(provider, 0, sizeof(*provider));
    provider->name = "test.note-provider";
    provider->version = "1";
    provider->model_sha256 = test_note_model_sha256;
    provider->context = state;
    provider->start = test_provider_start;
    provider->poll = test_provider_poll;
    provider->task_free = test_task_free;
    provider->destroy = test_provider_destroy;
}

static int test_run(TestFixture *fixture,
                    TestProviderState *state,
                    HWAStemNoteDerivationOptions *options,
                    HWAStemNoteDerivation **result,
                    char error[HWA_ERROR_SIZE])
{
    HWAInferenceProvider provider;
    test_provider_init(&provider, state);
    return hwa_stem_note_derivation_run(
        &fixture->bundle, fixture->sources, 2U, &provider, options,
        result, error, HWA_ERROR_SIZE);
}

static void test_valid_merge_is_ordered_and_linked(void)
{
    TestFixture fixture;
    TestProviderState state;
    HWAStemNoteDerivationOptions options;
    HWAStemNoteDerivation *result = NULL;
    const HWAInferenceOutput *output;
    const HWAEventBundle *bundle;
    const HWAPerformanceEvent *alpha_note;
    const HWAPerformanceEvent *zeta_note;
    char error[HWA_ERROR_SIZE] = {0};
    memset(&state, 0, sizeof(state));
    if (!test_fixture_init(&fixture)) return;
    hwa_stem_note_derivation_options_default(&options);
    CHECK(test_run(&fixture, &state, &options, &result, error) == 0 &&
              result != NULL,
          "valid stem-note merge failed: %s", error);
    if (result == NULL) {
        test_fixture_free(&fixture);
        return;
    }
    output = hwa_stem_note_derivation_output(result);
    bundle = output == NULL ? NULL : output->bundle;
    CHECK(state.starts == 2U && state.frees == 2U,
          "valid merge used %zu starts and %zu frees",
          state.starts, state.frees);
    CHECK(state.source_ids[0] == UINT64_C(1) &&
              state.source_ids[1] == UINT64_C(2) &&
              strcmp(state.source_names[0], "audio/alpha.wav") == 0 &&
              strcmp(state.source_names[1], "audio/zeta.wav") == 0,
          "stems did not run in stem-ID order");
    CHECK(bundle != NULL && bundle->provider_count == 3U &&
              bundle->audio_count == 3U && bundle->event_count == 4U &&
              bundle->trace_count == 0U && bundle->warning_count == 0U &&
              output->payload_count == 2U,
          "merged bundle has the wrong shape");
    if (bundle == NULL || bundle->provider_count < 3U ||
        bundle->event_count < 4U || output->payload_count < 2U) {
        hwa_stem_note_derivation_free(result);
        test_fixture_free(&fixture);
        return;
    }
    CHECK(bundle->providers[0].id == UINT64_C(1) &&
              bundle->providers[1].id == UINT64_C(2) &&
              bundle->providers[2].id == UINT64_C(3) &&
              strcmp(bundle->providers[1].settings_json,
                     "{\"fixture\":true}") == 0,
          "provider rows were not preserved and remapped");
    CHECK(bundle->audio[1].id == UINT64_C(1) &&
              bundle->audio[2].id == UINT64_C(2) &&
              bundle->events[0].id == UINT64_C(1) &&
              bundle->events[1].id == UINT64_C(2),
          "input audio or event rows changed");
    alpha_note = &bundle->events[2];
    zeta_note = &bundle->events[3];
    CHECK(alpha_note->id == UINT64_C(3) &&
              alpha_note->source_recording_id == UINT64_C(9) &&
              alpha_note->evidence_audio_id_valid &&
              alpha_note->evidence_audio_id == UINT64_C(1) &&
              alpha_note->parent_id_valid &&
              alpha_note->parent_id == UINT64_C(1) &&
              alpha_note->start_sample == UINT64_C(3) &&
              alpha_note->end_sample == UINT64_C(8) &&
              strcmp(alpha_note->voice, "") == 0 &&
              strcmp(alpha_note->part, "") == 0 &&
              strcmp(alpha_note->score_event_id, "") == 0 &&
              alpha_note->values[0].provider_id == UINT64_C(2) &&
              alpha_note->values[0].number == 440.0,
          "alpha note was not linked or remapped");
    CHECK(zeta_note->id == UINT64_C(4) &&
              zeta_note->source_recording_id == UINT64_C(9) &&
              zeta_note->evidence_audio_id_valid &&
              zeta_note->evidence_audio_id == UINT64_C(2) &&
              zeta_note->parent_id_valid &&
              zeta_note->parent_id == UINT64_C(2) &&
              zeta_note->start_sample == UINT64_C(5) &&
              zeta_note->end_sample == UINT64_C(11) &&
              strcmp(zeta_note->voice, "") == 0 &&
              strcmp(zeta_note->part, "") == 0 &&
              strcmp(zeta_note->score_event_id, "") == 0 &&
              zeta_note->values[0].provider_id == UINT64_C(3) &&
              zeta_note->values[0].number == 660.0,
          "zeta note was not linked or remapped");
    CHECK(strcmp(output->payloads[0].relative_path,
                 "audio/alpha.wav") == 0 &&
              output->payloads[0].bytes.context ==
                  fixture.sources[1].bytes.context &&
              strcmp(output->payloads[1].relative_path,
                     "audio/zeta.wav") == 0 &&
              output->payloads[1].bytes.context ==
                  fixture.sources[0].bytes.context,
          "stem payloads were not retained in stem-ID order");
    CHECK(hwa_inference_output_validate(
              output, &options.output_limits,
              error, sizeof(error)) == 0,
          "merged output did not validate: %s", error);
    hwa_stem_note_derivation_free(result);
    test_fixture_free(&fixture);
}

static void test_zero_notes_still_records_each_run(void)
{
    TestFixture fixture;
    TestProviderState state;
    HWAStemNoteDerivationOptions options;
    HWAStemNoteDerivation *result = NULL;
    const HWAInferenceOutput *output;
    char error[HWA_ERROR_SIZE] = {0};
    memset(&state, 0, sizeof(state));
    state.mode = TEST_PROVIDER_ZERO_NOTES;
    if (!test_fixture_init(&fixture)) return;
    hwa_stem_note_derivation_options_default(&options);
    CHECK(test_run(&fixture, &state, &options, &result, error) == 0 &&
              result != NULL,
          "zero-note merge failed: %s", error);
    output = hwa_stem_note_derivation_output(result);
    CHECK(output != NULL && output->bundle->provider_count == 3U &&
              output->bundle->event_count == 2U &&
              state.starts == 2U && state.frees == 2U,
          "zero-note merge did not retain both provider runs");
    hwa_stem_note_derivation_free(result);
    test_fixture_free(&fixture);
}

static void test_global_note_cap_is_all_or_nothing(void)
{
    TestFixture fixture;
    TestProviderState state;
    HWAStemNoteDerivationOptions options;
    HWAStemNoteDerivation *result = NULL;
    char error[HWA_ERROR_SIZE] = {0};
    memset(&state, 0, sizeof(state));
    if (!test_fixture_init(&fixture)) return;
    hwa_stem_note_derivation_options_default(&options);
    options.max_note_events = 1U;
    CHECK(test_run(&fixture, &state, &options, &result, error) != 0 &&
              result == NULL && state.starts == 2U && state.frees == 2U &&
              strstr(error, "limit") != NULL,
          "global note cap did not fail the whole merge: %s", error);
    test_fixture_free(&fixture);
}

static void test_bad_input_binding_stops_before_inference(void)
{
    TestFixture fixture;
    TestProviderState state;
    HWAStemNoteDerivationOptions options;
    HWAInferenceProvider provider;
    HWAStemNoteDerivation *result = NULL;
    char error[HWA_ERROR_SIZE] = {0};
    memset(&state, 0, sizeof(state));
    if (!test_fixture_init(&fixture)) return;
    hwa_stem_note_derivation_options_default(&options);
    test_provider_init(&provider, &state);
    CHECK(hwa_stem_note_derivation_run(
              &fixture.bundle, fixture.sources, 1U, &provider,
              &options, &result, error, sizeof(error)) != 0 &&
              result == NULL && state.starts == 0U && state.frees == 0U,
          "missing stem binding reached note inference: %s", error);
    error[0] = '\0';
    fixture.stem_waves[0].bytes.data[44] ^= UINT8_C(1);
    CHECK(hwa_stem_note_derivation_run(
              &fixture.bundle, fixture.sources, 2U, &provider,
              &options, &result, error, sizeof(error)) != 0 &&
              result == NULL && state.starts == 0U && state.frees == 0U,
          "changed stem payload reached note inference: %s", error);
    test_fixture_free(&fixture);
}

static void test_non_note_provider_result_is_rejected(void)
{
    TestFixture fixture;
    TestProviderState state;
    HWAStemNoteDerivationOptions options;
    HWAStemNoteDerivation *result = NULL;
    char error[HWA_ERROR_SIZE] = {0};
    memset(&state, 0, sizeof(state));
    state.mode = TEST_PROVIDER_NON_NOTE;
    if (!test_fixture_init(&fixture)) return;
    hwa_stem_note_derivation_options_default(&options);
    CHECK(test_run(&fixture, &state, &options, &result, error) != 0 &&
              result == NULL && state.starts == 1U && state.frees == 1U &&
              strstr(error, "non-note") != NULL,
          "merge accepted a non-note provider event: %s", error);
    test_fixture_free(&fixture);
}

static void test_noncanonical_stem_bundle_stops_before_inference(void)
{
    TestFixture fixture;
    TestProviderState state;
    HWAStemNoteDerivationOptions options;
    HWAStemNoteDerivation *result = NULL;
    char error[HWA_ERROR_SIZE] = {0};
    memset(&state, 0, sizeof(state));
    if (!test_fixture_init(&fixture)) return;
    fixture.provider.id = UINT64_C(4);
    fixture.region_values[0].provider_id = UINT64_C(4);
    fixture.region_values[1].provider_id = UINT64_C(4);
    hwa_stem_note_derivation_options_default(&options);
    CHECK(test_run(&fixture, &state, &options, &result, error) != 0 &&
              result == NULL && state.starts == 0U && state.frees == 0U &&
              strstr(error, "canonical instrument-stems-v1") != NULL,
          "noncanonical stem bundle reached note inference: %s", error);
    test_fixture_free(&fixture);
}

static void test_default_options_are_bounded(void)
{
    HWAStemNoteDerivationOptions options;
    memset(&options, 0xff, sizeof(options));
    hwa_stem_note_derivation_options_default(&options);
    CHECK(strcmp(options.note_task,
                 "org.hlolli.note-events-on-audio-v1") == 0 &&
              strcmp(options.note_settings_json, "{}") == 0 &&
              options.max_input_file_bytes != 0U &&
              options.max_input_bytes >= options.max_input_file_bytes &&
              options.timeout_milliseconds != 0U &&
              options.max_note_events != 0U &&
              options.output_limits.max_events != 0U,
          "stem-note defaults are not complete and bounded");
}

int main(void)
{
    test_default_options_are_bounded();
    test_valid_merge_is_ordered_and_linked();
    test_zero_notes_still_records_each_run();
    test_global_note_cap_is_all_or_nothing();
    test_bad_input_binding_stops_before_inference();
    test_non_note_provider_result_is_rejected();
    test_noncanonical_stem_bundle_stops_before_inference();
    if (failures != 0) {
        (void)fprintf(stderr, "%d stem-note derivation test(s) failed\n",
                      failures);
        return 1;
    }
    (void)puts("Stem-note derivation tests passed");
    return 0;
}

#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "instrument_stem_provider.h"
#include "inference_clock.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#define TEST_PID _getpid
#define TEST_RMDIR _rmdir
#define TEST_UNLINK _unlink
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define TEST_PID getpid
#define TEST_RMDIR rmdir
#define TEST_UNLINK unlink
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TEST_SOURCE_FRAMES UINT64_C(96)
#define TEST_SAMPLE_RATE UINT32_C(8000)
#define TEST_MAX_WORK_BYTES (UINT64_C(1024) * UINT64_C(1024))

static const char test_model_sha256[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char test_adapter_sha256[] =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

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
    unsigned char *data;
    size_t size;
    HWAFormat format;
} TestWave;

typedef enum TestRunnerMode {
    TEST_RUNNER_VALID = 0,
    TEST_RUNNER_ZERO_STEMS,
    TEST_RUNNER_NULL_STEMS,
    TEST_RUNNER_FAILURE,
    TEST_RUNNER_BAD_STEM_ID,
    TEST_RUNNER_EMPTY_INSTRUMENT,
    TEST_RUNNER_BAD_SCORE,
    TEST_RUNNER_BAD_SCORE_FLAG,
    TEST_RUNNER_SCORE_WITHOUT_FLAG,
    TEST_RUNNER_BAD_CLOCK,
    TEST_RUNNER_MALFORMED_WAVE,
    TEST_RUNNER_DUPLICATE_ID,
    TEST_RUNNER_DUPLICATE_INSTRUMENT
} TestRunnerMode;

typedef struct TestOwnedStem {
    TestBytes bytes;
} TestOwnedStem;

typedef struct TestRunnerState {
    TestRunnerMode mode;
    uint64_t expected_frames;
    uint32_t expected_sample_rate;
    uint64_t expected_timeout_milliseconds;
    size_t calls;
    size_t results_destroy_calls;
    size_t context_destroy_calls;
    uint64_t runner_delay_milliseconds;
    unsigned char *live_first_wave;
    size_t live_first_wave_size;
} TestRunnerState;

typedef struct TestCase {
    TestWave source_wave;
    TestBytes source_bytes;
    TestRunnerState runner_state;
    HWAInferenceInput input;
    HWAInferenceRequest request;
    HWAInferenceProvider provider;
    char source_sha256[HWA_SHA256_HEX_SIZE];
    void *task;
    int provider_open;
} TestCase;

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

static int test_wave_build(TestWave *wave,
                           uint64_t frames,
                           uint32_t sample_rate,
                           uint16_t channels,
                           int16_t bias)
{
    uint64_t data_bytes;
    uint64_t file_bytes;
    uint32_t block_align;
    uint64_t frame;
    if (wave == NULL || sample_rate == 0U || channels == 0U ||
        frames > UINT64_MAX / (uint64_t)channels / 2U)
        return -1;
    memset(wave, 0, sizeof(*wave));
    data_bytes = frames * (uint64_t)channels * 2U;
    file_bytes = data_bytes + 44U;
    block_align = (uint32_t)channels * 2U;
    if (data_bytes > UINT32_MAX - 36U || file_bytes > (uint64_t)SIZE_MAX ||
        block_align > UINT16_MAX || sample_rate > UINT32_MAX / block_align)
        return -1;
    wave->data = (unsigned char *)calloc((size_t)file_bytes, 1U);
    if (wave->data == NULL) return -1;
    wave->size = (size_t)file_bytes;
    memcpy(wave->data, "RIFF", 4U);
    test_le32(wave->data + 4U, (uint32_t)data_bytes + 36U);
    memcpy(wave->data + 8U, "WAVEfmt ", 8U);
    test_le32(wave->data + 16U, UINT32_C(16));
    test_le16(wave->data + 20U, UINT16_C(1));
    test_le16(wave->data + 22U, channels);
    test_le32(wave->data + 24U, sample_rate);
    test_le32(wave->data + 28U, sample_rate * block_align);
    test_le16(wave->data + 32U, (uint16_t)block_align);
    test_le16(wave->data + 34U, UINT16_C(16));
    memcpy(wave->data + 36U, "data", 4U);
    test_le32(wave->data + 40U, (uint32_t)data_bytes);
    for (frame = 0U; frame < frames; ++frame) {
        uint16_t channel;
        int32_t sample = (int32_t)(frame % UINT64_C(101)) - 50 + bias;
        uint16_t bits = sample < 0
                            ? (uint16_t)(UINT32_C(65536) +
                                         (uint32_t)sample)
                            : (uint16_t)sample;
        for (channel = 0U; channel < channels; ++channel) {
            uint64_t sample_index =
                frame * (uint64_t)channels + (uint64_t)channel;
            test_le16(wave->data + 44U + (size_t)sample_index * 2U, bits);
        }
    }
    wave->format.container = HWA_CONTAINER_RIFF;
    wave->format.encoding = HWA_ENCODING_PCM;
    wave->format.channels = channels;
    wave->format.sample_rate_hz = sample_rate;
    wave->format.bits_per_sample = 16U;
    wave->format.valid_bits_per_sample = 16U;
    wave->format.block_align = (uint16_t)block_align;
    wave->format.frames = frames;
    wave->format.data_bytes = data_bytes;
    wave->format.duration_seconds =
        (double)frames / (double)sample_rate;
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

static void test_error(char *error,
                       size_t error_size,
                       const char *message)
{
    if (error == NULL || error_size == 0U) return;
    (void)snprintf(error, error_size, "%s", message);
    error[error_size - 1U] = '\0';
}

static void test_results_destroy(void *context_value,
                                 HWAInstrumentStemResult *stems,
                                 size_t stem_count)
{
    TestRunnerState *state = (TestRunnerState *)context_value;
    size_t index;
    if (state != NULL) {
        state->results_destroy_calls++;
        state->live_first_wave = NULL;
        state->live_first_wave_size = 0U;
    }
    for (index = 0U; index < stem_count; ++index) {
        TestOwnedStem *owned =
            (TestOwnedStem *)stems[index].wave.context;
        if (owned != NULL) {
            free(owned->bytes.data);
            free(owned);
        }
    }
    free(stems);
}

static void test_runner_destroy(void *context_value)
{
    TestRunnerState *state = (TestRunnerState *)context_value;
    if (state != NULL) state->context_destroy_calls++;
}

static int test_result_wave_init(HWAInstrumentStemResult *stem,
                                 uint64_t frames,
                                 uint32_t sample_rate,
                                 uint16_t channels,
                                 int16_t bias)
{
    TestOwnedStem *owned;
    TestWave wave;
    if (stem == NULL) return -1;
    owned = (TestOwnedStem *)calloc(1U, sizeof(*owned));
    if (owned == NULL) return -1;
    if (test_wave_build(&wave, frames, sample_rate, channels, bias) != 0) {
        free(owned);
        return -1;
    }
    owned->bytes.data = wave.data;
    owned->bytes.size = wave.size;
    stem->wave.context = owned;
    stem->wave.name = "runner-stem.wav";
    stem->wave.size = (uint64_t)wave.size;
    stem->wave.read_at = test_read_at;
    return 0;
}

static int test_run(void *context_value,
                    const HWAByteSource *source,
                    const HWAFormat *source_format,
                    uint64_t seed,
                    uint64_t timeout_milliseconds,
                    HWAInstrumentStemResult **stems,
                    size_t *stem_count,
                    char *error,
                    size_t error_size)
{
    TestRunnerState *state = (TestRunnerState *)context_value;
    HWAInstrumentStemResult *rows;
    size_t count = 2U;
    if (stems != NULL) *stems = NULL;
    if (stem_count != NULL) *stem_count = 0U;
    if (state == NULL || source == NULL || source_format == NULL ||
        stems == NULL || stem_count == NULL || source->read_at == NULL ||
        source_format->frames != state->expected_frames ||
        source_format->sample_rate_hz != state->expected_sample_rate ||
        seed != UINT64_C(42) ||
        timeout_milliseconds != state->expected_timeout_milliseconds) {
        test_error(error, error_size, "bad fake stem runner call");
        return -1;
    }
    state->calls++;
    if (state->mode == TEST_RUNNER_FAILURE) {
        test_error(error, error_size, "fake separator failed");
        return -1;
    }
    if (state->mode == TEST_RUNNER_ZERO_STEMS) return 0;
    if (state->mode == TEST_RUNNER_NULL_STEMS) {
        *stem_count = 2U;
        return 0;
    }
    if (state->mode != TEST_RUNNER_VALID &&
        state->mode != TEST_RUNNER_DUPLICATE_INSTRUMENT)
        count = state->mode == TEST_RUNNER_DUPLICATE_ID ? 2U : 1U;
    rows = (HWAInstrumentStemResult *)calloc(count, sizeof(*rows));
    if (rows == NULL) {
        test_error(error, error_size, "cannot allocate fake stems");
        return -1;
    }
    rows[0].stem_id = "cello-main";
    rows[0].instrument = "violoncello";
    rows[0].score = 0.75;
    rows[0].score_valid = 1;
    if (state->mode == TEST_RUNNER_BAD_STEM_ID)
        rows[0].stem_id = "Cello/Main";
    if (state->mode == TEST_RUNNER_EMPTY_INSTRUMENT)
        rows[0].instrument = "";
    if (state->mode == TEST_RUNNER_BAD_SCORE) rows[0].score = 1.25;
    if (state->mode == TEST_RUNNER_BAD_SCORE_FLAG)
        rows[0].score_valid = 2;
    if (state->mode == TEST_RUNNER_SCORE_WITHOUT_FLAG)
        rows[0].score_valid = 0;
    if (test_result_wave_init(
            &rows[0], state->expected_frames,
            state->mode == TEST_RUNNER_BAD_CLOCK
                ? state->expected_sample_rate + 1U
                : state->expected_sample_rate,
            1U, INT16_C(200)) != 0) {
        test_results_destroy(state, rows, count);
        test_error(error, error_size, "cannot allocate first fake stem");
        return -1;
    }
    if (state->mode == TEST_RUNNER_MALFORMED_WAVE) {
        TestOwnedStem *owned = (TestOwnedStem *)rows[0].wave.context;
        owned->bytes.data[0] = (unsigned char)'X';
    }
    if (count == 2U) {
        rows[1].stem_id = state->mode == TEST_RUNNER_DUPLICATE_ID
                              ? "cello-main"
                              : "bassoon_main";
        rows[1].instrument = state->mode == TEST_RUNNER_DUPLICATE_INSTRUMENT
                                 ? "violoncello"
                                 : "bassoon";
        rows[1].score = 0.0;
        rows[1].score_valid = 0;
        if (test_result_wave_init(
                &rows[1], state->expected_frames,
                state->expected_sample_rate, 2U, INT16_C(-200)) != 0) {
            test_results_destroy(state, rows, count);
            test_error(error, error_size, "cannot allocate second fake stem");
            return -1;
        }
    }
    state->live_first_wave =
        ((TestOwnedStem *)rows[0].wave.context)->bytes.data;
    state->live_first_wave_size =
        ((TestOwnedStem *)rows[0].wave.context)->bytes.size;
    *stems = rows;
    *stem_count = count;
    if (state->runner_delay_milliseconds != 0U) {
        uint64_t started = 0U;
        char ignored[HWA_ERROR_SIZE] = {0};
        if (hwa_inference_deadline_start(
                &started, error, error_size) != 0)
            return -1;
        while (hwa_inference_deadline_check(
                   started, state->runner_delay_milliseconds,
                   ignored, sizeof(ignored)) == 0) {
        }
    }
    return 0;
}

static void test_case_cleanup(TestCase *test)
{
    if (test->task != NULL && test->provider.task_free != NULL) {
        test->provider.task_free(test->provider.context, test->task);
        test->task = NULL;
    }
    if (test->provider_open) {
        hwa_inference_provider_destroy(&test->provider);
        test->provider_open = 0;
    }
    free(test->source_wave.data);
    test->source_wave.data = NULL;
}

static int test_case_init(TestCase *test,
                          uint64_t frames,
                          uint64_t max_work_bytes)
{
    HWAInstrumentStemRunner runner;
    char error[HWA_ERROR_SIZE] = {0};
    memset(test, 0, sizeof(*test));
    memset(&runner, 0, sizeof(runner));
    if (test_wave_build(&test->source_wave, frames, TEST_SAMPLE_RATE,
                        1U, INT16_C(0)) != 0) {
        CHECK(0, "cannot build source WAVE fixture");
        return 0;
    }
    test->source_bytes.data = test->source_wave.data;
    test->source_bytes.size = test->source_wave.size;
    test->input.id = "source";
    test->input.role = "source-recording";
    test->input.media_type = "audio/wav";
    test->input.sha256 = test->source_sha256;
    test->input.bytes.context = &test->source_bytes;
    test->input.bytes.name = "memory.wav";
    test->input.bytes.size = (uint64_t)test->source_wave.size;
    test->input.bytes.read_at = test_read_at;
    if (hwa_inference_byte_source_sha256(
            &test->input.bytes, (uint64_t)test->source_wave.size,
            test->source_sha256, error, sizeof(error)) != 0) {
        CHECK(0, "cannot hash source WAVE fixture: %s", error);
        test_case_cleanup(test);
        return 0;
    }
    test->runner_state.expected_frames = frames;
    test->runner_state.expected_sample_rate = TEST_SAMPLE_RATE;
    test->runner_state.expected_timeout_milliseconds = UINT64_C(900);
    runner.context = &test->runner_state;
    runner.runtime_name = "fake-runtime";
    runner.runtime_version = "1.2.3";
    runner.backend = "cpu";
    runner.fallback = "";
    runner.run = test_run;
    runner.results_destroy = test_results_destroy;
    runner.destroy = test_runner_destroy;
    if (hwa_instrument_stem_provider_init(
            &test->provider, test_model_sha256, test_adapter_sha256,
            max_work_bytes, &runner, error, sizeof(error)) != 0) {
        CHECK(0, "cannot initialize stem provider: %s", error);
        test_case_cleanup(test);
        return 0;
    }
    test->provider_open = 1;
    memset(&test->request, 0, sizeof(test->request));
    hwa_event_bundle_limits_default(&test->request.output_limits);
    test->request.output_limits.max_audio_files = 8U;
    test->request.output_limits.max_events = 8U;
    test->request.output_limits.max_values = 8U;
    test->request.output_limits.max_work_bytes = TEST_MAX_WORK_BYTES;
    test->request.task = HWA_INSTRUMENT_STEM_TASK_NAME;
    test->request.settings_json = "{}";
    test->request.expected_provider_name =
        HWA_INSTRUMENT_STEM_PROVIDER_NAME;
    test->request.expected_provider_version =
        HWA_INSTRUMENT_STEM_PROVIDER_VERSION;
    test->request.expected_model_sha256 = test_model_sha256;
    test->request.seed = UINT64_C(42);
    test->request.source_recording_id = UINT64_C(7);
    test->request.source_input_id = test->input.id;
    test->request.inputs = &test->input;
    test->request.input_count = 1U;
    test->request.source_format = test->source_wave.format;
    test->request.max_input_file_bytes =
        (uint64_t)test->source_wave.size;
    test->request.max_input_bytes = (uint64_t)test->source_wave.size;
    test->request.timeout_milliseconds = UINT64_C(900);
    return 1;
}

static int test_start_and_poll(TestCase *test,
                               const HWAInferenceOutput **output,
                               char error[HWA_ERROR_SIZE])
{
    HWAInferencePollState state = HWA_INFERENCE_PENDING;
    if (test->provider.start(
            test->provider.context, &test->request, &test->task,
            error, HWA_ERROR_SIZE) != 0)
        return -1;
    if (test->provider.poll(
            test->provider.context, test->task, &state, output,
            error, HWA_ERROR_SIZE) != 0)
        return -1;
    if (state != HWA_INFERENCE_READY || output == NULL || *output == NULL) {
        test_error(error, HWA_ERROR_SIZE,
                   "synchronous stem provider was not ready");
        return -1;
    }
    return 0;
}

static int test_make_unused_path(char path[PATH_MAX])
{
    unsigned attempt;
#if defined(_WIN32)
    const char *temporary = getenv("TEMP");
    if (temporary == NULL || temporary[0] == '\0') temporary = ".";
#else
    const char *temporary = "/tmp";
#endif
    for (attempt = 0U; attempt < 100U; ++attempt) {
        int written = snprintf(path, PATH_MAX,
                               "%s/hwa-instrument-stems-%ld-%u",
                               temporary, (long)TEST_PID(), attempt);
        if (written < 0 || (size_t)written >= PATH_MAX) return 0;
#if defined(_WIN32)
        if (_mkdir(path) == 0) {
#else
        if (mkdir(path, 0700) == 0) {
#endif
            if (TEST_RMDIR(path) != 0) return 0;
            return 1;
        }
        if (errno != EEXIST) return 0;
    }
    return 0;
}

static int test_join_path(char path[PATH_MAX],
                          const char *directory,
                          const char *relative_path)
{
    int written = snprintf(path, PATH_MAX, "%s/%s", directory,
                           relative_path);
    return written >= 0 && (size_t)written < PATH_MAX;
}

static char *test_replace_once(const char *text,
                               const char *needle,
                               const char *replacement)
{
    const char *found;
    size_t text_size;
    size_t needle_size;
    size_t replacement_size;
    size_t prefix_size;
    size_t result_size;
    char *result;
    if (text == NULL || needle == NULL || replacement == NULL)
        return NULL;
    found = strstr(text, needle);
    if (found == NULL) return NULL;
    text_size = strlen(text);
    needle_size = strlen(needle);
    replacement_size = strlen(replacement);
    prefix_size = (size_t)(found - text);
    if (replacement_size > SIZE_MAX - (text_size - needle_size) - 1U)
        return NULL;
    result_size = text_size - needle_size + replacement_size;
    result = (char *)malloc(result_size + 1U);
    if (result == NULL) return NULL;
    memcpy(result, text, prefix_size);
    memcpy(result + prefix_size, replacement, replacement_size);
    memcpy(result + prefix_size + replacement_size,
           found + needle_size,
           text_size - prefix_size - needle_size + 1U);
    return result;
}

static void test_reject_provenance_change(
    const HWAEventBundle *bundle,
    const char *needle,
    const char *replacement,
    const char *description)
{
    HWAEventBundle changed;
    HWAEventProvider provider;
    char *settings;
    char error[HWA_ERROR_SIZE] = {0};
    if (bundle == NULL || bundle->provider_count != 1U) {
        CHECK(0, "cannot test %s without a stem provider", description);
        return;
    }
    settings = test_replace_once(
        bundle->providers[0].settings_json, needle, replacement);
    CHECK(settings != NULL, "cannot build %s provenance", description);
    if (settings == NULL) return;
    changed = *bundle;
    provider = bundle->providers[0];
    provider.settings_json = settings;
    changed.providers = &provider;
    CHECK(hwa_instrument_stem_bundle_validate_v1(
              &changed, error, sizeof(error)) != 0 &&
              strstr(error, "canonical") != NULL,
          "validator accepted %s provenance: %s", description, error);
    free(settings);
}

static void test_remove_bundle(const char *directory)
{
    char path[PATH_MAX];
    if (test_join_path(path, directory, "audio/bassoon_main.wav"))
        (void)TEST_UNLINK(path);
    if (test_join_path(path, directory, "audio/cello-main.wav"))
        (void)TEST_UNLINK(path);
    if (test_join_path(path, directory, "events.jsonl"))
        (void)TEST_UNLINK(path);
    if (test_join_path(path, directory, "traces.jsonl"))
        (void)TEST_UNLINK(path);
    if (test_join_path(path, directory, "manifest.json"))
        (void)TEST_UNLINK(path);
    if (test_join_path(path, directory, "audio")) (void)TEST_RMDIR(path);
    (void)TEST_RMDIR(directory);
}

static void test_valid_two_stems_and_round_trip(void)
{
    TestCase test;
    const HWAInferenceOutput *output = NULL;
    const HWAEventBundle *bundle;
    const HWAEventAudio *bassoon;
    const HWAEventAudio *cello;
    const HWAPerformanceEvent *bassoon_event;
    const HWAPerformanceEvent *cello_event;
    HWAEventBundle loaded;
    char bassoon_sha256[HWA_SHA256_HEX_SIZE];
    char cello_sha256[HWA_SHA256_HEX_SIZE];
    char expected_provenance[4096];
    char output_path[PATH_MAX] = {0};
    char error[HWA_ERROR_SIZE] = {0};
    int written;
    int write_result = -1;
    int read_result = -1;
    memset(&loaded, 0, sizeof(loaded));
    if (!test_case_init(
            &test, TEST_SOURCE_FRAMES, TEST_MAX_WORK_BYTES))
        return;
    CHECK(test_start_and_poll(&test, &output, error) == 0,
          "valid stem request failed: %s", error);
    if (output == NULL || output->bundle == NULL) {
        test_case_cleanup(&test);
        return;
    }
    bundle = output->bundle;
    CHECK(test.runner_state.calls == 1U,
          "valid request called runner %zu times", test.runner_state.calls);
    CHECK(bundle->provider_count == 1U && bundle->audio_count == 3U &&
              bundle->event_count == 2U && bundle->trace_count == 0U &&
              output->payload_count == 2U,
          "valid stem bundle has the wrong row counts");
    CHECK(strcmp(bundle->providers[0].name,
                 HWA_INSTRUMENT_STEM_PROVIDER_NAME) == 0 &&
              strcmp(bundle->providers[0].version,
                     HWA_INSTRUMENT_STEM_PROVIDER_VERSION) == 0 &&
              strcmp(bundle->providers[0].model_sha256,
                     test_model_sha256) == 0,
          "stem provider row has the wrong identity");
    written = snprintf(
        expected_provenance, sizeof(expected_provenance),
        "{\"task\":\"%s\",\"seed\":\"00000000000000000042\","
        "\"inputs\":[{\"id\":\"source\",\"role\":\"source-recording\","
        "\"media_type\":\"audio/wav\",\"name\":\"memory.wav\","
        "\"bytes\":\"%020llu\",\"sha256\":\"%s\"}],"
        "\"runtime\":{\"name\":\"fake-runtime\",\"version\":\"1.2.3\","
        "\"backend\":\"cpu\",\"fallback\":\"\","
        "\"adapter_sha256\":\"%s\"},\"task_settings\":{}}",
        HWA_INSTRUMENT_STEM_TASK_NAME,
        (unsigned long long)test.source_wave.size,
        test.source_sha256, test_adapter_sha256);
    CHECK(written > 0 && (size_t)written < sizeof(expected_provenance),
          "expected provenance text overflowed");
    if (written > 0 && (size_t)written < sizeof(expected_provenance)) {
        CHECK(strcmp(bundle->providers[0].settings_json,
                     expected_provenance) == 0,
              "stem provenance is not canonical: %s",
              bundle->providers[0].settings_json);
    }
    CHECK(bundle->audio[0].id == UINT64_C(7) &&
              bundle->audio[0].kind == HWA_EVENT_SOURCE_RECORDING &&
              strcmp(bundle->audio[0].name, "memory.wav") == 0 &&
              strcmp(bundle->audio[0].relative_path, "") == 0 &&
              strcmp(bundle->audio[0].sha256, test.source_sha256) == 0 &&
              bundle->audio[0].file_bytes ==
                  (uint64_t)test.source_wave.size,
          "source audio row does not match its request");
    bassoon = &bundle->audio[1];
    cello = &bundle->audio[2];
    CHECK(bassoon->id == UINT64_C(1) &&
              bassoon->kind == HWA_EVENT_INSTRUMENT_STEM &&
              strcmp(bassoon->name, "bassoon_main") == 0 &&
              strcmp(bassoon->relative_path,
                     "audio/bassoon_main.wav") == 0 &&
              bassoon->source_recording_id_valid &&
              bassoon->source_recording_id == UINT64_C(7) &&
              bassoon->format.frames == TEST_SOURCE_FRAMES &&
              bassoon->format.sample_rate_hz == TEST_SAMPLE_RATE &&
              bassoon->format.channels == 2U,
          "sorted bassoon audio row is wrong");
    CHECK(cello->id == UINT64_C(2) &&
              cello->kind == HWA_EVENT_INSTRUMENT_STEM &&
              strcmp(cello->name, "cello-main") == 0 &&
              strcmp(cello->relative_path,
                     "audio/cello-main.wav") == 0 &&
              cello->source_recording_id_valid &&
              cello->source_recording_id == UINT64_C(7) &&
              cello->format.frames == TEST_SOURCE_FRAMES &&
              cello->format.sample_rate_hz == TEST_SAMPLE_RATE &&
              cello->format.channels == 1U,
          "sorted cello audio row is wrong");
    CHECK(hwa_inference_byte_source_sha256(
              &output->payloads[0].bytes,
              output->payloads[0].bytes.size,
              bassoon_sha256, error, sizeof(error)) == 0 &&
              strcmp(output->payloads[0].relative_path,
                     "audio/bassoon_main.wav") == 0 &&
              strcmp(bassoon->sha256, bassoon_sha256) == 0 &&
              bassoon->file_bytes == output->payloads[0].bytes.size,
          "bassoon payload binding or hash is wrong: %s", error);
    CHECK(hwa_inference_byte_source_sha256(
              &output->payloads[1].bytes,
              output->payloads[1].bytes.size,
              cello_sha256, error, sizeof(error)) == 0 &&
              strcmp(output->payloads[1].relative_path,
                     "audio/cello-main.wav") == 0 &&
              strcmp(cello->sha256, cello_sha256) == 0 &&
              cello->file_bytes == output->payloads[1].bytes.size,
          "cello payload binding or hash is wrong: %s", error);
    bassoon_event = &bundle->events[0];
    cello_event = &bundle->events[1];
    CHECK(bassoon_event->id == UINT64_C(1) &&
              strcmp(bassoon_event->kind, "instrument-region") == 0 &&
              bassoon_event->source_recording_id == UINT64_C(7) &&
              bassoon_event->evidence_audio_id_valid &&
              bassoon_event->evidence_audio_id == bassoon->id &&
              bassoon_event->start_sample == 0U &&
              bassoon_event->end_sample == TEST_SOURCE_FRAMES &&
              strcmp(bassoon_event->part, "bassoon_main") == 0 &&
              bassoon_event->value_count == 1U,
          "bassoon event fields are wrong");
    CHECK(strcmp(bassoon_event->values[0].name, "instrument") == 0 &&
              bassoon_event->values[0].kind == HWA_EVENT_VALUE_TEXT &&
              bassoon_event->values[0].basis == HWA_EVENT_INFERENCE &&
              strcmp(bassoon_event->values[0].text, "bassoon") == 0 &&
              !bassoon_event->values[0].score_valid &&
              bassoon_event->values[0].provider_id_valid &&
              bassoon_event->values[0].provider_id == UINT64_C(1) &&
              bassoon_event->values[0].selected,
          "bassoon instrument value is wrong");
    CHECK(cello_event->id == UINT64_C(2) &&
              strcmp(cello_event->kind, "instrument-region") == 0 &&
              cello_event->source_recording_id == UINT64_C(7) &&
              cello_event->evidence_audio_id_valid &&
              cello_event->evidence_audio_id == cello->id &&
              cello_event->start_sample == 0U &&
              cello_event->end_sample == TEST_SOURCE_FRAMES &&
              strcmp(cello_event->part, "cello-main") == 0 &&
              cello_event->value_count == 1U,
          "cello event fields are wrong");
    CHECK(strcmp(cello_event->values[0].name, "instrument") == 0 &&
              cello_event->values[0].kind == HWA_EVENT_VALUE_TEXT &&
              cello_event->values[0].basis == HWA_EVENT_INFERENCE &&
              strcmp(cello_event->values[0].text, "violoncello") == 0 &&
              cello_event->values[0].score_valid &&
              fabs(cello_event->values[0].score - 0.75) < 1e-12 &&
              cello_event->values[0].provider_id_valid &&
              cello_event->values[0].provider_id == UINT64_C(1) &&
              cello_event->values[0].selected,
          "cello instrument value is wrong");
    CHECK(hwa_inference_output_validate_for_request(
              &test.provider, &test.request, output,
              error, sizeof(error)) == 0,
          "valid stem output did not validate: %s", error);
    CHECK(test_make_unused_path(output_path),
          "cannot reserve stem round-trip path");
    if (output_path[0] != '\0') {
        error[0] = '\0';
        write_result = hwa_inference_output_write(
            output_path, output, &test.request.output_limits,
            error, sizeof(error));
        CHECK(write_result == 0,
              "cannot write stem output: %s", error);
    }
    if (write_result == 0) {
        error[0] = '\0';
        read_result = hwa_event_bundle_read(
            output_path, &test.request.output_limits, &loaded,
            error, sizeof(error));
        CHECK(read_result == 0,
              "cannot read written stem output: %s", error);
    }
    if (read_result == 0) {
        CHECK(loaded.audio_count == 3U && loaded.event_count == 2U &&
                  strcmp(loaded.audio[1].sha256, bassoon_sha256) == 0 &&
                  strcmp(loaded.audio[2].sha256, cello_sha256) == 0 &&
                  strcmp(loaded.events[0].values[0].text, "bassoon") == 0 &&
                  strcmp(loaded.events[1].values[0].text,
                         "violoncello") == 0,
              "stem bundle changed during write/read");
    }
    hwa_event_bundle_free(&loaded);
    if (output_path[0] != '\0') test_remove_bundle(output_path);
    CHECK(test.runner_state.results_destroy_calls == 0U,
          "provider freed runner results before task_free");
    test_case_cleanup(&test);
    CHECK(test.runner_state.results_destroy_calls == 1U &&
              test.runner_state.context_destroy_calls == 1U,
          "valid result ownership cleanup was wrong");
}

static void test_saved_provenance_shape_and_source_match(void)
{
    TestCase test;
    const HWAInferenceOutput *output = NULL;
    const HWAEventBundle *bundle;
    char duplicate_input[1024];
    char error[HWA_ERROR_SIZE] = {0};
    int written;
    if (!test_case_init(
            &test, TEST_SOURCE_FRAMES, TEST_MAX_WORK_BYTES))
        return;
    CHECK(test_start_and_poll(&test, &output, error) == 0 &&
              output != NULL && output->bundle != NULL,
          "cannot build provenance test output: %s", error);
    if (output == NULL || output->bundle == NULL) {
        test_case_cleanup(&test);
        return;
    }
    bundle = output->bundle;
    CHECK(hwa_instrument_stem_bundle_validate_v1(
              bundle, error, sizeof(error)) == 0,
          "validator rejected canonical provenance: %s", error);

    test_reject_provenance_change(
        bundle,
        ",\"seed\":\"00000000000000000042\"", "",
        "missing top-level key");
    test_reject_provenance_change(
        bundle,
        ",\"seed\":\"00000000000000000042\"",
        ",\"seed\":\"00000000000000000042\""
        ",\"seed\":\"00000000000000000042\"",
        "duplicate top-level key");
    test_reject_provenance_change(
        bundle, ",\"seed\":",
        ",\"extra\":true,\"seed\":", "extra top-level key");
    test_reject_provenance_change(
        bundle, "\"00000000000000000042\"", "\"42\"",
        "noncanonical seed");

    test_reject_provenance_change(
        bundle, ",\"role\":\"source-recording\"", "",
        "missing input key");
    test_reject_provenance_change(
        bundle, ",\"role\":\"source-recording\"",
        ",\"role\":\"source-recording\""
        ",\"role\":\"source-recording\"",
        "duplicate input key");
    test_reject_provenance_change(
        bundle, ",\"role\":",
        ",\"extra\":true,\"role\":", "extra input key");
    test_reject_provenance_change(
        bundle, "\"id\":\"source\"", "\"id\":\"bad id\"",
        "invalid input ID");

    written = snprintf(
        duplicate_input, sizeof(duplicate_input),
        "},{\"id\":\"second\",\"role\":\"source-recording\","
        "\"media_type\":\"audio/wav\",\"name\":\"memory.wav\","
        "\"bytes\":\"%020llu\",\"sha256\":\"%s\"}],"
        "\"runtime\"",
        (unsigned long long)test.source_wave.size, test.source_sha256);
    CHECK(written > 0 && (size_t)written < sizeof(duplicate_input),
          "cannot build extra input provenance");
    if (written > 0 && (size_t)written < sizeof(duplicate_input)) {
        test_reject_provenance_change(
            bundle, "}],\"runtime\"", duplicate_input,
            "extra provenance input");
    }

    test_reject_provenance_change(
        bundle, "\"name\":\"memory.wav\"",
        "\"name\":\"other.wav\"", "mismatched input name");
    test_reject_provenance_change(
        bundle, "\"bytes\":\"00000000000000000236\"",
        "\"bytes\":\"00000000000000000237\"",
        "mismatched input byte count");
    test_reject_provenance_change(
        bundle, test.source_sha256,
        "00000000000000000000000000000000"
        "00000000000000000000000000000000",
        "mismatched input hash");

    test_reject_provenance_change(
        bundle, ",\"version\":\"1.2.3\"", "",
        "missing runtime key");
    test_reject_provenance_change(
        bundle, ",\"backend\":\"cpu\"",
        ",\"backend\":\"cpu\",\"backend\":\"cpu\"",
        "duplicate runtime key");
    test_reject_provenance_change(
        bundle, ",\"fallback\":",
        ",\"extra\":true,\"fallback\":", "extra runtime key");
    test_reject_provenance_change(
        bundle, "\"backend\":\"cpu\"",
        "\"backend\":\"\\u0001\"", "runtime control byte");
    test_reject_provenance_change(
        bundle, "\"task_settings\":{}",
        "\"task_settings\":{\"extra\":true}",
        "nonempty task settings");
    test_case_cleanup(&test);
}

static void test_saved_provenance_canonical_string_escape(void)
{
    TestCase test;
    const HWAInferenceOutput *output = NULL;
    char error[HWA_ERROR_SIZE] = {0};
    if (!test_case_init(
            &test, TEST_SOURCE_FRAMES, TEST_MAX_WORK_BYTES))
        return;
    test.input.bytes.name = "mem\"ory\\.wav";
    CHECK(test_start_and_poll(&test, &output, error) == 0 &&
              output != NULL && output->bundle != NULL,
          "escaped source name failed: %s", error);
    if (output != NULL && output->bundle != NULL) {
        CHECK(strstr(output->bundle->providers[0].settings_json,
                     "mem\\\"ory\\\\.wav") != NULL,
              "source name did not use canonical provenance escapes: %s",
              output->bundle->providers[0].settings_json);
        CHECK(hwa_instrument_stem_bundle_validate_v1(
                  output->bundle, error, sizeof(error)) == 0,
              "validator rejected canonical escaped source name: %s",
              error);
    }
    test_case_cleanup(&test);
}

static void test_duplicate_instrument_labels_are_valid(void)
{
    TestCase test;
    const HWAInferenceOutput *output = NULL;
    char error[HWA_ERROR_SIZE] = {0};
    if (!test_case_init(
            &test, TEST_SOURCE_FRAMES, TEST_MAX_WORK_BYTES))
        return;
    test.runner_state.mode = TEST_RUNNER_DUPLICATE_INSTRUMENT;
    CHECK(test_start_and_poll(&test, &output, error) == 0 &&
              output != NULL && output->bundle != NULL &&
              output->bundle->event_count == 2U &&
              strcmp(output->bundle->events[0].values[0].text,
                     "violoncello") == 0 &&
              strcmp(output->bundle->events[1].values[0].text,
                     "violoncello") == 0,
          "provider rejected duplicate instrument labels: %s", error);
    test_case_cleanup(&test);
}

static void test_source_audio_id_is_skipped(void)
{
    TestCase test;
    const HWAInferenceOutput *output = NULL;
    char error[HWA_ERROR_SIZE] = {0};
    if (!test_case_init(
            &test, TEST_SOURCE_FRAMES, TEST_MAX_WORK_BYTES))
        return;
    test.request.source_recording_id = UINT64_C(1);
    CHECK(test_start_and_poll(&test, &output, error) == 0 &&
              output != NULL && output->bundle != NULL &&
              output->bundle->audio[0].id == UINT64_C(1) &&
              output->bundle->audio[1].id == UINT64_C(2) &&
              output->bundle->audio[2].id == UINT64_C(3) &&
              output->bundle->events[0].evidence_audio_id == UINT64_C(2) &&
              output->bundle->events[1].evidence_audio_id == UINT64_C(3),
          "provider did not skip the source audio ID: %s", error);
    test_case_cleanup(&test);
}

static void test_task_settings_and_input_shape(void)
{
    TestCase test;
    const HWAInferenceOutput *output = NULL;
    HWAInferenceInput inputs[2];
    char error[HWA_ERROR_SIZE] = {0};
    if (test_case_init(
            &test, TEST_SOURCE_FRAMES, TEST_MAX_WORK_BYTES)) {
        test.request.settings_json = " \n { \t } \r";
        CHECK(test_start_and_poll(&test, &output, error) == 0 &&
                  output != NULL && output->bundle != NULL &&
                  strstr(output->bundle->providers[0].settings_json,
                         "\"task_settings\":{}") != NULL,
              "provider did not normalize empty settings: %s", error);
        test_case_cleanup(&test);
    }
    error[0] = '\0';
    if (test_case_init(
            &test, TEST_SOURCE_FRAMES, TEST_MAX_WORK_BYTES)) {
        test.request.settings_json = "{\"extra\":true}";
        CHECK(test.provider.start(
                  test.provider.context, &test.request, &test.task,
                  error, sizeof(error)) != 0 && test.task == NULL,
              "provider accepted non-empty task settings");
        CHECK(test.runner_state.calls == 0U,
              "non-empty task settings reached the runner");
        test_case_cleanup(&test);
    }
    error[0] = '\0';
    if (test_case_init(
            &test, TEST_SOURCE_FRAMES, TEST_MAX_WORK_BYTES)) {
        inputs[0] = test.input;
        inputs[1] = test.input;
        inputs[1].id = "score";
        inputs[1].role = "score-context";
        inputs[1].media_type = "application/xml";
        test.request.inputs = inputs;
        test.request.input_count = 2U;
        test.request.max_input_bytes =
            (uint64_t)test.source_wave.size * UINT64_C(2);
        CHECK(test.provider.start(
                  test.provider.context, &test.request, &test.task,
                  error, sizeof(error)) != 0 && test.task == NULL,
              "provider accepted an extra input");
        CHECK(test.runner_state.calls == 0U,
              "extra input reached the runner");
        test_case_cleanup(&test);
    }
}

static void test_runner_result_rejections(void)
{
    static const struct {
        TestRunnerMode mode;
        const char *description;
        const char *error_fragment;
    } cases[] = {
        {TEST_RUNNER_ZERO_STEMS, "zero stems", "stem"},
        {TEST_RUNNER_NULL_STEMS, "null stems with a count", "stem"},
        {TEST_RUNNER_BAD_STEM_ID, "bad stem ID", "stem"},
        {TEST_RUNNER_EMPTY_INSTRUMENT, "empty instrument", "instrument"},
        {TEST_RUNNER_BAD_SCORE, "out-of-range score", NULL},
        {TEST_RUNNER_BAD_SCORE_FLAG, "invalid score flag", NULL},
        {TEST_RUNNER_SCORE_WITHOUT_FLAG, "hidden unflagged score", NULL},
        {TEST_RUNNER_BAD_CLOCK, "wrong stem clock", "clock"},
        {TEST_RUNNER_MALFORMED_WAVE, "malformed stem WAVE", "WAVE"},
        {TEST_RUNNER_DUPLICATE_ID, "duplicate stem ID", "duplicate"}
    };
    size_t index;
    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        TestCase test;
        char error[HWA_ERROR_SIZE] = {0};
        if (!test_case_init(
                &test, TEST_SOURCE_FRAMES, TEST_MAX_WORK_BYTES))
            continue;
        test.runner_state.mode = cases[index].mode;
        CHECK(test.provider.start(
                  test.provider.context, &test.request, &test.task,
                  error, sizeof(error)) != 0 && test.task == NULL,
              "provider accepted %s", cases[index].description);
        CHECK(test.runner_state.calls == 1U,
              "%s did not call the runner exactly once",
              cases[index].description);
        CHECK(error[0] != '\0' &&
                  (cases[index].error_fragment == NULL ||
                   strstr(error, cases[index].error_fragment) != NULL),
              "%s gave the wrong error: %s",
              cases[index].description, error);
        CHECK(test.runner_state.results_destroy_calls == 1U,
              "%s freed runner results the wrong number of times",
              cases[index].description);
        test_case_cleanup(&test);
    }
}

static void test_bad_hash_is_caught_before_write(void)
{
    TestCase test;
    const HWAInferenceOutput *output = NULL;
    char error[HWA_ERROR_SIZE] = {0};
    if (!test_case_init(
            &test, TEST_SOURCE_FRAMES, TEST_MAX_WORK_BYTES))
        return;
    CHECK(test_start_and_poll(&test, &output, error) == 0,
          "cannot build output for hash test: %s", error);
    if (output != NULL && test.runner_state.live_first_wave != NULL &&
        test.runner_state.live_first_wave_size > 44U) {
        test.runner_state.live_first_wave[44] ^= UINT8_C(1);
        error[0] = '\0';
        CHECK(hwa_inference_output_validate_for_request(
                  &test.provider, &test.request, output,
                  error, sizeof(error)) != 0 &&
                  strstr(error, "hash") != NULL,
              "mutated stem bytes did not fail their saved hash: %s",
              error);
    }
    test_case_cleanup(&test);
}

static void test_output_caps_and_zero_frame_source(void)
{
    TestCase test;
    char error[HWA_ERROR_SIZE] = {0};
    if (test_case_init(
            &test, TEST_SOURCE_FRAMES, TEST_MAX_WORK_BYTES)) {
        test.request.output_limits.max_audio_files = 1U;
        CHECK(test.provider.start(
                  test.provider.context, &test.request, &test.task,
                  error, sizeof(error)) != 0 && test.task == NULL,
              "provider accepted limits that cannot hold one stem");
        CHECK(test.runner_state.calls == 0U &&
                  test.runner_state.results_destroy_calls == 0U,
              "impossible output limits reached the runner");
        test_case_cleanup(&test);
    }
    error[0] = '\0';
    if (test_case_init(
            &test, TEST_SOURCE_FRAMES, TEST_MAX_WORK_BYTES)) {
        test.request.output_limits.max_audio_files = 2U;
        CHECK(test.provider.start(
                  test.provider.context, &test.request, &test.task,
                  error, sizeof(error)) != 0 && test.task == NULL,
              "provider exceeded max_audio_files");
        CHECK(test.runner_state.results_destroy_calls == 1U,
              "capped output did not release runner results");
        test_case_cleanup(&test);
    }
    error[0] = '\0';
    if (test_case_init(
            &test, TEST_SOURCE_FRAMES, TEST_MAX_WORK_BYTES)) {
        test.request.output_limits.max_work_bytes = UINT64_C(128);
        CHECK(test.provider.start(
                  test.provider.context, &test.request, &test.task,
                  error, sizeof(error)) != 0 && test.task == NULL,
              "provider exceeded the request output work cap");
        CHECK(test.runner_state.calls == 1U &&
                  test.runner_state.results_destroy_calls == 1U,
              "request work cap did not release runner results");
        test_case_cleanup(&test);
    }
    error[0] = '\0';
    if (test_case_init(&test, TEST_SOURCE_FRAMES, UINT64_C(128))) {
        CHECK(test.provider.start(
                  test.provider.context, &test.request, &test.task,
                  error, sizeof(error)) != 0 && test.task == NULL,
              "provider exceeded its output work cap");
        CHECK(test.runner_state.calls == 1U &&
                  test.runner_state.results_destroy_calls == 1U,
              "work-capped output did not release runner results");
        test_case_cleanup(&test);
    }
    error[0] = '\0';
    if (!test_case_init(&test, 0U, TEST_MAX_WORK_BYTES)) return;
    CHECK(test.provider.start(
              test.provider.context, &test.request, &test.task,
              error, sizeof(error)) != 0 && test.task == NULL,
          "provider accepted a zero-frame source");
    CHECK(test.runner_state.calls == 0U,
          "zero-frame source reached the runner");
    CHECK(strstr(error, "empty") != NULL,
          "zero-frame source gave the wrong error: %s", error);
    test_case_cleanup(&test);
}

static void test_runner_failure_and_failed_init_ownership(void)
{
    TestCase test;
    TestRunnerState state;
    HWAInstrumentStemRunner runner;
    HWAInferenceProvider provider;
    char error[HWA_ERROR_SIZE] = {0};
    if (test_case_init(
            &test, TEST_SOURCE_FRAMES, TEST_MAX_WORK_BYTES)) {
        test.runner_state.mode = TEST_RUNNER_FAILURE;
        CHECK(test.provider.start(
                  test.provider.context, &test.request, &test.task,
                  error, sizeof(error)) != 0 && test.task == NULL,
              "provider accepted runner failure");
        CHECK(test.runner_state.calls == 1U &&
                  test.runner_state.results_destroy_calls == 1U &&
                  strstr(error, "fake separator failed") != NULL,
              "runner failure propagation or ownership is wrong: %s",
              error);
        test_case_cleanup(&test);
    }
    memset(&state, 0, sizeof(state));
    memset(&runner, 0, sizeof(runner));
    memset(&provider, 0xff, sizeof(provider));
    runner.context = &state;
    runner.runtime_name = "fake-runtime";
    runner.runtime_version = "1.2.3";
    runner.backend = "cpu";
    runner.fallback = "";
    runner.run = test_run;
    runner.results_destroy = test_results_destroy;
    runner.destroy = test_runner_destroy;
    CHECK(hwa_instrument_stem_provider_init(
              &provider, test_model_sha256, "not-a-sha256",
              TEST_MAX_WORK_BYTES, &runner,
              error, sizeof(error)) != 0,
          "provider init accepted a bad adapter hash");
    CHECK(provider.context == NULL && state.context_destroy_calls == 0U,
          "failed provider init took runner ownership");
    test_runner_destroy(&state);
    CHECK(state.context_destroy_calls == 1U,
          "caller could not destroy its retained runner");
}

static void test_late_runner_result_is_rejected(void)
{
    TestCase test;
    char error[HWA_ERROR_SIZE] = {0};
    if (!test_case_init(
            &test, TEST_SOURCE_FRAMES, TEST_MAX_WORK_BYTES))
        return;
    test.runner_state.runner_delay_milliseconds = UINT64_C(10);
    test.runner_state.expected_timeout_milliseconds = UINT64_C(1);
    test.request.timeout_milliseconds = UINT64_C(1);
    CHECK(test.provider.start(
              test.provider.context, &test.request, &test.task,
              error, sizeof(error)) != 0 && test.task == NULL,
          "provider accepted a late runner result");
    CHECK(test.runner_state.calls == 1U &&
              test.runner_state.results_destroy_calls == 1U,
          "late runner result was not released");
    CHECK(strstr(error, "deadline") != NULL,
          "late runner gave the wrong error: %s", error);
    test_case_cleanup(&test);
}

int main(void)
{
    test_valid_two_stems_and_round_trip();
    test_saved_provenance_shape_and_source_match();
    test_saved_provenance_canonical_string_escape();
    test_duplicate_instrument_labels_are_valid();
    test_source_audio_id_is_skipped();
    test_task_settings_and_input_shape();
    test_runner_result_rejections();
    test_bad_hash_is_caught_before_write();
    test_output_caps_and_zero_frame_source();
    test_runner_failure_and_failed_init_ownership();
    test_late_runner_result_is_rejected();
    if (failures != 0) {
        (void)fprintf(stderr, "%d instrument stem provider test(s) failed\n",
                      failures);
        return 1;
    }
    (void)puts("Instrument stem provider tests passed");
    return 0;
}

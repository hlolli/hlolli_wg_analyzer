#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "hlolli_wg_analyzer.h"
#include "sha256.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#include "windows_test_process.h"
#define TEST_MKDIR(path) _mkdir(path)
#define TEST_PID() ((long)_getpid())
#define TEST_RMDIR(path) _rmdir(path)
#define TEST_UNLINK(path) _unlink(path)
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define TEST_MKDIR(path) mkdir((path), 0700)
#define TEST_PID() ((long)getpid())
#define TEST_RMDIR(path) rmdir(path)
#define TEST_UNLINK(path) unlink(path)
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TEST_SAMPLE_RATE UINT32_C(44100)
#define TEST_FRAME_COUNT (UINT32_C(2) * TEST_SAMPLE_RATE)
#define TEST_TONE_START (TEST_SAMPLE_RATE * UINT32_C(2) / UINT32_C(5))
#define TEST_TONE_END (TEST_SAMPLE_RATE * UINT32_C(6) / UINT32_C(5))
#define TEST_BLOCK_ALIGN UINT32_C(8)
#define TEST_DATA_BYTES (TEST_FRAME_COUNT * TEST_BLOCK_ALIGN)
#define TEST_WAVE_BYTES (UINT64_C(44) + (uint64_t)TEST_DATA_BYTES)

#ifndef HWA_BASIC_PITCH_TEST_MODEL_SHA256
#error "HWA_BASIC_PITCH_TEST_MODEL_SHA256 must name the configured model hash"
#endif

static const char test_model_sha256[] =
    HWA_BASIC_PITCH_TEST_MODEL_SHA256;
static const char test_stem_model_sha256[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char test_source_sha256[] =
    "cccccccccccccccccccccccccccccccc"
    "cccccccccccccccccccccccccccccccc";
static const char test_stem_settings[] =
    "{\"task\":\"org.hlolli.instrument-stems-v1\","
    "\"seed\":\"00000000000000000000\",\"inputs\":[{"
    "\"id\":\"source\",\"role\":\"source-recording\","
    "\"media_type\":\"audio/wav\",\"name\":\"mix.wav\","
    "\"bytes\":\"00000000000000705644\","
    "\"sha256\":\"cccccccccccccccccccccccccccccccc"
    "cccccccccccccccccccccccccccccccc\"}],"
    "\"runtime\":{\"name\":\"fixture\",\"version\":\"1\","
    "\"backend\":\"cpu\",\"fallback\":\"\","
    "\"adapter_sha256\":\"dddddddddddddddddddddddddddddddd"
    "dddddddddddddddddddddddddddddddd\"},\"task_settings\":{}}";

typedef struct TestWorkspace {
    char directory[PATH_MAX];
    char stem_input[PATH_MAX];
    char input_bundle[PATH_MAX];
    char first_bundle[PATH_MAX];
    char second_bundle[PATH_MAX];
    char output[PATH_MAX];
    char errors[PATH_MAX];
    char stem_sha256[HWA_SHA256_HEX_SIZE];
} TestWorkspace;

static const char *analyzer_path;
static const char *model_path;
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

static int test_join(char path[PATH_MAX],
                     const char *directory,
                     const char *name)
{
    int length = snprintf(path, PATH_MAX, "%s/%s", directory, name);
    return length >= 0 && (size_t)length < PATH_MAX;
}

static int test_write_bytes(FILE *stream, const void *bytes, size_t size)
{
    return fwrite(bytes, 1U, size, stream) == size;
}

static int test_write_u16(FILE *stream, uint16_t value)
{
    unsigned char bytes[2];
    bytes[0] = (unsigned char)(value & UINT16_C(0xff));
    bytes[1] = (unsigned char)(value >> 8U);
    return test_write_bytes(stream, bytes, sizeof(bytes));
}

static int test_write_u32(FILE *stream, uint32_t value)
{
    unsigned char bytes[4];
    bytes[0] = (unsigned char)(value & UINT32_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT32_C(0xff));
    bytes[2] = (unsigned char)((value >> 16U) & UINT32_C(0xff));
    bytes[3] = (unsigned char)(value >> 24U);
    return test_write_bytes(stream, bytes, sizeof(bytes));
}

static int test_write_f32(FILE *stream, float value)
{
    uint32_t bits;
    if (sizeof(value) != sizeof(bits)) return 0;
    memcpy(&bits, &value, sizeof(bits));
    return test_write_u32(stream, bits);
}

static float test_tone_sample(uint32_t frame)
{
    static const int16_t cycle[32] = {
        0, 1598, 3135, 4551, 5793, 6811, 7568, 8035,
        8192, 8035, 7568, 6811, 5793, 4551, 3135, 1598,
        0, -1598, -3135, -4551, -5793, -6811, -7568, -8035,
        -8192, -8035, -7568, -6811, -5793, -4551, -3135, -1598
    };
    if (frame < TEST_TONE_START || frame >= TEST_TONE_END) return 0.0f;
    return (float)cycle[((frame - TEST_TONE_START) / UINT32_C(2)) %
                        UINT32_C(32)] /
           32768.0f;
}

static int test_write_wave(const char *path)
{
    FILE *stream = fopen(path, "wb");
    uint32_t frame;
    int result = 0;
    if (stream == NULL) return 0;
    if (!test_write_bytes(stream, "RIFF", 4U) ||
        !test_write_u32(stream, UINT32_C(36) + TEST_DATA_BYTES) ||
        !test_write_bytes(stream, "WAVEfmt ", 8U) ||
        !test_write_u32(stream, UINT32_C(16)) ||
        !test_write_u16(stream, UINT16_C(3)) ||
        !test_write_u16(stream, UINT16_C(2)) ||
        !test_write_u32(stream, TEST_SAMPLE_RATE) ||
        !test_write_u32(stream, TEST_SAMPLE_RATE * TEST_BLOCK_ALIGN) ||
        !test_write_u16(stream, (uint16_t)TEST_BLOCK_ALIGN) ||
        !test_write_u16(stream, UINT16_C(32)) ||
        !test_write_bytes(stream, "data", 4U) ||
        !test_write_u32(stream, TEST_DATA_BYTES)) {
        goto cleanup;
    }
    for (frame = 0U; frame < TEST_FRAME_COUNT; ++frame) {
        float sample = test_tone_sample(frame);
        if (!test_write_f32(stream, sample) ||
            !test_write_f32(stream, sample))
            goto cleanup;
    }
    result = 1;
cleanup:
    if (fclose(stream) != 0) result = 0;
    return result;
}

static void test_set_format(HWAFormat *format)
{
    memset(format, 0, sizeof(*format));
    format->container = HWA_CONTAINER_RIFF;
    format->encoding = HWA_ENCODING_IEEE_FLOAT;
    format->channels = UINT16_C(2);
    format->sample_rate_hz = TEST_SAMPLE_RATE;
    format->bits_per_sample = UINT16_C(32);
    format->valid_bits_per_sample = UINT16_C(32);
    format->block_align = (uint16_t)TEST_BLOCK_ALIGN;
    format->frames = TEST_FRAME_COUNT;
    format->data_bytes = TEST_DATA_BYTES;
    format->duration_seconds =
        (double)TEST_FRAME_COUNT / (double)TEST_SAMPLE_RATE;
}

static int test_write_input_bundle(TestWorkspace *workspace)
{
    HWAEventBundleLimits limits;
    HWAEventProvider provider;
    HWAEventAudio audio[2];
    HWAEventValue value;
    HWAPerformanceEvent region;
    HWAEventBundle bundle;
    HWAEventFileBinding binding;
    HWAFormat format;
    char error[HWA_ERROR_SIZE] = {0};
    memset(&provider, 0, sizeof(provider));
    memset(audio, 0, sizeof(audio));
    memset(&value, 0, sizeof(value));
    memset(&region, 0, sizeof(region));
    memset(&bundle, 0, sizeof(bundle));
    test_set_format(&format);
    if (hwa_sha256_file(workspace->stem_input, TEST_WAVE_BYTES,
                        workspace->stem_sha256,
                        error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "cannot hash stem fixture: %s\n", error);
        return 0;
    }

    provider.id = UINT64_C(1);
    provider.name = "org.hlolli.instrument-stem-provider";
    provider.version = "1";
    memcpy(provider.model_sha256, test_stem_model_sha256,
           sizeof(provider.model_sha256));
    provider.settings_json = (char *)test_stem_settings;

    audio[0].id = UINT64_C(9);
    audio[0].kind = HWA_EVENT_SOURCE_RECORDING;
    audio[0].name = "mix.wav";
    audio[0].relative_path = "";
    audio[0].path_hint = "";
    memcpy(audio[0].sha256, test_source_sha256,
           sizeof(audio[0].sha256));
    audio[0].file_bytes = TEST_WAVE_BYTES;
    audio[0].format = format;

    audio[1].id = UINT64_C(1);
    audio[1].kind = HWA_EVENT_INSTRUMENT_STEM;
    audio[1].name = "tone";
    audio[1].relative_path = "audio/tone.wav";
    audio[1].path_hint = "";
    memcpy(audio[1].sha256, workspace->stem_sha256,
           sizeof(audio[1].sha256));
    audio[1].file_bytes = TEST_WAVE_BYTES;
    audio[1].format = format;
    audio[1].source_recording_id = UINT64_C(9);
    audio[1].source_recording_id_valid = 1;

    value.name = "instrument";
    value.kind = HWA_EVENT_VALUE_TEXT;
    value.basis = HWA_EVENT_INFERENCE;
    value.text = "other";
    value.unit = "";
    value.provider_id = UINT64_C(1);
    value.provider_id_valid = 1;
    value.selected = 1;

    region.id = UINT64_C(1);
    region.kind = "instrument-region";
    region.source_recording_id = UINT64_C(9);
    region.evidence_audio_id = UINT64_C(1);
    region.evidence_audio_id_valid = 1;
    region.start_sample = 0U;
    region.end_sample = TEST_FRAME_COUNT;
    region.voice = "";
    region.part = "tone";
    region.score_event_id = "";
    region.values = &value;
    region.value_count = 1U;

    bundle.providers = &provider;
    bundle.provider_count = 1U;
    bundle.audio = audio;
    bundle.audio_count = 2U;
    bundle.events = &region;
    bundle.event_count = 1U;
    binding.relative_path = "audio/tone.wav";
    binding.source_path = workspace->stem_input;
    hwa_event_bundle_limits_default(&limits);
    if (hwa_event_bundle_write(workspace->input_bundle, &bundle,
                               &binding, 1U, &limits,
                               error, sizeof(error)) != 0) {
        (void)fprintf(stderr, "cannot write stem fixture: %s\n", error);
        return 0;
    }
    return 1;
}

static int test_make_workspace(TestWorkspace *workspace)
{
    const char *root;
    unsigned attempt;
    memset(workspace, 0, sizeof(*workspace));
#if defined(_WIN32)
    root = getenv("TEMP");
    if (root == NULL || root[0] == '\0') root = ".";
#else
    root = "/tmp";
#endif
    for (attempt = 0U; attempt < 100U; ++attempt) {
        int length = snprintf(workspace->directory, PATH_MAX,
                              "%s/hwa-stem-note-onnx-%ld-%u",
                              root, TEST_PID(), attempt);
        if (length < 0 || (size_t)length >= PATH_MAX) return 0;
        if (TEST_MKDIR(workspace->directory) == 0) break;
        if (errno != EEXIST) return 0;
    }
    return attempt < 100U &&
           test_join(workspace->stem_input, workspace->directory,
                     "tone.wav") &&
           test_join(workspace->input_bundle, workspace->directory,
                     "stems.hwa-events") &&
           test_join(workspace->first_bundle, workspace->directory,
                     "first.hwa-events") &&
           test_join(workspace->second_bundle, workspace->directory,
                     "second.hwa-events") &&
           test_join(workspace->output, workspace->directory,
                     "stdout.txt") &&
           test_join(workspace->errors, workspace->directory,
                     "stderr.txt") &&
           test_write_wave(workspace->stem_input) &&
           test_write_input_bundle(workspace);
}

static int test_run(const TestWorkspace *workspace,
                    const char *const *arguments,
                    size_t argument_count)
{
#if defined(_WIN32)
    return hwa_test_spawn_redirected(analyzer_path, arguments, argument_count,
                                     NULL, workspace->output,
                                     workspace->errors);
#else
    pid_t child = fork();
    int status;
    if (child < 0) return -1;
    if (child == 0) {
        char **argv = (char **)calloc(argument_count + 2U, sizeof(*argv));
        int output;
        int errors;
        size_t index;
        if (argv == NULL) _exit(126);
        argv[0] = (char *)analyzer_path;
        for (index = 0U; index < argument_count; ++index)
            argv[index + 1U] = (char *)arguments[index];
        output = open(workspace->output,
                      O_CREAT | O_TRUNC | O_WRONLY, 0600);
        errors = open(workspace->errors,
                      O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (output < 0 || errors < 0 || dup2(output, STDOUT_FILENO) < 0 ||
            dup2(errors, STDERR_FILENO) < 0)
            _exit(126);
        (void)close(output);
        (void)close(errors);
        (void)execv(analyzer_path, argv);
        _exit(127);
    }
    if (waitpid(child, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#endif
}

static int test_file_is_empty(const char *path)
{
    FILE *stream = fopen(path, "rb");
    int result;
    if (stream == NULL) return 0;
    result = fgetc(stream) == EOF && !ferror(stream);
    if (fclose(stream) != 0) result = 0;
    return result;
}

static int test_file_contains(const char *path, const char *needle)
{
    char buffer[4096];
    FILE *stream = fopen(path, "rb");
    size_t count;
    int result;
    if (stream == NULL) return 0;
    count = fread(buffer, 1U, sizeof(buffer) - 1U, stream);
    buffer[count] = '\0';
    result = !ferror(stream) && strstr(buffer, needle) != NULL;
    if (fclose(stream) != 0) result = 0;
    return result;
}

static int test_files_equal(const char *first, const char *second)
{
    FILE *left = fopen(first, "rb");
    FILE *right = fopen(second, "rb");
    int result = left != NULL && right != NULL;
    if (!result) goto cleanup;
    for (;;) {
        unsigned char left_bytes[4096];
        unsigned char right_bytes[4096];
        size_t left_count = fread(left_bytes, 1U, sizeof(left_bytes), left);
        size_t right_count = fread(right_bytes, 1U, sizeof(right_bytes), right);
        if (left_count != right_count ||
            memcmp(left_bytes, right_bytes, left_count) != 0) {
            result = 0;
            break;
        }
        if (left_count < sizeof(left_bytes)) {
            if (ferror(left) || ferror(right)) result = 0;
            break;
        }
    }
cleanup:
    if (left != NULL && fclose(left) != 0) result = 0;
    if (right != NULL && fclose(right) != 0) result = 0;
    return result;
}

static int test_format_is_expected(const HWAFormat *format)
{
    return format->container == HWA_CONTAINER_RIFF &&
           format->encoding == HWA_ENCODING_IEEE_FLOAT &&
           format->channels == 2U &&
           format->sample_rate_hz == TEST_SAMPLE_RATE &&
           format->bits_per_sample == 32U &&
           format->valid_bits_per_sample == 32U &&
           format->block_align == TEST_BLOCK_ALIGN &&
           format->frames == TEST_FRAME_COUNT &&
           format->data_bytes == TEST_DATA_BYTES &&
           format->duration_seconds == 2.0;
}

static const HWAEventProvider *test_find_provider(
    const HWAEventBundle *bundle,
    const char *name)
{
    const HWAEventProvider *found = NULL;
    size_t index;
    for (index = 0U; index < bundle->provider_count; ++index) {
        const HWAEventProvider *provider = &bundle->providers[index];
        if (provider->name != NULL && strcmp(provider->name, name) == 0) {
            if (found != NULL) return NULL;
            found = provider;
        }
    }
    return found;
}

static const HWAEventAudio *test_find_audio(const HWAEventBundle *bundle,
                                            uint64_t id)
{
    size_t index;
    for (index = 0U; index < bundle->audio_count; ++index)
        if (bundle->audio[index].id == id) return &bundle->audio[index];
    return NULL;
}

static const HWAPerformanceEvent *test_find_event(
    const HWAEventBundle *bundle,
    uint64_t id)
{
    size_t index;
    for (index = 0U; index < bundle->event_count; ++index)
        if (bundle->events[index].id == id) return &bundle->events[index];
    return NULL;
}

static const HWAEventValue *test_selected_pitch(
    const HWAPerformanceEvent *event)
{
    const HWAEventValue *found = NULL;
    size_t index;
    for (index = 0U; index < event->value_count; ++index) {
        const HWAEventValue *value = &event->values[index];
        if (value->name != NULL && strcmp(value->name, "pitch-hz") == 0 &&
            value->selected) {
            if (found != NULL) return NULL;
            found = value;
        }
    }
    return found;
}

static void test_check_region(const HWAPerformanceEvent *region)
{
    const HWAEventValue *value;
    CHECK(region != NULL, "input instrument region was not preserved");
    if (region == NULL) return;
    CHECK(region->kind != NULL &&
              strcmp(region->kind, "instrument-region") == 0 &&
              region->source_recording_id == UINT64_C(9) &&
              region->evidence_audio_id_valid &&
              region->evidence_audio_id == UINT64_C(1) &&
              !region->parent_id_valid && region->start_sample == 0U &&
              region->end_sample == TEST_FRAME_COUNT &&
              region->voice != NULL && region->voice[0] == '\0' &&
              region->part != NULL && strcmp(region->part, "tone") == 0 &&
              region->score_event_id != NULL &&
              region->score_event_id[0] == '\0' &&
              region->value_count == 1U && region->trace_ref_count == 0U,
          "input instrument region fields changed");
    if (region->value_count != 1U || region->values == NULL) return;
    value = &region->values[0];
    CHECK(value->name != NULL && strcmp(value->name, "instrument") == 0 &&
              value->kind == HWA_EVENT_VALUE_TEXT &&
              value->basis == HWA_EVENT_INFERENCE &&
              value->text != NULL && strcmp(value->text, "other") == 0 &&
              value->unit != NULL && value->unit[0] == '\0' &&
              !value->score_valid && value->provider_id_valid &&
              value->provider_id == UINT64_C(1) && value->selected,
          "input instrument value changed");
}

static void test_check_bundle(const HWAEventBundle *bundle,
                              const TestWorkspace *workspace)
{
    const HWAEventProvider *stem_provider;
    const HWAEventProvider *note_provider;
    const HWAEventAudio *source;
    const HWAEventAudio *stem;
    size_t note_count = 0U;
    size_t target_count = 0U;
    size_t index;
    CHECK(bundle->provider_count == 2U && bundle->providers != NULL,
          "derived bundle has %zu providers, wanted 2",
          bundle->provider_count);
    CHECK(bundle->audio_count == 2U && bundle->audio != NULL,
          "derived bundle has %zu audio rows, wanted 2",
          bundle->audio_count);
    CHECK(bundle->event_count >= 2U && bundle->events != NULL,
          "derived bundle has no inferred notes");
    CHECK(bundle->trace_count == 0U && bundle->warning_count == 0U,
          "derived bundle has unexpected traces or warnings");

    stem_provider = test_find_provider(
        bundle, "org.hlolli.instrument-stem-provider");
    note_provider = test_find_provider(
        bundle, "org.hlolli.basic-pitch-audio-provider");
    CHECK(stem_provider != NULL && stem_provider->id == UINT64_C(1) &&
              stem_provider->version != NULL &&
              strcmp(stem_provider->version, "1") == 0 &&
              strcmp(stem_provider->model_sha256,
                     test_stem_model_sha256) == 0 &&
              stem_provider->settings_json != NULL &&
              strcmp(stem_provider->settings_json, test_stem_settings) == 0,
          "input stem provider changed");
    CHECK(note_provider != NULL && note_provider->id == UINT64_C(2) &&
              note_provider->version != NULL &&
              strcmp(note_provider->version, "1") == 0 &&
              strcmp(note_provider->model_sha256,
                     test_model_sha256) == 0 &&
              note_provider->settings_json != NULL &&
              strstr(note_provider->settings_json,
                     "\"task\":\"org.hlolli.note-events-on-audio-v1\"") !=
                  NULL &&
              strstr(note_provider->settings_json,
                     "\"name\":\"stereo-average-blackman-sinc-127-"
                     "decimate-2-v1\"") != NULL &&
              strstr(note_provider->settings_json,
                     "\"name\":\"org.hlolli.basic-pitch-onnx\"") != NULL,
          "raw-audio note provider identity or provenance is wrong");

    source = test_find_audio(bundle, UINT64_C(9));
    stem = test_find_audio(bundle, UINT64_C(1));
    CHECK(source != NULL && source->kind == HWA_EVENT_SOURCE_RECORDING &&
              source->name != NULL && strcmp(source->name, "mix.wav") == 0 &&
              source->relative_path != NULL &&
              source->relative_path[0] == '\0' &&
              source->path_hint != NULL && source->path_hint[0] == '\0' &&
              strcmp(source->sha256, test_source_sha256) == 0 &&
              source->file_bytes == TEST_WAVE_BYTES &&
              test_format_is_expected(&source->format) &&
              !source->source_recording_id_valid,
          "input source row changed");
    CHECK(stem != NULL && stem->kind == HWA_EVENT_INSTRUMENT_STEM &&
              stem->name != NULL && strcmp(stem->name, "tone") == 0 &&
              stem->relative_path != NULL &&
              strcmp(stem->relative_path, "audio/tone.wav") == 0 &&
              stem->path_hint != NULL && stem->path_hint[0] == '\0' &&
              strcmp(stem->sha256, workspace->stem_sha256) == 0 &&
              stem->file_bytes == TEST_WAVE_BYTES &&
              test_format_is_expected(&stem->format) &&
              stem->source_recording_id_valid &&
              stem->source_recording_id == UINT64_C(9),
          "input stem row changed");
    test_check_region(test_find_event(bundle, UINT64_C(1)));

    for (index = 0U; index < bundle->event_count; ++index) {
        const HWAPerformanceEvent *event = &bundle->events[index];
        const HWAEventValue *pitch;
        if (event->id == UINT64_C(1)) continue;
        note_count++;
        pitch = test_selected_pitch(event);
        CHECK(event->kind != NULL && strcmp(event->kind, "note") == 0 &&
                  event->source_recording_id == UINT64_C(9) &&
                  event->evidence_audio_id_valid &&
                  event->evidence_audio_id == UINT64_C(1) &&
                  event->parent_id_valid &&
                  event->parent_id == UINT64_C(1) &&
                  event->start_sample < event->end_sample &&
                  event->end_sample <= TEST_FRAME_COUNT &&
                  event->start_sample % UINT64_C(2) == 0U &&
                  event->end_sample % UINT64_C(2) == 0U &&
                  event->voice != NULL && event->voice[0] == '\0' &&
                  event->part != NULL && event->part[0] == '\0' &&
                  event->score_event_id != NULL &&
                  event->score_event_id[0] == '\0' &&
                  event->trace_ref_count == 0U,
              "inferred note has wrong links or source-clock bounds");
        CHECK(pitch != NULL && pitch->kind == HWA_EVENT_VALUE_F64 &&
                  pitch->basis == HWA_EVENT_INFERENCE &&
                  pitch->number > 0.0 && pitch->unit != NULL &&
                  strcmp(pitch->unit, "Hz") == 0 &&
                  pitch->score_valid && pitch->score >= 0.0 &&
                  pitch->score <= 1.0 && pitch->provider_id_valid &&
                  pitch->provider_id == UINT64_C(2),
              "inferred note has a wrong selected pitch");
        if (pitch != NULL && pitch->number >= 680.0 &&
            pitch->number <= 715.0 &&
            event->start_sample >= UINT64_C(12000) &&
            event->start_sample <= UINT64_C(24000) &&
            event->end_sample >= UINT64_C(44000) &&
            event->end_sample <= UINT64_C(62000)) {
            target_count++;
        }
    }
    CHECK(note_count != 0U, "real model returned no note events");
    CHECK(target_count != 0U,
          "no 689 Hz note used the mapped 44100 Hz source clock");
}

static int test_infer(const TestWorkspace *workspace, const char *output)
{
    const char *arguments[] = {
        "infer-stem-note-events", workspace->input_bundle,
        "--model", model_path,
        "--expect-model-sha256", test_model_sha256,
        "--output", output
    };
    int status = test_run(
        workspace, arguments, sizeof(arguments) / sizeof(arguments[0]));
    CHECK(status == 0,
          "real-model stem note inference exited with status %d", status);
    CHECK(test_file_is_empty(workspace->output),
          "successful stem note inference wrote standard output");
    CHECK(test_file_is_empty(workspace->errors),
          "successful stem note inference wrote standard error");
    return status == 0;
}

static void test_reject_descendant_output(const TestWorkspace *workspace)
{
    char descendant[PATH_MAX];
    const char *arguments[] = {
        "infer-stem-note-events", workspace->input_bundle,
        "--model", model_path,
        "--expect-model-sha256", test_model_sha256,
        "--output", descendant
    };
    int written = snprintf(descendant, sizeof(descendant), "%s/derived",
                           workspace->input_bundle);
    int status;
    CHECK(written > 0 && (size_t)written < sizeof(descendant),
          "cannot form descendant output path");
    if (written <= 0 || (size_t)written >= sizeof(descendant)) return;
    status = test_run(
        workspace, arguments, sizeof(arguments) / sizeof(arguments[0]));
    CHECK(status != 0,
          "stem-note command accepted output inside its input bundle");
    CHECK(test_file_contains(workspace->errors,
                             "inside its source bundle"),
          "descendant output gave the wrong error");
}

static void test_validate(const TestWorkspace *workspace,
                          const char *bundle)
{
    const char *arguments[] = {"validate-event-bundle", bundle};
    int status = test_run(
        workspace, arguments, sizeof(arguments) / sizeof(arguments[0]));
    CHECK(status == 0, "bundle validation exited with status %d", status);
    CHECK(test_file_contains(workspace->output,
                             "Event bundle validation passed\n"),
          "bundle validation wrote no success report");
    CHECK(test_file_is_empty(workspace->errors),
          "successful bundle validation wrote standard error");
}

static void test_compare_saved_files(const TestWorkspace *workspace)
{
    static const char *const names[] = {
        "manifest.json", "events.jsonl", "traces.jsonl", "audio/tone.wav"
    };
    char first[PATH_MAX];
    char second[PATH_MAX];
    char input[PATH_MAX];
    size_t index;
    for (index = 0U; index < sizeof(names) / sizeof(names[0]); ++index) {
        CHECK(test_join(first, workspace->first_bundle, names[index]) &&
                  test_join(second, workspace->second_bundle, names[index]) &&
                  test_files_equal(first, second),
              "repeated stem note inference changed %s bytes", names[index]);
    }
    CHECK(test_join(input, workspace->input_bundle, "audio/tone.wav") &&
              test_join(first, workspace->first_bundle, "audio/tone.wav") &&
              test_join(second, workspace->second_bundle,
                        "audio/tone.wav") &&
              test_files_equal(input, first) && test_files_equal(input, second),
          "stem payload bytes changed during derivation");
}

static void test_remove_bundle(const char *directory)
{
    static const char *const root_names[] = {
        "manifest.json", "events.jsonl", "traces.jsonl"
    };
    char path[PATH_MAX];
    size_t index;
    if (directory == NULL || directory[0] == '\0') return;
    if (test_join(path, directory, "audio/tone.wav"))
        (void)TEST_UNLINK(path);
    if (test_join(path, directory, "audio")) (void)TEST_RMDIR(path);
    for (index = 0U;
         index < sizeof(root_names) / sizeof(root_names[0]); ++index) {
        if (test_join(path, directory, root_names[index]))
            (void)TEST_UNLINK(path);
    }
    if (test_join(path, directory, "traces")) (void)TEST_RMDIR(path);
    (void)TEST_RMDIR(directory);
}

static void test_workspace_close(TestWorkspace *workspace)
{
    test_remove_bundle(workspace->input_bundle);
    test_remove_bundle(workspace->first_bundle);
    test_remove_bundle(workspace->second_bundle);
    if (workspace->stem_input[0] != '\0')
        (void)TEST_UNLINK(workspace->stem_input);
    if (workspace->output[0] != '\0') (void)TEST_UNLINK(workspace->output);
    if (workspace->errors[0] != '\0') (void)TEST_UNLINK(workspace->errors);
    if (workspace->directory[0] != '\0')
        (void)TEST_RMDIR(workspace->directory);
}

static void test_real_model(void)
{
    TestWorkspace workspace;
    HWAEventBundleLimits limits;
    HWAEventBundle first;
    HWAEventBundle second;
    char error[HWA_ERROR_SIZE] = {0};
    memset(&workspace, 0, sizeof(workspace));
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    CHECK(test_make_workspace(&workspace),
          "cannot make stem note ONNX workspace");
    if (failures != 0) {
        test_workspace_close(&workspace);
        return;
    }
    test_reject_descendant_output(&workspace);
    if (failures != 0) goto cleanup;
    if (!test_infer(&workspace, workspace.first_bundle) ||
        !test_infer(&workspace, workspace.second_bundle))
        goto cleanup;
    test_validate(&workspace, workspace.first_bundle);
    test_validate(&workspace, workspace.second_bundle);
    test_compare_saved_files(&workspace);
    hwa_event_bundle_limits_default(&limits);
    if (hwa_event_bundle_read(workspace.first_bundle, &limits, &first,
                              error, sizeof(error)) == 0) {
        test_check_bundle(&first, &workspace);
    } else {
        CHECK(0, "cannot read first stem note bundle: %s", error);
    }
    error[0] = '\0';
    if (hwa_event_bundle_read(workspace.second_bundle, &limits, &second,
                              error, sizeof(error)) == 0) {
        test_check_bundle(&second, &workspace);
    } else {
        CHECK(0, "cannot read second stem note bundle: %s", error);
    }
cleanup:
    hwa_event_bundle_free(&first);
    hwa_event_bundle_free(&second);
    test_workspace_close(&workspace);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        (void)fputs(
            "usage: stem_note_onnx_cli_tests ANALYZER MODEL\n", stderr);
        return 2;
    }
    analyzer_path = argv[1];
    model_path = argv[2];
    test_real_model();
    if (failures != 0) {
        (void)fprintf(stderr, "%d stem note ONNX CLI test(s) failed\n",
                      failures);
        return 1;
    }
    (void)puts("Stem note ONNX CLI tests passed");
    return 0;
}

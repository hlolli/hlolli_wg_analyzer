#include "inference_provenance.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int unused_read(void *context,
                       uint64_t offset,
                       unsigned char *buffer,
                       size_t count)
{
    (void)context;
    (void)offset;
    (void)buffer;
    return count == 0U ? 0 : -1;
}

static void make_request(HWAInferenceRequest *request,
                         HWAInferenceInput inputs[2],
                         int reverse)
{
    static const char first_hash[] =
        "1111111111111111111111111111111111111111111111111111111111111111";
    static const char second_hash[] =
        "2222222222222222222222222222222222222222222222222222222222222222";
    HWAInferenceInput first;
    HWAInferenceInput second;
    memset(request, 0, sizeof(*request));
    memset(inputs, 0, 2U * sizeof(*inputs));
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    first.id = "source";
    first.role = "source-recording";
    first.media_type = "audio/wav";
    first.sha256 = first_hash;
    first.bytes.name = "take \"one\".wav";
    first.bytes.size = 1234U;
    first.bytes.read_at = unused_read;
    second.id = "score";
    second.role = "score-context";
    second.media_type = "application/vnd.recordare.musicxml+xml";
    second.sha256 = second_hash;
    second.bytes.name = "part.musicxml";
    second.bytes.size = 567U;
    second.bytes.read_at = unused_read;
    inputs[reverse ? 1 : 0] = first;
    inputs[reverse ? 0 : 1] = second;
    request->task = "org.hlolli.polyphonic-note-events-v1";
    request->settings_json = "{\"frame_threshold\":0.3}";
    request->seed = UINT64_C(42);
    request->inputs = inputs;
    request->input_count = 2U;
}

static HWAInferenceRuntimeProvenance make_runtime(void)
{
    HWAInferenceRuntimeProvenance runtime;
    runtime.name = "onnxruntime";
    runtime.version = "1.23.0";
    runtime.backend = "cpu";
    runtime.fallback = "";
    runtime.adapter_sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    return runtime;
}

static void test_settings_are_exact_and_input_order_independent(void)
{
    static const char expected[] =
        "{\"task\":\"org.hlolli.polyphonic-note-events-v1\","
        "\"seed\":\"00000000000000000042\",\"inputs\":["
        "{\"id\":\"score\",\"role\":\"score-context\","
        "\"media_type\":\"application/vnd.recordare.musicxml+xml\","
        "\"name\":\"part.musicxml\",\"bytes\":\"00000000000000000567\","
        "\"sha256\":\"2222222222222222222222222222222222222222222222222222222222222222\"},"
        "{\"id\":\"source\",\"role\":\"source-recording\","
        "\"media_type\":\"audio/wav\",\"name\":\"take \\\"one\\\".wav\","
        "\"bytes\":\"00000000000000001234\","
        "\"sha256\":\"1111111111111111111111111111111111111111111111111111111111111111\"}],"
        "\"runtime\":{\"name\":\"onnxruntime\",\"version\":\"1.23.0\","
        "\"backend\":\"cpu\",\"fallback\":\"\","
        "\"adapter_sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},"
        "\"task_settings\":{\"frame_threshold\":0.3}}";
    HWAInferenceRequest first_request;
    HWAInferenceRequest second_request;
    HWAInferenceInput first_inputs[2];
    HWAInferenceInput second_inputs[2];
    HWAInferenceRuntimeProvenance runtime = make_runtime();
    char *first = NULL;
    char *second = NULL;
    char error[128] = {0};
    make_request(&first_request, first_inputs, 0);
    make_request(&second_request, second_inputs, 1);
    CHECK(hwa_inference_provenance_settings_build(
              &first_request, &runtime, 8192U, &first,
              error, sizeof(error)) == 0,
          "cannot build first provenance: %s", error);
    CHECK(hwa_inference_provenance_settings_build(
              &second_request, &runtime, 8192U, &second,
              error, sizeof(error)) == 0,
          "cannot build reordered provenance: %s", error);
    CHECK(first != NULL && strcmp(first, expected) == 0,
          "provenance JSON differs:\n%s", first != NULL ? first : "<null>");
    CHECK(first != NULL && second != NULL && strcmp(first, second) == 0,
          "input order changed provenance JSON");
    free(first);
    free(second);
}

static void test_settings_fail_closed(void)
{
    HWAInferenceRequest request;
    HWAInferenceInput inputs[2];
    HWAInferenceRuntimeProvenance runtime = make_runtime();
    char *settings = (char *)(uintptr_t)1U;
    char error[128] = {0};
    make_request(&request, inputs, 0);
    CHECK(hwa_inference_provenance_settings_build(
              &request, &runtime, 32U, &settings,
              error, sizeof(error)) != 0 && settings == NULL &&
              strstr(error, "byte limit") != NULL,
          "small settings cap did not fail closed: %s", error);
    inputs[1].id = inputs[0].id;
    CHECK(hwa_inference_provenance_settings_build(
              &request, &runtime, 8192U, &settings,
              error, sizeof(error)) != 0 && settings == NULL &&
              strstr(error, "duplicate") != NULL,
          "duplicate input IDs gave the wrong failure: %s", error);
    inputs[1].id = "score";
    request.settings_json = "[]";
    CHECK(hwa_inference_provenance_settings_build(
              &request, &runtime, 8192U, &settings,
              error, sizeof(error)) != 0 && settings == NULL &&
              strstr(error, "facts") != NULL,
          "non-object task settings gave the wrong failure: %s", error);
    request.settings_json = "{}";
    runtime.adapter_sha256 = "";
    CHECK(hwa_inference_provenance_settings_build(
              &request, &runtime, 8192U, &settings,
              error, sizeof(error)) != 0 && settings == NULL &&
              strstr(error, "facts") != NULL,
          "empty adapter hash gave the wrong failure: %s", error);
}

static void test_invalid_name_bytes_fail_closed(void)
{
    HWAInferenceRequest request;
    HWAInferenceInput inputs[2];
    HWAInferenceRuntimeProvenance runtime = make_runtime();
    char name[] = {'b', 'a', 'd', (char)0xff, '.', 'w', 'a', 'v', '\0'};
    char *settings = NULL;
    char error[128] = {0};
    make_request(&request, inputs, 0);
    inputs[0].bytes.name = name;
    CHECK(hwa_inference_provenance_settings_build(
              &request, &runtime, 8192U, &settings,
              error, sizeof(error)) != 0 && settings == NULL &&
              strstr(error, "facts") != NULL,
          "invalid UTF-8 name gave the wrong failure: %s", error);
    free(settings);
}

int main(void)
{
    test_settings_are_exact_and_input_order_independent();
    test_settings_fail_closed();
    test_invalid_name_bytes_fail_closed();
    if (failures != 0) {
        (void)fprintf(stderr, "%d inference provenance test(s) failed\n",
                      failures);
        return 1;
    }
    (void)puts("inference provenance tests passed");
    return 0;
}

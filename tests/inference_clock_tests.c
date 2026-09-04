#include "inference_clock.h"

#include <stdint.h>
#include <stdio.h>
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

static void test_exact_basic_pitch_clock(void)
{
    uint64_t start = 0U;
    uint64_t end = 0U;
    char error[128] = {0};
    CHECK(hwa_inference_frame_span_to_samples(
              2U, 5U, 22050U, 256U, 22050U, 4096U,
              &start, &end, error, sizeof(error)) == 0,
          "exact clock mapping failed: %s", error);
    CHECK(start == 512U && end == 1280U,
          "exact clock mapped to %llu..%llu",
          (unsigned long long)start, (unsigned long long)end);
}

static void test_noninteger_clock_covers_the_span(void)
{
    uint64_t start = 0U;
    uint64_t end = 0U;
    char error[128] = {0};
    CHECK(hwa_inference_frame_span_to_samples(
              1U, 2U, 22050U, 256U, 48000U, 4096U,
              &start, &end, error, sizeof(error)) == 0,
          "noninteger clock mapping failed: %s", error);
    CHECK(start == 557U && end == 1115U,
          "noninteger clock did not use floor/ceiling: %llu..%llu",
          (unsigned long long)start, (unsigned long long)end);
}

static void test_end_is_clipped(void)
{
    uint64_t start = 0U;
    uint64_t end = 0U;
    char error[128] = {0};
    CHECK(hwa_inference_frame_span_to_samples(
              3U, 8U, 10U, 1U, 10U, 5U,
              &start, &end, error, sizeof(error)) == 0,
          "end clipping failed: %s", error);
    CHECK(start == 3U && end == 5U,
          "clipped clock mapped to %llu..%llu",
          (unsigned long long)start, (unsigned long long)end);
}

static void test_large_intermediate_stays_exact(void)
{
    uint64_t start = 0U;
    uint64_t end = 0U;
    char error[128] = {0};
    CHECK(hwa_inference_frame_span_to_samples(
              UINT64_MAX - 1U, UINT64_MAX, UINT64_MAX, 1U, 1U, 2U,
              &start, &end, error, sizeof(error)) == 0,
          "overflow-safe clock mapping failed: %s", error);
    CHECK(start == 0U && end == 1U,
          "overflow-safe clock mapped to %llu..%llu",
          (unsigned long long)start, (unsigned long long)end);
}

static void test_small_clock_grid(void)
{
    uint64_t numerator;
    uint64_t denominator;
    uint64_t sample_rate;
    uint64_t start_frame;
    for (numerator = 1U; numerator <= 9U; ++numerator) {
        for (denominator = 1U; denominator <= 7U; ++denominator) {
            for (sample_rate = 1U; sample_rate <= 11U; ++sample_rate) {
                for (start_frame = 0U; start_frame <= 12U; ++start_frame) {
                    uint64_t end_frame;
                    for (end_frame = start_frame + 1U;
                         end_frame <= 13U; ++end_frame) {
                        uint64_t source_frames =
                            (start_frame + end_frame + numerator +
                             denominator + sample_rate) % 37U + 1U;
                        uint64_t start_scaled =
                            start_frame * sample_rate * denominator;
                        uint64_t end_scaled =
                            end_frame * sample_rate * denominator;
                        uint64_t expected_start = start_scaled / numerator;
                        uint64_t expected_end =
                            (end_scaled + numerator - 1U) / numerator;
                        uint64_t actual_start = 99U;
                        uint64_t actual_end = 99U;
                        char error[128] = {0};
                        int result;
                        if (expected_start > source_frames)
                            expected_start = source_frames;
                        if (expected_end > source_frames)
                            expected_end = source_frames;
                        result = hwa_inference_frame_span_to_samples(
                            start_frame, end_frame, numerator, denominator,
                            (uint32_t)sample_rate, source_frames,
                            &actual_start, &actual_end, error, sizeof(error));
                        if (expected_start >= expected_end) {
                            CHECK(result != 0 && actual_start == 0U &&
                                      actual_end == 0U,
                                  "empty small-grid span passed");
                        } else {
                            CHECK(result == 0 &&
                                      actual_start == expected_start &&
                                      actual_end == expected_end,
                                  "small-grid span mapped to %llu..%llu, "
                                  "wanted %llu..%llu: %s",
                                  (unsigned long long)actual_start,
                                  (unsigned long long)actual_end,
                                  (unsigned long long)expected_start,
                                  (unsigned long long)expected_end, error);
                        }
                    }
                }
            }
        }
    }
}

static void test_overflowing_end_is_clipped(void)
{
    uint64_t start = 99U;
    uint64_t end = 99U;
    char error[128] = {0};
    CHECK(hwa_inference_frame_span_to_samples(
              0U, UINT64_MAX, 1U, 1U, 48000U, 192U,
              &start, &end, error, sizeof(error)) == 0,
          "overflowing end did not clip: %s", error);
    CHECK(start == 0U && end == 192U,
          "overflowing end mapped to %llu..%llu",
          (unsigned long long)start, (unsigned long long)end);
}

static void test_bad_and_outside_spans_fail_closed(void)
{
    uint64_t start = 99U;
    uint64_t end = 99U;
    char error[128] = {0};
    CHECK(hwa_inference_frame_span_to_samples(
              2U, 2U, 10U, 1U, 10U, 20U,
              &start, &end, error, sizeof(error)) != 0 &&
              start == 0U && end == 0U && error[0] != '\0',
          "empty model span did not fail closed");
    start = 99U;
    end = 99U;
    CHECK(hwa_inference_frame_span_to_samples(
              20U, 21U, 10U, 1U, 10U, 20U,
              &start, &end, error, sizeof(error)) != 0 &&
              start == 0U && end == 0U &&
              strstr(error, "past the source") != NULL,
          "out-of-source model span gave the wrong failure: %s", error);
    CHECK(hwa_inference_frame_span_to_samples(
              UINT64_MAX - 1U, UINT64_MAX, 1U, UINT64_MAX,
              UINT32_MAX, UINT64_MAX, &start, &end,
              error, sizeof(error)) != 0 &&
              strstr(error, "past the source") != NULL,
          "out-of-range large span gave the wrong failure: %s", error);
    start = 99U;
    CHECK(hwa_inference_frame_span_to_samples(
              0U, 1U, 1U, 1U, 1U, 2U,
              &start, &start, error, sizeof(error)) != 0 &&
              start == 0U,
          "aliased clock outputs did not fail closed");
}

static void test_monotonic_deadline(void)
{
    uint64_t started = 0U;
    uint64_t remaining = 0U;
    char error[128] = {0};
    CHECK(hwa_inference_deadline_start(
              &started, error, sizeof(error)) == 0,
          "deadline clock did not start: %s", error);
    CHECK(hwa_inference_deadline_check(
              started, UINT64_MAX, error, sizeof(error)) == 0,
          "open deadline expired: %s", error);
    CHECK(hwa_inference_deadline_check(
              UINT64_MAX, UINT64_C(1), error, sizeof(error)) != 0 &&
              strstr(error, "expired") != NULL,
          "backwards deadline did not fail closed: %s", error);
    CHECK(hwa_inference_deadline_check(
              started, UINT64_C(0), error, sizeof(error)) != 0 &&
              strstr(error, "invalid") != NULL,
          "zero deadline did not fail closed: %s", error);
    CHECK(hwa_inference_deadline_remaining(
              started, UINT64_MAX, &remaining,
              error, sizeof(error)) == 0 && remaining != 0U,
          "live deadline has no remaining time: %s", error);
    remaining = 99U;
    CHECK(hwa_inference_deadline_remaining(
              UINT64_MAX, UINT64_C(1), &remaining,
              error, sizeof(error)) != 0 && remaining == 0U &&
              strstr(error, "expired") != NULL,
          "backwards remaining deadline did not fail closed: %s", error);
    remaining = 99U;
    CHECK(hwa_inference_deadline_remaining(
              started, UINT64_C(0), &remaining,
              error, sizeof(error)) != 0 && remaining == 0U &&
              strstr(error, "invalid") != NULL,
          "zero remaining deadline did not fail closed: %s", error);
    CHECK(hwa_inference_deadline_remaining(
              started, UINT64_MAX, NULL,
              error, sizeof(error)) != 0 &&
              strstr(error, "invalid") != NULL,
          "null remaining deadline output did not fail closed: %s", error);
}

int main(void)
{
    test_exact_basic_pitch_clock();
    test_noninteger_clock_covers_the_span();
    test_end_is_clipped();
    test_large_intermediate_stays_exact();
    test_small_clock_grid();
    test_overflowing_end_is_clipped();
    test_bad_and_outside_spans_fail_closed();
    test_monotonic_deadline();
    if (failures != 0) {
        (void)fprintf(stderr, "%d inference clock test(s) failed\n", failures);
        return 1;
    }
    (void)puts("inference clock tests passed");
    return 0;
}

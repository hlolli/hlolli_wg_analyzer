#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "inference_clock.h"

#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

static void hwa_inference_clock_error(char *error,
                                      size_t error_size,
                                      const char *message)
{
    if (error == NULL || error_size == 0U) return;
    (void)snprintf(error, error_size, "%s", message);
    error[error_size - 1U] = '\0';
}

static int hwa_inference_now_milliseconds(uint64_t *milliseconds)
{
    if (milliseconds == NULL) return -1;
#ifdef _WIN32
    *milliseconds = (uint64_t)GetTickCount64();
    return 0;
#else
    {
        struct timespec now;
        uint64_t seconds;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
            now.tv_sec < 0 || now.tv_nsec < 0)
            return -1;
        seconds = (uint64_t)now.tv_sec;
        if (seconds > UINT64_MAX / UINT64_C(1000)) return -1;
        *milliseconds = seconds * UINT64_C(1000) +
                        (uint64_t)now.tv_nsec / UINT64_C(1000000);
        return 0;
    }
#endif
}

int hwa_inference_deadline_start(uint64_t *started_milliseconds,
                                 char *error,
                                 size_t error_size)
{
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (started_milliseconds != NULL) *started_milliseconds = 0U;
    if (hwa_inference_now_milliseconds(started_milliseconds) != 0) {
        hwa_inference_clock_error(
            error, error_size, "cannot read the monotonic inference clock");
        return -1;
    }
    return 0;
}

int hwa_inference_deadline_check(uint64_t started_milliseconds,
                                 uint64_t timeout_milliseconds,
                                 char *error,
                                 size_t error_size)
{
    uint64_t now = 0U;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (timeout_milliseconds == 0U) {
        hwa_inference_clock_error(
            error, error_size, "invalid inference deadline");
        return -1;
    }
    if (hwa_inference_now_milliseconds(&now) != 0) {
        hwa_inference_clock_error(
            error, error_size, "cannot read the monotonic inference clock");
        return -1;
    }
    if (now < started_milliseconds ||
        now - started_milliseconds >= timeout_milliseconds) {
        hwa_inference_clock_error(
            error, error_size, "inference deadline expired");
        return -1;
    }
    return 0;
}

int hwa_inference_deadline_remaining(uint64_t started_milliseconds,
                                     uint64_t timeout_milliseconds,
                                     uint64_t *remaining_milliseconds,
                                     char *error,
                                     size_t error_size)
{
    uint64_t now = 0U;
    uint64_t elapsed;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (remaining_milliseconds != NULL) *remaining_milliseconds = 0U;
    if (remaining_milliseconds == NULL || timeout_milliseconds == 0U) {
        hwa_inference_clock_error(
            error, error_size, "invalid inference deadline");
        return -1;
    }
    if (hwa_inference_now_milliseconds(&now) != 0) {
        hwa_inference_clock_error(
            error, error_size, "cannot read the monotonic inference clock");
        return -1;
    }
    if (now < started_milliseconds) {
        hwa_inference_clock_error(
            error, error_size, "inference deadline expired");
        return -1;
    }
    elapsed = now - started_milliseconds;
    if (elapsed >= timeout_milliseconds) {
        hwa_inference_clock_error(
            error, error_size, "inference deadline expired");
        return -1;
    }
    *remaining_milliseconds = timeout_milliseconds - elapsed;
    return 0;
}

/* Return the quotient and remainder of left * right / divisor. */
static int hwa_inference_mul_div(uint64_t left,
                                 uint64_t right,
                                 uint64_t divisor,
                                 uint64_t *quotient,
                                 uint64_t *remainder)
{
    uint64_t result_q = 0U;
    uint64_t result_r = 0U;
    uint64_t term_q;
    uint64_t term_r;
    if (divisor == 0U || quotient == NULL || remainder == NULL) return -1;
    term_q = left / divisor;
    term_r = left % divisor;
    while (right != 0U) {
        if ((right & UINT64_C(1)) != 0U) {
            uint64_t carry;
            uint64_t next_r;
            if (term_r == 0U) {
                carry = 0U;
                next_r = result_r;
            } else if (result_r >= divisor - term_r) {
                carry = 1U;
                next_r = result_r - (divisor - term_r);
            } else {
                carry = 0U;
                next_r = result_r + term_r;
            }
            if (result_q > UINT64_MAX - term_q ||
                result_q + term_q > UINT64_MAX - carry)
                return -1;
            result_q += term_q + carry;
            result_r = next_r;
        }
        right >>= 1U;
        if (right != 0U) {
            uint64_t carry;
            uint64_t next_r;
            if (term_r != 0U && term_r >= divisor - term_r) {
                carry = 1U;
                next_r = term_r - (divisor - term_r);
            } else {
                carry = 0U;
                next_r = term_r + term_r;
            }
            if (term_q > (UINT64_MAX - carry) / UINT64_C(2)) return -1;
            term_q = term_q * UINT64_C(2) + carry;
            term_r = next_r;
        }
    }
    *quotient = result_q;
    *remainder = result_r;
    return 0;
}

/* Return the quotient and remainder of first * second * third / divisor. */
static int hwa_inference_mul3_div(uint64_t first,
                                  uint64_t second,
                                  uint64_t third,
                                  uint64_t divisor,
                                  uint64_t *quotient,
                                  uint64_t *remainder)
{
    uint64_t first_q;
    uint64_t first_r;
    uint64_t tail_q;
    uint64_t tail_r;
    uint64_t whole;
    if (hwa_inference_mul_div(first, second, divisor,
                              &first_q, &first_r) != 0 ||
        hwa_inference_mul_div(first_r, third, divisor,
                              &tail_q, &tail_r) != 0 ||
        (third != 0U && first_q > UINT64_MAX / third))
        return -1;
    whole = first_q * third;
    if (whole > UINT64_MAX - tail_q) return -1;
    *quotient = whole + tail_q;
    *remainder = tail_r;
    return 0;
}

static void hwa_inference_frame_to_sample_clipped(
    uint64_t frame,
    uint32_t source_sample_rate,
    uint64_t rate_denominator,
    uint64_t rate_numerator,
    int round_up,
    uint64_t clip_at,
    uint64_t *sample)
{
    uint64_t quotient;
    uint64_t remainder;
    if (hwa_inference_mul3_div(
            frame, (uint64_t)source_sample_rate, rate_denominator,
            rate_numerator, &quotient, &remainder) != 0 ||
        quotient >= clip_at) {
        *sample = clip_at;
        return;
    }
    if (round_up && remainder != 0U) {
        quotient++;
    }
    *sample = quotient;
}

int hwa_inference_frame_span_to_samples(
    uint64_t start_frame,
    uint64_t end_frame,
    uint64_t frame_rate_numerator,
    uint64_t frame_rate_denominator,
    uint32_t source_sample_rate,
    uint64_t source_frames,
    uint64_t *start_sample,
    uint64_t *end_sample,
    char *error,
    size_t error_size)
{
    uint64_t mapped_start = 0U;
    uint64_t mapped_end = 0U;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (start_sample != NULL) *start_sample = 0U;
    if (end_sample != NULL) *end_sample = 0U;
    if (start_sample == NULL || end_sample == NULL ||
        start_sample == end_sample ||
        start_frame >= end_frame || frame_rate_numerator == 0U ||
        frame_rate_denominator == 0U || source_sample_rate == 0U ||
        source_frames == 0U) {
        hwa_inference_clock_error(error, error_size,
                                  "invalid inference frame span");
        return -1;
    }
    hwa_inference_frame_to_sample_clipped(
        start_frame, source_sample_rate, frame_rate_denominator,
        frame_rate_numerator, 0, source_frames, &mapped_start);
    hwa_inference_frame_to_sample_clipped(
        end_frame, source_sample_rate, frame_rate_denominator,
        frame_rate_numerator, 1, source_frames, &mapped_end);
    if (mapped_start >= source_frames) {
        hwa_inference_clock_error(error, error_size,
                                  "inference frame span starts past the source");
        return -1;
    }
    if (mapped_start >= mapped_end) {
        hwa_inference_clock_error(error, error_size,
                                  "inference frame span is empty after clipping");
        return -1;
    }
    *start_sample = mapped_start;
    *end_sample = mapped_end;
    return 0;
}

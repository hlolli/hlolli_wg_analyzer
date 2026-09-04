#ifndef HWA_INFERENCE_CLOCK_H
#define HWA_INFERENCE_CLOCK_H

#include <stddef.h>
#include <stdint.h>

/*
 * Map one half-open model-frame span to the source sample clock.
 *
 * The model frame rate is frame_rate_numerator / frame_rate_denominator.
 * The start uses floor and the end uses ceiling, then the end is clipped to
 * source_frames. The function rejects an empty or wholly out-of-range span.
 * start_sample and end_sample must point to different objects.
 */
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
    size_t error_size);

/*
 * Start and check one monotonic task deadline. A check fails closed if the
 * host clock cannot be read, moves backwards, or reaches the time limit.
 */
int hwa_inference_deadline_start(uint64_t *started_milliseconds,
                                 char *error,
                                 size_t error_size);

int hwa_inference_deadline_check(uint64_t started_milliseconds,
                                 uint64_t timeout_milliseconds,
                                 char *error,
                                 size_t error_size);

/*
 * Return the whole milliseconds left on one task deadline. A live deadline
 * always returns at least one millisecond so it can feed a child request.
 */
int hwa_inference_deadline_remaining(uint64_t started_milliseconds,
                                     uint64_t timeout_milliseconds,
                                     uint64_t *remaining_milliseconds,
                                     char *error,
                                     size_t error_size);

#endif

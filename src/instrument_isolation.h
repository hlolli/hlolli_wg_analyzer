#ifndef HWA_INSTRUMENT_ISOLATION_H
#define HWA_INSTRUMENT_ISOLATION_H

#include <stddef.h>
#include <stdint.h>

typedef enum HwaIsolationInstrument {
    HWA_ISOLATION_VIOLIN,
    HWA_ISOLATION_VIOLA
} HwaIsolationInstrument;

typedef struct HwaIsolationRange {
    uint64_t start_sample;
    uint64_t end_sample;
} HwaIsolationRange;

/*
 * Estimate the selected solo instrument from finite interleaved mono or stereo
 * samples within float32 range at 8000 through 192000 Hz. Input and output
 * must not overlap.
 * Ranges are ordered, disjoint, half-open intervals in source samples.
 * With ranges, samples outside them pass through. With zero ranges the
 * estimator uses conservative automatic detection across the recording.
 * The estimate keeps the input layout and can peak outside [-1, 1].
 * max_work_bytes limits scratch allocations. Returns zero on success.
 */
int hwa_isolate_instrument(const double *input, size_t frame_count,
                           unsigned channels, unsigned sample_rate_hz,
                           HwaIsolationInstrument instrument,
                           const HwaIsolationRange *ranges,
                           size_t range_count, uint64_t max_work_bytes,
                           double *output, char *error, size_t error_size);

#endif

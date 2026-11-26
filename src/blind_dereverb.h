#ifndef HWA_BLIND_DEREVERB_H
#define HWA_BLIND_DEREVERB_H

#include <stddef.h>
#include <stdint.h>

typedef struct HwaBlindDereverbResult {
    double decay_seconds;
    size_t release_count;
} HwaBlindDereverbResult;

/*
 * Reduce late energy in supported release spans of finite interleaved mono or
 * stereo samples. Estimate the time for a 60 dB decay from the recording.
 * Weak evidence leaves the samples unchanged and returns a zero result.
 */
int hwa_blind_dereverb(double *samples, size_t frames, unsigned channels,
                       uint32_t sample_rate, uint64_t max_work_bytes,
                       HwaBlindDereverbResult *result, char *error, size_t error_size);

#endif

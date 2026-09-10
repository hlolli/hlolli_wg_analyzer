#include "internal.h"
#include "sha256.h"
#include <stdlib.h>
#include <string.h>

typedef struct Snapshot {
    const unsigned char *bytes;
    size_t size;
} Snapshot;

static int snapshot_read(void *context, uint64_t offset, unsigned char *output,
                           size_t count)
{
    const Snapshot *snapshot = (const Snapshot *)context;
    if (offset > snapshot->size || count > snapshot->size - (size_t)offset) return -1;
    memcpy(output, snapshot->bytes + (size_t)offset, count);
    return 0;
}

int hwa_evaluate_separation_wav(const char *reference_path,
                                const char *estimate_path,
                                const char *mixture_path,
                                const HWASeparationEvalOptions *options,
                                HWASeparationEvaluation *result,
                                char *error, size_t error_size)
{
    HWASeparationEvalOptions defaults;
    HWASeparationEvalOptions checked;
    const char *paths[3] = {reference_path, estimate_path, mixture_path};
    double *samples[3] = {NULL, NULL, NULL};
    char hashes[3][HWA_SHA256_HEX_SIZE];
    HWAFormat format;
    uint64_t retained = 0U;
    unsigned count = mixture_path == NULL ? 2U : 3U;
    unsigned signal;
    int status = -1;
    hwa_separation_eval_options_default(&defaults);
    checked = options == NULL ? defaults : *options;
    if (result == NULL) {
        hwa_set_error(error, error_size, "separation evaluation needs a result");
        return -1;
    }
    memset(result, 0, sizeof(*result));
    memset(&format, 0, sizeof(format));
    if (reference_path == NULL || estimate_path == NULL ||
        checked.max_input_bytes == 0U || checked.max_work_bytes == 0U) {
        hwa_set_error(error, error_size, "invalid separation WAVE arguments");
        return -1;
    }
    for (signal = 0U; signal < count; ++signal) {
        unsigned char *bytes = NULL;
        size_t size = 0U;
        Snapshot snapshot;
        HWAByteSource source;
        HWAWavReader reader;
        size_t values, index;
        uint64_t available = checked.max_work_bytes - retained;
        uint64_t cap = checked.max_input_bytes < available ? checked.max_input_bytes : available;
        if (cap == 0U) {
            hwa_set_error(error, error_size, "separation snapshots exceed the work limit");
            goto cleanup;
        }
        if (hwa_sha256_read_file(paths[signal], cap, &bytes, &size, hashes[signal],
                error, error_size) != 0) goto cleanup;
        snapshot.bytes = bytes;
        snapshot.size = size;
        source.context = &snapshot;
        source.name = paths[signal];
        source.size = size;
        source.read_at = snapshot_read;
        if (hwa_wav_reader_open_source(&reader, &source, cap, error, error_size) != 0) {
            free(bytes);
            goto cleanup;
        }
        if (signal == 0U) format = reader.format;
        if (reader.format.frames != format.frames ||
            reader.format.sample_rate_hz != format.sample_rate_hz ||
            reader.format.channels != format.channels ||
            reader.format.channel_mask != format.channel_mask) {
            hwa_set_error(error, error_size, "separation WAVE clocks or channel layouts differ");
            hwa_wav_reader_close(&reader);
            free(bytes);
            goto cleanup;
        }
        if (format.frames == 0U || format.frames > checked.max_frames ||
            format.frames > SIZE_MAX / format.channels / sizeof(double) ||
            format.frames > checked.max_sample_values / count / format.channels) {
            hwa_set_error(error, error_size, "separation WAVE sample count exceeds the limit");
            hwa_wav_reader_close(&reader);
            free(bytes);
            goto cleanup;
        }
        values = (size_t)format.frames * format.channels;
        if (size > available || values * sizeof(double) > available - size) {
            hwa_set_error(error, error_size, "separation decoded samples exceed the work limit");
            hwa_wav_reader_close(&reader);
            free(bytes);
            goto cleanup;
        }
        samples[signal] = (double *)malloc(values * sizeof(double));
        if (samples[signal] == NULL) {
            hwa_set_error(error, error_size, "cannot allocate separation samples");
            hwa_wav_reader_close(&reader);
            free(bytes);
            goto cleanup;
        }
        for (index = 0U; index < values; ++index) {
            int clipped;
            samples[signal][index] = hwa_wav_decode_sample(&reader,
                bytes + (size_t)reader.data_offset + index * reader.bytes_per_sample, &clipped);
        }
        retained += values * sizeof(double);
        hwa_wav_reader_close(&reader);
        free(bytes);
    }
    if (hwa_evaluate_separation_samples(samples[0], samples[1], samples[2],
            (size_t)format.frames, format.channels, &checked, result,
            error, error_size) != 0) goto cleanup;
    result->sample_rate_hz = format.sample_rate_hz;
    result->channel_mask = format.channel_mask;
    memcpy(result->reference_sha256, hashes[0], HWA_SHA256_HEX_SIZE);
    memcpy(result->estimate_sha256, hashes[1], HWA_SHA256_HEX_SIZE);
    if (count == 3U) memcpy(result->mixture_sha256, hashes[2], HWA_SHA256_HEX_SIZE);
    status = 0;
cleanup:
    for (signal = 0U; signal < 3U; ++signal) free(samples[signal]);
    return status;
}

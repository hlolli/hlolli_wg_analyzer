#include "internal.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    HWA_REGRESSION_MAX_INPUT_BYTES = 1024 * 1024,
    HWA_REGRESSION_BLOCK_FRAMES = 64
};

typedef struct HWARegressionSource {
    const uint8_t *data;
    size_t size;
} HWARegressionSource;

static int regression_read_at(void *context, uint64_t offset,
                              unsigned char *destination, size_t size) {
    const HWARegressionSource *source =
        (const HWARegressionSource *)context;
    if (source == NULL || (size != 0U && destination == NULL) ||
        offset > (uint64_t)source->size ||
        (uint64_t)size > (uint64_t)source->size - offset) {
        return -1;
    }
    if (size != 0U) {
        (void)memcpy(destination, source->data + (size_t)offset, size);
    }
    return 0;
}

int hwa_exercise_wav(const uint8_t *data, size_t size,
                             int *accepted,
                             uint64_t *decoded_samples,
                             uint64_t *nonfinite_samples,
                             uint64_t *decoded_checksum);

static void regression_checksum_byte(uint64_t *checksum,
                                     unsigned char value) {
    *checksum ^= (uint64_t)value;
    *checksum *= UINT64_C(1099511628211);
}

int hwa_exercise_wav(const uint8_t *data, size_t size,
                             int *accepted,
                             uint64_t *decoded_samples,
                             uint64_t *nonfinite_samples,
                             uint64_t *decoded_checksum) {
    HWAWavReader reader;
    HWARegressionSource memory;
    HWAByteSource source;
    char error[HWA_ERROR_SIZE];
    unsigned char *raw;
    size_t raw_size;
    if (accepted == NULL || decoded_samples == NULL ||
        nonfinite_samples == NULL || decoded_checksum == NULL ||
        (data == NULL && size != 0U) ||
        size > HWA_REGRESSION_MAX_INPUT_BYTES) {
        return -1;
    }
    *accepted = 0;
    *decoded_samples = 0U;
    *nonfinite_samples = 0U;
    *decoded_checksum = UINT64_C(14695981039346656037);
    memory.data = data;
    memory.size = size;
    source.context = &memory;
    source.name = "regression-input";
    source.size = (uint64_t)size;
    source.read_at = regression_read_at;
    if (hwa_wav_reader_open_source(&reader, &source,
                                   HWA_REGRESSION_MAX_INPUT_BYTES,
                                   error, sizeof(error)) != 0) {
        return 0;
    }
    *accepted = 1;
    if (reader.format.block_align == 0U) {
        hwa_wav_reader_close(&reader);
        return 0;
    }
    raw_size = (size_t)reader.format.block_align * HWA_REGRESSION_BLOCK_FRAMES;
    raw = (unsigned char *)malloc(raw_size);
    if (raw == NULL) {
        hwa_wav_reader_close(&reader);
        return -1;
    }

    for (;;) {
        size_t frame_count = 0U;
        uint32_t channel;
        size_t frame;

        if (hwa_wav_reader_read_frames(&reader, raw,
                                       HWA_REGRESSION_BLOCK_FRAMES,
                                       &frame_count, error,
                                       sizeof(error)) != 0) {
            free(raw);
            hwa_wav_reader_close(&reader);
            return -1;
        }
        for (frame = 0U; frame < frame_count; ++frame) {
            const unsigned char *frame_data =
                raw + frame * (size_t)reader.format.block_align;
            for (channel = 0U; channel < reader.format.channels; ++channel) {
                int clipped = 0;
                const unsigned char *sample_data =
                    frame_data + (size_t)channel * reader.bytes_per_sample;
                const double decoded =
                    hwa_wav_decode_sample(&reader, sample_data, &clipped);
                uint64_t bits = UINT64_C(0x7ff8000000000000);
                unsigned shift;
                if (isfinite(decoded)) {
                    memcpy(&bits, &decoded, sizeof(bits));
                } else {
                    ++*nonfinite_samples;
                }
                for (shift = 0U; shift < 64U; shift += 8U) {
                    regression_checksum_byte(
                        decoded_checksum,
                        (unsigned char)((bits >> shift) & UINT64_C(0xff)));
                }
                regression_checksum_byte(decoded_checksum,
                                         (unsigned char)(clipped != 0));
                ++*decoded_samples;
            }
        }
        if (frame_count == 0U) {
            break;
        }
    }

    free(raw);
    hwa_wav_reader_close(&reader);
    return 0;
}

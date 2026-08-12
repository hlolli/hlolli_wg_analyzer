#include "fuzz_support.h"

#include "internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    HWAFuzzBytes bytes;
    HWAByteSource source;
    HWAWavReader reader;
    unsigned char *buffer = NULL;
    char error[HWA_ERROR_SIZE];
    volatile double sink = 0.0;
    if ((uint64_t)size > HWA_FUZZ_MAX_INPUT_BYTES) return 0;
    bytes.data = data;
    bytes.size = size;
    source.context = &bytes;
    source.name = "fuzz.wav";
    source.size = (uint64_t)size;
    source.read_at = hwa_fuzz_read_at;
    if (hwa_wav_reader_open_source(&reader, &source,
                                   HWA_FUZZ_MAX_INPUT_BYTES,
                                   error, sizeof(error)) != 0)
        return 0;
    if (reader.format.block_align != 0U && reader.bytes_per_sample != 0U) {
        size_t frames_read = 0U;
        size_t bytes_per_block = (size_t)reader.format.block_align * 64U;
        buffer = (unsigned char *)malloc(bytes_per_block);
        if (buffer != NULL) {
            do {
                size_t frame;
                if (hwa_wav_reader_read_frames(
                        &reader, buffer, 64U, &frames_read,
                        error, sizeof(error)) != 0)
                    break;
                for (frame = 0U; frame < frames_read; ++frame) {
                    size_t byte_index;
                    for (byte_index = 0U;
                         byte_index < reader.format.block_align;
                         byte_index += reader.bytes_per_sample) {
                        int clipped = 0;
                        sink += hwa_wav_decode_sample(
                            &reader,
                            buffer + frame *
                                (size_t)reader.format.block_align +
                                byte_index,
                            &clipped);
                        sink += clipped ? 1.0 : 0.0;
                    }
                }
            } while (frames_read != 0U);
        }
    }
    free(buffer);
    hwa_wav_reader_close(&reader);
    (void)sink;
    return 0;
}

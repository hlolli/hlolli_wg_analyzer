#include "speech_span_audio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int hwa_speech_charge_transforms(HWASpeechWork *work, uint64_t count,
                                 char *error, size_t error_size)
{
    if (count > work->max_transforms - work->transforms) {
        hwa_set_error(error, error_size, "speech comparison exceeds the transform limit");
        return -1;
    }
    work->transforms += count;
    return 0;
}

static void speech_u16(unsigned char *out, uint16_t value)
{
    out[0] = (unsigned char)value;
    out[1] = (unsigned char)(value >> 8U);
}

static void speech_u32(unsigned char *out, uint32_t value)
{
    speech_u16(out, (uint16_t)value);
    speech_u16(out + 2U, (uint16_t)(value >> 16U));
}

static int speech_audio_scan(const HWAWavReader *reader, HwaPairedSampleSpan span,
                              HWASpeechWork *work, HWASpeechAudioLevel *level,
                              double *mono, FILE *pcm, double gain,
                              char *error, size_t error_size)
{
    HWAWavReader slice = *reader;
    uint64_t count = span.end_sample - span.start_sample;
    unsigned channels = reader->format.channels;
    size_t input_stride = reader->format.block_align;
    size_t output_stride = pcm != NULL ? channels * 2U : 0U;
    size_t buffer_stride = input_stride + output_stride;
    uint64_t capacity = work->available_bytes / buffer_stride;
    uint64_t index = 0U;
    double power = 0.0, peak = 0.0;
    unsigned char *block, *encoded;
    int status = -1;

    if (count > work->max_frames - work->decoded_frames) {
        hwa_set_error(error, error_size, "speech comparison exceeds the decoded frame limit");
        return -1;
    }
    if (count == 0U || capacity == 0U) {
        hwa_set_error(error, error_size, "cannot allocate a speech audio block");
        return -1;
    }
    if (capacity > 1024U) capacity = 1024U;
    if (capacity > count) capacity = count;
    block = malloc((size_t)capacity * buffer_stride);
    if (block == NULL) {
        hwa_set_error(error, error_size, "cannot allocate a speech audio block");
        return -1;
    }
    encoded = block + (size_t)capacity * input_stride;
    slice.cursor = slice.data_offset + span.start_sample * input_stride;
    slice.bytes_remaining = count * input_stride;
    while (index < count) {
        size_t read_count, frame;
        if (hwa_wav_reader_read_frames(&slice, block, (size_t)capacity, &read_count,
                                      error, error_size) != 0) goto done;
        if (read_count == 0U) {
            hwa_set_error(error, error_size, "short WAVE read in speech span");
            goto done;
        }
        for (frame = 0U; frame < read_count; ++frame) {
            double mix = 0.0;
            unsigned channel;
            for (channel = 0U; channel < channels; ++channel) {
                int clipped;
                double sample = hwa_wav_decode_sample(reader, block + frame * input_stride +
                    channel * reader->bytes_per_sample, &clipped);
                if (!isfinite(sample) || fabs(sample) > 1e100) {
                    hwa_set_error(error, error_size, "unsupported sample value in speech audio");
                    goto done;
                }
                if (level != NULL) {
                    power += sample * sample;
                    peak = fmax(peak, fabs(sample));
                }
                if (mono != NULL) mix += sample;
                if (pcm != NULL) {
                    double scaled = fmin(32767.0, fmax(-32768.0, sample * gain * 32768.0));
                    uint16_t value = (uint16_t)lround(scaled);
                    speech_u16(encoded + frame * output_stride + channel * 2U, value);
                }
            }
            if (mono != NULL) mono[index + frame] = mix / (double)channels;
        }
        if (pcm != NULL && fwrite(encoded, output_stride, read_count, pcm) != read_count) {
            hwa_set_error(error, error_size, "cannot write speech excerpt samples");
            goto done;
        }
        index += read_count;
    }
    if (level != NULL) {
        level->rms = sqrt(power / ((double)count * (double)channels));
        level->peak = peak;
    }
    work->decoded_frames += count;
    status = 0;
done:
    free(block);
    return status;
}

int hwa_speech_audio_level(const HWAWavReader *reader, HwaPairedSampleSpan span,
                            HWASpeechWork *work, HWASpeechAudioLevel *level,
                            char *error, size_t error_size)
{
    return speech_audio_scan(reader, span, work, level, NULL, NULL, 1.0, error, error_size);
}

int hwa_speech_audio_mono(const HWAWavReader *reader, HwaPairedSampleSpan span,
                           HWASpeechWork *work, double *samples, char *error, size_t error_size)
{
    return speech_audio_scan(reader, span, work, NULL, samples, NULL, 1.0, error, error_size);
}

int hwa_speech_audio_write(FILE *stream, const HWAWavReader *reader, HwaPairedSampleSpan span,
                            double gain, HWASpeechWork *work, char *error, size_t error_size)
{
    unsigned char header[44] = {0};
    uint64_t frames = span.end_sample - span.start_sample;
    uint32_t block_align = (uint32_t)reader->format.channels * 2U;
    uint32_t bytes;

    if (frames > (UINT32_MAX - 36U) / block_align ||
        reader->format.sample_rate_hz > UINT32_MAX / block_align) {
        hwa_set_error(error, error_size, "speech excerpt exceeds the PCM WAVE size limit");
        return -1;
    }
    bytes = (uint32_t)frames * block_align;
    memcpy(header, "RIFF", 4U);
    speech_u32(header + 4U, bytes + 36U);
    memcpy(header + 8U, "WAVEfmt ", 8U);
    speech_u32(header + 16U, 16U);
    speech_u16(header + 20U, 1U);
    speech_u16(header + 22U, reader->format.channels);
    speech_u32(header + 24U, reader->format.sample_rate_hz);
    speech_u32(header + 28U, reader->format.sample_rate_hz * block_align);
    speech_u16(header + 32U, (uint16_t)block_align);
    speech_u16(header + 34U, 16U);
    memcpy(header + 36U, "data", 4U);
    speech_u32(header + 40U, bytes);
    if (fwrite(header, 1U, sizeof(header), stream) != sizeof(header)) {
        hwa_set_error(error, error_size, "cannot write speech excerpt header");
        return -1;
    }
    return speech_audio_scan(reader, span, work, NULL, NULL, stream, gain, error, error_size);
}

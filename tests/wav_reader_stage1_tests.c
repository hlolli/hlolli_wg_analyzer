#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "internal.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

typedef struct TestBytes {
    unsigned char data[1024];
    size_t size;
} TestBytes;

static int failures = 0;

static void check(int condition, const char *message)
{
    if (!condition) {
        (void)fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static void bytes_put(TestBytes *bytes, const void *data, size_t size)
{
    if (bytes->size + size > sizeof(bytes->data)) {
        (void)fprintf(stderr, "fixture buffer overflow\n");
        exit(2);
    }
    memcpy(bytes->data + bytes->size, data, size);
    bytes->size += size;
}

static void bytes_u16(TestBytes *bytes, uint16_t value)
{
    unsigned char data[2];

    data[0] = (unsigned char)(value & 0xffU);
    data[1] = (unsigned char)((value >> 8U) & 0xffU);
    bytes_put(bytes, data, sizeof(data));
}

static void bytes_u32(TestBytes *bytes, uint32_t value)
{
    unsigned char data[4];

    data[0] = (unsigned char)(value & 0xffU);
    data[1] = (unsigned char)((value >> 8U) & 0xffU);
    data[2] = (unsigned char)((value >> 16U) & 0xffU);
    data[3] = (unsigned char)((value >> 24U) & 0xffU);
    bytes_put(bytes, data, sizeof(data));
}

static void bytes_u64(TestBytes *bytes, uint64_t value)
{
    unsigned shift;

    for (shift = 0U; shift < 64U; shift += 8U) {
        unsigned char byte = (unsigned char)((value >> shift) & 0xffU);
        bytes_put(bytes, &byte, 1U);
    }
}

static void patch_u32(TestBytes *bytes, size_t offset, uint32_t value)
{
    unsigned index;

    for (index = 0U; index < 4U; ++index) {
        bytes->data[offset + index] =
            (unsigned char)((value >> (index * 8U)) & 0xffU);
    }
}

static void patch_u64(TestBytes *bytes, size_t offset, uint64_t value)
{
    unsigned index;

    for (index = 0U; index < 8U; ++index) {
        bytes->data[offset + index] =
            (unsigned char)((value >> (index * 8U)) & 0xffU);
    }
}

static size_t find_id(const TestBytes *bytes,
                      const char id[4],
                      size_t start)
{
    size_t offset;

    for (offset = start; offset + 4U <= bytes->size; ++offset) {
        if (memcmp(bytes->data + offset, id, 4U) == 0) {
            return offset;
        }
    }
    return SIZE_MAX;
}

static void riff_begin(TestBytes *bytes)
{
    memset(bytes, 0, sizeof(*bytes));
    bytes_put(bytes, "RIFF", 4U);
    bytes_u32(bytes, 0U);
    bytes_put(bytes, "WAVE", 4U);
}

static void riff_finish(TestBytes *bytes)
{
    patch_u32(bytes, 4U, (uint32_t)(bytes->size - 8U));
}

static void chunk(TestBytes *bytes,
                  const char id[4],
                  const unsigned char *payload,
                  size_t size)
{
    static const unsigned char padding = 0U;

    bytes_put(bytes, id, 4U);
    bytes_u32(bytes, (uint32_t)size);
    bytes_put(bytes, payload, size);
    if ((size & 1U) != 0U) {
        bytes_put(bytes, &padding, 1U);
    }
}

static void pcm_format(TestBytes *format,
                       uint16_t tag,
                       uint16_t channels,
                       uint32_t rate,
                       uint16_t bits)
{
    uint16_t align = (uint16_t)(channels * (bits / 8U));

    memset(format, 0, sizeof(*format));
    bytes_u16(format, tag);
    bytes_u16(format, channels);
    bytes_u32(format, rate);
    bytes_u32(format, rate * align);
    bytes_u16(format, align);
    bytes_u16(format, bits);
}

static int write_temp(const TestBytes *bytes, char *path, size_t path_size)
{
    FILE *file;

#if defined(_WIN32)
    if (tmpnam_s(path, path_size) != 0) {
        return -1;
    }
    file = fopen(path, "wb");
#else
    int descriptor;
    const char pattern[] = "/tmp/hwa-reader-stage1-XXXXXX";

    if (path_size < sizeof(pattern)) {
        return -1;
    }
    memcpy(path, pattern, sizeof(pattern));
    descriptor = mkstemp(path);
    if (descriptor < 0) {
        return -1;
    }
    file = fdopen(descriptor, "wb");
    if (file == NULL) {
        (void)close(descriptor);
        (void)remove(path);
        return -1;
    }
#endif
    if (file == NULL) {
        (void)remove(path);
        return -1;
    }
    if (fwrite(bytes->data, 1U, bytes->size, file) != bytes->size) {
        (void)fclose(file);
        (void)remove(path);
        return -1;
    }
    if (fclose(file) != 0) {
        (void)remove(path);
        return -1;
    }
    return 0;
}

static void test_pcm8_odd_chunks(void)
{
    static const unsigned char junk[] = {1U, 2U, 3U};
    static const unsigned char samples[] = {0U, 128U, 255U};
    TestBytes wave;
    TestBytes format;
    HWAWavReader reader;
    unsigned char frame_data[3];
    size_t frames_read = 0U;
    char path[128];
    char error[256];
    int clipped;

    riff_begin(&wave);
    chunk(&wave, "JUNK", junk, sizeof(junk));
    pcm_format(&format, 1U, 1U, 8000U, 8U);
    chunk(&wave, "fmt ", format.data, format.size);
    chunk(&wave, "data", samples, sizeof(samples));
    riff_finish(&wave);

    check(write_temp(&wave, path, sizeof(path)) == 0,
          "write PCM8 fixture");
    check(hwa_wav_reader_open(&reader, path, 0U,
                              error, sizeof(error)) == 0,
          "open PCM8 with odd unknown and data chunks");
    if (reader.file != NULL) {
        check(reader.format.container == HWA_CONTAINER_RIFF,
              "report RIFF container");
        check(reader.format.bits_per_sample == 8U &&
                  reader.format.valid_bits_per_sample == 8U,
              "report PCM8 sample size");
        check(reader.format.frames == 3U,
              "report PCM8 frame count");
        check(hwa_wav_reader_read_frames(&reader, frame_data, 3U,
                                         &frames_read, error,
                                         sizeof(error)) == 0 &&
                  frames_read == 3U,
              "read PCM8 frames");
        check(hwa_wav_decode_sample(&reader, frame_data, &clipped) == -1.0 &&
                  clipped,
              "decode unsigned PCM8 minimum");
        check(hwa_wav_decode_sample(&reader, frame_data + 1U, &clipped) == 0.0 &&
                  !clipped,
              "decode unsigned PCM8 midpoint");
        check(fabs(hwa_wav_decode_sample(&reader, frame_data + 2U, &clipped) -
                       (127.0 / 128.0)) < 1e-12 && clipped,
              "decode unsigned PCM8 maximum");
        hwa_wav_reader_close(&reader);
    }
    (void)remove(path);
}

static void test_extensible_valid_bits(void)
{
    static const unsigned char guid_tail[12] = {
        0x00U, 0x00U, 0x10U, 0x00U,
        0x80U, 0x00U, 0x00U, 0xaaU,
        0x00U, 0x38U, 0x9bU, 0x71U
    };
    static const unsigned char samples[6] = {
        0xf0U, 0xffU, 0x7fU, 0x00U, 0x00U, 0x80U
    };
    TestBytes wave;
    TestBytes format;
    HWAWavReader reader;
    unsigned char frame_data[6];
    size_t frames_read = 0U;
    char path[128];
    char error[256];
    int clipped;
    double value;

    memset(&format, 0, sizeof(format));
    bytes_u16(&format, 0xfffeU);
    bytes_u16(&format, 2U);
    bytes_u32(&format, 48000U);
    bytes_u32(&format, 288000U);
    bytes_u16(&format, 6U);
    bytes_u16(&format, 24U);
    bytes_u16(&format, 22U);
    bytes_u16(&format, 20U);
    bytes_u32(&format, 3U);
    bytes_u32(&format, 1U);
    bytes_put(&format, guid_tail, sizeof(guid_tail));

    riff_begin(&wave);
    chunk(&wave, "fmt ", format.data, format.size);
    chunk(&wave, "data", samples, sizeof(samples));
    riff_finish(&wave);
    check(write_temp(&wave, path, sizeof(path)) == 0,
          "write extensible fixture");
    check(hwa_wav_reader_open(&reader, path, 0U,
                              error, sizeof(error)) == 0,
          "open extensible PCM fixture");
    if (reader.file != NULL) {
        check(reader.format.valid_bits_per_sample == 20U,
              "keep extensible valid bit count");
        check(reader.format.channel_mask == 3U,
              "keep extensible channel mask");
        check(hwa_wav_reader_read_frames(&reader, frame_data, 1U,
                                         &frames_read, error,
                                         sizeof(error)) == 0 &&
                  frames_read == 1U,
              "read extensible frame");
        value = hwa_wav_decode_sample(&reader, frame_data, &clipped);
        check(fabs(value - (524287.0 / 524288.0)) < 1e-12 && clipped,
              "decode left-aligned 20-bit PCM maximum");
        value = hwa_wav_decode_sample(&reader, frame_data + 3U, &clipped);
        check(value == -1.0 && clipped,
              "decode left-aligned 20-bit PCM minimum");
        hwa_wav_reader_close(&reader);
    }
    (void)remove(path);
}

static void test_float64(void)
{
    TestBytes wave;
    TestBytes format;
    TestBytes samples;
    HWAWavReader reader;
    unsigned char frame_data[8];
    size_t frames_read = 0U;
    char path[128];
    char error[256];
    int clipped;

    pcm_format(&format, 3U, 1U, 44100U, 64U);
    memset(&samples, 0, sizeof(samples));
    bytes_u64(&samples, UINT64_C(0x3fe0000000000000));
    riff_begin(&wave);
    chunk(&wave, "fmt ", format.data, format.size);
    chunk(&wave, "data", samples.data, samples.size);
    riff_finish(&wave);
    check(write_temp(&wave, path, sizeof(path)) == 0,
          "write float64 fixture");
    check(hwa_wav_reader_open(&reader, path, 0U,
                              error, sizeof(error)) == 0,
          "open float64 fixture");
    if (reader.file != NULL) {
        check(hwa_wav_reader_read_frames(&reader, frame_data, 1U,
                                         &frames_read, error,
                                         sizeof(error)) == 0 &&
                  frames_read == 1U,
              "read float64 frame");
        check(hwa_wav_decode_sample(&reader, frame_data, &clipped) == 0.5 &&
                  !clipped,
              "decode IEEE binary64");
        hwa_wav_reader_close(&reader);
    }
    (void)remove(path);
}

static void make_rf64(TestBytes *wave)
{
    TestBytes format;
    static const unsigned char junk[3] = {7U, 8U, 9U};
    static const unsigned char samples[4] = {0U, 0U, 0xffU, 0x7fU};
    static const unsigned char padding = 0U;

    memset(wave, 0, sizeof(*wave));
    bytes_put(wave, "RF64", 4U);
    bytes_u32(wave, UINT32_MAX);
    bytes_put(wave, "WAVE", 4U);
    bytes_put(wave, "ds64", 4U);
    bytes_u32(wave, 40U);
    bytes_u64(wave, 0U);
    bytes_u64(wave, sizeof(samples));
    bytes_u64(wave, 2U);
    bytes_u32(wave, 1U);
    bytes_put(wave, "JUNK", 4U);
    bytes_u64(wave, sizeof(junk));
    bytes_put(wave, "JUNK", 4U);
    bytes_u32(wave, UINT32_MAX);
    bytes_put(wave, junk, sizeof(junk));
    bytes_put(wave, &padding, 1U);
    pcm_format(&format, 1U, 1U, 8000U, 16U);
    chunk(wave, "fmt ", format.data, format.size);
    bytes_put(wave, "data", 4U);
    bytes_u32(wave, UINT32_MAX);
    bytes_put(wave, samples, sizeof(samples));
    patch_u64(wave, 20U, (uint64_t)wave->size - 8U);
}

static void test_rf64(void)
{
    TestBytes wave;
    HWAWavReader reader;
    char path[128];
    char error[256];
    size_t data_offset;

    make_rf64(&wave);
    check(write_temp(&wave, path, sizeof(path)) == 0,
          "write RF64 fixture");
    check(hwa_wav_reader_open(&reader, path, 0U,
                              error, sizeof(error)) == 0,
          "open RF64 with ds64 table entry");
    if (reader.file != NULL) {
        check(reader.format.container == HWA_CONTAINER_RF64,
              "report RF64 container");
        check(reader.format.data_bytes == 4U && reader.format.frames == 2U,
              "use RF64 64-bit data size");
        hwa_wav_reader_close(&reader);
    }
    (void)remove(path);

    patch_u32(&wave, 4U, 0U);
    check(write_temp(&wave, path, sizeof(path)) == 0,
          "write bad RF64 fixture");
    memset(&reader, 0, sizeof(reader));
    check(hwa_wav_reader_open(&reader, path, 0U,
                              error, sizeof(error)) != 0 &&
              strstr(error, "sentinel") != NULL,
          "reject RF64 without header size sentinel");
    hwa_wav_reader_close(&reader);
    (void)remove(path);

    make_rf64(&wave);
    memcpy(wave.data + 12U, "JUNK", 4U);
    check(write_temp(&wave, path, sizeof(path)) == 0,
          "write RF64 without ds64 fixture");
    memset(&reader, 0, sizeof(reader));
    check(hwa_wav_reader_open(&reader, path, 0U,
                              error, sizeof(error)) != 0 &&
              strstr(error, "ds64") != NULL,
          "reject RF64 without leading ds64");
    hwa_wav_reader_close(&reader);
    (void)remove(path);

    make_rf64(&wave);
    data_offset = find_id(&wave, "data", 60U);
    check(data_offset != SIZE_MAX,
          "find RF64 data header in fixture");
    if (data_offset != SIZE_MAX) {
        patch_u32(&wave, data_offset + 4U, 4U);
    }
    check(write_temp(&wave, path, sizeof(path)) == 0,
          "write RF64 data without sentinel fixture");
    memset(&reader, 0, sizeof(reader));
    check(hwa_wav_reader_open(&reader, path, 0U,
                              error, sizeof(error)) != 0 &&
              strstr(error, "sentinel") != NULL,
          "reject RF64 data without size sentinel");
    hwa_wav_reader_close(&reader);
    (void)remove(path);

    make_rf64(&wave);
    patch_u32(&wave, 44U, 2U);
    check(write_temp(&wave, path, sizeof(path)) == 0,
          "write RF64 with oversized ds64 table fixture");
    memset(&reader, 0, sizeof(reader));
    check(hwa_wav_reader_open(&reader, path, 0U,
                              error, sizeof(error)) != 0 &&
              strstr(error, "table") != NULL,
          "reject ds64 table beyond its chunk");
    hwa_wav_reader_close(&reader);
    (void)remove(path);
}

static void test_size_bound_and_stdin(void)
{
    static const unsigned char sample[2] = {0U, 0U};
    TestBytes wave;
    TestBytes format;
    HWAWavReader reader;
    char path[128];
    char error[256];

    riff_begin(&wave);
    pcm_format(&format, 1U, 1U, 8000U, 16U);
    chunk(&wave, "fmt ", format.data, format.size);
    chunk(&wave, "data", sample, sizeof(sample));
    riff_finish(&wave);
    check(write_temp(&wave, path, sizeof(path)) == 0,
          "write bounded input fixture");
    memset(&reader, 0, sizeof(reader));
    check(hwa_wav_reader_open(&reader, path, (uint64_t)wave.size - 1U,
                              error, sizeof(error)) != 0 &&
              strstr(error, "byte limit") != NULL,
          "reject regular file over its byte bound");
    hwa_wav_reader_close(&reader);

#if defined(_WIN32)
    {
        FILE *redirected = NULL;
        check(freopen_s(&redirected, path, "rb", stdin) == 0,
              "redirect stdin to fixture");
    }
#else
    check(freopen(path, "rb", stdin) != NULL,
          "redirect stdin to fixture");
#endif
    memset(&reader, 0, sizeof(reader));
    check(hwa_wav_reader_open(&reader, "-", (uint64_t)wave.size,
                              error, sizeof(error)) == 0,
          "spool bounded standard input");
    if (reader.file != NULL) {
        check(reader.format.frames == 1U,
              "parse spooled standard input");
        hwa_wav_reader_close(&reader);
    }
    (void)remove(path);
}

int main(void)
{
    test_pcm8_odd_chunks();
    test_extensible_valid_bits();
    test_float64();
    test_rf64();
    test_size_bound_and_stdin();

    if (failures != 0) {
        (void)fprintf(stderr, "%d WAVE reader test(s) failed\n", failures);
        return 1;
    }
    (void)puts("WAVE reader Stage 1 tests passed");
    return 0;
}

#if !defined(_WIN32) && !defined(HWA_WASM_REACTOR)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "hlolli_wg_analyzer.h"

#include <math.h>
#include <stdint.h>
#if !defined(HWA_WASM_REACTOR)
#include <stdio.h>
#endif
#include <stdlib.h>
#include <string.h>

#if !defined(HWA_WASM_REACTOR)
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif
#endif

#define TEST_FRAME_COUNT 1024U
#define TEST_WAVE_SIZE (44U + TEST_FRAME_COUNT * 2U)

typedef struct TestSource {
    unsigned char bytes[TEST_WAVE_SIZE];
    size_t size;
    size_t calls;
    size_t bytes_read;
    size_t fragment_size;
    size_t fail_call;
    int bad_span;
} TestSource;

static int failures;

#if defined(HWA_WASM_REACTOR)
#define CHECK(condition)                 \
    do {                                 \
        if (!(condition)) failures++;    \
    } while (0)
#else
#define CHECK(condition)                                                   \
    do {                                                                   \
        if (!(condition)) {                                                \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",          \
                          __FILE__, __LINE__, #condition);                  \
            failures++;                                                    \
        }                                                                  \
    } while (0)
#endif

static void put_u16(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8U);
}

static void put_u32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static void make_wave(TestSource *source)
{
    size_t frame;
    memset(source, 0, sizeof(*source));
    source->size = TEST_WAVE_SIZE;
    source->fragment_size = TEST_WAVE_SIZE;
    memcpy(source->bytes, "RIFF", 4U);
    put_u32(source->bytes + 4U, (uint32_t)source->size - 8U);
    memcpy(source->bytes + 8U, "WAVEfmt ", 8U);
    put_u32(source->bytes + 16U, 16U);
    put_u16(source->bytes + 20U, 1U);
    put_u16(source->bytes + 22U, 1U);
    put_u32(source->bytes + 24U, 8000U);
    put_u32(source->bytes + 28U, 16000U);
    put_u16(source->bytes + 32U, 2U);
    put_u16(source->bytes + 34U, 16U);
    memcpy(source->bytes + 36U, "data", 4U);
    put_u32(source->bytes + 40U, TEST_FRAME_COUNT * 2U);
    for (frame = 0U; frame < TEST_FRAME_COUNT; ++frame) {
        double phase = 6.283185307179586476925286766559 *
                       250.0 * (double)frame / 8000.0;
        int16_t sample = (int16_t)(sin(phase) * 8192.0);
        put_u16(source->bytes + 44U + frame * 2U, (uint16_t)sample);
    }
}

static int read_at(void *context,
                   uint64_t offset,
                   unsigned char *destination,
                   size_t size)
{
    TestSource *source = (TestSource *)context;
    size_t copied = 0U;
    source->calls++;
    if (source->fail_call != 0U && source->calls == source->fail_call) return -1;
    if (offset > (uint64_t)source->size ||
        (uint64_t)size > (uint64_t)source->size - offset) {
        source->bad_span = 1;
        return -1;
    }
    while (copied < size) {
        size_t remaining = size - copied;
        size_t part = remaining < source->fragment_size
                          ? remaining : source->fragment_size;
        memcpy(destination + copied,
               source->bytes + (size_t)offset + copied, part);
        copied += part;
    }
    source->bytes_read += size;
    return 0;
}

static HWAAnalysisOptions test_options(void)
{
    HWAAnalysisOptions options;
    hwa_analysis_options_default(&options);
    options.decode_block_frames = 37U;
    options.frame_size = 256U;
    options.hop_size = 64U;
    options.max_input_bytes = TEST_WAVE_SIZE;
    options.max_input_frames = TEST_FRAME_COUNT;
    options.max_work_bytes = UINT64_C(16777216);
    options.max_transforms = 1000U;
    options.max_track_points = 1000U;
    options.max_spectrum_values = 100000U;
    options.true_peak_oversample = 1U;
    options.collect_tracks = 1;
    return options;
}

static void test_source_analysis(void)
{
    TestSource source;
    HWAByteSource bytes;
    HWAAnalysisOptions options = test_options();
    HWAAnalysis analysis;
    char error[HWA_ERROR_SIZE];
    make_wave(&source);
    source.fragment_size = 1U;
    bytes.context = &source;
    bytes.name = "worker-input.wav";
    bytes.size = source.size;
    bytes.read_at = read_at;
    CHECK(hwa_analyze_wav_source(&bytes, &options, &analysis,
                                 error, sizeof(error)) == 0);
    CHECK(strcmp(analysis.path, "worker-input.wav") == 0);
    CHECK(analysis.format.frames == TEST_FRAME_COUNT);
    CHECK(analysis.format.sample_rate_hz == 8000U);
    CHECK(analysis.analyzed_channels == 1U);
    CHECK(analysis.track_count > 0U);
    CHECK(analysis.spectrum.valid != 0);
    CHECK(analysis.spectrum.transform_count > 0U);
    CHECK(analysis.channels != NULL && analysis.channels[0].peak > 0.24 &&
          analysis.channels[0].peak < 0.26);
    CHECK(source.calls > 20U && source.bytes_read < source.size * 2U);
    CHECK(source.bad_span == 0);
    hwa_analysis_free(&analysis);
}

static void test_limits_and_failure(void)
{
    TestSource source;
    HWAByteSource bytes;
    HWAAnalysisOptions options = test_options();
    HWAAnalysis analysis;
    char error[HWA_ERROR_SIZE];
    make_wave(&source);
    bytes.context = &source;
    bytes.name = "bounded.wav";
    bytes.size = source.size;
    bytes.read_at = read_at;
    options.max_input_bytes--;
    CHECK(hwa_analyze_wav_source(&bytes, &options, &analysis,
                                 error, sizeof(error)) != 0);
    CHECK(source.calls == 0U);
    CHECK(analysis.path == NULL && analysis.channels == NULL);

    options.max_input_bytes++;
    source.fail_call = 2U;
    CHECK(hwa_analyze_wav_source(&bytes, &options, &analysis,
                                 error, sizeof(error)) != 0);
    CHECK(source.bad_span == 0);
    CHECK(analysis.path == NULL && analysis.channels == NULL);
}

static void test_virtual_long_source(void)
{
    TestSource source;
    HWAByteSource bytes;
    HWAAnalysisOptions options = test_options();
    HWAAnalysis analysis;
    char error[HWA_ERROR_SIZE];
    make_wave(&source);
    bytes.context = &source;
    bytes.name = "virtual-long.wav";
    bytes.size = UINT64_MAX;
    bytes.read_at = read_at;
    CHECK(hwa_analyze_wav_source(&bytes, &options, &analysis,
                                 error, sizeof(error)) != 0);
    CHECK(source.calls == 0U);
    CHECK(source.bad_span == 0);
}

#if !defined(HWA_WASM_REACTOR)
static int write_wave_file(const TestSource *source,
                           char *path,
                           size_t path_size)
{
#if defined(_WIN32)
    char directory[MAX_PATH + 1U];
    DWORD length;
    FILE *file = NULL;
    if (path_size < MAX_PATH + 1U) return -1;
    length = GetTempPathA(MAX_PATH + 1U, directory);
    if (length == 0U || length > MAX_PATH ||
        GetTempFileNameA(directory, "hwa", 0U, path) == 0U ||
        fopen_s(&file, path, "wb") != 0 || file == NULL) return -1;
    if (fwrite(source->bytes, 1U, source->size, file) != source->size ||
        fclose(file) != 0) {
        (void)remove(path);
        return -1;
    }
#else
    static const char pattern[] = "/tmp/hwa-portability-parity-XXXXXX";
    int descriptor;
    size_t written = 0U;
    if (path_size < sizeof(pattern)) return -1;
    memcpy(path, pattern, sizeof(pattern));
    descriptor = mkstemp(path);
    if (descriptor < 0) return -1;
    while (written < source->size) {
        ssize_t count = write(descriptor, source->bytes + written,
                              source->size - written);
        if (count <= 0) {
            (void)close(descriptor);
            (void)remove(path);
            return -1;
        }
        written += (size_t)count;
    }
    if (close(descriptor) != 0) {
        (void)remove(path);
        return -1;
    }
#endif
    return 0;
}

static int analyses_match(const HWAAnalysis *left, const HWAAnalysis *right)
{
    size_t channel_bytes;
    size_t track_bytes;
    size_t spectrum_bytes;
    if (strcmp(left->path, right->path) != 0 ||
        memcmp(&left->format, &right->format, sizeof(left->format)) != 0 ||
        memcmp(&left->options, &right->options, sizeof(left->options)) != 0 ||
        left->analyzed_channels != right->analyzed_channels ||
        left->track_count != right->track_count ||
        left->spectrum_bins != right->spectrum_bins ||
        memcmp(&left->loudness, &right->loudness,
               sizeof(left->loudness)) != 0 ||
        memcmp(&left->spectrum, &right->spectrum,
               sizeof(left->spectrum)) != 0 ||
        memcmp(&left->activity, &right->activity,
               sizeof(left->activity)) != 0 ||
        memcmp(&left->stereo, &right->stereo, sizeof(left->stereo)) != 0) {
        return 0;
    }
    channel_bytes = (size_t)left->analyzed_channels * sizeof(*left->channels);
    track_bytes = left->track_count * sizeof(*left->tracks);
    spectrum_bytes = left->track_count * left->spectrum_bins *
                     sizeof(*left->spectrogram_db);
    return memcmp(left->channels, right->channels, channel_bytes) == 0 &&
           memcmp(left->tracks, right->tracks, track_bytes) == 0 &&
           memcmp(left->spectrogram_db, right->spectrogram_db,
                  spectrum_bytes) == 0;
}

static void test_native_path_parity(void)
{
    TestSource source;
    HWAByteSource bytes;
    HWAAnalysisOptions options = test_options();
    HWAAnalysis source_analysis;
    HWAAnalysis path_analysis;
    char path[512];
    char error[HWA_ERROR_SIZE];
    make_wave(&source);
    options.collect_spectrogram = 1;
    if (write_wave_file(&source, path, sizeof(path)) != 0) {
        CHECK(0);
        return;
    }
    bytes.context = &source;
    bytes.name = path;
    bytes.size = source.size;
    bytes.read_at = read_at;
    CHECK(hwa_analyze_wav_source(&bytes, &options, &source_analysis,
                                 error, sizeof(error)) == 0);
    CHECK(hwa_analyze_wav_with_options(path, &options, &path_analysis,
                                       error, sizeof(error)) == 0);
    CHECK(analyses_match(&source_analysis, &path_analysis));
    hwa_analysis_free(&source_analysis);
    hwa_analysis_free(&path_analysis);
    CHECK(remove(path) == 0);
}
#endif

#if defined(HWA_WASM_REACTOR)
__attribute__((export_name("hwa_portability_test")))
int hwa_portability_test(void)
#else
int main(void)
#endif
{
    failures = 0;
    test_source_analysis();
    test_limits_and_failure();
    test_virtual_long_source();
#if !defined(HWA_WASM_REACTOR)
    test_native_path_parity();
#endif
#if defined(HWA_WASM_REACTOR)
    if (failures != 0) __builtin_trap();
#else
    if (failures != 0) {
        (void)fprintf(stderr, "%d portability checks failed\n", failures);
        return 1;
    }
#endif
    return 0;
}

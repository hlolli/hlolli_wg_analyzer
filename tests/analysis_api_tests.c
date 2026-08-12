#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "hlolli_wg_analyzer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define FIXTURE_RATE 8000U
#define FIXTURE_FRAMES 1024U

static int failures;

#define CHECK(condition, ...)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);       \
            (void)fprintf(stderr, __VA_ARGS__);                              \
            (void)fputc('\n', stderr);                                       \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static long test_process_id(void)
{
#if defined(_WIN32)
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}

static int remove_file(const char *path)
{
#if defined(_WIN32)
    return _unlink(path);
#else
    return unlink(path);
#endif
}

static int write_bytes(FILE *file, const void *bytes, size_t size)
{
    return fwrite(bytes, 1U, size, file) == size;
}

static int write_u16(FILE *file, uint16_t value)
{
    unsigned char bytes[2];

    bytes[0] = (unsigned char)(value & UINT16_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT16_C(0xff));
    return write_bytes(file, bytes, sizeof(bytes));
}

static int write_u32(FILE *file, uint32_t value)
{
    unsigned char bytes[4];

    bytes[0] = (unsigned char)(value & UINT32_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT32_C(0xff));
    bytes[2] = (unsigned char)((value >> 16U) & UINT32_C(0xff));
    bytes[3] = (unsigned char)((value >> 24U) & UINT32_C(0xff));
    return write_bytes(file, bytes, sizeof(bytes));
}

static int write_fixture(const char *path)
{
    uint32_t frame;
    FILE *file = fopen(path, "wb");

    if (file == NULL) {
        return 0;
    }
    if (!write_bytes(file, "RIFF", 4U) ||
        !write_u32(file, 36U + FIXTURE_FRAMES * 2U) ||
        !write_bytes(file, "WAVE", 4U) ||
        !write_bytes(file, "fmt ", 4U) ||
        !write_u32(file, 16U) ||
        !write_u16(file, 1U) ||
        !write_u16(file, 1U) ||
        !write_u32(file, FIXTURE_RATE) ||
        !write_u32(file, FIXTURE_RATE * 2U) ||
        !write_u16(file, 2U) ||
        !write_u16(file, 16U) ||
        !write_bytes(file, "data", 4U) ||
        !write_u32(file, FIXTURE_FRAMES * 2U)) {
        (void)fclose(file);
        return 0;
    }
    for (frame = 0U; frame < FIXTURE_FRAMES; ++frame) {
        int16_t sample = (frame & 1U) == 0U ? INT16_C(1000) : INT16_C(-1000);

        if (!write_u16(file, (uint16_t)sample)) {
            (void)fclose(file);
            return 0;
        }
    }
    return fclose(file) == 0;
}

static int all_bytes_zero(const void *value, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)value;
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return 0;
        }
    }
    return 1;
}

static void test_invalid_options_clear_result(const char *path)
{
    HWAAnalysisOptions options;
    HWAAnalysis analysis;
    char error[HWA_ERROR_SIZE];
    int status;

    hwa_analysis_options_default(&options);
    options.frame_size = 300U;
    memset(&analysis, 0xa5, sizeof(analysis));
    status = hwa_analyze_wav_with_options(
        path, &options, &analysis, error, sizeof(error));
    CHECK(status != 0, "invalid frame size should fail");
    CHECK(strstr(error, "frame size") != NULL,
          "invalid frame size should give a useful error: %s", error);
    CHECK(all_bytes_zero(&analysis, sizeof(analysis)),
          "an invalid options call must still initialize the whole result");
}

static void test_options_may_alias_result(const char *path)
{
    HWAAnalysis analysis;
    char error[HWA_ERROR_SIZE];
    int status;

    memset(&analysis, 0, sizeof(analysis));
    hwa_analysis_options_default(&analysis.options);
    analysis.options.frame_size = 256U;
    analysis.options.hop_size = 128U;
    status = hwa_analyze_wav_with_options(
        path, &analysis.options, &analysis, error, sizeof(error));
    CHECK(status == 0, "aliased options should work: %s", error);
    if (status == 0) {
        CHECK(analysis.options.frame_size == 256U &&
                  analysis.options.hop_size == 128U,
              "analysis must retain copied aliased options");
        CHECK(analysis.path != NULL && strcmp(analysis.path, path) == 0,
              "analysis must retain the input path");
        CHECK(analysis.format.frames == FIXTURE_FRAMES &&
                  analysis.analyzed_channels == 1U &&
                  analysis.channels != NULL,
              "aliased options call must return a complete result");
        hwa_analysis_free(&analysis);
    }
}

int main(void)
{
    char path[PATH_MAX];
#if defined(_WIN32)
    const char *temporary_root = getenv("TEMP");

    if (temporary_root == NULL || temporary_root[0] == '\0') {
        temporary_root = ".";
    }
#else
    const char *temporary_root = "/tmp";
#endif
    int length = snprintf(path, sizeof(path), "%s/hwa-analysis-api-%ld.wav",
                          temporary_root, test_process_id());

    CHECK(length >= 0 && (size_t)length < sizeof(path),
          "temporary fixture path is too long");
    if (failures == 0) {
        CHECK(write_fixture(path), "could not write the API fixture");
    }
    if (failures == 0) {
        test_invalid_options_clear_result(path);
        test_options_may_alias_result(path);
    }
    (void)remove_file(path);
    if (failures != 0) {
        (void)fprintf(stderr, "%d API assertion(s) failed\n", failures);
        return 1;
    }
    (void)puts("PASS analysis API contracts");
    return 0;
}

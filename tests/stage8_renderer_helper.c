#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

#ifndef HWA_STAGE8_RENDERER_MODE
#define HWA_STAGE8_RENDERER_MODE 0
#endif

#if HWA_STAGE8_RENDERER_MODE != 1
static void hwa_test_u16(FILE *stream, uint16_t value)
{
    (void)fputc((int)(value & UINT16_C(0xff)), stream);
    (void)fputc((int)((value >> 8U) & UINT16_C(0xff)), stream);
}

static void hwa_test_u32(FILE *stream, uint32_t value)
{
    (void)fputc((int)(value & UINT32_C(0xff)), stream);
    (void)fputc((int)((value >> 8U) & UINT32_C(0xff)), stream);
    (void)fputc((int)((value >> 16U) & UINT32_C(0xff)), stream);
    (void)fputc((int)((value >> 24U) & UINT32_C(0xff)), stream);
}

static int hwa_test_write_wave(const char *path)
{
    const uint32_t frames = UINT32_C(4096);
    const uint32_t data_bytes = frames * UINT32_C(2);
    uint32_t state = UINT32_C(0x12345678);
    FILE *stream = fopen(path, "wb");
    uint32_t index;

    if (stream == NULL) return -1;
    if (fwrite("RIFF", 1U, 4U, stream) != 4U) goto failed;
    hwa_test_u32(stream, UINT32_C(36) + data_bytes);
    if (fwrite("WAVEfmt ", 1U, 8U, stream) != 8U) goto failed;
    hwa_test_u32(stream, UINT32_C(16));
    hwa_test_u16(stream, UINT16_C(1));
    hwa_test_u16(stream, UINT16_C(1));
    hwa_test_u32(stream, UINT32_C(48000));
    hwa_test_u32(stream, UINT32_C(96000));
    hwa_test_u16(stream, UINT16_C(2));
    hwa_test_u16(stream, UINT16_C(16));
    if (fwrite("data", 1U, 4U, stream) != 4U) goto failed;
    hwa_test_u32(stream, data_bytes);
    for (index = 0U; index < frames; ++index) {
        int16_t sample;

        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
        sample = (int16_t)((state >> 17U) & UINT32_C(0x7fff));
        sample = (int16_t)(sample - INT16_C(16384));
        hwa_test_u16(stream, (uint16_t)sample);
    }
    if (fflush(stream) != 0 || ferror(stream) || fclose(stream) != 0) return -1;
    return 0;
failed:
    (void)fclose(stream);
    return -1;
}

static int hwa_test_join(char *buffer,
                         size_t buffer_size,
                         const char *directory,
                         const char *name)
{
    int written = snprintf(buffer, buffer_size, "%s/%s", directory, name);
    return written < 0 || (size_t)written >= buffer_size ? -1 : 0;
}
#endif

#if HWA_STAGE8_RENDERER_MODE == 4 || \
    (HWA_STAGE8_RENDERER_MODE == 5 && !defined(_WIN32))
static int hwa_test_write_extra(const char *directory, const char *name)
{
    char path[4096];
    FILE *stream;
    int okay;

    if (hwa_test_join(path, sizeof(path), directory, name) != 0) return -1;
    stream = fopen(path, "wb");
    if (stream == NULL) return -1;
    okay = fwrite("extra", 1U, 5U, stream) == 5U &&
           fflush(stream) == 0 && !ferror(stream);
    if (fclose(stream) != 0) okay = 0;
    return okay ? 0 : -1;
}
#endif

#if HWA_STAGE8_RENDERER_MODE == 7 && !defined(_WIN32)
static int hwa_test_churn_regular_scratch(const char *directory)
{
    char path[4096];
    unsigned round;
    unsigned slot;

    for (round = 0U; round < 200U; ++round) {
        for (slot = 0U; slot < 256U; ++slot) {
            FILE *stream;
            char name[64];
            int written = snprintf(name, sizeof(name), ".scratch-%u", slot);
            if (written < 0 || (size_t)written >= sizeof(name) ||
                hwa_test_join(path, sizeof(path), directory, name) != 0)
                return -1;
            stream = fopen(path, "wb");
            if (stream == NULL || fputc((int)(round & 0xffU), stream) == EOF ||
                fclose(stream) != 0) return -1;
        }
        for (slot = 0U; slot < 256U; ++slot) {
            char name[64];
            int written = snprintf(name, sizeof(name), ".scratch-%u", slot);
            if (written < 0 || (size_t)written >= sizeof(name) ||
                hwa_test_join(path, sizeof(path), directory, name) != 0 ||
                unlink(path) != 0) return -1;
        }
    }
    return 0;
}
#endif

int main(int argc, char **argv)
{
#if HWA_STAGE8_RENDERER_MODE != 1
    char output_path[4096];
#endif

    if (argc != 5 || strcmp(argv[1], "--hwa-experiment-job") != 0 ||
        strcmp(argv[3], "--output-dir") != 0 ||
        getenv("HOME") != NULL || getenv("PATH") != NULL) {
        return 90;
    }
    {
        FILE *request = fopen(argv[2], "rb");
        char text[64];
        size_t count;

        if (request == NULL) return 91;
        count = fread(text, 1U, sizeof(text) - 1U, request);
        text[count] = '\0';
        if (fclose(request) != 0 || strstr(text, "hwa-render-job") == NULL) {
            return 92;
        }
    }
#if HWA_STAGE8_RENDERER_MODE == 1
    return 23;
#else
#if HWA_STAGE8_RENDERER_MODE == 2
#if defined(_WIN32)
    Sleep(2000U);
#else
    {
        struct timespec delay;
        delay.tv_sec = 2;
        delay.tv_nsec = 0;
        (void)nanosleep(&delay, NULL);
    }
#endif
#elif HWA_STAGE8_RENDERER_MODE == 6
#if defined(_WIN32)
    Sleep(5U);
#else
    {
        struct timespec delay;
        delay.tv_sec = 0;
        delay.tv_nsec = 5000000L;
        (void)nanosleep(&delay, NULL);
    }
#endif
#endif
#if HWA_STAGE8_RENDERER_MODE == 7 && !defined(_WIN32)
    if (hwa_test_churn_regular_scratch(argv[4]) != 0) return 100;
#endif
    if (hwa_test_join(output_path, sizeof(output_path), argv[4],
                      "model.wav") != 0 ||
        hwa_test_write_wave(output_path) != 0) return 93;
#if HWA_STAGE8_RENDERER_MODE == 3
    {
        FILE *extra;
        size_t index;

        if (hwa_test_join(output_path, sizeof(output_path), argv[4],
                          "oversized.bin") != 0) return 94;
        extra = fopen(output_path, "wb");
        if (extra == NULL) return 95;
        for (index = 0U; index < 1048576U; ++index) {
            if (fputc(0, extra) == EOF) {
                (void)fclose(extra);
                return 96;
            }
        }
        if (fclose(extra) != 0) return 97;
    }
#elif HWA_STAGE8_RENDERER_MODE == 4
    if (hwa_test_write_extra(argv[4], "undeclared.bin") != 0) return 98;
#elif HWA_STAGE8_RENDERER_MODE == 5
#if !defined(_WIN32)
    {
        pid_t worker = fork();
        if (worker < 0) return 99;
        if (worker == 0) {
            struct timespec delay;
            delay.tv_sec = 0;
            delay.tv_nsec = 500000000L;
            (void)nanosleep(&delay, NULL);
            _exit(hwa_test_write_extra(argv[4], "late.bin") == 0 ? 0 : 1);
        }
    }
#endif
#endif
    (void)puts("stage8 renderer helper");
    return 0;
#endif
}

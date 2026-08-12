#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "hlolli_wg_analyzer.h"
#include "run.h"
#include "sha256.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#define TEST_PID _getpid
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define TEST_PID getpid
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define TEST_PI 3.14159265358979323846264338327950288
#define ODD_RATE 11025U
#define ODD_WINDOW 221U
#define ODD_HOP 110U
#define ODD_DELAY_SAMPLES 2750U
#define ODD_FRAMES 13421U
#define ODD_PROBE_COUNT 80U

typedef struct TestFiles {
    char directory[PATH_MAX];
    char reference[PATH_MAX];
    char source[PATH_MAX];
    char body[PATH_MAX];
    char wet[PATH_MAX];
    char final[PATH_MAX];
    char link_reference[PATH_MAX];
    char link_model[PATH_MAX];
    char csv_probe[PATH_MAX];
    char binary_probe[PATH_MAX];
    char stage_manifest[PATH_MAX];
    char csv_manifest[PATH_MAX];
    char binary_manifest[PATH_MAX];
    char bad_manifest[PATH_MAX];
    char authority_manifest[PATH_MAX];
    char hostile_manifest[PATH_MAX];
    char hostile_probe[PATH_MAX];
    char nan_wav[PATH_MAX];
    char nan_manifest[PATH_MAX];
    char symlink_wav[PATH_MAX];
    char hardlink_wav[PATH_MAX];
    char odd_reference[PATH_MAX];
    char odd_model[PATH_MAX];
    char odd_probe[PATH_MAX];
    char odd_manifest[PATH_MAX];
} TestFiles;

static int failures;

#define CHECK(condition, ...)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            (void)fprintf(stderr, "FAIL: ");                                \
            (void)fprintf(stderr, __VA_ARGS__);                              \
            (void)fputc('\n', stderr);                                       \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void put_le16(unsigned char bytes[2], uint16_t value)
{
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xffU);
}

static void put_le32(unsigned char bytes[4], uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xffU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xffU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xffU);
}

static void put_le64(unsigned char bytes[8], uint64_t value)
{
    size_t index;
    for (index = 0U; index < 8U; ++index)
        bytes[index] = (unsigned char)((value >> (8U * (unsigned)index)) & 0xffU);
}

static int join_path(char path[PATH_MAX],
                     const char *directory,
                     const char *name)
{
    int written = snprintf(path, PATH_MAX, "%s/%s", directory, name);
    return written > 0 && written < PATH_MAX;
}

static int make_directory(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

static void remove_path(const char *path)
{
#if defined(_WIN32)
    (void)_unlink(path);
#else
    (void)unlink(path);
#endif
}

static int files_open(TestFiles *files)
{
    unsigned attempt;
    memset(files, 0, sizeof(*files));
    for (attempt = 0U; attempt < 100U; ++attempt) {
        int written = snprintf(files->directory, sizeof(files->directory),
                               "/tmp/hwa-run-test-%ld-%u",
                               (long)TEST_PID(), attempt);
        if (written < 0 || (size_t)written >= sizeof(files->directory))
            return 0;
        if (make_directory(files->directory) == 0) break;
        if (errno != EEXIST) return 0;
    }
    return attempt < 100U &&
        join_path(files->reference, files->directory, "reference.wav") &&
        join_path(files->source, files->directory, "source.wav") &&
        join_path(files->body, files->directory, "body.wav") &&
        join_path(files->wet, files->directory, "wet.wav") &&
        join_path(files->final, files->directory, "final.wav") &&
        join_path(files->link_reference, files->directory, "link-ref.wav") &&
        join_path(files->link_model, files->directory, "link-model.wav") &&
        join_path(files->csv_probe, files->directory, "probe.csv") &&
        join_path(files->binary_probe, files->directory, "probe.bin") &&
        join_path(files->stage_manifest, files->directory, "stage.json") &&
        join_path(files->csv_manifest, files->directory, "csv.json") &&
        join_path(files->binary_manifest, files->directory, "binary.json") &&
        join_path(files->bad_manifest, files->directory, "bad.json") &&
        join_path(files->authority_manifest, files->directory,
                  "authority.json") &&
        join_path(files->hostile_manifest, files->directory, "hostile.json") &&
        join_path(files->hostile_probe, files->directory, "hostile.csv") &&
        join_path(files->nan_wav, files->directory, "nan.wav") &&
        join_path(files->nan_manifest, files->directory, "nan.json") &&
        join_path(files->symlink_wav, files->directory, "symlink.wav") &&
        join_path(files->hardlink_wav, files->directory, "hardlink.wav") &&
        join_path(files->odd_reference, files->directory, "odd-reference.wav") &&
        join_path(files->odd_model, files->directory, "odd-model.wav") &&
        join_path(files->odd_probe, files->directory, "odd-probe.csv") &&
        join_path(files->odd_manifest, files->directory, "odd.json");
}

static void files_close(TestFiles *files)
{
    const char *paths[] = {
        files->reference, files->source, files->body, files->wet, files->final,
        files->link_reference, files->link_model, files->csv_probe,
        files->binary_probe, files->stage_manifest, files->csv_manifest,
        files->binary_manifest, files->bad_manifest,
        files->authority_manifest, files->hostile_manifest,
        files->hostile_probe, files->nan_wav, files->nan_manifest,
        files->symlink_wav, files->hardlink_wav, files->odd_reference,
        files->odd_model, files->odd_probe, files->odd_manifest
    };
    size_t index;
    for (index = 0U; index < sizeof(paths) / sizeof(paths[0]); ++index)
        remove_path(paths[index]);
#if defined(_WIN32)
    (void)_rmdir(files->directory);
#else
    (void)rmdir(files->directory);
#endif
}

static int write_wav(const char *path,
                     uint32_t rate,
                     uint32_t frames,
                     double amplitude,
                     const double *probe,
                     size_t probe_count,
                     size_t delay_hops)
{
    FILE *stream = fopen(path, "wb");
    unsigned char header[44] = {0};
    uint32_t frame;
    if (stream == NULL || frames > (UINT32_MAX - 36U) / 2U) return 0;
    memcpy(header, "RIFF", 4U);
    put_le32(header + 4U, 36U + frames * 2U);
    memcpy(header + 8U, "WAVEfmt ", 8U);
    put_le32(header + 16U, 16U);
    put_le16(header + 20U, 1U);
    put_le16(header + 22U, 1U);
    put_le32(header + 24U, rate);
    put_le32(header + 28U, rate * 2U);
    put_le16(header + 32U, 2U);
    put_le16(header + 34U, 16U);
    memcpy(header + 36U, "data", 4U);
    put_le32(header + 40U, frames * 2U);
    if (fwrite(header, 1U, sizeof(header), stream) != sizeof(header)) goto failed;
    for (frame = 0U; frame < frames; ++frame) {
        double used_amplitude = amplitude;
        double sample;
        long encoded;
        unsigned char bytes[2];
        if (probe != NULL && probe_count != 0U) {
            size_t hop = rate / 100U;
            size_t position = hop == 0U ? 0U : (size_t)frame / hop;
            size_t probe_index = position > delay_hops
                                     ? position - delay_hops
                                     : 0U;
            if (probe_index >= probe_count) probe_index = probe_count - 1U;
            used_amplitude = sqrt(2.0) * pow(10.0, probe[probe_index] / 20.0);
        }
        sample = used_amplitude * sin(2.0 * TEST_PI * 1000.0 *
                                      (double)frame / (double)rate);
        encoded = lround(fmax(-1.0, fmin(0.999969, sample)) * 32768.0);
        put_le16(bytes, (uint16_t)(int16_t)encoded);
        if (fwrite(bytes, 1U, sizeof(bytes), stream) != sizeof(bytes))
            goto failed;
    }
    return fclose(stream) == 0;
failed:
    (void)fclose(stream);
    return 0;
}

static int write_float_nan_wav(const char *path,
                               uint32_t rate,
                               uint32_t frames)
{
    FILE *stream = fopen(path, "wb");
    unsigned char header[44] = {0};
    uint32_t frame;
    if (stream == NULL || frames == 0U ||
        frames > (UINT32_MAX - 36U) / 4U) return 0;
    memcpy(header, "RIFF", 4U);
    put_le32(header + 4U, 36U + frames * 4U);
    memcpy(header + 8U, "WAVEfmt ", 8U);
    put_le32(header + 16U, 16U);
    put_le16(header + 20U, 3U);
    put_le16(header + 22U, 1U);
    put_le32(header + 24U, rate);
    put_le32(header + 28U, rate * 4U);
    put_le16(header + 32U, 4U);
    put_le16(header + 34U, 32U);
    memcpy(header + 36U, "data", 4U);
    put_le32(header + 40U, frames * 4U);
    if (fwrite(header, 1U, sizeof(header), stream) != sizeof(header))
        goto failed;
    for (frame = 0U; frame < frames; ++frame) {
        unsigned char bytes[4];
        uint32_t bits = frame + 1U == frames
                            ? UINT32_C(0x7fc00000)
                            : UINT32_C(0x3dcccccd);
        put_le32(bytes, bits);
        if (fwrite(bytes, 1U, sizeof(bytes), stream) != sizeof(bytes))
            goto failed;
    }
    return fclose(stream) == 0;
failed:
    (void)fclose(stream);
    return 0;
}

static int write_odd_wav(const char *path,
                         const double probe[ODD_PROBE_COUNT])
{
    FILE *stream = fopen(path, "wb");
    unsigned char header[44] = {0};
    uint32_t frame;
    if (stream == NULL) return 0;
    memcpy(header, "RIFF", 4U);
    put_le32(header + 4U, 36U + ODD_FRAMES * 2U);
    memcpy(header + 8U, "WAVEfmt ", 8U);
    put_le32(header + 16U, 16U);
    put_le16(header + 20U, 1U);
    put_le16(header + 22U, 1U);
    put_le32(header + 24U, ODD_RATE);
    put_le32(header + 28U, ODD_RATE * 2U);
    put_le16(header + 32U, 2U);
    put_le16(header + 34U, 16U);
    memcpy(header + 36U, "data", 4U);
    put_le32(header + 40U, ODD_FRAMES * 2U);
    if (fwrite(header, 1U, sizeof(header), stream) != sizeof(header))
        goto failed;
    for (frame = 0U; frame < ODD_FRAMES; ++frame) {
        double level = -60.0;
        double amplitude;
        long encoded;
        unsigned char bytes[2];
        if (frame >= ODD_DELAY_SAMPLES + 10U) {
            uint32_t local = frame - ODD_DELAY_SAMPLES - 10U;
            uint32_t probe_index = local / 100U;
            if (probe_index < ODD_PROBE_COUNT) level = probe[probe_index];
        }
        amplitude = pow(10.0, level / 20.0);
        encoded = lround(amplitude * 32767.0);
        if ((frame & 1U) != 0U) encoded = -encoded;
        put_le16(bytes, (uint16_t)(int16_t)encoded);
        if (fwrite(bytes, 1U, sizeof(bytes), stream) != sizeof(bytes))
            goto failed;
    }
    return fclose(stream) == 0;
failed:
    (void)fclose(stream);
    return 0;
}

static int write_raw_file(const char *path,
                          const unsigned char *bytes,
                          size_t size)
{
    FILE *stream = fopen(path, "wb");
    if (stream == NULL) return 0;
    if (size != 0U && fwrite(bytes, 1U, size, stream) != size) {
        (void)fclose(stream);
        return 0;
    }
    return fclose(stream) == 0;
}

static int file_hash(const char *path, char hash[HWA_SHA256_HEX_SIZE])
{
    char error[HWA_ERROR_SIZE];
    return hwa_sha256_file(path, UINT64_C(100000000), hash,
                           error, sizeof(error)) == 0;
}

static int write_probe_csv(const char *path,
                           const double *values,
                           size_t count)
{
    FILE *stream = fopen(path, "wb");
    size_t index;
    if (stream == NULL || fputs("index,value\n", stream) == EOF) return 0;
    for (index = 0U; index < count; ++index)
        if (fprintf(stream, "%zu,%.17g\n", index, values[index]) < 0)
            goto failed;
    return fclose(stream) == 0;
failed:
    (void)fclose(stream);
    return 0;
}

static int write_probe_binary(const char *path,
                              const double *values,
                              size_t count)
{
    FILE *stream = fopen(path, "wb");
    unsigned char header[16] = {'H','W','A','P','R','B','1','\0'};
    size_t index;
    if (stream == NULL) return 0;
    put_le64(header + 8U, (uint64_t)count);
    if (fwrite(header, 1U, sizeof(header), stream) != sizeof(header)) goto failed;
    for (index = 0U; index < count; ++index) {
        uint64_t bits;
        unsigned char bytes[8];
        memcpy(&bits, &values[index], sizeof(bits));
        put_le64(bytes, bits);
        if (fwrite(bytes, 1U, sizeof(bytes), stream) != sizeof(bytes)) goto failed;
    }
    return fclose(stream) == 0;
failed:
    (void)fclose(stream);
    return 0;
}

static int write_stage_manifest(const TestFiles *files)
{
    char hashes[5][HWA_SHA256_HEX_SIZE];
    FILE *stream;
    if (!file_hash(files->reference, hashes[0]) ||
        !file_hash(files->source, hashes[1]) ||
        !file_hash(files->body, hashes[2]) ||
        !file_hash(files->wet, hashes[3]) ||
        !file_hash(files->final, hashes[4])) return 0;
    stream = fopen(files->stage_manifest, "wb");
    if (stream == NULL) return 0;
    if (fprintf(stream,
        "{\"links\":[],\"probes\":[],\"stems\":["
        "{\"channels\":1,\"gain_db\":0,\"id\":\"z.target\",\"rate_hz\":48000,\"role\":\"final\",\"sha256\":\"%s\",\"side\":\"reference\",\"start_sample\":0},"
        "{\"id\":\"q.source\",\"side\":\"model\",\"role\":\"source\",\"sha256\":\"%s\",\"start_sample\":0,\"gain_db\":0,\"rate_hz\":48000,\"channels\":1},"
        "{\"id\":\"p.body\",\"side\":\"model\",\"role\":\"body\",\"sha256\":\"%s\",\"start_sample\":0,\"gain_db\":0,\"rate_hz\":48000,\"channels\":1},"
        "{\"id\":\"n.wet\",\"side\":\"model\",\"role\":\"wet\",\"sha256\":\"%s\",\"start_sample\":0,\"gain_db\":0,\"rate_hz\":48000,\"channels\":1},"
        "{\"id\":\"m.final\",\"side\":\"model\",\"role\":\"final\",\"sha256\":\"%s\",\"start_sample\":0,\"gain_db\":0,\"rate_hz\":48000,\"channels\":1}],"
        "\"clock_rate_hz\":48000,\"method_version\":\"stage7-1\",\"schema_version\":1,\"schema\":\"hwa-run\"}",
        hashes[0], hashes[1], hashes[2], hashes[3], hashes[4]) < 0) {
        (void)fclose(stream);
        return 0;
    }
    return fclose(stream) == 0;
}

static int write_pair_manifest(const char *manifest_path,
                               const char *reference_path,
                               const char *model_path,
                               uint32_t rate)
{
    char reference_hash[HWA_SHA256_HEX_SIZE];
    char model_hash[HWA_SHA256_HEX_SIZE];
    FILE *stream;
    if (!file_hash(reference_path, reference_hash) ||
        !file_hash(model_path, model_hash)) return 0;
    stream = fopen(manifest_path, "wb");
    if (stream == NULL) return 0;
    if (fprintf(stream,
        "{\"schema\":\"hwa-run\",\"schema_version\":1,\"method_version\":\"stage7-1\",\"clock_rate_hz\":%u,"
        "\"stems\":["
        "{\"id\":\"target\",\"side\":\"reference\",\"role\":\"final\",\"sha256\":\"%s\",\"start_sample\":0,\"gain_db\":0,\"rate_hz\":%u,\"channels\":1},"
        "{\"id\":\"render\",\"side\":\"model\",\"role\":\"final\",\"sha256\":\"%s\",\"start_sample\":0,\"gain_db\":0,\"rate_hz\":%u,\"channels\":1}],"
        "\"probes\":[],\"links\":[]}",
        rate, reference_hash, rate, model_hash, rate) < 0) {
        (void)fclose(stream);
        return 0;
    }
    return fclose(stream) == 0;
}

static int write_timed_link_manifest(
    const char *manifest_path,
    const char *reference_path,
    const char *model_path,
    const char *probe_path,
    uint32_t clock_rate,
    int64_t probe_start,
    uint64_t rate_numerator,
    uint64_t rate_denominator,
    uint64_t value_count)
{
    char reference_hash[HWA_SHA256_HEX_SIZE];
    char model_hash[HWA_SHA256_HEX_SIZE];
    char probe_hash[HWA_SHA256_HEX_SIZE];
    FILE *stream;
    if (!file_hash(reference_path, reference_hash) ||
        !file_hash(model_path, model_hash) ||
        !file_hash(probe_path, probe_hash)) return 0;
    stream = fopen(manifest_path, "wb");
    if (stream == NULL) return 0;
    if (fprintf(stream,
        "{\"schema\":\"hwa-run\",\"schema_version\":1,\"method_version\":\"stage7-1\",\"clock_rate_hz\":%u,"
        "\"stems\":["
        "{\"id\":\"target\",\"side\":\"reference\",\"role\":\"final\",\"sha256\":\"%s\",\"start_sample\":0,\"gain_db\":0,\"rate_hz\":%u,\"channels\":1},"
        "{\"id\":\"render\",\"side\":\"model\",\"role\":\"final\",\"sha256\":\"%s\",\"start_sample\":0,\"gain_db\":0,\"rate_hz\":%u,\"channels\":1}],"
        "\"probes\":[{\"id\":\"force.input\",\"side\":\"model\",\"name\":\"model.force\",\"unit\":\"N\",\"format\":\"csv-f64\",\"sha256\":\"%s\",\"start_sample\":%lld,\"rate_numerator\":%llu,\"rate_denominator\":%llu,\"value_count\":%llu}],"
        "\"links\":[{\"stem\":\"render\",\"probe\":\"force.input\",\"feature\":\"rms_dbfs\"}]}",
        clock_rate, reference_hash, clock_rate, model_hash, clock_rate,
        probe_hash, (long long)probe_start,
        (unsigned long long)rate_numerator,
        (unsigned long long)rate_denominator,
        (unsigned long long)value_count) < 0) {
        (void)fclose(stream);
        return 0;
    }
    return fclose(stream) == 0;
}

static int write_link_manifest(const char *path,
                               const char *reference_path,
                               const char *model_path,
                               const char *probe_path,
                               const char *format,
                               int bad_hash,
                               size_t value_count)
{
    char reference_hash[HWA_SHA256_HEX_SIZE];
    char model_hash[HWA_SHA256_HEX_SIZE];
    char probe_hash[HWA_SHA256_HEX_SIZE];
    FILE *stream;
    if (!file_hash(reference_path, reference_hash) ||
        !file_hash(model_path, model_hash) || !file_hash(probe_path, probe_hash))
        return 0;
    if (bad_hash) probe_hash[0] = probe_hash[0] == '0' ? '1' : '0';
    stream = fopen(path, "wb");
    if (stream == NULL) return 0;
    if (fprintf(stream,
        "{\"schema\":\"hwa-run\",\"schema_version\":1,\"method_version\":\"stage7-1\",\"clock_rate_hz\":8000,"
        "\"stems\":["
        "{\"id\":\"target\",\"side\":\"reference\",\"role\":\"final\",\"sha256\":\"%s\",\"start_sample\":0,\"gain_db\":0,\"rate_hz\":8000,\"channels\":1},"
        "{\"id\":\"render\",\"side\":\"model\",\"role\":\"final\",\"sha256\":\"%s\",\"start_sample\":0,\"gain_db\":0,\"rate_hz\":8000,\"channels\":1}],"
        "\"probes\":[{\"id\":\"force.input\",\"side\":\"model\",\"name\":\"model.force\",\"unit\":\"N\",\"format\":\"%s\",\"sha256\":\"%s\",\"start_sample\":0,\"rate_numerator\":100,\"rate_denominator\":1,\"value_count\":%zu}],"
        "\"links\":[{\"stem\":\"render\",\"probe\":\"force.input\",\"feature\":\"rms_dbfs\"}]}",
        reference_hash, model_hash, format, probe_hash, value_count) < 0) {
        (void)fclose(stream);
        return 0;
    }
    return fclose(stream) == 0;
}

static void test_stage_run(const TestFiles *files)
{
    HWARunBinding bindings[5] = {
        {"m.final", files->final}, {"z.target", files->reference},
        {"n.wet", files->wet}, {"p.body", files->body},
        {"q.source", files->source}
    };
    HWARunResult result;
    char error[HWA_ERROR_SIZE];
    memset(&result, 0, sizeof(result));
    CHECK(hwa_analyze_run_files(files->stage_manifest, bindings, 5U, NULL,
                                &result, error, sizeof(error)) == 0,
          "stage run failed: %s", error);
    if (result.source_count != 0U) {
        CHECK(result.source_count == 5U, "stage source count");
        CHECK(result.clock_count == 4U, "stage clock count");
        CHECK(result.feature_count == 48U, "stage feature count");
        CHECK(result.stage_count == 3U, "stage row count");
        CHECK(result.stages[0].availability == HWA_RUN_AVAILABLE,
              "source-body stage unavailable");
        CHECK(result.stages[0].added_gap > 0.1,
              "source-body stage did not add the known gap: %.17g",
              result.stages[0].added_gap);
        CHECK(result.stages[0].rank == 1U, "known stage rank");
    }
    hwa_run_result_free(&result);
    hwa_run_result_free(&result);
    CHECK(result.source_count == 0U && result.manifest_path == NULL,
          "run free did not zero the result");
}

static void test_probe_runs(const TestFiles *files,
                            size_t value_count)
{
    HWARunBinding csv_bindings[3] = {
        {"force.input", files->csv_probe},
        {"render", files->link_model},
        {"target", files->link_reference}
    };
    HWARunBinding binary_bindings[3] = {
        {"target", files->link_reference},
        {"force.input", files->binary_probe},
        {"render", files->link_model}
    };
    HWARunResult csv;
    HWARunResult binary;
    HWARunOptions options;
    uint64_t exact_evaluations = 0U;
    char error[HWA_ERROR_SIZE];
    memset(&csv, 0, sizeof(csv));
    memset(&binary, 0, sizeof(binary));
    CHECK(hwa_analyze_run_files(files->csv_manifest, csv_bindings, 3U, NULL,
                                &csv, error, sizeof(error)) == 0,
          "CSV run failed: %s", error);
    CHECK(hwa_analyze_run_files(files->binary_manifest, binary_bindings, 3U,
                                NULL, &binary, error, sizeof(error)) == 0,
          "binary run failed: %s", error);
    if (csv.link_count == 1U && binary.link_count == 1U) {
        CHECK(csv.links[0].availability == HWA_RUN_AVAILABLE,
              "CSV link unavailable: points=%zu coverage=%.17g flags=%u",
              csv.links[0].point_count, csv.links[0].coverage,
              csv.links[0].quality_flags);
        CHECK(csv.links[0].lag_hops > 0,
              "known probe-leading lag is not positive: %lld",
              (long long)csv.links[0].lag_hops);
        CHECK(fabs(csv.links[0].correlation) > 0.7,
              "known link correlation too small: %.17g",
              csv.links[0].correlation);
        CHECK(csv.links[0].lag_hops == binary.links[0].lag_hops &&
              csv.links[0].correlation == binary.links[0].correlation &&
              csv.links[0].slope == binary.links[0].slope &&
              csv.probes[0].mean == binary.probes[0].mean &&
              csv.probes[0].population_sd == binary.probes[0].population_sd,
              "CSV and binary probe results differ");
    } else {
        CHECK(0, "probe run row counts");
    }
    exact_evaluations = csv.evaluation_count;
    CHECK(exact_evaluations == UINT64_C(167828),
          "link evaluation ledger: got %llu",
          (unsigned long long)exact_evaluations);
    hwa_run_result_free(&binary);
    hwa_run_result_free(&csv);

    hwa_run_options_default(&options);
    options.max_evaluations = exact_evaluations;
    CHECK(hwa_analyze_run_files(files->csv_manifest, csv_bindings, 3U,
                                &options, &csv, error, sizeof(error)) == 0,
          "exact evaluation cap failed: %s", error);
    CHECK(csv.evaluation_count == exact_evaluations,
          "exact evaluation cap changed the ledger");
    hwa_run_result_free(&csv);
    options.max_evaluations = exact_evaluations - UINT64_C(1);
    CHECK(hwa_analyze_run_files(files->csv_manifest, csv_bindings, 3U,
                                &options, &csv, error, sizeof(error)) != 0,
          "one-under evaluation cap was accepted");
    CHECK(csv.source_count == 0U,
          "failed evaluation-capped result was not cleared");

    hwa_run_options_default(&options);
    options.max_probe_values = (uint64_t)value_count - UINT64_C(1);
    CHECK(hwa_analyze_run_files(files->csv_manifest, csv_bindings, 3U,
                                &options, &csv, error, sizeof(error)) != 0,
          "probe value cap was not enforced");
    CHECK(csv.source_count == 0U, "failed capped result was not cleared");
    CHECK(hwa_analyze_run_files(files->bad_manifest, csv_bindings, 3U, NULL,
                                &csv, error, sizeof(error)) != 0,
          "expected SHA-256 mismatch was accepted");
}

static void test_source_authority(const TestFiles *files)
{
    HWARunBinding pair_bindings[2] = {
        {"target", files->link_reference},
        {"render", files->link_model}
    };
    HWARunBinding probe_bindings[3] = {
        {"target", files->link_reference},
        {"render", files->link_model},
        {"force.input", files->hostile_probe}
    };
    HWARunResult result;
    char error[HWA_ERROR_SIZE];
    static const unsigned char nul_csv[] =
        "index,value\n0,1\0garbage\n";
    static const unsigned char plus_csv[] = "index,value\n0,+1\n";
    static const unsigned char good_csv[] = "index,value\n0,1\n";
    memset(&result, 0, sizeof(result));

#if !defined(_WIN32)
    CHECK(symlink(files->link_model, files->symlink_wav) == 0 &&
          write_pair_manifest(files->authority_manifest,
                              files->link_reference, files->link_model,
                              8000U),
          "cannot make symlink authority fixture");
    pair_bindings[1].path = files->symlink_wav;
    CHECK(hwa_analyze_run_files(files->authority_manifest, pair_bindings, 2U,
                                NULL, &result, error, sizeof(error)) != 0,
          "symlink input was accepted");
    remove_path(files->symlink_wav);

    CHECK(link(files->link_reference, files->hardlink_wav) == 0 &&
          write_pair_manifest(files->authority_manifest,
                              files->link_reference, files->hardlink_wav,
                              8000U),
          "cannot make hard-link authority fixture");
    pair_bindings[1].path = files->hardlink_wav;
    CHECK(hwa_analyze_run_files(files->authority_manifest, pair_bindings, 2U,
                                NULL, &result, error, sizeof(error)) != 0,
          "two bindings to one file identity were accepted");
    remove_path(files->hardlink_wav);
#endif

    CHECK(write_float_nan_wav(files->nan_wav, 8000U, 400U) &&
          write_pair_manifest(files->nan_manifest, files->link_reference,
                              files->nan_wav, 8000U),
          "cannot make non-finite WAVE fixture");
    pair_bindings[1].path = files->nan_wav;
    CHECK(hwa_analyze_run_files(files->nan_manifest, pair_bindings, 2U,
                                NULL, &result, error, sizeof(error)) != 0,
          "non-finite WAVE sample was accepted");

    CHECK(write_raw_file(files->hostile_probe, nul_csv,
                         sizeof(nul_csv) - 1U) &&
          write_link_manifest(files->hostile_manifest, files->link_reference,
                              files->link_model, files->hostile_probe,
                              "csv-f64", 0, 1U),
          "cannot make embedded-NUL CSV fixture");
    CHECK(hwa_analyze_run_files(files->hostile_manifest, probe_bindings, 3U,
                                NULL, &result, error, sizeof(error)) != 0,
          "embedded-NUL CSV value was accepted");

    CHECK(write_raw_file(files->hostile_probe, plus_csv,
                         sizeof(plus_csv) - 1U) &&
          write_link_manifest(files->hostile_manifest, files->link_reference,
                              files->link_model, files->hostile_probe,
                              "csv-f64", 0, 1U),
          "cannot make noncanonical CSV fixture");
    CHECK(hwa_analyze_run_files(files->hostile_manifest, probe_bindings, 3U,
                                NULL, &result, error, sizeof(error)) != 0,
          "noncanonical +1 CSV value was accepted");

    CHECK(write_raw_file(files->hostile_probe, good_csv,
                         sizeof(good_csv) - 1U) &&
          write_link_manifest(files->hostile_manifest, files->link_reference,
                              files->link_model, files->hostile_probe,
                              "csv-f64", 0, 1U),
          "cannot make hash-mutation fixture");
    {
        FILE *stream = fopen(files->hostile_probe, "r+b");
        CHECK(stream != NULL && fseek(stream, 14L, SEEK_SET) == 0 &&
              fputc('2', stream) != EOF && fclose(stream) == 0,
              "cannot mutate hash fixture");
    }
    CHECK(hwa_analyze_run_files(files->hostile_manifest, probe_bindings, 3U,
                                NULL, &result, error, sizeof(error)) != 0,
          "same-size content mutation was accepted");
    hwa_run_result_free(&result);
}

static void test_work_cap(const TestFiles *files)
{
    HWARunBinding bindings[2] = {
        {"target", files->link_reference},
        {"render", files->link_model}
    };
    HWARunOptions options;
    HWARunResult result;
    char error[HWA_ERROR_SIZE];
    uint64_t low = UINT64_C(1);
    uint64_t high = UINT64_C(1048576);
    uint64_t retained = 0U;
    static const unsigned char malformed[] = "{\"stems\":[x";
    memset(&result, 0, sizeof(result));
    hwa_run_options_default(&options);
    CHECK(options.max_evaluations == UINT64_C(250000000),
          "default evaluation cap changed");
    CHECK(write_pair_manifest(files->authority_manifest,
                              files->link_reference, files->link_model,
                              8000U),
          "cannot make work-cap fixture");
    options.max_work_bytes = high;
    CHECK(hwa_analyze_run_files(files->authority_manifest, bindings, 2U,
                                &options, &result, error, sizeof(error)) == 0,
          "work-cap search upper bound failed: %s", error);
    hwa_run_result_free(&result);
    while (low < high) {
        uint64_t middle = low + (high - low) / UINT64_C(2);
        options.max_work_bytes = middle;
        if (hwa_analyze_run_files(files->authority_manifest, bindings, 2U,
                                  &options, &result, error,
                                  sizeof(error)) == 0) {
            high = middle;
            hwa_run_result_free(&result);
        } else {
            low = middle + UINT64_C(1);
        }
    }
    options.max_work_bytes = low;
    CHECK(hwa_analyze_run_files(files->authority_manifest, bindings, 2U,
                                &options, &result, error, sizeof(error)) == 0,
          "exact peak-work cap failed: %s", error);
    retained = result.retained_work_bytes;
    CHECK(low > retained,
          "work cap only measured retained bytes: cap=%llu retained=%llu",
          (unsigned long long)low, (unsigned long long)retained);
    hwa_run_result_free(&result);
    CHECK(low > 1U, "invalid exact peak-work cap");
    options.max_work_bytes = low - UINT64_C(1);
    CHECK(hwa_analyze_run_files(files->authority_manifest, bindings, 2U,
                                &options, &result, error, sizeof(error)) != 0,
          "one-under peak-work cap was accepted");
    CHECK(result.source_count == 0U,
          "failed work-capped result was not cleared");

    CHECK(write_raw_file(files->bad_manifest, malformed,
                         sizeof(malformed) - 1U),
          "cannot make malformed work-cap fixture");
    low = UINT64_C(1);
    high = UINT64_C(4096);
    options.max_work_bytes = high;
    CHECK(hwa_analyze_run_files(files->bad_manifest, NULL, 0U, &options,
                                &result, error, sizeof(error)) != 0 &&
          strstr(error, "work") == NULL,
          "malformed parser upper cap did not reach syntax failure: %s",
          error);
    while (low < high) {
        uint64_t middle = low + (high - low) / UINT64_C(2);
        options.max_work_bytes = middle;
        CHECK(hwa_analyze_run_files(files->bad_manifest, NULL, 0U, &options,
                                    &result, error, sizeof(error)) != 0,
              "malformed manifest was accepted");
        if (strstr(error, "work") == NULL) high = middle;
        else low = middle + UINT64_C(1);
    }
    options.max_work_bytes = low;
    CHECK(hwa_analyze_run_files(files->bad_manifest, NULL, 0U, &options,
                                &result, error, sizeof(error)) != 0 &&
          strstr(error, "work") == NULL,
          "exact malformed parser peak did not reach syntax failure: %s",
          error);
    CHECK(low > 1U, "invalid malformed parser peak");
    options.max_work_bytes = low - UINT64_C(1);
    CHECK(hwa_analyze_run_files(files->bad_manifest, NULL, 0U, &options,
                                &result, error, sizeof(error)) != 0 &&
          strstr(error, "work") != NULL,
          "one-under malformed parser peak was not capped: %s", error);
}

static void test_rational_timing(const TestFiles *files)
{
    HWARunBinding odd_bindings[3] = {
        {"target", files->odd_reference},
        {"render", files->odd_model},
        {"force.input", files->odd_probe}
    };
    HWARunBinding extreme_bindings[3] = {
        {"target", files->link_reference},
        {"render", files->link_model},
        {"force.input", files->hostile_probe}
    };
    HWARunResult result;
    char error[HWA_ERROR_SIZE];
    double probe[ODD_PROBE_COUNT];
    uint32_t state = UINT32_C(0x6d2b79f5);
    size_t index;
    static const unsigned char constant_csv[] = "index,value\n0,1\n";
    _Static_assert(
        (ODD_RATE * 20U + 500U) / 1000U == ODD_WINDOW &&
            (ODD_RATE * 10U + 500U) / 1000U == ODD_HOP &&
            ODD_WINDOW / 2U == 110U &&
            ODD_DELAY_SAMPLES / ODD_HOP == 25U &&
            ODD_DELAY_SAMPLES <= ODD_RATE / 4U &&
            26U * ODD_HOP > ODD_RATE / 4U &&
            1U + (ODD_FRAMES - ODD_WINDOW) / ODD_HOP == 121U,
        "noninteger timing fixture arithmetic");
    memset(&result, 0, sizeof(result));
    for (index = 0U; index < ODD_PROBE_COUNT; ++index) {
        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
        probe[index] = -26.0 + 16.0 *
            (double)(state & UINT32_C(0xffff)) / 65535.0;
    }
    CHECK(write_odd_wav(files->odd_reference, probe) &&
          write_odd_wav(files->odd_model, probe) &&
          write_probe_csv(files->odd_probe, probe, ODD_PROBE_COUNT) &&
          write_timed_link_manifest(
              files->odd_manifest, files->odd_reference, files->odd_model,
              files->odd_probe, ODD_RATE, 10, UINT64_C(441), UINT64_C(4),
              ODD_PROBE_COUNT),
          "cannot make noninteger timing fixture");
    CHECK(hwa_analyze_run_files(files->odd_manifest, odd_bindings, 3U, NULL,
                                &result, error, sizeof(error)) == 0,
          "noninteger timing run failed: %s", error);
    if (result.link_count == 1U) {
        const HWARunLink *link = &result.links[0];
        CHECK(link->availability == HWA_RUN_AVAILABLE && link->fit_valid,
              "noninteger timing link unavailable");
        CHECK(link->lag_hops == 25 && link->lag_samples == 2750,
              "noninteger timing lag: %lld/%lld",
              (long long)link->lag_hops, (long long)link->lag_samples);
        CHECK(link->point_count == 72U &&
              link->coverage == 72.0 / 121.0,
              "noninteger ZOH support: %zu/%.17g",
              link->point_count, link->coverage);
        CHECK(link->quality_flags == 0U,
              "noninteger timing flags: %u", link->quality_flags);
    } else {
        CHECK(0, "noninteger timing link row count");
    }
    hwa_run_result_free(&result);

    CHECK(write_raw_file(files->hostile_probe, constant_csv,
                         sizeof(constant_csv) - 1U) &&
          write_timed_link_manifest(
              files->hostile_manifest, files->link_reference,
              files->link_model, files->hostile_probe, 8000U, INT64_MIN,
              UINT64_C(2), UINT64_MAX, UINT64_C(1)),
          "cannot make full-u64 rational fixture");
    CHECK(hwa_analyze_run_files(files->hostile_manifest, extreme_bindings, 3U,
                                NULL, &result, error, sizeof(error)) == 0,
          "full-u64 rational run failed: %s", error);
    if (result.link_count == 1U) {
        const HWARunLink *link = &result.links[0];
        CHECK(link->availability == HWA_RUN_INSUFFICIENT && !link->fit_valid,
              "constant full-u64 link fit unexpectedly");
        CHECK(link->point_count == 199U && link->coverage == 1.0,
              "full-u64 lookup dropped points: %zu/%.17g",
              link->point_count, link->coverage);
        CHECK(link->quality_flags == HWA_RUN_QUALITY_LOW_VARIANCE,
              "full-u64 lookup flags: %u", link->quality_flags);
    } else {
        CHECK(0, "full-u64 link row count");
    }
    hwa_run_result_free(&result);
}

static void test_result_truth_gate(const TestFiles *files)
{
    HWARunBinding bindings[3] = {
        {"target", files->link_reference},
        {"render", files->link_model},
        {"force.input", files->csv_probe}
    };
    HWARunResult result;
    char error[HWA_ERROR_SIZE];
    size_t reference_index = SIZE_MAX;
    size_t model_index = SIZE_MAX;
    size_t probe_index = SIZE_MAX;
    size_t index;
    uint64_t expected_evaluations;
    memset(&result, 0, sizeof(result));
    CHECK(hwa_analyze_run_files(files->csv_manifest, bindings, 3U, NULL,
                                &result, error, sizeof(error)) == 0,
          "truth-gate fixture failed: %s", error);
    for (index = 0U; index < result.source_count; ++index) {
        if (result.sources[index].kind == HWA_RUN_SOURCE_PROBE)
            probe_index = index;
        else if (result.sources[index].side == HWA_RUN_REFERENCE)
            reference_index = index;
        else
            model_index = index;
    }
    CHECK(reference_index != SIZE_MAX && model_index != SIZE_MAX &&
          probe_index != SIZE_MAX,
          "truth-gate source lookup");
    CHECK(hwa_run_evaluations_expected(&result, &expected_evaluations) == 0 &&
          expected_evaluations == result.evaluation_count,
          "shared evaluation derivation");
    if (reference_index != SIZE_MAX && model_index != SIZE_MAX &&
        probe_index != SIZE_MAX) {
        char saved_character = result.sources[probe_index].binding_id[1];
        result.sources[probe_index].binding_id[1] = '%';
        CHECK(hwa_run_result_validate(&result, error, sizeof(error)) != 0,
              "forged source token passed");
        result.sources[probe_index].binding_id[1] = saved_character;

        {
            char *saved = result.sources[0].binding_id;
            result.sources[0].binding_id = result.sources[1].binding_id;
            result.sources[1].binding_id = saved;
            CHECK(hwa_run_result_validate(&result, error, sizeof(error)) != 0,
                  "noncanonical source order passed");
            saved = result.sources[0].binding_id;
            result.sources[0].binding_id = result.sources[1].binding_id;
            result.sources[1].binding_id = saved;
        }
        result.sources[reference_index].role = HWA_RUN_STEM_BODY;
        CHECK(hwa_run_result_validate(&result, error, sizeof(error)) != 0,
              "forged reference role passed");
        result.sources[reference_index].role = HWA_RUN_STEM_FINAL;

        result.sources[model_index].format.bits_per_sample = 40U;
        CHECK(hwa_run_result_validate(&result, error, sizeof(error)) != 0,
              "unsupported saved WAVE shape passed");
        result.sources[model_index].format.bits_per_sample = 16U;

        result.clocks[0].start_offset_samples++;
        CHECK(hwa_run_result_validate(&result, error, sizeof(error)) != 0,
              "forged clock offset passed");
        result.clocks[0].start_offset_samples--;

        result.features[0].quality_flags ^= HWA_RUN_QUALITY_CLOCK_OFFSET;
        CHECK(hwa_run_result_validate(&result, error, sizeof(error)) != 0,
              "forged feature flags passed");
        result.features[0].quality_flags ^= HWA_RUN_QUALITY_CLOCK_OFFSET;

        result.probes[0].value_count--;
        CHECK(hwa_run_result_validate(&result, error, sizeof(error)) != 0,
              "forged probe count passed");
        result.probes[0].value_count++;

        {
            HWARunProbeFormat saved_format =
                result.sources[probe_index].probe_format;
            uint64_t saved_file_bytes =
                result.sources[probe_index].file_bytes;
            result.sources[probe_index].probe_format =
                HWA_RUN_PROBE_BINARY_F64LE;
            result.sources[probe_index].file_bytes =
                UINT64_C(16) + UINT64_C(8) *
                                     result.sources[probe_index].value_count;
            CHECK(hwa_run_result_validate(&result, error, sizeof(error)) == 0,
                  "valid binary probe byte count failed: %s", error);
            result.sources[probe_index].file_bytes++;
            CHECK(hwa_run_result_validate(&result, error, sizeof(error)) != 0,
                  "forged binary probe byte count passed");
            result.sources[probe_index].probe_format = saved_format;
            result.sources[probe_index].file_bytes = saved_file_bytes;
        }

        result.links[0].probe_source_id = result.sources[model_index].id;
        CHECK(hwa_run_result_validate(&result, error, sizeof(error)) != 0,
              "link to a stem as probe passed");
        result.links[0].probe_source_id = result.sources[probe_index].id;

        result.links[0].coverage = nextafter(result.links[0].coverage, 0.0);
        CHECK(hwa_run_result_validate(&result, error, sizeof(error)) != 0,
              "forged exact link coverage passed");
        result.links[0].coverage =
            (double)result.links[0].point_count / 199.0;

        result.evaluation_count++;
        CHECK(hwa_run_result_validate(&result, error, sizeof(error)) != 0,
              "forged evaluation ledger passed");
        result.evaluation_count--;
        CHECK(hwa_run_result_validate(&result, error, sizeof(error)) == 0,
              "restored truth-gate fixture failed: %s", error);
    }
    hwa_run_result_free(&result);
}

int main(void)
{
    TestFiles files;
    enum { PROBE_COUNT = 200 };
    double probe[PROBE_COUNT];
    uint32_t state = UINT32_C(0x12345678);
    size_t index;
    if (!files_open(&files)) {
        (void)fputs("cannot create Stage 7 test directory\n", stderr);
        return 1;
    }
    for (index = 0U; index < PROBE_COUNT; ++index) {
        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
        probe[index] = -22.0 + 4.0 * (double)(state & UINT32_C(0xffff)) /
                                      65535.0;
    }
    CHECK(write_wav(files.reference, 48000U, 48000U, 0.4, NULL, 0U, 0U) &&
          write_wav(files.source, 48000U, 48000U, 0.4, NULL, 0U, 0U) &&
          write_wav(files.body, 48000U, 48000U, 0.2, NULL, 0U, 0U) &&
          write_wav(files.wet, 48000U, 48000U, 0.2, NULL, 0U, 0U) &&
          write_wav(files.final, 48000U, 48000U, 0.2, NULL, 0U, 0U) &&
          write_stage_manifest(&files), "cannot make stage fixtures");
    CHECK(write_wav(files.link_reference, 8000U, 16000U, 0.1,
                    probe, PROBE_COUNT, 2U) &&
          write_wav(files.link_model, 8000U, 16000U, 0.1,
                    probe, PROBE_COUNT, 2U) &&
          write_probe_csv(files.csv_probe, probe, PROBE_COUNT) &&
          write_probe_binary(files.binary_probe, probe, PROBE_COUNT) &&
          write_link_manifest(files.csv_manifest, files.link_reference,
                              files.link_model, files.csv_probe,
                              "csv-f64", 0, PROBE_COUNT) &&
          write_link_manifest(files.binary_manifest, files.link_reference,
                              files.link_model, files.binary_probe,
                              "binary-f64le", 0, PROBE_COUNT) &&
          write_link_manifest(files.bad_manifest, files.link_reference,
                              files.link_model, files.csv_probe,
                              "csv-f64", 1, PROBE_COUNT),
          "cannot make probe fixtures");
    if (failures == 0) {
        test_stage_run(&files);
        test_probe_runs(&files, PROBE_COUNT);
        test_source_authority(&files);
        test_work_cap(&files);
        test_rational_timing(&files);
        test_result_truth_gate(&files);
    }
    files_close(&files);
    if (failures != 0) {
        (void)fprintf(stderr, "%d Stage 7 test(s) failed\n", failures);
        return 1;
    }
    (void)puts("Stage 7 run tests passed");
    return 0;
}

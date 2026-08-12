#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "gap_report_clip.h"
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
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int failures;

#define CHECK(condition, message)                                           \
    do {                                                                    \
        if (!(condition)) {                                                 \
            (void)fprintf(stderr, "FAIL: %s\n", message);                 \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static void put16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xffU);
    p[1] = (unsigned char)(v >> 8U);
}

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xffU);
    p[1] = (unsigned char)((v >> 8U) & 0xffU);
    p[2] = (unsigned char)((v >> 16U) & 0xffU);
    p[3] = (unsigned char)((v >> 24U) & 0xffU);
}

static int write_wave(const char *path, double amplitude, double frequency)
{
    const uint32_t rate = 48000U;
    const uint32_t frames = 48000U;
    const double pi = 3.14159265358979323846;
    unsigned char header[44];
    FILE *stream = fopen(path, "wb");
    uint32_t frame;
    if (stream == NULL) return 0;
    memset(header, 0, sizeof(header));
    memcpy(header, "RIFF", 4U);
    put32(header + 4U, UINT32_C(36) + frames * UINT32_C(2));
    memcpy(header + 8U, "WAVEfmt ", 8U);
    put32(header + 16U, UINT32_C(16));
    put16(header + 20U, UINT16_C(1));
    put16(header + 22U, UINT16_C(1));
    put32(header + 24U, rate);
    put32(header + 28U, rate * UINT32_C(2));
    put16(header + 32U, UINT16_C(2));
    put16(header + 34U, UINT16_C(16));
    memcpy(header + 36U, "data", 4U);
    put32(header + 40U, frames * UINT32_C(2));
    if (fwrite(header, 1U, sizeof(header), stream) != sizeof(header)) goto fail;
    for (frame = 0U; frame < frames; ++frame) {
        unsigned char sample[2];
        double value = amplitude * sin(2.0 * pi * frequency *
                                       (double)frame / (double)rate);
        int pcm = value >= 0.0 ? (int)floor(value * 32767.0 + 0.5)
                               : (int)ceil(value * 32768.0 - 0.5);
        put16(sample, (uint16_t)(int16_t)pcm);
        if (fwrite(sample, 1U, sizeof(sample), stream) != sizeof(sample))
            goto fail;
    }
    return fclose(stream) == 0;
fail:
    (void)fclose(stream);
    return 0;
}

static int mkdir_new(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

static int remove_tree(const char *path)
{
#if defined(_WIN32)
    char command[PATH_MAX + 32U];
    int length = snprintf(command, sizeof(command), "rmdir /s /q \"%s\"", path);
    return length > 0 && (size_t)length < sizeof(command) ? system(command) : -1;
#else
    struct stat facts;
    DIR *directory;
    struct dirent *entry;
    int status = 0;
    if (lstat(path, &facts) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(facts.st_mode)) return unlink(path);
    directory = opendir(path);
    if (directory == NULL) return -1;
    while ((entry = readdir(directory)) != NULL) {
        char child[PATH_MAX];
        int length;
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;
        length = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(child) ||
            remove_tree(child) != 0) status = -1;
    }
    if (closedir(directory) != 0) status = -1;
    if (rmdir(path) != 0) status = -1;
    return status;
#endif
}

static char *copy_text(const char *text)
{
    size_t size = strlen(text) + 1U;
    char *copy = (char *)malloc(size);
    if (copy != NULL) memcpy(copy, text, size);
    return copy;
}

static int make_workspace(char root[PATH_MAX])
{
    unsigned attempt;
#if defined(_WIN32)
    const char *temporary = getenv("TEMP");
    long process = (long)_getpid();
#else
    const char *temporary = "/tmp";
    long process = (long)getpid();
#endif
    if (temporary == NULL) return 0;
    for (attempt = 0U; attempt < 100U; ++attempt) {
        int length = snprintf(root, PATH_MAX, "%s/hwa-stage9-clip-%ld-%u",
                              temporary, process, attempt);
        if (length < 0 || length >= PATH_MAX) return 0;
        if (mkdir_new(root) == 0) return 1;
        if (errno != EEXIST) return 0;
    }
    return 0;
}

static int files_equal(const char *left, const char *right)
{
    FILE *a = fopen(left, "rb");
    FILE *b = fopen(right, "rb");
    int equal = a != NULL && b != NULL;
    while (equal) {
        unsigned char x[4096];
        unsigned char y[4096];
        size_t xn = fread(x, 1U, sizeof(x), a);
        size_t yn = fread(y, 1U, sizeof(y), b);
        if (xn != yn || memcmp(x, y, xn) != 0) equal = 0;
        if (xn != sizeof(x)) break;
    }
    if (a != NULL) (void)fclose(a);
    if (b != NULL) (void)fclose(b);
    return equal;
}

static uint64_t test_loudness_evaluation_bound(uint64_t frames)
{
    const uint64_t fft_size = UINT64_C(2048);
    const uint64_t hop_size = UINT64_C(512);
    const uint64_t fft_logarithm = UINT64_C(11);
    const uint64_t fixed_passes = UINT64_C(8);
    uint64_t windows = (frames - UINT64_C(1)) / hop_size + UINT64_C(1);
    uint64_t blocks = (frames - UINT64_C(1)) / UINT64_C(65536) + UINT64_C(1);
    return frames + blocks +
        windows * fft_size * (fft_logarithm + fixed_passes);
}

static uint64_t test_raw_feature_boundary(const HWAGapReportResult *result)
{
    const HWAGapReportExcerpt *excerpt = &result->excerpts[0];
    uint64_t lookup_visits = (uint64_t)result->candidate_count +
        UINT64_C(3) * (uint64_t)result->source_count;
    uint64_t render_visits = excerpt->frame_count * UINT64_C(11);
    return lookup_visits + render_visits +
        UINT64_C(2) * test_loudness_evaluation_bound(excerpt->frame_count);
}

static int initialize_result(HWAGapReportResult *result,
                             const char *root,
                             const char *reference_path,
                             const char *model_path,
                             const char *output,
                             HWAGapReportView view)
{
    char reference_hash[HWA_SHA256_HEX_SIZE];
    char model_hash[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    memset(result, 0, sizeof(*result));
    hwa_gap_report_options_default(&result->options);
    result->mode = HWA_GAP_REPORT_EXCERPTS;
    result->manifest_path = copy_text(root);
    result->output_directory = copy_text(output);
    result->title = copy_text("Clip test");
    result->audibility_method = copy_text(HWA_GAP_REPORT_AUDIBILITY_METHOD);
    memcpy(result->manifest_sha256,
           "000102030405060708090a0b0c0d0e0f"
           "101112131415161718191a1b1c1d1e1f", 65U);
    result->sources = (HWAGapReportSource *)calloc(3U, sizeof(*result->sources));
    result->candidates = (HWAGapReportCandidate *)calloc(
        1U, sizeof(*result->candidates));
    result->cases = (HWAGapReportCase *)calloc(1U, sizeof(*result->cases));
    result->excerpts = (HWAGapReportExcerpt *)calloc(1U, sizeof(*result->excerpts));
    if (result->manifest_path == NULL || result->output_directory == NULL ||
        result->title == NULL || result->audibility_method == NULL ||
        result->sources == NULL || result->candidates == NULL ||
        result->cases == NULL || result->excerpts == NULL ||
        hwa_sha256_file(reference_path, UINT64_C(1000000), reference_hash,
                        error, sizeof(error)) != 0 ||
        hwa_sha256_file(model_path, UINT64_C(1000000), model_hash,
                        error, sizeof(error)) != 0) return 0;
    result->source_count = 3U;
    result->sources[0].id = 1U;
    result->sources[0].name = copy_text("candidate");
    result->sources[0].kind = HWA_GAP_REPORT_SOURCE_EXPERIMENT;
    result->sources[0].path = copy_text(root);
    result->sources[0].candidate_count = 1U;
    memcpy(result->sources[0].sha256,
           "1111111111111111111111111111111111111111111111111111111111111111", 65U);
    result->sources[1].id = 2U;
    result->sources[1].name = copy_text("model");
    result->sources[1].kind = HWA_GAP_REPORT_SOURCE_WAVE;
    result->sources[1].path = copy_text(model_path);
    memcpy(result->sources[1].sha256, model_hash, sizeof(model_hash));
    result->sources[1].file_bytes = UINT64_C(96044);
    result->sources[2].id = 3U;
    result->sources[2].name = copy_text("reference");
    result->sources[2].kind = HWA_GAP_REPORT_SOURCE_WAVE;
    result->sources[2].path = copy_text(reference_path);
    memcpy(result->sources[2].sha256, reference_hash, sizeof(reference_hash));
    result->sources[2].file_bytes = UINT64_C(96044);
    result->total_input_bytes = UINT64_C(192088);
    result->candidate_count = 1U;
    result->candidates[0].id = 1U;
    result->candidates[0].source_id = 1U;
    result->candidates[0].source_row = 2U;
    result->candidates[0].case_id = copy_text("baseline-check");
    result->candidates[0].metric = copy_text("level");
    result->candidates[0].family_key = copy_text(
        "source:1:experiment:role:4:level");
    result->candidates[0].kind = HWA_GAP_REPORT_CANDIDATE_EXPERIMENT;
    result->candidates[0].availability = HWA_GAP_REPORT_AVAILABLE;
    result->candidates[0].raw_value = 0.5;
    result->candidates[0].raw_value_valid = 1;
    result->candidates[0].reason = copy_text("");
    result->case_count = 1U;
    result->cases[0].id = 1U;
    result->cases[0].candidate_id = 1U;
    result->cases[0].case_id = copy_text("case");
    result->cases[0].availability = HWA_GAP_REPORT_AVAILABLE;
    result->cases[0].value = 0.5;
    result->cases[0].confidence = 1.0;
    result->cases[0].value_valid = 1;
    result->cases[0].confidence_valid = 1;
    result->cases[0].reason = copy_text("");
    result->excerpt_count = 1U;
    result->excerpts[0].id = 1U;
    result->excerpts[0].name = copy_text("clip");
    result->excerpts[0].candidate_source_id = 1U;
    result->excerpts[0].candidate_row = 2U;
    result->excerpts[0].view = view;
    result->excerpts[0].reference_source_id = 3U;
    result->excerpts[0].model_source_id = 2U;
    result->excerpts[0].frame_count = UINT64_C(48000);
    result->excerpts[0].make_x = 1;
    result->excerpts[0].availability = HWA_GAP_REPORT_UNAVAILABLE;
    result->excerpts[0].reference_path = copy_text("");
    result->excerpts[0].model_path = copy_text("");
    result->excerpts[0].x_path = copy_text("");
    result->excerpts[0].reason = copy_text("excerpt-not-rendered");
    return hwa_gap_report_result_rebuild(result, error, sizeof(error)) == 0;
}

static void test_raw_and_no_replace(void)
{
    char root[PATH_MAX];
    char reference[PATH_MAX + 32U];
    char model[PATH_MAX + 32U];
    char output[PATH_MAX + 32U];
    char a[PATH_MAX + 64U];
    char b[PATH_MAX + 64U];
    char x[PATH_MAX + 64U];
    char error[HWA_ERROR_SIZE];
    HWAGapReportResult result;
    int ready;
    CHECK(make_workspace(root), "cannot create clip test workspace");
    if (failures != 0) return;
    (void)snprintf(reference, sizeof(reference), "%s/reference.wav", root);
    (void)snprintf(model, sizeof(model), "%s/model.wav", root);
    (void)snprintf(output, sizeof(output), "%s/bundle", root);
    CHECK(write_wave(reference, 0.5, 440.0) && write_wave(model, 0.2, 660.0),
          "cannot write clip WAVE fixtures");
    ready = initialize_result(&result, root, reference, model, output,
                              HWA_GAP_REPORT_VIEW_RAW);
    CHECK(ready, "cannot initialize RAW result fixture");
    if (ready) {
        if (hwa_gap_report_build_clip_bundle(
                &result, 0U, error, sizeof(error)) != 0) {
            (void)fprintf(stderr, "RAW bundle error: %s\n", error);
            CHECK(0, "RAW clip bundle failed");
        }
        (void)snprintf(a, sizeof(a), "%s/audio/clip-a.wav", output);
        (void)snprintf(b, sizeof(b), "%s/audio/clip-b.wav", output);
        (void)snprintf(x, sizeof(x), "%s/audio/clip-x.wav", output);
        CHECK(result.excerpts[0].availability == HWA_GAP_REPORT_AVAILABLE,
              "RAW excerpt did not become available");
        CHECK(result.excerpts[0].reference_gain_db <= 0.0 &&
              result.excerpts[0].model_gain_db <= 0.0,
              "loudness match amplified an excerpt");
        CHECK(files_equal(x, result.excerpts[0].x_is_reference ? a : b),
              "X is not an exact copy of the chosen A or B clip");
        CHECK(hwa_gap_report_build_clip_bundle(
                  &result, 0U, error, sizeof(error)) != 0,
              "clip bundle replaced an existing output");
        CHECK(files_equal(x, result.excerpts[0].x_is_reference ? a : b),
              "failed replace changed the committed output");
        hwa_gap_report_result_free(&result);
    }
    CHECK(remove_tree(root) == 0, "cannot clean clip test workspace");
}

static void test_view_stays_unavailable(void)
{
    char root[PATH_MAX];
    char reference[PATH_MAX + 32U];
    char model[PATH_MAX + 32U];
    char output[PATH_MAX + 32U];
    char error[HWA_ERROR_SIZE];
    HWAGapReportResult result;
    int ready;
    CHECK(make_workspace(root), "cannot create view test workspace");
    if (failures != 0) return;
    (void)snprintf(reference, sizeof(reference), "%s/reference.wav", root);
    (void)snprintf(model, sizeof(model), "%s/model.wav", root);
    (void)snprintf(output, sizeof(output), "%s/bundle", root);
    CHECK(write_wave(reference, 0.5, 440.0) && write_wave(model, 0.2, 660.0),
          "cannot write view WAVE fixtures");
    ready = initialize_result(&result, root, reference, model, output,
                              HWA_GAP_REPORT_VIEW_BROAD_EQ_MATCHED);
    CHECK(ready, "cannot initialize view result fixture");
    if (ready) {
        CHECK(hwa_gap_report_build_clip_bundle(
                  &result, 0U, error, sizeof(error)) == 0,
              "unavailable view report bundle failed");
        CHECK(result.excerpts[0].availability == HWA_GAP_REPORT_UNAVAILABLE &&
              strcmp(result.excerpts[0].reason,
                     "view-authority-unavailable") == 0,
              "unsupported view was mislabeled as available");
        CHECK(result.total_output_bytes == 0U,
              "unsupported view wrote hidden audio");
        hwa_gap_report_result_free(&result);
    }
    CHECK(remove_tree(root) == 0, "cannot clean view test workspace");
}

static void test_loudness_evaluation_precharge(void)
{
    char root[PATH_MAX];
    char reference[PATH_MAX + 32U];
    char model[PATH_MAX + 32U];
    char output[PATH_MAX + 32U];
    char error[HWA_ERROR_SIZE];
    HWAGapReportResult result;
    uint64_t boundary;
    uint64_t audio_bytes;
    int ready;
    CHECK(make_workspace(root), "cannot create evaluation test workspace");
    if (failures != 0) return;
    (void)snprintf(reference, sizeof(reference), "%s/reference.wav", root);
    (void)snprintf(model, sizeof(model), "%s/model.wav", root);
    CHECK(write_wave(reference, 0.5, 440.0) &&
              write_wave(model, 0.2, 660.0),
          "cannot write evaluation WAVE fixtures");

    (void)snprintf(output, sizeof(output), "%s/exact", root);
    ready = initialize_result(&result, root, reference, model, output,
                              HWA_GAP_REPORT_VIEW_RAW);
    CHECK(ready, "cannot initialize exact evaluation fixture");
    if (ready) {
        boundary = test_raw_feature_boundary(&result);
        audio_bytes = (result.excerpts[0].frame_count * UINT64_C(2) +
                       UINT64_C(44)) * UINT64_C(3);
        CHECK(boundary == UINT64_C(7939468),
              "Stage 1 evaluation boundary changed without a test update");
        result.options.max_evaluations = boundary;
        CHECK(hwa_gap_report_build_clip_bundle(
                  &result, 0U, error, sizeof(error)) != 0 &&
                  strcmp(error, "Stage 9 evaluation cap exceeded") == 0,
              "exact feature charge did not reach the later warning charge");
        CHECK(result.total_output_bytes == audio_bytes,
              "exact feature charge did not finish both loudness passes");
        hwa_gap_report_result_free(&result);
    }

    (void)snprintf(output, sizeof(output), "%s/one-under", root);
    ready = initialize_result(&result, root, reference, model, output,
                              HWA_GAP_REPORT_VIEW_RAW);
    CHECK(ready, "cannot initialize one-under evaluation fixture");
    if (ready) {
        boundary = test_raw_feature_boundary(&result);
        result.options.max_evaluations = boundary - UINT64_C(1);
        CHECK(hwa_gap_report_build_clip_bundle(
                  &result, 0U, error, sizeof(error)) != 0 &&
                  strcmp(error, "Stage 9 evaluation cap exceeded") == 0,
              "one-under feature charge was accepted");
        CHECK(result.total_output_bytes == 0U,
              "one-under feature charge ran a loudness pass");
        hwa_gap_report_result_free(&result);
    }
    CHECK(remove_tree(root) == 0,
          "cannot clean evaluation test workspace");
}

static void copy_hash(char target[HWA_SHA256_HEX_SIZE], const char *source)
{
    memcpy(target, source, HWA_SHA256_HEX_SIZE);
}

static void test_view_authority_facts(void)
{
    static const char hash_a[HWA_SHA256_HEX_SIZE] =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    static const char hash_b[HWA_SHA256_HEX_SIZE] =
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    static const char hash_c[HWA_SHA256_HEX_SIZE] =
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    static const char hash_d[HWA_SHA256_HEX_SIZE] =
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
    HWAProductionResult production;
    HWAProductionSource production_sources[3];
    HWAProductionViewRow production_rows[2];
    HWAProductionFit production_fits[
        HWA_PRODUCTION_EQ_NODE_COUNT + HWA_PRODUCTION_ROOM_BAND_COUNT * 2U];
    HWARunResult run;
    HWARunSource run_sources[2];
    HWARunFeature run_feature;
    HWARunLink run_link;
    double eq[HWA_PRODUCTION_EQ_NODE_COUNT];
    double early = 0.0;
    double late = 0.0;
    static const double room_early[HWA_PRODUCTION_ROOM_BAND_COUNT] = {
        -24.0, -20.0, -12.0, -10.0, -8.0, 0.0
    };
    static const double room_late[HWA_PRODUCTION_ROOM_BAND_COUNT] = {
        0.05, 0.10, 1.0, 2.0, 8.0, 10.0
    };
    size_t index;
    memset(&production, 0, sizeof(production));
    memset(production_sources, 0, sizeof(production_sources));
    memset(production_rows, 0, sizeof(production_rows));
    memset(production_fits, 0, sizeof(production_fits));
    production.sources = production_sources;
    production.source_count = 3U;
    production_sources[0].role = (char *)"reference:audio";
    production_sources[0].is_wave = 1;
    copy_hash(production_sources[0].sha256, hash_a);
    production_sources[1].role = (char *)"model:audio";
    production_sources[1].is_wave = 1;
    copy_hash(production_sources[1].sha256, hash_b);
    production_sources[2].role = (char *)"room-ir";
    production_sources[2].is_wave = 1;
    copy_hash(production_sources[2].sha256, hash_c);
    production.view_rows = production_rows;
    production.view_row_count = 2U;
    production_rows[0].id = 1U;
    production_rows[0].view = HWA_PRODUCTION_VIEW_DRY_LIKE;
    production_rows[1].id = 2U;
    production_rows[1].view = HWA_PRODUCTION_VIEW_ROOM_MATCHED;
    production.fits = production_fits;
    production.fit_count = sizeof(production_fits) / sizeof(production_fits[0]);
    for (index = 0U; index < HWA_PRODUCTION_EQ_NODE_COUNT; ++index) {
        HWAProductionFit *fit = &production_fits[index];
        fit->scope = HWA_PRODUCTION_SCOPE_CORRECTION;
        fit->kind = HWA_PRODUCTION_FIT_EQ_GAIN_DB;
        fit->index = (uint32_t)index;
        fit->availability = HWA_PRODUCTION_AVAILABLE;
        fit->estimate = (double)index - 3.0;
        fit->estimate_valid = 1;
    }
    for (index = 0U; index < HWA_PRODUCTION_ROOM_BAND_COUNT; ++index) {
        HWAProductionFit *early_fit =
            &production_fits[HWA_PRODUCTION_EQ_NODE_COUNT + index];
        HWAProductionFit *late_fit = &production_fits[
            HWA_PRODUCTION_EQ_NODE_COUNT + HWA_PRODUCTION_ROOM_BAND_COUNT +
            index];
        early_fit->scope = HWA_PRODUCTION_SCOPE_ROOM_IR;
        early_fit->kind = HWA_PRODUCTION_FIT_EARLY_REFLECTION_DB;
        early_fit->index = (uint32_t)index;
        early_fit->availability = HWA_PRODUCTION_AVAILABLE;
        early_fit->estimate = room_early[index];
        early_fit->estimate_valid = 1;
        late_fit->scope = HWA_PRODUCTION_SCOPE_ROOM_IR;
        late_fit->kind = HWA_PRODUCTION_FIT_LATE_DECAY_SECONDS;
        late_fit->index = (uint32_t)index;
        late_fit->availability = HWA_PRODUCTION_AVAILABLE;
        late_fit->estimate = room_late[index];
        late_fit->estimate_valid = 1;
    }
    CHECK(hwa_gap_report_production_view_authorized(
              &production, 1U, HWA_GAP_REPORT_VIEW_BROAD_EQ_MATCHED,
              hash_a, hash_b, NULL, eq, &early, &late) == 1,
          "valid broad-EQ authority was rejected");
    CHECK(hwa_gap_report_production_view_authorized(
              &production, 1U, HWA_GAP_REPORT_VIEW_BROAD_EQ_MATCHED,
              hash_d, hash_b, NULL, eq, &early, &late) == 0,
          "broad-EQ hash mismatch was accepted");
    CHECK(hwa_gap_report_production_view_authorized(
              &production, 2U, HWA_GAP_REPORT_VIEW_ROOM_MATCHED,
              hash_a, hash_b, hash_c, eq, &early, &late) == 1 &&
              early == -11.0 && late == 1.5,
          "valid room authority was rejected");
    CHECK(hwa_gap_report_production_view_authorized(
              &production, 2U, HWA_GAP_REPORT_VIEW_ROOM_MATCHED,
              hash_a, hash_b, hash_d, eq, &early, &late) == 0,
          "room hash mismatch was accepted");

    memset(&run, 0, sizeof(run));
    memset(run_sources, 0, sizeof(run_sources));
    memset(&run_feature, 0, sizeof(run_feature));
    memset(&run_link, 0, sizeof(run_link));
    run.sources = run_sources;
    run.source_count = 2U;
    run_sources[0].id = 1U;
    run_sources[0].kind = HWA_RUN_SOURCE_STEM;
    run_sources[0].side = HWA_RUN_REFERENCE;
    run_sources[0].role = HWA_RUN_STEM_FINAL;
    copy_hash(run_sources[0].sha256, hash_a);
    run_sources[1].id = 2U;
    run_sources[1].kind = HWA_RUN_SOURCE_STEM;
    run_sources[1].side = HWA_RUN_MODEL;
    run_sources[1].role = HWA_RUN_STEM_BODY;
    copy_hash(run_sources[1].sha256, hash_b);
    run.features = &run_feature;
    run.feature_count = 1U;
    run_feature.id = 7U;
    run_feature.role = HWA_RUN_STEM_BODY;
    run_feature.kind = HWA_RUN_FEATURE_RMS_DBFS;
    run_feature.index = 0U;
    run.links = &run_link;
    run.link_count = 1U;
    run_link.stem_source_id = 2U;
    run_link.feature = HWA_RUN_FEATURE_RMS_DBFS;
    run_link.feature_index = 0U;
    run_link.availability = HWA_RUN_AVAILABLE;
    run_link.fit_valid = 1;
    CHECK(hwa_gap_report_run_view_authorized(
              &run, 7U, HWA_GAP_REPORT_VIEW_STEM, hash_a, hash_b) == 1,
          "valid stem authority was rejected");
    CHECK(hwa_gap_report_run_view_authorized(
              &run, 7U, HWA_GAP_REPORT_VIEW_STEM, hash_a, hash_d) == 0,
          "stem hash mismatch was accepted");
    CHECK(hwa_gap_report_run_view_authorized(
              &run, 7U, HWA_GAP_REPORT_VIEW_PROBE_LINKED,
              hash_a, hash_b) == 1,
          "valid probe-link authority was rejected");
    run_link.availability = HWA_RUN_INSUFFICIENT;
    CHECK(hwa_gap_report_run_view_authorized(
              &run, 7U, HWA_GAP_REPORT_VIEW_PROBE_LINKED,
              hash_a, hash_b) == 0,
          "insufficient probe link was accepted");
}

int main(void)
{
    char error[HWA_ERROR_SIZE];
#if defined(_WIN32)
    CHECK(hwa_gap_report_clip_path_absolute("C:\\report") &&
              hwa_gap_report_clip_path_absolute("C:/report") &&
              hwa_gap_report_clip_path_absolute("\\\\server\\share\\report"),
          "Windows absolute output path forms were rejected");
    CHECK(!hwa_gap_report_clip_path_absolute("C:report") &&
              !hwa_gap_report_clip_path_absolute("report"),
          "Windows relative output path was accepted");
#else
    CHECK(hwa_gap_report_clip_path_absolute("/tmp/report") &&
              !hwa_gap_report_clip_path_absolute("tmp/report"),
          "POSIX absolute output path rule failed");
#endif
    CHECK(hwa_gap_report_build_clip_bundle(
              NULL, 0U, error, sizeof(error)) != 0,
          "null clip bundle input was accepted");
    test_raw_and_no_replace();
    test_view_stays_unavailable();
    test_loudness_evaluation_precharge();
    test_view_authority_facts();
    if (failures != 0) return EXIT_FAILURE;
    (void)puts("Stage 9 clip tests passed");
    return EXIT_SUCCESS;
}

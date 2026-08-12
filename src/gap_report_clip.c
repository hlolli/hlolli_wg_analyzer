#if !defined(_WIN32)
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "gap_report_clip.h"

#include "hwa_features.h"
#include "gap_report_output.h"
#include "internal.h"
#include "production.h"
#include "production_file.h"
#include "run_file.h"
#include "sha256.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#define HWA_GRC_SEPARATOR '\\'
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define HWA_GRC_SEPARATOR '/'
#endif

#if defined(__APPLE__)
#ifndef RENAME_EXCL
#define RENAME_EXCL 0x00000004
#endif
extern int renameatx_np(int from_directory, const char *from,
                        int to_directory, const char *to,
                        unsigned int flags);
#endif

#if defined(__linux__)
#include <sys/syscall.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0)
#endif
extern long syscall(long number, ...);
#endif

#if !defined(O_NOFOLLOW)
#define O_NOFOLLOW 0
#endif
#if !defined(O_CLOEXEC)
#define O_CLOEXEC 0
#endif

#define HWA_GRC_FEATURE_PUSH_FRAMES UINT64_C(65536)
#define HWA_GRC_FEATURE_FIXED_PASSES UINT64_C(8)

typedef struct HWAGRCIdentity {
#if defined(_WIN32)
    uint64_t device;
    uint64_t inode;
    int64_t size;
    int64_t modified;
    int64_t modified_nanoseconds;
    int64_t changed;
    int64_t changed_nanoseconds;
#else
    dev_t device;
    ino_t inode;
    off_t size;
    time_t modified;
    long modified_nanoseconds;
    time_t changed;
    long changed_nanoseconds;
#endif
} HWAGRCIdentity;

typedef struct HWAGRCFileOwnership {
    HWAGRCIdentity identity;
    int valid;
} HWAGRCFileOwnership;

typedef struct HWAGRCInput {
    int descriptor;
    HWAGRCIdentity identity;
    HWAWavReader reader;
} HWAGRCInput;

typedef struct HWAGRCClip {
    double *samples;
    uint64_t frame_count;
    uint32_t sample_rate_hz;
} HWAGRCClip;

typedef struct HWAGRCViewAuthority {
    double eq_gain[HWA_PRODUCTION_EQ_NODE_COUNT];
    double room_early_db;
    double room_late_seconds;
} HWAGRCViewAuthority;

static int hwa_grc_u64_add(uint64_t *value, uint64_t add)
{
    if (value == NULL || add > UINT64_MAX - *value) return -1;
    *value += add;
    return 0;
}

static int hwa_grc_u64_mul(uint64_t left, uint64_t right, uint64_t *value)
{
    if (value == NULL || (left != 0U && right > UINT64_MAX / left)) return -1;
    *value = left * right;
    return 0;
}

static int hwa_grc_evaluations_add(uint64_t *evaluations,
                                   uint64_t maximum,
                                   uint64_t add,
                                   char *error,
                                   size_t error_size)
{
    if (add > maximum || *evaluations > maximum - add) {
        hwa_set_error(error, error_size, "Stage 9 evaluation cap exceeded");
        return -1;
    }
    *evaluations += add;
    return 0;
}

/*
 * Stage 1 uses one input-frame pass and one fixed-size spectral FFT for each
 * analysis window when tracks are off. The fixed passes cover window fill,
 * spectral-bin scans, descriptor scans, setup, and finish. This bound also
 * charges each push block. It is reserved before either loudness pass starts.
 */
static int hwa_grc_loudness_evaluation_bound(uint64_t frames,
                                             uint64_t *evaluations)
{
    HWAAnalysisOptions options;
    uint64_t fft_size;
    uint64_t hop_size;
    uint64_t windows;
    uint64_t blocks;
    uint64_t logarithm = 0U;
    uint64_t cursor;
    uint64_t per_window;
    uint64_t window_work;
    uint64_t total = frames;
    if (evaluations == NULL) return -1;
    hwa_analysis_options_default(&options);
    fft_size = (uint64_t)options.frame_size;
    hop_size = (uint64_t)options.hop_size;
    if (fft_size == 0U || hop_size == 0U ||
        (fft_size & (fft_size - UINT64_C(1))) != 0U) return -1;
    for (cursor = fft_size; cursor > UINT64_C(1); cursor >>= 1U)
        logarithm++;
    windows = frames == 0U
        ? 0U : (frames - UINT64_C(1)) / hop_size + UINT64_C(1);
    blocks = frames == 0U
        ? 0U
        : (frames - UINT64_C(1)) / HWA_GRC_FEATURE_PUSH_FRAMES + UINT64_C(1);
    if (hwa_grc_u64_mul(
            fft_size, logarithm + HWA_GRC_FEATURE_FIXED_PASSES,
            &per_window) != 0 ||
        hwa_grc_u64_mul(windows, per_window, &window_work) != 0 ||
        hwa_grc_u64_add(&total, blocks) != 0 ||
        hwa_grc_u64_add(&total, window_work) != 0)
        return -1;
    *evaluations = total;
    return 0;
}

static char *hwa_grc_copy(const char *text)
{
    size_t size;
    char *copy;
    if (text == NULL) return NULL;
    size = strlen(text) + 1U;
    copy = (char *)malloc(size);
    if (copy != NULL) memcpy(copy, text, size);
    return copy;
}

static int hwa_grc_join(char **joined, const char *left, const char *right)
{
    size_t left_size;
    size_t right_size;
    int separator;
    char *path;
    if (joined == NULL || left == NULL || right == NULL || left[0] == '\0' ||
        right[0] == '\0') return -1;
    left_size = strlen(left);
    right_size = strlen(right);
    separator = left[left_size - 1U] != HWA_GRC_SEPARATOR;
    if (left_size > SIZE_MAX - right_size - (size_t)separator - 1U) return -1;
    path = (char *)malloc(left_size + right_size + (size_t)separator + 1U);
    if (path == NULL) return -1;
    memcpy(path, left, left_size);
    if (separator) path[left_size++] = HWA_GRC_SEPARATOR;
    memcpy(path + left_size, right, right_size + 1U);
    *joined = path;
    return 0;
}

int hwa_gap_report_clip_path_absolute(const char *path)
{
    if (path == NULL || path[0] == '\0') return 0;
#if defined(_WIN32)
    return (path[0] == '\\' && path[1] == '\\') ||
        (isalpha((unsigned char)path[0]) && path[1] == ':' &&
         (path[2] == '\\' || path[2] == '/'));
#else
    return path[0] == '/';
#endif
}

static int hwa_grc_component(const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    if (text == NULL || text[0] == '\0' || strcmp(text, ".") == 0 ||
        strcmp(text, "..") == 0) return 0;
    while (*cursor != 0U) {
        if (!isalnum(*cursor) && *cursor != '-' && *cursor != '_' &&
            *cursor != '.') return 0;
        cursor++;
    }
    return 1;
}

#if defined(_WIN32)
static void hwa_grc_windows_identity(
    const BY_HANDLE_FILE_INFORMATION *facts,
    HANDLE handle,
    HWAGRCIdentity *identity)
{
    FILE_BASIC_INFO basic;
    identity->device = (uint64_t)facts->dwVolumeSerialNumber;
    identity->inode = ((uint64_t)facts->nFileIndexHigh << 32U) |
                      (uint64_t)facts->nFileIndexLow;
    identity->size = (int64_t)(
        ((uint64_t)facts->nFileSizeHigh << 32U) |
        (uint64_t)facts->nFileSizeLow);
    identity->modified = (int64_t)(
        ((uint64_t)facts->ftLastWriteTime.dwHighDateTime << 32U) |
        (uint64_t)facts->ftLastWriteTime.dwLowDateTime);
    identity->modified_nanoseconds = 0;
    identity->changed =
        GetFileInformationByHandleEx(
            handle, FileBasicInfo, &basic, (DWORD)sizeof(basic))
            ? (int64_t)basic.ChangeTime.QuadPart
            : identity->modified;
    identity->changed_nanoseconds = 0;
}
#endif

static int hwa_grc_identity_fd(int descriptor, HWAGRCIdentity *identity)
{
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION facts;
    intptr_t raw = _get_osfhandle(descriptor);
    uint64_t size;
    if (raw == (intptr_t)-1 ||
        !GetFileInformationByHandle((HANDLE)raw, &facts) ||
        (facts.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U)
        return -1;
    size = ((uint64_t)facts.nFileSizeHigh << 32U) |
           (uint64_t)facts.nFileSizeLow;
    if (size > (uint64_t)INT64_MAX) return -1;
    hwa_grc_windows_identity(&facts, (HANDLE)raw, identity);
#else
    struct stat facts;
    if (fstat(descriptor, &facts) != 0 || !S_ISREG(facts.st_mode)) return -1;
    identity->device = facts.st_dev;
    identity->inode = facts.st_ino;
    identity->size = facts.st_size;
#if defined(__APPLE__)
    identity->modified = facts.st_mtimespec.tv_sec;
    identity->modified_nanoseconds = facts.st_mtimespec.tv_nsec;
    identity->changed = facts.st_ctimespec.tv_sec;
    identity->changed_nanoseconds = facts.st_ctimespec.tv_nsec;
#else
    identity->modified = facts.st_mtim.tv_sec;
    identity->modified_nanoseconds = facts.st_mtim.tv_nsec;
    identity->changed = facts.st_ctim.tv_sec;
    identity->changed_nanoseconds = facts.st_ctim.tv_nsec;
#endif
#endif
    return 0;
}

static int hwa_grc_identity_path(const char *path,
                                 int directory,
                                 HWAGRCIdentity *identity)
{
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION facts;
    HANDLE handle = CreateFileA(
        path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT |
            (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0U),
        NULL);
    uint64_t size;
    int is_directory;
    if (handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(handle, &facts)) {
        if (handle != INVALID_HANDLE_VALUE) (void)CloseHandle(handle);
        return -1;
    }
    is_directory =
        (facts.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    if ((facts.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        is_directory != directory) {
        (void)CloseHandle(handle);
        return -1;
    }
    size = ((uint64_t)facts.nFileSizeHigh << 32U) |
           (uint64_t)facts.nFileSizeLow;
    if (size > (uint64_t)INT64_MAX) {
        (void)CloseHandle(handle);
        return -1;
    }
    hwa_grc_windows_identity(&facts, handle, identity);
    (void)CloseHandle(handle);
#else
    struct stat facts;
    if (lstat(path, &facts) != 0 ||
        (directory ? !S_ISDIR(facts.st_mode) : !S_ISREG(facts.st_mode)))
        return -1;
    identity->device = facts.st_dev;
    identity->inode = facts.st_ino;
    identity->size = facts.st_size;
#if defined(__APPLE__)
    identity->modified = facts.st_mtimespec.tv_sec;
    identity->modified_nanoseconds = facts.st_mtimespec.tv_nsec;
    identity->changed = facts.st_ctimespec.tv_sec;
    identity->changed_nanoseconds = facts.st_ctimespec.tv_nsec;
#else
    identity->modified = facts.st_mtim.tv_sec;
    identity->modified_nanoseconds = facts.st_mtim.tv_nsec;
    identity->changed = facts.st_ctim.tv_sec;
    identity->changed_nanoseconds = facts.st_ctim.tv_nsec;
#endif
#endif
    return 0;
}

static int hwa_grc_identity_equal(const HWAGRCIdentity *left,
                                  const HWAGRCIdentity *right)
{
    return left->device == right->device && left->inode == right->inode &&
        left->size == right->size && left->modified == right->modified &&
        left->modified_nanoseconds == right->modified_nanoseconds &&
        left->changed == right->changed &&
        left->changed_nanoseconds == right->changed_nanoseconds;
}

static int hwa_grc_identity_same_node(const HWAGRCIdentity *left,
                                      const HWAGRCIdentity *right)
{
    return left->device == right->device && left->inode == right->inode;
}

static int hwa_grc_ownership_capture(const char *path,
                                     HWAGRCFileOwnership *owned)
{
    HWAGRCIdentity identity;
    if (owned == NULL || hwa_grc_identity_path(path, 0, &identity) != 0)
        return -1;
    owned->identity = identity;
    owned->valid = 1;
    return 0;
}

static int hwa_grc_ownership_after_close(
    const char *path,
    const HWAGRCIdentity *created,
    HWAGRCFileOwnership *owned)
{
    HWAGRCIdentity current;
    if (created == NULL || owned == NULL ||
        hwa_grc_identity_path(path, 0, &current) != 0) return -1;
#if defined(_WIN32)
    /* Windows can finalize write/change times when the last handle closes. */
    if (!hwa_grc_identity_same_node(created, &current) ||
        created->size != current.size) return -1;
#else
    if (!hwa_grc_identity_equal(created, &current)) return -1;
#endif
    owned->identity = current;
    owned->valid = 1;
    return 0;
}

static int hwa_grc_remove_owned_regular(const char *path,
                                        HWAGRCFileOwnership *owned)
{
    HWAGRCIdentity current;
    if (path == NULL || owned == NULL || !owned->valid) return 0;
    if (hwa_grc_identity_path(path, 0, &current) != 0 ||
        !hwa_grc_identity_equal(&owned->identity, &current)) return -1;
#if defined(_WIN32)
    if (!DeleteFileA(path)) return -1;
#else
    if (unlink(path) != 0) return -1;
#endif
    owned->valid = 0;
    return 0;
}

static int hwa_grc_hash_fd(int descriptor, char hex[HWA_SHA256_HEX_SIZE])
{
    unsigned char buffer[65536];
    unsigned char digest[32];
    HWASha256 hash;
#if defined(_WIN32)
    if (_lseeki64(descriptor, 0, SEEK_SET) < 0) return -1;
#else
    if (lseek(descriptor, (off_t)0, SEEK_SET) < 0) return -1;
#endif
    hwa_sha256_init(&hash);
    for (;;) {
#if defined(_WIN32)
        int count = _read(descriptor, buffer, (unsigned int)sizeof(buffer));
#else
        ssize_t count = read(descriptor, buffer, sizeof(buffer));
#endif
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (count == 0) break;
        hwa_sha256_update(&hash, buffer, (size_t)count);
    }
    hwa_sha256_final(&hash, digest);
    hwa_sha256_hex(digest, hex);
    return 0;
}

static const HWAGapReportSource *hwa_grc_source(
    const HWAGapReportResult *result, uint64_t id)
{
    size_t index;
    for (index = 0U; index < result->source_count; ++index)
        if (result->sources[index].id == id) return &result->sources[index];
    return NULL;
}

static const HWAGapReportCandidate *hwa_grc_candidate(
    const HWAGapReportResult *result, uint64_t source_id, uint64_t source_row)
{
    size_t index;
    const HWAGapReportCandidate *match = NULL;
    for (index = 0U; index < result->candidate_count; ++index) {
        const HWAGapReportCandidate *candidate = &result->candidates[index];
        if (candidate->source_id == source_id &&
            candidate->source_row == source_row) {
            if (match != NULL) return NULL;
            match = candidate;
        }
    }
    return match;
}

static int hwa_grc_source_hash_matches(const HWAGapReportSource *source,
                                       const char hash[HWA_SHA256_HEX_SIZE])
{
    return source != NULL && source->kind == HWA_GAP_REPORT_SOURCE_WAVE &&
        strcmp(source->sha256, hash) == 0;
}

static const HWAProductionFit *hwa_grc_production_fit(
    const HWAProductionResult *production,
    HWAProductionScope scope,
    HWAProductionFitKind kind,
    uint32_t index)
{
    size_t fit_index;
    for (fit_index = 0U; fit_index < production->fit_count; ++fit_index) {
        const HWAProductionFit *fit = &production->fits[fit_index];
        if (fit->scope == scope && fit->kind == kind && fit->index == index)
            return fit;
    }
    return NULL;
}

int hwa_gap_report_production_view_authorized(
    const HWAProductionResult *production,
    uint64_t candidate_row,
    HWAGapReportView view,
    const char reference_sha256[HWA_SHA256_HEX_SIZE],
    const char model_sha256[HWA_SHA256_HEX_SIZE],
    const char *room_sha256,
    double eq_gain[HWA_PRODUCTION_EQ_NODE_COUNT],
    double *room_early_db,
    double *room_late_seconds)
{
    const HWAProductionViewRow *row = NULL;
    const HWAProductionSource *reference_wave = NULL;
    const HWAProductionSource *model_wave = NULL;
    const HWAProductionSource *room_wave = NULL;
    size_t index;
    if (production == NULL || reference_sha256 == NULL ||
        model_sha256 == NULL || eq_gain == NULL || room_early_db == NULL ||
        room_late_seconds == NULL) return 0;
    for (index = 0U; index < production->view_row_count; ++index)
        if (production->view_rows[index].id == candidate_row) {
            row = &production->view_rows[index];
            break;
        }
    for (index = 0U; index < production->source_count; ++index) {
        const HWAProductionSource *source = &production->sources[index];
        if (!source->is_wave || source->role == NULL) continue;
        if (strcmp(source->role, "reference:audio") == 0) reference_wave = source;
        else if (strcmp(source->role, "model:audio") == 0) model_wave = source;
        else if (strcmp(source->role, "room-ir") == 0) room_wave = source;
    }
    if (row == NULL || reference_wave == NULL || model_wave == NULL ||
        strcmp(reference_wave->sha256, reference_sha256) != 0 ||
        strcmp(model_wave->sha256, model_sha256) != 0) return 0;
    if (view == HWA_GAP_REPORT_VIEW_BROAD_EQ_MATCHED) {
        if (row->view != HWA_PRODUCTION_VIEW_DRY_LIKE) return 0;
        for (index = 0U; index < HWA_PRODUCTION_EQ_NODE_COUNT; ++index) {
            const HWAProductionFit *fit = hwa_grc_production_fit(
                production, HWA_PRODUCTION_SCOPE_CORRECTION,
                HWA_PRODUCTION_FIT_EQ_GAIN_DB, (uint32_t)index);
            if (fit == NULL || fit->availability != HWA_PRODUCTION_AVAILABLE ||
                !fit->estimate_valid || !isfinite(fit->estimate)) return 0;
            eq_gain[index] = fit->estimate < -6.0 ? -6.0 :
                fit->estimate > 6.0 ? 6.0 : fit->estimate;
        }
        return 1;
    }
    if (view == HWA_GAP_REPORT_VIEW_ROOM_MATCHED) {
        double early[HWA_PRODUCTION_ROOM_BAND_COUNT];
        double late[HWA_PRODUCTION_ROOM_BAND_COUNT];
        if (row->view != HWA_PRODUCTION_VIEW_ROOM_MATCHED ||
            room_wave == NULL || room_sha256 == NULL ||
            strcmp(room_wave->sha256, room_sha256) != 0) return 0;
        for (index = 0U; index < HWA_PRODUCTION_ROOM_BAND_COUNT; ++index) {
            const HWAProductionFit *early_fit = hwa_grc_production_fit(
                production, HWA_PRODUCTION_SCOPE_ROOM_IR,
                HWA_PRODUCTION_FIT_EARLY_REFLECTION_DB, (uint32_t)index);
            const HWAProductionFit *late_fit = hwa_grc_production_fit(
                production, HWA_PRODUCTION_SCOPE_ROOM_IR,
                HWA_PRODUCTION_FIT_LATE_DECAY_SECONDS, (uint32_t)index);
            if (early_fit == NULL || late_fit == NULL ||
                early_fit->availability != HWA_PRODUCTION_AVAILABLE ||
                late_fit->availability != HWA_PRODUCTION_AVAILABLE ||
                !early_fit->estimate_valid || !late_fit->estimate_valid ||
                !isfinite(early_fit->estimate) ||
                !isfinite(late_fit->estimate)) return 0;
            early[index] = early_fit->estimate;
            late[index] = late_fit->estimate;
        }
        for (index = 1U; index < HWA_PRODUCTION_ROOM_BAND_COUNT; ++index) {
            size_t position = index;
            double value = early[index];
            while (position != 0U && early[position - 1U] > value) {
                early[position] = early[position - 1U];
                position--;
            }
            early[position] = value;
            position = index;
            value = late[index];
            while (position != 0U && late[position - 1U] > value) {
                late[position] = late[position - 1U];
                position--;
            }
            late[position] = value;
        }
        *room_early_db =
            (early[HWA_PRODUCTION_ROOM_BAND_COUNT / 2U - 1U] +
             early[HWA_PRODUCTION_ROOM_BAND_COUNT / 2U]) / 2.0;
        *room_late_seconds =
            (late[HWA_PRODUCTION_ROOM_BAND_COUNT / 2U - 1U] +
             late[HWA_PRODUCTION_ROOM_BAND_COUNT / 2U]) / 2.0;
        return 1;
    }
    return 0;
}

int hwa_gap_report_run_view_authorized(
    const HWARunResult *run,
    uint64_t candidate_row,
    HWAGapReportView view,
    const char reference_sha256[HWA_SHA256_HEX_SIZE],
    const char model_sha256[HWA_SHA256_HEX_SIZE])
{
    const HWARunFeature *feature = NULL;
    const HWARunSource *reference_stem = NULL;
    const HWARunSource *model_stem = NULL;
    size_t index;
    if (run == NULL || reference_sha256 == NULL || model_sha256 == NULL)
        return 0;
    for (index = 0U; index < run->feature_count; ++index)
        if (run->features[index].id == candidate_row) {
            feature = &run->features[index];
            break;
        }
    if (feature == NULL) return 0;
    for (index = 0U; index < run->source_count; ++index) {
        const HWARunSource *source = &run->sources[index];
        if (source->kind != HWA_RUN_SOURCE_STEM) continue;
        if (source->side == HWA_RUN_REFERENCE &&
            source->role == HWA_RUN_STEM_FINAL) reference_stem = source;
        if (source->side == HWA_RUN_MODEL &&
            source->role == feature->role) model_stem = source;
    }
    if (reference_stem == NULL || model_stem == NULL ||
        strcmp(reference_stem->sha256, reference_sha256) != 0 ||
        strcmp(model_stem->sha256, model_sha256) != 0) return 0;
    if (view == HWA_GAP_REPORT_VIEW_STEM) return 1;
    if (view != HWA_GAP_REPORT_VIEW_PROBE_LINKED) return 0;
    for (index = 0U; index < run->link_count; ++index) {
        const HWARunLink *link = &run->links[index];
        if (link->stem_source_id == model_stem->id &&
            link->feature == feature->kind &&
            link->feature_index == feature->index &&
            link->availability == HWA_RUN_AVAILABLE && link->fit_valid)
            return 1;
    }
    return 0;
}

static int hwa_grc_view_production_authority(
    const HWAGapReportResult *result,
    const HWAGapReportCandidate *candidate,
    const HWAGapReportSource *reference,
    const HWAGapReportSource *model,
    HWAGapReportView view,
    HWAGRCViewAuthority *authority,
    uint64_t outer_path_work,
    uint64_t *evaluations,
    char *error,
    size_t error_size)
{
    const HWAGapReportSource *report_source = hwa_grc_source(
        result, candidate->source_id);
    HWAProductionResult production;
    HWAProductionOptions limits;
    char hash[HWA_SHA256_HEX_SIZE];
    const char *room_hash = NULL;
    uint64_t live_work;
    size_t index;
    int valid;
    memset(&production, 0, sizeof(production));
    if (candidate->kind != HWA_GAP_REPORT_CANDIDATE_PRODUCTION ||
        report_source == NULL) return 0;
    live_work = result->retained_work_bytes;
    if (hwa_grc_u64_add(&live_work, outer_path_work) != 0 ||
        live_work >= result->options.max_work_bytes) {
        hwa_set_error(error, error_size,
                      "Stage 9 view authority exceeds its work cap");
        return -1;
    }
    limits = result->options.production;
    limits.max_work_bytes = result->options.max_work_bytes -
        live_work;
    if (*evaluations >= result->options.max_evaluations) {
        hwa_set_error(error, error_size,
                      "Stage 9 view authority exceeds its evaluation cap");
        return -1;
    }
    if (limits.max_evaluations >
        result->options.max_evaluations - *evaluations)
        limits.max_evaluations =
            result->options.max_evaluations - *evaluations;
    if (hwa_production_file_read(report_source->path,
            &limits, &production, hash,
            error, error_size) != 0 || strcmp(hash, report_source->sha256) != 0) {
        hwa_production_result_free(&production);
        return -1;
    }
    if (hwa_grc_evaluations_add(evaluations,
            result->options.max_evaluations, production.evaluation_count,
            error, error_size) != 0) {
        hwa_production_result_free(&production);
        return -1;
    }
    {
        uint64_t scans = (uint64_t)production.view_row_count;
        uint64_t fit_scans;
        if (hwa_grc_u64_add(&scans,
                (uint64_t)production.source_count) != 0 ||
            hwa_grc_u64_mul((uint64_t)production.fit_count,
                view == HWA_GAP_REPORT_VIEW_ROOM_MATCHED ?
                    UINT64_C(12) : UINT64_C(7), &fit_scans) != 0 ||
            hwa_grc_u64_add(&scans, fit_scans) != 0 ||
            (view == HWA_GAP_REPORT_VIEW_ROOM_MATCHED &&
             (hwa_grc_u64_mul((uint64_t)production.source_count,
                  (uint64_t)result->source_count, &fit_scans) != 0 ||
              hwa_grc_u64_add(&scans, fit_scans) != 0)) ||
            hwa_grc_evaluations_add(evaluations,
                result->options.max_evaluations, scans,
                error, error_size) != 0) {
            hwa_production_result_free(&production);
            return -1;
        }
    }
    if (view == HWA_GAP_REPORT_VIEW_ROOM_MATCHED) {
        for (index = 0U; index < production.source_count; ++index) {
            const HWAProductionSource *source = &production.sources[index];
            size_t bound;
            if (!source->is_wave || source->role == NULL ||
                strcmp(source->role, "room-ir") != 0) continue;
            for (bound = 0U; bound < result->source_count; ++bound)
                if (result->sources[bound].id != reference->id &&
                    result->sources[bound].id != model->id &&
                    hwa_grc_source_hash_matches(
                        &result->sources[bound], source->sha256)) {
                    room_hash = source->sha256;
                    break;
                }
        }
    }
    valid = hwa_gap_report_production_view_authorized(
        &production, candidate->source_row, view,
        reference->sha256, model->sha256, room_hash,
        authority->eq_gain, &authority->room_early_db,
        &authority->room_late_seconds);
    hwa_production_result_free(&production);
    return valid;
}

static int hwa_grc_view_run_authority(
    const HWAGapReportResult *result,
    const HWAGapReportCandidate *candidate,
    const HWAGapReportSource *reference,
    const HWAGapReportSource *model,
    HWAGapReportView view,
    uint64_t outer_path_work,
    uint64_t *evaluations,
    char *error,
    size_t error_size)
{
    const HWAGapReportSource *report_source = hwa_grc_source(
        result, candidate->source_id);
    HWARunResult run;
    HWARunOptions limits;
    char hash[HWA_SHA256_HEX_SIZE];
    uint64_t live_work;
    int valid;
    memset(&run, 0, sizeof(run));
    if (candidate->kind != HWA_GAP_REPORT_CANDIDATE_RUN_FEATURE ||
        report_source == NULL) return 0;
    live_work = result->retained_work_bytes;
    if (hwa_grc_u64_add(&live_work, outer_path_work) != 0 ||
        live_work >= result->options.max_work_bytes) {
        hwa_set_error(error, error_size,
                      "Stage 9 view authority exceeds its work cap");
        return -1;
    }
    limits = result->options.run;
    limits.max_work_bytes = result->options.max_work_bytes -
        live_work;
    if (*evaluations >= result->options.max_evaluations) {
        hwa_set_error(error, error_size,
                      "Stage 9 view authority exceeds its evaluation cap");
        return -1;
    }
    if (limits.max_evaluations >
        result->options.max_evaluations - *evaluations)
        limits.max_evaluations =
            result->options.max_evaluations - *evaluations;
    if (hwa_run_file_read(report_source->path, &limits,
                          &run, hash, error, error_size) != 0 ||
        strcmp(hash, report_source->sha256) != 0) {
        hwa_run_result_free(&run);
        return -1;
    }
    if (hwa_grc_evaluations_add(evaluations,
            result->options.max_evaluations, run.evaluation_count,
            error, error_size) != 0) {
        hwa_run_result_free(&run);
        return -1;
    }
    {
        uint64_t scans = (uint64_t)run.feature_count;
        if (hwa_grc_u64_add(&scans, (uint64_t)run.source_count) != 0 ||
            hwa_grc_u64_add(&scans, (uint64_t)run.link_count) != 0 ||
            hwa_grc_evaluations_add(evaluations,
                result->options.max_evaluations, scans,
                error, error_size) != 0) {
            hwa_run_result_free(&run);
            return -1;
        }
    }
    valid = hwa_gap_report_run_view_authorized(
        &run, candidate->source_row, view,
        reference->sha256, model->sha256);
    hwa_run_result_free(&run);
    return valid;
}

static void hwa_grc_input_close(HWAGRCInput *input)
{
    if (input == NULL) return;
    hwa_wav_reader_close(&input->reader);
    if (input->descriptor >= 0) {
#if defined(_WIN32)
        (void)_close(input->descriptor);
#else
        (void)close(input->descriptor);
#endif
    }
    memset(input, 0, sizeof(*input));
    input->descriptor = -1;
}

static int hwa_grc_input_open(const HWAGapReportSource *source,
                              const HWAGapReportOptions *options,
                              HWAGRCInput *input,
                              char *error,
                              size_t error_size)
{
    char hash[HWA_SHA256_HEX_SIZE];
    HWAGRCIdentity current;
    FILE *reader_stream = NULL;
    int reader_descriptor = -1;
    memset(input, 0, sizeof(*input));
    input->descriptor = -1;
    if (source == NULL || options == NULL || source->kind != HWA_GAP_REPORT_SOURCE_WAVE ||
        source->path == NULL || source->path[0] == '\0' ||
        source->file_bytes > options->max_input_bytes) {
        hwa_set_error(error, error_size, "invalid Stage 9 WAVE source");
        return -1;
    }
#if defined(_WIN32)
    {
        HANDLE handle = CreateFileA(
            source->path, GENERIC_READ, FILE_SHARE_READ,
            NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        BY_HANDLE_FILE_INFORMATION information;
        if (handle == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandle(handle, &information) ||
            (information.dwFileAttributes &
             (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0U) {
            if (handle != INVALID_HANDLE_VALUE) (void)CloseHandle(handle);
            hwa_set_error(error, error_size,
                          "Stage 9 WAVE input is not a regular named file");
            return -1;
        }
        input->descriptor = _open_osfhandle(
            (intptr_t)handle, _O_RDONLY | _O_BINARY | _O_NOINHERIT);
        if (input->descriptor < 0) (void)CloseHandle(handle);
    }
#else
    {
        struct stat named;
        if (lstat(source->path, &named) != 0 || !S_ISREG(named.st_mode)) {
            hwa_set_error(error, error_size,
                          "Stage 9 WAVE input is not a regular named file");
            return -1;
        }
    }
    input->descriptor = open(source->path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
#endif
    if (input->descriptor < 0 ||
        hwa_grc_identity_fd(input->descriptor, &input->identity) != 0 ||
        input->identity.size < 0 ||
        (uint64_t)input->identity.size != source->file_bytes ||
        hwa_grc_hash_fd(input->descriptor, hash) != 0 ||
        strcmp(hash, source->sha256) != 0) {
        hwa_set_error(error, error_size,
                      "Stage 9 WAVE input does not match its authority");
        hwa_grc_input_close(input);
        return -1;
    }
#if defined(_WIN32)
    reader_descriptor = _dup(input->descriptor);
    if (reader_descriptor >= 0)
        reader_stream = _fdopen(reader_descriptor, "rb");
#else
    reader_descriptor = dup(input->descriptor);
    if (reader_descriptor >= 0)
        reader_stream = fdopen(reader_descriptor, "rb");
#endif
    if (reader_stream == NULL) {
        if (reader_descriptor >= 0) {
#if defined(_WIN32)
            (void)_close(reader_descriptor);
#else
            (void)close(reader_descriptor);
#endif
        }
        hwa_grc_input_close(input);
        hwa_set_error(error, error_size,
                      "cannot duplicate the checked Stage 9 WAVE input");
        return -1;
    }
    if (hwa_wav_reader_open_file(&input->reader, reader_stream,
                            source->file_bytes,
                            error, error_size) != 0 ||
        hwa_grc_identity_fd(input->descriptor, &current) != 0 ||
        !hwa_grc_identity_equal(&input->identity, &current)
#if defined(_WIN32)
        || hwa_grc_identity_fd(_fileno(input->reader.file), &current) != 0 ||
#else
        || hwa_grc_identity_fd(fileno(input->reader.file), &current) != 0 ||
#endif
        !hwa_grc_identity_equal(&input->identity, &current)
        ) {
        if (error != NULL && error_size != 0U && error[0] == '\0')
            hwa_set_error(error, error_size,
                          "Stage 9 WAVE changed while it was opened");
        hwa_grc_input_close(input);
        return -1;
    }
    return 0;
}

static int hwa_grc_input_verify(HWAGRCInput *input,
                                const HWAGapReportSource *source,
                                char *error,
                                size_t error_size)
{
    HWAGRCIdentity current;
    char hash[HWA_SHA256_HEX_SIZE];
    if (hwa_grc_identity_fd(input->descriptor, &current) != 0 ||
        !hwa_grc_identity_equal(&input->identity, &current) ||
        hwa_grc_hash_fd(input->descriptor, hash) != 0 ||
        strcmp(hash, source->sha256) != 0) {
        hwa_set_error(error, error_size,
                      "Stage 9 WAVE changed during excerpt decode");
        return -1;
    }
    return 0;
}

static int hwa_grc_decode_mono(HWAGRCInput *input,
                               uint64_t start,
                               uint64_t frame_count,
                               size_t block_frames,
                               HWAGRCClip *clip,
                               char *error,
                               size_t error_size)
{
    unsigned char *buffer = NULL;
    uint64_t bytes;
    uint64_t skipped = 0U;
    uint64_t written = 0U;
    size_t allocation;
    if (input == NULL || clip == NULL || block_frames == 0U ||
        start > input->reader.format.frames ||
        frame_count > input->reader.format.frames - start ||
        frame_count > (uint64_t)SIZE_MAX ||
        hwa_grc_u64_mul((uint64_t)block_frames,
                        (uint64_t)input->reader.format.block_align,
                        &bytes) != 0 || bytes > (uint64_t)SIZE_MAX ||
        frame_count > (uint64_t)(SIZE_MAX / sizeof(*clip->samples))) {
        hwa_set_error(error, error_size, "invalid Stage 9 excerpt span");
        return -1;
    }
    allocation = (size_t)bytes;
    buffer = (unsigned char *)malloc(allocation);
    clip->samples = (double *)malloc((size_t)frame_count * sizeof(*clip->samples));
    if (buffer == NULL || clip->samples == NULL) {
        free(buffer);
        free(clip->samples);
        memset(clip, 0, sizeof(*clip));
        hwa_set_error(error, error_size, "cannot allocate Stage 9 excerpt work");
        return -1;
    }
    clip->frame_count = frame_count;
    clip->sample_rate_hz = input->reader.format.sample_rate_hz;
    while (skipped < start) {
        uint64_t left = start - skipped;
        size_t request = left < (uint64_t)block_frames ? (size_t)left : block_frames;
        size_t got = 0U;
        if (hwa_wav_reader_read_frames(&input->reader, buffer, request, &got,
                                       error, error_size) != 0 || got == 0U) {
            hwa_set_error(error, error_size, "truncated Stage 9 excerpt prefix");
            goto failed;
        }
        skipped += (uint64_t)got;
    }
    while (written < frame_count) {
        uint64_t left = frame_count - written;
        size_t request = left < (uint64_t)block_frames ? (size_t)left : block_frames;
        size_t got = 0U;
        size_t frame;
        if (hwa_wav_reader_read_frames(&input->reader, buffer, request, &got,
                                       error, error_size) != 0 || got == 0U) {
            hwa_set_error(error, error_size, "truncated Stage 9 excerpt audio");
            goto failed;
        }
        for (frame = 0U; frame < got; ++frame) {
            const unsigned char *source = buffer +
                frame * (size_t)input->reader.format.block_align;
            long double sum = 0.0L;
            uint16_t channel;
            for (channel = 0U; channel < input->reader.format.channels; ++channel) {
                int clipped = 0;
                double sample = hwa_wav_decode_sample(
                    &input->reader,
                    source + (size_t)channel * input->reader.bytes_per_sample,
                    &clipped);
                (void)clipped;
                if (!isfinite(sample)) {
                    hwa_set_error(error, error_size,
                                  "non-finite Stage 9 excerpt sample");
                    goto failed;
                }
                sum += (long double)sample;
            }
            clip->samples[(size_t)written + frame] =
                (double)(sum / (long double)input->reader.format.channels);
        }
        written += (uint64_t)got;
    }
    free(buffer);
    return 0;
failed:
    free(buffer);
    free(clip->samples);
    memset(clip, 0, sizeof(*clip));
    return -1;
}

static void hwa_grc_clip_free(HWAGRCClip *clip)
{
    if (clip != NULL) {
        free(clip->samples);
        memset(clip, 0, sizeof(*clip));
    }
}

static int hwa_grc_loudness(const HWAGRCClip *clip,
                            uint64_t max_work_bytes,
                            double *lufs,
                            char *error,
                            size_t error_size)
{
    HWAAnalysisOptions options;
    HWAAnalysis analysis;
    HWAFeatureProcessor *processor = NULL;
    uint64_t offset = 0U;
    int status = -1;
    memset(&analysis, 0, sizeof(analysis));
    hwa_analysis_options_default(&options);
    options.collect_tracks = 0;
    options.collect_spectrogram = 0;
    options.max_work_bytes = max_work_bytes;
    options.max_input_frames = clip->frame_count;
    while (offset < clip->frame_count) {
        uint64_t left = clip->frame_count - offset;
        size_t count = left < HWA_GRC_FEATURE_PUSH_FRAMES
            ? (size_t)left : (size_t)HWA_GRC_FEATURE_PUSH_FRAMES;
        if (processor == NULL && hwa_features_create(
                &processor, clip->sample_rate_hz, 1U, 0U,
                clip->frame_count, &options, error, error_size) != 0)
            goto cleanup;
        if (hwa_features_push(processor, clip->samples + (size_t)offset,
                              NULL, count, error, error_size) != 0)
            goto cleanup;
        offset += (uint64_t)count;
    }
    if (processor == NULL ||
        hwa_features_finish(processor, &analysis, error, error_size) != 0)
        goto cleanup;
    if (!analysis.loudness.integrated_valid ||
        !isfinite(analysis.loudness.integrated_lufs)) {
        status = 1;
        goto cleanup;
    }
    *lufs = analysis.loudness.integrated_lufs;
    status = 0;
cleanup:
    hwa_features_destroy(processor);
    hwa_analysis_free(&analysis);
    return status;
}

static void hwa_grc_prepare_pair(HWAGRCClip *reference,
                                 HWAGRCClip *model,
                                 double reference_lufs,
                                 double model_lufs,
                                 double *reference_gain_db,
                                 double *model_gain_db)
{
    const double pi = 3.14159265358979323846264338327950288;
    const double ceiling = 0.8912509381337456; /* -1 dBFS */
    double target = reference_lufs < model_lufs ? reference_lufs : model_lufs;
    double reference_db = target - reference_lufs;
    double model_db = target - model_lufs;
    double reference_gain = pow(10.0, reference_db / 20.0);
    double model_gain = pow(10.0, model_db / 20.0);
    double peak = 0.0;
    double common = 1.0;
    uint64_t frame;
    uint64_t fade = ((uint64_t)reference->sample_rate_hz + UINT64_C(50)) /
        UINT64_C(100);
    uint64_t half = reference->frame_count / UINT64_C(2);
    if (fade == 0U) fade = 1U;
    if (fade > half) fade = half;
    for (frame = 0U; frame < reference->frame_count; ++frame) {
        double r = fabs(reference->samples[(size_t)frame] * reference_gain);
        double m = fabs(model->samples[(size_t)frame] * model_gain);
        if (r > peak) peak = r;
        if (m > peak) peak = m;
    }
    if (peak > ceiling) common = ceiling / peak;
    if (common < 1.0) {
        double common_db = 20.0 * log10(common);
        reference_db += common_db;
        model_db += common_db;
        reference_gain *= common;
        model_gain *= common;
    }
    for (frame = 0U; frame < reference->frame_count; ++frame) {
        double envelope = 1.0;
        if (fade != 0U && frame < fade) {
            double phase = (double)frame / (double)fade;
            envelope *= 0.5 - 0.5 * cos(pi * phase);
        }
        if (fade != 0U && reference->frame_count - frame <= fade) {
            double phase = (double)(reference->frame_count - frame - 1U) /
                (double)fade;
            envelope *= 0.5 - 0.5 * cos(pi * phase);
        }
        reference->samples[(size_t)frame] *= reference_gain * envelope;
        model->samples[(size_t)frame] *= model_gain * envelope;
    }
    *reference_gain_db = reference_db == 0.0 ? 0.0 : reference_db;
    *model_gain_db = model_db == 0.0 ? 0.0 : model_db;
}

static int hwa_grc_apply_eq(HWAGRCClip *clip,
                            const HWAGRCViewAuthority *authority)
{
    size_t node;
    const double pi = 3.14159265358979323846264338327950288;
    for (node = 0U; node < HWA_PRODUCTION_EQ_NODE_COUNT; ++node) {
        double gain = authority->eq_gain[node];
        double a = pow(10.0, gain / 40.0);
        double omega = 2.0 * pi *
            hwa_production_eq_node_frequency_hz(node) /
            (double)clip->sample_rate_hz;
        double alpha = sin(omega) / 2.0; /* Q = 1 */
        double a0 = 1.0 + alpha / a;
        double b0 = (1.0 + alpha * a) / a0;
        double b1 = (-2.0 * cos(omega)) / a0;
        double b2 = (1.0 - alpha * a) / a0;
        double a1 = (-2.0 * cos(omega)) / a0;
        double a2 = (1.0 - alpha / a) / a0;
        double x1 = 0.0;
        double x2 = 0.0;
        double y1 = 0.0;
        double y2 = 0.0;
        uint64_t frame;
        for (frame = 0U; frame < clip->frame_count; ++frame) {
            double input = clip->samples[(size_t)frame];
            double output = b0 * input + b1 * x1 + b2 * x2 -
                a1 * y1 - a2 * y2;
            if (!isfinite(output)) return -1;
            clip->samples[(size_t)frame] = output;
            x2 = x1;
            x1 = input;
            y2 = y1;
            y1 = output;
        }
    }
    return 0;
}

static int hwa_grc_apply_room(HWAGRCClip *clip,
                              const HWAGRCViewAuthority *authority)
{
    uint64_t delay = ((uint64_t)clip->sample_rate_hz * UINT64_C(20) +
                      UINT64_C(500)) / UINT64_C(1000);
    double early = authority->room_early_db;
    double late = authority->room_late_seconds;
    double reflection;
    double feedback;
    double *dry_delay = NULL;
    double *wet_delay = NULL;
    uint64_t frame;
    if (delay == 0U) delay = 1U;
    if (early < -24.0) early = -24.0;
    if (early > 0.0) early = 0.0;
    if (late < 0.05) late = 0.05;
    if (late > 10.0) late = 10.0;
    reflection = pow(10.0, early / 20.0);
    feedback = pow(10.0,
        -3.0 * ((double)delay / (double)clip->sample_rate_hz) / late);
    if (delay > (uint64_t)(SIZE_MAX / sizeof(*dry_delay))) return -1;
    dry_delay = (double *)calloc((size_t)delay, sizeof(*dry_delay));
    wet_delay = (double *)calloc((size_t)delay, sizeof(*wet_delay));
    if (dry_delay == NULL || wet_delay == NULL) {
        free(dry_delay);
        free(wet_delay);
        return -1;
    }
    for (frame = 0U; frame < clip->frame_count; ++frame) {
        size_t slot = (size_t)(frame % delay);
        double dry = clip->samples[(size_t)frame];
        double delayed_dry = frame >= delay ? dry_delay[slot] : 0.0;
        double delayed_wet = frame >= delay ? wet_delay[slot] : 0.0;
        double output = dry + reflection * delayed_dry +
            feedback * delayed_wet;
        if (!isfinite(output)) {
            free(dry_delay);
            free(wet_delay);
            return -1;
        }
        dry_delay[slot] = dry;
        wet_delay[slot] = output;
        clip->samples[(size_t)frame] = output;
    }
    free(dry_delay);
    free(wet_delay);
    return 0;
}

static void hwa_grc_put16(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xffU);
}

static void hwa_grc_put32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xffU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xffU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xffU);
}

static int hwa_grc_write_all(int descriptor,
                             const unsigned char *data,
                             size_t size)
{
    while (size != 0U) {
#if defined(_WIN32)
        unsigned int request = size > (size_t)UINT_MAX ? UINT_MAX : (unsigned int)size;
        int count = _write(descriptor, data, request);
#else
        ssize_t count = write(descriptor, data, size);
#endif
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (count == 0) return -1;
        data += (size_t)count;
        size -= (size_t)count;
    }
    return 0;
}

static int hwa_grc_open_read_no_follow(const char *path)
{
#if defined(_WIN32)
    HANDLE handle = CreateFileA(
        path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BY_HANDLE_FILE_INFORMATION facts;
    int descriptor;
    if (handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(handle, &facts) ||
        (facts.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        if (handle != INVALID_HANDLE_VALUE) (void)CloseHandle(handle);
        return -1;
    }
    descriptor = _open_osfhandle(
        (intptr_t)handle, _O_RDONLY | _O_BINARY | _O_NOINHERIT);
    if (descriptor < 0) (void)CloseHandle(handle);
    return descriptor;
#else
    return open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
#endif
}

static int hwa_grc_pcm16(double sample)
{
    if (sample >= 1.0) return 32767;
    if (sample <= -1.0) return -32768;
    if (sample >= 0.0) return (int)floor(sample * 32767.0 + 0.5);
    return (int)ceil(sample * 32768.0 - 0.5);
}

static int hwa_grc_write_wave(const char *path,
                              const HWAGRCClip *clip,
                              uint64_t maximum,
                              uint64_t *file_bytes,
                              char hash[HWA_SHA256_HEX_SIZE],
                              HWAGRCFileOwnership *owned,
                              char *error,
                              size_t error_size)
{
    unsigned char header[44];
    unsigned char block[8192];
    HWASha256 sha;
    unsigned char digest[32];
    uint64_t data_bytes;
    uint64_t total;
    uint64_t frame = 0U;
    HWAGRCIdentity created;
    int created_valid = 0;
    int descriptor = -1;
    int status = -1;
    if (hwa_grc_u64_mul(clip->frame_count, UINT64_C(2), &data_bytes) != 0 ||
        data_bytes > UINT32_MAX - UINT32_C(36) ||
        data_bytes > UINT64_MAX - UINT64_C(44)) {
        hwa_set_error(error, error_size, "Stage 9 excerpt is too large for RIFF");
        return -1;
    }
    total = data_bytes + UINT64_C(44);
    if (total > maximum) {
        hwa_set_error(error, error_size, "Stage 9 output file cap exceeded");
        return -1;
    }
    memset(header, 0, sizeof(header));
    memcpy(header, "RIFF", 4U);
    hwa_grc_put32(header + 4U, (uint32_t)data_bytes + UINT32_C(36));
    memcpy(header + 8U, "WAVEfmt ", 8U);
    hwa_grc_put32(header + 16U, UINT32_C(16));
    hwa_grc_put16(header + 20U, UINT16_C(1));
    hwa_grc_put16(header + 22U, UINT16_C(1));
    hwa_grc_put32(header + 24U, clip->sample_rate_hz);
    if (clip->sample_rate_hz > UINT32_MAX / UINT32_C(2)) return -1;
    hwa_grc_put32(header + 28U, clip->sample_rate_hz * UINT32_C(2));
    hwa_grc_put16(header + 32U, UINT16_C(2));
    hwa_grc_put16(header + 34U, UINT16_C(16));
    memcpy(header + 36U, "data", 4U);
    hwa_grc_put32(header + 40U, (uint32_t)data_bytes);
#if defined(_WIN32)
    descriptor = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY |
                       _O_NOINHERIT, _S_IREAD | _S_IWRITE);
#else
    descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                      O_NOFOLLOW, 0600);
#endif
    if (descriptor < 0) {
        hwa_set_error(error, error_size, "cannot create Stage 9 excerpt file");
        return -1;
    }
    if (hwa_grc_identity_fd(descriptor, &created) != 0) goto cleanup;
    created_valid = 1;
    hwa_sha256_init(&sha);
    if (hwa_grc_write_all(descriptor, header, sizeof(header)) != 0) goto cleanup;
    hwa_sha256_update(&sha, header, sizeof(header));
    while (frame < clip->frame_count) {
        size_t count = (clip->frame_count - frame) < UINT64_C(4096) ?
            (size_t)(clip->frame_count - frame) : 4096U;
        size_t index;
        for (index = 0U; index < count; ++index) {
            int value = hwa_grc_pcm16(clip->samples[(size_t)frame + index]);
            hwa_grc_put16(block + index * 2U, (uint16_t)(int16_t)value);
        }
        if (hwa_grc_write_all(descriptor, block, count * 2U) != 0) goto cleanup;
        hwa_sha256_update(&sha, block, count * 2U);
        frame += (uint64_t)count;
    }
#if defined(_WIN32)
    {
        int sync_failed = _commit(descriptor) != 0;
        int identity_failed = hwa_grc_identity_fd(descriptor, &created) != 0;
        int close_failed = _close(descriptor) != 0;
        descriptor = -1;
        if (sync_failed || identity_failed || close_failed) goto cleanup;
    }
#else
    {
        int sync_failed = fsync(descriptor) != 0;
        int identity_failed = hwa_grc_identity_fd(descriptor, &created) != 0;
        int close_failed = close(descriptor) != 0;
        descriptor = -1;
        if (sync_failed || identity_failed || close_failed) goto cleanup;
    }
#endif
    descriptor = -1;
    if (hwa_grc_ownership_after_close(path, &created, owned) != 0)
        goto cleanup;
    hwa_sha256_final(&sha, digest);
    hwa_sha256_hex(digest, hash);
    *file_bytes = total;
    status = 0;
cleanup:
    if (descriptor >= 0) {
        if (hwa_grc_identity_fd(descriptor, &created) == 0)
            created_valid = 1;
#if defined(_WIN32)
        (void)_close(descriptor);
#else
        (void)close(descriptor);
#endif
    }
    if (status != 0) {
        if (created_valid && !owned->valid)
            (void)hwa_grc_ownership_after_close(path, &created, owned);
        (void)hwa_grc_remove_owned_regular(path, owned);
        hwa_set_error(error, error_size, "cannot write Stage 9 excerpt file");
    }
    return status;
}

static int hwa_grc_copy_file(const char *source,
                             const HWAGRCFileOwnership *source_owned,
                             const char *target,
                             uint64_t expected_bytes,
                             const char expected_hash[HWA_SHA256_HEX_SIZE],
                             uint64_t maximum,
                             uint64_t *file_bytes,
                             char hash[HWA_SHA256_HEX_SIZE],
                             HWAGRCFileOwnership *owned,
                             char *error,
                             size_t error_size)
{
    unsigned char buffer[65536];
    HWASha256 sha;
    unsigned char digest[32];
    int input = -1;
    int output = -1;
    HWAGRCIdentity source_opened;
    HWAGRCIdentity source_after;
    HWAGRCIdentity created;
    int created_valid = 0;
    uint64_t total = 0U;
    int status = -1;
    if (expected_bytes > maximum) goto cleanup;
#if defined(_WIN32)
    input = hwa_grc_open_read_no_follow(source);
    output = _open(target, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY |
                   _O_NOINHERIT, _S_IREAD | _S_IWRITE);
#else
    input = hwa_grc_open_read_no_follow(source);
    output = open(target, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                  O_NOFOLLOW, 0600);
#endif
    if (input < 0 || output < 0 || source_owned == NULL ||
        !source_owned->valid ||
        hwa_grc_identity_fd(input, &source_opened) != 0 ||
        !hwa_grc_identity_equal(&source_owned->identity, &source_opened) ||
        hwa_grc_identity_fd(output, &created) != 0) goto cleanup;
    created_valid = 1;
    hwa_sha256_init(&sha);
    for (;;) {
        int count;
#if defined(_WIN32)
        count = _read(input, buffer, (unsigned int)sizeof(buffer));
#else
        ssize_t read_count = read(input, buffer, sizeof(buffer));
        if (read_count > INT_MAX) goto cleanup;
        count = (int)read_count;
#endif
        if (count < 0) {
            if (errno == EINTR) continue;
            goto cleanup;
        }
        if (count == 0) break;
        if ((uint64_t)count > expected_bytes - total ||
            hwa_grc_write_all(output, buffer, (size_t)count) != 0)
            goto cleanup;
        hwa_sha256_update(&sha, buffer, (size_t)count);
        total += (uint64_t)count;
    }
    if (hwa_grc_identity_fd(input, &source_after) != 0 ||
        !hwa_grc_identity_equal(&source_opened, &source_after)) goto cleanup;
#if defined(_WIN32)
    if (_close(input) != 0) { input = -1; goto cleanup; }
#else
    if (close(input) != 0) { input = -1; goto cleanup; }
#endif
    input = -1;
#if defined(_WIN32)
    {
        int sync_failed = _commit(output) != 0;
        int identity_failed = hwa_grc_identity_fd(output, &created) != 0;
        int close_failed = _close(output) != 0;
        output = -1;
        if (sync_failed || identity_failed || close_failed) goto cleanup;
    }
#else
    {
        int sync_failed = fsync(output) != 0;
        int identity_failed = hwa_grc_identity_fd(output, &created) != 0;
        int close_failed = close(output) != 0;
        output = -1;
        if (sync_failed || identity_failed || close_failed) goto cleanup;
    }
#endif
    output = -1;
    if (hwa_grc_ownership_after_close(target, &created, owned) != 0)
        goto cleanup;
    hwa_sha256_final(&sha, digest);
    hwa_sha256_hex(digest, hash);
    if (total != expected_bytes || strcmp(hash, expected_hash) != 0)
        goto cleanup;
    *file_bytes = total;
    status = 0;
cleanup:
    if (input >= 0) {
#if defined(_WIN32)
        (void)_close(input);
#else
        (void)close(input);
#endif
    }
    if (output >= 0) {
        if (hwa_grc_identity_fd(output, &created) == 0)
            created_valid = 1;
#if defined(_WIN32)
        (void)_close(output);
#else
        (void)close(output);
#endif
    }
    if (status != 0) {
        if (created_valid && !owned->valid)
            (void)hwa_grc_ownership_after_close(target, &created, owned);
        (void)hwa_grc_remove_owned_regular(target, owned);
        hwa_set_error(error, error_size, "cannot copy Stage 9 X excerpt");
    }
    return status;
}

static int hwa_grc_mkdir_new(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0700);
#endif
}

static int hwa_grc_path_absent(const char *path)
{
#if defined(_WIN32)
    DWORD attributes = GetFileAttributesA(path);
    return attributes == INVALID_FILE_ATTRIBUTES &&
        GetLastError() == ERROR_FILE_NOT_FOUND;
#else
    struct stat facts;
    return lstat(path, &facts) != 0 && errno == ENOENT;
#endif
}

static int hwa_grc_temp_root(const char *output,
                             char **temporary,
                             char *error,
                             size_t error_size)
{
    size_t size = strlen(output);
    unsigned attempt;
    if (size > SIZE_MAX - 40U) return -1;
    for (attempt = 0U; attempt < 256U; ++attempt) {
        char *path = (char *)malloc(size + 40U);
        int length;
        if (path == NULL) return -1;
#if defined(_WIN32)
        length = snprintf(path, size + 40U, "%s.hwa-private-%lu-%u",
                          output, (unsigned long)GetCurrentProcessId(), attempt);
#else
        length = snprintf(path, size + 40U, "%s.hwa-private-%ld-%u",
                          output, (long)getpid(), attempt);
#endif
        if (length > 0 && (size_t)length < size + 40U &&
            hwa_grc_mkdir_new(path) == 0) {
            *temporary = path;
            return 0;
        }
        free(path);
        if (errno != EEXIST) break;
    }
    hwa_set_error(error, error_size,
                  "cannot create private Stage 9 output directory");
    return -1;
}

static int hwa_grc_remove_private(const char *root,
                                  const HWAGapReportResult *result,
                                  HWAGRCFileOwnership *owned,
                                  size_t owned_count,
                                  const HWAGRCIdentity *audio_owned,
                                  const HWAGRCIdentity *root_owned)
{
    static const char *reports[] = {
        "result.hwa-report", "report.csv", "report.json", "report.html"
    };
    char *path = NULL;
    char *audio = NULL;
    size_t index;
    int status = 0;
    size_t owned_index = 0U;
    for (index = 0U; index < result->excerpt_count; ++index) {
        const HWAGapReportExcerpt *excerpt = &result->excerpts[index];
        const char *relative[3] = {
            excerpt->reference_path, excerpt->model_path, excerpt->x_path
        };
        size_t clip;
        for (clip = 0U; clip < 3U; ++clip) {
            if (relative[clip] == NULL || relative[clip][0] == '\0') continue;
            if (owned_index >= owned_count) continue;
            if (hwa_grc_join(&path, root, relative[clip]) != 0) status = -1;
            else if (hwa_grc_remove_owned_regular(
                         path, &owned[owned_index++]) != 0) status = -1;
            free(path);
            path = NULL;
        }
    }
    for (index = 0U; index < sizeof(reports) / sizeof(reports[0]); ++index) {
        if (owned_index >= owned_count) break;
        if (hwa_grc_join(&path, root, reports[index]) != 0) status = -1;
        else if (hwa_grc_remove_owned_regular(
                     path, &owned[owned_index++]) != 0) status = -1;
        free(path);
        path = NULL;
    }
    if (hwa_grc_join(&audio, root, "audio") != 0) status = -1;
    if (audio != NULL) {
        HWAGRCIdentity current;
        if (hwa_grc_identity_path(audio, 1, &current) != 0 ||
            !hwa_grc_identity_same_node(audio_owned, &current)) status = -1;
#if defined(_WIN32)
        else if (!RemoveDirectoryA(audio)) status = -1;
#else
        else if (rmdir(audio) != 0) status = -1;
#endif
    }
    {
        HWAGRCIdentity current;
        if (hwa_grc_identity_path(root, 1, &current) != 0 ||
            !hwa_grc_identity_same_node(root_owned, &current)) status = -1;
#if defined(_WIN32)
        else if (!RemoveDirectoryA(root)) status = -1;
#else
        else if (rmdir(root) != 0) status = -1;
#endif
    }
    free(audio);
    return status;
}

static int hwa_grc_capture_reports(const char *root,
                                   HWAGapReportMode mode,
                                   HWAGRCFileOwnership *owned,
                                   size_t capacity,
                                   size_t *count)
{
    static const char *reports[] = {
        "result.hwa-report", "report.csv", "report.json", "report.html"
    };
    size_t report_count = mode == HWA_GAP_REPORT_FULL ? 4U : 1U;
    size_t index;
    char *path = NULL;
    if (*count > capacity || report_count > capacity - *count) return -1;
    for (index = 0U; index < report_count; ++index) {
        if (hwa_grc_join(&path, root, reports[index]) != 0 ||
            hwa_grc_ownership_capture(path, &owned[*count]) != 0) {
            free(path);
            return -1;
        }
        free(path);
        path = NULL;
        (*count)++;
    }
    return 0;
}

static int hwa_grc_rename_no_replace(const char *from, const char *to)
{
#if defined(_WIN32)
    return MoveFileA(from, to) ? 0 : -1;
#elif defined(__APPLE__)
    return renameatx_np(AT_FDCWD, from, AT_FDCWD, to, RENAME_EXCL);
#elif defined(__linux__) && defined(SYS_renameat2)
    return (int)syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to,
                        RENAME_NOREPLACE);
#else
    (void)from;
    (void)to;
    errno = ENOTSUP;
    return -1;
#endif
}

static int hwa_grc_relative_path(char **path,
                                 const char *name,
                                 const char *suffix)
{
    int length;
    char *text;
    if (!hwa_grc_component(name)) return -1;
    length = snprintf(NULL, 0U, "audio/%s-%s.wav", name, suffix);
    if (length < 0) return -1;
    text = (char *)malloc((size_t)length + 1U);
    if (text == NULL) return -1;
    (void)snprintf(text, (size_t)length + 1U,
                   "audio/%s-%s.wav", name, suffix);
    *path = text;
    return 0;
}

static void hwa_grc_excerpt_outputs_free(HWAGapReportExcerpt *excerpt)
{
    free(excerpt->reference_path);
    free(excerpt->model_path);
    free(excerpt->x_path);
    excerpt->reference_path = NULL;
    excerpt->model_path = NULL;
    excerpt->x_path = NULL;
    memset(excerpt->reference_sha256, 0, sizeof(excerpt->reference_sha256));
    memset(excerpt->model_sha256, 0, sizeof(excerpt->model_sha256));
    memset(excerpt->x_sha256, 0, sizeof(excerpt->x_sha256));
    excerpt->reference_file_bytes = 0U;
    excerpt->model_file_bytes = 0U;
    excerpt->x_file_bytes = 0U;
    excerpt->reference_gain_db = 0.0;
    excerpt->model_gain_db = 0.0;
    excerpt->x_is_reference = 0;
}

static int hwa_grc_excerpt_empty_paths(HWAGapReportExcerpt *excerpt)
{
    excerpt->reference_path = hwa_grc_copy("");
    excerpt->model_path = hwa_grc_copy("");
    excerpt->x_path = hwa_grc_copy("");
    return excerpt->reference_path != NULL && excerpt->model_path != NULL &&
        excerpt->x_path != NULL ? 0 : -1;
}

static int hwa_grc_render_excerpt(const char *root,
                                  HWAGapReportResult *result,
                                  HWAGapReportExcerpt *excerpt,
                                  uint64_t outer_path_work,
                                  HWAGRCFileOwnership *ownership,
                                  size_t ownership_capacity,
                                  size_t *ownership_count,
                                  uint64_t *total_frames,
                                  uint64_t *total_output,
                                  uint64_t *clip_evaluations,
                                  char *error,
                                  size_t error_size)
{
    const HWAGapReportCandidate *candidate;
    const HWAGapReportSource *reference_source;
    const HWAGapReportSource *model_source;
    HWAGRCViewAuthority authority;
    HWAGRCInput reference_input;
    HWAGRCInput model_input;
    HWAGRCClip reference;
    HWAGRCClip model;
    char *reference_path = NULL;
    char *model_path = NULL;
    char *x_path = NULL;
    double reference_lufs;
    double model_lufs;
    uint64_t projected;
    uint64_t work;
    uint64_t part;
    uint64_t feature_budget;
    uint64_t visits;
    uint64_t reference_visits;
    uint64_t model_visits;
    uint64_t room_delay;
    int view_authorized = 1;
    int loudness;
    int status = -1;
    size_t ownership_base = *ownership_count;
    size_t ownership_needed = excerpt->make_x ? 3U : 2U;
    HWAGRCFileOwnership *reference_owned;
    HWAGRCFileOwnership *model_owned;
    HWAGRCFileOwnership *x_owned;
    if (ownership_base > ownership_capacity ||
        ownership_needed > ownership_capacity - ownership_base) {
        hwa_set_error(error, error_size,
                      "Stage 9 clip ownership cap exceeded");
        return -1;
    }
    reference_owned = &ownership[ownership_base];
    model_owned = &ownership[ownership_base + 1U];
    x_owned = excerpt->make_x ? &ownership[ownership_base + 2U] : NULL;
    memset(&reference_input, 0, sizeof(reference_input));
    memset(&model_input, 0, sizeof(model_input));
    memset(&reference, 0, sizeof(reference));
    memset(&model, 0, sizeof(model));
    memset(&authority, 0, sizeof(authority));
    reference_input.descriptor = -1;
    model_input.descriptor = -1;
    hwa_grc_excerpt_outputs_free(excerpt);
    visits = (uint64_t)result->candidate_count;
    if (hwa_grc_u64_mul((uint64_t)result->source_count,
                        UINT64_C(3), &part) != 0 ||
        hwa_grc_u64_add(&visits, part) != 0 ||
        hwa_grc_evaluations_add(clip_evaluations,
            result->options.max_evaluations, visits,
            error, error_size) != 0)
        goto cleanup;
    candidate = hwa_grc_candidate(result, excerpt->candidate_source_id,
                                  excerpt->candidate_row);
    if (candidate == NULL) {
        hwa_set_error(error, error_size,
                      "Stage 9 excerpt names no unique candidate row");
        goto cleanup;
    }
    excerpt->availability = HWA_GAP_REPORT_AVAILABLE;
    free(excerpt->reason);
    excerpt->reason = hwa_grc_copy("");
    if (excerpt->reason == NULL) goto cleanup;
    if (hwa_gap_report_result_retained_bytes(
            result, &result->retained_work_bytes) != 0 ||
        result->retained_work_bytes > result->options.max_work_bytes)
        goto work_failed;
    reference_source = hwa_grc_source(result, excerpt->reference_source_id);
    model_source = hwa_grc_source(result, excerpt->model_source_id);
    if (excerpt->view == HWA_GAP_REPORT_VIEW_BROAD_EQ_MATCHED ||
        excerpt->view == HWA_GAP_REPORT_VIEW_ROOM_MATCHED)
        view_authorized = hwa_grc_view_production_authority(
            result, candidate, reference_source, model_source,
            excerpt->view, &authority, outer_path_work,
            clip_evaluations, error, error_size);
    else if (excerpt->view == HWA_GAP_REPORT_VIEW_STEM ||
             excerpt->view == HWA_GAP_REPORT_VIEW_PROBE_LINKED)
        view_authorized = hwa_grc_view_run_authority(
            result, candidate, reference_source, model_source,
            excerpt->view, outer_path_work,
            clip_evaluations, error, error_size);
    if (view_authorized < 0) goto cleanup;
    if (!view_authorized) {
        excerpt->availability = HWA_GAP_REPORT_UNAVAILABLE;
        free(excerpt->reason);
        excerpt->reason = hwa_grc_copy("view-authority-unavailable");
        if (excerpt->reason == NULL || hwa_grc_excerpt_empty_paths(excerpt) != 0)
            goto cleanup;
        status = 0;
        goto cleanup;
    }
    if (reference_source == NULL || model_source == NULL ||
        excerpt->frame_count > result->options.max_excerpt_frames ||
        excerpt->frame_count > result->options.max_total_excerpt_frames -
            *total_frames) {
        hwa_set_error(error, error_size, "Stage 9 excerpt cap exceeded");
        goto cleanup;
    }
    if (hwa_grc_u64_mul(excerpt->frame_count, UINT64_C(2), &projected) != 0 ||
        hwa_grc_u64_add(&projected, UINT64_C(44)) != 0 ||
        projected > result->options.max_output_file_bytes ||
        hwa_grc_u64_mul(projected, excerpt->make_x ? UINT64_C(3) : UINT64_C(2),
                        &projected) != 0 ||
        *total_output > result->options.max_bundle_bytes ||
        projected > result->options.max_bundle_bytes - *total_output) {
        hwa_set_error(error, error_size, "Stage 9 bundle cap exceeded");
        goto cleanup;
    }
    if (hwa_grc_input_open(reference_source, &result->options,
                           &reference_input, error, error_size) != 0 ||
        hwa_grc_input_open(model_source, &result->options,
                           &model_input, error, error_size) != 0 ||
        reference_input.reader.format.sample_rate_hz !=
            model_input.reader.format.sample_rate_hz ||
        reference_input.reader.format.frames >
            result->options.max_input_frames ||
        model_input.reader.format.frames > result->options.max_input_frames) {
        if (error != NULL && error_size != 0U && error[0] == '\0')
            hwa_set_error(error, error_size,
                          "Stage 9 excerpt inputs must have equal rates");
        goto cleanup;
    }
    work = result->retained_work_bytes;
    if (hwa_grc_u64_add(&work, outer_path_work) != 0) goto work_failed;
    if (hwa_grc_u64_mul((uint64_t)strlen(excerpt->name) + UINT64_C(16),
            excerpt->make_x ? UINT64_C(3) : UINT64_C(2), &part) != 0 ||
        hwa_grc_u64_add(&work, part) != 0 ||
        hwa_grc_u64_mul((uint64_t)strlen(root) +
                (uint64_t)strlen(excerpt->name) + UINT64_C(18),
            excerpt->make_x ? UINT64_C(3) : UINT64_C(2), &part) != 0 ||
        hwa_grc_u64_add(&work, part) != 0) goto work_failed;
    if (hwa_grc_u64_mul(excerpt->frame_count, UINT64_C(2), &part) != 0 ||
        hwa_grc_u64_mul(part, (uint64_t)sizeof(double), &part) != 0 ||
        hwa_grc_u64_add(&work, part) != 0 ||
        hwa_grc_u64_mul((uint64_t)result->options.decode_block_frames,
            (uint64_t)reference_input.reader.format.block_align +
                (uint64_t)model_input.reader.format.block_align, &part) != 0 ||
        hwa_grc_u64_add(&work, part) != 0) goto work_failed;
    room_delay = ((uint64_t)reference_input.reader.format.sample_rate_hz *
                  UINT64_C(20) + UINT64_C(500)) / UINT64_C(1000);
    if (room_delay == 0U) room_delay = 1U;
    if (excerpt->view == HWA_GAP_REPORT_VIEW_ROOM_MATCHED &&
        (hwa_grc_u64_mul(room_delay, UINT64_C(2) *
             (uint64_t)sizeof(double), &part) != 0 ||
         hwa_grc_u64_add(&work, part) != 0)) goto work_failed;
    if (work >= result->options.max_work_bytes) goto work_failed;
    feature_budget = result->options.max_work_bytes - work;
    if (hwa_grc_u64_mul(excerpt->reference_start_sample,
            (uint64_t)reference_input.reader.format.channels,
            &reference_visits) != 0 ||
        hwa_grc_u64_mul(excerpt->model_start_sample,
            (uint64_t)model_input.reader.format.channels,
            &model_visits) != 0 ||
        hwa_grc_u64_add(&reference_visits, model_visits) != 0)
        goto work_failed;
    visits = (uint64_t)reference_input.reader.format.channels +
        (uint64_t)model_input.reader.format.channels + UINT64_C(8) +
        (excerpt->make_x ? UINT64_C(1) : UINT64_C(0));
    if (excerpt->view == HWA_GAP_REPORT_VIEW_BROAD_EQ_MATCHED)
        visits += HWA_PRODUCTION_EQ_NODE_COUNT;
    else if (excerpt->view == HWA_GAP_REPORT_VIEW_ROOM_MATCHED)
        visits++;
    if (hwa_grc_u64_mul(excerpt->frame_count, visits, &visits) != 0 ||
        hwa_grc_u64_add(&visits, reference_visits) != 0 ||
        hwa_grc_evaluations_add(clip_evaluations,
            result->options.max_evaluations, visits,
            error, error_size) != 0)
        goto cleanup;
    if (hwa_grc_decode_mono(&reference_input, excerpt->reference_start_sample,
                            excerpt->frame_count,
                            result->options.decode_block_frames,
                            &reference, error, error_size) != 0 ||
        hwa_grc_decode_mono(&model_input, excerpt->model_start_sample,
                            excerpt->frame_count,
                            result->options.decode_block_frames,
                            &model, error, error_size) != 0 ||
        hwa_grc_input_verify(&reference_input, reference_source,
                             error, error_size) != 0 ||
        hwa_grc_input_verify(&model_input, model_source,
                             error, error_size) != 0) {
        if (error != NULL && error_size != 0U && error[0] == '\0')
            hwa_set_error(error, error_size,
                          "Stage 9 excerpt inputs must have equal rates and spans");
        goto cleanup;
    }
    if ((excerpt->view == HWA_GAP_REPORT_VIEW_BROAD_EQ_MATCHED &&
         hwa_grc_apply_eq(&model, &authority) != 0) ||
        (excerpt->view == HWA_GAP_REPORT_VIEW_ROOM_MATCHED &&
         hwa_grc_apply_room(&model, &authority) != 0)) {
        hwa_set_error(error, error_size, "Stage 9 view DSP failed");
        goto cleanup;
    }
    if (hwa_grc_loudness_evaluation_bound(
            reference.frame_count, &visits) != 0 ||
        hwa_grc_loudness_evaluation_bound(model.frame_count, &part) != 0 ||
        hwa_grc_u64_add(&visits, part) != 0) {
        hwa_set_error(error, error_size, "Stage 9 evaluation cap exceeded");
        goto cleanup;
    }
    if (hwa_grc_evaluations_add(
            clip_evaluations, result->options.max_evaluations, visits,
            error, error_size) != 0)
        goto cleanup;
    loudness = hwa_grc_loudness(&reference, feature_budget,
                                &reference_lufs, error, error_size);
    if (loudness != 0) {
        if (loudness > 0) {
            excerpt->availability = HWA_GAP_REPORT_INSUFFICIENT;
            free(excerpt->reason);
            excerpt->reason = hwa_grc_copy("integrated-loudness-unavailable");
            status = excerpt->reason == NULL ||
                hwa_grc_excerpt_empty_paths(excerpt) != 0 ? -1 : 0;
        }
        goto cleanup;
    }
    loudness = hwa_grc_loudness(&model, feature_budget,
                                &model_lufs, error, error_size);
    if (loudness != 0) {
        if (loudness > 0) {
            excerpt->availability = HWA_GAP_REPORT_INSUFFICIENT;
            free(excerpt->reason);
            excerpt->reason = hwa_grc_copy("integrated-loudness-unavailable");
            status = excerpt->reason == NULL ||
                hwa_grc_excerpt_empty_paths(excerpt) != 0 ? -1 : 0;
        }
        goto cleanup;
    }
    hwa_grc_prepare_pair(&reference, &model, reference_lufs, model_lufs,
                         &excerpt->reference_gain_db,
                         &excerpt->model_gain_db);
    if (hwa_grc_relative_path(&excerpt->reference_path, excerpt->name, "a") != 0 ||
        hwa_grc_relative_path(&excerpt->model_path, excerpt->name, "b") != 0 ||
        (excerpt->make_x &&
         hwa_grc_relative_path(&excerpt->x_path, excerpt->name, "x") != 0) ||
        hwa_grc_join(&reference_path, root, excerpt->reference_path) != 0 ||
        hwa_grc_join(&model_path, root, excerpt->model_path) != 0 ||
        (excerpt->make_x &&
         hwa_grc_join(&x_path, root, excerpt->x_path) != 0) ||
        hwa_grc_write_wave(reference_path, &reference,
                           result->options.max_output_file_bytes,
                           &excerpt->reference_file_bytes,
                           excerpt->reference_sha256, reference_owned,
                           error, error_size) != 0 ||
        hwa_grc_write_wave(model_path, &model,
                           result->options.max_output_file_bytes,
                           &excerpt->model_file_bytes,
                           excerpt->model_sha256, model_owned,
                           error, error_size) != 0)
        goto cleanup;
    if (excerpt->make_x) {
        const char *chosen_path;
        uint64_t chosen_bytes;
        const char *chosen_hash;
        excerpt->x_is_reference =
            hwa_gap_report_excerpt_x_is_reference(result, excerpt);
        chosen_path = excerpt->x_is_reference ? reference_path : model_path;
        chosen_bytes = excerpt->x_is_reference ?
            excerpt->reference_file_bytes : excerpt->model_file_bytes;
        chosen_hash = excerpt->x_is_reference ?
            excerpt->reference_sha256 : excerpt->model_sha256;
        if (hwa_grc_copy_file(chosen_path,
                              excerpt->x_is_reference
                                  ? reference_owned : model_owned,
                              x_path, chosen_bytes, chosen_hash,
                              result->options.max_output_file_bytes,
                              &excerpt->x_file_bytes, excerpt->x_sha256,
                              x_owned, error, error_size) != 0)
            goto cleanup;
    }
    if (hwa_grc_u64_add(total_output, excerpt->reference_file_bytes) != 0 ||
        hwa_grc_u64_add(total_output, excerpt->model_file_bytes) != 0 ||
        hwa_grc_u64_add(total_output, excerpt->x_file_bytes) != 0) goto cleanup;
    *total_frames += excerpt->frame_count;
    *ownership_count = ownership_base + ownership_needed;
    status = 0;
cleanup:
    hwa_grc_input_close(&reference_input);
    hwa_grc_input_close(&model_input);
    hwa_grc_clip_free(&reference);
    hwa_grc_clip_free(&model);
    if (status != 0) {
        (void)hwa_grc_remove_owned_regular(reference_path, reference_owned);
        (void)hwa_grc_remove_owned_regular(model_path, model_owned);
        (void)hwa_grc_remove_owned_regular(x_path, x_owned);
        hwa_grc_excerpt_outputs_free(excerpt);
    }
    free(reference_path);
    free(model_path);
    free(x_path);
    return status;
work_failed:
    hwa_set_error(error, error_size, "Stage 9 excerpt work cap exceeded");
    goto cleanup;
}

int hwa_gap_report_build_clip_bundle(HWAGapReportResult *result,
                                     uint64_t starting_evaluations,
                                     char *error,
                                     size_t error_size)
{
    char *temporary = NULL;
    char *audio = NULL;
    uint64_t total_frames = 0U;
    uint64_t retained = 0U;
    uint64_t clip_evaluations = starting_evaluations;
    uint64_t projected_audio = 0U;
    uint64_t projected_tree = 0U;
    uint64_t path_work = 0U;
    uint64_t ownership_bytes = 0U;
    HWAGRCFileOwnership *ownership = NULL;
    size_t ownership_capacity = 0U;
    size_t ownership_count = 0U;
    HWAGRCIdentity root_owned;
    HWAGRCIdentity audio_owned;
    int root_owned_valid = 0;
    int audio_owned_valid = 0;
    size_t index;
    int status = -1;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (result == NULL || result->mode == HWA_GAP_REPORT_RANK ||
        starting_evaluations > result->options.max_evaluations ||
        !hwa_gap_report_clip_path_absolute(result->output_directory) ||
        !hwa_grc_path_absent(result->output_directory) ||
        result->excerpt_count > result->options.max_excerpts) {
        if (error != NULL && error_size != 0U && error[0] == '\0')
            hwa_set_error(error, error_size,
                          "invalid Stage 9 clip bundle arguments");
        goto cleanup;
    }
    for (index = 0U; index < result->excerpt_count; ++index) {
        const HWAGapReportExcerpt *excerpt = &result->excerpts[index];
        uint64_t file_bytes;
        uint64_t files = excerpt->make_x ? UINT64_C(3) : UINT64_C(2);
        if (excerpt->frame_count > result->options.max_excerpt_frames ||
            hwa_grc_u64_add(&total_frames, excerpt->frame_count) != 0 ||
            total_frames > result->options.max_total_excerpt_frames ||
            hwa_grc_u64_mul(excerpt->frame_count, UINT64_C(2),
                            &file_bytes) != 0 ||
            hwa_grc_u64_add(&file_bytes, UINT64_C(44)) != 0 ||
            file_bytes > result->options.max_output_file_bytes ||
            hwa_grc_u64_mul(file_bytes, files, &file_bytes) != 0 ||
            hwa_grc_u64_add(&projected_audio, file_bytes) != 0) {
            hwa_set_error(error, error_size,
                          "Stage 9 projected clip size exceeds its cap");
            goto cleanup;
        }
        if (files > (uint64_t)(SIZE_MAX - ownership_capacity)) {
            hwa_set_error(error, error_size,
                          "Stage 9 clip ownership count overflow");
            goto cleanup;
        }
        ownership_capacity += (size_t)files;
    }
    if (ownership_capacity > SIZE_MAX -
            (result->mode == HWA_GAP_REPORT_FULL ? 4U : 1U))
        goto cleanup;
    ownership_capacity += result->mode == HWA_GAP_REPORT_FULL ? 4U : 1U;
    if (ownership_capacity > SIZE_MAX / sizeof(*ownership)) goto cleanup;
    ownership_bytes = (uint64_t)(ownership_capacity * sizeof(*ownership));
    total_frames = 0U;
    if (hwa_gap_report_result_retained_bytes(result, &retained) != 0 ||
        hwa_grc_u64_add(&path_work,
            (uint64_t)strlen(result->output_directory) + UINT64_C(40)) != 0 ||
        hwa_grc_u64_add(&path_work,
            (uint64_t)strlen(result->output_directory) + UINT64_C(47)) != 0 ||
        hwa_grc_u64_add(&path_work,
            (uint64_t)strlen(result->output_directory) + UINT64_C(64)) != 0 ||
        hwa_grc_u64_add(&retained, path_work) != 0 ||
        hwa_grc_u64_add(&retained, ownership_bytes) != 0 ||
        retained > result->options.max_work_bytes ||
        hwa_gap_report_output_projected_upper(
            result, result->mode, projected_audio, &projected_tree) != 0 ||
        projected_tree > result->options.max_bundle_bytes ||
        (ownership = (HWAGRCFileOwnership *)calloc(
            ownership_capacity, sizeof(*ownership))) == NULL) {
        if (error != NULL && error_size != 0U && error[0] == '\0')
            hwa_set_error(error, error_size,
                          "invalid Stage 9 clip bundle arguments");
        goto cleanup;
    }
    if (hwa_grc_temp_root(result->output_directory, &temporary,
                          error, error_size) != 0 ||
        hwa_grc_identity_path(temporary, 1, &root_owned) != 0)
        goto cleanup;
    root_owned_valid = 1;
    if (hwa_grc_join(&audio, temporary, "audio") != 0 ||
        hwa_grc_mkdir_new(audio) != 0 ||
        hwa_grc_identity_path(audio, 1, &audio_owned) != 0)
        goto cleanup;
    audio_owned_valid = 1;
    result->total_output_bytes = 0U;
    for (index = 0U; index < result->excerpt_count; ++index) {
        if (hwa_grc_render_excerpt(
                temporary, result, &result->excerpts[index],
                path_work + ownership_bytes,
                ownership, ownership_capacity, &ownership_count,
                &total_frames, &result->total_output_bytes,
                &clip_evaluations,
                error, error_size) != 0)
            goto cleanup;
    }
    {
        uint64_t warning_visits;
        if (hwa_grc_u64_mul((uint64_t)result->candidate_count,
                            UINT64_C(3), &warning_visits) != 0 ||
            hwa_grc_u64_add(&warning_visits,
                            (uint64_t)result->source_count) != 0 ||
            hwa_grc_u64_add(&warning_visits,
                            (uint64_t)result->excerpt_count) != 0 ||
            hwa_grc_evaluations_add(&clip_evaluations,
                result->options.max_evaluations, warning_visits,
                error, error_size) != 0)
            goto cleanup;
    }
    if (hwa_gap_report_result_peak_work_bytes(
            result, path_work + ownership_bytes, &retained) != 0 ||
        retained > result->options.max_work_bytes ||
        hwa_gap_report_warnings_rebuild(result, error, error_size) != 0 ||
        hwa_gap_report_result_retained_bytes(result, &retained) != 0 ||
        retained > result->options.max_work_bytes) {
        hwa_set_error(error, error_size, "Stage 9 result exceeds its work cap");
        goto cleanup;
    }
    result->retained_work_bytes = retained;
    if (hwa_gap_report_result_validate(result, error, error_size) != 0 ||
        hwa_gap_report_output_write_tree(
            temporary, result->mode, result,
            path_work + ownership_bytes, error, error_size) != 0 ||
        hwa_grc_capture_reports(
            temporary, result->mode, ownership, ownership_capacity,
            &ownership_count) != 0 ||
        hwa_grc_rename_no_replace(temporary, result->output_directory) != 0) {
        if (error != NULL && error_size != 0U && error[0] == '\0')
            hwa_set_error(error, error_size,
                          "cannot commit Stage 9 output directory");
        goto cleanup;
    }
    free(temporary);
    temporary = NULL;
    status = 0;
cleanup:
    if (temporary != NULL && root_owned_valid && audio_owned_valid)
        (void)hwa_grc_remove_private(
            temporary, result, ownership, ownership_count,
            &audio_owned, &root_owned);
    else if (temporary != NULL && root_owned_valid) {
        HWAGRCIdentity current;
        if (hwa_grc_identity_path(temporary, 1, &current) == 0 &&
            hwa_grc_identity_same_node(&root_owned, &current)) {
#if defined(_WIN32)
            (void)RemoveDirectoryA(temporary);
#else
            (void)rmdir(temporary);
#endif
        }
    }
    free(temporary);
    free(audio);
    free(ownership);
    return status;
}

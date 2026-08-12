#ifndef HWA_ITEM_FILE_H
#define HWA_ITEM_FILE_H

#include "hlolli_wg_analyzer.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define HWA_ITEM_FILE_SCHEMA_VERSION 1U

typedef struct HWAItemFileLimits {
    uint64_t max_bytes;
    uint64_t max_work_bytes;
    size_t max_fields_per_row;
    size_t max_field_bytes;
    size_t max_events;
    size_t max_items;
    size_t max_manual_items;
    size_t max_members;
    size_t max_warnings;
} HWAItemFileLimits;

typedef struct HWAItemEditSet {
    char *path;
    char sha256[HWA_SHA256_HEX_SIZE];
    char alignment_sha256[HWA_SHA256_HEX_SIZE];
    char audio_sha256[HWA_SHA256_HEX_SIZE];
    char labels_sha256[HWA_SHA256_HEX_SIZE];
    HWAItemEdit *edits;
    size_t edit_count;
    uint64_t retained_work_bytes;
} HWAItemEditSet;

/*
 * A full item-file load owns both the decoded Stage 3 result and the explicit
 * item-file path. `sha256` names the exact item-file bytes. The nested
 * HWAItemSet keeps the work count saved by Stage 3; `retained_work_bytes`
 * counts storage retained by this reader.
 */
typedef struct HWAItemFileData {
    char *path;
    char sha256[HWA_SHA256_HEX_SIZE];
    HWAItemSet items;
    uint64_t retained_work_bytes;
} HWAItemFileData;

void hwa_item_file_limits_default(HWAItemFileLimits *limits);

int hwa_item_file_write(FILE *stream,
                        const HWAItemSet *items,
                        char *error,
                        size_t error_size);

int hwa_item_file_read_edits(const char *path,
                             const HWAItemFileLimits *limits,
                             HWAItemEditSet *edits,
                             char *error,
                             size_t error_size);

int hwa_item_file_read_full(const char *path,
                            const HWAItemFileLimits *limits,
                            HWAItemFileData *result,
                            char *error,
                            size_t error_size);

int hwa_item_edit_set_matches(
    const HWAItemEditSet *edits,
    const char alignment_sha256[HWA_SHA256_HEX_SIZE],
    const char audio_sha256[HWA_SHA256_HEX_SIZE],
    const char labels_sha256[HWA_SHA256_HEX_SIZE],
    char *error,
    size_t error_size);

void hwa_item_edit_set_free(HWAItemEditSet *edits);
void hwa_item_file_data_free(HWAItemFileData *result);

#endif

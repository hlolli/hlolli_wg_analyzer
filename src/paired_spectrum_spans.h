#ifndef HWA_PAIRED_SPECTRUM_SPANS_H
#define HWA_PAIRED_SPECTRUM_SPANS_H

#include "internal.h"

#define HWA_PAIRED_LABEL_MAX 24U

typedef struct HwaPairedSampleSpan {
    uint64_t start_sample;
    uint64_t end_sample;
    /* Supplied or mapped bounds remain present when they cannot be measured. */
    int present;
    int valid;
} HwaPairedSampleSpan;

typedef struct HwaPairedLabel {
    const char *name;
    const char *value;
} HwaPairedLabel;

typedef struct HwaPairedSpan {
    const char *id;
    HwaPairedSampleSpan reference;
    HwaPairedSampleSpan target;
    HwaPairedSampleSpan held_reference;
    HwaPairedSampleSpan held_target;
    double expected_hz;
    double confidence;
    int confidence_valid;
    const char *skip_reason;
    HwaPairedLabel labels[HWA_PAIRED_LABEL_MAX];
    size_t label_count;
} HwaPairedSpan;

struct PairedMap;

typedef struct HwaPairedSpans {
    HWAWavReader reference;
    HWAWavReader target;
    HwaPairedSpan *rows;
    size_t count;
    int notes_on_target;
    const char *label_names[HWA_PAIRED_LABEL_MAX];
    size_t label_count;
    uint64_t retained_work_bytes;
    char alignment_sha256[HWA_SHA256_HEX_SIZE];
    char notes_sha256[HWA_SHA256_HEX_SIZE];
    char reference_sha256[HWA_SHA256_HEX_SIZE];
    char target_sha256[HWA_SHA256_HEX_SIZE];
    char alignment_reference_sha256[HWA_SHA256_HEX_SIZE];
    char alignment_target_sha256[HWA_SHA256_HEX_SIZE];
    unsigned char *csv_data;
    HWAAlignment score_alignment;
    HWAAlignment audio_alignment;
    struct PairedMap *map;
} HwaPairedSpans;

/* Confidence combines the annotation and the lowest supporting anchor for the
 * held interval. A held interval overlapping a saved prefix, suffix, skip,
 * repeat, rest or cadenza gap on either recording yields unmatched-alignment. */
int hwa_paired_spans_open(const char *reference_path, const char *target_path,
                          const char *alignment_path,
                          const char *alignment_reference_path, const char *alignment_target_path,
                          const char *notes_path, const char *score_alignment_path,
                          int notes_on_target, int keep_map,
                          const HWAAnalysisOptions *limits, double held_fraction,
                          double min_confidence, HwaPairedSpans *result,
                          char *error, size_t error_size);
/* Map a fresh interval on the reference clock using a retained mapping index.
 * The caller sets reference bounds and any annotation confidence on the row. */
void hwa_paired_spans_map_reference(const HwaPairedSpans *spans, HwaPairedSpan *row);
void hwa_paired_spans_close(HwaPairedSpans *result);

#endif

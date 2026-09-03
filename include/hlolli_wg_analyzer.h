#ifndef HLOLLI_WG_ANALYZER_H
#define HLOLLI_WG_ANALYZER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HWA_VERSION "1.1.0"
#define HWA_ANALYSIS_METHOD_VERSION "stage1-1"
#define HWA_ISOLATED_NOTE_METHOD_VERSION "isolated-note-1"
#define HWA_HARMONIC_DECAY_METHOD_VERSION "harmonic-decay-1"
#define HWA_ALIGNMENT_METHOD_VERSION "stage2-1"
#define HWA_SEGMENTATION_METHOD_VERSION "stage3-1"
#define HWA_MEASUREMENT_METHOD_VERSION "stage4-1"
#define HWA_PROFILE_COMPARISON_METHOD_VERSION "stage4-compare-1"
#define HWA_PHYSICAL_CHECK_METHOD_VERSION "stage5-1"
#define HWA_PRODUCTION_METHOD_VERSION "stage6-1"
#define HWA_RUN_METHOD_VERSION "stage7-1"
#define HWA_EXPERIMENT_METHOD_VERSION "stage8-1"
#define HWA_GAP_REPORT_METHOD_VERSION "stage9-1"
#define HWA_GAP_REPORT_AUDIBILITY_METHOD "hwa-audibility-1"
#define HWA_EVENT_BUNDLE_SCHEMA "hwa-events"
#define HWA_EVENT_BUNDLE_SCHEMA_VERSION 1U
#define HWA_PRODUCTION_SOURCE_MEASUREMENT_METHOD_VERSION "stage4-1"
/* Kept for source compatibility with the original analysis API. */
#define HWA_METHOD_VERSION HWA_ANALYSIS_METHOD_VERSION
#define HWA_ERROR_SIZE 512
#define HWA_BAND_COUNT 10U
#define HWA_CHROMA_BIN_COUNT 12U
#define HWA_SHA256_HEX_SIZE 65U
#define HWA_PRODUCTION_EQ_NODE_COUNT 7U
#define HWA_PRODUCTION_ROOM_BAND_COUNT 6U

typedef enum HWAContainer {
    HWA_CONTAINER_RIFF = 1,
    HWA_CONTAINER_RF64 = 2
} HWAContainer;

typedef enum HWAEncoding {
    HWA_ENCODING_PCM = 1,
    HWA_ENCODING_IEEE_FLOAT = 3
} HWAEncoding;

typedef enum HWAChannelMode {
    HWA_CHANNEL_KEEP = 0,
    HWA_CHANNEL_SELECT = 1,
    HWA_CHANNEL_MIX = 2
} HWAChannelMode;

typedef struct HWAFormat {
    HWAContainer container;
    HWAEncoding encoding;
    uint16_t channels;
    uint32_t sample_rate_hz;
    uint16_t bits_per_sample;
    uint16_t valid_bits_per_sample;
    uint16_t block_align;
    uint32_t channel_mask;
    uint64_t frames;
    uint64_t data_bytes;
    double duration_seconds;
} HWAFormat;

/*
 * A bounded, seekable byte source for hosts that do not expose native paths.
 * read_at must fill the full requested span and return zero, or return nonzero
 * without changing source state visible to the analyzer. The analyzer checks
 * every span against size before it calls read_at. The caller owns name,
 * context, and all source bytes for the duration of the call.
 */
typedef int (*HWAReadAtFunction)(void *context,
                                 uint64_t offset,
                                 unsigned char *destination,
                                 size_t size);

typedef struct HWAByteSource {
    void *context;
    const char *name;
    uint64_t size;
    HWAReadAtFunction read_at;
} HWAByteSource;

typedef struct HWAAnalysisOptions {
    HWAChannelMode channel_mode;
    uint16_t selected_channel;
    size_t decode_block_frames;
    size_t frame_size;
    size_t hop_size;
    double silence_threshold_dbfs;
    uint64_t max_input_bytes;
    uint64_t max_input_frames;
    uint64_t max_work_bytes;
    size_t max_transforms;
    size_t max_track_points;
    size_t max_spectrum_values;
    size_t max_lag_samples;
    unsigned true_peak_oversample;
    int collect_tracks;
    int collect_spectrogram;
} HWAAnalysisOptions;

typedef struct HWAChannelMetrics {
    double peak;
    double true_peak;
    double rms;
    double dc_offset;
    double crest_factor;
    double zero_crossing_rate;
    uint64_t clipped_samples;
    int crest_factor_valid;
    int true_peak_valid;
} HWAChannelMetrics;

typedef struct HWALoudnessMetrics {
    double integrated_lufs;
    double loudness_range_lu;
    double momentary_max_lufs;
    double short_term_max_lufs;
    uint64_t blocks_above_absolute_gate;
    uint64_t blocks_above_relative_gate;
    int integrated_valid;
    int range_valid;
    int momentary_valid;
    int short_term_valid;
} HWALoudnessMetrics;

typedef struct HWASpectralMetrics {
    double centroid_hz;
    double spread_hz;
    double rolloff_85_hz;
    double flatness;
    double slope_db_per_octave;
    double mean_flux;
    double band_power[HWA_BAND_COUNT];
    uint64_t transform_count;
    int valid;
} HWASpectralMetrics;

typedef struct HWAActivityMetrics {
    double threshold_dbfs;
    double silence_fraction;
    double active_start_seconds;
    double active_end_seconds;
    int classified_valid;
    int active_span_valid;
} HWAActivityMetrics;

typedef struct HWAStereoMetrics {
    double correlation;
    double mid_rms;
    double side_rms;
    double balance_db;
    double width_ratio;
    double interchannel_delay_samples;
    double interchannel_delay_seconds;
    double interchannel_delay_confidence;
    double band_width[HWA_BAND_COUNT];
    uint16_t band_width_valid_mask;
    int available;
    int level_valid;
    int width_valid;
    int correlation_valid;
    int delay_valid;
} HWAStereoMetrics;

typedef struct HWAFrameMetrics {
    double time_seconds;
    double rms_dbfs;
    double frame_lufs;
    double pitch_hz;
    double pitch_confidence;
    double onset_strength;
    double energy_onset_strength;
    double phase_onset_strength;
    double pitch_change_strength;
    double combined_onset_strength;
    double chroma[HWA_CHROMA_BIN_COUNT];
    double spectral_centroid_hz;
    double spectral_rolloff_85_hz;
    double spectral_flatness;
    double band_power_db[HWA_BAND_COUNT];
    int loudness_valid;
    int pitch_valid;
    int spectrum_valid;
    int phase_onset_valid;
    int pitch_change_valid;
    int chroma_valid;
} HWAFrameMetrics;

typedef struct HWAAnalysis {
    char *path;
    HWAFormat format;
    HWAAnalysisOptions options;
    uint16_t analyzed_channels;
    HWAChannelMetrics *channels;
    HWALoudnessMetrics loudness;
    HWASpectralMetrics spectrum;
    HWAActivityMetrics activity;
    HWAStereoMetrics stereo;
    HWAFrameMetrics *tracks;
    size_t track_count;
    size_t spectrum_bins;
    double *spectrogram_db;
} HWAAnalysis;

typedef enum HWAIsolatedNoteMetric {
    HWA_ISOLATED_NOTE_PITCH = 1U << 0,
    HWA_ISOLATED_NOTE_PASSIVE_DECAY = 1U << 1
} HWAIsolatedNoteMetric;

enum {
    HWA_ISOLATED_NOTE_REJECT_SILENCE = 1U << 0,
    HWA_ISOLATED_NOTE_REJECT_NOISE = 1U << 1,
    HWA_ISOLATED_NOTE_REJECT_OCTAVE = 1U << 2,
    HWA_ISOLATED_NOTE_REJECT_BOUNDARY = 1U << 3,
    HWA_ISOLATED_NOTE_REJECT_LOW_SUPPORT = 1U << 4,
    HWA_ISOLATED_NOTE_REJECT_LOW_DYNAMIC_RANGE = 1U << 5,
    HWA_ISOLATED_NOTE_REJECT_HIGH_RESIDUAL = 1U << 6,
    HWA_ISOLATED_NOTE_REJECT_LATE_PULSE = 1U << 7
};

typedef struct HWAIsolatedNoteOptions {
    double expected_hz;
    uint32_t metric_mask;
    size_t decode_block_frames;
    uint64_t max_input_bytes;
    uint64_t max_input_frames;
    uint64_t max_work_bytes;
    uint64_t max_evaluations;
} HWAIsolatedNoteOptions;

typedef struct HWAIsolatedNotePitch {
    double hz;
    double cents;
    double confidence;
    double coverage;
    uint64_t window_count;
    uint64_t accepted_window_count;
    uint64_t start_sample;
    uint64_t end_sample;
    int valid;
} HWAIsolatedNotePitch;

typedef struct HWAIsolatedNoteDecay {
    double slope_db_per_second;
    double t60_seconds;
    double support_seconds;
    double dynamic_range_db;
    double residual_db;
    double floor_dbfs;
    uint64_t point_count;
    uint64_t start_sample;
    uint64_t end_sample;
    int valid;
} HWAIsolatedNoteDecay;

typedef struct HWAIsolatedNoteResult {
    char *path;
    HWAFormat format;
    double expected_hz;
    uint32_t requested_mask;
    uint32_t valid_mask;
    uint32_t rejection_mask;
    HWAIsolatedNotePitch pitch;
    HWAIsolatedNoteDecay decay;
    uint64_t peak_work_bytes;
    uint64_t evaluation_count;
} HWAIsolatedNoteResult;

enum {
    HWA_HARMONIC_DECAY_REJECT_NO_ONSET = 1U << 0,
    HWA_HARMONIC_DECAY_REJECT_LATE_PULSE = 1U << 1,
    HWA_HARMONIC_DECAY_REJECT_LOW_ANCHOR_SNR = 1U << 2,
    HWA_HARMONIC_DECAY_REJECT_BAND_OUT_OF_RANGE = 1U << 3,
    HWA_HARMONIC_DECAY_REJECT_LOW_SUPPORT = 1U << 4,
    HWA_HARMONIC_DECAY_REJECT_LOW_DYNAMIC_RANGE = 1U << 5,
    HWA_HARMONIC_DECAY_REJECT_NON_DECAY = 1U << 6,
    HWA_HARMONIC_DECAY_REJECT_HIGH_RESIDUAL = 1U << 7,
    HWA_HARMONIC_DECAY_REJECT_TRUNCATED_FIT = 1U << 8,
    HWA_HARMONIC_DECAY_REJECT_LOW_HARMONIC_COVERAGE = 1U << 9
};

typedef struct HWAHarmonicDecayOptions {
    double expected_hz;
    size_t decode_block_frames;
    uint64_t max_input_bytes;
    uint64_t max_input_frames;
    uint64_t max_work_bytes;
    uint64_t max_evaluations;
} HWAHarmonicDecayOptions;

typedef struct HWAHarmonicDecayBand {
    uint32_t harmonic_number;
    double target_hz;
    double selected_hz;
    size_t selected_bin;
    size_t signal_first_bin;
    size_t signal_last_bin;
    size_t lower_noise_first_bin;
    size_t lower_noise_last_bin;
    size_t upper_noise_first_bin;
    size_t upper_noise_last_bin;
    double anchor_snr_db;
    double slope_db_per_second;
    double t60_seconds;
    double support_seconds;
    double fit_dynamic_range_db;
    double residual_db;
    uint64_t fit_point_count;
    uint64_t fit_start_sample;
    uint64_t fit_end_sample;
    uint64_t tail_boundary_sample;
    uint32_t rejection_mask;
    int valid;
} HWAHarmonicDecayBand;

typedef struct HWAHarmonicDecayProfile {
    char *path;
    HWAFormat format;
    size_t fft_size;
    size_t hop_samples;
    uint64_t onset_sample;
    uint64_t broad_peak_sample;
    uint64_t analysis_start_sample;
    uint64_t analysis_end_sample;
    HWAHarmonicDecayBand *bands;
    size_t band_count;
    size_t valid_band_count;
    uint32_t rejection_mask;
    uint64_t peak_work_bytes;
    uint64_t evaluation_count;
    int valid;
} HWAHarmonicDecayProfile;

typedef struct HWAHarmonicDecayComparison {
    uint32_t harmonic_number;
    double t60_log_error_db;
    int reference_valid;
    int model_valid;
    int valid;
} HWAHarmonicDecayComparison;

typedef struct HWAHarmonicDecayResult {
    HWAHarmonicDecayOptions options;
    double expected_hz;
    HWAHarmonicDecayProfile reference;
    HWAHarmonicDecayProfile model;
    HWAHarmonicDecayComparison *comparisons;
    size_t comparison_count;
    size_t shared_valid_band_count;
    double shared_reference_coverage;
    double t60_log_rmse_db;
    double median_t60_log_bias_db;
    uint64_t peak_work_bytes;
    uint64_t evaluation_count;
    int model_present;
    int comparison_valid;
} HWAHarmonicDecayResult;

typedef enum HWABodyEnvelopeStatus {
    HWA_BODY_ENVELOPE_VALID = 1,
    HWA_BODY_ENVELOPE_LOW_SUPPORT = 2,
    HWA_BODY_ENVELOPE_NO_SUPPORT = 3
} HWABodyEnvelopeStatus;

enum {
    HWA_BODY_ENVELOPE_LOW_POINT_SUPPORT = 1U << 0,
    HWA_BODY_ENVELOPE_LOW_PITCH_SPREAD = 1U << 1,
    HWA_BODY_ENVELOPE_LOW_PARTIAL_SPREAD = 1U << 2,
    HWA_BODY_ENVELOPE_HIGH_RESIDUAL = 1U << 3
};

typedef struct HWABodyEnvelopeOptions {
    HWAAnalysisOptions analysis;
    double min_frequency_hz;
    double max_frequency_hz;
    double min_pitch_confidence;
    size_t max_harmonics;
    size_t bins_per_octave;
    size_t fit_passes;
    size_t max_observations;
    size_t max_points;
    uint64_t max_fit_evaluations;
} HWABodyEnvelopeOptions;

typedef struct HWABodyEnvelopePoint {
    double frequency_hz;
    double relative_db;
    double residual_spread_db;
    double confidence;
    uint64_t observation_count;
    size_t pitch_cell_count;
    size_t harmonic_count;
    uint32_t quality_flags;
    int valid;
} HWABodyEnvelopePoint;

typedef struct HWABodyEnvelopeEstimate {
    char *path;
    HWABodyEnvelopeStatus status;
    double confidence;
    double pitch_min_hz;
    double pitch_max_hz;
    uint64_t frames_seen;
    uint64_t frames_used;
    uint64_t frames_rejected_pitch;
    size_t observation_count;
    HWABodyEnvelopePoint *points;
    size_t point_count;
} HWABodyEnvelopeEstimate;

typedef struct HWABodyEnvelopeGap {
    double frequency_hz;
    double model_minus_reference_db;
    double confidence;
    int valid;
} HWABodyEnvelopeGap;

typedef struct HWABodyEnvelopeResult {
    HWABodyEnvelopeOptions options;
    HWABodyEnvelopeEstimate reference;
    HWABodyEnvelopeEstimate model;
    HWABodyEnvelopeGap *gaps;
    size_t gap_count;
    double shape_rmse_db;
    double shape_correlation;
    double comparison_confidence;
    uint64_t retained_work_bytes;
    uint64_t fit_evaluations;
    int model_present;
    int comparison_valid;
} HWABodyEnvelopeResult;

typedef enum HWAAlignmentMode {
    HWA_ALIGNMENT_AUDIO_TO_AUDIO = 1,
    HWA_ALIGNMENT_SCORE_TO_AUDIO = 2
} HWAAlignmentMode;

typedef enum HWAAlignmentOrigin {
    HWA_ALIGNMENT_ORIGIN_AUTO = 1,
    HWA_ALIGNMENT_ORIGIN_MANUAL = 2
} HWAAlignmentOrigin;

typedef enum HWAAlignmentSide {
    HWA_ALIGNMENT_REFERENCE = 1,
    HWA_ALIGNMENT_TARGET = 2
} HWAAlignmentSide;

typedef enum HWAAlignmentStatus {
    HWA_ALIGNMENT_MATCHED = 1,
    HWA_ALIGNMENT_LOW_CONFIDENCE = 2,
    HWA_ALIGNMENT_SKIPPED = 3,
    HWA_ALIGNMENT_REPEATED = 4,
    HWA_ALIGNMENT_REST = 5,
    HWA_ALIGNMENT_ORNAMENT = 6,
    HWA_ALIGNMENT_CADENZA = 7
} HWAAlignmentStatus;

typedef enum HWAUnmatchedReason {
    HWA_UNMATCHED_PREFIX = 1,
    HWA_UNMATCHED_SUFFIX = 2,
    HWA_UNMATCHED_SKIP = 3,
    HWA_UNMATCHED_REPEAT = 4,
    HWA_UNMATCHED_REST = 5,
    HWA_UNMATCHED_CADENZA = 6,
    HWA_UNMATCHED_LOW_CONFIDENCE = 7,
    HWA_UNMATCHED_NO_EVIDENCE = 8
} HWAUnmatchedReason;

enum {
    HWA_ALIGNMENT_EVIDENCE_CHROMA = 1U << 0,
    HWA_ALIGNMENT_EVIDENCE_SPECTRAL_ONSET = 1U << 1,
    HWA_ALIGNMENT_EVIDENCE_ENERGY_ONSET = 1U << 2,
    HWA_ALIGNMENT_EVIDENCE_PHASE_ONSET = 1U << 3,
    HWA_ALIGNMENT_EVIDENCE_PITCH = 1U << 4,
    HWA_ALIGNMENT_EVIDENCE_ENVELOPE = 1U << 5
};

typedef struct HWAAlignmentOptions {
    HWAAnalysisOptions analysis;
    double alignment_step_seconds;
    double coarse_step_seconds;
    double dtw_band_seconds;
    double fine_radius_seconds;
    double refine_radius_seconds;
    double match_threshold;
    double chroma_weight;
    double onset_weight;
    double pitch_weight;
    double envelope_weight;
    double activity_weight;
    double skip_cost;
    double repeat_cost;
    double ornament_cost;
    double rest_cost;
    double cadenza_cost;
    uint64_t max_dtw_cells;
    uint64_t max_alignment_work_bytes;
    size_t max_alignment_points;
    size_t max_score_events;
    size_t max_manual_anchors;
} HWAAlignmentOptions;

typedef struct HWAAlignmentAnchor {
    uint64_t id;
    double reference_seconds;
    double target_seconds;
    double score_beat;
    double confidence;
    uint32_t evidence_flags;
    HWAAlignmentOrigin origin;
    int score_beat_valid;
    int locked;
} HWAAlignmentAnchor;

typedef struct HWAAlignmentMatch {
    uint64_t id;
    double reference_start_seconds;
    double reference_end_seconds;
    double target_start_seconds;
    double target_end_seconds;
    double score_start_beat;
    double score_end_beat;
    double confidence;
    uint32_t evidence_flags;
    HWAAlignmentStatus status;
    int score_span_valid;
    char *event_id;
    char *kind;
    char *voice;
    char *midi_note;
    char *velocity;
    char *tie;
    char *dynamic;
    char *mark;
    char *score_position;
    double tempo_bpm;
    int tempo_valid;
} HWAAlignmentMatch;

typedef struct HWAUnmatchedSpan {
    uint64_t id;
    HWAAlignmentSide side;
    HWAUnmatchedReason reason;
    double start_seconds;
    double end_seconds;
    double start_beat;
    double end_beat;
    double confidence;
    int score_span_valid;
} HWAUnmatchedSpan;

typedef struct HWAAlignmentWarning {
    uint64_t id;
    char *code;
    char *message;
} HWAAlignmentWarning;

typedef struct HWAAlignment {
    HWAAlignmentMode mode;
    HWAAlignmentOptions options;
    char *reference_path;
    char *target_path;
    char *score_path;
    char reference_sha256[HWA_SHA256_HEX_SIZE];
    char target_sha256[HWA_SHA256_HEX_SIZE];
    char score_sha256[HWA_SHA256_HEX_SIZE];
    double reference_duration_seconds;
    double target_duration_seconds;
    double tuning_offset_cents;
    double tuning_confidence;
    double total_cost;
    double normalized_cost;
    double matched_coverage;
    double global_confidence;
    uint64_t dtw_cells;
    uint64_t path_points;
    uint64_t retained_work_bytes;
    HWAAlignmentAnchor *anchors;
    size_t anchor_count;
    HWAAlignmentMatch *matches;
    size_t match_count;
    HWAUnmatchedSpan *unmatched_spans;
    size_t unmatched_span_count;
    HWAAlignmentWarning *warnings;
    size_t warning_count;
} HWAAlignment;

typedef enum HWAItemKind {
    HWA_ITEM_NOTE = 1,
    HWA_ITEM_ATTACK = 2,
    HWA_ITEM_BODY = 3,
    HWA_ITEM_RELEASE = 4,
    HWA_ITEM_RESIDUAL_TAIL = 5,
    HWA_ITEM_REST = 6,
    HWA_ITEM_TRANSITION = 7,
    HWA_ITEM_GESTURE = 8,
    HWA_ITEM_MULTI_NOTE = 9
} HWAItemKind;

typedef enum HWAItemOrigin {
    HWA_ITEM_ORIGIN_AUTO = 1,
    HWA_ITEM_ORIGIN_MANUAL = 2
} HWAItemOrigin;

typedef enum HWAItemMemberRole {
    HWA_ITEM_MEMBER_SOURCE = 1,
    HWA_ITEM_MEMBER_FROM = 2,
    HWA_ITEM_MEMBER_TO = 3,
    HWA_ITEM_MEMBER_ACTIVE = 4
} HWAItemMemberRole;

enum {
    HWA_ITEM_EVIDENCE_ALIGNMENT = 1U << 0,
    HWA_ITEM_EVIDENCE_ONSET = 1U << 1,
    HWA_ITEM_EVIDENCE_ENERGY = 1U << 2,
    HWA_ITEM_EVIDENCE_PITCH = 1U << 3,
    HWA_ITEM_EVIDENCE_SCORE = 1U << 4,
    HWA_ITEM_EVIDENCE_MANUAL = 1U << 5
};

enum {
    HWA_ITEM_QUALITY_LOW_CONFIDENCE = 1U << 0,
    HWA_ITEM_QUALITY_NO_EVIDENCE = 1U << 1,
    HWA_ITEM_QUALITY_COLLAPSED = 1U << 2,
    HWA_ITEM_QUALITY_TRUNCATED = 1U << 3
};

enum {
    HWA_LABEL_OVERRIDE_PITCH = 1U << 0,
    HWA_LABEL_OVERRIDE_REGISTER = 1U << 1,
    HWA_LABEL_OVERRIDE_DYNAMIC = 1U << 2,
    HWA_LABEL_OVERRIDE_ARTICULATION = 1U << 3,
    HWA_LABEL_OVERRIDE_PART = 1U << 4,
    HWA_LABEL_OVERRIDE_PHYSICAL_ELEMENT = 1U << 5,
    HWA_LABEL_OVERRIDE_CONTROLLER = 1U << 6,
    HWA_LABEL_OVERRIDE_TECHNIQUE = 1U << 7,
    HWA_LABEL_OVERRIDE_SCORE_SECTION = 1U << 8,
    HWA_LABEL_OVERRIDE_TRANSITION = 1U << 9,
    HWA_LABEL_OVERRIDE_GESTURE = 1U << 10
};

typedef struct HWATypedLabels {
    char *pitch;
    char *register_name;
    char *dynamic;
    char *articulation;
    char *part;
    char *physical_element;
    char *controller;
    char *technique;
    char *score_section;
    char *transition;
    char *gesture;
    uint32_t override_flags;
} HWATypedLabels;

typedef struct HWASegmentationOptions {
    size_t decode_block_frames;
    uint64_t max_input_bytes;
    uint64_t max_input_frames;
    uint64_t max_analysis_work_bytes;
    size_t max_transforms;
    size_t max_track_points;
    double boundary_search_seconds;
    double tail_limit_seconds;
    double min_phase_seconds;
    double min_body_seconds;
    double item_confidence_threshold;
    uint64_t max_segmentation_work_bytes;
    uint64_t max_boundary_evaluations;
    size_t max_events;
    size_t max_items;
    size_t max_item_members;
    size_t max_label_rows;
    size_t max_manual_items;
} HWASegmentationOptions;

typedef struct HWAItemEvent {
    uint64_t id;
    char *event_id;
    char *kind;
    char *voice;
    char *midi_note;
    char *velocity;
    char *tie;
    char *dynamic;
    char *mark;
    char *score_position;
    HWATypedLabels labels;
    double score_start_beat;
    double score_end_beat;
    double score_start_seconds;
    double score_end_seconds;
    uint64_t audio_start_sample;
    uint64_t audio_end_sample;
    double audio_start_seconds;
    double audio_end_seconds;
    double tempo_bpm;
    double alignment_confidence;
    uint32_t alignment_evidence_flags;
    HWAAlignmentStatus alignment_status;
    int tempo_valid;
} HWAItemEvent;

typedef struct HWAItem {
    uint64_t id;
    char *key;
    char *role;
    char *exclusion_reason;
    HWAItemKind kind;
    uint64_t parent_id;
    uint64_t start_sample;
    uint64_t end_sample;
    double start_seconds;
    double end_seconds;
    double score_start_beat;
    double score_end_beat;
    double confidence;
    uint32_t evidence_flags;
    uint32_t quality_flags;
    HWAItemOrigin origin;
    int parent_valid;
    int locked;
    int excluded;
} HWAItem;

typedef struct HWAItemMember {
    uint64_t item_id;
    uint64_t event_id;
    uint32_t order;
    HWAItemMemberRole role;
} HWAItemMember;

typedef struct HWAItemWarning {
    uint64_t id;
    char *code;
    char *message;
    uint64_t item_id;
    uint64_t event_id;
    int item_id_valid;
    int event_id_valid;
} HWAItemWarning;

/*
 * Item keys and reason strings in an edit remain caller-owned. Locked sample
 * bounds are half-open. Set exclusion_set when `excluded` should replace the
 * automatic value, including when it should clear a prior exclusion.
 */
typedef struct HWAItemEdit {
    const char *key;
    uint64_t start_sample;
    uint64_t end_sample;
    const char *exclusion_reason;
    int locked;
    int exclusion_set;
    int excluded;
} HWAItemEdit;

typedef struct HWAItemSet {
    HWASegmentationOptions options;
    char *alignment_path;
    char *audio_path;
    char *labels_path;
    char *amendment_path;
    char *source_score_path;
    char alignment_sha256[HWA_SHA256_HEX_SIZE];
    char audio_sha256[HWA_SHA256_HEX_SIZE];
    char labels_sha256[HWA_SHA256_HEX_SIZE];
    char amendment_sha256[HWA_SHA256_HEX_SIZE];
    char source_score_sha256[HWA_SHA256_HEX_SIZE];
    HWAFormat audio_format;
    double source_score_duration_seconds;
    double alignment_confidence;
    uint64_t boundary_evaluations;
    uint64_t retained_work_bytes;
    HWAItemEvent *events;
    size_t event_count;
    HWAItem *items;
    size_t item_count;
    HWAItemMember *members;
    size_t member_count;
    HWAItemWarning *warnings;
    size_t warning_count;
    size_t locked_item_count;
    size_t excluded_item_count;
    size_t low_confidence_item_count;
} HWAItemSet;

typedef enum HWAMeasureView {
    HWA_MEASURE_VIEW_RAW = 1,
    HWA_MEASURE_VIEW_LEVEL_RELATIVE = 2,
    /* Reserved for production accounting. Item measurement does not emit it. */
    HWA_MEASURE_VIEW_PRODUCTION_CORRECTED = 3,
    HWA_MEASURE_VIEW_COUNT = 4
} HWAMeasureView;

typedef enum HWAMeasureStatus {
    HWA_MEASURE_STATUS_VALID = 1,
    HWA_MEASURE_STATUS_NO_DATA = 2,
    HWA_MEASURE_STATUS_UNSUPPORTED_ITEM = 3,
    HWA_MEASURE_STATUS_EMPTY_SPAN = 4,
    HWA_MEASURE_STATUS_TOO_SHORT = 5,
    HWA_MEASURE_STATUS_NO_SIGNAL = 6,
    HWA_MEASURE_STATUS_BELOW_FLOOR = 7,
    HWA_MEASURE_STATUS_NO_PITCH = 8,
    HWA_MEASURE_STATUS_MULTI_PITCH = 9,
    HWA_MEASURE_STATUS_NO_REFERENCE = 10,
    HWA_MEASURE_STATUS_COUNT = 11
} HWAMeasureStatus;

typedef enum HWAMeasureUnit {
    HWA_MEASURE_UNIT_DBFS = 1,
    HWA_MEASURE_UNIT_DB = 2,
    HWA_MEASURE_UNIT_HZ = 3,
    HWA_MEASURE_UNIT_HZ_PER_SECOND = 4,
    HWA_MEASURE_UNIT_SECONDS = 5,
    HWA_MEASURE_UNIT_RATIO = 6,
    HWA_MEASURE_UNIT_CENTS = 7,
    HWA_MEASURE_UNIT_CENTS_PER_SECOND = 8,
    HWA_MEASURE_UNIT_DB_PER_SECOND = 9,
    HWA_MEASURE_UNIT_HARMONIC_INDEX = 10,
    HWA_MEASURE_UNIT_ORDER = 11,
    HWA_MEASURE_UNIT_COUNT = 12
} HWAMeasureUnit;

/*
 * Scalar measures use index zero. Band measures use indexes 0 through 9 in
 * the HWA_BAND_COUNT order. Partial measures use indexes 1 through the
 * requested max_partials value.
 */
typedef enum HWAMeasureKind {
    HWA_MEASURE_RMS_DBFS = 1,
    HWA_MEASURE_PEAK_DBFS = 2,
    HWA_MEASURE_CREST_DB = 3,
    HWA_MEASURE_BAND_LEVEL_DBFS = 4,
    HWA_MEASURE_BAND_BALANCE_DB = 5,
    HWA_MEASURE_CENTROID_HZ = 6,
    HWA_MEASURE_FLATNESS = 7,
    HWA_MEASURE_LEVEL_SLOPE_DB_PER_SECOND = 8,
    HWA_MEASURE_CENTROID_SLOPE_HZ_PER_SECOND = 9,
    HWA_MEASURE_BAND_SLOPE_DB_PER_SECOND = 10,
    HWA_MEASURE_TRANSIENT_RATE_HZ = 11,
    HWA_MEASURE_FIXED_STATE_FRACTION = 12,
    HWA_MEASURE_LEVEL_MODULATION_SPREAD_DB = 13,
    HWA_MEASURE_CENTROID_MODULATION_SPREAD_HZ = 14,
    HWA_MEASURE_BAND_MODULATION_SPREAD_DB = 15,
    HWA_MEASURE_PITCH_HZ = 16,
    HWA_MEASURE_TUNING_OFFSET_CENTS = 17,
    HWA_MEASURE_PITCH_SPREAD_CENTS = 18,
    HWA_MEASURE_PITCH_OVERSHOOT_CENTS = 19,
    HWA_MEASURE_PITCH_UNDERSHOOT_CENTS = 20,
    HWA_MEASURE_PITCH_DRIFT_CENTS_PER_SECOND = 21,
    HWA_MEASURE_PITCH_SETTLE_SECONDS = 22,
    HWA_MEASURE_OCTAVE_FAULT_FRACTION = 23,
    HWA_MEASURE_GLIDE_TIME_SECONDS = 24,
    HWA_MEASURE_PORTAMENTO_LINEARITY = 25,
    HWA_MEASURE_INHARMONICITY_B = 26,
    HWA_MEASURE_ODD_EVEN_BALANCE_DB = 27,
    HWA_MEASURE_HARMONIC_CENTROID = 28,
    HWA_MEASURE_HARMONIC_LEVEL_DBFS = 29,
    HWA_MEASURE_RESIDUAL_LEVEL_DBFS = 30,
    HWA_MEASURE_HNR_DB = 31,
    HWA_MEASURE_PARTIAL_FREQUENCY_ERROR_CENTS = 32,
    HWA_MEASURE_PARTIAL_LEVEL_DBFS = 33,
    HWA_MEASURE_PARTIAL_BALANCE_DB = 34,
    HWA_MEASURE_PARTIAL_PRESENCE_FRACTION = 35,
    HWA_MEASURE_PARTIAL_BIRTH_SECONDS = 36,
    HWA_MEASURE_PARTIAL_LOSS_SECONDS = 37,
    HWA_MEASURE_PARTIAL_LEVEL_SLOPE_DB_PER_SECOND = 38,
    HWA_MEASURE_PARTIAL_ONSET_ORDER = 39,
    HWA_MEASURE_PARTIAL_ONSET_SPREAD_SECONDS = 40,
    HWA_MEASURE_RESIDUAL_BAND_LEVEL_DBFS = 41,
    HWA_MEASURE_RESIDUAL_BAND_BALANCE_DB = 42,
    HWA_MEASURE_HARMONIC_DECAY_DB_PER_SECOND = 43,
    HWA_MEASURE_VIBRATO_DELAY_SECONDS = 44,
    HWA_MEASURE_VIBRATO_RATE_HZ = 45,
    HWA_MEASURE_VIBRATO_DEPTH_CENTS = 46,
    HWA_MEASURE_VIBRATO_WAVEFORM_RESIDUAL_RATIO = 47,
    HWA_MEASURE_VIBRATO_RATE_DRIFT_HZ_PER_SECOND = 48,
    HWA_MEASURE_VIBRATO_DEPTH_DRIFT_CENTS_PER_SECOND = 49,
    HWA_MEASURE_PITCH_LEVEL_CORRELATION = 50,
    HWA_MEASURE_PITCH_TONE_CORRELATION = 51,
    HWA_MEASURE_ATTACK_DELAY_SECONDS = 52,
    HWA_MEASURE_RISE_10_SECONDS = 53,
    HWA_MEASURE_RISE_50_SECONDS = 54,
    HWA_MEASURE_RISE_90_SECONDS = 55,
    HWA_MEASURE_ATTACK_SLOPE_DB_PER_SECOND = 56,
    HWA_MEASURE_ATTACK_OVERSHOOT_DB = 57,
    HWA_MEASURE_NOISE_BURST_SECONDS = 58,
    HWA_MEASURE_PRE_NOTE_RESIDUAL_DBFS = 59,
    HWA_MEASURE_EARLY_DAMPING_DB_PER_SECOND = 60,
    HWA_MEASURE_PITCH_FALL_CENTS = 61,
    HWA_MEASURE_DECAY_DB = 62,
    HWA_MEASURE_RESIDUAL_EXCITATION_DBFS = 63,
    HWA_MEASURE_GAP_OVERLAP_SECONDS = 64,
    HWA_MEASURE_CARRYOVER_DB = 65,
    HWA_MEASURE_TRANSITION_PITCH_CHANGE_CENTS = 66,
    HWA_MEASURE_TRANSITION_TONE_CHANGE_HZ = 67,
    HWA_MEASURE_REPEATED_ATTACK_SIMILARITY = 68,
    HWA_MEASURE_REPEATED_PITCH_CURVE_SIMILARITY = 69,
    HWA_MEASURE_LOCAL_CONTRAST_DB = 70,
    HWA_MEASURE_ACCENT_SIZE_DB = 71,
    HWA_MEASURE_DURATION_SECONDS = 72,
    HWA_MEASURE_KIND_COUNT = 73
} HWAMeasureKind;

typedef enum HWAMeasureGroupSelector {
    HWA_MEASURE_GROUP_ALL = 1,
    HWA_MEASURE_GROUP_PITCH = 2,
    HWA_MEASURE_GROUP_REGISTER = 3,
    HWA_MEASURE_GROUP_DYNAMIC = 4,
    HWA_MEASURE_GROUP_ARTICULATION = 5,
    HWA_MEASURE_GROUP_PART = 6,
    HWA_MEASURE_GROUP_PHYSICAL_ELEMENT = 7,
    HWA_MEASURE_GROUP_CONTROLLER = 8,
    HWA_MEASURE_GROUP_TECHNIQUE = 9,
    HWA_MEASURE_GROUP_SCORE_SECTION = 10,
    HWA_MEASURE_GROUP_TRANSITION = 11,
    HWA_MEASURE_GROUP_GESTURE = 12,
    HWA_MEASURE_GROUP_SELECTOR_COUNT = 13
} HWAMeasureGroupSelector;

enum {
    HWA_MEASURE_EVIDENCE_SAMPLES = 1U << 0,
    HWA_MEASURE_EVIDENCE_ENVELOPE = 1U << 1,
    HWA_MEASURE_EVIDENCE_SPECTRUM = 1U << 2,
    HWA_MEASURE_EVIDENCE_PITCH = 1U << 3,
    HWA_MEASURE_EVIDENCE_TARGET_PITCH = 1U << 4,
    HWA_MEASURE_EVIDENCE_HARMONICS = 1U << 5,
    HWA_MEASURE_EVIDENCE_RESIDUAL = 1U << 6,
    HWA_MEASURE_EVIDENCE_ITEM_BOUNDS = 1U << 7,
    HWA_MEASURE_EVIDENCE_MEMBERS = 1U << 8,
    HWA_MEASURE_EVIDENCE_PREVIOUS_ITEM = 1U << 9,
    HWA_MEASURE_EVIDENCE_LEVEL_REFERENCE = 1U << 10
};

enum {
    HWA_MEASURE_QUALITY_LOW_CONFIDENCE = 1U << 0,
    HWA_MEASURE_QUALITY_INCOMPLETE_COVERAGE = 1U << 1,
    HWA_MEASURE_QUALITY_AMBIGUOUS_CONTEXT = 1U << 2,
    HWA_MEASURE_QUALITY_BOUNDARY_LIMITED = 1U << 3,
    HWA_MEASURE_QUALITY_INHERITED_ITEM = 1U << 4,
    HWA_MEASURE_QUALITY_NEAR_SPECTRAL_FLOOR = 1U << 5,
    HWA_MEASURE_QUALITY_FALLBACK = 1U << 6
};

enum {
    HWA_MEASURE_CAPABILITY_PRODUCTION_CORRECTION = 1U << 0,
    HWA_MEASURE_CAPABILITY_NUMERIC_CONTROL_PROBES = 1U << 1
};

typedef struct HWAMeasurementOptions {
    size_t decode_block_frames;
    size_t fft_size;
    size_t hop_size;
    double pitch_confidence_floor;
    double spectral_floor_dbfs;
    size_t max_partials;
    uint64_t max_input_bytes;
    uint64_t max_input_frames;
    uint64_t max_work_bytes;
    size_t max_transforms;
    size_t max_series_points;
    uint64_t max_item_frame_evaluations;
    size_t max_events;
    size_t max_items;
    size_t max_item_members;
    size_t max_measurements;
    size_t max_groups;
    size_t max_group_members;
    size_t max_statistics;
    size_t max_warnings;
} HWAMeasurementOptions;

typedef struct HWAMeasureItemContext {
    uint64_t item_id;
    char *item_key;
    HWAItemKind item_kind;
    char *item_role;
    uint64_t start_sample;
    uint64_t end_sample;
    HWATypedLabels labels;
    size_t source_event_count;
    double item_confidence;
    uint32_t item_quality_flags;
    int excluded;
} HWAMeasureItemContext;

typedef struct HWAMeasureObservation {
    uint64_t id;
    uint64_t item_id;
    HWAMeasureKind kind;
    uint32_t index;
    HWAMeasureUnit unit;
    HWAMeasureView view;
    HWAMeasureStatus status;
    double value;
    double confidence;
    uint32_t evidence_flags;
    uint32_t quality_flags;
} HWAMeasureObservation;

typedef struct HWAMeasureGroup {
    uint64_t id;
    char *key;
    HWAItemKind item_kind;
    char *item_role;
    HWAMeasureGroupSelector selector;
    char *value;
    size_t member_count;
} HWAMeasureGroup;

typedef struct HWAMeasureGroupMember {
    uint64_t group_id;
    uint64_t item_id;
} HWAMeasureGroupMember;

typedef struct HWAMeasureStatistics {
    size_t total_count;
    size_t valid_count;
    size_t missing_count;
    double minimum;
    double q05;
    double q25;
    double q50;
    double q75;
    double q95;
    double maximum;
    double mean;
    double population_sd;
    double confidence;
    int valid;
} HWAMeasureStatistics;

typedef struct HWAMeasureStatistic {
    uint64_t id;
    uint64_t group_id;
    HWAMeasureKind kind;
    uint32_t index;
    HWAMeasureUnit unit;
    HWAMeasureView view;
    HWAMeasureStatistics statistics;
    uint32_t quality_flags;
} HWAMeasureStatistic;

typedef struct HWAMeasureWarning {
    uint64_t id;
    char *code;
    char *message;
    uint64_t item_id;
    uint64_t observation_id;
    int item_id_valid;
    int observation_id_valid;
} HWAMeasureWarning;

typedef struct HWAMeasurementSet {
    HWAMeasurementOptions options;
    char *items_path;
    char *audio_path;
    char *alignment_path;
    char *labels_path;
    char *amendment_path;
    char *source_score_path;
    char items_sha256[HWA_SHA256_HEX_SIZE];
    char audio_sha256[HWA_SHA256_HEX_SIZE];
    char alignment_sha256[HWA_SHA256_HEX_SIZE];
    char labels_sha256[HWA_SHA256_HEX_SIZE];
    char amendment_sha256[HWA_SHA256_HEX_SIZE];
    char source_score_sha256[HWA_SHA256_HEX_SIZE];
    HWAFormat audio_format;
    double level_reference_dbfs;
    size_t level_reference_item_count;
    int level_reference_valid;
    uint32_t capability_flags;
    uint64_t item_frame_evaluations;
    uint64_t retained_work_bytes;
    size_t transform_count;
    HWAMeasureItemContext *contexts;
    size_t context_count;
    HWAMeasureObservation *measurements;
    size_t measurement_count;
    HWAMeasureGroup *groups;
    size_t group_count;
    HWAMeasureGroupMember *group_members;
    size_t group_member_count;
    HWAMeasureStatistic *statistics;
    size_t statistic_count;
    HWAMeasureWarning *warnings;
    size_t warning_count;
} HWAMeasurementSet;

typedef struct HWAProfileComparisonOptions {
    uint64_t max_input_bytes;
    uint64_t max_work_bytes;
    size_t max_contexts;
    size_t max_measurements;
    size_t max_groups;
    size_t max_group_members;
    size_t max_statistics;
    size_t max_warnings;
    size_t max_distributions;
    size_t max_gaps;
} HWAProfileComparisonOptions;

typedef struct HWAProfileDistribution {
    uint64_t id;
    uint64_t group_id;
    HWAMeasureKind kind;
    uint32_t index;
    HWAMeasureUnit unit;
    HWAMeasureView view;
    HWAMeasureStatistics reference_statistics;
    HWAMeasureStatistics model_statistics;
    int reference_valid;
    int model_valid;
} HWAProfileDistribution;

typedef struct HWAProfileGap {
    uint64_t id;
    uint64_t distribution_id;
    double mean_delta;
    double median_delta;
    double quantile_distance;
    double standardized_mean_shift;
    double valid_coverage;
    double gap_score;
    size_t rank;
    uint32_t quality_flags;
    int mean_delta_valid;
    int median_delta_valid;
    int quantile_distance_valid;
    int standardized_mean_shift_valid;
    int valid_coverage_valid;
    int gap_score_valid;
} HWAProfileGap;

typedef struct HWAProfileWarning {
    uint64_t id;
    char *code;
    char *message;
    uint64_t group_id;
    uint64_t distribution_id;
    int group_id_valid;
    int distribution_id_valid;
} HWAProfileWarning;

typedef struct HWAProfileComparisonSet {
    HWAProfileComparisonOptions options;
    char *reference_path;
    char *model_path;
    char reference_sha256[HWA_SHA256_HEX_SIZE];
    char model_sha256[HWA_SHA256_HEX_SIZE];
    uint64_t retained_work_bytes;
    HWAMeasureGroup *groups;
    size_t group_count;
    HWAProfileDistribution *distributions;
    size_t distribution_count;
    HWAProfileGap *gaps;
    size_t gap_count;
    HWAProfileWarning *warnings;
    size_t warning_count;
} HWAProfileComparisonSet;

typedef enum HWAPhysicalAvailability {
    HWA_PHYSICAL_AVAILABLE = 1,
    HWA_PHYSICAL_UNAVAILABLE = 2,
    HWA_PHYSICAL_INSUFFICIENT = 3,
    HWA_PHYSICAL_AVAILABILITY_COUNT = 4
} HWAPhysicalAvailability;

typedef enum HWAPhysicalFindingClass {
    HWA_PHYSICAL_FINDING_NONE = 1,
    HWA_PHYSICAL_FINDING_GAP = 2,
    HWA_PHYSICAL_FINDING_FAULT = 3,
    HWA_PHYSICAL_FINDING_UNAVAILABLE = 4,
    HWA_PHYSICAL_FINDING_CLASS_COUNT = 5
} HWAPhysicalFindingClass;

typedef enum HWAPhysicalSeverity {
    HWA_PHYSICAL_SEVERITY_INFO = 1,
    HWA_PHYSICAL_SEVERITY_WARNING = 2,
    HWA_PHYSICAL_SEVERITY_CRITICAL = 3,
    HWA_PHYSICAL_SEVERITY_COUNT = 4
} HWAPhysicalSeverity;

typedef enum HWAPhysicalUnit {
    HWA_PHYSICAL_UNIT_DBFS = 1,
    HWA_PHYSICAL_UNIT_DB = 2,
    HWA_PHYSICAL_UNIT_HZ = 3,
    HWA_PHYSICAL_UNIT_SECONDS = 4,
    HWA_PHYSICAL_UNIT_RATIO = 5,
    HWA_PHYSICAL_UNIT_CENTS = 6,
    HWA_PHYSICAL_UNIT_SAMPLES = 7,
    HWA_PHYSICAL_UNIT_HZ_PER_SECOND = 8,
    HWA_PHYSICAL_UNIT_DB_PER_SECOND = 9,
    HWA_PHYSICAL_UNIT_COUNT_VALUE = 10,
    HWA_PHYSICAL_UNIT_COUNT = 11
} HWAPhysicalUnit;

typedef enum HWAPhysicalCheckKind {
    HWA_PHYSICAL_ELEMENT_TRAIT_DELTA = 1,
    HWA_PHYSICAL_ELEMENT_REFERENCE_DISTANCE = 2,
    HWA_PHYSICAL_ELEMENT_MODEL_DISTANCE = 3,
    HWA_PHYSICAL_ELEMENT_DISTINCTNESS_RATIO = 4,
    HWA_PHYSICAL_ELEMENT_GAIN_ONLY_SCORE = 5,
    HWA_PHYSICAL_ELEMENT_PITCH_ONLY_SCORE = 6,
    HWA_PHYSICAL_ELEMENT_CARRYOVER_DB = 7,
    HWA_PHYSICAL_BODY_MODE_FREQUENCY_HZ = 8,
    HWA_PHYSICAL_BODY_MODE_BANDWIDTH_HZ = 9,
    HWA_PHYSICAL_BODY_MODE_Q = 10,
    HWA_PHYSICAL_BODY_MODE_PROMINENCE_DB = 11,
    HWA_PHYSICAL_BODY_MODE_DECAY_SECONDS = 12,
    HWA_PHYSICAL_BODY_MODE_PAN = 13,
    HWA_PHYSICAL_BODY_MODE_DENSITY_PER_KHZ = 14,
    HWA_PHYSICAL_BODY_MODE_DISTANCE_CENTS = 15,
    HWA_PHYSICAL_JOINT_RESIDUAL_DB = 16,
    HWA_PHYSICAL_SHARED_GAIN_DB = 17,
    HWA_PHYSICAL_INTERMODULATION_RATIO = 18,
    HWA_PHYSICAL_SUM_TONE_DB = 19,
    HWA_PHYSICAL_DIFFERENCE_TONE_DB = 20,
    HWA_PHYSICAL_BEATING_DEPTH_RATIO = 21,
    HWA_PHYSICAL_BEATING_RATE_HZ = 22,
    HWA_PHYSICAL_ROUGHNESS_RATIO = 23,
    HWA_PHYSICAL_PITCH_PULL_CENTS = 24,
    HWA_PHYSICAL_RENDER_RMS_ERROR_DB = 25,
    HWA_PHYSICAL_RENDER_MAX_ERROR_DBFS = 26,
    HWA_PHYSICAL_RENDER_CORRELATION = 27,
    HWA_PHYSICAL_RENDER_LAG_SAMPLES = 28,
    HWA_PHYSICAL_RENDER_PITCH_DELTA_CENTS = 29,
    HWA_PHYSICAL_RENDER_ATTACK_DELTA_SECONDS = 30,
    HWA_PHYSICAL_RENDER_DECAY_DELTA_SECONDS = 31,
    HWA_PHYSICAL_RENDER_SPECTRAL_DISTANCE_DB = 32,
    HWA_PHYSICAL_DC_OFFSET = 33,
    HWA_PHYSICAL_DC_DRIFT = 34,
    HWA_PHYSICAL_CLIP_FRACTION = 35,
    HWA_PHYSICAL_HARD_BOUND_FRACTION = 36,
    HWA_PHYSICAL_REPEATED_BLOCK_FRACTION = 37,
    HWA_PHYSICAL_STUCK_STATE_FRACTION = 38,
    HWA_PHYSICAL_MAX_STEP_DBFS = 39,
    HWA_PHYSICAL_RUNAWAY_SLOPE_DB_PER_SECOND = 40,
    HWA_PHYSICAL_RETURN_LEVEL_DBFS = 41,
    HWA_PHYSICAL_HIGH_BAND_RATIO = 42,
    HWA_PHYSICAL_SUBHARMONIC_RATIO = 43,
    HWA_PHYSICAL_FIXED_TONE_PROMINENCE_DB = 44,
    HWA_PHYSICAL_DENORMAL_FRACTION = 45,
    HWA_PHYSICAL_CHECK_KIND_COUNT = 46
} HWAPhysicalCheckKind;

enum {
    HWA_PHYSICAL_EVIDENCE_REFERENCE_PROFILE = 1U << 0,
    HWA_PHYSICAL_EVIDENCE_MODEL_PROFILE = 1U << 1,
    HWA_PHYSICAL_EVIDENCE_ELEMENT_LABEL = 1U << 2,
    HWA_PHYSICAL_EVIDENCE_WAVE_SAMPLES = 1U << 3,
    HWA_PHYSICAL_EVIDENCE_SPECTRUM = 1U << 4,
    HWA_PHYSICAL_EVIDENCE_ENVELOPE = 1U << 5,
    HWA_PHYSICAL_EVIDENCE_BODY_RESPONSE = 1U << 6,
    HWA_PHYSICAL_EVIDENCE_JOINT_SUM = 1U << 7,
    HWA_PHYSICAL_EVIDENCE_RENDER_PAIR = 1U << 8,
    HWA_PHYSICAL_EVIDENCE_TARGET_PITCH = 1U << 9
};

enum {
    HWA_PHYSICAL_QUALITY_LOW_COVERAGE = 1U << 0,
    HWA_PHYSICAL_QUALITY_PITCH_CONFOUNDED = 1U << 1,
    HWA_PHYSICAL_QUALITY_ROOM_CONFOUNDED = 1U << 2,
    HWA_PHYSICAL_QUALITY_EXPECTED_BAND_LIMIT = 1U << 3,
    HWA_PHYSICAL_QUALITY_LOW_SIGNAL = 1U << 4,
    HWA_PHYSICAL_QUALITY_INCOMPATIBLE_CLOCK = 1U << 5,
    HWA_PHYSICAL_QUALITY_FALLBACK = 1U << 6
};

typedef struct HWAPhysicalInput {
    const char *role;
    const char *path;
} HWAPhysicalInput;

typedef struct HWAPhysicalOptions {
    size_t decode_block_frames;
    size_t fft_size;
    size_t hop_size;
    double spectral_floor_dbfs;
    uint64_t max_wave_bytes;
    uint64_t max_wave_frames;
    uint64_t max_work_bytes;
    uint64_t max_pair_evaluations;
    size_t max_bindings;
    size_t max_transforms;
    size_t max_modes;
    size_t max_checks;
    size_t max_findings;
    size_t max_warnings;
    HWAProfileComparisonOptions profile_limits;
} HWAPhysicalOptions;

typedef struct HWAPhysicalSource {
    uint64_t id;
    char *role;
    char *path;
    char sha256[HWA_SHA256_HEX_SIZE];
    HWAFormat format;
    int is_wave;
} HWAPhysicalSource;

typedef struct HWAPhysicalCheck {
    uint64_t id;
    char *scope;
    char *case_id;
    char *element;
    HWAPhysicalCheckKind kind;
    uint32_t index;
    HWAPhysicalUnit unit;
    HWAPhysicalAvailability availability;
    double reference_value;
    double model_value;
    double delta;
    double confidence;
    uint32_t evidence_flags;
    uint32_t quality_flags;
    int reference_valid;
    int model_valid;
    int delta_valid;
} HWAPhysicalCheck;

typedef struct HWAPhysicalFinding {
    uint64_t id;
    HWAPhysicalFindingClass finding_class;
    HWAPhysicalSeverity severity;
    char *code;
    char *message;
    uint64_t check_id;
    double score;
    size_t rank;
    int check_id_valid;
    int score_valid;
} HWAPhysicalFinding;

typedef struct HWAPhysicalWarning {
    uint64_t id;
    char *code;
    char *message;
    uint64_t source_id;
    uint64_t check_id;
    int source_id_valid;
    int check_id_valid;
} HWAPhysicalWarning;

typedef struct HWAPhysicalCheckSet {
    HWAPhysicalOptions options;
    char *reference_measures_path;
    char *model_measures_path;
    char reference_measures_sha256[HWA_SHA256_HEX_SIZE];
    char model_measures_sha256[HWA_SHA256_HEX_SIZE];
    uint64_t retained_work_bytes;
    uint64_t pair_evaluations;
    size_t transform_count;
    HWAPhysicalSource *sources;
    size_t source_count;
    HWAPhysicalCheck *checks;
    size_t check_count;
    HWAPhysicalFinding *findings;
    size_t finding_count;
    HWAPhysicalWarning *warnings;
    size_t warning_count;
} HWAPhysicalCheckSet;

typedef struct HWAProductionInputs {
    const char *reference_measures_path;
    const char *reference_audio_path;
    const char *model_measures_path;
    const char *model_audio_path;
    const char *room_ir_path;
} HWAProductionInputs;

typedef enum HWAProductionAvailability {
    HWA_PRODUCTION_AVAILABLE = 1,
    HWA_PRODUCTION_UNAVAILABLE = 2,
    HWA_PRODUCTION_INSUFFICIENT = 3,
    HWA_PRODUCTION_AVAILABILITY_COUNT = 4
} HWAProductionAvailability;

typedef enum HWAProductionSplit {
    HWA_PRODUCTION_TRAIN = 1,
    HWA_PRODUCTION_CHECK = 2,
    HWA_PRODUCTION_SPLIT_COUNT = 3
} HWAProductionSplit;

typedef enum HWAProductionView {
    HWA_PRODUCTION_VIEW_RAW = 1,
    HWA_PRODUCTION_VIEW_DRY_LIKE = 2,
    HWA_PRODUCTION_VIEW_ROOM_MATCHED = 3,
    HWA_PRODUCTION_VIEW_COUNT = 4
} HWAProductionView;

typedef enum HWAProductionScope {
    HWA_PRODUCTION_SCOPE_CORRECTION = 1,
    HWA_PRODUCTION_SCOPE_REFERENCE = 2,
    HWA_PRODUCTION_SCOPE_MODEL = 3,
    HWA_PRODUCTION_SCOPE_ROOM_IR = 4,
    HWA_PRODUCTION_SCOPE_COUNT = 5
} HWAProductionScope;

typedef enum HWAProductionUnit {
    HWA_PRODUCTION_UNIT_DBFS = 1,
    HWA_PRODUCTION_UNIT_DB = 2,
    HWA_PRODUCTION_UNIT_RATIO = 3,
    HWA_PRODUCTION_UNIT_SECONDS = 4,
    HWA_PRODUCTION_UNIT_SAMPLES = 5,
    HWA_PRODUCTION_UNIT_COUNT = 6
} HWAProductionUnit;

typedef enum HWAProductionFitKind {
    HWA_PRODUCTION_FIT_EQ_GAIN_DB = 1,
    HWA_PRODUCTION_FIT_THRESHOLD_DBFS = 2,
    HWA_PRODUCTION_FIT_RATIO = 3,
    HWA_PRODUCTION_FIT_MAKEUP_DB = 4,
    HWA_PRODUCTION_FIT_STEREO_DELAY_SAMPLES = 5,
    HWA_PRODUCTION_FIT_CHANNEL_POLARITY = 6,
    HWA_PRODUCTION_FIT_CHANNEL_BALANCE_DB = 7,
    HWA_PRODUCTION_FIT_STEREO_WIDTH_RATIO = 8,
    HWA_PRODUCTION_FIT_STEREO_CORRELATION = 9,
    HWA_PRODUCTION_FIT_EARLY_REFLECTION_DB = 10,
    HWA_PRODUCTION_FIT_LATE_DECAY_SECONDS = 11,
    HWA_PRODUCTION_FIT_KIND_COUNT = 12
} HWAProductionFitKind;

typedef enum HWAProductionMetricKind {
    HWA_PRODUCTION_METRIC_RMS_DBFS = 1,
    HWA_PRODUCTION_METRIC_CREST_DB = 2,
    HWA_PRODUCTION_METRIC_BAND_LEVEL_DBFS = 3,
    HWA_PRODUCTION_METRIC_LEVEL_SPREAD_DB = 4,
    HWA_PRODUCTION_METRIC_CHANNEL_BALANCE_DB = 5,
    HWA_PRODUCTION_METRIC_STEREO_WIDTH_RATIO = 6,
    HWA_PRODUCTION_METRIC_STEREO_CORRELATION = 7,
    HWA_PRODUCTION_METRIC_STEREO_DELAY_SAMPLES = 8,
    HWA_PRODUCTION_METRIC_EARLY_REFLECTION_DB = 9,
    HWA_PRODUCTION_METRIC_LATE_DECAY_SECONDS = 10,
    HWA_PRODUCTION_METRIC_KIND_COUNT = 11
} HWAProductionMetricKind;

/* Shared measurement facts required by the production profile pair. */
typedef struct HWAProductionProfileMethod {
    size_t fft_size;
    size_t hop_size;
    double pitch_confidence_floor;
    double spectral_floor_dbfs;
    size_t max_partials;
} HWAProductionProfileMethod;

enum {
    HWA_PRODUCTION_SPAN_EQ = 1U << 0,
    HWA_PRODUCTION_SPAN_DYNAMICS = 1U << 1,
    HWA_PRODUCTION_SPAN_STEREO = 1U << 2,
    HWA_PRODUCTION_SPAN_DECAY = 1U << 3
};

enum {
    HWA_PRODUCTION_EVIDENCE_PROFILE = 1U << 0,
    HWA_PRODUCTION_EVIDENCE_SAMPLES = 1U << 1,
    HWA_PRODUCTION_EVIDENCE_SPECTRUM = 1U << 2,
    HWA_PRODUCTION_EVIDENCE_ENVELOPE = 1U << 3,
    HWA_PRODUCTION_EVIDENCE_STEREO = 1U << 4,
    HWA_PRODUCTION_EVIDENCE_ROOM_IR = 1U << 5,
    HWA_PRODUCTION_EVIDENCE_HELD_OUT = 1U << 6
};

enum {
    HWA_PRODUCTION_QUALITY_LOW_COVERAGE = 1U << 0,
    HWA_PRODUCTION_QUALITY_SOURCE_CONFOUNDED = 1U << 1,
    HWA_PRODUCTION_QUALITY_FIT_AT_BOUND = 1U << 2,
    HWA_PRODUCTION_QUALITY_MONO = 1U << 3,
    HWA_PRODUCTION_QUALITY_IR_SUPPLIED = 1U << 4,
    HWA_PRODUCTION_QUALITY_CORRECTION_INCOMPLETE = 1U << 5
};

typedef struct HWAProductionOptions {
    size_t decode_block_frames;
    uint64_t max_input_bytes;
    uint64_t max_input_frames;
    uint64_t max_ir_frames;
    uint64_t max_work_bytes;
    uint64_t max_evaluations;
    size_t max_spans;
    size_t max_envelope_points;
    size_t max_fits;
    size_t max_evaluation_rows;
    size_t max_view_rows;
    size_t max_warnings;
    HWAProfileComparisonOptions profile_limits;
} HWAProductionOptions;

typedef struct HWAProductionSource {
    uint64_t id;
    char *role;
    char *path;
    char sha256[HWA_SHA256_HEX_SIZE];
    HWAFormat format;
    int is_wave;
} HWAProductionSource;

typedef struct HWAProductionSpan {
    uint64_t id;
    char *item_key;
    HWAItemKind item_kind;
    char *item_role;
    HWAProductionSplit split;
    uint64_t reference_item_id;
    uint64_t reference_start_sample;
    uint64_t reference_end_sample;
    uint64_t model_item_id;
    uint64_t model_start_sample;
    uint64_t model_end_sample;
    uint32_t eligibility_flags;
} HWAProductionSpan;

typedef struct HWAProductionFit {
    uint64_t id;
    HWAProductionScope scope;
    HWAProductionFitKind kind;
    uint32_t index;
    HWAProductionUnit unit;
    HWAProductionAvailability availability;
    double estimate;
    double q05;
    double q95;
    size_t span_count;
    size_t point_count;
    uint32_t quality_flags;
    int estimate_valid;
    int uncertainty_valid;
} HWAProductionFit;

typedef struct HWAProductionStatistics {
    size_t total_count;
    size_t valid_count;
    size_t missing_count;
    double minimum;
    double q05;
    double q25;
    double q50;
    double q75;
    double q95;
    double maximum;
    double mean;
    double population_sd;
    double confidence;
    int valid;
} HWAProductionStatistics;

typedef struct HWAProductionEvaluation {
    uint64_t id;
    uint64_t span_id;
    HWAProductionView view;
    HWAProductionMetricKind kind;
    uint32_t index;
    HWAProductionUnit unit;
    HWAProductionAvailability availability;
    double reference_value;
    double model_value;
    double delta;
    double confidence;
    uint32_t evidence_flags;
    uint32_t quality_flags;
    int reference_valid;
    int model_valid;
    int delta_valid;
} HWAProductionEvaluation;

typedef struct HWAProductionViewRow {
    uint64_t id;
    HWAProductionSplit split;
    HWAProductionView view;
    HWAProductionMetricKind kind;
    uint32_t index;
    HWAProductionUnit unit;
    HWAProductionAvailability availability;
    HWAProductionStatistics reference_statistics;
    HWAProductionStatistics model_statistics;
    double median_delta;
    double quantile_distance;
    double gap_score;
    double raw_gap_score;
    uint32_t quality_flags;
    int survives;
    int gap_valid;
} HWAProductionViewRow;

typedef struct HWAProductionWarning {
    uint64_t id;
    char *code;
    char *message;
    uint64_t span_id;
    uint64_t fit_id;
    int span_id_valid;
    int fit_id_valid;
} HWAProductionWarning;

typedef struct HWAProductionResult {
    HWAProductionOptions options;
    HWAProductionProfileMethod profile_method;
    uint64_t retained_work_bytes;
    uint64_t evaluation_count;
    HWAProductionSource *sources;
    size_t source_count;
    HWAProductionSpan *spans;
    size_t span_count;
    HWAProductionFit *fits;
    size_t fit_count;
    HWAProductionEvaluation *evaluations;
    size_t evaluation_row_count;
    HWAProductionViewRow *view_rows;
    size_t view_row_count;
    HWAProductionWarning *warnings;
    size_t warning_count;
} HWAProductionResult;

typedef struct HWARunBinding {
    const char *id;
    const char *path;
} HWARunBinding;

typedef enum HWARunAvailability {
    HWA_RUN_AVAILABLE = 1,
    HWA_RUN_UNAVAILABLE = 2,
    HWA_RUN_INSUFFICIENT = 3,
    HWA_RUN_AVAILABILITY_COUNT = 4
} HWARunAvailability;

typedef enum HWARunSourceKind {
    HWA_RUN_SOURCE_STEM = 1,
    HWA_RUN_SOURCE_PROBE = 2,
    HWA_RUN_SOURCE_KIND_COUNT = 3
} HWARunSourceKind;

typedef enum HWARunSide {
    HWA_RUN_REFERENCE = 1,
    HWA_RUN_MODEL = 2,
    HWA_RUN_SIDE_COUNT = 3
} HWARunSide;

typedef enum HWARunStemRole {
    HWA_RUN_STEM_SOURCE = 1,
    HWA_RUN_STEM_BODY = 2,
    HWA_RUN_STEM_WET = 3,
    HWA_RUN_STEM_FINAL = 4,
    HWA_RUN_STEM_ROOM = 5,
    HWA_RUN_STEM_NOISE = 6,
    HWA_RUN_STEM_ROLE_COUNT = 7
} HWARunStemRole;

typedef enum HWARunProbeFormat {
    HWA_RUN_PROBE_CSV_F64 = 1,
    HWA_RUN_PROBE_BINARY_F64LE = 2,
    HWA_RUN_PROBE_FORMAT_COUNT = 3
} HWARunProbeFormat;

typedef enum HWARunFeatureKind {
    HWA_RUN_FEATURE_RMS_DBFS = 1,
    HWA_RUN_FEATURE_CREST_DB = 2,
    HWA_RUN_FEATURE_BAND_LEVEL_DBFS = 3,
    HWA_RUN_FEATURE_KIND_COUNT = 4
} HWARunFeatureKind;

typedef enum HWARunUnit {
    HWA_RUN_UNIT_DBFS = 1,
    HWA_RUN_UNIT_DB = 2,
    HWA_RUN_UNIT_RATIO = 3,
    HWA_RUN_UNIT_SAMPLES = 4,
    HWA_RUN_UNIT_PPM = 5,
    HWA_RUN_UNIT_COUNT = 6
} HWARunUnit;

enum {
    HWA_RUN_QUALITY_CLOCK_OFFSET = 1U << 0,
    HWA_RUN_QUALITY_CLOCK_DRIFT = 1U << 1,
    HWA_RUN_QUALITY_LOW_OVERLAP = 1U << 2,
    HWA_RUN_QUALITY_LOW_VARIANCE = 1U << 3,
    HWA_RUN_QUALITY_STAGE_CONFOUNDED = 1U << 4
};

typedef struct HWARunOptions {
    size_t decode_block_frames;
    uint64_t max_manifest_bytes;
    uint64_t max_input_bytes;
    uint64_t max_input_frames;
    uint64_t max_probe_bytes;
    uint64_t max_probe_values;
    uint64_t max_work_bytes;
    uint64_t max_evaluations;
    size_t max_stems;
    size_t max_probes;
    size_t max_links;
    size_t max_json_depth;
    size_t max_json_tokens;
    size_t max_result_rows;
    size_t max_warnings;
} HWARunOptions;

typedef struct HWARunSource {
    uint64_t id;
    char *binding_id;
    char *path;
    char sha256[HWA_SHA256_HEX_SIZE];
    HWARunSourceKind kind;
    HWARunSide side;
    HWARunStemRole role;
    HWARunProbeFormat probe_format;
    char *probe_name;
    char *unit;
    HWAFormat format;
    int64_t start_sample;
    double gain_db;
    uint64_t rate_numerator;
    uint64_t rate_denominator;
    uint64_t file_bytes;
    uint64_t value_count;
} HWARunSource;

typedef struct HWARunClock {
    uint64_t id;
    HWARunStemRole role;
    uint64_t reference_source_id;
    uint64_t model_source_id;
    HWARunAvailability availability;
    int64_t start_offset_samples;
    int64_t end_offset_samples;
    int64_t drift_samples;
    uint64_t overlap_frames;
    double drift_ppm;
    uint32_t quality_flags;
} HWARunClock;

typedef struct HWARunFeature {
    uint64_t id;
    uint64_t clock_id;
    HWARunStemRole role;
    HWARunFeatureKind kind;
    uint32_t index;
    HWARunUnit unit;
    HWARunAvailability availability;
    double reference_value;
    double model_value;
    double delta;
    double normalized_gap;
    uint32_t quality_flags;
    int reference_valid;
    int model_valid;
    int delta_valid;
    int gap_valid;
} HWARunFeature;

typedef struct HWARunStage {
    uint64_t id;
    HWARunStemRole from_role;
    HWARunStemRole to_role;
    HWARunAvailability availability;
    double prior_gap;
    double current_gap;
    double added_gap;
    size_t rank;
    uint32_t quality_flags;
    int gap_valid;
} HWARunStage;

typedef struct HWARunProbe {
    uint64_t id;
    uint64_t source_id;
    HWARunAvailability availability;
    uint64_t value_count;
    double minimum;
    double maximum;
    double mean;
    double population_sd;
    int statistics_valid;
} HWARunProbe;

typedef struct HWARunLink {
    uint64_t id;
    uint64_t stem_source_id;
    uint64_t probe_source_id;
    HWARunFeatureKind feature;
    uint32_t feature_index;
    HWARunAvailability availability;
    int64_t lag_hops;
    int64_t lag_samples;
    double correlation;
    double slope;
    double intercept;
    double r_squared;
    size_t point_count;
    double coverage;
    uint32_t quality_flags;
    int fit_valid;
} HWARunLink;

typedef struct HWARunWarning {
    uint64_t id;
    char *code;
    char *message;
    uint64_t source_id;
    uint64_t clock_id;
    uint64_t stage_id;
    uint64_t link_id;
    int source_id_valid;
    int clock_id_valid;
    int stage_id_valid;
    int link_id_valid;
} HWARunWarning;

typedef struct HWARunResult {
    HWARunOptions options;
    char *manifest_path;
    char manifest_sha256[HWA_SHA256_HEX_SIZE];
    uint32_t clock_rate_hz;
    uint64_t retained_work_bytes;
    uint64_t evaluation_count;
    HWARunSource *sources;
    size_t source_count;
    HWARunClock *clocks;
    size_t clock_count;
    HWARunFeature *features;
    size_t feature_count;
    HWARunStage *stages;
    size_t stage_count;
    HWARunProbe *probes;
    size_t probe_count;
    HWARunLink *links;
    size_t link_count;
    HWARunWarning *warnings;
    size_t warning_count;
} HWARunResult;

typedef enum HWAExperimentPlanKind {
    HWA_EXPERIMENT_ONE_AT_A_TIME = 1,
    HWA_EXPERIMENT_GRID = 2,
    HWA_EXPERIMENT_RANDOM = 3,
    HWA_EXPERIMENT_PLAN_KIND_COUNT = 4
} HWAExperimentPlanKind;

typedef enum HWAExperimentSplit {
    HWA_EXPERIMENT_FIT = 1,
    HWA_EXPERIMENT_CHECK = 2,
    HWA_EXPERIMENT_SPLIT_COUNT = 3
} HWAExperimentSplit;

typedef enum HWAExperimentMonotonicity {
    HWA_EXPERIMENT_MONOTONIC_NONE = 1,
    HWA_EXPERIMENT_MONOTONIC_FLAT = 2,
    HWA_EXPERIMENT_MONOTONIC_INCREASING = 3,
    HWA_EXPERIMENT_MONOTONIC_DECREASING = 4,
    HWA_EXPERIMENT_MONOTONIC_MIXED = 5,
    HWA_EXPERIMENT_MONOTONICITY_COUNT = 6
} HWAExperimentMonotonicity;

enum {
    HWA_EXPERIMENT_QUALITY_FLAT = 1U << 0,
    HWA_EXPERIMENT_QUALITY_NONMONOTONIC = 1U << 1,
    HWA_EXPERIMENT_QUALITY_REPLICATE_NOISE = 1U << 2,
    HWA_EXPERIMENT_QUALITY_CHECK_HARM = 1U << 3,
    HWA_EXPERIMENT_QUALITY_RANDOM_LINEAR_ONLY = 1U << 4
};

typedef struct HWAExperimentOptions {
    HWARunOptions run;
    uint64_t max_manifest_bytes;
    uint64_t max_input_bytes;
    uint64_t max_work_bytes;
    uint64_t max_bundle_bytes;
    uint64_t max_output_file_bytes;
    uint64_t max_total_run_evaluations;
    uint64_t max_job_milliseconds;
    uint64_t max_total_milliseconds;
    size_t max_parameters;
    size_t max_levels;
    size_t max_cases;
    size_t max_responses;
    size_t max_points;
    size_t max_jobs;
    size_t max_replicates;
    size_t max_artifacts;
    size_t max_observations;
    size_t max_sensitivities;
    size_t max_interactions;
    size_t max_warnings;
} HWAExperimentOptions;

typedef struct HWAExperimentRenderParameter {
    const char *id;
    const char *unit;
    double value;
} HWAExperimentRenderParameter;

typedef struct HWAExperimentRenderInput {
    const char *resource_id;
    const char *binding_id;
    const char *path;
    const char *sha256;
    HWARunSourceKind kind;
    HWARunSide side;
    HWARunStemRole role;
    HWARunProbeFormat probe_format;
    const char *probe_name;
    const char *unit;
    int64_t start_sample;
    double gain_db;
    uint32_t rate_hz;
    uint16_t channels;
    uint64_t rate_numerator;
    uint64_t rate_denominator;
    uint64_t value_count;
} HWAExperimentRenderInput;

typedef struct HWAExperimentRenderOutput {
    const char *id;
    const char *path;
    HWARunSourceKind kind;
    HWARunSide side;
    HWARunStemRole role;
    HWARunProbeFormat probe_format;
    const char *probe_name;
    const char *unit;
    int64_t start_sample;
    double gain_db;
    uint32_t rate_hz;
    uint16_t channels;
    uint64_t rate_numerator;
    uint64_t rate_denominator;
    uint64_t value_count;
} HWAExperimentRenderOutput;

typedef struct HWAExperimentRenderRequest {
    uint64_t job_id;
    const char *job_key;
    const char *case_id;
    HWAExperimentSplit split;
    size_t replicate;
    uint64_t seed;
    const char *job_directory;
    const char *request_path;
    const char *stdout_path;
    const char *stderr_path;
    const HWAExperimentRenderParameter *parameters;
    size_t parameter_count;
    const HWAExperimentRenderInput *inputs;
    size_t input_count;
    const HWAExperimentRenderOutput *outputs;
    size_t output_count;
    uint64_t max_output_file_bytes;
    uint64_t max_output_bytes;
    uint64_t timeout_milliseconds;
} HWAExperimentRenderRequest;

typedef int (*HWAExperimentRenderFunction)(
    void *context,
    const HWAExperimentRenderRequest *request,
    char *error,
    size_t error_size);

typedef struct HWAExperimentRenderer {
    const char *id;
    const char *sha256;
    void *context;
    HWAExperimentRenderFunction render;
} HWAExperimentRenderer;

typedef struct HWAExperimentInput {
    uint64_t id;
    char *binding_id;
    char *path;
    char sha256[HWA_SHA256_HEX_SIZE];
    uint64_t file_bytes;
} HWAExperimentInput;

typedef struct HWAExperimentParameter {
    uint64_t id;
    char *name;
    char *unit;
    double minimum;
    double maximum;
    double baseline;
    size_t first_level;
    size_t level_count;
} HWAExperimentParameter;

typedef struct HWAExperimentLevel {
    uint64_t id;
    uint64_t parameter_id;
    double value;
} HWAExperimentLevel;

typedef struct HWAExperimentCase {
    uint64_t id;
    char *name;
    HWAExperimentSplit split;
    double weight;
} HWAExperimentCase;

typedef struct HWAExperimentResponse {
    uint64_t id;
    char *name;
    HWARunStemRole role;
    HWARunFeatureKind feature;
    uint32_t feature_index;
} HWAExperimentResponse;

typedef struct HWAExperimentPoint {
    uint64_t id;
    char key[HWA_SHA256_HEX_SIZE];
    int baseline;
} HWAExperimentPoint;

typedef struct HWAExperimentValue {
    uint64_t id;
    uint64_t point_id;
    uint64_t parameter_id;
    double value;
} HWAExperimentValue;

typedef struct HWAExperimentJob {
    uint64_t id;
    char key[HWA_SHA256_HEX_SIZE];
    uint64_t point_id;
    uint64_t case_id;
    size_t replicate;
    uint64_t seed;
    char *run_result_path;
    char run_manifest_sha256[HWA_SHA256_HEX_SIZE];
    char run_result_sha256[HWA_SHA256_HEX_SIZE];
    uint64_t output_bytes;
    uint64_t duration_milliseconds;
    uint64_t run_evaluations;
    int reused;
} HWAExperimentJob;

typedef struct HWAExperimentArtifact {
    uint64_t id;
    uint64_t job_id;
    char *resource_id;
    char *path;
    char sha256[HWA_SHA256_HEX_SIZE];
    uint64_t file_bytes;
    HWARunSourceKind kind;
} HWAExperimentArtifact;

typedef struct HWAExperimentObservation {
    uint64_t id;
    uint64_t job_id;
    uint64_t response_id;
    HWARunAvailability availability;
    double value;
    uint32_t quality_flags;
    int value_valid;
} HWAExperimentObservation;

typedef struct HWAExperimentCandidate {
    uint64_t id;
    uint64_t point_id;
    uint64_t response_id;
    HWAExperimentSplit split;
    HWARunAvailability availability;
    double mean_gap;
    double improvement;
    double worst_harm;
    size_t case_count;
    uint32_t quality_flags;
    int values_valid;
} HWAExperimentCandidate;

typedef struct HWAExperimentSensitivity {
    uint64_t id;
    uint64_t parameter_id;
    uint64_t response_id;
    HWAExperimentSplit split;
    HWARunAvailability availability;
    double slope;
    double pearson;
    double linear_r_squared;
    double effect_fraction;
    double response_range;
    double noise_sd;
    size_t point_count;
    HWAExperimentMonotonicity monotonicity;
    uint32_t quality_flags;
    int linear_valid;
    int effect_valid;
    int noise_valid;
} HWAExperimentSensitivity;

typedef struct HWAExperimentInteraction {
    uint64_t id;
    uint64_t left_parameter_id;
    uint64_t right_parameter_id;
    uint64_t response_id;
    HWAExperimentSplit split;
    HWARunAvailability availability;
    double effect_fraction;
    size_t point_count;
    int effect_valid;
} HWAExperimentInteraction;

typedef struct HWAExperimentWarning {
    uint64_t id;
    char *code;
    char *message;
    uint64_t job_id;
    uint64_t point_id;
    uint64_t parameter_id;
    uint64_t response_id;
    int job_id_valid;
    int point_id_valid;
    int parameter_id_valid;
    int response_id_valid;
} HWAExperimentWarning;

typedef struct HWAExperimentResult {
    HWAExperimentOptions options;
    HWAExperimentPlanKind plan_kind;
    uint64_t plan_seed;
    size_t plan_replicates;
    char *manifest_path;
    char manifest_sha256[HWA_SHA256_HEX_SIZE];
    char *output_directory;
    char *resume_directory;
    char *renderer_id;
    char renderer_sha256[HWA_SHA256_HEX_SIZE];
    uint64_t retained_work_bytes;
    uint64_t total_run_evaluations;
    uint64_t total_output_bytes;
    uint64_t total_duration_milliseconds;
    size_t rendered_job_count;
    size_t reused_job_count;
    HWAExperimentInput *inputs;
    size_t input_count;
    HWAExperimentParameter *parameters;
    size_t parameter_count;
    HWAExperimentLevel *levels;
    size_t level_count;
    HWAExperimentCase *cases;
    size_t case_count;
    HWAExperimentResponse *responses;
    size_t response_count;
    HWAExperimentPoint *points;
    size_t point_count;
    HWAExperimentValue *values;
    size_t value_count;
    HWAExperimentJob *jobs;
    size_t job_count;
    HWAExperimentArtifact *artifacts;
    size_t artifact_count;
    HWAExperimentObservation *observations;
    size_t observation_count;
    HWAExperimentCandidate *candidates;
    size_t candidate_count;
    HWAExperimentSensitivity *sensitivities;
    size_t sensitivity_count;
    HWAExperimentInteraction *interactions;
    size_t interaction_count;
    HWAExperimentWarning *warnings;
    size_t warning_count;
} HWAExperimentResult;

typedef enum HWAGapReportMode {
    HWA_GAP_REPORT_RANK = 1,
    HWA_GAP_REPORT_EXCERPTS = 2,
    HWA_GAP_REPORT_FULL = 3,
    HWA_GAP_REPORT_MODE_COUNT = 4
} HWAGapReportMode;

typedef enum HWAGapReportSourceKind {
    HWA_GAP_REPORT_SOURCE_MEASUREMENT = 1,
    HWA_GAP_REPORT_SOURCE_PRODUCTION = 2,
    HWA_GAP_REPORT_SOURCE_RUN = 3,
    HWA_GAP_REPORT_SOURCE_EXPERIMENT = 4,
    HWA_GAP_REPORT_SOURCE_WAVE = 5,
    HWA_GAP_REPORT_SOURCE_KIND_COUNT = 6
} HWAGapReportSourceKind;

typedef enum HWAGapReportAvailability {
    HWA_GAP_REPORT_AVAILABLE = 1,
    HWA_GAP_REPORT_UNAVAILABLE = 2,
    HWA_GAP_REPORT_INSUFFICIENT = 3,
    HWA_GAP_REPORT_EXCLUDED = 4,
    HWA_GAP_REPORT_AVAILABILITY_COUNT = 5
} HWAGapReportAvailability;

typedef enum HWAGapReportAxis {
    HWA_GAP_REPORT_AXIS_PITCH = 1,
    HWA_GAP_REPORT_AXIS_REGISTER = 2,
    HWA_GAP_REPORT_AXIS_DYNAMIC = 3,
    HWA_GAP_REPORT_AXIS_GESTURE = 4,
    HWA_GAP_REPORT_AXIS_PHYSICAL_ELEMENT = 5,
    HWA_GAP_REPORT_AXIS_SECTION = 6,
    HWA_GAP_REPORT_AXIS_COUNT = 7
} HWAGapReportAxis;

typedef enum HWAGapReportView {
    HWA_GAP_REPORT_VIEW_RAW = 1,
    HWA_GAP_REPORT_VIEW_BROAD_EQ_MATCHED = 2,
    HWA_GAP_REPORT_VIEW_ROOM_MATCHED = 3,
    HWA_GAP_REPORT_VIEW_STEM = 4,
    HWA_GAP_REPORT_VIEW_PROBE_LINKED = 5,
    HWA_GAP_REPORT_VIEW_COUNT = 6
} HWAGapReportView;

typedef enum HWAGapReportCandidateKind {
    HWA_GAP_REPORT_CANDIDATE_MEASUREMENT = 1,
    HWA_GAP_REPORT_CANDIDATE_PRODUCTION = 2,
    HWA_GAP_REPORT_CANDIDATE_RUN_FEATURE = 3,
    HWA_GAP_REPORT_CANDIDATE_RUN_STAGE = 4,
    HWA_GAP_REPORT_CANDIDATE_EXPERIMENT = 5,
    HWA_GAP_REPORT_CANDIDATE_KIND_COUNT = 6
} HWAGapReportCandidateKind;

enum {
    HWA_GAP_REPORT_QUALITY_LOW_CONFIDENCE = 1U << 0,
    HWA_GAP_REPORT_QUALITY_LINKED_SECONDARY = 1U << 1,
    HWA_GAP_REPORT_QUALITY_AUDIBILITY_UNAVAILABLE = 1U << 2,
    HWA_GAP_REPORT_QUALITY_OCCURRENCE_UNAVAILABLE = 1U << 3,
    HWA_GAP_REPORT_QUALITY_MISSING_LABEL = 1U << 4,
    HWA_GAP_REPORT_QUALITY_SOURCE_WARNING = 1U << 5
};

typedef struct HWAGapReportOptions {
    size_t decode_block_frames;
    uint64_t max_manifest_bytes;
    uint64_t max_input_bytes;
    uint64_t max_input_frames;
    uint64_t max_work_bytes;
    uint64_t max_evaluations;
    uint64_t max_output_file_bytes;
    uint64_t max_bundle_bytes;
    uint64_t max_excerpt_frames;
    uint64_t max_total_excerpt_frames;
    size_t max_sources;
    size_t max_labels;
    size_t max_candidates;
    size_t max_families;
    size_t max_groups;
    size_t max_cases;
    size_t max_excerpts;
    size_t max_warnings;
    size_t max_json_depth;
    size_t max_json_tokens;
    HWAProfileComparisonOptions measurement;
    HWAProductionOptions production;
    HWARunOptions run;
    HWAExperimentOptions experiment;
} HWAGapReportOptions;

typedef struct HWAGapReportSource {
    uint64_t id;
    char *name;
    HWAGapReportSourceKind kind;
    char *path;
    char sha256[HWA_SHA256_HEX_SIZE];
    uint64_t file_bytes;
    size_t candidate_count;
} HWAGapReportSource;

typedef struct HWAGapReportLabel {
    uint64_t id;
    uint64_t source_id;
    char *case_id;
    char *pitch;
    char *register_name;
    char *dynamic;
    char *gesture;
    char *physical_element;
    char *section;
} HWAGapReportLabel;

typedef struct HWAGapReportCandidate {
    uint64_t id;
    uint64_t source_id;
    uint64_t source_row;
    char *case_id;
    char *metric;
    char *family_key;
    HWAGapReportCandidateKind kind;
    HWAGapReportAvailability availability;
    double raw_value;
    double size_factor;
    double audibility_factor;
    double occurrence_factor;
    double confidence_factor;
    double score;
    uint64_t occurrence_count;
    uint64_t eligible_count;
    uint64_t linked_family_id;
    size_t rank;
    uint32_t quality_flags;
    int raw_value_valid;
    int size_valid;
    int audibility_valid;
    int occurrence_valid;
    int confidence_valid;
    int score_valid;
    int primary;
    char *reason;
} HWAGapReportCandidate;

typedef struct HWAGapReportFamily {
    uint64_t id;
    char *key;
    uint64_t primary_candidate_id;
    size_t member_count;
    size_t rank;
} HWAGapReportFamily;

typedef struct HWAGapReportGroup {
    uint64_t id;
    HWAGapReportAxis axis;
    char *value;
    size_t family_count;
    size_t candidate_count;
    size_t available_count;
    size_t missing_count;
    size_t excluded_count;
    double q05;
    double q25;
    double median;
    double q75;
    double q95;
    double spread;
    double confidence;
    int statistics_valid;
    int confidence_valid;
} HWAGapReportGroup;

typedef struct HWAGapReportCase {
    uint64_t id;
    uint64_t candidate_id;
    char *case_id;
    HWAGapReportAvailability availability;
    double value;
    double confidence;
    double score;
    int value_valid;
    int confidence_valid;
    int score_valid;
    char *reason;
} HWAGapReportCase;

typedef struct HWAGapReportExcerpt {
    uint64_t id;
    char *name;
    uint64_t candidate_source_id;
    uint64_t candidate_row;
    HWAGapReportView view;
    uint64_t reference_source_id;
    uint64_t model_source_id;
    uint64_t reference_start_sample;
    uint64_t model_start_sample;
    uint64_t frame_count;
    int make_x;
    HWAGapReportAvailability availability;
    char *reference_path;
    char *model_path;
    char *x_path;
    char reference_sha256[HWA_SHA256_HEX_SIZE];
    char model_sha256[HWA_SHA256_HEX_SIZE];
    char x_sha256[HWA_SHA256_HEX_SIZE];
    uint64_t reference_file_bytes;
    uint64_t model_file_bytes;
    uint64_t x_file_bytes;
    double reference_gain_db;
    double model_gain_db;
    int x_is_reference;
    char *reason;
} HWAGapReportExcerpt;

typedef struct HWAGapReportWarning {
    uint64_t id;
    char *code;
    char *message;
    uint64_t source_id;
    uint64_t candidate_id;
    uint64_t excerpt_id;
    int source_id_valid;
    int candidate_id_valid;
    int excerpt_id_valid;
} HWAGapReportWarning;

typedef struct HWAGapReportResult {
    HWAGapReportOptions options;
    HWAGapReportMode mode;
    char *manifest_path;
    char manifest_sha256[HWA_SHA256_HEX_SIZE];
    char *output_directory;
    char *title;
    char *audibility_method;
    uint64_t retained_work_bytes;
    uint64_t total_input_bytes;
    uint64_t total_output_bytes;
    uint64_t evaluation_count;
    HWAGapReportSource *sources;
    size_t source_count;
    HWAGapReportLabel *labels;
    size_t label_count;
    HWAGapReportCandidate *candidates;
    size_t candidate_count;
    HWAGapReportFamily *families;
    size_t family_count;
    HWAGapReportGroup *groups;
    size_t group_count;
    HWAGapReportCase *cases;
    size_t case_count;
    HWAGapReportExcerpt *excerpts;
    size_t excerpt_count;
    HWAGapReportWarning *warnings;
    size_t warning_count;
} HWAGapReportResult;

typedef enum HWAEventAudioKind {
    HWA_EVENT_SOURCE_RECORDING = 1,
    HWA_EVENT_DERIVED_AUDIO = 2,
    HWA_EVENT_INSTRUMENT_STEM = 3,
    HWA_EVENT_AUDIO_KIND_COUNT = 4
} HWAEventAudioKind;

typedef enum HWAEventValueKind {
    HWA_EVENT_VALUE_TEXT = 1,
    HWA_EVENT_VALUE_F64 = 2,
    HWA_EVENT_VALUE_I64 = 3,
    HWA_EVENT_VALUE_BOOL = 4,
    HWA_EVENT_VALUE_KIND_COUNT = 5
} HWAEventValueKind;

typedef enum HWAEventValueBasis {
    HWA_EVENT_OBSERVATION = 1,
    HWA_EVENT_INFERENCE = 2,
    HWA_EVENT_SCORE_VALUE = 3,
    HWA_EVENT_VALUE_BASIS_COUNT = 4
} HWAEventValueBasis;

typedef enum HWAEventTraceFormat {
    HWA_EVENT_TRACE_CSV_F64 = 1,
    HWA_EVENT_TRACE_F64LE = 2,
    HWA_EVENT_TRACE_FORMAT_COUNT = 3
} HWAEventTraceFormat;

typedef struct HWAEventBundleLimits {
    uint64_t max_manifest_bytes;
    uint64_t max_index_bytes;
    uint64_t max_payload_file_bytes;
    uint64_t max_bundle_bytes;
    uint64_t max_work_bytes;
    size_t max_audio_files;
    size_t max_events;
    size_t max_values;
    size_t max_traces;
    size_t max_trace_refs;
    size_t max_providers;
    size_t max_warnings;
    size_t max_nesting_depth;
    size_t max_json_depth;
    size_t max_json_tokens;
} HWAEventBundleLimits;

typedef struct HWAEventProvider {
    uint64_t id;
    char *name;
    char *version;
    char model_sha256[HWA_SHA256_HEX_SIZE];
    char *settings_json;
} HWAEventProvider;

typedef struct HWAEventAudio {
    uint64_t id;
    HWAEventAudioKind kind;
    char *name;
    char *relative_path;
    char *path_hint;
    char sha256[HWA_SHA256_HEX_SIZE];
    uint64_t file_bytes;
    HWAFormat format;
    uint64_t source_recording_id;
    int source_recording_id_valid;
} HWAEventAudio;

typedef struct HWAEventValue {
    char *name;
    HWAEventValueKind kind;
    HWAEventValueBasis basis;
    char *text;
    double number;
    int64_t integer;
    int boolean;
    char *unit;
    double score;
    uint64_t provider_id;
    int score_valid;
    int provider_id_valid;
    int selected;
} HWAEventValue;

typedef struct HWAEventTrace {
    uint64_t id;
    char *name;
    char *unit;
    char *relative_path;
    char sha256[HWA_SHA256_HEX_SIZE];
    HWAEventTraceFormat format;
    uint64_t source_recording_id;
    uint64_t first_sample;
    uint64_t hop_samples;
    uint64_t window_samples;
    uint64_t point_count;
    uint32_t value_width;
    uint64_t file_bytes;
} HWAEventTrace;

typedef struct HWAEventTraceRef {
    uint64_t trace_id;
    char *role;
    uint64_t first_point;
    uint64_t point_count;
} HWAEventTraceRef;

typedef struct HWAPerformanceEvent {
    uint64_t id;
    char *kind;
    uint64_t source_recording_id;
    uint64_t evidence_audio_id;
    uint64_t parent_id;
    uint64_t start_sample;
    uint64_t end_sample;
    char *voice;
    char *part;
    char *score_event_id;
    int evidence_audio_id_valid;
    int parent_id_valid;
    HWAEventValue *values;
    size_t value_count;
    HWAEventTraceRef *trace_refs;
    size_t trace_ref_count;
} HWAPerformanceEvent;

typedef struct HWAEventWarning {
    uint64_t id;
    char *code;
    char *message;
    uint64_t event_id;
    int event_id_valid;
} HWAEventWarning;

typedef struct HWAEventBundle {
    char *directory;
    char manifest_sha256[HWA_SHA256_HEX_SIZE];
    uint64_t total_file_bytes;
    uint64_t retained_work_bytes;
    HWAEventProvider *providers;
    size_t provider_count;
    HWAEventAudio *audio;
    size_t audio_count;
    HWAEventTrace *traces;
    size_t trace_count;
    HWAPerformanceEvent *events;
    size_t event_count;
    HWAEventWarning *warnings;
    size_t warning_count;
} HWAEventBundle;

typedef struct HWAEventFileBinding {
    const char *relative_path;
    const char *source_path;
} HWAEventFileBinding;

void hwa_analysis_options_default(HWAAnalysisOptions *options);

/*
 * Analyze one caller-owned byte source without opening a path. The source name
 * is copied into the result as provenance and is never opened. The call
 * initializes every analysis field, including on failure.
 */
int hwa_analyze_wav_source(const HWAByteSource *source,
                           const HWAAnalysisOptions *options,
                           HWAAnalysis *analysis,
                           char *error,
                           size_t error_size);

/*
 * The first analysis call initializes every field in `analysis`; callers need
 * not clear it first. Call hwa_analysis_free() before reusing a successful
 * result. Options are copied before the result is initialized, so they may
 * point to `analysis->options` on a fresh result.
 */
int hwa_analyze_wav_with_options(const char *path,
                                 const HWAAnalysisOptions *options,
                                 HWAAnalysis *analysis,
                                 char *error,
                                 size_t error_size);

int hwa_analyze_wav(const char *path,
                    HWAAnalysis *analysis,
                    char *error,
                    size_t error_size);

void hwa_analysis_free(HWAAnalysis *analysis);

void hwa_isolated_note_options_default(HWAIsolatedNoteOptions *options);

/*
 * Check one isolated, pitched WAVE note against an exact expected frequency.
 * The requested and valid masks let callers require pitch, passive decay, or
 * both without treating a rejected estimate as an input or I/O failure.
 * The call initializes every result field, including on failure.
 */
int hwa_analyze_isolated_note_wav(const char *path,
                                  const HWAIsolatedNoteOptions *options,
                                  HWAIsolatedNoteResult *result,
                                  char *error,
                                  size_t error_size);

void hwa_isolated_note_result_free(HWAIsolatedNoteResult *result);

void hwa_harmonic_decay_options_default(HWAHarmonicDecayOptions *options);

/*
 * Estimate late per-harmonic T60 values from one isolated note. When
 * model_path is non-null, compare matching harmonic numbers. The method keeps
 * channel power separate, fixes all DSP choices in the method version, and
 * initializes every result field, including on failure.
 */
int hwa_harmonic_decay_wavs(const char *reference_path,
                            const char *model_path,
                            const HWAHarmonicDecayOptions *options,
                            HWAHarmonicDecayResult *result,
                            char *error,
                            size_t error_size);

void hwa_harmonic_decay_result_free(HWAHarmonicDecayResult *result);

void hwa_body_envelope_options_default(HWABodyEnvelopeOptions *options);

/*
 * Estimate a pitch-conditioned radiated envelope from one WAVE, or compare
 * two WAVE files when model_path is non-null. The result describes spectral
 * shape only: room, microphone, strings, and playing remain mixed into it.
 * The call initializes every result field, including on failure.
 */
int hwa_body_envelope_wavs(const char *reference_path,
                           const char *model_path,
                           const HWABodyEnvelopeOptions *options,
                           HWABodyEnvelopeResult *result,
                           char *error,
                           size_t error_size);

void hwa_body_envelope_result_free(HWABodyEnvelopeResult *result);

void hwa_alignment_options_default(HWAAlignmentOptions *options);

/*
 * Each alignment call initializes every field in `alignment`, including on
 * failure when `alignment` is non-null. Call hwa_alignment_free() before
 * reusing a successful result. Options are copied before the result is
 * initialized, so they may point to `alignment->options` on a fresh result.
 * Locked anchors remain caller-owned; only their two time fields constrain
 * the path. Score beats in the returned anchors come from the score track.
 */
int hwa_align_audio_wav(const char *reference_path,
                        const char *target_path,
                        const HWAAlignmentOptions *options,
                        const HWAAlignmentAnchor *locked_anchors,
                        size_t locked_anchor_count,
                        HWAAlignment *alignment,
                        char *error,
                        size_t error_size);

int hwa_align_score_manifest_wav(const char *score_path,
                                 const char *audio_path,
                                 const HWAAlignmentOptions *options,
                                 const HWAAlignmentAnchor *locked_anchors,
                                 size_t locked_anchor_count,
                                 HWAAlignment *alignment,
                                 char *error,
                                 size_t error_size);

void hwa_alignment_free(HWAAlignment *alignment);

void hwa_segmentation_options_default(HWASegmentationOptions *options);

/*
 * Segmentation accepts only a canonical score-to-audio alignment and an
 * explicit named WAVE input whose SHA-256 matches that alignment. The first
 * call initializes every field in `items`, including on failure. Options are
 * copied before the result is initialized. Labels, edits, and their strings
 * remain caller-owned; a successful result owns all nested arrays and strings.
 * Pass either a named amendment with no raw edits, or raw in-memory edits with
 * a null amendment path. The named form validates and loads its own edits.
 */
int hwa_segment_score_alignment_wav(
    const char *alignment_path,
    const char *audio_path,
    const char *labels_path,
    const char *amendment_path,
    const HWASegmentationOptions *options,
    const HWAItemEdit *edits,
    size_t edit_count,
    HWAItemSet *items,
    char *error,
    size_t error_size);

void hwa_item_set_free(HWAItemSet *items);

void hwa_measurement_options_default(HWAMeasurementOptions *options);

/*
 * Measurement accepts a canonical item file and an explicit named WAVE input.
 * Its SHA-256, duration, sample rate, and frame count must match the item
 * file. The call never opens a path stored in the item file. It initializes
 * every field in `result`, including on failure. Options are copied before
 * that initialization. A successful result owns all nested arrays and
 * strings.
 */
int hwa_measure_item_file_wav(const char *items_path,
                              const char *audio_path,
                              const HWAMeasurementOptions *options,
                              HWAMeasurementSet *result,
                              char *error,
                              size_t error_size);

void hwa_measurement_set_free(HWAMeasurementSet *result);

void hwa_profile_comparison_options_default(
    HWAProfileComparisonOptions *options);

/*
 * Comparison reads only the two explicit canonical measurement files. It
 * never opens their stored item, audio, alignment, label, amendment, or score
 * paths. The call initializes every field in `result`, including on failure.
 * A successful result owns all nested arrays and strings.
 */
int hwa_compare_measure_files(
    const char *reference_path,
    const char *model_path,
    const HWAProfileComparisonOptions *options,
    HWAProfileComparisonSet *result,
    char *error,
    size_t error_size);

void hwa_profile_comparison_set_free(HWAProfileComparisonSet *result);

void hwa_physical_options_default(HWAPhysicalOptions *options);

/*
 * Physical checks compare two explicit canonical measurement profiles.
 * Optional raw WAVE evidence remains caller-authorized through explicit
 * role/path pairs. No path stored inside a profile is opened. A role has the
 * exact form
 * `side:kind:case`, where side is `reference` or `model` and kind is one of
 * `body`, `joint`, `isolated-a`, `isolated-b`, `render-baseline`,
 * `render-variant`, or `scan`. The call initializes every result field,
 * including on failure. Inputs remain caller-owned; success owns every path,
 * role, scope, element, finding, warning, and result array. The call copies
 * options before it initializes result, so options may point to
 * result->options when result is a fresh or freed object.
 */
int hwa_check_physical_files(
    const char *reference_measures_path,
    const char *model_measures_path,
    const HWAPhysicalInput *inputs,
    size_t input_count,
    const HWAPhysicalOptions *options,
    HWAPhysicalCheckSet *result,
    char *error,
    size_t error_size);

void hwa_physical_check_set_free(HWAPhysicalCheckSet *result);

void hwa_production_options_default(HWAProductionOptions *options);

/*
 * Production accounting reads two explicit canonical measurement profiles
 * and their two explicit WAVE files. An optional explicit room impulse
 * remains caller evidence. No path stored in a profile or saved result is
 * opened. The method chooses and saves its fixed train/check split, fits only
 * train spans, and evaluates both
 * splits without writing corrected audio. It initializes every result field,
 * including on failure. Inputs remain caller-owned; success owns all result
 * strings and arrays. Options are copied before result initialization.
 */
int hwa_account_production_files(
    const HWAProductionInputs *inputs,
    const HWAProductionOptions *options,
    HWAProductionResult *result,
    char *error,
    size_t error_size);

void hwa_production_result_free(HWAProductionResult *result);

void hwa_run_options_default(HWARunOptions *options);

/*
 * Run analysis reads one explicit JSON manifest and one explicit binding
 * for every declared stem and probe. The manifest records expected SHA-256
 * values but no paths. The call opens no path retained by another artifact.
 * It initializes every result field, including on failure. Bindings and
 * options remain caller-owned; success owns all result strings and arrays.
 */
int hwa_analyze_run_files(const char *manifest_path,
                          const HWARunBinding *bindings,
                          size_t binding_count,
                          const HWARunOptions *options,
                          HWARunResult *result,
                          char *error,
                          size_t error_size);

void hwa_run_result_free(HWARunResult *result);

void hwa_experiment_options_default(HWAExperimentOptions *options);

/*
 * Experiment execution reads one explicit manifest and one explicit binding
 * for every fixed input. Passing a renderer adapter gives the caller authority
 * to render. The module creates a new output directory, gives each job a
 * private child directory, and runs analysis for every completed job. It
 * commits the bundle only after all checks pass. An optional resume directory
 * remains caller-authorized and read-only. The call initializes every result
 * field, including on failure. Bindings, renderer fields, and options remain
 * caller-owned; success owns all result strings and arrays.
 *
 * A renderer callback is synchronous. Request strings and arrays remain
 * borrowed and read-only. Before it returns, the callback must close every
 * declared output and stop every worker it started. It must create exactly
 * the declared outputs. replicate is zero-based. max_output_file_bytes caps
 * each file. max_output_bytes caps all files in the private job directory,
 * including request and log files.
 */
int hwa_execute_experiment_files(
    const char *manifest_path,
    const HWARunBinding *bindings,
    size_t binding_count,
    const char *output_directory,
    const char *resume_directory,
    const HWAExperimentRenderer *renderer,
    const HWAExperimentOptions *options,
    HWAExperimentResult *result,
    char *error,
    size_t error_size);

void hwa_experiment_result_free(HWAExperimentResult *result);

void hwa_gap_report_options_default(HWAGapReportOptions *options);

/*
 * Gap reporting reads one strict, path-free manifest and one binding
 * for each declared source. It never opens a path retained by a saved source.
 * Rank mode creates no output and requires a null output directory. Excerpt
 * and full modes require a new output directory and publish it only after all
 * source, limit, clip, report, and inventory checks pass. The call initializes
 * every result field, including on failure. Bindings and options remain
 * caller-owned; success owns all result strings and arrays.
 */
int hwa_build_gap_report_files(
    const char *manifest_path,
    const HWARunBinding *bindings,
    size_t binding_count,
    const char *output_directory,
    HWAGapReportMode mode,
    const HWAGapReportOptions *options,
    HWAGapReportResult *result,
    char *error,
    size_t error_size);

void hwa_gap_report_result_free(HWAGapReportResult *result);

void hwa_event_bundle_limits_default(HWAEventBundleLimits *limits);

/* Validate one caller-owned event bundle without reading or writing files. */
int hwa_event_bundle_validate(const HWAEventBundle *bundle,
                              const HWAEventBundleLimits *limits,
                              char *error,
                              size_t error_size);

/*
 * Read and validate one complete hwa-events directory. Stored path hints stay
 * inert. Success returns owned metadata while dense trace and audio payloads
 * remain on disk. The call initializes every result field, including on
 * failure. Free a successful result before reusing it.
 */
int hwa_event_bundle_read(const char *directory,
                          const HWAEventBundleLimits *limits,
                          HWAEventBundle *bundle,
                          char *error,
                          size_t error_size);

/*
 * Write and validate one new hwa-events directory. Every bundled payload must
 * have one explicit relative-path/source-path binding. Existing output is
 * never replaced. The input bundle and bindings remain caller-owned.
 */
int hwa_event_bundle_write(const char *output_directory,
                           const HWAEventBundle *bundle,
                           const HWAEventFileBinding *bindings,
                           size_t binding_count,
                           const HWAEventBundleLimits *limits,
                           char *error,
                           size_t error_size);

void hwa_event_bundle_free(HWAEventBundle *bundle);

const char *hwa_container_name(HWAContainer container);
const char *hwa_encoding_name(HWAEncoding encoding);
const char *hwa_band_name(size_t band);

#ifdef __cplusplus
}
#endif

#endif

#ifndef HWA_MEASURE_ENGINE_H
#define HWA_MEASURE_ENGINE_H

#include "hlolli_wg_analyzer.h"

#include <stddef.h>
#include <stdint.h>

#define HWA_MEASURE_ATTACK_SHAPE_BINS 16U

/* Borrowed values from the same FFT frames used for scalar measurements.
 * Called only for included items whose span contains the frame center.
 * A nonzero return aborts analysis. Sink storage counts as retained input.
 */
typedef int (*HWAMeasureFrameSink)(void *context, size_t item_index,
    uint64_t start_sample, double level_dbfs, double centroid_hz,
    double flatness, char *error, size_t error_size);

int hwa_measure_engine_wav_frames(const HWAItemSet *items,
    const char *explicit_audio_path, const HWAMeasurementOptions *options,
    uint64_t retained_input_bytes, HWAMeasurementSet *result,
    HWAMeasureFrameSink sink, void *context, char *error, size_t error_size);

/*
 * Run the Stage 4 scalar engine on an already checked Stage 3 item set. The
 * engine reads only explicit_audio_path. It fills audio facts, item contexts,
 * observations, the level reference, and its own work counts. Groups and
 * statistics are built by hwa_measure_build_profile(). retained_input_bytes
 * lets the caller place the full item reader and engine under one live-work
 * cap.
 */
int hwa_measure_engine_wav(const HWAItemSet *items,
                           const char *explicit_audio_path,
                           const HWAMeasurementOptions *options,
                           uint64_t retained_input_bytes,
                           HWAMeasurementSet *result,
                           char *error,
                           size_t error_size);

/* A fixed sample path used by the synthetic proof tests. Samples are mono. */
int hwa_measure_engine_samples(const HWAItemSet *items,
                               const double *samples,
                               uint64_t frame_count,
                               uint32_t sample_rate_hz,
                               const HWAMeasurementOptions *options,
                               uint64_t retained_input_bytes,
                               HWAMeasurementSet *result,
                               char *error,
                               size_t error_size);

#endif

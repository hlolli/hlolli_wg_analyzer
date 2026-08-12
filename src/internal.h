#ifndef HWA_INTERNAL_H
#define HWA_INTERNAL_H

#include "hlolli_wg_analyzer.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define HWA_MAX_CHANNELS 1024U

typedef struct HWAWavReader {
    FILE *file;
    HWAByteSource source;
    uint64_t cursor;
    void (*close_context)(void *context);
    HWAFormat format;
    uint64_t data_offset;
    uint64_t bytes_remaining;
    size_t bytes_per_sample;
} HWAWavReader;

int hwa_wav_reader_open_source(HWAWavReader *reader,
                               const HWAByteSource *source,
                               uint64_t max_input_bytes,
                               char *error,
                               size_t error_size);

int hwa_wav_reader_open(HWAWavReader *reader,
                        const char *path,
                        uint64_t max_input_bytes,
                        char *error,
                        size_t error_size);

/* Takes ownership of an already-open, read-only regular-file stream. */
int hwa_wav_reader_open_file(HWAWavReader *reader,
                             FILE *file,
                             uint64_t max_input_bytes,
                             char *error,
                             size_t error_size);

int hwa_wav_reader_read_frames(HWAWavReader *reader,
                               unsigned char *buffer,
                               size_t frame_capacity,
                               size_t *frames_read,
                               char *error,
                               size_t error_size);

double hwa_wav_decode_sample(const HWAWavReader *reader,
                             const unsigned char *sample,
                             int *clipped);

void hwa_wav_reader_close(HWAWavReader *reader);

int hwa_analysis_options_validate(const HWAAnalysisOptions *options,
                                  char *error,
                                  size_t error_size);

int hwa_analyze_wav_reader(HWAWavReader *reader,
                           const char *name,
                           const HWAAnalysisOptions *options,
                           HWAAnalysis *analysis,
                           char *error,
                           size_t error_size);

#if defined(__clang__) || defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
void hwa_set_error(char *error, size_t error_size, const char *format, ...);

#endif

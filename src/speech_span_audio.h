#ifndef HWA_SPEECH_SPAN_AUDIO_H
#define HWA_SPEECH_SPAN_AUDIO_H

#include "paired_spectrum_spans.h"
#include "speech_span_dsp.h"

typedef struct HWASpeechAudioLevel {
    double rms;
    double peak;
} HWASpeechAudioLevel;

int hwa_speech_audio_level(const HWAWavReader *reader, HwaPairedSampleSpan span,
                            HWASpeechWork *work, HWASpeechAudioLevel *level,
                            char *error, size_t error_size);

int hwa_speech_audio_mono(const HWAWavReader *reader, HwaPairedSampleSpan span,
                           HWASpeechWork *work, double *samples, char *error,
                           size_t error_size);

int hwa_speech_audio_write(FILE *stream, const HWAWavReader *reader, HwaPairedSampleSpan span,
                            double gain, HWASpeechWork *work, char *error, size_t error_size);

#endif

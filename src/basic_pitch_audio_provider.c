#include "basic_pitch_audio_provider.h"

#include "basic_pitch_provider.h"
#include "inference_clock.h"
#include "internal.h"

#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HWA_BP_AUDIO_INPUT_RATE UINT32_C(44100)
#define HWA_BP_AUDIO_OUTPUT_RATE UINT32_C(22050)
#define HWA_BP_AUDIO_CHANNELS UINT16_C(2)
#define HWA_BP_AUDIO_BITS UINT16_C(32)
#define HWA_BP_AUDIO_INPUT_BLOCK_ALIGN UINT16_C(8)
#define HWA_BP_AUDIO_OUTPUT_BLOCK_ALIGN UINT16_C(4)
#define HWA_BP_AUDIO_FIR_TAPS 127U
#define HWA_BP_AUDIO_FIR_HALF 63U
#define HWA_BP_AUDIO_IO_FRAMES 4096U
#define HWA_BP_AUDIO_WAVE_HEADER_BYTES UINT64_C(44)
#define HWA_BP_AUDIO_PROVIDER_ID UINT64_C(1)
#define HWA_BP_AUDIO_SETTINGS_MAX 131072U
#define HWA_BP_AUDIO_TEXT_LIMIT 4096U
#define HWA_BP_AUDIO_WRAPPER_RESERVE \
    ((uint64_t)HWA_BP_AUDIO_SETTINGS_MAX + \
     (uint64_t)HWA_BP_AUDIO_TEXT_LIMIT + UINT64_C(1024))
#define HWA_BP_AUDIO_PREPARED_NAME "basic-pitch-mono-22050.wav"
#define HWA_BP_AUDIO_PREPARED_INPUT_ID "prepared-source"

typedef struct HWABasicPitchAudioContext {
    HWAInferenceProvider child;
    char model_sha256[HWA_SHA256_HEX_SIZE];
    char adapter_sha256[HWA_SHA256_HEX_SIZE];
    uint64_t max_work_bytes;
} HWABasicPitchAudioContext;

typedef struct HWABasicPitchAudioTask {
    HWAInferenceOutput output;
    HWAEventBundle bundle;
    HWAInferenceProvider child;
    void *child_task;
    HWAInferenceRequest child_request;
    HWAInferenceInput child_input;
    HWAWavReader prepared_reader;
    const HWAInferenceRequest *request;
    char prepared_sha256[HWA_SHA256_HEX_SIZE];
    uint64_t deadline_started;
    uint64_t max_work_bytes;
    int prepared_reader_open;
    int ready;
} HWABasicPitchAudioTask;

typedef struct HWABasicPitchAudioDecoder {
    HWAWavReader *reader;
    unsigned char *raw;
    size_t raw_capacity;
    size_t raw_count;
    size_t raw_index;
    uint64_t decoded_frames;
    uint64_t total_frames;
} HWABasicPitchAudioDecoder;

typedef struct HWABasicPitchAudioJson {
    char *text;
    size_t length;
    size_t capacity;
    size_t maximum;
} HWABasicPitchAudioJson;

/*
 * These binary64 coefficients are the fixed filter named by
 * HWA_BASIC_PITCH_AUDIO_TRANSFORM_NAME. They were formed from
 *   h[k] = sin(2*pi*0.225*k)/(pi*k), h[0] = 0.45,
 * multiplied by a 127-point Blackman window, mirrored exactly, then divided
 * by their binary64 sum. Keeping the rounded coefficients here makes the
 * adapter independent of the host's trigonometric library.
 */
static const double hwa_bp_audio_fir[HWA_BP_AUDIO_FIR_TAPS] = {
    -6.2475591511437281e-20, -3.5537857764245635e-07,
    -4.6303638502489774e-06, 7.3885607545267806e-20,
    1.9352896755700896e-05, 9.6990802030193067e-06,
    -4.1361652873465162e-05, -3.8212186085684384e-05,
    6.1879673829577187e-05, 9.2490659100827617e-05,
    -6.6236948233797411e-05, -0.00017378714807902239,
    3.5254473473766729e-05, 0.00027438917793276622,
    5.1694550341539075e-05, -0.00037495987982861953,
    -0.00021180530473917481, 0.00044343232531450212,
    0.00045242573488839168, -0.0004364871993462422,
    -0.00076394204227866536, 0.00030445718334406199,
    0.0011133662743582991, -2.8316186394115396e-18,
    -0.0014404362339239833, -0.00050989891987996121,
    0.0016581300783157955, 0.0012299701094622179,
    -0.0016591563257958195, -0.0021230228512436251,
    0.001329174746763773, 0.0030995257138512352,
    -0.00056631685481920791, -0.0040134451604907015,
    -0.00069479892777437853, 0.0046669001571105206,
    0.0024576416637414108, -0.0048250632496133873,
    -0.0046408815386572129, 0.0042410870642069672,
    0.0070621447696921349, -0.0026889152848482852,
    -0.009431721773539764, 1.1548971399213448e-17,
    0.01135766122592512, 0.0039014256565189313,
    -0.012360574981316179, -0.0089704461989427251,
    0.01189173419998178, 0.015026429127509773,
    -0.0093401360003000731, -0.021754883850538961,
    0.0039977924081366131, 0.028727906687434673,
    0.0050921263813805779, -0.03544205734916419,
    -0.019635336255642271, 0.041369630574874347,
    0.043881220647857259, -0.046016886149802289,
    -0.093674690692193377, 0.048981336114745963,
    0.31407033731340678, 0.44999966006384096,
    0.31407033731340678, 0.048981336114745963,
    -0.093674690692193377, -0.046016886149802289,
    0.043881220647857259, 0.041369630574874347,
    -0.019635336255642271, -0.03544205734916419,
    0.0050921263813805779, 0.028727906687434673,
    0.0039977924081366131, -0.021754883850538961,
    -0.0093401360003000731, 0.015026429127509773,
    0.01189173419998178, -0.0089704461989427251,
    -0.012360574981316179, 0.0039014256565189313,
    0.01135766122592512, 1.1548971399213448e-17,
    -0.009431721773539764, -0.0026889152848482852,
    0.0070621447696921349, 0.0042410870642069672,
    -0.0046408815386572129, -0.0048250632496133873,
    0.0024576416637414108, 0.0046669001571105206,
    -0.00069479892777437853, -0.0040134451604907015,
    -0.00056631685481920791, 0.0030995257138512352,
    0.001329174746763773, -0.0021230228512436251,
    -0.0016591563257958195, 0.0012299701094622179,
    0.0016581300783157955, -0.00050989891987996121,
    -0.0014404362339239833, -2.8316186394115396e-18,
    0.0011133662743582991, 0.00030445718334406199,
    -0.00076394204227866536, -0.0004364871993462422,
    0.00045242573488839168, 0.00044343232531450212,
    -0.00021180530473917481, -0.00037495987982861953,
    5.1694550341539075e-05, 0.00027438917793276622,
    3.5254473473766729e-05, -0.00017378714807902239,
    -6.6236948233797411e-05, 9.2490659100827617e-05,
    6.1879673829577187e-05, -3.8212186085684384e-05,
    -4.1361652873465162e-05, 9.6990802030193067e-06,
    1.9352896755700896e-05, 7.3885607545267806e-20,
    -4.6303638502489774e-06, -3.5537857764245635e-07,
    -6.2475591511437281e-20
};

static int hwa_bp_audio_start(void *context_value,
                              const HWAInferenceRequest *request,
                              void **task_result,
                              char *error,
                              size_t error_size);
static int hwa_bp_audio_poll(void *context_value,
                             void *task_value,
                             HWAInferencePollState *state,
                             const HWAInferenceOutput **output,
                             char *error,
                             size_t error_size);
static void hwa_bp_audio_task_free(void *context_value, void *task_value);
static void hwa_bp_audio_destroy(void *context_value);

static void hwa_bp_audio_error(char *error,
                               size_t error_size,
                               const char *message)
{
    if (error == NULL || error_size == 0U) return;
    (void)snprintf(error, error_size, "%s", message);
    error[error_size - 1U] = '\0';
}

static int hwa_bp_audio_lower_sha256(const char *text)
{
    size_t text_index;
    if (text == NULL) return 0;
    for (text_index = 0U; text_index < 64U; ++text_index) {
        unsigned char value = (unsigned char)text[text_index];
        if (!((value >= (unsigned char)'0' &&
               value <= (unsigned char)'9') ||
              (value >= (unsigned char)'a' &&
               value <= (unsigned char)'f')))
            return 0;
    }
    return text[64] == '\0';
}

static int hwa_bp_audio_text_size(const char *text,
                                  size_t maximum,
                                  int allow_empty,
                                  size_t *size)
{
    size_t text_index;
    if (text == NULL || maximum == 0U) return -1;
    for (text_index = 0U; text_index < maximum; ++text_index) {
        if (text[text_index] == '\0') {
            if (!allow_empty && text_index == 0U) return -1;
            if (size != NULL) *size = text_index;
            return 0;
        }
    }
    return -1;
}

static char *hwa_bp_audio_copy_text(const char *text)
{
    char *copy;
    size_t size;
    if (text == NULL) return NULL;
    if (hwa_bp_audio_text_size(
            text, HWA_BP_AUDIO_SETTINGS_MAX, 1, &size) != 0 ||
        size == SIZE_MAX)
        return NULL;
    copy = (char *)malloc(size + 1U);
    if (copy != NULL) memcpy(copy, text, size + 1U);
    return copy;
}

static void hwa_bp_audio_descriptor(HWABasicPitchAudioContext *adapter,
                                    HWAInferenceProvider *provider)
{
    memset(provider, 0, sizeof(*provider));
    provider->name = HWA_BASIC_PITCH_AUDIO_PROVIDER_NAME;
    provider->version = HWA_BASIC_PITCH_AUDIO_PROVIDER_VERSION;
    provider->model_sha256 = adapter->model_sha256;
    provider->context = adapter;
    provider->start = hwa_bp_audio_start;
    provider->poll = hwa_bp_audio_poll;
    provider->task_free = hwa_bp_audio_task_free;
    provider->destroy = hwa_bp_audio_destroy;
}

static const HWAInferenceInput *hwa_bp_audio_source(
    const HWAInferenceRequest *request)
{
    size_t input_index;
    if (request == NULL || request->inputs == NULL ||
        request->source_input_id == NULL)
        return NULL;
    for (input_index = 0U; input_index < request->input_count;
         ++input_index) {
        if (strcmp(request->inputs[input_index].id,
                   request->source_input_id) == 0)
            return &request->inputs[input_index];
    }
    return NULL;
}

static void hwa_bp_audio_le16(unsigned char bytes[2], uint16_t value)
{
    bytes[0] = (unsigned char)(value & UINT16_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT16_C(0xff));
}

static void hwa_bp_audio_le32(unsigned char bytes[4], uint32_t value)
{
    bytes[0] = (unsigned char)(value & UINT32_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT32_C(0xff));
    bytes[2] = (unsigned char)((value >> 16U) & UINT32_C(0xff));
    bytes[3] = (unsigned char)((value >> 24U) & UINT32_C(0xff));
}

static int hwa_bp_audio_write_header(FILE *stream, uint64_t frames)
{
    unsigned char header[44] = {0};
    uint64_t data_bytes = frames * HWA_BP_AUDIO_OUTPUT_BLOCK_ALIGN;
    if (stream == NULL || data_bytes > UINT32_MAX - UINT32_C(36))
        return -1;
    memcpy(header, "RIFF", 4U);
    hwa_bp_audio_le32(header + 4U,
                      (uint32_t)data_bytes + UINT32_C(36));
    memcpy(header + 8U, "WAVEfmt ", 8U);
    hwa_bp_audio_le32(header + 16U, UINT32_C(16));
    hwa_bp_audio_le16(header + 20U, UINT16_C(3));
    hwa_bp_audio_le16(header + 22U, UINT16_C(1));
    hwa_bp_audio_le32(header + 24U, HWA_BP_AUDIO_OUTPUT_RATE);
    hwa_bp_audio_le32(
        header + 28U,
        HWA_BP_AUDIO_OUTPUT_RATE * HWA_BP_AUDIO_OUTPUT_BLOCK_ALIGN);
    hwa_bp_audio_le16(header + 32U, HWA_BP_AUDIO_OUTPUT_BLOCK_ALIGN);
    hwa_bp_audio_le16(header + 34U, HWA_BP_AUDIO_BITS);
    memcpy(header + 36U, "data", 4U);
    hwa_bp_audio_le32(header + 40U, (uint32_t)data_bytes);
    return fwrite(header, 1U, sizeof(header), stream) == sizeof(header)
               ? 0 : -1;
}

static int hwa_bp_audio_decoder_next(HWABasicPitchAudioDecoder *decoder,
                                     float *sample,
                                     char *error,
                                     size_t error_size)
{
    const unsigned char *frame;
    double left;
    double right;
    double mixed;
    int clipped = 0;
    if (decoder == NULL || sample == NULL || decoder->reader == NULL ||
        decoder->decoded_frames >= decoder->total_frames) {
        hwa_bp_audio_error(error, error_size,
                           "invalid raw-audio decode request");
        return -1;
    }
    if (decoder->raw_index == decoder->raw_count) {
        size_t got = 0U;
        if (hwa_wav_reader_read_frames(
                decoder->reader, decoder->raw, decoder->raw_capacity,
                &got, error, error_size) != 0)
            return -1;
        if (got == 0U) {
            hwa_bp_audio_error(error, error_size,
                               "raw audio ended before its data chunk");
            return -1;
        }
        decoder->raw_count = got;
        decoder->raw_index = 0U;
    }
    frame = decoder->raw +
            decoder->raw_index * decoder->reader->format.block_align;
    left = hwa_wav_decode_sample(decoder->reader, frame, &clipped);
    right = hwa_wav_decode_sample(
        decoder->reader, frame + decoder->reader->bytes_per_sample,
        &clipped);
    (void)clipped;
    mixed = left * 0.5 + right * 0.5;
    *sample = (float)mixed;
    if (!isfinite(left) || !isfinite(right) || !isfinite(mixed) ||
        !isfinite((double)*sample)) {
        hwa_bp_audio_error(
            error, error_size,
            "raw audio contains a non-finite or oversized sample");
        return -1;
    }
    decoder->raw_index++;
    decoder->decoded_frames++;
    return 0;
}

static int hwa_bp_audio_decoder_next_or_zero(
    HWABasicPitchAudioDecoder *decoder,
    float *sample,
    char *error,
    size_t error_size)
{
    if (decoder->decoded_frames == decoder->total_frames) {
        *sample = 0.0f;
        return 0;
    }
    return hwa_bp_audio_decoder_next(decoder, sample, error, error_size);
}

static int hwa_bp_audio_prepare(
    HWABasicPitchAudioTask *task,
    const HWAInferenceInput *source,
    const HWAFormat *source_format,
    char *error,
    size_t error_size)
{
    HWAWavReader raw_reader;
    HWABasicPitchAudioDecoder decoder;
    FILE *prepared_stream = NULL;
    unsigned char *raw = NULL;
    unsigned char *encoded = NULL;
    float window[HWA_BP_AUDIO_FIR_TAPS] = {0.0f};
    uint64_t prepared_frames;
    uint64_t output_index;
    size_t ring_start = 0U;
    size_t encoded_count = 0U;
    size_t fill_index;
    size_t raw_bytes;
    size_t encoded_bytes;
    uint64_t fixed_work;
    int raw_reader_open = 0;
    int result = -1;
    memset(&raw_reader, 0, sizeof(raw_reader));
    memset(&decoder, 0, sizeof(decoder));
    if (task == NULL || source == NULL || source_format == NULL ||
        source_format->frames == 0U ||
        source_format->frames > UINT64_MAX - UINT64_C(1)) {
        hwa_bp_audio_error(error, error_size,
                           "invalid raw audio for Basic Pitch");
        return -1;
    }
    prepared_frames = source_format->frames / UINT64_C(2) +
                      source_format->frames % UINT64_C(2);
    if (prepared_frames >
        (uint64_t)(UINT32_MAX - UINT32_C(36)) /
            HWA_BP_AUDIO_OUTPUT_BLOCK_ALIGN) {
        hwa_bp_audio_error(
            error, error_size,
            "raw audio exceeds the prepared RIFF frame limit");
        return -1;
    }
    raw_bytes = HWA_BP_AUDIO_IO_FRAMES *
                (size_t)HWA_BP_AUDIO_INPUT_BLOCK_ALIGN;
    encoded_bytes = HWA_BP_AUDIO_IO_FRAMES *
                    (size_t)HWA_BP_AUDIO_OUTPUT_BLOCK_ALIGN;
    fixed_work = (uint64_t)sizeof(*task) + (uint64_t)raw_bytes +
                 (uint64_t)encoded_bytes + (uint64_t)sizeof(window);
    if (fixed_work > task->max_work_bytes) {
        hwa_bp_audio_error(
            error, error_size,
            "raw-audio preparation exceeds its work limit");
        return -1;
    }
    if (hwa_inference_deadline_check(
            task->deadline_started, task->request->timeout_milliseconds,
            error, error_size) != 0)
        return -1;
    if (hwa_wav_reader_open_source(
            &raw_reader, &source->bytes,
            task->request->max_input_file_bytes,
            error, error_size) != 0)
        return -1;
    raw_reader_open = 1;
    if (raw_reader.format.encoding != HWA_ENCODING_IEEE_FLOAT ||
        raw_reader.format.channels != HWA_BP_AUDIO_CHANNELS ||
        raw_reader.format.sample_rate_hz != HWA_BP_AUDIO_INPUT_RATE ||
        raw_reader.format.bits_per_sample != HWA_BP_AUDIO_BITS ||
        raw_reader.format.valid_bits_per_sample != HWA_BP_AUDIO_BITS ||
        raw_reader.format.block_align != HWA_BP_AUDIO_INPUT_BLOCK_ALIGN ||
        raw_reader.format.frames != source_format->frames) {
        hwa_bp_audio_error(
            error, error_size,
            "raw note inference needs stereo float32 44100 Hz WAVE audio");
        goto cleanup;
    }
    raw = (unsigned char *)malloc(raw_bytes);
    encoded = (unsigned char *)malloc(encoded_bytes);
    prepared_stream = tmpfile();
    if (raw == NULL || encoded == NULL || prepared_stream == NULL ||
        setvbuf(prepared_stream, NULL, _IONBF, 0) != 0 ||
        hwa_bp_audio_write_header(prepared_stream, prepared_frames) != 0) {
        hwa_bp_audio_error(
            error, error_size,
            "cannot allocate raw-audio preparation work");
        goto cleanup;
    }
    decoder.reader = &raw_reader;
    decoder.raw = raw;
    decoder.raw_capacity = HWA_BP_AUDIO_IO_FRAMES;
    decoder.total_frames = source_format->frames;
    for (fill_index = HWA_BP_AUDIO_FIR_HALF;
         fill_index < HWA_BP_AUDIO_FIR_TAPS; ++fill_index) {
        if (hwa_bp_audio_decoder_next_or_zero(
                &decoder, &window[fill_index], error, error_size) != 0)
            goto cleanup;
    }
    for (output_index = 0U; output_index < prepared_frames;
         ++output_index) {
        double sum = 0.0;
        float converted;
        uint32_t bits;
        size_t tap_index;
        for (tap_index = 0U; tap_index < HWA_BP_AUDIO_FIR_TAPS;
             ++tap_index) {
            size_t sample_index = ring_start + tap_index;
            if (sample_index >= HWA_BP_AUDIO_FIR_TAPS)
                sample_index -= HWA_BP_AUDIO_FIR_TAPS;
            sum += hwa_bp_audio_fir[tap_index] *
                   (double)window[sample_index];
        }
        converted = (float)sum;
        if (!isfinite(sum) || !isfinite((double)converted)) {
            hwa_bp_audio_error(
                error, error_size,
                "raw-audio filter produced a non-finite sample");
            goto cleanup;
        }
        memcpy(&bits, &converted, sizeof(bits));
        hwa_bp_audio_le32(encoded + encoded_count * sizeof(float), bits);
        encoded_count++;
        if (encoded_count == HWA_BP_AUDIO_IO_FRAMES) {
            if (fwrite(encoded, sizeof(float), encoded_count,
                       prepared_stream) != encoded_count ||
                hwa_inference_deadline_check(
                    task->deadline_started,
                    task->request->timeout_milliseconds,
                    error, error_size) != 0) {
                if (error != NULL && error_size != 0U && error[0] == '\0')
                    hwa_bp_audio_error(
                        error, error_size,
                        "cannot write prepared Basic Pitch audio");
                goto cleanup;
            }
            encoded_count = 0U;
        }
        if (output_index + UINT64_C(1) < prepared_frames) {
            if (hwa_bp_audio_decoder_next_or_zero(
                    &decoder, &window[ring_start],
                    error, error_size) != 0 ||
                hwa_bp_audio_decoder_next_or_zero(
                    &decoder,
                    &window[(ring_start + 1U) % HWA_BP_AUDIO_FIR_TAPS],
                    error, error_size) != 0)
                goto cleanup;
            ring_start = (ring_start + 2U) % HWA_BP_AUDIO_FIR_TAPS;
        }
    }
    if (encoded_count != 0U &&
        fwrite(encoded, sizeof(float), encoded_count,
               prepared_stream) != encoded_count) {
        hwa_bp_audio_error(error, error_size,
                           "cannot write prepared Basic Pitch audio");
        goto cleanup;
    }
    if (decoder.decoded_frames != source_format->frames ||
        fflush(prepared_stream) != 0) {
        hwa_bp_audio_error(error, error_size,
                           "cannot finish prepared Basic Pitch audio");
        goto cleanup;
    }
    hwa_wav_reader_close(&raw_reader);
    raw_reader_open = 0;
    free(raw);
    raw = NULL;
    free(encoded);
    encoded = NULL;
    if (hwa_wav_reader_open_file(
            &task->prepared_reader, prepared_stream,
            HWA_BP_AUDIO_WAVE_HEADER_BYTES +
                prepared_frames * HWA_BP_AUDIO_OUTPUT_BLOCK_ALIGN,
            error, error_size) != 0) {
        prepared_stream = NULL;
        goto cleanup;
    }
    prepared_stream = NULL;
    task->prepared_reader_open = 1;
    task->prepared_reader.source.name = HWA_BP_AUDIO_PREPARED_NAME;
    if (task->prepared_reader.format.encoding != HWA_ENCODING_IEEE_FLOAT ||
        task->prepared_reader.format.channels != UINT16_C(1) ||
        task->prepared_reader.format.sample_rate_hz !=
            HWA_BP_AUDIO_OUTPUT_RATE ||
        task->prepared_reader.format.bits_per_sample != HWA_BP_AUDIO_BITS ||
        task->prepared_reader.format.frames != prepared_frames ||
        hwa_inference_byte_source_sha256(
            &task->prepared_reader.source,
            task->prepared_reader.source.size,
            task->prepared_sha256, error, error_size) != 0)
        goto cleanup;
    result = 0;
cleanup:
    if (raw_reader_open) hwa_wav_reader_close(&raw_reader);
    free(raw);
    free(encoded);
    if (prepared_stream != NULL) (void)fclose(prepared_stream);
    return result;
}

static int hwa_bp_audio_json_reserve(HWABasicPitchAudioJson *json,
                                     size_t extra)
{
    size_t needed;
    size_t capacity;
    char *grown;
    if (json == NULL || extra > SIZE_MAX - json->length - 1U)
        return -1;
    needed = json->length + extra + 1U;
    if (needed > json->maximum) return -1;
    if (needed <= json->capacity) return 0;
    capacity = json->capacity == 0U ? 256U : json->capacity;
    while (capacity < needed) {
        size_t next = capacity > SIZE_MAX / 2U
                          ? SIZE_MAX : capacity * 2U;
        if (next > json->maximum) next = json->maximum;
        if (next <= capacity) return -1;
        capacity = next;
    }
    grown = (char *)realloc(json->text, capacity);
    if (grown == NULL) return -1;
    json->text = grown;
    json->capacity = capacity;
    return 0;
}

static int hwa_bp_audio_json_append_size(HWABasicPitchAudioJson *json,
                                         const char *text,
                                         size_t size)
{
    if (text == NULL || hwa_bp_audio_json_reserve(json, size) != 0)
        return -1;
    memcpy(json->text + json->length, text, size);
    json->length += size;
    json->text[json->length] = '\0';
    return 0;
}

static int hwa_bp_audio_json_append(HWABasicPitchAudioJson *json,
                                    const char *text)
{
    size_t size;
    if (hwa_bp_audio_text_size(
            text, HWA_BP_AUDIO_SETTINGS_MAX, 1, &size) != 0)
        return -1;
    return hwa_bp_audio_json_append_size(json, text, size);
}

static int hwa_bp_audio_json_string(HWABasicPitchAudioJson *json,
                                    const char *text)
{
    static const char hex[] = "0123456789abcdef";
    size_t size;
    size_t text_index;
    if (hwa_bp_audio_text_size(
            text, HWA_BP_AUDIO_TEXT_LIMIT, 1, &size) != 0 ||
        hwa_bp_audio_json_append_size(json, "\"", 1U) != 0)
        return -1;
    for (text_index = 0U; text_index < size; ++text_index) {
        unsigned char value = (unsigned char)text[text_index];
        if (value == (unsigned char)'\"' ||
            value == (unsigned char)'\\') {
            char escaped[2];
            escaped[0] = '\\';
            escaped[1] = (char)value;
            if (hwa_bp_audio_json_append_size(json, escaped, 2U) != 0)
                return -1;
        } else if (value < 0x20U) {
            char escaped[6] = {'\\', 'u', '0', '0', '0', '0'};
            escaped[4] = hex[value >> 4U];
            escaped[5] = hex[value & 0x0fU];
            if (hwa_bp_audio_json_append_size(json, escaped, 6U) != 0)
                return -1;
        } else if (hwa_bp_audio_json_append_size(
                       json, text + text_index, 1U) != 0) {
            return -1;
        }
    }
    return hwa_bp_audio_json_append_size(json, "\"", 1U);
}

static int hwa_bp_audio_json_u64(HWABasicPitchAudioJson *json,
                                 uint64_t value)
{
    char text[23];
    int written = snprintf(text, sizeof(text), "\"%020" PRIu64 "\"",
                           value);
    return written == 22
               ? hwa_bp_audio_json_append_size(json, text, 22U)
               : -1;
}

static int hwa_bp_audio_settings_build(
    const HWABasicPitchAudioContext *adapter,
    const HWABasicPitchAudioTask *task,
    const HWAEventProvider *child_provider,
    char **settings_result,
    char *error,
    size_t error_size)
{
    HWABasicPitchAudioJson json;
    const HWAInferenceInput *source = hwa_bp_audio_source(task->request);
    size_t maximum = HWA_BP_AUDIO_SETTINGS_MAX;
    char *exact;
    int status = -1;
    if (adapter == NULL || settings_result == NULL || source == NULL ||
        child_provider == NULL || child_provider->settings_json == NULL) {
        hwa_bp_audio_error(error, error_size,
                           "invalid raw-audio provenance facts");
        return -1;
    }
    *settings_result = NULL;
    if (task->max_work_bytes < (uint64_t)maximum)
        maximum = (size_t)task->max_work_bytes;
    if (maximum < 2U) {
        hwa_bp_audio_error(error, error_size,
                           "raw-audio provenance exceeds its work limit");
        return -1;
    }
    memset(&json, 0, sizeof(json));
    json.maximum = maximum;
    if (hwa_bp_audio_json_append(&json, "{\"task\":") != 0 ||
        hwa_bp_audio_json_string(&json, task->request->task) != 0 ||
        hwa_bp_audio_json_append(&json, ",\"seed\":") != 0 ||
        hwa_bp_audio_json_u64(&json, task->request->seed) != 0 ||
        hwa_bp_audio_json_append(&json, ",\"raw_input\":{\"id\":") != 0 ||
        hwa_bp_audio_json_string(&json, source->id) != 0 ||
        hwa_bp_audio_json_append(&json, ",\"role\":") != 0 ||
        hwa_bp_audio_json_string(&json, source->role) != 0 ||
        hwa_bp_audio_json_append(&json, ",\"media_type\":") != 0 ||
        hwa_bp_audio_json_string(&json, source->media_type) != 0 ||
        hwa_bp_audio_json_append(&json, ",\"name\":") != 0 ||
        hwa_bp_audio_json_string(&json, source->bytes.name) != 0 ||
        hwa_bp_audio_json_append(&json, ",\"bytes\":") != 0 ||
        hwa_bp_audio_json_u64(&json, source->bytes.size) != 0 ||
        hwa_bp_audio_json_append(&json, ",\"sha256\":") != 0 ||
        hwa_bp_audio_json_string(&json, source->sha256) != 0 ||
        hwa_bp_audio_json_append(
            &json,
            "},\"transform\":{\"name\":\""
            HWA_BASIC_PITCH_AUDIO_TRANSFORM_NAME
            "\",\"adapter_sha256\":") != 0 ||
        hwa_bp_audio_json_string(&json, adapter->adapter_sha256) != 0 ||
        hwa_bp_audio_json_append(
            &json,
            ",\"downmix\":\"arithmetic-mean-left-right\","
            "\"input_sample_rate_hz\":44100,"
            "\"output_sample_rate_hz\":22050,"
            "\"fir\":{\"taps\":127,\"window\":\"blackman\","
            "\"cutoff_cycles_per_input_sample\":0.225,"
            "\"dc_gain_normalized\":true},"
            "\"boundary\":\"zero-padding\","
            "\"output_frame_rule\":\"ceil(input_frames/2)\","
            "\"prepared_sample_map\":"
            "\"sample-m-centered-at-input-sample-2m\","
            "\"event_map\":"
            "\"[2*start,min(input_frames,2*end))\","
            "\"prepared\":{\"name\":") != 0 ||
        hwa_bp_audio_json_string(
            &json, task->prepared_reader.source.name) != 0 ||
        hwa_bp_audio_json_append(&json, ",\"bytes\":") != 0 ||
        hwa_bp_audio_json_u64(
            &json, task->prepared_reader.source.size) != 0 ||
        hwa_bp_audio_json_append(&json, ",\"frames\":") != 0 ||
        hwa_bp_audio_json_u64(
            &json, task->prepared_reader.format.frames) != 0 ||
        hwa_bp_audio_json_append(&json, ",\"sha256\":") != 0 ||
        hwa_bp_audio_json_string(&json, task->prepared_sha256) != 0 ||
        hwa_bp_audio_json_append(
            &json, "}},\"child_provider\":{\"name\":") != 0 ||
        hwa_bp_audio_json_string(&json, child_provider->name) != 0 ||
        hwa_bp_audio_json_append(&json, ",\"version\":") != 0 ||
        hwa_bp_audio_json_string(&json, child_provider->version) != 0 ||
        hwa_bp_audio_json_append(&json, ",\"model_sha256\":") != 0 ||
        hwa_bp_audio_json_string(
            &json, child_provider->model_sha256) != 0 ||
        hwa_bp_audio_json_append(&json, ",\"settings\":") != 0 ||
        hwa_bp_audio_json_append(
            &json, child_provider->settings_json) != 0 ||
        hwa_bp_audio_json_append(&json, "}}") != 0) {
        hwa_bp_audio_error(error, error_size,
                           "raw-audio provenance exceeds its byte limit");
        goto cleanup;
    }
    exact = (char *)realloc(json.text, json.length + 1U);
    if (exact == NULL) {
        hwa_bp_audio_error(error, error_size,
                           "cannot finalize raw-audio provenance");
        goto cleanup;
    }
    json.text = exact;
    *settings_result = json.text;
    json.text = NULL;
    status = 0;
cleanup:
    free(json.text);
    return status;
}

static int hwa_bp_audio_copy_value(HWAEventValue *target,
                                   const HWAEventValue *source,
                                   uint64_t child_provider_id)
{
    *target = *source;
    target->name = NULL;
    target->text = NULL;
    target->unit = NULL;
    if (source->provider_id_valid) {
        if (source->provider_id != child_provider_id) return -1;
        target->provider_id = HWA_BP_AUDIO_PROVIDER_ID;
    }
    target->name = hwa_bp_audio_copy_text(source->name);
    if (source->text != NULL)
        target->text = hwa_bp_audio_copy_text(source->text);
    target->unit = hwa_bp_audio_copy_text(source->unit);
    if (target->name == NULL ||
        (source->text != NULL && target->text == NULL) ||
        target->unit == NULL)
        return -1;
    return 0;
}

static int hwa_bp_audio_copy_event(HWAPerformanceEvent *target,
                                   const HWAPerformanceEvent *source,
                                   uint64_t child_provider_id,
                                   uint64_t source_id,
                                   uint64_t source_frames)
{
    uint64_t mapped_end;
    size_t value_index;
    *target = *source;
    target->kind = NULL;
    target->voice = NULL;
    target->part = NULL;
    target->score_event_id = NULL;
    target->values = NULL;
    target->trace_refs = NULL;
    target->value_count = 0U;
    target->trace_ref_count = 0U;
    if (strcmp(source->kind, "note") != 0 ||
        source->source_recording_id != source_id ||
        !source->evidence_audio_id_valid ||
        source->evidence_audio_id != source_id ||
        source->parent_id_valid || source->trace_ref_count != 0U ||
        source->start_sample > UINT64_MAX / UINT64_C(2) ||
        source->end_sample > UINT64_MAX / UINT64_C(2))
        return -1;
    target->source_recording_id = source_id;
    target->evidence_audio_id = source_id;
    target->start_sample = source->start_sample * UINT64_C(2);
    mapped_end = source->end_sample * UINT64_C(2);
    target->end_sample = mapped_end < source_frames
                             ? mapped_end : source_frames;
    if (target->start_sample >= target->end_sample ||
        target->end_sample > source_frames)
        return -1;
    target->kind = hwa_bp_audio_copy_text(source->kind);
    target->voice = hwa_bp_audio_copy_text(source->voice);
    target->part = hwa_bp_audio_copy_text(source->part);
    target->score_event_id =
        hwa_bp_audio_copy_text(source->score_event_id);
    if (target->kind == NULL || target->voice == NULL ||
        target->part == NULL || target->score_event_id == NULL)
        return -1;
    if (source->value_count != 0U) {
        if (source->value_count > SIZE_MAX / sizeof(*target->values))
            return -1;
        target->values = (HWAEventValue *)calloc(
            source->value_count, sizeof(*target->values));
        if (target->values == NULL) return -1;
        target->value_count = source->value_count;
        for (value_index = 0U; value_index < source->value_count;
             ++value_index) {
            if (hwa_bp_audio_copy_value(
                    &target->values[value_index],
                    &source->values[value_index],
                    child_provider_id) != 0)
                return -1;
        }
    }
    return 0;
}

static int hwa_bp_audio_build_output(
    HWABasicPitchAudioContext *adapter,
    HWABasicPitchAudioTask *task,
    const HWAInferenceOutput *child_output,
    char *error,
    size_t error_size)
{
    const HWAEventBundle *child_bundle = child_output->bundle;
    const HWAInferenceInput *source = hwa_bp_audio_source(task->request);
    const HWAEventProvider *child_provider = NULL;
    HWAEventBundleLimits work_limits;
    HWAInferenceProvider descriptor;
    char *settings = NULL;
    uint64_t child_provider_id = 0U;
    size_t provider_index;
    size_t event_index;
    if (source == NULL || child_output->payload_count != 0U ||
        child_bundle == NULL || child_bundle->provider_count != 1U ||
        child_bundle->audio_count != 1U || child_bundle->trace_count != 0U ||
        child_bundle->warning_count != 0U) {
        hwa_bp_audio_error(error, error_size,
                           "Basic Pitch returned an unsupported result shape");
        return -1;
    }
    for (provider_index = 0U;
         provider_index < child_bundle->provider_count;
         ++provider_index) {
        const HWAEventProvider *candidate =
            &child_bundle->providers[provider_index];
        if (strcmp(candidate->name, task->child.name) == 0 &&
            strcmp(candidate->version, task->child.version) == 0 &&
            strcmp(candidate->model_sha256,
                   task->child.model_sha256) == 0) {
            child_provider = candidate;
            child_provider_id = candidate->id;
        }
    }
    if (child_provider == NULL ||
        hwa_bp_audio_settings_build(
            adapter, task, child_provider, &settings, error, error_size) != 0)
        return -1;
    task->bundle.providers =
        (HWAEventProvider *)calloc(1U, sizeof(*task->bundle.providers));
    task->bundle.audio =
        (HWAEventAudio *)calloc(1U, sizeof(*task->bundle.audio));
    if (child_bundle->event_count != 0U) {
        if (child_bundle->event_count >
            SIZE_MAX / sizeof(*task->bundle.events)) {
            hwa_bp_audio_error(error, error_size,
                               "raw-audio note count overflows");
            goto failure;
        }
        task->bundle.events = (HWAPerformanceEvent *)calloc(
            child_bundle->event_count, sizeof(*task->bundle.events));
    }
    if (task->bundle.providers == NULL || task->bundle.audio == NULL ||
        (child_bundle->event_count != 0U && task->bundle.events == NULL)) {
        hwa_bp_audio_error(error, error_size,
                           "cannot allocate raw-audio note output");
        goto failure;
    }
    task->bundle.provider_count = 1U;
    task->bundle.audio_count = 1U;
    task->bundle.event_count = child_bundle->event_count;
    task->bundle.providers[0].id = HWA_BP_AUDIO_PROVIDER_ID;
    task->bundle.providers[0].name = hwa_bp_audio_copy_text(
        HWA_BASIC_PITCH_AUDIO_PROVIDER_NAME);
    task->bundle.providers[0].version = hwa_bp_audio_copy_text(
        HWA_BASIC_PITCH_AUDIO_PROVIDER_VERSION);
    task->bundle.providers[0].settings_json = settings;
    settings = NULL;
    memcpy(task->bundle.providers[0].model_sha256,
           adapter->model_sha256, HWA_SHA256_HEX_SIZE);
    task->bundle.audio[0].id = task->request->source_recording_id;
    task->bundle.audio[0].kind = HWA_EVENT_SOURCE_RECORDING;
    task->bundle.audio[0].name = hwa_bp_audio_copy_text(
        source->bytes.name);
    task->bundle.audio[0].relative_path = hwa_bp_audio_copy_text("");
    task->bundle.audio[0].path_hint = hwa_bp_audio_copy_text("");
    memcpy(task->bundle.audio[0].sha256, source->sha256,
           HWA_SHA256_HEX_SIZE);
    task->bundle.audio[0].file_bytes = source->bytes.size;
    task->bundle.audio[0].format = task->request->source_format;
    if (task->bundle.providers[0].name == NULL ||
        task->bundle.providers[0].version == NULL ||
        task->bundle.providers[0].settings_json == NULL ||
        task->bundle.audio[0].name == NULL ||
        task->bundle.audio[0].relative_path == NULL ||
        task->bundle.audio[0].path_hint == NULL) {
        hwa_bp_audio_error(error, error_size,
                           "cannot allocate raw-audio note output");
        goto failure;
    }
    for (event_index = 0U; event_index < child_bundle->event_count;
         ++event_index) {
        if (hwa_bp_audio_copy_event(
                &task->bundle.events[event_index],
                &child_bundle->events[event_index], child_provider_id,
                task->request->source_recording_id,
                task->request->source_format.frames) != 0) {
            hwa_bp_audio_error(
                error, error_size,
                "Basic Pitch returned an unsupported note event");
            goto failure;
        }
    }
    work_limits = task->request->output_limits;
    if (work_limits.max_work_bytes > task->max_work_bytes)
        work_limits.max_work_bytes = task->max_work_bytes;
    if (hwa_event_bundle_validate(
            &task->bundle, &work_limits, error, error_size) != 0)
        goto failure;
    task->output.bundle = &task->bundle;
    hwa_bp_audio_descriptor(adapter, &descriptor);
    if (hwa_inference_output_validate_for_request(
            &descriptor, task->request, &task->output,
            error, error_size) != 0)
        goto failure;
    return 0;
failure:
    free(settings);
    hwa_event_bundle_free(&task->bundle);
    memset(&task->output, 0, sizeof(task->output));
    return -1;
}

static int hwa_bp_audio_start(void *context_value,
                              const HWAInferenceRequest *request,
                              void **task_result,
                              char *error,
                              size_t error_size)
{
    HWABasicPitchAudioContext *adapter =
        (HWABasicPitchAudioContext *)context_value;
    HWABasicPitchAudioTask *task = NULL;
    HWAInferenceProvider descriptor;
    const HWAInferenceInput *source;
    uint64_t child_timeout_milliseconds = 0U;
    uint64_t child_work_bytes;
    uint64_t deadline_started = 0U;
    int status = -1;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (task_result == NULL) {
        hwa_bp_audio_error(error, error_size,
                           "raw-audio task output is null");
        return -1;
    }
    *task_result = NULL;
    if (adapter == NULL) {
        hwa_bp_audio_error(error, error_size,
                           "raw-audio provider context is null");
        return -1;
    }
    if (hwa_inference_deadline_start(
            &deadline_started, error, error_size) != 0)
        return -1;
    hwa_bp_audio_descriptor(adapter, &descriptor);
    if (hwa_inference_request_validate(
            &descriptor, request, error, error_size) != 0)
        return -1;
    if (strcmp(request->task, HWA_BASIC_PITCH_AUDIO_TASK_NAME) != 0 ||
        request->input_count != 1U ||
        request->source_format.encoding != HWA_ENCODING_IEEE_FLOAT ||
        request->source_format.channels != HWA_BP_AUDIO_CHANNELS ||
        request->source_format.sample_rate_hz != HWA_BP_AUDIO_INPUT_RATE ||
        request->source_format.bits_per_sample != HWA_BP_AUDIO_BITS ||
        request->source_format.valid_bits_per_sample != HWA_BP_AUDIO_BITS ||
        request->source_format.block_align !=
            HWA_BP_AUDIO_INPUT_BLOCK_ALIGN ||
        request->source_format.frames == 0U) {
        hwa_bp_audio_error(
            error, error_size,
            "raw note inference needs stereo float32 44100 Hz WAVE audio");
        return -1;
    }
    source = hwa_bp_audio_source(request);
    if (source == NULL) {
        hwa_bp_audio_error(error, error_size,
                           "raw-audio source input is missing");
        return -1;
    }
    task = (HWABasicPitchAudioTask *)calloc(1U, sizeof(*task));
    if (task == NULL) {
        hwa_bp_audio_error(error, error_size,
                           "cannot allocate raw-audio task");
        return -1;
    }
    task->child = adapter->child;
    task->request = request;
    task->deadline_started = deadline_started;
    task->max_work_bytes = adapter->max_work_bytes;
    if (task->max_work_bytes > request->output_limits.max_work_bytes)
        task->max_work_bytes = request->output_limits.max_work_bytes;
    if (hwa_bp_audio_prepare(
            task, source, &request->source_format,
            error, error_size) != 0)
        goto cleanup;
    if (hwa_inference_deadline_remaining(
            task->deadline_started, request->timeout_milliseconds,
            &child_timeout_milliseconds, error, error_size) != 0)
        goto cleanup;
    if (task->max_work_bytes <=
        (uint64_t)sizeof(*task) + HWA_BP_AUDIO_WRAPPER_RESERVE) {
        hwa_bp_audio_error(
            error, error_size, "raw-audio wrapper exceeds its work limit");
        goto cleanup;
    }
    child_work_bytes =
        (task->max_work_bytes - (uint64_t)sizeof(*task) -
         HWA_BP_AUDIO_WRAPPER_RESERVE) /
        UINT64_C(2);
    if (child_work_bytes == 0U) {
        hwa_bp_audio_error(
            error, error_size, "raw-audio wrapper exceeds its work limit");
        goto cleanup;
    }
    task->child_input.id = HWA_BP_AUDIO_PREPARED_INPUT_ID;
    task->child_input.role = "source-recording";
    task->child_input.media_type = "audio/wav";
    task->child_input.sha256 = task->prepared_sha256;
    task->child_input.bytes = task->prepared_reader.source;
    task->child_request.task = HWA_BASIC_PITCH_TASK_NAME;
    task->child_request.settings_json = request->settings_json;
    task->child_request.expected_provider_name = task->child.name;
    task->child_request.expected_provider_version = task->child.version;
    task->child_request.expected_model_sha256 = task->child.model_sha256;
    task->child_request.seed = request->seed;
    task->child_request.source_recording_id = request->source_recording_id;
    task->child_request.source_input_id = task->child_input.id;
    task->child_request.inputs = &task->child_input;
    task->child_request.input_count = 1U;
    task->child_request.source_format = task->prepared_reader.format;
    task->child_request.max_input_file_bytes =
        task->prepared_reader.source.size;
    task->child_request.max_input_bytes = task->prepared_reader.source.size;
    task->child_request.timeout_milliseconds = child_timeout_milliseconds;
    task->child_request.output_limits = request->output_limits;
    task->child_request.output_limits.max_work_bytes =
        child_work_bytes;
    if (hwa_inference_deadline_check(
            task->deadline_started, request->timeout_milliseconds,
            error, error_size) != 0 ||
        task->child.start(
            task->child.context, &task->child_request,
            &task->child_task, error, error_size) != 0)
        goto cleanup;
    if (task->child_task == NULL) {
        hwa_bp_audio_error(
            error, error_size, "Basic Pitch child returned no task");
        goto cleanup;
    }
    if (hwa_inference_deadline_check(
            task->deadline_started, request->timeout_milliseconds,
            error, error_size) != 0)
        goto cleanup;
    *task_result = task;
    task = NULL;
    status = 0;
cleanup:
    hwa_bp_audio_task_free(NULL, task);
    return status;
}

static int hwa_bp_audio_poll(void *context_value,
                             void *task_value,
                             HWAInferencePollState *state,
                             const HWAInferenceOutput **output,
                             char *error,
                             size_t error_size)
{
    HWABasicPitchAudioContext *adapter =
        (HWABasicPitchAudioContext *)context_value;
    HWABasicPitchAudioTask *task = (HWABasicPitchAudioTask *)task_value;
    const HWAInferenceOutput *child_output = NULL;
    HWAInferencePollState child_state = HWA_INFERENCE_PENDING;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (state != NULL) *state = HWA_INFERENCE_PENDING;
    if (output != NULL) *output = NULL;
    if (adapter == NULL || task == NULL || state == NULL || output == NULL) {
        hwa_bp_audio_error(error, error_size,
                           "invalid raw-audio inference poll");
        return -1;
    }
    if (task->ready) {
        *state = HWA_INFERENCE_READY;
        *output = &task->output;
        return 0;
    }
    if (hwa_inference_deadline_check(
            task->deadline_started,
            task->request->timeout_milliseconds,
            error, error_size) != 0)
        return -1;
    if (task->child.poll(
            task->child.context, task->child_task, &child_state,
            &child_output, error, error_size) != 0)
        return -1;
    if (child_state == HWA_INFERENCE_PENDING) return 0;
    if (child_state != HWA_INFERENCE_READY || child_output == NULL) {
        hwa_bp_audio_error(error, error_size,
                           "Basic Pitch returned an invalid poll state");
        return -1;
    }
    if (hwa_inference_output_validate_for_request(
            &task->child, &task->child_request, child_output,
            error, error_size) != 0 ||
        hwa_bp_audio_build_output(
            adapter, task, child_output, error, error_size) != 0 ||
        hwa_inference_deadline_check(
            task->deadline_started,
            task->request->timeout_milliseconds,
            error, error_size) != 0)
        return -1;
    task->child.task_free(task->child.context, task->child_task);
    task->child_task = NULL;
    if (task->prepared_reader_open) {
        hwa_wav_reader_close(&task->prepared_reader);
        task->prepared_reader_open = 0;
    }
    task->ready = 1;
    *state = HWA_INFERENCE_READY;
    *output = &task->output;
    return 0;
}

static void hwa_bp_audio_task_free(void *context_value, void *task_value)
{
    HWABasicPitchAudioTask *task =
        (HWABasicPitchAudioTask *)task_value;
    (void)context_value;
    if (task == NULL) return;
    if (task->child_task != NULL)
        task->child.task_free(task->child.context, task->child_task);
    if (task->prepared_reader_open)
        hwa_wav_reader_close(&task->prepared_reader);
    hwa_event_bundle_free(&task->bundle);
    free(task);
}

static void hwa_bp_audio_destroy(void *context_value)
{
    free(context_value);
}

int hwa_basic_pitch_audio_provider_init(
    HWAInferenceProvider *provider,
    const HWAInferenceProvider *basic_pitch,
    const char *adapter_sha256,
    uint64_t max_work_bytes,
    char *error,
    size_t error_size)
{
    HWABasicPitchAudioContext *adapter = NULL;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (provider == NULL) {
        hwa_bp_audio_error(error, error_size,
                           "raw-audio provider output is null");
        return -1;
    }
    memset(provider, 0, sizeof(*provider));
    if (basic_pitch == NULL ||
        basic_pitch->name == NULL ||
        strcmp(basic_pitch->name, HWA_BASIC_PITCH_PROVIDER_NAME) != 0 ||
        basic_pitch->version == NULL ||
        strcmp(basic_pitch->version, HWA_BASIC_PITCH_PROVIDER_VERSION) != 0 ||
        !hwa_bp_audio_lower_sha256(basic_pitch->model_sha256) ||
        !hwa_bp_audio_lower_sha256(adapter_sha256) ||
        basic_pitch->start == NULL || basic_pitch->poll == NULL ||
        basic_pitch->task_free == NULL || basic_pitch->destroy == NULL ||
        max_work_bytes == 0U ||
        sizeof(float) != 4U || FLT_RADIX != 2 || FLT_MANT_DIG != 24 ||
        FLT_MAX_EXP != 128) {
        hwa_bp_audio_error(
            error, error_size,
            "invalid borrowed Basic Pitch provider or work limit");
        return -1;
    }
    adapter = (HWABasicPitchAudioContext *)calloc(1U, sizeof(*adapter));
    if (adapter == NULL) {
        hwa_bp_audio_error(error, error_size,
                           "cannot allocate raw-audio provider");
        return -1;
    }
    adapter->child = *basic_pitch;
    memcpy(adapter->model_sha256, basic_pitch->model_sha256,
           HWA_SHA256_HEX_SIZE);
    memcpy(adapter->adapter_sha256, adapter_sha256,
           HWA_SHA256_HEX_SIZE);
    adapter->child.name = HWA_BASIC_PITCH_PROVIDER_NAME;
    adapter->child.version = HWA_BASIC_PITCH_PROVIDER_VERSION;
    adapter->child.model_sha256 = adapter->model_sha256;
    adapter->max_work_bytes = max_work_bytes;
    hwa_bp_audio_descriptor(adapter, provider);
    return 0;
}

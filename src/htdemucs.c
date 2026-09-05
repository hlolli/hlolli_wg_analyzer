#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "htdemucs.h"

#include "inference_clock.h"
#include "internal.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#endif

#define HWA_HTDEMUCS_TEXT_BYTES 4096U
#define HWA_HTDEMUCS_IO_BLOCK_FRAMES 4096U
#define HWA_HTDEMUCS_WAVE_HEADER_BYTES 44U
#define HWA_HTDEMUCS_OUTPUT_BLOCK_ALIGN 8U
#define HWA_HTDEMUCS_FADE_EPSILON 1.0e-8f

typedef struct HWAHTDemucsWaveFile {
    FILE *file;
    uint64_t size;
} HWAHTDemucsWaveFile;

typedef struct HWAHTDemucsResults {
    HWAInstrumentStemResult stems[HWA_HTDEMUCS_STEM_COUNT];
    HWAHTDemucsWaveFile files[HWA_HTDEMUCS_STEM_COUNT];
} HWAHTDemucsResults;

typedef struct HWAHTDemucsContext {
    uint64_t max_work_bytes;
    uint64_t persistent_work_bytes;
    void *model_context;
    char *runtime_name;
    char *runtime_version;
    char *backend;
    char *fallback;
    HWAHTDemucsRunWindowFunction run_window;
    HWAHTDemucsModelRunnerDestroyFunction model_destroy;
} HWAHTDemucsContext;

static const char *const hwa_htdemucs_stem_ids[HWA_HTDEMUCS_STEM_COUNT] = {
    "drums", "bass", "other", "vocals", "guitar", "piano"
};

static const char *const hwa_htdemucs_wave_names[HWA_HTDEMUCS_STEM_COUNT] = {
    "drums.wav", "bass.wav", "other.wav", "vocals.wav", "guitar.wav",
    "piano.wav"
};

static void hwa_htdemucs_error(char *error,
                               size_t error_size,
                               const char *message)
{
    if (error == NULL || error_size == 0U) return;
    (void)snprintf(error, error_size, "%s", message);
    error[error_size - 1U] = '\0';
}

static int hwa_htdemucs_text_size(const char *text,
                                  int allow_empty,
                                  size_t *size)
{
    size_t index;
    if (text == NULL) return -1;
    for (index = 0U; index < HWA_HTDEMUCS_TEXT_BYTES; ++index) {
        if (text[index] == '\0') {
            if (!allow_empty && index == 0U) return -1;
            if (size != NULL) *size = index;
            return 0;
        }
    }
    return -1;
}

static char *hwa_htdemucs_copy_text(const char *text,
                                    int allow_empty)
{
    char *copy;
    size_t size;
    if (hwa_htdemucs_text_size(text, allow_empty, &size) != 0 ||
        size == SIZE_MAX)
        return NULL;
    copy = (char *)malloc(size + 1U);
    if (copy != NULL) memcpy(copy, text, size + 1U);
    return copy;
}

static int hwa_htdemucs_work_add(uint64_t *total,
                                 size_t count,
                                 size_t item_size,
                                 uint64_t maximum)
{
    size_t bytes;
    if (total == NULL || (item_size != 0U && count > SIZE_MAX / item_size))
        return -1;
    bytes = count * item_size;
#if SIZE_MAX > UINT64_MAX
    if (bytes > (size_t)UINT64_MAX) return -1;
#endif
    if (*total > maximum || (uint64_t)bytes > maximum - *total)
        return -1;
    *total += (uint64_t)bytes;
    return 0;
}

static int hwa_htdemucs_work_add_text(uint64_t *total,
                                      const char *text,
                                      int allow_empty,
                                      uint64_t maximum)
{
    size_t size;
    if (hwa_htdemucs_text_size(text, allow_empty, &size) != 0 ||
        size == SIZE_MAX)
        return -1;
    return hwa_htdemucs_work_add(total, size + 1U, 1U, maximum);
}

static void hwa_htdemucs_le16(unsigned char bytes[2], uint16_t value)
{
    bytes[0] = (unsigned char)(value & UINT16_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT16_C(0xff));
}

static void hwa_htdemucs_le32(unsigned char bytes[4], uint32_t value)
{
    bytes[0] = (unsigned char)(value & UINT32_C(0xff));
    bytes[1] = (unsigned char)((value >> 8U) & UINT32_C(0xff));
    bytes[2] = (unsigned char)((value >> 16U) & UINT32_C(0xff));
    bytes[3] = (unsigned char)((value >> 24U) & UINT32_C(0xff));
}

static int hwa_htdemucs_file_seek(FILE *file, uint64_t offset)
{
#if defined(_WIN32)
    if (offset > (uint64_t)INT64_MAX) return -1;
    return _fseeki64(file, (__int64)offset, SEEK_SET) == 0 ? 0 : -1;
#else
    off_t target = (off_t)offset;
    if (target < (off_t)0 || (uint64_t)target != offset) return -1;
    return fseeko(file, target, SEEK_SET) == 0 ? 0 : -1;
#endif
}

static int hwa_htdemucs_wave_read_at(void *context,
                                     uint64_t offset,
                                     unsigned char *destination,
                                     size_t size)
{
    HWAHTDemucsWaveFile *wave = (HWAHTDemucsWaveFile *)context;
    if (wave == NULL || wave->file == NULL ||
        (destination == NULL && size != 0U) ||
        offset > wave->size || (uint64_t)size > wave->size - offset)
        return -1;
    if (size == 0U) return 0;
    clearerr(wave->file);
    if (hwa_htdemucs_file_seek(wave->file, offset) != 0 ||
        fread(destination, 1U, size, wave->file) != size)
        return -1;
    return 0;
}

static int hwa_htdemucs_wave_header(FILE *file,
                                    uint64_t frames)
{
    unsigned char header[HWA_HTDEMUCS_WAVE_HEADER_BYTES] = {0};
    uint64_t data_bytes = frames * HWA_HTDEMUCS_OUTPUT_BLOCK_ALIGN;
    if (file == NULL || data_bytes > UINT32_MAX - UINT32_C(36)) return -1;
    memcpy(header, "RIFF", 4U);
    hwa_htdemucs_le32(header + 4U, (uint32_t)data_bytes + UINT32_C(36));
    memcpy(header + 8U, "WAVEfmt ", 8U);
    hwa_htdemucs_le32(header + 16U, UINT32_C(16));
    hwa_htdemucs_le16(header + 20U, UINT16_C(3));
    hwa_htdemucs_le16(header + 22U, UINT16_C(2));
    hwa_htdemucs_le32(header + 24U, HWA_HTDEMUCS_SAMPLE_RATE);
    hwa_htdemucs_le32(
        header + 28U,
        HWA_HTDEMUCS_SAMPLE_RATE * HWA_HTDEMUCS_OUTPUT_BLOCK_ALIGN);
    hwa_htdemucs_le16(
        header + 32U, (uint16_t)HWA_HTDEMUCS_OUTPUT_BLOCK_ALIGN);
    hwa_htdemucs_le16(header + 34U, UINT16_C(32));
    memcpy(header + 36U, "data", 4U);
    hwa_htdemucs_le32(header + 40U, (uint32_t)data_bytes);
    return fwrite(header, 1U, sizeof(header), file) == sizeof(header) ? 0 : -1;
}

static void hwa_htdemucs_results_destroy(void *context,
                                         HWAInstrumentStemResult *stems,
                                         size_t stem_count)
{
    HWAHTDemucsResults *results = (HWAHTDemucsResults *)stems;
    size_t index;
    (void)context;
    (void)stem_count;
    if (results == NULL) return;
    for (index = 0U; index < HWA_HTDEMUCS_STEM_COUNT; ++index) {
        if (results->files[index].file != NULL)
            (void)fclose(results->files[index].file);
    }
    free(results);
}

static HWAHTDemucsResults *hwa_htdemucs_results_create(uint64_t frames,
                                                        char *error,
                                                        size_t error_size)
{
    HWAHTDemucsResults *results;
    size_t index;
    uint64_t data_bytes = frames * HWA_HTDEMUCS_OUTPUT_BLOCK_ALIGN;
    results = (HWAHTDemucsResults *)calloc(1U, sizeof(*results));
    if (results == NULL) {
        hwa_htdemucs_error(error, error_size,
                           "cannot allocate HTDemucs stem results");
        return NULL;
    }
    for (index = 0U; index < HWA_HTDEMUCS_STEM_COUNT; ++index) {
        HWAInstrumentStemResult *stem = &results->stems[index];
        HWAHTDemucsWaveFile *file = &results->files[index];
        file->file = tmpfile();
        if (file->file == NULL ||
            setvbuf(file->file, NULL, _IONBF, 0) != 0 ||
            hwa_htdemucs_wave_header(file->file, frames) != 0) {
            hwa_htdemucs_error(error, error_size,
                               "cannot create an HTDemucs stem file");
            hwa_htdemucs_results_destroy(NULL, results->stems,
                                          HWA_HTDEMUCS_STEM_COUNT);
            return NULL;
        }
        file->size = HWA_HTDEMUCS_WAVE_HEADER_BYTES;
        stem->stem_id = hwa_htdemucs_stem_ids[index];
        stem->instrument = hwa_htdemucs_stem_ids[index];
        stem->score = 0.0;
        stem->score_valid = 0;
        stem->wave.context = file;
        stem->wave.name = hwa_htdemucs_wave_names[index];
        stem->wave.size = HWA_HTDEMUCS_WAVE_HEADER_BYTES + data_bytes;
        stem->wave.read_at = hwa_htdemucs_wave_read_at;
    }
    return results;
}

static int hwa_htdemucs_format_equal(const HWAFormat *left,
                                     const HWAFormat *right)
{
    return left != NULL && right != NULL &&
           left->container == right->container &&
           left->encoding == right->encoding &&
           left->channels == right->channels &&
           left->sample_rate_hz == right->sample_rate_hz &&
           left->bits_per_sample == right->bits_per_sample &&
           left->valid_bits_per_sample == right->valid_bits_per_sample &&
           left->block_align == right->block_align &&
           left->channel_mask == right->channel_mask &&
           left->frames == right->frames &&
           left->data_bytes == right->data_bytes &&
           left->duration_seconds == right->duration_seconds;
}

static int hwa_htdemucs_decode_frames(HWAWavReader *reader,
                                      float *input,
                                      size_t destination_frame,
                                      size_t wanted,
                                      unsigned char *raw,
                                      char *error,
                                      size_t error_size)
{
    size_t written = 0U;
    while (written < wanted) {
        size_t request = wanted - written;
        size_t got = 0U;
        size_t frame;
        if (request > HWA_HTDEMUCS_IO_BLOCK_FRAMES)
            request = HWA_HTDEMUCS_IO_BLOCK_FRAMES;
        if (hwa_wav_reader_read_frames(
                reader, raw, request, &got, error, error_size) != 0)
            return -1;
        if (got == 0U) {
            hwa_htdemucs_error(
                error, error_size, "HTDemucs WAVE input ended early");
            return -1;
        }
        for (frame = 0U; frame < got; ++frame) {
            size_t channel;
            for (channel = 0U; channel < HWA_HTDEMUCS_CHANNELS; ++channel) {
                size_t source_channel = reader->format.channels == 1U
                                            ? 0U : channel;
                const unsigned char *sample =
                    raw + frame * reader->format.block_align +
                    source_channel * reader->bytes_per_sample;
                int clipped = 0;
                double decoded = hwa_wav_decode_sample(
                    reader, sample, &clipped);
                float converted = (float)decoded;
                (void)clipped;
                if (!isfinite(decoded) || decoded < -1.0 || decoded > 1.0 ||
                    !isfinite((double)converted)) {
                    hwa_htdemucs_error(
                        error, error_size,
                        "HTDemucs input sample is not finite or outside [-1, 1]");
                    return -1;
                }
                input[channel * HWA_HTDEMUCS_INPUT_SAMPLES +
                      destination_frame + written + frame] = converted;
            }
        }
        written += got;
    }
    return 0;
}

static int hwa_htdemucs_output_valid(const float *output)
{
    size_t index;
    for (index = 0U; index < HWA_HTDEMUCS_OUTPUT_CELLS; ++index) {
        if (!isfinite((double)output[index])) return 0;
    }
    return 1;
}

static float hwa_htdemucs_output_sample(const float *output,
                                        size_t stem,
                                        size_t channel,
                                        size_t frame)
{
    return output[(stem * HWA_HTDEMUCS_CHANNELS + channel) *
                      HWA_HTDEMUCS_INPUT_SAMPLES +
                  frame];
}

static float hwa_htdemucs_tail_sample(const float *tail,
                                      size_t stem,
                                      size_t channel,
                                      size_t frame)
{
    return tail[(stem * HWA_HTDEMUCS_CHANNELS + channel) *
                    HWA_HTDEMUCS_OVERLAP_SAMPLES +
                frame];
}

static int hwa_htdemucs_write_sample(unsigned char bytes[4], float value)
{
    uint32_t bits;
    if (!isfinite((double)value)) return -1;
    memcpy(&bits, &value, sizeof(bits));
    hwa_htdemucs_le32(bytes, bits);
    return 0;
}

static int hwa_htdemucs_emit_raw(HWAHTDemucsResults *results,
                                 const float *output,
                                 size_t local_start,
                                 size_t frame_count,
                                 int apply_head_weight,
                                 char *error,
                                 size_t error_size)
{
    size_t stem;
    for (stem = 0U; stem < HWA_HTDEMUCS_STEM_COUNT; ++stem) {
        size_t done = 0U;
        while (done < frame_count) {
            unsigned char encoded[HWA_HTDEMUCS_IO_BLOCK_FRAMES *
                                  HWA_HTDEMUCS_OUTPUT_BLOCK_ALIGN];
            size_t count = frame_count - done;
            size_t frame;
            if (count > HWA_HTDEMUCS_IO_BLOCK_FRAMES)
                count = HWA_HTDEMUCS_IO_BLOCK_FRAMES;
            for (frame = 0U; frame < count; ++frame) {
                size_t channel;
                for (channel = 0U; channel < HWA_HTDEMUCS_CHANNELS;
                     ++channel) {
                    size_t local_frame = local_start + done + frame;
                    float value = hwa_htdemucs_output_sample(
                        output, stem, channel, local_frame);
                    if (apply_head_weight &&
                        local_frame < HWA_HTDEMUCS_OVERLAP_SAMPLES) {
                        float weight =
                            (float)((double)local_frame /
                                    (double)(HWA_HTDEMUCS_OVERLAP_SAMPLES -
                                             1U));
                        float denominator = weight;
                        if (denominator < HWA_HTDEMUCS_FADE_EPSILON)
                            denominator = HWA_HTDEMUCS_FADE_EPSILON;
                        value = value * weight / denominator;
                    }
                    if (hwa_htdemucs_write_sample(
                            encoded +
                                (frame * HWA_HTDEMUCS_CHANNELS + channel) *
                                    sizeof(float),
                            value) != 0) {
                        hwa_htdemucs_error(
                            error, error_size,
                            "HTDemucs overlap produced a non-finite sample");
                        return -1;
                    }
                }
            }
            if (fwrite(encoded, HWA_HTDEMUCS_OUTPUT_BLOCK_ALIGN,
                       count, results->files[stem].file) != count) {
                hwa_htdemucs_error(error, error_size,
                                   "cannot write an HTDemucs stem file");
                return -1;
            }
            results->files[stem].size +=
                (uint64_t)count * HWA_HTDEMUCS_OUTPUT_BLOCK_ALIGN;
            done += count;
        }
    }
    return 0;
}

static int hwa_htdemucs_emit_overlap(HWAHTDemucsResults *results,
                                     const float *tail,
                                     const float *output,
                                     size_t frame_count,
                                     char *error,
                                     size_t error_size)
{
    size_t stem;
    const double fade_divisor =
        (double)(HWA_HTDEMUCS_OVERLAP_SAMPLES - 1U);
    for (stem = 0U; stem < HWA_HTDEMUCS_STEM_COUNT; ++stem) {
        size_t done = 0U;
        while (done < frame_count) {
            unsigned char encoded[HWA_HTDEMUCS_IO_BLOCK_FRAMES *
                                  HWA_HTDEMUCS_OUTPUT_BLOCK_ALIGN];
            size_t count = frame_count - done;
            size_t frame;
            if (count > HWA_HTDEMUCS_IO_BLOCK_FRAMES)
                count = HWA_HTDEMUCS_IO_BLOCK_FRAMES;
            for (frame = 0U; frame < count; ++frame) {
                size_t overlap_frame = done + frame;
                float right_weight =
                    (float)((double)overlap_frame / fade_divisor);
                float left_weight =
                    (float)((double)(HWA_HTDEMUCS_OVERLAP_SAMPLES - 1U -
                                    overlap_frame) /
                            fade_divisor);
                float denominator = left_weight + right_weight;
                size_t channel;
                if (denominator < HWA_HTDEMUCS_FADE_EPSILON)
                    denominator = HWA_HTDEMUCS_FADE_EPSILON;
                for (channel = 0U; channel < HWA_HTDEMUCS_CHANNELS;
                     ++channel) {
                    float left = hwa_htdemucs_tail_sample(
                        tail, stem, channel, overlap_frame);
                    float right = hwa_htdemucs_output_sample(
                        output, stem, channel, overlap_frame);
                    float value =
                        (left * left_weight + right * right_weight) /
                        denominator;
                    if (hwa_htdemucs_write_sample(
                            encoded +
                                (frame * HWA_HTDEMUCS_CHANNELS + channel) *
                                    sizeof(float),
                            value) != 0) {
                        hwa_htdemucs_error(
                            error, error_size,
                            "HTDemucs overlap produced a non-finite sample");
                        return -1;
                    }
                }
            }
            if (fwrite(encoded, HWA_HTDEMUCS_OUTPUT_BLOCK_ALIGN,
                       count, results->files[stem].file) != count) {
                hwa_htdemucs_error(error, error_size,
                                   "cannot write an HTDemucs stem file");
                return -1;
            }
            results->files[stem].size +=
                (uint64_t)count * HWA_HTDEMUCS_OUTPUT_BLOCK_ALIGN;
            done += count;
        }
    }
    return 0;
}

static void hwa_htdemucs_save_tail(float *tail,
                                   const float *output,
                                   size_t frame_count)
{
    size_t plane;
    for (plane = 0U;
         plane < HWA_HTDEMUCS_STEM_COUNT * HWA_HTDEMUCS_CHANNELS;
         ++plane) {
        memcpy(tail + plane * HWA_HTDEMUCS_OVERLAP_SAMPLES,
               output + plane * HWA_HTDEMUCS_INPUT_SAMPLES +
                   HWA_HTDEMUCS_STRIDE_SAMPLES,
               frame_count * sizeof(*tail));
    }
}

static int hwa_htdemucs_finalize_results(HWAHTDemucsResults *results,
                                         uint64_t expected_size,
                                         char *error,
                                         size_t error_size)
{
    size_t index;
    for (index = 0U; index < HWA_HTDEMUCS_STEM_COUNT; ++index) {
        if (results->files[index].size != expected_size ||
            fflush(results->files[index].file) != 0) {
            hwa_htdemucs_error(
                error, error_size, "HTDemucs stem file has the wrong size");
            return -1;
        }
    }
    return 0;
}

static int hwa_htdemucs_run(void *context_value,
                            const HWAByteSource *source,
                            const HWAFormat *source_format,
                            uint64_t seed,
                            uint64_t timeout_milliseconds,
                            HWAInstrumentStemResult **stems,
                            size_t *stem_count,
                            char *error,
                            size_t error_size)
{
    HWAHTDemucsContext *context = (HWAHTDemucsContext *)context_value;
    HWAHTDemucsResults *results = NULL;
    HWAWavReader reader;
    float *input = NULL;
    float *output = NULL;
    float *tail = NULL;
    unsigned char *raw = NULL;
    uint64_t work;
    uint64_t deadline_started = 0U;
    uint64_t loaded_frames = 0U;
    uint64_t window_count;
    uint64_t window_index;
    uint64_t written_frames = 0U;
    size_t previous_tail_frames = 0U;
    size_t raw_bytes;
    uint64_t output_data_bytes;
    uint64_t expected_size;
    int reader_open = 0;
    int status = -1;
    (void)seed;
    memset(&reader, 0, sizeof(reader));
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (stems != NULL) *stems = NULL;
    if (stem_count != NULL) *stem_count = 0U;
    if (context == NULL || source == NULL || source->read_at == NULL ||
        source_format == NULL || stems == NULL || stem_count == NULL ||
        timeout_milliseconds == 0U ||
        source_format->sample_rate_hz != HWA_HTDEMUCS_SAMPLE_RATE ||
        (source_format->channels != 1U && source_format->channels != 2U) ||
        source_format->block_align == 0U || source_format->frames == 0U ||
        sizeof(float) != 4U || FLT_RADIX != 2 || FLT_MANT_DIG != 24 ||
        FLT_MAX_EXP != 128) {
        hwa_htdemucs_error(
            error, error_size,
            "HTDemucs requires a mono or stereo 44100 Hz WAVE source");
        return -1;
    }
    if (source_format->frames >
        (uint64_t)(UINT32_MAX - UINT32_C(36)) /
            HWA_HTDEMUCS_OUTPUT_BLOCK_ALIGN) {
        hwa_htdemucs_error(
            error, error_size,
            "HTDemucs source exceeds the RIFF stem frame limit");
        return -1;
    }
    raw_bytes = HWA_HTDEMUCS_IO_BLOCK_FRAMES *
                (size_t)source_format->block_align;
    work = context->persistent_work_bytes;
    if (hwa_htdemucs_work_add(
            &work, HWA_HTDEMUCS_INPUT_CELLS, sizeof(float),
            context->max_work_bytes) != 0 ||
        hwa_htdemucs_work_add(
            &work, HWA_HTDEMUCS_OUTPUT_CELLS, sizeof(float),
            context->max_work_bytes) != 0 ||
        hwa_htdemucs_work_add(
            &work,
            HWA_HTDEMUCS_STEM_COUNT * HWA_HTDEMUCS_CHANNELS *
                HWA_HTDEMUCS_OVERLAP_SAMPLES,
            sizeof(float), context->max_work_bytes) != 0 ||
        hwa_htdemucs_work_add(
            &work, raw_bytes, 1U, context->max_work_bytes) != 0 ||
        hwa_htdemucs_work_add(
            &work, 1U, sizeof(HWAHTDemucsResults),
            context->max_work_bytes) != 0) {
        hwa_htdemucs_error(
            error, error_size, "HTDemucs adapter exceeds its work limit");
        return -1;
    }
    if (hwa_inference_deadline_start(
            &deadline_started, error, error_size) != 0 ||
        hwa_inference_deadline_check(
            deadline_started, timeout_milliseconds,
            error, error_size) != 0)
        return -1;
    if (hwa_wav_reader_open_source(
            &reader, source, source->size, error, error_size) != 0)
        return -1;
    reader_open = 1;
    if (!hwa_htdemucs_format_equal(&reader.format, source_format) ||
        reader.format.sample_rate_hz != HWA_HTDEMUCS_SAMPLE_RATE ||
        (reader.format.channels != 1U && reader.format.channels != 2U)) {
        hwa_htdemucs_error(
            error, error_size,
            "HTDemucs source format does not match the WAVE bytes");
        goto cleanup;
    }
    input = (float *)calloc(HWA_HTDEMUCS_INPUT_CELLS, sizeof(*input));
    output = (float *)malloc(
        HWA_HTDEMUCS_OUTPUT_CELLS * sizeof(*output));
    tail = (float *)malloc(
        HWA_HTDEMUCS_STEM_COUNT * HWA_HTDEMUCS_CHANNELS *
        HWA_HTDEMUCS_OVERLAP_SAMPLES * sizeof(*tail));
    raw = (unsigned char *)malloc(raw_bytes);
    if (input == NULL || output == NULL || tail == NULL || raw == NULL) {
        hwa_htdemucs_error(
            error, error_size, "cannot allocate HTDemucs adapter work");
        goto cleanup;
    }
    results = hwa_htdemucs_results_create(
        source_format->frames, error, error_size);
    if (results == NULL) goto cleanup;
    window_count =
        (source_format->frames + HWA_HTDEMUCS_STRIDE_SAMPLES - 1U) /
        HWA_HTDEMUCS_STRIDE_SAMPLES;
    for (window_index = 0U; window_index < window_count; ++window_index) {
        uint64_t chunk_start =
            window_index * (uint64_t)HWA_HTDEMUCS_STRIDE_SAMPLES;
        uint64_t remaining = source_format->frames - chunk_start;
        size_t chunk_frames = remaining < HWA_HTDEMUCS_INPUT_SAMPLES
                                  ? (size_t)remaining
                                  : HWA_HTDEMUCS_INPUT_SAMPLES;
        size_t stable_end;
        size_t output_index;
        if (hwa_inference_deadline_check(
                deadline_started, timeout_milliseconds,
                error, error_size) != 0)
            goto cleanup;
        if (window_index == 0U) {
            size_t wanted = chunk_frames;
            if (hwa_htdemucs_decode_frames(
                    &reader, input, 0U, wanted, raw,
                    error, error_size) != 0)
                goto cleanup;
            loaded_frames += (uint64_t)wanted;
        } else {
            size_t channel;
            uint64_t unread = source_format->frames - loaded_frames;
            size_t wanted = unread < HWA_HTDEMUCS_STRIDE_SAMPLES
                                ? (size_t)unread
                                : HWA_HTDEMUCS_STRIDE_SAMPLES;
            for (channel = 0U; channel < HWA_HTDEMUCS_CHANNELS; ++channel) {
                float *plane = input +
                               channel * HWA_HTDEMUCS_INPUT_SAMPLES;
                memmove(plane,
                        plane + HWA_HTDEMUCS_STRIDE_SAMPLES,
                        HWA_HTDEMUCS_OVERLAP_SAMPLES * sizeof(*plane));
                memset(plane + HWA_HTDEMUCS_OVERLAP_SAMPLES, 0,
                       HWA_HTDEMUCS_STRIDE_SAMPLES * sizeof(*plane));
            }
            if (hwa_htdemucs_decode_frames(
                    &reader, input, HWA_HTDEMUCS_OVERLAP_SAMPLES,
                    wanted, raw, error, error_size) != 0)
                goto cleanup;
            loaded_frames += (uint64_t)wanted;
        }
        for (output_index = 0U;
             output_index < HWA_HTDEMUCS_OUTPUT_CELLS;
             ++output_index)
            output[output_index] = NAN;
        if (context->run_window(
                context->model_context, input, output,
                error, error_size) != 0) {
            if (error != NULL && error_size != 0U && error[0] == '\0')
                hwa_htdemucs_error(
                    error, error_size, "HTDemucs model runner failed");
            goto cleanup;
        }
        if (!hwa_htdemucs_output_valid(output)) {
            hwa_htdemucs_error(
                error, error_size,
                "HTDemucs model returned a non-finite sample");
            goto cleanup;
        }
        if (hwa_inference_deadline_check(
                deadline_started, timeout_milliseconds,
                error, error_size) != 0)
            goto cleanup;
        if (window_index == 0U) {
            stable_end = chunk_frames < HWA_HTDEMUCS_STRIDE_SAMPLES
                             ? chunk_frames
                             : HWA_HTDEMUCS_STRIDE_SAMPLES;
            if (hwa_htdemucs_emit_raw(
                    results, output, 0U, stable_end, 1,
                    error, error_size) != 0)
                goto cleanup;
            written_frames += (uint64_t)stable_end;
        } else {
            size_t overlap_frames =
                chunk_frames < HWA_HTDEMUCS_OVERLAP_SAMPLES
                    ? chunk_frames : HWA_HTDEMUCS_OVERLAP_SAMPLES;
            if (overlap_frames != previous_tail_frames) {
                hwa_htdemucs_error(
                    error, error_size,
                    "HTDemucs overlap schedule is inconsistent");
                goto cleanup;
            }
            if (hwa_htdemucs_emit_overlap(
                    results, tail, output, overlap_frames,
                    error, error_size) != 0)
                goto cleanup;
            written_frames += (uint64_t)overlap_frames;
            stable_end = chunk_frames < HWA_HTDEMUCS_STRIDE_SAMPLES
                             ? chunk_frames
                             : HWA_HTDEMUCS_STRIDE_SAMPLES;
            if (stable_end > overlap_frames &&
                hwa_htdemucs_emit_raw(
                    results, output, overlap_frames,
                    stable_end - overlap_frames, 0,
                    error, error_size) != 0)
                goto cleanup;
            written_frames += (uint64_t)(stable_end - overlap_frames);
        }
        previous_tail_frames = 0U;
        if (window_index + 1U < window_count) {
            if (chunk_frames <= HWA_HTDEMUCS_STRIDE_SAMPLES) {
                hwa_htdemucs_error(
                    error, error_size,
                    "HTDemucs window schedule ended before its overlap");
                goto cleanup;
            }
            previous_tail_frames =
                chunk_frames - HWA_HTDEMUCS_STRIDE_SAMPLES;
            hwa_htdemucs_save_tail(
                tail, output, previous_tail_frames);
        }
    }
    if (loaded_frames != source_format->frames ||
        reader.bytes_remaining != 0U ||
        written_frames != source_format->frames) {
        hwa_htdemucs_error(
            error, error_size, "HTDemucs window schedule did not trim exactly");
        goto cleanup;
    }
    output_data_bytes =
        source_format->frames * HWA_HTDEMUCS_OUTPUT_BLOCK_ALIGN;
    expected_size = HWA_HTDEMUCS_WAVE_HEADER_BYTES + output_data_bytes;
    if (hwa_htdemucs_finalize_results(
            results, expected_size, error, error_size) != 0 ||
        hwa_inference_deadline_check(
            deadline_started, timeout_milliseconds,
            error, error_size) != 0)
        goto cleanup;
    *stems = results->stems;
    *stem_count = HWA_HTDEMUCS_STEM_COUNT;
    results = NULL;
    status = 0;
cleanup:
    hwa_htdemucs_results_destroy(
        NULL, results != NULL ? results->stems : NULL,
        results != NULL ? HWA_HTDEMUCS_STEM_COUNT : 0U);
    free(raw);
    free(tail);
    free(output);
    free(input);
    if (reader_open) hwa_wav_reader_close(&reader);
    return status;
}

static void hwa_htdemucs_destroy(void *context_value)
{
    HWAHTDemucsContext *context = (HWAHTDemucsContext *)context_value;
    if (context == NULL) return;
    if (context->model_destroy != NULL)
        context->model_destroy(context->model_context);
    free(context->runtime_name);
    free(context->runtime_version);
    free(context->backend);
    free(context->fallback);
    free(context);
}

int hwa_htdemucs_instrument_runner_init(
    HWAInstrumentStemRunner *out,
    uint64_t max_work_bytes,
    const HWAHTDemucsModelRunner *model_runner,
    char *error,
    size_t error_size)
{
    HWAHTDemucsContext *context = NULL;
    uint64_t persistent_work = 0U;
    int status = -1;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (out == NULL) {
        hwa_htdemucs_error(
            error, error_size, "HTDemucs instrument runner output is null");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (max_work_bytes == 0U || model_runner == NULL ||
        model_runner->run_window == NULL ||
        (model_runner->context != NULL && model_runner->destroy == NULL) ||
        hwa_htdemucs_text_size(
            model_runner->runtime_name, 0, NULL) != 0 ||
        hwa_htdemucs_text_size(
            model_runner->runtime_version, 0, NULL) != 0 ||
        hwa_htdemucs_text_size(model_runner->backend, 0, NULL) != 0 ||
        hwa_htdemucs_text_size(model_runner->fallback, 1, NULL) != 0 ||
        hwa_htdemucs_work_add(
            &persistent_work, 1U, sizeof(HWAHTDemucsContext),
            max_work_bytes) != 0 ||
        hwa_htdemucs_work_add_text(
            &persistent_work, model_runner->runtime_name, 0,
            max_work_bytes) != 0 ||
        hwa_htdemucs_work_add_text(
            &persistent_work, model_runner->runtime_version, 0,
            max_work_bytes) != 0 ||
        hwa_htdemucs_work_add_text(
            &persistent_work, model_runner->backend, 0,
            max_work_bytes) != 0 ||
        hwa_htdemucs_work_add_text(
            &persistent_work, model_runner->fallback, 1,
            max_work_bytes) != 0) {
        hwa_htdemucs_error(
            error, error_size, "invalid HTDemucs instrument runner arguments");
        return -1;
    }
    context = (HWAHTDemucsContext *)calloc(1U, sizeof(*context));
    if (context == NULL) goto allocation_failed;
    context->runtime_name = hwa_htdemucs_copy_text(
        model_runner->runtime_name, 0);
    context->runtime_version = hwa_htdemucs_copy_text(
        model_runner->runtime_version, 0);
    context->backend = hwa_htdemucs_copy_text(
        model_runner->backend, 0);
    context->fallback = hwa_htdemucs_copy_text(
        model_runner->fallback, 1);
    if (context->runtime_name == NULL || context->runtime_version == NULL ||
        context->backend == NULL || context->fallback == NULL)
        goto allocation_failed;
    context->max_work_bytes = max_work_bytes;
    context->persistent_work_bytes = persistent_work;
    context->model_context = model_runner->context;
    context->run_window = model_runner->run_window;
    context->model_destroy = model_runner->destroy;
    out->context = context;
    out->runtime_name = context->runtime_name;
    out->runtime_version = context->runtime_version;
    out->backend = context->backend;
    out->fallback = context->fallback;
    out->run = hwa_htdemucs_run;
    out->results_destroy = hwa_htdemucs_results_destroy;
    out->destroy = hwa_htdemucs_destroy;
    status = 0;
    goto cleanup;

allocation_failed:
    hwa_htdemucs_error(
        error, error_size, "cannot allocate HTDemucs instrument runner");
cleanup:
    if (status != 0 && context != NULL) {
        free(context->runtime_name);
        free(context->runtime_version);
        free(context->backend);
        free(context->fallback);
        free(context);
    }
    return status;
}

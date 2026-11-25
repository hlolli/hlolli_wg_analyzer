#include "instrument_isolation_file.h"

#include "event_bundle.h"
#include "inference_provider.h"
#include "internal.h"
#include "sha256.h"

#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct HwaWavSnapshot {
    const unsigned char *bytes;
    size_t size;
} HwaWavSnapshot;

typedef struct HwaIsolationWave {
    unsigned char header[80];
    size_t header_size;
    uint64_t file_size;
    const double *samples;
} HwaIsolationWave;

typedef struct HwaIsolationArtifact {
    char *instrument;
    char *relative_path;
    char *provider_name;
    char *method_version;
} HwaIsolationArtifact;

static const HwaIsolationArtifact hwa_isolation_artifacts[] = {
    [HWA_ISOLATION_VIOLIN] = {
        "violin", "audio/violin.wav", "hwa.violin-isolation", "violin-isolation-3"
    },
    [HWA_ISOLATION_VIOLA] = {
        "viola", "audio/viola.wav", "hwa.viola-isolation", "viola-isolation-2"
    },
    [HWA_ISOLATION_CELLO] = {
        "cello", "audio/cello.wav", "hwa.cello-isolation", "cello-isolation-2"
    },
    [HWA_ISOLATION_DOUBLE_BASS] = {
        "double-bass", "audio/double-bass.wav", "hwa.double-bass-isolation",
        "double-bass-isolation-1"
    },
    [HWA_ISOLATION_PIANO] = {
        "piano", "audio/piano.wav", "hwa.piano-isolation", "piano-isolation-1"
    }
};

int hwa_isolation_instrument_from_name(const char *name,
                                       HwaIsolationInstrument *instrument)
{
    size_t index;
    if (name == NULL || instrument == NULL) return -1;
    for (index = 0U; index < sizeof(hwa_isolation_artifacts) / sizeof(hwa_isolation_artifacts[0]);
         ++index) {
        if (strcmp(name, hwa_isolation_artifacts[index].instrument) == 0) {
            *instrument = (HwaIsolationInstrument)index;
            return 0;
        }
    }
    return -1;
}

static int hwa_isolation_supports_float32(void)
{
    return sizeof(float) == 4U && FLT_RADIX == 2 &&
           FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128;
}

static int hwa_isolation_snapshot_read(void *context, uint64_t offset,
                                       unsigned char *destination, size_t size)
{
    const HwaWavSnapshot *snapshot = (const HwaWavSnapshot *)context;
    if (offset > snapshot->size || size > snapshot->size - (size_t)offset)
        return -1;
    if (size != 0U)
        memcpy(destination, snapshot->bytes + (size_t)offset, size);
    return 0;
}

static int hwa_isolation_read_source(const char *path,
                                      const HWAAnalysisOptions *limits,
                                      HWAEventAudio *audio,
                                      double **samples,
                                      char *error,
                                      size_t error_size)
{
    unsigned char *bytes = NULL;
    size_t size = 0U;
    HwaWavSnapshot snapshot;
    HWAByteSource source;
    HWAWavReader reader;
    size_t count;
    size_t sample;
    int status = -1;
    uint64_t byte_limit = limits->max_input_bytes;
    if (byte_limit > limits->max_work_bytes)
        byte_limit = limits->max_work_bytes;
    if (hwa_sha256_read_file(path, byte_limit, &bytes, &size, audio->sha256,
                            error, error_size) != 0)
        return -1;
    snapshot = (HwaWavSnapshot){bytes, size};
    source = (HWAByteSource){&snapshot, path, size, hwa_isolation_snapshot_read};
    if (hwa_wav_reader_open_source(&reader, &source, byte_limit,
                                   error, error_size) != 0) {
        free(bytes);
        return -1;
    }
    audio->format = reader.format;
    audio->file_bytes = size;
    if (reader.format.channels > 2U) {
        hwa_set_error(error, error_size, "instrument isolation needs mono or stereo WAVE input");
        goto cleanup;
    }
    if (reader.format.frames == 0U ||
        reader.format.frames > limits->max_input_frames ||
        reader.format.frames > SIZE_MAX / reader.format.channels / sizeof(double)) {
        hwa_set_error(error, error_size, "instrument isolation WAVE frames are empty or exceed the frame limit");
        goto cleanup;
    }
    count = (size_t)reader.format.frames * reader.format.channels;
    if (count > (limits->max_work_bytes - (uint64_t)size) / sizeof(double)) {
        hwa_set_error(error, error_size, "instrument isolation samples exceed the work limit");
        goto cleanup;
    }
    *samples = (double *)malloc(count * sizeof(double));
    if (*samples == NULL) {
        hwa_set_error(error, error_size, "cannot allocate instrument source samples");
        goto cleanup;
    }
    for (sample = 0U; sample < count; ++sample) {
        int clipped;
        (*samples)[sample] = hwa_wav_decode_sample(
            &reader, bytes + (size_t)reader.data_offset +
                         sample * reader.bytes_per_sample, &clipped);
    }
    status = 0;
cleanup:
    hwa_wav_reader_close(&reader);
    free(bytes);
    return status;
}

static void hwa_isolation_store_u16(unsigned char *destination, uint16_t value)
{
    destination[0] = (unsigned char)value;
    destination[1] = (unsigned char)(value >> 8U);
}

static void hwa_isolation_store_u32(unsigned char *destination, uint32_t value)
{
    destination[0] = (unsigned char)value;
    destination[1] = (unsigned char)(value >> 8U);
    destination[2] = (unsigned char)(value >> 16U);
    destination[3] = (unsigned char)(value >> 24U);
}

static int hwa_isolation_wave_header(HwaIsolationWave *wave,
                                      const HWAFormat *format)
{
    static const unsigned char ieee_float_guid[16] = {
        3U, 0U, 0U, 0U, 0U, 0U, 16U, 0U,
        128U, 0U, 0U, 170U, 0U, 56U, 155U, 113U
    };
    unsigned char *header = wave->header;
    uint32_t fmt_size = format->channel_mask == 0U ? 18U : 40U;
    size_t fact_offset = 20U + fmt_size;
    size_t header_size = fact_offset + 20U;
    uint16_t block_align = (uint16_t)(format->channels * 4U);
    uint64_t byte_rate = (uint64_t)format->sample_rate_hz * block_align;
    uint64_t data_size;
    if (format->frames > (UINT32_MAX - (header_size - 8U)) / block_align ||
        byte_rate > UINT32_MAX)
        return -1;
    data_size = format->frames * block_align;
    wave->header_size = header_size;
    wave->file_size = (uint64_t)header_size + data_size;
    memset(header, 0, header_size);
    memcpy(header, "RIFF", 4U);
    hwa_isolation_store_u32(header + 4U, (uint32_t)(wave->file_size - 8U));
    memcpy(header + 8U, "WAVEfmt ", 8U);
    hwa_isolation_store_u32(header + 16U, fmt_size);
    hwa_isolation_store_u16(header + 20U,
                            format->channel_mask == 0U ? 3U : UINT16_MAX - 1U);
    hwa_isolation_store_u16(header + 22U, format->channels);
    hwa_isolation_store_u32(header + 24U, format->sample_rate_hz);
    hwa_isolation_store_u32(header + 28U, (uint32_t)byte_rate);
    hwa_isolation_store_u16(header + 32U, block_align);
    hwa_isolation_store_u16(header + 34U, 32U);
    if (format->channel_mask != 0U) {
        hwa_isolation_store_u16(header + 36U, 22U);
        hwa_isolation_store_u16(header + 38U, 32U);
        hwa_isolation_store_u32(header + 40U, format->channel_mask);
        memcpy(header + 44U, ieee_float_guid, sizeof(ieee_float_guid));
    }
    memcpy(header + fact_offset, "fact", 4U);
    hwa_isolation_store_u32(header + fact_offset + 4U, 4U);
    hwa_isolation_store_u32(header + fact_offset + 8U, (uint32_t)format->frames);
    memcpy(header + fact_offset + 12U, "data", 4U);
    hwa_isolation_store_u32(header + fact_offset + 16U, (uint32_t)data_size);
    return 0;
}

static int hwa_isolation_wave_read(void *context, uint64_t offset,
                                    unsigned char *destination, size_t size)
{
    /* Encode on demand so the bundle writer needs no second audio buffer. */
    const HwaIsolationWave *wave = (const HwaIsolationWave *)context;
    uint64_t data_offset;
    if (offset > wave->file_size || size > wave->file_size - offset)
        return -1;
    if (size == 0U) return 0;
    if (offset < wave->header_size) {
        size_t count = wave->header_size - (size_t)offset;
        if (count > size) count = size;
        memcpy(destination, wave->header + (size_t)offset, count);
        offset += count;
        destination += count;
        size -= count;
    }
    if (size == 0U) return 0;
    data_offset = offset - wave->header_size;
    while (size != 0U) {
        unsigned char sample_bytes[4];
        size_t within_sample = (size_t)(data_offset % 4U);
        size_t count = 4U - within_sample;
        float sample = (float)wave->samples[(size_t)(data_offset / 4U)];
        uint32_t bits;
        memcpy(&bits, &sample, sizeof(bits));
        hwa_isolation_store_u32(sample_bytes, bits);
        if (count > size) count = size;
        memcpy(destination, sample_bytes + within_sample, count);
        data_offset += count;
        destination += count;
        size -= count;
    }
    return 0;
}

static char *hwa_isolation_settings(const char *instrument,
                                    const HwaIsolationRange *ranges,
                                    size_t range_count,
                                    const HWAEventBundleLimits *limits,
                                    char *error,
                                    size_t error_size)
{
    char prefix[80];
    /* Two uint64_t decimals, brackets, and at most two commas per range. */
    const size_t range_capacity = 44U;
    size_t capacity;
    size_t offset;
    size_t range;
    char *settings;
    int prefix_size = snprintf(prefix, sizeof(prefix),
                               "{\"instrument\":\"%s\",\"disturbance_samples\":[",
                               instrument);
    if (prefix_size < 0 || (size_t)prefix_size >= sizeof(prefix)) {
        hwa_set_error(error, error_size, "cannot encode isolation instrument");
        return NULL;
    }
    offset = (size_t)prefix_size;
    if (range_count > (SIZE_MAX - offset - 3U) / range_capacity) {
        hwa_set_error(error, error_size, "instrument isolation settings size overflows");
        return NULL;
    }
    capacity = offset + range_count * range_capacity + 3U;
    if (capacity > limits->max_manifest_bytes ||
        capacity > limits->max_work_bytes) {
        hwa_set_error(error, error_size, "instrument isolation settings exceed the bundle limits");
        return NULL;
    }
    settings = (char *)malloc(capacity);
    if (settings == NULL) {
        hwa_set_error(error, error_size, "cannot allocate instrument isolation settings");
        return NULL;
    }
    memcpy(settings, prefix, offset);
    for (range = 0U; range < range_count; ++range) {
        int written = snprintf(settings + offset, capacity - offset,
                               "%s[%" PRIu64 ",%" PRIu64 "]",
                               range == 0U ? "" : ",",
                               ranges[range].start_sample,
                               ranges[range].end_sample);
        if (written < 0 || (size_t)written >= capacity - offset) {
            free(settings);
            hwa_set_error(error, error_size, "cannot encode instrument isolation settings");
            return NULL;
        }
        offset += (size_t)written;
    }
    memcpy(settings + offset, "]}", 3U);
    return settings;
}

int hwa_isolate_instrument_wav(const char *source_path,
                               const char *output_directory,
                               HwaIsolationInstrument instrument,
                               const HwaIsolationRange *ranges,
                               size_t range_count,
                               const HWAAnalysisOptions *input_limits,
                               const HWAEventBundleLimits *bundle_limits,
                               char *error,
                               size_t error_size)
{
    HWAEventAudio audio[3] = {
        {.id = 1U, .kind = HWA_EVENT_SOURCE_RECORDING, .name = "source",
         .relative_path = "", .path_hint = ""},
        {.id = 2U, .kind = HWA_EVENT_INSTRUMENT_STEM, .path_hint = "",
         .source_recording_id = 1U, .source_recording_id_valid = 1},
        {.id = 3U, .kind = HWA_EVENT_DERIVED_AUDIO, .name = "removed",
         .relative_path = "audio/removed.wav", .path_hint = "",
         .source_recording_id = 1U, .source_recording_id_valid = 1}
    };
    HwaIsolationWave waves[2];
    HWAEventSourceBinding bindings[2];
    HWAEventProvider provider = {.id = 1U};
    HWAEventValue value = {
        .name = "instrument", .kind = HWA_EVENT_VALUE_TEXT,
        .basis = HWA_EVENT_INFERENCE, .unit = "",
        .provider_id = 1U, .provider_id_valid = 1, .selected = 1
    };
    HWAPerformanceEvent event = {
        .id = 1U, .kind = "instrument-region", .source_recording_id = 1U,
        .evidence_audio_id = 2U, .evidence_audio_id_valid = 1,
        .voice = "", .score_event_id = "",
        .values = &value, .value_count = 1U
    };
    HWAEventBundle bundle = {
        .providers = &provider, .provider_count = 1U,
        .audio = audio, .audio_count = 3U,
        .events = &event, .event_count = 1U
    };
    const HwaIsolationArtifact *artifact;
    HWAEventBundleLimits write_limits;
    char current_source_sha256[HWA_SHA256_HEX_SIZE];
    double *input = NULL;
    double *estimate = NULL;
    size_t count;
    size_t index;
    uint64_t work_available;
    int result = -1;
    if (source_path == NULL || source_path[0] == '\0' ||
        output_directory == NULL || output_directory[0] == '\0' ||
        (size_t)instrument >= sizeof(hwa_isolation_artifacts) / sizeof(hwa_isolation_artifacts[0]) ||
        (range_count != 0U && ranges == NULL) ||
        input_limits == NULL || bundle_limits == NULL ||
        input_limits->max_input_bytes == 0U ||
        input_limits->max_input_frames == 0U ||
        input_limits->max_work_bytes == 0U) {
        hwa_set_error(error, error_size, "invalid instrument isolation file arguments");
        return -1;
    }
    artifact = &hwa_isolation_artifacts[instrument];
    audio[1].name = artifact->instrument;
    audio[1].relative_path = artifact->relative_path;
    provider.name = artifact->provider_name;
    provider.version = artifact->method_version;
    value.text = artifact->instrument;
    event.part = artifact->instrument;
    if (hwa_isolation_read_source(source_path, input_limits, &audio[0],
                                   &input, error, error_size) != 0)
        goto cleanup;
    event.end_sample = audio[0].format.frames;
    count = (size_t)audio[0].format.frames * audio[0].format.channels;
    if (count > (input_limits->max_work_bytes - 1U) / (2U * sizeof(double))) {
        hwa_set_error(error, error_size, "instrument isolation buffers exceed the work limit");
        goto cleanup;
    }
    work_available = input_limits->max_work_bytes -
                     (uint64_t)count * (2U * sizeof(double));
    if (!hwa_isolation_supports_float32()) {
        hwa_set_error(error, error_size, "instrument isolation requires IEEE binary32 float");
        goto cleanup;
    }
    if (hwa_isolation_wave_header(&waves[0], &audio[0].format) != 0 ||
        waves[0].file_size > bundle_limits->max_payload_file_bytes ||
        waves[0].file_size > bundle_limits->max_bundle_bytes / 2U) {
        hwa_set_error(error, error_size, "instrument isolation WAVE output exceeds RIFF or bundle limits");
        goto cleanup;
    }
    estimate = (double *)malloc(count * sizeof(double));
    if (estimate == NULL) {
        hwa_set_error(error, error_size, "cannot allocate instrument estimate samples");
        goto cleanup;
    }
    if (hwa_isolate_instrument(input, (size_t)audio[0].format.frames,
                               audio[0].format.channels, audio[0].format.sample_rate_hz,
                               instrument, ranges, range_count, work_available,
                               estimate, error, error_size) != 0)
        goto cleanup;
    /* Subtract the delivered float estimate to keep the two outputs additive. */
    for (index = 0U; index < count; ++index) {
        if (!isfinite(estimate[index]) || fabs(estimate[index]) > FLT_MAX) {
            hwa_set_error(error, error_size, "instrument isolation produced an out-of-range estimate");
            goto cleanup;
        }
        input[index] -= (double)(float)estimate[index];
        if (!isfinite(input[index]) || fabs(input[index]) > FLT_MAX) {
            hwa_set_error(error, error_size, "instrument isolation produced out-of-range removed audio");
            goto cleanup;
        }
    }
    waves[0].samples = estimate;
    waves[1] = waves[0];
    waves[1].samples = input;
    write_limits = *bundle_limits;
    if (write_limits.max_work_bytes > work_available)
        write_limits.max_work_bytes = work_available;
    provider.settings_json = hwa_isolation_settings(artifact->instrument, ranges, range_count,
                                                      &write_limits,
                                                      error, error_size);
    if (provider.settings_json == NULL) goto cleanup;
    for (index = 0U; index < 2U; ++index) {
        HWAEventAudio *derived = &audio[index + 1U];
        HWAWavReader reader;
        bindings[index].relative_path = derived->relative_path;
        bindings[index].source = (HWAByteSource){
            &waves[index], derived->relative_path, waves[index].file_size,
            hwa_isolation_wave_read
        };
        if (hwa_wav_reader_open_source(&reader, &bindings[index].source,
                                       write_limits.max_payload_file_bytes,
                                       error, error_size) != 0)
            goto cleanup;
        derived->format = reader.format;
        derived->file_bytes = waves[index].file_size;
        hwa_wav_reader_close(&reader);
        if (hwa_inference_byte_source_sha256(
                &bindings[index].source, write_limits.max_payload_file_bytes,
                derived->sha256, error, error_size) != 0)
            goto cleanup;
    }
    if (hwa_sha256_file(source_path, audio[0].file_bytes,
                        current_source_sha256, error, error_size) != 0)
        goto cleanup;
    if (strcmp(current_source_sha256, audio[0].sha256) != 0) {
        hwa_set_error(error, error_size,
                      "instrument isolation source changed before publication");
        goto cleanup;
    }
    result = hwa_event_bundle_write_sources(output_directory, &bundle,
                                             bindings, 2U, &write_limits,
                                             error, error_size);
cleanup:
    free(provider.settings_json);
    free(estimate);
    free(input);
    return result;
}

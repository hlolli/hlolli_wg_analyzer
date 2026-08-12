#include "internal.h"

#include <float.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define HWA_DS64_MAX_TABLE_ENTRIES 65536U

typedef struct HWADs64Entry {
    unsigned char id[4];
    uint64_t size;
    int used;
} HWADs64Entry;

typedef struct HWADs64 {
    uint64_t riff_size;
    uint64_t data_size;
    uint64_t sample_count;
    HWADs64Entry *entries;
    uint32_t entry_count;
    uint64_t next_chunk;
} HWADs64;

static uint16_t hwa_le16(const unsigned char *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] |
                      ((uint16_t)bytes[1] << 8U));
}

static uint32_t hwa_le32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static uint64_t hwa_le64(const unsigned char *bytes)
{
    return (uint64_t)bytes[0] |
           ((uint64_t)bytes[1] << 8U) |
           ((uint64_t)bytes[2] << 16U) |
           ((uint64_t)bytes[3] << 24U) |
           ((uint64_t)bytes[4] << 32U) |
           ((uint64_t)bytes[5] << 40U) |
           ((uint64_t)bytes[6] << 48U) |
           ((uint64_t)bytes[7] << 56U);
}

void hwa_set_error(char *error, size_t error_size, const char *format, ...)
{
    va_list arguments;

    if (error == NULL || error_size == 0U) {
        return;
    }

    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
    error[error_size - 1U] = '\0';
}

static int hwa_read_exact(const HWAByteSource *source,
                          uint64_t offset,
                          void *buffer,
                          size_t size)
{
    if (source == NULL || source->read_at == NULL ||
        (size != 0U && buffer == NULL) ||
        (uint64_t)size > source->size ||
        offset > source->size - (uint64_t)size) {
        return 0;
    }
    return size == 0U ||
           source->read_at(source->context, offset,
                           (unsigned char *)buffer, size) == 0;
}

static int hwa_guid_matches(const unsigned char *guid, uint32_t tag)
{
    static const unsigned char guid_tail[12] = {
        0x00U, 0x00U, 0x10U, 0x00U,
        0x80U, 0x00U, 0x00U, 0xaaU,
        0x00U, 0x38U, 0x9bU, 0x71U
    };

    return hwa_le32(guid) == tag &&
           memcmp(guid + 4U, guid_tail, sizeof(guid_tail)) == 0;
}

static unsigned hwa_bit_count32(uint32_t value)
{
    unsigned count = 0U;

    while (value != 0U) {
        count += value & 1U;
        value >>= 1U;
    }
    return count;
}

static int hwa_parse_format(const unsigned char *bytes,
                            uint64_t size,
                            HWAFormat *format,
                            char *error,
                            size_t error_size)
{
    uint16_t tag;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits;
    uint16_t valid_bits;
    uint32_t channel_mask = 0U;
    uint64_t expected_align;
    uint64_t expected_rate;
    int extensible = 0;

    if (size < 16U) {
        hwa_set_error(error, error_size, "fmt chunk is shorter than 16 bytes");
        return -1;
    }

    tag = hwa_le16(bytes);
    channels = hwa_le16(bytes + 2U);
    sample_rate = hwa_le32(bytes + 4U);
    byte_rate = hwa_le32(bytes + 8U);
    block_align = hwa_le16(bytes + 12U);
    bits = hwa_le16(bytes + 14U);
    valid_bits = bits;

    if (tag == 0xfffeU) {
        uint16_t extension_size;

        extensible = 1;
        if (size < 40U) {
            hwa_set_error(error, error_size,
                          "extensible fmt chunk is shorter than 40 bytes");
            return -1;
        }
        extension_size = hwa_le16(bytes + 16U);
        valid_bits = hwa_le16(bytes + 18U);
        channel_mask = hwa_le32(bytes + 20U);
        if (extension_size < 22U) {
            hwa_set_error(error, error_size,
                          "extensible fmt chunk has an invalid extension size");
            return -1;
        }
        if ((uint64_t)extension_size > size - 18U) {
            hwa_set_error(error, error_size,
                          "extensible fmt extension exceeds its chunk size");
            return -1;
        }
        if (hwa_guid_matches(bytes + 24U, 1U)) {
            tag = 1U;
        } else if (hwa_guid_matches(bytes + 24U, 3U)) {
            tag = 3U;
        } else {
            hwa_set_error(error, error_size,
                          "unsupported extensible WAVE subformat");
            return -1;
        }
    }

    if (tag != 1U && tag != 3U) {
        hwa_set_error(error, error_size,
                      "unsupported WAVE encoding tag %u", (unsigned)tag);
        return -1;
    }
    if (channels == 0U || channels > HWA_MAX_CHANNELS) {
        hwa_set_error(error, error_size,
                      "invalid or excessive channel count %u", (unsigned)channels);
        return -1;
    }
    if (sample_rate == 0U) {
        hwa_set_error(error, error_size, "sample rate must be nonzero");
        return -1;
    }
    if (bits == 0U || (bits & 7U) != 0U) {
        hwa_set_error(error, error_size,
                      "sample container size must use whole bytes");
        return -1;
    }
    if (tag == 1U && bits != 8U && bits != 16U &&
        bits != 24U && bits != 32U) {
        hwa_set_error(error, error_size,
                      "unsupported PCM bit depth %u", (unsigned)bits);
        return -1;
    }
    if (tag == 3U && bits != 32U && bits != 64U) {
        hwa_set_error(error, error_size,
                      "IEEE float WAVE must use 32-bit or 64-bit samples");
        return -1;
    }
    if (valid_bits == 0U || valid_bits > bits) {
        hwa_set_error(error, error_size,
                      "valid sample bits must be between 1 and the container size");
        return -1;
    }
    if (tag == 3U && valid_bits != bits) {
        hwa_set_error(error, error_size,
                      "IEEE float valid bits must match its container size");
        return -1;
    }
    if (tag == 3U && bits == 32U &&
        (sizeof(float) != 4U || FLT_RADIX != 2 ||
         FLT_MANT_DIG != 24 || FLT_MAX_EXP != 128)) {
        hwa_set_error(error, error_size,
                      "this platform does not provide IEEE binary32 float");
        return -1;
    }
    if (tag == 3U && bits == 64U &&
        (sizeof(double) != 8U || FLT_RADIX != 2 ||
         DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024)) {
        hwa_set_error(error, error_size,
                      "this platform does not provide IEEE binary64 float");
        return -1;
    }
    if (extensible && channel_mask != 0U &&
        hwa_bit_count32(channel_mask) != (unsigned)channels) {
        hwa_set_error(error, error_size,
                      "channel mask does not match the channel count");
        return -1;
    }

    expected_align = (uint64_t)channels * ((uint64_t)bits / 8U);
    expected_rate = (uint64_t)sample_rate * expected_align;
    if (expected_align > UINT16_MAX || block_align != (uint16_t)expected_align) {
        hwa_set_error(error, error_size,
                      "WAVE block alignment does not match its format");
        return -1;
    }
    if (expected_rate > UINT32_MAX || byte_rate != (uint32_t)expected_rate) {
        hwa_set_error(error, error_size,
                      "WAVE byte rate does not match its format");
        return -1;
    }

    memset(format, 0, sizeof(*format));
    format->encoding = tag == 1U ? HWA_ENCODING_PCM : HWA_ENCODING_IEEE_FLOAT;
    format->channels = channels;
    format->sample_rate_hz = sample_rate;
    format->bits_per_sample = bits;
    format->valid_bits_per_sample = valid_bits;
    format->block_align = block_align;
    format->channel_mask = channel_mask;
    return 0;
}

static int hwa_chunk_end(uint64_t payload_offset,
                         uint64_t payload_size,
                         uint64_t container_end,
                         uint64_t *next_chunk,
                         char *error,
                         size_t error_size)
{
    uint64_t payload_end;

    if (payload_offset > container_end ||
        payload_size > container_end - payload_offset) {
        hwa_set_error(error, error_size,
                      "RIFF chunk extends past the declared container size");
        return -1;
    }
    payload_end = payload_offset + payload_size;
    if ((payload_size & 1U) != 0U) {
        if (payload_end == container_end) {
            hwa_set_error(error, error_size,
                          "odd RIFF chunk is missing its padding byte");
            return -1;
        }
        payload_end++;
    }
    *next_chunk = payload_end;
    return 0;
}

static int hwa_read_ds64(const HWAByteSource *source,
                         uint64_t file_size,
                         HWADs64 *ds64,
                         char *error,
                         size_t error_size)
{
    unsigned char header[8];
    unsigned char fixed[28];
    uint32_t chunk_size;
    uint32_t table_count;
    uint64_t required_size;
    uint64_t payload_offset = 20U;
    uint64_t next_chunk;
    uint32_t index;

    memset(ds64, 0, sizeof(*ds64));
    if (file_size < 20U ||
        !hwa_read_exact(source, 12U, header, sizeof(header))) {
        hwa_set_error(error, error_size,
                      "RF64 input is missing its leading ds64 chunk");
        return -1;
    }
    if (memcmp(header, "ds64", 4U) != 0) {
        hwa_set_error(error, error_size,
                      "RF64 input must place ds64 first");
        return -1;
    }
    chunk_size = hwa_le32(header + 4U);
    if (chunk_size == UINT32_MAX || chunk_size < sizeof(fixed)) {
        hwa_set_error(error, error_size,
                      "RF64 ds64 chunk has an invalid size");
        return -1;
    }
    if (hwa_chunk_end(payload_offset, chunk_size, file_size,
                      &next_chunk, error, error_size) != 0 ||
        !hwa_read_exact(source, payload_offset, fixed, sizeof(fixed))) {
        if (error != NULL && error_size > 0U && error[0] == '\0') {
            hwa_set_error(error, error_size, "truncated RF64 ds64 chunk");
        }
        return -1;
    }

    ds64->riff_size = hwa_le64(fixed);
    ds64->data_size = hwa_le64(fixed + 8U);
    ds64->sample_count = hwa_le64(fixed + 16U);
    table_count = hwa_le32(fixed + 24U);
    if (table_count > HWA_DS64_MAX_TABLE_ENTRIES) {
        hwa_set_error(error, error_size,
                      "RF64 ds64 table has too many entries");
        return -1;
    }
    required_size = 28U + (uint64_t)table_count * 12U;
    if (required_size > chunk_size) {
        hwa_set_error(error, error_size,
                      "RF64 ds64 table exceeds its chunk size");
        return -1;
    }
    if (table_count != 0U) {
        ds64->entries = (HWADs64Entry *)calloc(
            (size_t)table_count, sizeof(*ds64->entries));
        if (ds64->entries == NULL) {
            hwa_set_error(error, error_size,
                          "out of memory for the RF64 ds64 table");
            return -1;
        }
        for (index = 0U; index < table_count; ++index) {
            unsigned char entry[12];
            uint64_t entry_offset = payload_offset + UINT64_C(28) +
                                    (uint64_t)index * UINT64_C(12);

            if (!hwa_read_exact(source, entry_offset,
                                entry, sizeof(entry))) {
                hwa_set_error(error, error_size,
                              "truncated RF64 ds64 table");
                return -1;
            }
            memcpy(ds64->entries[index].id, entry, 4U);
            ds64->entries[index].size = hwa_le64(entry + 4U);
        }
    }
    ds64->entry_count = table_count;
    ds64->next_chunk = next_chunk;
    return 0;
}

static int hwa_rf64_chunk_size(HWADs64 *ds64,
                               const unsigned char id[4],
                               uint32_t raw_size,
                               uint64_t *resolved_size,
                               char *error,
                               size_t error_size)
{
    uint32_t index;

    if (memcmp(id, "data", 4U) == 0) {
        if (raw_size != UINT32_MAX) {
            hwa_set_error(error, error_size,
                          "RF64 data chunk is missing its size sentinel");
            return -1;
        }
        *resolved_size = ds64->data_size;
        return 0;
    }
    if (raw_size != UINT32_MAX) {
        *resolved_size = raw_size;
        return 0;
    }
    for (index = 0U; index < ds64->entry_count; ++index) {
        HWADs64Entry *entry = &ds64->entries[index];

        if (!entry->used && memcmp(entry->id, id, 4U) == 0) {
            entry->used = 1;
            *resolved_size = entry->size;
            return 0;
        }
    }
    hwa_set_error(error, error_size,
                  "RF64 chunk %.4s has no ds64 size entry", id);
    return -1;
}

int hwa_wav_reader_open_source(HWAWavReader *reader,
                               const HWAByteSource *source,
                               uint64_t max_input_bytes,
                               char *error,
                               size_t error_size)
{
    unsigned char riff_header[12];
    uint64_t file_size;
    uint64_t riff_end;
    uint64_t cursor;
    uint64_t data_offset = 0U;
    uint64_t data_bytes = 0U;
    HWAFormat format;
    HWADs64 ds64;
    HWAContainer container;
    int have_format = 0;
    int have_data = 0;
    int result = -1;

    if (error != NULL && error_size > 0U) {
        error[0] = '\0';
    }
    if (reader == NULL || source == NULL || source->read_at == NULL) {
        hwa_set_error(error, error_size, "invalid WAVE reader arguments");
        return -1;
    }
    memset(reader, 0, sizeof(*reader));
    memset(&format, 0, sizeof(format));
    memset(&ds64, 0, sizeof(ds64));

    reader->source = *source;
    file_size = source->size;
    if (max_input_bytes != 0U && file_size > max_input_bytes) {
        hwa_set_error(error, error_size,
                      "input size %llu exceeds the byte limit of %llu",
                      (unsigned long long)file_size,
                      (unsigned long long)max_input_bytes);
        goto cleanup;
    }
    if (file_size < sizeof(riff_header) ||
        !hwa_read_exact(&reader->source, 0U,
                        riff_header, sizeof(riff_header))) {
        hwa_set_error(error, error_size,
                      "input is shorter than a RIFF/WAVE header");
        goto cleanup;
    }
    if (memcmp(riff_header + 8U, "WAVE", 4U) != 0) {
        hwa_set_error(error, error_size,
                      "input is not a little-endian RIFF/WAVE file");
        goto cleanup;
    }

    if (memcmp(riff_header, "RIFF", 4U) == 0) {
        uint32_t riff_size = hwa_le32(riff_header + 4U);

        container = HWA_CONTAINER_RIFF;
        if (riff_size < 4U) {
            hwa_set_error(error, error_size, "RIFF size is too small");
            goto cleanup;
        }
        riff_end = 8U + (uint64_t)riff_size;
        cursor = 12U;
    } else if (memcmp(riff_header, "RF64", 4U) == 0) {
        container = HWA_CONTAINER_RF64;
        if (hwa_le32(riff_header + 4U) != UINT32_MAX) {
            hwa_set_error(error, error_size,
                          "RF64 header is missing its size sentinel");
            goto cleanup;
        }
        if (hwa_read_ds64(&reader->source, file_size, &ds64,
                          error, error_size) != 0) {
            goto cleanup;
        }
        if (ds64.riff_size < 4U || ds64.riff_size > UINT64_MAX - 8U) {
            hwa_set_error(error, error_size,
                          "RF64 ds64 RIFF size is invalid");
            goto cleanup;
        }
        riff_end = 8U + ds64.riff_size;
        cursor = ds64.next_chunk;
        if (cursor > riff_end) {
            hwa_set_error(error, error_size,
                          "RF64 ds64 chunk exceeds its declared RIFF size");
            goto cleanup;
        }
    } else {
        hwa_set_error(error, error_size,
                      "input is not a little-endian RIFF/WAVE or RF64 file");
        goto cleanup;
    }
    if (riff_end > file_size) {
        hwa_set_error(error, error_size,
                      "RIFF size extends past the end of the input");
        goto cleanup;
    }

    while (cursor < riff_end) {
        unsigned char chunk_header[8];
        uint32_t raw_size;
        uint64_t chunk_size;
        uint64_t payload_offset;
        uint64_t next_chunk;

        if (riff_end - cursor < sizeof(chunk_header) ||
            !hwa_read_exact(&reader->source, cursor,
                            chunk_header, sizeof(chunk_header))) {
            hwa_set_error(error, error_size, "truncated RIFF chunk header");
            goto cleanup;
        }
        raw_size = hwa_le32(chunk_header + 4U);
        payload_offset = cursor + sizeof(chunk_header);
        if (container == HWA_CONTAINER_RF64) {
            if (memcmp(chunk_header, "ds64", 4U) == 0) {
                hwa_set_error(error, error_size,
                              "RF64 input contains more than one ds64 chunk");
                goto cleanup;
            }
            if (hwa_rf64_chunk_size(&ds64, chunk_header, raw_size,
                                    &chunk_size, error, error_size) != 0) {
                goto cleanup;
            }
        } else {
            chunk_size = raw_size;
        }
        if (hwa_chunk_end(payload_offset, chunk_size, riff_end,
                          &next_chunk, error, error_size) != 0) {
            goto cleanup;
        }

        if (memcmp(chunk_header, "fmt ", 4U) == 0) {
            unsigned char format_bytes[40] = {0};
            size_t bytes_to_read = chunk_size < sizeof(format_bytes)
                                       ? (size_t)chunk_size
                                       : sizeof(format_bytes);

            if (have_format) {
                hwa_set_error(error, error_size,
                              "WAVE input contains more than one fmt chunk");
                goto cleanup;
            }
            if (!hwa_read_exact(&reader->source, payload_offset,
                                format_bytes, bytes_to_read)) {
                hwa_set_error(error, error_size, "truncated fmt chunk");
                goto cleanup;
            }
            if (hwa_parse_format(format_bytes, chunk_size, &format,
                                 error, error_size) != 0) {
                goto cleanup;
            }
            have_format = 1;
        } else if (memcmp(chunk_header, "data", 4U) == 0) {
            if (have_data) {
                hwa_set_error(error, error_size,
                              "WAVE input contains more than one data chunk");
                goto cleanup;
            }
            data_offset = payload_offset;
            data_bytes = chunk_size;
            have_data = 1;
        }
        cursor = next_chunk;
    }

    if (!have_format || !have_data) {
        hwa_set_error(error, error_size,
                      "WAVE input must contain one fmt and one data chunk");
        goto cleanup;
    }
    if (data_bytes % format.block_align != 0U) {
        hwa_set_error(error, error_size,
                      "data chunk is not a whole number of sample frames");
        goto cleanup;
    }

    format.container = container;
    format.data_bytes = data_bytes;
    format.frames = data_bytes / format.block_align;
    format.duration_seconds =
        (double)format.frames / (double)format.sample_rate_hz;
    if (container == HWA_CONTAINER_RF64 && ds64.sample_count != 0U &&
        ds64.sample_count != format.frames) {
        hwa_set_error(error, error_size,
                      "RF64 sample count does not match its data size");
        goto cleanup;
    }

    reader->format = format;
    reader->data_offset = data_offset;
    reader->cursor = data_offset;
    reader->bytes_remaining = data_bytes;
    reader->bytes_per_sample = (size_t)format.bits_per_sample / 8U;
    result = 0;

cleanup:
    free(ds64.entries);
    if (result != 0) {
        hwa_wav_reader_close(reader);
    } else if (error != NULL && error_size > 0U) {
        error[0] = '\0';
    }
    return result;
}

int hwa_wav_reader_read_frames(HWAWavReader *reader,
                               unsigned char *buffer,
                               size_t frame_capacity,
                               size_t *frames_read,
                               char *error,
                               size_t error_size)
{
    uint64_t remaining_frames;
    size_t requested_frames;
    size_t requested_bytes;

    if (reader == NULL || reader->source.read_at == NULL || buffer == NULL ||
        frames_read == NULL || frame_capacity == 0U) {
        hwa_set_error(error, error_size, "invalid WAVE read arguments");
        return -1;
    }
    if (frame_capacity > SIZE_MAX / reader->format.block_align) {
        hwa_set_error(error, error_size, "WAVE read buffer size overflows");
        return -1;
    }

    remaining_frames = reader->bytes_remaining / reader->format.block_align;
    requested_frames = remaining_frames < (uint64_t)frame_capacity
                           ? (size_t)remaining_frames
                           : frame_capacity;
    requested_bytes = requested_frames * reader->format.block_align;
    if (requested_bytes == 0U) {
        *frames_read = 0U;
        return 0;
    }
    if (!hwa_read_exact(&reader->source, reader->cursor,
                        buffer, requested_bytes)) {
        hwa_set_error(error, error_size, "I/O error while reading sample data");
        return -1;
    }
    reader->cursor += (uint64_t)requested_bytes;
    reader->bytes_remaining -= (uint64_t)requested_bytes;
    *frames_read = requested_frames;
    return 0;
}

double hwa_wav_decode_sample(const HWAWavReader *reader,
                             const unsigned char *sample,
                             int *clipped)
{
    uint16_t bits = reader->format.bits_per_sample;

    *clipped = 0;
    if (reader->format.encoding == HWA_ENCODING_PCM) {
        uint16_t valid_bits = reader->format.valid_bits_per_sample;
        unsigned shift = (unsigned)(bits - valid_bits);
        uint64_t code;
        uint64_t range = UINT64_C(1) << valid_bits;
        uint64_t midpoint = range >> 1U;
        int64_t signed_value;

        if (bits == 8U) {
            code = (uint64_t)sample[0] >> shift;
            signed_value = (int64_t)code - (int64_t)midpoint;
            *clipped = code == 0U || code == range - 1U;
        } else {
            if (bits == 16U) {
                code = hwa_le16(sample);
            } else if (bits == 24U) {
                code = (uint64_t)sample[0] |
                       ((uint64_t)sample[1] << 8U) |
                       ((uint64_t)sample[2] << 16U);
            } else {
                code = hwa_le32(sample);
            }
            code >>= shift;
            signed_value = code >= midpoint
                               ? -(int64_t)(range - code)
                               : (int64_t)code;
            *clipped = code == midpoint || code == midpoint - 1U;
        }
        return (double)signed_value / (double)midpoint;
    }
    if (bits == 32U) {
        uint32_t value = hwa_le32(sample);
        float float_value;

        memcpy(&float_value, &value, sizeof(float_value));
        *clipped = float_value <= -1.0f || float_value >= 1.0f;
        return (double)float_value;
    }
    {
        uint64_t value = hwa_le64(sample);
        double double_value;

        memcpy(&double_value, &value, sizeof(double_value));
        *clipped = double_value <= -1.0 || double_value >= 1.0;
        return double_value;
    }
}

void hwa_wav_reader_close(HWAWavReader *reader)
{
    if (reader != NULL) {
        if (reader->close_context != NULL) {
            reader->close_context(reader->source.context);
        }
        memset(reader, 0, sizeof(*reader));
    }
}

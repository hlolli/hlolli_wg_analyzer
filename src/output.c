#include "output.h"

#include <stddef.h>

static size_t hwa_utf8_sequence_size(const unsigned char *text)
{
    unsigned char first = text[0];
    unsigned char second;

    if (first < 0x80U) {
        return 1U;
    }
    if (text[1] == 0U) {
        return 0U;
    }
    second = text[1];
    if (first >= 0xc2U && first <= 0xdfU) {
        return second >= 0x80U && second <= 0xbfU ? 2U : 0U;
    }
    if (text[2] == 0U) {
        return 0U;
    }
    if (first == 0xe0U) {
        return second >= 0xa0U && second <= 0xbfU &&
                       text[2] >= 0x80U && text[2] <= 0xbfU
                   ? 3U
                   : 0U;
    }
    if (first >= 0xe1U && first <= 0xecU) {
        return second >= 0x80U && second <= 0xbfU &&
                       text[2] >= 0x80U && text[2] <= 0xbfU
                   ? 3U
                   : 0U;
    }
    if (first == 0xedU) {
        return second >= 0x80U && second <= 0x9fU &&
                       text[2] >= 0x80U && text[2] <= 0xbfU
                   ? 3U
                   : 0U;
    }
    if (first >= 0xeeU && first <= 0xefU) {
        return second >= 0x80U && second <= 0xbfU &&
                       text[2] >= 0x80U && text[2] <= 0xbfU
                   ? 3U
                   : 0U;
    }
    if (text[3] == 0U) {
        return 0U;
    }
    if (first == 0xf0U) {
        return second >= 0x90U && second <= 0xbfU &&
                       text[2] >= 0x80U && text[2] <= 0xbfU &&
                       text[3] >= 0x80U && text[3] <= 0xbfU
                   ? 4U
                   : 0U;
    }
    if (first >= 0xf1U && first <= 0xf3U) {
        return second >= 0x80U && second <= 0xbfU &&
                       text[2] >= 0x80U && text[2] <= 0xbfU &&
                       text[3] >= 0x80U && text[3] <= 0xbfU
                   ? 4U
                   : 0U;
    }
    if (first == 0xf4U) {
        return second >= 0x80U && second <= 0x8fU &&
                       text[2] >= 0x80U && text[2] <= 0xbfU &&
                       text[3] >= 0x80U && text[3] <= 0xbfU
                   ? 4U
                   : 0U;
    }
    return 0U;
}

int hwa_json_write_string(FILE *stream, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;

    if (stream == NULL || text == NULL || fputc('"', stream) == EOF) {
        return -1;
    }
    while (*cursor != 0U) {
        unsigned char byte = *cursor;
        size_t sequence_size;

        if (byte >= 0x80U) {
            sequence_size = hwa_utf8_sequence_size(cursor);
            if (sequence_size != 0U) {
                if (fwrite(cursor, 1U, sequence_size, stream) != sequence_size) {
                    return -1;
                }
                cursor += sequence_size;
            } else {
                if (fprintf(stream, "\\u00%02x", (unsigned)byte) < 0) {
                    return -1;
                }
                cursor++;
            }
            continue;
        }
        cursor++;
        switch (byte) {
        case '"':
            if (fputs("\\\"", stream) == EOF) {
                return -1;
            }
            break;
        case '\\':
            if (fputs("\\\\", stream) == EOF) {
                return -1;
            }
            break;
        case '\b':
            if (fputs("\\b", stream) == EOF) {
                return -1;
            }
            break;
        case '\f':
            if (fputs("\\f", stream) == EOF) {
                return -1;
            }
            break;
        case '\n':
            if (fputs("\\n", stream) == EOF) {
                return -1;
            }
            break;
        case '\r':
            if (fputs("\\r", stream) == EOF) {
                return -1;
            }
            break;
        case '\t':
            if (fputs("\\t", stream) == EOF) {
                return -1;
            }
            break;
        default:
            if (byte < 0x20U) {
                if (fprintf(stream, "\\u%04x", (unsigned)byte) < 0) {
                    return -1;
                }
            } else if (fputc((int)byte, stream) == EOF) {
                return -1;
            }
            break;
        }
    }
    return fputc('"', stream) == EOF ? -1 : 0;
}

int hwa_json_write_byte_hex(FILE *stream, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;

    if (stream == NULL || text == NULL || fputc('"', stream) == EOF) {
        return -1;
    }
    while (*cursor != 0U) {
        if (fprintf(stream, "%02x", (unsigned)*cursor++) < 0) {
            return -1;
        }
    }
    return fputc('"', stream) == EOF ? -1 : 0;
}

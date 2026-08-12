#include "output.h"

#include <stdio.h>
#include <string.h>

typedef int (*json_writer)(FILE *stream, const char *text);

static int writer_matches(json_writer writer,
                          const char *input,
                          const unsigned char *expected,
                          size_t expected_size)
{
    unsigned char actual[256];
    FILE *stream = tmpfile();
    long output_size;
    int ok = 1;

    if (stream == NULL) {
        return 0;
    }
    if (writer(stream, input) != 0 || fflush(stream) != 0 || ferror(stream) ||
        fseek(stream, 0L, SEEK_END) != 0) {
        ok = 0;
    }
    output_size = ok ? ftell(stream) : -1L;
    if (output_size < 0 || (size_t)output_size != expected_size ||
        expected_size > sizeof(actual) || fseek(stream, 0L, SEEK_SET) != 0 ||
        fread(actual, 1U, expected_size, stream) != expected_size ||
        memcmp(actual, expected, expected_size) != 0) {
        ok = 0;
    }
    if (fclose(stream) != 0) {
        ok = 0;
    }
    return ok;
}

int main(void)
{
    static const unsigned char input[] = {
        'a', '"', '\\', '\n', '-', 0xc3U, 0xa9U, '-',
        0xffU, 0xc0U, 0xafU, 0U
    };
    static const unsigned char expected_json[] = {
        '"', 'a', '\\', '"', '\\', '\\', '\\', 'n', '-',
        0xc3U, 0xa9U, '-',
        '\\', 'u', '0', '0', 'f', 'f',
        '\\', 'u', '0', '0', 'c', '0',
        '\\', 'u', '0', '0', 'a', 'f', '"'
    };
    static const unsigned char expected_hex[] =
        "\"61225c0a2dc3a92dffc0af\"";

    if (!writer_matches(hwa_json_write_string,
                        (const char *)input,
                        expected_json,
                        sizeof(expected_json))) {
        (void)fputs("JSON string byte encoding mismatch\n", stderr);
        return 1;
    }
    if (!writer_matches(hwa_json_write_byte_hex,
                        (const char *)input,
                        expected_hex,
                        sizeof(expected_hex) - 1U)) {
        (void)fputs("JSON path byte hex mismatch\n", stderr);
        return 1;
    }
    return 0;
}

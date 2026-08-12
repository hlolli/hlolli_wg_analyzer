#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int hwa_exercise_wav(const uint8_t *data, size_t size,
                             int *accepted,
                             uint64_t *decoded_samples,
                             uint64_t *nonfinite_samples,
                             uint64_t *decoded_checksum);

typedef struct HWASeed {
    uint8_t *data;
    size_t size;
} HWASeed;

typedef struct HWAExpectedSeed {
    const char *name;
    int accepted;
    uint64_t decoded_samples;
    uint64_t nonfinite_samples;
    uint64_t decoded_checksum;
} HWAExpectedSeed;

static const HWAExpectedSeed expected_seeds[] = {
    {"pcm16-mono.hex", 1, UINT64_C(4), UINT64_C(0),
     UINT64_C(0x81f1e26974c293e6)},
    {"pcm8-odd-chunk.hex", 1, UINT64_C(1), UINT64_C(0),
     UINT64_C(0xe604823a249029bf)},
    {"rf64-pcm16.hex", 1, UINT64_C(1), UINT64_C(0),
     UINT64_C(0xe604823a249029bf)},
    {"float32-nan.hex", 1, UINT64_C(1), UINT64_C(1),
     UINT64_C(0x8011403cbfd257c0)},
    {"truncated.hex", 0, UINT64_C(0), UINT64_C(0),
     UINT64_C(0xcbf29ce484222325)}
};

static const char *base_name(const char *path) {
    const char *name = path;
    const char *cursor;
    for (cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') {
            name = cursor + 1;
        }
    }
    return name;
}

static const HWAExpectedSeed *find_expected(const char *path) {
    const char *name = base_name(path);
    size_t index;
    for (index = 0U;
         index < sizeof(expected_seeds) / sizeof(expected_seeds[0]);
         ++index) {
        if (strcmp(name, expected_seeds[index].name) == 0) {
            return &expected_seeds[index];
        }
    }
    return NULL;
}

static int hex_value(int character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static int read_hex_seed(const char *path, HWASeed *seed) {
    FILE *file;
    long length;
    char *text;
    size_t text_size;
    size_t index;
    size_t output_size = 0U;
    int high_nibble = -1;
    int in_comment = 0;

    seed->data = NULL;
    seed->size = 0U;
    file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0L, SEEK_END) != 0) {
        if (file != NULL) {
            (void)fclose(file);
        }
        return 0;
    }
    length = ftell(file);
    if (length < 0L || fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        return 0;
    }
    text_size = (size_t)length;
    text = (char *)malloc(text_size + 1U);
    seed->data = (uint8_t *)malloc(text_size / 2U + 1U);
    if (text == NULL || seed->data == NULL) {
        free(text);
        free(seed->data);
        seed->data = NULL;
        (void)fclose(file);
        return 0;
    }
    if (text_size != 0U && fread(text, 1U, text_size, file) != text_size) {
        free(text);
        free(seed->data);
        seed->data = NULL;
        (void)fclose(file);
        return 0;
    }
    (void)fclose(file);

    for (index = 0U; index < text_size; ++index) {
        const unsigned char character = (unsigned char)text[index];
        int value;
        if (in_comment) {
            if (character == '\n') {
                in_comment = 0;
            }
            continue;
        }
        if (character == '#') {
            in_comment = 1;
            continue;
        }
        if (isspace(character)) {
            continue;
        }
        value = hex_value(character);
        if (value < 0) {
            free(text);
            free(seed->data);
            seed->data = NULL;
            return 0;
        }
        if (high_nibble < 0) {
            high_nibble = value;
        } else {
            seed->data[output_size] =
                (uint8_t)((unsigned int)high_nibble * 16U +
                          (unsigned int)value);
            ++output_size;
            high_nibble = -1;
        }
    }
    free(text);
    if (high_nibble >= 0) {
        free(seed->data);
        seed->data = NULL;
        return 0;
    }
    seed->size = output_size;
    return 1;
}

int main(int argc, char **argv) {
    uint64_t decoded_samples = 0U;
    int index;

    if (argc < 2) {
        (void)fprintf(stderr, "usage: %s seed.hex [seed.hex ...]\n", argv[0]);
        return 2;
    }
    for (index = 1; index < argc; ++index) {
        HWASeed seed;
        const HWAExpectedSeed *expected = find_expected(argv[index]);
        int accepted = 0;
        uint64_t seed_samples = 0U;
        uint64_t nonfinite_samples = 0U;
        uint64_t decoded_checksum = 0U;
        if (expected == NULL) {
            (void)fprintf(stderr, "unknown fixed seed: %s\n", argv[index]);
            return 2;
        }
        if (!read_hex_seed(argv[index], &seed)) {
            (void)fprintf(stderr, "could not read hex seed: %s\n", argv[index]);
            return 2;
        }
        if (hwa_exercise_wav(seed.data, seed.size, &accepted,
                                     &seed_samples,
                                     &nonfinite_samples,
                                     &decoded_checksum) != 0) {
            free(seed.data);
            (void)fprintf(stderr, "could not exercise seed: %s\n", argv[index]);
            return 2;
        }
        if (accepted != expected->accepted ||
            seed_samples != expected->decoded_samples ||
            nonfinite_samples != expected->nonfinite_samples ||
            decoded_checksum != expected->decoded_checksum) {
            free(seed.data);
            (void)fprintf(
                stderr,
                "unexpected result for %s: accepted=%d samples=%" PRIu64
                " nonfinite=%" PRIu64 " checksum=%016" PRIx64 "\n",
                argv[index], accepted, seed_samples, nonfinite_samples,
                decoded_checksum);
            return 1;
        }
        decoded_samples += seed_samples;
        free(seed.data);
    }

    (void)printf("PASS: %d fixed WAVE inputs, %" PRIu64
                 " decoded samples\n",
                 argc - 1, decoded_samples);
    return 0;
}

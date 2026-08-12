#include <hlolli_wg_analyzer.h>

#include <stdint.h>
#include <string.h>

typedef struct ClientBytes {
    const unsigned char *data;
    size_t size;
} ClientBytes;

static int client_read_at(void *context,
                          uint64_t offset,
                          unsigned char *destination,
                          size_t size)
{
    ClientBytes *bytes = (ClientBytes *)context;
    if (offset > (uint64_t)bytes->size ||
        (uint64_t)size > (uint64_t)bytes->size - offset) return -1;
    memcpy(destination, bytes->data + (size_t)offset, size);
    return 0;
}

int main(void)
{
    static const unsigned char truncated_wave[12] = {
        'R', 'I', 'F', 'F', 4U, 0U, 0U, 0U,
        'W', 'A', 'V', 'E'
    };
    ClientBytes bytes = {truncated_wave, sizeof(truncated_wave)};
    HWAByteSource source = {
        &bytes, "public-client.wav", sizeof(truncated_wave), client_read_at
    };
    HWAAnalysis analysis;
    char error[HWA_ERROR_SIZE];
    if (strcmp(HWA_VERSION, "1.1.0") != 0 ||
        hwa_analyze_wav_source(&source, NULL, &analysis,
                               error, sizeof(error)) == 0 ||
        error[0] == '\0' || analysis.path != NULL ||
        analysis.channels != NULL || analysis.tracks != NULL ||
        analysis.spectrogram_db != NULL) {
        return 1;
    }
    hwa_analysis_free(&analysis);
    return 0;
}

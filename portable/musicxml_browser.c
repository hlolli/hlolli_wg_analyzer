#define _POSIX_C_SOURCE 200809L

#include "hlolli_wg_analyzer.h"
#include "musicxml_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Each worker owns one input and one report. A new input discards the old report. */
static unsigned char *input;
static size_t input_size;
static char *output;
static size_t output_size;
static char error_text[512];

void hwa_browser_score_clear(void)
{
    free(input);
    free(output);
    input = NULL;
    output = NULL;
    input_size = 0U;
    output_size = 0U;
    error_text[0] = '\0';
}

unsigned char *hwa_browser_score_input(size_t size)
{
    HWAMusicXMLOptions options;
    hwa_browser_score_clear();
    hwa_musicxml_options_default(&options);
    if (size == 0U || (uint64_t)size > options.max_input_bytes) {
        (void)snprintf(error_text, sizeof(error_text), "MusicXML input exceeds the byte limit or is empty");
        return NULL;
    }
    input = (unsigned char *)malloc(size);
    if (input == NULL) {
        (void)snprintf(error_text, sizeof(error_text), "Cannot allocate MusicXML input");
        return NULL;
    }
    input_size = size;
    return input;
}

int hwa_browser_score_read(void)
{
    HWAMusicXMLOptions options;
    HWAMusicXMLScore score = {0};
    FILE *stream;
    int result;
    free(output);
    output = NULL;
    output_size = 0U;
    error_text[0] = '\0';
    hwa_musicxml_options_default(&options);
    if (hwa_musicxml_read(input, input_size, &options, &score,
                         error_text, sizeof(error_text)) != 0) {
        return -1;
    }
    stream = open_memstream(&output, &output_size);
    if (stream == NULL) {
        hwa_musicxml_score_free(&score);
        (void)snprintf(error_text, sizeof(error_text), "Cannot allocate MusicXML report");
        return -1;
    }
    result = hwa_musicxml_report(stream, &score, &options);
    if (fclose(stream) != 0) result = -1;
    hwa_musicxml_score_free(&score);
    if (result != 0) {
        free(output);
        output = NULL;
        output_size = 0U;
        (void)snprintf(error_text, sizeof(error_text), "Cannot write MusicXML report");
    }
    return result;
}

const char *hwa_browser_score_output(void) { return output; }
size_t hwa_browser_score_output_size(void) { return output_size; }
const char *hwa_browser_score_error(void) { return error_text; }

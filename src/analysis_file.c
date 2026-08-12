#include "internal.h"

#include <string.h>

int hwa_analyze_wav_with_options(const char *path,
                                 const HWAAnalysisOptions *provided_options,
                                 HWAAnalysis *analysis,
                                 char *error,
                                 size_t error_size)
{
    HWAAnalysisOptions selected_options;
    HWAWavReader reader;
    int status;
    if (error != NULL && error_size > 0U) error[0] = '\0';
    if (analysis == NULL) {
        hwa_set_error(error, error_size, "invalid analysis arguments");
        return -1;
    }
    if (provided_options == NULL) hwa_analysis_options_default(&selected_options);
    else selected_options = *provided_options;
    memset(analysis, 0, sizeof(*analysis));
    if (path == NULL) {
        hwa_set_error(error, error_size, "input path is null");
        return -1;
    }
    if (hwa_analysis_options_validate(&selected_options,
                                      error, error_size) != 0) return -1;
    memset(&reader, 0, sizeof(reader));
    if (hwa_wav_reader_open(&reader, path, selected_options.max_input_bytes,
                            error, error_size) != 0) return -1;
    status = hwa_analyze_wav_reader(&reader, path, &selected_options,
                                    analysis, error, error_size);
    hwa_wav_reader_close(&reader);
    return status;
}

int hwa_analyze_wav(const char *path,
                    HWAAnalysis *analysis,
                    char *error,
                    size_t error_size)
{
    HWAAnalysisOptions options;
    hwa_analysis_options_default(&options);
    return hwa_analyze_wav_with_options(path, &options, analysis,
                                        error, error_size);
}

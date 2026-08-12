#ifndef HWA_EXPERIMENT_PROCESS_H
#define HWA_EXPERIMENT_PROCESS_H

#include "hlolli_wg_analyzer.h"

#include <stddef.h>
#include <stdint.h>

typedef struct HWAExperimentProcessRenderer {
    char *path;
    char *private_directory;
    char sha256[HWA_SHA256_HEX_SIZE];
    uint64_t max_executable_bytes;
#if defined(_WIN32)
    void *locked_file;
#endif
} HWAExperimentProcessRenderer;

/*
 * Bind one explicit regular executable by copying and hashing one checked
 * open stream into a private directory. The adapter runs only that private
 * copy. It passes a fixed argument vector, an empty environment, and the job
 * directory as the child cwd. It never invokes a shell.
 */
int hwa_experiment_process_renderer_open(
    HWAExperimentProcessRenderer *renderer,
    const char *path,
    uint64_t max_executable_bytes,
    char *error,
    size_t error_size);

int hwa_experiment_process_renderer_render(
    void *context,
    const HWAExperimentRenderRequest *request,
    char *error,
    size_t error_size);

void hwa_experiment_process_renderer_close(
    HWAExperimentProcessRenderer *renderer);

#endif

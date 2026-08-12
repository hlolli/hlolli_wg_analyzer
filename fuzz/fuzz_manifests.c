#include "fuzz_support.h"

#include "experiment.h"
#include "gap_report.h"
#include "run.h"
#include "score_manifest.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const char hwa_run_schema_prefix[] = "{\"schema\":\"hwa-run\"";
static const char hwa_experiment_schema_prefix[] =
    "{\"schema\":\"hwa-experiment\"";
static const char hwa_gap_schema_prefix[] = "{\"schema\":\"hwa-gap-report\"";

static void fuzz_score(const unsigned char *data, size_t size)
{
    HWAFuzzTempFile temporary;
    HWAScoreManifest manifest;
    char error[HWA_ERROR_SIZE];
    if (hwa_fuzz_write_temp(data, size, &temporary) != 0) return;
    if (hwa_score_manifest_load(temporary.path, HWA_FUZZ_MAX_INPUT_BYTES,
                                2048U, &manifest,
                                error, sizeof(error)) == 0)
        hwa_score_manifest_free(&manifest);
    hwa_fuzz_remove_temp(&temporary);
}

static void fuzz_run(const unsigned char *data, size_t size)
{
    HWARunOptions options;
    char error[HWA_ERROR_SIZE];
    hwa_run_options_default(&options);
    options.max_manifest_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    options.max_work_bytes = UINT64_C(67108864);
    (void)hwa_run_manifest_validate_bytes(data, size, &options,
                                          error, sizeof(error));
}

static void fuzz_experiment(const unsigned char *data, size_t size)
{
    HWAExperimentOptions options;
    char error[HWA_ERROR_SIZE];
    hwa_experiment_options_default(&options);
    options.max_manifest_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    options.max_work_bytes = UINT64_C(67108864);
    options.run.max_manifest_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    options.run.max_work_bytes = UINT64_C(67108864);
    (void)hwa_experiment_manifest_validate_bytes(data, size, &options,
                                                 error, sizeof(error));
}

static void fuzz_gap(const unsigned char *data, size_t size)
{
    HWAGapReportOptions options;
    char error[HWA_ERROR_SIZE];
    hwa_gap_report_options_default(&options);
    options.max_manifest_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    options.max_work_bytes = UINT64_C(67108864);
    options.run.max_manifest_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    options.run.max_work_bytes = UINT64_C(67108864);
    options.experiment.max_manifest_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    options.experiment.max_work_bytes = UINT64_C(67108864);
    (void)hwa_gap_report_manifest_validate_bytes(data, size, &options,
                                                 error, sizeof(error));
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const unsigned char *body;
    size_t body_size;
    if ((uint64_t)size > HWA_FUZZ_MAX_INPUT_BYTES || size == 0U)
        return 0;
    if (size >= 13U && memcmp(data, "event_id,kind", 13U) == 0) {
        fuzz_score(data, size);
        return 0;
    }
    if (size >= sizeof(hwa_run_schema_prefix) - 1U &&
        memcmp(data, hwa_run_schema_prefix,
               sizeof(hwa_run_schema_prefix) - 1U) == 0) {
        fuzz_run(data, size);
        return 0;
    }
    if (size >= sizeof(hwa_experiment_schema_prefix) - 1U &&
        memcmp(data, hwa_experiment_schema_prefix,
               sizeof(hwa_experiment_schema_prefix) - 1U) == 0) {
        fuzz_experiment(data, size);
        return 0;
    }
    if (size >= sizeof(hwa_gap_schema_prefix) - 1U &&
        memcmp(data, hwa_gap_schema_prefix,
               sizeof(hwa_gap_schema_prefix) - 1U) == 0) {
        fuzz_gap(data, size);
        return 0;
    }
    body = data + 1U;
    body_size = size - 1U;
    switch (data[0] % 4U) {
    case 0U: fuzz_score(body, body_size); break;
    case 1U: fuzz_run(body, body_size); break;
    case 2U: fuzz_experiment(body, body_size); break;
    default: fuzz_gap(body, body_size); break;
    }
    return 0;
}

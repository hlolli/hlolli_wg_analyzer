#include "fuzz_support.h"

#include "experiment_file.h"
#include "gap_report_file.h"
#include "item_file.h"
#include "measure_file.h"
#include "physical_file.h"
#include "production_file.h"
#include "run_file.h"
#include "typed_labels.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void fuzz_labels(const char *path)
{
    HWATypedLabelSet labels;
    char error[HWA_ERROR_SIZE];
    if (hwa_typed_labels_load(path, HWA_FUZZ_MAX_INPUT_BYTES,
                              UINT64_C(16777216), 2048U, 65536U,
                              &labels, error, sizeof(error)) == 0)
        hwa_typed_labels_free(&labels);
}

static void fuzz_item_edits(const char *path)
{
    HWAItemFileLimits limits;
    HWAItemEditSet edits;
    char error[HWA_ERROR_SIZE];
    hwa_item_file_limits_default(&limits);
    limits.max_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    limits.max_work_bytes = UINT64_C(67108864);
    if (hwa_item_file_read_edits(path, &limits, &edits,
                                 error, sizeof(error)) == 0)
        hwa_item_edit_set_free(&edits);
}

static void fuzz_item_full(const char *path)
{
    HWAItemFileLimits limits;
    HWAItemFileData items;
    char error[HWA_ERROR_SIZE];
    hwa_item_file_limits_default(&limits);
    limits.max_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    limits.max_work_bytes = UINT64_C(67108864);
    if (hwa_item_file_read_full(path, &limits, &items,
                                error, sizeof(error)) == 0)
        hwa_item_file_data_free(&items);
}

static void fuzz_measure(const char *path)
{
    HWAProfileComparisonOptions limits;
    HWAMeasurementSet set;
    char sha256[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    hwa_profile_comparison_options_default(&limits);
    limits.max_input_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    limits.max_work_bytes = UINT64_C(134217728);
    if (hwa_measure_file_read(path, &limits, &set, sha256,
                              error, sizeof(error)) == 0)
        hwa_measurement_set_free(&set);
}

static void fuzz_physical(const char *path)
{
    HWAPhysicalOptions limits;
    HWAPhysicalCheckSet set;
    char sha256[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    hwa_physical_options_default(&limits);
    limits.max_wave_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    limits.max_work_bytes = UINT64_C(134217728);
    limits.profile_limits.max_input_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    limits.profile_limits.max_work_bytes = UINT64_C(134217728);
    if (hwa_physical_file_read(path, &limits, &set, sha256,
                               error, sizeof(error)) == 0)
        hwa_physical_check_set_free(&set);
}

static void fuzz_production(const char *path)
{
    HWAProductionOptions limits;
    HWAProductionResult result;
    char sha256[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    hwa_production_options_default(&limits);
    limits.max_input_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    limits.max_work_bytes = UINT64_C(134217728);
    limits.profile_limits.max_input_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    limits.profile_limits.max_work_bytes = UINT64_C(134217728);
    if (hwa_production_file_read(path, &limits, &result, sha256,
                                 error, sizeof(error)) == 0)
        hwa_production_result_free(&result);
}

static void fuzz_run_result(const char *path)
{
    HWARunOptions limits;
    HWARunResult result;
    char sha256[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    hwa_run_options_default(&limits);
    limits.max_input_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    limits.max_work_bytes = UINT64_C(134217728);
    if (hwa_run_file_read(path, &limits, &result, sha256,
                          error, sizeof(error)) == 0)
        hwa_run_result_free(&result);
}

static void fuzz_experiment_result(const char *path)
{
    HWAExperimentOptions limits;
    HWAExperimentResult result;
    char sha256[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    hwa_experiment_options_default(&limits);
    limits.max_input_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    limits.max_bundle_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    limits.max_output_file_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    limits.max_work_bytes = UINT64_C(134217728);
    limits.run.max_input_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    limits.run.max_work_bytes = UINT64_C(134217728);
    if (hwa_experiment_file_read(path, &limits, &result, sha256,
                                 error, sizeof(error)) == 0)
        hwa_experiment_result_free(&result);
}

static void fuzz_gap_report(const char *path)
{
    HWAGapReportOptions limits;
    HWAGapReportResult result;
    char sha256[HWA_SHA256_HEX_SIZE];
    char error[HWA_ERROR_SIZE];
    hwa_gap_report_options_default(&limits);
    limits.max_input_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    limits.max_bundle_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    limits.max_output_file_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    limits.max_work_bytes = UINT64_C(134217728);
    limits.run.max_input_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    limits.run.max_work_bytes = UINT64_C(134217728);
    limits.experiment.max_input_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    limits.experiment.max_bundle_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    limits.experiment.max_output_file_bytes = HWA_FUZZ_MAX_INPUT_BYTES;
    limits.experiment.max_work_bytes = UINT64_C(134217728);
    if (hwa_gap_report_file_read(path, &limits, &result, sha256,
                                 error, sizeof(error)) == 0)
        hwa_gap_report_result_free(&result);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    HWAFuzzTempFile temporary;
    const unsigned char *body;
    size_t body_size;
    if ((uint64_t)size > HWA_FUZZ_MAX_INPUT_BYTES || size == 0U)
        return 0;
    if (hwa_fuzz_write_temp(data, size, &temporary) != 0)
        return 0;
    if (size >= 14U && memcmp(data, "event_id,pitch", 14U) == 0) {
        fuzz_labels(temporary.path);
    } else if (size >= 11U && memcmp(data, "HWA_ITEMS,1", 11U) == 0) {
        fuzz_item_edits(temporary.path);
        fuzz_item_full(temporary.path);
    } else if (size >= 14U && memcmp(data, "HWA_MEASURES,1", 14U) == 0) {
        fuzz_measure(temporary.path);
    } else if (size >= 14U && memcmp(data, "HWA_PHYSICAL,1", 14U) == 0) {
        fuzz_physical(temporary.path);
    } else if (size >= 16U && memcmp(data, "HWA_PRODUCTION,1", 16U) == 0) {
        fuzz_production(temporary.path);
    } else if (size >= 9U && memcmp(data, "HWA_RUN,1", 9U) == 0) {
        fuzz_run_result(temporary.path);
    } else if (size >= 16U && memcmp(data, "HWA_EXPERIMENT,1", 16U) == 0) {
        fuzz_experiment_result(temporary.path);
    } else if (size >= 12U && memcmp(data, "HWA_REPORT,1", 12U) == 0) {
        fuzz_gap_report(temporary.path);
    } else {
        hwa_fuzz_remove_temp(&temporary);
        body = data + 1U;
        body_size = size - 1U;
        if (hwa_fuzz_write_temp(body, body_size, &temporary) != 0)
            return 0;
        switch (data[0] % 9U) {
        case 0U: fuzz_labels(temporary.path); break;
        case 1U: fuzz_item_edits(temporary.path); break;
        case 2U: fuzz_item_full(temporary.path); break;
        case 3U: fuzz_measure(temporary.path); break;
        case 4U: fuzz_physical(temporary.path); break;
        case 5U: fuzz_production(temporary.path); break;
        case 6U: fuzz_run_result(temporary.path); break;
        case 7U: fuzz_experiment_result(temporary.path); break;
        default: fuzz_gap_report(temporary.path); break;
        }
    }
    hwa_fuzz_remove_temp(&temporary);
    return 0;
}

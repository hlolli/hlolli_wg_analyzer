#include "fuzz_support.h"

#include "run.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint64_t csv_row_count(const uint8_t *data, size_t size)
{
    uint64_t rows = 0U;
    size_t index;
    if (size < 12U || memcmp(data, "index,value", 11U) != 0)
        return UINT64_MAX;
    for (index = 0U; index < size; ++index)
        if (data[index] == '\n') {
            if (rows == UINT64_MAX) return UINT64_MAX;
            rows++;
        }
    return rows == 0U ? UINT64_MAX : rows - 1U;
}

static uint64_t le64(const uint8_t *data)
{
    return (uint64_t)data[0] | ((uint64_t)data[1] << 8U) |
           ((uint64_t)data[2] << 16U) | ((uint64_t)data[3] << 24U) |
           ((uint64_t)data[4] << 32U) | ((uint64_t)data[5] << 40U) |
           ((uint64_t)data[6] << 48U) | ((uint64_t)data[7] << 56U);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    HWARunProbeFormat format;
    uint64_t expected;
    const uint8_t *body = data;
    size_t body_size = size;
    double *values;
    char error[HWA_ERROR_SIZE];
    if ((uint64_t)size > HWA_FUZZ_MAX_INPUT_BYTES || size == 0U)
        return 0;
    expected = csv_row_count(data, size);
    if (expected != UINT64_MAX) format = HWA_RUN_PROBE_CSV_F64;
    else if (size >= 16U && memcmp(data, "HWAPRB1", 7U) == 0 &&
             data[7] == 0U && le64(data + 8U) <= UINT64_C(1024)) {
        format = HWA_RUN_PROBE_BINARY_F64LE;
        expected = le64(data + 8U);
    } else if (size >= 3U) {
        format = (data[0] & 1U) == 0U ? HWA_RUN_PROBE_CSV_F64
                                      : HWA_RUN_PROBE_BINARY_F64LE;
        expected = (uint64_t)data[1] | ((uint64_t)data[2] << 8U);
        expected %= UINT64_C(1025);
        body = data + 3U;
        body_size = size - 3U;
    } else {
        return 0;
    }
    values = (double *)calloc((size_t)(expected == 0U ? 1U : expected),
                              sizeof(*values));
    if (values == NULL) return 0;
    (void)hwa_run_probe_parse_bytes(format, body, body_size,
                                    expected, values,
                                    error, sizeof(error));
    free(values);
    return 0;
}

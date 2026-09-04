#include "inference_htdemucs_onnx.h"

#include "sha256.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HWA_HAVE_ONNX_RUNTIME
#define HWA_HAVE_ONNX_RUNTIME 0
#endif

#if HWA_HAVE_ONNX_RUNTIME
#include <onnxruntime_c_api.h>

#define HWA_HTDEMUCS_ONNX_INPUT_NAME "mix"
#define HWA_HTDEMUCS_ONNX_OUTPUT_NAME "stems"

typedef struct HWAInferenceHTDemucsOnnx {
    const OrtApi *api;
    OrtEnv *environment;
    OrtSessionOptions *session_options;
    OrtSession *session;
    OrtMemoryInfo *memory_info;
    unsigned char *model_bytes;
    size_t model_byte_count;
} HWAInferenceHTDemucsOnnx;

static void hwa_htdemucs_onnx_error(char *error,
                                    size_t error_size,
                                    const char *message)
{
    if (error == NULL || error_size == 0U) return;
    (void)snprintf(error, error_size, "%s", message);
    error[error_size - 1U] = '\0';
}

static int hwa_htdemucs_onnx_status(HWAInferenceHTDemucsOnnx *context,
                                    OrtStatus *status,
                                    const char *action,
                                    char *error,
                                    size_t error_size)
{
    const char *message;
    if (status == NULL) return 0;
    message = context != NULL && context->api != NULL
                  ? context->api->GetErrorMessage(status) : NULL;
    if (error != NULL && error_size != 0U) {
        (void)snprintf(error, error_size, "%s%s%s",
                       action != NULL ? action : "ONNX Runtime failed",
                       message != NULL ? ": " : "",
                       message != NULL ? message : "");
        error[error_size - 1U] = '\0';
    }
    if (context != NULL && context->api != NULL)
        context->api->ReleaseStatus(status);
    return -1;
}

static void hwa_htdemucs_onnx_destroy(void *context_value)
{
    HWAInferenceHTDemucsOnnx *context =
        (HWAInferenceHTDemucsOnnx *)context_value;
    if (context == NULL) return;
    if (context->api != NULL) {
        context->api->ReleaseMemoryInfo(context->memory_info);
        context->api->ReleaseSession(context->session);
        context->api->ReleaseSessionOptions(context->session_options);
        context->api->ReleaseEnv(context->environment);
    }
    free(context->model_bytes);
    memset(context, 0, sizeof(*context));
    free(context);
}

static int hwa_htdemucs_onnx_tensor_contract(
    HWAInferenceHTDemucsOnnx *context,
    size_t index,
    int output,
    const int64_t *expected_dimensions,
    size_t expected_rank,
    char *error,
    size_t error_size)
{
    OrtTypeInfo *type_info = NULL;
    const OrtTensorTypeAndShapeInfo *tensor_info = NULL;
    enum ONNXType value_type = ONNX_TYPE_UNKNOWN;
    enum ONNXTensorElementDataType element_type =
        ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    int64_t dimensions[4] = {0, 0, 0, 0};
    size_t rank = 0U;
    size_t dimension_index;
    OrtStatus *status;
    int result = -1;
    status = output
                 ? context->api->SessionGetOutputTypeInfo(
                       context->session, index, &type_info)
                 : context->api->SessionGetInputTypeInfo(
                       context->session, index, &type_info);
    if (hwa_htdemucs_onnx_status(
            context, status, "cannot read HTDemucs ONNX tensor type",
            error, error_size) != 0)
        goto cleanup;
    if (hwa_htdemucs_onnx_status(
            context,
            context->api->GetOnnxTypeFromTypeInfo(type_info, &value_type),
            "cannot read HTDemucs ONNX value type", error, error_size) != 0 ||
        value_type != ONNX_TYPE_TENSOR ||
        hwa_htdemucs_onnx_status(
            context,
            context->api->CastTypeInfoToTensorInfo(type_info, &tensor_info),
            "cannot read HTDemucs ONNX tensor metadata",
            error, error_size) != 0 ||
        tensor_info == NULL ||
        hwa_htdemucs_onnx_status(
            context,
            context->api->GetTensorElementType(tensor_info, &element_type),
            "cannot read HTDemucs ONNX tensor element type",
            error, error_size) != 0 ||
        hwa_htdemucs_onnx_status(
            context,
            context->api->GetDimensionsCount(tensor_info, &rank),
            "cannot read HTDemucs ONNX tensor rank", error, error_size) != 0 ||
        rank != expected_rank || rank > 4U ||
        hwa_htdemucs_onnx_status(
            context,
            context->api->GetDimensions(tensor_info, dimensions, rank),
            "cannot read HTDemucs ONNX tensor dimensions",
            error, error_size) != 0 ||
        element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        goto wrong_contract;
    for (dimension_index = 0U;
         dimension_index < expected_rank;
         ++dimension_index) {
        if (dimensions[dimension_index] !=
            expected_dimensions[dimension_index])
            goto wrong_contract;
    }
    result = 0;
    goto cleanup;

wrong_contract:
    if (error != NULL && error_size != 0U && error[0] == '\0') {
        (void)snprintf(error, error_size,
                       "HTDemucs ONNX tensor has the wrong type or shape");
        error[error_size - 1U] = '\0';
    }
cleanup:
    context->api->ReleaseTypeInfo(type_info);
    return result;
}

static int hwa_htdemucs_onnx_model_contract(
    HWAInferenceHTDemucsOnnx *context,
    char *error,
    size_t error_size)
{
    static const int64_t input_dimensions[3] = {
        1, 2, (int64_t)HWA_HTDEMUCS_INPUT_SAMPLES};
    static const int64_t output_dimensions[4] = {
        1, (int64_t)HWA_HTDEMUCS_STEM_COUNT, 2,
        (int64_t)HWA_HTDEMUCS_INPUT_SAMPLES};
    OrtAllocator *allocator = NULL;
    char *name = NULL;
    size_t input_count = 0U;
    size_t output_count = 0U;
    int result = -1;
    if (hwa_htdemucs_onnx_status(
            context,
            context->api->SessionGetInputCount(
                context->session, &input_count),
            "cannot read HTDemucs ONNX input count", error, error_size) != 0 ||
        hwa_htdemucs_onnx_status(
            context,
            context->api->SessionGetOutputCount(
                context->session, &output_count),
            "cannot read HTDemucs ONNX output count", error, error_size) != 0 ||
        input_count != 1U || output_count != 1U ||
        hwa_htdemucs_onnx_status(
            context,
            context->api->GetAllocatorWithDefaultOptions(&allocator),
            "cannot get the ONNX allocator", error, error_size) != 0) {
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            (void)snprintf(error, error_size,
                           "HTDemucs ONNX model has the wrong input or output count");
            error[error_size - 1U] = '\0';
        }
        return -1;
    }
    if (hwa_htdemucs_onnx_status(
            context,
            context->api->SessionGetInputName(
                context->session, 0U, allocator, &name),
            "cannot read HTDemucs ONNX input name", error, error_size) != 0)
        goto cleanup;
    if (strcmp(name, HWA_HTDEMUCS_ONNX_INPUT_NAME) != 0 ||
        hwa_htdemucs_onnx_tensor_contract(
            context, 0U, 0, input_dimensions, 3U,
            error, error_size) != 0) {
        if (error != NULL && error_size != 0U && error[0] == '\0')
            hwa_htdemucs_onnx_error(
                error, error_size, "HTDemucs ONNX model has the wrong input");
        goto cleanup;
    }
    if (hwa_htdemucs_onnx_status(
            context, context->api->AllocatorFree(allocator, name),
            "cannot free the HTDemucs ONNX input name",
            error, error_size) != 0)
        goto cleanup;
    name = NULL;
    if (hwa_htdemucs_onnx_status(
            context,
            context->api->SessionGetOutputName(
                context->session, 0U, allocator, &name),
            "cannot read HTDemucs ONNX output name", error, error_size) != 0)
        goto cleanup;
    if (strcmp(name, HWA_HTDEMUCS_ONNX_OUTPUT_NAME) != 0 ||
        hwa_htdemucs_onnx_tensor_contract(
            context, 0U, 1, output_dimensions, 4U,
            error, error_size) != 0) {
        if (error != NULL && error_size != 0U && error[0] == '\0')
            hwa_htdemucs_onnx_error(
                error, error_size, "HTDemucs ONNX model has the wrong output");
        goto cleanup;
    }
    result = 0;
cleanup:
    if (name != NULL)
        (void)context->api->AllocatorFree(allocator, name);
    return result;
}

static int hwa_htdemucs_onnx_run_window(
    void *context_value,
    const float input[HWA_HTDEMUCS_CHANNELS * HWA_HTDEMUCS_INPUT_SAMPLES],
    float output[HWA_HTDEMUCS_STEM_COUNT * HWA_HTDEMUCS_CHANNELS *
                 HWA_HTDEMUCS_INPUT_SAMPLES],
    char *error,
    size_t error_size)
{
    HWAInferenceHTDemucsOnnx *context =
        (HWAInferenceHTDemucsOnnx *)context_value;
    static const char *input_names[] = {HWA_HTDEMUCS_ONNX_INPUT_NAME};
    static const char *output_names[] = {HWA_HTDEMUCS_ONNX_OUTPUT_NAME};
    const OrtValue *inputs[1];
    OrtValue *input_value = NULL;
    OrtValue *outputs[1] = {NULL};
    int64_t input_shape[3] = {
        1, 2, (int64_t)HWA_HTDEMUCS_INPUT_SAMPLES};
    int64_t output_shape[4] = {
        1, (int64_t)HWA_HTDEMUCS_STEM_COUNT, 2,
        (int64_t)HWA_HTDEMUCS_INPUT_SAMPLES};
    size_t input_count =
        HWA_HTDEMUCS_CHANNELS * HWA_HTDEMUCS_INPUT_SAMPLES;
    size_t output_count =
        HWA_HTDEMUCS_STEM_COUNT * HWA_HTDEMUCS_CHANNELS *
        HWA_HTDEMUCS_INPUT_SAMPLES;
    size_t index;
    int result = -1;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || input == NULL || output == NULL) {
        hwa_htdemucs_onnx_error(
            error, error_size, "invalid HTDemucs ONNX run arguments");
        return -1;
    }
    if (hwa_htdemucs_onnx_status(
            context,
            context->api->CreateTensorWithDataAsOrtValue(
                context->memory_info, (void *)input,
                input_count * sizeof(*input), input_shape, 3U,
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_value),
            "cannot create the HTDemucs ONNX input", error, error_size) != 0 ||
        hwa_htdemucs_onnx_status(
            context,
            context->api->CreateTensorWithDataAsOrtValue(
                context->memory_info, output,
                output_count * sizeof(*output), output_shape, 4U,
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &outputs[0]),
            "cannot create the HTDemucs ONNX output", error, error_size) != 0)
        goto cleanup;
    inputs[0] = input_value;
    if (hwa_htdemucs_onnx_status(
            context,
            context->api->Run(
                context->session, NULL, input_names, inputs, 1U,
                output_names, 1U, outputs),
            "HTDemucs ONNX inference failed", error, error_size) != 0)
        goto cleanup;
    for (index = 0U; index < output_count; ++index) {
        if (!isfinite((double)output[index])) {
            hwa_htdemucs_onnx_error(
                error, error_size,
                "HTDemucs ONNX output has a non-finite sample");
            goto cleanup;
        }
    }
    result = 0;
cleanup:
    context->api->ReleaseValue(outputs[0]);
    context->api->ReleaseValue(input_value);
    return result;
}

static int hwa_htdemucs_onnx_lower_hash(const char *text)
{
    size_t index;
    if (text == NULL) return 0;
    for (index = 0U; index < 64U; ++index) {
        char value = text[index];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f')))
            return 0;
    }
    return text[64] == '\0';
}
#endif

int hwa_inference_htdemucs_onnx_available(void)
{
#if HWA_HAVE_ONNX_RUNTIME
    return 1;
#else
    return 0;
#endif
}

int hwa_inference_htdemucs_onnx_runner_open(
    const char *model_path,
    const char *expected_model_sha256,
    uint64_t max_model_bytes,
    HWAHTDemucsModelRunner *runner,
    char model_sha256[65],
    char *error,
    size_t error_size)
{
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (runner != NULL) memset(runner, 0, sizeof(*runner));
    if (model_sha256 != NULL) memset(model_sha256, 0, 65U);
#if HWA_HAVE_ONNX_RUNTIME
    HWAInferenceHTDemucsOnnx *context = NULL;
    const OrtApiBase *base;
    const char *runtime_version;
    uint64_t model_read_limit;
    if (model_path == NULL || model_path[0] == '\0' ||
        strcmp(model_path, "-") == 0 || runner == NULL ||
        model_sha256 == NULL || max_model_bytes == 0U ||
        (expected_model_sha256 != NULL &&
         expected_model_sha256[0] != '\0' &&
         !hwa_htdemucs_onnx_lower_hash(expected_model_sha256))) {
        hwa_htdemucs_onnx_error(
            error, error_size, "invalid HTDemucs ONNX model arguments");
        return -1;
    }
    context = (HWAInferenceHTDemucsOnnx *)calloc(1U, sizeof(*context));
    if (context == NULL) {
        hwa_htdemucs_onnx_error(
            error, error_size, "cannot allocate HTDemucs ONNX context");
        goto cleanup;
    }
    base = OrtGetApiBase();
    if (base == NULL || base->GetApi == NULL ||
        (context->api = base->GetApi(ORT_API_VERSION)) == NULL) {
        runtime_version = base != NULL && base->GetVersionString != NULL
                              ? base->GetVersionString() : "unknown";
        if (error != NULL && error_size != 0U) {
            (void)snprintf(error, error_size,
                           "ONNX Runtime %s does not support API version %u",
                           runtime_version, (unsigned)ORT_API_VERSION);
            error[error_size - 1U] = '\0';
        }
        goto cleanup;
    }
    model_read_limit = max_model_bytes;
    if (model_read_limit > (uint64_t)INT_MAX)
        model_read_limit = (uint64_t)INT_MAX;
    if (hwa_sha256_read_file(
            model_path, model_read_limit, &context->model_bytes,
            &context->model_byte_count, model_sha256,
            error, error_size) != 0)
        goto cleanup;
    if (context->model_byte_count == 0U ||
        (uint64_t)context->model_byte_count > (uint64_t)INT_MAX) {
        hwa_htdemucs_onnx_error(
            error, error_size,
            context->model_byte_count == 0U
                ? "HTDemucs ONNX model is empty"
                : "HTDemucs ONNX model exceeds the runtime size limit");
        goto cleanup;
    }
    if (strcmp(model_sha256, HWA_HTDEMUCS_6S_MODEL_SHA256) != 0) {
        hwa_htdemucs_onnx_error(
            error, error_size,
            "HTDemucs ONNX model is not the pinned six-stem artifact");
        goto cleanup;
    }
    if (expected_model_sha256 != NULL &&
        expected_model_sha256[0] != '\0' &&
        strcmp(expected_model_sha256, model_sha256) != 0) {
        hwa_htdemucs_onnx_error(
            error, error_size,
            "HTDemucs ONNX model SHA-256 does not match");
        goto cleanup;
    }
    if (hwa_htdemucs_onnx_status(
            context,
            context->api->CreateEnv(
                ORT_LOGGING_LEVEL_WARNING, "hlolli-wg-analyzer",
                &context->environment),
            "cannot create the ONNX Runtime environment",
            error, error_size) != 0 ||
        hwa_htdemucs_onnx_status(
            context,
            context->api->CreateSessionOptions(&context->session_options),
            "cannot create ONNX Runtime session options",
            error, error_size) != 0 ||
        hwa_htdemucs_onnx_status(
            context,
            context->api->CreateSessionFromArray(
                context->environment, context->model_bytes,
                context->model_byte_count, context->session_options,
                &context->session),
            "cannot load the HTDemucs ONNX model", error, error_size) != 0 ||
        hwa_htdemucs_onnx_status(
            context,
            context->api->CreateCpuMemoryInfo(
                OrtArenaAllocator, OrtMemTypeDefault, &context->memory_info),
            "cannot create ONNX Runtime CPU memory metadata",
            error, error_size) != 0 ||
        hwa_htdemucs_onnx_model_contract(context, error, error_size) != 0)
        goto cleanup;
    runtime_version = base->GetVersionString();
    runner->context = context;
    runner->runtime_name = "onnxruntime";
    runner->runtime_version = runtime_version != NULL
                                  ? runtime_version : "unknown";
    runner->backend = "CPUExecutionProvider";
    runner->fallback = "";
    runner->run_window = hwa_htdemucs_onnx_run_window;
    runner->destroy = hwa_htdemucs_onnx_destroy;
    return 0;

cleanup:
    hwa_htdemucs_onnx_destroy(context);
    memset(model_sha256, 0, 65U);
    return -1;
#else
    (void)model_path;
    (void)expected_model_sha256;
    (void)max_model_bytes;
    if (error != NULL && error_size != 0U) {
        (void)snprintf(error, error_size,
                       "ONNX Runtime support is not compiled");
        error[error_size - 1U] = '\0';
    }
    return -1;
#endif
}

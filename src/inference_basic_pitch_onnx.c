#include "inference_basic_pitch_onnx.h"

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

#define HWA_BP_ONNX_INPUT_NAME "serving_default_input_2:0"
#define HWA_BP_ONNX_NOTE_NAME "StatefulPartitionedCall:1"
#define HWA_BP_ONNX_ONSET_NAME "StatefulPartitionedCall:2"
#define HWA_BP_ONNX_CONTOUR_NAME "StatefulPartitionedCall:0"

typedef struct HWAInferenceBasicPitchOnnx {
    const OrtApi *api;
    OrtEnv *environment;
    OrtSessionOptions *session_options;
    OrtSession *session;
    OrtMemoryInfo *memory_info;
    unsigned char *model_bytes;
    size_t model_byte_count;
} HWAInferenceBasicPitchOnnx;

static int hwa_bp_onnx_status(HWAInferenceBasicPitchOnnx *context,
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

static void hwa_bp_onnx_destroy(void *context_value)
{
    HWAInferenceBasicPitchOnnx *context =
        (HWAInferenceBasicPitchOnnx *)context_value;
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

static int hwa_bp_onnx_model_shape(
    HWAInferenceBasicPitchOnnx *context,
    size_t index,
    int output,
    int64_t second,
    int64_t third,
    char *error,
    size_t error_size)
{
    OrtTypeInfo *type_info = NULL;
    const OrtTensorTypeAndShapeInfo *tensor_info = NULL;
    enum ONNXType value_type = ONNX_TYPE_UNKNOWN;
    enum ONNXTensorElementDataType element_type =
        ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    int64_t dimensions[3] = {0, 0, 0};
    size_t rank = 0U;
    OrtStatus *status;
    int result = -1;
    status = output
                 ? context->api->SessionGetOutputTypeInfo(
                       context->session, index, &type_info)
                 : context->api->SessionGetInputTypeInfo(
                       context->session, index, &type_info);
    if (hwa_bp_onnx_status(context, status,
                           "cannot read ONNX tensor type",
                           error, error_size) != 0)
        goto cleanup;
    if (hwa_bp_onnx_status(
            context,
            context->api->GetOnnxTypeFromTypeInfo(type_info, &value_type),
            "cannot read ONNX value type", error, error_size) != 0 ||
        value_type != ONNX_TYPE_TENSOR ||
        hwa_bp_onnx_status(
            context,
            context->api->CastTypeInfoToTensorInfo(
                type_info, &tensor_info),
            "cannot read ONNX tensor metadata", error, error_size) != 0 ||
        tensor_info == NULL ||
        hwa_bp_onnx_status(
            context,
            context->api->GetTensorElementType(
                tensor_info, &element_type),
            "cannot read ONNX tensor element type", error, error_size) != 0 ||
        hwa_bp_onnx_status(
            context,
            context->api->GetDimensionsCount(tensor_info, &rank),
            "cannot read ONNX tensor rank", error, error_size) != 0 ||
        rank != 3U ||
        hwa_bp_onnx_status(
            context,
            context->api->GetDimensions(tensor_info, dimensions, 3U),
            "cannot read ONNX tensor dimensions", error, error_size) != 0 ||
        element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
        (dimensions[0] != 1 && dimensions[0] >= 0) ||
        dimensions[1] != second || dimensions[2] != third) {
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            (void)snprintf(error, error_size,
                           "Basic Pitch ONNX tensor has the wrong type or shape");
            error[error_size - 1U] = '\0';
        }
        goto cleanup;
    }
    result = 0;
cleanup:
    context->api->ReleaseTypeInfo(type_info);
    return result;
}

static int hwa_bp_onnx_name_index(const char *name)
{
    if (strcmp(name, HWA_BP_ONNX_NOTE_NAME) == 0) return 0;
    if (strcmp(name, HWA_BP_ONNX_ONSET_NAME) == 0) return 1;
    if (strcmp(name, HWA_BP_ONNX_CONTOUR_NAME) == 0) return 2;
    return -1;
}

static int hwa_bp_onnx_model_contract(
    HWAInferenceBasicPitchOnnx *context,
    char *error,
    size_t error_size)
{
    OrtAllocator *allocator = NULL;
    char *name = NULL;
    size_t input_count = 0U;
    size_t output_count = 0U;
    unsigned seen = 0U;
    size_t index;
    if (hwa_bp_onnx_status(
            context,
            context->api->SessionGetInputCount(
                context->session, &input_count),
            "cannot read ONNX input count", error, error_size) != 0 ||
        hwa_bp_onnx_status(
            context,
            context->api->SessionGetOutputCount(
                context->session, &output_count),
            "cannot read ONNX output count", error, error_size) != 0 ||
        input_count != 1U || output_count != 3U ||
        hwa_bp_onnx_status(
            context,
            context->api->GetAllocatorWithDefaultOptions(&allocator),
            "cannot get the ONNX allocator", error, error_size) != 0) {
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            (void)snprintf(error, error_size,
                           "Basic Pitch ONNX model has the wrong input or output count");
            error[error_size - 1U] = '\0';
        }
        return -1;
    }
    if (hwa_bp_onnx_status(
            context,
            context->api->SessionGetInputName(
                context->session, 0U, allocator, &name),
            "cannot read ONNX input name", error, error_size) != 0)
        return -1;
    if (strcmp(name, HWA_BP_ONNX_INPUT_NAME) != 0 ||
        hwa_bp_onnx_model_shape(
            context, 0U, 0,
            (int64_t)HWA_BASIC_PITCH_INPUT_SAMPLES, 1,
            error, error_size) != 0) {
        (void)context->api->AllocatorFree(allocator, name);
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            (void)snprintf(error, error_size,
                           "Basic Pitch ONNX model has the wrong input");
            error[error_size - 1U] = '\0';
        }
        return -1;
    }
    if (hwa_bp_onnx_status(
            context, context->api->AllocatorFree(allocator, name),
            "cannot free the ONNX input name", error, error_size) != 0)
        return -1;
    name = NULL;
    for (index = 0U; index < output_count; ++index) {
        int kind;
        int64_t width;
        if (hwa_bp_onnx_status(
                context,
                context->api->SessionGetOutputName(
                    context->session, index, allocator, &name),
                "cannot read ONNX output name", error, error_size) != 0)
            return -1;
        kind = hwa_bp_onnx_name_index(name);
        if (kind < 0 || (seen & (1U << (unsigned)kind)) != 0U) {
            (void)context->api->AllocatorFree(allocator, name);
            if (error != NULL && error_size != 0U) {
                (void)snprintf(error, error_size,
                               "Basic Pitch ONNX model has an unknown or repeated output");
                error[error_size - 1U] = '\0';
            }
            return -1;
        }
        if (hwa_bp_onnx_status(
                context, context->api->AllocatorFree(allocator, name),
                "cannot free an ONNX output name", error, error_size) != 0)
            return -1;
        name = NULL;
        width = kind == 2
                    ? (int64_t)HWA_BASIC_PITCH_CONTOUR_BINS
                    : (int64_t)HWA_BASIC_PITCH_NOTE_BINS;
        if (hwa_bp_onnx_model_shape(
                context, index, 1,
                (int64_t)HWA_BASIC_PITCH_OUTPUT_FRAMES, width,
                error, error_size) != 0)
            return -1;
        seen |= 1U << (unsigned)kind;
    }
    if (seen != 7U) {
        if (error != NULL && error_size != 0U) {
            (void)snprintf(error, error_size,
                           "Basic Pitch ONNX outputs are incomplete");
            error[error_size - 1U] = '\0';
        }
        return -1;
    }
    return 0;
}

static int hwa_bp_onnx_actual_tensor(
    HWAInferenceBasicPitchOnnx *context,
    OrtValue *value,
    size_t width,
    float *destination,
    char *error,
    size_t error_size)
{
    OrtTensorTypeAndShapeInfo *shape = NULL;
    enum ONNXTensorElementDataType element_type =
        ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    int64_t dimensions[3] = {0, 0, 0};
    size_t rank = 0U;
    size_t element_count = 0U;
    void *data = NULL;
    int is_tensor = 0;
    size_t index;
    size_t expected = HWA_BASIC_PITCH_OUTPUT_FRAMES * width;
    if (hwa_bp_onnx_status(
            context, context->api->IsTensor(value, &is_tensor),
            "cannot inspect an ONNX output", error, error_size) != 0 ||
        !is_tensor ||
        hwa_bp_onnx_status(
            context, context->api->GetTensorTypeAndShape(value, &shape),
            "cannot read an ONNX output shape", error, error_size) != 0 ||
        hwa_bp_onnx_status(
            context, context->api->GetTensorElementType(
                shape, &element_type),
            "cannot read an ONNX output type", error, error_size) != 0 ||
        hwa_bp_onnx_status(
            context, context->api->GetDimensionsCount(shape, &rank),
            "cannot read an ONNX output rank", error, error_size) != 0 ||
        rank != 3U ||
        hwa_bp_onnx_status(
            context, context->api->GetDimensions(shape, dimensions, 3U),
            "cannot read ONNX output dimensions", error, error_size) != 0 ||
        hwa_bp_onnx_status(
            context, context->api->GetTensorShapeElementCount(
                shape, &element_count),
            "cannot read ONNX output size", error, error_size) != 0 ||
        element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
        dimensions[0] != 1 ||
        dimensions[1] != (int64_t)HWA_BASIC_PITCH_OUTPUT_FRAMES ||
        dimensions[2] != (int64_t)width || element_count != expected ||
        hwa_bp_onnx_status(
            context, context->api->GetTensorMutableData(value, &data),
            "cannot read ONNX output data", error, error_size) != 0 ||
        data == NULL) {
        if (error != NULL && error_size != 0U && error[0] == '\0') {
            (void)snprintf(error, error_size,
                           "Basic Pitch ONNX output has the wrong type or shape");
            error[error_size - 1U] = '\0';
        }
        context->api->ReleaseTensorTypeAndShapeInfo(shape);
        return -1;
    }
    for (index = 0U; index < expected; ++index) {
        float activation = ((const float *)data)[index];
        if (!isfinite((double)activation) ||
            activation < 0.0f || activation > 1.0f) {
            context->api->ReleaseTensorTypeAndShapeInfo(shape);
            if (error != NULL && error_size != 0U) {
                (void)snprintf(error, error_size,
                               "Basic Pitch ONNX output has an invalid activation");
                error[error_size - 1U] = '\0';
            }
            return -1;
        }
    }
    memcpy(destination, data, expected * sizeof(*destination));
    context->api->ReleaseTensorTypeAndShapeInfo(shape);
    return 0;
}

static int hwa_bp_onnx_run_window(
    void *context_value,
    const float input[HWA_BASIC_PITCH_INPUT_SAMPLES],
    float note_output[
        HWA_BASIC_PITCH_OUTPUT_FRAMES * HWA_BASIC_PITCH_NOTE_BINS],
    float onset_output[
        HWA_BASIC_PITCH_OUTPUT_FRAMES * HWA_BASIC_PITCH_NOTE_BINS],
    char *error,
    size_t error_size)
{
    HWAInferenceBasicPitchOnnx *context =
        (HWAInferenceBasicPitchOnnx *)context_value;
    static const char *input_names[] = {HWA_BP_ONNX_INPUT_NAME};
    static const char *output_names[] = {
        HWA_BP_ONNX_NOTE_NAME, HWA_BP_ONNX_ONSET_NAME};
    const OrtValue *inputs[1];
    OrtValue *input_value = NULL;
    OrtValue *outputs[2] = {NULL, NULL};
    int64_t shape[3] = {
        1, (int64_t)HWA_BASIC_PITCH_INPUT_SAMPLES, 1};
    int result = -1;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (context == NULL || input == NULL ||
        note_output == NULL || onset_output == NULL) {
        if (error != NULL && error_size != 0U) {
            (void)snprintf(error, error_size,
                           "invalid Basic Pitch ONNX run arguments");
            error[error_size - 1U] = '\0';
        }
        return -1;
    }
    if (hwa_bp_onnx_status(
            context,
            context->api->CreateTensorWithDataAsOrtValue(
                context->memory_info, (void *)input,
                HWA_BASIC_PITCH_INPUT_SAMPLES * sizeof(*input),
                shape, 3U, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                &input_value),
            "cannot create the Basic Pitch ONNX input",
            error, error_size) != 0)
        goto cleanup;
    inputs[0] = input_value;
    if (hwa_bp_onnx_status(
            context,
            context->api->Run(
                context->session, NULL, input_names, inputs, 1U,
                output_names, 2U, outputs),
            "Basic Pitch ONNX inference failed", error, error_size) != 0 ||
        hwa_bp_onnx_actual_tensor(
            context, outputs[0], HWA_BASIC_PITCH_NOTE_BINS,
            note_output, error, error_size) != 0 ||
        hwa_bp_onnx_actual_tensor(
            context, outputs[1], HWA_BASIC_PITCH_NOTE_BINS,
            onset_output, error, error_size) != 0)
        goto cleanup;
    result = 0;
cleanup:
    context->api->ReleaseValue(outputs[1]);
    context->api->ReleaseValue(outputs[0]);
    context->api->ReleaseValue(input_value);
    return result;
}

static int hwa_bp_onnx_lower_hash(const char *text)
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

int hwa_inference_basic_pitch_onnx_available(void)
{
#if HWA_HAVE_ONNX_RUNTIME
    return 1;
#else
    return 0;
#endif
}

int hwa_inference_basic_pitch_onnx_runner_open(
    const char *model_path,
    const char *expected_model_sha256,
    uint64_t max_model_bytes,
    HWABasicPitchRunner *runner,
    char model_sha256[65],
    char *error,
    size_t error_size)
{
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (runner != NULL) memset(runner, 0, sizeof(*runner));
    if (model_sha256 != NULL) memset(model_sha256, 0, 65U);
#if HWA_HAVE_ONNX_RUNTIME
    HWAInferenceBasicPitchOnnx *context = NULL;
    const OrtApiBase *base;
    const char *runtime_version;
    uint64_t model_read_limit;
    if (model_path == NULL || model_path[0] == '\0' ||
        strcmp(model_path, "-") == 0 || runner == NULL ||
        model_sha256 == NULL || max_model_bytes == 0U ||
        (expected_model_sha256 != NULL &&
         expected_model_sha256[0] != '\0' &&
         !hwa_bp_onnx_lower_hash(expected_model_sha256))) {
        if (error != NULL && error_size != 0U) {
            (void)snprintf(error, error_size,
                           "invalid Basic Pitch ONNX model arguments");
            error[error_size - 1U] = '\0';
        }
        return -1;
    }
    context = (HWAInferenceBasicPitchOnnx *)calloc(1U, sizeof(*context));
    if (context == NULL) goto allocation_failed;
    base = OrtGetApiBase();
    if (base == NULL || base->GetApi == NULL ||
        (context->api = base->GetApi(ORT_API_VERSION)) == NULL) {
        if (error != NULL && error_size != 0U) {
            runtime_version = base != NULL && base->GetVersionString != NULL
                                  ? base->GetVersionString() : "unknown";
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
        if (error != NULL && error_size != 0U) {
            const char *message = context->model_byte_count == 0U
                                      ? "Basic Pitch ONNX model is empty"
                                      : "Basic Pitch ONNX model exceeds the runtime size limit";
            (void)snprintf(error, error_size, "%s", message);
            error[error_size - 1U] = '\0';
        }
        goto cleanup;
    }
    if (expected_model_sha256 != NULL &&
        expected_model_sha256[0] != '\0' &&
        strcmp(expected_model_sha256, model_sha256) != 0) {
        if (error != NULL && error_size != 0U) {
            (void)snprintf(error, error_size,
                           "Basic Pitch ONNX model SHA-256 does not match");
            error[error_size - 1U] = '\0';
        }
        goto cleanup;
    }
    if (hwa_bp_onnx_status(
            context,
            context->api->CreateEnv(
                ORT_LOGGING_LEVEL_WARNING, "hlolli-wg-analyzer",
                &context->environment),
            "cannot create the ONNX Runtime environment",
            error, error_size) != 0 ||
        hwa_bp_onnx_status(
            context,
            context->api->CreateSessionOptions(&context->session_options),
            "cannot create ONNX Runtime session options",
            error, error_size) != 0 ||
        hwa_bp_onnx_status(
            context,
            context->api->CreateSessionFromArray(
                context->environment, context->model_bytes,
                context->model_byte_count, context->session_options,
                &context->session),
            "cannot load the Basic Pitch ONNX model",
            error, error_size) != 0 ||
        hwa_bp_onnx_status(
            context,
            context->api->CreateCpuMemoryInfo(
                OrtArenaAllocator, OrtMemTypeDefault,
                &context->memory_info),
            "cannot create ONNX Runtime CPU memory metadata",
            error, error_size) != 0 ||
        hwa_bp_onnx_model_contract(context, error, error_size) != 0)
        goto cleanup;
    runtime_version = base->GetVersionString();
    runner->context = context;
    runner->runtime_name = "onnxruntime";
    runner->runtime_version = runtime_version != NULL ? runtime_version : "unknown";
    runner->backend = "CPUExecutionProvider";
    runner->fallback = "";
    runner->run_window = hwa_bp_onnx_run_window;
    runner->destroy = hwa_bp_onnx_destroy;
    return 0;

allocation_failed:
    if (error != NULL && error_size != 0U) {
        (void)snprintf(error, error_size,
                       "cannot allocate Basic Pitch ONNX context");
        error[error_size - 1U] = '\0';
    }
cleanup:
    hwa_bp_onnx_destroy(context);
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

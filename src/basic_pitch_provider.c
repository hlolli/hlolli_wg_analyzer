#include "basic_pitch_provider.h"

#include "inference_clock.h"
#include "inference_provenance.h"
#include "internal.h"
#include "numeric_locale.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HWA_BASIC_PITCH_IO_BLOCK_FRAMES 4096U
#define HWA_BASIC_PITCH_SETTINGS_BYTES 1024U
#define HWA_BASIC_PITCH_PROVENANCE_BYTES 65536U
#define HWA_BASIC_PITCH_TEXT_BYTES 4096U
#define HWA_BASIC_PITCH_JSON_SAFE_INTEGER UINT64_C(9007199254740991)
#define HWA_BASIC_PITCH_FRAME_RATE_NUMERATOR UINT64_C(11025)
#define HWA_BASIC_PITCH_FRAME_RATE_DENOMINATOR UINT64_C(128)
#define HWA_BASIC_PITCH_PROVIDER_ID UINT64_C(1)
#define HWA_BASIC_PITCH_MAPPING_RULE \
    "model-frame-boundary-start-floor-end-ceil-clip-v1"

typedef struct HWABasicPitchProviderContext {
    char model_sha256[HWA_SHA256_HEX_SIZE];
    char adapter_sha256[HWA_SHA256_HEX_SIZE];
    char *runtime_name;
    char *runtime_version;
    char *backend;
    char *fallback;
    char *task_settings;
    HWABasicPitchDecoderOptions decoder_options;
    uint64_t max_work_bytes;
    void *runner_context;
    HWABasicPitchRunWindowFunction run_window;
    HWABasicPitchRunnerDestroyFunction runner_destroy;
} HWABasicPitchProviderContext;

typedef struct HWABasicPitchTask {
    HWAInferenceOutput output;
    HWAEventBundle bundle;
} HWABasicPitchTask;

static int hwa_basic_pitch_provider_start(void *context,
                                          const HWAInferenceRequest *request,
                                          void **task,
                                          char *error,
                                          size_t error_size);
static int hwa_basic_pitch_provider_poll(
    void *context,
    void *task,
    HWAInferencePollState *state,
    const HWAInferenceOutput **output,
    char *error,
    size_t error_size);
static void hwa_basic_pitch_provider_task_free(void *context, void *task);
static void hwa_basic_pitch_provider_destroy(void *context);

static void hwa_basic_pitch_provider_error(char *error,
                                           size_t error_size,
                                           const char *message)
{
    if (error == NULL || error_size == 0U) return;
    (void)snprintf(error, error_size, "%s", message);
    error[error_size - 1U] = '\0';
}

static int hwa_basic_pitch_text_size(const char *text,
                                     int allow_empty,
                                     size_t *size)
{
    size_t index;
    if (text == NULL) return -1;
    for (index = 0U; index < HWA_BASIC_PITCH_TEXT_BYTES; ++index) {
        if (text[index] == '\0') {
            if (!allow_empty && index == 0U) return -1;
            if (size != NULL) *size = index;
            return 0;
        }
    }
    return -1;
}

static char *hwa_basic_pitch_copy_text(const char *text, int allow_empty)
{
    char *copy;
    size_t size;
    if (hwa_basic_pitch_text_size(text, allow_empty, &size) != 0 ||
        size == SIZE_MAX)
        return NULL;
    copy = (char *)malloc(size + 1U);
    if (copy != NULL) memcpy(copy, text, size + 1U);
    return copy;
}

static int hwa_basic_pitch_lower_sha256(const char *text)
{
    size_t index;
    if (text == NULL) return 0;
    for (index = 0U; index < 64U; ++index) {
        unsigned char value = (unsigned char)text[index];
        if (!((value >= (unsigned char)'0' &&
               value <= (unsigned char)'9') ||
              (value >= (unsigned char)'a' &&
               value <= (unsigned char)'f')))
            return 0;
    }
    return text[64] == '\0';
}

static int hwa_basic_pitch_json_space(unsigned char value)
{
    return value == (unsigned char)' ' || value == (unsigned char)'\t' ||
           value == (unsigned char)'\n' || value == (unsigned char)'\r';
}

/* Compare canonical JSON tokens while allowing JSON white space. */
static int hwa_basic_pitch_settings_match(const char *left,
                                          const char *right)
{
    int in_string = 0;
    int escaped = 0;
    if (left == NULL || right == NULL) return 0;
    for (;;) {
        unsigned char left_value;
        unsigned char right_value;
        if (!in_string) {
            while (hwa_basic_pitch_json_space((unsigned char)*left)) left++;
            while (hwa_basic_pitch_json_space((unsigned char)*right)) right++;
        }
        left_value = (unsigned char)*left;
        right_value = (unsigned char)*right;
        if (left_value != right_value) return 0;
        if (left_value == (unsigned char)'\0') return 1;
        if (in_string) {
            if (escaped) {
                escaped = 0;
            } else if (left_value == (unsigned char)'\\') {
                escaped = 1;
            } else if (left_value == (unsigned char)'"') {
                in_string = 0;
            }
        } else if (left_value == (unsigned char)'"') {
            in_string = 1;
        }
        left++;
        right++;
    }
}

static int hwa_basic_pitch_u64_add(uint64_t *total,
                                   uint64_t value,
                                   uint64_t maximum)
{
    if (total == NULL || *total > maximum || value > maximum - *total)
        return -1;
    *total += value;
    return 0;
}

static int hwa_basic_pitch_size_bytes(size_t count,
                                      size_t item_size,
                                      size_t *bytes)
{
    if (bytes == NULL || (item_size != 0U && count > SIZE_MAX / item_size))
        return -1;
    *bytes = count * item_size;
    return 0;
}

static int hwa_basic_pitch_add_allocation(uint64_t *total,
                                          size_t count,
                                          size_t item_size,
                                          uint64_t maximum)
{
    size_t bytes;
    if (hwa_basic_pitch_size_bytes(count, item_size, &bytes) != 0)
        return -1;
    return hwa_basic_pitch_u64_add(total, (uint64_t)bytes, maximum);
}

static int hwa_basic_pitch_size_exceeds_json_integer(size_t value)
{
#if SIZE_MAX > HWA_BASIC_PITCH_JSON_SAFE_INTEGER
    return value > (size_t)HWA_BASIC_PITCH_JSON_SAFE_INTEGER;
#else
    (void)value;
    return 0;
#endif
}

static size_t hwa_basic_pitch_limit_json_integer(size_t value)
{
#if SIZE_MAX > HWA_BASIC_PITCH_JSON_SAFE_INTEGER
    if (value > (size_t)HWA_BASIC_PITCH_JSON_SAFE_INTEGER)
        return (size_t)HWA_BASIC_PITCH_JSON_SAFE_INTEGER;
#endif
    return value;
}

static int hwa_basic_pitch_size_exceeds_u64_half(size_t value)
{
#if SIZE_MAX > UINT64_MAX / 2U
    return value > (size_t)(UINT64_MAX / 2U);
#else
    (void)value;
    return 0;
#endif
}

int hwa_basic_pitch_task_settings_build(
    const HWABasicPitchDecoderOptions *decoder_options,
    char **settings_json,
    char *error,
    size_t error_size)
{
    HWANumericLocale locale;
    char onset_threshold[64];
    char frame_threshold[64];
    char *settings = NULL;
    int length;
    int locale_active = 0;
    int status = -1;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (settings_json == NULL) {
        hwa_basic_pitch_provider_error(
            error, error_size, "Basic Pitch settings output is null");
        return -1;
    }
    *settings_json = NULL;
    if (hwa_basic_pitch_decoder_options_validate(
            decoder_options, error, error_size) != 0)
        return -1;
    if (hwa_basic_pitch_size_exceeds_json_integer(
            decoder_options->minimum_note_frames) ||
        hwa_basic_pitch_size_exceeds_json_integer(
            decoder_options->energy_tolerance_frames)) {
        hwa_basic_pitch_provider_error(
            error, error_size,
            "Basic Pitch decoder counts exceed the JSON integer limit");
        return -1;
    }
    memset(&locale, 0, sizeof(locale));
    if (hwa_c_numeric_locale_begin(&locale) != 0) {
        hwa_basic_pitch_provider_error(
            error, error_size, "cannot enter the C numeric locale");
        return -1;
    }
    locale_active = 1;
    if (hwa_c_locale_format_double(
            &locale, onset_threshold, sizeof(onset_threshold),
            decoder_options->onset_threshold) != 0 ||
        hwa_c_locale_format_double(
            &locale, frame_threshold, sizeof(frame_threshold),
            decoder_options->frame_threshold) != 0) {
        hwa_basic_pitch_provider_error(
            error, error_size, "cannot format Basic Pitch thresholds");
        goto cleanup;
    }
    if (hwa_c_numeric_locale_end(&locale) != 0) {
        hwa_basic_pitch_provider_error(
            error, error_size, "cannot restore the numeric locale");
        return -1;
    }
    locale_active = 0;
    settings = (char *)malloc(HWA_BASIC_PITCH_SETTINGS_BYTES);
    if (settings == NULL) {
        hwa_basic_pitch_provider_error(
            error, error_size, "cannot allocate Basic Pitch settings");
        return -1;
    }
    length = snprintf(
        settings, HWA_BASIC_PITCH_SETTINGS_BYTES,
        "{\"thresholds\":{\"energy_tolerance_frames\":%zu,"
        "\"frame_threshold\":%s,\"minimum_note_frames\":%zu,"
        "\"onset_threshold\":%s},\"decoder\":{\"infer_onsets\":%s,"
        "\"melodia\":%s},\"model_frame_rate\":{\"numerator\":11025,"
        "\"denominator\":128},\"mapping_rule\":\"%s\","
        "\"window_schedule\":{\"crop_frames_each_side\":15,"
        "\"input_samples\":43844,\"kept_frames\":142,"
        "\"left_pad_samples\":3840,\"output_frames\":172,"
        "\"step_samples\":36352}}",
        decoder_options->energy_tolerance_frames, frame_threshold,
        decoder_options->minimum_note_frames, onset_threshold,
        decoder_options->infer_onsets ? "true" : "false",
        decoder_options->melodia ? "true" : "false",
        HWA_BASIC_PITCH_MAPPING_RULE);
    if (length < 0 || (size_t)length >= HWA_BASIC_PITCH_SETTINGS_BYTES) {
        hwa_basic_pitch_provider_error(
            error, error_size, "Basic Pitch settings exceed their limit");
        goto cleanup;
    }
    *settings_json = settings;
    settings = NULL;
    status = 0;
cleanup:
    if (locale_active && hwa_c_numeric_locale_end(&locale) != 0 &&
        status == 0) {
        hwa_basic_pitch_provider_error(
            error, error_size, "cannot restore the numeric locale");
        status = -1;
    }
    free(settings);
    return status;
}

static void hwa_basic_pitch_descriptor(
    HWABasicPitchProviderContext *context,
    HWAInferenceProvider *provider)
{
    memset(provider, 0, sizeof(*provider));
    provider->name = HWA_BASIC_PITCH_PROVIDER_NAME;
    provider->version = HWA_BASIC_PITCH_PROVIDER_VERSION;
    provider->model_sha256 = context->model_sha256;
    provider->context = context;
    provider->start = hwa_basic_pitch_provider_start;
    provider->poll = hwa_basic_pitch_provider_poll;
    provider->task_free = hwa_basic_pitch_provider_task_free;
    provider->destroy = hwa_basic_pitch_provider_destroy;
}

static const HWAInferenceInput *hwa_basic_pitch_source(
    const HWAInferenceRequest *request)
{
    size_t index;
    if (request == NULL || request->inputs == NULL ||
        request->source_input_id == NULL)
        return NULL;
    for (index = 0U; index < request->input_count; ++index) {
        if (strcmp(request->inputs[index].id,
                   request->source_input_id) == 0)
            return &request->inputs[index];
    }
    return NULL;
}

static int hwa_basic_pitch_decode_frames(HWAWavReader *reader,
                                         float *destination,
                                         size_t wanted,
                                         unsigned char *raw,
                                         size_t raw_frame_capacity,
                                         char *error,
                                         size_t error_size)
{
    size_t written = 0U;
    while (written < wanted && reader->bytes_remaining != 0U) {
        size_t request = wanted - written;
        size_t got = 0U;
        size_t frame;
        if (request > raw_frame_capacity) request = raw_frame_capacity;
        if (hwa_wav_reader_read_frames(
                reader, raw, request, &got, error, error_size) != 0)
            return -1;
        if (got == 0U) {
            hwa_basic_pitch_provider_error(
                error, error_size, "WAVE input ended before its data chunk");
            return -1;
        }
        for (frame = 0U; frame < got; ++frame) {
            int clipped;
            double value = hwa_wav_decode_sample(
                reader, raw + frame * reader->format.block_align, &clipped);
            (void)clipped;
            if (!isfinite(value)) {
                hwa_basic_pitch_provider_error(
                    error, error_size,
                    "Basic Pitch input contains a non-finite sample");
                return -1;
            }
            destination[written + frame] = (float)value;
            if (!isfinite((double)destination[written + frame])) {
                hwa_basic_pitch_provider_error(
                    error, error_size,
                    "Basic Pitch input sample exceeds float32 range");
                return -1;
            }
        }
        written += got;
    }
    return 0;
}

static int hwa_basic_pitch_activations_valid(const float *values,
                                              size_t count)
{
    size_t index;
    if (values == NULL && count != 0U) return 0;
    for (index = 0U; index < count; ++index) {
        double value = (double)values[index];
        if (!isfinite(value) || value < 0.0 || value > 1.0) return 0;
    }
    return 1;
}

static int hwa_basic_pitch_collect_activations(
    HWABasicPitchProviderContext *context,
    const HWAInferenceRequest *request,
    const HWAInferenceInput *source,
    uint64_t deadline_started,
    float **note_result,
    float **onset_result,
    size_t *frame_count_result,
    char *error,
    size_t error_size)
{
    HWAWavReader reader;
    float *notes = NULL;
    float *onsets = NULL;
    float *window = NULL;
    float *window_notes = NULL;
    float *window_onsets = NULL;
    unsigned char *raw = NULL;
    size_t frame_count;
    size_t cell_count;
    size_t matrix_bytes;
    size_t window_cell_count;
    size_t window_matrix_bytes;
    size_t input_bytes;
    size_t raw_bytes;
    size_t window_count;
    size_t window_index;
    uint64_t work = 0U;
    int reader_open = 0;
    int status = -1;
    memset(&reader, 0, sizeof(reader));
    *note_result = NULL;
    *onset_result = NULL;
    *frame_count_result = 0U;
    if (hwa_inference_deadline_check(
            deadline_started, request->timeout_milliseconds,
            error, error_size) != 0)
        return -1;
    if (request->source_format.channels != 1U ||
        request->source_format.sample_rate_hz !=
            HWA_BASIC_PITCH_SAMPLE_RATE) {
        hwa_basic_pitch_provider_error(
            error, error_size,
            "Basic Pitch requires an exact mono 22050 Hz WAVE source");
        return -1;
    }
    if (request->source_format.frames /
            (uint64_t)HWA_BASIC_PITCH_FRAME_SAMPLES >
        (uint64_t)SIZE_MAX) {
        hwa_basic_pitch_provider_error(
            error, error_size, "Basic Pitch frame count overflows this host");
        return -1;
    }
    frame_count = (size_t)(request->source_format.frames /
                           (uint64_t)HWA_BASIC_PITCH_FRAME_SAMPLES);
    if (hwa_basic_pitch_size_bytes(
            frame_count, HWA_BASIC_PITCH_NOTE_BINS, &cell_count) != 0 ||
        hwa_basic_pitch_size_bytes(
            cell_count, sizeof(float), &matrix_bytes) != 0) {
        hwa_basic_pitch_provider_error(
            error, error_size, "Basic Pitch activation size overflows");
        return -1;
    }
    if (frame_count == 0U) {
        *frame_count_result = 0U;
        return 0;
    }
    window_cell_count = HWA_BASIC_PITCH_OUTPUT_FRAMES *
                        HWA_BASIC_PITCH_NOTE_BINS;
    if (hwa_basic_pitch_size_bytes(
            window_cell_count, sizeof(float), &window_matrix_bytes) != 0 ||
        hwa_basic_pitch_size_bytes(
            HWA_BASIC_PITCH_INPUT_SAMPLES, sizeof(float), &input_bytes) != 0 ||
        hwa_basic_pitch_size_bytes(
            HWA_BASIC_PITCH_IO_BLOCK_FRAMES,
            request->source_format.block_align, &raw_bytes) != 0 ||
        hwa_basic_pitch_add_allocation(
            &work, 2U, matrix_bytes, context->max_work_bytes) != 0 ||
        hwa_basic_pitch_u64_add(
            &work, (uint64_t)input_bytes, context->max_work_bytes) != 0 ||
        hwa_basic_pitch_add_allocation(
            &work, 2U, window_matrix_bytes,
            context->max_work_bytes) != 0 ||
        hwa_basic_pitch_u64_add(
            &work, (uint64_t)raw_bytes, context->max_work_bytes) != 0) {
        hwa_basic_pitch_provider_error(
            error, error_size,
            "Basic Pitch inference exceeds its work limit");
        return -1;
    }
    if (frame_count > SIZE_MAX - (HWA_BASIC_PITCH_KEPT_FRAMES - 1U)) {
        hwa_basic_pitch_provider_error(
            error, error_size, "Basic Pitch window count overflows");
        return -1;
    }
    window_count =
        (frame_count + HWA_BASIC_PITCH_KEPT_FRAMES - 1U) /
        HWA_BASIC_PITCH_KEPT_FRAMES;
    notes = (float *)malloc(matrix_bytes);
    onsets = (float *)malloc(matrix_bytes);
    window = (float *)calloc(HWA_BASIC_PITCH_INPUT_SAMPLES, sizeof(float));
    window_notes = (float *)malloc(window_matrix_bytes);
    window_onsets = (float *)malloc(window_matrix_bytes);
    raw = (unsigned char *)malloc(raw_bytes);
    if (notes == NULL || onsets == NULL || window == NULL ||
        window_notes == NULL || window_onsets == NULL || raw == NULL) {
        hwa_basic_pitch_provider_error(
            error, error_size,
            "cannot allocate Basic Pitch inference work");
        goto cleanup;
    }
    if (hwa_wav_reader_open_source(
            &reader, &source->bytes, request->max_input_file_bytes,
            error, error_size) != 0)
        goto cleanup;
    reader_open = 1;
    if (reader.format.channels != 1U ||
        reader.format.sample_rate_hz != HWA_BASIC_PITCH_SAMPLE_RATE) {
        hwa_basic_pitch_provider_error(
            error, error_size,
            "Basic Pitch WAVE reader produced an unsupported clock");
        goto cleanup;
    }
    if (hwa_inference_deadline_check(
            deadline_started, request->timeout_milliseconds,
            error, error_size) != 0)
        goto cleanup;
    if (hwa_basic_pitch_decode_frames(
            &reader, window + HWA_BASIC_PITCH_LEFT_PAD_SAMPLES,
            HWA_BASIC_PITCH_INPUT_SAMPLES -
                HWA_BASIC_PITCH_LEFT_PAD_SAMPLES,
            raw, HWA_BASIC_PITCH_IO_BLOCK_FRAMES,
            error, error_size) != 0)
        goto cleanup;
    for (window_index = 0U; window_index < window_count; ++window_index) {
        size_t destination_frame =
            window_index * HWA_BASIC_PITCH_KEPT_FRAMES;
        size_t kept_frames = frame_count - destination_frame;
        size_t output_index;
        if (hwa_inference_deadline_check(
                deadline_started, request->timeout_milliseconds,
                error, error_size) != 0)
            goto cleanup;
        if (window_index != 0U) {
            size_t overlap = HWA_BASIC_PITCH_INPUT_SAMPLES -
                             HWA_BASIC_PITCH_WINDOW_STEP_SAMPLES;
            memmove(window,
                    window + HWA_BASIC_PITCH_WINDOW_STEP_SAMPLES,
                    overlap * sizeof(*window));
            memset(window + overlap, 0,
                   HWA_BASIC_PITCH_WINDOW_STEP_SAMPLES * sizeof(*window));
            if (hwa_basic_pitch_decode_frames(
                    &reader, window + overlap,
                    HWA_BASIC_PITCH_WINDOW_STEP_SAMPLES,
                    raw, HWA_BASIC_PITCH_IO_BLOCK_FRAMES,
                    error, error_size) != 0)
                goto cleanup;
        }
        if (hwa_inference_deadline_check(
                deadline_started, request->timeout_milliseconds,
                error, error_size) != 0)
            goto cleanup;
        for (output_index = 0U; output_index < window_cell_count;
             ++output_index) {
            window_notes[output_index] = NAN;
            window_onsets[output_index] = NAN;
        }
        if (context->run_window(
                context->runner_context, window,
                window_notes, window_onsets,
                error, error_size) != 0) {
            if (error != NULL && error_size != 0U && error[0] == '\0')
                hwa_basic_pitch_provider_error(
                    error, error_size, "Basic Pitch runner failed");
            goto cleanup;
        }
        if (hwa_inference_deadline_check(
                deadline_started, request->timeout_milliseconds,
                error, error_size) != 0)
            goto cleanup;
        if (!hwa_basic_pitch_activations_valid(
                window_notes, window_cell_count) ||
            !hwa_basic_pitch_activations_valid(
                window_onsets, window_cell_count)) {
            hwa_basic_pitch_provider_error(
                error, error_size,
                "Basic Pitch runner returned invalid activations");
            goto cleanup;
        }
        if (kept_frames > HWA_BASIC_PITCH_KEPT_FRAMES)
            kept_frames = HWA_BASIC_PITCH_KEPT_FRAMES;
        memcpy(notes + destination_frame * HWA_BASIC_PITCH_NOTE_BINS,
               window_notes + HWA_BASIC_PITCH_CROP_FRAMES *
                                  HWA_BASIC_PITCH_NOTE_BINS,
               kept_frames * HWA_BASIC_PITCH_NOTE_BINS * sizeof(*notes));
        memcpy(onsets + destination_frame * HWA_BASIC_PITCH_NOTE_BINS,
               window_onsets + HWA_BASIC_PITCH_CROP_FRAMES *
                                   HWA_BASIC_PITCH_NOTE_BINS,
               kept_frames * HWA_BASIC_PITCH_NOTE_BINS * sizeof(*onsets));
    }
    if (reader.bytes_remaining != 0U) {
        hwa_basic_pitch_provider_error(
            error, error_size,
            "Basic Pitch window schedule did not consume the source");
        goto cleanup;
    }
    *note_result = notes;
    *onset_result = onsets;
    *frame_count_result = frame_count;
    notes = NULL;
    onsets = NULL;
    status = 0;
cleanup:
    if (reader_open) hwa_wav_reader_close(&reader);
    free(raw);
    free(window_onsets);
    free(window_notes);
    free(window);
    free(onsets);
    free(notes);
    return status;
}

static size_t hwa_basic_pitch_note_capacity(size_t count,
                                            size_t maximum)
{
    size_t capacity = 0U;
    while (capacity < count) {
        size_t next;
        if (capacity == 0U)
            next = 64U;
        else if (capacity > SIZE_MAX / 2U)
            next = SIZE_MAX;
        else
            next = capacity * 2U;
        if (next > maximum) next = maximum;
        if (next <= capacity) return maximum;
        capacity = next;
    }
    return capacity;
}

static int hwa_basic_pitch_output_work(
    const char *source_name,
    const char *settings_json,
    size_t note_count,
    uint64_t maximum,
    uint64_t *work_result)
{
    uint64_t work = 0U;
    size_t source_name_size;
    size_t settings_size;
    size_t event_string_bytes =
        sizeof("note") + sizeof("") + sizeof("") + sizeof("") +
        sizeof("pitch-hz") + sizeof("Hz");
    if (work_result == NULL ||
        hwa_basic_pitch_text_size(source_name, 0, &source_name_size) != 0 ||
        settings_json == NULL)
        return -1;
    settings_size = strlen(settings_json);
    if (settings_size == SIZE_MAX) return -1;
    if (hwa_basic_pitch_u64_add(
            &work, (uint64_t)sizeof(HWABasicPitchTask), maximum) != 0 ||
        hwa_basic_pitch_add_allocation(
            &work, 1U, sizeof(HWAEventProvider), maximum) != 0 ||
        hwa_basic_pitch_add_allocation(
            &work, 1U, sizeof(HWAEventAudio), maximum) != 0 ||
        hwa_basic_pitch_add_allocation(
            &work, note_count, sizeof(HWAPerformanceEvent), maximum) != 0 ||
        hwa_basic_pitch_add_allocation(
            &work, note_count, sizeof(HWAEventValue), maximum) != 0 ||
        hwa_basic_pitch_u64_add(
            &work, (uint64_t)sizeof(HWA_BASIC_PITCH_PROVIDER_NAME),
            maximum) != 0 ||
        hwa_basic_pitch_u64_add(
            &work, (uint64_t)sizeof(HWA_BASIC_PITCH_PROVIDER_VERSION),
            maximum) != 0 ||
        hwa_basic_pitch_u64_add(
            &work, (uint64_t)settings_size + 1U, maximum) != 0 ||
        hwa_basic_pitch_u64_add(
            &work, (uint64_t)source_name_size + 1U, maximum) != 0 ||
        hwa_basic_pitch_u64_add(&work, 2U, maximum) != 0 ||
        hwa_basic_pitch_add_allocation(
            &work, note_count, event_string_bytes, maximum) != 0) {
        return -1;
    }
    *work_result = work;
    return 0;
}

static int hwa_basic_pitch_build_task(
    HWABasicPitchProviderContext *context,
    const HWAInferenceRequest *request,
    const HWAInferenceInput *source,
    const HWABasicPitchNote *notes,
    size_t note_count,
    size_t note_capacity,
    char **settings_json,
    HWABasicPitchTask **task_result,
    char *error,
    size_t error_size)
{
    HWABasicPitchTask *task = NULL;
    HWAEventProvider *provider;
    HWAEventAudio *audio;
    uint64_t work;
    size_t note_storage_bytes;
    size_t index;
    if (request->output_limits.max_audio_files < 1U ||
        request->output_limits.max_providers < 1U ||
        note_count > request->output_limits.max_events ||
        note_count > request->output_limits.max_values) {
        hwa_basic_pitch_provider_error(
            error, error_size, "Basic Pitch output count exceeds its limit");
        return -1;
    }
    if (hwa_basic_pitch_output_work(
            source->bytes.name, *settings_json, note_count,
            context->max_work_bytes, &work) != 0 ||
        hwa_basic_pitch_size_bytes(
            note_capacity, sizeof(*notes), &note_storage_bytes) != 0 ||
        hwa_basic_pitch_u64_add(
            &work, (uint64_t)note_storage_bytes,
            context->max_work_bytes) != 0) {
        hwa_basic_pitch_provider_error(
            error, error_size,
            "Basic Pitch output exceeds its work limit");
        return -1;
    }
    task = (HWABasicPitchTask *)calloc(1U, sizeof(*task));
    if (task == NULL) goto allocation_failed;
    task->bundle.providers =
        (HWAEventProvider *)calloc(1U, sizeof(*task->bundle.providers));
    task->bundle.audio =
        (HWAEventAudio *)calloc(1U, sizeof(*task->bundle.audio));
    if (task->bundle.providers == NULL || task->bundle.audio == NULL)
        goto allocation_failed;
    task->bundle.provider_count = 1U;
    task->bundle.audio_count = 1U;
    if (note_count != 0U) {
        task->bundle.events = (HWAPerformanceEvent *)calloc(
            note_count, sizeof(*task->bundle.events));
        if (task->bundle.events == NULL) goto allocation_failed;
        task->bundle.event_count = note_count;
    }
    provider = &task->bundle.providers[0];
    audio = &task->bundle.audio[0];
    provider->id = HWA_BASIC_PITCH_PROVIDER_ID;
    provider->name = hwa_basic_pitch_copy_text(
        HWA_BASIC_PITCH_PROVIDER_NAME, 0);
    provider->version = hwa_basic_pitch_copy_text(
        HWA_BASIC_PITCH_PROVIDER_VERSION, 0);
    provider->settings_json = *settings_json;
    *settings_json = NULL;
    memcpy(provider->model_sha256, context->model_sha256,
           HWA_SHA256_HEX_SIZE);
    audio->id = request->source_recording_id;
    audio->kind = HWA_EVENT_SOURCE_RECORDING;
    audio->name = hwa_basic_pitch_copy_text(source->bytes.name, 0);
    audio->relative_path = hwa_basic_pitch_copy_text("", 1);
    audio->path_hint = hwa_basic_pitch_copy_text("", 1);
    memcpy(audio->sha256, source->sha256, HWA_SHA256_HEX_SIZE);
    audio->file_bytes = source->bytes.size;
    audio->format = request->source_format;
    if (provider->name == NULL || provider->version == NULL ||
        provider->settings_json == NULL || audio->name == NULL ||
        audio->relative_path == NULL || audio->path_hint == NULL)
        goto allocation_failed;
    for (index = 0U; index < note_count; ++index) {
        HWAPerformanceEvent *event = &task->bundle.events[index];
        HWAEventValue *value;
        uint64_t start_sample;
        uint64_t end_sample;
        if (hwa_inference_frame_span_to_samples(
                notes[index].start_frame, notes[index].end_frame,
                HWA_BASIC_PITCH_FRAME_RATE_NUMERATOR,
                HWA_BASIC_PITCH_FRAME_RATE_DENOMINATOR,
                request->source_format.sample_rate_hz,
                request->source_format.frames,
                &start_sample, &end_sample,
                error, error_size) != 0)
            goto failure;
        event->values =
            (HWAEventValue *)calloc(1U, sizeof(*event->values));
        if (event->values == NULL) goto allocation_failed;
        event->value_count = 1U;
        value = &event->values[0];
        event->id = (uint64_t)index + 1U;
        event->kind = hwa_basic_pitch_copy_text("note", 0);
        event->source_recording_id = request->source_recording_id;
        event->evidence_audio_id = request->source_recording_id;
        event->evidence_audio_id_valid = 1;
        event->start_sample = start_sample;
        event->end_sample = end_sample;
        event->voice = hwa_basic_pitch_copy_text("", 1);
        event->part = hwa_basic_pitch_copy_text("", 1);
        event->score_event_id = hwa_basic_pitch_copy_text("", 1);
        value->name = hwa_basic_pitch_copy_text("pitch-hz", 0);
        value->kind = HWA_EVENT_VALUE_F64;
        value->basis = HWA_EVENT_INFERENCE;
        value->number = 440.0 * pow(
            2.0, ((double)notes[index].midi_note - 69.0) / 12.0);
        value->unit = hwa_basic_pitch_copy_text("Hz", 0);
        value->score = notes[index].score;
        value->provider_id = HWA_BASIC_PITCH_PROVIDER_ID;
        value->score_valid = 1;
        value->provider_id_valid = 1;
        value->selected = 1;
        if (event->kind == NULL || event->voice == NULL ||
            event->part == NULL || event->score_event_id == NULL ||
            value->name == NULL || value->unit == NULL ||
            !isfinite(value->number) || value->number <= 0.0 ||
            !isfinite(value->score) || value->score < 0.0 ||
            value->score > 1.0)
            goto allocation_failed;
    }
    task->output.bundle = &task->bundle;
    *task_result = task;
    return 0;

allocation_failed:
    hwa_basic_pitch_provider_error(
        error, error_size, "cannot allocate Basic Pitch output");
failure:
    hwa_basic_pitch_provider_task_free(NULL, task);
    return -1;
}

static int hwa_basic_pitch_provider_start(void *context_value,
                                          const HWAInferenceRequest *request,
                                          void **task_result,
                                          char *error,
                                          size_t error_size)
{
    HWABasicPitchProviderContext *context =
        (HWABasicPitchProviderContext *)context_value;
    HWABasicPitchProviderContext bounded_context;
    HWAInferenceProvider descriptor;
    const HWAInferenceInput *source;
    HWAInferenceRuntimeProvenance runtime;
    HWABasicPitchTask *task = NULL;
    HWABasicPitchNote *decoded_notes = NULL;
    float *note_activations = NULL;
    float *onset_activations = NULL;
    char *settings_json = NULL;
    size_t frame_count = 0U;
    size_t cell_count;
    size_t matrix_bytes;
    size_t max_notes;
    size_t note_count = 0U;
    size_t note_capacity;
    size_t note_storage_bytes;
    uint64_t global_bytes;
    uint64_t provenance_peak = 0U;
    uint64_t decode_work;
    uint64_t deadline_started = 0U;
    HWAInferenceRequest normalized_request;
    int status = -1;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (task_result == NULL) {
        hwa_basic_pitch_provider_error(
            error, error_size, "Basic Pitch task output is null");
        return -1;
    }
    *task_result = NULL;
    if (context == NULL) {
        hwa_basic_pitch_provider_error(
            error, error_size, "Basic Pitch provider context is null");
        return -1;
    }
    if (hwa_inference_deadline_start(
            &deadline_started, error, error_size) != 0)
        return -1;
    hwa_basic_pitch_descriptor(context, &descriptor);
    if (hwa_inference_request_validate(
            &descriptor, request, error, error_size) != 0)
        return -1;
    if (hwa_inference_deadline_check(
            deadline_started, request->timeout_milliseconds,
            error, error_size) != 0)
        return -1;
    bounded_context = *context;
    if (bounded_context.max_work_bytes >
        request->output_limits.max_work_bytes)
        bounded_context.max_work_bytes =
            request->output_limits.max_work_bytes;
    context = &bounded_context;
    if (strcmp(request->task, HWA_BASIC_PITCH_TASK_NAME) != 0 ||
        !hwa_basic_pitch_settings_match(
            request->settings_json, context->task_settings) ||
        request->input_count != 1U) {
        hwa_basic_pitch_provider_error(
            error, error_size,
            "Basic Pitch task, settings, or input count do not match");
        return -1;
    }
    source = hwa_basic_pitch_source(request);
    if (source == NULL) {
        hwa_basic_pitch_provider_error(
            error, error_size, "Basic Pitch source input is missing");
        return -1;
    }
    max_notes = request->output_limits.max_events;
    if (max_notes > request->output_limits.max_values)
        max_notes = request->output_limits.max_values;
    max_notes = hwa_basic_pitch_limit_json_integer(max_notes);
    if (hwa_basic_pitch_collect_activations(
            context, request, source, deadline_started,
            &note_activations, &onset_activations, &frame_count,
            error, error_size) != 0)
        goto cleanup;
    if (hwa_inference_deadline_check(
            deadline_started, request->timeout_milliseconds,
            error, error_size) != 0)
        goto cleanup;
    if (hwa_basic_pitch_size_bytes(
            frame_count, HWA_BASIC_PITCH_NOTE_BINS, &cell_count) != 0 ||
        hwa_basic_pitch_size_bytes(
            cell_count, sizeof(float), &matrix_bytes) != 0 ||
        hwa_basic_pitch_size_exceeds_u64_half(matrix_bytes)) {
        hwa_basic_pitch_provider_error(
            error, error_size, "Basic Pitch decode size overflows");
        goto cleanup;
    }
    global_bytes = (uint64_t)matrix_bytes * 2U;
    if (global_bytes > context->max_work_bytes) {
        hwa_basic_pitch_provider_error(
            error, error_size, "Basic Pitch decode exceeds its work limit");
        goto cleanup;
    }
    decode_work = context->max_work_bytes - global_bytes;
    if (hwa_basic_pitch_decode(
            note_activations, onset_activations, frame_count,
            &context->decoder_options, max_notes, decode_work,
            &decoded_notes, &note_count, error, error_size) != 0)
        goto cleanup;
    if (hwa_inference_deadline_check(
            deadline_started, request->timeout_milliseconds,
            error, error_size) != 0)
        goto cleanup;
    free(onset_activations);
    onset_activations = NULL;
    free(note_activations);
    note_activations = NULL;
    note_capacity = hwa_basic_pitch_note_capacity(note_count, max_notes);
    if (hwa_basic_pitch_size_bytes(
            note_capacity, sizeof(*decoded_notes),
            &note_storage_bytes) != 0 ||
        hwa_basic_pitch_u64_add(
            &provenance_peak, (uint64_t)note_storage_bytes,
            context->max_work_bytes) != 0 ||
        hwa_basic_pitch_u64_add(
            &provenance_peak,
            HWA_BASIC_PITCH_PROVENANCE_BYTES + 1U,
            context->max_work_bytes) != 0 ||
        hwa_basic_pitch_add_allocation(
            &provenance_peak, request->input_count,
            sizeof(HWAInferenceInput *),
            context->max_work_bytes) != 0) {
        hwa_basic_pitch_provider_error(
            error, error_size,
            "Basic Pitch provenance exceeds its work limit");
        goto cleanup;
    }
    memset(&runtime, 0, sizeof(runtime));
    runtime.name = context->runtime_name;
    runtime.version = context->runtime_version;
    runtime.backend = context->backend;
    runtime.fallback = context->fallback;
    runtime.adapter_sha256 = context->adapter_sha256;
    normalized_request = *request;
    normalized_request.settings_json = context->task_settings;
    if (hwa_inference_provenance_settings_build(
            &normalized_request, &runtime, HWA_BASIC_PITCH_PROVENANCE_BYTES,
            &settings_json, error, error_size) != 0)
        goto cleanup;
    if (hwa_basic_pitch_build_task(
            context, request, source, decoded_notes, note_count,
            note_capacity, &settings_json, &task,
            error, error_size) != 0)
        goto cleanup;
    free(decoded_notes);
    decoded_notes = NULL;
    if (hwa_inference_output_validate_for_request(
            &descriptor, request, &task->output,
            error, error_size) != 0)
        goto cleanup;
    if (hwa_inference_deadline_check(
            deadline_started, request->timeout_milliseconds,
            error, error_size) != 0)
        goto cleanup;
    *task_result = task;
    task = NULL;
    status = 0;
cleanup:
    hwa_basic_pitch_provider_task_free(NULL, task);
    free(settings_json);
    free(decoded_notes);
    free(onset_activations);
    free(note_activations);
    return status;
}

static int hwa_basic_pitch_provider_poll(
    void *context,
    void *task_value,
    HWAInferencePollState *state,
    const HWAInferenceOutput **output,
    char *error,
    size_t error_size)
{
    HWABasicPitchTask *task = (HWABasicPitchTask *)task_value;
    (void)context;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (state != NULL) *state = HWA_INFERENCE_PENDING;
    if (output != NULL) *output = NULL;
    if (task == NULL || state == NULL || output == NULL) {
        hwa_basic_pitch_provider_error(
            error, error_size, "invalid Basic Pitch inference poll");
        return -1;
    }
    *state = HWA_INFERENCE_READY;
    *output = &task->output;
    return 0;
}

static void hwa_basic_pitch_provider_task_free(void *context,
                                               void *task_value)
{
    HWABasicPitchTask *task = (HWABasicPitchTask *)task_value;
    (void)context;
    if (task == NULL) return;
    hwa_event_bundle_free(&task->bundle);
    free(task);
}

static void hwa_basic_pitch_provider_destroy(void *context_value)
{
    HWABasicPitchProviderContext *context =
        (HWABasicPitchProviderContext *)context_value;
    if (context == NULL) return;
    if (context->runner_destroy != NULL)
        context->runner_destroy(context->runner_context);
    free(context->runtime_name);
    free(context->runtime_version);
    free(context->backend);
    free(context->fallback);
    free(context->task_settings);
    free(context);
}

int hwa_basic_pitch_provider_init(
    HWAInferenceProvider *provider,
    const char *model_sha256,
    const char *adapter_sha256,
    const HWABasicPitchDecoderOptions *decoder_options,
    uint64_t max_work_bytes,
    const HWABasicPitchRunner *runner,
    char *error,
    size_t error_size)
{
    HWABasicPitchProviderContext *context = NULL;
    char *task_settings = NULL;
    int status = -1;
    if (error != NULL && error_size != 0U) error[0] = '\0';
    if (provider == NULL) {
        hwa_basic_pitch_provider_error(
            error, error_size, "Basic Pitch provider output is null");
        return -1;
    }
    memset(provider, 0, sizeof(*provider));
    if (!hwa_basic_pitch_lower_sha256(model_sha256) ||
        !hwa_basic_pitch_lower_sha256(adapter_sha256) ||
        max_work_bytes == 0U || runner == NULL ||
        runner->run_window == NULL ||
        (runner->context != NULL && runner->destroy == NULL) ||
        hwa_basic_pitch_text_size(runner->runtime_name, 0, NULL) != 0 ||
        hwa_basic_pitch_text_size(runner->runtime_version, 0, NULL) != 0 ||
        hwa_basic_pitch_text_size(runner->backend, 0, NULL) != 0 ||
        hwa_basic_pitch_text_size(runner->fallback, 1, NULL) != 0) {
        hwa_basic_pitch_provider_error(
            error, error_size, "invalid Basic Pitch provider arguments");
        return -1;
    }
    if (hwa_basic_pitch_task_settings_build(
            decoder_options, &task_settings, error, error_size) != 0)
        return -1;
    context = (HWABasicPitchProviderContext *)calloc(1U, sizeof(*context));
    if (context == NULL) goto allocation_failed;
    context->runtime_name =
        hwa_basic_pitch_copy_text(runner->runtime_name, 0);
    context->runtime_version =
        hwa_basic_pitch_copy_text(runner->runtime_version, 0);
    context->backend = hwa_basic_pitch_copy_text(runner->backend, 0);
    context->fallback = hwa_basic_pitch_copy_text(runner->fallback, 1);
    if (context->runtime_name == NULL || context->runtime_version == NULL ||
        context->backend == NULL || context->fallback == NULL)
        goto allocation_failed;
    memcpy(context->model_sha256, model_sha256, HWA_SHA256_HEX_SIZE);
    memcpy(context->adapter_sha256, adapter_sha256, HWA_SHA256_HEX_SIZE);
    context->task_settings = task_settings;
    task_settings = NULL;
    context->decoder_options = *decoder_options;
    context->max_work_bytes = max_work_bytes;
    context->runner_context = runner->context;
    context->run_window = runner->run_window;
    context->runner_destroy = runner->destroy;
    hwa_basic_pitch_descriptor(context, provider);
    status = 0;
    goto cleanup;

allocation_failed:
    hwa_basic_pitch_provider_error(
        error, error_size, "cannot allocate Basic Pitch provider");
cleanup:
    if (status != 0 && context != NULL) {
        free(context->runtime_name);
        free(context->runtime_version);
        free(context->backend);
        free(context->fallback);
        free(context->task_settings);
        free(context);
    }
    free(task_settings);
    return status;
}

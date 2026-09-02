#include "internal.h"
#include "dsp.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HWA_HD_PI 3.14159265358979323846264338327950288
#define HWA_HD_MAX_FFT_SIZE 65536U
#define HWA_HD_MAX_HARMONICS 24U
#define HWA_HD_MIN_VALID_HARMONICS 4U
#define HWA_HD_ANCHOR_FRAMES 5U
#define HWA_HD_SIGNAL_BINS 5U
#define HWA_HD_MIN_SIDE_BINS 4U
#define HWA_HD_MIN_FIT_POINTS 10U
#define HWA_HD_ANCHOR_SNR_DB 15.0
#define HWA_HD_TAIL_SNR_DB 10.0
#define HWA_HD_TAIL_SNR_RUN 3U
#define HWA_HD_FIT_UPPER_DB (-5.0)
#define HWA_HD_FIT_LOWER_DB (-25.0)
#define HWA_HD_FIT_ENDPOINT_TOLERANCE_DB 1.0
#define HWA_HD_LATE_BROAD_RISE_DB 6.0
#define HWA_HD_LATE_FRAME_TOLERANCE_DB 0.5
#define HWA_HD_LATE_ACTIVE_RANGE_DB 35.0
#define HWA_HD_LATE_ROUGHNESS_RETURN_DB 6.0
#define HWA_HD_POSITIVE_FLOOR 1.0e-300

typedef struct HWAHDWork {
    uint64_t live;
    uint64_t peak;
    uint64_t evaluations;
    uint64_t maximum_work;
    uint64_t maximum_evaluations;
} HWAHDWork;

typedef struct HWAHDAudio {
    HWAFormat format;
    double *samples;
    size_t frame_count;
    size_t channel_count;
    uint64_t sample_bytes;
} HWAHDAudio;

static int hwa_hd_u64_multiply(uint64_t left,
                               uint64_t right,
                               uint64_t *result)
{
    if (result == NULL || (left != 0U && right > UINT64_MAX / left)) {
        return -1;
    }
    *result = left * right;
    return 0;
}

static int hwa_hd_size_multiply(size_t left,
                                size_t right,
                                size_t *result)
{
    if (result == NULL || (left != 0U && right > SIZE_MAX / left)) {
        return -1;
    }
    *result = left * right;
    return 0;
}

static int hwa_hd_work_add(HWAHDWork *work,
                           uint64_t bytes,
                           char *error,
                           size_t error_size)
{
    if (work->live > work->maximum_work ||
        bytes > work->maximum_work - work->live) {
        hwa_set_error(error, error_size,
                      "harmonic-decay work limit exceeded");
        return -1;
    }
    work->live += bytes;
    if (work->live > work->peak) work->peak = work->live;
    return 0;
}

static void hwa_hd_work_remove(HWAHDWork *work, uint64_t bytes)
{
    if (bytes <= work->live) work->live -= bytes;
}

static int hwa_hd_evaluate(HWAHDWork *work,
                           uint64_t count,
                           char *error,
                           size_t error_size)
{
    if (work->evaluations > work->maximum_evaluations ||
        count > work->maximum_evaluations - work->evaluations) {
        hwa_set_error(error, error_size,
                      "harmonic-decay evaluation limit exceeded");
        return -1;
    }
    work->evaluations += count;
    return 0;
}

static char *hwa_hd_copy_path(const char *path,
                              HWAHDWork *work,
                              char *error,
                              size_t error_size)
{
    size_t length;
    char *copy;
    if (path == NULL) return NULL;
    length = strlen(path);
    if (length == SIZE_MAX ||
        hwa_hd_work_add(work, (uint64_t)length + 1U,
                        error, error_size) != 0) {
        return NULL;
    }
    copy = (char *)malloc(length + 1U);
    if (copy == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for harmonic-decay path");
        return NULL;
    }
    memcpy(copy, path, length + 1U);
    return copy;
}

void hwa_harmonic_decay_options_default(HWAHarmonicDecayOptions *options)
{
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    options->decode_block_frames = 4096U;
    options->max_input_bytes = UINT64_C(17179869184);
    options->max_input_frames = UINT64_C(2000000000);
    options->max_work_bytes = UINT64_C(268435456);
    options->max_evaluations = UINT64_C(500000000);
}

static void hwa_hd_profile_free(HWAHarmonicDecayProfile *profile)
{
    if (profile == NULL) return;
    free(profile->path);
    free(profile->bands);
    memset(profile, 0, sizeof(*profile));
}

void hwa_harmonic_decay_result_free(HWAHarmonicDecayResult *result)
{
    if (result == NULL) return;
    hwa_hd_profile_free(&result->reference);
    hwa_hd_profile_free(&result->model);
    free(result->comparisons);
    memset(result, 0, sizeof(*result));
}

static int hwa_hd_options_validate(const HWAHarmonicDecayOptions *options,
                                   char *error,
                                   size_t error_size)
{
    if (options == NULL || !isfinite(options->expected_hz) ||
        options->expected_hz < 20.0 || options->expected_hz > 5000.0) {
        hwa_set_error(error, error_size,
                      "harmonic-decay expected frequency is invalid");
        return -1;
    }
    if (options->decode_block_frames == 0U ||
        options->decode_block_frames > 1048576U ||
        options->max_input_bytes == 0U ||
        options->max_input_frames == 0U ||
        options->max_work_bytes == 0U ||
        options->max_evaluations == 0U) {
        hwa_set_error(error, error_size,
                      "harmonic-decay resource limit is invalid");
        return -1;
    }
    return 0;
}

static void hwa_hd_audio_free(HWAHDAudio *audio, HWAHDWork *work)
{
    if (audio == NULL) return;
    free(audio->samples);
    hwa_hd_work_remove(work, audio->sample_bytes);
    memset(audio, 0, sizeof(*audio));
}

static int hwa_hd_decode(const char *path,
                         const HWAHarmonicDecayOptions *options,
                         HWAHDAudio *audio,
                         HWAHDWork *work,
                         char *error,
                         size_t error_size)
{
    HWAWavReader reader;
    unsigned char *buffer = NULL;
    uint64_t buffer_bytes = 0U;
    uint64_t sample_count = 0U;
    size_t frame_offset = 0U;
    int status = -1;

    memset(&reader, 0, sizeof(reader));
    memset(audio, 0, sizeof(*audio));
    if (hwa_wav_reader_open(&reader, path, options->max_input_bytes,
                            error, error_size) != 0) {
        return -1;
    }
    if (reader.format.encoding != HWA_ENCODING_PCM ||
        (reader.format.bits_per_sample != 16U &&
         reader.format.bits_per_sample != 24U) ||
        (reader.format.channels != 1U && reader.format.channels != 2U)) {
        hwa_set_error(error, error_size,
                      "harmonic-decay needs mono or stereo 16-bit or 24-bit PCM WAVE audio");
        goto cleanup;
    }
    if (reader.format.frames == 0U ||
        reader.format.frames > options->max_input_frames ||
        reader.format.frames > (uint64_t)SIZE_MAX) {
        hwa_set_error(error, error_size,
                      "harmonic-decay input frame limit exceeded");
        goto cleanup;
    }
    if (hwa_hd_u64_multiply(reader.format.frames,
                            (uint64_t)reader.format.channels,
                            &sample_count) != 0 ||
        hwa_hd_u64_multiply(sample_count, (uint64_t)sizeof(double),
                            &audio->sample_bytes) != 0 ||
        hwa_hd_u64_multiply((uint64_t)options->decode_block_frames,
                            (uint64_t)reader.format.block_align,
                            &buffer_bytes) != 0 ||
        sample_count > (uint64_t)SIZE_MAX / sizeof(double) ||
        buffer_bytes > (uint64_t)SIZE_MAX) {
        hwa_set_error(error, error_size,
                      "harmonic-decay input size overflow");
        goto cleanup;
    }
    if (hwa_hd_work_add(work, audio->sample_bytes,
                        error, error_size) != 0 ||
        hwa_hd_work_add(work, buffer_bytes, error, error_size) != 0 ||
        hwa_hd_evaluate(work, sample_count, error, error_size) != 0) {
        goto cleanup;
    }
    audio->samples = (double *)malloc((size_t)audio->sample_bytes);
    buffer = (unsigned char *)malloc((size_t)buffer_bytes);
    if (audio->samples == NULL || buffer == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for harmonic-decay audio");
        goto cleanup;
    }
    for (;;) {
        size_t frames_read = 0U;
        size_t frame;
        if (hwa_wav_reader_read_frames(&reader, buffer,
                                       options->decode_block_frames,
                                       &frames_read, error, error_size) != 0) {
            goto cleanup;
        }
        if (frames_read == 0U) break;
        for (frame = 0U; frame < frames_read; ++frame) {
            size_t channel;
            const unsigned char *frame_bytes =
                buffer + frame * reader.format.block_align;
            for (channel = 0U;
                 channel < (size_t)reader.format.channels; ++channel) {
                int clipped = 0;
                audio->samples[
                    (frame_offset + frame) *
                    (size_t)reader.format.channels + channel
                ] = hwa_wav_decode_sample(
                    &reader,
                    frame_bytes + channel * reader.bytes_per_sample,
                    &clipped);
            }
        }
        frame_offset += frames_read;
    }
    if ((uint64_t)frame_offset != reader.format.frames) {
        hwa_set_error(error, error_size,
                      "harmonic-decay decoded frame count changed");
        goto cleanup;
    }
    audio->format = reader.format;
    audio->frame_count = frame_offset;
    audio->channel_count = (size_t)reader.format.channels;
    status = 0;

cleanup:
    free(buffer);
    hwa_hd_work_remove(work, buffer_bytes);
    hwa_wav_reader_close(&reader);
    if (status != 0) hwa_hd_audio_free(audio, work);
    return status;
}

static size_t hwa_hd_round_samples(uint32_t rate, double seconds)
{
    return (size_t)floor((double)rate * seconds + 0.5);
}

static int hwa_hd_fft_size(uint32_t rate,
                           double expected_hz,
                           size_t *fft_size,
                           char *error,
                           size_t error_size)
{
    double seconds = fmax(0.080, 12.0 / expected_hz);
    double wanted = ceil((double)rate * seconds);
    size_t size = 1024U;
    if (!isfinite(wanted) || wanted > (double)HWA_HD_MAX_FFT_SIZE) {
        hwa_set_error(error, error_size,
                      "harmonic-decay derived FFT size is too large");
        return -1;
    }
    while ((double)size < wanted && size < HWA_HD_MAX_FFT_SIZE) size *= 2U;
    if ((double)size < wanted || size > HWA_HD_MAX_FFT_SIZE) {
        hwa_set_error(error, error_size,
                      "harmonic-decay derived FFT size is too large");
        return -1;
    }
    *fft_size = size;
    return 0;
}

static void hwa_hd_frame_features(const HWAHDAudio *audio,
                                  size_t start,
                                  size_t count,
                                  double *level,
                                  double *roughness)
{
    size_t frame;
    long double power = 0.0L;
    long double difference_power = 0.0L;
    for (frame = 0U; frame < count; ++frame) {
        size_t channel;
        for (channel = 0U; channel < audio->channel_count; ++channel) {
            size_t absolute = start + frame;
            double value = audio->samples[
                absolute * audio->channel_count + channel];
            double previous = absolute == 0U ? 0.0 : audio->samples[
                (absolute - 1U) * audio->channel_count + channel];
            double difference = value - previous;
            power += (long double)value * (long double)value;
            difference_power +=
                (long double)difference * (long double)difference;
        }
    }
    power /= (long double)(count * audio->channel_count);
    difference_power /= (long double)(count * audio->channel_count);
    *level = power > 1.0e-30L ? 10.0 * log10((double)power) : -300.0;
    *roughness = 10.0 * log10(fmax(
        (double)difference_power / fmax((double)power, 1.0e-24),
        1.0e-12));
}

static int hwa_hd_find_event(const HWAHDAudio *audio,
                             HWAHarmonicDecayProfile *profile,
                             HWAHDWork *work,
                             char *error,
                             size_t error_size)
{
    const uint32_t rate = audio->format.sample_rate_hz;
    size_t window = hwa_hd_round_samples(rate, 0.020);
    size_t hop = hwa_hd_round_samples(rate, 0.005);
    size_t point_count;
    double *levels = NULL;
    double *roughness = NULL;
    uint64_t level_bytes;
    uint64_t feature_bytes;
    uint64_t evaluations;
    double peak = -300.0;
    size_t index;
    size_t group_count = 0U;
    size_t onset = 0U;
    size_t last_candidate = 0U;
    int have_candidate = 0;
    int status = -1;

    if (window == 0U || hop == 0U || window > audio->frame_count) {
        profile->rejection_mask |= HWA_HARMONIC_DECAY_REJECT_LOW_SUPPORT;
        return 0;
    }
    point_count = 1U + (audio->frame_count - window) / hop;
    if (hwa_hd_u64_multiply((uint64_t)point_count,
                            (uint64_t)sizeof(double), &level_bytes) != 0 ||
        hwa_hd_u64_multiply(level_bytes, 2U, &feature_bytes) != 0 ||
        hwa_hd_u64_multiply((uint64_t)point_count,
                            (uint64_t)window, &evaluations) != 0 ||
        hwa_hd_u64_multiply(evaluations,
                            (uint64_t)audio->channel_count,
                            &evaluations) != 0 ||
        hwa_hd_u64_multiply(evaluations, 2U, &evaluations) != 0 ||
        hwa_hd_work_add(work, feature_bytes, error, error_size) != 0) {
        return -1;
    }
    if (hwa_hd_evaluate(work, evaluations, error, error_size) != 0) {
        hwa_hd_work_remove(work, feature_bytes);
        return -1;
    }
    levels = (double *)malloc((size_t)level_bytes);
    roughness = (double *)malloc((size_t)level_bytes);
    if (levels == NULL || roughness == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for harmonic-decay onset");
        goto cleanup;
    }
    for (index = 0U; index < point_count; ++index) {
        hwa_hd_frame_features(audio, index * hop, window,
                              &levels[index], &roughness[index]);
        if (levels[index] > peak) peak = levels[index];
    }
    for (index = 2U; index < point_count; ++index) {
        double recent = fmin(levels[index - 2U], levels[index - 1U]);
        if (levels[index] >= peak - 30.0 &&
            levels[index] - recent >= 8.0) {
            if (!have_candidate ||
                (index - last_candidate) * hop >=
                    hwa_hd_round_samples(rate, 0.100)) {
                group_count++;
                if (group_count == 1U) onset = index;
            }
            last_candidate = index;
            have_candidate = 1;
        }
    }
    if (group_count == 0U) {
        profile->rejection_mask |= HWA_HARMONIC_DECAY_REJECT_NO_ONSET;
        status = 0;
        goto cleanup;
    }
    if (group_count != 1U) {
        profile->rejection_mask |= HWA_HARMONIC_DECAY_REJECT_LATE_PULSE;
        status = 0;
        goto cleanup;
    }
    {
        size_t broad_end = onset + hwa_hd_round_samples(rate, 0.040) / hop;
        size_t broad = onset;
        size_t long_rise_span = hwa_hd_round_samples(rate, 0.080) / hop;
        size_t event_span = hwa_hd_round_samples(rate, 0.100) / hop;
        size_t first_late;
        uint64_t late_evaluations;
        if (broad_end >= point_count) broad_end = point_count - 1U;
        for (index = onset + 1U; index <= broad_end; ++index) {
            if (levels[index] > levels[broad]) broad = index;
        }
        profile->onset_sample = (uint64_t)(onset * hop + window / 2U);
        profile->broad_peak_sample =
            (uint64_t)(broad * hop + window / 2U);
        if (long_rise_span < 2U) long_rise_span = 2U;
        if (event_span < 2U) event_span = 2U;
        first_late = broad <= SIZE_MAX - event_span
                         ? broad + event_span : SIZE_MAX;
        if (first_late < long_rise_span) first_late = long_rise_span;
        if (first_late < point_count &&
            (hwa_hd_u64_multiply(
                 (uint64_t)(point_count - first_late),
                 (uint64_t)long_rise_span,
                 &late_evaluations) != 0 ||
             hwa_hd_evaluate(work, late_evaluations,
                             error, error_size) != 0)) {
            goto cleanup;
        }
        for (index = first_late; index < point_count; ++index) {
            size_t lookback;
            double recent_low = levels[index - long_rise_span];
            for (lookback = index - long_rise_span;
                 lookback < index; ++lookback) {
                if (levels[lookback] < recent_low) {
                    recent_low = levels[lookback];
                }
            }
            if (levels[index] > peak - HWA_HD_LATE_ACTIVE_RANGE_DB &&
                levels[index] - recent_low >
                    HWA_HD_LATE_BROAD_RISE_DB -
                    HWA_HD_LATE_FRAME_TOLERANCE_DB &&
                roughness[index] >=
                    roughness[broad] -
                    HWA_HD_LATE_ROUGHNESS_RETURN_DB) {
                profile->rejection_mask |=
                    HWA_HARMONIC_DECAY_REJECT_LATE_PULSE;
                status = 0;
                goto cleanup;
            }
        }
    }
    status = 0;

cleanup:
    free(roughness);
    free(levels);
    hwa_hd_work_remove(work, feature_bytes);
    return status;
}

static size_t hwa_hd_log2_size(size_t value)
{
    size_t count = 0U;
    while (value > 1U) {
        value >>= 1U;
        count++;
    }
    return count;
}

static int hwa_hd_build_spectra(const HWAHDAudio *audio,
                                HWAHarmonicDecayProfile *profile,
                                double expected_hz,
                                double **spectra,
                                size_t *spectrum_bins,
                                size_t *frame_count,
                                HWAHDWork *work,
                                uint64_t *spectra_bytes,
                                char *error,
                                size_t error_size)
{
    const uint32_t rate = audio->format.sample_rate_hz;
    size_t fft_size;
    size_t hop = hwa_hd_round_samples(rate, 0.010);
    size_t first;
    size_t last_boundary;
    size_t frames;
    size_t bins;
    size_t power_count;
    double *window = NULL;
    HwaDspComplex *fft = NULL;
    uint64_t window_bytes = 0U;
    uint64_t fft_bytes = 0U;
    uint64_t butterflies;
    uint64_t total_butterflies;
    double window_energy = 0.0;
    size_t frame;
    int status = -1;

    *spectra = NULL;
    *spectra_bytes = 0U;
    if (hwa_hd_fft_size(rate, expected_hz, &fft_size,
                        error, error_size) != 0) {
        return -1;
    }
    if (hop == 0U) hop = 1U;
    first = (size_t)profile->broad_peak_sample +
            hwa_hd_round_samples(rate, 0.080);
    if (first % hop != 0U) first += hop - first % hop;
    {
        size_t end_guard = hwa_hd_round_samples(rate, 0.025);
        last_boundary = end_guard < audio->frame_count
                            ? audio->frame_count - end_guard : 0U;
    }
    if (first > last_boundary || fft_size > last_boundary - first) {
        profile->rejection_mask |= HWA_HARMONIC_DECAY_REJECT_LOW_SUPPORT;
        return 0;
    }
    frames = 1U + (last_boundary - first - fft_size) / hop;
    if (frames < HWA_HD_ANCHOR_FRAMES) {
        profile->rejection_mask |= HWA_HARMONIC_DECAY_REJECT_LOW_SUPPORT;
        return 0;
    }
    bins = fft_size / 2U + 1U;
    if (hwa_hd_size_multiply(frames, bins, &power_count) != 0 ||
        hwa_hd_u64_multiply((uint64_t)power_count,
                            (uint64_t)sizeof(double), spectra_bytes) != 0 ||
        hwa_hd_u64_multiply((uint64_t)fft_size,
                            (uint64_t)sizeof(double), &window_bytes) != 0 ||
        hwa_hd_u64_multiply((uint64_t)fft_size,
                            (uint64_t)sizeof(HwaDspComplex), &fft_bytes) != 0 ||
        hwa_hd_work_add(work, *spectra_bytes,
                        error, error_size) != 0 ||
        hwa_hd_work_add(work, window_bytes, error, error_size) != 0 ||
        hwa_hd_work_add(work, fft_bytes, error, error_size) != 0) {
        return -1;
    }
    butterflies = (uint64_t)(fft_size / 2U) *
                  (uint64_t)hwa_hd_log2_size(fft_size);
    if (hwa_hd_u64_multiply(butterflies,
                            (uint64_t)audio->channel_count,
                            &total_butterflies) != 0 ||
        hwa_hd_u64_multiply(total_butterflies,
                            (uint64_t)frames,
                            &total_butterflies) != 0 ||
        hwa_hd_evaluate(work, total_butterflies,
                        error, error_size) != 0) {
        return -1;
    }
    *spectra = (double *)calloc(power_count, sizeof(double));
    window = (double *)malloc((size_t)window_bytes);
    fft = (HwaDspComplex *)malloc((size_t)fft_bytes);
    if (*spectra == NULL || window == NULL || fft == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for harmonic-decay spectra");
        goto cleanup;
    }
    if (hwa_dsp_hann(window, fft_size) != HWA_DSP_OK) {
        hwa_set_error(error, error_size,
                      "could not create harmonic-decay Hann window");
        goto cleanup;
    }
    for (frame = 0U; frame < fft_size; ++frame) {
        window_energy += window[frame] * window[frame];
    }
    if (!(window_energy > 0.0)) {
        hwa_set_error(error, error_size,
                      "harmonic-decay Hann window has no energy");
        goto cleanup;
    }
    for (frame = 0U; frame < frames; ++frame) {
        size_t channel;
        size_t start = first + frame * hop;
        for (channel = 0U; channel < audio->channel_count; ++channel) {
            size_t index;
            long double sum = 0.0L;
            double mean;
            for (index = 0U; index < fft_size; ++index) {
                sum += audio->samples[
                    (start + index) * audio->channel_count + channel];
            }
            mean = (double)(sum / (long double)fft_size);
            for (index = 0U; index < fft_size; ++index) {
                double sample = audio->samples[
                    (start + index) * audio->channel_count + channel];
                fft[index].real = (sample - mean) * window[index];
                fft[index].imag = 0.0;
            }
            if (hwa_dsp_fft(fft, fft_size, 0) != HWA_DSP_OK) {
                hwa_set_error(error, error_size,
                              "harmonic-decay FFT failed");
                goto cleanup;
            }
            for (index = 0U; index < bins; ++index) {
                double real = fft[index].real;
                double imaginary = fft[index].imag;
                double one_sided =
                    (index == 0U || index + 1U == bins) ? 1.0 : 2.0;
                double power = one_sided *
                    (real * real + imaginary * imaginary) /
                    ((double)fft_size * window_energy);
                (*spectra)[frame * bins + index] +=
                    power / (double)audio->channel_count;
            }
        }
    }
    profile->fft_size = fft_size;
    profile->hop_samples = hop;
    profile->analysis_start_sample = (uint64_t)first;
    profile->analysis_end_sample = (uint64_t)last_boundary;
    *spectrum_bins = bins;
    *frame_count = frames;
    status = 0;

cleanup:
    free(fft);
    hwa_hd_work_remove(work, fft_bytes);
    free(window);
    hwa_hd_work_remove(work, window_bytes);
    if (status != 0) {
        free(*spectra);
        *spectra = NULL;
        hwa_hd_work_remove(work, *spectra_bytes);
        *spectra_bytes = 0U;
    }
    return status;
}

static int hwa_hd_compare_double(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static double hwa_hd_median(double *values, size_t count)
{
    qsort(values, count, sizeof(*values), hwa_hd_compare_double);
    if ((count & 1U) != 0U) return values[count / 2U];
    return 0.5 * (values[count / 2U - 1U] + values[count / 2U]);
}

static double hwa_hd_band_power(const double *row,
                                size_t first,
                                size_t last)
{
    double sum = 0.0;
    size_t bin;
    for (bin = first; bin <= last; ++bin) sum += row[bin];
    return sum;
}

static int hwa_hd_analyze_band(const double *spectra,
                               size_t spectrum_bins,
                               size_t frame_count,
                               const HWAHarmonicDecayProfile *profile,
                               double expected_hz,
                               HWAHarmonicDecayBand *band,
                               double *signal,
                               double *noise,
                               double *edc,
                               HWAHDWork *work,
                               char *error,
                               size_t error_size)
{
    const double rate = (double)profile->format.sample_rate_hz;
    const double bin_hz = rate / (double)profile->fft_size;
    double target_bin = band->target_hz / bin_hz;
    double search_hz = fmin(0.20 * expected_hz, 6.0 * bin_hz);
    ptrdiff_t first_center = (ptrdiff_t)ceil(
        target_bin - search_hz / bin_hz);
    ptrdiff_t last_center = (ptrdiff_t)floor(
        target_bin + search_hz / bin_hz);
    ptrdiff_t side_offset =
        (ptrdiff_t)floor((expected_hz / bin_hz) / 2.0) - 1;
    size_t selected = 0U;
    double selected_power = -1.0;
    ptrdiff_t center;
    size_t side_count;
    uint64_t evaluations;
    size_t frame;
    double anchor_signal = 0.0;
    double anchor_noise = 0.0;
    size_t low_run = 0U;
    size_t accepted = frame_count;
    size_t tail_boundary;
    long double cumulative = 0.0L;
    size_t fit_first = SIZE_MAX;
    size_t fit_last = 0U;
    size_t fit_count = 0U;
    double total_edc;
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;
    double divisor;
    double slope;
    double intercept;
    double residual_sum = 0.0;

    if (side_offset > 8) side_offset = 8;
    if (side_offset < 5) {
        band->rejection_mask |=
            HWA_HARMONIC_DECAY_REJECT_BAND_OUT_OF_RANGE;
        return 0;
    }
    if (first_center < side_offset) first_center = side_offset;
    if (last_center >= (ptrdiff_t)spectrum_bins - side_offset) {
        last_center = (ptrdiff_t)spectrum_bins - side_offset - 1;
    }
    if (first_center > last_center) {
        band->rejection_mask |=
            HWA_HARMONIC_DECAY_REJECT_BAND_OUT_OF_RANGE;
        return 0;
    }
    if (hwa_hd_u64_multiply(
            (uint64_t)(last_center - first_center + 1),
            (uint64_t)(HWA_HD_ANCHOR_FRAMES * HWA_HD_SIGNAL_BINS),
            &evaluations) != 0 ||
        hwa_hd_evaluate(work, evaluations, error, error_size) != 0) {
        return -1;
    }
    for (center = first_center; center <= last_center; ++center) {
        double power = 0.0;
        for (frame = 0U; frame < HWA_HD_ANCHOR_FRAMES; ++frame) {
            const double *row = spectra + frame * spectrum_bins;
            power += hwa_hd_band_power(
                row, (size_t)center - 2U, (size_t)center + 2U);
        }
        power /= (double)HWA_HD_ANCHOR_FRAMES;
        if (power > selected_power) {
            selected_power = power;
            selected = (size_t)center;
        }
    }
    band->selected_bin = selected;
    band->selected_hz = (double)selected * bin_hz;
    band->signal_first_bin = selected - 2U;
    band->signal_last_bin = selected + 2U;
    band->lower_noise_first_bin = selected - (size_t)side_offset;
    band->lower_noise_last_bin = selected - 4U;
    band->upper_noise_first_bin = selected + 4U;
    band->upper_noise_last_bin = selected + (size_t)side_offset;
    side_count = 2U * ((size_t)side_offset - 3U);
    if (side_count < HWA_HD_MIN_SIDE_BINS) {
        band->rejection_mask |=
            HWA_HARMONIC_DECAY_REJECT_BAND_OUT_OF_RANGE;
        return 0;
    }
    if (hwa_hd_u64_multiply(
            (uint64_t)frame_count,
            (uint64_t)(HWA_HD_SIGNAL_BINS + side_count),
            &evaluations) != 0 ||
        hwa_hd_evaluate(work, evaluations, error, error_size) != 0) {
        return -1;
    }
    for (frame = 0U; frame < frame_count; ++frame) {
        const double *row = spectra + frame * spectrum_bins;
        double sides[10];
        size_t count = 0U;
        size_t bin;
        signal[frame] = hwa_hd_band_power(
            row, band->signal_first_bin, band->signal_last_bin);
        for (bin = band->lower_noise_first_bin;
             bin <= band->lower_noise_last_bin; ++bin) {
            sides[count++] = row[bin];
        }
        for (bin = band->upper_noise_first_bin;
             bin <= band->upper_noise_last_bin; ++bin) {
            sides[count++] = row[bin];
        }
        noise[frame] = hwa_hd_median(sides, count) *
                       (double)HWA_HD_SIGNAL_BINS;
        if (frame < HWA_HD_ANCHOR_FRAMES) {
            anchor_signal += signal[frame];
            anchor_noise += noise[frame];
        }
    }
    band->anchor_snr_db = 10.0 * log10(
        fmax(anchor_signal, HWA_HD_POSITIVE_FLOOR) /
        fmax(anchor_noise, HWA_HD_POSITIVE_FLOOR));
    if (band->anchor_snr_db < HWA_HD_ANCHOR_SNR_DB) {
        band->rejection_mask |=
            HWA_HARMONIC_DECAY_REJECT_LOW_ANCHOR_SNR;
        return 0;
    }
    for (frame = 0U; frame < frame_count; ++frame) {
        double snr = 10.0 * log10(
            fmax(signal[frame], HWA_HD_POSITIVE_FLOOR) /
            fmax(noise[frame], HWA_HD_POSITIVE_FLOOR));
        if (snr < HWA_HD_TAIL_SNR_DB) {
            low_run++;
            if (low_run == HWA_HD_TAIL_SNR_RUN) {
                accepted = frame - (HWA_HD_TAIL_SNR_RUN - 1U);
                break;
            }
        } else {
            low_run = 0U;
        }
    }
    tail_boundary = accepted < frame_count
                        ? (size_t)profile->analysis_start_sample +
                          accepted * profile->hop_samples
                        : (size_t)profile->analysis_end_sample;
    band->tail_boundary_sample = (uint64_t)tail_boundary;
    if (accepted < HWA_HD_MIN_FIT_POINTS) {
        band->rejection_mask |= HWA_HARMONIC_DECAY_REJECT_LOW_SUPPORT;
        return 0;
    }
    if (hwa_hd_evaluate(work, (uint64_t)accepted * 2U,
                        error, error_size) != 0) {
        return -1;
    }
    for (frame = accepted; frame-- > 0U;) {
        double net = fmax(signal[frame] - noise[frame], 0.0);
        cumulative += (long double)net;
        edc[frame] = (double)cumulative;
    }
    if (!(edc[0] > 0.0)) {
        band->rejection_mask |=
            HWA_HARMONIC_DECAY_REJECT_LOW_DYNAMIC_RANGE;
        return 0;
    }
    total_edc = edc[0];
    for (frame = 0U; frame < accepted; ++frame) {
        edc[frame] = 10.0 * log10(
            fmax(edc[frame] / total_edc, HWA_HD_POSITIVE_FLOOR));
        if (edc[frame] >= HWA_HD_FIT_LOWER_DB &&
            edc[frame] <= HWA_HD_FIT_UPPER_DB) {
            double x;
            if (fit_first == SIZE_MAX) fit_first = frame;
            fit_last = frame;
            fit_count++;
            x = (double)(frame - fit_first) *
                (double)profile->hop_samples / rate;
            sum_x += x;
            sum_y += edc[frame];
            sum_xx += x * x;
            sum_xy += x * edc[frame];
        }
    }
    if (fit_count < HWA_HD_MIN_FIT_POINTS || fit_first == SIZE_MAX) {
        band->rejection_mask |=
            HWA_HARMONIC_DECAY_REJECT_LOW_DYNAMIC_RANGE;
        return 0;
    }
    band->support_seconds =
        (double)(fit_last - fit_first) *
        (double)profile->hop_samples / rate;
    band->fit_point_count = (uint64_t)fit_count;
    band->fit_start_sample = profile->analysis_start_sample +
        (uint64_t)(fit_first * profile->hop_samples + profile->fft_size / 2U);
    band->fit_end_sample = profile->analysis_start_sample +
        (uint64_t)(fit_last * profile->hop_samples + profile->fft_size / 2U);
    band->fit_dynamic_range_db = edc[fit_first] - edc[fit_last];
    if (edc[fit_first] <
            HWA_HD_FIT_UPPER_DB - HWA_HD_FIT_ENDPOINT_TOLERANCE_DB ||
        edc[fit_last] >
            HWA_HD_FIT_LOWER_DB + HWA_HD_FIT_ENDPOINT_TOLERANCE_DB) {
        band->rejection_mask |=
            HWA_HARMONIC_DECAY_REJECT_LOW_DYNAMIC_RANGE;
        return 0;
    }
    if (band->support_seconds < 0.100) {
        band->rejection_mask |= HWA_HARMONIC_DECAY_REJECT_LOW_SUPPORT;
        return 0;
    }
    divisor = (double)fit_count * sum_xx - sum_x * sum_x;
    if (fabs(divisor) < 1.0e-20) {
        band->rejection_mask |= HWA_HARMONIC_DECAY_REJECT_LOW_SUPPORT;
        return 0;
    }
    slope = ((double)fit_count * sum_xy - sum_x * sum_y) / divisor;
    intercept = (sum_y - slope * sum_x) / (double)fit_count;
    for (frame = fit_first; frame <= fit_last; ++frame) {
        if (edc[frame] >= HWA_HD_FIT_LOWER_DB &&
            edc[frame] <= HWA_HD_FIT_UPPER_DB) {
            double x = (double)(frame - fit_first) *
                       (double)profile->hop_samples / rate;
            double residual = edc[frame] - (intercept + slope * x);
            residual_sum += residual * residual;
        }
    }
    band->slope_db_per_second = slope;
    band->t60_seconds = slope < 0.0 ? -60.0 / slope : 0.0;
    band->residual_db = sqrt(residual_sum / (double)fit_count);
    if (slope >= -3.0) {
        band->rejection_mask |= HWA_HARMONIC_DECAY_REJECT_NON_DECAY;
        return 0;
    }
    if (band->residual_db > 1.5) {
        band->rejection_mask |= HWA_HARMONIC_DECAY_REJECT_HIGH_RESIDUAL;
        return 0;
    }
    if (band->fit_end_sample + (uint64_t)profile->fft_size >
        band->tail_boundary_sample) {
        band->rejection_mask |= HWA_HARMONIC_DECAY_REJECT_TRUNCATED_FIT;
        return 0;
    }
    band->valid = 1;
    return 0;
}

static int hwa_hd_analyze_bands(const HWAHDAudio *audio,
                                HWAHarmonicDecayProfile *profile,
                                double expected_hz,
                                const double *spectra,
                                size_t spectrum_bins,
                                size_t frame_count,
                                HWAHDWork *work,
                                char *error,
                                size_t error_size)
{
    double frequency_limit = fmin(
        8000.0, 0.45 * (double)audio->format.sample_rate_hz);
    size_t band_count = (size_t)floor(frequency_limit / expected_hz);
    uint64_t band_bytes;
    size_t scratch_count;
    uint64_t scratch_bytes;
    double *scratch = NULL;
    double *signal;
    double *noise;
    double *edc;
    size_t index;
    int status = -1;

    if (band_count > HWA_HD_MAX_HARMONICS) {
        band_count = HWA_HD_MAX_HARMONICS;
    }
    if (hwa_hd_u64_multiply((uint64_t)band_count,
                            (uint64_t)sizeof(*profile->bands),
                            &band_bytes) != 0 ||
        hwa_hd_work_add(work, band_bytes, error, error_size) != 0) {
        return -1;
    }
    profile->bands = (HWAHarmonicDecayBand *)calloc(
        band_count, sizeof(*profile->bands));
    if (band_count != 0U && profile->bands == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for harmonic-decay bands");
        return -1;
    }
    profile->band_count = band_count;
    if (hwa_hd_size_multiply(frame_count, 3U, &scratch_count) != 0 ||
        hwa_hd_u64_multiply((uint64_t)scratch_count,
                            (uint64_t)sizeof(double), &scratch_bytes) != 0 ||
        hwa_hd_work_add(work, scratch_bytes, error, error_size) != 0) {
        return -1;
    }
    scratch = (double *)malloc((size_t)scratch_bytes);
    if (scratch == NULL) {
        hwa_set_error(error, error_size,
                      "out of memory for harmonic-decay band work");
        goto cleanup;
    }
    signal = scratch;
    noise = scratch + frame_count;
    edc = scratch + frame_count * 2U;
    for (index = 0U; index < band_count; ++index) {
        HWAHarmonicDecayBand *band = &profile->bands[index];
        band->harmonic_number = (uint32_t)(index + 1U);
        band->target_hz = expected_hz * (double)(index + 1U);
        if (hwa_hd_analyze_band(
                spectra, spectrum_bins, frame_count, profile, expected_hz,
                band, signal, noise, edc, work, error, error_size) != 0) {
            goto cleanup;
        }
        if (band->valid != 0) profile->valid_band_count++;
    }
    if (profile->valid_band_count >= HWA_HD_MIN_VALID_HARMONICS) {
        profile->valid = 1;
    } else {
        profile->rejection_mask |=
            HWA_HARMONIC_DECAY_REJECT_LOW_HARMONIC_COVERAGE;
    }
    status = 0;

cleanup:
    free(scratch);
    hwa_hd_work_remove(work, scratch_bytes);
    return status;
}

static int hwa_hd_profile(const char *path,
                          const HWAHarmonicDecayOptions *options,
                          HWAHarmonicDecayProfile *profile,
                          HWAHDWork *work,
                          char *error,
                          size_t error_size)
{
    HWAHDAudio audio;
    double *spectra = NULL;
    size_t spectrum_bins = 0U;
    size_t spectrum_frames = 0U;
    uint64_t spectra_bytes = 0U;
    int status = -1;

    memset(&audio, 0, sizeof(audio));
    profile->path = hwa_hd_copy_path(path, work, error, error_size);
    if (profile->path == NULL) return -1;
    if (hwa_hd_decode(path, options, &audio, work,
                      error, error_size) != 0) {
        goto cleanup;
    }
    profile->format = audio.format;
    if (hwa_hd_find_event(&audio, profile, work,
                          error, error_size) != 0) {
        goto cleanup;
    }
    if ((profile->rejection_mask &
         (HWA_HARMONIC_DECAY_REJECT_NO_ONSET |
          HWA_HARMONIC_DECAY_REJECT_LATE_PULSE |
          HWA_HARMONIC_DECAY_REJECT_LOW_SUPPORT)) != 0U) {
        status = 0;
        goto cleanup;
    }
    if (hwa_hd_build_spectra(
            &audio, profile, options->expected_hz, &spectra,
            &spectrum_bins, &spectrum_frames, work, &spectra_bytes,
            error, error_size) != 0) {
        goto cleanup;
    }
    if (spectra == NULL) {
        status = 0;
        goto cleanup;
    }
    if (hwa_hd_analyze_bands(
            &audio, profile, options->expected_hz, spectra,
            spectrum_bins, spectrum_frames, work,
            error, error_size) != 0) {
        goto cleanup;
    }
    status = 0;

cleanup:
    free(spectra);
    hwa_hd_work_remove(work, spectra_bytes);
    hwa_hd_audio_free(&audio, work);
    profile->peak_work_bytes = work->peak;
    profile->evaluation_count = work->evaluations;
    return status;
}

static int hwa_hd_compare_profiles(HWAHarmonicDecayResult *result,
                                   HWAHDWork *work,
                                   char *error,
                                   size_t error_size)
{
    size_t count = result->reference.band_count > result->model.band_count
                       ? result->reference.band_count
                       : result->model.band_count;
    uint64_t comparison_bytes;
    uint64_t errors_bytes;
    double *errors = NULL;
    size_t index;
    double squared = 0.0;

    if (hwa_hd_u64_multiply((uint64_t)count,
                            (uint64_t)sizeof(*result->comparisons),
                            &comparison_bytes) != 0 ||
        hwa_hd_u64_multiply((uint64_t)count, (uint64_t)sizeof(double),
                            &errors_bytes) != 0 ||
        hwa_hd_work_add(work, comparison_bytes,
                        error, error_size) != 0 ||
        hwa_hd_work_add(work, errors_bytes, error, error_size) != 0) {
        return -1;
    }
    result->comparisons = (HWAHarmonicDecayComparison *)calloc(
        count, sizeof(*result->comparisons));
    errors = (double *)malloc((size_t)errors_bytes);
    if ((count != 0U && result->comparisons == NULL) ||
        (count != 0U && errors == NULL)) {
        hwa_set_error(error, error_size,
                      "out of memory for harmonic-decay comparison");
        free(errors);
        hwa_hd_work_remove(work, errors_bytes);
        return -1;
    }
    result->comparison_count = count;
    for (index = 0U; index < count; ++index) {
        HWAHarmonicDecayComparison *comparison =
            &result->comparisons[index];
        const HWAHarmonicDecayBand *reference =
            index < result->reference.band_count
                ? &result->reference.bands[index] : NULL;
        const HWAHarmonicDecayBand *model =
            index < result->model.band_count
                ? &result->model.bands[index] : NULL;
        comparison->harmonic_number = (uint32_t)(index + 1U);
        comparison->reference_valid = reference != NULL && reference->valid;
        comparison->model_valid = model != NULL && model->valid;
        if (comparison->reference_valid && comparison->model_valid) {
            double value = 20.0 * log10(
                model->t60_seconds / reference->t60_seconds);
            comparison->t60_log_error_db = value;
            comparison->valid = 1;
            errors[result->shared_valid_band_count++] = value;
            squared += value * value;
        }
    }
    result->shared_reference_coverage =
        result->reference.valid_band_count == 0U ? 0.0 :
        (double)result->shared_valid_band_count /
        (double)result->reference.valid_band_count;
    if (result->shared_valid_band_count != 0U) {
        result->t60_log_rmse_db = sqrt(
            squared / (double)result->shared_valid_band_count);
        result->median_t60_log_bias_db = hwa_hd_median(
            errors, result->shared_valid_band_count);
    }
    if (result->shared_valid_band_count >= HWA_HD_MIN_VALID_HARMONICS &&
        result->shared_reference_coverage >= 0.5) {
        result->comparison_valid = 1;
    }
    free(errors);
    hwa_hd_work_remove(work, errors_bytes);
    return 0;
}

int hwa_harmonic_decay_wavs(const char *reference_path,
                            const char *model_path,
                            const HWAHarmonicDecayOptions *options,
                            HWAHarmonicDecayResult *result,
                            char *error,
                            size_t error_size)
{
    HWAHarmonicDecayOptions copied;
    HWAHarmonicDecayResult computed;
    HWAHDWork work;
    uint64_t evaluation_start;
    int status = -1;

    if (error != NULL && error_size > 0U) error[0] = '\0';
    if (result != NULL) memset(result, 0, sizeof(*result));
    if (reference_path == NULL || options == NULL || result == NULL) {
        hwa_set_error(error, error_size,
                      "invalid harmonic-decay arguments");
        return -1;
    }
    copied = *options;
    if (hwa_hd_options_validate(&copied, error, error_size) != 0) return -1;
    memset(&computed, 0, sizeof(computed));
    memset(&work, 0, sizeof(work));
    work.maximum_work = copied.max_work_bytes;
    work.maximum_evaluations = copied.max_evaluations;
    computed.options = copied;
    computed.expected_hz = copied.expected_hz;
    computed.model_present = model_path != NULL;
    evaluation_start = work.evaluations;
    if (hwa_hd_profile(reference_path, &copied, &computed.reference,
                       &work, error, error_size) != 0) {
        goto cleanup;
    }
    computed.reference.evaluation_count =
        work.evaluations - evaluation_start;
    if (model_path != NULL) {
        evaluation_start = work.evaluations;
        if (hwa_hd_profile(model_path, &copied, &computed.model,
                           &work, error, error_size) != 0 ||
            hwa_hd_compare_profiles(&computed, &work,
                                    error, error_size) != 0) {
            goto cleanup;
        }
        computed.model.evaluation_count =
            work.evaluations - evaluation_start;
    }
    computed.peak_work_bytes = work.peak;
    computed.evaluation_count = work.evaluations;
    *result = computed;
    memset(&computed, 0, sizeof(computed));
    status = 0;

cleanup:
    hwa_harmonic_decay_result_free(&computed);
    return status;
}

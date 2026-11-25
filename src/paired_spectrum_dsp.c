#include "paired_spectrum_dsp.h"

#include "dsp.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HWA_PAIRED_MIN_FFT 256U
#define HWA_PAIRED_MAX_FFT 65536U
#define HWA_PAIRED_PI 3.14159265358979323846
#define HWA_PAIRED_SILENCE_POWER 1.0e-9

typedef struct HWAPairedSpectrumFrame {
    unsigned char *raw;
    double *samples;
    double *window;
    HwaDspComplex *fft;
    double *power;
    size_t size;
    size_t oldest;
    double bin_hz;
    double power_scale;
} HWAPairedSpectrumFrame;

typedef struct HWAPairedPitchMean {
    uint64_t count;
    double log_pitch;
    double confidence;
} HWAPairedPitchMean;

void hwa_paired_spectrum_dsp_options_default(HWAPairedSpectrumDspOptions *options)
{
    if (options == NULL) return;
    memset(options, 0, sizeof(*options));
    options->pitch_tolerance_cents = 50.0;
    options->min_pitch_confidence = 0.65;
    options->max_decoded_frames = UINT64_C(2000000000);
    options->max_transforms = UINT64_C(1000000);
    options->max_work_bytes = UINT64_C(536870912);
}

const char *hwa_paired_spectrum_side_status_text(HWAPairedSpectrumSideStatus status)
{
    switch (status) {
        case HWA_PAIRED_SPECTRUM_SIDE_OK: return "ok";
        case HWA_PAIRED_SPECTRUM_SIDE_TOO_SHORT: return "too-short";
        case HWA_PAIRED_SPECTRUM_SIDE_NO_SIGNAL: return "no-signal";
        case HWA_PAIRED_SPECTRUM_SIDE_PITCH_UNVERIFIED: return "pitch-unverified";
        case HWA_PAIRED_SPECTRUM_SIDE_PITCH_MISMATCH: return "pitch-mismatch";
        case HWA_PAIRED_SPECTRUM_SIDE_UNSUPPORTED_BAND: return "unsupported-band";
    }
    return "unknown";
}

static int hwa_paired_fft_size_valid(size_t size)
{
    return size >= HWA_PAIRED_MIN_FFT && size <= HWA_PAIRED_MAX_FFT &&
           (size & (size - 1U)) == 0U;
}

int hwa_paired_spectrum_dsp_options_validate(const HWAPairedSpectrumDspOptions *options,
                                             char *error,
                                             size_t error_size)
{
    if (options == NULL ||
        !isfinite(options->pitch_tolerance_cents) || options->pitch_tolerance_cents < 0.0 ||
        !isfinite(options->min_pitch_confidence) || options->min_pitch_confidence < 0.0 ||
        options->min_pitch_confidence > 1.0 ||
        options->max_decoded_frames == 0U || options->max_transforms == 0U ||
        options->max_work_bytes == 0U) {
        hwa_set_error(error, error_size, "invalid paired spectrum options");
        return -1;
    }
    if (!isfinite(options->normalization_min_hz) || options->normalization_min_hz < 0.0 ||
        !isfinite(options->normalization_max_hz) || options->normalization_max_hz < 0.0 ||
        (options->normalization_max_hz > 0.0 &&
         options->normalization_max_hz <= options->normalization_min_hz)) {
        hwa_set_error(error, error_size, "invalid paired spectrum normalization band");
        return -1;
    }
    if (options->fft_size != 0U && !hwa_paired_fft_size_valid(options->fft_size)) {
        hwa_set_error(error, error_size,
                      "paired spectrum frame size must be a power of two from %u to %u",
                      HWA_PAIRED_MIN_FFT, HWA_PAIRED_MAX_FFT);
        return -1;
    }
    if (options->hop_size > HWA_PAIRED_MAX_FFT ||
        (options->fft_size != 0U && options->hop_size > options->fft_size)) {
        hwa_set_error(error, error_size, "paired spectrum hop exceeds frame size");
        return -1;
    }
    return 0;
}

static size_t hwa_paired_adaptive_fft(uint32_t rate,
                                      uint64_t span,
                                      size_t hop_size,
                                      double expected_pitch_hz)
{
    /* Pick the power of two nearest 90 ms, then fit the held span. */
    double target = 0.090 * (double)rate;
    size_t size = HWA_PAIRED_MIN_FFT;

    /* Leave room for at least four periods after rounding down. */
    if (expected_pitch_hz > 0.0) {
        target = fmax(target, 5.7 * (double)rate / fmax(expected_pitch_hz, 20.0));
    }
    while (size <= HWA_PAIRED_MAX_FFT / 2U &&
           (uint64_t)(2U * size) <= span &&
           (target > 1.4142135623730951 * (double)size || size < hop_size)) {
        size *= 2U;
    }
    return size;
}

static void hwa_paired_frame_free(HWAPairedSpectrumFrame *frame)
{
    free(frame->raw);
    free(frame->samples);
    free(frame->window);
    free(frame->fft);
    free(frame->power);
}

static int hwa_paired_frame_init(HWAPairedSpectrumFrame *frame,
                                 const HWAFormat *format,
                                 size_t size,
                                 uint64_t budget,
                                 HWAPairedSpectrumWork *work,
                                 char *error,
                                 size_t error_size)
{
    size_t bins = size / 2U + 1U;
    uint64_t bytes = (uint64_t)size *
        ((uint64_t)format->block_align +
         (uint64_t)format->channels * sizeof(*frame->samples) +
         sizeof(*frame->fft)) +
        (uint64_t)(size + 1U) * sizeof(*frame->window) +
        (uint64_t)bins * sizeof(*frame->power);
    size_t index;
    double energy = 0.0;

    if (bytes > budget || bytes > SIZE_MAX) {
        hwa_set_error(error, error_size, "paired spectrum exceeds work byte limit");
        return -1;
    }
    frame->size = size;
    frame->bin_hz = (double)format->sample_rate_hz / (double)size;
    frame->raw = malloc(size * format->block_align);
    frame->samples = malloc(size * format->channels * sizeof(*frame->samples));
    frame->window = malloc((size + 1U) * sizeof(*frame->window));
    frame->fft = malloc(size * sizeof(*frame->fft));
    frame->power = malloc(bins * sizeof(*frame->power));
    if (frame->raw == NULL || frame->samples == NULL || frame->window == NULL ||
        frame->fft == NULL || frame->power == NULL) {
        hwa_set_error(error, error_size, "out of memory measuring paired spectrum");
        return -1;
    }
    if (work->peak_bytes < bytes) work->peak_bytes = bytes;
    /* Drop the repeated endpoint to get a periodic Hann. Its known circular
     * autocorrelation lets the pitch estimate compensate for the window. */
    if (hwa_dsp_hann(frame->window, size + 1U) != HWA_DSP_OK) {
        hwa_set_error(error, error_size, "could not construct paired spectrum window");
        return -1;
    }
    for (index = 0U; index < size; ++index) {
        energy += frame->window[index] * frame->window[index];
    }
    frame->power_scale = 1.0 / ((double)size * energy * (double)format->channels);
    return 0;
}

static int hwa_paired_frame_read(HWAPairedSpectrumFrame *frame,
                                 HWAWavReader *slice,
                                 size_t count,
                                 char *error,
                                 size_t error_size)
{
    size_t frames_read;
    size_t index;
    /* Leave headroom for the FFT and squaring before power normalization. */
    double max_sample = sqrt(DBL_MAX) / (4.0 * (double)frame->size);

    if (hwa_wav_reader_read_frames(slice, frame->raw, count, &frames_read,
                                   error, error_size) != 0) return -1;
    if (frames_read != count) {
        hwa_set_error(error, error_size, "short WAVE read in held span");
        return -1;
    }
    for (index = 0U; index < count; ++index) {
        size_t ring_index = (frame->oldest + index) & (frame->size - 1U);
        size_t channel;

        for (channel = 0U; channel < slice->format.channels; ++channel) {
            int clipped;
            double sample = hwa_wav_decode_sample(
                slice, frame->raw + index * slice->format.block_align +
                       channel * slice->bytes_per_sample, &clipped);

            if (!isfinite(sample) || fabs(sample) > max_sample) {
                hwa_set_error(error, error_size,
                              "nonfinite or excessive WAVE sample in held span");
                return -1;
            }
            frame->samples[ring_index * slice->format.channels + channel] = sample;
        }
    }
    frame->oldest = (frame->oldest + count) & (frame->size - 1U);
    return 0;
}

static int hwa_paired_frame_power(HWAPairedSpectrumFrame *frame,
                                  size_t channels,
                                  HWAPairedSpectrumWork *work)
{
    size_t nyquist = frame->size / 2U;
    size_t channel;

    memset(frame->power, 0, (nyquist + 1U) * sizeof(*frame->power));
    for (channel = 0U; channel < channels; ++channel) {
        size_t index;

        for (index = 0U; index < frame->size; ++index) {
            size_t sample_index = (frame->oldest + index) & (frame->size - 1U);
            frame->fft[index].real =
                frame->samples[sample_index * channels + channel] * frame->window[index];
            frame->fft[index].imag = 0.0;
        }
        if (hwa_dsp_fft(frame->fft, frame->size, 0) != HWA_DSP_OK) return -1;
        work->transforms++;
        for (index = 0U; index <= nyquist; ++index) {
            double real = frame->fft[index].real;
            double imag = frame->fft[index].imag;
            double scale = index == 0U || index == nyquist
                               ? frame->power_scale
                               : 2.0 * frame->power_scale;

            frame->power[index] += scale * (real * real + imag * imag);
        }
    }
    return 0;
}

static double hwa_paired_band_power(const HWAPairedSpectrumFrame *frame,
                                     double low_hz,
                                     double high_hz)
{
    size_t nyquist = frame->size / 2U;
    size_t first = (size_t)floor(low_hz / frame->bin_hz + 0.5);
    size_t last = (size_t)floor(high_hz / frame->bin_hz + 0.5);
    size_t bin;
    double sum = 0.0;

    /* Share an edge bin between adjacent bands in proportion to overlap. */
    for (bin = first; bin <= last; ++bin) {
        double left = bin == 0U ? 0.0 : ((double)bin - 0.5) * frame->bin_hz;
        double right = ((double)bin + (bin == nyquist ? 0.0 : 0.5)) * frame->bin_hz;
        double overlap = fmin(right, high_hz) - fmax(left, low_hz);

        if (overlap > 0.0) {
            sum += frame->power[bin] * (overlap / (right - left));
        }
    }
    return sum;
}

static int hwa_paired_peak(const HwaDspComplex *correlation, size_t index)
{
    return correlation[index].real > correlation[index - 1U].real &&
           correlation[index].real >= correlation[index + 1U].real;
}

static int hwa_paired_frame_pitch(HWAPairedSpectrumFrame *frame,
                                  uint32_t rate,
                                  HWAPairedSpectrumWork *work,
                                  double *pitch_hz,
                                  double *confidence)
{
    size_t first = (size_t)(rate / 5000U);
    size_t last = (size_t)(rate / 20U);
    size_t bin;
    size_t lag;
    double zero_lag;
    double best = 0.0;

    *pitch_hz = 0.0;
    *confidence = 0.0;
    if (first < 2U) first = 2U;
    if (last > frame->size / 4U) last = frame->size / 4U;
    if (first > last) return 0;
    for (bin = 0U; bin < frame->size; ++bin) {
        size_t folded = bin <= frame->size / 2U ? bin : frame->size - bin;
        double hz = (double)folded * frame->bin_hz;
        double value = 0.0;

        /* Bins 0 and 1 also contain DC after applying the periodic Hann. */
        if (folded > 1U && hz >= 20.0 && hz <= 8000.0) {
            value = frame->power[folded];
            if (folded != frame->size / 2U) value *= 0.5;
        }
        frame->fft[bin].real = value;
        frame->fft[bin].imag = 0.0;
    }
    if (hwa_dsp_fft(frame->fft, frame->size, 1) != HWA_DSP_OK) return -1;
    work->transforms++;
    zero_lag = frame->fft[0U].real;
    if (zero_lag <= HWA_PAIRED_SILENCE_POWER / (double)frame->size) return 0;
    for (lag = first - 1U; lag <= last + 1U; ++lag) {
        double phase = 2.0 * HWA_PAIRED_PI * (double)lag / (double)frame->size;
        double hann_corr = (2.0 + cos(phase)) / 3.0;

        frame->fft[lag].real = (frame->fft[lag].real / zero_lag) / hann_corr;
    }
    for (lag = first; lag <= last; ++lag) {
        if (hwa_paired_peak(frame->fft, lag)) best = fmax(best, frame->fft[lag].real);
    }
    if (best <= 0.0) return 0;
    /* Prefer the first strong peak. Later repeats can be just as periodic. */
    for (lag = first; lag <= last; ++lag) {
        double center = frame->fft[lag].real;

        if (center >= 0.94 * best && hwa_paired_peak(frame->fft, lag)) {
            double left = frame->fft[lag - 1U].real;
            double right = frame->fft[lag + 1U].real;
            double adjustment = 0.5 * (left - right) / (left - 2.0 * center + right);

            *pitch_hz = (double)rate / ((double)lag + adjustment);
            *confidence = fmin(1.0, center);
            break;
        }
    }
    return 0;
}

static void hwa_paired_pitch_add(HWAPairedPitchMean *mean,
                                 double pitch_hz, double confidence)
{
    ++mean->count;
    mean->log_pitch += (log(pitch_hz) - mean->log_pitch) / (double)mean->count;
    mean->confidence += (confidence - mean->confidence) / (double)mean->count;
}

int hwa_paired_spectrum_measure(HWAWavReader *reader,
                                uint64_t start,
                                uint64_t end,
                                double expected_pitch_hz,
                                const HWAPairedSpectrumBand *bands,
                                size_t band_count,
                                const HWAPairedSpectrumDspOptions *options,
                                HWAPairedSpectrumWork *work,
                                HWAPairedSpectrumSide *result,
                                double *band_powers,
                                char *error,
                                size_t error_size)
{
    HWAWavReader slice;
    HWAPairedSpectrumFrame frame = {0};
    HWAPairedPitchMean measured = {0};
    HWAPairedPitchMean matched = {0};
    uint64_t signal_windows = 0U;
    uint64_t minimum_windows;
    uint64_t windows;
    uint64_t decoded;
    uint64_t transforms_per_window;
    uint64_t index;
    size_t band;
    size_t fft_size;
    size_t hop_size;
    double nyquist_hz;
    double normalization_high_hz;
    double signal_confidence = 0.0;
    int outcome = -1;

    if (reader == NULL || reader->source.read_at == NULL ||
        reader->format.channels == 0U || reader->format.sample_rate_hz == 0U ||
        reader->format.block_align == 0U ||
        work == NULL || result == NULL ||
        (band_count != 0U && (bands == NULL || band_powers == NULL)) ||
        !isfinite(expected_pitch_hz) || expected_pitch_hz < 0.0 ||
        start > end || end > reader->format.frames) {
        hwa_set_error(error, error_size, "invalid paired spectrum measurement");
        return -1;
    }
    if (hwa_paired_spectrum_dsp_options_validate(options, error, error_size) != 0) return -1;
    memset(result, 0, sizeof(*result));
    nyquist_hz = 0.5 * (double)reader->format.sample_rate_hz;
    normalization_high_hz = options->normalization_max_hz > 0.0
        ? options->normalization_max_hz : nyquist_hz;
    if (normalization_high_hz > nyquist_hz ||
        options->normalization_min_hz >= normalization_high_hz) {
        hwa_set_error(error, error_size, "paired spectrum normalization band lies outside Nyquist");
        return -1;
    }
    for (band = 0U; band < band_count; ++band) {
        band_powers[band] = 0.0;
    }
    for (band = 0U; band < band_count; ++band) {
        if (!isfinite(bands[band].low_hz) || !isfinite(bands[band].high_hz) ||
            bands[band].low_hz < 0.0 || bands[band].high_hz <= bands[band].low_hz) {
            hwa_set_error(error, error_size, "invalid paired spectrum band");
            return -1;
        }
        if (bands[band].high_hz > nyquist_hz) {
            result->status = HWA_PAIRED_SPECTRUM_SIDE_UNSUPPORTED_BAND;
            return 0;
        }
    }
    fft_size = options->fft_size != 0U
        ? options->fft_size
        : hwa_paired_adaptive_fft(reader->format.sample_rate_hz, end - start,
                                  options->hop_size, expected_pitch_hz);
    if (end - start < (uint64_t)fft_size || options->hop_size > fft_size) {
        result->status = HWA_PAIRED_SPECTRUM_SIDE_TOO_SHORT;
        return 0;
    }
    hop_size = options->hop_size != 0U ? options->hop_size : fft_size / 2U;
    result->fft_size = fft_size;
    result->hop_size = hop_size;
    windows = 1U + (end - start - (uint64_t)fft_size) / (uint64_t)hop_size;
    decoded = (uint64_t)fft_size + (windows - 1U) * (uint64_t)hop_size;
    transforms_per_window = (uint64_t)reader->format.channels +
                            (expected_pitch_hz > 0.0 ? 1U : 0U);
    if (work->decoded_frames > options->max_decoded_frames ||
        decoded > options->max_decoded_frames - work->decoded_frames) {
        hwa_set_error(error, error_size, "paired spectrum exceeds decoded frame limit");
        return -1;
    }
    if (work->transforms > options->max_transforms ||
        windows > (options->max_transforms - work->transforms) / transforms_per_window) {
        hwa_set_error(error, error_size, "paired spectrum exceeds transform limit");
        return -1;
    }
    if (hwa_paired_frame_init(&frame, &reader->format, fft_size,
                              options->max_work_bytes, work, error, error_size) != 0) {
        goto cleanup;
    }
    /* The reader already validated the data chunk and its absolute offset. */
    slice = *reader;
    slice.cursor = reader->data_offset + start * (uint64_t)reader->format.block_align;
    slice.bytes_remaining = decoded * (uint64_t)reader->format.block_align;
    for (index = 0U; index < windows; ++index) {
        size_t count = index == 0U ? fft_size : hop_size;
        double power;

        if (hwa_paired_frame_read(&frame, &slice, count, error, error_size) != 0) {
            goto cleanup;
        }
        work->decoded_frames += (uint64_t)count;
        if (hwa_paired_frame_power(&frame, reader->format.channels, work) != 0) {
            hwa_set_error(error, error_size, "paired spectrum FFT failed");
            goto cleanup;
        }
        result->window_count++;
        power = hwa_paired_band_power(&frame, 0.0, nyquist_hz);
        if (!isfinite(power) || power < 0.0) goto bad_power;
        if (power <= HWA_PAIRED_SILENCE_POWER) continue;
        signal_windows++;
        if (expected_pitch_hz > 0.0) {
            double pitch_hz;
            double confidence;
            double cents;

            if (hwa_paired_frame_pitch(&frame, reader->format.sample_rate_hz, work,
                                       &pitch_hz, &confidence) != 0) {
                hwa_set_error(error, error_size, "paired spectrum pitch FFT failed");
                goto cleanup;
            }
            signal_confidence += (confidence - signal_confidence) / (double)signal_windows;
            if (pitch_hz <= 0.0 || confidence < options->min_pitch_confidence) continue;
            hwa_paired_pitch_add(&measured, pitch_hz, confidence);
            cents = 1200.0 * fabs(log2(pitch_hz) - log2(expected_pitch_hz));
            if (cents > options->pitch_tolerance_cents) continue;
            hwa_paired_pitch_add(&matched, pitch_hz, confidence);
        }
        result->usable_windows++;
        if (options->normalization_min_hz > 0.0 || normalization_high_hz < nyquist_hz) {
            power = hwa_paired_band_power(&frame, options->normalization_min_hz,
                                          normalization_high_hz);
            if (!isfinite(power) || power < 0.0) goto bad_power;
        }
        result->total_power += (power - result->total_power) / (double)result->usable_windows;
        for (band = 0U; band < band_count; ++band) {
            power = hwa_paired_band_power(&frame, bands[band].low_hz, bands[band].high_hz);
            if (!isfinite(power) || power < 0.0) goto bad_power;
            band_powers[band] += (power - band_powers[band]) / (double)result->usable_windows;
        }
    }
    outcome = 0;
    result->pitch_windows = measured.count;
    /* Require a strict majority of the held windows with audible signal. */
    minimum_windows = signal_windows / 2U + 1U;
    if (signal_windows == 0U) {
        result->status = HWA_PAIRED_SPECTRUM_SIDE_NO_SIGNAL;
    } else if (expected_pitch_hz > 0.0) {
        const HWAPairedPitchMean *pitch = &matched;

        if (measured.count < minimum_windows) {
            result->status = HWA_PAIRED_SPECTRUM_SIDE_PITCH_UNVERIFIED;
        } else if (matched.count < minimum_windows) {
            result->status = HWA_PAIRED_SPECTRUM_SIDE_PITCH_MISMATCH;
        }
        if (result->status != HWA_PAIRED_SPECTRUM_SIDE_OK) pitch = &measured;
        result->measured_pitch_hz = pitch->count != 0U ? exp(pitch->log_pitch) : 0.0;
        result->pitch_confidence = pitch->count != 0U ? pitch->confidence : signal_confidence;
    }
    goto cleanup;
bad_power:
    hwa_set_error(error, error_size, "invalid paired spectrum power");
cleanup:
    hwa_paired_frame_free(&frame);
    return outcome;
}

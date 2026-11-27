#include "speech_span_dsp.h"

#include "dsp.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>

#define SPEECH_HIGH_BAND_LOW_HZ 2000.0
#define SPEECH_HIGH_BAND_HIGH_HZ 8000.0
#define SPEECH_VOWEL_BAND_WINDOWS 12U
#define SPEECH_CONTRAST_FLOOR_SHARE 1e-5

typedef struct SpeechContrastSpectrum {
    HwaDspComplex *bins;
    double *window;
    size_t fft_size;
    size_t window_size;
    double bin_hz;
    double power_scale;
    double high_hz;
} SpeechContrastSpectrum;

static double speech_mean_power(const double *samples, size_t count)
{
    double sum = 0.0;
    size_t index;

    for (index = 0U; index < count; ++index) sum += samples[index] * samples[index];
    return sum / (double)count;
}

static double speech_contrast_db(double power, double baseline, double floor_power)
{
    return 10.0 * (log10(fmax(power, floor_power)) - log10(fmax(baseline, floor_power)));
}

static size_t speech_next_window(size_t start, size_t last, size_t hop)
{
    return start + (last - start < hop ? last - start : hop);
}

static void speech_transition_envelope(const double *transition, size_t transition_count,
                                       size_t window_size, size_t hop, double baseline,
                                       double floor_power, HWASpeechContrast *result)
{
    size_t last = transition_count - window_size;
    size_t start = 0U;
    double previous = speech_mean_power(transition, window_size);
    double peak = previous;
    double attack = 0.0;

    /* A release can peak on either side of the phone boundary. */
    while (start < last) {
        double power;

        start = speech_next_window(start, last, hop);
        power = speech_mean_power(transition + start, window_size);
        peak = fmax(peak, power);
        attack = fmax(attack, power - previous);
        previous = power;
    }
    result->level_contrast_db = speech_contrast_db(peak, baseline, floor_power);
    if (last != 0U) {
        result->attack_contrast_db = speech_contrast_db(attack, baseline, floor_power);
    }
}

static int speech_contrast_spectrum_open(SpeechContrastSpectrum *spectrum,
                                         size_t window_size, unsigned rate,
                                         double high_hz, uint64_t available_bytes,
                                         char *error, size_t error_size)
{
    double window_power = 0.0;
    size_t index;

    spectrum->fft_size = 1U;
    while (spectrum->fft_size < window_size) spectrum->fft_size *= 2U;
    if ((uint64_t)spectrum->fft_size * sizeof(*spectrum->bins) +
            (uint64_t)window_size * sizeof(*spectrum->window) > available_bytes) {
        hwa_set_error(error, error_size, "speech contrast exceeds the work memory limit");
        return -1;
    }
    spectrum->bins = malloc(spectrum->fft_size * sizeof(*spectrum->bins));
    spectrum->window = malloc(window_size * sizeof(*spectrum->window));
    if (spectrum->bins == NULL || spectrum->window == NULL) {
        hwa_set_error(error, error_size, "cannot allocate speech contrast FFT workspace");
        return -1;
    }
    if (hwa_dsp_hann(spectrum->window, window_size) != HWA_DSP_OK) {
        hwa_set_error(error, error_size, "cannot create speech contrast FFT window");
        return -1;
    }
    for (index = 0U; index < window_size; ++index) {
        window_power += spectrum->window[index] * spectrum->window[index];
    }
    spectrum->window_size = window_size;
    spectrum->bin_hz = (double)rate / (double)spectrum->fft_size;
    spectrum->power_scale = 1.0 / ((double)spectrum->fft_size * window_power);
    spectrum->high_hz = high_hz;
    return 0;
}

static int speech_high_band_power(SpeechContrastSpectrum *spectrum, const double *samples,
                                  HWASpeechWork *work, double *result,
                                  char *error, size_t error_size)
{
    size_t nyquist = spectrum->fft_size / 2U;
    size_t first = (size_t)floor(SPEECH_HIGH_BAND_LOW_HZ / spectrum->bin_hz + 0.5);
    size_t last = (size_t)floor(spectrum->high_hz / spectrum->bin_hz + 0.5);
    double power = 0.0;
    size_t index;

    if (hwa_speech_charge_transforms(work, 1U, error, error_size) != 0) return -1;
    for (index = 0U; index < spectrum->fft_size; ++index) {
        spectrum->bins[index].real = index < spectrum->window_size
            ? samples[index] * spectrum->window[index] : 0.0;
        spectrum->bins[index].imag = 0.0;
    }
    if (hwa_dsp_fft(spectrum->bins, spectrum->fft_size, 0) != HWA_DSP_OK) {
        hwa_set_error(error, error_size, "speech contrast FFT failed");
        return -1;
    }
    /* Each bin contributes the fraction of its width inside the band. */
    for (index = first; index <= last; ++index) {
        double left = fmax(0.0, ((double)index - 0.5) * spectrum->bin_hz);
        double right = ((double)index + (index == nyquist ? 0.0 : 0.5)) * spectrum->bin_hz;
        double overlap = fmin(right, spectrum->high_hz) - fmax(left, SPEECH_HIGH_BAND_LOW_HZ);
        double real = spectrum->bins[index].real;
        double imag = spectrum->bins[index].imag;
        double scale = index == 0U || index == nyquist ? 1.0 : 2.0;

        if (overlap > 0.0) power += (real * real + imag * imag) * scale * overlap / (right - left);
    }
    *result = power * spectrum->power_scale;
    return 0;
}

static int speech_transition_band(const double *transition, size_t transition_count,
                                  const double *vowel, size_t vowel_count, size_t window_size,
                                  size_t hop, unsigned rate, double high_hz, double floor_power,
                                  HWASpeechWork *work, double *result,
                                  char *error, size_t error_size)
{
    SpeechContrastSpectrum spectrum = {0};
    size_t vowel_last = vowel_count - window_size;
    size_t vowel_hop = vowel_last / (SPEECH_VOWEL_BAND_WINDOWS - 1U) + 1U;
    size_t transition_last = transition_count - window_size;
    size_t start;
    size_t vowel_windows = 0U;
    double baseline = 0.0;
    double peak = 0.0;
    int status = -1;

    if (speech_contrast_spectrum_open(&spectrum, window_size, rate, high_hz,
                                     work->available_bytes, error, error_size) != 0) goto cleanup;
    for (start = 0U;; start = speech_next_window(start, vowel_last, vowel_hop)) {
        double power;

        if (speech_high_band_power(&spectrum, vowel + start, work, &power,
                                   error, error_size) != 0) goto cleanup;
        baseline += power;
        ++vowel_windows;
        if (start == vowel_last) break;
    }
    for (start = 0U;; start = speech_next_window(start, transition_last, hop)) {
        double power;

        if (speech_high_band_power(&spectrum, transition + start, work, &power,
                                   error, error_size) != 0) goto cleanup;
        peak = fmax(peak, power);
        if (start == transition_last) break;
    }
    *result = speech_contrast_db(peak, baseline / (double)vowel_windows, floor_power);
    status = 0;

cleanup:
    free(spectrum.window);
    free(spectrum.bins);
    return status;
}

int hwa_speech_contrast(const double *transition, size_t transition_count,
                        const double *vowel, size_t vowel_count, unsigned rate, double max_hz,
                        HWASpeechWork *work, HWASpeechContrast *result,
                        char *error, size_t error_size)
{
    size_t window_size = (size_t)floor(0.006 * (double)rate + 0.5);
    size_t hop = (size_t)floor(0.002 * (double)rate + 0.5);
    double high_hz = fmin(fmin(max_hz, (double)rate / 2.0), SPEECH_HIGH_BAND_HIGH_HZ);
    double baseline;
    double floor_power;

    *result = (HWASpeechContrast){"indeterminate", "transition-too-short", NAN, NAN, NAN};
    if (window_size < 3U || hop == 0U || transition_count < window_size)
        return 0;
    if (vowel_count < window_size) {
        result->reason = "vowel-too-short";
        return 0;
    }
    baseline = speech_mean_power(vowel, vowel_count);
    if (!isfinite(baseline) || baseline < DBL_MIN / SPEECH_CONTRAST_FLOOR_SHARE) {
        result->reason = "vowel-no-signal";
        return 0;
    }
    floor_power = baseline * SPEECH_CONTRAST_FLOOR_SHARE;

    speech_transition_envelope(transition, transition_count, window_size, hop,
                               baseline, floor_power, result);
    if (high_hz > SPEECH_HIGH_BAND_LOW_HZ &&
        speech_transition_band(transition, transition_count, vowel, vowel_count, window_size,
                               hop, rate, high_hz, floor_power, work,
                               &result->high_band_contrast_db, error, error_size) != 0) return -1;

    if (isfinite(result->level_contrast_db) || isfinite(result->high_band_contrast_db)) {
        result->status = "ok";
        result->reason = NULL;
    }
    return 0;
}

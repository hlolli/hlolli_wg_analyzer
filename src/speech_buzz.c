#include "speech_span_dsp.h"

#include "dsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BUZZ_PI 3.14159265358979323846
#define BUZZ_MAX_WINDOW 32768U
#define BUZZ_PITCH_MIN_HZ 60U
#define BUZZ_PITCH_MAX_HZ 550U
#define BUZZ_PITCH_BUCKETS (BUZZ_PITCH_MAX_HZ + 1U)
#define BUZZ_PITCH_LINES 12U
#define BUZZ_MIN_COVERAGE 0.70
#define BUZZ_MIN_PERSISTENCE 0.55
#define BUZZ_MIN_INDEPENDENT 0.35

typedef struct BuzzPeak {
    double frequency_hz;
    double power_share;
    double strength_db;
    int independent;
} BuzzPeak;

typedef struct BuzzTrack {
    BuzzPeak last;
    size_t last_window;
    size_t count;
    size_t independent;
    double frequency_sum;
    double power_sum;
    double strength_sum;
} BuzzTrack;

typedef struct BuzzPitchTrack {
    double previous_hz;
    size_t count;
    size_t jumps;
    int large_jump;
} BuzzPitchTrack;

typedef struct BuzzSpectrum {
    HwaDspComplex *fft;
    double *window;
    double *power;
    BuzzTrack *fixed;
    size_t frame_size;
    size_t fft_size;
    size_t last_bin;
    double resolution_hz;
    double bin_hz;
    double line_scale;
    double min_power_share;
} BuzzSpectrum;

static void buzz_spectrum_close(BuzzSpectrum *spectrum)
{
    free(spectrum->fixed);
    free(spectrum->power);
    free(spectrum->window);
    free(spectrum->fft);
}

static int buzz_spectrum_open(BuzzSpectrum *spectrum, size_t frame_size,
                              unsigned rate, double max_hz, int track_fixed,
                              uint64_t budget, char *error, size_t error_size)
{
    size_t fft_size = 1U;
    size_t bins;
    size_t index;
    uint64_t bytes;
    double window_sum = 0.0;
    double window_energy = 0.0;

    while (fft_size < 2U * frame_size) fft_size *= 2U;
    bins = fft_size / 2U + 1U;
    spectrum->frame_size = frame_size;
    spectrum->fft_size = fft_size;
    spectrum->bin_hz = (double)rate / (double)fft_size;
    spectrum->resolution_hz = (double)rate / (double)frame_size;
    spectrum->last_bin = (size_t)floor(max_hz / spectrum->bin_hz);
    if (spectrum->last_bin >= bins) spectrum->last_bin = bins - 1U;
    bytes = (uint64_t)fft_size * sizeof(*spectrum->fft) +
            (uint64_t)frame_size * sizeof(*spectrum->window) +
            (uint64_t)bins * sizeof(*spectrum->power);
    if (track_fixed) bytes += (uint64_t)(spectrum->last_bin + 1U) * sizeof(*spectrum->fixed);
    if (bytes > budget || bytes > SIZE_MAX) {
        hwa_set_error(error, error_size, "speech buzz analysis exceeds work byte limit");
        return -1;
    }
    spectrum->fft = malloc(fft_size * sizeof(*spectrum->fft));
    spectrum->window = malloc(frame_size * sizeof(*spectrum->window));
    spectrum->power = malloc(bins * sizeof(*spectrum->power));
    if (track_fixed) spectrum->fixed = calloc(spectrum->last_bin + 1U, sizeof(*spectrum->fixed));
    if (spectrum->fft == NULL || spectrum->window == NULL || spectrum->power == NULL ||
        (track_fixed && spectrum->fixed == NULL)) {
        hwa_set_error(error, error_size, "out of memory measuring speech buzz");
        return -1;
    }
    if (hwa_dsp_hann(spectrum->window, frame_size) != HWA_DSP_OK) {
        hwa_set_error(error, error_size, "could not construct speech buzz window");
        return -1;
    }
    for (index = 0U; index < frame_size; ++index) {
        double weight = spectrum->window[index];
        window_sum += weight;
        window_energy += weight * weight;
    }
    /* Correct an interpolated line's peak power for the Hann and zero padding. */
    spectrum->line_scale = (double)fft_size * window_energy / (window_sum * window_sum);
    spectrum->min_power_share = pow(10.0, HWA_SPEECH_BUZZ_LEVEL_FLOOR_DB / 10.0);
    return 0;
}

static int buzz_window_power(BuzzSpectrum *spectrum, const double *samples,
                              HWASpeechWork *work, double *total,
                              char *error, size_t error_size)
{
    size_t index;
    double peak = 0.0;
    double mean = 0.0;

    *total = 0.0;
    for (index = 0U; index < spectrum->frame_size; ++index)
        peak = fmax(peak, fabs(samples[index]));
    if (peak == 0.0) return 0;
    for (index = 0U; index < spectrum->frame_size; ++index)
        mean += samples[index] / peak;
    mean /= (double)spectrum->frame_size;
    memset(spectrum->fft, 0, spectrum->fft_size * sizeof(*spectrum->fft));
    for (index = 0U; index < spectrum->frame_size; ++index)
        spectrum->fft[index].real = (samples[index] / peak - mean) * spectrum->window[index];

    if (hwa_speech_charge_transforms(work, 1U, error, error_size) != 0) return -1;
    if (hwa_dsp_fft(spectrum->fft, spectrum->fft_size, 0) != HWA_DSP_OK) {
        hwa_set_error(error, error_size, "speech buzz FFT failed");
        return -1;
    }
    for (index = 0U; index <= spectrum->fft_size / 2U; ++index) {
        double re = spectrum->fft[index].real;
        double im = spectrum->fft[index].imag;
        spectrum->power[index] = re * re + im * im;
        if (index > 0U && index <= spectrum->last_bin) *total += spectrum->power[index];
    }
    return 0;
}

static int buzz_correlation_peak(const HwaDspComplex *correlation, size_t lag)
{
    return correlation[lag].real > correlation[lag - 1U].real &&
           correlation[lag].real >= correlation[lag + 1U].real;
}

static double buzz_peak_offset(double left, double center, double right)
{
    double curvature = (left - center) + (right - center);
    return curvature < 0.0 ? 0.5 * (left - right) / curvature : 0.0;
}

static size_t buzz_harmonic_index(const BuzzSpectrum *spectrum, double frequency_hz, double pitch);
static int buzz_line_peak(const BuzzSpectrum *spectrum, size_t bin, double total,
                          BuzzPeak *peak, double *height);
static int buzz_narrow_peak(const BuzzSpectrum *spectrum, size_t bin);

static unsigned buzz_common_divisor(unsigned first, unsigned second)
{
    while (second != 0U) {
        unsigned remainder = first % second;
        first = second;
        second = remainder;
    }
    return first;
}

static double buzz_pitch_weight(double frequency)
{
    /* Keep a strong high-frequency tone from supplying the voice period. */
    double ratio = frequency / 650.0;
    double squared = ratio * ratio;
    return 1.0 / (1.0 + squared * squared);
}

static size_t buzz_pitch_lines(const BuzzSpectrum *spectrum, double total, BuzzPeak *lines)
{
    size_t bin, count = 0U;
    size_t first = (size_t)ceil(45.0 / spectrum->bin_hz);
    size_t end = (size_t)ceil(1800.0 / spectrum->bin_hz);

    if (first < 1U) first = 1U;
    if (end > spectrum->last_bin) end = spectrum->last_bin;
    for (bin = first; bin < end; ++bin) {
        BuzzPeak peak = {0};
        double height;
        size_t pos;

        if (!buzz_narrow_peak(spectrum, bin) ||
            !buzz_line_peak(spectrum, bin, total, &peak, &height)) continue;
        peak.power_share *= buzz_pitch_weight(peak.frequency_hz);
        pos = count < BUZZ_PITCH_LINES ? count : BUZZ_PITCH_LINES - 1U;
        if (count == BUZZ_PITCH_LINES && peak.power_share <= lines[pos].power_share) continue;
        while (pos > 0U && peak.power_share > lines[pos - 1U].power_share) {
            lines[pos] = lines[pos - 1U];
            --pos;
        }
        lines[pos] = peak;
        if (count < BUZZ_PITCH_LINES) ++count;
    }
    return count;
}

static double buzz_pitch_score(const BuzzSpectrum *spectrum, const BuzzPeak *lines,
                                size_t count, double pitch)
{
    size_t index;
    unsigned divisor = 0U;
    double score = 0.0;

    /* A longer period needs interleaved lines to support it. Spectral
     * lines at harmonics 2 and 4 alone establish the shorter period. */
    for (index = 0U; index < count; ++index) {
        unsigned harmonic = (unsigned)buzz_harmonic_index(spectrum, lines[index].frequency_hz, pitch);
        if (harmonic != 0U) {
            divisor = buzz_common_divisor(divisor, harmonic);
            score += lines[index].power_share;
        }
    }
    return divisor == 1U ? score : 0.0;
}

static int buzz_pitch(BuzzSpectrum *spectrum, unsigned rate, double total, double previous,
                      double *pitch)
{
    size_t first = (size_t)ceil((double)rate / BUZZ_PITCH_MAX_HZ);
    size_t last = rate / BUZZ_PITCH_MIN_HZ;
    size_t bin;
    size_t lag;
    BuzzPeak lines[BUZZ_PITCH_LINES];
    size_t line_count;
    double zero, floor;
    double best = 0.0, best_score = 0.0;
    double continuation_hz = 0.0, continuation_score = 0.0;

    *pitch = 0.0;
    if (last > spectrum->frame_size / 2U) last = spectrum->frame_size / 2U;
    if (first < 2U) first = 2U;
    if (first > last) return 0;
    for (bin = 0U; bin < spectrum->fft_size; ++bin) {
        size_t folded = bin <= spectrum->fft_size / 2U ? bin : spectrum->fft_size - bin;
        double hz = (double)folded * spectrum->bin_hz;
        double power = 0.0;

        if (hz >= 45.0 && hz <= 1800.0 && folded <= spectrum->last_bin)
            power = spectrum->power[folded] * buzz_pitch_weight(hz);
        spectrum->fft[bin] = (HwaDspComplex){power, 0.0};
    }
    if (hwa_dsp_fft(spectrum->fft, spectrum->fft_size, 1) != HWA_DSP_OK) return -1;
    zero = spectrum->fft[0].real;
    if (zero <= 0.0) return 0;
    for (lag = first - 1U; lag <= last + 1U; ++lag) {
        double fraction = (double)lag / (double)(spectrum->frame_size - 1U);
        double phase = 2.0 * BUZZ_PI * fraction;
        /* Normalize the linear Hann autocorrelation for the overlap at this lag. */
        double overlap = (1.0 - fraction) * (2.0 + cos(phase)) / 3.0 +
                         sin(phase) / (2.0 * BUZZ_PI);
        spectrum->fft[lag].real /= zero * overlap;
    }
    for (lag = first; lag <= last; ++lag) {
        if (buzz_correlation_peak(spectrum->fft, lag))
            best = fmax(best, spectrum->fft[lag].real);
    }
    floor = fmax(0.70, 0.92 * best);
    if (best < floor) return 0;
    line_count = buzz_pitch_lines(spectrum, total, lines);
    for (lag = first; lag <= last; ++lag) {
        double center = spectrum->fft[lag].real;
        double offset, frequency, score;

        if (center < floor || !buzz_correlation_peak(spectrum->fft, lag)) continue;
        offset = buzz_peak_offset(spectrum->fft[lag - 1U].real, center,
                                 spectrum->fft[lag + 1U].real);
        frequency = (double)rate / ((double)lag + offset);
        score = buzz_pitch_score(spectrum, lines, line_count, frequency);
        if (score > best_score) {
            best_score = score;
            *pitch = frequency;
        }
        if (frequency <= 1.18 * previous && previous <= 1.18 * frequency &&
            score > continuation_score) {
            continuation_hz = frequency;
            continuation_score = score;
        }
    }
    /* A narrow fixed tone can briefly align with a subharmonic. Keep a
     * comparably supported pitch when an isolated window suggests a jump. */
    if (continuation_score > 0.0 && continuation_score >= 0.90 * best_score)
        *pitch = continuation_hz;
    return 0;
}

static void buzz_add_peak(BuzzTrack *track, const BuzzPeak *peak, size_t window)
{
    if (track->count != 0U && track->last_window == window) {
        if (track->last.power_share >= peak->power_share) return;
        track->frequency_sum -= track->last.frequency_hz;
        track->power_sum -= track->last.power_share;
        track->strength_sum -= track->last.strength_db;
        track->independent -= (size_t)track->last.independent;
    } else {
        ++track->count;
    }
    track->last = *peak;
    track->last_window = window;
    track->frequency_sum += peak->frequency_hz;
    track->power_sum += peak->power_share;
    track->strength_sum += peak->strength_db;
    track->independent += (size_t)peak->independent;
}

static double buzz_surrounding_power(const BuzzSpectrum *spectrum, size_t bin)
{
    size_t guard = (size_t)ceil(1.75 * spectrum->resolution_hz / spectrum->bin_hz);
    size_t radius = (size_t)ceil(4.0 * spectrum->resolution_hz / spectrum->bin_hz);
    size_t first = bin > radius ? bin - radius : 1U;
    size_t last = bin + radius < spectrum->last_bin ? bin + radius : spectrum->last_bin;
    size_t count = 0U;
    size_t index;
    double sum = 0.0;

    for (index = first; index <= last; ++index) {
        if (index + guard >= bin && index <= bin + guard) continue;
        sum += spectrum->power[index];
        ++count;
    }
    return count != 0U ? sum / (double)count : 0.0;
}

static size_t buzz_harmonic_index(const BuzzSpectrum *spectrum, double frequency_hz, double pitch)
{
    double harmonic = floor(frequency_hz / pitch + 0.5);
    double tolerance = fmin(0.24 * pitch, fmax(0.65 * spectrum->resolution_hz, 0.015 * frequency_hz));

    return harmonic >= 1.0 && fabs(frequency_hz - harmonic * pitch) <= tolerance
        ? (size_t)harmonic : 0U;
}

static int buzz_line_peak(const BuzzSpectrum *spectrum, size_t bin, double total,
                          BuzzPeak *peak, double *height)
{
    double center = spectrum->power[bin];
    double left, right, offset;

    if (center <= spectrum->power[bin - 1U] || center < spectrum->power[bin + 1U] ||
        center * spectrum->line_scale < 0.80 * spectrum->min_power_share * total) return 0;
    left = log(fmax(spectrum->power[bin - 1U], center * 1.0e-12));
    right = log(fmax(spectrum->power[bin + 1U], center * 1.0e-12));
    center = log(center);
    offset = buzz_peak_offset(left, center, right);
    *height = exp(center - 0.25 * (left - right) * offset);
    peak->frequency_hz = ((double)bin + offset) * spectrum->bin_hz;
    peak->power_share = *height * spectrum->line_scale / total;
    return peak->power_share >= spectrum->min_power_share;
}

static int buzz_measure_peak(const BuzzSpectrum *spectrum, size_t bin, double total, BuzzPeak *peak)
{
    double height, surrounding;

    if (!buzz_line_peak(spectrum, bin, total, peak, &height)) return 0;
    surrounding = buzz_surrounding_power(spectrum, bin);
    peak->strength_db = 10.0 * log10(height / fmax(surrounding, total * 1.0e-10));
    return peak->strength_db >= 6.0;
}

static int buzz_close_to_fixed(const HWASpeechBuzz *fixed, double frequency_hz, double margin)
{
    size_t index;

    if (fixed == NULL) return 0;
    for (index = 0U; index < HWA_SPEECH_FIXED_PEAKS && fixed->fixed_peaks[index].present; ++index) {
        if (fabs(fixed->fixed_peaks[index].frequency_hz - frequency_hz) <= margin) return 1;
    }
    return 0;
}

static int buzz_narrow_peak(const BuzzSpectrum *spectrum, size_t bin)
{
    size_t shoulder = (size_t)ceil(1.1 * spectrum->resolution_hz / spectrum->bin_hz);
    return bin > shoulder && bin + shoulder <= spectrum->last_bin &&
           fmax(spectrum->power[bin - shoulder], spectrum->power[bin + shoulder]) <=
           0.5 * spectrum->power[bin];
}

static void buzz_collect_peaks(BuzzSpectrum *spectrum, BuzzTrack *harmonics,
                                double total, double pitch, size_t window,
                                const HWASpeechBuzz *exclude_fixed)
{
    size_t bin;
    size_t first = (size_t)ceil(fmax(45.0, 1.5 * spectrum->resolution_hz) / spectrum->bin_hz);

    for (bin = first; bin < spectrum->last_bin; ++bin) {
        BuzzPeak peak;
        size_t harmonic;
        size_t center;
        size_t offset;
        size_t last;

        if (!buzz_measure_peak(spectrum, bin, total, &peak)) continue;
        harmonic = buzz_harmonic_index(spectrum, peak.frequency_hz, pitch);
        peak.independent = harmonic == 0U;
        if (!peak.independent && harmonic <= HWA_SPEECH_HARMONICS &&
            !buzz_close_to_fixed(exclude_fixed, peak.frequency_hz, 1.5 * spectrum->resolution_hz))
            buzz_add_peak(&harmonics[harmonic - 1U], &peak, window);

        if (exclude_fixed != NULL || !buzz_narrow_peak(spectrum, bin)) continue;

        /* Give adjacent centers the vote so a line near a bin boundary can
         * persist despite small estimation errors or a harmonic crossing. */
        center = (size_t)floor(peak.frequency_hz / spectrum->bin_hz + 0.5);
        last = center + 1U <= spectrum->last_bin ? center + 1U : spectrum->last_bin;
        for (offset = center - 1U; offset <= last; ++offset)
            buzz_add_peak(&spectrum->fixed[offset], &peak, window);
    }
}

static double buzz_pitch_percentile(const size_t *buckets, size_t count, size_t percent)
{
    size_t desired = count / 100U * percent + count % 100U * percent / 100U;
    size_t index;
    size_t sum = 0U;

    for (index = BUZZ_PITCH_MIN_HZ; index <= BUZZ_PITCH_MAX_HZ; ++index) {
        sum += buckets[index];
        if (sum > desired) return (double)index;
    }
    return (double)BUZZ_PITCH_MAX_HZ;
}

static HWASpeechPeak buzz_export_peak(const BuzzTrack *track, size_t pitched, double harmonic)
{
    return (HWASpeechPeak){
        .present = 1,
        .frequency_hz = track->frequency_sum / (double)track->count,
        .persistence = (double)track->count / (double)pitched,
        .strength_db = track->strength_sum / (double)track->count,
        .relative_level_db = 10.0 * log10(track->power_sum / (double)track->count),
        .harmonic_number = harmonic
    };
}

static int buzz_near_existing_peak(const HWASpeechPeak *peaks, size_t count,
                                   double hz, double margin)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (fabs(peaks[index].frequency_hz - hz) <= margin) return 1;
    }
    return 0;
}

static void buzz_finish_fixed(const BuzzSpectrum *spectrum, size_t pitched, HWASpeechBuzz *result)
{
    size_t index;

    for (index = 0U; index < HWA_SPEECH_FIXED_PEAKS; ++index) {
        size_t bin;
        const BuzzTrack *best = NULL;

        for (bin = 1U; bin <= spectrum->last_bin; ++bin) {
            const BuzzTrack *track = &spectrum->fixed[bin];

            if ((double)track->count < BUZZ_MIN_PERSISTENCE * (double)pitched ||
                (double)track->independent < BUZZ_MIN_INDEPENDENT * (double)pitched ||
                (best != NULL && track->power_sum <= best->power_sum) ||
                buzz_near_existing_peak(result->fixed_peaks, index,
                                        track->frequency_sum / (double)track->count,
                                        spectrum->resolution_hz)) continue;
            best = track;
        }
        if (best == NULL) break;
        result->fixed_peaks[index] = buzz_export_peak(best, pitched, NAN);
    }
    result->fixed = result->fixed_peaks[0];
}

static void buzz_finish_harmonics(const BuzzTrack *harmonics, size_t pitched,
                                  HWASpeechBuzz *result)
{
    double best_power = 0.0;
    double pitch_motion = result->pitch_max_hz - result->pitch_min_hz;
    size_t index;

    for (index = 0U; index < HWA_SPEECH_HARMONICS; ++index) {
        const BuzzTrack *track = &harmonics[index];
        double harmonic = (double)(index + 1U);

        if ((double)track->count < BUZZ_MIN_PERSISTENCE * (double)pitched ||
            pitch_motion * harmonic < 0.5 * result->frequency_resolution_hz) continue;
        result->harmonics[index] = buzz_export_peak(track, pitched, harmonic);
        if (track->power_sum > best_power) {
            best_power = track->power_sum;
            result->harmonic = result->harmonics[index];
        }
    }
}

static size_t buzz_frame_size(size_t count, unsigned rate, double max_hz, const char **reason)
{
    size_t frame_size;

    if (rate < 1000U || !isfinite(max_hz) || max_hz < BUZZ_PITCH_MIN_HZ) {
        *reason = "unsupported-frequency-range";
        return 0U;
    }
    frame_size = (size_t)((uint64_t)rate * 48U / 1000U);
    if (frame_size > BUZZ_MAX_WINDOW) {
        *reason = "unsupported-sample-rate";
        return 0U;
    }
    if (count < frame_size + 3U * (frame_size / 4U)) frame_size = count / 7U * 4U;
    if (frame_size < rate / 32U) {
        *reason = "too-short";
        return 0U;
    }
    return frame_size;
}

static int buzz_pitched_window(BuzzSpectrum *spectrum, const double *samples,
                                unsigned rate, HWASpeechWork *work, BuzzPitchTrack *track,
                                double *total, double *pitch, char *error, size_t error_size)
{
    double current = 0.0;

    *pitch = 0.0;
    if (buzz_window_power(spectrum, samples, work, total, error, error_size) != 0) return -1;
    if (*total > 0.0) {
        if (hwa_speech_charge_transforms(work, 1U, error, error_size) != 0) return -1;
        if (buzz_pitch(spectrum, rate, *total, track->previous_hz, &current) != 0) {
            hwa_set_error(error, error_size, "speech buzz FFT failed");
            return -1;
        }
    }
    if (current < BUZZ_PITCH_MIN_HZ || current > BUZZ_PITCH_MAX_HZ) return 0;
    if (track->previous_hz > 0.0 &&
        (current > 1.18 * track->previous_hz || track->previous_hz > 1.18 * current)) {
        ++track->jumps;
        if (current > 1.6 * track->previous_hz || track->previous_hz > 1.6 * current)
            track->large_jump = 1;
    } else {
        ++track->count;
        *pitch = current;
    }
    track->previous_hz = current;
    return 0;
}

static int buzz_analyze_windows(BuzzSpectrum *spectrum, const double *samples, unsigned rate,
                                size_t windows, size_t hop_size, HWASpeechWork *work,
                                BuzzTrack *harmonics, const HWASpeechBuzz *exclude_fixed,
                                size_t *pitch_buckets, BuzzPitchTrack *track,
                                char *error, size_t error_size)
{
    size_t window;

    *track = (BuzzPitchTrack){0};
    for (window = 0U; window < windows; ++window) {
        double total;
        double pitch;

        if (buzz_pitched_window(spectrum, samples + window * hop_size, rate, work,
                                track, &total, &pitch, error, error_size) != 0) return -1;
        if (pitch <= 0.0) continue;
        if (pitch_buckets != NULL) ++pitch_buckets[(size_t)floor(pitch + 0.5)];
        buzz_collect_peaks(spectrum, harmonics, total, pitch, window, exclude_fixed);
    }
    return 0;
}

int hwa_speech_buzz(const double *samples, size_t count, unsigned rate,
                    double max_hz, HWASpeechWork *work, HWASpeechBuzz *result,
                    char *error, size_t error_size)
{
    BuzzSpectrum spectrum = {0};
    BuzzTrack harmonics[HWA_SPEECH_HARMONICS] = {0};
    size_t pitch_buckets[BUZZ_PITCH_BUCKETS] = {0};
    size_t frame_size;
    size_t hop_size;
    size_t windows;
    BuzzPitchTrack track;
    int status = -1;

    memset(result, 0, sizeof(*result));
    result->status = "indeterminate";
    result->pitch_min_hz = NAN;
    result->pitch_max_hz = NAN;
    result->pitch_coverage = NAN;
    result->frequency_resolution_hz = NAN;
    frame_size = buzz_frame_size(count, rate, max_hz, &result->reason);
    if (frame_size == 0U) return 0;
    hop_size = frame_size / 4U;
    windows = 1U + (count - frame_size) / hop_size;

    if (buzz_spectrum_open(&spectrum, frame_size, rate, fmin(max_hz, 0.5 * (double)rate),
                            1, work->available_bytes, error, error_size) != 0) goto cleanup;
    result->frequency_resolution_hz = spectrum.resolution_hz;
    if (buzz_analyze_windows(&spectrum, samples, rate, windows, hop_size, work, harmonics,
                             NULL, pitch_buckets, &track, error, error_size) != 0)
        goto cleanup;
    result->pitch_coverage = (double)track.count / (double)windows;
    if (track.count < 4U || result->pitch_coverage < BUZZ_MIN_COVERAGE ||
        track.large_jump || track.jumps > 1U + track.count / 8U) {
        result->reason = "unreliable-pitch";
    } else {
        result->pitch_min_hz = buzz_pitch_percentile(pitch_buckets, track.count, 10U);
        result->pitch_max_hz = buzz_pitch_percentile(pitch_buckets, track.count, 90U);
        if (result->pitch_max_hz < 1.06 * result->pitch_min_hz) {
            result->reason = "insufficient-pitch-motion";
        } else {
            result->status = "ok";
            result->reason = NULL;
            buzz_finish_fixed(&spectrum, track.count, result);
            /* At a crossing, the fixed line can masquerade as a stronger
             * harmonic. Recheck harmonic tracks with those windows masked. */
            if (result->fixed.present) {
                memset(harmonics, 0, sizeof(harmonics));
                if (buzz_analyze_windows(&spectrum, samples, rate, windows, hop_size, work,
                        harmonics, result, NULL, &track, error, error_size) != 0)
                    goto cleanup;
            }
            buzz_finish_harmonics(harmonics, track.count, result);
        }
    }
    status = 0;

cleanup:
    buzz_spectrum_close(&spectrum);
    return status;
}

static int buzz_probe_detected(const BuzzSpectrum *spectrum, size_t bin, double frequency_hz,
                                double pitch, unsigned harmonic, double total)
{
    size_t radius = harmonic == 0U ? 1U : (size_t)ceil(0.24 * pitch / spectrum->bin_hz);
    size_t first = bin > radius ? bin - radius : 1U;
    size_t last = bin + radius < spectrum->last_bin ? bin + radius : spectrum->last_bin - 1U;
    size_t search_bin;

    for (search_bin = first; search_bin <= last; ++search_bin) {
        BuzzPeak peak;

        if ((harmonic != 0U || buzz_narrow_peak(spectrum, search_bin)) &&
            buzz_measure_peak(spectrum, search_bin, total, &peak) &&
            (harmonic == 0U ? fabs(peak.frequency_hz - frequency_hz) <= spectrum->bin_hz
             : buzz_harmonic_index(spectrum, peak.frequency_hz, pitch) == harmonic)) return 1;
    }
    return 0;
}

typedef struct BuzzProbe {
    double frequency_sum;
    double level_sum;
    double strength_sum;
    size_t eligible;
    size_t detections;
} BuzzProbe;

static void buzz_probe_window(BuzzProbe *probe, const HWASpeechPeak *target,
                              const BuzzSpectrum *spectrum, double max_hz, double pitch,
                              double total)
{
    unsigned harmonic = isfinite(target->harmonic_number) ? (unsigned)target->harmonic_number : 0U;
    double frequency = harmonic == 0U ? target->frequency_hz : pitch * (double)harmonic;
    double bin_position, fraction, height;
    size_t bin, lower;

    if (frequency < 45.0 || frequency >= max_hz ||
        (harmonic == 0U && buzz_harmonic_index(spectrum, frequency, pitch) != 0U)) return;
    bin_position = frequency / spectrum->bin_hz;
    lower = (size_t)floor(bin_position);
    fraction = bin_position - (double)lower;
    bin = (size_t)floor(bin_position + 0.5);
    if (bin <= 1U || bin >= spectrum->last_bin) return;

    height = (1.0 - fraction) * spectrum->power[lower] + fraction * spectrum->power[lower + 1U];
    probe->frequency_sum += frequency;
    probe->level_sum += height * spectrum->line_scale / total;
    probe->strength_sum += 10.0 * log10(fmax(height, total * 1.0e-15) /
        fmax(buzz_surrounding_power(spectrum, bin), total * 1.0e-10));
    ++probe->eligible;
    if (buzz_probe_detected(spectrum, bin, frequency, pitch, harmonic, total))
        ++probe->detections;
}

int hwa_speech_buzz_probe(const double *samples, size_t count, unsigned rate, double max_hz,
                          const HWASpeechPeak *const *targets, size_t target_count,
                          HWASpeechWork *work, HWASpeechPeak *results, char *error, size_t error_size)
{
    BuzzSpectrum spectrum = {0};
    BuzzPitchTrack track = {0};
    BuzzProbe probes[HWA_SPEECH_BUZZ_PROBE_LIMIT] = {0};
    const char *reason;
    size_t frame_size = buzz_frame_size(count, rate, max_hz, &reason);
    size_t hop_size;
    size_t window;
    size_t index;
    int status = -1;

    if (target_count > HWA_SPEECH_BUZZ_PROBE_LIMIT) {
        hwa_set_error(error, error_size, "too many speech buzz probes");
        return -1;
    }
    memset(results, 0, target_count * sizeof(*results));
    if (frame_size == 0U || target_count == 0U) return 0;

    max_hz = fmin(max_hz, 0.5 * (double)rate);
    hop_size = frame_size / 4U;
    if (buzz_spectrum_open(&spectrum, frame_size, rate, max_hz,
                            0, work->available_bytes, error, error_size) != 0) goto cleanup;
    for (window = 0U; window <= (count - frame_size) / hop_size; ++window) {
        double total, pitch;

        if (buzz_pitched_window(&spectrum, samples + window * hop_size, rate, work,
                                &track, &total, &pitch, error, error_size) != 0) goto cleanup;
        if (pitch <= 0.0) continue;
        for (index = 0U; index < target_count; ++index) {
            buzz_probe_window(&probes[index], targets[index], &spectrum, max_hz, pitch, total);
        }
    }
    for (index = 0U; index < target_count; ++index) {
        const BuzzProbe *probe = &probes[index];
        double eligible = (double)probe->eligible;

        if (probe->eligible < 2U || eligible < BUZZ_MIN_INDEPENDENT * (double)track.count)
            continue;
        results[index] = (HWASpeechPeak){
            .present = 1,
            .frequency_hz = isfinite(targets[index]->harmonic_number)
                ? probe->frequency_sum / eligible : targets[index]->frequency_hz,
            .persistence = (double)probe->detections / eligible,
            .strength_db = probe->strength_sum / eligible,
            .relative_level_db = 10.0 * log10(fmax(probe->level_sum / eligible, 1.0e-15)),
            .harmonic_number = targets[index]->harmonic_number
        };
    }
    status = 0;

cleanup:
    buzz_spectrum_close(&spectrum);
    return status;
}

#include "instrument_isolation.h"
#include "dsp.h"
#include "internal.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ISOLATION_PITCH_HZ 2600.0
#define PITCH_CONTEXT_FRAMES 7U
#define PIANO_CONTEXT_PEAKS 4U
#define PIANO_MEASURED_PEAKS 12U

typedef struct IsolationProfile {
    double min_pitch_hz;
    double max_pitch_hz;
    double low_band_hz;
    double low_mask_start_hz;
    double low_mask_width_hz;
    double min_window_seconds;
    double auto_pitch_context_seconds;
    double rest_context_seconds;
    double min_rest_power_ratio;
    double short_low_note_hz;
    double tonal_release_off_fraction;
    unsigned window_overlap;
    int protect_marked_chords;
    int protect_marked_spectrum;
    int protect_low_notes;
    int detect_marked_rests;
    int pitched_wind;
    int low_wind;
    int recorder;
} IsolationProfile;

static const IsolationProfile isolation_profiles[] = {
    [HWA_ISOLATION_VIOLIN] = {
        .min_pitch_hz = 175.0, .low_band_hz = 145.0,
        .low_mask_start_hz = 105.0, .low_mask_width_hz = 75.0,
        .min_window_seconds = 0.0625, .window_overlap = 4U
    },
    /* Keep the low-frequency masks below each instrument's lowest notes. */
    [HWA_ISOLATION_VIOLA] = {
        .min_pitch_hz = 115.0, .low_band_hz = 100.0,
        .low_mask_start_hz = 65.0, .low_mask_width_hz = 50.0,
        .min_window_seconds = 0.0625, .window_overlap = 4U,
        .protect_marked_chords = 1
    },
    [HWA_ISOLATION_CELLO] = {
        .min_pitch_hz = 55.0, .low_band_hz = 45.0,
        .low_mask_start_hz = 28.0, .low_mask_width_hz = 25.0,
        .min_window_seconds = 0.125, .auto_pitch_context_seconds = 0.125,
        .window_overlap = 4U, .protect_marked_spectrum = 1,
        .protect_low_notes = 1
    },
    [HWA_ISOLATION_DOUBLE_BASS] = {
        /* Also protect the five-string bass's B0 at about 31 Hz. */
        .min_pitch_hz = 27.0, .low_band_hz = 22.0,
        .low_mask_start_hz = 17.0, .low_mask_width_hz = 10.0,
        .min_window_seconds = 0.250, .auto_pitch_context_seconds = 0.25,
        .rest_context_seconds = 0.8, .window_overlap = 8U,
        .protect_marked_spectrum = 1, .protect_low_notes = 1,
        .detect_marked_rests = 1
    },
    [HWA_ISOLATION_PIANO] = {
        .low_mask_width_hz = 1.0,
        .min_window_seconds = 0.125, .rest_context_seconds = 0.75,
        .window_overlap = 4U, .protect_marked_spectrum = 1,
        .detect_marked_rests = 1
    },
    [HWA_ISOLATION_FLUTE] = {
        /* Allow the B foot and keep the low mask clear of its lowest note. */
        .min_pitch_hz = 228.0, .max_pitch_hz = 3200.0, .low_band_hz = 190.0,
        .low_mask_start_hz = 105.0, .low_mask_width_hz = 75.0,
        .min_window_seconds = 0.040, .rest_context_seconds = 0.70, .min_rest_power_ratio = 6.0,
        .window_overlap = 8U, .protect_marked_spectrum = 1,
        .detect_marked_rests = 1, .pitched_wind = 1
    },
    [HWA_ISOLATION_OBOE] = {
        /* Leave room around the oboe's B-flat3 to A6 range. */
        .min_pitch_hz = 215.0, .max_pitch_hz = 1950.0, .low_band_hz = 180.0,
        .low_mask_start_hz = 100.0, .low_mask_width_hz = 70.0,
        .min_window_seconds = 0.040, .rest_context_seconds = 0.70, .min_rest_power_ratio = 4.5,
        .tonal_release_off_fraction = 0.05,
        .window_overlap = 8U, .protect_marked_spectrum = 1,
        .detect_marked_rests = 1, .pitched_wind = 1
    },
    [HWA_ISOLATION_ENGLISH_HORN] = {
        /* Use sounding pitches, with room below the usual low E3. */
        .min_pitch_hz = 145.0, .max_pitch_hz = 1180.0, .low_band_hz = 125.0,
        .low_mask_start_hz = 65.0, .low_mask_width_hz = 55.0,
        .min_window_seconds = 0.040, .rest_context_seconds = 0.70, .min_rest_power_ratio = 4.5,
        .short_low_note_hz = 225.0, .tonal_release_off_fraction = 0.05,
        .window_overlap = 8U, .protect_marked_spectrum = 1,
        .detect_marked_rests = 1, .pitched_wind = 1
    },
    [HWA_ISOLATION_CLARINET] = {
        /* Use the B-flat clarinet's sounding D3 to B-flat6 range. */
        .min_pitch_hz = 135.0, .max_pitch_hz = 2050.0, .low_band_hz = 115.0,
        .low_mask_start_hz = 60.0, .low_mask_width_hz = 50.0,
        .min_window_seconds = 0.040, .rest_context_seconds = 0.70, .min_rest_power_ratio = 4.5,
        .short_low_note_hz = 225.0, .tonal_release_off_fraction = 0.12,
        .window_overlap = 8U, .protect_marked_spectrum = 1,
        .detect_marked_rests = 1, .pitched_wind = 1
    },
    [HWA_ISOLATION_PICCOLO] = {
        /* Use sounding pitches and leave room for a low C extension. */
        .min_pitch_hz = 480.0, .max_pitch_hz = 4400.0, .low_band_hz = 400.0,
        .low_mask_start_hz = 220.0, .low_mask_width_hz = 180.0,
        .min_window_seconds = 0.025, .rest_context_seconds = 0.70, .min_rest_power_ratio = 6.0,
        .window_overlap = 8U, .protect_marked_spectrum = 1,
        .detect_marked_rests = 1, .pitched_wind = 1
    },
    [HWA_ISOLATION_BASSOON] = {
        /* Leave room around the sounding B-flat1 to F5 range. */
        .min_pitch_hz = 53.0, .max_pitch_hz = 740.0, .low_band_hz = 48.0,
        .low_mask_start_hz = 22.0, .low_mask_width_hz = 25.0,
        .min_window_seconds = 0.065, .rest_context_seconds = 0.70, .min_rest_power_ratio = 4.5,
        .short_low_note_hz = 180.0, .tonal_release_off_fraction = 0.12,
        .window_overlap = 8U, .protect_marked_spectrum = 1,
        .detect_marked_rests = 1, .pitched_wind = 1, .low_wind = 1
    },
    [HWA_ISOLATION_BASS_CLARINET] = {
        /* Use sounding pitches for a bass clarinet with a low C extension. */
        .min_pitch_hz = 53.0, .max_pitch_hz = 1080.0, .low_band_hz = 48.0,
        .low_mask_start_hz = 22.0, .low_mask_width_hz = 25.0,
        .min_window_seconds = 0.065, .rest_context_seconds = 0.70, .min_rest_power_ratio = 4.5,
        .short_low_note_hz = 180.0, .tonal_release_off_fraction = 0.12,
        .window_overlap = 8U, .protect_marked_spectrum = 1,
        .detect_marked_rests = 1, .pitched_wind = 1, .low_wind = 1
    },
    [HWA_ISOLATION_CONTRABASSOON] = {
        /* Keep the rumble mask below low A0 and track several cycles of the reed. */
        .min_pitch_hz = 24.5, .max_pitch_hz = 300.0, .low_band_hz = 21.0,
        .low_mask_start_hz = 11.0, .low_mask_width_hz = 10.0,
        .min_window_seconds = 0.135, .rest_context_seconds = 0.85, .min_rest_power_ratio = 4.5,
        .short_low_note_hz = 90.0, .tonal_release_off_fraction = 0.12,
        .window_overlap = 8U, .protect_marked_spectrum = 1,
        .detect_marked_rests = 1, .pitched_wind = 1, .low_wind = 1
    },
    [HWA_ISOLATION_E_FLAT_CLARINET] = {
        /* Leave room around the sounding G3 to C7 range. */
        .min_pitch_hz = 180.0, .max_pitch_hz = 2250.0, .low_band_hz = 165.0,
        .low_mask_start_hz = 90.0, .low_mask_width_hz = 60.0,
        .min_window_seconds = 0.025, .rest_context_seconds = 0.70, .min_rest_power_ratio = 4.5,
        .short_low_note_hz = 300.0, .tonal_release_off_fraction = 0.12,
        .window_overlap = 8U, .protect_marked_spectrum = 1,
        .detect_marked_rests = 1, .pitched_wind = 1
    },
    [HWA_ISOLATION_ALTO_FLUTE] = {
        /* Use sounding pitches, with room for a low B foot and the upper register. */
        .min_pitch_hz = 172.0, .max_pitch_hz = 1900.0, .low_band_hz = 150.0,
        .low_mask_start_hz = 85.0, .low_mask_width_hz = 60.0,
        .min_window_seconds = 0.025, .rest_context_seconds = 0.70, .min_rest_power_ratio = 6.0,
        .short_low_note_hz = 250.0, .tonal_release_off_fraction = 0.08,
        .window_overlap = 8U, .protect_marked_spectrum = 1,
        .detect_marked_rests = 1, .pitched_wind = 1
    },
    [HWA_ISOLATION_ALTO_SAXOPHONE] = {
        /* Leave room for bends around the sounding D-flat3 to A5 range. */
        .min_pitch_hz = 120.0, .max_pitch_hz = 1050.0, .low_band_hz = 100.0,
        .low_mask_start_hz = 55.0, .low_mask_width_hz = 45.0,
        .min_window_seconds = 0.030, .rest_context_seconds = 0.70, .min_rest_power_ratio = 4.5,
        .short_low_note_hz = 215.0, .tonal_release_off_fraction = 0.12,
        .window_overlap = 8U, .protect_marked_spectrum = 1,
        .detect_marked_rests = 1, .pitched_wind = 1
    },
    [HWA_ISOLATION_TENOR_SAXOPHONE] = {
        /* Leave room for bends around the sounding A-flat2 to E5 range. */
        .min_pitch_hz = 90.0, .max_pitch_hz = 790.0, .low_band_hz = 74.0,
        .low_mask_start_hz = 42.0, .low_mask_width_hz = 30.0,
        .min_window_seconds = 0.042, .rest_context_seconds = 0.70, .min_rest_power_ratio = 4.5,
        .short_low_note_hz = 180.0, .tonal_release_off_fraction = 0.12,
        .window_overlap = 8U, .protect_marked_spectrum = 1,
        .detect_marked_rests = 1, .pitched_wind = 1
    },
    [HWA_ISOLATION_RECORDER] = {
        /* Use sounding pitches and allow room for lower historical tuning. */
        .min_pitch_hz = 470.0, .max_pitch_hz = 2650.0, .low_band_hz = 420.0,
        .low_mask_start_hz = 220.0, .low_mask_width_hz = 180.0,
        .min_window_seconds = 0.0225, .rest_context_seconds = 0.70, .min_rest_power_ratio = 6.0,
        .tonal_release_off_fraction = 0.08,
        .window_overlap = 8U, .protect_marked_spectrum = 1,
        .detect_marked_rests = 1, .pitched_wind = 1, .recorder = 1
    },
    [HWA_ISOLATION_SOPRANINO_RECORDER] = {
        /* The sopranino sounds a fourth above the soprano. */
        .min_pitch_hz = 625.0, .max_pitch_hz = 3480.0, .low_band_hz = 560.0,
        .low_mask_start_hz = 275.0, .low_mask_width_hz = 250.0,
        .min_window_seconds = 0.0225, .rest_context_seconds = 0.70, .min_rest_power_ratio = 6.0,
        .tonal_release_off_fraction = 0.08,
        .window_overlap = 8U, .protect_marked_spectrum = 1,
        .detect_marked_rests = 1, .pitched_wind = 1, .recorder = 1
    }
};

typedef struct IsolationFrame {
    double power;
    double low_power;
    double pitch_hz;
    double confidence;
    double off_harmonic_power;
} IsolationFrame;

typedef struct PianoFrame {
    double tonal_power;
    double broad_power;
    double low_note_power;
    size_t peaks[PIANO_MEASURED_PEAKS];
    double peak_fractions[PIANO_MEASURED_PEAKS];
    int separated_partials;
} PianoFrame;

typedef struct IsolationWorkspace {
    const IsolationProfile *profile;
    const double *input;
    size_t frame_count;
    unsigned channels;
    unsigned frame_stride;
    unsigned sample_rate;
    size_t length;
    size_t hop;
    size_t padding;
    size_t grid_count;
    double bin_hz;
    double *window;
    double *power;
    double *piano_channel_power;
    double *normalizer;
    double *low_peaks;
    double *marked_background;
    HwaDspComplex *spectrum;
    HwaDspComplex *autocorrelation;
    IsolationFrame *frames;
    PianoFrame *piano_frames;
} IsolationWorkspace;

static double smooth_step(double value)
{
    value = fmax(0.0, fmin(1.0, value));
    return value * value * (3.0 - 2.0 * value);
}

static double harmonic_weight(size_t bin, double period_bins, double width_scale)
{
    double frequency_bin = (double)bin;
    double harmonic = floor(frequency_bin / period_bins + 0.5);
    double distance = fabs(frequency_bin - harmonic * period_bins);
    double width = fmin(0.44 * period_bins, width_scale * (2.4 + 0.014 * frequency_bin));
    if (harmonic < 1.0) return 0.0;
    return 1.0 - smooth_step((distance - 0.70 * width) / (0.30 * width));
}

static int spectral_frame(IsolationWorkspace *work, size_t start)
{
    size_t bins = work->length / 2U + 1U;
    unsigned channel;
    memset(work->power, 0, bins * sizeof(*work->power));
    for (channel = 0U; channel < work->channels; ++channel) {
        HwaDspComplex *spectrum = work->spectrum + work->length * channel;
        double *channel_power = work->piano_channel_power != NULL ?
            work->piano_channel_power + bins * channel : NULL;
        size_t index;
        for (index = 0U; index < work->length; ++index) {
            size_t padded_sample = start + index;
            double sample = 0.0;
            if (padded_sample >= work->padding &&
                padded_sample - work->padding < work->frame_count) {
                size_t frame = padded_sample - work->padding;
                sample = work->input[frame * work->frame_stride + channel];
            }
            spectrum[index].real = sample * work->window[index];
            spectrum[index].imag = 0.0;
        }
        if (hwa_dsp_fft(spectrum, work->length, 0) != HWA_DSP_OK) return -1;
        for (index = 0U; index < bins; ++index) {
            double power = spectrum[index].real * spectrum[index].real +
                           spectrum[index].imag * spectrum[index].imag;
            work->power[index] += power;
            if (channel_power != NULL) channel_power[index] = power;
        }
    }
    return 0;
}

static double narrow_peak_power(const IsolationWorkspace *work, double pitch_hz)
{
    size_t bin = (size_t)floor(pitch_hz / work->bin_hz + 0.5);
    double peak, shoulders;
    if (bin < 2U || bin + 2U > work->length / 2U) return 0.0;
    peak = fmax(work->power[bin], fmax(work->power[bin - 1U], work->power[bin + 1U]));
    shoulders = fmax(work->power[bin - 2U], work->power[bin + 2U]);
    return peak >= 4.0 * shoulders ? peak : 0.0;
}

static double clarinet_fundamental(const IsolationWorkspace *work, const IsolationFrame *frame)
{
    double candidate = frame->pitch_hz / 3.0;
    double peak;
    if (candidate < work->profile->min_pitch_hz) return frame->pitch_hz;

    /* A strong third harmonic can hide a weaker fundamental a twelfth below. */
    peak = narrow_peak_power(work, candidate);
    if (peak >= 0.006 * frame->power)
        return candidate;
    /* Use another odd partial to confirm a weak bass clarinet fundamental. */
    if (work->profile == &isolation_profiles[HWA_ISOLATION_BASS_CLARINET] &&
        peak >= 0.001 * frame->power &&
        narrow_peak_power(work, 5.0 * candidate) >= 0.00005 * frame->power)
        return candidate;
    return frame->pitch_hz;
}

static double saxophone_fundamental(const IsolationWorkspace *work, const IsolationFrame *frame)
{
    double candidate = 0.5 * frame->pitch_hz;
    int tenor = work->profile == &isolation_profiles[HWA_ISOLATION_TENOR_SAXOPHONE];
    double min_fundamental = (tenor ? 0.00025 : 0.0015) * frame->power;
    /* Confirm the weaker fundamental with the third partial or, for tenor, the fifth. */
    if (candidate >= work->profile->min_pitch_hz &&
        narrow_peak_power(work, candidate) >= min_fundamental &&
        (narrow_peak_power(work, 3.0 * candidate) >= 0.0005 * frame->power ||
         (tenor && narrow_peak_power(work, 5.0 * candidate) >= 0.0005 * frame->power)))
        return candidate;
    return frame->pitch_hz;
}

static int measure_narrow_wind_pitch(const IsolationWorkspace *work, IsolationFrame *frame, size_t first)
{
    size_t half = work->length / 2U;
    size_t last = (size_t)(work->profile->max_pitch_hz / work->bin_hz);
    size_t peak = first;
    size_t bin;
    double left, center, right, local, pitch_hz, adjustment = 0.0;
    double concentration = work->profile->recorder ? 0.55 : 0.65;

    if (last > half) last = half;
    if (first > last) return 0;
    for (bin = first + 1U; bin <= last; ++bin) {
        if (work->power[bin] > work->power[peak])
            peak = bin;
    }
    left = work->power[peak - 1U];
    center = work->power[peak];
    right = peak < half ? work->power[peak + 1U] : 0.0;
    local = left + center + right;
    if (center < left || center < right) return 0;
    if (peak == half) {
        /* The sampled amplitude of a tone this close to Nyquist depends on phase. */
        if (center < 0.15 * frame->power || local < 0.20 * frame->power) return 0;
    } else {
        double curvature = left - 2.0 * center + right;
        if (center < 0.30 * frame->power || local < concentration * frame->power) return 0;
        if (curvature < 0.0)
            adjustment = (left - right) / (2.0 * curvature);
    }
    pitch_hz = ((double)peak + adjustment) * work->bin_hz;
    /* A strong second partial can exceed the fundamental on some recorder notes. */
    if (work->profile->recorder && 0.5 * pitch_hz >= work->profile->min_pitch_hz &&
        narrow_peak_power(work, 0.5 * pitch_hz) >= 0.02 * frame->power)
        return 0;
    frame->pitch_hz = pitch_hz;
    frame->confidence = local / frame->power;
    /* An accepted recorder peak meets the confidence floor used by the wind masks. */
    if (work->profile->recorder) frame->confidence = fmax(frame->confidence, 0.65);
    return 1;
}

static int measure_pitch(IsolationWorkspace *work, IsolationFrame *frame,
                         double excluded_pitch_hz)
{
    HwaDspComplex *correlation = work->autocorrelation;
    double excluded_period = excluded_pitch_hz / work->bin_hz;
    size_t min_lag = (size_t)ceil((double)work->sample_rate / MAX_ISOLATION_PITCH_HZ);
    size_t max_lag = (size_t)floor((double)work->sample_rate / work->profile->min_pitch_hz);
    double best = 0.0;
    double zero_lag;
    size_t index;
    /* A concentrated spectral peak gives better pitch precision near Nyquist. */
    if (work->profile == &isolation_profiles[HWA_ISOLATION_PICCOLO] &&
        excluded_pitch_hz == 0.0 && measure_narrow_wind_pitch(work, frame, work->length / 4U))
        return 0;
    /* A recorder can keep a narrow peak through noise that obscures its period. */
    if (work->profile->recorder && excluded_pitch_hz == 0.0 &&
        measure_narrow_wind_pitch(work, frame, (size_t)(work->profile->min_pitch_hz / work->bin_hz)))
        return 0;
    /* At low rates the full window can favor a suboctave during a slur. */
    if (work->profile == &isolation_profiles[HWA_ISOLATION_ALTO_FLUTE] &&
        excluded_pitch_hz == 0.0 && measure_narrow_wind_pitch(work, frame, work->length / 8U))
        return 0;
    /* With fewer than six samples per cycle, the lag search can lock onto an octave below. */
    if (work->profile == &isolation_profiles[HWA_ISOLATION_E_FLAT_CLARINET] &&
        excluded_pitch_hz == 0.0 && measure_narrow_wind_pitch(work, frame, work->length / 6U)) {
        frame->pitch_hz = clarinet_fundamental(work, frame);
        return 0;
    }
    if (work->profile->max_pitch_hz > 0.0)
        min_lag = (size_t)((double)work->sample_rate / work->profile->max_pitch_hz);
    /* The sampling limit sets the shortest useful period. */
    if (min_lag < 2U) min_lag = 2U;
    /* Combine channel powers so opposite polarity does not cancel a note. */
    for (index = 0U; index < work->length; ++index) {
        size_t bin = index <= work->length / 2U ? index : work->length - index;
        double frequency = (double)bin * work->bin_hz;
        double power = frequency < work->profile->low_band_hz || frequency > 8000.0 ?
            0.0 : work->power[bin];
        if (excluded_period > 0.0 && power > 0.0) {
            power *= 1.0 - harmonic_weight(bin, excluded_period, 1.0);
        }
        correlation[index].real = power;
        correlation[index].imag = 0.0;
    }
    if (hwa_dsp_fft(correlation, work->length, 1) != HWA_DSP_OK) return -1;
    zero_lag = correlation[0].real;
    if (zero_lag <= 0.0) return 0;
    for (index = min_lag; index <= max_lag; ++index) {
        if (correlation[index].real >= correlation[index - 1U].real &&
            correlation[index].real >= correlation[index + 1U].real) {
            best = fmax(best, correlation[index].real);
        }
    }
    if (best < 0.55 * zero_lag) return 0;
    /* Prefer the earliest equally plausible period to avoid suboctave locks. */
    for (index = min_lag; index <= max_lag; ++index) {
        double left = correlation[index - 1U].real;
        double center = correlation[index].real;
        double right = correlation[index + 1U].real;
        if (center >= 0.94 * best && center >= left && center >= right) {
            double curvature = left - 2.0 * center + right;
            double adjustment = curvature < 0.0 ? (left - right) / (2.0 * curvature) : 0.0;
            frame->pitch_hz = (double)work->sample_rate / ((double)index + adjustment);
            frame->confidence = center / zero_lag;
            if (work->profile == &isolation_profiles[HWA_ISOLATION_CLARINET] ||
                work->profile == &isolation_profiles[HWA_ISOLATION_BASS_CLARINET] ||
                work->profile == &isolation_profiles[HWA_ISOLATION_E_FLAT_CLARINET])
                frame->pitch_hz = clarinet_fundamental(work, frame);
            else if (work->profile == &isolation_profiles[HWA_ISOLATION_ALTO_SAXOPHONE] ||
                     work->profile == &isolation_profiles[HWA_ISOLATION_TENOR_SAXOPHONE])
                frame->pitch_hz = saxophone_fundamental(work, frame);
            break;
        }
    }
    return 0;
}

static double off_harmonic_power(const IsolationWorkspace *work, double pitch_hz)
{
    double period_bins = pitch_hz / work->bin_hz;
    double power = 0.0;
    size_t bin;
    for (bin = 0U; bin <= work->length / 2U; ++bin) {
        power += (1.0 - harmonic_weight(bin, period_bins, 1.0)) * work->power[bin];
    }
    return power;
}

static double spectral_broadband_weight(const double *power, size_t half, size_t bin)
{
    double left, right, floor;
    if (bin < 4U || bin + 4U > half || power[bin] == 0.0) return 0.0;
    left = fmin(power[bin - 3U], power[bin - 4U]);
    right = fmin(power[bin + 3U], power[bin + 4U]);
    floor = fmin(left, right);
    return smooth_step((floor / power[bin] - 0.04) / 0.25);
}

static double spectral_broadband_power(const double *power, size_t half)
{
    double broadband = 0.0;
    size_t bin;
    for (bin = 4U; bin + 4U <= half; ++bin)
        broadband += power[bin] * spectral_broadband_weight(power, half, bin);
    return broadband;
}

static int has_piano_partial_gap(const double *power, size_t half, double total)
{
    double below = power[0] + power[1];
    double min_side = 0.23 * total;
    double max_gap = 0.004 * total;
    size_t bin;
    /* Even overlapping partials can leave a gap between two bands of energy. */
    for (bin = 2U; bin + 2U <= half; ++bin) {
        below += power[bin];
        if (below < min_side) continue;
        if (total - below < min_side) break;
        if (power[bin - 2U] + power[bin - 1U] + power[bin] +
            power[bin + 1U] + power[bin + 2U] < max_gap)
            return 1;
    }
    return 0;
}

static int has_piano_partials_in_channels(const IsolationWorkspace *work, double total)
{
    size_t half = work->length / 2U;
    unsigned channel;
    if (!has_piano_partial_gap(work->power, half, total)) return 0;
    if (work->piano_channel_power == NULL) return 1;
    /* A gap between a step in one channel and a note in the other is not a chord. */
    for (channel = 0U; channel < work->channels; ++channel) {
        const double *power = work->piano_channel_power + (half + 1U) * channel;
        double channel_total = 0.0;
        size_t bin;
        for (bin = 0U; bin <= half; ++bin) channel_total += power[bin];
        if (channel_total > 0.10 * total && !has_piano_partial_gap(power, half, channel_total))
            return 0;
    }
    return 1;
}

static void measure_piano_spectrum(const IsolationWorkspace *work, size_t index)
{
    PianoFrame *piano = &work->piano_frames[index];
    double minimum_peak = 0.012 * work->frames[index].power;
    size_t half = work->length / 2U;
    size_t covered = 0U;
    size_t bin;

    /* Close bass notes can merge in the FFT, even without interference. */
    for (bin = (size_t)ceil(20.0 / work->bin_hz);
         (double)bin * work->bin_hz <= 100.0; ++bin)
        piano->low_note_power += work->power[bin];
    for (bin = 3U; bin + 3U <= half; ++bin) {
        double power = work->power[bin];
        double frequency = (double)bin * work->bin_hz;
        size_t shoulder = frequency < 100.0 ? 2U : 3U;
        double shoulder_fraction = shoulder == 2U ? 0.30 : 0.13;
        size_t peak, near;
        /* Held-chord matching still needs individually resolved partials. */
        if (power < minimum_peak || frequency < 20.0 ||
            power <= work->power[bin - 1U] || power < work->power[bin + 1U] ||
            fmax(work->power[bin - shoulder], work->power[bin + shoulder]) >
                shoulder_fraction * power)
            continue;
        for (near = bin - 2U > covered ? bin - 2U : covered; near <= bin + 2U; ++near)
            piano->tonal_power += work->power[near];
        covered = near;
        for (peak = 0U; peak < PIANO_MEASURED_PEAKS; ++peak) {
            size_t prior = piano->peaks[peak];
            if (prior == 0U || power > work->power[prior]) {
                memmove(&piano->peaks[peak + 1U], &piano->peaks[peak],
                        (PIANO_MEASURED_PEAKS - peak - 1U) * sizeof(*piano->peaks));
                memmove(&piano->peak_fractions[peak + 1U], &piano->peak_fractions[peak],
                        (PIANO_MEASURED_PEAKS - peak - 1U) * sizeof(*piano->peak_fractions));
                piano->peaks[peak] = bin;
                piano->peak_fractions[peak] = power / work->frames[index].power;
                break;
            }
        }
    }
    piano->broad_power = spectral_broadband_power(work->power, half);
    piano->separated_partials = has_piano_partials_in_channels(work, work->frames[index].power);
}

static int frame_intersects_ranges(const IsolationWorkspace *work,
                                   size_t start, size_t margin,
                                   const HwaIsolationRange *ranges,
                                   size_t range_count, size_t *range)
{
    if (range_count == 0U) return 1;
    while (*range < range_count &&
           start >= ranges[*range].end_sample + work->padding + margin) {
        ++*range;
    }
    return *range < range_count && start + work->length + margin >
        ranges[*range].start_sample + work->padding;
}

static int measure_frames(IsolationWorkspace *work,
                          const HwaIsolationRange *ranges, size_t range_count)
{
    size_t range = 0U;
    size_t index;
    /* Marked cleanup needs pitch only in overlapping windows and their neighbors. */
    size_t margin = PITCH_CONTEXT_FRAMES * work->hop;
    if (range_count != 0U && work->profile->detect_marked_rests) {
        size_t rest_margin = (size_t)(work->profile->rest_context_seconds * work->sample_rate);
        if (rest_margin > margin) margin = rest_margin;
    }
    for (index = 0U; index < work->grid_count; ++index) {
        IsolationFrame *frame = &work->frames[index];
        size_t start = index * work->hop;
        size_t bin;
        if (!frame_intersects_ranges(work, start, margin, ranges, range_count, &range))
            continue;
        if (spectral_frame(work, start) != 0) return -1;
        for (bin = 0U; bin <= work->length / 2U; ++bin) {
            frame->power += work->power[bin];
            if ((double)bin * work->bin_hz <= work->profile->low_band_hz) {
                frame->low_power += work->power[bin];
            }
        }
        if (frame->power == 0.0) continue;
        if (work->piano_frames != NULL) {
            measure_piano_spectrum(work, index);
            continue;
        }
        if (work->low_peaks != NULL) {
            size_t first_low_bin = (size_t)ceil(work->profile->min_pitch_hz / work->bin_hz);
            size_t last_low_bin = (size_t)(250.0 / work->bin_hz);
            double peak_power = 0.05 * frame->power;
            /* Low partials can stay stable even when bow noise hides a pitch. */
            for (bin = first_low_bin; bin <= last_low_bin; ++bin) {
                if (work->power[bin] > peak_power &&
                    work->power[bin] > work->power[bin - 1U] &&
                    work->power[bin] >= work->power[bin + 1U]) {
                    work->low_peaks[index] = (double)bin * work->bin_hz;
                    peak_power = work->power[bin];
                }
            }
        }
        if (measure_pitch(work, frame, 0.0) != 0) return -1;
        if (frame->pitch_hz > 0.0) {
            frame->off_harmonic_power = off_harmonic_power(work, frame->pitch_hz);
        }
    }
    return 0;
}

static int likely_instrument_pitch(const IsolationFrame *frame)
{
    return frame->confidence >= 0.67 &&
           frame->off_harmonic_power <= 0.20 * frame->power;
}

static int pitches_agree(double first, double second)
{
    return first > 0.0 && second > 0.0 &&
           fmax(first, second) <= 1.06 * fmin(first, second);
}

static int wind_frame_has_note(const IsolationFrame *frame)
{
    return frame->confidence >= 0.65 && frame->off_harmonic_power <= 0.48 * frame->power;
}

static int wind_pitch_persists(const IsolationFrame *left, const IsolationFrame *right)
{
    return wind_frame_has_note(left) && wind_frame_has_note(right) &&
           fmax(left->pitch_hz, right->pitch_hz) <= 1.028 * fmin(left->pitch_hz, right->pitch_hz);
}

static int wind_short_pitch_pair(const IsolationFrame *left, const IsolationFrame *right,
                                 double minimum_power)
{
    return left->power >= minimum_power && right->power >= minimum_power &&
           left->confidence >= 0.55 && right->confidence >= 0.55 &&
           left->off_harmonic_power <= 0.10 * left->power &&
           right->off_harmonic_power <= 0.10 * right->power &&
           pitches_agree(left->pitch_hz, right->pitch_hz);
}

static int wind_overlaps_note(const IsolationWorkspace *work, size_t index)
{
    size_t overlap = work->profile->window_overlap - 1U;
    size_t first = index > overlap ? index - overlap : 0U;
    size_t last = index + overlap < work->grid_count ? index + overlap : work->grid_count - 1U;
    size_t separation = work->profile->window_overlap / 2U;
    size_t breath_span = (size_t)work->sample_rate / (8U * work->hop);
    double power = work->frames[index].power;
    size_t i;
    /* Protect the air and tongue around a pitch that persists beyond an impact. */
    for (i = first; i + separation <= last; ++i) {
        if (wind_pitch_persists(&work->frames[i], &work->frames[i + separation]))
            return 1;
    }
    /* Two clean neighboring frames can reveal a note shorter than the usual
     * pitch-persistence interval. Long windows can depress its confidence. */
    for (i = first + 1U; i <= last; ++i) {
        const IsolationFrame *right = &work->frames[i];
        const IsolationFrame *left = right - 1;
        if (wind_short_pitch_pair(left, right, 0.10 * power))
            return 1;
    }
    /* The highest sampled tone can emerge slowly from its breath near Nyquist. */
    if (work->profile == &isolation_profiles[HWA_ISOLATION_PICCOLO] &&
        0.5 * (double)work->sample_rate <= work->profile->max_pitch_hz) {
        size_t nyquist_span = 2U * work->profile->window_overlap - 1U;
        size_t near_first = index > nyquist_span ? index - nyquist_span : 0U;
        size_t near_last = index + nyquist_span < work->grid_count ?
            index + nyquist_span : work->grid_count - 1U;
        double nyquist_hz = 0.5 * (double)work->sample_rate;
        for (i = near_first; i + separation <= near_last; ++i) {
            const IsolationFrame *left = &work->frames[i];
            const IsolationFrame *right = left + separation;
            if (left->pitch_hz == nyquist_hz && right->pitch_hz == nyquist_hz)
                return 1;
        }
    }
    /* One window can span several fast changes in a wind phrase. */
    if (work->profile->low_wind || work->profile->recorder ||
        work->profile == &isolation_profiles[HWA_ISOLATION_ALTO_FLUTE] ||
        work->profile == &isolation_profiles[HWA_ISOLATION_ALTO_SAXOPHONE] ||
        work->profile == &isolation_profiles[HWA_ISOLATION_TENOR_SAXOPHONE]) {
        int contrabassoon = work->profile == &isolation_profiles[HWA_ISOLATION_CONTRABASSOON];
        unsigned supporting = 0U;
        size_t first_tonal = first;
        for (i = first; i <= last; ++i) {
            const IsolationFrame *frame = &work->frames[i];
            if (frame->confidence < 0.55 || frame->power < 0.10 * power ||
                frame->off_harmonic_power > 0.18 * frame->power)
                continue;
            /* Short low contrabassoon notes may fill only part of a window. */
            if (contrabassoon && i > first && frame->pitch_hz <= work->profile->short_low_note_hz &&
                frame->off_harmonic_power <= 0.06 * frame->power) {
                const IsolationFrame *previous = frame - 1;
                if (previous->confidence >= 0.55 && previous->power >= 0.10 * power &&
                    previous->off_harmonic_power <= 0.06 * previous->power &&
                    pitches_agree(previous->pitch_hz, frame->pitch_hz))
                    return 1;
            }
            if (supporting++ == 0U) first_tonal = i;
            if (supporting >= 3U && i - first_tonal >= separation) return 1;
        }
    }
    /* A short wind note may have reliable pitch in only a few windows. */
    if (work->profile->tonal_release_off_fraction > 0.0) {
        size_t short_separation = separation / 2U;
        const double max_off_fraction = 0.08;
        for (i = first; i + short_separation <= last; ++i) {
            const IsolationFrame *left = &work->frames[i];
            const IsolationFrame *middle = left + short_separation / 2U;
            const IsolationFrame *right = left + short_separation;
            if (!wind_pitch_persists(left, right)) continue;
            if (left->pitch_hz <= work->profile->short_low_note_hz &&
                right->pitch_hz <= work->profile->short_low_note_hz &&
                left->off_harmonic_power <= 0.05 * left->power &&
                right->off_harmonic_power <= 0.05 * right->power)
                return 1;
            /* Require an energy peak to avoid protecting the decay of an impact. */
            if (wind_pitch_persists(left, middle) &&
                middle->power >= fmax(left->power, right->power) &&
                left->off_harmonic_power <= max_off_fraction * left->power &&
                middle->off_harmonic_power <= max_off_fraction * middle->power &&
                right->off_harmonic_power <= max_off_fraction * right->power)
                return 1;
        }
    }
    /* A stronger note can take longer to emerge from its breath. */
    for (i = 0U; i < 2U; ++i) {
        size_t available = i == 0U ? index : work->grid_count - index - 1U;
        size_t limit = available < breath_span ? available : breath_span;
        size_t step;
        for (step = 1U; step + separation <= limit; ++step) {
            size_t nearest = i == 0U ? index - step : index + step;
            const IsolationFrame *near = &work->frames[nearest];
            const IsolationFrame *far = i == 0U ? near - separation : near + separation;
            if (near->power <= 0.06 * power) break;
            if (near->power >= 4.0 * power && far->power >= 4.0 * power &&
                wind_pitch_persists(near, far))
                return 1;
        }
    }
    return 0;
}

static double wind_breath_floor(const IsolationWorkspace *work, size_t index, int forward)
{
    const IsolationFrame *current = &work->frames[index];
    size_t available = forward ? work->grid_count - index - 1U : index;
    size_t limit = (size_t)work->sample_rate / (3U * work->hop);
    size_t noisy_octave_limit = work->profile->recorder ?
        (size_t)work->sample_rate / (8U * work->hop) : 0U;
    size_t noisy_octave_windows = 0U;
    double floor = 1.0;
    size_t step;
    if (limit > available) limit = available;
    /* Use a stable note on both sides without measuring a different note or a rest. */
    for (step = work->profile->window_overlap; step <= limit; ++step) {
        const IsolationFrame *frame = &work->frames[forward ? index + step : index - step];
        double off;
        if (frame->power < 0.08 * current->power) break;
        if (!wind_frame_has_note(frame)) continue;
        if (!pitches_agree(current->pitch_hz, frame->pitch_hz)) {
            /* A cough can briefly pull the pitch estimate down an octave. */
            if (noisy_octave_windows < noisy_octave_limit &&
                frame->off_harmonic_power > 0.08 * frame->power &&
                pitches_agree(current->pitch_hz, 2.0 * frame->pitch_hz)) {
                ++noisy_octave_windows;
                continue;
            }
            break;
        }
        /* Overlapping notes in a long window can shift the pitch estimate. */
        if (work->profile->low_wind && fmax(current->pitch_hz, frame->pitch_hz) >
                                      1.035 * fmin(current->pitch_hz, frame->pitch_hz))
            break;
        /* Compare breath power against the softer frame when note level changes. */
        off = frame->off_harmonic_power / fmin(frame->power, current->power);
        if (off < floor) floor = off;
    }
    return floor;
}

static int wind_pitch_changes_near_frame(const IsolationWorkspace *work, size_t index)
{
    size_t overlap = work->profile->window_overlap - 1U;
    size_t first = index > overlap ? index - overlap : 0U;
    size_t last = index + overlap < work->grid_count ? index + overlap : work->grid_count - 1U;
    double pitch = work->frames[index].pitch_hz;
    double low_limit = pitch / 1.10;
    double high_limit = 1.10 * pitch;
    double lowest = high_limit;
    double highest = 0.0;
    int recorder = work->profile->recorder;
    int tenor = work->profile == &isolation_profiles[HWA_ISOLATION_TENOR_SAXOPHONE];
    int bass_clarinet = work->profile == &isolation_profiles[HWA_ISOLATION_BASS_CLARINET];
    int protect_octave = tenor || recorder ||
        work->profile == &isolation_profiles[HWA_ISOLATION_E_FLAT_CLARINET] ||
        work->profile == &isolation_profiles[HWA_ISOLATION_ALTO_FLUTE] ||
        work->profile == &isolation_profiles[HWA_ISOLATION_ALTO_SAXOPHONE];
    size_t i;

    /* Clean partials from a nearby note can spread beyond the current harmonic mask. */
    for (i = first; i <= last; ++i) {
        const IsolationFrame *frame = &work->frames[i];
        const IsolationFrame *previous = i > first ? frame - 1 : NULL;
        int clean_pitch_pair = protect_octave && previous != NULL &&
            frame->off_harmonic_power <= 0.035 * frame->power &&
            previous->off_harmonic_power <= 0.035 * previous->power &&
            wind_pitch_persists(previous, frame);
        /* Pitch slides into the higher note also spread across harmonic bins. */
        if (protect_octave && (frame->confidence >= 0.85 || clean_pitch_pair) &&
            frame->power >= 0.20 * work->frames[index].power &&
            frame->off_harmonic_power <= 0.08 * frame->power &&
            pitches_agree(frame->pitch_hz, 2.0 * pitch))
            return 1;
        /* Higher harmonic masks can cut partials of a nearby lower note. */
        int lower_octave = protect_octave && pitches_agree(2.0 * frame->pitch_hz, pitch);
        int lower_twelfth = bass_clarinet && pitches_agree(3.0 * frame->pitch_hz, pitch);
        if (previous != NULL && (lower_octave || lower_twelfth)) {
            /* Fewer periods fit in a window at the bottom of the tenor range. */
            if (frame->power >= 0.20 * work->frames[index].power &&
                frame->off_harmonic_power <= 0.08 * frame->power &&
                previous->off_harmonic_power <= 0.08 * previous->power &&
                (lower_twelfth || (tenor && frame->pitch_hz <= 120.0) || clean_pitch_pair ||
                 (previous->confidence >= 0.85 && frame->confidence >= 0.85)) &&
                wind_pitch_persists(previous, frame))
                return 1;
        }
        if (frame->confidence < 0.75 ||
            frame->off_harmonic_power > (recorder ? 0.08 : 0.04) * frame->power ||
            frame->pitch_hz < low_limit || frame->pitch_hz > high_limit)
            continue;
        lowest = fmin(lowest, frame->pitch_hz);
        highest = fmax(highest, frame->pitch_hz);
    }
    return highest > (recorder ? 1.04 : 1.05) * lowest;
}

static int wind_clip_edge_holds(const IsolationWorkspace *work, const IsolationFrame *frame,
                               int end)
{
    size_t center = (end ? work->frame_count : 0U) + work->padding - work->length / 2U;
    const IsolationFrame *edge = &work->frames[(center + work->hop / 2U) / work->hop];
    return edge->power >= 0.15 * frame->power &&
           pitches_agree(frame->pitch_hz, edge->pitch_hz);
}

static void wind_gain(const IsolationWorkspace *work, size_t index, int marked,
                      const HwaIsolationRange *active_range,
                      double *pitch_hz, double *strength, double *low_strength)
{
    const IsolationFrame *frame = &work->frames[index];
    int short_lead = active_range != NULL && active_range->start_sample < work->length;
    int short_tail = active_range != NULL &&
        work->frame_count - active_range->end_sample < work->length;
    double left_floor, right_floor;
    double floor, excess;
    *pitch_hz = frame->pitch_hz;
    *strength = 0.0;
    *low_strength = 0.76 * smooth_step((frame->low_power / frame->power - 0.05) / 0.23);
    if (!wind_frame_has_note(frame)) return;
    /* Use one clean flank only when the held pitch already reaches the clip edge. */
    left_floor = short_lead && !short_tail && wind_clip_edge_holds(work, frame, 0) ? 0.0 :
        wind_breath_floor(work, index, 0);
    right_floor = short_tail && !short_lead && wind_clip_edge_holds(work, frame, 1) ? 0.0 :
        wind_breath_floor(work, index, 1);
    floor = fmax(0.002, fmax(left_floor, right_floor));
    excess = frame->off_harmonic_power / frame->power - (marked ? 1.70 : 2.25) * floor;
    *strength = (marked ? 0.80 : 0.74) * smooth_step((excess - 0.008) / 0.045);
    if (*strength > 0.0 && wind_pitch_changes_near_frame(work, index)) *strength = 0.0;
}

static int piano_frame_is_tonal(const IsolationWorkspace *work, size_t index)
{
    return work->piano_frames[index].tonal_power > 0.30 * work->frames[index].power;
}

static int piano_peaks_match(size_t first, size_t second)
{
    return first > second ? first - second <= 1U : second - first <= 1U;
}

static double piano_peak_fraction(const PianoFrame *frame, size_t bin)
{
    size_t index;
    for (index = 0U; index < PIANO_MEASURED_PEAKS && frame->peaks[index] != 0U; ++index) {
        if (piano_peaks_match(frame->peaks[index], bin)) return frame->peak_fractions[index];
    }
    return 0.0;
}

static size_t piano_order_peaks(const PianoFrame *frame, size_t *ordered)
{
    size_t count;
    for (count = 0U; count < PIANO_CONTEXT_PEAKS && frame->peaks[count] != 0U; ++count) {
        size_t bin = frame->peaks[count];
        size_t index = count;
        while (index > 0U && ordered[index - 1U] > bin) {
            ordered[index] = ordered[index - 1U];
            --index;
        }
        ordered[index] = bin;
    }
    return count;
}

static int piano_chords_agree(const PianoFrame *first, const PianoFrame *second)
{
    size_t first_bins[PIANO_CONTEXT_PEAKS], second_bins[PIANO_CONTEXT_PEAKS];
    size_t first_count = piano_order_peaks(first, first_bins);
    size_t second_count = piano_order_peaks(second, second_bins);
    size_t left = 0U, right = 0U, matches = 0U;
    /* Match in frequency order so one peak cannot account for two notes. */
    while (left < first_count && right < second_count) {
        if (piano_peaks_match(first_bins[left], second_bins[right])) {
            ++left;
            ++right;
            if (++matches == 2U) return 1;
        } else if (first_bins[left] < second_bins[right]) {
            ++left;
        } else {
            ++right;
        }
    }
    return matches == 1U && first_count == 1U && second_count == 1U;
}

static int piano_frame_has_note(const IsolationWorkspace *work, size_t index)
{
    const PianoFrame *current = &work->piano_frames[index];
    double power = work->frames[index].power;
    if (current->separated_partials || current->low_note_power > 0.45 * power) return 1;
    if (!piano_frame_is_tonal(work, index)) return 0;
    if (current->tonal_power > 0.86 * power) return 1;
    /* A footstep may ring briefly; check that the partials persist. */
    return (index > 0U && piano_frame_is_tonal(work, index - 1U) &&
            piano_chords_agree(current, current - 1U)) ||
           (index + 1U < work->grid_count && piano_frame_is_tonal(work, index + 1U) &&
            piano_chords_agree(current, current + 1U));
}

static const PianoFrame *piano_chord_context(const IsolationWorkspace *work,
                                             size_t index, int forward, int marked)
{
    const PianoFrame *current = &work->piano_frames[index];
    size_t available = forward ? work->grid_count - index - 1U : index;
    size_t limit = (size_t)work->sample_rate / (2U * work->hop);
    size_t step;
    if (limit > available) limit = available;
    /* Skip overlapping windows so the interference cannot set its own floor. */
    for (step = work->profile->window_overlap; step <= limit; ++step) {
        size_t other = forward ? index + step : index - step;
        const PianoFrame *neighbor = &work->piano_frames[other];
        size_t start = other * work->hop;
        if (marked && (start < work->padding ||
                       start + work->length > work->padding + work->frame_count))
            break;
        if (work->frames[other].power < 0.03 * work->frames[index].power)
            break;
        if (piano_frame_is_tonal(work, other) && piano_chords_agree(current, neighbor))
            return neighbor;
    }
    return NULL;
}

static int piano_has_new_peak(const PianoFrame *current, const PianoFrame *before,
                              const PianoFrame *after)
{
    size_t peak;
    for (peak = 0U; peak < PIANO_MEASURED_PEAKS && current->peaks[peak] != 0U; ++peak) {
        size_t bin = current->peaks[peak];
        double fraction = current->peak_fractions[peak];
        double prior;
        if (fraction <= 0.035) break;
        prior = piano_peak_fraction(before, bin);
        if (fraction > 2.5 * prior &&
            (after == NULL || piano_peak_fraction(after, bin) > fmax(0.018, 1.5 * prior)))
            return 1;
    }
    return 0;
}

static double piano_chord_gain(const IsolationWorkspace *work, size_t index, int marked)
{
    const PianoFrame *current = &work->piano_frames[index];
    const PianoFrame *left, *right;
    double power = work->frames[index].power;
    double baseline, excess;
    size_t step;
    if (current->broad_power <= 0.0003 * power || current->peaks[0] == 0U)
        return 0.0;
    left = piano_chord_context(work, index, 0, marked);
    right = piano_chord_context(work, index, 1, marked);
    if (left == NULL || right == NULL) {
        size_t start = index * work->hop;
        if (!marked) return 0.0;
        /* At a recording edge a marked disturbance may only have one full reference. */
        if (left == NULL && right != NULL && start < work->padding + work->length)
            left = right;
        else if (right == NULL && left != NULL &&
                 start + 2U * work->length > work->padding + work->frame_count)
            right = left;
        else
            return 0.0;
    } else if (!piano_chords_agree(left, right)) {
        return 0.0;
    }
    /* A reattack can retain the same pitches and still add musical broadband sound. */
    if (current->tonal_power > 1.40 * fmax(left->tonal_power, right->tonal_power))
        return 0.0;
    /* The hammer can reach a window before its new pitch is clear. */
    if (piano_has_new_peak(current, left, NULL)) return 0.0;
    for (step = 1U; step <= 2U && index + step < work->grid_count; ++step) {
        if (piano_frame_is_tonal(work, index + step) &&
            piano_has_new_peak(current + step, left, right))
            return 0.0;
    }
    baseline = fmax(left->broad_power, right->broad_power);
    excess = fmax(0.0, current->broad_power - (marked ? 1.35 : 2.0) * baseline);
    return (marked ? 0.82 : 0.66) *
        smooth_step((excess / power - 0.0002) / (marked ? 0.009 : 0.016)) *
        smooth_step((excess / current->broad_power - 0.20) / 0.60);
}

static const IsolationFrame *nearby_pitch(const IsolationWorkspace *work,
                                          size_t index, int forward, int marked)
{
    int local_context = work->profile->protect_low_notes && !marked;
    size_t limit = local_context ? (size_t)(work->profile->auto_pitch_context_seconds *
                                            work->sample_rate / (double)work->hop) :
                                   PITCH_CONTEXT_FRAMES;
    size_t step;
    for (step = 1U; step <= limit; ++step) {
        const IsolationFrame *other;
        if (forward ? step >= work->grid_count - index : step > index) break;
        other = &work->frames[forward ? index + step : index - step];
        if (local_context && other->power <= 0.06 * work->frames[index].power)
            break;
        if (likely_instrument_pitch(other)) return other;
    }
    return NULL;
}

static double quiet_interference_gain(const IsolationWorkspace *work,
                                      size_t index, double pitch_hz,
                                      double off_harmonic_fraction)
{
    size_t context = (size_t)work->sample_rate * 2U / 5U / work->hop;
    size_t direction;
    double baseline[2] = {1.0, 1.0};
    double background;
    if (off_harmonic_fraction <= 0.001 || off_harmonic_fraction >= 0.10) {
        return 0.0;
    }
    /* Find quiet windows on both sides with the same pitch. Skip windows
     * that overlap this one so a short disturbance cannot set its own floor. */
    for (direction = 0U; direction < 2U; ++direction) {
        size_t step;
        for (step = work->length / work->hop; step <= context; ++step) {
            const IsolationFrame *neighbor;
            if (direction == 0U ? step > index : step >= work->grid_count - index) {
                break;
            }
            neighbor = &work->frames[direction == 0U ? index - step : index + step];
            if (likely_instrument_pitch(neighbor) && pitches_agree(pitch_hz, neighbor->pitch_hz)) {
                baseline[direction] = fmin(baseline[direction],
                                           neighbor->off_harmonic_power / neighbor->power);
            }
        }
    }
    background = fmax(0.0003, fmax(baseline[0], baseline[1]));
    return 0.60 * smooth_step((off_harmonic_fraction / background - 3.0) / 7.0) *
           smooth_step((off_harmonic_fraction - 0.001) / 0.004) *
           smooth_step((0.10 - off_harmonic_fraction) / 0.04);
}

static int has_competing_low_peak(const IsolationWorkspace *work,
                                  double pitch_hz, double frame_power)
{
    size_t first = (size_t)ceil(work->profile->min_pitch_hz / work->bin_hz);
    size_t last = (size_t)(250.0 / work->bin_hz);
    double minimum_power = 0.035 * frame_power;
    size_t bin;

    /* A short note can leave a secondary peak within the longer FFT window. */
    for (bin = first; bin <= last; ++bin) {
        double frequency, harmonic;
        if (work->power[bin] <= minimum_power ||
            work->power[bin] <= work->power[bin - 1U] ||
            work->power[bin] < work->power[bin + 1U])
            continue;
        frequency = (double)bin * work->bin_hz;
        harmonic = floor(frequency / pitch_hz + 0.5);
        if (harmonic < 1.0 || fabs(frequency - harmonic * pitch_hz) > 1.5 * work->bin_hz)
            return 1;
    }
    return 0;
}

/* A glide can spread its harmonics across several bins in one long window. */
static int overlaps_moving_pitch(const IsolationWorkspace *work, size_t index)
{
    size_t overlap = work->length / work->hop - 1U;
    size_t first = index > overlap ? index - overlap : 0U;
    size_t last = index + overlap < work->grid_count ? index + overlap : work->grid_count - 1U;
    double minimum_power = 0.12 * work->frames[index].power;
    double first_pitch = 0.0;
    size_t i;

    for (i = first; i <= last; ++i) {
        const IsolationFrame *frame = &work->frames[i];
        if (frame->power < minimum_power || frame->confidence < 0.60 ||
            frame->off_harmonic_power > 0.75 * frame->power)
            continue;
        if (first_pitch == 0.0) first_pitch = frame->pitch_hz;
        else if (!pitches_agree(first_pitch, frame->pitch_hz)) return 1;
    }
    return 0;
}

static void isolation_gain(const IsolationWorkspace *work, size_t index,
                           int marked, double *pitch_hz, double *strength,
                           double *low_strength)
{
    const IsolationFrame *frame = &work->frames[index];
    const IsolationFrame *left = nearby_pitch(work, index, 0, marked);
    const IsolationFrame *right = nearby_pitch(work, index, 1, marked);
    double matched_pitch = 0.0;
    double surrounding_off_harmonic_power = 0.0;
    *pitch_hz = frame->pitch_hz;
    *strength = 0.0;
    *low_strength = 0.0;
    if (frame->power == 0.0) return;

    if (left != NULL && right != NULL && pitches_agree(left->pitch_hz, right->pitch_hz)) {
        matched_pitch = 0.5 * (left->pitch_hz + right->pitch_hz);
        surrounding_off_harmonic_power =
            fmax(left->off_harmonic_power, right->off_harmonic_power);
    }
    if (marked) {
        if (work->profile->protect_marked_spectrum) {
            *pitch_hz = likely_instrument_pitch(frame) ? frame->pitch_hz : 0.0;
            /* A pitch that emerges after this window can be a bow attack. */
            *strength = left == NULL && right != NULL ? 0.10 : 0.82;
            /* A low partial alone still warrants care with broad bow noise. */
            if (*pitch_hz == 0.0 && work->low_peaks != NULL && work->low_peaks[index] > 0.0)
                *strength = fmin(*strength, 0.52);
            *low_strength = 0.88;
            if (work->profile->window_overlap > 4U) {
                if (overlaps_moving_pitch(work, index)) *strength = fmin(*strength, 0.075);
                if (*pitch_hz == 0.0 && left == NULL && right == NULL) {
                    /* Short notes can spread some power below the bass range. */
                    double low_fraction = frame->low_power / frame->power;
                    *low_strength *= smooth_step((low_fraction - 0.10) / 0.25);
                }
            }
            return;
        }
        if (matched_pitch > 0.0 && !likely_instrument_pitch(frame) &&
            !pitches_agree(*pitch_hz, matched_pitch)) {
            *pitch_hz = matched_pitch;
        }
        /* A marked unpitched disturbance can be reduced across its spectrum. */
        *strength = *pitch_hz > 0.0 ? 0.82 : 0.70;
        return;
    }

    /* Strong low-frequency events can sit between notes as well as over them. */
    *low_strength = 0.88 * smooth_step((frame->low_power / frame->power - 0.10) / 0.25);
    /* Unpitched low energy can be part of an attack. */
    if (work->profile->protect_low_notes && matched_pitch == 0.0 &&
        !likely_instrument_pitch(frame))
        *low_strength = 0.0;
    if (matched_pitch == 0.0 ||
        (likely_instrument_pitch(frame) && !pitches_agree(*pitch_hz, matched_pitch))) {
        return;
    }
    if (work->profile == &isolation_profiles[HWA_ISOLATION_DOUBLE_BASS] &&
        has_competing_low_peak(work, matched_pitch, frame->power))
        return;
    if (!pitches_agree(*pitch_hz, matched_pitch)) *pitch_hz = matched_pitch;
    {
        double off_power = off_harmonic_power(work, *pitch_hz);
        double excess = (off_power - 2.0 * surrounding_off_harmonic_power) / frame->power;
        /* Seek a brief rise outside a stable pitch, not a change of note. */
        *strength = fmax(0.70 * smooth_step((excess - 0.035) / 0.19),
                        quiet_interference_gain(work, index, *pitch_hz,
                                                off_power / frame->power));
    }
}

/* Find two quiet windows on one side without crossing comparable pitched sound. */
static size_t find_quiet_shoulder(const IsolationWorkspace *work, size_t index,
                                  size_t limit, int forward, double *floor)
{
    double peak = work->frames[index].power;
    double previous_power = peak;
    double previous_pitch = 0.0;
    size_t available = forward ? work->grid_count - index - 1U : index;
    size_t step;
    int preserve_breath = work->profile->recorder ||
                          work->profile == &isolation_profiles[HWA_ISOLATION_ALTO_FLUTE] ||
                          work->profile == &isolation_profiles[HWA_ISOLATION_ALTO_SAXOPHONE] ||
                          work->profile == &isolation_profiles[HWA_ISOLATION_TENOR_SAXOPHONE];
    size_t breath_span = preserve_breath ? (size_t)work->sample_rate / (5U * work->hop) : 0U;
    int stop_at_note = !work->profile->pitched_wind || work->profile->low_wind;
    /* In a long window, a louder note can mask the gap before a quiet attack. */
    double note_power = (work->profile->low_wind ? 0.50 : 0.12) * peak;
    /* Follow the connected air of a slow attack or release into a stronger tone. */
    if (preserve_breath) note_power = 2.0 * peak;

    if (limit > available) limit = available;
    for (step = 1U; step <= limit; ++step) {
        size_t other = forward ? index + step : index - step;
        const IsolationFrame *frame = &work->frames[other];
        double pitch = (stop_at_note || step <= breath_span) &&
            frame->power >= note_power && likely_instrument_pitch(frame) ? frame->pitch_hz : 0.0;
        if (pitches_agree(previous_pitch, pitch) ||
            (work->piano_frames != NULL && frame->power >= 0.12 * peak &&
             piano_frame_has_note(work, other)))
            break;
        if (frame->power <= 0.06 * peak && previous_power <= 0.06 * peak) {
            *floor = fmax(frame->power, previous_power);
            return step;
        }
        previous_power = frame->power;
        previous_pitch = pitch;
    }
    return 0U;
}

static int overlaps_short_note(const IsolationWorkspace *work, size_t index)
{
    size_t overlap = work->length / work->hop - 1U;
    size_t first = index > overlap ? index - overlap : 0U;
    size_t last = index + overlap < work->grid_count ? index + overlap : work->grid_count - 1U;
    double minimum_power = 0.12 * work->frames[index].power;
    double previous_pitch = 0.0;
    double previous_low_peak = 0.0;
    size_t i;

    /* Short notes spread energy across extra bins, but their pitch stays steady. */
    for (i = first; i <= last; ++i) {
        const IsolationFrame *frame = &work->frames[i];
        double pitch = frame->power >= minimum_power && frame->confidence >= 0.65 &&
            frame->off_harmonic_power <= 0.48 * frame->power ? frame->pitch_hz : 0.0;
        if (pitches_agree(previous_pitch, pitch)) return 1;
        if (work->piano_frames != NULL && frame->power >= minimum_power &&
            piano_frame_has_note(work, i))
            return 1;
        if (work->low_peaks != NULL) {
            double low_peak = frame->power >= minimum_power ? work->low_peaks[i] : 0.0;
            if (previous_low_peak > 0.0 && low_peak > 0.0 &&
                fabs(previous_low_peak - low_peak) <= work->bin_hz)
                return 1;
            previous_low_peak = low_peak;
        }
        previous_pitch = pitch;
    }
    return 0;
}

static double rest_disturbance_strength(const IsolationWorkspace *work, size_t index,
                                        double *low_strength)
{
    const IsolationFrame *frame = &work->frames[index];
    /* Look for brief bursts bracketed by quiet; longer windows need more context. */
    size_t limit = work->profile->rest_context_seconds > 0.0 ?
        (size_t)(work->profile->rest_context_seconds * work->sample_rate / (double)work->hop) :
        (size_t)work->sample_rate * 3U / (5U * work->hop);
    double left_floor, right_floor;
    double floor, total_power = 0.0, peak_power = 0.0;
    double impulse_power_ratio;
    size_t left, right, i;
    int pitched_wind = work->profile->pitched_wind;

    /* A wind release can stay tonal even when its pitch starts to move. */
    if (work->profile->tonal_release_off_fraction > 0.0 && frame->confidence >= 0.80 &&
        frame->off_harmonic_power <= work->profile->tonal_release_off_fraction * frame->power)
        return 0.0;
    if (frame->power == 0.0 ||
        (pitched_wind ? wind_overlaps_note(work, index) : likely_instrument_pitch(frame)) ||
        (work->piano_frames != NULL && piano_frame_has_note(work, index)))
        return 0.0;
    left = find_quiet_shoulder(work, index, limit, 0, &left_floor);
    if (left == 0U) return 0.0;
    right = find_quiet_shoulder(work, index, limit - left, 1, &right_floor);
    if (right == 0U || (!pitched_wind && (overlaps_short_note(work, index) ||
        (work->profile->window_overlap > 4U && overlaps_moving_pitch(work, index)))))
        return 0.0;
    floor = fmax(left_floor, right_floor);
    /* A burst dominated by frequencies below the instrument can be filtered even if
     * no note provides pitch evidence and the burst is too short for broadband cleanup. */
    if (work->profile->protect_low_notes && frame->low_power >= 0.65 * frame->power) {
        *low_strength = fmax(*low_strength, 0.88 * (1.0 - sqrt(floor / frame->power)));
    }
    for (i = index - left + 1U; i < index + right; ++i) {
        double power = fmax(0.0, work->frames[i].power - floor);
        const IsolationFrame *current = &work->frames[i];
        /* The quiet shoulders can enclose a whole phrase of short notes. */
        if (pitched_wind && i > index - left + 1U &&
            wind_short_pitch_pair(current - 1, current, 0.10 * frame->power))
            return 0.0;
        total_power += power;
        peak_power = fmax(peak_power, power);
    }
    /* Squared Hann weights sum to 3/8 of the overlap count for a single
     * impulse. Allow for impulses falling between window centers. */
    impulse_power_ratio = work->profile->window_overlap > 4U
        ? 3.0 * work->profile->window_overlap / 8.0 + 0.50 : 2.25;
    /* Brief wind attacks can carry energy before their pitch is trackable. */
    impulse_power_ratio = fmax(impulse_power_ratio, work->profile->min_rest_power_ratio);
    if (total_power <= impulse_power_ratio * peak_power) return 0.0;
    return 0.85 * (1.0 - sqrt(floor / frame->power));
}

typedef struct MarkedPeak {
    size_t bin;
    double power;
} MarkedPeak;

static int has_marked_pitch_conflict(IsolationWorkspace *work,
                                     size_t index, double pitch_hz)
{
    size_t half = work->length / 2U;
    size_t first = (size_t)ceil(work->profile->min_pitch_hz / work->bin_hz);
    double period_bins = pitch_hz / work->bin_hz;
    double power = work->frames[index].power;
    double tonal_power = 0.0;
    double other_power = 0.0;
    size_t covered_end = 0U;
    MarkedPeak prominent = {0};
    MarkedPeak second = {0};
    MarkedPeak outside = {0};
    size_t bin;
    const size_t offset = 2U;
    size_t side;

    /* A brief impact can have sharp peaks too. Require energy on both sides. */
    if (index < offset || work->grid_count - index <= offset ||
        work->frames[index - offset].power < 0.58 * power ||
        work->frames[index + offset].power < 0.58 * power)
        return 0;
    if (first < 2U) first = 2U;
    for (bin = first; bin + 2U <= half; ++bin) {
        double peak = work->power[bin];
        size_t neighbor;
        size_t start;
        if (peak < 0.012 * power || peak <= work->power[bin - 1U] ||
            peak < work->power[bin + 1U] ||
            fmin(work->power[bin - 2U], work->power[bin + 2U]) > 0.20 * peak)
            continue;
        if (peak > prominent.power) {
            second = prominent;
            prominent = (MarkedPeak){bin, peak};
        } else if (peak > second.power) {
            second = (MarkedPeak){bin, peak};
        }
        if (period_bins > 0.0 && harmonic_weight(bin, period_bins, 1.0) < 0.5 &&
            peak > outside.power)
            outside = (MarkedPeak){bin, peak};
        start = bin - 2U > covered_end ? bin - 2U : covered_end;
        for (neighbor = start; neighbor <= bin + 2U; ++neighbor) {
            tonal_power += work->power[neighbor];
            if (period_bins > 0.0) {
                other_power += (1.0 - harmonic_weight(neighbor, period_bins, 1.0)) *
                    work->power[neighbor];
            }
        }
        covered_end = neighbor;
    }
    if (tonal_power < 0.82 * power || second.power < 0.035 * power)
        return 0;
    if (period_bins > 0.0) {
        if (outside.power < 0.035 * power || other_power < 0.035 * power)
            return 0;
        if (prominent.bin != outside.bin) second = outside;
    }
    /* Confirm that both peaks persist. Restore the current spectrum if we filter it. */
    for (side = 0U; side < 2U; ++side) {
        size_t neighbor = side == 0U ? index - offset : index + offset;
        size_t peak_index;
        MarkedPeak candidates[2] = {prominent, second};
        if (spectral_frame(work, neighbor * work->hop) != 0) return -1;
        for (peak_index = 0U; peak_index < 2U; ++peak_index) {
            MarkedPeak candidate = candidates[peak_index];
            double support = fmax(work->power[candidate.bin],
                fmax(work->power[candidate.bin - 1U], work->power[candidate.bin + 1U]));
            if (support < 0.50 * candidate.power || support > 2.0 * candidate.power)
                return spectral_frame(work, index * work->hop);
        }
    }
    return 1;
}

static double marked_broadband_weight(const IsolationWorkspace *work, size_t bin,
                                       int reliable_pitch)
{
    size_t half = work->length / 2U;
    double left, right, floor, weight;
    if (bin < 4U || bin + 4U > half || work->power[bin] == 0.0) return 0.0;
    left = fmin(work->power[bin - 3U], work->power[bin - 4U]);
    right = fmin(work->power[bin + 3U], work->power[bin + 4U]);
    /* Leave narrow spectral peaks intact, including partials of a second note. */
    floor = fmin(left, right);
    weight = smooth_step((floor / work->power[bin] - 0.04) / 0.21);
    /* An uncertain attack can carry broad energy below its upper partials. */
    if (!reliable_pitch) {
        double frequency = (double)bin * work->bin_hz;
        weight *= smooth_step((frequency - 400.0) / 1200.0);
    }
    return weight;
}

static int measure_marked_background(IsolationWorkspace *work,
                                      const HwaIsolationRange *ranges,
                                      size_t range_count, size_t range)
{
    size_t half = work->length / 2U;
    uint64_t before = range > 0U ? ranges[range - 1U].end_sample : 0U;
    uint64_t after = range + 1U < range_count ?
        ranges[range + 1U].start_sample : work->frame_count;
    size_t starts[2];
    size_t count = 0U;
    size_t index;
    if (ranges[range].start_sample - before >= work->length) {
        starts[count++] = (size_t)ranges[range].start_sample - work->length + work->padding;
    }
    if (after - ranges[range].end_sample >= work->length) {
        starts[count++] = (size_t)ranges[range].end_sample + work->padding;
    }
    memset(work->marked_background, 0, (half + 1U) * sizeof(*work->marked_background));
    for (index = 0U; index < count; ++index) {
        size_t bin;
        if (spectral_frame(work, starts[index]) != 0) return -1;
        for (bin = 0U; bin <= half; ++bin) {
            /* Include an adjacent bin on either side to allow slight pitch changes. */
            double power = work->power[bin];
            if (bin > 0U) power = fmax(power, work->power[bin - 1U]);
            if (bin < half) power = fmax(power, work->power[bin + 1U]);
            work->marked_background[bin] = fmax(work->marked_background[bin], power);
        }
    }
    return 0;
}

static int add_correction(IsolationWorkspace *work, size_t start,
                          double pitch_hz, double alternate_pitch_hz,
                          double strength, double low_strength,
                          int confirmed_rest,
                          const HwaIsolationRange *active_range,
                          double *output)
{
    double period_bins = pitch_hz / work->bin_hz;
    double alternate_period = alternate_pitch_hz / work->bin_hz;
    size_t padded_begin = work->padding +
        (active_range != NULL ? (size_t)active_range->start_sample : 0U);
    size_t padded_end = work->padding +
        (active_range != NULL ? (size_t)active_range->end_sample : work->frame_count);
    size_t first = padded_begin > start ? padded_begin - start : 0U;
    size_t end = padded_end - start;
    size_t half = work->length / 2U;
    const double *rest_power[2] = {NULL, NULL};
    double rest_broadband_mix[2] = {0.0, 0.0};
    size_t bin;
    unsigned channel;
    if (end > work->length) end = work->length;
    if (confirmed_rest && work->piano_frames != NULL) {
        size_t frame = start / work->hop;
        for (channel = 0U; channel < work->channels; ++channel) {
            double total = work->frames[frame].power;
            double broadband = work->piano_frames[frame].broad_power;
            rest_power[channel] = work->power;
            if (work->piano_channel_power != NULL) {
                rest_power[channel] = work->piano_channel_power + (half + 1U) * channel;
                total = 0.0;
                for (bin = 0U; bin <= half; ++bin) total += rest_power[channel][bin];
                broadband = spectral_broadband_power(rest_power[channel], half);
            }
            /* The other channel may have supplied all the noise evidence. */
            if (total > 0.0)
                rest_broadband_mix[channel] = smooth_step((broadband / total - 0.16) / 0.15);
        }
    }
    for (bin = 0U; bin <= half; ++bin) {
        double harmonic = period_bins > 0.0 ? harmonic_weight(bin, period_bins, 1.0) : 0.0;
        double weight = 1.0;
        double low = 1.0 - smooth_step(((double)bin * work->bin_hz -
                                        work->profile->low_mask_start_hz) /
                                       work->profile->low_mask_width_hz);
        if (alternate_period > 0.0) {
            /* Short notes occupy wider bins than a sustained pitch. */
            harmonic = fmax(harmonic, harmonic_weight(bin, alternate_period, 1.8));
        }
        if (!confirmed_rest) {
            if (work->piano_frames != NULL)
                weight = spectral_broadband_weight(work->power, half, bin);
            else if (work->marked_background != NULL && !work->profile->pitched_wind)
                weight = marked_broadband_weight(work, bin, pitch_hz > 0.0);
        }
        double correction = -fmax(strength * (1.0 - harmonic) * weight, low_strength * low);
        if (work->marked_background != NULL && work->power[bin] > 0.0) {
            double background = work->marked_background[bin] / work->power[bin];
            correction *= smooth_step((1.0 - 1.4 * background) / 0.82);
        }
        for (channel = 0U; channel < work->channels; ++channel) {
            HwaDspComplex *spectrum = work->spectrum + work->length * channel;
            double channel_correction = correction;
            if (rest_power[channel] != NULL) {
                double spectral_weight = spectral_broadband_weight(rest_power[channel], half, bin);
                double mix = rest_broadband_mix[channel];
                channel_correction *= mix * (mix + (1.0 - mix) * spectral_weight);
            }
            spectrum[bin].real *= channel_correction;
            spectrum[bin].imag *= channel_correction;
            if (bin > 0U && bin < half) {
                spectrum[work->length - bin].real = spectrum[bin].real;
                spectrum[work->length - bin].imag = -spectrum[bin].imag;
            }
        }
    }
    for (channel = 0U; channel < work->channels; ++channel) {
        HwaDspComplex *spectrum = work->spectrum + work->length * channel;
        size_t index;
        if (hwa_dsp_fft(spectrum, work->length, 1) != HWA_DSP_OK) return -1;
        for (index = first; index < end; ++index) {
            size_t frame = start + index - work->padding;
            output[frame * work->frame_stride + channel] +=
                spectrum[index].real * work->window[index];
        }
    }
    return 0;
}

static int accumulate_frame_correction(IsolationWorkspace *work, size_t index,
                                       int marked, const HwaIsolationRange *active_range,
                                       double *output)
{
    size_t start = index * work->hop;
    double pitch_hz;
    double strength;
    double low_strength;
    int confirmed_rest = 0;
    int pitched_wind = work->profile->pitched_wind;
    IsolationFrame alternate = {0};
    if (!pitched_wind && spectral_frame(work, start) != 0) return -1;
    if (work->piano_frames != NULL) {
        pitch_hz = 0.0;
        low_strength = 0.0;
        strength = piano_chord_gain(work, index, marked);
    } else if (pitched_wind) {
        wind_gain(work, index, marked, active_range, &pitch_hz, &strength, &low_strength);
    } else {
        isolation_gain(work, index, marked, &pitch_hz, &strength, &low_strength);
    }
    if (!marked || work->profile->detect_marked_rests) {
        double rest_strength = rest_disturbance_strength(work, index, &low_strength);
        if (rest_strength > 0.0) {
            confirmed_rest = 1;
            strength = rest_strength;
            pitch_hz = 0.0;
        } else if (!marked && strength > 0.0 && work->piano_frames == NULL && !pitched_wind) {
            /* A short played note can appear in the spectrum of its neighbors. */
            if (measure_pitch(work, &alternate, pitch_hz) != 0) return -1;
            if (alternate.confidence >= 0.80) strength = 0.0;
            if (alternate.confidence < 0.70) alternate.pitch_hz = 0.0;
        }
    }
    if (marked && work->profile->protect_marked_chords) {
        int conflict = has_marked_pitch_conflict(work, index, pitch_hz);
        if (conflict < 0) return -1;
        if (conflict > 0) return 0;
    }
    if (strength == 0.0 && low_strength == 0.0) return 0;
    /* Wind gain uses cached frame measures, so transform only when needed. */
    if (pitched_wind && spectral_frame(work, start) != 0) return -1;
    /* A long contrabassoon window can span several notes with no single pitch.
     * Reduce broadband rest noise in proportion to its share of that spectrum. */
    if (confirmed_rest && work->profile == &isolation_profiles[HWA_ISOLATION_CONTRABASSOON]) {
        double broadband = spectral_broadband_power(work->power, work->length / 2U);
        strength *= smooth_step((broadband / work->frames[index].power - 0.22) / 0.15);
        if (strength == 0.0 && low_strength == 0.0) return 0;
    }
    return add_correction(work, start, pitch_hz, alternate.pitch_hz, strength,
                          low_strength, confirmed_rest, active_range, output);
}

static int accumulate_marked_ranges(IsolationWorkspace *work,
                                    const HwaIsolationRange *ranges,
                                    size_t range_count, double *output)
{
    size_t range;
    /* Overlapping windows contribute with each range's own background mask. */
    for (range = 0U; range < range_count; ++range) {
        const HwaIsolationRange *active_range = &ranges[range];
        /* A window ends one hop past its grid index on the source clock. */
        size_t first = (size_t)active_range->start_sample / work->hop;
        size_t end = ((size_t)active_range->end_sample + work->padding - 1U) /
            work->hop + 1U;
        size_t index;
        if (measure_marked_background(work, ranges, range_count, range) != 0) return -1;
        for (index = first; index < end; ++index) {
            if (work->frames[index].power != 0.0 &&
                accumulate_frame_correction(work, index, 1, active_range, output) != 0)
                return -1;
        }
    }
    return 0;
}

static int accumulate_correction(IsolationWorkspace *work,
                                 const HwaIsolationRange *ranges,
                                 size_t range_count, double *output)
{
    size_t range = 0U;
    size_t measured_range = SIZE_MAX;
    size_t index;
    if (work->marked_background != NULL &&
        (work->profile == &isolation_profiles[HWA_ISOLATION_DOUBLE_BASS] ||
         work->piano_frames != NULL || work->profile->pitched_wind))
        return accumulate_marked_ranges(work, ranges, range_count, output);
    for (index = 0U; index < work->grid_count; ++index) {
        size_t start = index * work->hop;
        if (!frame_intersects_ranges(work, start, 0U, ranges, range_count, &range) ||
            work->frames[index].power == 0.0) continue;
        if (work->marked_background != NULL && measured_range != range) {
            if (measure_marked_background(work, ranges, range_count, range) != 0) return -1;
            measured_range = range;
        }
        if (accumulate_frame_correction(work, index, range_count != 0U, NULL, output) != 0)
            return -1;
    }
    return 0;
}

static void finish_estimate(const IsolationWorkspace *work,
                            const HwaIsolationRange *ranges, size_t range_count,
                            double *output)
{
    size_t range = 0U;
    size_t index;
    for (index = 0U; index < work->frame_count; ++index) {
        double strength = 1.0;
        unsigned channel;
        if (range_count != 0U) {
            while (range < range_count && index >= ranges[range].end_sample) ++range;
            if (range < range_count && index >= ranges[range].start_sample) {
                uint64_t length = ranges[range].end_sample - ranges[range].start_sample;
                uint64_t edge = index - ranges[range].start_sample;
                uint64_t right = ranges[range].end_sample - 1U - index;
                size_t ramp = length / 2U < work->hop ? (size_t)(length / 2U) : work->hop;
                if (right < edge) edge = right;
                strength = smooth_step((double)(edge + 1U) / (double)(ramp + 1U));
            } else {
                strength = 0.0;
            }
        }
        for (channel = 0U; channel < work->channels; ++channel) {
            size_t sample = index * work->frame_stride + channel;
            if (strength == 0.0 || output[sample] == 0.0) {
                output[sample] = work->input[sample];
            } else {
                output[sample] = work->input[sample] + strength *
                    output[sample] / work->normalizer[index % work->hop];
            }
        }
    }
}

int hwa_isolate_instrument(const double *input, size_t frame_count,
                           unsigned channels, unsigned sample_rate_hz,
                           HwaIsolationInstrument instrument,
                           const HwaIsolationRange *ranges,
                           size_t range_count, uint64_t max_work_bytes,
                           double *output, char *error, size_t error_size)
{
    IsolationWorkspace work = {0};
    size_t fixed_work;
    size_t per_frame_work;
    size_t sample_count;
    size_t index;
    unsigned channel_passes;
    unsigned pass;
    int use_marked_background;
    int measure_low_peaks;
    int piano = instrument == HWA_ISOLATION_PIANO;
    int piano_stereo = piano && channels == 2U;
    int result = -1;
    if (input == NULL || output == NULL || input == output || frame_count == 0U ||
        (channels != 1U && channels != 2U) ||
        sample_rate_hz < 8000U || sample_rate_hz > 192000U ||
        frame_count > SIZE_MAX / sizeof(*output) / channels ||
        (ranges == NULL && range_count != 0U)) {
        hwa_set_error(error, error_size, "instrument isolation requires nonempty mono or stereo audio at 8000 to 192000 Hz");
        return -1;
    }
    if ((size_t)instrument >= sizeof(isolation_profiles) / sizeof(isolation_profiles[0])) {
        hwa_set_error(error, error_size, "unsupported isolation instrument");
        return -1;
    }
    for (index = 0U; index < range_count; ++index) {
        if (ranges[index].start_sample >= ranges[index].end_sample ||
            ranges[index].end_sample > frame_count ||
            (index > 0U && ranges[index].start_sample < ranges[index - 1U].end_sample)) {
            hwa_set_error(error, error_size, "instrument isolation ranges must be ordered, disjoint, and within the source");
            return -1;
        }
    }
    sample_count = frame_count * channels;
    for (index = 0U; index < sample_count; ++index) {
        if (!isfinite(input[index]) || fabs(input[index]) > FLT_MAX) {
            hwa_set_error(error, error_size, "instrument isolation requires finite samples within float32 range");
            return -1;
        }
    }
    work.profile = &isolation_profiles[instrument];
    use_marked_background = range_count != 0U && work.profile->protect_marked_spectrum;
    measure_low_peaks = work.profile->protect_low_notes &&
                        (range_count == 0U || work.profile->detect_marked_rests);
    /* Noise on one stereo channel cannot supply evidence to cut the other. */
    channel_passes = work.profile->pitched_wind ? channels : 1U;
    work.frame_count = frame_count;
    work.channels = channels / channel_passes;
    work.frame_stride = channels;
    work.sample_rate = sample_rate_hz;
    /* Keep enough cycles of the lowest notes in each power-of-two window. */
    work.length = work.profile->recorder || instrument == HWA_ISOLATION_PICCOLO ||
        instrument == HWA_ISOLATION_E_FLAT_CLARINET ||
        instrument == HWA_ISOLATION_ALTO_FLUTE ||
        instrument == HWA_ISOLATION_ALTO_SAXOPHONE ? 256U : 512U;
    while ((double)work.length < work.profile->min_window_seconds * sample_rate_hz)
        work.length *= 2U;
    work.hop = work.length / work.profile->window_overlap;
    /* Include every overlapping window at both ends of the recording. */
    work.padding = work.length - work.hop;
    work.bin_hz = (double)sample_rate_hz / (double)work.length;
    work.grid_count = (frame_count + work.padding - 1U) / work.hop + 1U;
    fixed_work = (work.length + work.length / 2U + 1U + work.hop) * sizeof(double) +
                 work.length * (work.channels + (piano ? 0U : 1U)) * sizeof(HwaDspComplex);
    if (use_marked_background)
        fixed_work += (work.length / 2U + 1U) * sizeof(*work.marked_background);
    if (piano_stereo)
        fixed_work += (work.length / 2U + 1U) * channels * sizeof(*work.piano_channel_power);
    per_frame_work = sizeof(*work.frames);
    if (measure_low_peaks) per_frame_work += sizeof(*work.low_peaks);
    if (piano) per_frame_work += sizeof(*work.piano_frames);
    if (fixed_work > max_work_bytes ||
        work.grid_count > SIZE_MAX / per_frame_work ||
        work.grid_count > (max_work_bytes - fixed_work) / per_frame_work) {
        hwa_set_error(error, error_size, "instrument isolation exceeds the work limit");
        return -1;
    }
    work.window = malloc(work.length * sizeof(*work.window));
    work.power = malloc((work.length / 2U + 1U) * sizeof(*work.power));
    if (piano_stereo) {
        work.piano_channel_power = malloc((work.length / 2U + 1U) * channels *
                                          sizeof(*work.piano_channel_power));
    }
    work.normalizer = calloc(work.hop, sizeof(*work.normalizer));
    work.spectrum = malloc(work.length * work.channels * sizeof(*work.spectrum));
    if (!piano) work.autocorrelation = malloc(work.length * sizeof(*work.autocorrelation));
    work.frames = calloc(work.grid_count, sizeof(*work.frames));
    if (piano) work.piano_frames = calloc(work.grid_count, sizeof(*work.piano_frames));
    if (measure_low_peaks) work.low_peaks = calloc(work.grid_count, sizeof(*work.low_peaks));
    if (use_marked_background) {
        work.marked_background = malloc((work.length / 2U + 1U) *
                                        sizeof(*work.marked_background));
    }
    if ((use_marked_background && work.marked_background == NULL) ||
        (piano_stereo && work.piano_channel_power == NULL) ||
        work.window == NULL || work.power == NULL || work.normalizer == NULL ||
        work.spectrum == NULL || work.frames == NULL ||
        (piano ? work.piano_frames == NULL : work.autocorrelation == NULL) ||
        (measure_low_peaks && work.low_peaks == NULL)) {
        hwa_set_error(error, error_size, "cannot allocate instrument isolation workspace");
        goto cleanup;
    }
    if (hwa_dsp_hann(work.window, work.length) != HWA_DSP_OK) goto transform_error;
    /* A window enters twice, once in analysis and once in reconstruction. */
    for (index = 0U; index < work.length; ++index) {
        work.normalizer[index % work.hop] += work.window[index] * work.window[index];
    }
    memset(output, 0, sample_count * sizeof(*output));
    for (pass = 0U; pass < channel_passes; ++pass) {
        work.input = input + pass;
        if (pass != 0U) memset(work.frames, 0, work.grid_count * sizeof(*work.frames));
        if (measure_frames(&work, ranges, range_count) != 0 ||
            accumulate_correction(&work, ranges, range_count, output + pass) != 0)
            goto transform_error;
        finish_estimate(&work, ranges, range_count, output + pass);
    }
    result = 0;
    goto cleanup;

transform_error:
    hwa_set_error(error, error_size, "instrument isolation spectral transform failed");
cleanup:
    free(work.piano_frames);
    free(work.marked_background);
    free(work.low_peaks);
    free(work.window);
    free(work.power);
    free(work.piano_channel_power);
    free(work.normalizer);
    free(work.spectrum);
    free(work.autocorrelation);
    free(work.frames);
    return result;
}

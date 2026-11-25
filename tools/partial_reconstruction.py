"""Offline measured-partial reconstruction; requires NumPy and SciPy.

Recording-dependent diagnostic, not a synthesis voice or decay fitter. Carrier
clock: source sample zero. Zero-phase filtering uses future samples; later
windows from this recording are not independent validation takes.
"""
from __future__ import annotations

import math
from numbers import Integral

import numpy as np
from scipy import interpolate, signal


def mono_signal(samples):
    values = np.asarray(samples, dtype=np.float64)
    if values.ndim != 1 or len(values) < 64 or not np.isfinite(values).all():
        raise ValueError("need a finite mono recording of at least 64 samples")
    return values


def window(samples, rate, bounds):
    """Select a half-open interval rounded to the nearest source samples."""
    start, stop = _window_indices(len(samples), rate, bounds)
    return samples[start:stop]


def _window_indices(length, rate, bounds):
    start, stop = (float(value) for value in bounds)
    if not math.isfinite(rate) or rate <= 0 or not 0 <= start < stop <= length / rate:
        raise ValueError("window is empty or outside the source recording")
    start, stop = round(start * rate), round(stop * rate)
    if start == stop:
        raise ValueError("window is empty or outside the source recording")
    return start, stop


def rms(samples):
    return float(np.sqrt(np.mean(np.abs(samples) ** 2)))


def db(value):
    return float(20 * np.log10(max(float(value), 1e-30)))


def peak_centres(samples, rate, fundamental, bounds, maximum_hz=6000, fft_size=8192):
    """One measured peak per harmonic neighborhood, including weak candidates.

    Hann Welch spectrum, 7/8 overlap, log-power quadratic bin interpolation.
    The caller must screen background; a harmonic grid is only a search seed.
    """
    values = mono_signal(samples)
    if not math.isfinite(rate) or not 0 < fundamental < maximum_hz < rate / 2:
        raise ValueError("need 0 < fundamental < maximum_hz < Nyquist")
    if not isinstance(fft_size, Integral) or fft_size < 8:
        raise ValueError("FFT size must be an integer of at least 8 samples")
    held = window(values, rate, bounds)
    if len(held) < fft_size:
        raise ValueError("peak window is shorter than the requested FFT")
    frequencies, power = signal.welch(
        held, rate, window="hann", nperseg=fft_size,
        noverlap=fft_size - fft_size // 8, detrend=False)
    bin_width = rate / fft_size
    peaks = []
    for harmonic in range(1, math.floor(maximum_hz / fundamental) + 1):
        lower = (harmonic - .3) * fundamental
        upper = min((harmonic + .3) * fundamental, maximum_hz)
        # A bin just outside the window can interpolate to a peak inside it.
        first, stop = np.searchsorted(
            frequencies, (lower - bin_width / 2, upper + bin_width / 2))
        indices = np.arange(max(1, first), min(stop, len(power) - 1))
        for index in indices[np.argsort(-power[indices], kind="stable")]:
            a, b, c = np.log(np.maximum(power[index - 1:index + 2], 1e-60))
            denominator = a - 2 * b + c
            offset = 0 if abs(denominator) < 1e-15 else np.clip(.5 * (a - c) / denominator, -.5, .5)
            centre = float((index + offset) * bin_width)
            if lower < centre <= upper:
                peaks.append({"harmonic": harmonic, "centre_hz": centre})
                break
    return peaks


def complex_trace(samples, rate, centre, bandwidth, trace_rate=400):
    """Demodulate, anti-alias/resample, then zero-phase low-pass.

    Bandwidth sets the envelope filter's one-pass Butterworth -3 dB cutoff.
    Forward/backward filtering gives about -6 dB there. No onset suppression,
    phase reset, time stretching or per-window normalization.
    """
    values = mono_signal(samples)
    _validate_trace_rates(rate, bandwidth, trace_rate)
    if not bandwidth < centre < rate / 2 - bandwidth:
        raise ValueError("invalid rate, carrier, or bandwidth")
    times = np.arange(len(values)) / rate
    baseband = 2 * values * np.exp(-2j * np.pi * centre * times)
    if trace_rate < rate:
        # Suppress strong out-of-band energy before it can fold into the
        # measured passband. The resampler's FIR alone leaves detectable aliases.
        antialias = signal.butter(8, .35 * trace_rate, fs=rate, output="sos")
        baseband = signal.sosfiltfilt(antialias, baseband)
    divisor = math.gcd(rate, trace_rate)
    decimated = signal.resample_poly(baseband, trace_rate // divisor, rate // divisor)
    if len(decimated) < 32:
        raise ValueError("recording is too short for the envelope filter")
    filtered = signal.sosfiltfilt(
        signal.butter(6, bandwidth, fs=trace_rate, output="sos"), decimated)
    return np.arange(len(filtered)) / trace_rate, filtered


def _validate_trace_rates(rate, bandwidth, trace_rate):
    if (not isinstance(rate, Integral) or not isinstance(trace_rate, Integral)
            or not 0 < trace_rate <= rate or not 0 < bandwidth < trace_rate / 4):
        raise ValueError("invalid rate, carrier, or bandwidth")


def reconstruct(samples, rate, peaks, bandwidth, signal_bounds, noise_bounds,
                minimum_snr_db=20, trace_rate=400):
    """Return measured sum, slow complex traces, and all selection results.

    Require quiet *source* samples, not inserted silence. The residual is the
    filter-bank complement, not isolated bow noise. SNR is descriptive.
    """
    values = mono_signal(samples)
    _validate_trace_rates(rate, bandwidth, trace_rate)
    if not math.isfinite(minimum_snr_db):
        raise ValueError("minimum SNR must be finite")
    peaks = list(peaks)
    ordered = sorted(float(peak["centre_hz"]) for peak in peaks)
    if not ordered:
        raise ValueError("need at least one partial")
    if any(right - left <= 2 * bandwidth for left, right in zip(ordered, ordered[1:])):
        raise ValueError("partial passbands must not overlap")
    held_start, held_stop = _window_indices(len(values), rate, signal_bounds)
    noise_start, noise_stop = _window_indices(len(values), rate, noise_bounds)
    if held_start < noise_stop and noise_start < held_stop:
        raise ValueError("signal and background windows must not overlap")
    trace_start, trace_stop = (
        (frame * trace_rate + rate - 1) // rate for frame in (held_start, held_stop))
    if trace_start == trace_stop:
        raise ValueError("signal window contains no trace samples")
    background = values[noise_start:noise_stop]
    if np.count_nonzero(background) < len(background) / 2 or rms(background) <= 1e-12:
        raise ValueError("background is digital silence or unavailable, not measured noise")
    background_duration = len(background) / rate
    edge_guard = min(3 / bandwidth, background_duration / 4)
    times = np.arange(len(values)) / rate
    output = np.zeros(len(values))
    rows, traces = [], []
    for peak in peaks:
        centre = float(peak["centre_hz"])
        trace_times, trace = complex_trace(values, rate, centre, bandwidth, trace_rate)
        level = rms(trace[trace_start:trace_stop])
        # Filter the background separately so note energy cannot leak into it.
        _, background_trace = complex_trace(background, rate, centre, bandwidth, trace_rate)
        noise = rms(window(background_trace, trace_rate,
                           (edge_guard, background_duration - edge_guard)))
        snr = None if noise <= 1e-15 else db(level / noise)
        accepted = snr is not None and snr >= minimum_snr_db
        rows.append(dict(peak, signal_db=db(level), background_db=db(noise),
                         snr_db=snr, retained=bool(accepted)))
        traces.append(trace)
        if accepted:
            envelope = interpolate.CubicSpline(trace_times, trace)(times)
            output += np.real(envelope * np.exp(2j * np.pi * centre * times))
    return output, trace_times, np.asarray(traces), rows

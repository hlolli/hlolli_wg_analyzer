# Measured partial reconstruction (optional Python diagnostic)

`tools/partial_reconstruction.py` measures complex partial traces and
reconstructs selected partials from a mono recording. Instrument analysis
scripts can import it from the analyzer's `tools` directory. It requires NumPy
and SciPy.

`peak_centres` uses a supplied fundamental only to seed harmonic neighborhoods
and measures peaks up to the requested frequency limit with a Hann/Welch
spectrum. `complex_trace` demodulates relative to source sample zero, filters
out frequencies that could alias during resampling, and applies a zero-phase
low-pass filter at the requested bandwidth.

`reconstruct` takes nonoverlapping signal and background windows in seconds.
It rounds their bounds to source samples, then measures signal strength on
the shared trace clock. It filters the background on its own and excludes
its edges from the noise measurement. Both the recording and the background
interval must yield at least 32 trace samples at the chosen trace rate.
The function rejects digital silence as unavailable background. A recorded
tail may contain instrument or room energy, so treat its level as a
conservative background estimate.

```python
from partial_reconstruction import peak_centres, reconstruct

held = (0.5, 1.5)
background = (0.1, 0.25)
peaks = peak_centres(samples, sample_rate, fundamental, held)
partial_sum, trace_times, traces, results = reconstruct(
    samples, sample_rate, peaks, 20, held, background)
residual = samples - partial_sum
```

`traces` and `results` follow the input peak order. Each result records the
signal and background levels in dB, the estimated SNR, and whether the sum
includes that partial. An unavailable SNR appears as `None` and excludes
the partial. Partial centres must be more than twice the bandwidth apart.

Zero-phase filtering uses future samples, and both the traces and the sum
depend on this recording. The residual contains everything the filter bank
leaves behind, which can include inharmonic components and missed partials.

Run the known-signal checks in a Python environment with NumPy and SciPy.

```sh
python3 -B tests/test_partial_reconstruction.py
```

Instrument adapters choose and prepare recordings, set comparison windows,
and produce plots and listening copies. Keep recordings and experiment
output outside checkouts.

"""Known-signal checks for the optional measured-partial diagnostic."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
try:
    import numpy as np
    import partial_reconstruction as partials
except ModuleNotFoundError as error:
    if error.name not in {"numpy", "scipy"}:
        raise
    if __name__ == "__main__":
        print("Partial reconstruction tests require NumPy and SciPy.")
        sys.exit(77)
    raise unittest.SkipTest("partial reconstruction tests require NumPy and SciPy")


class ReconstructionTests(unittest.TestCase):
    def test_gain_phase_and_modulation_at_two_sample_rates(self):
        for rate in (44100, 48000):
            with self.subTest(rate=rate):
                times = np.arange(rate * 3) / rate
                centre = 197.37
                envelope = (.1 + .03 * np.cos(2 * np.pi * 2 * times)) * np.exp(
                    1j * (.73 + .25 * np.sin(2 * np.pi * 3 * times)))
                samples = np.real(envelope * np.exp(2j * np.pi * centre * times))
                t, trace = partials.complex_trace(samples, rate, centre, 20)
                expected = (.1 + .03 * np.cos(2 * np.pi * 2 * t)) * np.exp(
                    1j * (.73 + .25 * np.sin(2 * np.pi * 3 * t)))
                interior = (t > .4) & (t < 2.6)
                error = np.linalg.norm(trace[interior] - expected[interior]) / np.linalg.norm(expected[interior])
                self.assertLess(error, .001)

    def test_separates_neighbor_and_preserves_sample_clock(self):
        rate = 48000
        times = np.arange(rate * 3) / rate
        expected = .13 * np.exp(1j * (1.1 + 2 * np.pi * 1.7 * times))
        samples = np.real(expected * np.exp(2j * np.pi * 98.8 * times))
        samples += .3 * np.sin(2 * np.pi * 197.6 * times)
        t, trace = partials.complex_trace(samples, rate, 98.8, 20)
        truth = .13 * np.exp(1j * (1.1 + 2 * np.pi * 1.7 * t))
        interior = (t > .4) & (t < 2.6)
        self.assertLess(np.max(abs(trace[interior] - truth[interior])), .0002)

    def test_measured_peak_is_not_forced_to_harmonic_grid(self):
        rate = 44100
        times = np.arange(rate) / rate
        peaks = partials.peak_centres(np.sin(2 * np.pi * 98.8 * times), rate, 98, (.1, .9), 600)
        self.assertAlmostEqual(peaks[0]["centre_hz"], 98.8, delta=.15)

    def test_peak_search_respects_the_frequency_limit(self):
        rate = 44100
        times = np.arange(rate) / rate
        samples = .8 * np.sin(2 * np.pi * 311 * times)
        samples += .2 * np.sin(2 * np.pi * 282 * times)
        peaks = partials.peak_centres(samples, rate, 100, (.1, .9), 300)
        self.assertAlmostEqual(peaks[-1]["centre_hz"], 282, delta=.2)
        self.assertTrue(all(peak["centre_hz"] <= 300 for peak in peaks))

    def test_full_reconstruction_and_residual_identity(self):
        rate = 44100
        times = np.arange(rate * 3) / rate
        samples = np.random.default_rng(17).normal(0, 1e-5, len(times))
        envelope = np.minimum(np.maximum(times - .4, 0) / .1, 1)
        tone = .1 * envelope * np.cos(2 * np.pi * 98.8 * times + .9)
        samples += tone
        reconstructed, _, _, rows = partials.reconstruct(
            samples, rate, [{"centre_hz": 98.8}], 20, (.8, 2.5), (.1, .2))
        self.assertTrue(rows[0]["retained"])
        held = (times > .8) & (times < 2.5)
        self.assertLess(partials.rms((reconstructed - tone)[held]), .0001)
        np.testing.assert_allclose(reconstructed + (samples - reconstructed), samples, atol=1e-15, rtol=0)

    def test_out_of_band_tone_does_not_create_a_partial(self):
        rate = 44100
        times = np.arange(rate * 3) / rate
        samples = np.random.default_rng(5).normal(0, 1e-5, len(times))
        samples += np.where(times > .4, .9 * np.cos(2 * np.pi * 500 * times), 0)
        peaks = [{"centre_hz": 100}, {"centre_hz": 500}]
        _, _, _, rows = partials.reconstruct(
            samples, rate, peaks, 20, (1, 2), (.1, .2))
        self.assertFalse(rows[0]["retained"])
        self.assertTrue(rows[1]["retained"])

    def test_reconstruct_accepts_a_peak_iterator(self):
        rate = 44100
        samples = np.random.default_rng(8).normal(0, .01, rate)
        peaks = ({"centre_hz": centre} for centre in (500, 100))
        _, _, traces, rows = partials.reconstruct(
            samples, rate, peaks, 20, (.5, .9), (.1, .2))
        self.assertEqual([row["centre_hz"] for row in rows], [500, 100])
        self.assertEqual(len(traces), 2)

    def test_screening_windows_use_the_source_clock(self):
        rate = 44100
        times = np.arange(rate * 3 - 54) / rate
        samples = .1 * np.cos(2 * np.pi * 100 * times)
        peaks = [{"centre_hz": 100}]
        with self.assertRaisesRegex(ValueError, "overlap"):
            partials.reconstruct(samples, rate, peaks, 20, (.5, 1.1), (.9, 1.4))
        with self.assertRaisesRegex(ValueError, "outside the source recording"):
            partials.reconstruct(samples, rate, peaks, 20, (1, 3), (0, .4))

    def test_digital_silence_is_not_a_noise_measurement(self):
        rate = 44100
        times = np.arange(rate) / rate
        samples = np.where(times > .3, .1 * np.cos(2 * np.pi * 100 * times), 0)
        with self.assertRaisesRegex(ValueError, "digital silence"):
            partials.reconstruct(samples, rate, [{"centre_hz": 100}],
                                 20, (.5, .8), (.02, .12))

    def test_rejects_nonfinite_overlap_and_missing_windows(self):
        with self.assertRaises(ValueError):
            partials.complex_trace(np.full(1000, np.nan), 1000, 100, 20)
        with self.assertRaises(ValueError):
            partials.reconstruct(np.zeros(1000), 1000,
                                 [{"centre_hz": 100}, {"centre_hz": 120}],
                                 20, (.1, .5), (0, .1))
        with self.assertRaises(ValueError):
            partials.window(np.zeros(100), 1000, (0, 1))


if __name__ == "__main__":
    unittest.main()

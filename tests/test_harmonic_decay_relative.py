#!/usr/bin/env python3
"""Check decay estimates against known signals, gain, and noise limits."""
import importlib.util
import json
import math
from pathlib import Path
import random
import subprocess
import tempfile
import unittest
import wave

import test_instrument_fit as fixtures

SPEC = importlib.util.spec_from_file_location(
    "instrument_fit", Path(__file__).resolve().parents[1] / "tools/instrument_fit.py")
FIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIT)
METHOD = "harmonic-decay-v2"


def write_signal(path, *, gain=1.0, width=3, noise=0.0, missing=False, tau=.4):
    rate = 16000
    maximum = (1 << (width * 8 - 1)) - 1
    rng = random.Random(721)
    raw = bytearray()
    for index in range(rate * 3):
        age = index / rate - 0.1
        value = 0.0
        if age >= 0:
            for harmonic in range(1, 5):
                if missing and harmonic == 4:
                    continue
                # Integer cycles per analysis window limit inter-band leakage.
                value += (.18 / harmonic * math.exp(-age / tau) *
                          math.sin(2 * math.pi * 125 * harmonic * age))
        value = gain * (value + noise * rng.uniform(-1, 1))
        raw.extend(round(value * maximum).to_bytes(width, "little", signed=True))
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(width)
        out.setframerate(rate)
        out.writeframes(raw)


class RelativeHarmonicTests(unittest.TestCase):
    def profile(self, path):
        return FIT.harmonic_decay_profile(path, 125, 4, method_version=METHOD)

    def test_known_decay_survives_gain_change_above_quantization(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            profiles = []
            for gain in (1.0, .001):
                path = root / (str(gain) + '.wav')
                write_signal(path, gain=gain)
                profiles.append(self.profile(path))
            for rows in profiles:
                self.assertEqual([r['status'] for r in rows], ['valid'] * 4)
                for row in rows:
                    self.assertAlmostEqual(row['t60_seconds'], .4 * 3 * math.log(10), delta=.006)
                    self.assertGreaterEqual(row['dynamic_range_db'], 20)
            # Old reports must retain the old method and its old outcome.
            old = FIT.harmonic_decay_profile(path, 125, 4)
            self.assertEqual([r['status'] for r in old], ['valid', 'valid', 'too-short', 'too-short'])

    def test_measured_noise_floor_moves_with_gain(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            profiles = []
            for gain in (1.0, .1):
                path = root / (str(gain) + '.wav')
                write_signal(path, gain=gain, noise=.002)
                profiles.append(self.profile(path))
            self.assertEqual([r['status'] for r in profiles[0]], [r['status'] for r in profiles[1]])
            self.assertTrue(any(r['status'] == 'valid' for r in profiles[0]))
            for loud, quiet in zip(*profiles):
                self.assertAlmostEqual(loud['noise_floor_dbfs'] - quiet['noise_floor_dbfs'], 20, delta=.02)
                if loud['status'] == 'valid':
                    self.assertAlmostEqual(loud['t60_seconds'], quiet['t60_seconds'], delta=.02)
                    self.assertAlmostEqual(loud['t60_seconds'], .4 * 3 * math.log(10), delta=.15)

    def test_noise_does_not_supply_a_missing_harmonic(self):
        with tempfile.TemporaryDirectory() as text:
            path = Path(text) / 'missing.wav'
            write_signal(path, noise=.02, missing=True)
            rows = self.profile(path)
            self.assertNotEqual(rows[3]['status'], 'valid')

    def test_quiet_pcm16_does_not_gain_false_precision(self):
        with tempfile.TemporaryDirectory() as text:
            path = Path(text) / 'quiet.wav'
            write_signal(path, gain=.002, width=2)
            rows = self.profile(path)
            self.assertTrue(all(r['status'] != 'valid' for r in rows))
            self.assertTrue(all(r['fit_floor_dbfs'] >= r['quantization_bound_dbfs'] + 12 for r in rows))

    def test_mixed_profile_methods_are_rejected(self):
        with tempfile.TemporaryDirectory() as text:
            path = Path(text) / 'decay.wav'
            write_signal(path)
            old = FIT.harmonic_decay_profile(path, 125, 4)
            new = self.profile(path)
            with self.assertRaisesRegex(FIT.FitError, 'method'):
                FIT.compare_harmonic_decay_profiles(old, new, 125, 4, method_version=METHOD)

    def test_unknown_method_is_rejected(self):
        with self.assertRaisesRegex(FIT.FitError, 'method'):
            FIT.harmonic_decay_profile(Path('unused.wav'), 125, 4, method_version='typo')

    def test_manifest_requires_one_known_harmonic_method(self):
        with tempfile.TemporaryDirectory() as text:
            fixture = fixtures.make_v1_fixture(Path(text))
            manifest = json.loads(fixture['manifest'].read_text())
            for row in manifest['objectives']:
                row.update(kind='harmonic-decay', fundamental_hz=125,
                           harmonic_count=4, harmonic_method_version=METHOD)
            manifest['selection'].update(max_candidate_harmonic_mean_error_octaves=.75,
                                         max_candidate_harmonic_maximum_error_octaves=1.5)
            fixture['manifest'].write_text(json.dumps(manifest))
            checked = FIT.fit_manifest(fixture['manifest'])
            self.assertEqual(FIT.passive_method_versions(checked['objectives'])['harmonic_decay'], METHOD)
            for bad in ('harmonic-decay-v1', 'typo'):
                manifest['objectives'][0]['harmonic_method_version'] = bad
                fixture['manifest'].write_text(json.dumps(manifest))
                with self.assertRaisesRegex(FIT.FitError, 'method'):
                    FIT.fit_manifest(fixture['manifest'])
            manifest['objectives'][0].update(kind='passive-decay', harmonic_method_version=METHOD)
            fixture['manifest'].write_text(json.dumps(manifest))
            with self.assertRaisesRegex(FIT.FitError, 'requires a harmonic-decay'):
                FIT.fit_manifest(fixture['manifest'])

    def test_selector_uses_declared_method_on_real_audio(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            fixture = fixtures.make_v1_fixture(root)
            for path in fixture['references'].values():
                write_signal(path)
            experiment = json.loads(fixture['experiment'].read_text())
            jobs = {row['id']: row for row in experiment['jobs']}
            for row in experiment['artifacts']:
                path = FIT.artifact_path(root, row)
                candidate = jobs[row['job_id']]['point_id'] == 2
                write_signal(path, gain=.001 if candidate else 1.0, tau=.4 if candidate else .8)
                row['artifact']['sha256'] = FIT.sha256(path)
                row['file_bytes'] = path.stat().st_size
            fixture['experiment'].write_text(json.dumps(experiment))
            manifest = json.loads(fixture['manifest'].read_text())
            for row in manifest['objectives']:
                row.update(kind='harmonic-decay', fundamental_hz=125, harmonic_count=4)
            manifest['selection'].update(max_candidate_harmonic_mean_error_octaves=.75,
                                         max_candidate_harmonic_maximum_error_octaves=1.5)
            for method in ('harmonic-decay-v1', METHOD):
                for row in manifest['objectives']:
                    row['harmonic_method_version'] = method
                fixture['manifest'].write_text(json.dumps(manifest))
                output = root / (method + '.json')
                run = subprocess.run(fixtures.select_v1_command(fixture, output),
                                     capture_output=True, text=True, timeout=15)
                self.assertTrue(output.exists(), run.stderr)
                result = json.loads(output.read_text())
                self.assertEqual(result['method_versions']['harmonic_decay'], method)
                chosen = next(row for row in result['points'] if row['point_id'] == 2)
                self.assertEqual(chosen['eligible'], method == METHOD)
                if method == METHOD:
                    self.assertEqual(result['chosen_point_id'], 2)
                    self.assertTrue(all(row['valid_harmonic_count'] == 4 for row in chosen['evidence']))


if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
"""Tests for the frozen double-bass physical-dynamics check."""

from __future__ import annotations

import copy
import importlib.util
import math
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = (ROOT / "adapters" / "hlolli_wg_double_bass" /
               "physical_dynamics.py")
SPEC = importlib.util.spec_from_file_location("double_bass_physical_dynamics", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
DYNAMICS = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = DYNAMICS
SPEC.loader.exec_module(DYNAMICS)


class PhysicalDynamicsTest(unittest.TestCase):
    def declaration(self) -> dict:
        return {
            "schema": DYNAMICS.DECLARATION_SCHEMA,
            "schema_version": 1,
            "status": "frozen-before-orchideasol-analysis",
            "source_roster_sha256": "1" * 64,
            "runtime": {"source_sha256": "2" * 64,
                        "model_sha256": "3" * 64},
            "controls": {
                "articulation": 0,
                "force": 0.475,
                "position": 0.12,
                "speed_law": {
                    "reference_dynamic": "mf",
                    "reference_speed_m_per_s": 0.14,
                    "runtime_speed_scale_m_per_s": 0.65,
                },
            },
            "render": {
                "sample_rate_hz": 44100,
                "duration_seconds": 6.0,
                "release_seconds": 5.5,
                "probe_seconds": 4.5,
            },
            "gates": {
                "source_pitch_max_abs_cents": 35.0,
                "model_pitch_max_abs_cents": 35.0,
                "model_dynamic_level_max_error_db": 1.5,
                "dynamic_spectrum_mean_max_error_db": 4.0,
                "dynamic_spectrum_worst_max_error_db": 10.0,
                "model_peak_max": 0.98,
                "contact_scratch_max_fraction": 0.10,
                "require_strict_source_level_order": True,
                "require_zero_contact_failures": True,
            },
        }

    def roster(self) -> dict:
        coefficients = [-52.7699, 0.1928, 4.4722, 0.0101, -0.0051, 0.5130]
        intensities = {"pp": -2.5, "p": -1.5, "mp": -0.5,
                       "mf": 0.5, "f": 1.5, "ff": 2.5}
        rows = []
        for string_index, string_id in enumerate(DYNAMICS.STRINGS):
            for dynamic in DYNAMICS.DYNAMICS:
                rows.append({
                    "bytes": 100,
                    "dynamic": dynamic,
                    "dynamic_intensity": intensities[dynamic],
                    "midi": 28 + 5 * string_index,
                    "publisher_md5_list_match": True,
                    "relative_path": f"audio/{string_id}-{dynamic}.wav",
                    "sha256": format(len(rows) + 1, "064x"),
                    "string_id": string_id,
                    "wave": {"bits_per_sample": 24, "channels": 1,
                             "frame_count": 1000,
                             "sample_rate_hz": 44100},
                })
        return {
            "schema": DYNAMICS.ROSTER_SCHEMA,
            "schema_version": 1,
            "status": "frozen-before-pcm-analysis",
            "orchideasol": {
                "license_spdx": "CC-BY-4.0",
                "dynamic_regression": {
                    "coefficient_order": ["1", "m", "d", "m*d", "m^2", "d^2"],
                    "coefficients": coefficients,
                    "dynamic_intensity": intensities,
                },
                "selected_ordinary_open_string_files": rows,
            },
        }

    def test_validates_frozen_twelve_file_contract(self) -> None:
        rows = DYNAMICS.validate_roster(self.roster())
        self.assertEqual(len(rows), 12)
        DYNAMICS.validate_declaration(self.declaration())
        duplicate = self.roster()
        duplicate["orchideasol"]["selected_ordinary_open_string_files"][1]["dynamic"] = "pp"
        with self.assertRaisesRegex(DYNAMICS.DynamicsError, "duplicate"):
            DYNAMICS.validate_roster(duplicate)

    def test_speed_law_uses_regression_relative_to_mf(self) -> None:
        roster = self.roster()
        declaration = self.declaration()
        speed, control = DYNAMICS.dynamic_speed(roster, declaration, 28.0, "mf")
        self.assertEqual(speed, 0.14)
        self.assertAlmostEqual(control, 0.14 / 0.65, places=14)
        pp_speed, _ = DYNAMICS.dynamic_speed(roster, declaration, 28.0, "pp")
        ff_speed, _ = DYNAMICS.dynamic_speed(roster, declaration, 28.0, "ff")
        self.assertLess(pp_speed, speed)
        self.assertLess(speed, ff_speed)
        target_span = (DYNAMICS.regression_level(roster, 28.0, "ff")
                       - DYNAMICS.regression_level(roster, 28.0, "pp"))
        self.assertAlmostEqual(20.0 * math.log10(ff_speed / pp_speed),
                               target_span, places=12)

    def measured_rows(self) -> list[dict]:
        rows = []
        level = {"pp": -10.0, "mf": 0.0, "ff": 10.0}
        spectrum = {name: -float(index) for index, name in enumerate(DYNAMICS.BAND_NAMES)}
        for string_id in DYNAMICS.STRINGS:
            for dynamic in DYNAMICS.DYNAMICS:
                source_spectrum = {name: value + level[dynamic] * 0.1
                                   for name, value in spectrum.items()}
                rows.append({
                    "string_id": string_id,
                    "dynamic": dynamic,
                    "target_level_db": level[dynamic],
                    "source": {
                        "short_term_max_lufs": level[dynamic],
                        "pitch_valid": True,
                        "pitch_cents": 0.0,
                        "spectrum_db": source_spectrum,
                    },
                    "model": {
                        "short_term_max_lufs": level[dynamic] - 20.0,
                        "pitch_valid": True,
                        "pitch_cents": 0.0,
                        "peak": 0.1,
                        "spectrum_db": dict(source_spectrum),
                    },
                    "contact": {
                        "finite": 1.0,
                        "solver_failures": 0.0,
                        "solver_fallbacks": 0.0,
                        "recoveries": 0.0,
                        "stick_samples": 100.0,
                        "slip_samples": 100.0,
                        "scratch_samples": 0.0,
                        "no_motion_samples": 0.0,
                    },
                })
        return rows

    def test_evaluation_checks_levels_spectra_and_contact(self) -> None:
        summaries, failures = DYNAMICS.evaluate(
            self.measured_rows(), self.declaration())
        self.assertEqual(len(summaries), 4)
        self.assertEqual(failures, [])
        rejected = self.measured_rows()
        rejected[2]["model"]["short_term_max_lufs"] += 2.0
        rejected[2]["contact"]["solver_failures"] = 1.0
        _, failures = DYNAMICS.evaluate(rejected, self.declaration())
        self.assertTrue(any("dynamic-level" in value for value in failures))
        self.assertTrue(any("contact-state" in value for value in failures))


if __name__ == "__main__":
    unittest.main()

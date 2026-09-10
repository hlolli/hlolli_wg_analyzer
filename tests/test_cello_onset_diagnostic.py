#!/usr/bin/env python3
"""Focused synthetic tests for cello onset diagnostic contracts."""
from __future__ import annotations

import importlib.util
import json
import math
from pathlib import Path
import tempfile
import unittest
import wave

MODULE = (Path(__file__).resolve().parents[1] / "adapters" /
          "hlolli_wg_cello" / "onset_diagnostic.py")
SPEC = importlib.util.spec_from_file_location("onset_diagnostic", MODULE)
assert SPEC is not None and SPEC.loader is not None
onset = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(onset)


def csv_text(rows: list[dict[str, str]]) -> str:
    fields = [
        "time_seconds", "onset_strength", "spectral_centroid_hz",
        "spectral_rolloff_85_hz", "spectral_flatness",
        *(f"band_{index}_db" for index in range(10)),
    ]
    lines = [",".join(fields)]
    lines.extend(",".join(row[field] for field in fields) for row in rows)
    return "\n".join(lines) + "\n"


def frame(time: str = "0.1", onset_strength: str = "1",
          high: str = "-20") -> dict[str, str]:
    row = {
        "time_seconds": time,
        "onset_strength": onset_strength,
        "spectral_centroid_hz": "1000",
        "spectral_rolloff_85_hz": "2000",
        "spectral_flatness": "0.5",
    }
    row.update({f"band_{index}_db": (high if index >= 7 else "0")
                for index in range(10)})
    return row


PITCH_PATH = Path("/staged/raw.wav")
PITCH_EXPECTED_HZ = 65.0
PITCH_RATE = 44_100
PITCH_FRAMES = 529_200
PITCH_METHOD_WINDOW = max(
    2048, math.ceil(6.0 * PITCH_RATE / PITCH_EXPECTED_HZ))
PITCH_METHOD_HOP = PITCH_METHOD_WINDOW // 2
PITCH_METHOD_WINDOW_COUNT = (
    1 + (PITCH_FRAMES - PITCH_METHOD_WINDOW) // PITCH_METHOD_HOP)


def valid_pitch_report() -> dict[str, object]:
    accepted = 8
    return {
        "schema": "hwa-isolated-note", "schema_version": 1,
        "command": "isolated-note", "method": "isolated-note-1",
        "path": str(PITCH_PATH), "expected_hz": PITCH_EXPECTED_HZ,
        "requested_mask": 1, "valid_mask": 1, "rejection_mask": 0,
        "requested_metrics": ["pitch"], "valid_metrics": ["pitch"],
        "rejections": [],
        "format": {"container": "riff", "encoding": "pcm",
                   "sample_rate_hz": PITCH_RATE, "channels": 2,
                   "bits_per_sample": 24,
                   "valid_bits_per_sample": 24, "frames": PITCH_FRAMES},
        "pitch": {
            "valid": True, "hz": 65.1,
            "cents": 1200.0 * math.log2(65.1 / PITCH_EXPECTED_HZ),
            "confidence": 0.9, "coverage": 0.8,
            "window_count": 10, "accepted_window_count": accepted,
            "start_sample": 0,
            "end_sample": PITCH_METHOD_WINDOW +
            (accepted - 1) * PITCH_METHOD_HOP,
        },
        "decay": {"valid": False, "slope_db_per_second": 0.0,
                  "t60_seconds": 0.0, "support_seconds": 0.0,
                  "dynamic_range_db": 0.0, "residual_db": 0.0,
                  "floor_dbfs": 0.0, "point_count": 0,
                  "start_sample": 0, "end_sample": 0},
        "work": {"peak_bytes": 4_258_176,
                 "evaluations": 6_577_471},
    }


class OnsetDiagnosticTests(unittest.TestCase):
    def test_harmonic_fit_and_h10_notch(self) -> None:
        rate = 8000
        fundamental = 100.0
        amplitudes = {number: 0.05 for number in range(1, 12)}
        amplitudes[10] = 0.005
        samples = []
        for index in range(rate):
            value = sum(
                amplitude * math.cos(
                    2.0 * math.pi * fundamental * number * index / rate)
                for number, amplitude in amplitudes.items())
            integer = round(value * onset.PCM24_SCALE)
            samples.append((integer, integer))
        rows = onset.harmonic_levels(samples, 0, rate, fundamental)
        self.assertAlmostEqual(float(rows[0]["amplitude"]), 0.05, places=4)
        self.assertAlmostEqual(onset.h10_notch_db(rows), -20.0, places=1)

    def test_ratios_are_scalar_invariant_before_quantization(self) -> None:
        samples = [(0, 0)] * 400
        for index in range(10):
            samples[index] = ((-1 if index % 2 else 1) * 1000,) * 2
        for index in range(20, 300):
            samples[index] = (100,) * 2
        result = onset.ratio_invariance(samples, 0, 1000, 3.14159)
        self.assertLessEqual(result["maximum_absolute_difference"], 1e-12)
        self.assertAlmostEqual(result["before"]["transient_to_tail_db"], 20)

    def test_silent_windows_rejected(self) -> None:
        with self.assertRaises(onset.DiagnosticError):
            onset.onset_ratios([(0, 0)] * 400, 0, 1000)
        with self.assertRaises(onset.DiagnosticError):
            onset.tail_rms([(0, 0)] * 400, 0, 1000)

    def test_pcm_facts_report_rails_and_each_channel(self) -> None:
        facts = onset.pcm24_facts([(1, 0), (0, onset.PCM24_MAX)])
        self.assertTrue(facts["left_nonzero"])
        self.assertTrue(facts["right_nonzero"])
        self.assertEqual(facts["rail_frame_count"], 1)

    def test_six_channel_dry_requires_exact_zero_auxiliary(self) -> None:
        dry, facts = onset.extract_six_channel_dry([(1, 2, 0, 0, 0, 0)])
        self.assertEqual(dry, [(1, 2)])
        self.assertTrue(facts["body_path_absent"])
        with self.assertRaises(onset.DiagnosticError):
            onset.extract_six_channel_dry([(1, 2, 0, 0, 1, 0)])

    def test_analyzer_tie_uses_earliest(self) -> None:
        before_onset = frame("0.091", "0")
        for name in (
                "spectral_centroid_hz", "spectral_rolloff_85_hz",
                "spectral_flatness",
                *(f"band_{index}_db" for index in range(10))):
            before_onset[name] = ""
        first = frame("0.100", "1")
        second = frame("0.101", "1")
        second["spectral_centroid_hz"] = "3000"
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "frames.csv"
            path.write_text(
                csv_text([before_onset, first, second]), encoding="utf-8")
            selected = onset.selected_analyzer_frame(path)
        self.assertEqual(selected["time_seconds"], 0.1)
        self.assertEqual(selected["centroid_hz"], 1000)

    def test_all_floor_high_band_is_censored_not_numeric(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "frames.csv"
            path.write_text(csv_text([frame(high="-300")]), encoding="utf-8")
            high = onset.selected_analyzer_frame(path)["four_khz_plus"]
        self.assertIsNone(high["db_fraction"])
        self.assertTrue(high["high_band_censored"])
        self.assertEqual(high["status"], "censored-at-analyzer-floor")
        affected = onset.high_band_contrast(
            high, {"db_fraction": -12.0, "high_band_censored": False})
        self.assertEqual(affected, {"value": None, "conclusive": False,
                                    "high_band_censored": True})

    def test_partial_floor_is_marked_floor_limited(self) -> None:
        row = frame(high="-300")
        row["band_8_db"] = "-30"
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "frames.csv"
            path.write_text(csv_text([row]), encoding="utf-8")
            high = onset.selected_analyzer_frame(path)["four_khz_plus"]
        self.assertIsInstance(high["db_fraction"], float)
        self.assertEqual(high["status"], "floor-limited-estimate")

    def test_malformed_and_nonfinite_csv_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            missing = Path(folder) / "missing.csv"
            missing.write_text("time_seconds,onset_strength\n0.1,1\n")
            with self.assertRaises(onset.DiagnosticError):
                onset.selected_analyzer_frame(missing)
            for name, row in (
                    ("nonfinite", frame(time="nan")),
                    ("empty", frame()),
                    ("below-floor", frame(high="-301"))):
                if name == "empty":
                    row["band_3_db"] = ""
                invalid = Path(folder) / f"{name}.csv"
                invalid.write_text(csv_text([row]))
                with self.assertRaises(onset.DiagnosticError):
                    onset.selected_analyzer_frame(invalid)

    def test_pcm16_quantized_boundaries(self) -> None:
        payload = onset.encode_pcm16(
            [(onset.PCM24_MAX, onset.PCM24_MIN)], 2.0)
        self.assertEqual(payload, b"\xff\x7f\x00\x80")
        self.assertEqual(onset.decode_pcm16_stereo(payload),
                         [(32767 << 8, -32768 << 8)])
        with self.assertRaises(onset.DiagnosticError):
            onset.decode_pcm16_stereo(b"\0")

    def test_abba_has_exact_order_frame_count_and_one_middle_silence(self) -> None:
        item_a = b"A" * 8
        item_b = b"B" * 8
        silence = b"\0" * 12
        payload = onset.assemble_abba(
            item_a, item_b, item_b, item_a, silence)
        self.assertEqual(payload, item_a + item_b + silence + item_b + item_a)
        self.assertEqual(onset.listening_layout_facts(payload, 2, 3), {
            "order": ["forward_item_1", "forward_item_2", "silence",
                      "reverse_item_1", "reverse_item_2"],
            "segment_frames": 2, "silence_frames": 3,
            "frame_count": 11, "gap_count": 1,
        })
        with self.assertRaises(onset.DiagnosticError):
            onset.listening_layout_facts(payload + b"\0" * 4, 2, 3)

    def test_command_template_must_match_before_expansion(self) -> None:
        frozen = ["{python}", "-I", "-B", "{script}", "--check"]
        context = {"python": "/nix/store/python", "script": "/stage/tool.py"}
        self.assertEqual(onset.expand_exact_command(frozen, frozen, context),
                         ["/nix/store/python", "-I", "-B",
                          "/stage/tool.py", "--check"])
        with self.assertRaises(onset.DiagnosticError):
            onset.expand_exact_command(frozen[:-1], frozen, context)
        with self.assertRaises(onset.DiagnosticError):
            onset.expand_exact_command(frozen, frozen, {"python": "python"})

    def test_pitch_command_has_frozen_pre_execution_work_limits(self) -> None:
        frozen = [
            "{analyzer}", "--json", "isolated-note", "{raw_wave}",
            "--expected-hz", "{frequency_17g}", "--metrics", "pitch",
            "--max-work-bytes", str(onset.MAX_PITCH_PEAK_BYTES),
            "--max-note-evaluations", str(onset.MAX_PITCH_EVALUATIONS),
        ]
        context = {
            "analyzer": "/stage/hlolli-wg-analyzer",
            "raw_wave": "/run/raw.wav", "frequency_17g": "65.0",
        }
        self.assertEqual(
            onset.expand_exact_command(frozen, frozen, context),
            ["/stage/hlolli-wg-analyzer", "--json", "isolated-note",
             "/run/raw.wav", "--expected-hz", "65.0", "--metrics",
             "pitch", "--max-work-bytes", "8388608",
             "--max-note-evaluations", "10000000"],
        )
        for omitted in ("--max-work-bytes", "--max-note-evaluations"):
            with self.subTest(omitted=omitted):
                candidate = frozen[:]
                index = candidate.index(omitted)
                del candidate[index:index + 2]
                with self.assertRaises(onset.DiagnosticError):
                    onset.expand_exact_command(candidate, frozen, context)

    def test_token_preserving_model_patch(self) -> None:
        source = (
            b'{\n  "other": 0.0025,\n  "exciters": {\n'
            b'    "pizzicato_right": {\n'
            b'      "force_scale_newtons": 0.36,\n'
            b'      "noise_gain": 0.0025,\n'
            b'      "release_min_seconds": 0.00018,\n'
            b'      "release_range_seconds": 0.00072\n    }\n  }\n}\n')
        patched, facts = onset.patch_pizzicato_right_model(
            source, "0.00072", "0.00288", "0.0")
        self.assertIn(b'  "other": 0.0025,', patched)
        self.assertEqual(len(facts), 3)
        self.assertTrue(all(fact["changed"] for fact in facts))
        self.assertEqual(patched.replace(b"0.00072", b"0.00018", 1)
                         .replace(b"0.00288", b"0.00072", 1)
                         .replace(b'"noise_gain": 0.0',
                                  b'"noise_gain": 0.0025', 1), source)
        before = json.loads(source)
        after = json.loads(patched)
        before["exciters"]["pizzicato_right"].update(
            after["exciters"]["pizzicato_right"])
        self.assertEqual(before, after)
        current, current_facts = onset.patch_pizzicato_right_model(
            source, "0.00018", "0.00072", "0.0025")
        self.assertEqual(current, source)
        self.assertFalse(any(fact["changed"] for fact in current_facts))

    def test_mono_pcm16_reference_is_scaled_and_duplicated(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "reference.wav"
            with wave.open(str(path), "wb") as output:
                output.setnchannels(1)
                output.setsampwidth(2)
                output.setframerate(8000)
                output.writeframes((123).to_bytes(2, "little", signed=True))
            self.assertEqual(onset.read_pcm_to_stereo24(path, 8000),
                             [(123 << 8, 123 << 8)])

    def test_pitch_report_requires_full_contract(self) -> None:
        report = valid_pitch_report()
        self.assertEqual(onset.validate_pitch_report(
            report, PITCH_PATH, PITCH_EXPECTED_HZ, PITCH_RATE,
            PITCH_FRAMES), 65.1)
        report["method"] = "changed"
        with self.assertRaises(onset.DiagnosticError):
            onset.validate_pitch_report(
                report, PITCH_PATH, PITCH_EXPECTED_HZ, PITCH_RATE,
                PITCH_FRAMES)
        report["method"] = "isolated-note-1"
        report["pitch"]["hz"] = math.inf
        with self.assertRaises(onset.DiagnosticError):
            onset.validate_pitch_report(
                report, PITCH_PATH, PITCH_EXPECTED_HZ, PITCH_RATE,
                PITCH_FRAMES)

    def test_pitch_report_rejects_each_method_invariant(self) -> None:
        excessive_active_windows = PITCH_METHOD_WINDOW_COUNT + 1
        enough_accepted = math.ceil(
            onset.MIN_PITCH_COVERAGE * excessive_active_windows)
        mutations = (
            ("fewer than two accepted windows", lambda row: row["pitch"].update(
                window_count=2, accepted_window_count=1, coverage=0.5,
                start_sample=0, end_sample=PITCH_METHOD_WINDOW)),
            ("confidence below method floor", lambda row: row["pitch"].update(
                confidence=onset.MIN_PITCH_CONFIDENCE - 1.0e-12)),
            ("coverage below method floor", lambda row: row["pitch"].update(
                window_count=6, accepted_window_count=2, coverage=1.0 / 3.0,
                start_sample=0,
                end_sample=PITCH_METHOD_WINDOW + PITCH_METHOD_HOP)),
            ("start off method hop grid", lambda row: row["pitch"].update(
                start_sample=1)),
            ("end off method window grid", lambda row: row["pitch"].update(
                end_sample=row["pitch"]["end_sample"] + 1)),
            ("accepted count exceeds support span", lambda row: row["pitch"].update(
                window_count=3, accepted_window_count=3, coverage=1.0,
                start_sample=0,
                end_sample=PITCH_METHOD_WINDOW + PITCH_METHOD_HOP)),
            ("active windows exceed input", lambda row: row["pitch"].update(
                window_count=excessive_active_windows,
                accepted_window_count=enough_accepted,
                coverage=enough_accepted / excessive_active_windows,
                start_sample=0,
                end_sample=PITCH_METHOD_WINDOW +
                (enough_accepted - 1) * PITCH_METHOD_HOP)),
            ("pitch end past input", lambda row: row["pitch"].update(
                end_sample=PITCH_METHOD_WINDOW +
                PITCH_METHOD_WINDOW_COUNT * PITCH_METHOD_HOP)),
            ("sub-tolerance coverage disagrees with counts",
             lambda row: row["pitch"].update(
                 coverage=0.8000000000005001)),
            ("pitch exceeds positive method boundary",
             lambda row: row["pitch"].update(
                 hz=PITCH_EXPECTED_HZ * math.pow(2.0, 81.0 / 1200.0),
                 cents=81.0)),
            ("pitch exceeds negative method boundary",
             lambda row: row["pitch"].update(
                 hz=PITCH_EXPECTED_HZ * math.pow(2.0, -81.0 / 1200.0),
                 cents=-81.0)),
        )
        for label, mutate in mutations:
            with self.subTest(label=label):
                candidate = json.loads(json.dumps(valid_pitch_report()))
                mutate(candidate)
                with self.assertRaises(onset.DiagnosticError):
                    onset.validate_pitch_report(
                        candidate, PITCH_PATH, PITCH_EXPECTED_HZ, PITCH_RATE,
                        PITCH_FRAMES)

    def test_pitch_method_boundary_is_inclusive(self) -> None:
        for cents in (-80.0, 80.0):
            with self.subTest(cents=cents):
                candidate = valid_pitch_report()
                candidate["pitch"]["hz"] = (
                    PITCH_EXPECTED_HZ * math.pow(2.0, cents / 1200.0))
                candidate["pitch"]["cents"] = cents
                self.assertGreater(
                    onset.validate_pitch_report(
                        candidate, PITCH_PATH, PITCH_EXPECTED_HZ,
                        PITCH_RATE, PITCH_FRAMES), 0.0)

    def test_pitch_only_report_rejects_valid_or_nonzero_decay(self) -> None:
        valid_decay = valid_pitch_report()
        valid_decay["decay"].update({
            "valid": True, "slope_db_per_second": -60.0,
            "t60_seconds": 1.0, "support_seconds": 100 / PITCH_RATE,
            "dynamic_range_db": 10.0, "residual_db": 0.1,
            "floor_dbfs": -100.0, "point_count": 2,
            "start_sample": 0, "end_sample": 100,
        })
        with self.assertRaises(onset.DiagnosticError):
            onset.validate_pitch_report(
                valid_decay, PITCH_PATH, PITCH_EXPECTED_HZ, PITCH_RATE,
                PITCH_FRAMES)
        fields = (
            "slope_db_per_second", "t60_seconds", "support_seconds",
            "dynamic_range_db", "residual_db", "floor_dbfs", "point_count",
            "start_sample", "end_sample",
        )
        for field in fields:
            with self.subTest(field=field):
                candidate = valid_pitch_report()
                candidate["decay"][field] = (
                    1 if field in {"point_count", "start_sample", "end_sample"}
                    else 1.0)
                with self.assertRaises(onset.DiagnosticError):
                    onset.validate_pitch_report(
                        candidate, PITCH_PATH, PITCH_EXPECTED_HZ,
                        PITCH_RATE, PITCH_FRAMES)

    def test_pitch_report_rejects_excess_post_execution_work(self) -> None:
        mutations = (
            ("peak_bytes", onset.MAX_PITCH_PEAK_BYTES + 1),
            ("evaluations", onset.MAX_PITCH_EVALUATIONS + 1),
        )
        for field, value in mutations:
            with self.subTest(field=field):
                candidate = valid_pitch_report()
                candidate["work"][field] = value
                with self.assertRaises(onset.DiagnosticError):
                    onset.validate_pitch_report(
                        candidate, PITCH_PATH, PITCH_EXPECTED_HZ,
                        PITCH_RATE, PITCH_FRAMES)


if __name__ == "__main__":
    unittest.main()

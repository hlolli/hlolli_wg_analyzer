#!/usr/bin/env python3
"""Note-phase contract and acoustic fixtures through the public CLI."""
import argparse
import hashlib
import copy
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock
import wave

ANALYZER = None
IOWA_RECORDING = None


def load_fit_tests():
    path = Path(__file__).with_name("test_instrument_fit.py")
    spec = importlib.util.spec_from_file_location("phase_fit_tests", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def recording(path, second=False, silence=False, sustain_gain=0.3,
              tail_seconds=1.4, rate=16000):
    raw = bytearray()
    for i in range(round((1.2 + tail_seconds) * rate)):
        t = i / rate
        envelope = min(1.0, max(0.0, (t - 0.2) / 0.08))
        if t > 1.2:
            envelope *= math.exp(-(t - 1.2) / 0.18)
        value = sustain_gain * envelope * (
            math.sin(math.tau * 220.0 * t) +
            0.3 * math.sin(math.tau * 660.0 * t))
        if second and t >= 1.45:
            value += 0.45 * math.exp(-(t - 1.45) / 0.2) * math.sin(math.tau * 330.0 * t)
        if silence:
            value = 0.0
        raw.extend(round(value * 32767).to_bytes(2, "little", signed=True))
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(1)
        stream.setsampwidth(2)
        stream.setframerate(rate)
        stream.writeframes(raw)


class NotePhasesTests(unittest.TestCase):
    def run_report(self, path, *options, expected=0):
        process = subprocess.run([
            str(ANALYZER), "--json", "note-phases", str(path),
            "--note-start-sample", "3200", "--note-end-sample", "19200",
            *options], capture_output=True, text=True)
        self.assertEqual(process.returncode, expected, process.stderr)
        return json.loads(process.stdout) if expected == 0 else process

    def test_phases_cover_a_note_with_known_envelope(self):
        with tempfile.TemporaryDirectory(prefix="hwa phase note ") as directory:
            path = Path(directory) / "note.wav"
            recording(path)
            report = self.run_report(path)
            self.assertEqual(report["method"], "note-phases-1")
            self.assertEqual(report["audio_sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
            self.assertEqual(report["sample_rate_hz"], 16000)
            self.assertIsNone(report["next_onset_sample"])
            phases = report["phases"]
            self.assertEqual([p["phase"] for p in phases],
                             ["attack", "sustain", "release", "clean-tail"])
            self.assertLess(abs(phases[0]["start_sample"] - 3200), 2048)
            self.assertLess(abs(phases[2]["start_sample"] - 19200), 3072)
            for i, phase in enumerate(phases):
                self.assertEqual(phase["status"], "valid", phase)
                self.assertGreater(phase["end_sample"], phase["start_sample"])
                self.assertEqual(phase["duration_seconds"],
                    (phase["end_sample"] - phase["start_sample"]) / 16000)
                if i:
                    self.assertEqual(phase["start_sample"], phases[i-1]["end_sample"])
            self.assertLess(phases[2]["metrics"]["level_slope_db_per_second"]["value"], 0.0)
            self.assertGreater(phases[1]["metrics"]["centroid_hz"]["value"], 200.0)

    def test_next_note_rejects_tail_and_exposes_no_score(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "interrupted.wav"
            recording(path, second=True)
            report = self.run_report(path)
            self.assertIsNotNone(report["next_onset_sample"])
            tail = report["phases"][3]
            self.assertEqual(tail["status"], "interrupted")
            self.assertTrue(all(v["value"] is None for v in tail["metrics"].values()))

    def test_silence_and_truncated_tail_do_not_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "silence.wav"
            recording(path, silence=True)
            report = self.run_report(path)
            self.assertTrue(all(p["status"] != "valid" for p in report["phases"]))
            recording(path, tail_seconds=0.35)
            report = self.run_report(path)
            self.assertEqual(report["phases"][3]["status"], "truncated")

    def test_decode_block_invariance_and_resource_limits(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "note.wav"
            recording(path)
            first = self.run_report(path, "--block-frames", "97")
            second = self.run_report(path, "--block-frames", "4096")
            self.assertEqual(first, second)
            self.run_report(path, "--max-work-bytes", "1", expected=1)
            self.run_report(path, "--max-boundary-evaluations", "1", expected=1)
            self.run_report(path, "--max-measurements", "1", expected=1)
            self.run_report(path, "--note-end-sample", "20000", expected=2)
            self.run_report(path, "--output", str(Path(directory) / "unused"), expected=2)
            self.run_report(path, "--frame-size", "63", expected=1)

    def test_manifest_rejects_bad_phase_specs_and_v2(self):
        fixtures = load_fit_tests()
        with tempfile.TemporaryDirectory() as directory:
            fixture = fixtures.make_v1_fixture(Path(directory))
            original = json.loads(fixture["manifest"].read_text())
            original["objectives"][0].update(
                kind="note-phase", phase="sustain", metric="rms_dbfs",
                reference_span=[3200, 19200], model_span=[3200, 19200])
            for change in ("phase", "metric", "reversed", "bool", "v2"):
                manifest = copy.deepcopy(original)
                row = manifest["objectives"][0]
                if change in ("phase", "metric"):
                    row[change] = "invalid"
                elif change == "reversed":
                    row["model_span"] = [19200, 3200]
                elif change == "bool":
                    row["reference_span"] = [False, 19200]
                else:
                    manifest["schema_version"] = 2
                fixture["manifest"].write_text(json.dumps(manifest))
                with self.subTest(change=change):
                    with self.assertRaises(fixtures.MODULE.FitError):
                        fixtures.MODULE.fit_manifest(fixture["manifest"])

    def test_checked_evidence_rejects_changed_contracts(self):
        evidence = load_fit_tests().MODULE.analyzer_evidence_module()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "note.wav"
            recording(path)
            checks = evidence.AnalyzerEvidence(ANALYZER, {
                "note": (path, hashlib.sha256(path.read_bytes()).hexdigest())})
            original = checks.note_phases("note", 3200, 19200)
            for change in ("clock", "duration", "rejected", "unit", "nonfinite"):
                report = copy.deepcopy(original.report)
                phase = report["phases"][1]
                if change == "clock":
                    report["boundary_hop_size"] = 64
                elif change == "duration":
                    phase["duration_seconds"] += 1
                elif change == "rejected":
                    phase["status"] = "no-signal"
                elif change == "unit":
                    phase["metrics"]["rms_dbfs"]["unit"] = "Hz"
                else:
                    phase["metrics"]["rms_dbfs"]["value"] = float("nan")
                with self.subTest(change=change), mock.patch.object(
                        checks, "_run", return_value=report):
                    with self.assertRaises(evidence.EvidenceError):
                        checks.note_phases("note", 3200, 19200)

    def test_fit_measures_gain_and_rejects_interrupted_tail(self):
        fit = load_fit_tests().MODULE
        with tempfile.TemporaryDirectory() as directory:
            reference = Path(directory) / "reference.wav"
            model = Path(directory) / "model.wav"
            recording(reference)
            recording(model, sustain_gain=0.15)
            objective = {"phase": "sustain", "metric": "rms_dbfs",
                         "reference_span": [3200, 19200], "model_span": [3200, 19200]}
            def measure():
                return fit.run_note_phase(ANALYZER, reference, model, objective,
                    fit.sha256(ANALYZER), fit.sha256(reference), fit.sha256(model))
            result = measure()
            self.assertAlmostEqual(result["absolute_delta"], 20 * math.log10(2), places=2)
            objective.update(phase="clean-tail", metric="level_slope_db_per_second")
            recording(model, second=True)
            with self.assertRaisesRegex(fit.FitError, "interrupted"):
                measure()

    def test_v1_select_uses_phase_loss(self):
        fixtures = load_fit_tests()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = fixtures.make_v1_fixture(root)
            fixture["analyzer"] = ANALYZER
            manifest = json.loads(fixture["manifest"].read_text())
            for row in manifest["objectives"]:
                row.update(kind="note-phase", phase="sustain", metric="rms_dbfs",
                           reference_span=[3200, 19200], model_span=[3200, 19200])
            fixture["manifest"].write_text(json.dumps(manifest))
            for path in fixture["references"].values():
                recording(path)
            experiment = json.loads(fixture["experiment"].read_text())
            for artifact in experiment["artifacts"]:
                path = root / artifact["artifact"]["path"]
                job = next(row for row in experiment["jobs"] if row["id"] == artifact["job_id"])
                recording(path, sustain_gain=0.15 if job["point_id"] == 1 else 0.3)
                artifact["artifact"]["sha256"] = fixtures.MODULE.sha256(path)
                artifact["file_bytes"] = path.stat().st_size
            fixture["experiment"].write_text(json.dumps(experiment))
            output = root / "result.json"
            process = subprocess.run(fixtures.select_v1_command(fixture, output),
                                     capture_output=True, text=True)
            self.assertEqual(process.returncode, 0, process.stderr)
            result = json.loads(output.read_text())
            self.assertEqual(result["chosen_point_id"], 2)

    def test_iowa_recording_tail_limit(self):
        if IOWA_RECORDING is None:
            self.skipTest("pass --iowa-recording to check the real recording")
        self.assertEqual(hashlib.sha256(IOWA_RECORDING.read_bytes()).hexdigest(),
            "06127fea47be15bbaab33c0a0b78b5bfb0190a491cadaebaaf4fc856a95edc15")
        reports = []
        for limit in ("1.5", "4"):
            report = json.loads(subprocess.check_output([
                str(ANALYZER), "note-phases", str(IOWA_RECORDING),
                "--note-start-sample", "44100", "--note-end-sample", "88200",
                "--tail-limit", limit], text=True))
            reports.append(report)
            self.assertEqual(report["sample_rate_hz"], 44100)
            self.assertIsNone(report["next_onset_sample"])
            self.assertEqual(report["phases"][0]["status"], "valid")
        self.assertEqual(reports[0]["phases"][3]["status"], "truncated")
        self.assertEqual(reports[1]["phases"][3]["status"], "valid")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--analyzer", required=True)
    parser.add_argument("--iowa-recording", type=Path)
    options, remaining = parser.parse_known_args()
    ANALYZER = Path(options.analyzer).resolve()
    IOWA_RECORDING = options.iowa_recording
    unittest.main(argv=[__file__, *remaining])

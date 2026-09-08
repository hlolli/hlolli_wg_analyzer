#!/usr/bin/env python3
"""Note-phase contract and acoustic fixtures through the public CLI."""
import argparse
import hashlib
import copy
import importlib.util
import json
import math
from pathlib import Path
import shutil
import struct
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
              tail_seconds=1.4, rate=16000, decay_seconds=0.18,
              modulation_db=0.0):
    raw = bytearray()
    for i in range(round((1.2 + tail_seconds) * rate)):
        t = i / rate
        envelope = min(1.0, max(0.0, (t - 0.2) / 0.08))
        if t > 1.2:
            envelope *= math.exp(-(t - 1.2) / decay_seconds)
        value = sustain_gain * envelope * (
            math.sin(math.tau * 220.0 * t) +
            0.3 * math.sin(math.tau * 660.0 * t))
        if 0.4 <= t <= 1.1:
            value *= 10 ** (modulation_db * math.sin(math.tau * 5 * (t - 0.4)) / 20)
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

    def test_attack_window_contains_the_known_rise(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "note.wav"
            recording(path)
            report = self.run_report(path)
            attack = report["phases"][0]
            self.assertGreater(attack["end_sample"], 3200,
                             "attack ends before the first nonzero sample")
            self.assertGreaterEqual(attack["end_sample"], 4480,
                                    "fallback cuts off the known 80 ms rise")
            self.assertEqual(report["phases"][1]["boundary_confidence"], 0.0)
            self.assertEqual(attack["metrics"]["rms_dbfs"]["status"], "valid")

    def test_envelope_metrics_match_samples_and_preserve_basic_output(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "note.wav"
            recording(path)
            basic = self.run_report(path)
            extended = self.run_report(path, "--phase-envelope")
            self.assertEqual(extended.pop("envelope_method"), "note-phase-envelope-1")
            bins = extended.pop("attack_envelope_bins")
            self.assertEqual(bins, 16)
            attack = extended["phases"][0]
            first, last = attack["start_sample"], attack["end_sample"]
            with wave.open(str(path)) as audio:
                samples = struct.unpack("<" + str(audio.getnframes()) + "h",
                                        audio.readframes(audio.getnframes()))
            rms = []
            for index in range(bins):
                span = samples[first + (last-first)*index//bins:
                               first + (last-first)*(index+1)//bins]
                rms.append(math.sqrt(sum(x*x for x in span) / len(span)) / 32768)
            peak = max(rms)
            rises = []
            for percent in (10, 50, 90):
                index = next(i for i, value in enumerate(rms) if value >= peak * percent/100)
                expected = (index + 0.5) * (last-first) / bins / 16000
                value = attack["metrics"]["rise_" + str(percent) + "_seconds"]
                self.assertEqual(value["status"], "valid")
                self.assertAlmostEqual(value["value"], expected, places=12)
                rises.append(value["value"])
            self.assertAlmostEqual(attack["metrics"]["attack_slope_db_per_second"]["value"],
                                   20 * math.log10(9) / (rises[2] - rises[0]), places=8)
            overshoot = 20 * math.log10(peak) - sum(20 * math.log10(v) for v in rms[-4:])/4
            self.assertAlmostEqual(attack["metrics"]["attack_overshoot_db"]["value"],
                                   overshoot, places=8)
            for before, after in zip(basic["phases"], extended["phases"]):
                for name in set(after["metrics"]) - set(before["metrics"]):
                    del after["metrics"][name]
            self.assertEqual(extended, basic)

    def test_envelope_rejections_and_variation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "note.wav"
            recording(path)
            steady = self.run_report(path, "--phase-envelope")
            recording(path, modulation_db=4)
            varying = self.run_report(path, "--phase-envelope")
            key = "level_modulation_spread_db"
            self.assertGreater(varying["phases"][1]["metrics"][key]["value"],
                               steady["phases"][1]["metrics"][key]["value"] + 0.5)
            for phase in varying["phases"]:
                metrics = phase["metrics"]
                if metrics["crest_db"]["status"] == "valid":
                    self.assertAlmostEqual(metrics["crest_db"]["value"],
                        metrics["peak_dbfs"]["value"] - metrics["rms_dbfs"]["value"], places=8)
                if phase["phase"] != "attack":
                    self.assertNotIn("rise_90_seconds", metrics)
            recording(path, second=True)
            interrupted = self.run_report(path, "--phase-envelope")
            self.assertTrue(all(v["value"] is None for v in interrupted["phases"][3]["metrics"].values()))
            self.run_report(path, "--phase-envelope", "--phase-envelope", expected=2)
            process = subprocess.run([str(ANALYZER), "inspect", str(path), "--phase-envelope"],
                                     capture_output=True)
            self.assertEqual(process.returncode, 2)
            recording(path, silence=True)
            silent = self.run_report(path, "--phase-envelope")
            self.assertTrue(all(v["value"] is None for phase in silent["phases"]
                                for v in phase["metrics"].values()))
            recording(path, tail_seconds=0.35)
            truncated = self.run_report(path, "--phase-envelope")["phases"][3]
            self.assertEqual(truncated["status"], "truncated")
            self.assertTrue(all(v["value"] is None for v in truncated["metrics"].values()))

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
            self.assertEqual(
                self.run_report(path, "--phase-envelope", "--block-frames", "97"),
                self.run_report(path, "--phase-envelope", "--block-frames", "4096"))
            for limit in ("--max-work-bytes", "--max-boundary-evaluations", "--max-measurements"):
                self.run_report(path, "--phase-envelope", limit, "1", expected=1)
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
            for change in ("phase", "metric", "reversed", "bool", "v2",
                           "settings-null", "settings-range", "settings-typo",
                           "settings-other-kind", "attack-only"):
                manifest = copy.deepcopy(original)
                row = manifest["objectives"][0]
                if change in ("phase", "metric"):
                    row[change] = "invalid"
                elif change == "reversed":
                    row["model_span"] = [19200, 3200]
                elif change == "bool":
                    row["reference_span"] = [False, 19200]
                elif change == "v2":
                    manifest["schema_version"] = 2
                elif change == "settings-null":
                    row["phase_options"] = None
                elif change == "settings-range":
                    row["phase_options"] = {"tail_limit_seconds": 11}
                elif change == "settings-typo":
                    row["phase_options"] = {"tail_limit": 4}
                elif change == "attack-only":
                    row["metric"] = "rise_90_seconds"
                else:
                    row.update(kind="body-envelope", phase_options={})
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

    def test_envelope_evidence_cache_and_fit(self):
        fit = load_fit_tests().MODULE
        evidence = fit.analyzer_evidence_module()
        with tempfile.TemporaryDirectory() as directory:
            reference = Path(directory) / "reference.wav"
            model = Path(directory) / "model.wav"
            recording(reference)
            recording(model, modulation_db=4)
            checks = evidence.AnalyzerEvidence(ANALYZER, {
                "note": (reference, fit.sha256(reference))})
            cache = evidence.NotePhaseCache()
            with mock.patch.object(checks, "_run", wraps=checks._run) as run:
                basic = cache.note_phases(checks, "note", 3200, 19200)
                extra = cache.note_phases(checks, "note", 3200, 19200, envelope=True)
                self.assertEqual(cache.note_phases(checks, "note", 3200, 19200), basic)
                self.assertEqual(cache.note_phases(checks, "note", 3200, 19200,
                                                  envelope=True), extra)
                self.assertEqual(run.call_count, 2)
            for change in ("method", "bins", "crest", "negative", "rise", "slope"):
                report = copy.deepcopy(extra.report)
                metrics = report["phases"][0]["metrics"]
                if change == "method":
                    report["envelope_method"] = "invented"
                elif change == "bins":
                    report["attack_envelope_bins"] = 32
                elif change == "crest":
                    metrics["crest_db"]["value"] += 1
                elif change == "negative":
                    metrics["level_modulation_spread_db"]["value"] = -1
                elif change == "rise":
                    metrics["rise_10_seconds"]["value"] = 10
                else:
                    metrics["attack_slope_db_per_second"]["value"] += 1
                with self.subTest(change=change), mock.patch.object(checks, "_run", return_value=report):
                    with self.assertRaises(evidence.EvidenceError):
                        checks.note_phases("note", 3200, 19200, envelope=True)
            for runner in (checks, cache):
                args = ("note", 3200, 19200) if runner is checks else (checks, "note", 3200, 19200)
                with self.assertRaises(evidence.EvidenceError):
                    runner.note_phases(*args, envelope=1)
            objective = {"kind": "note-phase", "phase": "sustain",
                         "metric": "level_modulation_spread_db",
                         "reference_span": [3200, 19200], "model_span": [3200, 19200]}
            def measure():
                return fit.run_note_phase(ANALYZER, reference, model, objective,
                    fit.sha256(ANALYZER), fit.sha256(reference), fit.sha256(model), cache=cache)
            self.assertGreater(measure()["absolute_delta"], 0.5)
            objective.update(phase="attack", metric="rise_90_seconds")
            self.assertEqual(measure()["absolute_delta"], 0)
            self.assertEqual(fit.passive_method_versions([objective]), {
                "note_phases": "note-phases-1", "note_phase_envelope": "note-phase-envelope-1"})

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
                recording(path, tail_seconds=4, decay_seconds=0.6)
            experiment = json.loads(fixture["experiment"].read_text())
            for artifact in experiment["artifacts"]:
                path = root / artifact["artifact"]["path"]
                job = next(row for row in experiment["jobs"] if row["id"] == artifact["job_id"])
                recording(path, sustain_gain=0.15 if job["point_id"] == 1 else 0.3,
                          tail_seconds=4, decay_seconds=0.6)
                artifact["artifact"]["sha256"] = fixtures.MODULE.sha256(path)
                artifact["file_bytes"] = path.stat().st_size
            fixture["experiment"].write_text(json.dumps(experiment))
            output = root / "result.json"
            process = subprocess.run(fixtures.select_v1_command(fixture, output),
                                     capture_output=True, text=True)
            self.assertEqual(process.returncode, 0, process.stderr)
            result = json.loads(output.read_text())
            self.assertEqual(result["chosen_point_id"], 2)
            settings = {"tail_limit_seconds": 4, "boundary_hop_size": 128,
                        "measurement_fft_size": 2048, "measurement_hop_size": 128}
            for row in manifest["objectives"]:
                row.update(phase="clean-tail", phase_options=settings)
            fixture["manifest"].write_text(json.dumps(manifest))
            before = fixture["manifest"].read_bytes()
            output = root / "tail-result.json"
            process = subprocess.run(fixtures.select_v1_command(fixture, output),
                                     capture_output=True, text=True)
            self.assertEqual(process.returncode, 0, process.stderr)
            self.assertEqual(fixture["manifest"].read_bytes(), before)
            result = json.loads(output.read_text())
            self.assertEqual(result["chosen_point_id"], 2)
            expected = fixtures.MODULE.analyzer_evidence_module().note_phase_options(settings)
            for point in result["points"]:
                for row in point["evidence"]:
                    self.assertEqual(row["phase_options"], expected)
            manifest["objectives"] = [
                {**row, "id": row["id"] + "-" + metric, "metric": metric}
                for row in manifest["objectives"]
                for metric in ("rms_dbfs", "centroid_hz", "duration_seconds")]
            fixture["manifest"].write_text(json.dumps(manifest))
            fit = fixtures.MODULE
            evidence = fit.analyzer_evidence_module()
            results = []
            for name, calls in (("cached", 6), ("uncached", 24), ("repeat", 6)):
                output = root / (name + "-result.json")
                original_run = evidence.AnalyzerEvidence._run
                with mock.patch.object(evidence.AnalyzerEvidence, "_run", autospec=True,
                                       side_effect=original_run) as run, mock.patch.object(
                        fit.sys, "argv", fixtures.select_v1_command(fixture, output)[1:]):
                    if name == "uncached":
                        with mock.patch.object(evidence, "NotePhaseCache", return_value=None):
                            self.assertEqual(fit.main(), 0)
                    else:
                        self.assertEqual(fit.main(), 0)
                    self.assertEqual(run.call_count, calls, name)
                results.append(json.loads(output.read_text()))
            self.assertEqual(results[0], results[1])
            self.assertEqual(results[0], results[2])
            for row in manifest["objectives"]:
                row.update(phase="attack", metric="rise_90_seconds")
            fixture["manifest"].write_text(json.dumps(manifest))
            output = root / "envelope-result.json"
            process = subprocess.run(fixtures.select_v1_command(fixture, output),
                                     capture_output=True, text=True)
            self.assertEqual(process.returncode, 0, process.stderr)
            result = json.loads(output.read_text())
            self.assertEqual(result["method_versions"]["note_phase_envelope"],
                             "note-phase-envelope-1")
            for point in result["points"]:
                for row in point["evidence"]:
                    self.assertEqual(row["metric"], "rise_90_seconds")
                    self.assertEqual(row["unit"], "seconds")

    def test_fit_can_measure_a_slow_tail_with_explicit_settings(self):
        fit = load_fit_tests().MODULE
        with tempfile.TemporaryDirectory() as directory:
            reference = Path(directory) / "reference.wav"
            model = Path(directory) / "model.wav"
            recording(reference, tail_seconds=4, decay_seconds=0.6)
            recording(model, tail_seconds=4, decay_seconds=0.6)
            objective = {"phase": "clean-tail", "metric": "level_slope_db_per_second",
                         "reference_span": [3200, 19200], "model_span": [3200, 19200]}
            def measure():
                return fit.run_note_phase(ANALYZER, reference, model, objective,
                    fit.sha256(ANALYZER), fit.sha256(reference), fit.sha256(model))
            with self.assertRaisesRegex(fit.FitError, "truncated"):
                measure()
            objective["phase_options"] = {
                "tail_limit_seconds": 4, "boundary_frame_size": 1024,
                "boundary_hop_size": 128, "measurement_fft_size": 2048,
                "measurement_hop_size": 128, "boundary_search_seconds": 0.1,
                "silence_threshold_dbfs": -65, "min_phase_seconds": 0.01,
                "min_body_seconds": 0.04,
            }
            result = measure()
            self.assertEqual(result["absolute_delta"], 0.0)
            self.assertEqual(result["phase_options"], objective["phase_options"])
            self.assertLess(result["reference_value"], -10)

    def test_phase_settings_are_validated_before_running(self):
        evidence = load_fit_tests().MODULE.analyzer_evidence_module()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "note.wav"
            recording(path)
            checks = evidence.AnalyzerEvidence(ANALYZER, {
                "note": (path, hashlib.sha256(path.read_bytes()).hexdigest())})
            invalid = [[], {"typo": 4}, {"tail_limit_seconds": True},
                {"tail_limit_seconds": float("inf")}, {"tail_limit_seconds": 11},
                {"tail_limit_seconds": -1}, {"boundary_frame_size": 1000},
                {"boundary_frame_size": 1024.0}, {"boundary_hop_size": 4096},
                {"measurement_fft_size": 128}, {"measurement_hop_size": 0},
                {"silence_threshold_dbfs": -201}]
            for options in invalid:
                with self.subTest(options=options), mock.patch.object(checks, "_run") as run:
                    with self.assertRaises(evidence.EvidenceError):
                        checks.note_phases("note", 3200, 19200, options=options)
                    run.assert_not_called()

    def test_fit_reuses_phase_reports_across_objectives_and_candidates(self):
        fit = load_fit_tests().MODULE
        evidence = fit.analyzer_evidence_module()
        with tempfile.TemporaryDirectory() as directory:
            reference = Path(directory) / "reference.wav"
            models = [Path(directory) / (name + ".wav") for name in ("first", "second")]
            recording(reference)
            for model in models:
                recording(model, sustain_gain=0.15)
            cache = evidence.NotePhaseCache()
            original_run = evidence.AnalyzerEvidence._run
            with mock.patch.object(evidence.AnalyzerEvidence, "_run", autospec=True,
                                   side_effect=original_run) as run:
                for model in models:
                    for phase, metric in (("attack", "duration_seconds"),
                                          ("sustain", "centroid_hz"),
                                          ("release", "level_slope_db_per_second")):
                        objective = {"phase": phase, "metric": metric,
                            "reference_span": [3200, 19200], "model_span": [3200, 19200]}
                        cached = fit.run_note_phase(ANALYZER, reference, model, objective,
                            fit.sha256(ANALYZER), fit.sha256(reference), fit.sha256(model),
                            cache=cache)
                        with mock.patch.object(evidence.AnalyzerEvidence, "_run", original_run):
                            direct = fit.run_note_phase(ANALYZER, reference, model, objective,
                                fit.sha256(ANALYZER), fit.sha256(reference), fit.sha256(model))
                        self.assertEqual(cached, direct)
                self.assertEqual(run.call_count, 3)

    def test_phase_cache_returns_copies_and_rechecks_inputs(self):
        evidence = load_fit_tests().MODULE.analyzer_evidence_module()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.wav"
            analyzer = Path(directory) / ANALYZER.name
            shutil.copy2(ANALYZER, analyzer)
            recording(source)
            checks = evidence.AnalyzerEvidence(analyzer, {
                "note": (source, hashlib.sha256(source.read_bytes()).hexdigest())})
            cache = evidence.NotePhaseCache()
            first = cache.note_phases(checks, "note", 3200, 19200)
            expected = copy.deepcopy(first.report)
            first.phases["sustain"]["metrics"]["rms_dbfs"]["value"] = 123
            with mock.patch.object(checks, "_run") as run:
                self.assertEqual(cache.note_phases(checks, "note", 3200, 19200).report, expected)
                run.assert_not_called()
                recording(source, sustain_gain=0.15)
                with self.assertRaisesRegex(evidence.EvidenceError, "changed"):
                    cache.note_phases(checks, "note", 3200, 19200)
                recording(source)
                with analyzer.open("ab") as stream:
                    stream.write(b"changed")
                with self.assertRaisesRegex(evidence.EvidenceError, "changed"):
                    cache.note_phases(checks, "note", 3200, 19200)
                run.assert_not_called()

    def test_phase_cache_separates_settings_spans_and_evicts_old_reports(self):
        evidence = load_fit_tests().MODULE.analyzer_evidence_module()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.wav"
            recording(source)
            checks = evidence.AnalyzerEvidence(ANALYZER, {
                "note": (source, hashlib.sha256(source.read_bytes()).hexdigest())})
            cache = evidence.NotePhaseCache(max_entries=2)
            with mock.patch.object(checks, "_run", wraps=checks._run) as run:
                cache.note_phases(checks, "note", 3200, 19200)
                cache.note_phases(checks, "note", 3200, 19200,
                                 options=evidence.note_phase_options())
                self.assertEqual(run.call_count, 1)
                cache.note_phases(checks, "note", 3200, 19200, options={"tail_limit_seconds": 4})
                cache.note_phases(checks, "note", 3200, 19200)
                cache.note_phases(checks, "note", 3200, 19328)
                self.assertEqual(run.call_count, 3)
                cache.note_phases(checks, "note", 3200, 19200)
                self.assertEqual(run.call_count, 3)
                cache.note_phases(checks, "note", 3200, 19200, options={"tail_limit_seconds": 4})
                self.assertEqual(run.call_count, 4)

    def test_phase_cache_separates_source_analyzer_and_process_context(self):
        evidence = load_fit_tests().MODULE.analyzer_evidence_module()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.wav"
            recording(source)
            digest = hashlib.sha256(source.read_bytes()).hexdigest()
            second = root / "second.wav"
            shutil.copy2(source, second)
            analyzer = root / ANALYZER.name
            shutil.copy2(ANALYZER, analyzer)
            cache = evidence.NotePhaseCache()
            original_run = evidence.AnalyzerEvidence._run
            rows = [(ANALYZER, source, {}, 1), (ANALYZER, source, {}, 1),
                    (ANALYZER, second, {}, 2), (analyzer, source, {}, 3),
                    (ANALYZER, source, {"cwd": root}, 4),
                    (ANALYZER, source, {"environment": {"LC_ALL": "C"}}, 5)]
            with mock.patch.object(evidence.AnalyzerEvidence, "_run", autospec=True,
                                   side_effect=original_run) as run:
                for index, (binary, path, options, count) in enumerate(rows):
                    source_id = "alias-" + str(index)
                    checks = evidence.AnalyzerEvidence(binary, {source_id: (path, digest)}, **options)
                    cache.note_phases(checks, source_id, 3200, 19200)
                    self.assertEqual(run.call_count, count)

    def test_checker_rejects_wrong_or_missing_phase_settings(self):
        evidence = load_fit_tests().MODULE.analyzer_evidence_module()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "note.wav"
            recording(path)
            checks = evidence.AnalyzerEvidence(ANALYZER, {
                "note": (path, hashlib.sha256(path.read_bytes()).hexdigest())})
            options = {"tail_limit_seconds": 4}
            original = checks.note_phases("note", 3200, 19200, options=options).report
            for key in evidence.note_phase_options():
                for missing in (False, True):
                    report = copy.deepcopy(original)
                    if missing:
                        del report[key]
                    else:
                        report[key] += 0.125
                    with self.subTest(key=key, missing=missing), mock.patch.object(
                            checks, "_run", return_value=report):
                        with self.assertRaises(evidence.EvidenceError):
                            checks.note_phases("note", 3200, 19200, options=options)

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
        fit = load_fit_tests().MODULE
        result = fit.run_note_phase(ANALYZER, IOWA_RECORDING, IOWA_RECORDING,
            {"phase": "clean-tail", "metric": "level_slope_db_per_second",
             "reference_span": [44100, 88200], "model_span": [44100, 88200],
             "phase_options": {"tail_limit_seconds": 4}},
            fit.sha256(ANALYZER), fit.sha256(IOWA_RECORDING), fit.sha256(IOWA_RECORDING))
        self.assertEqual(result["absolute_delta"], 0.0)
        self.assertEqual(result["reference_end_sample"], reports[1]["phases"][3]["end_sample"])
        result = fit.run_note_phase(ANALYZER, IOWA_RECORDING, IOWA_RECORDING,
            {"phase": "attack", "metric": "rise_90_seconds",
             "reference_span": [44100, 88200], "model_span": [44100, 88200]},
            fit.sha256(ANALYZER), fit.sha256(IOWA_RECORDING), fit.sha256(IOWA_RECORDING))
        self.assertEqual(result["absolute_delta"], 0.0)
        self.assertGreater(result["reference_value"], 0.0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--analyzer", required=True)
    parser.add_argument("--iowa-recording", type=Path)
    options, remaining = parser.parse_known_args()
    ANALYZER = Path(options.analyzer).resolve()
    IOWA_RECORDING = options.iowa_recording
    unittest.main(argv=[__file__, *remaining])

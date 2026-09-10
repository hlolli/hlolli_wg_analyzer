#!/usr/bin/env python3
"""Note-phase contract and acoustic fixtures through the public CLI."""
import argparse
import cmath
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

    def test_partial_tracks_follow_tones_and_preserve_frames(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "note.wav"
            recording(path)
            base = self.run_report(path, "--phase-frames")
            report = self.run_report(path, "--phase-partials")
            metadata = report["frame_series"]["partial_tracking"]
            self.assertEqual(metadata["method"], "spectral-peak-tracks-1")
            self.assertEqual(metadata["frequency_state"], "estimated")
            self.assertEqual(metadata["bin_power_unit"], "full-scale-squared")
            steady = [r for r in report["frame_series"]["rows"] if 8000 <= r["center_sample"] <= 14000]
            self.assertTrue(steady)
            for frequency in (220, 660):
                peaks = [min(r["partials"], key=lambda p: abs(p["frequency_hz"]-frequency)) for r in steady]
                self.assertTrue(all(abs(p["frequency_hz"]-frequency) < 0.2 for p in peaks))
                self.assertEqual(len({p["track_id"] for p in peaks}), 1)
                self.assertTrue(all(p["continued"] for p in peaks[1:]))
            both = self.run_report(path, "--phase-partials", "--phase-spectra")
            both["frame_series"].pop("spectra")
            for row in both["frame_series"]["rows"]:
                powers = row.pop("bin_powers")
                for point in row["partials"]:
                    k = point["bin_index"]
                    self.assertEqual(point["bin_power"], powers[k])
                    self.assertGreater(powers[k], powers[k-1])
                    self.assertGreaterEqual(powers[k], powers[k+1])
            self.assertEqual(both, report)
            report["frame_series"].pop("partial_tracking")
            for row in report["frame_series"]["rows"]:
                row.pop("partials")
            self.assertEqual(report, base)
            limited = self.run_report(path, "--phase-partials", "--max-partials", "1")
            self.assertGreater(limited["frame_series"]["partial_tracking"]["omitted_peak_count"], 0)
            self.assertTrue(all(len(r["partials"]) <= 1 for r in limited["frame_series"]["rows"]))
            custom = self.run_report(path, "--phase-partials", "--partial-link-cents", "50",
                                      "--partial-relative-floor-db", "-10")
            self.assertEqual(custom["frame_series"]["partial_tracking"]["max_step_cents"], 50)
            for flags in (("--partial-link-cents", "0"), ("--partial-relative-floor-db", "1"),
                          ("--partial-link-cents", "50")):
                self.run_report(path, *flags, expected=2)
            self.run_report(path, "--phase-partials", "--phase-partials", expected=2)
            for limit in ("--max-measurement-work-bytes", "--max-measurement-series-points"):
                self.run_report(path, "--phase-partials", limit, "1", expected=1)
            for variant in ({"silence": True}, {"second": True}, {"tail_seconds": 0.35}):
                recording(path, **variant)
                report = self.run_report(path, "--phase-partials")
                valid = {p["phase"] for p in report["phases"] if p["status"] == "valid"}
                self.assertTrue(all(r["phase"] in valid for r in report["frame_series"]["rows"]))
                if variant.get("silence"):
                    self.assertEqual(report["frame_series"]["partial_tracking"]["track_count"], 0)

    def test_spectra_match_dft_and_preserve_reports(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "note.wav"
            flags = ("--measure-fft-size", "256", "--measure-hop-size", "128")
            for fixture in ("note", "edges", "padded"):
                recording(path, tail_seconds=0.05 if fixture == "padded" else 1.4)
                if fixture == "edges":
                    with wave.open(str(path), "wb") as audio:
                        audio.setnchannels(1)
                        audio.setsampwidth(2)
                        audio.setframerate(16000)
                        audio.writeframes(b"".join(struct.pack("<h", round(32768 * (0.2 +
                            0.2 * (-1)**i + 0.2 * math.sin(math.tau * 16 * i / 256))))
                            for i in range(41600)))
                with wave.open(str(path)) as audio:
                    samples = struct.unpack("<" + str(audio.getnframes()) + "h",
                                            audio.readframes(audio.getnframes()))
                frame_only = self.run_report(path, *flags, "--phase-frames")
                report = self.run_report(path, *flags, "--phase-spectra")
                self.assertEqual(report, self.run_report(path, *flags, "--phase-spectra", "--phase-frames"))
                series = report["frame_series"]
                self.assertEqual(series["spectra"]["method"], "note-phase-spectra-1")
                self.assertEqual(series["spectra"]["power_unit"], "full-scale-squared")
                self.assertEqual(series["spectra"]["bin_count"], 129)
                self.assertEqual(series["spectra"]["first_bin_hz"], 0)
                self.assertEqual(series["spectra"]["bin_step_hz"], 62.5)
                rows = series["rows"]
                self.assertTrue(rows)
                for row in rows:
                    powers = row["bin_powers"]
                    self.assertEqual(len(powers), 129)
                    self.assertTrue(all(math.isfinite(p) and p >= 0 for p in powers))
                    total = sum(powers)
                    self.assertAlmostEqual(row["level_dbfs"],
                        10 * math.log10(total) if total > 1e-30 else -300, places=9)
                window = [0.5 * (1 - math.cos(math.tau * i / 255)) for i in range(256)]
                energy = sum(w*w for w in window)
                for row in (rows[0], rows[len(rows)//2], rows[-1]):
                    start = row["start_sample"]
                    values = [(samples[start+i]/32768 if start+i < len(samples) else 0)*window[i]
                              for i in range(256)]
                    expected = [abs(sum(v * cmath.exp(-1j * math.tau * k * n / 256)
                                       for n, v in enumerate(values)))**2 / (256 * energy)
                                * (1 if k in (0, 128) else 2) for k in range(129)]
                    for actual, power in zip(row["bin_powers"], expected):
                        self.assertTrue(math.isclose(actual, power, rel_tol=1e-8, abs_tol=1e-14))
                    self.assertAlmostEqual(sum(row["bin_powers"]),
                                           sum(v*v for v in values)/energy, places=12)
                if fixture == "edges":
                    mid = rows[len(rows)//2]["bin_powers"]
                    self.assertGreater(mid[0], 0.02)
                    self.assertGreater(mid[-1], 0.02)
                if fixture == "padded":
                    self.assertTrue(any(r["zero_padded"] for r in rows))
                series.pop("spectra")
                for row in rows:
                    row.pop("bin_powers")
                self.assertEqual(report, frame_only)
            recording(path)
            envelope = self.run_report(path, *flags, "--phase-envelope", "--phase-frames")
            both = self.run_report(path, *flags, "--phase-envelope", "--phase-spectra")
            both["frame_series"].pop("spectra")
            for row in both["frame_series"]["rows"]:
                row.pop("bin_powers")
            self.assertEqual(both, envelope)

    def test_spectra_limits_rejections_and_low_power(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "note.wav"
            recording(path)
            flags = ("--phase-spectra", "--measure-fft-size", "256")
            report = self.run_report(path, *flags, "--block-frames", "97")
            self.assertEqual(report, self.run_report(path, *flags, "--block-frames", "4096"))
            transforms = (report["frames"] - 128 + 255) // 256
            self.run_report(path, *flags, "--max-measurement-transforms", str(transforms))
            for limit in ("--max-work-bytes", "--max-measurement-work-bytes", "--max-measurements", "--max-measurement-series-points",
                          "--max-measurement-transforms"):
                failed = self.run_report(path, *flags, limit, "1", expected=1)
                self.assertEqual(failed.stdout, "")
            # Frame storage fits here; dense spectrum storage does not.
            self.run_report(path, "--phase-frames", "--measure-hop-size", "4", "--max-measurement-work-bytes", "4000000")
            failed = self.run_report(path, "--phase-spectra", "--measure-hop-size", "4",
                                     "--max-measurement-work-bytes", "4000000", expected=1)
            self.assertIn("spectra exceed the work limit", failed.stderr)
            self.assertEqual(failed.stdout, "")
            self.run_report(path, *flags, "--phase-spectra", expected=2)
            wrong = subprocess.run([str(ANALYZER), "inspect", str(path), "--phase-spectra"], capture_output=True)
            self.assertEqual(wrong.returncode, 2)
            low = self.run_report(path, *flags, "--spectral-floor-dbfs", "0")
            self.assertTrue(any(sum(r["bin_powers"]) > 0 for r in low["frame_series"]["rows"]))
            self.assertTrue(all(r["centroid_hz"] is None for r in low["frame_series"]["rows"]))
            self.assertEqual([r["bin_powers"] for r in report["frame_series"]["rows"]],
                             [r["bin_powers"] for r in low["frame_series"]["rows"]])
            for variant in ({"silence": True}, {"second": True}, {"tail_seconds": 0.35}):
                recording(path, **variant)
                report = self.run_report(path, *flags)
                valid = {p["phase"] for p in report["phases"] if p["status"] == "valid"}
                self.assertTrue(all(r["phase"] in valid for r in report["frame_series"]["rows"]))
                if variant.get("silence"):
                    self.assertEqual(report["frame_series"]["rows"], [])
                    self.assertEqual(report["frame_series"]["spectra"]["bin_count"], 129)

    def test_frame_series_grid_and_samples(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "note.wav"
            recording(path, modulation_db=4)
            flags = ("--measure-fft-size", "256", "--measure-hop-size", "64")
            basic = self.run_report(path, *flags)
            report = self.run_report(path, *flags, "--phase-frames")
            series = report.pop("frame_series")
            self.assertEqual(report, basic)
            self.assertEqual(series["method"], "note-phase-frames-1")
            self.assertEqual(series["window"], "symmetric-hann")
            self.assertEqual(series["spectrum_weighting"], "one-sided-power")
            self.assertIs(series["flatness_excludes_dc"], True)
            self.assertEqual(series["channel_mix"], "arithmetic-mean")
            self.assertEqual(series["level_weighting"], "window-energy-normalized")
            self.assertEqual(series["level_floor_dbfs"], -300)
            self.assertEqual(series["spectral_floor_dbfs"], -100)
            rows = series["rows"]
            expected = []
            for phase in report["phases"]:
                if phase["status"] != "valid":
                    continue
                expected.extend((phase["phase"], center) for center in range(128, report["frames"], 64)
                                if phase["start_sample"] <= center < phase["end_sample"])
            self.assertEqual([(r["phase"], r["center_sample"]) for r in rows], expected)
            phases = {p["phase"]: p for p in report["phases"]}
            for row in rows:
                phase = phases[row["phase"]]
                start = row["center_sample"] - 128
                self.assertEqual(row["start_sample"], start)
                self.assertEqual(row["end_sample"], min(start + 256, report["frames"]))
                self.assertEqual(row["zero_padded"], start + 256 > report["frames"])
                self.assertEqual(row["crosses_phase_bounds"],
                    start < phase["start_sample"] or start + 256 > phase["end_sample"])
            with wave.open(str(path)) as audio:
                samples = struct.unpack("<" + str(audio.getnframes()) + "h",
                                        audio.readframes(audio.getnframes()))
            window = [0.5 * (1 - math.cos(math.tau * i / 255)) for i in range(256)]
            energy = sum(w*w for w in window)
            for row in (rows[0], rows[len(rows)//2], rows[-1]):
                start = row["start_sample"]
                values = [(samples[start+i] / 32768 if start+i < len(samples) else 0) * window[i]
                          for i in range(256)]
                power = [abs(sum(v * cmath.exp(-1j * math.tau * k * n / 256)
                                 for n, v in enumerate(values))) ** 2 / (256 * energy)
                         * (1 if k in (0, 128) else 2) for k in range(129)]
                total = sum(power)
                self.assertAlmostEqual(row["level_dbfs"],
                    10 * math.log10(total) if total > 1e-30 else -300, places=8)
                if row["spectral_status"] == "valid":
                    self.assertAlmostEqual(row["centroid_hz"],
                        sum(k * 16000 / 256 * p for k, p in enumerate(power)) / total, places=7)
                    flatness = math.exp(sum(math.log(max(p, 1e-30)) for p in power[1:])/128) / (sum(power[1:])/128)
                    self.assertAlmostEqual(row["flatness"], flatness, places=8)
            envelope = self.run_report(path, *flags, "--phase-envelope")
            both = self.run_report(path, *flags, "--phase-envelope", "--phase-frames")
            self.assertEqual(both.pop("frame_series"), series)
            self.assertEqual(both, envelope)
            wider = self.run_report(path, "--phase-frames", "--measure-fft-size", "256",
                                    "--measure-hop-size", "128")["frame_series"]["rows"]
            self.assertEqual(wider, [r for r in rows if r["start_sample"] % 128 == 0])
            sustain = [r for r in rows if r["phase"] == "sustain"]
            mean = sum(r["level_dbfs"] for r in sustain) / len(sustain)
            spread = math.sqrt(sum((r["level_dbfs"]-mean)**2 for r in sustain) / len(sustain))
            self.assertAlmostEqual(spread,
                envelope["phases"][1]["metrics"]["level_modulation_spread_db"]["value"], places=8)

    def test_frame_series_rejections_limits_and_blocks(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "note.wav"
            recording(path)
            first = self.run_report(path, "--phase-frames", "--block-frames", "97")
            self.assertEqual(first, self.run_report(path, "--phase-frames", "--block-frames", "4096"))
            transforms = (first["frames"] - first["measurement_fft_size"]//2 +
                          first["measurement_hop_size"] - 1) // first["measurement_hop_size"]
            for flags in ((), ("--phase-frames",)):
                self.run_report(path, *flags, "--max-measurement-transforms", str(transforms))
                self.run_report(path, *flags, "--max-measurement-transforms", str(transforms-1), expected=1)
            for limit in ("--max-work-bytes", "--max-measurement-series-points", "--max-measurements"):
                self.run_report(path, "--phase-frames", limit, "1", expected=1)
            for grid in (("--measure-hop-size", "0"), ("--measure-fft-size", "63")):
                process = subprocess.run([str(ANALYZER), "note-phases", str(path),
                    "--note-start-sample", "3200", "--note-end-sample", "19200",
                    "--phase-frames", *grid], capture_output=True)
                self.assertNotEqual(process.returncode, 0)
                self.assertEqual(process.stdout, b"")
            self.run_report(path, "--phase-frames", "--phase-frames", expected=2)
            process = subprocess.run([str(ANALYZER), "inspect", str(path), "--phase-frames"],
                                     capture_output=True)
            self.assertEqual(process.returncode, 2)
            below = self.run_report(path, "--phase-frames", "--spectral-floor-dbfs", "0")
            self.assertTrue(below["frame_series"]["rows"])
            for row in below["frame_series"]["rows"]:
                self.assertNotEqual(row["spectral_status"], "valid")
                self.assertIsNone(row["centroid_hz"])
                self.assertIsNone(row["flatness"])
            for variant in ({"silence": True}, {"second": True}, {"tail_seconds": 0.35}):
                recording(path, **variant)
                report = self.run_report(path, "--phase-frames")
                valid = {p["phase"] for p in report["phases"] if p["status"] == "valid"}
                self.assertTrue(all(r["phase"] in valid for r in report["frame_series"]["rows"]))
                if variant.get("silence"):
                    self.assertEqual(report["frame_series"]["rows"], [])
            recording(path, tail_seconds=0.05)
            report = self.run_report(path, "--phase-frames")
            padded = [r for r in report["frame_series"]["rows"] if r["zero_padded"]]
            self.assertTrue(padded)
            self.assertTrue(all(r["end_sample"] == report["frames"] for r in padded))

    def test_frame_series_dense_grid_and_channel_mix(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "note.wav"
            recording(path)
            dense = self.run_report(path, "--phase-frames", "--measure-fft-size", "256",
                                    "--measure-hop-size", "8")
            sustain = [r for r in dense["frame_series"]["rows"] if r["phase"] == "sustain"]
            self.assertGreater(len(sustain), 512)
            self.assertTrue(all(b["center_sample"] - a["center_sample"] == 8
                                for a, b in zip(sustain, sustain[1:])))
            with wave.open(str(path)) as audio:
                samples = struct.unpack("<" + str(audio.getnframes()) + "h",
                                        audio.readframes(audio.getnframes()))
            stereo = Path(directory) / "stereo.wav"
            with wave.open(str(stereo), "wb") as audio:
                audio.setnchannels(2)
                audio.setsampwidth(2)
                audio.setframerate(16000)
                audio.writeframes(b"".join(struct.pack("<hh", s, -s) for s in samples))
            report = self.run_report(stereo, "--phase-spectra", "--measure-fft-size", "256")
            self.assertTrue(report["frame_series"]["rows"])
            for row in report["frame_series"]["rows"]:
                self.assertEqual(row["level_dbfs"], -300)
                self.assertEqual(row["spectral_status"], "no-signal")
                self.assertIsNone(row["centroid_hz"])
                self.assertIsNone(row["flatness"])
                self.assertTrue(all(p == 0 for p in row["bin_powers"]))

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

    def test_partial_tracks_checked_evidence(self):
        fit = load_fit_tests().MODULE
        evidence = fit.analyzer_evidence_module()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "note.wav"
            recording(path)
            checks = evidence.AnalyzerEvidence(ANALYZER, {"note": (path, fit.sha256(path))})
            checks.note_phases("note", 3200, 19200, partials=True)
            original = checks.note_phases("note", 3200, 19200, partials=True, spectra=True).report
            cache = evidence.NotePhaseCache()
            with mock.patch.object(checks, "_run", wraps=checks._run) as run:
                for _ in range(2):
                    cache.note_phases(checks, "note", 3200, 19200, partials=True,
                                      partial_options={"max_peaks": 2, "max_step_cents": 50, "relative_floor_db": -20})
                self.assertEqual(run.call_count, 2)
            for change in ("id", "continued", "frequency", "power", "missing", "count", "omitted", "method", "settings"):
                report = copy.deepcopy(original)
                meta = report["frame_series"]["partial_tracking"]
                row = next(r for r in report["frame_series"]["rows"] if r["partials"])
                point = row["partials"][0]
                if change == "id":
                    point["track_id"] += 1
                elif change == "continued":
                    point["continued"] = not point["continued"]
                elif change == "frequency":
                    point["frequency_hz"] += 0.1
                elif change == "power":
                    point["bin_power"] *= 2
                elif change == "missing":
                    row["partials"].clear()
                elif change == "count":
                    meta["track_count"] += 1
                elif change == "omitted":
                    meta["omitted_peak_count"] += 1
                elif change == "method":
                    meta["identity"] = "harmonics"
                else:
                    meta["max_step_cents"] = 200
                with self.subTest(change=change), mock.patch.object(checks, "_run", return_value=report):
                    with self.assertRaises(evidence.EvidenceError):
                        checks.note_phases("note", 3200, 19200, partials=True, spectra=True)
            with mock.patch.object(checks, "_run", return_value=original):
                with self.assertRaises(evidence.EvidenceError):
                    checks.note_phases("note", 3200, 19200, spectra=True)
            for options in ({"max_peaks": True}, {"max_peaks": 33}, {"max_step_cents": 0},
                            {"relative_floor_db": 1}, {"typo": 1}):
                with self.assertRaises(evidence.EvidenceError):
                    checks.note_phases("note", 3200, 19200, partials=True, partial_options=options)
            with self.assertRaises(evidence.EvidenceError):
                checks.note_phases("note", 3200, 19200, partial_options={})
            with self.assertRaises(evidence.EvidenceError):
                checks.note_phases("note", 3200, 19200, partials=1)

    def test_spectra_checked_evidence_and_cache(self):
        fit = load_fit_tests().MODULE
        evidence = fit.analyzer_evidence_module()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "note.wav"
            recording(path)
            checks = evidence.AnalyzerEvidence(ANALYZER, {"note": (path, fit.sha256(path))})
            settings = {"measurement_fft_size": 256}
            original = checks.note_phases("note", 3200, 19200, spectra=True, options=settings).report
            self.assertEqual(original, checks.note_phases("note", 3200, 19200,
                spectra=True, frames=True, options=settings).report)
            cache = evidence.NotePhaseCache()
            with mock.patch.object(checks, "_run", wraps=checks._run) as run:
                for _ in range(2):
                    cache.note_phases(checks, "note", 3200, 19200, frames=True, options=settings)
                    result = cache.note_phases(checks, "note", 3200, 19200, spectra=True, options=settings)
                    self.assertEqual(result.report, original)
                    result.report["frame_series"]["rows"].clear()
                self.assertEqual(run.call_count, 3)
            for change in ("method", "unit", "bin-count", "spacing", "normalization", "edges",
                           "floor", "missing", "negative", "nan", "bool", "power", "swapped"):
                report = copy.deepcopy(original)
                meta = report["frame_series"]["spectra"]
                row = next(r for r in report["frame_series"]["rows"] if r["spectral_status"] == "valid")
                powers = row["bin_powers"]
                if change == "method":
                    meta["method"] = "unknown"
                elif change == "unit":
                    meta["power_unit"] = "dBFS"
                elif change == "bin-count":
                    meta["bin_count"] -= 1
                elif change == "spacing":
                    meta["bin_step_hz"] += 1
                elif change == "normalization":
                    meta["normalization"] = "power-density"
                elif change == "edges":
                    meta["dc_nyquist_bin_factor"] = 2
                elif change == "floor":
                    meta["includes_below_floor"] = False
                elif change == "missing":
                    powers.pop()
                elif change == "negative":
                    powers[0] = -1
                elif change == "nan":
                    powers[0] = float("nan")
                elif change == "bool":
                    powers[0] = True
                elif change == "power":
                    powers[0] += 1
                else:
                    powers.reverse()
                with self.subTest(change=change), mock.patch.object(checks, "_run", return_value=report):
                    with self.assertRaises(evidence.EvidenceError):
                        checks.note_phases("note", 3200, 19200, spectra=True, options=settings)
            with mock.patch.object(checks, "_run", return_value=original):
                with self.assertRaises(evidence.EvidenceError):
                    checks.note_phases("note", 3200, 19200, frames=True, options=settings)
            with self.assertRaises(evidence.EvidenceError):
                checks.note_phases("note", 3200, 19200, spectra=1)
            with self.assertRaises(evidence.EvidenceError):
                cache.note_phases(checks, "note", 3200, 19200, spectra=1)
            with mock.patch.object(evidence, "MAX_FRAME_REPORT_BYTES", 100):
                with self.assertRaisesRegex(evidence.EvidenceError, "byte limit"):
                    checks.note_phases("note", 3200, 19200, spectra=True)
            for variant in ({"silence": True}, {"second": True}, {"tail_seconds": 0.05}):
                recording(path, **variant)
                current = evidence.AnalyzerEvidence(ANALYZER, {"note": (path, fit.sha256(path))})
                current.note_phases("note", 3200, 19200, spectra=True, options=settings)

    def test_frame_series_checked_evidence_and_cache(self):
        fit = load_fit_tests().MODULE
        evidence = fit.analyzer_evidence_module()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "note.wav"
            recording(path)
            checks = evidence.AnalyzerEvidence(ANALYZER, {"note": (path, fit.sha256(path))})
            cache = evidence.NotePhaseCache()
            with mock.patch.object(checks, "_run", wraps=checks._run) as run:
                for frames, envelope in ((False, False), (True, False), (False, True), (True, True)):
                    original = cache.note_phases(checks, "note", 3200, 19200,
                                                 frames=frames, envelope=envelope)
                    self.assertEqual(cache.note_phases(checks, "note", 3200, 19200,
                                                       frames=frames, envelope=envelope), original)
                self.assertEqual(run.call_count, 4)
                cache.note_phases(checks, "note", 3200, 19200, frames=True,
                                  options={"measurement_hop_size": 64})
                self.assertEqual(run.call_count, 5)
            original = checks.note_phases("note", 3200, 19200, frames=True).report
            dense_options = {"measurement_fft_size": 256, "measurement_hop_size": 4}
            with mock.patch.object(checks, "_run", wraps=checks._run) as run:
                for _ in range(2):
                    dense = cache.note_phases(checks, "note", 3200, 19200,
                                              frames=True, options=dense_options)
                    self.assertGreater(len(json.dumps(dense.report)), evidence.MAX_REPORT_BYTES)
                self.assertEqual(run.call_count, 2)
            with mock.patch.object(evidence, "MAX_FRAME_REPORT_BYTES", 100):
                with self.assertRaisesRegex(evidence.EvidenceError, "byte limit"):
                    checks.note_phases("note", 3200, 19200, frames=True)
            for change in ("method", "unit", "mix", "floor", "missing", "duplicate", "clock",
                           "phase", "bounds", "padding", "crossing", "level", "centroid",
                           "flatness", "status", "bool"):
                report = copy.deepcopy(original)
                series = report["frame_series"]
                row = next(r for r in series["rows"] if r["spectral_status"] == "valid")
                if change == "method":
                    series["method"] = "unknown"
                elif change == "unit":
                    series["units"]["level_dbfs"] = "Hz"
                elif change == "mix":
                    series["channel_mix"] = "sum"
                elif change == "floor":
                    series["spectral_floor_dbfs"] = -90
                elif change == "missing":
                    series["rows"].pop()
                elif change == "duplicate":
                    series["rows"].append(copy.deepcopy(row))
                elif change == "clock":
                    row["center_sample"] += 1
                elif change == "phase":
                    row["phase"] = "clean-tail"
                elif change == "bounds":
                    row["end_sample"] -= 1
                elif change == "padding":
                    row["zero_padded"] = not row["zero_padded"]
                elif change == "crossing":
                    row["crosses_phase_bounds"] = not row["crosses_phase_bounds"]
                elif change == "level":
                    row["level_dbfs"] = float("nan")
                elif change == "centroid":
                    row["centroid_hz"] = 100000
                elif change == "flatness":
                    row["flatness"] = -1
                elif change == "status":
                    row["spectral_status"] = "below-floor"
                else:
                    row["start_sample"] = True
                with self.subTest(change=change), mock.patch.object(checks, "_run", return_value=report):
                    with self.assertRaises(evidence.EvidenceError):
                        checks.note_phases("note", 3200, 19200, frames=True)
            with mock.patch.object(checks, "_run", return_value=original):
                with self.assertRaises(evidence.EvidenceError):
                    checks.note_phases("note", 3200, 19200)
            with self.assertRaises(evidence.EvidenceError):
                checks.note_phases("note", 3200, 19200, frames=1)
            with self.assertRaises(evidence.EvidenceError):
                cache.note_phases(checks, "note", 3200, 19200, frames=1)
            result = cache.note_phases(checks, "note", 3200, 19200, frames=True)
            result.report["frame_series"]["rows"].clear()
            self.assertTrue(cache.note_phases(checks, "note", 3200, 19200,
                                             frames=True).report["frame_series"]["rows"])
            for variant in ({"silence": True}, {"second": True}, {"tail_seconds": 0.05}):
                recording(path, **variant)
                current = evidence.AnalyzerEvidence(ANALYZER, {"note": (path, fit.sha256(path))})
                current.note_phases("note", 3200, 19200, frames=True)

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
        checks = fit.analyzer_evidence_module().AnalyzerEvidence(ANALYZER, {
            "note": (IOWA_RECORDING, fit.sha256(IOWA_RECORDING))})
        frames = checks.note_phases("note", 44100, 88200, frames=True,
                                    options={"tail_limit_seconds": 4}).report["frame_series"]["rows"]
        self.assertTrue(frames)
        self.assertEqual({r["phase"] for r in frames}, {"attack", "sustain", "release", "clean-tail"})
        spectra = checks.note_phases("note", 44100, 88200, spectra=True,
                                     options={"tail_limit_seconds": 4}).report["frame_series"]["rows"]
        for row in spectra:
            self.assertEqual(len(row.pop("bin_powers")), 2049)
        self.assertEqual(spectra, frames)

        partial_report = checks.note_phases("note", 44100, 88200, partials=True,
            options={"tail_limit_seconds": 4}).report["frame_series"]
        self.assertGreater(partial_report["partial_tracking"]["track_count"], 0)
        self.assertTrue(any(row["partials"] for row in partial_report["rows"]))
        for row in partial_report["rows"]:
            row.pop("partials")
        self.assertEqual(partial_report["rows"], frames)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--analyzer", required=True)
    parser.add_argument("--iowa-recording", type=Path)
    options, remaining = parser.parse_known_args()
    ANALYZER = Path(options.analyzer).resolve()
    IOWA_RECORDING = options.iowa_recording
    unittest.main(argv=[__file__, *remaining])

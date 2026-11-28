#!/usr/bin/env python3
"""Verify local speech comparisons on the caller-declared sample clock."""
import argparse
import json
import math
from pathlib import Path
import subprocess
import tempfile
import unittest
import wave

from test_paired_note_spectra_cli import RATE, read_samples, write_csv, write_wave
import test_speech_spans_cli as speech


ANALYZER = None
COLUMNS = ("id", "start_sample", "end_sample", "word", "phone", "kind")
WORD_START = round(0.22 * RATE)
WORD_END = WORD_START + round(0.275 * RATE)
EDIT_START = round(0.3297 * RATE)
EDIT_END = EDIT_START + round(0.0335 * RATE)
ATTACK = round(0.145 * RATE)
ATTACK_HALF = round(0.003 * RATE)


def raised_edge(index, start, end, width):
    fraction = min(1.0, (index - start) / width, (end - index) / width)
    return 0.5 * (1 - math.cos(math.pi * fraction))


def signal(edit=None):
    samples = []
    for index in range(round(0.68 * RATE)):
        time = index / RATE
        value = 0.0
        if abs(index - ATTACK) < ATTACK_HALF:
            # A short common stop burst must not qualify as a pairwise change.
            value = 0.67 * math.cos(math.pi * (index - ATTACK) / (2 * ATTACK_HALF)) ** 2
            value *= math.cos(math.tau * 3190 * time)
        if WORD_START <= index < WORD_END:
            envelope = raised_edge(index, WORD_START, WORD_END, 0.018 * RATE)
            low = math.sin(math.tau * 360 * time)
            high = math.sin(math.tau * 2420 * time)
            voiced = 0.23 * math.sin(math.tau * 180 * time) + 0.13 * low + 0.025 * high
            if EDIT_START <= index < EDIT_END:
                taper = raised_edge(index, EDIT_START, EDIT_END, 0.006 * RATE)
                if edit == "spectral":
                    voiced += 0.13 * taper * (high - low)
                elif edit == "level":
                    voiced *= 1 - 0.74 * taper
            value = envelope * voiced
        samples.append(round(32767 * value))
    return samples


def rms(values):
    return math.sqrt(sum(value * value for value in values) / len(values))


class SpeechTemporalTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="hwa speech temporal ")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.reference = self.root / "reference.wav"
        self.candidate = self.root / "candidate.wav"
        self.spans = self.root / "phones.csv"
        self.reference_samples = signal()
        write_wave(self.reference, self.reference_samples)
        write_csv(self.spans, COLUMNS, [
            ("stop", ATTACK - ATTACK_HALF, ATTACK + ATTACK_HALF, "tók", "t", "consonant"),
            ("fann", WORD_START, WORD_END, "fann", "", "word"),
            ("phone", round(0.27 * RATE), round(0.43 * RATE), "fann", "a", "vowel"),
        ])

    def cli(self, *args):
        return subprocess.run([str(ANALYZER), *map(str, args)], capture_output=True,
                              encoding="utf-8", timeout=30)

    def compare(self, *options, swap=False):
        reference, candidate = self.reference, self.candidate
        if swap:
            reference, candidate = candidate, reference
        result = self.cli("compare-speech-spans", reference, candidate,
                          "--spans", self.spans, "--shared-timing", *options)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(result.stderr, result.stderr)
        doc = json.loads(result.stdout)
        self.assertEqual(doc["timing_mode"], "shared")
        self.assertEqual(doc["temporal"]["status"], "ok")
        for setting in ("window", "hop"):
            self.assertGreater(doc["temporal"][f"{setting}_samples"], 0)
            self.assertAlmostEqual(doc["temporal"][f"{setting}_ms"],
                                   1000 * doc["temporal"][f"{setting}_samples"] / RATE)
        return doc

    def sample_range(self, span):
        start, end = span["start_sample"], span["end_sample"]
        self.assertLess(start, end)
        self.assertAlmostEqual(span["start_seconds"], start / RATE)
        self.assertAlmostEqual(span["end_seconds"], end / RATE)
        return start, end

    def check_local_region(self, change, temporal):
        self.assertEqual((change["rank"], change["word"]), (1, "fann"), change)
        self.assertEqual(change["phone"], "a", change)
        start, end = self.sample_range(change["reference_span"])
        self.assertEqual(self.sample_range(change["candidate_span"]), (start, end))
        self.assertAlmostEqual(change["duration_ms"], 1000 * (end - start) / RATE)
        self.assertLess(start, EDIT_END)
        self.assertGreater(end, EDIT_START)
        self.assertLessEqual(abs(start - EDIT_START), temporal["hop_samples"], change)
        self.assertLessEqual(abs(end - EDIT_END), temporal["hop_samples"], change)
        self.assertAlmostEqual(change["difference"], change["candidate"] - change["reference"])
        self.assertTrue(change["metric"])
        self.assertLessEqual(start, change["peak_sample"])
        self.assertLess(change["peak_sample"], end)
        self.assertAlmostEqual(change["peak_seconds"], change["peak_sample"] / RATE)
        return start, end

    def test_spectral_edit_localizes_and_reverses_under_swap(self):
        write_wave(self.candidate, signal(edit="spectral"))
        temporal = self.compare()["temporal"]
        self.assertTrue(temporal["spectral"], temporal)
        change = temporal["spectral"][0]
        bounds = self.check_local_region(change, temporal)
        self.assertGreater(change["difference"], 0.0, change)

        swapped = self.compare(swap=True)["temporal"]
        reversed_change = swapped["spectral"][0]
        self.assertEqual(self.check_local_region(reversed_change, swapped), bounds)
        self.assertEqual(reversed_change["metric"], change["metric"])
        self.assertAlmostEqual(reversed_change["difference"], -change["difference"])

    def test_temporal_level_dip_has_its_own_rank(self):
        write_wave(self.candidate, signal(edit="level"))
        temporal = self.compare()["temporal"]
        self.assertTrue(temporal["level"], temporal)
        change = temporal["level"][0]
        self.check_local_region(change, temporal)
        self.assertLess(change["difference"], -2.0, change)

    def test_identical_and_gain_only_controls_with_common_sharp_attack(self):
        for delta in (0, -7):
            with self.subTest(gain_db=delta):
                gain = 10 ** (delta / 20)
                write_wave(self.candidate, [round(gain * value) for value in self.reference_samples])
                report = self.compare()
                self.assertAlmostEqual(report["levels"]["candidate_minus_reference_db"], delta, delta=0.01)
                temporal = report["temporal"]
                self.assertEqual(temporal["level"], [], temporal)
                self.assertEqual(temporal["spectral"], [], temporal)
                self.assertIn("tók", {row["word"] for row in temporal["spans"]})

    def test_declared_clock_keeps_a_275_ms_word_analyzable(self):
        cropped = self.reference_samples[WORD_START:WORD_END]
        write_wave(self.reference, cropped)
        write_wave(self.candidate, cropped)
        write_csv(self.spans, COLUMNS[:4], [("fann", 0, len(cropped), "fann")])
        report = self.compare()
        rows = report["temporal"]["spans"]
        self.assertEqual(len(rows), 1, rows)
        self.assertEqual(rows[0]["word"], "fann")
        self.assertEqual(self.sample_range(rows[0]["reference_span"]), (0, len(cropped)))
        self.assertEqual(self.sample_range(rows[0]["candidate_span"]), (0, len(cropped)))
        self.assertEqual(report["temporal"]["level"], [])
        self.assertEqual(report["temporal"]["spectral"], [])

    def test_boundary_keeps_label_and_exports_level_matched_same_clock_audio(self):
        candidate_samples = signal(edit="spectral")
        write_wave(self.candidate, candidate_samples)
        write_csv(self.spans, COLUMNS, [
            ("fann-word", WORD_START, WORD_END, "fann", "", "word"),
            ("fann-join", EDIT_START - RATE // 50, EDIT_END + RATE // 50, "fann", "a→n", "boundary"),
            ("unchanged-join", ATTACK - ATTACK_HALF, ATTACK + ATTACK_HALF, "tók", "t→o", "boundary"),
        ])
        directory = self.root / "listen here"
        directory.mkdir()
        temporal = self.compare("--excerpt-dir", directory)["temporal"]
        ranked = temporal["spectral"][0]
        self.assertEqual((ranked["word"], ranked["boundary"]), ("fann", "a→n"), ranked)
        excerpt = ranked["excerpt"]
        self.assertIsNotNone(excerpt, ranked)
        expected_range = self.sample_range(excerpt["reference_span"])
        self.assertEqual(self.sample_range(excerpt["candidate_span"]), expected_range)
        start, end = expected_range
        self.assertLessEqual(start, WORD_START)
        self.assertGreaterEqual(end, WORD_END)
        powers = []
        for side, source in (("reference", self.reference_samples),
                             ("candidate", candidate_samples)):
            path = Path(excerpt[f"{side}_path"])
            self.assertEqual(path.parent, directory)
            with wave.open(str(path), "rb") as audio:
                self.assertEqual((audio.getframerate(), audio.getnchannels()), (RATE, 1))
            rendered = read_samples(path)
            original = source[start:end]
            self.assertEqual(len(rendered), len(original))
            gain = 10 ** (excerpt[f"{side}_gain_db"] / 20)
            self.assertLessEqual(max(abs(value - gain * raw) for value, raw in zip(rendered, original)), 1.0)
            powers.append(rms(rendered))
        self.assertGreater(min(powers), 0.05 * 32768)
        self.assertAlmostEqual(20 * math.log10(powers[1] / powers[0]), 0.0, delta=0.02)
        unchanged = next(row for row in temporal["spans"] if row["id"] == "unchanged-join")
        self.assertGreater(unchanged["cells"], 0)
        for measurement in unchanged["measurements"].values():
            self.assertAlmostEqual(measurement["difference"], 0.0)
        self.assertNotIn("tók", {row["word"] for row in temporal["spectral"] + temporal["level"]})

    def test_temporal_excerpt_collision_rolls_back_new_files(self):
        write_wave(self.candidate, signal(edit="spectral"))
        directory = self.root / "collision"
        directory.mkdir()
        temporal = self.compare("--excerpt-dir", directory)["temporal"]
        existing = Path(temporal["spectral"][0]["excerpt"]["candidate_path"])
        before = existing.read_bytes()
        for path in directory.iterdir():
            if path != existing:
                path.unlink()

        output = self.root / "output.json"
        self.assert_rejected("--shared-timing", "--excerpt-dir", directory, "--output", output)
        self.assertFalse(output.exists())
        self.assertEqual(list(directory.iterdir()), [existing])
        self.assertEqual(existing.read_bytes(), before)

    def test_respects_transform_budget(self):
        write_wave(self.candidate, signal(edit="spectral"))
        result = self.cli("compare-speech-spans", self.reference, self.candidate,
                          "--spans", self.spans, "--shared-timing", "--max-transforms", 1)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("transform", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_repeated_peak_prefers_boundary_for_the_same_word(self):
        write_wave(self.candidate, signal(edit="spectral"))
        write_csv(self.spans, COLUMNS, [
            ("fann-join", 5200, 6020, "fann", "a→n", "boundary"),
            ("other-phone", WORD_START, WORD_END, "another", "a", "phone"),
            ("fann-word", WORD_START, WORD_END, "fann", "", "word"),
        ])
        rows = self.compare()["temporal"]["spectral"]
        self.assertEqual({row["id"] for row in rows}, {"fann-join", "other-phone"})
        self.assertEqual(len(rows), 2)

    def test_span_without_a_frame_center_has_no_peak(self):
        write_wave(self.candidate, signal(edit="spectral"))
        # The range overlaps an edited cell but excludes its analysis center.
        write_csv(self.spans, COLUMNS[:4], [("tiny", 5350, 5355, "fann")])
        temporal = self.compare()["temporal"]
        self.assertEqual(temporal["spectral"], [])
        self.assertEqual(temporal["level"], [])
        self.assertEqual(temporal["spans"][0]["status"], "insufficient-temporal-support")

    def assert_rejected(self, *options):
        result = self.cli("compare-speech-spans", self.reference, self.candidate,
                          "--spans", self.spans, *options)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertFalse(result.stdout, result.stdout)

    def test_shared_timing_requires_explicit_opt_in_and_rejects_alignment_flags(self):
        write_wave(self.candidate, self.reference_samples)
        self.assert_rejected()
        dummy = self.root / "map.hwa-align"
        for flag, path in (("--alignment", dummy), ("--alignment-reference", self.reference),
                           ("--alignment-target", self.candidate)):
            with self.subTest(flag=flag):
                self.assert_rejected("--shared-timing", flag, path)

    def test_rejects_unequal_rates_and_lengths(self):
        for name, samples, rate in (("different rate", self.reference_samples, RATE + 1),
                                    ("different length", self.reference_samples[:-1], RATE)):
            with self.subTest(name=name):
                write_wave(self.candidate, samples, rate)
                self.assert_rejected("--shared-timing")

    def test_rejects_invalid_bounds_and_empty_wavs(self):
        write_wave(self.candidate, self.reference_samples)
        for start, end in ((0, len(self.reference_samples) + 1), (WORD_END, WORD_END),
                           (WORD_END, WORD_START)):
            with self.subTest(start=start, end=end):
                write_csv(self.spans, COLUMNS[:4], [("fann", start, end, "fann")])
                self.assert_rejected("--shared-timing")
        write_wave(self.reference, [])
        write_wave(self.candidate, [])
        write_csv(self.spans, COLUMNS[:4], [("fann", 0, 1, "fann")])
        self.assert_rejected("--shared-timing")

    def test_unsure_acoustic_alignment_still_abstains(self):
        fixture = speech.SpeechSpansTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        fixture.prepare(retimed=True, low_confidence=True, release_gain=10 ** (-18 / 20))
        doc = fixture.compare()
        self.assertEqual(doc["timing_mode"], "alignment")
        self.assertEqual(doc["temporal"]["status"], "requires-shared-timing")
        consonant = next(row for row in doc["consonant_loss"] if row["id"] == "c1")
        self.assertEqual(consonant["status"], "low-alignment-confidence")
        self.assertIsNone(consonant["rank"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--analyzer", required=True, type=Path)
    args, rest = parser.parse_known_args()
    ANALYZER = speech.ANALYZER = args.analyzer
    unittest.main(argv=[__file__, *rest])

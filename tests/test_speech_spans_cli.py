#!/usr/bin/env python3
"""Check speech evidence against known changes and known alignment anchors."""
import argparse
import json
import math
from pathlib import Path
import subprocess
import tempfile
import unittest
import wave

from test_paired_note_spectra_cli import RATE, read_samples, write_csv, write_wave


ANALYZER = None
COLUMNS = ("id", "start_sample", "end_sample", "word", "phone", "kind",
           "vowel_id", "edge", "confidence")
WORDS = ('"tæki,"', "poki")
FIXED_HZ = 1637.0


def word_audio(closure_count, vowel_count, pitch, pitch_change, release_gain,
               tone_hz, harmonic_gain):
    release_half = RATE // 200
    vowel_delay = round(0.009 * RATE)
    samples = []
    for index in range(-closure_count, vowel_count):
        value = 0.0
        if -release_half <= index < release_half:
            position = (index + release_half) / (2 * release_half)
            t = index / RATE
            value = 0.42 * release_gain * math.sin(math.pi * position) ** 2 * (
                0.6 * math.cos(math.tau * 3132 * t) + 0.4 * math.cos(math.tau * 4187 * t))
        if index >= vowel_delay:
            t = (index - vowel_delay) / RATE
            duration = (vowel_count - vowel_delay) / RATE
            phase = math.tau * (pitch * t + 0.5 * pitch_change * t * t / duration)
            envelope = min(1.0, t / 0.004, (duration - t) / 0.025)
            value = envelope * (0.28 * math.sin(phase) + 0.08 * math.sin(2 * phase)
                                + 0.04 * math.sin(3 * phase)
                                + harmonic_gain * math.sin(7 * phase)
                                + 0.08 * math.sin(math.tau * tone_hz * t))
        samples.append(round(32767 * value))
    return samples


def phrase(path, *, retimed=False, release_gain=1.0, second_release_gain=1.0,
           tone_hz=0.0, harmonic_gain=0.01, steady=False):
    prefix, gap, suffix = ((0.29, 0.21, 0.21) if retimed else (0.16, 0.16, 0.18))
    closures = (0.065, 0.055) if retimed else (0.053, 0.063)
    vowels = (0.48, 0.61) if retimed else (0.59, 0.51)
    samples = [0] * round(prefix * RATE)
    rows = []
    anchors = [0]
    for index, (closure, vowel) in enumerate(zip(closures, vowels)):
        start = len(samples)
        closure_count, vowel_count = round(closure * RATE), round(vowel * RATE)
        boundary = start + closure_count
        end = boundary + vowel_count
        n = index + 1
        word = WORDS[index]
        rows.extend([(f"c{n}", start, boundary, word, "t" if index == 0 else "p",
                      "consonant", f"v{n}", "end", 1),
                     (f"v{n}", boundary, end, word, "ai" if index == 0 else "ɔ",
                      "vowel", "", "", 1)])
        samples.extend(word_audio(closure_count, vowel_count,
                                  190 if steady else (150 + 22 * index),
                                  0 if steady else 90,
                                  release_gain if index == 0 else second_release_gain,
                                  tone_hz if index == 0 else 0.0,
                                  harmonic_gain if index == 0 else 0.01))
        anchors.extend((start, boundary, end))
        samples.extend([0] * round((gap if index == 0 else suffix) * RATE))
    anchors.append(len(samples))
    write_wave(path, samples)
    return rows, anchors


def rms(samples):
    return math.sqrt(sum(value * value for value in samples) / len(samples))


class SpeechSpansTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="hwa speech spans ")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.reference = self.root / "reference.wav"
        self.candidate = self.root / "candidate.wav"
        self.alignment = self.root / "pair.hwa-align"
        self.spans = self.root / "spans.csv"

    def cli(self, *args):
        return subprocess.run([str(ANALYZER), *map(str, args)], capture_output=True,
                              encoding="utf-8", timeout=30)

    def save_alignment(self, ref_anchors, cand_anchors, low_at=None):
        saved = self.cli("align", self.reference, self.candidate, "--output", self.alignment)
        self.assertEqual(saved.returncode, 0, saved.stderr)
        # Keep canonical settings and input hashes. Use the known boundaries so
        # a change in automatic alignment cannot obscure a speech DSP regression.
        saved_rows = self.alignment.read_text(encoding="utf-8").splitlines()
        rows = [saved_rows[0], *(line for line in saved_rows[1:]
                                 if line.startswith(("META,", "INPUT,")))]
        rows.extend(f"ANCHOR,{i},{a / RATE:.17g},{b / RATE:.17g},,"
                    f"{0.1 if a == low_at else 1},manual,1,1"
                    for i, (a, b) in enumerate(zip(ref_anchors, cand_anchors), 1))
        self.alignment.write_bytes(("\r\n".join(rows) + "\r\n").encode("utf-8"))

    def compare(self, *options):
        result = self.cli("compare-speech-spans", self.reference, self.candidate,
                          "--alignment", self.alignment, "--spans", self.spans, *options)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(result.stderr, result.stderr)
        return json.loads(result.stdout)

    def prepare(self, *, steady=False, low_confidence=False, **options):
        reference_rows, reference_anchors = phrase(self.reference, steady=steady)
        candidate_rows, candidate_anchors = phrase(self.candidate, steady=steady, **options)
        self.save_alignment(reference_anchors, candidate_anchors,
                            low_at=reference_rows[0][2] if low_confidence else None)
        write_csv(self.spans, COLUMNS, reference_rows)
        return reference_rows, candidate_rows

    def assert_no_findings(self, rows):
        for row in rows:
            self.assertNotEqual(row["status"], "finding", row)
            self.assertIsNone(row["rank"], row)

    def check_span(self, span, row):
        self.assertEqual((span["start_sample"], span["end_sample"]), tuple(row[1:3]))
        self.assertAlmostEqual(span["start_seconds"], row[1] / RATE)
        self.assertAlmostEqual(span["end_seconds"], row[2] / RATE)

    def test_gain_only_reports_level_without_findings(self):
        rows, anchors = phrase(self.reference)
        scale = 10 ** (-7 / 20)
        write_wave(self.candidate, [round(scale * value) for value in read_samples(self.reference)])
        self.save_alignment(anchors, anchors)
        write_csv(self.spans, COLUMNS, rows)
        report = self.compare()
        self.assertAlmostEqual(report["levels"]["candidate_minus_reference_db"], -7, delta=0.01)
        self.assertEqual(len(report["buzz"]), 2)
        self.assertEqual(len(report["consonant_loss"]), 2)
        self.assert_no_findings(report["buzz"] + report["consonant_loss"])
        for row in report["consonant_loss"]:
            for key in ("level_contrast_db", "high_band_contrast_db", "attack_contrast_db"):
                value = row["difference"][key]
                if value is not None:
                    self.assertAlmostEqual(value, 0.0, delta=0.2, msg=key)
            self.assertIsNotNone(row["difference"]["level_contrast_db"])

    def test_glide_distinguishes_stationary_peak_from_harmonic(self):
        cases = (("fixed", {"tone_hz": FIXED_HZ}), ("harmonic", {"harmonic_gain": 0.09}))
        for classification, options in cases:
            with self.subTest(classification=classification):
                self.prepare(**options)
                report = self.compare()
                changed, unchanged = report["buzz"]
                self.assertEqual((changed["id"], changed["word"], changed["phone"]),
                                 ("v1", WORDS[0], "ai"))
                self.assertEqual((changed["status"], changed["rank"]), ("finding", 1), changed)
                self.assertEqual(changed["classification"], classification, changed)
                peak = changed["candidate"][classification]
                self.assertGreater(peak["strength_db"], 0)
                self.assertGreater(peak["persistence"], 0.5)
                if classification == "fixed":
                    self.assertAlmostEqual(peak["frequency_hz"], FIXED_HZ, delta=25)
                    self.assertIsNone(changed["reference"]["fixed"])
                    self.assertAlmostEqual(changed["reference"]["fixed_probe"]["frequency_hz"],
                                           peak["frequency_hz"], delta=0.01)
                    self.assertGreater(changed["difference"]["fixed"]["relative_level_db"], 3)
                else:
                    self.assertEqual(peak["harmonic_number"], 7)
                self.assert_no_findings([unchanged])
                self.alignment.unlink()

    def test_steady_pitch_abstains_when_tone_coincides_with_harmonic(self):
        self.prepare(steady=True, tone_hz=7 * 190)
        report = self.compare()
        changed = next(row for row in report["buzz"] if row["id"] == "v1")
        self.assertEqual(changed["status"], "indeterminate", changed)
        self.assertIsNone(changed["classification"])
        self.assertIsNone(changed["rank"])

    def test_lost_release_flags_transition_and_preserves_neighboring_vowel(self):
        reference, candidate = self.prepare(release_gain=10 ** (-18 / 20))
        report = self.compare()
        changed, unchanged = report["consonant_loss"]
        self.assertEqual((changed["id"], changed["word"], changed["phone"]),
                         ("c1", WORDS[0], "t"))
        self.assertEqual((changed["status"], changed["rank"]), ("finding", 1), changed)
        self.assertGreaterEqual(changed["alignment_confidence"], 0.35)
        self.check_span(changed["reference_span"], reference[0])
        self.check_span(changed["candidate_span"], candidate[0])
        for side, rows in (("reference", reference), ("candidate", candidate)):
            transition = changed[f"{side}_transition"]
            edge = rows[0][2]
            self.assertLess(transition["start_sample"], edge)
            self.assertGreater(transition["end_sample"], edge)
        self.assertLess(changed["difference"]["high_band_contrast_db"], -6)
        self.assert_no_findings([unchanged, *report["buzz"]])

    def test_start_edge_uses_vowel_before_consonant(self):
        ref_rows, ref_anchors = phrase(self.reference)
        _, cand_anchors = phrase(self.candidate, release_gain=10 ** (-18 / 20))
        # Reverse the fixture to put the burst at the consonant's start.
        for path in (self.reference, self.candidate):
            write_wave(path, read_samples(path)[::-1])
        frames = ref_anchors[-1]
        anchors = [frames - point for point in reversed(ref_anchors)]
        self.assertEqual(ref_anchors, cand_anchors)
        self.save_alignment(anchors, anchors)
        rows = [(id_, frames - end, frames - start, word, phone, kind,
                 vowel_id, "start" if edge else "", confidence)
                for id_, start, end, word, phone, kind, vowel_id, edge, confidence
                in reversed(ref_rows)]
        write_csv(self.spans, COLUMNS, rows)
        report = self.compare()
        changed, unchanged = report["consonant_loss"]
        self.assertEqual((changed["id"], changed["status"]), ("c1", "finding"), changed)
        self.assertLess(changed["difference"]["high_band_contrast_db"], -6)
        self.assert_no_findings([unchanged])

    def test_consonant_evidence_ranks_larger_loss_first(self):
        self.prepare(release_gain=10 ** (-8 / 20), second_release_gain=10 ** (-18 / 20))
        report = self.compare()
        self.assertEqual([(row["id"], row["rank"]) for row in report["consonant_loss"]],
                         [("c2", 1), ("c1", 2)])
        self.assert_no_findings(report["buzz"])

    def assert_excerpt_content(self, path, span, bounds, original):
        self.assertTrue(path.is_file(), path)
        with wave.open(str(path), "rb") as audio:
            self.assertEqual((audio.getframerate(), audio.getnchannels(), audio.getsampwidth()),
                             (RATE, 1, 2))
        start, end = span["start_sample"], span["end_sample"]
        self.assertLessEqual(start, bounds[1])
        self.assertGreaterEqual(end - bounds[2], round(0.08 * RATE))
        excerpt = read_samples(path)
        self.assertEqual(len(excerpt), end - start)
        source = read_samples(original)[start:end]
        gain = rms(excerpt) / rms(source)
        self.assertLessEqual(max(abs(a - gain * b) for a, b in zip(excerpt, source)), 2)
        return rms(excerpt)

    def test_retimed_excerpt_plays_the_labeled_sound_in_both_wavs(self):
        reference, candidate = self.prepare(retimed=True, release_gain=10 ** (-18 / 20))
        excerpts = self.root / "A B"
        excerpts.mkdir()
        report = self.compare("--excerpt-dir", excerpts)
        changed = report["consonant_loss"][0]
        self.assertEqual((changed["id"], changed["rank"]), ("c1", 1), changed)
        self.assertNotEqual(reference[0][2], candidate[0][2])
        self.check_span(changed["reference_span"], reference[0])
        self.check_span(changed["candidate_span"], candidate[0])
        excerpt = changed["excerpt"]
        levels = []
        for side, spans, audio in (("reference", reference, self.reference),
                                   ("candidate", candidate, self.candidate)):
            path = Path(excerpt[f"{side}_path"])
            self.assertEqual(path.parent, excerpts)
            levels.append(self.assert_excerpt_content(path, excerpt[f"{side}_span"], spans[0], audio))
        self.assertAlmostEqual(20 * math.log10(levels[1] / levels[0]), 0.0, delta=0.05)

    def test_excerpt_collision_keeps_existing_file_and_rolls_back_new_file(self):
        self.prepare(release_gain=10 ** (-18 / 20))
        excerpts = self.root / "A B"
        excerpts.mkdir()
        report = self.compare("--excerpt-dir", excerpts)
        pair = report["consonant_loss"][0]["excerpt"]
        reference = Path(pair["reference_path"])
        candidate = Path(pair["candidate_path"])
        reference.unlink()
        original = candidate.read_bytes()
        output = self.root / "speech.json"
        result = self.cli("compare-speech-spans", self.reference, self.candidate,
                          "--alignment", self.alignment, "--spans", self.spans,
                          "--excerpt-dir", excerpts, "--output", output)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())
        self.assertFalse(reference.exists())
        self.assertEqual(candidate.read_bytes(), original)

    def test_low_confidence_keeps_mapped_bounds_and_rank_empty(self):
        reference, candidate = self.prepare(retimed=True, low_confidence=True,
                                            release_gain=10 ** (-18 / 20))
        excerpts = self.root / "untrusted excerpts"
        excerpts.mkdir()
        report = self.compare("--excerpt-dir", excerpts)
        changed = next(row for row in report["consonant_loss"] if row["id"] == "c1")
        self.assertEqual(changed["status"], "low-alignment-confidence", changed)
        self.assertAlmostEqual(changed["alignment_confidence"], 0.1)
        self.check_span(changed["reference_span"], reference[0])
        self.check_span(changed["candidate_span"], candidate[0])
        self.assertIsNone(changed["rank"])
        self.assertIsNone(changed["excerpt"])

    def test_saved_uncertain_interval_blocks_rank_between_confident_anchors(self):
        reference, candidate = self.prepare(retimed=True, release_gain=10 ** (-18 / 20))
        alignment = self.alignment.read_bytes()
        edge = reference[0][2]
        for reason in ("low-confidence", "no-evidence"):
            with self.subTest(reason=reason):
                start = (edge - 12) / RATE
                end = (edge + 12) / RATE
                gap = f"UNMATCHED,1,reference,{start:.17g},{end:.17g},,,{reason},0.1\r\n"
                self.alignment.write_bytes(alignment + gap.encode("ascii"))
                report = self.compare()
                changed = next(row for row in report["consonant_loss"] if row["id"] == "c1")
                self.assertIn(changed["status"], ("unmatched-alignment", "low-alignment-confidence"))
                self.check_span(changed["reference_span"], reference[0])
                self.check_span(changed["candidate_span"], candidate[0])
                self.assertIsNone(changed["rank"])

    def test_consonant_rank_requires_confident_neighboring_vowel(self):
        reference, _ = self.prepare(release_gain=10 ** (-18 / 20))
        vowel = list(reference[1])
        vowel[COLUMNS.index("confidence")] = 0.1
        write_csv(self.spans, COLUMNS, [reference[0], vowel, *reference[2:]])
        report = self.compare()
        changed = next(row for row in report["consonant_loss"] if row["id"] == "c1")
        self.assertAlmostEqual(changed["alignment_confidence"], 0.1)
        self.assertEqual(changed["status"], "low-alignment-confidence", changed)
        self.assertIsNone(changed["rank"])

    def test_consonant_requires_a_known_vowel(self):
        reference, _ = self.prepare()
        first = list(reference[0])
        first[COLUMNS.index("vowel_id")] = "missing"
        write_csv(self.spans, COLUMNS, [first, *reference[1:]])
        result = self.cli("compare-speech-spans", self.reference, self.candidate,
                          "--alignment", self.alignment, "--spans", self.spans)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("vowel", result.stderr.lower())
        self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--analyzer", required=True, type=Path)
    args, rest = parser.parse_known_args()
    ANALYZER = args.analyzer
    unittest.main(argv=[__file__, *rest])

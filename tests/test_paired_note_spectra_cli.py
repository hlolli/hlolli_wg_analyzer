#!/usr/bin/env python3
"""Compare note spectra through a saved audio-to-audio alignment."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import wave


ANALYZER = None
RATE = 16000
PITCHES = (220.0, 277.0, 330.0, 392.0)
NOTE_COLUMNS = ("id", "start_sample", "end_sample", "expected_hz", "register", "string")
SCORE_COLUMNS = ("event_id", "kind", "start_beats", "duration_beats", "midi_note",
                 "velocity", "voice", "tie", "dynamic", "mark", "score_position", "tempo_bpm")


def write_wave(path, samples, rate=RATE):
    with wave.open(str(path), "wb") as audio:
        audio.setparams((1, 2, rate, 0, "NONE", "not compressed"))
        audio.writeframes(struct.pack(f"<{len(samples)}h", *samples))


def read_samples(path):
    with wave.open(str(path), "rb") as audio:
        frames = audio.getnframes()
        return struct.unpack(f"<{frames}h", audio.readframes(frames))


def recording(path, timing, gain, boosted_note=None):
    cursor = RATE // 4
    samples = [0] * cursor
    spans = []
    for index, (pitch, (duration, gap)) in enumerate(zip(PITCHES, timing)):
        frames = round(duration * RATE)
        middle_hz = pitch * math.ceil(2200.0 / pitch)
        high_hz = pitch * math.ceil(3300.0 / pitch)
        high_gain = 0.24 if index == boosted_note else 0.12
        for frame in range(frames):
            t = frame / RATE
            envelope = min(1.0, frame / 560, (frames - 1 - frame) / 560)
            value = gain * envelope * (
                math.sin(math.tau * pitch * t)
                + 0.35 * math.sin(math.tau * 2 * pitch * t)
                + 0.2 * math.sin(math.tau * middle_hz * t)
                + high_gain * math.sin(math.tau * high_hz * t)
            )
            samples.append(round(32767 * value))
        spans.append((cursor, cursor + frames))
        cursor += frames + round(gap * RATE)
        samples.extend([0] * round(gap * RATE))
    write_wave(path, samples)
    return spans


def write_csv(path, columns, rows):
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(columns)
        writer.writerows(rows)


class PairedNoteSpectraTests(unittest.TestCase):
    def cli(self, *args):
        return subprocess.run([str(ANALYZER), *map(str, args)], capture_output=True,
                              encoding="utf-8", timeout=30)

    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="hwa paired spectra ")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.reference = self.root / "reference.wav"
        self.target = self.root / "target.wav"
        self.alignment = self.root / "phrase.hwa-align"
        self.reference_spans = recording(self.reference, ((0.8, 0.22), (0.92, 0.18),
                                         (1.08, 0.23), (0.84, 0.30)), gain=0.4)
        self.target_spans = recording(self.target, ((0.66, 0.18), (1.03, 0.21),
                                      (0.89, 0.17), (1.02, 0.24)), gain=0.2,
                                      boosted_note=1)
        self.assertNotEqual(self.reference_spans, self.target_spans)
        saved = self.cli("align", self.reference, self.target, "--output", self.alignment)
        self.assertEqual(saved.returncode, 0, saved.stderr)

    def compare(self, *options):
        result = self.cli("compare-note-spectra", self.reference, self.target,
                          "--alignment", self.alignment, "--band", "2000:3000",
                          "--band", "3000:4000", "--output", "-", *options)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_aligned_held_notes_detect_high_band_change_and_reject_wrong_pitch(self):
        notes = self.root / "notes.csv"
        note_rows = [(f"n{index + 1}", start, end,
                      PITCHES[index] if index < 3 else 523.25,
                      "low" if index < 2 else "high", "G" if index % 2 == 0 else "D")
                     for index, (start, end) in enumerate(self.reference_spans)]
        write_csv(notes, NOTE_COLUMNS, note_rows)
        report = self.compare("--notes", notes, "--group-by", "register",
                              "--group-by", "string")
        self.assertEqual(report["schema"], "hwa-paired-note-spectra")
        summary = report["summary"]
        self.assertEqual((summary["usable_pairs"], summary["skipped_pairs"]), (3, 1))
        rows = report["notes"]
        self.assertEqual([row["id"] for row in rows], ["n1", "n2", "n3", "n4"])
        skipped = rows[-1]
        self.assertEqual(skipped["status"], "skipped")
        self.assertIn(skipped["skip_reason"],
                      ("reference-pitch-mismatch", "target-pitch-mismatch"))
        self.assertEqual(summary["skip_reasons"][skipped["skip_reason"]], 1)
        self.assertEqual(sum(summary["skip_reasons"].values()), 1)

        for index, row in enumerate(rows[:3]):
            with self.subTest(note=row["id"]):
                self.assertEqual(row["status"], "usable")
                self.assertIsNone(row["skip_reason"])
                self.assertGreaterEqual(row["confidence"], 0.35)
                self.assertLessEqual(row["confidence"], 1.0)
                ref, model = row["reference"], row["target"]
                self.assertEqual((ref["start_sample"], ref["end_sample"]),
                                 self.reference_spans[index])
                for field, known in zip(("start_sample", "end_sample"),
                                        self.target_spans[index]):
                    self.assertAlmostEqual(model[field] / RATE, known / RATE,
                                           delta=0.08)
                for side in (ref, model):
                    self.assertLessEqual(side["start_sample"], side["held_start_sample"])
                    self.assertLess(side["held_start_sample"], side["held_end_sample"])
                    self.assertLessEqual(side["held_end_sample"], side["end_sample"])
                    self.assertAlmostEqual(side["pitch_hz"], PITCHES[index], delta=2.0)
                    self.assertGreater(side["pitch_confidence"], 0.0)
                    self.assertGreater(side["window_count"], 0)
                    self.assertGreater(side["total_power"], 0.0)
                bands = row["bands"]
                self.assertEqual([(b["low_hz"], b["high_hz"]) for b in bands],
                                 [(2000, 3000), (3000, 4000)])
                # The mean square of each sinusoid is half its amplitude squared.
                for band, reference_amplitude, target_amplitude in zip(
                        bands, (0.4 * 0.2, 0.4 * 0.12),
                        (0.2 * 0.2, 0.2 * (0.24 if index == 1 else 0.12))):
                    self.assertAlmostEqual(band["reference_power"],
                                           reference_amplitude ** 2 / 2, delta=0.00001)
                    self.assertAlmostEqual(band["target_power"],
                                           target_amplitude ** 2 / 2, delta=0.00001)
                self.assertAlmostEqual(bands[0]["absolute_difference_db"], -6.02,
                                       delta=0.35)
                self.assertAlmostEqual(bands[0]["normalized_difference_db"], 0.0,
                                       delta=0.35)
                if index == 1:
                    self.assertAlmostEqual(bands[1]["absolute_difference_db"], 0.0,
                                           delta=0.35)
                    self.assertAlmostEqual(bands[1]["normalized_difference_db"], 5.86,
                                           delta=0.40)
                else:
                    self.assertAlmostEqual(bands[1]["absolute_difference_db"], -6.02,
                                           delta=0.35)
                    self.assertAlmostEqual(bands[1]["normalized_difference_db"], 0.0,
                                           delta=0.35)

        counts = {(group["label"], group["value"]):
                  (group["usable_pairs"], group["skipped_pairs"])
                  for group in report["groups"]}
        self.assertEqual(counts, {("register", "low"): (2, 0),
                                  ("register", "high"): (1, 1),
                                  ("string", "G"): (2, 0),
                                  ("string", "D"): (1, 1)})
        self.assertAlmostEqual(summary["bands"][1]["mean_normalized_difference_db"],
                               5.86 / 3, delta=0.2)

    def test_only_requested_spans_use_decode_and_transform_budget(self):
        single_note = self.root / "one note.csv"
        start, end = self.reference_spans[2]
        write_csv(single_note, NOTE_COLUMNS, [("n3", start, end, PITCHES[2], "high", "G")])
        max_frames, max_transforms = 24000, 80
        for source in (self.reference, self.target):
            with wave.open(str(source), "rb") as audio:
                self.assertGreater(audio.getnframes(), max_frames)

        report = self.compare("--notes", single_note, "--max-frames", max_frames,
                              "--max-transforms", max_transforms)
        self.assertEqual(report["summary"]["usable_pairs"], 1)
        self.assertEqual(len(report["notes"]), 1)
        note = report["notes"][0]
        self.assertEqual(note["id"], "n3")
        self.assertEqual((note["reference"]["start_sample"], note["reference"]["end_sample"]),
                         (start, end))
        self.assertGreater(note["target"]["start_sample"], 2 * RATE)
        self.assertAlmostEqual(note["bands"][1]["normalized_difference_db"], 0.0, delta=0.35)
        for field, limit in (("decoded_frames", max_frames), ("transforms", max_transforms)):
            self.assertGreater(report["work"][field], 0)
            self.assertLessEqual(report["work"][field], limit)

    def test_score_alignment_supplies_notes_pitches_and_labels(self):
        score = self.root / "score.csv"
        # At 60 BPM the nominal beat positions use the same scale as seconds.
        rows = [("tempo", "tempo", 0, 0, "", "", "", "", "", "", "m1", 60)]
        midi_notes = (57, 61, 64, 67)
        for index, ((start, end), midi) in enumerate(zip(self.reference_spans, midi_notes), 1):
            rows.append((f"s{index}", "note", start / RATE, (end - start) / RATE,
                         midi, 88, "solo", "none", "mf", "plain", f"m1b{index}", ""))
        write_csv(score, SCORE_COLUMNS, rows)
        for audio in (self.reference, self.target):
            with self.subTest(score_aligned_to=audio.name):
                score_alignment = audio.with_suffix(".hwa-align")
                saved = self.cli("align", "--score", score, audio, "--output", score_alignment)
                self.assertEqual(saved.returncode, 0, saved.stderr)
                report = self.compare("--score-alignment", score_alignment, "--group-by", "voice")
                self.assertEqual(report["summary"]["usable_pairs"], 4)
                self.assertEqual(report["summary"]["skipped_pairs"], 0)
                self.assertEqual([row["id"] for row in report["notes"]], ["s1", "s2", "s3", "s4"])
                note = report["notes"][1]
                self.assertAlmostEqual(note["expected_hz"], 277.18, delta=0.01)
                self.assertAlmostEqual(note["reference"]["pitch_hz"], PITCHES[1], delta=2.0)
                self.assertAlmostEqual(note["target"]["pitch_hz"], PITCHES[1], delta=2.0)
                for side, spans in (("reference", self.reference_spans), ("target", self.target_spans)):
                    for field, known in zip(("start_sample", "end_sample"), spans[1]):
                        self.assertAlmostEqual(note[side][field] / RATE, known / RATE, delta=0.08)
                self.assertAlmostEqual(note["bands"][1]["normalized_difference_db"],
                                       5.86, delta=0.40)
                self.assertEqual(len(report["groups"]), 1)
                group = report["groups"][0]
                self.assertEqual((group["label"], group["value"], group["usable_pairs"]),
                                 ("voice", "solo", 4))

    def test_low_alignment_confidence_skips_without_transforms(self):
        notes = self.root / "notes.csv"
        start, end = self.reference_spans[2]
        write_csv(notes, NOTE_COLUMNS, [("n3", start, end, PITCHES[2], "high", "G")])
        report = self.compare("--notes", notes, "--min-pair-confidence", 1.0)
        self.assertEqual(report["summary"]["usable_pairs"], 0)
        self.assertEqual(report["summary"]["skipped_pairs"], 1)
        self.assertEqual(report["summary"]["skip_reasons"], {"low-alignment-confidence": 1})
        note = report["notes"][0]
        self.assertEqual(note["status"], "skipped")
        self.assertEqual(note["skip_reason"], "low-alignment-confidence")
        self.assertGreater(note["confidence"], 0.0)
        self.assertLess(note["confidence"], 1.0)
        self.assertEqual(report["work"]["transforms"], 0)
        self.assertEqual(report["work"]["decoded_frames"], 0)

    def test_alignment_rejects_changed_recording_of_same_duration(self):
        notes = self.root / "notes.csv"
        start, end = self.reference_spans[2]
        write_csv(notes, NOTE_COLUMNS, [("n3", start, end, PITCHES[2], "high", "G")])
        with self.target.open("r+b") as audio:
            # Alter the last sample in the trailing rest and keep the WAVE geometry.
            audio.seek(-2, 2)
            audio.write(struct.pack("<h", 1))
        output = self.root / "report.json"
        result = self.cli("compare-note-spectra", self.reference, self.target,
                          "--alignment", self.alignment, "--notes", notes, "--output", output)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("target WAVE SHA-256 differs from the alignment", result.stderr)
        self.assertFalse(output.exists())

    def test_proxy_alignment_maps_target_clock_notes_and_analyzes_originals(self):
        reference_proxy = self.root / "reference proxy.wav"
        target_proxy = self.root / "target proxy.wav"
        proxy_alignment = self.root / "proxy.hwa-align"
        reference_samples = read_samples(self.reference)
        target_samples = read_samples(self.target)
        # All generated tones are below the proxies' 4 kHz Nyquist frequency.
        write_wave(reference_proxy, reference_samples[::2], RATE // 2)
        write_wave(target_proxy, target_samples[::2], RATE // 2)
        saved = self.cli("align", reference_proxy, target_proxy, "--output", proxy_alignment)
        self.assertEqual(saved.returncode, 0, saved.stderr)

        notes = self.root / "target notes.csv"
        start, end = self.target_spans[2]
        write_csv(notes, NOTE_COLUMNS, [("n3", start, end, PITCHES[2], "high", "G")])
        options = ("--alignment", proxy_alignment, "--alignment-reference", reference_proxy,
                   "--alignment-target", target_proxy, "--notes", notes, "--notes-clock", "target",
                   "--band", "3000:4000", "--band", "5000:6000", "--output", "-")
        result = self.cli("compare-note-spectra", self.reference, self.target, *options)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["summary"]["usable_pairs"], 1)
        self.assertEqual((report["reference"]["rate_hz"], report["target"]["rate_hz"]),
                         (RATE, RATE))
        for side, original, proxy in (("reference", self.reference, reference_proxy),
                                      ("target", self.target, target_proxy)):
            self.assertEqual(report[side]["sha256"], hashlib.sha256(original.read_bytes()).hexdigest())
            self.assertEqual(report[f"alignment_{side}_sha256"],
                             hashlib.sha256(proxy.read_bytes()).hexdigest())
        self.assertEqual(report["settings"]["normalization_max_hz"], RATE / 2)
        self.assertEqual(report["notes_clock"], "target")
        note = report["notes"][0]
        self.assertEqual(note["id"], "n3")
        self.assertEqual((note["target"]["start_sample"], note["target"]["end_sample"]), (start, end))
        for field, known in zip(("start_sample", "end_sample"), self.reference_spans[2]):
            self.assertAlmostEqual(note["reference"][field] / RATE, known / RATE, delta=0.08)
        self.assertAlmostEqual(note["bands"][0]["normalized_difference_db"], 0.0, delta=0.35)

        custom = self.cli("compare-note-spectra", self.reference, self.target,
                          *options, "--normalize-band", "200:1000")
        self.assertEqual(custom.returncode, 0, custom.stderr)
        custom_report = json.loads(custom.stdout)
        self.assertEqual(custom_report["settings"]["normalization_min_hz"], 200)
        self.assertEqual(custom_report["settings"]["normalization_max_hz"], 1000)
        custom_note = custom_report["notes"][0]
        self.assertLess(custom_note["reference"]["total_power"], note["reference"]["total_power"])
        self.assertEqual(custom_note["bands"][0]["reference_power"],
                         note["bands"][0]["reference_power"])
        self.assertAlmostEqual(custom_note["bands"][0]["normalized_difference_db"], 0.0, delta=0.35)

        shortened = self.root / "shortened recording.wav"
        write_wave(shortened, reference_samples[:-RATE // 10])
        wrong_duration = self.cli("compare-note-spectra", shortened, self.target, *options)
        self.assertNotEqual(wrong_duration.returncode, 0)
        self.assertIn("duration", wrong_duration.stderr)
        self.assertEqual(wrong_duration.stdout, "")

        with target_proxy.open("r+b") as audio:
            audio.seek(-2, 2)
            audio.write(struct.pack("<h", 1))
        wrong_hash = self.cli("compare-note-spectra", self.reference, self.target, *options)
        self.assertNotEqual(wrong_hash.returncode, 0)
        self.assertIn("SHA-256", wrong_hash.stderr)
        self.assertEqual(wrong_hash.stdout, "")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--analyzer", required=True, type=Path)
    args, rest = parser.parse_known_args()
    ANALYZER = args.analyzer
    unittest.main(argv=[__file__, *rest])

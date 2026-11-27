import argparse
import csv
import io
import math
import unittest
from pathlib import Path

import test_speech_spans_cli as speech


def csv_records(lines):
    output = io.StringIO()
    csv.writer(output).writerows(lines)
    return output.getvalue().encode("ascii")


def match_record(index, reference_start, reference_end, target_start, target_end, confidence,
                 status="matched"):
    return ("MATCH", index, reference_start, reference_end, target_start, target_end,
            "", "", confidence, status, *("" for _ in range(10)), 1)


class SpeechAlignmentRegressions(unittest.TestCase):
    def setUp(self):
        self.fixture = speech.SpeechSpansTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)

    def test_saved_match_confidence_limits_a_retimed_finding(self):
        fixture = self.fixture
        reference, candidate = fixture.prepare(retimed=True, release_gain=10 ** (-18 / 20))
        alignment = fixture.alignment.read_bytes()
        before = fixture.compare()
        self.assertEqual(before["consonant_loss"][0]["id"], "c1")
        self.assertEqual(before["consonant_loss"][0]["rank"], 1)
        ref_start, ref_end = (sample / speech.RATE for sample in reference[0][1:3])
        target_start, target_end = (sample / speech.RATE for sample in candidate[0][1:3])

        for confidence, status, threshold in ((0.65, "matched", 0.8),
                                              (0.65, "low-confidence", 0.35)):
            with self.subTest(status=status, threshold=threshold):
                fixture.alignment.write_bytes(alignment + csv_records([
                    match_record(1, ref_start, ref_end, target_start, target_end, confidence, status),
                ]))
                report = fixture.compare("--min-pair-confidence", threshold)
                changed = next(row for row in report["consonant_loss"] if row["id"] == "c1")
                self.assertEqual(changed["status"], "low-alignment-confidence", changed)
                self.assertAlmostEqual(changed["alignment_confidence"], confidence)
                fixture.check_span(changed["candidate_span"], candidate[0])
                self.assertIsNone(changed["rank"])

    def test_overlapping_saved_intervals_use_minimum_on_both_clocks(self):
        fixture = self.fixture
        reference, candidate = fixture.prepare(retimed=True, release_gain=10 ** (-18 / 20))
        alignment = fixture.alignment.read_bytes()
        ref_start, ref_end = (sample / speech.RATE for sample in reference[0][1:3])
        target_start, target_end = (sample / speech.RATE for sample in candidate[0][1:3])
        fixture.alignment.write_bytes(alignment + csv_records([
            match_record(1, 0, ref_end, 0, target_end, 0.9),
            match_record(2, ref_start, ref_end, target_start, target_end, 0.7),
            ("UNMATCHED", 1, "reference", ref_end - 0.008, ref_end + 0.008, "", "",
             "low-confidence", 0.45),
            ("UNMATCHED", 2, "target", target_end - 0.01, target_end + 0.01, "", "",
             "no-evidence", 0.2),
            ("UNMATCHED", 3, "target", target_end - 0.005, target_end + 0.005, "", "",
             "low-confidence", 0.3),
        ]))
        changed = next(row for row in fixture.compare()["consonant_loss"] if row["id"] == "c1")
        self.assertAlmostEqual(changed["alignment_confidence"], 0.2)
        self.assertEqual(changed["status"], "low-alignment-confidence", changed)
        self.assertIsNone(changed["rank"])

    def test_proxy_rounding_maps_eof_only_if_the_alignment_reaches_it(self):
        fixture = self.fixture
        reference, ref_anchors = speech.phrase(fixture.reference)
        _, candidate_anchors = speech.phrase(fixture.candidate, retimed=True)
        reference_proxy = fixture.root / "reference-proxy.wav"
        candidate_proxy = fixture.root / "candidate-proxy.wav"
        speech.write_wave(reference_proxy, speech.read_samples(fixture.reference)[:-1])
        speech.write_wave(candidate_proxy, speech.read_samples(fixture.candidate)[:-1])

        final_vowel = list(reference[-1])
        final_vowel[2] = ref_anchors[-1]
        speech.write_csv(fixture.spans, speech.COLUMNS, [final_vowel])
        for reaches_eof in (True, False):
            with self.subTest(reaches_eof=reaches_eof):
                original_paths = fixture.reference, fixture.candidate
                fixture.reference, fixture.candidate = reference_proxy, candidate_proxy
                # The accepted proxies are 1 sample shorter. Omitting a second
                # sample at the last anchor leaves an actual uncovered suffix.
                offset = 1 if reaches_eof else 2
                try:
                    fixture.save_alignment([*ref_anchors[:-1], ref_anchors[-1] - offset],
                                           [*candidate_anchors[:-1], candidate_anchors[-1] - offset])
                finally:
                    fixture.reference, fixture.candidate = original_paths
                report = fixture.compare("--alignment-reference", reference_proxy,
                                         "--alignment-target", candidate_proxy)
                vowel = report["buzz"][0]
                self.assertEqual(vowel["reference_span"]["end_sample"], ref_anchors[-1])
                if reaches_eof:
                    self.assertEqual(vowel["candidate_span"]["end_sample"], candidate_anchors[-1])
                    self.assertTrue(math.isclose(vowel["alignment_confidence"], 1.0))
                    self.assertNotEqual(vowel["status"], "outside-alignment")
                else:
                    self.assertEqual(vowel["status"], "outside-alignment")
                    self.assertIsNone(vowel["candidate_span"])
                fixture.alignment.unlink()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--analyzer", type=Path, required=True)
    args, rest = parser.parse_known_args()
    speech.ANALYZER = args.analyzer
    unittest.main(argv=[__file__, *rest])

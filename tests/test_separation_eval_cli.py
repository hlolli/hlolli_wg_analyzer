#!/usr/bin/env python3
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import unittest
import wave

PARSER = argparse.ArgumentParser()
PARSER.add_argument("--analyzer", required=True, type=Path)
ARGS, REST = PARSER.parse_known_args()


def pcm(path, samples, rate=16000, channels=1):
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(channels)
        stream.setsampwidth(2)
        stream.setframerate(rate)
        stream.writeframes(struct.pack("<" + "h" * len(samples), *samples))


def floats(path, samples):
    audio = struct.pack("<" + "d" * len(samples), *samples)
    body = (b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 3, 1, 16000, 128000, 8, 64) +
            b"data" + struct.pack("<I", len(audio)) + audio)
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


def extensible_stereo(path, mask):
    pcm_guid = bytes.fromhex("0100000000001000800000aa00389b71")
    fmt = struct.pack("<HHIIHHHHI", 0xfffe, 2, 16000, 64000, 4, 16, 22, 16, mask) + pcm_guid
    audio = struct.pack("<hhhhhhhh", 8192, 8192, -8192, -8192, 8192, 8192, -8192, -8192)
    body = b"WAVEfmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(audio)) + audio
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


class SeparationEvaluation(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hwa separation check ")
        self.root = Path(self.temp.name)
        self.reference = self.root / "reference.wav"
        self.estimate = self.root / "estimate.wav"
        self.mixture = self.root / "mixture.wav"
        pcm(self.reference, [8192, -8192, 8192, -8192] * 128)
        pcm(self.estimate, [12288, -4096, 4096, -12288] * 128)
        pcm(self.mixture, [16384, 0, 0, -16384] * 128)

    def tearDown(self):
        self.temp.cleanup()

    def run_cli(self, *extra, mixture=False, output="-"):
        command = [str(ARGS.analyzer), "evaluate-separation", str(self.reference),
                   str(self.estimate)]
        if mixture:
            command.append(str(self.mixture))
        return subprocess.run(command + ["--output", str(output), *extra],
                              capture_output=True, text=True, timeout=30)

    def report(self, **kwargs):
        result = self.run_cli(**kwargs)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_known_recovery_and_hashes(self):
        value = self.report(mixture=True)
        self.assertEqual(value["schema"], "hwa-separation-evaluation")
        self.assertEqual(value["method_version"], "separation-waveform-1")
        self.assertEqual(value["frames"], 512)
        self.assertEqual(value["rate_hz"], 16000)
        self.assertEqual(value["alignment"], "none")
        self.assertEqual(value["channel_policy"], "joint-signed-projection")
        self.assertAlmostEqual(value["estimate"]["si_sdr"]["db"], 10 * math.log10(4), places=11)
        self.assertAlmostEqual(value["mixture"]["si_sdr"]["db"], 0, places=11)
        self.assertAlmostEqual(value["si_sdr_improvement_db"], 10 * math.log10(4), places=11)
        self.assertAlmostEqual(value["estimate"]["reference_level_dbfs"]["db"],
                               20 * math.log10(0.25), places=11)
        self.assertAlmostEqual(value["estimate"]["estimate_level_dbfs"]["db"],
                               10 * math.log10(1.25 * 0.25 ** 2), places=11)
        self.assertAlmostEqual(value["estimate"]["error_level_dbfs"]["db"],
                               20 * math.log10(0.125), places=11)
        for label in ("reference", "estimate", "mixture"):
            self.assertEqual(value[label + "_sha256"], hashlib.sha256(
                getattr(self, label).read_bytes()).hexdigest())

    def test_no_overwrite_or_partial_output(self):
        output = self.root / "report.json"
        first = self.run_cli(output=output)
        self.assertEqual(first.returncode, 0, first.stderr)
        original = output.read_bytes()
        for path in (output, self.reference, self.estimate):
            before = path.read_bytes()
            failed = self.run_cli(output=path)
            self.assertNotEqual(failed.returncode, 0)
            self.assertEqual(path.read_bytes(), before)
        self.assertEqual(output.read_bytes(), original)
        self.estimate.write_bytes(b"invalid")
        missing = self.root / "invalid-output.json"
        self.assertNotEqual(self.run_cli(output=missing).returncode, 0)
        self.assertFalse(missing.exists())
        self.assertEqual(self.run_cli().stdout, "")

    def test_limits_and_unrelated_flags(self):
        for flags in (("--max-work-bytes", "16"), ("--max-bytes", "16"),
                      ("--max-frames", "511"), ("--frame-size", "1024"),
                      ("--max-transforms", "10"), ("--replace",),
                      ("--phase-partials",)):
            with self.subTest(flags=flags):
                result = self.run_cli(*flags)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")
        result = self.run_cli("--max-frames", "512", "--max-work-bytes", "65536")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_mismatched_clock_or_layout(self):
        for rate, channels, frames in ((8000, 1, 512), (16000, 2, 512), (16000, 1, 511)):
            pcm(self.estimate, [0] * frames * channels, rate, channels)
            result = self.run_cli()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("clocks or channel layouts differ", result.stderr)
            self.assertEqual(result.stdout, "")

    def test_no_hidden_time_alignment(self):
        rng = random.Random(19)
        values = [rng.randint(-12000, 12000) for _ in range(512)]
        pcm(self.reference, values)
        pcm(self.estimate, [0] + values[:-1])
        delayed = self.report()
        self.assertLess(delayed["estimate"]["si_sdr"]["db"], -10)
        pcm(self.estimate, values)
        self.assertEqual(self.report()["estimate"]["si_sdr"],
                         {"status": "positive-infinity", "db": None})

    def test_silence_and_gain_are_explicit(self):
        pcm(self.estimate, [-16384, 16384, -16384, 16384] * 128)
        value = self.report()["estimate"]
        self.assertEqual(value["si_sdr"]["status"], "positive-infinity")
        self.assertAlmostEqual(value["projection_gain"], -2)
        self.assertLess(value["snr"]["db"], 0)
        pcm(self.estimate, [0] * 512)
        value = self.report()["estimate"]
        self.assertTrue(value["estimate_silent"])
        self.assertEqual(value["si_sdr"], {"status": "undefined", "db": None})
        self.assertEqual(value["snr"], {"status": "finite", "db": 0})

    def test_float_extremes_and_nonfinite_samples(self):
        floats(self.reference, [1e-300, -1e-300, 1e-300, -1e-300])
        floats(self.estimate, [1.5e300, -0.5e300, 0.5e300, -1.5e300])
        value = self.report()["estimate"]
        self.assertAlmostEqual(value["si_sdr"]["db"], 10 * math.log10(4), places=11)
        self.assertIsNone(value["projection_gain"])
        self.assertTrue(math.isfinite(value["snr"]["db"]))
        self.assertAlmostEqual(value["reference_level_dbfs"]["db"], -6000, places=9)
        self.assertAlmostEqual(value["estimate_level_dbfs"]["db"],
                               6000 + 10 * math.log10(1.25), places=9)
        floats(self.estimate, [float("nan"), 0, 0, 0])
        self.assertNotEqual(self.run_cli().returncode, 0)
        self.assertEqual(self.run_cli().stdout, "")

    def test_silent_target_keeps_unwanted_output_level(self):
        pcm(self.reference, [0] * 512)
        pcm(self.estimate, [4096, -4096] * 256)
        value = self.report()["estimate"]
        self.assertEqual(value["si_sdr"], {"status": "undefined", "db": None})
        self.assertEqual(value["reference_level_dbfs"],
                         {"status": "negative-infinity", "db": None})
        self.assertAlmostEqual(value["estimate_level_dbfs"]["db"], 20 * math.log10(0.125))
        self.assertEqual(value["estimate_level_dbfs"], value["error_level_dbfs"])
        floats(self.reference, [1e-300, -1e-300] * 2)
        floats(self.estimate, [0] * 4)
        value = self.report()["estimate"]
        self.assertEqual(value["snr"], {"status": "finite", "db": 0})
        self.assertAlmostEqual(value["error_level_dbfs"]["db"], -6000, places=9)

    def test_speaker_masks_must_match(self):
        extensible_stereo(self.reference, 3)
        extensible_stereo(self.estimate, 3)
        self.assertEqual(self.report()["channel_mask"], 3)
        extensible_stereo(self.estimate, 12)
        result = self.run_cli()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("channel layouts differ", result.stderr)

    def test_sample_encodings_can_differ(self):
        pcm(self.reference, [8192, -8192, 8192, -8192])
        floats(self.estimate, [0.25, -0.25, 0.25, -0.25])
        value = self.report()["estimate"]
        self.assertEqual(value["snr"]["status"], "positive-infinity")
        self.assertEqual(value["si_sdr"]["status"], "positive-infinity")


if __name__ == "__main__":
    unittest.main(argv=[__file__, *REST])

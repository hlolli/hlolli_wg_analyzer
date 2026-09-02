#!/usr/bin/env python3

import importlib.util
import math
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import wave


TOOL = Path(__file__).resolve().parents[1] / "tools" / "bounded_pitch.py"
SPEC = importlib.util.spec_from_file_location("bounded_pitch", TOOL)
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class BoundedPitchTests(unittest.TestCase):
    def test_stereo_pcm16_sine_is_measured_near_expected_pitch(self):
        with tempfile.TemporaryDirectory() as text:
            path = Path(text) / "tone.wav"
            rate = 8000
            samples = bytearray()
            for index in range(rate):
                left = round(12000 * math.sin(2 * math.pi * 219.7 * index / rate))
                right = round(9000 * math.sin(2 * math.pi * 219.7 * index / rate + 0.4))
                samples.extend(struct.pack("<hh", left, right))
            with wave.open(str(path), "wb") as stream:
                stream.setnchannels(2)
                stream.setsampwidth(2)
                stream.setframerate(rate)
                stream.writeframes(samples)
            result = MODULE.analyze(path, 220.0, 0.0, 1.0, 0.5, 0.25, 50.0)
        self.assertEqual(result["schema"], "hwa-bounded-pitch")
        self.assertEqual(result["summary"]["frame_count_per_channel"], 3)
        self.assertEqual(result["summary"]["estimate_count"], 6)
        self.assertAlmostEqual(result["summary"]["pitch_hz"], 219.7, delta=0.1)
        self.assertEqual(result["request"]["channels"], [1, 2])

    def test_pcm24_sign_extension(self):
        values = [
            (b"\x00\x00\x00", 0.0),
            (b"\xff\xff\x7f", 8388607 / 8388608.0),
            (b"\x00\x00\x80", -1.0),
            (b"\xff\xff\xff", -1 / 8388608.0),
        ]
        for data, expected in values:
            with self.subTest(data=data):
                self.assertEqual(MODULE.decode_sample(data, 0, 3), expected)

    def test_wave_extensible_pcm24_is_read(self):
        with tempfile.TemporaryDirectory() as text:
            path = Path(text) / "extensible.wav"
            rate = 44100
            frame_data = b"\x01\x00\x00\xff\xff\xff" * 2
            format_data = struct.pack(
                "<HHIIHHHHI16s", 0xFFFE, 2, rate, rate * 6, 6, 24,
                22, 24, 3, MODULE.PCM_GUID)
            chunks = (b"fmt " + struct.pack("<I", len(format_data)) +
                      format_data + b"data" +
                      struct.pack("<I", len(frame_data)) + frame_data)
            path.write_bytes(b"RIFF" + struct.pack("<I", len(chunks) + 4) +
                             b"WAVE" + chunks)
            channels, sample_rate, sample_width, frame_count = (
                MODULE.read_pcm_wave(path))
        self.assertEqual(sample_rate, rate)
        self.assertEqual(sample_width, 3)
        self.assertEqual(frame_count, 2)
        self.assertEqual(list(channels[0]), [1 / 8388608.0] * 2)
        self.assertEqual(list(channels[1]), [-1 / 8388608.0] * 2)


if __name__ == "__main__":
    unittest.main()

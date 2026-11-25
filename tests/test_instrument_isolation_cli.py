#!/usr/bin/env python3
"""Check solo instrument isolation through the CLI."""

import argparse
import gzip
import hashlib
import io
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
FIXTURES = Path(__file__).with_name("fixtures") / "violin-isolation"
VIOLA_FIXTURES = FIXTURES.with_name("viola-isolation")


def write_pcm(path, samples, rate, channels, mask=0):
    audio = struct.pack(f"<{len(samples)}h", *samples)
    if mask:
        pcm_guid = bytes.fromhex("0100000000001000800000aa00389b71")
        align = channels * 2
        fmt = struct.pack("<HHIIHHHHI", 0xfffe, channels, rate, rate * align,
                          align, 16, 22, 16, mask) + pcm_guid
        body = (b"WAVEfmt " + struct.pack("<I", len(fmt)) + fmt +
                b"data" + struct.pack("<I", len(audio)) + audio)
        path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)
        return
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(channels)
        stream.setsampwidth(2)
        stream.setframerate(rate)
        stream.writeframes(audio)


def read_float_pcm(path):
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"Expected float32 RIFF WAVE in {path}")
    offset = 12
    while offset + 8 <= len(data):
        tag, length = struct.unpack_from("<4sI", data, offset)
        offset += 8
        if tag == b"data":
            return struct.unpack_from(f"<{length // 4}f", data, offset)
        offset += length + length % 2
    raise ValueError(f"Missing WAVE data in {path}")


def write_float_pcm(path, samples, rate):
    audio = struct.pack(f"<{len(samples)}f", *samples)
    fmt = struct.pack("<HHIIHHH", 3, 1, rate, rate * 4, 4, 32, 0)
    body = (b"WAVEfmt " + struct.pack("<I", len(fmt)) + fmt +
            b"fact" + struct.pack("<II", 4, len(samples)) +
            b"data" + struct.pack("<I", len(audio)) + audio)
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


def read_fixture(name, directory=FIXTURES):
    wav = io.BytesIO(gzip.decompress((directory / (name + ".wav.gz")).read_bytes()))
    with wave.open(wav, "rb") as stream:
        format_tuple = stream.getnchannels(), stream.getsampwidth(), stream.getframerate()
        if format_tuple != (1, 2, 22050):
            raise ValueError(f"Unexpected WAVE format in {name} fixture")
        frames = stream.getnframes()
        return struct.unpack(f"<{frames}h", stream.readframes(frames))


class InstrumentIsolationTest(unittest.TestCase):
    instrument = "violin"

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hwa instrument isolation ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source.wav"
        self.output = self.root / (self.instrument + ".hwa-events")

    def cli(self, *args):
        return subprocess.run([str(ARGS.analyzer), *map(str, args)],
                              capture_output=True, text=True, timeout=30)

    def isolate(self, *flags, instrument=None):
        return self.cli("isolate-instrument", self.source, "--instrument",
                        self.instrument if instrument is None else instrument,
                        "--output", self.output, *flags)

    def evaluate(self, reference, stem):
        report = self.cli("evaluate-separation", reference, stem, self.source, "--output", "-")
        self.assertEqual(report.returncode, 0, report.stderr)
        return json.loads(report.stdout)


class ViolinIsolation(InstrumentIsolationTest):
    def test_bundle_preserves_stereo_clock_and_adds_back_to_source(self):
        rate = 22050
        frames = 6023
        samples = [value for frame in range(frames)
                   for value in (round(6000 * math.sin(math.tau * 440 * frame / rate)),
                                 round(4500 * math.sin(math.tau * 440 * frame / rate + 0.6)))]
        write_pcm(self.source, samples, rate, 2, mask=3)
        result = self.isolate()
        self.assertEqual(result.returncode, 0, result.stderr)
        validated = self.cli("validate-event-bundle", self.output)
        self.assertEqual(validated.returncode, 0, validated.stderr)
        manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema"], "hwa-events")
        self.assertEqual(manifest["schema_version"], 1)
        audio = {row["kind"]: row for row in manifest["audio"]}
        source = audio["source-recording"]
        self.assertEqual(source["relative_path"], "")
        self.assertEqual(source["sha256"], hashlib.sha256(self.source.read_bytes()).hexdigest())
        for kind in audio:
            fmt = audio[kind]["format"]
            self.assertEqual((fmt["sample_rate_hz"], fmt["channels"], fmt["frames"]),
                             (rate, 2, frames))
            self.assertEqual(fmt["channel_mask"], 3)
        waveforms = []
        for kind in ("instrument-stem", "derived-audio"):
            row = audio[kind]
            self.assertEqual(row["source_recording_id"], source["id"])
            self.assertEqual(row["format"]["encoding"], "ieee-float")
            self.assertEqual(row["format"]["bits_per_sample"], 32)
            waveforms.append(read_float_pcm(self.output / row["relative_path"]))
        self.assertEqual([len(w) for w in waveforms], [2 * frames, 2 * frames])
        self.assertLess(max(abs(a + b - expected / 32768)
                            for a, b, expected in zip(*waveforms, samples)), 1e-7)
        events = [json.loads(line) for line in
                  (self.output / "events.jsonl").read_text(encoding="utf-8").splitlines()]
        event, = events
        self.assertEqual((event["kind"], event["start_sample"], event["end_sample"]),
                         ("instrument-region", 0, frames))
        self.assertEqual(event["evidence_audio_id"], audio["instrument-stem"]["id"])

    def real_mix(self, disturbances=(("cough", 7056, 2), ("step", 33075, 24))):
        reference = [round(value / 2) for value in read_fixture("violin")]
        mixed = reference.copy()
        ranges = []
        for name, offset, gain in disturbances:
            recording = read_fixture(name)
            for frame, sample in enumerate(recording, start=offset):
                mixed[frame] += round(gain * sample)
            ranges.append((offset - 256, offset + len(recording) + 256))
        reference_path = self.root / "reference.wav"
        write_pcm(reference_path, reference, 22050, 1)
        write_pcm(self.source, mixed, 22050, 1)
        return reference_path, reference, mixed, ranges

    def test_marked_cleanup_improves_real_mix_and_keeps_unmarked_audio(self):
        reference_path, reference, mixed, ranges = self.real_mix()
        start, end = ranges[0]
        split = (start + end) // 2
        spans = (ranges[1], (split, end), (start, split))
        flags = [item for start, end in spans
                 for item in ("--disturbance-samples", f"{start}:{end}")]
        result = self.isolate(*flags)
        self.assertEqual(result.returncode, 0, result.stderr)
        stem = self.output / "audio" / "violin.wav"
        removed = read_float_pcm(self.output / "audio" / "removed.wav")
        estimate = read_float_pcm(stem)
        self.assertEqual(len(estimate), len(mixed))
        self.assertEqual(len(removed), len(mixed))
        self.assertLess(max(abs(a + b - original / 32768)
                            for a, b, original in zip(estimate, removed, mixed)), 1e-7)
        for frame, original in enumerate(mixed):
            if all(frame < start or frame >= end for start, end in ranges):
                self.assertEqual(estimate[frame], original / 32768)
                self.assertEqual(removed[frame], 0.0)
        manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
        provider, = manifest["providers"]
        self.assertEqual(provider["settings"]["disturbance_samples"], [list(span) for span in ranges])
        value = self.evaluate(reference_path, stem)
        self.assertGreater(value["si_sdr_improvement_db"], 2.0, value)
        self.assertGreater(value["estimate"]["snr"]["db"], value["mixture"]["snr"]["db"])
        for name, (start, end) in zip(("cough", "footstep"), ranges):
            with self.subTest(disturbance=name):
                mixed_error = sum((mixed[i] - reference[i]) ** 2 for i in range(start, end))
                stem_error = sum((estimate[i] * 32768 - reference[i]) ** 2
                                 for i in range(start, end))
                self.assertLess(stem_error, mixed_error / 1.5)

    def test_automatic_cleanup_improves_real_mix(self):
        reference, _, _, _ = self.real_mix()
        result = self.isolate()
        self.assertEqual(result.returncode, 0, result.stderr)
        value = self.evaluate(reference, self.output / "audio" / "violin.wav")
        self.assertGreater(value["si_sdr_improvement_db"], 0.75, value)
        self.assertGreater(value["estimate"]["snr"]["db"], value["mixture"]["snr"]["db"])

    def test_automatic_cleanup_reduces_quiet_disturbances_over_violin(self):
        for name, offset, gain in (("cough", 7056, 0.5), ("step", 33075, 6)):
            with self.subTest(disturbance=name):
                reference, _, _, _ = self.real_mix(((name, offset, gain),))
                self.output = self.root / (name + ".hwa-events")
                result = self.isolate()
                self.assertEqual(result.returncode, 0, result.stderr)
                value = self.evaluate(reference, self.output / "audio" / "violin.wav")
                improvement = value["estimate"]["snr"]["db"] - value["mixture"]["snr"]["db"]
                self.assertGreater(improvement, 0.25, value)

    def test_automatic_cleanup_preserves_clean_violin(self):
        write_pcm(self.source, read_fixture("violin"), 22050, 1)
        result = self.isolate()
        self.assertEqual(result.returncode, 0, result.stderr)
        value = self.evaluate(self.source, self.output / "audio" / "violin.wav")
        snr = value["estimate"]["snr"]
        if snr["status"] != "positive-infinity":
            self.assertEqual(snr["status"], "finite")
            self.assertGreater(snr["db"], 40.0, value)

    def test_automatic_cleanup_preserves_a_short_note_between_equal_notes(self):
        rate = 48000
        start, end = rate // 2, rate // 2 + 1440
        phase = 0.0
        samples = []
        for frame in range(rate):
            phase += math.tau * (587.33 if start <= frame < end else 440) / rate
            edge = min(1.0, frame / 500, (rate - frame) / 500)
            partials = sum(gain * math.sin((index + 1) * phase)
                           for index, gain in enumerate((0.60, 0.31, 0.16, 0.10)))
            samples.append(round(edge * 10000 * partials))
        write_pcm(self.source, samples, rate, 1)
        result = self.isolate()
        self.assertEqual(result.returncode, 0, result.stderr)
        estimate = read_float_pcm(self.output / "audio" / "violin.wav")
        played_energy = sum(value ** 2 for value in samples[start:end])
        error_energy = sum((estimate[frame] * 32768 - samples[frame]) ** 2
                           for frame in range(start, end))
        self.assertLess(error_energy, played_energy / 100)

    def assert_cleanup_attenuates_disturbances_in_a_rest(self, marked):
        violin = read_fixture("violin")
        start = 26000
        rest_start, rest_end = 20000, 40000
        rng = random.Random(31)
        room_floor = [rng.randint(-24, 24) for _ in range(rest_end - rest_start)]
        reference = list(violin[:rest_start]) + room_floor + list(violin[rest_start:])
        for name, gain in (("cough", 2), ("step", 24)):
            with self.subTest(disturbance=name):
                disturbance = [gain * value for value in read_fixture(name)]
                end = start + len(disturbance)
                mixed = reference.copy()
                for i, value in enumerate(disturbance, start):
                    mixed[i] += value
                write_pcm(self.source, mixed, 22050, 1)
                self.output = self.root / (name + ".hwa-events")
                flags = ("--disturbance-samples", f"{start - 256}:{end + 256}") if marked else ()
                result = self.isolate(*flags)
                self.assertEqual(result.returncode, 0, result.stderr)
                stem = read_float_pcm(self.output / "audio" / "violin.wav")
                mixed_energy = sum(value ** 2 for value in disturbance)
                stem_error = sum((stem[i] * 32768 - reference[i]) ** 2
                                 for i in range(start, end))
                self.assertLess(stem_error, mixed_energy / 1.5)
                violin_error = sum((actual * 32768 - value) ** 2
                                   for actual, value in zip(stem[:rest_start] + stem[rest_end:],
                                                            violin))
                self.assertLess(violin_error, sum(value ** 2 for value in violin) / 10000)

    def test_marked_cleanup_attenuates_disturbances_in_a_rest(self):
        self.assert_cleanup_attenuates_disturbances_in_a_rest(marked=True)

    def test_automatic_cleanup_attenuates_disturbances_in_a_rest(self):
        self.assert_cleanup_attenuates_disturbances_in_a_rest(marked=False)

    def test_automatic_cleanup_preserves_a_short_isolated_note(self):
        rate = 22050
        start = 26000
        violin = read_fixture("violin")
        reference = list(violin[:20000]) + [0] * 20000 + list(violin[20000:])
        for name, hz, duration_ms in (("G3", 196, 30), ("D5", 587.33, 20), ("D5", 587.33, 10)):
            with self.subTest(note=name, duration_ms=duration_ms):
                length = round(rate * duration_ms / 1000)
                samples = reference.copy()
                for i in range(length):
                    phase = math.tau * hz * i / rate
                    envelope = math.sin(math.pi * i / length) ** 2
                    partials = math.sin(phase) + 0.4 * math.sin(2 * phase) + 0.2 * math.sin(3 * phase)
                    samples[start + i] = round(3000 * envelope * partials)
                write_pcm(self.source, samples, rate, 1)
                self.output = self.root / f"{name}-{duration_ms}ms.hwa-events"
                result = self.isolate()
                self.assertEqual(result.returncode, 0, result.stderr)
                stem = read_float_pcm(self.output / "audio" / "violin.wav")
                note_energy = sum(value ** 2 for value in samples[start:start + length])
                error_energy = sum((stem[frame] * 32768 - samples[frame]) ** 2
                                   for frame in range(start, start + length))
                self.assertLess(error_energy, note_energy / 100)

    def test_float_headroom_survives_isolation_and_a_second_pass(self):
        rate = 22050
        frames = rate // 2
        samples = [1.3 * math.sin(math.tau * 440 * frame / rate)
                   for frame in range(frames)]
        write_float_pcm(self.source, samples, rate)
        source = read_float_pcm(self.source)
        result = self.isolate("--disturbance-samples", f"0:{frames}")
        self.assertEqual(result.returncode, 0, result.stderr)
        stem_path = self.output / "audio" / "violin.wav"
        stem = read_float_pcm(stem_path)
        removed = read_float_pcm(self.output / "audio" / "removed.wav")
        self.assertGreater(max(map(abs, stem)), 1.0)
        self.assertLess(max(abs(a + b - value) for a, b, value in
                            zip(stem, removed, source)), 1e-6)
        self.source = stem_path
        self.output = self.root / "second.hwa-events"
        result = self.isolate()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_source_filename_does_not_change_bundle_manifest(self):
        write_pcm(self.source, [0] * 1024, 22050, 1)
        result = self.isolate()
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = (self.output / "manifest.json").read_bytes()
        renamed = self.source.with_name("same audio, another name.wav")
        self.source.rename(renamed)
        self.source = renamed
        self.output = self.root / "same-audio.hwa-events"
        result = self.isolate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.output / "manifest.json").read_bytes(), manifest)

    def test_rejects_unsupported_instrument_and_bad_ranges_without_output(self):
        write_pcm(self.source, [0] * 1024, 22050, 1)
        unsupported = self.isolate(instrument="double-bass")
        self.assertNotEqual(unsupported.returncode, 0)
        self.assertFalse(self.output.exists())
        for spans in (("0:0",), ("20:10",), ("1:",), ("-1:8",), ("20:1025",),
                      ("18446744073709551616:18446744073709551617",),
                      ("10:20", "15:25")):
            with self.subTest(spans=spans):
                flags = [item for span in spans for item in ("--disturbance-samples", span)]
                result = self.isolate(*flags)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.output.exists())

    def test_invalid_input_and_existing_output_are_preserved(self):
        self.source.write_bytes(b"not a WAVE file")
        result = self.isolate()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())
        write_pcm(self.source, [0] * 2048, 22050, 1)
        first = self.isolate()
        self.assertEqual(first.returncode, 0, first.stderr)
        before = {path: path.read_bytes() for path in self.output.rglob("*") if path.is_file()}
        self.assertNotEqual(self.isolate().returncode, 0)
        after = {path: path.read_bytes() for path in self.output.rglob("*") if path.is_file()}
        self.assertEqual(after, before)

    def test_resource_limits_and_option_scope(self):
        write_pcm(self.source, [0] * 1024, 22050, 1)
        for limit, value in (("--max-frames", "1023"), ("--max-work-bytes", "16"),
                             ("--max-work-bytes", " -1")):
            with self.subTest(limit=limit):
                result = self.isolate(limit, value)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.output.exists())
        for flag, value in (("--instrument", "violin"), ("--disturbance-samples", "1:10")):
            result = self.cli("inspect", self.source, flag, value)
            self.assertNotEqual(result.returncode, 0)


class ViolaIsolation(InstrumentIsolationTest):
    instrument = "viola"

    def test_stereo_bundle_names_the_instrument_and_preserves_the_source_clock(self):
        rate = 22050
        recording = read_fixture("viola-c3", VIOLA_FIXTURES)[:rate // 3]
        samples = [sample for value in recording for sample in (20 * value, 12 * value)]
        write_pcm(self.source, samples, rate, 2, mask=3)
        result = self.isolate()
        self.assertEqual(result.returncode, 0, result.stderr)
        checked = self.cli("validate-event-bundle", self.output)
        self.assertEqual(checked.returncode, 0, checked.stderr)
        manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
        provider, = manifest["providers"]
        self.assertEqual(provider["settings"]["instrument"], "viola")
        audio = {row["kind"]: row for row in manifest["audio"]}
        for row in audio.values():
            fmt = row["format"]
            self.assertEqual((fmt["sample_rate_hz"], fmt["channels"], fmt["channel_mask"],
                              fmt["frames"]), (rate, 2, 3, len(recording)))
        target = audio["instrument-stem"]
        self.assertEqual((target["name"], target["relative_path"]), ("viola", "audio/viola.wav"))
        self.assertEqual(audio["derived-audio"]["relative_path"], "audio/removed.wav")
        events = (self.output / "events.jsonl").read_text(encoding="utf-8").splitlines()
        event, = map(json.loads, events)
        self.assertEqual((event["kind"], event["part"], event["evidence_audio_id"]),
                         ("instrument-region", "viola", target["id"]))
        stem = read_float_pcm(self.output / "audio" / "viola.wav")
        removed = read_float_pcm(self.output / "audio" / "removed.wav")
        self.assertEqual((len(stem), len(removed)), (len(samples), len(samples)))
        self.assertLess(max(abs(a + b - source / 32768)
                            for a, b, source in zip(stem, removed, samples)), 1e-7)

    def test_automatic_cleanup_preserves_the_open_c_fundamental(self):
        rate = 48000
        for hz in (130.81, 123.50):
            with self.subTest(hz=hz):
                samples = []
                for i in range(rate):
                    phase = math.tau * hz * i / rate
                    envelope = min(1.0, i / 2400, (rate - i) / 2400)
                    tone = math.sin(phase) + 0.25 * math.sin(phase * 2) + 0.15 * math.sin(phase * 3)
                    samples.append(round(10000 * envelope * tone))
                self.output = self.root / f"c3-{hz}.hwa-events"
                write_pcm(self.source, samples, rate, 1)
                result = self.isolate()
                self.assertEqual(result.returncode, 0, result.stderr)
                estimate = read_float_pcm(self.output / "audio" / "viola.wav")
                signal = sum(sample ** 2 for sample in samples)
                error = sum((value * 32768 - sample) ** 2 for value, sample in zip(estimate, samples))
                self.assertLess(error, signal / 10000)

    def test_automatic_and_marked_cleanup_improve_a_recorded_viola_mix(self):
        rate = 22050
        clean = [20 * value for value in read_fixture("viola-c3", VIOLA_FIXTURES)]
        mixed = clean.copy()
        spans = []
        for name, offset, gain in (("cough", 9000, 2), ("step", 32500, 24)):
            noise = read_fixture(name)
            for frame, sample in enumerate(noise, offset):
                mixed[frame] += gain * sample
            spans.append((offset - 256, offset + len(noise) + 256))
        reference = self.root / "reference.wav"
        write_pcm(reference, clean, rate, 1)
        write_pcm(self.source, mixed, rate, 1)
        marked = [value for start, end in spans
                  for value in ("--disturbance-samples", f"{start}:{end}")]
        for mode, flags, minimum in (("automatic", [], 0.25), ("marked", marked, 2.0)):
            with self.subTest(mode=mode):
                self.output = self.root / (mode + ".hwa-events")
                result = self.isolate(*flags)
                self.assertEqual(result.returncode, 0, result.stderr)
                stem = self.output / "audio" / "viola.wav"
                value = self.evaluate(reference, stem)
                self.assertGreater(value["si_sdr_improvement_db"], minimum, value)
                self.assertGreater(value["estimate"]["snr"]["db"], value["mixture"]["snr"]["db"])

    def test_marked_cleanup_keeps_double_stops_over_a_quiet_cough(self):
        rate = 22050
        start = rate // 3
        cough = read_fixture("cough")
        end = start + len(cough)
        chords = ((130.81, 196.0, 1.0), (130.81, 220.0, 1.0),
                  (196.0, 293.66, 1.0), (196.0, 293.66, 0.5))
        for first_hz, second_hz, balance in chords:
            with self.subTest(pitches=(first_hz, second_hz), balance=balance):
                clean = [round(4500 * sum(
                    amplitude * gain * math.sin(math.tau * hz * (partial + 1) * frame / rate)
                    for hz, amplitude in ((first_hz, 1.0), (second_hz, balance))
                    for partial, gain in enumerate((1.0, 0.4, 0.2, 0.1))))
                    for frame in range(rate)]
                mixed = clean.copy()
                for frame, value in enumerate(cough, start):
                    mixed[frame] += round(value / 8)
                write_pcm(self.source, mixed, rate, 1)
                self.output = self.root / f"double-stop-{first_hz}-{second_hz}-{balance}.hwa-events"
                result = self.isolate("--disturbance-samples", f"{start - 256}:{end + 256}")
                self.assertEqual(result.returncode, 0, result.stderr)
                estimate = read_float_pcm(self.output / "audio" / "viola.wav")
                mixture_error = sum((mixed[i] - clean[i]) ** 2 for i in range(start, end))
                stem_error = sum((estimate[i] * 32768 - clean[i]) ** 2 for i in range(start, end))
                self.assertLess(stem_error, 1.25 * mixture_error)


class CelloIsolation(InstrumentIsolationTest):
    instrument = "cello"

    @staticmethod
    def bowed_note(rate, frames, hz, amplitude, edge_ms=40):
        envelope_edge = rate * edge_ms // 1000
        samples = []
        for i in range(frames):
            phase = math.tau * hz * i / rate
            envelope = min(1.0, i / envelope_edge, (frames - i) / envelope_edge)
            tone = (math.sin(phase) + 0.45 * math.sin(2 * phase)
                    + 0.2 * math.sin(3 * phase) + 0.1 * math.sin(4 * phase))
            samples.append(round(amplitude * envelope * tone))
        return samples

    @staticmethod
    def textured_c2(rate, frames, attack, release, bow_level, bow_decay_seconds=None):
        rng = random.Random(123)
        samples = []
        for i in range(frames):
            t = i / rate
            envelope = min(1.0, t / attack, (frames - i) / (rate * release))
            phase = math.tau * 65.41 * t
            tone = sum(gain * math.sin(harmonic * phase)
                       for harmonic, gain in enumerate((0.7, 1.0, 0.42, 0.32), 1))
            bow_envelope = 1.0 if bow_decay_seconds is None else math.exp(-t / bow_decay_seconds)
            bow = bow_level * rng.uniform(-1.0, 1.0) * bow_envelope
            samples.append(round(4000 * envelope * (tone + bow)))
        return samples

    def isolate_mono(self, samples, rate, span=None):
        write_pcm(self.source, samples, rate, 1)
        flags = () if span is None else ("--disturbance-samples", f"{span[0]}:{span[1]}")
        result = self.isolate(*flags)
        self.assertEqual(result.returncode, 0, result.stderr)
        return [value * 32768 for value in read_float_pcm(self.output / "audio" / "cello.wav")]

    def test_stereo_cello_bundle_preserves_source_clock(self):
        rate, frames = 48000, 14603
        left = self.bowed_note(rate, frames, 65.41, 9000)
        right = self.bowed_note(rate, frames, 65.41, 6500)
        samples = [sample for pair in zip(left, right) for sample in pair]
        write_pcm(self.source, samples, rate, 2, mask=3)
        result = self.isolate()
        self.assertEqual(result.returncode, 0, result.stderr)
        checked = self.cli("validate-event-bundle", self.output)
        self.assertEqual(checked.returncode, 0, checked.stderr)
        manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
        provider, = manifest["providers"]
        self.assertEqual(provider["settings"]["instrument"], "cello")
        audio = {row["kind"]: row for row in manifest["audio"]}
        for row in audio.values():
            fmt = row["format"]
            self.assertEqual((fmt["sample_rate_hz"], fmt["channels"], fmt["channel_mask"],
                              fmt["frames"]), (rate, 2, 3, frames))
        target = audio["instrument-stem"]
        removed = audio["derived-audio"]
        self.assertEqual((target["name"], target["relative_path"]), ("cello", "audio/cello.wav"))
        self.assertEqual(removed["relative_path"], "audio/removed.wav")
        self.assertEqual(target["source_recording_id"], audio["source-recording"]["id"])
        events = (self.output / "events.jsonl").read_text(encoding="utf-8").splitlines()
        event, = map(json.loads, events)
        self.assertEqual((event["part"], event["start_sample"], event["end_sample"],
                          event["evidence_audio_id"]), ("cello", 0, frames, target["id"]))
        stem_audio = read_float_pcm(self.output / target["relative_path"])
        removed_audio = read_float_pcm(self.output / removed["relative_path"])
        self.assertEqual((len(stem_audio), len(removed_audio)), (len(samples), len(samples)))
        self.assertLess(max(abs(a + b - source / 32768)
                            for a, b, source in zip(stem_audio, removed_audio, samples)), 1e-7)

    def test_open_c_and_lower_tuning_survive_cleanup(self):
        for rate in (8000, 48000):
            start, end = rate // 4, rate * 3 // 4
            for hz in (65.41, 61.74):
                samples = self.bowed_note(rate, rate, hz, 10000)
                write_pcm(self.source, samples, rate, 1)
                for flags in ((), ("--disturbance-samples", f"{start}:{end}")):
                    with self.subTest(rate=rate, hz=hz, marked=bool(flags)):
                        self.output = self.root / f"c2-{rate}-{hz}-{bool(flags)}.hwa-events"
                        result = self.isolate(*flags)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        estimate = read_float_pcm(self.output / "audio" / "cello.wav")
                        signal = sum(value ** 2 for value in samples[start:end])
                        error = sum((estimate[i] * 32768 - samples[i]) ** 2
                                    for i in range(start, end))
                        self.assertLess(error, signal / 1000)

    def test_automatic_cleanup_preserves_a_short_open_c(self):
        rate, hz = 22050, 65.41
        start, frames = rate * 2 // 5, rate // 50
        samples = [0] * rate
        for i in range(frames):
            phase = math.tau * hz * i / rate
            envelope = math.sin(math.pi * i / frames) ** 2
            tone = (math.sin(phase) + 0.45 * math.sin(2 * phase)
                    + 0.2 * math.sin(3 * phase) + 0.1 * math.sin(4 * phase))
            samples[start + i] = round(6500 * envelope * tone)
        write_pcm(self.source, samples, rate, 1)
        result = self.isolate()
        self.assertEqual(result.returncode, 0, result.stderr)
        estimate = read_float_pcm(self.output / "audio" / "cello.wav")
        note = range(start, start + frames)
        signal = sum(samples[i] ** 2 for i in note)
        error = sum((estimate[i] * 32768 - samples[i]) ** 2 for i in note)
        self.assertLess(error, signal / 10000)

    def test_automatic_cleanup_preserves_a_short_textured_c2_between_rests(self):
        rate = 48000
        start = rate * 52 // 100
        note = self.textured_c2(rate, rate // 10, 0.012, 0.012, 1.0)
        estimate = self.isolate_mono([0] * start + note + [0] * rate, rate)
        signal = sum(value ** 2 for value in note)
        error = sum((value - estimate[i]) ** 2 for i, value in enumerate(note, start))
        self.assertLess(error, 0.03 * signal)

    def test_automatic_cleanup_preserves_a_short_c2_between_g2_notes(self):
        rate = 22050
        g2 = self.bowed_note(rate, rate * 430 // 1000, 98.0, 6500, edge_ms=17)
        c2 = self.bowed_note(rate, rate * 45 // 1000, 65.41, 6500, edge_ms=10)
        gap = [0] * (rate * 130 // 1000)
        padding = [0] * (rate * 110 // 1000)
        prefix = padding + g2 + gap
        estimate = self.isolate_mono(prefix + c2 + gap + g2 + padding, rate)
        signal = sum(value ** 2 for value in c2)
        error = sum((value - estimate[i]) ** 2 for i, value in enumerate(c2, len(prefix)))
        self.assertLess(error, 0.03 * signal)

    def test_automatic_cleanup_reduces_a_low_thump_in_a_rest(self):
        rate = 22050
        clean = (self.bowed_note(rate, rate // 2, 65.406, 6900, edge_ms=60)
                 + [0] * rate + self.bowed_note(rate, rate // 2, 98.0, 6900, edge_ms=60))
        start, length = round(0.95 * rate), rate // 10
        thump = [round(0.35 * 32768 * math.sin(math.pi * i / length) ** 2
                       * math.sin(math.tau * 32.0 * i / rate)) for i in range(length)]
        mixed = clean.copy()
        mixed[start:start + length] = thump
        estimate = self.isolate_mono(mixed, rate)
        self.assertLess(sum(value ** 2 for value in estimate[start:start + length]),
                        0.5 * sum(value ** 2 for value in thump))
        note_samples = (*range(rate // 2), *range(3 * rate // 2, 2 * rate))
        note_error = sum((estimate[i] - clean[i]) ** 2 for i in note_samples)
        self.assertLess(note_error, 0.02 * sum(clean[i] ** 2 for i in note_samples))

    def test_cleanup_improves_cello_with_recorded_disturbances(self):
        rate = 22050
        clean = (self.bowed_note(rate, rate * 5 // 4, 65.41, 7000)
                 + [0] * rate + self.bowed_note(rate, rate, 98.0, 6000))
        mixed = clean.copy()
        spans = []
        for name, offset, gain in (("cough", 9000, 2), ("step", 34500, 24)):
            noise = read_fixture(name)
            for i, sample in enumerate(noise, offset):
                mixed[i] += gain * sample
            spans.append((offset - 256, offset + len(noise) + 256))
        reference = self.root / "reference.wav"
        write_pcm(reference, clean, rate, 1)
        write_pcm(self.source, mixed, rate, 1)
        marked = [value for start, end in spans
                  for value in ("--disturbance-samples", f"{start}:{end}")]
        for mode, flags in (("automatic", ()), ("marked", marked)):
            with self.subTest(mode=mode):
                self.output = self.root / (mode + ".hwa-events")
                result = self.isolate(*flags)
                self.assertEqual(result.returncode, 0, result.stderr)
                stem = self.output / "audio" / "cello.wav"
                report = self.evaluate(reference, stem)
                self.assertGreater(report["si_sdr_improvement_db"], 0.5, report)
                estimate = read_float_pcm(stem)
                for start, end in spans:
                    frames = range(start + 256, end - 256)
                    mixture_error = sum((mixed[i] - clean[i]) ** 2 for i in frames)
                    stem_error = sum((estimate[i] * 32768 - clean[i]) ** 2 for i in frames)
                    self.assertLess(stem_error, mixture_error)

    def test_marked_cleanup_preserves_a_textured_c2_attack(self):
        rate = 22050
        note = self.textured_c2(rate, rate * 3 // 4, 0.014, 0.04, 2.0, bow_decay_seconds=0.085)
        clean = [0] * (rate // 2) + note + [0] * rate
        start = rate // 2 - 100
        cough = read_fixture("cough")
        mixed = clean.copy()
        for i, sample in enumerate(cough, start):
            mixed[i] += sample
        span = (start - 256, start + len(cough) + 256)
        estimate = self.isolate_mono(mixed, rate, span)
        self.assertLess(sum((estimate[i] - clean[i]) ** 2 for i in range(*span)),
                        1.02 * sum(sample ** 2 for sample in cough))

    def test_marked_cleanup_reduces_disturbances_over_a_low_double_stop(self):
        rate = 22050
        frames = rate * 2
        start = rate * 2 // 3
        clean = [a + b for a, b in zip(self.bowed_note(rate, frames, 65.406, 6900, edge_ms=60),
                                       self.bowed_note(rate, frames, 98.0, 5300, edge_ms=60))]
        length = rate * 24 // 100
        thump = [round(3000 * math.sin(math.pi * i / length) ** 2
                       * math.sin(math.tau * 31.0 * i / rate)) for i in range(length)]
        for kind, noise in (("cough", read_fixture("cough")), ("thump", thump)):
            with self.subTest(kind=kind):
                self.output = self.root / f"{kind}.hwa-events"
                mixed = clean.copy()
                for i, sample in enumerate(noise, start):
                    mixed[i] += sample
                span = (start - 441, start + len(noise) + 441)
                estimate = self.isolate_mono(mixed, rate, span)
                self.assertLess(sum((estimate[i] - clean[i]) ** 2 for i in range(*span)),
                                0.95 * sum(sample ** 2 for sample in noise))
                self.assertEqual(estimate[:span[0]], clean[:span[0]])
                self.assertEqual(estimate[span[1]:], clean[span[1]:])


if __name__ == "__main__":
    unittest.main(argv=[__file__, *REST])

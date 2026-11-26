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


def write_float_pcm(path, samples, rate, bits=32):
    sample_format = "f" if bits == 32 else "d"
    width = bits // 8
    audio = struct.pack(f"<{len(samples)}{sample_format}", *samples)
    fmt = struct.pack("<HHIIHHH", 3, 1, rate, rate * width, width, bits, 0)
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
    def textured_bowed_note(rate, frames, hz, attack, release, bow_level, bow_decay_seconds=None):
        rng = random.Random(123)
        samples = []
        for i in range(frames):
            t = i / rate
            envelope = min(1.0, t / attack, (frames - i) / (rate * release))
            phase = math.tau * hz * t
            tone = sum(gain * math.sin(harmonic * phase)
                       for harmonic, gain in enumerate((0.7, 1.0, 0.42, 0.32), 1))
            bow_envelope = 1.0 if bow_decay_seconds is None else math.exp(-t / bow_decay_seconds)
            bow = bow_level * rng.uniform(-1.0, 1.0) * bow_envelope
            samples.append(round(4000 * envelope * (tone + bow)))
        return samples

    @staticmethod
    def slurred_phrase(rate, note_frames, pitches, levels):
        phrase_frames = len(pitches) * note_frames
        rng = random.Random(86)
        phase = previous_noise = 0.0
        phrase = []
        for i in range(phrase_frames):
            note, offset = divmod(i, note_frames)
            pitch = pitches[note]
            if note > 0:
                transition = min(1.0, offset / (0.007 * rate))
                pitch = (1.0 - transition) * pitches[note - 1] + transition * pitch
            phase += math.tau * pitch / rate
            tone = sum(level * math.sin(j * phase) for j, level in enumerate(levels, 1)
                       if j * pitch < rate / 2)
            noise = rng.uniform(-1.0, 1.0)
            breath = (noise - 0.55 * previous_noise) * (0.08 + 0.14 * math.exp(-i / (0.014 * rate)))
            previous_noise = noise
            envelope = min(1.0, i / (0.008 * rate), (phrase_frames - i) / (0.013 * rate))
            phrase.append(round(2200 * envelope * (tone + breath)))
        return phrase

    @staticmethod
    def note_with_fundamental_swell(rate, pitch, swell_end,
                                   levels=(0.04, 0, 0.94, 0, 0.014, 0, 0.01), peak=0.29):
        step = math.tau * pitch / rate
        phase = 0.0
        rng = random.Random(88)
        samples = []
        for i in range(rate):
            time = i / rate
            envelope = min(1.0, time / 0.01, (1.0 - time) / 0.02)
            rise = min(1.0, max(0.0, (time - 0.40) / 0.01))
            fall = min(1.0, max(0.0, (swell_end - time) / 0.01))
            phase += step
            fundamental = levels[0] + (peak - levels[0]) * rise * fall
            tone = fundamental * math.sin(phase)
            for j, level in enumerate(levels[1:], 2):
                tone += level * math.sin(j * phase)
            samples.append(round(4800 * envelope * (tone + rng.uniform(-0.024, 0.024))))
        return samples

    @staticmethod
    def slow_breathy_note(rate, pitch, levels, pitch_dip):
        rng = random.Random(101)
        phase = previous_noise = 0.0
        note = []
        for i in range(round(0.46 * rate)):
            time = i / rate
            envelope = min(1.0, time / 0.024, (0.46 - time) / 0.026)
            pitch_gain = min(1.0, max(0.0, (time - 0.10) / 0.050),
                             max(0.0, (0.385 - time) / 0.065))
            breath_gain = 0.35 * (1.0 - pitch_gain) + 0.035 * pitch_gain
            phase += math.tau * pitch * 2 ** (-pitch_dip * (1.0 - pitch_gain)) / rate
            tone = sum(level * math.sin(j * phase) for j, level in enumerate(levels, 1))
            noise = rng.uniform(-1.0, 1.0)
            air = (noise - 0.65 * previous_noise) * breath_gain
            previous_noise = noise
            note.append(round(250 * envelope * (pitch_gain * tone + air)))
        return note

    @staticmethod
    def saxophone_note(rate, frames, pitch, amplitude, seed, levels, vibrato_cents):
        rng = random.Random(seed)
        peak_pitch = pitch * 2 ** (abs(vibrato_cents) / 1200)
        partials = [(j, level) for j, level in enumerate(levels, 1) if j * peak_pitch < rate / 2]
        phase = previous_noise = 0.0
        samples = []
        for i in range(frames):
            time = i / rate
            cents = vibrato_cents * math.sin(math.tau * 5.2 * time)
            phase += math.tau * pitch * 2 ** (cents / 1200) / rate
            envelope = min(1.0, time / 0.008, (frames - i) / (rate * 0.017))
            tone = sum(level * math.sin(j * phase) for j, level in partials)
            noise = rng.uniform(-1.0, 1.0)
            air = (noise - 0.55 * previous_noise) * (0.048 + 0.36 * math.exp(-time / 0.013))
            previous_noise = noise
            samples.append(round(amplitude * envelope * (tone + air)))
        return samples

    def isolate_mono(self, samples, rate, span=None):
        write_pcm(self.source, samples, rate, 1)
        flags = () if span is None else ("--disturbance-samples", f"{span[0]}:{span[1]}")
        result = self.isolate(*flags)
        self.assertEqual(result.returncode, 0, result.stderr)
        stem = self.output / "audio" / f"{self.instrument}.wav"
        return [value * 32768 for value in read_float_pcm(stem)]

    def assert_recorded_stereo_cleanup(self, held, quiet):
        rate, frames = 22050, 71663
        onset, footfall = rate // 4, round(2.40 * rate)
        left, right = [0] * frames, [0] * frames
        left[onset:onset + len(held)] = held
        right[footfall:footfall + len(quiet)] = quiet
        mixture = left.copy()
        spans = []
        for name, start, gain in (("cough", round(0.92 * rate), 1), ("step", footfall, 24)):
            noise = read_fixture(name)
            for i, sample in enumerate(noise, start):
                mixture[i] += gain * sample
            spans.append((name, start, start + len(noise)))
        source = [value for pair in zip(mixture, right) for value in pair]
        reference = self.root / "reference.wav"
        write_pcm(reference, [value for pair in zip(left, right) for value in pair], rate, 2, mask=3)
        write_pcm(self.source, source, rate, 2, mask=3)
        bounds = [[start - 441, end + 441] for _, start, end in spans]
        flags = tuple(arg for start, end in bounds for arg in
                      ("--disturbance-samples", f"{start}:{end}"))

        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"{self.instrument}-stereo-{marked}.hwa-events"
                result = self.isolate(*(flags if marked else ()))
                self.assertEqual(result.returncode, 0, result.stderr)
                valid = self.cli("validate-event-bundle", self.output)
                self.assertEqual(valid.returncode, 0, valid.stderr)
                manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
                provider, = manifest["providers"]
                self.assertEqual((provider["name"], provider["version"]),
                                 (f"hwa.{self.instrument}-isolation", f"{self.instrument}-isolation-2"))
                self.assertEqual(provider["settings"], {"instrument": self.instrument,
                                 "disturbance_samples": bounds if marked else []})
                audio = {row["kind"]: row for row in manifest["audio"]}
                for row in audio.values():
                    fmt = row["format"]
                    self.assertEqual((fmt["sample_rate_hz"], fmt["channels"], fmt["channel_mask"],
                                      fmt["frames"]), (rate, 2, 3, frames))
                artifact = audio["instrument-stem"]
                self.assertEqual((artifact["name"], artifact["relative_path"],
                                  artifact["source_recording_id"]),
                                 (self.instrument, f"audio/{self.instrument}.wav",
                                  audio["source-recording"]["id"]))
                events = (self.output / "events.jsonl").read_text(encoding="utf-8").splitlines()
                event, = map(json.loads, events)
                self.assertEqual((event["part"], event["start_sample"], event["end_sample"],
                                  event["evidence_audio_id"]), (self.instrument, 0, frames, artifact["id"]))
                stem_path = self.output / artifact["relative_path"]
                self.assertGreater(self.evaluate(reference, stem_path)["si_sdr_improvement_db"], 0.7)
                stem = read_float_pcm(stem_path)
                removed = read_float_pcm(self.output / audio["derived-audio"]["relative_path"])
                self.assertEqual((len(stem), len(removed)), (len(source), len(source)))
                self.assertLess(max(abs(a + b - original / 32768)
                                    for a, b, original in zip(stem, removed, source)), 1e-7)
                for name, start, end in spans:
                    original_error = sum((mixture[i] - left[i]) ** 2 for i in range(start, end))
                    stem_error = sum((stem[2 * i] * 32768 - left[i]) ** 2 for i in range(start, end))
                    self.assertLess(stem_error, 0.85 * original_error, name)
                quiet_error = sum((sample * 32768 - clean) ** 2
                                  for sample, clean in zip(stem[1::2], right))
                self.assertLess(quiet_error, 0.0025 * sum(sample ** 2 for sample in quiet))
                if marked:
                    self.assertFalse(any(removed[:2 * bounds[0][0]]))
                    self.assertFalse(any(removed[2 * bounds[0][1]:2 * bounds[1][0]]))
                    self.assertFalse(any(removed[2 * bounds[1][1]:]))


class ViolinIsolation(InstrumentIsolationTest):
    def test_versions_the_float64_residual_policy(self):
        write_float_pcm(self.source, [0.1, 0.0, 0.0], 8000, bits=64)
        result = self.isolate("--disturbance-samples", "1:2")
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["providers"][0]["version"], "violin-isolation-4")
        removed = read_float_pcm(self.output / "audio" / "removed.wav")
        self.assertEqual((removed[0], removed[2]), (0.0, 0.0))

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
        unsupported = self.isolate(instrument="not-an-instrument")
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
        note = self.textured_bowed_note(rate, rate // 10, 65.41, 0.012, 0.012, 1.0)
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
        note = self.textured_bowed_note(rate, rate * 3 // 4, 65.41, 0.014, 0.04, 2.0,
                                        bow_decay_seconds=0.085)
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


class DoubleBassIsolation(InstrumentIsolationTest):
    instrument = "double-bass"

    def test_stereo_bundle_preserves_clock_and_audio_sum(self):
        rate, frames, pitch = 22050, 44099, 41.20
        left = self.bowed_note(rate, frames, pitch, 7500)
        right = self.bowed_note(rate, frames, pitch, -4800)
        cough = read_fixture("cough")
        onset = rate * 2 // 3
        for frame, sample in enumerate(cough, onset):
            left[frame] += sample
        span = (onset - 441, onset + len(cough) + 441)
        samples = [value for pair in zip(left, right) for value in pair]
        write_pcm(self.source, samples, rate, 2, mask=3)

        result = self.isolate("--disturbance-samples", f"{span[0]}:{span[1]}")
        self.assertEqual(result.returncode, 0, result.stderr)
        checked = self.cli("validate-event-bundle", self.output)
        self.assertEqual(checked.returncode, 0, checked.stderr)
        manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
        provider, = manifest["providers"]
        self.assertEqual((provider["name"], provider["version"]),
                         ("hwa.double-bass-isolation", "double-bass-isolation-2"))
        self.assertEqual(provider["settings"],
                         {"instrument": "double-bass", "disturbance_samples": [list(span)]})
        audio = {row["kind"]: row for row in manifest["audio"]}
        for row in audio.values():
            fmt = row["format"]
            self.assertEqual((fmt["sample_rate_hz"], fmt["channels"], fmt["frames"],
                              fmt["channel_mask"]), (rate, 2, frames, 3))
        target = audio["instrument-stem"]
        self.assertEqual((target["name"], target["relative_path"]),
                         ("double-bass", "audio/double-bass.wav"))
        self.assertEqual(target["source_recording_id"], audio["source-recording"]["id"])
        events = (self.output / "events.jsonl").read_text(encoding="utf-8").splitlines()
        event, = map(json.loads, events)
        self.assertEqual((event["kind"], event["part"], event["start_sample"], event["end_sample"],
                          event["evidence_audio_id"]),
                         ("instrument-region", "double-bass", 0, frames, target["id"]))

        stem = read_float_pcm(self.output / target["relative_path"])
        removed = read_float_pcm(self.output / "audio" / "removed.wav")
        self.assertEqual((len(stem), len(removed)), (len(samples), len(samples)))
        self.assertTrue(any(removed))
        self.assertLess(max(abs(a + b - sample / 32768)
                            for a, b, sample in zip(stem, removed, samples)), 1e-7)
        self.assertFalse(any(removed[:2 * span[0]]))
        self.assertFalse(any(removed[2 * span[1]:]))

    def test_preserves_open_e_and_lower_notes_in_both_modes(self):
        for rate in (8000, 48000):
            for pitch in (41.20, 32.70, 30.87):
                samples = self.bowed_note(rate, rate, pitch, 10000, edge_ms=60)
                for span in (None, (rate // 4, 3 * rate // 4)):
                    with self.subTest(rate=rate, pitch=pitch, marked=span is not None):
                        self.output = self.root / f"low-{rate}-{pitch}-{span is not None}.hwa-events"
                        stem = self.isolate_mono(samples, rate, span)
                        power = sum(value ** 2 for value in samples)
                        error = sum((est - value) ** 2 for est, value in zip(stem, samples))
                        self.assertLess(error, 0.001 * power)

    def test_preserves_short_bowed_and_plucked_notes_between_long_notes(self):
        rate = 48000
        neighbor = self.bowed_note(rate, rate * 430 // 1000, 55.0, 6500, edge_ms=17)
        gap = [0] * (rate * 130 // 1000)
        padding = [0] * (rate * 110 // 1000)
        prefix = padding + neighbor + gap
        suffix = gap + neighbor + padding
        bowed = self.textured_bowed_note(rate, rate // 10, 30.87, 0.012, 0.012, 1.0)
        plucked = []
        frames = rate * 3 // 20
        for frame in range(frames):
            t = frame / rate
            phase = math.tau * 41.20 * t
            envelope = min(1.0, t / 0.002, (frames - frame) / (rate * 0.015))
            tone = sum(gain * math.exp(-harmonic * t / 0.09) * math.sin(harmonic * phase)
                       for harmonic, gain in enumerate((1.0, 0.6, 0.35, 0.2, 0.12, 0.07), 1))
            plucked.append(round(7500 * envelope * tone))
        for technique, note in (("bowed B0", bowed), ("plucked E1", plucked)):
            start = len(prefix)
            margin = rate // 100
            for span in (None, (start - margin, start + len(note) + margin)):
                with self.subTest(technique=technique, marked=span is not None):
                    self.output = self.root / f"{technique}-{span is not None}.hwa-events"
                    stem = self.isolate_mono(prefix + note + suffix, rate, span)
                    power = sum(value ** 2 for value in note)
                    error = sum((value - stem[i]) ** 2 for i, value in enumerate(note, start))
                    self.assertLess(error, 0.03 * power)

    def test_preserves_a_detached_low_pluck(self):
        rate = 44100
        frames = rate * 40 // 1000
        rng = random.Random(2)
        partials = [(gain, rng.uniform(0.0, math.tau))
                    for gain in (1.0, 0.7, 0.6, 0.35, 0.31, 0.2)]
        note = []
        for frame in range(frames):
            time = frame / rate
            envelope = min(1.0, time / 0.008, (frames - frame) / (rate * 0.008))
            tone = sum(gain * math.sin(math.tau * 30.87 * harmonic * time + phase)
                       * math.exp(-harmonic * time / 0.12)
                       for harmonic, (gain, phase) in enumerate(partials, 1))
            note.append(round(4500 * envelope * tone))
        source = [0] * rate + note + [0] * rate
        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"detached-b0-{marked}.hwa-events"
                span = (rate - 256, rate + frames + 256) if marked else None
                stem = self.isolate_mono(source, rate, span)
                error = sum((sample - stem[i]) ** 2 for i, sample in enumerate(note, rate))
                self.assertLess(error, 0.03 * sum(sample ** 2 for sample in note))

    def test_automatic_cleanup_preserves_a_gapless_e1_between_a1_notes(self):
        rate = 22050

        def note(pitch, duration):
            frames = round(rate * duration)
            partials = (1.0, 0.65, 0.33, 0.12, 0.05)
            result = []
            for frame in range(frames):
                envelope = min(1.0, frame / (0.006 * rate), (frames - frame) / (0.006 * rate))
                phase = math.tau * pitch * frame / rate
                tone = sum(gain * math.sin(harmonic * phase)
                           for harmonic, gain in enumerate(partials, 1))
                result.append(round(6000 * envelope * tone))
            return result

        neighbor = note(55.0, 0.4)
        e1 = note(41.2, 0.075)
        prefix = [0] * rate + neighbor
        stem = self.isolate_mono(prefix + e1 + neighbor + [0] * rate, rate)
        error = sum((sample - stem[i]) ** 2 for i, sample in enumerate(e1, len(prefix)))
        self.assertLess(error, 0.01 * sum(sample ** 2 for sample in e1))

    def test_preserves_a_pitch_glide_under_a_recorded_cough(self):
        rate = 22050
        duration = 0.25
        start = rate // 2
        frames = rate // 4
        partials = (1.0, 0.65, 0.37, 0.57, 0.22, 0.31, 0.28, 0.22, 0.2,
                    0.28, 0.22, 0.14, 0.12, 0.09, 0.13, 0.08, 0.04, 0.03, 0.02)
        note = []
        for frame in range(frames):
            time = frame / rate
            cycles = 293.66 * time + 0.5 * 96.91 * time ** 2 / duration
            envelope = min(1.0, time / 0.005, (frames - frame) / (rate * 0.025))
            tone = sum(gain * math.sin(math.tau * harmonic * cycles + harmonic % 5)
                       for harmonic, gain in enumerate(partials, 1))
            note.append(round(4100 * envelope * tone))
        clean = [0] * start + note + [0] * rate
        mix = clean.copy()
        cough = read_fixture("cough")
        first = start - rate // 18
        end = first + len(cough)
        gain = math.sqrt(sum(value ** 2 for value in note) / sum(value ** 2 for value in cough)
                         * 10 ** (-18 / 10))
        for i, sample in enumerate(cough, first):
            mix[i] += round(gain * sample)
        input_error = sum((mix[i] - clean[i]) ** 2 for i in range(first, end))
        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"glide-{marked}.hwa-events"
                span = (first, end) if marked else None
                stem = self.isolate_mono(mix, rate, span)
                error = sum((stem[i] - clean[i]) ** 2 for i in range(first, end))
                self.assertLess(error, 1.1 * input_error)

    def test_nearby_marked_spans_use_their_own_background(self):
        rate = 48000
        early = (rate * 3 // 2, rate * 155 // 100)
        late = (rate * 160 // 100, rate * 177 // 100)

        def thump(time, start, end, amplitude):
            if not start <= time < end:
                return 0
            envelope = math.sin(math.pi * (time - start) / (end - start)) ** 2
            return round(amplitude * envelope * math.sin(math.tau * 19 * time))

        clean = []
        source = []
        for i in range(3 * rate):
            time = i / rate
            tone = round(4000 * sum(gain * math.sin(math.tau * 41.203 * harmonic * time)
                                    for harmonic, gain in enumerate((1.0, 0.7, 0.6), 1)))
            clean.append(tone)
            source.append(tone + thump(time, 1.16, 1.29, 13000)
                          + thump(time, late[0] / rate, late[1] / rate, 3400))
        for i in range(*early):
            envelope = math.sin(math.pi * (i - early[0]) / (early[1] - early[0])) ** 2
            source[i] += round(3500 * envelope * math.sin(math.tau * 780 * i / rate))
        write_pcm(self.source, source, rate, 1)
        errors = []
        for spans in ((late,), (early, late)):
            self.output = self.root / f"noise-floors-{len(spans)}.hwa-events"
            flags = [arg for start, end in spans
                     for arg in ("--disturbance-samples", f"{start}:{end}")]
            result = self.isolate(*flags)
            self.assertEqual(result.returncode, 0, result.stderr)
            stem = [value * 32768 for value in
                    read_float_pcm(self.output / "audio" / "double-bass.wav")]
            errors.append(sum((stem[i] - clean[i]) ** 2 for i in range(*late)))
            self.assertEqual(stem[early[1]:late[0]], source[early[1]:late[0]])
        unfiltered_error = sum((source[i] - clean[i]) ** 2 for i in range(*late))
        self.assertLess(errors[0], 0.3 * unfiltered_error)
        self.assertLess(errors[1], 1.1 * errors[0])

    def test_cleanup_improves_bass_with_recorded_cough_and_step(self):
        rate = 22050
        clean = (self.bowed_note(rate, rate * 5 // 4, 41.20, 7000)
                 + [0] * rate + self.bowed_note(rate, rate, 55.0, 6000))
        mixed = clean.copy()
        spans = []
        for kind, onset, gain in (("cough", 9000, 2), ("step", 34500, 24)):
            noise = read_fixture(kind)
            for i, sample in enumerate(noise, onset):
                mixed[i] += gain * sample
            spans.append((kind, onset, onset + len(noise)))
        reference = self.root / "reference.wav"
        write_pcm(reference, clean, rate, 1)
        write_pcm(self.source, mixed, rate, 1)
        marked = tuple(value for _, start, end in spans
                       for value in ("--disturbance-samples", f"{start - 256}:{end + 256}"))
        for mode, flags in (("automatic", ()), ("marked", marked)):
            with self.subTest(mode=mode):
                self.output = self.root / f"{mode}.hwa-events"
                result = self.isolate(*flags)
                self.assertEqual(result.returncode, 0, result.stderr)
                stem = self.output / "audio" / "double-bass.wav"
                report = self.evaluate(reference, stem)
                self.assertGreater(report["si_sdr_improvement_db"], 0.25, report)
                estimate = read_float_pcm(stem)
                for kind, start, end in spans:
                    mixture_error = sum((mixed[i] - clean[i]) ** 2 for i in range(start, end))
                    stem_error = sum((estimate[i] * 32768 - clean[i]) ** 2
                                     for i in range(start, end))
                    self.assertLess(stem_error, 0.99 * mixture_error, kind)


class PianoIsolation(InstrumentIsolationTest):
    instrument = "piano"

    @staticmethod
    def strike(rate, frames, pitch, amplitude, decay, seed):
        rng = random.Random(seed)
        partials = []
        weights = (1.0, 0.75, 0.44, 0.32, 0.19, 0.13, 0.09)
        for harmonic, weight in enumerate(weights, 1):
            frequency = pitch * harmonic * math.sqrt(1.0 + 0.0004 * (harmonic ** 2 - 1))
            if frequency >= rate / 2:
                break
            partials.append((math.tau * frequency, weight, rng.uniform(-math.pi, math.pi)))
        samples = []
        for frame in range(frames):
            t = frame / rate
            envelope = min(1.0, t / 0.002, (frames - frame) / (rate * 0.012))
            tone = sum(weight * math.sin(angular * t + phase)
                       * math.exp(-t * (1.0 + 0.18 * index) / decay)
                       for index, (angular, weight, phase) in enumerate(partials))
            hammer = 0.28 * rng.uniform(-1.0, 1.0) * math.exp(-t / 0.006)
            samples.append(round(amplitude * envelope * (tone + hammer)))
        return samples

    def phrase(self, rate, duration, notes):
        samples = [0] * round(rate * duration)
        for seed, (onset, length, pitch, amplitude, decay) in enumerate(notes, 1):
            start = round(onset * rate)
            note = self.strike(rate, round(length * rate), pitch, amplitude, decay, seed)
            for i, value in enumerate(note, start):
                samples[i] += value
        return samples

    def test_stereo_marked_bundle_keeps_the_clock_and_adds_back_to_source(self):
        rate, frames = 22050, 46237
        left = self.strike(rate, frames, 130.81, 5500, 1.8, 1)
        right = self.strike(rate, frames, 196.0, 4500, 1.8, 2)
        cough = read_fixture("cough")
        onset = rate * 2 // 3
        for i, sample in enumerate(cough, onset):
            left[i] += sample
        span = (onset - 441, onset + len(cough) + 441)
        source = [value for pair in zip(left, right) for value in pair]
        write_pcm(self.source, source, rate, 2, mask=3)

        result = self.isolate("--disturbance-samples", f"{span[0]}:{span[1]}")
        self.assertEqual(result.returncode, 0, result.stderr)
        validated = self.cli("validate-event-bundle", self.output)
        self.assertEqual(validated.returncode, 0, validated.stderr)
        manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
        provider, = manifest["providers"]
        self.assertEqual((provider["name"], provider["version"]),
                         ("hwa.piano-isolation", "piano-isolation-2"))
        self.assertEqual(provider["settings"],
                         {"instrument": "piano", "disturbance_samples": [list(span)]})
        audio = {row["kind"]: row for row in manifest["audio"]}
        for row in audio.values():
            fmt = row["format"]
            self.assertEqual((fmt["sample_rate_hz"], fmt["channels"], fmt["channel_mask"],
                              fmt["frames"]), (rate, 2, 3, frames))
        piano = audio["instrument-stem"]
        self.assertEqual((piano["name"], piano["relative_path"]), ("piano", "audio/piano.wav"))
        self.assertEqual(piano["source_recording_id"], audio["source-recording"]["id"])
        events = (self.output / "events.jsonl").read_text(encoding="utf-8").splitlines()
        event, = map(json.loads, events)
        self.assertEqual((event["kind"], event["part"], event["start_sample"], event["end_sample"],
                          event["evidence_audio_id"]),
                         ("instrument-region", "piano", 0, frames, piano["id"]))

        stem = read_float_pcm(self.output / piano["relative_path"])
        removed = read_float_pcm(self.output / audio["derived-audio"]["relative_path"])
        self.assertEqual((len(stem), len(removed)), (len(source), len(source)))
        self.assertLess(max(abs(keep + drop - sample / 32768)
                            for keep, drop, sample in zip(stem, removed, source)), 1e-7)
        self.assertTrue(any(removed[2 * span[0]:2 * span[1]]))
        self.assertFalse(any(removed[:2 * span[0]]))
        self.assertFalse(any(removed[2 * span[1]:]))

    def test_removes_a_step_without_cutting_a_note_in_the_other_channel(self):
        rate = 22050
        onset = int(0.65 * rate)
        step = [4 * sample for sample in read_fixture("step")]
        note = self.strike(rate, int(0.12 * rate), 523.25, 500, 0.1, 123)
        left = [0] * (2 * rate)
        right = left.copy()
        left[onset:onset + len(step)] = step
        right[onset:onset + len(note)] = note
        write_pcm(self.source, [value for pair in zip(left, right) for value in pair], rate, 2)

        mark = f"{onset - 2048}:{onset + len(step) + 2048}"
        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"stereo-step-{marked}.hwa-events"
                flags = ("--disturbance-samples", mark) if marked else ()
                result = self.isolate(*flags)
                self.assertEqual(result.returncode, 0, result.stderr)
                stem = read_float_pcm(self.output / "audio" / "piano.wav")
                left_energy = sum((sample * 32768) ** 2 for sample in stem[::2])
                right_error = sum((sample * 32768 - clean) ** 2
                                  for sample, clean in zip(stem[1::2], right))
                self.assertLess(right_error, 0.01 * sum(sample ** 2 for sample in note))
                self.assertLess(left_energy, 0.8 * sum(sample ** 2 for sample in step))

    def test_preserves_a0_at_low_sample_rate(self):
        rate = 8000
        start = rate // 2
        for frames in (rate * 60 // 1000, rate // 2):
            note = self.strike(rate, frames, 27.5, 8000, 0.55, 1)
            samples = [0] * start + note + [0] * rate
            for marked in (False, True):
                with self.subTest(frames=frames, marked=marked):
                    span = (start - 160, start + frames + 160) if marked else None
                    self.output = self.root / f"a0-{frames}-{marked}.hwa-events"
                    stem = self.isolate_mono(samples, rate, span)
                    signal = sum(value ** 2 for value in note)
                    error = sum((value - stem[i]) ** 2 for i, value in enumerate(note, start))
                    self.assertLess(error, 0.01 * signal)

    def test_preserves_high_key_chords_reattacks_and_pedal_tail(self):
        rate = 22050
        samples = self.phrase(rate, 2.3, (
            (0.30, 0.06, 4186.01, 6500, 0.055),
            (0.60, 1.25, 55.00, 3800, 0.32),
            (0.60, 1.25, 130.81, 2800, 0.27),
            (0.60, 1.25, 164.81, 2400, 0.28),
            (0.60, 1.25, 196.00, 2100, 0.22),
            (0.72, 1.13, 196.00, 3500, 0.23),
            (0.79, 0.10, 1046.50, 3900, 0.065),
        ))
        regions = ((0.30, 0.36), (0.60, 0.72), (0.72, 0.89), (1.35, 1.85))
        for marked in (False, True):
            with self.subTest(marked=marked):
                span = (round(0.27 * rate), round(1.88 * rate)) if marked else None
                self.output = self.root / f"chord-{marked}.hwa-events"
                stem = self.isolate_mono(samples, rate, span)
                for start, end in regions:
                    indices = range(round(start * rate), round(end * rate))
                    signal = sum(samples[i] ** 2 for i in indices)
                    error = sum((samples[i] - stem[i]) ** 2 for i in indices)
                    self.assertLess(error, 0.015 * signal, (start, end))

    def test_preserves_a_detached_cluster_of_low_notes(self):
        rate = 22050
        notes = tuple((0.5, 0.16, pitch, 4200, 0.29)
                      for pitch in (27.5, 30.87, 36.71, 41.2))
        samples = self.phrase(rate, 1.8, notes)
        signal = sum(sample ** 2 for sample in samples)
        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"low-cluster-{marked}.hwa-events"
                span = (round(0.48 * rate), round(0.68 * rate)) if marked else None
                stem = self.isolate_mono(samples, rate, span)
                error = sum((sample - value) ** 2 for sample, value in zip(samples, stem))
                self.assertLess(error, 0.01 * signal)

    def test_preserves_short_detached_chromatic_chords(self):
        rate = 22050
        for pitches in ((50, 51), (48, 49, 50, 51)):
            notes = tuple((0.5, 0.16, 440 * 2 ** ((midi - 69) / 12), 3000, 0.5)
                          for midi in pitches)
            clean = self.phrase(rate, 1.8, notes)
            energy = sum(sample ** 2 for sample in clean)
            for marked in (False, True):
                with self.subTest(pitches=pitches, marked=marked):
                    self.output = self.root / f"short-chord-{len(pitches)}-{marked}.hwa-events"
                    span = (round(0.48 * rate), round(0.68 * rate)) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    error = sum((sample - value) ** 2 for sample, value in zip(clean, stem))
                    self.assertLess(error, 0.01 * energy)

    def test_preserves_a_treble_attack_over_a_held_chord(self):
        rate = 22050
        chord = ((130.81, 5600), (196, 4000), (261.63, 3300), (329.63, 2650))
        clean = self.phrase(rate, 1.85, [(0.15, 1.55, pitch, amp, 1.7)
                                        for pitch, amp in chord])
        onset = round(0.75 * rate)
        amplitude = 1700
        note = self.strike(rate, round(0.25 * rate), 783.99, amplitude, 0.12, 234)
        rng = random.Random(2344)
        for i in range(round(0.03 * rate)):
            t = i / rate
            hammer = 1.5 * amplitude * math.exp(-t / 0.009) * min(1.0, t / 0.002)
            note[i] += round(hammer * rng.uniform(-1.0, 1.0))
        for i, sample in enumerate(note, onset):
            clean[i] += sample

        # First differences emphasize the high frequencies in the hammer attack.
        attack = range(1, round(0.028 * rate))
        energy = sum((note[i] - note[i - 1]) ** 2 for i in attack)
        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"treble-attack-{marked}.hwa-events"
                span = (round(0.71 * rate), round(0.86 * rate)) if marked else None
                stem = self.isolate_mono(clean, rate, span)
                error = sum(((stem[onset + i] - stem[onset + i - 1])
                             - (clean[onset + i] - clean[onset + i - 1])) ** 2
                            for i in attack)
                self.assertLess(error, 0.01 * energy)

    def test_reduces_a_marked_cough_near_the_end_of_a_recording(self):
        rate = 22050
        chord = ((130.81, 4800), (164.81, 3400), (196, 2800), (261.63, 2400))
        clean = self.phrase(rate, 2.8, [(0.25, 2.4, pitch, amp, 1.85) for pitch, amp in chord])
        onset = round(1.22 * rate)
        cough = read_fixture("cough")
        start, end = onset - 441, onset + len(cough) + 441
        mixed = clean.copy()
        for i, sample in enumerate(cough, onset):
            mixed[i] += sample

        noise_energy = sum(sample ** 2 for sample in cough)
        for trailing_ms in (0, 50):
            with self.subTest(trailing_ms=trailing_ms):
                self.output = self.root / f"cough-near-end-{trailing_ms}.hwa-events"
                mix = mixed[:end + round(rate * trailing_ms / 1000)]
                stem = self.isolate_mono(mix, rate, (start, end))
                error = sum((stem[i] - clean[i]) ** 2 for i in range(start, end))
                self.assertLess(error, 0.8 * noise_energy)
                self.assertEqual(stem[:start], mix[:start])
                self.assertEqual(stem[end:], mix[end:])

    def test_reduces_recorded_cough_over_chord_and_step_in_rest(self):
        rate = 22050
        clean = self.phrase(rate, 4.3, (
            (0.25, 2.40, 130.81, 4800, 1.85),
            (0.25, 2.40, 164.81, 3400, 1.70),
            (0.25, 2.40, 196.00, 2800, 1.55),
            (0.25, 2.40, 261.63, 2400, 1.40),
        ))
        mix = clean.copy()
        spans = []
        for kind, onset, gain in (("cough", 1.22, 1), ("step", 3.10, 24)):
            start = round(onset * rate)
            noise = read_fixture(kind)
            for i, value in enumerate(noise, start):
                mix[i] += gain * value
            spans.append((kind, start, start + len(noise)))
        reference = self.root / "piano-reference.wav"
        write_pcm(reference, clean, rate, 1)
        write_pcm(self.source, mix, rate, 1)
        marks = tuple(arg for _, start, end in spans
                      for arg in ("--disturbance-samples", f"{start - 441}:{end + 441}"))
        for mode, flags in (("automatic", ()), ("marked", marks)):
            with self.subTest(mode=mode):
                self.output = self.root / f"piano-{mode}.hwa-events"
                result = self.isolate(*flags)
                self.assertEqual(result.returncode, 0, result.stderr)
                stem_path = self.output / "audio" / "piano.wav"
                report = self.evaluate(reference, stem_path)
                self.assertGreater(report["si_sdr_improvement_db"], 1.0, report)
                stem = read_float_pcm(stem_path)
                for kind, start, end in spans:
                    frames = range(start, end)
                    mix_error = sum((mix[i] - clean[i]) ** 2 for i in frames)
                    stem_error = sum((stem[i] * 32768 - clean[i]) ** 2 for i in frames)
                    self.assertLess(stem_error, 0.8 * mix_error, kind)


class FluteIsolation(InstrumentIsolationTest):
    instrument = "flute"

    @staticmethod
    def note(rate, frames, pitch, amplitude, seed, vibrato_cents=0, breath=0.04):
        rng = random.Random(seed)
        harmonics = [(harmonic, gain)
                     for harmonic, gain in enumerate((1.0, 0.37, 0.16, 0.08, 0.035), 1)
                     if harmonic * pitch < rate / 2]
        phase = 0.0
        previous_noise = 0.0
        samples = []
        for frame in range(frames):
            t = frame / rate
            vibrato = math.sin(math.tau * 5.3 * t)
            phase += math.tau * pitch * 2 ** (vibrato * vibrato_cents / 1200) / rate
            envelope = min(1.0, t / 0.004, (frames - frame) / (rate * 0.012))
            tone = sum(gain * math.sin(harmonic * phase) for harmonic, gain in harmonics)
            noise = rng.uniform(-1.0, 1.0)
            air = (noise - 0.55 * previous_noise) * (breath + 0.24 * math.exp(-t / 0.014))
            previous_noise = noise
            samples.append(round(amplitude * envelope * (tone + air)))
        return samples

    def test_stereo_marked_bundle_preserves_the_source_clock(self):
        rate, frames = 22050, 46237
        left = [0] * frames
        right = left.copy()
        onset = round(0.22 * rate)
        note = self.note(rate, round(0.55 * rate), 246.94, 4300, 1, vibrato_cents=15)
        left[onset:onset + len(note)] = note
        right[onset:onset + len(note)] = [round(-0.7 * sample) for sample in note]
        step = read_fixture("step")
        footfall = round(1.2 * rate)
        for i, sample in enumerate(step, footfall):
            left[i] += 24 * sample
            right[i] += 18 * sample
        span = footfall - 441, footfall + len(step) + 441
        source = [value for pair in zip(left, right) for value in pair]
        write_pcm(self.source, source, rate, 2, mask=3)

        result = self.isolate("--disturbance-samples", f"{span[0]}:{span[1]}")
        self.assertEqual(result.returncode, 0, result.stderr)
        validated = self.cli("validate-event-bundle", self.output)
        self.assertEqual(validated.returncode, 0, validated.stderr)
        manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
        provider, = manifest["providers"]
        self.assertEqual((provider["name"], provider["version"]),
                         ("hwa.flute-isolation", "flute-isolation-2"))
        self.assertEqual(provider["settings"],
                         {"instrument": "flute", "disturbance_samples": [list(span)]})
        audio = {row["kind"]: row for row in manifest["audio"]}
        recording = audio["source-recording"]
        self.assertEqual(recording["sha256"], hashlib.sha256(self.source.read_bytes()).hexdigest())
        for row in audio.values():
            fmt = row["format"]
            self.assertEqual((fmt["sample_rate_hz"], fmt["channels"], fmt["channel_mask"],
                              fmt["frames"]), (rate, 2, 3, frames))
        flute = audio["instrument-stem"]
        self.assertEqual((flute["name"], flute["relative_path"]), ("flute", "audio/flute.wav"))
        self.assertEqual(flute["source_recording_id"], recording["id"])
        events = (self.output / "events.jsonl").read_text(encoding="utf-8").splitlines()
        event, = map(json.loads, events)
        self.assertEqual((event["kind"], event["part"], event["start_sample"], event["end_sample"],
                          event["evidence_audio_id"]),
                         ("instrument-region", "flute", 0, frames, flute["id"]))

        stem = read_float_pcm(self.output / flute["relative_path"])
        removed = read_float_pcm(self.output / audio["derived-audio"]["relative_path"])
        self.assertEqual((len(stem), len(removed)), (len(source), len(source)))
        self.assertLess(max(abs(keep + drop - original / 32768)
                            for keep, drop, original in zip(stem, removed, source)), 1e-7)
        self.assertTrue(any(removed[2 * span[0]:2 * span[1]]))
        self.assertFalse(any(removed[:2 * span[0]]))
        self.assertFalse(any(removed[2 * span[1]:]))

    def test_float64_conversion_leaves_no_removed_audio_outside_the_mark(self):
        rate = 8000
        samples = [0.18 * math.sin(math.tau * 523.25 * i / rate) for i in range(1607)]
        write_float_pcm(self.source, samples, rate, bits=64)
        begin, end = 350, 1050
        result = self.isolate("--disturbance-samples", f"{begin}:{end}")
        self.assertEqual(result.returncode, 0, result.stderr)

        kept = read_float_pcm(self.output / "audio" / "flute.wav")
        removed = read_float_pcm(self.output / "audio" / "removed.wav")
        expected = struct.unpack(f"<{len(samples)}f", struct.pack(f"<{len(samples)}f", *samples))
        self.assertEqual(kept[:begin] + kept[end:], expected[:begin] + expected[end:])
        self.assertFalse(any(removed[:begin] + removed[end:]))
        self.assertLess(max(abs(a + b - source) for a, b, source in zip(kept, removed, samples)), 1e-7)

    def test_removes_a_panned_step_without_cutting_a_note_in_the_other_channel(self):
        rate = 22050
        onset = int(0.65 * rate)
        step = [8 * sample for sample in read_fixture("step")]
        note = self.note(rate, round(0.12 * rate), 523.25, 1000, 123)
        left = [0] * (2 * rate)
        right = left.copy()
        left[onset:onset + len(step)] = step
        right[onset:onset + len(note)] = note
        write_pcm(self.source, [value for pair in zip(left, right) for value in pair], rate, 2)
        step_energy = sum(sample ** 2 for sample in step)
        note_energy = sum(sample ** 2 for sample in note)

        mark = f"{onset - 2048}:{onset + len(step) + 2048}"
        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"flute-stereo-step-{marked}.hwa-events"
                flags = ("--disturbance-samples", mark) if marked else ()
                result = self.isolate(*flags)
                self.assertEqual(result.returncode, 0, result.stderr)
                stem = read_float_pcm(self.output / "audio" / "flute.wav")
                left_energy = sum((sample * 32768) ** 2 for sample in stem[::2])
                right_error = sum((sample * 32768 - clean) ** 2
                                  for sample, clean in zip(stem[1::2], right))
                self.assertLess(right_error, 0.01 * note_energy)
                self.assertLess(left_energy, 0.8 * step_energy)

    def test_preserves_tongued_low_notes_and_vibrato(self):
        rate = 22050
        clean = [0] * round(2.7 * rate)
        regions = []
        for seed, (onset, duration, pitch, cents) in enumerate((
            (0.35, 0.058, 246.94, 0),
            (0.65, 0.076, 261.63, 0),
            (1.02, 0.52, 880.0, 23),
            (1.86, 0.055, 2093.0, 0),
        ), 1):
            start, frames = round(onset * rate), round(duration * rate)
            note = self.note(rate, frames, pitch, 5200, seed, vibrato_cents=cents)
            clean[start:start + frames] = note
            regions.append((pitch, start, start + frames))

        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"flute-phrase-{marked}.hwa-events"
                span = (round(0.30 * rate), round(1.97 * rate)) if marked else None
                stem = self.isolate_mono(clean, rate, span)
                for pitch, start, end in regions:
                    energy = sum(sample ** 2 for sample in clean[start:end])
                    error = sum((sample - value) ** 2
                                for sample, value in zip(clean[start:end], stem[start:end]))
                    self.assertLess(error, 0.01 * energy, pitch)
                    # First differences give the breath in the attack more weight.
                    attack = range(start + 1, start + round(0.025 * rate))
                    attack_energy = sum((clean[i] - clean[i - 1]) ** 2 for i in attack)
                    attack_error = sum(((clean[i] - clean[i - 1]) - (stem[i] - stem[i - 1])) ** 2
                                       for i in attack)
                    self.assertLess(attack_error, 0.02 * attack_energy, pitch)

    def test_preserves_a_short_airy_low_note(self):
        rate = 8000
        onset = rate // 5
        note = self.note(rate, round(0.056 * rate), 261.63, 2300, 113,
                         vibrato_cents=16, breath=0.75)
        clean = [0] * onset + note + [0] * onset
        energy = sum(sample ** 2 for sample in note)
        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"flute-short-airy-{marked}.hwa-events"
                span = (onset - rate // 50, onset + len(note) + rate // 50) if marked else None
                stem = self.isolate_mono(clean, rate, span)
                error = sum((sample - stem[i]) ** 2 for i, sample in enumerate(note, onset))
                self.assertLess(error, 0.01 * energy)

    def test_preserves_breath_before_and_after_a_short_note(self):
        rate = 22050
        frames = round(0.07 * rate)
        rng = random.Random(34123)
        breaths = []
        for _ in range(2):
            air = []
            previous_noise = 0.0
            for i in range(frames):
                noise = rng.uniform(-1.0, 1.0)
                envelope = math.sqrt(math.sin(math.pi * i / frames))
                air.append(round(900 * envelope * (noise - 0.6 * previous_noise)))
                previous_noise = noise
            breaths.append(air)
        note = self.note(rate, round(0.058 * rate), 246.94, 4200, 42139,
                         vibrato_cents=23, breath=0.22)
        onset = round(0.63 * rate)
        phrase = breaths[0] + note + breaths[1]
        clean = [0] * onset + phrase + [0] * rate
        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"flute-breath-{marked}.hwa-events"
                span = (onset - 441, onset + len(phrase) + 441) if marked else None
                stem = self.isolate_mono(clean, rate, span)
                for start, air in zip((onset, onset + frames + len(note)), breaths):
                    error = sum((sample - stem[i]) ** 2 for i, sample in enumerate(air, start))
                    self.assertLess(error, 0.01 * sum(sample ** 2 for sample in air))

    def test_reduces_recorded_cough_over_a_held_note_and_step_in_rest(self):
        rate = 22050
        clean = [0] * round(3.25 * rate)
        onset = round(0.25 * rate)
        note = self.note(rate, round(1.65 * rate), 392.0, 5000, 1, vibrato_cents=15, breath=0.02)
        clean[onset:onset + len(note)] = note
        mix = clean.copy()
        spans = []
        for kind, time, gain in (("cough", 0.92, 1), ("step", 2.40, 24)):
            noise = read_fixture(kind)
            start = round(time * rate)
            for i, sample in enumerate(noise, start):
                mix[i] += gain * sample
            spans.append((kind, start, start + len(noise)))
        reference = self.root / "flute-reference.wav"
        write_pcm(reference, clean, rate, 1)
        write_pcm(self.source, mix, rate, 1)
        marks = tuple(arg for _, start, end in spans
                      for arg in ("--disturbance-samples", f"{start - 441}:{end + 441}"))

        for mode, flags in (("automatic", ()), ("marked", marks)):
            with self.subTest(mode=mode):
                self.output = self.root / f"flute-{mode}.hwa-events"
                result = self.isolate(*flags)
                self.assertEqual(result.returncode, 0, result.stderr)
                stem_path = self.output / "audio" / "flute.wav"
                report = self.evaluate(reference, stem_path)
                self.assertGreater(report["si_sdr_improvement_db"], 1.0, report)
                stem = read_float_pcm(stem_path)
                for kind, start, end in spans:
                    indices = range(start, end)
                    mix_error = sum((mix[i] - clean[i]) ** 2 for i in indices)
                    stem_error = sum((stem[i] * 32768 - clean[i]) ** 2 for i in indices)
                    self.assertLess(stem_error, 0.8 * mix_error, kind)


class OboeIsolation(InstrumentIsolationTest):
    instrument = "oboe"

    @staticmethod
    def note(rate, frames, pitch, amplitude, seed, vibrato_cents=0):
        rng = random.Random(seed)
        harmonics = [(harmonic, level)
                     for harmonic, level in enumerate((0.58, 1.0, 0.83, 0.56,
                                                       0.35, 0.26, 0.14, 0.08), 1)
                     if harmonic * pitch < rate / 2]
        phase = 0.0
        samples = []
        for frame in range(frames):
            t = frame / rate
            vibrato = math.sin(math.tau * 5.3 * t) * vibrato_cents
            phase += math.tau * pitch * 2 ** (vibrato / 1200) / rate
            envelope = min(1.0, t / 0.004, (frames - frame) / (rate * 0.010))
            reed = sum(level * math.sin(harmonic * phase) for harmonic, level in harmonics)
            tongue = (0.015 + 0.45 * math.exp(-t / 0.008)) * rng.uniform(-1.0, 1.0)
            samples.append(round(amplitude * envelope * (reed + tongue)))
        return samples

    def test_preserves_tongued_and_vibrato_notes_across_the_range(self):
        for rate in (8000, 22050):
            clean = [0] * round(1.85 * rate)
            regions = []
            for seed, (onset, duration, pitch, cents) in enumerate((
                (0.25, 0.065, 233.08, 0),   # B-flat3
                (0.55, 0.46, 587.33, 27),  # D5
                (1.30, 0.060, 1760.0, 0),  # A6
            ), 1):
                start, frames = round(onset * rate), round(duration * rate)
                clean[start:start + frames] = self.note(rate, frames, pitch, 5200, seed, cents)
                regions.append((pitch, start, start + frames))

            for marked in (False, True):
                with self.subTest(rate=rate, marked=marked):
                    self.output = self.root / f"oboe-{rate}-{marked}.hwa-events"
                    span = (round(0.22 * rate), round(1.40 * rate)) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    for pitch, start, end in regions:
                        energy = sum(sample ** 2 for sample in clean[start:end])
                        error = sum((stem[i] - clean[i]) ** 2 for i in range(start, end))
                        self.assertLess(error, 0.01 * energy, pitch)
                        # First differences give the high-frequency attack more weight.
                        attack = range(start + 1, start + round(0.025 * rate))
                        attack_energy = sum((clean[i] - clean[i - 1]) ** 2 for i in attack)
                        attack_error = sum(((stem[i] - stem[i - 1])
                                            - (clean[i] - clean[i - 1])) ** 2 for i in attack)
                        self.assertLess(attack_error, 0.02 * attack_energy, pitch)

    def test_preserves_a_high_note_release_at_8_khz(self):
        rate = 8000
        first = self.note(rate, round(0.261 * rate), 587.33, 2600, 47, 33)
        for start, pitch, cents in ((0.72, 783.99, 29), (0.70, 1396.91, 15)):
            onset = round(start * rate)
            last = self.note(rate, round(0.142 * rate), pitch, 2900, 48, cents)
            end = onset + len(first) + len(last)
            clean = [0] * onset + first + last + [0] * rate
            released = last[-(rate // 50):]
            energy = sum(sample ** 2 for sample in released)
            for marked in (False, True):
                with self.subTest(pitch=pitch, marked=marked):
                    self.output = self.root / f"oboe-release-{pitch}-{marked}.hwa-events"
                    span = (onset - rate // 20, end + rate // 20) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    error = sum((sample - value) ** 2
                                for sample, value in zip(released, stem[end - len(released):end]))
                    self.assertLess(error, 0.01 * energy)

    def test_preserves_a_very_short_quiet_high_f(self):
        rate = 8000
        onset = rate // 5
        note = self.note(rate, round(0.03 * rate), 1396.91, 115, 45)
        clean = [0] * onset + note + [0] * rate
        energy = sum(sample ** 2 for sample in note)
        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"oboe-short-f-{marked}.hwa-events"
                span = (onset - rate // 50, onset + len(note) + rate // 50) if marked else None
                stem = self.isolate_mono(clean, rate, span)
                error = sum((sample - stem[i]) ** 2 for i, sample in enumerate(note, onset))
                self.assertLess(error, 0.01 * energy)

    def test_reduces_recorded_cough_on_a_held_note_and_step_in_a_rest(self):
        rate = 22050
        onset = round(0.25 * rate)
        note = self.note(rate, round(1.65 * rate), 349.23, 5000, 11, vibrato_cents=18)
        clean = [0] * round(3.25 * rate)
        clean[onset:onset + len(note)] = note
        mix = clean.copy()
        spans = []
        for kind, time, gain in (("cough", 0.92, 1), ("step", 2.4, 24)):
            start = round(time * rate)
            noise = read_fixture(kind)
            for i, sample in enumerate(noise, start):
                mix[i] += gain * sample
            spans.append((kind, start, start + len(noise)))
        reference = self.root / "reference.wav"
        write_pcm(reference, clean, rate, 1)
        write_pcm(self.source, mix, rate, 1)
        marks = tuple(arg for _, start, end in spans
                      for arg in ("--disturbance-samples", f"{start - 441}:{end + 441}"))

        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"oboe-disturbance-{marked}.hwa-events"
                result = self.isolate(*(marks if marked else ()))
                self.assertEqual(result.returncode, 0, result.stderr)
                stem_path = self.output / "audio" / "oboe.wav"
                report = self.evaluate(reference, stem_path)
                self.assertGreater(report["si_sdr_improvement_db"], 0.7, report)
                stem = read_float_pcm(stem_path)
                for kind, start, end in spans:
                    indices = range(start, end)
                    mix_error = sum((mix[i] - clean[i]) ** 2 for i in indices)
                    stem_error = sum((stem[i] * 32768 - clean[i]) ** 2 for i in indices)
                    self.assertLess(stem_error, 0.85 * mix_error, kind)

    def test_panned_step_cleanup_preserves_the_other_channel_and_source_clock(self):
        rate, frames = 22050, 46237
        onset = round(0.7 * rate)
        step = [12 * sample for sample in read_fixture("step")]
        note = self.note(rate, round(0.12 * rate), 523.25, 550, 23)
        left, right = [0] * frames, [0] * frames
        left[onset:onset + len(step)] = step
        right[onset:onset + len(note)] = note
        source = [value for pair in zip(left, right) for value in pair]
        write_pcm(self.source, source, rate, 2, mask=3)

        start, end = onset - 441, onset + len(step) + 441
        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"oboe-stereo-{marked}.hwa-events"
                flags = ("--disturbance-samples", f"{start}:{end}") if marked else ()
                result = self.isolate(*flags)
                self.assertEqual(result.returncode, 0, result.stderr)
                validated = self.cli("validate-event-bundle", self.output)
                self.assertEqual(validated.returncode, 0, validated.stderr)
                manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
                provider, = manifest["providers"]
                self.assertEqual((provider["name"], provider["version"]),
                                 ("hwa.oboe-isolation", "oboe-isolation-2"))
                self.assertEqual(provider["settings"], {"instrument": "oboe",
                                 "disturbance_samples": [[start, end]] if marked else []})
                audio = {row["kind"]: row for row in manifest["audio"]}
                recording = audio["source-recording"]
                for row in audio.values():
                    fmt = row["format"]
                    self.assertEqual((fmt["sample_rate_hz"], fmt["channels"], fmt["channel_mask"],
                                      fmt["frames"]), (rate, 2, 3, frames))
                oboe = audio["instrument-stem"]
                self.assertEqual((oboe["name"], oboe["relative_path"]), ("oboe", "audio/oboe.wav"))
                self.assertEqual(oboe["source_recording_id"], recording["id"])
                lines = (self.output / "events.jsonl").read_text(encoding="utf-8").splitlines()
                event, = map(json.loads, lines)
                self.assertEqual((event["kind"], event["part"], event["start_sample"],
                                  event["end_sample"], event["evidence_audio_id"]),
                                 ("instrument-region", "oboe", 0, frames, oboe["id"]))
                stem = read_float_pcm(self.output / oboe["relative_path"])
                removed = read_float_pcm(self.output / audio["derived-audio"]["relative_path"])
                self.assertEqual((len(stem), len(removed)), (2 * frames, 2 * frames))
                self.assertLess(max(abs(a + b - original / 32768)
                                    for a, b, original in zip(stem, removed, source)), 1e-7)
                step_energy = sum((sample * 32768) ** 2 for sample in stem[::2])
                note_error = sum((sample * 32768 - original) ** 2
                                 for sample, original in zip(stem[1::2], right))
                self.assertLess(step_energy, 0.8 * sum(sample ** 2 for sample in step))
                self.assertLess(note_error, 0.01 * sum(sample ** 2 for sample in note))
                if marked:
                    self.assertFalse(any(removed[:2 * start]))
                    self.assertFalse(any(removed[2 * end:]))


class EnglishHornIsolation(InstrumentIsolationTest):
    instrument = "english-horn"

    def test_preserves_low_and_high_notes_with_tongued_attacks(self):
        for rate in (8000, 22050):
            clean = [0] * round(1.8 * rate)
            notes = []
            for seed, (time, duration, pitch, cents) in enumerate((
                (0.25, 0.062, 164.81, 0),     # Sounding E3
                (0.53, 0.42, 261.63, 24),    # Sounding C4
                (1.18, 0.142, 1046.50, 19),  # Sounding C6
            ), 1):
                begin = round(time * rate)
                note = OboeIsolation.note(rate, round(duration * rate), pitch, 4700, seed, cents)
                end = begin + len(note)
                clean[begin:end] = note
                notes.append((pitch, begin, end))

            for marked in (False, True):
                with self.subTest(rate=rate, marked=marked):
                    self.output = self.root / f"english-horn-{rate}-{marked}.hwa-events"
                    span = (round(0.20 * rate), round(1.40 * rate)) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    for pitch, begin, end in notes:
                        energy = sum(sample ** 2 for sample in clean[begin:end])
                        error = sum((stem[i] - clean[i]) ** 2 for i in range(begin, end))
                        self.assertLess(error, 0.01 * energy, pitch)

                        # First differences give the tongued attack more weight.
                        attack = range(begin + 1, begin + round(0.022 * rate))
                        attack_energy = sum((clean[i] - clean[i - 1]) ** 2 for i in attack)
                        attack_error = sum(((stem[i] - stem[i - 1])
                                            - (clean[i] - clean[i - 1])) ** 2 for i in attack)
                        self.assertLess(attack_error, 0.02 * attack_energy, pitch)

                        release = range(end - rate // 50, end)
                        release_energy = sum(clean[i] ** 2 for i in release)
                        release_error = sum((stem[i] - clean[i]) ** 2 for i in release)
                        self.assertLess(release_error, 0.02 * release_energy, pitch)

    def test_preserves_a_very_short_quiet_low_e(self):
        for rate in (8000, 22050):
            onset = rate // 5
            note = OboeIsolation.note(rate, round(0.03 * rate), 164.81, 115, 45)
            clean = [0] * onset + note + [0] * rate
            energy = sum(sample ** 2 for sample in note)
            for marked in (False, True):
                with self.subTest(rate=rate, marked=marked):
                    self.output = self.root / f"english-horn-short-e-{rate}-{marked}.hwa-events"
                    span = (onset - rate // 50, onset + len(note) + rate // 50) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    error = sum((sample - stem[i]) ** 2 for i, sample in enumerate(note, onset))
                    self.assertLess(error, 0.01 * energy)

    def test_preserves_a_quiet_note_between_short_notes(self):
        rate = 8000
        gap = rate // 200
        clean = [0] * (rate // 5)
        notes = []
        for seed, (pitch, duration, amplitude) in enumerate((
            (164.81, 0.028, 3400), (174.61, 0.048, 115),
            (196.0, 0.038, 4400), (164.81, 0.030, 240),
        ), 27):
            note = OboeIsolation.note(rate, round(duration * rate), pitch, amplitude, seed, 17)
            notes.append((len(clean), note))
            clean.extend(note + [0] * gap)
        clean.extend([0] * (rate // 5))
        onset, quiet = notes[1]
        energy = sum(sample ** 2 for sample in quiet)
        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"english-horn-quiet-between-{marked}.hwa-events"
                span = (rate // 8, len(clean) - rate // 8) if marked else None
                stem = self.isolate_mono(clean, rate, span)
                error = sum((sample - stem[i]) ** 2 for i, sample in enumerate(quiet, onset))
                self.assertLess(error, 0.01 * energy)

    def test_reduces_a_recorded_cough_and_step(self):
        rate = 22050
        onset = rate // 4
        note = OboeIsolation.note(rate, round(1.65 * rate), 220.0, 5000, 11, 18)
        clean = [0] * round(3.25 * rate)
        clean[onset:onset + len(note)] = note
        mixture = clean.copy()
        spans = []
        for kind, time, gain in (("cough", 0.92, 1), ("step", 2.40, 24)):
            noise = read_fixture(kind)
            start = round(time * rate)
            for i, sample in enumerate(noise, start):
                mixture[i] += gain * sample
            spans.append((kind, start, start + len(noise)))
        reference = self.root / "reference.wav"
        write_pcm(reference, clean, rate, 1)
        write_pcm(self.source, mixture, rate, 1)
        marks = tuple(flag for _, start, end in spans
                      for flag in ("--disturbance-samples", f"{start - 441}:{end + 441}"))

        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"english-horn-disturbance-{marked}.hwa-events"
                result = self.isolate(*(marks if marked else ()))
                self.assertEqual(result.returncode, 0, result.stderr)
                stem_path = self.output / "audio" / "english-horn.wav"
                self.assertGreater(self.evaluate(reference, stem_path)["si_sdr_improvement_db"], 0.7)
                stem = read_float_pcm(stem_path)
                for kind, start, end in spans:
                    mixture_error = sum((mixture[i] - clean[i]) ** 2 for i in range(start, end))
                    stem_error = sum((stem[i] * 32768 - clean[i]) ** 2 for i in range(start, end))
                    self.assertLess(stem_error, 0.85 * mixture_error, kind)

    def test_panned_step_cleanup_preserves_the_other_channel_and_source_clock(self):
        rate, frames = 22050, 46237
        onset = round(0.7 * rate)
        step = [12 * sample for sample in read_fixture("step")]
        note = OboeIsolation.note(rate, round(0.12 * rate), 164.81, 625, 23)
        left, right = [0] * frames, [0] * frames
        left[onset:onset + len(step)] = step
        right[onset:onset + len(note)] = note
        source = [value for pair in zip(left, right) for value in pair]
        write_pcm(self.source, source, rate, 2, mask=3)
        start, end = onset - 441, onset + len(step) + 441

        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"english-horn-stereo-{marked}.hwa-events"
                marks = ("--disturbance-samples", f"{start}:{end}") if marked else ()
                result = self.isolate(*marks)
                self.assertEqual(result.returncode, 0, result.stderr)
                validated = self.cli("validate-event-bundle", self.output)
                self.assertEqual(validated.returncode, 0, validated.stderr)
                manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
                provider, = manifest["providers"]
                self.assertEqual((provider["name"], provider["version"]),
                                 ("hwa.english-horn-isolation", "english-horn-isolation-2"))
                self.assertEqual(provider["settings"], {"instrument": "english-horn",
                                 "disturbance_samples": [[start, end]] if marked else []})
                audio = {row["kind"]: row for row in manifest["audio"]}
                for row in audio.values():
                    fmt = row["format"]
                    self.assertEqual((fmt["sample_rate_hz"], fmt["channels"], fmt["channel_mask"],
                                      fmt["frames"]), (rate, 2, 3, frames))
                horn = audio["instrument-stem"]
                self.assertEqual((horn["name"], horn["relative_path"], horn["source_recording_id"]),
                                 ("english-horn", "audio/english-horn.wav",
                                  audio["source-recording"]["id"]))
                lines = (self.output / "events.jsonl").read_text(encoding="utf-8").splitlines()
                event, = map(json.loads, lines)
                self.assertEqual((event["part"], event["start_sample"], event["end_sample"],
                                  event["evidence_audio_id"]), ("english-horn", 0, frames, horn["id"]))
                stem = read_float_pcm(self.output / horn["relative_path"])
                removed = read_float_pcm(self.output / audio["derived-audio"]["relative_path"])
                self.assertEqual((len(stem), len(removed)), (2 * frames, 2 * frames))
                self.assertLess(max(abs(a + b - sample / 32768)
                                    for a, b, sample in zip(stem, removed, source)), 1e-7)
                step_energy = sum((sample * 32768) ** 2 for sample in stem[::2])
                note_error = sum((sample * 32768 - clean) ** 2
                                 for sample, clean in zip(stem[1::2], right))
                self.assertLess(step_energy, 0.8 * sum(sample ** 2 for sample in step))
                self.assertLess(note_error, 0.01 * sum(sample ** 2 for sample in note))
                if marked:
                    self.assertFalse(any(removed[:2 * start]))
                    self.assertFalse(any(removed[2 * end:]))


class ClarinetIsolation(InstrumentIsolationTest):
    instrument = "clarinet"

    @staticmethod
    def note(rate, frames, pitch, amplitude, seed, vibrato_cents=0, upper_register=False):
        rng = random.Random(seed)
        levels = (1.0, 0.16, 0.46, 0.10, 0.21, 0.07, 0.12) if upper_register else (
            0.72, 0.03, 1.0, 0.035, 0.48, 0.025, 0.24, 0.018, 0.12)
        partials = [(j, level) for j, level in enumerate(levels, 1)
                    if j * pitch * 2 ** (abs(vibrato_cents) / 1200) < rate / 2]
        phase = 0.0
        samples = []
        for i in range(frames):
            time = i / rate
            cents = vibrato_cents * math.sin(math.tau * 5.4 * time)
            phase += math.tau * pitch * 2 ** (cents / 1200) / rate
            envelope = min(1.0, time / 0.004, (frames - i) / (rate * 0.010))
            tone = sum(level * math.sin(j * phase) for j, level in partials)
            tongue = (0.025 + 0.30 * math.exp(-time / 0.007)) * rng.uniform(-1.0, 1.0)
            samples.append(round(amplitude * envelope * (tone + tongue)))
        return samples

    def test_preserves_a_short_quiet_low_d_and_upper_b_flat(self):
        for rate in (8000, 22050):
            low_onset = rate // 5
            high_onset = round(0.7 * rate)
            low = self.note(rate, round(0.036 * rate), 146.83, 115, 45)
            high = self.note(rate, round(0.09 * rate), 1864.66, 4300, 19, 18, upper_register=True)
            clean = [0] * low_onset + low
            clean.extend([0] * (high_onset - len(clean)))
            clean.extend(high + [0] * (rate // 5))
            for marked in (False, True):
                with self.subTest(rate=rate, marked=marked):
                    span = (rate // 8, high_onset + len(high) + rate // 20) if marked else None
                    self.output = self.root / f"clarinet-range-{rate}-{marked}.hwa-events"
                    stem = self.isolate_mono(clean, rate, span)
                    for onset, note in ((low_onset, low), (high_onset, high)):
                        energy = sum(sample ** 2 for sample in note)
                        error = sum((sample - stem[i]) ** 2 for i, sample in enumerate(note, onset))
                        self.assertLess(error, 0.01 * energy)
                    high_end = high_onset + len(high)
                    release = range(high_end - rate // 50, high_end)
                    release_energy = sum(clean[i] ** 2 for i in release)
                    release_error = sum((stem[i] - clean[i]) ** 2 for i in release)
                    self.assertLess(release_error, 0.0025 * release_energy)

    def test_preserves_changes_to_the_clarion_register(self):
        rate = 8000
        onset = rate // 5
        transition = round(0.006 * rate)
        for lower_pitch in (415.30, 146.83):
            # A-flat4 to A4 crosses the throat break. Low D3 also overblows to A4.
            lower = self.note(rate, round(0.25 * rate), lower_pitch, 3800, 27, 15)
            upper = self.note(rate, round(0.19 * rate), 440.0, 2800, 28, 20, upper_register=True)
            phrase = lower[:-transition]
            phrase.extend(a + b for a, b in zip(lower[-transition:], upper[:transition]))
            phrase.extend(upper[transition:])
            clean = [0] * onset + phrase + [0] * onset
            boundary = onset + len(lower) - transition
            region = range(boundary - rate // 40, boundary + rate // 25)
            energy = sum(clean[i] ** 2 for i in region)
            for marked in (False, True):
                with self.subTest(lower_pitch=lower_pitch, marked=marked):
                    span = (boundary - rate // 20, boundary + rate // 20) if marked else None
                    self.output = self.root / f"clarinet-register-{lower_pitch}-{marked}.hwa-events"
                    stem = self.isolate_mono(clean, rate, span)
                    error = sum((stem[i] - clean[i]) ** 2 for i in region)
                    self.assertLess(error, 0.01 * energy)

    def test_preserves_a_low_note_as_its_fundamental_grows(self):
        rate = 22050
        clean = self.note_with_fundamental_swell(rate, 146.8324, 0.595)
        region = range(round(0.37 * rate), round(0.625 * rate))
        energy = sum(clean[i] ** 2 for i in region)
        for marked in (False, True):
            with self.subTest(marked=marked):
                span = (round(0.30 * rate), round(0.695 * rate)) if marked else None
                self.output = self.root / f"clarinet-low-partial-{marked}.hwa-events"
                stem = self.isolate_mono(clean, rate, span)
                error = sum((stem[i] - clean[i]) ** 2 for i in region)
                self.assertLess(error, 0.01 * energy)

    def test_reduces_a_recorded_cough_and_step(self):
        rate = 22050
        onset = rate // 4
        note = self.note(rate, round(1.65 * rate), 293.66, 4300, 11, 17)
        clean = [0] * round(3.25 * rate)
        clean[onset:onset + len(note)] = note
        mixture = clean.copy()
        disturbed = []
        for kind, time, gain in (("cough", 0.92, 1), ("step", 2.40, 24)):
            noise = read_fixture(kind)
            start = round(time * rate)
            for i, sample in enumerate(noise, start):
                mixture[i] += gain * sample
            disturbed.append((kind, start, start + len(noise)))
        reference = self.root / "reference.wav"
        write_pcm(reference, clean, rate, 1)
        write_pcm(self.source, mixture, rate, 1)
        marks = tuple(flag for _, start, end in disturbed
                      for flag in ("--disturbance-samples", f"{start - 441}:{end + 441}"))

        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"clarinet-disturbance-{marked}.hwa-events"
                result = self.isolate(*(marks if marked else ()))
                self.assertEqual(result.returncode, 0, result.stderr)
                stem_path = self.output / "audio" / "clarinet.wav"
                self.assertGreater(self.evaluate(reference, stem_path)["si_sdr_improvement_db"], 0.7)
                stem = read_float_pcm(stem_path)
                for kind, start, end in disturbed:
                    mixture_error = sum((mixture[i] - clean[i]) ** 2 for i in range(start, end))
                    stem_error = sum((stem[i] * 32768 - clean[i]) ** 2 for i in range(start, end))
                    self.assertLess(stem_error, 0.85 * mixture_error, kind)

    def test_panned_step_preserves_the_other_channel_and_source_clock(self):
        rate, frames = 22050, 46237
        onset = round(0.7 * rate)
        step = [12 * sample for sample in read_fixture("step")]
        note = self.note(rate, round(0.12 * rate), 146.83, 625, 23)
        left, right = [0] * frames, [0] * frames
        left[onset:onset + len(step)] = step
        right[onset:onset + len(note)] = note
        source = [value for pair in zip(left, right) for value in pair]
        write_pcm(self.source, source, rate, 2, mask=3)
        start, end = onset - 441, onset + len(step) + 441

        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"clarinet-stereo-{marked}.hwa-events"
                marks = ("--disturbance-samples", f"{start}:{end}") if marked else ()
                result = self.isolate(*marks)
                self.assertEqual(result.returncode, 0, result.stderr)
                validated = self.cli("validate-event-bundle", self.output)
                self.assertEqual(validated.returncode, 0, validated.stderr)
                manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
                provider, = manifest["providers"]
                self.assertEqual((provider["name"], provider["version"]),
                                 ("hwa.clarinet-isolation", "clarinet-isolation-2"))
                self.assertEqual(provider["settings"], {"instrument": "clarinet",
                                 "disturbance_samples": [[start, end]] if marked else []})
                audio = {row["kind"]: row for row in manifest["audio"]}
                for row in audio.values():
                    fmt = row["format"]
                    self.assertEqual((fmt["sample_rate_hz"], fmt["channels"], fmt["channel_mask"],
                                      fmt["frames"]), (rate, 2, 3, frames))
                clarinet = audio["instrument-stem"]
                self.assertEqual((clarinet["name"], clarinet["relative_path"],
                                  clarinet["source_recording_id"]),
                                 ("clarinet", "audio/clarinet.wav", audio["source-recording"]["id"]))
                lines = (self.output / "events.jsonl").read_text(encoding="utf-8").splitlines()
                event, = map(json.loads, lines)
                self.assertEqual((event["part"], event["start_sample"], event["end_sample"],
                                  event["evidence_audio_id"]), ("clarinet", 0, frames, clarinet["id"]))
                stem = read_float_pcm(self.output / clarinet["relative_path"])
                removed = read_float_pcm(self.output / audio["derived-audio"]["relative_path"])
                self.assertEqual((len(stem), len(removed)), (2 * frames, 2 * frames))
                self.assertLess(max(abs(a + b - sample / 32768)
                                    for a, b, sample in zip(stem, removed, source)), 1e-7)
                step_energy = sum((sample * 32768) ** 2 for sample in stem[::2])
                note_error = sum((sample * 32768 - clean) ** 2
                                 for sample, clean in zip(stem[1::2], right))
                self.assertLess(step_energy, 0.8 * sum(sample ** 2 for sample in step))
                self.assertLess(note_error, 0.01 * sum(sample ** 2 for sample in note))
                if marked:
                    self.assertFalse(any(removed[:2 * start]))
                    self.assertFalse(any(removed[2 * end:]))


class PiccoloIsolation(InstrumentIsolationTest):
    instrument = "piccolo"

    def test_preserves_short_notes_across_the_sounding_range(self):
        for rate, low_pitch, high_pitch in ((8000, 587.33, 3951.07),
                                            (8500, 587.33, 4186.01),
                                            (22050, 523.25, 4186.01)):
            low_onset = rate // 5
            high_onset = round(0.62 * rate)
            low = FluteIsolation.note(rate, round(0.042 * rate), low_pitch, 230, 45, 0, 0.13)
            # The upper test notes stay below Nyquist even at their vibrato peaks.
            high = FluteIsolation.note(rate, round(0.071 * rate), high_pitch, 4450, 19, 8, 0.02)
            clean = [0] * low_onset + low
            clean.extend([0] * (high_onset - len(clean)))
            clean.extend(high + [0] * low_onset)
            for marked in (False, True):
                with self.subTest(rate=rate, high_pitch=high_pitch, marked=marked):
                    self.output = self.root / f"piccolo-range-{rate}-{marked}.hwa-events"
                    span = (rate // 8, high_onset + len(high) + rate // 20) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    for onset, note in ((low_onset, low), (high_onset, high)):
                        energy = sum(sample ** 2 for sample in note)
                        error = sum((sample - stem[i]) ** 2 for i, sample in enumerate(note, onset))
                        self.assertLess(error, 0.01 * energy)

                        attack = range(onset + 1, onset + rate // 50)
                        attack_energy = sum((clean[i] - clean[i - 1]) ** 2 for i in attack)
                        attack_error = sum(((stem[i] - stem[i - 1])
                                            - (clean[i] - clean[i - 1])) ** 2 for i in attack)
                        self.assertLess(attack_error, 0.02 * attack_energy)

    def test_preserves_an_airy_c8_near_nyquist(self):
        for rate in (8373, 8700):
            onset = round(0.77 * rate)
            note = FluteIsolation.note(rate, round(0.071 * rate), 4186.009, 6500, 136, breath=0.12)
            clean = [0] * onset + note + [0] * (rate // 5)
            energy = sum(sample ** 2 for sample in note)
            for marked in (False, True):
                with self.subTest(rate=rate, marked=marked):
                    self.output = self.root / f"piccolo-airy-c8-{rate}-{marked}.hwa-events"
                    span = (onset - rate // 50, onset + len(note) + rate // 50) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    error = sum((sample - stem[i]) ** 2 for i, sample in enumerate(note, onset))
                    self.assertLess(error, 0.01 * energy)

    def test_preserves_a_high_trill_with_continuous_air(self):
        for rate in (8000, 12801, 16000):
            dwell = round(rate * 0.05)
            change = round(rate * 0.004)
            onset = rate // 5
            count = 8 * dwell
            pitches = (3520.0, 3729.31)
            rng = random.Random(172)
            phase = 0.0
            phrase = []
            for i in range(count):
                note, offset = divmod(i, dwell)
                pitch = pitches[note % 2]
                if note:
                    blend = min(1.0, offset / change)
                    pitch = blend * pitch + (1.0 - blend) * pitches[(note - 1) % 2]
                phase += math.tau * pitch / rate
                envelope = min(1.0, i / (rate * 0.004), (count - i) / (rate * 0.01))
                phrase.append(round(4200 * envelope * (math.sin(phase) + rng.uniform(-0.035, 0.035))))
            clean = [0] * onset + phrase + [0] * onset
            for marked in (False, True):
                with self.subTest(rate=rate, marked=marked):
                    self.output = self.root / f"piccolo-trill-{rate}-{marked}.hwa-events"
                    span = (onset - rate // 50, onset + count + rate // 50) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    for note in range(count // dwell):
                        start = onset + note * dwell
                        region = range(start, start + dwell)
                        energy = sum(clean[i] ** 2 for i in region)
                        error = sum((stem[i] - clean[i]) ** 2 for i in region)
                        self.assertLess(error, 0.01 * energy, note)

    def test_reduces_a_recorded_cough_and_step(self):
        rate = 22050
        onset = rate // 4
        note = FluteIsolation.note(rate, round(1.65 * rate), 1174.66, 4600, 11, 14, 0.02)
        clean = [0] * round(3.25 * rate)
        clean[onset:onset + len(note)] = note
        mixture = clean.copy()
        disturbed = []
        for kind, time, gain in (("cough", 0.92, 1), ("step", 2.40, 24)):
            noise = read_fixture(kind)
            start = round(time * rate)
            for i, sample in enumerate(noise, start):
                mixture[i] += gain * sample
            disturbed.append((kind, start, start + len(noise)))
        reference = self.root / "reference.wav"
        write_pcm(reference, clean, rate, 1)
        write_pcm(self.source, mixture, rate, 1)
        marks = tuple(flag for _, start, end in disturbed
                      for flag in ("--disturbance-samples", f"{start - 441}:{end + 441}"))

        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"piccolo-disturbance-{marked}.hwa-events"
                result = self.isolate(*(marks if marked else ()))
                self.assertEqual(result.returncode, 0, result.stderr)
                stem_path = self.output / "audio" / "piccolo.wav"
                self.assertGreater(self.evaluate(reference, stem_path)["si_sdr_improvement_db"], 0.7)
                stem = read_float_pcm(stem_path)
                for kind, start, end in disturbed:
                    mixture_error = sum((mixture[i] - clean[i]) ** 2 for i in range(start, end))
                    stem_error = sum((stem[i] * 32768 - clean[i]) ** 2 for i in range(start, end))
                    self.assertLess(stem_error, 0.85 * mixture_error, kind)

    def test_a_panned_step_leaves_the_other_channel_and_source_clock_intact(self):
        rate, frames = 22050, 46237
        onset = round(0.70 * rate)
        step = [12 * sample for sample in read_fixture("step")]
        note = FluteIsolation.note(rate, round(0.12 * rate), 3520.0, 780, 23, 12, 0.02)
        left, right = [0] * frames, [0] * frames
        left[onset:onset + len(step)] = step
        right[onset:onset + len(note)] = note
        source = [value for pair in zip(left, right) for value in pair]
        write_pcm(self.source, source, rate, 2, mask=3)
        start, end = onset - 441, onset + len(step) + 441

        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"piccolo-stereo-{marked}.hwa-events"
                marks = ("--disturbance-samples", f"{start}:{end}") if marked else ()
                result = self.isolate(*marks)
                self.assertEqual(result.returncode, 0, result.stderr)
                valid = self.cli("validate-event-bundle", self.output)
                self.assertEqual(valid.returncode, 0, valid.stderr)
                manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
                provider, = manifest["providers"]
                self.assertEqual((provider["name"], provider["version"]),
                                 ("hwa.piccolo-isolation", "piccolo-isolation-2"))
                self.assertEqual(provider["settings"], {"instrument": "piccolo",
                                 "disturbance_samples": [[start, end]] if marked else []})
                audio = {row["kind"]: row for row in manifest["audio"]}
                for row in audio.values():
                    fmt = row["format"]
                    self.assertEqual((fmt["sample_rate_hz"], fmt["channels"], fmt["channel_mask"],
                                      fmt["frames"]), (rate, 2, 3, frames))
                piccolo = audio["instrument-stem"]
                self.assertEqual((piccolo["name"], piccolo["relative_path"],
                                  piccolo["source_recording_id"]),
                                 ("piccolo", "audio/piccolo.wav", audio["source-recording"]["id"]))
                stem = read_float_pcm(self.output / piccolo["relative_path"])
                removed = read_float_pcm(self.output / audio["derived-audio"]["relative_path"])
                self.assertEqual((len(stem), len(removed)), (2 * frames, 2 * frames))
                self.assertLess(max(abs(a + b - sample / 32768)
                                    for a, b, sample in zip(stem, removed, source)), 1e-7)
                step_energy = sum((sample * 32768) ** 2 for sample in stem[::2])
                note_error = sum((sample * 32768 - clean) ** 2
                                 for sample, clean in zip(stem[1::2], right))
                self.assertLess(step_energy, 0.8 * sum(sample ** 2 for sample in step))
                self.assertLess(note_error, 0.01 * sum(sample ** 2 for sample in note))
                if marked:
                    self.assertFalse(any(removed[:2 * start]))
                    self.assertFalse(any(removed[2 * end:]))


class BassoonIsolation(InstrumentIsolationTest):
    instrument = "bassoon"

    @staticmethod
    def note(rate, frames, pitch, amplitude, seed, vibrato_cents=0, fundamental=0.18):
        rng = random.Random(seed)
        levels = (fundamental, 0.72, 1.0, 0.58, 0.36, 0.25, 0.18, 0.10, 0.055)
        peak_pitch = pitch * 2 ** (abs(vibrato_cents) / 1200)
        partials = [(j, level) for j, level in enumerate(levels, 1) if j * peak_pitch < rate / 2]
        phase = 0.0
        previous_noise = 0.0
        samples = []
        for i in range(frames):
            time = i / rate
            cents = vibrato_cents * math.sin(math.tau * 5.2 * time)
            phase += math.tau * pitch * 2 ** (cents / 1200) / rate
            envelope = min(1.0, time / 0.007, (frames - i) / (rate * 0.016))
            tone = sum(level * math.sin(j * phase) for j, level in partials)
            noise = rng.uniform(-1.0, 1.0)
            air = (noise - 0.65 * previous_noise) * (0.045 + 0.40 * math.exp(-time / 0.012))
            previous_noise = noise
            samples.append(round(amplitude * envelope * (tone + air)))
        return samples

    def test_preserves_low_b_flat_breath_and_high_register(self):
        for rate in (8000, 22050):
            low_onset = round(0.27 * rate)
            high_onset = round(0.80 * rate)
            low = self.note(rate, round(0.112 * rate), 58.27, 270, 45)
            high = self.note(rate, round(0.27 * rate), 698.46, 4200, 19, 27, fundamental=0.58)
            breath_frames = round(0.032 * rate)
            rng = random.Random(29)
            breath = [round(95 * math.sin(math.pi * i / breath_frames) * rng.uniform(-1.0, 1.0))
                      for i in range(breath_frames)]
            clean = [0] * (low_onset - breath_frames) + breath + low
            clean.extend([0] * (high_onset - len(clean)))
            clean.extend(high + [0] * (rate // 5))

            for marked in (False, True):
                with self.subTest(rate=rate, marked=marked):
                    self.output = self.root / f"bassoon-range-{rate}-{marked}.hwa-events"
                    span = (rate // 5, high_onset + len(high) + rate // 20) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    for onset, note in ((low_onset - breath_frames, breath),
                                        (low_onset, low), (high_onset, high)):
                        energy = sum(sample ** 2 for sample in note)
                        error = sum((sample - stem[i]) ** 2 for i, sample in enumerate(note, onset))
                        self.assertLess(error, 0.01 * energy, onset)

                    attack = range(low_onset + 1, low_onset + rate // 50)
                    attack_energy = sum((clean[i] - clean[i - 1]) ** 2 for i in attack)
                    attack_error = sum(((stem[i] - stem[i - 1])
                                        - (clean[i] - clean[i - 1])) ** 2 for i in attack)
                    self.assertLess(attack_error, 0.02 * attack_energy)

    def test_preserves_a_very_short_quiet_b_after_a_louder_note(self):
        levels = (0.10, 0.30, 0.35, 0.32, 0.52, 0.61, 0.85, 1.0, 0.94, 0.70, 0.52, 0.26, 0.14)

        def low_note(rate, pitch, duration, amplitude, seed):
            length = round(rate * duration)
            rng = random.Random(seed)
            phase = 0.0
            previous_air = 0.0
            samples = []
            for i in range(length):
                time = i / rate
                envelope = min(1.0, time / 0.006, (length - i) / (rate * 0.016))
                phase += math.tau * pitch / rate
                air = rng.uniform(-1.0, 1.0)
                noise = (air - 0.55 * previous_air) * (0.040 + 0.16 * math.exp(-time / 0.009))
                previous_air = air
                tone = sum(level * math.sin(j * phase + 0.9 * (j - 1))
                           for j, level in enumerate(levels, 1))
                samples.append(round(amplitude * envelope * (tone + noise)))
            return samples

        for rate in (8000, 13654):
            loud_start, quiet_start = round(1.76 * rate), round(2.02 * rate)
            loud = low_note(rate, 58.2705, 0.16, 2000, 142)
            quiet = low_note(rate, 61.7354, 0.036, 115, 143)
            clean = [0] * loud_start + loud
            clean.extend([0] * (quiet_start - len(clean)))
            clean.extend(quiet + [0] * (rate // 4))
            for marked in (False, True):
                with self.subTest(rate=rate, marked=marked):
                    self.output = self.root / f"bassoon-quiet-after-{rate}-{marked}.hwa-events"
                    span = (loud_start - rate // 10, len(clean) - rate // 10) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    energy = sum(sample ** 2 for sample in quiet)
                    error = sum((sample - stem[i]) ** 2 for i, sample in enumerate(quiet, quiet_start))
                    self.assertLess(error, 0.01 * energy)

    def test_preserves_short_slurred_notes(self):
        low_levels = (0.12, 0.61, 0.31, 0.89, 0.44, 0.75, 0.92, 0.32, 1.0, 0.69,
                      0.31, 0.16, 0.14, 0.08)
        high_levels = (1.0, 0.96, 0.48, 0.18, 0.13, 0.07, 0.03)
        for rate, duration, pitches, levels in (
            (8000, 0.055, (58.2705, 61.7354, 65.4064, 61.7354), low_levels),
            (15754, 0.050, (622.2540, 659.2551, 698.4565, 659.2551) * 2, high_levels),
        ):
            note_frames = round(duration * rate)
            phrase = self.slurred_phrase(rate, note_frames, pitches, levels)
            onset = rate // 5
            clean = [0] * onset + phrase + [0] * onset
            for marked in (False, True):
                with self.subTest(rate=rate, marked=marked):
                    self.output = self.root / f"bassoon-slur-{rate}-{marked}.hwa-events"
                    span = (onset - rate // 50, onset + len(phrase) + rate // 50) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    for j, pitch in enumerate(pitches):
                        start = onset + j * note_frames
                        frames = range(start, start + note_frames)
                        energy = sum(clean[i] ** 2 for i in frames)
                        error = sum((clean[i] - stem[i]) ** 2 for i in frames)
                        self.assertLessEqual(error, 0.0025 * energy, pitch)

    def test_reduces_a_recorded_cough_and_step(self):
        rate = 22050
        onset = rate // 4
        note = self.note(rate, round(1.65 * rate), 164.81, 3900, 11, 17)
        clean = [0] * round(3.25 * rate)
        clean[onset:onset + len(note)] = note
        mixture = clean.copy()
        spans = []
        for kind, time, gain in (("cough", 0.92, 1), ("step", 2.40, 24)):
            noise = read_fixture(kind)
            start = round(time * rate)
            for i, sample in enumerate(noise, start):
                mixture[i] += gain * sample
            spans.append((kind, start, start + len(noise)))
        reference = self.root / "reference.wav"
        write_pcm(reference, clean, rate, 1)
        write_pcm(self.source, mixture, rate, 1)
        marks = tuple(flag for _, start, end in spans
                      for flag in ("--disturbance-samples", f"{start - 441}:{end + 441}"))

        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"bassoon-disturbance-{marked}.hwa-events"
                result = self.isolate(*(marks if marked else ()))
                self.assertEqual(result.returncode, 0, result.stderr)
                stem_path = self.output / "audio" / "bassoon.wav"
                self.assertGreater(self.evaluate(reference, stem_path)["si_sdr_improvement_db"], 0.7)
                stem = read_float_pcm(stem_path)
                for kind, start, end in spans:
                    mixture_error = sum((mixture[i] - clean[i]) ** 2 for i in range(start, end))
                    stem_error = sum((stem[i] * 32768 - clean[i]) ** 2 for i in range(start, end))
                    self.assertLess(stem_error, 0.85 * mixture_error, kind)

    def test_panned_step_preserves_the_other_channel_and_source_clock(self):
        rate, frames = 22050, 46237
        onset = round(0.70 * rate)
        step = [12 * sample for sample in read_fixture("step")]
        note = self.note(rate, round(0.15 * rate), 58.27, 600, 23)
        left, right = [0] * frames, [0] * frames
        left[onset:onset + len(step)] = step
        right[onset:onset + len(note)] = note
        source = [value for pair in zip(left, right) for value in pair]
        write_pcm(self.source, source, rate, 2, mask=3)
        start, end = onset - 441, onset + len(step) + 441

        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"bassoon-stereo-{marked}.hwa-events"
                marks = ("--disturbance-samples", f"{start}:{end}") if marked else ()
                result = self.isolate(*marks)
                self.assertEqual(result.returncode, 0, result.stderr)
                valid = self.cli("validate-event-bundle", self.output)
                self.assertEqual(valid.returncode, 0, valid.stderr)
                manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
                provider, = manifest["providers"]
                self.assertEqual((provider["name"], provider["version"]),
                                 ("hwa.bassoon-isolation", "bassoon-isolation-2"))
                self.assertEqual(provider["settings"], {"instrument": "bassoon",
                                 "disturbance_samples": [[start, end]] if marked else []})
                audio = {row["kind"]: row for row in manifest["audio"]}
                for row in audio.values():
                    fmt = row["format"]
                    self.assertEqual((fmt["sample_rate_hz"], fmt["channels"], fmt["channel_mask"],
                                      fmt["frames"]), (rate, 2, 3, frames))
                bassoon = audio["instrument-stem"]
                self.assertEqual((bassoon["name"], bassoon["relative_path"],
                                  bassoon["source_recording_id"]),
                                 ("bassoon", "audio/bassoon.wav", audio["source-recording"]["id"]))
                stem = read_float_pcm(self.output / bassoon["relative_path"])
                removed = read_float_pcm(self.output / audio["derived-audio"]["relative_path"])
                self.assertEqual((len(stem), len(removed)), (2 * frames, 2 * frames))
                self.assertLess(max(abs(a + b - sample / 32768)
                                    for a, b, sample in zip(stem, removed, source)), 1e-7)
                step_energy = sum((sample * 32768) ** 2 for sample in stem[::2])
                note_error = sum((sample * 32768 - clean) ** 2
                                 for sample, clean in zip(stem[1::2], right))
                self.assertLess(step_energy, 0.8 * sum(sample ** 2 for sample in step))
                self.assertLess(note_error, 0.01 * sum(sample ** 2 for sample in note))
                if marked:
                    self.assertFalse(any(removed[:2 * start]))
                    self.assertFalse(any(removed[2 * end:]))


class BassClarinetIsolation(InstrumentIsolationTest):
    instrument = "bass-clarinet"

    @staticmethod
    def note(rate, frames, pitch, amplitude, seed, vibrato_cents=0, upper_register=False):
        rng = random.Random(seed)
        levels = (1.0, 0.26, 0.47, 0.17, 0.23, 0.09, 0.10) if upper_register else (
            0.33, 0.025, 1.0, 0.06, 0.74, 0.045, 0.49, 0.035, 0.29, 0.025, 0.14)
        peak_pitch = pitch * 2 ** (abs(vibrato_cents) / 1200)
        partials = [(j, level) for j, level in enumerate(levels, 1) if j * peak_pitch < rate / 2]
        phase = previous_noise = 0.0
        samples = []
        for i in range(frames):
            time = i / rate
            cents = vibrato_cents * math.sin(math.tau * 5.1 * time)
            phase += math.tau * pitch * 2 ** (cents / 1200) / rate
            envelope = min(1.0, time / 0.007, (frames - i) / (rate * 0.014))
            tone = sum(level * math.sin(j * phase) for j, level in partials)
            noise = rng.uniform(-1.0, 1.0)
            air = (noise - 0.50 * previous_noise) * (0.045 + 0.33 * math.exp(-time / 0.010))
            previous_noise = noise
            samples.append(round(amplitude * envelope * (tone + air)))
        return samples

    def test_preserves_short_quiet_low_b_flat_and_upper_b(self):
        for rate in (8000, 22050):
            low_onset = rate // 5
            high_onset = round(0.8 * rate)
            low = self.note(rate, round(0.046 * rate), 58.2705, 140, 45)
            high = self.note(rate, round(0.11 * rate), 987.7666, 4100, 19, 20, upper_register=True)
            clean = [0] * (high_onset + len(high) + rate // 4)
            clean[low_onset:low_onset + len(low)] = low
            clean[high_onset:high_onset + len(high)] = high
            for marked in (False, True):
                with self.subTest(rate=rate, marked=marked):
                    self.output = self.root / f"bass-clarinet-range-{rate}-{marked}.hwa-events"
                    span = (rate // 8, high_onset + len(high) + rate // 20) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    for onset, note in ((low_onset, low), (high_onset, high)):
                        energy = sum(sample ** 2 for sample in note)
                        error = sum((sample - stem[i]) ** 2 for i, sample in enumerate(note, onset))
                        self.assertLess(error, 0.0025 * energy, onset)

    def test_preserves_a_slurred_twelfth(self):
        rate = 8000
        onset = rate // 5
        overlap = round(0.007 * rate)
        # A bass clarinet overblows from sounding B-flat1 to F3.
        low = self.note(rate, round(0.19 * rate), 58.2705, 3950, 32)
        upper = self.note(rate, round(0.16 * rate), 174.6141, 2900, 33, upper_register=True)
        phrase = low[:-overlap]
        phrase.extend(a + b for a, b in zip(low[-overlap:], upper[:overlap]))
        phrase.extend(upper[overlap:])
        clean = [0] * onset + phrase + [0] * onset
        boundary = onset + len(low) - overlap
        region = range(boundary - rate // 25, boundary + rate // 20)
        energy = sum(clean[i] ** 2 for i in region)
        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"bass-clarinet-register-{marked}.hwa-events"
                span = (region.start - rate // 50, region.stop + rate // 50) if marked else None
                stem = self.isolate_mono(clean, rate, span)
                error = sum((stem[i] - clean[i]) ** 2 for i in region)
                self.assertLess(error, 0.0025 * energy)

    def test_preserves_fast_repeated_twelfths(self):
        rate = 15754
        note_frames = round(0.064 * rate)
        pitches = (65.4064, 196.2192) * 4
        levels = (0.12, 0.025, 1.0, 0.035, 0.48, 0.03, 0.29, 0.04, 0.15, 0.02,
                  0.13, 0.016, 0.14, 0.012)
        phrase = self.slurred_phrase(rate, note_frames, pitches, levels)
        onset = rate // 5
        clean = [0] * onset + phrase + [0] * onset
        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"bass-clarinet-fast-twelfths-{marked}.hwa-events"
                span = (onset - rate // 50, onset + len(phrase) + rate // 50) if marked else None
                stem = self.isolate_mono(clean, rate, span)
                for i, pitch in enumerate(pitches):
                    start = onset + i * note_frames
                    frames = range(start, start + note_frames)
                    energy = sum(clean[j] ** 2 for j in frames)
                    error = sum((clean[j] - stem[j]) ** 2 for j in frames)
                    self.assertLessEqual(error, 0.0025 * energy, (i, pitch))

    def test_preserves_a_held_low_b_flat_as_its_fundamental_grows(self):
        for rate in (8000, 15754):
            clean = self.note_with_fundamental_swell(rate, 58.27047, 0.445)
            region = range(round(0.37 * rate), round(0.475 * rate))
            energy = sum(clean[i] ** 2 for i in region)
            for marked in (False, True):
                with self.subTest(rate=rate, marked=marked):
                    self.output = self.root / f"bass-clarinet-low-partial-{rate}-{marked}.hwa-events"
                    span = (round(0.30 * rate), round(0.545 * rate)) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    error = sum((stem[i] - clean[i]) ** 2 for i in region)
                    self.assertLessEqual(error, 0.0025 * energy)

    def test_reduces_a_recorded_cough_and_step_in_stereo(self):
        rate, frames = 22050, 71663
        onset = rate // 4
        step_onset = round(2.40 * rate)
        left, right = [0] * frames, [0] * frames
        held = self.note(rate, round(1.65 * rate), 196.0, 3900, 11, 17)
        quiet = self.note(rate, round(0.16 * rate), 58.2705, 280, 23)
        left[onset:onset + len(held)] = held
        right[step_onset:step_onset + len(quiet)] = quiet
        mixture = left.copy()
        spans = []
        for kind, start, gain in (("cough", round(0.92 * rate), 1), ("step", step_onset, 24)):
            noise = read_fixture(kind)
            for i, sample in enumerate(noise, start):
                mixture[i] += gain * sample
            spans.append((start, start + len(noise)))
        source = [value for pair in zip(mixture, right) for value in pair]
        reference = self.root / "reference.wav"
        write_pcm(reference, [value for pair in zip(left, right) for value in pair], rate, 2, mask=3)
        write_pcm(self.source, source, rate, 2, mask=3)
        bounds = [[start - 441, end + 441] for start, end in spans]
        flags = tuple(flag for start, end in bounds for flag in
                      ("--disturbance-samples", f"{start}:{end}"))

        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"bass-clarinet-stereo-{marked}.hwa-events"
                result = self.isolate(*(flags if marked else ()))
                self.assertEqual(result.returncode, 0, result.stderr)
                valid = self.cli("validate-event-bundle", self.output)
                self.assertEqual(valid.returncode, 0, valid.stderr)
                manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
                provider, = manifest["providers"]
                self.assertEqual((provider["name"], provider["version"]),
                                 ("hwa.bass-clarinet-isolation", "bass-clarinet-isolation-2"))
                self.assertEqual(provider["settings"], {"instrument": self.instrument,
                                 "disturbance_samples": bounds if marked else []})
                audio = {row["kind"]: row for row in manifest["audio"]}
                for row in audio.values():
                    fmt = row["format"]
                    self.assertEqual((fmt["sample_rate_hz"], fmt["channels"], fmt["channel_mask"],
                                      fmt["frames"]), (rate, 2, 3, frames))
                artifact = audio["instrument-stem"]
                self.assertEqual((artifact["name"], artifact["relative_path"],
                                  artifact["source_recording_id"]),
                                 (self.instrument, "audio/bass-clarinet.wav", audio["source-recording"]["id"]))
                stem_path = self.output / artifact["relative_path"]
                self.assertGreater(self.evaluate(reference, stem_path)["si_sdr_improvement_db"], 0.7)
                stem = read_float_pcm(stem_path)
                removed = read_float_pcm(self.output / audio["derived-audio"]["relative_path"])
                self.assertEqual((len(stem), len(removed)), (2 * frames, 2 * frames))
                self.assertLess(max(abs(a + b - sample / 32768)
                                    for a, b, sample in zip(stem, removed, source)), 1e-7)
                for start, end in spans:
                    source_error = sum((mixture[i] - left[i]) ** 2 for i in range(start, end))
                    stem_error = sum((stem[2 * i] * 32768 - left[i]) ** 2 for i in range(start, end))
                    self.assertLess(stem_error, 0.85 * source_error, start)
                quiet_error = sum((sample * 32768 - clean) ** 2
                                  for sample, clean in zip(stem[1::2], right))
                self.assertLess(quiet_error, 0.0025 * sum(sample ** 2 for sample in quiet))
                if marked:
                    self.assertFalse(any(removed[:2 * bounds[0][0]]))
                    self.assertFalse(any(removed[2 * bounds[0][1]:2 * bounds[1][0]]))
                    self.assertFalse(any(removed[2 * bounds[1][1]:]))


class ContrabassoonIsolation(InstrumentIsolationTest):
    instrument = "contrabassoon"

    @staticmethod
    def note(rate, frames, pitch, amplitude, seed, attack=0.055):
        rng = random.Random(seed)
        levels = (0.22, 0.45, 0.18, 0.75, 1.0, 0.83, 0.46, 0.19, 0.11, 0.05)
        partials = [(j, level) for j, level in enumerate(levels, 1) if j * pitch < rate / 2]
        phase = previous_noise = 0.0
        samples = []
        for i in range(frames):
            time = i / rate
            phase += math.tau * pitch / rate
            envelope = min(1.0, time / attack, (frames - i) / (rate * 0.035))
            tone = sum(level * math.sin(j * phase) for j, level in partials)
            noise = rng.uniform(-1.0, 1.0)
            air = (noise - 0.60 * previous_noise) * (0.045 + 0.30 * math.exp(-time / 0.038))
            previous_noise = noise
            samples.append(round(amplitude * envelope * (tone + air)))
        return samples

    def test_preserves_quiet_low_notes_and_upper_c(self):
        for rate, low_pitch in ((8000, 27.5), (8193, 29.1352), (22050, 27.5), (48000, 29.1352)):
            low_onset, high_onset = round(0.24 * rate), round(0.83 * rate)
            low = self.note(rate, round(0.18 * rate), low_pitch, 230, 23)
            high = self.note(rate, round(0.23 * rate), 261.6256, 3150, 28, attack=0.016)
            clean = [0] * (high_onset + len(high) + rate // 4)
            clean[low_onset:low_onset + len(low)] = low
            clean[high_onset:high_onset + len(high)] = high
            for marked in (False, True):
                with self.subTest(rate=rate, marked=marked):
                    self.output = self.root / f"contrabassoon-range-{rate}-{marked}.hwa-events"
                    span = (rate // 8, len(clean) - rate // 10) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    for onset, note in ((low_onset, low), (high_onset, high)):
                        error = sum((sample - stem[i]) ** 2 for i, sample in enumerate(note, onset))
                        self.assertLess(error, 0.0025 * sum(sample ** 2 for sample in note))
                    attack = range(low_onset, low_onset + round(0.07 * rate))
                    error = sum((clean[i] - stem[i]) ** 2 for i in attack)
                    self.assertLess(error, 0.0025 * sum(clean[i] ** 2 for i in attack))

    def test_preserves_a_short_low_a_after_a_louder_note(self):
        for rate in (15170, 48000):
            onset = round(0.70 * rate)
            prior = self.note(rate, round(0.23 * rate), 29.1352, 4200, 101)
            quiet = self.note(rate, round(0.135 * rate), 27.5, 230, 86, attack=0.022)
            later = self.note(rate, rate // 5, 130.8128, 5000, 211, attack=0.018)
            clean = [0] * round(0.35 * rate) + prior
            clean.extend([0] * (onset - len(clean)))
            clean.extend(quiet + [0] * round(0.35 * rate) + later + [0] * (rate // 4))
            for marked in (False, True):
                with self.subTest(rate=rate, marked=marked):
                    self.output = self.root / f"contrabassoon-short-low-{rate}-{marked}.hwa-events"
                    span = (round(0.30 * rate), len(clean) - rate // 10) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    error = sum((sample - stem[i]) ** 2 for i, sample in enumerate(quiet, onset))
                    self.assertLess(error, 0.0025 * sum(sample ** 2 for sample in quiet))

    def test_reduces_rumble_under_the_bottom_register(self):
        rate = 22050
        clean = self.note(rate, 2 * rate, 27.5, 2100, 58)
        start, end = rate // 2, 3 * rate // 2
        mixture = clean.copy()
        for i in range(start, end):
            envelope = math.sin(math.pi * (i - start) / (end - start)) ** 2
            mixture[i] += round(2400 * envelope * math.sin(math.tau * 13.0 * i / rate))

        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"contrabassoon-rumble-{marked}.hwa-events"
                stem = self.isolate_mono(mixture, rate, (start, end) if marked else None)
                region = range(start, end)
                original_error = sum((mixture[i] - clean[i]) ** 2 for i in region)
                cleaned_error = sum((stem[i] - clean[i]) ** 2 for i in region)
                self.assertLess(cleaned_error, 0.7 * original_error)
                # Measure the sounding A0 independently of the stronger overtones.
                phase = math.tau * 27.5 / rate
                reference = sum(clean[i] * math.sin(phase * (i + 1)) for i in region)
                retained = sum(stem[i] * math.sin(phase * (i + 1)) for i in region)
                self.assertLess(abs(retained - reference), 0.05 * abs(reference))

    def test_reduces_a_recorded_cough_and_step_in_stereo(self):
        rate, frames = 22050, 77201
        onset, step_onset = rate // 4, round(2.64 * rate)
        left, right = [0] * frames, [0] * frames
        held = self.note(rate, round(1.84 * rate), 98.0, 2900, 11)
        quiet = self.note(rate, round(0.22 * rate), 29.1352, 250, 23)
        left[onset:onset + len(held)] = held
        right[step_onset:step_onset + len(quiet)] = quiet
        mixture = left.copy()
        spans = []
        for kind, start, gain in (("cough", rate, 1), ("step", step_onset, 24)):
            noise = read_fixture(kind)
            for i, sample in enumerate(noise, start):
                mixture[i] += gain * sample
            spans.append((start, start + len(noise)))
        source = [value for pair in zip(mixture, right) for value in pair]
        write_pcm(self.source, source, rate, 2, mask=3)
        bounds = [[start - 441, end + 441] for start, end in spans]
        flags = tuple(flag for start, end in bounds for flag in
                      ("--disturbance-samples", f"{start}:{end}"))

        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"contrabassoon-stereo-{marked}.hwa-events"
                result = self.isolate(*(flags if marked else ()))
                self.assertEqual(result.returncode, 0, result.stderr)
                valid = self.cli("validate-event-bundle", self.output)
                self.assertEqual(valid.returncode, 0, valid.stderr)
                manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
                provider, = manifest["providers"]
                self.assertEqual((provider["name"], provider["version"]),
                                 ("hwa.contrabassoon-isolation", "contrabassoon-isolation-2"))
                self.assertEqual(provider["settings"], {"instrument": self.instrument,
                                 "disturbance_samples": bounds if marked else []})
                audio = {row["kind"]: row for row in manifest["audio"]}
                for row in audio.values():
                    fmt = row["format"]
                    self.assertEqual((fmt["sample_rate_hz"], fmt["channels"], fmt["channel_mask"],
                                      fmt["frames"]), (rate, 2, 3, frames))
                self.assertEqual(audio["instrument-stem"]["relative_path"], "audio/contrabassoon.wav")
                stem = read_float_pcm(self.output / audio["instrument-stem"]["relative_path"])
                removed = read_float_pcm(self.output / audio["derived-audio"]["relative_path"])
                self.assertEqual((len(stem), len(removed)), (2 * frames, 2 * frames))
                self.assertLess(max(abs(a + b - sample / 32768)
                                    for a, b, sample in zip(stem, removed, source)), 1e-7)
                for start, end in spans:
                    original_error = sum((mixture[i] - left[i]) ** 2 for i in range(start, end))
                    cleaned_error = sum((stem[2 * i] * 32768 - left[i]) ** 2 for i in range(start, end))
                    self.assertLess(cleaned_error, 0.85 * original_error)
                quiet_error = sum((sample * 32768 - clean) ** 2
                                  for sample, clean in zip(stem[1::2], right))
                self.assertLess(quiet_error, 0.0025 * sum(sample ** 2 for sample in quiet))
                if marked:
                    self.assertFalse(any(removed[:2 * bounds[0][0]]))
                    self.assertFalse(any(removed[2 * bounds[0][1]:2 * bounds[1][0]]))
                    self.assertFalse(any(removed[2 * bounds[1][1]:]))


class EFlatClarinetIsolation(InstrumentIsolationTest):
    instrument = "e-flat-clarinet"

    def test_preserves_quiet_g3_and_high_c7(self):
        for rate in (8000, 11025, 22050, 44100):
            low_onset, high_onset = round(0.22 * rate), round(0.74 * rate)
            low = BassClarinetIsolation.note(rate, round(0.042 * rate), 196.0, 200, 29)
            high = BassClarinetIsolation.note(rate, round(0.085 * rate), 2093.005, 4050, 31,
                                               vibrato_cents=15, upper_register=True)
            clean = [0] * (high_onset + len(high) + rate // 4)
            clean[low_onset:low_onset + len(low)] = low
            clean[high_onset:high_onset + len(high)] = high
            for marked in (False, True):
                with self.subTest(rate=rate, marked=marked):
                    self.output = self.root / f"e-flat-clarinet-range-{rate}-{marked}.hwa-events"
                    span = (rate // 8, len(clean) - rate // 8) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    for onset, note in ((low_onset, low), (high_onset, high)):
                        error = sum((sample - stem[i]) ** 2 for i, sample in enumerate(note, onset))
                        self.assertLess(error, 0.0025 * sum(sample ** 2 for sample in note), onset)

    def test_preserves_fast_upper_register_slurs(self):
        pitches = (1864.655, 1975.533, 2093.005, 1975.533) * 2
        for rate in (8000, 25601):
            dwell = round(0.05 * rate)
            levels = (1.0, 0.36, 0.29, 0.18, 0.13, 0.07, 0.03)
            levels = levels[:int((rate / 2) / max(pitches))]
            phrase = self.slurred_phrase(rate, dwell, pitches, levels)
            onset = rate // 5
            clean = [0] * onset + phrase + [0] * onset
            for marked in (False, True):
                with self.subTest(rate=rate, marked=marked):
                    self.output = self.root / f"e-flat-clarinet-high-slurs-{rate}-{marked}.hwa-events"
                    span = (onset - rate // 50, onset + len(phrase) + rate // 50) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    for i in range(len(pitches)):
                        region = range(onset + i * dwell, onset + (i + 1) * dwell)
                        error = sum((clean[j] - stem[j]) ** 2 for j in region)
                        self.assertLess(error, 0.001 * sum(clean[j] ** 2 for j in region), i)

    def test_preserves_repeated_twelfths(self):
        rate = 22050
        dwell = round(0.054 * rate)
        pitches = (196.0, 587.3295) * 4
        levels = (0.12, 0.03, 1.0, 0.07, 0.54, 0.05, 0.21, 0.03, 0.10)
        phrase = self.slurred_phrase(rate, dwell, pitches, levels)
        onset = rate // 5
        clean = [0] * onset + phrase + [0] * onset
        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"e-flat-clarinet-twelfths-{marked}.hwa-events"
                span = (onset - rate // 50, onset + len(phrase) + rate // 50) if marked else None
                stem = self.isolate_mono(clean, rate, span)
                for i in range(len(pitches)):
                    region = range(onset + i * dwell, onset + (i + 1) * dwell)
                    error = sum((clean[j] - stem[j]) ** 2 for j in region)
                    self.assertLess(error, 0.0025 * sum(clean[j] ** 2 for j in region), i)

    def test_reduces_a_recorded_cough_and_step_in_stereo(self):
        rate, frames = 22050, 72601
        onset, step_onset = rate // 4, round(2.43 * rate)
        left, right = [0] * frames, [0] * frames
        held = BassClarinetIsolation.note(rate, round(1.72 * rate), 392.0, 4150, 11, 17)
        quiet = BassClarinetIsolation.note(rate, round(0.14 * rate), 196.0, 300, 23)
        left[onset:onset + len(held)] = held
        right[step_onset:step_onset + len(quiet)] = quiet
        mixture = left.copy()
        spans = []
        for kind, start, gain in (("cough", rate, 1), ("step", step_onset, 22)):
            noise = read_fixture(kind)
            for i, sample in enumerate(noise, start):
                mixture[i] += gain * sample
            spans.append((start, start + len(noise)))
        source = [value for pair in zip(mixture, right) for value in pair]
        write_pcm(self.source, source, rate, 2, mask=3)
        bounds = [[start - 441, end + 441] for start, end in spans]
        flags = tuple(flag for start, end in bounds for flag in
                      ("--disturbance-samples", f"{start}:{end}"))

        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"e-flat-clarinet-stereo-{marked}.hwa-events"
                result = self.isolate(*(flags if marked else ()))
                self.assertEqual(result.returncode, 0, result.stderr)
                valid = self.cli("validate-event-bundle", self.output)
                self.assertEqual(valid.returncode, 0, valid.stderr)
                manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
                provider, = manifest["providers"]
                self.assertEqual((provider["name"], provider["version"]),
                                 ("hwa.e-flat-clarinet-isolation", "e-flat-clarinet-isolation-2"))
                self.assertEqual(provider["settings"], {"instrument": self.instrument,
                                 "disturbance_samples": bounds if marked else []})
                audio = {row["kind"]: row for row in manifest["audio"]}
                for row in audio.values():
                    fmt = row["format"]
                    self.assertEqual((fmt["sample_rate_hz"], fmt["channels"], fmt["channel_mask"],
                                      fmt["frames"]), (rate, 2, 3, frames))
                self.assertEqual(audio["instrument-stem"]["relative_path"], "audio/e-flat-clarinet.wav")
                stem = read_float_pcm(self.output / audio["instrument-stem"]["relative_path"])
                removed = read_float_pcm(self.output / audio["derived-audio"]["relative_path"])
                self.assertEqual((len(stem), len(removed)), (2 * frames, 2 * frames))
                self.assertLess(max(abs(a + b - sample / 32768)
                                    for a, b, sample in zip(stem, removed, source)), 1e-7)
                for start, end in spans:
                    original_error = sum((mixture[i] - left[i]) ** 2 for i in range(start, end))
                    cleaned_error = sum((stem[2 * i] * 32768 - left[i]) ** 2 for i in range(start, end))
                    self.assertLess(cleaned_error, 0.85 * original_error)
                quiet_error = sum((sample * 32768 - clean) ** 2
                                  for sample, clean in zip(stem[1::2], right))
                self.assertLess(quiet_error, 0.0025 * sum(sample ** 2 for sample in quiet))
                if marked:
                    self.assertFalse(any(removed[:2 * bounds[0][0]]))
                    self.assertFalse(any(removed[2 * bounds[0][1]:2 * bounds[1][0]]))
                    self.assertFalse(any(removed[2 * bounds[1][1]:]))


class AltoFluteIsolation(InstrumentIsolationTest):
    instrument = "alto-flute"

    def test_preserves_quiet_low_notes_and_vibrato_on_high_g(self):
        for rate, low_pitch in ((8000, 195.9977), (12801, 184.9972), (22050, 195.9977)):
            low_start, high_start = rate // 5, round(0.70 * rate)
            low = FluteIsolation.note(rate, round(0.054 * rate), low_pitch, 235, 38, breath=0.20)
            high = FluteIsolation.note(rate, round(0.14 * rate), 1567.982, 3800, 91, vibrato_cents=26)
            clean = [0] * (high_start + len(high) + rate // 5)
            clean[low_start:low_start + len(low)] = low
            clean[high_start:high_start + len(high)] = high
            for marked in (False, True):
                with self.subTest(rate=rate, low_pitch=low_pitch, marked=marked):
                    self.output = self.root / f"alto-flute-range-{rate}-{marked}.hwa-events"
                    span = (rate // 8, high_start + len(high) + rate // 20) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    for onset, note in ((low_start, low), (high_start, high)):
                        energy = sum(sample ** 2 for sample in note)
                        error = sum((sample - stem[i]) ** 2 for i, sample in enumerate(note, onset))
                        self.assertLess(error, 0.0025 * energy, onset)

    def test_preserves_a_slow_breathy_low_g(self):
        levels = (1.0, 0.42, 0.19, 0.038, 0.019)
        for rate in (8000, 12801, 22050):
            note = self.slow_breathy_note(rate, 195.9977, levels, pitch_dip=0.025)
            onset = rate // 5
            clean = [0] * onset + note + [0] * onset
            lead_end = onset + round(0.10 * rate)
            release_start = onset + round(0.32 * rate)
            for marked in (False, True):
                with self.subTest(rate=rate, marked=marked):
                    self.output = self.root / f"alto-flute-breath-{rate}-{marked}.hwa-events"
                    span = (onset - rate // 50, onset + len(note) + rate // 50) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    for start, end in ((onset, lead_end), (lead_end, release_start),
                                       (release_start, onset + len(note))):
                        energy = sum(clean[i] ** 2 for i in range(start, end))
                        error = sum((stem[i] - clean[i]) ** 2 for i in range(start, end))
                        self.assertLess(error, 0.0025 * energy, start)

    def test_preserves_a_fast_slurred_phrase_across_registers(self):
        pitches = (195.9977, 293.6648, 391.9954, 587.3295, 783.9909, 880.0, 987.7666, 1046.5023)
        levels = (1.0, 0.31, 0.18, 0.08, 0.05, 0.03, 0.012)
        for rate in (12801, 22050):
            onset = rate // 5
            note_frames = round(0.038 * rate)
            phrase = self.slurred_phrase(rate, note_frames, pitches, levels)
            clean = [0] * onset + phrase + [0] * onset
            for marked in (False, True):
                with self.subTest(rate=rate, marked=marked):
                    self.output = self.root / f"alto-flute-slur-{rate}-{marked}.hwa-events"
                    span = (onset - rate // 50, onset + len(phrase) + rate // 50) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    for note, pitch in enumerate(pitches):
                        start = onset + note * note_frames
                        frames = range(start, start + note_frames)
                        energy = sum(clean[i] ** 2 for i in frames)
                        error = sum((stem[i] - clean[i]) ** 2 for i in frames)
                        self.assertLess(error, 0.0025 * energy, pitch)

    def test_reduces_recorded_cough_and_step_without_cutting_the_other_channel(self):
        rate, frames = 22050, 71663
        left, right = [0] * frames, [0] * frames
        onset, footfall = rate // 4, round(2.40 * rate)
        held = FluteIsolation.note(rate, round(1.65 * rate), 293.6648, 5100, 21, vibrato_cents=18)
        quiet = FluteIsolation.note(rate, round(0.16 * rate), 195.9977, 430, 39, breath=0.12)
        left[onset:onset + len(held)] = held
        right[footfall:footfall + len(quiet)] = quiet
        mixture = left.copy()
        spans = []
        for name, start, gain in (("cough", round(0.92 * rate), 1), ("step", footfall, 24)):
            noise = read_fixture(name)
            for i, sample in enumerate(noise, start):
                mixture[i] += gain * sample
            spans.append((name, start, start + len(noise)))
        source = [value for pair in zip(mixture, right) for value in pair]
        reference = self.root / "reference.wav"
        write_pcm(reference, [value for pair in zip(left, right) for value in pair], rate, 2, mask=3)
        write_pcm(self.source, source, rate, 2, mask=3)
        bounds = [[start - 441, end + 441] for _, start, end in spans]
        flags = tuple(arg for start, end in bounds for arg in
                      ("--disturbance-samples", f"{start}:{end}"))

        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"alto-flute-stereo-{marked}.hwa-events"
                result = self.isolate(*(flags if marked else ()))
                self.assertEqual(result.returncode, 0, result.stderr)
                valid = self.cli("validate-event-bundle", self.output)
                self.assertEqual(valid.returncode, 0, valid.stderr)
                manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
                provider, = manifest["providers"]
                self.assertEqual((provider["name"], provider["version"]),
                                 ("hwa.alto-flute-isolation", "alto-flute-isolation-2"))
                self.assertEqual(provider["settings"], {"instrument": self.instrument,
                                 "disturbance_samples": bounds if marked else []})
                audio = {row["kind"]: row for row in manifest["audio"]}
                for row in audio.values():
                    fmt = row["format"]
                    self.assertEqual((fmt["sample_rate_hz"], fmt["channels"], fmt["channel_mask"],
                                      fmt["frames"]), (rate, 2, 3, frames))
                artifact = audio["instrument-stem"]
                self.assertEqual((artifact["name"], artifact["relative_path"],
                                  artifact["source_recording_id"]),
                                 (self.instrument, "audio/alto-flute.wav", audio["source-recording"]["id"]))
                stem_path = self.output / artifact["relative_path"]
                self.assertGreater(self.evaluate(reference, stem_path)["si_sdr_improvement_db"], 0.7)
                stem = read_float_pcm(stem_path)
                removed = read_float_pcm(self.output / audio["derived-audio"]["relative_path"])
                self.assertEqual((len(stem), len(removed)), (len(source), len(source)))
                self.assertLess(max(abs(a + b - original / 32768)
                                    for a, b, original in zip(stem, removed, source)), 1e-7)
                for name, start, end in spans:
                    original_error = sum((mixture[i] - left[i]) ** 2 for i in range(start, end))
                    stem_error = sum((stem[2 * i] * 32768 - left[i]) ** 2 for i in range(start, end))
                    self.assertLess(stem_error, 0.85 * original_error, name)
                quiet_error = sum((sample * 32768 - clean) ** 2
                                  for sample, clean in zip(stem[1::2], right))
                self.assertLess(quiet_error, 0.0025 * sum(sample ** 2 for sample in quiet))
                if marked:
                    self.assertFalse(any(removed[:2 * bounds[0][0]]))
                    self.assertFalse(any(removed[2 * bounds[0][1]:2 * bounds[1][0]]))
                    self.assertFalse(any(removed[2 * bounds[1][1]:]))


class AltoSaxophoneIsolation(InstrumentIsolationTest):
    instrument = "alto-saxophone"

    @classmethod
    def note(cls, rate, frames, pitch, amplitude, seed, vibrato_cents=0):
        levels = (0.72, 1.0, 0.69, 0.48, 0.28, 0.16, 0.09, 0.06)
        return cls.saxophone_note(rate, frames, pitch, amplitude, seed, levels, vibrato_cents)

    def test_preserves_air_in_a_quiet_low_d_flat_and_upper_a(self):
        for rate in (8000, 22050):
            low_start, high_start = rate // 5, round(0.75 * rate)
            low = self.note(rate, round(0.056 * rate), 138.5913, 225, 32)
            high = self.note(rate, round(0.13 * rate), 880.0, 4000, 18, vibrato_cents=26)
            clean = [0] * (high_start + len(high) + rate // 5)
            clean[low_start:low_start + len(low)] = low
            clean[high_start:high_start + len(high)] = high
            for marked in (False, True):
                with self.subTest(rate=rate, marked=marked):
                    self.output = self.root / f"alto-saxophone-range-{rate}-{marked}.hwa-events"
                    span = (rate // 8, high_start + len(high) + rate // 20) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    for onset, note in ((low_start, low), (high_start, high)):
                        energy = sum(sample ** 2 for sample in note)
                        error = sum((sample - stem[i]) ** 2 for i, sample in enumerate(note, onset))
                        self.assertLess(error, 0.0025 * energy, onset)

    def test_preserves_a_slow_breathy_low_d_flat(self):
        rate = 22050
        levels = (0.45, 1.0, 0.52, 0.58, 0.41, 0.23, 0.15, 0.09, 0.06)
        note = self.slow_breathy_note(rate, 138.5913, levels, pitch_dip=0.133333)
        onset = rate // 5
        clean = [0] * onset + note + [0] * onset
        lead_end = onset + round(0.10 * rate)
        release_start = onset + round(0.32 * rate)
        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"alto-saxophone-breath-{marked}.hwa-events"
                span = (onset - rate // 50, onset + len(note) + rate // 50) if marked else None
                stem = self.isolate_mono(clean, rate, span)
                for start, end in ((onset, lead_end), (lead_end, release_start),
                                   (release_start, onset + len(note))):
                    energy = sum(clean[i] ** 2 for i in range(start, end))
                    error = sum((stem[i] - clean[i]) ** 2 for i in range(start, end))
                    self.assertLess(error, 0.0025 * energy, start)

    def test_preserves_quick_slurs_and_octave_leaps(self):
        rate = 12801
        groups = (
            (0.048, (138.5913, 277.1826) * 4, (0.18, 1.0, 0.57, 0.58, 0.35, 0.28, 0.21, 0.15, 0.08)),
            (0.038, (138.5913, 185.0, 261.626, 349.228, 523.251, 698.456, 783.99, 880.0),
             (1.0, 0.74, 0.47, 0.32, 0.23, 0.13, 0.092)),
        )
        for duration, pitches, levels in groups:
            onset = rate // 5
            note_frames = round(duration * rate)
            phrase = self.slurred_phrase(rate, note_frames, pitches, levels)
            clean = [0] * onset + phrase + [0] * onset
            for marked in (False, True):
                with self.subTest(duration=duration, marked=marked):
                    self.output = self.root / f"alto-saxophone-slur-{note_frames}-{marked}.hwa-events"
                    span = (onset - rate // 50, onset + len(phrase) + rate // 50) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    for note, pitch in enumerate(pitches):
                        start = onset + note * note_frames
                        frames = range(start, start + note_frames)
                        energy = sum(clean[i] ** 2 for i in frames)
                        error = sum((stem[i] - clean[i]) ** 2 for i in frames)
                        self.assertLess(error, 0.0025 * energy, pitch)

    def test_preserves_a_held_low_d_flat_as_its_fundamental_grows(self):
        rate = 22050
        levels = (0.08, 1.0, 0.045, 0.24, 0.04)
        clean = self.note_with_fundamental_swell(rate, 138.5913, 0.595, levels, peak=0.25)
        region = range(round(0.37 * rate), round(0.625 * rate))
        energy = sum(clean[i] ** 2 for i in region)
        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"alto-saxophone-low-partial-{marked}.hwa-events"
                span = (round(0.30 * rate), round(0.695 * rate)) if marked else None
                stem = self.isolate_mono(clean, rate, span)
                error = sum((stem[i] - clean[i]) ** 2 for i in region)
                self.assertLess(error, 0.0025 * energy)

    def test_reduces_recorded_cough_and_step_without_cutting_the_other_channel(self):
        rate = 22050
        held = self.note(rate, round(1.65 * rate), 293.6648, 3900, 21, vibrato_cents=17)
        quiet = self.note(rate, round(0.16 * rate), 138.5913, 360, 39)
        self.assert_recorded_stereo_cleanup(held, quiet)


class TenorSaxophoneIsolation(InstrumentIsolationTest):
    instrument = "tenor-saxophone"

    @classmethod
    def note(cls, rate, frames, pitch, amplitude, seed, vibrato_cents=0):
        levels = (0.86, 1.0, 0.74, 0.43, 0.30, 0.18, 0.15, 0.07, 0.04)
        return cls.saxophone_note(rate, frames, pitch, amplitude, seed, levels, vibrato_cents)

    def test_preserves_a_quiet_low_a_flat_and_vibrato_on_upper_e(self):
        for rate in (8000, 22050):
            low_start, high_start = rate // 5, round(0.76 * rate)
            low = self.note(rate, round(0.067 * rate), 103.8262, 235, 43)
            high = self.note(rate, round(0.14 * rate), 659.2551, 3720, 18, vibrato_cents=27)
            clean = [0] * (high_start + len(high) + rate // 5)
            clean[low_start:low_start + len(low)] = low
            clean[high_start:high_start + len(high)] = high
            for marked in (False, True):
                with self.subTest(rate=rate, marked=marked):
                    self.output = self.root / f"tenor-saxophone-range-{rate}-{marked}.hwa-events"
                    span = (rate // 8, high_start + len(high) + rate // 20) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    for onset, note in ((low_start, low), (high_start, high)):
                        energy = sum(sample ** 2 for sample in note)
                        error = sum((sample - stem[i]) ** 2 for i, sample in enumerate(note, onset))
                        self.assertLess(error, 0.0025 * energy, onset)

    def test_preserves_the_breath_in_a_quiet_low_a_flat(self):
        rate = 13653
        levels = (0.45, 1.0, 0.52, 0.58, 0.41, 0.23, 0.15, 0.09, 0.06)
        note = self.slow_breathy_note(rate, 103.8262, levels, pitch_dip=0.133333)
        onset = rate // 5
        clean = [0] * onset + note + [0] * onset
        lead_end = onset + round(0.10 * rate)
        release_start = onset + round(0.32 * rate)
        for marked in (False, True):
            with self.subTest(marked=marked):
                self.output = self.root / f"tenor-saxophone-breath-{marked}.hwa-events"
                span = (onset - rate // 50, onset + len(note) + rate // 50) if marked else None
                stem = self.isolate_mono(clean, rate, span)
                for start, end in ((onset, lead_end), (lead_end, release_start),
                                   (release_start, onset + len(note))):
                    energy = sum(clean[i] ** 2 for i in range(start, end))
                    error = sum((stem[i] - clean[i]) ** 2 for i in range(start, end))
                    self.assertLess(error, 0.0025 * energy, start)

    def test_preserves_quick_slurs_and_octave_leaps(self):
        low_levels = (0.18, 1.0, 0.57, 0.58, 0.35, 0.28, 0.21, 0.15, 0.08)
        high_levels = (1.0, 0.74, 0.47, 0.32, 0.23, 0.13, 0.092)
        run = (103.8262, 138.5913, 195.9977, 261.6256, 391.9954, 523.2511, 587.3295, 659.2551)
        groups = (
            (13653, 0.038, run, high_levels),
            (13654, 0.048, (103.8262, 207.6524) * 4, low_levels),
            (12801, 0.050, (329.6276, 659.2551) * 4, high_levels),
        )
        for rate, duration, pitches, levels in groups:
            onset = rate // 5
            note_frames = round(duration * rate)
            phrase = self.slurred_phrase(rate, note_frames, pitches, levels)
            clean = [0] * onset + phrase + [0] * onset
            for marked in (False, True):
                with self.subTest(rate=rate, marked=marked):
                    self.output = self.root / f"tenor-saxophone-slur-{rate}-{marked}.hwa-events"
                    span = (onset - rate // 50, onset + len(phrase) + rate // 50) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    for note, pitch in enumerate(pitches):
                        start = onset + note * note_frames
                        frames = range(start, start + note_frames)
                        energy = sum(clean[i] ** 2 for i in frames)
                        error = sum((stem[i] - clean[i]) ** 2 for i in frames)
                        self.assertLess(error, 0.0025 * energy, pitch)

    def test_preserves_a_weak_fundamental_in_a_held_low_a_flat(self):
        for rate, bend in ((13653, 0), (48000, -60)):
            rng = random.Random(206)
            phase = 0.0
            clean = []
            for i in range(round(0.98 * rate)):
                time = i / rate
                envelope = min(1.0, time / 0.03, (0.98 - time) / 0.045)
                swell = max(0.0, min(1.0, (time - 0.38) / 0.014, (0.53 - time) / 0.018))
                pitch = 103.8262 * 2 ** (bend * swell / 1200)
                phase += math.tau * pitch / rate
                levels = (0.025 + (0.29 - 0.025) * swell, 1.0, 0.04, 0.25, 0.037, 0.13, 0.021)
                tone = sum(level * math.sin(j * phase) for j, level in enumerate(levels, 1))
                clean.append(round(3200 * envelope * (tone + 0.03 * rng.uniform(-1.0, 1.0))))

            region = range(round(0.32 * rate), round(0.59 * rate))
            energy = sum(clean[i] ** 2 for i in region)
            for marked in (False, True):
                with self.subTest(rate=rate, bend=bend, marked=marked):
                    self.output = self.root / f"tenor-saxophone-fundamental-{rate}-{marked}.hwa-events"
                    span = (round(0.29 * rate), round(0.67 * rate)) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    error = sum((stem[i] - clean[i]) ** 2 for i in region)
                    self.assertLess(error, 0.0025 * energy)

    def test_reduces_recorded_cough_and_step_without_cutting_the_other_channel(self):
        rate = 22050
        held = self.note(rate, round(1.65 * rate), 220.0, 3750, 62, vibrato_cents=16)
        quiet = self.note(rate, round(0.16 * rate), 103.8262, 365, 17)
        self.assert_recorded_stereo_cleanup(held, quiet)


class RecorderIsolationTest(InstrumentIsolationTest):
    def assert_preserves_recorder_range(self, low_pitch, high_pitch):
        for rate in (8000, 22050):
            low_start, high_start = rate // 5, round(0.68 * rate)
            low = FluteIsolation.note(rate, round(0.067 * rate), low_pitch, 230, 95, breath=0.23)
            high = FluteIsolation.note(rate, round(0.093 * rate), high_pitch, 3800, 17, breath=0.09)
            clean = [0] * (high_start + len(high) + rate // 5)
            clean[low_start:low_start + len(low)] = low
            clean[high_start:high_start + len(high)] = high
            for marked in (False, True):
                with self.subTest(rate=rate, marked=marked):
                    self.output = self.root / f"recorder-range-{rate}-{marked}.hwa-events"
                    span = (rate // 8, high_start + len(high) + rate // 20) if marked else None
                    stem = self.isolate_mono(clean, rate, span)
                    for onset, note in ((low_start, low), (high_start, high)):
                        energy = sum(sample ** 2 for sample in note)
                        error = sum((sample - stem[i]) ** 2 for i, sample in enumerate(note, onset))
                        self.assertLess(error, 0.0025 * energy, onset)

    def assert_preserves_recorder_slur(self, rate, lower, upper, duration, levels):
        pitches = (lower, upper) * 4
        note_frames = round(duration * rate)
        phrase = self.slurred_phrase(rate, note_frames, pitches, levels)
        onset = rate // 5
        clean = [0] * onset + phrase + [0] * onset
        for marked in (False, True):
            with self.subTest(rate=rate, marked=marked):
                self.output = self.root / f"recorder-slur-{rate}-{marked}.hwa-events"
                span = (onset - rate // 50, onset + len(phrase) + rate // 50) if marked else None
                stem = self.isolate_mono(clean, rate, span)
                for i, pitch in enumerate(pitches):
                    start = onset + i * note_frames
                    frames = range(start, start + note_frames)
                    energy = sum(clean[j] ** 2 for j in frames)
                    error = sum((stem[j] - clean[j]) ** 2 for j in frames)
                    self.assertLess(error, 0.0025 * energy, (i, pitch))

    def assert_recorder_stereo_cleanup(self, held_pitch, quiet_pitch):
        rate = 22050
        held = FluteIsolation.note(rate, round(1.65 * rate), held_pitch, 4100, 62, breath=0.05)
        quiet = FluteIsolation.note(rate, round(0.16 * rate), quiet_pitch, 360, 17, breath=0.18)
        self.assert_recorded_stereo_cleanup(held, quiet)


class RecorderIsolation(RecorderIsolationTest):
    instrument = "recorder"

    def test_preserves_a_quiet_low_c_and_upper_d(self):
        self.assert_preserves_recorder_range(523.2511, 2349.318)

    def test_preserves_a_fast_c_d_trill_and_low_octave_slurs(self):
        self.assert_preserves_recorder_slur(8000, 2093.005, 2349.318, 0.042, (1.0, 0.32, 0.08, 0.04))
        self.assert_preserves_recorder_slur(11025, 523.2511, 1046.5022, 0.035,
                                            (1.0, 0.48, 0.39, 0.18, 0.085, 0.035))

    def test_reduces_recorded_cough_and_step_without_cutting_the_other_channel(self):
        self.assert_recorder_stereo_cleanup(1046.502, 523.2511)


class SopraninoRecorderIsolation(RecorderIsolationTest):
    instrument = "sopranino-recorder"

    def test_preserves_a_quiet_low_f_and_upper_g(self):
        self.assert_preserves_recorder_range(698.4565, 3135.963)

    def test_preserves_a_fast_f_g_trill_and_high_octave_slurs(self):
        self.assert_preserves_recorder_slur(8000, 2793.826, 3135.963, 0.042, (1.0, 0.32, 0.08, 0.04))
        self.assert_preserves_recorder_slur(20481, 1567.982, 3135.964, 0.037, (1.0, 0.07, 0.026))

    def test_reduces_recorded_cough_and_step_without_cutting_the_other_channel(self):
        self.assert_recorder_stereo_cleanup(1760.0, 698.4565)


class WindMarkedIsolation(InstrumentIsolationTest):
    def assert_preserves_phrase_notes(self, rate, phrase, notes, max_error):
        onset = rate // 5
        clean = [0] * onset + phrase + [0] * onset
        for marked in (False, True):
            with self.subTest(instrument=self.instrument, rate=rate, marked=marked):
                self.output = self.root / f"{self.instrument}-phrase-{rate}-{marked}.hwa-events"
                span = (onset - rate // 50, onset + len(phrase) + rate // 50) if marked else None
                stem = self.isolate_mono(clean, rate, span)
                for first, last in notes:
                    frames = range(onset + first, onset + last)
                    energy = sum(clean[i] ** 2 for i in frames)
                    error = sum((stem[i] - clean[i]) ** 2 for i in frames)
                    self.assertLess(error, max_error ** 2 * energy, first)

    def test_preserves_short_detached_low_tenor_sax_notes(self):
        self.instrument = "tenor-saxophone"
        rate = 48000
        dwell, gap = round(0.040 * rate), round(0.032 * rate)
        levels = (0.45, 1.0, 0.52, 0.58, 0.38, 0.25, 0.11)
        pitches = (103.8262, 207.6524, 103.8262, 207.6524, 103.8262, 155.7393, 103.8262, 103.8262)
        phase = 0.0
        phrase, notes = [], []
        for pitch in pitches:
            notes.append((len(phrase), len(phrase) + dwell))
            for i in range(dwell):
                phase += math.tau * pitch / rate
                envelope = min(1.0, (i + 1) / (0.007 * rate), (dwell - i) / (0.012 * rate))
                tone = sum(level * math.sin(harmonic * phase) for harmonic, level in enumerate(levels, 1))
                phrase.append(round(3000 * envelope * tone))
            phrase.extend([0] * gap)
        self.assert_preserves_phrase_notes(rate, phrase, notes, max_error=0.06)

    def test_preserves_short_bassoon_and_contrabassoon_slurs(self):
        rate = 16000
        cases = (
            ("bassoon", 0.042, (58.2705, 116.541) * 4,
             (0.18, 0.72, 1.0, 0.58, 0.36, 0.25, 0.18, 0.10, 0.055)),
            ("contrabassoon", 0.051, (261.6256, 233.0819, 207.6524, 233.0819) * 2,
             (0.22, 0.45, 0.18, 0.75, 1.0, 0.83, 0.46, 0.19, 0.11)),
        )
        for instrument, duration, pitches, levels in cases:
            self.instrument = instrument
            dwell = round(duration * rate)
            phrase = self.slurred_phrase(rate, dwell, pitches, levels)
            notes = [(i * dwell, (i + 1) * dwell) for i in range(len(pitches))]
            self.assert_preserves_phrase_notes(rate, phrase, notes, max_error=0.10)

    def test_preserves_fast_upper_octave_slurs(self):
        levels = (1.0, 0.37, 0.16, 0.08, 0.035)
        for instrument, rate, pitch in (("e-flat-clarinet", 22050, 1046.5025),
                                        ("alto-flute", 44100, 783.991),
                                        ("alto-flute", 10241, 783.991)):
            self.instrument = instrument
            dwell = round(0.047 * rate)
            pitches = (pitch, 2 * pitch) * 4
            phrase = self.slurred_phrase(rate, dwell, pitches, levels)
            notes = [(i * dwell, (i + 1) * dwell) for i in range(len(pitches))]
            self.assert_preserves_phrase_notes(rate, phrase, notes, max_error=0.05)

    def test_preserves_breath_when_a_held_note_softens(self):
        self.instrument = "flute"
        rate = 22050
        beginning, ending, length = 0.60, 0.795, 1.20
        pitch = 880.0
        levels = (1.0, 0.37, 0.16, 0.08, 0.035)
        rng = random.Random(88)
        phase = 0.0
        clean = []
        for i in range(round(length * rate)):
            time = i / rate
            envelope = min(1.0, time / 0.01, (length - time) / 0.02)
            rise = min(1.0, max(0.0, (time - beginning) / 0.008))
            fall = min(1.0, max(0.0, (ending - time) / 0.008))
            tone_level = 1.0 - 0.8 * rise * fall
            phase += math.tau * pitch / rate
            harmonic = sum(level * math.sin(j * phase) for j, level in enumerate(levels, 1))
            breath = rng.uniform(-0.09, 0.09)
            clean.append(round(4800 * envelope * (tone_level * harmonic + breath)))

        region = range(round((beginning - 0.02) * rate), round((ending + 0.02) * rate))
        energy = sum(clean[i] ** 2 for i in region)
        for marked in (False, True):
            with self.subTest(marked=marked):
                mark = (round(0.50 * rate), round(0.895 * rate)) if marked else None
                self.output = self.root / f"flute-soft-hold-{marked}.hwa-events"
                stem = self.isolate_mono(clean, rate, mark)
                error = sum((stem[i] - clean[i]) ** 2 for i in region)
                self.assertLess(error, 0.0001 * energy)

    def test_cleans_a_held_note_at_either_clip_edge(self):
        rate = 22050
        frames = round(1.5 * rate)
        onset = round(0.65 * rate)
        rng = random.Random(48)
        raw = [rng.uniform(-1.0, 1.0) for _ in range(round(0.12 * rate) + 1)]
        noise = [round(2800 * min(1.0, i / 100, (len(raw) - i) / 100) * (b - a))
                 for i, (a, b) in enumerate(zip(raw, raw[1:]))]
        end = onset + len(noise)
        profiles = (("flute", 440.0, FluteIsolation.note),
                    ("oboe", 392.0, OboeIsolation.note),
                    ("english-horn", 220.0, OboeIsolation.note),
                    ("clarinet", 392.0, ClarinetIsolation.note),
                    ("piccolo", 1174.66, FluteIsolation.note),
                    ("bassoon", 164.81, BassoonIsolation.note),
                    ("bass-clarinet", 174.61, BassClarinetIsolation.note),
                    ("contrabassoon", 98.0, ContrabassoonIsolation.note),
                    ("e-flat-clarinet", 392.0, BassClarinetIsolation.note),
                    ("alto-flute", 293.66, FluteIsolation.note),
                    ("alto-saxophone", 293.66, AltoSaxophoneIsolation.note),
                    ("tenor-saxophone", 220.0, TenorSaxophoneIsolation.note),
                    ("recorder", 783.99, FluteIsolation.note),
                    ("sopranino-recorder", 1174.66, FluteIsolation.note))
        for instrument, pitch, note in profiles:
            self.instrument = instrument
            reference = note(rate, frames, pitch, 6500, 432)
            mixture = reference.copy()
            for i, disturbance in enumerate(noise, onset):
                mixture[i] += disturbance

            for crop_first, crop_end in ((onset - 100, frames), (0, end - 600)):
                with self.subTest(instrument=instrument, crop_first=crop_first, crop_end=crop_end):
                    mark = (onset - 100 - crop_first, min(end + 100, crop_end) - crop_first)
                    self.output = self.root / f"{instrument}-{crop_first}-{crop_end}.hwa-events"
                    clipped = mixture[crop_first:crop_end]
                    stem = self.isolate_mono(clipped, rate, mark)
                    active = range(onset, min(end, crop_end))
                    noise_error = sum((mixture[i] - reference[i]) ** 2 for i in active)
                    stem_error = sum((stem[i - crop_first] - reference[i]) ** 2 for i in active)
                    self.assertLess(stem_error, 0.97 * noise_error)
                    self.assertEqual(stem[:mark[0]], clipped[:mark[0]])
                    self.assertEqual(stem[mark[1]:], clipped[mark[1]:])

    def test_preserves_an_airy_attack_or_release_at_clip_edge(self):
        rate = 22050
        mark_length = round(0.22 * rate)
        profiles = (("flute", 440.0, FluteIsolation.note),
                    ("oboe", 392.0, OboeIsolation.note),
                    ("english-horn", 220.0, OboeIsolation.note),
                    ("piccolo", 1174.66, FluteIsolation.note),
                    ("bassoon", 164.81, BassoonIsolation.note),
                    ("bass-clarinet", 174.61, BassClarinetIsolation.note),
                    ("alto-flute", 293.66, FluteIsolation.note),
                    ("alto-saxophone", 293.66, AltoSaxophoneIsolation.note),
                    ("tenor-saxophone", 220.0, TenorSaxophoneIsolation.note),
                    ("recorder", 783.99, FluteIsolation.note),
                    ("sopranino-recorder", 1174.66, FluteIsolation.note))
        for instrument, pitch, note in profiles:
            self.instrument = instrument
            phrase = note(rate, rate, pitch, 6500, 432)
            rng = random.Random(818)
            previous = 0.0
            for i in range(round(0.15 * rate)):
                value = rng.uniform(-1.0, 1.0)
                envelope = math.exp(-i / (0.035 * rate)) * min(1.0, i / 100)
                phrase[i] += round(1600 * (value - previous) * envelope)
                previous = value
            phrase = [0] * round(0.04 * rate) + phrase

            for at_end in (False, True):
                with self.subTest(instrument=instrument, at_end=at_end):
                    clean = phrase[::-1] if at_end else phrase
                    mark = (len(clean) - mark_length, len(clean)) if at_end else (0, mark_length)
                    self.output = self.root / f"{instrument}-air-{at_end}.hwa-events"
                    self.assertEqual(self.isolate_mono(clean, rate, mark), clean)

    def test_preserves_a_legato_semitone(self):
        rate = 8000
        beginning, ending, length = 0.60, 0.665, 1.20
        upper = 2 ** (1 / 12)
        for instrument, pitch in (("flute", 880.0), ("oboe", 392.0), ("english-horn", 196.0)):
            self.instrument = instrument
            flute = instrument == "flute"
            levels = (1.0, 0.37, 0.16, 0.08, 0.035) if flute else (
                0.58, 1.0, 0.83, 0.56, 0.35, 0.26, 0.14, 0.08)
            partials = [(j, level) for j, level in enumerate(levels, 1)
                        if j * pitch * upper < rate / 2]
            breath = 0.09 if flute else 0.03
            rng = random.Random(88)
            phase = 0.0
            clean = []
            for i in range(round(length * rate)):
                time = i / rate
                envelope = min(1.0, time / 0.01, (length - time) / 0.02)
                rise = min(1.0, max(0.0, (time - beginning) / 0.008))
                fall = min(1.0, max(0.0, (ending - time) / 0.008))
                phase += math.tau * pitch * 2 ** (rise * fall / 12) / rate
                harmonic = sum(level * math.sin(j * phase) for j, level in partials)
                clean.append(round(4800 * envelope * (harmonic + rng.uniform(-breath, breath))))

            region = range(round((beginning - 0.02) * rate), round((ending + 0.02) * rate))
            energy = sum(clean[i] ** 2 for i in region)
            for marked in (False, True):
                with self.subTest(instrument=instrument, marked=marked):
                    mark = (round(0.50 * rate), round(0.765 * rate)) if marked else None
                    self.output = self.root / f"{instrument}-semitone-{marked}.hwa-events"
                    stem = self.isolate_mono(clean, rate, mark)
                    error = sum((stem[i] - clean[i]) ** 2 for i in region)
                    self.assertLess(error, 0.0001 * energy)


if __name__ == "__main__":
    unittest.main(argv=[__file__, *REST])

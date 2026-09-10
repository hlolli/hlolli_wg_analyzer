import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import wave


def midi_notes(path):
    data = path.read_bytes()
    if data[:4] != b"MThd":
        raise AssertionError("missing MIDI header")
    division = int.from_bytes(data[12:14], "big")
    if division & 0x8000:
        raise AssertionError("unexpected SMPTE clock")
    offset = 8 + int.from_bytes(data[4:8], "big")
    result = []
    tempos = []
    while offset < len(data):
        if data[offset:offset+4] != b"MTrk":
            raise AssertionError("missing MIDI track")
        length = int.from_bytes(data[offset+4:offset+8], "big")
        track = data[offset+8:offset+8+length]
        offset += 8 + length
        pos = tick = 0
        running = None
        active = {}
        notes = []
        name = ""

        def variable():
            nonlocal pos
            value = 0
            for _ in range(4):
                byte = track[pos]
                pos += 1
                value = (value << 7) | (byte & 127)
                if not byte & 128:
                    return value
            raise AssertionError("invalid MIDI variable integer")

        while pos < len(track):
            tick += variable()
            status = track[pos]
            if status & 128:
                pos += 1
            elif running is not None:
                status = running
            else:
                raise AssertionError("missing running status")
            if status == 255:
                kind = track[pos]
                pos += 1
                size = variable()
                payload = track[pos:pos+size]
                pos += size
                if kind == 3:
                    name = payload.decode("utf-8")
                elif kind == 81:
                    tempos.append((tick, int.from_bytes(payload, "big")))
                continue
            if status in (240, 247):
                size = variable()
                pos += size
                running = None
                continue
            running = status
            size = 1 if status & 240 in (192, 208) else 2
            payload = track[pos:pos+size]
            pos += size
            key = (status & 15, payload[0])
            if status & 240 == 144 and payload[1]:
                if key in active:
                    raise AssertionError("unexpected same-channel duplicate note")
                active[key] = tick
            elif status & 240 == 128 or (status & 240 == 144 and not payload[1]):
                start = active.pop(key)
                notes.append((payload[0], start / division, tick / division))
        if active:
            raise AssertionError("hanging MIDI notes")
        if notes:
            result.append((name, sorted(notes)))
    return result, tempos


class EventScoreTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bundle = self.root / "quartet.hwa-events"
        subprocess.run([str(FIXTURE), str(self.bundle)], check=True, capture_output=True)

    def command(self, *args, ok=True):
        result = subprocess.run([str(ANALYZER), "export-event-score", str(self.bundle), *map(str, args)],
                                capture_output=True, text=True)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
        return result

    def edit_bundle(self, edit):
        manifest = json.loads((self.bundle / "manifest.json").read_text())
        events = [json.loads(line) for line in (self.bundle / "events.jsonl").read_text().splitlines() if line]
        edit(manifest, events)
        content = "".join(json.dumps(event, separators=(",", ":")) + "\n" for event in events).encode()
        (self.bundle / "events.jsonl").write_bytes(content)
        manifest["counts"]["audio"] = len(manifest["audio"])
        manifest["counts"]["events"] = len(events)
        manifest["counts"]["values"] = sum(len(event["values"]) for event in events)
        entry = next(item for item in manifest["files"] if item["relative_path"] == "events.jsonl")
        entry.update(file_bytes=len(content), sha256=hashlib.sha256(content).hexdigest())
        (self.bundle / "manifest.json").write_text(json.dumps(manifest))

    def test_csound_tracks_exact_values_and_no_input_changes(self):
        before = {path.name: path.read_bytes() for path in self.bundle.iterdir()}
        text = self.command("--format", "csound", "--output", "-").stdout
        notes = [line.split() for line in text.splitlines() if line.startswith("i ")]
        self.assertEqual(len(notes), 5)
        self.assertEqual({int(row[1]) for row in notes}, {1, 2, 3, 4})
        events = [json.loads(line) for line in before["events.jsonl"].decode().splitlines() if line]
        for row in notes:
            event = next(event for event in events if event["id"] == int(row[6]))
            self.assertEqual(float(row[2]), event["start_sample"] / 8000)
            self.assertEqual(float(row[3]), (event["end_sample"] - event["start_sample"]) / 8000)
            self.assertEqual(float(row[4]), event["values"][0]["value"])
            self.assertEqual([int(row[7]), int(row[8])], [event["start_sample"], event["end_sample"]])
            self.assertEqual(row[1], row[9])
        self.assertIn("omitted_unpitched_notes=1", text)
        self.assertEqual(before, {path.name: path.read_bytes() for path in self.bundle.iterdir()})
        output = self.root / "score.sco"
        self.command("--format", "csound", "--source-id", "1", "--output", output)
        self.assertEqual(output.read_text(), text)
        self.command("--format", "csound", "--output", output, ok=False)
        self.assertEqual(output.read_text(), text)
        self.command("--format", "csound", "--replace", "--output", output, ok=False)
        self.assertEqual(output.read_text(), text)

    def test_options_validation_and_cleanup(self):
        output = self.root / "score"
        for flags in [[], ["--format", "unknown"], ["--format", "lilypond"],
                      ["--format", "csound", "--tempo-bpm", "120"],
                      ["--format", "lilypond", "--tempo-bpm", "9"],
                      ["--format", "lilypond", "--tempo-bpm", "120.5"],
                      ["--format", "csound", "--format", "csound"],
                      ["--format", "csound", "--source-id", "0"],
                      ["--format", "csound", "--source-id", "99"],
                      ["--format", "csound", "--max-partials", "1"],
                      ["--format", "csound", "--json"]]:
            with self.subTest(flags=flags):
                self.command(*flags, "--output", output, ok=False)
                self.assertFalse(output.exists())
        result = subprocess.run([str(ANALYZER), "validate-event-bundle", str(self.bundle),
                                 "--format", "csound"], capture_output=True)
        self.assertEqual(result.returncode, 2)
        self.edit_bundle(lambda manifest, events: events[0]["values"][0].update(unit="wrong"))
        result = self.command("--format", "csound", "--output", output, ok=False)
        self.assertIn("selected pitch", result.stderr)
        self.assertFalse(output.exists())
        result = self.command("--format", "csound", "--output", "-", ok=False)
        self.assertEqual(result.stdout, "")

    def test_inference_scope_does_not_claim_orchestral_separation(self):
        result = subprocess.run([str(ANALYZER), "--json", "inference-capabilities"],
                                capture_output=True, text=True, check=True)
        report = json.loads(result.stdout)
        self.assertEqual(report["instrument_stem_task"]["stem_labels"],
                         ["drums", "bass", "other", "vocals", "guitar", "piano"])
        self.assertEqual(report["scope"], {
            "native_execution_provider": "CPUExecutionProvider",
            "caller_supplied_models": True,
            "models_checked_by_this_command": False,
            "browser_inference": False,
            "webnn_inference": False,
            "orchestral_part_separation": False,
            "dry_instrument_recovery": False,
        })

    def test_multiple_sources_and_stable_track_order(self):
        original = self.command("--format", "csound", "--output", "-").stdout

        def change(manifest, events):
            second = copy.deepcopy(manifest["audio"][0])
            second["id"] = 2
            manifest["audio"].append(second)
            events.reverse()

        self.edit_bundle(change)
        result = self.command("--format", "csound", "--output", "-", ok=False)
        self.assertIn("--source-id", result.stderr)
        self.assertEqual(self.command("--format", "csound", "--source-id", "1", "--output", "-").stdout, original)

    def test_lilypond_timing_rounding_and_label_safety(self):
        def change(manifest, events):
            events[0].update(start_sample=62, end_sample=63, part='violin"\n#(error "not code")\\')
        self.edit_bundle(change)
        text = self.command("--format", "lilypond", "--tempo-bpm", "120", "--output", "-").stdout
        self.assertIn("start_sample=62 end_sample=63 pitch_hz=440 start_tick=0 end_tick=1", text)
        self.assertIn("a'128", text)
        self.assertNotIn('#(error "not code")', text)
        self.assertIn("part_utf8_hex=", text)
        self.assertNotIn("\\time ", text)

    def test_lilypond_compiles_and_midi_matches_all_voices(self):
        if LILYPOND is None:
            self.skipTest("LilyPond not configured")
        score = self.root / "quartet.ly"
        self.command("--format", "lilypond", "--tempo-bpm", "120", "--output", score)
        env = dict(os.environ)
        env["PATH"] = str(LILYPOND.parent) + os.pathsep + env.get("PATH", "")
        env["XDG_CACHE_HOME"] = str(self.root / "cache")
        result = subprocess.run([str(LILYPOND), "-dno-point-and-click", "-o", str(self.root / "quartet"), str(score)],
                                capture_output=True, text=True, timeout=60, env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("warning:", result.stderr)
        self.assertTrue((self.root / "quartet.pdf").is_file())
        tracks, tempos = midi_notes(self.root / "quartet.midi")
        self.assertTrue(tempos)
        self.assertTrue(all(tempo == 500000 for _, tempo in tempos))
        expected = [[(48, 0, 4)], [(60, 0, 4)], [(69, 0, 2), (72, 1, 3)], [(76, 1, 3)]]
        self.assertEqual(sorted(notes for _, notes in tracks), sorted(expected))

    def test_csound_renders_tracks_with_pitch_and_overlap(self):
        if CSOUND is None:
            self.skipTest("Csound not configured")
        score = self.root / "quartet.sco"
        self.command("--format", "csound", "--output", score)
        orchestra = self.root / "quartet.orc"
        orchestra.write_text("sr=8000\nksmps=1\nnchnls=4\n0dbfs=1\ninstr 1,2,3,4\na1 oscili p5,p4\noutch p1,a1\nendin\n")
        output = self.root / "quartet.wav"
        result = subprocess.run([str(CSOUND), "-d", "-m0", "--sample-accurate", "-W", "-s", "-o", str(output),
                                 str(orchestra), str(score)], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        with wave.open(str(output), "rb") as wav:
            self.assertEqual((wav.getnchannels(), wav.getframerate(), wav.getnframes()), (4, 8000, 24000))
            samples = struct.unpack("<" + "h" * (wav.getnframes() * 4), wav.readframes(wav.getnframes()))
        notes = [line.split() for line in score.read_text().splitlines() if line.startswith("i ")]
        for channel in range(4):
            rows = [row for row in notes if int(row[1]) == channel + 1]
            for start, end in [(0, 4000), (4000, 8000), (8000, 12000), (12000, 16000), (16000, 24000)]:
                active = [row for row in rows if int(row[7]) <= start and int(row[8]) >= end]
                segment = [samples[index * 4 + channel] / 32768 for index in range(start, end)]
                if not active:
                    self.assertLessEqual(max(map(abs, segment)), 1 / 32768)
                for row in active:
                    freq = float(row[4])
                    real = sum(value * math.cos(2 * math.pi * freq * index / 8000) for index, value in enumerate(segment))
                    imag = sum(value * math.sin(2 * math.pi * freq * index / 8000) for index, value in enumerate(segment))
                    amplitude = 2 * math.hypot(real, imag) / len(segment)
                    self.assertAlmostEqual(amplitude, 0.2, delta=0.006)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--analyzer", required=True, type=Path)
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--lilypond", type=Path)
    parser.add_argument("--csound", type=Path)
    args, rest = parser.parse_known_args()
    ANALYZER, FIXTURE = args.analyzer.resolve(), args.fixture.resolve()
    LILYPOND, CSOUND = args.lilypond, args.csound
    unittest.main(argv=[__file__, *rest])

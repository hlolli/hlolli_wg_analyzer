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
import xml.etree.ElementTree as ET


def midi_notes(path, details=None):
    data = path.read_bytes()
    if len(data) < 14 or data[:8] != b"MThd\0\0\0\x06":
        raise AssertionError("missing MIDI header")
    division = int.from_bytes(data[12:14], "big")
    if not division or division & 0x8000:
        raise AssertionError("unexpected SMPTE clock")
    offset = 8 + int.from_bytes(data[4:8], "big")
    result = []
    tempos = []
    track_count = 0
    while offset < len(data):
        track_count += 1
        if data[offset:offset+4] != b"MTrk":
            raise AssertionError("missing MIDI track")
        length = int.from_bytes(data[offset+4:offset+8], "big")
        track = data[offset+8:offset+8+length]
        if len(track) != length:
            raise AssertionError("truncated MIDI track")
        offset += 8 + length
        pos = tick = 0
        running = None
        active = {}
        notes = []
        name = ""
        channels = set()
        ended = False

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
                if len(payload) != size:
                    raise AssertionError("truncated MIDI metadata")
                if kind == 3:
                    name = payload.decode("utf-8")
                elif kind == 81:
                    if size != 3:
                        raise AssertionError("invalid MIDI tempo")
                    tempos.append((tick, int.from_bytes(payload, "big")))
                elif kind == 47:
                    if size != 0 or pos != len(track):
                        raise AssertionError("invalid MIDI track end")
                    ended = True
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
            if len(payload) != size or any(byte > 127 for byte in payload):
                raise AssertionError("invalid MIDI channel data")
            key = (status & 15, payload[0])
            if status & 240 == 144 and payload[1]:
                channels.add(status & 15)
                if key in active:
                    raise AssertionError("unexpected same-channel duplicate note")
                active[key] = tick
            elif status & 240 == 128 or (status & 240 == 144 and not payload[1]):
                start = active.pop(key)
                notes.append((payload[0], start / division, tick / division))
        if active or not ended:
            raise AssertionError("hanging MIDI notes or missing track end")
        if details is not None:
            details.append((channels, tick))
        if notes:
            result.append((name, sorted(notes)))
    if track_count != int.from_bytes(data[10:12], "big"):
        raise AssertionError("wrong MIDI track count")
    return result, tempos


def musicxml_notes(text):
    root = ET.fromstring(text)
    if root.tag != "score-partwise" or root.get("version") != "4.0":
        raise AssertionError("unexpected MusicXML root")
    names = {part.get("id"): part.findtext("part-name")
             for part in root.findall("part-list/score-part")}
    result = {}
    ends = []
    semitones = dict(C=0, D=2, E=4, F=5, G=7, A=9, B=11)
    for part in root.findall("part"):
        measures = part.findall("measure")
        if len(measures) != 1 or measures[0].find("attributes/time/senza-misura") is None:
            raise AssertionError("expected a single unmetered measure")
        cursor = 0
        lane_ends = {}
        for item in measures[0]:
            if item.tag == "backup":
                ticks = int(item.findtext("duration"))
                if ticks <= 0 or cursor != ticks:
                    raise AssertionError("backup does not return to measure start")
                cursor -= ticks
            elif item.tag == "note":
                ticks = int(item.findtext("duration"))
                voice = int(item.findtext("voice"))
                if ticks <= 0 or cursor != lane_ends.get(voice, 0):
                    raise AssertionError("nonpositive duration or broken voice sequence")
                pitch = item.find("pitch")
                if pitch is not None:
                    key = (12*(int(pitch.findtext("octave"))+1) +
                           semitones[pitch.findtext("step")] + int(pitch.findtext("alter", "0")))
                    event_id = item.get("id")
                    if event_id in result:
                        raise AssertionError("duplicate event ID")
                    result[event_id] = (names[part.get("id")], voice, key, cursor, cursor+ticks)
                cursor += ticks
                lane_ends[voice] = cursor
        if not lane_ends or set(lane_ends.values()) != {cursor}:
            raise AssertionError("voices have different end times")
        ends.append(cursor)
    if len(names) != len(ends) or len(set(ends)) != 1:
        raise AssertionError("parts have different end times")
    return root, result, ends[0]


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

    def midi_command(self, output, ok=True):
        result = subprocess.run(
            [str(ANALYZER), "export-event-score", str(self.bundle),
             "--format", "midi", "--output", str(output)], capture_output=True)
        self.assertEqual(result.returncode == 0, ok, result.stderr)
        return result

    def test_direct_midi_tracks_clock_and_input_preservation(self):
        before = {path.name: path.read_bytes() for path in self.bundle.iterdir()}
        output = self.root / "direct.mid"
        self.midi_command(output)
        data = output.read_bytes()
        self.assertEqual(data[:14], b"MThd\0\0\0\x06\0\x01\0\x05\x7f\xff")
        tracks, tempos = midi_notes(output)
        self.assertEqual(tempos, [(0, 500000)])
        self.assertEqual(tracks, [
            ("cello / 1", [(48, 0, 4)]),
            ("viola / 1", [(60, 0, 4)]),
            ("violin-1 / 1", [(69, 0, 2), (72, 1, 3)]),
            ("violin-2 / 1", [(76, 1, 3)]),
        ])
        self.assertEqual(self.midi_command("-").stdout, data)
        self.midi_command(output, ok=False)
        self.assertEqual(output.read_bytes(), data)
        self.assertEqual(before, {path.name: path.read_bytes() for path in self.bundle.iterdir()})
        self.edit_bundle(lambda manifest, events: events.reverse())
        self.assertEqual(self.midi_command("-").stdout, data)

    def test_direct_midi_rounding_labels_and_retrigger(self):
        def change(manifest, events):
            events[0].update(start_sample=1, end_sample=2, part='声\\"\n')
            events[4]["values"][0]["value"] = 440.0
            events[4].update(start_sample=2, end_sample=3, part='声\\"\n')
        self.edit_bundle(change)
        output = self.root / "small.mid"
        self.midi_command(output)
        tracks, _ = midi_notes(output)
        notes = dict(tracks)['声\\"\n / 1']
        self.assertEqual(notes, [(69, 8/32767, 16/32767), (69, 16/32767, 25/32767)])

    def test_direct_midi_rejects_ambiguous_overlap_without_partial_output(self):
        def change(manifest, events):
            events[4]["values"][0]["value"] = 440.0
        self.edit_bundle(change)
        output = self.root / "bad.mid"
        result = self.midi_command(output, ok=False)
        self.assertIn(b"same-key overlap", result.stderr)
        self.assertFalse(output.exists())
        self.assertEqual(self.midi_command("-", ok=False).stdout, b"")

    def test_direct_midi_channel_limit(self):
        def change(manifest, events):
            template = copy.deepcopy(events[0])
            events.clear()
            for index in range(16):
                event = copy.deepcopy(template)
                event.update(id=index+1, part="part-%02d" % index)
                events.append(event)
        self.edit_bundle(change)
        output = self.root / "too-many.mid"
        result = self.midi_command(output, ok=False)
        self.assertIn(b"15-track", result.stderr)
        self.assertFalse(output.exists())
        self.assertEqual(self.midi_command("-", ok=False).stdout, b"")
        self.edit_bundle(lambda manifest, events: events.pop())
        self.midi_command(output)
        details = []
        self.assertEqual(len(midi_notes(output, details)[0]), 15)
        self.assertEqual(details[0], (set(), 0))
        self.assertEqual(details[1:], [({channel}, 3*65534)
                                      for channel in range(16) if channel != 9])

    def test_direct_midi_sample_clocks_and_minimum_duration(self):
        for rate in (44100, 48000, 768000):
            with self.subTest(rate=rate):
                def change(manifest, events):
                    manifest["audio"][0]["format"].update(
                        sample_rate_hz=rate, frames=rate*3,
                        data_bytes=rate*6, duration_seconds=3)
                    del events[1:]
                    events[0].update(start_sample=1, end_sample=2)
                self.edit_bundle(change)
                output = self.root / (str(rate) + ".mid")
                self.midi_command(output)
                start = (65534 + rate//2)//rate
                end = max(start+1, (2*65534 + rate//2)//rate)
                self.assertEqual(midi_notes(output)[0], [
                    ("violin-1 / 1", [(69, start/32767, end/32767)])])

    def test_direct_midi_delta_limit_and_pitch_range(self):
        def change(manifest, events):
            manifest["audio"][0]["format"].update(
                frames=8000*4000, data_bytes=16000*4000, duration_seconds=4000)
            del events[1:]
            events[0].update(start_sample=8000*3999, end_sample=8000*4000)
        self.edit_bundle(change)
        output = self.root / "long.mid"
        self.midi_command(output)
        self.assertEqual(midi_notes(output)[0], [
            ("violin-1 / 1", [(69, 7998, 8000)])])

        def excessive_gap(manifest, events):
            manifest["audio"][0]["format"].update(
                frames=8000*10000, data_bytes=16000*10000, duration_seconds=10000)
        self.edit_bundle(excessive_gap)
        bad = self.root / "bad.mid"
        self.assertIn(b"delta/chunk", self.midi_command(bad, ok=False).stderr)
        self.assertFalse(bad.exists())
        self.assertEqual(self.midi_command("-", ok=False).stdout, b"")
        self.edit_bundle(lambda manifest, events: events[0]["values"][0].update(value=1))
        self.assertIn(b"pitch or time range", self.midi_command(bad, ok=False).stderr)
        self.assertFalse(bad.exists())
        self.assertEqual(self.midi_command("-", ok=False).stdout, b"")

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

    def test_musicxml_tracks_timing_and_exact_source_comments(self):
        before = {path.name: path.read_bytes() for path in self.bundle.iterdir()}
        text = self.command("--format", "musicxml", "--tempo-bpm", "120", "--output", "-").stdout
        root, notes, end = musicxml_notes(text)
        self.assertEqual(notes, {
            "E1": ("violin-1 / 1", 1, 69, 0, 64),
            "E2": ("violin-2 / 1", 1, 76, 32, 96),
            "E3": ("viola / 1", 1, 60, 0, 128),
            "E4": ("cello / 1", 1, 48, 0, 128),
            "E5": ("violin-1 / 1", 2, 72, 32, 96),
        })
        self.assertEqual(end, 192)
        self.assertEqual([node.text for node in root.findall(".//divisions")], ["32"]*4)
        self.assertEqual([node.get("tempo") for node in root.findall(".//sound")], ["120"]*4)
        for tag in ("key", "beats", "beat-type", "midi-instrument", "articulations", "ornaments"):
            self.assertEqual(root.findall(".//"+tag), [])
        self.assertIn("event_id=1 start_sample=0 end_sample=8000 pitch_hz=440", text)
        self.assertIn("omitted_unpitched_notes=1", text)
        output = self.root / "score.musicxml"
        self.command("--format", "musicxml", "--tempo-bpm", "120", "--source-id", "1", "--output", output)
        self.assertEqual(output.read_text(), text)
        self.command("--format", "musicxml", "--tempo-bpm", "120", "--output", output, ok=False)
        self.assertEqual(output.read_text(), text)
        self.assertEqual(before, {path.name: path.read_bytes() for path in self.bundle.iterdir()})
        self.edit_bundle(lambda manifest, events: events.reverse())
        self.assertEqual(self.command("--format", "musicxml", "--tempo-bpm", "120", "--output", "-").stdout, text)

    def test_musicxml_labels_rounding_and_same_pitch_overlap(self):
        label = "声<&\"'>--\r\n\t🎻"
        def change(manifest, events):
            events[0].update(start_sample=1, end_sample=2, part=label)
            events[4].update(start_sample=1, end_sample=3, part=label)
            events[4]["values"][0]["value"] = 440.0
        self.edit_bundle(change)
        text = self.command("--format", "musicxml", "--tempo-bpm", "120", "--output", "-").stdout
        _, notes, _ = musicxml_notes(text)
        self.assertEqual(notes["E1"], (label+" / 1", 1, 69, 0, 1))
        self.assertEqual(notes["E5"], (label+" / 1", 2, 69, 0, 1))
        self.assertIn("&lt;&amp;&quot;&apos;&gt;--&#13;", text)

    def test_musicxml_xml_characters_and_pitch_limits(self):
        output = self.root / "bad.musicxml"
        for label in ("bad\x01", "bad\ufffe", "bad\uffff"):
            self.edit_bundle(lambda manifest, events: events[0].update(part=label))
            result = self.command("--format", "musicxml", "--tempo-bpm", "120", "--output", output, ok=False)
            self.assertIn("forbidden XML character", result.stderr)
            self.assertFalse(output.exists())
            self.assertEqual(self.command("--format", "musicxml", "--tempo-bpm", "120", "--output", "-", ok=False).stdout, "")
        def change(manifest, events):
            events[0].update(part="low")
            events[0]["values"][0]["value"] = 8.175798915643707
        self.edit_bundle(change)
        result = self.command("--format", "musicxml", "--tempo-bpm", "120", "--output", output, ok=False)
        self.assertIn("pitch or time range", result.stderr)
        self.assertFalse(output.exists())
        self.edit_bundle(lambda manifest, events: events[0]["values"][0].update(value=16.351597831287414))
        text = self.command("--format", "musicxml", "--tempo-bpm", "120", "--output", "-").stdout
        self.assertEqual(musicxml_notes(text)[1]["E1"][2], 12)

    def test_musicxml_rate_tempo_and_large_event_ids(self):
        for rate, tempo in ((44100, 10), (48000, 137), (768000, 1000)):
            with self.subTest(rate=rate, tempo=tempo):
                def change(manifest, events):
                    manifest["audio"][0]["format"].update(
                        sample_rate_hz=rate, frames=3*rate, data_bytes=6*rate, duration_seconds=3)
                    del events[1:]
                    events[0].update(id=9007199254740991, start_sample=rate//3, end_sample=rate+1)
                self.edit_bundle(change)
                text = self.command("--format", "musicxml", "--tempo-bpm", str(tempo), "--output", "-").stdout
                _, notes, end = musicxml_notes(text)
                divisor = rate*60
                start = ((rate//3)*tempo*32 + divisor//2)//divisor
                stop = ((rate+1)*tempo*32 + divisor//2)//divisor
                self.assertEqual(notes["E9007199254740991"][3:], (start, stop))
                self.assertEqual(end, (3*rate*tempo*32 + divisor//2)//divisor)

    def test_options_validation_and_cleanup(self):
        output = self.root / "score"
        for flags in [[], ["--format", "unknown"], ["--format", "lilypond"],
                      ["--format", "musicxml"],
                      ["--format", "musicxml", "--tempo-bpm", "0"],
                      ["--format", "musicxml", "--tempo-bpm", "1001"],
                      ["--format", "csound", "--tempo-bpm", "120"],
                      ["--format", "midi", "--tempo-bpm", "120"],
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

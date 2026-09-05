#!/usr/bin/env python3

from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock
import wave


ROOT = Path(__file__).resolve().parents[1]
VALIDATOR = (ROOT / "adapters" / "hlolli_wg_viola" /
             "recording_session.py")
MODULE_SPEC = importlib.util.spec_from_file_location(
    "viola_recording_session", VALIDATOR,
)
assert MODULE_SPEC is not None and MODULE_SPEC.loader is not None
MODULE = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(MODULE)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_wave(path: Path, *, channels: int = 2, frames: int = 1000,
               rate: int = 96000, width: int = 3) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(channels)
        stream.setsampwidth(width)
        stream.setframerate(rate)
        sample = (1).to_bytes(width, "little", signed=True)
        stream.writeframes(sample * channels * frames)


def valid_manifest(source: Path) -> dict:
    return {
        "schema": "hwa-viola-recording-session",
        "schema_version": 1,
        "session_id": "session-01",
        "source_family": "controlled-viola-01",
        "split": "check",
        "recording_setup": {
            "instrument_id": "viola-01",
            "bow_id": "bow-01",
            "performer_id": "performer-01",
            "room_id": "room-01",
            "recorder_id": "recorder-01",
            "audio_interface_id": "interface-01",
            "clock_source": "interface-internal",
        },
        "tuning": {
            "a4_hz": 440.0,
            "reference": {
                "pitch": "A4",
                "frequency_hz": 440.0,
                "measurement_method": "strobe-tuner-01",
            },
        },
        "strings": [
            {"id": "c", "open_pitch": "C3", "length_m": 0.4,
             "measurement_method": "steel-rule-01"},
            {"id": "g", "open_pitch": "G3", "length_m": 0.4,
             "measurement_method": "steel-rule-01"},
            {"id": "d", "open_pitch": "D4", "length_m": 0.4,
             "measurement_method": "steel-rule-01"},
            {"id": "a", "open_pitch": "A4", "length_m": 0.4,
             "measurement_method": "steel-rule-01"},
        ],
        "channels": [
            {"id": "bridge-mic", "role": "instrument",
             "transducer_id": "mic-01", "input_id": "input-01"},
            {"id": "room-mic", "role": "room",
             "transducer_id": "mic-02", "input_id": "input-02"},
        ],
        "source_files": [{
            "id": "source-01",
            "path": "audio/source.wav",
            "sha256": sha256(source),
            "layout": {
                "sample_rate_hz": 96000,
                "bits_per_sample": 24,
                "frame_count": 1000,
                "channel_ids": ["bridge-mic", "room-mic"],
            },
        }],
        "takes": [
            {
                "id": "pizz-c-01",
                "kind": "passive-pizzicato",
                "file_id": "source-01",
                "start_frame": 0,
                "frame_count": 100,
                "string_id": "c",
                "pitch": "C3",
            },
            {
                "id": "arco-g-01",
                "kind": "steady-arco",
                "file_id": "source-01",
                "start_frame": 100,
                "frame_count": 100,
                "string_id": "g",
                "pitch": "G3",
                "articulation": "sustain",
                "bow": {
                    "force_n": 1.0,
                    "speed_m_per_s": 0.2,
                    "bridge_distance_m": 0.04,
                    "position_ratio": 0.1,
                    "direction": "down-bow",
                    "measurement_methods": {
                        "force": "load-cell-01",
                        "speed": "motion-track-01",
                        "bridge_distance": "steel-rule-01",
                    },
                },
            },
            {
                "id": "room-01", "kind": "room-tone",
                "file_id": "source-01", "start_frame": 200,
                "frame_count": 100,
            },
            {
                "id": "sync-01", "kind": "sync",
                "file_id": "source-01", "start_frame": 300,
                "frame_count": 10,
            },
        ],
        "processing": [],
    }


class ViolaRecordingSessionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(
            prefix="hwa viola recording session ",
        )
        self.root = Path(self.temporary.name)
        self.source = self.root / "audio" / "source.wav"
        write_wave(self.source)
        self.manifest = self.root / "manifest.json"
        self.value = valid_manifest(self.source)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_manifest(self, value: dict | None = None) -> Path:
        if value is None:
            value = self.value
        self.manifest.write_text(json.dumps(value), encoding="utf-8")
        return self.manifest

    def test_file_snapshot_does_not_mix_path_and_handle_timestamps(self) -> None:
        common = {
            "st_mode": 0o100600,
            "st_dev": 1,
            "st_ino": 2,
            "st_size": 3,
        }
        opened = types.SimpleNamespace(
            **common, st_mtime_ns=10, st_ctime_ns=20,
        )
        current = types.SimpleNamespace(
            **common, st_mtime_ns=10, st_ctime_ns=20,
        )
        named_before = types.SimpleNamespace(
            **common, st_mtime_ns=11, st_ctime_ns=21,
        )
        named_after = types.SimpleNamespace(
            **common, st_mtime_ns=11, st_ctime_ns=21,
        )
        with mock.patch.object(MODULE.os, "lstat", return_value=named_after):
            self.assertTrue(MODULE._same_open_file(
                self.manifest, named_before, opened, current,
            ))

        changed = types.SimpleNamespace(
            **common, st_mtime_ns=12, st_ctime_ns=21,
        )
        with mock.patch.object(MODULE.os, "lstat", return_value=changed):
            self.assertFalse(MODULE._same_open_file(
                self.manifest, named_before, opened, current,
            ))

    def assert_rejected(self, value: dict, message: str | None = None) -> None:
        self.write_manifest(value)
        context = (self.assertRaisesRegex(MODULE.ContractError, message)
                   if message is not None
                   else self.assertRaises(MODULE.ContractError))
        with context:
            MODULE.validate_manifest(self.manifest)

    def test_valid_fixture_cli_is_path_free_deterministic_and_read_only(self) -> None:
        self.write_manifest()
        source_before = self.source.read_bytes()
        manifest_before = self.manifest.read_bytes()
        files_before = sorted(
            path.relative_to(self.root).as_posix()
            for path in self.root.rglob("*") if path.is_file()
        )
        command = [
            sys.executable, "-B", str(VALIDATOR), "validate",
            "--manifest", str(self.manifest),
        ]
        first = subprocess.run(
            command, check=False, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, env={}, cwd=ROOT,
        )
        second = subprocess.run(
            command, check=False, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, env={}, cwd=ROOT,
        )
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(first.stdout, second.stdout)
        self.assertEqual(first.stderr, "")
        self.assertNotIn(str(self.root), first.stdout)
        self.assertNotIn("audio/source.wav", first.stdout)
        self.assertEqual(json.loads(first.stdout), {
            "counts": {
                "channels": 2,
                "source_files": 1,
                "takes": 4,
                "takes_by_kind": {
                    "passive-pizzicato": 1,
                    "room-tone": 1,
                    "steady-arco": 1,
                    "sync": 1,
                },
            },
            "layout": {
                "bits_per_sample": 24,
                "channel_counts": [2],
                "sample_rate_hz": 96000,
            },
            "manifest_sha256": hashlib.sha256(manifest_before).hexdigest(),
            "schema": "hwa-viola-recording-session-summary",
            "schema_version": 1,
        })
        self.assertEqual(self.source.read_bytes(), source_before)
        self.assertEqual(self.manifest.read_bytes(), manifest_before)
        self.assertEqual(sorted(
            path.relative_to(self.root).as_posix()
            for path in self.root.rglob("*") if path.is_file()
        ), files_before)

    def test_rejects_unknown_and_duplicate_json_keys(self) -> None:
        unknown = copy.deepcopy(self.value)
        unknown["unexpected"] = True
        self.assert_rejected(unknown, "invalid fields")

        source = json.dumps(self.value)
        source = source.replace(
            '"session_id": "session-01"',
            '"session_id": "session-01", "session_id": "session-02"',
            1,
        )
        self.manifest.write_text(source, encoding="utf-8")
        with self.assertRaisesRegex(MODULE.ContractError,
                                    "duplicate JSON key: session_id"):
            MODULE.validate_manifest(self.manifest)

    def test_rejects_duplicate_channel_file_and_take_ids(self) -> None:
        channel = copy.deepcopy(self.value)
        channel["channels"][1]["id"] = channel["channels"][0]["id"]
        file_value = copy.deepcopy(self.value)
        file_value["source_files"].append(
            copy.deepcopy(file_value["source_files"][0])
        )
        take = copy.deepcopy(self.value)
        take["takes"][1]["id"] = take["takes"][0]["id"]
        for name, value in (("channel", channel), ("file", file_value),
                            ("take", take)):
            with self.subTest(identifier=name):
                self.assert_rejected(value, "duplicate")

    def test_rejects_bad_paths_hashes_layouts_and_take_bounds(self) -> None:
        variants = []
        for bad_path in ("../source.wav", "audio/./source.wav",
                         "audio//source.wav", str(self.source)):
            value = copy.deepcopy(self.value)
            value["source_files"][0]["path"] = bad_path
            variants.append(("path", value))
        value = copy.deepcopy(self.value)
        value["source_files"][0]["sha256"] = "0" * 64
        variants.append(("hash", value))
        value = copy.deepcopy(self.value)
        value["source_files"][0]["layout"]["frame_count"] = 999
        variants.append(("frame layout", value))
        value = copy.deepcopy(self.value)
        value["source_files"][0]["layout"]["channel_ids"] = ["bridge-mic"]
        variants.append(("channel layout", value))
        value = copy.deepcopy(self.value)
        value["takes"][0]["start_frame"] = 950
        value["takes"][0]["frame_count"] = 51
        variants.append(("bounds", value))
        value = copy.deepcopy(self.value)
        value["takes"][0]["frame_count"] = 0
        variants.append(("empty bounds", value))
        for name, value in variants:
            with self.subTest(variant=name):
                self.assert_rejected(value)

        bad_wave = self.root / "audio" / "pcm16.wav"
        write_wave(bad_wave, width=2)
        value = copy.deepcopy(self.value)
        value["source_files"][0]["path"] = "audio/pcm16.wav"
        value["source_files"][0]["sha256"] = sha256(bad_wave)
        self.assert_rejected(value, "96 kHz PCM24")

    def test_rejects_take_pitch_that_does_not_match_the_open_string(self) -> None:
        value = copy.deepcopy(self.value)
        value["takes"][0]["pitch"] = "G3"
        self.assert_rejected(value, "pitch does not match its open string")

    def test_rejects_missing_forbidden_and_invalid_bow_metadata(self) -> None:
        missing = copy.deepcopy(self.value)
        missing["takes"][1].pop("bow")
        forbidden = copy.deepcopy(self.value)
        forbidden["takes"][0]["bow"] = copy.deepcopy(
            forbidden["takes"][1]["bow"]
        )
        force = copy.deepcopy(self.value)
        force["takes"][1]["bow"]["force_n"] = 0
        speed = copy.deepcopy(self.value)
        speed["takes"][1]["bow"]["speed_m_per_s"] = True
        direction = copy.deepcopy(self.value)
        direction["takes"][1]["bow"]["direction"] = "sideways"
        method = copy.deepcopy(self.value)
        method["takes"][1]["bow"]["measurement_methods"].pop("speed")
        distance = copy.deepcopy(self.value)
        distance["takes"][1]["bow"]["bridge_distance_m"] = 0.5
        for name, value in (
                ("missing", missing), ("forbidden", forbidden),
                ("force", force), ("speed", speed),
                ("direction", direction), ("method", method),
                ("distance", distance)):
            with self.subTest(variant=name):
                self.assert_rejected(value)

    def test_rejects_wrong_derived_position_ratio_and_processing(self) -> None:
        ratio = copy.deepcopy(self.value)
        ratio["takes"][1]["bow"]["position_ratio"] = 0.10000001
        self.assert_rejected(ratio, "position_ratio")

        processing = copy.deepcopy(self.value)
        processing["processing"] = ["normalized"]
        self.write_manifest(processing)
        completed = subprocess.run(
            [sys.executable, "-B", str(VALIDATOR), "validate",
             "--manifest", str(self.manifest)],
            check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, env={}, cwd=ROOT,
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertEqual(completed.stdout, "")
        self.assertIn("processing must be an empty array", completed.stderr)
        self.assertNotIn(str(self.root), completed.stderr)

    def test_rejects_symlink_source(self) -> None:
        link = self.root / "audio" / "linked.wav"
        try:
            link.symlink_to(self.source)
        except (NotImplementedError, OSError) as error:
            self.skipTest("symlinks unavailable: {}".format(error))
        value = copy.deepcopy(self.value)
        value["source_files"][0]["path"] = "audio/linked.wav"
        self.assert_rejected(value, "regular non-symlink")

    def test_caches_repeated_source_file_inspection(self) -> None:
        value = copy.deepcopy(self.value)
        second = copy.deepcopy(value["source_files"][0])
        second["id"] = "source-02"
        value["source_files"].append(second)
        value["takes"][3]["file_id"] = "source-02"
        self.write_manifest(value)
        with mock.patch.object(
                MODULE, "inspect_wave", wraps=MODULE.inspect_wave) as inspected:
            summary = MODULE.validate_manifest(self.manifest)
        self.assertEqual(inspected.call_count, 1)
        self.assertEqual(summary["counts"]["source_files"], 2)


if __name__ == "__main__":
    unittest.main()

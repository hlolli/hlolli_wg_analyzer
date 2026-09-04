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
import unittest
from unittest import mock
import wave


ROOT = Path(__file__).resolve().parents[1]
VALIDATOR = (ROOT / "adapters" / "hlolli_wg_double_bass" /
             "recording_session.py")
MODULE_SPEC = importlib.util.spec_from_file_location(
    "double_bass_recording_session", VALIDATOR,
)
assert MODULE_SPEC is not None and MODULE_SPEC.loader is not None
MODULE = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(MODULE)


PITCHES = {"e": "E1", "a": "A1", "d": "D2", "g": "G2"}
LEVEL_VALUES = {
    "force": {"low": 0.5, "medium": 1.0, "high": 2.0},
    "speed": {"slow": 0.1, "medium": 0.2, "fast": 0.4},
    "position": {"ponticello": 0.05, "middle": 0.1, "tasto": 0.2},
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_wave(path: Path, *, channels: int = 2, frames: int = 1000,
               rate: int = 96000, width: int = 3, sample_value: int = 1) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(channels)
        stream.setsampwidth(width)
        stream.setframerate(rate)
        sample = sample_value.to_bytes(width, "little", signed=True)
        stream.writeframes(sample * channels * frames)


def capture_takes(split: str) -> list[dict]:
    takes = []
    start = 0
    passive_repetitions = 5 if split == "fit" else 3
    cell_repetitions = 2 if split == "fit" else 1
    for string_id, pitch in PITCHES.items():
        for repetition in range(passive_repetitions):
            takes.append({
                "id": f"pizz-{string_id}-{repetition + 1:02d}",
                "kind": "passive-pizzicato",
                "file_id": "source-01",
                "start_frame": start,
                "frame_count": 1,
                "string_id": string_id,
                "pitch": pitch,
                "articulation": "ordinary-pizzicato",
                "pluck": {
                    "bridge_distance_m": 0.21,
                    "position_ratio": 0.2,
                    "measurement_method": "steel-rule-01",
                },
            })
            start += 1
        for cell, levels in MODULE.CONTROL_CELLS.items():
            force_level, speed_level, position_level, direction = levels
            for repetition in range(cell_repetitions):
                bridge_distance = LEVEL_VALUES["position"][position_level]
                takes.append({
                    "id": f"arco-{string_id}-{cell}-{repetition + 1:02d}",
                    "kind": "steady-arco",
                    "file_id": "source-01",
                    "start_frame": start,
                    "frame_count": 1,
                    "string_id": string_id,
                    "pitch": pitch,
                    "articulation": "sustain",
                    "control_cell": cell,
                    "bow": {
                        "force_n": LEVEL_VALUES["force"][force_level],
                        "speed_m_per_s": LEVEL_VALUES["speed"][speed_level],
                        "bridge_distance_m": bridge_distance,
                        "position_ratio": bridge_distance / 1.05,
                        "direction": direction,
                        "measurement_methods": {
                            "force": "load-cell-01",
                            "speed": "motion-track-01",
                            "bridge_distance": "steel-rule-01",
                        },
                    },
                })
                start += 1
    takes.extend((
        {
            "id": "room-01", "kind": "room-tone", "file_id": "source-01",
            "start_frame": start, "frame_count": 1,
        },
        {
            "id": "sync-01", "kind": "sync", "file_id": "source-01",
            "start_frame": start + 1, "frame_count": 1,
        },
    ))
    return takes


def valid_manifest(source: Path, evidence: Path, *, split: str = "check",
                   source_path: str = "audio/source.wav",
                   evidence_path: str = "rights/consent.txt") -> dict:
    return {
        "schema": "hwa-double-bass-recording-session",
        "schema_version": 1,
        "session_id": f"session-{split}-01",
        "source_group_id": f"controlled-double-bass-{split}-01",
        "split": split,
        "recorded_at": (
            "2026-09-05T10:00:00+03:00" if split == "fit"
            else "2026-09-06T10:00:00+03:00"
        ),
        "recording_setup": {
            "instrument_id": "double-bass-01",
            "bow_id": "bow-01",
            "performer_id": "performer-01",
            "room_id": "room-01",
            "recorder_id": "recorder-01",
            "audio_interface_id": "interface-01",
            "clock_source": "interface-internal",
            "physical_string_logging_method": "direct-session-log",
        },
        "rights": {
            "rights_holder_id": "rights-holder-01",
            "performer_consent": "documented",
            "analysis_permission": True,
            "model_fitting_permission": True,
            "audio_redistribution_permission": False,
            "evidence_path": evidence_path,
            "evidence_sha256": sha256(evidence),
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
            {"id": string_id, "open_pitch": pitch, "length_m": 1.05,
             "maker_model_id": f"string-{string_id}-01",
             "measurement_method": "steel-rule-01"}
            for string_id, pitch in PITCHES.items()
        ],
        "channels": [
            {"id": "bridge-mic", "role": "instrument",
             "transducer_id": "mic-01", "input_id": "input-01",
             "distance_m": 0.5, "height_m": 1.0, "azimuth_degrees": 0.0,
             "measurement_method": "laser-rule-01"},
            {"id": "room-mic", "role": "room",
             "transducer_id": "mic-02", "input_id": "input-02",
             "distance_m": 2.0, "height_m": 1.5, "azimuth_degrees": 0.0,
             "measurement_method": "laser-rule-01"},
        ],
        "source_files": [{
            "id": "source-01",
            "path": source_path,
            "sha256": sha256(source),
            "layout": {
                "sample_rate_hz": 96000,
                "bits_per_sample": 24,
                "frame_count": 1000,
                "channel_ids": ["bridge-mic", "room-mic"],
            },
        }],
        "takes": capture_takes(split),
        "processing": [],
    }


class DoubleBassRecordingSessionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(
            prefix="hwa double bass recording session ",
        )
        self.root = Path(self.temporary.name)
        self.source = self.root / "audio" / "source.wav"
        write_wave(self.source)
        self.evidence = self.root / "rights" / "consent.txt"
        self.evidence.parent.mkdir(parents=True)
        self.evidence.write_text("fixture only", encoding="utf-8")
        self.manifest = self.root / "manifest.json"
        self.value = valid_manifest(self.source, self.evidence)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_manifest(self, value: dict | None = None) -> Path:
        if value is None:
            value = self.value
        self.manifest.write_text(json.dumps(value), encoding="utf-8")
        return self.manifest

    def assert_rejected(self, value: dict, message: str | None = None) -> None:
        self.write_manifest(value)
        context = (self.assertRaisesRegex(MODULE.ContractError, message)
                   if message is not None
                   else self.assertRaises(MODULE.ContractError))
        with context:
            MODULE.validate_manifest(self.manifest)

    def first_take(self, value: dict, kind: str) -> dict:
        return next(take for take in value["takes"] if take["kind"] == kind)

    def test_valid_session_cli_is_path_free_deterministic_and_read_only(self) -> None:
        self.write_manifest()
        source_before = self.source.read_bytes()
        evidence_before = self.evidence.read_bytes()
        manifest_before = self.manifest.read_bytes()
        files_before = sorted(
            path.relative_to(self.root).as_posix()
            for path in self.root.rglob("*") if path.is_file()
        )
        command = [
            sys.executable, "-B", str(VALIDATOR), "validate-session",
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
        summary = json.loads(first.stdout)
        self.assertEqual(summary["schema"],
                         "hwa-double-bass-recording-session-summary")
        self.assertEqual(summary["manifest_sha256"],
                         hashlib.sha256(manifest_before).hexdigest())
        self.assertEqual(summary["counts"]["takes"], 38)
        self.assertEqual(summary["counts"]["takes_by_kind"], {
            "passive-pizzicato": 12,
            "room-tone": 1,
            "steady-arco": 24,
            "sync": 1,
        })
        self.assertEqual(summary["session"]["split"], "check")
        self.assertEqual(summary["session"]["source_file_sha256"],
                         [sha256(self.source)])
        self.assertEqual(self.source.read_bytes(), source_before)
        self.assertEqual(self.evidence.read_bytes(), evidence_before)
        self.assertEqual(self.manifest.read_bytes(), manifest_before)
        self.assertEqual(sorted(
            path.relative_to(self.root).as_posix()
            for path in self.root.rglob("*") if path.is_file()
        ), files_before)

    def test_rejects_unknown_duplicate_keys_and_invalid_timestamp(self) -> None:
        unknown = copy.deepcopy(self.value)
        unknown["unexpected"] = True
        self.assert_rejected(unknown, "invalid fields")

        source = json.dumps(self.value)
        source = source.replace(
            '"session_id": "session-check-01"',
            '"session_id": "session-check-01", "session_id": "other"', 1,
        )
        self.manifest.write_text(source, encoding="utf-8")
        with self.assertRaisesRegex(MODULE.ContractError,
                                    "duplicate JSON key: session_id"):
            MODULE.validate_manifest(self.manifest)

        timestamp = copy.deepcopy(self.value)
        timestamp["recorded_at"] = "tomorrow"
        self.assert_rejected(timestamp, "RFC 3339")

    def test_rejects_bad_rights_evidence_or_permissions(self) -> None:
        for field, replacement, message in (
            ("analysis_permission", False, "private analysis"),
            ("model_fitting_permission", False, "model fitting"),
            ("audio_redistribution_permission", "unknown", "boolean"),
            ("performer_consent", "pending", "documented"),
            ("evidence_sha256", "0" * 64, "SHA-256"),
        ):
            with self.subTest(field=field):
                value = copy.deepcopy(self.value)
                value["rights"][field] = replacement
                self.assert_rejected(value, message)

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
        value["takes"][0]["start_frame"] = 1000
        value["takes"][0]["frame_count"] = 1
        variants.append(("bounds", value))
        for name, value in variants:
            with self.subTest(variant=name):
                self.assert_rejected(value)

        bad_wave = self.root / "audio" / "pcm16.wav"
        write_wave(bad_wave, width=2)
        value = copy.deepcopy(self.value)
        value["source_files"][0]["path"] = "audio/pcm16.wav"
        value["source_files"][0]["sha256"] = sha256(bad_wave)
        self.assert_rejected(value, "96 kHz PCM24")

    def test_rejects_bad_string_pitch_articulation_and_position(self) -> None:
        pitch = copy.deepcopy(self.value)
        self.first_take(pitch, "passive-pizzicato")["pitch"] = "A1"
        self.assert_rejected(pitch, "pitch does not match")

        articulation = copy.deepcopy(self.value)
        self.first_take(articulation, "passive-pizzicato")[
            "articulation"
        ] = "bartok-pizzicato"
        self.assert_rejected(articulation, "ordinary-pizzicato")

        ratio = copy.deepcopy(self.value)
        self.first_take(ratio, "steady-arco")["bow"][
            "position_ratio"
        ] = 0.123
        self.assert_rejected(ratio, "position_ratio")

    def test_rejects_missing_cells_bad_direction_and_unseparated_levels(self) -> None:
        missing = copy.deepcopy(self.value)
        missing["takes"] = [
            take for take in missing["takes"]
            if not (take["kind"] == "steady-arco"
                    and take.get("string_id") == "e"
                    and take.get("control_cell") == "high-fast-middle-up")
        ]
        self.assert_rejected(missing, "lacks control cell")

        direction = copy.deepcopy(self.value)
        self.first_take(direction, "steady-arco")["bow"][
            "direction"
        ] = "up-bow"
        self.assert_rejected(direction, "declared control cell")

        levels = copy.deepcopy(self.value)
        for take in levels["takes"]:
            if (take["kind"] == "steady-arco"
                    and take["string_id"] == "e"
                    and take["control_cell"].startswith("low-")):
                take["bow"]["force_n"] = 4.0
        self.assert_rejected(levels, "bow-force levels")

    def test_rejects_overlap_missing_room_and_non_instrument_channels(self) -> None:
        overlap = copy.deepcopy(self.value)
        overlap["takes"][1]["start_frame"] = overlap["takes"][0]["start_frame"]
        self.assert_rejected(overlap, "overlap")

        room = copy.deepcopy(self.value)
        room["takes"] = [take for take in room["takes"]
                         if take["kind"] != "room-tone"]
        self.assert_rejected(room, "room-tone")

        channels = copy.deepcopy(self.value)
        channels["channels"][0]["role"] = "room"
        self.assert_rejected(channels, "instrument channel")

    def test_rejects_symlink_source_and_caches_repeated_inspection(self) -> None:
        link = self.root / "audio" / "linked.wav"
        try:
            link.symlink_to(self.source)
        except (NotImplementedError, OSError) as error:
            self.skipTest("symlinks unavailable: {}".format(error))
        value = copy.deepcopy(self.value)
        value["source_files"][0]["path"] = "audio/linked.wav"
        self.assert_rejected(value, "regular non-symlink")

        value = copy.deepcopy(self.value)
        second = copy.deepcopy(value["source_files"][0])
        second["id"] = "source-02"
        value["source_files"].append(second)
        value["takes"][-1]["file_id"] = "source-02"
        self.write_manifest(value)
        with mock.patch.object(
                MODULE, "inspect_wave", wraps=MODULE.inspect_wave) as inspected:
            summary = MODULE.validate_manifest(self.manifest)
        self.assertEqual(inspected.call_count, 1)
        self.assertEqual(summary["counts"]["source_files"], 2)

    def test_campaign_binds_distinct_fit_and_check_groups(self) -> None:
        fit_dir = self.root / "fit"
        check_dir = self.root / "check"
        for directory in (fit_dir, check_dir):
            (directory / "audio").mkdir(parents=True)
            (directory / "rights").mkdir()
            (directory / "rights" / "consent.txt").write_text(
                "fixture only", encoding="utf-8",
            )
        fit_source = fit_dir / "audio" / "source.wav"
        check_source = check_dir / "audio" / "source.wav"
        write_wave(fit_source, sample_value=1)
        write_wave(check_source, sample_value=2)
        fit_manifest = fit_dir / "manifest.json"
        check_manifest = check_dir / "manifest.json"
        fit_manifest.write_text(json.dumps(valid_manifest(
            fit_source, fit_dir / "rights" / "consent.txt", split="fit",
        )), encoding="utf-8")
        check_manifest.write_text(json.dumps(valid_manifest(
            check_source, check_dir / "rights" / "consent.txt", split="check",
        )), encoding="utf-8")
        campaign = self.root / "campaign.json"
        campaign_value = {
            "schema": "hwa-double-bass-recording-campaign",
            "schema_version": 1,
            "campaign_id": "double-bass-stage3-01",
            "split_policy": "source-group-separated-before-fitting",
            "sessions": [
                {"split": "fit", "manifest_path": "fit/manifest.json",
                 "sha256": sha256(fit_manifest)},
                {"split": "check", "manifest_path": "check/manifest.json",
                 "sha256": sha256(check_manifest)},
            ],
            "processing": [],
        }
        campaign.write_text(json.dumps(campaign_value), encoding="utf-8")
        before = {
            path: path.read_bytes() for path in self.root.rglob("*")
            if path.is_file()
        }
        completed = subprocess.run(
            [sys.executable, "-B", str(VALIDATOR), "validate-campaign",
             "--campaign", str(campaign)],
            check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, env={}, cwd=ROOT,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        summary = json.loads(completed.stdout)
        self.assertEqual(summary["schema"],
                         "hwa-double-bass-recording-campaign-summary")
        self.assertEqual(summary["counts"], {
            "sessions": 2, "source_files": 2, "takes": 108,
        })
        self.assertEqual(set(summary["sessions"]), {"fit", "check"})
        self.assertNotIn(str(self.root), completed.stdout)
        self.assertEqual(before, {
            path: path.read_bytes() for path in self.root.rglob("*")
            if path.is_file()
        })

        same_group = copy.deepcopy(campaign_value)
        check_value = json.loads(check_manifest.read_text())
        check_value["source_group_id"] = "controlled-double-bass-fit-01"
        check_manifest.write_text(json.dumps(check_value), encoding="utf-8")
        same_group["sessions"][1]["sha256"] = sha256(check_manifest)
        campaign.write_text(json.dumps(same_group), encoding="utf-8")
        with self.assertRaisesRegex(MODULE.ContractError,
                                    "distinct source groups"):
            MODULE.validate_campaign(campaign)

    def test_relative_manifest_and_campaign_paths_are_rejected(self) -> None:
        with self.assertRaisesRegex(MODULE.ContractError, "absolute"):
            MODULE.validate_manifest(Path("manifest.json"))
        with self.assertRaisesRegex(MODULE.ContractError, "absolute"):
            MODULE.validate_campaign(Path("campaign.json"))


if __name__ == "__main__":
    unittest.main()

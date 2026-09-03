#!/usr/bin/python3
"""Build and run the checked renderer for the fixed viola model."""

import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Iterable, Optional

sys.dont_write_bytecode = True


FROZEN_CONFIG = None
ROOT = Path(__file__).resolve().parents[2]
VIOLA_ROOT = ROOT.parent / "hlolli_wg_viola"
SOURCE_DIR = Path(__file__).resolve().parent
FIT_MANIFEST = SOURCE_DIR / "fit-passive-c-v1.json"
ADAPTER_ID = "hlolli-wg-viola-passive-c-v1"
METHOD_VERSION = "stage8-1"
ROSTER_SCHEMA = "hwa-viola-passive-tail-roster"
JOINT_DIAGNOSTIC_MODE = "joint-passive-diagnostic-roster"
JOINT_DIAGNOSTIC_METHOD = "viola-passive-joint-1"
JOINT_DIAGNOSTIC_FRAMES = 705600
JOINT_DIAGNOSTIC_MODULE = "hlolli_wg_viola_joint_test"
JOINT_CUTOFF_LEVELS = (0.5, 1.0, 2.0, 4.0)
# Keep tool lookup deterministic while supporting NixOS, whose system tools
# live outside /usr/bin. Configured Python, Csound, and compiler paths remain
# explicit checked resources.
CLEAN_PATH = "/run/current-system/sw/bin:/usr/bin:/bin"
ROSTER_SOURCE_RULES = {
    "iowa-2012": {"split": "fit", "total_weight": 0.5},
    "rwc-variation-1": {"split": "fit", "total_weight": 0.5},
    "rwc-variation-2": {"split": "check", "total_weight": 1.0},
    "best-music-tools-a442": {"split": "check", "total_weight": 1.0},
}
ROSTER_CHECK_FAMILIES = {
    "c3": "rwc-variation-2",
    "g3": "best-music-tools-a442",
    "d4": "rwc-variation-2",
    "a4": "rwc-variation-2",
}
ROSTER_FIT_COUNTS = {
    "c3": {"iowa-2012": 1, "rwc-variation-1": 3},
    "g3": {"iowa-2012": 1, "rwc-variation-1": 1},
    "d4": {"iowa-2012": 1, "rwc-variation-1": 2},
    "a4": {"iowa-2012": 1, "rwc-variation-1": 2},
}
STRING_TARGETS = {
    "c3": {
        "adapter_id": ADAPTER_ID,
        "fit_manifest": SOURCE_DIR / "fit-passive-c-v1.json",
        "profile_index": 0,
        "render_string": 1,
        "nominal_open_hz": 130.8127826502993,
        "measured_reference_hz": 130.58241487372985,
        "parameter_id": "loss_time_constant_c_seconds",
        "baseline": 1.15,
    },
    "g3": {
        "adapter_id": "hlolli-wg-viola-passive-g-v1",
        "fit_manifest": SOURCE_DIR / "fit-passive-g-v1.json",
        "profile_index": 1,
        "render_string": 2,
        "nominal_open_hz": 195.99771799087463,
        "measured_reference_hz": 195.58538558422308,
        "parameter_id": "loss_time_constant_g_seconds",
        "baseline": 1.9,
    },
    "d4": {
        "adapter_id": "hlolli-wg-viola-passive-d-v1",
        "fit_manifest": SOURCE_DIR / "fit-passive-d-v1.json",
        "profile_index": 2,
        "render_string": 3,
        "nominal_open_hz": 293.6647679174076,
        "measured_reference_hz": 292.96845778349154,
        "parameter_id": "loss_time_constant_d_seconds",
        "baseline": 0.85,
    },
    "a4": {
        "adapter_id": "hlolli-wg-viola-passive-a-v1",
        "fit_manifest": SOURCE_DIR / "fit-passive-a-v1.json",
        "profile_index": 3,
        "render_string": 4,
        "nominal_open_hz": 440.0,
        "measured_reference_hz": 441.08823856808476,
        "parameter_id": "loss_time_constant_a_seconds",
        "baseline": 0.45,
    },
}
for _target_spec in STRING_TARGETS.values():
    _target_spec["render_a4"] = (
        440.0 * _target_spec["measured_reference_hz"] /
        _target_spec["nominal_open_hz"]
    )
MAX_JSON_BYTES = 1024 * 1024
PASSIVE_SEARCH_LEVELS = (
    0.10, 0.15, 0.25, 0.35, 0.45, 0.55,
    0.70, 0.85, 1.00, 1.15, 1.30, 1.45, 1.60, 1.75,
    1.90, 2.05, 2.20, 2.50, 3.00, 4.00, 5.00,
)


def passive_parameter(target: str) -> dict[str, Any]:
    spec = STRING_TARGETS[target]
    return {
        "id": spec["parameter_id"],
        "unit": "seconds",
        "minimum": 0.02,
        "maximum": 5.0,
        "baseline": spec["baseline"],
        "profile_paths": [[
            "strings", spec["profile_index"], "loss_time_constant_seconds",
        ]],
    }


PASSIVE_PARAMETER = passive_parameter("c3")


def joint_diagnostic_adapter_id(target: str) -> str:
    if target not in STRING_TARGETS:
        raise AdapterError("unknown viola diagnostic target")
    return "hlolli-wg-viola-passive-{}-joint-diagnostic-v1".format(target)


def joint_diagnostic_parameters(target: str) -> list[dict[str, Any]]:
    passive = passive_parameter(target)
    rows = [{
        "id": passive["id"],
        "unit": passive["unit"],
        "minimum": passive["minimum"],
        "maximum": passive["maximum"],
        "baseline": passive["baseline"],
        "levels": list(PASSIVE_SEARCH_LEVELS),
    }, {
        "id": "nut_bridge_cutoff_scale",
        "unit": "ratio",
        "minimum": 0.5,
        "maximum": 4.0,
        "baseline": 1.0,
        "levels": list(JOINT_CUTOFF_LEVELS),
    }]
    return sorted(rows, key=lambda row: row["id"])


class AdapterError(ValueError):
    pass


def unique_object(pairs: Iterable[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise AdapterError("duplicate JSON key: " + key)
        result[key] = value
    return result


def read_bounded_json(path: Path) -> bytes:
    try:
        with path.open("rb") as stream:
            if os.fstat(stream.fileno()).st_size > MAX_JSON_BYTES:
                raise AdapterError(
                    "JSON input exceeds the byte limit: {}".format(path)
                )
            source = stream.read(MAX_JSON_BYTES + 1)
    except OSError as error:
        raise AdapterError("cannot read JSON {}: {}".format(path, error)) from error
    if len(source) > MAX_JSON_BYTES:
        raise AdapterError("JSON input exceeds the byte limit: {}".format(path))
    return source


def parse_json(source: bytes, path: Path) -> dict[str, Any]:
    def invalid_constant(value: str) -> None:
        raise AdapterError("invalid JSON number: " + value)

    try:
        value = json.loads(
            source.decode("utf-8"),
            object_pairs_hook=unique_object,
            parse_constant=invalid_constant,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise AdapterError("cannot read JSON {}: {}".format(path, error)) from error
    if type(value) is not dict:
        raise AdapterError("JSON root must be an object: {}".format(path))
    return value


def load_json_evidence(path: Path) -> tuple[dict[str, Any], str]:
    source = read_bounded_json(path)
    source_hash = hashlib.sha256(source).hexdigest()
    return parse_json(source, path), source_hash


def load_json(path: Path) -> dict[str, Any]:
    value, _ = load_json_evidence(path)
    return value


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise AdapterError("cannot hash {}: {}".format(path, error)) from error
    return digest.hexdigest()


def regular(path: Path, name: str) -> Path:
    path = path.absolute()
    if not path.is_file() or path.is_symlink():
        raise AdapterError("{} must be a regular file: {}".format(name, path))
    return path


def executable(path: Path, name: str) -> Path:
    path = regular(path, name)
    if not os.access(path, os.X_OK):
        raise AdapterError("{} must be executable: {}".format(name, path))
    return path


def real_directory(path: Path, name: str) -> Path:
    path = path.absolute()
    if not path.is_dir() or path.is_symlink():
        raise AdapterError("{} must be a real directory: {}".format(name, path))
    return path


def under(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        return False
    return True


def wave_evidence(path: Path, name: str
                  ) -> tuple[int, int, int, int, int, str]:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            header = stream.read(12)
            digest.update(header)
            if (len(header) != 12 or header[:4] != b"RIFF" or
                    header[8:12] != b"WAVE"):
                raise AdapterError("invalid PCM WAVE header: " + name)
            file_size = os.fstat(stream.fileno()).st_size
            declared_size = int.from_bytes(header[4:8], "little") + 8
            if declared_size != file_size or declared_size < 12:
                raise AdapterError("PCM WAVE size mismatch: " + name)
            format_chunk = None
            data_start = None
            data_size = None
            while stream.tell() < declared_size:
                if declared_size - stream.tell() < 8:
                    raise AdapterError("truncated WAVE chunk: " + name)
                chunk = stream.read(8)
                if len(chunk) != 8:
                    raise AdapterError("truncated WAVE chunk: " + name)
                digest.update(chunk)
                chunk_name = chunk[:4]
                size = int.from_bytes(chunk[4:8], "little")
                start = stream.tell()
                end = start + size
                padded_end = end + (size & 1)
                if end > declared_size or padded_end > declared_size:
                    if end != declared_size:
                        raise AdapterError("truncated WAVE chunk: " + name)
                    padded_end = end
                if chunk_name == b"fmt ":
                    if format_chunk is not None or size > 4096:
                        raise AdapterError("invalid WAVE format chunk: " + name)
                    format_chunk = stream.read(size)
                    if len(format_chunk) != size:
                        raise AdapterError("truncated WAVE format: " + name)
                    digest.update(format_chunk)
                elif chunk_name == b"data":
                    if data_start is not None:
                        raise AdapterError("duplicate WAVE data chunk: " + name)
                    data_start = start
                    data_size = size
                if chunk_name != b"fmt ":
                    remaining = size
                    while remaining:
                        block = stream.read(min(remaining, 1024 * 1024))
                        if not block:
                            raise AdapterError("truncated WAVE chunk: " + name)
                        digest.update(block)
                        remaining -= len(block)
                if padded_end != end:
                    padding = stream.read(1)
                    if len(padding) != 1:
                        raise AdapterError("truncated WAVE chunk: " + name)
                    digest.update(padding)
                if stream.tell() != padded_end:
                    raise AdapterError("invalid WAVE chunk size: " + name)
    except OSError as error:
        raise AdapterError("cannot read PCM WAVE {}: {}".format(name, error)) from error
    if (format_chunk is None or data_start is None or data_size is None or
            len(format_chunk) < 16):
        raise AdapterError("PCM WAVE lacks format or data: " + name)
    format_tag = int.from_bytes(format_chunk[0:2], "little")
    channels = int.from_bytes(format_chunk[2:4], "little")
    rate = int.from_bytes(format_chunk[4:8], "little")
    byte_rate = int.from_bytes(format_chunk[8:12], "little")
    block_align = int.from_bytes(format_chunk[12:14], "little")
    bits = int.from_bytes(format_chunk[14:16], "little")
    if format_tag == 0xfffe:
        pcm_guid = bytes.fromhex("0100000000001000800000aa00389b71")
        if (len(format_chunk) < 40 or
                int.from_bytes(format_chunk[16:18], "little") < 22 or
                int.from_bytes(format_chunk[18:20], "little") != bits or
                format_chunk[24:40] != pcm_guid):
            raise AdapterError("WAVE extensible input is not integer PCM: " + name)
    elif format_tag != 1:
        raise AdapterError("WAVE input is not integer PCM: " + name)
    sample_bytes = bits // 8
    if (rate != 44100 or channels not in (1, 2) or bits not in (16, 24) or
            block_align != channels * sample_bytes or
            byte_rate != rate * block_align or data_size < block_align or
            data_size % block_align != 0):
        raise AdapterError(
            "{} must be nonempty mono or stereo 44.1 kHz PCM16 or PCM24 WAVE".format(
                name
            )
        )
    return (rate, channels, data_size // block_align, data_start, data_size,
            digest.hexdigest())


def wave_layout(path: Path, name: str) -> tuple[int, int, int, int, int]:
    rate, channels, frames, data_start, data_size, _ = wave_evidence(path, name)
    return rate, channels, frames, data_start, data_size


def wave_facts(path: Path, name: str) -> tuple[int, int, int]:
    rate, channels, frames, _, _ = wave_layout(path, name)
    return rate, channels, frames


def exact_object(value: Any, keys: Iterable[str], name: str) -> dict[str, Any]:
    if type(value) is not dict or set(value) != set(keys):
        raise AdapterError(name + " has invalid fields")
    return value


def exact_value(value: Any, expected: Any) -> bool:
    return type(value) is type(expected) and value == expected


def exact_tree(value: Any, expected: Any) -> bool:
    if type(value) is not type(expected):
        return False
    if type(expected) is dict:
        return (set(value) == set(expected) and
                all(exact_tree(value[key], expected[key]) for key in expected))
    if type(expected) is list:
        return (len(value) == len(expected) and
                all(exact_tree(left, right)
                    for left, right in zip(value, expected)))
    return value == expected


def unsigned(value: Any, name: str, minimum: int = 0) -> int:
    if (type(value) is not int or value < minimum or
            value > 0xffffffffffffffff):
        raise AdapterError(name + " is invalid")
    return value


def finite(value: Any, name: str) -> float:
    if type(value) not in (int, float):
        raise AdapterError(name + " must be finite")
    result = float(value)
    if not math.isfinite(result):
        raise AdapterError(name + " must be finite")
    return result


def render_facts(target: str, frequency_hz: Any,
                 name: str) -> dict[str, Any]:
    spec = STRING_TARGETS[target]
    frequency = finite(frequency_hz, name + " frequency")
    a4_hz = 440.0 * frequency / spec["nominal_open_hz"]
    if a4_hz < 380.0 or a4_hz > 480.0:
        raise AdapterError(name + " frequency is outside the numeric-A4 range")
    return {
        "a4_hz": a4_hz,
        "frequency_hz": frequency,
        "string": spec["render_string"],
    }


def roster_case_id(value: Any, name: str) -> str:
    if (type(value) is not str or
            re.fullmatch(r"[a-z0-9][a-z0-9._-]{0,99}", value) is None):
        raise AdapterError(name + " must be a stable lower-case id")
    objective = value + "-passive-decay"
    if (len(objective) > 127 or
            re.fullmatch(r"[A-Za-z0-9._-]+", objective) is None):
        raise AdapterError(name + " cannot form a fit objective id")
    return value


def load_selection_roster(path: Path,
                          repository_roots: Iterable[Path] = ()) -> dict[str, Any]:
    supplied = Path(path)
    if not supplied.is_absolute():
        raise AdapterError("selection roster path must be absolute")
    source_path = regular(supplied, "selection roster").resolve()
    roots = tuple(Path(root) for root in repository_roots)
    if any(under(source_path, root) for root in roots):
        raise AdapterError(
            "selection roster must stay outside the repositories"
        )
    value, source_hash = load_json_evidence(source_path)
    if sha256(source_path) != source_hash:
        raise AdapterError("selection roster changed while reading")
    exact_object(value, ("schema", "schema_version", "target", "cases"),
                 "selection roster")
    if (not exact_value(value["schema"], ROSTER_SCHEMA) or
            not exact_value(value["schema_version"], 1)):
        raise AdapterError("unsupported selection roster")
    target = value["target"]
    if type(target) is not str or target not in STRING_TARGETS:
        raise AdapterError("selection roster has an unknown target")
    source_cases = value["cases"]
    if type(source_cases) is not list or not source_cases:
        raise AdapterError("selection roster needs accepted tails")

    cases = []
    ids = set()
    hashes = set()
    family_counts = {name: 0 for name in ROSTER_SOURCE_RULES}
    check_family = ROSTER_CHECK_FAMILIES[target]
    for index, source_case in enumerate(source_cases):
        name = "selection roster case {}".format(index)
        row = exact_object(source_case, (
            "id", "source_family", "split", "path", "sha256",
            "frequency_hz",
        ), name)
        case_id = roster_case_id(row["id"], name + " id")
        if case_id in ids:
            raise AdapterError("selection roster has a duplicate case id")
        ids.add(case_id)
        family = row["source_family"]
        rule = (ROSTER_SOURCE_RULES.get(family)
                if type(family) is str else None)
        if rule is None:
            raise AdapterError("selection roster has an unknown source family")
        if rule["split"] == "check" and family != check_family:
            raise AdapterError(
                "selection roster has the wrong check source family"
            )
        if not exact_value(row["split"], rule["split"]):
            raise AdapterError("selection roster source family has wrong split")
        path_text = row["path"]
        if type(path_text) is not str or not Path(path_text).is_absolute():
            raise AdapterError("selection roster WAVE path must be absolute")
        recording = regular(Path(path_text), name + " WAVE").resolve()
        if any(under(recording, root) for root in roots):
            raise AdapterError(
                "selection roster recordings must stay outside the repositories"
            )
        expected_hash = row["sha256"]
        if (type(expected_hash) is not str or
                re.fullmatch(r"[0-9a-f]{64}", expected_hash) is None):
            raise AdapterError("selection roster has an invalid SHA-256")
        (rate, channels, frames, _, _,
         actual_hash) = wave_evidence(recording, name + " WAVE")
        if actual_hash != expected_hash:
            raise AdapterError("selection roster WAVE hash does not match")
        if actual_hash in hashes:
            raise AdapterError("selection roster has duplicate audio")
        hashes.add(actual_hash)
        render = render_facts(target, row["frequency_hz"], name)
        family_counts[family] += 1
        cases.append({
            "id": case_id,
            "source_family": family,
            "split": rule["split"],
            "path": recording,
            "sha256": actual_hash,
            "frequency_hz": render["frequency_hz"],
            "a4_hz": render["a4_hz"],
            "string": render["string"],
            "rate_hz": rate,
            "channels": channels,
            "frames": frames,
        })
    required_families = (
        "iowa-2012", "rwc-variation-1", check_family,
    )
    missing = [name for name in required_families
               if family_counts[name] == 0]
    if missing:
        raise AdapterError(
            "selection roster needs an accepted tail from each source family"
        )
    for family, expected_count in ROSTER_FIT_COUNTS[target].items():
        if family_counts[family] != expected_count:
            raise AdapterError(
                "selection roster has the wrong accepted fit-tail count"
            )
    check_count = family_counts[check_family]
    if target == "g3" and check_count != 1:
        raise AdapterError(
            "selection roster needs exactly one best-music-tools-a442 "
            "check tail"
        )
    if target != "g3" and not 1 <= check_count <= 3:
        raise AdapterError(
            "selection roster needs 1..3 accepted RWC variation-2 tails"
        )
    objective_ids = {row["id"] + "-passive-decay" for row in cases}
    if len(objective_ids) != len(cases):
        raise AdapterError("selection roster objective ids collide")
    for row in cases:
        row["weight"] = (
            ROSTER_SOURCE_RULES[row["source_family"]]["total_weight"] /
            family_counts[row["source_family"]]
        )
    cases.sort(key=lambda row: row["id"])
    normalized = {
        "schema": ROSTER_SCHEMA,
        "schema_version": 1,
        "target": target,
        "cases": [{
            "id": row["id"],
            "source_family": row["source_family"],
            "split": row["split"],
            "path": str(row["path"]),
            "sha256": row["sha256"],
            "frequency_hz": row["frequency_hz"],
        } for row in cases],
    }
    return {
        "source_path": source_path,
        "source_sha256": source_hash,
        "target": target,
        "cases": cases,
        "normalized": normalized,
    }


def validate_fit_manifest(value: dict[str, Any], target: str = "c3") -> None:
    if target not in STRING_TARGETS:
        raise AdapterError("unknown viola fit target")
    spec = STRING_TARGETS[target]
    expected = {
        "schema": "hwa-instrument-fit",
        "schema_version": 1,
        "adapter_id": spec["adapter_id"],
        "parameters": [passive_parameter(target)],
        "objectives": [
            {
                "id": "fit_passive_decay",
                "kind": "passive-decay",
                "case": target + "-passive-fit",
                "reference_binding": "reference_{}_fit".format(target),
                "resource_id": "model.final",
                "split": "fit",
                "weight": 1.0,
                "scale": 3.0,
            },
            {
                "id": "check_passive_decay",
                "kind": "passive-decay",
                "case": target + "-passive-check",
                "reference_binding": "reference_{}_check".format(target),
                "resource_id": "model.final",
                "split": "check",
                "weight": 1.0,
                "scale": 3.0,
            },
        ],
        "selection": {
            "check_weight": 1.0,
            "max_check_loss_increase": 0.0,
            "max_candidate_worst_harm": 3.0,
        },
    }
    if not exact_tree(value, expected):
        raise AdapterError(
            "fit manifest differs from the passive {} contract".format(target)
        )


def parameter_rows(target: str = "c3",
                   levels: Iterable[float] = ()) -> list[dict[str, Any]]:
    result = []
    manifest = load_json(STRING_TARGETS[target]["fit_manifest"])
    validate_fit_manifest(manifest, target)
    for row in manifest["parameters"]:
        result.append({
            "id": row["id"],
            "unit": row["unit"],
            "minimum": row["minimum"],
            "maximum": row["maximum"],
            "baseline": row["baseline"],
            "levels": list(levels),
        })
    return sorted(result, key=lambda row: row["id"])


def stem(resource_id: str, side: str, input_id: Optional[str],
         output: Optional[str], channels: int) -> dict[str, Any]:
    return {
        "id": resource_id,
        "side": side,
        "role": "final",
        "input_id": input_id,
        "output": output,
        "start_sample": 0,
        "gain_db": 0,
        "rate_hz": 44100,
        "channels": channels,
    }


def build_experiment(references: dict[str, Path], sample_count: int,
                     target: str = "c3") -> dict[str, Any]:
    if type(sample_count) is not int or not 1 <= sample_count <= 256:
        raise AdapterError("sample count must be 1..256")
    if target not in STRING_TARGETS:
        raise AdapterError("unknown viola fit target")
    expected = {
        "reference_{}_fit".format(target),
        "reference_{}_check".format(target),
    }
    if set(references) != expected:
        raise AdapterError(
            "reference set must contain the target fit and check recordings"
        )
    checked = {name: regular(Path(path), name)
               for name, path in references.items()}
    facts = {name: wave_facts(path, name) for name, path in checked.items()}
    hashes = {name: sha256(path) for name, path in checked.items()}
    if len(set(hashes.values())) != 2:
        raise AdapterError("fit and check recordings must have distinct hashes")

    cases = []
    for case_id, split, binding in (
        (target + "-passive-check", "check",
         "reference_{}_check".format(target)),
        (target + "-passive-fit", "fit",
         "reference_{}_fit".format(target)),
    ):
        cases.append({
            "id": case_id,
            "split": split,
            "weight": 1,
            "stems": [
                stem("model.final", "model", None, "model.wav", 2),
                stem("reference.final", "reference", binding, None,
                     facts[binding][1]),
            ],
            "probes": [],
            "links": [],
        })
    responses = [
        {"id": "final.rms", "role": "final",
         "feature": "rms_dbfs", "index": 0},
        {"id": "final.band.120-250", "role": "final",
         "feature": "band_level_dbfs", "index": 2},
        {"id": "final.band.250-500", "role": "final",
         "feature": "band_level_dbfs", "index": 3},
        {"id": "final.band.500-1000", "role": "final",
         "feature": "band_level_dbfs", "index": 4},
    ]
    return {
        "schema": "hwa-experiment",
        "schema_version": 1,
        "method_version": METHOD_VERSION,
        "clock_rate_hz": 44100,
        "inputs": [
            {"id": name, "sha256": hashes[name]} for name in sorted(checked)
        ],
        "parameters": parameter_rows(target),
        "plan": {
            "kind": "random",
            "seed": 29042016,
            "sample_count": sample_count,
            "replicates": 1,
        },
        "cases": cases,
        "responses": sorted(responses, key=lambda row: row["id"]),
    }


def build_roster_experiment(roster: dict[str, Any]) -> dict[str, Any]:
    cases = []
    for row in roster["cases"]:
        cases.append({
            "id": row["id"],
            "split": row["split"],
            "weight": row["weight"],
            "stems": [
                stem("model.final", "model", None, "model.wav", 2),
                stem("reference.final", "reference", row["id"], None,
                     row["channels"]),
            ],
            "probes": [],
            "links": [],
        })
    responses = [
        {"id": "final.rms", "role": "final",
         "feature": "rms_dbfs", "index": 0},
        {"id": "final.band.120-250", "role": "final",
         "feature": "band_level_dbfs", "index": 2},
        {"id": "final.band.250-500", "role": "final",
         "feature": "band_level_dbfs", "index": 3},
        {"id": "final.band.500-1000", "role": "final",
         "feature": "band_level_dbfs", "index": 4},
    ]
    return {
        "schema": "hwa-experiment",
        "schema_version": 1,
        "method_version": METHOD_VERSION,
        "clock_rate_hz": 44100,
        "inputs": [
            {"id": row["id"], "sha256": row["sha256"]}
            for row in roster["cases"]
        ],
        "parameters": parameter_rows(roster["target"], PASSIVE_SEARCH_LEVELS),
        "plan": {
            "kind": "grid",
            "seed": 29042016,
            "sample_count": 0,
            "replicates": 1,
        },
        "cases": cases,
        "responses": sorted(responses, key=lambda row: row["id"]),
    }


def build_joint_diagnostic_experiment(
        roster: dict[str, Any]) -> dict[str, Any]:
    experiment = build_roster_experiment(roster)
    experiment["parameters"] = joint_diagnostic_parameters(roster["target"])
    return experiment


def build_roster_fit_manifest(roster: dict[str, Any]) -> dict[str, Any]:
    target = roster["target"]
    return {
        "schema": "hwa-instrument-fit",
        "schema_version": 1,
        "adapter_id": STRING_TARGETS[target]["adapter_id"],
        "parameters": [passive_parameter(target)],
        "objectives": [{
            "id": row["id"] + "-passive-decay",
            "kind": "passive-decay",
            "case": row["id"],
            "reference_binding": row["id"],
            "resource_id": "model.final",
            "split": row["split"],
            "weight": row["weight"],
            "scale": 3.0,
        } for row in roster["cases"]],
        "selection": {
            "check_weight": 0.5,
            "max_candidate_loss": 2.0,
            "max_check_loss_increase": 0.0,
            "max_candidate_worst_harm": 3.0,
            "maximum_candidate_t60_ratio": 2.0,
            "minimum_candidate_support_ratio": 0.5,
            "minimum_candidate_t60_ratio": 0.5,
        },
    }


def validator_files() -> tuple[Path, Path]:
    if FROZEN_CONFIG is not None:
        files = FROZEN_CONFIG["files"]
        return (checked_file(files["generator"], "generator"),
                checked_file(files["schema"], "schema"))
    return (VIOLA_ROOT / "tools" / "generate_model.py",
            VIOLA_ROOT / "model" / "schema" / "viola-model-v1.schema.json")


def checked_file(row: dict[str, str], name: str) -> Path:
    path = regular(Path(row["path"]), name)
    if sha256(path) != row["sha256"]:
        raise AdapterError("configured resource changed: " + name)
    return path


def header_inventory(path: Path) -> list[dict[str, str]]:
    path = real_directory(path, "Csound include directory")
    rows = []
    for child in sorted(path.rglob("*")):
        if child.is_symlink():
            raise AdapterError("Csound include directory contains a link: {}".format(child))
        if child.is_file() and child.suffix.lower() in (".h", ".hpp"):
            rows.append({
                "name": child.relative_to(path).as_posix(),
                "sha256": sha256(child),
            })
    names = {row["name"] for row in rows}
    for required in ("csdl.h", "version.h", "float-version.h"):
        if required not in names:
            raise AdapterError("Csound include directory lacks " + required)
    try:
        version_text = (path / "version.h").read_text(encoding="ascii")
    except (OSError, UnicodeError) as error:
        raise AdapterError("cannot read Csound version header") from error
    version = re.search(
        r"^\s*#\s*define\s+CS_VERSION\s+\(?\s*([0-9]+)",
        version_text, re.MULTILINE,
    )
    if version is None or int(version.group(1)) < 7:
        raise AdapterError("viola fit requires Csound 7 headers")
    return rows


def checked_include_directories() -> list[Path]:
    if FROZEN_CONFIG is None:
        raise AdapterError("use a generated renderer")
    result = []
    for index, row in enumerate(FROZEN_CONFIG["include_dirs"]):
        path = real_directory(Path(row["path"]),
                              "Csound include directory {}".format(index + 1))
        if header_inventory(path) != row["headers"]:
            raise AdapterError(
                "configured resource changed: csound_headers_{}".format(index + 1)
            )
        result.append(path)
    return result


def roster_renderer_mode(mode: Any) -> bool:
    return mode in ("selection-roster", JOINT_DIAGNOSTIC_MODE)


def diagnostic_module_path() -> Path:
    if FROZEN_CONFIG is None:
        raise AdapterError("use a generated renderer")
    row = FROZEN_CONFIG.get("diagnostic")
    if type(row) is not dict:
        raise AdapterError("invalid frozen viola diagnostic configuration")
    bundle_text = row.get("bundle_path")
    name = row.get("module_name")
    module_text = row.get("module_path")
    digest = row.get("module_sha256")
    suffix = ".dylib" if sys.platform == "darwin" else ".so"
    if (type(bundle_text) is not str or type(module_text) is not str or
            not exact_value(name, JOINT_DIAGNOSTIC_MODULE + suffix) or
            type(digest) is not str or
            re.fullmatch(r"[0-9a-f]{64}", digest) is None):
        raise AdapterError("invalid frozen viola diagnostic module")
    bundle = real_directory(Path(bundle_text), "diagnostic bundle")
    path = regular(Path(module_text), "diagnostic module")
    if (str(bundle.resolve()) != bundle_text or
            str(path.resolve()) != module_text or
            path.parent != bundle or path.name != name):
        raise AdapterError("invalid frozen viola diagnostic module path")
    if sha256(path) != digest:
        raise AdapterError("configured resource changed: diagnostic_module")
    return path


def joint_diagnostic_model_facts(model: Any, target: str) -> dict[str, float]:
    spec = STRING_TARGETS.get(target)
    strings = model.get("strings") if type(model) is dict else None
    index = spec["profile_index"] if spec is not None else -1
    if (type(strings) is not list or index < 0 or index >= len(strings) or
            type(strings[index]) is not dict):
        raise AdapterError("fixed viola model lacks joint diagnostic data")
    profile = strings[index]
    result = {}
    for name in ("nut_cutoff_hz", "bridge_cutoff_hz",
                 "nut_loss_fraction"):
        value = profile.get(name)
        if type(value) not in (int, float) or not math.isfinite(float(value)):
            raise AdapterError("fixed viola model has invalid " + name)
        result[name] = float(value)
    if (result["nut_cutoff_hz"] <= 0.0 or
            result["bridge_cutoff_hz"] <= 0.0 or
            result["nut_loss_fraction"] < 0.0 or
            result["nut_loss_fraction"] > 1.0):
        raise AdapterError("fixed viola model has invalid joint diagnostic data")
    return result


def validate_frozen_target_config() -> None:
    if type(FROZEN_CONFIG) is not dict:
        raise AdapterError("invalid frozen renderer configuration")
    target = FROZEN_CONFIG.get("target")
    spec = STRING_TARGETS.get(target) if type(target) is str else None
    mode = FROZEN_CONFIG.get("mode", "smoke")
    expected_adapter_id = (joint_diagnostic_adapter_id(target)
                           if spec is not None and
                           mode == JOINT_DIAGNOSTIC_MODE
                           else spec["adapter_id"] if spec is not None
                           else None)
    expected_parameters = (joint_diagnostic_parameters(target)
                           if spec is not None and
                           mode == JOINT_DIAGNOSTIC_MODE
                           else [passive_parameter(target)]
                           if spec is not None else None)
    if (spec is None or
            FROZEN_CONFIG.get("adapter_id") != expected_adapter_id or
            FROZEN_CONFIG.get("method_version") != METHOD_VERSION or
            not exact_tree(FROZEN_CONFIG.get("parameters"),
                           expected_parameters)):
        raise AdapterError("invalid frozen viola target configuration")
    if mode not in ("smoke", "legacy", "selection-roster",
                    JOINT_DIAGNOSTIC_MODE):
        raise AdapterError("invalid frozen viola renderer mode")
    if mode == JOINT_DIAGNOSTIC_MODE:
        row = FROZEN_CONFIG.get("diagnostic")
        if (type(row) is not dict or set(row) != {
                "bundle_path", "method_version", "module_name",
                "module_path", "module_sha256", "render_frames",
                "total_seconds", "model_facts"} or
                not exact_value(row["method_version"],
                                JOINT_DIAGNOSTIC_METHOD) or
                not exact_value(row["render_frames"],
                                JOINT_DIAGNOSTIC_FRAMES) or
                not exact_value(row["total_seconds"], 16)):
            raise AdapterError("invalid frozen viola diagnostic configuration")
        suffix = ".dylib" if sys.platform == "darwin" else ".so"
        bundle_text = row["bundle_path"]
        module_text = row["module_path"]
        bundle = (Path(bundle_text)
                  if type(bundle_text) is str else Path("."))
        module = (Path(module_text)
                  if type(module_text) is str else Path("."))
        if (type(bundle_text) is not str or
                not bundle.is_absolute() or
                str(bundle.resolve()) != bundle_text or
                type(module_text) is not str or
                not module.is_absolute() or
                str(module.resolve()) != module_text or
                module.parent != bundle or
                not exact_value(row["module_name"],
                                JOINT_DIAGNOSTIC_MODULE + suffix) or
                module.name != row["module_name"] or
                type(row["module_sha256"]) is not str or
                re.fullmatch(r"[0-9a-f]{64}",
                             row["module_sha256"]) is None):
            raise AdapterError("invalid frozen viola diagnostic module")
        facts = row["model_facts"]
        if (type(facts) is not dict or set(facts) != {
                "nut_cutoff_hz", "bridge_cutoff_hz",
                "nut_loss_fraction"}):
            raise AdapterError("invalid frozen viola diagnostic model facts")
        for name in facts:
            value = facts[name]
            if type(value) is not float or not math.isfinite(value):
                raise AdapterError(
                    "invalid frozen viola diagnostic model facts"
                )
        if (facts["nut_cutoff_hz"] <= 0.0 or
                facts["bridge_cutoff_hz"] <= 0.0 or
                facts["nut_loss_fraction"] < 0.0 or
                facts["nut_loss_fraction"] > 1.0):
            raise AdapterError("invalid frozen viola diagnostic model facts")
    elif "diagnostic" in FROZEN_CONFIG:
        raise AdapterError("invalid frozen viola diagnostic configuration")
    if roster_renderer_mode(mode):
        cases = FROZEN_CONFIG.get("cases")
        if type(cases) is not dict or not cases:
            raise AdapterError("invalid frozen viola roster case table")
        family_counts = {name: 0 for name in ROSTER_SOURCE_RULES}
        check_family = ROSTER_CHECK_FAMILIES[target]
        hashes = set()
        for case_id, row in cases.items():
            roster_case_id(case_id, "frozen roster case id")
            if type(row) is not dict or set(row) != {
                    "a4_hz", "frequency_hz", "reference", "split",
                    "string", "reference_channels", "reference_sha256",
                    "reference_path", "source_family", "weight"}:
                raise AdapterError("invalid frozen viola roster case")
            family = row["source_family"]
            rule = (ROSTER_SOURCE_RULES.get(family)
                    if type(family) is str else None)
            if (rule is None or
                    (rule["split"] == "check" and
                     family != check_family) or
                    row["reference"] != case_id or
                    not exact_value(row["split"], rule["split"])):
                raise AdapterError("frozen viola roster case facts changed")
            reference_path = row["reference_path"]
            if (type(reference_path) is not str or
                    not Path(reference_path).is_absolute() or
                    str(Path(reference_path).resolve()) != reference_path):
                raise AdapterError("invalid frozen viola roster path")
            family_counts[family] += 1
            digest = row["reference_sha256"]
            if (type(digest) is not str or
                    re.fullmatch(r"[0-9a-f]{64}", digest) is None or
                    digest in hashes):
                raise AdapterError("invalid frozen viola roster audio hash")
            hashes.add(digest)
            try:
                render = render_facts(
                    target, row["frequency_hz"], case_id + " frozen"
                )
            except AdapterError as error:
                raise AdapterError("invalid frozen viola tuning facts") from error
            if (not exact_value(row["a4_hz"], render["a4_hz"]) or
                    not exact_value(row["string"], render["string"]) or
                    type(row["reference_channels"]) is not int or
                    row["reference_channels"] not in (1, 2) or
                    type(row["frequency_hz"]) is not float or
                    type(row["a4_hz"]) is not float or
                    type(row["weight"]) is not float or
                    not math.isfinite(row["weight"]) or row["weight"] <= 0.0):
                raise AdapterError("invalid frozen viola roster case facts")
        required_families = (
            "iowa-2012", "rwc-variation-1", check_family,
        )
        if any(family_counts[family] == 0
               for family in required_families):
            raise AdapterError("frozen viola roster lacks a source family")
        for family, expected_count in ROSTER_FIT_COUNTS[target].items():
            if family_counts[family] != expected_count:
                raise AdapterError("frozen viola roster fit-tail count changed")
        check_count = family_counts[check_family]
        if ((target == "g3" and check_count != 1) or
                (target != "g3" and not 1 <= check_count <= 3)):
            raise AdapterError("frozen viola roster check-tail count changed")
        for row in cases.values():
            expected_weight = (
                ROSTER_SOURCE_RULES[row["source_family"]]["total_weight"] /
                family_counts[row["source_family"]]
            )
            if not exact_value(row["weight"], expected_weight):
                raise AdapterError("frozen viola roster weight changed")
        return
    expected_cases = {
        target + "-passive-" + split: {
            "reference": "reference_{}_{}".format(target, split),
            "split": split,
            "string": spec["render_string"],
        }
        for split in ("check", "fit")
    }
    cases = FROZEN_CONFIG.get("cases")
    if type(cases) is not dict or set(cases) != set(expected_cases):
        raise AdapterError("invalid frozen viola case table")
    for case_id, fixed in expected_cases.items():
        row = cases[case_id]
        if type(row) is not dict or set(row) != {
                "a4_hz", "frequency_hz", "reference", "split", "string",
                "reference_channels", "reference_sha256"}:
            raise AdapterError("invalid frozen viola case")
        if any(not exact_value(row[key], value)
               for key, value in fixed.items()):
            raise AdapterError("frozen viola case facts changed")
        try:
            render = render_facts(
                target, row["frequency_hz"], case_id + " frozen"
            )
        except AdapterError as error:
            raise AdapterError("invalid frozen viola tuning facts") from error
        if (not exact_value(row["a4_hz"], render["a4_hz"]) or
                not exact_value(row["string"], render["string"])):
            raise AdapterError("frozen viola tuning facts changed")
        if (type(row["reference_channels"]) is not int or
                row["reference_channels"] not in (1, 2) or
                type(row["frequency_hz"]) is not float or
                type(row["a4_hz"]) is not float or
                type(row["reference_sha256"]) is not str or
                re.fullmatch(r"[0-9a-f]{64}", row["reference_sha256"]) is None):
            raise AdapterError("invalid frozen viola reference facts")


def verify_frozen_config() -> None:
    if FROZEN_CONFIG is None:
        raise AdapterError("use a generated renderer")
    validate_frozen_target_config()
    for name, row in sorted(FROZEN_CONFIG["files"].items()):
        path = checked_file(row, name)
        if name in ("c_compiler", "csound", "python") and not os.access(path, os.X_OK):
            raise AdapterError("configured resource is not executable: " + name)
    library = checked_file(FROZEN_CONFIG["files"]["csound_library"],
                           "csound_library")
    if str(library.resolve()) != FROZEN_CONFIG["csound_library_realpath"]:
        raise AdapterError("configured Csound core library moved")
    checked_include_directories()
    if FROZEN_CONFIG.get("mode") == JOINT_DIAGNOSTIC_MODE:
        diagnostic_module_path()


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def model_path_value(root: Any, parts: list[Any]) -> tuple[Any, Any]:
    current = root
    for part in parts[:-1]:
        if type(part) is int:
            if type(current) is not list or part < 0 or part >= len(current):
                raise AdapterError("model path index is out of range")
        elif type(part) is str:
            if type(current) is not dict or part not in current:
                raise AdapterError("model path key is missing")
        else:
            raise AdapterError("model path part is invalid")
        current = current[part]
    final = parts[-1]
    if ((type(final) is int and
         (type(current) is not list or final < 0 or final >= len(current))) or
        (type(final) is str and
         (type(current) is not dict or final not in current))):
        raise AdapterError("model path target is missing")
    return current, final


def build_renderer(config: dict[str, Any], output: Path) -> None:
    source = Path(__file__).read_text(encoding="utf-8")
    marker = "FROZEN_CONFIG = None"
    lines = source.splitlines(keepends=True)
    matches = [index for index, line in enumerate(lines)
               if line.rstrip("\r\n") == marker]
    if len(matches) != 1:
        raise AdapterError("renderer source marker changed")
    if not lines or not lines[0].startswith("#!"):
        raise AdapterError("renderer source needs an interpreter line")
    ending = "\n" if lines[matches[0]].endswith("\n") else ""
    lines[matches[0]] = "FROZEN_CONFIG = " + repr(config) + ending
    python_path = config["files"]["python"]["path"]
    lines[0] = (
        "#!/bin/sh\n\"\"\":\"\n"
        "TMPDIR=${{TMPDIR:-/tmp}}\nexport TMPDIR\n"
        "exec {} \"$0\" \"$@\"\n\":\"\"\"\n".format(
            shlex.quote(python_path)
        )
    )
    output.write_text("".join(lines), encoding="utf-8")
    output.chmod(0o700)


def build_target(arguments: argparse.Namespace
                 ) -> tuple[str, Path, Path, float, float]:
    legacy = (
        getattr(arguments, "reference_c3_fit", None),
        getattr(arguments, "reference_c3_check", None),
    )
    generic = (
        getattr(arguments, "reference_fit", None),
        getattr(arguments, "reference_check", None),
    )
    target = getattr(arguments, "target", None)
    fit_frequency = getattr(arguments, "fit_frequency_hz", None)
    check_frequency = getattr(arguments, "check_frequency_hz", None)
    if any(legacy) and any(generic):
        raise AdapterError("legacy and target reference options cannot be mixed")
    if any(legacy):
        if not all(legacy):
            raise AdapterError("both legacy C3 references are required")
        if target not in (None, "c3"):
            raise AdapterError("legacy reference options only support C3")
        if fit_frequency is not None or check_frequency is not None:
            raise AdapterError("legacy C3 references use their fixed tuning")
        fixed = STRING_TARGETS["c3"]["measured_reference_hz"]
        return "c3", Path(legacy[0]), Path(legacy[1]), fixed, fixed
    if not all(generic):
        raise AdapterError("--reference-fit and --reference-check are required")
    selected = target or "c3"
    if selected not in STRING_TARGETS:
        raise AdapterError("unknown viola fit target")
    if fit_frequency is None or check_frequency is None:
        raise AdapterError(
            "--fit-frequency-hz and --check-frequency-hz are required"
        )
    fit_render = render_facts(selected, fit_frequency, "fit reference")
    check_render = render_facts(selected, check_frequency, "check reference")
    return (selected, Path(generic[0]), Path(generic[1]),
            fit_render["frequency_hz"], check_render["frequency_hz"])


def build(arguments: argparse.Namespace) -> None:
    if sys.platform != "darwin" and not sys.platform.startswith("linux"):
        raise AdapterError("the viola fit builder supports Darwin and Linux")
    output = Path(arguments.output_dir).absolute()
    if output.exists() or output.is_symlink() or not output.parent.is_dir():
        raise AdapterError("output directory must be new with an existing parent")
    build_parent = output.parent.resolve()
    build_parent_stat = build_parent.stat()
    build_parent_identity = (
        build_parent_stat.st_dev, build_parent_stat.st_ino,
    )
    viola_root = real_directory(Path(arguments.viola_root), "viola root")
    roster_argument = getattr(arguments, "roster", None)
    joint_diagnostic = bool(
        getattr(arguments, "joint_passive_diagnostic", False)
    )
    published_argument = getattr(arguments, "published_output_dir", None)
    if published_argument is not None and not joint_diagnostic:
        raise AdapterError(
            "--published-output-dir requires --joint-passive-diagnostic"
        )
    if joint_diagnostic and roster_argument is None:
        raise AdapterError("--joint-passive-diagnostic requires --roster")
    build_output = output.parent.resolve() / output.name
    published_output = build_output
    if joint_diagnostic and published_argument is not None:
        candidate = Path(published_argument)
        if not candidate.is_absolute():
            raise AdapterError(
                "published output directory must be an absolute path"
            )
        published_output = candidate
    if joint_diagnostic:
        if (str(published_output.resolve()) != str(published_output) or
                published_output.name != output.name or
                published_output.exists() or published_output.is_symlink()):
            raise AdapterError(
                "published output directory must be a new canonical path "
                "with the build output name"
            )
        if any(under(published_output, root)
               for root in (ROOT, viola_root)):
            raise AdapterError(
                "published diagnostic bundle must stay outside the repositories"
            )
        if (published_output != build_output and
                under(published_output, build_output)):
            raise AdapterError(
                "published output directory cannot be nested under build output"
            )
    if (roster_argument is not None and
            any(under(output, root) for root in (ROOT, viola_root))):
        raise AdapterError(
            "selection bundle output must stay outside the repositories"
        )
    selection_roster = None
    if roster_argument is not None:
        if getattr(arguments, "sample_count", None) is not None:
            raise AdapterError("--sample-count cannot be used with --roster")
        single_options = (
            getattr(arguments, "target", None),
            getattr(arguments, "reference_fit", None),
            getattr(arguments, "reference_check", None),
            getattr(arguments, "reference_c3_fit", None),
            getattr(arguments, "reference_c3_check", None),
            getattr(arguments, "fit_frequency_hz", None),
            getattr(arguments, "check_frequency_hz", None),
        )
        if any(value is not None for value in single_options):
            raise AdapterError(
                "--roster cannot be mixed with single-reference options"
            )
        selection_roster = load_selection_roster(
            Path(roster_argument), (ROOT, viola_root)
        )
        target = selection_roster["target"]
        mode = (JOINT_DIAGNOSTIC_MODE if joint_diagnostic
                else "selection-roster")
        render = {
            row["id"]: {
                "a4_hz": row["a4_hz"],
                "frequency_hz": row["frequency_hz"],
                "string": row["string"],
            }
            for row in selection_roster["cases"]
        }
    else:
        legacy_mode = any((
            getattr(arguments, "reference_c3_fit", None),
            getattr(arguments, "reference_c3_check", None),
        ))
        (target, fit_reference, check_reference, fit_frequency_hz,
         check_frequency_hz) = build_target(arguments)
        mode = "legacy" if legacy_mode else "smoke"
        render = {
            "fit": render_facts(target, fit_frequency_hz, "fit reference"),
            "check": render_facts(
                target, check_frequency_hz, "check reference"
            ),
        }
    target_spec = STRING_TARGETS[target]
    adapter_id = (joint_diagnostic_adapter_id(target)
                  if joint_diagnostic else target_spec["adapter_id"])
    files = {
        "c_compiler": executable(Path(arguments.c_compiler), "C compiler"),
        "csound": executable(Path(arguments.csound), "Csound"),
        "csound_library": regular(Path(arguments.csound_library),
                                   "Csound core library"),
        "csd": regular(
            viola_root / "tests" /
            ("fit_passive_joint.csd" if joint_diagnostic
             else "fit_passive.csd"),
            "passive fit CSD",
        ),
        "generator": regular(viola_root / "tools" / "generate_model.py",
                             "model generator"),
        "model": regular(viola_root / "model" / "viola-v1.json", "viola model"),
        "python": executable(Path(arguments.python), "Python"),
        "schema": regular(
            viola_root / "model" / "schema" / "viola-model-v1.schema.json",
            "viola model schema",
        ),
        "source": regular(viola_root / "src" / "hlolli_wg_viola.c",
                          "viola source"),
    }
    if b"hlolli_wg_viola_test_string_impulse" not in files["source"].read_bytes():
        raise AdapterError("viola fit needs the private string impulse opcode")
    if (joint_diagnostic and
            b"hlolli_wg_viola_test_passive_joint" not in
            files["source"].read_bytes()):
        raise AdapterError(
            "joint diagnostic needs the private passive control opcode"
        )
    include_paths = [real_directory(Path(path), "Csound include directory")
                     for path in arguments.csound_include_dir]
    if not include_paths:
        raise AdapterError("at least one Csound include directory is required")
    include_rows = [
        {"path": str(path), "headers": header_inventory(path)}
        for path in include_paths
    ]
    with tempfile.TemporaryDirectory(
            prefix=".hwa-viola-generator-test-", dir=output.parent) as text:
        test_environment = {
            "LC_ALL": "C", "LANG": "C", "TZ": "UTC",
            "PATH": CLEAN_PATH, "TMPDIR": text,
            "TMP": text, "TEMP": text,
        }
        run_tool([
            str(files["python"]), "-B", str(files["generator"]),
            "--model", str(files["model"]),
            "--schema", str(files["schema"]),
            "--source", str(files["source"]),
            "--self-test",
        ], "fixed model generator self-test", viola_root, test_environment)
        version = run_tool(
            [str(files["csound"]), "--version"], "Csound version check",
            viola_root, test_environment,
        )
        version_text = version.stdout + "\n" + version.stderr
        if re.search(r"Csound version 7(?:\.|\s)", version_text) is None:
            raise AdapterError("viola fit requires a Csound 7 executable")
        verify_csound_library(
            files["csound"], files["csound_library"], viola_root,
            test_environment,
        )
    if selection_roster is not None:
        references = {
            row["id"]: row["path"] for row in selection_roster["cases"]
        }
        experiment = (build_joint_diagnostic_experiment(selection_roster)
                      if joint_diagnostic
                      else build_roster_experiment(selection_roster))
        reference_hashes = {
            row["id"]: row["sha256"] for row in selection_roster["cases"]
        }
        reference_facts = {
            row["id"]: (row["rate_hz"], row["channels"], row["frames"])
            for row in selection_roster["cases"]
        }
        manifest = (None if joint_diagnostic
                    else build_roster_fit_manifest(selection_roster))
        fit_manifest = None
        config_cases = {
            row["id"]: {
                "reference": row["id"],
                "string": row["string"],
                "frequency_hz": row["frequency_hz"],
                "a4_hz": row["a4_hz"],
                "split": row["split"],
                "reference_channels": row["channels"],
                "reference_sha256": row["sha256"],
                "reference_path": str(row["path"]),
                "source_family": row["source_family"],
                "weight": row["weight"],
            }
            for row in selection_roster["cases"]
        }
    else:
        fit_binding = "reference_{}_fit".format(target)
        check_binding = "reference_{}_check".format(target)
        references = {
            fit_binding: regular(fit_reference, "target fit reference"),
            check_binding: regular(check_reference, "target check reference"),
        }
        sample_count = getattr(arguments, "sample_count", None)
        if sample_count is None:
            sample_count = 12
        experiment = build_experiment(references, sample_count, target)
        reference_hashes = {row["id"]: row["sha256"]
                            for row in experiment["inputs"]}
        reference_facts = {name: wave_facts(path, name)
                           for name, path in references.items()}
        fit_manifest = target_spec["fit_manifest"]
        manifest = load_json(fit_manifest)
        validate_fit_manifest(manifest, target)
        config_cases = {
            target + "-passive-check": {
                "reference": check_binding,
                "string": render["check"]["string"],
                "frequency_hz": render["check"]["frequency_hz"],
                "a4_hz": render["check"]["a4_hz"],
                "split": "check",
                "reference_channels": reference_facts[check_binding][1],
                "reference_sha256": reference_hashes[check_binding],
            },
            target + "-passive-fit": {
                "reference": fit_binding,
                "string": render["fit"]["string"],
                "frequency_hz": render["fit"]["frequency_hz"],
                "a4_hz": render["fit"]["a4_hz"],
                "split": "fit",
                "reference_channels": reference_facts[fit_binding][1],
                "reference_sha256": reference_hashes[fit_binding],
            },
        }
    for name, path in references.items():
        if sha256(path) != reference_hashes[name]:
            raise AdapterError("reference changed while building: " + name)
    model = load_json(files["model"])
    diagnostic_model_facts = (
        joint_diagnostic_model_facts(model, target)
        if joint_diagnostic else None
    )
    baseline_parameters = ([passive_parameter(target)] if joint_diagnostic
                           else manifest["parameters"])
    for row in baseline_parameters:
        if len(row["profile_paths"]) != 1:
            raise AdapterError("each passive fit parameter needs one model path")
        parent, key = model_path_value(model, row["profile_paths"][0])
        if float(parent[key]) != float(row["baseline"]):
            raise AdapterError("fit baseline differs from the fixed viola model")
    file_rows = {
        name: {"path": str(path), "sha256": sha256(path)}
        for name, path in sorted(files.items())
    }
    config = {
        "adapter_id": adapter_id,
        "mode": mode,
        "target": target,
        "method_version": METHOD_VERSION,
        "files": file_rows,
        "include_dirs": include_rows,
        "platform": sys.platform,
        "csound_library_realpath": str(files["csound_library"].resolve()),
        "parameters": (joint_diagnostic_parameters(target)
                       if joint_diagnostic else manifest["parameters"]),
        "cases": config_cases,
    }
    adapter_source_hash = sha256(Path(__file__))
    fit_manifest_source_hash = (
        sha256(fit_manifest) if fit_manifest is not None else None
    )
    temporary = Path(tempfile.mkdtemp(prefix=".{}-".format(output.name),
                                      dir=output.parent))
    try:
        diagnostic_module = None
        if joint_diagnostic:
            suffix = ".dylib" if sys.platform == "darwin" else ".so"
            diagnostic_source = temporary / "hlolli_wg_viola.c"
            diagnostic_module = temporary / (JOINT_DIAGNOSTIC_MODULE + suffix)
            final_bundle = published_output
            final_module = final_bundle / diagnostic_module.name
            shutil.copyfile(files["source"], diagnostic_source)
            if (sha256(diagnostic_source) !=
                    config["files"]["source"]["sha256"]):
                raise AdapterError(
                    "viola source changed before diagnostic compile"
                )
            compile_environment = {
                "LC_ALL": "C", "LANG": "C", "TZ": "UTC",
                "PATH": CLEAN_PATH, "TMPDIR": str(temporary),
                "TMP": str(temporary), "TEMP": str(temporary),
            }
            compile_test_module(
                diagnostic_source, diagnostic_module, files["c_compiler"],
                include_paths, sys.platform, temporary, compile_environment,
            )
            diagnostic_source.unlink()
            config["diagnostic"] = {
                "bundle_path": str(final_bundle),
                "method_version": JOINT_DIAGNOSTIC_METHOD,
                "module_name": diagnostic_module.name,
                "module_path": str(final_module),
                "module_sha256": sha256(diagnostic_module),
                "render_frames": JOINT_DIAGNOSTIC_FRAMES,
                "total_seconds": 16,
                "model_facts": diagnostic_model_facts,
            }
        renderer = temporary / "renderer"
        build_renderer(config, renderer)
        write_json(temporary / "experiment.json", experiment)
        fit_output = None
        if selection_roster is not None and not joint_diagnostic:
            fit_output = temporary / "fit.json"
            write_json(fit_output, manifest)
            write_json(temporary / "roster.json",
                       selection_roster["normalized"])
        elif selection_roster is not None:
            write_json(temporary / "roster.json",
                       selection_roster["normalized"])
        else:
            fit_output = temporary / "fit.json"
            shutil.copyfile(fit_manifest, fit_output)
            if sha256(fit_output) != fit_manifest_source_hash:
                raise AdapterError("fit manifest changed while building")
        fit_manifest_hash = (sha256(fit_output)
                             if fit_output is not None else None)
        bindings = {
            "schema": "hwa-fit-bindings",
            "schema_version": 1,
            "bindings": [
                {"id": name, "path": str(path),
                 "sha256": reference_hashes[name]}
                for name, path in sorted(references.items())
            ],
        }
        write_json(temporary / "bindings.json", bindings)
        resource_rows = [
            {"id": name, "sha256": row["sha256"]}
            for name, row in sorted(config["files"].items())
        ]
        if diagnostic_module is not None:
            resource_rows.append({
                "id": "diagnostic_module",
                "path": config["diagnostic"]["module_path"],
                "sha256": config["diagnostic"]["module_sha256"],
            })
        for index, row in enumerate(include_rows):
            for header in row["headers"]:
                resource_rows.append({
                    "id": "csound_headers_{}.{}".format(index + 1,
                                                        header["name"]),
                    "sha256": header["sha256"],
                })
        if selection_roster is not None:
            reference_receipt = [{
                "id": row["id"],
                "source_family": row["source_family"],
                "split": row["split"],
                "path": str(row["path"]),
                "sha256": row["sha256"],
                "weight": row["weight"],
                "frequency_hz": row["frequency_hz"],
                "a4_hz": row["a4_hz"],
                "string": row["string"],
                "rate_hz": row["rate_hz"],
                "channels": row["channels"],
                "frames": row["frames"],
            } for row in selection_roster["cases"]]
            family_weights = [{
                "source_family": family,
                "split": rule["split"],
                "total_weight": rule["total_weight"],
                "case_count": sum(
                    row["source_family"] == family
                    for row in selection_roster["cases"]
                ),
            } for family, rule in sorted(ROSTER_SOURCE_RULES.items())
                if any(row["source_family"] == family
                       for row in selection_roster["cases"])]
        else:
            reference_receipt = [
                {"id": name, "sha256": reference_hashes[name]}
                for name, path in sorted(references.items())
            ]
            family_weights = []
        receipt = {
            "schema": ("hwa-viola-passive-joint-diagnostic-bundle"
                       if joint_diagnostic
                       else "hwa-viola-fit-adapter-bundle"),
            "schema_version": 1,
            "adapter_id": adapter_id,
            "mode": mode,
            "target": target,
            "render": render,
            "adapter_source_sha256": adapter_source_hash,
            "experiment_sha256": sha256(temporary / "experiment.json"),
            "bindings_sha256": sha256(temporary / "bindings.json"),
            "renderer_sha256": sha256(renderer),
            "resources": resource_rows,
            "references": reference_receipt,
        }
        if joint_diagnostic:
            receipt.update({
                "build_output_dir": str(build_output),
                "diagnostic_method_version": JOINT_DIAGNOSTIC_METHOD,
                "diagnostic_module_name":
                    config["diagnostic"]["module_name"],
                "diagnostic_module_path":
                    config["diagnostic"]["module_path"],
                "diagnostic_module_sha256":
                    config["diagnostic"]["module_sha256"],
                "diagnostic_model_facts": diagnostic_model_facts,
                "parameters": config["parameters"],
                "point_count": 84,
                "published_output_dir": str(published_output),
                "job_count": 84 * len(selection_roster["cases"]),
                "render_frames": JOINT_DIAGNOSTIC_FRAMES,
                "total_seconds": 16,
            })
        else:
            receipt["fit_manifest_sha256"] = fit_manifest_hash
        if selection_roster is not None:
            receipt.update({
                "roster_source_path": str(selection_roster["source_path"]),
                "roster_source_sha256": selection_roster["source_sha256"],
                "roster_sha256": sha256(temporary / "roster.json"),
                "source_family_weights": family_weights,
            })
        else:
            receipt["fit_manifest_source_sha256"] = fit_manifest_source_hash
        write_json(temporary / "receipt.json", receipt)
        if sha256(Path(__file__)) != adapter_source_hash:
            raise AdapterError("adapter source changed while building")
        if (fit_manifest is not None and
                sha256(fit_manifest) != fit_manifest_source_hash):
            raise AdapterError("fit manifest changed while building")
        for name, row in file_rows.items():
            checked_file(row, name)
        if (diagnostic_module is not None and
                sha256(diagnostic_module) !=
                config["diagnostic"]["module_sha256"]):
            raise AdapterError("diagnostic module changed while building")
        for index, row in enumerate(include_rows):
            if header_inventory(Path(row["path"])) != row["headers"]:
                raise AdapterError(
                    "Csound headers changed while building: {}".format(index + 1)
                )
        for name, path in references.items():
            if sha256(path) != reference_hashes[name]:
                raise AdapterError("reference changed while building: " + name)
        if (selection_roster is not None and
                sha256(selection_roster["source_path"]) !=
                selection_roster["source_sha256"]):
            raise AdapterError("selection roster changed before publish")
        try:
            current_build_parent = output.parent.resolve(strict=True)
            current_build_parent_stat = current_build_parent.stat()
        except OSError as error:
            raise AdapterError(
                "build output directory changed while building"
            ) from error
        current_build_parent_identity = (
            current_build_parent_stat.st_dev,
            current_build_parent_stat.st_ino,
        )
        if (current_build_parent != build_parent or
                current_build_parent_identity != build_parent_identity or
                output.exists() or output.is_symlink()):
            raise AdapterError(
                "build output directory changed while building"
            )
        if (joint_diagnostic and
                (str(published_output.resolve()) != str(published_output) or
                 published_output.exists() or published_output.is_symlink())):
            raise AdapterError(
                "published output directory changed while building"
            )
        temporary.rename(output)
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def describe() -> dict[str, Any]:
    verify_frozen_config()
    target = FROZEN_CONFIG["target"]
    mode = FROZEN_CONFIG.get("mode", "smoke")
    render = {}
    if roster_renderer_mode(mode):
        for case_id, case in sorted(FROZEN_CONFIG["cases"].items()):
            render[case_id] = {
                "a4_hz": case["a4_hz"],
                "frequency_hz": case["frequency_hz"],
                "string": case["string"],
                "source_family": case["source_family"],
                "split": case["split"],
                "weight": case["weight"],
            }
    else:
        for split in ("fit", "check"):
            case = FROZEN_CONFIG["cases"][target + "-passive-" + split]
            render[split] = {
                "a4_hz": case["a4_hz"],
                "frequency_hz": case["frequency_hz"],
                "string": case["string"],
            }
    resources = [
        {"id": name, "sha256": row["sha256"]}
        for name, row in sorted(FROZEN_CONFIG["files"].items())
    ]
    for index, row in enumerate(FROZEN_CONFIG["include_dirs"]):
        resources.extend({
            "id": "csound_headers_{}.{}".format(index + 1, header["name"]),
            "sha256": header["sha256"],
        } for header in row["headers"])
    if mode == JOINT_DIAGNOSTIC_MODE:
        resources.append({
            "id": "diagnostic_module",
            "path": FROZEN_CONFIG["diagnostic"]["module_path"],
            "sha256": FROZEN_CONFIG["diagnostic"]["module_sha256"],
        })
    return {
        "schema": "hwa-viola-renderer",
        "schema_version": 1,
        "adapter_id": FROZEN_CONFIG["adapter_id"],
        "mode": mode,
        "target": target,
        "render": render,
        "method_version": FROZEN_CONFIG["method_version"],
        "permissions": {
            "render": True,
            "validate_profile": mode != JOINT_DIAGNOSTIC_MODE,
            "write_profile": False,
        },
        "resources": resources,
    }


def validate_profile(path: Path) -> None:
    path = regular(Path(path), "profile")
    generator_path, schema_path = validator_files()
    generator_path = regular(generator_path, "generator")
    schema_path = regular(schema_path, "schema")
    spec = importlib.util.spec_from_file_location("viola_model_generator",
                                                  generator_path)
    if spec is None or spec.loader is None:
        raise AdapterError("cannot load viola model validator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.load_model(path, schema_path)


def request_values(request: dict[str, Any]) -> dict[str, float]:
    rows = request.get("parameters")
    if type(rows) is not list:
        raise AdapterError("render request has no parameter list")
    values: dict[str, float] = {}
    units = {row["id"]: row["unit"] for row in FROZEN_CONFIG["parameters"]}
    for row in rows:
        row = exact_object(row, ("id", "unit", "value"), "render parameter")
        if type(row["id"]) is not str:
            raise AdapterError("render request has an invalid parameter")
        name = row["id"]
        if name in values or name not in units or row["unit"] != units[name]:
            raise AdapterError("render request has an invalid parameter")
        value = finite(row["value"], "render parameter " + name)
        values[name] = value
    expected = {row["id"] for row in FROZEN_CONFIG["parameters"]}
    if set(values) != expected:
        raise AdapterError("render parameter set changed")
    for row in FROZEN_CONFIG["parameters"]:
        value = values[row["id"]]
        if value < float(row["minimum"]) or value > float(row["maximum"]):
            raise AdapterError("render parameter is out of range: " + row["id"])
        if (FROZEN_CONFIG.get("mode") == JOINT_DIAGNOSTIC_MODE and
                value not in row["levels"]):
            raise AdapterError(
                "render parameter is outside the fixed diagnostic grid: " +
                row["id"]
            )
    return values


def apply_values(model: dict[str, Any], values: dict[str, float]) -> None:
    for row in FROZEN_CONFIG["parameters"]:
        paths = row["profile_paths"]
        if type(paths) is not list or len(paths) != 1:
            raise AdapterError("render parameter has no single model path")
        parent, key = model_path_value(model, paths[0])
        parent[key] = values[row["id"]]


def run_tool(command: list[str], name: str, cwd: Path,
             environment: dict[str, str]):
    try:
        completed = subprocess.run(
            command, check=False, cwd=cwd, env=environment,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            timeout=600,
        )
    except subprocess.TimeoutExpired as error:
        raise AdapterError(name + " timed out") from error
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        if len(detail) > 2000:
            detail = detail[-2000:]
        if detail:
            raise AdapterError("{} failed: {}".format(name, detail))
        raise AdapterError(name + " failed")
    return completed


def validate_joint_diagnostic_facts(completed: Any,
                                    case: dict[str, Any],
                                    tau: float,
                                    cutoff_scale: float) -> None:
    marker = "WG_PASSIVE_JOINT_FACTS"
    rows = []
    output = completed.stdout + "\n" + completed.stderr
    for line in output.splitlines():
        offset = line.find(marker)
        if offset < 0:
            continue
        fields = line[offset:].split()
        if len(fields) != 10 or fields[0] != marker:
            raise AdapterError("joint diagnostic facts do not match")
        try:
            values = [float(value) for value in fields[1:10]]
        except ValueError as error:
            raise AdapterError(
                "joint diagnostic facts do not match"
            ) from error
        rows.append(values)
    if len(rows) != 1:
        raise AdapterError("joint diagnostic facts do not match")
    values = rows[0]
    expected = (
        float(case["string"]), float(case["frequency_hz"]),
        tau, cutoff_scale,
    )
    if (any(not math.isfinite(value) for value in values) or
            tuple(values[:4]) != expected or values[8] != 1.0):
        raise AdapterError("joint diagnostic facts do not match")
    model_facts = FROZEN_CONFIG["diagnostic"]["model_facts"]
    sample_rate = 44100.0
    cutoff_cap = 0.45 * sample_rate
    nut_cutoff = min(
        model_facts["nut_cutoff_hz"] * cutoff_scale, cutoff_cap
    )
    bridge_cutoff = min(
        model_facts["bridge_cutoff_hz"] * cutoff_scale, cutoff_cap
    )
    loop_gain = math.exp(-1.0 / (case["frequency_hz"] * tau))
    nut_fraction = model_facts["nut_loss_fraction"]
    expected_coefficients = (
        math.exp(-2.0 * math.pi * nut_cutoff / sample_rate),
        min(max(math.pow(loop_gain, nut_fraction), 0.0), 0.99995),
        math.exp(-2.0 * math.pi * bridge_cutoff / sample_rate),
        min(max(math.pow(loop_gain, 1.0 - nut_fraction), 0.0), 0.99995),
    )
    for actual, wanted in zip(values[4:8], expected_coefficients):
        tolerance = 2.0e-13 * max(1.0, abs(wanted))
        if abs(actual - wanted) > tolerance:
            raise AdapterError("joint diagnostic facts do not match")


def verify_csound_library(csound: Path, library: Path, cwd: Path,
                          environment: dict[str, str]) -> None:
    expected = library.resolve()
    if sys.platform == "darwin":
        traced_environment = dict(environment)
        traced_environment["DYLD_PRINT_LIBRARIES"] = "1"
        completed = run_tool(
            [str(csound), "--version"], "Csound core library check",
            cwd, traced_environment,
        )
        lines = (completed.stdout + "\n" + completed.stderr).splitlines()
        if not any(line.strip().endswith(str(expected)) for line in lines):
            raise AdapterError(
                "Csound did not load the named core library: {}".format(expected)
            )
        return
    if sys.platform.startswith("linux"):
        ldd = shutil.which("ldd", path=environment.get("PATH"))
        if ldd is None:
            raise AdapterError("cannot find ldd for the Csound core library check")
        completed = run_tool(
            [ldd, str(csound)], "Csound core library check", cwd, environment
        )
        found = set()
        for line in (completed.stdout + "\n" + completed.stderr).splitlines():
            text = line.strip()
            if "=>" in text:
                text = text.split("=>", 1)[1].strip()
            candidate = text.split(" ", 1)[0]
            if candidate.startswith("/"):
                found.add(Path(candidate).resolve())
        if expected not in found:
            raise AdapterError(
                "Csound did not load the named core library: {}".format(expected)
            )
        return
    raise AdapterError("cannot check the Csound core library on this platform")


def compile_test_module(source: Path, module: Path, compiler: Path,
                        include_dirs: Iterable[Path], platform_name: str,
                        scratch: Path,
                        environment: dict[str, str]) -> None:
    if platform_name != sys.platform:
        raise AdapterError("renderer platform changed")
    command = [
        str(compiler), "-DBUILD_PLUGINS", "-DHLOLLI_WG_VIOLA_TEST_API=1",
        "-std=c11", "-O2", "-fvisibility=hidden", "-Wall", "-Wextra",
        "-Wpedantic", "-Wconversion", "-Wformat=2", "-Wshadow", "-Werror",
    ]
    if platform_name == "darwin":
        command.extend(["-dynamiclib", "-fPIC"])
    elif platform_name.startswith("linux"):
        command.extend(["-shared", "-fPIC"])
    else:
        raise AdapterError("unsupported frozen renderer platform")
    for include_dir in include_dirs:
        command.extend(["-isystem", str(include_dir)])
    command.extend([str(source), "-o", str(module)])
    if platform_name != "darwin" and not platform_name.startswith("win"):
        command.append("-lm")
    run_tool(command, "candidate module compile", scratch, environment)
    regular(module, "candidate module")


def compile_candidate(source: Path, module: Path, scratch: Path,
                      environment: dict[str, str]) -> None:
    compiler = checked_file(FROZEN_CONFIG["files"]["c_compiler"], "c_compiler")
    include_dirs = checked_include_directories()
    platform_name = FROZEN_CONFIG["platform"]
    compile_test_module(source, module, compiler, include_dirs, platform_name,
                        scratch, environment)


def output_wave(path: Path) -> dict[str, Any]:
    path = regular(path, "model output")
    (rate, channels, frames, data_start, data_size,
     output_hash) = wave_evidence(path, "model output")
    expected_frames = (JOINT_DIAGNOSTIC_FRAMES
                       if FROZEN_CONFIG is not None and
                       FROZEN_CONFIG.get("mode") == JOINT_DIAGNOSTIC_MODE
                       else 176416)
    if channels != 2 or frames != expected_frames:
        raise AdapterError("model output has the wrong passive-tail duration")
    try:
        with path.open("rb") as stream:
            stream.seek(data_start, os.SEEK_SET)
            audio = stream.read(data_size)
    except OSError as error:
        raise AdapterError("cannot read model output: {}".format(error)) from error
    if len(audio) != frames * channels * 3:
        raise AdapterError("model output is truncated")
    nonzero_samples = 0
    clipped_samples = 0
    for index in range(0, len(audio), 3):
        value = (audio[index] | (audio[index + 1] << 8) |
                 (audio[index + 2] << 16))
        if value & 0x800000:
            value -= 0x1000000
        if value != 0:
            nonzero_samples += 1
        if value in (-8388608, 8388607):
            clipped_samples += 1
    if nonzero_samples == 0:
        raise AdapterError("model output is silent")
    if clipped_samples != 0:
        raise AdapterError("model output clips")
    return {
        "sha256": output_hash,
        "rate_hz": rate,
        "channels": channels,
        "frames": frames,
        "nonzero_samples": nonzero_samples,
        "clipped_samples": clipped_samples,
    }


def validate_render_request(request_path: Path, output_dir: Path,
                            request: dict[str, Any]
                            ) -> tuple[dict[str, Any], dict[str, Any], Path,
                                       Path, dict[str, float]]:
    request_path = regular(Path(request_path), "render request")
    output_dir = real_directory(Path(output_dir), "render output directory")
    if request_path.parent.resolve() != output_dir.resolve():
        raise AdapterError("render request must be inside the output directory")
    exact_object(request, (
        "schema", "schema_version", "method_version", "case_id", "job_id",
        "job_key", "inputs", "outputs", "parameters", "replicate", "seed",
        "split",
    ), "render request")
    if (not exact_value(request["schema"], "hwa-render-job") or
            not exact_value(request["schema_version"], 1) or
            not exact_value(request["method_version"], "stage8-1")):
        raise AdapterError("unsupported render request")
    case_id = request["case_id"]
    case = (FROZEN_CONFIG["cases"].get(case_id)
            if type(case_id) is str else None)
    if case is None or not exact_value(request["split"], case["split"]):
        raise AdapterError("unknown viola fit case or wrong split")
    unsigned(request["job_id"], "job_id", 1)
    unsigned(request["replicate"], "replicate")
    unsigned(request["seed"], "seed")
    if request["replicate"] != 0:
        raise AdapterError("viola fit uses one replicate")
    if (type(request["job_key"]) is not str or
            re.fullmatch(r"[0-9a-f]{64}", request["job_key"]) is None):
        raise AdapterError("job_key is invalid")

    inputs = request["inputs"]
    if type(inputs) is not list or len(inputs) != 1:
        raise AdapterError("viola renderer needs one reference input")
    input_row = exact_object(inputs[0], (
        "binding_id", "channels", "gain_db", "kind", "path",
        "probe_format", "probe_name", "rate_denominator", "rate_hz",
        "rate_numerator", "resource_id", "role", "sha256", "side",
        "start_sample", "unit", "value_count",
    ), "reference input")
    expected_input = {
        "binding_id": case["reference"],
        "channels": case["reference_channels"],
        "gain_db": 0,
        "kind": "stem",
        "probe_format": None,
        "probe_name": None,
        "rate_denominator": 0,
        "rate_hz": 44100,
        "rate_numerator": 0,
        "resource_id": "reference.final",
        "role": "final",
        "sha256": case["reference_sha256"],
        "side": "reference",
        "start_sample": 0,
        "unit": None,
        "value_count": 0,
    }
    for key, expected in expected_input.items():
        if not exact_value(input_row[key], expected):
            raise AdapterError("reference input has wrong " + key)
    input_text = input_row["path"]
    if type(input_text) is not str or not Path(input_text).is_absolute():
        raise AdapterError("reference input path must be absolute")
    reference = regular(Path(input_text), "reference input")
    if (roster_renderer_mode(FROZEN_CONFIG.get("mode")) and
            str(reference.resolve()) != case["reference_path"]):
        raise AdapterError("reference input has wrong path")
    (rate, channels, _, _, _,
     reference_hash) = wave_evidence(reference, "reference input")
    if reference_hash != case["reference_sha256"]:
        raise AdapterError("reference input hash changed")
    if rate != 44100 or channels != case["reference_channels"]:
        raise AdapterError("reference input facts changed")

    outputs = request["outputs"]
    if type(outputs) is not list or len(outputs) != 1:
        raise AdapterError("viola renderer needs one model output")
    output_row = exact_object(outputs[0], (
        "id", "kind", "path", "side", "role", "probe_format",
        "probe_name", "unit", "start_sample", "gain_db", "rate_hz",
        "channels", "rate_numerator", "rate_denominator", "value_count",
    ), "model output")
    expected_output = {
        "id": "model.final", "kind": "stem", "side": "model",
        "role": "final", "probe_format": None, "probe_name": None,
        "unit": None, "start_sample": 0, "gain_db": 0, "rate_hz": 44100,
        "channels": 2, "rate_numerator": 0, "rate_denominator": 0,
        "value_count": 0,
    }
    for key, expected in expected_output.items():
        if not exact_value(output_row[key], expected):
            raise AdapterError("model output has wrong " + key)
    output_text = output_row["path"]
    if type(output_text) is not str or not Path(output_text).is_absolute():
        raise AdapterError("renderer output path must be absolute")
    output = Path(output_text)
    if (output.parent.resolve() != output_dir.resolve() or
            output.name != "model.wav" or output.exists() or
            output.is_symlink()):
        raise AdapterError("output path must be output-directory/model.wav")
    values = request_values(request)
    return request, case, reference, output, values


def render_job(request_path: Path, output_dir: Path) -> None:
    verify_frozen_config()
    request_path = regular(Path(request_path), "render request")
    request, request_hash = load_json_evidence(request_path)
    output_dir = Path(output_dir)
    request, case, reference, output, values = validate_render_request(
        request_path, output_dir, request
    )
    if sha256(request_path) != request_hash:
        raise AdapterError("render request changed during validation")
    reference_hash = case["reference_sha256"]
    if sha256(reference) != reference_hash:
        raise AdapterError("reference input changed during validation")
    files = FROZEN_CONFIG["files"]
    environment = {
        "LC_ALL": "C", "LANG": "C", "TZ": "UTC",
        "PATH": CLEAN_PATH,
    }
    suffix = ".dylib" if sys.platform == "darwin" else (
        ".dll" if sys.platform.startswith("win") else ".so"
    )
    published_identity = None
    try:
        with tempfile.TemporaryDirectory(
                prefix=".hwa-viola-passive-render-",
                dir=output_dir.parent) as text:
            scratch = Path(text)
            environment["TMPDIR"] = str(scratch)
            environment["TMP"] = str(scratch)
            environment["TEMP"] = str(scratch)
            rendered = scratch / "model.wav"
            csound = checked_file(files["csound"], "csound")
            checked_file(files["csound_library"], "csound_library")
            csd = checked_file(files["csd"], "csd")
            if FROZEN_CONFIG.get("mode") == JOINT_DIAGNOSTIC_MODE:
                module = diagnostic_module_path()
                tau_id = STRING_TARGETS[FROZEN_CONFIG["target"]]["parameter_id"]
                command = [
                    str(csound), "--opcode-lib={}".format(module),
                    "--sample-accurate", "--num-threads=1", "-W", "-3",
                    "--nopeaks", "-o", str(rendered),
                    "--omacro:FIT_STRING={}".format(case["string"]),
                    "--omacro:FIT_FREQUENCY={:.17g}".format(
                        case["frequency_hz"]),
                    "--omacro:FIT_A4={:.17g}".format(case["a4_hz"]),
                    "--omacro:FIT_PASSIVE_TAU={:.17g}".format(values[tau_id]),
                    "--omacro:FIT_CUTOFF_SCALE={:.17g}".format(
                        values["nut_bridge_cutoff_scale"]),
                    "--omacro:FIT_TOTAL_SECONDS=16",
                    str(csd),
                ]
            else:
                candidate_model = scratch / "viola-v1.json"
                candidate_source = scratch / "hlolli_wg_viola.c"
                candidate_module = scratch / ("hlolli_wg_viola_test" + suffix)
                model = load_json(checked_file(files["model"], "model"))
                apply_values(model, values)
                write_json(candidate_model, model)
                shutil.copyfile(checked_file(files["source"], "source"),
                                candidate_source)
                python = checked_file(files["python"], "python")
                generator = checked_file(files["generator"], "generator")
                schema = checked_file(files["schema"], "schema")
                generation = [
                    str(python), "-B", str(generator),
                    "--model", str(candidate_model),
                    "--schema", str(schema),
                    "--source", str(candidate_source),
                ]
                run_tool(generation, "candidate model generation", scratch,
                         environment)
                run_tool(generation + ["--check"], "candidate model check",
                         scratch, environment)
                compile_candidate(candidate_source, candidate_module, scratch,
                                  environment)
                command = [
                    str(csound), "--opcode-lib={}".format(candidate_module),
                    "--sample-accurate", "--num-threads=1", "-W", "-3",
                    "--nopeaks", "-o", str(rendered),
                    "--omacro:FIT_STRING={}".format(case["string"]),
                    "--omacro:FIT_FREQUENCY={:.17g}".format(
                        case["frequency_hz"]),
                    "--omacro:FIT_A4={:.17g}".format(case["a4_hz"]),
                    str(csd),
                ]
            completed = run_tool(
                command, "Csound viola fit render", scratch, environment
            )
            if FROZEN_CONFIG.get("mode") == JOINT_DIAGNOSTIC_MODE:
                validate_joint_diagnostic_facts(
                    completed, case, values[tau_id],
                    values["nut_bridge_cutoff_scale"],
                )
            facts = output_wave(rendered)
            verify_frozen_config()
            if sha256(request_path) != request_hash:
                raise AdapterError("render request changed during the job")
            if sha256(reference) != reference_hash:
                raise AdapterError("reference input changed during the job")
            os.link(rendered, output)
            published_stat = output.stat()
            published_identity = (published_stat.st_dev, published_stat.st_ino)
            if output_wave(output) != facts:
                raise AdapterError("published model output changed")
    except BaseException:
        if published_identity is not None:
            try:
                current = output.stat()
                if (current.st_dev, current.st_ino) == published_identity:
                    output.unlink()
            except (FileNotFoundError, OSError):
                pass
        raise
    print(json.dumps({
        "adapter_id": FROZEN_CONFIG["adapter_id"],
        "case_id": request["case_id"],
        "job_key": request["job_key"],
        "model_sha256": facts["sha256"],
        "nonzero_samples": facts["nonzero_samples"],
        "clipped_samples": facts["clipped_samples"],
    }, sort_keys=True, allow_nan=False))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hwa-experiment-job", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--validate-profile", type=Path)
    parser.add_argument("--describe", action="store_true")
    commands = parser.add_subparsers(dest="command")
    build_parser = commands.add_parser("build")
    build_parser.add_argument("--viola-root", required=True, type=Path)
    build_parser.add_argument("--csound", required=True, type=Path)
    build_parser.add_argument("--csound-library", required=True, type=Path)
    build_parser.add_argument("--c-compiler", required=True, type=Path)
    build_parser.add_argument("--python", required=True, type=Path)
    build_parser.add_argument("--csound-include-dir", action="append",
                              required=True, type=Path)
    build_parser.add_argument("--roster", type=Path)
    build_parser.add_argument("--joint-passive-diagnostic",
                              action="store_true")
    build_parser.add_argument("--published-output-dir", type=Path)
    build_parser.add_argument("--target", choices=tuple(STRING_TARGETS))
    build_parser.add_argument("--reference-fit", type=Path)
    build_parser.add_argument("--reference-check", type=Path)
    build_parser.add_argument("--fit-frequency-hz", type=float)
    build_parser.add_argument("--check-frequency-hz", type=float)
    build_parser.add_argument("--reference-c3-fit", type=Path,
                              help=argparse.SUPPRESS)
    build_parser.add_argument("--reference-c3-check", type=Path,
                              help=argparse.SUPPRESS)
    build_parser.add_argument("--sample-count", type=int)
    build_parser.add_argument("--output-dir", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    try:
        arguments = parse_args()
        selected = sum((
            arguments.hwa_experiment_job is not None,
            arguments.validate_profile is not None,
            arguments.describe,
            arguments.command == "build",
        ))
        if selected != 1:
            raise AdapterError("choose one adapter action")
        if arguments.hwa_experiment_job is not None:
            if arguments.output_dir is None:
                raise AdapterError("render job needs --output-dir")
            render_job(arguments.hwa_experiment_job, arguments.output_dir)
        elif arguments.validate_profile is not None:
            if FROZEN_CONFIG is not None:
                verify_frozen_config()
                if FROZEN_CONFIG.get("mode") == JOINT_DIAGNOSTIC_MODE:
                    raise AdapterError(
                        "joint diagnostic renderer has no profile interface"
                    )
            validate_profile(arguments.validate_profile)
        elif arguments.describe:
            print(json.dumps(describe(), sort_keys=True, allow_nan=False))
        else:
            if FROZEN_CONFIG is not None:
                raise AdapterError("a frozen renderer cannot build a bundle")
            build(arguments)
    except (AdapterError, OSError, UnicodeError, ValueError,
            json.JSONDecodeError) as error:
        print("viola_fit_adapter.py: {}".format(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

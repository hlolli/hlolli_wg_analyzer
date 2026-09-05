#!/usr/bin/python3 -I
"""Validate and build the first double-bass passive-loss manifest bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Sequence


ADAPTER_DIR = Path(__file__).resolve().parent
DEFAULT_FIT = ADAPTER_DIR / "fit-passive-open-v1.json"
DEFAULT_CONTRACT = ADAPTER_DIR / "reference-contract-v1.json"
DEFAULT_V2_CONTRACT = ADAPTER_DIR / "fit-reference-contract-v2.json"
DEFAULT_V2_FITS = {
    target: ADAPTER_DIR / "fit-passive-{}-v2.json".format(target[0])
    for target in ("e1", "a1", "d2", "g2")
}
DEFAULT_D_FREQUENCY_FIT = ADAPTER_DIR / "fit-passive-d-frequency-v3.json"
MAX_JSON_BYTES = 1024 * 1024
MAX_WAVE_BYTES = 64 * 1024 * 1024

V2_TARGETS = ("e1", "a1", "d2", "g2")
V2_STRING_INDEX = {"e1": 0, "a1": 1, "d2": 2, "g2": 3}
V2_LOSS_LEVELS = (
    0.01, 0.02, 0.04, 0.08, 0.125, 0.25, 0.5, 0.75, 1.0, 1.5,
    2.0, 2.5, 3.0, 4.0, 6.0, 8.0, 12.0, 20.0, 30.0,
)
V2_D_REFINEMENT_LEVELS = (0.85, 0.875, 0.9, 0.925, 0.95)
D_FREQUENCY_BRIDGE_CUTOFF_LEVELS = (
    1000.0, 1500.0, 2000.0, 3000.0, 7086.471045764144,
)
D_FREQUENCY_LOSS_LEVELS = (0.25, 1.0, 1.5, 2.0, 2.5, 3.0)

JOINT_TARGETS = V2_TARGETS
JOINT_ADAPTER_ID = "hlolli_wg_double_bass-passive-joint-validation-v1"
JOINT_V2_ADAPTER_ID = "hlolli_wg_double_bass-passive-joint-validation-v2"
JOINT_V2_DATASET_ID = "mtg-good-sounds-v1"
JOINT_PARAMETER = {
    "id": "joint_candidate", "unit": "choice", "minimum": 0.0,
    "maximum": 1.0, "baseline": 0.0, "levels": [0.0, 1.0],
}
JOINT_OPEN_TARGETS = {
    "e1": {"expected_hz": 41.390732998718335, "note": "E1"},
    "a1": {"expected_hz": 55.25, "note": "A1"},
    "d2": {"expected_hz": 73.74990194289438, "note": "D2"},
    "g2": {"expected_hz": 98.4443083545075, "note": "G2"},
}
JOINT_V2_STRING_ASSIGNMENT_EVIDENCE = {
    "good_sounds_metadata_string": None,
    "kind": "open-pitch-transfer-proxy",
    "source_proven_physical_string": False,
}
JOINT_SELECTION_METHODS = {
    "checked_harmonic_decay": "harmonic-decay-1",
    "isolated_note": "isolated-note-1",
    "selection": "instrument-fit-fit-only-v1",
}
JOINT_SELECTION_MINIMUM_VALID_HARMONICS = 3
RENDERER_ADAPTER_ID = "hlolli_wg_double_bass"
RENDERER_SCHEMA = "hwa-double-bass-renderer"
RENDERER_SCHEMA_VERSION = 1
RENDERER_REQUIRED_RESOURCES = {
    "c_compiler", "csound", "csound_include/csdl.h",
    "csound_include/float-version.h", "csound_include/version.h",
    "generator", "model", "python", "source",
}
JOINT_SELECTION_PARAMETERS = {
    "e1": {"string_e_loss_seconds": 0.5},
    "a1": {"string_a_loss_seconds": 1.5},
    "d2": {
        "string_d_bridge_cutoff_hz": 1500.0,
        "string_d_loss_seconds": 3.0,
    },
    "g2": {"string_g_loss_seconds": 0.5},
}
JOINT_SELECTION_SHA256 = {
    "e1": "26d268f9104ed58e1fe6e3c7f3f4cdcc873270946dd0f7a599db172c3748927d",
    "a1": "5973790bb69546a8dbdece4d49d5553600911faba30f989fb3eb968b75b8cc91",
    "d2": "827d37314ab3f282a55f8ce5d08f2d07c30665f394af3ef480b51de64d876259",
    "g2": "4fd46af8c3f04a11e0fa401c113c38429e7a3dd67e0c3bc6c95c6eeda6e86854",
}


class ManifestError(ValueError):
    pass


def reject_constant(value: str) -> Any:
    raise ManifestError("non-finite JSON number: " + value)


def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ManifestError("duplicate JSON key: " + key)
        result[key] = value
    return result


def load_json(path: Path) -> dict[str, Any]:
    path = path.absolute()
    try:
        if not path.is_file() or path.is_symlink():
            raise ManifestError("JSON input must be a regular file: " + str(path))
        if path.stat().st_size > MAX_JSON_BYTES:
            raise ManifestError("JSON input exceeds its byte limit")
        source = path.read_bytes()
        value = json.loads(
            source.decode("utf-8"), object_pairs_hook=unique_object,
            parse_constant=reject_constant)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ManifestError("cannot read JSON: {}: {}".format(path, error)) from error
    if type(value) is not dict:
        raise ManifestError("JSON root must be an object: " + str(path))
    return value


def regular(path: Path, field: str) -> Path:
    path = path.absolute()
    if not path.is_file() or path.is_symlink():
        raise ManifestError("{} must be a regular file: {}".format(field, path))
    return path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise ManifestError("cannot hash {}: {}".format(path, error)) from error
    return digest.hexdigest()


def wave_facts(path: Path, field: str, analyzer: Path) -> dict[str, Any]:
    path = regular(path, field)
    try:
        file_bytes = path.stat().st_size
        if file_bytes > MAX_WAVE_BYTES:
            raise ManifestError(field + " exceeds its byte limit")
        completed = subprocess.run(
            [str(analyzer), "--json", "--max-bytes", str(MAX_WAVE_BYTES),
             "inspect", str(path)],
            check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, env={"LC_ALL": "C", "LANG": "C", "TZ": "UTC"},
            timeout=120)
    except subprocess.TimeoutExpired as error:
        raise ManifestError("analyzer inspection timed out: " + field) from error
    except OSError as error:
        raise ManifestError("cannot inspect {}: {}".format(field, error)) from error
    if completed.returncode != 0:
        detail = (completed.stderr.strip() or completed.stdout.strip())[-2000:]
        raise ManifestError("analyzer inspection failed: " + field +
                            (": " + detail if detail else ""))
    if len(completed.stdout.encode("utf-8")) > MAX_JSON_BYTES:
        raise ManifestError("analyzer inspection exceeds its byte limit")
    try:
        value = json.loads(
            completed.stdout, object_pairs_hook=unique_object,
            parse_constant=reject_constant)
    except json.JSONDecodeError as error:
        raise ManifestError("analyzer returned invalid JSON") from error
    file_row = value.get("file") if type(value) is dict else None
    format_row = file_row.get("format") if type(file_row) is dict else None
    if (type(value) is not dict or value.get("schema_version") != 2 or
            value.get("command") != "inspect" or
            type(file_row) is not dict or
            file_row.get("path") != str(path) or
            type(format_row) is not dict or
            format_row.get("container") != "riff" or
            format_row.get("encoding") != "pcm"):
        raise ManifestError("analyzer returned unknown WAVE facts: " + field)
    channels = format_row.get("channels")
    rate = format_row.get("sample_rate_hz")
    bits = format_row.get("bits_per_sample")
    valid_bits = format_row.get("valid_bits_per_sample")
    block_align = format_row.get("block_align")
    frames = format_row.get("frames")
    if type(bits) is not int or bits not in (16, 24):
        raise ManifestError(field + " has an invalid PCM layout")
    width = bits // 8
    if (type(channels) is not int or channels < 1 or
            type(rate) is not int or rate < 1 or valid_bits != bits or
            type(block_align) is not int or
            block_align != channels * width or
            type(frames) is not int or frames < 1):
        raise ManifestError(field + " has an invalid PCM layout")
    digest = sha256(path)
    if path.stat().st_size != file_bytes:
        raise ManifestError(field + " changed during inspection")
    return {
        "bits_per_sample": bits,
        "channels": channels,
        "file_bytes": file_bytes,
        "frames": frames,
        "sample_rate_hz": rate,
        "sha256": digest,
    }


def expected_parameters() -> list[dict[str, Any]]:
    rows = []
    for name, index in (("a", 1), ("d", 2), ("e", 0), ("g", 3)):
        rows.append({
            "baseline": 0.25,
            "id": "string_{}_loss_seconds".format(name),
            "maximum": 30.0,
            "minimum": 0.01,
            "profile_paths": [["strings", index,
                               "loss_time_constant_seconds"]],
            "unit": "seconds",
        })
    return rows


def expected_fit() -> dict[str, Any]:
    objectives = []
    groups = (
        ("check", (
            ("a1", "iowa2001-pizz-mf-open-a1-heldout-48k-soxr"),
            ("d2", "iowa2001-pizz-mf-open-d2-heldout-48k-soxr"),
            ("e1", "iowa2001-pizz-mf-open-e1-heldout-48k-soxr"),
            ("g2", "iowa2001-pizz-mf-open-g2-heldout-48k-soxr"),
        )),
        ("fit", (
            ("a1", "iowa2012-pizz-a-mf-open"),
            ("d2", "iowa2012-pizz-d-mf-open"),
            ("e1", "iowa2012-pizz-e-ff-open"),
            ("g2", "iowa2012-pizz-g-pp-open"),
        )),
    )
    for split, rows in groups:
        for note, source_id in rows:
            objectives.append({
                "case": source_id,
                "id": "{}_{}_passive_decay".format(split, note),
                "kind": "passive-decay", "reference_binding": source_id,
                "resource_id": "model.final", "scale": 3.0,
                "split": split, "weight": 1.0,
            })
    return {
        "adapter_id": "hlolli_wg_double_bass-passive-open-v1",
        "objectives": objectives, "parameters": expected_parameters(),
        "schema": "hwa-instrument-fit", "schema_version": 1,
        "selection": {
            "check_weight": 1.0, "max_candidate_worst_harm": 0.25,
            "max_check_loss_increase": 0.15,
        },
    }


def expected_fit_references() -> list[dict[str, Any]]:
    return [
        {
            "articulation": "pizzicato", "bits_per_sample": 24,
            "channels": 1, "dynamic": "mf", "frames": 418366,
            "id": "iowa2012-pizz-a-mf-open", "note": "A1", "role": "fit",
            "sample_rate_hz": 48000,
            "sha256": "c02df52f7fef08b73c0d5a57595a448ba9f23d949a4daa926727becb883237b3",
            "source_group": "iowa2012",
        },
        {
            "articulation": "pizzicato", "bits_per_sample": 24,
            "channels": 1, "dynamic": "mf", "frames": 549412,
            "id": "iowa2012-pizz-d-mf-open", "note": "D2", "role": "fit",
            "sample_rate_hz": 48000,
            "sha256": "032494ea7dd48f6e209e2d6509c842e399d2b4ed464bd9f642e266958fb6356f",
            "source_group": "iowa2012",
        },
        {
            "articulation": "pizzicato", "bits_per_sample": 24,
            "channels": 1, "dynamic": "ff", "frames": 396725,
            "id": "iowa2012-pizz-e-ff-open", "note": "E1", "role": "fit",
            "sample_rate_hz": 48000,
            "sha256": "bf2654fe57ec3d0470abca3786615f163ca133b19969a7d2377dc461731b6e2c",
            "source_group": "iowa2012",
        },
        {
            "articulation": "pizzicato", "bits_per_sample": 24,
            "channels": 1, "dynamic": "pp", "frames": 578239,
            "id": "iowa2012-pizz-g-pp-open", "note": "G2", "role": "fit",
            "sample_rate_hz": 48000,
            "sha256": "c2dd4a3f69fd038faa0d9dd86cf5445e4c274efc4e5dd01047f994c55386c16b",
            "source_group": "iowa2012",
        },
    ]


def expected_heldout_sources() -> list[dict[str, Any]]:
    return [
        {
            "articulation": "pizzicato", "bits_per_sample": 16,
            "channels": 1, "dynamic": "mf", "frames": 164693,
            "id": "iowa2001-pizz-mf-open-a1-heldout", "note": "A1",
            "role": "heldout", "sample_rate_hz": 44100,
            "sha256": "ce5878f5f31304d07ef5c105befd3aeec1287eb285e389cbef8d65698046ccde",
            "source_group": "iowa2001",
        },
        {
            "articulation": "pizzicato", "bits_per_sample": 16,
            "channels": 1, "dynamic": "mf", "frames": 201612,
            "id": "iowa2001-pizz-mf-open-d2-heldout", "note": "D2",
            "role": "heldout", "sample_rate_hz": 44100,
            "sha256": "d30592256dc5358b6a87ddeb54955bcacbd22f6cd7bc6e282bd06eded16362b6",
            "source_group": "iowa2001",
        },
        {
            "articulation": "pizzicato", "bits_per_sample": 16,
            "channels": 1, "dynamic": "mf", "frames": 291351,
            "id": "iowa2001-pizz-mf-open-e1-heldout", "note": "E1",
            "role": "heldout", "sample_rate_hz": 44100,
            "sha256": "ff557aaeb6d726989cc46c5721da48eb13a85fdba7422cd6e54af1b580aa4f48",
            "source_group": "iowa2001",
        },
        {
            "articulation": "pizzicato", "bits_per_sample": 16,
            "channels": 1, "dynamic": "mf", "frames": 165103,
            "id": "iowa2001-pizz-mf-open-g2-heldout", "note": "G2",
            "role": "heldout", "sample_rate_hz": 44100,
            "sha256": "d70cc9cd1b97c98592520ab6b5898028e9d055d5989d20c4049aa969ed7d4b94",
            "source_group": "iowa2001",
        },
    ]


def expected_derived_bindings() -> list[dict[str, Any]]:
    rows = []
    for source_id in (
            "iowa2001-pizz-mf-open-a1-heldout",
            "iowa2001-pizz-mf-open-d2-heldout",
            "iowa2001-pizz-mf-open-e1-heldout",
            "iowa2001-pizz-mf-open-g2-heldout"):
        rows.append({
            "bits_per_sample": 24, "channels": 1,
            "frame_rule": "nearest_duration",
            "id": source_id + "-48k-soxr", "sample_rate_hz": 48000,
            "source_id": source_id,
        })
    return rows


def expected_transform() -> dict[str, Any]:
    return {
        "argv": [
            "-nostdin", "-hide_banner", "-loglevel", "error", "-xerror",
            "-i", "{source}", "-map_metadata", "-1", "-vn", "-sn",
            "-dn", "-ac", "1", "-af",
            "aresample=resampler=soxr:precision=33:dither_method=none",
            "-ar", "48000", "-c:a", "pcm_s24le", "-fflags", "+bitexact",
            "-flags:a", "+bitexact", "-f", "wav", "{derived}",
        ],
        "method": "ffmpeg-libsoxr-precision33-v1",
    }


def expected_contract() -> dict[str, Any]:
    return {
        "derived_bindings": expected_derived_bindings(),
        "fit_references": expected_fit_references(),
        "heldout_sources": expected_heldout_sources(),
        "schema": "hwa-double-bass-reference-contract", "schema_version": 1,
        "transform": expected_transform(),
    }


def expected_v2_contract() -> dict[str, Any]:
    """Return the public v2 reference contract without reading local state."""
    rows = [
        {
            "articulation": "pizzicato", "bits_per_sample": 24,
            "channels": 1, "dynamic": "ff",
            "expected_hz": 48.999429497718666, "frames": 200260,
            "id": "iowa2012-pizz-e-string-g1-ff-left-48k-soxr",
            "note": "G1", "role": "fit", "sample_rate_hz": 48000,
            "selection": "first-valid-ascending-after-e1-f1-rejection",
            "sha256": "103d0df33df6460825b40ec160581e79d7e84c30be76be55fc6454806749f041",
            "source_file": "Bass.pizz.ff.sulE.G1.stereo.aif",
            "source_group": "iowa2012-pitches-stereo",
            "source_sha256": "3fb0c2d4ba9d86111c72361489f759bfa705c7b764af8517b320053c24b9db69",
            "string_id": "e", "target": "e1",
            "transform": "left-channel-ffmpeg-soxr-48k-pcm24-no-gain-no-dither",
        },
        {
            "articulation": "pizzicato", "bits_per_sample": 24,
            "channels": 1, "dynamic": "ff", "expected_hz": 55.0,
            "frames": 435801, "id": "iowa2012-pizz-a-ff-open-left-48k-soxr",
            "note": "A1", "role": "fit", "sample_rate_hz": 48000,
            "selection": "open-string",
            "sha256": "0eaf4b87f47da09c9d6219e0c78b1908aeffe438447299c7361fae3e6a41e844",
            "source_file": "Bass.pizz.ff.sulA.A1.stereo.aif",
            "source_group": "iowa2012-pitches-stereo",
            "source_sha256": "178ad88c9c36823c6fca75df195405691abd714990b3226d3daa44e9e81ecf0a",
            "string_id": "a", "target": "a1",
            "transform": "left-channel-ffmpeg-soxr-48k-pcm24-no-gain-no-dither",
        },
        {
            "articulation": "pizzicato", "bits_per_sample": 24,
            "channels": 1, "dynamic": "ff",
            "expected_hz": 73.41619197935188, "frames": 517772,
            "id": "iowa2012-pizz-d-ff-open-left-48k-soxr",
            "note": "D2", "role": "fit", "sample_rate_hz": 48000,
            "selection": "open-string",
            "sha256": "9e44ff5e642cb9f3a2b6aafbc61d3d9011af6389279dc12685cc675edd1315f3",
            "source_file": "Bass.pizz.ff.sulD.D2.stereo.aif",
            "source_group": "iowa2012-pitches-stereo",
            "source_sha256": "c8cbb04753fb8482351bd7b7d709c8d68286081402af67c5fd007495c1a1018d",
            "string_id": "d", "target": "d2",
            "transform": "left-channel-ffmpeg-soxr-48k-pcm24-no-gain-no-dither",
        },
        {
            "articulation": "pizzicato", "bits_per_sample": 24,
            "channels": 1, "dynamic": "ff",
            "expected_hz": 97.99885899543733, "frames": 188329,
            "id": "iowa2012-pizz-g-ff-open-left-48k-soxr",
            "note": "G2", "role": "fit", "sample_rate_hz": 48000,
            "selection": "open-string",
            "sha256": "394c3b9527e4645b5e4a4b15e9fc92f35dd1fa7a701e4db3e27ddf829d75944f",
            "source_file": "Bass.pizz.ff.sulG.G2.stereo.aif",
            "source_group": "iowa2012-pitches-stereo",
            "source_sha256": "8ca42c590b2625bdae81829141f27de68f104b76e34d9233e11c384fd556f6ea",
            "string_id": "g", "target": "g2",
            "transform": "left-channel-ffmpeg-soxr-48k-pcm24-no-gain-no-dither",
        },
    ]
    return {
        "fit_references": rows,
        "policy": {
            "audio_copied": False,
            "candidate_source": "fit-references-only",
            "dynamic_model": "passive-loss-amplitude-independent",
            "other_corpora": "separate-development-or-audit-only",
            "source_selection": "one-ff-note-per-physical-string",
        },
        "schema": "hwa-double-bass-fit-reference-contract",
        "schema_version": 2,
    }


def v2_reference_for(target: str,
                     contract: dict[str, Any]) -> dict[str, Any]:
    if target not in V2_TARGETS:
        raise ManifestError("unknown v2 target: " + target)
    rows = contract.get("fit_references")
    if type(rows) is not list:
        raise ManifestError("v2 reference contract has invalid fit references")
    found = [row for row in rows
             if type(row) is dict and row.get("target") == target]
    if len(found) != 1:
        raise ManifestError("v2 reference contract must name each target once")
    return found[0]


def v2_selection() -> dict[str, Any]:
    return {
        "check_weight": 0.0,
        "max_candidate_harmonic_maximum_error_octaves": 1.5,
        "max_candidate_harmonic_mean_error_octaves": 0.75,
        "max_candidate_worst_harm": 0.0,
        "max_check_loss_increase": 0.0,
        "mode": "fit-only",
    }


def v2_objective(target: str,
                 reference: dict[str, Any]) -> dict[str, Any]:
    identifier = reference["id"]
    return {
        "case": identifier, "expected_hz": reference["expected_hz"],
        "id": "fit_{}_checked_note_harmonic_decay".format(target),
        "kind": "checked-note-harmonic-decay",
        "reference_binding": identifier,
        "reference_sha256": reference["sha256"],
        "resource_id": "model.final", "scale": 1.0,
        "split": "fit", "weight": 1.0,
    }


def expected_v2_fit(target: str,
                    contract: dict[str, Any] | None = None) -> dict[str, Any]:
    if contract is None:
        contract = expected_v2_contract()
    reference = v2_reference_for(target, contract)
    letter = target[0]
    return {
        "adapter_id": "hlolli_wg_double_bass-passive-{}-v2".format(target),
        "objectives": [v2_objective(target, reference)],
        "parameters": [{
            "baseline": 0.25, "id": "string_{}_loss_seconds".format(letter),
            "maximum": 30.0, "minimum": 0.01,
            "profile_paths": [["strings", V2_STRING_INDEX[target],
                               "loss_time_constant_seconds"]],
            "unit": "seconds",
        }],
        "schema": "hwa-instrument-fit", "schema_version": 1,
        "selection": v2_selection(),
    }


def expected_d_frequency_fit(
        contract: dict[str, Any] | None = None) -> dict[str, Any]:
    if contract is None:
        contract = expected_v2_contract()
    reference = v2_reference_for("d2", contract)
    return {
        "adapter_id": "hlolli_wg_double_bass-passive-d2-frequency-v3",
        "objectives": [v2_objective("d2", reference)],
        "parameters": [
            {
                "baseline": 7086.471045764144,
                "id": "string_d_bridge_cutoff_hz",
                "maximum": 7086.471045764144, "minimum": 1000.0,
                "profile_paths": [["strings", 2, "bridge_cutoff_hz"]],
                "unit": "hertz",
            },
            {
                "baseline": 0.25, "id": "string_d_loss_seconds",
                "maximum": 3.0, "minimum": 0.25,
                "profile_paths": [["strings", 2,
                                   "loss_time_constant_seconds"]],
                "unit": "seconds",
            },
        ],
        "schema": "hwa-instrument-fit", "schema_version": 1,
        "selection": v2_selection(),
    }


def validate_v2_contracts(contract_path: Path,
                          fit_paths: dict[str, Path]) -> dict[str, Any]:
    contract = load_json(contract_path)
    if contract != expected_v2_contract():
        raise ManifestError("v2 reference contract changed")
    if set(fit_paths) != set(V2_TARGETS):
        raise ManifestError("v2 fit targets differ")
    for target in V2_TARGETS:
        if load_json(fit_paths[target]) != expected_v2_fit(target, contract):
            raise ManifestError("v2 fit manifest changed: " + target)
    return {
        "audio_copied": False,
        "fit_cases": [v2_reference_for(target, contract)["id"]
                      for target in V2_TARGETS],
        "schema": "hwa-double-bass-fit-contract",
        "schema_version": 2,
        "selection": "fit-only",
    }


def validate_d_frequency_contract(contract_path: Path,
                                  fit_path: Path) -> dict[str, Any]:
    contract = load_json(contract_path)
    if contract != expected_v2_contract():
        raise ManifestError("v2 reference contract changed")
    if load_json(fit_path) != expected_d_frequency_fit(contract):
        raise ManifestError("D frequency fit manifest changed")
    return {
        "audio_copied": False, "grid_points": 30,
        "parameters": ["string_d_bridge_cutoff_hz",
                       "string_d_loss_seconds"],
        "selection": "fit-only", "target": "d2",
    }


def exact_fields(value: Any, wanted: set[str], field: str) -> None:
    if type(value) is not dict:
        raise ManifestError(field + " must be an object")
    found = set(value)
    if found != wanted:
        raise ManifestError(
            "{} fields differ; extra={} missing={}".format(
                field, sorted(found - wanted), sorted(wanted - found)))


def checked_digest(value: Any, field: str) -> str:
    if (type(value) is not str or
            re.fullmatch(r"[0-9a-f]{64}", value) is None):
        raise ManifestError(field + " must be a lower-case SHA-256")
    return value


def run_json(arguments: list[str], field: str,
             timeout: int = 120) -> dict[str, Any]:
    try:
        completed = subprocess.run(
            arguments, check=False, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True,
            env={"LC_ALL": "C", "LANG": "C", "TZ": "UTC"},
            timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise ManifestError(field + " timed out") from error
    except OSError as error:
        raise ManifestError("cannot run {}: {}".format(field, error)) from error
    if completed.returncode != 0:
        detail = (completed.stderr.strip() or completed.stdout.strip())[-2000:]
        raise ManifestError(field + " failed" +
                            (": " + detail if detail else ""))
    if len(completed.stdout.encode("utf-8")) > MAX_JSON_BYTES:
        raise ManifestError(field + " exceeds its byte limit")
    try:
        value = json.loads(
            completed.stdout, object_pairs_hook=unique_object,
            parse_constant=reject_constant)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ManifestError(field + " returned invalid JSON") from error
    if type(value) is not dict:
        raise ManifestError(field + " returned a non-object")
    return value


def v2_reference_evidence(analyzer: Path, path: Path,
                          row: dict[str, Any]) -> dict[str, Any]:
    identifier = str(row.get("id", ""))
    path, facts = require_file_facts(
        path, row, "fit reference " + identifier, analyzer)
    expected_hz = row.get("expected_hz")
    if type(expected_hz) not in (int, float) or type(expected_hz) is bool:
        raise ManifestError("fit reference has invalid expected frequency: " +
                            identifier)
    frequency = repr(float(expected_hz))
    isolated = run_json([
        str(analyzer), "--json", "isolated-note", str(path),
        "--expected-hz", frequency, "--metrics", "pitch",
    ], "fit reference pitch check")
    pitch = isolated.get("pitch")
    if (isolated.get("schema") != "hwa-isolated-note" or
            isolated.get("schema_version") != 1 or
            isolated.get("command") != "isolated-note" or
            isolated.get("method") != "isolated-note-1" or
            isolated.get("path") != str(path) or
            isolated.get("expected_hz") != expected_hz or
            isolated.get("requested_metrics") != ["pitch"] or
            type(pitch) is not dict or pitch.get("valid") is not True):
        raise ManifestError("fit reference failed checked pitch: " + identifier)
    harmonic = run_json([
        str(analyzer), "--json", "harmonic-decay", str(path),
        "--expected-hz", frequency,
    ], "fit reference harmonic-decay check")
    profile = harmonic.get("reference")
    if (harmonic.get("schema") != "hwa-harmonic-decay" or
            harmonic.get("schema_version") != 1 or
            harmonic.get("command") != "harmonic-decay" or
            harmonic.get("method") != "harmonic-decay-1" or
            harmonic.get("expected_hz") != expected_hz or
            type(profile) is not dict or profile.get("path") != str(path) or
            profile.get("valid") is not True or
            type(profile.get("valid_band_count")) is not int or
            profile["valid_band_count"] < 4 or
            harmonic.get("model") is not None or
            harmonic.get("comparison") is not None):
        raise ManifestError(
            "fit reference failed checked harmonic decay: " + identifier)
    return {
        "file": facts, "harmonic_decay": harmonic, "isolated_note": isolated,
    }


def experiment_stem(resource_id: str, side: str, input_id: Any,
                    output: Any) -> dict[str, Any]:
    return {
        "channels": 1, "gain_db": 0, "id": resource_id,
        "input_id": input_id, "output": output, "rate_hz": 48000,
        "role": "final", "side": side, "start_sample": 0,
    }


def v2_experiment(target: str, reference: dict[str, Any],
                  fit: dict[str, Any], levels: Sequence[float]
                  ) -> dict[str, Any]:
    identifier = reference["id"]
    parameter_rows = []
    for index, row in enumerate(fit["parameters"]):
        copy = {
            "baseline": row["baseline"], "id": row["id"],
            "levels": list(levels) if len(fit["parameters"]) == 1 else [],
            "maximum": row["maximum"], "minimum": row["minimum"],
            "unit": row["unit"],
        }
        parameter_rows.append(copy)
    if len(parameter_rows) == 2:
        parameter_rows[0]["levels"] = list(D_FREQUENCY_BRIDGE_CUTOFF_LEVELS)
        parameter_rows[1]["levels"] = list(D_FREQUENCY_LOSS_LEVELS)

    def one_case(case_id: str, split: str) -> dict[str, Any]:
        return {
            "id": case_id, "links": [], "probes": [], "split": split,
            "stems": [
                experiment_stem("model.final", "model", None, "model.wav"),
                experiment_stem("reference.final", "reference", identifier,
                                None),
            ],
            "weight": 1,
        }

    return {
        "cases": [
            one_case(identifier, "fit"),
            one_case(identifier + "-diagnostic-check", "check"),
        ],
        "clock_rate_hz": 48000,
        "inputs": [{"id": identifier, "sha256": reference["sha256"]}],
        "method_version": "stage8-1", "parameters": parameter_rows,
        "plan": {
            "kind": "grid", "replicates": 1,
            "sample_count": 0, "seed": 1701,
        },
        "responses": [{
            "feature": "rms_dbfs", "id": "diagnostic.rms",
            "index": 0, "role": "final",
        }],
        "schema": "hwa-experiment", "schema_version": 1,
    }


def checked_output_path(output: Path) -> Path:
    output = output.absolute()
    if output.exists() or output.is_symlink() or not output.parent.is_dir():
        raise ManifestError(
            "output directory must be new with an existing parent")
    return output


def publish_directory(scratch: Path, output: Path) -> None:
    try:
        os.replace(scratch, output)
    except OSError as error:
        raise ManifestError("cannot publish output directory: " + str(error)) \
            from error


def v2_bindings(reference: dict[str, Any], path: Path) -> dict[str, Any]:
    return {
        "bindings": [{
            "id": reference["id"], "path": str(path),
            "sha256": reference["sha256"],
        }],
        "schema": "hwa-fit-bindings", "schema_version": 1,
    }


def build_v2_bundle(arguments: argparse.Namespace) -> dict[str, Any]:
    output = checked_output_path(arguments.output_dir)
    contract_path = regular(arguments.reference_contract,
                            "v2 reference contract")
    contract = load_json(contract_path)
    if contract != expected_v2_contract():
        raise ManifestError("v2 reference contract changed")
    fit_arguments = binding_arguments(
        arguments.fit_manifest,
        [{"id": target} for target in V2_TARGETS], "fit manifest")
    reference_arguments = binding_arguments(
        arguments.fit_reference, contract["fit_references"], "fit reference")
    analyzer = regular(arguments.analyzer.resolve(strict=True), "analyzer")
    analyzer_hash = sha256(analyzer)
    analyzer_release = analyzer_version(analyzer)
    fits = {}
    references = {}
    evidence = {}
    for target in V2_TARGETS:
        reference = v2_reference_for(target, contract)
        fit_path = regular(fit_arguments[target], "fit manifest " + target)
        fit = load_json(fit_path)
        if fit != expected_v2_fit(target, contract):
            raise ManifestError("v2 fit manifest changed: " + target)
        reference_path = reference_arguments[reference["id"]]
        evidence[reference["id"]] = v2_reference_evidence(
            analyzer, reference_path, reference)
        fits[target] = (fit_path, fit)
        references[target] = (reference_path, reference)
    if sha256(analyzer) != analyzer_hash:
        raise ManifestError("analyzer changed during v2 reference checks")

    scratch = Path(tempfile.mkdtemp(prefix=".{}-".format(output.name),
                                    dir=output.parent))
    try:
        child_hashes = {}
        for target in V2_TARGETS:
            fit_path, fit = fits[target]
            reference_path, reference = references[target]
            levels = list(V2_LOSS_LEVELS)
            if target == "d2":
                levels.extend(V2_D_REFINEMENT_LEVELS)
                levels.sort()
            target_dir = scratch / target
            target_dir.mkdir(mode=0o700)
            files = {
                "bindings.local.json": v2_bindings(reference, reference_path),
                "experiment.json": v2_experiment(
                    target, reference, fit, levels),
                "fit.json": fit,
                "reference-evidence.json": evidence[reference["id"]],
            }
            child_hashes[target] = {}
            for name, value in files.items():
                source = json_bytes(value)
                write_private(target_dir / name, source)
                child_hashes[target][name] = hashlib.sha256(source).hexdigest()
        receipt = {
            "analyzer": {
                "sha256": analyzer_hash, "version": analyzer_release,
            },
            "audio_copied": False,
            "builder_sha256": sha256(Path(__file__).resolve()),
            "candidate_source": "fit-references-only",
            "files": child_hashes,
            "fit_references": [
                {name: row[name] for name in (
                    "bits_per_sample", "channels", "frames", "id",
                    "sample_rate_hz", "sha256", "target")}
                for row in contract["fit_references"]
            ],
            "reference_contract_sha256": sha256(contract_path),
            "schema": "hwa-double-bass-fit-bundle",
            "schema_version": 2,
        }
        write_private(scratch / "receipt.json", json_bytes(receipt))
        for target, (path, row) in references.items():
            if sha256(path) != row["sha256"]:
                raise ManifestError("fit reference changed: " + target)
        for target, (path, unused_fit) in fits.items():
            if load_json(path) != expected_v2_fit(target, contract):
                raise ManifestError("fit manifest changed: " + target)
        if (sha256(analyzer) != analyzer_hash or
                load_json(contract_path) != contract):
            raise ManifestError("v2 bundle input changed during build")
        publish_directory(scratch, output)
    except BaseException:
        shutil.rmtree(scratch, ignore_errors=True)
        raise
    return {
        "audio_copied": False, "output": str(output),
        "targets": list(V2_TARGETS),
    }


def build_d_frequency_bundle(arguments: argparse.Namespace) -> dict[str, Any]:
    output = checked_output_path(arguments.output_dir)
    contract_path = regular(arguments.reference_contract,
                            "v2 reference contract")
    contract = load_json(contract_path)
    if contract != expected_v2_contract():
        raise ManifestError("v2 reference contract changed")
    fit_path = regular(arguments.fit_manifest, "D frequency fit manifest")
    fit = load_json(fit_path)
    if fit != expected_d_frequency_fit(contract):
        raise ManifestError("D frequency fit manifest changed")
    reference = v2_reference_for("d2", contract)
    reference_arguments = binding_arguments(
        arguments.fit_reference, [reference], "fit reference")
    reference_path = reference_arguments[reference["id"]]
    analyzer = regular(arguments.analyzer.resolve(strict=True), "analyzer")
    analyzer_hash = sha256(analyzer)
    evidence = v2_reference_evidence(analyzer, reference_path, reference)
    experiment = v2_experiment("d2", reference, fit, ())
    files = {
        "bindings.local.json": v2_bindings(reference, reference_path),
        "experiment.json": experiment,
        "fit.json": fit,
        "reference-evidence.json": evidence,
    }
    scratch = Path(tempfile.mkdtemp(prefix=".{}-".format(output.name),
                                    dir=output.parent))
    try:
        hashes = {}
        for name, value in files.items():
            source = json_bytes(value)
            write_private(scratch / name, source)
            hashes[name] = hashlib.sha256(source).hexdigest()
        receipt = {
            "analyzer_sha256": analyzer_hash, "audio_copied": False,
            "builder_sha256": sha256(Path(__file__).resolve()),
            "candidate_model_written": False,
            "files": hashes,
            "grid": {
                "bridge_cutoff_hz": list(D_FREQUENCY_BRIDGE_CUTOFF_LEVELS),
                "loss_seconds": list(D_FREQUENCY_LOSS_LEVELS),
                "point_count": 30,
            },
            "reference": {name: reference[name] for name in (
                "bits_per_sample", "channels", "frames", "id",
                "sample_rate_hz", "sha256", "target")},
            "schema": "hwa-double-bass-d-frequency-fit-bundle",
            "schema_version": 1, "selection": "fit-only",
        }
        write_private(scratch / "receipt.json", json_bytes(receipt))
        if (sha256(analyzer) != analyzer_hash or
                sha256(reference_path) != reference["sha256"] or
                load_json(fit_path) != fit or
                load_json(contract_path) != contract):
            raise ManifestError("D frequency bundle input changed during build")
        publish_directory(scratch, output)
    except BaseException:
        shutil.rmtree(scratch, ignore_errors=True)
        raise
    return {
        "audio_copied": False, "grid_points": 30,
        "output": str(output), "target": "d2",
    }


def joint_candidate_changes(
        selection_hashes: dict[str, str] | None = None
        ) -> list[dict[str, Any]]:
    hashes = JOINT_SELECTION_SHA256 if selection_hashes is None \
        else selection_hashes
    if set(hashes) != set(JOINT_TARGETS):
        raise ManifestError("joint selection hashes must name four targets")
    for target in JOINT_TARGETS:
        checked_digest(hashes[target], "joint selection hash " + target)
    return [
        {
            "after": 0.5, "before": 0.25, "maximum": 30.0,
            "minimum": 0.01, "parameter": "string_e_loss_seconds",
            "path": ["strings", 0, "loss_time_constant_seconds"],
            "source_fit_result_sha256": hashes["e1"], "unit": "seconds",
        },
        {
            "after": 1.5, "before": 0.25, "maximum": 30.0,
            "minimum": 0.01, "parameter": "string_a_loss_seconds",
            "path": ["strings", 1, "loss_time_constant_seconds"],
            "source_fit_result_sha256": hashes["a1"], "unit": "seconds",
        },
        {
            "after": 1500.0, "before": 7086.471045764144,
            "maximum": 7086.471045764144, "minimum": 1000.0,
            "parameter": "string_d_bridge_cutoff_hz",
            "path": ["strings", 2, "bridge_cutoff_hz"],
            "source_fit_result_sha256": hashes["d2"], "unit": "hertz",
        },
        {
            "after": 3.0, "before": 0.25, "maximum": 3.0,
            "minimum": 0.25, "parameter": "string_d_loss_seconds",
            "path": ["strings", 2, "loss_time_constant_seconds"],
            "source_fit_result_sha256": hashes["d2"], "unit": "seconds",
        },
        {
            "after": 0.5, "before": 0.25, "maximum": 30.0,
            "minimum": 0.01, "parameter": "string_g_loss_seconds",
            "path": ["strings", 3, "loss_time_constant_seconds"],
            "source_fit_result_sha256": hashes["g2"], "unit": "seconds",
        },
    ]


def applied_joint_candidate(profile: dict[str, Any],
                            changes: list[dict[str, Any]]) -> dict[str, Any]:
    result = json.loads(json.dumps(profile, allow_nan=False))
    for row in changes:
        current = result
        try:
            for part in row["path"][:-1]:
                current = current[part]
            if current[row["path"][-1]] != row["before"]:
                raise ManifestError(
                    "fixed model differs from the joint candidate baseline")
            current[row["path"][-1]] = row["after"]
        except (KeyError, IndexError, TypeError) as error:
            raise ManifestError("fixed model lacks a joint candidate path") \
                from error
    return result


def joint_case_id(target: str, split: str) -> str:
    return "joint-{}-{}".format(target[0], split)


def joint_experiment(references: dict[str, Any]) -> dict[str, Any]:
    if set(references) != set(JOINT_TARGETS):
        raise ManifestError("joint references must name four targets")
    inputs = {}
    cases = []
    for target in JOINT_TARGETS:
        roles = references[target]
        if type(roles) is not dict or set(roles) != {"fit", "audit"}:
            raise ManifestError("joint target needs fit and audit references")
        for role in ("fit", "audit"):
            row = roles[role]
            exact_fields(row, {"expected_hz", "id", "path", "sha256"},
                         "joint reference")
            checked_digest(row["sha256"], "joint reference hash")
            inputs[row["id"]] = row["sha256"]
        for split in ("fit", "check", "audit"):
            role = "audit" if split == "audit" else "fit"
            reference = roles[role]
            cases.append({
                "id": joint_case_id(target, split),
                "links": [], "probes": [], "split": split,
                "stems": [
                    experiment_stem(
                        "model.final", "model", None, "model.wav"),
                    experiment_stem(
                        "reference.final", "reference", reference["id"], None),
                ],
                "weight": 1,
            })
    cases.sort(key=lambda row: row["id"])
    return {
        "cases": cases, "clock_rate_hz": 48000,
        "inputs": [{"id": name, "sha256": digest}
                   for name, digest in sorted(inputs.items())],
        "method_version": "stage8-1",
        "parameters": [dict(JOINT_PARAMETER)],
        "plan": {
            "kind": "one-at-a-time", "replicates": 1,
            "sample_count": 0, "seed": 1701,
        },
        "responses": [{
            "feature": "rms_dbfs", "id": "diagnostic.rms",
            "index": 0, "role": "final",
        }],
        "schema": "hwa-experiment", "schema_version": 1,
    }


def joint_objective(target: str, split: str,
                    reference: dict[str, Any]) -> dict[str, Any]:
    return {
        "case": joint_case_id(target, split),
        "expected_hz": reference["expected_hz"],
        "id": "{}_{}_checked_note_harmonic_decay".format(split, target),
        "kind": "checked-note-harmonic-decay",
        "reference_binding": reference["id"],
        "reference_sha256": reference["sha256"],
        "resource_id": "model.final", "scale": 1.0,
        "split": split, "weight": 1.0,
    }


def joint_fit_manifest(references: dict[str, Any],
                       expected_losses: dict[str, float],
                       changes: list[dict[str, Any]],
                       profile_adapter_sha256: str,
                       adapter_id: str = JOINT_ADAPTER_ID) -> dict[str, Any]:
    if set(references) != set(JOINT_TARGETS):
        raise ManifestError("joint references must name four targets")
    if set(expected_losses) != set(JOINT_TARGETS):
        raise ManifestError("joint expected losses must name four targets")
    checked_digest(profile_adapter_sha256, "profile adapter hash")
    objectives = []
    candidate_losses = {}
    for target in JOINT_TARGETS:
        for split in ("audit", "check", "fit"):
            role = "audit" if split == "audit" else "fit"
            objective = joint_objective(
                target, split, references[target][role])
            objectives.append(objective)
            if split != "audit":
                candidate_losses[objective["id"]] = expected_losses[target]
    objectives.sort(key=lambda row: row["id"])
    return {
        "adapter_id": adapter_id,
        "candidate": {
            "expected_objective_losses": candidate_losses,
            "parameters": {"joint_candidate": 1.0},
            "profile_adapter_sha256": profile_adapter_sha256,
            "profile_changes": changes,
        },
        "objectives": objectives,
        "parameters": [{
            "baseline": JOINT_PARAMETER["baseline"],
            "id": JOINT_PARAMETER["id"],
            "maximum": JOINT_PARAMETER["maximum"],
            "minimum": JOINT_PARAMETER["minimum"],
            "profile_paths": [], "unit": JOINT_PARAMETER["unit"],
        }],
        "schema": "hwa-instrument-fit", "schema_version": 2,
        "selection": {
            "limits": [
                {
                    "max_candidate_loss": 8.0,
                    "max_mean_loss_increase": 0.0,
                    "max_objective_loss_increase": 0.0,
                    "split": "fit",
                },
                {
                    "max_candidate_loss": 8.0,
                    "max_mean_loss_increase": 0.0,
                    "max_objective_loss_increase": 0.0,
                    "split": "check",
                },
                {
                    "max_candidate_loss": 8.0,
                    "max_mean_loss_increase": 0.0,
                    "max_objective_loss_increase": 0.0,
                    "split": "audit",
                },
            ],
            "max_candidate_harmonic_maximum_error_octaves": 1.5,
            "max_candidate_harmonic_mean_error_octaves": 0.75,
            "max_candidate_worst_harm": 0.0,
            "max_expected_loss_increase": 0.0,
            "max_score_increase": 0.0,
            "maximum_candidate_t60_ratio": 2.0,
            "minimum_candidate_harmonic_count": 4,
            "minimum_candidate_support_ratio": 0.5,
            "minimum_candidate_t60_ratio": 0.5,
            "mode": "verify-candidate",
            "score_weights": {"audit": 1.0, "check": 1.0, "fit": 1.0},
        },
    }


def contained_by(path: Path, directory: Path) -> bool:
    try:
        path.resolve(strict=True).relative_to(directory.resolve(strict=True))
        return True
    except ValueError:
        return False


def checked_declared_file(value: Any, field: str) -> Path:
    exact_fields(value, {"path", "sha256"}, field)
    path_value = value["path"]
    if type(path_value) is not str or not Path(path_value).is_absolute():
        raise ManifestError(field + " path must be absolute")
    path = regular(Path(path_value), field)
    digest = checked_digest(value["sha256"], field + " hash")
    if sha256(path) != digest:
        raise ManifestError(field + " hash differs")
    return path


def checked_finite(value: Any, field: str) -> float:
    if type(value) not in (int, float) or not math.isfinite(float(value)):
        raise ManifestError(field + " must be finite")
    return float(value)


def checked_nonnegative(value: Any, field: str) -> float:
    result = checked_finite(value, field)
    if result < 0.0:
        raise ManifestError(field + " must be nonnegative")
    return result


def joint_selection_manifest(target: str,
                             contract: dict[str, Any]) -> dict[str, Any]:
    if target == "d2":
        return expected_d_frequency_fit(contract)
    return expected_v2_fit(target, contract)


def checked_joint_selection(
        path: Path, target: str, selection_hash: str,
        fit_id: str, fit_hash: str, gates: dict[str, Any],
        analyzer_hash: str, selector_hash: str, profile_hash: str,
        contract: dict[str, Any]) -> tuple[dict[str, Any], float]:
    if selection_hash != JOINT_SELECTION_SHA256[target]:
        raise ManifestError("fit selection is not the frozen result: " + target)
    if sha256(path) != selection_hash:
        raise ManifestError("fit selection hash differs: " + target)
    selection = load_json(path)
    exact_fields(selection, {
        "adapter_id", "analyzer_sha256", "baseline_point_id",
        "baseline_score", "chosen_parameters", "chosen_point_id",
        "chosen_score", "experiment_result_sha256", "fit_manifest_sha256",
        "method_versions", "points", "profile_sha256", "reference_bindings",
        "schema", "schema_version", "selection_mode", "selector_sha256",
        "status",
    }, "fit selection " + target)
    fit_manifest = joint_selection_manifest(target, contract)
    fit_manifest_hash = hashlib.sha256(json_bytes(fit_manifest)).hexdigest()
    if (selection["schema"] != "hwa-instrument-fit-result" or
            selection["schema_version"] != 1 or
            selection["selection_mode"] != "fit-only" or
            selection["status"] != "pass" or
            selection["adapter_id"] != fit_manifest["adapter_id"] or
            selection["method_versions"] != JOINT_SELECTION_METHODS or
            selection["fit_manifest_sha256"] != fit_manifest_hash or
            selection["analyzer_sha256"] != analyzer_hash or
            selection["selector_sha256"] != selector_hash or
            selection["profile_sha256"] != profile_hash):
        raise ManifestError("fit selection authority differs: " + target)
    checked_digest(selection["experiment_result_sha256"],
                   "fit selection experiment result hash")
    bindings = selection["reference_bindings"]
    if (bindings != [{"id": fit_id, "sha256": fit_hash}] or
            selection["chosen_parameters"] !=
            JOINT_SELECTION_PARAMETERS[target]):
        raise ManifestError("fit selection binding or parameters differ: " +
                            target)

    parameters = {row["id"]: row for row in fit_manifest["parameters"]}
    points = selection["points"]
    if type(points) is not list or not points:
        raise ManifestError("fit selection lacks checked points: " + target)
    checked_points: dict[int, dict[str, Any]] = {}
    evidence_fields = {
        "checked_harmonic_decay_valid", "checked_note_valid", "loss",
        "loss_increase_from_baseline", "maximum_absolute_t60_error_octaves",
        "mean_absolute_t60_error_octaves", "median_t60_bias_octaves",
        "model_pitch_error_cents", "objective", "reference_pitch_error_cents",
        "rms_t60_error_octaves", "shared_reference_coverage",
        "valid_harmonic_count",
    }
    objective = fit_manifest["objectives"][0]
    for index, row in enumerate(points):
        exact_fields(row, {
            "baseline", "check_loss", "eligible", "evidence", "fit_loss",
            "parameters", "point_id", "point_key", "score", "source_groups",
            "worst_harm",
        }, "fit selection point {}".format(index))
        point_id = row["point_id"]
        if (type(point_id) is not int or type(point_id) is bool or
                point_id in checked_points or type(row["baseline"]) is not bool or
                type(row["eligible"]) is not bool):
            raise ManifestError("fit selection point identity differs: " + target)
        checked_digest(row["point_key"], "fit selection point key")
        values = row["parameters"]
        if type(values) is not dict or set(values) != set(parameters):
            raise ManifestError("fit selection point parameters differ: " + target)
        checked_values = {}
        for name, parameter in parameters.items():
            value = checked_finite(values[name], "fit selection parameter")
            if not float(parameter["minimum"]) <= value <= float(
                    parameter["maximum"]):
                raise ManifestError("fit selection parameter is out of range: " +
                                    target)
            checked_values[name] = value
        evidence_rows = row["evidence"]
        if type(evidence_rows) is not list or len(evidence_rows) != 1:
            raise ManifestError("fit selection evidence count differs: " + target)
        evidence = evidence_rows[0]
        exact_fields(evidence, evidence_fields,
                     "fit selection evidence " + target)
        if (evidence["objective"] != objective["id"] or
                type(evidence["checked_note_valid"]) is not bool or
                type(evidence["checked_harmonic_decay_valid"]) is not bool or
                type(evidence["valid_harmonic_count"]) is not int or
                type(evidence["valid_harmonic_count"]) is bool or
                evidence["valid_harmonic_count"] < 0):
            raise ManifestError("fit selection evidence contract differs: " +
                                target)
        loss = checked_nonnegative(evidence["loss"],
                                   "fit selection evidence loss")
        rms = checked_nonnegative(evidence["rms_t60_error_octaves"],
                                  "fit selection evidence RMS")
        mean = checked_nonnegative(
            evidence["mean_absolute_t60_error_octaves"],
            "fit selection evidence mean error")
        maximum = checked_nonnegative(
            evidence["maximum_absolute_t60_error_octaves"],
            "fit selection evidence maximum error")
        for name in (
                "loss_increase_from_baseline", "median_t60_bias_octaves",
                "model_pitch_error_cents", "reference_pitch_error_cents",
                "shared_reference_coverage"):
            checked_finite(evidence[name], "fit selection evidence " + name)
        fit_loss = checked_nonnegative(row["fit_loss"],
                                       "fit selection point fit loss")
        check_loss = checked_nonnegative(row["check_loss"],
                                         "fit selection point check loss")
        score = checked_nonnegative(row["score"],
                                    "fit selection point score")
        worst_harm = checked_nonnegative(row["worst_harm"],
                                         "fit selection point worst harm")
        eligible = bool(
            evidence["valid_harmonic_count"] >=
            JOINT_SELECTION_MINIMUM_VALID_HARMONICS and
            mean <= float(fit_manifest["selection"][
                "max_candidate_harmonic_mean_error_octaves"]) and
            maximum <= float(fit_manifest["selection"][
                "max_candidate_harmonic_maximum_error_octaves"])
        )
        if (loss != rms / float(objective["scale"]) or
                fit_loss != loss or check_loss != 0.0 or score != fit_loss or
                worst_harm != 0.0 or row["source_groups"] != [] or
                row["eligible"] is not eligible):
            raise ManifestError("fit selection point summary differs: " + target)
        checked_points[point_id] = {
            "raw": row, "parameters": checked_values, "evidence": evidence,
            "score": score,
        }

    baseline_id = selection["baseline_point_id"]
    chosen_id = selection["chosen_point_id"]
    if (type(baseline_id) is not int or type(baseline_id) is bool or
            type(chosen_id) is not int or type(chosen_id) is bool or
            baseline_id == chosen_id or baseline_id not in checked_points or
            chosen_id not in checked_points or
            sum(row["raw"]["baseline"] for row in checked_points.values()) != 1 or
            checked_points[baseline_id]["raw"]["baseline"] is not True):
        raise ManifestError("fit selection point roles differ: " + target)
    baseline = checked_points[baseline_id]
    for row in checked_points.values():
        expected_increase = (row["evidence"]["loss"] -
                             baseline["evidence"]["loss"])
        if row["evidence"]["loss_increase_from_baseline"] != expected_increase:
            raise ManifestError("fit selection loss increase differs: " + target)
    expected_baseline = {
        name: float(row["baseline"]) for name, row in parameters.items()
    }
    chosen = checked_points[chosen_id]
    baseline_score = checked_nonnegative(
        selection["baseline_score"], "fit selection baseline score")
    chosen_score = checked_nonnegative(
        selection["chosen_score"], "fit selection chosen score")
    eligible_points = [
        row for row in checked_points.values() if row["raw"]["eligible"]
    ]
    ranked = min(eligible_points,
                 key=lambda row: (row["score"], 0.0,
                                  row["raw"]["point_id"])) \
        if eligible_points else None
    expected_order = sorted(
        points, key=lambda row: (not row["eligible"], row["score"],
                                 row["point_id"]))
    if (baseline["parameters"] != expected_baseline or
            baseline_score != baseline["score"] or
            chosen["raw"]["eligible"] is not True or
            chosen["raw"]["parameters"] != selection["chosen_parameters"] or
            chosen_score != chosen["score"] or ranked is not chosen or
            points != expected_order):
        raise ManifestError("fit selection result summary differs: " + target)
    evidence = chosen["evidence"]
    if (evidence["checked_harmonic_decay_valid"] is not True or
            evidence["checked_note_valid"] is not True or
            evidence["valid_harmonic_count"] < gates["minimum_valid_harmonics"] or
            evidence["mean_absolute_t60_error_octaves"] >
            gates["maximum_mean_absolute_t60_error_octaves"] or
            evidence["maximum_absolute_t60_error_octaves"] >
            gates["maximum_absolute_t60_error_octaves"]):
        raise ManifestError("fit selection failed its checked gate: " + target)
    return selection, float(chosen["raw"]["fit_loss"])


def checked_joint_declaration_v2(path: Path) -> dict[str, Any]:
    declaration = load_json(path)
    exact_fields(declaration, {
        "audit", "candidate", "commands", "created_utc", "fit_references",
        "fit_selections", "fixed_model", "gates", "policy",
        "renderer_base_config", "repositories", "schema", "schema_version",
        "tools", "validation_roster",
    }, "joint v2 declaration")
    if (declaration["schema"] !=
            "hwa-double-bass-joint-validation-declaration" or
            declaration["schema_version"] != 2):
        raise ManifestError("unsupported joint v2 declaration")
    if declaration["commands"] != ["build-joint-validation-v2"]:
        raise ManifestError("joint v2 declaration commands differ")
    created = declaration["created_utc"]
    if (type(created) is not str or
            re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:"
                         r"[0-9]{2}:[0-9]{2}Z", created) is None):
        raise ManifestError("joint v2 declaration has an invalid timestamp")

    audit = declaration["audit"]
    exact_fields(audit, {
        "dataset_id", "independent_of_fit_source_group", "source_group_id",
    }, "joint v2 audit")
    if (audit["dataset_id"] != JOINT_V2_DATASET_ID or
            audit["independent_of_fit_source_group"] is not True or
            type(audit["source_group_id"]) is not str or
            not audit["source_group_id"]):
        raise ManifestError("joint v2 audit declaration differs")

    expected_policy = {
        "candidate_results_outside_repositories": True,
        "candidate_tuning_after_validation": False,
        "fixed_model_write": False,
        "raw_and_derived_audio_outside_repositories": True,
        "validation_runs": 1,
    }
    if declaration["policy"] != expected_policy:
        raise ManifestError("joint v2 policy differs")
    expected_gates = {
        "harmonic_decay_method": "harmonic-decay-1",
        "maximum_absolute_t60_error_octaves": 1.5,
        "maximum_mean_absolute_t60_error_octaves": 0.75,
        "minimum_valid_harmonics": 4,
        "pitch_method": "isolated-note-1",
    }
    if declaration["gates"] != expected_gates:
        raise ManifestError("joint v2 gates differ")

    repository_values = declaration["repositories"]
    if (type(repository_values) is not list or len(repository_values) != 2 or
            any(type(value) is not str or not Path(value).is_absolute()
                for value in repository_values)):
        raise ManifestError("joint v2 repositories differ")
    repositories = []
    for value in repository_values:
        directory = Path(value)
        if not directory.is_dir() or directory.is_symlink():
            raise ManifestError("repository must be a regular directory: " +
                                str(directory))
        repositories.append(directory)
    if len({str(row.resolve()) for row in repositories}) != 2:
        raise ManifestError("joint v2 repositories must be distinct")

    fixed_path = checked_declared_file(
        declaration["fixed_model"], "fixed model")
    candidate_path = checked_declared_file(
        declaration["candidate"], "candidate model")
    renderer_config_path = checked_declared_file(
        declaration["renderer_base_config"], "renderer base config")
    if not any(contained_by(fixed_path, row) for row in repositories):
        raise ManifestError("fixed model must be inside a declared repository")
    for field, local_path in (
            ("candidate model", candidate_path),
            ("renderer base config", renderer_config_path)):
        if any(contained_by(local_path, row) for row in repositories):
            raise ManifestError(field + " must stay outside repositories")
    renderer_config = load_json(renderer_config_path)
    permissions = renderer_config.get("permissions")
    if (type(permissions) is not dict or
            permissions.get("write_profile") is not False):
        raise ManifestError("renderer base config must forbid profile writes")
    if "joint_candidate" in renderer_config:
        raise ManifestError("renderer base config already has a joint candidate")

    tools = declaration["tools"]
    exact_fields(tools, {
        "analyzer", "fit_selector", "manifest_builder", "renderer_builder",
    }, "joint v2 tools")
    tool_paths = {
        name: checked_declared_file(value, "tool " + name)
        for name, value in tools.items()
    }
    running_builder = Path(__file__).resolve(strict=True)
    if tool_paths["manifest_builder"].resolve(strict=True) != running_builder:
        raise ManifestError("declared manifest builder is not this builder")

    fit_rows = declaration["fit_references"]
    if type(fit_rows) is not list or len(fit_rows) != len(JOINT_TARGETS):
        raise ManifestError("joint v2 fit references must name four targets")
    fit_by_target = {}
    for index, row in enumerate(fit_rows):
        exact_fields(row, {"path", "target"},
                     "fit reference {}".format(index))
        target = row["target"]
        if target not in JOINT_TARGETS or target in fit_by_target:
            raise ManifestError("joint v2 fit reference targets differ")
        value = row["path"]
        if type(value) is not str or not Path(value).is_absolute():
            raise ManifestError("fit reference path must be absolute")
        fit_by_target[target] = regular(Path(value),
                                        "fit reference " + target)
    if set(fit_by_target) != set(JOINT_TARGETS):
        raise ManifestError("joint v2 fit reference targets differ")

    selection_rows = declaration["fit_selections"]
    if (type(selection_rows) is not list or
            len(selection_rows) != len(JOINT_TARGETS)):
        raise ManifestError("joint v2 fit selections must name four targets")
    selection_by_target = {}
    for index, row in enumerate(selection_rows):
        exact_fields(row, {"path", "sha256", "target"},
                     "fit selection {}".format(index))
        target = row["target"]
        if target not in JOINT_TARGETS or target in selection_by_target:
            raise ManifestError("joint v2 fit selection targets differ")
        value = row["path"]
        if type(value) is not str or not Path(value).is_absolute():
            raise ManifestError("fit selection path must be absolute")
        selection_path = regular(Path(value), "fit selection " + target)
        declared_hash = checked_digest(row["sha256"], "fit selection hash")
        if declared_hash != JOINT_SELECTION_SHA256[target]:
            raise ManifestError("fit selection is not the frozen result: " + target)
        if sha256(selection_path) != declared_hash:
            raise ManifestError("fit selection hash differs: " + target)
        selection_by_target[target] = selection_path
    if set(selection_by_target) != set(JOINT_TARGETS):
        raise ManifestError("joint v2 fit selection targets differ")

    fit_contract = expected_v2_contract()
    fit_source_groups = {
        row["source_group"] for row in fit_contract["fit_references"]
    }
    if audit["source_group_id"] in fit_source_groups:
        raise ManifestError("audit source group reuses the fit source group")
    fit_inodes = {
        (path.stat().st_dev, path.stat().st_ino) for path in fit_by_target.values()
    }
    fit_hashes = {sha256(path) for path in fit_by_target.values()}
    fit_hashes.update(row["sha256"] for row in fit_contract["fit_references"])

    roster = declaration["validation_roster"]
    if type(roster) is not list or len(roster) != len(JOINT_TARGETS):
        raise ManifestError("joint v2 roster must name four exact open targets")
    roster_by_target = {}
    archives = []
    licenses = []
    source_ids = set()
    source_paths = set()
    member_paths = set()
    for index, row in enumerate(roster):
        exact_fields(row, {"expected_hz", "note", "source", "target"},
                     "validation roster {}".format(index))
        target = row["target"]
        expected = JOINT_OPEN_TARGETS.get(target)
        if (expected is None or target in roster_by_target or
                row["note"] != expected["note"] or
                row["expected_hz"] != expected["expected_hz"]):
            raise ManifestError(
                "validation roster must use each exact open target once")
        source = row["source"]
        exact_fields(source, {
            "archive", "bits_per_sample", "bytes", "channels", "frames",
            "id", "license", "member", "path", "sample_rate_hz", "sha256",
            "source_group_id", "string_assignment_evidence", "whole_file",
        }, "validation source " + target)
        if (source["source_group_id"] != audit["source_group_id"] or
                type(source["source_group_id"]) is not str):
            raise ManifestError("validation roster must use one source group")
        if source["string_assignment_evidence"] != \
                JOINT_V2_STRING_ASSIGNMENT_EVIDENCE:
            raise ManifestError(
                "validation source must remain an open-pitch transfer proxy")
        if (source["whole_file"] is not True or
                source["channels"] != 1 or source["sample_rate_hz"] != 48000 or
                source["bits_per_sample"] not in (16, 24) or
                type(source["frames"]) is not int or source["frames"] < 1 or
                type(source["bytes"]) is not int or source["bytes"] < 1):
            raise ManifestError("validation source has invalid WAVE facts")
        identifier = source["id"]
        source_value = source["path"]
        if (type(identifier) is not str or not identifier or
                identifier in source_ids or type(source_value) is not str or
                not Path(source_value).is_absolute()):
            raise ManifestError("validation source IDs and paths must be unique")
        source_path = regular(Path(source_value),
                              "validation source " + target)
        if (str(source_path.resolve()) in source_paths or
                any(contained_by(source_path, repo) for repo in repositories)):
            raise ManifestError("validation source paths must be distinct and "
                                "outside repositories")
        digest = checked_digest(source["sha256"],
                                "validation source hash")
        source_stat = source_path.stat()
        if ((source_stat.st_dev, source_stat.st_ino) in fit_inodes or
                digest in fit_hashes):
            raise ManifestError("validation source reuses a fit source")
        if (sha256(source_path) != digest or
                source_path.stat().st_size != source["bytes"]):
            raise ManifestError("validation source hash or size differs")
        archive = source["archive"]
        exact_fields(archive, {"bytes", "md5", "name", "url"},
                     "validation archive")
        if (type(archive["bytes"]) is not int or archive["bytes"] < 1 or
                type(archive["name"]) is not str or not archive["name"] or
                type(archive["url"]) is not str or
                not archive["url"].startswith("https://") or
                type(archive["md5"]) is not str or
                re.fullmatch(r"[0-9a-f]{32}", archive["md5"]) is None):
            raise ManifestError("validation archive provenance differs")
        member = source["member"]
        exact_fields(member, {"bytes", "path", "sha256"},
                     "validation archive member")
        if (member["bytes"] != source["bytes"] or
                member["sha256"] != digest or
                type(member["path"]) is not str or not member["path"] or
                Path(member["path"]).is_absolute() or
                ".." in Path(member["path"]).parts or
                member["path"] in member_paths):
            raise ManifestError("validation archive member differs from source")
        license_row = source["license"]
        exact_fields(license_row, {
            "archive_description_spdx", "archive_record_url",
            "archive_structured_metadata_spdx", "member_page_spdx",
            "member_page_url", "status",
        }, "validation source license")
        if (license_row["archive_description_spdx"] != "CC-BY-NC-4.0" or
                license_row["archive_structured_metadata_spdx"] != "CC-BY-4.0" or
                license_row["member_page_spdx"] != "CC-BY-3.0" or
                license_row["status"] !=
                "conflicting-source-metadata-private-analysis-only" or
                not str(license_row["archive_record_url"]).startswith("https://") or
                not str(license_row["member_page_url"]).startswith("https://")):
            raise ManifestError("validation source must retain its license conflict")
        source_ids.add(identifier)
        source_paths.add(str(source_path.resolve()))
        member_paths.add(member["path"])
        archives.append(archive)
        licenses.append(license_row)
        roster_by_target[target] = row
    if set(roster_by_target) != set(JOINT_TARGETS):
        raise ManifestError("joint v2 roster must name four exact open targets")
    if any(row != archives[0] for row in archives[1:]):
        raise ManifestError("validation roster must use one archive")
    shared_license_fields = (
        "archive_description_spdx", "archive_record_url",
        "archive_structured_metadata_spdx", "member_page_spdx", "status",
    )
    if any(
            any(row[name] != licenses[0][name]
                for name in shared_license_fields)
            for row in licenses[1:]):
        raise ManifestError("validation roster must use one license record")

    for target in JOINT_TARGETS:
        fit_path = fit_by_target[target]
        if any(contained_by(fit_path, repo) for repo in repositories):
            raise ManifestError("fit references must stay outside repositories")
        fit_row = v2_reference_for(target, fit_contract)
        checked_joint_selection(
            selection_by_target[target], target,
            next(row["sha256"] for row in selection_rows
                 if row["target"] == target),
            fit_row["id"], sha256(fit_path), declaration["gates"],
            declaration["tools"]["analyzer"]["sha256"],
            declaration["tools"]["fit_selector"]["sha256"],
            declaration["fixed_model"]["sha256"], fit_contract)

    fixed = load_json(fixed_path)
    candidate = load_json(candidate_path)
    expected_candidate = applied_joint_candidate(
        fixed, joint_candidate_changes({
            target: next(item["sha256"] for item in selection_rows
                         if item["target"] == target)
            for target in JOINT_TARGETS
        }))
    if candidate != expected_candidate:
        raise ManifestError("candidate model has changes outside the frozen set")
    return declaration


def joint_renderer_cases(references: dict[str, Any]) -> dict[str, Any]:
    result = {}
    for target in JOINT_TARGETS:
        for split in ("fit", "check", "audit"):
            reference = references[target][
                "audit" if split == "audit" else "fit"]
            name = joint_case_id(target, split)
            result[name] = {
                "articulation": 5, "binding_id": reference["id"],
                "binding_sha256": reference["sha256"], "force": 0.75,
                "frequency_hz": reference["expected_hz"], "joint": True,
                "position": 0.12, "sample_rate": 48000, "speed": 0.8,
                "split": split, "string": V2_STRING_INDEX[target] + 1,
            }
    return result


def run_renderer_command(arguments: list[str], field: str,
                         cwd: Path) -> subprocess.CompletedProcess[str]:
    try:
        completed = subprocess.run(
            arguments, check=False, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, cwd=cwd,
            env={"LC_ALL": "C", "LANG": "C", "TZ": "UTC"}, timeout=120)
    except subprocess.TimeoutExpired as error:
        raise ManifestError(field + " timed out") from error
    except OSError as error:
        raise ManifestError("cannot run {}: {}".format(field, error)) from error
    return completed


def checked_renderer_description(renderer: Path,
                                 renderer_config: dict[str, Any]) -> None:
    completed = run_renderer_command(
        [str(renderer), "--describe"], "renderer description", renderer.parent)
    if completed.returncode != 0 or completed.stderr:
        detail = (completed.stderr.strip() or completed.stdout.strip())[-2000:]
        raise ManifestError("built renderer description failed" +
                            (": " + detail if detail else ""))
    if len(completed.stdout.encode("utf-8")) > MAX_JSON_BYTES:
        raise ManifestError("renderer description exceeds its byte limit")
    try:
        description = json.loads(
            completed.stdout, object_pairs_hook=unique_object,
            parse_constant=reject_constant)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ManifestError("built renderer returned an invalid description") \
            from error
    exact_fields(description, {
        "adapter_id", "compiler", "joint_candidate", "permissions",
        "platform", "resources", "schema", "schema_version",
    }, "renderer description")
    if (description["schema"] != RENDERER_SCHEMA or
            description["schema_version"] != RENDERER_SCHEMA_VERSION or
            description["adapter_id"] != RENDERER_ADAPTER_ID or
            description["platform"] != sys.platform or
            description["permissions"] != renderer_config["permissions"] or
            description["joint_candidate"] !=
            renderer_config["joint_candidate"]):
        raise ManifestError("built renderer description differs")
    exact_fields(description["compiler"], {"macos_sdk_root"},
                 "renderer compiler description")
    resources = description["resources"]
    if type(resources) is not list or not resources:
        raise ManifestError("built renderer has no resources")
    resource_ids = set()
    for index, row in enumerate(resources):
        exact_fields(row, {"id", "path", "sha256"},
                     "renderer resource {}".format(index))
        identifier = row["id"]
        if type(identifier) is not str or not identifier or identifier in resource_ids:
            raise ManifestError("built renderer resource IDs differ")
        path_value = row["path"]
        if type(path_value) is not str or not Path(path_value).is_absolute():
            raise ManifestError("built renderer resource path differs")
        resource = regular(Path(path_value), "built renderer resource")
        if sha256(resource) != checked_digest(
                row["sha256"], "built renderer resource hash"):
            raise ManifestError("built renderer resource hash differs")
        resource_ids.add(identifier)
    if not RENDERER_REQUIRED_RESOURCES.issubset(resource_ids):
        raise ManifestError("built renderer lacks a required resource")


def checked_renderer_profile(renderer: Path, profile: Path) -> None:
    completed = run_renderer_command(
        [str(renderer), "--validate-profile", str(profile)],
        "renderer profile validation", renderer.parent)
    if completed.returncode != 0 or completed.stdout or completed.stderr:
        detail = (completed.stderr.strip() or completed.stdout.strip())[-2000:]
        raise ManifestError("built renderer rejected the candidate profile" +
                            (": " + detail if detail else ""))
    invalid = renderer.parent / "invalid-profile.json"
    write_private(invalid, b"{}\n")
    try:
        completed = run_renderer_command(
            [str(renderer), "--validate-profile", str(invalid)],
            "renderer invalid-profile probe", renderer.parent)
        if completed.returncode == 0:
            raise ManifestError("built renderer accepted an invalid profile")
    finally:
        invalid.unlink()


def run_renderer_builder(builder: Path, config: Path, output: Path,
                         profile: Path,
                         renderer_config: dict[str, Any]) -> str:
    config_hash = sha256(config)
    profile_hash = sha256(profile)
    try:
        completed = subprocess.run(
            [sys.executable, "-I", str(builder), "build", "--config",
             str(config), "--output", str(output)], check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, cwd=output.parent,
            env={"LC_ALL": "C", "LANG": "C", "TZ": "UTC"}, timeout=120)
    except subprocess.TimeoutExpired as error:
        raise ManifestError("renderer build timed out") from error
    except OSError as error:
        raise ManifestError("cannot build renderer: " + str(error)) from error
    if completed.returncode != 0:
        detail = (completed.stderr.strip() or completed.stdout.strip())[-2000:]
        raise ManifestError("renderer build failed" +
                            (": " + detail if detail else ""))
    regular(output, "built renderer")
    renderer_hash = sha256(output)
    checked_renderer_description(output, renderer_config)
    checked_renderer_profile(output, profile)
    if (sha256(output) != renderer_hash or sha256(config) != config_hash or
            sha256(profile) != profile_hash):
        raise ManifestError("renderer build or probe changed an input")
    expected = {config.name, output.name, profile.name}
    if ({path.name for path in output.parent.iterdir()} != expected or
            any(path.is_symlink() or not path.is_file()
                for path in output.parent.iterdir())):
        raise ManifestError("renderer builder left unexpected files")
    return renderer_hash


def checked_bundle_files(directory: Path, values: dict[str, Any],
                         renderer_hash: str,
                         receipt: dict[str, Any] | None = None) -> dict[str, str]:
    expected = set(values) | {"renderer"}
    if receipt is not None:
        expected.add("receipt.json")
    children = list(directory.iterdir())
    if ({path.name for path in children} != expected or
            any(path.is_symlink() or not path.is_file() for path in children)):
        raise ManifestError("joint v2 bundle members differ")
    hashes = {}
    for name, value in values.items():
        path = directory / name
        source = json_bytes(value)
        if path.read_bytes() != source or load_json(path) != value:
            raise ManifestError("joint v2 bundle JSON differs: " + name)
        hashes[name] = sha256(path)
    if sha256(directory / "renderer") != renderer_hash:
        raise ManifestError("joint v2 bundle renderer differs")
    hashes["renderer"] = renderer_hash
    if receipt is not None:
        if load_json(directory / "receipt.json") != receipt:
            raise ManifestError("joint v2 bundle receipt differs")
        if receipt.get("files") != hashes:
            raise ManifestError("joint v2 bundle receipt hashes differ")
    return hashes


def build_joint_validation_v2_bundle(
        arguments: argparse.Namespace) -> dict[str, Any]:
    output = checked_output_path(arguments.output_dir)
    declaration_path = regular(arguments.declaration,
                               "joint v2 declaration")
    declaration_hash = sha256(declaration_path)
    declaration = checked_joint_declaration_v2(declaration_path)
    tools = {
        name: Path(row["path"])
        for name, row in declaration["tools"].items()
    }
    analyzer = tools["analyzer"]
    contract = expected_v2_contract()
    fit_paths = {
        row["target"]: Path(row["path"])
        for row in declaration["fit_references"]
    }
    selection_rows = {
        row["target"]: row for row in declaration["fit_selections"]
    }
    roster_rows = {
        row["target"]: row for row in declaration["validation_roster"]
    }
    references = {}
    evidence = {}
    expected_losses = {}
    for target in JOINT_TARGETS:
        fit_row = v2_reference_for(target, contract)
        fit_path = fit_paths[target]
        fit_evidence = v2_reference_evidence(analyzer, fit_path, fit_row)
        audit_roster = roster_rows[target]
        audit_row = dict(audit_roster["source"])
        audit_row["expected_hz"] = audit_roster["expected_hz"]
        audit_path = Path(audit_row["path"])
        audit_evidence = v2_reference_evidence(
            analyzer, audit_path, audit_row)
        if (audit_evidence["harmonic_decay"]["reference"]
                ["valid_band_count"] <
                declaration["gates"]["minimum_valid_harmonics"]):
            raise ManifestError("validation reference has too few harmonics: " +
                                target)
        selection_path = Path(selection_rows[target]["path"])
        unused_selection, expected_loss = checked_joint_selection(
            selection_path, target, selection_rows[target]["sha256"],
            fit_row["id"], fit_row["sha256"], declaration["gates"],
            declaration["tools"]["analyzer"]["sha256"],
            declaration["tools"]["fit_selector"]["sha256"],
            declaration["fixed_model"]["sha256"], contract)
        expected_losses[target] = expected_loss
        references[target] = {
            "fit": {
                "expected_hz": fit_row["expected_hz"], "id": fit_row["id"],
                "path": str(fit_path), "sha256": fit_row["sha256"],
            },
            "audit": {
                "expected_hz": audit_roster["expected_hz"],
                "id": audit_row["id"], "path": str(audit_path),
                "sha256": audit_row["sha256"],
            },
        }
        evidence[target] = {
            "audit": audit_evidence, "fit": fit_evidence,
        }

    selection_hashes = {
        target: selection_rows[target]["sha256"] for target in JOINT_TARGETS
    }
    changes = joint_candidate_changes(selection_hashes)
    fixed_model = load_json(Path(declaration["fixed_model"]["path"]))
    candidate_model = applied_joint_candidate(fixed_model, changes)
    candidate_source = (
        json.dumps(candidate_model, indent=2, allow_nan=False) + "\n"
    ).encode("utf-8")
    candidate_hash = hashlib.sha256(candidate_source).hexdigest()
    experiment = joint_experiment(references)
    base_config = load_json(Path(declaration["renderer_base_config"]["path"]))
    renderer_config = dict(base_config)
    renderer_config["joint_candidate"] = {
        "adapter_id": JOINT_V2_ADAPTER_ID,
        "candidate_profile_sha256": candidate_hash,
        "cases": joint_renderer_cases(references), "changes": changes,
    }
    bindings = {
        "bindings": [
            {
                "id": role["id"], "path": role["path"],
                "sha256": role["sha256"],
            }
            for target in JOINT_TARGETS
            for role in (references[target]["fit"],
                         references[target]["audit"])
        ],
        "schema": "hwa-fit-bindings", "schema_version": 1,
    }
    validation_receipt = []
    for target in JOINT_TARGETS:
        copy = json.loads(json.dumps(roster_rows[target], allow_nan=False))
        copy["source"].pop("path")
        copy["preflight"] = {
            "harmonic_decay_method": evidence[target]["audit"]
                ["harmonic_decay"]["method"],
            "pitch_method": evidence[target]["audit"]
                ["isolated_note"]["method"],
            "valid_harmonic_count": evidence[target]["audit"]
                ["harmonic_decay"]["reference"]["valid_band_count"],
        }
        validation_receipt.append(copy)

    builder_scratch = Path(tempfile.mkdtemp(
        prefix=".{}-renderer-".format(output.name), dir=output.parent))
    try:
        scratch = Path(tempfile.mkdtemp(
            prefix=".{}-bundle-".format(output.name), dir=output.parent))
    except BaseException:
        shutil.rmtree(builder_scratch, ignore_errors=True)
        raise
    try:
        child_config = builder_scratch / "renderer-config.json"
        child_profile = builder_scratch / "candidate-profile.json"
        child_renderer = builder_scratch / "renderer"
        write_private(child_config, json_bytes(renderer_config))
        write_private(child_profile, candidate_source)
        renderer_hash = run_renderer_builder(
            tools["renderer_builder"], child_config, child_renderer,
            child_profile, renderer_config)
        fit = joint_fit_manifest(
            references, expected_losses, changes, renderer_hash,
            adapter_id=JOINT_V2_ADAPTER_ID)
        files = {
            "bindings.local.json": bindings,
            "experiment.json": experiment,
            "fit.json": fit,
            "renderer-config.json": renderer_config,
        }
        for name, value in files.items():
            write_private(scratch / name, json_bytes(value))
        renderer = scratch / "renderer"
        os.replace(child_renderer, renderer)
        hashes = checked_bundle_files(scratch, files, renderer_hash)
        receipt = {
            "adapter_id": JOINT_V2_ADAPTER_ID,
            "audit": declaration["audit"], "audio_copied": False,
            "candidate_profile_sha256": candidate_hash,
            "declaration_sha256": declaration_hash, "files": hashes,
            "schema": "hwa-double-bass-joint-validation-bundle",
            "schema_version": 2,
            "validation_references": validation_receipt,
        }
        write_private(scratch / "receipt.json", json_bytes(receipt))
        checked_bundle_files(scratch, files, renderer_hash, receipt)

        if sha256(declaration_path) != declaration_hash:
            raise ManifestError("joint v2 declaration changed during build")
        for name, row in declaration["tools"].items():
            if sha256(Path(row["path"])) != row["sha256"]:
                raise ManifestError("joint v2 tool changed: " + name)
        for row in declaration["fit_selections"]:
            if sha256(Path(row["path"])) != row["sha256"]:
                raise ManifestError("fit selection changed: " + row["target"])
        for target in JOINT_TARGETS:
            if sha256(fit_paths[target]) != references[target]["fit"]["sha256"]:
                raise ManifestError("fit reference changed: " + target)
            source = roster_rows[target]["source"]
            if sha256(Path(source["path"])) != source["sha256"]:
                raise ManifestError("validation source changed: " + target)
        if (sha256(Path(declaration["candidate"]["path"])) !=
                declaration["candidate"]["sha256"] or
                sha256(Path(declaration["fixed_model"]["path"])) !=
                declaration["fixed_model"]["sha256"] or
                sha256(Path(declaration["renderer_base_config"]["path"])) !=
                declaration["renderer_base_config"]["sha256"]):
            raise ManifestError("joint v2 model or config changed during build")
        publish_directory(scratch, output)
    except BaseException:
        shutil.rmtree(scratch, ignore_errors=True)
        raise
    finally:
        shutil.rmtree(builder_scratch, ignore_errors=True)
    return {
        "audit_scope": "open-pitch-transfer-proxy", "audio_copied": False,
        "output": str(output),
        "source_group_id": declaration["audit"]["source_group_id"],
        "targets": list(JOINT_TARGETS),
    }


def checked_reference_contract(path: Path) -> dict[str, Any]:
    contract = load_json(path)
    wanted = expected_contract()
    if contract != wanted:
        raise ManifestError("reference contract changed")
    return contract


def validate_contracts(fit_path: Path, contract_path: Path) -> dict[str, Any]:
    if load_json(fit_path) != expected_fit():
        raise ManifestError("fit manifest changed")
    contract = checked_reference_contract(contract_path)
    return {
        "adapter_id": "hlolli_wg_double_bass-passive-open-v1",
        "check_cases": [row["id"] for row in contract["derived_bindings"]],
        "fit_cases": [row["id"] for row in contract["fit_references"]],
        "heldout_sources": [row["id"]
                            for row in contract["heldout_sources"]],
        "schema": "hwa-double-bass-manifest-contract",
        "schema_version": 1,
    }


def require_file_facts(path: Path, row: dict[str, Any], field: str,
                       analyzer: Path
                       ) -> tuple[Path, dict[str, Any]]:
    path = regular(path, field)
    facts = wave_facts(path, field, analyzer)
    for name in ("bits_per_sample", "channels", "frames", "sample_rate_hz",
                 "sha256"):
        if facts[name] != row[name]:
            raise ManifestError("{} has wrong {}".format(field, name))
    return path, facts


def binding_arguments(values: Sequence[str], rows: list[dict[str, Any]],
                      field: str) -> dict[str, Path]:
    result = {}
    for value in values:
        if "=" not in value:
            raise ManifestError(field + " must use ID=PATH")
        identifier, path = value.split("=", 1)
        if not identifier or not path:
            raise ManifestError(field + " must use ID=PATH")
        if identifier in result:
            raise ManifestError("duplicate {} ID: {}".format(
                field, identifier))
        result[identifier] = Path(path)
    expected = {row["id"] for row in rows}
    found = set(result)
    if found != expected:
        raise ManifestError(
            "{} IDs differ; extra={} missing={}".format(
                field, sorted(found - expected), sorted(expected - found)))
    return result


def analyzer_version(analyzer: Path) -> str:
    try:
        completed = subprocess.run(
            [str(analyzer), "--version"], check=False,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            env={"LC_ALL": "C", "LANG": "C", "TZ": "UTC"}, timeout=10)
    except subprocess.TimeoutExpired as error:
        raise ManifestError("analyzer version check timed out") from error
    if completed.returncode != 0:
        raise ManifestError("analyzer version check failed")
    match = re.search(r"\bhlolli-wg-analyzer [0-9A-Za-z._+-]+\b",
                      completed.stdout + "\n" + completed.stderr)
    if match is None:
        raise ManifestError("analyzer returned an unknown version")
    return match.group(0)


def ffmpeg_version(ffmpeg: Path) -> str:
    try:
        completed = subprocess.run(
            [str(ffmpeg), "-version"], check=False, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True,
            env={"LC_ALL": "C", "LANG": "C", "TZ": "UTC"}, timeout=10)
    except subprocess.TimeoutExpired as error:
        raise ManifestError("FFmpeg version check timed out") from error
    if completed.returncode != 0:
        raise ManifestError("FFmpeg version check failed")
    match = re.search(r"\bffmpeg version [0-9A-Za-z._+-]+\b",
                      completed.stdout + "\n" + completed.stderr)
    if match is None:
        raise ManifestError("FFmpeg returned an unknown version")
    return match.group(0)


def run_ffmpeg(ffmpeg: Path, source: Path, output: Path,
               template: list[str]) -> None:
    arguments = [
        str(source) if item == "{source}" else
        str(output) if item == "{derived}" else item
        for item in template
    ]
    try:
        completed = subprocess.run(
            [str(ffmpeg), *arguments], check=False, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, cwd=output.parent,
            env={"LC_ALL": "C", "LANG": "C", "TZ": "UTC",
                 "TMPDIR": str(output.parent), "TMP": str(output.parent),
                 "TEMP": str(output.parent)}, timeout=120)
    except subprocess.TimeoutExpired as error:
        raise ManifestError("FFmpeg conversion timed out") from error
    if completed.returncode != 0:
        detail = (completed.stderr.strip() or completed.stdout.strip())[-2000:]
        raise ManifestError("FFmpeg conversion failed" +
                            (": " + detail if detail else ""))


def files_equal(first: Path, second: Path) -> bool:
    if first.stat().st_size != second.stat().st_size:
        return False
    with first.open("rb") as one, second.open("rb") as two:
        while True:
            left = one.read(1024 * 1024)
            right = two.read(1024 * 1024)
            if left != right:
                return False
            if not left:
                return True


def experiment_manifest(input_hashes: dict[str, str]) -> dict[str, Any]:
    def stem(resource_id: str, side: str, input_id: Any,
             output: Any) -> dict[str, Any]:
        return {
            "channels": 1, "gain_db": 0, "id": resource_id,
            "input_id": input_id, "output": output, "rate_hz": 48000,
            "role": "final", "side": side, "start_sample": 0,
        }

    def case(name: str, split: str) -> dict[str, Any]:
        return {
            "id": name, "links": [], "probes": [], "split": split,
            "stems": [
                stem("model.final", "model", None, "model.wav"),
                stem("reference.final", "reference", name, None),
            ],
            "weight": 1,
        }

    parameters = []
    for row in expected_parameters():
        parameters.append({
            "baseline": row["baseline"], "id": row["id"],
            "levels": [], "maximum": row["maximum"],
            "minimum": row["minimum"], "unit": row["unit"],
        })
    derived_ids = [row["id"] for row in expected_derived_bindings()]
    fit_ids = [row["id"] for row in expected_fit_references()]
    return {
        "cases": ([case(identifier, "check") for identifier in derived_ids] +
                  [case(identifier, "fit") for identifier in fit_ids]),
        "clock_rate_hz": 48000,
        "inputs": [{"id": identifier, "sha256": input_hashes[identifier]}
                   for identifier in derived_ids + fit_ids],
        "method_version": "stage8-1",
        "parameters": parameters,
        "plan": {
            "kind": "random", "replicates": 1,
            "sample_count": 32, "seed": 1701,
        },
        "responses": [{
            "feature": "rms_dbfs", "id": "final.rms", "index": 0,
            "role": "final",
        }],
        "schema": "hwa-experiment", "schema_version": 1,
    }


def json_bytes(value: Any) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True, allow_nan=False) +
            "\n").encode("utf-8")


def write_private(path: Path, source: bytes) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(source)
        stream.flush()
        os.fsync(stream.fileno())


def build_bundle(arguments: argparse.Namespace) -> dict[str, Any]:
    builder_path = regular(Path(__file__).resolve(), "manifest builder")
    builder_hash = sha256(builder_path)
    fit_path = regular(arguments.fit_manifest, "fit manifest")
    fit = load_json(arguments.fit_manifest)
    if fit != expected_fit():
        raise ManifestError("fit manifest changed")
    contract_path = regular(arguments.reference_contract,
                            "reference contract")
    contract = checked_reference_contract(contract_path)
    fit_arguments = binding_arguments(
        arguments.fit_reference, contract["fit_references"],
        "fit reference")
    heldout_arguments = binding_arguments(
        arguments.heldout_source, contract["heldout_sources"],
        "heldout source")

    try:
        analyzer_path = arguments.analyzer.resolve(strict=True)
    except OSError as error:
        raise ManifestError("cannot resolve analyzer: " + str(error)) from error
    analyzer = regular(analyzer_path, "analyzer")
    analyzer_hash = sha256(analyzer)
    analyzer_release = analyzer_version(analyzer)
    if sha256(analyzer) != analyzer_hash:
        raise ManifestError("analyzer changed during its version check")

    fit_files = {}
    fit_facts = {}
    for row in contract["fit_references"]:
        identifier = row["id"]
        path, facts = require_file_facts(
            fit_arguments[identifier], row,
            "fit reference " + identifier, analyzer)
        fit_files[identifier] = path
        fit_facts[identifier] = facts
    heldout_files = {}
    heldout_facts = {}
    for row in contract["heldout_sources"]:
        identifier = row["id"]
        path, facts = require_file_facts(
            heldout_arguments[identifier], row,
            "heldout source " + identifier, analyzer)
        heldout_files[identifier] = path
        heldout_facts[identifier] = facts

    try:
        ffmpeg_path = arguments.ffmpeg.resolve(strict=True)
    except OSError as error:
        raise ManifestError("cannot resolve FFmpeg: " + str(error)) from error
    ffmpeg = regular(ffmpeg_path, "FFmpeg")
    ffmpeg_hash = sha256(ffmpeg)
    version = ffmpeg_version(ffmpeg)
    if sha256(ffmpeg) != ffmpeg_hash:
        raise ManifestError("FFmpeg changed during its version check")
    fit_file_hash = sha256(fit_path)
    contract_file_hash = sha256(contract_path)
    output = arguments.output_dir.absolute()
    if output.exists() or output.is_symlink() or not output.parent.is_dir():
        raise ManifestError(
            "output directory must be new with an existing parent")

    def check_inputs_unchanged() -> None:
        if sha256(builder_path) != builder_hash:
            raise ManifestError("manifest builder changed during conversion")
        if sha256(analyzer) != analyzer_hash:
            raise ManifestError("analyzer changed during conversion")
        if sha256(ffmpeg) != ffmpeg_hash:
            raise ManifestError("FFmpeg changed during conversion")
        if (sha256(fit_path) != fit_file_hash or
                sha256(contract_path) != contract_file_hash):
            raise ManifestError("checked manifest input changed")
        for identifier, path in fit_files.items():
            if sha256(path) != fit_facts[identifier]["sha256"]:
                raise ManifestError("fit reference changed: " + identifier)
        for identifier, path in heldout_files.items():
            if sha256(path) != heldout_facts[identifier]["sha256"]:
                raise ManifestError("heldout source changed: " + identifier)

    with tempfile.TemporaryDirectory(
            prefix=".hwa-double-bass-manifest-", dir=output.parent) as text:
        scratch = Path(text)
        template = contract["transform"]["argv"]
        derived_files = {}
        derived_facts = {}
        for row in contract["derived_bindings"]:
            identifier = row["id"]
            source_id = row["source_id"]
            source = heldout_files[source_id]
            first = scratch / (identifier + "-first.wav")
            second = scratch / (identifier + "-second.wav")
            run_ffmpeg(ffmpeg, source, first, template)
            check_inputs_unchanged()
            run_ffmpeg(ffmpeg, source, second, template)
            check_inputs_unchanged()
            first_facts = wave_facts(
                first, "first derived heldout binding " + identifier,
                analyzer)
            second_facts = wave_facts(
                second, "second derived heldout binding " + identifier,
                analyzer)
            if (first_facts != second_facts or
                    not files_equal(first, second)):
                raise ManifestError(
                    "FFmpeg conversion is not byte-repeatable: " + identifier)
            facts = first_facts
            source_facts = heldout_facts[source_id]
            expected_frames = (
                source_facts["frames"] * row["sample_rate_hz"] +
                source_facts["sample_rate_hz"] // 2
            ) // source_facts["sample_rate_hz"]
            for name in ("bits_per_sample", "channels", "sample_rate_hz"):
                if facts[name] != row[name]:
                    raise ManifestError(
                        "derived binding {} has wrong {}".format(
                            identifier, name))
            if facts["frames"] != expected_frames:
                raise ManifestError(
                    "derived binding has wrong frame count: " + identifier)
            derived_files[identifier] = first
            derived_facts[identifier] = facts
        check_inputs_unchanged()

        input_hashes = {
            identifier: facts["sha256"]
            for identifier, facts in derived_facts.items()
        }
        input_hashes.update({
            identifier: facts["sha256"]
            for identifier, facts in fit_facts.items()
        })
        experiment_source = json_bytes(experiment_manifest(input_hashes))
        fit_source = json_bytes(fit)
        fit_receipt = []
        for row in contract["fit_references"]:
            copy = dict(row)
            copy["file_bytes"] = fit_facts[row["id"]]["file_bytes"]
            fit_receipt.append(copy)
        heldout_receipt = []
        for row in contract["heldout_sources"]:
            copy = dict(row)
            copy["file_bytes"] = heldout_facts[row["id"]]["file_bytes"]
            heldout_receipt.append(copy)
        derived_receipt = []
        for row in contract["derived_bindings"]:
            facts = derived_facts[row["id"]]
            copy = dict(row)
            copy.update({
                "file_bytes": facts["file_bytes"], "frames": facts["frames"],
                "sha256": facts["sha256"],
            })
            derived_receipt.append(copy)
        receipt = {
            "analyzer": {
                "path": str(analyzer), "sha256": analyzer_hash,
                "version": analyzer_release,
            },
            "adapter_id": fit["adapter_id"],
            "builder_sha256": builder_hash,
            "derived_bindings": derived_receipt,
            "experiment_sha256": hashlib.sha256(experiment_source).hexdigest(),
            "fit_manifest_sha256": hashlib.sha256(fit_source).hexdigest(),
            "fit_references": fit_receipt,
            "heldout_sources": heldout_receipt,
            "reference_contract_sha256": contract_file_hash,
            "schema": "hwa-double-bass-manifest-bundle",
            "schema_version": 1,
            "transform": {
                "argv": template, "method": contract["transform"]["method"],
                "tool": {
                    "path": str(ffmpeg), "sha256": ffmpeg_hash,
                    "version": version,
                },
            },
        }
        bindings_rows = []
        for row in contract["derived_bindings"]:
            identifier = row["id"]
            bindings_rows.append({
                "id": identifier,
                "path": str(output / (identifier + ".wav")),
                "sha256": derived_facts[identifier]["sha256"],
            })
        for row in contract["fit_references"]:
            identifier = row["id"]
            bindings_rows.append({
                "id": identifier, "path": str(fit_files[identifier]),
                "sha256": fit_facts[identifier]["sha256"],
            })
        bindings = {
            "bindings": bindings_rows,
            "schema": "hwa-fit-bindings", "schema_version": 1,
        }
        try:
            output.mkdir(mode=0o700)
        except FileExistsError as error:
            raise ManifestError("output directory already exists") from error
        os.chmod(output, 0o700)
        for row in contract["derived_bindings"]:
            identifier = row["id"]
            destination = output / (identifier + ".wav")
            os.link(derived_files[identifier], destination,
                    follow_symlinks=False)
            os.chmod(destination, 0o600)
        write_private(output / "experiment.json", experiment_source)
        write_private(output / "fit.json", fit_source)
        write_private(output / "bindings.local.json", json_bytes(bindings))
        write_private(output / "receipt.json", json_bytes(receipt))
        check_inputs_unchanged()
        directory_descriptor = os.open(output, os.O_RDONLY)
        try:
            os.fsync(directory_descriptor)
        finally:
            os.close(directory_descriptor)
        parent_descriptor = os.open(output.parent, os.O_RDONLY)
        try:
            os.fsync(parent_descriptor)
        finally:
            os.close(parent_descriptor)
    return {
        "derived_bindings": [
            {"id": row["id"],
             "sha256": derived_facts[row["id"]]["sha256"]}
            for row in contract["derived_bindings"]
        ],
        "output": str(output),
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)
    validate = commands.add_parser("validate")
    validate.add_argument("--fit-manifest", type=Path, default=DEFAULT_FIT)
    validate.add_argument("--reference-contract", type=Path,
                          default=DEFAULT_CONTRACT)
    validate_v2 = commands.add_parser("validate-v2")
    validate_v2.add_argument("--reference-contract", type=Path,
                             default=DEFAULT_V2_CONTRACT)
    for target in V2_TARGETS:
        validate_v2.add_argument(
            "--fit-{}-manifest".format(target), type=Path,
            default=DEFAULT_V2_FITS[target])
    validate_d = commands.add_parser("validate-d-frequency-v3")
    validate_d.add_argument("--reference-contract", type=Path,
                            default=DEFAULT_V2_CONTRACT)
    validate_d.add_argument("--fit-manifest", type=Path,
                            default=DEFAULT_D_FREQUENCY_FIT)
    build = commands.add_parser("build")
    build.add_argument("--fit-manifest", type=Path, default=DEFAULT_FIT)
    build.add_argument("--reference-contract", type=Path,
                       default=DEFAULT_CONTRACT)
    build.add_argument(
        "--fit-reference", action="append", default=[], metavar="ID=PATH",
        required=True)
    build.add_argument(
        "--heldout-source", action="append", default=[], metavar="ID=PATH",
        required=True)
    build.add_argument("--analyzer", type=Path, required=True)
    build.add_argument("--ffmpeg", type=Path, required=True)
    build.add_argument("--output-dir", type=Path, required=True)
    build_v2 = commands.add_parser("build-v2")
    build_v2.add_argument("--reference-contract", type=Path,
                          default=DEFAULT_V2_CONTRACT)
    build_v2.add_argument(
        "--fit-manifest", action="append", default=[], metavar="TARGET=PATH")
    build_v2.add_argument(
        "--fit-reference", action="append", default=[], metavar="ID=PATH",
        required=True)
    build_v2.add_argument("--analyzer", type=Path, required=True)
    build_v2.add_argument("--output-dir", type=Path, required=True)
    build_d = commands.add_parser("build-d-frequency-v3")
    build_d.add_argument("--reference-contract", type=Path,
                         default=DEFAULT_V2_CONTRACT)
    build_d.add_argument("--fit-manifest", type=Path,
                         default=DEFAULT_D_FREQUENCY_FIT)
    build_d.add_argument(
        "--fit-reference", action="append", default=[], metavar="ID=PATH",
        required=True)
    build_d.add_argument("--analyzer", type=Path, required=True)
    build_d.add_argument("--output-dir", type=Path, required=True)
    build_joint_v2 = commands.add_parser("build-joint-validation-v2")
    build_joint_v2.add_argument("--declaration", type=Path, required=True)
    build_joint_v2.add_argument("--output-dir", type=Path, required=True)
    return result


def main(argv: Sequence[str] | None = None) -> int:
    try:
        arguments = parser().parse_args(argv)
        if arguments.command == "validate":
            summary = validate_contracts(
                arguments.fit_manifest, arguments.reference_contract)
        elif arguments.command == "validate-v2":
            summary = validate_v2_contracts(
                arguments.reference_contract,
                {target: getattr(arguments, "fit_{}_manifest".format(target))
                 for target in V2_TARGETS})
        elif arguments.command == "validate-d-frequency-v3":
            summary = validate_d_frequency_contract(
                arguments.reference_contract, arguments.fit_manifest)
        elif arguments.command == "build":
            summary = build_bundle(arguments)
        elif arguments.command == "build-v2":
            if not arguments.fit_manifest:
                arguments.fit_manifest = [
                    "{}={}".format(target, DEFAULT_V2_FITS[target])
                    for target in V2_TARGETS
                ]
            summary = build_v2_bundle(arguments)
        elif arguments.command == "build-d-frequency-v3":
            summary = build_d_frequency_bundle(arguments)
        else:
            summary = build_joint_validation_v2_bundle(arguments)
        print(json.dumps(summary, sort_keys=True, allow_nan=False))
        return 0
    except (ManifestError, OSError, UnicodeError, ValueError) as error:
        print("double_bass_manifest: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

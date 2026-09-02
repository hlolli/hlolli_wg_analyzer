#!/usr/bin/python3 -I
"""Validate and build the first double-bass passive-loss manifest bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import Any, Sequence


ADAPTER_DIR = Path(__file__).resolve().parent
DEFAULT_FIT = ADAPTER_DIR / "fit-passive-open-v1.json"
DEFAULT_CONTRACT = ADAPTER_DIR / "reference-contract-v1.json"
MAX_JSON_BYTES = 1024 * 1024
MAX_WAVE_BYTES = 64 * 1024 * 1024


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
    return result


def main(argv: Sequence[str] | None = None) -> int:
    try:
        arguments = parser().parse_args(argv)
        if arguments.command == "validate":
            summary = validate_contracts(
                arguments.fit_manifest, arguments.reference_contract)
            print(json.dumps(summary, sort_keys=True, allow_nan=False))
        else:
            summary = build_bundle(arguments)
            print(json.dumps(summary, sort_keys=True, allow_nan=False))
        return 0
    except (ManifestError, OSError, UnicodeError, ValueError) as error:
        print("double_bass_manifest: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

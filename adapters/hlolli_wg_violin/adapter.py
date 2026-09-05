#!/usr/bin/python3
"""Build and run the checked renderer for the fixed violin model."""

from __future__ import annotations

import argparse
from array import array
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from typing import Any, Iterable, NoReturn, Sequence
import wave


sys.dont_write_bytecode = True

EMBEDDED_CONFIG_JSON = None  # HWA_EMBED_CONFIG
ROOT = Path(__file__).resolve().parents[2]
SOURCE_DIR = Path(__file__).resolve().parent
FIT_MANIFEST = SOURCE_DIR / "fit.json"
ADAPTER_ID = "hlolli-wg-violin-v1"
METHOD_VERSION = "stage8-1"
RENDERER_SCHEMA = "hwa-violin-renderer"
RENDER_RESULT_SCHEMA = "hwa-violin-render-result"
BUNDLE_SCHEMA = "hwa-violin-fit-bundle-receipt"
MAX_JSON_BYTES = 1024 * 1024
MAX_MODULE_BYTES = 64 * 1024 * 1024
MODEL_FRAMES = 176416
CLEAN_PATH = "/run/current-system/sw/bin:/usr/bin:/bin"
CONFIG_MARKER = "EMBEDDED_CONFIG_JSON = " + "None  # HWA_EMBED_CONFIG"
MODULE_MARKER = b"hlolli_wg_violin_test_diagnostic_gains"

CASE_SPECS = {
    "body-check": {
        "split": "check", "mode": "phrase", "string": 3,
        "frequency_hz": 440.0, "reference": "reference_body_check",
    },
    "body-fit": {
        "split": "fit", "mode": "phrase", "string": 3,
        "frequency_hz": 440.0, "reference": "reference_body_fit",
    },
    "open-a4": {
        "split": "fit", "mode": "open", "string": 3,
        "frequency_hz": 440.0, "reference": "reference_a4",
    },
    "open-d4": {
        "split": "check", "mode": "open", "string": 2,
        "frequency_hz": 293.664767917408, "reference": "reference_d4",
    },
    "open-e5": {
        "split": "check", "mode": "open", "string": 4,
        "frequency_hz": 659.255113825740, "reference": "reference_e5",
    },
    "open-g3": {
        "split": "fit", "mode": "open", "string": 1,
        "frequency_hz": 195.997717990875, "reference": "reference_g3",
    },
}

REFERENCE_ARGUMENTS = {
    "reference_a4": "reference_a4",
    "reference_body_check": "reference_body_check",
    "reference_body_fit": "reference_body_fit",
    "reference_d4": "reference_d4",
    "reference_e5": "reference_e5",
    "reference_g3": "reference_g3",
}

OPEN_STRING_CASES = ("open-g3", "open-d4", "open-a4", "open-e5")


class AdapterError(ValueError):
    pass


def reject_constant(value: str) -> NoReturn:
    raise AdapterError("non-finite JSON number: " + value)


def unique_object(pairs: Iterable[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise AdapterError("duplicate JSON key: " + key)
        result[key] = value
    return result


def read_bounded(path: Path, maximum: int, name: str) -> bytes:
    path = regular(path, name)
    try:
        with path.open("rb") as stream:
            size = os.fstat(stream.fileno()).st_size
            if size > maximum:
                raise AdapterError(name + " exceeds its byte limit")
            value = stream.read(maximum + 1)
    except OSError as error:
        raise AdapterError("cannot read {}: {}".format(name, error)) from error
    if len(value) > maximum:
        raise AdapterError(name + " exceeds its byte limit")
    return value


def parse_json(source: bytes, name: str) -> dict[str, Any]:
    try:
        value = json.loads(
            source.decode("utf-8"), object_pairs_hook=unique_object,
            parse_constant=reject_constant,
        )
    except (UnicodeError, json.JSONDecodeError) as error:
        raise AdapterError("cannot parse {}: {}".format(name, error)) from error
    if type(value) is not dict:
        raise AdapterError(name + " root must be an object")
    return value


def load_json_evidence(path: Path, name: str = "JSON input"
                       ) -> tuple[dict[str, Any], str]:
    source = read_bounded(path, MAX_JSON_BYTES, name)
    return parse_json(source, name), hashlib.sha256(source).hexdigest()


def load_json(path: Path) -> dict[str, Any]:
    return load_json_evidence(path, str(path))[0]


def canonical_json(value: Any) -> str:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), allow_nan=False,
    )


def write_json(path: Path, value: Any) -> None:
    source = json.dumps(
        value, indent=2, sort_keys=True, allow_nan=False,
    ).encode("utf-8") + b"\n"
    try:
        with path.open("xb") as stream:
            os.fchmod(stream.fileno(), 0o600)
            stream.write(source)
            stream.flush()
            os.fsync(stream.fileno())
    except OSError as error:
        raise AdapterError("cannot write {}: {}".format(path.name, error)) from error


def exact_keys(value: Any, wanted: set[str], name: str) -> dict[str, Any]:
    if type(value) is not dict or set(value) != wanted:
        raise AdapterError(name + " has the wrong fields")
    return value


def exact_value(actual: Any, wanted: Any) -> bool:
    if isinstance(wanted, bool) or isinstance(actual, bool):
        return type(actual) is type(wanted) and actual == wanted
    if isinstance(wanted, (int, float)):
        return (isinstance(actual, (int, float)) and
                not isinstance(actual, bool) and actual == wanted)
    return type(actual) is type(wanted) and actual == wanted


def exact_tree(actual: Any, wanted: Any) -> bool:
    if type(wanted) is dict:
        return (type(actual) is dict and set(actual) == set(wanted) and
                all(exact_tree(actual[key], value)
                    for key, value in wanted.items()))
    if type(wanted) is list:
        return (type(actual) is list and len(actual) == len(wanted) and
                all(exact_tree(left, right)
                    for left, right in zip(actual, wanted)))
    return exact_value(actual, wanted)


def finite(value: Any, name: str) -> float:
    if (not isinstance(value, (int, float)) or isinstance(value, bool) or
            not math.isfinite(float(value))):
        raise AdapterError(name + " must be finite")
    return float(value)


def unsigned(value: Any, name: str, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        raise AdapterError(name + " must be an unsigned integer")
    return value


def regular(path: Path, name: str) -> Path:
    path = path.absolute()
    try:
        status = os.lstat(path)
    except OSError as error:
        raise AdapterError("{} must be a regular file: {}".format(
            name, path)) from error
    if not os.path.isfile(path) or os.path.islink(path):
        raise AdapterError("{} must be a regular file: {}".format(name, path))
    if status.st_size < 1:
        raise AdapterError(name + " must not be empty")
    return path


def executable(path: Path, name: str) -> Path:
    path = regular(path, name)
    if not os.access(path, os.X_OK):
        raise AdapterError(name + " must be executable")
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


def sha256(path: Path) -> str:
    path = regular(path, "hash input")
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise AdapterError("cannot hash {}: {}".format(path, error)) from error
    return digest.hexdigest()


def resource(path: Path, name: str, needs_execute: bool = False
             ) -> dict[str, str]:
    checked = executable(path, name) if needs_execute else regular(path, name)
    return {"path": str(checked), "sha256": sha256(checked)}


def checked_file(row: Any, name: str, needs_execute: bool = False) -> Path:
    exact_keys(row, {"path", "sha256"}, "configured " + name)
    text = row["path"]
    digest = row["sha256"]
    if (type(text) is not str or not Path(text).is_absolute() or
            type(digest) is not str or
            re.fullmatch(r"[0-9a-f]{64}", digest) is None):
        raise AdapterError("configured resource is invalid: " + name)
    path = executable(Path(text), name) if needs_execute else regular(
        Path(text), name)
    if sha256(path) != digest:
        raise AdapterError("configured resource changed: " + name)
    return path


def wave_facts(path: Path, name: str = "WAVE") -> dict[str, Any]:
    path = regular(path, name)
    try:
        with path.open("rb") as raw:
            header = raw.read(12)
        if (len(header) != 12 or header[:4] != b"RIFF" or
                header[8:12] != b"WAVE"):
            raise AdapterError(name + " must be little-endian RIFF/WAVE")
        with wave.open(str(path), "rb") as stream:
            channels = stream.getnchannels()
            width = stream.getsampwidth()
            rate = stream.getframerate()
            frames = stream.getnframes()
            compression = stream.getcomptype()
    except (OSError, EOFError, wave.Error) as error:
        raise AdapterError("invalid {}: {}".format(name, error)) from error
    if (compression != "NONE" or width != 2 or channels not in (1, 2) or
            not 8000 <= rate <= 768000 or frames < 1):
        raise AdapterError(name + " must be nonempty mono/stereo PCM16 WAVE")
    return {
        "rate_hz": rate, "channels": channels, "sample_width_bytes": width,
        "frames": frames, "sha256": sha256(path),
    }


def write_excerpt(source: Path, output: Path, seconds: float) -> dict[str, Any]:
    facts = wave_facts(source, "body reference")
    if facts["rate_hz"] != 44100:
        raise AdapterError("body references must use 44100 Hz")
    if output.exists() or output.is_symlink():
        raise AdapterError("body excerpt output already exists")
    try:
        with wave.open(str(source), "rb") as input_stream:
            total = input_stream.getnframes()
            count = min(total, round(seconds * facts["rate_hz"]))
            start = (total - count) // 2
            input_stream.setpos(start)
            audio = input_stream.readframes(count)
    except (OSError, EOFError, wave.Error) as error:
        raise AdapterError("cannot read body reference excerpt") from error
    expected = count * facts["channels"] * 2
    if count < 1 or len(audio) != expected:
        raise AdapterError("cannot read body reference excerpt")
    try:
        with output.open("xb") as raw_output:
            os.fchmod(raw_output.fileno(), 0o600)
            with wave.open(raw_output, "wb") as output_stream:
                output_stream.setnchannels(facts["channels"])
                output_stream.setsampwidth(2)
                output_stream.setframerate(44100)
                output_stream.writeframes(audio)
    except (OSError, wave.Error) as error:
        raise AdapterError("cannot write body reference excerpt") from error
    excerpt = wave_facts(output, "body reference excerpt")
    return {
        "source_path": str(source),
        "source_sha256": facts["sha256"],
        "excerpt_path": str(output),
        "excerpt_sha256": excerpt["sha256"],
        "start_frame": start,
        "source_frames": count,
        "excerpt_frames": excerpt["frames"],
        "excerpt_rate_hz": excerpt["rate_hz"],
    }


def expected_fit_manifest(
        body_reference_hashes: dict[str, str] | None = None
        ) -> dict[str, Any]:
    if body_reference_hashes is not None:
        if set(body_reference_hashes) != {
                "reference_body_fit", "reference_body_check"}:
            raise AdapterError("body reference hashes differ from the contract")
        for name, digest in body_reference_hashes.items():
            if (type(digest) is not str or
                    re.fullmatch(r"[0-9a-f]{64}", digest) is None):
                raise AdapterError("invalid body reference hash: " + name)
    parameters = [
        {
            "id": "body_wet_gain", "unit": "ratio", "minimum": 0.1,
            "maximum": 0.55, "baseline": 0.45,
            "profile_paths": [["body", "wet_gain"]],
        },
        {
            "id": "bridge_cutoff_g_hz", "unit": "Hz", "minimum": 4800.0,
            "maximum": 8200.0, "baseline": 5386.995271806526,
            "profile_paths": [["strings", 0, "bridge_cutoff_hz"]],
        },
        {
            "id": "bridge_cutoff_d_hz", "unit": "Hz", "minimum": 2800.0,
            "maximum": 5200.0, "baseline": 4964.244281108786,
            "profile_paths": [["strings", 1, "bridge_cutoff_hz"]],
        },
        {
            "id": "bridge_cutoff_a_hz", "unit": "Hz", "minimum": 4800.0,
            "maximum": 8200.0, "baseline": 7086.471045764144,
            "profile_paths": [["strings", 2, "bridge_cutoff_hz"]],
        },
        {
            "id": "bridge_cutoff_e_hz", "unit": "Hz", "minimum": 4400.0,
            "maximum": 7600.0, "baseline": 6024.580442039031,
            "profile_paths": [["strings", 3, "bridge_cutoff_hz"]],
        },
    ]
    objectives = []
    for split in ("fit", "check"):
        objectives.extend([
            {
                "id": split + "_mid_band", "kind": "experiment-gap",
                "response": "final.band.500-1000", "split": split,
                "weight": 0.4, "scale": 6.0,
            },
            {
                "id": split + "_presence_band", "kind": "experiment-gap",
                "response": "final.band.2-4k", "split": split,
                "weight": 0.8, "scale": 6.0,
            },
            {
                "id": split + "_metal_band", "kind": "experiment-gap",
                "response": "final.band.4-8k", "split": split,
                "weight": 1.0, "scale": 6.0,
            },
            {
                "id": split + "_body_shape", "kind": "body-envelope",
                "case": "body-" + split,
                "reference_binding": "reference_body_" + split,
                "resource_id": "model.final", "split": split,
                "weight": 1.4, "scale": 6.0,
                **({"reference_sha256": body_reference_hashes[
                    "reference_body_" + split]}
                   if body_reference_hashes is not None else {}),
            },
        ])
    return {
        "schema": "hwa-instrument-fit", "schema_version": 1,
        "adapter_id": ADAPTER_ID, "parameters": parameters,
        "objectives": objectives,
        "selection": {
            "check_weight": 1.0, "max_check_loss_increase": 0.08,
            "max_candidate_worst_harm": 4.0,
        },
    }


def validate_fit_manifest(value: dict[str, Any]) -> None:
    if exact_tree(value, expected_fit_manifest()):
        return
    objectives = value.get("objectives") if type(value) is dict else None
    if type(objectives) is list:
        rows = {
            row.get("reference_binding"): row
            for row in objectives
            if type(row) is dict and row.get("kind") == "body-envelope"
        }
        if set(rows) == {"reference_body_fit", "reference_body_check"}:
            hashes = {
                name: row.get("reference_sha256")
                for name, row in rows.items()
            }
            try:
                expected = expected_fit_manifest(hashes)
            except AdapterError:
                expected = None
            if expected is not None and exact_tree(value, expected):
                return
    raise AdapterError("fit manifest differs from the violin v1 contract")


def profile_value(profile: dict[str, Any], path: list[Any]) -> Any:
    value: Any = profile
    for part in path:
        if type(part) is int:
            if type(value) is not list or not 0 <= part < len(value):
                raise AdapterError("profile path is missing")
        elif type(part) is str:
            if type(value) is not dict or part not in value:
                raise AdapterError("profile path is missing")
        else:
            raise AdapterError("profile path is invalid")
        value = value[part]
    return value


def check_profile_baselines(profile: dict[str, Any], manifest: dict[str, Any]
                            ) -> None:
    for row in manifest["parameters"]:
        paths = row["profile_paths"]
        if type(paths) is not list or len(paths) != 1:
            raise AdapterError("each violin parameter needs one profile path")
        value = finite(profile_value(profile, paths[0]), row["id"])
        if value != float(row["baseline"]):
            raise AdapterError("profile baseline differs for " + row["id"])


def import_profile_validator(generator_path: Path):
    try:
        specification = importlib.util.spec_from_file_location(
            "hwa_frozen_violin_profile_validator", generator_path)
        if specification is None or specification.loader is None:
            raise AdapterError("cannot load violin profile validator")
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
    except (OSError, ImportError, AttributeError) as error:
        raise AdapterError("cannot load violin profile validator") from error
    if (not callable(getattr(module, "load_json", None)) or
            not callable(getattr(module, "validate_profile", None))):
        raise AdapterError("violin profile validator has the wrong interface")
    return module


def validate_profile_with(path: Path, generator_path: Path,
                          schema_path: Path) -> None:
    path = regular(path, "profile")
    generator_path = regular(generator_path, "profile generator")
    schema_path = regular(schema_path, "profile schema")
    module = import_profile_validator(generator_path)
    try:
        schema = module.load_json(schema_path)
        profile = module.load_json(path)
        module.validate_profile(profile, schema)
    except BaseException as error:
        if isinstance(error, (KeyboardInterrupt, SystemExit)):
            raise
        raise AdapterError("violin profile validation failed: " + str(error)) from error


def parameter_rows(manifest: dict[str, Any]) -> list[dict[str, Any]]:
    result = []
    for row in manifest["parameters"]:
        result.append({
            "id": row["id"], "unit": row["unit"],
            "minimum": row["minimum"], "maximum": row["maximum"],
            "baseline": row["baseline"], "levels": [],
        })
    return sorted(result, key=lambda row: row["id"])


def stem(resource_id: str, side: str, input_id: str | None,
         output: str | None, channels: int) -> dict[str, Any]:
    return {
        "id": resource_id, "side": side, "role": "final",
        "input_id": input_id, "output": output, "start_sample": 0,
        "gain_db": 0, "rate_hz": 44100, "channels": channels,
    }


def build_experiment(references: dict[str, Path], sample_count: int,
                     manifest: dict[str, Any] | None = None
                     ) -> dict[str, Any]:
    if type(sample_count) is not int or not 1 <= sample_count <= 256:
        raise AdapterError("sample count must be 1..256")
    if set(references) != set(REFERENCE_ARGUMENTS):
        raise AdapterError("reference set differs from the violin contract")
    manifest = load_json(FIT_MANIFEST) if manifest is None else manifest
    validate_fit_manifest(manifest)
    checked = {
        name: regular(Path(path), name) for name, path in references.items()
    }
    facts = {name: wave_facts(path, name) for name, path in checked.items()}
    if len({row["sha256"] for row in facts.values()}) != len(facts):
        raise AdapterError("violin references must have distinct hashes")
    for name in ("reference_g3", "reference_d4", "reference_a4",
                 "reference_e5"):
        if facts[name]["rate_hz"] != 44100:
            raise AdapterError("open-string references must use 44100 Hz")
    for name in ("reference_body_fit", "reference_body_check"):
        if facts[name]["rate_hz"] != 44100:
            raise AdapterError("body excerpts must use 44100 Hz")

    cases = []
    for case_id, spec in sorted(CASE_SPECS.items()):
        binding = spec["reference"]
        cases.append({
            "id": case_id, "split": spec["split"], "weight": 1,
            "stems": [
                stem("model.final", "model", None, "model.wav", 2),
                stem("reference.final", "reference", binding, None,
                     facts[binding]["channels"]),
            ],
            "probes": [], "links": [],
        })
    responses = [
        {"id": "final.rms", "role": "final",
         "feature": "rms_dbfs", "index": 0},
        {"id": "final.crest", "role": "final",
         "feature": "crest_db", "index": 0},
    ]
    for index, name in (
        (4, "500-1000"), (5, "1-2k"), (6, "2-4k"),
        (7, "4-8k"), (8, "8-16k"),
    ):
        responses.append({
            "id": "final.band." + name, "role": "final",
            "feature": "band_level_dbfs", "index": index,
        })
    return {
        "schema": "hwa-experiment", "schema_version": 1,
        "method_version": METHOD_VERSION, "clock_rate_hz": 44100,
        "inputs": [
            {"id": name, "sha256": facts[name]["sha256"]}
            for name in sorted(facts)
        ],
        "parameters": parameter_rows(manifest),
        "plan": {
            "kind": "random", "seed": 17012010,
            "sample_count": sample_count, "replicates": 1,
        },
        "cases": cases,
        "responses": sorted(responses, key=lambda row: row["id"]),
    }


def clean_environment(scratch: Path | None = None) -> dict[str, str]:
    environment = {
        "LC_ALL": "C", "LANG": "C", "TZ": "UTC", "PATH": CLEAN_PATH,
    }
    if scratch is not None:
        environment.update({
            "TMPDIR": str(scratch), "TMP": str(scratch), "TEMP": str(scratch),
        })
    return environment


def run_tool(arguments: Sequence[Path | str], name: str, cwd: Path,
             environment: dict[str, str] | None = None,
             timeout: int = 120) -> subprocess.CompletedProcess[str]:
    try:
        completed = subprocess.run(
            [str(value) for value in arguments], check=False, cwd=str(cwd),
            env=clean_environment(cwd) if environment is None else environment,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        raise AdapterError(name + " timed out") from error
    except OSError as error:
        raise AdapterError("cannot run {}: {}".format(name, error)) from error
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        if len(detail) > 2000:
            detail = detail[-2000:]
        suffix = ": " + detail if detail else ""
        raise AdapterError("{} failed with status {}{}".format(
            name, completed.returncode, suffix))
    return completed


def verify_loaded_library(csound: Path, library: Path, cwd: Path) -> None:
    csound = executable(csound, "Csound")
    expected = regular(library, "Csound library").resolve()
    environment = clean_environment(cwd)
    if sys.platform == "darwin":
        environment["DYLD_PRINT_LIBRARIES"] = "1"
        completed = run_tool(
            [csound, "--version"], "Csound core library check", cwd,
            environment,
        )
        lines = (completed.stdout + "\n" + completed.stderr).splitlines()
        if not any(line.strip().endswith(str(expected)) for line in lines):
            raise AdapterError("Csound did not load the named core library")
        return
    if sys.platform.startswith("linux"):
        ldd = shutil.which("ldd", path=CLEAN_PATH)
        if ldd is None:
            raise AdapterError("cannot find ldd for the Csound library check")
        completed = run_tool(
            [ldd, csound], "Csound core library check", cwd, environment,
        )
        loaded = set()
        for line in (completed.stdout + "\n" + completed.stderr).splitlines():
            item = line.strip()
            if "=>" in item:
                item = item.split("=>", 1)[1].strip()
            candidate = item.split(" ", 1)[0]
            if candidate.startswith("/"):
                loaded.add(Path(candidate).resolve())
        if expected not in loaded:
            raise AdapterError("Csound did not load the named core library")
        return
    raise AdapterError("cannot check the Csound core library on this platform")


def contains_module_marker(path: Path) -> bool:
    source = read_bounded(path, MAX_MODULE_BYTES, "violin test module")
    return MODULE_MARKER in source


def checked_python_shebang(path: Path) -> str:
    path = executable(path, "Python")
    text = str(path)
    if (any(character.isspace() for character in text) or
            len(text.encode("utf-8")) > 240):
        raise AdapterError("Python path cannot form a renderer shebang")
    return "#!" + text + "\n"


def make_renderer_config(
        resources: dict[str, Path], cases: dict[str, dict[str, Any]],
        parameters: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "schema": RENDERER_SCHEMA + "-config", "schema_version": 1,
        "adapter_id": ADAPTER_ID, "method_version": METHOD_VERSION,
        "platform": sys.platform,
        "permissions": {
            "render": True, "validate_profile": True,
            "write_profile": False,
        },
        "files": {
            name: resource(
                path, name, name in ("analyzer", "csound", "python"))
            for name, path in sorted(resources.items())
        },
        "parameters": parameters, "cases": cases,
        "model_frames": MODEL_FRAMES,
    }


def write_renderer(path: Path, config: dict[str, Any]) -> None:
    path = path.absolute()
    if path.exists() or path.is_symlink() or not path.parent.is_dir():
        raise AdapterError("renderer output must be new")
    try:
        source = Path(__file__).read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise AdapterError("cannot read renderer source") from error
    if source.count(CONFIG_MARKER) != 1:
        raise AdapterError("renderer source has an invalid embed marker")
    first_end = source.find("\n")
    if first_end < 0 or not source.startswith("#!"):
        raise AdapterError("renderer source has no shebang")
    python = Path(config["files"]["python"]["path"])
    source = checked_python_shebang(python) + source[first_end + 1:]
    replacement = (
        "EMBEDDED_CONFIG_JSON = " + repr(canonical_json(config)) +
        "  # HWA_EMBED_CONFIG"
    )
    rendered = source.replace(CONFIG_MARKER, replacement).encode("utf-8")
    try:
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o700)
        with os.fdopen(descriptor, "wb") as stream:
            os.fchmod(stream.fileno(), 0o700)
            stream.write(rendered)
            stream.flush()
            os.fsync(stream.fileno())
    except OSError as error:
        raise AdapterError("cannot write frozen renderer") from error


def embedded_config() -> dict[str, Any]:
    if type(EMBEDDED_CONFIG_JSON) is not str:
        raise AdapterError("use a generated violin renderer")
    config = parse_json(EMBEDDED_CONFIG_JSON.encode("utf-8"),
                        "embedded renderer config")
    exact_keys(config, {
        "schema", "schema_version", "adapter_id", "method_version",
        "platform", "permissions", "files", "parameters", "cases",
        "model_frames",
    }, "embedded renderer config")
    if (not exact_value(config["schema"], RENDERER_SCHEMA + "-config") or
            not exact_value(config["schema_version"], 1) or
            not exact_value(config["adapter_id"], ADAPTER_ID) or
            not exact_value(config["method_version"], METHOD_VERSION) or
            not exact_value(config["platform"], sys.platform) or
            not exact_value(config["model_frames"], MODEL_FRAMES)):
        raise AdapterError("embedded renderer config has invalid values")
    if not exact_tree(config["permissions"], {
            "render": True, "validate_profile": True,
            "write_profile": False}):
        raise AdapterError("embedded renderer permissions changed")
    files = exact_keys(config["files"], {
        "analyzer", "csound", "csound_library", "csd", "fit_manifest",
        "generator", "module", "profile", "python", "schema",
    }, "embedded renderer files")
    for name, row in files.items():
        exact_keys(row, {"path", "sha256"}, "configured " + name)
    manifest = expected_fit_manifest()
    if not exact_tree(config["parameters"], parameter_rows(manifest)):
        raise AdapterError("embedded renderer parameters changed")
    cases = config["cases"]
    if type(cases) is not dict or set(cases) != set(CASE_SPECS):
        raise AdapterError("embedded renderer cases changed")
    for case_id, expected in CASE_SPECS.items():
        case = exact_keys(cases[case_id], {
            "split", "mode", "string", "frequency_hz", "reference",
            "reference_path", "reference_sha256", "reference_channels",
            "reference_frames",
        }, "configured case " + case_id)
        for name in ("split", "mode", "string", "frequency_hz", "reference"):
            if not exact_value(case[name], expected[name]):
                raise AdapterError("configured case changed: " + case_id)
        if (type(case["reference_path"]) is not str or
                not Path(case["reference_path"]).is_absolute() or
                type(case["reference_sha256"]) is not str or
                re.fullmatch(r"[0-9a-f]{64}",
                             case["reference_sha256"]) is None or
                type(case["reference_channels"]) is not int or
                case["reference_channels"] not in (1, 2) or
                type(case["reference_frames"]) is not int or
                case["reference_frames"] < 1):
            raise AdapterError("configured case reference is invalid")
    return config


def check_resources(config: dict[str, Any]) -> dict[str, Path]:
    files = config["files"]
    result = {
        name: checked_file(
            row, name, name in ("analyzer", "csound", "python"))
        for name, row in sorted(files.items())
    }
    if not contains_module_marker(result["module"]):
        raise AdapterError("violin renderer needs the diagnostic test module")
    manifest = load_json(result["fit_manifest"])
    validate_fit_manifest(manifest)
    return result


def verify_frozen_config() -> tuple[dict[str, Any], dict[str, Path]]:
    config = embedded_config()
    files = check_resources(config)
    return config, files


def describe() -> dict[str, Any]:
    config, _ = verify_frozen_config()
    return {
        "schema": RENDERER_SCHEMA, "schema_version": 1,
        "adapter_id": ADAPTER_ID, "method_version": METHOD_VERSION,
        "permissions": config["permissions"],
        "cases": [
            {
                "id": case_id, "split": row["split"],
                "string": row["string"],
                "frequency_hz": row["frequency_hz"],
            }
            for case_id, row in sorted(config["cases"].items())
        ],
        "parameters": [
            {
                "id": row["id"], "unit": row["unit"],
                "minimum": row["minimum"], "maximum": row["maximum"],
                "baseline": row["baseline"],
            }
            for row in config["parameters"]
        ],
        "resources": [
            {"id": name, "sha256": row["sha256"]}
            for name, row in sorted(config["files"].items())
        ],
    }


def validate_profile(path: Path) -> None:
    config, files = verify_frozen_config()
    candidate_hash = sha256(regular(path, "profile"))
    validate_profile_with(path, files["generator"], files["schema"])
    if sha256(path) != candidate_hash:
        raise AdapterError("profile changed during validation")
    check_resources(config)


def request_values(request: dict[str, Any],
                   parameters: list[dict[str, Any]]) -> dict[str, float]:
    rows = request["parameters"]
    if type(rows) is not list or len(rows) != len(parameters):
        raise AdapterError("render request has the wrong parameter set")
    expected = {row["id"]: row for row in parameters}
    result: dict[str, float] = {}
    for index, source in enumerate(rows):
        row = exact_keys(source, {"id", "unit", "value"},
                         "render parameter {}".format(index))
        name = row["id"]
        specification = expected.get(name) if type(name) is str else None
        if (specification is None or name in result or
                not exact_value(row["unit"], specification["unit"])):
            raise AdapterError("render request has the wrong parameter set")
        value = finite(row["value"], "render parameter " + name)
        if value < specification["minimum"] or value > specification["maximum"]:
            raise AdapterError("render parameter is out of range: " + name)
        result[name] = value
    if set(result) != set(expected):
        raise AdapterError("render request has the wrong parameter set")
    return result


def validate_render_request(
        request_path: Path, output_dir: Path, request: dict[str, Any],
        config: dict[str, Any]) -> tuple[dict[str, Any], Path, Path,
                                        dict[str, float]]:
    request_path = regular(request_path, "render request")
    output_dir = real_directory(output_dir, "render output directory")
    if request_path.parent.resolve() != output_dir.resolve():
        raise AdapterError("render request must be inside the output directory")
    exact_keys(request, {
        "schema", "schema_version", "method_version", "case_id", "job_id",
        "job_key", "inputs", "outputs", "parameters", "replicate", "seed",
        "split",
    }, "render request")
    if (not exact_value(request["schema"], "hwa-render-job") or
            not exact_value(request["schema_version"], 1) or
            not exact_value(request["method_version"], METHOD_VERSION)):
        raise AdapterError("unsupported render request")
    case_id = request["case_id"]
    case = config["cases"].get(case_id) if type(case_id) is str else None
    if case is None or not exact_value(request["split"], case["split"]):
        raise AdapterError("unknown violin fit case or wrong split")
    unsigned(request["job_id"], "job_id", 1)
    unsigned(request["replicate"], "replicate")
    unsigned(request["seed"], "seed")
    if request["replicate"] != 0:
        raise AdapterError("violin fit uses one replicate")
    if (type(request["job_key"]) is not str or
            re.fullmatch(r"[0-9a-f]{64}", request["job_key"]) is None):
        raise AdapterError("job_key is invalid")

    inputs = request["inputs"]
    if type(inputs) is not list or len(inputs) != 1:
        raise AdapterError("violin renderer needs one reference input")
    input_row = exact_keys(inputs[0], {
        "binding_id", "channels", "gain_db", "kind", "path",
        "probe_format", "probe_name", "rate_denominator", "rate_hz",
        "rate_numerator", "resource_id", "role", "sha256", "side",
        "start_sample", "unit", "value_count",
    }, "reference input")
    expected_input = {
        "binding_id": case["reference"],
        "channels": case["reference_channels"], "gain_db": 0,
        "kind": "stem", "probe_format": None, "probe_name": None,
        "rate_denominator": 0, "rate_hz": 44100, "rate_numerator": 0,
        "resource_id": "reference.final", "role": "final",
        "sha256": case["reference_sha256"], "side": "reference",
        "start_sample": 0, "unit": None, "value_count": 0,
    }
    for name, wanted in expected_input.items():
        if not exact_value(input_row[name], wanted):
            raise AdapterError("reference input has wrong " + name)
    input_text = input_row["path"]
    if type(input_text) is not str or not Path(input_text).is_absolute():
        raise AdapterError("reference input path must be absolute")
    reference = regular(Path(input_text), "reference input")
    if reference.resolve() != Path(case["reference_path"]).resolve():
        raise AdapterError("reference input has the wrong path")
    reference_facts = wave_facts(reference, "reference input")
    if (reference_facts["sha256"] != case["reference_sha256"] or
            reference_facts["rate_hz"] != 44100 or
            reference_facts["channels"] != case["reference_channels"] or
            reference_facts["frames"] != case["reference_frames"]):
        raise AdapterError("reference input facts changed")

    outputs = request["outputs"]
    if type(outputs) is not list or len(outputs) != 1:
        raise AdapterError("violin renderer needs one model output")
    output_row = exact_keys(outputs[0], {
        "id", "kind", "path", "side", "role", "probe_format",
        "probe_name", "unit", "start_sample", "gain_db", "rate_hz",
        "channels", "rate_numerator", "rate_denominator", "value_count",
    }, "model output")
    expected_output = {
        "id": "model.final", "kind": "stem", "side": "model",
        "role": "final", "probe_format": None, "probe_name": None,
        "unit": None, "start_sample": 0, "gain_db": 0, "rate_hz": 44100,
        "channels": 2, "rate_numerator": 0, "rate_denominator": 0,
        "value_count": 0,
    }
    for name, wanted in expected_output.items():
        if not exact_value(output_row[name], wanted):
            raise AdapterError("model output has wrong " + name)
    output_text = output_row["path"]
    if type(output_text) is not str or not Path(output_text).is_absolute():
        raise AdapterError("renderer output path must be absolute")
    output = Path(output_text)
    if (output.parent.resolve() != output_dir.resolve() or
            output.name != "model.wav" or output.exists() or
            output.is_symlink()):
        raise AdapterError("output path must be output-directory/model.wav")
    values = request_values(request, config["parameters"])
    return case, reference, output, values


def output_wave(path: Path, expected_frames: int) -> dict[str, Any]:
    facts = wave_facts(path, "model output")
    if (facts["rate_hz"] != 44100 or facts["channels"] != 2 or
            facts["frames"] != expected_frames):
        raise AdapterError("model output has the wrong WAVE facts")
    nonzero = 0
    clipped = 0
    try:
        with wave.open(str(path), "rb") as stream:
            remaining = facts["frames"]
            while remaining:
                count = min(remaining, 65536)
                audio = stream.readframes(count)
                if len(audio) != count * 4:
                    raise AdapterError("model output is truncated")
                samples = array("h")
                samples.frombytes(audio)
                if sys.byteorder != "little":
                    samples.byteswap()
                for value in samples:
                    if value != 0:
                        nonzero += 1
                    if value in (-32768, 32767):
                        clipped += 1
                remaining -= count
    except (OSError, EOFError, wave.Error) as error:
        raise AdapterError("cannot inspect model output") from error
    if nonzero == 0:
        raise AdapterError("model output is silent")
    if clipped != 0:
        raise AdapterError("model output clips")
    return {
        "sha256": facts["sha256"], "rate_hz": facts["rate_hz"],
        "channels": facts["channels"], "frames": facts["frames"],
        "nonzero_samples": nonzero, "clipped_samples": clipped,
    }


def unlink_if_identity(path: Path, identity: tuple[int, int] | None) -> None:
    if identity is None:
        return
    try:
        status = os.lstat(path)
        if (status.st_dev, status.st_ino) == identity:
            path.unlink()
    except (FileNotFoundError, OSError):
        pass


def render_job(request_path: Path, output_dir: Path) -> None:
    config, files = verify_frozen_config()
    request_path = regular(request_path, "render request")
    request, request_hash = load_json_evidence(request_path, "render request")
    case, reference, output, values = validate_render_request(
        request_path, output_dir, request, config)
    reference_hash = case["reference_sha256"]
    if sha256(request_path) != request_hash or sha256(reference) != reference_hash:
        raise AdapterError("render inputs changed during validation")
    published_identity = None
    try:
        with tempfile.TemporaryDirectory(
                prefix=".hwa-violin-render-",
                dir=str(real_directory(output_dir, "render output").parent)) as text:
            scratch = Path(text)
            staged = scratch / "model.wav"
            environment = clean_environment(scratch)
            library = files["csound_library"]
            if sys.platform == "darwin":
                environment["DYLD_LIBRARY_PATH"] = str(library.parent)
            elif sys.platform.startswith("linux"):
                environment["LD_LIBRARY_PATH"] = str(library.parent)
            string = int(case["string"])
            cutoff_names = (
                "bridge_cutoff_g_hz", "bridge_cutoff_d_hz",
                "bridge_cutoff_a_hz", "bridge_cutoff_e_hz",
            )
            baselines = {
                row["id"]: float(row["baseline"])
                for row in config["parameters"]
            }
            macros = {
                "DIAG_MODE": case["mode"],
                "DIAG_STRING": string,
                "DIAG_FREQUENCY": case["frequency_hz"],
                "DIAG_BODY_GAIN": (
                    values["body_wet_gain"] / baselines["body_wet_gain"]),
                "DIAG_BRIDGE_CUTOFF_SCALE": (
                    values[cutoff_names[string - 1]] /
                    baselines[cutoff_names[string - 1]]),
            }
            command = [
                str(files["csound"]),
                "--opcode-lib=" + str(files["module"]),
                "--sample-accurate", "--num-threads=1", "-d", "-m0",
                "-W", "-s", "--nopeaks", "-o", str(staged),
            ]
            for name, value in macros.items():
                rendered = value if type(value) is str else format(value, ".17g")
                command.append("--omacro:{}={}".format(name, rendered))
            command.append(str(files["csd"]))
            run_tool(command, "Csound violin fit render", scratch,
                     environment, timeout=120)
            facts = output_wave(staged, config["model_frames"])
            check_resources(config)
            if sha256(request_path) != request_hash:
                raise AdapterError("render request changed during the job")
            if sha256(reference) != reference_hash:
                raise AdapterError("reference input changed during the job")
            try:
                os.link(staged, output, follow_symlinks=False)
            except FileExistsError as error:
                raise AdapterError("model output already exists") from error
            except OSError as error:
                raise AdapterError("cannot publish model output") from error
            status = os.lstat(output)
            published_identity = (status.st_dev, status.st_ino)
            if output_wave(output, config["model_frames"]) != facts:
                raise AdapterError("published model output changed")
    except BaseException:
        unlink_if_identity(output, published_identity)
        raise
    print(canonical_json({
        "schema": RENDER_RESULT_SCHEMA, "schema_version": 1,
        "adapter_id": ADAPTER_ID, "case_id": request["case_id"],
        "job_key": request["job_key"], "model_sha256": facts["sha256"],
        "rate_hz": facts["rate_hz"], "channels": facts["channels"],
        "frames": facts["frames"],
        "nonzero_samples": facts["nonzero_samples"],
        "clipped_samples": facts["clipped_samples"],
    }))


def publish_new_directory(staged: Path, output: Path,
                          expected_names: set[str]) -> None:
    staged = real_directory(staged, "staged bundle")
    output = output.absolute()
    if (not expected_names or
            any(type(name) is not str or Path(name).name != name
                for name in expected_names)):
        raise AdapterError("bundle file names are invalid")
    children = {path.name: path for path in staged.iterdir()}
    if set(children) != expected_names:
        raise AdapterError(
            "staged bundle has unexpected files: " +
            ", ".join(sorted(set(children) ^ expected_names)))
    sources = {
        name: regular(children[name], "staged " + name)
        for name in expected_names
    }
    for name, source in sources.items():
        wanted_mode = 0o700 if name == "renderer" else 0o600
        if stat.S_IMODE(os.lstat(source).st_mode) != wanted_mode:
            raise AdapterError("staged bundle file has wrong mode: " + name)
    try:
        output.mkdir(mode=0o700)
        output.chmod(0o700)
    except FileExistsError as error:
        raise AdapterError("output directory already exists") from error
    except OSError as error:
        raise AdapterError("cannot create output directory") from error
    status = os.lstat(output)
    directory_identity = (status.st_dev, status.st_ino)
    published: dict[str, tuple[int, int]] = {}
    try:
        for name in sorted(expected_names):
            source_status = os.lstat(sources[name])
            identity = (source_status.st_dev, source_status.st_ino)
            destination = output / name
            try:
                os.link(sources[name], destination, follow_symlinks=False)
            except FileExistsError as error:
                raise AdapterError("bundle output already exists: " + name) from error
            except OSError as error:
                raise AdapterError("cannot publish bundle output: " + name) from error
            published[name] = identity
            destination_status = os.lstat(destination)
            if (destination_status.st_dev, destination_status.st_ino) != identity:
                raise AdapterError("published bundle file changed: " + name)
        current = os.lstat(output)
        if ((current.st_dev, current.st_ino) != directory_identity or
                stat.S_IMODE(current.st_mode) != 0o700 or
                {path.name for path in output.iterdir()} != expected_names):
            raise AdapterError("published bundle directory changed")
        descriptor = os.open(output, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
    except BaseException:
        for name, identity in published.items():
            unlink_if_identity(output / name, identity)
        try:
            current = os.lstat(output)
            if (current.st_dev, current.st_ino) == directory_identity:
                output.rmdir()
        except (FileNotFoundError, OSError):
            pass
        raise


def required_violin_files(violin_root: Path) -> dict[str, Path]:
    paths = {
        "profile": violin_root / "profiles" / "generic_violin.json",
        "schema": (violin_root / "profiles" / "schema" /
                   "violin-profile-v1.schema.json"),
        "generator": violin_root / "tools" / "generate_profiles.py",
        "csd": violin_root / "examples" / "realism_diagnostic.csd",
    }
    return {
        name: regular(path, "violin " + name)
        for name, path in sorted(paths.items())
    }


def reference_arguments(arguments: argparse.Namespace) -> dict[str, Path]:
    result = {}
    for binding, attribute in REFERENCE_ARGUMENTS.items():
        value = getattr(arguments, attribute)
        path = Path(value)
        if not path.is_absolute():
            raise AdapterError(binding + " path must be absolute")
        result[binding] = regular(path, binding)
    facts = {name: wave_facts(path, name) for name, path in result.items()}
    if len({row["sha256"] for row in facts.values()}) != len(facts):
        raise AdapterError("all six source recordings must have distinct hashes")
    for name in ("reference_g3", "reference_d4", "reference_a4",
                 "reference_e5"):
        if facts[name]["rate_hz"] != 44100:
            raise AdapterError("open-string references must use 44100 Hz")
    return result


def verify_pitch_preflight_inputs(
        analyzer: Path, analyzer_hash: str, references: dict[str, Path],
        reference_hashes: dict[str, str]) -> None:
    if sha256(analyzer) != analyzer_hash:
        raise AdapterError("analyzer changed during open-string preflight")
    if set(references) != set(reference_hashes):
        raise AdapterError("open-string preflight hash set changed")
    for binding, digest in reference_hashes.items():
        if sha256(references[binding]) != digest:
            raise AdapterError(
                binding + " changed during open-string preflight")


def preflight_open_string_references(
        analyzer: Path, references: dict[str, Path],
        reference_hashes: dict[str, str], cwd: Path
        ) -> tuple[str, list[dict[str, Any]]]:
    analyzer = executable(analyzer, "analyzer")
    if set(references) != set(REFERENCE_ARGUMENTS):
        raise AdapterError("reference set differs from the violin contract")
    if set(reference_hashes) != set(references):
        raise AdapterError("reference hashes differ from the violin contract")
    analyzer_hash = sha256(analyzer)
    verify_pitch_preflight_inputs(
        analyzer, analyzer_hash, references, reference_hashes)
    rows = []
    for case_id in OPEN_STRING_CASES:
        specification = CASE_SPECS[case_id]
        binding = specification["reference"]
        reference = references[binding]
        expected_hz = float(specification["frequency_hz"])
        verify_pitch_preflight_inputs(
            analyzer, analyzer_hash, {binding: reference},
            {binding: reference_hashes[binding]})
        completed = run_tool([
            analyzer, "--json", "isolated-note", reference,
            "--expected-hz", format(expected_hz, ".17g"),
            "--metrics", "pitch",
        ], case_id + " pitch preflight", cwd)
        source = completed.stdout.encode("utf-8")
        if len(source) > MAX_JSON_BYTES:
            raise AdapterError(case_id + " pitch report exceeds its byte limit")
        report = parse_json(source, case_id + " pitch report")
        pitch = report.get("pitch")
        if (not exact_value(report.get("schema"), "hwa-isolated-note") or
                not exact_value(report.get("schema_version"), 1) or
                not exact_value(report.get("command"), "isolated-note") or
                not exact_value(report.get("method"), "isolated-note-1") or
                not exact_value(report.get("path"), str(reference)) or
                finite(report.get("expected_hz"),
                       case_id + " reported expected_hz") != expected_hz or
                not exact_value(report.get("requested_mask"), 1) or
                not exact_tree(report.get("requested_metrics"), ["pitch"]) or
                type(pitch) is not dict or
                type(pitch.get("valid")) is not bool):
            raise AdapterError(
                case_id + " pitch preflight returned an unknown contract")
        if (pitch["valid"] is not True or
                not exact_value(report.get("valid_mask"), 1) or
                not exact_tree(report.get("valid_metrics"), ["pitch"])):
            raise AdapterError(case_id + " pitch is not valid")
        rows.append({
            "case_id": case_id, "reference_binding": binding,
            "expected_hz": expected_hz,
            "measured_hz": finite(pitch.get("hz"), case_id + " pitch hz"),
            "cents": finite(pitch.get("cents"), case_id + " pitch cents"),
            "confidence": finite(
                pitch.get("confidence"), case_id + " pitch confidence"),
            "coverage": finite(
                pitch.get("coverage"), case_id + " pitch coverage"),
        })
        verify_pitch_preflight_inputs(
            analyzer, analyzer_hash, {binding: reference},
            {binding: reference_hashes[binding]})
    verify_pitch_preflight_inputs(
        analyzer, analyzer_hash, references, reference_hashes)
    return analyzer_hash, rows


def case_config(references: dict[str, Path],
                published_paths: dict[str, Path]) -> dict[str, dict[str, Any]]:
    facts = {name: wave_facts(path, name) for name, path in references.items()}
    result = {}
    for case_id, specification in sorted(CASE_SPECS.items()):
        binding = specification["reference"]
        row = dict(specification)
        row.update({
            "reference_path": str(published_paths[binding]),
            "reference_sha256": facts[binding]["sha256"],
            "reference_channels": facts[binding]["channels"],
            "reference_frames": facts[binding]["frames"],
        })
        result[case_id] = row
    return result


def baseline_render_request(config: dict[str, Any], output: Path
                            ) -> dict[str, Any]:
    case_id = "open-g3"
    case = config["cases"][case_id]
    return {
        "schema": "hwa-render-job", "schema_version": 1,
        "method_version": METHOD_VERSION, "case_id": case_id,
        "job_id": 1, "job_key": "0" * 64,
        "inputs": [{
            "binding_id": case["reference"],
            "channels": case["reference_channels"], "gain_db": 0,
            "kind": "stem", "path": case["reference_path"],
            "probe_format": None, "probe_name": None,
            "rate_denominator": 0, "rate_hz": 44100,
            "rate_numerator": 0, "resource_id": "reference.final",
            "role": "final", "sha256": case["reference_sha256"],
            "side": "reference", "start_sample": 0, "unit": None,
            "value_count": 0,
        }],
        "outputs": [{
            "id": "model.final", "kind": "stem", "path": str(output),
            "side": "model", "role": "final", "probe_format": None,
            "probe_name": None, "unit": None, "start_sample": 0,
            "gain_db": 0, "rate_hz": 44100, "channels": 2,
            "rate_numerator": 0, "rate_denominator": 0,
            "value_count": 0,
        }],
        "parameters": [{
            "id": row["id"], "unit": row["unit"],
            "value": row["baseline"],
        } for row in config["parameters"]],
        "replicate": 0, "seed": 17012010, "split": case["split"],
    }


def probe_renderer(renderer: Path, profile: Path,
                   config: dict[str, Any], parent: Path) -> None:
    with tempfile.TemporaryDirectory(
            prefix=".hwa-violin-renderer-probe-",
            dir=str(parent)) as probe_text:
        probe = Path(probe_text)
        described = run_tool(
            [renderer, "--describe"], "frozen renderer description", probe)
        descriptor = parse_json(
            described.stdout.encode("utf-8"), "renderer description")
        if (descriptor.get("schema") != RENDERER_SCHEMA or
                descriptor.get("adapter_id") != ADAPTER_ID):
            raise AdapterError("frozen renderer returned a wrong description")
        run_tool(
            [renderer, "--validate-profile", profile],
            "frozen renderer profile validation", probe)
        request_path = probe / "request.json"
        output = probe / "model.wav"
        write_json(request_path, baseline_render_request(config, output))
        rendered = run_tool(
            [renderer, "--hwa-experiment-job", request_path,
             "--output-dir", probe],
            "frozen renderer baseline render", probe)
        result = parse_json(
            rendered.stdout.encode("utf-8"), "renderer result")
        if (result.get("schema") != RENDER_RESULT_SCHEMA or
                result.get("adapter_id") != ADAPTER_ID or
                result.get("case_id") != "open-g3" or
                result.get("model_sha256") != sha256(output)):
            raise AdapterError("frozen renderer returned a wrong render result")
        if {path.name for path in probe.iterdir()} != {
                "request.json", "model.wav"}:
            raise AdapterError("frozen renderer probe left unexpected files")


def build_bundle(arguments: argparse.Namespace) -> None:
    output = Path(arguments.output_dir)
    if not output.is_absolute():
        raise AdapterError("output directory path must be absolute")
    output = output.absolute()
    parent = real_directory(output.parent, "output parent")
    if output.exists() or output.is_symlink():
        raise AdapterError("output directory must be new")
    violin_root = real_directory(Path(arguments.violin_root), "violin root")
    if under(output, ROOT) or under(output, violin_root):
        raise AdapterError("output directory must stay outside source repositories")
    sample_count = unsigned(arguments.sample_count, "sample count", 1)
    if sample_count > 256:
        raise AdapterError("sample count must be 1..256")
    excerpt_seconds = finite(
        arguments.body_reference_seconds, "body reference seconds")
    if not 5.0 <= excerpt_seconds <= 120.0:
        raise AdapterError("body reference seconds must be 5..120")

    manifest = load_json(FIT_MANIFEST)
    validate_fit_manifest(manifest)
    violin_files = required_violin_files(violin_root)
    profile = load_json(violin_files["profile"])
    check_profile_baselines(profile, manifest)
    validate_profile_with(
        violin_files["profile"], violin_files["generator"],
        violin_files["schema"])

    analyzer = executable(Path(arguments.analyzer), "analyzer")
    csound = executable(Path(arguments.csound), "Csound")
    python = executable(Path(arguments.python), "Python")
    module = regular(Path(arguments.module), "violin test module")
    if not contains_module_marker(module):
        raise AdapterError("violin fit needs the diagnostic test module")
    csound_library = regular(
        Path(arguments.csound_library), "Csound library")
    verify_loaded_library(csound, csound_library, parent)
    source_references = reference_arguments(arguments)
    source_hashes = {
        name: sha256(path) for name, path in source_references.items()
    }
    analyzer_hash, pitch_check_rows = preflight_open_string_references(
        analyzer, source_references, source_hashes, parent)

    resources = {
        **violin_files,
        "analyzer": analyzer,
        "fit_manifest": FIT_MANIFEST,
        "csound": csound,
        "csound_library": csound_library,
        "module": module,
        "python": python,
    }
    temporary = Path(tempfile.mkdtemp(
        prefix="." + output.name + "-", dir=str(parent)))
    expected_names = {
        "renderer", "experiment.json", "fit.json", "bindings.json",
        "receipt.json", "reference_body_fit.wav",
        "reference_body_check.wav",
    }
    try:
        bound_references = dict(source_references)
        published_paths = dict(source_references)
        excerpt_rows = []
        for binding in ("reference_body_fit", "reference_body_check"):
            staged_path = temporary / (binding + ".wav")
            row = write_excerpt(
                source_references[binding], staged_path, excerpt_seconds)
            final_path = output / staged_path.name
            row["excerpt_path"] = str(final_path)
            excerpt_rows.append(row)
            bound_references[binding] = staged_path
            published_paths[binding] = final_path
        bound_facts = {
            name: wave_facts(path, name)
            for name, path in bound_references.items()
        }
        if len({row["sha256"] for row in bound_facts.values()}) != 6:
            raise AdapterError("bound violin references must have distinct hashes")

        bundle_manifest = expected_fit_manifest({
            name: bound_facts[name]["sha256"]
            for name in ("reference_body_fit", "reference_body_check")
        })
        validate_fit_manifest(bundle_manifest)
        experiment = build_experiment(
            bound_references, sample_count, bundle_manifest)
        parameters = experiment["parameters"]
        cases = case_config(bound_references, published_paths)
        config = make_renderer_config(resources, cases, parameters)
        if config["files"]["analyzer"]["sha256"] != analyzer_hash:
            raise AdapterError("analyzer changed after open-string preflight")
        renderer = temporary / "renderer"
        write_renderer(renderer, config)
        experiment_path = temporary / "experiment.json"
        fit_path = temporary / "fit.json"
        bindings_path = temporary / "bindings.json"
        receipt_path = temporary / "receipt.json"
        write_json(experiment_path, experiment)
        write_json(fit_path, bundle_manifest)
        binding_rows = [
            {
                "id": name, "path": str(published_paths[name]),
                "sha256": bound_facts[name]["sha256"],
            }
            for name in sorted(bound_references)
        ]
        write_json(bindings_path, {
            "schema": "hwa-fit-bindings", "schema_version": 1,
            "bindings": binding_rows,
            "body_reference_excerpts": excerpt_rows,
        })
        reference_rows = []
        for name in sorted(bound_references):
            source = wave_facts(source_references[name], name + " source")
            bound = bound_facts[name]
            reference_rows.append({
                "id": name, "path": str(published_paths[name]),
                "sha256": bound["sha256"], "rate_hz": bound["rate_hz"],
                "channels": bound["channels"], "frames": bound["frames"],
                "source_path": str(source_references[name]),
                "source_sha256": source["sha256"],
            })
        receipt = {
            "schema": BUNDLE_SCHEMA, "schema_version": 1,
            "adapter_id": ADAPTER_ID, "method_version": METHOD_VERSION,
            "case_count": len(cases), "point_count": sample_count + 1,
            "renderer_sha256": sha256(renderer),
            "experiment_sha256": sha256(experiment_path),
            "fit_manifest_sha256": sha256(fit_path),
            "bindings_sha256": sha256(bindings_path),
            "resources": [
                {"id": name, **row}
                for name, row in sorted(config["files"].items())
            ],
            "references": reference_rows,
            "open_string_pitch_checks": pitch_check_rows,
        }
        write_json(receipt_path, receipt)

        probe_renderer(
            renderer, violin_files["profile"], config, parent)
        check_resources(config)
        for name, path in source_references.items():
            if sha256(path) != source_hashes[name]:
                raise AdapterError("source recording changed during bundle build")
        for name, path in bound_references.items():
            if sha256(path) != bound_facts[name]["sha256"]:
                raise AdapterError("bound recording changed during bundle build")
        publish_new_directory(temporary, output, expected_names)
    finally:
        shutil.rmtree(temporary, ignore_errors=True)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build")
    build.add_argument("--violin-root", required=True, type=Path)
    build.add_argument("--analyzer", required=True, type=Path)
    build.add_argument("--csound", required=True, type=Path)
    build.add_argument("--module", required=True, type=Path)
    build.add_argument("--python", required=True, type=Path)
    build.add_argument("--csound-library", required=True, type=Path)
    build.add_argument("--reference-g3", required=True, type=Path)
    build.add_argument("--reference-d4", required=True, type=Path)
    build.add_argument("--reference-a4", required=True, type=Path)
    build.add_argument("--reference-e5", required=True, type=Path)
    build.add_argument("--reference-body-fit", required=True, type=Path)
    build.add_argument("--reference-body-check", required=True, type=Path)
    build.add_argument("--sample-count", type=int, default=24)
    build.add_argument("--body-reference-seconds", type=float, default=30.0)
    build.add_argument("--output-dir", required=True, type=Path)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        arguments = list(sys.argv[1:] if argv is None else argv)
        if arguments == ["--describe"]:
            print(canonical_json(describe()))
            return 0
        if len(arguments) == 2 and arguments[0] == "--validate-profile":
            validate_profile(Path(arguments[1]))
            return 0
        if (len(arguments) == 4 and
                arguments[0] == "--hwa-experiment-job" and
                arguments[2] == "--output-dir"):
            render_job(Path(arguments[1]), Path(arguments[3]))
            return 0
        if EMBEDDED_CONFIG_JSON is not None:
            raise AdapterError("frozen renderer accepts one declared action")
        options = build_parser().parse_args(arguments)
        if options.command != "build":
            raise AdapterError("choose the build command")
        build_bundle(options)
        return 0
    except (AdapterError, OSError, UnicodeError, ValueError,
            json.JSONDecodeError, subprocess.TimeoutExpired) as error:
        print("violin_adapter: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

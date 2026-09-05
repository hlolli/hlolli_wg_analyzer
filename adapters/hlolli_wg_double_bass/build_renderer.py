#!/usr/bin/python3 -I
"""Build and run one checked local double-bass experiment renderer."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Sequence


sys.dont_write_bytecode = True

EMBEDDED_CONFIG_JSON = None  # HWA_EMBED_CONFIG
ADAPTER_ID = "hlolli_wg_double_bass"
BUILD_SCHEMA = "hwa-double-bass-renderer-build"
RENDERER_SCHEMA = "hwa-double-bass-renderer"
SCHEMA_VERSION = 1
CHILD_FILE_LIMIT_BYTES = 64 * 1024 * 1024
MAX_JSON_INPUT_BYTES = 1024 * 1024
SOURCE_SHEBANG = "#!/usr/bin/python3 -I\n"
CASES = {
    "iowa2012-pizz-e-ff-open": {
        "articulation": 5,
        "binding_id": "iowa2012-pizz-e-ff-open",
        "force": 0.75,
        "frequency_hz": 41.20344461410875,
        "position": 0.12,
        "sample_rate": 48000,
        "speed": 0.80,
        "split": "fit",
        "string": 1,
    },
    "iowa2012-pizz-a-mf-open": {
        "articulation": 5,
        "binding_id": "iowa2012-pizz-a-mf-open",
        "force": 0.75,
        "frequency_hz": 55.0,
        "position": 0.12,
        "sample_rate": 48000,
        "speed": 0.80,
        "split": "fit",
        "string": 2,
    },
    "iowa2012-pizz-d-mf-open": {
        "articulation": 5,
        "binding_id": "iowa2012-pizz-d-mf-open",
        "force": 0.75,
        "frequency_hz": 73.41619197935188,
        "position": 0.12,
        "sample_rate": 48000,
        "speed": 0.80,
        "split": "fit",
        "string": 3,
    },
    "iowa2012-pizz-g-pp-open": {
        "articulation": 5,
        "binding_id": "iowa2012-pizz-g-pp-open",
        "force": 0.75,
        "frequency_hz": 97.99885899543733,
        "position": 0.12,
        "sample_rate": 48000,
        "speed": 0.80,
        "split": "fit",
        "string": 4,
    },
    "iowa2001-pizz-mf-open-e1-heldout": {
        "articulation": 5,
        "binding_id": "iowa2001-pizz-mf-open-e1-heldout",
        "force": 0.75,
        "frequency_hz": 41.20344461410875,
        "position": 0.12,
        "sample_rate": 44100,
        "speed": 0.80,
        "split": "check",
        "string": 1,
    },
    "iowa2001-pizz-mf-open-a1-heldout": {
        "articulation": 5,
        "binding_id": "iowa2001-pizz-mf-open-a1-heldout",
        "force": 0.75,
        "frequency_hz": 55.0,
        "position": 0.12,
        "sample_rate": 44100,
        "speed": 0.80,
        "split": "check",
        "string": 2,
    },
    "iowa2001-pizz-mf-open-d2-heldout": {
        "articulation": 5,
        "binding_id": "iowa2001-pizz-mf-open-d2-heldout",
        "force": 0.75,
        "frequency_hz": 73.41619197935188,
        "position": 0.12,
        "sample_rate": 44100,
        "speed": 0.80,
        "split": "check",
        "string": 3,
    },
    "iowa2001-pizz-mf-open-g2-heldout": {
        "articulation": 5,
        "binding_id": "iowa2001-pizz-mf-open-g2-heldout",
        "force": 0.75,
        "frequency_hz": 97.99885899543733,
        "position": 0.12,
        "sample_rate": 44100,
        "speed": 0.80,
        "split": "check",
        "string": 4,
    },
    "iowa2001-pizz-mf-open-e1-heldout-48k-soxr": {
        "articulation": 5,
        "binding_id": "iowa2001-pizz-mf-open-e1-heldout-48k-soxr",
        "force": 0.75,
        "frequency_hz": 41.20344461410875,
        "position": 0.12,
        "sample_rate": 48000,
        "speed": 0.80,
        "split": "check",
        "string": 1,
    },
    "iowa2001-pizz-mf-open-a1-heldout-48k-soxr": {
        "articulation": 5,
        "binding_id": "iowa2001-pizz-mf-open-a1-heldout-48k-soxr",
        "force": 0.75,
        "frequency_hz": 55.0,
        "position": 0.12,
        "sample_rate": 48000,
        "speed": 0.80,
        "split": "check",
        "string": 2,
    },
    "iowa2001-pizz-mf-open-d2-heldout-48k-soxr": {
        "articulation": 5,
        "binding_id": "iowa2001-pizz-mf-open-d2-heldout-48k-soxr",
        "force": 0.75,
        "frequency_hz": 73.41619197935188,
        "position": 0.12,
        "sample_rate": 48000,
        "speed": 0.80,
        "split": "check",
        "string": 3,
    },
    "iowa2001-pizz-mf-open-g2-heldout-48k-soxr": {
        "articulation": 5,
        "binding_id": "iowa2001-pizz-mf-open-g2-heldout-48k-soxr",
        "force": 0.75,
        "frequency_hz": 97.99885899543733,
        "position": 0.12,
        "sample_rate": 48000,
        "speed": 0.80,
        "split": "check",
        "string": 4,
    },
}
V2_CASE_TARGETS = {
    "iowa2012-pizz-e-string-g1-ff-left-48k-soxr": {
        "frequency_hz": 48.999429497718666,
        "parameter": "string_e_loss_seconds",
        "string": 1,
    },
    "iowa2012-pizz-a-ff-open-left-48k-soxr": {
        "frequency_hz": 55.0,
        "parameter": "string_a_loss_seconds",
        "string": 2,
    },
    "iowa2012-pizz-d-ff-open-left-48k-soxr": {
        "frequency_hz": 73.41619197935188,
        "parameter": "string_d_loss_seconds",
        "string": 3,
    },
    "iowa2012-pizz-g-ff-open-left-48k-soxr": {
        "frequency_hz": 97.99885899543733,
        "parameter": "string_g_loss_seconds",
        "string": 4,
    },
}
for v2_case_id, v2_target in V2_CASE_TARGETS.items():
    common = {
        "articulation": 5,
        "binding_id": v2_case_id,
        "force": 0.75,
        "frequency_hz": v2_target["frequency_hz"],
        "passive": True,
        "position": 0.12,
        "sample_rate": 48000,
        "speed": 0.80,
        "string": v2_target["string"],
    }
    CASES[v2_case_id] = dict(common, split="fit")
    CASES[v2_case_id + "-diagnostic-check"] = dict(common, split="check")

PARAMETER_PATHS = {
    "string_e_loss_seconds": ("strings", 0, "loss_time_constant_seconds"),
    "string_a_loss_seconds": ("strings", 1, "loss_time_constant_seconds"),
    "string_d_loss_seconds": ("strings", 2, "loss_time_constant_seconds"),
    "string_g_loss_seconds": ("strings", 3, "loss_time_constant_seconds"),
    "string_d_bridge_cutoff_hz": ("strings", 2, "bridge_cutoff_hz"),
}
LOSS_PARAMETERS = frozenset(
    name for name in PARAMETER_PATHS if name.endswith("_loss_seconds"))
PARAMETER_LIMITS = {
    name: ("seconds", 0.01, 30.0) for name in LOSS_PARAMETERS
}
PARAMETER_LIMITS["string_d_bridge_cutoff_hz"] = (
    "hertz", 1000.0, 7086.471045764144)
JOINT_PARAMETER = "joint_candidate"
JOINT_ADAPTER_IDS = frozenset({
    "hlolli_wg_double_bass-passive-joint-validation-v1",
    "hlolli_wg_double_bass-passive-joint-validation-v2",
})
FROZEN_JOINT_CHANGES = {
    "string_e_loss_seconds": {
        "after": 0.5, "before": 0.25, "maximum": 30.0,
        "minimum": 0.01, "source": "e1", "unit": "seconds",
    },
    "string_a_loss_seconds": {
        "after": 1.5, "before": 0.25, "maximum": 30.0,
        "minimum": 0.01, "source": "a1", "unit": "seconds",
    },
    "string_d_bridge_cutoff_hz": {
        "after": 1500.0, "before": 7086.471045764144,
        "maximum": 7086.471045764144, "minimum": 1000.0,
        "source": "d2", "unit": "hertz",
    },
    "string_d_loss_seconds": {
        "after": 3.0, "before": 0.25, "maximum": 3.0,
        "minimum": 0.25, "source": "d2", "unit": "seconds",
    },
    "string_g_loss_seconds": {
        "after": 0.5, "before": 0.25, "maximum": 30.0,
        "minimum": 0.01, "source": "g2", "unit": "seconds",
    },
}


class AdapterError(ValueError):
    pass


def reject_constant(value: str) -> Any:
    raise AdapterError(f"non-finite JSON number: {value}")


def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise AdapterError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def read_bounded(path: Path, field: str, limit: int) -> bytes:
    try:
        with path.open("rb") as stream:
            value = stream.read(limit + 1)
    except OSError as error:
        raise AdapterError(f"cannot read {field}: {path}: {error}") from error
    if len(value) > limit:
        raise AdapterError(f"{field} exceeds {limit} bytes: {path}")
    return value


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(
            read_bounded(
                path, "JSON input", MAX_JSON_INPUT_BYTES).decode("utf-8"),
            object_pairs_hook=unique_object,
            parse_constant=reject_constant,
        )
    except (UnicodeError, json.JSONDecodeError) as error:
        raise AdapterError(f"cannot read JSON: {path}: {error}") from error
    if type(value) is not dict:
        raise AdapterError(f"JSON root must be an object: {path}")
    return value


def canonical_json(value: Any) -> str:
    return json.dumps(
        value, ensure_ascii=True, allow_nan=False, sort_keys=True,
        separators=(",", ":"))


def exact_keys(value: dict[str, Any], wanted: set[str], field: str) -> None:
    found = set(value)
    if found != wanted:
        raise AdapterError(
            f"{field} fields differ; extra={sorted(found - wanted)} "
            f"missing={sorted(wanted - found)}")


def regular(path: Path, field: str) -> Path:
    result = path.absolute()
    if not result.is_file() or result.is_symlink():
        raise AdapterError(f"{field} must be a regular file: {result}")
    return result


def directory(path: Path, field: str) -> Path:
    result = path.absolute()
    if not result.is_dir() or result.is_symlink():
        raise AdapterError(f"{field} must be a directory: {result}")
    return result


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while True:
                block = stream.read(1024 * 1024)
                if not block:
                    break
                digest.update(block)
    except OSError as error:
        raise AdapterError(f"cannot hash file: {path}: {error}") from error
    return digest.hexdigest()


def checked_python_shebang(path: Path) -> str:
    text = str(path)
    try:
        encoded = ("#!" + text + " -I\n").encode("ascii")
    except UnicodeEncodeError as error:
        raise AdapterError(
            "python_executable is unsafe for a shebang") from error
    if (not path.is_absolute() or len(encoded) > 127 or
            any(byte < 0x21 or byte > 0x7e for byte in encoded[2:-4])):
        raise AdapterError("python_executable is unsafe for a shebang")
    return encoded.decode("ascii")


def build_config(path: Path) -> dict[str, Any]:
    source = load_json(path)
    wanted_fields = {
        "schema", "schema_version", "plugin_root", "csound_executable",
        "csound_include_dir", "c_compiler", "python_executable",
        "extra_resources", "macos_sdk_root", "permissions",
    }
    if "joint_candidate" in source:
        wanted_fields.add("joint_candidate")
    exact_keys(source, wanted_fields, "build config")
    if (source["schema"] != BUILD_SCHEMA or
            source["schema_version"] != SCHEMA_VERSION):
        raise AdapterError("unsupported build config")
    permissions = source["permissions"]
    if type(permissions) is not dict:
        raise AdapterError("permissions must be an object")
    exact_keys(permissions, {
        "render", "validate_profile", "write_profile",
    }, "permissions")
    if any(type(permissions[key]) is not bool for key in permissions):
        raise AdapterError("permissions must be boolean")
    for field in ("plugin_root", "csound_executable", "csound_include_dir",
                  "c_compiler", "python_executable"):
        if type(source[field]) is not str or not source[field]:
            raise AdapterError(f"{field} must be a nonempty string")

    plugin = directory(Path(source["plugin_root"]), "plugin_root")
    includes = directory(
        Path(source["csound_include_dir"]), "csound_include_dir")
    paths = {
        "c_compiler": regular(Path(source["c_compiler"]), "c_compiler"),
        "csound": regular(
            Path(source["csound_executable"]), "csound_executable"),
        "generator": regular(
            plugin / "tools" / "generate_model.py", "model generator"),
        "model": regular(
            plugin / "model" / "double_bass-v1.json", "source model"),
        "python": regular(
            Path(source["python_executable"]), "python_executable"),
        "source": regular(
            plugin / "src" / "hlolli_wg_double_bass.c", "plug-in source"),
    }
    checked_python_shebang(paths["python"])
    macos_sdk_root = source["macos_sdk_root"]
    if sys.platform == "darwin":
        if type(macos_sdk_root) is not str or not macos_sdk_root:
            raise AdapterError("macos_sdk_root is required on macOS")
        sdk_root = directory(Path(macos_sdk_root), "macos_sdk_root")
        paths["macos_sdk_settings"] = regular(
            sdk_root / "SDKSettings.json", "macOS SDK settings")
        compiler = {"macos_sdk_root": str(sdk_root)}
    else:
        if macos_sdk_root is not None:
            raise AdapterError("macos_sdk_root must be null off macOS")
        compiler = {"macos_sdk_root": None}
    include_count = 0
    for child in sorted(includes.rglob("*")):
        if child.is_symlink():
            raise AdapterError(f"Csound include tree contains a symlink: {child}")
        if child.is_dir():
            continue
        relative = child.relative_to(includes).as_posix()
        name = "csound_include/" + relative
        paths[name] = regular(child, f"Csound include {relative}")
        include_count += 1
        if include_count > 1024:
            raise AdapterError("Csound include tree has too many files")
    for required in ("csdl.h", "float-version.h", "version.h"):
        if "csound_include/" + required not in paths:
            raise AdapterError(f"Csound include tree lacks {required}")

    extra_resources = source["extra_resources"]
    if type(extra_resources) is not list or len(extra_resources) > 128:
        raise AdapterError("extra_resources must be a bounded list")
    for index, row in enumerate(extra_resources):
        if type(row) is not dict:
            raise AdapterError(f"extra_resources[{index}] must be an object")
        exact_keys(row, {"id", "path"}, f"extra_resources[{index}]")
        name = token(row["id"], f"extra_resources[{index}].id")
        if name in paths or name.startswith("csound_include"):
            raise AdapterError(f"duplicate or reserved resource ID: {name}")
        if type(row["path"]) is not str or not row["path"]:
            raise AdapterError(f"extra_resources[{index}].path is invalid")
        paths[name] = regular(
            Path(row["path"]), f"extra resource {name}")
    result = {
        "adapter_id": ADAPTER_ID,
        "compiler": compiler,
        "permissions": permissions,
        "platform": sys.platform,
        "resources": [
            {"id": name, "path": str(resource),
             "sha256": sha256(resource)}
            for name, resource in sorted(paths.items())
        ],
        "schema": RENDERER_SCHEMA,
        "schema_version": SCHEMA_VERSION,
    }
    if "joint_candidate" in source:
        result["joint_candidate"] = checked_joint_candidate(
            source["joint_candidate"], paths["model"])
    return result


def write_renderer(output: Path, config: dict[str, Any]) -> None:
    output = output.absolute()
    if output.exists() or output.is_symlink():
        raise AdapterError(f"output already exists: {output}")
    if not output.parent.is_dir():
        raise AdapterError(f"output parent is missing: {output.parent}")
    try:
        source = Path(__file__).absolute().read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise AdapterError(f"cannot read builder source: {error}") from error
    marker = "EMBEDDED_CONFIG_JSON = None" + "  # HWA_EMBED_CONFIG"
    python_rows = [
        row for row in config["resources"] if row.get("id") == "python"
    ]
    if source.count(marker) != 1 or not source.startswith(SOURCE_SHEBANG):
        raise AdapterError("builder source has an invalid embed marker")
    if len(python_rows) != 1:
        raise AdapterError("renderer config lacks one Python resource")
    source = (checked_python_shebang(Path(python_rows[0]["path"])) +
              source[len(SOURCE_SHEBANG):])
    replacement = (
        "EMBEDDED_CONFIG_JSON = " + repr(canonical_json(config)) +
        "  # HWA_EMBED_CONFIG")
    rendered = source.replace(marker, replacement).encode("utf-8")
    descriptor = -1
    temporary_text = None
    try:
        descriptor, temporary_text = tempfile.mkstemp(
            prefix=".hwa-double-bass-renderer-", dir=output.parent)
        os.fchmod(descriptor, 0o700)
        with os.fdopen(descriptor, "wb") as stream:
            descriptor = -1
            stream.write(rendered)
            stream.flush()
            os.fsync(stream.fileno())
        os.link(temporary_text, output, follow_symlinks=False)
        parent_descriptor = os.open(output.parent, os.O_RDONLY)
        try:
            os.fsync(parent_descriptor)
        finally:
            os.close(parent_descriptor)
    except FileExistsError as error:
        raise AdapterError(f"output already exists: {output}") from error
    except OSError as error:
        raise AdapterError(f"cannot publish renderer: {error}") from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if temporary_text is not None:
            try:
                Path(temporary_text).unlink()
            except FileNotFoundError:
                pass


def embedded_config() -> dict[str, Any]:
    if type(EMBEDDED_CONFIG_JSON) is not str:
        raise AdapterError("this source has no embedded renderer config")
    try:
        value = json.loads(
            EMBEDDED_CONFIG_JSON, object_pairs_hook=unique_object,
            parse_constant=reject_constant)
    except (json.JSONDecodeError, AdapterError) as error:
        raise AdapterError("embedded renderer config is invalid") from error
    if type(value) is not dict:
        raise AdapterError("embedded renderer config is not an object")
    wanted_fields = {
        "adapter_id", "compiler", "permissions", "platform", "resources",
        "schema", "schema_version",
    }
    if "joint_candidate" in value:
        wanted_fields.add("joint_candidate")
    exact_keys(value, wanted_fields, "embedded config")
    if (value["schema"] != RENDERER_SCHEMA or
            value["schema_version"] != SCHEMA_VERSION or
            value["adapter_id"] != ADAPTER_ID or
            value["platform"] != sys.platform or
            type(value["compiler"]) is not dict or
            type(value["resources"]) is not list or
            type(value["permissions"]) is not dict):
        raise AdapterError("embedded renderer config has invalid values")
    exact_keys(value["compiler"], {"macos_sdk_root"}, "compiler config")
    sdk_root = value["compiler"]["macos_sdk_root"]
    if ((sys.platform == "darwin" and
         (type(sdk_root) is not str or not sdk_root)) or
            (sys.platform != "darwin" and sdk_root is not None)):
        raise AdapterError("embedded compiler config has invalid values")
    if "joint_candidate" in value:
        value["joint_candidate"] = checked_joint_candidate(
            value["joint_candidate"])
    return value


def check_resources(config: dict[str, Any]) -> None:
    seen: set[str] = set()
    for index, row in enumerate(config["resources"]):
        if type(row) is not dict:
            raise AdapterError(f"resources[{index}] must be an object")
        exact_keys(row, {"id", "path", "sha256"}, f"resources[{index}]")
        name = row["id"]
        if type(name) is not str or not name or name in seen:
            raise AdapterError("resource IDs must be unique nonempty strings")
        seen.add(name)
        resource = regular(Path(row["path"]), f"configured resource {name}")
        if sha256(resource) != row["sha256"]:
            raise AdapterError(f"configured resource changed: {name}")


def resource_paths(config: dict[str, Any]) -> dict[str, Path]:
    check_resources(config)
    result = {row["id"]: Path(row["path"]) for row in config["resources"]}
    wanted = {
        "c_compiler", "csound", "csound_include/csdl.h",
        "csound_include/float-version.h", "csound_include/version.h",
        "generator", "model", "python", "source",
    }
    if not wanted.issubset(result):
        raise AdapterError("embedded renderer lacks a required resource")
    return result


def validate_profile(profile: Path) -> None:
    config = embedded_config()
    if config["permissions"].get("validate_profile") is not True:
        raise AdapterError("profile validation is not granted")
    resources = resource_paths(config)
    profile = regular(profile, "profile")
    profile_bytes = read_bounded(
        profile, "JSON input", MAX_JSON_INPUT_BYTES)
    with tempfile.TemporaryDirectory(
            prefix="hwa-double-bass-validate-") as text:
        scratch = Path(text)
        checked_profile = scratch / "profile.json"
        source = scratch / "hlolli_wg_double_bass.c"
        checked_profile.write_bytes(profile_bytes)
        shutil.copyfile(resources["source"], source)
        run_checked(
            [str(resources["python"]), "-I", str(resources["generator"]),
             "--model", str(checked_profile), "--source", str(source)],
            scratch, "profile validation")


def finite_number(value: Any, field: str) -> float:
    if (type(value) not in (int, float) or type(value) is bool or
            not math.isfinite(float(value))):
        raise AdapterError(f"{field} must be finite")
    return float(value)


def token(value: Any, field: str) -> str:
    if (type(value) is not str or not value or len(value) > 127 or
            any(character not in
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"
                for character in value)):
        raise AdapterError(f"{field} is invalid")
    return value


def hash_text(value: Any, field: str) -> str:
    if (type(value) is not str or len(value) != 64 or
            any(character not in "0123456789abcdef" for character in value)):
        raise AdapterError(f"{field} must be a lowercase SHA-256")
    return value


def path_value(root: Any, parts: tuple[Any, ...], field: str) -> float:
    current = root
    for part in parts:
        try:
            current = current[part]
        except (KeyError, IndexError, TypeError) as error:
            raise AdapterError(f"{field} path is missing: {parts}") from error
    return finite_number(current, field)


def checked_joint_candidate(value: Any,
                            model_path: Path | None = None) -> dict[str, Any]:
    if type(value) is not dict:
        raise AdapterError("joint_candidate must be an object")
    exact_keys(value, {
        "adapter_id", "candidate_profile_sha256", "cases", "changes",
    }, "joint_candidate")
    if value["adapter_id"] not in JOINT_ADAPTER_IDS:
        raise AdapterError("joint_candidate has an unsupported adapter_id")
    candidate_hash = hash_text(
        value["candidate_profile_sha256"],
        "joint_candidate candidate_profile_sha256")

    changes = value["changes"]
    if type(changes) is not list or len(changes) != len(PARAMETER_PATHS):
        raise AdapterError("joint_candidate must have five changes")
    seen_parameters: set[str] = set()
    source_hashes: dict[str, str] = {}
    checked_changes: list[dict[str, Any]] = []
    for index, row in enumerate(changes):
        field = f"joint_candidate changes[{index}]"
        if type(row) is not dict:
            raise AdapterError(f"{field} must be an object")
        exact_keys(row, {
            "after", "before", "maximum", "minimum", "parameter", "path",
            "source_fit_result_sha256", "unit",
        }, field)
        name = token(row["parameter"], field + " parameter")
        expected = FROZEN_JOINT_CHANGES.get(name)
        if expected is None or name in seen_parameters:
            raise AdapterError(f"{field} has an invalid parameter")
        seen_parameters.add(name)
        if row["unit"] != expected["unit"]:
            raise AdapterError(f"{field} has the wrong unit")
        path = row["path"]
        if type(path) is not list or tuple(path) != PARAMETER_PATHS[name]:
            raise AdapterError(f"{field} has the wrong model path")
        for number_field in ("after", "before", "maximum", "minimum"):
            found = finite_number(
                row[number_field], f"{field} {number_field}")
            if found != expected[number_field]:
                raise AdapterError(f"{field} differs from the frozen change")
        result_hash = hash_text(
            row["source_fit_result_sha256"],
            field + " source_fit_result_sha256")
        source = expected["source"]
        if source in source_hashes and source_hashes[source] != result_hash:
            raise AdapterError(f"{field} has a mismatched source result hash")
        source_hashes[source] = result_hash
        checked_changes.append(row)
    if (seen_parameters != set(FROZEN_JOINT_CHANGES) or
            set(source_hashes) != {"e1", "a1", "d2", "g2"}):
        raise AdapterError("joint_candidate has the wrong change set")

    cases = value["cases"]
    if type(cases) is not dict or not 1 <= len(cases) <= 64:
        raise AdapterError("joint_candidate cases must be a bounded object")
    required_splits = {"fit", "check", "audit"}
    routes: set[tuple[str, int]] = set()
    for case_id, row in cases.items():
        name = token(case_id, "joint_candidate case ID")
        if name in CASES or type(row) is not dict:
            raise AdapterError(f"invalid joint_candidate case: {name}")
        exact_keys(row, {
            "articulation", "binding_id", "binding_sha256", "force",
            "frequency_hz", "joint", "position", "sample_rate", "speed",
            "split", "string",
        }, f"joint_candidate case {name}")
        if row["joint"] is not True:
            raise AdapterError(f"joint_candidate case {name} is not joint")
        token(row["binding_id"], f"joint_candidate case {name} binding_id")
        hash_text(
            row["binding_sha256"],
            f"joint_candidate case {name} binding_sha256")
        split = row["split"]
        string_index = row["string"]
        if split not in required_splits or type(string_index) is not int or not (
                1 <= string_index <= 4):
            raise AdapterError(f"joint_candidate case {name} has invalid routing")
        route = (split, string_index)
        if route in routes:
            raise AdapterError(f"joint_candidate case {name} repeats a route")
        routes.add(route)
        if (type(row["articulation"]) is not int or
                row["articulation"] != 5 or row["sample_rate"] != 48000 or
                finite_number(row["frequency_hz"], name + " frequency_hz") <=
                0.0 or
                finite_number(row["force"], name + " force") != 0.75 or
                finite_number(row["speed"], name + " speed") != 0.8 or
                finite_number(row["position"], name + " position") != 0.12):
            raise AdapterError(f"joint_candidate case {name} has invalid values")
    expected_routes = {
        (split, string_index)
        for split in required_splits for string_index in (1, 2, 3, 4)
    }
    if routes != expected_routes or len(cases) != 12:
        raise AdapterError(
            "joint_candidate cases must have exactly one case per route")

    if model_path is not None:
        profile = load_json(model_path)
        for index, row in enumerate(checked_changes):
            parts = tuple(row["path"])
            before = path_value(
                profile, parts, f"joint_candidate changes[{index}] before")
            if before != float(row["before"]):
                raise AdapterError(
                    f"joint_candidate changes[{index}] before differs from model")
            set_path(profile, parts, float(row["after"]))
        candidate_source = (
            json.dumps(profile, indent=2, allow_nan=False) + "\n"
        ).encode("utf-8")
        if hashlib.sha256(candidate_source).hexdigest() != candidate_hash:
            raise AdapterError("joint_candidate candidate profile hash differs")
    return value


def renderer_cases(config: dict[str, Any]) -> dict[str, dict[str, Any]]:
    joint = config.get("joint_candidate")
    if type(joint) is dict:
        return dict(joint["cases"])
    return dict(CASES)


def checked_job(path: Path, output_dir: Path,
                config: dict[str, Any]) -> dict[str, Any]:
    job = load_json(regular(path, "render job"))
    exact_keys(job, {
        "schema", "schema_version", "method_version", "case_id", "job_id",
        "job_key", "inputs", "outputs", "parameters", "replicate", "seed",
        "split",
    }, "render job")
    if (job["schema"] != "hwa-render-job" or job["schema_version"] != 1 or
            job["method_version"] != "stage8-1"):
        raise AdapterError("unsupported render job")
    case_id = token(job["case_id"], "case_id")
    cases = renderer_cases(config)
    if case_id not in cases:
        raise AdapterError(f"unknown render case: {case_id}")
    case = cases[case_id]
    if (type(job["job_id"]) is not int or job["job_id"] < 1 or
            type(job["replicate"]) is not int or job["replicate"] < 0 or
            type(job["seed"]) is not int or job["seed"] < 0 or
            job["seed"] > 0xffffffffffffffff):
        raise AdapterError("render job has invalid integer fields")
    key = job["job_key"]
    if (type(key) is not str or len(key) != 64 or
            any(character not in "0123456789abcdef" for character in key)):
        raise AdapterError("render job has an invalid job_key")
    if job["split"] not in ("fit", "check", "audit"):
        raise AdapterError("render job has an invalid split")
    if job["split"] != case["split"]:
        raise AdapterError("render job split does not match its case")

    inputs = job["inputs"]
    if type(inputs) is not list or len(inputs) != 1 or type(inputs[0]) is not dict:
        raise AdapterError("render job needs one reference input")
    reference = inputs[0]
    exact_keys(reference, {
        "binding_id", "channels", "gain_db", "kind", "path",
        "probe_format", "probe_name", "rate_denominator", "rate_hz",
        "rate_numerator", "resource_id", "role", "sha256", "side",
        "start_sample", "unit", "value_count",
    }, "reference input")
    token(reference["binding_id"], "reference binding_id")
    if reference["binding_id"] != case["binding_id"]:
        raise AdapterError("reference binding does not match its case")
    if (reference["kind"] != "stem" or reference["side"] != "reference" or
            reference["role"] != "final" or
            reference["resource_id"] != "reference.final" or
            reference["probe_format"] is not None or
            reference["probe_name"] is not None or reference["unit"] is not None or
            reference["channels"] != 1 or
            reference["rate_hz"] != case["sample_rate"] or
            reference["rate_numerator"] != 0 or
            reference["rate_denominator"] != 0 or
            reference["start_sample"] != 0 or reference["value_count"] != 0 or
            finite_number(reference["gain_db"], "reference gain_db") != 0.0):
        raise AdapterError("reference input has unsupported fields")
    reference_path = regular(Path(reference["path"]), "reference WAVE")
    reference_hash = sha256(reference_path)
    if reference_hash != reference["sha256"]:
        raise AdapterError("reference WAVE hash changed")
    if ("binding_sha256" in case and
            reference_hash != case["binding_sha256"]):
        raise AdapterError("reference WAVE does not match its joint case")

    outputs = job["outputs"]
    if type(outputs) is not list or len(outputs) != 1 or type(outputs[0]) is not dict:
        raise AdapterError("render job needs one model output")
    output = outputs[0]
    exact_keys(output, {
        "id", "kind", "path", "side", "role", "probe_format",
        "probe_name", "unit", "start_sample", "gain_db", "rate_hz",
        "channels", "rate_numerator", "rate_denominator", "value_count",
    }, "model output")
    output_path = Path(output["path"]).absolute()
    if (output["id"] != "model.final" or output["kind"] != "stem" or
            output["side"] != "model" or output["role"] != "final" or
            output["probe_format"] is not None or
            output["probe_name"] is not None or output["unit"] is not None or
            output["start_sample"] != 0 or
            finite_number(output["gain_db"], "model gain_db") != 0.0 or
            output["rate_hz"] != case["sample_rate"] or
            output["channels"] != 1 or
            output["rate_numerator"] != 0 or
            output["rate_denominator"] != 0 or output["value_count"] != 0 or
            output_path.parent != output_dir or output_path.name != "model.wav" or
            output_path.exists() or output_path.is_symlink()):
        raise AdapterError("model output has unsupported fields")

    parameters = job["parameters"]
    if (type(parameters) is not list or not parameters or
            len(parameters) > len(PARAMETER_PATHS)):
        raise AdapterError("render job has the wrong parameter count")
    values: dict[str, float] = {}
    for index, row in enumerate(parameters):
        if type(row) is not dict:
            raise AdapterError(f"parameters[{index}] must be an object")
        exact_keys(row, {"id", "unit", "value"}, f"parameters[{index}]")
        name = token(row["id"], f"parameters[{index}].id")
        value = finite_number(row["value"], f"parameters[{index}].value")
        if case.get("joint") is True:
            valid = (name == JOINT_PARAMETER and row["unit"] == "choice" and
                     value in (0.0, 1.0))
        else:
            limits = PARAMETER_LIMITS.get(name)
            valid = (limits is not None and row["unit"] == limits[0] and
                     limits[1] <= value <= limits[2])
        if not valid or name in values:
            raise AdapterError(f"invalid render parameter: {name}")
        values[name] = value

    names = set(values)
    parameter_set_is_valid = False
    if case.get("joint") is True:
        parameter_set_is_valid = names == {JOINT_PARAMETER}
    else:
        base_case_id = case_id.removesuffix("-diagnostic-check")
        target = V2_CASE_TARGETS.get(base_case_id)
        if target is None:
            parameter_set_is_valid = names == set(LOSS_PARAMETERS)
        else:
            allowed = [{target["parameter"]}]
            d_frequency_parameters = {
                "string_d_bridge_cutoff_hz",
                "string_d_loss_seconds",
            }
            if target["string"] == 3:
                allowed.append(d_frequency_parameters)
            parameter_set_is_valid = names in allowed
            if (parameter_set_is_valid and
                    names == d_frequency_parameters and
                    not 0.25 <= values["string_d_loss_seconds"] <= 3.0):
                parameter_set_is_valid = False
    if not parameter_set_is_valid:
        raise AdapterError("render job has the wrong parameter set")
    job["reference_path"] = reference_path
    job["output_path"] = output_path
    job["parameter_values"] = values
    return job


def set_path(root: Any, parts: tuple[Any, ...], value: float) -> None:
    current = root
    for part in parts[:-1]:
        try:
            current = current[part]
        except (KeyError, IndexError, TypeError) as error:
            raise AdapterError(f"model path is missing: {parts}") from error
    final = parts[-1]
    try:
        old = current[final]
    except (KeyError, IndexError, TypeError) as error:
        raise AdapterError(f"model path is missing: {parts}") from error
    finite_number(old, f"model value at {parts}")
    current[final] = value


def csd_text(case: dict[str, Any], duration: float) -> str:
    passive = case.get("passive") is True or case.get("joint") is True
    lead = min(0.10, duration / 4.0) if passive else 0.0
    note_duration = duration - lead
    gate = "1" if passive else (
        f"timeinsts() < {max(0.05, duration - 0.50):.17g} ? 1 : 0")
    return """<CsoundSynthesizer>
<CsOptions>
-d -m0
</CsOptions>
<CsInstruments>
sr = {sample_rate}
ksmps = 32
nchnls = 1
0dbfs = 1

giBass hlolli_wg_double_bass_create

instr Bass
  kGate = {gate}
  aLeft, aRight hlolli_wg_double_bass kGate, {frequency_hz:.17g}, \
      {force:.17g}, {speed:.17g}, {position:.17g}, 0, 0, \
      {articulation}, 0, 0, {string}, giBass
  out 0.5 * (aLeft + aRight)
endin
</CsInstruments>
<CsScore>
i "Bass" {lead:.17g} {note_duration:.17g}
e
</CsScore>
</CsoundSynthesizer>
""".format(gate=gate, lead=lead, note_duration=note_duration, **case)


def run_checked(command: list[str], cwd: Path, label: str,
                timeout: int = 30) -> None:
    def limit_child_files() -> None:
        import resource
        unused_soft, hard = resource.getrlimit(resource.RLIMIT_FSIZE)
        limit = CHILD_FILE_LIMIT_BYTES
        if hard != resource.RLIM_INFINITY:
            limit = min(limit, hard)
        resource.setrlimit(resource.RLIMIT_FSIZE, (limit, hard))

    def log_tail(stream: Any) -> str:
        stream.flush()
        stream.seek(0, os.SEEK_END)
        length = stream.tell()
        stream.seek(max(0, length - 4000))
        return stream.read().decode("utf-8", errors="replace").strip()

    with tempfile.TemporaryFile(dir=cwd) as stdout_stream, \
            tempfile.TemporaryFile(dir=cwd) as stderr_stream:
        completed = subprocess.run(
            command, check=False, stdout=stdout_stream, stderr=stderr_stream,
            cwd=cwd,
            env={"LC_ALL": "C", "LANG": "C", "TZ": "UTC",
                 "TMPDIR": str(cwd), "TMP": str(cwd), "TEMP": str(cwd)},
            timeout=timeout,
            preexec_fn=limit_child_files if os.name == "posix" else None)
        detail = log_tail(stderr_stream) or log_tail(stdout_stream)
    if completed.returncode != 0:
        raise AdapterError(f"{label} failed" + (f": {detail}" if detail else ""))


def pcm_wave_frames(path: Path, expected_rate: int) -> int:
    """Read the frame count from checked mono PCM WAVE audio."""
    pcm_guid = bytes.fromhex("0100000000001000800000aa00389b71")
    try:
        file_bytes = path.stat().st_size
        with path.open("rb") as stream:
            header = stream.read(12)
            if (len(header) != 12 or header[:4] != b"RIFF" or
                    header[8:12] != b"WAVE"):
                raise AdapterError("reference audio is not a RIFF WAVE file")
            declared = int.from_bytes(header[4:8], "little") + 8
            if declared != file_bytes:
                raise AdapterError("reference WAVE size does not match RIFF")
            offset = 12
            format_data = None
            data_bytes = None
            chunk_count = 0
            while offset < declared:
                if offset + 8 > declared or chunk_count >= 1024:
                    raise AdapterError("reference WAVE has invalid chunks")
                stream.seek(offset)
                chunk = stream.read(8)
                if len(chunk) != 8:
                    raise AdapterError("reference WAVE chunk is truncated")
                size = int.from_bytes(chunk[4:8], "little")
                end = offset + 8 + size
                padded_end = end + (size & 1)
                if end < offset or padded_end > declared:
                    raise AdapterError("reference WAVE chunk is truncated")
                if chunk[:4] == b"fmt ":
                    if format_data is not None or size < 16 or size > 1024:
                        raise AdapterError("reference WAVE has invalid format")
                    format_data = stream.read(size)
                    if len(format_data) != size:
                        raise AdapterError("reference WAVE format is truncated")
                elif chunk[:4] == b"data":
                    if data_bytes is not None:
                        raise AdapterError("reference WAVE repeats audio data")
                    data_bytes = size
                offset = padded_end
                chunk_count += 1
    except OSError as error:
        raise AdapterError(f"cannot read reference WAVE: {error}") from error

    if format_data is None or data_bytes is None:
        raise AdapterError("reference WAVE lacks format or audio data")
    tag = int.from_bytes(format_data[0:2], "little")
    channels = int.from_bytes(format_data[2:4], "little")
    rate = int.from_bytes(format_data[4:8], "little")
    byte_rate = int.from_bytes(format_data[8:12], "little")
    block_align = int.from_bytes(format_data[12:14], "little")
    bits = int.from_bytes(format_data[14:16], "little")
    if tag == 0xfffe:
        if (len(format_data) < 40 or
                int.from_bytes(format_data[16:18], "little") < 22 or
                int.from_bytes(format_data[18:20], "little") != bits or
                format_data[24:40] != pcm_guid):
            raise AdapterError("reference WAVE extensible format is not PCM")
    elif tag != 1:
        raise AdapterError("reference WAVE format is not integer PCM")
    width = bits // 8
    if (channels != 1 or rate != expected_rate or bits not in (16, 24) or
            block_align != channels * width or
            byte_rate != rate * block_align or data_bytes == 0 or
            data_bytes % block_align != 0):
        raise AdapterError(
            f"reference WAVE must be nonempty mono {expected_rate} Hz "
            "PCM16 or PCM24")
    return data_bytes // block_align


def trim_rendered_wave(path: Path, wanted_frames: int,
                       expected_rate: int) -> None:
    """Remove at most one padded control block from a rendered WAVE."""
    path = regular(path, "rendered model WAVE")
    try:
        file_bytes = path.stat().st_size
        with path.open("r+b") as stream:
            header = stream.read(12)
            if (len(header) != 12 or header[:4] != b"RIFF" or
                    header[8:12] != b"WAVE" or
                    int.from_bytes(header[4:8], "little") + 8 != file_bytes):
                raise AdapterError("rendered model is not a bounded RIFF WAVE")
            offset = 12
            format_data = None
            fact_offset = None
            data_header = None
            data_offset = None
            data_bytes = None
            chunk_count = 0
            while offset < file_bytes:
                if offset + 8 > file_bytes or chunk_count >= 1024:
                    raise AdapterError("rendered model has invalid chunks")
                stream.seek(offset)
                chunk = stream.read(8)
                if len(chunk) != 8:
                    raise AdapterError("rendered model chunk is truncated")
                size = int.from_bytes(chunk[4:8], "little")
                end = offset + 8 + size
                padded_end = end + (size & 1)
                if end < offset or padded_end > file_bytes:
                    raise AdapterError("rendered model chunk is truncated")
                if chunk[:4] == b"fmt ":
                    if format_data is not None or size < 16 or size > 1024:
                        raise AdapterError("rendered model has invalid format")
                    format_data = stream.read(size)
                elif chunk[:4] == b"fact":
                    if fact_offset is not None or size < 4:
                        raise AdapterError("rendered model has invalid facts")
                    fact_offset = offset + 8
                elif chunk[:4] == b"data":
                    if data_bytes is not None or padded_end != file_bytes:
                        raise AdapterError("rendered model audio must be last")
                    data_header = offset
                    data_offset = offset + 8
                    data_bytes = size
                offset = padded_end
                chunk_count += 1
            if format_data is None or data_bytes is None:
                raise AdapterError("rendered model lacks format or audio data")
            tag = int.from_bytes(format_data[0:2], "little")
            channels = int.from_bytes(format_data[2:4], "little")
            rate = int.from_bytes(format_data[4:8], "little")
            block_align = int.from_bytes(format_data[12:14], "little")
            bits = int.from_bytes(format_data[14:16], "little")
            if (tag != 1 or channels != 1 or rate != expected_rate or
                    bits != 24 or block_align != 3 or
                    data_bytes % block_align != 0):
                raise AdapterError(
                    f"rendered model must be mono {expected_rate} Hz "
                    "integer PCM24")
            found_frames = data_bytes // block_align
            if (wanted_frames < 1 or found_frames < wanted_frames or
                    found_frames - wanted_frames >= 32):
                raise AdapterError("rendered model frame count is out of bounds")
            wanted_data_bytes = wanted_frames * block_align
            padding_bytes = wanted_data_bytes & 1
            final_bytes = data_offset + wanted_data_bytes + padding_bytes
            if final_bytes - 8 > 0xffffffff or wanted_data_bytes > 0xffffffff:
                raise AdapterError("rendered model is too large for RIFF")
            stream.seek(4)
            stream.write((final_bytes - 8).to_bytes(4, "little"))
            if fact_offset is not None:
                if wanted_frames > 0xffffffff:
                    raise AdapterError("rendered model has too many frames")
                stream.seek(fact_offset)
                stream.write(wanted_frames.to_bytes(4, "little"))
            stream.seek(data_header + 4)
            stream.write(wanted_data_bytes.to_bytes(4, "little"))
            if padding_bytes:
                stream.seek(data_offset + wanted_data_bytes)
                stream.write(b"\0")
            stream.flush()
            os.ftruncate(stream.fileno(), final_bytes)
            os.fsync(stream.fileno())
    except OSError as error:
        raise AdapterError(f"cannot trim rendered model WAVE: {error}") from error


def render_job(request: Path, output_dir: Path) -> None:
    config = embedded_config()
    if config["permissions"].get("render") is not True:
        raise AdapterError("rendering is not granted")
    resources = resource_paths(config)
    output_dir = directory(output_dir, "output directory")
    job = checked_job(request, output_dir, config)
    case = renderer_cases(config)[job["case_id"]]
    sample_rate = case["sample_rate"]
    reference_frames = pcm_wave_frames(job["reference_path"], sample_rate)
    duration = reference_frames / sample_rate
    profile = load_json(resources["model"])
    if case.get("joint") is True:
        if job["parameter_values"][JOINT_PARAMETER] == 1.0:
            joint = config["joint_candidate"]
            for change in joint["changes"]:
                set_path(
                    profile, tuple(change["path"]), float(change["after"]))
            candidate_source = (
                json.dumps(profile, indent=2, allow_nan=False) + "\n"
            ).encode("utf-8")
            if hashlib.sha256(candidate_source).hexdigest() != joint[
                    "candidate_profile_sha256"]:
                raise AdapterError("joint candidate profile hash changed")
    else:
        for name, value in job["parameter_values"].items():
            set_path(profile, PARAMETER_PATHS[name], value)

    scratch = output_dir
    model = scratch / ".hwa-double-bass-candidate.json"
    source = scratch / ".hwa-double-bass-source.c"
    module = scratch / (".hwa-double-bass-module.dylib"
                        if sys.platform == "darwin"
                        else ".hwa-double-bass-module.so")
    csd = scratch / ".hwa-double-bass-render.csd"
    xcrun_cache = scratch / "xcrun_db"
    scratch_files = (model, source, module, csd, xcrun_cache)
    if any(path.exists() or path.is_symlink() for path in scratch_files):
        raise AdapterError("render scratch path already exists")
    try:
        model.write_text(json.dumps(profile, indent=2, allow_nan=False) + "\n",
                         encoding="utf-8")
        shutil.copyfile(resources["source"], source)
        run_checked(
            [str(resources["python"]), "-I", str(resources["generator"]),
             "--model", str(model), "--source", str(source)],
            scratch, "model generation")
        check_resources(config)
        compile_command = [
            str(resources["c_compiler"]), "-DBUILD_PLUGINS", "-std=c11",
            "-fPIC", "-fvisibility=hidden", "-O2",
            "-I" + str(resources["csound_include/csdl.h"].parent),
            "-dynamiclib" if sys.platform == "darwin" else "-shared",
        ]
        if sys.platform == "darwin":
            sdk_root = directory(
                Path(config["compiler"]["macos_sdk_root"]),
                "configured macOS SDK root")
            compile_command.extend(["-isysroot", str(sdk_root)])
        compile_command.extend(["-o", str(module), str(source)])
        if sys.platform != "darwin":
            compile_command.append("-lm")
        run_checked(compile_command, scratch, "plug-in compile")
        check_resources(config)
        csd.write_text(csd_text(case, duration),
                       encoding="utf-8")
        run_checked(
            [str(resources["csound"]), "--opcode-lib=" + str(module),
             "--sample-accurate", "--num-threads=1", "-d", "-m0", "-W",
             "-3", "-K", "-o", str(job["output_path"]), str(csd)],
            scratch, "Csound render", timeout=120)
        check_resources(config)
        trim_rendered_wave(
            job["output_path"], reference_frames, sample_rate)
    finally:
        for path in scratch_files:
            try:
                path.unlink()
            except FileNotFoundError:
                pass
    regular(job["output_path"], "rendered model WAVE")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build")
    build.add_argument("--config", required=True, type=Path)
    build.add_argument("--output", required=True, type=Path)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        arguments = list(sys.argv[1:] if argv is None else argv)
        if arguments == ["--describe"]:
            config = embedded_config()
            check_resources(config)
            print(json.dumps(config, sort_keys=True, allow_nan=False))
            return 0
        if len(arguments) == 2 and arguments[0] == "--validate-profile":
            validate_profile(Path(arguments[1]))
            return 0
        if (len(arguments) == 4 and
                arguments[0] == "--hwa-experiment-job" and
                arguments[2] == "--output-dir"):
            render_job(Path(arguments[1]), Path(arguments[3]))
            return 0
        options = build_parser().parse_args(arguments)
        if EMBEDDED_CONFIG_JSON is not None:
            raise AdapterError("an embedded renderer cannot build another renderer")
        config = build_config(options.config)
        write_renderer(options.output, config)
        return 0
    except (AdapterError, OSError, UnicodeError, ValueError,
            subprocess.TimeoutExpired) as error:
        print(f"double_bass_renderer: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

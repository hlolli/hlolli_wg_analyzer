#!/usr/bin/env python3
"""Select a combined instrument fit and apply it through a checked adapter."""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any, Optional
import wave


class FitError(ValueError):
    pass


SELECTION_METHOD_VERSION = "instrument-fit-selection-v1"
FIT_ONLY_SELECTION_METHOD_VERSION = "instrument-fit-fit-only-v1"
VERIFY_CANDIDATE_METHOD_VERSION = "instrument-fit-verify-candidate-v1"
PASSIVE_DECAY_METHOD_VERSION = "passive-decay-v4"
PASSIVE_DECAY_SHAPE_METHOD_VERSION = "passive-decay-shape-v1"
HARMONIC_DECAY_METHOD_VERSION = "harmonic-decay-v1"
CHECKED_NOTE_METHOD_VERSION = "isolated-note-1"
CHECKED_HARMONIC_DECAY_METHOD_VERSION = "harmonic-decay-1"
T60_DB_PER_OCTAVE = 20.0 * math.log10(2.0)
INVALID_HARMONIC_LOSS_OCTAVES = 8.0
MAX_PASSIVE_WAVE_BYTES = 16 * 1024 * 1024
MAX_PASSIVE_FRAMES = 1_000_000
MAX_PASSIVE_CHANNELS = 8
MAX_PASSIVE_RATE_HZ = 384_000
MAX_CONTROL_FILE_BYTES = 16 * 1024 * 1024
MIN_PASSIVE_DYNAMIC_RANGE_DB = 20.0
MAX_PASSIVE_LINE_RESIDUAL_DB = 5.0
HARMONIC_WINDOW_SECONDS = 0.08
HARMONIC_HOP_SECONDS = 0.08
HARMONIC_LATE_START_SECONDS = 0.04
HARMONIC_BAND_HALF_WIDTH_HZ = 0.0
HARMONIC_BAND_OFFSETS_HZ = (0.0,)
HARMONIC_FIT_RANGE_DB = 35.0
MIN_HARMONIC_LEVEL_DBFS = -90.0
MIN_HARMONIC_SUPPORT_SECONDS = 0.20
MAX_HARMONIC_LINE_RESIDUAL_DB = 5.0
MIN_HARMONIC_VALID_BANDS = 3
MAX_HARMONIC_BANDS = 16


def object_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise FitError(f"duplicate JSON key: {key}")
        value[key] = item
    return value


def parse_json(source: bytes, path: Path) -> dict[str, Any]:
    try:
        value = json.loads(source.decode("utf-8"), object_pairs_hook=object_pairs)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise FitError(f"cannot read JSON: {path}: {error}") from error
    if type(value) is not dict:
        raise FitError(f"JSON root must be an object: {path}")
    return value


def file_snapshot(path: Path, field: str,
                  limit: int = MAX_CONTROL_FILE_BYTES) -> tuple[Path, bytes, str]:
    path = regular(path, field)
    try:
        if path.stat().st_size > limit:
            raise FitError(f"{field} exceeds the byte limit")
        with path.open("rb") as stream:
            source = stream.read(limit + 1)
    except OSError as error:
        raise FitError(f"cannot read {field}: {path}: {error}") from error
    if len(source) > limit:
        raise FitError(f"{field} exceeds the byte limit")
    return path, source, hashlib.sha256(source).hexdigest()


def load_json(path: Path) -> dict[str, Any]:
    path, source, _ = file_snapshot(path, "JSON file")
    return parse_json(source, path)


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
        raise FitError(f"cannot hash file: {path}: {error}") from error
    return digest.hexdigest()


def token(value: Any, field: str) -> str:
    if type(value) is not str or not value or len(value) > 127:
        raise FitError(f"{field} must be a short nonempty string")
    allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"
    if any(character not in allowed for character in value):
        raise FitError(f"{field} has an invalid character")
    return value


def finite(value: Any, field: str) -> float:
    if type(value) not in (int, float) or not math.isfinite(float(value)):
        raise FitError(f"{field} must be finite")
    return float(value)


def positive(value: Any, field: str) -> float:
    result = finite(value, field)
    if result <= 0.0:
        raise FitError(f"{field} must be positive")
    return result


def nonnegative(value: Any, field: str) -> float:
    result = finite(value, field)
    if result < 0.0:
        raise FitError(f"{field} must be nonnegative")
    return result


def digest(value: Any, field: str) -> str:
    if (type(value) is not str or len(value) != 64 or
            any(character not in "0123456789abcdef" for character in value)):
        raise FitError(f"{field} must be a lowercase SHA-256 digest")
    return value


def exact_keys(value: dict[str, Any], expected: set[str], field: str) -> None:
    if set(value) != expected:
        raise FitError(f"{field} has unexpected or missing fields")


def validate_profile_path(parts: Any, field: str) -> list[Any]:
    if type(parts) is not list or not parts:
        raise FitError(f"{field} is invalid")
    for part in parts:
        if type(part) not in (str, int) or type(part) is bool:
            raise FitError("profile path parts must be strings or integers")
    return parts


def regular(path: Path, field: str) -> Path:
    path = path.absolute()
    if not path.is_file() or path.is_symlink():
        raise FitError(f"{field} must be a regular file: {path}")
    return path


def parse_bindings(rows: list[str]) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for row in rows:
        if "=" not in row:
            raise FitError("bindings must use ID=PATH")
        name, text = row.split("=", 1)
        name = token(name, "binding id")
        if name in result:
            raise FitError(f"duplicate binding: {name}")
        result[name] = regular(Path(text), f"binding {name}")
    return result


def fit_manifest(path: Path, source: Optional[bytes] = None) -> dict[str, Any]:
    value = load_json(path) if source is None else parse_json(source, path)
    if (value.get("schema") != "hwa-instrument-fit" or
            value.get("schema_version") not in (1, 2)):
        raise FitError("unsupported fit manifest")
    version = value["schema_version"]
    if version == 2:
        exact_keys(value, {
            "schema", "schema_version", "adapter_id", "parameters",
            "objectives", "selection", "candidate",
        }, "fit manifest")
    token(value.get("adapter_id"), "adapter_id")
    parameters = value.get("parameters")
    objectives = value.get("objectives")
    selection = value.get("selection")
    if type(parameters) is not list or not parameters:
        raise FitError("fit manifest needs parameters")
    if type(objectives) is not list or not objectives:
        raise FitError("fit manifest needs objectives")
    if type(selection) is not dict:
        raise FitError("fit manifest needs selection")
    seen: set[str] = set()
    for index, row in enumerate(parameters):
        if type(row) is not dict:
            raise FitError(f"parameters[{index}] must be an object")
        name = token(row.get("id"), f"parameters[{index}].id")
        if name in seen:
            raise FitError(f"duplicate parameter: {name}")
        seen.add(name)
        token(row.get("unit"), f"parameters[{index}].unit")
        minimum = finite(row.get("minimum"), f"parameters[{index}].minimum")
        maximum = finite(row.get("maximum"), f"parameters[{index}].maximum")
        baseline = finite(row.get("baseline"), f"parameters[{index}].baseline")
        if minimum >= maximum:
            raise FitError(f"parameters[{index}] has an empty range")
        if baseline < minimum or baseline > maximum:
            raise FitError(f"parameters[{index}].baseline is out of range")
        if version == 2:
            exact_keys(row, {
                "id", "unit", "minimum", "maximum", "baseline",
                "profile_paths",
            }, f"parameters[{index}]")
        paths = row.get("profile_paths")
        if type(paths) is not list:
            raise FitError(f"parameters[{index}] needs profile_paths")
        for path_index, profile_path in enumerate(paths):
            validate_profile_path(
                profile_path,
                f"parameters[{index}].profile_paths[{path_index}]",
            )
    objective_ids: set[str] = set()
    for index, row in enumerate(objectives):
        if type(row) is not dict:
            raise FitError(f"objectives[{index}] must be an object")
        name = token(row.get("id"), f"objectives[{index}].id")
        if name in objective_ids:
            raise FitError(f"duplicate objective: {name}")
        objective_ids.add(name)
        kind = row.get("kind")
        if kind not in ("experiment-gap", "body-envelope", "passive-decay",
                        "passive-decay-shape", "harmonic-decay",
                        "checked-note-harmonic-decay"):
            raise FitError(f"objectives[{index}] has an unknown kind")
        if version == 2:
            objective_keys = {
                "id", "kind", "case", "reference_binding", "resource_id",
                "split", "weight", "scale",
            }
            if kind == "checked-note-harmonic-decay":
                objective_keys.update({"expected_hz", "reference_sha256"})
            exact_keys(row, objective_keys, f"objectives[{index}]")
        valid_splits = ("fit", "check", "audit") if version == 2 else (
            "fit", "check"
        )
        if row.get("split") not in valid_splits:
            raise FitError(f"objectives[{index}] has an invalid split")
        positive(row.get("weight"), f"objectives[{index}].weight")
        positive(row.get("scale"), f"objectives[{index}].scale")
        source_group = row.get("source_group")
        if source_group is not None:
            token(source_group, f"objectives[{index}].source_group")
        if kind == "experiment-gap":
            token(row.get("response"), f"objectives[{index}].response")
        else:
            token(row.get("case"), f"objectives[{index}].case")
            token(row.get("reference_binding"),
                  f"objectives[{index}].reference_binding")
            token(row.get("resource_id"), f"objectives[{index}].resource_id")
            if kind == "harmonic-decay":
                positive(row.get("fundamental_hz"),
                         f"objectives[{index}].fundamental_hz")
                harmonic_count = row.get("harmonic_count")
                if (type(harmonic_count) is not int or
                        type(harmonic_count) is bool or
                        harmonic_count < MIN_HARMONIC_VALID_BANDS or
                        harmonic_count > MAX_HARMONIC_BANDS):
                    raise FitError(
                        f"objectives[{index}].harmonic_count is out of range"
                    )
            elif kind == "checked-note-harmonic-decay":
                positive(row.get("expected_hz"),
                         f"objectives[{index}].expected_hz")
                digest(row.get("reference_sha256"),
                       f"objectives[{index}].reference_sha256")
    if version == 1:
        mode = selection.get("mode", "fit-check")
        if mode not in ("fit-check", "fit-only"):
            raise FitError("selection.mode must be fit-check or fit-only")
        nonnegative(selection.get("check_weight"), "selection.check_weight")
        finite(selection.get("max_check_loss_increase"),
               "selection.max_check_loss_increase")
        finite(selection.get("max_candidate_worst_harm"),
               "selection.max_candidate_worst_harm")
        objective_splits = {row["split"] for row in objectives}
        if mode == "fit-only":
            if (objective_splits != {"fit"} or
                    float(selection["check_weight"]) != 0.0 or
                    float(selection["max_check_loss_increase"]) != 0.0):
                raise FitError(
                    "fit-only selection needs only fit objectives and zero "
                    "check limits"
                )
        elif not {"fit", "check"}.issubset(objective_splits):
            raise FitError("fit-check selection needs fit and check objectives")
        if "max_objective_loss_increase" in selection:
            nonnegative(
                selection.get("max_objective_loss_increase"),
                "selection.max_objective_loss_increase",
            )
        source_group_keys = {
            "max_candidate_source_mean_loss",
            "max_source_mean_loss_increase",
        }
        source_group_present = source_group_keys.intersection(selection)
        if source_group_present and source_group_present != source_group_keys:
            raise FitError(
                "selection must provide every source-group limit"
            )
        if source_group_present:
            for name in sorted(source_group_keys):
                nonnegative(selection.get(name), "selection." + name)
            if any(row.get("source_group") is None for row in objectives):
                raise FitError(
                    "source-group limits need a group on every objective"
                )
            splits = ("fit",) if mode == "fit-only" else ("fit", "check")
            for split in splits:
                groups = {
                    row["source_group"] for row in objectives
                    if row["split"] == split
                }
                if len(groups) < 2:
                    raise FitError(
                        "source-group selection needs two groups per split"
                    )
        absolute_keys = {
            "max_candidate_loss", "minimum_candidate_t60_ratio",
            "maximum_candidate_t60_ratio",
            "minimum_candidate_support_ratio",
        }
        present = absolute_keys.intersection(selection)
        if present and present != absolute_keys:
            raise FitError(
                "selection must provide every absolute candidate limit"
            )
        if present:
            nonnegative(selection.get("max_candidate_loss"),
                        "selection.max_candidate_loss")
            minimum_t60 = positive(
                selection.get("minimum_candidate_t60_ratio"),
                "selection.minimum_candidate_t60_ratio",
            )
            maximum_t60 = positive(
                selection.get("maximum_candidate_t60_ratio"),
                "selection.maximum_candidate_t60_ratio",
            )
            if minimum_t60 > maximum_t60:
                raise FitError("selection candidate T60 ratio range is empty")
            positive(selection.get("minimum_candidate_support_ratio"),
                     "selection.minimum_candidate_support_ratio")
        shape_keys = {
            "max_candidate_level_rmse_db",
            "max_candidate_spectral_rmse_db",
        }
        shape_present = shape_keys.intersection(selection)
        has_shape = any(
            row["kind"] == "passive-decay-shape" for row in objectives
        )
        if has_shape and shape_present != shape_keys:
            raise FitError(
                "passive-decay-shape needs both component RMSE limits"
            )
        if not has_shape and shape_present:
            raise FitError(
                "component RMSE limits need a passive-decay-shape objective"
            )
        for name in sorted(shape_present):
            nonnegative(selection.get(name), "selection." + name)
        harmonic_keys = {
            "max_candidate_harmonic_mean_error_octaves",
            "max_candidate_harmonic_maximum_error_octaves",
        }
        harmonic_present = harmonic_keys.intersection(selection)
        has_harmonic = any(
            row["kind"] in (
                "harmonic-decay", "checked-note-harmonic-decay"
            ) for row in objectives
        )
        if has_harmonic and harmonic_present != harmonic_keys:
            raise FitError(
                "harmonic-decay needs both harmonic error limits"
            )
        if not has_harmonic and harmonic_present:
            raise FitError(
                "harmonic error limits need a harmonic-decay objective"
            )
        for name in sorted(harmonic_present):
            nonnegative(selection.get(name), "selection." + name)
    else:
        validate_verify_candidate_manifest(value, objective_ids)
    return value


def validate_verify_candidate_manifest(
        manifest: dict[str, Any], objective_ids: set[str]) -> None:
    parameters = manifest["parameters"]
    objectives = manifest["objectives"]
    selection = manifest["selection"]
    candidate = manifest.get("candidate")
    selection_keys = {
        "mode", "score_weights", "max_score_increase",
        "max_candidate_worst_harm", "max_expected_loss_increase", "limits",
        "minimum_candidate_t60_ratio", "maximum_candidate_t60_ratio",
        "minimum_candidate_support_ratio",
    }
    has_checked_harmonic = any(
        row["kind"] == "checked-note-harmonic-decay" for row in objectives
    )
    if has_checked_harmonic:
        selection_keys.update({
            "max_candidate_harmonic_mean_error_octaves",
            "max_candidate_harmonic_maximum_error_octaves",
            "minimum_candidate_harmonic_count",
        })
    exact_keys(selection, selection_keys, "selection")
    if selection.get("mode") != "verify-candidate":
        raise FitError("selection.mode must be verify-candidate")
    score_weights = selection.get("score_weights")
    if type(score_weights) is not dict:
        raise FitError("selection.score_weights must be an object")
    exact_keys(score_weights, {"fit", "check", "audit"},
               "selection.score_weights")
    for split in ("fit", "check", "audit"):
        positive(score_weights.get(split),
                 f"selection.score_weights.{split}")
    nonnegative(selection.get("max_score_increase"),
                "selection.max_score_increase")
    nonnegative(selection.get("max_candidate_worst_harm"),
                "selection.max_candidate_worst_harm")
    nonnegative(selection.get("max_expected_loss_increase"),
                "selection.max_expected_loss_increase")
    minimum_t60 = positive(selection.get("minimum_candidate_t60_ratio"),
                           "selection.minimum_candidate_t60_ratio")
    maximum_t60 = positive(selection.get("maximum_candidate_t60_ratio"),
                           "selection.maximum_candidate_t60_ratio")
    if minimum_t60 > maximum_t60:
        raise FitError("selection candidate T60 ratio range is empty")
    positive(selection.get("minimum_candidate_support_ratio"),
             "selection.minimum_candidate_support_ratio")
    if has_checked_harmonic:
        nonnegative(
            selection.get("max_candidate_harmonic_mean_error_octaves"),
            "selection.max_candidate_harmonic_mean_error_octaves",
        )
        nonnegative(
            selection.get("max_candidate_harmonic_maximum_error_octaves"),
            "selection.max_candidate_harmonic_maximum_error_octaves",
        )
        minimum_harmonics = selection.get("minimum_candidate_harmonic_count")
        if (type(minimum_harmonics) is not int or
                type(minimum_harmonics) is bool or
                minimum_harmonics < MIN_HARMONIC_VALID_BANDS or
                minimum_harmonics > MAX_HARMONIC_BANDS):
            raise FitError(
                "selection.minimum_candidate_harmonic_count is out of range"
            )
    limits = selection.get("limits")
    if type(limits) is not list or len(limits) != 3:
        raise FitError("selection.limits must name fit, check, and audit")
    seen_splits: set[str] = set()
    for index, row in enumerate(limits):
        if type(row) is not dict:
            raise FitError(f"selection.limits[{index}] must be an object")
        exact_keys(row, {
            "split", "max_mean_loss_increase",
            "max_objective_loss_increase", "max_candidate_loss",
        }, f"selection.limits[{index}]")
        split = row.get("split")
        if split not in ("fit", "check", "audit") or split in seen_splits:
            raise FitError("selection.limits has an invalid or duplicate split")
        seen_splits.add(split)
        nonnegative(row.get("max_mean_loss_increase"),
                    f"selection.limits[{index}].max_mean_loss_increase")
        nonnegative(row.get("max_objective_loss_increase"),
                    f"selection.limits[{index}].max_objective_loss_increase")
        nonnegative(row.get("max_candidate_loss"),
                    f"selection.limits[{index}].max_candidate_loss")
    objective_splits = {row["split"] for row in objectives}
    if objective_splits != {"fit", "check", "audit"}:
        raise FitError("verify-candidate needs fit, check, and audit objectives")
    if any(row["kind"] not in (
            "passive-decay", "passive-decay-shape",
            "checked-note-harmonic-decay",
    ) for row in objectives):
        raise FitError(
            "verify-candidate only supports checked passive-decay objectives"
        )

    if type(candidate) is not dict:
        raise FitError("fit manifest needs a candidate")
    exact_keys(candidate, {
        "parameters", "expected_objective_losses", "profile_changes",
        "profile_adapter_sha256",
    }, "candidate")
    digest(candidate.get("profile_adapter_sha256"),
           "candidate.profile_adapter_sha256")
    candidate_parameters = candidate.get("parameters")
    if type(candidate_parameters) is not dict:
        raise FitError("candidate.parameters must be an object")
    manifest_parameters = by_name(parameters, "id")
    if set(candidate_parameters) != set(manifest_parameters):
        raise FitError("candidate parameter set differs from the manifest")
    differs = False
    for name, row in manifest_parameters.items():
        value = finite(candidate_parameters[name], f"candidate parameter {name}")
        if value < float(row["minimum"]) or value > float(row["maximum"]):
            raise FitError(f"candidate parameter is out of range: {name}")
        differs = differs or value != float(row["baseline"])
    if not differs:
        raise FitError("candidate parameters equal the baseline")

    expected_losses = candidate.get("expected_objective_losses")
    if type(expected_losses) is not dict:
        raise FitError("candidate.expected_objective_losses must be an object")
    expected_ids = {
        row["id"] for row in objectives if row["split"] in ("fit", "check")
    }
    if set(expected_losses) != expected_ids:
        raise FitError(
            "candidate expected losses must name every fit and check objective"
        )
    for name in sorted(expected_losses):
        token(name, "expected objective id")
        nonnegative(expected_losses[name], f"expected loss {name}")
    if not expected_ids.issubset(objective_ids):
        raise FitError("candidate expected loss has an unknown objective")

    changes = candidate.get("profile_changes")
    cello_changes = {
        4: [
            ("loss_time_constant_c_seconds",
             ["strings", 0, "loss_time_constant_seconds"]),
            ("loss_time_constant_g_seconds",
             ["strings", 1, "loss_time_constant_seconds"]),
            ("loss_time_constant_d_seconds",
             ["strings", 2, "loss_time_constant_seconds"]),
            ("loss_time_constant_a_seconds",
             ["strings", 3, "loss_time_constant_seconds"]),
        ],
        7: [
            ("loss_time_constant_c_seconds",
             ["strings", 0, "loss_time_constant_seconds"]),
            ("bridge_cutoff_g_hz", ["strings", 1, "bridge_cutoff_hz"]),
            ("bridge_loss_peak_bandwidth_g_hz",
             ["strings", 1, "bridge_loss_peak_bandwidth_hz"]),
            ("bridge_loss_peak_g_fraction",
             ["strings", 1, "bridge_loss_peak_fraction"]),
            ("loss_time_constant_g_seconds",
             ["strings", 1, "loss_time_constant_seconds"]),
            ("loss_time_constant_d_seconds",
             ["strings", 2, "loss_time_constant_seconds"]),
            ("loss_time_constant_a_seconds",
             ["strings", 3, "loss_time_constant_seconds"]),
        ],
    }
    double_bass_changes = [
        ("string_e_loss_seconds",
         ["strings", 0, "loss_time_constant_seconds"]),
        ("string_a_loss_seconds",
         ["strings", 1, "loss_time_constant_seconds"]),
        ("string_d_bridge_cutoff_hz", ["strings", 2, "bridge_cutoff_hz"]),
        ("string_d_loss_seconds",
         ["strings", 2, "loss_time_constant_seconds"]),
        ("string_g_loss_seconds",
         ["strings", 3, "loss_time_constant_seconds"]),
    ]
    if manifest["adapter_id"] in {
            "hlolli_wg_double_bass-passive-joint-validation-v1",
            "hlolli_wg_double_bass-passive-joint-validation-v2",
    }:
        expected_rows = double_bass_changes
    elif type(changes) is list:
        expected_rows = cello_changes.get(len(changes), [])
    else:
        expected_rows = []
    if type(changes) is not list or len(changes) != len(expected_rows):
        raise FitError(
            "candidate.profile_changes must contain one checked change set"
        )
    seen_paths: set[str] = set()
    seen_change_parameters: set[str] = set()
    sources_by_string: dict[int, str] = {}
    for index, row in enumerate(changes):
        if type(row) is not dict:
            raise FitError(f"candidate.profile_changes[{index}] must be an object")
        exact_keys(row, {
            "parameter", "path", "before", "after", "minimum", "maximum",
            "unit", "source_fit_result_sha256",
        }, f"candidate.profile_changes[{index}]")
        change_parameter = token(
            row.get("parameter"),
            f"candidate.profile_changes[{index}].parameter",
        )
        expected_parameter, expected_path = expected_rows[index]
        if (change_parameter != expected_parameter or
                change_parameter in seen_change_parameters):
            raise FitError("candidate profile changes have the wrong string order")
        seen_change_parameters.add(change_parameter)
        token(row.get("unit"), f"candidate.profile_changes[{index}].unit")
        path = validate_profile_path(
            row.get("path"), f"candidate.profile_changes[{index}].path"
        )
        if path != expected_path:
            raise FitError("candidate profile changes have the wrong string paths")
        path_key = json.dumps(path, separators=(",", ":"), ensure_ascii=True)
        if path_key in seen_paths:
            raise FitError("candidate.profile_changes has a duplicate path")
        seen_paths.add(path_key)
        minimum = finite(row.get("minimum"),
                         f"candidate.profile_changes[{index}].minimum")
        maximum = finite(row.get("maximum"),
                         f"candidate.profile_changes[{index}].maximum")
        before = finite(row.get("before"),
                        f"candidate.profile_changes[{index}].before")
        after = finite(row.get("after"),
                       f"candidate.profile_changes[{index}].after")
        if minimum >= maximum:
            raise FitError("candidate profile change has an empty range")
        if not minimum <= before <= maximum or not minimum <= after <= maximum:
            raise FitError("candidate profile change is out of range")
        if before == after:
            raise FitError("candidate profile change does not change its target")
        source_digest = digest(
            row.get("source_fit_result_sha256"),
            f"candidate.profile_changes[{index}].source_fit_result_sha256",
        )
        string_index = int(path[1])
        prior_source = sources_by_string.get(string_index)
        if prior_source is not None and prior_source != source_digest:
            raise FitError("one string reuses different fit results")
        if (prior_source is None and
                source_digest in sources_by_string.values()):
            raise FitError("candidate profile changes reuse a fit result")
        sources_by_string[string_index] = source_digest
    if set(sources_by_string) != {0, 1, 2, 3}:
        raise FitError("candidate profile changes do not cover four strings")


def v1_objective_passes_absolute_limits(
        evidence: dict[str, Any], selection: dict[str, Any]) -> bool:
    """Check optional scalar limits shared with the joint candidate gate."""
    if ("max_candidate_loss" in selection and
            evidence["loss"] > float(selection["max_candidate_loss"])):
        return False
    if ("max_candidate_level_rmse_db" in selection and
            "spectral_change_rmse_db" in evidence):
        if (float(evidence["shape_rmse_db"]) >
                float(selection["max_candidate_level_rmse_db"]) or
                float(evidence["spectral_change_rmse_db"]) >
                float(selection["max_candidate_spectral_rmse_db"])):
            return False
    if "mean_absolute_t60_error_octaves" in evidence:
        if (int(evidence["valid_harmonic_count"]) <
                MIN_HARMONIC_VALID_BANDS or
                float(evidence["mean_absolute_t60_error_octaves"]) >
                float(selection[
                    "max_candidate_harmonic_mean_error_octaves"]) or
                float(evidence["maximum_absolute_t60_error_octaves"]) >
                float(selection[
                    "max_candidate_harmonic_maximum_error_octaves"])):
            return False
    if ("reference_t60_seconds" not in evidence or
            "minimum_candidate_t60_ratio" not in selection):
        return True
    reference_t60 = float(evidence["reference_t60_seconds"])
    reference_support = float(evidence["reference_support_seconds"])
    if reference_t60 <= 0.0 or reference_support <= 0.0:
        raise FitError("passive-decay evidence has invalid reference support")
    t60_ratio = float(evidence["model_t60_seconds"]) / reference_t60
    support_ratio = (
        float(evidence["model_support_seconds"]) / reference_support
    )
    return bool(
        t60_ratio >= float(selection["minimum_candidate_t60_ratio"]) and
        t60_ratio <= float(selection["maximum_candidate_t60_ratio"]) and
        support_ratio >= float(selection["minimum_candidate_support_ratio"])
    )


def v1_rank_key(
        row: dict[str, Any], check_weight: float
        ) -> tuple[float, float, int]:
    """Rank v1 candidates without a hidden validation tie-break."""
    source_groups = row.get("source_groups")
    if type(source_groups) is list and source_groups:
        maximum_source_loss = max(
            float(item["loss"]) for item in source_groups
        )
        return float(row["score"]), maximum_source_loss, int(row["point_id"])
    return (
        float(row["score"]),
        float(row["check_loss"]) if check_weight > 0.0 else 0.0,
        int(row["point_id"]),
    )


def v1_source_group_rows(
        objectives: list[dict[str, Any]], evidence: list[dict[str, Any]]
        ) -> list[dict[str, Any]]:
    """Return source-balanced group means for a v1 corpus manifest."""
    objectives_by_id = by_name(objectives, "id")
    totals: dict[tuple[str, str], list[float]] = {}
    grouped = False
    for item in evidence:
        objective = objectives_by_id[item["objective"]]
        source_group = objective.get("source_group")
        if source_group is None:
            continue
        grouped = True
        key = (objective["split"], source_group)
        total = totals.setdefault(key, [0.0, 0.0])
        weight = float(objective["weight"])
        total[0] += weight * float(item["loss"])
        total[1] += weight
    if not grouped:
        return []
    if len(totals) == 0 or any(weight <= 0.0 for unused, weight in totals.values()):
        raise FitError("source group has no objective weight")
    return [{
        "split": split,
        "source_group": source_group,
        "loss": total / weight,
    } for (split, source_group), (total, weight) in sorted(totals.items())]


def by_name(rows: list[dict[str, Any]], field: str) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for row in rows:
        name = row.get(field)
        if type(name) is not str or name in result:
            raise FitError(f"invalid or duplicate {field}")
        result[name] = row
    return result


def experiment_bundle(path: Path, source: Optional[bytes] = None
                      ) -> tuple[dict[str, Any], Path]:
    path = regular(path, "experiment result")
    if source is None:
        path, source, _ = file_snapshot(path, "experiment result")
    prefix = source[:32]
    if prefix.startswith(b"HWA_EXPERIMENT,1\r\n"):
        value = load_saved_experiment(path, source)
    else:
        value = parse_json(source, path)
    if value.get("command") != "experiment" or value.get("schema_version") != 10:
        raise FitError("unsupported experiment result")
    for field in ("parameters", "cases", "responses", "points", "values",
                  "jobs", "artifacts", "candidates"):
        if type(value.get(field)) is not list:
            raise FitError(f"experiment result has invalid {field}")
    return value, path.parent.absolute()


def load_saved_experiment(path: Path, source: Optional[bytes] = None
                          ) -> dict[str, Any]:
    groups: dict[str, list[list[str]]] = {}
    meta: dict[str, str] = {}
    try:
        if source is None:
            path, source, _ = file_snapshot(path, "saved experiment")
        text = source.decode("ascii")
        rows = list(csv.reader(io.StringIO(text, newline=""), strict=True))
    except (OSError, UnicodeError, csv.Error) as error:
        raise FitError(f"cannot read saved experiment: {path}: {error}") from error
    if not rows or rows[0] != ["HWA_EXPERIMENT", "1"]:
        raise FitError("saved experiment has an invalid header")
    for row in rows[1:]:
        if not row:
            raise FitError("saved experiment has an empty row")
        if row[0] == "META":
            if len(row) != 4 or row[1] in meta:
                raise FitError("saved experiment has invalid metadata")
            meta[row[1]] = row[2]
        else:
            groups.setdefault(row[0], []).append(row)

    def integer(text: str, field: str) -> int:
        if not text.isdigit():
            raise FitError(f"saved experiment has invalid {field}")
        return int(text)

    def number(text: str, field: str) -> float:
        try:
            value = float(text)
        except ValueError as error:
            raise FitError(f"saved experiment has invalid {field}") from error
        if not math.isfinite(value):
            raise FitError(f"saved experiment has nonfinite {field}")
        return value

    def records(name: str, width: int) -> list[list[str]]:
        result = groups.get(name, [])
        if any(len(row) != width for row in result):
            raise FitError(f"saved experiment has invalid {name} row")
        for index, row in enumerate(result, 1):
            if integer(row[1], f"{name} id") != index:
                raise FitError(f"saved experiment has non-dense {name} ids")
        declared = meta.get(name.lower() + "_count")
        if declared is None or integer(declared, name + " count") != len(result):
            raise FitError(f"saved experiment has a wrong {name} count")
        return result

    parameters = [{
        "id": integer(row[1], "parameter id"), "name": row[2],
        "unit": row[3], "minimum": number(row[4], "parameter minimum"),
        "maximum": number(row[5], "parameter maximum"),
        "baseline": number(row[6], "parameter baseline"),
    } for row in records("PARAMETER", 9)]
    cases = [{
        "id": integer(row[1], "case id"), "name": row[2],
        "split": row[3], "weight": number(row[4], "case weight"),
    } for row in records("CASE", 5)]
    responses = [{
        "id": integer(row[1], "response id"), "name": row[2],
        "role": row[3], "feature": row[4],
        "feature_index": integer(row[5], "feature index"),
    } for row in records("RESPONSE", 6)]
    points = [{
        "id": integer(row[1], "point id"), "key": row[2],
        "baseline": row[3] == "1",
    } for row in records("POINT", 4)]
    if any(row[3] not in ("0", "1") for row in records("POINT", 4)):
        raise FitError("saved experiment has invalid baseline flags")
    values = [{
        "id": integer(row[1], "value id"),
        "point_id": integer(row[2], "value point id"),
        "parameter_id": integer(row[3], "value parameter id"),
        "value": number(row[4], "parameter value"),
    } for row in records("VALUE", 5)]
    jobs = [{
        "id": integer(row[1], "job id"), "key": row[2],
        "point_id": integer(row[3], "job point id"),
        "case_id": integer(row[4], "job case id"),
    } for row in records("JOB", 12)]
    artifacts = []
    for row in records("ARTIFACT", 8):
        try:
            encoded = bytes.fromhex(row[4])
            artifact_name = encoded.decode("ascii")
        except (ValueError, UnicodeError) as error:
            raise FitError("saved experiment has invalid artifact path bytes") from error
        artifacts.append({
            "id": integer(row[1], "artifact id"),
            "job_id": integer(row[2], "artifact job id"),
            "resource_id": row[3],
            "artifact": {"path": artifact_name, "sha256": row[5]},
            "file_bytes": integer(row[6], "artifact bytes"), "kind": row[7],
        })
    candidates = []
    for row in records("CANDIDATE", 12):
        valid = row[11] == "1"
        if row[11] not in ("0", "1"):
            raise FitError("saved experiment has invalid candidate flag")
        candidates.append({
            "id": integer(row[1], "candidate id"),
            "point_id": integer(row[2], "candidate point id"),
            "response_id": integer(row[3], "candidate response id"),
            "split": row[4], "availability": row[5],
            "mean_gap": number(row[6], "candidate mean gap") if valid else None,
            "improvement": number(row[7], "candidate improvement") if valid else None,
            "worst_harm": number(row[8], "candidate worst harm") if valid else None,
            "values_valid": valid,
        })
    return {
        "command": "experiment", "schema_version": 10,
        "parameters": parameters, "cases": cases, "responses": responses,
        "points": points, "values": values, "jobs": jobs,
        "artifacts": artifacts, "candidates": candidates,
    }


def artifact_path(root: Path, row: dict[str, Any]) -> Path:
    artifact = row.get("artifact")
    if type(artifact) is not dict or type(artifact.get("path")) is not str:
        raise FitError("experiment artifact has no path")
    relative = Path(artifact["path"])
    if (relative.is_absolute() or not relative.parts or
            any(part in (".", "..") for part in relative.parts)):
        raise FitError("experiment artifact path escapes its bundle")
    root = root.resolve()
    path = root
    for part in relative.parts:
        path = path / part
        if path.is_symlink():
            raise FitError("experiment artifact path uses a symlink")
    path = path.resolve()
    try:
        path.relative_to(root)
    except ValueError as error:
        raise FitError("experiment artifact path escapes its bundle") from error
    path = regular(path, "experiment artifact")
    if sha256(path) != artifact.get("sha256"):
        raise FitError(f"experiment artifact hash changed: {path}")
    return path


def run_body_envelope(analyzer: Path, reference: Path, model: Path) -> dict[str, Any]:
    completed = subprocess.run(
        [str(analyzer), "--json", "body-envelope", str(reference), str(model)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env={"LC_ALL": "C", "LANG": "C", "TZ": "UTC"},
    )
    if completed.returncode != 0:
        raise FitError(
            "body-envelope failed: " + completed.stderr.strip()
        )
    try:
        result = json.loads(completed.stdout, object_pairs_hook=object_pairs)
    except (json.JSONDecodeError, FitError) as error:
        raise FitError("body-envelope returned invalid JSON") from error
    comparison = result.get("comparison")
    if type(comparison) is not dict or comparison.get("valid") is not True:
        raise FitError("body-envelope comparison has too little support")
    rmse = finite(comparison.get("shape_rmse_db"), "shape_rmse_db")
    correlation = finite(comparison.get("shape_correlation"), "shape_correlation")
    confidence = finite(comparison.get("confidence"), "body confidence")
    return {"shape_rmse_db": rmse, "shape_correlation": correlation,
            "confidence": confidence}


def run_checked_analyzer_json(
        analyzer: Path, arguments: list[str], label: str) -> dict[str, Any]:
    """Run one bounded analyzer command and parse its duplicate-free JSON."""
    try:
        completed = subprocess.run(
            [str(analyzer), "--json", *arguments], check=False,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            env={"LC_ALL": "C", "LANG": "C", "TZ": "UTC"}, timeout=120,
        )
    except subprocess.TimeoutExpired as error:
        raise FitError(f"{label} timed out") from error
    except OSError as error:
        raise FitError(f"cannot run {label}: {error}") from error
    if completed.returncode != 0:
        detail = (completed.stderr.strip() or completed.stdout.strip())[-2000:]
        raise FitError(
            f"{label} failed" + (f": {detail}" if detail else "")
        )
    source = completed.stdout.encode("utf-8")
    if len(source) > MAX_CONTROL_FILE_BYTES:
        raise FitError(f"{label} output exceeds the byte limit")
    return parse_json(source, analyzer)


def checked_note_report(
        analyzer: Path, audio: Path, expected_hz: float,
        expected_sha256: str, field: str) -> dict[str, Any]:
    """Require the analyzer's fixed isolated-note pitch contract."""
    audio = regular(audio, field)
    if sha256(audio) != expected_sha256:
        raise FitError(f"{field} hash changed")
    report = run_checked_analyzer_json(
        analyzer,
        ["isolated-note", str(audio), "--expected-hz", repr(expected_hz),
         "--metrics", "pitch"],
        field + " checked note",
    )
    pitch = report.get("pitch")
    if (report.get("schema") != "hwa-isolated-note" or
            report.get("schema_version") != 1 or
            report.get("command") != "isolated-note" or
            report.get("method") != CHECKED_NOTE_METHOD_VERSION or
            report.get("path") != str(audio) or
            finite(report.get("expected_hz"), field + " expected_hz") !=
            expected_hz or
            report.get("requested_metrics") != ["pitch"] or
            type(pitch) is not dict or type(pitch.get("valid")) is not bool):
        raise FitError(f"{field} checked note returned an unknown contract")
    if sha256(audio) != expected_sha256:
        raise FitError(f"{field} changed during checked note analysis")
    return report


def checked_note_harmonic_decay(
        analyzer: Path, reference: Path, model: Path, expected_hz: float,
        analyzer_sha256: str, reference_sha256: str,
        model_sha256: str) -> dict[str, Any]:
    """Score native checked pitch and per-harmonic T60 evidence."""
    analyzer = regular(analyzer, "analyzer")
    reference = regular(reference, "checked harmonic reference")
    model = regular(model, "checked harmonic model")
    if sha256(analyzer) != analyzer_sha256:
        raise FitError("checked harmonic analyzer hash changed")
    if sha256(reference) != reference_sha256:
        raise FitError("checked harmonic reference hash changed")
    if sha256(model) != model_sha256:
        raise FitError("checked harmonic model hash changed")
    reference_note = checked_note_report(
        analyzer, reference, expected_hz, reference_sha256,
        "checked harmonic reference",
    )
    if reference_note["pitch"]["valid"] is not True:
        raise FitError("checked harmonic reference failed the note pitch gate")
    model_note = checked_note_report(
        analyzer, model, expected_hz, model_sha256,
        "checked harmonic model",
    )
    report = run_checked_analyzer_json(
        analyzer,
        ["harmonic-decay", str(reference), str(model),
         "--expected-hz", repr(expected_hz)],
        "checked harmonic-decay",
    )
    reference_profile = report.get("reference")
    model_profile = report.get("model")
    comparison = report.get("comparison")
    if (report.get("schema") != "hwa-harmonic-decay" or
            report.get("schema_version") != 1 or
            report.get("command") != "harmonic-decay" or
            report.get("method") != CHECKED_HARMONIC_DECAY_METHOD_VERSION or
            finite(report.get("expected_hz"), "harmonic expected_hz") !=
            expected_hz or
            type(reference_profile) is not dict or
            reference_profile.get("path") != str(reference) or
            type(reference_profile.get("valid")) is not bool or
            type(model_profile) is not dict or
            model_profile.get("path") != str(model) or
            type(model_profile.get("valid")) is not bool or
            type(comparison) is not dict or
            type(comparison.get("valid")) is not bool or
            type(comparison.get("bands")) is not list):
        raise FitError("checked harmonic-decay returned an unknown contract")
    if reference_profile["valid"] is not True:
        raise FitError("checked harmonic reference has no valid profile")

    errors = []
    for index, row in enumerate(comparison["bands"]):
        if type(row) is not dict or type(row.get("valid")) is not bool:
            raise FitError("checked harmonic-decay has an invalid band row")
        if row["valid"]:
            errors.append(abs(finite(
                row.get("t60_log_error_db"),
                f"checked harmonic-decay bands[{index}].t60_log_error_db",
            )) / T60_DB_PER_OCTAVE)
    shared_count = comparison.get("shared_valid_band_count")
    if (type(shared_count) is not int or type(shared_count) is bool or
            shared_count < 0 or shared_count != len(errors)):
        raise FitError("checked harmonic-decay shared band count changed")
    coverage = finite(
        comparison.get("shared_reference_coverage"),
        "checked harmonic-decay reference coverage",
    )
    comparison_valid = bool(
        model_note["pitch"]["valid"] and model_profile["valid"] and
        comparison["valid"] and
        shared_count >= MIN_HARMONIC_VALID_BANDS
    )
    if comparison_valid:
        rms_error = nonnegative(
            comparison.get("t60_log_rmse_db"),
            "checked harmonic-decay T60 RMSE",
        ) / T60_DB_PER_OCTAVE
        median_bias = finite(
            comparison.get("median_t60_log_bias_db"),
            "checked harmonic-decay median bias",
        ) / T60_DB_PER_OCTAVE
        mean_error = sum(errors) / len(errors)
        maximum_error = max(errors)
    else:
        rms_error = INVALID_HARMONIC_LOSS_OCTAVES
        median_bias = 0.0
        mean_error = INVALID_HARMONIC_LOSS_OCTAVES
        maximum_error = INVALID_HARMONIC_LOSS_OCTAVES
    if sha256(analyzer) != analyzer_sha256:
        raise FitError("checked harmonic analyzer changed during analysis")
    if (sha256(reference) != reference_sha256 or
            sha256(model) != model_sha256):
        raise FitError("checked harmonic-decay audio changed during analysis")
    return {
        "checked_note_valid": model_note["pitch"]["valid"],
        "checked_harmonic_decay_valid": comparison_valid,
        "reference_pitch_error_cents": finite(
            reference_note["pitch"].get("cents"),
            "checked harmonic reference pitch cents",
        ),
        "model_pitch_error_cents": finite(
            model_note["pitch"].get("cents"),
            "checked harmonic model pitch cents",
        ),
        "valid_harmonic_count": shared_count,
        "shared_reference_coverage": coverage,
        "rms_t60_error_octaves": rms_error,
        "mean_absolute_t60_error_octaves": mean_error,
        "maximum_absolute_t60_error_octaves": maximum_error,
        "median_t60_bias_octaves": median_bias,
    }


def read_pcm_wave_payload(
        path: Path, expected_sha256: Optional[str] = None
        ) -> tuple[int, int, int, bytes]:
    """Read and check one 16-bit or 24-bit PCM WAVE payload."""
    path = regular(path, "passive-decay audio")
    try:
        if path.stat().st_size > MAX_PASSIVE_WAVE_BYTES:
            raise FitError("passive-decay audio exceeds the byte limit")
        with path.open("rb") as stream:
            source = stream.read(MAX_PASSIVE_WAVE_BYTES + 1)
    except OSError as error:
        raise FitError(f"cannot read PCM WAVE: {path}: {error}") from error
    if len(source) > MAX_PASSIVE_WAVE_BYTES:
        raise FitError("passive-decay audio exceeds the byte limit")
    if (expected_sha256 is not None and
            hashlib.sha256(source).hexdigest() != expected_sha256):
        raise FitError("passive-decay audio hash changed")
    if len(source) < 12 or source[:4] != b"RIFF" or source[8:12] != b"WAVE":
        raise FitError("passive-decay audio is not a RIFF WAVE file")
    declared = int.from_bytes(source[4:8], "little") + 8
    if declared != len(source):
        raise FitError("passive-decay RIFF size does not match the file")
    chunks: dict[bytes, bytes] = {}
    offset = 12
    while offset + 8 <= declared:
        name = source[offset:offset + 4]
        size = int.from_bytes(source[offset + 4:offset + 8], "little")
        start = offset + 8
        end = start + size
        if end > declared:
            raise FitError("passive-decay WAVE chunk is truncated")
        if name not in chunks:
            chunks[name] = source[start:end]
        offset = end + (size & 1)
    format_chunk = chunks.get(b"fmt ")
    raw = chunks.get(b"data")
    if format_chunk is None or raw is None or len(format_chunk) < 16:
        raise FitError("passive-decay WAVE lacks format or audio data")
    format_tag = int.from_bytes(format_chunk[0:2], "little")
    channels = int.from_bytes(format_chunk[2:4], "little")
    rate = int.from_bytes(format_chunk[4:8], "little")
    frame_bytes = int.from_bytes(format_chunk[12:14], "little")
    bits = int.from_bytes(format_chunk[14:16], "little")
    pcm_guid = bytes.fromhex("0100000000001000800000aa00389b71")
    if format_tag == 0xfffe:
        if (len(format_chunk) < 40 or
                int.from_bytes(format_chunk[16:18], "little") < 22 or
                format_chunk[24:40] != pcm_guid):
            raise FitError("passive-decay WAVE extensible data is not PCM")
    elif format_tag != 1:
        raise FitError("passive-decay WAVE data is not integer PCM")
    if (bits not in (16, 24) or channels < 1 or
            channels > MAX_PASSIVE_CHANNELS or rate < 8000 or
            rate > MAX_PASSIVE_RATE_HZ):
        raise FitError("passive-decay needs 16-bit or 24-bit PCM WAVE audio")
    width = bits // 8
    if frame_bytes != channels * width or not raw or len(raw) % frame_bytes:
        raise FitError("passive-decay audio is truncated")
    count = len(raw) // frame_bytes
    if count > MAX_PASSIVE_FRAMES:
        raise FitError("passive-decay audio exceeds the frame limit")
    return rate, channels, width, raw


def read_pcm_wave_channels(
        path: Path, expected_sha256: Optional[str] = None
        ) -> tuple[int, list[list[float]]]:
    """Read checked PCM WAVE samples by channel."""
    rate, channels, width, raw = read_pcm_wave_payload(
        path, expected_sha256
    )
    scale = float(1 << (width * 8 - 1))
    samples: list[list[float]] = [[] for _ in range(channels)]
    offset = 0
    count = len(raw) // (channels * width)
    for _ in range(count):
        for channel in range(channels):
            sample = int.from_bytes(raw[offset:offset + width], "little", signed=True)
            offset += width
            samples[channel].append(sample / scale)
    return rate, samples


def read_pcm_wave(path: Path, expected_sha256: Optional[str] = None
                  ) -> tuple[int, list[float], list[float]]:
    """Read PCM WAVE frame energy and first-difference energy."""
    rate, channels, width, raw = read_pcm_wave_payload(
        path, expected_sha256
    )
    scale = float(1 << (width * 8 - 1))
    energies: list[float] = []
    differences: list[float] = []
    previous = [0.0] * channels
    offset = 0
    count = len(raw) // (channels * width)
    for _ in range(count):
        total = 0.0
        difference_total = 0.0
        for channel in range(channels):
            sample = int.from_bytes(
                raw[offset:offset + width], "little", signed=True
            )
            offset += width
            value = sample / scale
            total += value * value
            difference = value - previous[channel]
            difference_total += difference * difference
            previous[channel] = value
        energies.append(total / channels)
        differences.append(difference_total / channels)
    return rate, energies, differences


def goertzel_level_dbfs(samples: list[list[float]], start: int, count: int,
                        frequency_hz: float, rate_hz: int) -> float:
    coefficient = 2.0 * math.cos(2.0 * math.pi * frequency_hz / rate_hz)
    power = 0.0
    for channel in samples:
        previous = 0.0
        older = 0.0
        for sample in channel[start:start + count]:
            current = sample + coefficient * previous - older
            older = previous
            previous = current
        channel_power = max(
            0.0, previous * previous + older * older -
            coefficient * previous * older,
        )
        power += channel_power
    amplitude = 2.0 * math.sqrt(power / len(samples)) / count
    return 20.0 * math.log10(max(amplitude, 1.0e-12))


def harmonic_decay_profile(
        path: Path, fundamental_hz: float, harmonic_count: int,
        expected_sha256: Optional[str] = None) -> list[dict[str, Any]]:
    fundamental = positive(fundamental_hz, "harmonic fundamental")
    if (type(harmonic_count) is not int or type(harmonic_count) is bool or
            harmonic_count < MIN_HARMONIC_VALID_BANDS or
            harmonic_count > MAX_HARMONIC_BANDS):
        raise FitError("harmonic count is out of range")
    rate, samples = read_pcm_wave_channels(path, expected_sha256)
    tail = decay_tail(path, expected_sha256)
    window = max(8, round(rate * HARMONIC_WINDOW_SECONDS))
    hop = max(1, round(rate * HARMONIC_HOP_SECONDS))
    start = round(
        (tail["tail_start_seconds"] + HARMONIC_LATE_START_SECONDS) * rate
    )
    if start + window > len(samples[0]):
        raise FitError("harmonic-decay tail is too short")
    rows = []
    for harmonic in range(1, harmonic_count + 1):
        center = fundamental * harmonic
        if center + HARMONIC_BAND_HALF_WIDTH_HZ >= 0.45 * rate:
            raise FitError("harmonic band exceeds the checked audio range")
        levels = []
        times = []
        for offset in range(start, len(samples[0]) - window + 1, hop):
            level = max(
                goertzel_level_dbfs(
                    samples, offset, window, center + frequency_offset, rate
                )
                for frequency_offset in HARMONIC_BAND_OFFSETS_HZ
                if center + frequency_offset > 0.0
            )
            levels.append(level)
            times.append((offset - start + 0.5 * window) / rate)
        peak = max(levels)
        first_peak = levels.index(peak)
        stop = len(levels)
        for index in range(first_peak + 1, len(levels)):
            if (levels[index] <= peak - HARMONIC_FIT_RANGE_DB or
                    levels[index] <= MIN_HARMONIC_LEVEL_DBFS):
                stop = index + 1
                break
        fit_times = times[first_peak:stop]
        fit_levels = levels[first_peak:stop]
        status = "valid"
        slope = None
        residual = None
        t60 = None
        support = 0.0
        if len(fit_times) < 4:
            status = "too-short"
        else:
            support = fit_times[-1] - fit_times[0]
            if support < MIN_HARMONIC_SUPPORT_SECONDS:
                status = "too-short"
            else:
                relative_times = [time - fit_times[0] for time in fit_times]
                slope, residual = linear_facts(relative_times, fit_levels)
                if slope >= -3.0:
                    status = "not-decaying"
                elif residual > MAX_HARMONIC_LINE_RESIDUAL_DB:
                    status = "too-irregular"
                else:
                    t60 = -60.0 / slope
        rows.append({
            "harmonic": harmonic,
            "center_hz": center,
            "status": status,
            "peak_dbfs": peak,
            "support_seconds": support,
            "slope_db_per_second": slope,
            "line_residual_db": residual,
            "t60_seconds": t60,
        })
    return rows


def compare_harmonic_decay_profiles(
        reference_rows: list[dict[str, Any]],
        model_rows: list[dict[str, Any]], fundamental_hz: float,
        harmonic_count: int) -> dict[str, Any]:
    """Compare two checked harmonic profiles."""
    reference_valid_count = sum(
        row["status"] == "valid" for row in reference_rows
    )
    if reference_valid_count < MIN_HARMONIC_VALID_BANDS:
        raise FitError("harmonic-decay reference has too few valid bands")
    rows = []
    errors = []
    for reference_row, model_row in zip(reference_rows, model_rows):
        status = "valid"
        error = None
        if reference_row["status"] != "valid":
            status = "invalid-reference"
        elif model_row["status"] != "valid":
            status = "invalid-model"
        else:
            error = abs(math.log2(
                float(model_row["t60_seconds"]) /
                float(reference_row["t60_seconds"])
            ))
            errors.append(error)
        rows.append({
            "harmonic": reference_row["harmonic"],
            "center_hz": reference_row["center_hz"],
            "status": status,
            "reference": reference_row,
            "model": model_row,
            "absolute_t60_error_octaves": error,
        })
    mean_error = sum(errors) / len(errors) if errors else 8.0
    maximum_error = max(errors) if errors else 8.0
    return {
        "method_version": HARMONIC_DECAY_METHOD_VERSION,
        "fundamental_hz": float(fundamental_hz),
        "harmonic_count": harmonic_count,
        "harmonics": rows,
        "comparison": {
            "valid_harmonic_count": len(errors),
            "mean_absolute_t60_error_octaves": mean_error,
            "maximum_absolute_t60_error_octaves": maximum_error,
        },
    }


def run_harmonic_decay(
        reference: Path, model: Path, fundamental_hz: float,
        harmonic_count: int = 8,
        reference_sha256: Optional[str] = None,
        model_sha256: Optional[str] = None) -> dict[str, Any]:
    """Compare per-harmonic T60 values after one plucked-string attack."""
    reference_rows = harmonic_decay_profile(
        reference, fundamental_hz, harmonic_count, reference_sha256
    )
    model_rows = harmonic_decay_profile(
        model, fundamental_hz, harmonic_count, model_sha256
    )
    return compare_harmonic_decay_profiles(
        reference_rows, model_rows, fundamental_hz, harmonic_count
    )


def linear_facts(times: list[float], values: list[float]) -> tuple[float, float]:
    mean_time = sum(times) / len(times)
    mean_value = sum(values) / len(values)
    variance = sum((time - mean_time) ** 2 for time in times)
    if variance <= 0.0:
        raise FitError("passive-decay tail has too little time support")
    slope = sum((time - mean_time) * (value - mean_value)
                for time, value in zip(times, values)) / variance
    intercept = mean_value - slope * mean_time
    residual = math.sqrt(sum(
        (value - (intercept + slope * time)) ** 2
        for time, value in zip(times, values)
    ) / len(times))
    return slope, residual


def decay_tail(path: Path, expected_sha256: Optional[str] = None
               ) -> dict[str, Any]:
    """Find and check one post-attack decay curve."""
    rate, energies, differences = read_pcm_wave(path, expected_sha256)
    window = max(1, round(rate * 0.020))
    hop = max(1, round(rate * 0.005))
    if len(energies) < round(rate * 0.35) or len(energies) < window:
        raise FitError("passive-decay tail is too short")
    prefix = [0.0]
    difference_prefix = [0.0]
    for energy in energies:
        prefix.append(prefix[-1] + energy)
    for energy in differences:
        difference_prefix.append(difference_prefix[-1] + energy)
    rms: list[float] = []
    roughness: list[float] = []
    times: list[float] = []
    for start in range(0, len(energies) - window + 1, hop):
        mean_square = (prefix[start + window] - prefix[start]) / window
        difference_mean_square = (
            difference_prefix[start + window] - difference_prefix[start]
        ) / window
        rms.append(math.sqrt(max(0.0, mean_square)))
        roughness.append(10.0 * math.log10(max(
            difference_mean_square / max(mean_square, 1.0e-24),
            1.0e-12,
        )))
        times.append((start + 0.5 * window) / rate)
    peak = max(rms)
    if peak <= 1.0e-7:
        raise FitError("passive-decay tail has too little level")
    peak_db = 20.0 * math.log10(peak)
    levels = [20.0 * math.log10(max(value, 1.0e-12)) for value in rms]
    onset_floor = peak_db - 30.0
    floor_onset = next((index for index, level in enumerate(levels)
                        if level >= onset_floor), None)
    if floor_onset is None:
        raise FitError("passive-decay onset was not found")

    # A level rise must happen quickly or change waveform roughness. This
    # distinguishes an attack from the slower swells caused by modal beating.
    long_rise_span = max(2, round(0.080 * rate / hop))
    fast_rise_span = max(2, round(0.010 * rate / hop))
    rise_candidates: list[int] = []
    for index in range(long_rise_span, len(levels)):
        recent_low = min(levels[index - long_rise_span:index])
        recent_roughness = sorted(
            roughness[index - long_rise_span:index]
        )[long_rise_span // 2]
        fast_low = min(levels[index - fast_rise_span:index])
        if (levels[index] - recent_low > 8.0 and
                levels[index] > onset_floor and
                (levels[index] - fast_low > 8.0 or
                 roughness[index] - recent_roughness > 6.0)):
            rise_candidates.append(index)

    event_span = max(2, round(0.100 * rate / hop))
    events: list[int] = []
    for candidate in rise_candidates:
        if not events or candidate - events[-1] >= event_span:
            events.append(candidate)
    if events:
        onset = events[0]
        first_end = min(
            len(levels), onset + max(2, round(0.040 * rate / hop))
        )
    else:
        onset = floor_onset
        first_end = min(len(levels), onset + event_span)
    first_peak = max(range(onset, first_end), key=lambda index: levels[index])
    if any(event - first_peak >= event_span for event in events[1:]):
        raise FitError("passive-decay tail has a second onset")

    start = first_peak + max(1, round(0.040 * rate / hop))
    if start + 4 >= len(levels):
        raise FitError("passive-decay tail is too short after its attack")
    initial = sum(levels[start:start + 3]) / 3.0
    stop = len(levels) - max(1, round(0.025 * rate / hop))
    for index in range(start + 1, stop):
        if levels[index] <= initial - 35.0:
            stop = index + 1
            break
    tail_times = [time - times[start] for time in times[start:stop]]
    tail_levels = levels[start:stop]
    if len(tail_times) < 20 or tail_times[-1] < 0.20:
        raise FitError("passive-decay tail has too little time support")
    dynamic_range = initial - min(tail_levels)
    if dynamic_range < MIN_PASSIVE_DYNAMIC_RANGE_DB:
        raise FitError(
            "passive-decay tail is noisy or has too little dynamic range"
        )
    slope, residual = linear_facts(tail_times, tail_levels)
    if slope >= -3.0:
        raise FitError("passive-decay tail does not decay")
    if residual > MAX_PASSIVE_LINE_RESIDUAL_DB:
        raise FitError("passive-decay tail is too irregular")
    normalized = [level - initial for level in tail_levels]
    tail_spectral_change = roughness[start:stop]
    initial_spectral_change = sum(tail_spectral_change[:3]) / 3.0
    normalized_spectral_change = [
        value - initial_spectral_change for value in tail_spectral_change
    ]
    return {
        "times": tail_times,
        "levels_db": normalized,
        "spectral_change_db": normalized_spectral_change,
        "tail_start_seconds": times[start],
        "slope_db_per_second": slope,
        "t60_seconds": -60.0 / slope,
        "support_seconds": tail_times[-1],
        "dynamic_range_db": dynamic_range,
        "line_residual_db": residual,
        "noise_floor_dbfs": sum(sorted(levels[-max(5, len(levels) // 10):])[:5]) / 5.0,
    }


def interpolate(times: list[float], values: list[float], target: float,
                index: int) -> tuple[float, int]:
    while index + 1 < len(times) and times[index + 1] < target:
        index += 1
    if index + 1 >= len(times):
        return values[-1], index
    width = times[index + 1] - times[index]
    fraction = 0.0 if width <= 0.0 else (target - times[index]) / width
    return values[index] + fraction * (values[index + 1] - values[index]), index


def run_passive_decay(reference: Path, model: Path,
                      reference_sha256: Optional[str] = None,
                      model_sha256: Optional[str] = None) -> dict[str, Any]:
    """Compare passive decay while ignoring gain, polarity, and leading silence."""
    reference_tail = decay_tail(reference, reference_sha256)
    model_tail = decay_tail(model, model_sha256)
    duration = reference_tail["support_seconds"]
    if duration < 0.20:
        raise FitError("passive-decay comparison has too little shared support")
    count = max(20, int(duration / 0.005) + 1)
    reference_index = 0
    model_index = 0
    squared = 0.0
    spectral_squared = 0.0
    for step in range(count):
        target = duration * step / (count - 1)
        reference_level, reference_index = interpolate(
            reference_tail["times"], reference_tail["levels_db"],
            target, reference_index
        )
        model_level, model_index = interpolate(
            model_tail["times"], model_tail["levels_db"], target, model_index
        )
        squared += (reference_level - model_level) ** 2
        reference_spectral, _ = interpolate(
            reference_tail["times"], reference_tail["spectral_change_db"],
            target, reference_index
        )
        model_spectral, _ = interpolate(
            model_tail["times"], model_tail["spectral_change_db"],
            target, model_index
        )
        spectral_squared += (reference_spectral - model_spectral) ** 2
    level_rmse = math.sqrt(squared / count)
    spectral_rmse = math.sqrt(spectral_squared / count)
    return {
        "shape_rmse_db": level_rmse,
        "spectral_change_rmse_db": spectral_rmse,
        "combined_shape_rmse_db": math.hypot(level_rmse, spectral_rmse),
        "reference_slope_db_per_second": reference_tail["slope_db_per_second"],
        "model_slope_db_per_second": model_tail["slope_db_per_second"],
        "slope_delta_db_per_second": (
            model_tail["slope_db_per_second"] -
            reference_tail["slope_db_per_second"]
        ),
        "reference_t60_seconds": reference_tail["t60_seconds"],
        "model_t60_seconds": model_tail["t60_seconds"],
        "support_seconds": duration,
        "comparison_support_seconds": duration,
        "reference_support_seconds": reference_tail["support_seconds"],
        "model_support_seconds": model_tail["support_seconds"],
        "model_support_shortfall_seconds": max(
            0.0,
            reference_tail["support_seconds"] - model_tail["support_seconds"],
        ),
        "reference_dynamic_range_db": reference_tail["dynamic_range_db"],
        "model_dynamic_range_db": model_tail["dynamic_range_db"],
        "reference_line_residual_db": reference_tail["line_residual_db"],
        "model_line_residual_db": model_tail["line_residual_db"],
        "reference_noise_floor_dbfs": reference_tail["noise_floor_dbfs"],
        "model_noise_floor_dbfs": model_tail["noise_floor_dbfs"],
    }


def passive_decay_loss(kind: str, measure: dict[str, Any], scale: float) -> float:
    """Return the named passive objective without changing the v4 score."""
    if kind == "harmonic-decay":
        field = "mean_absolute_t60_error_octaves"
    elif kind == "checked-note-harmonic-decay":
        field = "rms_t60_error_octaves"
    else:
        field = ("combined_shape_rmse_db"
                 if kind == "passive-decay-shape" else "shape_rmse_db")
    return finite(measure[field], field) / scale


def selection_method_version(manifest: dict[str, Any]) -> str:
    if (manifest["schema_version"] == 1 and
            manifest["selection"].get("mode") == "fit-only"):
        return FIT_ONLY_SELECTION_METHOD_VERSION
    return SELECTION_METHOD_VERSION


def passive_method_versions(objectives: list[dict[str, Any]]) -> dict[str, str]:
    kinds = {row["kind"] for row in objectives}
    result: dict[str, str] = {}
    if kinds.intersection({"passive-decay", "passive-decay-shape"}):
        result["passive_decay"] = PASSIVE_DECAY_METHOD_VERSION
    if "passive-decay-shape" in kinds:
        result["passive_decay_shape"] = PASSIVE_DECAY_SHAPE_METHOD_VERSION
    if "harmonic-decay" in kinds:
        result["harmonic_decay"] = HARMONIC_DECAY_METHOD_VERSION
    if "checked-note-harmonic-decay" in kinds:
        result["isolated_note"] = CHECKED_NOTE_METHOD_VERSION
        result["checked_harmonic_decay"] = (
            CHECKED_HARMONIC_DECAY_METHOD_VERSION
        )
    return result


def parameter_values(experiment: dict[str, Any]) -> dict[int, dict[str, float]]:
    parameter_names = {
        int(row["id"]): token(row.get("name"), "experiment parameter name")
        for row in experiment["parameters"]
    }
    result = {int(row["id"]): {} for row in experiment["points"]}
    baselines = {
        int(row["id"]): finite(row.get("baseline"), "parameter baseline")
        for row in experiment["parameters"]
    }
    for point in result.values():
        for parameter_id, value in baselines.items():
            point[parameter_names[parameter_id]] = value
    for row in experiment["values"]:
        point_id = int(row["point_id"])
        parameter_id = int(row["parameter_id"])
        if point_id not in result or parameter_id not in parameter_names:
            raise FitError("experiment value has an unknown id")
        result[point_id][parameter_names[parameter_id]] = finite(
            row.get("value"), "experiment parameter value"
        )
    return result


def verify_candidate_experiment(
        manifest: dict[str, Any], experiment: dict[str, Any]
        ) -> tuple[int, int, dict[int, dict[str, float]]]:
    manifest_parameters = by_name(manifest["parameters"], "id")
    experiment_parameters: dict[str, dict[str, Any]] = {}
    parameter_ids: set[int] = set()
    for index, row in enumerate(experiment["parameters"]):
        if type(row) is not dict:
            raise FitError(f"experiment parameters[{index}] must be an object")
        identifier = row.get("id")
        if type(identifier) is not int or type(identifier) is bool or identifier < 1:
            raise FitError("experiment parameter has an invalid id")
        if identifier in parameter_ids:
            raise FitError("experiment parameter has a duplicate id")
        parameter_ids.add(identifier)
        name = token(row.get("name"), "experiment parameter name")
        if name in experiment_parameters:
            raise FitError("experiment parameter has a duplicate name")
        experiment_parameters[name] = row
    if set(manifest_parameters) != set(experiment_parameters):
        raise FitError("fit and experiment parameter sets differ")
    for name, row in manifest_parameters.items():
        other = experiment_parameters[name]
        if (other.get("unit") != row["unit"] or
                finite(other.get("baseline"), "experiment parameter baseline") !=
                float(row["baseline"]) or
                finite(other.get("minimum"), "experiment parameter minimum") !=
                float(row["minimum"]) or
                finite(other.get("maximum"), "experiment parameter maximum") !=
                float(row["maximum"])):
            raise FitError(f"parameter contract changed: {name}")

    point_ids: set[int] = set()
    baseline_ids: list[int] = []
    for index, row in enumerate(experiment["points"]):
        if type(row) is not dict:
            raise FitError(f"experiment points[{index}] must be an object")
        identifier = row.get("id")
        if type(identifier) is not int or type(identifier) is bool or identifier < 1:
            raise FitError("experiment point has an invalid id")
        if identifier in point_ids:
            raise FitError("experiment point has a duplicate id")
        point_ids.add(identifier)
        digest(row.get("key"), "experiment point key")
        if type(row.get("baseline")) is not bool:
            raise FitError("experiment point baseline flag must be boolean")
        if row["baseline"]:
            baseline_ids.append(identifier)
    if len(point_ids) != 2 or len(baseline_ids) != 1:
        raise FitError(
            "verify-candidate experiment needs one baseline and one candidate point"
        )

    value_ids: set[int] = set()
    value_pairs: set[tuple[int, int]] = set()
    for index, row in enumerate(experiment["values"]):
        if type(row) is not dict:
            raise FitError(f"experiment values[{index}] must be an object")
        identifier = row.get("id")
        point_id = row.get("point_id")
        parameter_id = row.get("parameter_id")
        if (type(identifier) is not int or type(identifier) is bool or
                identifier < 1 or identifier in value_ids):
            raise FitError("experiment value has an invalid or duplicate id")
        value_ids.add(identifier)
        if (type(point_id) is not int or type(point_id) is bool or
                point_id not in point_ids or
                type(parameter_id) is not int or type(parameter_id) is bool or
                parameter_id not in parameter_ids):
            raise FitError("experiment value has an unknown id")
        pair = (point_id, parameter_id)
        if pair in value_pairs:
            raise FitError("experiment has a duplicate point parameter value")
        value_pairs.add(pair)
        finite(row.get("value"), "experiment parameter value")

    point_values = parameter_values(experiment)
    for point in point_values.values():
        for name, value in point.items():
            row = manifest_parameters[name]
            if value < float(row["minimum"]) or value > float(row["maximum"]):
                raise FitError(f"experiment parameter is out of range: {name}")
    baseline_id = baseline_ids[0]
    expected_baseline = {
        name: float(row["baseline"])
        for name, row in manifest_parameters.items()
    }
    if point_values[baseline_id] != expected_baseline:
        raise FitError("baseline point parameters differ from the manifest")
    wanted = {
        name: float(value)
        for name, value in manifest["candidate"]["parameters"].items()
    }
    candidate_ids = [
        point_id for point_id, values in point_values.items()
        if point_id != baseline_id and values == wanted
    ]
    if len(candidate_ids) != 1:
        raise FitError("experiment has no exact candidate point")
    return baseline_id, candidate_ids[0], point_values


def verify_candidate_profile_changes(
        manifest: dict[str, Any], profile: dict[str, Any]) -> None:
    for row in manifest["candidate"]["profile_changes"]:
        parent, key = path_value(profile, row["path"])
        old = finite(parent[key], f"profile target {row['path']}")
        if old != float(row["before"]):
            raise FitError(
                f"candidate profile source changed at path: {row['path']}"
            )


def candidate_gate_data(
        manifest: dict[str, Any], baseline: dict[str, Any],
        candidate: dict[str, Any]) -> dict[str, Any]:
    selection = manifest["selection"]
    limits = by_name(selection["limits"], "split")
    baseline_evidence = by_name(baseline["evidence"], "objective")
    candidate_evidence = by_name(candidate["evidence"], "objective")
    split_gates = []
    for split in ("fit", "check", "audit"):
        increase = (candidate["split_losses"][split] -
                    baseline["split_losses"][split])
        maximum = float(limits[split]["max_mean_loss_increase"])
        split_gates.append({
            "split": split,
            "baseline_loss": baseline["split_losses"][split],
            "candidate_loss": candidate["split_losses"][split],
            "loss_increase": increase,
            "maximum_loss_increase": maximum,
            "passed": increase <= maximum,
        })
    objective_gates = []
    objective_increases = []
    for objective in manifest["objectives"]:
        name = objective["id"]
        split = objective["split"]
        base_loss = baseline_evidence[name]["loss"]
        candidate_loss = candidate_evidence[name]["loss"]
        increase = candidate_loss - base_loss
        maximum = float(limits[split]["max_objective_loss_increase"])
        absolute_maximum = float(limits[split]["max_candidate_loss"])
        objective_increases.append(increase)
        common_gate = {
            "objective": name, "split": split,
            "baseline_loss": base_loss, "candidate_loss": candidate_loss,
            "loss_increase": increase,
            "maximum_loss_increase": maximum,
            "maximum_candidate_loss": absolute_maximum,
        }
        if objective["kind"] == "checked-note-harmonic-decay":
            evidence = candidate_evidence[name]
            mean_error = nonnegative(
                evidence["mean_absolute_t60_error_octaves"],
                "candidate harmonic mean error",
            )
            maximum_error = nonnegative(
                evidence["maximum_absolute_t60_error_octaves"],
                "candidate harmonic maximum error",
            )
            valid_count = evidence["valid_harmonic_count"]
            if (type(valid_count) is not int or type(valid_count) is bool or
                    valid_count < 0):
                raise FitError("candidate valid harmonic count is invalid")
            mean_limit = float(selection[
                "max_candidate_harmonic_mean_error_octaves"
            ])
            harmonic_limit = float(selection[
                "max_candidate_harmonic_maximum_error_octaves"
            ])
            minimum_harmonics = int(
                selection["minimum_candidate_harmonic_count"]
            )
            objective_gates.append({
                **common_gate,
                "checked_note_valid": evidence["checked_note_valid"],
                "checked_harmonic_decay_valid": evidence[
                    "checked_harmonic_decay_valid"
                ],
                "valid_harmonic_count": valid_count,
                "minimum_valid_harmonic_count": minimum_harmonics,
                "mean_absolute_t60_error_octaves": mean_error,
                "maximum_mean_absolute_t60_error_octaves": mean_limit,
                "maximum_absolute_t60_error_octaves": maximum_error,
                "maximum_harmonic_error_octaves": harmonic_limit,
                "passed": bool(
                    increase <= maximum and
                    candidate_loss <= absolute_maximum and
                    evidence["checked_note_valid"] and
                    evidence["checked_harmonic_decay_valid"] and
                    valid_count >= minimum_harmonics and
                    mean_error <= mean_limit and
                    maximum_error <= harmonic_limit
                ),
            })
        else:
            evidence = candidate_evidence[name]
            reference_t60 = positive(
                evidence["reference_t60_seconds"],
                "candidate reference T60")
            model_t60 = positive(
                evidence["model_t60_seconds"],
                "candidate model T60")
            reference_support = positive(
                evidence["reference_support_seconds"],
                "candidate reference support")
            model_support = positive(
                evidence["model_support_seconds"],
                "candidate model support")
            t60_ratio = model_t60 / reference_t60
            support_ratio = model_support / reference_support
            minimum_t60 = float(selection["minimum_candidate_t60_ratio"])
            maximum_t60 = float(selection["maximum_candidate_t60_ratio"])
            minimum_support = float(selection[
                "minimum_candidate_support_ratio"
            ])
            objective_gates.append({
                **common_gate,
                "candidate_t60_ratio": t60_ratio,
                "minimum_candidate_t60_ratio": minimum_t60,
                "maximum_candidate_t60_ratio": maximum_t60,
                "candidate_support_ratio": support_ratio,
                "minimum_candidate_support_ratio": minimum_support,
                "passed": (increase <= maximum and
                           candidate_loss <= absolute_maximum and
                           minimum_t60 <= t60_ratio <= maximum_t60 and
                           support_ratio >= minimum_support),
            })
    expected_gates = []
    maximum_expected = float(selection["max_expected_loss_increase"])
    for name in sorted(manifest["candidate"]["expected_objective_losses"]):
        expected = float(
            manifest["candidate"]["expected_objective_losses"][name]
        )
        actual = candidate_evidence[name]["loss"]
        increase = actual - expected
        expected_gates.append({
            "objective": name, "expected_loss": expected,
            "candidate_loss": actual, "loss_increase": increase,
            "maximum_loss_increase": maximum_expected,
            "passed": increase <= maximum_expected,
        })
    score_increase = candidate["score"] - baseline["score"]
    max_score = float(selection["max_score_increase"])
    score_gate = {
        "baseline_score": baseline["score"],
        "candidate_score": candidate["score"],
        "score_increase": score_increase,
        "maximum_score_increase": max_score,
        "passed": score_increase <= max_score,
    }
    worst_harm = max([0.0] + objective_increases)
    max_harm = float(selection["max_candidate_worst_harm"])
    harm_gate = {
        "candidate_worst_harm": worst_harm,
        "maximum_candidate_worst_harm": max_harm,
        "passed": worst_harm <= max_harm,
    }
    passed = bool(
        score_gate["passed"] and harm_gate["passed"] and
        all(row["passed"] for row in split_gates) and
        all(row["passed"] for row in objective_gates) and
        all(row["passed"] for row in expected_gates)
    )
    return {
        "passed": passed, "score": score_gate,
        "candidate_worst_harm": harm_gate,
        "splits": split_gates, "objectives": objective_gates,
        "expected_objectives": expected_gates,
    }


def verify_candidate_points(
        manifest: dict[str, Any], experiment: dict[str, Any], bundle_root: Path,
        bindings: dict[str, Path], binding_hashes: dict[str, str],
        analyzer: Path, analyzer_hash: str, baseline_id: int,
        candidate_id: int,
        point_values: dict[int, dict[str, float]]) -> list[dict[str, Any]]:
    required_bindings = {row["reference_binding"] for row in manifest["objectives"]}
    if set(bindings) != required_bindings:
        raise FitError("bindings differ from the verify-candidate objectives")
    cases = by_name(experiment["cases"], "name")
    case_ids: set[int] = set()
    for row in cases.values():
        identifier = row.get("id")
        if (type(identifier) is not int or type(identifier) is bool or
                identifier < 1 or identifier in case_ids):
            raise FitError("experiment case has an invalid or duplicate id")
        case_ids.add(identifier)
        if row.get("split") not in ("fit", "check"):
            raise FitError("experiment case has an invalid split")
    required_case_names = {row["case"] for row in manifest["objectives"]}
    if set(cases) != required_case_names:
        raise FitError("experiment case set differs from the manifest")
    jobs: dict[int, dict[str, Any]] = {}
    for row in experiment["jobs"]:
        identifier = row.get("id") if type(row) is dict else None
        if (type(identifier) is not int or type(identifier) is bool or
                identifier < 1 or identifier in jobs):
            raise FitError("experiment job has an invalid or duplicate id")
        if (type(row.get("point_id")) is not int or
                row["point_id"] not in point_values or
                type(row.get("case_id")) is not int or
                row["case_id"] not in case_ids):
            raise FitError("experiment job has an unknown point or case")
        jobs[identifier] = row
    expected_job_pairs = {
        (point_id, int(case["id"]))
        for point_id in point_values for case in cases.values()
    }
    job_pairs = {(row["point_id"], row["case_id"]) for row in jobs.values()}
    if len(jobs) != len(expected_job_pairs) or job_pairs != expected_job_pairs:
        raise FitError("experiment job set differs from its points and cases")
    artifacts: dict[tuple[int, str], dict[str, Any]] = {}
    for row in experiment["artifacts"]:
        if type(row) is not dict or type(row.get("job_id")) is not int:
            raise FitError("experiment artifact has an invalid job id")
        resource_id = token(row.get("resource_id"),
                            "experiment artifact resource_id")
        key = (row["job_id"], resource_id)
        if row["job_id"] not in jobs or key in artifacts:
            raise FitError("experiment artifact has an unknown or duplicate job")
        artifacts[key] = row
    resources_by_case = {}
    for objective in manifest["objectives"]:
        resources_by_case.setdefault(objective["case"], set()).add(
            objective["resource_id"])
    expected_artifacts = {
        (job_id, resource_id)
        for job_id, job in jobs.items()
        for resource_id in resources_by_case[
            next(name for name, case in cases.items()
                 if case["id"] == job["case_id"])
        ]
    }
    if set(artifacts) != expected_artifacts:
        raise FitError("experiment artifact set differs from its jobs")

    point_descriptions = {
        int(row["id"]): row for row in experiment["points"]
    }
    score_weights = manifest["selection"]["score_weights"]
    audio_cache: dict[tuple[int, str], dict[str, Any]] = {}
    result = []
    for point_id in (baseline_id, candidate_id):
        split_totals = {"fit": 0.0, "check": 0.0, "audit": 0.0}
        split_weights = {"fit": 0.0, "check": 0.0, "audit": 0.0}
        evidence = []
        for objective in manifest["objectives"]:
            case = cases.get(objective["case"])
            if case is None:
                raise FitError(f"missing case: {objective['case']}")
            expected_case_split = (
                "fit" if objective["split"] == "fit" else "check"
            )
            if case.get("split") != expected_case_split:
                raise FitError(
                    f"objective uses the wrong case split: {objective['id']}"
                )
            matching_jobs = [
                row for row in jobs.values()
                if row["point_id"] == point_id and
                row["case_id"] == case["id"]
            ]
            if len(matching_jobs) != 1:
                raise FitError("audio objective needs one job per point")
            job = matching_jobs[0]
            artifact = artifacts.get((job["id"], objective["resource_id"]))
            if artifact is None:
                raise FitError("audio objective model artifact is missing")
            cache_key = (job["id"], objective["id"])
            measure = audio_cache.get(cache_key)
            if measure is None:
                model = artifact_path(bundle_root, artifact)
                artifact_value = artifact.get("artifact")
                if type(artifact_value) is not dict:
                    raise FitError("experiment artifact has invalid provenance")
                model_hash = digest(artifact_value.get("sha256"),
                                    "experiment artifact sha256")
                binding_id = objective["reference_binding"]
                if objective["kind"] == "checked-note-harmonic-decay":
                    if (binding_hashes[binding_id] !=
                            objective["reference_sha256"]):
                        raise FitError(
                            "checked harmonic reference has the wrong hash: " +
                            binding_id
                        )
                    measure = checked_note_harmonic_decay(
                        analyzer, bindings[binding_id], model,
                        float(objective["expected_hz"]), analyzer_hash,
                        binding_hashes[binding_id], model_hash,
                    )
                else:
                    measure = run_passive_decay(
                        bindings[binding_id], model,
                        binding_hashes[binding_id], model_hash,
                    )
                audio_cache[cache_key] = measure
            loss = passive_decay_loss(
                objective["kind"], measure, float(objective["scale"])
            )
            split = objective["split"]
            weight = float(objective["weight"])
            split_totals[split] += weight * loss
            split_weights[split] += weight
            evidence.append({
                "objective": objective["id"], "split": split,
                "weight": weight, "scale": float(objective["scale"]),
                "loss": loss, **measure,
            })
        split_losses = {}
        for split in ("fit", "check", "audit"):
            if split_weights[split] <= 0.0:
                raise FitError(f"fit manifest has no {split} objective")
            split_losses[split] = split_totals[split] / split_weights[split]
        score = sum(
            split_losses[split] * float(score_weights[split])
            for split in ("fit", "check", "audit")
        )
        description = point_descriptions[point_id]
        result.append({
            "point_id": point_id, "point_key": description["key"],
            "baseline": point_id == baseline_id,
            "parameters": point_values[point_id],
            "split_losses": split_losses, "score": score,
            "evidence": evidence,
        })
    return result


def select_verified_candidate(
        arguments: argparse.Namespace, manifest: dict[str, Any],
        manifest_hash: str, manifest_path: Path,
        experiment: dict[str, Any], experiment_hash: str,
        experiment_path: Path, bundle_root: Path, analyzer_hash: str,
        analyzer: Path, profile_hash: str, profile_source: bytes,
        profile_path: Path, selector_hash: str, bindings: dict[str, Path],
        binding_hashes: dict[str, str]) -> bool:
    baseline_id, candidate_id, point_values = verify_candidate_experiment(
        manifest, experiment
    )
    profile = parse_json(profile_source, profile_path)
    verify_candidate_profile_changes(manifest, profile)
    points = verify_candidate_points(
        manifest, experiment, bundle_root, bindings, binding_hashes,
        analyzer, analyzer_hash, baseline_id, candidate_id, point_values,
    )
    baseline = next(row for row in points if row["baseline"])
    candidate = next(row for row in points if not row["baseline"])
    gates = candidate_gate_data(manifest, baseline, candidate)
    for path, expected, field in (
            (manifest_path, manifest_hash, "fit manifest"),
            (experiment_path, experiment_hash, "experiment result"),
            (analyzer, analyzer_hash, "analyzer"),
            (profile_path, profile_hash, "profile"),
            (Path(__file__), selector_hash, "fit selector")):
        if sha256(path) != expected:
            raise FitError(f"{field} changed during selection")
    for name, path in bindings.items():
        if sha256(path) != binding_hashes[name]:
            raise FitError(f"binding changed during selection: {name}")
    result = {
        "schema": "hwa-instrument-fit-result", "schema_version": 2,
        "status": "pass" if gates["passed"] else "fail",
        "selection_mode": "verify-candidate",
        "method_versions": {
            "selection": VERIFY_CANDIDATE_METHOD_VERSION,
            **passive_method_versions(manifest["objectives"]),
        },
        "adapter_id": manifest["adapter_id"],
        "fit_manifest_sha256": manifest_hash,
        "experiment_result_sha256": experiment_hash,
        "analyzer_sha256": analyzer_hash,
        "selector_sha256": selector_hash,
        "profile_sha256": profile_hash,
        "reference_bindings": [
            {"id": name, "sha256": binding_hashes[name]}
            for name in sorted(bindings)
        ],
        "baseline_point_id": baseline_id,
        "candidate_point_id": candidate_id,
        "candidate_parameters": manifest["candidate"]["parameters"],
        "profile_changes": manifest["candidate"]["profile_changes"],
        "profile_adapter_sha256": manifest["candidate"][
            "profile_adapter_sha256"],
        "baseline_score": baseline["score"],
        "candidate_score": candidate["score"],
        "losses": {
            "baseline": baseline["split_losses"],
            "candidate": candidate["split_losses"],
        },
        "points": points, "gates": gates,
    }
    if gates["passed"]:
        result["chosen_point_id"] = candidate_id
        result["chosen_parameters"] = manifest["candidate"]["parameters"]
    write_new_json(arguments.output, result)
    return bool(gates["passed"])


def select(arguments: argparse.Namespace) -> Optional[bool]:
    _selector_path, _selector_source, selector_hash = file_snapshot(
        Path(__file__), "fit selector"
    )
    manifest_path, manifest_source, manifest_hash = file_snapshot(
        arguments.manifest, "fit manifest"
    )
    manifest = fit_manifest(manifest_path, manifest_source)
    experiment_path, experiment_source, experiment_hash = file_snapshot(
        arguments.experiment, "experiment result"
    )
    experiment, bundle_root = experiment_bundle(experiment_path,
                                                  experiment_source)
    analyzer, _analyzer_source, analyzer_hash = file_snapshot(
        arguments.analyzer, "analyzer"
    )
    profile, profile_source, profile_hash = file_snapshot(
        arguments.profile, "profile"
    )
    bindings = parse_bindings(arguments.bind)
    binding_hashes = {name: sha256(path) for name, path in bindings.items()}

    if manifest["schema_version"] == 2:
        return select_verified_candidate(
            arguments, manifest, manifest_hash, manifest_path,
            experiment, experiment_hash, experiment_path, bundle_root,
            analyzer_hash, analyzer, profile_hash, profile_source, profile,
            selector_hash, bindings, binding_hashes,
        )

    required_bindings = {
        row["reference_binding"] for row in manifest["objectives"]
        if row["kind"] != "experiment-gap"
    }
    fit_only = manifest["selection"].get("mode") == "fit-only"
    if fit_only and set(bindings) != required_bindings:
        raise FitError("fit-only bindings differ from its fit objectives")
    for objective in manifest["objectives"]:
        expected_hash = objective.get("reference_sha256")
        if expected_hash is not None:
            binding_name = objective["reference_binding"]
            if binding_hashes.get(binding_name) != expected_hash:
                raise FitError(
                    f"reference binding has the wrong hash: {binding_name}"
                )

    manifest_parameters = by_name(manifest["parameters"], "id")
    experiment_parameters = by_name(experiment["parameters"], "name")
    if set(manifest_parameters) != set(experiment_parameters):
        raise FitError("fit and experiment parameter sets differ")
    for name, row in manifest_parameters.items():
        other = experiment_parameters[name]
        if (other.get("unit") != row["unit"] or
                float(other["baseline"]) != float(row["baseline"]) or
                float(other["minimum"]) != float(row["minimum"]) or
                float(other["maximum"]) != float(row["maximum"])):
            raise FitError(f"parameter contract changed: {name}")

    cases = by_name(experiment["cases"], "name")
    responses = by_name(experiment["responses"], "name")
    points = {int(row["id"]): row for row in experiment["points"]}
    baseline_ids = [point_id for point_id, row in points.items()
                    if row.get("baseline") is True]
    if len(baseline_ids) != 1:
        raise FitError("experiment must have one baseline point")
    point_values = parameter_values(experiment)
    candidate_index: dict[tuple[int, int, str], dict[str, Any]] = {}
    for row in experiment["candidates"]:
        key = (int(row["point_id"]), int(row["response_id"]), row.get("split"))
        if key in candidate_index:
            raise FitError("duplicate experiment candidate")
        candidate_index[key] = row

    jobs = {int(row["id"]): row for row in experiment["jobs"]}
    artifacts: dict[tuple[int, str], dict[str, Any]] = {}
    for row in experiment["artifacts"]:
        key = (int(row["job_id"]), row.get("resource_id"))
        if key in artifacts:
            raise FitError("duplicate job artifact")
        artifacts[key] = row

    audio_cache: dict[tuple[int, str], dict[str, Any]] = {}
    harmonic_reference_cache: dict[
        tuple[str, float, int], list[dict[str, Any]]
    ] = {}
    point_rows: list[dict[str, Any]] = []
    selection = manifest["selection"]
    for point_id in sorted(points):
        losses = {"fit": 0.0, "check": 0.0}
        weights = {"fit": 0.0, "check": 0.0}
        worst_harm = 0.0
        evidence: list[dict[str, Any]] = []
        eligible = True
        for objective in manifest["objectives"]:
            split = objective["split"]
            weight = float(objective["weight"])
            scale = float(objective["scale"])
            if objective["kind"] == "experiment-gap":
                response = responses.get(objective["response"])
                if response is None:
                    raise FitError(f"missing response: {objective['response']}")
                candidate = candidate_index.get((point_id, int(response["id"]), split))
                if candidate is None or candidate.get("values_valid") is not True:
                    eligible = False
                    loss = math.inf
                    harm = math.inf
                else:
                    loss = finite(candidate.get("mean_gap"), "candidate mean_gap") / scale
                    harm = finite(candidate.get("worst_harm"), "candidate worst_harm")
                    worst_harm = max(worst_harm, harm)
                evidence.append({"objective": objective["id"], "loss": loss,
                                 "worst_harm": harm})
            else:
                case = cases.get(objective["case"])
                reference = bindings.get(objective["reference_binding"])
                if case is None:
                    raise FitError(f"missing case: {objective['case']}")
                if reference is None:
                    raise FitError(
                        f"missing binding: {objective['reference_binding']}"
                    )
                matching_jobs = [row for row in jobs.values()
                                 if int(row["point_id"]) == point_id and
                                 int(row["case_id"]) == int(case["id"])]
                if len(matching_jobs) != 1:
                    raise FitError("audio objective needs one job per point")
                job = matching_jobs[0]
                artifact = artifacts.get((int(job["id"]), objective["resource_id"]))
                if artifact is None:
                    raise FitError("audio objective model artifact is missing")
                cache_key = (int(job["id"]), objective["id"])
                measure = audio_cache.get(cache_key)
                if measure is None:
                    model = artifact_path(bundle_root, artifact)
                    model_hash = artifact["artifact"]["sha256"]
                    if objective["kind"] == "body-envelope":
                        measure = run_body_envelope(analyzer, reference, model)
                        if (sha256(analyzer) != analyzer_hash or
                                sha256(reference) != binding_hashes[
                                objective["reference_binding"]] or
                                sha256(model) != model_hash):
                            raise FitError(
                                "body-envelope input changed during analysis"
                            )
                    elif objective["kind"] == "harmonic-decay":
                        fundamental = float(objective["fundamental_hz"])
                        count = int(objective["harmonic_count"])
                        reference_key = (
                            objective["reference_binding"], fundamental, count
                        )
                        reference_profile = harmonic_reference_cache.get(
                            reference_key
                        )
                        if reference_profile is None:
                            reference_profile = harmonic_decay_profile(
                                reference, fundamental, count,
                                binding_hashes[
                                    objective["reference_binding"]],
                            )
                            harmonic_reference_cache[reference_key] = (
                                reference_profile
                            )
                        model_profile = harmonic_decay_profile(
                            model, fundamental, count, model_hash
                        )
                        harmonic = compare_harmonic_decay_profiles(
                            reference_profile, model_profile,
                            fundamental, count,
                        )
                        comparison = harmonic["comparison"]
                        measure = {
                            "fundamental_hz": harmonic["fundamental_hz"],
                            "harmonic_count": harmonic["harmonic_count"],
                            "valid_harmonic_count": comparison[
                                "valid_harmonic_count"],
                            "mean_absolute_t60_error_octaves": comparison[
                                "mean_absolute_t60_error_octaves"],
                            "maximum_absolute_t60_error_octaves": comparison[
                                "maximum_absolute_t60_error_octaves"],
                        }
                    elif objective["kind"] == "checked-note-harmonic-decay":
                        expected_hz = float(objective["expected_hz"])
                        measure = checked_note_harmonic_decay(
                            analyzer, reference, model, expected_hz,
                            analyzer_hash,
                            binding_hashes[objective["reference_binding"]],
                            model_hash,
                        )
                    else:
                        measure = run_passive_decay(
                            reference, model,
                            binding_hashes[objective["reference_binding"]],
                            model_hash,
                        )
                    audio_cache[cache_key] = measure
                loss = passive_decay_loss(objective["kind"], measure, scale)
                harm = 0.0
                evidence.append({"objective": objective["id"], "loss": loss,
                                 **measure})
            losses[split] += weight * loss
            weights[split] += weight
        required_splits = ("fit",) if fit_only else ("fit", "check")
        for split in required_splits:
            if weights[split] <= 0.0:
                raise FitError(f"fit manifest has no {split} objective")
            losses[split] /= weights[split]
        if not all(v1_objective_passes_absolute_limits(row, selection)
                   for row in evidence):
            eligible = False
        score = losses["fit"] + float(selection["check_weight"]) * losses["check"]
        source_groups = v1_source_group_rows(
            manifest["objectives"], evidence
        )
        point_rows.append({
            "point_id": point_id,
            "point_key": points[point_id]["key"],
            "baseline": points[point_id].get("baseline") is True,
            "parameters": point_values[point_id],
            "fit_loss": losses["fit"],
            "check_loss": losses["check"],
            "score": score,
            "worst_harm": worst_harm,
            "eligible": eligible,
            "evidence": evidence,
            "source_groups": source_groups,
        })

    baseline = next(row for row in point_rows if row["baseline"])
    max_check = (baseline["check_loss"] +
                 float(selection["max_check_loss_increase"]))
    max_harm = float(selection["max_candidate_worst_harm"])
    baseline_evidence = by_name(baseline["evidence"], "objective")
    maximum_objective_increase = selection.get(
        "max_objective_loss_increase"
    )
    baseline_source_groups = {
        (item["split"], item["source_group"]): item
        for item in baseline["source_groups"]
    }
    maximum_source_loss = selection.get(
        "max_candidate_source_mean_loss"
    )
    maximum_source_increase = selection.get(
        "max_source_mean_loss_increase"
    )
    for row in point_rows:
        for item in row["evidence"]:
            baseline_item = baseline_evidence[item["objective"]]
            item["loss_increase_from_baseline"] = (
                float(item["loss"]) - float(baseline_item["loss"])
            )
        objective_increase_passes = bool(
            maximum_objective_increase is None or all(
                float(item["loss_increase_from_baseline"]) <=
                float(maximum_objective_increase)
                for item in row["evidence"]
            )
        )
        for item in row["source_groups"]:
            key = (item["split"], item["source_group"])
            baseline_item = baseline_source_groups.get(key)
            if baseline_item is None:
                raise FitError("candidate source groups differ from baseline")
            item["loss_increase_from_baseline"] = (
                float(item["loss"]) - float(baseline_item["loss"])
            )
        source_groups_pass = bool(
            maximum_source_loss is None or all(
                float(item["loss"]) <= float(maximum_source_loss) and
                float(item["loss_increase_from_baseline"]) <=
                float(maximum_source_increase)
                for item in row["source_groups"]
            )
        )
        row["eligible"] = bool(
            (fit_only or not row["baseline"]) and row["eligible"] and
            row["check_loss"] <= max_check and
            row["worst_harm"] <= max_harm and
            objective_increase_passes and source_groups_pass
        )
    eligible = [row for row in point_rows if row["eligible"]]
    result = {
        "schema": "hwa-instrument-fit-result",
        "schema_version": 1,
        "status": "pass" if eligible else "fail",
        "method_versions": {
            "selection": selection_method_version(manifest),
            **passive_method_versions(manifest["objectives"]),
        },
        "adapter_id": manifest["adapter_id"],
        "fit_manifest_sha256": manifest_hash,
        "experiment_result_sha256": experiment_hash,
        "analyzer_sha256": analyzer_hash,
        "selector_sha256": selector_hash,
        "profile_sha256": profile_hash,
        "reference_bindings": [
            {"id": name, "sha256": binding_hashes[name]}
            for name in sorted(bindings)
        ],
        "baseline_point_id": baseline["point_id"],
        "baseline_score": baseline["score"],
        "points": sorted(point_rows, key=lambda row: (
            not row["eligible"], row["score"], row["point_id"]
        )),
    }
    if fit_only:
        result["selection_mode"] = "fit-only"
    if eligible:
        chosen = min(
            eligible,
            key=lambda row: v1_rank_key(
                row, float(selection["check_weight"])
            ),
        )
        result.update({
            "chosen_point_id": chosen["point_id"],
            "chosen_parameters": chosen["parameters"],
            "chosen_score": chosen["score"],
        })
    write_new_json(arguments.output, result)
    return bool(eligible)


def path_value(root: Any, parts: list[Any]) -> tuple[Any, Any]:
    current = root
    for part in parts[:-1]:
        if type(part) is int:
            if type(current) is not list or part < 0 or part >= len(current):
                raise FitError(f"profile path index is out of range: {parts}")
        else:
            if type(current) is not dict or part not in current:
                raise FitError(f"profile path key is missing: {parts}")
        current = current[part]
    final = parts[-1]
    if ((type(final) is int and
         (type(current) is not list or final < 0 or final >= len(current))) or
        (type(final) is str and
         (type(current) is not dict or final not in current))):
        raise FitError(f"profile path target is missing: {parts}")
    return current, final


def write_new_json(path: Path, value: dict[str, Any]) -> None:
    path = path.absolute()
    if path.exists() or path.is_symlink():
        raise FitError(f"output already exists: {path}")
    if not path.parent.is_dir():
        raise FitError(f"output parent is missing: {path.parent}")
    text = json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n"
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.tmp-", dir=path.parent
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        try:
            os.link(temporary_name, path)
        except FileExistsError as error:
            raise FitError(f"output already exists: {path}") from error
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise
    os.unlink(temporary_name)


def validated_verify_result_point(
        manifest: dict[str, Any], row: Any, point_id: int,
        baseline: bool, parameters: dict[str, float]) -> dict[str, Any]:
    if type(row) is not dict:
        raise FitError("fit result point must be an object")
    exact_keys(row, {
        "point_id", "point_key", "baseline", "parameters", "split_losses",
        "score", "evidence",
    }, "fit result point")
    if row.get("point_id") != point_id or row.get("baseline") is not baseline:
        raise FitError("fit result point identity changed")
    digest(row.get("point_key"), "fit result point key")
    values = row.get("parameters")
    if type(values) is not dict or set(values) != set(parameters):
        raise FitError("fit result point parameter set changed")
    checked_values = {
        name: finite(values[name], f"fit result point parameter {name}")
        for name in sorted(values)
    }
    if checked_values != parameters:
        raise FitError("fit result point parameters changed")
    evidence = row.get("evidence")
    if type(evidence) is not list or len(evidence) != len(manifest["objectives"]):
        raise FitError("fit result point evidence count changed")
    evidence_by_name = by_name(evidence, "objective")
    if set(evidence_by_name) != {item["id"] for item in manifest["objectives"]}:
        raise FitError("fit result point objective set changed")
    totals = {"fit": 0.0, "check": 0.0, "audit": 0.0}
    weights = {"fit": 0.0, "check": 0.0, "audit": 0.0}
    checked_evidence = []
    for objective in manifest["objectives"]:
        item = evidence_by_name[objective["id"]]
        if type(item) is not dict:
            raise FitError("fit result evidence must be an object")
        if item.get("split") != objective["split"]:
            raise FitError("fit result evidence split changed")
        weight = positive(item.get("weight"), "fit result evidence weight")
        scale = positive(item.get("scale"), "fit result evidence scale")
        if (weight != float(objective["weight"]) or
                scale != float(objective["scale"])):
            raise FitError("fit result evidence contract changed")
        loss = nonnegative(item.get("loss"), "fit result evidence loss")
        if objective["kind"] == "checked-note-harmonic-decay":
            exact_keys(item, {
                "objective", "split", "weight", "scale", "loss",
                "checked_note_valid", "checked_harmonic_decay_valid",
                "reference_pitch_error_cents", "model_pitch_error_cents",
                "valid_harmonic_count", "shared_reference_coverage",
                "rms_t60_error_octaves",
                "mean_absolute_t60_error_octaves",
                "maximum_absolute_t60_error_octaves",
                "median_t60_bias_octaves",
            }, "fit result checked harmonic evidence")
            for name in ("checked_note_valid",
                         "checked_harmonic_decay_valid"):
                if type(item[name]) is not bool:
                    raise FitError(
                        "fit result checked harmonic validity changed"
                    )
            count = item["valid_harmonic_count"]
            if (type(count) is not int or type(count) is bool or count < 0):
                raise FitError(
                    "fit result valid harmonic count changed"
                )
            measured = nonnegative(
                item.get("rms_t60_error_octaves"),
                "fit result evidence rms_t60_error_octaves",
            )
            for name in (
                    "reference_pitch_error_cents", "model_pitch_error_cents",
                    "shared_reference_coverage",
                    "mean_absolute_t60_error_octaves",
                    "maximum_absolute_t60_error_octaves",
                    "median_t60_bias_octaves"):
                finite(item[name], f"fit result evidence {name}")
        else:
            measured_field = (
                "combined_shape_rmse_db"
                if objective["kind"] == "passive-decay-shape"
                else "shape_rmse_db"
            )
            measured = nonnegative(
                item.get(measured_field),
                "fit result evidence " + measured_field,
            )
            for name, value in item.items():
                if name not in ("objective", "split"):
                    finite(value, f"fit result evidence {name}")
        if loss != measured / scale:
            raise FitError("fit result evidence loss changed")
        split = objective["split"]
        totals[split] += weight * loss
        weights[split] += weight
        checked_evidence.append(item)
    calculated_losses = {
        split: totals[split] / weights[split]
        for split in ("fit", "check", "audit")
    }
    split_losses = row.get("split_losses")
    if type(split_losses) is not dict or set(split_losses) != set(calculated_losses):
        raise FitError("fit result split losses changed")
    checked_losses = {
        split: nonnegative(split_losses[split], f"fit result {split} loss")
        for split in ("fit", "check", "audit")
    }
    if checked_losses != calculated_losses:
        raise FitError("fit result split losses changed")
    score = nonnegative(row.get("score"), "fit result point score")
    calculated_score = sum(
        calculated_losses[split] *
        float(manifest["selection"]["score_weights"][split])
        for split in ("fit", "check", "audit")
    )
    if score != calculated_score:
        raise FitError("fit result point score changed")
    return {
        "point_id": point_id, "point_key": row["point_key"],
        "baseline": baseline, "parameters": checked_values,
        "split_losses": checked_losses, "score": score,
        "evidence": checked_evidence,
    }


def validate_verify_result(
        fit: dict[str, Any], manifest: dict[str, Any], manifest_hash: str,
        source_hash: str, selector_hash: str) -> tuple[
            dict[str, Any], dict[str, Any]]:
    status = fit.get("status")
    if status not in ("pass", "fail"):
        raise FitError("fit result has an invalid status")
    expected_fields = {
        "schema", "schema_version", "status", "method_versions",
        "selection_mode",
        "adapter_id", "fit_manifest_sha256", "experiment_result_sha256",
        "analyzer_sha256", "selector_sha256", "profile_sha256",
        "reference_bindings", "baseline_point_id", "candidate_point_id",
        "candidate_parameters", "profile_changes", "baseline_score",
        "profile_adapter_sha256", "candidate_score", "losses", "points",
        "gates",
    }
    if status == "pass":
        expected_fields.update({"chosen_point_id", "chosen_parameters"})
    exact_keys(fit, expected_fields, "fit result")
    expected_methods = {
        "selection": VERIFY_CANDIDATE_METHOD_VERSION,
        **passive_method_versions(manifest["objectives"]),
    }
    if (fit.get("schema") != "hwa-instrument-fit-result" or
            fit.get("schema_version") != 2 or
            fit.get("selection_mode") != "verify-candidate" or
            fit.get("adapter_id") != manifest["adapter_id"] or
            fit.get("method_versions") != expected_methods or
            fit.get("fit_manifest_sha256") != manifest_hash or
            fit.get("profile_sha256") != source_hash or
            fit.get("selector_sha256") != selector_hash):
        raise FitError("fit result does not bind this manifest and profile")
    digest(fit.get("experiment_result_sha256"),
           "fit result experiment_result_sha256")
    digest(fit.get("analyzer_sha256"), "fit result analyzer_sha256")
    digest(fit.get("selector_sha256"), "fit result selector_sha256")
    bindings = fit.get("reference_bindings")
    if type(bindings) is not list:
        raise FitError("fit result reference_bindings must be a list")
    binding_rows: dict[str, dict[str, Any]] = {}
    for index, row in enumerate(bindings):
        if type(row) is not dict:
            raise FitError(f"reference_bindings[{index}] must be an object")
        exact_keys(row, {"id", "sha256"}, f"reference_bindings[{index}]")
        name = token(row.get("id"), f"reference_bindings[{index}].id")
        if name in binding_rows:
            raise FitError("fit result has a duplicate reference binding")
        digest(row.get("sha256"), f"reference_bindings[{index}].sha256")
        binding_rows[name] = row
    expected_bindings = {
        row["reference_binding"] for row in manifest["objectives"]
    }
    if set(binding_rows) != expected_bindings:
        raise FitError("fit result reference bindings changed")

    baseline_id = fit.get("baseline_point_id")
    candidate_id = fit.get("candidate_point_id")
    if (type(baseline_id) is not int or type(baseline_id) is bool or
            type(candidate_id) is not int or type(candidate_id) is bool or
            baseline_id == candidate_id):
        raise FitError("fit result point ids are invalid")
    manifest_parameters = by_name(manifest["parameters"], "id")
    baseline_parameters = {
        name: float(row["baseline"])
        for name, row in manifest_parameters.items()
    }
    candidate_parameters = {
        name: float(value)
        for name, value in manifest["candidate"]["parameters"].items()
    }
    if fit.get("candidate_parameters") != manifest["candidate"]["parameters"]:
        raise FitError("fit result candidate parameters changed")
    if fit.get("profile_changes") != manifest["candidate"]["profile_changes"]:
        raise FitError("fit result profile changes changed")
    if (fit.get("profile_adapter_sha256") !=
            manifest["candidate"]["profile_adapter_sha256"]):
        raise FitError("fit result profile adapter hash changed")
    if status == "pass" and (
            fit.get("chosen_point_id") != candidate_id or
            fit.get("chosen_parameters") != manifest["candidate"]["parameters"]):
        raise FitError("fit result chosen candidate changed")
    rows = fit.get("points")
    if type(rows) is not list or len(rows) != 2:
        raise FitError("fit result must contain two points")
    by_id: dict[int, dict[str, Any]] = {}
    for row in rows:
        if type(row) is not dict or type(row.get("point_id")) is not int:
            raise FitError("fit result point has an invalid id")
        if row["point_id"] in by_id:
            raise FitError("fit result point has a duplicate id")
        by_id[row["point_id"]] = row
    if set(by_id) != {baseline_id, candidate_id}:
        raise FitError("fit result point ids changed")
    baseline = validated_verify_result_point(
        manifest, by_id[baseline_id], baseline_id, True, baseline_parameters
    )
    candidate = validated_verify_result_point(
        manifest, by_id[candidate_id], candidate_id, False,
        candidate_parameters,
    )
    if (finite(fit.get("baseline_score"), "fit result baseline_score") !=
            baseline["score"] or
            finite(fit.get("candidate_score"), "fit result candidate_score") !=
            candidate["score"]):
        raise FitError("fit result summary scores changed")
    losses = fit.get("losses")
    expected_losses = {
        "baseline": baseline["split_losses"],
        "candidate": candidate["split_losses"],
    }
    if losses != expected_losses:
        raise FitError("fit result split loss summary changed")
    gates = candidate_gate_data(manifest, baseline, candidate)
    if fit.get("gates") != gates:
        raise FitError("fit result gate data changed")
    expected_status = "pass" if gates["passed"] else "fail"
    if status != expected_status:
        raise FitError("fit result status differs from its gates")
    if not gates["passed"]:
        raise FitError("fit result did not pass candidate gates")
    return baseline, candidate


def write_verified_profile(
        arguments: argparse.Namespace, manifest: dict[str, Any],
        manifest_path: Path, manifest_hash: str, fit: dict[str, Any],
        fit_path: Path, fit_hash: str, source: Path, source_bytes: bytes,
        source_hash: str, adapter: Path, adapter_hash: str) -> None:
    selector_hash = sha256(Path(__file__))
    validate_verify_result(
        fit, manifest, manifest_hash, source_hash, selector_hash
    )
    if adapter_hash != manifest["candidate"]["profile_adapter_sha256"]:
        raise FitError("profile adapter does not match the verified candidate")
    profile = parse_json(source_bytes, source)
    verify_candidate_profile_changes(manifest, profile)
    changes = manifest["candidate"]["profile_changes"]
    for row in changes:
        parent, key = path_value(profile, row["path"])
        parent[key] = float(row["after"])

    output = arguments.output.absolute()
    receipt = arguments.receipt.absolute()
    if output.exists() or output.is_symlink() or receipt.exists() or receipt.is_symlink():
        raise FitError("profile or receipt output already exists")
    if output.parent != receipt.parent or not output.parent.is_dir():
        raise FitError("profile and receipt need the same existing parent")
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{output.name}.tmp-", dir=output.parent
    )
    os.close(descriptor)
    temporary = Path(temporary_name)
    published = False
    receipt_published = False
    try:
        temporary.write_text(
            json.dumps(profile, indent=2, ensure_ascii=True,
                       allow_nan=False) + "\n",
            encoding="utf-8",
        )
        completed = subprocess.run(
            [str(adapter), "--validate-profile", str(temporary)],
            check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, env={"LC_ALL": "C", "LANG": "C", "TZ": "UTC"},
        )
        if completed.returncode != 0:
            raise FitError(
                "profile adapter rejected output: " + completed.stderr.strip()
            )
        for path, expected, field in (
                (adapter, adapter_hash, "profile adapter"),
                (manifest_path, manifest_hash, "fit manifest"),
                (fit_path, fit_hash, "fit result"),
                (source, source_hash, "source profile"),
                (Path(__file__), selector_hash, "fit selector")):
            if sha256(path) != expected:
                raise FitError(f"{field} changed during profile validation")
        with temporary.open("rb") as stream:
            os.fsync(stream.fileno())
        try:
            os.link(temporary, output)
        except FileExistsError as error:
            raise FitError("profile output already exists") from error
        published = True
        result = {
            "schema": "hwa-profile-write-receipt", "schema_version": 2,
            "adapter_id": manifest["adapter_id"],
            "adapter_sha256": adapter_hash,
            "selector_sha256": selector_hash,
            "fit_manifest_sha256": manifest_hash,
            "fit_result_sha256": fit_hash,
            "experiment_result_sha256": fit["experiment_result_sha256"],
            "source_profile_sha256": source_hash,
            "output_profile_sha256": sha256(temporary),
            "reference_bindings": fit["reference_bindings"],
            "joint_gates": fit["gates"],
            "source_fit_results": [
                {"parameter": row["parameter"],
                 "sha256": row["source_fit_result_sha256"]}
                for row in changes
            ],
            "changes": changes,
        }
        write_new_json(receipt, result)
        receipt_published = True
    except BaseException:
        if published and not receipt_published:
            try:
                if output.exists() and os.path.samefile(output, temporary):
                    output.unlink()
            except (FileNotFoundError, OSError):
                pass
        raise
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def write_profile(arguments: argparse.Namespace) -> None:
    manifest_path, manifest_source, manifest_hash = file_snapshot(
        arguments.manifest, "fit manifest"
    )
    manifest = fit_manifest(manifest_path, manifest_source)
    fit_path, fit_source, fit_hash = file_snapshot(arguments.fit, "fit result")
    fit = parse_json(fit_source, fit_path)
    source, source_bytes, source_hash = file_snapshot(
        arguments.source, "source profile"
    )
    adapter, _adapter_source, adapter_hash = file_snapshot(
        arguments.adapter, "profile adapter"
    )
    if manifest["schema_version"] == 2:
        write_verified_profile(
            arguments, manifest, manifest_path, manifest_hash, fit, fit_path,
            fit_hash, source, source_bytes, source_hash, adapter, adapter_hash,
        )
        return
    required_methods = {
        "selection": selection_method_version(manifest),
        **passive_method_versions(manifest["objectives"]),
    }
    status = fit.get("status")
    selector_hash = fit.get("selector_sha256")
    if (fit.get("schema") != "hwa-instrument-fit-result" or
            fit.get("schema_version") != 1 or
            status not in (None, "pass", "fail") or
            fit.get("adapter_id") != manifest["adapter_id"] or
            fit.get("method_versions") != required_methods or
            type(selector_hash) is not str or len(selector_hash) != 64 or
            any(character not in "0123456789abcdef"
                for character in selector_hash) or
            fit.get("fit_manifest_sha256") != manifest_hash or
            fit.get("profile_sha256") != source_hash):
        raise FitError("fit result does not bind this manifest and profile")
    if status == "fail":
        raise FitError("fit result did not pass selection gates")
    profile = parse_json(source_bytes, source)
    chosen = fit.get("chosen_parameters")
    if type(chosen) is not dict:
        raise FitError("fit result has no chosen parameters")
    expected = {row["id"] for row in manifest["parameters"]}
    if set(chosen) != expected:
        raise FitError("chosen parameter set differs from the manifest")
    changes: list[dict[str, Any]] = []
    for row in manifest["parameters"]:
        if not row["profile_paths"]:
            raise FitError(
                f"adapter does not map {row['id']} to a saved profile"
            )
        value = finite(chosen[row["id"]], f"chosen {row['id']}")
        if value < float(row["minimum"]) or value > float(row["maximum"]):
            raise FitError(f"chosen parameter is out of range: {row['id']}")
        for parts in row["profile_paths"]:
            parent, key = path_value(profile, parts)
            old = finite(parent[key], f"profile target {parts}")
            parent[key] = value
            changes.append({"parameter": row["id"], "path": parts,
                            "before": old, "after": value})

    output = arguments.output.absolute()
    receipt = arguments.receipt.absolute()
    if output.exists() or output.is_symlink() or receipt.exists() or receipt.is_symlink():
        raise FitError("profile or receipt output already exists")
    if output.parent != receipt.parent or not output.parent.is_dir():
        raise FitError("profile and receipt need the same existing parent")
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{output.name}.tmp-", dir=output.parent
    )
    os.close(descriptor)
    temporary = Path(temporary_name)
    published = False
    receipt_published = False
    try:
        temporary.write_text(
            json.dumps(profile, indent=2, ensure_ascii=True, allow_nan=False) + "\n",
            encoding="utf-8"
        )
        completed = subprocess.run(
            [str(adapter), "--validate-profile", str(temporary)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env={"LC_ALL": "C", "LANG": "C", "TZ": "UTC"},
        )
        if completed.returncode != 0:
            raise FitError("profile adapter rejected output: " + completed.stderr.strip())
        if sha256(adapter) != adapter_hash:
            raise FitError("profile adapter changed during validation")
        with temporary.open("rb") as stream:
            os.fsync(stream.fileno())
        try:
            os.link(temporary, output)
        except FileExistsError as error:
            raise FitError("profile output already exists") from error
        published = True
        result = {
            "schema": "hwa-profile-write-receipt",
            "schema_version": 1,
            "adapter_id": manifest["adapter_id"],
            "adapter_sha256": adapter_hash,
            "fit_result_sha256": fit_hash,
            "source_profile_sha256": source_hash,
            "output_profile_sha256": sha256(temporary),
            "changes": changes,
        }
        write_new_json(receipt, result)
        receipt_published = True
    except BaseException:
        if published and not receipt_published:
            try:
                if output.exists() and os.path.samefile(output, temporary):
                    output.unlink()
            except (FileNotFoundError, OSError):
                pass
        raise
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def check_recordings(arguments: argparse.Namespace) -> None:
    analyzer = regular(arguments.analyzer, "analyzer")
    if (not math.isfinite(arguments.excerpt_seconds) or
            arguments.excerpt_seconds < 5.0 or
            arguments.excerpt_seconds > 120.0):
        raise FitError("excerpt-seconds must be from 5 through 120")
    rows = []
    with tempfile.TemporaryDirectory(prefix="hwa-body-recording-check-") as text:
        temporary = Path(text)
        for index, source_text in enumerate(arguments.recording):
            path = regular(Path(source_text), "recording")
            excerpt = temporary / f"recording-{index}.wav"
            with wave.open(str(path), "rb") as input_stream:
                rate = input_stream.getframerate()
                total = input_stream.getnframes()
                count = min(total, round(arguments.excerpt_seconds * rate))
                start = (total - count) // 2
                input_stream.setpos(start)
                audio = input_stream.readframes(count)
                parameters = input_stream.getparams()
            if count < 1 or len(audio) != count * parameters.nchannels * parameters.sampwidth:
                raise FitError(f"cannot read recording excerpt: {path}")
            with wave.open(str(excerpt), "wb") as output_stream:
                output_stream.setparams(parameters)
                output_stream.setnframes(count)
                output_stream.writeframes(audio)
            completed = subprocess.run(
                [str(analyzer), "--json", "body-envelope", str(excerpt)],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={"LC_ALL": "C", "LANG": "C", "TZ": "UTC"}
            )
            if completed.returncode != 0:
                raise FitError("body-envelope failed: " + completed.stderr.strip())
            report = json.loads(completed.stdout, object_pairs_hook=object_pairs)
            reference = report.get("reference")
            if type(reference) is not dict or reference.get("status") != "valid":
                raise FitError(f"recording has too little body-envelope support: {path}")
            rows.append({
                "name": path.name,
                "source_sha256": sha256(path),
                "excerpt_sha256": sha256(excerpt),
                "excerpt_start_frame": start,
                "excerpt_frames": count,
                "confidence": finite(reference.get("confidence"), "confidence"),
                "frames_seen": int(reference.get("frames_seen")),
                "frames_used": int(reference.get("frames_used")),
                "valid_points": sum(1 for point in reference.get("points", [])
                                    if point.get("valid") is True),
            })
    write_new_json(arguments.output, {
        "schema": "hwa-body-envelope-recording-check",
        "schema_version": 1,
        "analyzer_sha256": sha256(analyzer),
        "recordings": rows,
    })


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    choose = commands.add_parser("select")
    choose.add_argument("--manifest", required=True, type=Path)
    choose.add_argument("--experiment", required=True, type=Path)
    choose.add_argument("--analyzer", required=True, type=Path)
    choose.add_argument("--profile", required=True, type=Path)
    choose.add_argument("--bind", action="append", default=[])
    choose.add_argument("--output", required=True, type=Path)
    write = commands.add_parser("write-profile")
    write.add_argument("--manifest", required=True, type=Path)
    write.add_argument("--fit", required=True, type=Path)
    write.add_argument("--source", required=True, type=Path)
    write.add_argument("--adapter", required=True, type=Path)
    write.add_argument("--output", required=True, type=Path)
    write.add_argument("--receipt", required=True, type=Path)
    recordings = commands.add_parser("check-recordings")
    recordings.add_argument("--analyzer", required=True, type=Path)
    recordings.add_argument("--recording", action="append", required=True)
    recordings.add_argument("--excerpt-seconds", type=float, default=30.0)
    recordings.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    try:
        arguments = parse_args()
        if arguments.command == "select":
            if select(arguments) is False:
                return 2
        elif arguments.command == "write-profile":
            write_profile(arguments)
        else:
            check_recordings(arguments)
    except (FitError, OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"instrument_fit.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

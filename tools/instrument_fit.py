#!/usr/bin/env python3
"""Select a combined instrument fit and apply it through a checked adapter."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any
import wave


class FitError(ValueError):
    pass


def object_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise FitError(f"duplicate JSON key: {key}")
        value[key] = item
    return value


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=object_pairs)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise FitError(f"cannot read JSON: {path}: {error}") from error
    if type(value) is not dict:
        raise FitError(f"JSON root must be an object: {path}")
    return value


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


def fit_manifest(path: Path) -> dict[str, Any]:
    value = load_json(path)
    if value.get("schema") != "hwa-instrument-fit" or value.get("schema_version") != 1:
        raise FitError("unsupported fit manifest")
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
        minimum = finite(row.get("minimum"), f"parameters[{index}].minimum")
        maximum = finite(row.get("maximum"), f"parameters[{index}].maximum")
        if minimum >= maximum:
            raise FitError(f"parameters[{index}] has an empty range")
        paths = row.get("profile_paths")
        if type(paths) is not list:
            raise FitError(f"parameters[{index}] needs profile_paths")
        for path_index, profile_path in enumerate(paths):
            if type(profile_path) is not list or not profile_path:
                raise FitError(
                    f"parameters[{index}].profile_paths[{path_index}] is invalid"
                )
            for part in profile_path:
                if type(part) not in (str, int) or type(part) is bool:
                    raise FitError("profile path parts must be strings or integers")
    objective_ids: set[str] = set()
    for index, row in enumerate(objectives):
        if type(row) is not dict:
            raise FitError(f"objectives[{index}] must be an object")
        name = token(row.get("id"), f"objectives[{index}].id")
        if name in objective_ids:
            raise FitError(f"duplicate objective: {name}")
        objective_ids.add(name)
        kind = row.get("kind")
        if kind not in ("experiment-gap", "body-envelope"):
            raise FitError(f"objectives[{index}] has an unknown kind")
        if row.get("split") not in ("fit", "check"):
            raise FitError(f"objectives[{index}] has an invalid split")
        positive(row.get("weight"), f"objectives[{index}].weight")
        positive(row.get("scale"), f"objectives[{index}].scale")
        if kind == "experiment-gap":
            token(row.get("response"), f"objectives[{index}].response")
        else:
            token(row.get("case"), f"objectives[{index}].case")
            token(row.get("reference_binding"),
                  f"objectives[{index}].reference_binding")
            token(row.get("resource_id"), f"objectives[{index}].resource_id")
    positive(selection.get("check_weight"), "selection.check_weight")
    finite(selection.get("max_check_loss_increase"),
           "selection.max_check_loss_increase")
    finite(selection.get("max_candidate_worst_harm"),
           "selection.max_candidate_worst_harm")
    return value


def by_name(rows: list[dict[str, Any]], field: str) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for row in rows:
        name = row.get(field)
        if type(name) is not str or name in result:
            raise FitError(f"invalid or duplicate {field}")
        result[name] = row
    return result


def experiment_bundle(path: Path) -> tuple[dict[str, Any], Path]:
    path = regular(path, "experiment result")
    try:
        prefix = path.read_bytes()[:32]
    except OSError as error:
        raise FitError(f"cannot read experiment result: {path}: {error}") from error
    if prefix.startswith(b"HWA_EXPERIMENT,1\r\n"):
        value = load_saved_experiment(path)
    else:
        value = load_json(path)
    if value.get("command") != "experiment" or value.get("schema_version") != 10:
        raise FitError("unsupported experiment result")
    for field in ("parameters", "cases", "responses", "points", "values",
                  "jobs", "artifacts", "candidates"):
        if type(value.get(field)) is not list:
            raise FitError(f"experiment result has invalid {field}")
    return value, path.parent.absolute()


def load_saved_experiment(path: Path) -> dict[str, Any]:
    groups: dict[str, list[list[str]]] = {}
    meta: dict[str, str] = {}
    try:
        with path.open("r", encoding="ascii", newline="") as stream:
            rows = list(csv.reader(stream, strict=True))
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
    path = (root / artifact["path"]).absolute()
    try:
        path.relative_to(root)
    except ValueError as error:
        raise FitError("experiment artifact escapes its bundle") from error
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


def select(arguments: argparse.Namespace) -> None:
    manifest_path = regular(arguments.manifest, "fit manifest")
    manifest = fit_manifest(manifest_path)
    experiment_path = regular(arguments.experiment, "experiment result")
    experiment, bundle_root = experiment_bundle(experiment_path)
    analyzer = regular(arguments.analyzer, "analyzer")
    profile = regular(arguments.profile, "profile")
    bindings = parse_bindings(arguments.bind)

    manifest_parameters = by_name(manifest["parameters"], "id")
    experiment_parameters = by_name(experiment["parameters"], "name")
    if set(manifest_parameters) != set(experiment_parameters):
        raise FitError("fit and experiment parameter sets differ")
    for name, row in manifest_parameters.items():
        other = experiment_parameters[name]
        if (float(other["minimum"]) != float(row["minimum"]) or
                float(other["maximum"]) != float(row["maximum"])):
            raise FitError(f"parameter range changed: {name}")

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

    body_cache: dict[tuple[int, str], dict[str, Any]] = {}
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
                    raise FitError("body-envelope case needs one job per point")
                job = matching_jobs[0]
                artifact = artifacts.get((int(job["id"]), objective["resource_id"]))
                if artifact is None:
                    raise FitError("body-envelope model artifact is missing")
                cache_key = (int(job["id"]), objective["id"])
                body = body_cache.get(cache_key)
                if body is None:
                    body = run_body_envelope(
                        analyzer, reference, artifact_path(bundle_root, artifact)
                    )
                    body_cache[cache_key] = body
                loss = body["shape_rmse_db"] / scale
                harm = 0.0
                evidence.append({"objective": objective["id"], "loss": loss,
                                 **body})
            losses[split] += weight * loss
            weights[split] += weight
        for split in ("fit", "check"):
            if weights[split] <= 0.0:
                raise FitError(f"fit manifest has no {split} objective")
            losses[split] /= weights[split]
        score = losses["fit"] + float(selection["check_weight"]) * losses["check"]
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
        })

    baseline = next(row for row in point_rows if row["baseline"])
    max_check = (baseline["check_loss"] +
                 float(selection["max_check_loss_increase"]))
    max_harm = float(selection["max_candidate_worst_harm"])
    for row in point_rows:
        row["eligible"] = bool(
            row["eligible"] and row["check_loss"] <= max_check and
            row["worst_harm"] <= max_harm
        )
    eligible = [row for row in point_rows if row["eligible"]]
    if not eligible:
        raise FitError("no candidate passed held-out limits")
    chosen = min(eligible, key=lambda row: (
        row["score"], row["check_loss"], row["point_id"]
    ))
    result = {
        "schema": "hwa-instrument-fit-result",
        "schema_version": 1,
        "adapter_id": manifest["adapter_id"],
        "fit_manifest_sha256": sha256(manifest_path),
        "experiment_result_sha256": sha256(experiment_path),
        "analyzer_sha256": sha256(analyzer),
        "profile_sha256": sha256(profile),
        "reference_bindings": [
            {"id": name, "sha256": sha256(path)}
            for name, path in sorted(bindings.items())
        ],
        "baseline_point_id": baseline["point_id"],
        "chosen_point_id": chosen["point_id"],
        "chosen_parameters": chosen["parameters"],
        "baseline_score": baseline["score"],
        "chosen_score": chosen["score"],
        "points": sorted(point_rows, key=lambda row: (
            not row["eligible"], row["score"], row["point_id"]
        )),
    }
    write_new_json(arguments.output, result)


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
        os.replace(temporary_name, path)
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def write_profile(arguments: argparse.Namespace) -> None:
    manifest_path = regular(arguments.manifest, "fit manifest")
    manifest = fit_manifest(manifest_path)
    fit_path = regular(arguments.fit, "fit result")
    fit = load_json(fit_path)
    source = regular(arguments.source, "source profile")
    adapter = regular(arguments.adapter, "profile adapter")
    if (fit.get("schema") != "hwa-instrument-fit-result" or
            fit.get("schema_version") != 1 or
            fit.get("adapter_id") != manifest["adapter_id"] or
            fit.get("fit_manifest_sha256") != sha256(manifest_path) or
            fit.get("profile_sha256") != sha256(source)):
        raise FitError("fit result does not bind this manifest and profile")
    profile = load_json(source)
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
        with temporary.open("rb") as stream:
            os.fsync(stream.fileno())
        os.replace(temporary, output)
        result = {
            "schema": "hwa-profile-write-receipt",
            "schema_version": 1,
            "adapter_id": manifest["adapter_id"],
            "adapter_sha256": sha256(adapter),
            "fit_result_sha256": sha256(fit_path),
            "source_profile_sha256": sha256(source),
            "output_profile_sha256": sha256(output),
            "changes": changes,
        }
        write_new_json(receipt, result)
    except BaseException:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        if output.exists() and not receipt.exists():
            output.unlink()
        raise


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
            select(arguments)
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

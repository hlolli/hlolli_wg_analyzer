#!/usr/bin/python3 -I
"""Falsify one frozen double-bass bowed-dynamics candidate."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import wave
from typing import Any, Mapping, Sequence

sys.dont_write_bytecode = True

ROSTER_SCHEMA = "hwa-double-bass-physical-dynamics-source-roster"
DECLARATION_SCHEMA = "hwa-double-bass-physical-dynamics-declaration"
RESULT_SCHEMA = "hwa-double-bass-physical-dynamics-result"
DYNAMICS = ("pp", "mf", "ff")
STRINGS = ("e", "a", "d", "g")
BAND_NAMES = (
    "60-120 Hz",
    "120-250 Hz",
    "250-500 Hz",
    "500-1000 Hz",
    "1-2 kHz",
    "2-4 kHz",
    "4-8 kHz",
)
MAX_JSON_BYTES = 4 * 1024 * 1024


class DynamicsError(ValueError):
    """A declaration, source, renderer, or analysis failed its contract."""


def reject_constant(value: str) -> Any:
    raise DynamicsError(f"non-finite JSON number: {value}")


def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DynamicsError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def load_json(path: Path, label: str) -> dict[str, Any]:
    try:
        if path.stat().st_size > MAX_JSON_BYTES:
            raise DynamicsError(f"{label} exceeds {MAX_JSON_BYTES} bytes")
        value = json.loads(
            path.read_text(encoding="utf-8"),
            parse_constant=reject_constant,
            object_pairs_hook=unique_object,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise DynamicsError(f"cannot read {label}: {error}") from error
    if not isinstance(value, dict):
        raise DynamicsError(f"{label} must be a JSON object")
    return value


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while block := stream.read(1024 * 1024):
                digest.update(block)
    except OSError as error:
        raise DynamicsError(f"cannot hash required file: {error}") from error
    return digest.hexdigest()


def finite_number(value: Any, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise DynamicsError(f"{label} must be a finite number")
    result = float(value)
    if not math.isfinite(result):
        raise DynamicsError(f"{label} must be a finite number")
    return result


def positive_number(value: Any, label: str) -> float:
    result = finite_number(value, label)
    if not result > 0.0:
        raise DynamicsError(f"{label} must be positive")
    return result


def safe_relative_path(value: Any, label: str) -> Path:
    if not isinstance(value, str) or not value:
        raise DynamicsError(f"{label} must be a nonempty relative path")
    result = Path(value)
    if result.is_absolute() or ".." in result.parts:
        raise DynamicsError(f"{label} must be a safe relative path")
    return result


def require_sha256(value: Any, label: str) -> str:
    if (not isinstance(value, str) or len(value) != 64
            or any(character not in "0123456789abcdef" for character in value)):
        raise DynamicsError(f"{label} must be a lowercase SHA-256")
    return value


def validate_roster(roster: Mapping[str, Any]) -> list[dict[str, Any]]:
    if roster.get("schema") != ROSTER_SCHEMA or roster.get("schema_version") != 1:
        raise DynamicsError("unsupported physical-dynamics source roster")
    if roster.get("status") != "frozen-before-pcm-analysis":
        raise DynamicsError("source roster was not frozen before PCM analysis")
    orchidea = roster.get("orchideasol")
    if not isinstance(orchidea, dict):
        raise DynamicsError("source roster has no OrchideaSOL object")
    if orchidea.get("license_spdx") != "CC-BY-4.0":
        raise DynamicsError("OrchideaSOL license is not pinned to CC-BY-4.0")
    regression = orchidea.get("dynamic_regression")
    if not isinstance(regression, dict):
        raise DynamicsError("source roster has no dynamic regression")
    if regression.get("coefficient_order") != ["1", "m", "d", "m*d", "m^2", "d^2"]:
        raise DynamicsError("unexpected dynamic-regression coefficient order")
    coefficients = regression.get("coefficients")
    if not isinstance(coefficients, list) or len(coefficients) != 6:
        raise DynamicsError("dynamic regression must have six coefficients")
    for index, value in enumerate(coefficients):
        finite_number(value, f"dynamic regression coefficient {index}")
    intensities = regression.get("dynamic_intensity")
    if not isinstance(intensities, dict):
        raise DynamicsError("dynamic regression has no intensity map")
    for dynamic in DYNAMICS:
        finite_number(intensities.get(dynamic), f"dynamic intensity {dynamic}")
    rows = orchidea.get("selected_ordinary_open_string_files")
    if not isinstance(rows, list) or len(rows) != len(STRINGS) * len(DYNAMICS):
        raise DynamicsError("source roster must contain exactly twelve selected files")
    result: list[dict[str, Any]] = []
    seen: set[tuple[str, str]] = set()
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            raise DynamicsError(f"selected source {index} must be an object")
        string_id = row.get("string_id")
        dynamic = row.get("dynamic")
        key = (string_id, dynamic)
        if string_id not in STRINGS or dynamic not in DYNAMICS or key in seen:
            raise DynamicsError(f"selected source {index} has invalid or duplicate identity")
        seen.add(key)
        safe_relative_path(row.get("relative_path"), f"selected source {index} path")
        require_sha256(row.get("sha256"), f"selected source {index} hash")
        if row.get("publisher_md5_list_match") is not True:
            raise DynamicsError(f"selected source {index} lacks publisher checksum proof")
        wave_facts = row.get("wave")
        if not isinstance(wave_facts, dict):
            raise DynamicsError(f"selected source {index} has no WAVE declaration")
        expected = {
            "sample_rate_hz": 44100,
            "channels": 1,
            "bits_per_sample": 24,
        }
        if any(wave_facts.get(name) != value for name, value in expected.items()):
            raise DynamicsError(f"selected source {index} has unexpected WAVE facts")
        positive_number(wave_facts.get("frame_count"), f"selected source {index} frames")
        positive_number(row.get("bytes"), f"selected source {index} bytes")
        positive_number(row.get("midi"), f"selected source {index} MIDI note")
        finite_number(
            row.get("dynamic_intensity"),
            f"selected source {index} intensity",
        )
        result.append(dict(row))
    if seen != {(string_id, dynamic) for string_id in STRINGS for dynamic in DYNAMICS}:
        raise DynamicsError("selected source roster is incomplete")
    return result


def validate_declaration(declaration: Mapping[str, Any]) -> None:
    if (declaration.get("schema") != DECLARATION_SCHEMA
            or declaration.get("schema_version") != 1):
        raise DynamicsError("unsupported physical-dynamics declaration")
    if declaration.get("status") != "frozen-before-orchideasol-analysis":
        raise DynamicsError("declaration was not frozen before OrchideaSOL analysis")
    require_sha256(declaration.get("source_roster_sha256"), "source roster hash")
    runtime = declaration.get("runtime")
    if not isinstance(runtime, dict):
        raise DynamicsError("declaration has no runtime object")
    require_sha256(runtime.get("source_sha256"), "runtime source hash")
    require_sha256(runtime.get("model_sha256"), "runtime model hash")
    controls = declaration.get("controls")
    if not isinstance(controls, dict):
        raise DynamicsError("declaration has no controls")
    if controls.get("articulation") != 0:
        raise DynamicsError("physical-dynamics candidate must use ordinary arco")
    force = finite_number(controls.get("force"), "force control")
    position = finite_number(controls.get("position"), "position control")
    if not 0.0 < force <= 1.0 or not 0.01 <= position <= 0.49:
        raise DynamicsError("force or position control is outside its runtime range")
    law = controls.get("speed_law")
    if not isinstance(law, dict) or law.get("reference_dynamic") != "mf":
        raise DynamicsError("speed law must use mf as its reference dynamic")
    positive_number(law.get("reference_speed_m_per_s"), "reference bow speed")
    positive_number(law.get("runtime_speed_scale_m_per_s"), "runtime speed scale")
    render = declaration.get("render")
    if not isinstance(render, dict):
        raise DynamicsError("declaration has no render object")
    if render.get("sample_rate_hz") != 44100:
        raise DynamicsError("candidate render must retain the source sample rate")
    duration = positive_number(render.get("duration_seconds"), "render duration")
    release = positive_number(render.get("release_seconds"), "render release")
    probe = positive_number(render.get("probe_seconds"), "render probe time")
    if not probe < release < duration:
        raise DynamicsError("render timing must satisfy probe < release < duration")
    gates = declaration.get("gates")
    if not isinstance(gates, dict):
        raise DynamicsError("declaration has no gates")
    for name in (
        "source_pitch_max_abs_cents",
        "model_pitch_max_abs_cents",
        "model_dynamic_level_max_error_db",
        "dynamic_spectrum_mean_max_error_db",
        "dynamic_spectrum_worst_max_error_db",
        "model_peak_max",
        "contact_scratch_max_fraction",
    ):
        positive_number(gates.get(name), f"gate {name}")
    if gates.get("require_strict_source_level_order") is not True:
        raise DynamicsError("source level ordering gate must be enabled")
    if gates.get("require_zero_contact_failures") is not True:
        raise DynamicsError("contact failure gate must be enabled")


def regression_level(roster: Mapping[str, Any], midi: float, dynamic: str) -> float:
    regression = roster["orchideasol"]["dynamic_regression"]
    b0, b1, b2, b3, b4, b5 = (
        float(value) for value in regression["coefficients"])
    d = float(regression["dynamic_intensity"][dynamic])
    return b0 + b1 * midi + b2 * d + b3 * midi * d + b4 * midi * midi + b5 * d * d


def dynamic_speed(roster: Mapping[str, Any], declaration: Mapping[str, Any], midi: float, dynamic: str) -> tuple[float, float]:
    law = declaration["controls"]["speed_law"]
    reference = law["reference_dynamic"]
    delta_db = regression_level(roster, midi, dynamic) - regression_level(roster, midi, reference)
    speed = float(law["reference_speed_m_per_s"]) * 10.0 ** (delta_db / 20.0)
    scale = float(law["runtime_speed_scale_m_per_s"])
    return speed, max(-1.0, min(1.0, speed / scale))


def wave_facts(path: Path) -> dict[str, int]:
    try:
        with wave.open(str(path), "rb") as stream:
            return {
                "sample_rate_hz": stream.getframerate(),
                "channels": stream.getnchannels(),
                "bits_per_sample": 8 * stream.getsampwidth(),
                "frame_count": stream.getnframes(),
            }
    except (OSError, wave.Error) as error:
        raise DynamicsError(f"cannot read selected WAVE: {error}") from error


def checked_json(command: Sequence[str], label: str, timeout: int = 120) -> dict[str, Any]:
    try:
        completed = subprocess.run(
            list(command), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, timeout=timeout, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise DynamicsError(f"cannot run {label}: {error}") from error
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise DynamicsError(f"{label} failed: {detail[-2000:]}")
    try:
        value = json.loads(completed.stdout, parse_constant=reject_constant)
    except json.JSONDecodeError as error:
        raise DynamicsError(f"{label} returned invalid JSON: {error}") from error
    if not isinstance(value, dict):
        raise DynamicsError(f"{label} did not return a JSON object")
    return value


def inspect_audio(analyzer: Path, path: Path, expected_hz: float) -> dict[str, Any]:
    inspected = checked_json([str(analyzer), "--json", "inspect", str(path)], "audio inspection")
    pitched = checked_json([
        str(analyzer), "--json", "isolated-note", str(path),
        "--expected-hz", format(expected_hz, ".17g"), "--metrics", "pitch",
    ], "pitch inspection")
    file_result = inspected.get("file")
    if not isinstance(file_result, dict):
        raise DynamicsError("audio inspection has no file result")
    channels = file_result.get("channels")
    loudness = file_result.get("loudness")
    spectrum = file_result.get("spectrum")
    pitch = pitched.get("pitch")
    if (not isinstance(channels, list) or len(channels) != 1
            or not isinstance(loudness, dict)
            or not isinstance(spectrum, dict)
            or not isinstance(pitch, dict)):
        raise DynamicsError("audio inspection omitted required mono measurements")
    bands = spectrum.get("band_power")
    if not isinstance(bands, list):
        raise DynamicsError("audio inspection omitted spectral bands")
    powers = {row.get("name"): finite_number(row.get("value"), "band power")
              for row in bands if isinstance(row, dict)}
    if any(name not in powers for name in BAND_NAMES):
        raise DynamicsError("audio inspection omitted a required spectral band")
    selected_total = sum(max(0.0, powers[name]) for name in BAND_NAMES)
    if not selected_total > 0.0:
        raise DynamicsError("selected spectral bands have no power")
    return {
        "peak": finite_number(channels[0].get("peak"), "audio peak"),
        "rms_dbfs": 20.0 * math.log10(max(
            finite_number(channels[0].get("rms"), "audio RMS"), 1.0e-15)),
        "short_term_max_lufs": finite_number(
            loudness.get("short_term_max_lufs"), "short-term loudness"),
        "pitch_valid": pitch.get("valid") is True,
        "pitch_cents": finite_number(pitch.get("cents"), "pitch cents"),
        "spectrum_db": {
            name: 10.0 * math.log10(max(powers[name] / selected_total, 1.0e-15))
            for name in BAND_NAMES
        },
    }


def render_csd(row: Mapping[str, Any], declaration: Mapping[str, Any], speed_control: float) -> str:
    controls = declaration["controls"]
    render = declaration["render"]
    duration = float(render["duration_seconds"])
    return f"""<CsoundSynthesizer>
<CsOptions>
-d -m128
</CsOptions>
<CsInstruments>
sr = {int(render['sample_rate_hz'])}
ksmps = 32
nchnls = 1
0dbfs = 1

giBass hlolli_wg_double_bass_create

instr Voice
  kGate = timeinsts() < {float(render['release_seconds']):.17g} ? 1 : 0
  aLeft, aRight hlolli_wg_double_bass kGate, {float(row['expected_hz']):.17g}, \
      {float(controls['force']):.17g}, {speed_control:.17g}, \
      {float(controls['position']):.17g}, 0, 0, 0, 0, 0, \
      {int(row['string_number'])}, giBass
  out 0.5 * (aLeft + aRight)
endin

instr Probe
  iB01, iRequested, iEffective, iBowSpeed, iContact, iB06, iB07, iB08, \
      iB09, iB10, iB11, iB12, iB13, iB14, iB15, iMinForce, iMaxForce, \
      iB18, iB19, iB20, iB21, iB22, iSolverCalls, iSolverFailures, \
      iSolverFallbacks, iStick, iSlip, iScratch, iNoMotion, iB30, iB31, \
      iB32, iRecoveries, iScratchScore, iB35, iFinite \
      hlolli_wg_double_bass_test_bow giBass, {int(row['string_number'])}
  printf_i "WG_DYNAMICS_CONTACT %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g\\n", \
      1, iRequested, iEffective, iBowSpeed, iContact, iMinForce, iMaxForce, \
      iSolverCalls, iSolverFailures, iSolverFallbacks, iStick, iSlip, \
      iScratch, iNoMotion, iRecoveries, iScratchScore, iFinite
endin
</CsInstruments>
<CsScore>
i "Voice" 0 {duration:.17g}
i "Probe" {float(render['probe_seconds']):.17g} 0.001
e
</CsScore>
</CsoundSynthesizer>
""".replace("\n+", "\n")


def run_render(csound: Path, module: Path, csd: Path, output: Path) -> dict[str, float]:
    command = [
        str(csound), "--opcode-lib=" + str(module), "--sample-accurate",
        "--num-threads=1", "-W", "-3", "--nopeaks", "-d", "-m128",
        "-o", str(output), str(csd),
    ]
    try:
        completed = subprocess.run(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=120, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise DynamicsError(f"cannot run Csound candidate render: {error}") from error
    if completed.returncode != 0 or not output.is_file():
        raise DynamicsError(f"Csound candidate render failed: {completed.stdout[-2000:]}")
    marker = "WG_DYNAMICS_CONTACT "
    rows = [line[line.index(marker) + len(marker):].split()
            for line in completed.stdout.splitlines() if marker in line]
    if len(rows) != 1 or len(rows[0]) != 16:
        raise DynamicsError("Csound candidate render returned malformed contact state")
    names = (
        "requested_force", "effective_force", "bow_speed", "contact",
        "minimum_force", "maximum_force", "solver_calls", "solver_failures",
        "solver_fallbacks", "stick_samples", "slip_samples", "scratch_samples",
        "no_motion_samples", "recoveries", "scratch_score", "finite",
    )
    return dict(zip(names, (float(value) for value in rows[0])))


def spectrum_delta(left: Mapping[str, float], right: Mapping[str, float]) -> dict[str, float]:
    return {name: float(left[name]) - float(right[name]) for name in BAND_NAMES}


def evaluate(rows: Sequence[Mapping[str, Any]], declaration: Mapping[str, Any]) -> tuple[list[dict[str, Any]], list[str]]:
    gates = declaration["gates"]
    summaries: list[dict[str, Any]] = []
    failures: list[str] = []
    by_key = {(row["string_id"], row["dynamic"]): row for row in rows}
    for string_id in STRINGS:
        group = {dynamic: by_key[(string_id, dynamic)] for dynamic in DYNAMICS}
        source_levels = {dynamic: group[dynamic]["source"]["short_term_max_lufs"] for dynamic in DYNAMICS}
        if not source_levels["pp"] < source_levels["mf"] < source_levels["ff"]:
            failures.append(f"{string_id}: source levels are not strictly pp < mf < ff")
        pitch_limit = float(gates["source_pitch_max_abs_cents"])
        for dynamic in DYNAMICS:
            source = group[dynamic]["source"]
            model = group[dynamic]["model"]
            contact = group[dynamic]["contact"]
            if not source["pitch_valid"] or abs(source["pitch_cents"]) > pitch_limit:
                failures.append(f"{string_id}-{dynamic}: source pitch failed")
            if (not model["pitch_valid"]
                    or abs(model["pitch_cents"]) > float(gates["model_pitch_max_abs_cents"])):
                failures.append(f"{string_id}-{dynamic}: model pitch failed")
            if model["peak"] >= float(gates["model_peak_max"]):
                failures.append(f"{string_id}-{dynamic}: model peak failed")
            state_total = sum(contact[name] for name in (
                "stick_samples", "slip_samples", "scratch_samples", "no_motion_samples"))
            scratch_fraction = contact["scratch_samples"] / max(1.0, state_total)
            contact["scratch_fraction"] = scratch_fraction
            if (contact["finite"] != 1.0
                    or contact["solver_failures"] != 0.0
                    or contact["solver_fallbacks"] != 0.0
                    or contact["recoveries"] != 0.0
                    or scratch_fraction > float(gates["contact_scratch_max_fraction"])):
                failures.append(f"{string_id}-{dynamic}: contact-state gate failed")
        level_errors: dict[str, float] = {}
        spectrum_errors: dict[str, dict[str, float]] = {}
        for dynamic in ("pp", "ff"):
            target_delta = group[dynamic]["target_level_db"] - group["mf"]["target_level_db"]
            model_delta = (group[dynamic]["model"]["short_term_max_lufs"]
                           - group["mf"]["model"]["short_term_max_lufs"])
            level_error = model_delta - target_delta
            level_errors[dynamic] = level_error
            if abs(level_error) > float(gates["model_dynamic_level_max_error_db"]):
                failures.append(f"{string_id}-{dynamic}: dynamic-level error failed")
            source_change = spectrum_delta(
                group[dynamic]["source"]["spectrum_db"],
                group["mf"]["source"]["spectrum_db"])
            model_change = spectrum_delta(
                group[dynamic]["model"]["spectrum_db"],
                group["mf"]["model"]["spectrum_db"])
            errors = [model_change[name] - source_change[name] for name in BAND_NAMES]
            mean_error = sum(abs(value) for value in errors) / len(errors)
            worst_error = max(abs(value) for value in errors)
            spectrum_errors[dynamic] = {
                "mean_abs_db": mean_error,
                "worst_abs_db": worst_error,
            }
            if (mean_error > float(gates["dynamic_spectrum_mean_max_error_db"])
                    or worst_error > float(gates["dynamic_spectrum_worst_max_error_db"])):
                failures.append(f"{string_id}-{dynamic}: dynamic-spectrum error failed")
        summaries.append({
            "string_id": string_id,
            "source_level_lufs": source_levels,
            "model_dynamic_level_error_db": level_errors,
            "dynamic_spectrum_error": spectrum_errors,
        })
    return summaries, failures


def run(arguments: argparse.Namespace) -> int:
    for name in ("declaration", "roster", "source_root", "runtime_source", "model", "csound", "module", "analyzer", "output"):
        path = getattr(arguments, name)
        if not path.is_absolute():
            raise DynamicsError(f"--{name.replace('_', '-')} must be absolute")
    if arguments.output.exists():
        raise DynamicsError("output already exists")
    declaration = load_json(arguments.declaration, "declaration")
    roster = load_json(arguments.roster, "source roster")
    validate_declaration(declaration)
    roster_rows = validate_roster(roster)
    if sha256(arguments.roster) != declaration["source_roster_sha256"]:
        raise DynamicsError("source roster hash does not match declaration")
    if sha256(arguments.runtime_source) != declaration["runtime"]["source_sha256"]:
        raise DynamicsError("runtime source hash does not match declaration")
    if sha256(arguments.model) != declaration["runtime"]["model_sha256"]:
        raise DynamicsError("runtime model hash does not match declaration")
    for path, label in ((arguments.csound, "Csound"), (arguments.module, "test module"), (arguments.analyzer, "analyzer")):
        if not path.is_file():
            raise DynamicsError(f"{label} is not a regular file")
    string_numbers = {string_id: index + 1 for index, string_id in enumerate(STRINGS)}
    measured: list[dict[str, Any]] = []
    with tempfile.TemporaryDirectory(prefix="hwa-double-bass-dynamics-", dir=arguments.output.parent) as folder:
        temporary = Path(folder)
        for index, source_row in enumerate(roster_rows):
            source = arguments.source_root / safe_relative_path(source_row["relative_path"], "source path")
            if not source.is_file():
                raise DynamicsError(f"selected source {index} is missing")
            if source.stat().st_size != source_row["bytes"] or sha256(source) != source_row["sha256"]:
                raise DynamicsError(f"selected source {index} failed byte/hash preflight")
            if wave_facts(source) != source_row["wave"]:
                raise DynamicsError(f"selected source {index} failed WAVE preflight")
            expected_hz = 440.0 * 2.0 ** ((float(source_row["midi"]) - 69.0) / 12.0)
            speed_m_per_s, speed_control = dynamic_speed(
                roster, declaration, float(source_row["midi"]), source_row["dynamic"])
            row = {
                "id": f"{source_row['string_id']}-{source_row['dynamic']}",
                "string_id": source_row["string_id"],
                "string_number": string_numbers[source_row["string_id"]],
                "dynamic": source_row["dynamic"],
                "midi": source_row["midi"],
                "expected_hz": expected_hz,
                "source_sha256": source_row["sha256"],
                "target_level_db": regression_level(
                    roster, float(source_row["midi"]), source_row["dynamic"]),
                "bow_speed_m_per_s": speed_m_per_s,
                "speed_control": speed_control,
            }
            row["source"] = inspect_audio(arguments.analyzer, source, expected_hz)
            csd = temporary / f"{index:02d}.csd"
            output = temporary / f"{index:02d}.wav"
            csd.write_text(render_csd(row, declaration, speed_control), encoding="utf-8")
            row["contact"] = run_render(arguments.csound, arguments.module, csd, output)
            row["model"] = inspect_audio(arguments.analyzer, output, expected_hz)
            measured.append(row)
    summaries, failures = evaluate(measured, declaration)
    result = {
        "schema": RESULT_SCHEMA,
        "schema_version": 1,
        "status": "pass" if not failures else "rejected",
        "evidence_role": "development-falsification-not-independent-audit",
        "inputs": {
            "declaration_sha256": sha256(arguments.declaration),
            "source_roster_sha256": sha256(arguments.roster),
            "runtime_source_sha256": sha256(arguments.runtime_source),
            "model_sha256": sha256(arguments.model),
            "module_sha256": sha256(arguments.module),
            "analyzer_sha256": sha256(arguments.analyzer),
        },
        "controls": declaration["controls"],
        "gates": declaration["gates"],
        "rows": measured,
        "strings": summaries,
        "failures": failures,
    }
    arguments.output.write_text(
        json.dumps(result, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8")
    print(json.dumps({
        "schema": RESULT_SCHEMA,
        "status": result["status"],
        "row_count": len(measured),
        "failure_count": len(failures),
        "output_sha256": sha256(arguments.output),
    }, sort_keys=True, separators=(",", ":")))
    return 0 if not failures else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--declaration", required=True, type=Path)
    parser.add_argument("--roster", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--runtime-source", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--csound", required=True, type=Path)
    parser.add_argument("--module", required=True, type=Path)
    parser.add_argument("--analyzer", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    arguments = parser.parse_args()
    try:
        return run(arguments)
    except DynamicsError as error:
        print(f"physical-dynamics: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

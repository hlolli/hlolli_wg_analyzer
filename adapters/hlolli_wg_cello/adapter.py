#!/usr/bin/env python3
"""Build and run the checked renderer for the fixed cello model."""

import argparse
from decimal import Decimal
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
import wave


FROZEN_CONFIG = None

BUILD_TYPE = "Release"
MODEL_FRAMES = 529200
SOURCE_DIR = Path(__file__).resolve().parent
FIT_SELECTOR = SOURCE_DIR.parents[1] / "tools" / "instrument_fit.py"
LEVELS = [0.08, 0.125, 0.25, 0.5, 0.75, 1.0, 1.5]
JOINT_ADAPTER_ID = "hlolli-wg-cello-passive-joint-v1"
CLEAN_PATH = "/run/current-system/sw/bin:/usr/bin:/bin"
JOINT_PARAMETER = {
    "id": "joint_candidate",
    "unit": "choice",
    "minimum": 0.0,
    "maximum": 1.0,
    "baseline": 0.0,
    "levels": [0.0, 1.0],
}
SELECTION_METHOD_VERSION = "instrument-fit-selection-v1"
PASSIVE_DECAY_METHOD_VERSION = "passive-decay-v4"
PASSIVE_DECAY_SHAPE_METHOD_VERSION = "passive-decay-shape-v1"
HARMONIC_DECAY_METHOD_VERSION = "harmonic-decay-v1"
CORPUS_PLAN_SCHEMA = "hwa-cello-passive-corpus-plan"
CORPUS_BUNDLE_SCHEMA = "hwa-cello-corpus-fit-adapter-bundle"
NOMINAL_OPEN_FREQUENCIES = {
    "c2": 65.40639132514966,
    "g2": 97.99885899543733,
    "d3": 146.8323839587038,
    "a3": 220.0,
}
STRING_SPECS = {
    "c2": {
        "adapter_id": "hlolli-wg-cello-c2-passive-v1",
        "case_prefix": "c2-pizz",
        "fit_manifest": SOURCE_DIR / "fit.json",
        "input_prefix": "reference_c2",
        "parameter_id": "loss_time_constant_c_seconds",
        "profile_index": 0,
        "render_a4": 436.71734722436616,
        "render_frequency": 64.91842207054125,
        "render_string": 1,
    },
    "g2": {
        "adapter_id": "hlolli-wg-cello-g2-passive-v1",
        "case_prefix": "g2-pizz",
        "fit_manifest": SOURCE_DIR / "fit-g2.json",
        "input_prefix": "reference_g2",
        "levels": [
            0.08, 0.125, 0.25, 0.5, 0.75, 1.0,
            1.2, 1.25, 1.3, 1.35, 1.4, 1.45, 1.5,
            1.6, 1.75, 2.0, 2.25, 2.5, 3.0,
        ],
        "maximum": 3.0,
        "parameter_id": "loss_time_constant_g_seconds",
        "profile_index": 1,
        "render_a4": 435.32549817151545,
        "render_frequency": 96.95773207326926,
        "render_string": 2,
    },
    "d3": {
        "adapter_id": "hlolli-wg-cello-d3-passive-v1",
        "case_prefix": "d3-pizz",
        "fit_manifest": SOURCE_DIR / "fit-d3.json",
        "input_prefix": "reference_d3",
        "parameter_id": "loss_time_constant_d_seconds",
        "profile_index": 2,
        "render_a4": 433.643929788049,
        "render_frequency": 144.7113,
        "render_string": 3,
    },
    "a3": {
        "adapter_id": "hlolli-wg-cello-a3-passive-v1",
        "case_prefix": "a3-pizz",
        "fit_manifest": SOURCE_DIR / "fit-a3.json",
        "input_prefix": "reference_a3",
        "parameter_id": "loss_time_constant_a_seconds",
        "profile_index": 3,
        "render_a4": 440.35616873782317,
        "render_frequency": 220.17808436891158,
        "render_string": 4,
    },
}

SHAPE_SPECS = {
    "c2": {
        "adapter_id": "hlolli-wg-cello-c2-frequency-loss-v1",
        "fit_manifest": SOURCE_DIR / "fit-c2-frequency-loss.json",
        "parameters": [
            {
                "id": "bridge_cutoff_c_hz", "unit": "hertz",
                "baseline": 5386.995271806526,
                "levels": [1800.0, 3500.0, 5386.995271806526],
                "profile_key": "bridge_cutoff_hz",
            },
            {
                "id": "loss_time_constant_c_seconds", "unit": "seconds",
                "baseline": 0.25,
                "levels": [0.25, 0.75, 1.0, 1.25],
                "profile_key": "loss_time_constant_seconds",
            },
            {
                "id": "nut_cutoff_c_hz", "unit": "hertz",
                "baseline": 20000.0,
                "levels": [1500.0, 3000.0, 6000.0, 20000.0],
                "profile_key": "nut_cutoff_hz",
            },
        ],
        "fit_frequency": 64.91842207054125,
        "check_frequency": 65.48199107496103,
        "check_weight": 0.0,
    },
    "g2": {
        "adapter_id": "hlolli-wg-cello-g2-passive-bridge-peak-v1",
        "fit_manifest": SOURCE_DIR / "fit-g2-bridge-peak.json",
        "parameters": [
            {
                "id": "bridge_cutoff_g_hz", "unit": "hertz",
                "baseline": 4964.244281108786,
                "levels": [4964.244281108786, 18000.0],
                "profile_key": "bridge_cutoff_hz",
            },
            {
                "id": "bridge_loss_peak_bandwidth_g_hz", "unit": "hertz",
                "baseline": 200.0,
                "levels": [50.0, 100.0, 200.0],
                "profile_key": "bridge_loss_peak_bandwidth_hz",
            },
            {
                "id": "bridge_loss_peak_g_fraction",
                "unit": "ratio", "baseline": 0.0,
                "levels": [0.0, 0.02, 0.03, 0.04],
                "profile_key": "bridge_loss_peak_fraction",
            },
            {
                "id": "loss_time_constant_g_seconds", "unit": "seconds",
                "baseline": 0.25,
                "levels": [0.25, 2.0, 2.5, 3.0],
                "profile_key": "loss_time_constant_seconds",
            },
        ],
        "fit_frequency": 96.95773207326926,
        "check_frequency": 97.5819,
    },
    "d3": {
        "adapter_id": "hlolli-wg-cello-d3-frequency-loss-v2",
        "fit_manifest": SOURCE_DIR / "fit-d3-frequency-loss.json",
        "parameters": [
            {
                "id": "bridge_cutoff_d_hz", "unit": "hertz",
                "baseline": 7086.471045764144,
                "levels": [3500.0, 7086.471045764144],
                "profile_key": "bridge_cutoff_hz",
            },
            {
                "id": "loss_time_constant_d_seconds", "unit": "seconds",
                "baseline": 0.25,
                "levels": [0.25, 0.5, 0.75, 1.0],
                "profile_key": "loss_time_constant_seconds",
            },
            {
                "id": "nut_cutoff_d_hz", "unit": "hertz",
                "baseline": 12000.0,
                "levels": [4000.0, 8000.0, 12000.0],
                "profile_key": "nut_cutoff_hz",
            },
        ],
        "fit_frequency": 146.7681298,
        "check_frequency": 146.38423966606234,
        "check_weight": 0.0,
    },
}


CORPUS_PARAMETER_SPECS = {
    "c2": {
        "adapter_id": "hlolli-wg-cello-c2-high-shelf-v1",
        "parameters": [
            {
                "id": "bridge_high_shelf_cutoff_c_hz", "unit": "hertz",
                "baseline": 200.0,
                "levels": [100.0, 200.0, 400.0],
                "profile_key": "bridge_high_shelf_cutoff_hz",
            },
            {
                "id": "bridge_high_shelf_loss_c_fraction", "unit": "ratio",
                "baseline": 0.0,
                "levels": [0.0, 0.00125, 0.0025, 0.005],
                "profile_key": "bridge_high_shelf_loss_fraction",
            },
            {
                "id": "loss_time_constant_c_seconds", "unit": "seconds",
                "baseline": 0.25,
                "levels": [0.25, 1.25, 1.5, 2.0],
                "profile_key": "loss_time_constant_seconds",
            },
            {
                "id": "nut_cutoff_c_hz", "unit": "hertz",
                "baseline": 20000.0,
                "levels": [6000.0, 20000.0],
                "profile_key": "nut_cutoff_hz",
            },
        ],
    },
    "a3": {
        "adapter_id": "hlolli-wg-cello-a3-frequency-loss-v1",
        "parameters": [
            {
                "id": "bridge_cutoff_a_hz", "unit": "hertz",
                "baseline": 6024.580442039031,
                "levels": [6024.580442039031, 12000.0, 18000.0],
                "profile_key": "bridge_cutoff_hz",
            },
            {
                "id": "loss_time_constant_a_seconds", "unit": "seconds",
                "baseline": 0.25,
                "levels": [0.25, 1.0, 1.5, 2.0],
                "profile_key": "loss_time_constant_seconds",
            },
            {
                "id": "nut_cutoff_a_hz", "unit": "hertz",
                "baseline": 12000.0,
                "levels": [12000.0, 20000.0],
                "profile_key": "nut_cutoff_hz",
            },
        ],
    },
}


def parameter_for(spec):
    return {
        "id": spec["parameter_id"],
        "unit": "seconds",
        "minimum": 0.08,
        "maximum": spec.get("maximum", 1.5),
        "baseline": 0.25,
        "levels": list(spec.get("levels", LEVELS)),
    }


def shape_parameters(target):
    spec = STRING_SPECS[target]
    shape = SHAPE_SPECS[target]
    if "parameters" in shape:
        rows = [
            {
                **row,
                "minimum": min(row["levels"]),
                "maximum": max(row["levels"]),
                "levels": list(row["levels"]),
            }
            for row in shape["parameters"]
        ]
        return sorted(rows, key=lambda row: row["id"])
    letter = target[0]
    rows = [
        {
            "id": "bridge_cutoff_{}_hz".format(letter),
            "unit": "hertz",
            "minimum": min(shape["bridge_cutoff_levels"]),
            "maximum": max(shape["bridge_cutoff_levels"]),
            "baseline": shape["bridge_cutoff_levels"][0]
                if target == "g2" else shape["bridge_cutoff_levels"][-1],
            "levels": list(shape["bridge_cutoff_levels"]),
            "profile_key": "bridge_cutoff_hz",
        },
        {
            "id": spec["parameter_id"],
            "unit": "seconds",
            "minimum": min(shape["tau_levels"]),
            "maximum": max(shape["tau_levels"]),
            "baseline": 0.25,
            "levels": list(shape["tau_levels"]),
            "profile_key": "loss_time_constant_seconds",
        },
        {
            "id": "nut_cutoff_{}_hz".format(letter),
            "unit": "hertz",
            "minimum": min(shape["nut_cutoff_levels"]),
            "maximum": max(shape["nut_cutoff_levels"]),
            "baseline": 4200.0 if target == "g2" else 12000.0,
            "levels": list(shape["nut_cutoff_levels"]),
            "profile_key": "nut_cutoff_hz",
        },
    ]
    return rows


def public_parameter(row):
    return {name: row[name] for name in (
        "id", "unit", "minimum", "maximum", "baseline", "levels"
    )}


def corpus_parameter_rows(target):
    if target in CORPUS_PARAMETER_SPECS:
        rows = [
            {
                **row,
                "minimum": min(row["levels"]),
                "maximum": max(row["levels"]),
                "levels": list(row["levels"]),
            }
            for row in CORPUS_PARAMETER_SPECS[target]["parameters"]
        ]
        return sorted(rows, key=lambda row: row["id"])
    if target in SHAPE_SPECS:
        return shape_parameters(target)
    return [{
        **parameter_for(STRING_SPECS[target]),
        "profile_key": "loss_time_constant_seconds",
    }]


def corpus_adapter_id(target):
    if target in CORPUS_PARAMETER_SPECS:
        base = CORPUS_PARAMETER_SPECS[target]["adapter_id"]
    else:
        base = (SHAPE_SPECS[target]["adapter_id"]
                if target in SHAPE_SPECS else
                STRING_SPECS[target]["adapter_id"])
    return base + "-corpus-v1"


class AdapterError(ValueError):
    pass


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def regular(path, name):
    path = Path(path).absolute()
    if not path.is_file() or path.is_symlink():
        raise AdapterError("{} must be a regular file: {}".format(name, path))
    return path


def directory(path, name):
    path = Path(path).resolve()
    if not path.is_dir() or path.is_symlink():
        raise AdapterError("{} must be a directory: {}".format(name, path))
    return path


def object_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise AdapterError("duplicate JSON key: {}".format(key))
        result[key] = value
    return result


def load_json(path):
    try:
        value = json.loads(Path(path).read_text(encoding="utf-8"),
                           object_pairs_hook=object_pairs)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise AdapterError("cannot read JSON: {}: {}".format(path, error))
    if type(value) is not dict:
        raise AdapterError("JSON root must be an object: {}".format(path))
    return value


def wave_facts(path):
    try:
        with Path(path).open("rb") as stream:
            header = stream.read(12)
            if (len(header) != 12 or header[:4] != b"RIFF" or
                    header[8:12] != b"WAVE"):
                raise AdapterError("invalid PCM WAVE header: {}".format(path))
            file_size = os.fstat(stream.fileno()).st_size
            declared_size = int.from_bytes(header[4:8], "little") + 8
            if declared_size < 12 or declared_size > file_size:
                raise AdapterError("truncated PCM WAVE RIFF: {}".format(path))
            format_chunk = None
            data_size = None
            while stream.tell() < declared_size:
                if declared_size - stream.tell() < 8:
                    raise AdapterError("truncated WAVE chunk: {}".format(path))
                chunk = stream.read(8)
                if len(chunk) != 8:
                    raise AdapterError("truncated WAVE chunk: {}".format(path))
                name = chunk[:4]
                size = int.from_bytes(chunk[4:8], "little")
                start = stream.tell()
                end = start + size
                padded_end = end + (size & 1)
                if end > declared_size or padded_end > declared_size or padded_end > file_size:
                    raise AdapterError("truncated WAVE chunk: {}".format(path))
                if name == b"fmt " and format_chunk is None:
                    if size > 4096:
                        raise AdapterError("oversized WAVE format: {}".format(path))
                    format_chunk = stream.read(size)
                    if len(format_chunk) != size:
                        raise AdapterError("truncated WAVE format: {}".format(path))
                elif name == b"data" and data_size is None:
                    data_size = size
                stream.seek(padded_end, os.SEEK_SET)
    except OSError as error:
        raise AdapterError("cannot read PCM WAVE: {}: {}".format(path, error))
    if format_chunk is None or data_size is None or len(format_chunk) < 16:
        raise AdapterError("PCM WAVE lacks format or data: {}".format(path))
    format_tag = int.from_bytes(format_chunk[0:2], "little")
    channels = int.from_bytes(format_chunk[2:4], "little")
    rate = int.from_bytes(format_chunk[4:8], "little")
    block_align = int.from_bytes(format_chunk[12:14], "little")
    bits = int.from_bytes(format_chunk[14:16], "little")
    if format_tag == 0xfffe:
        pcm_guid = bytes.fromhex("0100000000001000800000aa00389b71")
        if (len(format_chunk) < 40 or
                int.from_bytes(format_chunk[16:18], "little") < 22 or
                format_chunk[24:40] != pcm_guid):
            raise AdapterError("WAVE extensible input is not PCM: {}".format(path))
    elif format_tag != 1:
        raise AdapterError("WAVE input is not integer PCM: {}".format(path))
    width = bits // 8
    if bits not in (16, 24):
        raise AdapterError("PCM WAVE must be 16-bit or 24-bit: {}".format(path))
    if (channels < 1 or rate < 1 or
            block_align != channels * width or data_size % block_align):
        raise AdapterError("invalid PCM WAVE layout: {}".format(path))
    return rate, channels, data_size // block_align


def stem(resource_id, side, input_id, output, channels=2):
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


def build_experiment(references, dependencies, spec=None):
    if spec is None:
        spec = STRING_SPECS["c2"]
    input_prefix = spec["input_prefix"]
    case_prefix = spec["case_prefix"]
    expected = {input_prefix + "_fit", input_prefix + "_check"}
    if set(references) != expected:
        raise AdapterError("reference set must contain the target fit and check recordings")
    checked_references = {
        name: regular(path, name) for name, path in references.items()
    }
    reference_hashes = {name: sha256(path)
                        for name, path in checked_references.items()}
    if len(set(reference_hashes.values())) != len(reference_hashes):
        raise AdapterError("fit and check recordings must have distinct hashes")
    reference_channels = {}
    for name, path in checked_references.items():
        rate, channels, frames = wave_facts(path)
        if rate != 44100 or channels not in (1, 2) or frames < 1:
            raise AdapterError(
                "{} must be nonempty mono or stereo 44.1 kHz PCM WAVE".format(
                    name))
        reference_channels[name] = channels

    inputs = dict(checked_references)
    for name, path in dependencies.items():
        if name in inputs:
            raise AdapterError("duplicate input id: {}".format(name))
        inputs[name] = regular(path, name)

    cases = []
    for case_id, split, input_id in (
            (case_prefix + "-check", "check", input_prefix + "_check"),
            (case_prefix + "-fit", "fit", input_prefix + "_fit")):
        cases.append({
            "id": case_id,
            "split": split,
            "weight": 1,
            "stems": [
                stem("model.final", "model", None, "model.wav"),
                stem("reference.final", "reference", input_id, None,
                     reference_channels[input_id]),
            ],
            "probes": [],
            "links": [],
        })

    responses = [
        {"id": "final.band.120-250", "role": "final",
         "feature": "band_level_dbfs", "index": 2},
        {"id": "final.band.250-500", "role": "final",
         "feature": "band_level_dbfs", "index": 3},
        {"id": "final.band.60-120", "role": "final",
         "feature": "band_level_dbfs", "index": 1},
        {"id": "final.rms", "role": "final",
         "feature": "rms_dbfs", "index": 0},
    ]
    return {
        "schema": "hwa-experiment",
        "schema_version": 1,
        "method_version": "stage8-1",
        "clock_rate_hz": 44100,
        "inputs": [
            {"id": name, "sha256": sha256(path)}
            for name, path in sorted(inputs.items())
        ],
        "parameters": [parameter_for(spec)],
        "plan": {
            "kind": "one-at-a-time",
            "seed": 1007,
            "sample_count": 0,
            "replicates": 1,
        },
        "cases": cases,
        "responses": responses,
    }


def build_shape_experiment(references, dependencies, target):
    spec = STRING_SPECS[target]
    experiment = build_experiment(references, dependencies, spec)
    experiment["parameters"] = [
        public_parameter(row) for row in shape_parameters(target)
    ]
    experiment["plan"] = {
        "kind": "grid", "seed": 1007,
        "sample_count": 0, "replicates": 1,
    }
    return experiment


def corpus_fit_manifest(target, rows):
    spec = STRING_SPECS[target]
    parameters = corpus_parameter_rows(target)
    counts = {}
    for row in rows:
        key = (row["split"], row["source_id"])
        counts[key] = counts.get(key, 0) + 1
    objectives = []
    for row in rows:
        prefix = "{}_{}_{}_{}".format(
            row["split"], target, row["source_id"], row["dynamic"]
        )
        weight = 1.0 / counts[(row["split"], row["source_id"])]
        common = {
            "case": row["case_id"],
            "reference_binding": row["binding_id"],
            "resource_id": "model.final",
            "source_group": row["source_id"],
            "split": row["split"],
            "weight": weight,
        }
        objectives.extend([
            {
                **common,
                "id": prefix + "_passive_decay",
                "kind": "passive-decay",
                "scale": 3.0,
            },
            {
                **common,
                "fundamental_hz": row["fundamental_hz"],
                "harmonic_count": 8,
                "id": prefix + "_harmonic_decay",
                "kind": "harmonic-decay",
                "scale": 0.5,
            },
        ])
    return {
        "schema": "hwa-instrument-fit",
        "schema_version": 1,
        "adapter_id": corpus_adapter_id(target),
        "parameters": [{
            "id": row["id"],
            "unit": row["unit"],
            "minimum": row["minimum"],
            "maximum": row["maximum"],
            "baseline": row["baseline"],
            "profile_paths": [[
                "strings", spec["profile_index"], row["profile_key"]
            ]],
        } for row in parameters],
        "objectives": sorted(objectives, key=lambda row: row["id"]),
        "selection": {
            "check_weight": 1.0,
            "max_candidate_harmonic_maximum_error_octaves": 3.0,
            "max_candidate_harmonic_mean_error_octaves": 1.5,
            "max_candidate_loss": 16.0,
            "max_candidate_source_mean_loss": 3.0,
            "max_candidate_worst_harm": 0.25,
            "max_check_loss_increase": 0.0,
            "max_source_mean_loss_increase": 0.25,
            "maximum_candidate_t60_ratio": 3.0,
            "minimum_candidate_support_ratio": 1.0 / 3.0,
            "minimum_candidate_t60_ratio": 1.0 / 3.0,
        },
    }


def build_corpus_experiment(rows, dependencies, target):
    inputs = {row["binding_id"]: row["path"] for row in rows}
    for name, path in dependencies.items():
        if name in inputs:
            raise AdapterError("duplicate input id: {}".format(name))
        inputs[name] = regular(path, name)
    cases = {}
    grouped = {}
    for row in rows:
        grouped.setdefault((row["split"], row["source_id"]), []).append(row)
    nominal = NOMINAL_OPEN_FREQUENCIES[target]
    for (split, source_id), source_rows in sorted(grouped.items()):
        source_rows.sort(key=lambda row: (row["dynamic"], row["id"]))
        frequencies = sorted(row["fundamental_hz"] for row in source_rows)
        middle = len(frequencies) // 2
        frequency = (frequencies[middle] if len(frequencies) % 2 else
                     0.5 * (frequencies[middle - 1] + frequencies[middle]))
        case_id = source_rows[0]["case_id"]
        anchor = source_rows[0]
        cases[case_id] = {
            "id": case_id,
            "split": split,
            "weight": 1,
            "stems": [
                stem("model.final", "model", None, "model.wav"),
                stem("reference.final", "reference", anchor["binding_id"],
                     None, anchor["channels"]),
            ],
            "probes": [],
            "links": [],
        }
        render = {
            "a4": 440.0 * frequency / nominal,
            "frames": MODEL_FRAMES,
            "frequency": frequency,
            "string": STRING_SPECS[target]["render_string"],
        }
        for row in source_rows:
            row["render"] = render
    responses = [
        {"id": "final.band.120-250", "role": "final",
         "feature": "band_level_dbfs", "index": 2},
        {"id": "final.band.250-500", "role": "final",
         "feature": "band_level_dbfs", "index": 3},
        {"id": "final.band.60-120", "role": "final",
         "feature": "band_level_dbfs", "index": 1},
        {"id": "final.rms", "role": "final",
         "feature": "rms_dbfs", "index": 0},
    ]
    return {
        "schema": "hwa-experiment",
        "schema_version": 1,
        "method_version": "stage8-1",
        "clock_rate_hz": 44100,
        "inputs": [
            {"id": name, "sha256": sha256(path)}
            for name, path in sorted(inputs.items())
        ],
        "parameters": [
            public_parameter(row) for row in corpus_parameter_rows(target)
        ],
        "plan": {
            "kind": "grid", "seed": 1007,
            "sample_count": 0, "replicates": 1,
        },
        "cases": [cases[name] for name in sorted(cases)],
        "responses": responses,
    }


def joint_experiment(references, dependencies):
    expected = {
        "reference_{}_{}".format(target, role)
        for target in STRING_SPECS for role in ("fit", "check", "audit")
    }
    if set(references) != expected:
        raise AdapterError("joint references must contain fit, check, and audit")
    checked = {name: regular(path, name)
               for name, path in references.items()}
    channels = {}
    for name, path in checked.items():
        rate, count, frames = wave_facts(path)
        if (rate != 44100 or count not in (1, 2) or frames < 1 or
                frames > 1000000 or path.stat().st_size > 16 * 1024 * 1024):
            raise AdapterError(
                "{} must be a bounded mono or stereo 44.1 kHz PCM WAVE".format(
                    name))
        channels[name] = count

    inputs = dict(checked)
    for name, path in dependencies.items():
        if name in inputs:
            raise AdapterError("duplicate input id: {}".format(name))
        inputs[name] = regular(path, name)

    cases = []
    for target in sorted(STRING_SPECS):
        for role, split in (("check", "check"), ("fit", "fit")):
            input_id = "reference_{}_{}".format(target, role)
            cases.append({
                "id": "{}-pizz-{}".format(target, role),
                "split": split,
                "weight": 1,
                "stems": [
                    stem("model.final", "model", None, "model.wav"),
                    stem("reference.final", "reference", input_id, None,
                         channels[input_id]),
                ],
                "probes": [],
                "links": [],
            })
    cases.sort(key=lambda row: row["id"])
    responses = [
        {"id": "final.band.120-250", "role": "final",
         "feature": "band_level_dbfs", "index": 2},
        {"id": "final.band.250-500", "role": "final",
         "feature": "band_level_dbfs", "index": 3},
        {"id": "final.band.60-120", "role": "final",
         "feature": "band_level_dbfs", "index": 1},
        {"id": "final.rms", "role": "final",
         "feature": "rms_dbfs", "index": 0},
    ]
    return {
        "schema": "hwa-experiment",
        "schema_version": 1,
        "method_version": "stage8-1",
        "clock_rate_hz": 44100,
        "inputs": [
            {"id": name, "sha256": sha256(path)}
            for name, path in sorted(inputs.items())
        ],
        "parameters": [dict(JOINT_PARAMETER)],
        "plan": {
            "kind": "one-at-a-time", "seed": 1007,
            "sample_count": 0, "replicates": 1,
        },
        "cases": cases,
        "responses": responses,
    }


def joint_fit_manifest(chains, profile_adapter_sha256):
    changes = []
    expected_losses = {}
    objectives = []
    for target, spec in STRING_SPECS.items():
        chain = chains[target]
        changes.extend(chain["changes"])
        for split in ("audit", "check", "fit"):
            case_role = "fit" if split == "fit" else "check"
            objective_id = "{}_{}_passive_decay".format(split, target)
            objectives.append({
                "case": "{}-pizz-{}".format(target, case_role),
                "id": objective_id,
                "kind": "passive-decay",
                "reference_binding": "reference_{}_{}".format(target, split),
                "resource_id": "model.final",
                "scale": 3.0,
                "split": split,
                "weight": 1.0,
            })
            if split != "audit":
                expected_losses[objective_id] = chain["losses"][split]
    objectives.sort(key=lambda row: row["id"])
    return {
        "schema": "hwa-instrument-fit",
        "schema_version": 2,
        "adapter_id": JOINT_ADAPTER_ID,
        "parameters": [{
            "id": JOINT_PARAMETER["id"],
            "unit": JOINT_PARAMETER["unit"],
            "minimum": JOINT_PARAMETER["minimum"],
            "maximum": JOINT_PARAMETER["maximum"],
            "baseline": JOINT_PARAMETER["baseline"],
            "profile_paths": [],
        }],
        "candidate": {
            "parameters": {JOINT_PARAMETER["id"]: 1.0},
            "expected_objective_losses": expected_losses,
            "profile_changes": changes,
            "profile_adapter_sha256": digest_text(
                profile_adapter_sha256, "profile adapter hash"),
        },
        "objectives": objectives,
        "selection": {
            "mode": "verify-candidate",
            "score_weights": {"fit": 1.0, "check": 1.0, "audit": 1.0},
            "max_score_increase": 0.0,
            "max_candidate_worst_harm": 0.25,
            "max_expected_loss_increase": 0.25,
            "minimum_candidate_t60_ratio": 0.5,
            "maximum_candidate_t60_ratio": 2.0,
            "minimum_candidate_support_ratio": 0.5,
            "limits": [
                {"split": "fit", "max_mean_loss_increase": 0.0,
                 "max_objective_loss_increase": 0.25,
                 "max_candidate_loss": 2.0},
                {"split": "check", "max_mean_loss_increase": 0.15,
                 "max_objective_loss_increase": 0.25,
                 "max_candidate_loss": 2.0},
                {"split": "audit", "max_mean_loss_increase": 0.15,
                 "max_objective_loss_increase": 0.25,
                 "max_candidate_loss": 2.0},
            ],
        },
    }


def validate_fit_manifest(path, spec=None):
    if spec is None:
        spec = STRING_SPECS["c2"]
    parameter = parameter_for(spec)
    note = spec["case_prefix"].split("-", 1)[0]
    manifest = load_json(path)
    if (set(manifest) != {"schema", "schema_version", "adapter_id",
                          "parameters", "objectives", "selection"} or
            manifest.get("schema") != "hwa-instrument-fit" or
            manifest.get("schema_version") != 1 or
            manifest.get("adapter_id") != spec["adapter_id"]):
        raise AdapterError("fit manifest header does not match the adapter")
    expected_parameter = {
        "id": parameter["id"],
        "unit": parameter["unit"],
        "minimum": parameter["minimum"],
        "maximum": parameter["maximum"],
        "baseline": parameter["baseline"],
        "profile_paths": [["strings", spec["profile_index"],
                           "loss_time_constant_seconds"]],
    }
    if manifest.get("parameters") != [expected_parameter]:
        raise AdapterError("fit manifest parameter contract changed")
    expected_objectives = [
        {
            "case": spec["case_prefix"] + "-check",
            "id": "check_{}_passive_decay".format(note),
            "kind": "passive-decay",
            "reference_binding": spec["input_prefix"] + "_check",
            "resource_id": "model.final", "scale": 3.0,
            "split": "check", "weight": 1.0,
        },
        {
            "case": spec["case_prefix"] + "-fit",
            "id": "fit_{}_passive_decay".format(note),
            "kind": "passive-decay",
            "reference_binding": spec["input_prefix"] + "_fit",
            "resource_id": "model.final", "scale": 3.0,
            "split": "fit", "weight": 1.0,
        },
    ]
    if manifest.get("objectives") != expected_objectives:
        raise AdapterError("fit manifest objective contract changed")
    if manifest.get("selection") != {
            "check_weight": 1.0,
            "max_candidate_loss": 2.0,
            "max_candidate_worst_harm": 0.25,
            "max_check_loss_increase": 0.15,
            "maximum_candidate_t60_ratio": 2.0,
            "minimum_candidate_support_ratio": 0.5,
            "minimum_candidate_t60_ratio": 0.5}:
        raise AdapterError("fit manifest selection contract changed")
    return manifest


def validate_shape_fit_manifest(path, target):
    spec = STRING_SPECS[target]
    shape = SHAPE_SPECS[target]
    note = spec["case_prefix"].split("-", 1)[0]
    manifest = load_json(path)
    if (set(manifest) != {"schema", "schema_version", "adapter_id",
                          "parameters", "objectives", "selection"} or
            manifest.get("schema") != "hwa-instrument-fit" or
            manifest.get("schema_version") != 1 or
            manifest.get("adapter_id") != shape["adapter_id"]):
        raise AdapterError("shape fit manifest header does not match the adapter")
    expected_parameters = []
    for row in shape_parameters(target):
        expected_parameters.append({
            "id": row["id"], "unit": row["unit"],
            "minimum": row["minimum"], "maximum": row["maximum"],
            "baseline": row["baseline"],
            "profile_paths": [["strings", spec["profile_index"],
                               row["profile_key"]]],
        })
    if manifest.get("parameters") != expected_parameters:
        raise AdapterError("shape fit manifest parameter contract changed")
    expected_objectives = [
        {
            "case": spec["case_prefix"] + "-check",
            "id": "check_{}_passive_decay".format(note),
            "kind": "passive-decay",
            "reference_binding": spec["input_prefix"] + "_check",
            "resource_id": "model.final", "scale": 3.0,
            "split": "check", "weight": 1.0,
        },
        {
            "case": spec["case_prefix"] + "-check",
            "fundamental_hz": shape["check_frequency"],
            "harmonic_count": 8,
            "id": "check_{}_harmonic_decay".format(note),
            "kind": "harmonic-decay",
            "reference_binding": spec["input_prefix"] + "_check",
            "resource_id": "model.final", "scale": 0.5,
            "split": "check", "weight": 1.0,
        },
        {
            "case": spec["case_prefix"] + "-fit",
            "id": "fit_{}_passive_decay".format(note),
            "kind": "passive-decay",
            "reference_binding": spec["input_prefix"] + "_fit",
            "resource_id": "model.final", "scale": 3.0,
            "split": "fit", "weight": 1.0,
        },
        {
            "case": spec["case_prefix"] + "-fit",
            "fundamental_hz": shape["fit_frequency"],
            "harmonic_count": 8,
            "id": "fit_{}_harmonic_decay".format(note),
            "kind": "harmonic-decay",
            "reference_binding": spec["input_prefix"] + "_fit",
            "resource_id": "model.final", "scale": 0.5,
            "split": "fit", "weight": 1.0,
        },
    ]
    if manifest.get("objectives") != expected_objectives:
        raise AdapterError("shape fit manifest objective contract changed")
    expected_selection = {
        "check_weight": shape.get("check_weight", 1.0),
        "max_candidate_harmonic_maximum_error_octaves": 1.5,
        "max_candidate_harmonic_mean_error_octaves": 0.75,
        "max_candidate_loss": 2.0,
        "max_candidate_worst_harm": 0.25,
        "max_check_loss_increase": 0.15,
        "maximum_candidate_t60_ratio": 2.0,
        "minimum_candidate_support_ratio": 0.5,
        "minimum_candidate_t60_ratio": 0.5,
    }
    if manifest.get("selection") != expected_selection:
        raise AdapterError("shape fit manifest selection contract changed")
    return manifest


def digest_text(value, name):
    if (type(value) is not str or
            re.fullmatch(r"[0-9a-f]{64}", value) is None):
        raise AdapterError("{} is not a SHA-256 value".format(name))
    return value


def rows_by_id(rows, name):
    if type(rows) is not list:
        raise AdapterError("{} must be a list".format(name))
    result = {}
    for index, row in enumerate(rows):
        if type(row) is not dict or type(row.get("id")) is not str:
            raise AdapterError("{}[{}] is invalid".format(name, index))
        if row["id"] in result:
            raise AdapterError("{} has duplicate id {}".format(name, row["id"]))
        result[row["id"]] = row
    return result


def checked_bundle_file(bundle, name, expected_hash):
    path = regular(bundle / name, "scalar bundle {}".format(name))
    if sha256(path) != digest_text(expected_hash, name + " hash"):
        raise AdapterError("scalar bundle {} hash changed".format(name))
    return path


def scalar_chain(target, bundle_path, selection_path):
    spec = STRING_SPECS[target]
    bundle = directory(bundle_path, "{} scalar bundle".format(target))
    receipt_path = regular(bundle / "receipt.json", "scalar bundle receipt")
    receipt = load_json(receipt_path)
    shape_mode = receipt.get("schema") == "hwa-cello-shape-fit-adapter-bundle"
    if shape_mode:
        if target not in SHAPE_SPECS:
            raise AdapterError("{} has no shape bundle contract".format(target))
        shape = SHAPE_SPECS[target]
        expected_parameters = [
            public_parameter(row) for row in shape_parameters(target)
        ]
        expected_paths = [
            ["strings", spec["profile_index"], row["profile_key"]]
            for row in shape_parameters(target)
        ]
        compatible = (
            receipt.get("schema_version") == 1 and
            receipt.get("adapter_id") == shape["adapter_id"] and
            receipt.get("target") == target and
            receipt.get("build_type") == BUILD_TYPE and
            receipt.get("parameters") == expected_parameters and
            receipt.get("profile_paths") == expected_paths
        )
    else:
        compatible = (
            receipt.get("schema") == "hwa-cello-fit-adapter-bundle" and
            receipt.get("schema_version") == 1 and
            receipt.get("adapter_id") == spec["adapter_id"] and
            receipt.get("target") == target and
            receipt.get("build_type") == BUILD_TYPE and
            receipt.get("parameter") == parameter_for(spec) and
            receipt.get("profile_path") == [
                "strings", spec["profile_index"],
                "loss_time_constant_seconds"]
        )
    if not compatible:
        raise AdapterError("{} scalar bundle receipt is incompatible".format(target))
    fit_path = checked_bundle_file(
        bundle, "fit.json", receipt.get("fit_manifest_sha256"))
    if shape_mode:
        validate_shape_fit_manifest(fit_path, target)
    else:
        validate_fit_manifest(fit_path, spec)
    checked_bundle_file(
        bundle, "experiment.json", receipt.get("experiment_sha256"))
    bindings_path = checked_bundle_file(
        bundle, "bindings.json", receipt.get("bindings_sha256"))
    checked_bundle_file(
        bundle, "renderer", receipt.get("renderer_sha256"))

    bindings_value = load_json(bindings_path)
    if (set(bindings_value) != {"schema", "schema_version", "bindings"} or
            bindings_value.get("schema") != "hwa-fit-bindings" or
            bindings_value.get("schema_version") != 1):
        raise AdapterError("{} scalar bindings are invalid".format(target))
    bindings = rows_by_id(bindings_value.get("bindings"), "scalar bindings")
    for name, row in bindings.items():
        if set(row) != {"id", "path", "sha256"}:
            raise AdapterError("scalar binding has invalid fields: {}".format(name))
        path = regular(Path(row["path"]), "scalar binding {}".format(name))
        if sha256(path) != digest_text(row["sha256"], name + " hash"):
            raise AdapterError("scalar binding hash changed: {}".format(name))

    reference_rows = rows_by_id(receipt.get("references"),
                                "scalar receipt references")
    expected_reference_ids = {
        spec["input_prefix"] + "_fit", spec["input_prefix"] + "_check"
    }
    if set(reference_rows) != expected_reference_ids:
        raise AdapterError("{} scalar references changed".format(target))
    references = {}
    for name in sorted(expected_reference_ids):
        row = reference_rows[name]
        binding = bindings.get(name)
        if (binding is None or binding.get("sha256") != row.get("sha256") or
                set(row) != {"id", "sha256", "rate_hz", "channels", "frames"}):
            raise AdapterError("{} scalar reference receipt changed".format(name))
        path = Path(binding["path"])
        if wave_facts(path) != (row["rate_hz"], row["channels"], row["frames"]):
            raise AdapterError("{} scalar reference facts changed".format(name))
        references[name.rsplit("_", 1)[1]] = path

    file_rows_value = rows_by_id(receipt.get("files"), "scalar receipt files")
    tree_rows_value = rows_by_id(receipt.get("trees"), "scalar receipt trees")
    required_files = {
        "base_profile", "cc", "cello_cmake", "cello_source", "cmake",
        "csound", "csound_library", "model_generator", "model_manifest",
        "model_schema", "ninja", "probe_csd", "python", "sndfile_library",
        "wasm_preparer",
    }
    if set(file_rows_value) != required_files or set(tree_rows_value) != {
            "csound_build", "csound_source"}:
        raise AdapterError("{} scalar dependency receipt changed".format(target))
    for name, row in file_rows_value.items():
        if set(row) != {"id", "path", "sha256"}:
            raise AdapterError("scalar dependency row changed: {}".format(name))
        checked_file({"path": row["path"], "sha256": row["sha256"]}, name)
    for name, row in tree_rows_value.items():
        if set(row) != {"id", "path", "sha256", "file_count"}:
            raise AdapterError("scalar tree row changed: {}".format(name))
        checked_tree({"path": row["path"], "sha256": row["sha256"],
                      "file_count": row["file_count"]}, name)

    selection_path = regular(selection_path, "{} scalar selection".format(target))
    selection = load_json(selection_path)
    expected_adapter_id = (
        SHAPE_SPECS[target]["adapter_id"] if shape_mode else spec["adapter_id"]
    )
    expected_methods = {
        "selection": SELECTION_METHOD_VERSION,
        "passive_decay": PASSIVE_DECAY_METHOD_VERSION,
    }
    if shape_mode:
        expected_methods["harmonic_decay"] = HARMONIC_DECAY_METHOD_VERSION
    if (selection.get("schema") != "hwa-instrument-fit-result" or
            selection.get("schema_version") != 1 or
            selection.get("adapter_id") != expected_adapter_id or
            selection.get("method_versions") != expected_methods or
            selection.get("fit_manifest_sha256") != sha256(fit_path) or
            selection.get("profile_sha256") !=
            file_rows_value["base_profile"]["sha256"] or
            selection.get("selector_sha256") != sha256(FIT_SELECTOR)):
        raise AdapterError("{} scalar selection is stale or incompatible".format(target))
    digest_text(selection.get("experiment_result_sha256"),
                "scalar experiment result hash")
    expected_bindings = [
        {"id": spec["input_prefix"] + "_check",
         "sha256": sha256(references["check"])},
        {"id": spec["input_prefix"] + "_fit",
         "sha256": sha256(references["fit"])},
    ]
    if selection.get("reference_bindings") != expected_bindings:
        raise AdapterError("{} scalar selection reference hashes changed".format(target))
    chosen_id = selection.get("chosen_point_id")
    points = selection.get("points")
    if type(chosen_id) is not int or type(points) is not list:
        raise AdapterError("{} scalar selection has no chosen point".format(target))
    chosen_rows = [row for row in points
                   if type(row) is dict and row.get("point_id") == chosen_id]
    if len(chosen_rows) != 1 or chosen_rows[0].get("eligible") is not True:
        raise AdapterError("{} scalar chosen point is not eligible".format(target))
    chosen = chosen_rows[0]
    parameters = selection.get("chosen_parameters")
    parameter_rows = (
        shape_parameters(target) if shape_mode else [parameter_for(spec)]
    )
    if (type(parameters) is not dict or
            set(parameters) != {row["id"] for row in parameter_rows} or
            chosen.get("parameters") != parameters):
        raise AdapterError("{} scalar chosen parameters changed".format(target))
    changes = []
    for row in parameter_rows:
        value = finite(
            parameters[row["id"]], "{} chosen value".format(row["id"])
        )
        if value not in row["levels"]:
            raise AdapterError(
                "{} scalar chosen value is outside the grid".format(target)
            )
        profile_key = (
            row["profile_key"] if shape_mode
            else "loss_time_constant_seconds"
        )
        if value != row["baseline"]:
            changes.append({
                "parameter": row["id"],
                "path": ["strings", spec["profile_index"], profile_key],
                "before": row["baseline"], "after": value,
                "minimum": row["minimum"], "maximum": row["maximum"],
                "unit": row["unit"],
                "source_fit_result_sha256": sha256(selection_path),
            })
    evidence = rows_by_id(
        [{"id": row.get("objective"), **row}
         for row in chosen.get("evidence", []) if type(row) is dict],
        "{} chosen evidence".format(target),
    )
    losses = {}
    for split in ("fit", "check"):
        objective = "{}_{}_passive_decay".format(split, target)
        row = evidence.get(objective)
        if row is None:
            raise AdapterError("{} scalar chosen evidence is missing".format(target))
        loss = finite(row.get("loss"), objective + " loss")
        if (loss < 0.0 or
                (not shape_mode and
                 loss != finite(chosen.get(split + "_loss"), split + " loss"))):
            raise AdapterError("{} scalar chosen loss changed".format(target))
        losses[split] = loss
    value = parameters[spec["parameter_id"]]
    return {
        "target": target,
        "bundle": bundle,
        "bundle_receipt_sha256": sha256(receipt_path),
        "selection_sha256": sha256(selection_path),
        "experiment_result_sha256": selection["experiment_result_sha256"],
        "value": value,
        "changes": changes,
        "losses": losses,
        "references": references,
        "reference_hashes": {name: sha256(path)
                             for name, path in references.items()},
        "files": file_rows_value,
        "trees": tree_rows_value,
        "selection_path": selection_path,
        "receipt_path": receipt_path,
        "fit_path": fit_path,
    }


def write_json(path, value):
    Path(path).write_text(
        json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def frozen_renderer(config, output):
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
    lines[0] = (
        "#!/bin/sh\n\"\"\":\"\n"
        "TMPDIR=${{TMPDIR:-/tmp}}\nexport TMPDIR\n"
        "exec {} \"$0\" \"$@\"\n\":\"\"\"\n".format(
            shlex.quote(config["tools"]["python"]["path"])
        )
    )
    output.write_text("".join(lines), encoding="utf-8")
    output.chmod(0o755)


def source_paths(cello_root):
    relative = {
        "cello_cmake": "CMakeLists.txt",
        "cello_source": "src/hlolli_wg_cello.c",
        "model_generator": "tools/generate_model.py",
        "wasm_preparer": "tools/prepare_wasm_source.py",
        "probe_csd": "examples/passive_open_string_fit.csd",
        "model_manifest": "model/manifest.json",
        "base_profile": "model/cello-v1.json",
        "model_schema": "model/schema/cello-v1.schema.json",
    }
    return {
        name: regular(cello_root / child, name)
        for name, child in relative.items()
    }


def file_rows(paths):
    return {
        name: {"path": str(path), "sha256": sha256(path)}
        for name, path in sorted(paths.items())
    }


def tree_row(path, name):
    path = directory(path, name)
    rows = []
    for child in sorted(path.rglob("*")):
        if child.is_symlink():
            raise AdapterError("{} contains a symlink: {}".format(name, child))
        if child.is_file():
            relative = child.relative_to(path).as_posix()
            rows.append((relative, sha256(child)))
    if not rows:
        raise AdapterError("{} has no regular files".format(name))
    digest = hashlib.sha256()
    for relative, file_hash in rows:
        encoded = relative.encode("utf-8")
        digest.update(len(encoded).to_bytes(8, "big"))
        digest.update(encoded)
        digest.update(bytes.fromhex(file_hash))
    return {
        "path": str(path),
        "sha256": digest.hexdigest(),
        "file_count": len(rows),
    }


def checked_tree(row, name):
    if type(row) is not dict or set(row) != {"path", "sha256", "file_count"}:
        raise AdapterError("frozen {} tree entry is invalid".format(name))
    current = tree_row(Path(row["path"]), name)
    if (current["sha256"] != row["sha256"] or
            current["file_count"] != row["file_count"]):
        raise AdapterError("{} header tree digest changed".format(name))
    return current


def checked_file(row, name):
    if type(row) is not dict or set(row) != {"path", "sha256"}:
        raise AdapterError("frozen {} entry is invalid".format(name))
    path = regular(Path(row["path"]), name)
    if sha256(path) != row["sha256"]:
        raise AdapterError("{} hash changed: {}".format(name, path))
    return path


def checked_config(config):
    if type(config) is not dict:
        raise AdapterError("invalid frozen adapter configuration")
    mode = config.get("mode", "scalar")
    if mode == "scalar":
        target = config.get("target")
        spec = STRING_SPECS.get(target) if type(target) is str else None
        expected_render = ({
            "a4": spec["render_a4"],
            "frames": MODEL_FRAMES,
            "frequency": spec["render_frequency"],
            "string": spec["render_string"],
        } if spec is not None else None)
        valid_mode = (
            spec is not None and config.get("adapter_id") == spec["adapter_id"] and
            config.get("parameter") == parameter_for(spec) and
            config.get("profile_index") == spec["profile_index"] and
            config.get("render") == expected_render
        )
    elif mode == "shape":
        target = config.get("target")
        spec = STRING_SPECS.get(target) if type(target) is str else None
        expected_render = ({
            "a4": spec["render_a4"],
            "frames": MODEL_FRAMES,
            "frequency": spec["render_frequency"],
            "string": spec["render_string"],
        } if spec is not None else None)
        expected_parameters = (
            [public_parameter(row) for row in shape_parameters(target)]
            if target in SHAPE_SPECS else None
        )
        valid_mode = (
            target in SHAPE_SPECS and spec is not None and
            config.get("adapter_id") == SHAPE_SPECS[target]["adapter_id"] and
            config.get("parameters") == expected_parameters and
            config.get("profile_index") == spec["profile_index"] and
            config.get("render") == expected_render
        )
    elif mode == "corpus":
        target = config.get("target")
        spec = STRING_SPECS.get(target) if type(target) is str else None
        expected_parameters = (
            [public_parameter(row) for row in corpus_parameter_rows(target)]
            if target in STRING_SPECS else None
        )
        cases = config.get("cases")
        valid_cases = type(cases) is dict and len(cases) >= 4
        split_counts = {"fit": 0, "check": 0}
        seen_bindings = set()
        if valid_cases:
            try:
                for case_id, case in cases.items():
                    if (type(case_id) is not str or len(case_id) > 127 or
                            type(case) is not dict or
                            set(case) != {"split", "binding",
                                         "reference_channels", "render"} or
                            case["split"] not in split_counts or
                            type(case["binding"]) is not str or
                            case["binding"] in seen_bindings or
                            case["reference_channels"] not in (1, 2)):
                        valid_cases = False
                        break
                    render = case["render"]
                    if (type(render) is not dict or
                            set(render) != {"a4", "frames", "frequency",
                                           "string"} or
                            render["frames"] != MODEL_FRAMES or
                            render["string"] != spec["render_string"]):
                        valid_cases = False
                        break
                    frequency = finite(
                        render["frequency"], "corpus render frequency"
                    )
                    a4 = finite(render["a4"], "corpus render A4")
                    nominal = NOMINAL_OPEN_FREQUENCIES[target]
                    if (abs(1200.0 * math.log2(frequency / nominal)) > 60.0 or
                            not math.isclose(
                                a4, 440.0 * frequency / nominal,
                                rel_tol=0.0, abs_tol=1.0e-12)):
                        valid_cases = False
                        break
                    split_counts[case["split"]] += 1
                    seen_bindings.add(case["binding"])
            except (AdapterError, KeyError, TypeError, ValueError):
                valid_cases = False
        valid_cases = valid_cases and all(
            count >= 2 for count in split_counts.values()
        )
        valid_mode = (
            spec is not None and
            config.get("adapter_id") == corpus_adapter_id(target) and
            config.get("parameters") == expected_parameters and
            config.get("profile_index") == spec["profile_index"] and
            valid_cases
        )
    elif mode == "joint":
        changes = config.get("candidate_changes")
        allowed_keys = {
            "bridge_cutoff_hz", "bridge_high_shelf_cutoff_hz",
            "bridge_high_shelf_loss_fraction",
            "bridge_loss_peak_bandwidth_hz", "bridge_loss_peak_fraction",
            "loss_time_constant_seconds", "nut_cutoff_hz",
        }
        valid_changes = type(changes) is list and bool(changes)
        if valid_changes:
            try:
                paths = [row.get("path") for row in changes]
                valid_changes = all(
                    type(row) is dict and type(row.get("path")) is list and
                    len(row["path"]) == 3 and row["path"][0] == "strings" and
                    type(row["path"][1]) is int and
                    0 <= row["path"][1] < len(STRING_SPECS) and
                    row["path"][2] in allowed_keys and
                    finite(row.get("minimum"), "joint minimum") <
                    finite(row.get("maximum"), "joint maximum") and
                    finite(row.get("minimum"), "joint minimum") <=
                    finite(row.get("after"), "joint value") <=
                    finite(row.get("maximum"), "joint maximum") and
                    finite(row.get("before"), "joint baseline") !=
                    finite(row.get("after"), "joint value")
                    for row in changes
                ) and len({tuple(path) for path in paths}) == len(paths) and {
                    path[1] for path in paths
                    if path[2] == "loss_time_constant_seconds"
                } == set(range(len(STRING_SPECS)))
            except (AdapterError, TypeError):
                valid_changes = False
        valid_mode = (
            config.get("adapter_id") == JOINT_ADAPTER_ID and
            config.get("parameter") == JOINT_PARAMETER and valid_changes and
            re.fullmatch(r"[0-9a-f]{64}",
                         str(config.get("candidate_profile_sha256"))) is not None
        )
    else:
        valid_mode = False
    if not valid_mode or config.get("build_type") != BUILD_TYPE:
        raise AdapterError("invalid frozen adapter configuration")
    files = config.get("files")
    tools = config.get("tools")
    trees = config.get("trees")
    if (type(files) is not dict or type(tools) is not dict or
            type(trees) is not dict):
        raise AdapterError("frozen adapter file tables are invalid")
    checked_files = {name: checked_file(row, name)
                     for name, row in sorted(files.items())}
    checked_tools = {name: checked_file(row, name)
                     for name, row in sorted(tools.items())}
    for name, row in sorted(trees.items()):
        checked_tree(row, name)
    directory(Path(config.get("csound_build_dir", "")),
              "Csound build tree")
    directory(Path(config.get("csound_source_dir", "")),
              "Csound source tree")
    return checked_files, checked_tools


def validate_profile(path):
    if FROZEN_CONFIG is None:
        raise AdapterError("profile validation needs a frozen renderer")
    files, unused_tools = checked_config(FROZEN_CONFIG)
    profile_path = regular(path, "profile")
    generator_path = files.get("model_generator")
    schema_path = files.get("model_schema")
    if generator_path is None or schema_path is None:
        raise AdapterError("frozen profile validator inputs are missing")
    spec = importlib.util.spec_from_file_location(
        "hwa_cello_model_generator", str(generator_path))
    if spec is None or spec.loader is None:
        raise AdapterError("cannot load the cello model generator")
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
        profile = module.load_json(profile_path)
        schema = module.load_json(schema_path)
        module.validate_profile(profile, schema, "cello_v1")
        if FROZEN_CONFIG.get("mode") == "joint":
            canonical = (
                json.dumps(profile, indent=2, sort_keys=True,
                           allow_nan=False, default=decimal_as_float) + "\n"
            ).encode("utf-8")
            if hashlib.sha256(canonical).hexdigest() != FROZEN_CONFIG.get(
                    "candidate_profile_sha256"):
                raise AdapterError("profile is not the frozen joint candidate")
    except Exception as error:
        raise AdapterError("profile validation failed: {}".format(error))


def decimal_as_float(value):
    if isinstance(value, Decimal):
        return float(value)
    raise TypeError("cannot encode {} as JSON".format(type(value).__name__))


def exact_object(value, keys, name):
    if type(value) is not dict or set(value) != set(keys):
        raise AdapterError("{} has invalid fields".format(name))
    return value


def integer(value, name, minimum=None):
    if type(value) is not int or (minimum is not None and value < minimum):
        raise AdapterError("{} is invalid".format(name))
    return value


def finite(value, name):
    if type(value) not in (int, float) or type(value) is bool:
        raise AdapterError("{} must be finite".format(name))
    result = float(value)
    if not math.isfinite(result):
        raise AdapterError("{} must be finite".format(name))
    return result


def validate_render_request(request_path, output_dir):
    if FROZEN_CONFIG is None:
        raise AdapterError("rendering needs a frozen renderer")
    files, tools = checked_config(FROZEN_CONFIG)
    request_path = regular(request_path, "render request")
    output_dir = directory(output_dir, "job output directory")
    if request_path.parent.resolve() != output_dir:
        raise AdapterError("render request must be inside the job directory")
    request = load_json(request_path)
    exact_object(request, (
        "schema", "schema_version", "method_version", "case_id", "job_id",
        "job_key", "inputs", "outputs", "parameters", "replicate", "seed",
        "split",
    ), "render request")
    if (request["schema"] != "hwa-render-job" or
            request["schema_version"] != 1 or
            request["method_version"] != "stage8-1"):
        raise AdapterError("unsupported render request")
    cases = FROZEN_CONFIG.get("cases")
    case = cases.get(request["case_id"]) if type(cases) is dict else None
    if type(case) is not dict or request["split"] != case.get("split"):
        raise AdapterError("unknown case or wrong split")
    integer(request["job_id"], "job_id", 1)
    integer(request["replicate"], "replicate", 0)
    integer(request["seed"], "seed", 0)
    if (type(request["job_key"]) is not str or
            re.fullmatch(r"[0-9a-f]{64}", request["job_key"]) is None):
        raise AdapterError("job_key is invalid")

    inputs = request["inputs"]
    if type(inputs) is not list or len(inputs) != 1:
        raise AdapterError("render request needs one reference input")
    input_row = exact_object(inputs[0], (
        "binding_id", "channels", "gain_db", "kind", "path",
        "probe_format", "probe_name", "rate_denominator", "rate_hz",
        "rate_numerator", "resource_id", "role", "sha256", "side",
        "start_sample", "unit", "value_count",
    ), "reference input")
    expected_input = {
        "binding_id": case["binding"],
        "channels": case.get("reference_channels"), "gain_db": 0,
        "kind": "stem", "probe_format": None, "probe_name": None,
        "rate_denominator": 0, "rate_hz": 44100, "rate_numerator": 0,
        "resource_id": "reference.final", "role": "final",
        "side": "reference", "start_sample": 0, "unit": None,
        "value_count": 0,
    }
    for key, value in expected_input.items():
        if input_row[key] != value:
            raise AdapterError("reference input has wrong {}".format(key))
    if (type(input_row["sha256"]) is not str or
            re.fullmatch(r"[0-9a-f]{64}", input_row["sha256"]) is None):
        raise AdapterError("reference input hash is invalid")
    reference = regular(Path(input_row["path"]), "reference input")
    if sha256(reference) != input_row["sha256"]:
        raise AdapterError("reference input hash changed")
    rate, channels, frames = wave_facts(reference)
    if (rate != 44100 or channels != case.get("reference_channels") or
            channels not in (1, 2) or frames < 1):
        raise AdapterError(
            "reference input must match its mono or stereo 44.1 kHz contract")

    outputs = request["outputs"]
    if type(outputs) is not list or len(outputs) != 1:
        raise AdapterError("render request needs one model output")
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
    for key, value in expected_output.items():
        if output_row[key] != value:
            raise AdapterError("model output has wrong {}".format(key))
    output_path = Path(output_row["path"])
    if (not output_path.is_absolute() or
            output_path.parent.resolve() != output_dir or
            output_path.name != "model.wav"):
        raise AdapterError("output path must be job-directory/model.wav")
    if output_path.exists() or output_path.is_symlink():
        raise AdapterError("output path already exists")

    parameters = request["parameters"]
    if FROZEN_CONFIG.get("mode") in ("shape", "corpus"):
        expected_parameters = FROZEN_CONFIG.get("parameters")
        if (type(parameters) is not list or
                type(expected_parameters) is not list or
                len(parameters) != len(expected_parameters)):
            raise AdapterError("render request has a wrong parameter set")
        expected_by_id = {row["id"]: row for row in expected_parameters}
        values = {}
        for item in parameters:
            parameter = exact_object(
                item, ("id", "unit", "value"), "render parameter"
            )
            expected_parameter = expected_by_id.get(parameter["id"])
            if (expected_parameter is None or
                    parameter["unit"] != expected_parameter.get("unit") or
                    parameter["id"] in values):
                raise AdapterError("render request has a wrong parameter set")
            value = finite(parameter["value"], "render parameter value")
            if (value < expected_parameter["minimum"] or
                    value > expected_parameter["maximum"]):
                raise AdapterError("render parameter value is out of range")
            if value not in expected_parameter.get("levels", []):
                raise AdapterError(
                    "render parameter value is outside the frozen grid"
                )
            values[parameter["id"]] = value
        if set(values) != set(expected_by_id):
            raise AdapterError("render request has a wrong parameter set")
        value = values
    else:
        if type(parameters) is not list or len(parameters) != 1:
            raise AdapterError("render request has a wrong parameter set")
        parameter = exact_object(parameters[0], ("id", "unit", "value"),
                                 "render parameter")
        expected_parameter = FROZEN_CONFIG.get("parameter")
        if (type(expected_parameter) is not dict or
                parameter["id"] != expected_parameter.get("id") or
                parameter["unit"] != expected_parameter.get("unit")):
            raise AdapterError("render request has a wrong parameter set")
        value = finite(parameter["value"], "render parameter value")
        if (value < expected_parameter["minimum"] or
                value > expected_parameter["maximum"]):
            raise AdapterError("render parameter value is out of range")
        if value not in expected_parameter.get("levels", []):
            raise AdapterError("render parameter value is outside the frozen grid")
    return request, request_path, reference, output_path, value, case, files, tools


def run_command(arguments, name, cwd=None, extra_environment=None):
    environment = {
        "LC_ALL": "C",
        "LANG": "C",
        "TZ": "UTC",
        "PATH": CLEAN_PATH,
    }
    if extra_environment is not None:
        environment.update(extra_environment)
    try:
        completed = subprocess.run(
            [str(value) for value in arguments],
            check=False,
            cwd=str(cwd) if cwd is not None else None,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
            timeout=600,
        )
    except subprocess.TimeoutExpired as error:
        raise AdapterError("{} timed out".format(name))
    if completed.returncode != 0:
        detail = completed.stderr[-2000:].decode("utf-8", "replace").strip()
        raise AdapterError("{} failed with status {}: {}".format(
            name, completed.returncode, detail))
    return completed


def verify_loaded_libraries(csound, libraries, cwd):
    expected = {
        name: path.resolve() for name, path in sorted(libraries.items())
    }
    if sys.platform == "darwin":
        completed = run_command(
            [csound, "--version"], "Csound library check", cwd,
            {"DYLD_PRINT_LIBRARIES": "1"},
        )
        lines = (completed.stdout + b"\n" + completed.stderr).decode(
            "utf-8", "replace").splitlines()
        loaded = {
            name: any(line.strip().endswith(str(path)) for line in lines)
            for name, path in expected.items()
        }
    elif sys.platform.startswith("linux"):
        ldd = shutil.which("ldd", path=CLEAN_PATH)
        if ldd is None:
            raise AdapterError("cannot find ldd for the Csound library check")
        completed = run_command(
            [ldd, csound], "Csound library check", cwd)
        found = set()
        for line in (completed.stdout + b"\n" + completed.stderr).decode(
                "utf-8", "replace").splitlines():
            item = line.strip()
            if "=>" in item:
                item = item.split("=>", 1)[1].strip()
            candidate = item.split(" ", 1)[0]
            if candidate.startswith("/"):
                found.add(Path(candidate).resolve())
        loaded = {name: path in found for name, path in expected.items()}
    else:
        raise AdapterError(
            "cannot check Csound libraries on this platform")
    missing = [name for name, present in sorted(loaded.items()) if not present]
    if missing:
        raise AdapterError(
            "Csound did not load the named libraries: {}".format(
                ", ".join(missing)))
    return [
        {"id": name, "path": str(path), "sha256": sha256(path)}
        for name, path in expected.items()
    ]


def write_joint_profile(source, output, changes):
    profile = load_json(source)
    strings = profile.get("strings")
    if (type(strings) is not list or len(strings) != len(STRING_SPECS) or
            type(changes) is not list or not changes):
        raise AdapterError("base profile has no four-string loss table")
    seen_paths = set()
    changed_strings = set()
    for row in changes:
        if type(row) is not dict:
            raise AdapterError("joint candidate has an invalid change")
        path = row.get("path")
        if (type(path) is not list or len(path) != 3 or
                path[0] != "strings" or type(path[1]) is not int or
                path[1] < 0 or path[1] >= len(strings) or
                type(path[2]) is not str or not path[2] or
                tuple(path) in seen_paths or
                type(strings[path[1]]) is not dict or
                path[2] not in strings[path[1]]):
            raise AdapterError("joint candidate has an invalid profile path")
        seen_paths.add(tuple(path))
        changed_strings.add(path[1])
        old = finite(strings[path[1]][path[2]], "joint source value")
        new = finite(row.get("after"), "joint candidate value")
        minimum = finite(row.get("minimum"), "joint candidate minimum")
        maximum = finite(row.get("maximum"), "joint candidate maximum")
        if (old != finite(row.get("before"), "joint source value") or
                minimum >= maximum or not minimum <= new <= maximum or
                old == new):
            raise AdapterError("base or candidate profile value changed")
        strings[path[1]][path[2]] = new
    if changed_strings != set(range(len(strings))):
        raise AdapterError("joint candidate does not change all four strings")
    write_json(output, profile)


def write_candidate_profile(source, output, value):
    profile = load_json(source)
    strings = profile.get("strings")
    if type(strings) is not list or len(strings) != len(STRING_SPECS):
        raise AdapterError("base profile has no four-string loss table")
    mode = FROZEN_CONFIG.get("mode", "scalar")
    if mode == "joint":
        if value not in (0.0, 1.0):
            raise AdapterError("joint candidate must be baseline or staged")
        if value == 1.0:
            write_joint_profile(
                source, output, FROZEN_CONFIG["candidate_changes"])
            if sha256(output) != FROZEN_CONFIG["candidate_profile_sha256"]:
                raise AdapterError("generated joint candidate profile hash changed")
            return
        for target, spec in STRING_SPECS.items():
            index = spec["profile_index"]
            if type(strings[index]) is not dict or finite(
                    strings[index].get("loss_time_constant_seconds"),
                    "base string loss time") != 0.25:
                raise AdapterError("base string loss time changed")
    elif mode in ("shape", "corpus"):
        target = FROZEN_CONFIG.get("target")
        if (mode == "shape" and target not in SHAPE_SPECS) or (
                mode == "corpus" and target not in STRING_SPECS) or (
                type(value) is not dict):
            raise AdapterError("shape candidate has a wrong target or values")
        index = STRING_SPECS[target]["profile_index"]
        if type(strings[index]) is not dict:
            raise AdapterError("base profile has no selected string")
        rows = (shape_parameters(target) if mode == "shape"
                else corpus_parameter_rows(target))
        if set(value) != {row["id"] for row in rows}:
            raise AdapterError("shape candidate has a wrong parameter set")
        for row in rows:
            old = finite(strings[index].get(row["profile_key"]),
                         "base string shape value")
            new = finite(value[row["id"]], "shape candidate value")
            if (old != row["baseline"] or new not in row["levels"]):
                raise AdapterError("base or candidate string shape changed")
            strings[index][row["profile_key"]] = new
    else:
        index = FROZEN_CONFIG.get("profile_index")
        if (type(index) is not int or index < 0 or index >= len(strings) or
                type(strings[index]) is not dict):
            raise AdapterError("base profile has no selected string")
        old = finite(strings[index].get("loss_time_constant_seconds"),
                     "base string loss time")
        expected = float(FROZEN_CONFIG["parameter"]["baseline"])
        if old != expected:
            raise AdapterError("base target-string loss time changed")
        strings[index]["loss_time_constant_seconds"] = value
    write_json(output, profile)


def copy_candidate_tree(files, source_root, value):
    destinations = {
        "cello_cmake": "CMakeLists.txt",
        "cello_source": "src/hlolli_wg_cello.c",
        "model_generator": "tools/generate_model.py",
        "wasm_preparer": "tools/prepare_wasm_source.py",
        "model_manifest": "model/manifest.json",
        "model_schema": "model/schema/cello-v1.schema.json",
    }
    for name, relative in destinations.items():
        destination = source_root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(str(files[name]), str(destination))
    profile = source_root / "model" / "cello-v1.json"
    write_candidate_profile(files["base_profile"], profile, value)


def find_module(build_root):
    suffixes = (".dylib", ".so", ".dll")
    candidates = sorted(
        path for path in build_root.rglob("*hlolli_wg_cello*")
        if path.is_file() and not path.is_symlink() and
        path.name.endswith(suffixes)
    )
    if len(candidates) != 1:
        raise AdapterError("candidate build produced {} plug-in modules".format(
            len(candidates)))
    return candidates[0]


def verify_model_wave(path):
    path = regular(path, "rendered model WAVE")
    try:
        with wave.open(str(path), "rb") as stream:
            facts = (stream.getnchannels(), stream.getsampwidth(),
                     stream.getframerate(), stream.getnframes(),
                     stream.getcomptype())
            audio = stream.readframes(stream.getnframes())
    except (wave.Error, EOFError) as error:
        raise AdapterError("invalid rendered model WAVE: {}".format(error))
    if facts != (2, 3, 44100, MODEL_FRAMES, "NONE"):
        raise AdapterError("rendered model WAVE has wrong format: {}".format(facts))
    if len(audio) != MODEL_FRAMES * 2 * 3:
        raise AdapterError("rendered model WAVE is truncated")
    nonzero = 0
    clipped = 0
    for index in range(0, len(audio), 3):
        value = audio[index] | (audio[index + 1] << 8) | (audio[index + 2] << 16)
        if value & 0x800000:
            value -= 0x1000000
        if value != 0:
            nonzero += 1
        if value in (-8388608, 8388607):
            clipped += 1
    if nonzero == 0:
        raise AdapterError("rendered model WAVE is silent")
    if clipped != 0:
        raise AdapterError("rendered model WAVE clips")
    return {"sha256": sha256(path), "nonzero_samples": nonzero,
            "clipped_samples": clipped}


def publish_new_file(source, output):
    source = regular(source, "rendered model WAVE")
    source_stat = os.lstat(str(source))
    identity = (source_stat.st_dev, source_stat.st_ino)
    try:
        os.link(str(source), str(output))
    except FileExistsError:
        raise AdapterError("model output already exists")
    except OSError as error:
        raise AdapterError("cannot publish model output: {}".format(error))
    return identity


def unlink_if_identity(path, identity):
    if identity is None:
        return
    try:
        current = os.lstat(str(path))
        if (current.st_dev, current.st_ino) == identity:
            path.unlink()
    except (FileNotFoundError, OSError):
        pass


def publish_new_directory(staged, output, expected_names):
    staged = directory(staged, "staged bundle")
    output = Path(output).absolute()
    names = set(expected_names)
    if (not names or any(type(name) is not str or Path(name).name != name
                         for name in names)):
        raise AdapterError("bundle file names are invalid")
    children = {path.name: path for path in staged.iterdir()}
    if set(children) != names:
        raise AdapterError("staged bundle has unexpected files")
    sources = {name: regular(children[name], "staged " + name)
               for name in names}
    try:
        output.mkdir()
    except FileExistsError as error:
        raise AdapterError("output directory already exists") from error
    except OSError as error:
        raise AdapterError("cannot create output directory: {}".format(
            error)) from error
    created = os.lstat(str(output))
    directory_identity = (created.st_dev, created.st_ino)
    published = {}
    try:
        for name in sorted(names):
            destination = output / name
            source_stat = os.lstat(str(sources[name]))
            identity = (source_stat.st_dev, source_stat.st_ino)
            try:
                os.link(str(sources[name]), str(destination),
                        follow_symlinks=False)
            except FileExistsError as error:
                raise AdapterError(
                    "bundle output already exists: {}".format(name)
                ) from error
            except OSError as error:
                raise AdapterError(
                    "cannot publish bundle output {}: {}".format(name, error)
                ) from error
            published[name] = identity
            destination_stat = os.lstat(str(destination))
            if ((destination_stat.st_dev, destination_stat.st_ino) !=
                    identity):
                raise AdapterError("published bundle file changed: " + name)
        current = os.lstat(str(output))
        if ((current.st_dev, current.st_ino) != directory_identity or
                {path.name for path in output.iterdir()} != names):
            raise AdapterError("published bundle directory changed")
        flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
        descriptor = os.open(str(output), flags)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
    except BaseException:
        for name, identity in published.items():
            unlink_if_identity(output / name, identity)
        try:
            current = os.lstat(str(output))
            if (current.st_dev, current.st_ino) == directory_identity:
                output.rmdir()
        except (FileNotFoundError, OSError):
            pass
        raise


def build_and_render(value, output_path, case, files, tools):
    published_identity = None
    with tempfile.TemporaryDirectory(
            prefix=".hwa-cello-candidate-",
            dir=str(output_path.parent.parent)) as text:
        scratch = Path(text)
        source_root = scratch / "source"
        build_root = scratch / "build"
        source_root.mkdir()
        copy_candidate_tree(files, source_root, value)
        generator = source_root / "tools" / "generate_model.py"
        manifest = source_root / "model" / "manifest.json"
        source = source_root / "src" / "hlolli_wg_cello.c"
        run_command([
            tools["python"], "-B", generator,
            "--manifest", manifest, "--source", source,
        ], "model generation", cwd=source_root)
        run_command([
            tools["python"], "-B", generator,
            "--manifest", manifest, "--source", source, "--check",
        ], "model check", cwd=source_root)
        run_command([
            tools["cmake"], "-S", source_root, "-B", build_root, "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE:STRING={}".format(BUILD_TYPE),
            "-DCMAKE_C_COMPILER:FILEPATH={}".format(tools["cc"]),
            "-DCMAKE_MAKE_PROGRAM:FILEPATH={}".format(tools["ninja"]),
            "-DPython3_EXECUTABLE:FILEPATH={}".format(tools["python"]),
            "-DHLOLLI_CSOUND_BUILD_DIR:PATH={}".format(
                FROZEN_CONFIG["csound_build_dir"]),
            "-DHLOLLI_CSOUND_SOURCE_DIR:PATH={}".format(
                FROZEN_CONFIG["csound_source_dir"]),
            "-DCSOUND_EXECUTABLE:FILEPATH={}".format(tools["csound"]),
            "-DHLOLLI_WG_CELLO_CUSTOM_CMAKE:FILEPATH=",
            "-DHLOLLI_WG_CELLO_BUILD_TESTS:BOOL=OFF",
            "-DHLOLLI_WG_CELLO_INSTALL:BOOL=OFF",
            "-DHLOLLI_WG_CELLO_WARNINGS_AS_ERRORS:BOOL=ON",
            "-DHLOLLI_WG_CELLO_WORKBENCH_DIR:PATH=",
        ], "candidate configure", cwd=scratch)
        run_command([
            tools["cmake"], "--build", build_root,
            "--target", "hlolli_wg_cello", "--parallel", "1",
        ], "candidate build", cwd=scratch)
        module = find_module(build_root)
        rendered = scratch / "model.wav"
        render = case.get("render", FROZEN_CONFIG.get("render"))
        if type(render) is not dict:
            raise AdapterError("render case has no frozen string geometry")
        run_command([
            tools["csound"], "--opcode-lib={}".format(module),
            "--omacro:FIT_A4={:.17g}".format(render["a4"]),
            "--omacro:FIT_FREQUENCY={:.17g}".format(
                render["frequency"]),
            "--omacro:FIT_STRING={}".format(
                render["string"]),
            "--sample-accurate", "--num-threads=1", "-W", "-3",
            "--nopeaks", "-d", "-m128", "-o", rendered,
            files["probe_csd"],
        ], "candidate render", cwd=scratch)
        facts = verify_model_wave(rendered)
        try:
            published_identity = publish_new_file(rendered, output_path)
            published = verify_model_wave(output_path)
            if published != facts:
                raise AdapterError("published model WAVE changed")
            return facts, published_identity
        except BaseException:
            unlink_if_identity(output_path, published_identity)
            raise


def render_job(request_path, output_dir):
    (request, checked_request, reference, output_path, value, case,
     files, tools) = validate_render_request(request_path, output_dir)
    request_hash = sha256(checked_request)
    reference_hash = sha256(reference)
    published_identity = None
    try:
        facts, published_identity = build_and_render(
            value, output_path, case, files, tools)
        checked_config(FROZEN_CONFIG)
        if sha256(checked_request) != request_hash:
            raise AdapterError("render request changed during the job")
        if sha256(reference) != reference_hash:
            raise AdapterError("reference input changed during the job")
    except BaseException:
        unlink_if_identity(output_path, published_identity)
        raise
    print(json.dumps({
        "adapter_id": FROZEN_CONFIG["adapter_id"],
        "case_id": request["case_id"],
        "job_key": request["job_key"],
        "parameter": value,
        "model_sha256": facts["sha256"],
        "nonzero_samples": facts["nonzero_samples"],
        "clipped_samples": facts["clipped_samples"],
    }, sort_keys=True))


def build_target(arguments):
    legacy = (arguments.reference_c2_fit, arguments.reference_c2_check)
    generic = (arguments.reference_fit, arguments.reference_check)
    if any(legacy) and any(generic):
        raise AdapterError("legacy and generic reference options cannot be mixed")
    if any(legacy):
        if not all(legacy):
            raise AdapterError("both legacy C2 references are required")
        if arguments.target not in (None, "c2"):
            raise AdapterError("legacy reference options only support target c2")
        return "c2", legacy[0], legacy[1]
    if not all(generic):
        raise AdapterError("--reference-fit and --reference-check are required")
    return arguments.target or "c2", generic[0], generic[1]


def corpus_plan(path, target, cello_root):
    path = regular(path, "corpus plan")
    value = load_json(path)
    if (set(value) != {
            "schema", "schema_version", "target", "predeclaration_path",
            "predeclaration_sha256", "references"} or
            value.get("schema") != CORPUS_PLAN_SCHEMA or
            value.get("schema_version") != 1 or
            value.get("target") != target):
        raise AdapterError("corpus plan header is invalid")
    predeclaration = regular(
        Path(value["predeclaration_path"]), "corpus predeclaration"
    )
    if sha256(predeclaration) != digest_text(
            value["predeclaration_sha256"], "corpus predeclaration hash"):
        raise AdapterError("corpus predeclaration hash changed")
    raw_rows = value.get("references")
    if type(raw_rows) is not list or not raw_rows:
        raise AdapterError("corpus plan has no references")
    expected_keys = {
        "id", "source_id", "performance_id", "dynamic", "split", "path",
        "fundamental_hz",
    }
    rows = []
    ids = set()
    hashes = set()
    source_splits = {}
    performances = set()
    nominal = NOMINAL_OPEN_FREQUENCIES[target]
    for index, raw in enumerate(raw_rows):
        if type(raw) is not dict or set(raw) != expected_keys:
            raise AdapterError("corpus reference {} is invalid".format(index))
        tokens = {}
        for name in ("id", "source_id", "performance_id", "dynamic"):
            item = raw.get(name)
            if (type(item) is not str or
                    re.fullmatch(r"[A-Za-z0-9._-]{1,80}", item) is None):
                raise AdapterError(
                    "corpus reference {} has invalid {}".format(index, name)
                )
            tokens[name] = item
        split = raw.get("split")
        if split not in ("fit", "check"):
            raise AdapterError("corpus reference has an invalid split")
        source_split = source_splits.setdefault(tokens["source_id"], split)
        if source_split != split:
            raise AdapterError("one corpus source crosses fit and check")
        identity = (tokens["source_id"], tokens["performance_id"])
        if identity in performances:
            raise AdapterError("corpus performance is duplicated")
        performances.add(identity)
        if tokens["id"] in ids:
            raise AdapterError("corpus reference id is duplicated")
        ids.add(tokens["id"])
        reference = regular(Path(raw["path"]), "corpus reference")
        if under(reference, cello_root) or under(reference, SOURCE_DIR.parents[1]):
            raise AdapterError("corpus references must stay outside repositories")
        file_hash = sha256(reference)
        if file_hash in hashes:
            raise AdapterError("corpus reference audio is duplicated")
        hashes.add(file_hash)
        rate, channels, frames = wave_facts(reference)
        if (rate != 44100 or channels not in (1, 2) or frames < 1 or
                frames > 1000000 or reference.stat().st_size > 16 * 1024 * 1024):
            raise AdapterError(
                "corpus reference must be a bounded mono or stereo 44.1 kHz PCM WAVE"
            )
        frequency = finite(raw.get("fundamental_hz"), "corpus fundamental")
        cents = 1200.0 * math.log2(frequency / nominal)
        if abs(cents) > 60.0:
            raise AdapterError("corpus reference pitch exceeds 60 cents")
        binding_id = "reference_{}_{}".format(target, tokens["id"])
        case_id = "{}-pizz-{}-{}".format(
            target, split, tokens["source_id"]
        )
        if len(binding_id) > 127 or len(case_id) > 127:
            raise AdapterError("corpus reference identifiers are too long")
        rows.append({
            **tokens,
            "split": split,
            "path": reference,
            "sha256": file_hash,
            "fundamental_hz": frequency,
            "binding_id": binding_id,
            "case_id": case_id,
            "rate_hz": rate,
            "channels": channels,
            "frames": frames,
        })
    sources_by_split = {
        split: {row["source_id"] for row in rows if row["split"] == split}
        for split in ("fit", "check")
    }
    if any(len(source_ids) < 2 for source_ids in sources_by_split.values()):
        raise AdapterError("corpus fit and check each need two independent sources")
    return path, predeclaration, sorted(rows, key=lambda row: row["id"])


def build_corpus_bundle(arguments):
    output = arguments.output_dir.absolute()
    if output.exists() or output.is_symlink() or not output.parent.is_dir():
        raise AdapterError("output directory must be new with an existing parent")
    cello_root = directory(arguments.cello_root, "cello root")
    if under(output, cello_root) or under(output, SOURCE_DIR.parents[1]):
        raise AdapterError(
            "output directory must stay outside the source repositories"
        )
    target = arguments.target
    spec = STRING_SPECS[target]
    plan_path, predeclaration, rows = corpus_plan(
        arguments.corpus_plan, target, cello_root
    )
    files = source_paths(cello_root)
    if arguments.probe_csd is not None:
        supplied_probe = regular(arguments.probe_csd, "probe CSD")
        if supplied_probe.resolve() != files["probe_csd"].resolve():
            raise AdapterError(
                "--probe-csd must name the canonical probe under cello-root"
            )
    files["corpus_plan"] = plan_path
    files["corpus_predeclaration"] = predeclaration
    tools = {
        "csound": regular(Path(arguments.csound).resolve(), "Csound"),
        "csound_library": regular(
            Path(arguments.csound_library).resolve(), "Csound library"),
        "sndfile_library": regular(
            Path(arguments.sndfile_library).resolve(), "libsndfile library"),
        "cmake": regular(Path(arguments.cmake).resolve(), "CMake"),
        "ninja": regular(Path(arguments.ninja).resolve(), "Ninja"),
        "python": regular(Path(arguments.python).resolve(), "Python"),
        "cc": regular(Path(arguments.cc).resolve(), "C compiler"),
    }
    csound_build = directory(arguments.csound_build_dir, "Csound build tree")
    csound_source = directory(arguments.csound_source_dir, "Csound source tree")
    loaded_libraries = verify_loaded_libraries(
        tools["csound"], {
            "csound_library": tools["csound_library"],
            "sndfile_library": tools["sndfile_library"],
        }, cello_root)
    trees = {
        "csound_build": tree_row(csound_build / "include",
                                 "Csound build header tree"),
        "csound_source": tree_row(csound_source / "include",
                                  "Csound source header tree"),
    }
    dependencies = dict(files)
    experiment = build_corpus_experiment(rows, dependencies, target)
    fit = corpus_fit_manifest(target, rows)
    experiment_cases = rows_by_id(experiment["cases"], "corpus cases")
    cases = {}
    for case_id, case in experiment_cases.items():
        source_rows = [row for row in rows if row["case_id"] == case_id]
        renders = {json.dumps(row["render"], sort_keys=True)
                   for row in source_rows}
        if len(renders) != 1:
            raise AdapterError("corpus source has inconsistent render geometry")
        cases[case_id] = {
            "split": case["split"],
            "binding": case["stems"][1]["input_id"],
            "reference_channels": case["stems"][1]["channels"],
            "render": source_rows[0]["render"],
        }
    parameters = [
        public_parameter(row) for row in corpus_parameter_rows(target)
    ]
    config = {
        "mode": "corpus",
        "adapter_id": corpus_adapter_id(target),
        "build_type": BUILD_TYPE,
        "target": target,
        "profile_index": spec["profile_index"],
        "files": file_rows(files),
        "tools": file_rows(tools),
        "trees": trees,
        "csound_build_dir": str(csound_build),
        "csound_source_dir": str(csound_source),
        "parameters": parameters,
        "cases": cases,
    }
    references = {row["binding_id"]: row["path"] for row in rows}
    all_inputs = dict(references)
    all_inputs.update(dependencies)
    temporary = Path(tempfile.mkdtemp(
        prefix=".{}-".format(output.name), dir=str(output.parent)))
    try:
        renderer = temporary / "renderer"
        frozen_renderer(config, renderer)
        experiment_path = temporary / "experiment.json"
        fit_path = temporary / "fit.json"
        bindings_path = temporary / "bindings.json"
        write_json(experiment_path, experiment)
        write_json(fit_path, fit)
        write_json(bindings_path, {
            "schema": "hwa-fit-bindings",
            "schema_version": 1,
            "bindings": [
                {"id": name, "path": str(path), "sha256": sha256(path)}
                for name, path in sorted(all_inputs.items())
            ],
        })
        reference_rows = [{
            "id": row["id"],
            "binding_id": row["binding_id"],
            "source_id": row["source_id"],
            "performance_id": row["performance_id"],
            "dynamic": row["dynamic"],
            "split": row["split"],
            "fundamental_hz": row["fundamental_hz"],
            "sha256": row["sha256"],
            "rate_hz": row["rate_hz"],
            "channels": row["channels"],
            "frames": row["frames"],
        } for row in rows]
        receipt = {
            "schema": CORPUS_BUNDLE_SCHEMA,
            "schema_version": 1,
            "adapter_id": config["adapter_id"],
            "build_type": BUILD_TYPE,
            "target": target,
            "references": reference_rows,
            "source_count": len({row["source_id"] for row in rows}),
            "parameters": parameters,
            "profile_paths": [
                ["strings", spec["profile_index"], row["profile_key"]]
                for row in corpus_parameter_rows(target)
            ],
            "plan": experiment["plan"],
            "point_count": math.prod(
                len(row["levels"]) for row in parameters
            ),
            "case_count": len(cases),
            "job_count": math.prod(
                len(row["levels"]) for row in parameters
            ) * len(cases),
            "corpus_plan_sha256": sha256(plan_path),
            "corpus_predeclaration_sha256": sha256(predeclaration),
            "loaded_libraries": loaded_libraries,
            "renderer_sha256": sha256(renderer),
            "experiment_sha256": sha256(experiment_path),
            "fit_manifest_sha256": sha256(fit_path),
            "bindings_sha256": sha256(bindings_path),
            "files": [
                {"id": name, "path": str(path), "sha256": sha256(path)}
                for name, path in sorted({**files, **tools}.items())
            ],
            "trees": [
                {"id": name, **row} for name, row in sorted(trees.items())
            ],
        }
        write_json(temporary / "receipt.json", receipt)
        publish_new_directory(temporary, output, {
            "renderer", "experiment.json", "fit.json", "bindings.json",
            "receipt.json",
        })
        shutil.rmtree(str(temporary), ignore_errors=True)
    except BaseException:
        shutil.rmtree(str(temporary), ignore_errors=True)
        raise


def build_bundle(arguments):
    shape_mode = arguments.command == "build-shape"
    output = arguments.output_dir.absolute()
    if output.exists() or output.is_symlink() or not output.parent.is_dir():
        raise AdapterError("output directory must be new with an existing parent")
    cello_root = directory(arguments.cello_root, "cello root")
    if under(output, cello_root) or under(output, SOURCE_DIR.parents[1]):
        raise AdapterError(
            "output directory must stay outside the source repositories")
    target, fit_reference, check_reference = build_target(arguments)
    if shape_mode and target not in SHAPE_SPECS:
        raise AdapterError("shape fit target has no frozen grid")
    spec = STRING_SPECS[target]
    files = source_paths(cello_root)
    if arguments.probe_csd is not None:
        supplied_probe = regular(arguments.probe_csd, "probe CSD")
        if supplied_probe.resolve() != files["probe_csd"].resolve():
            raise AdapterError(
                "--probe-csd must name the canonical probe under cello-root"
            )
    fit_manifest = regular(
        SHAPE_SPECS[target]["fit_manifest"] if shape_mode
        else spec["fit_manifest"],
        "fit manifest",
    )
    if shape_mode:
        validate_shape_fit_manifest(fit_manifest, target)
    else:
        validate_fit_manifest(fit_manifest, spec)
    fit_id = spec["input_prefix"] + "_fit"
    check_id = spec["input_prefix"] + "_check"
    references = {
        fit_id: regular(fit_reference, "fit reference"),
        check_id: regular(check_reference, "check reference"),
    }
    tools = {
        "csound": regular(Path(arguments.csound).resolve(), "Csound"),
        "csound_library": regular(
            Path(arguments.csound_library).resolve(), "Csound library"),
        "sndfile_library": regular(
            Path(arguments.sndfile_library).resolve(), "libsndfile library"),
        "cmake": regular(Path(arguments.cmake).resolve(), "CMake"),
        "ninja": regular(Path(arguments.ninja).resolve(), "Ninja"),
        "python": regular(Path(arguments.python).resolve(), "Python"),
        "cc": regular(Path(arguments.cc).resolve(), "C compiler"),
    }
    csound_build = directory(arguments.csound_build_dir, "Csound build tree")
    csound_source = directory(arguments.csound_source_dir, "Csound source tree")
    loaded_libraries = verify_loaded_libraries(
        tools["csound"], {
            "csound_library": tools["csound_library"],
            "sndfile_library": tools["sndfile_library"],
        }, cello_root)
    trees = {
        "csound_build": tree_row(csound_build / "include",
                                 "Csound build header tree"),
        "csound_source": tree_row(csound_source / "include",
                                  "Csound source header tree"),
    }
    dependencies = dict(files)
    experiment = (
        build_shape_experiment(references, dependencies, target)
        if shape_mode else build_experiment(references, dependencies, spec)
    )
    case_prefix = spec["case_prefix"]
    config = {
        "mode": "shape" if shape_mode else "scalar",
        "adapter_id": (SHAPE_SPECS[target]["adapter_id"]
                       if shape_mode else spec["adapter_id"]),
        "build_type": BUILD_TYPE,
        "target": target,
        "profile_index": spec["profile_index"],
        "files": file_rows(files),
        "tools": file_rows(tools),
        "trees": trees,
        "csound_build_dir": str(csound_build),
        "csound_source_dir": str(csound_source),
        "render": {
            "a4": spec["render_a4"],
            "frames": MODEL_FRAMES,
            "frequency": spec["render_frequency"],
            "string": spec["render_string"],
        },
        "cases": {
            case_prefix + "-check": {
                "split": "check", "binding": check_id,
                "reference_channels": wave_facts(references[check_id])[1],
            },
            case_prefix + "-fit": {
                "split": "fit", "binding": fit_id,
                "reference_channels": wave_facts(references[fit_id])[1],
            },
        },
    }
    if shape_mode:
        config["parameters"] = [
            public_parameter(row) for row in shape_parameters(target)
        ]
    else:
        config["parameter"] = parameter_for(spec)
    all_inputs = dict(references)
    all_inputs.update(dependencies)
    reference_rows = []
    for name, path in sorted(references.items()):
        rate, channels, frames = wave_facts(path)
        reference_rows.append({
            "id": name,
            "sha256": sha256(path),
            "rate_hz": rate,
            "channels": channels,
            "frames": frames,
        })
    temporary = Path(tempfile.mkdtemp(
        prefix=".{}-".format(output.name), dir=str(output.parent)))
    try:
        renderer = temporary / "renderer"
        frozen_renderer(config, renderer)
        experiment_path = temporary / "experiment.json"
        fit_path = temporary / "fit.json"
        bindings_path = temporary / "bindings.json"
        write_json(experiment_path, experiment)
        shutil.copyfile(str(fit_manifest), str(fit_path))
        write_json(bindings_path, {
            "schema": "hwa-fit-bindings",
            "schema_version": 1,
            "bindings": [
                {"id": name, "path": str(path), "sha256": sha256(path)}
                for name, path in sorted(all_inputs.items())
            ],
        })
        receipt = {
            "schema": ("hwa-cello-shape-fit-adapter-bundle" if shape_mode
                       else "hwa-cello-fit-adapter-bundle"),
            "schema_version": 1,
            "adapter_id": config["adapter_id"],
            "build_type": BUILD_TYPE,
            "target": target,
            "references": reference_rows,
            "render": config["render"],
            "loaded_libraries": loaded_libraries,
            "renderer_sha256": sha256(renderer),
            "experiment_sha256": sha256(experiment_path),
            "fit_manifest_sha256": sha256(fit_path),
            "bindings_sha256": sha256(bindings_path),
            "files": [
                {"id": name, "path": str(path), "sha256": sha256(path)}
                for name, path in sorted({**files, **tools}.items())
            ],
            "trees": [
                {"id": name, **row} for name, row in sorted(trees.items())
            ],
        }
        if shape_mode:
            receipt["parameters"] = config["parameters"]
            receipt["profile_paths"] = [
                ["strings", spec["profile_index"], row["profile_key"]]
                for row in shape_parameters(target)
            ]
            receipt["plan"] = experiment["plan"]
            receipt["point_count"] = math.prod(
                len(row["levels"]) for row in config["parameters"]
            )
        else:
            receipt["parameter"] = parameter_for(spec)
            receipt["profile_path"] = [
                "strings", spec["profile_index"],
                "loss_time_constant_seconds",
            ]
        write_json(temporary / "receipt.json", receipt)
        publish_new_directory(temporary, output, {
            "renderer", "experiment.json", "fit.json", "bindings.json",
            "receipt.json",
        })
        shutil.rmtree(str(temporary), ignore_errors=True)
    except BaseException:
        shutil.rmtree(str(temporary), ignore_errors=True)
        raise


def argument_rows(rows, width, name):
    if type(rows) is not list:
        raise AdapterError("{} rows are required".format(name))
    result = {}
    for row in rows:
        if type(row) is not list or len(row) != width:
            raise AdapterError("{} row is invalid".format(name))
        target = row[0]
        if target not in STRING_SPECS or target in result:
            raise AdapterError("{} target is missing or duplicated".format(name))
        result[target] = row[1:]
    if set(result) != set(STRING_SPECS):
        raise AdapterError("{} must name c2, g2, d3, and a3 once".format(name))
    return result


def under(path, root):
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def build_joint_bundle(arguments):
    output = arguments.output_dir.absolute()
    if output.exists() or output.is_symlink() or not output.parent.is_dir():
        raise AdapterError("output directory must be new with an existing parent")
    scalar_arguments = argument_rows(arguments.scalar, 3, "scalar")
    audit_arguments = argument_rows(arguments.audit, 4, "audit")
    chains = {
        target: scalar_chain(
            target, Path(scalar_arguments[target][0]),
            Path(scalar_arguments[target][1]))
        for target in STRING_SPECS
    }

    first = chains["c2"]
    file_hashes = {name: row["sha256"]
                   for name, row in first["files"].items()}
    tree_facts = {
        name: {key: row[key] for key in ("path", "sha256", "file_count")}
        for name, row in first["trees"].items()
    }
    for target, chain in chains.items():
        if ({name: row["sha256"] for name, row in chain["files"].items()} !=
                file_hashes or
                {name: (row["sha256"], row["file_count"])
                 for name, row in chain["trees"].items()} !=
                {name: (row["sha256"], row["file_count"])
                 for name, row in first["trees"].items()}):
            raise AdapterError(
                "{} scalar bundle uses different source or tools".format(target))

    source_names = {
        "cello_cmake", "cello_source", "model_generator", "wasm_preparer",
        "probe_csd", "model_manifest", "base_profile", "model_schema",
    }
    tool_names = {
        "csound", "csound_library", "sndfile_library", "cmake", "ninja",
        "python", "cc",
    }
    files = {
        name: checked_file({"path": first["files"][name]["path"],
                            "sha256": first["files"][name]["sha256"]}, name)
        for name in source_names
    }
    tools = {
        name: checked_file({"path": first["files"][name]["path"],
                            "sha256": first["files"][name]["sha256"]}, name)
        for name in tool_names
    }
    cello_root = files["base_profile"].parent.parent
    if under(output, cello_root) or under(output, SOURCE_DIR.parents[1]):
        raise AdapterError(
            "output directory must stay outside the source repositories")
    canonical = source_paths(cello_root)
    if ({name: (str(path), sha256(path)) for name, path in canonical.items()} !=
            {name: (str(path), sha256(path)) for name, path in files.items()}):
        raise AdapterError("scalar receipt paths do not name one cello source tree")

    references = {}
    reference_receipt = []
    known_hashes = set()
    for target, chain in chains.items():
        for role in ("fit", "check"):
            path = chain["references"][role]
            file_hash = chain["reference_hashes"][role]
            references["reference_{}_{}".format(target, role)] = path
            if file_hash in known_hashes:
                raise AdapterError(
                    "scalar recordings must be distinct across strings")
            known_hashes.add(file_hash)
            rate, channels, frames = wave_facts(path)
            reference_receipt.append({
                "id": "reference_{}_{}".format(target, role),
                "target": target, "role": role, "sha256": file_hash,
                "rate_hz": rate, "channels": channels, "frames": frames,
            })

    audit_hashes = set()
    for target in STRING_SPECS:
        path_text, source_id, performance_id = audit_arguments[target]
        if (re.fullmatch(r"[A-Za-z0-9._-]{1,127}", source_id) is None or
                re.fullmatch(r"[A-Za-z0-9._-]{1,127}", performance_id) is None):
            raise AdapterError("audit source and performance ids are invalid")
        path = regular(Path(path_text), "{} audit recording".format(target))
        if under(path, cello_root) or under(path, SOURCE_DIR.parents[1]):
            raise AdapterError("audit recording must stay outside the repositories")
        file_hash = sha256(path)
        if file_hash in known_hashes or file_hash in audit_hashes:
            raise AdapterError("audit recording must be new and distinct")
        audit_hashes.add(file_hash)
        rate, channels, frames = wave_facts(path)
        if (rate != 44100 or channels not in (1, 2) or frames < 1 or
                frames > 1000000 or path.stat().st_size > 16 * 1024 * 1024):
            raise AdapterError(
                "{} audit recording has an unsupported WAVE layout".format(target))
        input_id = "reference_{}_audit".format(target)
        references[input_id] = path
        reference_receipt.append({
            "id": input_id, "target": target, "role": "audit",
            "source_id": source_id, "performance_id": performance_id,
            "sha256": file_hash, "rate_hz": rate, "channels": channels,
            "frames": frames,
        })

    provenance_files = {
        "fit_selector": regular(FIT_SELECTOR, "fit selector"),
    }
    for target, chain in chains.items():
        provenance_files["scalar_{}_selection".format(target)] = chain[
            "selection_path"]
        provenance_files["scalar_{}_bundle_receipt".format(target)] = chain[
            "receipt_path"]
        provenance_files["scalar_{}_fit_manifest".format(target)] = chain[
            "fit_path"]
    frozen_extras = dict(provenance_files)
    for target in STRING_SPECS:
        frozen_extras["reference_{}_audit".format(target)] = references[
            "reference_{}_audit".format(target)]
    frozen_files = dict(files)
    frozen_files.update(frozen_extras)
    dependencies = dict(files)
    dependencies.update(provenance_files)
    experiment = joint_experiment(references, dependencies)

    csound_build = Path(tree_facts["csound_build"]["path"]).parent
    csound_source = Path(tree_facts["csound_source"]["path"]).parent
    loaded_libraries = verify_loaded_libraries(
        tools["csound"], {
            "csound_library": tools["csound_library"],
            "sndfile_library": tools["sndfile_library"],
        }, cello_root)
    temporary = Path(tempfile.mkdtemp(
        prefix=".{}-".format(output.name), dir=str(output.parent)))
    try:
        candidate_profile = temporary / "candidate-profile.json"
        values = {target: chains[target]["value"] for target in STRING_SPECS}
        changes = [
            row for target in STRING_SPECS for row in chains[target]["changes"]
        ]
        write_joint_profile(files["base_profile"], candidate_profile, changes)
        candidate_hash = sha256(candidate_profile)
        cases = {}
        for case in experiment["cases"]:
            target = case["id"].split("-", 1)[0]
            role = case["id"].rsplit("-", 1)[1]
            spec = STRING_SPECS[target]
            binding = "reference_{}_{}".format(target, role)
            cases[case["id"]] = {
                "split": case["split"], "binding": binding,
                "reference_channels": wave_facts(references[binding])[1],
                "target": target,
                "render": {
                    "a4": spec["render_a4"], "frames": MODEL_FRAMES,
                    "frequency": spec["render_frequency"],
                    "string": spec["render_string"],
                },
            }
        config = {
            "mode": "joint", "adapter_id": JOINT_ADAPTER_ID,
            "build_type": BUILD_TYPE, "parameter": dict(JOINT_PARAMETER),
            "candidate_changes": changes,
            "candidate_profile_sha256": candidate_hash,
            "files": file_rows(frozen_files), "tools": file_rows(tools),
            "trees": tree_facts,
            "csound_build_dir": str(csound_build),
            "csound_source_dir": str(csound_source),
            "cases": cases,
        }
        renderer = temporary / "renderer"
        frozen_renderer(config, renderer)
        manifest = joint_fit_manifest(chains, sha256(renderer))
        completed = subprocess.run(
            [str(renderer), "--validate-profile", str(candidate_profile)],
            check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, env={"LC_ALL": "C", "LANG": "C", "TZ": "UTC"},
        )
        if completed.returncode != 0:
            raise AdapterError(
                "joint candidate profile failed validation: {}".format(
                    completed.stderr.strip()))

        experiment_path = temporary / "experiment.json"
        fit_path = temporary / "fit.json"
        bindings_path = temporary / "bindings.json"
        write_json(experiment_path, experiment)
        write_json(fit_path, manifest)
        all_inputs = dict(references)
        all_inputs.update(dependencies)
        write_json(bindings_path, {
            "schema": "hwa-fit-bindings", "schema_version": 1,
            "bindings": [
                {"id": name, "path": str(path), "sha256": sha256(path)}
                for name, path in sorted(all_inputs.items())
            ],
        })
        scalar_receipts = []
        for target, chain in chains.items():
            scalar_receipts.append({
                "target": target, "value": chain["value"],
                "bundle_receipt_sha256": chain["bundle_receipt_sha256"],
                "selection_sha256": chain["selection_sha256"],
                "experiment_result_sha256": chain["experiment_result_sha256"],
                "fit_loss": chain["losses"]["fit"],
                "check_loss": chain["losses"]["check"],
            })
        write_json(temporary / "receipt.json", {
            "schema": "hwa-cello-joint-fit-adapter-bundle",
            "schema_version": 1, "adapter_id": JOINT_ADAPTER_ID,
            "build_type": BUILD_TYPE, "job_count": 16,
            "source_profile_sha256": sha256(files["base_profile"]),
            "candidate_profile_sha256": candidate_hash,
            "candidate_values": values,
            "candidate_changes": changes,
            "scalar_results": scalar_receipts,
            "references": sorted(reference_receipt, key=lambda row: row["id"]),
            "loaded_libraries": loaded_libraries,
            "renderer_sha256": sha256(renderer),
            "experiment_sha256": sha256(experiment_path),
            "fit_manifest_sha256": sha256(fit_path),
            "bindings_sha256": sha256(bindings_path),
            "files": [
                {"id": name, "path": str(path), "sha256": sha256(path)}
                for name, path in sorted({**frozen_files, **tools}.items())
            ],
            "trees": [
                {"id": name, **row} for name, row in sorted(tree_facts.items())
            ],
        })
        publish_new_directory(temporary, output, {
            "renderer", "experiment.json", "fit.json", "bindings.json",
            "receipt.json", "candidate-profile.json",
        })
        shutil.rmtree(str(temporary), ignore_errors=True)
    except BaseException:
        shutil.rmtree(str(temporary), ignore_errors=True)
        raise


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    def add_build_arguments(command, targets):
        command.add_argument("--cello-root", required=True, type=Path)
        command.add_argument("--csound", required=True, type=Path)
        command.add_argument("--csound-library", required=True, type=Path)
        command.add_argument("--sndfile-library", required=True, type=Path)
        command.add_argument("--cmake", required=True, type=Path)
        command.add_argument("--ninja", required=True, type=Path)
        command.add_argument("--python", required=True, type=Path)
        command.add_argument("--cc", required=True, type=Path)
        command.add_argument("--csound-build-dir", required=True, type=Path)
        command.add_argument("--csound-source-dir", required=True, type=Path)
        command.add_argument("--probe-csd", type=Path,
                             help=argparse.SUPPRESS)
        command.add_argument("--target", choices=sorted(targets))
        command.add_argument("--reference-fit", type=Path)
        command.add_argument("--reference-check", type=Path)
        command.add_argument("--reference-c2-fit", type=Path,
                             help=argparse.SUPPRESS)
        command.add_argument("--reference-c2-check", type=Path,
                             help=argparse.SUPPRESS)
        command.add_argument("--output-dir", required=True, type=Path)

    add_build_arguments(commands.add_parser("build"), STRING_SPECS)
    add_build_arguments(commands.add_parser("build-shape"), SHAPE_SPECS)
    corpus = commands.add_parser("build-corpus")
    add_build_arguments(corpus, STRING_SPECS)
    corpus.add_argument("--corpus-plan", required=True, type=Path)
    joint = commands.add_parser("build-joint")
    joint.add_argument(
        "--scalar", action="append", nargs=3, metavar=("TARGET", "BUNDLE", "SELECTION"),
        required=True,
    )
    joint.add_argument(
        "--audit", action="append", nargs=4,
        metavar=("TARGET", "WAVE", "SOURCE_ID", "PERFORMANCE_ID"),
        required=True,
    )
    joint.add_argument("--output-dir", required=True, type=Path)
    return parser.parse_args()


def main():
    try:
        if len(sys.argv) == 3 and sys.argv[1] == "--validate-profile":
            validate_profile(Path(sys.argv[2]))
            return 0
        if (len(sys.argv) == 5 and
                sys.argv[1] == "--hwa-experiment-job" and
                sys.argv[3] == "--output-dir"):
            render_job(Path(sys.argv[2]), Path(sys.argv[4]))
            return 0
        arguments = parse_args()
        if arguments.command in ("build", "build-shape"):
            if FROZEN_CONFIG is not None:
                raise AdapterError("a frozen renderer cannot build a bundle")
            build_bundle(arguments)
        elif arguments.command == "build-corpus":
            if FROZEN_CONFIG is not None:
                raise AdapterError("a frozen renderer cannot build a bundle")
            build_corpus_bundle(arguments)
        elif arguments.command == "build-joint":
            if FROZEN_CONFIG is not None:
                raise AdapterError("a frozen renderer cannot build a bundle")
            build_joint_bundle(arguments)
    except (AdapterError, OSError, UnicodeError, json.JSONDecodeError) as error:
        print("adapter.py: {}".format(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

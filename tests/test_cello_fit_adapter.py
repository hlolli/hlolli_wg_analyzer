#!/usr/bin/env python3

import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import types
import unittest
from unittest import mock
import wave


ROOT = Path(__file__).resolve().parents[1]
ADAPTER = ROOT / "adapters" / "hlolli_wg_cello" / "adapter.py"
FIT_TOOL = ROOT / "tools" / "instrument_fit.py"
PYTHON = Path(sys.executable).resolve()


def load_adapter():
    spec = importlib.util.spec_from_file_location("cello_fit_adapter", ADAPTER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def rows_by_id_for_test(rows):
    return {row["id"]: row for row in rows}


def run_adapter(command):
    module = load_adapter()

    def checked_libraries(unused_csound, libraries, unused_cwd):
        return [
            {
                "id": name,
                "path": str(path.resolve()),
                "sha256": module.sha256(path),
            }
            for name, path in sorted(libraries.items())
        ]

    stdout = io.StringIO()
    stderr = io.StringIO()
    with mock.patch.object(module.sys, "argv", [str(ADAPTER), *command[2:]]), \
            mock.patch.object(
                module, "verify_loaded_libraries",
                side_effect=checked_libraries,
            ), contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        return_code = module.main()
    return types.SimpleNamespace(
        returncode=return_code,
        stdout=stdout.getvalue(),
        stderr=stderr.getvalue(),
    )


def pcm16_wave(path, frames=256, rate=44100, channels=2):
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(channels)
        stream.setsampwidth(2)
        stream.setframerate(rate)
        stream.writeframes(b"\0\0" * frames * channels)


def pcm24_extensible_wave(path, frames=256, rate=44100, channels=2):
    raw = b"\x01\x00\x00" * frames * channels
    block = channels * 3
    format_chunk = struct.pack(
        "<HHIIHHH", 0xfffe, channels, rate, rate * block, block, 24, 22
    ) + struct.pack("<HI", 24, 3) + bytes.fromhex(
        "0100000000001000800000aa00389b71"
    )
    chunks = (b"fmt " + struct.pack("<I", len(format_chunk)) + format_chunk +
              b"data" + struct.pack("<I", len(raw)) + raw)
    path.write_bytes(b"RIFF" + struct.pack("<I", len(chunks) + 4) +
                     b"WAVE" + chunks)


def fake_cello_tree(root):
    files = {
        "CMakeLists.txt": "cmake",
        "src/hlolli_wg_cello.c": "source",
        "tools/generate_model.py": (
            "from decimal import Decimal\n"
            "import json\n"
            "def load_json(path):\n"
            "    return json.loads(path.read_text(encoding='utf-8'), "
            "parse_float=Decimal)\n"
            "def validate_profile(profile, schema, expected_id=None):\n"
            "    if profile.get('id') != expected_id:\n"
            "        raise ValueError('wrong profile id')\n"
        ),
        "tools/prepare_wasm_source.py": "wasm",
        "examples/passive_open_string_fit.csd": "probe",
        "model/manifest.json": "{}",
        "model/cello-v1.json": (
            '{"id":"cello_v1","strings":'
            '[{"loss_time_constant_seconds":0.25,"nut_cutoff_hz":20000.0,'
            '"bridge_cutoff_hz":5386.995271806526,'
            '"bridge_low_shelf_cutoff_hz":500.0,'
            '"bridge_low_shelf_loss_fraction":0.0,'
            '"bridge_high_shelf_cutoff_hz":200.0,'
            '"bridge_high_shelf_loss_fraction":0.0,'
            '"bridge_loss_peak_frequency_hz":293.0,'
            '"bridge_loss_peak_bandwidth_hz":200.0,'
            '"bridge_loss_peak_fraction":0.0},'
            '{"loss_time_constant_seconds":0.25,"nut_cutoff_hz":4200.0,'
            '"bridge_cutoff_hz":4964.244281108786,'
            '"bridge_low_shelf_cutoff_hz":500.0,'
            '"bridge_low_shelf_loss_fraction":0.0,'
            '"bridge_high_shelf_cutoff_hz":200.0,'
            '"bridge_high_shelf_loss_fraction":0.0,'
            '"bridge_loss_peak_frequency_hz":293.0,'
            '"bridge_loss_peak_bandwidth_hz":200.0,'
            '"bridge_loss_peak_fraction":0.0},'
            '{"loss_time_constant_seconds":0.25,"nut_cutoff_hz":12000.0,'
            '"bridge_cutoff_hz":7086.471045764144,'
            '"bridge_low_shelf_cutoff_hz":500.0,'
            '"bridge_low_shelf_loss_fraction":0.0,'
            '"bridge_high_shelf_cutoff_hz":200.0,'
            '"bridge_high_shelf_loss_fraction":0.0,'
            '"bridge_loss_peak_frequency_hz":293.0,'
            '"bridge_loss_peak_bandwidth_hz":200.0,'
            '"bridge_loss_peak_fraction":0.0},'
            '{"loss_time_constant_seconds":0.25,"nut_cutoff_hz":12000.0,'
            '"bridge_cutoff_hz":6024.580442039031,'
            '"bridge_low_shelf_cutoff_hz":500.0,'
            '"bridge_low_shelf_loss_fraction":0.0,'
            '"bridge_high_shelf_cutoff_hz":200.0,'
            '"bridge_high_shelf_loss_fraction":0.0,'
            '"bridge_loss_peak_frequency_hz":293.0,'
            '"bridge_loss_peak_bandwidth_hz":200.0,'
            '"bridge_loss_peak_fraction":0.0}]}'
        ),
        "model/schema/cello-v1.schema.json": "{}",
    }
    for name, content in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="ascii")


def build_fake_bundle(root, target=None, probe_override=None, shape=False,
                      reference_offset=0, reference_paths=None,
                      output_dir=None, corpus=False):
    cello = root / "cello"
    cello.mkdir()
    fake_cello_tree(cello)
    if reference_paths is None:
        fit = root / "fit.wav"
        check = root / "check.wav"
        pcm16_wave(fit, frames=256 + reference_offset)
        pcm16_wave(check, frames=257 + reference_offset)
    else:
        fit, check = reference_paths
    tools = []
    for name in ("csound", "csound-library", "cmake", "ninja", "python",
                 "cc", "sndfile-library"):
        path = root / name
        if name == "csound":
            source = (
                "#!{}\n".format(PYTHON) +
                "import json, pathlib, sys, wave\n"
                "if '--version' in sys.argv:\n"
                "    root = pathlib.Path(__file__).resolve().parent\n"
                "    print('loaded ' + str((root / 'csound-library').resolve()), file=sys.stderr)\n"
                "    print('loaded ' + str((root / 'sndfile-library').resolve()), file=sys.stderr)\n"
                "    raise SystemExit(0)\n"
                "output = sys.argv[sys.argv.index('-o') + 1]\n"
                "module = next(x.split('=', 1)[1] for x in sys.argv "
                "if x.startswith('--opcode-lib='))\n"
                "slots = [x for x in sys.argv if x.startswith('--omacro:FIT_STRING=')]\n"
                "slot = int(slots[0].split('=', 1)[1]) - 1 if slots else 0\n"
                "rows = json.loads((pathlib.Path(module).parent / 'taus').read_text())\n"
                "row = rows[slot]\n"
                "sample_value = (float(row['loss_time_constant_seconds']) * 10000 + "
                "float(row['nut_cutoff_hz']) / 10 + "
                "float(row['bridge_cutoff_hz']) / 10 + "
                "float(row['bridge_low_shelf_cutoff_hz']) / 10 + "
                "float(row['bridge_low_shelf_loss_fraction']) * 1000 + "
                "float(row['bridge_high_shelf_loss_fraction']) * 100000 + "
                "float(row['bridge_loss_peak_bandwidth_hz']) / 10 + "
                "float(row['bridge_loss_peak_fraction']) * 2000)\n"
                "sample = round(sample_value).to_bytes(3, 'little', signed=True)\n"
                "with wave.open(output, 'wb') as stream:\n"
                "    stream.setnchannels(2)\n"
                "    stream.setsampwidth(3)\n"
                "    stream.setframerate(44100)\n"
                "    stream.writeframes(sample * 2 * 529200)\n"
            )
        elif name == "cmake":
            source = (
                "#!{}\n".format(PYTHON) +
                "import json, pathlib, sys\n"
                "if '--build' in sys.argv:\n"
                "    root = pathlib.Path(sys.argv[sys.argv.index('--build') + 1])\n"
                "    (root / 'libhlolli_wg_cello.dylib').write_bytes(b'module')\n"
                "else:\n"
                "    assert '-DCMAKE_BUILD_TYPE:STRING=Release' in sys.argv\n"
                "    source = pathlib.Path(sys.argv[sys.argv.index('-S') + 1])\n"
                "    root = pathlib.Path(sys.argv[sys.argv.index('-B') + 1])\n"
                "    root.mkdir(parents=True)\n"
                "    profile = json.loads((source / 'model/cello-v1.json').read_text())\n"
                "    values = profile['strings']\n"
                "    (root / 'taus').write_text(json.dumps(values))\n"
            )
        else:
            source = "#!{}\n".format(PYTHON)
        path.write_text(source, encoding="ascii")
        path.chmod(0o755)
        tools.append(path)
    csound_build = root / "csound-build"
    csound_source = root / "csound-source"
    (csound_build / "include").mkdir(parents=True)
    (csound_source / "include").mkdir(parents=True)
    (csound_build / "include" / "version.h").write_text(
        "version", encoding="ascii")
    (csound_source / "include" / "csdl.h").write_text(
        "csdl", encoding="ascii")
    output = output_dir if output_dir is not None else root / "bundle"
    command_name = ("build-corpus" if corpus else
                    "build-shape" if shape else "build")
    command = [
        sys.executable, str(ADAPTER), command_name,
        "--cello-root", str(cello),
        "--csound", str(tools[0]),
        "--csound-library", str(tools[1]),
        "--sndfile-library", str(tools[6]),
        "--cmake", str(tools[2]),
        "--ninja", str(tools[3]),
        "--python", str(PYTHON),
        "--cc", str(tools[5]),
        "--csound-build-dir", str(csound_build),
        "--csound-source-dir", str(csound_source),
        "--output-dir", str(output),
    ]
    if probe_override is not None:
        command.extend(["--probe-csd", str(probe_override)])
    if corpus:
        if target is None:
            target = "c2"
        predeclaration = root / "predeclaration.json"
        predeclaration.write_text('{"frozen":true}\n', encoding="ascii")
        references = []
        nominal = {
            "c2": 65.40639132514966,
            "g2": 97.99885899543733,
            "d3": 146.8323839587038,
            "a3": 220.0,
        }[target]
        for index, (split, source) in enumerate((
                ("fit", "fit-one"), ("fit", "fit-two"),
                ("check", "check-one"), ("check", "check-two"))):
            path = root / (source + ".wav")
            pcm16_wave(path, frames=300 + index)
            references.append({
                "id": source,
                "source_id": source,
                "performance_id": source + "-mf",
                "dynamic": "mf",
                "split": split,
                "path": str(path),
                "fundamental_hz": nominal + 0.01 * index,
            })
        corpus_plan = root / "corpus-plan.json"
        corpus_plan.write_text(json.dumps({
            "schema": "hwa-cello-passive-corpus-plan",
            "schema_version": 1,
            "target": target,
            "predeclaration_path": str(predeclaration),
            "predeclaration_sha256": hashlib.sha256(
                predeclaration.read_bytes()
            ).hexdigest(),
            "references": references,
        }), encoding="utf-8")
        command.extend([
            "--target", target,
            "--corpus-plan", str(corpus_plan),
        ])
    elif target is None:
        command.extend([
            "--reference-c2-fit", str(fit),
            "--reference-c2-check", str(check),
        ])
    else:
        command.extend([
            "--target", target,
            "--reference-fit", str(fit),
            "--reference-check", str(check),
        ])
    completed = run_adapter(command)
    return completed, output, cello


def render_request(binding, output_path, target="c2", letter="c"):
    return {
        "schema": "hwa-render-job",
        "schema_version": 1,
        "method_version": "stage8-1",
        "case_id": target + "-pizz-fit",
        "job_id": 1,
        "job_key": "0" * 64,
        "inputs": [{
            "binding_id": "reference_{}_fit".format(target),
            "channels": 2,
            "gain_db": 0,
            "kind": "stem",
            "path": binding["path"],
            "probe_format": None,
            "probe_name": None,
            "rate_denominator": 0,
            "rate_hz": 44100,
            "rate_numerator": 0,
            "resource_id": "reference.final",
            "role": "final",
            "sha256": binding["sha256"],
            "side": "reference",
            "start_sample": 0,
            "unit": None,
            "value_count": 0,
        }],
        "outputs": [{
            "id": "model.final",
            "kind": "stem",
            "path": str(output_path),
            "side": "model",
            "role": "final",
            "probe_format": None,
            "probe_name": None,
            "unit": None,
            "start_sample": 0,
            "gain_db": 0,
            "rate_hz": 44100,
            "channels": 2,
            "rate_numerator": 0,
            "rate_denominator": 0,
            "value_count": 0,
        }],
        "parameters": [{
            "id": "loss_time_constant_{}_seconds".format(letter),
            "unit": "seconds",
            "value": 0.25,
        }],
        "replicate": 0,
        "seed": 1007,
        "split": "fit",
    }


def fake_scalar_selection(bundle, target, value):
    receipt_path = bundle / "receipt.json"
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    parameter = receipt["parameter"]["id"]
    profile_hash = next(
        row["sha256"] for row in receipt["files"]
        if row["id"] == "base_profile"
    )
    evidence = [
        {"objective": "check_{}_passive_decay".format(target),
         "loss": 1.25},
        {"objective": "fit_{}_passive_decay".format(target),
         "loss": 1.0},
    ]
    result = {
        "schema": "hwa-instrument-fit-result",
        "schema_version": 1,
        "method_versions": {
            "passive_decay": "passive-decay-v4",
            "selection": "instrument-fit-selection-v1",
        },
        "adapter_id": receipt["adapter_id"],
        "fit_manifest_sha256": receipt["fit_manifest_sha256"],
        "experiment_result_sha256": hashlib.sha256(
            target.encode("ascii")
        ).hexdigest(),
        "analyzer_sha256": "a" * 64,
        "selector_sha256": hashlib.sha256(FIT_TOOL.read_bytes()).hexdigest(),
        "profile_sha256": profile_hash,
        "reference_bindings": [
            {"id": row["id"], "sha256": row["sha256"]}
            for row in receipt["references"]
        ],
        "baseline_point_id": 1,
        "chosen_point_id": 2,
        "chosen_parameters": {parameter: value},
        "baseline_score": 8.0,
        "chosen_score": 2.25,
        "points": [
            {
                "point_id": 2, "point_key": "2" * 64,
                "baseline": False, "parameters": {parameter: value},
                "fit_loss": 1.0, "check_loss": 1.25,
                "score": 2.25, "worst_harm": 0.0,
                "eligible": True, "evidence": evidence,
            },
            {
                "point_id": 1, "point_key": "1" * 64,
                "baseline": True, "parameters": {parameter: 0.25},
                "fit_loss": 4.0, "check_loss": 4.0,
                "score": 8.0, "worst_harm": 0.0,
                "eligible": True, "evidence": evidence,
            },
        ],
    }
    path = bundle.parent / (target + "-selection.json")
    path.write_text(json.dumps(result), encoding="utf-8")
    return path


def fake_shape_selection(bundle, target, values):
    receipt = json.loads((bundle / "receipt.json").read_text(encoding="utf-8"))
    profile_hash = next(
        row["sha256"] for row in receipt["files"]
        if row["id"] == "base_profile"
    )
    baseline = {row["id"]: row["baseline"] for row in receipt["parameters"]}
    evidence = [
        {"objective": "check_{}_passive_decay".format(target), "loss": 1.25},
        {"objective": "check_{}_harmonic_decay".format(target), "loss": 0.5},
        {"objective": "fit_{}_passive_decay".format(target), "loss": 1.0},
        {"objective": "fit_{}_harmonic_decay".format(target), "loss": 0.5},
    ]
    result = {
        "schema": "hwa-instrument-fit-result", "schema_version": 1,
        "method_versions": {
            "harmonic_decay": "harmonic-decay-v1",
            "passive_decay": "passive-decay-v4",
            "selection": "instrument-fit-selection-v1",
        },
        "adapter_id": receipt["adapter_id"],
        "fit_manifest_sha256": receipt["fit_manifest_sha256"],
        "experiment_result_sha256": hashlib.sha256(
            (target + "-shape").encode("ascii")
        ).hexdigest(),
        "analyzer_sha256": "a" * 64,
        "selector_sha256": hashlib.sha256(FIT_TOOL.read_bytes()).hexdigest(),
        "profile_sha256": profile_hash,
        "reference_bindings": [
            {"id": row["id"], "sha256": row["sha256"]}
            for row in receipt["references"]
        ],
        "baseline_point_id": 1, "chosen_point_id": 2,
        "chosen_parameters": values,
        "baseline_score": 8.0, "chosen_score": 1.625,
        "points": [
            {"point_id": 2, "point_key": "2" * 64, "baseline": False,
             "parameters": values, "fit_loss": 0.75, "check_loss": 0.875,
             "score": 1.625, "worst_harm": 0.0, "eligible": True,
             "evidence": evidence},
            {"point_id": 1, "point_key": "1" * 64, "baseline": True,
             "parameters": baseline, "fit_loss": 4.0, "check_loss": 4.0,
             "score": 8.0, "worst_harm": 0.0, "eligible": True,
             "evidence": evidence},
        ],
    }
    path = bundle.parent / (target + "-shape-selection.json")
    path.write_text(json.dumps(result), encoding="utf-8")
    return path


def build_fake_joint_bundle(root, tamper=None, selected=None, shape_g2=False,
                            shape_targets=None):
    if selected is None:
        selected = {"c2": 1.0, "g2": 1.0, "d3": 0.75, "a3": 1.0}
    if shape_targets is None:
        shape_targets = {"g2"} if shape_g2 else set()
    scalar_rows = []
    audit_rows = []
    source_profiles = []
    first_references = None
    for index, target in enumerate(("c2", "g2", "d3", "a3")):
        scalar_root = root / ("scalar-" + target)
        scalar_root.mkdir()
        reference_paths = None
        if tamper == "reused-scalar" and target == "g2":
            reference_paths = first_references
        completed, bundle, cello = build_fake_bundle(
            scalar_root, target, reference_offset=index * 2,
            reference_paths=reference_paths,
            shape=target in shape_targets)
        if completed.returncode != 0:
            return completed, root / "joint-bundle", source_profiles
        if target in shape_targets:
            selection = fake_shape_selection(bundle, target, selected[target])
        else:
            selection = fake_scalar_selection(bundle, target, selected[target])
        if tamper == "ineligible" and target == "d3":
            value = json.loads(selection.read_text(encoding="utf-8"))
            next(row for row in value["points"]
                 if row["point_id"] == value["chosen_point_id"])["eligible"] = False
            selection.write_text(json.dumps(value), encoding="utf-8")
        fit_binding = json.loads(
            (bundle / "bindings.json").read_text(encoding="utf-8"))
        fit_path = Path(next(
            row["path"] for row in fit_binding["bindings"]
            if row["id"] == "reference_{}_fit".format(target)
        ))
        check_path = Path(next(
            row["path"] for row in fit_binding["bindings"]
            if row["id"] == "reference_{}_check".format(target)
        ))
        if target == "c2":
            first_references = (fit_path, check_path)
        audit = root / ("audit-" + target + ".wav")
        if tamper == "reused-audit" and target == "g2":
            audit = fit_path
        else:
            pcm16_wave(audit, frames=300 + index, channels=1)
        scalar_rows.extend(["--scalar", target, str(bundle), str(selection)])
        audit_rows.extend([
            "--audit", target, str(audit), "audit-session-2026",
            "audit-performance-{}".format(target),
        ])
        source_profiles.append(cello / "model" / "cello-v1.json")
    if tamper == "output-in-cello":
        output = source_profiles[0].parents[1] / "joint-bundle"
    else:
        output = root / "joint-bundle"
    command = [sys.executable, str(ADAPTER), "build-joint"]
    command.extend(scalar_rows)
    command.extend(audit_rows)
    command.extend(["--output-dir", str(output)])
    completed = run_adapter(command)
    return completed, output, source_profiles


def joint_render_request(binding, output_path, target, value):
    request = render_request(binding, output_path, target=target)
    request["case_id"] = target + "-pizz-fit"
    request["inputs"][0]["binding_id"] = "reference_{}_fit".format(target)
    request["parameters"] = [{
        "id": "joint_candidate", "unit": "choice", "value": value,
    }]
    return request


def shape_render_request(binding, output_path, target="g2", values=None):
    if values is None:
        values = {
            "bridge_cutoff_g_hz": 4964.244281108786,
            "bridge_loss_peak_bandwidth_g_hz": 200.0,
            "bridge_loss_peak_g_fraction": 0.0,
            "loss_time_constant_g_seconds": 0.25,
        }
    request = render_request(binding, output_path, target=target,
                             letter=target[0])
    units = {}
    for name in values:
        if "time_constant" in name:
            units[name] = "seconds"
        elif name.endswith("_fraction"):
            units[name] = "ratio"
        else:
            units[name] = "hertz"
    request["parameters"] = [
        {"id": name, "unit": units[name], "value": values[name]}
        for name in sorted(values)
    ]
    return request


class CelloFitAdapterTests(unittest.TestCase):
    def test_linux_library_check_finds_ldd_on_the_clean_host_path(self):
        module = load_adapter()
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            csound_library = root / "libcsound64.so"
            sndfile_library = root / "libsndfile.so"
            csound_library.write_bytes(b"csound")
            sndfile_library.write_bytes(b"sndfile")
            completed = types.SimpleNamespace(
                stdout=(
                    "libcsound64.so => {} (0x1)\n"
                    "libsndfile.so => {} (0x2)\n"
                ).format(csound_library, sndfile_library).encode("utf-8"),
                stderr=b"",
            )
            with mock.patch.object(module.sys, "platform", "linux"), \
                    mock.patch.object(
                        module.shutil, "which", return_value="/host/bin/ldd",
                    ) as find_ldd, mock.patch.object(
                        module, "run_command", return_value=completed,
                    ):
                rows = module.verify_loaded_libraries(
                    root / "csound",
                    {
                        "csound_library": csound_library,
                        "sndfile_library": sndfile_library,
                    },
                    root,
                )
            find_ldd.assert_called_once_with("ldd", path=module.CLEAN_PATH)
            self.assertEqual(
                [row["id"] for row in rows],
                ["csound_library", "sndfile_library"],
            )

    def test_build_shape_freezes_a_g2_bridge_peak_grid(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            completed, bundle, unused_cello = build_fake_bundle(
                root, "g2", shape=True
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            experiment = json.loads(
                (bundle / "experiment.json").read_text(encoding="utf-8")
            )
            fit = json.loads(
                (bundle / "fit.json").read_text(encoding="utf-8")
            )
            receipt = json.loads(
                (bundle / "receipt.json").read_text(encoding="utf-8")
            )
            self.assertEqual(experiment["plan"]["kind"], "grid")
            self.assertEqual(
                [row["id"] for row in experiment["parameters"]],
                ["bridge_cutoff_g_hz",
                 "bridge_loss_peak_bandwidth_g_hz",
                 "bridge_loss_peak_g_fraction",
                 "loss_time_constant_g_seconds"],
            )
            self.assertEqual(receipt["point_count"], 96)
            self.assertEqual(
                receipt["schema"], "hwa-cello-shape-fit-adapter-bundle"
            )
            self.assertEqual(
                {row["kind"] for row in fit["objectives"]},
                {"passive-decay", "harmonic-decay"},
            )
            harmonic = [
                row for row in fit["objectives"]
                if row["kind"] == "harmonic-decay"
            ]
            self.assertEqual(len(harmonic), 2)
            self.assertTrue(all(row["harmonic_count"] == 8
                                for row in harmonic))

            bindings = json.loads(
                (bundle / "bindings.json").read_text(encoding="utf-8")
            )
            binding = next(
                row for row in bindings["bindings"]
                if row["id"] == "reference_g2_fit"
            )
            hashes = []
            for index, bridge in enumerate((4964.244281108786, 18000.0)):
                job = root / "shape-job-{}".format(index)
                job.mkdir()
                output = job / "model.wav"
                request = job / "request.json"
                value = shape_render_request(binding, output)
                value["parameters"][0]["value"] = bridge
                request.write_text(json.dumps(value), encoding="utf-8")
                rendered = subprocess.run([
                    str(bundle / "renderer"), "--hwa-experiment-job",
                    str(request), "--output-dir", str(job),
                ], check=False, stdout=subprocess.PIPE,
                   stderr=subprocess.PIPE, text=True)
                self.assertEqual(rendered.returncode, 0, rendered.stderr)
                hashes.append(hashlib.sha256(output.read_bytes()).hexdigest())
            self.assertNotEqual(hashes[0], hashes[1])

    def test_build_shape_freezes_c2_and_d3_frequency_loss_grids(self):
        expected = {
            "c2": (48, ["bridge_cutoff_c_hz",
                         "loss_time_constant_c_seconds",
                         "nut_cutoff_c_hz"]),
            "d3": (24, ["bridge_cutoff_d_hz",
                         "loss_time_constant_d_seconds",
                         "nut_cutoff_d_hz"]),
        }
        for target, (point_count, parameter_ids) in expected.items():
            with self.subTest(target=target), tempfile.TemporaryDirectory() as text:
                completed, bundle, unused_cello = build_fake_bundle(
                    Path(text), target, shape=True
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)
                experiment = json.loads(
                    (bundle / "experiment.json").read_text(encoding="utf-8")
                )
                fit = json.loads(
                    (bundle / "fit.json").read_text(encoding="utf-8")
                )
                receipt = json.loads(
                    (bundle / "receipt.json").read_text(encoding="utf-8")
                )
                self.assertEqual(receipt["point_count"], point_count)
                self.assertEqual(
                    [row["id"] for row in experiment["parameters"]],
                    parameter_ids,
                )
                self.assertEqual(fit["selection"]["check_weight"], 0.0)
                self.assertEqual(
                    {row["kind"] for row in fit["objectives"]},
                    {"passive-decay", "harmonic-decay"},
                )

    def test_build_corpus_balances_four_independent_sources(self):
        for target, point_count in (("c2", 96), ("g2", 96),
                                    ("d3", 24), ("a3", 24)):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as text:
                completed, bundle, unused_cello = build_fake_bundle(
                    Path(text), target, corpus=True
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)
                experiment = json.loads(
                    (bundle / "experiment.json").read_text(encoding="utf-8")
                )
                fit = json.loads(
                    (bundle / "fit.json").read_text(encoding="utf-8")
                )
                receipt = json.loads(
                    (bundle / "receipt.json").read_text(encoding="utf-8")
                )
                self.assertEqual(receipt["schema"],
                                 "hwa-cello-corpus-fit-adapter-bundle")
                self.assertEqual(receipt["source_count"], 4)
                self.assertEqual(receipt["point_count"], point_count)
                self.assertEqual(receipt["case_count"], 4)
                self.assertEqual(len(experiment["cases"]), 4)
                self.assertEqual(len(fit["objectives"]), 8)
                self.assertEqual(fit["selection"]["check_weight"], 1.0)
                self.assertEqual(
                    fit["selection"]["max_candidate_source_mean_loss"], 3.0
                )
                self.assertEqual(
                    {row["source_group"] for row in fit["objectives"]},
                    {"fit-one", "fit-two", "check-one", "check-two"},
                )
                self.assertEqual(
                    {row["weight"] for row in fit["objectives"]}, {1.0}
                )

    def test_corpus_manifest_balances_unequal_source_counts(self):
        module = load_adapter()
        rows = []
        for split, source, dynamics in (
                ("fit", "fit-one", ("pp", "mf")),
                ("fit", "fit-two", ("ff",)),
                ("check", "check-one", ("pp", "mf", "ff")),
                ("check", "check-two", ("mf",))):
            for dynamic in dynamics:
                rows.append({
                    "split": split,
                    "source_id": source,
                    "dynamic": dynamic,
                    "case_id": "c2-pizz-{}-{}".format(split, source),
                    "binding_id": "reference-c2-{}-{}".format(
                        source, dynamic
                    ),
                    "fundamental_hz": 65.4,
                })
        manifest = module.corpus_fit_manifest("c2", rows)
        totals = {}
        for row in manifest["objectives"]:
            kind = row["kind"]
            source = next(
                source for source in ("fit-one", "fit-two", "check-one",
                                      "check-two")
                if source in row["id"]
            )
            totals[(row["split"], source, kind)] = (
                totals.get((row["split"], source, kind), 0.0) +
                row["weight"]
            )
        self.assertEqual(set(totals.values()), {1.0})

    def test_corpus_followup_grids_are_frozen(self):
        module = load_adapter()
        self.assertEqual(
            [row["levels"] for row in module.corpus_parameter_rows("c2")],
            [
                [100.0, 200.0, 400.0],
                [0.0, 0.00125, 0.0025, 0.005],
                [0.25, 1.25, 1.5, 2.0],
                [6000.0, 20000.0],
            ],
        )
        self.assertEqual(
            [row["levels"] for row in module.corpus_parameter_rows("a3")],
            [
                [6024.580442039031, 12000.0, 18000.0],
                [0.25, 1.0, 1.5, 2.0],
                [12000.0, 20000.0],
            ],
        )

    def test_corpus_renderer_uses_frozen_source_geometry(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            completed, bundle, unused_cello = build_fake_bundle(
                root, "c2", corpus=True
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            bindings = rows_by_id_for_test(json.loads(
                (bundle / "bindings.json").read_text(encoding="utf-8")
            )["bindings"])
            binding = bindings["reference_c2_fit-one"]
            job = root / "corpus-job"
            job.mkdir()
            output = job / "model.wav"
            request = render_request(binding, output)
            request["case_id"] = "c2-pizz-fit-fit-one"
            request["inputs"][0]["binding_id"] = binding["id"]
            request["parameters"] = [{
                "id": row["id"],
                "unit": row["unit"],
                "value": row["baseline"],
            } for row in json.loads(
                (bundle / "receipt.json").read_text(encoding="utf-8")
            )["parameters"]]
            request_path = job / "request.json"
            request_path.write_text(json.dumps(request), encoding="utf-8")
            rendered = subprocess.run([
                str(bundle / "renderer"), "--hwa-experiment-job",
                str(request_path), "--output-dir", str(job),
            ], check=False, stdout=subprocess.PIPE,
               stderr=subprocess.PIPE, text=True)
            self.assertEqual(rendered.returncode, 0, rendered.stderr)
            with wave.open(str(output), "rb") as stream:
                self.assertEqual(stream.getnframes(), 529200)
            neutral_hash = hashlib.sha256(output.read_bytes()).hexdigest()

            shelf_job = root / "corpus-shelf-job"
            shelf_job.mkdir()
            shelf_output = shelf_job / "model.wav"
            shelf_request = render_request(binding, shelf_output)
            shelf_request["case_id"] = "c2-pizz-fit-fit-one"
            shelf_request["inputs"][0]["binding_id"] = binding["id"]
            shelf_request["parameters"] = [{
                "id": row["id"],
                "unit": row["unit"],
                "value": (0.0025 if row["id"] ==
                          "bridge_high_shelf_loss_c_fraction" else
                          row["baseline"]),
            } for row in json.loads(
                (bundle / "receipt.json").read_text(encoding="utf-8")
            )["parameters"]]
            shelf_request_path = shelf_job / "request.json"
            shelf_request_path.write_text(
                json.dumps(shelf_request), encoding="utf-8"
            )
            shelf_rendered = subprocess.run([
                str(bundle / "renderer"), "--hwa-experiment-job",
                str(shelf_request_path), "--output-dir", str(shelf_job),
            ], check=False, stdout=subprocess.PIPE,
               stderr=subprocess.PIPE, text=True)
            self.assertEqual(
                shelf_rendered.returncode, 0, shelf_rendered.stderr
            )
            self.assertNotEqual(
                neutral_hash,
                hashlib.sha256(shelf_output.read_bytes()).hexdigest(),
            )

    def test_build_joint_accepts_a_target_specific_g2_grid_value(self):
        with tempfile.TemporaryDirectory() as text:
            selected = {"c2": 1.0, "g2": 2.25, "d3": 0.75, "a3": 1.0}
            completed, output, unused_profiles = build_fake_joint_bundle(
                Path(text), selected=selected)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            receipt = json.loads(
                (output / "receipt.json").read_text(encoding="utf-8"))
            self.assertEqual(receipt["candidate_values"], selected)

    def test_build_joint_accepts_the_checked_g2_bridge_peak_shape(self):
        with tempfile.TemporaryDirectory() as text:
            selected = {
                "c2": 1.0,
                "g2": {
                    "bridge_cutoff_g_hz": 18000.0,
                    "bridge_loss_peak_bandwidth_g_hz": 100.0,
                    "bridge_loss_peak_g_fraction": 0.02,
                    "loss_time_constant_g_seconds": 2.5,
                },
                "d3": 0.75, "a3": 1.0,
            }
            completed, output, unused_profiles = build_fake_joint_bundle(
                Path(text), selected=selected, shape_g2=True)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            receipt = json.loads(
                (output / "receipt.json").read_text(encoding="utf-8"))
            fit = json.loads((output / "fit.json").read_text(encoding="utf-8"))
            profile = json.loads(
                (output / "candidate-profile.json").read_text(encoding="utf-8"))
            self.assertEqual(len(receipt["candidate_changes"]), 7)
            self.assertEqual(len(fit["candidate"]["profile_changes"]), 7)
            self.assertEqual(profile["strings"][1]["bridge_cutoff_hz"], 18000.0)
            self.assertEqual(
                profile["strings"][1]["bridge_loss_peak_bandwidth_hz"],
                100.0,
            )
            self.assertEqual(
                profile["strings"][1]["bridge_loss_peak_fraction"], 0.02
            )
            self.assertEqual(
                profile["strings"][1]["loss_time_constant_seconds"], 2.5
            )
            bindings = rows_by_id_for_test(json.loads(
                (output / "bindings.json").read_text(encoding="utf-8")
            )["bindings"])
            job = Path(text) / "joint-shape-job-g2"
            job.mkdir()
            model = job / "model.wav"
            request = job / "request.json"
            request.write_text(json.dumps(joint_render_request(
                bindings["reference_g2_fit"], model, "g2", 1.0
            )), encoding="utf-8")
            rendered = subprocess.run([
                str(output / "renderer"), "--hwa-experiment-job",
                str(request), "--output-dir", str(job),
            ], check=False, stdout=subprocess.PIPE,
               stderr=subprocess.PIPE, text=True)
            self.assertEqual(rendered.returncode, 0, rendered.stderr)
            with wave.open(str(model), "rb") as stream:
                sample = int.from_bytes(
                    stream.readframes(1)[:3], "little", signed=True)
            self.assertEqual(sample, 27320)

    def test_build_joint_accepts_frequency_loss_shapes_on_three_strings(self):
        with tempfile.TemporaryDirectory() as text:
            selected = {
                "c2": {
                    "bridge_cutoff_c_hz": 3500.0,
                    "loss_time_constant_c_seconds": 1.0,
                    "nut_cutoff_c_hz": 6000.0,
                },
                "g2": {
                    "bridge_cutoff_g_hz": 18000.0,
                    "bridge_loss_peak_bandwidth_g_hz": 100.0,
                    "bridge_loss_peak_g_fraction": 0.02,
                    "loss_time_constant_g_seconds": 2.5,
                },
                "d3": {
                    "bridge_cutoff_d_hz": 7086.471045764144,
                    "loss_time_constant_d_seconds": 0.75,
                    "nut_cutoff_d_hz": 8000.0,
                },
                "a3": 1.0,
            }
            completed, output, unused_profiles = build_fake_joint_bundle(
                Path(text), selected=selected,
                shape_targets={"c2", "g2", "d3"},
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            receipt = json.loads(
                (output / "receipt.json").read_text(encoding="utf-8")
            )
            profile = json.loads(
                (output / "candidate-profile.json").read_text(encoding="utf-8")
            )
            self.assertEqual(len(receipt["candidate_changes"]), 10)
            self.assertEqual(profile["strings"][0]["nut_cutoff_hz"], 6000.0)
            self.assertEqual(profile["strings"][2]["nut_cutoff_hz"], 8000.0)

    def test_build_joint_freezes_one_four_string_candidate(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            completed, output, profiles = build_fake_joint_bundle(root)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(
                sorted(path.name for path in output.iterdir()),
                ["bindings.json", "candidate-profile.json", "experiment.json",
                 "fit.json", "receipt.json", "renderer"],
            )
            experiment = json.loads(
                (output / "experiment.json").read_text(encoding="utf-8"))
            fit = json.loads((output / "fit.json").read_text(encoding="utf-8"))
            receipt = json.loads(
                (output / "receipt.json").read_text(encoding="utf-8"))
            candidate = json.loads(
                (output / "candidate-profile.json").read_text(encoding="utf-8"))

            self.assertEqual(experiment["parameters"], [{
                "id": "joint_candidate", "unit": "choice",
                "minimum": 0.0, "maximum": 1.0, "baseline": 0.0,
                "levels": [0.0, 1.0],
            }])
            self.assertEqual(len(experiment["cases"]), 8)
            self.assertEqual(
                [row["id"] for row in experiment["cases"]],
                sorted(row["id"] for row in experiment["cases"]),
            )
            self.assertEqual(fit["schema_version"], 2)
            self.assertEqual(fit["selection"]["mode"], "verify-candidate")
            self.assertEqual(
                {row["split"] for row in fit["objectives"]},
                {"fit", "check", "audit"},
            )
            self.assertEqual(len(fit["objectives"]), 12)
            self.assertEqual(
                [row["after"] for row in fit["candidate"]["profile_changes"]],
                [1.0, 1.0, 0.75, 1.0],
            )
            self.assertEqual(
                [row["loss_time_constant_seconds"]
                 for row in candidate["strings"]],
                [1.0, 1.0, 0.75, 1.0],
            )
            self.assertEqual(receipt["schema"],
                             "hwa-cello-joint-fit-adapter-bundle")
            self.assertEqual(receipt["job_count"], 16)
            self.assertEqual(receipt["candidate_profile_sha256"],
                             hashlib.sha256(
                                 (output / "candidate-profile.json").read_bytes()
                             ).hexdigest())
            accepted = subprocess.run([
                str(output / "renderer"), "--validate-profile",
                str(output / "candidate-profile.json"),
            ], check=False, stdout=subprocess.PIPE,
               stderr=subprocess.PIPE, text=True)
            rejected = subprocess.run([
                str(output / "renderer"), "--validate-profile",
                str(profiles[0]),
            ], check=False, stdout=subprocess.PIPE,
               stderr=subprocess.PIPE, text=True)
            self.assertEqual(accepted.returncode, 0, accepted.stderr)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("frozen joint candidate", rejected.stderr)
            self.assertEqual(
                [hashlib.sha256(path.read_bytes()).hexdigest()
                 for path in profiles],
                [receipt["source_profile_sha256"]] * 4,
            )

    def test_build_joint_rejects_reused_audit_audio(self):
        with tempfile.TemporaryDirectory() as text:
            completed, output, unused_profiles = build_fake_joint_bundle(
                Path(text), tamper="reused-audit")
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("audit recording", completed.stderr)
        self.assertFalse(output.exists())

    def test_build_joint_rejects_reused_scalar_audio(self):
        with tempfile.TemporaryDirectory() as text:
            completed, output, unused_profiles = build_fake_joint_bundle(
                Path(text), tamper="reused-scalar")
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("scalar recordings must be distinct", completed.stderr)
        self.assertFalse(output.exists())

    def test_build_joint_rejects_output_inside_a_source_repository(self):
        with tempfile.TemporaryDirectory() as text:
            completed, output, unused_profiles = build_fake_joint_bundle(
                Path(text), tamper="output-in-cello")
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("outside the source repositories", completed.stderr)
        self.assertFalse(output.exists())

    def test_build_joint_rejects_an_ineligible_scalar_choice(self):
        with tempfile.TemporaryDirectory() as text:
            completed, output, unused_profiles = build_fake_joint_bundle(
                Path(text), tamper="ineligible")
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("eligible", completed.stderr)
        self.assertFalse(output.exists())

    def test_joint_renderer_applies_all_four_values_as_one_choice(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            completed, bundle, profiles = build_fake_joint_bundle(root)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            before = [hashlib.sha256(path.read_bytes()).hexdigest()
                      for path in profiles]
            bindings = rows_by_id_for_test(json.loads(
                (bundle / "bindings.json").read_text(encoding="utf-8")
            )["bindings"])
            expected = {"c2": 1.0, "g2": 1.0, "d3": 0.75, "a3": 1.0}
            cutoff_sum = {
                "c2": 20000.0 + 5386.995271806526 + 500.0,
                "g2": 4200.0 + 4964.244281108786 + 500.0,
                "d3": 12000.0 + 7086.471045764144 + 500.0,
                "a3": 12000.0 + 6024.580442039031 + 500.0,
            }
            for index, target in enumerate(("c2", "g2", "d3", "a3")):
                job = root / ("joint-job-" + target)
                job.mkdir()
                output = job / "model.wav"
                request = job / "request.json"
                request.write_text(json.dumps(joint_render_request(
                    bindings["reference_{}_fit".format(target)], output,
                    target, 1.0)), encoding="utf-8")
                rendered = subprocess.run([
                    str(bundle / "renderer"), "--hwa-experiment-job",
                    str(request), "--output-dir", str(job),
                ], check=False, stdout=subprocess.PIPE,
                   stderr=subprocess.PIPE, text=True)
                self.assertEqual(rendered.returncode, 0, rendered.stderr)
                with wave.open(str(output), "rb") as stream:
                    sample = int.from_bytes(
                        stream.readframes(1)[:3], "little", signed=True)
                self.assertEqual(
                    sample,
                    round(expected[target] * 10000 +
                          cutoff_sum[target] / 10 + 20.0),
                )

            job = root / "joint-job-baseline"
            job.mkdir()
            output = job / "model.wav"
            request = job / "request.json"
            request.write_text(json.dumps(joint_render_request(
                bindings["reference_d3_fit"], output, "d3", 0.0
            )), encoding="utf-8")
            rendered = subprocess.run([
                str(bundle / "renderer"), "--hwa-experiment-job",
                str(request), "--output-dir", str(job),
            ], check=False, stdout=subprocess.PIPE,
               stderr=subprocess.PIPE, text=True)
            self.assertEqual(rendered.returncode, 0, rendered.stderr)
            with wave.open(str(output), "rb") as stream:
                sample = int.from_bytes(
                    stream.readframes(1)[:3], "little", signed=True)
            self.assertEqual(
                sample, round(0.25 * 10000 + cutoff_sum["d3"] / 10 + 20.0)
            )
            self.assertEqual(before, [
                hashlib.sha256(path.read_bytes()).hexdigest()
                for path in profiles
            ])

    def test_publish_never_follows_an_existing_symlink(self):
        module = load_adapter()
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            source = root / "source.wav"
            target = root / "target.txt"
            output = root / "model.wav"
            source.write_bytes(b"model")
            target.write_bytes(b"keep")
            output.symlink_to(target)
            with self.assertRaisesRegex(module.AdapterError,
                                        "already exists"):
                module.publish_new_file(source, output)
            self.assertEqual(target.read_bytes(), b"keep")
            self.assertTrue(output.is_symlink())

    def test_bundle_publish_never_replaces_an_existing_directory(self):
        module = load_adapter()
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            staged = root / "staged"
            output = root / "bundle"
            staged.mkdir()
            output.mkdir()
            (staged / "receipt.json").write_text("receipt", encoding="ascii")
            identity = (output.stat().st_dev, output.stat().st_ino)
            with self.assertRaisesRegex(module.AdapterError,
                                        "already exists"):
                module.publish_new_directory(
                    staged, output, {"receipt.json"})
            self.assertEqual(
                (output.stat().st_dev, output.stat().st_ino), identity)
            self.assertEqual(list(output.iterdir()), [])
            self.assertTrue((staged / "receipt.json").is_file())

    def test_build_rejects_a_caller_selected_probe(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            wrong = root / "wrong.csd"
            wrong.write_text("wrong probe", encoding="ascii")
            completed, output, unused_cello = build_fake_bundle(
                root, "g2", probe_override=wrong)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("canonical probe", completed.stderr)
        self.assertFalse(output.exists())

    def test_build_rejects_output_inside_the_cello_repository(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            completed, output, unused_cello = build_fake_bundle(
                root, "g2", output_dir=root / "cello" / "bundle")
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("outside the source repositories", completed.stderr)
        self.assertFalse(output.exists())

    def test_build_freezes_one_named_open_string(self):
        targets = {
            "c2": "c", "g2": "g", "d3": "d", "a3": "a",
        }
        for target, letter in targets.items():
            with self.subTest(target=target), tempfile.TemporaryDirectory() as text:
                root = Path(text)
                completed, output, unused_cello = build_fake_bundle(root, target)
                self.assertEqual(completed.returncode, 0, completed.stderr)
                experiment = json.loads((output / "experiment.json").read_text())
                fit = json.loads((output / "fit.json").read_text())
                receipt = json.loads((output / "receipt.json").read_text())
                parameter = "loss_time_constant_{}_seconds".format(letter)
                self.assertEqual(
                    [row["id"] for row in experiment["parameters"]],
                    [parameter],
                )
                self.assertEqual(
                    [row["id"] for row in experiment["cases"]],
                    [target + "-pizz-check", target + "-pizz-fit"],
                )
                self.assertEqual(fit["parameters"][0]["id"], parameter)
                self.assertEqual(receipt["target"], target)

    def test_fit_manifest_must_match_the_renderer_parameter_contract(self):
        module = load_adapter()
        manifest = json.loads(
            (ROOT / "adapters" / "hlolli_wg_cello" / "fit.json").read_text())
        manifest["parameters"][0]["baseline"] = 0.3
        with tempfile.TemporaryDirectory() as text:
            path = Path(text) / "fit.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(module.AdapterError,
                                        "parameter contract"):
                module.validate_fit_manifest(path)

    def test_experiment_uses_the_analyzer_contract(self):
        module = load_adapter()
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            fit = root / "fit.wav"
            check = root / "check.wav"
            dependency = root / "source.c"
            pcm24_extensible_wave(fit)
            pcm16_wave(check, frames=257)
            dependency.write_text("source", encoding="ascii")
            experiment = module.build_experiment(
                {"reference_c2_fit": fit, "reference_c2_check": check},
                {"cello_source": dependency},
            )

        self.assertEqual(experiment["schema"], "hwa-experiment")
        self.assertEqual(experiment["schema_version"], 1)
        self.assertEqual(experiment["method_version"], "stage8-1")
        self.assertEqual(experiment["clock_rate_hz"], 44100)
        self.assertEqual(experiment["plan"], {
            "kind": "one-at-a-time", "seed": 1007,
            "sample_count": 0, "replicates": 1,
        })
        self.assertEqual(
            [row["id"] for row in experiment["parameters"]],
            ["loss_time_constant_c_seconds"],
        )
        self.assertEqual(
            [row["id"] for row in experiment["cases"]],
            ["c2-pizz-check", "c2-pizz-fit"],
        )
        self.assertEqual(
            [row["id"] for row in experiment["inputs"]],
            sorted(row["id"] for row in experiment["inputs"]),
        )
        self.assertEqual(
            next(row for row in experiment["inputs"]
                 if row["id"] == "cello_source")["sha256"],
            hashlib.sha256(b"source").hexdigest(),
        )

    def test_experiment_keeps_a_mono_held_out_reference_mono(self):
        module = load_adapter()
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            fit = root / "fit.wav"
            check = root / "check.wav"
            dependency = root / "source.c"
            pcm16_wave(fit, channels=2)
            pcm16_wave(check, frames=257, channels=1)
            dependency.write_text("source", encoding="ascii")
            experiment = module.build_experiment(
                {"reference_c2_fit": fit, "reference_c2_check": check},
                {"cello_source": dependency},
            )
        check_case = next(row for row in experiment["cases"]
                          if row["id"] == "c2-pizz-check")
        reference = next(row for row in check_case["stems"]
                         if row["id"] == "reference.final")
        self.assertEqual(reference["channels"], 1)

    def test_experiment_rejects_unsupported_or_truncated_reference_pcm(self):
        module = load_adapter()
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            good = root / "good.wav"
            bad = root / "bad.wav"
            dependency = root / "source.c"
            pcm16_wave(good)
            dependency.write_text("source", encoding="ascii")
            with wave.open(str(bad), "wb") as stream:
                stream.setnchannels(2)
                stream.setsampwidth(4)
                stream.setframerate(44100)
                stream.writeframes(b"\0" * 4 * 2 * 256)
            with self.assertRaisesRegex(module.AdapterError,
                                        "16-bit or 24-bit"):
                module.build_experiment(
                    {"reference_c2_fit": good,
                     "reference_c2_check": bad},
                    {"cello_source": dependency})
            bad.write_bytes(good.read_bytes()[:-1])
            with self.assertRaisesRegex(module.AdapterError, "truncated"):
                module.build_experiment(
                    {"reference_c2_fit": good,
                     "reference_c2_check": bad},
                    {"cello_source": dependency})

    def test_build_publishes_one_frozen_bundle(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            completed, output, unused_cello = build_fake_bundle(root)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(
                sorted(path.name for path in output.iterdir()),
                ["bindings.json", "experiment.json", "fit.json",
                 "receipt.json", "renderer"],
            )
            experiment = json.loads((output / "experiment.json").read_text())
            receipt = json.loads((output / "receipt.json").read_text())
            self.assertEqual(experiment["method_version"], "stage8-1")
            self.assertEqual(receipt["schema"],
                             "hwa-cello-fit-adapter-bundle")
            self.assertEqual(receipt["build_type"], "Release")
            self.assertEqual(
                [row["id"] for row in receipt["loaded_libraries"]],
                ["csound_library", "sndfile_library"],
            )
            self.assertTrue((output / "renderer").stat().st_mode & 0o111)

    def test_bundle_receipt_names_the_target_and_reference_facts(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            completed, output, unused_cello = build_fake_bundle(root, "g2")
            self.assertEqual(completed.returncode, 0, completed.stderr)
            receipt = json.loads((output / "receipt.json").read_text())
            self.assertEqual(receipt["target"], "g2")
            self.assertEqual(receipt["profile_path"],
                             ["strings", 1, "loss_time_constant_seconds"])
            self.assertEqual(receipt["parameter"], {
                "baseline": 0.25,
                "id": "loss_time_constant_g_seconds",
                "levels": [
                    0.08, 0.125, 0.25, 0.5, 0.75, 1.0,
                    1.2, 1.25, 1.3, 1.35, 1.4, 1.45, 1.5,
                    1.6, 1.75, 2.0, 2.25, 2.5, 3.0,
                ],
                "maximum": 3.0,
                "minimum": 0.08,
                "unit": "seconds",
            })
            self.assertEqual(receipt["render"]["string"], 2)
            self.assertEqual(receipt["render"]["frames"], 529200)
            self.assertEqual(
                [(row["id"], row["channels"], row["frames"])
                 for row in receipt["references"]],
                [("reference_g2_check", 2, 257),
                 ("reference_g2_fit", 2, 256)],
            )

    def test_private_renderer_validates_profile_and_frozen_hashes(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            completed, bundle, cello = build_fake_bundle(root)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            private = root / "private-renderer"
            private.write_bytes((bundle / "renderer").read_bytes())
            private.chmod(0o755)
            good = cello / "model" / "cello-v1.json"
            bad = root / "bad.json"
            bad.write_text('{"id":"not_cello"}', encoding="ascii")

            accepted = subprocess.run(
                [str(private), "--validate-profile", str(good)],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True)
            rejected = subprocess.run(
                [str(private), "--validate-profile", str(bad)],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True)
            self.assertEqual(accepted.returncode, 0, accepted.stderr)
            self.assertNotEqual(rejected.returncode, 0)

            generator = cello / "tools" / "generate_model.py"
            original_generator = generator.read_text(encoding="ascii")
            generator.write_text("changed", encoding="ascii")
            changed = subprocess.run(
                [str(private), "--validate-profile", str(good)],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True)
            self.assertNotEqual(changed.returncode, 0)
            self.assertIn("hash changed", changed.stderr)
            generator.write_text(original_generator, encoding="ascii")
            header = root / "csound-source" / "include" / "csdl.h"
            header.write_text("changed header", encoding="ascii")
            changed_tree = subprocess.run(
                [str(private), "--validate-profile", str(good)],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True)
            self.assertNotEqual(changed_tree.returncode, 0)
            self.assertIn("header tree digest changed", changed_tree.stderr)

    def test_renderer_rejects_an_output_outside_the_job_directory(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            completed, bundle, unused_cello = build_fake_bundle(root)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            private = root / "private-renderer"
            private.write_bytes((bundle / "renderer").read_bytes())
            private.chmod(0o755)
            bindings = json.loads((bundle / "bindings.json").read_text())
            binding = next(row for row in bindings["bindings"]
                           if row["id"] == "reference_c2_fit")
            job = root / "job"
            job.mkdir()
            outside = root / "outside.wav"
            request = job / "request.json"
            request.write_text(
                json.dumps(render_request(binding, outside)), encoding="utf-8")

            rendered = subprocess.run([
                str(private), "--hwa-experiment-job", str(request),
                "--output-dir", str(job),
            ], check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
               text=True)
            self.assertNotEqual(rendered.returncode, 0)
            self.assertIn("output path", rendered.stderr)
            self.assertFalse(outside.exists())

    def test_renderer_is_deterministic_and_does_not_edit_the_cello_tree(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            completed, bundle, cello = build_fake_bundle(root)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            renderer = bundle / "renderer"
            bindings = json.loads((bundle / "bindings.json").read_text())
            binding = next(row for row in bindings["bindings"]
                           if row["id"] == "reference_c2_fit")
            source = cello / "src" / "hlolli_wg_cello.c"
            profile = cello / "model" / "cello-v1.json"
            before = (hashlib.sha256(source.read_bytes()).hexdigest(),
                      hashlib.sha256(profile.read_bytes()).hexdigest())
            hashes = []
            for index, value in enumerate((0.25, 0.25, 1.0)):
                job = root / "job-{}".format(index)
                job.mkdir()
                output = job / "model.wav"
                request = job / "request.json"
                request_value = render_request(binding, output)
                request_value["parameters"][0]["value"] = value
                request.write_text(
                    json.dumps(request_value),
                    encoding="utf-8")
                rendered = subprocess.run([
                    str(renderer), "--hwa-experiment-job", str(request),
                    "--output-dir", str(job),
                ], check=False, stdout=subprocess.PIPE,
                   stderr=subprocess.PIPE, text=True)
                self.assertEqual(rendered.returncode, 0, rendered.stderr)
                hashes.append(hashlib.sha256(output.read_bytes()).hexdigest())
                with wave.open(str(output), "rb") as stream:
                    self.assertEqual(stream.getparams()[:4],
                                     (2, 3, 44100, 529200))
                self.assertEqual(
                    sorted(path.name for path in job.iterdir()),
                    ["model.wav", "request.json"],
                )
            self.assertEqual(hashes[0], hashes[1])
            self.assertNotEqual(hashes[0], hashes[2])
            self.assertEqual(before, (
                hashlib.sha256(source.read_bytes()).hexdigest(),
                hashlib.sha256(profile.read_bytes()).hexdigest(),
            ))

    def test_renderer_dispatches_the_selected_string_slot(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            completed, bundle, cello = build_fake_bundle(root, "g2")
            self.assertEqual(completed.returncode, 0, completed.stderr)
            renderer = bundle / "renderer"
            bindings = json.loads((bundle / "bindings.json").read_text())
            binding = next(row for row in bindings["bindings"]
                           if row["id"] == "reference_g2_fit")
            profile = cello / "model" / "cello-v1.json"
            before = hashlib.sha256(profile.read_bytes()).hexdigest()
            hashes = []
            for index, value in enumerate((0.25, 1.0)):
                job = root / "g2-job-{}".format(index)
                job.mkdir()
                output = job / "model.wav"
                request = job / "request.json"
                request_value = render_request(
                    binding, output, target="g2", letter="g")
                request_value["parameters"][0]["value"] = value
                request.write_text(json.dumps(request_value), encoding="utf-8")
                rendered = subprocess.run([
                    str(renderer), "--hwa-experiment-job", str(request),
                    "--output-dir", str(job),
                ], check=False, stdout=subprocess.PIPE,
                   stderr=subprocess.PIPE, text=True)
                self.assertEqual(rendered.returncode, 0, rendered.stderr)
                hashes.append(hashlib.sha256(output.read_bytes()).hexdigest())
            self.assertNotEqual(hashes[0], hashes[1])
            self.assertEqual(before,
                             hashlib.sha256(profile.read_bytes()).hexdigest())


if __name__ == "__main__":
    unittest.main()

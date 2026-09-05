#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock
import wave


ROOT = Path(__file__).resolve().parents[1]
ADAPTER = ROOT / "adapters" / "hlolli_wg_violin" / "adapter.py"
FIT_MANIFEST = ROOT / "adapters" / "hlolli_wg_violin" / "fit.json"
FIT_TOOL = ROOT / "tools" / "instrument_fit.py"
PYTHON = Path(sys.executable).resolve()
ANALYZER = None
for index, argument in list(enumerate(sys.argv[1:], start=1)):
    if argument.startswith("--analyzer="):
        if ANALYZER is not None:
            raise RuntimeError("analyzer executable was supplied twice")
        ANALYZER = Path(argument.split("=", 1)[1]).resolve()
        del sys.argv[index]
        break


def load_adapter():
    spec = importlib.util.spec_from_file_location("violin_fit_adapter", ADAPTER)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load violin fit adapter")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_executable(path: Path, source: str) -> None:
    path.write_text(source, encoding="utf-8")
    path.chmod(0o700)


def noise_samples(frames: int, channels: int, seed: int = 0x13579BDF) -> bytes:
    state = seed
    samples = bytearray()
    for unused_frame in range(frames):
        state = (1103515245 * state + 12345) & 0x7FFFFFFF
        sample = ((state >> 8) & 0xFFFF) - 32768
        sample //= 4
        packed = struct.pack("<h", sample)
        samples.extend(packed * channels)
    return bytes(samples)


def phrase_samples(frames: int, channels: int, rate: int = 44100,
                   variant: int = 0) -> bytes:
    frequencies = (220.0, 293.664767917408, 329.627556912870, 440.0)
    samples = bytearray()
    for frame in range(frames):
        frequency = frequencies[min(3, frame * 4 // frames)]
        time = frame / rate
        value = 0.0
        for harmonic in range(1, 13):
            weight = (1.0 + 0.008 * variant * ((harmonic % 3) - 1)) / harmonic
            value += weight * math.sin(
                2.0 * math.pi * harmonic * frequency * time +
                0.03 * variant * harmonic)
        sample = max(-32767, min(32767, round(2600.0 * value)))
        samples.extend(struct.pack("<h", sample) * channels)
    return bytes(samples)


def note_samples(frequency: float, duration: float = 1.6,
                 rate: int = 44100) -> bytes:
    frames = round(rate * duration)
    lead = round(rate * 0.05)
    samples = bytearray()
    for frame in range(frames):
        value = 0.0
        if frame >= lead:
            time = (frame - lead) / rate
            value = 0.75 * math.exp(-time / 0.75) * (
                0.82 * math.sin(math.tau * frequency * time) +
                0.18 * math.sin(
                    2 * math.tau * frequency * time + 0.31))
        samples.extend(struct.pack(
            "<h", round(max(-1.0, min(1.0, value)) * 32767)))
    return bytes(samples)


def write_wave(path: Path, frames: int = 44100, channels: int = 1,
               rate: int = 44100, seed: int = 0x13579BDF,
               phrase_variant: int | None = None,
               note_frequency: float | None = None,
               note_duration: float = 1.6) -> None:
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(channels)
        stream.setsampwidth(2)
        stream.setframerate(rate)
        if note_frequency is not None:
            if channels != 1:
                raise ValueError("note fixture must be mono")
            audio = note_samples(note_frequency, note_duration, rate)
        elif phrase_variant is None:
            audio = noise_samples(frames, channels, seed)
        else:
            audio = phrase_samples(frames, channels, rate, phrase_variant)
        stream.writeframes(audio)


def fake_generator_source() -> str:
    return """import json

def load_json(path):
    with open(path, encoding='utf-8') as stream:
        return json.load(stream)

def validate_profile(value, unused_schema):
    if type(value) is not dict or set(value) != {'body', 'strings'}:
        raise ValueError('invalid violin profile root')
    body = value.get('body')
    strings = value.get('strings')
    if type(body) is not dict or set(body) != {'wet_gain'}:
        raise ValueError('invalid violin body')
    wet = body['wet_gain']
    if type(wet) not in (int, float) or type(wet) is bool or not 0.1 <= wet <= 0.55:
        raise ValueError('invalid body wet gain')
    if type(strings) is not list or len(strings) != 4:
        raise ValueError('invalid violin strings')
    bounds = ((4800, 8200), (2800, 5200), (4800, 8200), (4400, 7600))
    for row, limits in zip(strings, bounds):
        if type(row) is not dict or set(row) != {'bridge_cutoff_hz'}:
            raise ValueError('invalid violin string')
        value = row['bridge_cutoff_hz']
        if type(value) not in (int, float) or type(value) is bool:
            raise ValueError('invalid violin bridge cutoff')
        if not limits[0] <= value <= limits[1]:
            raise ValueError('violin bridge cutoff is out of range')
"""


def fake_csound_source(fail_render: bool = False) -> str:
    return """#!{} -I
import math
import struct
import sys
import wave

arguments = sys.argv[1:]
if '--version' in arguments:
    print('fake Csound')
    raise SystemExit(0)
if {}:
    print('declared Csound cannot render', file=sys.stderr)
    raise SystemExit(3)
try:
    index = arguments.index('-o')
    output = arguments[index + 1]
except (ValueError, IndexError):
    output = next((value[2:] for value in arguments
                   if value.startswith('-o') and len(value) > 2), None)
if output is None:
    raise SystemExit(2)
samples = bytearray()
frequencies = (220.0, 293.664767917408, 329.627556912870, 440.0)
for frame in range(176416):
    frequency = frequencies[min(3, frame * 4 // 176416)]
    time = frame / 44100.0
    value = sum(math.sin(2.0 * math.pi * harmonic * frequency * time) / harmonic
                for harmonic in range(1, 13))
    sample = max(-32767, min(32767, round(2600.0 * value)))
    samples.extend(struct.pack('<hh', sample, sample))
with wave.open(output, 'wb') as stream:
    stream.setnchannels(2)
    stream.setsampwidth(2)
    stream.setframerate(44100)
    stream.writeframes(bytes(samples))
""".format(PYTHON, repr(fail_render))


def fake_analyzer_source() -> str:
    return """#!{} -I
import json
import sys

arguments = sys.argv[1:]
if (len(arguments) != 7 or arguments[:2] != ['--json', 'isolated-note'] or
        arguments[3] != '--expected-hz' or
        arguments[5:] != ['--metrics', 'pitch']):
    print('wrong isolated-note request', file=sys.stderr)
    raise SystemExit(2)
expected_hz = float(arguments[4])
print(json.dumps({{
    'schema': 'hwa-isolated-note',
    'schema_version': 1,
    'command': 'isolated-note',
    'method': 'isolated-note-1',
    'path': arguments[2],
    'expected_hz': expected_hz,
    'requested_mask': 1,
    'valid_mask': 1,
    'requested_metrics': ['pitch'],
    'valid_metrics': ['pitch'],
    'pitch': {{
        'valid': True,
        'hz': expected_hz,
        'cents': 0.0,
        'confidence': 1.0,
        'coverage': 1.0,
    }},
}}, sort_keys=True, separators=(',', ':')))
""".format(PYTHON)


def minimal_profile() -> dict:
    return {
        "body": {"wet_gain": 0.45},
        "strings": [
            {"bridge_cutoff_hz": 5386.995271806526},
            {"bridge_cutoff_hz": 4964.244281108786},
            {"bridge_cutoff_hz": 7086.471045764144},
            {"bridge_cutoff_hz": 6024.580442039031},
        ],
    }


def fake_violin_tree(root: Path) -> Path:
    violin = root / "fake violin"
    (violin / "profiles" / "schema").mkdir(parents=True)
    (violin / "tools").mkdir()
    (violin / "examples").mkdir()
    (violin / "profiles" / "generic_violin.json").write_text(
        json.dumps(minimal_profile()), encoding="utf-8")
    (violin / "profiles" / "schema" /
     "violin-profile-v1.schema.json").write_text("{}\n", encoding="utf-8")
    (violin / "tools" / "generate_profiles.py").write_text(
        fake_generator_source(), encoding="utf-8")
    (violin / "examples" / "realism_diagnostic.csd").write_text(
        "<CsoundSynthesizer/>\n", encoding="utf-8")
    return violin


def build_fixture(root: Path, *, body_rate: int = 44100,
                  fail_render: bool = False, analyzer: Path | None = None,
                  open_frequencies: dict[str, float] | None = None,
                  open_durations: dict[str, float] | None = None):
    module = load_adapter()
    violin = fake_violin_tree(root)
    resources = root / "fake resources"
    references = root / "private references"
    resources.mkdir()
    references.mkdir()
    csound = resources / "fake csound"
    write_executable(csound, fake_csound_source(fail_render))
    if analyzer is None:
        analyzer = resources / "fake analyzer"
        write_executable(analyzer, fake_analyzer_source())
    opcode = resources / "lib violin test.so"
    opcode.write_bytes(b"hlolli_wg_violin_test_diagnostic_gains\n")
    csound_library = resources / "lib csound fake.so"
    csound_library.write_bytes(b"fake csound library\n")
    frequencies = {
        "g3": 195.997717990875,
        "d4": 293.664767917408,
        "a4": 440.0,
        "e5": 659.255113825740,
    }
    if open_frequencies is not None:
        frequencies.update(open_frequencies)
    durations = {name: 1.6 for name in frequencies}
    if open_durations is not None:
        durations.update(open_durations)
    reference_paths = {}
    for index, name in enumerate(
            ("g3", "d4", "a4", "e5", "body_fit", "body_check")):
        path = references / (name + ".wav")
        if name.startswith("body_"):
            write_wave(
                path, frames=5 * body_rate, rate=body_rate,
                phrase_variant=index + 1)
        else:
            write_wave(
                path, note_frequency=frequencies[name],
                note_duration=durations[name])
        reference_paths[name] = path
    output = root / "new violin bundle"
    arguments = types.SimpleNamespace(
        violin_root=violin,
        analyzer=analyzer,
        csound=csound,
        module=opcode,
        python=PYTHON,
        csound_library=csound_library,
        reference_g3=reference_paths["g3"],
        reference_d4=reference_paths["d4"],
        reference_a4=reference_paths["a4"],
        reference_e5=reference_paths["e5"],
        reference_body_fit=reference_paths["body_fit"],
        reference_body_check=reference_paths["body_check"],
        sample_count=1,
        body_reference_seconds=5.0,
        output_dir=output,
    )
    with mock.patch.object(module, "verify_loaded_library"):
        module.build_bundle(arguments)
    return module, output, violin, reference_paths


def binding_rows(bundle: Path) -> dict[str, dict]:
    value = json.loads((bundle / "bindings.json").read_text(encoding="utf-8"))
    return {row["id"]: row for row in value["bindings"]}


def render_request(binding: dict, output: Path) -> dict:
    manifest = json.loads(FIT_MANIFEST.read_text(encoding="utf-8"))
    parameters = [{
        "id": row["id"], "unit": row["unit"], "value": row["baseline"],
    } for row in sorted(manifest["parameters"], key=lambda item: item["id"])]
    reference = Path(binding["path"])
    with wave.open(str(reference), "rb") as stream:
        rate = stream.getframerate()
        channels = stream.getnchannels()
    return {
        "schema": "hwa-render-job",
        "schema_version": 1,
        "method_version": "stage8-1",
        "case_id": "open-g3",
        "job_id": 1,
        "job_key": "1" * 64,
        "inputs": [{
            "binding_id": "reference_g3",
            "channels": channels,
            "gain_db": 0,
            "kind": "stem",
            "path": str(reference),
            "probe_format": None,
            "probe_name": None,
            "rate_denominator": 0,
            "rate_hz": rate,
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
            "path": str(output),
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
        "parameters": parameters,
        "replicate": 0,
        "seed": 17012010,
        "split": "fit",
    }


class ViolinFitAdapterTests(unittest.TestCase):
    def test_manifest_baselines_match_the_fixed_profile(self) -> None:
        manifest = json.loads(FIT_MANIFEST.read_text(encoding="utf-8"))
        profile = minimal_profile()
        expected = {
            "body_wet_gain": ("ratio", 0.1, 0.55, 0.45,
                              ["body", "wet_gain"]),
            "bridge_cutoff_g_hz": ("Hz", 4800.0, 8200.0,
                                   5386.995271806526,
                                   ["strings", 0, "bridge_cutoff_hz"]),
            "bridge_cutoff_d_hz": ("Hz", 2800.0, 5200.0,
                                   4964.244281108786,
                                   ["strings", 1, "bridge_cutoff_hz"]),
            "bridge_cutoff_a_hz": ("Hz", 4800.0, 8200.0,
                                   7086.471045764144,
                                   ["strings", 2, "bridge_cutoff_hz"]),
            "bridge_cutoff_e_hz": ("Hz", 4400.0, 7600.0,
                                   6024.580442039031,
                                   ["strings", 3, "bridge_cutoff_hz"]),
        }
        self.assertEqual(manifest["schema"], "hwa-instrument-fit")
        self.assertEqual(manifest["schema_version"], 1)
        self.assertEqual(manifest["adapter_id"], "hlolli-wg-violin-v1")
        self.assertEqual({row["id"] for row in manifest["parameters"]},
                         set(expected))
        for row in manifest["parameters"]:
            unit, minimum, maximum, baseline, path = expected[row["id"]]
            self.assertEqual((row["unit"], row["minimum"], row["maximum"],
                              row["baseline"], row["profile_paths"]),
                             (unit, minimum, maximum, baseline, [path]))
            value = profile
            for part in path:
                value = value[part]
            self.assertEqual(float(value), float(baseline))
        self.assertEqual(
            {(row["kind"], row["split"]) for row in manifest["objectives"]},
            {("experiment-gap", "fit"), ("experiment-gap", "check"),
             ("body-envelope", "fit"), ("body-envelope", "check")},
        )

    def test_build_receipt_and_renderer_description(self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa violin contract ") as text:
            root = Path(text)
            old_umask = os.umask(0)
            try:
                unused_module, bundle, unused_violin, unused_refs = build_fixture(
                    root)
            finally:
                os.umask(old_umask)
            self.assertEqual(
                {path.name for path in bundle.iterdir()},
                {"renderer", "experiment.json", "fit.json", "bindings.json",
                 "receipt.json", "reference_body_fit.wav",
                 "reference_body_check.wav"},
            )
            receipt = json.loads((bundle / "receipt.json").read_text(
                encoding="utf-8"))
            self.assertEqual(receipt["schema"],
                             "hwa-violin-fit-bundle-receipt")
            self.assertEqual(receipt["schema_version"], 1)
            self.assertEqual(receipt["adapter_id"], "hlolli-wg-violin-v1")
            self.assertEqual(receipt["method_version"], "stage8-1")
            self.assertEqual(receipt["case_count"], 6)
            self.assertEqual(receipt["point_count"], 2)
            self.assertEqual(receipt["renderer_sha256"], sha256(bundle / "renderer"))
            self.assertEqual(receipt["experiment_sha256"],
                             sha256(bundle / "experiment.json"))
            self.assertEqual(receipt["fit_manifest_sha256"],
                             sha256(bundle / "fit.json"))
            self.assertEqual(receipt["bindings_sha256"],
                             sha256(bundle / "bindings.json"))
            resources = {row["id"]: row for row in receipt["resources"]}
            self.assertIn("analyzer", resources)
            self.assertEqual(resources["analyzer"]["sha256"],
                             sha256(Path(resources["analyzer"]["path"])))
            pitch_checks = {
                row["case_id"]: row
                for row in receipt["open_string_pitch_checks"]
            }
            self.assertEqual(set(pitch_checks), {
                "open-g3", "open-d4", "open-a4", "open-e5"})
            for case_id, expected_hz in {
                    "open-g3": 195.997717990875,
                    "open-d4": 293.664767917408,
                    "open-a4": 440.0,
                    "open-e5": 659.255113825740,
                    }.items():
                self.assertEqual(pitch_checks[case_id]["expected_hz"],
                                 expected_hz)
                self.assertEqual(pitch_checks[case_id]["measured_hz"],
                                 expected_hz)
            self.assertEqual({row["id"] for row in receipt["references"]},
                             {"reference_g3", "reference_d4", "reference_a4",
                              "reference_e5", "reference_body_fit",
                              "reference_body_check"})
            self.assertEqual(stat.S_IMODE(bundle.stat().st_mode), 0o700)
            for path in bundle.iterdir():
                wanted = 0o700 if path.name == "renderer" else 0o600
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), wanted)
            manifest = json.loads((bundle / "fit.json").read_text(
                encoding="utf-8"))
            bindings = binding_rows(bundle)
            body_objectives = {
                row["reference_binding"]: row
                for row in manifest["objectives"]
                if row["kind"] == "body-envelope"
            }
            self.assertEqual(set(body_objectives), {
                "reference_body_fit", "reference_body_check"})
            for name, row in body_objectives.items():
                self.assertEqual(row["reference_sha256"],
                                 bindings[name]["sha256"])

            completed = subprocess.run(
                [str(bundle / "renderer"), "--describe"], check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                env={},
            )
            self.assertEqual(
                completed.returncode, 0,
                "{}\n{}".format(completed.args, completed.stderr),
            )
            description = json.loads(completed.stdout)
            self.assertEqual(set(description), {
                "schema", "schema_version", "adapter_id", "method_version",
                "permissions", "cases", "parameters", "resources",
            })
            self.assertEqual(description["schema"], "hwa-violin-renderer")
            self.assertEqual(description["schema_version"], 1)
            self.assertEqual(description["adapter_id"], "hlolli-wg-violin-v1")
            self.assertEqual(description["method_version"], "stage8-1")
            self.assertEqual(description["permissions"], {
                "render": True, "validate_profile": True,
                "write_profile": False,
            })
            self.assertEqual({row["id"] for row in description["cases"]},
                             {"open-g3", "open-d4", "open-a4", "open-e5",
                              "body-fit", "body-check"})
            self.assertEqual({row["id"] for row in description["parameters"]},
                             {row["id"] for row in
                              json.loads(FIT_MANIFEST.read_text())["parameters"]})
            self.assertIn("analyzer", {
                row["id"] for row in description["resources"]})

    def test_build_rejects_unusable_body_rate_and_failed_render_probe(
            self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa violin build gates ") as text:
            root = Path(text)
            with self.assertRaisesRegex(ValueError, "body references must use 44100"):
                build_fixture(root / "wrong rate", body_rate=48000)
            with self.assertRaisesRegex(ValueError, "baseline render"):
                build_fixture(root / "bad renderer", fail_render=True)

    @unittest.skipIf(ANALYZER is None, "analyzer executable was not supplied")
    def test_build_rejects_wrong_open_string_pitch(self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa violin wrong pitch ") as text:
            root = Path(text)
            with self.assertRaisesRegex(
                    ValueError, "open-g3 pitch is not valid"):
                build_fixture(
                    root, analyzer=ANALYZER,
                    open_frequencies={"g3": 391.99543598175})
            self.assertFalse((root / "new violin bundle").exists())

    @unittest.skipIf(ANALYZER is None, "analyzer executable was not supplied")
    def test_build_rejects_open_string_with_too_little_support(self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa violin short note ") as text:
            root = Path(text)
            with self.assertRaisesRegex(
                    ValueError, "open-e5 pitch is not valid"):
                build_fixture(
                    root, analyzer=ANALYZER,
                    open_durations={"e5": 0.02})
            self.assertFalse((root / "new violin bundle").exists())

    def test_python_path_with_spaces_cannot_make_a_broken_shebang(self) -> None:
        module = load_adapter()
        with tempfile.TemporaryDirectory(prefix="hwa violin python ") as text:
            python = Path(text) / "python with space"
            write_executable(python, "#!/bin/sh\nexit 0\n")
            with self.assertRaisesRegex(ValueError, "renderer shebang"):
                module.checked_python_shebang(python)

    def test_renderer_rejects_request_authority_changes_and_resource_tamper(
            self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa violin renderer ") as text:
            root = Path(text)
            unused_module, bundle, unused_violin, unused_refs = build_fixture(root)
            renderer = bundle / "renderer"
            binding = binding_rows(bundle)["reference_g3"]
            for name in ("case", "output", "hash"):
                with self.subTest(name=name):
                    job = root / ("job " + name)
                    job.mkdir()
                    output = job / "model.wav"
                    request = render_request(binding, output)
                    if name == "case":
                        request["case_id"] = "unknown-case"
                    elif name == "output":
                        request["outputs"][0]["path"] = str(root / "outside.wav")
                    else:
                        request["inputs"][0]["sha256"] = "0" * 64
                    request_path = job / "request.json"
                    request_path.write_text(json.dumps(request), encoding="utf-8")
                    completed = subprocess.run(
                        [str(renderer), "--hwa-experiment-job", str(request_path),
                         "--output-dir", str(job)], check=False,
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                        env={}, cwd=job,
                    )
                    self.assertNotEqual(completed.returncode, 0)
                    self.assertFalse(output.exists())
                    self.assertFalse((root / "outside.wav").exists())

            receipt = json.loads((bundle / "receipt.json").read_text(
                encoding="utf-8"))
            analyzer = Path(next(
                row["path"] for row in receipt["resources"]
                if row["id"] == "analyzer"))
            analyzer.write_text("changed\n", encoding="utf-8")
            completed = subprocess.run(
                [str(renderer), "--describe"], check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                env={},
            )
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("changed", completed.stderr.lower())

    def test_profile_validation_and_validator_tamper(self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa violin profile ") as text:
            root = Path(text)
            unused_module, bundle, violin, unused_refs = build_fixture(root)
            renderer = bundle / "renderer"
            profile = violin / "profiles" / "generic_violin.json"
            bad = root / "bad-profile.json"
            bad.write_text('{"body":{},"strings":[]}', encoding="utf-8")
            good_result = subprocess.run(
                [str(renderer), "--validate-profile", str(profile)], check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env={})
            bad_result = subprocess.run(
                [str(renderer), "--validate-profile", str(bad)], check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env={})
            self.assertEqual(good_result.returncode, 0, good_result.stderr)
            self.assertNotEqual(bad_result.returncode, 0)

            generator = violin / "tools" / "generate_profiles.py"
            generator.write_text("changed\n", encoding="utf-8")
            changed_result = subprocess.run(
                [str(renderer), "--validate-profile", str(profile)], check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env={})
            self.assertNotEqual(changed_result.returncode, 0)
            self.assertIn("changed", changed_result.stderr.lower())

    @unittest.skipIf(ANALYZER is None, "analyzer executable was not supplied")
    def test_full_manifest_experiment_select_and_profile_receipt(self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa violin end to end ") as text:
            root = Path(text)
            unused_module, bundle, violin, unused_refs = build_fixture(
                root, analyzer=ANALYZER)
            experiment_result = root / "experiment-result"
            bindings = binding_rows(bundle)
            command = [
                str(ANALYZER), "--renderer", str(bundle / "renderer"),
                "--allow-run",
            ]
            for name in sorted(bindings):
                command.extend(["--bind", name + "=" + bindings[name]["path"]])
            command.extend([
                "--output", str(experiment_result), "experiment",
                str(bundle / "experiment.json"),
            ])
            completed = subprocess.run(
                command, check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={})
            self.assertEqual(
                completed.returncode, 0,
                "{}\n{}".format(completed.args, completed.stderr),
            )

            manifest_path = bundle / "fit.json"
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            source_profile = violin / "profiles" / "generic_violin.json"
            source_before = source_profile.read_bytes()
            bad_selection = root / "bad-selection.json"
            completed = subprocess.run([
                str(PYTHON), "-B", "-I", str(FIT_TOOL), "select",
                "--manifest", str(manifest_path),
                "--experiment", str(experiment_result / "result.hwa-experiment"),
                "--analyzer", str(ANALYZER),
                "--profile", str(source_profile),
                "--bind", "reference_body_fit=" +
                bindings["reference_body_check"]["path"],
                "--bind", "reference_body_check=" +
                bindings["reference_body_check"]["path"],
                "--output", str(bad_selection),
            ], check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
               text=True, env={})
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("wrong hash", completed.stderr)
            self.assertFalse(bad_selection.exists())

            selection = root / "selection.json"
            completed = subprocess.run([
                str(PYTHON), "-B", "-I", str(FIT_TOOL), "select",
                "--manifest", str(manifest_path),
                "--experiment", str(experiment_result / "result.hwa-experiment"),
                "--analyzer", str(ANALYZER),
                "--profile", str(source_profile),
                "--bind", "reference_body_fit=" +
                bindings["reference_body_fit"]["path"],
                "--bind", "reference_body_check=" +
                bindings["reference_body_check"]["path"],
                "--output", str(selection),
            ], check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
               text=True, env={})
            self.assertEqual(completed.returncode, 0, completed.stderr)
            selected = json.loads(selection.read_text(encoding="utf-8"))
            self.assertEqual(selected["status"], "pass")
            self.assertEqual(set(selected["chosen_parameters"]),
                             {row["id"] for row in manifest["parameters"]})
            chosen = next(
                row for row in selected["points"]
                if row["point_id"] == selected["chosen_point_id"])
            self.assertEqual(
                {row["objective"] for row in chosen["evidence"]},
                {row["id"] for row in manifest["objectives"]},
            )
            self.assertEqual(selected["profile_adapter_sha256"],
                             sha256(bundle / "renderer"))

            wrong_adapter = root / "wrong-renderer"
            wrong_adapter.write_bytes(
                (bundle / "renderer").read_bytes() + b"\n# changed\n")
            wrong_adapter.chmod(0o700)
            wrong_output = root / "wrong-adapter-profile.json"
            wrong_receipt = root / "wrong-adapter-receipt.json"
            completed = subprocess.run([
                str(PYTHON), "-B", "-I", str(FIT_TOOL), "write-profile",
                "--manifest", str(manifest_path),
                "--fit", str(selection),
                "--source", str(source_profile),
                "--adapter", str(wrong_adapter),
                "--output", str(wrong_output),
                "--receipt", str(wrong_receipt),
            ], check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
               text=True, env={})
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("does not match", completed.stderr)
            self.assertFalse(wrong_output.exists())
            self.assertFalse(wrong_receipt.exists())

            output_profile = root / "chosen-profile.json"
            receipt_path = root / "profile-receipt.json"
            completed = subprocess.run([
                str(PYTHON), "-B", "-I", str(FIT_TOOL), "write-profile",
                "--manifest", str(manifest_path),
                "--fit", str(selection),
                "--source", str(source_profile),
                "--adapter", str(bundle / "renderer"),
                "--output", str(output_profile),
                "--receipt", str(receipt_path),
            ], check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
               text=True, env={})
            self.assertEqual(completed.returncode, 0, completed.stderr)
            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
            self.assertEqual(receipt["schema"], "hwa-profile-write-receipt")
            self.assertEqual(receipt["schema_version"], 1)
            self.assertEqual(receipt["adapter_id"], "hlolli-wg-violin-v1")
            self.assertEqual(receipt["adapter_sha256"],
                             sha256(bundle / "renderer"))
            self.assertEqual(receipt["source_profile_sha256"],
                             hashlib.sha256(source_before).hexdigest())
            self.assertEqual(receipt["output_profile_sha256"],
                             sha256(output_profile))
            self.assertEqual(len(receipt["changes"]), 5)
            self.assertEqual(source_profile.read_bytes(), source_before)


if __name__ == "__main__":
    unittest.main()

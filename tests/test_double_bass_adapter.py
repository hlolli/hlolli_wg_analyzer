#!/usr/bin/env python3

import json
import hashlib
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
import wave


ROOT = Path(__file__).resolve().parents[1]
BUILDER = (ROOT / "adapters" / "hlolli_wg_double_bass" /
           "build_renderer.py")
PYTHON = Path(sys.executable).resolve()
ANALYZER = None
if len(sys.argv) == 2 and sys.argv[1].startswith("--analyzer="):
    ANALYZER = Path(sys.argv.pop(1).split("=", 1)[1]).resolve()

CASE_SPECS = (
    ("iowa2012-pizz-e-string-g1-ff-left-48k-soxr", "fit", 48000,
     1, 48.999429497718666),
    ("iowa2012-pizz-a-ff-open-left-48k-soxr", "fit", 48000,
     2, 55.0),
    ("iowa2012-pizz-d-ff-open-left-48k-soxr", "fit", 48000,
     3, 73.41619197935188),
    ("iowa2012-pizz-g-ff-open-left-48k-soxr", "fit", 48000,
     4, 97.99885899543733),
    ("iowa2012-pizz-e-string-g1-ff-left-48k-soxr-diagnostic-check",
     "check", 48000, 1, 48.999429497718666),
    ("iowa2012-pizz-a-ff-open-left-48k-soxr-diagnostic-check",
     "check", 48000, 2, 55.0),
    ("iowa2012-pizz-d-ff-open-left-48k-soxr-diagnostic-check",
     "check", 48000, 3, 73.41619197935188),
    ("iowa2012-pizz-g-ff-open-left-48k-soxr-diagnostic-check",
     "check", 48000, 4, 97.99885899543733),
    ("iowa2012-pizz-e-ff-open", "fit", 48000,
     1, 41.20344461410875),
    ("iowa2012-pizz-a-mf-open", "fit", 48000, 2, 55.0),
    ("iowa2012-pizz-d-mf-open", "fit", 48000,
     3, 73.41619197935188),
    ("iowa2012-pizz-g-pp-open", "fit", 48000,
     4, 97.99885899543733),
    ("iowa2001-pizz-mf-open-e1-heldout", "check", 44100,
     1, 41.20344461410875),
    ("iowa2001-pizz-mf-open-a1-heldout", "check", 44100,
     2, 55.0),
    ("iowa2001-pizz-mf-open-d2-heldout", "check", 44100,
     3, 73.41619197935188),
    ("iowa2001-pizz-mf-open-g2-heldout", "check", 44100,
     4, 97.99885899543733),
    ("iowa2001-pizz-mf-open-e1-heldout-48k-soxr", "check", 48000,
     1, 41.20344461410875),
    ("iowa2001-pizz-mf-open-a1-heldout-48k-soxr", "check", 48000,
     2, 55.0),
    ("iowa2001-pizz-mf-open-d2-heldout-48k-soxr", "check", 48000,
     3, 73.41619197935188),
    ("iowa2001-pizz-mf-open-g2-heldout-48k-soxr", "check", 48000,
     4, 97.99885899543733),
)
MAX_JSON_INPUT_BYTES = 1024 * 1024


def write(path: Path, data: bytes, mode: int = 0o600) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    path.chmod(mode)


def fixture(root: Path) -> Path:
    plugin = root / "plug in"
    write(plugin / "src" / "hlolli_wg_double_bass.c", b"source\n")
    model = {
        "valid": True,
        "strings": [
            {
                "bridge_cutoff_hz": 7000.0,
                "loss_time_constant_seconds": 0.25,
            } for _ in range(4)
        ],
    }
    write(plugin / "model" / "double_bass-v1.json",
          (json.dumps(model) + "\n").encode("utf-8"))
    write(
        plugin / "tools" / "generate_model.py",
        f"#!{PYTHON} -I\n".encode("ascii")
        + b"import argparse, json, pathlib, sys\n"
        b"p=argparse.ArgumentParser()\n"
        b"p.add_argument('--model', required=True)\n"
        b"p.add_argument('--source', required=True)\n"
        b"a=p.parse_args()\n"
        b"v=json.loads(pathlib.Path(a.model).read_text())\n"
        b"if v.get('valid') is not True:\n"
        b" sys.stderr.write('x'*20000+'VALIDATION-END')\n"
        b" raise SystemExit(7)\n"
        b"rows=v.get('strings',[])\n"
        b"losses=[row['loss_time_constant_seconds'] for row in rows] if len(rows)==4 else []\n"
        b"cutoffs=[row['bridge_cutoff_hz'] for row in rows] if len(rows)==4 else []\n"
        b"source=pathlib.Path(a.source)\n"
        b"source.write_text(source.read_text()+'\\n/*FAKE_MODEL_VALUES='+json.dumps(losses,separators=(',',':'))+'*/\\n/*FAKE_BRIDGE_CUTOFFS='+json.dumps(cutoffs,separators=(',',':'))+'*/\\n')\n")
    includes = root / "Csound headers"
    write(includes / "csdl.h", b"csdl\n")
    write(includes / "csoundCore.h", b"core\n")
    write(includes / "version.h", b"version\n")
    write(includes / "float-version.h", b"float version\n")
    sdk = root / "SDK"
    write(sdk / "SDKSettings.json", b"sdk\n")
    tools = root / "tools with spaces"
    compiler = tools / "compiler"
    csound = tools / "csound"
    runtime = tools / "csound runtime"
    write(runtime, b"runtime\n")
    write(
        compiler,
        f"#!{PYTHON} -I\n".encode("ascii")
        + b"import os, pathlib, sys\n"
        b"i=sys.argv.index('-o')\n"
        b"(pathlib.Path(os.environ['TMPDIR'])/'xcrun_db').write_bytes(b'cache')\n"
        b"source=next(pathlib.Path(value) for value in reversed(sys.argv) if value.endswith('.c'))\n"
        b"pathlib.Path(sys.argv[i+1]).write_bytes(source.read_bytes())\n",
        0o700)
    write(
        csound,
        f"#!{PYTHON} -I\n".encode("ascii")
        + b"import json, math, pathlib, re, sys, wave\n"
        b"i=sys.argv.index('-o')\n"
        b"p=pathlib.Path(sys.argv[i+1])\n"
        b"if '-3' not in sys.argv or '-f' in sys.argv or '-K' not in sys.argv: raise SystemExit(10)\n"
        b"text=pathlib.Path(sys.argv[-1]).read_text()\n"
        b"(pathlib.Path(sys.argv[0]).parent/'last.csd').write_text(text)\n"
        b"module=pathlib.Path(next(value.split('=',1)[1] for value in sys.argv if value.startswith('--opcode-lib='))).read_text()\n"
        b"values=json.loads(re.search(r'FAKE_MODEL_VALUES=(\\[[^]]*\\])',module).group(1))\n"
        b"cutoffs=json.loads(re.search(r'FAKE_BRIDGE_CUTOFFS=(\\[[^]]*\\])',module).group(1))\n"
        b"if sum(value != 0.25 for value in values)==1 and '  kGate = 1\\n' not in text: raise SystemExit(11)\n"
        b"line=next(value for value in text.splitlines() if 'hlolli_wg_double_bass kGate,' in value)\n"
        b"controls=[value.strip() for value in line.split('kGate,',1)[1].split(',')]\n"
        b"frequency=float(controls[0]); force=float(controls[1]); speed=float(controls[2]); position=float(controls[3]); articulation=int(controls[6]); string=int(controls[9])\n"
        b"expected={1:(41.20344461410875,48.999429497718666),2:(55.0,),3:(73.41619197935188,),4:(97.99885899543733,)}\n"
        b"valid=string in expected and any(abs(frequency-value)<1e-12 for value in expected[string]) and force==0.75\n"
        b"if not valid or speed!=0.8 or position!=0.12 or articulation!=5: raise SystemExit(9)\n"
        b"rate=int(re.search(r'^sr = ([0-9]+)$',text,re.MULTILINE).group(1))\n"
        b"if rate not in (44100,48000): raise SystemExit(8)\n"
        b"event=re.search(r'i \\\"Bass\\\" ([0-9.eE+-]+) ([0-9.eE+-]+)',text)\n"
        b"duration=float(event.group(1))+float(event.group(2))\n"
        b"frames=round(duration*rate)\n"
        b"frames=((frames+31)//32)*32\n"
        b"prefix=[round(value*10000) for value in values]+[string*1000,round(frequency*10),round(cutoffs[2])]\n"
        b"with wave.open(str(p),'wb') as w:\n"
        b" w.setnchannels(1); w.setsampwidth(3); w.setframerate(rate)\n"
        b" w.writeframes(b''.join(int(prefix[n] if n<len(prefix) else round(8000*math.sin(2*math.pi*frequency*n/rate))).to_bytes(3,'little',signed=True) for n in range(frames)))\n",
        0o700)
    config = {
        "schema": "hwa-double-bass-renderer-build",
        "schema_version": 1,
        "plugin_root": str(plugin),
        "csound_executable": str(csound),
        "csound_include_dir": str(includes),
        "c_compiler": str(compiler),
        "macos_sdk_root": str(sdk) if sys.platform == "darwin" else None,
        "python_executable": str(PYTHON),
        "extra_resources": [
            {"id": "csound_runtime", "path": str(runtime)},
        ],
        "permissions": {
            "render": True,
            "validate_profile": True,
            "write_profile": False,
        },
    }
    path = root / "local config.json"
    path.write_text(json.dumps(config), encoding="utf-8")
    return path


def add_joint_candidate_config(config: Path) -> None:
    value = json.loads(config.read_text(encoding="utf-8"))
    profile_path = (
        Path(value["plugin_root"]) / "model" / "double_bass-v1.json"
    )
    profile = json.loads(profile_path.read_text(encoding="utf-8"))
    changes = [
        {
            "after": 0.5, "before": 0.25, "maximum": 30.0,
            "minimum": 0.01, "parameter": "string_e_loss_seconds",
            "path": ["strings", 0, "loss_time_constant_seconds"],
            "source_fit_result_sha256": "1" * 64, "unit": "seconds",
        },
        {
            "after": 1.5, "before": 0.25, "maximum": 30.0,
            "minimum": 0.01, "parameter": "string_a_loss_seconds",
            "path": ["strings", 1, "loss_time_constant_seconds"],
            "source_fit_result_sha256": "2" * 64, "unit": "seconds",
        },
        {
            "after": 1500.0, "before": 7000.0, "maximum": 7086.471,
            "minimum": 1000.0, "parameter": "string_d_bridge_cutoff_hz",
            "path": ["strings", 2, "bridge_cutoff_hz"],
            "source_fit_result_sha256": "3" * 64, "unit": "hertz",
        },
        {
            "after": 3.0, "before": 0.25, "maximum": 3.0,
            "minimum": 0.25, "parameter": "string_d_loss_seconds",
            "path": ["strings", 2, "loss_time_constant_seconds"],
            "source_fit_result_sha256": "3" * 64, "unit": "seconds",
        },
        {
            "after": 0.5, "before": 0.25, "maximum": 30.0,
            "minimum": 0.01, "parameter": "string_g_loss_seconds",
            "path": ["strings", 3, "loss_time_constant_seconds"],
            "source_fit_result_sha256": "4" * 64, "unit": "seconds",
        },
    ]
    for row in changes:
        target = profile
        for part in row["path"][:-1]:
            target = target[part]
        target[row["path"][-1]] = row["after"]
    candidate_source = (
        json.dumps(profile, indent=2, allow_nan=False) + "\n"
    ).encode("utf-8")
    cases = {}
    specs = (
        ("e", 1, 48.999429497718666),
        ("a", 2, 55.0),
        ("d", 3, 73.41619197935188),
        ("g", 4, 97.99885899543733),
    )
    for letter, string_index, frequency in specs:
        for split in ("fit", "check"):
            name = f"joint-{letter}-{split}"
            cases[name] = {
                "articulation": 5, "binding_id": name,
                "binding_sha256": "0" * 64, "force": 0.75,
                "frequency_hz": frequency, "joint": True,
                "position": 0.12, "sample_rate": 48000, "speed": 0.8,
                "split": split, "string": string_index,
            }
    value["joint_candidate"] = {
        "adapter_id":
            "hlolli_wg_double_bass-passive-joint-validation-v1",
        "candidate_profile_sha256": hashlib.sha256(
            candidate_source
        ).hexdigest(),
        "cases": cases, "changes": changes,
    }
    config.write_text(json.dumps(value), encoding="utf-8")


def wave_file(path: Path, frames: int = 12000,
              rate_hz: int = 48000) -> None:
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(1)
        stream.setsampwidth(2)
        stream.setframerate(rate_hz)
        stream.writeframes(b"".join(
            struct.pack("<h", (index % 200) - 100)
            for index in range(frames)))


def extensible_wave_file(path: Path) -> None:
    frames = b"".join(
        int((index % 200) - 100).to_bytes(3, "little", signed=True)
        for index in range(12000)
    )
    pcm_guid = bytes.fromhex("0100000000001000800000aa00389b71")
    format_data = struct.pack(
        "<HHIIHHHHI16s", 0xfffe, 1, 48000, 144000, 3, 24,
        22, 24, 4, pcm_guid,
    )
    chunks = (b"fmt " + struct.pack("<I", len(format_data)) + format_data +
              b"data" + struct.pack("<I", len(frames)) + frames)
    path.write_bytes(
        b"RIFF" + struct.pack("<I", len(chunks) + 4) + b"WAVE" + chunks)


def file_hash(path: Path) -> str:
    import hashlib
    return hashlib.sha256(path.read_bytes()).hexdigest()


def experiment_manifest(check_reference: Path,
                        fit_reference: Path) -> dict:
    def case(name: str, split: str, binding: str) -> dict:
        return {
            "id": name,
            "split": split,
            "weight": 1,
            "stems": [
                {
                    "id": "model.final", "side": "model", "role": "final",
                    "input_id": None, "output": "model.wav",
                    "start_sample": 0, "gain_db": 0, "rate_hz": 48000,
                    "channels": 1,
                },
                {
                    "id": "reference.final", "side": "reference",
                    "role": "final", "input_id": binding,
                    "output": None, "start_sample": 0, "gain_db": 0,
                    "rate_hz": 48000, "channels": 1,
                },
            ],
            "probes": [],
            "links": [],
        }

    return {
        "schema": "hwa-experiment",
        "schema_version": 1,
        "method_version": "stage8-1",
        "clock_rate_hz": 48000,
        "inputs": [
            {"id": "iowa2001-pizz-mf-open-a1-heldout-48k-soxr",
             "sha256": file_hash(check_reference)},
            {"id": "iowa2012-pizz-e-ff-open",
             "sha256": file_hash(fit_reference)},
        ],
        "parameters": [
            {
                "id": f"string_{name}_loss_seconds", "unit": "seconds",
                "minimum": 0.2, "maximum": 0.3, "baseline": 0.25,
                "levels": [0.25],
            }
            for name in ("a", "d", "e", "g")
        ],
        "plan": {
            "kind": "one-at-a-time", "seed": 17,
            "sample_count": 0, "replicates": 1,
        },
        "cases": [
            case("iowa2001-pizz-mf-open-a1-heldout-48k-soxr", "check",
                 "iowa2001-pizz-mf-open-a1-heldout-48k-soxr"),
            case("iowa2012-pizz-e-ff-open", "fit",
                 "iowa2012-pizz-e-ff-open"),
        ],
        "responses": [
            {
                "id": "final.rms", "role": "final",
                "feature": "rms_dbfs", "index": 0,
            },
        ],
    }


def render_request(reference: Path, output: Path,
                   case_id: str = "iowa2012-pizz-e-ff-open",
                   split: str = "fit",
                   rate_hz: int = 48000,
                   values=None) -> dict:
    if values is None:
        values = {name: 0.25 for name in ("a", "d", "e", "g")}
    return {
        "schema": "hwa-render-job",
        "schema_version": 1,
        "method_version": "stage8-1",
        "case_id": case_id,
        "job_id": 1,
        "job_key": "1" * 64,
        "inputs": [{
            "binding_id": case_id,
            "channels": 1,
            "gain_db": 0,
            "kind": "stem",
            "path": str(reference),
            "probe_format": None,
            "probe_name": None,
            "rate_denominator": 0,
            "rate_hz": rate_hz,
            "rate_numerator": 0,
            "resource_id": "reference.final",
            "role": "final",
            "sha256": file_hash(reference),
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
            "rate_hz": rate_hz,
            "channels": 1,
            "rate_numerator": 0,
            "rate_denominator": 0,
            "value_count": 0,
        }],
        "parameters": [
            {"id": f"string_{name}_loss_seconds",
             "unit": "seconds", "value": values[name]}
            for name in ("a", "d", "e", "g")
        ],
        "replicate": 0,
        "seed": 17,
        "split": split,
    }


class DoubleBassAdapterTests(unittest.TestCase):
    def test_builder_emits_one_stable_checked_executable(self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa bass adapter ") as text:
            root = Path(text)
            config = fixture(root)
            first = root / "renderer one"
            second = root / "renderer two"
            for output in (first, second):
                completed = subprocess.run(
                    [str(PYTHON), "-I", str(BUILDER), "build",
                     "--config", str(config), "--output", str(output)],
                    check=False, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, text=True, env={})
                self.assertEqual(completed.returncode, 0, completed.stderr)

            self.assertEqual(first.read_bytes(), second.read_bytes())
            self.assertEqual(stat.S_IMODE(first.stat().st_mode), 0o700)
            description = subprocess.run(
                [str(first), "--describe"], check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={}, cwd=root)
            self.assertEqual(description.returncode, 0, description.stderr)
            value = json.loads(description.stdout)
            self.assertEqual(value["schema"], "hwa-double-bass-renderer")
            self.assertEqual(value["schema_version"], 1)
            self.assertEqual(value["adapter_id"], "hlolli_wg_double_bass")
            self.assertEqual(value["permissions"], {
                "render": True,
                "validate_profile": True,
                "write_profile": False,
            })
            expected_resources = [
                "c_compiler", "csound", "csound_include/csdl.h",
                "csound_include/csoundCore.h", "csound_include/float-version.h",
                "csound_include/version.h", "csound_runtime", "generator",
                "model", "python", "source",
            ]
            if sys.platform == "darwin":
                expected_resources.append("macos_sdk_settings")
            self.assertEqual(
                [row["id"] for row in value["resources"]],
                sorted(expected_resources))
            self.assertTrue(all(len(row["sha256"]) == 64
                                for row in value["resources"]))

            header = root / "Csound headers" / "csoundCore.h"
            header.write_bytes(b"changed core\n")
            changed_header = subprocess.run(
                [str(first), "--describe"], check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={}, cwd=root)
            self.assertNotEqual(changed_header.returncode, 0)
            self.assertIn(
                "configured resource changed: csound_include/csoundCore.h",
                changed_header.stderr)
            header.write_bytes(b"core\n")

            source = (root / "plug in" / "src" /
                      "hlolli_wg_double_bass.c")
            source.write_bytes(b"changed\n")
            changed = subprocess.run(
                [str(first), "--describe"], check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={}, cwd=root)
            self.assertNotEqual(changed.returncode, 0)
            self.assertIn("configured resource changed: source",
                          changed.stderr)

    def test_builder_binds_the_renderer_shebang_to_checked_python(self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa-bass-python-") as text:
            root = Path(text)
            config = fixture(root)
            value = json.loads(config.read_text(encoding="utf-8"))
            configured_python = (
                PYTHON.parent / ".." / PYTHON.parent.name / PYTHON.name
            )
            value["python_executable"] = str(configured_python)
            config.write_text(json.dumps(value), encoding="utf-8")
            renderer = root / "renderer"
            built = subprocess.run(
                [str(PYTHON), "-I", str(BUILDER), "build",
                 "--config", str(config), "--output", str(renderer)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={})
            self.assertEqual(built.returncode, 0, built.stderr)
            self.assertEqual(
                renderer.read_bytes().splitlines()[0],
                f"#!{configured_python} -I".encode("ascii"))
            described = subprocess.run(
                [str(renderer), "--describe"], check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={}, cwd=root)
            self.assertEqual(described.returncode, 0, described.stderr)
            description = json.loads(described.stdout)
            python_resource = next(
                row for row in description["resources"]
                if row["id"] == "python")
            self.assertEqual(python_resource["path"], str(configured_python))

            unsafe_python = root / "unsafe python"
            shutil.copyfile(PYTHON, unsafe_python)
            unsafe_python.chmod(0o700)
            value["python_executable"] = str(unsafe_python)
            config.write_text(json.dumps(value), encoding="utf-8")
            rejected = subprocess.run(
                [str(PYTHON), "-I", str(BUILDER), "build",
                 "--config", str(config), "--output", str(root / "unsafe")],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={})
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("python_executable is unsafe for a shebang",
                          rejected.stderr)

    def test_builder_and_renderer_reject_oversized_json_inputs(self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa bass json ") as text:
            root = Path(text)
            config = fixture(root)
            config_bytes = config.read_bytes()
            config.write_bytes(
                config_bytes +
                b" " * (MAX_JSON_INPUT_BYTES + 1 - len(config_bytes)))
            renderer = root / "renderer"
            rejected_config = subprocess.run(
                [str(PYTHON), "-I", str(BUILDER), "build",
                 "--config", str(config), "--output", str(renderer)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={})
            self.assertNotEqual(rejected_config.returncode, 0)
            self.assertIn("JSON input exceeds 1048576 bytes",
                          rejected_config.stderr)
            self.assertFalse(renderer.exists())

            config.write_bytes(config_bytes)
            built = subprocess.run(
                [str(PYTHON), "-I", str(BUILDER), "build",
                 "--config", str(config), "--output", str(renderer)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={})
            self.assertEqual(built.returncode, 0, built.stderr)

            profile = root / "large profile.json"
            profile_value = b'{"valid":true}'
            profile.write_bytes(
                profile_value +
                b" " * (MAX_JSON_INPUT_BYTES + 1 - len(profile_value)))
            rejected_profile = subprocess.run(
                [str(renderer), "--validate-profile", str(profile)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={}, cwd=root)
            self.assertNotEqual(rejected_profile.returncode, 0)
            self.assertIn("JSON input exceeds 1048576 bytes",
                          rejected_profile.stderr)

            reference = root / "reference.wav"
            wave_file(reference)
            output = root / "job"
            output.mkdir()
            model = output / "model.wav"
            request = root / "large request.json"
            request_value = json.dumps(
                render_request(reference, model)).encode("utf-8")
            request.write_bytes(
                request_value +
                b" " * (MAX_JSON_INPUT_BYTES + 1 - len(request_value)))
            rejected_request = subprocess.run(
                [str(renderer), "--hwa-experiment-job", str(request),
                 "--output-dir", str(output)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={}, cwd=output)
            self.assertNotEqual(rejected_request.returncode, 0)
            self.assertIn("JSON input exceeds 1048576 bytes",
                          rejected_request.stderr)
            self.assertFalse(model.exists())

    def test_profile_validation_uses_the_bound_generator(self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa bass profile ") as text:
            root = Path(text)
            config = fixture(root)
            renderer = root / "renderer"
            built = subprocess.run(
                [str(PYTHON), "-I", str(BUILDER), "build",
                 "--config", str(config), "--output", str(renderer)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={})
            self.assertEqual(built.returncode, 0, built.stderr)
            valid = root / "valid profile.json"
            invalid = root / "invalid profile.json"
            valid.write_text('{"valid":true}\n', encoding="utf-8")
            invalid.write_text('{"valid":false}\n', encoding="utf-8")
            source = (root / "plug in" / "src" /
                      "hlolli_wg_double_bass.c")
            before = source.read_bytes()

            accepted = subprocess.run(
                [str(renderer), "--validate-profile", str(valid)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={}, cwd=root)
            self.assertEqual(accepted.returncode, 0, accepted.stderr)
            rejected = subprocess.run(
                [str(renderer), "--validate-profile", str(invalid)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={}, cwd=root)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("profile validation failed", rejected.stderr)
            self.assertIn("VALIDATION-END", rejected.stderr)
            self.assertLess(len(rejected.stderr), 6000)
            self.assertEqual(source.read_bytes(), before)

    def test_renderer_accepts_one_checked_experiment_job(self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa bass render ") as text:
            root = Path(text)
            config = fixture(root)
            renderer = root / "renderer"
            built = subprocess.run(
                [str(PYTHON), "-I", str(BUILDER), "build",
                 "--config", str(config), "--output", str(renderer)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={})
            self.assertEqual(built.returncode, 0, built.stderr)
            reference = root / "reference.wav"
            wave_file(reference)
            output = root / "job output"
            output.mkdir()
            model = output / "model.wav"
            job = {
                "schema": "hwa-render-job",
                "schema_version": 1,
                "method_version": "stage8-1",
                "case_id": "iowa2012-pizz-e-ff-open",
                "job_id": 1,
                "job_key": "1" * 64,
                "inputs": [{
                    "binding_id": "iowa2012-pizz-e-ff-open",
                    "channels": 1,
                    "gain_db": 0,
                    "kind": "stem",
                    "path": str(reference),
                    "probe_format": None,
                    "probe_name": None,
                    "rate_denominator": 0,
                    "rate_hz": 48000,
                    "rate_numerator": 0,
                    "resource_id": "reference.final",
                    "role": "final",
                    "sha256": file_hash(reference),
                    "side": "reference",
                    "start_sample": 0,
                    "unit": None,
                    "value_count": 0,
                }],
                "outputs": [{
                    "id": "model.final",
                    "kind": "stem",
                    "path": str(model),
                    "side": "model",
                    "role": "final",
                    "probe_format": None,
                    "probe_name": None,
                    "unit": None,
                    "start_sample": 0,
                    "gain_db": 0,
                    "rate_hz": 48000,
                    "channels": 1,
                    "rate_numerator": 0,
                    "rate_denominator": 0,
                    "value_count": 0,
                }],
                "parameters": [
                    {"id": f"string_{name}_loss_seconds",
                     "unit": "seconds", "value": 0.25}
                    for name in ("a", "d", "e", "g")
                ],
                "replicate": 0,
                "seed": 17,
                "split": "fit",
            }
            request = root / "request.json"
            request.write_text(json.dumps(job), encoding="utf-8")
            completed = subprocess.run(
                [str(renderer), "--hwa-experiment-job", str(request),
                 "--output-dir", str(output)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={}, cwd=output)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual([path.name for path in output.iterdir()],
                             ["model.wav"])
            with wave.open(str(model), "rb") as stream:
                self.assertEqual(stream.getframerate(), 48000)
                self.assertEqual(stream.getnchannels(), 1)
                self.assertGreater(stream.getnframes(), 0)

    def test_renderer_accepts_extensible_pcm24_reference(self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa bass pcm24 ") as text:
            root = Path(text)
            config = fixture(root)
            renderer = root / "renderer"
            built = subprocess.run(
                [str(PYTHON), "-I", str(BUILDER), "build",
                 "--config", str(config), "--output", str(renderer)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={})
            self.assertEqual(built.returncode, 0, built.stderr)
            reference = root / "reference-extensible.wav"
            extensible_wave_file(reference)
            output = root / "job output"
            output.mkdir()
            model = output / "model.wav"
            request = root / "request.json"
            request.write_text(
                json.dumps(render_request(reference, model)), encoding="utf-8")

            completed = subprocess.run(
                [str(renderer), "--hwa-experiment-job", str(request),
                 "--output-dir", str(output)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={}, cwd=output)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertTrue(model.is_file())

    def test_renderer_keeps_each_case_at_its_authorized_rate(self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa bass rates ") as text:
            root = Path(text)
            config = fixture(root)
            renderer = root / "renderer"
            built = subprocess.run(
                [str(PYTHON), "-I", str(BUILDER), "build",
                 "--config", str(config), "--output", str(renderer)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={})
            self.assertEqual(built.returncode, 0, built.stderr)

            for index, (case_id, split, rate_hz, string_index,
                        frequency_hz) in enumerate(CASE_SPECS):
                with self.subTest(case_id=case_id):
                    reference = root / f"reference-{index}.wav"
                    wave_file(reference, frames=2207, rate_hz=rate_hz)
                    output = root / f"job-{index}"
                    output.mkdir()
                    model = output / "model.wav"
                    request = root / f"request-{index}.json"
                    request_value = render_request(
                        reference, model, case_id=case_id, split=split,
                        rate_hz=rate_hz,
                    )
                    if case_id.endswith("-diagnostic-check"):
                        request_value["inputs"][0]["binding_id"] = (
                            case_id.removesuffix("-diagnostic-check")
                        )
                    request.write_text(
                        json.dumps(request_value), encoding="utf-8")
                    completed = subprocess.run(
                        [str(renderer), "--hwa-experiment-job", str(request),
                         "--output-dir", str(output)],
                        check=False, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True, env={}, cwd=output)
                    self.assertEqual(
                        completed.returncode, 0, completed.stderr)
                    with wave.open(str(model), "rb") as stream:
                        self.assertEqual(stream.getframerate(), rate_hz)
                        self.assertEqual(stream.getsampwidth(), 3)
                        self.assertEqual(stream.getnframes(), 2207)
                        first = stream.readframes(6)
                    samples = tuple(
                        int.from_bytes(
                            first[offset:offset + 3], "little", signed=True)
                        for offset in range(0, 18, 3)
                    )
                    self.assertEqual(
                        samples[4:],
                        (string_index * 1000, round(frequency_hz * 10)))

            native_case = "iowa2001-pizz-mf-open-a1-heldout"
            for mismatch in ("input", "output", "file"):
                with self.subTest(mismatch=mismatch):
                    reference = root / f"mismatch-{mismatch}.wav"
                    wave_file(
                        reference, frames=2207,
                        rate_hz=48000 if mismatch == "file" else 44100)
                    output = root / f"mismatch-{mismatch}"
                    output.mkdir()
                    model = output / "model.wav"
                    value = render_request(
                        reference, model, case_id=native_case,
                        split="check", rate_hz=44100)
                    if mismatch == "input":
                        value["inputs"][0]["rate_hz"] = 48000
                    elif mismatch == "output":
                        value["outputs"][0]["rate_hz"] = 48000
                    request = root / f"mismatch-{mismatch}.json"
                    request.write_text(json.dumps(value), encoding="utf-8")
                    completed = subprocess.run(
                        [str(renderer), "--hwa-experiment-job", str(request),
                         "--output-dir", str(output)],
                        check=False, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True, env={}, cwd=output)
                    self.assertNotEqual(completed.returncode, 0)
                    self.assertFalse(model.exists())

    def test_renderer_produces_byte_equal_fresh_waves(self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa bass repeat ") as text:
            root = Path(text)
            config = fixture(root)
            renderer = root / "renderer"
            built = subprocess.run(
                [str(PYTHON), "-I", str(BUILDER), "build",
                 "--config", str(config), "--output", str(renderer)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={})
            self.assertEqual(built.returncode, 0, built.stderr)
            reference = root / "reference.wav"
            wave_file(reference)
            hashes = []
            for index in range(2):
                output = root / f"job-{index}"
                output.mkdir()
                model = output / "model.wav"
                request = root / f"request-{index}.json"
                request.write_text(
                    json.dumps(render_request(reference, model)),
                    encoding="utf-8")
                completed = subprocess.run(
                    [str(renderer), "--hwa-experiment-job", str(request),
                     "--output-dir", str(output)],
                    check=False, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, text=True, env={}, cwd=output)
                self.assertEqual(completed.returncode, 0, completed.stderr)
                hashes.append(file_hash(model))
            self.assertEqual(hashes[0], hashes[1])

    def test_renderer_trims_the_last_control_block_to_reference_frames(self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa bass frames ") as text:
            root = Path(text)
            config = fixture(root)
            renderer = root / "renderer"
            built = subprocess.run(
                [str(PYTHON), "-I", str(BUILDER), "build",
                 "--config", str(config), "--output", str(renderer)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={})
            self.assertEqual(built.returncode, 0, built.stderr)
            reference = root / "reference.wav"
            wave_file(reference, frames=12011)
            output = root / "job"
            output.mkdir()
            model = output / "model.wav"
            request = root / "request.json"
            request.write_text(
                json.dumps(render_request(reference, model)), encoding="utf-8")
            completed = subprocess.run(
                [str(renderer), "--hwa-experiment-job", str(request),
                 "--output-dir", str(output)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={}, cwd=output)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            with wave.open(str(model), "rb") as stream:
                self.assertEqual(stream.getnframes(), 12011)
            raw = model.read_bytes()
            data_header = raw.index(b"data")
            data_offset = data_header + 8
            data_bytes = int.from_bytes(
                raw[data_header + 4:data_header + 8], "little")
            self.assertEqual(data_bytes, 12011 * 3)
            self.assertEqual(len(raw), data_offset + data_bytes + 1)
            self.assertEqual(int.from_bytes(raw[4:8], "little") + 8,
                             len(raw))
            self.assertEqual(raw[data_offset + data_bytes], 0)

    def test_distinct_loss_values_reach_the_expected_model_strings(self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa bass values ") as text:
            root = Path(text)
            config = fixture(root)
            renderer = root / "renderer"
            built = subprocess.run(
                [str(PYTHON), "-I", str(BUILDER), "build",
                 "--config", str(config), "--output", str(renderer)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={})
            self.assertEqual(built.returncode, 0, built.stderr)
            reference = root / "reference.wav"
            wave_file(reference)
            output = root / "job"
            output.mkdir()
            model = output / "model.wav"
            request = root / "request.json"
            request.write_text(json.dumps(render_request(
                reference, model,
                values={"a": 0.21, "d": 0.22, "e": 0.23, "g": 0.24},
            )), encoding="utf-8")
            completed = subprocess.run(
                [str(renderer), "--hwa-experiment-job", str(request),
                 "--output-dir", str(output)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={}, cwd=output)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            with wave.open(str(model), "rb") as stream:
                data = stream.readframes(4)
                first = tuple(
                    int.from_bytes(data[offset:offset + 3], "little", signed=True)
                    for offset in range(0, 12, 3)
                )
            self.assertEqual(first, (2300, 2100, 2200, 2400))

    def test_v2_fit_job_changes_only_its_named_string_and_keeps_gate_high(
            self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa bass v2 scalar ") as text:
            root = Path(text)
            config = fixture(root)
            renderer = root / "renderer"
            built = subprocess.run(
                [str(PYTHON), "-I", str(BUILDER), "build",
                 "--config", str(config), "--output", str(renderer)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={})
            self.assertEqual(built.returncode, 0, built.stderr)
            reference = root / "reference.wav"
            wave_file(reference)
            output = root / "job"
            output.mkdir()
            model = output / "model.wav"
            request_value = render_request(reference, model)
            request_value["parameters"] = [{
                "id": "string_e_loss_seconds",
                "unit": "seconds",
                "value": 0.23,
            }]
            request = root / "request.json"
            request.write_text(json.dumps(request_value), encoding="utf-8")
            completed = subprocess.run(
                [str(renderer), "--hwa-experiment-job", str(request),
                 "--output-dir", str(output)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={}, cwd=output)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            with wave.open(str(model), "rb") as stream:
                data = stream.readframes(4)
            values = tuple(
                int.from_bytes(data[offset:offset + 3], "little", signed=True)
                for offset in range(0, 12, 3)
            )
            self.assertEqual(values, (2300, 2500, 2500, 2500))
            csd = (root / "tools with spaces" / "last.csd").read_text(
                encoding="utf-8")
            self.assertIn("  kGate = 1\n", csd)
            event = next(line for line in csd.splitlines()
                         if line.startswith('i "Bass" '))
            self.assertGreater(float(event.split()[2]), 0.0)

            check_output = root / "check-job"
            check_output.mkdir()
            check_model = check_output / "model.wav"
            check_value = render_request(
                reference, check_model,
                case_id=(
                    "iowa2012-pizz-e-string-g1-ff-left-48k-soxr-"
                    "diagnostic-check"
                ),
                split="check",
            )
            check_value["inputs"][0]["binding_id"] = (
                "iowa2012-pizz-e-string-g1-ff-left-48k-soxr"
            )
            check_value["parameters"] = [{
                "id": "string_e_loss_seconds",
                "unit": "seconds",
                "value": 0.23,
            }]
            check_request = root / "check-request.json"
            check_request.write_text(
                json.dumps(check_value), encoding="utf-8")
            checked = subprocess.run(
                [str(renderer), "--hwa-experiment-job", str(check_request),
                 "--output-dir", str(check_output)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={}, cwd=check_output)
            self.assertEqual(checked.returncode, 0, checked.stderr)
            self.assertTrue(check_model.is_file())
            csd = (root / "tools with spaces" / "last.csd").read_text(
                encoding="utf-8")
            self.assertIn("  kGate = 1\n", csd)
            event = next(line for line in csd.splitlines()
                         if line.startswith('i "Bass" '))
            self.assertGreater(float(event.split()[2]), 0.0)

            wrong_output = root / "wrong-job"
            wrong_output.mkdir()
            wrong_model = wrong_output / "model.wav"
            wrong_value = render_request(reference, wrong_model)
            wrong_value["parameters"] = [{
                "id": "string_a_loss_seconds",
                "unit": "seconds",
                "value": 0.23,
            }]
            wrong_request = root / "wrong-request.json"
            wrong_request.write_text(json.dumps(wrong_value), encoding="utf-8")
            rejected = subprocess.run(
                [str(renderer), "--hwa-experiment-job", str(wrong_request),
                 "--output-dir", str(wrong_output)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={}, cwd=wrong_output)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("wrong parameter set", rejected.stderr)
            self.assertFalse(wrong_model.exists())

    def test_d_frequency_job_changes_only_d_loss_and_bridge_cutoff(
            self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa bass d frequency ") as text:
            root = Path(text)
            config = fixture(root)
            renderer = root / "renderer"
            built = subprocess.run(
                [str(PYTHON), "-I", str(BUILDER), "build",
                 "--config", str(config), "--output", str(renderer)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={})
            self.assertEqual(built.returncode, 0, built.stderr)
            reference = root / "reference.wav"
            wave_file(reference)
            output = root / "job"
            output.mkdir()
            model = output / "model.wav"
            case_id = "iowa2012-pizz-d-ff-open-left-48k-soxr"
            request_value = render_request(
                reference, model, case_id=case_id, split="fit",
            )
            request_value["parameters"] = [
                {
                    "id": "string_d_loss_seconds",
                    "unit": "seconds",
                    "value": 2.0,
                },
                {
                    "id": "string_d_bridge_cutoff_hz",
                    "unit": "hertz",
                    "value": 1500.0,
                },
            ]
            request = root / "request.json"
            request.write_text(json.dumps(request_value), encoding="utf-8")
            completed = subprocess.run(
                [str(renderer), "--hwa-experiment-job", str(request),
                 "--output-dir", str(output)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={}, cwd=output)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            with wave.open(str(model), "rb") as stream:
                data = stream.readframes(7)
            values = tuple(
                int.from_bytes(data[offset:offset + 3], "little", signed=True)
                for offset in range(0, 21, 3)
            )
            self.assertEqual(
                values, (2500, 2500, 20000, 2500, 3000, 734, 1500))
            csd = (root / "tools with spaces" / "last.csd").read_text(
                encoding="utf-8")
            self.assertIn("  kGate = 1\n", csd)

            for name, change in (
                    ("wrong unit", lambda value: value["parameters"][1].update(
                        unit="seconds")),
                    ("wrong case", lambda value: value.update(
                        case_id="iowa2012-pizz-a-ff-open-left-48k-soxr"))):
                with self.subTest(name=name):
                    rejected_output = root / name.replace(" ", "-")
                    rejected_output.mkdir()
                    rejected_model = rejected_output / "model.wav"
                    rejected_value = render_request(
                        reference, rejected_model,
                        case_id=case_id, split="fit",
                    )
                    rejected_value["parameters"] = [dict(row)
                                                     for row in request_value[
                                                         "parameters"]]
                    change(rejected_value)
                    rejected_request = root / (name.replace(" ", "-") +
                                               ".json")
                    rejected_request.write_text(
                        json.dumps(rejected_value), encoding="utf-8")
                    rejected = subprocess.run(
                        [str(renderer), "--hwa-experiment-job",
                         str(rejected_request), "--output-dir",
                         str(rejected_output)],
                        check=False, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True, env={},
                        cwd=rejected_output)
                    self.assertNotEqual(rejected.returncode, 0)
                    self.assertFalse(rejected_model.exists())

    def test_joint_candidate_changes_only_the_five_declared_values(
            self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa bass joint ") as text:
            root = Path(text)
            config = fixture(root)
            reference = root / "reference.wav"
            wave_file(reference)
            add_joint_candidate_config(config)
            value = json.loads(config.read_text(encoding="utf-8"))
            for case in value["joint_candidate"]["cases"].values():
                case["binding_sha256"] = file_hash(reference)
            config.write_text(json.dumps(value), encoding="utf-8")
            v1_renderer = root / "v1-renderer"
            built = subprocess.run(
                [str(PYTHON), "-I", str(BUILDER), "build",
                 "--config", str(config), "--output", str(v1_renderer)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={})
            self.assertEqual(built.returncode, 0, built.stderr)

            value["joint_candidate"]["adapter_id"] = (
                "hlolli_wg_double_bass-passive-joint-validation-v2"
            )
            config.write_text(json.dumps(value), encoding="utf-8")
            renderer = root / "v2-renderer"
            built = subprocess.run(
                [str(PYTHON), "-I", str(BUILDER), "build",
                 "--config", str(config), "--output", str(renderer)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={})
            self.assertEqual(built.returncode, 0, built.stderr)

            output = root / "joint-output"
            output.mkdir()
            model = output / "model.wav"
            request_value = render_request(
                reference, model, case_id="joint-e-fit", split="fit",
            )
            request_value["inputs"][0]["binding_id"] = "joint-e-fit"
            request_value["parameters"] = [{
                "id": "joint_candidate", "unit": "choice", "value": 1.0,
            }]
            request = root / "joint-request.json"
            request.write_text(json.dumps(request_value), encoding="utf-8")
            completed = subprocess.run(
                [str(renderer), "--hwa-experiment-job", str(request),
                 "--output-dir", str(output)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={}, cwd=output)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            with wave.open(str(model), "rb") as stream:
                data = stream.readframes(7)
            values = tuple(
                int.from_bytes(data[offset:offset + 3], "little", signed=True)
                for offset in range(0, 21, 3)
            )
            self.assertEqual(
                values, (5000, 15000, 30000, 5000, 1000, 490, 1500)
            )

            rejected_output = root / "rejected-joint"
            rejected_output.mkdir()
            rejected_model = rejected_output / "model.wav"
            rejected_value = render_request(
                reference, rejected_model,
                case_id="joint-e-fit", split="fit",
            )
            rejected_value["inputs"][0]["binding_id"] = "joint-e-fit"
            rejected_value["parameters"] = [{
                "id": "joint_candidate", "unit": "choice", "value": 0.5,
            }]
            rejected_request = root / "rejected-joint.json"
            rejected_request.write_text(
                json.dumps(rejected_value), encoding="utf-8"
            )
            rejected = subprocess.run(
                [str(renderer), "--hwa-experiment-job",
                 str(rejected_request), "--output-dir",
                 str(rejected_output)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={},
                cwd=rejected_output)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertFalse(rejected_model.exists())

    def test_renderer_rejects_case_and_output_authority_changes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa bass authority ") as text:
            root = Path(text)
            config = fixture(root)
            renderer = root / "renderer"
            built = subprocess.run(
                [str(PYTHON), "-I", str(BUILDER), "build",
                 "--config", str(config), "--output", str(renderer)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={})
            self.assertEqual(built.returncode, 0, built.stderr)
            reference = root / "reference.wav"
            wave_file(reference)

            for name in ("binding", "split", "case", "output"):
                with self.subTest(name=name):
                    output = root / ("job-" + name)
                    output.mkdir()
                    model = output / "model.wav"
                    request_value = render_request(reference, model)
                    if name == "binding":
                        request_value["inputs"][0]["binding_id"] = (
                            "iowa2001-pizz-mf-open-a1-heldout-48k-soxr")
                    elif name == "split":
                        request_value["split"] = "check"
                    elif name == "case":
                        request_value["case_id"] = "unknown-case"
                    else:
                        request_value["outputs"][0]["path"] = str(
                            root / "outside.wav")
                    request = root / ("request-" + name + ".json")
                    request.write_text(
                        json.dumps(request_value), encoding="utf-8")
                    completed = subprocess.run(
                        [str(renderer), "--hwa-experiment-job", str(request),
                         "--output-dir", str(output)],
                        check=False, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True, env={}, cwd=output)
                    self.assertNotEqual(completed.returncode, 0)
                    self.assertFalse(model.exists())
                    self.assertFalse((root / "outside.wav").exists())

    @unittest.skipIf(ANALYZER is None, "analyzer executable was not supplied")
    def test_public_experiment_and_resume_use_the_frozen_renderer(self) -> None:
        with tempfile.TemporaryDirectory(prefix="hwa bass experiment ") as text:
            root = Path(text)
            config = fixture(root)
            renderer = root / "renderer"
            built = subprocess.run(
                [str(PYTHON), "-I", str(BUILDER), "build",
                 "--config", str(config), "--output", str(renderer)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={})
            self.assertEqual(built.returncode, 0, built.stderr)
            check_reference = root / "check-reference.wav"
            fit_reference = root / "fit-reference.wav"
            wave_file(check_reference, frames=12032)
            wave_file(fit_reference)
            manifest = root / "experiment.json"
            manifest.write_text(
                json.dumps(experiment_manifest(check_reference, fit_reference)),
                encoding="utf-8")
            fresh = root / "fresh"
            resumed = root / "resumed"

            first = subprocess.run(
                [str(ANALYZER), "--renderer", str(renderer), "--allow-run",
                 "--bind", "iowa2001-pizz-mf-open-a1-heldout-48k-soxr=" +
                 str(check_reference),
                 "--bind", "iowa2012-pizz-e-ff-open=" + str(fit_reference),
                 "--output", str(fresh), "experiment", str(manifest)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={})
            self.assertEqual(first.returncode, 0, first.stderr)
            second = subprocess.run(
                [str(ANALYZER), "--renderer", str(renderer), "--allow-run",
                 "--bind", "iowa2001-pizz-mf-open-a1-heldout-48k-soxr=" +
                 str(check_reference),
                 "--bind", "iowa2012-pizz-e-ff-open=" + str(fit_reference),
                 "--resume-from", str(fresh), "--output", str(resumed),
                 "experiment", str(manifest)],
                check=False, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, env={})
            self.assertEqual(second.returncode, 0, second.stderr)

            first_result = fresh / "result.hwa-experiment"
            second_result = resumed / "result.hwa-experiment"
            self.assertEqual(first_result.read_bytes(),
                             second_result.read_bytes())
            self.assertIn(file_hash(renderer).encode("ascii"),
                          first_result.read_bytes())
            self.assertFalse(any(
                path.name in {"request.json", "stdout.txt", "stderr.txt"}
                for path in fresh.rglob("*")
            ))


if __name__ == "__main__":
    unittest.main()

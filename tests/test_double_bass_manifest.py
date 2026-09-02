#!/usr/bin/env python3

import json
import hashlib
import importlib.util
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock
import wave


ROOT = Path(__file__).resolve().parents[1]
ADAPTER = ROOT / "adapters" / "hlolli_wg_double_bass"
BUILDER = ADAPTER / "build_manifest.py"
FIT = ADAPTER / "fit-passive-open-v1.json"
CONTRACT = ADAPTER / "reference-contract-v1.json"
PYTHON = Path("/usr/bin/python3")
MODULE_SPEC = importlib.util.spec_from_file_location(
    "double_bass_build_manifest", BUILDER)
assert MODULE_SPEC is not None and MODULE_SPEC.loader is not None
MODULE = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(MODULE)
ROWS = (
    ("a", "iowa2012-pizz-a-mf-open",
     "iowa2001-pizz-mf-open-a1-heldout",
     "iowa2001-pizz-mf-open-a1-heldout-48k-soxr"),
    ("d", "iowa2012-pizz-d-mf-open",
     "iowa2001-pizz-mf-open-d2-heldout",
     "iowa2001-pizz-mf-open-d2-heldout-48k-soxr"),
    ("e", "iowa2012-pizz-e-ff-open",
     "iowa2001-pizz-mf-open-e1-heldout",
     "iowa2001-pizz-mf-open-e1-heldout-48k-soxr"),
    ("g", "iowa2012-pizz-g-pp-open",
     "iowa2001-pizz-mf-open-g2-heldout",
     "iowa2001-pizz-mf-open-g2-heldout-48k-soxr"),
)


def strings(value):
    if isinstance(value, str):
        yield value
    elif isinstance(value, list):
        for item in value:
            yield from strings(item)
    elif isinstance(value, dict):
        for key, item in value.items():
            yield key
            yield from strings(item)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def wave_file(path, rate, width, frames):
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(1)
        stream.setsampwidth(width)
        stream.setframerate(rate)
        stream.writeframes(b"".join(
            int((index % 201) - 100).to_bytes(width, "little", signed=True)
            for index in range(frames)))


def fake_ffmpeg(path, nonrepeatable=False):
    path.write_text(
        "#!/usr/bin/python3 -I\n"
        "import pathlib, sys, wave\n"
        "if sys.argv[1:] == ['-version']:\n"
        " print('ffmpeg version 8.1.2-test Copyright test'); raise SystemExit(0)\n"
        "a=sys.argv[1:]\n"
        "expected=['-nostdin','-hide_banner','-loglevel','error','-xerror','-i','{source}','-map_metadata','-1','-vn','-sn','-dn','-ac','1','-af','aresample=resampler=soxr:precision=33:dither_method=none','-ar','48000','-c:a','pcm_s24le','-fflags','+bitexact','-flags:a','+bitexact','-f','wav','{derived}']\n"
        "if len(a)!=len(expected) or any(w not in ('{source}','{derived}') and w!=v for w,v in zip(expected,a)):\n"
        " raise SystemExit(9)\n"
        "source=pathlib.Path(a[6]); output=pathlib.Path(a[-1])\n"
        "with wave.open(str(source),'rb') as r:\n"
        " if r.getnchannels()!=1 or r.getsampwidth()!=2 or r.getframerate()!=44100: raise SystemExit(8)\n"
        " n=r.getnframes()\n"
        "frames=(n*48000+22050)//44100\n"
        "offset=1 if " + repr(nonrepeatable) +
        " and '-second.wav' in output.name else 0\n"
        "with wave.open(str(output),'wb') as w:\n"
        " w.setnchannels(1); w.setsampwidth(3); w.setframerate(48000)\n"
        " w.writeframes(b''.join(int((i%201)-100+offset).to_bytes(3,'little',signed=True) for i in range(frames)))\n",
        encoding="utf-8")
    path.chmod(0o700)


def fake_analyzer(path):
    path.write_text(
        "#!/usr/bin/python3 -I\n"
        "import json, pathlib, sys, wave\n"
        "if sys.argv[1:] == ['--version']:\n"
        " print('hlolli-wg-analyzer 1.1.0-test'); raise SystemExit(0)\n"
        "a=sys.argv[1:]\n"
        "if a[:4] != ['--json','--max-bytes','67108864','inspect'] or len(a)!=5: raise SystemExit(9)\n"
        "source=pathlib.Path(a[4])\n"
        "with wave.open(str(source),'rb') as r:\n"
        " f={'container':'riff','encoding':'pcm','channels':r.getnchannels(),'sample_rate_hz':r.getframerate(),'bits_per_sample':r.getsampwidth()*8,'valid_bits_per_sample':r.getsampwidth()*8,'block_align':r.getnchannels()*r.getsampwidth(),'frames':r.getnframes()}\n"
        "print(json.dumps({'schema_version':2,'command':'inspect','file':{'path':str(source),'format':f}}))\n",
        encoding="utf-8")
    path.chmod(0o700)


class DoubleBassManifestTests(unittest.TestCase):
    def test_checked_fit_and_reference_contract_validate_without_local_paths(self):
        completed = subprocess.run(
            [str(PYTHON), "-I", str(BUILDER), "validate",
             "--fit-manifest", str(FIT),
             "--reference-contract", str(CONTRACT)],
            check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, env={})
        self.assertEqual(completed.returncode, 0, completed.stderr)
        summary = json.loads(completed.stdout)
        self.assertEqual(summary, {
            "adapter_id": "hlolli_wg_double_bass-passive-open-v1",
            "check_cases": [row[3] for row in ROWS],
            "fit_cases": [row[1] for row in ROWS],
            "heldout_sources": [row[2] for row in ROWS],
            "schema": "hwa-double-bass-manifest-contract",
            "schema_version": 1,
        })

        for path in (FIT, CONTRACT):
            value = json.loads(path.read_text(encoding="utf-8"))
            self.assertFalse(any(text.startswith("/") for text in strings(value)))
            self.assertNotIn("force", json.dumps(value).lower())

        changed = json.loads(CONTRACT.read_text(encoding="utf-8"))
        changed["heldout_sources"][0]["dynamic"] = "ff"
        with tempfile.TemporaryDirectory() as text:
            path = Path(text) / "changed.json"
            path.write_text(json.dumps(changed), encoding="utf-8")
            rejected = subprocess.run(
                [str(PYTHON), "-I", str(BUILDER), "validate",
                 "--fit-manifest", str(FIT),
                 "--reference-contract", str(path)],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={})
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("reference contract changed", rejected.stderr)

    def test_build_makes_a_repeatable_explicit_derived_binding_bundle(self):
        with tempfile.TemporaryDirectory(prefix="hwa bass manifest ") as text:
            root = Path(text)
            fit_references = {}
            heldout_sources = {}
            for index, (_, fit_id, source_id, _) in enumerate(ROWS):
                fit_references[fit_id] = root / (fit_id + ".wav")
                heldout_sources[source_id] = root / (source_id + ".wav")
                wave_file(fit_references[fit_id], 48000, 3,
                          12000 + index * 2)
                wave_file(heldout_sources[source_id], 44100, 2,
                          11025 + index * 147)
            ffmpeg = root / "ffmpeg tool"
            fake_ffmpeg(ffmpeg)
            analyzer = root / "analyzer tool"
            fake_analyzer(analyzer)
            contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
            for row in contract["fit_references"]:
                path = fit_references[row["id"]]
                with wave.open(str(path), "rb") as stream:
                    row["frames"] = stream.getnframes()
                row["sha256"] = sha256(path)
            for row in contract["heldout_sources"]:
                path = heldout_sources[row["id"]]
                with wave.open(str(path), "rb") as stream:
                    row["frames"] = stream.getnframes()
                row["sha256"] = sha256(path)
            contract_path = root / "contract.json"
            contract_path.write_text(json.dumps(contract), encoding="utf-8")

            def command_for(bundle):
                command = [
                    str(PYTHON), "-I", str(BUILDER), "build",
                    "--fit-manifest", str(FIT),
                    "--reference-contract", str(contract_path),
                    "--analyzer", str(analyzer),
                    "--ffmpeg", str(ffmpeg), "--output-dir", str(bundle),
                ]
                for _, fit_id, source_id, _ in ROWS:
                    command.extend(("--fit-reference",
                                    fit_id + "=" + str(fit_references[fit_id])))
                    command.extend(("--heldout-source",
                                    source_id + "=" +
                                    str(heldout_sources[source_id])))
                return command

            rejected_contract = subprocess.run(
                command_for(root / "unchecked bundle"), check=False,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, env={})
            self.assertNotEqual(rejected_contract.returncode, 0)
            self.assertIn("reference contract changed",
                          rejected_contract.stderr)

            def arguments_for(bundle):
                return types.SimpleNamespace(
                    fit_manifest=FIT,
                    reference_contract=contract_path,
                    fit_reference=[
                        fit_id + "=" + str(fit_references[fit_id])
                        for _, fit_id, _, _ in ROWS
                    ],
                    heldout_source=[
                        source_id + "=" + str(heldout_sources[source_id])
                        for _, _, source_id, _ in ROWS
                    ],
                    analyzer=analyzer,
                    ffmpeg=ffmpeg,
                    output_dir=bundle,
                )

            bundles = [root / "bundle one", root / "bundle two"]
            for bundle in bundles:
                with mock.patch.object(
                        MODULE, "expected_contract", return_value=contract):
                    summary = MODULE.build_bundle(arguments_for(bundle))
                self.assertEqual(summary, {
                    "derived_bindings": [
                        {"id": derived_id,
                         "sha256": sha256(bundle / (derived_id + ".wav"))}
                        for _, _, _, derived_id in ROWS
                    ],
                    "output": str(bundle.absolute()),
                })
                self.assertEqual(stat.S_IMODE(bundle.stat().st_mode), 0o700)
                self.assertEqual(sorted(path.name for path in bundle.iterdir()), [
                    "bindings.local.json", "experiment.json", "fit.json",
                    *sorted(row[3] + ".wav" for row in ROWS),
                    "receipt.json",
                ])

            stable = ("experiment.json", "fit.json", "receipt.json",
                      *(row[3] + ".wav" for row in ROWS))
            for name in stable:
                self.assertEqual((bundles[0] / name).read_bytes(),
                                 (bundles[1] / name).read_bytes())

            experiment = json.loads(
                (bundles[0] / "experiment.json").read_text(encoding="utf-8"))
            self.assertEqual(experiment["clock_rate_hz"], 48000)
            self.assertEqual(
                [row["id"] for row in experiment["inputs"]],
                [row[3] for row in ROWS] + [row[1] for row in ROWS])
            self.assertEqual(
                [row["id"] for row in experiment["cases"]],
                [row[3] for row in ROWS] + [row[1] for row in ROWS])
            self.assertEqual(
                [row["id"] for row in experiment["parameters"]],
                ["string_a_loss_seconds", "string_d_loss_seconds",
                 "string_e_loss_seconds", "string_g_loss_seconds"])
            self.assertEqual(experiment["plan"], {
                "kind": "random", "replicates": 1,
                "sample_count": 32, "seed": 1701,
            })
            self.assertTrue(all(not row["levels"]
                                for row in experiment["parameters"]))
            fit_value = json.loads(
                (bundles[0] / "fit.json").read_text(encoding="utf-8"))
            expected_cases = [
                (row[3], "check") for row in ROWS
            ] + [
                (row[1], "fit") for row in ROWS
            ]
            self.assertEqual(
                [(row["case"], row["split"])
                 for row in fit_value["objectives"]],
                expected_cases)
            self.assertTrue(all(
                row["reference_binding"] == row["case"]
                for row in fit_value["objectives"]))

            for _, _, source_id, derived_id in ROWS:
                derived = bundles[0] / (derived_id + ".wav")
                with wave.open(str(heldout_sources[source_id]), "rb") as stream:
                    expected_frames = (
                        stream.getnframes() * 48000 + 22050) // 44100
                with wave.open(str(derived), "rb") as stream:
                    self.assertEqual(
                        (stream.getframerate(), stream.getnchannels(),
                         stream.getsampwidth(), stream.getnframes()),
                        (48000, 1, 3, expected_frames))
            receipt = json.loads(
                (bundles[0] / "receipt.json").read_text(encoding="utf-8"))
            self.assertEqual(receipt["builder_sha256"], sha256(BUILDER))
            self.assertEqual(receipt["analyzer"], {
                "path": str(analyzer.resolve()),
                "sha256": sha256(analyzer),
                "version": "hlolli-wg-analyzer 1.1.0-test",
            })
            self.assertEqual(receipt["transform"]["tool"], {
                "path": str(ffmpeg.resolve()),
                "sha256": sha256(ffmpeg),
                "version": "ffmpeg version 8.1.2-test",
            })
            self.assertTrue(all(row["sample_rate_hz"] == 44100
                                for row in receipt["heldout_sources"]))
            self.assertTrue(all(row["sample_rate_hz"] == 48000
                                for row in receipt["derived_bindings"]))
            self.assertEqual(
                [row["source_id"] for row in receipt["derived_bindings"]],
                [row[2] for row in ROWS])
            bindings = json.loads(
                (bundles[0] / "bindings.local.json").read_text(encoding="utf-8"))
            self.assertEqual(
                [row["id"] for row in bindings["bindings"]],
                [row[3] for row in ROWS] + [row[1] for row in ROWS])
            for index, (_, _, _, derived_id) in enumerate(ROWS):
                self.assertEqual(
                    Path(bindings["bindings"][index]["path"]),
                    bundles[0] / (derived_id + ".wav"))

            fake_ffmpeg(ffmpeg, nonrepeatable=True)
            rejected_output = root / "rejected bundle"
            with mock.patch.object(
                    MODULE, "expected_contract", return_value=contract):
                with self.assertRaisesRegex(
                        MODULE.ManifestError, "is not byte-repeatable"):
                    MODULE.build_bundle(arguments_for(rejected_output))
            self.assertFalse(rejected_output.exists())


if __name__ == "__main__":
    unittest.main()

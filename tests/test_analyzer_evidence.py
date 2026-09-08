#!/usr/bin/env python3
"""Tests for checked analyzer evidence shared by fit tools."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import wave


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "analyzer_evidence.py"
ANALYZER = None


def load_module():
    specification = importlib.util.spec_from_file_location(
        "hwa_analyzer_evidence_test", MODULE_PATH)
    if specification is None or specification.loader is None:
        raise RuntimeError("cannot load analyzer evidence module")
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_real_harmonic_decay(path: Path) -> None:
    rate = 16000
    lead = 0.10
    duration = 4.0
    fundamental = 110.0
    taus = (0.8, 0.6, 0.4, 0.3)
    maximum = 32767
    raw = bytearray()
    for index in range(round((lead + duration) * rate)):
        time = index / rate
        value = 0.0
        if time >= lead:
            age = time - lead
            for harmonic, tau in enumerate(taus, 1):
                value += (
                    0.18 / harmonic * math.exp(-age / tau) *
                    math.sin(
                        2.0 * math.pi * fundamental * harmonic * age)
                )
        sample = max(-maximum, min(maximum, round(value * maximum)))
        encoded = int(sample).to_bytes(2, "little", signed=True)
        raw.extend(encoded)
        raw.extend(encoded)
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(2)
        stream.setsampwidth(2)
        stream.setframerate(rate)
        stream.writeframes(raw)


def write_pitch_analyzer(path: Path, mutation: str = "") -> None:
    path.write_text(
        "#!{} -I\n".format(Path(sys.executable).resolve()) +
        "import json, pathlib, sys\n"
        "a=sys.argv[1:]\n"
        "if len(a)!=7 or a[:2]!=['--json','isolated-note'] or a[3]!='--expected-hz' or a[5:]!=['--metrics','pitch']: raise SystemExit(9)\n"
        "p=str(pathlib.Path(a[2]).absolute()); hz=float(a[4])\n"
        "report={'schema':'hwa-isolated-note','schema_version':1,'command':'isolated-note','method':'isolated-note-1','path':p,'expected_hz':hz,'requested_mask':1,'valid_mask':1,'rejection_mask':0,'requested_metrics':['pitch'],'valid_metrics':['pitch'],'rejections':[],'pitch':{'valid':True,'hz':hz,'cents':0.0,'confidence':0.75,'coverage':0.875}}\n" +
        mutation + "\n"
        "print(json.dumps(report))\n",
        encoding="utf-8",
    )
    path.chmod(0o700)


def write_body_analyzer(
        path: Path, mutation: str = "", supported: int = 8) -> None:
    source = '''import json, math, pathlib, sys
a = sys.argv[1:]
if len(a) not in (3, 4) or a[:2] != ['--json', 'body-envelope']:
    raise SystemExit(9)
count = SUPPORTED
def profile(path, model=False):
    points = []
    for i in range(160):
        valid = i < count
        points.append({
            'frequency_hz': 120.0 * 2.0**(i / 24.0),
            'relative_db': (2.0*i + 5.0 if model else float(i)) if valid else 0.0,
            'residual_spread_db': 0.0,
            'observation_count': 20 if valid else 0,
            'pitch_cell_count': 4 if valid else 0,
            'harmonic_count': 4 if valid else 0,
            'quality_flags': 0 if valid else 7,
            'confidence': 1.0 if valid else 0.0, 'valid': valid,
        })
    return {
        'path': path,
        'status': 'valid' if count >= 8 else 'low-support' if count else 'no-support',
        'confidence': 1.0 if count else 0.0,
        'frames_seen': 20, 'frames_used': 20 if count else 0,
        'frames_rejected_pitch': 0 if count else 20,
        'pitch_min_hz': 110.0 if count else 0.0,
        'pitch_max_hz': 220.0 if count else 0.0,
        'observation_count': 20*count, 'points': points,
    }
report = {
    'schema_version': 1, 'command': 'body-envelope',
    'method': 'crossed-harmonic-response-1',
    'shape_constraints': ['zero-mean', 'zero-log-frequency-slope'],
    'reference': profile(a[2]), 'fit_evaluations': 100,
    'retained_work_bytes': 1000,
}
if len(a) == 4:
    report['model'] = profile(a[3], model=True)
    valid = count >= 3
    deltas = [i - (count - 1)/2.0 for i in range(count)] if valid else []
    report['comparison'] = {
        'valid': valid,
        'shape_rmse_db': math.sqrt(sum(d*d for d in deltas)/count) if valid else 0.0,
        'shape_correlation': 1.0 if valid else 0.0,
        'confidence': 1.0 if valid else 0.0,
        'gaps': [{
            'frequency_hz': point['frequency_hz'],
            'model_minus_reference_db': deltas[i] if valid and i < count else 0.0,
            'confidence': 1.0 if valid and i < count else 0.0,
            'valid': valid and i < count,
        } for i, point in enumerate(report['reference']['points'])],
    }
MUTATION
print(json.dumps(report))
'''
    path.write_text(
        "#!{} -I\n".format(Path(sys.executable).resolve()) +
        source.replace("SUPPORTED", str(supported)).replace("MUTATION", mutation),
        encoding="utf-8")
    path.chmod(0o700)


def write_body_phrase(path: Path, silent: bool = False) -> None:
    rate = 16000
    frequencies = (220.0, 293.664767917408, 329.627556912870, 440.0)
    raw = bytearray()
    for frame in range(4 * rate):
        frequency = frequencies[frame // rate]
        time = frame / rate
        value = 0.0 if silent else sum(
            math.sin(math.tau * harmonic * frequency * time) / harmonic
            for harmonic in range(1, 13))
        raw.extend(round(2600.0 * value).to_bytes(2, "little", signed=True))
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(1)
        stream.setsampwidth(2)
        stream.setframerate(rate)
        stream.writeframes(raw)


def write_harmonic_analyzer(
        path: Path, valid_band_count: int = 3,
        profile_mutation: str = "") -> None:
    path.write_text(
        "#!{} -I\n".format(Path(sys.executable).resolve()) +
        "import json, pathlib, sys\n"
        "a=sys.argv[1:]\n"
        "if len(a)!=5 or a[:2]!=['--json','harmonic-decay'] or a[3]!='--expected-hz': raise SystemExit(9)\n"
        "p=str(pathlib.Path(a[2]).absolute()); hz=float(a[4]); count={}\n".format(
            valid_band_count) +
        "bands=[{'harmonic_number':i+1,'target_hz':hz*(i+1),'valid':i<count,'t60_seconds':1.0 if i<count else None,'rejection_mask':0 if i<count else 4,'rejections':[] if i<count else ['low-anchor-snr']} for i in range(6)]\n"
        "profile={'path':p,'valid':count>=4,'band_count':len(bands),'valid_band_count':count,'rejection_mask':0 if count>=4 else 512,'rejections':[] if count>=4 else ['low-harmonic-coverage'],'bands':bands}\n" +
        profile_mutation + "\n"
        "print(json.dumps({'schema':'hwa-harmonic-decay','schema_version':1,'command':'harmonic-decay','method':'harmonic-decay-1','expected_hz':hz,'reference':profile,'model':None,'comparison':None}))\n",
        encoding="utf-8",
    )
    path.chmod(0o700)


def write_harmonic_comparison_analyzer(
        path: Path, reported_rmse: float = 1.5811388300841898,
        forge_profile: bool = False) -> None:
    path.write_text(
        "#!{} -I\n".format(Path(sys.executable).resolve()) +
        "import json, pathlib, sys\n"
        "a=sys.argv[1:]\n"
        "if len(a)!=6 or a[:2]!=['--json','harmonic-decay'] or a[4]!='--expected-hz': raise SystemExit(9)\n"
        "r=str(pathlib.Path(a[2]).absolute()); m=str(pathlib.Path(a[3]).absolute()); hz=float(a[5])\n"
        "errors=(-2.0,-1.0,1.0,2.0)\n"
        "reference_t60=[1.0 for _ in errors]\n"
        "model_t60=[1.0 if %r else 10.0**(v/20.0) for v in errors]\n" % forge_profile +
        "profile=lambda p,values:{'path':p,'valid':True,'band_count':4,'valid_band_count':4,'rejection_mask':0,'rejections':[],'bands':[{'harmonic_number':i+1,'target_hz':hz*(i+1),'valid':True,'t60_seconds':v,'rejection_mask':0,'rejections':[]} for i,v in enumerate(values)]}\n"
        "bands=[{'harmonic_number':i+1,'reference_valid':True,'model_valid':True,'valid':True,'t60_log_error_db':v} for i,v in enumerate(errors)]\n"
        "comparison={'valid':True,'band_count':4,'shared_valid_band_count':4,'shared_reference_coverage':1.0,'t60_log_rmse_db':%r,'median_t60_log_bias_db':0.0,'bands':bands}\n" % reported_rmse +
        "print(json.dumps({'schema':'hwa-harmonic-decay','schema_version':1,'command':'harmonic-decay','method':'harmonic-decay-1','expected_hz':hz,'reference':profile(r,reference_t60),'model':profile(m,model_t60),'comparison':comparison}))\n",
        encoding="utf-8",
    )
    path.chmod(0o700)


class AnalyzerEvidenceTests(unittest.TestCase):
    def test_body_recording_command_preserves_receipt_and_rejects_bad_evidence(self):
        tool = ROOT / "tools" / "instrument_fit.py"
        for mutation, expected_status in (
                ("", 0), ("report['reference']['confidence']=0.1", 1)):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as text:
                root = Path(text)
                analyzer, source = root / "analyzer.py", root / "phrase.wav"
                output = root / "recordings.json"
                write_body_analyzer(analyzer, mutation)
                write_body_phrase(source)
                completed = subprocess.run([
                    sys.executable, "-I", str(tool), "check-recordings",
                    "--analyzer", str(analyzer), "--recording", str(source),
                    "--excerpt-seconds", "5", "--output", str(output),
                ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                self.assertEqual(completed.returncode, expected_status,
                                 completed.stderr)
                if expected_status:
                    self.assertFalse(output.exists())
                    self.assertIn("confidence", completed.stderr)
                else:
                    receipt = json.loads(output.read_text(encoding="utf-8"))
                    self.assertEqual(set(receipt), {
                        "schema", "schema_version", "analyzer_sha256", "recordings",
                    })
                    self.assertEqual(receipt["schema"], "hwa-body-envelope-recording-check")
                    self.assertEqual(receipt["schema_version"], 1)
                    self.assertEqual(receipt["analyzer_sha256"], sha256(analyzer))
                    self.assertEqual(receipt["recordings"], [{
                        "name": source.name, "source_sha256": sha256(source),
                        "excerpt_sha256": sha256(source), "excerpt_start_frame": 0,
                        "excerpt_frames": 64000, "confidence": 1.0,
                        "frames_seen": 20, "frames_used": 20, "valid_points": 8,
                    }])

    def test_body_recording_command_rejects_source_changed_during_analysis(self):
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            analyzer, source = root / "analyzer.py", root / "phrase.wav"
            output = root / "recordings.json"
            write_body_phrase(source)
            write_body_analyzer(analyzer,
                "pathlib.Path({!r}).write_bytes(b'changed')".format(str(source)))
            completed = subprocess.run([
                sys.executable, "-I", str(ROOT / "tools" / "instrument_fit.py"),
                "check-recordings", "--analyzer", str(analyzer),
                "--recording", str(source), "--output", str(output),
            ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertEqual(completed.returncode, 1, completed.stderr)
            self.assertIn("recording-0 changed", completed.stderr)
            self.assertFalse(output.exists())

    def test_body_fit_caller_keeps_result_fields_and_translates_evidence_errors(self):
        specification = importlib.util.spec_from_file_location(
            "hwa_body_fit_test", ROOT / "tools" / "instrument_fit.py")
        fit = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(fit)
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            analyzer, reference = root / "analyzer.py", root / "reference.wav"
            reference.write_bytes(b"reference")
            for mutation, supported in (("", 8), ("", 2), (
                    "report['comparison']['shape_rmse_db']=0.0", 8)):
                write_body_analyzer(analyzer, mutation, supported)
                arguments = {
                    "analyzer_sha256": sha256(analyzer),
                    "reference_sha256": sha256(reference),
                    "model_sha256": sha256(reference),
                }
                if mutation or supported < 3:
                    with self.assertRaises(fit.FitError):
                        fit.run_body_envelope(analyzer, reference, reference, **arguments)
                else:
                    result = fit.run_body_envelope(
                        analyzer, reference, reference, **arguments)
                    self.assertEqual(result, {
                        "shape_rmse_db": math.sqrt(5.25),
                        "shape_correlation": 1.0, "confidence": 1.0,
                    })
                    arguments["reference_sha256"] = "0" * 64
                    with self.assertRaisesRegex(fit.FitError, "hash changed"):
                        fit.run_body_envelope(analyzer, reference, reference, **arguments)

    def test_body_envelope_returns_checked_profile_and_comparison(self):
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa body evidence ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            reference = root / "reference.wav"
            model = root / "model.wav"
            reference.write_bytes(b"reference")
            model.write_bytes(b"model")
            write_body_analyzer(analyzer)
            checks = module.AnalyzerEvidence(analyzer, {
                "reference": (reference, sha256(reference)),
                "model": (model, sha256(model)),
            })
            single = checks.body_envelope("reference")
            self.assertEqual(single.reference.status, "valid")
            self.assertEqual(single.reference.valid_point_count, 8)
            self.assertEqual(single.reference.frames_used, 20)
            self.assertIsNone(single.comparison_valid)
            self.assertNotIn("model", single.report)
            pair = checks.body_envelope("reference", "model")
            self.assertTrue(pair.comparison_valid)
            self.assertAlmostEqual(pair.shape_rmse_db, math.sqrt(5.25))
            self.assertAlmostEqual(pair.shape_correlation, 1.0)
            self.assertEqual(pair.confidence, 1.0)

    def test_body_envelope_rejects_forged_reports(self):
        changes = {
            "method": "report['method']='changed'",
            "boolean version": "report['schema_version']=True",
            "path": "report['reference']['path']='wrong.wav'",
            "constraints": "report['shape_constraints']=[]",
            "profile support": "report['reference']['frames_used']=21",
            "point support": "report['reference']['points'][0]['observation_count']=0",
            "point confidence": "report['reference']['points'][0]['confidence']=0.5",
            "profile status": "report['reference']['status']='no-support'",
            "grid": "report['model']['points'][0]['frequency_hz']=123.0",
            "comparison validity": "report['comparison']['valid']=False",
            "rmse": "report['comparison']['shape_rmse_db']=0.0",
            "correlation": "report['comparison']['shape_correlation']=0.0",
            "confidence": "report['comparison']['confidence']=0.1",
            "gap": "report['comparison']['gaps'][0]['model_minus_reference_db']=0.0",
            "negative rmse": "report['comparison']['shape_rmse_db']=-1.0",
            "NaN": "report['comparison']['shape_rmse_db']=float('nan')",
            "missing model": "del report['model']",
        }
        module = load_module()
        for name, mutation in changes.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as text:
                root = Path(text)
                analyzer = root / "analyzer.py"
                reference = root / "reference.wav"
                reference.write_bytes(b"reference")
                write_body_analyzer(analyzer, mutation)
                checks = module.AnalyzerEvidence(analyzer, {
                    "reference": (reference, sha256(reference)),
                })
                with self.assertRaises(module.EvidenceError):
                    checks.body_envelope("reference", "reference")

    def test_body_envelope_uses_confidence_weighted_offset(self):
        module = load_module()
        mutation = '''for name in ('reference', 'model'):
    report[name]['points'][0]['residual_spread_db'] = 12.0 * math.log(2.0)
    report[name]['points'][0]['confidence'] = 0.5
    report[name]['confidence'] = 7.5 / 8.0
for i in range(8):
    report['comparison']['gaps'][i]['model_minus_reference_db'] -= 7.0 / 30.0
report['comparison']['gaps'][0]['confidence'] = 0.5
report['comparison']['confidence'] = 7.5 / 8.0
report['comparison']['shape_rmse_db'] = math.sqrt(5.25 + (7.0 / 30.0)**2)
'''
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            analyzer, source = root / "analyzer.py", root / "source.wav"
            write_body_analyzer(analyzer, mutation)
            source.write_bytes(b"source")
            checks = module.AnalyzerEvidence(
                analyzer, {"source": (source, sha256(source))})
            pair = checks.body_envelope("source", "source")
            self.assertAlmostEqual(pair.shape_rmse_db,
                                   math.sqrt(5.25 + (7.0 / 30.0)**2))
            self.assertEqual(pair.confidence, 0.9375)

    def test_body_envelope_high_residual_is_a_quality_flag(self):
        module = load_module()
        with tempfile.TemporaryDirectory() as text:
            root = Path(text)
            analyzer, source = root / "analyzer.py", root / "source.wav"
            write_body_analyzer(analyzer, "\n".join((
                "point = report['reference']['points'][0]",
                "point['residual_spread_db'] = 24.0",
                "point['confidence'] = math.exp(-2.0)",
                "point['quality_flags'] = 8",
                "report['reference']['confidence'] = (7.0 + point['confidence']) / 8.0",
            )))
            source.write_bytes(b"source")
            checks = module.AnalyzerEvidence(
                analyzer, {"source": (source, sha256(source))})
            profile = checks.body_envelope("source").reference
            self.assertEqual(profile.status, "valid")
            self.assertEqual(profile.valid_point_count, 8)

    def test_body_envelope_preserves_low_and_no_support_results(self):
        module = load_module()
        for count, status, comparison_valid in (
                (0, "no-support", False), (2, "low-support", False),
                (3, "low-support", True)):
            with self.subTest(count=count), tempfile.TemporaryDirectory() as text:
                root = Path(text)
                analyzer = root / "analyzer.py"
                source = root / "note.wav"
                source.write_bytes(b"source")
                write_body_analyzer(analyzer, supported=count)
                checks = module.AnalyzerEvidence(
                    analyzer, {"note": (source, sha256(source))})
                single = checks.body_envelope("note")
                pair = checks.body_envelope("note", "note")
                self.assertEqual(single.reference.status, status)
                self.assertEqual(pair.comparison_valid, comparison_valid)

    def test_real_body_envelope_and_silence_match_shared_contract(self):
        if ANALYZER is None:
            self.skipTest("analyzer executable was not supplied")
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa real body ") as text:
            root = Path(text)
            phrase, silence = root / "phrase.wav", root / "silence.wav"
            write_body_phrase(phrase)
            write_body_phrase(silence, silent=True)
            checks = module.AnalyzerEvidence(ANALYZER, {
                "phrase": (phrase, sha256(phrase)),
                "silence": (silence, sha256(silence)),
            })
            pair = checks.body_envelope("phrase", "phrase")
            self.assertEqual(pair.reference.status, "valid")
            self.assertTrue(pair.comparison_valid)
            self.assertAlmostEqual(pair.shape_rmse_db, 0.0)
            quiet = checks.body_envelope("silence")
            self.assertEqual(quiet.reference.status, "no-support")
            mismatch = checks.body_envelope("phrase", "silence")
            self.assertFalse(mismatch.comparison_valid)

    def test_checked_run_uses_supplied_environment_and_scratch(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(
                prefix="hwa analyzer environment ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_pitch_analyzer(
                analyzer,
                "if __import__('os').environ.get('HWA_SENTINEL') != "
                "'fixed': raise SystemExit(8)",
            )
            source.write_bytes(b"source recording")
            directories = []
            temporary_file = module.tempfile.TemporaryFile

            def record_temporary_file(*args, **kwargs):
                directories.append(kwargs.get("dir"))
                return temporary_file(*args, **kwargs)

            checks = module.AnalyzerEvidence(
                analyzer, {"note": (source, sha256(source))},
                environment={"HWA_SENTINEL": "fixed"}, scratch=root)
            with mock.patch.object(
                    module.tempfile, "TemporaryFile",
                    side_effect=record_temporary_file):
                evidence = checks.isolated_note("note", 440.0)

            self.assertTrue(evidence.pitch.valid)
            self.assertEqual(directories, [root, root])

    def test_real_analyzer_harmonic_profile_matches_the_shared_contract(
            self) -> None:
        if ANALYZER is None:
            self.skipTest("analyzer executable was not supplied")
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa real evidence ") as text:
            root = Path(text)
            source = root / "harmonic.wav"
            write_real_harmonic_decay(source)
            digest = sha256(source)
            checks = module.AnalyzerEvidence(ANALYZER, {
                "reference": (source, digest),
                "model": (source, digest),
            })

            evidence = checks.harmonic_decay("reference", 110.0)
            comparison = checks.harmonic_decay(
                "reference", 110.0, model_id="model")

            self.assertTrue(evidence.reference_valid)
            self.assertGreaterEqual(evidence.reference_valid_band_count, 4)
            self.assertTrue(comparison.comparison_valid)
            self.assertGreaterEqual(comparison.shared_valid_band_count, 4)

    def test_checked_pitch_returns_raw_and_normalized_evidence(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa analyzer evidence ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_pitch_analyzer(analyzer)
            source.write_bytes(b"source recording")

            checks = module.AnalyzerEvidence(
                analyzer, {"note": (source, sha256(source))})
            evidence = checks.isolated_note("note", 440.0)

            self.assertEqual(checks.analyzer_sha256, sha256(analyzer))
            with self.assertRaises(AttributeError):
                checks.analyzer_sha256 = "0" * 64
            self.assertEqual(evidence.report["path"], str(source.absolute()))
            self.assertTrue(evidence.pitch.valid)
            self.assertEqual(evidence.pitch.hz, 440.0)
            self.assertEqual(evidence.pitch.cents, 0.0)
            self.assertEqual(evidence.pitch.confidence, 0.75)
            self.assertEqual(evidence.pitch.coverage, 0.875)

    def test_checked_pitch_rejects_contract_changes(self) -> None:
        module = load_module()
        changes = {
            "missing mask": "del report['requested_mask']",
            "missing rejection mask": "del report['rejection_mask']",
            "wrong mask": "report['valid_mask']=0",
            "decay-only rejection": (
                "report['rejection_mask']=128; "
                "report['rejections']=['late-pulse']"
            ),
            "wrong metric": "report['requested_metrics']=['decay']",
            "wrong method": "report['method']='isolated-note-2'",
            "wrong path": "report['path']=p+'.changed'",
            "non-finite pitch": "report['pitch']['hz']=float('nan')",
            "nonpositive pitch": "report['pitch']['hz']=0.0",
            "inconsistent cents": "report['pitch']['cents']=1.0",
            "bad confidence": "report['pitch']['confidence']=1.25",
            "bad coverage": "report['pitch']['coverage']=-0.25",
            "false valid flag": (
                "report['pitch']['valid']=False; report['valid_mask']=0; "
                "report['valid_metrics']=[]"
            ),
        }
        for name, mutation in changes.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory(
                    prefix="hwa bad pitch contract ") as text:
                root = Path(text)
                analyzer = root / "analyzer.py"
                source = root / "note.wav"
                write_pitch_analyzer(analyzer, mutation)
                source.write_bytes(b"source recording")
                checks = module.AnalyzerEvidence(
                    analyzer, {"note": (source, sha256(source))})
                with self.assertRaises(module.EvidenceError):
                    checks.isolated_note("note", 440.0)

    def test_checked_pitch_returns_a_coherent_invalid_measurement(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa invalid pitch ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_pitch_analyzer(
                analyzer,
                "report['pitch']={'valid':False,'hz':0.0,'cents':0.0,"
                "'confidence':0.0,'coverage':0.0}; "
                "report['valid_mask']=0; report['valid_metrics']=[]; "
                "report['rejection_mask']=16; "
                "report['rejections']=['low-support']",
            )
            source.write_bytes(b"source recording")
            checks = module.AnalyzerEvidence(
                analyzer, {"note": (source, sha256(source))})

            evidence = checks.isolated_note("note", 440.0)

            self.assertFalse(evidence.pitch.valid)
            self.assertEqual(evidence.pitch.hz, 0.0)

    def test_checked_pitch_rejects_duplicate_json_keys(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa duplicate JSON ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_pitch_analyzer(
                analyzer,
                "print('{\"pitch\":{\"valid\":true,\"valid\":false}}'); "
                "raise SystemExit(0)",
            )
            source.write_bytes(b"source recording")
            checks = module.AnalyzerEvidence(
                analyzer, {"note": (source, sha256(source))})
            with self.assertRaisesRegex(
                    module.EvidenceError, "duplicate JSON key: valid"):
                checks.isolated_note("note", 440.0)

    def test_checked_pitch_rejects_overflow_in_an_extra_json_field(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa JSON overflow ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_pitch_analyzer(
                analyzer,
                "print(json.dumps(report)[:-1]+',\"extra\":1e9999}'); "
                "raise SystemExit(0)",
            )
            source.write_bytes(b"source recording")
            checks = module.AnalyzerEvidence(
                analyzer, {"note": (source, sha256(source))})
            with self.assertRaisesRegex(
                    module.EvidenceError, "non-finite JSON number"):
                checks.isolated_note("note", 440.0)

    def test_analyzer_failure_uses_a_bounded_error_tail(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa analyzer failure ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_pitch_analyzer(
                analyzer,
                "print('named analyzer failure', file=sys.stderr); "
                "raise SystemExit(7)",
            )
            source.write_bytes(b"source recording")
            checks = module.AnalyzerEvidence(
                analyzer, {"note": (source, sha256(source))})
            with self.assertRaisesRegex(
                    module.EvidenceError, "failed: named analyzer failure"):
                checks.isolated_note("note", 440.0)

    def test_analyzer_report_has_a_byte_limit(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa analyzer output ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_pitch_analyzer(
                analyzer,
                "print('x'*(1024*1024+1)); raise SystemExit(0)",
            )
            source.write_bytes(b"source recording")
            checks = module.AnalyzerEvidence(
                analyzer, {"note": (source, sha256(source))})
            with self.assertRaisesRegex(
                    module.EvidenceError, "output exceeds the byte limit"):
                checks.isolated_note("note", 440.0)

    def test_analyzer_run_has_a_deadline(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa analyzer timeout ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_pitch_analyzer(
                analyzer, "import time; time.sleep(1.0)")
            source.write_bytes(b"source recording")
            checks = module.AnalyzerEvidence(
                analyzer, {"note": (source, sha256(source))})
            old_timeout = module.RUN_TIMEOUT_SECONDS
            module.RUN_TIMEOUT_SECONDS = 0.05
            try:
                with self.assertRaisesRegex(module.EvidenceError, "timed out"):
                    checks.isolated_note("note", 440.0)
            finally:
                module.RUN_TIMEOUT_SECONDS = old_timeout

    def test_constructor_rejects_a_changed_source_hash(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa changed source ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_pitch_analyzer(analyzer)
            source.write_bytes(b"source recording")
            with self.assertRaisesRegex(module.EvidenceError, "hash changed"):
                module.AnalyzerEvidence(
                    analyzer, {"note": (source, "0" * 64)})

    def test_run_rejects_a_source_changed_by_the_analyzer(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa source mutation ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_pitch_analyzer(
                analyzer, "pathlib.Path(p).write_bytes(b'changed')")
            source.write_bytes(b"source recording")
            checks = module.AnalyzerEvidence(
                analyzer, {"note": (source, sha256(source))})

            with self.assertRaisesRegex(
                    module.EvidenceError, "changed during"):
                checks.isolated_note("note", 440.0)

    def test_three_band_harmonic_profile_is_checked_but_not_valid(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa harmonic evidence ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_harmonic_analyzer(analyzer, valid_band_count=3)
            source.write_bytes(b"source recording")

            checks = module.AnalyzerEvidence(
                analyzer, {"reference": (source, sha256(source))})
            evidence = checks.harmonic_decay("reference", 440.0)

            self.assertFalse(evidence.reference_valid)
            self.assertEqual(evidence.reference_valid_band_count, 3)
            self.assertIsNone(evidence.model_valid)
            self.assertIsNone(evidence.comparison_valid)
            self.assertEqual(evidence.report["method"], "harmonic-decay-1")

    def test_harmonic_profile_rejects_missing_rejection_data(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa harmonic rejection ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            source = root / "note.wav"
            write_harmonic_analyzer(
                analyzer, profile_mutation="del profile['rejection_mask']")
            source.write_bytes(b"source recording")
            checks = module.AnalyzerEvidence(
                analyzer, {"reference": (source, sha256(source))})

            with self.assertRaises(module.EvidenceError):
                checks.harmonic_decay("reference", 440.0)

    def test_harmonic_comparison_returns_checked_values(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa harmonic comparison ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            reference = root / "reference.wav"
            model = root / "model.wav"
            write_harmonic_comparison_analyzer(analyzer)
            reference.write_bytes(b"reference recording")
            model.write_bytes(b"model recording")

            checks = module.AnalyzerEvidence(analyzer, {
                "reference": (reference, sha256(reference)),
                "model": (model, sha256(model)),
            })
            evidence = checks.harmonic_decay(
                "reference", 440.0, model_id="model")

            self.assertTrue(evidence.reference_valid)
            self.assertTrue(evidence.model_valid)
            self.assertTrue(evidence.comparison_valid)
            self.assertEqual(evidence.shared_valid_band_count, 4)
            self.assertEqual(evidence.shared_reference_coverage, 1.0)
            self.assertEqual(
                evidence.band_t60_log_errors_db, (-2.0, -1.0, 1.0, 2.0))
            self.assertEqual(
                evidence.t60_log_rmse_db, 1.5811388300841898)
            self.assertEqual(evidence.median_t60_log_bias_db, 0.0)

    def test_harmonic_comparison_rejects_a_false_summary(self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa false harmonic sum ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            reference = root / "reference.wav"
            model = root / "model.wav"
            write_harmonic_comparison_analyzer(analyzer, reported_rmse=1.5)
            reference.write_bytes(b"reference recording")
            model.write_bytes(b"model recording")
            checks = module.AnalyzerEvidence(analyzer, {
                "reference": (reference, sha256(reference)),
                "model": (model, sha256(model)),
            })
            with self.assertRaisesRegex(
                    module.EvidenceError, "unknown contract"):
                checks.harmonic_decay(
                    "reference", 440.0, model_id="model")

    def test_harmonic_comparison_rejects_errors_unbound_from_profiles(
            self) -> None:
        module = load_module()
        with tempfile.TemporaryDirectory(prefix="hwa forged harmonic ") as text:
            root = Path(text)
            analyzer = root / "analyzer.py"
            reference = root / "reference.wav"
            model = root / "model.wav"
            write_harmonic_comparison_analyzer(analyzer, forge_profile=True)
            reference.write_bytes(b"reference recording")
            model.write_bytes(b"model recording")
            checks = module.AnalyzerEvidence(analyzer, {
                "reference": (reference, sha256(reference)),
                "model": (model, sha256(model)),
            })
            with self.assertRaisesRegex(
                    module.EvidenceError, "unknown contract"):
                checks.harmonic_decay(
                    "reference", 440.0, model_id="model")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--analyzer", type=Path)
    arguments, unittest_arguments = parser.parse_known_args()
    ANALYZER = arguments.analyzer
    unittest.main(argv=[sys.argv[0], *unittest_arguments])

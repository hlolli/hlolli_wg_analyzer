import io
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile


EXE = str(pathlib.Path(sys.argv.pop(1)).resolve())


def note(step="C", duration=1, extra="", before=""):
    return (f"<note>{before}<pitch><step>{step}</step><octave>4</octave></pitch>"
            f"<duration>{duration}</duration>{extra}</note>")


def measure(body, number=1):
    return f'<measure number="{number}"><attributes><divisions>1</divisions></attributes>{body}</measure>'


def score(*measures):
    return ('<?xml version="1.0"?><score-partwise><part-list><score-part id="P">'
            '<part-name>Part &amp; voice</part-name></score-part></part-list><part id="P">'
            + "".join(measures) + '</part></score-partwise>').encode()


FORWARD = '<barline location="left"><repeat direction="forward"/></barline>'
BACKWARD = '<barline><repeat direction="backward"/></barline>'


class MusicXML(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="hwa-musicxml-")
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)

    def run_score(self, data, *options, ok=True):
        source = self.root / 'score.input'
        source.write_bytes(data)
        result = subprocess.run([EXE, 'import-score', str(source), *options],
                                capture_output=True, text=True, timeout=10)
        if not ok:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertTrue(result.stderr)
            return result
        self.assertEqual(result.returncode, 0, result.stderr)
        parsed = json.loads(result.stdout)
        self.assertEqual(parsed['schema'], 'hwa-musicxml-score')
        self.assertEqual([e['start_beats'] for e in parsed['events']],
                         sorted(e['start_beats'] for e in parsed['events']))
        return parsed

    @staticmethod
    def notes(parsed):
        return [e for e in parsed['events'] if e['kind'] == 'note']

    def archive(self, xml, method=zipfile.ZIP_DEFLATED, descriptor=False):
        class Unseekable(io.BytesIO):
            def seek(self, *args):
                raise OSError('streaming archive')
        target = Unseekable() if descriptor else io.BytesIO()
        with zipfile.ZipFile(target, 'w', compression=method) as z:
            z.writestr('META-INF/container.xml', '<container><rootfiles><rootfile full-path="scores/main.xml" '
                       'media-type="application/vnd.recordare.musicxml+xml"/></rootfiles></container>')
            z.writestr('scores/main.xml', xml)
        return target.getvalue()

    def test_zip_methods_and_descriptors(self):
        xml = score(measure(note() + note('E')))
        expected = self.run_score(xml)
        for method in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
            for descriptor in (False, True):
                with self.subTest(method=method, descriptor=descriptor):
                    self.assertEqual(self.run_score(self.archive(xml, method, descriptor)), expected)
        # Enough text to exercise a dynamic Huffman block and long back-references.
        xml = score(*(measure(note('G') * 20, i) for i in range(30)))
        self.assertEqual(len(self.notes(self.run_score(self.archive(xml)))), 600)

    def test_zip_failures(self):
        archive = self.archive(score(measure(note())), zipfile.ZIP_STORED)
        for length in (0, 1, 20, len(archive)-1, len(archive)-22):
            self.run_score(archive[:length], ok=False)
        corrupt = bytearray(archive)
        corrupt[corrupt.index(b'<score-partwise>')] ^= 1
        self.run_score(corrupt, ok=False)
        bomb = bytearray(archive)
        central = bomb.index(b'PK\x01\x02')
        struct.pack_into('<I', bomb, central + 24, 0x7fffffff)
        self.run_score(bomb, ok=False)
        encrypted = bytearray(archive)
        struct.pack_into('<H', encrypted, central + 8, 1)
        self.run_score(encrypted, ok=False)
        self.run_score(archive, '--score-max-bytes', '100', ok=False)

    def test_repeats_and_endings(self):
        first = '<barline location="left"><ending number="1" type="start"/></barline>'
        stop = '<barline><ending number="1" type="stop"/><repeat direction="backward"/></barline>'
        second = '<barline location="left"><ending number="2" type="start"/></barline>'
        end = '<barline><ending number="2" type="discontinue"/></barline>'
        xml = score(measure(FORWARD + note()), measure(first + note('D') + stop, 2),
                    measure(second + note('E') + end, 3))
        parsed = self.run_score(xml)
        notes = self.notes(parsed)
        self.assertEqual([e['midi_pitch'] for e in notes], [60, 62, 60, 64])
        self.assertEqual([e['start_beats'] for e in notes], [0, 1, 2, 3])
        self.assertEqual([e['written_start_beats'] for e in notes], [0, 1, 0, 2])
        self.assertEqual([e['occurrence'] for e in notes], [1, 1, 2, 1])
        self.assertEqual(notes[0]['source_offset'], notes[2]['source_offset'])
        self.assertEqual(parsed['measure_visits'], 4)
        self.run_score(xml, '--score-max-visits', '2', ok=False)
        self.run_score(xml, '--score-max-events', '3', ok=False)
        third_pass = xml.replace(b'number="1" type=', b'number="1,2" type=').replace(b'number="2" type=', b'number="3" type=')
        self.assertEqual([e['midi_pitch'] for e in self.notes(self.run_score(third_pass))], [60, 62, 60, 62, 60, 64])

    def test_nested_and_implicit_repeats(self):
        xml = score(measure(FORWARD + note()), measure(FORWARD + note('D') + BACKWARD, 2),
                    measure(note('E') + BACKWARD, 3))
        self.assertEqual([e['midi_pitch'] for e in self.notes(self.run_score(xml))], [60, 62, 62, 64] * 2)
        xml = score(measure(note() + BACKWARD))
        self.assertEqual(len(self.notes(self.run_score(xml))), 2)
        self.run_score(score(measure(FORWARD + note())), ok=False)

    def test_tempo_restored_at_repeat(self):
        xml = score(measure('<sound tempo="80"/>' + note()),
                    measure(FORWARD + note('D'), 2),
                    measure('<sound tempo="120"/>' + note('E') + BACKWARD, 3))
        parsed = self.run_score(xml)
        self.assertEqual([(e['start_beats'], e['tempo_bpm']) for e in parsed['events'] if e['kind'] == 'tempo'],
                         [(0, 80), (2, 120), (3, 80), (4, 120)])
        self.run_score(score(measure('<sound dacapo="yes"/>' + note())), ok=False)

    def test_da_capo_and_coda(self):
        xml = score(measure(note() + '<sound fine="yes"/>'),
                    measure(note('D') + '<sound dacapo="yes"/>', 2))
        self.assertEqual([e['midi_pitch'] for e in self.notes(self.run_score(xml))], [60, 62, 60])
        xml = score(measure('<sound segno="s"/>' + note() + '<sound tocoda="c"/>'),
                    measure(note('D') + '<sound dalsegno="s"/>', 2),
                    measure('<sound coda="c"/>' + note('E'), 3))
        self.assertEqual([e['midi_pitch'] for e in self.notes(self.run_score(xml))], [60, 62, 60, 64])
        self.run_score(score(measure(note() + '<sound dalsegno="missing"/>')), ok=False)

    def test_controls_and_dynamics(self):
        xml = score(measure('<direction><direction-type><dynamics><p/></dynamics><pedal type="start"/></direction-type></direction>'
                            + note() + '<direction><sound dynamics="80" damper-pedal="50" soft-pedal="yes"/></direction>'
                            + note('D') + '<direction><direction-type><pedal type="change"/></direction-type></direction>'
                            + note('E')))
        parsed = self.run_score(xml, '--score-mode', 'performance')
        self.assertEqual([e['velocity'] for e in self.notes(parsed)], [45, 72, 72])
        controls = [(e['controller'], e['value']) for e in parsed['events'] if e['kind'] == 'control']
        self.assertEqual(controls, [(64, 127), (64, 63.5), (67, 127), (64, 0), (64, 127)])

    def test_grace_ornaments_and_articulations(self):
        grace = '<note><grace steal-time-following="25"/><pitch><step>D</step><octave>4</octave></pitch></note>'
        xml = score(measure(grace + note(duration=2) + note('E', extra='<notations><articulations><staccato/><accent/></articulations></notations>')
                            + note('G', extra='<notations><ornaments><trill-mark beats="4" trill-step="half"/></ornaments></notations>')))
        written = self.run_score(xml)
        self.assertEqual(len(self.notes(written)), 3)
        performed = self.run_score(xml, '--score-mode', 'performance')
        notes = self.notes(performed)
        self.assertEqual(len(notes), 7)
        self.assertEqual([(e['start_beats'], e['duration_beats']) for e in notes[:3]], [(0, .5), (.5, 1.5), (2, .5)])
        self.assertEqual([e['midi_pitch'] for e in notes[3:]], [67, 68, 67, 68])
        self.assertEqual([e['written_duration_beats'] for e in notes[3:]], [1]*4)
        self.assertGreater(performed['interpreted_events'], 0)
        self.assertEqual(self.notes(written)[0]['velocity'], None)

    def test_sostenuto_release_and_grace_chord(self):
        grace = '<note><grace/><pitch><step>D</step><octave>4</octave></pitch></note>'
        chord = '<note><grace/><chord/><pitch><step>F</step><octave>4</octave></pitch></note>'
        xml = score(measure('<direction><direction-type><pedal type="sostenuto" number="2"/></direction-type></direction>'
                            + grace + chord + note() + '<direction><direction-type><pedal type="stop" number="2"/></direction-type></direction>'))
        parsed = self.run_score(xml, '--score-mode', 'performance')
        self.assertEqual([(e['controller'], e['value']) for e in parsed['events'] if e['kind'] == 'control'], [(66, 127), (66, 0)])
        self.assertEqual([(e['start_beats'], e['duration_beats']) for e in self.notes(parsed)], [(0, .125), (0, .125), (.125, .875)])

    def test_tempo_and_output_safety(self):
        xml = score(measure(note()))
        parsed = self.run_score(xml, '--score-tempo-bpm', '0')
        self.assertFalse(parsed['used_default_tempo'])
        self.assertFalse(any(e['kind'] == 'tempo' for e in parsed['events']))
        parsed = self.run_score(xml, '--score-tempo-bpm', '72')
        self.assertEqual(parsed['events'][0]['tempo_bpm'], 72)
        source = self.root / 'score.input'
        self.run_score(xml, '--output', str(source), '--replace', ok=False)
        self.assertEqual(source.read_bytes(), xml)
        self.run_score(xml, '--score-mode', 'performance', '--score-grace-fraction', '2', ok=False)
        self.run_score(xml, '--fft-size', '1024', ok=False)


if __name__ == '__main__':
    unittest.main()

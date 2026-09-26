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

    def test_expression_metadata_survives_repeat_expansion(self):
        body = (FORWARD + '<direction><direction-type><words>Andante &amp; dolce</words>'
                '<dynamics><p/></dynamics><wedge type="crescendo" number="2"/>'
                '</direction-type></direction>' + note(duration=1) +
                '<direction><direction-type><wedge type="continue" number="2"/>'
                '</direction-type></direction>' + note(duration=1) +
                '<direction><direction-type><wedge type="stop" number="2"/>'
                '<dynamics><other-dynamics>subito pp</other-dynamics></dynamics>'
                '</direction-type><voice>2</voice><staff>2</staff></direction>' +
                note(duration=1, extra='<notations><dynamics><ff/></dynamics></notations>') + BACKWARD)
        parsed = self.run_score(score(measure(body)))
        marks = [e for e in parsed['events'] if e['kind'] == 'mark']
        words = [e for e in marks if e['mark_tag'] == 'words']
        self.assertEqual([e['mark_text'] for e in words], ['Andante & dolce'] * 2)
        self.assertEqual([e['occurrence'] for e in words], [1, 2])
        self.assertTrue(all(e['staff'] == '' and e['voice'] == '' for e in words))
        wedges = [e for e in marks if e['mark_tag'] == 'wedge']
        self.assertEqual([e['mark_type'] for e in wedges], ['crescendo', 'continue', 'stop'] * 2)
        self.assertTrue(all(e['mark_number'] == '2' for e in wedges))
        self.assertEqual([e['written_start_beats'] for e in wedges], [0, 1, 2] * 2)
        other = [e for e in marks if e['mark_tag'] == 'other-dynamics']
        self.assertTrue(all(e['mark_text'] == 'subito pp' and e['staff'] == '2' and e['voice'] == '2'
                            for e in other))
        self.assertEqual(len([e for e in marks if e['mark_tag'] == 'ff']), 2)
        self.assertTrue(all(e['velocity'] is None for e in self.notes(parsed)))

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

    def test_note_dynamics_keep_written_marks_separate_from_playback(self):
        body = note(extra='<notations><dynamics><ff/></dynamics></notations>')
        xml = score(measure(body.replace('<note>', '<note dynamics="50">') + note('D')))
        parsed = self.run_score(xml, '--score-mode', 'performance')
        mark = next(e for e in parsed['events'] if e['kind'] == 'mark' and e['mark'] == 'ff')
        self.assertFalse(mark['interpretation'] & 2)
        self.assertEqual([e['velocity'] for e in self.notes(parsed)], [45, 104])

    def test_note_attached_dynamics_apply_to_the_note_but_cues_do_not_play(self):
        for cue in (False, True):
            body = note(before='<cue/>' if cue else '',
                        extra='<notations><dynamics><ff/></dynamics></notations>')
            parsed = self.run_score(score(measure(body + note('D'))),
                                    '--score-mode', 'performance')
            self.assertEqual([e['velocity'] for e in self.notes(parsed)],
                             [64] if cue else [104, 104])
            mark = next(e for e in parsed['events'] if e['kind'] == 'mark' and e['mark'] == 'ff')
            self.assertEqual(bool(mark['interpretation'] & 32), cue)

    def test_direction_and_sound_offsets_keep_separate_positions(self):
        for sound_attribute, explicit, expected in [('', '', 2), (' sound="no"', '', 2),
                (' sound="yes"', '', 1), ('', '<offset>1</offset>', 3),
                (' sound="yes"', '<offset>1</offset>', 3)]:
            with self.subTest(sound_attribute=sound_attribute, explicit=explicit):
                direction = ('<direction><direction-type><words>Andante</words>'
                    '<dynamics><p/></dynamics></direction-type>'
                    f'<offset{sound_attribute}>-1</offset>'
                    f'<sound tempo="90" dynamics="50" damper-pedal="yes">{explicit}</sound>'
                    '</direction>')
                parsed = self.run_score(score(measure(note(duration=2) + direction + note('D', 2))),
                                        '--score-tempo-bpm', '0')
                written = [e for e in parsed['events'] if e['kind'] == 'direction'
                           or e.get('mark_tag') in ('words', 'p')]
                playback = [e for e in parsed['events'] if e['kind'] in ('tempo', 'control')
                            or e.get('mark') == 'velocity']
                self.assertEqual(len(written), 3)
                self.assertEqual(len(playback), 3)
                self.assertTrue(all(e['start_beats'] == e['written_start_beats'] == 1
                                    for e in written))
                self.assertTrue(all(e['start_beats'] == e['written_start_beats'] == expected
                                    for e in playback))

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

    def test_cues_keep_notation_without_becoming_played_notes(self):
        cue = note(before='<cue/>', extra='<notations><articulations><staccato/></articulations>'
                   '<ornaments><trill-mark/></ornaments></notations>')
        xml = score(measure(cue.replace('<note>', '<note dynamics="90">') + note('D')))
        for mode in ('written', 'performance'):
            with self.subTest(mode=mode):
                parsed = self.run_score(xml, '--score-mode', mode, '--score-tempo-bpm', '0')
                cues = [e for e in parsed['events'] if e['kind'] == 'cue']
                self.assertEqual(len(cues), 1)
                self.assertEqual(cues[0]['midi_pitch'], 60)
                self.assertEqual(cues[0]['start_beats'], 0)
                self.assertEqual(cues[0]['duration_beats'], 1)
                self.assertIsNone(cues[0]['velocity'])
                self.assertEqual([(e['midi_pitch'], e['start_beats'])
                                 for e in self.notes(parsed)], [(62, 1)])
                self.assertEqual(parsed['duration_beats'], 2)

    def test_cue_chords_rests_grace_and_small_played_notes(self):
        grace = ('<note><grace steal-time-following="50"/><cue/>'
                 '<pitch><step>B</step><octave>4</octave></pitch></note>')
        body = (FORWARD + grace + note(before='<cue/>')
                + note('E', before='<chord/>')
                + note('G', before='<cue/><chord/>')
                + '<note><cue/><rest/><duration>1</duration></note>'
                + note('D', extra='<type size="cue">quarter</type>') + BACKWARD)
        for mode in ('written', 'performance'):
            with self.subTest(mode=mode):
                parsed = self.run_score(score(measure(body)), '--score-mode', mode,
                                        '--score-tempo-bpm', '0')
                self.assertEqual([(e['midi_pitch'], e['start_beats'], e['duration_beats'])
                                 for e in self.notes(parsed)],
                                 [(64, 0, 1), (62, 2, 1), (64, 3, 1), (62, 5, 1)])
                cues = [e for e in parsed['events'] if e['kind'] == 'cue']
                self.assertEqual(len(cues), 8)
                self.assertEqual(sum(e['midi_pitch'] is None for e in cues), 2)
                self.assertEqual(sum(e['duration_beats'] == 0 for e in cues), 2)
                self.assertTrue(all(e['velocity'] is None for e in cues))
                self.assertEqual(parsed['duration_beats'], 6)

    def test_cues_still_validate_durations_and_sound_ties(self):
        for body in (note(before='<cue/><cue/>'), note(duration=0, before='<cue/>'),
                     note(before='<cue/>', extra='<tie type="start"/>')):
            self.run_score(score(measure(body)), ok=False)

    def test_tuplet_rounding_padding_does_not_extend_measure(self):
        ratio = ('<type>32nd</type><time-modification><actual-notes>9</actual-notes>'
                 '<normal-notes>8</normal-notes></time-modification>')
        attrs = ('<attributes><divisions>480</divisions>'
                 '<time><beats>3</beats><beat-type>4</beat-type></time></attributes>')
        body = (note(duration=480) + note(duration=53, extra=ratio) * 9
                + '<forward><duration>3</duration></forward>' + note('D', duration=480))
        xml = score('<measure number="1">' + attrs + body + '</measure>')
        strict = self.run_score(xml)
        self.assertEqual(strict['repaired_tuplets'], 0)
        parsed = self.run_score(xml, '--score-repair-tuplets')
        self.assertEqual(parsed['duration_beats'], 3)
        self.assertEqual(parsed['repaired_tuplets'], 9)
        self.assertEqual(parsed['repaired_tuplet_forwards'], 1)
        self.assertAlmostEqual(self.notes(parsed)[-1]['start_beats'], 2)
        # A different forward duration is not evidence of rounding padding.
        for ticks in (2, 4, 483):
            self.run_score(xml.replace(b'<duration>3</duration>',
                                       f'<duration>{ticks}</duration>'.encode()),
                           '--score-repair-tuplets', ok=False)

    def test_overfull_measure_policy_preserves_declared_time(self):
        attrs = '<attributes><time><beats>1</beats><beat-type>4</beat-type></time></attributes>'
        xml = score(measure(attrs + FORWARD + note(duration=2) + BACKWARD),
                    measure(note('D'), 2))
        result = self.run_score(xml, ok=False)
        self.assertIn('measure exceeds its meter', result.stderr)
        self.assertIn('part P, measure 1', result.stderr)
        self.assertIn('2 > 1 quarter notes', result.stderr)
        parsed = self.run_score(xml, '--score-overfull-measures', 'preserve')
        self.assertEqual(parsed['policy']['overfull_measures'], 'preserve')
        self.assertEqual(parsed['overfull_measures'], 1)
        self.assertEqual(parsed['duration_beats'], 5)
        notes = self.notes(parsed)
        self.assertEqual([e['start_beats'] for e in notes], [0, 2, 4])
        self.assertEqual([bool(e['interpretation'] & 16) for e in notes], [True, True, False])
        self.run_score(xml, '--score-overfull-measures', 'error', ok=False)
        self.run_score(xml, '--score-overfull-measures', 'guess', ok=False)
        # This policy does not accept a backup before beat zero.
        self.run_score(score(measure('<backup><duration>1</duration></backup>' + note())),
                       '--score-overfull-measures', 'preserve', ok=False)


    def test_rounded_tuplet_repair(self):
        tuplet = ('<voice>1</voice><type>16th</type><time-modification>'
                  '<actual-notes>7</actual-notes><normal-notes>4</normal-notes></time-modification>')
        attrs = ('<attributes><divisions>480</divisions>'
                 '<time><beats>1</beats><beat-type>4</beat-type></time></attributes>')
        upper = note(duration=69, extra=tuplet) * 7
        lower = note('G', duration=480, extra='<voice>2</voice>')
        xml = score('<measure number="1">' + attrs + FORWARD + upper
                    + '<backup><duration>480</duration></backup>' + lower + BACKWARD + '</measure>',
                    '<measure number="2">' + note('D', duration=480) + '</measure>')
        result = self.run_score(xml, ok=False)
        self.assertIn('measure exceeds its meter', result.stderr)
        for data in (xml, self.archive(xml)):
            parsed = self.run_score(data, '--score-repair-tuplets', '--score-tempo-bpm', '0')
            self.assertEqual(parsed['repaired_tuplets'], 7)
            self.assertTrue(parsed['policy']['repair_tuplets'])
            self.assertEqual(parsed['duration_beats'], 3)
            notes = self.notes(parsed)
            self.assertEqual(len(notes), 17)
            self.assertEqual([e['start_beats'] for e in notes if e['voice'] == '2'], [0, 1])
            for e in notes:
                if e['interpretation'] & 4:
                    self.assertAlmostEqual(e['duration_beats'], 1/7)
                    self.assertAlmostEqual(e['written_duration_beats'], 1/7)
            performed = self.run_score(data, '--score-repair-tuplets', '--score-mode', 'performance')
            self.assertEqual(performed['repaired_tuplets'], 7)
        self.run_score(xml, '--score-repair-tuplets', '--score-repair-tuplets', ok=False)

    def test_tuplet_repair_does_not_hide_bad_durations(self):
        attrs = '<attributes><divisions>480</divisions><time><beats>1</beats><beat-type>4</beat-type></time></attributes>'
        ratio = '<type>16th</type><time-modification><actual-notes>7</actual-notes><normal-notes>4</normal-notes></time-modification>'
        for body in (note(duration=481), note(duration=70, extra=ratio)*7,
                     note(duration=69, extra=ratio)*7 + '<backup><duration>240</duration></backup>',
                     note(duration=69, extra=ratio.replace('>7<', '>0<')),
                     note(duration=69, extra=ratio.replace('>7<', '>1.5<'))):
            self.run_score(score('<measure number="1">' + attrs + body + '</measure>'),
                           '--score-repair-tuplets', ok=False)
        # Explicit fractional durations must not be guessed back to notation.
        xml = score('<measure number="1">' + attrs + note(duration=68.6, extra=ratio) + '</measure>')
        parsed = self.run_score(xml, '--score-repair-tuplets')
        self.assertEqual(parsed['repaired_tuplets'], 0)
        self.assertAlmostEqual(self.notes(parsed)[0]['duration_beats'], 68.6/480)

    def test_tuplet_repair_absorbs_backward_rounding_padding(self):
        ratio = ('<type>16th</type><time-modification><actual-notes>7</actual-notes>'
                 '<normal-notes>4</normal-notes></time-modification>')
        xml = score('<measure number="1"><attributes><divisions>480</divisions></attributes>'
                    + note(duration=69, extra=ratio)*7
                    + '<backup><duration>3</duration></backup>' + note('D', duration=480)
                    + '</measure>')
        for mode in ('written', 'performance'):
            parsed = self.run_score(xml, '--score-repair-tuplets', '--score-mode', mode)
            self.assertEqual(parsed['repaired_tuplet_backups'], 1)
            self.assertEqual(parsed['repaired_tuplet_forwards'], 0)
            self.assertAlmostEqual(self.notes(parsed)[-1]['start_beats'], 1)
            self.assertAlmostEqual(parsed['duration_beats'], 2)
        # Nearby values are not enough evidence of rounding padding.
        self.run_score(xml.replace(b'<duration>3</duration>', b'<duration>4</duration>'),
                       '--score-repair-tuplets', ok=False)

    def test_partial_backup_after_tuplets_can_rewind_unchanged_notes(self):
        ratio = ('<voice>1</voice><type>16th</type><time-modification><actual-notes>7</actual-notes>'
                 '<normal-notes>4</normal-notes></time-modification>')
        xml = score('<measure number="1"><attributes><divisions>484</divisions></attributes>'
                    + note(duration=69, extra=ratio)*7 + note('D', duration=484)
                    + note('E', duration=484) + '<backup><duration>484</duration></backup>'
                    + note('F', duration=484, extra='<voice>2</voice>') + '</measure>')
        parsed = self.run_score(xml, '--score-repair-tuplets')
        self.assertEqual(parsed['repaired_tuplet_backups'], 1)
        self.assertAlmostEqual(parsed['duration_beats'], 3)
        lower = next(e for e in self.notes(parsed) if e['voice'] == '2')
        self.assertAlmostEqual(lower['start_beats'], 2)

    def test_dotted_tuplet_chords_and_rests(self):
        ratio = ('<type>16th</type><dot/><time-modification><actual-notes>7</actual-notes>'
                 '<normal-notes>4</normal-notes><normal-type>16th</normal-type>'
                 '<normal-dot/></time-modification>')
        xml = score('<measure number="1"><attributes><divisions>480</divisions></attributes>'
                    + note(duration=103, extra=ratio)
                    + note('E', duration=103, extra=ratio, before='<chord/>')
                    + '<note><rest/><duration>103</duration>' + ratio + '</note></measure>')
        strict = self.run_score(xml)
        self.assertEqual(strict['repaired_tuplets'], 0)
        self.assertAlmostEqual(self.notes(strict)[0]['duration_beats'], 103/480)
        repaired = self.run_score(xml, '--score-repair-tuplets', '--score-tempo-bpm', '0')
        self.assertEqual(repaired['repaired_tuplets'], 3)
        self.assertAlmostEqual(repaired['duration_beats'], 3/7)
        for e, onset in zip(repaired['events'], (0, 0, 3/14)):
            self.assertAlmostEqual(e['start_beats'], onset)
            self.assertAlmostEqual(e['duration_beats'], 3/14)
            self.assertTrue(e['interpretation'] & 4)

    def test_tempo_conflict_policy(self):
        xml = score(measure('<sound tempo="130"/><sound tempo="110"/>' + note()))
        self.assertIn('conflicting tempos', self.run_score(xml, ok=False).stderr)
        self.assertIn('conflicting tempos', self.run_score(xml, '--score-tempo-conflicts', 'error', ok=False).stderr)
        parsed = self.run_score(xml, '--score-tempo-conflicts', 'last')
        tempos = [e for e in parsed['events'] if e['kind'] == 'tempo']
        self.assertEqual([e['tempo_bpm'] for e in tempos], [110])
        self.assertEqual(parsed['tempo_conflicts'], 1)
        self.assertEqual(parsed['policy']['tempo_conflicts'], 'last')
        self.assertTrue(tempos[0]['interpretation'] & 8)
        self.run_score(xml, '--score-tempo-conflicts', 'guess', ok=False)
        same = self.run_score(score(measure('<sound tempo="110"/><sound tempo="110"/>' + note())))
        self.assertEqual(same['tempo_conflicts'], 0)

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

    def test_voice_dynamics_precedence_and_part_reset(self):
        body = ('<direction><voice>1</voice><sound dynamics="40"/></direction>'
                + note('C', extra='<voice>1</voice>')
                + '<backup><duration>1</duration></backup>'
                + note('E', extra='<voice>2</voice>')
                + '<sound dynamics="100"/>'
                + note('D', extra='<voice>1</voice>')
                + '<backup><duration>1</duration></backup>'
                + note('F', extra='<voice>2</voice>')
                + note('G', extra='<voice>1</voice>').replace('<note>', '<note dynamics="10">'))
        xml = ('<score-partwise><part-list>'
               '<score-part id="A"><part-name>A</part-name></score-part>'
               '<score-part id="B"><part-name>B</part-name></score-part>'
               '</part-list><part id="A">' + measure(body) + '</part>'
               '<part id="B">' + measure(note(duration=3)) + '</part></score-partwise>').encode()
        for mode, default in [('written', None), ('performance', 64)]:
            with self.subTest(mode=mode):
                parsed = self.run_score(xml, '--score-mode', mode)
                velocities = {(e['part'], e['midi_pitch']): e['velocity'] for e in self.notes(parsed)}
                self.assertEqual(velocities, {('A', 60): 36, ('A', 64): default,
                                             ('A', 62): 90, ('A', 65): 90,
                                             ('A', 67): 9, ('B', 60): default})

    def test_staff_dynamics_do_not_change_the_other_staff(self):
        for mode in ('written', 'performance'):
            marks = ('<sound dynamics="50"/>'
                '<direction><staff>2</staff><sound dynamics="100"/></direction>')
            body = (marks + note('C', extra='<voice>1</voice><staff>1</staff>')
                + '<backup><duration>1</duration></backup>'
                + note('E', extra='<voice>1</voice><staff>2</staff>')
                + '<sound dynamics="60"/>'
                + note('D', extra='<voice>1</voice><staff>1</staff>')
                + '<backup><duration>1</duration></backup>'
                + note('F', extra='<voice>1</voice><staff>2</staff>'))
            parsed = self.run_score(score(measure(body)), '--score-mode', mode)
            self.assertEqual([e['velocity'] for e in self.notes(parsed)], [45, 90, 54, 54])

    def test_grace_group_borrows_from_previous_chord(self):
        grace = ('<note><grace steal-time-previous="25"/>{chord}'
                 '<pitch><step>{step}</step><octave>4</octave></pitch></note>')
        xml = score(measure(note(duration=2) + note('E', duration=2, before='<chord/>')
                            + grace.format(step='D', chord='')
                            + grace.format(step='F', chord='<chord/>')
                            + grace.format(step='G', chord='') + note('B')))
        parsed = self.run_score(xml, '--score-mode', 'performance')
        notes = self.notes(parsed)
        self.assertEqual([(e['midi_pitch'], e['start_beats'], e['duration_beats']) for e in notes],
                         [(60, 0, 1.5), (64, 0, 1.5), (62, 1.5, .25),
                          (65, 1.5, .25), (67, 1.75, .25), (71, 2, 1)])
        for event in notes[2:5]:
            self.assertEqual(event['written_start_beats'], 2)
            self.assertEqual(event['written_duration_beats'], 0)
            self.assertEqual(event['grace_previous_percent'], 25)
            self.assertIsNone(event['grace_following_percent'])
            self.assertEqual(event['interpretation'], 3)  # Default velocity plus XML timing.
        self.assertEqual(parsed['interpreted_events'], 6)

    def test_grace_timing_errors_and_unrendered_group(self):
        grace = '<note><grace {timing}/><pitch><step>D</step><octave>4</octave></pitch></note>'
        cases = [
            (grace.format(timing='steal-time-previous="25"') + note(), 'no preceding note'),
            (note() + grace.format(timing='steal-time-following="25"'), 'no following note'),
            (note() + grace.format(timing='steal-time-previous="25"')
             + grace.format(timing='steal-time-previous="50"'), 'conflicting timing attributes'),
        ]
        for body, error in cases:
            with self.subTest(error=error):
                result = self.run_score(score(measure(body)), '--score-mode', 'performance', ok=False)
                self.assertIn(error, result.stderr)
        body = grace.format(timing='') + '<note><rest/><duration>1</duration></note>'
        parsed = self.run_score(score(measure(body)), '--score-mode', 'performance')
        self.assertEqual(parsed['unrendered_marks'], 1)
        remaining = [e for e in parsed['events'] if e['kind'] == 'grace']
        self.assertEqual(len(remaining), 1)
        self.assertEqual(remaining[0]['duration_beats'], 0)

    def test_report_nullable_fields_and_source_order(self):
        xml = score(measure('<sound tempo="90" damper-pedal="50"/>'
                            '<direction><direction-type><words>A &amp; &quot;B&quot;</words>'
                            '</direction-type></direction>' + note()
                            + '<note><rest/><duration>1</duration></note>'))
        parsed = self.run_score(xml)
        fields = {'index', 'kind', 'id', 'part', 'part_name', 'voice', 'staff', 'measure', 'mark',
                  'start_beats', 'duration_beats', 'written_start_beats', 'written_duration_beats',
                  'measure_index', 'occurrence', 'tie', 'source_offset', 'source_size', 'sequence',
                  'interpretation', 'articulations', 'chord', 'midi_pitch', 'velocity', 'tempo_bpm',
                  'controller', 'value', 'grace_previous_percent', 'grace_following_percent',
                  'grace_make_beats', 'mark_tag', 'mark_text', 'mark_type', 'mark_number',
                  'tempo_source', 'metronome'}
        for index, event in enumerate(parsed['events']):
            self.assertEqual(set(event), fields)
            self.assertEqual(event['index'], index)
            self.assertIsNone(event['velocity'])
            for key in ('grace_previous_percent', 'grace_following_percent', 'grace_make_beats'):
                self.assertIsNone(event[key])
            if event['kind'] != 'note':
                self.assertIsNone(event['midi_pitch'])
            if event['kind'] != 'tempo':
                self.assertIsNone(event['tempo_bpm'])
            if event['kind'] != 'control':
                self.assertIsNone(event['controller'])
                self.assertIsNone(event['value'])
        self.assertEqual(parsed['events'][0]['kind'], 'tempo')
        controls = [e for e in parsed['events'] if e['kind'] == 'control']
        self.assertEqual([(e['controller'], e['value']) for e in controls], [(64, 63.5)])
        self.assertTrue(any(e['mark'] == 'A & "B"' for e in parsed['events']))

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

    def test_tempo_sources_and_metronome_units(self):
        default = self.run_score(score(measure(note())))
        self.assertEqual(default['events'][0]['tempo_source'], 'default')
        for unit, dots, rate, expected in [('half', 0, 80, 160),
                                          ('quarter', 1, 80, 120),
                                          ('eighth', 2, 80, 70)]:
            with self.subTest(unit=unit, dots=dots):
                xml = score(measure('<direction><direction-type><metronome>'
                    f'<beat-unit>{unit}</beat-unit>' + '<beat-unit-dot/>' * dots +
                    f'<per-minute>{rate}</per-minute></metronome></direction-type>'
                    '</direction>' + note()))
                parsed = self.run_score(xml)
                tempo = next(e for e in parsed['events'] if e['kind'] == 'tempo')
                self.assertEqual((tempo['tempo_source'], tempo['tempo_bpm']),
                                 ('metronome', expected))
                mark = next(e for e in parsed['events'] if e['mark_tag'] == 'metronome')
                self.assertEqual(mark['metronome'], dict(beat_unit=unit, dots=dots,
                    per_minute=str(rate), quarter_bpm=expected, visible=True))

    def test_playback_tempo_does_not_discard_printed_mark(self):
        for printed in ('yes', 'no'):
            with self.subTest(printed=printed):
                xml = score(measure('<direction><direction-type><words>Andante</words>'
                    f'</direction-type><direction-type><metronome print-object="{printed}">'
                    '<beat-unit>half</beat-unit><per-minute>50</per-minute></metronome>'
                    '</direction-type><offset>1</offset><sound tempo="120"><offset>2</offset>'
                    '</sound></direction>' + note(duration=3)))
                parsed = self.run_score(xml, '--score-tempo-bpm', '0')
                tempo = next(e for e in parsed['events'] if e['kind'] == 'tempo')
                mark = next(e for e in parsed['events'] if e['mark_tag'] == 'metronome')
                self.assertEqual((tempo['tempo_source'], tempo['tempo_bpm'], tempo['start_beats']),
                                 ('sound', 120, 2))
                self.assertEqual(mark['start_beats'], 1)
                self.assertEqual(mark['metronome'], dict(beat_unit='half', dots=0,
                    per_minute='50', quarter_bpm=100, visible=printed == 'yes'))
                self.assertTrue(any(e['mark_text'] == 'Andante' for e in parsed['events']))

    def test_tempo_source_survives_repeat_restore(self):
        xml = score(measure('<sound tempo="80"/>' + note()),
                    measure(FORWARD + note('D'), 2),
                    measure('<sound tempo="120"/>' + note('E') + BACKWARD, 3))
        parsed = self.run_score(xml)
        restored = next(e for e in parsed['events'] if e['mark'] == 'tempo-restore')
        self.assertEqual((restored['tempo_source'], restored['tempo_bpm']), ('sound', 80))
        xml = score(measure(FORWARD + note()),
                    measure('<sound tempo="90"/>' + note('D') + BACKWARD, 2))
        parsed = self.run_score(xml)
        restored = next(e for e in parsed['events'] if e['mark'] == 'tempo-restore')
        self.assertEqual((restored['tempo_source'], restored['tempo_bpm']), ('default', 120))

    def test_non_numeric_metronomes_are_not_guessed(self):
        for content in ('<beat-unit>quarter</beat-unit><per-minute>80-100</per-minute>',
                        '<beat-unit>half</beat-unit><beat-unit>quarter</beat-unit>',
                        '<beat-unit>quarter</beat-unit><beat-unit-tied><beat-unit>eighth</beat-unit>'
                        '</beat-unit-tied><per-minute>60</per-minute>',
                        '<beat-unit>quarter</beat-unit><per-minute>80<x/></per-minute>'):
            with self.subTest(content=content):
                direction = ('<direction><direction-type><metronome>' + content +
                             '</metronome></direction-type>{}</direction>')
                self.assertIn('unsupported metronome',
                    self.run_score(score(measure(direction.format('') + note())), ok=False).stderr)
                parsed = self.run_score(score(measure(direction.format('<sound tempo="90"/>') + note())))
                mark = next(e for e in parsed['events'] if e['mark_tag'] == 'metronome')
                self.assertIsNone(mark['metronome']['quarter_bpm'])
                self.assertEqual(parsed['events'][0]['tempo_source'], 'sound')
                self.assertEqual(parsed['events'][0]['tempo_bpm'], 90)


if __name__ == '__main__':
    unittest.main()

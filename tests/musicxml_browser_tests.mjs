import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import {mkdtemp, readFile, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {createMusicXMLReader} from '../portable/musicxml.mjs';

const [wasm, native] = process.argv.slice(2);
assert.ok(wasm, 'Usage: node tests/musicxml_browser_tests.mjs MODULE.wasm [ANALYZER]');
const read = await createMusicXMLReader(await readFile(wasm));
const encode = text => new TextEncoder().encode(text);
const xml = measures => encode(`<?xml version="1.0"?>
<score-partwise><part-list><score-part id="P"><part-name>Piano &amp; keys</part-name>
</score-part></part-list><part id="P">${measures}</part></score-partwise>`);
const measure = index => `<measure number="${index + 1}"><attributes><divisions>3</divisions>
<time><beats>4</beats><beat-type>4</beat-type></time></attributes>
<note><pitch><step>C</step><octave>4</octave></pitch><duration>2</duration><voice>1</voice></note>
<note><rest/><duration>10</duration><voice>1</voice></note></measure>`;
const small = xml(measure(0));
const pedal = xml(`<measure number="1"><attributes><divisions>1</divisions></attributes>
<sound damper-pedal="50" sostenuto-pedal="yes" soft-pedal="no"/>
<note><rest/><duration>1</duration></note><barline><repeat direction="backward"/></barline></measure>`);
const pedalScore = JSON.parse(read(pedal));
for (const occurrence of [1, 2]) {
  const events = pedalScore.events.filter(event => event.occurrence === occurrence &&
    ['control', 'direction'].includes(event.kind));
  assert.deepEqual(events.map(event => [event.kind, event.sequence]),
    [['direction', 0], ['control', 1], ['control', 2], ['control', 3]]);
}
const expected = JSON.parse(read(small));
assert.equal(expected.mode, 'written');
assert.equal(expected.part_count, 1);
assert.equal(expected.events.find(event => event.kind === 'note').duration_beats, 2 / 3);
assert.throws(() => read(new Uint8Array()), /1 byte to 32 MiB/);
assert.throws(() => read(new Uint8Array(32 * 1024 * 1024 + 1)), /1 byte to 32 MiB/);
assert.throws(() => read('not bytes'), /Uint8Array/);
for (let i = 0; i < 30; i++) {
  assert.throws(() => read(encode('<score-partwise><broken>')), /MusicXML/);
  assert.deepEqual(JSON.parse(read(small)), expected);
}
// A larger import grows linear memory; the next read must not use a stale view.
const large = xml(Array.from({length: 1000}, (_, i) => measure(i)).join(''));
assert.equal(JSON.parse(read(large)).events.filter(event => event.kind === 'note').length, 1000);
assert.deepEqual(JSON.parse(read(small)), expected);
const header = await readFile(new URL('./musicxml_mxl_fixture.h', import.meta.url), 'utf8');
const compressed = Uint8Array.from(header.match(/0x[0-9a-f]{2}/g), text => parseInt(text, 16));
const mxl = JSON.parse(read(compressed));
assert.equal(mxl.mode, 'written');
assert.ok(mxl.events.some(event => event.occurrence === 2));
assert.throws(() => read(compressed.subarray(0, 50)), /MusicXML/);
assert.deepEqual(JSON.parse(read(compressed)), mxl);
if (native) {
  const root = await mkdtemp(join(tmpdir(), 'musicxml-wasm-test-'));
  try {
    for (const bytes of [small, large, compressed, pedal]) {
      const source = join(root, 'score.input');
      await writeFile(source, bytes);
      const result = execFileSync(resolve(native), ['import-score', source], {maxBuffer: 16 * 1024 * 1024});
      assert.deepEqual(JSON.parse(read(bytes)), JSON.parse(result), 'WASM and native reports differ');
    }
  } finally { await rm(root, {recursive: true, force: true}); }
}
console.log('MusicXML WASM: XML, MXL, limits, memory growth, failure recovery, and report parity passed');

import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {readFile} from 'node:fs/promises';
import http from 'node:http';
import {createRequire} from 'node:module';
import path from 'node:path';
import {parseArgs} from 'node:util';

const {values: args} = parseArgs({options: {
  browser: {type: 'string'}, 'ort-dist': {type: 'string'}, model: {type: 'string'},
  'decoder-wasm': {type: 'string'}, 'playwright-module': {type: 'string', default: 'playwright'},
  'require-webnn': {type: 'boolean', default: false}, help: {type: 'boolean', default: false},
}});
if (args.help) {
  console.log('node tests/browser_basic_pitch_tests.mjs --browser CHROME --ort-dist ORT_1_29_0/dist --model nmp.onnx --decoder-wasm hwa_basic_pitch_browser_tests.wasm [--playwright-module PATH] [--require-webnn]');
  process.exit(0);
}
for (const name of ['browser', 'ort-dist', 'model', 'decoder-wasm']) {
  assert(args[name] && path.isAbsolute(args[name]), `--${name} needs an absolute path`);
}
const model = await readFile(args.model);
const modelHash = createHash('sha256').update(model).digest('hex');
assert.equal(modelHash, '2c3c1d144bfa61ad236e92e169c13535c880469a12a047d4e73451f2c059a0ec', 'Basic Pitch model hash changed');
const decoderBytes = await readFile(args['decoder-wasm']);
const decoderHash = createHash('sha256').update(decoderBytes).digest('hex');
const require = createRequire(import.meta.url);
const {chromium} = require(args['playwright-module']);
const server = http.createServer(async (request, response) => {
  try {
    const url = new URL(request.url, 'http://localhost');
    if (url.pathname === '/') {
      response.setHeader('Content-Type', 'text/html');
      response.end('<!doctype html><title>Basic Pitch backend test</title><script src="/ort/ort.all.min.js"></script>');
    } else if (url.pathname === '/model.onnx') {
      response.end(model);
    } else if (url.pathname === '/decoder.wasm') {
      response.setHeader('Content-Type', 'application/wasm');
      response.end(decoderBytes);
    } else if (/^\/ort\/[a-zA-Z0-9_-][a-zA-Z0-9_.-]*$/.test(url.pathname)) {
      response.setHeader('Content-Type', url.pathname.endsWith('.wasm') ? 'application/wasm' : 'text/javascript');
      response.end(await readFile(path.join(args['ort-dist'], url.pathname.slice(5))));
    } else { response.statusCode = 404; response.end(); }
  } catch { response.statusCode = 500; response.end(); }
});
let browser;
const deadline = setTimeout(() => { console.error('browser test timed out'); process.exitCode = 1; browser?.close(); server.close(); }, 120000);
try {
  await new Promise((resolve, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', resolve); });
  const origin = `http://127.0.0.1:${server.address().port}`;
  browser = await chromium.launch({headless: true, executablePath: args.browser,
    args: ['--enable-experimental-web-platform-features', '--enable-features=WebMachineLearningNeuralNetwork']});
  const page = await browser.newPage();
  await page.route('**/*', route => new URL(route.request().url()).origin === origin ? route.continue() : route.abort());
  await page.goto(origin);
  const result = await page.evaluate(async requireWebnn => {
    const check = (condition, message) => { if (!condition) throw Error(message); };
    check(ort.env.versions.web === '1.29.0', 'this test pins ONNX Runtime Web 1.29.0');
    ort.env.wasm.wasmPaths = '/ort/';
    ort.env.wasm.numThreads = 1;
    const module = await WebAssembly.compile(await (await fetch('/decoder.wasm')).arrayBuffer());
    for (const item of WebAssembly.Module.imports(module)) {
      check(item.kind === 'function' && item.module === 'wasi_snapshot_preview1' &&
        ['fd_close','fd_seek','fd_write'].includes(item.name), 'unexpected decoder import');
    }
    const instance = await WebAssembly.instantiate(module, {wasi_snapshot_preview1:
      Object.fromEntries(['fd_close','fd_seek','fd_write'].map(name => [name, () => {throw Error(`decoder attempted ${name}`);}]))});
    const decoder = instance.exports;
    decoder._initialize?.();
    const cases = [
      {id: 'silence', frequencies: [], expectedMidi: []},
      {id: 'a4', frequencies: [440], expectedMidi: [69]},
      {id: 'chord', frequencies: [440, 523.2511306011972], expectedMidi: [69, 72]},
    ];
    const baseline = new Map();
    const reports = [];
    let webnnAvailable = false;
    let webnnReason = null;
    if (navigator.ml) {
      try { const context = await navigator.ml.createContext({deviceType:'cpu'}); context.destroy(); webnnAvailable = true; }
      catch (error) { webnnReason = String(error); }
    } else webnnReason = 'navigator.ml is unavailable';
    check(!requireWebnn || webnnAvailable, webnnReason);
    for (const mode of webnnAvailable ? ['wasm', 'webnn-strict'] : ['wasm']) {
      const options = {executionProviders: mode === 'wasm' ? ['wasm'] : [{name:'webnn',deviceType:'cpu'}],
        freeDimensionOverrides: {unk__749: 1}};
      if (mode === 'webnn-strict') options.extra = {session:{disable_cpu_ep_fallback:'1'}};
      const session = await ort.InferenceSession.create('/model.onnx', options);
      try {
        check(session.inputNames.length === 1 && session.inputNames[0] === 'serving_default_input_2:0', 'unexpected model input');
        for (const item of cases) {
          const input = new Float32Array(43844);
          for (let i=5000; i<33000; i++) for (const hz of item.frequencies) input[i] += 0.25/item.frequencies.length*Math.sin(2*Math.PI*hz*i/22050);
          const tensor = new ort.Tensor('float32', input, [1,43844,1]);
          const outputs = await session.run({'serving_default_input_2:0':tensor});
          let maxDifference = 0;
          check(Object.keys(outputs).length === 3, 'unexpected output count');
          for (const [name, width] of [['StatefulPartitionedCall:0',264],['StatefulPartitionedCall:1',88],['StatefulPartitionedCall:2',88]]) {
            const output = outputs[name];
            check(output?.type === 'float32' && JSON.stringify(output.dims) === JSON.stringify([1,172,width]), 'unexpected model tensor');
            for (let i=0; i<output.data.length; i++) {
              const value = output.data[i];
              check(Number.isFinite(value) && value >= 0 && value <= 1, 'invalid model activation');
              if (baseline.has(item.id)) maxDifference = Math.max(maxDifference, Math.abs(value-baseline.get(item.id).tensors[name][i]));
            }
          }
          new Float32Array(decoder.memory.buffer,decoder.hwa_test_note_buffer(),172*88).set(outputs['StatefulPartitionedCall:1'].data);
          new Float32Array(decoder.memory.buffer,decoder.hwa_test_onset_buffer(),172*88).set(outputs['StatefulPartitionedCall:2'].data);
          check(decoder.hwa_test_decode() === 0, 'C decoder failed');
          const notes = Array.from({length:decoder.hwa_test_note_count()}, (_,i) => [0,1,2].map(field=>decoder.hwa_test_note_field(i,field)));
          const midi = notes.map(note=>note[2]).sort((a,b)=>a-b);
          check(JSON.stringify(midi) === JSON.stringify(item.expectedMidi), `${item.id} unexpected notes: ${JSON.stringify(notes)}`);
          if (baseline.has(item.id)) {
            check(maxDifference <= 0.001, `${item.id} activation drift: ${maxDifference}`);
            check(JSON.stringify(notes) === JSON.stringify(baseline.get(item.id).notes), `${item.id} decoded events changed across backends`);
          } else baseline.set(item.id, {notes, tensors:Object.fromEntries(Object.entries(outputs).map(([name,value])=>[name,new Float32Array(value.data)]))});
          reports.push({case:item.id, backend:mode, notes, max_activation_difference_from_wasm:maxDifference});
          for (const value of Object.values(outputs)) value.dispose();
          tensor.dispose();
        }
      } finally { await session.release(); }
    }
    return {runtime:ort.env.versions.web, browser:navigator.userAgent,
      webnn_available:webnnAvailable, webnn_unavailable_reason:webnnReason,
      webnn_cpu_fallback_allowed:false, experimental_browser_flags:true,
      clock:'raw-model-output-frames-before-provider-cropping', reports};
  }, args['require-webnn']);
  console.log(JSON.stringify({schema:'hwa-basic-pitch-browser-check',schema_version:1,
    model_sha256:modelHash,decoder_wasm_sha256:decoderHash,...result},null,2));
} finally {
  clearTimeout(deadline);
  if (browser) await browser.close();
  server.close();
}

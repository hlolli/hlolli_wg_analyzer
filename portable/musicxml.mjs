// The C reader owns parsing and JSON output. This host grants no file or network access.
export async function createMusicXMLReader(wasm) {
  const module = await WebAssembly.compile(wasm);
  let api;
  const forbidden = name => () => { throw new Error(`MusicXML reader attempted ${name}`); };
  const wasi = {
    environ_sizes_get(count, size) {
      const memory = new DataView(api.memory.buffer);
      memory.setUint32(count, 0, true);
      memory.setUint32(size, 0, true);
      return 0;
    },
    environ_get: () => 0,
    fd_close: forbidden('fd_close'), fd_seek: forbidden('fd_seek'), fd_write: forbidden('fd_write'),
    proc_exit: code => { throw new Error(`MusicXML reader exited with code ${code}`); },
  };
  for (const item of WebAssembly.Module.imports(module)) {
    if (item.module !== 'wasi_snapshot_preview1' || item.kind !== 'function' ||
        !Object.hasOwn(wasi, item.name)) throw new Error(`Unexpected MusicXML import: ${item.module}.${item.name}`);
  }
  api = (await WebAssembly.instantiate(module, {wasi_snapshot_preview1: wasi})).exports;
  api._initialize();
  const decoder = new TextDecoder();
  function failure() {
    const bytes = new Uint8Array(api.memory.buffer);
    const start = api.hwa_browser_score_error();
    return new Error(decoder.decode(bytes.subarray(start, bytes.indexOf(0, start))) || 'MusicXML import failed');
  }
  return bytes => {
    if (!(bytes instanceof Uint8Array)) throw new TypeError('MusicXML input must be Uint8Array');
    if (!bytes.length || bytes.length > 32 * 1024 * 1024) throw new Error('MusicXML input must contain 1 byte to 32 MiB');
    try {
      const pointer = api.hwa_browser_score_input(bytes.length);
      if (!pointer) throw failure();
      new Uint8Array(api.memory.buffer, pointer, bytes.length).set(bytes);
      if (api.hwa_browser_score_read() !== 0) throw failure();
      // Parsing can grow memory; take the output view only after the C call.
      return decoder.decode(new Uint8Array(api.memory.buffer,
        api.hwa_browser_score_output(), api.hwa_browser_score_output_size()));
    } finally { api.hwa_browser_score_clear(); }
  };
}

// Runs a WASI command module with node's WASI and directory preopens:
//   node --experimental-wasm-exnref run_wasi.mjs x.wasm [--preopen /guest=HOSTDIR]... [-- program args...]
import { readFile } from 'node:fs/promises';
import { WASI } from 'node:wasi';
import process from 'node:process';

const argv = process.argv.slice(2);
const wasmPath = argv.shift();
const preopens = {};
while (argv.length && argv[0] === '--preopen') {
  argv.shift();
  const spec = argv.shift() ?? '';
  const eq = spec.indexOf('=');
  if (eq <= 0) { console.error('run_wasi: --preopen needs /guest=HOSTDIR'); process.exit(2); }
  preopens[spec.slice(0, eq)] = spec.slice(eq + 1);
}
if (argv[0] === '--') argv.shift();

const wasi = new WASI({ version: 'preview1', args: [wasmPath, ...argv], env: {}, preopens, returnOnExit: true });
const module = await WebAssembly.compile(await readFile(wasmPath));
const instance = await WebAssembly.instantiate(module, wasi.getImportObject());
process.exitCode = wasi.start(instance);

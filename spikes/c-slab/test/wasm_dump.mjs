// Usage: node test/wasm_dump.mjs build/slab.wasm
// Instantiates the module with no imports, meshes the golden slab through the glue ABI and prints
// the v/i/e lines of expected/scene.txt (f32 as %08x of the bits, lowercase). Exit 0 or 1.
import { readFile } from 'node:fs/promises';

const COS20_BITS = 0x3f708fb2; // cosf(20 deg) as computed by the native (glibc) build
const fail = (msg) => { console.error(msg); process.exit(1); };

const mod = new WebAssembly.Module(await readFile(process.argv[2] ?? 'build/slab.wasm'));
const imports = WebAssembly.Module.imports(mod);
if (imports.length !== 0) fail(`module has imports: ${JSON.stringify(imports)}`);
const e = new WebAssembly.Instance(mod, {}).exports;
if (e.slab_mesh(0, 0, 6000, 4000, 220) !== 1) fail('slab_mesh returned an error');
if (e.slab_mesh(0, 0, 6000, 4000, -1) !== 0) fail('slab_mesh accepted a negative thickness');
if (e.slab_mesh(0, 0, 6000, 4000, 220) !== 1) fail('slab_mesh returned an error');

// cos(20 deg) is hard-coded in main_wasm.c as the f32 the native (glibc) build computes. Check the
// constant there, and that it is the correctly rounded cos of the same f32 argument.
const src = await readFile(new URL('../main_wasm.c', import.meta.url), 'utf8');
if (!src.includes(`out.u = 0x${COS20_BITS.toString(16)}u;`)) fail('main_wasm.c does not hard-code the native cos(20 deg)');
const f32bits = (x) => new Uint32Array(new Float32Array([x]).buffer)[0];
const arg = Math.fround(20 * Math.fround(Math.PI / 180));
if (f32bits(arg) !== 0x3eb2b8c2 || f32bits(Math.cos(arg)) !== COS20_BITS) fail('cos(20 deg) f32 differs from the native value');

const buf = e.memory.buffer;
const hex = (u) => u.toString(16).padStart(8, '0');
const pos = new Uint32Array(buf, e.pos_ptr(), e.pos_len() * 3);
const idx = new Uint32Array(buf, e.idx_ptr(), e.idx_len());
const edges = new Uint32Array(buf, e.edge_ptr(), e.edge_len() * 6);
const out = [];
for (let i = 0; i < pos.length; i += 3) out.push(`v ${[...pos.subarray(i, i + 3)].map(hex).join(' ')}`);
for (let i = 0; i < idx.length; i += 3) out.push(`i ${idx[i]} ${idx[i + 1]} ${idx[i + 2]}`);
for (let i = 0; i < edges.length; i += 6) out.push(`e ${[...edges.subarray(i, i + 6)].map(hex).join(' ')}`);
process.stdout.write(out.join('\n') + '\n');

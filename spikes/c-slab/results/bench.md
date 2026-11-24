# C slab spike: edit → rebuild, edit → pixels

2026-11-24, aarch64, 8 cores. clang version 23.1.3. tcc version 0.9.28rc 2026-01-17 makepkg@b39cbc70 (AArch64 Linux). Chrome/153.0.8010.36 (headless, SwiftShader). 10 runs each.

## edit→rebuild (hyperfine, prepare: bench/touch.sh edits slab.c)

| build | median ms | min ms | peak RSS MB |
|---|---|---|---|
| tcc (native) | 12 | 10 | 3.6 |
| clang -O0 (native) | 97 | 89 | 100.8 |
| clang wasm -O0 (wasm-dev) | 60 | 53 | 99.4 |
| clang wasm -O2 (wasm) | 264 | 239 | 115.3 |

## edit→pixels (SPIKE_K flip, build.sh wasm-dev, slab.reload, first changed frame)

| step | median ms | p95 ms |
|---|---|---|
| build (t1−t0) | 63 | 77 |
| reload→pixels (t2−t1) | 42 | 82 |
| total edit→pixels (t2−t0) | 118 | 145 |

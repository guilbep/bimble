# Where JSON costs time in the C core (ticket J1)

Question (Pierre): "json is not very efficient, you can try exotic things if necessary. Our goal
is open | speed | reliability." Measure first, then replace JSON only where it costs real time.
Synthetic models only (`bench/large_model.py`, 2k / 20k / 60k elements, see `large.md`).

## Method

`ccore/build.sh native && ccore/bench/json_bench.sh` (clang 23 -O2, aarch64, 16 GB dev machine;
median of 5; raw JSON in `out/j1/`, not committed). `bench/json_bench.c` links the native library
and times, in one process:

- **open v1** (the JSON text file): whole `cc_model_load`; `jr_parse` (text → DOM); `validate`;
  `canonical`; DOM → model is the rest. **open v2** (the same model saved by `cc_model_save_v2`):
  whole load; `cc_gunzip`; `cbor_parse` (CBOR → DOM); validate; canonical; DOM → model the rest.
- **save** v1 (`cc_model_save`), v2 split: model → CBOR (`cc_model_to_cbor`, already direct, no
  DOM), gzip.
- **journal**: a session with its session file (`cc_session_open_persisted`) on the v2 copy; per
  change (verb place a column, move one element, delete one) the call + `cc_session_persist_now`
  minus the same call without the session file, and the bytes the entry adds. Restore: reopen with
  1,000 entries (place / move / delete in turn) minus reopen with none. Snapshot: a call that makes
  two history steps forces a snapshot with the model text in it (as after 2,000 entries or 4× the
  snapshot's bytes); its write and its restore.
- **protocol**: `query elements` (default 1,000 rows, and `limit` 10,000), `query element`, the
  change JSON: build ms and bytes in C; `JSON.parse` of the same bytes in Node 26 (V8, the
  engine of the page's Chromium; `bench/json_parse.mjs`).

## Before (commit of this file)

ms, median of 5. 2k = 1,982 elements, 20k = 20,026, 60k = 59,832.

| stage | 2k | 20k | 60k |
|---|---:|---:|---:|
| **open v1** (1.5 / 15.2 / 45.5 MB text) total | 7.1 | 54.9 | 179.9 |
| — text → DOM (`jr_parse`) | 4.3 | 41.4 | 126.0 |
| — DOM → model | 2.7 | 12.3 | 49.7 |
| — validate + canonical | 0.1 | 1.2 | 4.2 |
| **open v2** (83 KB / 880 KB / 2.7 MB file; CBOR 0.5 / 5.3 / 16.0 MB) total | 4.1 | 44.7 | 157.7 |
| — gunzip | 2.1 | 22.3 | 67.6 |
| — CBOR → DOM | 0.9 | 9.0 | 27.3 |
| — DOM → model | 1.0 | 12.1 | 58.7 |
| — validate + canonical | 0.1 | 1.3 | 4.2 |
| **save v1** | 2.2 | 23.6 | 72.1 |
| **save v2**: model → CBOR | 0.7 | 7.2 | 22.0 |
| — gzip | 3.7 | 37.2 | 113.2 |
| journal entry write (place / move / delete) | 0.010 / 0.005 / 0.006 | 0.38 / 0.007 / 0.011 | 0.09 / 0.005 / 0.05 |
| journal entry bytes (place / move / delete) | 549 / 449 / 563 | 549 / 449 / 594 | 549 / 458 / 596 |
| the call itself (place / move / delete) | 0.022 / 0.002 / 0.011 | 0.61 / 0.002 / 0.11 | 2.11 / 0.002 / 0.61 |
| **restore**, no entries (= open v2 + session) | 4.3 | 49.6 | 168.4 |
| **replay of 1,000 entries** | 14.2 | 228.5 | 1,105.1 |
| snapshot with model text: write | 5.7 | 59.6 | 184.1 |
| — its size (MB) | 1.6 | 16.7 | 50.1 |
| — its restore | 8.2 | 94.2 | 306.6 |
| `query elements` 1,000 rows: build / bytes / `JSON.parse` | 0.23 / 161 KB / 0.81 | 0.24 / 162 KB / 0.49 | 0.24 / 162 KB / 0.50 |
| `query elements` 10,000 rows: build / bytes / `JSON.parse` | 0.47 / 318 KB / 1.57 | 2.36 / 1.6 MB / 6.69 | 2.64 / 1.6 MB / 5.16 |
| `query element`, change JSON: build / `JSON.parse` | < 0.01 / < 0.01 | < 0.01 / < 0.01 | < 0.01 / < 0.01 |

## What dominates

1. **The journal replay, and it is not JSON.** 1,000 entries cost 1.1 s at 60k (1.1 ms each),
   228 ms at 20k: it grows with the model, not with the entry. The entries are ~0.5 KB each
   (0.5 MB per 1,000: parsing them is ~2 ms). The time is `add_element` (and `remove`):
   `arr_insert` (src/command.c) allocates a new element array in the model's arena for every
   insert and copies all of it (59,832 × 272 B = 16 MB per place at 60k), and the arena never
   gives the old one back: 333 places in the replay leave ~5 GB of dead arrays. The live call
   pays the same (place: 2.1 ms at 60k). Up to 2,000 entries are replayed at a restart.
2. **The session snapshot with model text is JSON inside JSON.** When the snapshot must hold the
   model (more than one change in a call, then every 2,000 entries) it writes the v1 text
   (`cc_model_save`) as one escaped JSON string (50 MB at 60k), and a restore parses the line,
   unescapes the string, then parses the text again: 306 ms, twice the 168 ms of opening the v2
   file.
3. **Opening v2: gzip, not CBOR.** Of 158 ms at 60k, gunzip is 68 (43 %); of that, ~50 ms is the
   CRC-32 (a byte-at-a-time table: 16 MB in 50 ms, measured alone). CBOR → DOM is 27 ms (17 %),
   DOM → model 59. Saving v2: gzip is 113 of 135 ms.
4. **The protocol is not a cost.** A 1,000-row `query elements` is 0.24 ms to build and 0.5–0.8 ms
   to `JSON.parse`; the change JSON and `query element` are under 10 µs. The 10,000-row cap is
   2.6 + 5–7 ms. Journal entries cost 5–400 µs to write.

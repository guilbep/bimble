# ADR 0071 — The PDF export is Helvetica by default; a generated weight table per feature

- Status: Accepted, 25 Nov 2026
- Decided by: Pierre, 25 Nov 2026: "Default to Helvetica (0 bytes, PDF viewers have it built in)!!
  Roboto as optional!! I don't want [0.4] MB just for a Roboto font"; "we should keep a table of
  the WEIGHT/cost of each feature". Ticket SIZE1 (`docs/research/round-4.md`).
- Amends: ADR 0041 for the C core (Roboto is no longer embedded in PDFs by default, and the
  Roboto files are no longer shipped with the C core's builds). ADR 0041's interface fonts stay.
- Reversal cost: low (one default in `session/sheet_svg.c`, the copies in `build.sh release` and
  `copy-ccore.mjs`).

## Decision

1. **Helvetica is the PDF export's default** (`sheet_pdf`, `view_pdf`): one of the 14 fonts every
   PDF reader has, nothing embedded, laid out with Helvetica's metrics (Rust's Helvetica file
   byte for byte). **Roboto is optional**: `{"font": "roboto"}` when the core has the two files
   (`query pdf_fonts`). No Roboto file ships in the program, the wasm, the release archive or the
   web build. The native host reads them from `$BIMBLE_FONTS`, the repository (dev builds) or
   `<config dir>/bimble/fonts`; a missing file never fails the export.
2. **A weight table, generated**: `ccore/tools/weight.mjs` writes `ccore/results/weight.md`
   (per feature: lines of C, editor and viewer wasm bytes, native bytes, static files; per file of
   the release archive and of the web build). `ci/run.sh unit|full` regenerates it and prints the
   change. A new source file that no feature claims fails the run; growth never fails.
3. **The release link drops unused code** (`--gc-sections`, manifold built in function
   sections): measured as a pure win (`ccore/results/native-size.md`: 3.67 → 2.23 MB, the read
   benchmark unchanged).

## Consequences

- The C core's PDF differs from Rust's default (Rust embeds Roboto): `ccore/golden/DEVIATIONS.md`
  16. The Helvetica output equals Rust's Helvetica golden.
- Budgets: the viewer wasm keeps its 1,000,000-byte limit (ADR 0069, amended); the editor wasm,
  the native binary and the archive have none yet, and the table prints them.

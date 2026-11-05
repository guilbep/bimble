# ADR 0009 — Testing against IfcOpenShell, CI with a software GPU, releases

- Status: Accepted (5 Nov 2026)
- Decided by: Pierre asked for "a release with the binary and a proper GitHub workflow"; setup by Claude
- Reversal cost: low

## Decision
- **IfcOpenShell is the oracle**, used in tests only (Python, not linked):
  - `scripts/check_ifc.py`: our IFC export opens in IfcOpenShell with the right classes, names, storeys,
    property sets, volumes and positions;
  - `scripts/compare_ifcopenshell.py`: our IFC reader vs IfcOpenShell, element by element (same set,
    class, volume within 0.5 %, bounding box within 1 mm).
- **CI** (`ci.yml`): fmt, clippy `-D warnings`, tests on Linux/macOS/Windows. Linux installs Mesa lavapipe so
  renderer tests really draw. The oracle job checks export and reader on the demo model and uploads a render.
- **Releases** (`release.yml`): macOS arm64 + x86_64, Windows x86_64, Linux x86_64 (built on Ubuntu 22.04 for
  an older glibc); smoke test on native targets; archives + `SHA256SUMS`. The tag must equal the
  `Cargo.toml` version. Pull requests get the same archives as artifacts.
- **Three ways to publish** (2 and 3 arrive with PR #3, "release: attach to an existing release"):
  1. `git tag vX.Y.Z && git push origin vX.Y.Z` → workflow creates a pre-release with our notes;
  2. create the release on GitHub (also creates the tag) → workflow attaches files, keeps your notes;
  3. *Actions → release → Run workflow* with `tag: vX.Y.Z` → rebuilds that tag and attaches files.
  v0.1.0 was created on GitHub before (2) existed; its publish step failed and (3) was added to finish it.
- Binaries are **unsigned** for now (macOS quarantine and Windows SmartScreen instructions in the notes).

## Lessons
- **Toolchain drift:** CI uses latest stable; twice a new clippy lint (`chunks_exact_to_as_chunks`) failed CI
  while the local toolchain (1.97) passed. Keep the local toolchain current (`rustup update stable`), or pin
  one in `rust-toolchain.toml` and bump it on purpose. Not pinned yet: an open choice.
- Write a test so that it fails on the old code (done for the review regressions), not only passes on the new.

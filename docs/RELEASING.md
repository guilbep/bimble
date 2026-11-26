# Releasing

Releases of the C core, `bimble-ccore` (`.github/workflows/ccore.yml`, `ccore/README.md`
"Release"). The Rust releases ended with v0.19.0 (ADR 0072); their workflow and this file's old
text are on the tag `v0.19.0` and the branch `oracle/rust`.

## A version

1. Set `ccore/VERSION` (for example `0.1.0-preview.3`) in a pull request to `main`, with the
   release notes in its description. Merge it once `ccore.yml` is green.
2. Tag the merge commit `ccore-v<VERSION>` and push the tag:
   `git pull && git tag ccore-v0.1.0-preview.3 && git push origin ccore-v0.1.0-preview.3`.
   The tag must name `ccore/VERSION`, or the job `ccore-release` stops.
3. `ccore.yml` runs the suites on Linux and macOS, builds and checks the archives (Linux
   x86_64 and aarch64, macOS arm64 and x86_64; the Windows x86_64 zip when its job passes: it may
   still fail without stopping the release) and, when all passed, attaches them and their
   `SHA256SUMS` to a GitHub prerelease `ccore-v<VERSION>`. It never replaces an asset already
   attached (no `--clobber`): to redo a version, delete its assets or the release by hand first.
   It prints the Homebrew formula and keeps it as the artifact `ccore-formula`.
   Each archive holds `THIRD-PARTY.txt` and `licences/` (the code and fonts of others inside, with
   their SPDX identifiers); the Linux program is linked with `-z relro -z now`,
   `_FORTIFY_SOURCE=2` and `-fstack-protector-strong` (`release_test.mjs` checks `BIND_NOW`).
4. Commit that formula to `main` as `Formula/bimble-ccore.rb` (`brew install bimble-ccore`), or
   write it with `ccore/ci/formula.sh <VERSION> SHA256SUMS > Formula/bimble-ccore.rb`.

Never push a tag `v*`: that prefix is the Rust releases'. `Formula/bimble.rb` installs v0.19.0
and stays as it is.

## The archives of a branch before a tag

A manual run of `ccore.yml` on that branch (Actions → ccore → Run workflow, or
`gh workflow run ccore.yml --ref <branch>`) builds and checks the archives and keeps them as
artifacts, without a release. On `main`, a push whose head commit message holds `[ccore-release]`
does the same.

## The web pages

`.github/workflows/pages.yml` publishes the C lab from `main` at the site root on every push to `main`
that changes `ccore/` or `web-ui/`, with the v0.19.0 lab under `/rust/` (its wasm comes from the
release assets: Rust is never compiled). When the C lab fails to build, nothing is deployed and
the site stays as it was. `/c/` and `/egui/` send old links on. A manual run (Actions →
pages → Run workflow) can name another `ccore_ref` or publish a pull request's lab under
`preview/pr-<number>/`; only a run started on `main` deploys (a run on another branch builds and
stops). A preview is served from the production origin and shares its browser storage with the
published labs: start one only for a pull request whose code you have read (ADR 0073). After a failed deployment, start a fresh run; never "Re-run all jobs"
(the header of `pages.yml` says why).

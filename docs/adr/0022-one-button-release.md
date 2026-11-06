# ADR 0022 — Releases from one button: the workflow sets the version

- Status: Accepted (6 Nov 2026)
- Decided by: Pierre ("I forgot to bump Cargo.toml, every time; could the release do it?", chose
  the one-button option); workflow by Claude
- Reversal cost: low (one job in `release.yml`; tagging by hand still works)

## Context / Problem
ADR 0009 makes a release fail when the tag doesn't match `Cargo.toml`, so `bimble --version` is
never wrong. In practice the bump PR gets forgotten before tagging (v0.6.0, and before): a failed
release, a tag to delete, a PR, a new tag.

## Decision
- `release.yml` takes a `version` input on a manual run from `main`. A `prepare` job checks it,
  writes it into `Cargo.toml` and `Cargo.lock` (`cargo update --workspace`, refusing any other
  change), commits "Version X.Y.Z" to `main` as `github-actions[bot]`, and pushes tag `vX.Y.Z`.
  The same run then builds and publishes, because a tag pushed with the workflow's token starts no
  other workflow.
- A tag without a release is treated as a release that didn't happen and moved; an existing release
  is never touched (rebuilding it stays the `tag` input).
- `pages.yml` deploys after a successful release run started by a tag push or by hand (the newest
  release when the event doesn't name it), never after pull requests. It used to accept any branch
  whose name starts with "v", and tried to publish a release called "views".
- Tagging by hand keeps working, with the same check as before.

## Alternatives considered
- **Stamp the tag's version into Cargo.toml only in CI:** right binaries, but `main` would show the
  previous version.
- **A bot opens a bump PR when the tag doesn't match:** still two steps and a re-tag.

## Evidence
- The bump step run on a copy of the repo changes exactly `Cargo.toml` and `Cargo.lock` (8 workspace
  crates, 0.6.0 → 0.7.0).
- The workflow itself runs only on `main`; it is checked on the next release.

## Revisit when
- `main` gets branch protection that blocks the bot's commit (then: a PR, or a ruleset bypass).

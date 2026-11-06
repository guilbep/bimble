# Releasing

## The usual way: one button

*Actions → release → Run workflow*, branch `main`, **version** `X.Y.Z` (no `v`). The workflow:

1. stops if release `vX.Y.Z` already exists;
2. sets `version` in the root `Cargo.toml` and updates `Cargo.lock`, and commits "Version X.Y.Z" to
   `main` (nothing to commit if the version is already there);
3. tags that commit `vX.Y.Z`. A tag of that name without a release (pushed by mistake, e.g. before
   the bump) is moved to this commit;
4. builds, smoke-tests and publishes the release (four archives, the web zip, `SHA256SUMS`, notes
   from `.github/release-notes.md` plus GitHub's list of merged PRs);
5. then the `pages` workflow publishes the web version.

Update `.github/release-notes.md` (what's new, known limits) in a PR before releasing.

## By hand

The tag must match the version in `Cargo.toml`, or the workflow stops (on purpose: otherwise
`bimble --version` would report the old number).

1. Change `version` in the root `Cargo.toml` (`[workspace.package]`), run `cargo build` so
   `Cargo.lock` follows, merge.
2. `git pull && git tag vX.Y.Z && git push origin vX.Y.Z`, or create the release on GitHub with a new
   tag `vX.Y.Z` on `main` (the workflow attaches the files and keeps your notes).

**If a tag was pushed before the bump**, the one-button release with that version fixes it. Or by
hand: `git push origin :refs/tags/vX.Y.Z && git tag -d vX.Y.Z`, bump, tag again.

**To rebuild or re-attach files for an existing tag:** *Actions → release → Run workflow*, branch
`main`, **tag** `vX.Y.Z`.

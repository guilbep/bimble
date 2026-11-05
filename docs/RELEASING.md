# Releasing

The tag must match the version in `Cargo.toml`, or the release workflow stops (on purpose: otherwise
`bimble --version` would report the old number).

1. **Bump the version** in a PR: change `version` in the root `Cargo.toml` (`[workspace.package]`), run
   `cargo build` so `Cargo.lock` follows, merge.
2. **Tag the merged commit**, either way:
   - `git pull && git tag vX.Y.Z && git push origin vX.Y.Z` (the workflow creates the release with
     `.github/release-notes.md` plus GitHub's list of merged PRs), or
   - create the release on GitHub with a new tag `vX.Y.Z` on `main` (the workflow attaches the files and
     keeps your notes).
3. About 2 minutes later the release has four archives and `SHA256SUMS`.

**If a tag was pushed by mistake** (e.g. before the bump) and no release was made, delete it and tag again
after the bump: `git push origin :refs/tags/vX.Y.Z && git tag -d vX.Y.Z`.

**To rebuild or re-attach files for an existing tag:** *Actions → release → Run workflow*, branch `main`,
tag `vX.Y.Z`.

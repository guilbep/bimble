# ADR 0043 — Installers on every system, unsigned for now; a Homebrew tap

- Status: Accepted (10 Nov 2026)
- Decided by: Pierre ("a homebrew tap is enough, we'll pay the 99 if we arrive at the end of this
  project; unsigned msi is good for now; do the pipeline until we have the 99"); tools by Claude
- Reversal cost: low (release workflow only)

## Context / Problem

Releases were archives of one binary. On macOS a downloaded unsigned binary is blocked until
`xattr -d com.apple.quarantine`; on Windows SmartScreen warns of an unknown publisher; on Linux
nothing installs it in a menu. Office users expect an installer and an icon.

## Decision

1. **Installers built in the release workflow, from the same binary as the archives**, with
   cargo-packager (one config in `crates/bimble-cli/Cargo.toml`):
   - macOS: `Bimble.app` in a `.dmg` (drag to Applications), arm64 and x86_64;
   - Windows: `.msi` (WiX);
   - Linux: `.deb`, AppImage, and `.rpm` (cargo-generate-rpm: cargo-packager has none).
   The archives stay, for scripts and the Homebrew formula.
2. **Not signed by a paid certificate yet.** The macOS app is signed ad hoc
   (`signing-identity = "-"`): without any signature the bundle fails `codesign --verify`
   ("code has no resources") and macOS calls a downloaded copy "damaged"; ad hoc, it says
   "unidentified developer" and System Settings offers **Open Anyway**. The `.msi` is unsigned
   (SmartScreen: *More info → Run anyway*).
3. **A Homebrew tap in this repository** (`Formula/bimble.rb`, macOS and Linux):
   `brew tap guilbep/bimble https://github.com/guilbep/bimble && brew install guilbep/bimble/bimble`.
   Homebrew doesn't quarantine what it downloads, so no prompt. The publish job regenerates the
   formula from the release's `SHA256SUMS` (`scripts/homebrew-formula.sh`) and commits it to main.
4. **An icon** (`packaging/icon-*.png`: a frame, two columns and a beam), and a menu entry on
   Linux (`packaging/bimble.desktop`).

## Alternatives considered

- **Apple Developer ID + notarization ($99 a year):** removes the macOS prompt; deferred until
  the project's end (Pierre). Then: the identity in `signing-identity`, notarization credentials
  as CI secrets (cargo-packager notarizes when `APPLE_ID`/`APPLE_PASSWORD`/`APPLE_TEAM_ID` or an
  API key are set).
- **Windows signing:** SignPath Foundation (free for open source, on application) or Azure
  Trusted Signing (~$10 a month). Later.
- **A separate `homebrew-bimble` repository:** the usual layout (`brew install guilbep/bimble/bimble`
  without the URL), but a second repository and a token secret to push to it. The tap here costs
  one URL in the instructions.
- **A Homebrew cask of the `.app`:** casks are quarantined, so the prompt would come back.
- **Flatpak (Flathub), Snap, winget:** later, once the installers are used.

## Revisit when

- The $99 is paid: sign and notarize in CI.
- A Windows signing service accepts the project.
- Someone asks for Linux arm64 or a distribution repository (apt/dnf).

Bimble {{VERSION}}: a preview of a fast, open-source structural BIM tool. It **views** IFC files in 3D, plans, ceiling plans and sections, overlays **another office's DXF or PDF plan** to check it, draws **storey plans at scale**, and **edits and versions** Bimble's own `.bimble` models. It also runs **in a browser**.

User manual: https://guilbep.github.io/bimble-docs/

## Install

| System | Install with | Or the archive |
|---|---|---|
| macOS, Apple Silicon (M1–M4) | `Bimble_*_aarch64.dmg`, or Homebrew (below) | `bimble-*-aarch64-apple-darwin.tar.gz` |
| macOS, Intel | `Bimble_*_x64.dmg`, or Homebrew | `bimble-*-x86_64-apple-darwin.tar.gz` |
| Windows 10/11 | `bimble_*_x64_en-US.msi` | `bimble-*-x86_64-pc-windows-msvc.zip` |
| Debian, Ubuntu | `bimble_*_amd64.deb`: `sudo apt install ./bimble_*_amd64.deb` | `bimble-*-x86_64-unknown-linux-gnu.tar.gz` |
| Fedora, Red Hat, openSUSE | `bimble-*.x86_64.rpm`: `sudo dnf install ./bimble-*.x86_64.rpm` | same |
| Any Linux | `bimble_*_x86_64.AppImage`: `chmod +x`, then run it | same |
| Any browser (static page, to host yourself) | `bimble-*-web.zip` | |

Homebrew (macOS and Linux), no security prompt:

```sh
brew tap guilbep/bimble https://github.com/guilbep/bimble
brew install guilbep/bimble/bimble
```

`SHA256SUMS` lists the checksums. The browser version is also online: https://guilbep.github.io/bimble/ (files you open there stay on your computer).

## Run

Open **Bimble** from Applications, the Start menu or your desktop's menu, or run `bimble` in a terminal (`bimble view your-model.ifc`). You can also drop a file on the window.

The installers aren't signed by a paid certificate yet:

- **macOS:** the first time, macOS says Bimble is from an unidentified developer. Open **System Settings → Privacy & Security** and click **Open Anyway** (or right-click Bimble → **Open**). Homebrew installs don't ask.
- **Windows:** SmartScreen may warn about an unknown publisher; choose *More info → Run anyway*.

## Try

- Open an IFC, then **File → Convert to .bimble (editable)…**: every element comes along; columns, beams, walls, slabs and footings are editable by their dimensions, the rest can be moved and versioned.
- The left panel lists the model's **views** (3D, plans, ceiling plans, sections) with their view range; select, edit and measure in any of them. **Project → Reference plan…** lays a DXF or vector PDF over a plan.
- **Sheets**: place views at their scales, dimension and annotate, export PDF or SVG. `bimble plan model.ifc plan.pdf --storey "Level 1" --scale 100` from the command line.
- **File → Share for viewing…** writes a small `.bimbleview` that opens in seconds, also in a browser.
- **History** (Manage tab, or `bimble history …`): commits, branches, merges; `bimble history clone / sync / borrow` to work together through a shared folder.
- `bimble bench --json > bench.json`: how fast it is on your machine. Please send the file, especially from old laptops.

## Known limits

- `.bimble` files saved by 0.12 and later can't be opened by 0.11 and older: upgrade together, or share `.json`.
- Models with elements hidden or isolated in a view, or with a reference plan added since 0.14, can't be opened by 0.13 and older: everyone working on a model upgrades together.
- Elements of other classes than columns, beams, walls, slabs and footings are moved, not edited by dimensions.
- Working together (Collaborate → Shared folder, Synchronize): the desktop only; a conflict is settled for all elements at once (from the command line with `--prefer`), not element by element yet. Rights (view / annotate / model) aren't enforced yet.
- Reference plans: DXF model space and the first page of a vector PDF; no text, hatches or scanned PDFs.
- In the browser: no model history yet; loading uses one processor core; at most 4 GB per tab.
- Curved geometry is drawn as fine facets (0.5 mm tolerance), not exact surfaces.
- On a machine whose graphics refuse a drawing step, Bimble turns that step off and says so in red: please report the message with your machine and system.

## Changes

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

- `bimble view model.ifc`: the viewer. The left panel lists the model's **views** (3D, plans, ceiling plans looking up, sections) with their **view range** (top, cut plane, bottom, view depth); select, edit and measure in any of them. In a plan, **Add reference plan…** lays a DXF or vector PDF over the model (DWG: convert it to DXF first); move it into place and measure on it. Also: section box (B), measuring (M), view cube, linked models, **Plan…** with a live preview, **Style…** for every colour and hatch.
- `bimble plan model.ifc plan.pdf --storey "Level 1" --scale 100`: a storey plan at scale on A4–A0 (PDF, SVG or PNG); `--template` for your office's title block and words.
- `bimble demo frame.bimble`, then edit it in the viewer (Edit…, History…) or with `bimble history …`.
- `bimble bench --json > bench.json`: how fast it is on your machine. Please send the file, especially from old laptops.

## Known limits

- Editing works on `.bimble` models, not on IFC files (IFC is read for viewing and plans). An IFC's views are saved next to it in `model.ifc.bimble-views.json`.
- Layouts: sheets with several views aren't there yet; the paper plan (**Plan…**) is a storey plan cut at one height.
- Reference plans: DXF model space and the first page of a vector PDF; no text, hatches or scanned PDFs.
- In the browser: no model history yet, and an IFC's views file can't be read back; loading uses one processor core.
- Plans: hidden lines are removed for flat tops only; no dimensioned openings, labels or section markers yet (see the issues).
- Curved geometry is drawn as fine facets (0.5 mm tolerance), not exact surfaces.
- Performance on old laptops still has to be measured. Please report what you see (model size, machine, how it feels).

## Changes

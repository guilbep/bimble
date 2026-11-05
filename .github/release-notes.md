Bimble {{VERSION}}: a preview of a fast, open-source structural BIM tool. It **views** IFC files and Bimble's own `.bimble` models; editing comes next.

## Download

| System | File |
|---|---|
| macOS, Apple Silicon (M1–M4) | `bimble-*-aarch64-apple-darwin.tar.gz` |
| macOS, Intel | `bimble-*-x86_64-apple-darwin.tar.gz` |
| Windows 10/11 | `bimble-*-x86_64-pc-windows-msvc.zip` |
| Linux x86_64 | `bimble-*-x86_64-unknown-linux-gnu.tar.gz` |

`SHA256SUMS` lists the checksums.

## Run

Unpack, then run `bimble` to open the viewer, or `bimble view your-model.ifc`. You can also drop a file on the window.

- **macOS:** the binary isn't signed yet. If macOS blocks it, run `xattr -d com.apple.quarantine bimble` in the unpacked folder once.
- **Windows:** SmartScreen may warn about an unknown publisher; choose *More info → Run anyway*.

## Known limits

- Openings are not cut out yet (walls and slabs show without their holes).
- Boolean clippings are drawn without the cut.
- No editing in the viewer yet.
- Performance on old laptops still has to be measured. Please report what you see (model size, machine, how it feels).

## Changes

# ADR 0088 — Bimble on macOS: the native app as an unsigned disk image

- Status: Accepted, 28 Nov 2026
- Decided by: Pierre on 28 Nov 2026: macOS `.dmg` first, then Windows; unsigned to start, with signing and
  notarisation addable later without rework; his MacBook (Apple Silicon) is the human test, Intel is checked on CI
  only; "performance is everything". The choices below were delegated to Claude (ticket MACAPP), each with its
  alternatives and the numbers measured on GitHub's runners.
- Concerns: ADR 0084 (a native app beside the web lab), ADR 0069 (release archives), ticket MAC1 (the core on macOS).
- Reversal cost: low. Packaging and build scripts only. Code shared with Linux is touched only under `__APPLE__`, and
  only the shader *format* changes per device.

## Context

`bimble` (ADR 0084) is SDL3's GPU API plus Dear ImGui and the C core, and runs on Linux on Vulkan. SDL's Metal backend
takes MSL (or a metallib), not SPIR-V. The release's macOS rows already build `bimble-ccore` for arm64 and x86_64
(macOS 11+, signed ad hoc). The dev machine is aarch64 Linux, so the loop runs on GitHub's macOS runners.

## Decision

1. **MSL committed beside the SPIR-V.** `native/macos/shadercross.sh` builds SDL_shadercross (SDL's own SPIR-V→MSL
   tool, on SPIRV-Cross) at pinned commits, in about 40 s on Linux or a Mac, with nothing installed system-wide.
   `native/build.sh shaders --msl` translates the *committed* SPIR-V into `shaders/gen/<name>.msl.h`:
   - line 1 is the source signature (the same as the SPIR-V header's);
   - line 2 is the tool version and the SPIR-V header's sha1;
   - the rest is a C string.
   `shaders --check` verifies both sets (signatures, plus bytes when the tool is built). Neither the macOS build nor
   the release needs a shader tool, the same decision as for glslc (NUI3).
   - All 20 shaders translate unchanged, and Xcode 16.4's `metal` compiles all 20 for macOS 11 / Metal 2.0.
   - shadercross remaps the bindings to SDL's MSL order: uniforms, then storage buffers; in compute, read-only before
     read-write; sampled textures before storage textures. It also renames the entry point `main0`.
   - The renderer takes MSL when `SDL_GetGPUShaderFormats` lists it (`render_int.h` `nv_shader_pick`). The MSL is
     compiled into the macOS build only.
   - Alternatives: SPIRV-Cross's own CLI (it does not do SDL's binding remap by itself); translating at build time on
     the runner (a C++ tool in every macOS build); a metallib built by `xcrun metal` (needs Xcode's Metal toolchain on
     every build machine, and `-std` per macOS version). The MSL source is compiled by Metal at device creation, and the
     OS caches it.
2. **Dear ImGui's SDL_GPU backend on its MSL path.** The backend prefers its metallib, which needs macOS 14, whenever
   the device lists METALLIB, and Metal always does. `native/macos/imgui_msl.h`, force-included in that one file on
   macOS, makes the flag read as 0 there. ImGui's bytes and checksums are unchanged. This keeps
   **the deployment target at macOS 11.0**: the oldest for Apple Silicon, the core's and the release's, and above
   SDL's Metal GPU minimum (10.14 with `MTLGPUFamilyMac2`).
3. **Static third parties; one executable, no dylib.** `native/macos/deps.sh` builds SDL 3.4.18 (the Linux pin),
   brotli 1.2.1 and FreeType 2.14.3 statically, per architecture, at pinned commits. FreeType reads the lab's WOFF2
   fonts through brotli, as on Linux, so the text is the same. The bundle therefore has no `@rpath` and nothing to
   re-sign inside it, and `otool -L` names only `/usr/lib` and `/System`. The alternative, a bundled `libSDL3.dylib`
   with `@rpath`, adds a second signed object and a load path for the same bytes. CoreText instead of FreeType is
   possible (ImGui's FreeType is optional), but it would render the text differently.
4. **macOS's places, shared with `bimble-ccore`:**
   - settings: `~/Library/Application Support/Bimble`, the host's `cc_host_config_dir`;
   - link caches: still beside the IFC file by default, as on Linux. Only the platform's fallback folder (a read-only
     project folder, or when asked) moves from `~/.cache/bimble` to `~/Library/Caches/bimble`, for both products
     (`BIMBLE_CACHE_DIR` and `XDG_CACHE_HOME` still first);
   - session logs: `~/Library/Logs/bimble/sessions` (`--last-log`).
   Memory comes from `mach_task_basic_info`; freed memory is returned with `malloc_zone_pressure_relief`.
   `/proc/self/exe` becomes `_NSGetExecutablePath`, and the load average comes from `getloadavg`.
5. **Crash reports**: the signal handler as on Linux. The log names the executable's LC_UUID and load address.
   `native/build.sh app` keeps `out/app/debug/<UUID>.dSYM` (the last 20 builds), and `native/symbolize.sh` runs
   `atos` on it. The release attaches `bimble-<v>-macos-<arch>.dSYM.zip`.
6. **Input**: Cmd acts as Ctrl. This is the lab's own rule (`keys.ts` `rowOf`: ctrl is Ctrl or Meta), so no platform
   column is exported; the key table shows "Cmd". Cmd-click adds to the selection. Cmd+Q, the menu's Quit and closing
   the window go through `SDL_EVENT_QUIT`. Finder's open-with and `open -a` arrive as `SDL_EVENT_DROP_FILE`, and a
   `-psn_…` argument is ignored. Retina sizes come from the swapchain texture (the Linux fix). Pinch and two-finger
   pan work as on Linux.
7. **Offscreen on Metal**: SDL's Metal backend needs a video driver that has Metal views, and SDL's offscreen driver has
   none (measured: "No supported SDL_GPU backend found!"). `--offscreen` on macOS therefore uses Cocoa, with a hidden
   window that is never claimed: no swapchain, no Dock icon (`SDL_HINT_MAC_BACKGROUND_APP`), pixel density 1. The 3D view
   and the UI draw into the app's own textures, as on Linux. So `--offscreen --preset orbit-zoom` runs from Terminal on
   any Mac with the same `summary.md`.
8. **The bundle and the image** (`native/macos/bundle.sh`):
   - `bimble.app` has `ch.bimble.bimble`, the version from `ccore/VERSION` (`CFBundleShortVersionString` gets its
     three numbers, `BimbleVersion` the full string), the document types and UTIs (`.bimble` and `.bimbleview`
     exported, `org.buildingsmart.ifc` imported), `NSHighResolutionCapable`, `LSMinimumSystemVersion` 11.0, an
     `.icns` made from `packaging/icon.png`, and the licences.
   - It is signed **ad hoc** (`codesign -s -`, which Apple Silicon needs to run anything at all).
   - The `.dmg` per architecture holds the app, an `/Applications` link and a README with the first launch's steps.
   - **Image format and two images versus one universal: see "Measured".**
9. **Signing and notarisation later, without rework.** `bundle.sh` signs with `SIGN_IDENTITY` (Developer ID, hardened
   runtime, `entitlements.plist`, empty on purpose, and a secure timestamp) and, with `NOTARIZE=1`, submits with
   `notarytool` and staples. The release job has the steps, skipped while these secrets are missing:
   - `MACOS_CERT_P12`, `MACOS_CERT_PASSWORD` and `MACOS_SIGN_IDENTITY`;
   - `NOTARY_KEY_P8`, `NOTARY_KEY_ID` and `NOTARY_ISSUER` (an App Store Connect API key).
   Pierre's Apple Developer Program membership is the only missing piece.
10. **First launch, unsigned.** On macOS 15 (Sequoia) and newer, Control-click → Open no longer overrides Gatekeeper.
    The user tries to open the app once, then goes to System Settings → Privacy & Security → **Open Anyway** (there for
    about an hour after the attempt), enters the login password, then Open. macOS keeps the exception. Sources:
    Apple, "Updates to runtime protection in macOS Sequoia" (developer.apple.com/news/?id=saqachfa, 6 Aug 2024);
    "Safely open apps on your Mac" (support.apple.com/en-us/102445); "Open a Mac app from an unknown developer"
    (support.apple.com/guide/mac-help/mh40616). On macOS 11–14, right-click → Open still works. On any version,
    `xattr -dr com.apple.quarantine /Applications/bimble.app` once. The image's README and the release notes say this.

## Measured

GitHub runners, 28 Nov 2026 (`.github/workflows/macapp.yml`, runs 46150628076, 46151176806 and 46151797259; the journal
`2026-11-28-macapp.md` has the order).

- **Metal on the runners** (`MTLCopyAllDevices`):
  - `macos-15`, arm64 (macOS 15.8.2, Xcode 16.4): "Apple Paravirtual device", Mac2 family, read-write texture tier 2,
    3.5 GB maximum buffer. It is the same under Rosetta.
  - `macos-15-intel` (i7-8700B): a paravirtual device without Mac2 and with tier 1, which SDL refuses ("Device does not
    meet the hardware requirements for SDL_GPU Metal").
  The Intel build is therefore checked for build and image there, and run under Rosetta 2 on Apple Silicon. The
  release's `macos-x86_64` row does the same (it runs on `macos-15`, which is arm64).
- **Offscreen needs Cocoa.** SDL's offscreen video driver offers no Metal view, so the device is refused ("No supported
  SDL_GPU backend found!"). Offscreen on macOS is therefore Cocoa with a hidden, unclaimed window (no Dock icon,
  density 1).
- **The suites on Metal**, arm64 and Rosetta:
  - `native_app_test` 81/0 and `native_geo_test` 7/0;
  - the truth test reports 0 ids missing (orbit-zoom 2,847 frames; tiles fixture 278; tiles links 167);
  - against the Linux references: the fixture differs by 0.12 % of pixels (limit 6 %), the far building by 0.184 %
    (limit 0.2 %), from edge rasterisation on another GPU;
  - Metal API and GPU validation report no error;
  - every input is answered within 200 ms (worst event → GPU fence 88 ms).
  Frame times on a VM with load average 17–21 are only indicative: on arm64, orbit-zoom moving frames are 5.3 / 8.0 ms
  median / p95, input → frame 7.4 / 11.4 ms, and the first picture 1.6 s.
- **Sizes** (0.1.0-preview.8):

  | | arm64 | x86_64 |
  |---|---|---|
  | executable (stripped, static) | 6.03 MB | 6.84 MB |
  | .dmg UDZO (zlib) | 3.25 MB | 3.70 MB |
  | .dmg ULFO (lzfse) | 3.09 MB | 3.45 MB |
  | **.dmg ULMO (lzma, chosen)** | **2.40 MB** | **2.89 MB** |
  | .dSYM.zip | 0.95 MB | 0.98 MB |

  A universal executable (`lipo`) is 12.9 MB, and its image 6.39 MB in ULFO, against 3.09 MB for arm64 alone.
  **Two images**: a universal image would make every download about twice as large for nothing, and the architecture is
  one word in the file name. ULMO needs macOS 10.15 or newer, below our minimum of 11.

## Consequences

- A shader change needs `shadercross.sh` once (about 40 s) besides glslc; `shaders --check` catches a forgotten MSL.
- The Intel build is checked on CI only, and the arm64 build is tested by a person on Pierre's MacBook.
- Not yet on macOS (shown as such in the app where it applies):
  - "Open in the web lab": `bimble.app` holds no `bimble-ccore`;
  - a native menu bar (ImGui's own menu stays);
  - the trackpad's smart zoom (SDL has no event for it);
  - Developer ID and notarisation.

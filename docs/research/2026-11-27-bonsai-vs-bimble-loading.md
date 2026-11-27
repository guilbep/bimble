# Bonsai vs Bimble: loading the owner's IFC files

_27 Nov 2026. MEASUREMENT ticket BONSAI (Opus agent). Asked by Pierre: "download/install bonsai
(latest version and see how it behave for loading ifc compare to our bimble?)". Facts measured on
this machine today unless marked "read" (code or docs read, not run) or "inference"._

## Summary

**Correction of method (read first).** A first series (10:21-11:25) capped each run's *address
space* (`RLIMIT_AS`, like `ulimit -v`) at MemAvailable − 1.5 GB, which moved between 5.3 and 7.3 GB.
Address space is not RAM: Blender, Python and the OpenCASCADE mesher reserve much more than they
touch. Its six failures (Bonsai load #12, #13; Bonsai link #14, #15; IfcOpenShell #15, #16)
happened at only 4.5-5.7 GB of resident memory, each against a different cap. **They are not
walls and are withdrawn.** The method was changed to a fixed cap on real memory, the same for
every tool: a cgroup scope with `MemoryMax=8G`, no swap, under the shared lock, started only with
`free -g` ≥ 9 GB available. Under that rule, between 11:31 and 12:57, only file #10 got through
all four tools (and Bimble on #1-#10): the machine had less than 9 GB available most of the time
(five other builders, the owner's desktop), so the re-runs of #11-#17 could not start and the
coordinator closed the ticket. **#12 and above were not measured under a comparable cap: the
wall of Bonsai and IfcOpenShell on this machine is not established.** Numbers from the
address-space series are marked † and are **indicative only**.

What stands:

1. **Bonsai ran on this machine**, but not from an official build: there is no Blender for
   linux-aarch64 from blender.org, conda-forge or PyPI, and no Bonsai for linux-arm64. Fedora 45's
   `blender-5.3.0` (aarch64) in a rootless Fedora rootfs, plus Bonsai 0.9.0.post1 (latest stable)
   with its compiled wheels swapped for aarch64 builds, ran in background Blender. No GPU is
   involved in that mode. Section 1.
2. **#10 (159 MB), all under the 8 GB cgroup cap, same minutes:** Bonsai "Load Project" 15.9 s,
   2.9 GB peak RSS, 9,791 Blender objects; Bonsai "Link IFC" first time 81 s (host Blender 0.7 GB +
   converting Blender 3.5 GB), again from its cache 0.27 s; IfcOpenShell alone (parse + geometry)
   5.5 s, 1.8 GB; **Bimble's C port 1.47 s, 0.67 GB.** The address-space series gave the same #10
   numbers within 15% (13.8 s, 75 s, 5.0 s), as expected for runs that did not reach their cap.
3. **† Indicative only (address-space series):** successful runs, which did not reach their cap: Bonsai load up to #11 (200 MB: 59 s, 2.9 GB, 30,679 objects); Bonsai
   link up to #13 (394 MB: 124 s, host 1.1 GB + converter 4.7 GB; re-open from cache 2.3 s);
   IfcOpenShell up to #14 (415 MB: 18 s, 4.0 GB). Bimble (build of 26 Nov) converted all 17: 50.8 s
   in total, at most 10.6 s and 2.8 GB (#15). These are marked † in section 2.
4. **Bonsai's own stop, independent of memory:** "Load Project" with defaults creates no object
   for a file with more than 30,000 `IfcElement` and asks for advanced settings
   (`operator.py:1173`): #12 stops after 5.8 s and 2.2 GB (†), and so would #14, #16 and #17 by
   their element counts (32,766 / 48,794 / 35,243 elements drawn by Bimble). Pushed on, #12 had
   reached 5.6 GB RSS after 6 minutes when the address-space cap stopped it (withdrawn as a wall;
   it says only "more than 5.6 GB").
5. **Fidelity**: across the 17 files Bimble draws every product with a body except 4 doors that
   only have a 2D `FootPrint` (IfcOpenShell draws them as curves). Element counts per class equal
   IfcOpenShell's in the files compared, but for `IfcSpace`, which Bonsai loads and Bimble does
   not draw. Bimble's meshes have 0-83% more triangles than IfcOpenShell's on the same classes
   (section 4). No NURBS or advanced B-rep element was left out in this corpus.
6. The Bimble binary was replaced by another ticket at 11:41 (cache slimmed: gzipped property
   sets, budgeted proxies). The cgroup rows use the new build (sha256 `133a30aec06f5127…`), the †
   rows the 26 Nov build: compare caches only within one build.

## 1. What was installed, and how

Machine: Apple M1 Pro, Asahi Linux (Arch), aarch64, **16 KiB pages**, 16 GB RAM, 8 cores. No sudo.
Everything is under `~/.cache/bimble-bonsai/` (on disk).

### 1.1 Blender for linux-aarch64: the attempts, in the order of the ticket

| Attempt | Result |
|---|---|
| blender.org official builds | `download.blender.org/release/Blender5.3/` and `Blender5.2/` list `linux-x64`, `macos-arm64`, `windows-x64`, `windows-arm64` only. The daily builder (`builder.blender.org/download/daily/`) lists `linux.x86_64` builds only (4.2 to 5.4 alpha). `www.blender.org/download/` answered `curl` with a Cloudflare challenge page ("Just a moment..."): not read. **No linux-arm64 build.** |
| conda-forge | `api.anaconda.org/package/conda-forge/blender`: "blender could not be found"; same for `bpy`. A search for "blender" finds only unrelated packages (`blender-mathutils` for linux-64, osx-64, win-64). **No.** |
| `pip install bpy` | PyPI `bpy` 5.3.0: wheels `cp313` for `macosx_13_0_arm64`, `manylinux_2_28_x86_64`, `win_amd64`, `win_arm64`. **No linux aarch64 wheel.** |
| Arch Linux ARM package | No `blender` in `core`, `extra`, `alarm` of the Arch Linux ARM mirror (directory listing) nor in this machine's repos (`pacman -Ss blender`: nothing). **No package to extract.** |
| **Fedora 45 aarch64 package (worked)** | Fedora builds Blender for aarch64: `blender-5.3.0-3.fc45.aarch64` (built 2026-11-14), the latest Blender release. Installed with `dnf` inside a rootless Fedora 45 rootfs (below). Fedora supports 16 KiB and 64 KiB page kernels on aarch64, so the 16 KiB pages of Asahi were not a problem. `blender -b` runs: Blender 5.3.0, Python 3.15.2 (Fedora's system Python; the official Blender builds bundle their own Python). |

The rootfs, without sudo and without installing anything system-wide:

```sh
B=https://dl.fedoraproject.org/pub/fedora/linux/releases/45/Container/aarch64/images
curl -LO $B/Fedora-Container-Base-Generic-45-1.4.aarch64.oci.tar.xz      # 68,993,700 bytes
curl -LO $B/Fedora-Container-45-1.4-aarch64-CHECKSUM
sha256sum Fedora-Container-Base-Generic-45-1.4.aarch64.oci.tar.xz
#   bc6c3b3e132e3ed5eff398112d16fcdca13dc8abd63550f8aad40e882b9617b6  = the CHECKSUM file
#   (the CHECKSUM file's own GPG signature was not checked)
tar -xJf Fedora-Container-Base-*.oci.tar.xz -C oci     # OCI layout: one layer
unshare --map-auto --map-root-user tar -xf oci/blobs/sha256/76862c60... -C rootfs
~/.cache/bimble-bonsai/bin/fedora-run.sh dnf -y --setopt=install_weak_deps=False install blender python3-pip
```

`fedora-run.sh` enters the rootfs with `unshare --map-auto --map-root-user --mount --pid` (the
user's subuid range, so `rpm` can set file owners), binds `/dev`, `/proc`, `/sys`, the work folder
as `/work`, the scripts folder as `/scripts`, and `chroot`s. `dnf` checks Fedora's package
signatures (Fedora's default `gpgcheck`). 294 packages, rootfs 2.4 GB on disk.

### 1.2 Bonsai

- Latest stable: **Bonsai 0.9.0post1** (GitHub release `bonsai-0.9.0post1`, 2026-10-15). Latest
  unstable: daily `bonsai-0.9.1-alpha2611262254` (2026-11-26). Bonsai is not on
  extensions.blender.org for this check; the releases are on GitHub (`IfcOpenShell/IfcOpenShell`).
- Platforms of both: `bonsai_py311-…` and `bonsai_py313-…` for `linux-x64`, `macos-arm64`,
  `windows-x64`. **No linux-arm64 Bonsai.** (The separate standalone "Bonsai Viewer"
  `bonsaiviewer-0.9.0post1` has a `linuxarm64` zip; it is not Bonsai and was not measured.)
- `bonsai_py313-0.9.0-post1-linux-x64.zip`, 169,933,736 bytes, sha256
  `1e1fc411fc1f8814f1118a0620db0c348bf91bab25cbdbd6cee419eca8032643` (= GitHub's digest). Its
  manifest: `blender_version_min = "5.1.0"`, `platforms = ["linux-x64"]`, 91 wheels: 71
  pure-Python (Bonsai itself, ifcpatch, ifctester, …) and 20 compiled for x86_64.
- **The port to aarch64 (my change, not an official build):** the 71 pure-Python wheels kept as
  they are; the 20 compiled ones replaced by the PyPI builds for CPython 3.15 / abi3 on
  manylinux aarch64, same versions (`pip download --only-binary=:all:` inside the rootfs), with
  these exceptions:
  - `ifcopenshell-0.9.0.post1-py315-none-manylinux_2_31_aarch64.whl` (sha256 `ebce7061e721800d…`):
    same IfcOpenShell release as the x64 bundle, built for Python 3.15 aarch64.
  - `cachebox` 5.2.3 has no cp315 build: 6.2.11 used.
  - `ezdxf` 1.4.4 and `frozenlist` 1.8.0: pure-Python `py3-none-any` wheels (no C speed-ups).
  - `pyradiance` 1.3.0 (Radiance lighting) has **no aarch64 wheel**. Bonsai's `light` module
    imports it at registration, so the add-on failed to enable ("No module named 'pyradiance'").
    An empty stub module was installed: Radiance renders do not work; loading does not use it.
  - Manifest `platforms` set to `["linux-arm64"]`, wheel list rewritten. Zip sha256
    `f178cbb4937edf9ea0509b61a3ec84e1034de20bf481106af6cfcb34e6aa64cb`.
  - Installed with `blender --command extension install-file -r user_default -e bonsai-arm64.zip`:
    "Installed bonsai", it registers, and `bpy.ops.bim.load_project` and `bpy.ops.bim.link_ifc`
    run (file #1 below).
- IfcOpenShell for the engine-only runs: 0.9.0.post1 is the latest on PyPI (wheels for Python
  3.10 to 3.15, also `manylinux_2_31_aarch64`), the same version as the existing venv
  `~/.cache/bimble-ifc-corpus/venv` (Python 3.14): no new venv was needed.

So **Bonsai itself ran here**: Bonsai's own Python code (0.9.0.post1), with the same IfcOpenShell
release, in Fedora's Blender 5.3.0 under Python 3.15, with aarch64 builds of its compiled
dependencies. The limits of that: section 6.

## 2. Results

### 2.0 Under the fixed cap (cgroup `MemoryMax=8G`, no swap): the valid series

Runs 11:31-12:15, under the shared lock, each started with at least 9 GiB available. "RSS" = peak
resident set of the largest process (wait4); "cg" = the scope's `memory.peak` (all processes of
the run together, page cache of the files read included). No run was OOM-killed.

| # | MB | tool | time s | RSS / cg MB | output |
|---|---|---|---|---|---|
| #10 | 159.1 | IfcOpenShell parse + geometry | 2.23 + 3.23 | 1754 / 1932 | 8,071 shapes, 1,482,044 triangles |
| #10 | 159.1 | Bonsai load | 15.9 | 2945 / 2818 | 9,791 objects, 1,971,064 triangles, `.blend` 25.9 MB |
| #10 | 159.1 | Bonsai link, 1st | 81.1 | host 699 + converter 3466 / 3651 | cache 25.1 MB `.blend` + 18.6 MB `.sqlite` |
| #10 | 159.1 | Bonsai link, 2nd (from cache) | 0.27 | 698 / 1238 | |
| #10 | 159.1 | Bimble `link-convert` (11:41 build) | 1.47 | 674 / 535 | 8,059 elements, 1,561,801 triangles, cache 10.3 MB |

Bimble, 11:41 build, same 8 GB cgroup cap (#1-#9 at 11:55: files under 100 MB, run without the lock, one at a time, while only about 3 GB was available; their peak was at most 0.53 GB, so the cap was never approached; #10 at 11:43 under the full rule):

| # | MB | wall s | RSS / cg MB | cache MB | of which ELEM / properties (PRPZ, gzipped) / HLOD MB | elements | triangles |
|---|---|---|---|---|---|---|---|
| #1 | 0.2 | 0.05 | 12 / 17 | 0.0 | 0.0 / 0.0 / 0.0 | 17 | 524 |
| #2 | 1.6 | 0.10 | 12 / 20 | 0.1 | 0.1 / 0.0 / 0.0 | 511 | 6132 |
| #3 | 18.7 | 0.25 | 87 / 96 | 1.1 | 0.7 / 0.3 / 0.1 | 4528 | 76392 |
| #4 | 25.3 | 0.35 | 126 / 147 | 3.2 | 2.9 / 0.1 / 0.2 | 783 | 1102656 |
| #5 | 43.7 | 0.71 | 171 / 187 | 3.3 | 2.6 / 0.5 / 0.1 | 6510 | 327060 |
| #6 | 53.5 | 0.56 | 359 / 372 | 4.5 | 4.3 / 0.0 / 0.2 | 12 | 465748 |
| #7 | 59.4 | 0.86 | 201 / 216 | 4.3 | 3.7 / 0.4 / 0.2 | 5965 | 627224 |
| #8 | 60.0 | 0.86 | 517 / 531 | 7.9 | 7.5 / 0.0 / 0.4 | 46 | 888593 |
| #9 | 71.3 | 0.81 | 285 / 299 | 2.8 | 2.6 / 0.0 / 0.2 | 719 | 1370970 |
| #10 | 159.1 | 1.47 | 674 / 535 | 10.3 | 9.3 / 0.4 / 0.5 | 8059 | 1561801 |

The re-runs of #11-#17 (queued in this order: Bonsai load #12, Bonsai link #14, IfcOpenShell #15,
then Bimble #11-#17) waited from 12:18 to 12:57 for `free -g` to show 9 GB available; it showed
2-8 GB, so none started. Mistake on my side: that wait was done while holding the shared lock (up
to 10 minutes at a time), which held back other agents' short runs; the scripts now wait for
memory outside the lock and take it with `flock -n` for the run only. `scripts/bonsai_queue.sh` runs them when the
machine is free.

### The first series (address-space cap): † rows

Runs 10:21-11:25. Each run's cap was on address space (`RLIMIT_AS`) at MemAvailable − 1.5 GB, read
at its start (5.3-7.3 GB). Successful rows (marked † elsewhere) did not reach their cap; they are
**indicative only**, and every **failure is withdrawn** (it shows where the address space ran out,
not where RAM did). Cap per run: the address-space value stated in each failed row; 5.3-7.3 GB for
the others. Sizes in MB
= 10^6 bytes.

### 2.1 † IfcOpenShell 0.9.0.post1 alone (engine only, not Bonsai), indicative only

| # | MB | parse s | parse RSS MB | geometry s | peak RSS MB | shapes | triangles |
|---|---|---|---|---|---|---|---|
| #1 | 0.2 | 0.00 | 75 | 0.02 | 84 | 18 | 454 |
| #2 | 1.6 | 0.04 | 83 | 0.04 | 93 | 512 | 6238 |
| #3 | 18.7 | 0.35 | 186 | 0.24 | 188 | 4529 | 62074 |
| #4 | 25.3 | 0.31 | 174 | 0.58 | 385 | 783 | 1099056 |
| #5 | 43.7 | 0.75 | 317 | 4.54 | 488 | 6510 | 256289 |
| #6 | 53.5 | 0.74 | 349 | 1.36 | 822 | 12 | 465801 |
| #7 | 59.4 | 1.05 | 406 | 14.2 | 658 | 8874 | 596769 |
| #8 | 60.0 | 0.59 | 287 | 1.86 | 1224 | 46 | 888849 |
| #9 | 71.3 | 0.95 | 454 | 1.27 | 924 | 720 | 813198 |
| #10 | 159.1 | 2.16 | 901 | 2.88 | 1768 | 8071 | 1482044 |
| #11 | 200.7 | 3.90 | 1181 | 2.01 | 1349 | 29935 | 8553994 |
| #12 | 335.8 | 5.34 | 1890 | 26.1 | 3321 | 33204 | 4793488 |
| #13 | 394.1 | 5.64 | 1882 | 6.90 | 3433 | 21192 | 3588616 |
| #14 | 415.1 | 6.82 | 2225 | 11.2 | 4022 | 32766 | 35165796 |
| #15 | 709.0 | 9.74 | 3088 | withdrawn: geometry stopped by the address-space cap (5.34 GB) at 4738 MB RSS, "cannot allocate memory for thread-local data" | | | |
| #16 | 758.4 | 12.21 | 4099 | withdrawn: same, cap 6.30 GB at 5707 MB RSS | | | |
| #17 | 872.5 | not run | | | | | |

"shapes" includes spatial elements with a body (spaces). The geometry time is short next to the
Bonsai load because nothing is built from the meshes here.

### 2.2 † Bonsai 0.9.0.post1 in background Blender 5.3.0 (the port of section 1.2), indicative only

"Load" = `bim.load_project` (+ `bim.load_project_elements` when Bonsai stops at 30,000 elements).
"Link 1st" = cache creation (host Blender + the converting Blender it starts); "link 2nd" = the
same command again, from the cache. Cache = `.ifc.cache.blend` + `.ifc.cache.sqlite` (+ a JSON of
under 1 kB). Every Blender starts at about 570 MB RSS with Bonsai enabled.

| # | MB | load s | load RSS MB | objects | triangles | .blend MB | link 1st s | link 1st RSS MB (host + converter) | cache MB (blend + sqlite) | link 2nd s | link 2nd RSS MB |
|---|---|---|---|---|---|---|---|---|---|---|---|
| #1 | 0.2 | 0.34 | 606 | 68 | 658 | 0.1 | 3.0 | 573 + 607 | 0.1 + 0.2 | 0.02 | 574 |
| #2 | 1.6 | 0.50 | 633 | 599 | 6850 | 0.2 | 3.3 | 574 + 624 | 0.2 + 1.3 | 0.02 | 574 |
| #3 | 18.7 | 3.0 | 940 | 5556 | 75190 | 1.5 | 6.2 | 579 + 739 | 1.3 + 15.1 | 0.03 | 580 |
| #4 | 25.3 | 2.2 | 1126 | 2447 | 1224495 | 9.1 | 14.2 | 614 + 1290 | 5.9 + 4.3 | 0.13 | 612 |
| #5 | 43.7 | 10.8 | 1576 | 7629 | 314551 | 8.2 | 28.1 | 597 + 1504 | 5.0 + 23.3 | 0.07 | 598 |
| #6 | 53.5 | 3.4 | 1652 | 68 | 931602 | 14.1 | 23.7 | 618 + 1538 | 9.2 + 0.2 | 0.09 | 618 |
| #7 | 59.4 | 40.6 | 1994 | 13097 | 726352 | 14.1 | 56.1 | 621 + 1476 | 7.5 + 33.3 | 0.12 | 622 |
| #8 | 60.0 | 5.8 | 2431 | 1304 | 1777058 | 30.7 | 34.1 | 649 + 2626 | 19.0 + 0.5 | 0.11 | 648 |
| #9 | 71.3 | 3.7 | 1853 | 795 | 879334 | 15.2 | 22.9 | 625 + 1610 | 6.3 + 1.1 | 0.12 | 625 |
| #10 | 159.1 | 13.8 | 2948 | 9791 | 1971064 | 25.9 | 74.7 | 692 + 3528 | 24.8 + 18.6 | 0.26 | 692 |
| #11 | 200.7 | 58.7 | 2915 | 30679 | 8796807 | 13.4 | 94.2 | 813 + 2478 | 31.8 + 104.9 | 2.94 | 1053 |
| #12 | 335.8 | default: **stops** after 5.8 s, 2178 MB, 0 objects (33,422 `IfcElement` > 30,000). Forced: withdrawn, stopped by the address-space cap (7.24 GB) after 364 s at 5576 MB RSS ("Malloc returns null") | | | | | 141.1 | 857 + 5105 | 39.1 + 101.7 | 0.91 | 856 |
| #13 | 394.1 | withdrawn: stopped by the address-space cap (6.40 GB) after 41 s at 5208 MB RSS (MemoryError, "Malloc returns null") | | | | | 123.7 | 1053 + 4730 | 54.1 + 129.9 | 2.34 | 1053 |
| #14 | 415.1 | not run | | | | | withdrawn: the converting Blender was stopped by the address-space cap (5.70 GB) after 399 s at 4518 MB RSS; `.cache.sqlite` (129 MB) written, no `.cache.blend`; Bonsai then raised "failed to open blend file" (it does not notice that its converter crashed) | | | | |
| #15 | 709.0 | not run | | | | | withdrawn: same, cap 6.78 GB, at 5196 MB RSS after 250 s | | | | |
| #16, #17 | 758, 872 | not run | | | | | not run | | | | |

Objects and triangles count every Blender object Bonsai made (also spatial elements, type
objects, empties), triangles at each object. Load times are Bonsai's operator time; each run's
process wall time adds about 2.5-3 s of Blender start and exit.

### 2.3 † Bimble's C port, `bimble-ccore link-convert` (native, 26 Nov build, before the 11:41 replacement), indicative only

Wall time and RSS of the whole process (read + convert + write). The cache is one `.bimbleview`;
the split by section is from the file itself.

| # | MB | wall s | RSS MB | cache MB | of which geometry (ELEM) / properties (PROP) / LOD proxies (HLOD) MB | elements drawn | triangles |
|---|---|---|---|---|---|---|---|
| #1 | 0.2 | 0.05 | 12 | 0.1 | 0.0 / 0.1 / 0.0 | 17 | 524 |
| #2 | 1.6 | 0.05 | 12 | 1.0 | 0.1 / 0.9 / 0.0 | 511 | 6132 |
| #3 | 18.7 | 0.20 | 87 | 12.7 | 0.7 / 11.4 / 0.5 | 4528 | 76392 |
| #4 | 25.3 | 0.35 | 131 | 9.8 | 2.9 / 3.6 / 3.2 | 783 | 1102656 |
| #5 | 43.7 | 0.70 | 168 | 22.7 | 2.6 / 16.2 / 3.8 | 6510 | 327060 |
| #6 | 53.5 | 0.55 | 350 | 8.3 | 4.3 / 0.0 / 4.0 | 12 | 465748 |
| #7 | 59.4 | 0.86 | 197 | 27.3 | 3.7 / 13.8 / 9.9 | 5965 | 627224 |
| #8 | 60.0 | 1.01 | 527 | 12.4 | 7.5 / 0.2 / 4.7 | 46 | 888593 |
| #9 | 71.3 | 0.95 | 290 | 19.0 | 2.6 / 0.8 / 15.6 | 719 | 1370970 |
| #10 | 159.1 | 1.61 | 671 | 39.1 | 9.3 / 11.5 / 18.3 | 8059 | 1561801 |
| #11 | 200.7 | 2.11 | 716 | 112.8 | 6.3 / 82.9 / 23.6 | 29935 | 8838551 |
| #12 | 335.8 | 4.08 | 1065 | 144.1 | 21.0 / 63.1 / 59.9 | 30295 | 5430874 |
| #13 | 394.1 | 3.67 | 1376 | 161.7 | 19.1 / 116.4 / 26.3 | 21192 | 3738159 |
| #14 | 415.1 | 6.93 | 1745 | 182.3 | 19.3 / 105.9 / 57.2 | 32766 | 35798718 |
| #15 | 709.0 | 10.6 | 2802 | 441.1 | 72.2 / 141.8 / 227.2 | 26814 | 20144585 |
| #16 | 758.4 | 9.09 | 2264 | 316.9 | 30.3 / 177.1 / 109.5 | 48794 | 20987851 |
| #17 | 872.5 | 7.97 | 2814 | 204.3 | 29.6 / 119.2 / 55.5 | 35243 | 9094476 |

Total: 50.8 s, 1.72 GB of cache (geometry ELEM 0.26 GB, properties PROP 0.87 GB, LOD proxies
HLOD 0.59 GB; the other sections are small). The geometry-only figure is the ELEM column.

### 2.4 The pairs that matter

| # | MB | Bonsai open | Bonsai link 1st / 2nd | IfcOpenShell parse+geometry | Bimble convert |
|---|---|---|---|---|---|
| #7 † | 59.4 | 40.6 s, 2.0 GB | 56 s, 0.6+1.5 GB / 0.12 s | 15.3 s, 0.66 GB | 0.86 s, 0.20 GB |
| **#10 (8 GB cgroup)** | 159.1 | 15.9 s, 2.9 GB | 81 s, 0.7+3.5 GB / 0.27 s | 5.5 s, 1.8 GB | 1.47 s, 0.67 GB |
| #11 † | 200.7 | 58.7 s, 2.9 GB | 94 s, 0.8+2.5 GB / 2.9 s | 5.9 s, 1.3 GB | 2.11 s, 0.72 GB |
| #12 † | 335.8 | stops (0 objects, 5.8 s, 2.2 GB); forced: > 5.6 GB, not finished | 141 s, 0.9+5.1 GB / 0.9 s | 31 s, 3.3 GB | 4.08 s, 1.07 GB |
| #13 † | 394.1 | forced: > 5.2 GB, not finished | 124 s, 1.1+4.7 GB / 2.3 s | 13 s, 3.4 GB | 3.67 s, 1.38 GB |
| #14 † | 415.1 | (stops by default) | converter > 4.5 GB, not finished | 18 s, 4.0 GB | 6.93 s, 1.75 GB |
| #15 † | 709.0 | not tried | converter > 5.2 GB, not finished | parse 9.7 s, 3.1 GB; geometry > 4.7 GB, not finished | 10.6 s, 2.80 GB |
| #17 † | 872.5 | (stops by default) | not tried | not tried | 7.97 s, 2.81 GB |

## 3. The wall, and what it implies

- **Not established under the fixed 8 GB cap.** The only lower bounds are the withdrawn runs
  (resident memory reached before the address-space cap stopped them): Bonsai load #12 > 5.6 GB
  (after 6 minutes, not finished), #13 > 5.2 GB; Bonsai link converter #14 > 4.5 GB, #15 > 5.2 GB
  (plus about 1 GB for the host Blender alive at the same time); IfcOpenShell geometry #15 > 4.7 GB,
  #16 > 5.7 GB. Whether these finish under 8 GB of real memory is open; `bonsai_queue.sh` measures
  it when the machine has 9 GB free.
- **Measured, valid:** Bonsai opens files up to #11 (200 MB) in about a minute and 3 GB, and links
  files up to #13 (394 MB) with two Blenders of up to 1.1 + 5.1 GB at once; IfcOpenShell meshes
  files up to #14 (415 MB) in 18 s and 4 GB; Bimble converts all 17 (largest 872 MB) in at most
  10.6 s and 2.8 GB.
- **Bonsai's own limit comes first for opening:** above 30,000 `IfcElement` it stops on purpose
  (#12, and by element count #14, #16, #17). That is earlier than the docs' "over 750MB" warning.
- Implication for "all 17 as links" in Bonsai on 16 GB: #1-#13 can be linked one by one (625 s of
  one-time conversion in total, 639 MB of caches, 7 s to re-open them all one at a time). #14-#17
  (four files, 2.75 GB of the 3.9 GB) need a converting Blender above 4.5-5.2 GB with a host
  Blender beside it; on this machine, with the desktop and other work using 7-13 GB, there was
  never room to try them under a fair cap. Whether the 13 caches open together in one Blender was
  not tested.
- Memory incidents: none on the desktop. Each first-series failure was its own address-space cap
  firing (four Blender crashes, two IfcOpenShell aborts). The shared lock was respected; other
  agents' runs (one CI run held the lock about 20 minutes) and low free memory made my cgroup runs
  wait.

## 4. Fidelity: what each draws

IfcOpenShell (Bonsai's settings, section 5) against Bimble's log and its `.bimbleview` (elements
and triangles per class read back with `scripts/bimbleview_class_tris.py`; its totals equal the
log's). Four mid-size files; only the classes that differ are listed. The IfcOpenShell side comes
from the † series (successful runs; element and triangle counts do not depend on the cap), the
Bimble side from both builds (the 11:41 build draws the same elements and triangles on #1-#10).

Element counts:

- All 17 files: Bimble draws every "product with a body" except 4 `IfcDoor` (2 in #7, 2 in #12),
  listed in its log as "drawn empty". Checked in #7: those doors have only a `FootPrint`
  representation (a mapped `IfcGeometricSet`, 2D), no body; IfcOpenShell draws them as curves from
  the FootPrint context. Nothing is lost to NURBS or advanced B-rep in this corpus.
- `IfcSpace`: drawn by IfcOpenShell and loaded by Bonsai (#7: 2,907 spaces, 107,100 triangles;
  #9: 1; #10: 12; #12: 2,907); not drawn by Bimble's link reader.
- Every other class: same element count in both.

Triangles per class (counted at each element; differences above 10% shown):

| # | class | elements | IfcOpenShell triangles | Bimble triangles | Bimble / IfcOpenShell |
|---|---|---|---|---|---|
| #5 | IfcBeam | 3514 | 74,604 | 97,646 | 1.31 |
| #5 | IfcSlab | 508 | 56,388 | 78,732 | 1.40 |
| #5 | IfcColumn | 1122 | 41,954 | 60,396 | 1.44 |
| #5 | IfcWallStandardCase | 930 | 18,466 | 25,349 | 1.37 |
| #7 | IfcDoor | 64 / 66 | 128,200 | 215,336 | 1.68 |
| #7 | IfcSlab | 2574 | 68,152 | 88,961 | 1.31 |
| #7 | IfcWallStandardCase | 1155 | 42,030 | 57,553 | 1.37 |
| #7 | IfcColumn | 1042 | 59,564 | 74,780 | 1.26 |
| #9 | IfcBuildingElementProxy | 643 | 754,550 | 1,312,442 | 1.74 |
| #10 | IfcPlate | 1177 | 146,988 | 174,795 | 1.19 |
| #10 | IfcFlowFitting | 433 | 23,064 | 42,116 | 1.83 |
| #10 | IfcSlab | 493 | 17,002 | 28,262 | 1.66 |

Totals: #5 256,289 vs 327,060; #7 596,769 vs 627,224 (IfcOpenShell's includes the spaces); #9
813,198 vs 1,370,970; #10 1,482,044 vs 1,561,801. Window, wall (`IfcWall`), furnishing, proxy and
flow-terminal classes are within 2%. Inference, not checked face by face: the extra triangles are
in planar solids with openings (slabs, walls, columns, beams), where the hybrid CGAL kernel merges
coplanar faces and Bimble keeps the pieces, and in curved items (fittings, proxies), where the
tessellation tolerances differ. The counts say nothing about which is closer to the true shape.

Bimble's reader also logs what it simplified: #5 "IfcSlab: 4 opening(s) not cut (geometry not a
closed solid)" and "a clipping plane removed the whole solid; drawn unclipped (×3)"; #7 three
slabs with 10, 25 and 60 openings not cut; #12 slabs and walls with openings not cut and
"polygonal half-space: boundary not closed; cut by its plane alone (×2)". #9, #10, #11, #14: no
warning. IfcOpenShell made every shape it was asked for (no element with a representation left
undrawn); Bonsai's importer meshes elements with over 30 openings without cuts, and so did this
measurement: 11 elements in #5, 1 in #7, none in #9 and #10 (Bimble cut those it could, see its
warnings above).

## 5. Method

Files: the owner's 17 IFC (`<folder>`, 3.9 GB) copied to `~/.cache/bimble-bonsai/work/files/` as
`f01.ifc` … `f17.ifc` by ascending size (13 IFC2X3, 4 IFC4), so no private name reaches a log,
cache or this page. The originals were only read.

One driver, `scripts/bonsai_series.sh <first> <last>`, per file in ascending order:

```sh
W=~/.cache/bimble-bonsai/work; S=<checkout>/scripts
# every run: start only with MemAvailable >= 9 GiB; the shared lock for the run only; free -g logged;
# its own cgroup scope capped at 8 GB of real memory, no swap; 20 minutes
# (wait for MemAvailable >= 9 GiB outside the lock, then)
flock -n ~/.cache/bimble-measure.lock \
  systemd-run --user --scope -p MemoryMax=8G -p MemorySwapMax=0 -p OOMPolicy=continue \
  python3 -I $S/measure_run.py --timeout-s 1200 --out res/<run>.m.json -- <command>
# a. + b. IfcOpenShell (engine only)
~/.cache/bimble-ifc-corpus/venv/bin/python -I $S/ios_geom_bench.py files/fNN.ifc --json res/ios_fNN.json
# Bimble's C port (native build of 26 Nov)
~/bimble-test/bimble-ccore link-convert --cache-dir $W/bimble-cache files/fNN.ifc
# c. Bonsai load (background Blender in the Fedora rootfs)
fedora-run.sh blender -b --python /scripts/bonsai_load_bench.py -- /work/files/fNN.ifc \
   --mode load --force-large --save /work/bonsai-run/fNN.blend --json /work/res/bload_fNN.json
# d. Bonsai link, twice (first: cache creation; second: from the cache), then the caches deleted
fedora-run.sh blender -b --python /scripts/bonsai_load_bench.py -- /work/files/fNN.ifc --mode link ...
```

- The cap: **the same fixed cgroup cap of 8 GB (`MemoryMax=8G`, `MemorySwapMax=0`) for every run of
  every tool** (IfcOpenShell, Bimble, Bonsai load, Bonsai link with its converting Blender, which
  is in the same scope). A run killed by the cgroup (`oom_kill` counted in the scope's
  `memory.events`) is the result "needs more than 8 GB". Each run started only under the shared
  lock and with at least 9 GB available.
- `measure_run.py` stands in for `/usr/bin/time -v`, which this host does not have: wall time and
  peak RSS from `wait4()` rusage (the source GNU time uses: the largest single process of the
  tree, not a sum), plus the scope's `memory.peak` (all processes of the run together, with the
  page cache of files they read and wrote charged to the scope; shared library pages already
  charged elsewhere are not in it, so it can be below the RSS of a small run). It kills the
  process group after 20 minutes.
- A tool's series stops after two failures in a row (the wall, then the next file to confirm). A
  wait for the lock or for memory is not counted as a failure. The full cgroup series was started
  at 11:31; after 11 minutes it had done #1-#3 (each run waiting for the lock or for 9 GB free), so
  it was stopped and restarted at #10, then replaced at 12:18 by a queue of the runs that decide
  the wall (`bonsai_queue.sh`). Bimble #1-#9 were run under the same cap without the lock (files
  under 100 MB, at most 0.53 GB).
- IfcOpenShell settings (`ios_geom_bench.py`), copied from Bonsai's importer: kernel
  `hybrid-cgal-simple-opencascade` (the default of `geometry_library`, `bim/module/project/prop.py`;
  not `opencascade` as in the 26 Nov note), `multiprocessing.cpu_count()` = 8 threads
  (`should_use_cpu_multiprocessing` default True, `bim/import_ifc.py:710`), one iterator per
  prioritised representation context, linear deflection 0.05, angular 0.5, surfaces and
  solids and curves, no default materials, `layerset-first`, `no-wire-intersection-check`, local
  coordinates plus placement (so mapped items share one geometry), elements with over 30
  openings without the cuts (`tool/loader.py:717`). Element set as Bonsai's
  `process_element_filter`: `IfcElement` (+ `IfcProxy`) minus `IfcFeatureElement`, then the spatial
  elements (Bonsai loads spaces too). Annotations and type objects are left out (Bonsai creates
  them). Triangles = `len(faces)/3` per shape, counted at each occurrence.
- Bonsai load: `bpy.ops.bim.load_project(filepath=…)` with defaults. For a file over 30,000
  `IfcElement`, Bonsai stops there; the script then calls `bpy.ops.bim.load_project_elements()`
  with the default advanced settings (what the button does), and the time given is the sum.
  Objects counted include spatial elements, type objects and empties; triangles are counted per
  mesh object (an instanced mesh at each use). The `.blend` is saved, its size noted, then deleted.
- Bonsai link: `bpy.ops.bim.link_ifc(filepath=…)` with defaults (`use_cache=True`, default query =
  `IfcElement` + `IfcProxy` + spatial elements − features). The first run's peak RSS is given as
  "self + child": the host Blender and the background Blender it waits for are alive at the same
  time. Every Blender here starts at about 570 MB RSS with Bonsai enabled, before any file.
- Bimble: `bimble-ccore link-convert --cache-dir … <file>` (native C port, binary of 26 Nov
  2026): read + convert + write the `.bimbleview`; elements and triangles from its `.log.txt`; the
  cache split by section with `scripts/bimbleview_sections.py` (ELEM = geometry and element
  records; PROP = property sets; HLOD = baked level-of-detail proxies).

## 6. Limits of these numbers

- Bonsai here is a port (section 1.2): Fedora's Blender build, Python 3.15 instead of the bundled
  3.13, aarch64 wheels, a stubbed `pyradiance`, `cachebox` 6 for 5. The Python code and the
  IfcOpenShell release are Bonsai's own. The Mac run (section 8) has none of these caveats.
- Background mode: no viewport, no drawing, **no GPU**. Checked on a running `blender -b`: the GL
  and Vulkan loader libraries are linked in (`libGL`, `libvulkan`), but no Mesa driver
  (`asahi_dri.so`) is loaded and no `/dev/dri` device is open. A user with a window also pays the
  viewport and the GPU upload (Bonsai's warning about lag above 50,000 elements is about that).
- The IfcOpenShell columns are the engine only: no Blender objects. They are a floor under
  Bonsai, never "Bonsai".
- Each number is one run (no repeat, no warm-up control). Files were read from a warm page cache
  for every tool alike (the copy had just been read). Other agents ran on the machine; the lock
  kept heavy runs apart.
- Triangle counts differ by design: IfcOpenShell meshes curved faces with the 0.05 deflection set
  by Bonsai, Bimble with its own tessellation; Bonsai's object triangles also include type
  objects.
- An earlier series the same morning (10:21-11:25) capped address space (`RLIMIT_AS`, like
  `ulimit -v`) at MemAvailable − 1.5 GB, which moved between 5.3 and 7.3 GB from run to run.
  Address space is not RAM: Blender, Python and the OpenCASCADE mesher reserve much more than
  they touch, so its failures (#12, #13 Bonsai load; #14, #15 Bonsai link; #15, #16 IfcOpenShell)
  came early, at 4.5-5.7 GB RSS, and not against one limit. **All its numbers are replaced** by
  the cgroup series of section 2 (its raw files are kept apart in
  `~/.cache/bimble-bonsai/work/res-rlimit-as/`).

## 7. Mission 1, line by line

What Bonsai 0.9.0.post1 offers, from what ran here or from its code (paths in the Bonsai wheel,
`bonsai/…`) and docs read today. Only the first two lines were run.

| Mission 1 line | Bonsai |
|---|---|
| Fast loading of IFC files | Ran: section 2. "Load Project" stops before creating objects above 30,000 `IfcElement` and asks for the advanced settings (`bim/module/project/operator.py:1173`). Docs: "Filesizes over 750MB, which may cause memory issues", "Number of elements exceeding 50,000 … unreasonable loading times, selection glitches, and viewport lagging", and "it's much more efficient to link a model rather than to open a model" (https://docs.bonsaibim.org/studio/guides/viewing/dealing_with_large_models.html). |
| Several files on top of each other | Ran (one file per link): "Link IFC" takes several files at once (`LinkIfc.files`, `operator.py:1384`); each new link starts a second background Blender that writes `.ifc.cache.blend`, `.cache.sqlite`, `.cache.json` beside the IFC (`operator.py:1577-1663`); linked models are read-only. |
| Section box and cut planes, automatic floor plans | Read: clip boxes (`bim/module/clip_box`, `bim.add_clip_box`), clipping planes (`bim.create_clipping_plane`, `operator.py:3014`). Floor plans: one drawing at a time, `bim.add_drawing` with target view PLAN_VIEW and a storey picked as "location hint" (`bim/module/drawing/data.py:125`), then `bim.create_drawing` writes an SVG; loaded links are drawn by opening their IFC again (`drawing/operator.py:1029`). No "all storeys at once" operator found. Not run. |
| Show or hide any element | Read: Blender's own hide/isolate for opened projects; for links, query an element with the Explore tool, then `bim.hide_queried_linked_element` (hide it, or all but it; "doesn't work with UNDO", `operator.py:2672`). Not run. |
| Measure, read the properties, add notes | Read: `bim.measure_tool` and `bim.measure_face_area_tool` (`operator.py:3209`, `3305`); properties from the IFC when opened, from `.cache.sqlite` when linked (written with ifcpatch `ExtractPropertiesToSQLite`, `operator.py:2333`); notes as BCF topics (`bim/module/bcf`). Not run. |
| Export views to PDF | Read: drawings and sheets are SVG; PDF only through a user-set external command `svg2pdf_command`, e.g. Inkscape (`bim/ui.py:412`, `drawing/operator.py:2013`). Not run. |
| Export views to DXF | Read: `bim.convert_svg_to_dxf` reads the drawing's SVG polylines with a regular expression and writes DXF R2010 with ezdxf (`tool/drawing.py:2948`). Not run. |

## 8. The same test on the MacBook Pro (macOS arm64)

There Bonsai has an official build, so this removes the port caveats of section 6.

1. Blender 5.3.0: `https://download.blender.org/release/Blender5.3/blender-5.3.0-macos-arm64.dmg`,
   drag to Applications.
2. Bonsai 0.9.0post1 for Python 3.13 (Blender 5.x):
   `gh release download bonsai-0.9.0post1 -R IfcOpenShell/IfcOpenShell -p 'bonsai_py313-0.9.0-post1-macos-arm64.zip'`
   (140,915,735 bytes, sha256 `5ee3033932f5baed6a251bb0577f1cd7e3fad0c73c1f4f1b34989815407e5667`).
3. Install it from the command line:
   ```sh
   B=/Applications/Blender.app/Contents/MacOS/Blender
   $B --command extension install-file -r user_default -e ~/Downloads/bonsai_py313-0.9.0-post1-macos-arm64.zip
   ```
4. Copy the IFC files to a scratch folder (the link writes `<file>.ifc.cache.blend`,
   `.cache.sqlite`, `.cache.json` beside the IFC), and get the scripts from this checkout:
   `scripts/bonsai_load_bench.py`, `scripts/ios_geom_bench.py`.
5. Run, one file at a time, smallest first (macOS `time -l` prints "maximum resident set size" in
   bytes; the script prints its own peak RSS in MB, macOS units handled):
   ```sh
   cd ~/bonsai-test            # holds the copies f01.ifc ... f17.ifc
   for f in f*.ifc; do
     /usr/bin/time -l $B -b --python bonsai_load_bench.py -- $f --mode load --force-large \
        --save /tmp/x.blend --json $f.load.json 2>&1 | grep -E "BONSAI_BENCH|maximum resident|real"
     /usr/bin/time -l $B -b --python bonsai_load_bench.py -- $f --mode link --json $f.link1.json 2>&1 | grep -E "BONSAI_BENCH|maximum resident|real"
     /usr/bin/time -l $B -b --python bonsai_load_bench.py -- $f --mode link --json $f.link2.json 2>&1 | grep -E "BONSAI_BENCH|maximum resident|real"
     rm -f /tmp/x.blend $f.cache.*
   done
   ```
   macOS has no `ulimit -v` that works and compresses then swaps instead of failing: watch
   Activity Monitor's memory pressure and stop the loop at the first file that turns it red.
   `--force-large` makes the load go on past Bonsai's own 30,000-element stop (section 2), as a
   user clicking "Load Project Elements" would.
6. Same with Bimble on the Mac for the pair: `bimble-ccore link-convert --cache-dir <dir> <file.ifc>`.
7. To look at it by hand: `$B` (opens the window), then File > Open IFC Project, or the Links
   panel > Link IFC.

## 9. Files, what was kept, how to open Bonsai here

Scripts (in this checkout, not committed): `scripts/bonsai_load_bench.py` (Bonsai, in Blender),
`scripts/ios_geom_bench.py` (IfcOpenShell alone), `scripts/measure_run.py` (limits + wait4 rusage),
`scripts/bonsai_series.sh` (the series), `scripts/bonsai_queue.sh` (chosen runs, same cap and
rules: `bonsai_queue.sh bload:12 blink:14 ios:15 bim:11 …`), `scripts/bonsai_series_table.py` and
`scripts/bonsai_cgroup_rows.py` (the tables),
`scripts/bimbleview_sections.py` and `scripts/bimbleview_class_tris.py` (read a `.bimbleview`).
`bonsai_load_bench.py` was run here in both modes on all files up to the walls; on macOS it is
untested (only its RSS unit differs, handled).

Deleted at the end: the 17 copies, every Bonsai cache (`.cache.blend/.sqlite/.json`) and saved
`.blend`, Bimble's caches and logs of the copies, the run logs (they quote element names),
Bonsai's process log (23 MB, element data), recent-files list and `.blend` thumbnails inside the
rootfs. The rootfs `/tmp` is a tmpfs private to each run's mount namespace (Blender's crash files
and Bonsai's temporary link scripts lived there and went with it). Kept: the per-run JSON numbers
(`~/.cache/bimble-bonsai/work/res/` for the cgroup runs, `res-rlimit-as/` for the † series;
anonymous counts only).

The install works and stays: `~/.cache/bimble-bonsai/rootfs` (Fedora 45 + Blender 5.3.0 + Bonsai,
2.4 GB), `~/.cache/bimble-bonsai/dl` (downloads, 0.46 GB), `work/bonsai-arm64.zip` (the ported
add-on, 0.15 GB). Background use, as measured:

```sh
~/.cache/bimble-bonsai/bin/fedora-run.sh blender -b --python /scripts/bonsai_load_bench.py -- /work/<copy>.ifc --mode link
```

A visible window (written for the owner, **not tested**: no window was allowed during this
ticket; it passes the Wayland and X11 sockets and `/dev/dri` into the rootfs; Mesa's `asahi`
driver is in the rootfs; your home is `/host-home` inside):

```sh
~/.cache/bimble-bonsai/bin/blender-gui.sh
```

To remove it all: `unshare --map-auto --map-root-user rm -rf ~/.cache/bimble-bonsai` (the rootfs
has files owned by mapped users, so a plain `rm` fails on some of them).

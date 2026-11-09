# A weak GPU, simulated

`bimble bench` on Mesa's llvmpipe (a Vulkan driver that draws on the CPU) in a Linux container,
capped to a few cores: a stand-in for an old integrated GPU when none is at hand (ADR 0036). Not
the real thing: a real GPU pays less for MSAA, and llvmpipe shares the CPU with the rest of the
frame, so run it on an otherwise idle machine and compare runs made the same way.

```sh
docker build -t bimble-slowgpu scripts/slowgpu
# Build Bimble for Linux once (the target folder is kept in a volume).
docker run --rm -v "$PWD":/src -v bimble-target:/target -v cargo-reg:/usr/local/cargo/registry \
  bimble-slowgpu cargo build --release -p bimble-cli
# Bench, 2 cores, MSAA off; models from a folder mounted read-only.
docker run --rm --cpus=2 -v /path/to/models:/bim:ro -v bimble-target:/target bimble-slowgpu \
  /target/release/bimble bench --quick --frames 10 --samples 1 /bim/model.ifc
```

Options to compare (one at a time): `--samples 1`, `--width 960 --height 540`, `--occlusion`,
`--proxies 3`, `--gpu-occlusion`, `--gpu-cull`.

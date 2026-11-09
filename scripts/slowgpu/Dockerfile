FROM rust:1-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
    mesa-vulkan-drivers libvulkan1 vulkan-tools pkg-config cmake clang \
    libxkbcommon-dev libwayland-dev libx11-dev libxcursor-dev libxrandr-dev libxi-dev \
    libgl1-mesa-dev libgtk-3-dev && rm -rf /var/lib/apt/lists/*
ENV WGPU_BACKEND=vulkan CARGO_TARGET_DIR=/target
WORKDIR /src

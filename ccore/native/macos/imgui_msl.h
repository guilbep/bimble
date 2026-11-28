/* MACAPP (ADR 0088): included first (-include) in Dear ImGui's SDL_GPU backend on macOS only (native/deps.sh). The
 * backend takes its precompiled metallib when the device lists SDL_GPU_SHADERFORMAT_METALLIB (Metal always does), and
 * that metallib needs macOS 14 (imgui_impl_sdlgpu3.cpp, 2025-11-26); its MSL source path runs on 10.14 and newer.
 * The flag seen as 0 in that one file makes the backend take the MSL path, so the app runs on macOS 11 (the core's and
 * the release's oldest), not only 14. The bytes of ImGui stay those of native/deps.sh's checksums. */
#ifndef BIMBLE_IMGUI_MSL_H
#define BIMBLE_IMGUI_MSL_H
#include <SDL3/SDL_gpu.h>
#undef SDL_GPU_SHADERFORMAT_METALLIB
#define SDL_GPU_SHADERFORMAT_METALLIB 0u
#endif

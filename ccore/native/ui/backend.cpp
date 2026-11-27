// Prototype I (spike NAT2): the only C++ of the native shell. Dear ImGui's SDL3 platform backend
// and SDL_GPU renderer backend behind five C functions (backend.h); everything else of the shell is
// C on cimgui (built by tcc). Compiled into libbimble_imgui.so by deps.sh.
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include "backend.h"

extern "C" int nui_backend_init(SDL_Window *win, SDL_GPUDevice *dev, SDL_GPUTextureFormat target_format) {
    if (!ImGui_ImplSDL3_InitForSDLGPU(win)) return -1;
    ImGui_ImplSDLGPU3_InitInfo ii = {};
    ii.Device = dev;
    ii.ColorTargetFormat = target_format;
    ii.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    return ImGui_ImplSDLGPU3_Init(&ii) ? 0 : -1;
}

extern "C" int nui_backend_event(const SDL_Event *ev) { return ImGui_ImplSDL3_ProcessEvent(ev) ? 1 : 0; }

extern "C" void nui_backend_new_frame(void) {
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
}

// ImGui::Render, then the UI drawn into `target` (its content kept: LOAD) in the caller's command
// buffer, after whatever the caller recorded before (the 3D frame).
extern "C" void nui_backend_render(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, int clear, const float rgba[4]) {
    ImGui::Render();
    ImDrawData *dd = ImGui::GetDrawData();
    ImGui_ImplSDLGPU3_PrepareDrawData(dd, cmd);
    SDL_GPUColorTargetInfo ct = {};
    ct.texture = target;
    ct.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    if (rgba) ct.clear_color = SDL_FColor{rgba[0], rgba[1], rgba[2], rgba[3]};
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    ImGui_ImplSDLGPU3_RenderDrawData(dd, cmd, rp);
    SDL_EndGPURenderPass(rp);
}

extern "C" void nui_backend_shutdown(void) {
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
}

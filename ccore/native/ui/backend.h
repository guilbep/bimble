/* Prototype I (spike NAT2): Dear ImGui's SDL3 + SDL_GPU backends as C functions (backend.cpp). */
#ifndef BIMBLE_NATIVE_UI_BACKEND_H
#define BIMBLE_NATIVE_UI_BACKEND_H
#include <SDL3/SDL.h>
#ifdef __cplusplus
extern "C" {
#endif
int nui_backend_init(SDL_Window *win, SDL_GPUDevice *dev, SDL_GPUTextureFormat target_format);
int nui_backend_event(const SDL_Event *ev);
void nui_backend_new_frame(void);
void nui_backend_render(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, int clear, const float rgba[4]);
void nui_backend_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif

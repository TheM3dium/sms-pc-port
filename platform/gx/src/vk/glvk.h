// glvk: the OpenGL 3.3 subset sms_gx draws with, implemented on Vulkan.
//
// sms_gx fetches every GL entry point through one get-proc function
// (gl_funcs.h, gl::load). With SMS_GX_BACKEND=vulkan the platform layer hands
// it glvk::getProc instead of SDL's or EGL's, so the renderer itself (the GX
// state, EFB copies, peeks, pixel metrics, texture packs, post-processing) is
// the same code on both backends, and draws the same pixels.
//
// Everything here runs on the thread that makes the GL calls (the GL thread,
// gx_glthread.h); glvk keeps no locks.
#ifndef SMS_GX_GLVK_H
#define SMS_GX_GLVK_H

#include <stdint.h>

namespace glvk {

struct InitInfo {
    void* sdlWindow = nullptr;  // an SDL_Window made with SDL_WINDOW_VULKAN, or null for headless
    int vsync = 0;              // as SDL_GL_SetSwapInterval: 0 off, 1 on, -1 adaptive
};

// Brings up the instance, device and (with a window) the swapchain. False,
// with the reason logged, when Vulkan is unavailable or too old: the caller
// then falls back to OpenGL.
bool init(const InitInfo& info);
void shutdown();
bool active();

// The GL entry points (for gl::load).
void* getProc(const char* name);

// The window's drawable size, which framebuffer 0 follows (call before
// drawing a frame into it).
void setDefaultFramebufferSize(int w, int h);
// Shows framebuffer 0 (SDL_GL_SwapWindow).
void present();
void setSwapInterval(int vsync);

// "NVIDIA GeForce GTX 1060 6GB" and similar, for logs.
const char* deviceName();

// The Vulkan library glvk loads when it is not the system's loader: on macOS
// the libMoltenVK.dylib beside the executable or in the app's Frameworks
// (SMS_VK_LIBRARY names another). SDL must load the same one
// (SDL_Vulkan_LoadLibrary) before it makes a Vulkan window. Null: the loader.
const char* libraryPath();

}  // namespace glvk

#endif

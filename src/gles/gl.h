// OpenGL ES (via ANGLE) and EAGL.
#pragma once
#include "common.h"
#include <functional>
#include <string>
#include <windows.h>

namespace gles {

// Loads ANGLE and creates the EGL display + window surface for `hwnd`.
void init(HWND hwnd);
// Replaces the window surface (any thread); nullptr drops frames until a window is set again.
void set_window(HWND hwnd);
// Boot screen: calls `draw` about 60 times a second on the calling thread, with its own context
// current on the window surface, until the game presents its first frame.
void present_until_first_frame(const std::function<void(int w, int h)>& draw);
void install_gl();    // _gl* exports
void install_eagl();  // EAGLContext, CAEAGLLayer

// Save a PNG of the presented image every N frames (0 = off).
extern int g_screenshot_every;
// Saves the next presented frame to `name` (PNG).
void request_screenshot(const std::string& name);
extern bool g_glcheck;  // log GL errors per call

// True once the game has presented a frame to the window.
bool window_presented();
// Takes a pending request_screenshot() name, if any (for presenters other than the game's).
bool take_screenshot_request(std::string& name);
void write_png(const char* path, const u8* rgba, int w, int h, bool bottom_up = false);
void* current_egl_context();
// Overlays drawn into framebuffer 0 after the game's frame (top-left origin, surface pixels);
// GL state is restored afterwards.
void fill_rect(int x, int y, int w, int h, int surface_h, float r, float g, float b);
void draw_rgba_rect(const u8* rgba, int w, int h, u64 key, int x, int y, int dw, int dh, int surface_h);
// Called on the presenting thread with the surface size, after the frame and before it is shown.
extern void (*g_overlay)(int surface_w, int surface_h);
// Draws RGBA8 pixels (first row = top) letterboxed into framebuffer 0; `key` identifies the pixels.
void draw_rgba_fit(const u8* rgba, int w, int h, u64 key, int dst_w, int dst_h);

// Size of the window surface in pixels.
void surface_size(int& w, int& h);

// Decodes PVRTC1 (4 or 2 bpp) into RGBA8.
void pvrtc_decode(const u8* src, int width, int height, bool two_bpp, u8* rgba_out);

// Android: PVRTC textures re-encoded as ETC2 so far (uploads, their size, the RGBA8 size they replace, time).
struct Etc2Stats {
    u64 textures, bytes, rgba_bytes, us;
};
Etc2Stats etc2_stats();

// Looks up a function of the OpenGL ES library (ANGLE's libGLESv2.dll, or the device's libGLESv2.so).
void* gl_proc(const char* name);

// Whether the current context runs on a Qualcomm Adreno GPU (needs a current context).
bool gpu_is_adreno();

// Function-pointer access for internal use (present blit, texture uploads).
namespace fn {
using GLenum = u32;
using GLint = s32;
using GLuint = u32;
using GLsizei = s32;
extern void(__stdcall* BindFramebuffer)(GLenum, GLuint);
extern void(__stdcall* GetIntegerv)(GLenum, GLint*);
extern void(__stdcall* TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
}  // namespace fn

}  // namespace gles

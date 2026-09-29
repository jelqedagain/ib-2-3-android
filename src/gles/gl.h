// OpenGL ES (via ANGLE) and EAGL.
#pragma once
#include "common.h"
#include <string>
#include <windows.h>

namespace gles {

// Loads ANGLE and creates the EGL display + window surface for `hwnd`.
void init(HWND hwnd);
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
// Draws RGBA8 pixels (first row = top) letterboxed into framebuffer 0; `key` identifies the pixels.
void draw_rgba_fit(const u8* rgba, int w, int h, u64 key, int dst_w, int dst_h);

// Size of the window surface in pixels.
void surface_size(int& w, int& h);

// Decodes PVRTC1 (4 or 2 bpp) into RGBA8.
void pvrtc_decode(const u8* src, int width, int height, bool two_bpp, u8* rgba_out);

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

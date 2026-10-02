// OpenGL ES entry points forwarded to ANGLE (libGLESv2.dll).
#include "gles/gl.h"
#include "game/game.h"
#include "hle.h"
#include "audio/video.h"
#include "uikit/labels.h"
#include <miniz/miniz.h>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <tuple>
#include <string>
#include <unordered_map>
#include <vector>
#ifdef __ANDROID__
#include <etcpak/ProcessRGB.hpp>
#include <chrono>
#include <thread>
#include <sys/system_properties.h>
#endif

namespace gles {

using GLenum = u32;
using GLint = s32;
using GLuint = u32;
using GLsizei = s32;
using GLfloat = float;
using GLboolean = u8;
using GLbitfield = u32;
using GLintptr = s64;
using GLsizeiptr = s64;
#undef APIENTRY
#define APIENTRY __stdcall

// name, return type, parameter list, argument list
#define GL_FUNCS(X)                                                                                                  \
    X(glActiveTexture, void, (GLenum a), (a))                                                                        \
    X(glAttachShader, void, (GLuint a, GLuint b), (a, b))                                                            \
    X(glBeginQuery, void, (GLenum a, GLuint b), (a, b))                                                              \
    X(glBeginQueryEXT, void, (GLenum a, GLuint b), (a, b))                                                           \
    X(glBindBuffer, void, (GLenum a, GLuint b), (a, b))                                                              \
    X(glBindFramebuffer, void, (GLenum a, GLuint b), (a, b))                                                         \
    X(glBindRenderbuffer, void, (GLenum a, GLuint b), (a, b))                                                        \
    X(glBindTexture, void, (GLenum a, GLuint b), (a, b))                                                             \
    X(glBlendColor, void, (GLfloat a, GLfloat b, GLfloat c, GLfloat d), (a, b, c, d))                                \
    X(glBlendEquationSeparate, void, (GLenum a, GLenum b), (a, b))                                                   \
    X(glBlendFuncSeparate, void, (GLenum a, GLenum b, GLenum c, GLenum d), (a, b, c, d))                             \
    X(glBlitFramebuffer, void,                                                                                       \
      (GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, GLbitfield i, GLenum j),              \
      (a, b, c, d, e, f, g, h, i, j))                                                                                \
    X(glBufferData, void, (GLenum a, GLsizeiptr b, const void* c, GLenum d), (a, b, c, d))                           \
    X(glBufferSubData, void, (GLenum a, GLintptr b, GLsizeiptr c, const void* d), (a, b, c, d))                      \
    X(glClear, void, (GLbitfield a), (a))                                                                            \
    X(glClearColor, void, (GLfloat a, GLfloat b, GLfloat c, GLfloat d), (a, b, c, d))                                \
    X(glClearDepthf, void, (GLfloat a), (a))                                                                         \
    X(glClearStencil, void, (GLint a), (a))                                                                          \
    X(glColorMask, void, (GLboolean a, GLboolean b, GLboolean c, GLboolean d), (a, b, c, d))                         \
    X(glCompileShader, void, (GLuint a), (a))                                                                        \
    X(glCompressedTexImage2D, void,                                                                                  \
      (GLenum a, GLint b, GLenum c, GLsizei d, GLsizei e, GLint f, GLsizei g, const void* h), (a, b, c, d, e, f, g, h)) \
    X(glCreateProgram, GLuint, (), ())                                                                               \
    X(glCreateShader, GLuint, (GLenum a), (a))                                                                       \
    X(glDeleteBuffers, void, (GLsizei a, const GLuint* b), (a, b))                                                   \
    X(glDeleteFramebuffers, void, (GLsizei a, const GLuint* b), (a, b))                                              \
    X(glDeleteQueries, void, (GLsizei a, const GLuint* b), (a, b))                                                   \
    X(glDeleteQueriesEXT, void, (GLsizei a, const GLuint* b), (a, b))                                                \
    X(glDeleteRenderbuffers, void, (GLsizei a, const GLuint* b), (a, b))                                             \
    X(glDeleteTextures, void, (GLsizei a, const GLuint* b), (a, b))                                                  \
    X(glDepthFunc, void, (GLenum a), (a))                                                                            \
    X(glDepthMask, void, (GLboolean a), (a))                                                                         \
    X(glDepthRangef, void, (GLfloat a, GLfloat b), (a, b))                                                           \
    X(glDisable, void, (GLenum a), (a))                                                                              \
    X(glDisableVertexAttribArray, void, (GLuint a), (a))                                                             \
    X(glDiscardFramebufferEXT, void, (GLenum a, GLsizei b, const GLenum* c), (a, b, c))                              \
    X(glDrawArrays, void, (GLenum a, GLint b, GLsizei c), (a, b, c))                                                 \
    X(glDrawElements, void, (GLenum a, GLsizei b, GLenum c, const void* d), (a, b, c, d))                            \
    X(glEnable, void, (GLenum a), (a))                                                                               \
    X(glEnableVertexAttribArray, void, (GLuint a), (a))                                                              \
    X(glEndQuery, void, (GLenum a), (a))                                                                             \
    X(glEndQueryEXT, void, (GLenum a), (a))                                                                          \
    X(glFinish, void, (), ())                                                                                        \
    X(glFlush, void, (), ())                                                                                         \
    X(glFramebufferRenderbuffer, void, (GLenum a, GLenum b, GLenum c, GLuint d), (a, b, c, d))                       \
    X(glFramebufferTexture2D, void, (GLenum a, GLenum b, GLenum c, GLuint d, GLint e), (a, b, c, d, e))              \
    X(glFrontFace, void, (GLenum a), (a))                                                                            \
    X(glGenBuffers, void, (GLsizei a, GLuint* b), (a, b))                                                            \
    X(glGenFramebuffers, void, (GLsizei a, GLuint* b), (a, b))                                                       \
    X(glGenQueries, void, (GLsizei a, GLuint* b), (a, b))                                                            \
    X(glGenQueriesEXT, void, (GLsizei a, GLuint* b), (a, b))                                                         \
    X(glGenRenderbuffers, void, (GLsizei a, GLuint* b), (a, b))                                                      \
    X(glGenTextures, void, (GLsizei a, GLuint* b), (a, b))                                                           \
    X(glGetAttribLocation, GLint, (GLuint a, const char* b), (a, b))                                                 \
    X(glGetError, GLenum, (), ())                                                                                    \
    X(glGetIntegerv, void, (GLenum a, GLint* b), (a, b))                                                             \
    X(glGetProgramiv, void, (GLuint a, GLenum b, GLint* c), (a, b, c))                                               \
    X(glGetQueryObjectuiv, void, (GLuint a, GLenum b, GLuint* c), (a, b, c))                                         \
    X(glGetQueryObjectuivEXT, void, (GLuint a, GLenum b, GLuint* c), (a, b, c))                                      \
    X(glGetRenderbufferParameteriv, void, (GLenum a, GLenum b, GLint* c), (a, b, c))                                 \
    X(glGetShaderPrecisionFormat, void, (GLenum a, GLenum b, GLint* c, GLint* d), (a, b, c, d))                      \
    X(glGetString, const u8*, (GLenum a), (a))                                                                       \
    X(glGetUniformLocation, GLint, (GLuint a, const char* b), (a, b))                                                \
    X(glInvalidateFramebuffer, void, (GLenum a, GLsizei b, const GLenum* c), (a, b, c))                              \
    X(glLinkProgram, void, (GLuint a), (a))                                                                          \
    X(glMapBufferOES, void*, (GLenum a, GLenum b), (a, b))                                                           \
    X(glMapBufferRange, void*, (GLenum a, GLintptr b, GLsizeiptr c, GLbitfield d), (a, b, c, d))                     \
    X(glPixelStorei, void, (GLenum a, GLint b), (a, b))                                                              \
    X(glPolygonOffset, void, (GLfloat a, GLfloat b), (a, b))                                                         \
    X(glReadPixels, void, (GLint a, GLint b, GLsizei c, GLsizei d, GLenum e, GLenum f, void* g), (a, b, c, d, e, f, g)) \
    X(glRenderbufferStorage, void, (GLenum a, GLenum b, GLsizei c, GLsizei d), (a, b, c, d))                         \
    X(glRenderbufferStorageMultisample, void, (GLenum a, GLsizei b, GLenum c, GLsizei d, GLsizei e), (a, b, c, d, e)) \
    X(glScissor, void, (GLint a, GLint b, GLsizei c, GLsizei d), (a, b, c, d))                                       \
    X(glShaderSource, void, (GLuint a, GLsizei b, const char* const* c, const GLint* d), (a, b, c, d))               \
    X(glStencilFunc, void, (GLenum a, GLint b, GLuint c), (a, b, c))                                                 \
    X(glStencilFuncSeparate, void, (GLenum a, GLenum b, GLint c, GLuint d), (a, b, c, d))                            \
    X(glStencilMask, void, (GLuint a), (a))                                                                          \
    X(glStencilOp, void, (GLenum a, GLenum b, GLenum c), (a, b, c))                                                  \
    X(glStencilOpSeparate, void, (GLenum a, GLenum b, GLenum c, GLenum d), (a, b, c, d))                             \
    X(glTexImage2D, void,                                                                                            \
      (GLenum a, GLint b, GLint c, GLsizei d, GLsizei e, GLint f, GLenum g, GLenum h, const void* i),                \
      (a, b, c, d, e, f, g, h, i))                                                                                   \
    X(glTexParameteri, void, (GLenum a, GLenum b, GLint c), (a, b, c))                                               \
    X(glGetTexParameteriv, void, (GLenum a, GLenum b, GLint* c), (a, b, c))                                          \
    X(glGetAttachedShaders, void, (GLuint a, GLsizei b, GLsizei* c, GLuint* d), (a, b, c, d))                       \
    X(glUniform1fv, void, (GLint a, GLsizei b, const GLfloat* c), (a, b, c))                                         \
    X(glUniform2fv, void, (GLint a, GLsizei b, const GLfloat* c), (a, b, c))                                         \
    X(glUniform3fv, void, (GLint a, GLsizei b, const GLfloat* c), (a, b, c))                                         \
    X(glUniform4fv, void, (GLint a, GLsizei b, const GLfloat* c), (a, b, c))                                         \
    X(glUniform1i, void, (GLint a, GLint b), (a, b))                                                                 \
    X(glUniformMatrix3fv, void, (GLint a, GLsizei b, GLboolean c, const GLfloat* d), (a, b, c, d))                   \
    X(glUniformMatrix4fv, void, (GLint a, GLsizei b, GLboolean c, const GLfloat* d), (a, b, c, d))                   \
    X(glUnmapBuffer, GLboolean, (GLenum a), (a))                                                                     \
    X(glUnmapBufferOES, GLboolean, (GLenum a), (a))                                                                  \
    X(glUseProgram, void, (GLuint a), (a))                                                                           \
    X(glValidateProgram, void, (GLuint a), (a))                                                                      \
    X(glVertexAttribPointer, void, (GLuint a, GLint b, GLenum c, GLboolean d, GLsizei e, const void* f),             \
      (a, b, c, d, e, f))                                                                                            \
    X(glViewport, void, (GLint a, GLint b, GLsizei c, GLsizei d), (a, b, c, d))                                      \
    X(glGetShaderiv, void, (GLuint a, GLenum b, GLint* c), (a, b, c))                                                \
    X(glGetShaderInfoLog, void, (GLuint a, GLsizei b, GLsizei* c, char* d), (a, b, c, d))                            \
    X(glGetProgramInfoLog, void, (GLuint a, GLsizei b, GLsizei* c, char* d), (a, b, c, d))                           \
    X(glGetShaderSource, void, (GLuint a, GLsizei b, GLsizei* c, char* d), (a, b, c, d))                             \
    X(glGetFramebufferAttachmentParameteriv, void, (GLenum a, GLenum b, GLenum c, GLint* d), (a, b, c, d))           \
    X(glCheckFramebufferStatus, GLenum, (GLenum a), (a))                                                             \
    X(glIsEnabled, GLboolean, (GLenum a), (a))

namespace {
#define DECLARE(name, ret, params, args) ret(APIENTRY* p_##name) params = nullptr;
GL_FUNCS(DECLARE)
#undef DECLARE

HMODULE g_gles = nullptr;

constexpr GLenum GL_RENDERBUFFER = 0x8D41, GL_READ_FRAMEBUFFER = 0x8CA8, GL_DRAW_FRAMEBUFFER = 0x8CA9,
                 GL_READ_FRAMEBUFFER_BINDING = 0x8CAA, GL_DRAW_FRAMEBUFFER_BINDING = 0x8CA6, GL_COLOR_BUFFER_BIT = 0x4000,
                 GL_NEAREST = 0x2600, GL_EXTENSIONS = 0x1F03, GL_RENDERER = 0x1F01, GL_VENDOR = 0x1F00, GL_VERSION = 0x1F02,
                 GL_SHADING_LANGUAGE_VERSION = 0x8B8C, GL_RGBA = 0x1908, GL_UNSIGNED_BYTE = 0x1401,
                 GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82, GL_BGRA_EXT = 0x80E1, GL_RENDERBUFFER_BINDING = 0x8CA7,
                 GL_SCISSOR_TEST = 0x0C11, GL_HALF_FLOAT_OES = 0x8D61, GL_TEXTURE_2D = 0x0DE1;

std::mutex g_rb_mutex;
std::unordered_map<GLuint, std::pair<int, int>> g_rb_sizes;  // renderbuffer -> size (for APPLE resolve)

void record_rb_size(GLsizei w, GLsizei h) {
    GLint rb = 0;
    p_glGetIntegerv(GL_RENDERBUFFER_BINDING, &rb);
    std::lock_guard lock(g_rb_mutex);
    g_rb_sizes[rb] = {w, h};
}

std::pair<int, int> fb_size(GLenum target) {
    GLint type = 0, name = 0;
    p_glGetFramebufferAttachmentParameteriv(target, 0x8CE0, 0x8CD0, &type);  // COLOR_ATTACHMENT0, OBJECT_TYPE
    p_glGetFramebufferAttachmentParameteriv(target, 0x8CE0, 0x8CD1, &name);  // OBJECT_NAME
    if (type == (GLint)GL_RENDERBUFFER) {
        std::lock_guard lock(g_rb_mutex);
        auto it = g_rb_sizes.find(name);
        if (it != g_rb_sizes.end()) return it->second;
    }
    return {0, 0};
}

std::string g_extensions;

// Removes " name" from a space-separated extension list (whole words only).
void remove_extension(std::string& list, const std::string& name) {
    std::string padded = " " + list + " ";
    for (size_t p; (p = padded.find(" " + name + " ")) != std::string::npos;) padded.erase(p, name.size() + 1);
    list = padded.substr(1, padded.size() - 2);
}

// `debug.ibport.glext` changes the list for tests: "+GL_X -GL_Y" adds or hides extensions.
void apply_extension_overrides(std::string& list) {
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.ibport.glext", v) <= 0) return;
    std::string words = v;
    for (size_t i = 0; i < words.size();) {
        size_t j = words.find(' ', i);
        if (j == std::string::npos) j = words.size();
        std::string w = words.substr(i, j - i);
        i = j + 1;
        if (w.size() < 2) continue;
        remove_extension(list, w.substr(1));
        if (w[0] == '+') list += " " + w.substr(1);
        LOG_INFO("GL_EXTENSIONS: %s %s (debug.ibport.glext)", w[0] == '+' ? "added" : "hidden", w.c_str() + 1);
    }
#endif
}

struct FrameStats {
    std::atomic<u32> draws{0}, clears{0}, errors{0};
    std::atomic<s32> last_draw_fb{-1};
};
}  // namespace

namespace fn {
void(__stdcall* BindFramebuffer)(GLenum, GLuint) = nullptr;
void(__stdcall* GetIntegerv)(GLenum, GLint*) = nullptr;
void(__stdcall* TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
}  // namespace fn

void load_gl_functions() {
#ifdef _WIN32
    g_gles = LoadLibraryW(L"libGLESv2.dll");
    if (!g_gles) fatal("could not load libGLESv2.dll (ANGLE)");
#else
    g_gles = LoadLibraryW(L"libGLESv2.so");
    if (!g_gles) fatal("could not load libGLESv2.so");
#endif
#define LOAD(name, ret, params, args)                                                   \
    p_##name = reinterpret_cast<decltype(p_##name)>(GetProcAddress(g_gles, #name));     \
    if (!p_##name) LOG_WARN("ANGLE is missing %s", #name);
    GL_FUNCS(LOAD)
#undef LOAD
    fn::BindFramebuffer = p_glBindFramebuffer;
    fn::GetIntegerv = p_glGetIntegerv;
    fn::TexImage2D = p_glTexImage2D;
}

void* gl_proc(const char* name) { return reinterpret_cast<void*>(GetProcAddress(g_gles, name)); }

FrameStats g_stats;

bool gpu_is_adreno() {
    const u8* r = p_glGetString(GL_RENDERER);
    return r && std::strstr(reinterpret_cast<const char*>(r), "Adreno");
}

bool gl_diag();

// debug.ibport.framelog N (a new N each time): logs every viewport, framebuffer, texture filter and draw of
// one frame, with the textures bound to units 0-3 (for render-pass bugs).
std::atomic<int> g_framelog{0};
std::string bound_textures() {
    std::string s;
    GLint active = 0;
    p_glGetIntegerv(0x84E0 /*GL_ACTIVE_TEXTURE*/, &active);
    for (int u = 0; u < 4; u++) {
        p_glActiveTexture(0x84C0 + u);
        GLint t = 0, mn = 0, mg = 0;
        p_glGetIntegerv(0x8069, &t);
        if (!t) continue;
        p_glGetTexParameteriv(GL_TEXTURE_2D, 0x2801, &mn);
        p_glGetTexParameteriv(GL_TEXTURE_2D, 0x2800, &mg);
        s += " u" + std::to_string(u) + "=tex" + std::to_string(t) + "(min " + std::to_string(mn) + " mag " + std::to_string(mg) + ")";
    }
    p_glActiveTexture(active);
    return s;
}

void framelog_draw(const char* what, int count) {
    GLint fb = 0, prog = 0, vp[4] = {};
    p_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    p_glGetIntegerv(0x8B8D /*GL_CURRENT_PROGRAM*/, &prog);
    p_glGetIntegerv(0x0BA2 /*GL_VIEWPORT*/, vp);
    // Which pass: the post-process uniforms its shaders declare.
    std::string tags;
    GLuint shaders[4] = {};
    GLsizei n = 0;
    if (prog) p_glGetAttachedShaders(prog, 4, &n, shaders);
    static char src[65536];
    for (GLsizei i = 0; i < n; i++) {
        p_glGetShaderSource(shaders[i], sizeof src, nullptr, src);
        for (const char* tag : {"DOFPackedParameters", "BloomScaleAndThreshold", "SampleWeights", "SampleOffsets", "OffsetUVs",
                                "SourceTextureScaleBias", "SceneColorScaleBias", "HalfResMaskRect", "TexCenter", "TextureTransform"})
            if (std::strstr(src, tag) && tags.find(tag) == std::string::npos) tags += std::string(" ") + tag;
    }
    std::string ids;
    for (GLsizei i = 0; i < n; i++) ids += " " + std::to_string(shaders[i]);
    LOG_INFO("framelog: %s %d fb %d prog %d (shaders%s) viewport %d,%d %dx%d%s |%s", what, count, fb, prog, ids.c_str(), vp[0], vp[1],
             vp[2], vp[3], bound_textures().c_str(), tags.c_str());
}

// Called at every present: starts or ends a logged frame.
void framelog_frame(u64 frame) {
#ifdef __ANDROID__
    static std::string last = "0";
    if (g_framelog > 0 && --g_framelog == 0) LOG_INFO("framelog: frame %llu ends", (unsigned long long)frame);
    if (frame % 30) return;
    char v[PROP_VALUE_MAX] = "";
    __system_property_get("debug.ibport.framelog", v);
    if (last == v) return;
    bool first = last == "0" && std::string(v) == "0";
    last = v;
    if (!first && v[0] && std::string(v) != "0") {
        g_framelog = 2;  // the rest of this frame, then a whole one
        LOG_INFO("framelog: from frame %llu", (unsigned long long)frame);
    }
#else
    (void)frame;
#endif
}

void log_frame_stats(u64 frame) {
    if (frame == 600 && gl_diag()) g_glcheck = false;  // diagnostics: errors of the first frames only
    if (frame == 1) {  // which GPU and driver: many graphics bugs are specific to one
        auto str = [](GLenum name) { const u8* s = p_glGetString(name); return s ? reinterpret_cast<const char*>(s) : "?"; };
        LOG_INFO("GPU: %s (%s), %s", str(GL_RENDERER), str(GL_VENDOR), str(GL_VERSION));
    }
    // Drain errors the game did not look at.
    u32 pending = 0;
    while (p_glGetError() != 0 && pending < 32) pending++;
    GLint fb = 0, rb = 0;
    p_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    p_glGetIntegerv(GL_RENDERBUFFER_BINDING, &rb);
    static u64 last_tick = GetTickCount64(), last_frame = 0;
    u64 now = GetTickCount64();
    double fps = now > last_tick ? (double)(frame - last_frame) * 1000.0 / (double)(now - last_tick) : 0;
    last_tick = now;
    last_frame = frame;
    LOG_INFO("frame %llu: %.1f fps, %u draws, %u clears, %u+%u GL errors, present fb %d rb %d", (unsigned long long)frame, fps,
             g_stats.draws.load(), g_stats.clears.load(), g_stats.errors.load(), pending, fb, rb);
    g_stats.draws = 0;
    g_stats.clears = 0;
    g_stats.errors = 0;
}

// Saves renderbuffer `rb` (w x h) as a PNG (diagnostics / screenshots).
void save_screenshot(GLuint rb, int w, int h, const char* path) {
    GLuint fbo = 0;
    GLint prev_read = 0;
    p_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    if (rb) {
        p_glGenFramebuffers(1, &fbo);
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        p_glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, 0x8CE0, GL_RENDERBUFFER, rb);
    } else {
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);  // the window surface
    }
    std::vector<u8> px((size_t)w * h * 4);
    p_glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
    if (fbo) p_glDeleteFramebuffers(1, &fbo);
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;  // ignore destination alpha
    write_png(path, px.data(), w, h, true);  // GL rows are bottom-up
}

void write_png(const char* path, const u8* rgba, int w, int h, bool bottom_up) {
    size_t len = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba, w, h, 4, &len, 6, bottom_up);
    if (FILE* f = std::fopen(path, "wb")) {
        std::fwrite(png, 1, len, f);
        std::fclose(f);
        LOG_INFO("saved screenshot %s", path);
    }
    mz_free(png);
}

// Draws an RGBA8 image (first row = top) letterboxed on black into framebuffer 0.
// `key` identifies the pixels; the texture is re-uploaded only when it changes.
void draw_rgba_fit(const u8* rgba, int w, int h, u64 key, int dst_w, int dst_h) {
    // Texture and framebuffer names belong to the context that made them (FBOs are never shared).
    static thread_local void* ctx = nullptr;
    static thread_local GLuint tex = 0, fbo = 0;
    static thread_local u64 uploaded = ~0ull;
    static thread_local int tw = 0, th = 0;
    if (ctx != current_egl_context()) {
        ctx = current_egl_context();
        tex = fbo = 0;
        uploaded = ~0ull;
    }
    GLint prev_tex = 0, prev_read = 0, prev_draw = 0, prev_active = 0, prev_unpack = 0;
    p_glGetIntegerv(0x84E0, &prev_active);  // GL_ACTIVE_TEXTURE
    p_glActiveTexture(0x84C0);
    p_glGetIntegerv(0x8069, &prev_tex);     // GL_TEXTURE_BINDING_2D
    p_glGetIntegerv(0x0CF5, &prev_unpack);  // GL_UNPACK_ALIGNMENT
    p_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    p_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
    if (!tex) {
        p_glGenTextures(1, &tex);
        p_glGenFramebuffers(1, &fbo);
    }
    p_glBindTexture(GL_TEXTURE_2D, tex);
    if (key != uploaded || w != tw || h != th) {
        p_glPixelStorei(0x0CF5, 4);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        p_glTexParameteri(GL_TEXTURE_2D, 0x2801, 0x2601);  // MIN_FILTER LINEAR
        p_glTexParameteri(GL_TEXTURE_2D, 0x2800, 0x2601);  // MAG_FILTER LINEAR
        uploaded = key;
        tw = w, th = h;
    }
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    p_glFramebufferTexture2D(GL_READ_FRAMEBUFFER, 0x8CE0, GL_TEXTURE_2D, tex, 0);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    GLboolean scissor = p_glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) p_glDisable(GL_SCISSOR_TEST);
    double s = std::min((double)dst_w / tw, (double)dst_h / th);
    int ow = (int)(tw * s), oh = (int)(th * s), ox = (dst_w - ow) / 2, oy = (dst_h - oh) / 2;
    p_glClearColor(0, 0, 0, 1);
    p_glClear(GL_COLOR_BUFFER_BIT);
    // The image's first row is the top of the picture, so flip vertically while blitting.
    p_glBlitFramebuffer(0, 0, tw, th, ox, oy + oh, ox + ow, oy, GL_COLOR_BUFFER_BIT, 0x2601);
    if (scissor) p_glEnable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
    p_glBindTexture(GL_TEXTURE_2D, prev_tex);
    p_glPixelStorei(0x0CF5, prev_unpack);
    p_glActiveTexture(prev_active);
}

// --- Overlay drawing (after the game's frame is in framebuffer 0) --------------------------
// Both helpers restore every piece of state they touch: UE3's renderer caches GL state.

struct SavedState {
    GLint draw_fb = 0, read_fb = 0, box[4] = {};
    GLboolean scissor = 0;
    float clear[4] = {};
    SavedState() {
        static auto get_floats = reinterpret_cast<void(__stdcall*)(GLenum, float*)>(GetProcAddress(g_gles, "glGetFloatv"));
        p_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
        p_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
        p_glGetIntegerv(0x0C10 /*GL_SCISSOR_BOX*/, box);
        scissor = p_glIsEnabled(GL_SCISSOR_TEST);
        if (get_floats) get_floats(0x0C22 /*GL_COLOR_CLEAR_VALUE*/, clear);
    }
    ~SavedState() {
        p_glClearColor(clear[0], clear[1], clear[2], clear[3]);
        p_glScissor(box[0], box[1], box[2], box[3]);
        if (scissor) p_glEnable(GL_SCISSOR_TEST);
        else p_glDisable(GL_SCISSOR_TEST);
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fb);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fb);
    }
};

void fill_rect(int x, int y, int w, int h, int surface_h, float r, float g, float b) {
    if (w <= 0 || h <= 0) return;
    SavedState saved;
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    p_glEnable(GL_SCISSOR_TEST);
    p_glScissor(x, surface_h - y - h, w, h);  // top-left origin -> GL's bottom-left
    p_glClearColor(r, g, b, 1);
    p_glClear(GL_COLOR_BUFFER_BIT);
}

void draw_rgba_rect(const u8* rgba, int w, int h, u64 key, int x, int y, int dw, int dh, int surface_h) {
    static thread_local void* ctx = nullptr;
    static thread_local GLuint tex = 0, fbo = 0;
    static thread_local u64 uploaded = ~0ull;
    if (ctx != current_egl_context()) {
        ctx = current_egl_context();
        tex = fbo = 0;
        uploaded = ~0ull;
    }
    SavedState saved;
    GLint prev_tex = 0, prev_active = 0, prev_unpack = 0;
    p_glGetIntegerv(0x84E0, &prev_active);
    p_glActiveTexture(0x84C0);
    p_glGetIntegerv(0x8069, &prev_tex);
    p_glGetIntegerv(0x0CF5, &prev_unpack);
    if (!tex) {
        p_glGenTextures(1, &tex);
        p_glGenFramebuffers(1, &fbo);
    }
    p_glBindTexture(GL_TEXTURE_2D, tex);
    if (key != uploaded) {
        p_glPixelStorei(0x0CF5, 4);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        p_glTexParameteri(GL_TEXTURE_2D, 0x2801, 0x2601);
        p_glTexParameteri(GL_TEXTURE_2D, 0x2800, 0x2601);
        uploaded = key;
    }
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    p_glFramebufferTexture2D(GL_READ_FRAMEBUFFER, 0x8CE0, GL_TEXTURE_2D, tex, 0);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    p_glDisable(GL_SCISSOR_TEST);
    int gy = surface_h - y - dh;  // first image row is the top
    p_glBlitFramebuffer(0, 0, w, h, x, gy + dh, x + dw, gy, GL_COLOR_BUFFER_BIT, 0x2601);
    p_glBindTexture(GL_TEXTURE_2D, prev_tex);
    p_glPixelStorei(0x0CF5, prev_unpack);
    p_glActiveTexture(prev_active);
}

// Draws the current movie frame (if any) letterboxed into the window. Returns false if no movie.
bool draw_movie(int dst_w, int dst_h) {
    const u8* rgba;
    int w, h;
    u64 serial;
    if (!video::current_frame(rgba, w, h, serial)) return false;
    u64 label_version = 0;
    auto labels = uikit::visible_labels(label_version);
    const u8* pixels = rgba;
    static thread_local std::vector<u8> composed;
    if (!labels.empty()) {  // subtitles are UILabels on top of the movie
        composed.assign(rgba, rgba + (size_t)w * h * 4);
        uikit::draw_labels(composed.data(), w, h, labels);
        pixels = composed.data();
    }
    draw_rgba_fit(pixels, w, h, serial * 0x9E3779B97F4A7C15ull ^ label_version, dst_w, dst_h);
    video::release_frame();
    return true;
}

// Present helper: copies renderbuffer `rb` (w x h) to the window surface (framebuffer 0).
void blit_to_window(GLuint rb, int w, int h, int dst_w, int dst_h) {
    static thread_local GLuint fbo = 0;
    GLint prev_read = 0, prev_draw = 0;
    p_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    p_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
    GLboolean scissor = p_glIsEnabled(GL_SCISSOR_TEST);
    if (!fbo) p_glGenFramebuffers(1, &fbo);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    p_glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, 0x8CE0, GL_RENDERBUFFER, rb);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    if (scissor) p_glDisable(GL_SCISSOR_TEST);
    // Letterbox to preserve aspect ratio.
    double sx = (double)dst_w / w, sy = (double)dst_h / h, s = std::min(sx, sy);
    int ow = (int)(w * s), oh = (int)(h * s), ox = (dst_w - ow) / 2, oy = (dst_h - oh) / 2;
    p_glClearColor(0, 0, 0, 1);
    p_glClear(GL_COLOR_BUFFER_BIT);
    p_glBlitFramebuffer(0, 0, w, h, ox, oy, ox + ow, oy + oh, GL_COLOR_BUFFER_BIT, 0x2601 /*LINEAR*/);
    if (scissor) p_glEnable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
}

bool g_glcheck = false;

// With -glcheck, reports the first GL error raised by each entry point.
struct ErrorCheck {
    const char* name;
    ~ErrorCheck() {
        if (!g_glcheck || !p_glGetError) return;
        GLenum e = p_glGetError();
        if (!e) return;
        static std::mutex m;
        static std::unordered_map<std::string, int> seen;
        std::lock_guard lock(m);
        if (seen[name]++ < 3) LOG_ERROR("GL error 0x%x from %s", e, name);
    }
};

// Diagnostics for GPUs we cannot test on (IB3 draws no world on PowerVR): render target formats,
// framebuffer completeness and, for the first 600 frames, the first GL errors of each call.
// On for PowerVR; debug.ibport.gldiag 1/0 forces it.
bool gl_diag() {
    static int on = -1;
    if (on < 0) {
        const u8* r = p_glGetString(GL_RENDERER);
        if (!r) return false;  // no context yet
        on = std::strstr(reinterpret_cast<const char*>(r), "PowerVR") != nullptr;
#ifdef __ANDROID__
        char v[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.ibport.gldiag", v) > 0 && (v[0] == '0' || v[0] == '1')) on = v[0] == '1';
#endif
        if (on) {
            g_glcheck = true;
            LOG_INFO("gldiag: on (render targets, framebuffers, GL errors of the first 600 frames)");
        }
    }
    return on;
}

// Whether the GPU can draw into GL_BGRA_EXT textures. IB3's render targets (scene colour, bloom,
// shadow colour) are empty BGRA textures; PowerVR (Pixel 10) only samples BGRA, so those framebuffers
// were incomplete and the world stayed black under the HUD (IB2's are RGBA). Tested once with a
// small framebuffer; debug.ibport.bgrart 0/1 forces the answer.
bool bgra_renderable() {
    static int ok = -1;
    if (ok >= 0) return ok;
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.ibport.bgrart", v) > 0 && (v[0] == '0' || v[0] == '1')) {
        ok = v[0] == '1';
        LOG_INFO("render targets: BGRA %s (debug.ibport.bgrart)", ok ? "used" : "replaced by RGBA");
        return ok;
    }
#endif
    GLint prev_tex = 0, prev_draw = 0, prev_read = 0;
    p_glGetIntegerv(0x8069 /*GL_TEXTURE_BINDING_2D*/, &prev_tex);
    p_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
    p_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    GLuint tex = 0, fbo = 0;
    p_glGenTextures(1, &tex);
    p_glBindTexture(GL_TEXTURE_2D, tex);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, 16, 16, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, nullptr);
    p_glGenFramebuffers(1, &fbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    p_glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, 0x8CE0 /*COLOR_ATTACHMENT0*/, GL_TEXTURE_2D, tex, 0);
    GLenum st = p_glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
    p_glDeleteFramebuffers(1, &fbo);
    p_glBindTexture(GL_TEXTURE_2D, prev_tex);
    p_glDeleteTextures(1, &tex);
    while (p_glGetError() != 0) {}
    ok = st == 0x8CD5;
    if (!ok) LOG_INFO("render targets: this GPU cannot draw into BGRA textures (0x%x); empty ones are made RGBA", st);
    return ok;
}

// Whether the GPU can draw into half-float RGBA16F textures (EXT_color_buffer_half_float / _float).
// Mods that turn on FloatingPointRenderTargets (the IB3 "Dev Mod" .ipa's graphics profiles) make the
// render targets unsized GL_RGBA with GL_HALF_FLOAT: Apple's driver took that, GLES 3 rejects it
// (GL_INVALID_OPERATION), so the targets never existed and the world was black under the HUD.
bool half_float_renderable() {
    static int ok = -1;
    if (ok >= 0) return ok;
    GLint prev_tex = 0, prev_draw = 0, prev_read = 0;
    p_glGetIntegerv(0x8069 /*GL_TEXTURE_BINDING_2D*/, &prev_tex);
    p_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
    p_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    GLuint tex = 0, fbo = 0;
    p_glGenTextures(1, &tex);
    p_glBindTexture(GL_TEXTURE_2D, tex);
    p_glTexImage2D(GL_TEXTURE_2D, 0, 0x881A /*GL_RGBA16F*/, 16, 16, 0, GL_RGBA, 0x140B /*GL_HALF_FLOAT*/, nullptr);
    p_glGenFramebuffers(1, &fbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    p_glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, 0x8CE0 /*COLOR_ATTACHMENT0*/, GL_TEXTURE_2D, tex, 0);
    GLenum st = p_glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
    p_glDeleteFramebuffers(1, &fbo);
    p_glBindTexture(GL_TEXTURE_2D, prev_tex);
    p_glDeleteTextures(1, &tex);
    while (p_glGetError() != 0) {}
    ok = st == 0x8CD5;
    LOG_INFO("render targets: half-float ones are %s", ok ? "RGBA16F" : "made 8-bit RGBA (this GPU cannot draw into RGBA16F)");
    return ok;
}

// IB3 with MSAA (Settings, Anti-aliasing MSAA 4x) draws its menu scenes, such as the item models of the
// inventory, into a framebuffer of the multisampled colour renderbuffer and the single-sampled scene depth
// texture. Apple's driver took that; GLES calls it incomplete (0x8D56) and draws nothing, so the items
// were invisible. Such a framebuffer gets a multisampled depth-stencil renderbuffer of the same size instead
// (for depth and stencil: the game attaches the depth-stencil texture to both, one after the other).
void fix_mixed_samples(GLenum target) {
    if (game::is_infinity_blade_2()) return;
    GLenum status = p_glCheckFramebufferStatus(target);
    if (status != 0x8D56 /*INCOMPLETE_MULTISAMPLE*/ && status != 0x8CDD /*UNSUPPORTED*/) return;
    GLint type = 0, color = 0, depth_type = 0, stencil_type = 0;
    p_glGetFramebufferAttachmentParameteriv(target, 0x8CE0, 0x8CD0, &type);  // COLOR_ATTACHMENT0, OBJECT_TYPE
    p_glGetFramebufferAttachmentParameteriv(target, 0x8CE0, 0x8CD1, &color);  // OBJECT_NAME
    p_glGetFramebufferAttachmentParameteriv(target, 0x8D00, 0x8CD0, &depth_type);
    p_glGetFramebufferAttachmentParameteriv(target, 0x8D20, 0x8CD0, &stencil_type);
    if (type != (GLint)GL_RENDERBUFFER || (depth_type != 0x1702 /*TEXTURE*/ && stencil_type != 0x1702)) return;
    GLint prev_rb = 0, samples = 0, w = 0, h = 0;
    p_glGetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rb);
    p_glBindRenderbuffer(GL_RENDERBUFFER, color);
    p_glGetRenderbufferParameteriv(GL_RENDERBUFFER, 0x8CAB /*SAMPLES*/, &samples);
    p_glGetRenderbufferParameteriv(GL_RENDERBUFFER, 0x8D42 /*WIDTH*/, &w);
    p_glGetRenderbufferParameteriv(GL_RENDERBUFFER, 0x8D43 /*HEIGHT*/, &h);
    if (samples > 0 && w > 0 && h > 0) {
        static std::mutex m;
        static std::map<std::tuple<int, int, int>, GLuint> depth_buffers;
        std::lock_guard lock(m);
        GLuint& rb = depth_buffers[{w, h, samples}];
        if (!rb) {
            p_glGenRenderbuffers(1, &rb);
            p_glBindRenderbuffer(GL_RENDERBUFFER, rb);
            p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, 0x88F0 /*DEPTH24_STENCIL8*/, w, h);
        }
        p_glFramebufferRenderbuffer(target, 0x8D00 /*DEPTH*/, GL_RENDERBUFFER, rb);
        p_glFramebufferRenderbuffer(target, 0x8D20 /*STENCIL*/, GL_RENDERBUFFER, rb);
        static std::atomic<int> logged{0};
        if (logged++ < 3)
            LOG_INFO("msaa: framebuffer with a %d-sample colour buffer and a depth texture gets a %d-sample depth buffer (%dx%d): 0x%x",
                     samples, samples, w, h, p_glCheckFramebufferStatus(target));
    }
    p_glBindRenderbuffer(GL_RENDERBUFFER, prev_rb);
}

void diag_storage(GLsizei samples, GLenum fmt, GLsizei w, GLsizei h) {
    static std::atomic<int> logged{0};
    if (!gl_diag() || logged++ >= 40) return;
    GLint rb = 0;
    p_glGetIntegerv(GL_RENDERBUFFER_BINDING, &rb);
    LOG_INFO("gldiag: renderbuffer %d: %dx%d format 0x%x, %d samples", rb, w, h, fmt, samples);
}

void diag_framebuffer(const char* what, GLenum target, GLenum attachment, GLuint object, GLint level) {
    static std::atomic<int> n{0};
    if (!gl_diag() || n >= 120) return;
    n++;
    GLint fb = 0;
    p_glGetIntegerv(target == GL_READ_FRAMEBUFFER ? GL_READ_FRAMEBUFFER_BINDING : GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    GLenum st = p_glCheckFramebufferStatus(target);
    LOG_INFO("gldiag: framebuffer %d: %s %u (level %d) at 0x%x -> %s 0x%x", fb, what, object, level, attachment,
             st == 0x8CD5 ? "complete" : "INCOMPLETE", st);
}

// GLSL ES 1.00 only allows constant initializers on global variables. Apple's compiler also took
// uniforms (Infinity Blade II's light shafts: `float BloomScale = LightShaftParameters.y;`), which
// stricter drivers reject. Such globals become plain declarations assigned at the start of main().
// Whether an expression is made of numbers and constructors of basic types only (no variable, uniform or call).
bool only_literals(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        if (std::isalpha(c) || c == '_') {
            size_t j = i;
            while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
            std::string w = s.substr(i, j - i);
            bool type = w == "float" || w == "int" || w == "bool" || w == "true" || w == "false" || w.compare(0, 3, "vec") == 0 ||
                        w.compare(0, 4, "ivec") == 0 || w.compare(0, 4, "bvec") == 0 || w.compare(0, 3, "mat") == 0;
            if (!type) return false;
            i = j;
        } else if (std::isdigit(c) || c == '.') {
            while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '.' ||
                                    ((s[i] == '-' || s[i] == '+') && (s[i - 1] == 'e' || s[i - 1] == 'E'))))
                i++;
        } else {
            i++;
        }
    }
    return true;
}

std::string fix_global_initializers(const std::string& src) {
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    std::string out, assignments;
    int depth = 0;
    for (size_t pos = 0; pos < src.size();) {
        size_t end = src.find('\n', pos);
        if (end == std::string::npos) end = src.size();
        std::string line = src.substr(pos, end - pos);
        pos = end + 1;
        std::string code = trim(line.substr(0, line.find("//")));
        size_t eq = code.find('=');
        bool movable = depth == 0 && eq != std::string::npos && code.back() == ';' && code[0] != '#' &&
                       code.find_first_of("{}(") > eq && code.find(',') > eq && code.compare(0, 6, "const ") != 0 &&
                       code.compare(0, 8, "uniform ") != 0 && code.compare(0, 10, "attribute ") != 0 &&
                       code.compare(0, 8, "varying ") != 0 && code.compare(0, 10, "precision ") != 0;
        // A constant initializer is legal as it is (Infinity Blade III's FXAA pass has several): leave those alone.
        // Infinity Blade III only: Infinity Blade II keeps the rewrite it shipped with in 1.3.2.
        if (movable && !game::is_infinity_blade_2() && only_literals(trim(code.substr(eq + 1, code.size() - eq - 2)))) movable = false;
        std::string decl = movable ? trim(code.substr(0, eq)) : std::string();
        size_t name_at = decl.find_last_of(" \t");
        if (movable && name_at != std::string::npos) {
            out += decl + ";\n";
            assignments += decl.substr(name_at + 1) + " = " + trim(code.substr(eq + 1, code.size() - eq - 2)) + ";\n";
        } else {
            out += line + "\n";
        }
        for (char c : code) depth += c == '{' ? 1 : c == '}' ? -1 : 0;
    }
    if (assignments.empty()) return src;
    size_t main_at = out.find("void main()");
    size_t brace = main_at == std::string::npos ? std::string::npos : out.find('{', main_at);
    if (brace == std::string::npos) return src;
    out.insert(brace + 1, "\n" + assignments);
    return out;
}

// Infinity Blade II's modulated-shadow projection reconstructs each pixel's position from the scene
// depth texture in the shader's default precision (mediump, and lowp samplers). Apple's GPUs read
// depth at full precision anyway; others really use 16 bits, and device depth (close to 1.0) then
// snaps to a few values, so shadow edges come out as screen-aligned blocks. Run the shader at full
// precision. It also takes a single sample of the shadow map (bilinear on Apple's GPUs, the nearest
// texel elsewhere): use the engine's 3x3 PCF filter (ManualPCF) that the shader already contains,
// if the engine supplied the texel size.
std::string smooth_modulated_shadows(const std::string& src) {
    static const std::string single = "float Shadow = CalculateOcclusion( ReprojectedSceneDepth, ShadowDepth );";
    static const std::string medium = "precision mediump float;";
    size_t at = src.find(single), pcf = src.find("float  ManualPCF("), precision = src.find(medium);
    if (at == std::string::npos || pcf == std::string::npos || pcf > at || precision > pcf) return src;
    std::string out = src;
    out.replace(at, single.size(),
                "float Shadow = ShadowTexelSize.x > 0.0 ? ManualPCF( vec4(ShadowPosition.xy, ReprojectedSceneDepth, 1.0) )"
                " : CalculateOcclusion( ReprojectedSceneDepth, ShadowDepth );");
    out.replace(precision, medium.size(), "precision highp float;\nprecision highp sampler2D;");
    return out;
}

#ifdef __ANDROID__
// Android GPUs can't sample PVRTC, and decoded to RGBA8 it takes 8x (2 bpp: 16x) the memory the game
// budgets for. IB3: over 1 GB of textures at the beach, 2 GB while the tutorial hands over to it, enough
// for Android to kill it on 6-8 GB phones; IB2: 0.7-0.9 GB, GPU peaks of 1.2 GB. On Android both games'
// PVRTC is re-encoded as ETC2 (4 bpp, 8 with alpha).
namespace {
std::atomic<u64> g_etc2_textures{0}, g_etc2_bytes{0}, g_etc2_rgba_bytes{0};
std::atomic<u64> g_converted{0}, g_converted_us{0}, g_cached{0}, g_cached_us{0};

// `rgba` is w x h RGBA8; returns ETC2 blocks for the texture padded to whole 4x4 blocks.
std::vector<u64> encode_etc2(const u8* rgba, int w, int h, bool alpha) {
    const int bw = (w + 3) / 4, bh = (h + 3) / 4, pw = bw * 4;
    // etcpak wants BGRA pixels and whole blocks: swap red/blue, repeat the last row/column into the padding.
    std::vector<u32> px((size_t)pw * bh * 4);
    for (int y = 0; y < bh * 4; y++) {
        const u8* row = rgba + (size_t)std::min(y, h - 1) * w * 4;
        u32* out = &px[(size_t)y * pw];
        for (int x = 0; x < pw; x++) {
            const u8* p = row + std::min(x, w - 1) * 4;
            out[x] = p[2] | (p[1] << 8) | (p[0] << 16) | ((u32)p[3] << 24);
        }
    }
    const int words = alpha ? 2 : 1;  // ETC2 RGBA: an EAC alpha block before each colour block
    std::vector<u64> blocks((size_t)bw * bh * words);
    auto rows = [&](int y0, int y1) {
        if (y0 >= y1) return;
        const u32* src = &px[(size_t)y0 * 4 * pw];
        u64* dst = &blocks[(size_t)y0 * bw * words];
        if (alpha) CompressEtc2Rgba(src, dst, (u32)((y1 - y0) * bw), pw, true);
        else CompressEtc2Rgb(src, dst, (u32)((y1 - y0) * bw), pw, true);
    };
    // Big textures are split over a few threads (a 2048x2048 one is 262144 blocks).
    const int threads = bw * bh >= 16384 ? (int)std::min(4u, std::max(1u, std::thread::hardware_concurrency())) : 1;
    if (threads == 1) {
        rows(0, bh);
    } else {
        std::vector<std::thread> pool;
        int step = (bh + threads - 1) / threads;
        for (int i = 1; i < threads; i++) pool.emplace_back(rows, i * step, std::min(bh, (i + 1) * step));
        rows(0, std::min(bh, step));
        for (auto& t : pool) t.join();
    }
    return blocks;
}
}  // namespace
#endif

Etc2Stats etc2_stats() {
#ifdef __ANDROID__
    return {g_etc2_textures, g_etc2_bytes, g_etc2_rgba_bytes, g_converted, g_converted_us, g_cached, g_cached_us};
#else
    return {};
#endif
}

void install_gl() {
#define REGISTER(name, ret, params, args)     hle::fn("_" #name, [] params -> ret { ErrorCheck check{#name}; return p_##name args; });
    GL_FUNCS(REGISTER)
#undef REGISTER

    // Extension string: ANGLE's plus the Apple/IMG extensions we emulate.
    hle::fn("_glGetString", [](GLenum name) -> const u8* {
        const u8* s = p_glGetString(name);
        if (name == GL_EXTENSIONS) {
            static std::once_flag once;
            std::call_once(once, [s] {
                g_extensions = s ? reinterpret_cast<const char*>(s) : "";
                g_extensions += " GL_IMG_texture_compression_pvrtc GL_APPLE_framebuffer_multisample GL_APPLE_texture_max_level";
                apply_extension_overrides(g_extensions);
                LOG_INFO("GL_EXTENSIONS reported to game: %s", g_extensions.c_str());
            });
            return reinterpret_cast<const u8*>(g_extensions.c_str());
        }
        LOG_DEBUG("glGetString(0x%x) = %s", name, s ? reinterpret_cast<const char*>(s) : "(null)");
        return s;
    });

    hle::fn("_glCompressedTexImage2D", [](GLenum target, GLint level, GLenum fmt, GLsizei w, GLsizei h, GLint border, GLsizei size,
                                          const void* data) {
        ErrorCheck check{"glCompressedTexImage2D"};
        if (fmt >= 0x8C00 && fmt <= 0x8C03) {  // PVRTC1: decode to RGBA8
            bool two_bpp = fmt == 0x8C01 || fmt == 0x8C03;
#ifdef __ANDROID__
            if (w > 0 && h > 0) {
                auto t0 = std::chrono::steady_clock::now();
                bool alpha = fmt == 0x8C02 || fmt == 0x8C03;  // PVRTC RGBA formats
                u64 key = data ? texture_cache_key(data, (size_t)size, fmt, w, h) : 0;
                std::vector<u64> etc;
                bool cached = data && texture_cache_get(key, etc);
                if (!cached) {
                    std::vector<u8> rgba((size_t)w * h * 4);
                    if (data) pvrtc_decode(static_cast<const u8*>(data), w, h, two_bpp, rgba.data());
                    etc = encode_etc2(rgba.data(), w, h, alpha);
                    if (data) texture_cache_put(key, etc);
                }
                GLsizei bytes = (GLsizei)(etc.size() * 8);
                p_glCompressedTexImage2D(target, level, alpha ? 0x9278 : 0x9274,  // GL_COMPRESSED_RGBA8_ETC2_EAC / RGB8_ETC2
                                         w, h, border, bytes, etc.data());
                if (g_etc2_textures++ == 0) LOG_INFO("textures: PVRTC is re-encoded as ETC2 (Android GPUs can't use PVRTC)");
                g_etc2_bytes += bytes;
                g_etc2_rgba_bytes += (u64)w * h * 4;
                u64 us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
                (cached ? g_cached : g_converted)++;
                (cached ? g_cached_us : g_converted_us) += us;
                return;
            }
#endif
            std::vector<u8> rgba((size_t)std::max(w, 1) * std::max(h, 1) * 4);
            if (data) pvrtc_decode(static_cast<const u8*>(data), w, h, two_bpp, rgba.data());
            p_glTexImage2D(target, level, GL_RGBA, w, h, border, GL_RGBA, GL_UNSIGNED_BYTE, data ? rgba.data() : nullptr);
            return;
        }
        p_glCompressedTexImage2D(target, level, fmt, w, h, border, size, data);
    });
    hle::fn("_glTexImage2D", [](GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type,
                                const void* data) {
        ErrorCheck check{"glTexImage2D"};
        if (fmt == GL_BGRA_EXT) ifmt = GL_BGRA_EXT;  // APPLE_texture_format_BGRA8888 allows RGBA internal format
        // An empty BGRA texture is a render target: RGBA where BGRA cannot be drawn into (no pixels, so no swizzle).
        if (fmt == GL_BGRA_EXT && !data && !game::is_infinity_blade_2() && !bgra_renderable()) ifmt = fmt = GL_RGBA;
        // Half-float data with an unsized format (iOS) needs the sized one on GLES 3; an empty texture is a
        // render target, 8-bit RGBA where the GPU cannot draw into RGBA16F (half_float_renderable).
        if ((type == 0x140B /*GL_HALF_FLOAT*/ || type == GL_HALF_FLOAT_OES) && (ifmt == GL_RGBA || ifmt == 0x1907 /*GL_RGB*/)) {
            if (data || half_float_renderable()) {
                ifmt = ifmt == GL_RGBA ? 0x881A /*GL_RGBA16F*/ : 0x881B /*GL_RGB16F*/;
                type = 0x140B;
            } else {
                ifmt = fmt = GL_RGBA;
                type = GL_UNSIGNED_BYTE;
            }
        }
        p_glTexImage2D(target, level, ifmt, w, h, border, fmt, type, data);
        if (!ifmt && !fmt && gl_diag()) {
            static std::atomic<int> once{0};
            if (once++ == 0) LOG_INFO("gldiag: texture with no format from:\n%s", cpu::current().backtrace().c_str());
        }
        static std::atomic<int> logged{0};
        if (!data && gl_diag() && logged++ < 80) {
            GLint tex = 0;
            p_glGetIntegerv(0x8069 /*GL_TEXTURE_BINDING_2D*/, &tex);
            LOG_INFO("gldiag: empty texture %d target 0x%x level %d: %dx%d internal 0x%x format 0x%x type 0x%x", tex, target, level, w, h,
                     ifmt, fmt, type);
        }
    });
    hle::fn("_glFramebufferTexture2D", [](GLenum target, GLenum attachment, GLenum textarget, GLuint tex, GLint level) {
        ErrorCheck check{"glFramebufferTexture2D"};
        p_glFramebufferTexture2D(target, attachment, textarget, tex, level);
        diag_framebuffer("texture", target, attachment, tex, level);
        fix_mixed_samples(target);
    });
    hle::fn("_glFramebufferRenderbuffer", [](GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb) {
        ErrorCheck check{"glFramebufferRenderbuffer"};
        p_glFramebufferRenderbuffer(target, attachment, rbtarget, rb);
        diag_framebuffer("renderbuffer", target, attachment, rb, 0);
        fix_mixed_samples(target);
    });
    hle::fn("_glRenderbufferStorage", [](GLenum t, GLenum fmt, GLsizei w, GLsizei h) {
        ErrorCheck check{"glRenderbufferStorage"};
        p_glRenderbufferStorage(t, fmt, w, h);
        record_rb_size(w, h);
        diag_storage(0, fmt, w, h);
    });
    auto ms_storage = [](GLenum t, GLsizei samples, GLenum fmt, GLsizei w, GLsizei h) {
        p_glRenderbufferStorageMultisample(t, samples, fmt, w, h);
        record_rb_size(w, h);
        diag_storage(samples, fmt, w, h);
    };
    hle::fn("_glRenderbufferStorageMultisample", ms_storage);
    hle::fn("_glRenderbufferStorageMultisampleAPPLE", ms_storage);
    hle::fn("_glResolveMultisampleFramebufferAPPLE", []() {
        auto [w, h] = fb_size(GL_DRAW_FRAMEBUFFER);
        if (!w) std::tie(w, h) = fb_size(GL_READ_FRAMEBUFFER);
        GLboolean scissor = p_glIsEnabled(GL_SCISSOR_TEST);
        if (scissor) p_glDisable(GL_SCISSOR_TEST);
        p_glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        if (scissor) p_glEnable(GL_SCISSOR_TEST);
        if (gl_diag()) {
            GLint rfb = 0, dfb = 0, type = 0, name = 0;
            p_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
            p_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
            GLenum err = p_glGetError();
            p_glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, 0x8CE0, 0x8CD0, &type);
            p_glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, 0x8CE0, 0x8CD1, &name);
            static std::mutex m;
            static std::map<std::pair<int, int>, int> seen;
            std::lock_guard lock(m);
            if (seen[{rfb, dfb}]++ < 2)
                LOG_INFO("gldiag: multisample resolve %dx%d from framebuffer %d to %d (colour %s %d), GL error 0x%x", w, h, rfb, dfb,
                         type == 0x1702 ? "texture" : type == (GLint)GL_RENDERBUFFER ? "renderbuffer" : "none", name, err);
        }
    });

    // The game never checks compile/link status; log failures ourselves.
    // Per-frame statistics (logged from present) to diagnose blank frames.
    hle::fn("_glDrawElements", [](GLenum mode, GLsizei count, GLenum type, const void* idx) {
        ErrorCheck check{"glDrawElements"};
        g_stats.draws++;
        GLint fb = 0;
        p_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
        g_stats.last_draw_fb = fb;
        if (g_framelog > 0) framelog_draw("drawElements", count);
        p_glDrawElements(mode, count, type, idx);
    });
    hle::fn("_glDrawArrays", [](GLenum mode, GLint first, GLsizei count) {
        ErrorCheck check{"glDrawArrays"};
        g_stats.draws++;
        if (g_framelog > 0) framelog_draw("drawArrays", count);
        p_glDrawArrays(mode, first, count);
    });
    hle::fn("_glClear", [](GLbitfield mask) {
        ErrorCheck check{"glClear"};
        g_stats.clears++;
        p_glClear(mask);
    });
    hle::fn("_glGetError", []() -> GLenum {
        GLenum e = p_glGetError();
        if (e) g_stats.errors++;
        return e;
    });
    hle::fn("_glShaderSource", [](GLuint s, GLsizei count, const char* const* strings, const GLint* lengths) {
        std::string src;
        for (GLsizei i = 0; i < count; i++)
            src += lengths && lengths[i] >= 0 ? std::string(strings[i], lengths[i]) : std::string(strings[i]);
        std::string moved = fix_global_initializers(src);
        std::string fixed = smooth_modulated_shadows(moved);
        static std::atomic<int> total{0}, rewritten{0};
        int n = ++total;
        if (moved != src || fixed != moved) {
            int r = ++rewritten;
            if (r <= 10 || r % 50 == 0)
                LOG_INFO("shader %d rewritten (%d so far): %s", n, r, moved != src ? "globals moved into main" : "shadow filter");
        }
#ifdef __ANDROID__
        // debug.ibport.dumpshaders 1: every shader as compiled goes to shaderdump/<n>.glsl (for reverse engineering).
        static const bool dump = [] {
            char v[PROP_VALUE_MAX] = {};
            return __system_property_get("debug.ibport.dumpshaders", v) > 0 && v[0] == '1';
        }();
        if (dump) {
            std::error_code ec;
            std::filesystem::create_directories("shaderdump", ec);
            std::ofstream("shaderdump/" + std::to_string(s) + ".glsl", std::ios::binary) << fixed;
        }
#endif
        if (fixed == src) return p_glShaderSource(s, count, strings, lengths);
        const char* one = fixed.c_str();
        p_glShaderSource(s, 1, &one, nullptr);
    });
    hle::fn("_glCompileShader", [](GLuint s) {
        p_glCompileShader(s);
        GLint ok = 0;
        p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[4096] = {};
            p_glGetShaderInfoLog(s, sizeof log, nullptr, log);
            static char src[65536];
            p_glGetShaderSource(s, sizeof src, nullptr, src);
            LOG_ERROR("shader %u failed to compile:\n%s\n--- source ---\n%s", s, log, src);
            static std::atomic<int> once{0};
            if (once++ == 0) LOG_ERROR("first failing compile from:\n%s", cpu::current().backtrace().c_str());
        }
    });
    hle::fn("_glLinkProgram", [](GLuint p) {
        p_glLinkProgram(p);
        GLint ok = 0;
        p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[4096] = {};
            p_glGetProgramInfoLog(p, sizeof log, nullptr, log);
            LOG_ERROR("program %u failed to link: %s", p, log);
        }
    });
}

}  // namespace gles

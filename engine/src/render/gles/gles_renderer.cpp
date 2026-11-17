// GLES 3.0 fallback for devices without usable Vulkan 1.1.
// Mirrors the Vulkan path: borrowed ANativeWindow, RGBA8, no depth, vsync.
// Sprites: one instanced glDrawArraysInstanced per batch, corners from gl_VertexID, the
// 36-byte instance stream re-specified (orphaned) every frame. ES 3.0 has no base-instance,
// so each batch re-points the instance attributes at its first record instead.
// Textures keep their CPU bytes so a lost context (EGL_CONTEXT_LOST) can rebuild everything.
#include "mge/render/renderer.h"
#include "mge/log.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <cstddef>
#include <cstring>
#include <vector>

#ifndef GL_COMPRESSED_RGBA_ASTC_4x4_KHR
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR 0x93B0
#define GL_COMPRESSED_RGBA_ASTC_6x6_KHR 0x93B4
#define GL_COMPRESSED_RGBA_ASTC_8x8_KHR 0x93B7
#endif

namespace mge::render {
namespace {

using assets::TextureFormat;

const char* kSpriteVs = R"(#version 300 es
// Keep in sync with engine/shaders/sprite.vert (Vulkan) and SpriteInstance.
layout(location = 0) in vec4 iAxes;
layout(location = 1) in vec2 iOrigin;
layout(location = 2) in vec4 iUv;
layout(location = 3) in vec4 iColor;
uniform vec4 uView;
out vec2 vUv;
out vec4 vColor;
void main() {
    vec2 q = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    vec2 world = iOrigin + iAxes.xy * q.x + iAxes.zw * q.y;
    gl_Position = vec4(world * uView.xy + uView.zw, 0.0, 1.0);
    vUv = vec2(mix(iUv.x, iUv.z, q.x), mix(iUv.w, iUv.y, q.y));
    vColor = iColor;
}
)";

const char* kSpriteFs = R"(#version 300 es
precision mediump float;
uniform sampler2D uTexture;
in vec2 vUv;
in vec4 vColor;
out vec4 oColor;
void main() { oColor = texture(uTexture, vUv) * vColor; }
)";

GLenum compressedEnum(TextureFormat f) {
    switch (f) {
        case TextureFormat::ETC2_RGB8: return GL_COMPRESSED_RGB8_ETC2;
        case TextureFormat::ETC2_RGBA8: return GL_COMPRESSED_RGBA8_ETC2_EAC;
        case TextureFormat::ASTC_4x4: return GL_COMPRESSED_RGBA_ASTC_4x4_KHR;
        case TextureFormat::ASTC_6x6: return GL_COMPRESSED_RGBA_ASTC_6x6_KHR;
        case TextureFormat::ASTC_8x8: return GL_COMPRESSED_RGBA_ASTC_8x8_KHR;
        case TextureFormat::RGBA8: break;
    }
    return 0;
}

bool isAstc(TextureFormat f) {
    return f == TextureFormat::ASTC_4x4 || f == TextureFormat::ASTC_6x6 || f == TextureFormat::ASTC_8x8;
}

GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        MGE_LOGE("GLES shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

class GlesRenderer final : public Renderer {
public:
    ~GlesRenderer() override { shutdown(); }
    Backend backend() const override { return Backend::GLES3; }

    bool init() {
        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display_ == EGL_NO_DISPLAY) { MGE_LOGE("eglGetDisplay failed"); return false; }
        EGLint major = 0, minor = 0;
        if (!eglInitialize(display_, &major, &minor)) {
            MGE_LOGE("eglInitialize failed: 0x%x", eglGetError());
            display_ = EGL_NO_DISPLAY;
            return false;
        }
        const EGLint attribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
            EGL_NONE};
        EGLint count = 0;
        if (!eglChooseConfig(display_, attribs, &config_, 1, &count) || count < 1) {
            MGE_LOGE("No GLES3 RGBA8 EGL config");
            return false;
        }
        eglGetConfigAttrib(display_, config_, EGL_NATIVE_VISUAL_ID, &visualId_);

        const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, ctxAttribs);
        if (context_ == EGL_NO_CONTEXT) {
            MGE_LOGE("eglCreateContext(ES3) failed: 0x%x", eglGetError());
            return false;
        }
        const char* exts = eglQueryString(display_, EGL_EXTENSIONS);
        surfaceless_ = exts && std::strstr(exts, "EGL_KHR_surfaceless_context");
        MGE_LOGI("GLES3 renderer: EGL %d.%d", major, minor);

        // Slot 0: built-in white. Real textures are uploaded once a context is current.
        Tex white;
        white.live = true;
        white.data.format = TextureFormat::RGBA8;
        white.data.width = white.data.height = white.data.mipCount = 1;
        white.data.mips[0] = {1, 1, 0, 4};
        white.data.bytes = {255, 255, 255, 255};
        textures_.push_back(std::move(white));
        return true;
    }

    bool attachWindow(ANativeWindow* window) override {
        if (!context_) return false;
        ANativeWindow_setBuffersGeometry(window, 0, 0, visualId_);
        surface_ = eglCreateWindowSurface(display_, config_, window, nullptr);
        if (surface_ == EGL_NO_SURFACE) {
            MGE_LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
            return false;
        }
        if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
            MGE_LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
            return false;
        }
        eglSwapInterval(display_, 1);
        if (!loggedGl_) {
            MGE_LOGI("GL: %s | %s", reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                     reinterpret_cast<const char*>(glGetString(GL_VERSION)));
            loggedGl_ = true;
        }
        if (!glReady_ && !createGlObjects()) return false;
        querySize();
        return true;
    }

    void detachWindow() override {
        if (surface_ == EGL_NO_SURFACE) return;
        glFinish();
        // Keep the context alive across window loss (app backgrounded) so GL objects survive.
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       surfaceless_ ? context_ : EGL_NO_CONTEXT);
        eglDestroySurface(display_, surface_);
        surface_ = EGL_NO_SURFACE;
    }

    void resize(uint32_t, uint32_t) override { querySize(); }

    void viewportSize(uint32_t& w, uint32_t& h) const override { w = uint32_t(width_); h = uint32_t(height_); }

    bool supportsFormat(TextureFormat f) const override {
        if (isAstc(f)) return astc_;   // known once a context has been current; false before
        return true;                   // RGBA8 and ETC2 are core in ES 3.0
    }

    TextureId createTexture(const assets::TextureData& data) override {
        if (data.mipCount == 0 || data.bytes.empty()) return kWhiteTexture;
        if (glReady_ && !supportsFormat(data.format)) {
            MGE_LOGW("GLES: %s not supported by this GPU, using white", assets::formatName(data.format));
            return kWhiteTexture;
        }
        uint32_t id;
        if (!freeIds_.empty()) { id = freeIds_.back(); freeIds_.pop_back(); }
        else { id = uint32_t(textures_.size()); textures_.emplace_back(); }
        Tex& t = textures_[id];
        t = Tex{};
        t.live = true;
        t.data = data;   // CPU copy: re-uploaded after a lost context
        return id;       // uploaded at the start of the next frame, on the render thread
    }

    void destroyTexture(TextureId id) override {
        if (id == kWhiteTexture || id >= textures_.size() || !textures_[id].live) return;
        Tex& t = textures_[id];
        if (t.name) pendingDelete_.push_back(t.name);   // deleted when a context is current
        t = Tex{};
        freeIds_.push_back(id);
    }

    bool renderFrame(const FrameInfo& frame) override {
        if (surface_ == EGL_NO_SURFACE || !glReady_) return false;
        querySize();  // cheap; catches rotation the host has not reported yet
        flushTextureWork();

        stats_ = {};
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width_, height_);
        glClearColor(frame.clearColor.x, frame.clearColor.y, frame.clearColor.z, frame.clearColor.w);
        glClear(GL_COLOR_BUFFER_BIT);
        if (frame.sprites) drawSprites(*frame.sprites);

        // Nothing reads the colour target back next frame: let tilers skip the load.
        const GLenum color = GL_COLOR;
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &color);
        countTextures();
        if (!eglSwapBuffers(display_, surface_)) {
            EGLint err = eglGetError();
            if (err == EGL_CONTEXT_LOST) {
                MGE_LOGW("EGL context lost, recreating");
                recreateContext();
            } else {
                MGE_LOGW("eglSwapBuffers failed: 0x%x", err);
            }
            return false;
        }
        return true;
    }

    void waitIdle() override {
        if (surface_ != EGL_NO_SURFACE) glFinish();
    }

    RenderStats lastFrameStats() const override { return stats_; }

private:
    struct Tex {
        GLuint name = 0;           // 0 = not uploaded (new, or context lost)
        bool live = false;
        assets::TextureData data;
    };

    bool createGlObjects() {
        const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        astc_ = ext && std::strstr(ext, "GL_KHR_texture_compression_astc_ldr");
        MGE_LOGI("GLES: ETC2 yes, ASTC %s", astc_ ? "yes" : "no");

        GLuint vs = compileShader(GL_VERTEX_SHADER, kSpriteVs);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, kSpriteFs);
        if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); return false; }
        program_ = glCreateProgram();
        glAttachShader(program_, vs);
        glAttachShader(program_, fs);
        glLinkProgram(program_);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(program_, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
            MGE_LOGE("GLES sprite program link failed: %s", log);
            glDeleteProgram(program_);
            program_ = 0;
            return false;
        }
        uView_ = glGetUniformLocation(program_, "uView");
        glUseProgram(program_);
        glUniform1i(glGetUniformLocation(program_, "uTexture"), 0);

        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &instanceBuffer_);
        instanceCapacity_ = 0;
        glBindVertexArray(vao_);
        for (GLuint loc = 0; loc < 4; ++loc) {
            glEnableVertexAttribArray(loc);
            glVertexAttribDivisor(loc, 1);
        }
        glBindVertexArray(0);

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_DITHER);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        // Every texture (white included) re-uploads from its CPU copy.
        for (Tex& t : textures_) t.name = 0;
        pendingDelete_.clear();
        glReady_ = true;
        return true;
    }

    void flushTextureWork() {
        if (!pendingDelete_.empty()) {
            glDeleteTextures(GLsizei(pendingDelete_.size()), pendingDelete_.data());
            pendingDelete_.clear();
        }
        for (Tex& t : textures_)
            if (t.live && !t.name) upload(t);
    }

    void upload(Tex& t) {
        const assets::TextureData& d = t.data;
        if (!supportsFormat(d.format)) {
            MGE_LOGW("GLES: %s not supported by this GPU, using white", assets::formatName(d.format));
            t.data = textures_[0].data;   // degrade to white in place; id stays valid
        }
        glGenTextures(1, &t.name);
        glBindTexture(GL_TEXTURE_2D, t.name);
        GLenum comp = compressedEnum(t.data.format);
        for (uint32_t m = 0; m < t.data.mipCount; ++m) {
            const assets::MipLevel& ml = t.data.mips[m];
            const uint8_t* px = t.data.bytes.data() + ml.offset;
            if (comp)
                glCompressedTexImage2D(GL_TEXTURE_2D, GLint(m), comp, GLsizei(ml.width), GLsizei(ml.height), 0,
                                       GLsizei(ml.size), px);
            else
                glTexImage2D(GL_TEXTURE_2D, GLint(m), GL_RGBA8, GLsizei(ml.width), GLsizei(ml.height), 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, px);
        }
        bool mips = t.data.mipCount > 1;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, GLint(t.data.mipCount - 1));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) MGE_LOGW("GLES: texture upload (%s %ux%u) error 0x%x",
                                         assets::formatName(t.data.format), t.data.width, t.data.height, err);
    }

    void drawSprites(const SpriteBatcher& batcher) {
        const auto& inst = batcher.instances();
        if (inst.empty()) return;
        const size_t bytes = inst.size() * sizeof(SpriteInstance);
        glBindBuffer(GL_ARRAY_BUFFER, instanceBuffer_);
        // Orphan + refill: the driver hands us fresh storage, so no stall on last frame's draws.
        if (bytes > instanceCapacity_) instanceCapacity_ = bytes + bytes / 2;
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(instanceCapacity_), nullptr, GL_STREAM_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(bytes), inst.data());

        const Camera2D& cam = batcher.camera();
        vec2 s = cam.scale();
        glUseProgram(program_);
        glUniform4f(uView_, s.x, s.y, -cam.center.x * s.x, -cam.center.y * s.y);
        glBindVertexArray(vao_);
        glActiveTexture(GL_TEXTURE0);

        constexpr GLsizei kStride = sizeof(SpriteInstance);
        TextureId bound = UINT32_MAX;
        for (const DrawBatch& b : batcher.batches()) {
            TextureId id = b.texture < textures_.size() && textures_[b.texture].live && textures_[b.texture].name
                               ? b.texture : kWhiteTexture;
            if (id != bound) { glBindTexture(GL_TEXTURE_2D, textures_[id].name); bound = id; }
            const size_t base = size_t(b.firstInstance) * kStride;
            auto at = [&](size_t off) { return reinterpret_cast<const void*>(base + off); };
            glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, kStride, at(offsetof(SpriteInstance, axisX)));
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kStride, at(offsetof(SpriteInstance, origin)));
            glVertexAttribPointer(2, 4, GL_UNSIGNED_SHORT, GL_TRUE, kStride, at(offsetof(SpriteInstance, uv)));
            glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, kStride, at(offsetof(SpriteInstance, color)));
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, GLsizei(b.instanceCount));
            ++stats_.drawCalls;
            stats_.sprites += b.instanceCount;
        }
        glBindVertexArray(0);
    }

    void countTextures() {
        for (const Tex& t : textures_)
            if (t.live) { ++stats_.textures; stats_.textureBytes += t.data.bytes.size(); }
    }

    void querySize() {
        EGLint w = 0, h = 0;
        eglQuerySurface(display_, surface_, EGL_WIDTH, &w);
        eglQuerySurface(display_, surface_, EGL_HEIGHT, &h);
        width_ = w;
        height_ = h;
    }

    void recreateContext() {
        // Every GL name died with the old context: forget them, never delete them.
        glReady_ = false;
        program_ = vao_ = instanceBuffer_ = 0;
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context_) eglDestroyContext(display_, context_);
        const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, ctxAttribs);
        if (context_ == EGL_NO_CONTEXT) { MGE_LOGE("GLES context recreate failed: 0x%x", eglGetError()); return; }
        if (surface_ != EGL_NO_SURFACE && eglMakeCurrent(display_, surface_, surface_, context_)) {
            eglSwapInterval(display_, 1);
            if (createGlObjects()) MGE_LOGI("GLES context restored, %zu textures queued for re-upload", textures_.size());
        }
        // With no surface, attachWindow() rebuilds the objects on the next window.
    }

    void shutdown() {
        if (display_ == EGL_NO_DISPLAY) return;
        if (glReady_ && surface_ != EGL_NO_SURFACE) {
            for (Tex& t : textures_) if (t.name) glDeleteTextures(1, &t.name);
            if (!pendingDelete_.empty()) glDeleteTextures(GLsizei(pendingDelete_.size()), pendingDelete_.data());
            if (instanceBuffer_) glDeleteBuffers(1, &instanceBuffer_);
            if (vao_) glDeleteVertexArrays(1, &vao_);
            if (program_) glDeleteProgram(program_);
        }
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
        if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);   // frees remaining objects
        eglTerminate(display_);
        surface_ = EGL_NO_SURFACE;
        context_ = EGL_NO_CONTEXT;
        display_ = EGL_NO_DISPLAY;
    }

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLint visualId_ = 0;
    EGLint width_ = 0, height_ = 0;
    bool surfaceless_ = false;
    bool loggedGl_ = false;

    bool glReady_ = false;
    bool astc_ = false;
    GLuint program_ = 0, vao_ = 0, instanceBuffer_ = 0;
    GLint uView_ = -1;
    size_t instanceCapacity_ = 0;
    std::vector<Tex> textures_;
    std::vector<uint32_t> freeIds_;
    std::vector<GLuint> pendingDelete_;
    RenderStats stats_;
};

}  // namespace

Renderer* createGlesRenderer() {
    auto* r = new GlesRenderer();
    if (!r->init()) { delete r; return nullptr; }
    return r;
}

}  // namespace mge::render

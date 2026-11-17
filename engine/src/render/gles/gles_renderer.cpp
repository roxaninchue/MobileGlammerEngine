// GLES 3.0 fallback for devices without usable Vulkan 1.1.
// Mirrors the Vulkan path: borrowed ANativeWindow, RGBA8, no depth, vsync,
// clear + invalidate so tile GPUs never load or store more than the colour target.
#include "mge/render/renderer.h"
#include "mge/log.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <cstring>

namespace mge::render {
namespace {

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
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_DITHER);
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

    bool renderFrame(const FrameInfo& frame) override {
        if (surface_ == EGL_NO_SURFACE) return false;
        querySize();  // cheap; catches rotation the host has not reported yet
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width_, height_);
        glClearColor(frame.clearColor.x, frame.clearColor.y, frame.clearColor.z, frame.clearColor.w);
        glClear(GL_COLOR_BUFFER_BIT);
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

private:
    void querySize() {
        EGLint w = 0, h = 0;
        eglQuerySurface(display_, surface_, EGL_WIDTH, &w);
        eglQuerySurface(display_, surface_, EGL_HEIGHT, &h);
        width_ = w;
        height_ = h;
    }

    void recreateContext() {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context_) eglDestroyContext(display_, context_);
        const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, ctxAttribs);
        if (context_ != EGL_NO_CONTEXT && surface_ != EGL_NO_SURFACE)
            eglMakeCurrent(display_, surface_, surface_, context_);
    }

    void shutdown() {
        if (display_ == EGL_NO_DISPLAY) return;
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
        if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
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
};

}  // namespace

Renderer* createGlesRenderer() {
    auto* r = new GlesRenderer();
    if (!r->init()) { delete r; return nullptr; }
    return r;
}

}  // namespace mge::render

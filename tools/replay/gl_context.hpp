#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstdio>

// A GLES 3.1 context on Mesa's surfaceless EGL platform with a pbuffer surface, current on the
// thread that made it.
class GlContext {
public:
    bool make(int width, int height) {
        display_ = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) return fail("no EGL display");
        eglBindAPI(EGL_OPENGL_ES_API);
        const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                            EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
        EGLConfig config;
        EGLint count = 0;
        if (!eglChooseConfig(display_, config_attributes, &config, 1, &count) || !count) return fail("no EGL config");
        const EGLint surface_attributes[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
        surface_ = eglCreatePbufferSurface(display_, config, surface_attributes);
        const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
        context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, context_attributes);
        if (surface_ == EGL_NO_SURFACE || context_ == EGL_NO_CONTEXT || !eglMakeCurrent(display_, surface_, surface_, context_))
            return fail("cannot make a GLES 3.1 context current");
        return true;
    }
    void destroy() {
        if (display_ == EGL_NO_DISPLAY) return;
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
        if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
        eglTerminate(display_);
        display_ = EGL_NO_DISPLAY;
    }

private:
    bool fail(const char* what) {
        std::fprintf(stderr, "replay: %s\n", what);
        return false;
    }
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
};

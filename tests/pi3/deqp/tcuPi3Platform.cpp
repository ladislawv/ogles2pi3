/*
 * dEQP-GLES2 on Linux on the Raspberry Pi 3 with Mesa's vc4 (tests/pi3/deqp):
 * the platform, for the hardware's own baseline (PI3_PLAN P0, §6.1). A render
 * context is an EGL OpenGL ES 2.0 context on Mesa's surfaceless platform with
 * a pbuffer as its default framebuffer (whatever surface type is asked for),
 * so a run needs no display and leaves the desktop alone. The configuration
 * is the one the A1200 runs use (ports/deqp): RGBA8, depth 24, stencil 8,
 * 4x multisampled for an ms4 configuration.
 *
 * MIT licence (LICENSE, like the rest of tests/).
 */
#include "tcuPlatform.hpp"
#include "tcuDefs.hpp"
#include "tcuRenderTarget.hpp"
#include "tcuCommandLine.hpp"
#include "gluPlatform.hpp"
#include "gluContextFactory.hpp"
#include "gluRenderContext.hpp"
#include "gluRenderConfig.hpp"
#include "glwFunctions.hpp"
#include "glwFunctionLoader.hpp"
#include "glwInitFunctions.hpp"
#include "glwEnums.hpp"
#include "deStringUtil.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>

namespace tcu
{
namespace pi3
{

class FunctionLoader : public glw::FunctionLoader
{
public:
    glw::GenericFuncType get(const char *name) const override
    {
        return (glw::GenericFuncType)eglGetProcAddress(name);
    }
};

static int value(const glu::RenderConfig &config, int glu::RenderConfig::*field, int dflt)
{
    return config.*field == glu::RenderConfig::DONT_CARE ? dflt : config.*field;
}

class RenderContext : public glu::RenderContext
{
public:
    RenderContext(EGLDisplay dpy, const glu::RenderConfig &config);
    ~RenderContext(void) override;

    glu::ContextType getType(void) const override
    {
        return m_type;
    }
    const glw::Functions &getFunctions(void) const override
    {
        return m_gl;
    }
    const tcu::RenderTarget &getRenderTarget(void) const override
    {
        return m_target;
    }
    void postIterate(void) override
    {
        eglSwapBuffers(m_dpy, m_surface);
    }
    glw::GenericFuncType getProcAddress(const char *name) const override
    {
        return (glw::GenericFuncType)eglGetProcAddress(name);
    }
    void makeCurrent(void) override
    {
        eglMakeCurrent(m_dpy, m_surface, m_surface, m_ctx);
    }

private:
    glu::ContextType m_type;
    EGLDisplay m_dpy;
    EGLSurface m_surface;
    EGLContext m_ctx;
    glw::Functions m_gl;
    tcu::RenderTarget m_target;
};

RenderContext::RenderContext(EGLDisplay dpy, const glu::RenderConfig &config)
    : m_type(config.type)
    , m_dpy(dpy)
    , m_surface(EGL_NO_SURFACE)
    , m_ctx(EGL_NO_CONTEXT)
{
    if (config.type.getAPI() != glu::ApiType::es(2, 0))
        throw tcu::NotSupportedError("pi3: OpenGL ES 2.0 only");
    if (config.componentType == glu::RenderConfig::COMPONENT_TYPE_FLOAT)
        throw tcu::NotSupportedError("pi3: no floating-point default framebuffer");

    const EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, value(config, &glu::RenderConfig::redBits, 8),
        EGL_GREEN_SIZE, value(config, &glu::RenderConfig::greenBits, 8),
        EGL_BLUE_SIZE, value(config, &glu::RenderConfig::blueBits, 8),
        EGL_ALPHA_SIZE, value(config, &glu::RenderConfig::alphaBits, 8),
        EGL_DEPTH_SIZE, value(config, &glu::RenderConfig::depthBits, 24),
        EGL_STENCIL_SIZE, value(config, &glu::RenderConfig::stencilBits, 8),
        EGL_SAMPLES, value(config, &glu::RenderConfig::numSamples, 0),
        EGL_NONE};
    EGLConfig cfg = nullptr;
    EGLint n      = 0;
    if (!eglChooseConfig(dpy, attribs, &cfg, 1, &n) || n < 1)
        throw tcu::NotSupportedError("pi3: no EGL configuration for this render config");

    const EGLint size[] = {EGL_WIDTH, value(config, &glu::RenderConfig::width, 256), EGL_HEIGHT,
                           value(config, &glu::RenderConfig::height, 256), EGL_NONE};
    m_surface           = eglCreatePbufferSurface(dpy, cfg, size);
    if (m_surface == EGL_NO_SURFACE)
        throw tcu::ResourceError("eglCreatePbufferSurface failed, " + de::toString(eglGetError()));
    const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    eglBindAPI(EGL_OPENGL_ES_API);
    m_ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (m_ctx == EGL_NO_CONTEXT)
    {
        eglDestroySurface(dpy, m_surface);
        throw tcu::ResourceError("eglCreateContext failed, " + de::toString(eglGetError()));
    }
    eglMakeCurrent(dpy, m_surface, m_surface, m_ctx);

    FunctionLoader loader;
    glw::initES20(&m_gl, &loader);
    glu::initExtensionFunctions(&m_gl, &loader, m_type.getAPI());

    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, m_surface, EGL_WIDTH, &w);
    eglQuerySurface(dpy, m_surface, EGL_HEIGHT, &h);
    int r = 0, g = 0, b = 0, a = 0, d = 0, s = 0, ns = 0;
    m_gl.getIntegerv(GL_RED_BITS, &r);
    m_gl.getIntegerv(GL_GREEN_BITS, &g);
    m_gl.getIntegerv(GL_BLUE_BITS, &b);
    m_gl.getIntegerv(GL_ALPHA_BITS, &a);
    m_gl.getIntegerv(GL_DEPTH_BITS, &d);
    m_gl.getIntegerv(GL_STENCIL_BITS, &s);
    m_gl.getIntegerv(GL_SAMPLES, &ns);
    m_target = tcu::RenderTarget(w, h, tcu::PixelFormat(r, g, b, a), d, s, ns);
}

RenderContext::~RenderContext(void)
{
    eglMakeCurrent(m_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(m_dpy, m_ctx);
    eglDestroySurface(m_dpy, m_surface);
}

class ContextFactory : public glu::ContextFactory
{
public:
    ContextFactory(EGLDisplay dpy) : glu::ContextFactory("pi3", "EGL pbuffer on Mesa's surfaceless platform"), m_dpy(dpy)
    {
    }
    glu::RenderContext *createContext(const glu::RenderConfig &config, const tcu::CommandLine &,
                                      const glu::RenderContext *sharedContext) const override
    {
        if (sharedContext)
            throw tcu::NotSupportedError("pi3: shared contexts are not offered, as on the A1200");
        return new RenderContext(m_dpy, config);
    }

private:
    EGLDisplay m_dpy;
};

class Platform : public tcu::Platform, private glu::Platform
{
public:
    Platform(void)
    {
        PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        m_dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
                            : EGL_NO_DISPLAY;
        if (m_dpy == EGL_NO_DISPLAY || !eglInitialize(m_dpy, nullptr, nullptr))
            throw tcu::ResourceError("no surfaceless EGL display");
        m_contextFactoryRegistry.registerFactory(new ContextFactory(m_dpy));
    }
    ~Platform(void) override
    {
        eglTerminate(m_dpy);
    }
    const glu::Platform &getGLPlatform(void) const override
    {
        return static_cast<const glu::Platform &>(*this);
    }

private:
    EGLDisplay m_dpy;
};

} // namespace pi3
} // namespace tcu

tcu::Platform *createPlatform(void)
{
    return new tcu::pi3::Platform();
}

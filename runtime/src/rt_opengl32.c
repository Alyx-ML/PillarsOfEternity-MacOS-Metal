/*
 * OPENGL32: the system has no OpenGL. macOS graphics are Metal only.
 *
 * The result is that of Windows on a system without an OpenGL driver:
 *   - GDI32 finds no pixel format (rt_gdi32.c), so wglCreateContext makes
 *     no context. The wgl functions fail, and wglGetCurrentContext and
 *     wglGetCurrentDC return NULL.
 *   - The gl functions are called without a current context. On Windows,
 *     OPENGL32 then uses its table of functions that do nothing: the
 *     functions change no state, write no output, and return 0 (glGetError
 *     returns GL_NO_ERROR, glGetString returns NULL).
 */
#include "rt.h"
#include "rt_gdi32.h"

/* A gl function without a current context: no operation. The functions
   that return a value return 0. */
#define GL_NO_CONTEXT(name)                                                                                            \
    void rt_##name(_cpu *cpu);                                                                                         \
    void rt_##name(_cpu *cpu)                                                                                          \
    {                                                                                                                  \
        RET(0);                                                                                                        \
    }

GL_NO_CONTEXT(glAlphaFunc)
GL_NO_CONTEXT(glBegin)
GL_NO_CONTEXT(glBindTexture)
GL_NO_CONTEXT(glBlendFunc)
GL_NO_CONTEXT(glClear)
GL_NO_CONTEXT(glClearColor)
GL_NO_CONTEXT(glClearDepth)
GL_NO_CONTEXT(glClearStencil)
GL_NO_CONTEXT(glColor4f)
GL_NO_CONTEXT(glColor4fv)
GL_NO_CONTEXT(glColorMask)
GL_NO_CONTEXT(glColorMaterial)
GL_NO_CONTEXT(glColorPointer)
GL_NO_CONTEXT(glCopyTexSubImage2D)
GL_NO_CONTEXT(glCullFace)
GL_NO_CONTEXT(glDeleteTextures)
GL_NO_CONTEXT(glDepthFunc)
GL_NO_CONTEXT(glDepthMask)
GL_NO_CONTEXT(glDisable)
GL_NO_CONTEXT(glDisableClientState)
GL_NO_CONTEXT(glDrawArrays)
GL_NO_CONTEXT(glDrawBuffer)
GL_NO_CONTEXT(glDrawElements)
GL_NO_CONTEXT(glEnable)
GL_NO_CONTEXT(glEnableClientState)
GL_NO_CONTEXT(glEnd)
GL_NO_CONTEXT(glFinish)
GL_NO_CONTEXT(glFogf)
GL_NO_CONTEXT(glFogfv)
GL_NO_CONTEXT(glFogi)
GL_NO_CONTEXT(glFrontFace)
GL_NO_CONTEXT(glGenTextures)
GL_NO_CONTEXT(glGetError)
GL_NO_CONTEXT(glGetFloatv)
GL_NO_CONTEXT(glGetIntegerv)
GL_NO_CONTEXT(glGetString)
GL_NO_CONTEXT(glGetTexParameteriv)
GL_NO_CONTEXT(glHint)
GL_NO_CONTEXT(glIsEnabled)
GL_NO_CONTEXT(glIsTexture)
GL_NO_CONTEXT(glLightModelf)
GL_NO_CONTEXT(glLightModelfv)
GL_NO_CONTEXT(glLightModeli)
GL_NO_CONTEXT(glLightf)
GL_NO_CONTEXT(glLightfv)
GL_NO_CONTEXT(glLoadIdentity)
GL_NO_CONTEXT(glLoadMatrixf)
GL_NO_CONTEXT(glMaterialf)
GL_NO_CONTEXT(glMaterialfv)
GL_NO_CONTEXT(glMatrixMode)
GL_NO_CONTEXT(glMultMatrixf)
GL_NO_CONTEXT(glNormal3f)
GL_NO_CONTEXT(glNormalPointer)
GL_NO_CONTEXT(glPixelStorei)
GL_NO_CONTEXT(glPolygonMode)
GL_NO_CONTEXT(glPolygonOffset)
GL_NO_CONTEXT(glReadBuffer)
GL_NO_CONTEXT(glReadPixels)
GL_NO_CONTEXT(glScissor)
GL_NO_CONTEXT(glStencilFunc)
GL_NO_CONTEXT(glStencilMask)
GL_NO_CONTEXT(glStencilOp)
GL_NO_CONTEXT(glTexCoordPointer)
GL_NO_CONTEXT(glTexEnvf)
GL_NO_CONTEXT(glTexEnvfv)
GL_NO_CONTEXT(glTexEnvi)
GL_NO_CONTEXT(glTexGenfv)
GL_NO_CONTEXT(glTexGeni)
GL_NO_CONTEXT(glTexImage2D)
GL_NO_CONTEXT(glTexParameteri)
GL_NO_CONTEXT(glTexSubImage2D)
GL_NO_CONTEXT(glVertex3f)
GL_NO_CONTEXT(glVertexPointer)
GL_NO_CONTEXT(glViewport)

/* HGLRC wglCreateContext(HDC): a DC has no pixel format. */
void rt_wglCreateContext(_cpu *cpu)
{
    rt_set_last_error(rt_gdi32_is_dc(ARG(0)) ? ERROR_INVALID_PIXEL_FORMAT : ERROR_INVALID_HANDLE);
    RET(0);
}

/* BOOL wglDeleteContext(HGLRC): no context exists. */
void rt_wglDeleteContext(_cpu *cpu)
{
    rt_set_last_error(ERROR_INVALID_HANDLE);
    RET(0);
}

/* HGLRC wglGetCurrentContext(void) */
void rt_wglGetCurrentContext(_cpu *cpu)
{
    RET(0);
}

/* HDC wglGetCurrentDC(void) */
void rt_wglGetCurrentDC(_cpu *cpu)
{
    RET(0);
}

/* PROC wglGetProcAddress(LPCSTR): no current context. */
void rt_wglGetProcAddress(_cpu *cpu)
{
    rt_set_last_error(ERROR_PROC_NOT_FOUND);
    RET(0);
}

/* BOOL wglMakeCurrent(HDC, HGLRC): with HGLRC NULL, the thread has no
   current context, which is true. Any other HGLRC is not a context. */
void rt_wglMakeCurrent(_cpu *cpu)
{
    if (ARG(1) == 0)
    {
        RET(1);
        return;
    }
    rt_set_last_error(ERROR_INVALID_HANDLE);
    RET(0);
}

/* BOOL wglShareLists(HGLRC, HGLRC): no context exists. */
void rt_wglShareLists(_cpu *cpu)
{
    rt_set_last_error(ERROR_INVALID_HANDLE);
    RET(0);
}

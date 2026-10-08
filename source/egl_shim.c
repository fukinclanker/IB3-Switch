#include "egl_shim.h"

#include <stdint.h>

#include "config.h"
#include "controller_input.h"
#include "util.h"

static uint64_t draw_calls;
static GLint viewport_x;
static GLint viewport_y;
static GLsizei viewport_width = -1;
static GLsizei viewport_height = -1;

EGLDisplay eglGetDisplay_fake(EGLNativeDisplayType display_id) {
  EGLDisplay result = eglGetDisplay(display_id);
  debugPrintf("EGL: eglGetDisplay(%p) -> %p\n", (void *)display_id,
              (void *)result);
  return result;
}

EGLBoolean eglInitialize_fake(EGLDisplay display, EGLint *major, EGLint *minor) {
  EGLBoolean result = eglInitialize(display, major, minor);
  debugPrintf("EGL: eglInitialize(%p) -> %u version=%d.%d\n", (void *)display,
              (unsigned)result, major ? *major : -1, minor ? *minor : -1);
  return result;
}

static void log_config(EGLDisplay display, EGLConfig config) {
  EGLint r = 0, g = 0, b = 0, a = 0, depth = 0, stencil = 0, samples = 0;
  eglGetConfigAttrib(display, config, EGL_RED_SIZE, &r);
  eglGetConfigAttrib(display, config, EGL_GREEN_SIZE, &g);
  eglGetConfigAttrib(display, config, EGL_BLUE_SIZE, &b);
  eglGetConfigAttrib(display, config, EGL_ALPHA_SIZE, &a);
  eglGetConfigAttrib(display, config, EGL_DEPTH_SIZE, &depth);
  eglGetConfigAttrib(display, config, EGL_STENCIL_SIZE, &stencil);
  eglGetConfigAttrib(display, config, EGL_SAMPLES, &samples);
  debugPrintf("EGL: chosen config RGBA%d%d%d%d depth=%d stencil=%d samples=%d\n",
              r, g, b, a, depth, stencil, samples);
}

/* EGL sorts configs by *smallest* depth that satisfies the request, so a
 * request for a 16-bit depth buffer gets exactly that on Switch Mesa, and the
 * 3D scenes z-fight (flickering, surfaces bleeding through each other). Ask
 * for 24-bit depth (and an 8-bit stencil when any stencil is requested), and
 * fall back to the game's own request if no such config exists. */
#define MAX_EGL_ATTRIBS 64

EGLBoolean eglChooseConfig_fake(EGLDisplay display, const EGLint *attribs,
                                EGLConfig *configs, EGLint config_size,
                                EGLint *num_config) {
  EGLint upgraded[MAX_EGL_ATTRIBS * 2 + 1];
  int changed = 0;
  int count = 0;
  if (attribs) {
    while (count < MAX_EGL_ATTRIBS && attribs[count * 2] != EGL_NONE) {
      EGLint key = attribs[count * 2];
      EGLint value = attribs[count * 2 + 1];
      if (key == EGL_DEPTH_SIZE && value > 0 && value < 24) {
        value = 24;
        changed = 1;
      } else if (key == EGL_STENCIL_SIZE && value > 0 && value < 8) {
        value = 8;
        changed = 1;
      }
      upgraded[count * 2] = key;
      upgraded[count * 2 + 1] = value;
      count++;
    }
    if (attribs[count * 2] != EGL_NONE)
      changed = 0; /* unexpectedly long list: leave it alone */
  }
  upgraded[count * 2] = EGL_NONE;

  EGLBoolean result = EGL_FALSE;
  if (changed) {
    EGLint found = 0;
    result = eglChooseConfig(display, upgraded, configs, config_size, &found);
    if (result == EGL_TRUE && found > 0) {
      if (num_config)
        *num_config = found;
      debugPrintf("EGL: depth/stencil request raised to 24/8\n");
    } else {
      result = EGL_FALSE;
    }
  }
  if (result != EGL_TRUE)
    result = eglChooseConfig(display, attribs, configs, config_size,
                             num_config);
  if (result == EGL_TRUE && configs && config_size > 0 && num_config &&
      *num_config > 0)
    log_config(display, configs[0]);
  debugPrintf("EGL: eglChooseConfig(display=%p size=%d) -> %u count=%d first=%p\n",
              (void *)display, config_size, (unsigned)result,
              num_config ? *num_config : -1,
              (configs && config_size > 0) ? (void *)configs[0] : NULL);
  return result;
}

EGLContext eglCreateContext_fake(EGLDisplay display, EGLConfig config,
                                 EGLContext share_context,
                                 const EGLint *attribs) {
  EGLContext result = eglCreateContext(display, config, share_context, attribs);
  debugPrintf("EGL: eglCreateContext(display=%p config=%p share=%p) -> %p\n",
              (void *)display, (void *)config, (void *)share_context,
              (void *)result);
  return result;
}

EGLSurface eglCreateWindowSurface_fake(EGLDisplay display, EGLConfig config,
                                       EGLNativeWindowType window,
                                       const EGLint *attribs) {
  EGLSurface result = eglCreateWindowSurface(display, config, window, attribs);
  debugPrintf("EGL: eglCreateWindowSurface(display=%p config=%p window=%p) -> %p\n",
              (void *)display, (void *)config, (void *)window, (void *)result);
  return result;
}

EGLBoolean eglMakeCurrent_fake(EGLDisplay display, EGLSurface draw,
                               EGLSurface read, EGLContext context) {
  EGLBoolean result = eglMakeCurrent(display, draw, read, context);
  debugPrintf("EGL: eglMakeCurrent(display=%p draw=%p read=%p context=%p) -> %u\n",
              (void *)display, (void *)draw, (void *)read, (void *)context,
              (unsigned)result);
  return result;
}

EGLBoolean eglQuerySurface_fake(EGLDisplay display, EGLSurface surface,
                                EGLint attribute, EGLint *value) {
  EGLBoolean result = eglQuerySurface(display, surface, attribute, value);
  const EGLint native_value = value ? *value : -1;

  /* Switch Mesa reports zero for the dimensions of a window surface.  UE3
   * treats that as an actual 0x0 drawable, builds a zero-width canvas, and
   * eventually crashes while wrapping subtitles into it. */
  if (result == EGL_TRUE && value && *value <= 0) {
    if (attribute == EGL_WIDTH)
      *value = screen_width;
    else if (attribute == EGL_HEIGHT)
      *value = screen_height;
  }

  static unsigned logs;
  if (logs++ < 12 || (native_value > 0 && value && native_value != *value))
    debugPrintf("EGL: eglQuerySurface(surface=%p attr=0x%x) -> %u native=%d effective=%d\n",
                (void *)surface, attribute, (unsigned)result, native_value,
                value ? *value : -1);
  return result;
}

EGLBoolean eglSwapInterval_fake(EGLDisplay display, EGLint interval) {
  /* Interval 0 presents mid-scanout on Switch: visible tearing and uneven
   * frame pacing. Always wait for at least one vblank. */
  if (interval < 1)
    interval = 1;
  EGLBoolean result = eglSwapInterval(display, interval);
  debugPrintf("EGL: eglSwapInterval(display=%p interval=%d) -> %u\n",
              (void *)display, interval, (unsigned)result);
  return result;
}

EGLBoolean eglSwapBuffers_fake(EGLDisplay display, EGLSurface surface) {
  controller_input_draw_cursor();
  EGLBoolean result = eglSwapBuffers(display, surface);
  static uint64_t swaps;
  static uint64_t window_draws;      /* draw calls since last periodic log */
  static uint64_t window_max;        /* busiest single frame in that window */
  static uint64_t total_draws;
  static int first_draw_logged;
  swaps++;
  window_draws += draw_calls;
  total_draws += draw_calls;
  if (draw_calls > window_max)
    window_max = draw_calls;
  if (draw_calls && !first_draw_logged) {
    first_draw_logged = 1;
    debugPrintf("EGL: FIRST DRAWS at swap %llu: %llu draw calls, viewport=%d,%d %dx%d\n",
                (unsigned long long)swaps, (unsigned long long)draw_calls,
                viewport_x, viewport_y, viewport_width, viewport_height);
  }
  if (swaps <= 12 || swaps % 120 == 0) {
    debugPrintf("EGL: swap %llu draws(this=%llu window=%llu max=%llu total=%llu) viewport=%d,%d %dx%d\n",
                (unsigned long long)swaps, (unsigned long long)draw_calls,
                (unsigned long long)window_draws, (unsigned long long)window_max,
                (unsigned long long)total_draws, viewport_x, viewport_y,
                viewport_width, viewport_height);
    window_draws = 0;
    window_max = 0;
  }
  draw_calls = 0;
  return result;
}

void glDrawArrays_fake(GLenum mode, GLint first, GLsizei count) {
  draw_calls++;
  glDrawArrays(mode, first, count);
}

void glDrawElements_fake(GLenum mode, GLsizei count, GLenum type,
                         const void *indices) {
  draw_calls++;
  glDrawElements(mode, count, type, indices);
}

void glViewport_fake(GLint x, GLint y, GLsizei width, GLsizei height) {
  if (x != viewport_x || y != viewport_y || width != viewport_width ||
      height != viewport_height) {
    static unsigned logs;
    if (logs++ < 20)
      debugPrintf("GL: glViewport(%d, %d, %d, %d)\n", x, y, width, height);
    viewport_x = x;
    viewport_y = y;
    viewport_width = width;
    viewport_height = height;
  }
  glViewport(x, y, width, height);
}

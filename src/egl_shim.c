/*
 * egl_shim.c -- EGL wrapper backed by SDL2 (OpenGL ES 2.0)
 *
 * PowerVR GE8300: only ONE GL context can be current at a time, period.
 *
 * Game architecture:
 *   - AndroidMain: holds GL for entire critical sections (loading/frame prep)
 *   - renderThread_main: does all rendering inside critical sections
 *   - BeginCriticalSectionGL: locks recursive mutex2, calls eglMakeCurrent
 *     on first entry (lock_count 0→1)
 *   - EndCriticalSectionGL: decrements lock_count, unlocks mutex2,
 *     but NEVER calls eglMakeCurrent(NULL) — each thread keeps its context
 *
 * Strategy: Single SDL GL context. Hook pthread_mutex_unlock to detect
 * outermost EndCriticalSectionGL. Release GL at that point so the other
 * thread can acquire it. The game's own mutex2 serializes all GL access.
 */

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "egl_shim.h"
#include "util.h"

#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720

typedef struct {
  EGLBoolean is_pbuffer;
} _egl_context;

static SDL_Window *egl_window = NULL;
static SDL_GLContext egl_sdl_context = NULL;

/* GL ownership tracking */
static pthread_mutex_t gl_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gl_available_cond = PTHREAD_COND_INITIALIZER;
static pthread_t gl_owner;
static int gl_owned = 0;
static int frame_count = 0;

static _Thread_local _egl_context *current_context = NULL;
static _Thread_local int has_real_gl = 0;

/* Mutex-hook state: detect outermost EndCriticalSectionGL */
static _Thread_local void *last_locked_mutex = NULL;
static _Thread_local void *gl_critical_mutex = NULL;
static _Thread_local int gl_critical_depth = 0;

SDL_Window *egl_shim_get_window(void) { return egl_window; }

void egl_shim_create_window(void) {
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  egl_window = SDL_CreateWindow(
      "LEGO Star Wars: TCS", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      SCREEN_WIDTH, SCREEN_HEIGHT,
      SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
  if (!egl_window) {
    debugPrintf("egl_shim: SDL_CreateWindow FAILED: %s\n", SDL_GetError());
    return;
  }
  debugPrintf("egl_shim: Window created %dx%d\n", SCREEN_WIDTH, SCREEN_HEIGHT);

  egl_sdl_context = SDL_GL_CreateContext(egl_window);
  if (!egl_sdl_context) {
    debugPrintf("egl_shim: SDL_GL_CreateContext FAILED: %s\n", SDL_GetError());
    return;
  }
  debugPrintf("egl_shim: GL context created\n");

  /* GL test */
  glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  SDL_GL_SwapWindow(egl_window);
  debugPrintf("egl_shim: GL TEST -- RED for 2s\n");
  SDL_Delay(2000);

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  SDL_GL_SwapWindow(egl_window);

  /* Release context for game threads */
  SDL_GL_MakeCurrent(egl_window, NULL);
  debugPrintf("egl_shim: Context released, ready for game\n");
}

/* Acquire GL for the calling thread. Waits if another thread has it. */
static int gl_acquire(const char *surf_type, int mc) {
  pthread_mutex_lock(&gl_mutex);

  if (gl_owned && pthread_equal(gl_owner, pthread_self())) {
    /* Already own it */
    pthread_mutex_unlock(&gl_mutex);
    has_real_gl = 1;
    return 1;
  }

  /* Wait for GL to become free */
  int waited = 0;
  while (gl_owned) {
    if (!waited) {
      debugPrintf("egl_shim: MakeCurrent #%d %s [tid=%lx] waiting (GL held by %lx)\n",
                  mc, surf_type, (unsigned long)pthread_self(), (unsigned long)gl_owner);
    }
    waited = 1;
    pthread_cond_wait(&gl_available_cond, &gl_mutex);
  }

  /* GL is free — bind it to this thread */
  int ret = SDL_GL_MakeCurrent(egl_window, egl_sdl_context);
  if (ret == 0) {
    gl_owned = 1;
    gl_owner = pthread_self();
    has_real_gl = 1;
    pthread_mutex_unlock(&gl_mutex);
    static _Thread_local int acq_log = 0;
    if (acq_log < 10 || mc % 500 == 0) {
      debugPrintf("egl_shim: MakeCurrent #%d %s [tid=%lx] ACQUIRED%s\n",
                  mc, surf_type, (unsigned long)pthread_self(),
                  waited ? " (after wait)" : "");
      acq_log++;
    }
    return 1;
  } else {
    pthread_mutex_unlock(&gl_mutex);
    has_real_gl = 0;
    debugPrintf("egl_shim: MakeCurrent #%d %s [tid=%lx] SDL FAILED: %s\n",
                mc, surf_type, (unsigned long)pthread_self(), SDL_GetError());
    return 0;
  }
}

/* Release GL from the calling thread and wake waiters. */
static void gl_release(const char *reason) {
  pthread_mutex_lock(&gl_mutex);
  if (gl_owned && pthread_equal(gl_owner, pthread_self())) {
    SDL_GL_MakeCurrent(egl_window, NULL);
    gl_owned = 0;
    has_real_gl = 0;
    pthread_cond_broadcast(&gl_available_cond);
    static int rel_log = 0;
    if (rel_log < 20 || frame_count % 120 == 0) {
      debugPrintf("egl_shim: GL released [tid=%lx] reason=%s\n",
                  (unsigned long)pthread_self(), reason);
      rel_log++;
    }
  }
  pthread_mutex_unlock(&gl_mutex);
}

/* --- Mutex hooks (called from imports.c pthread wrappers) --- */

void egl_shim_on_mutex_post_lock(void *mutex_id) {
  last_locked_mutex = mutex_id;
  if (gl_critical_mutex && mutex_id == gl_critical_mutex) {
    gl_critical_depth++;
    static _Thread_local int lock_log = 0;
    if (lock_log < 10) {
      debugPrintf("egl_shim: mutex_hook lock depth=%d [tid=%lx] mutex=%p\n",
                  gl_critical_depth, (unsigned long)pthread_self(), mutex_id);
      lock_log++;
    }
  }
}

void egl_shim_on_mutex_pre_unlock(void *mutex_id) {
  if (gl_critical_mutex && mutex_id == gl_critical_mutex) {
    gl_critical_depth--;
    static _Thread_local int unlock_log = 0;
    if (unlock_log < 10) {
      debugPrintf("egl_shim: mutex_hook unlock depth=%d [tid=%lx] mutex=%p\n",
                  gl_critical_depth, (unsigned long)pthread_self(), mutex_id);
      unlock_log++;
    }
    if (gl_critical_depth == 0) {
      /* Outermost EndCriticalSectionGL — release GL */
      gl_release("EndCriticalSection");
      gl_critical_mutex = NULL;
    }
  }
}

/* --- EGL API --- */

EGLDisplay egl_shim_GetDisplay(EGLNativeDisplayType display_id) {
  (void)display_id;
  debugPrintf("egl_shim: eglGetDisplay()\n");
  return (EGLDisplay)strdup("display");
}

EGLBoolean egl_shim_Initialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
  (void)dpy;
  if (major) *major = 1;
  if (minor) *minor = 4;
  debugPrintf("egl_shim: eglInitialize() -> 1.4\n");
  return EGL_TRUE;
}

EGLBoolean egl_shim_Terminate(EGLDisplay dpy) {
  (void)dpy;
  debugPrintf("egl_shim: eglTerminate()\n");
  if (egl_sdl_context) {
    SDL_GL_DeleteContext(egl_sdl_context);
    egl_sdl_context = NULL;
  }
  if (egl_window) {
    SDL_DestroyWindow(egl_window);
    egl_window = NULL;
  }
  return EGL_TRUE;
}

EGLBoolean egl_shim_ChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                                  EGLConfig *configs, EGLint config_size,
                                  EGLint *num_config) {
  (void)dpy; (void)attrib_list;
  debugPrintf("egl_shim: eglChooseConfig()\n");
  if (configs && config_size > 0)
    configs[0] = (EGLConfig)strdup("config");
  if (num_config)
    *num_config = 1;
  return EGL_TRUE;
}

EGLSurface egl_shim_CreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                         EGLNativeWindowType win,
                                         const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)win; (void)attrib_list;
  EGLSurface s = (EGLSurface)strdup("window");
  debugPrintf("egl_shim: eglCreateWindowSurface() -> %p\n", s);
  return s;
}

EGLSurface egl_shim_CreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                          const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)attrib_list;
  EGLSurface s = (EGLSurface)strdup("pbuffer");
  debugPrintf("egl_shim: eglCreatePbufferSurface() -> %p\n", s);
  return s;
}

EGLContext egl_shim_CreateContext(EGLDisplay dpy, EGLConfig config,
                                  EGLContext share_context,
                                  const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)share_context; (void)attrib_list;
  EGLContext c = (EGLContext)calloc(1, sizeof(_egl_context));
  debugPrintf("egl_shim: eglCreateContext(share=%p) -> %p\n", share_context, c);
  return c;
}

EGLBoolean egl_shim_MakeCurrent(EGLDisplay dpy, EGLSurface draw,
                                 EGLSurface read, EGLContext ctx) {
  (void)dpy; (void)read;

  _egl_context *context = (_egl_context *)ctx;
  static _Thread_local int mc_count = 0;
  int mc = ++mc_count;

  /* === UNBIND === */
  if (context == NULL || draw == NULL) {
    current_context = NULL;
    gl_critical_mutex = NULL;
    gl_critical_depth = 0;
    if (egl_window)
      gl_release("eglMakeCurrent(NULL)");
    has_real_gl = 0;
    return EGL_TRUE;
  }

  int is_window = (((char *)draw)[0] == 'w');
  context->is_pbuffer = is_window ? EGL_FALSE : EGL_TRUE;
  current_context = context;

  if (!egl_window || !egl_sdl_context)
    return EGL_TRUE;

  /* Acquire GL (waits if another thread has it) */
  gl_acquire(is_window ? "WINDOW" : "PBUFFER", mc);

  /* Track this thread's critical section depth.
   * BeginCriticalSectionGL locks mutex2 THEN calls eglMakeCurrent.
   * So last_locked_mutex is mutex2's address. */
  if (gl_critical_mutex == NULL && last_locked_mutex != NULL) {
    gl_critical_mutex = last_locked_mutex;
    gl_critical_depth = 1;
    debugPrintf("egl_shim: tracking mutex %p as GL critical section [tid=%lx]\n",
                gl_critical_mutex, (unsigned long)pthread_self());
  } else if (gl_critical_mutex == NULL) {
    debugPrintf("egl_shim: WARNING no last_locked_mutex for GL tracking [tid=%lx]\n",
                (unsigned long)pthread_self());
  }

  return EGL_TRUE;
}

EGLBoolean egl_shim_SwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy; (void)surface;
  if (!egl_window) return EGL_TRUE;

  if (has_real_gl) {
    SDL_GL_SwapWindow(egl_window);
    int fc = ++frame_count;
    if (fc <= 10 || fc % 60 == 0) {
      debugPrintf("egl_shim: SwapBuffers #%d [tid=%lx]\n",
                  fc, (unsigned long)pthread_self());
    }
  } else {
    static int noswap_log = 0;
    if (noswap_log < 3) {
      debugPrintf("egl_shim: SwapBuffers SKIPPED (no real GL) [tid=%lx]\n",
                  (unsigned long)pthread_self());
      noswap_log++;
    }
  }
  return EGL_TRUE;
}

EGLBoolean egl_shim_DestroySurface(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy;
  free(surface);
  return EGL_TRUE;
}

EGLBoolean egl_shim_DestroyContext(EGLDisplay dpy, EGLContext ctx) {
  (void)dpy;
  free(ctx);
  return EGL_TRUE;
}

EGLBoolean egl_shim_QuerySurface(EGLDisplay dpy, EGLSurface surface,
                                  EGLint attribute, EGLint *value) {
  (void)dpy; (void)surface;
  if (attribute == 0x3057 && value) *value = SCREEN_WIDTH;
  else if (attribute == 0x3056 && value) *value = SCREEN_HEIGHT;
  return EGL_TRUE;
}

EGLBoolean egl_shim_GetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                                     EGLint attribute, EGLint *value) {
  (void)dpy; (void)config;
  if (!value) return EGL_TRUE;
  switch (attribute) {
  case 0x3020: *value = 8; break;
  case 0x3021: *value = 8; break;
  case 0x3022: *value = 8; break;
  case 0x3023: *value = 0; break;
  case 0x3025: *value = 24; break;
  case 0x3026: *value = 8; break;
  default: *value = 0; break;
  }
  return EGL_TRUE;
}

EGLint egl_shim_GetError(void) { return EGL_SUCCESS; }

void *egl_shim_GetProcAddress(const char *procname) {
  void *ptr = SDL_GL_GetProcAddress(procname);
  if (ptr) return ptr;

  size_t len = strlen(procname);
  if (len > 3 && strcmp(procname + len - 3, "OES") == 0) {
    char stripped[256];
    if (len - 3 < sizeof(stripped)) {
      memcpy(stripped, procname, len - 3);
      stripped[len - 3] = '\0';
      ptr = SDL_GL_GetProcAddress(stripped);
      if (ptr) return ptr;
    }
  }

  debugPrintf("egl_shim: eglGetProcAddress(%s) -> NOT FOUND\n", procname);
  return NULL;
}

EGLBoolean egl_shim_BindAPI(unsigned int api) {
  (void)api;
  return EGL_TRUE;
}

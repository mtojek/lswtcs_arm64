/*
 * imports.c -- .so import resolution for LSWTCS ARM64
 *
 * Maps all 286 undefined symbols from libTTapp.so to real
 * libc/GL/EGL functions or our shim implementations.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>

#include "android_shim.h"
#include "egl_shim.h"
#include "opensles_shim.h"
#include "so_util.h"
#include "util.h"

extern uintptr_t __cxa_atexit;
extern uintptr_t __cxa_finalize;

FILE *stderr_fake = (FILE *)0x1337;

static uint8_t fake_sF[3][0x100];
static uint64_t __stack_chk_guard_fake = 0x4242424242424242;

/* Vita-style: just log and return — no abort, no loop */
static void __stack_chk_fail_stub(void) {
  debugPrintf("__stack_chk_fail called from %p\n", __builtin_return_address(0));
}

/* errno compat */
static int *__errno_fake(void) { return &errno; }

/* __android_log */
int __android_log_print_fake(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
  va_list list;
  static char string[0x1000];
  va_start(list, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);
  debugPrintf("LOG [%s]: %s\n", tag, string);
  return 0;
}

/* fortified libc stubs */
void *__memcpy_chk(void *dst, const void *src, size_t n, size_t dst_len) {
  (void)dst_len;
  return memcpy(dst, src, n);
}

void *__memmove_chk(void *dst, const void *src, size_t n, size_t dst_len) {
  (void)dst_len;
  return memmove(dst, src, n);
}

void *__memset_chk(void *dst, int c, size_t n, size_t dst_len) {
  (void)dst_len;
  return memset(dst, c, n);
}

char *__strcat_chk(char *dst, const char *src, size_t dst_buf_size) {
  (void)dst_buf_size;
  return strcat(dst, src);
}

char *__strcpy_chk(char *dst, const char *src, size_t dst_len) {
  (void)dst_len;
  return strcpy(dst, src);
}

size_t __strlen_chk(const char *s, size_t max_len) {
  (void)max_len;
  return strlen(s);
}

char *__strrchr_chk(const char *s, int c, size_t n) {
  (void)n;
  return strrchr(s, c);
}

int __vsprintf_chk(char *dst, int flags, size_t dst_len_from_compiler,
                    const char *fmt, va_list ap) {
  (void)flags;
  (void)dst_len_from_compiler;
  return vsprintf(dst, fmt, ap);
}

int __vsnprintf_chk(char *dst, size_t supplied_size, int flags,
                     size_t dst_len_from_compiler, const char *fmt,
                     va_list ap) {
  (void)flags;
  (void)dst_len_from_compiler;
  return vsnprintf(dst, supplied_size, fmt, ap);
}

ssize_t __read_chk(int fd, void *buf, size_t count, size_t buf_size) {
  (void)buf_size;
  return read(fd, buf, count);
}

int __open_2(const char *pathname, int flags) {
  int fd = open(pathname, flags);
  if (strncmp(pathname, "/dev/", 5) != 0) {
    debugPrintf("open(\"%s\", 0x%x) = %d\n", pathname, flags, fd);
  }
  return fd;
}

/* open() wrapper for debugging — skip /dev/ spam */
int open_fake(const char *pathname, int flags, ...) {
  int fd = open(pathname, flags);
  if (strncmp(pathname, "/dev/", 5) != 0) {
    if (fd >= 0)
      debugPrintf("open(\"%s\", 0x%x) = %d\n", pathname, flags, fd);
    else
      debugPrintf("open(\"%s\", 0x%x) = %d (errno=%d: %s)\n",
                  pathname, flags, fd, errno, strerror(errno));
  }
  return fd;
}

/* ctype compat */
size_t __ctype_get_mb_cur_max_fake(void) { return 4; }

/* dl_iterate_phdr stub */
int dl_iterate_phdr_fake(void *callback, void *data) {
  (void)callback;
  (void)data;
  return 0;
}

/* android_set_abort_message stub */
void android_set_abort_message_fake(const char *msg) {
  debugPrintf("android_set_abort_message: %s\n", msg ? msg : "(null)");
}

/* dlopen/dlsym stubs — game may dynamically load libGLESv2.so etc. */
void *dlopen_fake(const char *filename, int flags) {
  debugPrintf("dlopen(\"%s\", %d)\n", filename ? filename : "(null)", flags);
  return (void *)0xDEAD0001; /* non-NULL dummy handle */
}

void *dlsym_fake(void *handle, const char *symbol) {
  debugPrintf("dlsym(%p, \"%s\")\n", handle, symbol);
  /* Try SDL GL proc address first (covers GL/EGL extensions) */
  void *ptr = SDL_GL_GetProcAddress(symbol);
  if (ptr) return ptr;
  debugPrintf("dlsym: NOT FOUND: %s\n", symbol);
  return NULL;
}

int dlclose_fake(void *handle) { (void)handle; return 0; }
char *dlerror_fake(void) { return NULL; }
int dladdr_fake(void *addr, void *info) { (void)addr; (void)info; return 0; }

/* getenv/setenv stubs */
char *getenv_fake(const char *name) {
  debugPrintf("getenv(\"%s\") -> NULL\n", name);
  return NULL;
}

int setenv_fake(const char *name, const char *value, int overwrite) {
  (void)name; (void)value; (void)overwrite;
  return 0;
}

/* __system_property_get stub */
int __system_property_get_fake(const char *name, char *value) {
  debugPrintf("__system_property_get(\"%s\")\n", name);
  value[0] = '\0';
  return 0;
}

/* Vita-style: log then call real function */
void abort_fake(void) {
  debugPrintf("abort() called from %p\n", __builtin_return_address(0));
  abort();
}

void exit_fake(int status) {
  debugPrintf("exit(%d) called from %p\n", status, __builtin_return_address(0));
  _exit(status);
}

/* Vita-style: stub sigaction — game shouldn't install signal handlers */
int sigaction_fake(int signum, const void *act, void *oldact) {
  (void)signum; (void)act; (void)oldact;
  return 0;
}

/* fopen wrapper for debugging */
FILE *fopen_fake(const char *filename, const char *mode) {
  FILE *f = fopen(filename, mode);
  if (!f)
    debugPrintf("fopen(\"%s\", \"%s\") = NULL (errno=%d: %s)\n",
                filename, mode, errno, strerror(errno));
  else
    debugPrintf("fopen(\"%s\", \"%s\") = %p\n", filename, mode, f);
  return f;
}

/* pthread wrappers (bionic struct sizes differ from glibc) */
int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *mutexattr) {
  (void)mutexattr;
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return -1;
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  int ret = pthread_mutex_init(m, &attr);
  pthread_mutexattr_destroy(&attr);
  if (ret < 0) { free(m); return -1; }
  *uid = m;
  return 0;
}

int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (uid && *uid && (uintptr_t)*uid > 0x8000) {
    pthread_mutex_destroy(*uid);
    free(*uid);
    *uid = NULL;
  }
  return 0;
}

int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  if (!*uid) pthread_mutex_init_fake(uid, NULL);
  else if ((uintptr_t)*uid == 0x4000) {
    int attr = 1;
    pthread_mutex_init_fake(uid, &attr);
  }
  int ret = pthread_mutex_lock(*uid);
  if (ret == 0) egl_shim_on_mutex_post_lock((void *)uid);
  return ret;
}

int pthread_mutex_trylock_fake(pthread_mutex_t **uid) {
  if (!*uid) pthread_mutex_init_fake(uid, NULL);
  else if ((uintptr_t)*uid == 0x4000) {
    int attr = 1;
    pthread_mutex_init_fake(uid, &attr);
  }
  int ret = pthread_mutex_trylock(*uid);
  if (ret == 0) egl_shim_on_mutex_post_lock((void *)uid);
  return ret;
}

int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  if (!*uid) pthread_mutex_init_fake(uid, NULL);
  else if ((uintptr_t)*uid == 0x4000) {
    int attr = 1;
    pthread_mutex_init_fake(uid, &attr);
  }
  egl_shim_on_mutex_pre_unlock((void *)uid);
  return pthread_mutex_unlock(*uid);
}

int pthread_cond_init_fake(pthread_cond_t **cnd, const int *condattr) {
  (void)condattr;
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return -1;
  if (pthread_cond_init(c, NULL) < 0) { free(c); return -1; }
  *cnd = c;
  return 0;
}

int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  if (cnd && *cnd) {
    pthread_cond_destroy(*cnd);
    free(*cnd);
    *cnd = NULL;
  }
  return 0;
}

int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  if (!*cnd) pthread_cond_init_fake(cnd, NULL);
  return pthread_cond_wait(*cnd, *mtx);
}

int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx,
                                 const struct timespec *t) {
  if (!*cnd) pthread_cond_init_fake(cnd, NULL);
  return pthread_cond_timedwait(*cnd, *mtx, t);
}

int pthread_cond_signal_fake(pthread_cond_t **cnd) {
  if (!*cnd) pthread_cond_init_fake(cnd, NULL);
  return pthread_cond_signal(*cnd);
}

int pthread_cond_broadcast_fake(pthread_cond_t **cnd) {
  if (!*cnd) pthread_cond_init_fake(cnd, NULL);
  return pthread_cond_broadcast(*cnd);
}

typedef struct {
  void *(*entry)(void *);
  void *arg;
} ThreadWrapper;

static void *thread_wrapper_func(void *data) {
  ThreadWrapper *w = (ThreadWrapper *)data;
  void *(*entry)(void *) = w->entry;
  void *arg = w->arg;
  free(w);
  debugPrintf("[thread %lx] starting entry=%p arg=%p\n",
              (unsigned long)pthread_self(), (void *)entry, arg);
  void *ret = entry(arg);
  debugPrintf("[thread %lx] entry returned %p\n",
              (unsigned long)pthread_self(), ret);
  return ret;
}

int pthread_create_fake(pthread_t *thread, const void *attr, void *entry,
                         void *arg) {
  debugPrintf("pthread_create_fake(entry=%p, arg=%p)\n", entry, arg);
  ThreadWrapper *w = malloc(sizeof(ThreadWrapper));
  w->entry = entry;
  w->arg = arg;
  pthread_attr_t real_attr;
  pthread_attr_init(&real_attr);
  pthread_attr_setstacksize(&real_attr, 2 * 1024 * 1024); // 2MB stack
  int ret = pthread_create(thread, &real_attr, thread_wrapper_func, w);
  pthread_attr_destroy(&real_attr);
  if (ret != 0) free(w);
  return ret;
}

int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine) return -1;
  if (__sync_lock_test_and_set(once_control, 1) == 0)
    (*init_routine)();
  return 0;
}

/* GL logging wrappers — diagnose if game makes any GL calls after MakeCurrent */
static const GLubyte *glGetString_wrap(GLenum name) {
  switch (name) {
  case 0x1f00: /* GL_VENDOR */
    debugPrintf("GL: glGetString(GL_VENDOR)\n");
    return (const GLubyte *)"Imagination Technologies";
  case 0x1f01: /* GL_RENDERER */
    debugPrintf("GL: glGetString(GL_RENDERER)\n");
    return (const GLubyte *)"PowerVR Rogue GE8300";
  case 0x1f02: /* GL_VERSION — game expects GLES2 */
    debugPrintf("GL: glGetString(GL_VERSION)\n");
    return (const GLubyte *)"OpenGL ES 2.0";
  case 0x8b8c: /* GL_SHADING_LANGUAGE_VERSION */
    debugPrintf("GL: glGetString(GL_SHADING_LANGUAGE_VERSION)\n");
    return (const GLubyte *)"OpenGL ES GLSL ES 1.00";
  case 0x1f03: { /* GL_EXTENSIONS */
    static const GLubyte ext[] =
        "GL_OES_depth_texture "
        "GL_OES_depth24 "
        "GL_OES_packed_depth_stencil "
        "GL_OES_element_index_uint "
        "GL_OES_texture_npot "
        "GL_OES_rgb8_rgba8 "
        "GL_OES_vertex_array_object "
        "GL_OES_mapbuffer "
        "GL_EXT_texture_format_BGRA8888 "
        "GL_IMG_texture_compression_pvrtc "
        "GL_OES_compressed_ETC1_RGB8_texture";
    debugPrintf("GL: glGetString(GL_EXTENSIONS) -> %zu bytes\n", sizeof(ext) - 1);
    return ext;
  }
  default: {
    const GLubyte *s = glGetString(name);
    debugPrintf("GL: glGetString(0x%x) = \"%s\"\n", name, s ? (const char *)s : "(null)");
    return s;
  }
  }
}

static void glGetIntegerv_wrap(GLenum pname, GLint *data) {
  glGetIntegerv(pname, data);
  debugPrintf("GL: glGetIntegerv(0x%x) = %d\n", pname, data ? *data : -1);
}

static void glFrontFace_wrap(GLenum mode) {
  debugPrintf("GL: glFrontFace(0x%x)\n", mode);
  glFrontFace(mode);
}

static GLuint glCreateShader_wrap(GLenum type) {
  GLuint s = glCreateShader(type);
  debugPrintf("GL: glCreateShader(0x%x) = %u\n", type, s);
  return s;
}

static GLuint glCreateProgram_wrap(void) {
  GLuint p = glCreateProgram();
  debugPrintf("GL: glCreateProgram() = %u\n", p);
  return p;
}

static void glGenTextures_wrap(GLsizei n, GLuint *textures) {
  glGenTextures(n, textures);
  debugPrintf("GL: glGenTextures(%d) = %u\n", n, textures ? textures[0] : 0);
}

static void glGenFramebuffers_wrap(GLsizei n, GLuint *framebuffers) {
  glGenFramebuffers(n, framebuffers);
  debugPrintf("GL: glGenFramebuffers(%d) = %u\n", n, framebuffers ? framebuffers[0] : 0);
}

static void glGenBuffers_wrap(GLsizei n, GLuint *buffers) {
  glGenBuffers(n, buffers);
  debugPrintf("GL: glGenBuffers(%d) = %u\n", n, buffers ? buffers[0] : 0);
}

static void glBindFramebuffer_wrap(GLenum target, GLuint framebuffer) {
  debugPrintf("GL: glBindFramebuffer(0x%x, %u)\n", target, framebuffer);
  glBindFramebuffer(target, framebuffer);
}

/* Import table */
DynLibFunction dynlib_functions[] = {
    /* Android stubs */
    {"__sF", (uintptr_t)&fake_sF},
    {"__errno", (uintptr_t)&__errno_fake},
    {"__stack_chk_fail", (uintptr_t)&__stack_chk_fail_stub},
    {"__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake},
    {"__cxa_atexit", (uintptr_t)&__cxa_atexit},
    {"__cxa_finalize", (uintptr_t)&__cxa_finalize},
    {"__android_log_print", (uintptr_t)&__android_log_print_fake},
    {"android_set_abort_message", (uintptr_t)&android_set_abort_message_fake},
    {"__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake},
    {"dl_iterate_phdr", (uintptr_t)&dl_iterate_phdr_fake},
    {"__system_property_get", (uintptr_t)&__system_property_get_fake},

    /* libdl */
    {"dlopen", (uintptr_t)&dlopen_fake},
    {"dlsym", (uintptr_t)&dlsym_fake},
    {"dlclose", (uintptr_t)&dlclose_fake},
    {"dlerror", (uintptr_t)&dlerror_fake},
    {"dladdr", (uintptr_t)&dladdr_fake},

    /* Fortified libc */
    {"__memcpy_chk", (uintptr_t)&__memcpy_chk},
    {"__memmove_chk", (uintptr_t)&__memmove_chk},
    {"__memset_chk", (uintptr_t)&__memset_chk},
    {"__strcat_chk", (uintptr_t)&__strcat_chk},
    {"__strcpy_chk", (uintptr_t)&__strcpy_chk},
    {"__strlen_chk", (uintptr_t)&__strlen_chk},
    {"__strrchr_chk", (uintptr_t)&__strrchr_chk},
    {"__vsprintf_chk", (uintptr_t)&__vsprintf_chk},
    {"__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk},
    {"__read_chk", (uintptr_t)&__read_chk},
    {"__open_2", (uintptr_t)&__open_2},

    /* pthread (wrapped for bionic compat) */
    {"pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake},
    {"pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake},
    {"pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake},
    {"pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake},
    {"pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake},
    {"pthread_mutexattr_init", (uintptr_t)&ret0},
    {"pthread_mutexattr_settype", (uintptr_t)&ret0},
    {"pthread_mutexattr_destroy", (uintptr_t)&ret0},
    {"pthread_cond_init", (uintptr_t)&pthread_cond_init_fake},
    {"pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake},
    {"pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake},
    {"pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake},
    {"pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake},
    {"pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake},
    {"pthread_create", (uintptr_t)&pthread_create_fake},
    {"pthread_join", (uintptr_t)&pthread_join},
    {"pthread_self", (uintptr_t)&pthread_self},
    {"pthread_equal", (uintptr_t)&pthread_equal},
    {"pthread_detach", (uintptr_t)&pthread_detach},
    {"pthread_exit", (uintptr_t)&pthread_exit},
    {"pthread_once", (uintptr_t)&pthread_once_fake},
    {"pthread_attr_init", (uintptr_t)&ret0},
    {"pthread_attr_destroy", (uintptr_t)&ret0},
    {"pthread_attr_setdetachstate", (uintptr_t)&ret0},
    {"pthread_attr_setstacksize", (uintptr_t)&ret0},
    {"pthread_attr_setschedparam", (uintptr_t)&ret0},
    {"pthread_getschedparam", (uintptr_t)&ret0},
    {"pthread_key_create", (uintptr_t)&pthread_key_create},
    {"pthread_key_delete", (uintptr_t)&pthread_key_delete},
    {"pthread_getspecific", (uintptr_t)&pthread_getspecific},
    {"pthread_setspecific", (uintptr_t)&pthread_setspecific},
    {"sched_yield", (uintptr_t)&sched_yield},

    /* Memory */
    {"malloc", (uintptr_t)&malloc},
    {"calloc", (uintptr_t)&calloc},
    {"realloc", (uintptr_t)&realloc},
    {"free", (uintptr_t)&free},
    {"posix_memalign", (uintptr_t)&posix_memalign},

    /* String/memory */
    {"memcmp", (uintptr_t)&memcmp},
    {"memcpy", (uintptr_t)&memcpy},
    {"memmove", (uintptr_t)&memmove},
    {"memset", (uintptr_t)&memset},
    {"memchr", (uintptr_t)&memchr},
    {"strcmp", (uintptr_t)&strcmp},
    {"strncpy", (uintptr_t)&strncpy},
    {"strcat", (uintptr_t)&strcat},
    {"strchr", (uintptr_t)&strchr},
    {"strrchr", (uintptr_t)&strrchr},
    {"strlen", (uintptr_t)&strlen},
    {"strncmp", (uintptr_t)&strncmp},
    {"strerror_r", (uintptr_t)&strerror_r},
    {"strcoll", (uintptr_t)&strcoll},
    {"strxfrm", (uintptr_t)&strxfrm},
    {"strcpy", (uintptr_t)&strcpy},
    {"strtod", (uintptr_t)&strtod},
    {"strtof", (uintptr_t)&strtof},
    {"strtol", (uintptr_t)&strtol},
    {"strtoul", (uintptr_t)&strtoul},
    {"strtoll", (uintptr_t)&strtoll},
    {"strtoull", (uintptr_t)&strtoull},
    {"strtold", (uintptr_t)&strtold},
    {"strtold_l", (uintptr_t)&strtold},
    {"strtoll_l", (uintptr_t)&strtoll},
    {"strtoull_l", (uintptr_t)&strtoull},
    {"strftime", (uintptr_t)&strftime},

    /* ctype */
    {"islower", (uintptr_t)&islower},
    {"isupper", (uintptr_t)&isupper},
    {"isxdigit", (uintptr_t)&isxdigit},
    {"tolower", (uintptr_t)&tolower},
    {"toupper", (uintptr_t)&toupper},

    /* wctype / wchar */
    {"towlower", (uintptr_t)&towlower},
    {"towupper", (uintptr_t)&towupper},
    {"iswalpha", (uintptr_t)&iswalpha},
    {"iswblank", (uintptr_t)&iswblank},
    {"iswcntrl", (uintptr_t)&iswcntrl},
    {"iswdigit", (uintptr_t)&iswdigit},
    {"iswlower", (uintptr_t)&iswlower},
    {"iswprint", (uintptr_t)&iswprint},
    {"iswpunct", (uintptr_t)&iswpunct},
    {"iswspace", (uintptr_t)&iswspace},
    {"iswupper", (uintptr_t)&iswupper},
    {"iswxdigit", (uintptr_t)&iswxdigit},
    {"wctob", (uintptr_t)&wctob},
    {"btowc", (uintptr_t)&btowc},
    {"wcstol", (uintptr_t)&wcstol},
    {"wcstoul", (uintptr_t)&wcstoul},
    {"wcstoll", (uintptr_t)&wcstoll},
    {"wcstoull", (uintptr_t)&wcstoull},
    {"wcstod", (uintptr_t)&wcstod},
    {"wcstof", (uintptr_t)&wcstof},
    {"wcstold", (uintptr_t)&wcstold},
    {"wcslen", (uintptr_t)&wcslen},
    {"wcscoll", (uintptr_t)&wcscoll},
    {"wcsxfrm", (uintptr_t)&wcsxfrm},
    {"wmemcmp", (uintptr_t)&wmemcmp},
    {"wmemcpy", (uintptr_t)&wmemcpy},
    {"wmemmove", (uintptr_t)&wmemmove},
    {"wmemset", (uintptr_t)&wmemset},
    {"wmemchr", (uintptr_t)&wmemchr},
    {"mbrtowc", (uintptr_t)&mbrtowc},
    {"wcrtomb", (uintptr_t)&wcrtomb},
    {"mbrlen", (uintptr_t)&mbrlen},
    {"mbtowc", (uintptr_t)&mbtowc},
    {"mbsrtowcs", (uintptr_t)&mbsrtowcs},
    {"mbsnrtowcs", (uintptr_t)&mbsnrtowcs},
    {"wcsnrtombs", (uintptr_t)&wcsnrtombs},
    {"swprintf", (uintptr_t)&swprintf},

    /* stdio */
    {"printf", (uintptr_t)&printf},
    {"fprintf", (uintptr_t)&fprintf},
    {"snprintf", (uintptr_t)&snprintf},
    {"vfprintf", (uintptr_t)&vfprintf},
    {"vsprintf", (uintptr_t)&vsprintf},
    {"vsnprintf", (uintptr_t)&vsnprintf},
    {"vasprintf", (uintptr_t)&vasprintf},
    {"sscanf", (uintptr_t)&sscanf},
    {"vsscanf", (uintptr_t)&vsscanf},
    {"fopen", (uintptr_t)&fopen_fake},
    {"fclose", (uintptr_t)&fclose},
    {"fflush", (uintptr_t)&fflush},
    {"fread", (uintptr_t)&fread},
    {"fwrite", (uintptr_t)&fwrite},
    {"fputc", (uintptr_t)&fputc},
    {"fseek", (uintptr_t)&fseek},
    {"ftell", (uintptr_t)&ftell},
    {"getc", (uintptr_t)&getc},
    {"putc", (uintptr_t)&putc},
    {"putchar", (uintptr_t)&putchar},
    {"puts", (uintptr_t)&puts},
    {"ungetc", (uintptr_t)&ungetc},

    /* POSIX I/O */
    {"open", (uintptr_t)&open_fake},
    {"close", (uintptr_t)&close},
    {"read", (uintptr_t)&read},
    {"write", (uintptr_t)&write},
    {"mkdir", (uintptr_t)&mkdir},
    {"chdir", (uintptr_t)&chdir},
    {"remove", (uintptr_t)&remove},
    {"rename", (uintptr_t)&rename},

    /* stdlib */
    {"abort", (uintptr_t)&abort_fake},
    {"exit", (uintptr_t)&exit_fake},
    {"getenv", (uintptr_t)&getenv_fake},
    {"setenv", (uintptr_t)&setenv_fake},
    {"qsort", (uintptr_t)&qsort},
    {"rand", (uintptr_t)&rand},
    {"srand", (uintptr_t)&srand},
    {"lrand48", (uintptr_t)&lrand48},
    {"srand48", (uintptr_t)&srand48},

    /* math */
    {"acosf", (uintptr_t)&acosf},
    {"asinf", (uintptr_t)&asinf},
    {"atanf", (uintptr_t)&atanf},
    {"atan2f", (uintptr_t)&atan2f},
    {"cosf", (uintptr_t)&cosf},
    {"sinf", (uintptr_t)&sinf},
    {"sin", (uintptr_t)&sin},
    {"sincos", (uintptr_t)&sincos},
    {"sincosf", (uintptr_t)&sincosf},
    {"exp", (uintptr_t)&exp},
    {"expf", (uintptr_t)&expf},
    {"log", (uintptr_t)&log},
    {"log10f", (uintptr_t)&log10f},
    {"logf", (uintptr_t)&logf},
    {"pow", (uintptr_t)&pow},
    {"powf", (uintptr_t)&powf},
    {"sqrtf", (uintptr_t)&sqrtf},
    {"ldexp", (uintptr_t)&ldexp},
    {"ldexpf", (uintptr_t)&ldexpf},

    /* time */
    {"clock_gettime", (uintptr_t)&clock_gettime},
    {"nanosleep", (uintptr_t)&nanosleep},

    /* locale */
    {"setlocale", (uintptr_t)&setlocale},
    {"localeconv", (uintptr_t)&localeconv},
    {"newlocale", (uintptr_t)&newlocale},
    {"uselocale", (uintptr_t)&uselocale},
    {"freelocale", (uintptr_t)&freelocale},

    /* syslog */
    {"openlog", (uintptr_t)&openlog},
    {"closelog", (uintptr_t)&closelog},
    {"syslog", (uintptr_t)&syslog},

    /* signals */
    {"raise", (uintptr_t)&ret0},
    {"sigaction", (uintptr_t)&sigaction_fake},

    /* syscall */
    {"syscall", (uintptr_t)&syscall},
    {"sysconf", (uintptr_t)&sysconf},

    /* EGL (our shim) */
    {"eglGetDisplay", (uintptr_t)&egl_shim_GetDisplay},
    {"eglInitialize", (uintptr_t)&egl_shim_Initialize},
    {"eglChooseConfig", (uintptr_t)&egl_shim_ChooseConfig},
    {"eglCreateWindowSurface", (uintptr_t)&egl_shim_CreateWindowSurface},
    {"eglCreatePbufferSurface", (uintptr_t)&egl_shim_CreatePbufferSurface},
    {"eglCreateContext", (uintptr_t)&egl_shim_CreateContext},
    {"eglMakeCurrent", (uintptr_t)&egl_shim_MakeCurrent},
    {"eglSwapBuffers", (uintptr_t)&egl_shim_SwapBuffers},
    {"eglDestroySurface", (uintptr_t)&egl_shim_DestroySurface},
    {"eglDestroyContext", (uintptr_t)&egl_shim_DestroyContext},
    {"eglTerminate", (uintptr_t)&egl_shim_Terminate},
    {"eglQuerySurface", (uintptr_t)&egl_shim_QuerySurface},
    {"eglGetConfigAttrib", (uintptr_t)&egl_shim_GetConfigAttrib},
    {"eglGetError", (uintptr_t)&egl_shim_GetError},
    {"eglGetProcAddress", (uintptr_t)&egl_shim_GetProcAddress},
    {"eglBindAPI", (uintptr_t)&egl_shim_BindAPI},

    /* OpenGL ES 2.0 (direct passthrough) */
    {"glActiveTexture", (uintptr_t)&glActiveTexture},
    {"glAttachShader", (uintptr_t)&glAttachShader},
    {"glBindAttribLocation", (uintptr_t)&glBindAttribLocation},
    {"glBindBuffer", (uintptr_t)&glBindBuffer},
    {"glBindFramebuffer", (uintptr_t)&glBindFramebuffer_wrap},
    {"glBindTexture", (uintptr_t)&glBindTexture},
    {"glBlendEquationSeparate", (uintptr_t)&glBlendEquationSeparate},
    {"glBlendFuncSeparate", (uintptr_t)&glBlendFuncSeparate},
    {"glBufferData", (uintptr_t)&glBufferData},
    {"glBufferSubData", (uintptr_t)&glBufferSubData},
    {"glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus},
    {"glClear", (uintptr_t)&glClear},
    {"glClearColor", (uintptr_t)&glClearColor},
    {"glCompileShader", (uintptr_t)&glCompileShader},
    {"glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D},
    {"glCopyTexImage2D", (uintptr_t)&glCopyTexImage2D},
    {"glCreateProgram", (uintptr_t)&glCreateProgram_wrap},
    {"glCreateShader", (uintptr_t)&glCreateShader_wrap},
    {"glCullFace", (uintptr_t)&glCullFace},
    {"glDeleteBuffers", (uintptr_t)&glDeleteBuffers},
    {"glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers},
    {"glDeleteProgram", (uintptr_t)&glDeleteProgram},
    {"glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers},
    {"glDeleteShader", (uintptr_t)&glDeleteShader},
    {"glDeleteTextures", (uintptr_t)&glDeleteTextures},
    {"glDepthFunc", (uintptr_t)&glDepthFunc},
    {"glDepthMask", (uintptr_t)&glDepthMask},
    {"glDisable", (uintptr_t)&glDisable},
    {"glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray},
    {"glDrawArrays", (uintptr_t)&glDrawArrays},
    {"glDrawElements", (uintptr_t)&glDrawElements},
    {"glEnable", (uintptr_t)&glEnable},
    {"glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray},
    {"glFinish", (uintptr_t)&glFinish},
    {"glFlush", (uintptr_t)&glFlush},
    {"glFrontFace", (uintptr_t)&glFrontFace_wrap},
    {"glGenBuffers", (uintptr_t)&glGenBuffers_wrap},
    {"glGenFramebuffers", (uintptr_t)&glGenFramebuffers_wrap},
    {"glGenTextures", (uintptr_t)&glGenTextures_wrap},
    {"glGenerateMipmap", (uintptr_t)&glGenerateMipmap},
    {"glGetActiveAttrib", (uintptr_t)&glGetActiveAttrib},
    {"glGetActiveUniform", (uintptr_t)&glGetActiveUniform},
    {"glGetAttachedShaders", (uintptr_t)&glGetAttachedShaders},
    {"glGetAttribLocation", (uintptr_t)&glGetAttribLocation},
    {"glGetError", (uintptr_t)&glGetError},
    {"glGetIntegerv", (uintptr_t)&glGetIntegerv_wrap},
    {"glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog},
    {"glGetProgramiv", (uintptr_t)&glGetProgramiv},
    {"glGetShaderSource", (uintptr_t)&glGetShaderSource},
    {"glGetShaderiv", (uintptr_t)&glGetShaderiv},
    {"glGetString", (uintptr_t)&glGetString_wrap},
    {"glGetUniformLocation", (uintptr_t)&glGetUniformLocation},
    {"glGetVertexAttribPointerv", (uintptr_t)&glGetVertexAttribPointerv},
    {"glGetVertexAttribiv", (uintptr_t)&glGetVertexAttribiv},
    {"glLinkProgram", (uintptr_t)&glLinkProgram},
    {"glReleaseShaderCompiler", (uintptr_t)&glReleaseShaderCompiler},
    {"glShaderSource", (uintptr_t)&glShaderSource},
    {"glTexImage2D", (uintptr_t)&glTexImage2D},
    {"glTexParameteri", (uintptr_t)&glTexParameteri},
    {"glUniform1fv", (uintptr_t)&glUniform1fv},
    {"glUniform1i", (uintptr_t)&glUniform1i},
    {"glUniform2fv", (uintptr_t)&glUniform2fv},
    {"glUniform3fv", (uintptr_t)&glUniform3fv},
    {"glUniform4fv", (uintptr_t)&glUniform4fv},
    {"glUseProgram", (uintptr_t)&glUseProgram},
    {"glValidateProgram", (uintptr_t)&ret0},
    {"glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer},
    {"glViewport", (uintptr_t)&glViewport},

    /* OpenSL ES (our shim) */
    {"slCreateEngine", (uintptr_t)&slCreateEngine_shim},
    {"SL_IID_ENGINE", (uintptr_t)&sl_IID_ENGINE},
    {"SL_IID_PLAY", (uintptr_t)&sl_IID_PLAY},
    {"SL_IID_VOLUME", (uintptr_t)&sl_IID_VOLUME},
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&sl_IID_BUFFERQUEUE},
    {"SL_IID_ENGINECAPABILITIES", (uintptr_t)&sl_IID_ENGINECAPABILITIES},
    {"SL_IID_ENVIRONMENTALREVERB", (uintptr_t)&sl_IID_ENVIRONMENTALREVERB},

    /* Android NDK (our shim) */
    {"AAssetManager_fromJava", (uintptr_t)&ret1},
    {"AAssetManager_open", (uintptr_t)&AAssetManager_open_fake},
    {"AAsset_close", (uintptr_t)&AAsset_close_fake},
    {"AAsset_read", (uintptr_t)&AAsset_read_fake},
    {"AAsset_getLength", (uintptr_t)&AAsset_getLength_fake},
    {"AAsset_seek", (uintptr_t)&AAsset_seek_fake},
    {"ANativeWindow_fromSurface", (uintptr_t)&ANativeWindow_fromSurface_fake},
    {"ANativeWindow_getWidth", (uintptr_t)&ANativeWindow_getWidth_fake},
    {"ANativeWindow_getHeight", (uintptr_t)&ANativeWindow_getHeight_fake},
    {"ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry_fake},
};

size_t dynlib_numfunctions =
    sizeof(dynlib_functions) / sizeof(*dynlib_functions);

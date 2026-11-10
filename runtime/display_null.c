/* Sem janela (Linux/servidor): o jogo roda e só grava capturas quando pedido (SHOT=...). */
#include "plat.h"
int g_display_on;
volatile uint32_t g_host_buttons;
volatile int g_host_touch, g_host_touch_x, g_host_touch_y, g_host_circle_x, g_host_circle_y;
void display_init(void) { }
void display_frame(const uint32_t *top, const uint32_t *bottom) { (void)top; (void)bottom; }

/* sem janela: AUDIO_WAV=arquivo grava o som (PCM cru, estéreo 16 bits, 32728 Hz) para conferir */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void audio_push(const int16_t *samples, int frames) {
    static FILE *f; static int tried;
    if (!tried) { tried = 1; if (getenv("AUDIO_WAV")) f = fopen(getenv("AUDIO_WAV"), "wb"); }
    if (f) fwrite(samples, 4, (size_t)frames, f);
}

/* GPU: no Linux, contexto OpenGL sem janela pelo EGL (Mesa "surfaceless"), carregado em tempo de
 * execução; serve para testar o renderer OpenGL sem tela. No Windows (sem SDL) não há OpenGL aqui. */
#ifndef _WIN32
#include <dlfcn.h>
typedef void *(*egl_proc_t)(const char *);
static void *g_egl_dpy, *g_egl_ctx;
static unsigned (*p_eglMakeCurrent)(void *, void *, void *, void *);
static egl_proc_t p_eglGetProcAddress;
static int gl_init(void) {
    static int state;                        /* 0 = não tentou, 1 = ok, -1 = sem GL */
    if (state) return state > 0;
    state = -1;
    const char *r = getenv("RENDER");
    if (r && !strcmp(r, "software")) return 0;
    void *lib = dlopen("libEGL.so.1", RTLD_NOW);
    if (!lib) return 0;
    void *(*getPlatformDisplay)(unsigned, void *, const intptr_t *) = dlsym(lib, "eglGetPlatformDisplay");
    unsigned (*initialize)(void *, int *, int *) = dlsym(lib, "eglInitialize");
    unsigned (*bindAPI)(unsigned) = dlsym(lib, "eglBindAPI");
    unsigned (*chooseConfig)(void *, const int *, void **, int, int *) = dlsym(lib, "eglChooseConfig");
    void *(*createContext)(void *, void *, void *, const int *) = dlsym(lib, "eglCreateContext");
    p_eglMakeCurrent = dlsym(lib, "eglMakeCurrent");
    p_eglGetProcAddress = (egl_proc_t)dlsym(lib, "eglGetProcAddress");
    if (!getPlatformDisplay || !initialize || !bindAPI || !chooseConfig || !createContext || !p_eglMakeCurrent || !p_eglGetProcAddress) return 0;
    g_egl_dpy = getPlatformDisplay(0x31DD /* EGL_PLATFORM_SURFACELESS_MESA */, NULL, NULL);
    int maj, min;
    if (!g_egl_dpy || !initialize(g_egl_dpy, &maj, &min)) return 0;
    bindAPI(0x30A2 /* EGL_OPENGL_API */);
    const int cfg_attr[] = { 0x3040 /* RENDERABLE_TYPE */, 0x0008 /* OPENGL_BIT */, 0x3033 /* SURFACE_TYPE */, 0, 0x3038 };
    void *cfg; int n = 0;
    if (!chooseConfig(g_egl_dpy, cfg_attr, &cfg, 1, &n) || n < 1) return 0;
    const int ctx_attr[] = { 0x3098, 3, 0x30FB, 3, 0x30FD, 1 /* núcleo */, 0x3038 };
    g_egl_ctx = createContext(g_egl_dpy, cfg, NULL, ctx_attr);
    if (!g_egl_ctx) return 0;
    state = 1;
    return 1;
}
int gpu_gl_begin(void) { return gl_init() && p_eglMakeCurrent(g_egl_dpy, NULL, NULL, g_egl_ctx); }
void gpu_gl_end(void) { p_eglMakeCurrent(g_egl_dpy, NULL, NULL, NULL); }
void *gpu_gl_proc(const char *name) { return p_eglGetProcAddress(name); }
#else
int gpu_gl_begin(void) { return 0; }
void gpu_gl_end(void) { }
void *gpu_gl_proc(const char *name) { (void)name; return 0; }
#endif

void display_frame_hd(const uint32_t *top, int sc) { (void)top; (void)sc; }
void display_frame_hd_bot(const uint32_t *bot, int sc) { (void)bot; (void)sc; }

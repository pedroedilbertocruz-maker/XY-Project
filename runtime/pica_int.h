/* Tipos e funções internas compartilhadas entre o renderer em software (pica_draw.c) e o
 * renderer OpenGL (pica_gl.c). */
#pragma once
#include <stdint.h>

typedef struct {
    float pos[4], color[4], tc0[2], tc0w, tc1[2], tc2[2], view[3], quat[4];
} Vertex;
typedef struct { uint32_t color, depth; int w, h, cfmt, dfmt; } Fb;   /* framebuffer de destino (registradores 0x116-0x11E) */
typedef struct { float x, y, z, invw; Vertex v; } SV;                /* vértice em coordenadas de tela */
typedef struct { SV v[3]; } Tri;

uint32_t p2v(uint32_t p);
Fb fb_get(void);
void fb_read(const Fb *f, int x, int y, uint8_t o[4]);
void fb_write(const Fb *f, int x, int y, const uint8_t c[4], uint32_t wmask);
uint32_t depth_read(const Fb *f, int x, int y);
void depth_write(const Fb *f, int x, int y, uint32_t d);
uint8_t stencil_read(const Fb *f, int x, int y);
void stencil_write(const Fb *f, int x, int y, uint8_t v);
void texel(uint32_t addr, uint32_t fmt, int w, int x, int y, uint8_t o[4]);

/* iluminação (pica_light.c) */
extern int g_light_on, g_light_lut_dirty;
void light_setup(void);
const float *light_lut_data(void);                   /* 24 tabelas x 256 x (valor, delta) */
void light_gl_params(uint32_t u[20], float l[49][3], float ld[8][2]);

/* renderer OpenGL (pica_gl.c) */
int pica_gl_list_begin(void);                        /* 1 = esta lista vai pelo OpenGL */
void pica_gl_list_end(void);
void pica_gl_draw(const Fb *f, const Tri *t, int n);
int pica_gl_active(void);
/* ganchos de sincronização memória <-> GPU (gsp.c) */
void pica_gpu_flush(uint32_t a, uint32_t len);
void pica_gpu_will_write(uint32_t a, uint32_t len);
void pica_gpu_written(uint32_t a, uint32_t len);
int pica_gpu_fill(uint32_t start, uint32_t end, uint32_t value, int width);                            /* lista atual indo pelo OpenGL */

/* tempo gasto por etapa (ns), mostrado a cada 300 quadros no log: 0 vértices, 1 recorte/fila, 2 desenho GL,
 * 3 devolução GPU->memória, 4 carga memória->GPU, 5 texturas (hash/decodificação) */
extern uint64_t g_prof_ns[8];
uint64_t prof_now(void);

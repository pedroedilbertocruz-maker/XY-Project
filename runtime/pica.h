/* GPU PICA200 em software: interfaces internas. */
#pragma once
#include "rt.h"
#include <stdint.h>

extern uint32_t g_regs[0x300];
extern long g_pica_draws, g_pica_tris;

void pica_run_list(uint32_t addr, uint32_t size);   /* executa uma lista de comandos */
uint32_t *pica_capture_list(uint32_t addr, uint32_t size, uint32_t *nwords);   /* cópia achatada (malloc) */
void pica_run_flat(const uint32_t *w, uint32_t nwords);
void pica_frame_end(void);                          /* chamada a cada VBlank */
void pica_reg_written(uint32_t id, uint32_t value); /* efeitos colaterais da escrita (desenho, upload de shader...) */

/* ---------------- shaders ---------------- */
/* instrução pré-decodificada (decodificada uma vez por programa, não a cada vértice) */
typedef struct {
    uint8_t op, dst, mask, relsrc;   /* relsrc: qual fonte (0-2) usa o registrador de endereço; 0xFF = nenhuma */
    uint8_t rel, neg[3];
    uint8_t src[3];
    uint8_t sw[3][4];
} DIns;
typedef struct {
    uint32_t code[4096];       /* programa (palavras de instrução; 512 no de vértice, até 4096 no de geometria) */
    uint32_t opdesc[128];      /* descritores de operandos (swizzle/máscara) */
    float uniform_f[96][4];    /* c0-c95 */
    uint8_t uniform_i[4][4];   /* i0-i3: x = repetições, y = valor inicial de aL, z = incremento */
    uint16_t uniform_b;        /* b0-b15 */
    uint32_t entry;            /* ponto de entrada */
    uint8_t warned[64];
    DIns dec[4096];            /* cache de decodificação; dok[pc] = 0 quando code[pc] ou os descritores mudam */
    uint8_t dok[4096];
} ShaderState;
static inline void sh_code_written(ShaderState *st, uint32_t pc) { if (pc < 4096) st->dok[pc] = 0; }
void sh_desc_written(ShaderState *st);
void sh_prepare(ShaderState *st, uint32_t n);   /* decodifica tudo antes de rodar em paralelo */
typedef struct {
    float in[16][4], out[16][4], tmp[16][4];
    int a0[2], aL, cmp[2];
    float out_alt[16][4]; int bank;      /* shader de geometria: dois bancos de saída alternados a cada EMIT */
} ShaderRun;
/* emissor do shader de geometria (instruções SETEMIT/EMIT) */
typedef struct Emitter {
    int vertex_id, prim_emit, winding;
    void (*emit)(struct Emitter *e, float out[16][4]);
} Emitter;
/* execução em lote: SB_L vértices por instrução (layout SoA: [registrador][componente][vértice]) */
#define SB_L 16
typedef struct {
    float in[16][4][SB_L], out[16][4][SB_L], tmp[16][4][SB_L];
    int a0[2][SB_L], aL, cmp[2][SB_L];
} ShaderRunB;
/* devolve 0 se os vértices divergirem num desvio condicional (aí o chamador usa sh_run um a um) */
int sh_run_batch(const ShaderState *st, ShaderRunB *s);
extern ShaderState g_vs, g_gs;
void vs_run(ShaderRun *s);
void sh_run(const ShaderState *st, ShaderRun *s, Emitter *em);
float f24_to_f32(uint32_t raw);

/* getenv com cache (o ambiente não muda durante a execução; getenv é lento no Windows) */
#define ENVC(n) ({ static const char *ev_; static volatile int ei_; if (!ei_) { ev_ = getenv(n); ei_ = 1; } ev_; })

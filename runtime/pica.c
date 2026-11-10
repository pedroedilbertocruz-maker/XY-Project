/* GPU PICA200 em software.
 *
 * Executa as listas de comando que o jogo envia pelo GSP (comando GX 1). Formato (3dbrew "GPU/Commands"):
 *   palavra de parâmetro, depois cabeçalho: bits 0-15 registrador, 16-19 máscara de bytes,
 *   20-30 parâmetros extras, 31 escrita consecutiva (registradores seguidos) ou no mesmo registrador.
 *   Cada comando ocupa um múltiplo de 8 bytes.
 * Registradores principais: 3dbrew "GPU/Internal Registers".
 */
#include "pica.h"
#include "pica_int.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t g_regs[0x300];
static int g_stats_frames = -1;      /* PICA_STATS=n: estatísticas dos n primeiros quadros */
static long g_reg_writes[0x300];
long g_pica_draws, g_pica_tris;
uint32_t g_pica_word_addr;   /* endereço da palavra da lista em execução (depuração) */

/* ------------------------------------------------------------ escrita de registradores */
static int g_jump = -1;
static void reg_write(uint32_t id, uint32_t value, uint32_t mask) {
    if (id >= 0x300) return;
    uint32_t m = (mask & 1 ? 0xFFu : 0) | (mask & 2 ? 0xFF00u : 0) | (mask & 4 ? 0xFF0000u : 0) | (mask & 8 ? 0xFF000000u : 0);
    g_regs[id] = (g_regs[id] & ~m) | (value & m);
    if (g_stats_frames > 0) g_reg_writes[id]++;
    if (id == 0x23C || id == 0x23D) { g_jump = id - 0x23C; return; }
    pica_reg_written(id, value & m);
}

/* "Saltos" da lista de comandos (registradores 0x238-0x23D): escrever em CMDBUF_JUMP0/1 faz a GPU
 * continuar a execução no buffer CMDBUF_ADDR0/1 (endereço físico / 8) com CMDBUF_SIZE0/1 (x 8 bytes).
 * Os modelos 3D guardam listas de comandos prontas e as encadeiam assim. */
void pica_run_list(uint32_t addr, uint32_t size) {
    if (g_stats_frames < 0) g_stats_frames = ENVC("PICA_STATS") ? atoi(ENVC("PICA_STATS")) : 0;
    static int off = -1; if (off < 0) off = ENVC("PICA_OFF") != NULL;
    if (off) return;                       /* PICA_OFF: não renderiza (para medir o custo do renderer) */
    int gl = pica_gl_list_begin();
    uint32_t p = addr, end = addr + size;
    while (p + 8 <= end) {
        uint32_t param = MEM32(p), hdr = MEM32(p + 4);
        uint32_t id = hdr & 0xFFFF, mask = (hdr >> 16) & 0xF, extra = (hdr >> 20) & 0x7FF, consec = hdr >> 31;
        g_pica_word_addr = p;
        g_jump = -1;
        reg_write(id, param, mask);
        uint32_t q = p + 8;
        for (uint32_t i = 0; i < extra; i++, q += 4) {
            if (consec) id++;
            g_pica_word_addr = q;
            reg_write(id, MEM32(q), mask);
        }
        p += 8 + ((extra + 1) & ~1u) * 4;   /* parâmetros extras alinhados a 8 bytes */
        if (g_jump >= 0) {
            uint32_t pa = g_regs[0x23A + g_jump] * 8, sz = g_regs[0x238 + g_jump] * 8;
            uint32_t va = pa >= 0x18000000u && pa < 0x18600000u ? pa + 0x07000000u : pa >= 0x20000000u && pa < 0x28000000u ? pa - 0x0C000000u : pa;
            static long njump; if (ENVC("PICA_JUMPLOG") && njump++ < 40) fprintf(stderr, "[pica] salto %d da lista para %08x (%u bytes)\n", g_jump, va, sz);
            p = va; end = va + sz; g_jump = -1;
        }
    }
    if (gl) pica_gl_list_end();
}

/* Cópia "achatada" de uma lista (seguindo os saltos) feita no momento do envio, para a GPU assíncrona:
 * o jogo pode reescrever os buffers de comando logo depois de enviar, como no 3DS (onde a GPU já os leu). */
static uint32_t g_cap_shadow[4];   /* 0x238-0x23B vistos durante a cópia */
uint32_t *pica_capture_list(uint32_t addr, uint32_t size, uint32_t *nwords) {
    size_t cap = size / 4 + 64, n = 0;
    uint32_t *out = malloc(cap * 4);
    uint32_t p = addr, end = addr + size;
    int guard = 0;
    while (p + 8 <= end) {
        uint32_t hdr = MEM32(p + 4);
        uint32_t id = hdr & 0xFFFF, mask = (hdr >> 16) & 0xF, extra = (hdr >> 20) & 0x7FF, consec = hdr >> 31;
        uint32_t words = 2 + ((extra + 1) & ~1u);
        if (n + words > cap) { cap = (n + words) * 2; out = realloc(out, cap * 4); }
        memcpy(out + n, g_mem + p, words * 4);
        n += words;
        uint32_t m = (mask & 1 ? 0xFFu : 0) | (mask & 2 ? 0xFF00u : 0) | (mask & 4 ? 0xFF0000u : 0) | (mask & 8 ? 0xFF000000u : 0);
        int jump = -1;
        uint32_t rid = id;
        for (uint32_t i = 0; i <= extra; i++) {
            if (i && consec) rid++;
            uint32_t v = i == 0 ? MEM32(p) : MEM32(p + 4 + 4 * i);
            if (rid >= 0x238 && rid <= 0x23B) g_cap_shadow[rid - 0x238] = (g_cap_shadow[rid - 0x238] & ~m) | (v & m);
            if (rid == 0x23C || rid == 0x23D) jump = (int)(rid - 0x23C);
        }
        p += 8 + ((extra + 1) & ~1u) * 4;
        if (jump >= 0) {
            if (++guard > 100000) break;
            uint32_t pa = g_cap_shadow[2 + jump] * 8, sz = g_cap_shadow[jump] * 8;
            uint32_t va = pa >= 0x18000000u && pa < 0x18600000u ? pa + 0x07000000u : pa >= 0x20000000u && pa < 0x28000000u ? pa - 0x0C000000u : pa;
            p = va; end = va + sz;
        }
    }
    *nwords = (uint32_t)n;
    return out;
}
/* executa uma cópia achatada (os saltos já foram seguidos na cópia) */
void pica_run_flat(const uint32_t *w, uint32_t nwords) {
    if (g_stats_frames < 0) g_stats_frames = ENVC("PICA_STATS") ? atoi(ENVC("PICA_STATS")) : 0;
    static int off = -1; if (off < 0) off = ENVC("PICA_OFF") != NULL;
    if (off) return;
    int gl = pica_gl_list_begin();
    uint32_t i = 0;
    while (i + 2 <= nwords) {
        uint32_t param = w[i], hdr = w[i + 1];
        uint32_t id = hdr & 0xFFFF, mask = (hdr >> 16) & 0xF, extra = (hdr >> 20) & 0x7FF, consec = hdr >> 31;
        g_pica_word_addr = 0;
        reg_write(id, param, mask);
        for (uint32_t k = 0; k < extra && i + 2 + k < nwords; k++) {
            if (consec) id++;
            reg_write(id, w[i + 2 + k], mask);
        }
        g_jump = -1;
        i += 2 + ((extra + 1) & ~1u);
    }
    if (gl) pica_gl_list_end();
}

void pica_texcache_clear(void);
void pica_frame_end(void) {
    pica_texcache_clear();
    static int frame;
    if (g_stats_frames > 0 && frame < g_stats_frames) {
        fprintf(stderr, "[pica] quadro %d: %ld desenhos, %ld triângulos; registradores escritos:", frame, g_pica_draws, g_pica_tris);
        for (int i = 0; i < 0x300; i++) if (g_reg_writes[i]) fprintf(stderr, " %03x:%ld", i, g_reg_writes[i]);
        fprintf(stderr, "\n");
        memset(g_reg_writes, 0, sizeof g_reg_writes);
    }
    frame++;
    g_pica_draws = g_pica_tris = 0;
}

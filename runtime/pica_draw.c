/* PICA200: efeitos das escritas de registradores — upload de shaders/uniforms, carregamento de
 * vértices, montagem de primitivas, rasterização, texturas, combinadores (TEV) e saída de fragmentos.
 * Referência de registradores e formatos: 3dbrew ("GPU/Internal Registers", "GPU/Textures",
 * "GPU/Texture Combiners"). Implementação em software; a rasterização usa todos os núcleos.
 */
#include "pica.h"
#include "pica_int.h"
#include <time.h>
uint64_t g_prof_ns[8];
uint64_t prof_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec; }
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "plat.h"

#define R(x) g_regs[x]

/* endereço físico -> endereço virtual do jogo (VRAM e FCRAM/heap linear) */
uint32_t p2v(uint32_t p) {
    if (p >= 0x18000000u && p < 0x18600000u) return p + 0x07000000u;   /* VRAM -> 0x1F000000 */
    if (p >= 0x20000000u && p < 0x28000000u) return p - 0x0C000000u;   /* FCRAM -> 0x14000000 */
    return p;
}

/* ------------------------------------------------------------ uploads (uniforms, código, atributos fixos) */
static uint32_t g_unif_buf[4]; static int g_unif_n;
static uint32_t g_fixed_buf[3]; static int g_fixed_n;
static float g_fixed_attr[16][4];
static int g_vtx_dirty;

static void decode_f24x4(const uint32_t *b, float out[4]) {   /* 3 palavras -> (x, y, z, w) */
    out[3] = f24_to_f32(b[0] >> 8);
    out[2] = f24_to_f32(((b[0] & 0xFF) << 16) | (b[1] >> 16));
    out[1] = f24_to_f32(((b[1] & 0xFFFF) << 8) | (b[2] >> 24));
    out[0] = f24_to_f32(b[2] & 0xFFFFFF);
}

static void draw(int indexed);
static void irec_tri(const Vertex *a, const Vertex *b, const Vertex *c);
static void irec_flush(const Fb *f);
int g_irec;            /* 60 fps: gravando os desenhos deste quadro (só com o OpenGL) */
static int g_irec_interp;  /* o desenho atual pode ser interpolado (malha indexada/lista normal) */
static void tex_setup(void);
static void tev_setup(void);
static void immediate_attr(const uint32_t *b);
static void immediate_reset(void);
void light_lut_write(uint32_t value); void light_setup(void); extern int g_light_on;
void light_fragment(const float quat[4], const float view[3], const uint8_t tex[3][4], uint8_t prim[4], uint8_t sec[4]);

/* o shader de geometria compartilha código/uniforms com o de vértice quando 0x244 bit 0 = 0 e a
 * geometria está desligada (como no Citra/Azahar) */
static int gs_shared(void) { return !(R(0x244) & 1) && (R(0x229) & 3) != 2; }
static uint32_t g_gs_unif_buf[4]; static int g_gs_unif_n;

static void gs_reg_written(uint32_t id, uint32_t value) {
    if (id >= 0x291 && id <= 0x298) {                     /* uniforms float do shader de geometria */
        int f32 = R(0x290) >> 31;
        g_gs_unif_buf[g_gs_unif_n++] = value;
        if (g_gs_unif_n == (f32 ? 4 : 3)) {
            uint32_t idx = R(0x290) & 0x7F;
            if (idx < 96) {
                float *u = g_gs.uniform_f[idx];
                if (f32) { for (int k = 0; k < 4; k++) memcpy(&u[3 - k], &g_gs_unif_buf[k], 4); }
                else decode_f24x4(g_gs_unif_buf, u);
            }
            R(0x290) = (R(0x290) & ~0x7Fu) | ((idx + 1) & 0x7F);
            g_gs_unif_n = 0;
        }
        return;
    }
    if (id == 0x290) { g_gs_unif_n = 0; return; }
    if (id >= 0x29C && id <= 0x2A3) {
        uint32_t o = R(0x29B) & 0xFFF; g_gs.code[o] = value; sh_code_written(&g_gs, o);
        R(0x29B) = o + 1; return;
    }
    if (id >= 0x2A6 && id <= 0x2AD) {
        uint32_t o = R(0x2A5) & 0x7F; if (g_gs.opdesc[o] != value) { g_gs.opdesc[o] = value; sh_desc_written(&g_gs); }
        R(0x2A5) = (o + 1) & 0x7F; return;
    }
    if (id == 0x280) { g_gs.uniform_b = (uint16_t)value; return; }
    if (id >= 0x281 && id <= 0x284) { for (int k = 0; k < 4; k++) g_gs.uniform_i[id - 0x281][k] = (uint8_t)(value >> (8 * k)); return; }
    if (id == 0x28A) { g_gs.entry = value & 0xFFFF; return; }
}

/* neblina (fog): tabela de 128 entradas escrita em 0xE8-0xEF a partir do índice em 0xE6 */
uint32_t g_fog_lut[128]; volatile int g_fog_lut_dirty = 1; unsigned g_fog_lut_gen; const uint32_t *g_fog_lut_override;
void pica_reg_written(uint32_t id, uint32_t value) {
    if (id >= 0x280 && id <= 0x2AF) { gs_reg_written(id, value); return; }
    if (id >= 0xE8 && id <= 0xEF) {
        uint32_t o = R(0xE6) & 0xFFFF;
        if (g_fog_lut[o % 128] != value) { g_fog_lut[o % 128] = value; g_fog_lut_dirty = 1; g_fog_lut_gen++; }
        R(0xE6) = (R(0xE6) & ~0xFFFFu) | ((o + 1) & 0xFFFF);
        return;
    }
    if (id == 0x2BD && gs_shared()) R(0x28D) = value;     /* máscara de saída também vale para a geometria */
    if (id >= 0x2C1 && id <= 0x2C8) {                     /* dados de uniform float */
        int f32 = R(0x2C0) >> 31;
        g_unif_buf[g_unif_n++] = value;
        if (g_unif_n == (f32 ? 4 : 3)) {
            uint32_t idx = R(0x2C0) & 0xFF;
            if (idx < 96) {
                float *u = g_vs.uniform_f[idx];
                if (f32) { for (int k = 0; k < 4; k++) memcpy(&u[3 - k], &g_unif_buf[k], 4); }
                else decode_f24x4(g_unif_buf, u);
                if (gs_shared()) memcpy(g_gs.uniform_f[idx], u, sizeof(float) * 4);
                static long nanlog;
                if (ENVC("PICA_NANLOG") && nanlog < 20 && (u[0] != u[0] || u[1] != u[1] || u[2] != u[2] || u[3] != u[3])) {
                    extern uint32_t g_pica_word_addr; extern long g_gsp_frames; nanlog++;
                    fprintf(stderr, "[pica] uniform c%u com NaN (%s) vindo da lista em %08x, quadro %ld: %08x %08x %08x %08x\n", idx, f32 ? "f32" : "f24",
                            g_pica_word_addr, g_gsp_frames, g_unif_buf[0], g_unif_buf[1], g_unif_buf[2], g_unif_buf[3]);
                }
            }
            R(0x2C0) = (R(0x2C0) & ~0xFFu) | ((idx + 1) & 0xFF);
            g_unif_n = 0;
        }
        return;
    }
    if (id == 0x2C0) { g_unif_n = 0; return; }
    if (id >= 0x2CC && id <= 0x2D3) {                     /* código do shader */
        uint32_t o = R(0x2CB) & 0xFFF; if (o < 512) { g_vs.code[o] = value; sh_code_written(&g_vs, o); if (gs_shared()) { g_gs.code[o] = value; sh_code_written(&g_gs, o); } }
        R(0x2CB) = o + 1; return;
    }
    if (id >= 0x2D6 && id <= 0x2DD) {                     /* descritores de operandos */
        uint32_t o = R(0x2D5) & 0x7F;
        if (g_vs.opdesc[o] != value) { g_vs.opdesc[o] = value; sh_desc_written(&g_vs); }
        if (gs_shared() && g_gs.opdesc[o] != value) { g_gs.opdesc[o] = value; sh_desc_written(&g_gs); }
        R(0x2D5) = (o + 1) & 0x7F; return;
    }
    if (id == 0x2B0) { g_vs.uniform_b = (uint16_t)value; return; }
    if (id >= 0x2B1 && id <= 0x2B4) { for (int k = 0; k < 4; k++) g_vs.uniform_i[id - 0x2B1][k] = (uint8_t)(value >> (8 * k)); return; }
    if (id == 0x2BA) { g_vs.entry = value & 0xFFFF; return; }
    if (id == 0x232) { g_fixed_n = 0; return; }
    if (id >= 0x233 && id <= 0x235) {                     /* atributo fixo (3 palavras com 4 float24) */
        g_fixed_buf[g_fixed_n++] = value;
        if (g_fixed_n == 3) {
            uint32_t idx = R(0x232) & 0xF;
            if (idx < 12) decode_f24x4(g_fixed_buf, g_fixed_attr[idx]);
            else if (idx == 0xF) immediate_attr(g_fixed_buf);
            g_fixed_n = 0;
        }
        return;
    }
    if (id >= 0x1C8 && id <= 0x1CF) { light_lut_write(value); return; }   /* tabelas da iluminação */
    if (id == 0x25F) { immediate_reset(); return; }          /* reinicia a primitiva */
    if (id == 0x22E && (value & 1)) { draw(0); return; }
    if (id == 0x22F && (value & 1)) { draw(1); return; }
}

/* ------------------------------------------------------------ vértices */

static void load_attr(uint32_t addr, uint32_t fmt, uint32_t n, float out[4]) {
    out[0] = out[1] = out[2] = 0; out[3] = 1;
    for (uint32_t k = 0; k < n; k++) {
        switch (fmt) {
        case 0: out[k] = (int8_t)MEM8(addr + k); break;
        case 1: out[k] = MEM8(addr + k); break;
        case 2: out[k] = (int16_t)MEM16(addr + 2 * k); break;
        default: { float f; uint32_t u = MEM32(addr + 4 * k); memcpy(&f, &u, 4); out[k] = f; }
        }
    }
}

static void shade_vertex(float attr[][4], int nattr, Vertex *v);
static int load_vertex_attrs(uint32_t index, float attr[12][4]);
static void vertex(uint32_t index, Vertex *v) {
    float attr[12][4];
    int nattr = load_vertex_attrs(index, attr);
    shade_vertex(attr, nattr, v);
}
/* plano de leitura dos atributos, montado uma vez por desenho a partir dos registradores 0x200-0x227 */
typedef struct { uint32_t va, stride; uint8_t fmt, n, attr; } APEnt;
static APEnt g_ap[64]; static int g_apn, g_ap_nattr;
static void ap_build(void) {
    uint32_t base = (R(0x200) & 0x1FFFFFFE) << 3;   /* físico: os deslocamentos somam antes da conversão */
    uint64_t fmts = ((uint64_t)(R(0x202) & 0xFFFF) << 32) | R(0x201);
    uint32_t fixed_mask = (R(0x202) >> 16) & 0xFFF;
    g_ap_nattr = (int)(R(0x202) >> 28) + 1; g_apn = 0;
    for (int b = 0; b < 12; b++) {
        uint32_t off = R(0x203 + 3 * b);
        uint64_t comps = ((uint64_t)(R(0x205 + 3 * b) & 0xFFFF) << 32) | R(0x204 + 3 * b);
        uint32_t stride = (R(0x205 + 3 * b) >> 16) & 0xFF, ncomp = R(0x205 + 3 * b) >> 28;
        uint32_t p = 0;
        for (uint32_t k = 0; k < ncomp; k++) {
            uint32_t a = (comps >> (4 * k)) & 0xF;
            if (a < 12) {
                uint32_t f = (fmts >> (4 * a)) & 3, n = ((fmts >> (4 * a + 2)) & 3) + 1;
                uint32_t es = f == 0 || f == 1 ? 1 : f == 2 ? 2 : 4;
                p = (p + es - 1) & ~(es - 1);
                if (!((fixed_mask >> a) & 1) && g_apn < 64)
                    g_ap[g_apn++] = (APEnt){ p2v(base + off + p), stride, (uint8_t)f, (uint8_t)n, (uint8_t)a };
                p += es * n;
            } else {
                p = (p + 3) & ~3u; p += (a - 11) * 4;
            }
        }
    }
}
static int load_vertex_attrs(uint32_t index, float attr[12][4]) {
    memcpy(attr, g_fixed_attr, sizeof(float) * 12 * 4);
    for (int i = 0; i < g_apn; i++) {
        const APEnt *e = &g_ap[i];
        load_attr(e->va + index * e->stride, e->fmt, e->n, attr[e->attr]);
    }
    return g_ap_nattr;
}

/* saídas empacotadas (na ordem dos bits da máscara de saída) -> semânticas (0x50-0x56) -> Vertex */
static void slots_to_vertex(float out[][4], int nslots, Vertex *v, int gs) {
    float sem[0x20]; for (int i = 0; i < 0x20; i++) sem[i] = gs ? 1 : 0;   /* sem saída: 1 no hardware */
    if (!gs) { sem[3] = 1; sem[0x0B] = 1; }
    int total = (int)(R(0x4F) & 7);
    for (int slot = 0; slot < nslots && slot < 7 && slot < total; slot++) {
        uint32_t m = R(0x50 + slot);
        for (int k = 0; k < 4; k++) { uint32_t sm = (m >> (8 * k)) & 0x1F; if (sm < 0x1F) sem[sm] = out[slot][k]; }
    }
    if (gs) for (int k = 8; k < 12; k++) { float c = fabsf(sem[k]); sem[k] = c < 1 ? c : 1; }
    for (int k = 0; k < 4; k++) { v->pos[k] = sem[k]; v->quat[k] = sem[4 + k]; v->color[k] = sem[8 + k]; }
    v->tc0[0] = sem[0x0C]; v->tc0[1] = sem[0x0D]; v->tc1[0] = sem[0x0E]; v->tc1[1] = sem[0x0F];
    v->tc0w = sem[0x10]; v->view[0] = sem[0x12]; v->view[1] = sem[0x13]; v->view[2] = sem[0x14];
    v->tc2[0] = sem[0x16]; v->tc2[1] = sem[0x17];
}
/* executa o shader de vértice; devolve as saídas empacotadas pela máscara 0x2BD */
static int vs_packed(float attr[][4], int nattr, float packed[16][4]) {
    ShaderRun s; memset(&s, 0, sizeof s);
    uint64_t map = ((uint64_t)R(0x2BC) << 32) | R(0x2BB);
    for (int a = 0; a < nattr && a < 12; a++) memcpy(s.in[(map >> (4 * a)) & 0xF], attr[a], sizeof attr[a]);
    vs_run(&s);
    uint32_t omask = R(0x2BD) & 0xFFFF; int n = 0;
    for (int o = 0; o < 16; o++) if ((omask >> o) & 1) memcpy(packed[n++], s.out[o], sizeof s.out[o]);
    return n;
}
static void shade_vertex(float attr[][4], int nattr, Vertex *v) {
    float packed[16][4];
    int n = vs_packed(attr, nattr, packed);
    slots_to_vertex(packed, n, v, 0);
}

/* ------------------------------------------------------------ texturas */
static int morton8(int x, int y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

static void etc1_texel(uint32_t blk_addr, int has_alpha, int x, int y, uint8_t o[4]) {
    /* blocos 4x4 dentro do bloco 8x8; cada um 8 bytes (+8 de alfa no ETC1A4) */
    int sub = ((y >> 2) << 1) | (x >> 2);   /* ordem: (0,0) (4,0) (0,4) (4,4) */
    int bs = has_alpha ? 16 : 8;
    uint32_t a = blk_addr + sub * bs;
    uint8_t alpha = 255;
    if (has_alpha) {
        uint64_t aw = (uint64_t)MEM32(a) | ((uint64_t)MEM32(a + 4) << 32);
        int lx = x & 3, ly = y & 3;
        alpha = (uint8_t)(((aw >> (4 * (lx * 4 + ly))) & 0xF) * 17);
        a += 8;
    }
    uint64_t w = (uint64_t)MEM32(a) | ((uint64_t)MEM32(a + 4) << 32);
    static const int tbl[8][2] = { {2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183} };
    int lx = x & 3, ly = y & 3;
    int flip = (w >> 32) & 1, diff = (w >> 33) & 1;
    int second = flip ? ly >= 2 : lx >= 2;
    int r, g, b;
    if (diff) {
        int r1 = (w >> 59) & 31, g1 = (w >> 51) & 31, b1 = (w >> 43) & 31;
        int dr = ((int)((w >> 56) & 7) ^ 4) - 4, dg = ((int)((w >> 48) & 7) ^ 4) - 4, db = ((int)((w >> 40) & 7) ^ 4) - 4;
        if (second) { r1 += dr; g1 += dg; b1 += db; }
        r = (r1 << 3) | (r1 >> 2); g = (g1 << 3) | (g1 >> 2); b = (b1 << 3) | (b1 >> 2);
    } else {
        int sh = second ? 4 : 0;   /* modo individual: 1º sub-bloco nos bits altos (R1 63-60, G1 55-52, B1 47-44) */
        r = ((w >> (60 - sh)) & 15) * 17; g = ((w >> (52 - sh)) & 15) * 17; b = ((w >> (44 - sh)) & 15) * 17;
    }
    int t = second ? (w >> 34) & 7 : (w >> 37) & 7;
    int idx = lx * 4 + ly;
    int msb = (w >> (idx + 16)) & 1, lsb = (w >> idx) & 1;
    int m = msb ? -tbl[t][lsb] : tbl[t][lsb];   /* índice 0:+a 1:+b 2:-a 3:-b */
    r += m; g += m; b += m;
    o[0] = r < 0 ? 0 : r > 255 ? 255 : r; o[1] = g < 0 ? 0 : g > 255 ? 255 : g; o[2] = b < 0 ? 0 : b > 255 ? 255 : b; o[3] = alpha;
}

void texel(uint32_t addr, uint32_t fmt, int w, int x, int y, uint8_t o[4]) {
    int tile = (y >> 3) * (w >> 3) + (x >> 3);
    int m = morton8(x & 7, y & 7);
    if (fmt == 12 || fmt == 13) { etc1_texel(addr + tile * (fmt == 13 ? 64 : 32), fmt == 13, x & 7, y & 7, o); return; }   /* bloco 8x8 = 4 sub-blocos de 8 bytes (16 com alfa) */
    static const int bpp[] = { 32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4 };
    uint32_t bit = (uint32_t)(tile * 64 + m) * bpp[fmt];
    uint32_t p = addr + bit / 8;
    uint16_t v16;
    switch (fmt) {
    case 0: o[0] = MEM8(p + 3); o[1] = MEM8(p + 2); o[2] = MEM8(p + 1); o[3] = MEM8(p); break;
    case 1: o[0] = MEM8(p + 2); o[1] = MEM8(p + 1); o[2] = MEM8(p); o[3] = 255; break;
    case 2: v16 = MEM16(p); o[0] = ((v16 >> 11) & 31) * 255 / 31; o[1] = ((v16 >> 6) & 31) * 255 / 31; o[2] = ((v16 >> 1) & 31) * 255 / 31; o[3] = (v16 & 1) * 255; break;
    case 3: v16 = MEM16(p); o[0] = ((v16 >> 11) & 31) * 255 / 31; o[1] = ((v16 >> 5) & 63) * 255 / 63; o[2] = (v16 & 31) * 255 / 31; o[3] = 255; break;
    case 4: v16 = MEM16(p); o[0] = ((v16 >> 12) & 15) * 17; o[1] = ((v16 >> 8) & 15) * 17; o[2] = ((v16 >> 4) & 15) * 17; o[3] = (v16 & 15) * 17; break;
    case 5: o[0] = o[1] = o[2] = MEM8(p + 1); o[3] = MEM8(p); break;              /* IA8 */
    case 6: o[0] = MEM8(p + 1); o[1] = MEM8(p); o[2] = 0; o[3] = 255; break;      /* RG8 */
    case 7: o[0] = o[1] = o[2] = MEM8(p); o[3] = 255; break;                      /* I8 */
    case 8: o[0] = o[1] = o[2] = 0; o[3] = MEM8(p); break;                        /* A8 */
    case 9: o[0] = o[1] = o[2] = (MEM8(p) >> 4) * 17; o[3] = (MEM8(p) & 15) * 17; break;   /* IA4 */
    case 10: { int n = (MEM8(p) >> (bit & 4)) & 15; o[0] = o[1] = o[2] = n * 17; o[3] = 255; break; }  /* I4 */
    case 11: { int n = (MEM8(p) >> (bit & 4)) & 15; o[0] = o[1] = o[2] = 0; o[3] = n * 17; break; }    /* A4 */
    default: o[0] = 255; o[1] = 0; o[2] = 255; o[3] = 255;
    }
}

static inline int wrap(int c, int size, int mode) {
    if ((unsigned)c < (unsigned)size) return c;                              /* caso comum: dentro da textura */
    if (mode == 2 && !(size & (size - 1))) return c & (size - 1);            /* repetir, tamanho potência de 2 */
    switch (mode) {
    case 2: c %= size; if (c < 0) c += size; return c;                       /* repetir */
    case 3: { int p = c % (2 * size); if (p < 0) p += 2 * size; return p < size ? p : 2 * size - 1 - p; }
    default: return c < 0 ? 0 : c >= size ? size - 1 : c;                    /* bordas */
    }
}

/* cache de texturas decodificadas para RGBA linear (limpo a cada quadro: o jogo pode reescrevê-las) */
typedef struct { uint32_t addr, fmt; int w, h; uint8_t *px; } TexCache;
static TexCache g_tc[64]; static int g_ntc;
void pica_texcache_clear(void) { for (int i = 0; i < g_ntc; i++) free(g_tc[i].px); g_ntc = 0; }
static const uint8_t *tex_decoded(uint32_t addr, uint32_t fmt, int w, int h) {
    for (int i = 0; i < g_ntc; i++)
        if (g_tc[i].addr == addr && g_tc[i].fmt == fmt && g_tc[i].w == w && g_tc[i].h == h) return g_tc[i].px;
    if (g_ntc == 64) pica_texcache_clear();
    uint8_t *px = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) texel(addr, fmt, w, x, y, px + ((size_t)y * w + x) * 4);
    g_tc[g_ntc++] = (TexCache){ addr, fmt, w, h, px };
    return px;
}
#define TEXEL(px, w, x, y, o) memcpy((o), (px) + ((size_t)(y) * (w) + (x)) * 4, 4)

typedef struct { const uint8_t *px; int w, h, wrap_s, wrap_t, linear; } TexUnit;
static TexUnit g_tu[3];
static void tex_setup(void) {
    static const uint32_t cfg_reg[3] = { 0x81, 0x91, 0x99 }, fmt_reg[3] = { 0x8E, 0x96, 0x9E };
    for (int unit = 0; unit < 3; unit++) {
        TexUnit *t = &g_tu[unit]; t->px = NULL;
        if (!((R(0x80) >> unit) & 1)) continue;
        uint32_t base = cfg_reg[unit];
        uint32_t size = R(base + 1), cfg = R(base + 2), addr = p2v(R(base + 4) * 8), fmt = R(fmt_reg[unit]) & 0xF;
        t->w = (size >> 16) & 0x7FF; t->h = size & 0x7FF;
        if (!t->w || !t->h || fmt > 13 || t->w > 1024 || t->h > 1024) continue;
        t->wrap_t = (cfg >> 8) & 7; t->wrap_s = (cfg >> 12) & 7; t->linear = (cfg >> 1) & 1;
        t->px = tex_decoded(addr, fmt, t->w, t->h);
    }
}
static void sample(int unit, float u, float v, uint8_t o[4]) {
    const TexUnit *tu = &g_tu[unit];
    if (!tu->px) { o[0] = o[1] = o[2] = o[3] = 0; return; }
    const uint8_t *px = tu->px; int w = tu->w, h = tu->h, wrap_s = tu->wrap_s, wrap_t = tu->wrap_t, linear = tu->linear;
    float fx = u * w, fy = (1.0f - v) * h;     /* a textura fica de cabeça para baixo na memória */
    if (!linear) {
        int x = wrap((int)floorf(fx), w, wrap_s), y = wrap((int)floorf(fy), h, wrap_t);
        TEXEL(px, w, x, y, o); return;
    }
    fx -= 0.5f; fy -= 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float ax = fx - x0, ay = fy - y0;
    uint8_t t[4][4];
    TEXEL(px, w, wrap(x0, w, wrap_s), wrap(y0, h, wrap_t), t[0]);
    TEXEL(px, w, wrap(x0 + 1, w, wrap_s), wrap(y0, h, wrap_t), t[1]);
    TEXEL(px, w, wrap(x0, w, wrap_s), wrap(y0 + 1, h, wrap_t), t[2]);
    TEXEL(px, w, wrap(x0 + 1, w, wrap_s), wrap(y0 + 1, h, wrap_t), t[3]);
    for (int k = 0; k < 4; k++) {
        float a = t[0][k] + (t[1][k] - t[0][k]) * ax, b = t[2][k] + (t[3][k] - t[2][k]) * ax;
        o[k] = (uint8_t)(a + (b - a) * ay + 0.5f);
    }
}

/* ------------------------------------------------------------ combinadores (TEV) */
static const uint32_t k_tev_base[6] = { 0xC0, 0xC8, 0xD0, 0xD8, 0xF0, 0xF8 };

static void color_mod(uint32_t m, const uint8_t s[4], uint8_t out[3]) {
    for (int k = 0; k < 3; k++) {
        int v;
        switch (m) {
        case 0: v = s[k]; break; case 1: v = 255 - s[k]; break;
        case 2: v = s[3]; break; case 3: v = 255 - s[3]; break;
        case 4: v = s[0]; break; case 5: v = 255 - s[0]; break;
        case 8: v = s[1]; break; case 9: v = 255 - s[1]; break;
        case 12: v = s[2]; break; case 13: v = 255 - s[2]; break;
        default: v = s[k];
        }
        out[k] = (uint8_t)v;
    }
}
static uint8_t alpha_mod(uint32_t m, const uint8_t s[4]) {
    switch (m) {
    case 0: return s[3]; case 1: return 255 - s[3]; case 2: return s[0]; case 3: return 255 - s[0];
    case 4: return s[1]; case 5: return 255 - s[1]; case 6: return s[2]; default: return 255 - s[2];
    }
}
static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static int combine(uint32_t op, int a, int b, int c) {
    switch (op) {
    case 0: return a;
    case 1: return a * b / 255;
    case 2: return clamp255(a + b);
    case 3: return clamp255(a + b - 128);
    case 4: return (a * c + b * (255 - c)) / 255;
    case 5: return clamp255(a - b);
    case 8: return clamp255(a * b / 255 + c);
    case 9: return clamp255(a + b) * c / 255;
    default: return a;
    }
}

typedef struct {
    uint8_t csrc[3], asrc[3], cmod[3], amod[3], cop, aop, cscale, ascale, skip;
    uint8_t konst[4];
} TevStage;
static TevStage g_tev[6];
static uint8_t g_tev_buf_init[4];
static uint32_t g_tev_upd;

/* lê a configuração do TEV uma vez por desenho; estágios que só repassam o anterior são pulados */
static void tev_setup(void) {
    uint32_t bc = R(0xFD);
    for (int k = 0; k < 4; k++) g_tev_buf_init[k] = (uint8_t)(bc >> (8 * k));
    g_tev_upd = R(0xE0);
    for (int st = 0; st < 6; st++) {
        uint32_t b = k_tev_base[st], src = R(b), opnd = R(b + 1), cmb = R(b + 2), cst = R(b + 3), scale = R(b + 4);
        TevStage *t = &g_tev[st];
        for (int i = 0; i < 3; i++) {
            t->csrc[i] = (src >> (4 * i)) & 0xF; t->asrc[i] = (src >> (16 + 4 * i)) & 0xF;
            t->cmod[i] = (opnd >> (4 * i)) & 0xF; t->amod[i] = (opnd >> (12 + 4 * i)) & 7;
        }
        t->cop = cmb & 0xF; t->aop = (cmb >> 16) & 0xF;
        t->cscale = 1 << (scale & 3); t->ascale = 1 << ((scale >> 16) & 3);
        for (int k = 0; k < 4; k++) t->konst[k] = (uint8_t)(cst >> (8 * k));
        int updates = st < 4 && ((g_tev_upd >> (8 + st)) & 1 || (g_tev_upd >> (12 + st)) & 1);
        t->skip = t->cop == 0 && t->aop == 0 && t->csrc[0] == 0xF && t->asrc[0] == 0xF &&
                  t->cmod[0] == 0 && t->amod[0] == 0 && t->cscale == 1 && t->ascale == 1 && !updates;
    }
}

static void tev(const uint8_t prim[4], const uint8_t tex[3][4], const uint8_t lprim[4], const uint8_t lsec[4], uint8_t out[4]) {
    uint8_t prev[4] = { 0, 0, 0, 0 }, buf[4], next_buf[4];
    static const uint8_t zero[4] = { 0, 0, 0, 0 };
    memcpy(buf, g_tev_buf_init, 4); memcpy(next_buf, g_tev_buf_init, 4);
    const uint8_t *srcs[16];
    for (int k = 0; k < 16; k++) srcs[k] = zero;
    srcs[0] = prim; srcs[1] = lprim; srcs[2] = lsec; srcs[3] = tex[0]; srcs[4] = tex[1]; srcs[5] = tex[2];
    srcs[0xD] = buf; srcs[0xF] = prev;
    for (int st = 0; st < 6; st++) {
        const TevStage *t = &g_tev[st];
        if (t->skip) { memcpy(buf, next_buf, 4); continue; }
        srcs[0xE] = t->konst;
        uint8_t cc[3][3], aa[3];
        for (int i = 0; i < 3; i++) {
            color_mod(t->cmod[i], srcs[t->csrc[i]], cc[i]);
            aa[i] = alpha_mod(t->amod[i], srcs[t->asrc[i]]);
        }
        uint8_t res[4];
        if (t->cop == 6 || t->cop == 7) {   /* DOT3 */
            int d = 0; for (int k = 0; k < 3; k++) d += ((cc[0][k] * 2 - 255) * (cc[1][k] * 2 - 255) + 128) / 256;
            d = clamp255(d);
            res[0] = res[1] = res[2] = (uint8_t)clamp255(d * t->cscale);
            res[3] = t->cop == 7 ? (uint8_t)d : (uint8_t)clamp255(combine(t->aop, aa[0], aa[1], aa[2]) * t->ascale);
        } else {
            for (int k = 0; k < 3; k++) res[k] = (uint8_t)clamp255(combine(t->cop, cc[0][k], cc[1][k], cc[2][k]) * t->cscale);
            res[3] = (uint8_t)clamp255(combine(t->aop, aa[0], aa[1], aa[2]) * t->ascale);
        }
        /* o buffer de combinação usado pelo estágio seguinte é o valor de antes desta atualização */
        memcpy(buf, next_buf, 4);
        if (st < 4) {
            if ((g_tev_upd >> (8 + st)) & 1) memcpy(next_buf, res, 3);
            if ((g_tev_upd >> (12 + st)) & 1) next_buf[3] = res[3];
        }
        memcpy(prev, res, 4);
    }
    memcpy(out, prev, 4);
}

/* ------------------------------------------------------------ framebuffer */
Fb fb_get(void) {
    Fb f;
    f.color = p2v(R(0x11D) * 8); f.depth = p2v(R(0x11C) * 8);
    f.w = R(0x11E) & 0x7FF; f.h = ((R(0x11E) >> 12) & 0x3FF) + 1;
    f.cfmt = (R(0x117) >> 16) & 7; f.dfmt = R(0x116) & 3;
    return f;
}
static uint32_t fb_offset(const Fb *f, int x, int y, int bpp) {
    int yy = f->h - 1 - y;   /* o framebuffer também fica de cabeça para baixo */
    return (uint32_t)(((yy >> 3) * (f->w >> 3) + (x >> 3)) * 64 + morton8(x & 7, yy & 7)) * bpp;
}
static int color_bpp(int fmt) { return fmt == 0 ? 4 : fmt == 1 ? 3 : 2; }
void fb_read(const Fb *f, int x, int y, uint8_t o[4]) {
    uint32_t a = f->color + fb_offset(f, x, y, color_bpp(f->cfmt));
    uint16_t v;
    switch (f->cfmt) {
    case 0: o[0] = MEM8(a + 3); o[1] = MEM8(a + 2); o[2] = MEM8(a + 1); o[3] = MEM8(a); break;
    case 1: o[0] = MEM8(a + 2); o[1] = MEM8(a + 1); o[2] = MEM8(a); o[3] = 255; break;
    case 2: v = MEM16(a); o[0] = ((v >> 11) & 31) * 255 / 31; o[1] = ((v >> 6) & 31) * 255 / 31; o[2] = ((v >> 1) & 31) * 255 / 31; o[3] = (v & 1) * 255; break;
    case 3: v = MEM16(a); o[0] = ((v >> 11) & 31) * 255 / 31; o[1] = ((v >> 5) & 63) * 255 / 63; o[2] = (v & 31) * 255 / 31; o[3] = 255; break;
    default: v = MEM16(a); o[0] = ((v >> 12) & 15) * 17; o[1] = ((v >> 8) & 15) * 17; o[2] = ((v >> 4) & 15) * 17; o[3] = (v & 15) * 17;
    }
}
void fb_write(const Fb *f, int x, int y, const uint8_t c[4], uint32_t wmask) {
    uint8_t old[4]; uint8_t v[4];
    if (wmask == 0xF) memcpy(v, c, 4);
    else { fb_read(f, x, y, old); for (int k = 0; k < 4; k++) v[k] = (wmask >> k) & 1 ? c[k] : old[k]; }
    uint32_t a = f->color + fb_offset(f, x, y, color_bpp(f->cfmt));
    switch (f->cfmt) {
    case 0: MEM8(a) = v[3]; MEM8(a + 1) = v[2]; MEM8(a + 2) = v[1]; MEM8(a + 3) = v[0]; break;
    case 1: MEM8(a) = v[2]; MEM8(a + 1) = v[1]; MEM8(a + 2) = v[0]; break;
    case 2: MEM16(a) = (uint16_t)(((v[0] >> 3) << 11) | ((v[1] >> 3) << 6) | ((v[2] >> 3) << 1) | (v[3] >> 7)); break;
    case 3: MEM16(a) = (uint16_t)(((v[0] >> 3) << 11) | ((v[1] >> 2) << 5) | (v[2] >> 3)); break;
    default: MEM16(a) = (uint16_t)(((v[0] >> 4) << 12) | ((v[1] >> 4) << 8) | ((v[2] >> 4) << 4) | (v[3] >> 4));
    }
}
uint32_t depth_read(const Fb *f, int x, int y) {
    int bpp = f->dfmt == 0 ? 2 : f->dfmt == 2 ? 3 : 4;
    uint32_t a = f->depth + fb_offset(f, x, y, bpp);
    if (f->dfmt == 0) return MEM16(a);
    return MEM8(a) | (MEM8(a + 1) << 8) | (MEM8(a + 2) << 16);
}
void depth_write(const Fb *f, int x, int y, uint32_t d) {
    int bpp = f->dfmt == 0 ? 2 : f->dfmt == 2 ? 3 : 4;
    uint32_t a = f->depth + fb_offset(f, x, y, bpp);
    if (f->dfmt == 0) MEM16(a) = (uint16_t)d;
    else { MEM8(a) = (uint8_t)d; MEM8(a + 1) = (uint8_t)(d >> 8); MEM8(a + 2) = (uint8_t)(d >> 16); }
}
uint8_t stencil_read(const Fb *f, int x, int y) { return MEM8(f->depth + fb_offset(f, x, y, 4) + 3); }
void stencil_write(const Fb *f, int x, int y, uint8_t v) { MEM8(f->depth + fb_offset(f, x, y, 4) + 3) = v; }
static void stencil_op(const Fb *f, int x, int y, uint8_t old, uint32_t op) {
    uint32_t st = R(0x105); uint8_t ref = (st >> 16) & 0xFF, wmask = (st >> 8) & 0xFF, v;
    switch (op) {
    case 0: return;                                   /* manter */
    case 1: v = 0; break; case 2: v = ref; break;
    case 3: v = old == 255 ? 255 : old + 1; break; case 4: v = old == 0 ? 0 : old - 1; break;
    case 5: v = ~old; break; case 6: v = old + 1; break; default: v = old - 1;
    }
    if (!(R(0x115) & 3)) return;
    uint32_t a = f->depth + fb_offset(f, x, y, 4) + 3;
    MEM8(a) = (uint8_t)((old & ~wmask) | (v & wmask));
}
static int cmp_test(uint32_t func, uint32_t a, uint32_t ref) {
    switch (func) {
    case 0: return 0; case 1: return 1; case 2: return a == ref; case 3: return a != ref;
    case 4: return a < ref; case 5: return a <= ref; case 6: return a > ref; default: return a >= ref;
    }
}
static int blend_factor(uint32_t f, int k, const uint8_t s[4], const uint8_t d[4], const uint8_t c[4]) {
    switch (f) {
    case 0: return 0; case 1: return 255;
    case 2: return s[k]; case 3: return 255 - s[k];
    case 4: return d[k]; case 5: return 255 - d[k];
    case 6: return s[3]; case 7: return 255 - s[3];
    case 8: return d[3]; case 9: return 255 - d[3];
    case 10: return c[k]; case 11: return 255 - c[k];
    case 12: return c[3]; case 13: return 255 - c[3];
    default: return k == 3 ? 255 : (s[3] < 255 - d[3] ? s[3] : 255 - d[3]);
    }
}

/* ------------------------------------------------------------ rasterização */

static __thread long g_fs[6]; static __thread long g_fsum[3];   /* estatística de fragmentos (PICA_DRAWLOG): total, scissor, estêncil, profundidade, alfa, escritos */
static void shade_pixel(const Fb *f, int x, int y, float z, const Vertex *v) {
    g_fs[0]++;
    /* scissor */
    uint32_t smode = R(0x65) & 3;
    if (smode == 3) {
        int x0 = R(0x66) & 0x3FF, y0 = (R(0x66) >> 16) & 0x3FF, x1 = R(0x67) & 0x3FF, y1 = (R(0x67) >> 16) & 0x3FF;
        if (x < x0 || x > x1 || y < y0 || y > y1) { g_fs[1]++; return; }
    }
    uint32_t dcm = R(0x107);
    uint32_t dmax = f->dfmt == 0 ? 0xFFFF : 0xFFFFFF;
    float zc = z < 0 ? 0 : z > 1 ? 1 : z;
    if (zc > 0) { int e; float m = frexpf(zc, &e); zc = ldexpf(floorf(m * 131072.0f) / 131072.0f, e); }   /* precisão do float24 */
    uint32_t dz = (uint32_t)(zc * dmax);
    /* stencil (só no formato D24S8) */
    uint32_t st = R(0x105), sop = R(0x106);
    int use_stencil = (st & 1) && f->dfmt == 3 && f->depth;
    uint8_t sval = use_stencil ? stencil_read(f, x, y) : 0;
    if (use_stencil) {
        uint8_t ref = (st >> 16) & 0xFF, imask = (st >> 24) & 0xFF;
        if (!cmp_test((st >> 4) & 7, sval & imask, ref & imask)) { g_fs[2]++; stencil_op(f, x, y, sval, sop & 7); return; }
    }
    if ((dcm & 1) && f->depth) {
        if (!cmp_test((dcm >> 4) & 7, dz, depth_read(f, x, y))) {
            if (use_stencil) stencil_op(f, x, y, sval, (sop >> 4) & 7);
            g_fs[3]++; return;
        }
    }
    if (use_stencil) stencil_op(f, x, y, sval, (sop >> 8) & 7);
    uint8_t prim[4];
    for (int k = 0; k < 4; k++) { float c = v->color[k]; prim[k] = (uint8_t)(c <= 0 ? 0 : c >= 1 ? 255 : c * 255 + 0.5f); }
    uint8_t tex[3][4] = { { 0 } };
    uint32_t tcfg = R(0x80);
    if (tcfg & 1) sample(0, v->tc0[0], v->tc0[1], tex[0]);
    if (tcfg & 2) sample(1, v->tc1[0], v->tc1[1], tex[1]);
    if (tcfg & 4) { const float *t = (tcfg >> 13) & 1 ? v->tc1 : v->tc2; sample(2, t[0], t[1], tex[2]); }
    uint8_t out[4], lprim[4] = { 0, 0, 0, 0 }, lsec[4] = { 0, 0, 0, 0 };
    if (g_light_on) light_fragment(v->quat, v->view, (const uint8_t (*)[4])tex, lprim, lsec);
    tev(prim, (const uint8_t (*)[4])tex, lprim, lsec, out);
    if ((R(0xE0) & 7) == 5) {                /* neblina, como no Azahar: índice = profundidade * 128 */
        float fi = (((R(0xE0) >> 16) & 1) ? 1.0f - zc : zc) * 128.0f;
        int i = (int)fi; if (i < 0) i = 0; if (i > 127) i = 127;
        float fr = fi - (float)i;
        uint32_t e = g_fog_lut[i];
        float val = (float)((e >> 13) & 0x7FF) / 2047.0f, diff = (float)((int32_t)(e << 19) >> 19) / 2047.0f;
        float ff = val + diff * fr; ff = ff < 0 ? 0 : ff > 1 ? 1 : ff;
        uint32_t fc = R(0xE1);
        for (int k = 0; k < 3; k++) { float fcol = (float)((fc >> (8 * k)) & 0xFF); out[k] = (uint8_t)(fcol + (out[k] - fcol) * ff + 0.5f); }
    }
    uint32_t at = R(0x104);
    if ((at & 1) && !cmp_test((at >> 4) & 7, out[3], (at >> 8) & 0xFF)) { g_fs[4]++; return; }
    g_fs[5]++; g_fsum[0] += out[0]; g_fsum[1] += out[1]; g_fsum[2] += out[2];
    uint32_t blend_on = R(0x100) & 0x100, bl = R(0x101);
    /* blending alfa padrão (src*a + dst*(1-a), soma) com alfa 0 não muda o pixel */
    if (blend_on && out[3] == 0 && (bl & 0x0707) == 0 && ((bl >> 16) & 0xFF) == 0x76 && !((dcm >> 12) & 1)) return;
    uint8_t dst[4]; fb_read(f, x, y, dst);
    uint8_t res[4];
    if (blend_on) {                  /* blending */
        uint32_t bl = R(0x101), bcol = R(0x103);
        uint8_t cc[4] = { (uint8_t)bcol, (uint8_t)(bcol >> 8), (uint8_t)(bcol >> 16), (uint8_t)(bcol >> 24) };
        for (int k = 0; k < 4; k++) {
            int alpha = k == 3;
            uint32_t eq = alpha ? (bl >> 8) & 7 : bl & 7;
            uint32_t fs = alpha ? (bl >> 24) & 0xF : (bl >> 16) & 0xF, fd = alpha ? (bl >> 28) & 0xF : (bl >> 20) & 0xF;
            int s = out[k] * blend_factor(fs, k, out, dst, cc) / 255, d = dst[k] * blend_factor(fd, k, out, dst, cc) / 255;
            int r;
            switch (eq) {
            case 1: r = s - d; break; case 2: r = d - s; break;
            case 3: r = out[k] < dst[k] ? out[k] : dst[k]; break; case 4: r = out[k] > dst[k] ? out[k] : dst[k]; break;
            default: r = s + d;
            }
            res[k] = (uint8_t)clamp255(r);
        }
    } else {                         /* operação lógica */
        for (int k = 0; k < 4; k++) {
            uint8_t a = out[k], b = dst[k];
            switch (R(0x102) & 0xF) {
            case 0: res[k] = 0; break; case 1: res[k] = a & b; break; case 2: res[k] = a & ~b; break;
            case 3: res[k] = a; break; case 4: res[k] = 255; break; case 5: res[k] = ~a; break;
            case 6: res[k] = b; break; case 7: res[k] = ~b; break; case 8: res[k] = ~(a & b); break;
            case 9: res[k] = a | b; break; case 10: res[k] = ~(a | b); break; case 11: res[k] = a ^ b; break;
            case 12: res[k] = ~(a ^ b); break; case 13: res[k] = ~a & b; break; case 14: res[k] = a | ~b; break;
            default: res[k] = ~a | b;
            }
        }
    }
    uint32_t wmask = (R(0x113) & 0xF) ? (dcm >> 8) & 0xF : 0;   /* 0x113/0x115: escrita de cor/profundidade permitida */
    if (wmask) fb_write(f, x, y, res, wmask);
    if (((dcm >> 12) & 1) && (R(0x115) & 3) && f->depth) depth_write(f, x, y, dz);
}

static float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static void lerp_vertex(const Vertex *a, const Vertex *b, float t, Vertex *o) {
    const float *pa = (const float *)a, *pb = (const float *)b; float *po = (float *)o;
    for (size_t k = 0; k < sizeof(Vertex) / sizeof(float); k++) po[k] = pa[k] + (pb[k] - pa[k]) * t;
}

/* atributos que o fragmento usa: cor (4), tc0 (2), tc1 (2), tc2 (2); com iluminação, também o
 * quatérnio da normal (4) e o vetor de visão (3) */
#define NV 17
static void frag_attrs(const Vertex *v, float o[NV]) {
    memcpy(o, v->color, 4 * sizeof(float));
    o[4] = v->tc0[0]; o[5] = v->tc0[1]; o[6] = v->tc1[0]; o[7] = v->tc1[1]; o[8] = v->tc2[0]; o[9] = v->tc2[1];
    memcpy(o + 10, v->quat, 4 * sizeof(float)); memcpy(o + 14, v->view, 3 * sizeof(float));
}

/* band/nbands: esta thread desenha só as faixas de 8 linhas com (y/8) % nbands == band */
static void raster_tri(const Fb *f, const SV *v0, const SV *v1, const SV *v2, int band, int nbands) {
    float area = edge(v0->x, v0->y, v1->x, v1->y, v2->x, v2->y);
    if (area == 0) return;
    static int nocull = -1; if (nocull < 0) nocull = ENVC("PICA_NOCULL") != NULL;
    uint32_t cull = nocull ? 0 : R(0x40) & 3;
    /* 0x40: 1 = mantém só os horários (área < 0), 2 = só os anti-horários (área > 0) */
    if (cull == 1 && area > 0) return;
    if (cull == 2 && area < 0) return;
    if (area < 0) { const SV *t = v1; v1 = v2; v2 = t; area = -area; }
    int minx = (int)floorf(fminf(v0->x, fminf(v1->x, v2->x))), maxx = (int)ceilf(fmaxf(v0->x, fmaxf(v1->x, v2->x)));
    int miny = (int)floorf(fminf(v0->y, fminf(v1->y, v2->y))), maxy = (int)ceilf(fmaxf(v0->y, fmaxf(v1->y, v2->y)));
    if (minx < 0) minx = 0; if (miny < 0) miny = 0;
    if (maxx > f->w - 1) maxx = f->w - 1; if (maxy > f->h - 1) maxy = f->h - 1;
    if (minx > maxx || miny > maxy) return;
    if (band == 0) g_pica_tris++;
    /* atributos pré-multiplicados por 1/w (correção de perspectiva) */
    float a0[NV], a1[NV], a2[NV];
    int nv = g_light_on ? NV : 10;
    frag_attrs(&v0->v, a0); frag_attrs(&v1->v, a1); frag_attrs(&v2->v, a2);
    if (g_light_on) {   /* q e -q são a mesma rotação: alinha os sinais antes de interpolar */
        float d1 = 0, d2 = 0; for (int k = 10; k < 14; k++) { d1 += a0[k] * a1[k]; d2 += a0[k] * a2[k]; }
        if (d1 < 0) for (int k = 10; k < 14; k++) a1[k] = -a1[k];
        if (d2 < 0) for (int k = 10; k < 14; k++) a2[k] = -a2[k];
    }
    for (int k = 0; k < nv; k++) { a0[k] *= v0->invw; a1[k] *= v1->invw; a2[k] *= v2->invw; }
    float inv_area = 1.0f / area;
    /* funções de borda incrementais: e(x+1) = e(x) + dx, e(y+1) = e(y) + dy */
    float e0dx = -(v2->y - v1->y), e0dy = (v2->x - v1->x);
    float e1dx = -(v0->y - v2->y), e1dy = (v0->x - v2->x);
    float e2dx = -(v1->y - v0->y), e2dy = (v1->x - v0->x);
    float px0 = minx + 0.5f, py0 = miny + 0.5f;
    float r0 = edge(v1->x, v1->y, v2->x, v2->y, px0, py0), r1 = edge(v2->x, v2->y, v0->x, v0->y, px0, py0), r2 = edge(v0->x, v0->y, v1->x, v1->y, px0, py0);
    Vertex pv; memset(&pv, 0, sizeof pv);
    int proj = ((R(0x83) >> 28) & 7) == 3;
    for (int y = miny; y <= maxy; y++, r0 += e0dy, r1 += e1dy, r2 += e2dy) {
        if (nbands > 1 && ((y >> 3) % nbands) != band) continue;
        float w0 = r0, w1 = r1, w2 = r2;
        for (int x = minx; x <= maxx; x++, w0 += e0dx, w1 += e1dx, w2 += e2dx) {
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float b0 = w0 * inv_area, b1 = w1 * inv_area, b2 = w2 * inv_area;
            float z = b0 * v0->z + b1 * v1->z + b2 * v2->z;
            float iw = 1.0f / (b0 * v0->invw + b1 * v1->invw + b2 * v2->invw);
            float t[NV];
            for (int k = 0; k < nv; k++) t[k] = (b0 * a0[k] + b1 * a1[k] + b2 * a2[k]) * iw;
            memcpy(pv.color, t, 4 * sizeof(float));
            pv.tc0[0] = t[4]; pv.tc0[1] = t[5]; pv.tc1[0] = t[6]; pv.tc1[1] = t[7]; pv.tc2[0] = t[8]; pv.tc2[1] = t[9];
            if (proj) {                       /* textura projetiva (0x83 tipo 3): tc0 / tc0w */
                float tw = (b0 * v0->v.tc0w * v0->invw + b1 * v1->v.tc0w * v1->invw + b2 * v2->v.tc0w * v2->invw) * iw;
                if (tw != 0) { pv.tc0[0] /= tw; pv.tc0[1] /= tw; }
            }
            if (g_light_on) { memcpy(pv.quat, t + 10, 4 * sizeof(float)); memcpy(pv.view, t + 14, 3 * sizeof(float)); }
            shade_pixel(f, x, y, z, &pv);
        }
    }
}

/* viewport e profundidade (0x41, 0x43, 0x68, 0x4D, 0x4E), lidos uma vez por desenho (vp_load) */
static __thread struct { float hx, hy, ox, oy, zs, zo; } g_vp;
static void vp_load(void) {
    g_vp.hx = f24_to_f32(R(0x41)); g_vp.hy = f24_to_f32(R(0x43));
    g_vp.ox = (float)((int32_t)(R(0x68) << 22) >> 22); g_vp.oy = (float)((int32_t)((R(0x68) >> 16) << 22) >> 22);
    g_vp.zs = f24_to_f32(R(0x4D)); g_vp.zo = f24_to_f32(R(0x4E));
}
static inline void to_screen(const Vertex *v, SV *s) {
    float invw = 1.0f / v->pos[3];
    s->x = (v->pos[0] * invw + 1.0f) * g_vp.hx + g_vp.ox;
    s->y = (v->pos[1] * invw + 1.0f) * g_vp.hy + g_vp.oy;
    s->z = g_vp.zo + g_vp.zs * (v->pos[2] * invw);
    s->invw = invw;
    s->v = *v;
}

/* ------------------------------------------------------------ fila de triângulos e threads
 * Os triângulos de um desenho vão para uma fila; no fim do desenho a fila é rasterizada em paralelo,
 * cada thread com as suas faixas de linhas da tela (pixels diferentes = sem conflito; a ordem dos
 * triângulos dentro de cada pixel é a mesma do desenho em série). */
static Tri *g_tq; static int g_ntq, g_captq; static long g_tq_area;
static void tq_push(const Fb *f, const SV *a, const SV *b, const SV *c) {
    if (g_ntq == g_captq) { g_captq = g_captq ? g_captq * 2 : 1024; g_tq = realloc(g_tq, sizeof(Tri) * g_captq); }
    Tri *t = &g_tq[g_ntq++]; t->v[0] = *a; t->v[1] = *b; t->v[2] = *c;
    float x0 = fminf(a->x, fminf(b->x, c->x)), x1 = fmaxf(a->x, fmaxf(b->x, c->x));
    float y0 = fminf(a->y, fminf(b->y, c->y)), y1 = fmaxf(a->y, fmaxf(b->y, c->y));
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > f->w) x1 = f->w; if (y1 > f->h) y1 = f->h;
    if (x1 > x0 && y1 > y0) g_tq_area += (long)((x1 - x0) * (y1 - y0)) / 2;
}
static int g_nbands = -1;                         /* threads do grupo (incluindo a que chama) */
static pthread_mutex_t g_pm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pc_go = PTHREAD_COND_INITIALIZER;
static Fb g_job_fb;
typedef void (*JobFn)(int blk, int nblk);
static JobFn g_job_fn;
static void run_band(const Fb *f, int band, int nbands) {
    for (int i = 0; i < g_ntq; i++) raster_tri(f, &g_tq[i].v[0], &g_tq[i].v[1], &g_tq[i].v[2], band, nbands);
}
/* grupo de threads com distribuição dinâmica: o trabalho é dividido em blocos e cada thread
 * (inclusive a que chama) pega o próximo bloco livre com uma operação atômica. Quem chama não
 * espera threads que acordaram tarde, só os blocos já pegos. As threads ficam um tempo girando
 * antes de dormir, porque os desenhos chegam em rajadas (acordar uma thread no Windows é lento). */
static volatile uint64_t g_job_word;               /* geração (24 bits) | nº de blocos (20) | próximo bloco (20) */
static volatile int g_job_done;                    /* blocos terminados */
static int g_sleepers;
#define JW(gen, nb, nx) (((uint64_t)(gen) << 40) | ((uint64_t)(nb) << 20) | (uint64_t)(nx))
static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}
/* pega e executa blocos do trabalho atual; devolve quantos executou */
static int job_work(void) {
    int n = 0;
    for (;;) {
        uint64_t w = __atomic_load_n(&g_job_word, __ATOMIC_ACQUIRE);
        uint32_t nb = (w >> 20) & 0xFFFFF, nx = w & 0xFFFFF;
        if (nx >= nb) return n;
        if (!__atomic_compare_exchange_n(&g_job_word, &w, w + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
        g_job_fn((int)nx, (int)nb);              /* g_job_fn não muda enquanto houver bloco pendente */
        __atomic_fetch_add(&g_job_done, 1, __ATOMIC_RELEASE);
        n++;
    }
}
static void *raster_worker(void *arg) {
    (void)arg;
    uint64_t seen = 0;
    for (;;) {
        int spins = 0;
        for (;;) {
            uint64_t w = __atomic_load_n(&g_job_word, __ATOMIC_ACQUIRE);
            if ((w >> 40) != (seen >> 40)) { seen = w; break; }
            if (++spins < 20000) { cpu_relax(); continue; }
            pthread_mutex_lock(&g_pm);
            g_sleepers++;
            while ((__atomic_load_n(&g_job_word, __ATOMIC_ACQUIRE) >> 40) == (seen >> 40)) pthread_cond_wait(&g_pc_go, &g_pm);
            g_sleepers--;
            pthread_mutex_unlock(&g_pm);
            spins = 0;
        }
        job_work();
    }
    return NULL;
}
static void raster_init(void) {
    const char *e = ENVC("PICA_THREADS");
    int n = e ? atoi(e) : plat_cpu_count() - 2;   /* deixa núcleos para o jogo e a thread da GPU */
    if (n > 12) n = 12;
    if (n < 1) n = 1;
    g_nbands = n;
    for (int i = 1; i < n; i++) {
        pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 1 << 20);
        if (pthread_create(&t, &at, raster_worker, (void *)(intptr_t)i)) { g_nbands = i; break; }
    }
    fprintf(stderr, "[pica] vértices/rasterização em %d thread(s)\n", g_nbands);
}
/* executa fn(bloco, nblocos) para todos os blocos, distribuídos entre as threads, e espera */
static void run_parallel_n(JobFn fn, int nblk) {
    if (g_nbands < 0) raster_init();
    if (g_nbands == 1 || nblk <= 1) { for (int k = 0; k < nblk; k++) fn(k, nblk); return; }
    static uint32_t gen;
    gen = (gen + 1) & 0xFFFFFF;
    g_job_fn = fn; g_job_done = 0;
    __atomic_store_n(&g_job_word, JW(gen, nblk, 0), __ATOMIC_RELEASE);
    pthread_mutex_lock(&g_pm);
    if (g_sleepers) pthread_cond_broadcast(&g_pc_go);
    pthread_mutex_unlock(&g_pm);
    job_work();
    while (__atomic_load_n(&g_job_done, __ATOMIC_ACQUIRE) < nblk) cpu_relax();
}
static void run_parallel(JobFn fn) { if (g_nbands < 0) raster_init(); run_parallel_n(fn, g_nbands); }
static void raster_job(int band, int nbands) { run_band(&g_job_fb, band, nbands); }
static void tq_flush(const Fb *f) {
    if (g_irec) irec_flush(f);
    if (!g_ntq) return;
    if (pica_gl_active()) { pica_gl_draw(f, g_tq, g_ntq); g_ntq = 0; g_tq_area = 0; return; }
    if (g_nbands < 0) raster_init();
    if (g_nbands == 1 || g_tq_area < 3000) run_band(f, 0, 1);      /* desenho pequeno: não compensa acordar as threads */
    else { g_job_fb = *f; run_parallel(raster_job); }
    g_ntq = 0; g_tq_area = 0;
}

/* recorte contra w > 0 (plano próximo) e desenho */
static void tq_area(const Fb *f, const Tri *t) {
    const SV *a = &t->v[0], *b = &t->v[1], *c = &t->v[2];
    float x0 = fminf(a->x, fminf(b->x, c->x)), x1 = fmaxf(a->x, fmaxf(b->x, c->x));
    float y0 = fminf(a->y, fminf(b->y, c->y)), y1 = fmaxf(a->y, fmaxf(b->y, c->y));
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > f->w) x1 = f->w; if (y1 > f->h) y1 = f->h;
    if (x1 > x0 && y1 > y0) g_tq_area += (long)((x1 - x0) * (y1 - y0)) / 2;
}
static inline int inside_all(const Vertex *v) { return v->pos[3] >= 1e-5f && v->pos[2] + v->pos[3] >= 0 && v->pos[2] <= 0; }
static void clip_draw(const Fb *f, const Vertex *a, const Vertex *b, const Vertex *c) {
    const float eps = 1e-5f;
    if (g_irec) irec_tri(a, b, c);
    if (inside_all(a) && inside_all(b) && inside_all(c)) {   /* caso comum: nada a recortar, vai direto para a fila */
        if (g_ntq == g_captq) { g_captq = g_captq ? g_captq * 2 : 1024; g_tq = realloc(g_tq, sizeof(Tri) * g_captq); }
        Tri *t = &g_tq[g_ntq++];
        to_screen(a, &t->v[0]); to_screen(b, &t->v[1]); to_screen(c, &t->v[2]);
        if (!pica_gl_active()) tq_area(f, t);
        return;
    }
    Vertex in[9] = { *a, *b, *c }, out[9]; int n = 3, m = 0;
    /* plano z <= 0 do espaço de recorte da PICA (z entre -w e 0) e w > eps */
    for (int plane = 0; plane < 3; plane++) {
        m = 0;
        for (int i = 0; i < n; i++) {
            const Vertex *p = &in[i], *q = &in[(i + 1) % n];
            float dp, dq;
            if (plane == 0) { dp = p->pos[3] - eps; dq = q->pos[3] - eps; }                 /* w >= eps */
            else if (plane == 1) { dp = p->pos[2] + p->pos[3]; dq = q->pos[2] + q->pos[3]; } /* z >= -w */
            else { dp = -p->pos[2]; dq = -q->pos[2]; }                                      /* z <= 0 */
            if (dp >= 0) out[m++] = *p;
            if ((dp >= 0) != (dq >= 0) && m < 9) lerp_vertex(p, q, dp / (dp - dq), &out[m++]);
        }
        memcpy(in, out, sizeof(Vertex) * m); n = m;
        if (n < 3) return;
    }
    SV s[9]; for (int i = 0; i < n; i++) to_screen(&in[i], &s[i]);
    for (int i = 1; i + 1 < n; i++) tq_push(f, &s[0], &s[i], &s[i + 1]);
}

/* ------------------------------------------------------------ shader de geometria
 * Modos (0x252): 0 = ponto (as saídas do shader de vértice enchem as entradas do de geometria),
 * 1 = primitiva variável (o 1º índice dá o número de vértices; vão para os uniforms), 2 = primitiva fixa
 * (n vértices por execução, nos uniforms a partir de start_index). As primitivas saem pelo EMIT, de 3 em 3
 * vértices. Lógica igual à do Citra/Azahar (geometry_pipeline.cpp). Usado para partículas e efeitos. */
static Fb g_gs_fb; static float g_em_buf[3][16][4];
static void gs_emit(Emitter *e, float out[16][4]) {
    uint32_t mask = R(0x28D) & 0xFFFF; int n = 0;
    int vid = e->vertex_id < 3 ? e->vertex_id : 2;
    for (int o = 0; o < 16; o++) if ((mask >> o) & 1) memcpy(g_em_buf[vid][n++], out[o], sizeof(float) * 4);
    if (!e->prim_emit) return;
    Vertex v[3];
    for (int k = 0; k < 3; k++) slots_to_vertex(g_em_buf[k], n, &v[k], 1);
    if (e->winding) clip_draw(&g_gs_fb, &v[1], &v[0], &v[2]);
    else clip_draw(&g_gs_fb, &v[0], &v[1], &v[2]);
}
static ShaderRun g_gs_run;                 /* o estado da unidade de geometria persiste entre execuções */
static void gs_draw(const Fb *f, int indexed, uint32_t count, uint32_t first, uint32_t ib, int ib16) {
    g_gs_fb = *f; vp_load();
    int vs_n = (int)(R(0x24A) & 0xF) + 1;
    uint32_t cfg = R(0x252); int mode = cfg & 0xFF;
    int gs_in_n = (int)(R(0x289) & 0xF) + 1;
    uint64_t gmap = ((uint64_t)R(0x28C) << 32) | R(0x28B);
    int fixed_n = (int)((cfg >> 8) & 0xF) + 1, start = (int)((cfg >> 16) & 0xFF);
    float buf[64][4]; int bn = 0;
    int need_index = 1, main_left = 0; uint32_t total_left = 0;
    Emitter em = { 0, 0, 0, gs_emit };
    static int logged; if (!logged && ENVC("PICA_GSLOG")) { logged = 1; fprintf(stderr, "[gs] modo %d, %d saídas do VS, %d entradas, fixo %d a partir de c%d, entrada %03x\n", mode, vs_n, gs_in_n, fixed_n, start, g_gs.entry); }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = indexed ? (ib16 ? MEM16(ib + 2 * i) : MEM8(ib + i)) : first + i;
        if (mode == 1 && need_index) {
            for (int k = 0; k < 4; k++) g_gs.uniform_f[0][k] = (float)idx;
            bn = 1; main_left = (int)R(0x254) + 1; total_left = idx; need_index = 0;
            if (!total_left) need_index = 1;
            continue;
        }
        float attr[12][4], packed[16][4];
        int na = load_vertex_attrs(idx, attr);
        vs_packed(attr, na, packed);
        int run = 0;
        if (mode == 0) {
            for (int k = 0; k < vs_n && bn < 64; k++) memcpy(buf[bn++], packed[k], sizeof packed[k]);
            if (bn >= gs_in_n) {
                for (int a = 0; a < gs_in_n; a++) memcpy(g_gs_run.in[(gmap >> (4 * a)) & 0xF], buf[a], sizeof buf[a]);
                bn = 0; run = 1;
            }
        } else if (mode == 2) {
            for (int k = 0; k < vs_n; k++, bn++) if (start + bn < 96) memcpy(g_gs.uniform_f[start + bn], packed[k], sizeof packed[k]);
            if (bn >= vs_n * fixed_n) { bn = 0; run = 1; }
        } else {
            if (main_left) { for (int k = 0; k < vs_n; k++, bn++) if (bn < 96) memcpy(g_gs.uniform_f[bn], packed[k], sizeof packed[k]); main_left--; }
            else { if (bn < 96) memcpy(g_gs.uniform_f[bn], packed[0], sizeof packed[0]); bn++; }
            if (--total_left == 0) { need_index = 1; run = 1; }
        }
        if (run) {
            sh_run(&g_gs, &g_gs_run, &em);
            g_gs.uniform_b |= 0x8000;      /* b15 = 1 depois de cada execução */
        }
    }
}

static long drawlog_n;
#define DLMAX (ENVC("PICA_DRAWLOG_N") ? atol(ENVC("PICA_DRAWLOG_N")) : 400)   /* quantos desenhos registrar */
/* registro de desenhos: PICA_DRAWLOG=quadro, ou criando o arquivo drawlog_agora.txt com o jogo rodando */
long g_drawlog_frame = -2;
int drawlog_active(void) {
    extern long g_gsp_frames;
    if (g_drawlog_frame == -2) g_drawlog_frame = ENVC("PICA_DRAWLOG") ? atol(ENVC("PICA_DRAWLOG")) : -1;
    return g_drawlog_frame >= 0 && g_gsp_frames >= g_drawlog_frame;
}
void drawlog_restart(long frame) { g_drawlog_frame = frame; drawlog_n = 0; }
static Vertex g_vc[65536]; static uint32_t g_vc_stamp[65536], g_vc_mark[65536], g_vc_gen;
static uint32_t *g_uniq; static uint32_t g_uniq_cap; static int g_nuniq;
static Vertex *g_vjob_out; static uint32_t g_vjob_first;
/* shader de vértice em lotes de SB_L vértices (interpretador SIMD); se o lote divergir, um a um */
static void vertex_batch(const uint32_t *idx, int n, Vertex **out) {
    static __thread ShaderRunB b;
    uint64_t map = ((uint64_t)R(0x2BC) << 32) | R(0x2BB);
    uint32_t omask = R(0x2BD) & 0xFFFF;
    memset(b.in, 0, sizeof b.in); memset(b.out, 0, sizeof b.out);
    for (int l = 0; l < SB_L; l++) {
        float attr[12][4];
        int na = load_vertex_attrs(idx[l < n ? l : 0], attr);
        for (int a = 0; a < na && a < 12; a++) { int q = (map >> (4 * a)) & 0xF; for (int c = 0; c < 4; c++) b.in[q][c][l] = attr[a][c]; }
    }
    if (!sh_run_batch(&g_vs, &b)) { for (int l = 0; l < n; l++) vertex(idx[l], out[l]); return; }
    static int chk = -1; if (chk < 0) chk = ENVC("PICA_VSCHECK") != NULL;
    for (int l = 0; l < n; l++) {
        float packed[16][4]; int np = 0;
        for (int o = 0; o < 16; o++) if ((omask >> o) & 1) { for (int c = 0; c < 4; c++) packed[np][c] = b.out[o][c][l]; np++; }
        slots_to_vertex(packed, np, out[l], 0);
        if (chk) {                                /* PICA_VSCHECK=1: confere o lote com o interpretador escalar */
            Vertex ref; vertex(idx[l], &ref);
            static int nbad;
            if (memcmp(&ref, out[l], sizeof ref) && nbad++ < 20) {
                { const float *x = (const float *)out[l], *y = (const float *)&ref; int k = 0; while (k < 22 && !memcmp(&x[k], &y[k], 4)) k++;
                  fprintf(stderr, "[vscheck] vértice %u (lane %d/%d) difere no campo %d: %g vs %g\n", idx[l], l, n, k, x[k], y[k]);
                  ShaderRun sr; memset(&sr, 0, sizeof sr);
                  for (int r = 0; r < 16; r++) for (int c = 0; c < 4; c++) sr.in[r][c] = b.in[r][c][l];
                  vs_run(&sr);
                  int bo = 0; for (int r = 0; r < 16; r++) for (int c = 0; c < 4; c++) if (memcmp(&sr.out[r][c], &b.out[r][c][l], 4)) bo++;
                  float attr[12][4]; int na = load_vertex_attrs(idx[l], attr); int bi = 0;
                  for (int a = 0; a < na && a < 12; a++) { int q = (map >> (4 * a)) & 0xF; for (int c = 0; c < 4; c++) if (memcmp(&b.in[q][c][l], &attr[a][c], 4)) bi++; }
                  fprintf(stderr, "[vscheck]   saídas diferentes com as mesmas entradas: %d; entradas diferentes: %d; omask %04x\n", bo, bi, omask); }
                if (nbad == 1) {
                    FILE *o = fopen("vscheck.bin", "wb");
                    if (o) {
                        fwrite(g_vs.code, 4, 4096, o); fwrite(g_vs.opdesc, 4, 128, o); fwrite(g_vs.uniform_f, 4, 96 * 4, o);
                        fwrite(g_vs.uniform_i, 1, 16, o); fwrite(&g_vs.uniform_b, 2, 1, o); fwrite(&g_vs.entry, 4, 1, o);
                        uint32_t cnt = (uint32_t)n; fwrite(&cnt, 4, 1, o);
                        for (int q = 0; q < n; q++) { float in[16][4]; for (int r = 0; r < 16; r++) for (int c = 0; c < 4; c++) in[r][c] = b.in[r][c][q]; fwrite(in, 4, 64, o); }
                        fclose(o);
                    }
                }
            }
        }
    }
}
static void vertex_job(int k, int nblk) {
    (void)nblk;
    int j0 = k * SB_L, n = g_nuniq - j0 < SB_L ? g_nuniq - j0 : SB_L;
    uint32_t idx[SB_L]; Vertex *out[SB_L];
    for (int l = 0; l < n; l++) {
        if (g_vjob_out) { idx[l] = g_vjob_first + (uint32_t)(j0 + l); out[l] = &g_vjob_out[j0 + l]; }
        else { idx[l] = g_uniq[j0 + l]; out[l] = &g_vc[idx[l]]; }
    }
    if (n == 1) vertex(idx[0], out[0]); else vertex_batch(idx, n, out);
}
static void draw(int indexed) {
    g_pica_draws++;
    Fb f = fb_get();
    static long dbg_frame = -1; extern long g_gsp_frames;
    /* PICA_DRAWLOG=quadro: registra os 400 primeiros desenhos a partir desse quadro */
    if (drawlog_active() && drawlog_n < DLMAX && ENVC("PICA_DRAWLOG_TEV")) {
        fprintf(stderr, "[tev] buf %08x cor %08x |", R(0xE0), R(0xFD));
        static const uint32_t tb[6] = { 0xC0, 0xC8, 0xD0, 0xD8, 0xF0, 0xF8 };
        for (int st = 0; st < 6; st++) fprintf(stderr, " %d:%08x/%08x/%08x/%08x/%08x", st, R(tb[st]), R(tb[st] + 1), R(tb[st] + 2), R(tb[st] + 3), R(tb[st] + 4));
        fprintf(stderr, " | 4d=%g 4e=%g 6d=%08x 4c=%08x", f24_to_f32(R(0x4D)), f24_to_f32(R(0x4E)), R(0x6D), R(0x4C));
        fprintf(stderr, " | tex1 %08x fmt %u tex2 %08x | 104=%08x 105=%08x 106=%08x\n", p2v(R(0x95) * 8), R(0x96) & 0xF, p2v(R(0x9D) * 8), R(0x104), R(0x105), R(0x106));
    }
    if (drawlog_active() && drawlog_n++ < DLMAX)
        fprintf(stderr, "[draw] fb %08x %dx%d gs=%08x/%08x n=%u topo=%u tex0 %08x %ux%u fmt %u cfg %08x | tev3 src %08x op %08x cmb %08x | blend %08x %08x alpha %08x sten %08x dcm %08x fixedmask %03x\n",
                f.color, f.w, f.h, R(0x229), R(0x244), R(0x228), (R(0x25E) >> 8) & 3, p2v(R(0x85) * 8), (R(0x82) >> 16) & 0x7FF, R(0x82) & 0x7FF, R(0x8E) & 0xF, R(0x83),
                R(0xD8), R(0xD9), R(0xDA), R(0x100), R(0x101), R(0x104), R(0x105), R(0x107), (R(0x202) >> 16) & 0xFFF);
    (void)dbg_frame;
    if (ENVC("PICA_DUMPTEX") && g_gsp_frames >= atol(ENVC("PICA_DUMPTEX")) && g_gsp_frames < atol(ENVC("PICA_DUMPTEX")) + (ENVC("PICA_DUMPTEX_N") ? atol(ENVC("PICA_DUMPTEX_N")) : 1) && (R(0x80) & 1)) {
        /* salva a textura 0 deste desenho como PPM (cor) + PGM (alfa) */
        uint32_t addr = p2v(R(0x85) * 8), fmt = R(0x8E) & 0xF; int w = (R(0x82) >> 16) & 0x7FF, h = R(0x82) & 0x7FF;
        char pth[256]; snprintf(pth, sizeof pth, "tex_%08x_%dx%d_f%u.ppm", addr, w, h, fmt);
        FILE *o = fopen(pth, "wb");
        if (o && w && h && w <= 1024 && h <= 1024) {
            fprintf(o, "P6\n%d %d\n255\n", w * 2, h);
            for (int y = 0; y < h; y++) for (int x = 0; x < 2 * w; x++) {
                uint8_t t[4]; texel(addr, fmt, w, x % w, y, t);
                if (x < w) fwrite(t, 1, 3, o); else { uint8_t a[3] = { t[3], t[3], t[3] }; fwrite(a, 1, 3, o); }
            }
        }
        if (o) fclose(o);
    }
    if (ENVC("PICA_DEBUG") && g_pica_draws < 40)
        fprintf(stderr, "[pica] desenho: fb cor %08x prof %08x %dx%d fmt %d/%d viewport %.1fx%.1f+(%d,%d) n=%u topo=%u tex0 %08x %ux%u fmt %u\n",
                f.color, f.depth, f.w, f.h, f.cfmt, f.dfmt, f24_to_f32(R(0x41)), f24_to_f32(R(0x43)), R(0x68) & 0x3FF, (R(0x68) >> 16) & 0x3FF,
                R(0x228), (R(0x25E) >> 8) & 3, p2v(R(0x85) * 8), (R(0x82) >> 16) & 0x7FF, R(0x82) & 0x7FF, R(0x8E) & 0xF);
    { extern int g_oplog; if (g_oplog) fprintf(stderr, "[op] draw fb %08x/%08x %dx%d n=%u topo=%u gs=%d tex0 %08x %ux%u vs=%03x\n", f.color, f.depth, f.w, f.h, R(0x228), (R(0x25E) >> 8) & 3, (R(0x229) & 3) == 2,
                                    p2v(R(0x85) * 8), (R(0x82) >> 16) & 0x7FF, R(0x82) & 0x7FF, g_vs.entry); }
    if (!f.color || !f.w) return;
    uint32_t count = R(0x228), first = R(0x22A);
    uint32_t topo = (R(0x25E) >> 8) & 3;
    uint32_t base = (R(0x200) & 0x1FFFFFFE) << 3;
    uint32_t ib = p2v(base + (R(0x227) & 0x7FFFFFFF)); int ib16 = R(0x227) >> 31;
    if (count > 100000) return;
    ap_build();
    if (!pica_gl_active()) { tev_setup(); tex_setup(); } light_setup();
    { static int done; const char *bf = ENVC("PICA_VSDUMP");   /* PICA_VSDUMP=quadro: salva programa + entradas de alguns desenhos (benchmark do interpretador) */
      extern long g_gsp_frames;
      if (bf && done < 8 && count >= 200 && (R(0x229) & 3) != 2 && g_gsp_frames >= atol(bf)) {
        char pth[64]; snprintf(pth, sizeof pth, "vsdump_%d.bin", done++);
        FILE *o = fopen(pth, "wb");
        if (o) {
            fwrite(g_vs.code, 4, 4096, o); fwrite(g_vs.opdesc, 4, 128, o); fwrite(g_vs.uniform_f, 4, 96 * 4, o);
            fwrite(g_vs.uniform_i, 1, 16, o); fwrite(&g_vs.uniform_b, 2, 1, o); fwrite(&g_vs.entry, 4, 1, o);
            fwrite(&count, 4, 1, o);
            uint64_t map = ((uint64_t)R(0x2BC) << 32) | R(0x2BB);
            for (uint32_t i = 0; i < count; i++) {
                uint32_t idx = indexed ? (ib16 ? MEM16(ib + 2 * i) : MEM8(ib + i)) : first + i;
                float attr[12][4], in[16][4]; memset(in, 0, sizeof in);
                int na = load_vertex_attrs(idx, attr);
                for (int k = 0; k < na && k < 12; k++) memcpy(in[(map >> (4 * k)) & 0xF], attr[k], sizeof attr[k]);
                fwrite(in, 4, 64, o);
            }
            fclose(o);
        } } }
    if ((R(0x229) & 3) == 2) {             /* shader de geometria */
        static int off = -1; if (off < 0) off = ENVC("PICA_SEM_GS") != NULL;
        if (!off) gs_draw(&f, indexed, count, first, ib, ib16);
        tq_flush(&f);
        return;
    }
    Vertex *vs = malloc(sizeof(Vertex) * (count ? count : 1));
    if (++g_vc_gen == 0) { memset(g_vc_stamp, 0, sizeof g_vc_stamp); memset(g_vc_mark, 0, sizeof g_vc_mark); g_vc_gen = 1; }
    /* cache simples: o mesmo índice gera o mesmo vértice */
    extern int g_vs_trace; static int traced;
    uint64_t t_v0 = prof_now();
    /* shader de vértice em paralelo: cada thread processa uma parte dos vértices distintos */
    int pre = !ENVC("PICA_VSTRACE");
    if (pre) {
        if (indexed) {
            if (count > g_uniq_cap) { g_uniq_cap = count; g_uniq = realloc(g_uniq, sizeof(uint32_t) * count); }
            g_nuniq = 0;
            uint32_t gen = g_vc_gen + 1; if (gen == 0) gen = 1;          /* marca provisória: "já na lista" */
            for (uint32_t i = 0; i < count; i++) {
                uint32_t idx = ib16 ? MEM16(ib + 2 * i) : MEM8(ib + i);
                if (g_vc_mark[idx] != gen) { g_vc_mark[idx] = gen; g_uniq[g_nuniq++] = idx; }
            }
            g_vjob_out = NULL;
        } else { g_nuniq = (int)count; g_vjob_first = first; g_vjob_out = vs; }
        sh_prepare(&g_vs, 512);
        { int nblk = (g_nuniq + SB_L - 1) / SB_L;
          if (nblk >= 4) run_parallel_n(vertex_job, nblk); else for (int k = 0; k < nblk; k++) vertex_job(k, nblk); }
        if (indexed) for (int j = 0; j < g_nuniq; j++) g_vc_stamp[g_uniq[j]] = g_vc_gen;
    }
    for (uint32_t i = 0; i < count && !(pre && !indexed); i++) {
        uint32_t idx = indexed ? (ib16 ? MEM16(ib + 2 * i) : MEM8(ib + i)) : first + i;
        if (i == 0 && !traced && ENVC("PICA_VSTRACE") && g_light_on &&
            (ENVC("PICA_VSTRACE_N") ? count == (uint32_t)atoi(ENVC("PICA_VSTRACE_N")) : count > 1000) &&
            g_gsp_frames >= atol(ENVC("PICA_VSTRACE"))) {
            traced = 1; g_vs_trace = 1;
            fprintf(stderr, "[vs] entrada %03x, mapa %08x %08x, saídas %04x\n", g_vs.entry, R(0x2BB), R(0x2BC), R(0x2BD));
            for (int u = 0; u < 96; u++) fprintf(stderr, "[vs] c%d = %g %g %g %g\n", u, g_vs.uniform_f[u][0], g_vs.uniform_f[u][1], g_vs.uniform_f[u][2], g_vs.uniform_f[u][3]);
            fprintf(stderr, "[vs] base %08x (reg %08x) formatos %08x %08x idx %u ib %08x ib16 %d first %u\n", base, R(0x200), R(0x201), R(0x202), idx, ib, ib16, first);
            for (int b = 0; b < 12; b++) if (R(0x205 + 3 * b) >> 28) {
                uint32_t off = R(0x203 + 3 * b), st = (R(0x205 + 3 * b) >> 16) & 0xFF;
                fprintf(stderr, "[vs] buffer %d: off %08x comps %08x %08x stride %u ->", b, off, R(0x204 + 3 * b), R(0x205 + 3 * b), st);
                for (int k = 0; k < 8; k++) fprintf(stderr, " %08x", MEM32(p2v(base + off + idx * st) + 4 * k));
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "[vs] ints %08x %08x %08x %08x bools %04x\n", *(uint32_t *)g_vs.uniform_i[0], *(uint32_t *)g_vs.uniform_i[1], *(uint32_t *)g_vs.uniform_i[2], *(uint32_t *)g_vs.uniform_i[3], g_vs.uniform_b);
        }
        if (indexed && !g_vs_trace) {               /* o mesmo índice gera o mesmo vértice */
            if (g_vc_stamp[idx] == g_vc_gen) { if (!pre) vs[i] = g_vc[idx]; continue; }
            vertex(idx, &vs[i]); g_vc[idx] = vs[i]; g_vc_stamp[idx] = g_vc_gen; continue;
        }
        vertex(idx, &vs[i]);
        if (g_vs_trace) {
            g_vs_trace = 0;
            fprintf(stderr, "[vs] posição (%g %g %g %g)\n", vs[i].pos[0], vs[i].pos[1], vs[i].pos[2], vs[i].pos[3]);
        }
    }
    { static int nsum; const char *dv = ENVC("PICA_DUMPV");   /* PICA_DUMPV=n1,n2,...: resumo dos desenhos com esses números de vértices */
      int match = 0; if (dv) { const char *q = dv; while (*q) { if ((uint32_t)atoi(q) == count) match = 1; while (*q && *q != ',') q++; if (*q) q++; } }
      if (match && nsum++ < 400) {
        int nan = 0, wneg = 0, in = 0; float mx[4] = { 1e30f, 1e30f, 1e30f, 1e30f }, Mx[4] = { -1e30f, -1e30f, -1e30f, -1e30f };
        for (uint32_t i = 0; i < count; i++) {
            float *p = vs[i].pos;
            if (p[0] != p[0] || p[1] != p[1] || p[2] != p[2] || p[3] != p[3]) { nan++; continue; }
            if (p[3] <= 0) { wneg++; continue; }
            float nx = p[0] / p[3], ny = p[1] / p[3];
            if (nx > -1 && nx < 1 && ny > -1 && ny < 1) in++;
            for (int k = 0; k < 4; k++) { if (p[k] < mx[k]) mx[k] = p[k]; if (p[k] > Mx[k]) Mx[k] = p[k]; }
        }
        extern long g_gsp_frames;
        fprintf(stderr, "[sumv] quadro %ld: tex %08x %u vértices, NaN %d, w<=0 %d, na tela %d | x %g..%g y %g..%g z %g..%g w %g..%g\n", g_gsp_frames, p2v(R(0x85) * 8), count, nan, wneg, in,
                mx[0], Mx[0], mx[1], Mx[1], mx[2], Mx[2], mx[3], Mx[3]);
      } }
    { static int dumped; const char *dv = ENVC("PICA_DUMPV");   /* PICA_DUMPV=n: posições dos vértices do 1º desenho com n vértices */
      extern long g_gsp_frames;
      if (dv && !dumped && count == (uint32_t)atoi(dv) && g_gsp_frames >= (ENVC("PICA_DUMPV_FRAME") ? atol(ENVC("PICA_DUMPV_FRAME")) : 0)) { dumped = 1;
        fprintf(stderr, "[dumpv] viewport hx %g hy %g off %08x clip %08x %08x %08x %08x\n", f24_to_f32(R(0x41)), f24_to_f32(R(0x43)), R(0x68), R(0x47), R(0x48), R(0x49), R(0x4A));
        for (uint32_t i = 0; i < count && i < 60; i++)
            fprintf(stderr, "[dumpv] %u idx %u pos %g %g %g %g cor %g %g %g %g\n", i, indexed ? (ib16 ? MEM16(ib + 2 * i) : MEM8(ib + i)) : first + i,
                    vs[i].pos[0], vs[i].pos[1], vs[i].pos[2], vs[i].pos[3], vs[i].color[0], vs[i].color[1], vs[i].color[2], vs[i].color[3]);
        fprintf(stderr, "[dumpv] cull %u indexed %d ib16 %d\n", R(0x40) & 3, indexed, ib16);
        static const uint32_t tb[6] = { 0xC0, 0xC8, 0xD0, 0xD8, 0xF0, 0xF8 };
        for (int st = 0; st < 6; st++) fprintf(stderr, "[dumpv] tev%d %08x %08x %08x %08x %08x\n", st, R(tb[st]), R(tb[st] + 1), R(tb[st] + 2), R(tb[st] + 3), R(tb[st] + 4));
        fprintf(stderr, "[dumpv] buf %08x upd %08x tex %08x luz 1c3=%08x 1c4=%08x 1d0=%08x 1d1=%08x 1d2=%08x 1c2=%08x 8f=%08x 1c6=%08x\n", R(0xFD), R(0xE0), R(0x80),
                R(0x1C3), R(0x1C4), R(0x1D0), R(0x1D1), R(0x1D2), R(0x1C2), R(0x8F), R(0x1C6)); } }
    uint64_t t_v1 = prof_now(); g_prof_ns[0] += t_v1 - t_v0;
    vp_load();
    /* vértices por ponteiro: nos desenhos indexados, direto do cache por índice (sem copiar para vs[]) */
    g_irec_interp = 1;
    #define VP(i) (vpi ? &g_vc[indexed_idx(i)] : &vs[i])
    int vpi = pre && indexed && !g_vs_trace;
    #define indexed_idx(i) (ib16 ? MEM16(ib + 2 * (i)) : MEM8(ib + (i)))
    if (topo == 0 || topo == 3) for (uint32_t i = 0; i + 2 < count; i += 3) clip_draw(&f, VP(i), VP(i + 1), VP(i + 2));
    else if (topo == 1) for (uint32_t i = 0; i + 2 < count; i++) {
        if (i & 1) clip_draw(&f, VP(i + 1), VP(i), VP(i + 2)); else clip_draw(&f, VP(i), VP(i + 1), VP(i + 2));
    }
    else for (uint32_t i = 1; i + 1 < count; i++) clip_draw(&f, VP(0), VP(i), VP(i + 1));
    #undef VP
    #undef indexed_idx
    { uint64_t t2 = prof_now(); g_prof_ns[1] += t2 - t_v1; tq_flush(&f); g_prof_ns[2] += prof_now() - t2; }
    g_irec_interp = 0;
    if (drawlog_n && drawlog_n <= DLMAX && drawlog_active()) {
        long w = g_fs[5] ? g_fs[5] : 1;
        fprintf(stderr, "[draw]   fragmentos %ld: scissor %ld, estêncil %ld, profundidade %ld, alfa %ld, escritos %ld (cor média %ld,%ld,%ld) luz=%d v0=(%.2f %.2f %.2f %.2f) q=(%.2f %.2f %.2f %.2f) depth %08x fmt %d\n",
                g_fs[0], g_fs[1], g_fs[2], g_fs[3], g_fs[4], g_fs[5], g_fsum[0] / w, g_fsum[1] / w, g_fsum[2] / w, g_light_on,
                count ? vs[0].pos[0] : 0, count ? vs[0].pos[1] : 0, count ? vs[0].pos[2] : 0, count ? vs[0].pos[3] : 0,
                count ? vs[0].quat[0] : 0, count ? vs[0].quat[1] : 0, count ? vs[0].quat[2] : 0, count ? vs[0].quat[3] : 0, f.depth, f.dfmt);
    }
    memset(g_fs, 0, sizeof g_fs); memset(g_fsum, 0, sizeof g_fsum);
    free(vs);
}

/* ------------------------------------------------------------ modo imediato
 * Com o índice de atributo fixo 0xF, cada grupo de 3 palavras é um atributo do próximo vértice;
 * quando chegam todos os atributos (registrador 0x242), o vértice é processado e entra na primitiva. */
static float g_imm_attr[12][4]; static int g_imm_n;
static Vertex g_imm_v[3]; static int g_imm_count, g_imm_strip_odd;
static void immediate_reset(void) { g_imm_count = 0; g_imm_strip_odd = 0; g_imm_n = 0; }
static void immediate_attr(const uint32_t *b) {
    int nattr = (int)(R(0x242) & 0xF) + 1;
    decode_f24x4(b, g_imm_attr[g_imm_n]);
    if (++g_imm_n < nattr) return;
    g_imm_n = 0;
    Vertex v; shade_vertex(g_imm_attr, nattr, &v);
    Fb f = fb_get(); if (!f.color || !f.w) return;
    if (!pica_gl_active()) { tev_setup(); tex_setup(); } light_setup();
    vp_load();
    uint32_t topo = (R(0x25E) >> 8) & 3;
    if (topo == 0 || topo == 3) {
        g_imm_v[g_imm_count++] = v;
        if (g_imm_count == 3) { clip_draw(&f, &g_imm_v[0], &g_imm_v[1], &g_imm_v[2]); g_imm_count = 0; }
    } else if (topo == 1) {
        if (g_imm_count < 2) { g_imm_v[g_imm_count++] = v; return; }
        if (g_imm_strip_odd) clip_draw(&f, &g_imm_v[1], &g_imm_v[0], &v); else clip_draw(&f, &g_imm_v[0], &g_imm_v[1], &v);
        g_imm_v[0] = g_imm_v[1]; g_imm_v[1] = v; g_imm_strip_odd ^= 1;
    } else {
        if (g_imm_count < 2) { g_imm_v[g_imm_count++] = v; return; }
        clip_draw(&f, &g_imm_v[0], &g_imm_v[1], &v); g_imm_v[1] = v;
    }
    tq_flush(&f);
}

/* ------------------------------------------------------------ 60 fps: quadros intermediários
 * O jogo desenha a 30 quadros por segundo e a lógica dele avança um passo por quadro, então não dá para
 * simplesmente pedir 60. Em vez disso, cada quadro desenhado é gravado (os triângulos antes do recorte, com
 * todos os registradores de cada desenho) e, quando a tela de cima do quadro N+1 fica pronta, o renderer
 * OpenGL desenha de novo o quadro N+1 com cada vértice no meio do caminho entre a posição dele no quadro N
 * e no N+1 (pica_gl.c, em cópias das superfícies, sem tocar na memória do jogo). A janela mostra então o
 * quadro intermediário e, um VBlank depois, o N+1. Os desenhos de um quadro e do outro são casados pela
 * ordem dentro de cada "chave" (destino, textura, nº de triângulos, programa); o que não casa (partículas,
 * texto que mudou, objetos que surgiram) vai na posição do quadro N+1. Troca de câmera ou cena (movimento
 * grande demais, poucos desenhos casados): sem quadro intermediário, a imagem só repete. */
typedef struct { uint8_t kind, interp, lerp_tc, skip, cam; int16_t lut, fog; Fb f; uint32_t regs, tri, ntri, key, p[8]; int match; } IOp;
enum { IOP_DRAW, IOP_FILL, IOP_XFER };
typedef struct {
    IOp *ops; int nops, capops;
    Vertex *v; size_t nv, capv;                 /* triângulos (3 vértices cada) */
    uint32_t *regs; size_t nregs, capregs;      /* cópias dos registradores (0x300 palavras por desenho) */
    float *luts; int nluts, capluts;            /* tabelas de iluminação (24 x 256 x 2) que mudaram no quadro */
    uint32_t *fogs; int nfogs, capfogs;         /* tabelas de neblina (128) */
    int over;                                   /* grande demais (o quadro não terminou numa tela de cima): descartado */
} IFrame;
#define IMAX_V (3u << 20)                       /* limite de vértices gravados por quadro */
#define LUTN (24 * 256 * 2)
static IFrame g_ifr[2]; static int g_icur;
static size_t g_irec_start;                     /* primeiro vértice do desenho em gravação */
static unsigned g_irec_lgen = ~0u, g_irec_fgen = ~0u;
extern unsigned g_light_lut_gen, g_fog_lut_gen;
#define GROW(ptr, cap, need, unit) do { if ((need) > (cap)) { size_t nc = (cap) ? (cap) : 64; while (nc < (size_t)(need)) nc *= 2; \
                                         (ptr) = realloc((ptr), nc * (unit)); (cap) = nc; } } while (0)

static void irec_tri(const Vertex *a, const Vertex *b, const Vertex *c) {
    IFrame *F = &g_ifr[g_icur];
    if (F->over || F->nv >= IMAX_V || F->nops > 60000) { F->over = 1; return; }
    GROW(F->v, F->capv, F->nv + 3, sizeof(Vertex));
    F->v[F->nv++] = *a; F->v[F->nv++] = *b; F->v[F->nv++] = *c;
}
static IOp *iop_new(IFrame *F) {
    if (F->nops == F->capops) { F->capops = F->capops ? F->capops * 2 : 256; F->ops = realloc(F->ops, sizeof(IOp) * F->capops); }
    IOp *o = &F->ops[F->nops++]; memset(o, 0, sizeof *o); o->match = -1; o->lut = o->fog = -1;
    return o;
}
static uint32_t ihash(uint32_t h, uint32_t v) { h ^= v; h *= 0x01000193u; return h ^ (h >> 15); }
static void irec_flush(const Fb *f) {
    IFrame *F = &g_ifr[g_icur];
    if (F->over) { g_irec_start = F->nv; return; }
    if (F->nv == g_irec_start) return;
    IOp *o = iop_new(F);
    o->kind = IOP_DRAW; o->interp = (uint8_t)g_irec_interp; o->f = *f;
    o->tri = (uint32_t)(g_irec_start / 3); o->ntri = (uint32_t)((F->nv - g_irec_start) / 3);
    g_irec_start = F->nv;
    GROW(F->regs, F->capregs, F->nregs + 0x300, sizeof(uint32_t));
    memcpy(F->regs + F->nregs, g_regs, 0x300 * sizeof(uint32_t)); o->regs = (uint32_t)F->nregs; F->nregs += 0x300;
    if (g_irec_lgen != g_light_lut_gen || !F->nluts) {           /* tabelas de iluminação mudaram: guarda uma cópia */
        const float *L = light_lut_data();
        if (!F->nluts || memcmp(F->luts + (size_t)(F->nluts - 1) * LUTN, L, LUTN * sizeof(float))) {
            GROW(F->luts, F->capluts, (size_t)(F->nluts + 1) * LUTN, sizeof(float));   /* (capacidade em floats) */
            memcpy(F->luts + (size_t)F->nluts * LUTN, L, LUTN * sizeof(float)); F->nluts++;
        }
        g_irec_lgen = g_light_lut_gen;
    }
    o->lut = (int16_t)(F->nluts - 1);
    if (g_irec_fgen != g_fog_lut_gen || !F->nfogs) {
        if (!F->nfogs || memcmp(F->fogs + (size_t)(F->nfogs - 1) * 128, g_fog_lut, sizeof g_fog_lut)) {
            GROW(F->fogs, F->capfogs, (size_t)(F->nfogs + 1) * 128, sizeof(uint32_t));
            memcpy(F->fogs + (size_t)F->nfogs * 128, g_fog_lut, sizeof g_fog_lut); F->nfogs++;
        }
        g_irec_fgen = g_fog_lut_gen;
    }
    o->fog = (int16_t)(F->nfogs - 1);
    uint32_t k = ihash(0x811C9DC5u, f->color); k = ihash(k, f->depth); k = ihash(k, R(0x85)); k = ihash(k, R(0x80));
    k = ihash(k, o->ntri); k = ihash(k, o->interp);
    o->key = k;
}
/* preenchimento / transferência feitos na GPU durante o quadro (pica_gl.c) */
void interp_rec_op(int kind, const uint32_t p[8]) {
    if (!g_irec) return;
    IFrame *F = &g_ifr[g_icur];
    if (F->over || F->nops > 60000) { F->over = 1; return; }
    IOp *o = iop_new(F); o->kind = (uint8_t)kind; memcpy(o->p, p, sizeof o->p);
}
/* começa um quadro novo: o atual vira o anterior */
void interp_frame_swap(void) {
    g_icur ^= 1;
    IFrame *F = &g_ifr[g_icur];
    F->nops = 0; F->nv = 0; F->nregs = 0; F->nluts = 0; F->nfogs = 0; F->over = 0;
    g_irec_start = 0; g_irec_lgen = g_irec_fgen = ~0u;
}
void interp_reset(void) { interp_frame_swap(); interp_frame_swap(); }

/* casa os desenhos do quadro atual com os do anterior; devolve 0 se não vale interpolar */
/* movimento da câmera entre os dois quadros: para o cenário parado, posição de recorte anterior = T · atual
 * (T = PV_anterior · PV_atual⁻¹, linear em 4D). Estimado por mínimos quadrados com os vértices dos desenhos
 * casados (descartando os que se movem por conta própria) e usado nos desenhos que não casaram: sem isso,
 * um pedaço do cenário ia na posição nova e o vizinho na do meio, e apareciam frestas e contornos. */
static double g_T[4][4]; static int g_T_ok;
static int persp(const Vertex *v, size_t n) {                  /* desenho em perspectiva (o 2D da interface tem w = 1) */
    for (size_t k = 0; k < n; k++) if (fabsf(v[k].pos[3] - 1.0f) > 1e-3f) return 1;
    return 0;
}
static int solve4(double A[4][4], double b[4], double x[4]) {
    double M[4][5];
    for (int i = 0; i < 4; i++) { for (int j = 0; j < 4; j++) M[i][j] = A[i][j]; M[i][4] = b[i]; }
    for (int c = 0; c < 4; c++) {
        int p = c; for (int r = c + 1; r < 4; r++) if (fabs(M[r][c]) > fabs(M[p][c])) p = r;
        if (fabs(M[p][c]) < 1e-12) return 0;
        if (p != c) for (int k = 0; k < 5; k++) { double t = M[c][k]; M[c][k] = M[p][k]; M[p][k] = t; }
        for (int r = 0; r < 4; r++) if (r != c) { double f = M[r][c] / M[c][c]; for (int k = c; k < 5; k++) M[r][k] -= f * M[c][k]; }
    }
    for (int i = 0; i < 4; i++) x[i] = M[i][4] / M[i][i];
    return 1;
}
static void tmul(const double T[4][4], const float c[4], float o[4]) {
    for (int r = 0; r < 4; r++) o[r] = (float)(T[r][0] * c[0] + T[r][1] * c[1] + T[r][2] * c[2] + T[r][3] * c[3]);
}
static float ndc_err(const float a[4], const float b[4]) {
    if (a[3] <= 1e-4f || b[3] <= 1e-4f) return 1e9f;
    return fabsf(a[0] / a[3] - b[0] / b[3]) + fabsf(a[1] / a[3] - b[1] / b[3]);
}
enum { NS = 6000 };
static float g_es_c[NS][4], g_es_p[NS][4]; static uint8_t g_es_use[NS]; static int g_es_n;   /* amostras da estimativa */
static void estimate_camera(IFrame *C, IFrame *P) {
    float (*sc)[4] = g_es_c, (*sp)[4] = g_es_p; uint8_t *use = g_es_use;
    int ns = 0; g_es_n = 0;
    long nd = 0; for (int i = 0; i < C->nops; i++) if (C->ops[i].kind == IOP_DRAW && C->ops[i].match >= 0) nd++;
    int per = nd ? (int)(NS / nd) : 0; if (per < 4) per = 4; if (per > 64) per = 64;
    for (int i = 0; i < C->nops && ns < NS; i++) {
        IOp *o = &C->ops[i];
        if (o->kind != IOP_DRAW || o->match < 0) continue;
        const Vertex *a = C->v + (size_t)o->tri * 3, *q = P->v + (size_t)P->ops[o->match].tri * 3;
        size_t n = (size_t)o->ntri * 3; if (!persp(a, n)) continue;
        size_t step = n > (size_t)per ? n / per : 1;
        for (size_t k = 0; k < n && ns < NS; k += step) {
            if (a[k].pos[3] <= 1e-4f || q[k].pos[3] <= 1e-4f) continue;
            memcpy(sc[ns], a[k].pos, 16); memcpy(sp[ns], q[k].pos, 16); use[ns] = 1; ns++;
        }
    }
    g_T_ok = 0;
    if (ns < 32) return;
    for (int it = 0; it < 3; it++) {
        double A[4][4] = { { 0 } }, B[4][4] = { { 0 } };
        int nu = 0;
        for (int k = 0; k < ns; k++) if (use[k]) {
            nu++;
            for (int i = 0; i < 4; i++) { for (int j = 0; j < 4; j++) A[i][j] += (double)sc[k][i] * sc[k][j];
                                          for (int r = 0; r < 4; r++) B[r][i] += (double)sc[k][i] * sp[k][r]; }
        }
        if (nu < 32) return;
        for (int r = 0; r < 4; r++) if (!solve4(A, B[r], g_T[r])) return;
        /* resíduos (na tela): fica com quem segue a câmera */
        static float err[NS]; float tmp[NS]; int ne = 0;
        for (int k = 0; k < ns; k++) { float t[4]; tmul(g_T, sc[k], t); err[k] = ndc_err(t, sp[k]); if (use[k]) tmp[ne++] = err[k]; }
        float med = 0;
        { static float m2[512]; int nm = 0, st = ne > 512 ? ne / 512 + 1 : 1;
          for (int a = 0; a < ne && nm < 512; a += st) m2[nm++] = tmp[a];
          for (int a = 1; a < nm; a++) { float v = m2[a]; int b = a - 1; while (b >= 0 && m2[b] > v) { m2[b + 1] = m2[b]; b--; } m2[b + 1] = v; }
          med = nm ? m2[nm / 2] : 0; }
        float lim = med * 3 + 0.002f;
        for (int k = 0; k < ns; k++) use[k] = err[k] < lim;
        if (it == 2) { int good = 0; for (int k = 0; k < ns; k++) good += use[k]; g_T_ok = good * 2 >= ns && med < 0.01f; }
    }
    g_es_n = ns;
}
/* movimento da câmera estimado só com os vértices casados perto (na tela) do desenho: a estimativa global erra
 * até ~1 pixel original em partes da tela (o cenário não segue uma única transformação), e perto do desenho
 * ela fica igual ao movimento real dos vizinhos, que vão com a média exata dos dois quadros */
static int local_camera(const Vertex *a, size_t n, double T[4][4]) {
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (size_t k = 0; k < n; k++) { const float *p = a[k].pos; if (p[3] <= 1e-4f) continue;
        float x = p[0] / p[3], y = p[1] / p[3]; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    if (x0 > x1) return 0;
    for (float r = 0.15f; r <= 0.61f; r *= 2) {
        double A[4][4] = { { 0 } }, B[4][4] = { { 0 } }; int nu = 0;
        for (int k = 0; k < g_es_n; k++) {
            if (!g_es_use[k]) continue;
            const float *c = g_es_c[k]; float x = c[0] / c[3], y = c[1] / c[3];
            if (x < x0 - r || x > x1 + r || y < y0 - r || y > y1 + r) continue;
            nu++;
            for (int i = 0; i < 4; i++) { for (int j = 0; j < 4; j++) A[i][j] += (double)c[i] * c[j];
                                          for (int q = 0; q < 4; q++) B[q][i] += (double)c[i] * g_es_p[k][q]; }
        }
        if (nu < 24) continue;
        double R[4][4]; int ok = 1;
        for (int q = 0; q < 4 && ok; q++) ok = solve4(A, B[q], R[q]);
        if (ok) { memcpy(T, R, sizeof R); return 1; }
    }
    return 0;
}

static int interp_match(float *mov_out) {
    IFrame *C = &g_ifr[g_icur], *P = &g_ifr[g_icur ^ 1];
    if (!P->nops || !C->nops || P->over || C->over) return 0;
    /* o que foi desenhado na superfície da tela de cima antes da última limpeza dela (a tela de baixo usa a
     * mesma superfície antes) não aparece no quadro: não precisa ser refeito */
    { IOp *x = &C->ops[C->nops - 1]; int L = -1;
      for (int i = 0; i < C->nops; i++) C->ops[i].skip = 0;
      if (x->kind == IOP_XFER)
          for (int i = C->nops - 2; i >= 0 && L < 0; i--) { IOp *o = &C->ops[i]; if (o->kind == IOP_FILL && o->p[0] <= x->p[0] && x->p[0] < o->p[1]) L = i; }
      if (L >= 0) for (int i = 0; i < L; i++) { IOp *o = &C->ops[i];
          if ((o->kind == IOP_DRAW && o->f.color == C->ops[L].p[0]) || (o->kind == IOP_FILL && o->p[0] == C->ops[L].p[0])) o->skip = 1; } }
    enum { HB = 4096 };
    static int head[HB], *nxt; static int capn;
    if (capn < P->nops) { capn = P->nops * 2; nxt = realloc(nxt, sizeof(int) * capn); }
    for (int i = 0; i < HB; i++) head[i] = -1;
    for (int i = P->nops - 1; i >= 0; i--) {                    /* listas por chave, em ordem */
        IOp *o = &P->ops[i];
        if (o->kind != IOP_DRAW || !o->interp) { nxt[i] = -2; continue; }
        int b = o->key & (HB - 1); nxt[i] = head[b]; head[b] = i;
    }
    long tris = 0, mtris = 0; double mov = 0; long nmov = 0;
    for (int i = 0; i < C->nops; i++) {
        IOp *o = &C->ops[i]; o->match = -1;
        if (o->kind != IOP_DRAW || !o->interp || o->skip) continue;
        tris += o->ntri;
        /* entre os desenhos do quadro anterior com a mesma chave, o mais parecido na tela (os primeiros da
         * fila, em ordem): partes iguais de um modelo (mãos, árvores repetidas) não trocam de par */
        int b = o->key & (HB - 1), prev = -1, j = -1, bp = -1, pp = -1, c = head[b], tried = 0; float bd = 1e30f;
        const Vertex *a0 = C->v + (size_t)o->tri * 3; size_t n0 = (size_t)o->ntri * 3, st0 = n0 > 8 ? n0 / 8 : 1;
        while (c >= 0 && tried < 48) {
            if (P->ops[c].key == o->key) {
                const Vertex *q0 = P->v + (size_t)P->ops[c].tri * 3; float dd = 0;
                for (size_t k = 0; k < n0; k += st0) { float e = ndc_err(a0[k].pos, q0[k].pos); dd += e < 1e8f ? e : 4.0f; }
                if (dd < bd) { bd = dd; j = c; bp = pp; }
                tried++;
                if (dd < 1e-6f) break;                                  /* idêntico: não precisa procurar mais */
            }
            pp = c; c = nxt[c];
        }
        prev = bp;
        if (j < 0) continue;
        if (prev < 0) head[b] = nxt[j]; else nxt[prev] = nxt[j];  /* tira da lista (cada um casa uma vez) */
        /* movimento médio deste desenho na tela; grande demais = provavelmente outro objeto: não interpola */
        const Vertex *a = C->v + (size_t)o->tri * 3, *q = P->v + (size_t)P->ops[j].tri * 3;
        size_t n = (size_t)o->ntri * 3, step = n > 64 ? n / 64 : 1; double d = 0; long nd = 0;
        for (size_t k = 0; k < n; k += step) {
            if (a[k].pos[3] <= 1e-4f || q[k].pos[3] <= 1e-4f) continue;
            d += fabsf(a[k].pos[0] / a[k].pos[3] - q[k].pos[0] / q[k].pos[3]) + fabsf(a[k].pos[1] / a[k].pos[3] - q[k].pos[1] / q[k].pos[3]); nd++;
        }
        if (nd && d / nd > 0.35 && persp(a, n)) { o->match = -2; continue; }   /* (2D: transições de tela se movem muito e devem misturar) */
        if (nd && !persp(a, n)) {                               /* 2D: só deslocado inteiro para longe = outro elemento */
            double mx = 0, my = 0, sp = 0; long m = 0;           /* (ex.: a barra de HP do jogador casada com a do inimigo) */
            for (size_t k = 0; k < n; k += step) { mx += a[k].pos[0] - q[k].pos[0]; my += a[k].pos[1] - q[k].pos[1]; m++; }
            mx /= m; my /= m;
            for (size_t k = 0; k < n; k += step) sp += fabs(a[k].pos[0] - q[k].pos[0] - mx) + fabs(a[k].pos[1] - q[k].pos[1] - my);
            sp /= m;
            double mv = fabs(mx) + fabs(my);
            if (mv > 0.2 && sp < mv * 0.25) { o->match = -5; continue; }
        }
        o->match = j; mtris += o->ntri;
        { float md = 0;                                          /* textura correndo devagar (água): mistura também */
          for (size_t k = 0; k < n; k += step) { float e = fabsf(a[k].tc0[0] - q[k].tc0[0]) + fabsf(a[k].tc0[1] - q[k].tc0[1]); if (e > md) md = e; }
          o->lerp_tc = md > 0 && md < 0.06f; }
        if (nd) { mov += d / nd * o->ntri; nmov += o->ntri; }
    }
    estimate_camera(C, P);
    if (g_T_ok)                                                 /* par trocado (outro objeto com a mesma chave): segue a câmera */
        for (int i = 0; i < C->nops; i++) {
            IOp *o = &C->ops[i];
            o->cam = 0;
            if (o->kind != IOP_DRAW || o->match < 0) continue;
            const Vertex *a = C->v + (size_t)o->tri * 3, *q = P->v + (size_t)P->ops[o->match].tri * 3;
            size_t n = (size_t)o->ntri * 3, step = n > 32 ? n / 32 : 1; double e = 0, es = 0; long ne = 0;
            if (!persp(a, n)) continue;
            for (size_t k = 0; k < n; k += step) { float t[4]; tmul(g_T, a[k].pos, t); float d = ndc_err(t, q[k].pos), d2 = ndc_err(a[k].pos, q[k].pos);
                                                   if (d < 1e8f && d2 < 1e8f) { e += d; es += d2; ne++; } }
            /* vértices que andam mais que o próprio tamanho do desenho entre dois quadros: par errado (outro
             * efeito com a mesma chave, como a grama mexida ao correr); a média dele virava um borrão */
            float bx0 = 1e9f, bx1 = -1e9f, by0 = 1e9f, by1 = -1e9f, dmax = 0;
            for (size_t k = 0; k < n; k += step) { const float *p = a[k].pos; if (p[3] <= 1e-4f) continue;
                float x = p[0] / p[3], y = p[1] / p[3]; if (x < bx0) bx0 = x; if (x > bx1) bx1 = x; if (y < by0) by0 = y; if (y > by1) by1 = y;
                float d = ndc_err(p, q[k].pos); if (d < 1e8f && d > dmax) dmax = d; }
            if (bx0 <= bx1 && dmax > 0.06f && dmax > 0.6f * ((bx1 - bx0) + (by1 - by0))) { o->match = -4; mtris -= o->ntri; continue; }
            /* nem segue a câmera nem ficou parado na tela (o jogador acompanha a câmera): par errado */
            if (ne && e / ne > 0.15 && es / ne > 0.15) { o->match = -4; mtris -= o->ntri; }
            /* cenário parado: antes ele também seguia o movimento estimado da câmera, mas a estimativa erra até
             * ~1 pixel original em partes da tela, e pedaços vizinhos (um com a média exata dos dois quadros,
             * outro com a estimativa) se separavam nos quadros intermediários: em 4x apareciam linhas entre
             * eles. Desenho com par usa sempre a média exata; só os sem par seguem a câmera */
        }
    { static int lg = -1; if (lg < 0) lg = ENVC("INTERP_LOG") != NULL;              /* depuração */
      static long nf; if (lg && nf++ % 4 == 0) {
          int nd = 0, nm = 0; for (int i = 0; i < C->nops; i++) if (C->ops[i].kind == IOP_DRAW) { nd++; if (C->ops[i].match >= 0) nm++; }
          fprintf(stderr, "[interp] desenhos %d (anterior %d), casados %d, triângulos %ld casados %ld, câmera %s\n", nd, P->nops, nm, tris, mtris, g_T_ok ? "ok" : "não");
          for (int i = 0; i < C->nops; i++) { IOp *o = &C->ops[i]; if (o->kind == IOP_DRAW && o->match < 0)
              fprintf(stderr, "[interp]   sem par (%d): %d interp=%d ntri=%u fb %08x tex %08x key %08x\n", o->match, i, o->interp, o->ntri, o->f.color, C->regs[o->regs + 0x85] * 8, o->key); } } }
    *mov_out = nmov ? (float)(mov / nmov) : 0;
    if (!tris) return 0;
    if (mtris * 2 < tris && !g_T_ok) return 0;                  /* cena nova (a câmera também não se explica) */
    return 1;
}

/* desenha de novo o quadro atual com os vértices na fração t entre o anterior e o atual
 * (chamada pelo pica_gl.c com as superfícies trocadas por cópias e o contexto do OpenGL ativo) */
extern const float *g_light_lut_override; extern const uint32_t *g_fog_lut_override;
extern int g_light_lut_dirty;
long g_interp_stats[4];                                         /* feitos, sem quadro anterior, cena nova */
int interp_replay(float t, int (*op_cb)(int kind, const uint32_t p[8])) {
    float mov = 0;
    if (!interp_match(&mov)) { g_interp_stats[g_ifr[g_icur ^ 1].nops ? 2 : 1]++; return 0; }
    IFrame *C = &g_ifr[g_icur], *P = &g_ifr[g_icur ^ 1];
    static uint32_t saved[0x300]; memcpy(saved, g_regs, sizeof saved);
    int was = g_irec; g_irec = 0;
    int cur_lut = -2, cur_fog = -2;
    { static int lg = -1; if (lg < 0) lg = ENVC("INTERP_OPS") != NULL;           /* depuração: operações refeitas */
      static long nr; nr++;
      if (lg) { int nd = 0; for (int i = 0; i < C->nops; i++) if (C->ops[i].kind == IOP_DRAW && !C->ops[i].skip) nd++;
                if (nd < 40) for (int i = 0; i < C->nops; i++) { IOp *o = &C->ops[i]; const uint32_t *rg = C->regs + o->regs;
                    if (o->kind == IOP_DRAW) fprintf(stderr, "[ops %ld] %d desenho%s fb %08x n=%u tex0 %08x %ux%u fmt %u interp %d par %d\n", nr, i, o->skip ? " (pulado)" : "", o->f.color, o->ntri,
                                                     (rg[0x80] & 1) ? rg[0x85] * 8 : 0, (rg[0x82] >> 16) & 0x7FF, rg[0x82] & 0x7FF, rg[0x8E] & 0xF, o->interp, o->match);
                    else fprintf(stderr, "[ops %ld] %d op %d%s %08x %08x %08x\n", nr, i, o->kind, o->skip ? " (pulado)" : "", o->p[0], o->p[1], o->p[2]); } } }
    for (int i = 0; i < C->nops; i++) {
        IOp *o = &C->ops[i];
        if (o->skip) continue;
        if (o->kind != IOP_DRAW) { op_cb(o->kind, o->p); continue; }
        memcpy(g_regs, C->regs + o->regs, 0x300 * sizeof(uint32_t));
        if (o->lut != cur_lut) { cur_lut = o->lut; g_light_lut_override = o->lut >= 0 ? C->luts + (size_t)o->lut * LUTN : NULL; g_light_lut_dirty = 1; }
        if (o->fog != cur_fog) { cur_fog = o->fog; g_fog_lut_override = o->fog >= 0 ? C->fogs + (size_t)o->fog * 128 : NULL; g_fog_lut_dirty = 1; }
        light_setup(); vp_load();
        const Vertex *a = C->v + (size_t)o->tri * 3;
        const Vertex *q = o->match >= 0 ? P->v + (size_t)P->ops[o->match].tri * 3 : NULL;
        int cam = (!q || o->cam) && o->interp && g_T_ok && persp(a, (size_t)o->ntri * 3);   /* sem par: segue a câmera */
        double TL[4][4]; memcpy(TL, g_T, sizeof TL);
        if (cam) local_camera(a, (size_t)o->ntri * 3, TL);
        Fb f = o->f;
        for (uint32_t k = 0; k < o->ntri; k++) {
            Vertex v[3];
            for (int m = 0; m < 3; m++) {
                v[m] = a[k * 3 + m];
                if (q) {
                    const Vertex *pv = &q[k * 3 + m];
                    const float *pp = pv->pos;
                    if (!cam && pp[3] > 1e-4f && v[m].pos[3] > 1e-4f)
                        for (int c = 0; c < 4; c++) v[m].pos[c] = pp[c] + (v[m].pos[c] - pp[c]) * t;
                    for (int c = 0; c < 4; c++) v[m].color[c] = pv->color[c] + (v[m].color[c] - pv->color[c]) * t;
                    if (o->lerp_tc)
                        for (int c = 0; c < 2; c++) { v[m].tc0[c] = pv->tc0[c] + (v[m].tc0[c] - pv->tc0[c]) * t;
                                                     v[m].tc1[c] = pv->tc1[c] + (v[m].tc1[c] - pv->tc1[c]) * t;
                                                     v[m].tc2[c] = pv->tc2[c] + (v[m].tc2[c] - pv->tc2[c]) * t; }
                }
                if (cam && v[m].pos[3] > 1e-4f) {
                    float pp[4]; tmul(TL, v[m].pos, pp);
                    if (pp[3] > 1e-4f) for (int c = 0; c < 4; c++) v[m].pos[c] = pp[c] + (v[m].pos[c] - pp[c]) * t;
                }
            }
            clip_draw(&f, &v[0], &v[1], &v[2]);
        }
        tq_flush(&f);
    }
    memcpy(g_regs, saved, sizeof saved);
    g_light_lut_override = NULL; g_fog_lut_override = NULL; g_light_lut_dirty = 1; g_fog_lut_dirty = 1;
    light_setup(); vp_load();
    g_irec = was;
    g_interp_stats[0]++;
    (void)mov;
    return 1;
}

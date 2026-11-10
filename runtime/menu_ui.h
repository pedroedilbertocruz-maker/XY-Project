/* Desenho do menu em software (no jogo e no lançador): tela ARGB pré-multiplicada, retângulos, degradês,
 * texto com as fontes de fonte_menu.h (Inter) em qualquer tamanho e o cartucho 3D girando (rasterizado aqui,
 * com textura em perspectiva, 2x2 amostras por pixel). Sem dependências além da libc: o lançador (Win32) e o
 * jogo (SDL) incluem este arquivo. Tudo é static; quem inclui usa só o que precisa. */
#ifndef MENU_UI_H
#define MENU_UI_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "fonte_menu.h"

#if defined(__GNUC__)
#define MU_UNUSED __attribute__((unused))
#else
#define MU_UNUSED
#endif

typedef struct { uint32_t *px; int w, h; } MuCanvas;          /* ARGB pré-multiplicado */
typedef struct { uint32_t *px; int w, h; } MuImg;             /* idem */
typedef struct { uint32_t base, base2, accent, accent2, text, dim; } MuTema;

/* cores das versões: X azul, Y vermelho; sem versão, dourado neutro */
static MU_UNUSED MuTema mu_tema(char versao) {
    MuTema t;
    if (versao == 'X')      { t.base = 0xFF050C18; t.base2 = 0xFF0B2448; t.accent = 0xFF3FA9F5; t.accent2 = 0xFF1C5FB8; }
    else if (versao == 'Y') { t.base = 0xFF14060A; t.base2 = 0xFF44101C; t.accent = 0xFFF0485A; t.accent2 = 0xFFA81C30; }
    else                    { t.base = 0xFF0C0C0E; t.base2 = 0xFF26221A; t.accent = 0xFFE8B54A; t.accent2 = 0xFFA87A22; }
    t.text = 0xFFF4F6FA; t.dim = 0xFFA8B0BC;
    return t;
}
static MU_UNUSED uint32_t mu_rgba(uint32_t rgb, int a) {        /* cor 0xRRGGBB (ou ARGB, alfa ignorado) + alfa -> pré-multiplicado */
    if (a <= 0) return 0; if (a > 255) a = 255;
    uint32_t r = (rgb >> 16 & 255) * a / 255, g = (rgb >> 8 & 255) * a / 255, b = (rgb & 255) * a / 255;
    return (uint32_t)a << 24 | r << 16 | g << 8 | b;
}
static inline uint32_t mu_over(uint32_t d, uint32_t s) {
    uint32_t sa = s >> 24; if (sa == 255) return s; if (!sa) return d;
    uint32_t ia = 255 - sa;
    uint32_t rb = ((d & 0x00FF00FFu) * ia + 0x00800080u); rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
    uint32_t ag = (((d >> 8) & 0x00FF00FFu) * ia + 0x00800080u); ag = ((ag + ((ag >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
    return s + (rb | ag << 8);
}
static inline uint32_t mu_scale(uint32_t c, int a) {           /* multiplica a cor pré-multiplicada por a/255 */
    if (a >= 255) return c; if (a <= 0) return 0;
    uint32_t rb = (c & 0x00FF00FFu) * (uint32_t)a + 0x00800080u; rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
    uint32_t ag = ((c >> 8) & 0x00FF00FFu) * (uint32_t)a + 0x00800080u; ag = ((ag + ((ag >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
    return rb | ag << 8;
}
static inline uint32_t mu_lerp(uint32_t a, uint32_t b, int t) { return mu_scale(a, 255 - t) + mu_scale(b, t); }

static MU_UNUSED void mu_clear(MuCanvas *c, uint32_t v) { for (long i = 0; i < (long)c->w * c->h; i++) c->px[i] = v; }
static MU_UNUSED void mu_fill(MuCanvas *c, int x, int y, int w, int h, uint32_t s) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
    for (int j = y0; j < y1; j++) { uint32_t *r = c->px + (long)j * c->w; for (int i = x0; i < x1; i++) r[i] = mu_over(r[i], s); }
}
/* degradê horizontal (s0 à esquerda, s1 à direita), com suavização opcional (ease) */
static MU_UNUSED void mu_hgrad(MuCanvas *c, int x, int y, int w, int h, uint32_t s0, uint32_t s1, int ease) {
    if (w <= 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
    for (int i = x0; i < x1; i++) {
        float t = (i - x + 0.5f) / w; if (ease) t = t * t * (3 - 2 * t);
        uint32_t s = mu_lerp(s0, s1, (int)(t * 255));
        for (int j = y0; j < y1; j++) c->px[(long)j * c->w + i] = mu_over(c->px[(long)j * c->w + i], s);
    }
}
static MU_UNUSED void mu_vgrad(MuCanvas *c, int x, int y, int w, int h, uint32_t s0, uint32_t s1, int ease) {
    if (h <= 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
    for (int j = y0; j < y1; j++) {
        float t = (j - y + 0.5f) / h; if (ease) t = t * t * (3 - 2 * t);
        uint32_t s = mu_lerp(s0, s1, (int)(t * 255));
        uint32_t *r = c->px + (long)j * c->w;
        for (int i = x0; i < x1; i++) r[i] = mu_over(r[i], s);
    }
}
/* brilho radial (cor no centro, some até o raio) */
static MU_UNUSED void mu_glow(MuCanvas *c, float cx, float cy, float rx, float ry, uint32_t s) {
    int x0 = (int)(cx - rx), x1 = (int)(cx + rx) + 1, y0 = (int)(cy - ry), y1 = (int)(cy + ry) + 1;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > c->w) x1 = c->w; if (y1 > c->h) y1 = c->h;
    for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) {
        float dx = (i + 0.5f - cx) / rx, dy = (j + 0.5f - cy) / ry, d = dx * dx + dy * dy;
        if (d >= 1) continue;
        float t = 1 - d; t = t * t;
        c->px[(long)j * c->w + i] = mu_over(c->px[(long)j * c->w + i], mu_scale(s, (int)(t * 255)));
    }
}
/* retângulo com cantos arredondados, bordas suavizadas */
static MU_UNUSED void mu_rrect(MuCanvas *c, float x, float y, float w, float h, float r, uint32_t s) {
    int x0 = (int)floorf(x), y0 = (int)floorf(y), x1 = (int)ceilf(x + w), y1 = (int)ceilf(y + h);
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > c->w) x1 = c->w; if (y1 > c->h) y1 = c->h;
    if (r > w / 2) r = w / 2; if (r > h / 2) r = h / 2;
    for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) {
        float px = i + 0.5f, py = j + 0.5f;
        float qx = fabsf(px - (x + w / 2)) - (w / 2 - r), qy = fabsf(py - (y + h / 2)) - (h / 2 - r);
        float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
        float dist = sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - r;
        float a = 0.5f - dist; if (a <= 0) continue; if (a > 1) a = 1;
        c->px[(long)j * c->w + i] = mu_over(c->px[(long)j * c->w + i], mu_scale(s, (int)(a * 255)));
    }
}

/* ---------------------------------------------------------------- texto */
static unsigned char *mu_glyph_px;                     /* glifos descomprimidos */
static unsigned *mu_glyph_off[8];
static void mu_font_init(void) {
    if (mu_glyph_px) return;
    size_t total = 0; int nf = (int)(sizeof fm_fontes / sizeof fm_fontes[0]);
    for (int f = 0; f < nf; f++) for (int i = 0; i < fm_fontes[f].n; i++) total += (size_t)fm_fontes[f].g[i].w * fm_fontes[f].g[i].h;
    mu_glyph_px = malloc(total + 1);
    size_t o = 0;
    for (int f = 0; f < nf; f++) {
        mu_glyph_off[f] = malloc(sizeof(unsigned) * fm_fontes[f].n);
        for (int i = 0; i < fm_fontes[f].n; i++) {
            const FmGlifo *g = &fm_fontes[f].g[i];
            mu_glyph_off[f][i] = (unsigned)o;
            const unsigned char *s = fm_px + g->off; size_t n = (size_t)g->w * g->h, k = 0;
            while (k < n) {
                if (*s == 0) { int z = s[1]; s += 2; while (z-- && k < n) mu_glyph_px[o + k++] = 0; }
                else mu_glyph_px[o + k++] = *s++;
            }
            o += n;
        }
    }
}
static unsigned mu_utf8(const char **p) {
    const unsigned char *s = (const unsigned char *)*p; unsigned c = *s++;
    if (c >= 0xC0 && c < 0xE0 && (*s & 0xC0) == 0x80) c = ((c & 0x1F) << 6) | (*s++ & 0x3F);
    else if (c >= 0xE0 && c < 0xF0 && (s[0] & 0xC0) == 0x80 && (s[1] & 0xC0) == 0x80) { c = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F); s += 2; }
    else if (c >= 0x80) c = '?';
    *p = (const char *)s; return c;
}
static int mu_find(int f, unsigned cp) {
    const FmFonte *F = &fm_fontes[f];
    int lo = 0, hi = F->n - 1;                         /* (os glifos estão em ordem de código? não garantido: busca linear) */
    (void)lo; (void)hi;
    for (int i = 0; i < F->n; i++) if (F->g[i].cp == cp) return i;
    if (cp >= 'a' && cp <= 'z') return mu_find(f, cp - 32);
    for (int i = 0; i < F->n; i++) if (F->g[i].cp == '?') return i;
    return 0;
}
/* largura do texto em pixels, no tamanho `size` (altura nominal da fonte), com espaçamento extra `track` */
static MU_UNUSED float mu_text_w(int f, float size, float track, const char *s) {
    float k = size / fm_fontes[f].px, w = 0;
    for (const char *p = s; *p;) { int gi = mu_find(f, mu_utf8(&p)); w += fm_fontes[f].g[gi].adv16 / 16.0f * k + track; }
    return w > 0 ? w - track : 0;
}
/* desenha com a linha de base em y; devolve a largura */
static MU_UNUSED float mu_text(MuCanvas *c, int f, float size, float x, float y, uint32_t color, float track, const char *s) {
    mu_font_init();
    const FmFonte *F = &fm_fontes[f]; float k = size / F->px, x0 = x;
    for (const char *p = s; *p;) {
        int gi = mu_find(f, mu_utf8(&p)); const FmGlifo *g = &F->g[gi];
        if (g->w && g->h) {
            const unsigned char *gp = mu_glyph_px + mu_glyph_off[f][gi];
            float gx = x + g->x0 * k, gy = y + g->y0 * k, gw = g->w * k, gh = g->h * k;
            int ix0 = (int)floorf(gx), iy0 = (int)floorf(gy), ix1 = (int)ceilf(gx + gw), iy1 = (int)ceilf(gy + gh);
            if (ix0 < 0) ix0 = 0; if (iy0 < 0) iy0 = 0; if (ix1 > c->w) ix1 = c->w; if (iy1 > c->h) iy1 = c->h;
            for (int j = iy0; j < iy1; j++) for (int i = ix0; i < ix1; i++) {
                int acc = 0;                           /* 2x2 amostras, cada uma bilinear */
                for (int sy = 0; sy < 2; sy++) for (int sx = 0; sx < 2; sx++) {
                    float u = (i + 0.25f + 0.5f * sx - gx) / k - 0.5f, v = (j + 0.25f + 0.5f * sy - gy) / k - 0.5f;
                    int u0 = (int)floorf(u), v0 = (int)floorf(v); float fu = u - u0, fv = v - v0;
                    int a00 = 0, a10 = 0, a01 = 0, a11 = 0;
                    if (v0 >= 0 && v0 < g->h) { if (u0 >= 0 && u0 < g->w) a00 = gp[v0 * g->w + u0]; if (u0 + 1 >= 0 && u0 + 1 < g->w) a10 = gp[v0 * g->w + u0 + 1]; }
                    if (v0 + 1 >= 0 && v0 + 1 < g->h) { if (u0 >= 0 && u0 < g->w) a01 = gp[(v0 + 1) * g->w + u0]; if (u0 + 1 >= 0 && u0 + 1 < g->w) a11 = gp[(v0 + 1) * g->w + u0 + 1]; }
                    acc += (int)((a00 * (1 - fu) + a10 * fu) * (1 - fv) + (a01 * (1 - fu) + a11 * fu) * fv);
                }
                acc >>= 2;
                if (acc) c->px[(long)j * c->w + i] = mu_over(c->px[(long)j * c->w + i], mu_scale(color, acc));
            }
        }
        x += g->adv16 / 16.0f * k + track;
    }
    return x - x0 - track;
}

/* ---------------------------------------------------------------- imagem (BMP de 32 ou 24 bits) */
static MU_UNUSED int mu_img_bmp(MuImg *im, const char *path) {
    memset(im, 0, sizeof *im);
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    unsigned char h[138]; size_t n = fread(h, 1, sizeof h, f);
    if (n < 54 || h[0] != 'B' || h[1] != 'M') { fclose(f); return 0; }
    uint32_t off = h[10] | h[11] << 8 | h[12] << 16 | (uint32_t)h[13] << 24;
    int32_t w = (int32_t)(h[18] | h[19] << 8 | h[20] << 16 | (uint32_t)h[21] << 24), hh = (int32_t)(h[22] | h[23] << 8 | h[24] << 16 | (uint32_t)h[25] << 24);
    int bpp = h[28] | h[29] << 8; uint32_t comp = h[30] | h[31] << 8;
    int top = hh < 0; if (top) hh = -hh;
    if (w <= 0 || hh <= 0 || w > 8192 || hh > 8192 || (bpp != 32 && bpp != 24) || (comp != 0 && comp != 3)) { fclose(f); return 0; }
    int stride = ((w * bpp / 8) + 3) & ~3;
    unsigned char *row = malloc((size_t)stride); im->px = malloc((size_t)w * hh * 4); im->w = w; im->h = hh;
    fseek(f, (long)off, SEEK_SET);
    for (int y = 0; y < hh; y++) {
        if (fread(row, 1, (size_t)stride, f) != (size_t)stride) break;
        uint32_t *d = im->px + (long)(top ? y : hh - 1 - y) * w;
        for (int x = 0; x < w; x++) {
            unsigned char *p = row + x * (bpp / 8);
            int a = bpp == 32 ? p[3] : 255;
            d[x] = mu_rgba((uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0], a);
        }
    }
    free(row); fclose(f);
    return 1;
}

/* ---------------------------------------------------------------- cartucho 3D
 * Malha (cartucho.malha: "PXM1", número de triângulos, e por vértice x y z, normal, u v; altura 1, frente em +z)
 * e textura (BMP) carregadas da pasta do projeto. Rasterizado aqui com z-buffer, textura em perspectiva,
 * luz difusa + especular e 2x2 amostras por pixel. */
typedef struct { MuImg tex; float *v; int ntri; } MuCart;
static MU_UNUSED int mu_cart_load(MuCart *ct, const char *malha, const char *bmp) {
    memset(ct, 0, sizeof *ct);
    FILE *f = fopen(malha, "rb"); if (!f) return 0;
    char m[4]; uint32_t n = 0;
    if (fread(m, 1, 4, f) != 4 || memcmp(m, "PXM1", 4) || fread(&n, 4, 1, f) != 1 || n == 0 || n > 100000) { fclose(f); return 0; }
    ct->v = malloc((size_t)n * 24 * sizeof(float));
    if (!ct->v || fread(ct->v, sizeof(float) * 24, n, f) != n) { fclose(f); free(ct->v); ct->v = NULL; return 0; }
    fclose(f); ct->ntri = (int)n;
    if (!mu_img_bmp(&ct->tex, bmp)) { free(ct->v); ct->v = NULL; ct->ntri = 0; return 0; }
    return 1;
}
static uint32_t mu_tex(const MuImg *im, float u, float v) {          /* bilinear, com repetição presa à borda */
    float x = u * im->w - 0.5f, y = (1 - v) * im->h - 0.5f; int x0 = (int)floorf(x), y0 = (int)floorf(y); float fx = x - x0, fy = y - y0;
    if (x0 < 0) { x0 = 0; fx = 0; } if (y0 < 0) { y0 = 0; fy = 0; } if (x0 >= im->w - 1) { x0 = im->w - 2; fx = 1; } if (y0 >= im->h - 1) { y0 = im->h - 2; fy = 1; }
    const uint32_t *p = im->px + (long)y0 * im->w + x0;
    uint32_t a = mu_lerp(p[0], p[1], (int)(fx * 255)), b = mu_lerp(p[im->w], p[im->w + 1], (int)(fx * 255));
    return mu_lerp(a, b, (int)(fy * 255));
}
/* desenha o cartucho centrado em (cx, cy), com altura `size` pixels, girado `yaw` (rad, em volta do eixo vertical)
 * e inclinado `pitch`; `glow` (0..1) clareia um pouco (foco) */
static MU_UNUSED void mu_cart_draw(MuCanvas *dst, const MuCart *ct, float cx, float cy, float size, float yaw, float pitch, float glow) {
    if (!ct->v || !ct->tex.px) return;
    int S = (int)(size * 1.3f) + 4, W = S * 2;
    uint32_t *col = calloc((size_t)W * W, 4); float *zb = calloc((size_t)W * W, sizeof(float));
    if (!col || !zb) { free(col); free(zb); return; }
    float D = 5.0f, f = (float)W / 1.3f * D;                               /* altura 1 ocupa ~1/1.3 da área */
    float cyw = cosf(yaw), syw = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    float Lx = -0.35f, Ly = 0.55f, Lz = 0.76f;                               /* luz (no espaço da câmera) */
    float Hx = Lx, Hy = Ly, Hz = Lz + 1; { float l = sqrtf(Hx * Hx + Hy * Hy + Hz * Hz); Hx /= l; Hy /= l; Hz /= l; }
    for (int t = 0; t < ct->ntri; t++) {
        float X[3], Y[3], Wv[3], N[3][3], U[3], V[3];
        for (int k = 0; k < 3; k++) {
            const float *p = ct->v + (t * 3 + k) * 8;
            float x = p[0] * cyw + p[2] * syw, z = -p[0] * syw + p[2] * cyw, y = p[1];
            float y2 = y * cp - z * sp, z2 = y * sp + z * cp;
            float nx = p[3] * cyw + p[5] * syw, nz = -p[3] * syw + p[5] * cyw, ny = p[4];
            N[k][0] = nx; N[k][1] = ny * cp - nz * sp; N[k][2] = ny * sp + nz * cp;
            float w = 1.0f / (D - z2);
            X[k] = W / 2.0f + f * x * w; Y[k] = W / 2.0f - f * y2 * w; Wv[k] = w; U[k] = p[6]; V[k] = p[7];
        }
        float area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
        if (area >= -1e-6f) continue;                                     /* costas (e degenerados) */
        int x0 = (int)floorf(fminf(X[0], fminf(X[1], X[2]))), x1 = (int)ceilf(fmaxf(X[0], fmaxf(X[1], X[2])));
        int y0 = (int)floorf(fminf(Y[0], fminf(Y[1], Y[2]))), y1 = (int)ceilf(fmaxf(Y[0], fmaxf(Y[1], Y[2])));
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > W) x1 = W; if (y1 > W) y1 = W;
        float ia = 1.0f / area;
        /* funções de aresta incrementais: b0 = A0*px + B0*py + C0 (já divididas pela área) */
        float A0 = (Y[1] - Y[2]) * ia, B0 = (X[2] - X[1]) * ia, C0 = (X[1] * Y[2] - X[2] * Y[1]) * ia;
        float A1 = (Y[2] - Y[0]) * ia, B1 = (X[0] - X[2]) * ia, C1 = (X[2] * Y[0] - X[0] * Y[2]) * ia;
        for (int j = y0; j < y1; j++) {
            float py = j + 0.5f, px0 = x0 + 0.5f;
            float b0 = A0 * px0 + B0 * py + C0, b1 = A1 * px0 + B1 * py + C1;
            float *zr = zb + (long)j * W; uint32_t *cr = col + (long)j * W;
            for (int i = x0; i < x1; i++, b0 += A0, b1 += A1) {
                float b2 = 1 - b0 - b1;
                if (b0 < -1e-5f || b1 < -1e-5f || b2 < -1e-5f) continue;
                float w = b0 * Wv[0] + b1 * Wv[1] + b2 * Wv[2];
                if (w <= zr[i]) continue;
                float iw = 1.0f / w, q0 = b0 * Wv[0] * iw, q1 = b1 * Wv[1] * iw, q2 = 1 - q0 - q1;   /* em perspectiva */
                float u = q0 * U[0] + q1 * U[1] + q2 * U[2], v = q0 * V[0] + q1 * V[1] + q2 * V[2];
                float nx = q0 * N[0][0] + q1 * N[1][0] + q2 * N[2][0], ny = q0 * N[0][1] + q1 * N[1][1] + q2 * N[2][1], nz = q0 * N[0][2] + q1 * N[1][2] + q2 * N[2][2];
                float nl = nx * nx + ny * ny + nz * nz; if (nl > 0) { nl = 1.0f / sqrtf(nl); nx *= nl; ny *= nl; nz *= nl; }
                float d = nx * Lx + ny * Ly + nz * Lz; if (d < 0) d = 0;
                float hs = nx * Hx + ny * Hy + nz * Hz; if (hs < 0) hs = 0;
                hs *= hs; hs *= hs; hs *= hs; hs *= hs; hs *= hs;                 /* ^32 */
                float spec = hs * 0.28f * 255;
                float lum = 0.46f + 0.56f * d + glow * 0.12f;
                uint32_t c = mu_tex(&ct->tex, u, v);
                float r = (c >> 16 & 255) * lum + spec, g = (c >> 8 & 255) * lum + spec, b = (c & 255) * lum + spec;
                if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
                cr[i] = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b; zr[i] = w;
            }
        }
    }
    int bx = (int)(cx - S / 2.0f), by = (int)(cy - S / 2.0f);
    for (int j = 0; j < S; j++) {
        int y = by + j; if (y < 0 || y >= dst->h) continue;
        for (int i = 0; i < S; i++) {
            int x = bx + i; if (x < 0 || x >= dst->w) continue;
            const uint32_t *p = col + (long)(2 * j) * W + 2 * i;
            uint32_t q[4] = { p[0], p[1], p[W], p[W + 1] };
            if (!(q[0] | q[1] | q[2] | q[3])) continue;
            uint32_t s = 0;
            for (int sh = 0; sh < 32; sh += 8) s |= ((((q[0] >> sh) & 255) + ((q[1] >> sh) & 255) + ((q[2] >> sh) & 255) + ((q[3] >> sh) & 255) + 2) >> 2) << sh;
            dst->px[(long)y * dst->w + x] = mu_over(dst->px[(long)y * dst->w + x], s);
        }
    }
    free(col); free(zb);
}
#endif

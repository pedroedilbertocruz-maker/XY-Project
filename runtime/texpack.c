/* Pacotes de texturas no formato do Citra/Azahar (texturas personalizadas).
 *
 * O pacote é uma pasta com arquivos "tex1_<largura>x<altura>_<hash>_<formato>[_mip0].png"; cada um substitui a
 * textura do jogo com aquele hash, normalmente em resolução maior (o desenho continua o mesmo: as coordenadas de
 * textura são normalizadas). Coloque o conteúdo do pacote em texturas/<ID do jogo>/ (pasta do projeto), por exemplo
 * texturas/0004000000055D00/ para Pokémon X. Subpastas valem. Nenhum pacote vem com o projeto.
 *
 * Pacotes sem pack.json (a maioria dos de Pokémon X/Y) usam o hash "antigo" do Citra: CityHash64 da textura
 * decodificada como o Citra decodifica sem conversão (MortonCopy<true, formato>): de baixo para cima,
 * RGBA8/RGB8/RGB565/RGB5A1/RGBA4 com os bytes como estão na memória e os outros formatos em RGBA de 8 bits.
 * Com pack.json, "use_new_hash" (padrão: sim) escolhe o hash novo, CityHash64 dos bytes da textura como estão na
 * memória, e "flip_png_files": false diz que os PNG estão de cabeça para baixo em relação ao padrão. A decodificação abaixo foi escrita a partir da descrição
 * desse formato; o CityHash64 é o da Google (licença MIT, abaixo).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG                        /* (JPEG: a arte de fundo do menu, menu_jogo.h) */
#define STBI_NO_STDIO
#include "stb_image.h"

/* ------------------------------------------------------------ CityHash64
 * Copyright (c) 2011 Google, Inc. — licença MIT:
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to the following conditions: The above copyright
 * notice and this permission notice shall be included in all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED. (CityHash, por Geoff Pike e
 * Jyrki Alakuijala.) */
static const uint64_t k0 = 0xc3a5c85c97cb3127ULL, k1 = 0xb492b66fbe98f273ULL, k2 = 0x9ae16a3b2f90404fULL;
static uint64_t F64(const uint8_t *p) { uint64_t r; memcpy(&r, p, 8); return r; }
static uint32_t F32(const uint8_t *p) { uint32_t r; memcpy(&r, p, 4); return r; }
static uint64_t Rot(uint64_t v, int s) { return s == 0 ? v : ((v >> s) | (v << (64 - s))); }
static uint64_t SMix(uint64_t v) { return v ^ (v >> 47); }
static uint64_t Bswap64(uint64_t v) { return __builtin_bswap64(v); }
static uint64_t H128to64(uint64_t lo, uint64_t hi) {
    const uint64_t m = 0x9ddfea08eb382d69ULL;
    uint64_t a = (lo ^ hi) * m; a ^= a >> 47;
    uint64_t b = (hi ^ a) * m; b ^= b >> 47; b *= m; return b;
}
static uint64_t HL16m(uint64_t u, uint64_t v, uint64_t mul) {
    uint64_t a = (u ^ v) * mul; a ^= a >> 47;
    uint64_t b = (v ^ a) * mul; b ^= b >> 47; b *= mul; return b;
}
static uint64_t HL0to16(const uint8_t *s, size_t len) {
    if (len >= 8) {
        uint64_t mul = k2 + len * 2, a = F64(s) + k2, b = F64(s + len - 8);
        uint64_t c = Rot(b, 37) * mul + a, d = (Rot(a, 25) + b) * mul;
        return HL16m(c, d, mul);
    }
    if (len >= 4) { uint64_t mul = k2 + len * 2, a = F32(s); return HL16m(len + (a << 3), F32(s + len - 4), mul); }
    if (len > 0) {
        uint8_t a = s[0], b = s[len >> 1], c = s[len - 1];
        uint32_t y = (uint32_t)a + ((uint32_t)b << 8), z = (uint32_t)len + ((uint32_t)c << 2);
        return SMix(y * k2 ^ z * k0) * k2;
    }
    return k2;
}
static uint64_t HL17to32(const uint8_t *s, size_t len) {
    uint64_t mul = k2 + len * 2, a = F64(s) * k1, b = F64(s + 8), c = F64(s + len - 8) * mul, d = F64(s + len - 16) * k2;
    return HL16m(Rot(a + b, 43) + Rot(c, 30) + d, a + Rot(b + k2, 18) + c, mul);
}
static void WHL32(uint64_t w, uint64_t x, uint64_t y, uint64_t z, uint64_t a, uint64_t b, uint64_t *o1, uint64_t *o2) {
    a += w; b = Rot(b + a + z, 21); uint64_t c = a; a += x; a += y; b += Rot(a, 44); *o1 = a + z; *o2 = b + c;
}
static void WHL32s(const uint8_t *s, uint64_t a, uint64_t b, uint64_t *o1, uint64_t *o2) {
    WHL32(F64(s), F64(s + 8), F64(s + 16), F64(s + 24), a, b, o1, o2);
}
static uint64_t HL33to64(const uint8_t *s, size_t len) {
    uint64_t mul = k2 + len * 2, a = F64(s) * k2, b = F64(s + 8), c = F64(s + len - 24), d = F64(s + len - 32);
    uint64_t e = F64(s + 16) * k2, f = F64(s + 24) * 9, g = F64(s + len - 8), h = F64(s + len - 16) * mul;
    uint64_t u = Rot(a + g, 43) + (Rot(b, 30) + c) * 9, v = ((a + g) ^ d) + f + 1;
    uint64_t w = Bswap64((u + v) * mul) + h, x = Rot(e + f, 42) + c, y = (Bswap64((v + w) * mul) + g) * mul, z = e + f + c;
    a = Bswap64((x + z) * mul + y) + b; b = SMix((z + a) * mul + d + h) * mul;
    return b + x;
}
uint64_t texpack_cityhash64(const uint8_t *s, size_t len) {
    if (len <= 32) return len <= 16 ? HL0to16(s, len) : HL17to32(s, len);
    if (len <= 64) return HL33to64(s, len);
    uint64_t x = F64(s + len - 40), y = F64(s + len - 16) + F64(s + len - 56), z = H128to64(F64(s + len - 48) + len, F64(s + len - 24));
    uint64_t v1, v2, w1, w2;
    WHL32s(s + len - 64, len, z, &v1, &v2);
    WHL32s(s + len - 32, y + k1, x, &w1, &w2);
    x = x * k1 + F64(s);
    len = (len - 1) & ~(size_t)63;
    do {
        x = Rot(x + y + v1 + F64(s + 8), 37) * k1;
        y = Rot(y + v2 + F64(s + 48), 42) * k1;
        x ^= w2;
        y += v1 + F64(s + 40);
        z = Rot(z + w1, 33) * k1;
        WHL32s(s, v2 * k1, x + w1, &v1, &v2);
        WHL32s(s + 32, z + w2, y + F64(s + 16), &w1, &w2);
        uint64_t t = z; z = x; x = t;
        s += 64; len -= 64;
    } while (len != 0);
    return H128to64(H128to64(v1, w1) + SMix(y) * k1 + z, H128to64(v2, w2) + x);
}

/* ------------------------------------------------------------ decodificação compatível com o hash antigo do Citra */
static const uint8_t k_bits[14] = { 32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8 };   /* bits por texel na memória */
static const uint8_t k_lbpp[14] = { 4, 3, 2, 2, 2, 4, 4, 4, 4, 4, 4, 4, 4, 4 };           /* bytes por texel decodificado */
static const uint8_t k_mrow[8] = { 0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A }, k_mx[8] = { 0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15 };
static uint8_t c4(uint8_t v) { return (uint8_t)((v << 4) | v); }
static uint8_t c5(uint8_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static void etc1_bloco(uint64_t blk, uint64_t alfa, int tem_alfa, uint8_t *lin, size_t pitch, int linha0, int x0) {
    static const int mods[8][4] = { { 2, 8, -2, -8 }, { 5, 17, -5, -17 }, { 9, 29, -9, -29 }, { 13, 42, -13, -42 },
                                    { 18, 60, -18, -60 }, { 24, 80, -24, -80 }, { 33, 106, -33, -106 }, { 47, 183, -47, -183 } };
    int flip = (blk >> 32) & 1, dif = (blk >> 33) & 1;
    unsigned tab[2] = { (unsigned)(blk >> 37) & 7, (unsigned)(blk >> 34) & 7 };
    uint32_t base[2] = { 0, 0 };
    static const int sh[3] = { 56, 48, 40 };
    for (int c = 0; c < 3; c++) {
        uint32_t val = (uint32_t)(blk >> (sh[c] + 3)) & 0x1F;
        int32_t d = (int32_t)((uint32_t)((blk >> sh[c]) & 7) << 29) >> 29;
        uint32_t d1 = c5((uint8_t)val), d2 = c5((uint8_t)((int32_t)val + d));
        uint32_t s1 = c4((uint8_t)((blk >> (sh[c] + 4)) & 0xF)), s2 = c4((uint8_t)((blk >> sh[c]) & 0xF));
        base[0] |= (dif ? d1 : s1) << (8 * c); base[1] |= (dif ? d2 : s2) << (8 * c);
    }
    uint32_t pal[8];
    for (int s = 0; s < 2; s++) for (int m = 0; m < 4; m++) {
        uint32_t col = tem_alfa ? 0u : 0xFF000000u;
        for (int c = 0; c < 3; c++) { int v = (int)((base[s] >> (8 * c)) & 0xFF) + mods[tab[s]][m]; col |= (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v) << (8 * c); }
        pal[s * 4 + m] = col;
    }
    uint32_t lsb = (uint32_t)blk & 0xFFFF, msb = (uint32_t)(blk >> 16) & 0xFFFF;
    for (int y = 0; y < 4; y++) {
        uint32_t sub = flip ? (y >= 2 ? 0x4444u : 0u) : 0x4400u;
        uint32_t idx = ((lsb >> y) & 0x1111u) | (((msb >> y) & 0x1111u) << 1) | sub;
        uint8_t *d = lin + (size_t)(linha0 - y) * pitch + (size_t)x0 * 4;
        for (int k = 0; k < 4; k++) {
            uint32_t px = pal[(idx >> (4 * k)) & 7];
            if (tem_alfa) px |= (uint32_t)c4((uint8_t)((alfa >> (4 * (4 * k + y))) & 0xF)) << 24;
            memcpy(d + k * 4, &px, 4);
        }
    }
}
/* decodifica (w x h, de baixo para cima) como o Citra faz para o hash; devolve o tamanho ou 0 */
static size_t citra_decodificar(const uint8_t *src, int w, int h, int fmt, uint8_t *out) {
    if (fmt < 0 || fmt > 13 || (w & 7) || (h & 7)) return 0;
    int lb = k_lbpp[fmt], tb = k_bits[fmt] * 64 / 8; size_t pitch = (size_t)w * lb;
    const uint8_t *t = src;
    for (int ty = 0; ty < h; ty += 8) for (int tx = 0; tx < w; tx += 8, t += tb) {
        int base = h - 8 - ty;                                  /* primeira linha (de baixo para cima) do bloco 8x8 */
        if (fmt == 12 || fmt == 13) {
            int bs = fmt == 13 ? 16 : 8;
            for (int i = 0; i < 4; i++) {
                const uint8_t *b = t + i * bs; uint64_t a = 0;
                if (fmt == 13) { a = F64(b); b += 8; }
                etc1_bloco(F64(b), a, fmt == 13, out, pitch, base + 7 - 4 * (i >> 1), tx + 4 * (i & 1));
            }
            continue;
        }
        for (int y = 0; y < 8; y++) {
            uint8_t *d = out + (size_t)(base + 7 - y) * pitch + (size_t)tx * lb;
            for (int x = 0; x < 8; x++, d += lb) {
                int m = k_mrow[y] + k_mx[x];
                if (fmt == 10 || fmt == 11) {                   /* I4 / A4: nibble baixo primeiro */
                    uint8_t v = c4((uint8_t)((t[m >> 1] >> ((m & 1) * 4)) & 0xF));
                    if (fmt == 10) { d[0] = d[1] = d[2] = v; d[3] = 255; } else { d[0] = d[1] = d[2] = 0; d[3] = v; }
                    continue;
                }
                const uint8_t *p = t + m * (k_bits[fmt] / 8);
                switch (fmt) {
                case 5: d[0] = d[1] = d[2] = p[1]; d[3] = p[0]; break;             /* IA8 */
                case 6: d[0] = p[1]; d[1] = p[0]; d[2] = 0; d[3] = 255; break;     /* RG8 */
                case 7: d[0] = d[1] = d[2] = p[0]; d[3] = 255; break;              /* I8 */
                case 8: d[0] = d[1] = d[2] = 0; d[3] = p[0]; break;                /* A8 */
                case 9: d[0] = d[1] = d[2] = c4(p[0] >> 4); d[3] = c4(p[0] & 0xF); break;   /* IA4 */
                default: memcpy(d, p, (size_t)lb); break;                          /* RGBA8/RGB8/565/5A1/RGBA4: como estão */
                }
            }
        }
    }
    return pitch * (size_t)h;
}
static int g_tp_novo, g_tp_virar;                     /* do pack.json: hash novo; PNG invertidos */
uint64_t texpack_hash(const uint8_t *src, int w, int h, int fmt) {
    if (fmt < 0 || fmt > 13 || w <= 0 || h <= 0) return 0;
    if (g_tp_novo) return texpack_cityhash64(src, (size_t)w * h * k_bits[fmt] / 8);
    static uint8_t *buf; static size_t cap;
    size_t need = (size_t)w * h * 4;
    if (need > cap) { cap = need; free(buf); buf = malloc(cap); if (!buf) { cap = 0; return 0; } }
    size_t n = citra_decodificar(src, w, h, fmt, buf);
    return n ? texpack_cityhash64(buf, n) : 0;
}

/* ------------------------------------------------------------ índice do pacote e leitura dos PNG */
typedef struct { uint64_t hash; char *path; int w, h, fmt; uint8_t *px; int pw, ph; volatile int st; int uso_contado; unsigned long uso; } TpE;   /* st: 0 no disco, 1 lendo, 2 pronto, 3 erro */
static TpE *g_tp; static int g_ntp, g_captp;
static size_t g_tp_bytes; static unsigned long g_tp_relogio;   /* PNG lidos guardados na memória (para recarregar) */
volatile int g_texpack_on = 1, g_texpack_gen = 1;
static int g_tp_init;
static pthread_mutex_t g_tp_mx = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_tp_cv = PTHREAD_COND_INITIALIZER;
static int g_tp_fila[256], g_tp_ini, g_tp_n;
static char g_tp_json[1024];
static void tp_scan(const char *dir, int prof) {
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (!g_tp_json[0] && !strcmp(e->d_name, "pack.json")) { snprintf(g_tp_json, sizeof g_tp_json, "%s", p); continue; }
        unsigned w, h, f; unsigned long long hs;
        if (!strncmp(e->d_name, "tex1_", 5) && sscanf(e->d_name, "tex1_%ux%u_%llX_%u", &w, &h, &hs, &f) == 4) {
            const char *mip = strstr(e->d_name, "_mip");
            if (mip && atoi(mip + 4) != 0) continue;            /* (só o nível 0; os menores saem dele) */
            size_t L = strlen(e->d_name); if (L < 4 || strcmp(e->d_name + L - 4, ".png")) continue;
            if (g_ntp == g_captp) { g_captp = g_captp ? g_captp * 2 : 1024; g_tp = realloc(g_tp, sizeof *g_tp * (size_t)g_captp); }
            TpE *t = &g_tp[g_ntp++]; memset(t, 0, sizeof *t);
            t->hash = hs; t->w = (int)w; t->h = (int)h; t->fmt = (int)f; t->path = strdup(p);
            continue;
        }
        struct stat st; if (prof < 6 && stat(p, &st) == 0 && S_ISDIR(st.st_mode)) tp_scan(p, prof + 1);
    }
    closedir(d);
}
static int tp_cmp(const void *a, const void *b) { uint64_t x = ((const TpE *)a)->hash, y = ((const TpE *)b)->hash; return x < y ? -1 : x > y; }
static void *tp_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_tp_mx);
        while (!g_tp_n) pthread_cond_wait(&g_tp_cv, &g_tp_mx);
        int i = g_tp_fila[g_tp_ini]; g_tp_ini = (g_tp_ini + 1) % 256; g_tp_n--;
        pthread_mutex_unlock(&g_tp_mx);
        TpE *t = &g_tp[i];
        FILE *f = fopen(t->path, "rb"); uint8_t *buf = NULL; long n = 0; int ok = 0;
        if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
                 if (n > 0 && (buf = malloc((size_t)n)) && fread(buf, 1, (size_t)n, f) == (size_t)n) ok = 1; fclose(f); }
        int pw = 0, ph = 0, ch = 0; uint8_t *px = ok ? stbi_load_from_memory(buf, (int)n, &pw, &ph, &ch, 4) : NULL;
        free(buf);
        if (px && g_tp_virar) {                         /* (pack.json com "flip_png_files": false) */
            size_t ln = (size_t)pw * 4; uint8_t *tmp = malloc(ln);
            if (tmp) for (int y = 0; y < ph / 2; y++) { uint8_t *a = px + (size_t)y * ln, *b = px + (size_t)(ph - 1 - y) * ln;
                memcpy(tmp, a, ln); memcpy(a, b, ln); memcpy(b, tmp, ln); }
            free(tmp);
        }
        if (px) {
            t->px = px; t->pw = pw; t->ph = ph; __atomic_store_n(&t->st, 2, __ATOMIC_RELEASE); }
        else { __atomic_store_n(&t->st, 3, __ATOMIC_RELEASE); fprintf(stderr, "[texturas] não consegui ler %s\n", t->path); }
    }
    return NULL;
}
static pthread_mutex_t g_tp_init_mx = PTHREAD_MUTEX_INITIALIZER;
int texpack_init(void) {                               /* (chamada do menu e da thread de desenho: a primeira faz o índice) */
    if (__atomic_load_n(&g_tp_init, __ATOMIC_ACQUIRE)) return g_ntp;
    pthread_mutex_lock(&g_tp_init_mx);
    if (!g_tp_init) {
        const char *dir = getenv("TEXTURAS_DIR"), *on = getenv("TEXTURAS");
        if (on && *on == '0') g_texpack_on = 0;
        if (dir && *dir) {
            tp_scan(dir, 0);
            if (g_tp_json[0]) {                        /* opções do pacote (só as duas que mudam a leitura) */
                FILE *f = fopen(g_tp_json, "rb"); char js[8192] = { 0 };
                if (f) { size_t n = fread(js, 1, sizeof js - 1, f); js[n] = 0; fclose(f); }
                const char *o = strstr(js, "\"use_new_hash\""); o = o ? strchr(o, ':') : NULL;
                g_tp_novo = !(o && strncmp(o + 1 + strspn(o + 1, " \t\r\n"), "false", 5) == 0);
                o = strstr(js, "\"flip_png_files\""); o = o ? strchr(o, ':') : NULL;
                g_tp_virar = o && strncmp(o + 1 + strspn(o + 1, " \t\r\n"), "false", 5) == 0;
                fprintf(stderr, "[texturas] %s: hash %s%s\n", g_tp_json, g_tp_novo ? "novo" : "antigo", g_tp_virar ? ", PNG invertidos" : "");
            }
            if (!g_ntp) fprintf(stderr, "[texturas] nenhuma textura personalizada em %s\n", dir);
            else {
                qsort(g_tp, (size_t)g_ntp, sizeof *g_tp, tp_cmp);
                pthread_t th; if (pthread_create(&th, NULL, tp_thread, NULL) == 0) pthread_detach(th); else g_ntp = 0;
                fprintf(stderr, "[texturas] %d textura(s) personalizada(s) em %s%s\n", g_ntp, dir, g_texpack_on ? "" : " (desligadas no menu)");
            }
        }
        __atomic_store_n(&g_tp_init, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_tp_init_mx);
    return g_ntp;
}
int texpack_ativo(void) { return texpack_init() > 0 && g_texpack_on; }
/* procura a textura personalizada deste hash. 1: pronta (*px em RGBA, na ordem de linhas do PNG, que já é a do
 * envio da textura à placa); 0: não há; -1: está sendo lida (tente de novo depois) */
static int g_tp_log = -1;
int texpack_get(uint64_t hash, const uint8_t **px, int *pw, int *ph) {
    if (!g_ntp || !hash) return 0;
    int lo = 0, hi = g_ntp - 1, i = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_tp[m].hash == hash) { i = m; break; } if (g_tp[m].hash < hash) lo = m + 1; else hi = m - 1; }
    if (g_tp_log < 0) { const char *e = getenv("TEXPACK_LOG"); g_tp_log = e ? atoi(e) : 0; }
    if (i < 0) { if (g_tp_log >= 2) fprintf(stderr, "[texturas] sem substituta: %016llX\n", (unsigned long long)hash); return 0; }
    TpE *t = &g_tp[i];
    if (g_tp_log && t->st == 0 && !t->uso) fprintf(stderr, "[texturas] usando %s\n", t->path);
    int st = __atomic_load_n(&t->st, __ATOMIC_ACQUIRE);
    if (st == 2) {
        t->uso = ++g_tp_relogio;
        if (!t->uso_contado) { t->uso_contado = 1; g_tp_bytes += (size_t)t->pw * t->ph * 4; }
        while (g_tp_bytes > ((size_t)1 << 30)) {        /* mais de 1 GB guardado: solta os usados há mais tempo */
            TpE *v = NULL;
            for (int k = 0; k < g_ntp; k++) if (g_tp[k].uso_contado && &g_tp[k] != t && (!v || g_tp[k].uso < v->uso)) v = &g_tp[k];
            if (!v) break;
            g_tp_bytes -= (size_t)v->pw * v->ph * 4; v->uso_contado = 0;
            stbi_image_free(v->px); v->px = NULL; __atomic_store_n(&v->st, 0, __ATOMIC_RELEASE);
        }
        *px = t->px; *pw = t->pw; *ph = t->ph; return 1;
    }
    if (st == 3) return 0;
    if (st == 0) {
        pthread_mutex_lock(&g_tp_mx);
        if (t->st == 0 && g_tp_n < 256) { t->st = 1; g_tp_fila[(g_tp_ini + g_tp_n) % 256] = i; g_tp_n++; pthread_cond_signal(&g_tp_cv); }
        pthread_mutex_unlock(&g_tp_mx);
    }
    return -1;
}

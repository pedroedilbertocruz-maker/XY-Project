/* Iluminação por fragmento da PICA200 ("fragment lighting").
 *
 * Os modelos 3D do jogo não usam a cor do vértice: o TEV lê as fontes 1 e 2, que são a cor difusa
 * (primária) e a especular (secundária) calculadas por pixel a partir da normal (um quatérnio vindo
 * do shader de vértice), do vetor de visão e de até 8 luzes, com tabelas (LUTs) de 256 entradas.
 * Registradores 0x140-0x1D9 e o cálculo seguem a documentação do 3dbrew ("GPU/Internal Registers",
 * lighting) e o comportamento da implementação de referência do Citra/Azahar.
 */
#include "pica.h"
#include "pica_int.h"
#include <math.h>
#include <string.h>

#define R(i) g_regs[i]

/* LUTs: 0 D0, 1 D1, 3 Fresnel, 4 RB, 5 RG, 6 RR, 8-15 spot das luzes 0-7, 16-23 atenuação por distância */
static float g_lut[24][256][2];
int g_light_lut_dirty = 1; unsigned g_light_lut_gen;
const float *g_light_lut_override;                 /* 60 fps: tabela gravada de outro quadro (refazendo o desenho) */
const float *light_lut_data(void) { return g_light_lut_override ? g_light_lut_override : &g_lut[0][0][0]; }

void light_lut_write(uint32_t value) {
    g_light_lut_dirty = 1; g_light_lut_gen++;
    uint32_t cfg = R(0x1C5), idx = cfg & 0xFF, sel = (cfg >> 8) & 0x1F;
    if (sel < 24) {
        g_lut[sel][idx][0] = (float)(value & 0xFFF) / 4095.0f;
        float d = (float)((value >> 12) & 0x7FF) / 4095.0f;
        g_lut[sel][idx][1] = (value >> 23) & 1 ? -d : d;
    }
    R(0x1C5) = (cfg & ~0xFFu) | ((idx + 1) & 0xFF);
}

typedef struct { float x, y, z; } V3;
static V3 v3(float x, float y, float z) { V3 r = { x, y, z }; return r; }
static V3 add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V3 sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 mul(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static V3 norm(V3 a) { float l = sqrtf(dot(a, a)); return l > 0 ? mul(a, 1.0f / l) : a; }

static float f16(uint32_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? INFINITY : ldexpf((float)(m | 0x400), (int)e - 25);
    return s ? -v : v;
}
static float f20(uint32_t h) {        /* 1 sinal, 7 expoente, 12 mantissa */
    uint32_t s = (h >> 19) & 1, e = (h >> 12) & 0x7F, m = h & 0xFFF;
    if (!e && !m) return 0;
    float v = ldexpf(1.0f + (float)m / 4096.0f, (int)e - 63);
    return s ? -v : v;
}
static float s11(uint32_t v) { int x = (int)(v & 0x1FFF); if (x & 0x1000) x -= 0x2000; return (float)x / 2047.0f; }
static V3 color10(uint32_t c) { return v3((float)((c >> 20) & 0xFF) / 255.0f, (float)((c >> 10) & 0xFF) / 255.0f, (float)(c & 0xFF) / 255.0f); }

typedef struct {
    int num; V3 spec0, spec1, diffuse, ambient, pos, spot;
    int directional, two_sided, geo0, geo1; float da_bias, da_scale;
} Light;
static Light g_lights[8];
static int g_nlights, g_cfg, g_fresnel_sel, g_clamp, g_bump_mode, g_bump_sel, g_bump_renorm;
static uint32_t g_cfg1, g_abs, g_input, g_scale_bits;
static V3 g_global_ambient;
int g_light_on;

int light_enabled(void) { return (R(0x8F) & 1) && !(R(0x1C6) & 1); }

void light_setup(void) {
    g_light_on = light_enabled();
    if (!g_light_on) return;
    g_nlights = (int)(R(0x1C2) & 7) + 1;
    uint32_t c0 = R(0x1C3);
    g_fresnel_sel = (c0 >> 2) & 3; g_cfg = (c0 >> 4) & 0xF; g_clamp = (c0 >> 27) & 1;
    g_bump_sel = (c0 >> 22) & 3; g_bump_mode = (c0 >> 28) & 3; g_bump_renorm = !((c0 >> 30) & 1);
    g_cfg1 = R(0x1C4); g_abs = R(0x1D0); g_input = R(0x1D1); g_scale_bits = R(0x1D2);
    g_global_ambient = color10(R(0x1C0));
    for (int i = 0; i < g_nlights; i++) {
        Light *l = &g_lights[i];
        int n = (int)((R(0x1D9) >> (4 * i)) & 7);
        uint32_t b = 0x140 + 0x10 * n;
        l->num = n;
        l->spec0 = color10(R(b)); l->spec1 = color10(R(b + 1)); l->diffuse = color10(R(b + 2)); l->ambient = color10(R(b + 3));
        l->pos = v3(f16(R(b + 4) & 0xFFFF), f16(R(b + 4) >> 16), f16(R(b + 5) & 0xFFFF));
        l->spot = v3(s11(R(b + 6)), s11(R(b + 6) >> 16), s11(R(b + 7)));
        uint32_t cf = R(b + 9);
        l->directional = cf & 1; l->two_sided = (cf >> 1) & 1; l->geo0 = (cf >> 2) & 1; l->geo1 = (cf >> 3) & 1;
        l->da_bias = f20(R(b + 0xA) & 0xFFFFF); l->da_scale = f20(R(b + 0xB) & 0xFFFFF);
    }
}

/* quais tabelas cada configuração (0x1C3 bits 4-7) disponibiliza */
enum { S_D0, S_D1, S_SP, S_FR, S_RB, S_RG, S_RR };
static int supported(int s) {
    switch (g_cfg) {
    case 0: return s == S_D0 || s == S_RR || s == S_SP;
    case 1: return s == S_RR || s == S_SP || s == S_FR;
    case 2: return s == S_D0 || s == S_D1 || s == S_RR;
    case 3: return s == S_D0 || s == S_D1 || s == S_FR;
    case 4: return s != S_FR;
    case 5: return s != S_D1;
    case 6: return s != S_RG && s != S_RB;
    default: return 1;
    }
}
static int lut_enabled(int s) {
    static const int dis_bit[] = { 16, 17, -1, 19, 20, 21, 22 };
    if (s == S_SP) return 1;
    return !((g_cfg1 >> dis_bit[s]) & 1) && supported(s);
}
/* posição dos campos de cada tabela em 0x1D0/0x1D1/0x1D2 */
static const int k_field[] = { 0, 4, 8, 12, 16, 20, 24 };
static float scale_of(int s) {
    switch ((g_scale_bits >> k_field[s]) & 7) { case 1: return 2; case 2: return 4; case 3: return 8; case 6: return 0.25f; case 7: return 0.5f; default: return 1; }
}
static float lut(int table, float x, int is_abs) {
    int idx; float delta;
    if (is_abs) {
        x = fabsf(x);
        float f = floorf(x * 256.0f); if (f < 0) f = 0; if (f > 255) f = 255;
        idx = (int)f; delta = x * 256.0f - f;
    } else {
        float f = floorf(x * 128.0f); if (f < -128) f = -128; if (f > 127) f = 127;
        delta = x * 128.0f - f; idx = ((int)f) & 0xFF;
    }
    return g_lut[table][idx][0] + g_lut[table][idx][1] * delta;
}
static float sample(int s, int table, V3 n, V3 h, V3 v, V3 l, V3 spot, V3 tangent) {
    float x;
    switch ((g_input >> k_field[s]) & 7) {
    case 0: x = dot(n, h); break;
    case 1: x = dot(v, h); break;
    case 2: x = dot(n, v); break;
    case 3: x = dot(l, n); break;
    case 4: x = dot(mul(l, -1), spot); break;
    default: { V3 hp = sub(h, mul(n, dot(n, h))); x = dot(hp, tangent); }
    }
    int is_abs = !((g_abs >> (k_field[s] + 1)) & 1);
    return scale_of(s) * lut(table, x, is_abs);
}

static V3 qrot(const float q[4], V3 v) {
    V3 u = v3(q[0], q[1], q[2]);
    return add(v, mul(cross(u, add(cross(u, v), mul(v, q[3]))), 2.0f));
}
static uint8_t to8(float f) { return (uint8_t)(f <= 0 ? 0 : f >= 1 ? 255 : f * 255.0f + 0.5f); }

/* tex: as três texturas já amostradas (para o mapa de normais) */
void light_fragment(const float quat_in[4], const float view_in[3], const uint8_t tex[3][4], uint8_t prim[4], uint8_t sec[4]) {
    float ql = sqrtf(quat_in[0] * quat_in[0] + quat_in[1] * quat_in[1] + quat_in[2] * quat_in[2] + quat_in[3] * quat_in[3]);
    float q[4] = { 0, 0, 0, 1 };
    if (ql > 0) for (int k = 0; k < 4; k++) q[k] = quat_in[k] / ql;
    V3 surf = v3(0, 0, 1), surf_t = v3(1, 0, 0);
    if (g_bump_mode == 1 || g_bump_mode == 2) {
        const uint8_t *t = tex[g_bump_sel < 3 ? g_bump_sel : 0];
        V3 p = v3(t[0] / 127.5f - 1, t[1] / 127.5f - 1, t[2] / 127.5f - 1);
        if (g_bump_mode == 1) {
            if (g_bump_renorm) { float z2 = 1 - p.x * p.x - p.y * p.y; p.z = z2 > 0 ? sqrtf(z2) : 0; }
            surf = p;
        } else surf_t = p;
    }
    V3 n = norm(qrot(q, surf)), tangent = norm(qrot(q, surf_t));
    V3 view = v3(view_in[0], view_in[1], view_in[2]);
    V3 nv = norm(view);
    V3 dsum = v3(0, 0, 0), ssum = v3(0, 0, 0);
    float dalpha = 1, salpha = 1;
    for (int i = 0; i < g_nlights; i++) {
        const Light *L = &g_lights[i];
        V3 l = L->directional ? norm(L->pos) : norm(add(L->pos, view));
        V3 h = norm(add(nv, l));
        float ln = dot(l, n);
        float dp = L->two_sided ? fabsf(ln) : (ln > 0 ? ln : 0);
        float clamp = g_clamp && ln <= 0 ? 0 : 1;
        float geo = 1;
        if (L->geo0 || L->geo1) { float hh = dot(add(nv, l), add(nv, l)); geo = hh == 0 ? 0 : fminf(dp / hh, 1.0f); }
        float da = 1;
        if (!((g_cfg1 >> (24 + L->num)) & 1) && !L->directional) {
            float d = sqrtf(dot(add(view, L->pos), add(view, L->pos)));
            float x = d * L->da_scale + L->da_bias; if (x < 0) x = 0; if (x > 1) x = 1;
            da = lut(16 + L->num, x, 1);
        }
        float spot = 1;
        if (!((g_cfg1 >> (8 + L->num)) & 1) && supported(S_SP))
            spot = sample(S_SP, 8 + L->num, n, h, nv, l, L->spot, tangent);
        float d0 = lut_enabled(S_D0) ? sample(S_D0, 0, n, h, nv, l, L->spot, tangent) : 1;
        V3 s0 = mul(L->spec0, d0 * (L->geo0 ? geo : 1));
        V3 refl;
        refl.x = lut_enabled(S_RR) ? sample(S_RR, 6, n, h, nv, l, L->spot, tangent) : 1;
        refl.y = lut_enabled(S_RG) ? sample(S_RG, 5, n, h, nv, l, L->spot, tangent) : refl.x;
        refl.z = lut_enabled(S_RB) ? sample(S_RB, 4, n, h, nv, l, L->spot, tangent) : refl.x;
        float d1 = lut_enabled(S_D1) ? sample(S_D1, 1, n, h, nv, l, L->spot, tangent) : 1;
        V3 s1 = v3(L->spec1.x * refl.x, L->spec1.y * refl.y, L->spec1.z * refl.z);
        s1 = mul(s1, d1 * (L->geo1 ? geo : 1));
        if (lut_enabled(S_FR) && g_fresnel_sel) {
            float fr = sample(S_FR, 3, n, h, nv, l, L->spot, tangent);
            if (g_fresnel_sel & 1) dalpha = fr;
            if (g_fresnel_sel & 2) salpha = fr;
        }
        float att = da * spot;
        dsum = add(dsum, mul(add(mul(L->diffuse, dp), L->ambient), att));
        ssum = add(ssum, mul(add(s0, s1), clamp * att));
    }
    dsum = add(dsum, g_global_ambient);
    prim[0] = to8(dsum.x); prim[1] = to8(dsum.y); prim[2] = to8(dsum.z); prim[3] = to8(dalpha);
    sec[0] = to8(ssum.x); sec[1] = to8(ssum.y); sec[2] = to8(ssum.z); sec[3] = to8(salpha);
}

/* parâmetros já decodificados para o shader do renderer OpenGL (mesmo cálculo de light_fragment) */
void light_gl_params(uint32_t u[20], float l[49][3], float ld[8][2]) {
    u[0] = g_light_on; u[1] = g_nlights; u[2] = g_cfg; u[3] = g_fresnel_sel; u[4] = g_clamp;
    u[5] = g_bump_mode; u[6] = g_bump_sel; u[7] = g_bump_renorm; u[8] = g_cfg1; u[9] = g_abs; u[10] = g_input; u[11] = g_scale_bits;
    for (int i = 0; i < 8; i++) {
        const Light *L = &g_lights[i];
        u[12 + i] = L->directional | L->two_sided << 1 | L->geo0 << 2 | L->geo1 << 3 | L->num << 8;
        const V3 *v[6] = { &L->spec0, &L->spec1, &L->diffuse, &L->ambient, &L->pos, &L->spot };
        for (int k = 0; k < 6; k++) { l[i * 6 + k][0] = v[k]->x; l[i * 6 + k][1] = v[k]->y; l[i * 6 + k][2] = v[k]->z; }
        ld[i][0] = L->da_bias; ld[i][1] = L->da_scale;
    }
    l[48][0] = g_global_ambient.x; l[48][1] = g_global_ambient.y; l[48][2] = g_global_ambient.z;
}

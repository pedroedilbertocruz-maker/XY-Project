/* Renderer da PICA200 na placa de vídeo (OpenGL 3.3).
 *
 * Divisão do trabalho:
 *  - CPU (como no renderer em software): shader de vértice da PICA, montagem das primitivas, recorte e
 *    descarte de faces. Cada triângulo chega aqui já em coordenadas de tela, com 1/w para a correção de
 *    perspectiva — assim a rasterização da placa bate com a do software.
 *  - GPU: rasterização, texturas, iluminação por pixel, combinadores (TEV), teste alfa, estêncil,
 *    profundidade e blending, num único shader de fragmentos que lê os registradores da PICA.
 *
 * A memória do jogo continua sendo a referência: no primeiro desenho de uma lista de comandos num
 * framebuffer, a cor e a profundidade são copiadas da memória para a GPU; ao fim da lista (ou ao trocar
 * de framebuffer, ou antes de usar como textura o que acabou de ser desenhado), o resultado volta para a
 * memória. Transferências de tela, cópias e capturas seguem funcionando como antes.
 */
#include "pica.h"
#include "pica_int.h"
#include "plat.h"
#include "gl_funcs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define R(x) g_regs[x]
const char *g_render_name = "software";

#define GLP(ret, name, args) static ret (GLAPIENTRY *name) args;
GL_FUNCS(GLP)
#undef GLP

static int g_gl_state;       /* 0 = não tentou, 1 = pronto, -1 = sem OpenGL (usa software) */
/* Vulkan (pica_vk.c): escolhido no lançador (RENDER=vulkan); se não abrir, fica o OpenGL. As funções públicas
 * daqui passam a chamada adiante quando ele está ligado */
int g_vk; static int g_vk_decided;
int pica_vk_init(void); int pica_vk_active(void); int pica_vk_list_begin(void); void pica_vk_list_end(void);
void pica_vk_draw(const Fb *f, const Tri *t, int n); void pica_vk_flush(uint32_t a, uint32_t len); void pica_vk_will_write(uint32_t a, uint32_t len);
void pica_vk_written(uint32_t a, uint32_t len); int pica_vk_fill(uint32_t start, uint32_t end, uint32_t value, int width);
int pica_vk_transfer(uint32_t in, int iw, int ih, uint32_t out, int ow, int oh, uint32_t flags, int in_fmt, int out_fmt);
void pica_vk_poll(void); int pica_vk_pending(void); void pica_vk_pending_wait(int us); void pica_vk_finish_hd(void);
static int g_in_list;        /* contexto atual nesta lista */
static GLuint g_prog, g_vao, g_vbo, g_samplers[3], g_lut_tex, g_black_tex;
static GLint g_loc_R, g_loc_L, g_loc_LD, g_loc_fog;
static unsigned g_list_serial;
#define VSTRIDE 22               /* floats por vértice enviados à GPU */

/* ------------------------------------------------------------ shaders */
static const char *k_vs =
"#version 330 core\n"
"layout(location=0) in vec4 a_pos; layout(location=1) in vec4 a_color; layout(location=2) in vec4 a_tc01;\n"
"layout(location=3) in vec3 a_tc2; layout(location=4) in vec4 a_quat; layout(location=5) in vec3 a_view;\n"
"out vec4 v_color; out vec4 v_tc01; out vec3 v_tc2; out vec4 v_quat; out vec3 v_view;\n"
"void main() {\n"
"  gl_Position = vec4(a_pos.xyz * a_pos.w, a_pos.w);\n"
"  v_color = a_color; v_tc01 = a_tc01; v_tc2 = a_tc2; v_quat = a_quat; v_view = a_view;\n"
"}\n";

/* uR: 0-29 registradores dos 6 estágios do TEV (5 cada), 30 = 0xFD (buffer inicial), 31 = 0xE0
 * (atualização do buffer), 32 = 0x80 (texturas), 33 = 0x104 (teste alfa), 34-53 = iluminação. */
static const char *k_fs =
"#version 330 core\n"
"in vec4 v_color; in vec4 v_tc01; in vec3 v_tc2; in vec4 v_quat; in vec3 v_view;\n"
"out vec4 o_color;\n"
"uniform sampler2D u_tex0; uniform sampler2D u_tex1; uniform sampler2D u_tex2; uniform sampler2D u_lut;\n"
"uniform uint uR[64]; uniform vec3 uL[49]; uniform vec2 uLD[8]; uniform vec2 uFog[128];\n"
"ivec4 q8(vec4 c) { return ivec4(clamp(floor(c * 255.0 + 0.5), 0.0, 255.0)); }\n"
/* ---- iluminação (mesmo cálculo de pica_light.c) */
"float lutv(int table, float x, bool isabs) {\n"
"  int idx; float delta;\n"
"  if (isabs) { x = abs(x); float f = clamp(floor(x * 256.0), 0.0, 255.0); idx = int(f); delta = x * 256.0 - f; }\n"
"  else { float f = clamp(floor(x * 128.0), -128.0, 127.0); delta = x * 128.0 - f; idx = int(f) & 255; }\n"
"  vec2 e = texelFetch(u_lut, ivec2(idx, table), 0).rg;\n"
"  return e.x + e.y * delta;\n"
"}\n"
"bool supported(int s) {\n"
"  int c = int(uR[36]);\n"
"  if (c == 0) return s == 0 || s == 6 || s == 2;\n"
"  if (c == 1) return s == 6 || s == 2 || s == 3;\n"
"  if (c == 2) return s == 0 || s == 1 || s == 6;\n"
"  if (c == 3) return s == 0 || s == 1 || s == 3;\n"
"  if (c == 4) return s != 3;\n"
"  if (c == 5) return s != 1;\n"
"  if (c == 6) return s != 5 && s != 4;\n"
"  return true;\n"
"}\n"
"bool lut_enabled(int s) {\n"
"  if (s == 2) return true;\n"
"  int bit = s == 0 ? 16 : s == 1 ? 17 : s == 3 ? 19 : s == 4 ? 20 : s == 5 ? 21 : 22;\n"
"  return ((uR[42] >> uint(bit)) & 1u) == 0u && supported(s);\n"
"}\n"
"float scale_of(int s) {\n"
"  uint v = (uR[45] >> uint(4 * s)) & 7u;\n"
"  return v == 1u ? 2.0 : v == 2u ? 4.0 : v == 3u ? 8.0 : v == 6u ? 0.25 : v == 7u ? 0.5 : 1.0;\n"
"}\n"
"float lsample(int s, int table, vec3 n, vec3 h, vec3 v, vec3 l, vec3 spot, vec3 tangent) {\n"
"  float x; uint inp = (uR[44] >> uint(4 * s)) & 7u;\n"
"  if (inp == 0u) x = dot(n, h); else if (inp == 1u) x = dot(v, h); else if (inp == 2u) x = dot(n, v);\n"
"  else if (inp == 3u) x = dot(l, n); else if (inp == 4u) x = dot(-l, spot);\n"
"  else { vec3 hp = h - n * dot(n, h); x = dot(hp, tangent); }\n"
"  bool isabs = ((uR[43] >> uint(4 * s + 1)) & 1u) == 0u;\n"
"  return scale_of(s) * lutv(table, x, isabs);\n"
"}\n"
"vec3 qrot(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + v * q.w); }\n"
"vec3 nrm(vec3 a) { float l = length(a); return l > 0.0 ? a / l : a; }\n"
"ivec4 g_tex[3]; ivec4 g_prim, g_lp, g_ls, g_buf, g_konst, g_prev;\n"
"void lighting() {\n"
"  vec4 q = v_quat; float ql = length(q); q = ql > 0.0 ? q / ql : vec4(0.0, 0.0, 0.0, 1.0);\n"
"  vec3 surf = vec3(0.0, 0.0, 1.0), surf_t = vec3(1.0, 0.0, 0.0);\n"
"  int bm = int(uR[39]);\n"
"  if (bm == 1 || bm == 2) {\n"
"    int bs = int(uR[40]); ivec4 t = g_tex[bs < 3 ? bs : 0];\n"
"    vec3 p = vec3(t.rgb) / 127.5 - 1.0;\n"
"    if (bm == 1) { if (uR[41] != 0u) { float z2 = 1.0 - p.x * p.x - p.y * p.y; p.z = z2 > 0.0 ? sqrt(z2) : 0.0; } surf = p; }\n"
"    else surf_t = p;\n"
"  }\n"
"  vec3 n = nrm(qrot(q, surf)), tangent = nrm(qrot(q, surf_t));\n"
"  vec3 view = v_view, nv = nrm(view);\n"
"  vec3 dsum = vec3(0.0), ssum = vec3(0.0); float dalpha = 1.0, salpha = 1.0;\n"
"  int nl = int(uR[35]);\n"
"  for (int i = 0; i < 8; i++) {\n"
"    if (i >= nl) break;\n"
"    uint fl = uR[46 + i];\n"
"    bool directional = (fl & 1u) != 0u, two = (fl & 2u) != 0u, g0 = (fl & 4u) != 0u, g1 = (fl & 8u) != 0u;\n"
"    int num = int((fl >> 8u) & 7u);\n"
"    vec3 spec0 = uL[i * 6], spec1 = uL[i * 6 + 1], diff = uL[i * 6 + 2], amb = uL[i * 6 + 3], pos = uL[i * 6 + 4], spt = uL[i * 6 + 5];\n"
"    vec3 l = directional ? nrm(pos) : nrm(pos + view);\n"
"    vec3 h = nrm(nv + l);\n"
"    float ln = dot(l, n);\n"
"    float dp = two ? abs(ln) : max(ln, 0.0);\n"
"    float clampf = (uR[38] != 0u && ln <= 0.0) ? 0.0 : 1.0;\n"
"    float geo = 1.0;\n"
"    if (g0 || g1) { vec3 s = nv + l; float hh = dot(s, s); geo = hh == 0.0 ? 0.0 : min(dp / hh, 1.0); }\n"
"    float da = 1.0;\n"
"    if (((uR[42] >> uint(24 + num)) & 1u) == 0u && !directional) {\n"
"      float d = length(view + pos); float x = clamp(d * uLD[i].y + uLD[i].x, 0.0, 1.0); da = lutv(16 + num, x, true);\n"
"    }\n"
"    float spot = 1.0;\n"
"    if (((uR[42] >> uint(8 + num)) & 1u) == 0u && supported(2)) spot = lsample(2, 8 + num, n, h, nv, l, spt, tangent);\n"
"    float d0 = lut_enabled(0) ? lsample(0, 0, n, h, nv, l, spt, tangent) : 1.0;\n"
"    vec3 s0 = spec0 * (d0 * (g0 ? geo : 1.0));\n"
"    vec3 refl;\n"
"    refl.x = lut_enabled(6) ? lsample(6, 6, n, h, nv, l, spt, tangent) : 1.0;\n"
"    refl.y = lut_enabled(5) ? lsample(5, 5, n, h, nv, l, spt, tangent) : refl.x;\n"
"    refl.z = lut_enabled(4) ? lsample(4, 4, n, h, nv, l, spt, tangent) : refl.x;\n"
"    float d1 = lut_enabled(1) ? lsample(1, 1, n, h, nv, l, spt, tangent) : 1.0;\n"
"    vec3 s1 = spec1 * refl * (d1 * (g1 ? geo : 1.0));\n"
"    if (lut_enabled(3) && uR[37] != 0u) {\n"
"      float fr = lsample(3, 3, n, h, nv, l, spt, tangent);\n"
"      if ((uR[37] & 1u) != 0u) dalpha = fr;\n"
"      if ((uR[37] & 2u) != 0u) salpha = fr;\n"
"    }\n"
"    float att = da * spot;\n"
"    dsum += (diff * dp + amb) * att;\n"
"    ssum += (s0 + s1) * (clampf * att);\n"
"  }\n"
"  dsum += uL[48];\n"
"  g_lp = q8(vec4(dsum, dalpha)); g_ls = q8(vec4(ssum, salpha));\n"
"}\n"
/* ---- combinadores */
"ivec4 pick(uint s) {\n"
"  if (s == 0u) return g_prim; if (s == 1u) return g_lp; if (s == 2u) return g_ls;\n"
"  if (s == 3u) return g_tex[0]; if (s == 4u) return g_tex[1]; if (s == 5u) return g_tex[2];\n"
"  if (s == 13u) return g_buf; if (s == 14u) return g_konst; if (s == 15u) return g_prev;\n"
"  return ivec4(0);\n"
"}\n"
"ivec3 cmod(uint m, ivec4 s) {\n"
"  ivec3 v = m < 2u ? s.rgb : m < 4u ? ivec3(s.a) : m < 6u ? ivec3(s.r) : (m == 8u || m == 9u) ? ivec3(s.g) : (m == 12u || m == 13u) ? ivec3(s.b) : s.rgb;\n"
"  bool inv = m == 1u || m == 3u || m == 5u || m == 9u || m == 13u;\n"
"  return inv ? 255 - v : v;\n"
"}\n"
"int amod(uint m, ivec4 s) {\n"
"  int v = m < 2u ? s.a : m < 4u ? s.r : m < 6u ? s.g : s.b;\n"
"  return (m & 1u) != 0u ? 255 - v : v;\n"
"}\n"
"ivec3 comb3(uint op, ivec3 a, ivec3 b, ivec3 c) {\n"
"  if (op == 1u) return a * b / 255; if (op == 2u) return clamp(a + b, 0, 255);\n"
"  if (op == 3u) return clamp(a + b - 128, 0, 255); if (op == 4u) return (a * c + b * (255 - c)) / 255;\n"
"  if (op == 5u) return clamp(a - b, 0, 255); if (op == 8u) return clamp(a * b / 255 + c, 0, 255);\n"
"  if (op == 9u) return clamp(a + b, 0, 255) * c / 255; return a;\n"
"}\n"
"int comb1(uint op, int a, int b, int c) {\n"
"  if (op == 1u) return a * b / 255; if (op == 2u) return clamp(a + b, 0, 255);\n"
"  if (op == 3u) return clamp(a + b - 128, 0, 255); if (op == 4u) return (a * c + b * (255 - c)) / 255;\n"
"  if (op == 5u) return clamp(a - b, 0, 255); if (op == 8u) return clamp(a * b / 255 + c, 0, 255);\n"
"  if (op == 9u) return clamp(a + b, 0, 255) * c / 255; return a;\n"
"}\n"
"ivec4 unpack8(uint v) { return ivec4(int(v & 255u), int((v >> 8u) & 255u), int((v >> 16u) & 255u), int(v >> 24u)); }\n"
"bool cmp(uint f, int a, int r) {\n"
"  if (f == 0u) return false; if (f == 1u) return true; if (f == 2u) return a == r; if (f == 3u) return a != r;\n"
"  if (f == 4u) return a < r; if (f == 5u) return a <= r; if (f == 6u) return a > r; return a >= r;\n"
"}\n"
"/* bicúbico (Catmull-Rom em 9 leituras bilineares); com a textura reduzida (vários texels por pixel) volta ao normal */\n"
"vec4 tex_bc(sampler2D t, vec2 uv) {\n"
"  vec2 sz = vec2(textureSize(t, 0)); vec2 d = fwidth(uv * sz);\n"
"  if (max(d.x, d.y) > 1.0) return texture(t, uv);\n"
"  vec2 p = uv * sz - 0.5; vec2 tp = floor(p) + 0.5; vec2 f = p - floor(p);\n"
"  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
"  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);\n"
"  vec2 w12 = w1 + w2, o12 = w2 / w12;\n"
"  vec2 t0 = (tp - 1.0) / sz, t3 = (tp + 2.0) / sz, t12 = (tp + o12) / sz;\n"
"  vec4 r = vec4(0.0);\n"
"  r += texture(t, vec2(t0.x, t0.y)) * w0.x * w0.y + texture(t, vec2(t12.x, t0.y)) * w12.x * w0.y + texture(t, vec2(t3.x, t0.y)) * w3.x * w0.y;\n"
"  r += texture(t, vec2(t0.x, t12.y)) * w0.x * w12.y + texture(t, vec2(t12.x, t12.y)) * w12.x * w12.y + texture(t, vec2(t3.x, t12.y)) * w3.x * w12.y;\n"
"  r += texture(t, vec2(t0.x, t3.y)) * w0.x * w3.y + texture(t, vec2(t12.x, t3.y)) * w12.x * w3.y + texture(t, vec2(t3.x, t3.y)) * w3.x * w3.y;\n"
"  return clamp(r, 0.0, 1.0);\n"
"}\n"
"vec4 tex0(vec2 uv) { return (uR[59] & 1u) != 0u ? tex_bc(u_tex0, uv) : texture(u_tex0, uv); }\n"
"vec4 tex1(vec2 uv) { return (uR[59] & 2u) != 0u ? tex_bc(u_tex1, uv) : texture(u_tex1, uv); }\n"
"vec4 tex2(vec2 uv) { return (uR[59] & 4u) != 0u ? tex_bc(u_tex2, uv) : texture(u_tex2, uv); }\n"
"void main() {\n"
"  g_prim = q8(v_color);\n"
"  uint tcfg = uR[32];\n"
"  g_tex[0] = ivec4(0); g_tex[1] = ivec4(0); g_tex[2] = ivec4(0);\n"
"  if ((tcfg & 1u) != 0u) { vec2 t0 = v_tc01.xy; if (((uR[58] >> 28u) & 7u) == 3u) t0 /= v_tc2.z;\n"   /* textura projetiva: tc0 / tc0w */
"    g_tex[0] = q8(tex0(vec2(t0.x, uR[54] != 0u ? t0.y : 1.0 - t0.y))); }\n"
"  if ((tcfg & 2u) != 0u) g_tex[1] = q8(tex1(vec2(v_tc01.z, uR[55] != 0u ? v_tc01.w : 1.0 - v_tc01.w)));\n"
"  if ((tcfg & 4u) != 0u) { vec2 t = ((tcfg >> 13u) & 1u) != 0u ? v_tc01.zw : v_tc2.xy; g_tex[2] = q8(tex2(vec2(t.x, uR[56] != 0u ? t.y : 1.0 - t.y))); }\n"
"  g_lp = ivec4(0); g_ls = ivec4(0);\n"
"  if (uR[34] != 0u) lighting();\n"
"  ivec4 nbuf = unpack8(uR[30]); g_buf = nbuf; g_prev = ivec4(0);\n"
"  uint upd = uR[31];\n"
"  for (int st = 0; st < 6; st++) {\n"
"    uint src = uR[st * 5], opnd = uR[st * 5 + 1], cmb = uR[st * 5 + 2], scale = uR[st * 5 + 4];\n"
"    g_konst = unpack8(uR[st * 5 + 3]);\n"
"    uint cop = cmb & 15u, aop = (cmb >> 16u) & 15u;\n"
"    int cs = 1 << int(scale & 3u), as = 1 << int((scale >> 16u) & 3u);\n"
"    ivec3 c0 = cmod(opnd & 15u, pick(src & 15u)), c1 = cmod((opnd >> 4u) & 15u, pick((src >> 4u) & 15u)), c2 = cmod((opnd >> 8u) & 15u, pick((src >> 8u) & 15u));\n"
"    int a0 = amod((opnd >> 12u) & 7u, pick((src >> 16u) & 15u)), a1 = amod((opnd >> 16u) & 7u, pick((src >> 20u) & 15u)), a2 = amod((opnd >> 20u) & 7u, pick((src >> 24u) & 15u));\n"
"    ivec4 res;\n"
"    if (cop == 6u || cop == 7u) {\n"
"      int d = 0;\n"
"      for (int k = 0; k < 3; k++) d += int(trunc(float((c0[k] * 2 - 255) * (c1[k] * 2 - 255) + 128) / 256.0));\n"
"      d = clamp(d, 0, 255);\n"
"      res.rgb = ivec3(clamp(d * cs, 0, 255));\n"
"      res.a = cop == 7u ? d : clamp(comb1(aop, a0, a1, a2) * as, 0, 255);\n"
"    } else {\n"
"      res.rgb = clamp(comb3(cop, c0, c1, c2) * cs, 0, 255);\n"
"      res.a = clamp(comb1(aop, a0, a1, a2) * as, 0, 255);\n"
"    }\n"
"    g_buf = nbuf;\n"
"    if (st < 4) {\n"
"      if (((upd >> uint(8 + st)) & 1u) != 0u) nbuf.rgb = res.rgb;\n"
"      if (((upd >> uint(12 + st)) & 1u) != 0u) nbuf.a = res.a;\n"
"    }\n"
"    g_prev = res;\n"
"  }\n"
"  if ((uR[31] & 7u) == 5u) {\n"          /* neblina (Azahar: WriteFog) */
"    float fz = ((uR[31] >> 16u) & 1u) != 0u ? 1.0 - gl_FragCoord.z : gl_FragCoord.z;\n"
"    float fi = fz * 128.0, fl = clamp(floor(fi), 0.0, 127.0);\n"
"    vec2 fe = uFog[int(fl)];\n"
"    float ff = clamp(fe.x + fe.y * (fi - fl), 0.0, 1.0);\n"
"    vec3 fc = vec3(float(uR[57] & 255u), float((uR[57] >> 8u) & 255u), float((uR[57] >> 16u) & 255u));\n"
"    g_prev.rgb = ivec3(round(mix(fc, vec3(g_prev.rgb), ff)));\n"
"  }\n"
"  uint at = uR[33];\n"
"  if ((at & 1u) != 0u && !cmp((at >> 4u) & 7u, g_prev.a, int((at >> 8u) & 255u))) discard;\n"
"  o_color = vec4(g_prev) / 255.0;\n"
"}\n";

static void morton_init(void);
static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096]; glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[gl] erro no shader de %s:\n%s\n", type == GL_VERTEX_SHADER ? "vértice" : "fragmento", log);
        return 0;
    }
    return s;
}

static int gl_setup(void) {
#define GLL(ret, name, args) name = (ret (GLAPIENTRY *) args)gpu_gl_proc(#name); \
    if (!name) { fprintf(stderr, "[gl] função %s não encontrada\n", #name); return 0; }
    GL_FUNCS(GLL)
#undef GLL
    fprintf(stderr, "[gl] %s | %s | %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
    GLuint vs = compile(GL_VERTEX_SHADER, k_vs), fs = compile(GL_FRAGMENT_SHADER, k_fs);
    if (!vs || !fs) return 0;
    g_prog = glCreateProgram();
    glAttachShader(g_prog, vs); glAttachShader(g_prog, fs);
    glLinkProgram(g_prog);
    GLint ok = 0; glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[4096]; glGetProgramInfoLog(g_prog, sizeof log, NULL, log); fprintf(stderr, "[gl] erro ao ligar o programa:\n%s\n", log); return 0; }
    glUseProgram(g_prog);
    glUniform1i(glGetUniformLocation(g_prog, "u_tex0"), 0);
    glUniform1i(glGetUniformLocation(g_prog, "u_tex1"), 1);
    glUniform1i(glGetUniformLocation(g_prog, "u_tex2"), 2);
    glUniform1i(glGetUniformLocation(g_prog, "u_lut"), 3);
    g_loc_R = glGetUniformLocation(g_prog, "uR");
    g_loc_L = glGetUniformLocation(g_prog, "uL");
    g_loc_LD = glGetUniformLocation(g_prog, "uLD");
    g_loc_fog = glGetUniformLocation(g_prog, "uFog");
    glGenVertexArrays(1, &g_vao); glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo); glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    static const struct { int n, off; } attrs[6] = { { 4, 0 }, { 4, 4 }, { 4, 8 }, { 3, 12 }, { 4, 15 }, { 3, 19 } };
    for (int i = 0; i < 6; i++) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, attrs[i].n, GL_FLOAT, GL_FALSE, VSTRIDE * sizeof(float), (const void *)(intptr_t)(attrs[i].off * sizeof(float)));
    }
    glGenSamplers(3, g_samplers);
    for (int i = 0; i < 3; i++) glBindSampler(i, g_samplers[i]);
    glGenTextures(1, &g_lut_tex);
    glActiveTexture(GL_TEXTURE0 + 3); glBindTexture(GL_TEXTURE_2D, g_lut_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, 256, 24, 0, GL_RG, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenTextures(1, &g_black_tex);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_black_tex);
    static const uint8_t black[4] = { 0, 0, 0, 0 };
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
    glDisable(GL_DITHER); glDisable(GL_CULL_FACE); glEnable(GL_DEPTH_CLAMP);
    glPixelStorei(GL_PACK_ALIGNMENT, 1); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    morton_init();
    g_render_name = "OpenGL";
    return 1;
}

/* ------------------------------------------------------------ superfícies (framebuffers) na GPU
 * Cada buffer de cor ou de profundidade que o jogo usa como destino vira uma textura na GPU e fica lá entre
 * listas e quadros. A memória do jogo só é atualizada quando alguém precisa ler aquela faixa (transferência
 * de tela, cópia, DMA, uso como textura) e só é recarregada na GPU quando algo escreve nela pela CPU.
 * Preenchimentos (limpeza de tela) que cobrem uma superfície inteira viram glClear direto na GPU. */
typedef struct { uint32_t addr, size; int w, h, fmt, depth, sc; GLuint tex; int valid, dirty; unsigned long used; unsigned ver, conv_ver; GLuint conv; } Surf;
/* resolução interna: as superfícies na GPU têm sc vezes o tamanho do 3DS (1 = nativa). Ao ler/escrever
 * a memória do jogo, a imagem passa por uma textura do tamanho nativo (redução/ampliação por blit). */
int g_sc = 1;                      /* escala em uso */
volatile int g_res_req;            /* pedido de troca (tecla F8 / RESOLUCAO=n); 0 = nenhum */
#define NSURF 48
static Surf g_surf[NSURF];
typedef struct { GLuint c, d, fbo; } FboE;
#define NFBO 64
static FboE g_fbo[NFBO]; static int g_nfbo;
static unsigned long g_surf_clock;
static uint8_t *g_pix; static size_t g_pix_cap;
static Surf *g_cur_c, *g_cur_d;          /* destino do desenho atual */
/* localizador de erros do OpenGL: depois que uma lista termina com erro, as próximas checam a cada etapa e dizem
 * onde o erro acontece (log "[gl] erro ... em ...") */
static int g_replay; static int g_glchk; static int g_glchk_n;
static void glchk(const char *where) {
    GLenum e = glGetError();
    if (e && g_glchk_n < 40) { g_glchk_n++; extern long g_gsp_frames;
        fprintf(stderr, "[gl] erro %04x em %s (60 fps: %s, quadro %ld)\n", e, where, g_replay ? "refazendo" : "normal", g_gsp_frames); }
}
#define GLCHK(w) do { if (g_glchk) glchk(w); } while (0)
/* 60 fps (pica_draw.c grava, aqui se desenha de novo): enquanto g_replay, os desenhos vão para cópias das
 * superfícies (g_shadow, mesma posição de g_surf), sem tocar no que o jogo desenhou nem na memória dele */
int g_fps60 = -1;                        /* modo 60 fps (FPS=60); -1 = ainda não leu */
volatile int g_fps60_req = -1;           /* troca pedida pela tecla F6 */
static int g_replay_suspect; static unsigned g_replay_id;
static Surf g_shadow[NSURF]; static unsigned g_shadow_stamp[NSURF];
static unsigned g_shadow_drawn[NSURF];   /* desenhada (não só limpa) neste quadro intermediário */
extern int g_irec;
int interp_replay(float t, int (*op_cb)(int kind, const uint32_t p[8]));
void interp_rec_op(int kind, const uint32_t p[8]);
void interp_frame_swap(void); void interp_reset(void);

static void pix_reserve(int w, int h) {
    size_t n = (size_t)w * h * 4;
    if (n > g_pix_cap) { g_pix_cap = n; g_pix = realloc(g_pix, n); }
}
static int cbpp(int fmt) { return fmt == 0 ? 4 : fmt == 1 ? 3 : 2; }
static int dbpp(int fmt) { return fmt == 0 ? 2 : fmt == 2 ? 3 : 4; }
static int overlap(uint32_t a, uint32_t al, uint32_t b, uint32_t bl) { return a < b + bl && b < a + al; }
static void tex_invalidate_range(uint32_t a, uint32_t len);
static GLuint fbo_get(GLuint c, GLuint d);

/* posição de cada pixel de um bloco 8x8 (ordem Morton), para converter um bloco inteiro de uma vez */
static uint8_t k_mx[64], k_my[64];
static void morton_init(void) {
    for (int i = 0; i < 64; i++) {
        int x = 0, y = 0;
        for (int b = 0; b < 3; b++) { x |= ((i >> (2 * b)) & 1) << b; y |= ((i >> (2 * b + 1)) & 1) << b; }
        k_mx[i] = (uint8_t)x; k_my[i] = (uint8_t)y;
    }
}
static Fb surf_fb(const Surf *s) {
    Fb f; memset(&f, 0, sizeof f);
    f.w = s->w; f.h = s->h;
    if (s->depth) { f.depth = s->addr; f.dfmt = s->fmt; } else { f.color = s->addr; f.cfmt = s->fmt; }
    return f;
}
/* memória -> buffer linear (linha 0 = y 0, como a GPU usa) */
static int g_nofast = -1;
static void mem_to_linear(const Surf *s, uint32_t *out) {
    int w = s->w, h = s->h;
    if (g_nofast < 0) g_nofast = ENVC("GL_SEM_RAPIDO") != NULL;
    if (!g_nofast && ((!s->depth && s->fmt == 0) || (s->depth && s->fmt == 3))) {      /* RGBA8 / D24S8: blocos direto */
        const uint8_t *m = g_mem + s->addr;
        for (int ty = 0; ty < h / 8; ty++) for (int tx = 0; tx < w / 8; tx++) {
            const uint32_t *blk = (const uint32_t *)(m + ((size_t)ty * (w / 8) + tx) * 256);
            for (int i = 0; i < 64; i++) {
                int x = tx * 8 + k_mx[i], y = h - 1 - (ty * 8 + k_my[i]);
                uint32_t v = blk[i];
                out[(size_t)y * w + x] = s->depth ? (v << 8) | (v >> 24) : __builtin_bswap32(v);
            }
        }
        return;
    }
    Fb f = surf_fb(s);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint32_t *o = &out[(size_t)y * w + x];
        if (!s->depth) fb_read(&f, x, y, (uint8_t *)o);
        else {
            uint32_t d = depth_read(&f, x, y);
            uint32_t d24 = s->fmt == 0 ? (d << 8 | d >> 8) : d;
            *o = d24 << 8 | (s->fmt == 3 ? stencil_read(&f, x, y) : 0);
        }
    }
}
static void linear_to_mem(const Surf *s, const uint32_t *in) {
    int w = s->w, h = s->h;
    if (!g_nofast && ((!s->depth && s->fmt == 0) || (s->depth && s->fmt == 3))) {
        uint8_t *m = g_mem + s->addr;
        for (int ty = 0; ty < h / 8; ty++) for (int tx = 0; tx < w / 8; tx++) {
            uint32_t *blk = (uint32_t *)(m + ((size_t)ty * (w / 8) + tx) * 256);
            for (int i = 0; i < 64; i++) {
                int x = tx * 8 + k_mx[i], y = h - 1 - (ty * 8 + k_my[i]);
                uint32_t v = in[(size_t)y * w + x];
                blk[i] = s->depth ? (v >> 8) | (v << 24) : __builtin_bswap32(v);
            }
        }
        return;
    }
    Fb f = surf_fb(s);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint32_t v = in[(size_t)y * w + x];
        if (!s->depth) fb_write(&f, x, y, (const uint8_t *)&v, 0xF);
        else {
            uint32_t d24 = v >> 8;
            depth_write(&f, x, y, s->fmt == 0 ? d24 >> 8 : d24);
            if (s->fmt == 3) stencil_write(&f, x, y, (uint8_t)v);
        }
    }
}

/* textura temporária do tamanho nativo (para reduzir/ampliar as superfícies em escala) */
static GLuint native_tmp(int w, int h, int depth) {
    static struct { int w, h, depth; GLuint tex; } c[8]; static int n, next;
    for (int i = 0; i < n; i++) if (c[i].w == w && c[i].h == h && c[i].depth == depth) return c[i].tex;
    int i = n < 8 ? n++ : (next++ % 8);
    if (c[i].tex) {
        for (int k = 0; k < g_nfbo; k++) if (g_fbo[k].c == c[i].tex || g_fbo[k].d == c[i].tex) { glDeleteFramebuffers(1, &g_fbo[k].fbo); g_fbo[k--] = g_fbo[--g_nfbo]; }
        glDeleteTextures(1, &c[i].tex);
    }
    c[i].w = w; c[i].h = h; c[i].depth = depth;
    glGenTextures(1, &c[i].tex);
    glActiveTexture(GL_TEXTURE0 + 4); glBindTexture(GL_TEXTURE_2D, c[i].tex);
    if (depth) glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return c[i].tex;
}
/* copia (com escala) entre superfície e textura nativa; dir 0: superfície -> nativa, 1: nativa -> superfície */
static void scale_blit(Surf *s, GLuint nat, int dir) {
    GLuint fs = s->depth ? fbo_get(0, s->tex) : fbo_get(s->tex, 0), fn = s->depth ? fbo_get(0, nat) : fbo_get(nat, 0);
    GLbitfield m = s->depth ? (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT) : GL_COLOR_BUFFER_BIT;
    glDisable(GL_SCISSOR_TEST);
    int W = s->w * s->sc, H = s->h * s->sc;
    if (!dir) { glBindFramebuffer(GL_READ_FRAMEBUFFER, fs); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fn);
                glBlitFramebuffer(0, 0, W, H, 0, 0, s->w, s->h, m, GL_NEAREST); }
    else { glBindFramebuffer(GL_READ_FRAMEBUFFER, fn); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fs);
           glBlitFramebuffer(0, 0, s->w, s->h, 0, 0, W, H, m, GL_NEAREST); }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    GLCHK(dir ? "ampliação da superfície" : "redução da superfície");
}

/* GPU -> memória do jogo (a superfície continua válida na GPU) */
static int g_dbg = -1;
static int dbg_on(void) { extern long g_gsp_frames; if (g_dbg < 0) g_dbg = ENVC("GL_DEBUG_FRAME") ? atoi(ENVC("GL_DEBUG_FRAME")) : 1 << 30; return g_gsp_frames >= g_dbg && g_gsp_frames < g_dbg + 8; }
static void surf_writeback(Surf *s) {
    if (!s->tex || !s->valid || !s->dirty) return;
    uint64_t t0 = prof_now();
    if (dbg_on()) fprintf(stderr, "[gldbg] devolve %s %08x %dx%d\n", s->depth ? "prof" : "cor", s->addr, s->w, s->h);
    pix_reserve(s->w, s->h);
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    GLuint src = s->tex;
    if (s->sc > 1) { src = native_tmp(s->w, s->h, s->depth); scale_blit(s, src, 0); }
    glBindFramebuffer(GL_FRAMEBUFFER, s->depth ? fbo_get(0, src) : fbo_get(src, 0));
    if (s->depth) glReadPixels(0, 0, s->w, s->h, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, g_pix);
    else glReadPixels(0, 0, s->w, s->h, GL_RGBA, GL_UNSIGNED_BYTE, g_pix);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev);
    GLCHK(s->depth ? "devolução da profundidade" : "devolução da cor");
    plat_touch(g_mem + s->addr, s->size);
    linear_to_mem(s, (const uint32_t *)g_pix);
    tex_invalidate_range(s->addr, s->size);
    s->dirty = 0;
    g_prof_ns[3] += prof_now() - t0;
}
/* memória -> GPU */
static void surf_upload(Surf *s) {
    uint64_t t0 = prof_now();
    if (dbg_on()) fprintf(stderr, "[gldbg] carrega %s %08x %dx%d\n", s->depth ? "prof" : "cor", s->addr, s->w, s->h);
    pix_reserve(s->w, s->h);
    plat_touch(g_mem + s->addr, s->size);
    mem_to_linear(s, (uint32_t *)g_pix);
    GLuint dst = s->sc > 1 ? native_tmp(s->w, s->h, s->depth) : s->tex;
    glActiveTexture(GL_TEXTURE0 + 4);
    glBindTexture(GL_TEXTURE_2D, dst);
    if (s->depth) glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s->w, s->h, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, g_pix);
    else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s->w, s->h, GL_RGBA, GL_UNSIGNED_BYTE, g_pix);
    if (s->sc > 1) { GLint prev = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev); scale_blit(s, dst, 1); glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev); }
    s->valid = 1; s->dirty = 0; s->ver++;
    GLCHK(s->depth ? "carga da profundidade" : "carga da cor");
    g_prof_ns[4] += prof_now() - t0;
}
/* uma textura vai ser apagada: esquece os framebuffers ligados a ela. O OpenGL reaproveita o número da textura
 * apagada, e um framebuffer antigo no cache continuaria preso à textura morta (desenhos sumindo para ela) */
static void fbo_forget(GLuint t) {
    if (!t) return;
    for (int i = 0; i < g_nfbo; i++)
        if (g_fbo[i].c == t || g_fbo[i].d == t) { glDeleteFramebuffers(1, &g_fbo[i].fbo); g_fbo[i--] = g_fbo[--g_nfbo]; }
}
static void surf_free(Surf *s) {
    fbo_forget(s->tex);
    fbo_forget(s->conv);     /* (a cópia convertida da profundidade também é destino de desenho) */
    if (s->tex) glDeleteTextures(1, &s->tex);
    if (s->conv) glDeleteTextures(1, &s->conv);
    if (g_cur_c == s) g_cur_c = NULL;
    if (g_cur_d == s) g_cur_d = NULL;
    memset(s, 0, sizeof *s);
}
static Surf *surf_get(uint32_t addr, int w, int h, int fmt, int depth) {
    uint32_t size = (uint32_t)(w * h * (depth ? dbpp(fmt) : cbpp(fmt)));
    Surf *hit = NULL;
    for (int i = 0; i < NSURF; i++) {
        Surf *s = &g_surf[i];
        if (!s->tex) continue;
        if (s->addr == addr && s->w == w && s->h == h && s->fmt == fmt && s->depth == depth) { hit = s; continue; }
        if (overlap(s->addr, s->size, addr, size)) { surf_writeback(s); surf_free(s); }   /* outra forma da mesma memória */
    }
    if (!hit) {
        for (int i = 0; i < NSURF && !hit; i++) if (!g_surf[i].tex) hit = &g_surf[i];
        if (!hit) { hit = &g_surf[0]; for (int i = 1; i < NSURF; i++) if (g_surf[i].used < hit->used) hit = &g_surf[i]; surf_writeback(hit); surf_free(hit); }
        hit->addr = addr; hit->size = size; hit->w = w; hit->h = h; hit->fmt = fmt; hit->depth = depth; hit->sc = g_sc;
        { static int nl; if (ENVC("GL_SURF_LOG") && nl++ < 200) fprintf(stderr, "[surf] nova %s %08x %dx%d fmt %d\n", depth ? "prof" : "cor", addr, w, h, fmt); }
        glGenTextures(1, &hit->tex);
        glActiveTexture(GL_TEXTURE0 + 4);
        glBindTexture(GL_TEXTURE_2D, hit->tex);
        if (depth) glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w * g_sc, h * g_sc, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
        else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w * g_sc, h * g_sc, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        hit->valid = 0;
    }
    if (!hit->valid) surf_upload(hit);
    hit->used = ++g_surf_clock;
    return hit;
}
static GLuint fbo_get(GLuint c, GLuint d) {
    for (int i = 0; i < g_nfbo; i++) if (g_fbo[i].c == c && g_fbo[i].d == d) return g_fbo[i].fbo;
    if (g_nfbo == NFBO) { glDeleteFramebuffers(1, &g_fbo[0].fbo); g_fbo[0] = g_fbo[--g_nfbo]; }
    FboE *e = &g_fbo[g_nfbo++];
    e->c = c; e->d = d;
    glGenFramebuffers(1, &e->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, e->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, d, 0);
    if (!c) { glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE); }   /* só profundidade */
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) fprintf(stderr, "[gl] framebuffer incompleto (%04x)\n", st);
    return e->fbo;
}
/* cópia de trabalho de uma superfície para o quadro intermediário; `copy`: começa com o conteúdo atual */
static Surf *shadow_of(Surf *s, int copy) {
    int i = (int)(s - g_surf); Surf *h = &g_shadow[i];
    if (h->tex && (h->addr != s->addr || h->w != s->w || h->h != s->h || h->fmt != s->fmt || h->depth != s->depth || h->sc != s->sc)) {
        surf_free(h); g_shadow_stamp[i] = 0;
    }
    if (!h->tex) {
        h->addr = s->addr; h->size = s->size; h->w = s->w; h->h = s->h; h->fmt = s->fmt; h->depth = s->depth; h->sc = s->sc;
        glGenTextures(1, &h->tex);
        glActiveTexture(GL_TEXTURE0 + 4); glBindTexture(GL_TEXTURE_2D, h->tex);
        if (h->depth) glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, h->w * h->sc, h->h * h->sc, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
        else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, h->w * h->sc, h->h * h->sc, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    if (g_shadow_stamp[i] != g_replay_id) {
        if (copy && s->valid) {
            GLbitfield m = s->depth ? (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT) : GL_COLOR_BUFFER_BIT;
            int W = s->w * s->sc, H = s->h * s->sc;
            glDisable(GL_SCISSOR_TEST);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s->depth ? fbo_get(0, s->tex) : fbo_get(s->tex, 0));
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, h->depth ? fbo_get(0, h->tex) : fbo_get(h->tex, 0));
            glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, m, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            GLCHK(s->depth ? "cópia de trabalho (profundidade)" : "cópia de trabalho (cor)");
        }
        h->valid = 1; h->dirty = 0; h->ver++; g_shadow_stamp[i] = g_replay_id;
    }
    return h;
}
/* limpeza de uma superfície inteira na GPU (preenchimento do GSP) */
static int g_os_epoch; static uint32_t g_os_store_fb;    /* bordas guardadas para o alargamento (ver pica_gl_draw) */
static void surf_clear(Surf *s, uint32_t value) {
    if (!s->depth && s->addr == g_os_store_fb) g_os_epoch++;   /* quadro novo nesse destino */
    GLuint fbo = s->depth ? fbo_get(0, s->tex) : fbo_get(s->tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glDisable(GL_SCISSOR_TEST);
    if (!s->depth) {
        float r, g, b, a;
        if (s->fmt == 0) { r = (value >> 24) / 255.0f; g = ((value >> 16) & 0xFF) / 255.0f; b = ((value >> 8) & 0xFF) / 255.0f; a = (value & 0xFF) / 255.0f; }
        else { r = ((value >> 16) & 0xFF) / 255.0f; g = ((value >> 8) & 0xFF) / 255.0f; b = (value & 0xFF) / 255.0f; a = 1; }
        glColorMask(1, 1, 1, 1);
        glClearColor(r, g, b, a);
        glClear(GL_COLOR_BUFFER_BIT);
    } else {
        double d = s->fmt == 0 ? (value & 0xFFFF) / 65535.0 : (value & 0xFFFFFF) / 16777215.0;
        glDepthMask(1); glStencilMask(0xFF);
        glClearDepth(d); glClearStencil(s->fmt == 3 ? (int)(value >> 24) : 0);
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    s->valid = 1; s->dirty = 1; s->ver++;
    GLCHK(s->depth ? "limpeza da profundidade" : "limpeza da cor");
}
/* prepara o destino do desenho */
static void surf_bind(const Fb *f) {
    g_cur_c = surf_get(f->color, f->w, f->h, f->cfmt, 0);
    g_cur_d = f->depth ? surf_get(f->depth, f->w, f->h, f->dfmt, 1) : NULL;
    if (g_cur_c && g_cur_d && !g_cur_c->tex) g_cur_c = surf_get(f->color, f->w, f->h, f->cfmt, 0);   /* (pode ter sido liberada por sobreposição) */
    if (g_replay) {
        g_shadow_drawn[g_cur_c - g_surf] = g_replay_id; g_cur_c = shadow_of(g_cur_c, 1);
        if (g_cur_d) { g_shadow_drawn[g_cur_d - g_surf] = g_replay_id; g_cur_d = shadow_of(g_cur_d, 1); }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_get(g_cur_c->tex, g_cur_d ? g_cur_d->tex : 0));
    glViewport(0, 0, f->w * g_cur_c->sc, f->h * g_cur_c->sc);
}

static void pend_resolve_range(uint32_t a, uint32_t len);
/* ---- ganchos chamados pelo GSP (gsp.c) antes/depois de ler ou escrever memória pela CPU */
static int ctx_enter(void) {
    if (g_in_list) return 1;
    if (g_gl_state <= 0) return 0;
    return gpu_gl_begin() ? 2 : 0;
}
static void ctx_leave(int c) { if (c == 2) gpu_gl_end(); }
static int any_overlap(uint32_t a, uint32_t len, int need_dirty) {
    for (int i = 0; i < NSURF; i++)
        if (g_surf[i].tex && overlap(g_surf[i].addr, g_surf[i].size, a, len) && (!need_dirty || (g_surf[i].valid && g_surf[i].dirty))) return 1;
    return 0;
}
/* a CPU vai ler [a, a+len): o que a GPU desenhou ali precisa estar na memória */
void pica_gpu_flush(uint32_t a, uint32_t len) {
    if (g_vk) { pica_vk_flush(a, len); return; }
    if (dbg_on()) fprintf(stderr, "[gldbg] CPU vai ler %08x +%x\n", a, len);
    pend_resolve_range(a, len);
    if (!any_overlap(a, len, 1)) return;
    int c = ctx_enter(); if (!c) return;
    for (int i = 0; i < NSURF; i++) if (g_surf[i].tex && overlap(g_surf[i].addr, g_surf[i].size, a, len)) surf_writeback(&g_surf[i]);
    ctx_leave(c);
}
/* a CPU vai escrever em [a, a+len): o que só existe na GPU vai para a memória antes (se a escrita não cobrir
 * tudo); depois da escrita, chame pica_gpu_written para a GPU recarregar */
void pica_gpu_will_write(uint32_t a, uint32_t len) {
    if (g_vk) { pica_vk_will_write(a, len); return; }
    pend_resolve_range(a, len);
    if (!any_overlap(a, len, 1)) return;
    int c = ctx_enter(); if (!c) return;
    for (int i = 0; i < NSURF; i++) {
        Surf *s = &g_surf[i];
        if (s->tex && overlap(s->addr, s->size, a, len) && !(a <= s->addr && a + len >= s->addr + s->size)) surf_writeback(s);
    }
    ctx_leave(c);
}
void pica_gpu_written(uint32_t a, uint32_t len) {
    if (g_vk) { pica_vk_written(a, len); return; }
    if (dbg_on()) fprintf(stderr, "[gldbg] CPU escreveu %08x +%x\n", a, len);
    for (int i = 0; i < NSURF; i++) {
        Surf *s = &g_surf[i];
        if (s->tex && overlap(s->addr, s->size, a, len)) { s->valid = 0; s->dirty = 0; }
    }
    tex_invalidate_range(a, len);
}
/* preenchimento de memória do GSP: se cobre exatamente uma superfície, limpa direto na GPU */
int pica_gpu_fill(uint32_t start, uint32_t end, uint32_t value, int width) {
    if (g_vk) return pica_vk_fill(start, end, value, width);
    static int off = -1; if (off < 0) off = ENVC("GL_SEM_LIMPEZA") != NULL;
    if (off) return 0;
    Surf *s = NULL;
    for (int i = 0; i < NSURF; i++)
        if (g_surf[i].tex && g_surf[i].addr == start && g_surf[i].addr + g_surf[i].size == end) { s = &g_surf[i]; break; }
    if (!s) return 0;
    int ok = (!s->depth && s->fmt == 0 && width >= 2) || (!s->depth && s->fmt == 1 && width == 1) ||
             (s->depth && s->fmt == 3 && width >= 2) || (s->depth && s->fmt == 2 && width == 1) || (s->depth && s->fmt == 0 && width == 0);
    if (!ok) return 0;
    int c = ctx_enter(); if (!c) return 0;
    surf_clear(s, value);
    if (g_irec) { uint32_t p[8] = { start, end, value, (uint32_t)width }; interp_rec_op(1, p); }   /* 60 fps */
    tex_invalidate_range(s->addr, s->size);
    ctx_leave(c);
    return 1;
}

/* ------------------------------------------------------------ texturas (decodificadas na CPU, guardadas na GPU)
 * Cada textura é conferida por um hash dos bytes uma vez por lista de comandos: se o jogo não mudou o
 * conteúdo, a cópia na GPU continua valendo entre quadros. */
typedef struct { uint32_t addr, fmt, size; int w, h, levels; uint64_t hash; GLuint tex; int gen; unsigned checked; unsigned long used; } GTex;
#define NGT 768
static GTex g_gt[NGT]; static int g_ngt; static unsigned long g_use_clock;
static uint8_t *g_tpix; static size_t g_tpix_cap;
static const int k_tex_bits[14] = { 32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8 };

static uint64_t hash_mem(uint32_t addr, uint32_t len) {
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *p = g_mem + addr;
    uint32_t i = 0;
    for (; i + 8 <= len; i += 8) { uint64_t w; memcpy(&w, p + i, 8); h = (h ^ w) * 0x100000001b3ull; h ^= h >> 29; }
    for (; i < len; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
static void tex_invalidate_range(uint32_t a, uint32_t len) {
    for (int i = 0; i < g_ngt; i++) if (overlap(g_gt[i].addr, g_gt[i].size, a, len)) g_gt[i].checked = 0;
}
/* levels: nº de níveis de mipmap (0x84 bits 16-19 + 1); os níveis ficam em sequência na memória, cada um com
 * metade da largura e da altura do anterior (mínimo 8x8, o tamanho do bloco da PICA) */
/* texturas personalizadas (texpack.c) */
int texpack_init(void); int texpack_ativo(void); uint64_t texpack_hash(const uint8_t *src, int w, int h, int fmt);
int texpack_get(uint64_t hash, const uint8_t **px, int *pw, int *ph); extern volatile int g_texpack_gen;
static GLuint tex_get(uint32_t addr, uint32_t fmt, int w, int h, int levels) {
    uint32_t size = 0;
    for (int l = 0; l < levels; l++) {
        int lw = w >> l, lh = h >> l;
        if (lw < 8 || lh < 8) { levels = l; break; }
        size += (uint32_t)((size_t)lw * lh * k_tex_bits[fmt] / 8);
    }
    if (levels < 1) { levels = 1; size = (uint32_t)((size_t)w * h * k_tex_bits[fmt] / 8); }
    GTex *e = NULL;
    for (int i = 0; i < g_ngt; i++)
        if (g_gt[i].addr == addr && g_gt[i].fmt == fmt && g_gt[i].w == w && g_gt[i].h == h && g_gt[i].levels == levels) { e = &g_gt[i]; break; }
    if (e && e->checked == g_list_serial) { e->used = ++g_use_clock; return e->tex; }
    pend_resolve_range(addr, size);                  /* (textura lida de uma tela ainda sendo copiada da GPU) */
    uint64_t t0 = prof_now();
    uint64_t hs = hash_mem(addr, size);
    if (e && e->hash == hs && e->gen == g_texpack_gen) { e->checked = g_list_serial; e->used = ++g_use_clock; g_prof_ns[5] += prof_now() - t0; return e->tex; }
    const uint8_t *cpx = NULL; int cw = 0, ch = 0, cst = 0;
    if (texpack_ativo()) cst = texpack_get(texpack_hash(g_mem + addr, w, h, (int)fmt), &cpx, &cw, &ch);
    if (!e) {
        if (g_ngt < NGT) e = &g_gt[g_ngt++];
        else { e = &g_gt[0]; for (int i = 1; i < NGT; i++) if (g_gt[i].used < e->used) e = &g_gt[i]; }
        if (!e->tex) glGenTextures(1, &e->tex);
        e->addr = addr; e->fmt = fmt; e->w = w; e->h = h; e->size = size; e->levels = levels;
    }
    e->gen = g_texpack_gen;
    if (cst == 1) {                                    /* substituta do pacote: maior, mesma imagem (coordenadas normalizadas) */
        glBindTexture(GL_TEXTURE_2D, e->tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cw, ch, 0, GL_RGBA, GL_UNSIGNED_BYTE, cpx);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels > 1 ? 1000 : 0);
        if (levels > 1) glGenerateMipmap(GL_TEXTURE_2D);
        GLCHK("textura personalizada");
        e->hash = hs; e->checked = g_list_serial; e->used = ++g_use_clock;
        g_prof_ns[5] += prof_now() - t0;
        return e->tex;
    }
    size_t n = (size_t)w * h * 4;
    if (n > g_tpix_cap) { g_tpix_cap = n; g_tpix = realloc(g_tpix, n); }
    glBindTexture(GL_TEXTURE_2D, e->tex);
    uint32_t la = addr;
    for (int l = 0; l < levels; l++) {
        int lw = w >> l, lh = h >> l;
        for (int y = 0; y < lh; y++) for (int x = 0; x < lw; x++) texel(la, fmt, lw, x, y, g_tpix + ((size_t)y * lw + x) * 4);
        glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_tpix);
        GLCHK("carga de textura");
        { const char *da = ENVC("GL_DUMPADDR"); char hx[16]; snprintf(hx, sizeof hx, "%08x", addr);
          if (da && l == 0 && strstr(da, hx)) {                 /* depuração: salva a textura decodificada (cor | alfa) */
              char pth[64]; snprintf(pth, sizeof pth, "tx_%08x_f%u.ppm", addr, fmt);
              FILE *o = fopen(pth, "wb");
              if (o) { fprintf(o, "P6\n%d %d\n255\n", lw * 2, lh);
                       for (int y = 0; y < lh; y++) for (int x = 0; x < 2 * lw; x++) { const uint8_t *q = g_tpix + ((size_t)y * lw + (x % lw)) * 4;
                           uint8_t c[3] = { q[0], q[1], q[2] }; if (x >= lw) c[0] = c[1] = c[2] = q[3]; fwrite(c, 1, 3, o); }
                       fclose(o); } } }
        la += (uint32_t)((size_t)lw * lh * k_tex_bits[fmt] / 8);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
    e->hash = cst < 0 ? hs ^ 1 : hs;                   /* (substituta ainda sendo lida: confere de novo na próxima lista) */
    e->checked = g_list_serial; e->used = ++g_use_clock;
    g_prof_ns[5] += prof_now() - t0;
    return e->tex;
}

/* ------------------------------------------------------------ estado fixo (profundidade, estêncil, blending...) */
static const GLenum k_func[8] = { GL_NEVER, GL_NEVER + 7 /* ALWAYS */, GL_NEVER + 2 /* EQUAL */, GL_NEVER + 5 /* NOTEQUAL */,
                                  GL_NEVER + 1 /* LESS */, GL_NEVER + 3 /* LEQUAL */, GL_NEVER + 4 /* GREATER */, GL_NEVER + 6 /* GEQUAL */ };
static const GLenum k_sop[8] = { GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP };
static const GLenum k_bfac[16] = { GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
                                   GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA,
                                   GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR, GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA,
                                   GL_SRC_ALPHA_SATURATE, GL_SRC_ALPHA_SATURATE };
static const GLenum k_beq[8] = { GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX, GL_FUNC_ADD, GL_FUNC_ADD, GL_FUNC_ADD };
static const GLenum k_logic[16] = { 0x1500, 0x1501, 0x1502, 0x1503, 0x150F, 0x150C, 0x1505, 0x150A,
                                    0x150E, 0x1507, 0x1508, 0x1506, 0x1509, 0x1504, 0x150B, 0x150D };
static const GLenum k_wrap[8] = { GL_CLAMP_TO_EDGE, GL_CLAMP_TO_BORDER, GL_REPEAT, GL_MIRRORED_REPEAT,
                                  GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE };

static void apply_state(const Fb *f) {
    uint32_t dcm = R(0x107);
    /* 0x113 / 0x115: a escrita de cor / profundidade+estêncil no framebuffer pode estar bloqueada (como no
     * Citra/Azahar: o teste continua ligado, só a escrita some). A abertura das batalhas desenha o fundo
     * com escrita de profundidade ligada em 0x107 mas bloqueada aqui; sem isso o fundo "tapava" a treinadora. */
    int allow_ds = (R(0x115) & 3) != 0, allow_c = (R(0x113) & 0xF) != 0;
    int dtest = dcm & 1, dwrite = (dcm >> 12) & 1;
    if (f->depth && (dtest || dwrite)) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(dtest ? k_func[(dcm >> 4) & 7] : k_func[1]);
        glDepthMask(dwrite && allow_ds ? GL_TRUE : GL_FALSE);
    } else glDisable(GL_DEPTH_TEST);
    { extern int g_dbg_modo; if (g_dbg_modo & 2) glDepthFunc(GL_NEVER + 7); }
    uint32_t st = R(0x105), sop = R(0x106);
    if ((st & 1) && f->depth && f->dfmt == 3) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(k_func[(st >> 4) & 7], (st >> 16) & 0xFF, (st >> 24) & 0xFF);
        glStencilMask(allow_ds ? (st >> 8) & 0xFF : 0);
        glStencilOp(k_sop[sop & 7], k_sop[(sop >> 4) & 7], k_sop[(sop >> 8) & 7]);
    } else glDisable(GL_STENCIL_TEST);
    uint32_t cm = allow_c ? (dcm >> 8) & 0xF : 0;
    glColorMask(cm & 1, (cm >> 1) & 1, (cm >> 2) & 1, (cm >> 3) & 1);
    if (R(0x100) & 0x100) {
        uint32_t bl = R(0x101), bc = R(0x103);
        glDisable(GL_COLOR_LOGIC_OP); glEnable(GL_BLEND);
        glBlendEquationSeparate(k_beq[bl & 7], k_beq[(bl >> 8) & 7]);
        glBlendFuncSeparate(k_bfac[(bl >> 16) & 0xF], k_bfac[(bl >> 20) & 0xF], k_bfac[(bl >> 24) & 0xF], k_bfac[(bl >> 28) & 0xF]);
        glBlendColor((bc & 0xFF) / 255.0f, ((bc >> 8) & 0xFF) / 255.0f, ((bc >> 16) & 0xFF) / 255.0f, (bc >> 24) / 255.0f);
    } else {
        glDisable(GL_BLEND); glEnable(GL_COLOR_LOGIC_OP);
        glLogicOp(k_logic[R(0x102) & 0xF]);
    }
    { extern int g_dbg_modo; if (g_dbg_modo & 4) glDisable(GL_BLEND); }
    if ((R(0x65) & 3) == 3) {
        int x0 = R(0x66) & 0x3FF, y0 = (R(0x66) >> 16) & 0x3FF, x1 = R(0x67) & 0x3FF, y1 = (R(0x67) >> 16) & 0x3FF;
        glEnable(GL_SCISSOR_TEST);
        int k = g_cur_c ? g_cur_c->sc : 1;
        glScissor(x0 * k, y0 * k, x1 >= x0 ? (x1 - x0 + 1) * k : 0, y1 >= y0 ? (y1 - y0 + 1) * k : 0);
    } else glDisable(GL_SCISSOR_TEST);
}

static GLuint surface_texture(uint32_t addr, int w, int h, uint32_t fmt, const Fb *f);
/* ---- filtro de textura (escolha do jogador: lançador FILTRO_TEXTURA=n ou tecla F7)
 * 0 = original (o que o jogo pede: linear ou "pixelado", mipmaps), 1 = suave (bilinear em tudo),
 * 2 = anisotrópico 16x, 3 = bicúbico + anisotrópico 16x (o bicúbico só onde o jogo já usa filtro linear,
 * para ícones e letras "pixelados" continuarem nítidos). Imagens geradas pela própria GPU (superfícies usadas
 * como textura: profundidade dos contornos, cópias de tela) ficam sempre como o jogo pede. */
volatile int g_tex_filter = -1, g_tex_filter_req = -1;
static float g_aniso = -1;                          /* anisotropia máxima da placa (0 = sem suporte) */
static uint32_t g_bicubic_mask;                     /* unidades de textura com bicúbico neste desenho */
static int tex_filter(void) {
    if (g_tex_filter < 0) { const char *e = ENVC("FILTRO_TEXTURA"); int v = e ? atoi(e) : 0; g_tex_filter = v >= 0 && v <= 3 ? v : 0; }
    if (g_tex_filter_req >= 0) { g_tex_filter = g_tex_filter_req & 3; g_tex_filter_req = -1; }
    if (g_aniso < 0) {
        GLint v = 0; glGetError(); glGetIntegerv(0x84FF /* MAX_TEXTURE_MAX_ANISOTROPY */, &v);
        g_aniso = glGetError() == 0 && v > 1 ? (float)(v < 16 ? v : 16) : 0;
        fprintf(stderr, "[gl] filtro anisotrópico: %s (máx %gx)\n", g_aniso > 0 ? "sim" : "não", g_aniso);
    }
    return g_tex_filter;
}
static void bind_textures(const Fb *f, uint32_t direct[3]) {
    static const uint32_t cfg_reg[3] = { 0x81, 0x91, 0x99 }, fmt_reg[3] = { 0x8E, 0x96, 0x9E };
    uint32_t tcfg = R(0x80);
    int tf = tex_filter();
    g_bicubic_mask = 0;
    for (int unit = 0; unit < 3; unit++) {
        GLuint tex = g_black_tex;
        direct[unit] = 0;
        if ((tcfg >> unit) & 1) {
            uint32_t b = cfg_reg[unit], size = R(b + 1), cfg = R(b + 2), addr = p2v(R(b + 4) * 8), fmt = R(fmt_reg[unit]) & 0xF;
            int w = (size >> 16) & 0x7FF, h = size & 0x7FF;
            if (w && h && fmt <= 13 && w <= 1024 && h <= 1024) {
                uint32_t tb = (uint32_t)((size_t)w * h * k_tex_bits[fmt] / 8);
                GLuint st = surface_texture(addr, w, h, fmt, f);
                if (st) { tex = st; direct[unit] = 1; }
                { static int lg = -1; if (lg < 0) lg = ENVC("TEXSRC_LOG") != NULL;     /* depuração: textura vinda de superfície */
                  if (lg && addr != 0x1f419800u && (st || (addr >= 0x1f000000u && addr < 0x1f600000u && w >= 128))) {
                      fprintf(stderr, "[texsrc]%s u%d %08x %dx%d fmt %u -> tex %u (destino %08x/%08x)", g_replay ? " REPLAY" : "", unit, addr, w, h, fmt, st, f->color, f->depth);
                      for (int i = 0; i < NSURF; i++) { Surf *q = &g_surf[i]; if (q->tex && q->addr == addr)
                          fprintf(stderr, " [surf%d %s fmt %d tex %u conv %u ver %u/%u valid %d | sombra tex %u conv %u stamp %d drawn %d]", i, q->depth ? "prof" : "cor", q->fmt, q->tex, q->conv, q->ver, q->conv_ver, q->valid,
                                  g_shadow[i].tex, g_shadow[i].conv, g_shadow_stamp[i] == g_replay_id, g_shadow_drawn[i] == g_replay_id); }
                      fputc('\n', stderr); } }
                { extern int drawlog_active(void); static int nd; extern long g_gsp_frames;
                  if (st && g_sc == 1 && ENVC("GL_DUMPTEX") && drawlog_active() && nd < 16) {   /* depuração: salva a textura vinda da GPU */
                      nd++;
                      uint8_t *px = malloc((size_t)w * h * 4);
                      glActiveTexture(GL_TEXTURE0 + 4); glBindTexture(GL_TEXTURE_2D, st);
                      glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
                      char pth[96]; snprintf(pth, sizeof pth, "dt_%08x_q%ld_%d.ppm", addr, g_gsp_frames, nd);
                      FILE *o = fopen(pth, "wb");
                      if (o) { fprintf(o, "P6\n%d %d\n255\n", w * 2, h);
                               for (int y = h - 1; y >= 0; y--) for (int x = 0; x < 2 * w; x++) {
                                   const uint8_t *q = px + ((size_t)y * w + (x % w)) * 4;
                                   uint8_t c[3] = { q[0], q[1], q[2] }; if (x >= w) c[0] = c[1] = c[2] = q[3];
                                   fwrite(c, 1, 3, o); }
                               fclose(o); }
                      free(px);
                      for (int i = 0; i < NSURF; i++) {          /* todas as superfícies nesse endereço, com estatística da profundidade */
                          Surf *q = &g_surf[i];
                          if (!q->tex || q->addr != addr) continue;
                          fprintf(stderr, "[dumptex] superfície %d %s %08x %dx%d fmt %d valid %d dirty %d ver %u conv_ver %u\n", i, q->depth ? "prof" : "cor", q->addr, q->w, q->h, q->fmt, q->valid, q->dirty, q->ver, q->conv_ver);
                          if (q->depth) {
                              uint32_t *dp = malloc((size_t)q->w * q->h * 4); GLint prev = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
                              glBindFramebuffer(GL_FRAMEBUFFER, fbo_get(0, q->tex));
                              glReadPixels(0, 0, q->w, q->h, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, dp);
                              glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev);
                              uint32_t dmin = ~0u, dmax = 0; long sh[4] = { 0 };
                              for (long k = 0; k < (long)q->w * q->h; k++) { uint32_t d = dp[k] >> 8, st = dp[k] & 255; if (d < dmin) dmin = d; if (d > dmax) dmax = d; sh[st == 0 ? 0 : st == 255 ? 2 : 1]++; }
                              fprintf(stderr, "[dumptex]   profundidade %06x..%06x, estêncil 0:%ld outros:%ld ff:%ld\n", dmin, dmax, sh[0], sh[1], sh[2]);
                              free(dp);
                          }
                      }
                  } }
                /* desenhar numa superfície e depois usá-la como textura: o resultado precisa estar na memória */
                /* quadro intermediário: textura lida da memória numa faixa que a GPU desenha (composição feita
                 * por cópias, como no começo das batalhas): o resultado não é confiável, não mostra este quadro */
                if (g_replay && !st)
                    for (int i = 0; i < NSURF; i++)
                        if (g_surf[i].tex && overlap(addr, tb, g_surf[i].addr, g_surf[i].size)) { g_replay_suspect = 1; break; }
                for (int i = 0; i < NSURF && !st && !g_replay; i++)
                    if (g_surf[i].tex && g_surf[i].dirty && overlap(addr, tb, g_surf[i].addr, g_surf[i].size)) {
                        static int nl; if (ENVC("GL_RTT_LOG") && nl++ < 30)
                            fprintf(stderr, "[rtt] textura %08x %dx%d fmt %u usa superfície %s %08x %dx%d fmt %d\n", addr, w, h, fmt,
                                    g_surf[i].depth ? "prof" : "cor", g_surf[i].addr, g_surf[i].w, g_surf[i].h, g_surf[i].fmt);
                        surf_writeback(&g_surf[i]);
                    }
                uint32_t lod = R(b + 3);                 /* 0x84/0x94/0x9C: bias, nível máximo e mínimo do mipmap */
                int levels = st ? 1 : (int)((lod >> 16) & 0xF) + 1;
                if (!st) tex = tex_get(addr, fmt, w, h, levels);
                GLuint s = g_samplers[unit];
                int magl = (cfg >> 1) & 1;
                { int lin = (cfg >> 2) & 1, mlin = (cfg >> 24) & 1;
                  if (tf == 1 && !st) { lin = 1; mlin = 1; magl = 1; }                 /* suave: linear em tudo */
                  if (g_aniso > 0) { float an = tf >= 2 && !st && lin ? g_aniso : 1.0f; glSamplerParameterfv(s, 0x84FE /* TEXTURE_MAX_ANISOTROPY */, &an); }
                  if (tf == 3 && !st && magl) g_bicubic_mask |= 1u << unit;
                  GLenum minf = levels > 1 ? (lin ? (mlin ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST) : (mlin ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST))
                                           : (lin ? GL_LINEAR : GL_NEAREST);
                  glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, minf);
                  float bias = (float)((int32_t)(lod << 19) >> 19) / 256.0f, minl = (float)((lod >> 24) & 0xF), maxl = (float)((lod >> 16) & 0xF);
                  glSamplerParameterfv(s, GL_TEXTURE_LOD_BIAS, &bias);
                  glSamplerParameterfv(s, GL_TEXTURE_MIN_LOD, &minl);
                  glSamplerParameterfv(s, GL_TEXTURE_MAX_LOD, &maxl); }
                glSamplerParameteri(s, GL_TEXTURE_WRAP_T, k_wrap[(cfg >> 8) & 7]);
                glSamplerParameteri(s, GL_TEXTURE_WRAP_S, k_wrap[(cfg >> 12) & 7]);
                glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, magl ? GL_LINEAR : GL_NEAREST);

                uint32_t bc = R(b);
                float border[4] = { (bc & 0xFF) / 255.0f, ((bc >> 8) & 0xFF) / 255.0f, ((bc >> 16) & 0xFF) / 255.0f, (bc >> 24) / 255.0f };
                glSamplerParameterfv(s, GL_TEXTURE_BORDER_COLOR, border);
            }
        }
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, tex);
    }
}

/* ---- superfície usada diretamente como textura (sem passar pela memória)
 * Cor: mesma textura da GPU (a linha 0 da superfície é a última da textura na memória, daí a flag sem inversão).
 * Profundidade D24S8 lida como RGBA8 (R = estêncil, G/B/A = bytes da profundidade): convertida na GPU. */
static GLuint g_conv_d_prog, g_conv_s_prog, g_empty_vao;
static int g_stencil_tex = -1;          /* driver permite ler o estêncil numa textura (GL 4.3 / ARB_stencil_texturing) */
static GLuint link2(const char *vs, const char *fs) {
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram(); glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}
static const char *k_conv_vs =
"#version 330 core\n"
"void main() { vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0); gl_Position = vec4(p, 0.0, 1.0); }\n";
static const char *k_conv_d_fs =
"#version 330 core\n"
"uniform sampler2D u_d; out vec4 o;\n"
/* profundidade com a precisão do float24 da PICA (16 bits de mantissa): sem isso os bytes baixos variam
 * sem parar numa superfície e os efeitos que leem a profundidade como cor (contornos) viram tracejado */
"float f24q(float d) { if (d <= 0.0) return 0.0; float st = exp2(floor(log2(d)) - 16.0); return floor(d / st) * st; }\n"
"void main() { uint v = uint(f24q(texelFetch(u_d, ivec2(gl_FragCoord.xy), 0).r) * 16777215.0);\n"
"  o = vec4(0.0, float((v >> 16u) & 255u), float((v >> 8u) & 255u), float(v & 255u)) / 255.0; }\n";
static const char *k_conv_s_fs =
"#version 330 core\n"
"uniform usampler2D u_s; out vec4 o;\n"
"void main() { o = vec4(float(texelFetch(u_s, ivec2(gl_FragCoord.xy), 0).r) / 255.0, 0.0, 0.0, 0.0); }\n";
static GLuint depth_as_rgba(Surf *s) {
    if (s->conv && s->conv_ver == s->ver) return s->conv;
    if (!g_conv_d_prog) {
        g_conv_d_prog = link2(k_conv_vs, k_conv_d_fs);
        g_conv_s_prog = link2(k_conv_vs, k_conv_s_fs);
        glGenVertexArrays(1, &g_empty_vao);
        if (!g_conv_d_prog) return 0;
        glUseProgram(g_conv_d_prog); glUniform1i(glGetUniformLocation(g_conv_d_prog, "u_d"), 5);
        if (g_conv_s_prog) { glUseProgram(g_conv_s_prog); glUniform1i(glGetUniformLocation(g_conv_s_prog, "u_s"), 5); }
    }
    if (!s->conv) {
        glGenTextures(1, &s->conv);
        glActiveTexture(GL_TEXTURE0 + 4); glBindTexture(GL_TEXTURE_2D, s->conv);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s->w * s->sc, s->h * s->sc, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_get(s->conv, 0));
    glViewport(0, 0, s->w * s->sc, s->h * s->sc);
    glDisable(GL_BLEND); glDisable(GL_COLOR_LOGIC_OP); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(g_empty_vao);
    glActiveTexture(GL_TEXTURE0 + 5); glBindTexture(GL_TEXTURE_2D, s->tex);
    glColorMask(1, 1, 1, 1);
    glUseProgram(g_conv_d_prog);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (g_conv_s_prog && g_stencil_tex != 0) {
        glGetError();
        glTexParameteri(GL_TEXTURE_2D, 0x90EA /* DEPTH_STENCIL_TEXTURE_MODE */, 0x1901 /* STENCIL_INDEX */);
        if (g_stencil_tex < 0) { g_stencil_tex = glGetError() == 0; if (!g_stencil_tex) fprintf(stderr, "[gl] sem leitura de estêncil em textura\n"); }
        if (g_stencil_tex) {
            glColorMask(1, 0, 0, 0);
            glUseProgram(g_conv_s_prog);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glTexParameteri(GL_TEXTURE_2D, 0x90EA, 0x1902 /* DEPTH_COMPONENT */);
        }
    }
    { static int nd; extern long g_gsp_frames;               /* depuração: GL_CONVDUMP salva a cópia convertida de vez em quando */
      if (ENVC("GL_CONVDUMP") && s->w == 256 && s->h == 512 && (nd++ % 400) == 0) {
        int W = s->w * s->sc, H = s->h * s->sc; uint8_t *px = malloc((size_t)W * H * 4);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
        char pth[256]; snprintf(pth, sizeof pth, "conv_%05ld.ppm", g_gsp_frames); FILE *o = fopen(pth, "wb");
        if (o) { fprintf(o, "P6\n%d %d\n255\n", W, H); for (int i = 0; i < W * H; i++) fwrite(px + 4 * i, 1, 3, o); fclose(o); }
        free(px); } }
    glUseProgram(g_prog);
    glBindVertexArray(g_vao);
    GLCHK("profundidade lida como cor");
    s->conv_ver = s->ver;
    return s->conv;
}
/* textura no endereço exato de uma superfície, com formato compatível: usa a GPU direto */
static GLuint surface_texture(uint32_t addr, int w, int h, uint32_t fmt, const Fb *f) {
    static int off = -1; if (off < 0) off = ENVC("GL_SEM_RTT") != NULL;
    if (off) return 0;
    { static long wa = -2; if (wa == -2) wa = ENVC("GL_TEXLOG") ? (long)strtoul(ENVC("GL_TEXLOG"), 0, 16) : -1;   /* depuração */
      static int nl; if (wa == (long)addr && (nl++ % 300) == 0) {
        fprintf(stderr, "[texlog] %08x %dx%d fmt %u (destino %08x/%08x):", addr, w, h, fmt, f->color, f->depth);
        for (int i = 0; i < NSURF; i++) { Surf *s = &g_surf[i]; if (s->tex && overlap(s->addr, s->size, addr, 1))
            fprintf(stderr, " [%s %08x %dx%d fmt %d valid %d dirty %d ver %u conv %u]", s->depth ? "prof" : "cor", s->addr, s->w, s->h, s->fmt, s->valid, s->dirty, s->ver, s->conv_ver); }
        fprintf(stderr, "\n"); } }
    for (int i = 0; i < NSURF; i++) {
        Surf *s = &g_surf[i];
        if (!s->tex || !s->valid || s->addr != addr || s->w != w || s->h != h) continue;
        /* desenhada neste quadro intermediário: usa a cópia de trabalho. Só limpa (a imagem veio de uma cópia
         * do GSP, ex.: a captura da tela no começo das batalhas): usa a imagem real */
        if (g_replay && g_shadow_stamp[i] == g_replay_id && g_shadow_drawn[i] == g_replay_id && g_shadow[i].tex) s = &g_shadow[i];
        if (!s->depth && (uint32_t)s->fmt == fmt && addr != f->color) return s->tex;          /* (não lê o próprio destino) */
        /* profundidade+estêncil lida como RGBA8 (R = estêncil): a conversão vai para uma cópia, então vale
         * mesmo se for o buffer de profundidade do próprio desenho (a abertura das batalhas faz isso para
         * recortar a treinadora com a máscara do estêncil) */
        if (s->depth && s->fmt == 3 && fmt == 0) return depth_as_rgba(s);
    }
    return 0;
}

/* ------------------------------------------------------------ desenho */
static float *g_vb; static size_t g_vb_cap;
int g_dbg_skip, g_dbg_modo;

/* ---- alargamento das bordas das malhas (ver pica_gl_draw) ----
 * Só são empurradas as bordas que encostam (a menos de uma fração de pixel) numa borda de outro desenho já
 * feito no mesmo quadro: é ali que ficam as frestas entre pedaços do cenário. Bordas livres (beiradas de
 * decalques, grama recortada, contornos) ficam onde estão, senão a última fileira da textura é esticada e
 * aparece como uma linha escura. */
typedef struct { int64_t k; float nx, ny, d; int cnt, gen, bnd; } OsV;
typedef struct { int64_t a, b; int cnt, gen; float nx, ny; } OsE;
static OsV *g_osv; static OsE *g_ose; static int g_os_cap, g_os_sz, g_os_dgen;
static int *g_os_el, *g_os_vl, g_os_ne, g_os_nv, g_os_lcap;   /* arestas e pontas de borda do desenho */    /* tabelas de dispersão, tamanho potência de 2 */
/* bordas dos desenhos anteriores do quadro, numa grade de células de 4 pixels */
typedef struct { float x0, y0, x1, y1; uint32_t fb; } OsSeg;
typedef struct { int seg, next; } OsLink;
typedef struct { int64_t cell; int head, gen; } OsCell;
#define OS_NCELL (1 << 16)
#define OS_MAXSEG (1 << 16)
#define OS_MAXLINK (1 << 18)
static OsSeg *g_seg; static int g_nseg; static OsLink *g_link; static int g_nlink; static OsCell *g_cell; static int g_os_gen;
/* arestas de borda exatas já vistas (para não empurrar a mesma aresta, que o OpenGL fecha sozinho) */
typedef struct { int64_t a, b; int gen; } OsEx;
#define OS_NEX (1 << 16)
static OsEx *g_ex;
/* bordas já empurradas no quadro: a mesma malha desenhada de novo (outra passada, com teste de profundidade
 * "igual") recebe o mesmo empurrão, senão a segunda passada falha na faixa alargada e aparece só a primeira */
typedef struct { int64_t a, b; float nx, ny, d; int gen; } OsPush;
#define OS_NPUSH (1 << 15)
static OsPush *g_push;
static float g_os_w, g_os_h; static long long g_os_ns;
static int g_os_store_epoch = -1; static long g_os_nb, g_os_np;   /* estatística (GL_ALARGAR_LOG) */
static inline int64_t os_key(const SV *v) {
    int32_t x = (int32_t)floorf(v->x * 16.0f + 0.5f), y = (int32_t)floorf(v->y * 16.0f + 0.5f);
    return ((int64_t)x << 32) | (uint32_t)y;
}
static inline float os_kx(int64_t k) { return (float)(int32_t)(k >> 32) * (1.0f / 16.0f); }
static inline float os_ky(int64_t k) { return (float)(int32_t)(uint32_t)k * (1.0f / 16.0f); }
static inline uint32_t os_hash(uint64_t h) { h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33; return (uint32_t)h; }
static OsV *os_vfind(int64_t k) {
    uint32_t m = g_os_sz - 1, i = os_hash((uint64_t)k) & m;
    while (g_osv[i].gen == g_os_dgen && g_osv[i].k != k) i = (i + 1) & m;
    if (g_osv[i].gen != g_os_dgen) { g_osv[i].k = k; g_osv[i].gen = g_os_dgen; g_osv[i].cnt = 0; g_osv[i].bnd = 0; g_osv[i].nx = g_osv[i].ny = g_osv[i].d = 0; }
    return &g_osv[i];
}
static OsE *os_efind(int64_t a, int64_t b) {
    if (a > b) { int64_t t = a; a = b; b = t; }
    uint32_t m = g_os_sz - 1, i = os_hash((uint64_t)a * 0x9E3779B97F4A7C15ULL ^ (uint64_t)b) & m;
    while (g_ose[i].gen == g_os_dgen && (g_ose[i].a != a || g_ose[i].b != b)) i = (i + 1) & m;
    if (g_ose[i].gen != g_os_dgen) { g_ose[i].a = a; g_ose[i].b = b; g_ose[i].gen = g_os_dgen; g_ose[i].cnt = 0; }
    return &g_ose[i];
}
static OsCell *os_cell(int cx, int cy, int create) {
    int64_t key = ((int64_t)cx << 32) | (uint32_t)cy;
    uint32_t m = OS_NCELL - 1, i = os_hash((uint64_t)key) & m;
    for (int probe = 0; probe < 16; probe++, i = (i + 1) & m) {
        if (g_cell[i].gen != g_os_gen) { if (!create) return NULL; g_cell[i].cell = key; g_cell[i].head = -1; g_cell[i].gen = g_os_gen; return &g_cell[i]; }
        if (g_cell[i].cell == key) return &g_cell[i];
    }
    return NULL;
}
static void os_store_reset(void) {
    if (!g_cell) { g_cell = calloc(OS_NCELL, sizeof(OsCell)); g_seg = malloc(sizeof(OsSeg) * OS_MAXSEG); g_link = malloc(sizeof(OsLink) * OS_MAXLINK);
                   g_push = calloc(OS_NPUSH, sizeof(OsPush)); g_ex = calloc(OS_NEX, sizeof(OsEx)); g_os_gen = 1; }
    else g_os_gen++;                       /* esvazia a grade sem apagar a memória */
    g_nseg = 0; g_nlink = 0;
}
static OsEx *os_ex_find(int64_t a, int64_t b, int create) {
    if (a > b) { int64_t t = a; a = b; b = t; }
    uint32_t m = OS_NEX - 1, i = os_hash((uint64_t)a * 0x9E3779B97F4A7C15ULL ^ (uint64_t)b) & m;
    for (int probe = 0; probe < 32; probe++, i = (i + 1) & m) {
        if (g_ex[i].gen != g_os_gen) { if (!create) return NULL; g_ex[i].a = a; g_ex[i].b = b; g_ex[i].gen = g_os_gen; return &g_ex[i]; }
        if (g_ex[i].a == a && g_ex[i].b == b) return &g_ex[i];
    }
    return NULL;
}
/* recorta o segmento ao retângulo da tela (com folga); 0 se fica todo fora */
static int os_clip(float *x0, float *y0, float *x1, float *y1) {
    float t0 = 0, t1 = 1, dx = *x1 - *x0, dy = *y1 - *y0;
    float p[4] = { -dx, dx, -dy, dy }, q[4] = { *x0 + 4, g_os_w + 4 - *x0, *y0 + 4, g_os_h + 4 - *y0 };
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0) { if (q[i] < 0) return 0; continue; }
        float r = q[i] / p[i];
        if (p[i] < 0) { if (r > t1) return 0; if (r > t0) t0 = r; }
        else { if (r < t0) return 0; if (r < t1) t1 = r; }
    }
    float ax = *x0 + dx * t0, ay = *y0 + dy * t0, bx = *x0 + dx * t1, by = *y0 + dy * t1;
    *x0 = ax; *y0 = ay; *x1 = bx; *y1 = by;
    return 1;
}
/* guarda o segmento em todas as células de 4 px que ele atravessa (percurso exato da grade) */
static void os_seg_add(float x0, float y0, float x1, float y1, uint32_t fb) {
    if (g_nseg >= OS_MAXSEG || !os_clip(&x0, &y0, &x1, &y1)) return;
    int id = g_nseg++; g_seg[id] = (OsSeg){ x0, y0, x1, y1, fb };
    const float C = 0.25f;                 /* 1 / tamanho da célula */
    float fx = x0 * C, fy = y0 * C, ex = x1 * C, ey = y1 * C;
    int cx = (int)floorf(fx), cy = (int)floorf(fy), tx = (int)floorf(ex), ty = (int)floorf(ey);
    float dx = ex - fx, dy = ey - fy;
    int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
    float tdx = dx != 0 ? fabsf(1.0f / dx) : 1e30f, tdy = dy != 0 ? fabsf(1.0f / dy) : 1e30f;
    float tmx = dx != 0 ? ((dx > 0 ? (cx + 1 - fx) : (fx - cx)) * tdx) : 1e30f;
    float tmy = dy != 0 ? ((dy > 0 ? (cy + 1 - fy) : (fy - cy)) * tdy) : 1e30f;
    for (int guard = 0; guard < 1024; guard++) {
        OsCell *c = os_cell(cx, cy, 1);
        if (c && g_nlink < OS_MAXLINK) { g_link[g_nlink] = (OsLink){ id, c->head }; c->head = g_nlink++; }
        if (cx == tx && cy == ty) break;
        if (tmx < tmy) { if (tmx > 1) break; tmx += tdx; cx += sx; } else { if (tmy > 1) break; tmy += tdy; cy += sy; }
    }
}
/* distância do ponto até a borda anterior mais próxima (até tol); -1 se nenhuma */
static float os_seg_near(float x, float y, float ux, float uy, float tol, uint32_t fb) {
    float best = -1;
    int x0 = (int)floorf((x - tol) * 0.25f), x1 = (int)floorf((x + tol) * 0.25f), y0 = (int)floorf((y - tol) * 0.25f), y1 = (int)floorf((y + tol) * 0.25f);
    for (int cy = y0; cy <= y1; cy++) for (int cx = x0; cx <= x1; cx++) {
        OsCell *c = os_cell(cx, cy, 0);
        if (!c) continue;
        for (int l = c->head; l >= 0; l = g_link[l].next) {
            const OsSeg *g = &g_seg[g_link[l].seg];
            if (g->fb != fb) continue;
            float vx = g->x1 - g->x0, vy = g->y1 - g->y0, L2 = vx * vx + vy * vy, t = 0;
            if (L2 < 0.25f || fabsf(ux * vy - uy * vx) > 0.05f * sqrtf(L2)) continue;   /* só bordas paralelas */
            if (L2 > 0) { t = ((x - g->x0) * vx + (y - g->y0) * vy) / L2; if (t < 0) t = 0; if (t > 1) t = 1; }
            float px = g->x0 + vx * t - x, py = g->y0 + vy * t - y, d = sqrtf(px * px + py * py);
            if (d <= tol && (best < 0 || d < best)) best = d;
        }
    }
    return best;
}
static OsPush *os_push_find(int64_t a, int64_t b, int create) {
    if (a > b) { int64_t t = a; a = b; b = t; }
    uint32_t m = OS_NPUSH - 1, i = os_hash((uint64_t)a * 0x9E3779B97F4A7C15ULL ^ (uint64_t)b) & m;
    for (int probe = 0; probe < 32; probe++, i = (i + 1) & m) {
        if (g_push[i].gen != g_os_gen) { if (!create) return NULL; g_push[i].a = a; g_push[i].b = b; g_push[i].gen = g_os_gen; return &g_push[i]; }
        if (g_push[i].a == a && g_push[i].b == b) return &g_push[i];
    }
    return NULL;
}
/* d: empurrão mínimo (meio pixel da tela em escala); tol: maior fresta que se fecha */
static int g_os_pushes;                  /* empurrões decididos no quadro */
static int outset_prepare(const Tri *t, int n, uint32_t cull, uint32_t fb, int fw, int fh, float d, float tol, int novo) {
    g_os_w = (float)fw; g_os_h = (float)fh;
    int need = 1; while (need < n * 4 + 16) need <<= 1;
    if (n * 3 + 16 > g_os_lcap) { g_os_lcap = n * 3 + 16; g_os_el = realloc(g_os_el, sizeof(int) * g_os_lcap); g_os_vl = realloc(g_os_vl, sizeof(int) * g_os_lcap * 2); }
    g_os_ne = g_os_nv = 0;
    if (need > g_os_cap) { free(g_osv); free(g_ose); g_os_cap = need; g_osv = calloc(need, sizeof(OsV)); g_ose = calloc(need, sizeof(OsE)); g_os_dgen = 0; }
    g_os_sz = need;
    if (++g_os_dgen == 0) { memset(g_osv, 0, sizeof(OsV) * g_os_cap); memset(g_ose, 0, sizeof(OsE) * g_os_cap); g_os_dgen = 1; }   /* esvazia sem apagar */
    if (g_os_store_epoch != g_os_epoch || !g_cell) { os_store_reset(); g_os_store_epoch = g_os_epoch; g_os_pushes = 0; }
    if (!novo && !g_os_pushes) return 0;
    g_os_store_fb = fb;
    /* 1) conta quantos triângulos usam cada aresta e guarda a normal para fora do triângulo */
    for (int i = 0; i < n; i++) {
        const SV *v[3] = { &t[i].v[0], &t[i].v[1], &t[i].v[2] };
        float area = (v[1]->x - v[0]->x) * (v[2]->y - v[0]->y) - (v[1]->y - v[0]->y) * (v[2]->x - v[0]->x);
        if (area == 0 || (cull == 1 && area > 0) || (cull == 2 && area < 0)) continue;
        if (fmaxf(v[0]->x, fmaxf(v[1]->x, v[2]->x)) < -4 || fminf(v[0]->x, fminf(v[1]->x, v[2]->x)) > g_os_w + 4 ||
            fmaxf(v[0]->y, fmaxf(v[1]->y, v[2]->y)) < -4 || fminf(v[0]->y, fminf(v[1]->y, v[2]->y)) > g_os_h + 4) continue;   /* fora da tela */
        int64_t k[3] = { os_key(v[0]), os_key(v[1]), os_key(v[2]) };
        for (int e = 0; e < 3; e++) {
            const SV *a = v[e], *b = v[(e + 1) % 3], *c = v[(e + 2) % 3];
            if (k[e] == k[(e + 1) % 3]) continue;
            OsE *E = os_efind(k[e], k[(e + 1) % 3]);
            if (E->cnt++ == 0) {
                g_os_el[g_os_ne++] = (int)(E - g_ose);
                float dx = b->x - a->x, dy = b->y - a->y, l = sqrtf(dx * dx + dy * dy);
                float nx = l > 0 ? dy / l : 0, ny = l > 0 ? -dx / l : 0;
                if ((c->x - a->x) * nx + (c->y - a->y) * ny > 0) { nx = -nx; ny = -ny; }
                E->nx = nx; E->ny = ny;
            }
        }
    }
    /* 2) borda (um só triângulo) cujas duas pontas caem sobre bordas de desenhos anteriores ou a menos de tol
     * delas (inclui as junções em T, em que a ponta fica no meio da borda vizinha), e que não é exatamente uma
     * aresta já vista (essa o OpenGL fecha sozinho): soma a normal nos dois vértices */
    for (int q = 0; q < g_os_ne; q++) {
        OsE *E = &g_ose[g_os_el[q]];
        if (E->cnt != 1) continue;
        float ax = os_kx(E->a), ay = os_ky(E->a), bx = os_kx(E->b), by = os_ky(E->b);
        OsV *A = os_vfind(E->a), *B = os_vfind(E->b);
        if (!A->bnd) { A->bnd = 1; g_os_vl[g_os_nv++] = (int)(A - g_osv); }
        if (!B->bnd) { B->bnd = 1; g_os_vl[g_os_nv++] = (int)(B - g_osv); }
        if (fmaxf(ax, bx) < -4 || fmaxf(ay, by) < -4 || fminf(ax, bx) > g_os_w + 4 || fminf(ay, by) > g_os_h + 4) continue;   /* fora da tela */
        g_os_nb++;
        float de, nx = E->nx, ny = E->ny;
        OsPush *pu = os_push_find(E->a, E->b, 0);
        if (pu && pu->nx * nx + pu->ny * ny > 0.9f) de = pu->d;            /* outra passada da mesma malha */
        else if (!novo) continue;
        else {
            static int tudo = -1; if (tudo < 0) tudo = ENVC("GL_ALARGAR_TUDO") != 0;   /* depuração: todas as bordas */
            if (!tudo && os_ex_find(E->a, E->b, 0)) continue;             /* a mesma aresta: sem fresta */
            float ex = bx - ax, ey = by - ay, el = sqrtf(ex * ex + ey * ey); if (el < 0.5f) continue;
            ex /= el; ey /= el;
            float ga = tudo ? 0 : os_seg_near(ax, ay, ex, ey, tol, fb); if (ga < 0) continue;
            float gb = tudo ? 0 : os_seg_near(bx, by, ex, ey, tol, fb); if (gb < 0) continue;
            de = d + fmaxf(ga, gb);
            if ((pu = os_push_find(E->a, E->b, 1))) { pu->nx = nx; pu->ny = ny; pu->d = de; g_os_pushes++; }
        }
        g_os_np++;
        A->nx += nx; A->ny += ny; A->cnt++; if (de > A->d) A->d = de;
        B->nx += nx; B->ny += ny; B->cnt++; if (de > B->d) B->d = de;
    }
    /* 3) as bordas deste desenho passam a valer para os próximos */
    if (novo) for (int q = 0; q < g_os_ne; q++) {
        OsE *E = &g_ose[g_os_el[q]];
        if (E->cnt != 1) continue;
        os_seg_add(os_kx(E->a), os_ky(E->a), os_kx(E->b), os_ky(E->b), fb);
        os_ex_find(E->a, E->b, 1);
    }
    return 1;
}
/* deslocamento do vértice: na esquina, a interseção das bordas empurradas (com limite nas pontas finas) */
static void outset_get(const SV *v, float out[2]) {
    out[0] = out[1] = 0;
    int64_t k = os_key(v);
    uint32_t m = g_os_sz - 1, i = os_hash((uint64_t)k) & m;
    while (g_osv[i].gen == g_os_dgen && g_osv[i].k != k) i = (i + 1) & m;
    if (g_osv[i].gen != g_os_dgen || g_osv[i].cnt <= 0) return;
    float sx = g_osv[i].nx, sy = g_osv[i].ny, s2 = sx * sx + sy * sy, d = g_osv[i].d;
    if (s2 < 1e-6f) return;
    float f = d * g_osv[i].cnt / s2;   /* 2 arestas: d (n1+n2) / (1 + n1·n2) */
    float ox = sx * f, oy = sy * f, mg = sqrtf(ox * ox + oy * oy), lim = 1.5f * d;
    if (mg > lim) { ox *= lim / mg; oy *= lim / mg; }
    out[0] = ox; out[1] = oy;
}

void pica_gl_draw(const Fb *f, const Tri *t, int n) {
    if (g_vk) { pica_vk_draw(f, t, n); return; }
    if (!n) return;
    uint32_t direct[3];
    bind_textures(f, direct);              /* antes do destino: pode precisar devolver superfícies à memória */
    GLCHK("texturas do desenho");
    { extern int g_dbg_skip; if (g_dbg_skip && (direct[0] || direct[1] || direct[2])) return; }   /* depuração (pula_composicao.txt) */
    surf_bind(f);
    GLCHK("destino do desenho");
    static const uint32_t k_tev_base[6] = { 0xC0, 0xC8, 0xD0, 0xD8, 0xF0, 0xF8 };
    uint32_t u[64]; memset(u, 0, sizeof u);
    for (int st = 0; st < 6; st++) for (int k = 0; k < 5; k++) u[st * 5 + k] = R(k_tev_base[st] + k);
    u[30] = R(0xFD); u[31] = R(0xE0); u[32] = R(0x80); u[33] = R(0x104);
    { extern int g_dbg_modo; if (g_dbg_modo & 1) u[33] = 0; }   /* depuração: sem teste de alfa */
    u[54] = direct[0]; u[55] = direct[1]; u[56] = direct[2]; u[57] = R(0xE1); u[58] = R(0x83); u[59] = g_bicubic_mask;
    { extern uint32_t g_fog_lut[128]; extern volatile int g_fog_lut_dirty; extern const uint32_t *g_fog_lut_override;
      if ((R(0xE0) & 7) == 5 && g_fog_lut_dirty) {
          float fl[128][2]; const uint32_t *fog = g_fog_lut_override ? g_fog_lut_override : g_fog_lut;
          for (int i = 0; i < 128; i++) { uint32_t e = fog[i]; fl[i][0] = (float)((e >> 13) & 0x7FF) / 2047.0f; fl[i][1] = (float)((int32_t)(e << 19) >> 19) / 2047.0f; }
          glUniform2fv(g_loc_fog, 128, &fl[0][0]); g_fog_lut_dirty = 0;
      } }
    if (g_light_on) {
        float L[49][3], LD[8][2];
        light_gl_params(u + 34, L, LD);
        glUniform3fv(g_loc_L, 49, &L[0][0]);
        glUniform2fv(g_loc_LD, 8, &LD[0][0]);
        if (g_light_lut_dirty) {
            glActiveTexture(GL_TEXTURE0 + 3); glBindTexture(GL_TEXTURE_2D, g_lut_tex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 24, GL_RG, GL_FLOAT, light_lut_data());
            g_light_lut_dirty = 0;
        }
    }
    glUniform1uiv(g_loc_R, 64, u);
    apply_state(f);
    GLCHK("estado do desenho");
    /* triângulos: descarte de faces como no software, depois tela -> coordenadas normalizadas */
    size_t need = (size_t)n * 3 * VSTRIDE;
    if (need > g_vb_cap) { g_vb_cap = need; g_vb = realloc(g_vb, need * sizeof(float)); }
    uint32_t cull = R(0x40) & 3;
    float sx = 2.0f / f->w, sy = 2.0f / f->h;
    int nv = 0;
    static int grade = -1; if (grade < 0) grade = !ENVC("GL_SEM_GRADE");
    static int sem_alargar = -1; if (sem_alargar < 0) sem_alargar = !ENVC("GL_ALARGAR");   /* desligado: só com GL_ALARGAR (teste) */
    /* resolução alta: o cenário do jogo tem frestas minúsculas entre pedaços vizinhos (frações de pixel, junções
     * em T). No aparelho elas quase nunca cobrem o centro de um pixel; com 2x-4x mais pixels aparecem como linhas
     * tracejadas da cor de fundo. Nos desenhos 3D com escrita de profundidade, as bordas de malha (arestas que só
     * um triângulo do desenho usa) que encostam numa borda de outro desenho do mesmo quadro são empurradas para
     * fora o tamanho da fresta mais meio pixel da tela em escala (só em desenhos opacos). Os vértices são movidos por igual em todos os
     * triângulos que os usam, então a malha continua sem buracos e sem sobreposição dentro dela */
    float outset = 0; int novo = 0;
    { uint32_t dcm = R(0x107);
      /* decide empurrões novos só nos desenhos opacos com escrita de profundidade (decalques com mistura por
       * cima do chão ficam como estão); os outros desenhos com teste de profundidade só repetem empurrões já
       * decididos para as mesmas arestas (outras passadas da mesma malha) */
      if (g_sc > 1 && !sem_alargar && f->depth && (dcm & 1)) outset = 0.5f / g_sc;
      novo = ((dcm >> 12) & 1) && (R(0x115) & 3) &&
             !((R(0x100) & 0x100) && ((R(0x101) >> 16 & 0xF) != 1 || (R(0x101) >> 20 & 0xF) != 0)); }
    if (outset > 0) {                      /* só desenhos em perspectiva (o 2D da interface fica como está) */
        int persp = 0;
        for (int i = 0; i < n && !persp; i++) for (int k = 0; k < 3; k++) if (fabsf(t[i].v[k].invw - 1.0f) > 1e-4f) { persp = 1; break; }
        extern uint64_t k_now_ns(void); uint64_t t0n = k_now_ns();
        if (!persp || !outset_prepare(t, n, cull, f->color, f->w, f->h, outset, 0.3f, novo)) outset = 0;
        g_os_ns += (long long)(k_now_ns() - t0n);
    }
    for (int i = 0; i < n; i++) {
        const SV *v0 = &t[i].v[0], *v1 = &t[i].v[1], *v2 = &t[i].v[2];
        float area = (v1->x - v0->x) * (v2->y - v0->y) - (v1->y - v0->y) * (v2->x - v0->x);
        if (area == 0 || (cull == 1 && area > 0) || (cull == 2 && area < 0)) continue;
        float qs[3] = { 1, 1, 1 };
        if (g_light_on) {
            const float *q0 = v0->v.quat, *q1 = v1->v.quat, *q2 = v2->v.quat;
            if (q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3] < 0) qs[1] = -1;
            if (q0[0] * q2[0] + q0[1] * q2[1] + q0[2] * q2[2] + q0[3] * q2[3] < 0) qs[2] = -1;
        }
        const SV *vv[3] = { v0, v1, v2 };
        float P[3][2], D[3][2] = { { 0 } }; int moved = 0;
        for (int k = 0; k < 3; k++) {
            /* posição na grade de 1/16 de pixel do 3DS (o rasterizador da PICA usa ponto fixo 12.4): pedaços
             * vizinhos do cenário calculados com matrizes diferentes caem no mesmo ponto, como no aparelho */
            P[k][0] = grade ? floorf(vv[k]->x * 16.0f + 0.5f) * (1.0f / 16.0f) : vv[k]->x;
            P[k][1] = grade ? floorf(vv[k]->y * 16.0f + 0.5f) * (1.0f / 16.0f) : vv[k]->y;
            if (outset > 0) { outset_get(vv[k], D[k]); if (D[k][0] != 0 || D[k][1] != 0) moved = 1; }
        }
        float *ob = g_vb + (size_t)nv * VSTRIDE;
        for (int k = 0; k < 3; k++) {
            const SV *s = vv[k]; const Vertex *v = &s->v;
            float *o = ob + (size_t)k * VSTRIDE;
            o[0] = P[k][0] * sx - 1.0f; o[1] = P[k][1] * sy - 1.0f; o[2] = s->z * 2.0f - 1.0f; o[3] = 1.0f / s->invw;
            memcpy(o + 4, v->color, 4 * sizeof(float));
            o[8] = v->tc0[0]; o[9] = v->tc0[1]; o[10] = v->tc1[0]; o[11] = v->tc1[1]; o[12] = v->tc2[0]; o[13] = v->tc2[1]; o[14] = v->tc0w;
            for (int j = 0; j < 4; j++) o[15 + j] = v->quat[j] * qs[k];
            memcpy(o + 19, v->view, 3 * sizeof(float));
        }
        /* vértice empurrado para fora: textura, cor e profundidade continuam o plano do triângulo (com correção de
         * perspectiva) em vez de esticar a última fileira de texels */
        if (moved) {
            float A[3][VSTRIDE]; memcpy(A, ob, sizeof A);
            float e1x = P[1][0] - P[0][0], e1y = P[1][1] - P[0][1], e2x = P[2][0] - P[0][0], e2y = P[2][1] - P[0][1];
            float det = e1x * e2y - e1y * e2x;
            for (int k = 0; k < 3 && det != 0; k++) {
                if (D[k][0] == 0 && D[k][1] == 0) continue;
                float px = P[k][0] + D[k][0] - P[0][0], py = P[k][1] + D[k][1] - P[0][1];
                float l1 = (px * e2y - py * e2x) / det, l2 = (e1x * py - e1y * px) / det, l0 = 1.0f - l1 - l2;
                float lam[3] = { l0, l1, l2 }, mu[3], sm = 0;
                for (int i = 0; i < 3; i++) { mu[i] = lam[i] / A[i][3]; sm += mu[i]; }
                if (!(sm > 1e-12f) || fabsf(l0) > 4 || fabsf(l1) > 4 || fabsf(l2) > 4) continue;
                float *o = ob + (size_t)k * VSTRIDE;
                o[0] = (P[k][0] + D[k][0]) * sx - 1.0f; o[1] = (P[k][1] + D[k][1]) * sy - 1.0f;
                o[2] = lam[0] * A[0][2] + lam[1] * A[1][2] + lam[2] * A[2][2];
                o[3] = 1.0f / sm;
                for (int j = 4; j < VSTRIDE; j++) o[j] = (mu[0] * A[0][j] + mu[1] * A[1][j] + mu[2] * A[2][j]) / sm;
            }
        }
        nv += 3;
    }
    g_pica_tris += nv / 3;
    if (!nv) return;
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nv * VSTRIDE * sizeof(float), g_vb, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, nv);
    GLCHK("desenho");
    { extern int drawlog_active(void); static int nz;
      if (ENVC("GL_ZDBG") && g_sc == 1 && drawlog_active() && nz < 120 && g_cur_d) {   /* depuração: profundidade antes/depois */
          nz++;
          float zmin = 1e9f, zmax = -1e9f;
          for (int i = 0; i < n; i++) for (int k = 0; k < 3; k++) { float z = t[i].v[k].z; if (z < zmin) zmin = z; if (z > zmax) zmax = z; }
          static uint32_t *dp; static int cap; if (cap < f->w * f->h) { cap = f->w * f->h; dp = realloc(dp, (size_t)cap * 4); }
          glReadPixels(0, 0, f->w, f->h, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, dp);
          uint32_t dmin = ~0u; long nn = 0;
          for (int k = 0; k < f->w * f->h; k++) { uint32_t d = dp[k] >> 8; if (d < 0xFFFFFF) { nn++; if (d < dmin) dmin = d; } }
          fprintf(stderr, "[zdbg] fb %08x n=%d z do desenho %.6f..%.6f (%06x..%06x) | buffer depois: %ld pixels < ffffff, mín %06x | dcm %08x\n",
                  f->color, n, zmin, zmax, (unsigned)(zmin * 16777215.0f), (unsigned)(zmax * 16777215.0f), nn, dmin, R(0x107));
      } }
    { extern long g_gsp_frames; static long nd; const char *e = ENVC("GL_DEBUG_FRAME");
      if (e && dbg_on() && nd++ < 60) {
        uint8_t px[4] = { 0 }; uint32_t dp = 0;
        glReadPixels(f->w / 2, f->h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        if (g_cur_d) glReadPixels(f->w / 2, f->h / 2, 1, 1, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, &dp);
        fprintf(stderr, "[gldbg] quadro %ld desenho n=%d fb %08x(%dx%d f%d) prof %08x f%d -> centro %d,%d,%d,%d prof %08x erro %04x dcm %08x\n",
                g_gsp_frames, nv, f->color, f->w, f->h, f->cfmt, f->depth, f->dfmt, px[0], px[1], px[2], px[3], dp, glGetError(), R(0x107)); } }
    uint32_t dcm = R(0x107);
    if (((dcm >> 8) & 0xF) && (R(0x113) & 0xF)) { g_cur_c->dirty = 1; g_cur_c->ver++; }
    if (g_cur_d && (R(0x115) & 3) && ((dcm >> 12) & 1 || ((R(0x105) & 1) && f->dfmt == 3))) { g_cur_d->dirty = 1; g_cur_d->ver++; }
}

/* ------------------------------------------------------------ início e fim de cada lista de comandos */
int pica_gl_active(void) { return g_vk ? pica_vk_active() : g_in_list; }

void pica_gl_poll(void); static void pend_poll(int upto);
#define NPEND 16
volatile int g_force_sw;                  /* depuração: software_agora.txt troca para o renderer em software no meio do jogo */
static void pend_poll(int upto);
int pica_gl_list_begin(void) {
    if (!g_vk_decided) {                                /* primeira lista: qual renderer */
        g_vk_decided = 1;
        const char *r = ENVC("RENDER");
        if (r && (!strcmp(r, "vulkan") || !strcmp(r, "Vulkan")) && pica_vk_init()) g_vk = 1;
    }
    { static long nl, depois = -1; if (depois < 0) { const char *e = ENVC("VK_DEPOIS"); depois = e ? atol(e) : 0; }   /* (teste: troca para o Vulkan depois de N listas) */
      if (depois && !g_vk && ++nl == depois && g_gl_state == 1 && gpu_gl_begin()) {
          pend_poll(NPEND);
          for (int i = 0; i < NSURF; i++) if (g_surf[i].tex) { surf_writeback(&g_surf[i]); surf_free(&g_surf[i]); }
          gpu_gl_end();
          if (pica_vk_init()) { g_vk = 1; fprintf(stderr, "[gl] trocado para o Vulkan na lista %ld\n", nl); }
      } }
    if (g_vk) return pica_vk_list_begin();
    g_irec = 0;
    if (g_gl_state < 0) return 0;
    if (g_force_sw) {
        if (g_gl_state == 1 && gpu_gl_begin()) {   /* devolve tudo à memória uma vez e larga a GPU */
            pend_poll(NPEND);
            for (int i = 0; i < NSURF; i++) if (g_surf[i].tex) { surf_writeback(&g_surf[i]); g_surf[i].valid = 0; }
            gpu_gl_end(); g_gl_state = 2;
            fprintf(stderr, "[gl] trocado para o renderer em software\n");
        }
        return 0;
    }
    if (!gpu_gl_begin()) {
        if (!g_gl_state) { g_gl_state = -1; fprintf(stderr, "[gl] sem OpenGL: usando o renderer em software\n"); }
        return 0;
    }
    if (!g_gl_state) {
        if (!gl_setup()) { g_gl_state = -1; gpu_gl_end(); g_render_name = "software"; fprintf(stderr, "[gl] falhou: usando o renderer em software\n"); return 0; }
        g_gl_state = 1;
    }
    { static int init; if (!init) { init = 1; const char *e = ENVC("RESOLUCAO"); if (e && !g_res_req) g_res_req = atoi(e); } }
    if (g_res_req && g_res_req != g_sc && g_res_req >= 1 && g_res_req <= 9) {   /* troca de resolução interna (até 9x: 4K) */
        for (int i = 0; i < NSURF; i++) if (g_surf[i].tex) { surf_writeback(&g_surf[i]); surf_free(&g_surf[i]); }
        g_sc = g_res_req;
        interp_reset();
        fprintf(stderr, "[gl] resolução interna %dx\n", g_sc);
    }
    g_res_req = 0;
    if (g_fps60 < 0) { const char *e = ENVC("FPS"); g_fps60 = e && atoi(e) >= 60 && ENVC("FPS60_TESTE");   /* 60 fps em pausa: só para testes */ if (g_fps60) fprintf(stderr, "[gl] 60 fps: quadros intermediários ligados\n"); }
    if (g_fps60_req >= 0) {                           /* F6 */
        g_fps60 = g_fps60_req; g_fps60_req = -1; interp_reset();
        fprintf(stderr, "[gl] %d fps\n", g_fps60 ? 60 : 30);
    }
    g_irec = g_fps60 > 0;
    g_in_list = 1; g_list_serial++;
    if (!g_list_serial) g_list_serial = 1;
    pica_gl_poll();                                   /* leituras de tela que a GPU já terminou */
    return 1;
}

void pica_gl_list_end(void) {
    if (g_vk) { pica_vk_list_end(); return; }
    if (!g_in_list) return;
    static int sync = -1; if (sync < 0) sync = ENVC("GL_SYNC_FIM") != NULL;
    if (sync) for (int i = 0; i < NSURF; i++) if (g_surf[i].tex) { surf_writeback(&g_surf[i]); g_surf[i].valid = 0; }
    GLenum e = glGetError();
    static int nerr;
    if (e && nerr++ < 10) fprintf(stderr, "[gl] erro %04x\n", e);
    if (e && nerr <= 3) g_glchk = 400;                  /* liga o localizador por um tempo */
    else if (g_glchk) g_glchk--;
    gpu_gl_end();
    g_in_list = 0;
}

/* ------------------------------------------------------------ imagem da tela em alta resolução
 * A transferência de tela (gsp.c) copia o framebuffer desenhado para a memória da tela no tamanho do 3DS.
 * Com resolução interna maior, guardamos também a imagem da superfície em escala, para a janela mostrar. */
#include <pthread.h>
typedef struct { uint32_t out; int w, h; uint32_t *px; unsigned long seq; } HdShot;
static HdShot g_hd[8]; static pthread_mutex_t g_hd_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_hd_seq;
/* ---- leituras assíncronas da GPU (transferências de tela)
 * Antes, cada transferência de tela parava a thread da GPU num glReadPixels até a placa terminar de desenhar
 * o quadro inteiro (e em 4x isso era a maior parte do "GPU->mem" do perfil). Agora a leitura vai para um
 * buffer de pixels (PBO) com uma cerca (fence) e o resultado só é usado quando a placa avisa que terminou:
 * a memória da tela e a imagem em escala da janela são atualizadas alguns milissegundos depois, sempre na
 * ordem em que as transferências foram pedidas. Se alguém precisar daquela memória antes (outra cópia, DMA,
 * uso como textura), a leitura pendente é concluída na hora. */
typedef struct { int kind; uint32_t out, len; int w, h, out_fmt; GLuint pbo; GLsync fence; uint64_t t; } Pend;   /* kind 1: tela em escala; 2: memória */
static Pend g_pend[NPEND]; static int g_pend_head, g_pend_n;
/* quantas leituras pendentes escrevem em cada endereço de tela (consultado pelo VBlank, em outra thread) */
static struct { uint32_t out; int n; } g_pout[8]; static pthread_mutex_t g_pout_lock = PTHREAD_MUTEX_INITIALIZER;
volatile int g_pend_done;                            /* alguma leitura terminou (gsp.c apresenta a tela) */
/* escala em que a janela mostra cada tela (display_sdl.c; 0 = sem limite). A imagem em escala que volta da placa
 * para a janela é reduzida na própria placa até essa escala: em 2K/4K num monitor menor, desenhar grande e
 * reduzir dá a suavização (supersampling), mas copiar 3600x2160 pixels por quadro para a CPU não serve de nada. */
volatile int g_hd_need_top, g_hd_need_bot;
static void pout_add(uint32_t out, int d) {
    pthread_mutex_lock(&g_pout_lock);
    int i, f = -1;
    for (i = 0; i < 8; i++) { if (g_pout[i].out == out && g_pout[i].n) break; if (f < 0 && !g_pout[i].n) f = i; }
    if (i == 8) i = f;
    if (i >= 0) { g_pout[i].out = out; g_pout[i].n += d; if (g_pout[i].n < 0) g_pout[i].n = 0; }
    pthread_mutex_unlock(&g_pout_lock);
}
/* número de sequência da última escrita completa em cada endereço de tela (para a janela nunca voltar no tempo) */
static struct { uint32_t out; unsigned long seq; } g_oseq[8]; static unsigned long g_oseq_clock;
void pica_gl_out_mark(uint32_t out) {
    pthread_mutex_lock(&g_pout_lock);
    int k = 0;
    for (int i = 0; i < 8; i++) { if (g_oseq[i].out == out) { k = i; goto set; } if (g_oseq[i].seq < g_oseq[k].seq) k = i; }
set:
    g_oseq[k].out = out; g_oseq[k].seq = ++g_oseq_clock;
    pthread_mutex_unlock(&g_pout_lock);
}
unsigned long pica_gl_out_seq(uint32_t out) {
    unsigned long r = 0;
    pthread_mutex_lock(&g_pout_lock);
    for (int i = 0; i < 8; i++) if (g_oseq[i].out == out) r = g_oseq[i].seq;
    pthread_mutex_unlock(&g_pout_lock);
    return r;
}
int pica_gl_out_pending(uint32_t out) {
    if (!out) return 0;
    int r = 0;
    pthread_mutex_lock(&g_pout_lock);
    for (int i = 0; i < 8; i++) if (g_pout[i].n && g_pout[i].out == out) r = 1;
    pthread_mutex_unlock(&g_pout_lock);
    return r;
}
static GLuint g_pbo_free[NPEND]; static int g_npbo_free;
long g_pend_forced;                                  /* estatística: leituras concluídas à força (esperando a GPU) */
/* ---- 60 fps: fila das imagens da tela de cima na ordem em que ficaram prontas (intermediária, real, ...);
 * a apresentação (gsp.c) tira uma por VBlank */
typedef struct { uint32_t *px; int w, h, mid; } Q60;
/* verificação de cada quadro intermediário contra os dois reais em volta (amostras 64x64 de brilho): se ele
 * foge muito dos dois (tela preta no meio de um clarão, algo mal refeito), é descartado e o real repete */
#define QS 64
static uint8_t g_qs_prev[QS * QS], g_qs_mid[QS * QS]; static int g_qs_have_prev;
static void q60_samples(const uint32_t *px, int w, int h, uint8_t *o) {
    for (int j = 0; j < QS; j++) for (int i = 0; i < QS; i++) {
        uint32_t v = px[(size_t)(j * h / QS) * w + i * w / QS];
        o[j * QS + i] = (uint8_t)(((v & 0xFF) * 77 + ((v >> 8) & 0xFF) * 150 + ((v >> 16) & 0xFF) * 29) >> 8);
    }
}
long g_q60_bad;                                      /* intermediários descartados pela verificação */
static int g_q60_cool;
static volatile int g_q60_pause;
void pica_gl_q60_pause(int n) { g_q60_pause = n; }
long g_q60_suspect;                                  /* intermediários não feitos (composição por cópias) */
static Q60 g_q60[6]; static int g_q60_head, g_q60_n; static volatile long g_q60_push_vb = -1000;
long g_q60_stats[3];                                 /* intermediárias, reais, descartadas (atraso) */
volatile long g_q60_shown;                           /* imagens mostradas (fps da barra de título) */
static void q60_push(uint32_t *px, int w, int h, int mid) {
    extern long g_gsp_frames;
    uint8_t cur[QS * QS]; q60_samples(px, w, h, cur);
    pthread_mutex_lock(&g_hd_lock);
    if (mid) memcpy(g_qs_mid, cur, sizeof cur);
    else {
        static int nover = -1; if (nover < 0) nover = ENVC("Q60_SEM_VERIF") != NULL;   /* depuração */
        if (!nover && g_q60_n && g_q60[(g_q60_head + g_q60_n - 1) % 6].mid && g_qs_have_prev) {   /* o intermediário logo antes deste */
            long dm = 0, dab = 0;
            for (int k = 0; k < QS * QS; k++) { int a = g_qs_prev[k], b = cur[k], m = g_qs_mid[k];
                                                 dab += abs(a - b); dm += abs(2 * m - a - b) / 2; }
            dm /= QS * QS; dab /= QS * QS;
            long bmax = -999;                                  /* também por região (8x8 blocos): uma camada faltando num pedaço */
            for (int by = 0; by < 8; by++) for (int bx = 0; bx < 8; bx++) {
                long bm = 0, bd = 0;
                for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { int k = (by * 8 + y) * QS + bx * 8 + x; int a = g_qs_prev[k], b = cur[k], m = g_qs_mid[k];
                                                                       bd += abs(a - b); bm += abs(2 * m - a - b) / 2; }
                if (bm / 64 - bd / 64 > bmax) bmax = bm / 64 - bd / 64;
            }
            int bloco = 0; (void)bmax;                        /* (só para o log: com movimento os blocos variam muito) */
            { static int lg = -1; if (lg < 0) lg = ENVC("Q60_LOG") != NULL; if (lg) fprintf(stderr, "[q60] dm %ld dab %ld bloco %ld\n", dm, dab, bmax); }
            { static int lg = -1; if (lg < 0) lg = ENVC("Q60_LOG") != NULL; if (lg) fprintf(stderr, "[q60] dm %ld dab %ld\n", dm, dab); }
            /* depois de um descarte (cena instável, ex.: começo da batalha) os próximos ~3 s ficam sem mistura */
            /* ruim: dm > 1,5·dab + 5. Depois de um ruim, só volta a misturar quando 30 intermediários seguidos
             * passarem num teste mais exigente (dm > dab + 3): a abertura das batalhas dura o quanto durar */
            int ruim = dm > dab + 3 || bloco || g_q60_pause > 0;   /* intermediário fora do caminho entre os dois reais */
            if (g_q60_pause > 0) g_q60_pause--;
            if (ruim) g_q60_cool = 30;
            else if (g_q60_cool > 0) g_q60_cool--;
            if (ruim || g_q60_cool > 0) {
                Q60 *e = &g_q60[(g_q60_head + g_q60_n - 1) % 6]; free(e->px); g_q60_n--; g_q60_bad++;
                static int nl; if (nl++ < 10) fprintf(stderr, "[60fps] quadro intermediário descartado (diferença %ld, entre os reais %ld)\n", dm, dab);
            }
        }
        memcpy(g_qs_prev, cur, sizeof cur); g_qs_have_prev = 1;
    }
    if (g_q60_n == 6) { free(g_q60[g_q60_head].px); g_q60_head = (g_q60_head + 1) % 6; g_q60_n--; g_q60_stats[2]++; }
    Q60 *e = &g_q60[(g_q60_head + g_q60_n) % 6]; e->px = px; e->w = w; e->h = h; e->mid = mid; g_q60_n++;
    g_q60_stats[mid ? 0 : 1]++;
    g_q60_push_vb = g_gsp_frames;
    pthread_mutex_unlock(&g_hd_lock);
}
/* chamada a cada apresentação: 1 = nova imagem em *outp (400*sc x 240*sc), 0 = nada novo neste VBlank,
 * -1 = modo 60 fps parado (sem quadros pela GPU há um tempo: usa a apresentação normal) */
int pica_gl_q60_active(long vb) { return g_q60_n || vb - g_q60_push_vb <= 8; }
static volatile long g_q60_last_take = -1;
/* a imagem mais antiga pode ser mostrada: um intermediário só depois que o real seguinte chegou (e o conferiu) */
/* pausa da mistura por `n` quadros reais (a abertura das batalhas monta a tela com truques que o quadro
 * intermediário não reproduz: chamada pelo ldr.c ao carregar o módulo da batalha) */
static int q60_ready(void) { return g_q60_n && !(g_q60[g_q60_head].mid && g_q60_n == 1); }
int pica_gl_q60_new(long vb) { return q60_ready() && g_q60_last_take != vb; }   /* há imagem e este VBlank ainda não mostrou nenhuma */
int g_q60_took_mid;                    /* depuração: a última imagem tirada era intermediária */
int pica_gl_q60_take(uint32_t **outp, int *cap, int *sc_out, long vb) {
    static long last_vb = -1;
    int r = 0;
    pthread_mutex_lock(&g_hd_lock);
    if (vb != last_vb && q60_ready()) {
        while (g_q60_n > 2) { free(g_q60[g_q60_head].px); g_q60_head = (g_q60_head + 1) % 6; g_q60_n--; g_q60_stats[2]++; }   /* atrasou: pula */
        Q60 *e = &g_q60[g_q60_head];
        int sc = e->w / 240;
        if (sc >= 1 && e->h == 400 * sc) {
            int OW = 400 * sc, OH = 240 * sc;
            if (*cap < OW * OH) { *cap = OW * OH; *outp = realloc(*outp, (size_t)*cap * 4); }
            uint32_t *o = *outp;
            for (int Y = 0; Y < OH; Y++) {
                int gx = e->w - 1 - Y;
                for (int X = 0; X < OW; X++) {
                    uint32_t v = e->px[(size_t)(e->h - 1 - X) * e->w + gx];
                    o[(size_t)Y * OW + X] = ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF);
                }
            }
            *sc_out = sc; r = 1; g_q60_shown++; g_q60_took_mid = e->mid;
        }
        free(e->px); g_q60_head = (g_q60_head + 1) % 6; g_q60_n--;
        last_vb = vb; g_q60_last_take = vb;
    }
    pthread_mutex_unlock(&g_hd_lock);
    return r;
}
static void store_hd(uint32_t out, uint32_t *px, int w, int h) {
    pthread_mutex_lock(&g_hd_lock);
    HdShot *e = NULL;
    for (int i = 0; i < 8; i++) if (g_hd[i].out == out) e = &g_hd[i];
    if (!e) { e = &g_hd[0]; for (int i = 1; i < 8; i++) if (g_hd[i].seq < e->seq) e = &g_hd[i]; }
    free(e->px); e->px = px; e->out = out; e->w = w; e->h = h; e->seq = ++g_hd_seq;
    pthread_mutex_unlock(&g_hd_lock);
}
/* para o pica_vk.c: imagem em escala pronta e contagem de leituras pendentes por endereço de tela */
void pica_hd_store(uint32_t out, uint32_t *px, int w, int h) { store_hd(out, px, w, h); }
void pica_pout_add(uint32_t out, int d) { pout_add(out, d); }
static void pend_check(const char *where) {          /* depuração: erro do OpenGL nas cópias de tela */
    GLenum e = glGetError(); static int n;
    if (e && n++ < 10) fprintf(stderr, "[gl] erro %04x em %s\n", e, where);
}
/* conclui a leitura mais antiga da fila (espera a GPU se ainda não terminou) */
static void pend_finish_oldest(void) {
    Pend *p = &g_pend[g_pend_head];
    pend_check("antes de concluir cópia");
    glBindBuffer(GL_PIXEL_PACK_BUFFER, p->pbo);
    const uint8_t *m = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)p->w * p->h * 4, GL_MAP_READ_BIT);
    if (m && (p->kind == 1 || p->kind == 3 || p->kind == 4)) {   /* 3: quadro intermediário, 4: quadro real (60 fps) */
        uint32_t *px = malloc((size_t)p->w * p->h * 4);
        memcpy(px, m, (size_t)p->w * p->h * 4);
        if (p->kind != 1) { uint32_t *q = p->kind == 4 ? malloc((size_t)p->w * p->h * 4) : px;
                            if (q != px) memcpy(q, px, (size_t)p->w * p->h * 4);
                            q60_push(q, p->w, p->h, p->kind == 3); }
        if (p->kind != 3) store_hd(p->out, px, p->w, p->h);
    } else if (m) {                                  /* memória da tela: linear, linha y = linha h-1-y lida */
        uint32_t bpp = p->out_fmt == 0 ? 4 : p->out_fmt == 1 ? 3 : 2;
        plat_touch(g_mem + p->out, p->len);
        for (int y = 0; y < p->h; y++) {
            const uint8_t *row = m + (size_t)(p->h - 1 - y) * p->w * 4;
            uint8_t *d = g_mem + p->out + (size_t)y * p->w * bpp;
            for (int x = 0; x < p->w; x++, row += 4, d += bpp) {
                uint8_t r = row[0], g = row[1], b = row[2];
                switch (p->out_fmt) {
                case 0: d[0] = 255; d[1] = b; d[2] = g; d[3] = r; break;
                case 1: d[0] = b; d[1] = g; d[2] = r; break;
                case 2: { uint16_t v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3; d[0] = v; d[1] = v >> 8; break; }
                case 3: { uint16_t v = (r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | 1; d[0] = v; d[1] = v >> 8; break; }
                default: { uint16_t v = (r >> 4) << 12 | (g >> 4) << 8 | (b >> 4) << 4 | 15; d[0] = v; d[1] = v >> 8; break; }
                }
            }
        }
        tex_invalidate_range(p->out, p->len);
        pica_gl_out_mark(p->out);
    }
    if (m) glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glDeleteSync(p->fence);
    if (p->out) pout_add(p->out, -1);
    g_pend_done = 1;
    pend_check(p->kind == 2 ? "cópia da memória" : "cópia em escala");
    if (g_npbo_free < NPEND) g_pbo_free[g_npbo_free++] = p->pbo; else glDeleteBuffers(1, &p->pbo);
    g_pend_head = (g_pend_head + 1) % NPEND; g_pend_n--;
}
/* conclui, em ordem, as leituras que a GPU já terminou (e, com `upto`, todas até essa posição da fila) */
static void pend_poll(int upto) {
    while (g_pend_n) {
        Pend *p = &g_pend[g_pend_head];
        if (upto <= 0) {
            GLenum r = glClientWaitSync(p->fence, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
            if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) break;
            { static long atraso = -1; if (atraso < 0) atraso = ENVC("GL_ATRASO_LEITURA") ? atol(ENVC("GL_ATRASO_LEITURA")) : 0;   /* teste: GPU lenta */
              extern uint64_t k_now_ns(void); if (atraso && k_now_ns() - p->t < (uint64_t)atraso * 1000000) break; }
        } else g_pend_forced++;
        pend_finish_oldest();
        upto--;
    }
}
/* alguém vai ler ou escrever [a, a+len): conclui antes as leituras pendentes que escrevem ali */
static void pend_resolve_range(uint32_t a, uint32_t len) {
    if (!g_pend_n) return;
    int last = -1;
    for (int k = 0; k < g_pend_n; k++) { Pend *p = &g_pend[(g_pend_head + k) % NPEND]; if (p->kind == 2 && overlap(p->out, p->len, a, len)) last = k; }
    if (last < 0) return;
    int c = ctx_enter(); if (!c) return;
    pend_poll(last + 1);
    ctx_leave(c);
}
static void pend_push(int kind, uint32_t out, uint32_t len, int w, int h, int out_fmt, GLuint fbo, int x, int y) {
    if (g_pend_n == NPEND) pend_poll(1);
    pend_check("antes de pedir cópia");
    Pend *p = &g_pend[(g_pend_head + g_pend_n) % NPEND];
    p->kind = kind; p->out = out; p->len = len; p->w = w; p->h = h; p->out_fmt = out_fmt;
    if (g_npbo_free) p->pbo = g_pbo_free[--g_npbo_free]; else glGenBuffers(1, &p->pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, p->pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)w * h * 4, NULL, GL_STREAM_READ);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, (void *)0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    p->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    GLCHK(kind == 2 ? "cópia da tela para a memória" : kind == 3 ? "cópia do quadro intermediário" : "cópia da tela em escala");
    if (out) pout_add(out, +1);
    pend_check(kind == 2 ? "pedido de cópia da memória" : "pedido de cópia em escala");
    { extern uint64_t k_now_ns(void); p->t = k_now_ns(); }
    g_pend_n++;
}
void pica_gl_poll(void) {                           /* chamada a cada lista da GPU (com o contexto ativo) */
    if (g_vk) { pica_vk_poll(); return; }
    if (g_pend_n) pend_poll(0);
}
int pica_gl_pending(void) { return g_vk ? pica_vk_pending() : g_pend_n; }
/* thread da GPU sem comandos: espera até `us` microssegundos pela leitura mais antiga e conclui as prontas */
void pica_gl_pending_wait(int us) {
    if (g_vk) { pica_vk_pending_wait(us); return; }
    if (!g_pend_n) return;
    int c = ctx_enter(); if (!c) return;
    glClientWaitSync(g_pend[g_pend_head].fence, GL_SYNC_FLUSH_COMMANDS_BIT, (GLuint64)us * 1000);
    pend_poll(0);
    ctx_leave(c);
}
/* depois da cópia normal (síncrona) de uma transferência: a GPU já terminou, então a imagem em escala fica
 * pronta junto com a memória da tela (a janela nunca mostra uma imagem em escala de outro quadro) */
void pica_gl_finish_hd(void) {
    if (g_vk) { pica_vk_finish_hd(); return; }
    if (!g_pend_n) return;
    int c = ctx_enter(); if (!c) return;
    while (g_pend_n) pend_finish_oldest();
    ctx_leave(c);
}
/* uma transferência da tela de cima que não passou pelo quadro intermediário (cópia diferente): recomeça
 * a gravação, para o próximo quadro não se misturar com este */
static int g_top60_done;
void pica_gl_top_transfer_done(void) {
    g_os_epoch++;
    { static int nq; if (ENVC("GL_ALARGAR_LOG") && ++nq % 20 == 0) { fprintf(stderr, "[gl] bordas: %ld vistas, %ld empurradas, %.1f ms por quadro\n", g_os_nb, g_os_np, g_os_ns / 20.0 / 1e6); g_os_nb = g_os_np = 0; g_os_ns = 0; } }
    if (g_fps60 > 0 && !g_top60_done) interp_reset();
    g_top60_done = 0;
}
/* operações gravadas além dos desenhos, refeitas no quadro intermediário (nas cópias das superfícies) */
static int replay_op(int kind, const uint32_t p[8]) {
    if (kind == 1) {                                  /* preenchimento de uma superfície inteira */
        for (int i = 0; i < NSURF; i++) {
            Surf *s = &g_surf[i];
            if (s->tex && s->addr == p[0] && s->addr + s->size == p[1]) { surf_clear(shadow_of(s, 0), p[2]); break; }
        }
        return 1;
    }
    if (kind == 3) {                                  /* o jogo copiou algo para a superfície (captura de tela das transições):
                                                         * a partir daqui vale o conteúdo real dela, não a cópia de trabalho */
        for (int i = 0; i < NSURF; i++)
            if (g_surf[i].tex && overlap(g_surf[i].addr, g_surf[i].size, p[0], p[1])) g_shadow_stamp[i] = 0;
        return 1;
    }
    if (kind == 2) {                                  /* a tela de cima: lê a imagem intermediária */
        uint32_t in = p[0]; int iw = (int)p[1], ow = (int)p[4], oh = (int)p[5], sy0 = (int)p[6];
        for (int i = 0; i < NSURF; i++) {
            Surf *s = &g_surf[i];
            if (!s->tex || !s->valid || s->depth || s->fmt != 0 || in < s->addr || in >= s->addr + s->size || s->w != iw) continue;
            if (g_shadow_stamp[i] != g_replay_id || g_shadow_drawn[i] != g_replay_id) return 0;   /* nada foi desenhado nela: não há quadro novo */
            if (g_replay_suspect) g_q60_suspect++;              /* (só estatística: a verificação de imagem decide) */
            { GLenum e = glGetError();                          /* algo falhou ao refazer o quadro: não mostra (ficava preto) */
              if (e) { static int nl; if (nl++ < 10) fprintf(stderr, "[60fps] quadro intermediário descartado (erro %04x do OpenGL)\n", e);
                       while (glGetError()) continue;
                       return 0; } }
            Surf *h = &g_shadow[i];
            pend_push(3, 0, 0, ow * h->sc, oh * h->sc, 0, fbo_get(h->tex, 0), 0, (h->h - sy0 - oh) * h->sc);
            return 1;
        }
    }
    return 0;
}
/* transferência de tela feita na GPU: bit 0 = memória da tela será escrita quando a GPU terminar (o GSP não
 * copia), bit 1 = imagem em escala para a janela. 0: o GSP faz a cópia normal. */
/* reduz na placa a região (sx, sy, sw x sh) do framebuffer `fb` para W x H: metades sucessivas com filtro linear
 * (média de 2x2) e o último passo direto. Devolve o framebuffer com o resultado em (0,0). */
static GLuint gl_reduzir(GLuint fb, int sx, int sy, int sw, int sh, int W, int H) {
    glDisable(GL_SCISSOR_TEST);
    for (;;) {
        int last = !(sw > 2 * W && sh > 2 * H), nw = last ? W : sw / 2, nh = last ? H : sh / 2;
        GLuint t = fbo_get(native_tmp(nw, nh, 0), 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t);
        glBlitFramebuffer(sx, sy, sx + sw, sy + sh, 0, 0, nw, nh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        fb = t; sx = sy = 0; sw = nw; sh = nh;
        if (last) break;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    GLCHK("redução da tela em escala");
    return fb;
}
int pica_gl_transfer(uint32_t in, int iw, int ih, uint32_t out, int ow, int oh, uint32_t flags, int in_fmt, int out_fmt) {
    if (g_vk) return pica_vk_transfer(in, iw, ih, out, ow, oh, flags, in_fmt, out_fmt);
    /* a memória da tela é escrita quando a GPU terminar; a janela não mostra uma tela com cópia pendente
     * (gsp.c, present) e a apresenta assim que a cópia termina. GL_LEITURA_SINCRONA=1 volta à cópia na hora. */
    static int off = -1; if (off < 0) off = ENVC("GL_LEITURA_SINCRONA") != NULL;
    if (flags & 0x03000023) return 0;                /* só a cópia simples (blocos -> linear), sem redução nem inversão */
    if (in_fmt != 0 || out_fmt > 4) return 0;
    Surf *s = NULL; int sy0 = 0;
    for (int i = 0; i < NSURF; i++) {                /* a entrada pode começar no meio da superfície (256x512 -> 240x400) */
        Surf *q = &g_surf[i];
        if (!q->tex || !q->valid || q->depth || q->fmt != 0 || in < q->addr || in >= q->addr + q->size || q->w != iw) continue;
        uint32_t off2 = in - q->addr, trow = (uint32_t)q->w * 8 * 4;
        if (off2 % trow) continue;
        sy0 = (int)(off2 / trow) * 8; s = q;
    }
    if (!s || ow > s->w || sy0 + oh > s->h) return 0;
    int c = ctx_enter(); if (!c) return 0;
    if (g_pend_n) pend_poll(0);
    int ret = 0;
    GLint prev = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    uint32_t len = (uint32_t)ow * oh * (out_fmt == 0 ? 4 : out_fmt == 1 ? 3 : 2);
    extern int gsp_is_top_fb(uint32_t);
    int top60 = g_fps60 > 0 && oh == 400 && ow == 240 && gsp_is_top_fb(out);
    if (top60) g_top60_done = 1;
    if (top60) {                                      /* fim de um quadro da tela de cima: desenha o intermediário */
        uint32_t p[8] = { in, (uint32_t)iw, (uint32_t)ih, out, (uint32_t)ow, (uint32_t)oh, (uint32_t)sy0, 0 };
        interp_rec_op(2, p);
        uint64_t t0 = prof_now();
        { int k = 0; while (glGetError() && k++ < 8) continue; }        /* (erros de antes não são do quadro intermediário) */
        int was = g_in_list; g_in_list = 1; g_replay = 1; g_os_epoch++; g_replay_suspect = 0; g_replay_id++; if (!g_replay_id) g_replay_id = 1;
        interp_replay(0.5f, replay_op);
        g_replay = 0; g_os_epoch++; g_in_list = was; g_cur_c = g_cur_d = NULL;
        interp_frame_swap();
        g_prof_ns[6] += prof_now() - t0;
    }
    if (s->sc > 1 || top60) {                         /* imagem em escala para a janela */
        int osc = s->sc;                              /* escala da imagem que volta: a da janela, se for menor */
        if (!top60 && !ENVC("GL_SEM_REDUCAO")) { int need = oh == 400 ? g_hd_need_top : g_hd_need_bot; if (need > 0) { if (need < 2) need = 2; if (need < osc) osc = need; } }
        GLuint fb = fbo_get(s->tex, 0); int y = (s->h - sy0 - oh) * s->sc;
        if (osc < s->sc) { fb = gl_reduzir(fb, 0, y, ow * s->sc, oh * s->sc, ow * osc, oh * osc); y = 0; }
        pend_push(top60 ? 4 : 1, out, 0, ow * osc, oh * osc, 0, fb, 0, y);
        ret |= 2;
    }
    /* memória da tela, em tamanho nativo; não vale se a saída for uma superfície da GPU */
    if (!off && !any_overlap(out, len, 0)) {
        pend_resolve_range(out, len);                 /* (escritas anteriores no mesmo lugar primeiro) */
        GLuint src = s->tex;
        if (s->sc > 1) { src = native_tmp(s->w, s->h, 0); scale_blit(s, src, 0); }
        pend_push(2, out, len, ow, oh, out_fmt, fbo_get(src, 0), 0, s->h - sy0 - oh);
        ret |= 1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev);
    ctx_leave(c);
    return ret;
}
/* a CPU escreveu na memória da tela sem passar por aqui: a imagem em escala deixa de valer */
void pica_gl_hd_forget(uint32_t out) {
    pthread_mutex_lock(&g_hd_lock);
    for (int i = 0; i < 8; i++) if (g_hd[i].out == out) g_hd[i].out = 0;
    pthread_mutex_unlock(&g_hd_lock);
}
/* tela de cima em escala (400*sc x 240*sc, 0x00RRGGBB) a partir do framebuffer em `addr` (240 x 400 na
 * memória, girado como no 3DS). Devolve a escala ou 0 se não houver imagem em escala para esse endereço. */
int pica_gl_hd_screen(uint32_t addr, int width, uint32_t **outp, int *cap, int *changed) {
    int got = 0;
    static unsigned long seqs[2]; static uint32_t addrs[2];   /* por tela: 0 = de cima (400), 1 = de baixo (320) */
    int scr = width == 400 ? 0 : 1;
    unsigned long last_seq = seqs[scr]; uint32_t last_addr = addrs[scr];
    *changed = 0;
    pthread_mutex_lock(&g_hd_lock);
    for (int i = 0; i < 8; i++) {
        HdShot *e = &g_hd[i];
        if (e->out != addr || !e->px || e->w % 240 || e->h % width) continue;
        int sc = e->w / 240; if (e->h != width * sc) continue;
        if (e->seq == last_seq && addr == last_addr && *outp) { got = sc; break; }   /* mesma imagem: nada a refazer */
        if (e->seq < last_seq && addr != last_addr && *outp) { got = sc; break; }    /* imagem mais velha que a mostrada: mantém */
        { static int lg = -1; if (lg < 0) lg = ENVC("GL_PRESENT_LOG") != NULL;
          if (lg) fprintf(stderr, "[present] %08x seq %lu%s\n", addr, e->seq, e->seq < last_seq ? " VOLTOU" : ""); }
        seqs[scr] = e->seq; addrs[scr] = addr; *changed = 1;
        { static int ult[2]; if (ult[scr] != sc) { ult[scr] = sc; fprintf(stderr, "[gl] tela %s chega na janela em %dx (%dx%d)\n", scr ? "de baixo" : "de cima", sc, width * sc, 240 * sc); } }
        int OW = width * sc, OH = 240 * sc;
        if (*cap < OW * OH) { *cap = OW * OH; *outp = realloc(*outp, (size_t)*cap * 4); }
        uint32_t *o = *outp;
        /* tela(X, Y) = framebuffer(linha X, coluna 239 - Y); na textura: coluna -> x, linha -> h - 1 - y.
         * Em blocos de 32x32: girar lendo coluna por coluna de uma imagem grande (2K/4K) é lento para a memória */
        for (int Y0 = 0; Y0 < OH; Y0 += 32)
            for (int X0 = 0; X0 < OW; X0 += 32) {
                int Y1 = Y0 + 32 < OH ? Y0 + 32 : OH, X1 = X0 + 32 < OW ? X0 + 32 : OW;
                for (int X = X0; X < X1; X++) {
                    const uint32_t *src = e->px + (size_t)(e->h - 1 - X) * e->w + (e->w - 1);
                    for (int Y = Y0; Y < Y1; Y++) {
                        uint32_t v = src[-Y];                       /* RGBA em bytes: R G B A */
                        o[(size_t)Y * OW + X] = ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF);
                    }
                }
            }
        got = sc; break;
    }
    pthread_mutex_unlock(&g_hd_lock);
    return got;
}

/* chamado pelo cão de guarda do kernel (sem travas, só leitura) */
void pica_vk_debug_dump(void);
void pica_debug_dump(void) {                /* chamado pelo cão de guarda do kernel */
    extern void gsp_debug_dump(void); gsp_debug_dump();
    fprintf(stderr, "[watchdog] renderer %s, em lista %d, cópias pendentes %d\n", g_vk ? "vulkan" : "opengl", g_in_list, g_pend_n);
    if (g_vk) pica_vk_debug_dump();
}

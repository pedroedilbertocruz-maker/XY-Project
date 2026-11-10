#version 450
/* (sem "switch": o SwiftShader trava compilando switch com return; os "if" fazem o mesmo) */
layout(location=0) in vec4 v_color; layout(location=1) in vec4 v_tc01; layout(location=2) in vec3 v_tc2; layout(location=3) in vec4 v_quat; layout(location=4) in vec3 v_view;
layout(location=0) out vec4 o_color;
/* teste de alfa: só os pipelines dos desenhos que o usam têm o "discard". Sem ele no shader, a placa faz o teste de
 * profundidade antes de rodar o shader (early-Z) e não gasta o shader inteiro em pixels que vão ficar escondidos */
layout(constant_id = 0) const bool c_teste_alfa = true;
/* versão especializada (compilada em segundo plano para cada configuração de material): a configuração dos 6
 * estágios de cor, das texturas e da iluminação vira constante e o compilador da placa elimina as escolhas */
layout(constant_id = 1) const bool c_esp = false;
layout(constant_id = 2) const uint cS0 = 0u;
layout(constant_id = 3) const uint cO0 = 0u;
layout(constant_id = 4) const uint cC0 = 0u;
layout(constant_id = 5) const uint cE0 = 0u;
layout(constant_id = 6) const uint cS1 = 0u;
layout(constant_id = 7) const uint cO1 = 0u;
layout(constant_id = 8) const uint cC1 = 0u;
layout(constant_id = 9) const uint cE1 = 0u;
layout(constant_id = 10) const uint cS2 = 0u;
layout(constant_id = 11) const uint cO2 = 0u;
layout(constant_id = 12) const uint cC2 = 0u;
layout(constant_id = 13) const uint cE2 = 0u;
layout(constant_id = 14) const uint cS3 = 0u;
layout(constant_id = 15) const uint cO3 = 0u;
layout(constant_id = 16) const uint cC3 = 0u;
layout(constant_id = 17) const uint cE3 = 0u;
layout(constant_id = 18) const uint cS4 = 0u;
layout(constant_id = 19) const uint cO4 = 0u;
layout(constant_id = 20) const uint cC4 = 0u;
layout(constant_id = 21) const uint cE4 = 0u;
layout(constant_id = 22) const uint cS5 = 0u;
layout(constant_id = 23) const uint cO5 = 0u;
layout(constant_id = 24) const uint cC5 = 0u;
layout(constant_id = 25) const uint cE5 = 0u;
layout(constant_id = 26) const uint cUpd = 0u; layout(constant_id = 27) const uint cTcfg = 0u;
layout(constant_id = 28) const uint cLuz = 0u; layout(constant_id = 29) const uint cBic = 0u;
#define TV(c, i) (c_esp ? (c) : uR(i))
layout(set=0, binding=1) uniform sampler2D u_tex0; layout(set=0, binding=2) uniform sampler2D u_tex1; layout(set=0, binding=3) uniform sampler2D u_tex2; layout(set=0, binding=4) uniform sampler2D u_lut;
layout(std430, set=0, binding=0) readonly buffer U { uint uR_[64]; vec4 uL4[49]; vec2 uLD_[8]; vec2 uFog_[128]; };
#define uR(i) uR_[i]
#define uL(i) uL4[i].xyz
#define uLD(i) uLD_[i]
#define uFog(i) uFog_[i]
ivec4 q8(vec4 c) { return ivec4(clamp(floor(c * 255.0 + 0.5), 0.0, 255.0)); }
float lutv(int table, float x, bool isabs) {
  int idx; float delta;
  if (isabs) { x = abs(x); float f = clamp(floor(x * 256.0), 0.0, 255.0); idx = int(f); delta = x * 256.0 - f; }
  else { float f = clamp(floor(x * 128.0), -128.0, 127.0); delta = x * 128.0 - f; idx = int(f) & 255; }
  vec2 e = texelFetch(u_lut, ivec2(idx, table), 0).rg;
  return e.x + e.y * delta;
}
bool supported(int s) {
  int c = int(uR(36));
  if (c == 0) return s == 0 || s == 6 || s == 2;
  if (c == 1) return s == 6 || s == 2 || s == 3;
  if (c == 2) return s == 0 || s == 1 || s == 6;
  if (c == 3) return s == 0 || s == 1 || s == 3;
  if (c == 4) return s != 3;
  if (c == 5) return s != 1;
  if (c == 6) return s != 5 && s != 4;
  return true;
}
bool lut_enabled(int s) {
  if (s == 2) return true;
  int bit = s == 0 ? 16 : s == 1 ? 17 : s == 3 ? 19 : s == 4 ? 20 : s == 5 ? 21 : 22;
  return ((uR(42) >> uint(bit)) & 1u) == 0u && supported(s);
}
float scale_of(int s) {
  uint v = (uR(45) >> uint(4 * s)) & 7u;
  return v == 1u ? 2.0 : v == 2u ? 4.0 : v == 3u ? 8.0 : v == 6u ? 0.25 : v == 7u ? 0.5 : 1.0;
}
float lsample(int s, int table, vec3 n, vec3 h, vec3 v, vec3 l, vec3 spot, vec3 tangent) {
  float x; uint inp = (uR(44) >> uint(4 * s)) & 7u;
  if (inp == 0u) x = dot(n, h); else if (inp == 1u) x = dot(v, h); else if (inp == 2u) x = dot(n, v);
  else if (inp == 3u) x = dot(l, n); else if (inp == 4u) x = dot(-l, spot);
  else { vec3 hp = h - n * dot(n, h); x = dot(hp, tangent); }
  bool isabs = ((uR(43) >> uint(4 * s + 1)) & 1u) == 0u;
  return scale_of(s) * lutv(table, x, isabs);
}
vec3 qrot(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + v * q.w); }
vec3 nrm(vec3 a) { float l = length(a); return l > 0.0 ? a / l : a; }
ivec4 g_tex[3]; ivec4 g_prim, g_lp, g_ls, g_buf, g_konst, g_prev;
void lighting() {
  vec4 q = v_quat; float ql = length(q); q = ql > 0.0 ? q / ql : vec4(0.0, 0.0, 0.0, 1.0);
  vec3 surf = vec3(0.0, 0.0, 1.0), surf_t = vec3(1.0, 0.0, 0.0);
  int bm = int(uR(39));
  if (bm == 1 || bm == 2) {
    int bs = int(uR(40)); ivec4 t = g_tex[bs < 3 ? bs : 0];
    vec3 p = vec3(t.rgb) / 127.5 - 1.0;
    if (bm == 1) { if (uR(41) != 0u) { float z2 = 1.0 - p.x * p.x - p.y * p.y; p.z = z2 > 0.0 ? sqrt(z2) : 0.0; } surf = p; }
    else surf_t = p;
  }
  vec3 n = nrm(qrot(q, surf)), tangent = nrm(qrot(q, surf_t));
  vec3 view = v_view, nv = nrm(view);
  vec3 dsum = vec3(0.0), ssum = vec3(0.0); float dalpha = 1.0, salpha = 1.0;
  int nl = int(uR(35));
  for (int i = 0; i < 8; i++) {
    if (i >= nl) break;
    uint fl = uR(46 + i);
    bool directional = (fl & 1u) != 0u, two = (fl & 2u) != 0u, g0 = (fl & 4u) != 0u, g1 = (fl & 8u) != 0u;
    int num = int((fl >> 8u) & 7u);
    vec3 spec0 = uL(i * 6), spec1 = uL(i * 6 + 1), diff = uL(i * 6 + 2), amb = uL(i * 6 + 3), pos = uL(i * 6 + 4), spt = uL(i * 6 + 5);
    vec3 l = directional ? nrm(pos) : nrm(pos + view);
    vec3 h = nrm(nv + l);
    float ln = dot(l, n);
    float dp = two ? abs(ln) : max(ln, 0.0);
    float clampf = (uR(38) != 0u && ln <= 0.0) ? 0.0 : 1.0;
    float geo = 1.0;
    if (g0 || g1) { vec3 s = nv + l; float hh = dot(s, s); geo = hh == 0.0 ? 0.0 : min(dp / hh, 1.0); }
    float da = 1.0;
    if (((uR(42) >> uint(24 + num)) & 1u) == 0u && !directional) {
      float d = length(view + pos); float x = clamp(d * uLD(i).y + uLD(i).x, 0.0, 1.0); da = lutv(16 + num, x, true);
    }
    float spot = 1.0;
    if (((uR(42) >> uint(8 + num)) & 1u) == 0u && supported(2)) spot = lsample(2, 8 + num, n, h, nv, l, spt, tangent);
    float d0 = lut_enabled(0) ? lsample(0, 0, n, h, nv, l, spt, tangent) : 1.0;
    vec3 s0 = spec0 * (d0 * (g0 ? geo : 1.0));
    vec3 refl;
    refl.x = lut_enabled(6) ? lsample(6, 6, n, h, nv, l, spt, tangent) : 1.0;
    refl.y = lut_enabled(5) ? lsample(5, 5, n, h, nv, l, spt, tangent) : refl.x;
    refl.z = lut_enabled(4) ? lsample(4, 4, n, h, nv, l, spt, tangent) : refl.x;
    float d1 = lut_enabled(1) ? lsample(1, 1, n, h, nv, l, spt, tangent) : 1.0;
    vec3 s1 = spec1 * refl * (d1 * (g1 ? geo : 1.0));
    if (lut_enabled(3) && uR(37) != 0u) {
      float fr = lsample(3, 3, n, h, nv, l, spt, tangent);
      if ((uR(37) & 1u) != 0u) dalpha = fr;
      if ((uR(37) & 2u) != 0u) salpha = fr;
    }
    float att = da * spot;
    dsum += (diff * dp + amb) * att;
    ssum += (s0 + s1) * (clampf * att);
  }
  dsum += uL(48);
  g_lp = q8(vec4(dsum, dalpha)); g_ls = q8(vec4(ssum, salpha));
}
ivec4 pick(uint s) {
  if (s == 0u) return g_prim; if (s == 1u) return g_lp; if (s == 2u) return g_ls;
  if (s == 3u) return g_tex[0]; if (s == 4u) return g_tex[1]; if (s == 5u) return g_tex[2];
  if (s == 13u) return g_buf; if (s == 14u) return g_konst; if (s == 15u) return g_prev;
  return ivec4(0);
}
ivec3 cmod(uint m, ivec4 s) {
  ivec3 v = m < 2u ? s.rgb : m < 4u ? ivec3(s.a) : m < 6u ? ivec3(s.r) : (m == 8u || m == 9u) ? ivec3(s.g) : (m == 12u || m == 13u) ? ivec3(s.b) : s.rgb;
  bool inv = m == 1u || m == 3u || m == 5u || m == 9u || m == 13u;
  return inv ? 255 - v : v;
}
int amod(uint m, ivec4 s) {
  int v = m < 2u ? s.a : m < 4u ? s.r : m < 6u ? s.g : s.b;
  return (m & 1u) != 0u ? 255 - v : v;
}
ivec3 comb3(uint op, ivec3 a, ivec3 b, ivec3 c) {
  if (op == 1u) return a * b / 255; if (op == 2u) return clamp(a + b, 0, 255);
  if (op == 3u) return clamp(a + b - 128, 0, 255); if (op == 4u) return (a * c + b * (255 - c)) / 255;
  if (op == 5u) return clamp(a - b, 0, 255); if (op == 8u) return clamp(a * b / 255 + c, 0, 255);
  if (op == 9u) return clamp(a + b, 0, 255) * c / 255; return a;
}
int comb1(uint op, int a, int b, int c) {
  if (op == 1u) return a * b / 255; if (op == 2u) return clamp(a + b, 0, 255);
  if (op == 3u) return clamp(a + b - 128, 0, 255); if (op == 4u) return (a * c + b * (255 - c)) / 255;
  if (op == 5u) return clamp(a - b, 0, 255); if (op == 8u) return clamp(a * b / 255 + c, 0, 255);
  if (op == 9u) return clamp(a + b, 0, 255) * c / 255; return a;
}
ivec4 unpk8(uint v) { return ivec4(int(v & 255u), int((v >> 8u) & 255u), int((v >> 16u) & 255u), int(v >> 24u)); }
bool cmp(uint f, int a, int r) {
  if (f == 0u) return false; if (f == 1u) return true; if (f == 2u) return a == r; if (f == 3u) return a != r;
  if (f == 4u) return a < r; if (f == 5u) return a <= r; if (f == 6u) return a > r; return a >= r;
}
/* bicúbico (Catmull-Rom em 9 leituras bilineares); com a textura reduzida (vários texels por pixel) volta ao normal */
vec4 tex_bc(sampler2D t, vec2 uv) {
  vec2 sz = vec2(textureSize(t, 0)); vec2 d = fwidth(uv * sz);
  if (max(d.x, d.y) > 1.0) return texture(t, uv);
  vec2 p = uv * sz - 0.5; vec2 tp = floor(p) + 0.5; vec2 f = p - floor(p);
  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
  vec2 w12 = w1 + w2, o12 = w2 / w12;
  vec2 t0 = (tp - 1.0) / sz, t3 = (tp + 2.0) / sz, t12 = (tp + o12) / sz;
  vec4 r = vec4(0.0);
  r += texture(t, vec2(t0.x, t0.y)) * w0.x * w0.y + texture(t, vec2(t12.x, t0.y)) * w12.x * w0.y + texture(t, vec2(t3.x, t0.y)) * w3.x * w0.y;
  r += texture(t, vec2(t0.x, t12.y)) * w0.x * w12.y + texture(t, vec2(t12.x, t12.y)) * w12.x * w12.y + texture(t, vec2(t3.x, t12.y)) * w3.x * w12.y;
  r += texture(t, vec2(t0.x, t3.y)) * w0.x * w3.y + texture(t, vec2(t12.x, t3.y)) * w12.x * w3.y + texture(t, vec2(t3.x, t3.y)) * w3.x * w3.y;
  return clamp(r, 0.0, 1.0);
}
vec4 tex0(vec2 uv) { return (TV(cBic, 59) & 1u) != 0u ? tex_bc(u_tex0, uv) : texture(u_tex0, uv); }
vec4 tex1(vec2 uv) { return (TV(cBic, 59) & 2u) != 0u ? tex_bc(u_tex1, uv) : texture(u_tex1, uv); }
vec4 tex2(vec2 uv) { return (TV(cBic, 59) & 4u) != 0u ? tex_bc(u_tex2, uv) : texture(u_tex2, uv); }
/* um estágio de combinação de cor (TEV) */
void estagio(int st, uint src, uint opnd, uint cmb, uint scale, uint upd, inout ivec4 nbuf) {
  g_konst = unpk8(uR(st * 5 + 3));
  uint cop = cmb & 15u, aop = (cmb >> 16u) & 15u;
  int cs = 1 << int(scale & 3u), as = 1 << int((scale >> 16u) & 3u);
  ivec3 c0 = cmod(opnd & 15u, pick(src & 15u)), c1 = cmod((opnd >> 4u) & 15u, pick((src >> 4u) & 15u)), c2 = cmod((opnd >> 8u) & 15u, pick((src >> 8u) & 15u));
  int a0 = amod((opnd >> 12u) & 7u, pick((src >> 16u) & 15u)), a1 = amod((opnd >> 16u) & 7u, pick((src >> 20u) & 15u)), a2 = amod((opnd >> 20u) & 7u, pick((src >> 24u) & 15u));
  ivec4 res;
  if (cop == 6u || cop == 7u) {
    int d = 0;
    for (int k = 0; k < 3; k++) d += int(trunc(float((c0[k] * 2 - 255) * (c1[k] * 2 - 255) + 128) / 256.0));
    d = clamp(d, 0, 255);
    res.rgb = ivec3(clamp(d * cs, 0, 255));
    res.a = cop == 7u ? d : clamp(comb1(aop, a0, a1, a2) * as, 0, 255);
  } else {
    res.rgb = clamp(comb3(cop, c0, c1, c2) * cs, 0, 255);
    res.a = clamp(comb1(aop, a0, a1, a2) * as, 0, 255);
  }
  g_buf = nbuf;
  if (st < 4) {
    if (((upd >> uint(8 + st)) & 1u) != 0u) nbuf.rgb = res.rgb;
    if (((upd >> uint(12 + st)) & 1u) != 0u) nbuf.a = res.a;
  }
  g_prev = res;
}
void main() {
  g_prim = q8(v_color);
  uint tcfg = TV(cTcfg, 32);
  g_tex[0] = ivec4(0); g_tex[1] = ivec4(0); g_tex[2] = ivec4(0);
  if ((tcfg & 1u) != 0u) { vec2 t0 = v_tc01.xy; if (((uR(58) >> 28u) & 7u) == 3u) t0 /= v_tc2.z;
    g_tex[0] = q8(tex0(vec2(t0.x, uR(54) != 0u ? t0.y : 1.0 - t0.y))); }
  if ((tcfg & 2u) != 0u) g_tex[1] = q8(tex1(vec2(v_tc01.z, uR(55) != 0u ? v_tc01.w : 1.0 - v_tc01.w)));
  if ((tcfg & 4u) != 0u) { vec2 t = ((tcfg >> 13u) & 1u) != 0u ? v_tc01.zw : v_tc2.xy; g_tex[2] = q8(tex2(vec2(t.x, uR(56) != 0u ? t.y : 1.0 - t.y))); }
  g_lp = ivec4(0); g_ls = ivec4(0);
  if (TV(cLuz, 34) != 0u) lighting();
  ivec4 nbuf = unpk8(uR(30)); g_buf = nbuf; g_prev = ivec4(0);
  uint upd = TV(cUpd, 31);
  estagio(0, TV(cS0, 0), TV(cO0, 1), TV(cC0, 2), TV(cE0, 4), upd, nbuf);
  estagio(1, TV(cS1, 5), TV(cO1, 6), TV(cC1, 7), TV(cE1, 9), upd, nbuf);
  estagio(2, TV(cS2, 10), TV(cO2, 11), TV(cC2, 12), TV(cE2, 14), upd, nbuf);
  estagio(3, TV(cS3, 15), TV(cO3, 16), TV(cC3, 17), TV(cE3, 19), upd, nbuf);
  estagio(4, TV(cS4, 20), TV(cO4, 21), TV(cC4, 22), TV(cE4, 24), upd, nbuf);
  estagio(5, TV(cS5, 25), TV(cO5, 26), TV(cC5, 27), TV(cE5, 29), upd, nbuf);
  if ((upd & 7u) == 5u) {
    float fz = ((upd >> 16u) & 1u) != 0u ? 1.0 - gl_FragCoord.z : gl_FragCoord.z;
    float fi = fz * 128.0, fl = clamp(floor(fi), 0.0, 127.0);
    vec2 fe = uFog(int(fl));
    float ff = clamp(fe.x + fe.y * (fi - fl), 0.0, 1.0);
    vec3 fc = vec3(float(uR(57) & 255u), float((uR(57) >> 8u) & 255u), float((uR(57) >> 16u) & 255u));
    g_prev.rgb = ivec3(round(mix(fc, vec3(g_prev.rgb), ff)));
  }
  uint at = uR(33);
  if (c_teste_alfa && (at & 1u) != 0u && !cmp((at >> 4u) & 7u, g_prev.a, int((at >> 8u) & 255u))) discard;
  o_color = vec4(g_prev) / 255.0;
}

#version 450
/* profundidade+estêncil lida como RGBA8 (R = estêncil, G/B/A = bytes da profundidade), com a precisão do
 * float24 da PICA (16 bits de mantissa), como em pica_gl.c */
layout(set=0, binding=0) uniform sampler2D u_d;
layout(set=0, binding=1) uniform usampler2D u_s;
layout(location=0) out vec4 o;
float f24q(float d) { if (d <= 0.0) return 0.0; float st = exp2(floor(log2(d)) - 16.0); return floor(d / st) * st; }
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  uint v = uint(f24q(texelFetch(u_d, p, 0).r) * 16777215.0);
  uint s = texelFetch(u_s, p, 0).r;
  o = vec4(float(s), float((v >> 16u) & 255u), float((v >> 8u) & 255u), float(v & 255u)) / 255.0;
}

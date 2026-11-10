#version 450
layout(location=0) in vec4 a_pos; layout(location=1) in vec4 a_color; layout(location=2) in vec4 a_tc01;
layout(location=3) in vec3 a_tc2; layout(location=4) in vec4 a_quat; layout(location=5) in vec3 a_view;
layout(location=0) out vec4 v_color; layout(location=1) out vec4 v_tc01; layout(location=2) out vec3 v_tc2; layout(location=3) out vec4 v_quat; layout(location=4) out vec3 v_view;
void main() {
  /* a_pos.z vem como no OpenGL (-1..1); no Vulkan a profundidade normalizada vai de 0 a 1 */
  gl_Position = vec4(a_pos.xy * a_pos.w, (a_pos.z * 0.5 + 0.5) * a_pos.w, a_pos.w);
  v_color = a_color; v_tc01 = a_tc01; v_tc2 = a_tc2; v_quat = a_quat; v_view = a_view;
}

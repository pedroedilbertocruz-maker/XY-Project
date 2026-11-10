/* casos: (nome, tipo, argumentos) */
#define INT_CASES(X) \
  X(t_arith, 3, 4) X(t_arith, -100, 77) X(t_arith, 0x7fffffff, -1) \
  X(t_udiv, 1000000, 7) X(t_udiv, 0xffffffff, 3) X(t_udiv, 5, 0) \
  X(t_loop, 0, 0) X(t_loop, 10, 0) X(t_loop, 1000, 0) \
  X(t_switch, 0, 0) X(t_switch, 3, 0) X(t_switch, 6, 0) X(t_switch, 7, 0) X(t_switch, -5, 0) \
  X(t_fib, 15, 0) X(t_memory, 3, 0) X(t_memory, -9, 0) X(t_bytes, 0, 0) X(t_bytes, 12345, 0) \
  X(t_short, 0x12345678, 0) X(t_short, -2, 0) X(t_short, 0x8081, 0) \
  X(t_fptr, 7, 5) X(t_fptr, -3, 100) X(t_struct, 1, 0) X(t_struct, 600, 0) \
  X(t_bits, 0x80000001, 0) X(t_bits, 0x00f00000, 0) X(t_bits, 0, 0) \
  X(t_smul, 0x12345678, 0x7fff8001) X(t_smul, -3, -70000) X(t_simd1, 0x12345678, 0x9abcdef0) X(t_simd1, 0x7f80ff01, 0x01ff807f) X(t_simd1, 0x80000000, 0x7fffffff) X(t_simd1, 0xdeadbeef, 0x0badf00d) X(t_simd2, 0x12345678, 0x9abcdef0) X(t_simd2, 0x7f80ff01, 0x01ff807f) X(t_simd2, 0x80000000, 0x7fffffff) X(t_simd2, 0xdeadbeef, 0x0badf00d) X(t_simd3, 0x12345678, 0x9abcdef0) X(t_simd3, 0x7f80ff01, 0x01ff807f) X(t_simd3, 0x80000000, 0x7fffffff) X(t_simd3, 0xdeadbeef, 0x0badf00d) X(t_simd4, 0x12345678, 0x9abcdef0) X(t_simd4, 0x7f80ff01, 0x01ff807f) X(t_simd4, 0x80000000, 0x7fffffff) X(t_simd4, 0xdeadbeef, 0x0badf00d) X(t_simd5, 0x12345678, 0x9abcdef0) X(t_simd5, 0x7f80ff01, 0x01ff807f) X(t_simd5, 0x80000000, 0x7fffffff) X(t_simd5, 0xdeadbeef, 0x0badf00d) \
  X(t_backjmp, 5, 7) X(t_backjmp, -300, 1) X(t_atomic, 1000, 77) X(t_atomic, -5, 0x12345) X(t_condret, 0, 9) X(t_condret, 60, 50) X(t_condret, 4, 5) X(t_condret, 9, 1) X(t_condret, 3, 1) X(t_bxret, 20, 1) X(t_v4call, 3, 4) X(t_v4call, -10, 2) X(t_v4b, 5, 3) X(t_v4b, 5, 0) X(t_tailret, 7, 8) X(t_cond, 5, 3) X(t_cond, -1, 3) X(t_cond, 2, 2) X(t_cond, 0, -7)

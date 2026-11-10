/* Programas de teste: compilados para ARM11 (3DS) e recompilados, e também compilados nativo. */
#include <stdint.h>
#define NOINLINE __attribute__((noinline))

static uint32_t udiv(uint32_t a, uint32_t b) {   /* ARMv6 não tem divisão por hardware */
    uint32_t q = 0, r = 0;
    for (int i = 31; i >= 0; i--) { r = (r << 1) | ((a >> i) & 1); if (r >= b) { r -= b; q |= 1u << i; } }
    return q;
}
NOINLINE int t_arith(int a, int b) { return (a * 7 - b) ^ (a >> 3) | (b << 5); }
NOINLINE uint32_t t_udiv(uint32_t a, uint32_t b) { return b ? udiv(a, b) + udiv(b, 3) : 0; }
NOINLINE int t_loop(int n) { int s = 0; for (int i = 0; i < n; i++) s += (i & 1) ? i * i : -i; return s; }
NOINLINE int t_switch(int x) {
    switch (x) { case 0: return 11; case 1: return 23; case 2: return 37; case 3: return 41;
                 case 4: return 59; case 5: return 61; case 6: return 79; default: return -1; }
}
#ifndef THUMB_TEST
NOINLINE int64_t t_mul64(int32_t a, int32_t b) { return (int64_t)a * b + ((uint64_t)(uint32_t)a * (uint32_t)b >> 7); }
#endif
NOINLINE int t_fib(int n) { return n < 2 ? n : t_fib(n - 1) + t_fib(n - 2); }
static int g_table[64];
NOINLINE int t_memory(int n) {
    for (int i = 0; i < 64; i++) g_table[i] = i * n;
    int s = 0; for (int i = 0; i < 64; i += 3) s += g_table[i];
    return s;
}
static const char *g_str = "Pokemon X recompilado!";
NOINLINE int t_bytes(int k) { int h = k; for (const char *p = g_str; *p; p++) h = h * 31 + (signed char)*p; return h; }
NOINLINE int t_short(int a) { int16_t x = (int16_t)a; uint16_t y = (uint16_t)(a >> 3); int8_t z = (int8_t)a; return x * 3 + y - z; }
#ifndef THUMB_TEST
NOINLINE float t_float(float a, float b) { return a * b + a / (b + 1.0f) - (a > b ? a : b); }
NOINLINE double t_double(double a, int n) { double s = 0; for (int i = 1; i <= n; i++) s += a / i; return s; }
NOINLINE int t_fcmp(float a, float b) { return (a < b) * 4 + (a == b) * 2 + (a > b); }
NOINLINE int t_f2i(float a) { return (int)(a * 3.5f) + (int)(unsigned)(a > 0 ? a : 0); }
#endif
typedef int (*binop)(int, int);
static NOINLINE int op_add(int a, int b) { return a + b; }
static NOINLINE int op_sub(int a, int b) { return a - b; }
static NOINLINE int op_mul(int a, int b) { return a * b; }
static binop volatile g_ops[3] = { op_add, op_sub, op_mul };
NOINLINE int t_fptr(int a, int b) { int s = 0; for (int i = 0; i < 3; i++) s = s * 3 + g_ops[i](a, b); return s; }
struct Poke { uint16_t species; uint8_t level; int8_t mood; uint32_t exp; int hp; };
static struct Poke g_party[6];
NOINLINE int t_struct(int seed) {
    for (int i = 0; i < 6; i++) { g_party[i].species = seed + i * 25; g_party[i].level = 5 + i * 7;
        g_party[i].mood = -i; g_party[i].exp = (uint32_t)g_party[i].level * g_party[i].level * g_party[i].level;
        g_party[i].hp = g_party[i].level * 3 / 2; }
    int s = 0; for (int i = 0; i < 6; i++) s += g_party[i].species + g_party[i].exp / 2 + g_party[i].mood + (int)g_party[i].hp;
    return s;
}
NOINLINE uint32_t t_bits(uint32_t x) { return __builtin_clz(x | 1) + __builtin_bswap32(x) + (x >> 29) + (uint32_t)((int32_t)x >> 30); }
NOINLINE int t_cond(int a, int b) { int r = 0; if (a > b) r += 1; if (a < 0) r += 2; if ((unsigned)a > (unsigned)b) r += 4; if (a == b) r += 8; return r; }
#ifndef THUMB_TEST
NOINLINE uint64_t t_u64(uint64_t a, uint64_t b) { return (a + b) ^ (a - b) ^ (a << 13) ^ (b >> 7); }
NOINLINE int t_smul(int a, int b) { int16_t x = a, y = b >> 16; return x * y + (int16_t)(a >> 16) * (int16_t)b; }
#endif
#ifndef THUMB_TEST
/* ---- SIMD do ARMv6: no ARM viram instruções reais; no nativo, referência escrita à mão ---- */
#ifdef __arm__
#include <arm_acle.h>
#define UQSUB8 __uqsub8
#define UADD8 __uadd8
#define USUB8 __usub8
#define SSUB8 __ssub8
#define SEL __sel
#define UXTB16 __uxtb16
#define UHADD8 __uhadd8
#define UQADD8 __uqadd8
#define QADD __qadd
#define QSUB16 __qsub16
#define SSAT16(x) __ssat16(x, 10)
#define USAT(x) __usat(x, 9)
#define SMUAD __smuad
#define SMUSD __smusd
#define SADD16 __sadd16
#else
static uint32_t g_ge;
static uint32_t lanes8(uint32_t a, uint32_t b, int op) {
    uint32_t r = 0, ge = 0;
    for (int i = 0; i < 4; i++) {
        int x = (a >> (8 * i)) & 255, y = (b >> (8 * i)) & 255, v = 0;
        int sx_ = (int8_t)x, sy = (int8_t)y;
        switch (op) {
        case 0: v = x - y; if (v < 0) v = 0; break;                 /* uqsub8 */
        case 1: v = x + y; if (v > 255) ge |= 1u << i; break;       /* uadd8 */
        case 2: v = x - y; if (v >= 0) ge |= 1u << i; break;        /* usub8 */
        case 3: v = sx_ - sy; if (v >= 0) ge |= 1u << i; break;     /* ssub8 */
        case 4: v = (x + y) >> 1; break;                            /* uhadd8 */
        case 5: v = x + y; if (v > 255) v = 255; break;            /* uqadd8 */
        }
        r |= (uint32_t)(v & 255) << (8 * i);
    }
    if (op >= 1 && op <= 3) g_ge = ge;
    return r;
}
#define UQSUB8(a, b) lanes8(a, b, 0)
#define UADD8(a, b) lanes8(a, b, 1)
#define USUB8(a, b) lanes8(a, b, 2)
#define SSUB8(a, b) lanes8(a, b, 3)
#define UHADD8(a, b) lanes8(a, b, 4)
#define UQADD8(a, b) lanes8(a, b, 5)
static uint32_t SEL(uint32_t a, uint32_t b) { uint32_t r = 0; for (int i = 0; i < 4; i++) r |= (((g_ge >> i) & 1) ? a : b) & (255u << (8 * i)); return r; }
static uint32_t UXTB16(uint32_t a) { return a & 0x00ff00ffu; }
static int32_t QADD(int32_t a, int32_t b) { int64_t v = (int64_t)a + b; return v > 2147483647 ? 2147483647 : v < -2147483647 - 1 ? -2147483647 - 1 : (int32_t)v; }
static int16_t s16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
static uint32_t QSUB16(uint32_t a, uint32_t b) { return (uint16_t)s16((int16_t)a - (int16_t)b) | (uint32_t)(uint16_t)s16((int16_t)(a >> 16) - (int16_t)(b >> 16)) << 16; }
static int32_t s10(int32_t v) { return v > 511 ? 511 : v < -512 ? -512 : v; }
static uint32_t SSAT16(uint32_t a) { return (uint16_t)s10((int16_t)a) | (uint32_t)(uint16_t)s10((int16_t)(a >> 16)) << 16; }
static uint32_t USAT(int32_t a) { return a < 0 ? 0 : a > 511 ? 511 : a; }
static int32_t SMUAD(uint32_t a, uint32_t b) { return (int16_t)a * (int16_t)b + (int16_t)(a >> 16) * (int16_t)(b >> 16); }
static int32_t SMUSD(uint32_t a, uint32_t b) { return (int16_t)a * (int16_t)b - (int16_t)(a >> 16) * (int16_t)(b >> 16); }
static uint32_t SADD16(uint32_t a, uint32_t b) { return (uint16_t)((int16_t)a + (int16_t)b) | (uint32_t)(uint16_t)((int16_t)(a >> 16) + (int16_t)(b >> 16)) << 16; }
#endif
NOINLINE uint32_t t_simd1(uint32_t a, uint32_t b) { uint32_t s = UQSUB8(a, b); uint32_t t = UADD8(a, b); return s ^ SEL(a, b) ^ (t * 3); }
NOINLINE uint32_t t_simd2(uint32_t a, uint32_t b) { return UXTB16(a) + UHADD8(a, b) * 5 + (USUB8(b, a) ^ SEL(b, a)); }
NOINLINE uint32_t t_simd3(uint32_t a, uint32_t b) { return UQADD8(a, b) ^ (SSUB8(a, b) + SEL(a, 0x55555555u)) ^ (uint32_t)QADD(a, b); }
NOINLINE uint32_t t_simd4(uint32_t a, uint32_t b) { return QSUB16(a, b) ^ SSAT16(a) ^ (USAT((int32_t)b >> 20) << 3) ^ SADD16(b, a); }
NOINLINE uint32_t t_simd5(uint32_t a, uint32_t b) { return (uint32_t)SMUAD(a, b) * 7 + (uint32_t)SMUSD(b, a) + ((a & 0xffff) | (b << 16)) + ((a & 0xffff0000u) | ((int32_t)b >> 16 & 0xffff)); }
#endif
#if defined(THUMB_TEST) && defined(__arm__)
/* o Thumb-1 não tem CLZ: o compilador chama este helper (que também é recompilado) */
int __clzsi2(unsigned x) { int n = 0; if (!x) return 32; while (!(x & 0x80000000u)) { x <<= 1; n++; } return n; }
#endif
/* ---- código compartilhado ANTES da entrada da função (o ARMCC faz isso com tail calls) ---- */
#if defined(__arm__) && !defined(THUMB_TEST)
__asm__(".text\n.arm\n.p2align 2\n"
        "shared_tail_code:\n  add r0, r0, #100\n  bx lr\n"
        ".global t_backjmp\n.type t_backjmp, %function\n"
        "t_backjmp:\n  add r0, r0, r1\n  b shared_tail_code\n");
int t_backjmp(int a, int b);
#elif !defined(__arm__)
int t_backjmp(int a, int b) { return a + b + 100; }
#endif

/* ---- retornos condicionais (ldr pc / ldm {pc} / mov pc com condição): o código depois deles continua vivo ---- */
#if defined(__arm__) && !defined(THUMB_TEST)
__asm__(".text\n.arm\n.p2align 2\n"
        ".global t_condret\n.type t_condret, %function\n"
        "t_condret:\n"
        "  str lr, [sp, #-4]!\n"
        "  cmp r0, #0\n"
        "  moveq r0, #7\n"
        "  ldreq pc, [sp], #4\n"          /* retorna se a == 0 */
        "  push {r4, lr}\n"
        "  add r4, r0, r1\n"
        "  cmp r4, #100\n"
        "  movgt r0, r4\n"
        "  popgt {r4, lr}\n"
        "  ldrgt pc, [sp], #4\n"          /* retorna se a+b > 100 */
        "  pop {r4, lr}\n"
        "  cmp r1, #5\n"
        "  addeq r0, r0, #1000\n"
        "  ldreq pc, [sp], #4\n"
        "  add sp, sp, #4\n"
        "  cmp r0, #3\n"
        "  rsb r0, r0, #0\n"
        "  movne pc, lr\n"                 /* retorna se a != 3 */
        "  mov r0, #42\n"
        "  bx lr\n");
int t_condret(int a, int b);
#elif !defined(__arm__)
int t_condret(int a, int b) {
    if (a == 0) return 7;
    if (a + b > 100) return a + b;
    if (b == 5) return a + 1000;
    if (a != 3) return -a;
    return 42;
}
#endif

/* ---- retorno por `pop {r3}; bx r3` (interworking): não é salto para função ---- */
#if defined(__arm__) && !defined(THUMB_TEST)
__asm__(".text\n.arm\n.p2align 2\n"
        ".global t_bxret\n.type t_bxret, %function\n"
        "t_bxret:\n"
        "  push {r4, lr}\n"
        "  add r4, r0, r1\n"
        "  mov r0, r4, lsl #1\n"
        "  pop {r4}\n"
        "  pop {r3}\n"
        "  bx r3\n");
int t_bxret(int a, int b);
#elif !defined(__arm__)
int t_bxret(int a, int b) { return (a + b) * 2; }
#endif

/* ---- chamada estilo ARMv4: `adr r4, sub; mov lr, pc; bx r4` (decodificador de vídeo Mobiclip) ---- */
#if defined(__arm__) && !defined(THUMB_TEST)
__asm__(".text\n.arm\n.p2align 2\n"
        ".global t_v4call\n.type t_v4call, %function\n"
        "t_v4call:\n"
        "  push {r4, lr}\n"
        "  adr r4, 1f\n"
        "  mov lr, pc\n"
        "  bx r4\n"
        "  add r0, r0, #1\n"
        "  adr r4, 2f\n"
        "  add lr, pc, #0\n"
        "  bx r4\n"
        "  pop {r4, pc}\n"
        "  nop\n"
        "1: add r0, r0, r1\n"
        "  bx lr\n"
        "2: mov r0, r0, lsl #1\n"
        "  bx lr\n");
int t_v4call(int a, int b);
#elif !defined(__arm__)
int t_v4call(int a, int b) { return (a + b + 1) * 2; }
#endif
/* ---- chamada estilo ARMv4 com desvio direto: `add lr, pc, #4; bne longe; b longe2` ---- */
#if defined(__arm__) && !defined(THUMB_TEST)
__asm__(".text\n.arm\n.p2align 2\n"
        ".global t_v4b\n.type t_v4b, %function\n"
        "t_v4b:\n"
        "  push {r4, lr}\n"
        "  cmp r1, #0\n"
        "  add lr, pc, #4\n"
        "  bne v4b_s1\n"
        "  b v4b_s2\n"
        "  add r0, r0, #100\n"
        "  pop {r4, pc}\n"
        "  .space 200\n"
        ".type v4b_s1, %function\nv4b_s1: add r0, r0, r1\n"
        "  mov pc, lr\n"
        ".type v4b_s2, %function\nv4b_s2: sub r0, r0, #7\n"
        "  mov pc, lr\n");
int t_v4b(int a, int b);
#elif !defined(__arm__)
int t_v4b(int a, int b) { return (b ? a + b : a - 7) + 100; }
#endif
/* ---- chamada de cauda que volta por um lr salvo antes (decodificador Mobiclip: `mov pc, r4` para a
 *      rotina da IDCT, que termina com `pop {..., pc}` usando o lr empilhado pela rotina anterior) ---- */
#if defined(__arm__) && !defined(THUMB_TEST)
__asm__(".text\n.arm\n.p2align 2\n"
        ".global t_tailret\n.type t_tailret, %function\n"
        "t_tailret:\n"
        "  push {r4, lr}\n"
        "  add lr, pc, #0\n"
        "  b tr_b\n"
        "  add r0, r0, #1000\n"
        "  pop {r4, pc}\n"
        "  .space 200\n"
        ".type tr_b, %function\ntr_b: push {r10, lr}\n"
        "  add r0, r0, r1\n"
        "  mov lr, #0\n"
        "  adr r4, tr_c\n"
        "  mov pc, r4\n"
        ".type tr_c, %function\ntr_c: mov r0, r0, lsl #1\n"
        "  pop {r10, pc}\n");
int t_tailret(int a, int b);
#elif !defined(__arm__)
int t_tailret(int a, int b) { return (a + b) * 2 + 1000; }
#endif
/* ---- atômicos de 8/16/64 bits: no ARMv6K viram LDREXB/H/D e STREXB/H/D ---- */
#ifndef THUMB_TEST
static volatile uint16_t g_a16; static volatile uint8_t g_a8; static volatile uint64_t g_a64;
NOINLINE int t_atomic(int a, int b) {
    g_a16 = (uint16_t)a; g_a8 = (uint8_t)b; g_a64 = ((uint64_t)a << 32) | (uint32_t)b;
    for (int i = 0; i < 5; i++) {
        __atomic_fetch_add(&g_a16, (uint16_t)(b + i), __ATOMIC_SEQ_CST);
        __atomic_fetch_xor(&g_a8, (uint8_t)(a + i), __ATOMIC_SEQ_CST);
        __atomic_fetch_add(&g_a64, (uint64_t)0x100000001ull * (unsigned)(i + 1), __ATOMIC_SEQ_CST);
    }
    return (int)(g_a16 * 7u + g_a8 + (uint32_t)(g_a64 >> 32) * 3u + (uint32_t)g_a64);
}
#endif

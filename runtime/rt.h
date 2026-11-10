/* Runtime mínimo do código recompilado. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct Ctx {
    uint32_t r[16];
    uint32_t n, z, c, v;
    uint32_t fpscr;
    uint32_t tls;
    uint32_t ge;    /* flags GE[3:0] do SIMD do ARMv6 */
    uint32_t q;     /* flag de saturação */
    uint32_t tret;  /* retorno esperado herdado numa chamada de cauda (0 = usar lr); ver JUMP/TAIL */
    union { float s[32]; double d[16]; };
} Ctx;

extern uint8_t *g_mem;   /* base do espaço de endereçamento de 32 bits do jogo */

static inline uint32_t *mem32p(uint32_t a) { return (uint32_t *)(g_mem + a); }
#define MEM32(a)  (*(uint32_t *)(g_mem + (uint32_t)(a)))
#define MEM16(a)  (*(uint16_t *)(g_mem + (uint32_t)(a)))
#define MEM8(a)   (*(uint8_t  *)(g_mem + (uint32_t)(a)))
#define MEMF32(a) (*(float    *)(g_mem + (uint32_t)(a)))
#define MEMF64(a) (*(double   *)(g_mem + (uint32_t)(a)))

#define ROR32(v, n) (((uint32_t)(v) >> (n)) | ((uint32_t)(v) << (32 - (n))))
#define CLZ32(v) ((v) ? (uint32_t)__builtin_clz(v) : 32u)
#define SET_NZ(r) do { c->n = (uint32_t)(r) >> 31; c->z = ((uint32_t)(r) == 0); } while (0)
#define FLAGS_ADD(x, y, r) do { SET_NZ(r); c->c = (uint32_t)(r) < (uint32_t)(x); \
    c->v = (((x) ^ (r)) & ((y) ^ (r))) >> 31; } while (0)
#define FLAGS_ADC(x, y, r) do { uint64_t u_ = (uint64_t)(x) + (y) + c->c; SET_NZ(r); \
    c->c = (uint32_t)(u_ >> 32); c->v = (((x) ^ (r)) & ((y) ^ (r))) >> 31; } while (0)
#define FLAGS_SUB(x, y, r) do { SET_NZ(r); c->c = (uint32_t)(x) >= (uint32_t)(y); \
    c->v = (((x) ^ (y)) & ((x) ^ (r))) >> 31; } while (0)
#define FLAGS_SBC(x, y, r) do { uint32_t b_ = !c->c; SET_NZ(r); \
    c->c = (uint64_t)(x) >= (uint64_t)(y) + b_; c->v = (((x) ^ (y)) & ((x) ^ (r))) >> 31; } while (0)

static inline uint32_t sh_lsl(Ctx *c, uint32_t v, uint32_t s, uint32_t *k) {
    if (s == 0) return v; if (s < 32) { *k = (v >> (32 - s)) & 1; return v << s; }
    *k = s == 32 ? v & 1 : 0; return 0; }
static inline uint32_t sh_lsr(Ctx *c, uint32_t v, uint32_t s, uint32_t *k) {
    if (s == 0) return v; if (s < 32) { *k = (v >> (s - 1)) & 1; return v >> s; }
    *k = s == 32 ? v >> 31 : 0; return 0; }
static inline uint32_t sh_asr(Ctx *c, uint32_t v, uint32_t s, uint32_t *k) {
    if (s == 0) return v; if (s < 32) { *k = (v >> (s - 1)) & 1; return (uint32_t)((int32_t)v >> s); }
    *k = v >> 31; return (uint32_t)((int32_t)v >> 31); }
static inline uint32_t sh_ror(Ctx *c, uint32_t v, uint32_t s, uint32_t *k) {
    if (s == 0) return v; s &= 31; if (s == 0) { *k = v >> 31; return v; }
    *k = (v >> (s - 1)) & 1; return ROR32(v, s); }

static inline void vcmp_set(Ctx *c, int lt, int eq, int un) {
    uint32_t f = un ? 0x3u : lt ? 0x8u : eq ? 0x6u : 0x2u;
    c->fpscr = (c->fpscr & 0x0fffffffu) | (f << 28); }
#define VCMP(c, a, b) vcmp_set(c, (a) < (b), (a) == (b), (a) != (a) || (b) != (b))

static inline uint32_t f2sf(float f)  { if (f != f) return 0; if (f >= 2147483647.0f) return 0x7fffffff; if (f <= -2147483648.0f) return 0x80000000u; return (uint32_t)(int32_t)f; }
static inline uint32_t f2uf(float f)  { if (!(f > 0)) return 0; if (f >= 4294967295.0f) return 0xffffffffu; return (uint32_t)f; }
static inline uint32_t f2sd(double f) { if (f != f) return 0; if (f >= 2147483647.0) return 0x7fffffff; if (f <= -2147483648.0) return 0x80000000u; return (uint32_t)(int32_t)f; }
static inline uint32_t f2ud(double f) { if (!(f > 0)) return 0; if (f >= 4294967295.0) return 0xffffffffu; return (uint32_t)f; }

/* ---------- SIMD do ARMv6 ---------- */
static inline int32_t sat_s(Ctx *c, int64_t v, int bits) {
    int64_t mx = (1ll << (bits - 1)) - 1, mn = -(1ll << (bits - 1));
    if (v > mx) { c->q = 1; return (int32_t)mx; } if (v < mn) { c->q = 1; return (int32_t)mn; } return (int32_t)v; }
static inline uint32_t sat_u(Ctx *c, int64_t v, int bits) {
    int64_t mx = (1ll << bits) - 1;
    if (v > mx) { c->q = 1; return (uint32_t)mx; } if (v < 0) { c->q = 1; return 0; } return (uint32_t)v; }
/* adição/subtração paralela: kind 1=S 2=Q 3=SH 5=U 6=UQ 7=UH; op 0=ADD16 1=ASX 2=SAX 3=SUB16 4=ADD8 7=SUB8 */
static inline uint32_t par_addsub(Ctx *c, int kind, int op, uint32_t a, uint32_t b) {
    int sgn = kind < 4; int lanes = op >= 4 ? 4 : 2; int w = 32 / lanes;
    uint32_t mask = (1u << w) - 1, res = 0, ge = 0;
    for (int i = 0; i < lanes; i++) {
        int ia = i, ib = i; int add;
        if (op == 0 || op == 4) add = 1; else if (op == 3 || op == 7) add = 0;
        else { ib = 1 - i; add = (op == 1) ? (i == 1) : (i == 0); }
        uint32_t ra = (a >> (ia * w)) & mask, rb = (b >> (ib * w)) & mask;
        int64_t x = sgn ? (int64_t)((int32_t)(ra << (32 - w)) >> (32 - w)) : ra;
        int64_t y = sgn ? (int64_t)((int32_t)(rb << (32 - w)) >> (32 - w)) : rb;
        int64_t r = add ? x + y : x - y;
        uint32_t lane;
        switch (kind) {
        case 2: lane = (uint32_t)sat_s(c, r, w); break;
        case 6: lane = sat_u(c, r, w); break;
        case 3: case 7: lane = (uint32_t)(r >> 1); break;
        default: lane = (uint32_t)r;
            if (kind == 1 ? r >= 0 : (add ? r >> w : r >= 0)) ge |= (lanes == 4 ? 1u : 3u) << (i * (lanes == 4 ? 1 : 2));
        }
        res |= (lane & mask) << (i * w);
    }
    if (kind == 1 || kind == 5) c->ge = ge;
    return res; }
static inline uint32_t simd_sel(Ctx *c, uint32_t a, uint32_t b) {
    uint32_t r = 0; for (int i = 0; i < 4; i++) r |= (((c->ge >> i) & 1) ? a : b) & (0xffu << (8 * i)); return r; }
static inline uint32_t qadd_sub(Ctx *c, int op, uint32_t m, uint32_t n) {
    int64_t nn = (int32_t)n; if (op & 2) nn = sat_s(c, 2 * nn, 32);
    return (uint32_t)sat_s(c, (op & 1) ? (int64_t)(int32_t)m - nn : (int64_t)(int32_t)m + nn, 32); }
static inline uint32_t sat16x2(Ctx *c, uint32_t v, int bits, int unsig) {
    uint32_t lo = unsig ? sat_u(c, (int16_t)v, bits) : (uint32_t)sat_s(c, (int16_t)v, bits);
    uint32_t hi = unsig ? sat_u(c, (int16_t)(v >> 16), bits) : (uint32_t)sat_s(c, (int16_t)(v >> 16), bits);
    return (lo & 0xffff) | (hi << 16); }
static inline uint32_t pack_cpsr(Ctx *c) {
    return (c->n << 31) | (c->z << 30) | (c->c << 29) | (c->v << 28) | (c->q << 27) | (c->ge << 16) | 0x10; }
static inline void unpack_cpsr(Ctx *c, uint32_t v, int f, int g) {
    if (f) { c->n = v >> 31; c->z = (v >> 30) & 1; c->c = (v >> 29) & 1; c->v = (v >> 28) & 1; c->q = (v >> 27) & 1; }
    if (g) c->ge = (v >> 16) & 0xf; }

void call_indirect(Ctx *c, uint32_t addr);   /* tabela endereço -> função */
/* salto indireto (bx rX, mov pc, ldr pc, pop {pc} com base != sp): se o destino é o endereço de retorno
 * da função atual, é um retorno; senão é um tail call para outra função */
/* Salto para fora da função: se o alvo é o endereço de retorno esperado, é um retorno; senão é uma chamada
 * de cauda, e a função alvo herda o nosso retorno esperado (c->tret) — ela volta para quem nos chamou. */
#define JUMP(x) do { uint32_t t_ = (x); if ((t_ | 1u) == (ret_ | 1u)) return; c->tret = ret_; call_indirect(c, t_); return; } while (0)
#define TAIL(f) do { c->tret = ret_; f(c); return; } while (0)
/* verificação contra o interpretador (gerado com RECOMP_VERIFY=1; ligado em tempo de execução com VERIFICAR) */
extern volatile int g_vfy;
extern volatile int g_opt_xp;              /* opção do menu: multiplicador do XP ganho em batalha (gancho de tools/recomp.py) */
extern volatile int g_opt_radar;           /* opção do menu: Poké Radar sempre carregado (ganchos de tools/recomp.py) */
int vfy_entry(Ctx *c, uint32_t addr, void (*fn)(Ctx *));
#define VFY_ENTRY(a, f) do { if (__builtin_expect(g_vfy, 0) && vfy_entry(c, (a), (f))) return; } while (0)
/* ponto de preempção nos laços (saltos para trás): uma thread do jogo que gira sem syscalls cede a vez quando
 * o kernel pede (g_kpoll é ligado por um relógio a cada ~2 ms); custa uma leitura por volta do laço */
extern volatile int g_kpoll;
void k_poll(void);
#define KPOLL() do { if (__builtin_expect(g_kpoll, 0)) k_poll(); } while (0)
void svc(Ctx *c, uint32_t num);              /* chamadas ao kernel do 3DS (HLE) */
void UNIMPL(Ctx *c, uint32_t addr, uint32_t insn);
void nan_log(Ctx *c, uint32_t addr, double a, double b);
void probe(Ctx *c, uint32_t addr);           /* sonda de depuração (PROBES no gerador) */

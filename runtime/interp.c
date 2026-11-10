/* Interpretador ARMv6K (ARM + Thumb + VFPv2) escrito à parte do recompilador, a partir da semântica do
 * manual da arquitetura. Dois usos:
 *  1. verificação (VERIFICAR): roda uma função do jogo numa memória sobreposta (as escritas ficam numa tabela
 *     à parte) e compara com a função traduzida;
 *  2. reserva: código que o recompilador não encontrou roda aqui em vez de parar o jogo.
 * O estado é o mesmo Ctx do código traduzido; o modo Thumb fica numa variável local (bit 0 dos endereços). */
#include "rt.h"
#include "interp.h"
#include <stdio.h>
#include <stdlib.h>

void svc(Ctx *c, uint32_t num);

/* ------------------------------------------------------------ memória (direta ou sobreposta) */
static __thread int t_ov;                        /* esta thread usa a memória sobreposta */
#define OV_BITS 20
#define OV_N (1u << OV_BITS)
typedef struct { uint32_t addr, gen, pc; uint8_t v; } OvEnt;
static __thread uint32_t t_curpc;
static __thread OvEnt *t_ovt; static __thread uint32_t t_ovgen; static __thread uint32_t t_ovn;
static __thread uint32_t *t_ovlist; static __thread uint32_t t_ovlist_n;
int ip_overflow;
void (*ip_tick_hook)(void);
uint32_t ip_last_info;

static inline uint32_t ov_hash(uint32_t a) { return (a * 2654435761u) >> (32 - OV_BITS); }
static OvEnt *ov_find(uint32_t a, int create) {
    uint32_t i = ov_hash(a);
    for (uint32_t k = 0; k < 64; k++, i = (i + 1) & (OV_N - 1)) {
        OvEnt *e = &t_ovt[i];
        if (e->gen != t_ovgen) { if (!create) return NULL;
            if (t_ovn >= OV_N / 2) { ip_overflow = 1; return NULL; }
            e->gen = t_ovgen; e->addr = a; t_ovn++; t_ovlist[t_ovlist_n++] = a; return e; }
        if (e->addr == a) return e;
    }
    ip_overflow = 1; return NULL;
}
void ip_overlay_begin(void) {
    if (!t_ovt) { t_ovt = calloc(OV_N, sizeof(OvEnt)); t_ovlist = malloc(sizeof(uint32_t) * (OV_N / 2 + 8)); }
    t_ovgen++; if (!t_ovgen) { memset(t_ovt, 0, sizeof(OvEnt) * OV_N); t_ovgen = 1; }
    t_ovn = 0; t_ovlist_n = 0; ip_overflow = 0; t_ov = 1;
}
void ip_overlay_end(void) { t_ov = 0; }
uint32_t ip_overlay_count(void) { return t_ovlist_n; }
uint32_t ip_overlay_addr(uint32_t i) { return t_ovlist[i]; }
uint32_t ip_overlay_pc(uint32_t a) { int s = t_ov; t_ov = 1; OvEnt *e = ov_find(a, 0); t_ov = s; return e ? e->pc : 0; }
uint8_t ip_overlay_val(uint32_t a) { int s = t_ov; t_ov = 1; OvEnt *e = ov_find(a, 0); t_ov = s; return e ? e->v : MEM8(a); }

static inline uint8_t rd8(uint32_t a) {
    if (t_ov) { OvEnt *e = ov_find(a, 0); if (e) return e->v; }
    return MEM8(a);
}
static inline void wr8(uint32_t a, uint8_t v) {
    if (t_ov) { OvEnt *e = ov_find(a, 1); if (e) { e->v = v; e->pc = t_curpc; } return; }
    MEM8(a) = v;
}
static inline uint16_t rd16(uint32_t a) { if (!t_ov) return MEM16(a); return (uint16_t)(rd8(a) | rd8(a + 1) << 8); }
static inline uint32_t rd32(uint32_t a) { if (!t_ov) return MEM32(a); return (uint32_t)rd16(a) | (uint32_t)rd16(a + 2) << 16; }
static inline void wr16(uint32_t a, uint16_t v) { if (!t_ov) { MEM16(a) = v; return; } wr8(a, (uint8_t)v); wr8(a + 1, (uint8_t)(v >> 8)); }
static inline void wr32(uint32_t a, uint32_t v) { if (!t_ov) { MEM32(a) = v; return; } wr16(a, (uint16_t)v); wr16(a + 2, (uint16_t)(v >> 16)); }
static inline uint64_t rd64(uint32_t a) { return (uint64_t)rd32(a) | (uint64_t)rd32(a + 4) << 32; }
static inline void wr64(uint32_t a, uint64_t v) { wr32(a, (uint32_t)v); wr32(a + 4, (uint32_t)(v >> 32)); }

/* ------------------------------------------------------------ auxiliares */
typedef struct { Ctx *c; int thumb; uint32_t pc; int stop; uint32_t ret; IpExit why; uint32_t info; } St;

static inline uint32_t ror(uint32_t v, unsigned n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
static inline void nz(Ctx *c, uint32_t r) { c->n = r >> 31; c->z = r == 0; }
static int cond_ok(Ctx *c, unsigned cc) {
    switch (cc) {
    case 0: return c->z; case 1: return !c->z; case 2: return c->c; case 3: return !c->c;
    case 4: return c->n; case 5: return !c->n; case 6: return c->v; case 7: return !c->v;
    case 8: return c->c && !c->z; case 9: return !c->c || c->z;
    case 10: return c->n == c->v; case 11: return c->n != c->v;
    case 12: return !c->z && c->n == c->v; case 13: return c->z || c->n != c->v;
    default: return 1;
    }
}
static uint32_t add_flags(Ctx *c, uint32_t a, uint32_t b, uint32_t cin, int s) {
    uint64_t u = (uint64_t)a + b + cin; uint32_t r = (uint32_t)u;
    if (s) { nz(c, r); c->c = (uint32_t)(u >> 32); c->v = ((a ^ r) & (b ^ r)) >> 31; }
    return r;
}
/* deslocamento do operando 2; *co = carry de saída */
static uint32_t shift_c(uint32_t v, unsigned typ, unsigned amt, int byreg, uint32_t cin, uint32_t *co) {
    *co = cin;
    if (byreg) {
        amt &= 0xFF;
        if (amt == 0) return v;
        switch (typ) {
        case 0: if (amt < 32) { *co = (v >> (32 - amt)) & 1; return v << amt; } *co = amt == 32 ? v & 1 : 0; return 0;
        case 1: if (amt < 32) { *co = (v >> (amt - 1)) & 1; return v >> amt; } *co = amt == 32 ? v >> 31 : 0; return 0;
        case 2: if (amt < 32) { *co = (v >> (amt - 1)) & 1; return (uint32_t)((int32_t)v >> amt); } *co = v >> 31; return (uint32_t)((int32_t)v >> 31);
        default: amt &= 31; if (amt == 0) { *co = v >> 31; return v; } *co = (v >> (amt - 1)) & 1; return ror(v, amt);
        }
    }
    switch (typ) {
    case 0: if (amt == 0) return v; *co = (v >> (32 - amt)) & 1; return v << amt;
    case 1: if (amt == 0) { *co = v >> 31; return 0; } *co = (v >> (amt - 1)) & 1; return v >> amt;
    case 2: if (amt == 0) { *co = v >> 31; return (uint32_t)((int32_t)v >> 31); } *co = (v >> (amt - 1)) & 1; return (uint32_t)((int32_t)v >> amt);
    default:
        if (amt == 0) { *co = v & 1; return (cin << 31) | (v >> 1); }    /* RRX */
        *co = (v >> (amt - 1)) & 1; return ror(v, amt);
    }
}
static inline uint32_t rget(St *s, unsigned r) { return r == 15 ? s->pc + (s->thumb ? 4 : 8) : s->c->r[r]; }

/* escrita em pc: com troca de modo (bx) ou não */
static inline void branch_to(St *s, uint32_t a, int interwork) {
    if (interwork) { s->thumb = a & 1; a &= ~1u; } else a &= s->thumb ? ~1u : ~3u;
    s->c->r[15] = a; s->stop = 1;       /* (stop = pc já atualizado) */
}
static inline void wreg(St *s, unsigned r, uint32_t v) {
    if (r == 15) branch_to(s, v, 1);     /* ARMv6: escrita em pc por instrução de dados não troca de modo... */
    else s->c->r[r] = v;
}

static uint32_t ip_sat_s(int64_t v, unsigned bits, int *q) {
    int64_t mx = ((int64_t)1 << (bits - 1)) - 1, mn = -((int64_t)1 << (bits - 1));
    if (v > mx) { *q = 1; return (uint32_t)mx; } if (v < mn) { *q = 1; return (uint32_t)mn; } return (uint32_t)v;
}
static uint32_t ip_sat_u(int64_t v, unsigned bits, int *q) {
    int64_t mx = ((int64_t)1 << bits) - 1;
    if (v > mx) { *q = 1; return (uint32_t)mx; } if (v < 0) { *q = 1; return 0; } return (uint32_t)v;
}

/* ------------------------------------------------------------ VFP */
static inline float S(Ctx *c, unsigned i) { return c->s[i]; }
static inline double D(Ctx *c, unsigned i) { return c->d[i]; }
static void vcmp_flags(Ctx *c, double a, double b) {
    uint32_t f = (a != a || b != b) ? 0x3u : a < b ? 0x8u : a == b ? 0x6u : 0x2u;
    c->fpscr = (c->fpscr & 0x0fffffffu) | (f << 28);
}
static int vfp_exec(St *s, uint32_t w) {
    Ctx *c = s->c;
    unsigned cp = (w >> 8) & 0xF;
    if (cp != 10 && cp != 11) return 0;
    int dbl = cp == 11;
    unsigned top = (w >> 24) & 0xF;
    /* registradores: S = Vd:D, D = D:Vd */
    #define SREG(v, b) ((((w >> (v)) & 0xF) << 1) | ((w >> (b)) & 1))
    #define DREG(v, b) ((((w >> (b)) & 1) << 4) | ((w >> (v)) & 0xF))
    if (top == 0xE && !(w & 0x10)) {            /* processamento de dados */
        unsigned opc1 = (w >> 20) & 0xB, opc2 = (w >> 16) & 0xF, opc3 = (w >> 6) & 3;
        unsigned d = dbl ? DREG(12, 22) : SREG(12, 22), n = dbl ? DREG(16, 7) : SREG(16, 7), m = dbl ? DREG(0, 5) : SREG(0, 5);
        int neg = (w >> 6) & 1;
        if (opc1 != 0xB) {
            double a = dbl ? D(c, n) : S(c, n), b = dbl ? D(c, m) : S(c, m), dd = dbl ? D(c, d) : S(c, d), r;
            if (!dbl) {          /* em precisão simples, faz a conta em float (como o hardware) */
                float fa = S(c, n), fb = S(c, m), fd = S(c, d), fr;
                switch (opc1) {
                case 0x0: fr = fa * fb; fr = neg ? fd - fr : fd + fr; break;                 /* VMLA / VMLS */
                case 0x1: fr = fa * fb; fr = neg ? -fd - fr : -fd + fr; break;               /* op=1 VNMLA (-d - a*b), op=0 VNMLS (-d + a*b) */
                case 0x2: fr = fa * fb; if (neg) fr = -fr; break;                             /* VMUL / VNMUL */
                case 0x3: fr = neg ? fa - fb : fa + fb; break;                                /* VADD / VSUB */
                case 0x8: if (neg) return 0; fr = fa / fb; break;                             /* VDIV */
                default: return 0;
                }
                c->s[d] = fr; return 1;
            }
            switch (opc1) {
            case 0x0: r = a * b; r = neg ? dd - r : dd + r; break;
            case 0x1: r = a * b; r = neg ? -dd - r : -dd + r; break;
            case 0x2: r = a * b; if (neg) r = -r; break;
            case 0x3: r = neg ? a - b : a + b; break;
            case 0x8: if (neg) return 0; r = a / b; break;
            default: return 0;
            }
            c->d[d] = r; return 1;
        }
        /* opc1 == 1x11: outras */
        switch (opc2) {
        case 0x0:
            if (opc3 == 1) { if (dbl) c->d[d] = c->d[m]; else c->s[d] = c->s[m]; return 1; }           /* VMOV */
            if (opc3 == 3) { if (dbl) c->d[d] = fabs(c->d[m]); else c->s[d] = fabsf(c->s[m]); return 1; } /* VABS */
            return 0;
        case 0x1:
            if (opc3 == 1) { if (dbl) c->d[d] = -c->d[m]; else c->s[d] = -c->s[m]; return 1; }         /* VNEG */
            if (opc3 == 3) { if (dbl) c->d[d] = sqrt(c->d[m]); else c->s[d] = sqrtf(c->s[m]); return 1; } /* VSQRT */
            return 0;
        case 0x4: case 0x5: {                                                                        /* VCMP(E) */
            double a = dbl ? D(c, d) : S(c, d), b = opc2 == 5 ? 0.0 : (dbl ? D(c, m) : S(c, m));
            vcmp_flags(c, a, b); return 1;
        }
        case 0x7:                                                                                     /* VCVT f32 <-> f64 */
            if (opc3 != 3) return 0;
            if (dbl) { unsigned sd = SREG(12, 22); c->s[sd] = (float)c->d[m]; }
            else { unsigned dd = DREG(12, 22); c->d[dd] = (double)c->s[m]; }
            return 1;
        case 0x8: {                                                                                   /* VCVT int -> float */
            unsigned sm = SREG(0, 5); uint32_t iv; memcpy(&iv, &c->s[sm], 4);
            int sgn = (w >> 7) & 1;
            if (dbl) c->d[d] = sgn ? (double)(int32_t)iv : (double)iv;
            else c->s[d] = sgn ? (float)(int32_t)iv : (float)iv;
            return 1;
        }
        case 0xC: case 0xD: {                                                                         /* VCVT float -> int */
            unsigned sd = SREG(12, 22); int sgn = opc2 == 0xD; int rz = (w >> 7) & 1;
            double v = dbl ? D(c, m) : S(c, m);
            if (!rz) v = nearbyint(v);    /* (arredondamento do FPSCR: ao par mais próximo, o padrão) */
            uint32_t r = sgn ? f2sd(v) : f2ud(v);
            memcpy(&c->s[sd], &r, 4); return 1;
        }
        default: return 0;
        }
    }
    if (top == 0xE && (w & 0x10)) {             /* transferências de registrador */
        unsigned L = (w >> 20) & 1, rt = (w >> 12) & 0xF;
        if (((w >> 21) & 7) == 7 && !dbl) {      /* VMRS / VMSR */
            if (((w >> 16) & 0xF) != 1) { if (L) { if (rt != 15) c->r[rt] = 0; } return 1; }   /* (FPSID/FPEXC: ignora) */
            if (L) {
                if (rt == 15) { c->n = c->fpscr >> 31; c->z = (c->fpscr >> 30) & 1; c->c = (c->fpscr >> 29) & 1; c->v = (c->fpscr >> 28) & 1; }
                else c->r[rt] = c->fpscr;
            } else c->fpscr = c->r[rt];
            return 1;
        }
        if (((w >> 21) & 7) == 0 && !dbl) {      /* VMOV core <-> single */
            unsigned sn = SREG(16, 7);
            if (L) memcpy(&c->r[rt], &c->s[sn], 4); else memcpy(&c->s[sn], &c->r[rt], 4);
            return 1;
        }
        if (dbl && ((w >> 21) & 6) == 0) {       /* VMOV core <-> metade de double */
            unsigned dn = DREG(16, 7), h = (w >> 21) & 1; uint32_t p[2]; memcpy(p, &c->d[dn], 8);
            if (L) c->r[rt] = p[h]; else { p[h] = c->r[rt]; memcpy(&c->d[dn], p, 8); }
            return 1;
        }
        return 0;
    }
    if (top == 0xC && ((w >> 21) & 0x7) == 2) {  /* VMOV dois cores <-> double / dois singles */
        unsigned L = (w >> 20) & 1, rt = (w >> 12) & 0xF, rt2 = (w >> 16) & 0xF;
        if (dbl) {
            unsigned dm = DREG(0, 5); uint32_t p[2];
            if (L) { memcpy(p, &c->d[dm], 8); c->r[rt] = p[0]; c->r[rt2] = p[1]; }
            else { p[0] = c->r[rt]; p[1] = c->r[rt2]; memcpy(&c->d[dm], p, 8); }
        } else {
            unsigned sm = SREG(0, 5);
            if (L) { memcpy(&c->r[rt], &c->s[sm], 4); memcpy(&c->r[rt2], &c->s[(sm + 1) & 31], 4); }
            else { memcpy(&c->s[sm], &c->r[rt], 4); memcpy(&c->s[(sm + 1) & 31], &c->r[rt2], 4); }
        }
        return 1;
    }
    if (top == 0xD && ((w >> 21) & 1) == 0) {    /* VLDR / VSTR */
        unsigned L = (w >> 20) & 1, rn = (w >> 16) & 0xF, U = (w >> 23) & 1;
        uint32_t base = rn == 15 ? ((s->pc + (s->thumb ? 4 : 8)) & ~3u) : c->r[rn];
        uint32_t a = base + (U ? (w & 0xFF) * 4 : -(w & 0xFF) * 4);
        if (dbl) { unsigned dd = DREG(12, 22); if (L) { uint64_t v = rd64(a); memcpy(&c->d[dd], &v, 8); } else { uint64_t v; memcpy(&v, &c->d[dd], 8); wr64(a, v); } }
        else { unsigned sd = SREG(12, 22); if (L) { uint32_t v = rd32(a); memcpy(&c->s[sd], &v, 4); } else { uint32_t v; memcpy(&v, &c->s[sd], 4); wr32(a, v); } }
        return 1;
    }
    if (top == 0xC || top == 0xD) {               /* VLDM / VSTM (e VPUSH/VPOP) */
        unsigned P = (w >> 24) & 1, U = (w >> 23) & 1, W = (w >> 21) & 1, L = (w >> 20) & 1, rn = (w >> 16) & 0xF;
        unsigned imm = w & 0xFF;
        if (P == U) return 0;
        uint32_t base = c->r[rn], a = P ? base - imm * 4 : base;
        if (dbl) {
            unsigned dd = DREG(12, 22), cnt = imm / 2;
            for (unsigned i = 0; i < cnt; i++, a += 8) {
                unsigned r = (dd + i) & 15;
                if (L) { uint64_t v = rd64(a); memcpy(&c->d[r], &v, 8); } else { uint64_t v; memcpy(&v, &c->d[r], 8); wr64(a, v); }
            }
        } else {
            unsigned sd = SREG(12, 22);
            for (unsigned i = 0; i < imm; i++, a += 4) {
                unsigned r = (sd + i) & 31;
                if (L) { uint32_t v = rd32(a); memcpy(&c->s[r], &v, 4); } else { uint32_t v; memcpy(&v, &c->s[r], 4); wr32(a, v); }
            }
        }
        if (W) c->r[rn] = U ? base + imm * 4 : base - imm * 4;
        return 1;
    }
    return 0;
    #undef SREG
    #undef DREG
}

/* ------------------------------------------------------------ ARM: mídia / SIMD do ARMv6 */
static uint32_t extend(uint32_t v, unsigned rot, unsigned op) {
    v = ror(v, rot * 8);
    switch (op) {
    case 0: return (uint32_t)(((int32_t)(int8_t)(v & 0xFF) & 0xFFFF) | (((int32_t)(int8_t)((v >> 16) & 0xFF) & 0xFFFF) << 16));  /* SXTB16 */
    case 2: return (uint32_t)(int32_t)(int8_t)v;                 /* SXTB */
    case 3: return (uint32_t)(int32_t)(int16_t)v;                /* SXTH */
    case 4: return v & 0x00FF00FF;                               /* UXTB16 */
    case 6: return v & 0xFF;                                     /* UXTB */
    case 7: return v & 0xFFFF;                                   /* UXTH */
    }
    return v;
}
static int parallel(Ctx *c, uint32_t w, uint32_t a, uint32_t b, uint32_t *out) {
    unsigned op1 = (w >> 20) & 7, op2 = (w >> 5) & 7;    /* op1: 1 S, 2 Q, 3 SH, 5 U, 6 UQ, 7 UH */
    int sgn = op1 < 4, kind = op1 & 3;                   /* kind 1 normal (GE), 2 saturada, 3 metade */
    uint32_t r = 0, ge = 0;
    if (op2 == 4 || op2 == 7) {                          /* ADD8 / SUB8 */
        for (int i = 0; i < 4; i++) {
            int32_t x = sgn ? (int8_t)(a >> (8 * i)) : (uint8_t)(a >> (8 * i));
            int32_t y = sgn ? (int8_t)(b >> (8 * i)) : (uint8_t)(b >> (8 * i));
            int32_t s = op2 == 4 ? x + y : x - y; int q = 0; uint32_t v;
            if (kind == 2) v = sgn ? ip_sat_s(s, 8, &q) : ip_sat_u(s, 8, &q);
            else if (kind == 3) v = (uint32_t)(s >> 1);
            else { v = (uint32_t)s; if (sgn ? s >= 0 : (op2 == 4 ? s >= 0x100 : s >= 0)) ge |= 1u << i; }
            r |= (v & 0xFF) << (8 * i);
        }
    } else if (op2 <= 3) {                               /* ADD16, ASX, SAX, SUB16 */
        int32_t xl = sgn ? (int16_t)a : (uint16_t)a, xh = sgn ? (int16_t)(a >> 16) : (uint16_t)(a >> 16);
        int32_t yl = sgn ? (int16_t)b : (uint16_t)b, yh = sgn ? (int16_t)(b >> 16) : (uint16_t)(b >> 16);
        int32_t lo, hi; int addlo, addhi;
        switch (op2) {
        case 0: lo = xl + yl; hi = xh + yh; addlo = addhi = 1; break;        /* ADD16 */
        case 1: lo = xl - yh; hi = xh + yl; addlo = 0; addhi = 1; break;     /* ASX */
        case 2: lo = xl + yh; hi = xh - yl; addlo = 1; addhi = 0; break;     /* SAX */
        default: lo = xl - yl; hi = xh - yh; addlo = addhi = 0; break;       /* SUB16 */
        }
        int32_t v[2] = { lo, hi }; int add[2] = { addlo, addhi };
        for (int i = 0; i < 2; i++) {
            int q = 0; uint32_t o;
            if (kind == 2) o = sgn ? ip_sat_s(v[i], 16, &q) : ip_sat_u(v[i], 16, &q);
            else if (kind == 3) o = (uint32_t)(v[i] >> 1);
            else { o = (uint32_t)v[i]; if (sgn ? v[i] >= 0 : (add[i] ? v[i] >= 0x10000 : v[i] >= 0)) ge |= 3u << (2 * i); }
            r |= (o & 0xFFFF) << (16 * i);
        }
    } else return 0;
    if (kind == 1) c->ge = ge;
    *out = r; return 1;
}

/* ------------------------------------------------------------ ARM */
static void undefined(St *s, uint32_t w) { s->why = IP_UNDEF; s->info = w; s->stop = 2; }

static void arm_ldm_stm(St *s, uint32_t w) {
    Ctx *c = s->c;
    unsigned P = (w >> 24) & 1, U = (w >> 23) & 1, Sb = (w >> 22) & 1, W = (w >> 21) & 1, L = (w >> 20) & 1, rn = (w >> 16) & 0xF;
    uint32_t list = w & 0xFFFF; unsigned cnt = (unsigned)__builtin_popcount(list);
    (void)Sb;
    uint32_t base = c->r[rn], a;
    if (U) a = base + (P ? 4 : 0); else a = base - cnt * 4 + (P ? 0 : 4);
    uint32_t nb = U ? base + cnt * 4 : base - cnt * 4;
    uint32_t newpc = 0; int pcload = 0;
    for (unsigned r = 0; r < 16; r++) {
        if (!(list >> r & 1)) continue;
        if (L) { uint32_t v = rd32(a); if (r == 15) { newpc = v; pcload = 1; } else c->r[r] = v; }
        else wr32(a, r == 15 ? s->pc + 8 : c->r[r]);
        a += 4;
    }
    if (W && !(L && (list >> rn & 1))) c->r[rn] = nb;
    if (pcload) branch_to(s, newpc, 1);
}

static void arm_ldst(St *s, uint32_t w) {        /* LDR/STR/LDRB/STRB */
    Ctx *c = s->c;
    unsigned I = (w >> 25) & 1, P = (w >> 24) & 1, U = (w >> 23) & 1, B = (w >> 22) & 1, W = (w >> 21) & 1, L = (w >> 20) & 1;
    unsigned rn = (w >> 16) & 0xF, rt = (w >> 12) & 0xF;
    uint32_t off;
    if (I) { uint32_t co; off = shift_c(rget(s, w & 0xF), (w >> 5) & 3, (w >> 7) & 0x1F, 0, c->c, &co); }
    else off = w & 0xFFF;
    uint32_t base = rget(s, rn); if (rn == 15) base &= ~3u;
    uint32_t addr = U ? base + off : base - off;
    uint32_t ea = P ? addr : base;
    if (L) {
        uint32_t v = B ? rd8(ea) : rd32(ea);
        if (!P || W) c->r[rn] = addr;
        if (rt == 15) branch_to(s, v, 1); else c->r[rt] = v;
    } else {
        uint32_t v = rt == 15 ? s->pc + 8 : c->r[rt];
        if (B) wr8(ea, (uint8_t)v); else wr32(ea, v);
        if (!P || W) c->r[rn] = addr;
    }
}

static void arm_extra_ldst(St *s, uint32_t w) {  /* LDRH/STRH/LDRSB/LDRSH/LDRD/STRD */
    Ctx *c = s->c;
    unsigned P = (w >> 24) & 1, U = (w >> 23) & 1, I = (w >> 22) & 1, W = (w >> 21) & 1, L = (w >> 20) & 1;
    unsigned rn = (w >> 16) & 0xF, rt = (w >> 12) & 0xF, sh = (w >> 5) & 3;
    uint32_t off = I ? (((w >> 4) & 0xF0) | (w & 0xF)) : c->r[w & 0xF];
    uint32_t base = rget(s, rn); if (rn == 15) base &= ~3u;
    uint32_t addr = U ? base + off : base - off, ea = P ? addr : base;
    if (L) {
        uint32_t v;
        if (sh == 1) v = rd16(ea); else if (sh == 2) v = (uint32_t)(int32_t)(int8_t)rd8(ea); else v = (uint32_t)(int32_t)(int16_t)rd16(ea);
        if (!P || W) c->r[rn] = addr;
        if (rt == 15) branch_to(s, v, 1); else c->r[rt] = v;
    } else {
        if (sh == 1) wr16(ea, (uint16_t)c->r[rt]);
        else if (sh == 2) { c->r[rt] = rd32(ea); c->r[rt + 1] = rd32(ea + 4); }       /* LDRD */
        else { wr32(ea, c->r[rt]); wr32(ea + 4, c->r[rt + 1]); }                      /* STRD */
        if (!P || W) c->r[rn] = addr;
    }
}

static void arm_dp(St *s, uint32_t w) {          /* processamento de dados */
    Ctx *c = s->c;
    unsigned op = (w >> 21) & 0xF, S = (w >> 20) & 1, rn = (w >> 16) & 0xF, rd = (w >> 12) & 0xF;
    uint32_t b, co = c->c;
    if ((w >> 25) & 1) { unsigned rot = ((w >> 8) & 0xF) * 2; b = ror(w & 0xFF, rot); if (rot) co = b >> 31; }
    else {
        int byreg = (w >> 4) & 1;
        uint32_t rmv = (w & 0xF) == 15 ? s->pc + (byreg ? 12 : 8) : c->r[w & 0xF];
        unsigned amt = byreg ? (c->r[(w >> 8) & 0xF] & 0xFF) : (w >> 7) & 0x1F;
        b = shift_c(rmv, (w >> 5) & 3, amt, byreg, c->c, &co);
    }
    uint32_t a = rn == 15 ? s->pc + (((w >> 25) & 1) || !((w >> 4) & 1) ? 8 : 12) : c->r[rn], r = 0;
    int logical = 0, write = 1;
    switch (op) {
    case 0x0: r = a & b; logical = 1; break;
    case 0x1: r = a ^ b; logical = 1; break;
    case 0x2: r = add_flags(c, a, ~b, 1, S && rd != 15); break;
    case 0x3: r = add_flags(c, b, ~a, 1, S && rd != 15); break;
    case 0x4: r = add_flags(c, a, b, 0, S && rd != 15); break;
    case 0x5: r = add_flags(c, a, b, c->c, S && rd != 15); break;
    case 0x6: r = add_flags(c, a, ~b, c->c, S && rd != 15); break;
    case 0x7: r = add_flags(c, b, ~a, c->c, S && rd != 15); break;
    case 0x8: r = a & b; logical = 1; write = 0; break;
    case 0x9: r = a ^ b; logical = 1; write = 0; break;
    case 0xA: add_flags(c, a, ~b, 1, 1); write = 0; break;
    case 0xB: add_flags(c, a, b, 0, 1); write = 0; break;
    case 0xC: r = a | b; logical = 1; break;
    case 0xD: r = b; logical = 1; break;
    case 0xE: r = a & ~b; logical = 1; break;
    case 0xF: r = ~b; logical = 1; break;
    }
    if (logical && S && (rd != 15 || !write)) { nz(c, r); c->c = co; }
    if (write) { if (rd == 15) branch_to(s, r, 0); else c->r[rd] = r; }
}

static void arm_misc(St *s, uint32_t w) {        /* espaço 000 com bits 7 e 4 (multiplicações, extras, misc) */
    Ctx *c = s->c;
    /* multiplicações */
    if ((w & 0x0F0000F0) == 0x00000090 || (w & 0x0F8000F0) == 0x00800090) {
        unsigned rd = (w >> 16) & 0xF, rn = (w >> 12) & 0xF, rs = (w >> 8) & 0xF, rm = w & 0xF, S = (w >> 20) & 1;
        unsigned op = (w >> 21) & 7;
        if (op == 0 || op == 1) {                                  /* MUL / MLA */
            uint32_t r = c->r[rm] * c->r[rs] + (op ? c->r[rn] : 0);
            c->r[rd] = r; if (S) nz(c, r); return;
        }
        if (op == 2) {                                             /* UMAAL */
            uint64_t r = (uint64_t)c->r[rm] * c->r[rs] + c->r[rn] + c->r[rd];
            c->r[rn] = (uint32_t)r; c->r[rd] = (uint32_t)(r >> 32); return;
        }
        if (op >= 4) {                                             /* UMULL/UMLAL/SMULL/SMLAL */
            uint64_t r;
            if (op & 2) r = (uint64_t)((int64_t)(int32_t)c->r[rm] * (int32_t)c->r[rs]); else r = (uint64_t)c->r[rm] * c->r[rs];
            if (op & 1) r += ((uint64_t)c->r[rd] << 32) | c->r[rn];
            c->r[rn] = (uint32_t)r; c->r[rd] = (uint32_t)(r >> 32);
            if (S) { c->n = (uint32_t)(r >> 63); c->z = r == 0; }
            return;
        }
        undefined(s, w); return;
    }
    /* SWP / SWPB */
    if ((w & 0x0FB00FF0) == 0x01000090) {
        unsigned rn = (w >> 16) & 0xF, rd = (w >> 12) & 0xF, rm = w & 0xF; uint32_t a = c->r[rn];
        if (w & (1 << 22)) { uint8_t o = rd8(a); wr8(a, (uint8_t)c->r[rm]); c->r[rd] = o; }
        else { uint32_t o = rd32(a); wr32(a, c->r[rm]); c->r[rd] = o; }
        return;
    }
    /* LDREX/STREX (e B/H/D) */
    if ((w & 0x0F900FF0) == 0x01800F90 || (w & 0x0F900FFF) == 0x01900F9F) {   /* (bits 7-4 = 1001: não confundir com STRH/LDRD com imediato 0xFx) */
        unsigned L = (w >> 20) & 1, sz = (w >> 21) & 3, rn = (w >> 16) & 0xF, rd = (w >> 12) & 0xF, rt = w & 0xF;
        uint32_t a = c->r[rn];
        if (L) {
            if (sz == 0) c->r[rd] = rd32(a); else if (sz == 1) { c->r[rd] = rd32(a); c->r[rd + 1] = rd32(a + 4); }
            else if (sz == 2) c->r[rd] = rd8(a); else c->r[rd] = rd16(a);
        } else {
            if (sz == 0) wr32(a, c->r[rt]); else if (sz == 1) { wr32(a, c->r[rt]); wr32(a + 4, c->r[rt + 1]); }
            else if (sz == 2) wr8(a, (uint8_t)c->r[rt]); else wr16(a, (uint16_t)c->r[rt]);
            c->r[rd] = 0;
        }
        return;
    }
    if ((w & 0x90) == 0x90 && (w & 0x60)) { arm_extra_ldst(s, w); return; }
    /* misc: BX/BLX reg, CLZ, MRS/MSR, QADD..., SMLAxy... */
    if ((w & 0x0FFFFFF0) == 0x012FFF10 || (w & 0x0FFFFFF0) == 0x012FFF20) { branch_to(s, c->r[w & 0xF], 1); return; }   /* BX / BXJ */
    if ((w & 0x0FFFFFF0) == 0x012FFF30) { uint32_t t = c->r[w & 0xF]; c->r[14] = s->pc + 4; branch_to(s, t, 1); return; } /* BLX reg */
    if ((w & 0x0FFF0FF0) == 0x016F0F10) { c->r[(w >> 12) & 0xF] = CLZ32(c->r[w & 0xF]); return; }                     /* CLZ */
    if ((w & 0x0FBF0FFF) == 0x010F0000) {                                                                              /* MRS */
        c->r[(w >> 12) & 0xF] = (w >> 22) & 1 ? 0 : (c->n << 31 | c->z << 30 | c->c << 29 | c->v << 28 | c->q << 27 | c->ge << 16 | 0x10);
        return;
    }
    if ((w & 0x0FB0FFF0) == 0x0120F000 || (w & 0x0FB0F000) == 0x0320F000) {                                            /* MSR */
        uint32_t v = (w >> 25) & 1 ? ror(w & 0xFF, ((w >> 8) & 0xF) * 2) : c->r[w & 0xF];
        if (!((w >> 22) & 1)) {
            if (w & (1 << 19)) { c->n = v >> 31; c->z = (v >> 30) & 1; c->c = (v >> 29) & 1; c->v = (v >> 28) & 1; c->q = (v >> 27) & 1; }
            if (w & (1 << 18)) c->ge = (v >> 16) & 0xF;
        }
        return;
    }
    if ((w & 0x0F900FF0) == 0x01000050) {                                                                              /* QADD/QSUB/QDADD/QDSUB */
        unsigned op = (w >> 21) & 3; int32_t a = (int32_t)c->r[w & 0xF], b = (int32_t)c->r[(w >> 16) & 0xF]; int q = 0;
        if (op & 2) b = (int32_t)ip_sat_s((int64_t)b * 2, 32, &q);
        int64_t r = op & 1 ? (int64_t)a - b : (int64_t)a + b;
        c->r[(w >> 12) & 0xF] = ip_sat_s(r, 32, &q); if (q) c->q = 1; return;
    }
    if ((w & 0x0F900090) == 0x01000080) {                                                                              /* SMLAxy etc. */
        unsigned op = (w >> 21) & 3, rd = (w >> 16) & 0xF, rn = (w >> 12) & 0xF, rs = (w >> 8) & 0xF, rm = w & 0xF;
        int x = (w >> 5) & 1, y = (w >> 6) & 1;
        int32_t a = x ? (int16_t)(c->r[rm] >> 16) : (int16_t)c->r[rm], b = y ? (int16_t)(c->r[rs] >> 16) : (int16_t)c->r[rs];
        switch (op) {
        case 0: { int64_t r = (int64_t)a * b + (int32_t)c->r[rn]; c->r[rd] = (uint32_t)r; if (r != (int32_t)r) c->q = 1; return; }       /* SMLAxy */
        case 1: { int32_t bb = y ? (int16_t)(c->r[rs] >> 16) : (int16_t)c->r[rs];
                  int64_t p = ((int64_t)(int32_t)c->r[rm] * bb) >> 16;
                  if (!x) { int64_t r = p + (int32_t)c->r[rn]; c->r[rd] = (uint32_t)r; if (r != (int32_t)r) c->q = 1; }   /* SMLAWy */
                  else c->r[rd] = (uint32_t)p;                                                                             /* SMULWy */
                  return; }
        case 2: { int64_t r = ((int64_t)c->r[rd] << 32 | c->r[rn]) + (int64_t)a * b; c->r[rn] = (uint32_t)r; c->r[rd] = (uint32_t)((uint64_t)r >> 32); return; } /* SMLALxy */
        default: c->r[rd] = (uint32_t)(a * b); return;                                                                    /* SMULxy */
        }
    }
    undefined(s, w);
}

static void arm_media(St *s, uint32_t w) {       /* espaço 011 com bit 4 */
    Ctx *c = s->c;
    unsigned op1 = (w >> 20) & 0x1F, rn = (w >> 16) & 0xF, rd = (w >> 12) & 0xF, rm = w & 0xF, op2 = (w >> 5) & 7;
    if (op1 < 8) {                                                    /* paralelas */
        uint32_t r; if (parallel(c, w, c->r[rn], c->r[rm], &r)) { c->r[rd] = r; return; }
        undefined(s, w); return;
    }
    if ((op1 & 0x18) == 0x08) {                                       /* PKH, SXT/UXT, SEL, REV, SSAT/USAT */
        if ((w & 0x0FF00030) == 0x06800010) {                         /* PKHBT / PKHTB */
            unsigned sh = (w >> 7) & 0x1F; uint32_t b;
            if (w & 0x40) { b = sh ? (uint32_t)((int32_t)c->r[rm] >> sh) : (uint32_t)((int32_t)c->r[rm] >> 31);
                            c->r[rd] = (c->r[rn] & 0xFFFF0000) | (b & 0xFFFF); }
            else { b = c->r[rm] << sh; c->r[rd] = (c->r[rn] & 0xFFFF) | (b & 0xFFFF0000); }
            return;
        }
        if ((w & 0x0F8003F0) == 0x06800070) {                         /* SXTAB16/SXTB16/SXTAB/SXTB/SXTAH/SXTH/UXT... */
            unsigned op = (w >> 20) & 7; uint32_t e = extend(c->r[rm], (w >> 10) & 3, op);
            if (rn == 15) c->r[rd] = e;
            else if (op == 0 || op == 4) c->r[rd] = (((c->r[rn] & 0xFFFF) + (e & 0xFFFF)) & 0xFFFF) | (((c->r[rn] >> 16) + (e >> 16)) << 16);
            else c->r[rd] = c->r[rn] + e;
            return;
        }
        if ((w & 0x0FF00FF0) == 0x06800FB0) {                         /* SEL */
            uint32_t r = 0; for (int i = 0; i < 4; i++) r |= ((c->ge >> i) & 1 ? c->r[rn] : c->r[rm]) & (0xFFu << (8 * i));
            c->r[rd] = r; return;
        }
        if ((w & 0x0FFF0FF0) == 0x06BF0F30) { c->r[rd] = __builtin_bswap32(c->r[rm]); return; }                          /* REV */
        if ((w & 0x0FFF0FF0) == 0x06BF0FB0) { uint32_t v = c->r[rm]; c->r[rd] = ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF); return; } /* REV16 */
        if ((w & 0x0FFF0FF0) == 0x06FF0FB0) { uint32_t v = c->r[rm]; c->r[rd] = (uint32_t)(int32_t)(int16_t)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF)); return; } /* REVSH */
        if ((w & 0x0FA00030) == 0x06A00010) {                         /* SSAT / USAT */
            unsigned sat = (w >> 16) & 0x1F, sh = (w >> 7) & 0x1F; int q = 0; uint32_t v = c->r[rm];
            int64_t x = (w & 0x40) ? (int64_t)((int32_t)v >> (sh ? sh : 31)) : (int64_t)(int32_t)(v << sh);
            if ((w >> 22) & 1) c->r[rd] = ip_sat_u(x, sat, &q); else c->r[rd] = ip_sat_s(x, sat + 1, &q);
            if (q) c->q = 1; return;
        }
        if ((w & 0x0FA00FF0) == 0x06A00F30) {                         /* SSAT16 / USAT16 */
            unsigned sat = (w >> 16) & 0xF; int q = 0; uint32_t v = c->r[rm], lo, hi;
            if ((w >> 22) & 1) { lo = ip_sat_u((int16_t)v, sat, &q); hi = ip_sat_u((int16_t)(v >> 16), sat, &q); }
            else { lo = ip_sat_s((int16_t)v, sat + 1, &q); hi = ip_sat_s((int16_t)(v >> 16), sat + 1, &q); }
            c->r[rd] = (lo & 0xFFFF) | (hi << 16); if (q) c->q = 1; return;
        }
        undefined(s, w); return;
    }
    if ((op1 & 0x18) == 0x10) {                                       /* multiplicações com sinal (SMLAD, SMUAD, SMMUL...) */
        unsigned ra = rd, rdd = rn, rs = (w >> 8) & 0xF; int x = (w >> 5) & 1;
        uint32_t m2 = x ? ror(c->r[rs], 16) : c->r[rs];
        int64_t p1 = (int64_t)(int16_t)c->r[rm] * (int16_t)m2, p2 = (int64_t)(int16_t)(c->r[rm] >> 16) * (int16_t)(m2 >> 16);
        if (op1 == 0x10) {                                            /* SMLAD/SMUAD/SMLSD/SMUSD */
            int64_t r = (op2 & 2) ? p1 - p2 : p1 + p2;
            if (ra != 15) r += (int32_t)c->r[ra];
            c->r[rdd] = (uint32_t)r; if (r != (int32_t)r) c->q = 1; return;
        }
        if (op1 == 0x14) {                                            /* SMLALD/SMLSLD */
            int64_t r = (op2 & 2) ? p1 - p2 : p1 + p2;
            r += (int64_t)((uint64_t)c->r[rdd] << 32 | c->r[ra]);
            c->r[ra] = (uint32_t)r; c->r[rdd] = (uint32_t)((uint64_t)r >> 32); return;
        }
        if (op1 == 0x15) {                                            /* SMMUL/SMMLA/SMMLS */
            int64_t p = (int64_t)(int32_t)c->r[rm] * (int32_t)c->r[rs];
            int64_t r;
            if ((op2 & 6) == 6) r = ((int64_t)((uint64_t)c->r[ra] << 32)) - p;   /* SMMLS */
            else r = p + (ra != 15 ? (int64_t)((uint64_t)c->r[ra] << 32) : 0);
            if (op2 & 1) r += 0x80000000LL;                           /* R: arredonda */
            c->r[rdd] = (uint32_t)((uint64_t)r >> 32); return;
        }
        undefined(s, w); return;
    }
    if (op1 == 0x18 && op2 == 0) {                                    /* USAD8 / USADA8 */
        unsigned rs = (w >> 8) & 0xF; uint32_t r = 0;
        for (int i = 0; i < 4; i++) { int d = (int)((c->r[rm] >> (8 * i)) & 0xFF) - (int)((c->r[rs] >> (8 * i)) & 0xFF); r += (uint32_t)(d < 0 ? -d : d); }
        if (rd != 15) r += c->r[rd];
        c->r[rn] = r; return;
    }
    undefined(s, w);
}

static void arm_step(St *s, uint32_t w) {
    Ctx *c = s->c;
    unsigned cc = w >> 28;
    if (cc == 0xF) {                                                  /* incondicionais */
        if ((w & 0xFE000000) == 0xFA000000) {                         /* BLX imediato */
            uint32_t t = s->pc + 8 + (uint32_t)(((int32_t)(w << 8)) >> 6) + ((w >> 23) & 2);
            c->r[14] = s->pc + 4; branch_to(s, t | 1, 1); return;
        }
        if ((w & 0x0D70F000) == 0x0550F000) return;                   /* PLD */
        if (w == 0xF57FF01F) return;                                  /* CLREX */
        if ((w & 0xFFF1FE20) == 0xF1000000) return;                   /* CPS */
        undefined(s, w); return;
    }
    if (!cond_ok(c, cc)) return;
    unsigned grp = (w >> 25) & 7;
    switch (grp) {
    case 0:
        if ((w & 0x90) == 0x90 || (w & 0x01900000) == 0x01000000) { arm_misc(s, w); return; }
        arm_dp(s, w); return;
    case 1:
        if ((w & 0x0FB00000) == 0x03000000) { undefined(s, w); return; }       /* MOVW/MOVT (ARMv6T2; não existe no ARM11) */
        if ((w & 0x0FB00000) == 0x03200000) {                                   /* MSR imediato / dicas */
            if ((w & 0x000F0000) == 0) return;                                  /* NOP/YIELD/WFE/WFI/SEV */
            arm_misc(s, w); return;
        }
        arm_dp(s, w); return;
    case 2: arm_ldst(s, w); return;
    case 3: if (w & 0x10) arm_media(s, w); else arm_ldst(s, w); return;
    case 4: arm_ldm_stm(s, w); return;
    case 5: {                                                         /* B / BL */
        uint32_t t = s->pc + 8 + (uint32_t)(((int32_t)(w << 8)) >> 6);
        if ((w >> 24) & 1) c->r[14] = s->pc + 4;
        branch_to(s, t, 0); return;
    }
    case 6: case 7:
        if (grp == 7 && (w & 0x0F000000) == 0x0F000000) {              /* SVC */
            s->why = IP_SVC; s->info = w & 0xFFFFFF; s->stop = 3; return;
        }
        if ((w & 0x0F100F10) == 0x0E100F10 && ((w >> 8) & 0xF) == 15) {  /* MRC p15 (TLS) */
            if ((w & 0x00EF00EF) == 0x000D0060 || (w & 0x0FFF0FFF) == 0x0E1D0F70) { unsigned rt = (w >> 12) & 0xF; if (rt != 15) c->r[rt] = c->tls; return; }
            undefined(s, w); return;
        }
        if ((w & 0x0F100F10) == 0x0E000F10 && ((w >> 8) & 0xF) == 15) return;   /* MCR p15 (barreiras etc.) */
        if (vfp_exec(s, w)) return;
        undefined(s, w); return;
    }
}

/* ------------------------------------------------------------ Thumb */
static void th_step(St *s, uint16_t h) {
    Ctx *c = s->c;
    unsigned top = h >> 11;
    switch (h >> 13) {
    case 0:
        if (top == 3) {                                                /* ADD/SUB reg/imm3 */
            unsigned rd = h & 7, rn = (h >> 3) & 7; uint32_t b = (h >> 10) & 1 ? (h >> 6) & 7 : c->r[(h >> 6) & 7];
            c->r[rd] = (h >> 9) & 1 ? add_flags(c, c->r[rn], ~b, 1, 1) : add_flags(c, c->r[rn], b, 0, 1);
            return;
        } else {                                                       /* LSL/LSR/ASR imediato */
            unsigned rd = h & 7, rm = (h >> 3) & 7, amt = (h >> 6) & 0x1F; uint32_t co;
            uint32_t r = shift_c(c->r[rm], top, amt, 0, c->c, &co);
            c->r[rd] = r; nz(c, r); c->c = co; return;
        }
    case 1: {                                                          /* MOV/CMP/ADD/SUB imm8 */
        unsigned rd = (h >> 8) & 7, op = (h >> 11) & 3; uint32_t imm = h & 0xFF;
        switch (op) {
        case 0: c->r[rd] = imm; nz(c, imm); break;
        case 1: add_flags(c, c->r[rd], ~imm, 1, 1); break;
        case 2: c->r[rd] = add_flags(c, c->r[rd], imm, 0, 1); break;
        default: c->r[rd] = add_flags(c, c->r[rd], ~imm, 1, 1); break;
        }
        return;
    }
    case 2:
        if ((h >> 10) == 0x10) {                                       /* ALU */
            unsigned op = (h >> 6) & 0xF, rd = h & 7, rm = (h >> 3) & 7; uint32_t a = c->r[rd], b = c->r[rm], r, co;
            switch (op) {
            case 0x0: r = a & b; c->r[rd] = r; nz(c, r); break;
            case 0x1: r = a ^ b; c->r[rd] = r; nz(c, r); break;
            case 0x2: r = shift_c(a, 0, b, 1, c->c, &co); c->r[rd] = r; nz(c, r); c->c = co; break;
            case 0x3: r = shift_c(a, 1, b, 1, c->c, &co); c->r[rd] = r; nz(c, r); c->c = co; break;
            case 0x4: r = shift_c(a, 2, b, 1, c->c, &co); c->r[rd] = r; nz(c, r); c->c = co; break;
            case 0x5: c->r[rd] = add_flags(c, a, b, c->c, 1); break;
            case 0x6: c->r[rd] = add_flags(c, a, ~b, c->c, 1); break;
            case 0x7: r = shift_c(a, 3, b, 1, c->c, &co); c->r[rd] = r; nz(c, r); c->c = co; break;
            case 0x8: nz(c, a & b); break;
            case 0x9: c->r[rd] = add_flags(c, 0, ~b, 1, 1); break;
            case 0xA: add_flags(c, a, ~b, 1, 1); break;
            case 0xB: add_flags(c, a, b, 0, 1); break;
            case 0xC: r = a | b; c->r[rd] = r; nz(c, r); break;
            case 0xD: r = a * b; c->r[rd] = r; nz(c, r); break;
            case 0xE: r = a & ~b; c->r[rd] = r; nz(c, r); break;
            default: r = ~b; c->r[rd] = r; nz(c, r); break;
            }
            return;
        }
        if ((h >> 10) == 0x11) {                                       /* hi regs / BX */
            unsigned op = (h >> 8) & 3, rd = (h & 7) | ((h >> 4) & 8), rm = (h >> 3) & 0xF;
            uint32_t b = rget(s, rm);
            switch (op) {
            case 0: { uint32_t r = rget(s, rd) + b; if (rd == 15) branch_to(s, r, 0); else c->r[rd] = r; return; }
            case 1: add_flags(c, rget(s, rd), ~b, 1, 1); return;
            case 2: if (rd == 15) branch_to(s, b, 0); else c->r[rd] = b; return;
            default:
                if (h & 0x80) { c->r[14] = (s->pc + 2) | 1; }          /* BLX reg */
                branch_to(s, b, 1); return;
            }
        }
        if ((h >> 11) == 9) {                                          /* LDR literal */
            c->r[(h >> 8) & 7] = rd32(((s->pc + 4) & ~3u) + (h & 0xFF) * 4); return;
        }
        {                                                               /* load/store com registrador */
            unsigned op = (h >> 9) & 7, rd = h & 7; uint32_t a = c->r[(h >> 3) & 7] + c->r[(h >> 6) & 7];
            switch (op) {
            case 0: wr32(a, c->r[rd]); break; case 1: wr16(a, (uint16_t)c->r[rd]); break;
            case 2: wr8(a, (uint8_t)c->r[rd]); break; case 3: c->r[rd] = (uint32_t)(int32_t)(int8_t)rd8(a); break;
            case 4: c->r[rd] = rd32(a); break; case 5: c->r[rd] = rd16(a); break;
            case 6: c->r[rd] = rd8(a); break; default: c->r[rd] = (uint32_t)(int32_t)(int16_t)rd16(a); break;
            }
            return;
        }
    case 3: {                                                          /* load/store imm5 (palavra/byte) */
        unsigned B = (h >> 12) & 1, L = (h >> 11) & 1, rd = h & 7; uint32_t imm = (h >> 6) & 0x1F;
        uint32_t a = c->r[(h >> 3) & 7] + (B ? imm : imm * 4);
        if (L) c->r[rd] = B ? rd8(a) : rd32(a); else if (B) wr8(a, (uint8_t)c->r[rd]); else wr32(a, c->r[rd]);
        return;
    }
    case 4:
        if (!((h >> 12) & 1)) {                                        /* halfword imm5 */
            unsigned L = (h >> 11) & 1, rd = h & 7; uint32_t a = c->r[(h >> 3) & 7] + ((h >> 6) & 0x1F) * 2;
            if (L) c->r[rd] = rd16(a); else wr16(a, (uint16_t)c->r[rd]); return;
        } else {                                                        /* SP-relativo */
            unsigned L = (h >> 11) & 1, rd = (h >> 8) & 7; uint32_t a = c->r[13] + (h & 0xFF) * 4;
            if (L) c->r[rd] = rd32(a); else wr32(a, c->r[rd]); return;
        }
    case 5:
        if (!((h >> 12) & 1)) {                                        /* ADD rd, pc/sp, imm */
            unsigned rd = (h >> 8) & 7; uint32_t imm = (h & 0xFF) * 4;
            c->r[rd] = ((h >> 11) & 1) ? c->r[13] + imm : ((s->pc + 4) & ~3u) + imm; return;
        }
        if ((h & 0xFF00) == 0xB000) { uint32_t imm = (h & 0x7F) * 4; c->r[13] += (h & 0x80) ? -imm : imm; return; }   /* ADD/SUB sp */
        if ((h & 0xF600) == 0xB400) {                                  /* PUSH / POP */
            unsigned L = (h >> 11) & 1, R = (h >> 8) & 1; uint32_t list = h & 0xFF;
            unsigned cnt = (unsigned)__builtin_popcount(list) + R;
            if (!L) {
                uint32_t a = c->r[13] - cnt * 4; c->r[13] = a;
                for (int r = 0; r < 8; r++) if (list >> r & 1) { wr32(a, c->r[r]); a += 4; }
                if (R) wr32(a, c->r[14]);
            } else {
                uint32_t a = c->r[13];
                for (int r = 0; r < 8; r++) if (list >> r & 1) { c->r[r] = rd32(a); a += 4; }
                uint32_t pc = 0; if (R) { pc = rd32(a); a += 4; }
                c->r[13] = a;
                if (R) branch_to(s, pc, 1);
            }
            return;
        }
        if ((h & 0xFF00) == 0xB200) {                                  /* SXTH/SXTB/UXTH/UXTB */
            unsigned rd = h & 7, rm = (h >> 3) & 7, op = (h >> 6) & 3; uint32_t v = c->r[rm];
            c->r[rd] = op == 0 ? (uint32_t)(int32_t)(int16_t)v : op == 1 ? (uint32_t)(int32_t)(int8_t)v : op == 2 ? (v & 0xFFFF) : (v & 0xFF);
            return;
        }
        if ((h & 0xFF00) == 0xBA00) {                                  /* REV/REV16/REVSH */
            unsigned rd = h & 7, rm = (h >> 3) & 7, op = (h >> 6) & 3; uint32_t v = c->r[rm];
            if (op == 0) c->r[rd] = __builtin_bswap32(v);
            else if (op == 1) c->r[rd] = ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF);
            else if (op == 3) c->r[rd] = (uint32_t)(int32_t)(int16_t)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF));
            else undefined(s, h);
            return;
        }
        if ((h & 0xFFE8) == 0xB660 || (h & 0xFF00) == 0xBF00) return;  /* CPS / dicas */
        if ((h & 0xFF00) == 0xBE00) { s->why = IP_UNDEF; s->info = h; s->stop = 2; return; }   /* BKPT */
        undefined(s, h); return;
    case 6:
        if (!((h >> 12) & 1)) {                                        /* LDMIA / STMIA */
            unsigned L = (h >> 11) & 1, rn = (h >> 8) & 7; uint32_t list = h & 0xFF, a = c->r[rn];
            for (int r = 0; r < 8; r++) if (list >> r & 1) { if (L) c->r[r] = rd32(a); else wr32(a, c->r[r]); a += 4; }
            if (!L || !(list >> rn & 1)) c->r[rn] = a;
            return;
        }
        if ((h & 0xFF00) == 0xDF00) { s->why = IP_SVC; s->info = h & 0xFF; s->stop = 3; return; }   /* SVC */
        if ((h & 0xFF00) == 0xDE00) { undefined(s, h); return; }
        {                                                               /* B condicional */
            if (cond_ok(c, (h >> 8) & 0xF)) branch_to(s, s->pc + 4 + (uint32_t)((int32_t)(int8_t)(h & 0xFF) * 2), 0);
            return;
        }
    default:
        if (top == 0x1C) { branch_to(s, s->pc + 4 + (uint32_t)(((int32_t)(h << 21)) >> 20), 0); return; }   /* B */
        if (top == 0x1E) { c->r[14] = s->pc + 4 + (uint32_t)(((int32_t)(h << 21)) >> 9); return; }          /* BL prefixo */
        if (top == 0x1F || top == 0x1D) {                               /* BL / BLX sufixo */
            uint32_t t = c->r[14] + (h & 0x7FF) * 2;
            c->r[14] = (s->pc + 2) | 1;
            if (top == 0x1D) branch_to(s, t & ~3u, 1); else branch_to(s, t | 1, 1);
            return;
        }
        undefined(s, h); return;
    }
}

/* ------------------------------------------------------------ execução */
/* Executa a partir de c->r[15] (bit 0 = Thumb) até chegar em `ret` (endereço de retorno), achar uma syscall,
 * uma instrução que não conhece ou passar de `max_steps`. As syscalls param (why = IP_SVC, c->r[15] na seguinte)
 * a menos que `do_svc` seja 1: aí são executadas (modo reserva). */
IpExit ip_run(Ctx *c, uint32_t start, uint32_t ret, long max_steps, int do_svc, long *steps_out) {
    St s = { c, (int)(start & 1), start & ~1u, 0, ret, IP_RET, 0 };
    uint32_t sp0 = c->r[13];
    long n = 0;
    for (;;) {
        if ((s.pc & ~1u) == (ret & ~1u) && n > 0 && c->r[13] >= sp0) { s.why = IP_RET; break; }   /* (com a pilha de volta: não é uma chamada interna ao mesmo endereço) */
        if (n >= max_steps) { s.why = IP_STEPS; break; }
        s.stop = 0; c->r[15] = s.pc; t_curpc = s.pc | (uint32_t)s.thumb;
        if (s.thumb) {
            uint16_t h = rd16(s.pc);
            th_step(&s, h);
            if (!s.stop) s.pc += 2; else if (s.stop == 1) s.pc = c->r[15];
        } else {
            uint32_t w = rd32(s.pc);
            arm_step(&s, w);
            if (!s.stop) s.pc += 4; else if (s.stop == 1) s.pc = c->r[15];
        }
        n++;
        if (do_svc && !(n & 0xFFFF) && ip_tick_hook) ip_tick_hook();   /* (reserva: deixa as outras threads andarem) */
        if (s.stop == 2) { c->r[15] = s.pc | (uint32_t)s.thumb; break; }
        if (s.stop == 3) {
            s.pc += s.thumb ? 2 : 4;
            if (!do_svc) { c->r[15] = s.pc | (uint32_t)s.thumb; break; }
            c->r[15] = s.pc;
            svc(c, s.info);
        }
    }
    c->r[15] = s.pc | (uint32_t)s.thumb;
    if (steps_out) *steps_out = n;
    ip_last_info = s.info;
    return s.why;
}

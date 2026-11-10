/* PICA200: interpretador de shaders de vértice.
 * Conjunto de instruções: 3dbrew "Shader Instruction Set". Registradores: v0-v15 (entradas),
 * r0-r15 (temporários), c0-c95 (uniforms float), i0-i3 (inteiros), b0-b15 (booleanos),
 * a0.x/a0.y/aL (endereço), o0-o15 (saídas). Precisão: float32 (o hardware usa float24).
 */
#include "pica.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

ShaderState g_vs, g_gs;
int g_vs_trace;   /* imprime cada instrução (depuração de NaN nos vértices) */

float f24_to_f32(uint32_t raw) {
    raw &= 0xFFFFFF;
    uint32_t sign = raw >> 23, exp = (raw >> 16) & 0x7F, mant = raw & 0xFFFF;
    if (!exp && !mant) return sign ? -0.0f : 0.0f;
    uint32_t bits;
    if (exp == 0x7F) bits = (sign << 31) | (0xFFu << 23) | (mant << 7);
    else bits = (sign << 31) | ((exp + 64) << 23) | (mant << 7);   /* viés 63 -> 127 */
    float f; memcpy(&f, &bits, 4); return f;
}

/* multiplicação da PICA: 0 * inf = 0 (em vez de NaN) */
static inline float pmul(float a, float b) {
    float r = a * b;
    if (r != r && a == a && b == b) return 0.0f;
    return r;
}

static __thread const ShaderState *cur_st;   /* programa em execução (g_vs ou g_gs) */

void sh_desc_written(ShaderState *st) { memset(st->dok, 0, sizeof st->dok); }

/* decodifica a instrução aritmética em pc (fontes, swizzles, máscara) para não repetir isso a cada vértice */
static void decode(ShaderState *st, uint32_t pc) {
    uint32_t ins = st->code[pc], op = ins >> 26;
    DIns *D = &st->dec[pc]; memset(D, 0, sizeof *D);
    D->op = (uint8_t)op; D->relsrc = 0xFF;
    if (op < 0x20 || op >= 0x2E) {
        uint32_t d, s1, s2, s3 = 0, idx;
        if (op >= 0x30) {
            d = st->opdesc[ins & 0x1F];
            D->dst = (ins >> 24) & 0x1F; idx = (ins >> 22) & 3; s1 = (ins >> 17) & 0x1F;
            if (op >= 0x38) { s2 = (ins >> 10) & 0x7F; s3 = (ins >> 5) & 0x1F; D->relsrc = 1; }
            else { s2 = (ins >> 12) & 0x1F; s3 = (ins >> 5) & 0x7F; D->relsrc = 2; }
        } else {
            d = st->opdesc[ins & 0x7F];
            idx = (ins >> 19) & 3;
            if (op == 0x2E || op == 0x2F) {
                s1 = (ins >> 12) & 0x7F; s2 = (ins >> 7) & 0x1F; D->relsrc = 0;
                D->dst = (ins >> 24) & 7; D->mask = (ins >> 21) & 7;      /* CMP: operadores x e y */
            } else {
                D->dst = (ins >> 21) & 0x1F;
                if (op >= 0x18 && op <= 0x1B) { s1 = (ins >> 14) & 0x1F; s2 = (ins >> 7) & 0x7F; D->relsrc = 1; }
                else { s1 = (ins >> 12) & 0x7F; s2 = (ins >> 7) & 0x1F; D->relsrc = 0; }
            }
        }
        if (!(op == 0x2E || op == 0x2F)) D->mask = d & 0xF;
        D->rel = (uint8_t)idx;
        D->src[0] = (uint8_t)s1; D->src[1] = (uint8_t)s2; D->src[2] = (uint8_t)s3;
        uint32_t sel[3] = { (d >> 5) & 0xFF, (d >> 14) & 0xFF, (d >> 23) & 0xFF };
        D->neg[0] = (d >> 4) & 1; D->neg[1] = (d >> 13) & 1; D->neg[2] = (d >> 22) & 1;
        for (int k = 0; k < 3; k++) for (int c = 0; c < 4; c++) D->sw[k][c] = (sel[k] >> (6 - 2 * c)) & 3;
    }
    __atomic_store_n(&st->dok[pc], 1, __ATOMIC_RELEASE);
}

/* decodifica de antemão as instruções ainda não decodificadas (antes de rodar em várias threads,
 * para que nenhuma thread leia uma instrução enquanto outra a decodifica) */
void sh_prepare(ShaderState *st, uint32_t n) {
    for (uint32_t pc = 0; pc < n && pc < 4096; pc++) if (!st->dok[pc]) decode(st, pc);
}

/* lê uma fonte já decodificada (k = 0..2), aplicando o registrador de endereço, swizzle e negação */
static inline void fetch(const ShaderRun *s, const DIns *D, int k, float out[4]) {
    uint32_t idx = D->src[k];
    const float *v;
    if (idx < 0x10) v = s->in[idx];
    else if (idx < 0x20) v = s->tmp[idx - 0x10];
    else {
        int u = (int)idx - 0x20;
        if (D->rel && D->relsrc == k) {
            u += D->rel == 1 ? s->a0[0] : D->rel == 2 ? s->a0[1] : s->aL;
            if (u < 0 || u > 95) u = 0;      /* fora da faixa: usamos c0 */
        }
        v = cur_st->uniform_f[u & 0x7F];
    }
    const uint8_t *sw = D->sw[k];
    if (D->neg[k]) { out[0] = -v[sw[0]]; out[1] = -v[sw[1]]; out[2] = -v[sw[2]]; out[3] = -v[sw[3]]; }
    else { out[0] = v[sw[0]]; out[1] = v[sw[1]]; out[2] = v[sw[2]]; out[3] = v[sw[3]]; }
}

static inline void store_dst(ShaderRun *s, uint32_t dst, uint32_t mask, const float v[4]) {
    float *d = dst < 0x10 ? (s->bank ? s->out_alt[dst] : s->out[dst]) : s->tmp[(dst - 0x10) & 15];
    if (mask == 0xF) { d[0] = v[0]; d[1] = v[1]; d[2] = v[2]; d[3] = v[3]; return; }
    for (int k = 0; k < 4; k++) if (mask & (8 >> k)) d[k] = v[k];
}

static int eval_cond(const ShaderRun *s, uint32_t op, uint32_t refx, uint32_t refy) {
    int x = s->cmp[0] == (int)refx, y = s->cmp[1] == (int)refy;
    switch (op) { case 0: return x || y; case 1: return x && y; case 2: return x; default: return y; }
}

static int compare(float a, float b, uint32_t op) {
    switch (op) {
    case 0: return a == b; case 1: return a != b; case 2: return a < b;
    case 3: return a <= b; case 4: return a > b; case 5: return a >= b; default: return 1;
    }
}

typedef struct { uint32_t final_addr, return_addr, loop_addr; int repeat, incr, is_loop; } CallFrame;

void vs_run(ShaderRun *s) { sh_run(&g_vs, s, NULL); }

void sh_run(const ShaderState *st, ShaderRun *s, Emitter *em) {
    CallFrame stack[16]; int sp = 0;
    const ShaderState *prev_st = cur_st; cur_st = st;
    uint32_t pc = st->entry;
    memset(s->tmp, 0, sizeof s->tmp);
    s->a0[0] = s->a0[1] = s->aL = 0; s->cmp[0] = s->cmp[1] = 0;
    if (g_vs_trace) for (int i = 0; i < 12; i++) fprintf(stderr, "[vs] v%d = %g %g %g %g\n", i, s->in[i][0], s->in[i][1], s->in[i][2], s->in[i][3]);
    for (int steps = 0; steps < 100000; steps++) {
        /* fim de bloco de chamada/laço/if */
        while (sp && pc == stack[sp - 1].final_addr) {
            CallFrame *f = &stack[sp - 1];
            s->aL += f->incr;
            if (f->repeat-- == 0) { pc = f->return_addr; sp--; }
            else { pc = f->loop_addr; break; }
        }
        if (pc >= 4096) break;
        uint32_t ins = st->code[pc], op = ins >> 26;
        uint32_t next = pc + 1;
        if (g_vs_trace) {
            fprintf(stderr, "[vs] %03x op %02x ins %08x desc %08x | a0 %d %d aL %d |", pc, op, ins, st->opdesc[ins & 0x7F], s->a0[0], s->a0[1], s->aL);
            for (int r = 0; r < 8; r++) fprintf(stderr, " r%d=(%g %g %g %g)", r, s->tmp[r][0], s->tmp[r][1], s->tmp[r][2], s->tmp[r][3]);
            fprintf(stderr, "\n");
        }
        #define CALL(off, n, ret, rep, inc, loop) do { if (sp < 16) { stack[sp++] = (CallFrame){ (off) + (n), (ret), (off), (rep), (inc), (loop) }; } next = (off); } while (0)
        if (op < 0x20 || (op >= 0x2E)) {
            /* ------------------------------------------------ aritméticas */
            if (__builtin_expect(!__atomic_load_n(&st->dok[pc], __ATOMIC_ACQUIRE), 0)) decode((ShaderState *)st, pc);
            const DIns *D = &st->dec[pc];
            float a[4], b[4], c3[4], r[4];
            if (op >= 0x30) {                          /* MAD (0x38-0x3F) / MADI (0x30-0x37) */
                fetch(s, D, 0, a); fetch(s, D, 1, b); fetch(s, D, 2, c3);
                for (int k = 0; k < 4; k++) r[k] = pmul(a[k], b[k]) + c3[k];
                store_dst(s, D->dst, D->mask, r);
                pc = next; continue;
            }
            fetch(s, D, 0, a); fetch(s, D, 1, b);
            if (op == 0x2E || op == 0x2F) {            /* CMP */
                s->cmp[0] = compare(a[0], b[0], D->dst); s->cmp[1] = compare(a[1], b[1], D->mask);
                pc = next; continue;
            }
            uint32_t mask = D->mask, dst = D->dst;
            switch (op) {
            case 0x00: for (int k = 0; k < 4; k++) r[k] = a[k] + b[k]; break;                       /* ADD */
            case 0x01: { float t = pmul(a[0], b[0]) + pmul(a[1], b[1]) + pmul(a[2], b[2]); for (int k = 0; k < 4; k++) r[k] = t; break; }
            case 0x02: { float t = pmul(a[0], b[0]) + pmul(a[1], b[1]) + pmul(a[2], b[2]) + pmul(a[3], b[3]); for (int k = 0; k < 4; k++) r[k] = t; break; }
            case 0x03: case 0x18: { float t = pmul(a[0], b[0]) + pmul(a[1], b[1]) + pmul(a[2], b[2]) + b[3]; for (int k = 0; k < 4; k++) r[k] = t; break; }
            case 0x04: case 0x19: r[0] = 1; r[1] = pmul(a[1], b[1]); r[2] = a[2]; r[3] = b[3]; break;  /* DST */
            case 0x05: { float t = exp2f(a[0]); for (int k = 0; k < 4; k++) r[k] = t; break; }          /* EX2 */
            case 0x06: { float t = log2f(a[0]); for (int k = 0; k < 4; k++) r[k] = t; break; }          /* LG2 */
            case 0x07: {                                                                                /* LITP */
                r[0] = fmaxf(a[0], 0); r[1] = fminf(fmaxf(a[1], -127.9961f), 127.9961f);
                r[2] = fmaxf(a[2], 0); r[3] = fmaxf(a[3], 0);
                s->cmp[0] = a[0] >= 0; s->cmp[1] = a[3] >= 0; break; }
            case 0x08: for (int k = 0; k < 4; k++) r[k] = pmul(a[k], b[k]); break;                    /* MUL */
            case 0x09: case 0x1A: for (int k = 0; k < 4; k++) r[k] = a[k] >= b[k] ? 1.0f : 0.0f; break;
            case 0x0A: case 0x1B: for (int k = 0; k < 4; k++) r[k] = a[k] < b[k] ? 1.0f : 0.0f; break;
            case 0x0B: for (int k = 0; k < 4; k++) r[k] = floorf(a[k]); break;                         /* FLR */
            case 0x0C: for (int k = 0; k < 4; k++) r[k] = a[k] > b[k] ? a[k] : b[k]; break;           /* MAX */
            case 0x0D: for (int k = 0; k < 4; k++) r[k] = a[k] < b[k] ? a[k] : b[k]; break;           /* MIN */
            case 0x0E: { float t = 1.0f / a[0]; for (int k = 0; k < 4; k++) r[k] = t; break; }        /* RCP */
            case 0x0F: { float t = 1.0f / sqrtf(a[0]); for (int k = 0; k < 4; k++) r[k] = t; break; } /* RSQ */
            case 0x12:                                                                                  /* MOVA */
                if (mask & 8) s->a0[0] = (int)a[0];
                if (mask & 4) s->a0[1] = (int)a[1];
                pc = next; continue;
            case 0x13: for (int k = 0; k < 4; k++) r[k] = a[k]; break;                                 /* MOV */
            default:
                { static int warned[64]; if (!warned[op]) { warned[op] = 1; fprintf(stderr, "[pica] instrução de shader %02x não suportada\n", op); } }
                pc = next; continue;
            }
            store_dst(s, dst, mask, r);
            pc = next; continue;
        }
        /* ------------------------------------------------ controle de fluxo */
        uint32_t dst_offs = (ins >> 10) & 0xFFF, num = ins & 0xFF;
        uint32_t condop = (ins >> 22) & 3, refx = (ins >> 25) & 1, refy = (ins >> 24) & 1;
        uint32_t bool_id = (ins >> 22) & 0xF, int_id = (ins >> 22) & 3;
        int b = (st->uniform_b >> bool_id) & 1;
        switch (op) {
        case 0x20: /* BREAK */
        case 0x23: /* BREAKC */
            if (op == 0x20 || eval_cond(s, condop, refx, refy)) {
                while (sp && !stack[sp - 1].is_loop) sp--;   /* sai do laço mais interno */
                if (sp) { next = stack[sp - 1].return_addr; sp--; }
            }
            break;
        case 0x21: break;                                   /* NOP */
        case 0x22: cur_st = prev_st; return;                /* END */
        case 0x24: CALL(dst_offs, num, pc + 1, 0, 0, 0); break;
        case 0x25: if (eval_cond(s, condop, refx, refy)) CALL(dst_offs, num, pc + 1, 0, 0, 0); break;
        case 0x26: if (b) CALL(dst_offs, num, pc + 1, 0, 0, 0); break;
        case 0x27: case 0x28: {                             /* IFU / IFC */
            int cnd = op == 0x27 ? b : eval_cond(s, condop, refx, refy);
            if (cnd) CALL(pc + 1, dst_offs - pc - 1, dst_offs + num, 0, 0, 0);
            else CALL(dst_offs, num, dst_offs + num, 0, 0, 0);
            break;
        }
        case 0x29: {                                        /* LOOP */
            const uint8_t *iv = st->uniform_i[int_id];
            s->aL = iv[1];
            CALL(pc + 1, dst_offs - pc, dst_offs + 1, iv[0], (int8_t)iv[2], 1);
            break;
        }
        case 0x2A:                                          /* EMIT (só geometria) */
            if (em) { em->emit(em, s->bank ? s->out_alt : s->out); s->bank = !s->bank; }
            break;
        case 0x2B:                                          /* SETEMIT */
            if (em) { em->vertex_id = (ins >> 24) & 3; em->prim_emit = (ins >> 23) & 1; em->winding = (ins >> 22) & 1; }
            break;
        case 0x2C: if (eval_cond(s, condop, refx, refy)) next = dst_offs; break;     /* JMPC */
        case 0x2D: if (b != (int)(num & 1)) next = dst_offs; break;                   /* JMPU (bit 0 inverte) */
        default:
            { static int warned[64]; if (!warned[op]) { warned[op] = 1; fprintf(stderr, "[pica] instrução de shader %02x não suportada\n", op); } }
        }
        pc = next;
        #undef CALL
    }
    cur_st = prev_st;
}

/* ======================================================================================
 * Interpretador em lote: cada instrução é aplicada a SB_L vértices de uma vez (os laços por
 * vértice viram SIMD). O fluxo de controle é comum ao lote; se uma condição baseada em CMP
 * der resultados diferentes entre os vértices, desistimos (retorno 0) e o chamador refaz
 * esses vértices com sh_run. As contas são as mesmas do interpretador escalar, na mesma ordem.
 * ====================================================================================== */
#define L SB_L
#define LANES for (int l = 0; l < L; l++)
typedef float VB[4][L];

static inline __attribute__((always_inline)) void fetch_b(const ShaderRunB *s, const ShaderState *st, const DIns *D, int k, VB out) {
    uint32_t idx = D->src[k];
    const uint8_t *sw = D->sw[k];
    int neg = D->neg[k];
    if (idx < 0x20) {
        const float (*v)[L] = idx < 0x10 ? s->in[idx] : s->tmp[idx - 0x10];
        for (int c = 0; c < 4; c++) memcpy(out[c], v[sw[c]], sizeof(float) * L);
        if (neg) for (int c = 0; c < 4; c++) LANES out[c][l] = -out[c][l];
        return;
    }
    int u = (int)idx - 0x20;
    if (D->rel && D->relsrc == k) {
        if (D->rel == 3) { u += s->aL; if (u < 0 || u > 95) u = 0; }
        else {                                            /* a0 pode variar por vértice: busca individual */
            const int *a0 = s->a0[D->rel - 1];
            LANES {
                int ul = u + a0[l]; if (ul < 0 || ul > 95) ul = 0;
                const float *v = st->uniform_f[ul & 0x7F];
                for (int c = 0; c < 4; c++) out[c][l] = v[sw[c]];
            }
            if (neg) for (int c = 0; c < 4; c++) LANES out[c][l] = -out[c][l];
            return;
        }
    }
    const float *v = st->uniform_f[u & 0x7F];
    for (int c = 0; c < 4; c++) { float x = v[sw[c]]; x = neg ? -x : x; LANES out[c][l] = x; }
}
static inline __attribute__((always_inline)) void store_b(ShaderRunB *s, uint32_t dst, uint32_t mask, const VB v, unsigned m) {
    float (*d)[L] = dst < 0x10 ? s->out[dst] : s->tmp[(dst - 0x10) & 15];
    if (m == (1u << L) - 1) { for (int c = 0; c < 4; c++) if (mask & (8 >> c)) memcpy(d[c], v[c], sizeof(float) * L); }
    else for (int c = 0; c < 4; c++) if (mask & (8 >> c)) LANES if ((m >> l) & 1) d[c][l] = v[c][l];
}
static inline float pmul_b(float a, float b) {           /* igual a pmul, sem desvio (vetorizável) */
    float r = a * b;
    return (r != r && a == a && b == b) ? 0.0f : r;
}
/* condição de desvio por vértice, como máscara de bits */
static unsigned cond_b(const ShaderRunB *s, uint32_t op, uint32_t refx, uint32_t refy) {
    unsigned r = 0;
    LANES {
        int x = s->cmp[0][l] == (int)refx, y = s->cmp[1][l] == (int)refy, t;
        switch (op) { case 0: t = x || y; break; case 1: t = x && y; break; case 2: t = x; break; default: t = y; }
        r |= (unsigned)t << l;
    }
    return r;
}

/* pilha do lote: além do quadro normal, guarda a máscara de vértices ativos a restaurar e, para
 * um IFC divergente, o bloco "senão" que ainda falta executar com os outros vértices */
typedef struct { uint32_t final_addr, return_addr, loop_addr; int repeat, incr, is_loop; unsigned mask_after, else_mask; uint32_t else_end; } BFrame;

int sh_run_batch(const ShaderState *st, ShaderRunB *s) {
    BFrame stack[16]; int sp = 0;
    const unsigned FULL = (1u << L) - 1;
    unsigned m = FULL;
    uint32_t pc = st->entry;
    memset(s->tmp, 0, sizeof s->tmp); memset(s->a0, 0, sizeof s->a0); memset(s->cmp, 0, sizeof s->cmp); s->aL = 0;
    VB a, b, c3, r;
    for (int steps = 0; steps < 100000; steps++) {
        while (sp && pc == stack[sp - 1].final_addr) {
            BFrame *f = &stack[sp - 1];
            if (f->else_mask) {                           /* fim do "então" divergente: agora o "senão" */
                m = f->else_mask; f->else_mask = 0;
                pc = f->final_addr; f->final_addr = f->else_end;
                if (pc == f->final_addr) { m = f->mask_after; pc = f->return_addr; sp--; }
                continue;
            }
            s->aL += f->incr;
            if (f->repeat-- == 0) { pc = f->return_addr; m = f->mask_after; sp--; }
            else { pc = f->loop_addr; break; }
        }
        if (pc >= 4096) break;
        uint32_t ins = st->code[pc], op = ins >> 26;
        uint32_t next = pc + 1;
        #define CALLM(off, n, ret, rep, inc, loop, newm) do { if (sp < 16) { stack[sp++] = (BFrame){ (off) + (n), (ret), (off), (rep), (inc), (loop), m, 0, 0 }; m = (newm); } next = (off); } while (0)
        #define CALL(off, n, ret, rep, inc, loop) CALLM(off, n, ret, rep, inc, loop, m)
        if (op < 0x20 || op >= 0x2E) {
            if (__builtin_expect(!__atomic_load_n(&st->dok[pc], __ATOMIC_ACQUIRE), 0)) decode((ShaderState *)st, pc);
            const DIns *D = &st->dec[pc];
            if (op >= 0x30) {
                fetch_b(s, st, D, 0, a); fetch_b(s, st, D, 1, b); fetch_b(s, st, D, 2, c3);
                for (int k = 0; k < 4; k++) LANES r[k][l] = pmul_b(a[k][l], b[k][l]) + c3[k][l];
                store_b(s, D->dst, D->mask, r, m);
                pc = next; continue;
            }
            fetch_b(s, st, D, 0, a); fetch_b(s, st, D, 1, b);
            if (op == 0x2E || op == 0x2F) {
                LANES if ((m >> l) & 1) { s->cmp[0][l] = compare(a[0][l], b[0][l], D->dst); s->cmp[1][l] = compare(a[1][l], b[1][l], D->mask); }
                pc = next; continue;
            }
            switch (op) {
            case 0x00: for (int k = 0; k < 4; k++) LANES r[k][l] = a[k][l] + b[k][l]; break;
            case 0x01: LANES { float t = pmul_b(a[0][l], b[0][l]) + pmul_b(a[1][l], b[1][l]) + pmul_b(a[2][l], b[2][l]); r[0][l] = r[1][l] = r[2][l] = r[3][l] = t; } break;
            case 0x02: LANES { float t = pmul_b(a[0][l], b[0][l]) + pmul_b(a[1][l], b[1][l]) + pmul_b(a[2][l], b[2][l]) + pmul_b(a[3][l], b[3][l]); r[0][l] = r[1][l] = r[2][l] = r[3][l] = t; } break;
            case 0x03: case 0x18: LANES { float t = pmul_b(a[0][l], b[0][l]) + pmul_b(a[1][l], b[1][l]) + pmul_b(a[2][l], b[2][l]) + b[3][l]; r[0][l] = r[1][l] = r[2][l] = r[3][l] = t; } break;
            case 0x04: case 0x19: LANES { r[0][l] = 1; r[1][l] = pmul_b(a[1][l], b[1][l]); r[2][l] = a[2][l]; r[3][l] = b[3][l]; } break;
            case 0x05: LANES { float t = exp2f(a[0][l]); r[0][l] = r[1][l] = r[2][l] = r[3][l] = t; } break;
            case 0x06: LANES { float t = log2f(a[0][l]); r[0][l] = r[1][l] = r[2][l] = r[3][l] = t; } break;
            case 0x07: LANES {
                r[0][l] = fmaxf(a[0][l], 0); r[1][l] = fminf(fmaxf(a[1][l], -127.9961f), 127.9961f);
                r[2][l] = fmaxf(a[2][l], 0); r[3][l] = fmaxf(a[3][l], 0);
                if ((m >> l) & 1) { s->cmp[0][l] = a[0][l] >= 0; s->cmp[1][l] = a[3][l] >= 0; } } break;
            case 0x08: for (int k = 0; k < 4; k++) LANES r[k][l] = pmul_b(a[k][l], b[k][l]); break;
            case 0x09: case 0x1A: for (int k = 0; k < 4; k++) LANES r[k][l] = a[k][l] >= b[k][l] ? 1.0f : 0.0f; break;
            case 0x0A: case 0x1B: for (int k = 0; k < 4; k++) LANES r[k][l] = a[k][l] < b[k][l] ? 1.0f : 0.0f; break;
            case 0x0B: for (int k = 0; k < 4; k++) LANES r[k][l] = floorf(a[k][l]); break;
            case 0x0C: for (int k = 0; k < 4; k++) LANES r[k][l] = a[k][l] > b[k][l] ? a[k][l] : b[k][l]; break;
            case 0x0D: for (int k = 0; k < 4; k++) LANES r[k][l] = a[k][l] < b[k][l] ? a[k][l] : b[k][l]; break;
            case 0x0E: LANES { float t = 1.0f / a[0][l]; r[0][l] = r[1][l] = r[2][l] = r[3][l] = t; } break;
            case 0x0F: LANES { float t = 1.0f / sqrtf(a[0][l]); r[0][l] = r[1][l] = r[2][l] = r[3][l] = t; } break;
            case 0x12:
                if (D->mask & 8) LANES if ((m >> l) & 1) s->a0[0][l] = (int)a[0][l];
                if (D->mask & 4) LANES if ((m >> l) & 1) s->a0[1][l] = (int)a[1][l];
                pc = next; continue;
            case 0x13: for (int k = 0; k < 4; k++) LANES r[k][l] = a[k][l]; break;
            default: pc = next; continue;
            }
            store_b(s, D->dst, D->mask, r, m);
            pc = next; continue;
        }
        uint32_t dst_offs = (ins >> 10) & 0xFFF, num = ins & 0xFF;
        uint32_t condop = (ins >> 22) & 3, refx = (ins >> 25) & 1, refy = (ins >> 24) & 1;
        uint32_t bool_id = (ins >> 22) & 0xF, int_id = (ins >> 22) & 3;
        int bb = (st->uniform_b >> bool_id) & 1;
        unsigned t;
        switch (op) {
        case 0x20: case 0x23:
            t = op == 0x20 ? m : cond_b(s, condop, refx, refy) & m;
            if (t) {
                if (t != m || m != FULL) return 0;        /* BREAK com vértices em caminhos diferentes */
                while (sp && !stack[sp - 1].is_loop) sp--;
                if (sp) { next = stack[sp - 1].return_addr; sp--; }
            }
            break;
        case 0x21: break;
        case 0x22: if (m != FULL) return 0; return 1;
        case 0x24: CALL(dst_offs, num, pc + 1, 0, 0, 0); break;
        case 0x25:
            t = cond_b(s, condop, refx, refy) & m;
            if (t) CALLM(dst_offs, num, pc + 1, 0, 0, 0, t);
            break;
        case 0x26: if (bb) CALL(dst_offs, num, pc + 1, 0, 0, 0); break;
        case 0x27: case 0x28: {
            t = op == 0x27 ? (bb ? m : 0) : cond_b(s, condop, refx, refy) & m;
            if (t == m) CALL(pc + 1, dst_offs - pc - 1, dst_offs + num, 0, 0, 0);
            else if (!t) CALL(dst_offs, num, dst_offs + num, 0, 0, 0);
            else {                                        /* divergente: "então" com t, depois "senão" com o resto */
                if (sp >= 16) return 0;
                stack[sp++] = (BFrame){ dst_offs, dst_offs + num, pc + 1, 0, 0, 0, m, m & ~t, dst_offs + num };
                m = t; next = pc + 1;
            }
            break;
        }
        case 0x29: {
            const uint8_t *iv = st->uniform_i[int_id];
            s->aL = iv[1];
            CALL(pc + 1, dst_offs - pc, dst_offs + 1, iv[0], (int8_t)iv[2], 1);
            break;
        }
        case 0x2A: case 0x2B: return 0;                    /* geometria: não usa o lote */
        case 0x2C:
            t = cond_b(s, condop, refx, refy) & m;
            if (t && t != m) return 0;
            if (t) next = dst_offs;
            break;
        case 0x2D: if (bb != (int)(num & 1)) next = dst_offs; break;
        default: break;
        }
        pc = next;
        #undef CALL
        #undef CALLM
    }
    return 1;
}
#undef L
#undef LANES

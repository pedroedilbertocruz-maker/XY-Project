/* Verificação automática do código traduzido contra o interpretador (interp.c).
 *
 * Com o código gerado por RECOMP_VERIFY=1, cada função começa com VFY_ENTRY. Com VERIFICAR=n no ambiente,
 * as n primeiras chamadas de cada função são verificadas assim:
 *   1. o interpretador roda a função a partir do mesmo estado, numa memória sobreposta (as escritas dele
 *      ficam numa tabela à parte), até voltar ao endereço de retorno;
 *   2. a função traduzida roda de verdade (sem troca de thread no meio);
 *   3. compara registradores, flags, registradores do VFP e cada byte que o interpretador escreveu.
 * Divergências vão para verificacao.txt (no máximo 3 por função); um resumo sai no registro a cada 2000. */
#include "rt.h"
#include "interp.h"
#include <stdio.h>
#include <stdlib.h>

volatile int g_vfy;
__thread int t_vfy_busy;                 /* runtime.c não troca de thread enquanto isto estiver ligado */
static int g_limit = 2;
static long g_max_steps = 3000000;
static long st_ok, st_bad, st_svc, st_undef, st_steps, st_ovf, st_total;

__attribute__((constructor)) static void vfy_init(void) {
    const char *e = getenv("VERIFICAR");
    if (!e) return;
    g_limit = atoi(e); if (g_limit <= 0) g_limit = 2;
    if (getenv("VERIFICAR_PASSOS")) g_max_steps = atol(getenv("VERIFICAR_PASSOS"));
    g_vfy = 1;
    FILE *f = fopen("verificacao.txt", "w"); if (f) { fprintf(f, "# divergências entre o código traduzido e o interpretador\n"); fclose(f); }
}

/* contagem por função (endereço -> vezes verificada, e vezes relatada) */
#define NF (1u << 18)
static struct { uint32_t addr; uint16_t n, rep; } g_fn[NF];
static int fn_slot(uint32_t a) {
    uint32_t i = (a * 2654435761u) >> 14;
    for (int k = 0; k < 64; k++, i = (i + 1) & (NF - 1)) {
        if (g_fn[i].addr == a) return (int)i;
        if (!g_fn[i].addr) { g_fn[i].addr = a; return (int)i; }
    }
    return -1;
}

static void summary(void) {
    fprintf(stderr, "[verifica] %ld funções verificadas: %ld iguais, %ld DIVERGENTES | puladas: %ld com syscall, %ld instrução desconhecida, %ld longas, %ld memória demais\n",
            st_total, st_ok, st_bad, st_svc, st_undef, st_steps, st_ovf);
}

static void report(uint32_t addr, const Ctx *in, const Ctx *ic, const Ctx *rc, uint32_t ret, long steps, int nmem, uint32_t m0, uint8_t iv, uint8_t rv) {
    FILE *f = fopen("verificacao.txt", "a"); if (!f) return;
    fprintf(f, "\n== função %08x (retorno %08x, %ld passos do interpretador)\n  entrada:", addr, ret, steps);
    for (int i = 0; i < 15; i++) fprintf(f, " r%d=%08x", i, in->r[i]);
    fprintf(f, " nzcv=%u%u%u%u\n", in->n, in->z, in->c, in->v);
    for (int i = 0; i < 15; i++) if (ic->r[i] != rc->r[i]) fprintf(f, "  r%d: interpretador %08x, traduzido %08x\n", i, ic->r[i], rc->r[i]);
    if (ic->n != rc->n || ic->z != rc->z || ic->c != rc->c || ic->v != rc->v)
        fprintf(f, "  nzcv: interpretador %u%u%u%u, traduzido %u%u%u%u\n", ic->n, ic->z, ic->c, ic->v, rc->n, rc->z, rc->c, rc->v);
    if (ic->ge != rc->ge) fprintf(f, "  ge: interpretador %x, traduzido %x\n", ic->ge, rc->ge);
    for (int i = 0; i < 32; i++) { uint32_t a, b; memcpy(&a, &ic->s[i], 4); memcpy(&b, &rc->s[i], 4);
        if (a != b) fprintf(f, "  s%d: interpretador %08x (%g), traduzido %08x (%g)\n", i, a, ic->s[i], b, rc->s[i]); }
    if ((ic->fpscr ^ rc->fpscr) & 0xF0000000u) fprintf(f, "  fpscr: interpretador %08x, traduzido %08x\n", ic->fpscr, rc->fpscr);
    if (nmem) fprintf(f, "  memória: %d bytes diferentes; o primeiro em %08x: interpretador %02x (escrito em %08x), traduzido %02x\n", nmem, m0, iv, ip_overlay_pc(m0), rv);
    fclose(f);
}

int vfy_entry(Ctx *c, uint32_t addr, void (*fn)(Ctx *)) {
    if (t_vfy_busy) return 0;
    int k = fn_slot(addr);
    if (k < 0 || g_fn[k].n >= g_limit) return 0;
    g_fn[k].n++;
    t_vfy_busy = 1;
    Ctx in = *c, ic = *c;
    uint32_t ret = c->tret ? c->tret : c->r[14];
    ic.tret = 0;
    long steps = 0;
    ip_overlay_begin();
    IpExit ex = ip_run(&ic, addr, ret, g_max_steps, 0, &steps);
    ip_overlay_end();
    st_total++;
    if (ex != IP_RET || ip_overflow) {
        if (ip_overflow) st_ovf++; else if (ex == IP_SVC) st_svc++; else if (ex == IP_STEPS) st_steps++;
        else { st_undef++; static int nu; if (nu++ < 40) fprintf(stderr, "[verifica] instrução desconhecida para o interpretador: %08x em %08x (função %08x)\n", ip_last_info, ic.r[15], addr); }
        t_vfy_busy = 0;            /* (as funções chamadas por esta ainda podem ser verificadas) */
        fn(c);
        return 1;
    }
    fn(c);                         /* a de verdade (o VFY_ENTRY dela não verifica de novo: t_vfy_busy) */
    t_vfy_busy = 0;
    int bad = 0, nmem = 0; uint32_t m0 = 0; uint8_t iv0 = 0, rv0 = 0;
    for (int i = 0; i < 15; i++) if (ic.r[i] != c->r[i]) bad = 1;
    if (ic.n != c->n || ic.z != c->z || ic.c != c->c || ic.v != c->v || ic.ge != c->ge) bad = 1;
    if (memcmp(ic.s, c->s, sizeof ic.s)) bad = 1;
    uint32_t nov = ip_overlay_count();
    for (uint32_t i = 0; i < nov; i++) {
        uint32_t a = ip_overlay_addr(i); uint8_t iv = ip_overlay_val(a), rv = MEM8(a);
        if (iv != rv) { if (!nmem) { m0 = a; iv0 = iv; rv0 = rv; } nmem++; }
    }
    if (nmem) bad = 1;
    if (bad) {
        st_bad++;
        if (g_fn[k].rep < 3) { g_fn[k].rep++; report(addr, &in, &ic, c, ret, steps, nmem, m0, iv0, rv0); }
        static int nb; if (nb++ < 30) fprintf(stderr, "[verifica] DIVERGÊNCIA na função %08x (detalhes em verificacao.txt)\n", addr);
    } else st_ok++;
    if (st_total % 2000 == 0) summary();
    return 1;
}

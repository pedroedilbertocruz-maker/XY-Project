/* Testes do kernel HLE: as "threads do jogo" aqui são funções C que fazem syscalls como o jogo faria. */
#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

uint8_t *g_mem;
void UNIMPL(Ctx *c, uint32_t a, uint32_t w) { abort(); }
void svc(Ctx *c, uint32_t n) { if (!k_svc(c, n)) { fprintf(stderr, "svc %x?\n", n); exit(2); } }

/* ---- "endereços" das funções de teste ---- */
typedef void (*Fn)(Ctx *);
static Fn g_fns[32]; static int g_nfns;
static uint32_t reg_fn(Fn f) { g_fns[g_nfns] = f; return 0x1000u + 4u * g_nfns++; }
void call_indirect(Ctx *c, uint32_t a) { g_fns[(a - 0x1000u) / 4](c); }

static uint32_t sys(Ctx *c, uint32_t n, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t r4) {
    c->r[0] = r0; c->r[1] = r1; c->r[2] = r2; c->r[3] = r3; c->r[4] = r4; svc(c, n); return c->r[0];
}
#define INF 0xFFFFFFFFu
static uint32_t create_thread(Ctx *c, Fn f, uint32_t arg, int prio) {
    static uint32_t stack = 0x08100000u; stack += 0x10000;
    sys(c, 0x08, (uint32_t)prio, reg_fn(f), arg, stack, 0); return c->r[1];
}
static void sleep_ms(Ctx *c, int ms) { uint64_t ns = (uint64_t)ms * 1000000; sys(c, 0x0A, (uint32_t)ns, (uint32_t)(ns >> 32), 0, 0, 0); }
static void yield(Ctx *c) { sys(c, 0x0A, 0, 0, 0, 0, 0); }
static uint32_t wait1(Ctx *c, uint32_t h, int64_t ns) { return sys(c, 0x24, h, 0, (uint32_t)ns, (uint32_t)((uint64_t)ns >> 32), 0); }
static uint32_t new_event(Ctx *c, int reset) { sys(c, 0x17, 0, (uint32_t)reset, 0, 0, 0); return c->r[1]; }

/* ---- registro de eventos ---- */
static char g_log[2048];
static void L(const char *s) { strcat(g_log, s); strcat(g_log, " "); }
static int fails, total;
static void check(const char *name, int ok, const char *detail) {
    total++; if (!ok) fails++;
    printf("%s %-28s %s\n", ok ? "ok    " : "FALHOU", name, ok ? "" : detail);
}
static void check_log(const char *name, const char *exp) {
    char d[2300]; snprintf(d, sizeof d, "esperado [%s] obtido [%s]", exp, g_log);
    size_t n = strlen(g_log); if (n && g_log[n - 1] == ' ') g_log[n - 1] = 0;
    check(name, !strcmp(g_log, exp), d); g_log[0] = 0;
}

/* ---- threads de teste ---- */
static uint32_t g_ev, g_mutex, g_sem, g_arb, g_counter, g_inside, g_overlap;
static void t_high(Ctx *c) { L("alta"); }
static void t_low(Ctx *c) { L("baixa"); }
static void t_event_waiter(Ctx *c) { L("espera"); wait1(c, g_ev, -1); L("acordou"); }
static void t_mutex_worker(Ctx *c) {
    for (int i = 0; i < 100; i++) {
        wait1(c, g_mutex, -1);
        if (g_inside++) g_overlap = 1;
        uint32_t v = g_counter; yield(c); g_counter = v + 1;   /* sem o mutex, perderia incrementos */
        g_inside--;
        sys(c, 0x14, g_mutex, 0, 0, 0, 0);
        if (i % 7 == 0) yield(c);
    }
}
static int g_consumed;
static void t_consumer(Ctx *c) { for (int i = 0; i < 5; i++) { wait1(c, g_sem, -1); g_consumed++; } }
static void t_arb_waiter(Ctx *c) {
    L("arb-espera");
    sys(c, 0x22, g_arb, 0x08000000u, 1, 1, 0);   /* espera enquanto mem < 1 */
    L("arb-acordou");
}
static void t_short(Ctx *c) { sleep_ms(c, 5); L("filha-fim"); }

static void main_test(Ctx *c) {
    /* 1. thread de prioridade maior roda na hora em que é criada */
    L("main1"); create_thread(c, t_high, 0, 0x20); L("main2");
    check_log("preempção na criação", "main1 alta main2");

    /* 2. prioridade menor só roda quando a principal bloqueia */
    create_thread(c, t_low, 0, 0x38); L("main-segue"); sleep_ms(c, 1); L("main-volta");
    check_log("prioridade menor espera", "main-segue baixa main-volta");

    /* 3. evento: quem espera (prioridade maior) acorda assim que é sinalizado */
    g_ev = new_event(c, RESET_ONESHOT);
    create_thread(c, t_event_waiter, 0, 0x20); L("sinaliza");
    sys(c, 0x18, g_ev, 0, 0, 0, 0); L("depois");
    check_log("evento acorda thread", "espera sinaliza acordou depois");
    check("evento oneshot foi consumido", wait1(c, g_ev, 0) == RES_TIMEOUT, "ainda sinalizado");

    /* 4. mutex garante exclusão mútua mesmo cedendo o núcleo dentro da seção crítica */
    sys(c, 0x13, 0, 0, 0, 0, 0); g_mutex = c->r[1];
    uint32_t a = create_thread(c, t_mutex_worker, 0, 0x30), b = create_thread(c, t_mutex_worker, 0, 0x30);
    wait1(c, a, -1); wait1(c, b, -1);
    char d[64]; snprintf(d, sizeof d, "contador=%u sobreposição=%u", g_counter, g_overlap);
    check("mutex: 200 incrementos", g_counter == 200 && !g_overlap, d);

    /* 5. semáforo: produtor/consumidor */
    sys(c, 0x15, 0, 0, 10, 0, 0); g_sem = c->r[1];
    uint32_t cons = create_thread(c, t_consumer, 0, 0x20);
    for (int i = 0; i < 5; i++) { sys(c, 0x16, 0, g_sem, 1, 0, 0); sleep_ms(c, 1); }
    wait1(c, cons, 1000000000);
    check("semáforo: 5 consumidos", g_consumed == 5, "faltaram itens");

    /* 6. árbitro de endereços */
    sys(c, 0x21, 0, 0, 0, 0, 0); g_arb = c->r[1];
    MEM32(0x08000000u) = 0;
    create_thread(c, t_arb_waiter, 0, 0x20);
    L("main-sinaliza"); MEM32(0x08000000u) = 1;
    sys(c, 0x22, g_arb, 0x08000000u, 0, (uint32_t)-1, 0);
    L("main-fim");
    check_log("árbitro: espera e sinal", "arb-espera main-sinaliza arb-acordou main-fim");

    /* 7. timeout */
    uint32_t ev2 = new_event(c, RESET_ONESHOT);
    uint64_t t0 = k_now_ns(); uint32_t r = wait1(c, ev2, 20000000); uint64_t dt = (k_now_ns() - t0) / 1000000;
    snprintf(d, sizeof d, "resultado=%08x tempo=%llums", r, (unsigned long long)dt);
    check("timeout de 20 ms", r == RES_TIMEOUT && dt >= 20 && dt < 100, d);

    /* 8. timer periódico */
    sys(c, 0x1A, 0, RESET_ONESHOT, 0, 0, 0); uint32_t tm = c->r[1];
    t0 = k_now_ns();
    sys(c, 0x1B, tm, 5000000, 5000000, 0, 0);   /* inicial 5 ms (r2:r3), intervalo 5 ms (r1:r4) */
    for (int i = 0; i < 6; i++) wait1(c, tm, -1);
    dt = (k_now_ns() - t0) / 1000000;
    snprintf(d, sizeof d, "tempo=%llums", (unsigned long long)dt);
    check("timer: 6 disparos de 5 ms", dt >= 29 && dt < 80, d);
    sys(c, 0x1C, tm, 0, 0, 0, 0);

    /* 9. WaitSynchronizationN: qualquer um */
    uint32_t e1 = new_event(c, RESET_STICKY), e2 = new_event(c, RESET_STICKY);
    MEM32(0x08001000u) = e1; MEM32(0x08001004u) = e2;
    sys(c, 0x18, e2, 0, 0, 0, 0);
    r = sys(c, 0x25, INF, 0x08001000u, 2, 0, INF);
    snprintf(d, sizeof d, "resultado=%08x índice=%d", r, (int)c->r[1]);
    check("WaitN (qualquer): índice 1", r == 0 && c->r[1] == 1, d);

    /* 10. WaitSynchronizationN: todos, com um faltando -> timeout */
    r = sys(c, 0x25, 10000000, 0x08001000u, 2, 1, 0);
    snprintf(d, sizeof d, "resultado=%08x", r);
    check("WaitN (todos) dá timeout", r == RES_TIMEOUT, d);

    /* 11. esperar uma thread terminar */
    uint32_t th = create_thread(c, t_short, 0, 0x38);
    wait1(c, th, -1); L("join");
    check_log("esperar thread terminar", "filha-fim join");

    printf("%d/%d testes do kernel passaram\n", total - fails, total);
    exit(fails != 0);
}

int main(void) {
    g_mem = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    kernel_run(reg_fn(main_test), 0x08100000u, 0x1FF82000u, 0x30);
}

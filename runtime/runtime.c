#define _GNU_SOURCE
/* Runtime do port: carrega a imagem linkada e roda o jogo recompilado sobre o kernel HLE.
 *   kernel.c   threads, escalonador, eventos/mutexes/semáforos/timers/árbitro
 *   services.c serviços do sistema (srv:, APT, hid, ...)
 *   runtime.c  carregador, chamadas indiretas e syscalls de memória/portas/informações
 */
#include "kernel.h"
#include "interp.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include "plat.h"
#ifndef _WIN32
#include <sys/mman.h>
#include <execinfo.h>
#include <unistd.h>
#endif

uint8_t *g_mem;
struct FuncEntry { uint32_t addr; void (*fn)(Ctx *); };
extern const struct FuncEntry g_functable[];
extern const int g_functable_len;
void ipc(Ctx *c, uint32_t handle);
void watchw_rearm(void);
extern uint32_t g_hid_shmem_handle, g_hid_shmem_addr;

static long g_svc_count, g_trace_limit = 400;
static uint32_t g_heap_top = 0x08000000u, g_linear_top = 0x14000000u;

#define STACK_TOP   0x10000000u
#define TLS_ADDR    0x1FF82000u
#define CFG_MEM     0x1FF80000u   /* página de configuração do kernel */

/* NANCHECK: primeira vez que cada instrução VFP produz NaN (com os operandos) */
void nan_log(Ctx *c, uint32_t addr, double a, double b) {
    static uint32_t seen[256]; static int n;
    for (int i = 0; i < n; i++) if (seen[i] == addr) return;
    if (n < 256) seen[n++] = addr;
    fprintf(stderr, "[nan] %08x produziu NaN (operandos %g, %g) lr=%08x\n", addr, a, b, c->r[14]);
}
void UNIMPL(Ctx *c, uint32_t a, uint32_t w) {
    if (w == 0xF57FF01Fu) return;   /* CLREX (código gerado antes do suporte no recompilador) */
    fprintf(stderr, "[runtime] instrução não suportada em %08x: %08x\n", a, w);
    exit(3);
}

long g_probe_count;
/* sonda: imprime os registradores antes da instrução marcada; PROBE_MEM=rN mostra 8 palavras em [rN] */
void probe(Ctx *c, uint32_t addr) {
    /* PROBE_SKIP=n: conta mas não imprime as n primeiras; PROBE_MAX=m: imprime no máximo m (depois do salto);
     * a cada 2^20 passagens imprime o total, para achar a região de interesse numa execução longa */
    static long skip = -1, max; long count;
    if (skip < 0) { skip = getenv("PROBE_SKIP") ? atol(getenv("PROBE_SKIP")) : 0; max = getenv("PROBE_MAX") ? atol(getenv("PROBE_MAX")) : 50; }
    { extern uint32_t g_cap_n; static long desde = -2; if (desde == -2) desde = getenv("PROBE_DESDE_CAP") ? atol(getenv("PROBE_DESDE_CAP")) : -1;
      if (desde >= 0 && (long)g_cap_n < desde) return; }
    count = ++g_probe_count;
    if ((count & 0xFFFFF) == 0) fprintf(stderr, "[probe] %ld passagens\n", count);
    if (count <= skip || count > skip + max) return;
    static int dumped;
    if (!dumped && getenv("DUMP_RANGES") && count > (getenv("DUMP_AT") ? atol(getenv("DUMP_AT")) : skip)) {   /* retrato da memória (DUMP_RANGES="ini-fim,...", hex) na sonda DUMP_AT (padrão: a primeira impressa) */
        dumped = 1;
        char tmp[512]; snprintf(tmp, sizeof tmp, "%s", getenv("DUMP_RANGES"));
        for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ",")) {
            uint32_t a = (uint32_t)strtoul(t, NULL, 16), b = (uint32_t)strtoul(strchr(t, '-') + 1, NULL, 16);
            char fn[64]; snprintf(fn, sizeof fn, "mem_%08x.bin", a);
            plat_touch(g_mem + a, b - a);
            FILE *o = fopen(fn, "wb"); if (o) { fwrite(g_mem + a, 1, b - a, o); fclose(o); }
        }
        fprintf(stderr, "[probe] memória gravada\n");
    }
    fprintf(stderr, "[probe %08x]", addr);
    for (int k = 0; k < 15; k++) fprintf(stderr, " r%d=%08x", k, c->r[k]);
    fprintf(stderr, " nzcv=%u%u%u%u tls=%08x\n", c->n, c->z, c->c, c->v, c->tls);
    if (getenv("PROBE_VFP")) { fprintf(stderr, "    s:"); for (int k = 0; k < 32; k++) fprintf(stderr, " %g", c->s[k]); fprintf(stderr, "\n"); }
    const char *pm = getenv("PROBE_MEM");
    if (pm && pm[0] == 'r') {
        uint32_t base = c->r[atoi(pm + 1) & 15];
        fprintf(stderr, "    mem[%08x]:", base);
        for (int k = 0; k < 8; k++) fprintf(stderr, " %08x", MEM32(base + 4 * k));
        fprintf(stderr, "\n");
    }
}

void k_tick(void);
Ctx g_cap_ctx; uint32_t g_cap_n;               /* depuração (CAPTURA=endereço): último contexto ao entrar nessa função */
void call_indirect(Ctx *c, uint32_t a) {
    { static long cap = -2; if (cap == -2) cap = getenv("CAPTURA") ? (long)strtoul(getenv("CAPTURA"), 0, 16) : -1;
      if (cap >= 0 && a == (uint32_t)cap) { g_cap_ctx = *c; g_cap_n++;
          static long cn = -2; if (cn == -2) cn = getenv("CAPTURA_N") ? atol(getenv("CAPTURA_N")) : -1;
          if (cn >= 0 && ((long)g_cap_n == cn || (long)g_cap_n == cn + 1)) {   /* retrato: registradores + memória */
              static const uint32_t rg[][2] = { { 0x00100000, 0x00800000 }, { 0x08000000, 0x04400000 }, { 0x0e000000, 0x00020000 }, { 0x1ff80000, 0x00010000 } };
              char fn[128];
              for (int k = 0; k < 4; k++) { snprintf(fn, sizeof fn, "cap%u_m_%08x.bin", g_cap_n, rg[k][0]); FILE *f = fopen(fn, "wb"); if (f) { fwrite(g_mem + rg[k][0], 1, rg[k][1], f); fclose(f); } }
              snprintf(fn, sizeof fn, "cap%u_regs.txt", g_cap_n); FILE *f = fopen(fn, "w");
              if (f) { for (int k = 0; k < 16; k++) fprintf(f, "%08x ", c->r[k]); fprintf(f, "%u %u %u %u %08x\n", c->n, c->z, c->c, c->v, c->tls); fclose(f); }
              fprintf(stderr, "[captura] %u gravada\n", g_cap_n);
          } } }
    /* ponto de preempção: uma thread que gira sem syscalls (laços de espera ativa com ldrex/strex) não
     * pode impedir que uma thread mais prioritária acorde quando o prazo dela vence (como no kernel real) */
    extern __thread int t_vfy_busy;
    static unsigned tick; if ((++tick & 1023) == 0 && !t_vfy_busy) k_tick();
    static long teste = -2; if (teste == -2) teste = getenv("RESERVA_TESTE") ? (long)strtoul(getenv("RESERVA_TESTE"), 0, 16) : -1;   /* teste: finge não conhecer este endereço */
    int lo = 0, hi = teste >= 0 && a == (uint32_t)teste ? -1 : g_functable_len - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t m = g_functable[mid].addr;
        if (m == a) { g_functable[mid].fn(c); return; }
        if (m < a) lo = mid + 1; else hi = mid - 1;
    }
    uint32_t ret = c->tret ? c->tret : c->r[14];
    c->tret = 0;
    { static uint32_t seen[256]; static int ns; int k;           /* registra para a próxima recompilação (uma vez por endereço) */
      for (k = 0; k < ns && seen[k] != a; k++) continue;
      if (k == ns) {
          if (ns < 256) seen[ns++] = a;
          fprintf(stderr, "[runtime] call_indirect para %08x: não é início de função conhecido (lr=%08x); rodando no interpretador\n", a, c->r[14]);
          FILE *x = fopen("extra_funcs.txt", "a"); if (x) { fprintf(x, "%08x lr=%08x\n", a, c->r[14]); fclose(x); }
      } }
    /* reserva: o interpretador roda o código até ele voltar (com as syscalls de verdade); só para se achar uma
     * instrução inválida (destino que nem é código) */
    if (!getenv("SEM_RESERVA")) {
        extern void k_tick(void);
        if (!ip_tick_hook) ip_tick_hook = k_tick;
        Ctx save = *c; long n = 0;
        IpExit e = ip_run(c, a, ret, 0x7FFFFFFFFFFFFFFFL, 1, &n);
        if (e == IP_RET) return;
        fprintf(stderr, "[runtime] o interpretador parou em %08x depois de %ld passos (instrução %08x)\n", c->r[15], n, ip_last_info);
        if (n < 4) *c = save;    /* (nem começou: mostra o estado de antes) */
    }
    fprintf(stderr, "[runtime] call_indirect para %08x: não é início de função conhecido (lr=%08x, captura %u)\n", a, c->r[14], g_cap_n);
    fprintf(stderr, "[runtime] regs:"); for (int k = 0; k < 16; k++) fprintf(stderr, " r%d=%08x", k, c->r[k]);
    fprintf(stderr, "\n[runtime] pilha:"); for (int k = 0; k < 48; k++) fprintf(stderr, " %08x", MEM32(c->r[13] + 4 * k));
    fprintf(stderr, "\n[runtime] mem[r4]:"); for (int k = 0; k < 8; k++) fprintf(stderr, " %08x", MEM32(c->r[4] + 4 * k));
    if (getenv("DUMPTAB")) {
        uint32_t t = (uint32_t)strtoul(getenv("DUMPTAB"), 0, 16);
        fprintf(stderr, "\n[runtime] tabela %08x:", t);
        for (int k = 0; k < 16; k++) { uint32_t p = MEM32(t + 4 * k); fprintf(stderr, " %08x->%08x", p, p ? MEM32(p) : 0); }
    }
    uint32_t vt = MEM32(c->r[4]);
    fprintf(stderr, "\n[runtime] vtable %08x:", vt); for (int k = 0; k < 12; k++) fprintf(stderr, " %08x", MEM32(vt + 4 * k));
    uint32_t vt0 = MEM32(c->r[0]);
    fprintf(stderr, "\n[runtime] vtable de r0 %08x:", vt0); for (int k = 0; k < 16; k++) fprintf(stderr, " %08x", MEM32(vt0 + 4 * k));
    fprintf(stderr, "\n");
    exit(5);
}

void svc(Ctx *c, uint32_t n) {
    g_svc_count++;
    if (g_svc_count <= g_trace_limit && n != 0x32 && n != 0x28)
        fprintf(stderr, "[svc] %02x  r0=%08x r1=%08x r2=%08x r3=%08x  lr=%08x\n", n, c->r[0], c->r[1], c->r[2], c->r[3], c->r[14]);
    static uint32_t watch = 0, last = 0xDEADBEEF;
    if (!watch && getenv("WATCH")) watch = (uint32_t)strtoul(getenv("WATCH"), 0, 16);
    if (watch && MEM32(watch) != last) {
        fprintf(stderr, "[watch] mem[%08x] = %d (svc %02x, lr=%08x, tls=%08x)\n", watch, (int)MEM32(watch), n, c->r[14], c->tls);
        last = MEM32(watch);
    }
    watchw_rearm();
    if (k_svc(c, n)) return;
    switch (n) {
    case 0x01: {                                  /* ControlMemory(op, addr0, addr1, size, perm) */
        uint32_t op = c->r[0] & 0xFF, size = (c->r[3] + 0xFFF) & ~0xFFFu, addr = c->r[1];
        if (op == 3) {                            /* COMMIT */
            if (c->r[0] & 0x10000) { if (!addr) { addr = g_linear_top; } g_linear_top = addr + size; }
            else if (!addr) { addr = g_heap_top; }
            if (!(c->r[0] & 0x10000) && addr + size > g_heap_top) g_heap_top = addr + size;
        }
        c->r[0] = 0; c->r[1] = addr; return;
    }
    case 0x02: c->r[0] = 0; c->r[1] = c->r[2]; c->r[2] = 0x1000; c->r[3] = 3; return; /* QueryMemory (aprox.) */
    case 0x03: fprintf(stderr, "[svc] ExitProcess\n"); exit(0);
    case 0x1E: c->r[0] = 0; c->r[1] = k_new(OBJ_SHMEM, "memblock"); return;          /* CreateMemoryBlock */
    case 0x1F: {                                  /* MapMemoryBlock(h, addr, perm, ...) — a memória já é plana */
        KObj *o = k_get(c->r[0]);
        if (o) { o->user = c->r[1]; fprintf(stderr, "[mem] %s mapeado em %08x\n", o->name, c->r[1]); }
        if (c->r[0] == g_hid_shmem_handle) g_hid_shmem_addr = c->r[1];
        c->r[0] = 0; return;
    }
    case 0x20: c->r[0] = 0; return;
    case 0x28: { uint64_t t = k_now_ns() / 1000 * 268111856ull / 1000000ull;   /* ticks de 268 MHz */
                 c->r[0] = (uint32_t)t; c->r[1] = (uint32_t)(t >> 32); return; }
    case 0x2A: c->r[0] = 0; c->r[1] = 0x04000000u; c->r[2] = 0; return;            /* GetSystemInfo */
    case 0x2B: c->r[0] = 0; c->r[1] = 0x04000000u; c->r[2] = 0; return;            /* GetProcessInfo */
    case 0x2C: c->r[0] = 0; c->r[1] = 0; c->r[2] = 0; return;                      /* GetThreadInfo */
    case 0x2D: { char name[12] = {0}; memcpy(name, g_mem + c->r[1], 11);           /* ConnectToPort */
                 fprintf(stderr, "[svc] ConnectToPort(\"%s\")\n", name);
                 c->r[0] = 0; c->r[1] = k_new(OBJ_SESSION, name); return; }
    case 0x32: {                                                                   /* SendSyncRequest */
        uint32_t h = c->r[0], cmdid = MEM32(c->tls + 0x80) >> 16;
        ipc(c, h); c->r[0] = 0;
        if (watch && MEM32(watch) != last) {
            fprintf(stderr, "[watch] mem[%08x] = %d DEPOIS do IPC %s cmd %04x\n", watch, (int)MEM32(watch), k_name(h), cmdid);
            last = MEM32(watch);
        }
        return;
    }
    case 0x33: case 0x34: c->r[0] = 0; c->r[1] = k_new(OBJ_OTHER, "processo"); return;
    case 0x35: c->r[0] = 0; c->r[1] = 0x20; return;                                /* GetProcessId */
    case 0x36: c->r[0] = 0; c->r[1] = 0x20; return;
    case 0x38: c->r[0] = 0; c->r[1] = k_new(OBJ_OTHER, "reslimit"); return;
    case 0x39: case 0x3A: c->r[0] = 0; return;
    case 0x3C: fprintf(stderr, "[svc] Break(%x) — o jogo abortou (lr=%08x)\n", c->r[0], c->r[14]); exit(6);
    case 0x3D: fprintf(stderr, "[debug] %.*s\n", (int)c->r[1], (char *)(g_mem + c->r[0])); return;
    default:
        fprintf(stderr, "[svc] %02x não implementada (lr=%08x)\n", n, c->r[14]);
        exit(7);
    }
}

/* WATCHW=endereço: vigia de escrita exato. A página fica protegida; em cada falta, libera a escrita,
 * executa UMA instrução do PC (trap flag do x86) e protege de novo. Se a palavra vigiada mudou,
 * mostra valor antigo/novo e a pilha de funções recompiladas. WATCHW_N = máximo de eventos (padrão 8). */
#ifndef _WIN32
#include <ucontext.h>
static uint32_t g_watchw, g_watchw_old; static int g_watchw_left; static uint8_t *g_watchw_snap; static int g_watchw_len;
static void watchw_segv(int sig, siginfo_t *si, void *uc_) {
    uint8_t *a = (uint8_t *)si->si_addr, *page = g_mem + (g_watchw & ~0xFFFu);
    if (a < page || a >= page + 0x1000) { signal(SIGSEGV, SIG_DFL); return; }
    mprotect(page, 0x1000, PROT_READ | PROT_WRITE);
    g_watchw_old = MEM32(g_watchw & ~3u);
    static int len = -1; if (len < 0) len = getenv("WATCHW_LEN") ? atoi(getenv("WATCHW_LEN")) : 4;
    static uint8_t snap[256]; memcpy(snap, g_mem + g_watchw, len > 256 ? 256 : len); g_watchw_snap = snap; g_watchw_len = len > 256 ? 256 : len;
    ((ucontext_t *)uc_)->uc_mcontext.gregs[REG_EFL] |= 0x100;    /* passo único */
}
static void watchw_trap(int sig, siginfo_t *si, void *uc_) {
    ((ucontext_t *)uc_)->uc_mcontext.gregs[REG_EFL] &= ~0x100;
    uint32_t now = MEM32(g_watchw & ~3u);
    if (g_watchw_len > 4 && memcmp(g_watchw_snap, g_mem + g_watchw, g_watchw_len)) g_watchw_old = ~now;   /* algo mudou na faixa */
    static long want = -2; if (want == -2) want = getenv("WATCHW_VAL") ? (long)strtoul(getenv("WATCHW_VAL"), 0, 16) : -1;
    if (now != g_watchw_old && g_watchw_left > 0 && (want < 0 || now == (uint32_t)want)) {
        g_watchw_left--;
        fprintf(stderr, "[watchw] mem[%08x]: %08x -> %08x; pilha:\n", g_watchw & ~3u, g_watchw_old, now);
        void *bt[16]; int n = backtrace(bt, 16); backtrace_symbols_fd(bt + 2, n - 2, 2);
    }
    if (g_watchw_left > 0) mprotect(g_mem + (g_watchw & ~0xFFFu), 0x1000, PROT_READ);
}
#endif
void watchw_rearm(void) { }

static void load_image(const char *pack) {
    FILE *f = fopen(pack, "rb");
    if (!f) { perror(pack); exit(1); }
    uint32_t hdr[2];
    while (fread(hdr, 4, 2, f) == 2)
        if ((plat_touch(g_mem + hdr[0], hdr[1]), fread(g_mem + hdr[0], 1, hdr[1], f)) != hdr[1]) { fprintf(stderr, "image.pack truncado\n"); exit(1); }
    fclose(f);
}

/* data e hora do PC para o jogo: o 3DS publica o relógio na página compartilhada (0x1FF81000): DATETIME_SELECTOR
 * (+0x00) diz qual das duas cópias vale, DATETIME_0 (+0x20) e DATETIME_1 (+0x40) = { milissegundos desde
 * 1/1/1900 (hora local), tique do sistema quando foi escrito, tiques por segundo, 0 }. O jogo soma o tempo
 * passado desde aquele tique. Atualizado ao abrir e a cada minuto (pega mudança de fuso/horário de verão). */
#include <time.h>
#define SHARED_PAGE 0x1FF81000u
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2; int64_t era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
void shared_time_update(void) {
    time_t now = time(NULL); struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    int64_t local = days_from_civil(lt.tm_year + 1900, (unsigned)lt.tm_mon + 1, (unsigned)lt.tm_mday) * 86400
                  + lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;                 /* segundos locais desde 1970 */
    uint64_t ns = k_now_ns(), tick = ns / 1000 * 268111856ull / 1000000ull;
    uint64_t ms = (uint64_t)(local + 2208988800ll) * 1000ull;                        /* desde 1900 */
    { struct timespec ts; if (timespec_get(&ts, TIME_UTC)) ms += (uint64_t)(ts.tv_nsec / 1000000); }
    uint32_t sel = MEM32(SHARED_PAGE) + 1, slot = SHARED_PAGE + 0x20 + (sel & 1) * 0x20;
    MEM32(slot + 0x00) = (uint32_t)ms; MEM32(slot + 0x04) = (uint32_t)(ms >> 32);
    MEM32(slot + 0x08) = (uint32_t)tick; MEM32(slot + 0x0C) = (uint32_t)(tick >> 32);
    MEM32(slot + 0x10) = 268111856u; MEM32(slot + 0x14) = 0;
    MEM32(slot + 0x18) = 0; MEM32(slot + 0x1C) = 0;
    MEM32(SHARED_PAGE) = sel;
    MEM8(SHARED_PAGE + 0x04) = 1;                                                    /* RUNNING_HW: aparelho de verdade */
    static int once; if (!once) { once = 1; fprintf(stderr, "[relógio] %04d-%02d-%02d %02d:%02d:%02d (hora do PC)\n",
        lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec); }
}

volatile int g_opt_xp = 1;                /* XP ganho em batalha x1/x2/x4/x6/x8 (XP_MULT; o menu troca na hora) */
volatile int g_opt_radar;                  /* Poké Radar sempre carregado (RADAR_CHEIO=1; o menu troca na hora) */
int main(int argc, char **argv) {
#ifdef _WIN32
    setvbuf(stderr, NULL, _IONBF, 0);
#else
    setvbuf(stderr, NULL, _IOLBF, 0);
#endif
    g_mem = plat_reserve_guest_memory();
    { extern void crash_instalar(void); crash_instalar(); }   /* relatório de erro (crash.c) */
    load_image(argc > 1 ? argv[1] : "image.pack");
    if (getenv("KTRACE")) g_kernel_trace = 1;
#ifndef _WIN32
    if (getenv("WATCHW")) {
        g_watchw = (uint32_t)strtoul(getenv("WATCHW"), 0, 16);
        g_watchw_left = getenv("WATCHW_N") ? atoi(getenv("WATCHW_N")) : 8;
        struct sigaction sa = {0}; sa.sa_sigaction = watchw_segv; sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        struct sigaction st = {0}; st.sa_sigaction = watchw_trap; st.sa_flags = SA_SIGINFO;
        sigaction(SIGTRAP, &st, NULL);
        mprotect(g_mem + (g_watchw & ~0xFFFu), 0x1000, PROT_READ);
    }
#endif
    MEM32(CFG_MEM + 0x00) = 0x022E0000u;   /* KERNEL_VERSION */
    MEM32(CFG_MEM + 0x40) = 0x04000000u;   /* APPMEMALLOC: 64 MB */
    shared_time_update();                  /* data e hora do PC */
    { const char *e = getenv("RADAR_CHEIO"); g_opt_radar = e && *e == '1'; }
    { const char *e = getenv("XP_MULT"); int m = e ? atoi(e) : 1; g_opt_xp = (m == 2 || m == 4 || m == 6 || m == 8) ? m : 1; }
    { extern volatile int g_opt_nuzlocke; const char *e = getenv("NUZLOCKE"); g_opt_nuzlocke = e && *e == '1'; }
    fprintf(stderr, "[runtime] %d funções recompiladas; iniciando em 0x00100000\n", g_functable_len);
    { extern void save_reparar(void); save_reparar(); }                 /* save trazido de fora: formato e verificações */
    { extern void idioma_jogo_aplicar(void); idioma_jogo_aplicar(); }   /* idioma escolhido no menu */
    display_init();
    kernel_run(0x00100000u, STACK_TOP, TLS_ADDR, 0x30);
}

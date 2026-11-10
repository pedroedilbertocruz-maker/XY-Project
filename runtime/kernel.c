/* Kernel do 3DS em HLE.
 *
 * Modelo: núcleo único, como o núcleo de aplicativo do 3DS. Cada thread do jogo é uma pthread do PC,
 * mas só a thread `current` executa código do jogo; ela segura a trava `big`. Quando bloqueia
 * (espera, sleep, árbitro) ou quando uma thread de prioridade maior fica pronta, o bastão passa
 * para a thread pronta de maior prioridade (número menor = mais prioritária).
 * Os objetos de sincronização seguem a semântica do kernel do 3DS (3dbrew).
 */
#define _GNU_SOURCE
#include "kernel.h"
#include <pthread.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include "plat.h"

int g_kernel_trace = 0;

enum { T_READY, T_WAITING, T_DEAD };
enum { W_NONE, W_HANDLES, W_ARBITER, W_SLEEP };
#define NO_DEADLINE UINT64_MAX
#define MAX_THREADS 4096   /* threads mortas ficam na tabela (os handles continuam válidos) */
#define MAX_HANDLES 4096
#define HANDLE_BASE 0x10

typedef struct KThread {
    int id, prio, state;
    int core;          /* 0 = núcleo da aplicação; 1 = núcleo do sistema (CreateThread com processador 1) */
    Ctx ctx;
    uint32_t entry, handle;
    pthread_t pt;
    int wait_kind, wait_all, wait_n;
    uint32_t wait_handles[64];
    uint64_t deadline;
    uint32_t arb_addr; KObj *arb;
    uint32_t result; int index;
    uint64_t ready_ns;      /* quando ficou pronta (estatística: quanto espera a vez) */
} KThread;

static pthread_mutex_t big = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t kcv = PTHREAD_COND_INITIALIZER;
static KThread *threads[MAX_THREADS];
static int nthreads;
static KThread *current;
static __thread KThread *self;
static KObj *handles[MAX_HANDLES];
static int nhandles;

/* Relógio do jogo. Como no Azahar (tempo emulado separado do tempo real), o trabalho que no 3DS a GPU e o
 * hardware de vídeo fazem em paralelo — executar as listas da PICA, transferências e preenchimentos — não
 * conta como tempo passado para o jogo: o relógio para enquanto o PC emula isso. Sem isso, o jogo achava que
 * cada quadro demorou demais. Experimental e desligado por padrão (TEMPO_PAUSADO=1 liga): com a GPU
 * assíncrona (gsp.c) o trabalho da GPU já roda em paralelo, como no 3DS. */
static volatile uint64_t g_paused_ns;
static uint64_t host_now_ns(void) { return plat_now_ns(); }   /* relógio preciso (plat.c) */
/* Acelerar (g_k_vel = 2, 3 ou 4 enquanto o jogador segura a tecla): o relógio do jogo anda mais rápido que o do
 * PC, então VBlank, temporizadores, esperas e o som do DSP vêm na mesma proporção, como num emulador. A troca de
 * velocidade "reancora" o relógio (o tempo do jogo nunca volta nem salta). */
volatile int g_k_vel = 1;
static uint64_t g_vel_host, g_vel_jogo; static int g_vel_atual = 1;   /* âncora: relógio do PC e do jogo na última troca */
static volatile int g_vel_trava;
static void vel_trava(void) { while (__atomic_exchange_n(&g_vel_trava, 1, __ATOMIC_ACQUIRE)) { } }
static void vel_solta(void) { __atomic_store_n(&g_vel_trava, 0, __ATOMIC_RELEASE); }
uint64_t k_now_ns(void) {
    uint64_t h = host_now_ns() - g_paused_ns;
    int v = g_k_vel; if (v < 1) v = 1; if (v > 8) v = 8;
    if (v == 1 && g_vel_atual == 1 && !g_vel_host) return h;          /* (nunca acelerou: o relógio de sempre) */
    vel_trava();
    if (v != g_vel_atual) { g_vel_jogo += (h - g_vel_host) * (uint64_t)g_vel_atual; g_vel_host = h; g_vel_atual = v; }
    uint64_t t = g_vel_jogo + (h - g_vel_host) * (uint64_t)g_vel_atual;
    vel_solta();
    return t;
}
uint64_t k_time_pause(void) { return host_now_ns(); }
void k_time_resume(uint64_t t0) {
    static int real = -1; if (real < 0) real = getenv("TEMPO_PAUSADO") == NULL;   /* opcional (experimento) */
    if (!real) g_paused_ns += host_now_ns() - t0;
}
static uint64_t host_deadline(uint64_t d) {                  /* prazo no relógio do jogo -> relógio do PC */
    if (!g_vel_host) return d + g_paused_ns;
    vel_trava();
    uint64_t r = d <= g_vel_jogo ? g_vel_host : g_vel_host + (d - g_vel_jogo) / (uint64_t)g_vel_atual;
    vel_solta();
    return r + g_paused_ns;
}

/* ---------------------------------------------------------------- handles */
/* Handles: CloseHandle libera o número para ser reaproveitado (antes nunca liberava e, depois de
 * ~25 minutos de jogo, os 4096 números acabavam). Como no kernel do 3DS, cada handle tem contagem de
 * referências (DuplicateHandle soma). O número fechado vai para uma fila e só volta a ser usado depois
 * de muitos outros, para que um serviço que ainda guarde um número velho não pegue o objeto novo logo. */
static uint16_t g_href[MAX_HANDLES];
static int g_freeq[MAX_HANDLES], g_fq_head, g_fq_n;
#define REUSE_AFTER 1024
static uint32_t g_handles_live;
uint32_t k_new(ObjKind kind, const char *name) {
    int slot;
    if (g_fq_n > REUSE_AFTER || (nhandles >= MAX_HANDLES && g_fq_n)) {
        slot = g_freeq[g_fq_head]; g_fq_head = (g_fq_head + 1) % MAX_HANDLES; g_fq_n--;
    } else if (nhandles < MAX_HANDLES) slot = nhandles++;
    else { fprintf(stderr, "[kernel] handles esgotados (%u abertos)\n", g_handles_live); exit(9); }
    KObj *o = calloc(1, sizeof *o);
    o->kind = kind;
    snprintf(o->name, sizeof o->name, "%s", name);
    handles[slot] = o;                     /* o objeto antigo do número (se havia) fica para trás */
    if (slot + 1 == nhandles && nhandles % 512 == 0) fprintf(stderr, "[kernel] %d números de handle usados (%u abertos, %d na fila de reuso)\n", nhandles, g_handles_live + 1, g_fq_n);
    g_href[slot] = 1; g_handles_live++;
    return HANDLE_BASE + (uint32_t)slot;
}
static void k_close(uint32_t h) {
    if (h < HANDLE_BASE || h >= HANDLE_BASE + (uint32_t)nhandles) return;
    int slot = (int)(h - HANDLE_BASE);
    KObj *o = handles[slot];
    if (!o || !g_href[slot]) return;
    if (o->kind == OBJ_THREAD && o->thread && o->thread->state != T_DEAD) return;   /* thread viva: mantém */
    if (--g_href[slot]) return;
    g_handles_live--;
    g_freeq[(g_fq_head + g_fq_n) % MAX_HANDLES] = slot; g_fq_n++;
}

KObj *k_get(uint32_t h) {
    if (h == 0xFFFF8000u && self) h = self->handle;
    if (h < HANDLE_BASE || h >= HANDLE_BASE + (uint32_t)nhandles) return NULL;
    return handles[h - HANDLE_BASE];
}

const char *k_name(uint32_t h) { KObj *o = k_get(h); return o ? o->name : "?"; }

/* ---------------------------------------------------------------- espera */
static int obj_ready(KObj *o, KThread *t) {
    switch (o->kind) {
    case OBJ_EVENT: case OBJ_TIMER: return o->signaled;
    case OBJ_MUTEX: return o->lock_count == 0 || o->owner == t;
    case OBJ_SEMAPHORE: return o->count > 0;
    case OBJ_THREAD: return o->thread && o->thread->state == T_DEAD;
    default: return 0;   /* sessões, memória compartilhada etc. nunca ficam "sinalizadas" */
    }
}

static void obj_acquire(KObj *o, KThread *t) {
    switch (o->kind) {
    case OBJ_EVENT: case OBJ_TIMER: if (o->reset == RESET_ONESHOT) o->signaled = 0; break;
    case OBJ_MUTEX: o->owner = t; o->lock_count++; break;
    case OBJ_SEMAPHORE: o->count--; break;
    default: break;
    }
}

/* tenta satisfazer a espera da thread; se conseguir, ela volta a ficar pronta */
static int try_satisfy(KThread *t, uint64_t now) {
    if (t->wait_kind == W_HANDLES) {
        if (t->wait_all) {
            int all = 1;
            for (int i = 0; i < t->wait_n; i++) if (!obj_ready(k_get(t->wait_handles[i]), t)) { all = 0; break; }
            if (all) {
                for (int i = 0; i < t->wait_n; i++) obj_acquire(k_get(t->wait_handles[i]), t);
                t->result = 0; t->index = 0; t->state = T_READY; t->ready_ns = now; return 1;
            }
        } else {
            for (int i = 0; i < t->wait_n; i++) {
                KObj *o = k_get(t->wait_handles[i]);
                if (obj_ready(o, t)) { obj_acquire(o, t); t->result = 0; t->index = i; t->state = T_READY; t->ready_ns = now; return 1; }
            }
        }
    }
    if (t->deadline != NO_DEADLINE && now >= t->deadline) {
        t->result = t->wait_kind == W_SLEEP ? 0 : RES_TIMEOUT;
        t->index = -1; t->state = T_READY; t->ready_ns = now; return 1;
    }
    return 0;
}

extern volatile uint64_t g_kdrops, g_kper_sum, g_kper_n, g_kper_max;
int g_periodic_defer; uint64_t g_periodic_late_ns;   /* ver process_timers */
typedef struct { void (*fn)(void); uint64_t period, next; int catchup; } Periodic;
static Periodic g_periodic[8]; static int g_nperiodic;
/* (a mesma função registrada de novo — ex.: o DSP reiniciado a cada volta da abertura — substitui a anterior;
 * antes cada registro ocupava uma vaga nova e a nona escrevia fora da tabela) */
static void periodic_set(void (*fn)(void), uint64_t period_ns, int catchup) {
    int i = 0;
    while (i < g_nperiodic && g_periodic[i].fn != fn) i++;
    if (i == g_nperiodic) {
        if (g_nperiodic >= (int)(sizeof g_periodic / sizeof g_periodic[0])) { fprintf(stderr, "[kernel] disparos periódicos demais\n"); return; }
        g_nperiodic++;
    }
    g_periodic[i] = (Periodic){ fn, period_ns, k_now_ns() + period_ns, catchup };
}
void k_add_periodic(void (*fn)(void), uint64_t period_ns) { periodic_set(fn, period_ns, 1); }
/* como k_add_periodic, mas se o kernel atrasou, recupera até `catchup` disparos perdidos (o som precisa de todos) */
void k_add_periodic_catchup(void (*fn)(void), uint64_t period_ns, int catchup) { periodic_set(fn, period_ns, catchup); }
void k_set_signaled(uint32_t h) { KObj *o = k_get(h); if (o) o->signaled = 1; }

static void process_timers(uint64_t now) {
    /* um disparo por passagem: quando o kernel atrasou, os disparos atrasados saem um de cada vez, com as
     * threads do jogo rodando entre eles (o jogo precisa reabastecer o som a cada quadro do DSP; em rajada,
     * as vozes ficavam sem dados e o som picotava). Atraso maior que `catchup` períodos é descartado. */
    for (int i = 0; i < g_nperiodic; i++) {
        Periodic *pe = &g_periodic[i];
        if (now < pe->next) continue;
        g_periodic_late_ns = now - pe->next; g_periodic_defer = 0;
        if (pe->period > 10000000ull) { g_kper_sum += g_periodic_late_ns; g_kper_n++; if (g_periodic_late_ns > g_kper_max) g_kper_max = g_periodic_late_ns; }
        pe->fn();
        if (g_periodic_defer) continue;           /* a função pediu para esperar (o jogo ainda não leu o quadro anterior) */
        pe->next += pe->period;
        if (now >= pe->next + pe->period * (uint64_t)pe->catchup) { pe->next = now + pe->period; g_kdrops++; }
    }
    for (int i = 0; i < nhandles; i++) {
        KObj *o = handles[i];
        if (o->kind == OBJ_TIMER && o->timer_active && now >= o->fire_ns) {
            o->signaled = 1;
            if (o->interval_ns) {
                o->fire_ns += o->interval_ns;
                if (o->fire_ns <= now) o->fire_ns = now + o->interval_ns;  /* atrasou: não acumula disparos */
            } else o->timer_active = 0;
        }
    }
}

/* reavalia todas as esperas (em ordem de prioridade) */
static void (*g_async_poll)(void);
static volatile int g_async_pending;
void k_set_async_poll(void (*fn)(void)) { g_async_poll = fn; }
/* chamada por threads do PC fora do jogo (GPU): há eventos a entregar; acorda o escalonador se estiver ocioso */
void k_async_notify(void) {
    __atomic_store_n(&g_async_pending, 1, __ATOMIC_SEQ_CST);
    plat_kick();   /* acorda a espera ociosa do kernel (no Windows ela não está na condição) */
    if (pthread_mutex_trylock(&big) == 0) { pthread_cond_broadcast(&kcv); pthread_mutex_unlock(&big); }
}
static void update_waiters(void) {
    if (__atomic_exchange_n(&g_async_pending, 0, __ATOMIC_SEQ_CST) && g_async_poll) g_async_poll();
    uint64_t now = k_now_ns();
    process_timers(now);
    for (int p = 0; p < 64; p++)
        for (int i = 0; i < nthreads; i++)
            if (threads[i]->state == T_WAITING && threads[i]->prio == p) try_satisfy(threads[i], now);
    /* eventos/timers do tipo "pulse" só valem para quem já estava esperando */
    for (int i = 0; i < nhandles; i++)
        if ((handles[i]->kind == OBJ_EVENT || handles[i]->kind == OBJ_TIMER) && handles[i]->reset == RESET_PULSE)
            handles[i]->signaled = 0;
}

static uint64_t next_deadline(void) {
    uint64_t d = NO_DEADLINE;
    for (int i = 0; i < nthreads; i++)
        if (threads[i]->state == T_WAITING && threads[i]->deadline < d) d = threads[i]->deadline;
    for (int i = 0; i < nhandles; i++)
        if (handles[i]->kind == OBJ_TIMER && handles[i]->timer_active && handles[i]->fire_ns < d) d = handles[i]->fire_ns;
    for (int i = 0; i < g_nperiodic; i++) if (g_periodic[i].next < d) d = g_periodic[i].next;
    return d;
}

static void threads_report(void);
static void deadlock_report(void) {
    fprintf(stderr, "[kernel] DEADLOCK: nenhuma thread pronta e nada agendado no tempo\n");
    threads_report();
    exit(8);
}
/* kill -USR1 <pid>: mostra o estado de cada thread (para achar esperas que nunca terminam) */
static void on_usr1(int sig) { (void)sig; fprintf(stderr, "[kernel] estado das threads (SIGUSR1):\n"); threads_report(); }
static void threads_report(void) {
    for (int i = 0; i < nthreads; i++) {
        KThread *t = threads[i];
        fprintf(stderr, "  thread %d entry=%08x prio=%d núcleo=%d estado=%s", t->id, t->entry, t->prio, t->core,
                t->state == T_DEAD ? "morta" : t->state == T_READY ? "pronta" : "esperando");
        if (t->state == T_WAITING) {
            if (t->wait_kind == W_ARBITER) fprintf(stderr, " árbitro em %08x", t->arb_addr);
            for (int k = 0; k < t->wait_n && t->wait_kind == W_HANDLES; k++)
                fprintf(stderr, " %s%s", k ? ", " : "handles: ", k_name(t->wait_handles[k]));
        }
        fprintf(stderr, "  lr=%08x pc~%08x\n", t->ctx.r[14], t->ctx.r[15]);
    }
}

/* Dois núcleos emulados em tempo compartilhado: as threads criadas para o núcleo 1 formam um segundo
 * domínio de prioridades. Cada núcleo recebe fatias de ~2 ms alternadas (g_turn), de modo que uma thread do
 * núcleo 1 roda mesmo quando o núcleo 0 tem threads mais prioritárias prontas — como no 3DS. */
static int g_turn; static uint64_t g_slice_start;
volatile uint64_t g_kidle_ns, g_ksw, g_kdrops, g_kboost;
/* estatística de ritmo: atraso ao acordar do prazo, atraso dos disparos periódicos (VBlank), espera pela vez */
volatile uint64_t g_kwake_max, g_kwake_n2, g_kper_sum, g_kper_n, g_kper_max, g_kq_sum, g_kq_n, g_kq_max;   /* g_kdrops: disparos periódicos descartados por atraso */    /* tempo ocioso (nenhuma thread pronta) e trocas de thread, para o perfil */
#define SLICE_NS 2000000ull
/* Prioridade da thread principal: no 3DS um trabalhador mais prioritário (ex.: carregamento, prio 36) leva o
 * tempo que leva; aqui, se a thread principal (prio 48) está pronta há mais de AGING_NS esperando por uma
 * thread do mesmo núcleo, ela ganha a vez na próxima chamada ao kernel, para não perder o quadro. A outra
 * volta logo que a principal esperar pelo VBlank. Desligado por padrão (uma espera ativa da principal sem chamar o kernel travaria o jogo);
 * PRIORIDADE_PRINCIPAL=<ms> liga, para testes. */
static uint64_t g_aging_ns = 0; static int g_force_main = -1;
static int pick_best(void) {
    if (g_force_main >= 0) { int f = g_force_main; g_force_main = -1;
        if (f < nthreads && threads[f]->state == T_READY) return f; }
    int b[2] = { -1, -1 }, start = 0;
    for (int i = 0; i < nthreads; i++) if (threads[i] == current) start = i + 1;
    for (int k = 0; k < nthreads; k++) {
        int i = (start + k) % nthreads, cr = threads[i]->core;
        if (threads[i]->state == T_READY && (b[cr] < 0 || threads[i]->prio < threads[b[cr]]->prio)) b[cr] = i;
    }
    if (b[g_turn] >= 0) return b[g_turn];
    return b[!g_turn];
}

/* escolhe a próxima thread pronta e passa o bastão; volta quando `self` for a atual de novo */
static void schedule(void) {
    for (;;) {
        update_waiters();
        int best = pick_best();
        if (best >= 0) {
            if (g_kernel_trace && threads[best] != current)
                fprintf(stderr, "[sched] -> thread %d (prio %d)\n", threads[best]->id, threads[best]->prio);
            if (threads[best] != current) {
                g_ksw++;
                uint64_t now = k_now_ns();
                if (self && self->state == T_READY) self->ready_ns = now;      /* trocada sem esperar: volta à fila agora */
                KThread *b = threads[best];
                if (b->id == 0 && b->ready_ns && now > b->ready_ns) {          /* thread principal do jogo */
                    uint64_t q = now - b->ready_ns; g_kq_sum += q; g_kq_n++; if (q > g_kq_max) g_kq_max = q;
                    if (q > 3000000ull && getenv("ESPERA_LOG"))
                        fprintf(stderr, "[espera] principal esperou %.1f ms; rodava thread %d (núcleo %d, prio %d)\n",
                                q / 1e6, self ? self->id : -1, self ? self->core : -1, self ? self->prio : -1);
                }
            }
            current = threads[best];
            pthread_cond_broadcast(&kcv); plat_kick();
            break;
        }
        uint64_t d = next_deadline();
        if (d == NO_DEADLINE) deadlock_report();
        current = NULL;
        uint64_t i0 = k_now_ns();
        plat_cond_wait_until(&kcv, &big, host_deadline(d));
        { uint64_t i1 = k_now_ns(); g_kidle_ns += i1 - i0;
          if (i1 > d && d != NO_DEADLINE) { uint64_t l = i1 - d; if (l > g_kwake_max) g_kwake_max = l; if (l > 2000000ull) g_kwake_n2++; } }
    }
    while (current != self) pthread_cond_wait(&kcv, &big);
}

/* se alguma thread pronta tem prioridade maior que a atual, cede o núcleo */
static void maybe_preempt(void) {
    update_waiters();
    uint64_t now = k_now_ns();
    if (now - g_slice_start > SLICE_NS) {          /* fim da fatia: passa a vez para o outro núcleo, se ele tem trabalho */
        g_slice_start = now;
        for (int i = 0; i < nthreads; i++)
            if (threads[i]->state == T_READY && threads[i]->core != self->core) { g_turn = threads[i]->core; schedule(); return; }
    }
    for (int i = 0; i < nthreads; i++)
        if (threads[i]->state == T_READY && threads[i]->core == self->core && threads[i]->prio < self->prio) { schedule(); return; }
    if (g_aging_ns && self->id != 0 && nthreads && threads[0]->state == T_READY && threads[0]->ready_ns &&
        now - threads[0]->ready_ns > g_aging_ns) { g_force_main = 0; g_kboost++; schedule(); }
}

static void block(void) { self->state = T_WAITING; schedule(); }

static uint64_t ns_deadline(uint32_t lo, uint32_t hi) {
    int64_t ns = (int64_t)(((uint64_t)hi << 32) | lo);
    if (ns < 0) return NO_DEADLINE;
    return k_now_ns() + (uint64_t)ns;
}

void k_signal(uint32_t h) {
    KObj *o = k_get(h);
    if (o && (o->kind == OBJ_EVENT || o->kind == OBJ_TIMER)) o->signaled = 1;
    if (self) maybe_preempt();
}

uint32_t k_new_periodic_event(const char *name, uint64_t period_ns) {
    uint32_t h = k_new(OBJ_TIMER, name);
    KObj *o = k_get(h);
    o->reset = RESET_ONESHOT; o->timer_active = 1; o->interval_ns = period_ns; o->fire_ns = k_now_ns() + period_ns;
    return h;
}

/* ---------------------------------------------------------------- threads */
/* a thread atual terminou: passa o bastão para outra (esperando o próximo prazo se ninguém estiver pronto).
 * Volta com a trava `big` liberada; quem chama encerra a pthread. */
static void thread_finish(KThread *me) {
    me->state = T_DEAD;
    /* como no 3DS: os mutexes que a thread ainda tinha ficam livres (senão quem espera por eles trava para sempre) */
    for (int i = 0; i < nhandles; i++) {
        KObj *o = handles[i];
        if (o && o->kind == OBJ_MUTEX && o->owner == me) {
            static int nl; if (nl++ < 5) fprintf(stderr, "[kernel] thread %d terminou com um mutex preso: liberado\n", me->id);
            o->owner = NULL; o->lock_count = 0;
        }
    }
    self = NULL;
    for (;;) {
        update_waiters();
        int alive = 0, best = -1;
        for (int i = 0; i < nthreads; i++) {
            if (threads[i]->state != T_DEAD) alive = 1;
            if (threads[i]->state == T_READY && (best < 0 || threads[i]->prio < threads[best]->prio)) best = i;
        }
        if (!alive) { fprintf(stderr, "[kernel] todas as threads terminaram\n"); exit(0); }
        if (best >= 0) { current = threads[best]; break; }
        uint64_t d = next_deadline();
        if (d == NO_DEADLINE) deadlock_report();
        current = NULL;
        plat_cond_wait_until(&kcv, &big, host_deadline(d));
        if (current) return (void)pthread_mutex_unlock(&big);   /* outra thread ociosa já escolheu */
    }
    pthread_cond_broadcast(&kcv); plat_kick();
    pthread_mutex_unlock(&big);
}

static void *thread_main(void *p) {
    KThread *t = p;
    pthread_mutex_lock(&big);
    self = t;
    plat_thread_boost();              /* só uma thread do jogo roda por vez (trava global): pode ter prioridade alta no SO */
    while (current != t) pthread_cond_wait(&kcv, &big);
    call_indirect(&t->ctx, t->entry);
    thread_finish(t);                 /* a função de entrada retornou: equivale a ExitThread */
    return NULL;
}

static KThread *new_thread(uint32_t entry, uint32_t arg, uint32_t stack, int prio, uint32_t tls) {
    if (nthreads >= MAX_THREADS) { fprintf(stderr, "[kernel] threads demais\n"); exit(9); }
    KThread *t = calloc(1, sizeof *t);
    t->id = nthreads; t->prio = prio; t->state = T_READY; t->entry = entry;
    t->ctx.r[0] = arg; t->ctx.r[13] = stack; t->ctx.tls = tls;
    t->deadline = NO_DEADLINE;
    t->handle = k_new(OBJ_THREAD, "thread");
    k_get(t->handle)->thread = t;
    threads[nthreads++] = t;
    return t;
}

static void start_host_thread(KThread *t) {
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, (plat_is_windows() ? 64u : 256u) << 20);   /* pilha do PC para as chamadas recompiladas */
    if (pthread_create(&t->pt, &at, thread_main, t)) { perror("pthread_create"); exit(9); }
}

static void kernel_init(void) {
    { const char *e = getenv("PRIORIDADE_PRINCIPAL"); if (e) g_aging_ns = (uint64_t)(atof(e) * 1e6); }   /* ms */
    pthread_condattr_t ca; pthread_condattr_init(&ca);
#ifndef _WIN32
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&kcv, &ca);
}

static volatile uint64_t g_last_svc_ns;
static void *watchdog(void *arg);
static void *kpoll_clock(void *arg);

void kernel_run(uint32_t entry, uint32_t stack_top, uint32_t tls, int prio) {
    kernel_init();
#ifdef SIGUSR1
    signal(SIGUSR1, on_usr1);
#else
    (void)on_usr1;
#endif
    if (!getenv("SEM_KPOLL")) { pthread_t w; pthread_create(&w, NULL, kpoll_clock, NULL); }
    if (!getenv("SEM_WATCHDOG")) { pthread_t w; g_last_svc_ns = k_now_ns(); pthread_create(&w, NULL, watchdog, NULL); }
    pthread_mutex_lock(&big);
    KThread *t = new_thread(entry, 0, stack_top, prio, tls);
    current = t;
    start_host_thread(t);
    pthread_mutex_unlock(&big);
    pthread_join(t->pt, NULL);
    /* a thread principal terminou; as outras continuam até o processo sair */
    for (;;) plat_sleep_ms(1000);
}

/* ---------------------------------------------------------------- syscalls */
static uint32_t g_tls_next = 0x1FF82200u;

/* cão de guarda (sempre ligado): se o jogo para de andar (sem VBlank ou sem syscall por alguns segundos),
 * escreve uma vez no registro o estado das threads, para descobrir onde travou */
extern long g_gsp_frames;
void pica_debug_dump(void);
static void *watchdog(void *arg) {
    (void)arg;
    long last_f = -1; uint64_t f_ns = k_now_ns(); int reported = 0;
    for (;;) {
        plat_sleep_ms(1000);
        uint64_t now = k_now_ns();
        long f = g_gsp_frames;
        if (f != last_f) { last_f = f; f_ns = now; reported = 0; }
        KThread *t = current;
        int svc_stuck = t && now - g_last_svc_ns > 4000000000ull, frames_stuck = f > 0 && now - f_ns > 4000000000ull;
        if ((svc_stuck || frames_stuck) && !reported) {
            reported = 1;
            fprintf(stderr, "[watchdog] o jogo parou: quadro %ld parado há %llus, última syscall há %llus, thread atual %d\n", f,
                    (unsigned long long)((now - f_ns) / 1000000000ull), (unsigned long long)((now - g_last_svc_ns) / 1000000000ull), t ? t->id : -1);
            if (t) { fprintf(stderr, "[watchdog] registradores:"); for (int k = 0; k < 16; k++) fprintf(stderr, " r%d=%08x", k, t->ctx.r[k]); fprintf(stderr, "\n"); }
            threads_report();
            pica_debug_dump();
            fflush(stderr);
        }
    }
    return NULL;
}

/* depuração: EVLOG=5d,5e registra criação/sinal/limpeza desses eventos */
static void ev_log(const char *what, uint32_t h, Ctx *c, int tid) {
    static int init; static uint32_t hs[8]; static int nh;
    if (!init) { init = 1; const char *e = getenv("EVLOG"); while (e && *e && nh < 8) { hs[nh++] = (uint32_t)strtoul(e, (char **)&e, 16); if (*e == ',') e++; } }
    for (int i = 0; i < nh; i++) if (hs[i] == h) { fprintf(stderr, "[evento] %s %x pela thread %d (lr %08x)\n", what, h, tid, c->r[14]); return; }
}
int k_svc(Ctx *c, uint32_t n) {
    KThread *me = self;
    g_last_svc_ns = k_now_ns();
    { static int tl = -2; if (tl == -2) tl = getenv("SVCLOG_THREAD") ? atoi(getenv("SVCLOG_THREAD")) : -1;   /* depuração */
      if (tl >= 0 && me && me->id == tl) fprintf(stderr, "[svc-t%d] %02x r0=%08x r1=%08x r2=%08x r3=%08x lr=%08x\n", tl, n, c->r[0], c->r[1], c->r[2], c->r[3], c->r[14]); }
    if (me) maybe_preempt();   /* timers e VBlank vencidos podem acordar threads mais prioritárias */
    switch (n) {
    case 0x08: {                                     /* CreateThread(prio, entry, arg, stack, core) */
        int prio = (int)c->r[0];
        if (prio < 0 || prio > 63) { c->r[0] = 0xE0E01BFD; return 1; }
        KThread *t = new_thread(c->r[1], c->r[2], c->r[3], prio, g_tls_next);
        t->core = (int)c->r[4] == 1 ? 1 : 0;
        g_tls_next += 0x200;
        if (g_kernel_trace || getenv("THREADS_LOG"))
            fprintf(stderr, "[kernel] thread %d criada: entry=%08x prio=%d núcleo=%d (pedido %d)\n", t->id, c->r[1], prio, t->core, (int)c->r[4]);
        c->r[0] = 0; c->r[1] = t->handle;
        start_host_thread(t);
        maybe_preempt();
        return 1;
    }
    case 0x09:                                       /* ExitThread */
        thread_finish(me);
        pthread_exit(NULL);
    case 0x0A: {                                     /* SleepThread(ns) */
        int64_t ns = (int64_t)(((uint64_t)c->r[1] << 32) | c->r[0]);
        if (ns <= 0) { schedule(); return 1; }       /* yield */
        me->wait_kind = W_SLEEP; me->wait_n = 0; me->deadline = k_now_ns() + (uint64_t)ns;
        block();
        return 1;
    }
    case 0x0B: { KObj *o = k_get(c->r[1]); c->r[0] = 0; c->r[1] = o && o->thread ? o->thread->prio : me->prio; return 1; }
    case 0x0C: { KObj *o = k_get(c->r[0]); if (o && o->thread) o->thread->prio = (int)c->r[1]; c->r[0] = 0; maybe_preempt(); return 1; }
    case 0x13: {                                     /* CreateMutex(locked) */
        uint32_t h = k_new(OBJ_MUTEX, "mutex");
        if (c->r[1]) { k_get(h)->owner = me; k_get(h)->lock_count = 1; }
        c->r[0] = 0; c->r[1] = h; return 1;
    }
    case 0x14: {                                     /* ReleaseMutex */
        KObj *o = k_get(c->r[0]);
        if (!o || o->kind != OBJ_MUTEX) { c->r[0] = RES_INVALID_HANDLE; return 1; }
        if (o->owner != me) { c->r[0] = 0xD8E0041F; return 1; }
        if (--o->lock_count == 0) o->owner = NULL;
        c->r[0] = 0; maybe_preempt(); return 1;
    }
    case 0x15: {                                     /* CreateSemaphore(initial, max) */
        uint32_t h = k_new(OBJ_SEMAPHORE, "semaphore");
        k_get(h)->count = (int)c->r[1]; k_get(h)->max = (int)c->r[2];
        c->r[0] = 0; c->r[1] = h; return 1;
    }
    case 0x16: {                                     /* ReleaseSemaphore(handle, count) -> anterior */
        KObj *o = k_get(c->r[1]);
        if (!o || o->kind != OBJ_SEMAPHORE) { c->r[0] = RES_INVALID_HANDLE; return 1; }
        int prev = o->count;
        if (o->count + (int)c->r[2] > o->max) { c->r[0] = 0xD8E007FD; return 1; }
        o->count += (int)c->r[2];
        c->r[0] = 0; c->r[1] = (uint32_t)prev; maybe_preempt(); return 1;
    }
    case 0x17: {                                     /* CreateEvent(reset) */
        uint32_t h = k_new(OBJ_EVENT, "event"); k_get(h)->reset = (int)c->r[1]; ev_log("cria", h, c, me->id);
        c->r[0] = 0; c->r[1] = h; return 1;
    }
    case 0x18: { KObj *o = k_get(c->r[0]); ev_log("sinaliza", c->r[0], c, me->id); if (o) o->signaled = 1; c->r[0] = o ? 0 : RES_INVALID_HANDLE; maybe_preempt(); return 1; }
    case 0x19: { KObj *o = k_get(c->r[0]); ev_log("limpa", c->r[0], c, me->id); if (o) o->signaled = 0; c->r[0] = o ? 0 : RES_INVALID_HANDLE; return 1; }
    case 0x1A: {                                     /* CreateTimer(reset) */
        uint32_t h = k_new(OBJ_TIMER, "timer"); k_get(h)->reset = (int)c->r[1];
        c->r[0] = 0; c->r[1] = h; return 1;
    }
    case 0x1B: {                                     /* SetTimer(h, inicial r2:r3, intervalo r1:r4) */
        KObj *o = k_get(c->r[0]);
        if (!o || o->kind != OBJ_TIMER) { c->r[0] = RES_INVALID_HANDLE; return 1; }
        int64_t initial = (int64_t)(((uint64_t)c->r[3] << 32) | c->r[2]);
        int64_t interval = (int64_t)(((uint64_t)c->r[4] << 32) | c->r[1]);
        o->timer_active = 1; o->fire_ns = k_now_ns() + (initial > 0 ? (uint64_t)initial : 0);
        o->interval_ns = interval > 0 ? (uint64_t)interval : 0;
        c->r[0] = 0; maybe_preempt(); return 1;
    }
    case 0x1C: { KObj *o = k_get(c->r[0]); if (o) o->timer_active = 0; c->r[0] = 0; return 1; }
    case 0x1D: { KObj *o = k_get(c->r[0]); if (o) o->signaled = 0; c->r[0] = 0; return 1; }
    case 0x21: c->r[0] = 0; c->r[1] = k_new(OBJ_ARBITER, "arbiter"); return 1;
    case 0x22: {                                     /* ArbitrateAddress(arb, addr, tipo, valor, ns r4:r5) */
        KObj *arb = k_get(c->r[0]);
        uint32_t addr = c->r[1], type = c->r[2]; int32_t value = (int32_t)c->r[3];
        if (!arb || arb->kind != OBJ_ARBITER) { c->r[0] = RES_INVALID_HANDLE; return 1; }
        if (type == 0 && g_kernel_trace) {
            fprintf(stderr, "[arb] SIGNAL %08x por thread %d; regs:", addr, me->id);
            for (int k = 0; k < 16; k++) fprintf(stderr, " r%d=%08x", k, c->r[k]);
            fprintf(stderr, "\n[arb] pilha:");
            for (int k = 0; k < 24; k++) fprintf(stderr, " %08x", MEM32(c->r[13] + 4 * k));
            fprintf(stderr, "\n");
        }
        if (type == 0) {                             /* SIGNAL: acorda até `value` threads (negativo = todas) */
            int woken = 0;
            for (int p = 0; p < 64; p++)
                for (int i = 0; i < nthreads; i++) {
                    KThread *t = threads[i];
                    if (t->prio != p || t->state != T_WAITING || t->wait_kind != W_ARBITER) continue;
                    if (t->arb != arb || t->arb_addr != addr) continue;
                    if (value >= 0 && woken >= value) continue;
                    t->state = T_READY; t->ready_ns = k_now_ns(); t->result = 0; woken++;
                }
            c->r[0] = 0; maybe_preempt(); return 1;
        }
        int32_t v = (int32_t)MEM32(addr);
        if (v < value) {
            if (type == 2 || type == 4) MEM32(addr) = (uint32_t)(v - 1);
            me->wait_kind = W_ARBITER; me->arb = arb; me->arb_addr = addr; me->wait_n = 0;
            me->deadline = (type >= 3) ? ns_deadline(c->r[4], c->r[5]) : NO_DEADLINE;
            block();
            c->r[0] = me->result; return 1;
        }
        if (g_kernel_trace) fprintf(stderr, "[arb] não esperou: mem[%08x]=%d valor=%d tipo=%u\n", addr, v, value, type);
        c->r[0] = 0; return 1;
    }
    case 0x23: k_close(c->r[0]); c->r[0] = 0; return 1;   /* CloseHandle */
    case 0x24: {                                     /* WaitSynchronization1(h, ns r2:r3) */
        KObj *o = k_get(c->r[0]);
        if (!o) { c->r[0] = RES_INVALID_HANDLE; return 1; }
        uint32_t h = c->r[0] == 0xFFFF8000u ? me->handle : c->r[0];
        me->wait_kind = W_HANDLES; me->wait_all = 0; me->wait_n = 1; me->wait_handles[0] = h;

        me->deadline = ns_deadline(c->r[2], c->r[3]);
        if (!try_satisfy(me, k_now_ns())) {
            if (me->deadline != NO_DEADLINE && me->deadline <= k_now_ns()) { c->r[0] = RES_TIMEOUT; return 1; }
            block();
        }
        c->r[0] = me->result; return 1;
    }
    case 0x25: {                                     /* WaitSynchronizationN(ns_lo, handles, n, all, ns_hi) */
        int cnt = (int)c->r[2];
        if (cnt < 0 || cnt > 64) { c->r[0] = 0xE0E01BFD; return 1; }
        for (int i = 0; i < cnt; i++) {
            uint32_t h = MEM32(c->r[1] + 4 * i);
            if (!k_get(h)) { c->r[0] = RES_INVALID_HANDLE; return 1; }
            me->wait_handles[i] = h == 0xFFFF8000u ? me->handle : h;
        }
        me->wait_kind = W_HANDLES; me->wait_all = (int)c->r[3]; me->wait_n = cnt;
        me->deadline = ns_deadline(c->r[0], c->r[4]);
        if (!try_satisfy(me, k_now_ns())) {
            if (me->deadline != NO_DEADLINE && me->deadline <= k_now_ns()) { c->r[0] = RES_TIMEOUT; c->r[1] = (uint32_t)-1; return 1; }
            block();
        }
        c->r[0] = me->result; c->r[1] = (uint32_t)me->index; return 1;
    }
    case 0x27: {                                     /* DuplicateHandle: mesmo objeto, mesmo número */
        uint32_t h = c->r[1] == 0xFFFF8000u ? me->handle : c->r[1];
        if (h >= HANDLE_BASE && h < HANDLE_BASE + (uint32_t)nhandles && g_href[h - HANDLE_BASE] < 65535) g_href[h - HANDLE_BASE]++;
        c->r[0] = 0; c->r[1] = h; return 1;
    }
    case 0x37: { KObj *o = k_get(c->r[1]); c->r[0] = 0; c->r[1] = o && o->thread ? (uint32_t)o->thread->id + 1 : 1; return 1; }
    }
    return 0;
}

/* processa timers mesmo quando ninguém bloqueia (chamado a cada syscall) */
void k_tick(void) {
    if (self) maybe_preempt();
}
volatile int g_kpoll;
/* só age se a thread atual está há um tempo sem syscall (um laço longo de verdade): o resto do jogo continua
 * trocando de thread só nas syscalls, como antes */
void k_poll(void) {
    g_kpoll = 0;
    if (k_now_ns() - g_last_svc_ns > 8000000ull) k_tick();
}
static void *kpoll_clock(void *arg) {           /* liga g_kpoll a cada ~2 ms (os laços do jogo olham) */
    (void)arg;
    for (;;) { plat_sleep_ms(2); g_kpoll = 1; }
    return NULL;
}

/* relatório de erro (crash.c): a thread do jogo que estava rodando e os registradores dela. Só lê; é chamado
 * de dentro do tratador de falha, então não pega trava nenhuma. */
void k_crash_info(FILE *o) {
    KThread *t = self ? self : current;
    if (!t) { fprintf(o, "[crash] thread do jogo: nenhuma (falha fora do código do jogo)\n"); return; }
    fprintf(o, "[crash] thread do jogo: %d (prioridade %d, %s)\n", t->id, t->prio, self ? "esta" : "a última a rodar");
    const uint32_t *r = t->ctx.r;
    fprintf(o, "[crash] pc %08x lr %08x sp %08x\n", r[15], r[14], r[13]);
    fprintf(o, "[crash] r0 %08x r1 %08x r2 %08x r3 %08x r4 %08x r5 %08x r6 %08x r7 %08x\n", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    fprintf(o, "[crash] r8 %08x r9 %08x r10 %08x r11 %08x r12 %08x\n", r[8], r[9], r[10], r[11], r[12]);
}

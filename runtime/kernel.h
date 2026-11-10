/* Kernel do 3DS em HLE: threads, escalonador de núcleo único e objetos de sincronização. */
#pragma once
#include "rt.h"
#include <stdint.h>

typedef enum {
    OBJ_NONE, OBJ_EVENT, OBJ_MUTEX, OBJ_SEMAPHORE, OBJ_TIMER, OBJ_THREAD, OBJ_ARBITER,
    OBJ_SESSION, OBJ_SHMEM, OBJ_OTHER
} ObjKind;

enum { RESET_ONESHOT = 0, RESET_STICKY = 1, RESET_PULSE = 2 };

struct KThread;
typedef struct KObj {
    ObjKind kind;
    char name[24];
    int reset, signaled;                 /* eventos e timers */
    int lock_count; struct KThread *owner; /* mutex */
    int count, max;                      /* semáforo */
    int timer_active; uint64_t fire_ns, interval_ns;
    struct KThread *thread;              /* objeto de thread */
    uint32_t user;                       /* dado livre para os serviços (ex.: endereço de memória compartilhada) */
} KObj;

#define RES_TIMEOUT        0x09401BFEu
#define RES_INVALID_HANDLE 0xD8E007F7u

uint32_t k_new(ObjKind kind, const char *name);   /* cria objeto e devolve o handle */
KObj *k_get(uint32_t handle);
const char *k_name(uint32_t handle);
void k_signal(uint32_t handle);                     /* sinaliza evento (e acorda quem espera) */
uint32_t k_new_periodic_event(const char *name, uint64_t period_ns); /* evento disparado por tempo */
uint64_t k_now_ns(void);
uint64_t k_time_pause(void);            /* trabalho de "hardware" (GPU etc.): não conta no relógio do jogo */
void k_time_resume(uint64_t t0);
void k_add_periodic_catchup(void (*fn)(void), uint64_t period_ns, int catchup);
void k_set_async_poll(void (*fn)(void));      /* função chamada pelo escalonador para entregar eventos de fora */
void k_async_notify(void);                     /* (de qualquer thread do PC) há eventos a entregar */
void k_set_signaled(uint32_t handle);               /* sinaliza sem trocar de thread (para callbacks) */
/* função chamada periodicamente pelo escalonador (ex.: VBlank a 60 Hz) */
void k_add_periodic(void (*fn)(void), uint64_t period_ns);

/* roda a thread principal do jogo (não retorna) */
void kernel_run(uint32_t entry, uint32_t stack_top, uint32_t tls, int prio);
/* trata syscalls do kernel; devolve 0 se o número não é do kernel */
int k_svc(Ctx *c, uint32_t n);
extern int g_kernel_trace;

/* Relatório de erro: se o programa cair (acesso inválido de memória, instrução ilegal, divisão por zero...),
 * escreve no log o que dá para saber naquela hora (tipo de erro, onde, quadro do jogo, thread e registradores
 * do jogo, módulos carregados) e sai com o código 10. O jogar.py monta com isso o arquivo erros/relatorio_*.txt,
 * que o lançador mostra. No Windows entra como o último tratador (o plat.c já usa um tratador vetorado para a
 * memória do 3DS, que continua tendo prioridade). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#include <execinfo.h>
#endif

extern long g_gsp_frames;
void k_crash_info(FILE *o);
void ldr_listar(FILE *o);
static volatile int g_dentro;

static void comum(void) {
    fprintf(stderr, "[crash] quadro do jogo: %ld\n", g_gsp_frames);
    k_crash_info(stderr);
    ldr_listar(stderr);
    fflush(stderr);
}
#ifdef _WIN32
static const char *nome_exc(DWORD c) {
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: return "acesso inválido de memória";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "instrução ilegal";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "divisão por zero";
    case EXCEPTION_STACK_OVERFLOW: return "estouro de pilha";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "fora dos limites";
    case EXCEPTION_PRIV_INSTRUCTION: return "instrução privilegiada";
    case EXCEPTION_IN_PAGE_ERROR: return "erro de leitura de página";
    default: return "exceção";
    }
}
static LONG WINAPI tratar(EXCEPTION_POINTERS *e) {
    if (g_dentro++) TerminateProcess(GetCurrentProcess(), 10);
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    HMODULE m = NULL; char nm[MAX_PATH] = "?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)r->ExceptionAddress, &m);
    if (m) { GetModuleFileNameA(m, nm, sizeof nm); }
    const char *b = strrchr(nm, '\\'); b = b ? b + 1 : nm;
    fprintf(stderr, "[crash] %s (0x%08lx) em %s+0x%llx\n", nome_exc(r->ExceptionCode), (unsigned long)r->ExceptionCode, b,
            (unsigned long long)((uintptr_t)r->ExceptionAddress - (uintptr_t)m));
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        fprintf(stderr, "[crash] %s no endereço %p\n", r->ExceptionInformation[0] == 1 ? "escrita" : r->ExceptionInformation[0] == 8 ? "execução" : "leitura",
                (void *)r->ExceptionInformation[1]);
    comum();
    TerminateProcess(GetCurrentProcess(), 10);
    return EXCEPTION_EXECUTE_HANDLER;
}
void crash_instalar(void) { SetUnhandledExceptionFilter(tratar); }
#else
static void tratar(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    if (g_dentro++) _exit(10);
    const char *n = sig == SIGSEGV ? "acesso inválido de memória" : sig == SIGBUS ? "erro de barramento" : sig == SIGILL ? "instrução ilegal"
                  : sig == SIGFPE ? "erro aritmético" : sig == SIGABRT ? "abortado" : "sinal";
    fprintf(stderr, "[crash] %s (sinal %d) no endereço %p\n", n, sig, si ? si->si_addr : NULL);
    void *bt[24]; int k = backtrace(bt, 24); backtrace_symbols_fd(bt, k, 2);
    comum();
    _exit(10);
}
void crash_instalar(void) {
    if (getenv("SEM_RELATORIO")) return;
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = tratar; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    static char pilha[1 << 16]; stack_t ss = { .ss_sp = pilha, .ss_size = sizeof pilha };
    sigaltstack(&ss, NULL);
    int s[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (unsigned i = 0; i < sizeof s / sizeof *s; i++) sigaction(s[i], &sa, NULL);
}
#endif

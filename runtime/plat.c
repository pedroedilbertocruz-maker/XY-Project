/* Implementação da camada de plataforma (ver plat.h). */
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <direct.h>

/* O espaço de 4 GB do 3DS é só reservado; cada pedaço de 64 KB é confirmado na primeira vez que o
 * código do jogo encosta nele (tratador de exceção vetorizado). Assim o Windows não precisa de 4 GB
 * de memória confirmada de uma vez. */
static uint8_t *g_base;
static LONG CALLBACK commit_on_fault(PEXCEPTION_POINTERS e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t *a = (uint8_t *)e->ExceptionRecord->ExceptionInformation[1];
    if (!g_base || a < g_base || a >= g_base + (1ull << 32)) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t *chunk = g_base + (((uint64_t)(a - g_base)) & ~0xFFFFull);
    if (!VirtualAlloc(chunk, 0x10000, MEM_COMMIT, PAGE_READWRITE)) return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_EXECUTION;
}
uint8_t *plat_reserve_guest_memory(void) {
    g_base = VirtualAlloc(NULL, 1ull << 32, MEM_RESERVE, PAGE_READWRITE);
    if (!g_base) { fprintf(stderr, "não consegui reservar 4 GB de endereços (erro %lu)\n", GetLastError()); exit(1); }
    AddVectoredExceptionHandler(1, commit_on_fault);
    return g_base;
}
int plat_open_read(const char *path) { return _open(path, _O_RDONLY | _O_BINARY); }
int plat_open_utf8(const char *path) {
    wchar_t w[1024]; if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024)) return -1;
    return _wopen(w, _O_RDONLY | _O_BINARY);
}
int plat_open_env(const char *var) {
    wchar_t wvar[64], *wpath;
    MultiByteToWideChar(CP_UTF8, 0, var, -1, wvar, 64);
    wpath = _wgetenv(wvar);                   /* caminho em UTF-16: nomes com acentos funcionam */
    if (!wpath) return -1;
    return _wopen(wpath, _O_RDONLY | _O_BINARY);
}
long long plat_pread(int fd, void *dst, uint32_t len, uint64_t off) {
    /* o runtime só lê com a trava do kernel, então posicionar + ler não tem corrida */
    if (_lseeki64(fd, (long long)off, SEEK_SET) < 0) return -1;
    return _read(fd, dst, len);
}
int plat_mkdir(const char *path) { return _mkdir(path); }
int plat_rmdir(const char *path) { return _rmdir(path); }
int plat_truncate(const char *path, uint64_t size) {
    int fd = _open(path, _O_RDWR | _O_BINARY);
    if (fd < 0) return -1;
    int r = _chsize_s(fd, (long long)size);
    _close(fd);
    return r;
}
void plat_sleep_ms(unsigned ms) { Sleep(ms); }
void plat_touch(void *p, uint32_t len) {
    volatile uint8_t *b = p;
    for (uint64_t o = 0; o < len; o += 0x1000) b[o] = b[o];
    if (len) b[len - 1] = b[len - 1];
}
int plat_is_windows(void) { return 1; }
int plat_cpu_count(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }

#include <time.h>
uint64_t plat_now_ns(void) {                 /* contador de alta precisão (QueryPerformanceCounter) */
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    uint64_t q = (uint64_t)c.QuadPart, fr = (uint64_t)f.QuadPart;
    return q / fr * 1000000000ull + q % fr * 1000000000ull / fr;
}
/* pede ao Windows o relógio de 1 ms (timeBeginPeriod) e, se der, 0,5 ms (NtSetTimerResolution); sem
 * isso as esperas acordam no tique padrão de 15,6 ms e o jogo recebe o VBlank atrasado (quadros perdidos) */
static void timer_res_init(void) {
    static int done; if (done) return; done = 1;
    HMODULE w = LoadLibraryA("winmm.dll");
    if (w) { typedef unsigned (WINAPI *TBP)(unsigned); TBP f = (TBP)(void *)GetProcAddress(w, "timeBeginPeriod"); if (f) f(1); }
    HMODULE n = GetModuleHandleA("ntdll.dll");
    if (n) { typedef LONG (NTAPI *NSTR)(ULONG, BOOLEAN, PULONG); NSTR f = (NSTR)(void *)GetProcAddress(n, "NtSetTimerResolution");
             ULONG cur; if (f) f(5000, TRUE, &cur); }
}
/* Espera ociosa do kernel: temporizador de alta resolução do Windows (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
 * precisão de ~0,5 ms qualquer que seja o relógio do sistema) + um evento que plat_kick dispara quando chega
 * trabalho (interrupção da GPU, troca de thread). O pthread_cond_timedwait do winpthreads acordava até 15 ms
 * depois do prazo mesmo com timeBeginPeriod(1), e o jogo perdia o VBlank. */
static HANDLE g_wtimer, g_kick;
static INIT_ONCE g_wait_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK wait_init(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    timer_res_init();
    g_kick = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_wtimer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!g_wtimer) g_wtimer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);   /* Windows antigo */
    return TRUE;
}
void plat_thread_boost(void) {
    if (!getenv("SEM_PRIORIDADE")) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
}

void plat_kick(void) {
    InitOnceExecuteOnce(&g_wait_once, wait_init, NULL, NULL);
    if (g_kick) SetEvent(g_kick);
}
void plat_cond_wait_until(pthread_cond_t *cv, pthread_mutex_t *mx, uint64_t deadline_ns) {
    InitOnceExecuteOnce(&g_wait_once, wait_init, NULL, NULL);
    uint64_t now = plat_now_ns();
    if (deadline_ns <= now) return;
    uint64_t rel = deadline_ns - now;
    if (!g_wtimer || !g_kick) {                       /* reserva: espera da condição */
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t ns = (uint64_t)ts.tv_nsec + rel;
        ts.tv_sec += (time_t)(ns / 1000000000ull); ts.tv_nsec = (long)(ns % 1000000000ull);
        pthread_cond_timedwait(cv, mx, &ts);
        return;
    }
    LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(rel / 100); if (!due.QuadPart) due.QuadPart = -1;   /* relativo, em 100 ns */
    SetWaitableTimer(g_wtimer, &due, 0, NULL, NULL, FALSE);
    pthread_mutex_unlock(mx);                         /* (um plat_kick entre aqui e a espera deixa o evento ligado: não se perde) */
    HANDLE h[2] = { g_kick, g_wtimer };
    WaitForMultipleObjects(2, h, FALSE, INFINITE);
    pthread_mutex_lock(mx);
}

#else
#include <sys/mman.h>
#include <unistd.h>
uint8_t *plat_reserve_guest_memory(void) {
    void *m = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); exit(1); }
    return m;
}
int plat_open_read(const char *path) { return open(path, O_RDONLY); }
int plat_open_env(const char *var) { const char *p = getenv(var); return p ? open(p, O_RDONLY) : -1; }
int plat_open_utf8(const char *path) { return open(path, O_RDONLY); }
long long plat_pread(int fd, void *dst, uint32_t len, uint64_t off) { return pread(fd, dst, len, (off_t)off); }
int plat_mkdir(const char *path) { return mkdir(path, 0755); }
int plat_rmdir(const char *path) { return rmdir(path); }
int plat_truncate(const char *path, uint64_t size) { return truncate(path, (off_t)size); }
void plat_sleep_ms(unsigned ms) { usleep(ms * 1000u); }
int plat_is_windows(void) { return 0; }
int plat_cpu_count(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
void plat_touch(void *p, uint32_t len) { (void)p; (void)len; }
#include <time.h>
uint64_t plat_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
/* a condição do kernel usa CLOCK_MONOTONIC (kernel_init) */
void plat_cond_wait_until(pthread_cond_t *cv, pthread_mutex_t *mx, uint64_t deadline_ns) {
    struct timespec ts = { (time_t)(deadline_ns / 1000000000ull), (long)(deadline_ns % 1000000000ull) };
    pthread_cond_timedwait(cv, mx, &ts);
}
void plat_kick(void) {}
void plat_thread_boost(void) {}
#endif

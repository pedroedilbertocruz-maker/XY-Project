/* Camada de plataforma: o que muda entre Linux e Windows (memória de 4 GB, arquivos, relógio, janela).
 * No Windows o runtime é compilado com o llvm-mingw (clang + winpthreads), então pthreads, stdio e
 * dirent continuam valendo; o resto passa por estas funções. */
#ifndef PLAT_H
#define PLAT_H
#include <stdint.h>

uint8_t *plat_reserve_guest_memory(void);            /* 4 GB endereçáveis (confirmados sob demanda no Windows) */
int plat_open_read(const char *path);                /* descritor só de leitura (binário) */
int plat_open_utf8(const char *path);                 /* abre só para leitura um caminho em UTF-8 (acentos no Windows) */
int plat_open_env(const char *var);                  /* abre o arquivo cujo caminho está na variável de ambiente (aceita acentos no Windows) */
long long plat_pread(int fd, void *dst, uint32_t len, uint64_t off);
int plat_mkdir(const char *path);
int plat_rmdir(const char *path);
int plat_truncate(const char *path, uint64_t size);
void plat_sleep_ms(unsigned ms);
int plat_is_windows(void);
int plat_cpu_count(void);                            /* núcleos lógicos */
/* garante que [p, p+len) da memória do jogo existe antes de passá-la a uma leitura/escrita do sistema
 * (no Windows o sistema não dispara o tratador que confirma páginas sob demanda) */
void plat_touch(void *p, uint32_t len);
/* relógio preciso (ns, origem qualquer) e espera numa condição até esse relógio chegar em `deadline_ns`.
 * A espera pode voltar antes (quem chama confere o estado e chama de novo); no Windows ela usa o relógio
 * de 1 ms do sistema só para a maior parte da espera e completa o resto cedendo a vez, porque o
 * pthread_cond_timedwait do winpthreads arredonda para o tique do sistema (até ~16 ms de atraso). */
#include <pthread.h>
uint64_t plat_now_ns(void);
void plat_cond_wait_until(pthread_cond_t *cv, pthread_mutex_t *mx, uint64_t deadline_ns);
void plat_thread_boost(void);                        /* prioridade do SO acima do normal para a thread atual (Windows) */
void plat_kick(void);                                /* acorda quem está em plat_cond_wait_until (chamar junto com o broadcast) */

/* janela e entrada (display_win32.c no Windows; display_null.c sem janela) */
void display_init(void);
extern int g_display_on;                             /* há janela (senão nem copia as telas) */
void display_frame(const uint32_t *top /* 400x240 0xRRGGBB */, const uint32_t *bottom /* 320x240 */);
void display_frame_hd(const uint32_t *top, int sc);   /* tela de cima em escala (NULL = sem) */
void display_frame_hd_bot(const uint32_t *bot, int sc);   /* tela de baixo em escala (320*sc x 240*sc; NULL = sem) */
void audio_push(const int16_t *samples, int frames);   /* som: estéreo 16 bits, 32728 Hz */
/* GPU (OpenGL 3.3): torna o contexto atual nesta thread (0 = sem OpenGL: usa o renderer em software),
 * solta o contexto, e busca as funções */
int gpu_gl_begin(void);
void gpu_gl_end(void);
void *gpu_gl_proc(const char *name);
extern const char *g_render_name;                    /* "OpenGL" ou "software" (barra de título) */
extern volatile uint32_t g_host_buttons;             /* botões vindos do teclado/controle */
extern volatile int g_host_touch, g_host_touch_x, g_host_touch_y, g_host_circle_x, g_host_circle_y;

#endif

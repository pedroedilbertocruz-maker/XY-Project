#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <time.h>
/* gsp::Gpu em HLE: fila de interrupções, fila de comandos GX e framebuffers.
 *
 * Memória compartilhada do GSP (por thread cliente; aqui só há o índice 0):
 *   +0x000  fila de interrupções (0x40 bytes)
 *   +0x200  informação de framebuffer das 2 telas (0x40 cada)
 *   +0x800  fila de comandos GX (cabeçalho 0x20 + 15 comandos de 0x20)
 * Comandos GX: 0 DMA, 1 lista de comandos da GPU (PICA200), 2 preenchimento de memória,
 * 3 transferência de tela, 4 cópia de textura, 5 flush de cache.
 * A execução das listas da PICA200 (o renderer 3D) ainda não existe: elas são só confirmadas.
 */
#include "kernel.h"
#include "pica.h"
#include "pica_int.h"
#include "plat.h"
int drawlog_active(void); void drawlog_restart(long frame);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

enum { IRQ_PSC0 = 0, IRQ_PSC1 = 1, IRQ_PDC0 = 2, IRQ_PDC1 = 3, IRQ_PPF = 4, IRQ_P3D = 5, IRQ_DMA = 6 };

static uint32_t g_gsp_shmem, g_gsp_event;
static int g_gsp_ready;
long g_gsp_frames, g_gsp_cmdlists, g_gsp_transfers, g_gsp_fills;
volatile long g_gsp_top_frames;   /* quadros desenhados da tela de cima (transferências 400 de altura): o fps da barra de título */
extern int g_ipc_trace;

static uint32_t shmem_base(void) { KObj *o = k_get(g_gsp_shmem); return o ? o->user : 0; }

/* informação do framebuffer de uma tela, como o jogo escreveu */
typedef struct { uint32_t active, left, right, stride, format, shown, unk; } FbInfo;
static FbInfo g_fb[2];

/* endereços em que o jogo mostrou a tela de cima (os buffers que ele alterna): só as transferências para eles
 * são quadros da tela de cima (60 fps); outras de 400 de altura são cópias de tela para efeitos */
static volatile uint32_t g_top_seen[4];
static void note_top_fb(uint32_t a) {
    if (!a) return;
    for (int i = 0; i < 4; i++) if (g_top_seen[i] == a) return;
    g_top_seen[3] = g_top_seen[2]; g_top_seen[2] = g_top_seen[1]; g_top_seen[1] = g_top_seen[0]; g_top_seen[0] = a;
}
int gsp_is_top_fb(uint32_t a) { for (int i = 0; i < 4; i++) if (g_top_seen[i] == a) return 1; return 0; }

static void signal_irq(int id) {
    uint32_t q = shmem_base();
    if (!g_gsp_ready || !q) return;
    uint8_t idx = MEM8(q), n = MEM8(q + 1);
    if (n >= 0x34) {                                     /* fila cheia: marca erro e descarta, mas acorda a thread do jogo
                                                          * (sem o sinal ela nunca esvazia a fila e o jogo para de vez) */
        MEM8(q + 2) = 1;
        static int nl; if (nl++ < 5) { fprintf(stderr, "[gsp] fila de interrupções cheia (id %d)\n", id); fflush(stderr); }
        k_set_signaled(g_gsp_event); return;
    }
    MEM8(q + 0xC + (idx + n) % 0x34) = (uint8_t)id;
    MEM8(q + 1) = n + 1;
    MEM8(q + 2) = 0;
    if (id == IRQ_PDC0 || id == IRQ_PDC1) {              /* troca de buffer pendente desta tela */
        int scr = id - IRQ_PDC0;
        uint32_t u = q + 0x200 + scr * 0x40;
        if (MEM8(u + 1)) {
            uint32_t i = MEM8(u) & 1;
            memcpy(&g_fb[scr], g_mem + u + 4 + i * 0x1C, sizeof(FbInfo));
            if (!scr) note_top_fb(g_fb[0].left);
            MEM8(u + 1) = 0;
        }
    }
    k_set_signaled(g_gsp_event);
}

/* ------------------------------------------------------------ captura de tela (PPM) */
static void rgb_of(uint32_t fmt, const uint8_t *p, uint8_t *o) {
    switch (fmt & 7) {
    case 0: o[0] = p[3]; o[1] = p[2]; o[2] = p[1]; break;                       /* RGBA8 (ABGR na memória) */
    case 1: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; break;                       /* RGB8 (BGR na memória) */
    case 2: { uint16_t v = p[0] | p[1] << 8; o[0] = (v >> 11) << 3; o[1] = ((v >> 5) & 63) << 2; o[2] = (v & 31) << 3; break; }
    case 3: { uint16_t v = p[0] | p[1] << 8; o[0] = (v >> 11) << 3; o[1] = ((v >> 6) & 31) << 3; o[2] = ((v >> 1) & 31) << 3; break; }
    default: { uint16_t v = p[0] | p[1] << 8; o[0] = (v >> 12) << 4; o[1] = ((v >> 8) & 15) << 4; o[2] = ((v >> 4) & 15) << 4; break; }
    }
}
static int bpp_of(uint32_t fmt) { return (fmt & 7) == 0 ? 4 : (fmt & 7) == 1 ? 3 : 2; }

/* salva as duas telas lado a lado num PPM. O framebuffer do 3DS é guardado "deitado" (240 x largura). */
/* copia as duas telas para imagens lineares 0xRRGGBB (cima 400x240, baixo 320x240) */
static int g_cap_mask = 3;                    /* telas a capturar (present: as que não voltariam no tempo) */
static void gsp_capture(uint32_t *top, uint32_t *bot) {
    for (int s = 0; s < 2; s++) {
        if (!((g_cap_mask >> s) & 1)) continue;
        FbInfo *f = &g_fb[s];
        int width = s == 0 ? 400 : 320;
        uint32_t *dst = s == 0 ? top : bot;
        if (!f->left) { memset(dst, 0, (size_t)width * 240 * 4); continue; }
        int bpp = bpp_of(f->format);
        uint32_t stride = f->stride ? f->stride : 240u * bpp;
        for (int x = 0; x < width; x++)
            for (int y = 0; y < 240; y++) {
                uint8_t c[3];
                rgb_of(f->format, g_mem + f->left + x * stride + (239 - y) * bpp, c);
                dst[y * width + x] = ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2];
            }
    }
}
static uint32_t g_top[400 * 240], g_bot[320 * 240];

void gsp_screenshot(const char *path) {
    int W = 400 + 320, H = 240;
    gsp_capture(g_top, g_bot);
    FILE *o = fopen(path, "wb");
    if (!o) return;
    fprintf(o, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t v = x < 400 ? g_top[y * 400 + x] : g_bot[y * 320 + x - 400];
            uint8_t c[3] = { (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
            fwrite(c, 1, 3, o);
        }
    fclose(o);
}

volatile long g_audio_under, g_audio_drop; volatile int g_audio_fill_ms, g_audio_target_ms;   /* estatística do som (janela SDL) */
static int g_async = -1;
static volatile int g_frame_end_pending;
/* estatística de ritmo (PICA_PERFIL): intervalos entre telas de cima em VBlanks (1, 2, 3, 4+), ponto do quadro
 * em que a tela ficou pronta, e quanto cada lista da GPU esperou na fila e levou para rodar */
static volatile long g_pace[4]; static volatile uint64_t g_vblank_ns, g_phase_sum; static volatile long g_phase_n;
static volatile uint64_t g_gq_wait_sum, g_gq_wait_max, g_gq_run_sum, g_gq_run_max; static volatile long g_gq_lists;
static void present(void);
static void vblank(void) {
    if (!g_gsp_ready) return;
    g_gsp_frames++;
    g_vblank_ns = k_now_ns();
    if (g_async > 0) g_frame_end_pending = 1; else pica_frame_end();
    signal_irq(IRQ_PDC0);
    signal_irq(IRQ_PDC1);
    if (g_gsp_frames % 300 == 0) {
        /* velocidade real: transferências de tela por segundo de relógio (1 ou 2 por quadro desenhado) */
        static double t_last; static long tr_last;
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = ts.tv_sec + ts.tv_nsec * 1e-9, fps = t_last ? (g_gsp_transfers - tr_last) / (now - t_last) : 0;
        t_last = now; tr_last = g_gsp_transfers;
        fprintf(stderr, "[gsp] quadro %ld: %ld listas da GPU, %ld transferências de tela, %ld preenchimentos; tela de cima em %08x (formato %u); %.1f transferências/s\n",
                g_gsp_frames, g_gsp_cmdlists, g_gsp_transfers, g_gsp_fills, g_fb[0].left, g_fb[0].format, fps);
        double el = t_last ? 0 : 0; (void)el;
        static double t_prev; double span = t_prev ? now - t_prev : 0; t_prev = now;
        if (span > 0 && ENVC("PICA_PERFIL"))
        { extern volatile uint64_t g_kidle_ns, g_ksw, g_kdrops; extern volatile long g_dsp_waits; static uint64_t sw0, kd0; static long au0, ad0, dw0;
            fprintf(stderr, "[perfil] %% do tempo: vértices %.1f, recorte %.1f, desenho %.1f, GPU->mem %.1f, mem->GPU %.1f, texturas %.1f | ocioso %.0f%% | %.0f trocas de thread/s | som: %ld faltas, %ld cortes, %lu quadros do DSP descartados, %ld esperas pelo jogo, fila %d ms (alvo %d)\n",
                    g_prof_ns[0] / 1e7 / span, g_prof_ns[1] / 1e7 / span, g_prof_ns[2] / 1e7 / span, g_prof_ns[3] / 1e7 / span, g_prof_ns[4] / 1e7 / span, g_prof_ns[5] / 1e7 / span,
                    g_kidle_ns / 1e7 / span, (g_ksw - sw0) / span, g_audio_under - au0, g_audio_drop - ad0, (unsigned long)(g_kdrops - kd0), g_dsp_waits - dw0, g_audio_fill_ms, g_audio_target_ms);
            g_kidle_ns = 0; sw0 = g_ksw; kd0 = g_kdrops; au0 = g_audio_under; ad0 = g_audio_drop; dw0 = g_dsp_waits;
            { static long vb0, tf0; long tf = g_gsp_top_frames;
              fprintf(stderr, "[ritmo] tela de cima %.1f/s, VBlanks %.1f/s | intervalos em VBlanks: 1=%ld 2=%ld 3=%ld 4+=%ld | pronta %.1f ms após o VBlank | listas da GPU: espera média %.1f ms (máx %.1f), execução média %.1f ms (máx %.1f)\n",
                      (tf - tf0) / span, (g_gsp_frames - vb0) / span, g_pace[0], g_pace[1], g_pace[2], g_pace[3],
                      g_phase_n ? g_phase_sum / 1e6 / g_phase_n : 0.0,
                      g_gq_lists ? g_gq_wait_sum / 1e6 / g_gq_lists : 0.0, g_gq_wait_max / 1e6, g_gq_lists ? g_gq_run_sum / 1e6 / g_gq_lists : 0.0, g_gq_run_max / 1e6);
              { extern volatile uint64_t g_kwake_max, g_kwake_n2, g_kper_sum, g_kper_n, g_kper_max, g_kq_sum, g_kq_n, g_kq_max, g_kboost;
                fprintf(stderr, "[ritmo] kernel: acordou atrasado máx %.1f ms (%lu vezes > 2 ms) | VBlank entregue com atraso médio %.2f ms (máx %.1f) | thread principal esperando a vez: média %.2f ms (máx %.1f), %lu vezes adiantada\n",
                        g_kwake_max / 1e6, (unsigned long)g_kwake_n2, g_kper_n ? g_kper_sum / 1e6 / g_kper_n : 0.0, g_kper_max / 1e6,
                        g_kq_n ? g_kq_sum / 1e6 / g_kq_n : 0.0, g_kq_max / 1e6, (unsigned long)g_kboost);
                g_kboost = 0; g_kwake_max = g_kwake_n2 = g_kper_sum = g_kper_n = g_kper_max = g_kq_sum = g_kq_n = g_kq_max = 0; }
              { extern int g_fps60; extern long g_interp_stats[4], g_q60_stats[3], g_q60_bad, g_q60_suspect;
                if (g_fps60 > 0) {
                    fprintf(stderr, "[60fps] quadros intermediários: %ld feitos, %ld sem quadro anterior, %ld cena nova (sem mistura) | mostradas: %ld intermediárias, %ld reais, %ld puladas, %ld descartadas, %ld com composição por cópias | tempo %.1f%%\n",
                            g_interp_stats[0], g_interp_stats[1], g_interp_stats[2], g_q60_stats[0], g_q60_stats[1], g_q60_stats[2], g_q60_bad, g_q60_suspect, g_prof_ns[6] / 1e7 / span);
                    g_q60_bad = 0; g_q60_suspect = 0;
                    memset(g_interp_stats, 0, sizeof g_interp_stats); memset(g_q60_stats, 0, sizeof g_q60_stats);
                } }
              vb0 = g_gsp_frames; tf0 = tf; memset((void *)g_pace, 0, sizeof g_pace); g_phase_sum = 0; g_phase_n = 0;
              g_gq_wait_sum = g_gq_wait_max = g_gq_run_sum = g_gq_run_max = 0; g_gq_lists = 0; } }
        memset(g_prof_ns, 0, sizeof g_prof_ns);
    }
    if (g_gsp_frames % 30 == 0) {           /* drawlog_agora.txt: registra os desenhos a partir do próximo quadro */
        { FILE *pc = fopen("pula_composicao.txt", "r");
          if (pc) { extern int g_dbg_skip; fclose(pc); remove("pula_composicao.txt"); g_dbg_skip = !g_dbg_skip;
                    fprintf(stderr, "[gsp] composição %s no quadro %ld\n", g_dbg_skip ? "desligada" : "ligada", g_gsp_frames); } }
        { FILE *sw = fopen("software_agora.txt", "r");
          if (sw) { extern volatile int g_force_sw; fclose(sw); remove("software_agora.txt"); g_force_sw = 1; } }
        { FILE *dm = fopen("dbg_modo.txt", "r");
          if (dm) { extern int g_dbg_modo; int v = 0; if (fscanf(dm, "%d", &v) != 1) v = 0; fclose(dm); remove("dbg_modo.txt"); g_dbg_modo = v;
                    fprintf(stderr, "[gsp] modo de depuração %d no quadro %ld\n", v, g_gsp_frames); } }
        FILE *dl = fopen("drawlog_agora.txt", "r");
        if (dl) { fclose(dl); remove("drawlog_agora.txt"); drawlog_restart(g_gsp_frames + 1);
                  fprintf(stderr, "[gsp] registro de desenhos ligado no quadro %ld\n", g_gsp_frames + 1); }
    }
    static const char *shot; static long every;
    if (!every) { shot = ENVC("SHOT"); every = ENVC("SHOT_EVERY") ? atol(ENVC("SHOT_EVERY")) : 60; }
    if (shot && g_gsp_frames % every == 0) {
        char p[512]; snprintf(p, sizeof p, "%s_%05ld.ppm", shot, g_gsp_frames);
        gsp_screenshot(p);
    }
    present();
}

/* mostra as telas na janela. Uma tela cuja cópia da GPU ainda está pendente não é mostrada (ficaria o quadro
 * velho daquele buffer): a janela mantém a imagem anterior e a thread da GPU chama de novo quando a cópia
 * termina (gsp_present_deferred) — como uma placa de vídeo que só troca a imagem com o quadro pronto. */
static pthread_mutex_t g_present_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_present_wanted;
static void present(void) {
    extern int pica_gl_out_pending(uint32_t);
    extern int g_fps60; extern int pica_gl_q60_take(uint32_t **, int *, int *, long); extern int pica_gl_q60_active(long);
    pthread_mutex_lock(&g_present_lock);
    /* 60 fps: a tela de cima vem da fila de imagens prontas (intermediária, real, ...), uma por VBlank */
    int a60 = g_fps60 > 0 && (g_display_on || ENVC("SHOT60")) && pica_gl_q60_active(g_gsp_frames);
    if ((!a60 && pica_gl_out_pending(g_fb[0].left)) || pica_gl_out_pending(g_fb[1].left)) {
        static int lg = -1; if (lg < 0) lg = ENVC("GL_PRESENT_LOG") != NULL;
        if (lg) fprintf(stderr, "[present] adiado (cópia pendente) %08x %08x\n", g_fb[0].left, g_fb[1].left);
        g_present_wanted = 1; pthread_mutex_unlock(&g_present_lock); return;
    }
    g_present_wanted = 0;
    if (g_display_on || ENVC("SHOT_HD") || ENVC("SHOT60")) {   /* janela: telas a cada VBlank */
        /* nunca volta no tempo: se o jogo aponta para um buffer cuja última cópia é mais velha que a da imagem
         * já mostrada (a cópia nova ainda nem chegou à GPU), mantém a imagem atual daquela tela */
        extern unsigned long pica_gl_out_seq(uint32_t);
        static uint32_t shown_addr[2]; static unsigned long shown_seq[2];
        int mask = 0;
        for (int k = 0; k < 2; k++) {
            uint32_t a = g_fb[k].left; unsigned long q = pica_gl_out_seq(a);
            if (a != shown_addr[k] && q && q < shown_seq[k]) continue;
            mask |= 1 << k; shown_addr[k] = a;
            if (q > shown_seq[k]) shown_seq[k] = q;
        }
        if (g_display_on && mask) { g_cap_mask = mask; gsp_capture(g_top, g_bot); g_cap_mask = 3; display_frame(g_top, g_bot); }
        extern int pica_gl_hd_screen(uint32_t, int, uint32_t **, int *, int *);
        static uint32_t *hd; static int cap, last_sc;
        int changed = 0, sc;
        if (a60) { int s60 = 0; if (pica_gl_q60_take(&hd, &cap, &s60, g_gsp_frames) == 1) { sc = s60; changed = 1; } else sc = last_sc;
                   const char *sp = ENVC("SHOT60"); static long n60;          /* depuração: salva as imagens mostradas em sequência */
                   static int ring = -1; if (ring < 0) ring = ENVC("SHOT60_ANEL") ? atoi(ENVC("SHOT60_ANEL")) : 0;
                   if (sp && changed && (ring || (access("shot60_agora.txt", 0) == 0 && n60 < (ENVC("SHOT60_N") ? atol(ENVC("SHOT60_N")) : 40)))) {
                       char p[512];
                       if (ring) { snprintf(p, sizeof p, "%s_%03ld.ppm", sp, n60 % ring); n60++; }
                       else snprintf(p, sizeof p, "%s_%03ld_%05ld.ppm", sp, n60++, g_gsp_frames);
                       FILE *o = fopen(p, "wb");
                       if (o) { fprintf(o, "P6\n%d %d\n255\n", 400 * sc, 240 * sc);
                                for (long i = 0; i < 400L * sc * 240 * sc; i++) { uint8_t c[3] = { (uint8_t)(hd[i] >> 16), (uint8_t)(hd[i] >> 8), (uint8_t)hd[i] }; fwrite(c, 1, 3, o); }
                                fclose(o); } } }
        else sc = g_fb[0].left ? pica_gl_hd_screen(g_fb[0].left, 400, &hd, &cap, &changed) : 0;
        if (changed || sc != last_sc) display_frame_hd(sc ? hd : NULL, sc);   /* só quando a imagem muda */
        last_sc = sc;
        { static uint32_t *hdb; static int capb, last_scb; int chb = 0;   /* tela de baixo em escala */
          int scb = g_fb[1].left ? pica_gl_hd_screen(g_fb[1].left, 320, &hdb, &capb, &chb) : 0;
          if (chb || scb != last_scb) display_frame_hd_bot(scb ? hdb : NULL, scb);
          last_scb = scb;
          { extern void captura_quadro(const uint32_t *, int, const uint32_t *, int, const uint32_t *, const uint32_t *);
            captura_quadro(hd, sc, hdb, scb, g_top, g_bot); }   /* F12 (captura.c) */
          const char *hp = ENVC("SHOT_HD");                 /* depuração: salva também a tela de baixo em escala */
          if (hp && scb > 1 && g_gsp_frames % 600 == 0) {
              char p[512]; snprintf(p, sizeof p, "%s_baixo_%05ld.ppm", hp, g_gsp_frames);
              FILE *o = fopen(p, "wb");
              if (o) { fprintf(o, "P6\n%d %d\n255\n", 320 * scb, 240 * scb);
                       for (long i = 0; i < 320L * scb * 240 * scb; i++) { uint8_t c[3] = { (uint8_t)(hdb[i] >> 16), (uint8_t)(hdb[i] >> 8), (uint8_t)hdb[i] }; fwrite(c, 1, 3, o); }
                       fclose(o); } } }
        { const char *hp = ENVC("SHOT_HD");               /* depuração: salva a tela de cima em escala */
          if (hp && sc > 1 && g_gsp_frames % 600 == 0) {
              char p[512]; snprintf(p, sizeof p, "%s_%05ld.ppm", hp, g_gsp_frames);
              FILE *o = fopen(p, "wb");
              if (o) { fprintf(o, "P6\n%d %d\n255\n", 400 * sc, 240 * sc);
                       for (long i = 0; i < 400L * sc * 240 * sc; i++) { uint8_t c[3] = { (uint8_t)(hd[i] >> 16), (uint8_t)(hd[i] >> 8), (uint8_t)hd[i] }; fwrite(c, 1, 3, o); }
                       fclose(o); } } }
    }
    pthread_mutex_unlock(&g_present_lock);
}
static void gsp_present_deferred(void) {
    extern volatile int g_pend_done; extern int g_fps60; extern int pica_gl_q60_new(long);
    if (!g_pend_done) return;
    g_pend_done = 0;
    if (g_present_wanted) present();
    /* 60 fps: a imagem ficou pronta depois do VBlank e este VBlank ainda não mostrou nada: mostra já, se ainda
     * está no começo do intervalo (senão espera o próximo, para o ritmo não ficar torto) */
    else if (g_fps60 > 0 && pica_gl_q60_new(g_gsp_frames) && k_now_ns() - g_vblank_ns < 8000000ull) present();
}

/* ------------------------------------------------------------ comandos GX */
static void fill(uint32_t start, uint32_t end, uint32_t value, uint32_t control) {
    if (!start || end <= start) return;
    int width = (control >> 8) & 3;                     /* 0: 16 bits, 1: 24 bits, 2/3: 32 bits */
    if (pica_gpu_fill(start, end, value, width)) return;   /* limpeza de um framebuffer que está na GPU */
    pica_gpu_will_write(start, end - start);
    for (uint32_t a = start; a < end;) {
        if (width == 0) { MEM16(a) = (uint16_t)value; a += 2; }
        else if (width == 1) { MEM8(a) = value; MEM8(a + 1) = value >> 8; MEM8(a + 2) = value >> 16; a += 3; }
        else { MEM32(a) = value; a += 4; }
    }
    pica_gpu_written(start, end - start);
}

/* transferência de tela: framebuffer em blocos 8x8 (morton) -> linear, com conversão de formato */
static int morton(int x, int y) {
    int i = 0;
    for (int b = 0; b < 3; b++) i |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1);
    return i;
}
static void display_transfer(uint32_t in, uint32_t out, uint32_t in_dim, uint32_t out_dim, uint32_t flags) {
    int iw = in_dim & 0xFFFF, ih = in_dim >> 16, ow = out_dim & 0xFFFF, oh = out_dim >> 16;
    int in_fmt = (flags >> 8) & 7, out_fmt = (flags >> 12) & 7;
    int linear_in = flags & 2;  /* bit 1: entrada já linear ("tiled -> linear" é o caso normal) */
    int flip = flags & 1, dont_swizzle = (flags >> 5) & 1;
    int out_tiled = linear_in || dont_swizzle;
    if (dont_swizzle) linear_in = 0;           /* tiled -> tiled: entrada também em blocos */
    int scale = (flags >> 24) & 3;               /* 1: reduz pela metade na horizontal; 2: nas duas direções */
    int ibpp = bpp_of(in_fmt), obpp = bpp_of(out_fmt);
    /* com redução (1 = 2x1, 2 = 2x2) as dimensões de saída informadas são as da entrada: a saída real é a metade
     * (como no Citra/Azahar). Antes a saída era escrita com o dobro da largura e passava do buffer do jogo. */
    if (scale) ow >>= 1;
    if (scale == 2) oh >>= 1;
    if (!in || !out || !iw || !ih) return;
    { static int nl; if (ENVC("GX_LOG") && nl++ < 400 && (out < 0x1F000000u || (flags & 0x23)))
        fprintf(stderr, "[gx] transferência %08x (%dx%d f%d) -> %08x (%dx%d f%d) flags %08x\n", in, iw, ih, in_fmt, out, ow, oh, out_fmt, flags); }
    { extern int pica_gl_transfer(uint32_t, int, int, uint32_t, int, int, uint32_t, int, int); extern void pica_gl_hd_forget(uint32_t);
      int r = pica_gl_transfer(in, iw, ih, out, ow, oh, flags, in_fmt, out_fmt);
      if (!(r & 2)) pica_gl_hd_forget(out);        /* sem imagem em escala para a janela */
      if (r & 1) return; }                           /* a GPU escreve a memória da tela quando terminar o quadro */
    pica_gpu_flush(in, (uint32_t)(iw * ih * ibpp));
    pica_gpu_will_write(out, (uint32_t)(ow * oh * obpp));
    for (int y = 0; y < oh && y < ih; y++)
        for (int x = 0; x < ow && x < iw; x++) {
            int sx = scale ? x * 2 : x, sy = scale == 2 ? y * 2 : y;
            if (sx >= iw || sy >= ih) continue;
            if (flip) sy = ih - 1 - sy;
            uint32_t src = linear_in ? in + (sy * iw + sx) * ibpp
                         : in + (((sy & ~7) * iw + (sx & ~7) * 8) + morton(sx & 7, sy & 7)) * ibpp;
            uint8_t rgb[3]; rgb_of(in_fmt, g_mem + src, rgb);
            /* saída em blocos (morton) quando a entrada é linear (linear -> tiled) ou com "dont_swizzle"
             * (tiled -> tiled); senão linear. Antes a saída era sempre linear: a cópia da tela usada como
             * textura nas transições (fim da cena do Fletchling) virava um mosaico. */
            uint8_t *d = out_tiled ? g_mem + out + (((y & ~7) * ow + (x & ~7) * 8) + morton(x & 7, y & 7)) * obpp
                                   : g_mem + out + (y * ow + x) * obpp;
            switch (out_fmt) {
            case 0: d[0] = 255; d[1] = rgb[2]; d[2] = rgb[1]; d[3] = rgb[0]; break;
            case 1: d[0] = rgb[2]; d[1] = rgb[1]; d[2] = rgb[0]; break;
            case 2: { uint16_t v = (rgb[0] >> 3) << 11 | (rgb[1] >> 2) << 5 | rgb[2] >> 3; d[0] = v; d[1] = v >> 8; break; }
            case 3: { uint16_t v = (rgb[0] >> 3) << 11 | (rgb[1] >> 3) << 6 | (rgb[2] >> 3) << 1 | 1; d[0] = v; d[1] = v >> 8; break; }
            default: { uint16_t v = (rgb[0] >> 4) << 12 | (rgb[1] >> 4) << 8 | (rgb[2] >> 4) << 4 | 15; d[0] = v; d[1] = v >> 8; break; }
            }
        }
    pica_gpu_written(out, (uint32_t)(ow * oh * obpp));
    { extern void pica_gl_finish_hd(void); extern void pica_gl_out_mark(uint32_t); pica_gl_finish_hd(); pica_gl_out_mark(out); }
}

static void texture_copy(uint32_t in, uint32_t out, uint32_t size, uint32_t in_gap_w, uint32_t out_gap_w) {
    uint32_t iw = (in_gap_w & 0xFFFF) * 16, ig = (in_gap_w >> 16) * 16;
    uint32_t ow = (out_gap_w & 0xFFFF) * 16, og = (out_gap_w >> 16) * 16;
    uint32_t ispan = iw ? (size + iw - 1) / iw * (iw + ig) : size, ospan = ow ? (size + ow - 1) / ow * (ow + og) : size;
    pica_gpu_flush(in, ispan);
    pica_gpu_will_write(out, ospan);
    if (!iw || !ow) { memmove(g_mem + out, g_mem + in, size); pica_gpu_written(out, ospan); return; }
    uint32_t out0 = out;
    uint32_t done = 0, ic = 0, oc = 0;
    while (done < size) {
        MEM8(out) = MEM8(in); in++; out++; done++;
        if (++ic == iw) { ic = 0; in += ig; }
        if (++oc == ow) { oc = 0; out += og; }
    }
    pica_gpu_written(out0, ospan);
}

static void gx_irq(int id);
static __thread const uint32_t *t_flat; static __thread uint32_t t_flat_n;
static __thread int t_in_cmd;            /* esta thread está executando um comando GX (60 fps: só ele grava cópias) */
int gsp_in_command(void) { return t_in_cmd; }
int g_oplog;   /* depuração (OPLOG=n): registra as operações de 2 quadros a partir do quadro de cima n */
static void run_command_(const uint32_t *w);
static void run_command(const uint32_t *w) { t_in_cmd = 1; run_command_(w); t_in_cmd = 0; }
static void run_command_(const uint32_t *w) {
    uint32_t id = w[0] & 0xFF;
    { static long from = -2; if (from == -2) from = ENVC("OPLOG") ? atol(ENVC("OPLOG")) : -1;
      g_oplog = from >= 0 && g_gsp_top_frames >= from && g_gsp_top_frames < from + 2;
      if (g_oplog && id != 1) fprintf(stderr, "[op] cmd %u %08x %08x %08x %08x %08x\n", id, w[1], w[2], w[3], w[4], w[5]);
      if (g_oplog && id == 1) fprintf(stderr, "[op] lista\n"); }
    { static long wa = -2; if (wa == -2) wa = ENVC("GX_WATCH") ? (long)strtoul(ENVC("GX_WATCH"), 0, 16) : -1;
      if (wa >= 0) fprintf(stderr, "[gxw] cmd %u %08x %08x %08x %08x %08x %08x %08x\n", id, w[1], w[2], w[3], w[4], w[5], w[6], w[7]); }
    switch (id) {
    case 0: pica_gpu_flush(w[1], w[3]); pica_gpu_will_write(w[2], w[3]);
            memmove(g_mem + w[2], g_mem + w[1], w[3]); pica_gpu_written(w[2], w[3]); gx_irq(IRQ_DMA); break;
    case 1: g_gsp_cmdlists++;
        if (t_flat) pica_run_flat(t_flat, t_flat_n); else pica_run_list(w[1], w[2]);
        gx_irq(IRQ_P3D); break;   /* lista de comandos da PICA200 */
    case 2:
        g_gsp_fills++;
        fill(w[1], w[3], w[2], w[7] & 0xFFFF); fill(w[4], w[6], w[5], w[7] >> 16);
        if (w[1]) gx_irq(IRQ_PSC0);
        if (w[4]) gx_irq(IRQ_PSC1);
        break;
    case 3: g_gsp_transfers++;
        static long tlog; extern long g_gsp_frames;
        if ((ENVC("PICA_DEBUG") && g_gsp_transfers < 6) ||
            (drawlog_active() && tlog++ < 60))
            fprintf(stderr, "[gsp] transferência %08x -> %08x w3=%08x w4=%08x flags=%08x\n", w[1], w[2], w[3], w[4], w[5]);
        if ((w[4] >> 16) == 400) {
            g_gsp_top_frames++;
            /* ritmo: quantos VBlanks desde a última tela de cima, e em que ponto entre dois VBlanks ela ficou pronta */
            static long last_vb; long d = g_gsp_frames - last_vb; last_vb = g_gsp_frames;
            g_pace[d <= 1 ? 0 : d == 2 ? 1 : d == 3 ? 2 : 3]++;
            uint64_t ph = k_now_ns() - g_vblank_ns; g_phase_sum += ph; g_phase_n++;
        }
        display_transfer(w[1], w[2], w[3], w[4], w[5]);   /* w[3] = dimensão de entrada, w[4] = de saída */
        if ((w[4] >> 16) == 400 && gsp_is_top_fb(w[2])) { extern void pica_gl_top_transfer_done(void); pica_gl_top_transfer_done(); }
        gx_irq(IRQ_PPF); break;
    case 4: texture_copy(w[1], w[2], w[3], w[4], w[5]); gx_irq(IRQ_PPF); break;
    case 5: break;
    default: fprintf(stderr, "[gsp] comando GX %u desconhecido\n", id);
    }
}

/* ---------------------------------------------------------------- GPU assíncrona
 * Como no 3DS, os comandos GX (listas da PICA, transferências, preenchimentos, cópias, DMA) rodam em paralelo
 * com o jogo, numa thread própria do PC; o jogo espera as interrupções (P3D, PPF, PSC...) quando precisa do
 * resultado. Assim a lógica do próximo quadro roda enquanto a GPU desenha este. GPU_SINCRONA=1 desliga. */
static pthread_mutex_t g_gq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gq_cv = PTHREAD_COND_INITIALIZER, g_gq_space = PTHREAD_COND_INITIALIZER;
#define GQ 256
static uint32_t g_gq[GQ][8]; static int g_gq_head, g_gq_n;
static uint32_t *g_gq_flat[GQ]; static uint32_t g_gq_nw[GQ];   /* listas da PICA já copiadas no envio */
static uint64_t g_gq_t[GQ];                                        /* quando cada comando entrou na fila */
static int g_irq_done[512]; static int g_irq_n;          /* interrupções a entregar (protegido por g_gq_lock) */
static void gx_irq(int id) {
    if (!g_async) { signal_irq(id); return; }
    pthread_mutex_lock(&g_gq_lock);
    if (g_irq_n < 512) g_irq_done[g_irq_n++] = id;
    pthread_mutex_unlock(&g_gq_lock);
    k_async_notify();
}
/* chamada pelo kernel (com a trava das threads do jogo) para entregar as interrupções da GPU */
static void gsp_async_poll(void) {
    int ids[512], n;
    pthread_mutex_lock(&g_gq_lock);
    n = g_irq_n; memcpy(ids, g_irq_done, sizeof(int) * n); g_irq_n = 0;
    pthread_mutex_unlock(&g_gq_lock);
    for (int i = 0; i < n; i++) signal_irq(ids[i]);
}
static volatile int g_gpu_busy; static volatile uint32_t g_gpu_cmd;
void gsp_debug_dump(void) {                 /* (cão de guarda: sem travas, só leitura) */
    fprintf(stderr, "[watchdog] gsp: fila da GPU %d, thread da GPU %s (comando %u), interrupções a entregar %d, listas %ld, transferências %ld\n",
            g_gq_n, g_gpu_busy == 1 ? "executando comando" : g_gpu_busy == 2 ? "apresentando a tela" : "esperando", g_gpu_cmd, g_irq_n, g_gsp_cmdlists, g_gsp_transfers);
}
static void *gpu_thread(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t w[8];
        pthread_mutex_lock(&g_gq_lock);
        while (!g_gq_n) {
            extern int pica_gl_pending(void); extern void pica_gl_pending_wait(int);
            if (pica_gl_pending()) {             /* cópias de tela em andamento: vigia as cercas enquanto não chega comando */
                pthread_mutex_unlock(&g_gq_lock);
                pica_gl_pending_wait(500);
                gsp_present_deferred();
                pthread_mutex_lock(&g_gq_lock);
                continue;
            }
            pthread_cond_wait(&g_gq_cv, &g_gq_lock);
        }
        memcpy(w, g_gq[g_gq_head], sizeof w);
        uint64_t tq = g_gq_t[g_gq_head];
        t_flat = g_gq_flat[g_gq_head]; t_flat_n = g_gq_nw[g_gq_head]; g_gq_flat[g_gq_head] = NULL;
        g_gq_head = (g_gq_head + 1) % GQ; g_gq_n--;
        pthread_cond_signal(&g_gq_space);
        pthread_mutex_unlock(&g_gq_lock);
        if (g_frame_end_pending) { g_frame_end_pending = 0; pica_frame_end(); }
        uint64_t t0 = k_now_ns();
        g_gpu_cmd = w[0] & 0xFF; g_gpu_busy = 1;
        run_command(w);
        g_gpu_busy = 2;
        gsp_present_deferred();
        g_gpu_busy = 0;
        if ((w[0] & 0xFF) == 1) {
            uint64_t t1 = k_now_ns(), wt = t0 - tq, rt = t1 - t0;
            g_gq_wait_sum += wt; if (wt > g_gq_wait_max) g_gq_wait_max = wt;
            g_gq_run_sum += rt; if (rt > g_gq_run_max) g_gq_run_max = rt;
            g_gq_lists++;
        }
        free((void *)t_flat); t_flat = NULL;
    }
    return NULL;
}
static void gx_submit(const uint32_t *w) {
    if (g_async < 0) {
        g_async = ENVC("GPU_SINCRONA") == NULL;
        if (g_async) {
            pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 16u << 20);
            if (pthread_create(&t, &at, gpu_thread, NULL)) g_async = 0;
            else k_set_async_poll(gsp_async_poll);
        }
    }
    if (!g_async) { run_command(w); return; }
    uint32_t *flat = NULL, nw = 0;
    if ((w[0] & 0xFF) == 1) flat = pica_capture_list(w[1], w[2], &nw);
    pthread_mutex_lock(&g_gq_lock);
    while (g_gq_n == GQ) pthread_cond_wait(&g_gq_space, &g_gq_lock);
    int slot = (g_gq_head + g_gq_n) % GQ;
    g_gq_flat[slot] = flat; g_gq_nw[slot] = nw; g_gq_t[slot] = k_now_ns();
    memcpy(g_gq[slot], w, 32);
    g_gq_n++;
    pthread_cond_signal(&g_gq_cv);
    pthread_mutex_unlock(&g_gq_lock);
}

static void trigger_queue(void) {
    uint32_t q = shmem_base() + 0x800;
    for (int guard = 0; guard < 15; guard++) {
        uint8_t idx = MEM8(q), n = MEM8(q + 1);
        if (!n) break;
        gx_submit((const uint32_t *)(g_mem + q + 0x20 + (idx % 15) * 0x20));
        MEM8(q) = (idx + 1) % 15;
        MEM8(q + 1) = n - 1;
    }
}

int gsp(Ctx *c, uint32_t *cmd) {
    uint32_t id = cmd[0] >> 16;
    switch (id) {
    case 0x0013:                                  /* RegisterInterruptRelayQueue(flags, desc, evento) */
        g_gsp_event = cmd[3];
        if (!g_gsp_shmem) g_gsp_shmem = k_new(OBJ_SHMEM, "gsp:memória");
        g_gsp_ready = 1;
        static int once;
        if (!once) { once = 1; k_add_periodic(vblank, ENVC("VBLANK_NS") ? atol(ENVC("VBLANK_NS")) : 16715000); }   /* ~59,8 Hz (VBLANK_NS: teste) */
        cmd[0] = 0x00130042; cmd[1] = 0x2A07; cmd[2] = 0; cmd[3] = 0; cmd[4] = g_gsp_shmem;
        return 1;
    case 0x000C: trigger_queue(); cmd[0] = 0x000C0040; cmd[1] = 0; return 1;   /* TriggerCmdReqQueue */
    case 0x0005: {                                /* SetBufferSwap(tela, FrameBufferInfo) */
        int s = cmd[1] & 1; memcpy(&g_fb[s], &cmd[2], sizeof(FbInfo));
        if (!s) note_top_fb(g_fb[0].left);
        cmd[0] = 0x00050040; cmd[1] = 0; return 1;
    }
    case 0x0004: {                                /* ReadHWRegs: devolve zeros */
        uint32_t size = cmd[2], ptr = MEM32(c->tls + 0x184);
        if (ptr) memset(g_mem + ptr, 0, size);
        cmd[0] = 0x00040042; cmd[1] = 0; return 1;
    }
    case 0x0001: case 0x0002: case 0x0008: case 0x0009: case 0x000A: case 0x000B: case 0x0014:
    case 0x0016: case 0x0017: case 0x0019: case 0x001A: case 0x001E: case 0x001F:
        cmd[0] = (id << 16) | 0x40; cmd[1] = 0; return 1;
    case 0x0018:                                  /* ImportDisplayCaptureInfo */
        cmd[0] = 0x00180240; cmd[1] = 0;
        for (int s = 0; s < 2; s++) { cmd[2 + s * 4] = g_fb[s].left; cmd[3 + s * 4] = g_fb[s].right;
                                      cmd[4 + s * 4] = g_fb[s].format; cmd[5 + s * 4] = g_fb[s].stride; }
        return 1;
    }
    return 0;
}

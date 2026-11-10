/* Captura de tela (F12): salva a tela de cima na resolução interna (2x, 4K...) como PNG na pasta CAPTURAS
 * (o jogar.py passa a pasta capturas/ do projeto). Se a tela de baixo está aparecendo na janela, ela vai junto,
 * embaixo, como no 3DS. A janela pede a imagem em escala cheia por alguns quadros (pica_gl/pica_vk reduzem a
 * leitura ao tamanho da janela), e o PNG é gravado numa thread à parte para o jogo não engasgar. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define STBIW_WINDOWS_UTF8
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "idioma_ui.h"

volatile int g_captura_pedida, g_captura_baixo;      /* display_sdl.c: F12 */
extern void (*g_ui_aviso)(const char *msg);

typedef struct { uint8_t *rgb; int w, h; char cam[1024], nome[160]; } Trabalho;
static void *gravar(void *arg) {
    Trabalho *t = arg;
    int ok = stbi_write_png(t->cam, t->w, t->h, 3, t->rgb, t->w * 3);
    char m[300];
    if (ok) snprintf(m, sizeof m, TR("Captura salva: %s", "Screenshot saved: %s", "Captura guardada: %s", "スクリーンショットを保存しました：%s"), t->nome);
    else snprintf(m, sizeof m, TR("Não consegui salvar a captura (%s)", "Could not save the screenshot (%s)", "No pude guardar la captura (%s)", "スクリーンショットを保存できませんでした（%s）"), t->cam);
    fprintf(stderr, "[captura] %s %s (%dx%d)\n", ok ? "salva" : "falhou", t->cam, t->w, t->h);
    if (g_ui_aviso) g_ui_aviso(m);
    free(t->rgb); free(t);
    return NULL;
}
/* copia uma tela (w0 x h0, ARGB) para o RGB de destino em escala k (vizinho mais próximo; k = escala/escala da origem) */
static void copiar(uint8_t *dst, int dw, int x0, int y0, const uint32_t *src, int sw, int sh, int w, int h) {
    for (int y = 0; y < h; y++) {
        const uint32_t *l = src + (long)(y * sh / h) * sw;
        uint8_t *d = dst + ((long)(y0 + y) * dw + x0) * 3;
        for (int x = 0; x < w; x++, d += 3) { uint32_t v = l[x * sw / w]; d[0] = (uint8_t)(v >> 16); d[1] = (uint8_t)(v >> 8); d[2] = (uint8_t)v; }
    }
}
/* chamado pelo gsp.c a cada imagem mostrada: hd/sc = tela de cima em escala (sc 0 = só a de 400x240), hdb/scb a de baixo */
void captura_quadro(const uint32_t *hd, int sc, const uint32_t *hdb, int scb, const uint32_t *top, const uint32_t *bot) {
    { extern long g_gsp_frames; static long alvo = -1;                 /* depuração: CAPTURA_QUADRO=n captura no quadro n */
      if (alvo < 0) { const char *e = getenv("CAPTURA_QUADRO"); alvo = e ? atol(e) : 0; }
      if (alvo > 0 && g_gsp_frames >= alvo) { alvo = 0; if (!g_captura_pedida) g_captura_pedida = 1; } }
    if (!g_captura_pedida) return;
    if (++g_captura_pedida < 6) return;              /* espera chegar a leitura em escala cheia */
    g_captura_pedida = 0;
    int s = sc > 0 ? sc : 1, baixo = g_captura_baixo;
    int W = 400 * s, H = 240 * s * (baixo ? 2 : 1);
    Trabalho *t = calloc(1, sizeof *t); if (!t) return;
    t->rgb = calloc((size_t)W * H, 3); if (!t->rgb) { free(t); return; }
    t->w = W; t->h = H;
    if (sc > 0) copiar(t->rgb, W, 0, 0, hd, 400 * sc, 240 * sc, W, 240 * s);
    else copiar(t->rgb, W, 0, 0, top, 400, 240, W, 240);
    if (baixo) {                                      /* tela de baixo centralizada embaixo, na mesma escala */
        if (scb > 0 && hdb) copiar(t->rgb, W, 40 * s, 240 * s, hdb, 320 * scb, 240 * scb, 320 * s, 240 * s);
        else copiar(t->rgb, W, 40 * s, 240 * s, bot, 320, 240, 320 * s, 240 * s);
    }
    const char *dir = getenv("CAPTURAS"); if (!dir || !*dir) dir = "capturas";
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    time_t tt = time(NULL); struct tm *lt = localtime(&tt);
    const char *v = getenv("JOGO_VERSAO");
    char base[96]; snprintf(base, sizeof base, "Pokemon_%s_%04d-%02d-%02d_%02d-%02d-%02d", v && *v ? v : "XY",
                            lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec);
    snprintf(t->nome, sizeof t->nome, "%s.png", base);
    snprintf(t->cam, sizeof t->cam, "%s/%s", dir, t->nome);
    for (int k = 2; k < 100; k++) {                  /* (duas no mesmo segundo: _2, _3...) */
        FILE *f = fopen(t->cam, "rb"); if (!f) break; fclose(f);
        snprintf(t->nome, sizeof t->nome, "%s_%d.png", base, k); snprintf(t->cam, sizeof t->cam, "%s/%s", dir, t->nome);
    }
    pthread_t th;
    if (pthread_create(&th, NULL, gravar, t) == 0) pthread_detach(th); else gravar(t);
}

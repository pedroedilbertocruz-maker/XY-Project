/* Idioma do programa (menus, avisos na tela, título da janela): 0 português, 1 English, 2 español, 3 日本語.
 * É escolhido no lançador na primeira vez (opcoes.txt, chave "idioma") e chega ao jogo pela variável IDIOMA
 * (pt/en/es/ja). Não tem nada a ver com o idioma do jogo (textos da Nintendo), que é outra opção. */
#ifndef IDIOMA_UI_H
#define IDIOMA_UI_H
#include <stdlib.h>
#include <string.h>

extern int g_ui_lang;                         /* input.c; -1 = ainda não lido do ambiente */
static inline int ui_lang_cod(const char *l) {
    return !l ? 0 : !strcmp(l, "en") ? 1 : !strcmp(l, "es") ? 2 : !strcmp(l, "ja") ? 3 : 0;
}
static inline int ui_lang(void) {
    if (g_ui_lang < 0) g_ui_lang = ui_lang_cod(getenv("IDIOMA"));
    return g_ui_lang;
}
#define TR(pt, en, es, ja) (ui_lang() == 1 ? (en) : ui_lang() == 2 ? (es) : ui_lang() == 3 ? (ja) : (pt))
static const char *const k_ui_lang_cod[4] = { "pt", "en", "es", "ja" };
static const char *const k_ui_lang_nome[4] = { "Português", "English", "Español", "日本語" };
#endif

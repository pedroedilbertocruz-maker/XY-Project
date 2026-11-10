/* Janela, entrada e som com SDL2 (Windows e Linux).
 *
 * Tela: as duas telas do 3DS uma em cima da outra (400x480), escaladas para a janela mantendo a
 * proporção. Som: as amostras que o DSP emulado produz (32728 Hz, estéreo) vão para a fila de áudio.
 *
 * Teclado (padrão do Citra/Azahar):
 *   setas = direcional analógico      T F G H = direcional digital (cima/esq/baixo/dir)
 *   A = A   S = B   Z = X   X = Y     Q = L   W = R     Enter ou M = START   Backspace ou N = SELECT
 *   F11 = tela cheia                  mouse (botão esquerdo) na tela de baixo = toque
 *   F9 = troca o layout (tela de cima ocupando a janela / as duas telas empilhadas)
 *   Tab = mostra/esconde a tela de baixo no canto (layout "tela de cima")   F10 = esticar para 16:9 / manter 5:3
 *   A mochila abre sozinha no canto.   F8 = resolução interna (1x, 2x, 3x, 4x)
 *   Controle: clique do analógico direito = mostra/esconde a tela de baixo
 * Controle: botões na posição do 3DS (o de baixo é B, o da direita é A), analógico esquerdo =
 * direcional analógico, LB/RB = L/R, Start/Back = START/SELECT.
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include "plat.h"
#include "idioma_ui.h"

int g_display_on;
volatile uint32_t g_host_buttons;
volatile int g_host_touch, g_host_touch_x, g_host_touch_y, g_host_circle_x, g_host_circle_y;

#define CW 400
#define CH 480
enum { B_A = 0, B_B, B_SELECT, B_START, B_RIGHT, B_LEFT, B_UP, B_DOWN, B_R, B_L, B_X, B_Y };

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex, *g_htex;   /* g_htex: tela de cima em resolução interna maior */
static int g_htex_w, g_htex_h, g_hd_sc; static uint32_t *g_hd_canvas; static int g_hd_cap; static volatile int g_hd_ready, g_hd_dirty;
/* tela de baixo em escala (mesma ideia) */
static SDL_Texture *g_hbtex; static int g_hbtex_w, g_hbtex_h, g_hb_sc; static uint32_t *g_hb_canvas; static int g_hb_cap; static volatile int g_hb_ready, g_hb_dirty;
static SDL_mutex *g_lock;
static uint32_t g_canvas[CW * CH];
static volatile int g_frame_ready;
static SDL_AudioDeviceID g_audio;
static uint32_t g_kb; static int g_left, g_right, g_up, g_down, g_mouse;
static SDL_Rect g_dst;                 /* área das duas telas (layout empilhado), em pontos da janela */
static SDL_Rect g_bot_dst;             /* tela de baixo na janela (qualquer layout); w = 0 se escondida */
/* layout: 1 = tela de cima ocupando a janela, tela de baixo sob demanda no canto inferior direito, translúcida;
 * 0 = as duas telas empilhadas como no 3DS. Guardado em janela.txt entre execuções. */
static int g_layout = 1, g_stretch = 1, g_show_bot = 0, g_hover_bot, g_res = 1;
/* escalas da resolução interna: 1x a 4x, 2K (6x, 2400x1440) e 4K (9x, 3600x2160) */
static const int k_res_ok[6] = { 1, 2, 3, 4, 6, 9 };
static int res_valida(int r) { for (int i = 0; i < 6; i++) if (k_res_ok[i] == r) return 1; return 0; }
extern volatile int g_res_req;          /* pica_gl.c: pedido de troca da resolução interna */
static void prefs_load(void) {
    FILE *f = fopen("janela.txt", "r"); if (!f) return;
    int a, b, c, d; int n = fscanf(f, "%d %d %d %d", &a, &b, &c, &d);
    if (n >= 3) { g_layout = a; g_stretch = b; g_show_bot = c; }
    if (n >= 4 && res_valida(d)) g_res = d;
    fclose(f);
    if (g_res > 1 && !getenv("RESOLUCAO")) g_res_req = g_res;
    { const char *e = getenv("RESOLUCAO"); int r = e ? atoi(e) : 0; if (res_valida(r)) g_res = r; }   /* escolha do lançador */
}
static void prefs_save(void) {
    FILE *f = fopen("janela.txt", "w"); if (!f) return;
    fprintf(f, "%d %d %d %d\n", g_layout, g_stretch, g_show_bot, g_res); fclose(f);
}

/* ---- mapeamento de teclas e botões (configurável no menu, página Controles; fica no opcoes.txt)
 * Cada ação tem até 2 teclas e até 2 botões de controle (SDL_GameController; 100/101 = gatilho esquerdo/direito).
 * As setas do teclado continuam sendo o Circle Pad. Controles DirectInput genéricos usam o mapeamento fixo. */
enum { AC_A, AC_B, AC_X, AC_Y, AC_L, AC_R, AC_START, AC_SELECT, AC_UP, AC_DOWN, AC_LEFT, AC_RIGHT, AC_TURBO, AC_PATINS, AC_N };
static const int k_ac_bit[AC_N] = { B_A, B_B, B_X, B_Y, B_L, B_R, B_START, B_SELECT, B_UP, B_DOWN, B_LEFT, B_RIGHT, -1, -1 };
static const char *const k_ac_cod[AC_N] = { "a", "b", "x", "y", "l", "r", "start", "select", "cima", "baixo", "esquerda", "direita", "acelerar", "patins" };
static SDL_Keycode g_tecla[AC_N][2];
static int g_botao[AC_N][2];
#define BT_LT 100
#define BT_RT 101
static int g_pad_labels = -1;                       /* 1 = botões por rótulo (o padrão do controle) */
static void teclas_padrao(void) {
    static const SDL_Keycode k[AC_N][2] = { { SDLK_a, 0 }, { SDLK_s, 0 }, { SDLK_z, 0 }, { SDLK_x, 0 }, { SDLK_q, 0 }, { SDLK_w, 0 },
        { SDLK_RETURN, SDLK_m }, { SDLK_BACKSPACE, SDLK_n }, { SDLK_t, 0 }, { SDLK_g, 0 }, { SDLK_f, 0 }, { SDLK_h, 0 }, { SDLK_SPACE, 0 }, { SDLK_p, 0 } };
    memcpy(g_tecla, k, sizeof k);
}
static void botoes_padrao(void) {                   /* por posição (como no 3DS: o da direita é o A) ou por rótulo */
    int lab = g_pad_labels > 0;
    int b[AC_N][2] = { { lab ? SDL_CONTROLLER_BUTTON_A : SDL_CONTROLLER_BUTTON_B, -1 }, { lab ? SDL_CONTROLLER_BUTTON_B : SDL_CONTROLLER_BUTTON_A, -1 },
        { lab ? SDL_CONTROLLER_BUTTON_X : SDL_CONTROLLER_BUTTON_Y, -1 }, { lab ? SDL_CONTROLLER_BUTTON_Y : SDL_CONTROLLER_BUTTON_X, -1 },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, -1 }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, -1 },
        { SDL_CONTROLLER_BUTTON_START, -1 }, { SDL_CONTROLLER_BUTTON_BACK, -1 },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, -1 }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, -1 }, { SDL_CONTROLLER_BUTTON_DPAD_LEFT, -1 }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, -1 },
        { BT_RT, -1 }, { BT_LT, -1 } };
    memcpy(g_botao, b, sizeof b);
}
/* lê/grava no arquivo de opções: tecla_<ação>=código,código e botao_<ação>=n,n (vazio = padrão) */
static int opc_ler(const char *k, char *v, int n) {
    const char *path = getenv("OPCOES"); if (!path || !*path) return 0;
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    char l[512]; size_t kl = strlen(k); int ok = 0;
    while (fgets(l, sizeof l, f)) if (!strncmp(l, k, kl) && l[kl] == '=') { char *s = l + kl + 1; s[strcspn(s, "\r\n")] = 0; snprintf(v, n, "%s", s); ok = 1; }
    fclose(f); return ok;
}
static volatile int g_turbo_mult = 2; static int g_mostrar_fps; static int g_patins;
static void controles_carregar(void) {
    if (g_pad_labels < 0) { const char *e = getenv("CONTROLE_BOTOES"); g_pad_labels = e && !strcmp(e, "rotulo"); }
    { char v[16]; if (opc_ler("acelerar", v, sizeof v)) { int m = atoi(v); if (m >= 2 && m <= 4) g_turbo_mult = m; }
      if (opc_ler("mostrar_fps", v, sizeof v)) g_mostrar_fps = atoi(v) != 0;
      if (opc_ler("patins", v, sizeof v)) g_patins = atoi(v) != 0; }
    teclas_padrao(); botoes_padrao();
    for (int a = 0; a < AC_N; a++) {
        char k[40], v[64]; long x0, x1;
        snprintf(k, sizeof k, "tecla_%s", k_ac_cod[a]);
        if (opc_ler(k, v, sizeof v) && *v && sscanf(v, "%ld,%ld", &x0, &x1) >= 1) { g_tecla[a][0] = (SDL_Keycode)x0; g_tecla[a][1] = strchr(v, ',') ? (SDL_Keycode)x1 : 0; }
        snprintf(k, sizeof k, "botao_%s", k_ac_cod[a]);
        if (opc_ler(k, v, sizeof v) && *v && sscanf(v, "%ld,%ld", &x0, &x1) >= 1) { g_botao[a][0] = (int)x0; g_botao[a][1] = strchr(v, ',') ? (int)x1 : -1; }
    }
}
static int key_bit(SDL_Keycode k) {
    for (int a = 0; a < AC_N; a++) if (k && (g_tecla[a][0] == k || g_tecla[a][1] == k)) return k_ac_bit[a] >= 0 ? k_ac_bit[a] : 100 + a;
    return -1;
}
static int g_menu_on, g_menu_dirty;      /* (menu_jogo.h) */
static void opcoes_set(const char *k, const char *v);
/* F12: captura de tela (captura.c grava o PNG) */
static void captura_pedir(void) {
    extern volatile int g_captura_pedida, g_captura_baixo;
    if (g_captura_pedida) return;
    g_captura_baixo = g_bot_dst.w > 0; g_captura_pedida = 1;
}
/* acelerar: segurando a tecla ou o botão, o jogo anda na velocidade escolhida (2x, 3x ou 4x; opção "acelerar") */
extern volatile int g_k_vel;
static int g_turbo_tecla, g_turbo_pad;
/* patins: desligados, o Circle Pad (analógico e setas) vira o direcional, e o personagem anda/corre (B) em vez de
 * patinar. Alternado pela tecla/botão da ação "patins" (P / LT); o ícone aparece por uns segundos no canto. */
static int g_patins = 1; static Uint32 g_patins_t0;
static void patins_alternar(void) {
    g_patins = !g_patins; g_patins_t0 = SDL_GetTicks(); if (!g_patins_t0) g_patins_t0 = 1;
    opcoes_set("patins", g_patins ? "1" : "0");
    fprintf(stderr, "[patins] %s\n", g_patins ? "ligados" : "desligados");
}
/* página Controles do menu: esperando a tecla/botão para uma ação (g_bind_ac = ação, -1 = não; col 0 teclado, 1 controle) */
static int g_bind_ac = -1, g_bind_col, g_bind_armado, g_bind_soltar;
static void controles_salvar(void) {
    for (int a = 0; a < AC_N; a++) {
        char k[40], v[48];
        snprintf(k, sizeof k, "tecla_%s", k_ac_cod[a]);
        if (g_tecla[a][1]) snprintf(v, sizeof v, "%ld,%ld", (long)g_tecla[a][0], (long)g_tecla[a][1]); else snprintf(v, sizeof v, "%ld", (long)g_tecla[a][0]);
        opcoes_set(k, v);
        snprintf(k, sizeof k, "botao_%s", k_ac_cod[a]);
        if (g_botao[a][1] >= 0) snprintf(v, sizeof v, "%d,%d", g_botao[a][0], g_botao[a][1]); else snprintf(v, sizeof v, "%d", g_botao[a][0]);
        opcoes_set(k, v);
    }
}
static void bind_tecla(SDL_Keycode k) {           /* a tecla vai para a ação escolhida e sai de qualquer outra */
    int a = g_bind_ac; g_bind_ac = -1; if (a < 0) return;
    for (int i = 0; i < AC_N; i++) for (int j = 0; j < 2; j++) if (i != a && g_tecla[i][j] == k) g_tecla[i][j] = 0;
    for (int i = 0; i < AC_N; i++) if (!g_tecla[i][0] && g_tecla[i][1]) { g_tecla[i][0] = g_tecla[i][1]; g_tecla[i][1] = 0; }
    g_tecla[a][0] = k; g_tecla[a][1] = 0;
    controles_salvar();
}
static void bind_botao(int bt) {
    int a = g_bind_ac; g_bind_ac = -1; g_bind_soltar = 1; if (a < 0) return;
    for (int i = 0; i < AC_N; i++) for (int j = 0; j < 2; j++) if (i != a && g_botao[i][j] == bt) g_botao[i][j] = -1;
    for (int i = 0; i < AC_N; i++) if (g_botao[i][0] < 0 && g_botao[i][1] >= 0) { g_botao[i][0] = g_botao[i][1]; g_botao[i][1] = -1; }
    g_botao[a][0] = bt; g_botao[a][1] = -1;
    controles_salvar();
}
/* espera um botão do controle: primeiro todos soltos (o A que abriu a espera), depois o primeiro apertado */
static void bind_pad_ler(void) {
    int bt = -1, algum = 0;
    for (int i = 0; i < SDL_NumJoysticks() && i < 8; i++) {
        if (!SDL_IsGameController(i)) continue;
        SDL_GameController *gc = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i)); if (!gc) continue;
        for (int k = 0; k < SDL_CONTROLLER_BUTTON_MAX; k++) if (k != SDL_CONTROLLER_BUTTON_GUIDE && SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)k)) { algum = 1; if (bt < 0) bt = k; }
        if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 20000) { algum = 1; if (bt < 0) bt = BT_LT; }
        if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 20000) { algum = 1; if (bt < 0) bt = BT_RT; }
    }
    if (!g_bind_armado) { if (!algum) g_bind_armado = 1; return; }
    if (bt >= 0) bind_botao(bt);
}

/* ---- controle
 * Controles reconhecidos pela SDL (Xbox, PlayStation, Switch e compatíveis) pelo mapeamento dela; outros
 * (DirectInput genérico) por um mapeamento padrão. Botões por posição (padrão do 3DS: o da direita é o A) ou
 * por rótulo (o A do Xbox é o A): CONTROLE_BOTOES=rotulo. O analógico direito move um cursor de toque na tela
 * de baixo (que aparece sozinha enquanto o cursor está em uso), o clique dele (R3) toca e o do esquerdo (L3)
 * mostra/esconde a tela de baixo. */
static float g_cur_x = 160, g_cur_y = 120;          /* cursor de toque do controle (coordenadas da tela de baixo) */
static Uint32 g_cur_until;                          /* cursor visível até este instante (SDL_GetTicks) */
static int g_pad_touch, g_pad_touch_prev;           /* R3 segurado = dedo na tela */
static SDL_Joystick *g_joy[8];                      /* joysticks sem mapeamento da SDL */
static int cursor_active(void) { return g_cur_until && !SDL_TICKS_PASSED(SDL_GetTicks(), g_cur_until); }
static void stick_cursor(int rx, int ry, float dt) {
    const int dz = 8000;
    if (abs(rx) < dz && abs(ry) < dz) return;
    float fx = abs(rx) < dz ? 0 : (rx - (rx > 0 ? dz : -dz)) / (32767.0f - dz);
    float fy = abs(ry) < dz ? 0 : (ry - (ry > 0 ? dz : -dz)) / (32767.0f - dz);
    float sp = 300.0f * dt;                                         /* pontos da tela de baixo por segundo */
    g_cur_x += fx * fabsf(fx) * sp; g_cur_y += fy * fabsf(fy) * sp; /* curva quadrática: precisão perto do centro */
    if (g_cur_x < 0) g_cur_x = 0; if (g_cur_x > 319) g_cur_x = 319;
    if (g_cur_y < 0) g_cur_y = 0; if (g_cur_y > 239) g_cur_y = 239;
    g_cur_until = SDL_GetTicks() + 4000;
}
static int menu_pad_filter(uint32_t *b, int cx, int cy);  /* menu: 1 = o menu ficou com a entrada */
static void update_input(void) {
    static Uint32 last; Uint32 now = SDL_GetTicks();
    float dt = last ? (now - last) / 1000.0f : 0; if (dt > 0.1f) dt = 0.1f; last = now;
    { static int lido; if (!lido) { lido = 1; controles_carregar(); } }
    if (g_bind_ac >= 0 && g_bind_col == 1) { bind_pad_ler(); if (g_bind_ac < 0) g_menu_dirty = 1; }
    uint32_t b = g_kb;
    int cx = (g_right - g_left) * 156, cy = (g_up - g_down) * 156;
    int touch = 0, turbo_pad = 0, patins_pad = 0;
    for (int i = 0; i < SDL_NumJoysticks() && i < 8; i++) {
        if (!SDL_IsGameController(i)) {                              /* DirectInput genérico */
            if (g_joy[i] && SDL_JoystickInstanceID(g_joy[i]) != SDL_JoystickGetDeviceInstanceID(i)) { SDL_JoystickClose(g_joy[i]); g_joy[i] = NULL; }
            if (!g_joy[i]) g_joy[i] = SDL_JoystickOpen(i);   /* (os índices mudam quando um controle sai) */
            SDL_Joystick *j = g_joy[i]; if (!j) continue;
            /* posições comuns: 0 esquerda, 1 baixo, 2 direita, 3 cima, 4/5 ombros, 6/7 gatilhos, 8 select, 9 start, 10 L3, 11 R3 */
            static const int pos[12] = { B_Y, B_B, B_A, B_X, B_L, B_R, B_L, B_R, B_SELECT, B_START, -1, -1 };
            static const int lab[12] = { B_X, B_A, B_B, B_Y, B_L, B_R, B_L, B_R, B_SELECT, B_START, -1, -1 };
            int nb = SDL_JoystickNumButtons(j);
            for (int k = 0; k < nb && k < 12; k++) {
                int bit = g_pad_labels ? lab[k] : pos[k];
                if (bit >= 0 && SDL_JoystickGetButton(j, k)) b |= 1u << bit;
            }
            if (nb > 11 && SDL_JoystickGetButton(j, 11)) touch = 1;
            if (SDL_JoystickNumHats(j) > 0) {
                Uint8 h = SDL_JoystickGetHat(j, 0);
                if (h & SDL_HAT_UP) b |= 1u << B_UP; if (h & SDL_HAT_DOWN) b |= 1u << B_DOWN;
                if (h & SDL_HAT_LEFT) b |= 1u << B_LEFT; if (h & SDL_HAT_RIGHT) b |= 1u << B_RIGHT;
            }
            if (SDL_JoystickNumAxes(j) >= 2) {
                int ax = SDL_JoystickGetAxis(j, 0), ay = SDL_JoystickGetAxis(j, 1);
                if (abs(ax) > 7849 || abs(ay) > 7849) { cx = ax * 156 / 32767; cy = -ay * 156 / 32767; }
            }
            if (SDL_JoystickNumAxes(j) >= 4) stick_cursor(SDL_JoystickGetAxis(j, 2), SDL_JoystickGetAxis(j, 3), dt);
            continue;
        }
        if (g_joy[i]) { SDL_JoystickClose(g_joy[i]); g_joy[i] = NULL; }
        SDL_GameController *gc = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
        if (!gc) gc = SDL_GameControllerOpen(i);
        if (!gc) continue;
        /* (a SDL dá os botões pela posição no padrão Xbox: A embaixo, B à direita, X à esquerda, Y em cima) */
        for (int a = 0; a < AC_N; a++) for (int s2 = 0; s2 < 2; s2++) {
            int bt = g_botao[a][s2], on = 0;
            if (bt == BT_LT) on = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000;
            else if (bt == BT_RT) on = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
            else if (bt >= 0 && bt < SDL_CONTROLLER_BUTTON_MAX) on = SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)bt);
            if (!on) continue;
            if (k_ac_bit[a] >= 0) b |= 1u << k_ac_bit[a]; else if (a == AC_TURBO) turbo_pad = 1; else patins_pad = 1;
        }
        int ax = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX), ay = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        if (abs(ax) > 7849 || abs(ay) > 7849) { cx = ax * 156 / 32767; cy = -ay * 156 / 32767; }
        stick_cursor(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX), SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY), dt);
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) touch = 1;
    }
    for (int i = SDL_NumJoysticks(); i < 8; i++) if (g_joy[i]) { SDL_JoystickClose(g_joy[i]); g_joy[i] = NULL; }
    g_turbo_pad = turbo_pad;
    { static int ant; if (patins_pad && !ant && !g_menu_on && g_bind_ac < 0) patins_alternar(); ant = patins_pad; }
    if (!g_patins && !g_menu_on && (abs(cx) > 50 || abs(cy) > 50)) {   /* sem patins: Circle Pad -> direcional */
        /* 8 direções: perto da diagonal (entre 22,5° e 67,5°) vão os dois botões do direcional juntos */
        int ax = abs(cx), ay = abs(cy);
        if (ax * 5 >= ay * 2) b |= 1u << (cx > 0 ? B_RIGHT : B_LEFT);
        if (ay * 5 >= ax * 2) b |= 1u << (cy > 0 ? B_UP : B_DOWN);
        cx = cy = 0;
    } else if (!g_patins && !g_menu_on) cx = cy = 0;
    { static int teste = -1; if (teste < 0) { const char *e = getenv("ACELERAR_TESTE"); teste = e ? atoi(e) : 0; }   /* depuração */
      { static Uint32 pt; const char *pe = getenv("PATINS_TESTE");                                                  /* depuração */
        if (pe && !pt) pt = now + (Uint32)atoi(pe) * 1000; if (pe && pt > 1 && SDL_TICKS_PASSED(now, pt)) { pt = 1; patins_alternar(); } }
      static Uint32 cap_t; const char *ce = getenv("CAPTURA_TESTE");                                                    /* depuração */
      if (ce && !cap_t) cap_t = now + (Uint32)atoi(ce) * 1000; if (ce && cap_t > 1 && SDL_TICKS_PASSED(now, cap_t)) { cap_t = 1; captura_pedir(); }
      int on = ((g_turbo_tecla || g_turbo_pad) && !g_menu_on) || (teste && SDL_TICKS_PASSED(now, (Uint32)teste * 1000)); int v = on ? g_turbo_mult : 1;   /* (nunca com o menu aberto) */
      if (g_k_vel != v) g_k_vel = v; }
    if (menu_pad_filter(&b, cx, cy)) { b = 0; cx = cy = 0; touch = 0; g_host_touch = 0; }
    g_host_buttons = b; g_host_circle_x = cx; g_host_circle_y = cy;
    /* toque pelo controle (R3): dedo no cursor; não atrapalha o toque com o mouse */
    g_pad_touch = touch;
    if (touch) { g_host_touch = 1; g_host_touch_x = (int)g_cur_x; g_host_touch_y = (int)g_cur_y; g_cur_until = now + 4000; }
    else if (g_pad_touch_prev && !g_mouse) g_host_touch = 0;
    g_pad_touch_prev = touch;
}

static void mouse_touch(int mx, int my, int down) {
    if (!down || g_bot_dst.w <= 0 || g_bot_dst.h <= 0) { g_host_touch = 0; return; }
    int tx = (mx - g_bot_dst.x) * 320 / g_bot_dst.w, ty = (my - g_bot_dst.y) * 240 / g_bot_dst.h;
    if (tx < 0 || tx >= 320 || ty < 0 || ty >= 240) { g_host_touch = 0; return; }
    g_host_touch_x = tx; g_host_touch_y = ty; g_host_touch = 1;
}

/* barra de título: quadros desenhados por segundo e velocidade do jogo (100% = 60 VBlanks/s, o 3DS real) */
extern volatile long g_gsp_top_frames; extern long g_gsp_frames;
/* ---- avisos na janela (controle conectado/desconectado): caixa escura no canto superior esquerdo por ~3,5 s */
#include "fonte_aviso.h"
static char g_toast[320]; static Uint32 g_toast_t0; static int g_toast_dirty;
static SDL_Texture *g_toast_tex; static int g_toast_w, g_toast_h;
static Uint32 g_toast_ms = 3500;
static void toast(const char *msg) {
    g_toast_ms = 3500;
    snprintf(g_toast, sizeof g_toast, "%s", msg);
    g_toast_t0 = SDL_GetTicks(); if (!g_toast_t0) g_toast_t0 = 1;
    g_toast_dirty = 1;
    fprintf(stderr, "[controle] %s\n", msg);
}
static int glyph(unsigned cp) {
    for (int i = 0; i < FONTE_N; i++) if (g_fonte_idx[i].cp == cp) return i;
    return '?' - 32;
}
static unsigned utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p; unsigned c = *s++;
    if (c >= 0xC0 && c < 0xE0 && (*s & 0xC0) == 0x80) { c = ((c & 0x1F) << 6) | (*s++ & 0x3F); }
    else if (c >= 0xE0 && c < 0xF0 && (s[0] & 0xC0) == 0x80 && (s[1] & 0xC0) == 0x80) { c = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F); s += 2; }
    else if (c >= 0x80) c = '?';
    *p = (const char *)s; return c;
}
static SDL_Texture *texto_tex(const char *txt, int *ow, int *oh) {   /* caixa escura com o texto (avisos, FPS) */
    int nl = 1, lw = 0, W = 0;                                     /* (aceita várias linhas, separadas por \n) */
    for (const char *p = txt; *p;) { if (*p == '\n') { nl++; p++; if (lw > W) W = lw; lw = 0; continue; } lw += g_fonte_idx[glyph(utf8_next(&p))].w; }
    if (lw > W) W = lw;
    const int pad = 12, H = nl * FONTE_ALTURA + 2 * pad - 6;
    W += 2 * pad;
    uint32_t *px = calloc((size_t)W * H, 4); if (!px) return NULL;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {          /* fundo escuro com cantos arredondados */
        int dx = x < 6 ? 6 - x : x >= W - 6 ? x - (W - 7) : 0, dy = y < 6 ? 6 - y : y >= H - 6 ? y - (H - 7) : 0;
        if (dx * dx + dy * dy > 36) continue;
        px[(size_t)y * W + x] = 0xD8000000u | 0x181818u;
    }
    int x0 = pad, y0 = 0;
    for (const char *p = txt; *p;) {
        if (*p == '\n') { p++; x0 = pad; y0 += FONTE_ALTURA; continue; }
        int gi = glyph(utf8_next(&p)), gw = g_fonte_idx[gi].w; const unsigned char *g = g_fonte_px + g_fonte_idx[gi].off;
        for (int y = 0; y < FONTE_ALTURA; y++) for (int x = 0; x < gw; x++) {
            int a = g[y * gw + x], ty = y0 + y + pad - 3; if (!a || ty < 0 || ty >= H) continue;
            uint32_t *d = &px[(size_t)ty * W + x0 + x];
            int r = 0x18 + (255 - 0x18) * a / 255;                       /* texto branco sobre o fundo */
            *d = 0xF0000000u | (uint32_t)r << 16 | (uint32_t)r << 8 | (uint32_t)r;
        }
        x0 += gw;
    }
    SDL_Texture *t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, W, H);
    if (t) { SDL_UpdateTexture(t, NULL, px, W * 4); SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); }
    *ow = W; *oh = H;
    free(px);
    return t;
}
static void toast_build(void) {
    if (g_toast_tex) SDL_DestroyTexture(g_toast_tex);
    g_toast_tex = texto_tex(g_toast, &g_toast_w, &g_toast_h);
}
/* FPS na tela (opção "Mostrar FPS") e o aviso de acelerado ("» 2x"), no canto superior direito */
static volatile double g_fps_atual;
/* ícone dos patins (desenhado aqui: um patins genérico de quatro rodas; com um X vermelho quando desligados) */
static SDL_Texture *patins_icone(int x_on) {
    static SDL_Texture *t[2]; if (t[x_on]) return t[x_on];
    const int N = 112, SS = 4; uint32_t *px = calloc((size_t)N * N, 4); if (!px) return NULL;
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        int fundo = 0, bota = 0, roda = 0, cubo = 0, cad = 0, xis = 0;
        for (int j = 0; j < SS; j++) for (int i = 0; i < SS; i++) {
            float u = x + (i + 0.5f) / SS, v = y + (j + 0.5f) / SS;
            float dx = u < 14 ? 14 - u : u > N - 14 ? u - (N - 14) : 0, dy = v < 14 ? 14 - v : v > N - 14 ? v - (N - 14) : 0;
            if (dx * dx + dy * dy <= 14 * 14) fundo++;                                         /* quadrado arredondado */
            int cano = u >= 34 && u <= 58 && v >= 16 && v <= 64;                               /* cano da bota */
            int pe = u >= 34 && u <= 80 && v >= 48 && v <= 74;
            float tx = u - 80, ty = v - 61; int bico = tx >= 0 && tx * tx + ty * ty <= 13 * 13 && v <= 74;
            int sola = u >= 30 && u <= 90 && v >= 74 && v <= 80;
            if (cano || pe || bico || sola) bota++;
            if ((u >= 40 && u <= 54 && (int)((v - 24) / 8) >= 0 && (int)((v - 24) / 8) < 4 && fmodf(v - 24, 8) < 2.5f) && v < 56) cad++;   /* cadarços */
            static const float rx[2] = { 42, 78 };
            for (int k = 0; k < 2; k++) { float a = u - rx[k], c = v - 89; float d2 = a * a + c * c; if (d2 <= 10 * 10) roda++; if (d2 <= 3.5f * 3.5f) cubo++; }
            if (x_on) { float d1 = fabsf((u - 18) - (v - 18)) / 1.4142f, d2 = fabsf((u - 18) + (v - 94) - 0) / 1.4142f;
                        int dentro = u >= 16 && u <= 96 && v >= 16 && v <= 96;
                        if (dentro && (d1 <= 6 || fabsf(u + v - 112) / 1.4142f <= 6)) xis++; (void)d2; }
        }
        float q = SS * SS;
        float a = fundo / q * 0.80f, r = 0.09f, g = 0.09f, bl = 0.10f;              /* fundo escuro */
        #define MIST(cov, R, G, B) { float c_ = (cov) / q; if (c_ > 0) { r = r * (1 - c_) + (R) * c_; g = g * (1 - c_) + (G) * c_; bl = bl * (1 - c_) + (B) * c_; a = a + (1 - a) * c_ * (fundo / q); } }
        MIST(bota, 0.96f, 0.96f, 0.97f)
        MIST(cad, 0.35f, 0.62f, 0.95f)
        MIST(roda, 0.98f, 0.70f, 0.20f)
        MIST(cubo, 0.25f, 0.20f, 0.15f)
        MIST(xis, 0.92f, 0.20f, 0.22f)
        #undef MIST
        px[(size_t)y * N + x] = (uint32_t)(a * 255) << 24 | (uint32_t)(r * 255) << 16 | (uint32_t)(g * 255) << 8 | (uint32_t)(bl * 255);
    }
    t[x_on] = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, N, N);
    if (t[x_on]) { SDL_UpdateTexture(t[x_on], NULL, px, N * 4); SDL_SetTextureBlendMode(t[x_on], SDL_BLENDMODE_BLEND); }
    free(px);
    return t[x_on];
}
/* ícone do usuário (opcional): icones/patins.png na pasta do projeto (o jogar.py passa a pasta em ICONES). Nenhuma
 * imagem vem com o projeto. O fundo liso em volta (a cor do canto) é tirado na hora: um preenchimento a partir
 * das bordas com tolerância e uma borda suave. O X do "desligado" é desenhado por cima. */
unsigned char *stbi_load_from_memory(const unsigned char *buf, int len, int *x, int *y, int *comp, int req);   /* texpack.c */
void stbi_image_free(void *p);
static SDL_Texture *patins_usuario(int x_on) {
    static SDL_Texture *t[2]; static int lido; static uint32_t *img; static int iw, ih;
    if (!lido) {
        lido = 1;
        const char *dir = getenv("ICONES"); if (!dir || !*dir) return NULL;
        char cam[1024]; snprintf(cam, sizeof cam, "%s/patins.png", dir);
        FILE *f = fopen(cam, "rb"); if (!f) return NULL;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        unsigned char *b = n > 0 && n < (16l << 20) ? malloc((size_t)n) : NULL;
        int ok = b && fread(b, 1, (size_t)n, f) == (size_t)n; fclose(f);
        int w = 0, h = 0, ch = 0; unsigned char *p = ok ? stbi_load_from_memory(b, (int)n, &w, &h, &ch, 4) : NULL; free(b);
        if (!p || w < 4 || h < 4 || w > 1024 || h > 1024) { if (p) stbi_image_free(p); fprintf(stderr, "[patins] não consegui ler %s\n", cam); return NULL; }
        img = malloc((size_t)w * h * 4); unsigned char *fora = calloc((size_t)w * h, 1); int *fila = malloc((size_t)w * h * sizeof(int));
        if (!img || !fora || !fila) { free(img); img = NULL; free(fora); free(fila); stbi_image_free(p); return NULL; }
        iw = w; ih = h;
        const unsigned char *c0 = p;                                   /* cor do fundo: canto de cima à esquerda */
        int opaco = 1; for (long i = 0; i < (long)w * h; i++) if (p[i * 4 + 3] < 250) { opaco = 0; break; }
        #define PERTO(i) (abs(p[(i) * 4] - c0[0]) + abs(p[(i) * 4 + 1] - c0[1]) + abs(p[(i) * 4 + 2] - c0[2]) < 90)
        int nf = 0;
        if (opaco) {                                                   /* (PNG já com transparência: usa a dele) */
            for (int x = 0; x < w; x++) { int a = x, b2 = (h - 1) * w + x; if (PERTO(a) && !fora[a]) { fora[a] = 1; fila[nf++] = a; } if (PERTO(b2) && !fora[b2]) { fora[b2] = 1; fila[nf++] = b2; } }
            for (int y = 0; y < h; y++) { int a = y * w, b2 = y * w + w - 1; if (PERTO(a) && !fora[a]) { fora[a] = 1; fila[nf++] = a; } if (PERTO(b2) && !fora[b2]) { fora[b2] = 1; fila[nf++] = b2; } }
            for (int k = 0; k < nf; k++) {
                int i = fila[k], x = i % w, y = i / w, v[4] = { x > 0 ? i - 1 : -1, x < w - 1 ? i + 1 : -1, y > 0 ? i - w : -1, y < h - 1 ? i + w : -1 };
                for (int j = 0; j < 4; j++) if (v[j] >= 0 && !fora[v[j]] && PERTO(v[j])) { fora[v[j]] = 1; fila[nf++] = v[j]; }
            }
        }
        #undef PERTO
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            long i = (long)y * w + x; int a = p[i * 4 + 3];
            if (fora[i]) a = 0;
            else if (opaco) {
                int viz = (x > 0 && fora[i - 1]) + (x < w - 1 && fora[i + 1]) + (y > 0 && fora[i - w]) + (y < h - 1 && fora[i + w]);
                if (viz) a = 255 - viz * 40;
            }
            img[i] = (uint32_t)a << 24 | (uint32_t)p[i * 4] << 16 | (uint32_t)p[i * 4 + 1] << 8 | p[i * 4 + 2];
        }
        free(fora); free(fila); stbi_image_free(p);
        fprintf(stderr, "[patins] ícone: %s (%dx%d)\n", cam, w, h);
    }
    if (!img) return NULL;
    if (t[x_on]) return t[x_on];
    const int N = 160; uint32_t *px = calloc((size_t)N * N, 4); if (!px) return NULL;   /* fundo escuro arredondado + ícone */
    float k = (N - 16.0f) / (iw > ih ? iw : ih); int dw = (int)(iw * k), dh = (int)(ih * k), ox = (N - dw) / 2, oy = (N - dh) / 2;
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        int dx = x < 18 ? 18 - x : x >= N - 18 ? x - (N - 19) : 0, dy = y < 18 ? 18 - y : y >= N - 18 ? y - (N - 19) : 0;
        float r = 0.09f, g = 0.09f, bl = 0.10f, a = dx * dx + dy * dy <= 18 * 18 ? 0.80f : 0;
        if (a > 0 && x >= ox && x < ox + dw && y >= oy && y < oy + dh) {
            float sx = (x - ox + 0.5f) / k - 0.5f, sy = (y - oy + 0.5f) / k - 0.5f;   /* bilinear */
            int ix = (int)sx, iy = (int)sy; if (ix < 0) ix = 0; if (iy < 0) iy = 0; if (ix > iw - 2) ix = iw - 2; if (iy > ih - 2) iy = ih - 2;
            float fx = sx - ix, fy = sy - iy; if (fx < 0) fx = 0; if (fy < 0) fy = 0; if (fx > 1) fx = 1; if (fy > 1) fy = 1;
            float c[4] = { 0 };
            for (int j = 0; j < 4; j++) {
                uint32_t q = img[(long)(iy + (j >> 1)) * iw + ix + (j & 1)];
                float wgt = ((j & 1) ? fx : 1 - fx) * ((j >> 1) ? fy : 1 - fy), qa = (q >> 24) / 255.0f * wgt;
                c[0] += (q >> 16 & 255) / 255.0f * qa; c[1] += (q >> 8 & 255) / 255.0f * qa; c[2] += (q & 255) / 255.0f * qa; c[3] += qa;
            }
            r = r * (1 - c[3]) + c[0]; g = g * (1 - c[3]) + c[1]; bl = bl * (1 - c[3]) + c[2];
        }
        if (x_on && a > 0) {                                           /* X vermelho */
            float u = x + 0.5f, v = y + 0.5f, d = fabsf(u - v) / 1.4142f, e = fabsf(u + v - N) / 1.4142f;
            float cov = d < 5.5f ? (d < 4.5f ? 1 : 5.5f - d) : 0, cov2 = e < 5.5f ? (e < 4.5f ? 1 : 5.5f - e) : 0; if (cov2 > cov) cov = cov2;
            if (u < 22 || u > N - 22 || v < 22 || v > N - 22) cov = 0;
            r = r * (1 - cov) + 0.92f * cov; g = g * (1 - cov) + 0.20f * cov; bl = bl * (1 - cov) + 0.22f * cov;
        }
        px[(size_t)y * N + x] = (uint32_t)(a * 255) << 24 | (uint32_t)(r * 255) << 16 | (uint32_t)(g * 255) << 8 | (uint32_t)(bl * 255);
    }
    t[x_on] = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, N, N);
    if (t[x_on]) { SDL_UpdateTexture(t[x_on], NULL, px, N * 4); SDL_SetTextureBlendMode(t[x_on], SDL_BLENDMODE_BLEND); }
    free(px);
    return t[x_on];
}
static void patins_draw(int ww, int wh) {
    if (!g_patins_t0) return;
    Uint32 age = SDL_GetTicks() - g_patins_t0, dur = 2500;
    if (age > dur) { g_patins_t0 = 0; return; }
    SDL_Texture *t = patins_usuario(!g_patins); if (!t) t = patins_icone(!g_patins);   /* (sem o do usuário: o genérico) */
    if (!t) return;
    float sc = wh / 720.0f; if (sc < 0.75f) sc = 0.75f;
    int m = ww / 60, n = (int)(72 * sc), y = m + ((g_mostrar_fps || g_k_vel > 1) ? (int)(44 * sc) : 0);
    SDL_Rect d = { ww - m - n, y, n, n };
    Uint8 a = age < 150 ? (Uint8)(age * 255 / 150) : age > dur - 600 ? (Uint8)((dur - age) * 255 / 600) : 255;
    SDL_SetTextureAlphaMod(t, a);
    SDL_RenderCopy(g_ren, t, NULL, &d);
}
static void hud_draw(int ww, int wh) {
    char t[64] = "";
    if (g_mostrar_fps) snprintf(t, sizeof t, "%.0f fps", g_fps_atual);
    if (g_k_vel > 1) { size_t L = strlen(t); snprintf(t + L, sizeof t - L, "%s\xc2\xbb %dx", L ? "   " : "", g_k_vel); }
    if (!*t || g_menu_on) return;
    static char ult[64]; static SDL_Texture *tx; static int w, h;
    if (!tx || strcmp(t, ult)) { if (tx) SDL_DestroyTexture(tx); tx = texto_tex(t, &w, &h); snprintf(ult, sizeof ult, "%s", t); }
    if (!tx) return;
    float sc = wh / 720.0f; if (sc < 0.75f) sc = 0.75f;
    int m = ww / 60;
    SDL_Rect d = { ww - m - (int)(w * sc), m, (int)(w * sc), (int)(h * sc) };
    SDL_SetTextureAlphaMod(tx, 225);
    SDL_RenderCopy(g_ren, tx, NULL, &d);
}
static void toast_draw(int ww, int wh) {
    if (!g_toast_t0) return;
    Uint32 age = SDL_GetTicks() - g_toast_t0;
    if (age > g_toast_ms) { g_toast_t0 = 0; g_toast_ms = 3500; return; }
    if (g_toast_dirty) { toast_build(); g_toast_dirty = 0; }
    if (!g_toast_tex) return;
    float sc = wh / 720.0f; if (sc < 0.75f) sc = 0.75f;               /* tamanho proporcional à janela */
    int m = ww / 60;
    SDL_Rect d = { m, m, (int)(g_toast_w * sc), (int)(g_toast_h * sc) };
    Uint8 a = age < 150 ? (Uint8)(age * 255 / 150) : age > g_toast_ms - 600 ? (Uint8)((g_toast_ms - age) * 255 / 600) : 255;   /* entra e sai suave */
    SDL_SetTextureAlphaMod(g_toast_tex, a);
    SDL_RenderCopy(g_ren, g_toast_tex, NULL, &d);
}
/* ---- mapeamento automático: um controle que a SDL não conhece (nem pelo banco gamecontrollerdb.txt) ganha
 * um mapeamento montado na hora a partir do que ele informa (botões, eixos, direcional), no layout mais comum
 * dos controles USB/DirectInput: botões 0-3 = esquerda, baixo, direita, cima; 4/5 ombros; 6/7 gatilhos;
 * 8 select; 9 start; 10/11 cliques dos analógicos; eixos 0/1 analógico esquerdo, 2/3 direito. Registrado na
 * SDL, ele passa a funcionar como um controle reconhecido. Um controles.txt (formato da SDL) corrige casos. */
static int g_automap_n;
static int automap(int index) {
    SDL_Joystick *j = SDL_JoystickOpen(index); if (!j) return 0;
    int nb = SDL_JoystickNumButtons(j), na = SDL_JoystickNumAxes(j), nh = SDL_JoystickNumHats(j);
    char guid[64]; SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(j), guid, sizeof guid);
    const char *nm = SDL_JoystickName(j);
    char name[96]; snprintf(name, sizeof name, "%s", nm ? nm : "Controle");
    for (char *c = name; *c; c++) if (*c == ',') *c = ' ';          /* vírgula separa campos no mapeamento */
    static const struct { const char *k; int b; } bt[] = {
        { "x", 0 }, { "a", 1 }, { "b", 2 }, { "y", 3 }, { "leftshoulder", 4 }, { "rightshoulder", 5 },
        { "lefttrigger", 6 }, { "righttrigger", 7 }, { "back", 8 }, { "start", 9 }, { "leftstick", 10 }, { "rightstick", 11 } };
    char m[1024]; int n = snprintf(m, sizeof m, "%s,%s,", guid, name);
    for (unsigned k = 0; k < sizeof bt / sizeof bt[0]; k++)
        if (bt[k].b < nb) n += snprintf(m + n, sizeof m - n, "%s:b%d,", bt[k].k, bt[k].b);
    if (nh > 0) n += snprintf(m + n, sizeof m - n, "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,");
    else if (nb >= 16) n += snprintf(m + n, sizeof m - n, "dpup:b12,dpdown:b13,dpleft:b14,dpright:b15,");
    if (na >= 2) n += snprintf(m + n, sizeof m - n, "leftx:a0,lefty:a1,");
    if (na >= 4) n += snprintf(m + n, sizeof m - n, "rightx:a2,righty:a3,");
    snprintf(m + n, sizeof m - n, "platform:%s,", SDL_GetPlatform());
    SDL_JoystickClose(j);
    int r = SDL_GameControllerAddMapping(m);
    fprintf(stderr, "[controle] mapeamento automático para \"%s\" (%d botões, %d eixos, %d direcionais): %s\n",
            name, nb, na, nh, r >= 0 ? "ok" : SDL_GetError());
    if (r >= 0) g_automap_n++;
    return r >= 0;
}
/* variável de ambiente em UTF-8 (no Windows o getenv entrega a página de código ANSI: caminhos com acento quebravam) */
static const char *env_utf8(const char *name, char *buf, int n) {
#ifdef _WIN32
    wchar_t wn[64], *wv; MultiByteToWideChar(CP_UTF8, 0, name, -1, wn, 64);
    wv = _wgetenv(wn); if (!wv) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, wv, -1, buf, n, NULL, NULL); buf[n - 1] = 0; return buf;
#else
    const char *v = getenv(name); if (!v) return NULL; snprintf(buf, n, "%s", v); return buf;
#endif
}
static void load_mappings(void) {
    static char b0[1024], b1[1024];
    const char *f[2] = { env_utf8("CONTROLES_DB", b0, sizeof b0), env_utf8("CONTROLES_EXTRA", b1, sizeof b1) };   /* banco da comunidade; ajustes do jogador */
    for (int i = 0; i < 2; i++) {
        if (!f[i] || !*f[i]) continue;
        int n = SDL_GameControllerAddMappingsFromFile(f[i]);
        if (n >= 0) fprintf(stderr, "[controle] %d mapeamento(s) lidos de %s\n", n, f[i]);
    }
}

/* nomes dos controles por id (para o aviso de desconexão, quando a SDL já não sabe o nome) */
static struct { SDL_JoystickID id; char name[96]; } g_padnames[8];
static void pad_added(SDL_JoystickID id, const char *name) {
    int k = 0; for (int i = 0; i < 8; i++) if (g_padnames[i].id == id || !g_padnames[i].name[0]) { k = i; break; }
    g_padnames[k].id = id; snprintf(g_padnames[k].name, sizeof g_padnames[k].name, "%s", name ? name : "?");
    char m[160]; snprintf(m, sizeof m, TR("Controle conectado: %s", "Controller connected: %s", "Mando conectado: %s", "コントローラー接続：%s"), g_padnames[k].name);
    toast(m);
}
static void pad_removed(SDL_JoystickID id) {
    const char *n = "?";
    for (int i = 0; i < 8; i++) if (g_padnames[i].name[0] && g_padnames[i].id == id) { n = g_padnames[i].name; break; }
    char m[160]; snprintf(m, sizeof m, TR("Controle desconectado: %s", "Controller disconnected: %s", "Mando desconectado: %s", "コントローラー切断：%s"), n);
    toast(m);
    for (int i = 0; i < 8; i++) if (g_padnames[i].name[0] && g_padnames[i].id == id) g_padnames[i].name[0] = 0;
    SDL_GameController *gc = SDL_GameControllerFromInstanceID(id); if (gc) SDL_GameControllerClose(gc);
    for (int i = 0; i < 8; i++) if (g_joy[i] && SDL_JoystickInstanceID(g_joy[i]) == id) { SDL_JoystickClose(g_joy[i]); g_joy[i] = NULL; }
    g_pad_touch = 0; if (g_pad_touch_prev && !g_mouse) g_host_touch = 0; g_pad_touch_prev = 0;
}

/* controle conectado/desconectado (eventos da SDL) */
static void pad_event(const SDL_Event *ev) {
    SDL_Event e = *ev;
    switch (e.type) {
    case SDL_CONTROLLERDEVICEADDED: {             /* controle reconhecido pela SDL (Xbox, PlayStation, Switch...) */
        SDL_GameController *gc = SDL_GameControllerOpen(e.cdevice.which);
        if (gc) pad_added(SDL_JoystickGetDeviceInstanceID(e.cdevice.which), SDL_GameControllerName(gc));
        break;
    }
    case SDL_JOYDEVICEADDED:               /* os reconhecidos chegam pelo evento acima */
        if (!SDL_IsGameController(e.jdevice.which)) {
            /* desconhecido: mapeia na hora e passa a tratá-lo como reconhecido; se não der, fica no modo genérico */
            int ok = automap(e.jdevice.which) && SDL_IsGameController(e.jdevice.which);
            char nm[128]; const char *n0 = SDL_JoystickNameForIndex(e.jdevice.which);
            snprintf(nm, sizeof nm, "%s%s", n0 ? n0 : "?", ok ? TR(" (mapeado automaticamente)", " (auto-mapped)", " (asignado automáticamente)", "（自動割り当て）") : "");
            pad_added(SDL_JoystickGetDeviceInstanceID(e.jdevice.which), nm);
        }
        break;
    case SDL_JOYDEVICEREMOVED: pad_removed(e.jdevice.which); break;   /* (vale para os dois tipos) */
    }
}

/* "Pokémon X" ou "Pokémon Y", conforme a ROM (JOGO_VERSAO, do jogar.py; só a letra, porque o getenv do
 * Windows não entrega acentos em UTF-8) */
static const char *game_name(void) {
    static char n[32];
    if (!n[0]) { const char *v = getenv("JOGO_VERSAO"); snprintf(n, sizeof n, "Pok\xc3\xa9mon %s", v && (!strcmp(v, "X") || !strcmp(v, "Y")) ? v : "XY"); }
    return n;
}
static void update_title(void) {
    static Uint32 t0; static long f0, v0;
    Uint32 now = SDL_GetTicks();
    if (!t0) { t0 = now; f0 = g_gsp_top_frames; v0 = g_gsp_frames; return; }
    if (now - t0 < 1000) return;
    double dt = (now - t0) / 1000.0;
    double fps = (g_gsp_top_frames - f0) / dt, speed = (g_gsp_frames - v0) / dt / 60.0 * 100.0;
    { extern volatile long g_q60_shown; extern int g_fps60; static long s0;   /* 60 fps: imagens mostradas */
      if (g_fps60 > 0 && g_q60_shown - s0 > fps * dt) fps = (g_q60_shown - s0) / dt;
      s0 = g_q60_shown; }
    char t[384];
    extern volatile int g_tex_filter, g_tex_filter_req;          /* (no idioma do programa) */
    static const char *const fn[4][4] = { { "original", "suave", "anisotrópico", "bicúbico" }, { "original", "smooth", "anisotropic", "bicubic" },
                                          { "original", "suave", "anisotrópico", "bicúbico" }, { "オリジナル", "スムーズ", "異方性", "バイキュービック" } };
    int fi = g_tex_filter_req >= 0 ? g_tex_filter_req & 3 : g_tex_filter >= 0 ? g_tex_filter & 3 : 0;
    const char *jn = game_name();
    snprintf(t, sizeof t, TR("%s \xe2\x80\x94 %.1f fps | velocidade %.0f%% | %s | %dx | textura: %s | F11: tela cheia | Tab: tela de baixo | F7: filtro de textura | F8: resolução | F9: layout | F12: captura de tela",
                             "%s \xe2\x80\x94 %.1f fps | speed %.0f%% | %s | %dx | texture: %s | F11: full screen | Tab: bottom screen | F7: texture filter | F8: resolution | F9: layout | F12: screenshot",
                             "%s \xe2\x80\x94 %.1f fps | velocidad %.0f%% | %s | %dx | textura: %s | F11: pantalla completa | Tab: pantalla inferior | F7: filtro de textura | F8: resolución | F9: disposición | F12: captura de pantalla",
                             "%s \xe2\x80\x94 %.1f fps | 速度 %.0f%% | %s | %dx | テクスチャ：%s | F11：全画面 | Tab：下画面 | F7：テクスチャフィルター | F8：解像度 | F9：レイアウト | F12：スクリーンショット"),
             jn, fps, speed, g_render_name, g_res, fn[ui_lang()][fi]);
    SDL_SetWindowTitle(g_win, t);
    g_fps_atual = fps;
    if (getenv("ACELERAR_TESTE")) fprintf(stderr, "[teste] fps %.1f velocidade %.0f%% (x%d)\n", fps, speed, g_k_vel);
    t0 = now; f0 = g_gsp_top_frames; v0 = g_gsp_frames;
}

/* OpenGL do renderer da GPU: contexto 3.3 numa janela escondida, criado aqui e usado pelas threads
 * do jogo (cada lista de comandos torna o contexto atual na thread que a executa e depois o solta) */
static SDL_Window *g_glwin;
static SDL_GLContext g_glctx;
static void gl_create(void) {
    const char *r = getenv("RENDER");
    if (r && !strcmp(r, "software")) return;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    g_glwin = SDL_CreateWindow("gpu", 0, 0, 16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (g_glwin) g_glctx = SDL_GL_CreateContext(g_glwin);
    if (!g_glctx) {
        fprintf(stderr, "[gl] não consegui criar o contexto OpenGL 3.3 (%s): usando o renderer em software\n", SDL_GetError());
        if (g_glwin) SDL_DestroyWindow(g_glwin);
        g_glwin = NULL;
    } else SDL_GL_MakeCurrent(g_glwin, NULL);
    SDL_GL_ResetAttributes();
}
int gpu_gl_begin(void) { return g_glctx && SDL_GL_MakeCurrent(g_glwin, g_glctx) == 0; }
void gpu_gl_end(void) { if (g_glwin) SDL_GL_MakeCurrent(g_glwin, NULL); }
void *gpu_gl_proc(const char *name) { return SDL_GL_GetProcAddress(name); }

/* contexto do jogo pelos módulos carregados: 2 = mochila aberta, 0 = resto */
int ldr_loaded(const char *name);
static int game_context(void) { return ldr_loaded("DllBag") ? 2 : 0; }

static volatile int g_volume_pct = 100;   /* volume do som (0..100; opção do menu, opcoes.txt "volume") */
static int g_frame_seen;                 /* já chegou alguma imagem do jogo */
/* modo de tela: 0 = janela, 1 = janela sem bordas (ocupa a tela toda, troca de programa na hora),
 * 2 = tela cheia exclusiva (na resolução da área de trabalho). MODO_TELA = janela | sem_bordas | tela_cheia */
static int g_modo_tela = -1, g_modo_cheio = 1;     /* g_modo_cheio: o modo de tela cheia que o F11 usa */
static void modo_tela_aplicar(int m);
#include "menu_jogo.h"
static void modo_tela_aplicar(int m) {
    if (!g_win || m < 0 || m > 2) return;
    g_modo_tela = m; if (m) g_modo_cheio = m;
    if (m == 0) {
        SDL_SetWindowFullscreen(g_win, 0);
        SDL_SetWindowBordered(g_win, SDL_TRUE);
        SDL_DisplayMode dm; int h = CH * 2, w;           /* tamanho de janela padrão, centralizada */
        if (SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(g_win), &dm) == 0 && dm.h * 85 / 100 < h) h = dm.h * 85 / 100;
        if (h < CH) h = CH;
        if (g_layout == 1) { h = h * 3 / 4; w = h * 16 / 9; } else w = h * CW / CH;
        SDL_SetWindowSize(g_win, w, h);
        SDL_SetWindowPosition(g_win, SDL_WINDOWPOS_CENTERED_DISPLAY(SDL_GetWindowDisplayIndex(g_win)), SDL_WINDOWPOS_CENTERED_DISPLAY(SDL_GetWindowDisplayIndex(g_win)));
    } else if (m == 1) {
        SDL_SetWindowFullscreen(g_win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(g_win), &dm) == 0) SDL_SetWindowDisplayMode(g_win, &dm);
        if (SDL_SetWindowFullscreen(g_win, SDL_WINDOW_FULLSCREEN) != 0) SDL_SetWindowFullscreen(g_win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
    fprintf(stderr, "[janela] modo: %s\n", m == 0 ? "janela" : m == 1 ? "janela sem bordas" : "tela cheia");
}
static uint32_t g_menu_hold;             /* botões que estavam apertados ao fechar o menu: o jogo só os vê depois de soltos */
static int menu_pad_filter(uint32_t *b, int cx, int cy) {
    if (!g_menu_on) { g_menu_hold &= *b; *b &= ~g_menu_hold; return 0; }
    if (g_bind_ac >= 0 || g_bind_soltar) {          /* configurando um botão: o menu não anda até soltar tudo */
        if (g_bind_ac < 0 && !*b && !cx && !cy) g_bind_soltar = 0;
        menu_pad(0, 0, 0); g_menu_hold = *b; return 1;
    }
    menu_pad(*b, cx, cy);
    g_menu_hold = *b | (g_kb & ((1u << B_START) | (1u << B_A)));
    return 1;
}

/* a janela e os eventos ficam numa thread própria (o jogo roda nas threads do kernel emulado) */
static int window_thread(void *arg) {
    SDL_sem *ready = arg;
    /* Controles: iniciados AQUI, na thread que lê os eventos. No Windows a SDL detecta os controles (RawInput,
     * avisos de dispositivo novo) por mensagens que vão para a fila da thread que iniciou o subsistema; iniciado
     * na thread principal (que roda o jogo e nunca lê mensagens), nenhum controle aparecia. A thread própria da
     * SDL para essas mensagens (JOYSTICK_THREAD) deixa isso ainda mais garantido. */
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");   /* controle funciona mesmo com outra janela em foco */
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) fprintf(stderr, "[controle] a SDL não iniciou os controles: %s\n", SDL_GetError());
    else {
        load_mappings();
        int n = SDL_NumJoysticks();
        fprintf(stderr, "[controle] SDL %d.%d.%d: %d dispositivo(s) de jogo visto(s)\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL, n);
        for (int i = 0; i < n; i++)
            fprintf(stderr, "[controle]   %d: %s (%s)\n", i, SDL_JoystickNameForIndex(i) ? SDL_JoystickNameForIndex(i) : "?",
                    SDL_IsGameController(i) ? "reconhecido" : "genérico");
    }
    gl_create();
    prefs_load();
    menu_init();
    SDL_DisplayMode dm; int h = CH * 2, w;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.h * 85 / 100 < h) h = dm.h * 85 / 100;
    if (h < CH) h = CH;
    if (g_layout == 1) { h = h * 3 / 4; w = h * 16 / 9; } else w = h * CW / CH;   /* tela de cima: janela 16:9 */
    g_win = SDL_CreateWindow(game_name(),                 /* o título com fps vem logo depois */
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                             SDL_WINDOW_RESIZABLE);
    { const char *e = getenv("MODO_TELA");               /* escolha do menu (opcoes.txt); sem ela: com o menu, sem bordas */
      int m = e && !strcmp(e, "janela") ? 0 : e && !strcmp(e, "tela_cheia") ? 2 : e && !strcmp(e, "sem_bordas") ? 1 : g_menu_mode ? 1 : 0;
      if (m) modo_tela_aplicar(m); else g_modo_tela = 0; }
    g_ren = g_win ? SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (g_win && !g_ren) g_ren = SDL_CreateRenderer(g_win, -1, 0);
    if (g_ren) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, CW, CH);   /* sem alfa: a translucidez vem do alpha mod */

    }
    SDL_SemPost(ready);
    if (!g_tex) return 0;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: exit(0);
            case SDL_KEYDOWN: case SDL_KEYUP: {
                int down = e.type == SDL_KEYDOWN;
                SDL_Keycode k = e.key.keysym.sym;
                if (g_menu_on && g_bind_ac >= 0) {           /* página Controles: esperando a tecla */
                    if (down && !e.key.repeat) {
                        if (k == SDLK_ESCAPE) { g_bind_ac = -1; g_bind_soltar = 0; }
                        else if (g_bind_col == 0 && !(k >= SDLK_F1 && k <= SDLK_F12) && k != SDLK_TAB && k != SDLK_UP && k != SDLK_DOWN && k != SDLK_LEFT && k != SDLK_RIGHT) bind_tecla(k);
                        g_menu_dirty = 1;
                    }
                    break;
                }
                if (g_menu_on && k != SDLK_F11) {            /* menu aberto: as teclas são dele */
                    if (down) {
                        int dv = k == SDLK_UP ? -1 : k == SDLK_DOWN ? 1 : 0, dh = k == SDLK_LEFT ? -1 : k == SDLK_RIGHT ? 1 : 0;
                        int ok = k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE || k == SDLK_a;
                        int back = (k == SDLK_ESCAPE || k == SDLK_BACKSPACE || k == SDLK_s) && !e.key.repeat;
                        if (dv || dh || (ok && !e.key.repeat) || back) menu_input(dv, dh, ok && !e.key.repeat, back);
                    }
                    break;
                }
                if (k == SDLK_ESCAPE && down && !e.key.repeat && g_menu_mode) { menu_open(); break; }
                if (k == SDLK_F11 && down && !e.key.repeat) {   /* alterna entre janela e o modo de tela cheia escolhido */
                    int m = g_modo_tela ? 0 : g_modo_cheio;
                    modo_tela_aplicar(m);
                    opcoes_set("modo_tela", m == 0 ? "janela" : m == 1 ? "sem_bordas" : "tela_cheia");
                }
                else if (k == SDLK_F9 && down && !e.key.repeat) { g_layout = !g_layout; prefs_save(); }
                else if (k == SDLK_F10 && down && !e.key.repeat) { g_stretch = !g_stretch; prefs_save(); }
                else if (k == SDLK_F8 && down && !e.key.repeat) { int i = 0; while (i < 5 && k_res_ok[i] != g_res) i++; g_res = k_res_ok[(i + 1) % 6]; g_res_req = g_res; prefs_save(); }
                else if (k == SDLK_F7 && down && !e.key.repeat) {   /* filtro de textura: original, suave, anisotrópico, bicúbico */
                    extern volatile int g_tex_filter, g_tex_filter_req;
                    g_tex_filter_req = ((g_tex_filter < 0 ? 0 : g_tex_filter) + 1) % 4; }
                else if (k == SDLK_F6 && down && !e.key.repeat && getenv("FPS60_TESTE")) {   /* 30 / 60 fps (em pausa: só teste) */
                    extern volatile int g_fps60_req; extern int g_fps60;
                    g_fps60_req = g_fps60 > 0 ? 0 : 1;
                    toast(g_fps60_req ? "60 fps" : "30 fps"); }
                else if (k == SDLK_TAB && down && !e.key.repeat) { g_show_bot = !g_show_bot; prefs_save(); }
                else if (k == SDLK_LEFT) g_left = down; else if (k == SDLK_RIGHT) g_right = down;
                else if (k == SDLK_UP) g_up = down; else if (k == SDLK_DOWN) g_down = down;
                else if (k == SDLK_F12 && down && !e.key.repeat) captura_pedir();
                else { int bit = key_bit(k);
                       if (bit == 100 + AC_TURBO) g_turbo_tecla = down;
                       else if (bit == 100 + AC_PATINS) { if (down && !e.key.repeat) patins_alternar(); }
                       else if (bit >= 0) { if (down) g_kb |= 1u << bit; else g_kb &= ~(1u << bit); } }
                break;
            }
            case SDL_CONTROLLERDEVICEADDED: case SDL_JOYDEVICEADDED: case SDL_JOYDEVICEREMOVED: pad_event(&e); break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) { g_kb = 0; g_left = g_right = g_up = g_down = 0; g_turbo_tecla = 0; }
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (g_menu_on) { if (e.button.button == SDL_BUTTON_LEFT) { int a, b2, c2, d2; SDL_GetRendererOutputSize(g_ren, &a, &b2); SDL_GetWindowSize(g_win, &c2, &d2);
                                 menu_mouse(e.button.x * a / (c2 ? c2 : 1), e.button.y * b2 / (d2 ? d2 : 1), 1); } break; }
                if (e.button.button == SDL_BUTTON_LEFT) { g_mouse = 1; mouse_touch(e.button.x, e.button.y, 1); } break;
            case SDL_MOUSEMOTION:
                if (g_menu_on) { int a, b2, c2, d2; SDL_GetRendererOutputSize(g_ren, &a, &b2); SDL_GetWindowSize(g_win, &c2, &d2);
                                 menu_mouse(e.motion.x * a / (c2 ? c2 : 1), e.motion.y * b2 / (d2 ? d2 : 1), 0); break; }
                g_hover_bot = g_bot_dst.w && e.motion.x >= g_bot_dst.x && e.motion.x < g_bot_dst.x + g_bot_dst.w &&
                              e.motion.y >= g_bot_dst.y && e.motion.y < g_bot_dst.y + g_bot_dst.h;
                if (g_mouse) mouse_touch(e.motion.x, e.motion.y, 1);
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE && !g_menu_on) { menu_open(); break; }   /* Home/Guide: menu */
                if (g_menu_on) break;
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK) { g_show_bot = !g_show_bot; prefs_save(); }   /* L3 */
                break;
            case SDL_JOYBUTTONDOWN:                       /* controle genérico: botão 10 (L3) = tela de baixo */
                if (g_menu_on) break;
                if (e.jbutton.button == 10 && !SDL_GameControllerFromInstanceID(e.jbutton.which)) { g_show_bot = !g_show_bot; prefs_save(); }
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT && g_menu_on) { int a, b2, c2, d2; SDL_GetRendererOutputSize(g_ren, &a, &b2); SDL_GetWindowSize(g_win, &c2, &d2);
                                 menu_mouse_up(e.button.x * a / (c2 ? c2 : 1)); }
                if (e.button.button == SDL_BUTTON_LEFT) { g_mouse = 0; mouse_touch(0, 0, 0); } break;
            }
        }
        update_input();
        if (g_frame_ready) {
            SDL_LockMutex(g_lock);
            SDL_UpdateTexture(g_tex, NULL, g_canvas, CW * 4);
            if (g_hd_ready && g_hd_dirty && g_hd_sc >= 1) {
                int w = 400 * g_hd_sc, h = 240 * g_hd_sc;
                if (!g_htex || g_htex_w != w || g_htex_h != h) {
                    if (g_htex) SDL_DestroyTexture(g_htex);
                    g_htex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, w, h);
                    g_htex_w = w; g_htex_h = h;
                }
                if (g_htex) SDL_UpdateTexture(g_htex, NULL, g_hd_canvas, w * 4);
                g_hd_dirty = 0;
            }
            if (g_hb_ready && g_hb_dirty && g_hb_sc > 1) {
                int w = 320 * g_hb_sc, h = 240 * g_hb_sc;
                if (!g_hbtex || g_hbtex_w != w || g_hbtex_h != h) {
                    if (g_hbtex) SDL_DestroyTexture(g_hbtex);
                    g_hbtex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, w, h);
                    g_hbtex_w = w; g_hbtex_h = h;
                }
                if (g_hbtex) SDL_UpdateTexture(g_hbtex, NULL, g_hb_canvas, w * 4);
                g_hb_dirty = 0;
            }
            g_frame_ready = 0; g_frame_seen = 1;
            SDL_UnlockMutex(g_lock);
        }
        update_title();
        int ww, wh; SDL_GetRendererOutputSize(g_ren, &ww, &wh);
        int wx, wy; SDL_GetWindowSize(g_win, &wx, &wy);    /* coordenadas do mouse estão em pontos da janela */
        float px = (float)wx / (ww ? ww : 1), py = (float)wy / (wh ? wh : 1);   /* pixels -> pontos (telas HiDPI) */
        SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
        SDL_RenderClear(g_ren);
        SDL_Rect top_src = { 0, 0, 400, 240 }, bot_src = { 40, 240, 320, 240 }, bot;
        extern volatile int g_hd_need_top, g_hd_need_bot;   /* pica_gl.c: escala em que cada tela aparece na janela */
        int need_top = 0, need_bot = 1;
        if (g_layout == 1) {
            /* tela de cima ocupando a janela (esticada para a proporção da janela, ou 5:3 com faixas pretas) */
            SDL_Rect t = { 0, 0, ww, wh };
            if (!g_stretch) { t.w = ww; t.h = ww * 240 / 400; if (t.h > wh) { t.h = wh; t.w = wh * 400 / 240; } t.x = (ww - t.w) / 2; t.y = (wh - t.h) / 2; }
            SDL_SetTextureBlendMode(g_tex, SDL_BLENDMODE_NONE); SDL_SetTextureAlphaMod(g_tex, 255);
            if (g_htex && g_hd_ready && g_hd_sc >= 1) SDL_RenderCopy(g_ren, g_htex, NULL, &t);
            else SDL_RenderCopy(g_ren, g_tex, &top_src, &t);
            need_top = (t.w + 399) / 400; if ((t.h + 239) / 240 > need_top) need_top = (t.h + 239) / 240;
            int ctx = game_context();
            if (g_show_bot || ctx == 2 || cursor_active()) {
                /* tela de baixo no canto inferior direito, translúcida (opaca com o mouse em cima); a mochila
                 * abre aqui sozinha, maior e quase opaca */
                int big = ctx == 2;
                int bw = ww * (big ? 40 : 30) / 100, bh = bw * 240 / 320, m = ww / 100;
                bot = (SDL_Rect){ ww - bw - m, wh - bh - m, bw, bh };
                SDL_SetTextureBlendMode(g_tex, SDL_BLENDMODE_BLEND);
                SDL_SetTextureAlphaMod(g_tex, (g_hover_bot || g_mouse || big || cursor_active()) ? 235 : 110);
                if (g_hbtex && g_hb_ready && g_hb_sc > 1) {        /* em escala, se houver */
                    SDL_SetTextureBlendMode(g_hbtex, SDL_BLENDMODE_BLEND);
                    SDL_SetTextureAlphaMod(g_hbtex, (g_hover_bot || g_mouse || big || cursor_active()) ? 235 : 110);
                    SDL_RenderCopy(g_ren, g_hbtex, NULL, &bot);
                } else SDL_RenderCopy(g_ren, g_tex, &bot_src, &bot);
                SDL_SetTextureAlphaMod(g_tex, 255); SDL_SetTextureBlendMode(g_tex, SDL_BLENDMODE_NONE);
                g_bot_dst = (SDL_Rect){ (int)(bot.x * px), (int)(bot.y * py), (int)(bot.w * px), (int)(bot.h * py) };
                need_bot = (bot.w + 319) / 320;
            } else g_bot_dst.w = 0;
        } else {
            int dw = ww, dh = ww * CH / CW;
            if (dh > wh) { dh = wh; dw = wh * CW / CH; }
            SDL_Rect r = { (ww - dw) / 2, (wh - dh) / 2, dw, dh };
            SDL_SetTextureBlendMode(g_tex, SDL_BLENDMODE_NONE); SDL_SetTextureAlphaMod(g_tex, 255);
            SDL_RenderCopy(g_ren, g_tex, NULL, &r);
            if (g_htex && g_hd_ready && g_hd_sc >= 1) { SDL_Rect tr = { r.x, r.y, r.w, r.h / 2 }; SDL_RenderCopy(g_ren, g_htex, NULL, &tr); }
            bot = (SDL_Rect){ r.x + r.w * 40 / 400, r.y + r.h / 2, r.w * 320 / 400, r.h / 2 };
            need_top = (r.w + 399) / 400; need_bot = (bot.w + 319) / 320;
            if (g_hbtex && g_hb_ready && g_hb_sc > 1) { SDL_SetTextureBlendMode(g_hbtex, SDL_BLENDMODE_NONE); SDL_SetTextureAlphaMod(g_hbtex, 255); SDL_RenderCopy(g_ren, g_hbtex, NULL, &bot); }
            g_bot_dst = (SDL_Rect){ (int)(bot.x * px), (int)(bot.y * py), (int)(bot.w * px), (int)(bot.h * py) };
            g_dst = (SDL_Rect){ (int)(r.x * px), (int)(r.y * py), (int)(r.w * px), (int)(r.h * py) };
        }
        if (cursor_active() && g_bot_dst.w > 0) {        /* cursor de toque do controle sobre a tela de baixo */
            int bx = (int)(g_bot_dst.x / px), by = (int)(g_bot_dst.y / py), bw = (int)(g_bot_dst.w / px), bh = (int)(g_bot_dst.h / py);
            int x = bx + (int)(g_cur_x * bw / 320), y = by + (int)(g_cur_y * bh / 240);
            int r = bw / 40 + 4;
            SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
            for (int k = 0; k < 2; k++) {                 /* contorno escuro e cruz clara */
                int o = k ? 0 : 1;
                SDL_SetRenderDrawColor(g_ren, k ? 255 : 0, k ? 255 : 0, k ? 255 : 0, 230);
                SDL_RenderDrawLine(g_ren, x - r - o, y, x + r + o, y); SDL_RenderDrawLine(g_ren, x, y - r - o, x, y + r + o);
                SDL_RenderDrawLine(g_ren, x - r - o, y + 1, x + r + o, y + 1); SDL_RenderDrawLine(g_ren, x + 1, y - r - o, x + 1, y + r + o);
            }
            if (g_pad_touch) { SDL_SetRenderDrawColor(g_ren, 255, 210, 60, 200); SDL_Rect d = { x - r / 2, y - r / 2, r + 1, r + 1 }; SDL_RenderFillRect(g_ren, &d); }
            SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_NONE);
        }
        { extern volatile int g_captura_pedida;            /* F12: lê as telas em escala cheia até a captura sair */
          if (g_captura_pedida) { need_top = 99; need_bot = 99; } }
        g_hd_need_top = need_top; g_hd_need_bot = need_bot;
        menu_draw(ww, wh);
        hud_draw(ww, wh);
        if (!g_menu_on) patins_draw(ww, wh);
        toast_draw(ww, wh);
        { static int np; const char *jp = getenv("JANELA_PRINT");   /* depuração: salva o que a janela mostra */
          int cada = getenv("JANELA_PRINT_CADA") ? atoi(getenv("JANELA_PRINT_CADA")) : 0;
          if (jp && (cada > 0 ? ++np % cada == 0 : ++np == 120)) {
              uint32_t *px32 = malloc((size_t)ww * wh * 4);
              if (px32 && SDL_RenderReadPixels(g_ren, NULL, SDL_PIXELFORMAT_RGB888, px32, ww * 4) == 0) {
                  char nm[600]; if (cada > 0) snprintf(nm, sizeof nm, "%s_%05d.ppm", jp, np); else snprintf(nm, sizeof nm, "%s", jp);
                  FILE *o = fopen(nm, "wb");
                  if (o) { fprintf(o, "P6\n%d %d\n255\n", ww, wh);
                           for (long i = 0; i < (long)ww * wh; i++) { uint8_t c[3] = { px32[i] >> 16, px32[i] >> 8, px32[i] }; fwrite(c, 1, 3, o); }
                           fclose(o); }
              }
              free(px32);
          } }
        SDL_RenderPresent(g_ren);                          /* com vsync, ~60 vezes por segundo */
        SDL_Delay(1);
    }
    return 0;
}

#define RING 32768                     /* ~1 s */
static int16_t g_ring[RING][2];
static int g_ring_r, g_ring_n;
static double g_frac;                   /* posição fracionária entre ring[r] e ring[r+1] */
static float g_last[2];
static int g_primed;
static float g_gain, g_vol = -1;                    /* sobe de 0 a 1 depois de uma falta (sem estalo na volta) */
extern volatile long g_audio_under, g_audio_drop; extern volatile int g_audio_fill_ms, g_audio_target_ms;
static int g_target_ms = 50, g_quiet;
static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    g_audio_fill_ms = g_ring_n * 1000 / 32728;
    int16_t *out = (int16_t *)stream; int n = len / 4;
    if (g_vol < 0) { g_vol = g_volume_pct / 100.0f; g_vol *= g_vol; }
    /* alvo da fila adaptável: começa baixo (menos atraso entre a ação e o som); cada falta sobe 15 ms
     * (até 150 ms) e, depois de 20 s sem faltas, desce 5 ms (até 45 ms) */
    g_quiet += n;
    if (g_quiet > 32728 * 20 && g_target_ms > 45) { g_target_ms -= 5; g_quiet = 0; }
    g_audio_target_ms = g_target_ms;
    const int target = 32728 * g_target_ms / 1000;
    if (!g_primed && g_ring_n >= target) g_primed = 1;
    double ratio = 1.0 + (double)(g_ring_n - target) / target * 0.01;
    if (ratio < 0.99) ratio = 0.99;
    if (ratio > 1.01) ratio = 1.01;
    for (int i = 0; i < n; i++) {
        if (g_primed && g_ring_n >= 2) {
            const int16_t *a = g_ring[g_ring_r], *b = g_ring[(g_ring_r + 1) % RING];
            for (int c = 0; c < 2; c++) g_last[c] = (float)(a[c] + (b[c] - a[c]) * g_frac);
            g_frac += ratio;
            while (g_frac >= 1.0 && g_ring_n >= 2) { g_frac -= 1.0; g_ring_r = (g_ring_r + 1) % RING; g_ring_n--; }
            if (g_gain < 1.0f) { g_gain += 1.0f / 256; if (g_gain > 1.0f) g_gain = 1.0f; }
        } else {
            if (g_primed) { g_audio_under++; g_quiet = 0; if (g_target_ms < 150) g_target_ms += 15; }
            g_primed = 0;                       /* faltou: espera juntar de novo antes de voltar */
            g_gain *= 0.99f;
        }
        { float alvo = g_volume_pct / 100.0f; alvo *= alvo;                     /* (curva quadrática: soa linear ao ouvido) */
          g_vol += (alvo - g_vol) * (1.0f / 512); }                            /* muda suave, sem estalo */
        out[2 * i] = (int16_t)(g_last[0] * g_gain * g_vol); out[2 * i + 1] = (int16_t)(g_last[1] * g_gain * g_vol);
    }
}

extern void (*g_ui_aviso)(const char *msg);
static void ui_aviso_sdl(const char *msg) { toast(msg); g_toast_ms = strchr(msg, '\n') ? 10000 : 6000; }   /* avisos do jogo ficam mais tempo */
void display_init(void) {
    g_ui_aviso = ui_aviso_sdl;
    { const char *v = getenv("VOLUME"); if (v && *v) { int x = atoi(v); g_volume_pct = x < 0 ? 0 : x > 100 ? 100 : x; } }
    if (getenv("AVISO_TESTE")) ui_aviso_sdl(getenv("AVISO_TESTE"));   /* depuração */
    SDL_SetMainReady();
    if (getenv("SEM_JANELA")) { SDL_setenv("SDL_VIDEODRIVER", "dummy", 1); }
    /* controles da Nintendo pela posição dos botões (como os outros), não pelo rótulo */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    /* o subsistema de controle é iniciado na thread da janela (window_thread), onde os eventos são lidos */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        /* sem vídeo (servidor): tenta só o som */
        if (SDL_Init(SDL_INIT_AUDIO) != 0) return;
    }
    SDL_AudioSpec want, have; memset(&want, 0, sizeof want);
    want.freq = 32728; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512;
    want.callback = audio_cb;
    g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_audio) SDL_PauseAudioDevice(g_audio, 0);
    if (getenv("SEM_JANELA") || !SDL_WasInit(SDL_INIT_VIDEO)) return;
    g_lock = SDL_CreateMutex();
    SDL_sem *ready = SDL_CreateSemaphore(0);
    SDL_CreateThread(window_thread, "janela", ready);
    SDL_SemWaitTimeout(ready, 5000);
    g_display_on = g_tex != NULL;
}

void display_frame(const uint32_t *top, const uint32_t *bottom) {
    SDL_LockMutex(g_lock);
    memcpy(g_canvas, top, sizeof(uint32_t) * 400 * 240);
    for (int y = 0; y < 240; y++) {
        uint32_t *row = g_canvas + (240 + y) * CW;
        memset(row, 0, 40 * 4); memset(row + 360, 0, 40 * 4);
        memcpy(row + 40, bottom + y * 320, 320 * 4);
    }
    g_frame_ready = 1;
    SDL_UnlockMutex(g_lock);
}

/* tela de baixo em resolução interna maior (320*sc x 240*sc); NULL = mostra a imagem normal */
void display_frame_hd_bot(const uint32_t *bot, int sc) {
    if (!g_lock) return;
    SDL_LockMutex(g_lock);
    if (!bot || sc <= 1) g_hb_ready = 0;
    else {
        int n = 320 * sc * 240 * sc;
        if (g_hb_cap < n) { g_hb_cap = n; g_hb_canvas = realloc(g_hb_canvas, (size_t)n * 4); }
        memcpy(g_hb_canvas, bot, (size_t)n * 4); g_hb_sc = sc; g_hb_ready = 1; g_hb_dirty = 1;
    }
    SDL_UnlockMutex(g_lock);
}
/* tela de cima em resolução interna maior (400*sc x 240*sc); NULL = mostra a imagem normal */
void display_frame_hd(const uint32_t *top, int sc) {
    if (!g_lock) return;
    SDL_LockMutex(g_lock);
    if (!top || sc < 1) g_hd_ready = 0;       /* (1x só chega aqui no modo 60 fps) */
    else {
        int n = 400 * sc * 240 * sc;
        if (g_hd_cap < n) { g_hd_cap = n; g_hd_canvas = realloc(g_hd_canvas, (size_t)n * 4); }
        memcpy(g_hd_canvas, top, (size_t)n * 4); g_hd_sc = sc; g_hd_ready = 1; g_hd_dirty = 1; g_frame_ready = 1;
    }
    SDL_UnlockMutex(g_lock);
}

/* som: quadros estéreo de 16 bits a 32728 Hz, num anel lido pela callback da SDL.
 * O jogo produz som no ritmo do relógio dele, que oscila um pouco em relação ao da placa de som;
 * em vez de deixar a fila esvaziar (estalos) ou descartar pedaços (saltos), a callback ajusta de
 * leve a velocidade de leitura (até ±1%, inaudível) para manter o alvo guardado (45–150 ms, adaptável). Se mesmo assim
 * faltar som (carregamento, travada), o volume desce suave até zero e volta suave depois. */
void audio_push(const int16_t *samples, int frames) {
    if (!g_audio) return;
    if (g_k_vel > 1) return;                    /* acelerando: sem som (a fila esvazia e o volume desce suave) */
    SDL_LockAudioDevice(g_audio);
    for (int i = 0; i < frames; i++) {
        if (g_ring_n >= RING) { g_ring_r = (g_ring_r + 1) % RING; g_ring_n--; }   /* cheio: descarta o mais antigo */
        int w = (g_ring_r + g_ring_n) % RING;
        g_ring[w][0] = samples[2 * i]; g_ring[w][1] = samples[2 * i + 1];
        g_ring_n++;
    }
    if (g_ring_n > 32728 / 4) {                /* mais de 250 ms guardados: pula para o alvo (som atrasado) */
        int drop = g_ring_n - 32728 * g_target_ms / 1000; g_audio_drop++;
        g_ring_r = (g_ring_r + drop) % RING; g_ring_n -= drop;
    }
    SDL_UnlockAudioDevice(g_audio);
}

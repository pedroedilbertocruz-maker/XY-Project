/* Menu do jogo (incluído por display_sdl.c, que tem a janela, as texturas e a entrada).
 *
 * Abre em tela cheia antes do jogo (MENU=1, o jogar.py liga): ao fundo, a tela de baixo do jogo ao vivo,
 * cobrindo a janela, escurecida à esquerda e embaixo; no alto à esquerda o nome do jogo; à esquerda o menu
 * (Opções, Sair); no canto inferior direito o cartucho da versão girando devagar (é o botão de jogar) e, bem
 * discreta, a ROM com "trocar". Cores pela versão (X azul, Y vermelho). Enquanto o menu está aberto o jogo
 * não recebe entrada. Esc (ou o botão Home/Guide do controle) reabre o menu durante o jogo.
 *
 * Opções: renderizador e idioma do jogo valem ao reiniciar (o menu oferece "Reiniciar agora": o jogo sai com
 * o código 77 e o jogar.py abre de novo); resolução, filtro, telas e botões valem na hora. As escolhas ficam no
 * arquivo de opções do projeto (OPCOES, chave=valor), lido pelo jogar.py e pelo lançador.
 * Trocar a ROM: o jogo sai com o código 78 e o lançador abre a escolha da ROM. */
#include "menu_ui.h"

enum { MS_JOGAR, MS_OPCOES, MS_ROM, MS_SAIR, MS_MODS, MS_RESET, MS_CONTROLES };
enum { OP_VOLUME, OP_RENDER, OP_RES, OP_MODO, OP_FILTRO, OP_TEXTURAS, OP_TELAS, OP_FPS, OP_BOTOES, OP_IDIOMA_UI, OP_IDIOMA, OP_RADAR, OP_XP, OP_NUZLOCKE, OP_REINICIAR, OP_VOLTAR, OP_N };
static int g_menu_mode, g_menu_on, g_menu_started, g_menu_page, g_menu_sel, g_menu_dirty = 1, g_menu_confirm = -1;
static float g_menu_fade;                 /* 0..1 */
static int g_menu_target;                 /* 1 = aparecendo, 0 = sumindo */
static SDL_Texture *g_ui_tex, *g_cart_tex, *g_bg_tex, *g_arte_tex, *g_arte_sombra, *g_brilho_tex; static int g_ui_w, g_ui_h, g_cart_S;
static MuCanvas g_ui; static MuCart g_cart; static MuTema g_tema; static int g_cart_ok;
static Uint32 g_cart_t0; static float g_cart_yaw, g_cart_boost;
static float g_vol_x0, g_vol_x1; static int g_vol_drag;   /* barra de volume (posição na tela) e arraste */
static SDL_Rect g_hit[80]; static int g_hit_id[80], g_hit_n;
static int g_op_val[OP_N], g_op_ini_render, g_op_ini_idioma;
/* mods (pasta mods/ do projeto; o jogar.py passa a lista em MODS_LISTA e os ligados em MODS_ATIVOS) */
#define MOD_MAX 24
enum { MR_ABRIR = 1000, MR_REINICIAR, MR_VOLTAR };
static char g_mod_nome[MOD_MAX][96]; static int g_mod_on[MOD_MAX], g_mod_ini[MOD_MAX], g_mod_n;
static void mods_init(void) {
    const char *l = getenv("MODS_LISTA"), *a = getenv("MODS_ATIVOS");
    for (const char *p = l; p && *p && g_mod_n < MOD_MAX;) {
        const char *e = strchr(p, '|'); size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && n < sizeof g_mod_nome[0]) {
            memcpy(g_mod_nome[g_mod_n], p, n); g_mod_nome[g_mod_n][n] = 0;
            int on = 0;                                    /* ligado se está na lista de ativos */
            for (const char *q = a; q && *q;) { const char *f = strchr(q, '|'); size_t m = f ? (size_t)(f - q) : strlen(q);
                if (m == n && !memcmp(q, p, n)) on = 1; q = f ? f + 1 : q + m; }
            g_mod_on[g_mod_n] = g_mod_ini[g_mod_n] = on; g_mod_n++;
        }
        p = e ? e + 1 : p + n;
    }
}
static int mods_mudou(void) { for (int i = 0; i < g_mod_n; i++) if (g_mod_on[i] != g_mod_ini[i]) return 1; return 0; }
static int mods_ativos_ini(void) { int n = 0; for (int i = 0; i < g_mod_n; i++) n += g_mod_ini[i]; return n; }
static void opcoes_set(const char *k, const char *v);
static void mods_salvar(void) {
    char b[MOD_MAX * 100] = ""; size_t L = 0;
    for (int i = 0; i < g_mod_n; i++) if (g_mod_on[i]) L += (size_t)snprintf(b + L, sizeof b - L, "%s%s", L ? "|" : "", g_mod_nome[i]);
    opcoes_set("mods", b);
}
static void mods_abrir_pasta(void) {
    const char *d = getenv("MODS_DIR"); if (!d) return;
    char u[1200] = "file:///"; size_t L = strlen(u);
    for (const char *p = d; *p && L < sizeof u - 1; p++) u[L++] = *p == '\\' ? '/' : *p;
    u[L] = 0;
    if (u[8] == '/') memmove(u + 7, u + 8, strlen(u + 8) + 1);   /* (Linux: file:///home/...) */
    SDL_OpenURL(u);
}
static int mod_rows(int *rows);
static char g_versao = 0;
static float g_cart_cx, g_cart_cy, g_cart_sz;
/* thread do cartucho: desenha (em software) o pedido mais recente; a janela só envia a imagem pronta */
static SDL_mutex *g_cart_mx; static SDL_cond *g_cart_cv_go;
static float g_cart_req_yaw, g_cart_req_pitch, g_cart_req_glow; static int g_cart_req_R;
static uint32_t *g_cart_front; static int g_cart_front_S, g_cart_new;
static int cart_thread(void *arg) {
    (void)arg; uint32_t *back = NULL; int backS = 0;
    for (;;) {
        SDL_LockMutex(g_cart_mx);
        while (!g_menu_on || !g_cart_req_R) SDL_CondWaitTimeout(g_cart_cv_go, g_cart_mx, 100);
        float yaw = g_cart_req_yaw, pitch = g_cart_req_pitch, glow = g_cart_req_glow; int R = g_cart_req_R;
        SDL_UnlockMutex(g_cart_mx);
        Uint32 t0 = SDL_GetTicks();
        int S = (int)(R * 1.3f) + 4;
        if (S != backS) { free(back); back = malloc((size_t)S * S * 4); backS = S; }
        if (!back) { SDL_Delay(100); continue; }
        memset(back, 0, (size_t)S * S * 4);
        MuCanvas cv = { back, S, S };
        mu_cart_draw(&cv, &g_cart, S / 2.0f, S / 2.0f, (float)R, yaw, pitch, glow);
        SDL_LockMutex(g_cart_mx);
        uint32_t *t = g_cart_front; g_cart_front = back; back = t;
        int ts = g_cart_front_S; g_cart_front_S = S; backS = t ? ts : 0; g_cart_new = 1;
        SDL_UnlockMutex(g_cart_mx);
        Uint32 el = SDL_GetTicks() - t0; if (el < 33) SDL_Delay(33 - el);          /* até ~30 por segundo */
    }
    return 0;
}
static const char *const k_idiomas[] = { "en", "es", "fr", "de", "it", "ja", "ko" };
static const char *const k_idioma_nome[4][7] = {   /* idiomas do jogo, escritos no idioma do programa */
    { "Inglês", "Espanhol", "Francês", "Alemão", "Italiano", "Japonês", "Coreano" },
    { "English", "Spanish", "French", "German", "Italian", "Japanese", "Korean" },
    { "Inglés", "Español", "Francés", "Alemán", "Italiano", "Japonés", "Coreano" },
    { "英語", "スペイン語", "フランス語", "ドイツ語", "イタリア語", "日本語", "韓国語" } };
extern volatile int g_tex_filter, g_tex_filter_req;
const char *idioma_jogo_save(void);
extern int g_lang_select;                 /* input.c: tela de idioma do jogo aberta */

/* ---- arquivo de opções (chave=valor, UTF-8) */
static void opcoes_set(const char *k, const char *v) {
    const char *path = getenv("OPCOES"); if (!path || !*path) return;
    char *buf = NULL; size_t n = 0;
    FILE *f = fopen(path, "rb");
    if (f) { fseek(f, 0, SEEK_END); long L = ftell(f); fseek(f, 0, SEEK_SET); if (L > 0 && L < 1 << 20) { buf = malloc((size_t)L + 1); n = fread(buf, 1, (size_t)L, f); buf[n] = 0; } fclose(f); }
    f = fopen(path, "wb"); if (!f) { free(buf); return; }
    size_t kl = strlen(k); int done = 0;
    for (char *l = buf; l && *l;) {
        char *e = strchr(l, '\n'); size_t len = e ? (size_t)(e - l) : strlen(l);
        if (len > kl && !strncmp(l, k, kl) && l[kl] == '=') { if (!done) fprintf(f, "%s=%s\n", k, v); done = 1; }
        else if (len) { fwrite(l, 1, len, f); if (l[len - 1] != '\r') fputc('\n', f); else fputc('\n', f); }
        l = e ? e + 1 : l + len;
    }
    if (!done) fprintf(f, "%s=%s\n", k, v);
    fclose(f); free(buf);
}

static int idioma_idx_de(const char *c) { for (int i = 0; i < 7; i++) if (c && !strcmp(c, k_idiomas[i])) return i; return -1; }
static Uint8 *g_snd_buf; static Uint32 g_snd_len; static SDL_AudioSpec g_snd_spec; static SDL_AudioDeviceID g_snd_dev;
/* coluna da esquerda: Opções, Mods, (Reiniciar jogo, só depois de começar), Sair */
static int menu_col_esq(int *v) {
    int n = 0; v[n++] = MS_OPCOES; v[n++] = MS_CONTROLES; v[n++] = MS_MODS;
    if (g_menu_started) v[n++] = MS_RESET;
    v[n++] = MS_SAIR; return n;
}
static void menu_load_vals(void);
static void menu_init(void) {
    const char *m = getenv("MENU"); g_menu_mode = m && *m && strcmp(m, "0");
    if (!g_menu_mode) return;
    g_menu_on = 1; g_menu_target = 1; g_menu_fade = 1; g_menu_sel = MS_JOGAR; g_turbo_tecla = 0;
    if (getenv("RESET_DIRETO")) { g_menu_on = 0; g_menu_target = 0; g_menu_fade = 0; g_menu_started = 1; }   /* veio do "reiniciar jogo" */
    const char *v = getenv("JOGO_VERSAO"); g_versao = v && (v[0] == 'X' || v[0] == 'Y') && !v[1] ? v[0] : 0;
    g_tema = mu_tema(g_versao);
    const char *dir = getenv("CARTUCHOS");
    if (dir && g_versao) {
        char a[1024], b[1024];
        snprintf(a, sizeof a, "%s/cartucho.malha", dir); snprintf(b, sizeof b, "%s/cartucho_%c.bmp", dir, g_versao == 'X' ? 'x' : 'y');
        g_cart_ok = mu_cart_load(&g_cart, a, b);
        char w[1024]; snprintf(w, sizeof w, "%s/jogar.wav", dir);
        if (!SDL_LoadWAV(w, &g_snd_spec, &g_snd_buf, &g_snd_len)) g_snd_buf = NULL;
        else {                                       /* abre o dispositivo já (abrir na hora do clique atrasa o som) */
            g_snd_spec.samples = 512; g_snd_spec.callback = NULL;
            g_snd_dev = SDL_OpenAudioDevice(NULL, 0, &g_snd_spec, NULL, 0);
            if (g_snd_dev) SDL_PauseAudioDevice(g_snd_dev, 0);
        }
        if (!g_cart_ok) fprintf(stderr, "[menu] cartucho não carregado (%s, %s)\n", a, b);
    }
    const char *r = getenv("RENDER"); g_op_val[OP_RENDER] = g_op_ini_render = r && !strcmp(r, "vulkan");
    const char *ij = getenv("IDIOMA_JOGO"); int ii = idioma_idx_de(ij && *ij ? ij : idioma_jogo_save());
    g_op_val[OP_IDIOMA] = g_op_ini_idioma = ii < 0 ? 0 : ii;
    g_cart_t0 = SDL_GetTicks();
    mods_init();
    mu_font_init();
    { const char *t = getenv("MENU_TESTE"); if (t) { menu_load_vals(); g_menu_page = atoi(t); const char *c = strchr(t, ','); if (c) g_menu_sel = atoi(c + 1); } }   /* depuração */
    if (g_cart_ok) { g_cart_mx = SDL_CreateMutex(); g_cart_cv_go = SDL_CreateCond(); SDL_CreateThread(cart_thread, "cartucho", NULL); }
}
extern volatile int g_opt_radar, g_opt_nuzlocke, g_opt_xp;
int texpack_init(void); extern volatile int g_texpack_on, g_texpack_gen;   /* texturas personalizadas (texpack.c) */
static const int k_xp_mult[5] = { 1, 2, 4, 6, 8 };
static void menu_load_vals(void) {
    g_op_val[OP_RADAR] = g_opt_radar != 0;
    g_op_val[OP_NUZLOCKE] = g_opt_nuzlocke != 0;
    g_op_val[OP_XP] = 0; for (int i = 0; i < 5; i++) if (k_xp_mult[i] == g_opt_xp) g_op_val[OP_XP] = i;
    g_op_val[OP_RES] = 0; for (int i = 0; i < 6; i++) if (k_res_ok[i] == g_res) g_op_val[OP_RES] = i;
    g_op_val[OP_FILTRO] = g_tex_filter < 0 ? (getenv("FILTRO_TEXTURA") ? atoi(getenv("FILTRO_TEXTURA")) & 3 : 0) : g_tex_filter;
    g_op_val[OP_TELAS] = g_layout ? 0 : 1;
    texpack_init(); g_op_val[OP_TEXTURAS] = g_texpack_on != 0;
    g_op_val[OP_MODO] = g_modo_tela < 0 ? 0 : g_modo_tela;
    g_op_val[OP_BOTOES] = g_pad_labels > 0;
    g_op_val[OP_FPS] = g_mostrar_fps != 0;
    g_op_val[OP_IDIOMA_UI] = ui_lang();
    g_op_val[OP_VOLUME] = g_volume_pct;
}
static int menu_pending(void) { return g_op_val[OP_RENDER] != g_op_ini_render || g_op_val[OP_IDIOMA] != g_op_ini_idioma || mods_mudou(); }
static void menu_open(void) {
    if (!g_menu_mode) return;
    g_menu_on = 1; g_menu_target = 1; g_menu_page = 0; g_menu_sel = MS_JOGAR; g_menu_confirm = -1; g_menu_dirty = 1;
    g_kb = 0; g_left = g_right = g_up = g_down = 0;
}
/* som de "jogar" (cartuchos/jogar.wav, opcional): toca num dispositivo de som próprio, por cima do jogo */
static void som_jogar(void) {
    if (!g_snd_buf) return;
    if (!g_snd_dev) g_snd_dev = SDL_OpenAudioDevice(NULL, 0, &g_snd_spec, NULL, 0);
    if (!g_snd_dev) return;
    SDL_ClearQueuedAudio(g_snd_dev); SDL_QueueAudio(g_snd_dev, g_snd_buf, g_snd_len); SDL_PauseAudioDevice(g_snd_dev, 0);
}
static void menu_play(int som) { g_menu_target = 0; g_menu_started = 1; g_cart_boost = 1; g_menu_dirty = 1; if (som) som_jogar(); }

static int op_count(int i) {
    static const int n[OP_N] = { 101, 2, 6, 3, 4, 2, 2, 2, 2, 4, 7, 2, 5, 2, 1, 1 };   /* (volume: 0 a 100%) */
    if (i == OP_TEXTURAS) return texpack_init() > 0 ? 2 : 1;   /* (sem pacote na pasta texturas: nada a ligar) */
    return n[i];
}
static const char *op_label(int i) {
    switch (i) {
    case OP_RENDER: return TR("Renderizador", "Renderer", "Renderizador", "レンダラー");
    case OP_RES: return TR("Resolução interna", "Internal resolution", "Resolución interna", "内部解像度");
    case OP_FILTRO: return TR("Filtro de textura", "Texture filter", "Filtro de textura", "テクスチャフィルター");
    case OP_MODO: return TR("Modo de tela", "Display mode", "Modo de pantalla", "表示モード");
    case OP_TEXTURAS: return TR("Texturas personalizadas", "Custom textures", "Texturas personalizadas", "カスタムテクスチャ");
    case OP_TELAS: return TR("Telas", "Screens", "Pantallas", "画面");
    case OP_BOTOES: return TR("Botões do controle", "Controller buttons", "Botones del mando", "コントローラーのボタン");
    case OP_FPS: return TR("Mostrar FPS", "Show FPS", "Mostrar FPS", "FPSを表示");
    case OP_VOLUME: return TR("Volume do som", "Sound volume", "Volumen del sonido", "音量");
    case OP_IDIOMA_UI: return TR("Idioma do programa", "Program language", "Idioma del programa", "プログラムの言語");
    case OP_IDIOMA: return TR("Idioma do jogo", "Game language", "Idioma del juego", "ゲームの言語");
    case OP_RADAR: return TR("Poké Radar sempre carregado", "Poké Radar always charged", "Poké Radar siempre cargado", "ポケトレを常にフル充電");
    case OP_XP: return TR("XP ganho em batalha", "Battle XP gain", "EXP ganada en combate", "バトルで得る経験値");
    case OP_NUZLOCKE: return "Nuzlocke Mode";
    case OP_REINICIAR: return TR("Reiniciar agora", "Restart now", "Reiniciar ahora", "今すぐ再起動");
    default: return TR("Voltar", "Back", "Volver", "戻る");
    }
}
static const char *op_value(int i, char *buf, int n) {
    int v = g_op_val[i];
    switch (i) {
    case OP_RENDER: return v ? "Vulkan" : "OpenGL";
    case OP_RES: { int r = k_res_ok[v % 6]; const char *nome = r == 6 ? "2K" : r == 9 ? "4K" : NULL;
        if (nome) snprintf(buf, n, "%s  ·  %d×%d", nome, 400 * r, 240 * r); else snprintf(buf, n, "%dx  ·  %d×%d", r, 400 * r, 240 * r); return buf; }
    case OP_FILTRO: { static const char *const t[4][4] = { { "Original", "Suave", "Anisotrópico 16x", "Bicúbico" }, { "Original", "Smooth", "Anisotropic 16x", "Bicubic" },
                                                          { "Original", "Suave", "Anisotrópico 16x", "Bicúbico" }, { "オリジナル", "スムーズ", "異方性 16x", "バイキュービック" } }; return t[ui_lang()][v & 3]; }
    case OP_MODO: return v == 0 ? TR("Janela", "Windowed", "Ventana", "ウィンドウ") : v == 1 ? TR("Janela sem bordas", "Borderless window", "Ventana sin bordes", "ボーダーレスウィンドウ") : TR("Tela cheia", "Fullscreen", "Pantalla completa", "フルスクリーン");
    case OP_TEXTURAS: { int nt = texpack_init(); if (!nt) return TR("Nenhum pacote", "No pack", "Ningún paquete", "パックなし");
        snprintf(buf, n, v ? TR("Ligadas  ·  %d", "On  ·  %d", "Activadas  ·  %d", "オン  ·  %d") : TR("Desligadas  ·  %d", "Off  ·  %d", "Desactivadas  ·  %d", "オフ  ·  %d"), nt); return buf; }
    case OP_TELAS: return v ? TR("Duas telas", "Both screens", "Ambas pantallas", "両画面") : TR("Tela de cima", "Top screen", "Pantalla superior", "上画面");
    case OP_BOTOES: return v ? TR("Por rótulo", "By label", "Por etiqueta", "表記どおり") : TR("Por posição", "By position", "Por posición", "位置どおり");
    case OP_VOLUME: if (!v) return TR("Mudo", "Muted", "Silencio", "ミュート");
        snprintf(buf, n, "%d%%", v); return buf;
    case OP_IDIOMA_UI: return k_ui_lang_nome[v & 3];
    case OP_IDIOMA: return k_idioma_nome[ui_lang()][v % 7];
    case OP_FPS: return v ? TR("Ligado", "On", "Activado", "オン") : TR("Desligado", "Off", "Desactivado", "オフ");
    case OP_RADAR: return v ? TR("Ligado", "On", "Activado", "オン") : TR("Desligado", "Off", "Desactivado", "オフ");
    case OP_XP: if (!v) return TR("Normal", "Normal", "Normal", "通常"); snprintf(buf, n, "%dx", k_xp_mult[v % 5]); return buf;
    case OP_NUZLOCKE: return v ? TR("Ligado", "On", "Activado", "オン") : TR("Desligado", "Off", "Desactivado", "オフ");
    }
    return "";
}
static void op_change(int i, int d) {
    int n = op_count(i); if (n < 2) return;
    int v = ((g_op_val[i] + d) % n + n) % n;
    if (i == OP_VOLUME) { v = (g_op_val[i] + 2) / 5 * 5 + d * 5; if (v < 0) v = 0; if (v > 100) v = 100; }   /* (volume: de 5 em 5, para nas pontas) */
    g_op_val[i] = v;
    char num[8]; snprintf(num, sizeof num, "%d", v);
    switch (i) {
    case OP_RENDER: opcoes_set("render", v ? "vulkan" : "opengl"); break;
    case OP_RES: g_res = k_res_ok[v % 6]; g_res_req = g_res; snprintf(num, sizeof num, "%d", g_res); opcoes_set("resolucao", num); prefs_save(); break;
    case OP_FILTRO: g_tex_filter_req = v; opcoes_set("filtro", num); break;
    case OP_MODO: modo_tela_aplicar(v); opcoes_set("modo_tela", v == 0 ? "janela" : v == 1 ? "sem_bordas" : "tela_cheia"); break;   /* vale na hora */
    case OP_TEXTURAS: g_texpack_on = v; g_texpack_gen++; opcoes_set("texturas", v ? "1" : "0"); break;   /* vale na hora */
    case OP_TELAS: g_layout = v ? 0 : 1; prefs_save(); break;
    case OP_BOTOES:                                  /* (página Controles) refaz os botões do controle no esquema escolhido */
        g_pad_labels = v; opcoes_set("botoes", v ? "rotulo" : "posicao"); botoes_padrao(); controles_salvar(); break;
    case OP_FPS: g_mostrar_fps = v; opcoes_set("mostrar_fps", v ? "1" : "0"); break;   /* vale na hora */
    case OP_VOLUME: g_volume_pct = v; snprintf(num, sizeof num, "%d", v); opcoes_set("volume", num); break;   /* vale na hora */
    case OP_IDIOMA_UI: g_ui_lang = v; opcoes_set("idioma", k_ui_lang_cod[v]); break;   /* vale na hora (menu, avisos, título) */
    case OP_IDIOMA: opcoes_set("idioma_jogo", k_idiomas[v]); break;
    case OP_RADAR: g_opt_radar = v; opcoes_set("radar_cheio", v ? "1" : "0"); break;   /* vale na hora */
    case OP_XP: g_opt_xp = k_xp_mult[v]; snprintf(num, sizeof num, "%d", k_xp_mult[v]); opcoes_set("xp", num); break;   /* vale na hora */
    case OP_NUZLOCKE:
        if (v) { g_op_val[i] = 0; g_menu_page = 3; g_menu_sel = 1; break; }       /* ligar: antes, o aviso e a confirmação */
        g_opt_nuzlocke = 0; opcoes_set("nuzlocke", "0"); break;
    }
    g_menu_dirty = 1;
}
/* linhas visíveis da página de opções */
static int op_rows(int *rows) {
    int n = 0;
    for (int i = 0; i < OP_N; i++) { if ((i == OP_REINICIAR && !menu_pending()) || i == OP_BOTOES) continue; rows[n++] = i; }   /* (botões: página Controles) */
    return n;
}

static int mod_rows(int *rows) {
    int n = 0;
    for (int i = 0; i < g_mod_n; i++) rows[n++] = i;
    rows[n++] = MR_ABRIR;
    if (menu_pending()) rows[n++] = MR_REINICIAR;
    rows[n++] = MR_VOLTAR;
    return n;
}

/* ---- página Controles: uma linha por ação (tecla e botão do controle), o esquema dos botões, a velocidade ao
 * acelerar, restaurar o padrão e voltar. Enter (ou clique) numa coluna espera a tecla/botão novo; Esc cancela. */
static const int k_ctl_ordem[AC_N] = { AC_UP, AC_DOWN, AC_LEFT, AC_RIGHT, AC_A, AC_B, AC_X, AC_Y, AC_L, AC_R, AC_START, AC_SELECT, AC_TURBO, AC_PATINS };
enum { CR_BOTOES = 50, CR_VEL, CR_PADRAO, CR_VOLTAR };
static int g_ctl_col;                              /* coluna escolhida: 0 teclado, 1 controle */
static int ctl_rows(int *rows) {
    int n = 0;
    for (int i = 0; i < AC_N; i++) rows[n++] = i;
    rows[n++] = CR_BOTOES; rows[n++] = CR_VEL; rows[n++] = CR_PADRAO; rows[n++] = CR_VOLTAR;
    return n;
}
static const char *ctl_nome(int a) {
    switch (a) {
    case AC_UP: return TR("Direcional: cima", "D-Pad: up", "Cruceta: arriba", "十字ボタン：上");
    case AC_DOWN: return TR("Direcional: baixo", "D-Pad: down", "Cruceta: abajo", "十字ボタン：下");
    case AC_LEFT: return TR("Direcional: esquerda", "D-Pad: left", "Cruceta: izquierda", "十字ボタン：左");
    case AC_RIGHT: return TR("Direcional: direita", "D-Pad: right", "Cruceta: derecha", "十字ボタン：右");
    case AC_A: return "A"; case AC_B: return "B"; case AC_X: return "X"; case AC_Y: return "Y";
    case AC_L: return "L"; case AC_R: return "R"; case AC_START: return "Start"; case AC_SELECT: return "Select";
    case AC_PATINS: return TR("Patins (liga/desliga)", "Roller Skates (on/off)", "Patines (activar/desactivar)", "ローラースケート（オン／オフ）");
    default: return TR("Acelerar (segurar)", "Fast-forward (hold)", "Acelerar (mantener)", "早送り（長押し）");
    }
}
static const char *botao_nome(int b) {
    static const char *const n[] = { "A", "B", "X", "Y", "Back", "Guide", "Start", "L3", "R3", "LB", "RB", "↑", "↓", "←", "→", "Misc", "P1", "P2", "P3", "P4", "Touchpad" };
    if (b == BT_LT) return "LT"; if (b == BT_RT) return "RT";
    return b >= 0 && b < (int)(sizeof n / sizeof *n) ? n[b] : "?";
}
static void ctl_texto(int a, int col, char *o, int n) {
    if (g_bind_ac == a && g_bind_col == col) { snprintf(o, n, "…"); return; }
    o[0] = 0;
    for (int j = 0; j < 2; j++) {
        char t[48] = "";
        if (col == 0 && g_tecla[a][j]) snprintf(t, sizeof t, "%.20s", SDL_GetKeyName(g_tecla[a][j]));
        if (col == 1 && g_botao[a][j] >= 0) snprintf(t, sizeof t, "%s", botao_nome(g_botao[a][j]));
        if (!*t) continue;
        size_t L = strlen(o); snprintf(o + L, n - L, "%s%s", L ? " / " : "", t);
    }
    if (!o[0]) snprintf(o, n, "—");
}
static void ctl_padrao(void) {
    teclas_padrao(); botoes_padrao(); g_turbo_mult = 2;
    for (int a = 0; a < AC_N; a++) { char k[40]; snprintf(k, sizeof k, "tecla_%s", k_ac_cod[a]); opcoes_set(k, ""); snprintf(k, sizeof k, "botao_%s", k_ac_cod[a]); opcoes_set(k, ""); }
    opcoes_set("acelerar", "2");
}
static void ctl_vel(int d) {
    int v = g_turbo_mult + d; if (v < 2) v = 4; if (v > 4) v = 2;
    g_turbo_mult = v; char num[8]; snprintf(num, sizeof num, "%d", v); opcoes_set("acelerar", num);
}
static void ctl_input(int dv, int dh, int ok, int back) {
    int rows[AC_N + 4], n = ctl_rows(rows);
    if (g_menu_sel >= n) g_menu_sel = n - 1;
    if (dv) g_menu_sel = (g_menu_sel + dv + n) % n;
    int it = rows[g_menu_sel];
    if (back || (ok && it == CR_VOLTAR)) { g_menu_page = 0; g_menu_sel = MS_CONTROLES; return; }
    if (it < AC_N) {
        if (dh) g_ctl_col = !g_ctl_col;
        if (ok) { g_bind_ac = k_ctl_ordem[it]; g_bind_col = g_ctl_col; g_bind_armado = 0; }
    } else if (it == CR_BOTOES && (dh || ok)) { op_change(OP_BOTOES, dh ? dh : 1); }
    else if (it == CR_VEL && (dh || ok)) ctl_vel(dh ? dh : 1);
    else if (it == CR_PADRAO && ok) ctl_padrao();
}

/* ---- entrada do menu: dir = -1/+1 vertical, h = -1/+1 horizontal, ok, back */
static void menu_input(int dv, int dh, int ok, int back) {
    if (!g_menu_on || g_menu_target == 0) return;
    g_menu_dirty = 1;
    if (g_menu_page == 0) {
        /* coluna da esquerda: Opções, Mods, Sair; da direita: Jogar (cartucho), ROM */
        int colL[6], nl = menu_col_esq(colL); static const int colR[2] = { MS_JOGAR, MS_ROM };
        int c = 0, r = 0;
        for (int i = 0; i < nl; i++) if (colL[i] == g_menu_sel) { c = 0; r = i; }
        for (int i = 0; i < 2; i++) if (colR[i] == g_menu_sel) { c = 1; r = i; }
        int nc = c ? 2 : nl;
        if (dv) { r = (r + dv + nc) % nc; g_menu_confirm = -1; }
        if (dh) { c = !c; nc = c ? 2 : nl; if (r >= nc) r = nc - 1; g_menu_confirm = -1; }
        g_menu_sel = c ? colR[r] : colL[r];
        if (back && g_menu_started) { menu_play(0); return; }
        if (!ok) return;
        switch (g_menu_sel) {
        case MS_JOGAR: menu_play(1); break;
        case MS_OPCOES: menu_load_vals(); g_menu_page = 1; g_menu_sel = 0; break;
        case MS_MODS: g_menu_page = 2; g_menu_sel = 0; break;
        case MS_CONTROLES: menu_load_vals(); g_menu_page = 4; g_menu_sel = 0; g_ctl_col = 0; break;
        case MS_ROM:
            if (g_menu_started && g_menu_confirm != MS_ROM) { g_menu_confirm = MS_ROM; break; }
            fprintf(stderr, "[menu] trocar a ROM\n"); exit(78);
        case MS_SAIR:
            if (g_menu_started && g_menu_confirm != MS_SAIR) { g_menu_confirm = MS_SAIR; break; }
            exit(0);
        case MS_RESET:                                      /* volta à tela de continuar (o jogar.py abre de novo) */
            if (g_menu_confirm != MS_RESET) { g_menu_confirm = MS_RESET; break; }
            fprintf(stderr, "[menu] reiniciar o jogo (volta à tela de continuar)\n"); exit(79);
        }
        return;
    }
    if (g_menu_page == 4) { ctl_input(dv, dh, ok, back); return; }
    if (g_menu_page == 3) {                              /* confirmação do Nuzlocke: 0 = ativar, 1 = cancelar */
        if (dv || dh) g_menu_sel = !g_menu_sel;
        if (back || (ok && g_menu_sel == 1)) { g_menu_page = 1; g_menu_sel = OP_NUZLOCKE; return; }
        if (ok && g_menu_sel == 0) {
            g_opt_nuzlocke = 1; g_op_val[OP_NUZLOCKE] = 1; opcoes_set("nuzlocke", "1");
            fprintf(stderr, "[menu] Nuzlocke Mode ligado\n");
            g_menu_page = 1; g_menu_sel = OP_NUZLOCKE;
        }
        return;
    }
    if (g_menu_page == 2) {                              /* mods */
        int rows[MOD_MAX + 3], n = mod_rows(rows);
        if (g_menu_sel >= n) g_menu_sel = n - 1;
        if (dv) { g_menu_sel = (g_menu_sel + dv + n) % n; g_menu_confirm = -1; }
        int it = rows[g_menu_sel];
        if (back || (ok && it == MR_VOLTAR)) { g_menu_page = 0; g_menu_sel = MS_MODS; return; }
        if (it < MR_ABRIR && (dh || ok)) { g_mod_on[it] = !g_mod_on[it]; mods_salvar(); }
        else if (ok && it == MR_ABRIR) mods_abrir_pasta();
        else if (ok && it == MR_REINICIAR) {
            if (g_menu_started && g_menu_confirm != OP_REINICIAR + 100) { g_menu_confirm = OP_REINICIAR + 100; return; }
            fprintf(stderr, "[menu] reiniciando para aplicar os mods\n"); exit(77);
        }
        return;
    }
    int rows[OP_N], n = op_rows(rows);
    if (g_menu_sel >= n) g_menu_sel = n - 1;
    if (dv) { g_menu_sel = (g_menu_sel + dv + n) % n; g_menu_confirm = -1; }
    int it = rows[g_menu_sel];
    if (dh) op_change(it, dh);
    if (back) { g_menu_page = 0; g_menu_sel = MS_OPCOES; return; }
    if (ok) {
        if (it == OP_VOLTAR) { g_menu_page = 0; g_menu_sel = MS_OPCOES; }
        else if (it == OP_REINICIAR) {
            if (g_menu_started && g_menu_confirm != OP_REINICIAR + 100) { g_menu_confirm = OP_REINICIAR + 100; return; }
            fprintf(stderr, "[menu] reiniciando para aplicar as opções\n"); exit(77);
        } else op_change(it, 1);
    }
}

/* ---- desenho */
static void hit_add(int id, float x, float y, float w, float h) {
    if (g_hit_n < 80) { g_hit[g_hit_n] = (SDL_Rect){ (int)x, (int)y, (int)w, (int)h }; g_hit_id[g_hit_n++] = id; }
}
/* arte de fundo da versão (opcional, do usuário): fundos/fundo_x e fundos/fundo_y (.jpg ou .png) na pasta do
 * projeto; o jogar.py passa a pasta em FUNDOS. Aparece no lugar do degradê enquanto a tela do jogo não está
 * visível (primeira abertura, tela de idioma do jogo, carregamentos). Nenhuma imagem vem com o projeto. */
unsigned char *stbi_load_from_memory(const unsigned char *buf, int len, int *x, int *y, int *comp, int req);   /* texpack.c */
void stbi_image_free(void *p);
static uint32_t *g_arte; static int g_arte_w, g_arte_h, g_arte_lida;
static void arte_carregar(void) {
    if (g_arte_lida) return;
    g_arte_lida = 1;
    const char *dir = getenv("FUNDOS"); if (!dir || !*dir || !g_versao) return;
    static const char *const ext[3] = { "jpg", "jpeg", "png" };
    for (int e = 0; e < 3 && !g_arte; e++) {
        char cam[1024]; snprintf(cam, sizeof cam, "%s/fundo_%c.%s", dir, g_versao == 'X' ? 'x' : 'y', ext[e]);
        FILE *f = fopen(cam, "rb"); if (!f) continue;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        unsigned char *b = n > 0 && n < (64l << 20) ? malloc((size_t)n) : NULL;
        int ok = b && fread(b, 1, (size_t)n, f) == (size_t)n; fclose(f);
        int w = 0, h = 0, ch = 0; unsigned char *px = ok ? stbi_load_from_memory(b, (int)n, &w, &h, &ch, 4) : NULL;
        free(b);
        if (!px) { fprintf(stderr, "[menu] não consegui ler %s\n", cam); continue; }
        g_arte = malloc((size_t)w * h * 4);
        if (g_arte) for (long i = 0; i < (long)w * h; i++) g_arte[i] = 0xFF000000u | (uint32_t)px[i * 4] << 16 | (uint32_t)px[i * 4 + 1] << 8 | px[i * 4 + 2];
        g_arte_w = w; g_arte_h = h; stbi_image_free(px);
        fprintf(stderr, "[menu] arte de fundo: %s (%dx%d)\n", cam, w, h);
    }
}
/* a arte cobrindo bw x bh (corta o que sobra), um pouco escurecida e com a cor da versão à esquerda, onde fica o menu */
static int arte_fundo_g(MuCanvas *b, int grad);
static int arte_fundo(MuCanvas *b) { return arte_fundo_g(b, 1); }
static int arte_fundo_g(MuCanvas *b, int grad) {
    arte_carregar();
    if (!g_arte) return 0;
    float k = (float)b->w / g_arte_w; if ((float)b->h / g_arte_h > k) k = (float)b->h / g_arte_h;
    float ox = (g_arte_w * k - b->w) / 2, oy = (g_arte_h * k - b->h) / 2;
    int ns = k < 0.5f ? (int)(1 / k + 0.5f) : 1; if (ns > 4) ns = 4;   /* reduzindo muito: várias amostras por pixel (sem serrilhado) */
    for (int y = 0; y < b->h; y++) for (int x = 0; x < b->w; x++) {
        unsigned r = 0, g = 0, bl = 0;
        for (int j = 0; j < ns; j++) for (int i = 0; i < ns; i++) {
            float sy = (y + oy + (j + 0.5f) / ns) / k - 0.5f, sx = (x + ox + (i + 0.5f) / ns) / k - 0.5f;
            int iy = (int)sy, ix = (int)sx; if (iy < 0) iy = 0; if (iy > g_arte_h - 2) iy = g_arte_h - 2; if (ix < 0) ix = 0; if (ix > g_arte_w - 2) ix = g_arte_w - 2;
            float fy = sy - iy, fx = sx - ix; if (fy < 0) fy = 0; if (fx < 0) fx = 0;
            const uint32_t *p = g_arte + (long)iy * g_arte_w + ix;
            uint32_t v = mu_lerp(mu_lerp(p[0], p[1], (int)(fx * 255)), mu_lerp(p[g_arte_w], p[g_arte_w + 1], (int)(fx * 255)), (int)(fy * 255));
            r += v >> 16 & 255; g += v >> 8 & 255; bl += v & 255;
        }
        unsigned q = (unsigned)(ns * ns);
        uint32_t v = 0xFF000000u | (r / q) << 16 | (g / q) << 8 | (bl / q);
        b->px[(long)y * b->w + x] = mu_scale(v, 175) | 0xFF000000u;   /* (~70%: o texto do menu fica legível) */
    }
    if (!grad) return 1;
    mu_hgrad(b, 0, 0, (int)(b->w * 0.62f), b->h, mu_rgba(g_tema.base, 215), mu_rgba(g_tema.base, 0), 1);
    mu_vgrad(b, 0, (int)(b->h * 0.7f), b->w, b->h - (int)(b->h * 0.7f), mu_rgba(g_tema.base, 0), mu_rgba(g_tema.base, 170), 1);
    return 1;
}
/* ---- arte de fundo animada: a imagem (um pouco maior que a janela) se aproxima e desliza devagar, num ciclo
 * longo; por cima, a sombra do lado do menu (fixa), um brilho que "respira" e partículas de luz subindo na cor da
 * versão. Tudo desenhado pela placa (texturas prontas), sem custo de CPU por quadro. MENU_ANIMADO=0 desliga. */
#define ARTE_MARGEM 1.18f
static int g_menu_animado = -1;
static void arte_animada_criar(int W, int H) {
    if (g_menu_animado < 0) { const char *e = getenv("MENU_ANIMADO"); g_menu_animado = !(e && *e == '0'); }
    if (g_arte_tex) { SDL_DestroyTexture(g_arte_tex); g_arte_tex = NULL; }
    if (g_arte_sombra) { SDL_DestroyTexture(g_arte_sombra); g_arte_sombra = NULL; }
    if (!g_menu_animado || !g_arte) return;
    int aw = (int)(W * ARTE_MARGEM), ah = (int)(H * ARTE_MARGEM);
    MuCanvas a = { malloc((size_t)aw * ah * 4), aw, ah };
    if (!a.px) return;
    arte_fundo_g(&a, 0);
    g_arte_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, aw, ah);
    if (g_arte_tex) { SDL_UpdateTexture(g_arte_tex, NULL, a.px, aw * 4); SDL_SetTextureScaleMode(g_arte_tex, SDL_ScaleModeLinear); }
    free(a.px);
    /* sombra (as mesmas faixas escuras da versão parada), em 1/4 da resolução: só alfa */
    int sw = W / 4 > 8 ? W / 4 : 8, sh = H / 4 > 8 ? H / 4 : 8;
    uint32_t *px = malloc((size_t)sw * sh * 4); if (!px) return;
    uint32_t cor = g_tema.base & 0xFFFFFF;
    for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) {
        float fx = x / (sw * 0.62f), ah1 = fx < 1 ? 215 * (1 - fx) : 0;
        float fy = (y - sh * 0.7f) / (sh * 0.3f), ah2 = fy > 0 ? 170 * fy : 0;
        float al = ah1 + ah2 * (1 - ah1 / 255);                       /* (as duas camadas, uma sobre a outra) */
        px[(long)y * sw + x] = (uint32_t)(al > 255 ? 255 : al) << 24 | cor;
    }
    g_arte_sombra = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, sw, sh);
    if (g_arte_sombra) { SDL_UpdateTexture(g_arte_sombra, NULL, px, sw * 4); SDL_SetTextureBlendMode(g_arte_sombra, SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(g_arte_sombra, SDL_ScaleModeLinear); }
    free(px);
    if (!g_brilho_tex) {                                              /* ponto de luz macio (partículas e brilho) */
        const int N = 64; uint32_t *q = malloc((size_t)N * N * 4);
        if (q) {
            for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
                float dx = (x + 0.5f - N / 2.0f) / (N / 2.0f), dy = (y + 0.5f - N / 2.0f) / (N / 2.0f), d = sqrtf(dx * dx + dy * dy);
                float v = d >= 1 ? 0 : (1 - d) * (1 - d); v = v * 0.85f + (d < 0.18f ? (0.18f - d) / 0.18f * 0.15f : 0);
                uint32_t c = (uint32_t)(v * 255); q[y * N + x] = 0xFF000000u | c << 16 | c << 8 | c;
            }
            g_brilho_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, N, N);
            if (g_brilho_tex) { SDL_UpdateTexture(g_brilho_tex, NULL, q, N * 4); SDL_SetTextureBlendMode(g_brilho_tex, SDL_BLENDMODE_ADD); SDL_SetTextureScaleMode(g_brilho_tex, SDL_ScaleModeLinear); }
            free(q);
        }
    }
}
/* partículas: posição/velocidade de cada uma sai de um número fixo (sem estado), o tempo decide onde está */
static float arte_hash(int i, int k) { uint32_t h = (uint32_t)i * 2654435761u ^ (uint32_t)k * 2246822519u; h ^= h >> 15; h *= 2654435761u; h ^= h >> 13; return (h & 0xFFFFFF) / 16777216.0f; }
static void arte_animada_desenhar(int ww, int wh, int a, float t) {
    if (!g_arte_tex) return;
    float s = wh / 1440.0f; if (ww / 2560.0f < s) s = ww / 2560.0f;
    /* aproximação lenta (ciclo de 40 s, indo e voltando) e deslize em outro ritmo, para não repetir igual */
    float z = 1.04f + 0.06f * (0.5f - 0.5f * cosf(t * 6.2831853f / 40.0f));
    float px = 0.018f * sinf(t * 6.2831853f / 53.0f), py = 0.012f * sinf(t * 6.2831853f / 47.0f + 1.3f);
    SDL_FRect d; d.w = ww * z; d.h = wh * z; d.x = (ww - d.w) / 2 + px * ww; d.y = (wh - d.h) / 2 + py * wh;
    if (d.x > 0) d.x = 0; if (d.y > 0) d.y = 0; if (d.x + d.w < ww) d.x = ww - d.w; if (d.y + d.h < wh) d.y = wh - d.h;
    SDL_SetTextureAlphaMod(g_arte_tex, (Uint8)a);
    SDL_RenderCopyF(g_ren, g_arte_tex, NULL, &d);
    if (!g_brilho_tex) goto sombra;
    {   /* brilho que respira, do lado direito (onde fica a arte), na cor da versão */
        float r = 0.5f + 0.5f * sinf(t * 6.2831853f / 7.0f);
        uint32_t c = g_tema.accent2;
        SDL_SetTextureColorMod(g_brilho_tex, (Uint8)(c >> 16), (Uint8)(c >> 8), (Uint8)c);
        SDL_SetTextureAlphaMod(g_brilho_tex, (Uint8)(a * (0.10f + 0.10f * r)));
        float R = wh * (1.05f + 0.08f * r);
        SDL_FRect g = { ww * 0.70f - R / 2, wh * 0.55f - R / 2, R, R };
        SDL_RenderCopyF(g_ren, g_brilho_tex, NULL, &g);
    }
    {   /* partículas de luz subindo: duas cores do tema, tamanhos e velocidades diferentes, entram e saem suaves */
        const int NP = 90;
        for (int i = 0; i < NP; i++) {
            float vel = 0.025f + 0.05f * arte_hash(i, 1);                     /* altura da janela por segundo */
            float fase = arte_hash(i, 2), ciclo = 1.25f / vel;
            float u = fmodf(t / ciclo + fase, 1.0f);                           /* 0 = embaixo, 1 = sumiu em cima */
            float x0 = 0.25f + 0.78f * arte_hash(i, 3);                        /* mais do lado da arte que do menu */
            float x = (x0 + 0.02f * sinf(t * (0.4f + arte_hash(i, 4)) + i)) * ww;
            float y = (1.1f - 1.25f * u) * wh;
            float tam = (6 + 22 * arte_hash(i, 5) * arte_hash(i, 5)) * s * 2.2f;
            float vida = u < 0.15f ? u / 0.15f : u > 0.75f ? (1 - u) / 0.25f : 1;
            float pisca = 0.7f + 0.3f * sinf(t * (1.5f + 2 * arte_hash(i, 6)) + i * 1.7f);
            uint32_t c = arte_hash(i, 7) < 0.6f ? g_tema.accent : g_tema.accent2;
            SDL_SetTextureColorMod(g_brilho_tex, (Uint8)(c >> 16), (Uint8)(c >> 8), (Uint8)c);
            SDL_SetTextureAlphaMod(g_brilho_tex, (Uint8)(a * vida * pisca * (0.45f + 0.55f * arte_hash(i, 8))));
            SDL_FRect q = { x - tam / 2, y - tam / 2, tam, tam };
            SDL_RenderCopyF(g_ren, g_brilho_tex, NULL, &q);
        }
        SDL_SetTextureColorMod(g_brilho_tex, 255, 255, 255);
    }
sombra:
    if (g_arte_sombra) { SDL_SetTextureAlphaMod(g_arte_sombra, (Uint8)a); SDL_Rect f = { 0, 0, ww, wh }; SDL_RenderCopy(g_ren, g_arte_sombra, NULL, &f); }
}
static void menu_build(int W, int H) {
    if (g_ui_w != W || g_ui_h != H) {
        free(g_ui.px); g_ui.px = malloc((size_t)W * H * 4); g_ui.w = W; g_ui.h = H; g_ui_w = W; g_ui_h = H;
        if (g_ui_tex) SDL_DestroyTexture(g_ui_tex);
        g_ui_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
        SDL_BlendMode pm = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
                                                      SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);
        if (g_ui_tex && SDL_SetTextureBlendMode(g_ui_tex, pm) != 0) SDL_SetTextureBlendMode(g_ui_tex, SDL_BLENDMODE_BLEND);
        /* fundo sem o jogo (antes da primeira imagem e na tela de idioma): degradê da versão */
        if (g_bg_tex) SDL_DestroyTexture(g_bg_tex);
        arte_carregar();
        int bw = W / 4 > 8 ? W / 4 : 8, bh = H / 4 > 8 ? H / 4 : 8;
        if (g_arte) { bw = W; bh = H; }                  /* (a arte: na resolução da janela) */
        MuCanvas b = { malloc((size_t)bw * bh * 4), bw, bh };
        if (!arte_fundo(&b)) {
            mu_clear(&b, g_tema.base);
            mu_vgrad(&b, 0, 0, bw, bh, mu_rgba(g_tema.base2, 0), mu_rgba(g_tema.base2, 255), 1);
            mu_glow(&b, bw * 0.78f, bh * 0.72f, bw * 0.55f, bh * 0.75f, mu_rgba(g_tema.accent2, 110));
        }
        { const char *mp = getenv("MENU_PPM"); if (mp) { char nm[1100]; snprintf(nm, sizeof nm, "%s.fundo.ppm", mp); FILE *o = fopen(nm, "wb");   /* depuração */
            if (o) { fprintf(o, "P6\n%d %d\n255\n", bw, bh); for (long k = 0; k < (long)bw * bh; k++) { uint32_t v = b.px[k]; uint8_t c3[3] = { (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; fwrite(c3, 1, 3, o); } fclose(o); } } }
        g_bg_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, bw, bh);
        if (g_bg_tex) SDL_UpdateTexture(g_bg_tex, NULL, b.px, bw * 4);
        free(b.px);
        arte_animada_criar(W, H);
        g_menu_dirty = 1;
    }
    if (!g_menu_dirty || !g_ui.px) return;
    g_menu_dirty = 0; g_hit_n = 0;
    MuCanvas *c = &g_ui; float s = H / 1440.0f; if (W / 2560.0f < s) s = W / 2560.0f;
    mu_clear(c, 0);
    /* escurece para o texto aparecer sobre o jogo */
    mu_fill(c, 0, 0, W, H, mu_rgba(g_tema.base, 70));
    mu_hgrad(c, 0, 0, (int)(W * 0.62f), H, mu_rgba(g_tema.base, 225), mu_rgba(g_tema.base, 0), 1);
    mu_vgrad(c, 0, (int)(H * 0.5f), W, H - (int)(H * 0.5f), mu_rgba(0, 0), mu_rgba(g_tema.base, 200), 1);
    mu_vgrad(c, 0, 0, W, (int)(H * 0.25f), mu_rgba(g_tema.base, 120), mu_rgba(0, 0), 1);
    float ML = 128 * s;
    /* nome do jogo */
    mu_text(c, FM_PEQ, 28 * s, ML, 176 * s, g_tema.accent, 7 * s, "XY PROJECT");
    const char *nome = g_versao == 'X' ? "Pokémon X" : g_versao == 'Y' ? "Pokémon Y" : "Pokémon XY";
    mu_text(c, FM_TITULO, 150 * s, ML - 6 * s, 318 * s, g_tema.text, -2 * s, nome);
    mu_hgrad(c, (int)ML, (int)(352 * s), (int)(120 * s), (int)(6 * s), mu_rgba(g_tema.accent, 255), mu_rgba(g_tema.accent, 0), 0);
    /* menu */
    if (g_menu_page == 0) {
        int ids[6], nl = menu_col_esq(ids);
        for (int k = 0; k < nl; k++) {
            float y = (nl > 4 ? 700 : nl > 3 ? 740 : 780) * s + k * (nl > 4 ? 88 : nl > 3 ? 92 : 100) * s;
            int sel = g_menu_sel == ids[k];
            char mt[64]; int na = 0; for (int i = 0; i < g_mod_n; i++) na += g_mod_on[i];
            if (na) snprintf(mt, sizeof mt, "MODS  ·  %d", na); else snprintf(mt, sizeof mt, "MODS");
            const char *t = ids[k] == MS_OPCOES ? TR("OPÇÕES", "SETTINGS", "OPCIONES", "設定") : ids[k] == MS_MODS ? mt
                          : ids[k] == MS_CONTROLES ? TR("CONTROLES", "CONTROLS", "CONTROLES", "操作設定")
                          : ids[k] == MS_RESET ? (g_menu_confirm == MS_RESET ? TR("REINICIAR? O QUE NÃO FOI SALVO SE PERDE", "RESET? UNSAVED PROGRESS IS LOST", "¿REINICIAR? SE PIERDE LO NO GUARDADO", "リセットする？ セーブしていない進行は失われます") : TR("REINICIAR JOGO", "RESET GAME", "REINICIAR JUEGO", "ゲームをリセット"))
                          : (g_menu_confirm == MS_SAIR ? TR("SAIR? O QUE NÃO FOI SALVO SE PERDE", "QUIT? UNSAVED PROGRESS IS LOST", "¿SALIR? SE PIERDE LO NO GUARDADO", "終了する？ セーブしていない進行は失われます") : TR("SAIR", "QUIT", "SALIR", "終了"));
            if (sel) {
                mu_hgrad(c, (int)(ML - 40 * s), (int)(y - 56 * s), (int)(720 * s), (int)(80 * s), mu_rgba(g_tema.accent, 235), mu_rgba(g_tema.accent2, 0), 1);
                mu_fill(c, (int)(ML - 40 * s), (int)(y - 56 * s), (int)(6 * s), (int)(80 * s), mu_rgba(0xFFFFFF, 230));
            }
            mu_text(c, FM_ITEM, 46 * s, ML, y, sel ? g_tema.text : mu_rgba(g_tema.dim, 220), 3 * s, t);
            hit_add(ids[k], ML - 40 * s, y - 56 * s, 720 * s, 80 * s);
        }
    } else if (g_menu_page == 3) {
        mu_text(c, FM_PEQ, 28 * s, ML, 540 * s, 0xFFFF6B6B, 7 * s, "NUZLOCKE MODE");
        static const char *const av[4][5] = {
            { "Com o Nuzlocke ligado, todo Pokémon que desmaiar em batalha", "é APAGADO da sua equipe e do seu save, para sempre.",
              "Não há como desfazer: reiniciar o jogo não o traz de volta.", "Se a equipe inteira desmaiar, o primeiro Pokémon fica (a equipe", "não pode ficar vazia). Ovos não são afetados." },
            { "With Nuzlocke on, every Pokémon that faints in battle", "is DELETED from your party and your save, for good.",
              "This cannot be undone: resetting the game won't bring it back.", "If the whole party faints, the first Pokémon stays (the party", "can't be empty). Eggs are not affected." },
            { "Con el Nuzlocke activado, todo Pokémon que se debilite en combate", "se BORRA de tu equipo y de tu partida guardada, para siempre.",
              "No se puede deshacer: reiniciar el juego no lo trae de vuelta.", "Si todo el equipo se debilita, el primer Pokémon se queda (el equipo", "no puede quedar vacío). Los Huevos no se ven afectados." },
            { "Nuzlockeをオンにすると、バトルでひんしになったポケモンは", "手持ちとセーブデータから永久に削除されます。",
              "元には戻せません：ゲームをリセットしても戻りません。", "手持ちが全員ひんしになった場合、先頭のポケモンは残ります", "（手持ちは空にできません）。タマゴは対象外です。" } };
        for (int k = 0; k < 5; k++) mu_text(c, FM_PEQ, 30 * s, ML, 610 * s + k * 46 * s, k == 1 ? g_tema.text : mu_rgba(g_tema.dim, 240), 0, av[ui_lang()][k]);
        static const char *const bt[4][2] = { { "Ativar Nuzlocke", "Cancelar" }, { "Turn on Nuzlocke", "Cancel" },
                                              { "Activar Nuzlocke", "Cancelar" }, { "Nuzlockeをオンにする", "キャンセル" } };
        for (int k = 0; k < 2; k++) {
            float y = 900 * s + k * 84 * s; int sel = g_menu_sel == k;
            if (sel) {
                mu_hgrad(c, (int)(ML - 40 * s), (int)(y - 54 * s), (int)(720 * s), (int)(76 * s), mu_rgba(k ? g_tema.accent : 0xD9363E, 235), mu_rgba(k ? g_tema.accent2 : 0x7A1018, 0), 1);
                mu_fill(c, (int)(ML - 40 * s), (int)(y - 54 * s), (int)(6 * s), (int)(76 * s), mu_rgba(0xFFFFFF, 230));
            }
            mu_text(c, FM_ITEM, 42 * s, ML, y, sel ? g_tema.text : mu_rgba(g_tema.dim, 220), 2 * s, bt[ui_lang()][k]);
            hit_add(300 + k, ML - 40 * s, y - 54 * s, 720 * s, 76 * s);
        }
    } else if (g_menu_page == 4) {
        int rows[AC_N + 4], n = ctl_rows(rows); char buf[96];
        mu_text(c, FM_PEQ, 28 * s, ML, 515 * s, g_tema.accent, 7 * s, TR("CONTROLES", "CONTROLS", "CONTROLES", "操作設定"));
        float RW = 1180 * s, st = 38 * s, bh = st - 3 * s, bt = bh * 0.73f, fi = 27 * s, y0 = 585 * s, gap = 22 * s;
        float cK = ML + 560 * s, cP = ML + 900 * s, cw = 300 * s;   /* centros das colunas teclado e controle */
        { const char *h1 = TR("TECLADO", "KEYBOARD", "TECLADO", "キーボード"), *h2 = TR("CONTROLE", "CONTROLLER", "MANDO", "コントローラー");
          mu_text(c, FM_PEQ, 20 * s, cK - mu_text_w(FM_PEQ, 20 * s, 4 * s, h1) / 2, y0 - 44 * s, mu_rgba(g_tema.dim, 220), 4 * s, h1);
          mu_text(c, FM_PEQ, 20 * s, cP - mu_text_w(FM_PEQ, 20 * s, 4 * s, h2) / 2, y0 - 44 * s, mu_rgba(g_tema.dim, 220), 4 * s, h2); }
        #define CR_GRUPO(it) ((it) >= CR_PADRAO ? 2 : (it) >= CR_BOTOES ? 1 : 0)
        for (int k = 0; k < n; k++) {
            int it = rows[k], sel = g_menu_sel == k;
            float y = y0 + k * st + CR_GRUPO(it) * gap;
            if (k > 0 && CR_GRUPO(it) != CR_GRUPO(rows[k - 1]))
                mu_fill(c, (int)ML, (int)(y - bt - gap / 2 - 1 * s), (int)RW, 2 * s > 1 ? (int)(2 * s) : 1, mu_rgba(g_tema.dim, 140));
            if (sel) {
                mu_hgrad(c, (int)(ML - 40 * s), (int)(y - bt), (int)(RW + 80 * s), (int)bh, mu_rgba(g_tema.accent, 200), mu_rgba(g_tema.accent2, 30), 1);
                mu_fill(c, (int)(ML - 40 * s), (int)(y - bt), (int)(6 * s), (int)bh, mu_rgba(0xFFFFFF, 230));
            }
            const char *lb = it < AC_N ? ctl_nome(k_ctl_ordem[it]) : it == CR_BOTOES ? op_label(OP_BOTOES)
                           : it == CR_VEL ? TR("Velocidade ao acelerar", "Fast-forward speed", "Velocidad al acelerar", "早送りの速度")
                           : it == CR_PADRAO ? TR("Restaurar o padrão", "Restore defaults", "Restaurar valores predeterminados", "初期設定に戻す") : TR("Voltar", "Back", "Volver", "戻る");
            mu_text(c, FM_ITEM, fi, ML, y, sel ? g_tema.text : mu_rgba(g_tema.dim, 230), 1 * s, lb);
            if (it < AC_N) {
                for (int col = 0; col < 2; col++) {
                    float cx = col ? cP : cK; int foco = sel && g_ctl_col == col, esp = g_bind_ac == k_ctl_ordem[it] && g_bind_col == col;
                    ctl_texto(k_ctl_ordem[it], col, buf, sizeof buf);
                    if (foco || esp) mu_rrect(c, cx - cw / 2, y - bt + 3 * s, cw, bh - 6 * s, 8 * s, mu_rgba(esp ? 0xFFFFFF : g_tema.base, esp ? 90 : 120));
                    float w = mu_text_w(FM_PEQ, 28 * s, 0, buf);
                    float cy = y - bt + bh / 2;                /* centro da caixa; a linha de base fica ~0,36 da letra abaixo */
                    mu_text(c, FM_PEQ, 28 * s, cx - w / 2, cy + 0.36f * 28 * s, foco || esp ? g_tema.text : mu_rgba(g_tema.dim, 230), 0, buf);
                    hit_add(400 + k * 2 + col, cx - cw / 2, y - bt, cw, bh);
                }
            } else if (it == CR_BOTOES || it == CR_VEL) {
                const char *v = it == CR_BOTOES ? op_value(OP_BOTOES, buf, sizeof buf) : (snprintf(buf, sizeof buf, "%dx", g_turbo_mult), buf);
                char vv[96]; snprintf(vv, sizeof vv, sel ? "‹  %s  ›" : "%s", v);
                float w = mu_text_w(FM_PEQ, 28 * s, 0, vv);
                mu_text(c, FM_PEQ, 28 * s, ML + RW - w, y - 3 * s, sel ? g_tema.text : mu_rgba(g_tema.dim, 230), 0, vv);
            }
            hit_add(100 + k, ML - 40 * s, y - bt, RW + 80 * s, bh);
        }
        const char *dc = g_bind_ac >= 0 ? (g_bind_col ? TR("Aperte o botão do controle para esta ação  (Esc cancela)", "Press the controller button for this action  (Esc cancels)", "Pulsa el botón del mando para esta acción  (Esc cancela)", "この操作に割り当てるボタンを押してください（Escでキャンセル）")
                                                      : TR("Aperte a tecla para esta ação  (Esc cancela)", "Press the key for this action  (Esc cancels)", "Pulsa la tecla para esta acción  (Esc cancela)", "この操作に割り当てるキーを押してください（Escでキャンセル）"))
                       : TR("Enter ou clique para trocar  ·  ←→ teclado / controle  ·  as setas do teclado são o Circle Pad", "Enter or click to change  ·  ←→ keyboard / controller  ·  the arrow keys are the Circle Pad",
                            "Enter o clic para cambiar  ·  ←→ teclado / mando  ·  las flechas del teclado son el Circle Pad", "Enter かクリックで変更 ・ ←→ キーボード／コントローラー ・ 矢印キーはスライドパッド");
        mu_text(c, FM_PEQ, 22 * s, ML, 455 * s, g_bind_ac >= 0 ? g_tema.accent : mu_rgba(g_tema.dim, 210), 0, dc);
    } else if (g_menu_page == 2) {
        int rows[MOD_MAX + 3], n = mod_rows(rows);
        mu_text(c, FM_PEQ, 28 * s, ML, 540 * s, g_tema.accent, 7 * s, "MODS");
        float RW = 980 * s, y0 = 620 * s;
        if (!g_mod_n) {
            mu_text(c, FM_PEQ, 30 * s, ML, 610 * s, g_tema.text, 0, TR("Nenhum mod encontrado.", "No mods found.", "No se encontró ningún mod.", "MODが見つかりません。"));
            mu_text(c, FM_PEQ, 24 * s, ML, 656 * s, mu_rgba(g_tema.dim, 230), 0,
                    TR("Coloque cada mod numa pasta dentro de mods/ (com a pasta romfs dentro).", "Put each mod in its own folder inside mods/ (with the romfs folder inside).", "Pon cada mod en su propia carpeta dentro de mods/ (con la carpeta romfs dentro).", "各MODを mods/ の中の個別のフォルダーに入れてください（その中に romfs フォルダー）。"));
            y0 = 760 * s;
        }
        float lh = n > 9 ? 60 * s : 74 * s, fs = n > 9 ? 32 * s : 38 * s;
        for (int k = 0; k < n; k++) {
            int it = rows[k], sel = g_menu_sel == k;
            float y = y0 + k * lh;
            if (sel) {
                mu_hgrad(c, (int)(ML - 40 * s), (int)(y - lh * 0.68f), (int)(RW + 80 * s), (int)(lh * 0.95f), mu_rgba(g_tema.accent, 225), mu_rgba(g_tema.accent2, 30), 1);
                mu_fill(c, (int)(ML - 40 * s), (int)(y - lh * 0.68f), (int)(6 * s), (int)(lh * 0.95f), mu_rgba(0xFFFFFF, 230));
            }
            const char *lb = it < MR_ABRIR ? g_mod_nome[it] : it == MR_ABRIR ? TR("Abrir a pasta de mods", "Open the mods folder", "Abrir la carpeta de mods", "MODフォルダーを開く")
                           : it == MR_REINICIAR ? (g_menu_confirm == OP_REINICIAR + 100 ? TR("Reiniciar? O que não foi salvo se perde", "Restart? Unsaved progress is lost", "¿Reiniciar? Se pierde lo no guardado", "再起動する？ セーブしていない進行は失われます") : TR("Reiniciar agora", "Restart now", "Reiniciar ahora", "今すぐ再起動"))
                           : TR("Voltar", "Back", "Volver", "戻る");
            mu_text(c, FM_ITEM, fs, ML, y, sel ? g_tema.text : mu_rgba(g_tema.dim, 230), 1 * s, lb);
            if (it < MR_ABRIR) {
                const char *v = g_mod_on[it] ? TR("Ligado", "On", "Activado", "オン") : TR("Desligado", "Off", "Desactivado", "オフ");
                char vv[64]; snprintf(vv, sizeof vv, g_mod_on[it] != g_mod_ini[it] ? (sel ? "‹  %s *  ›" : "%s *") : (sel ? "‹  %s  ›" : "%s"), v);
                float w = mu_text_w(FM_PEQ, 32 * s, 0, vv);
                mu_text(c, FM_PEQ, 32 * s, ML + RW - w, y - 2 * s, g_mod_on[it] ? (sel ? g_tema.text : g_tema.accent) : mu_rgba(g_tema.dim, 230), 0, vv);
            }
            hit_add(200 + k, ML - 40 * s, y - lh * 0.68f, RW + 80 * s, lh * 0.95f);
        }
        if (mods_mudou())
            mu_text(c, FM_PEQ, 24 * s, ML, y0 + n * lh + 6 * s, mu_rgba(g_tema.dim, 220), 0,
                    TR("* vale depois de reiniciar o jogo", "* applies after restarting the game", "* se aplica después de reiniciar el juego", "※ゲームの再起動後に反映されます"));
    } else {
        int rows[OP_N], n = op_rows(rows); char buf[64];
        mu_text(c, FM_PEQ, 28 * s, ML, 540 * s, g_tema.accent, 7 * s, TR("OPÇÕES", "SETTINGS", "OPCIONES", "設定"));
        float RW = 980 * s, st = (n > 13 ? 42 : n > 12 ? 48 : n > 11 ? 56 : 68) * s, bh = st - 4 * s, bt = bh * 0.73f, fi = (n > 13 ? 29 : n > 12 ? 31 : n > 11 ? 34 : 38) * s,   /* (muitas linhas: mais juntas) */
              y0 = (n > 13 ? 585 : n > 12 ? 600 : 620) * s, gap = 24 * s;
        /* grupos, separados por um espaço e uma linha: volume | vídeo, controle e idioma | funções de jogo
         * (Poké Radar, XP, Nuzlocke) | reiniciar e voltar */
        #define OP_GRUPO(it) ((it) >= OP_REINICIAR ? 3 : (it) >= OP_RADAR ? 2 : (it) >= OP_RENDER ? 1 : 0)
        for (int k = 0; k < n; k++) {
            int it = rows[k], sel = g_menu_sel == k;
            float y = y0 + k * st + OP_GRUPO(it) * gap;
            if (k > 0 && OP_GRUPO(it) != OP_GRUPO(rows[k - 1]))   /* linha entre dois grupos */
                mu_fill(c, (int)ML, (int)(y - bt - gap / 2 - 1 * s), (int)RW, 2 * s > 1 ? (int)(2 * s) : 1, mu_rgba(g_tema.dim, 140));
            if (sel) {
                mu_hgrad(c, (int)(ML - 40 * s), (int)(y - bt), (int)(RW + 80 * s), (int)bh, mu_rgba(g_tema.accent, 225), mu_rgba(g_tema.accent2, 30), 1);
                mu_fill(c, (int)(ML - 40 * s), (int)(y - bt), (int)(6 * s), (int)bh, mu_rgba(0xFFFFFF, 230));
            }
            const char *lb = op_label(it);
            if (it == OP_REINICIAR && g_menu_confirm == OP_REINICIAR + 100) lb = TR("Reiniciar? O que não foi salvo se perde", "Restart? Unsaved progress is lost", "¿Reiniciar? Se pierde lo no guardado", "再起動する？ セーブしていない進行は失われます");
            mu_text(c, FM_ITEM, fi, ML, y, sel ? g_tema.text : mu_rgba(g_tema.dim, 230), 1 * s, lb);
            if (it == OP_VOLUME) {                    /* barra: arrastar com o mouse, ou ← → de 5 em 5 */
                const char *v = op_value(it, buf, sizeof buf);
                float vw = 120 * s, bw = 380 * s, bx = ML + RW - vw - bw, by = y - 16 * s, th = 8 * s, f = g_op_val[OP_VOLUME] / 100.0f;
                float tw = mu_text_w(FM_PEQ, 32 * s, 0, v);
                mu_text(c, FM_PEQ, 32 * s, ML + RW - tw, y - 2 * s, sel ? g_tema.text : mu_rgba(g_tema.dim, 230), 0, v);
                mu_rrect(c, bx, by, bw, th, th / 2, mu_rgba(0xFFFFFF, sel ? 70 : 45));
                if (f > 0) mu_rrect(c, bx, by, bw * f, th, th / 2, sel ? mu_rgba(0xFFFFFF, 235) : g_tema.accent);
                float kr = (sel ? 15 : 12) * s;
                mu_rrect(c, bx + bw * f - kr, by + th / 2 - kr, 2 * kr, 2 * kr, kr, sel ? mu_rgba(0xFFFFFF, 255) : mu_rgba(0xFFFFFF, 200));
                g_vol_x0 = bx; g_vol_x1 = bx + bw;
            } else if (it < OP_REINICIAR) {
                const char *v = op_value(it, buf, sizeof buf);
                char vv[96]; snprintf(vv, sizeof vv, sel ? "‹  %s  ›" : "%s", v);
                if ((it == OP_RENDER && g_op_val[it] != g_op_ini_render) || (it == OP_IDIOMA && g_op_val[it] != g_op_ini_idioma))
                    snprintf(vv, sizeof vv, sel ? "‹  %s *  ›" : "%s *", v);
                float w = mu_text_w(FM_PEQ, 32 * s, 0, vv);
                mu_text(c, FM_PEQ, 32 * s, ML + RW - w, y - 2 * s, sel ? g_tema.text : mu_rgba(g_tema.dim, 230), 0, vv);
            }
            hit_add(100 + k, ML - 40 * s, y - bt, RW + 80 * s, bh);
        }
        if (menu_pending())
            mu_text(c, FM_PEQ, 24 * s, ML, y0 + n * st + 3 * gap + 6 * s, mu_rgba(g_tema.dim, 220), 0,
                    TR("* vale depois de reiniciar o jogo", "* applies after restarting the game", "* se aplica después de reiniciar el juego", "※ゲームの再起動後に反映されます"));
    }
    /* status embaixo à esquerda */
    const char *ij = k_idioma_nome[ui_lang()][g_op_ini_idioma % 7];
    char st[200]; snprintf(st, sizeof st, "Nintendo 3DS  •  %s  •  %s", nome, ij);
    { int na = mods_ativos_ini(); if (na) { size_t L = strlen(st); snprintf(st + L, sizeof st - L, "  •  %d mod%s", na, na > 1 ? "s" : ""); } }
    mu_text(c, FM_PEQ, 28 * s, ML, H - 118 * s, g_tema.text, 0, TR("✓  Jogo pronto", "✓  Game ready", "✓  Juego listo", "✓  準備完了"));
    mu_text(c, FM_PEQ, 24 * s, ML, H - 74 * s, mu_rgba(g_tema.dim, 230), 1 * s, st);
    /* cartucho (desenhado à parte, a cada quadro) e o rótulo dele */
    g_cart_sz = 460 * s; g_cart_cx = W - 330 * s; g_cart_cy = H - 520 * s;
    int fj = g_menu_page == 0 && g_menu_sel == MS_JOGAR;
    if (fj) mu_glow(c, g_cart_cx, g_cart_cy + 20 * s, 380 * s, 330 * s, mu_rgba(g_tema.accent, 120));
    else mu_glow(c, g_cart_cx, g_cart_cy + 20 * s, 340 * s, 300 * s, mu_rgba(g_tema.accent2, 60));
    const char *jl = TR("JOGAR", "PLAY", "JUGAR", "プレイ");
    float jw = mu_text_w(FM_ITEM, 40 * s, 10 * s, jl);
    mu_text(c, FM_ITEM, 40 * s, g_cart_cx - jw / 2, H - 196 * s, fj ? g_tema.text : mu_rgba(g_tema.dim, 230), 10 * s, jl);
    if (fj) mu_hgrad(c, (int)(g_cart_cx - jw / 2), (int)(H - 180 * s), (int)jw, (int)(4 * s), mu_rgba(g_tema.accent, 255), mu_rgba(g_tema.accent2, 255), 0);
    hit_add(MS_JOGAR, g_cart_cx - g_cart_sz * 0.55f, g_cart_cy - g_cart_sz * 0.6f, g_cart_sz * 1.1f, g_cart_sz * 0.6f + (H - 180 * s) - g_cart_cy);
    /* ROM, bem discreta, no canto */
    const char *rom = getenv("JOGO_3DS"); const char *rn = rom ? rom : "";
    for (const char *p = rn; *p; p++) if (*p == '/' || *p == '\\') rn = p + 1;
    int fr = g_menu_page == 0 && g_menu_sel == MS_ROM;
    const char *tr = g_menu_confirm == MS_ROM ? TR("TROCAR? O QUE NÃO FOI SALVO SE PERDE", "CHANGE? UNSAVED PROGRESS IS LOST", "¿CAMBIAR? SE PIERDE LO NO GUARDADO", "変更する？ セーブしていない進行は失われます") : TR("TROCAR ROM", "CHANGE ROM", "CAMBIAR ROM", "ROMを変更");
    float tw = mu_text_w(FM_PEQ, 22 * s, 4 * s, tr);
    char rl[300]; snprintf(rl, sizeof rl, "%.60s", rn);
    float rw = mu_text_w(FM_PEQ, 22 * s, 0, rl);
    float rx = W - 96 * s - tw, ry = H - 74 * s;
    mu_text(c, FM_PEQ, 22 * s, rx - 36 * s - rw, ry, mu_rgba(g_tema.dim, 200), 0, rl);
    mu_text(c, FM_PEQ, 22 * s, rx, ry, fr ? g_tema.accent : mu_rgba(g_tema.text, 230), 4 * s, tr);
    if (fr) mu_fill(c, (int)rx, (int)(ry + 10 * s), (int)tw, (int)(3 * s), mu_rgba(g_tema.accent, 255));
    hit_add(MS_ROM, rx - 36 * s - rw, ry - 34 * s, rw + 36 * s + tw, 54 * s);
    /* dica das teclas */
    const char *dica = g_menu_page ? TR("↑↓ escolher   ←→ mudar   Esc voltar", "↑↓ select   ←→ change   Esc back", "↑↓ elegir   ←→ cambiar   Esc volver", "↑↓ 選択   ←→ 変更   Esc 戻る")
                                   : (g_menu_started ? TR("Esc volta ao jogo", "Esc returns to the game", "Esc vuelve al juego", "Esc でゲームに戻る") : "");
    if (*dica) { float dw = mu_text_w(FM_PEQ, 22 * s, 0, dica); mu_text(c, FM_PEQ, 22 * s, W - 96 * s - dw, 120 * s, mu_rgba(g_tema.dim, 190), 0, dica); }
    if (g_ui_tex) SDL_UpdateTexture(g_ui_tex, NULL, g_ui.px, W * 4);
    { const char *mp = getenv("MENU_PPM"); if (mp) { FILE *o = fopen(mp, "wb");   /* depuração: salva o menu desenhado */
        if (o) { fprintf(o, "P6\n%d %d\n255\n", g_ui.w, g_ui.h); for (int k = 0; k < g_ui.w * g_ui.h; k++) { uint32_t v = g_ui.px[k]; uint8_t c3[3] = { (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; fwrite(c3, 1, 3, o); } fclose(o); } } }
}
/* desenha o menu por cima (fundo: tela de baixo ao vivo, ou o degradê) */
static void menu_draw(int ww, int wh) {
    if (!g_menu_on) return;
    Uint32 now = SDL_GetTicks(); static Uint32 last; float dt = last ? (now - last) / 1000.0f : 0; last = now; if (dt > 0.1f) dt = 0.1f;
    g_menu_fade += (g_menu_target ? 1 : -1) * dt / 0.28f;
    if (g_menu_fade >= 1) g_menu_fade = 1;
    if (g_menu_fade <= 0) { g_menu_fade = 0; g_menu_on = 0; g_cart_boost = 0; return; }
    int a = (int)(g_menu_fade * 255);
    /* fundo: o degradê da versão e, por cima, a tela de baixo ao vivo (antes de jogar) ou a de cima (depois de começar
     * o jogo); quando ela está preta (abertura, carregando)
     * o degradê aparece (transição suave pela luminosidade média da tela de baixo) */
    static float vis;
    float lum = 0;
    if (g_frame_seen && !g_lang_select) {
        SDL_LockMutex(g_lock);
        long acc = 0; int n = 0;
        int top = g_menu_started;                      /* depois de começar o jogo: fundo = tela de cima */
        for (int y = top ? 0 : 240; y < (top ? 240 : 480); y += 6) for (int x = top ? 0 : 40; x < (top ? 400 : 360); x += 8) { uint32_t p = g_canvas[y * CW + x]; acc += (p >> 16 & 255) + (p >> 8 & 255) + (p & 255); n++; }
        SDL_UnlockMutex(g_lock);
        lum = n ? acc / (3.0f * n) : 0;
    }
    float want = lum < 6 ? 0 : lum > 24 ? 1 : (lum - 6) / 18;
    vis += (want - vis) * (dt * 4 > 1 ? 1 : dt * 4);
    SDL_Rect full = { 0, 0, ww, wh };
    if (g_arte_tex && vis < 0.99f && g_ui_w == ww && g_ui_h == wh) {            /* arte animada (só enquanto aparece) */
        static Uint32 t0; if (!t0) t0 = now;
        float tt = (now - t0) / 1000.0f; { const char *e = getenv("MENU_ARTE_T"); if (e) tt = (float)atof(e); }   /* (depuração) */
        arte_animada_desenhar(ww, wh, a, tt);
    } else if (g_bg_tex) { SDL_SetTextureAlphaMod(g_bg_tex, (Uint8)a); SDL_RenderCopy(g_ren, g_bg_tex, NULL, &full); }
    int ba = (int)(a * vis);
    if (ba > 2) {
        int top = g_menu_started, sw = top ? 400 : 320;
        int h = ww * 240 / sw, y = (wh - h) / 2; SDL_Rect d = { 0, y, ww, h };
        if (h < wh) { int w = wh * sw / 240; d = (SDL_Rect){ (ww - w) / 2, 0, w, wh }; }
        SDL_Texture *t = top ? ((g_htex && g_hd_ready && g_hd_sc >= 1) ? g_htex : g_tex)
                             : ((g_hbtex && g_hb_ready && g_hb_sc > 1) ? g_hbtex : g_tex);
        SDL_Rect src = top ? (SDL_Rect){ 0, 0, 400, 240 } : (SDL_Rect){ 40, 240, 320, 240 };
        SDL_SetTextureBlendMode(t, ba >= 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND); SDL_SetTextureAlphaMod(t, (Uint8)ba);
        SDL_RenderCopy(g_ren, t, t == g_tex ? &src : NULL, &d);
        SDL_SetTextureAlphaMod(t, 255); SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
    }
    menu_build(ww, wh);
    if (g_ui_tex) { SDL_SetTextureColorMod(g_ui_tex, (Uint8)a, (Uint8)a, (Uint8)a); SDL_SetTextureAlphaMod(g_ui_tex, (Uint8)a); SDL_RenderCopy(g_ren, g_ui_tex, NULL, &full); }
    /* cartucho girando devagar na horizontal (mais rápido ao escolher jogar); desenhado numa thread à parte */
    if (g_cart_ok) {
        g_cart_yaw += dt * (0.42f + g_cart_boost * 7.0f);
        float bob = sinf(now / 1000.0f * 1.1f) * 0.035f;
        int fj = g_menu_page == 0 && g_menu_sel == MS_JOGAR;
        int R = (int)g_cart_sz; if (R > 420) R = 420;                       /* desenha no máximo a 420 px; a placa escala */
        SDL_LockMutex(g_cart_mx);
        g_cart_req_yaw = g_cart_yaw; g_cart_req_pitch = -0.16f + bob; g_cart_req_glow = fj ? 1.0f : 0.0f; g_cart_req_R = R;
        int S = g_cart_front_S; int ready = g_cart_new; g_cart_new = 0;
        if (ready && S > 0) {
            if (S != g_cart_S || !g_cart_tex) {
                g_cart_S = S;
                if (g_cart_tex) SDL_DestroyTexture(g_cart_tex);
                g_cart_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, S, S);
                SDL_BlendMode pm = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
                                                              SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);
                if (g_cart_tex && SDL_SetTextureBlendMode(g_cart_tex, pm) != 0) SDL_SetTextureBlendMode(g_cart_tex, SDL_BLENDMODE_BLEND);
            }
            if (g_cart_tex) SDL_UpdateTexture(g_cart_tex, NULL, g_cart_front, S * 4);
        }
        SDL_UnlockMutex(g_cart_mx);
        SDL_CondSignal(g_cart_cv_go);
        if (g_cart_tex && g_cart_S > 0) {
            float k = g_cart_sz / (g_cart_S / 1.3f - 3);                       /* textura (R) -> tamanho na tela */
            int D = (int)(g_cart_S * k);
            float ft = 8 * sinf(now / 1000.0f * 1.6f) * (wh / 1440.0f);
            SDL_Rect d = { (int)(g_cart_cx - D / 2.0f), (int)(g_cart_cy - D / 2.0f + ft), D, D };
            SDL_SetTextureColorMod(g_cart_tex, (Uint8)a, (Uint8)a, (Uint8)a); SDL_SetTextureAlphaMod(g_cart_tex, (Uint8)a);
            SDL_RenderCopy(g_ren, g_cart_tex, NULL, &d);
        }
    }
}
/* mouse: posição em pixels da janela */
static int menu_hit(int x, int y) {
    for (int i = 0; i < g_hit_n; i++) if (x >= g_hit[i].x && x < g_hit[i].x + g_hit[i].w && y >= g_hit[i].y && y < g_hit[i].y + g_hit[i].h) return g_hit_id[i];
    return -1;
}
static void vol_arrastar(int x, int salvar) {
    float f = (x - g_vol_x0) / (g_vol_x1 - g_vol_x0 > 1 ? g_vol_x1 - g_vol_x0 : 1);
    int v = (int)(f * 100 + 0.5f); if (v < 0) v = 0; if (v > 100) v = 100;
    if (v != g_op_val[OP_VOLUME]) { g_op_val[OP_VOLUME] = v; g_volume_pct = v; g_menu_dirty = 1; }
    if (salvar) { char num[8]; snprintf(num, sizeof num, "%d", v); opcoes_set("volume", num); }
}
/* botão do mouse solto: termina o arraste da barra de volume (e só então grava a escolha) */
static void menu_mouse_up(int x) { if (g_vol_drag) { g_vol_drag = 0; vol_arrastar(x, 1); } }
static void menu_mouse(int x, int y, int click) {
    if (!g_menu_on || g_menu_target == 0) return;
    if (g_vol_drag) {                                /* arrastando a barra de volume */
        if (!(SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK)) { menu_mouse_up(x); return; }
        vol_arrastar(x, 0); return;
    }
    int id = menu_hit(x, y); if (id < 0) return;
    if (g_menu_page == 0 && id < 100) {
        if (g_menu_sel != id) { g_menu_sel = id; g_menu_dirty = 1; if (!click) g_menu_confirm = -1; }
        if (click) menu_input(0, 0, 1, 0);
    } else if (g_menu_page == 3 && id >= 300) {
        int k = id - 300;
        if (g_menu_sel != k) { g_menu_sel = k; g_menu_dirty = 1; }
        if (click) menu_input(0, 0, 1, 0);
    } else if (g_menu_page == 4 && id >= 100 && id < 200 + 400) {
        if (g_bind_ac >= 0) return;                  /* esperando tecla/botão */
        int k = id >= 400 ? (id - 400) / 2 : id - 100, col = id >= 400 ? (id - 400) % 2 : -1;
        if (g_menu_sel != k || (col >= 0 && g_ctl_col != col)) { g_menu_sel = k; if (col >= 0) g_ctl_col = col; g_menu_dirty = 1; }
        if (click) {
            int rows[AC_N + 4]; ctl_rows(rows); int it = rows[k];
            if (it < AC_N && col < 0) return;            /* (clique no nome da ação: só escolhe a linha) */
            if (it == CR_BOTOES || it == CR_VEL) {
                float s = g_ui_h / 1440.0f; if (g_ui_w / 2560.0f < s) s = g_ui_w / 2560.0f;
                int d = x < 128 * s + 1180 * s * 0.5f ? -1 : 1; g_menu_dirty = 1;
                if (it == CR_BOTOES) op_change(OP_BOTOES, d); else ctl_vel(d);
            } else menu_input(0, 0, 1, 0);
        }
    } else if (g_menu_page == 2 && id >= 200 && id < 300) {
        int k = id - 200;
        if (g_menu_sel != k) { g_menu_sel = k; g_menu_dirty = 1; }
        if (click) menu_input(0, 0, 1, 0);
    } else if (g_menu_page == 1 && id >= 100 && id < 200) {
        int k = id - 100;
        if (g_menu_sel != k) { g_menu_sel = k; g_menu_dirty = 1; }
        if (click) {
            int rows[OP_N]; op_rows(rows); int it = rows[k];
            if (it == OP_VOLUME) {                   /* clique na barra (ou perto dela): pula para lá e começa a arrastar */
                float s = g_ui_h / 1440.0f; if (g_ui_w / 2560.0f < s) s = g_ui_w / 2560.0f;
                if (x >= g_vol_x0 - 30 * s && x <= g_vol_x1 + 30 * s) { g_vol_drag = 1; vol_arrastar(x, 0); }
            } else if (it < OP_REINICIAR) {                 /* clique na metade esquerda volta, na direita avança */
                float s = g_ui_h / 1440.0f; if (g_ui_w / 2560.0f < s) s = g_ui_w / 2560.0f;
                op_change(it, x < 128 * s + 980 * s * 0.75f ? (x < 128 * s + 980 * s * 0.5f ? -1 : 1) : 1);
            } else menu_input(0, 0, 1, 0);
        }
    }
}
/* controle: bordas dos botões já calculados (bits do 3DS) */
static void menu_pad(uint32_t b, int cx, int cy) {
    static uint32_t prev; static Uint32 rep_t; static int rep_dir;
    uint32_t e = b & ~prev; prev = b;
    int dv = 0, dh = 0;
    if (e & (1u << B_UP)) dv = -1; if (e & (1u << B_DOWN)) dv = 1;
    if (e & (1u << B_LEFT)) dh = -1; if (e & (1u << B_RIGHT)) dh = 1;
    int ad = cy > 100 ? -1 : cy < -100 ? 1 : 0, ah = cx > 100 ? 1 : cx < -100 ? -1 : 0;   /* analógico com repetição */
    int dir = ad ? 10 + ad : ah ? 20 + ah : 0; Uint32 now = SDL_GetTicks();
    if (dir && (dir != rep_dir || SDL_TICKS_PASSED(now, rep_t))) { if (ad) dv = ad; else dh = ah; rep_t = now + (dir != rep_dir ? 380 : 140); }
    rep_dir = dir;
    int ok = (e & (1u << B_A)) || (e & (1u << B_START)), back = (e & (1u << B_B)) != 0;
    if (dv || dh || ok || back) menu_input(dv, dh, ok, back);
}

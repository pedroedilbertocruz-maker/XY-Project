/* Lançador do Pokémon XY (port nativo de Pokémon X e Pokémon Y para PC).
 *
 * Abre em tela cheia, no mesmo estilo do menu do jogo (runtime/menu_ui.h): cores da versão da ROM (X azul,
 * Y vermelho), o nome do jogo no alto à esquerda e, no canto inferior direito, o cartucho da versão girando,
 * a ROM escolhida e uma barra de progresso discreta enquanto o jogo é preparado. Na primeira vez (ou ao trocar
 * a ROM) o jogo é recompilado para o PC; depois, abre na hora. Quando o jogo abre (com o menu dele, também em
 * tela cheia), o lançador some e espera; ele volta se o jogo pedir outra ROM (código 78) ou der erro.
 *
 *   - ROM: o .3ds DECIFRADO do próprio usuário: "Escolher ROM", arrastar e soltar, ou o .3ds arrastado para
 *     cima do .exe. As escolhas ficam em opcoes.txt (chave=valor, UTF-8), o mesmo arquivo do menu do jogo e do
 *     jogar.py. Nada do jogo é embutido aqui; o cartucho 3D vem da pasta cartuchos/ do projeto.
 *   - Teclas: setas e Enter; Esc cancela a preparação (ou fecha). Mouse: clique nos itens.
 *
 * Compilar (llvm-mingw): ver compilar.sh / compilar.bat.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "../runtime/menu_ui.h"

/* ------------------------------------------------------------------ textos: idioma do programa (0 português,
 * 1 English, 2 español, 3 日本語), escolhido na primeira vez e guardado em opcoes.txt ("idioma"). Não é o
 * idioma do jogo. */
static int g_lang;
#define TR(pt, en, es, ja) (g_lang == 1 ? (en) : g_lang == 2 ? (es) : g_lang == 3 ? (ja) : (pt))
static const char *const k_lang_cod[4] = { "pt", "en", "es", "ja" };
static const wchar_t *const k_lang_codw[4] = { L"pt", L"en", L"es", L"ja" };
static const char *const k_lang_nome[4] = { "Português", "English", "Español", "日本語" };

/* ------------------------------------------------------------------ estado */
enum { ST_INICIO, ST_PREP, ST_JOGO, ST_ERRO, ST_IDIOMA };
enum { B_ESCOLHER, B_JOGAR, B_TROCAR, B_TENTAR, B_DETALHES, B_REFAZER, B_IDIOMA, B_SAIR, B_L0, B_L1, B_L2, B_L3, B_N };   /* B_L*: os 4 idiomas */
#define WM_APP_LINE (WM_APP + 1)                     /* lParam: char* UTF-8 (liberar) */
#define WM_APP_DONE (WM_APP + 3)                     /* wParam: código de saída do jogar.py (ou 1000 = não iniciou, 1001 = interrompido) */
#define WM_APP_GAME (WM_APP + 4)                     /* o jogo abriu */

static HINSTANCE g_inst;
static HWND g_wnd;
static wchar_t g_root[MAX_PATH], g_opc[MAX_PATH];
static char g_root8[MAX_PATH * 3];
static wchar_t g_rom[MAX_PATH * 2];
static int g_state = ST_INICIO, g_sel, g_err_tools;
static char g_versao;                                /* 'X', 'Y' ou 0 */
static MuTema g_tema;
static MuCart g_cart; static int g_cart_ok; static char g_cart_v;
static float g_yaw; static DWORD g_t0, g_tlast;
static volatile LONG g_busy, g_stop;
static HANDLE g_job;
static int g_prog_a, g_prog_b;                       /* progresso da compilação (feitos, total) */
static char g_passo[400], g_erro[400];
static char *g_log; static size_t g_log_n, g_log_cap;
static int g_refazer;
static int g_idioma_ok;                              /* o idioma do programa já foi escolhido (opcoes.txt) */
/* desenho */
static HBITMAP g_dib; static uint32_t *g_px; static int g_W, g_H; static HDC g_mdc;
static MuCanvas g_base;                              /* fundo + textos (refeito quando algo muda) */
static int g_dirty = 1;
static RECT g_hit[B_N]; static int g_hit_on[B_N];
static float g_s;                                    /* escala (1 = tela de 1440 linhas) */
static float g_cart_cx, g_cart_cy, g_cart_sz;

/* ------------------------------------------------------------------ utilidades */
static void path_join(wchar_t *out, size_t n, const wchar_t *a, const wchar_t *b) { _snwprintf(out, n, L"%ls\\%ls", a, b); out[n - 1] = 0; }
static int file_exists(const wchar_t *p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static int tools_ok(void) {
    static const wchar_t *const need[] = { L"ferramentas\\python\\python.exe", L"ferramentas\\llvm-mingw\\bin\\clang.exe",
                                           L"ferramentas\\SDL2\\x86_64-w64-mingw32\\lib\\libSDL2.dll.a" };
    for (int i = 0; i < 3; i++) { wchar_t p[MAX_PATH]; path_join(p, MAX_PATH, g_root, need[i]); if (!file_exists(p)) return 0; }
    return 1;
}
static void to8(const wchar_t *w, char *o, int n) { WideCharToMultiByte(CP_UTF8, 0, w, -1, o, n, NULL, NULL); o[n - 1] = 0; }
static void to16(const char *s, wchar_t *o, int n) { MultiByteToWideChar(CP_UTF8, 0, s, -1, o, n); o[n - 1] = 0; }

/* ------------------------------------------------------------------ opcoes.txt (chave=valor, UTF-8) */
static int opc_get(const char *k, char *v, int n) {
    FILE *f = _wfopen(g_opc, L"rb"); if (!f) return 0;
    char l[2048]; size_t kl = strlen(k); int ok = 0;
    while (fgets(l, sizeof l, f)) {
        char *s = l; if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;
        if (!strncmp(s, k, kl) && s[kl] == '=') {
            s += kl + 1; s[strcspn(s, "\r\n")] = 0; snprintf(v, n, "%s", s); ok = 1;
        }
    }
    fclose(f); return ok;
}
static void opc_set(const char *k, const char *v) {
    char *buf = NULL; size_t n = 0;
    FILE *f = _wfopen(g_opc, L"rb");
    if (f) { fseek(f, 0, SEEK_END); long L = ftell(f); fseek(f, 0, SEEK_SET); if (L > 0 && L < 1 << 20) { buf = malloc((size_t)L + 1); n = fread(buf, 1, (size_t)L, f); buf[n] = 0; } fclose(f); }
    f = _wfopen(g_opc, L"wb"); if (!f) { free(buf); return; }
    size_t kl = strlen(k); int done = 0;
    for (char *l = buf; l && *l;) {
        char *e = strchr(l, '\n'); size_t len = e ? (size_t)(e - l) : strlen(l);
        while (len && (l[len - 1] == '\r')) len--;
        if (len > kl && !strncmp(l, k, kl) && l[kl] == '=') { if (!done) fprintf(f, "%s=%s\n", k, v); done = 1; }
        else if (len) { fwrite(l, 1, len, f); fputc('\n', f); }
        l = e ? e + 1 : l + strlen(l);
    }
    if (!done) fprintf(f, "%s=%s\n", k, v);
    fclose(f); free(buf);
}
/* primeira vez com este lançador: traz as escolhas do lancador.ini antigo e do jogo.txt do jogar.bat */
static void opc_migrar(void) {
    if (file_exists(g_opc)) return;
    wchar_t ini[MAX_PATH]; path_join(ini, MAX_PATH, g_root, L"lancador.ini");
    wchar_t w[MAX_PATH * 2]; char u[MAX_PATH * 6];
    GetPrivateProfileStringW(L"lancador", L"rom", L"", w, MAX_PATH * 2, ini);
    if (!w[0]) {
        wchar_t p[MAX_PATH]; path_join(p, MAX_PATH, g_root, L"jogo.txt");
        FILE *f = _wfopen(p, L"rb");
        if (f) { char b[2048]; size_t k = fread(b, 1, sizeof b - 1, f); fclose(f); b[k] = 0; char *s = b;
                 if ((unsigned char)s[0] == 0xEF) s += 3; s[strcspn(s, "\r\n")] = 0;
                 while (*s == '"') s++; char *e = s + strlen(s); while (e > s && e[-1] == '"') *--e = 0; to16(s, w, MAX_PATH * 2); }
    }
    if (w[0]) { to8(w, u, sizeof u); opc_set("rom", u); }
    GetPrivateProfileStringW(L"lancador", L"render", L"opengl", w, 16, ini); to8(w, u, sizeof u); opc_set("render", u);
    _snwprintf(w, 8, L"%d", GetPrivateProfileIntW(L"lancador", L"resolucao", 2, ini)); to8(w, u, sizeof u); opc_set("resolucao", u);
    _snwprintf(w, 8, L"%d", GetPrivateProfileIntW(L"lancador", L"filtro", 0, ini)); to8(w, u, sizeof u); opc_set("filtro", u);
    GetPrivateProfileStringW(L"lancador", L"botoes", L"posicao", w, 16, ini); to8(w, u, sizeof u); opc_set("botoes", u);
    GetPrivateProfileStringW(L"lancador", L"idioma", L"", w, 16, ini);
    if (w[0]) { to8(w, u, sizeof u); opc_set("idioma", u); }
}

/* versão pela identificação do título no cabeçalho da ROM (NCSD: 0x108; NCCH: 0x118) */
static char rom_versao(const wchar_t *rom) {
    FILE *f = _wfopen(rom, L"rb"); if (!f) return 0;
    unsigned char h[0x200]; size_t n = fread(h, 1, sizeof h, f); fclose(f);
    if (n < 0x120) return 0;
    uint64_t id = 0; int off = !memcmp(h + 0x100, "NCSD", 4) ? 0x108 : !memcmp(h + 0x100, "NCCH", 4) ? 0x118 : -1;
    if (off < 0) return 0;
    for (int i = 7; i >= 0; i--) id = id << 8 | h[off + i];
    return id == 0x0004000000055D00ull ? 'X' : id == 0x0004000000055E00ull ? 'Y' : 0;
}
static void set_rom(const wchar_t *rom) {
    wcsncpy(g_rom, rom, MAX_PATH * 2); g_rom[MAX_PATH * 2 - 1] = 0;
    wchar_t *s = g_rom; while (*s == L'"' || *s == L' ') s++;
    if (s != g_rom) memmove(g_rom, s, (wcslen(s) + 1) * sizeof(wchar_t));
    wchar_t *e = g_rom + wcslen(g_rom); while (e > g_rom && (e[-1] == L'"' || e[-1] == L' ')) *--e = 0;
    g_versao = g_rom[0] ? rom_versao(g_rom) : 0;
    g_tema = mu_tema(g_versao);
    if (g_versao && g_cart_v != g_versao) {
        if (g_cart_ok) { free(g_cart.v); free(g_cart.tex.px); }
        char a[MAX_PATH * 3 + 64], b[MAX_PATH * 3 + 64];
        snprintf(a, sizeof a, "%s\\cartuchos\\cartucho.malha", g_root8);
        snprintf(b, sizeof b, "%s\\cartuchos\\cartucho_%c.bmp", g_root8, g_versao == 'X' ? 'x' : 'y');
        g_cart_ok = mu_cart_load(&g_cart, a, b); g_cart_v = g_versao;
    }
    char u[MAX_PATH * 6]; to8(g_rom, u, sizeof u); opc_set("rom", u);
    g_dirty = 1;
}

/* ------------------------------------------------------------------ processos (console escondido) */
static void post_line(const char *s, int len) {
    if (len <= 0) return;
    char *c = malloc((size_t)len + 1); memcpy(c, s, (size_t)len); c[len] = 0;
    PostMessageW(g_wnd, WM_APP_LINE, 0, (LPARAM)c);
}
static int run_capture(wchar_t *cmd, DWORD *exit_code) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    si.hStdInput = nul; si.hStdOutput = wr; si.hStdError = wr;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NEW_CONSOLE | CREATE_SUSPENDED, NULL, g_root, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); char m[200]; snprintf(m, sizeof m, "não consegui iniciar (erro %lu)", GetLastError()); post_line(m, (int)strlen(m)); return 0; }
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread); CloseHandle(pi.hThread);
    char buf[4096], line[8192]; int ln = 0; DWORD got;
    while (ReadFile(rd, buf, sizeof buf, &got, NULL) && got) {
        for (DWORD i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') { post_line(line, ln); ln = 0; continue; }
            if (ln < (int)sizeof line - 1) line[ln++] = c;
        }
    }
    post_line(line, ln);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, exit_code);
    CloseHandle(pi.hProcess);
    return 1;
}
static DWORD WINAPI worker(LPVOID arg) {
    int refazer = (int)(INT_PTR)arg;
    DWORD code = 0;
    SetEnvironmentVariableW(L"IDIOMA", k_lang_codw[g_lang]);    /* mensagens do jogar.py e o idioma do jogo (menu, avisos) */
    SetEnvironmentVariableW(L"LANCADOR", L"1");                  /* jogar.py manda marcas de progresso */
    SetEnvironmentVariableW(L"FPS", L"30");
    SetEnvironmentVariableW(L"PYTHONUTF8", L"1");
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    SetEnvironmentVariableW(L"PYTHONUNBUFFERED", L"1");
    if (!tools_ok()) {
        const char *m = TR("== Baixando as ferramentas (só na primeira vez: Python portátil, compilador e SDL2)",
                           "== Downloading the tools (first time only: portable Python, compiler and SDL2)",
                           "== Descargando las herramientas (solo la primera vez: Python portátil, compilador y SDL2)",
                           "== ツールをダウンロード中（初回のみ：ポータブル版Python、コンパイラー、SDL2）");
        post_line(m, (int)strlen(m));
        wchar_t cmd[MAX_PATH * 2];
        _snwprintf(cmd, MAX_PATH * 2, L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%ls\\instalar.ps1\"", g_root);
        if (!run_capture(cmd, &code) || code != 0 || !tools_ok()) { g_err_tools = 1; PostMessageW(g_wnd, WM_APP_DONE, g_stop ? 1001 : 1000, 0); return 0; }
    }
    if (g_stop) { PostMessageW(g_wnd, WM_APP_DONE, 1001, 0); return 0; }
    wchar_t cmd[MAX_PATH * 6];
    _snwprintf(cmd, MAX_PATH * 6, L"\"%ls\\ferramentas\\python\\python.exe\" -u \"%ls\\tools\\jogar.py\" \"%ls\"%ls",
               g_root, g_root, g_rom, refazer ? L" --refazer" : L"");
    if (!run_capture(cmd, &code)) code = 1000;
    PostMessageW(g_wnd, WM_APP_DONE, g_stop ? 1001 : code, 0);
    return 0;
}

/* ------------------------------------------------------------------ ações */
static void log_add(const char *s) {
    size_t n = strlen(s);
    if (g_log_n + n + 3 > g_log_cap) { g_log_cap = (g_log_n + n + 3) * 2; g_log = realloc(g_log, g_log_cap); }
    memcpy(g_log + g_log_n, s, n); g_log_n += n; g_log[g_log_n++] = '\r'; g_log[g_log_n++] = '\n'; g_log[g_log_n] = 0;
}
static void start(int refazer) {
    if (g_busy || !g_rom[0]) return;
    if (!file_exists(g_rom)) { snprintf(g_erro, sizeof g_erro, "%s", TR("O arquivo da ROM não foi encontrado.", "The ROM file was not found.", "No se encontró el archivo de la ROM.", "ROMファイルが見つかりません。")); g_state = ST_ERRO; g_sel = B_ESCOLHER; g_dirty = 1; return; }
    wchar_t chk[MAX_PATH]; path_join(chk, MAX_PATH, g_root, L"tools\\jogar.py");
    if (!file_exists(chk)) { snprintf(g_erro, sizeof g_erro, "%s", TR("Este programa precisa ficar na pasta do projeto (ao lado de tools e runtime).", "This program must stay in the project folder (next to tools and runtime).", "Este programa debe quedarse en la carpeta del proyecto (junto a tools y runtime).", "このプログラムはプロジェクトのフォルダー（tools と runtime の隣）に置いてください。")); g_state = ST_ERRO; g_dirty = 1; return; }
    g_refazer = refazer; g_stop = 0; g_err_tools = 0; g_prog_a = g_prog_b = 0; g_log_n = 0; if (g_log) g_log[0] = 0;
    snprintf(g_passo, sizeof g_passo, "%s", TR("Preparando o jogo…", "Preparing the game…", "Preparando el juego…", "ゲームを準備中…"));
    InterlockedExchange(&g_busy, 1);
    g_state = ST_PREP; g_dirty = 1;
    HANDLE t = CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)refazer, 0, NULL); if (t) CloseHandle(t);
}
static void escolher(void) {
    if (g_busy) return;
    wchar_t file[MAX_PATH * 2]; wcscpy(file, g_rom);
    wchar_t filter[256];
    _snwprintf(filter, 256, L"%ls|*.3ds;*.cci;*.cxi|%ls|*.*|", TR(L"Jogo de 3DS decifrado (*.3ds, *.cci, *.cxi)", L"Decrypted 3DS game (*.3ds, *.cci, *.cxi)", L"Juego de 3DS descifrado (*.3ds, *.cci, *.cxi)", L"復号済みの3DSゲーム (*.3ds, *.cci, *.cxi)"), TR(L"Todos os arquivos", L"All files", L"Todos los archivos", L"すべてのファイル"));
    for (wchar_t *p = filter; *p; p++) if (*p == L'|') *p = 0;
    wchar_t dir[MAX_PATH * 2] = L"";
    if (file[0]) { wcscpy(dir, file); wchar_t *b = wcsrchr(dir, L'\\'); if (b) *b = 0; else dir[0] = 0; }
    OPENFILENAMEW of; memset(&of, 0, sizeof of);
    of.lStructSize = sizeof of; of.hwndOwner = g_wnd; of.lpstrFilter = filter;
    of.lpstrFile = file; of.nMaxFile = MAX_PATH * 2; of.lpstrTitle = TR(L"Escolha o arquivo .3ds do jogo", L"Choose the game's .3ds file", L"Elige el archivo .3ds del juego", L"ゲームの .3ds ファイルを選択");
    of.lpstrInitialDir = dir[0] ? dir : g_root;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&of)) { set_rom(file); start(0); }
}
static char g_relatorio[1024];                    /* relatório do último erro do jogo (jogar.py: @@RELATORIO) */
static void detalhes(void) {
    if (g_relatorio[0]) {                         /* o jogo caiu: abre o relatório do erro (tem o log junto) */
        wchar_t r[1024]; to16(g_relatorio, r, 1024);
        if (GetFileAttributesW(r) != INVALID_FILE_ATTRIBUTES) { ShellExecuteW(g_wnd, L"open", L"notepad.exe", r, NULL, SW_SHOWNORMAL); return; }
    }
    wchar_t p[MAX_PATH]; path_join(p, MAX_PATH, g_root, L"lancador.log");
    FILE *f = _wfopen(p, L"wb"); if (f) { static const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF }; fwrite(bom, 1, 3, f); if (g_log) fwrite(g_log, 1, g_log_n, f); fclose(f); }
    ShellExecuteW(g_wnd, L"open", L"notepad.exe", p, NULL, SW_SHOWNORMAL);
}
static void sair(void) {
    if (g_busy) { g_stop = 1; if (g_job) TerminateJobObject(g_job, 1); }
    DestroyWindow(g_wnd);
}
/* botões visíveis em cada estado, na ordem do foco */
static int botoes(int *b) {
    int n = 0;
    switch (g_state) {
    case ST_INICIO: if (g_rom[0] && g_versao) b[n++] = B_JOGAR; b[n++] = B_ESCOLHER; b[n++] = B_IDIOMA; b[n++] = B_SAIR; break;
    case ST_IDIOMA: for (int i = 0; i < 4; i++) b[n++] = B_L0 + i; break;
    case ST_PREP: break;
    case ST_ERRO: if (g_rom[0]) b[n++] = B_TENTAR; b[n++] = B_DETALHES; if (g_rom[0] && !g_err_tools) b[n++] = B_REFAZER; b[n++] = B_ESCOLHER; b[n++] = B_SAIR; break;
    }
    return n;
}
static void ativar(int id) {
    switch (id) {
    case B_ESCOLHER: escolher(); break;
    case B_JOGAR: {                                    /* som de "jogar" (cartuchos/jogar.wav, se existir) */
        wchar_t w[MAX_PATH]; path_join(w, MAX_PATH, g_root, L"cartuchos\\jogar.wav");
        if (file_exists(w)) PlaySoundW(w, NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        start(0); break; }
    case B_TENTAR: start(0); break;
    case B_REFAZER:
        if (MessageBoxW(g_wnd, TR(L"Recompilar o jogo inteiro do zero a partir da ROM?\n\nUse se o jogo estiver fechando sozinho ou se algo parecer quebrado depois de uma atualização. Leva de 10 a 40 minutos, como na primeira vez. O save não é apagado.", L"Rebuild the whole game from scratch from the ROM?\n\nUse it if the game keeps closing or something looks broken after an update. It takes 10 to 40 minutes, like the first time. Your save is not deleted.", L"¿Recompilar todo el juego desde cero a partir de la ROM?\n\nÚsalo si el juego se cierra solo o si algo parece roto después de una actualización. Tarda de 10 a 40 minutos, como la primera vez. La partida guardada no se borra.", L"ROMからゲーム全体を一から再コンパイルしますか？\n\nゲームが勝手に終了する場合や、更新後に何かがおかしい場合に使ってください。初回と同じく10〜40分かかります。セーブデータは削除されません。"),
                        L"XY Project", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) start(1);
        break;
    case B_DETALHES: detalhes(); break;
    case B_IDIOMA: g_state = ST_IDIOMA; g_sel = B_L0 + g_lang; g_dirty = 1; break;
    case B_L0: case B_L1: case B_L2: case B_L3: {        /* idioma escolhido: guarda e segue */
        int primeira = !g_idioma_ok;
        g_lang = id - B_L0; g_idioma_ok = 1; opc_set("idioma", k_lang_cod[g_lang]);
        g_state = ST_INICIO; g_sel = g_versao ? B_JOGAR : B_ESCOLHER; g_dirty = 1;
        if (primeira && g_rom[0] && g_versao && file_exists(g_rom)) start(0);   /* (como abriria se já tivesse escolhido) */
        break; }
    case B_SAIR: sair(); break;
    }
}

/* ------------------------------------------------------------------ desenho */
static const char *nome_jogo(void) { return g_versao == 'X' ? "Pokémon X" : g_versao == 'Y' ? "Pokémon Y" : "Pokémon XY"; }
static void hit(int id, float x, float y, float w, float h) { g_hit[id] = (RECT){ (LONG)x, (LONG)y, (LONG)(x + w), (LONG)(y + h) }; g_hit_on[id] = 1; }
static const char *rotulo(int id) {
    switch (id) {
    case B_ESCOLHER: return g_rom[0] ? TR("TROCAR ROM", "CHANGE ROM", "CAMBIAR ROM", "ROMを変更") : TR("ESCOLHER ROM", "CHOOSE ROM", "ELEGIR ROM", "ROMを選択");
    case B_JOGAR: return TR("JOGAR", "PLAY", "JUGAR", "プレイ");
    case B_TENTAR: return TR("TENTAR DE NOVO", "TRY AGAIN", "REINTENTAR", "もう一度");
    case B_DETALHES: return TR("VER DETALHES", "SHOW DETAILS", "VER DETALLES", "詳細を見る");
    case B_REFAZER: return TR("RECOMPILAR TUDO", "REBUILD EVERYTHING", "RECOMPILAR TODO", "すべて再コンパイル");
    case B_IDIOMA: return TR("IDIOMA", "LANGUAGE", "IDIOMA", "言語");
    case B_L0: case B_L1: case B_L2: case B_L3: return k_lang_nome[id - B_L0];
    case B_SAIR: return TR("SAIR", "QUIT", "SALIR", "終了");
    }
    return "";
}
static void build_base(void) {
    MuCanvas *c = &g_base; int W = g_W, H = g_H; float s = g_s;
    memset(g_hit_on, 0, sizeof g_hit_on);
    mu_clear(c, g_tema.base);
    mu_vgrad(c, 0, 0, W, H, mu_rgba(g_tema.base2, 0), mu_rgba(g_tema.base2, 255), 1);
    mu_glow(c, W * 0.80f, H * 0.66f, W * 0.48f, H * 0.62f, mu_rgba(g_tema.accent2, 120));
    mu_hgrad(c, 0, 0, (int)(W * 0.6f), H, mu_rgba(g_tema.base, 170), mu_rgba(g_tema.base, 0), 1);
    float ML = 128 * s;
    mu_text(c, FM_PEQ, 28 * s, ML, 176 * s, g_tema.accent, 7 * s, "XY PROJECT");
    mu_text(c, FM_TITULO, 150 * s, ML - 6 * s, 318 * s, g_tema.text, -2 * s, nome_jogo());
    mu_hgrad(c, (int)ML, (int)(352 * s), (int)(120 * s), (int)(6 * s), mu_rgba(g_tema.accent, 255), mu_rgba(g_tema.accent, 0), 0);
    /* menu à esquerda (como o do jogo) */
    int b[B_N], n = botoes(b);
    float y0 = 820 * s;
    if (g_state == ST_IDIOMA) {                          /* escolha do idioma do programa (o pedido, nas 4 línguas) */
        static const char *const pede[4] = { "Escolha o idioma do programa", "Choose the program language", "Elige el idioma del programa", "プログラムの言語を選んでください" };
        for (int i = 0; i < 4; i++) mu_text(c, FM_PEQ, 28 * s, ML, 560 * s + i * 44 * s, i == g_sel - B_L0 ? g_tema.text : mu_rgba(g_tema.dim, 230), 0, pede[i]);
        mu_text(c, FM_PEQ, 22 * s, ML, 760 * s, mu_rgba(g_tema.dim, 200), 0,
                TR("Isto não muda o idioma do jogo.", "This does not change the game's language.", "Esto no cambia el idioma del juego.", "ゲーム内の言語は変わりません。"));
        y0 = 900 * s;
    }
    if (g_state == ST_ERRO) {
        mu_text(c, FM_PEQ, 28 * s, ML, 640 * s, 0xFFFF6B6B, 4 * s, TR("ALGO DEU ERRADO", "SOMETHING WENT WRONG", "ALGO SALIÓ MAL", "問題が発生しました"));
        mu_text(c, FM_PEQ, 30 * s, ML, 700 * s, g_tema.text, 0, g_erro[0] ? g_erro : TR("Veja os detalhes.", "See the details.", "Mira los detalles.", "詳細を確認してください。"));
        if (g_relatorio[0])
            mu_text(c, FM_PEQ, 22 * s, ML, 746 * s, mu_rgba(g_tema.dim, 220), 0,
                    TR("Um relatório do erro foi salvo (VER DETALHES abre ele). Mande-o junto ao relatar o problema.", "An error report was saved (SHOW DETAILS opens it). Send it along when reporting the problem.",
                       "Se guardó un informe del error (VER DETALLES lo abre). Envíalo al reportar el problema.", "エラーレポートを保存しました（「詳細を見る」で開きます）。問題を報告するときに添付してください。"));
        y0 = 820 * s;
    } else if (g_state == ST_INICIO && !g_rom[0]) {
        mu_text(c, FM_PEQ, 30 * s, ML, 640 * s, g_tema.text, 0, TR("Escolha o arquivo .3ds do seu jogo (Pokémon X ou Y), ou arraste-o para esta tela.", "Choose your game's .3ds file (Pokémon X or Y), or drag it onto this screen.", "Elige el archivo .3ds de tu juego (Pokémon X o Y), o arrástralo a esta pantalla.", "ゲーム（Pokémon X または Y）の .3ds ファイルを選ぶか、この画面にドラッグしてください。"));
        mu_text(c, FM_PEQ, 24 * s, ML, 690 * s, mu_rgba(g_tema.dim, 230), 0, TR("Use apenas o seu próprio dump do jogo (decifrado).", "Use only your own dump of the game (decrypted).", "Usa solo tu propio dump del juego (descifrado).", "自分で吸い出した（復号済みの）ゲームだけを使ってください。"));
    }
    for (int k = 0; k < n; k++) {
        int id = b[k]; if (id == B_JOGAR) continue;           /* jogar = o cartucho */
        float y = y0; y0 += 96 * s;
        int sel = g_sel == id;
        if (sel) {
            mu_hgrad(c, (int)(ML - 40 * s), (int)(y - 54 * s), (int)(720 * s), (int)(76 * s), mu_rgba(g_tema.accent, 235), mu_rgba(g_tema.accent2, 0), 1);
            mu_fill(c, (int)(ML - 40 * s), (int)(y - 54 * s), (int)(6 * s), (int)(76 * s), mu_rgba(0xFFFFFF, 230));
        }
        mu_text(c, FM_ITEM, 42 * s, ML, y, sel ? g_tema.text : mu_rgba(g_tema.dim, 220), 3 * s, rotulo(id));
        hit(id, ML - 40 * s, y - 54 * s, 720 * s, 76 * s);
    }
    /* canto inferior direito: cartucho, rótulo, ROM e progresso */
    g_cart_sz = 460 * s; g_cart_cx = W - 330 * s; g_cart_cy = H - 540 * s;
    if (g_versao) {
        int fj = g_sel == B_JOGAR && g_state == ST_INICIO;
        mu_glow(c, g_cart_cx, g_cart_cy + 20 * s, (fj ? 380 : 340) * s, (fj ? 330 : 300) * s, mu_rgba(fj ? g_tema.accent : g_tema.accent2, fj ? 120 : 70));
        if (g_state == ST_INICIO) {
            const char *jl = TR("JOGAR", "PLAY", "JUGAR", "プレイ"); float jw = mu_text_w(FM_ITEM, 40 * s, 10 * s, jl);
            mu_text(c, FM_ITEM, 40 * s, g_cart_cx - jw / 2, H - 214 * s, fj ? g_tema.text : mu_rgba(g_tema.dim, 230), 10 * s, jl);
            if (fj) mu_hgrad(c, (int)(g_cart_cx - jw / 2), (int)(H - 198 * s), (int)jw, (int)(4 * s), mu_rgba(g_tema.accent, 255), mu_rgba(g_tema.accent2, 255), 0);
            hit(B_JOGAR, g_cart_cx - g_cart_sz * 0.55f, g_cart_cy - g_cart_sz * 0.6f, g_cart_sz * 1.1f, g_cart_sz * 1.25f);
        }
    }
    if (g_rom[0]) {
        const wchar_t *rn = g_rom; for (const wchar_t *p = g_rom; *p; p++) if (*p == L'\\' || *p == L'/') rn = p + 1;
        char r8[400]; to8(rn, r8, sizeof r8);
        float rw = mu_text_w(FM_PEQ, 22 * s, 0, r8);
        mu_text(c, FM_PEQ, 22 * s, W - 96 * s - rw, H - 74 * s, mu_rgba(g_tema.dim, 210), 0, r8);
    }
    if (g_state == ST_INICIO && !g_versao && g_rom[0])
        mu_text(c, FM_PEQ, 26 * s, ML, 700 * s, 0xFFFF6B6B, 0, TR("Esta ROM não parece ser Pokémon X nem Pokémon Y.", "This ROM does not look like Pokémon X or Pokémon Y.", "Esta ROM no parece ser Pokémon X ni Pokémon Y.", "このROMは Pokémon X でも Pokémon Y でもないようです。"));
    const char *dica = g_state == ST_IDIOMA ? "↑↓   Enter" : g_state == ST_PREP ? TR("Esc cancela", "Esc cancels", "Esc cancela", "Esc でキャンセル") : TR("↑↓ escolher   Enter confirmar   Esc sair", "↑↓ select   Enter confirm   Esc quit", "↑↓ elegir   Enter confirmar   Esc salir", "↑↓ 選択   Enter 決定   Esc 終了");
    float dw = mu_text_w(FM_PEQ, 22 * s, 0, dica); mu_text(c, FM_PEQ, 22 * s, W - 96 * s - dw, 120 * s, mu_rgba(g_tema.dim, 190), 0, dica);
}
/* progresso: barra fina e textos (por quadro, para a barra andar) */
static void draw_prog(MuCanvas *c) {
    float s = g_s; int W = g_W, H = g_H;
    float bw = 520 * s, bx = W - 96 * s - bw, by = H - 150 * s;
    char pct[32] = "";
    float f = g_prog_b > 0 ? (float)g_prog_a / g_prog_b : -1;
    mu_rrect(c, bx, by, bw, 6 * s, 3 * s, mu_rgba(0xFFFFFF, 40));
    if (f >= 0) {
        mu_rrect(c, bx, by, bw * f, 6 * s, 3 * s, g_tema.accent);
        snprintf(pct, sizeof pct, "%d%%", (int)(f * 100 + 0.5f));
    } else {                                           /* sem total: um trecho correndo */
        float t = (GetTickCount() - g_t0) / 1400.0f; t -= (int)t;
        float w = bw * 0.28f, x = bx + (bw + w) * t - w, x0 = x < bx ? bx : x, x1 = x + w > bx + bw ? bx + bw : x + w;
        if (x1 > x0) mu_rrect(c, x0, by, x1 - x0, 6 * s, 3 * s, g_tema.accent);
    }
    /* etapa: acima da barra, quebrada em até 2 linhas na largura da barra (com "…" se ainda sobrar);
     * a porcentagem fica embaixo da barra, à direita */
    float fs = 24 * s, lh = 34 * s;
    char linhas[2][420] = { "", "" }; int nl = 0;
    const char *p = g_passo;
    while (*p && nl < 2) {
        int best = 0, n = 0; char tmp[420];
        while (p[n] && n < 400) {                        /* avança palavra a palavra enquanto couber */
            int m = n; while (p[m] == ' ') m++; while (p[m] && p[m] != ' ') m++;
            memcpy(tmp, p, (size_t)m); tmp[m] = 0;
            if (mu_text_w(FM_PEQ, fs, 0, tmp) > bw && n > 0) break;
            n = m; best = m;
            if (mu_text_w(FM_PEQ, fs, 0, tmp) > bw) break;  /* uma palavra só, maior que a barra */
        }
        if (!best) break;
        memcpy(linhas[nl], p, (size_t)best); linhas[nl][best] = 0; nl++;
        p += best; while (*p == ' ') p++;
    }
    if (*p && nl == 2) {                                  /* sobrou texto: corta a 2ª linha com reticências */
        char *l = linhas[1]; size_t L = strlen(l);
        for (;;) {
            char t[440]; snprintf(t, sizeof t, "%s…", l);
            if (mu_text_w(FM_PEQ, fs, 0, t) <= bw || L == 0) { snprintf(linhas[1], sizeof linhas[1], "%s", t); break; }
            do L--; while (L > 0 && ((unsigned char)l[L] & 0xC0) == 0x80);   /* (não corta no meio de um caractere) */
            l[L] = 0; while (L > 0 && l[L - 1] == ' ') l[--L] = 0;
        }
    }
    for (int i = 0; i < nl; i++) mu_text(c, FM_PEQ, fs, bx, by - 22 * s - (nl - 1 - i) * lh, g_tema.text, 0, linhas[i]);
    if (pct[0]) { float pw = mu_text_w(FM_PEQ, fs, 0, pct); mu_text(c, FM_PEQ, fs, bx + bw - pw, by + 42 * s, g_tema.accent, 0, pct); }
}
static void frame(void) {
    if (!g_px || g_state == ST_JOGO) return;
    if (g_dirty) { build_base(); g_dirty = 0; }
    memcpy(g_px, g_base.px, (size_t)g_W * g_H * 4);
    MuCanvas c = { g_px, g_W, g_H };
    DWORD now = GetTickCount(); float dt = g_tlast ? (now - g_tlast) / 1000.0f : 0; g_tlast = now; if (dt > 0.1f) dt = 0.1f;
    g_yaw += dt * (g_state == ST_PREP ? 0.6f : 0.42f);
    if (g_cart_ok && g_versao) {
        float R = g_cart_sz > 420 ? 420 : g_cart_sz;   /* desenha a até 420 px e amplia */
        if (R >= g_cart_sz) mu_cart_draw(&c, &g_cart, g_cart_cx, g_cart_cy + 8 * sinf(now / 1000.0f * 1.6f) * g_s, g_cart_sz, g_yaw, -0.16f + sinf(now / 1000.0f * 1.1f) * 0.035f, g_sel == B_JOGAR ? 1.0f : 0);
        else {
            int S = (int)(R * 1.3f) + 4; MuCanvas t = { calloc((size_t)S * S, 4), S, S };
            if (t.px) {
                mu_cart_draw(&t, &g_cart, S / 2.0f, S / 2.0f, R, g_yaw, -0.16f + sinf(now / 1000.0f * 1.1f) * 0.035f, g_sel == B_JOGAR ? 1.0f : 0);
                float k = g_cart_sz / R; int D = (int)(S * k);
                int x0 = (int)(g_cart_cx - D / 2.0f), y0 = (int)(g_cart_cy - D / 2.0f + 8 * sinf(now / 1000.0f * 1.6f) * g_s);
                for (int j = 0; j < D; j++) { int y = y0 + j; if (y < 0 || y >= g_H) continue;
                    float sy = (j + 0.5f) / k - 0.5f; int iy = (int)sy; if (iy < 0) iy = 0; if (iy > S - 2) iy = S - 2; float fy = sy - iy;
                    for (int i = 0; i < D; i++) { int x = x0 + i; if (x < 0 || x >= g_W) continue;
                        float sx = (i + 0.5f) / k - 0.5f; int ix = (int)sx; if (ix < 0) ix = 0; if (ix > S - 2) ix = S - 2; float fx = sx - ix;
                        const uint32_t *p = t.px + (long)iy * S + ix;
                        uint32_t a = mu_lerp(p[0], p[1], (int)(fx * 255)), b = mu_lerp(p[S], p[S + 1], (int)(fx * 255)), v = mu_lerp(a, b, (int)(fy * 255));
                        if (v >> 24) g_px[(long)y * g_W + x] = mu_over(g_px[(long)y * g_W + x], v); } }
                free(t.px);
            }
        }
    }
    if (g_state == ST_PREP) draw_prog(&c);
    HDC dc = GetDC(g_wnd); BitBlt(dc, 0, 0, g_W, g_H, g_mdc, 0, 0, SRCCOPY); ReleaseDC(g_wnd, dc);
}

/* ------------------------------------------------------------------ mensagens */
static void on_line(char *w) {
    if (!strncmp(w, "@@PROGRESSO ", 12)) { int a = 0, b = 0; if (sscanf(w + 12, "%d %d", &a, &b) == 2 && b > 0) { g_prog_a = a; g_prog_b = b; } return; }
    if (!strcmp(w, "@@JOGO_ABERTO")) { g_relatorio[0] = 0; PostMessageW(g_wnd, WM_APP_GAME, 0, 0); return; }
    if (!strncmp(w, "@@RELATORIO ", 12)) { snprintf(g_relatorio, sizeof g_relatorio, "%s", w + 12); g_relatorio[strcspn(g_relatorio, "\r\n")] = 0; return; }
    if (!strncmp(w, "== ", 3)) { snprintf(g_passo, sizeof g_passo, "%s", w + 3); if (g_passo[0] >= 'a' && g_passo[0] <= 'z') g_passo[0] -= 32; g_prog_a = g_prog_b = 0; }
    log_add(w);
}
static void nav(int d) {
    int b[B_N], n = botoes(b); if (!n) return;
    int k = 0; for (int i = 0; i < n; i++) if (b[i] == g_sel) k = i;
    k = (k + d + n) % n; g_sel = b[k]; g_dirty = 1;
}
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_TIMER: frame(); return 0;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); if (g_mdc) BitBlt(dc, 0, 0, g_W, g_H, g_mdc, 0, 0, SRCCOPY); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR: SetCursor(LoadCursor(NULL, IDC_ARROW)); return TRUE;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (g_state == ST_PREP) { g_stop = 1; if (g_job) TerminateJobObject(g_job, 1); }
            else if (g_state == ST_IDIOMA && g_idioma_ok) { g_state = ST_INICIO; g_sel = B_IDIOMA; g_dirty = 1; }
            else sair();
            return 0; }
        if (g_state == ST_PREP) return 0;
        if (wp == VK_UP || wp == VK_LEFT) nav(-1);
        else if (wp == VK_DOWN || wp == VK_RIGHT || wp == VK_TAB) nav(1);
        else if (wp == VK_RETURN || wp == VK_SPACE) ativar(g_sel);
        return 0;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        for (int i = 0; i < B_N; i++) if (g_hit_on[i] && x >= g_hit[i].left && x < g_hit[i].right && y >= g_hit[i].top && y < g_hit[i].bottom) {
            if (g_sel != i) { g_sel = i; g_dirty = 1; }
            if (m == WM_LBUTTONDOWN) ativar(i);
            break;
        }
        return 0;
    }
    case WM_DROPFILES: {
        wchar_t f[MAX_PATH * 2];
        if (!g_busy && DragQueryFileW((HDROP)wp, 0, f, MAX_PATH * 2)) {
            set_rom(f);
            if (g_state == ST_IDIOMA) { }                    /* (primeiro o idioma; a ROM fica guardada) */
            else if (g_versao) start(0); else { g_state = ST_INICIO; g_sel = B_ESCOLHER; } }
        DragFinish((HDROP)wp); return 0;
    }
    case WM_APP_LINE: { char *w = (char *)lp; on_line(w); free(w); return 0; }
    case WM_APP_GAME: g_state = ST_JOGO; ShowWindow(h, SW_HIDE); return 0;   /* o jogo abre em tela cheia com o menu dele */
    case WM_APP_DONE: {
        InterlockedExchange(&g_busy, 0);
        int code = (int)wp;
        if (code == 0 && g_state == ST_JOGO) { DestroyWindow(h); return 0; }           /* o jogo foi fechado: fim */
        ShowWindow(h, SW_SHOW); SetForegroundWindow(h);
        g_dirty = 1;
        if (code == 78) { g_state = ST_INICIO; g_sel = B_ESCOLHER; frame(); escolher(); return 0; }   /* o menu do jogo pediu outra ROM */
        if (code == 1001) { g_state = ST_INICIO; g_sel = g_versao ? B_JOGAR : B_ESCOLHER; return 0; }   /* cancelado */
        g_state = ST_ERRO; g_sel = g_rom[0] ? B_TENTAR : B_ESCOLHER;
        if (g_err_tools) snprintf(g_erro, sizeof g_erro, "%s", TR("Não consegui baixar as ferramentas. Verifique a internet e tente de novo.", "Could not download the tools. Check your internet connection and try again.", "No pude descargar las herramientas. Revisa tu conexión a internet e inténtalo de nuevo.", "ツールをダウンロードできませんでした。インターネット接続を確認して、もう一度お試しください。"));
        else if (code == 3) snprintf(g_erro, sizeof g_erro, "%s", TR("O jogo fechou com um erro.", "The game closed with an error.", "El juego se cerró con un error.", "ゲームがエラーで終了しました。"));
        else snprintf(g_erro, sizeof g_erro, "%s", TR("A preparação do jogo falhou.", "Preparing the game failed.", "La preparación del juego falló.", "ゲームの準備に失敗しました。"));
        return 0;
    }
    case WM_CLOSE: sair(); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev; (void)cmdline; (void)show;
    g_inst = inst;
    SetProcessDPIAware();
    GetModuleFileNameW(NULL, g_root, MAX_PATH);
    { wchar_t *b = wcsrchr(g_root, L'\\'); if (b) *b = 0; }
    to8(g_root, g_root8, sizeof g_root8);
    path_join(g_opc, MAX_PATH, g_root, L"opcoes.txt");
    opc_migrar();
    char v[MAX_PATH * 6];
    g_idioma_ok = 0; g_lang = 0;
    if (opc_get("idioma", v, sizeof v)) for (int i = 0; i < 4; i++) if (!strcmp(v, k_lang_cod[i])) { g_lang = i; g_idioma_ok = 1; }
    if (!g_idioma_ok) {                                  /* primeira vez: a tela de idioma já vem marcando o do Windows */
        LANGID l = PRIMARYLANGID(GetUserDefaultUILanguage());
        g_lang = l == LANG_PORTUGUESE ? 0 : l == LANG_SPANISH ? 2 : l == LANG_JAPANESE ? 3 : 1;
    }
    mu_font_init();

    g_job = CreateJobObjectW(NULL, NULL);               /* se o lançador fechar, a preparação e o jogo fecham junto */
    if (g_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li; memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    /* tela cheia no monitor onde está o mouse */
    POINT pt; GetCursorPos(&pt);
    MONITORINFO mi = { sizeof mi }; GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
    g_W = mi.rcMonitor.right - mi.rcMonitor.left; g_H = mi.rcMonitor.bottom - mi.rcMonitor.top;
    g_s = g_H / 1440.0f; if (g_W / 2560.0f < g_s) g_s = g_W / 2560.0f;
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = g_W; bi.bmiHeader.biHeight = -g_H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    g_mdc = CreateCompatibleDC(NULL);
    g_dib = CreateDIBSection(g_mdc, &bi, DIB_RGB_COLORS, (void **)&g_px, NULL, 0);
    SelectObject(g_mdc, g_dib);
    g_base.px = malloc((size_t)g_W * g_H * 4); g_base.w = g_W; g_base.h = g_H;

    wchar_t rom[MAX_PATH * 2] = L"";
    if (opc_get("rom", v, sizeof v)) to16(v, rom, MAX_PATH * 2);
    int argc; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) { wcsncpy(rom, argv[1], MAX_PATH * 2); rom[MAX_PATH * 2 - 1] = 0; }   /* .3ds arrastado para cima do .exe */
    if (argv) LocalFree(argv);
    g_tema = mu_tema(0);
    if (rom[0]) set_rom(rom);
    g_sel = g_versao ? B_JOGAR : B_ESCOLHER;
    if (!g_idioma_ok) { g_state = ST_IDIOMA; g_sel = B_L0 + g_lang; }

    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc; wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = L"PokemonXYLancador";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(WS_EX_ACCEPTFILES | WS_EX_APPWINDOW, wc.lpszClassName, L"XY Project", WS_POPUP,
                            mi.rcMonitor.left, mi.rcMonitor.top, g_W, g_H, NULL, NULL, inst, NULL);
    g_t0 = GetTickCount();
    frame();
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
    SetTimer(g_wnd, 1, 33, NULL);
    /* ROM conhecida e da versão certa: já prepara/abre (o jogo abre direto no menu dele) */
    if (g_idioma_ok && g_rom[0] && g_versao && file_exists(g_rom)) start(0);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_job) CloseHandle(g_job);
    return 0;
}

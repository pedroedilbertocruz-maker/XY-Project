/* Janela no Windows (Win32 puro, sem bibliotecas extras): mostra as duas telas, uma em cima da outra,
 * e lê teclado, mouse (tela de toque) e controles XInput (Xbox e compatíveis).
 *
 * Teclado (mesmo padrão do Citra/Azahar):
 *   setas = direcional analógico      T F G H = direcional digital (cima/esq/baixo/dir)
 *   A = A   S = B   Z = X   X = Y     Q = L   W = R     Enter ou M = START   Backspace ou N = SELECT
 *   mouse (botão esquerdo) na tela de baixo = toque
 * Controle: botões na posição do 3DS (o de baixo é B, o da direita é A), analógico esquerdo = direcional
 * analógico, LB/RB = L/R, Start/Back = START/SELECT.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "plat.h"

int g_display_on;
volatile uint32_t g_host_buttons;
volatile int g_host_touch, g_host_touch_x, g_host_touch_y, g_host_circle_x, g_host_circle_y;

#define CW 400            /* tela de cima 400x240 sobre a de baixo 320x240 (centralizada) */
#define CH 480
static uint32_t g_canvas[CW * CH];
static CRITICAL_SECTION g_cs;
static HWND g_wnd;
static RECT g_dst;        /* onde a imagem foi desenhada na área cliente (para mapear o mouse) */
static uint32_t g_kb_buttons; static int g_kb_left, g_kb_right, g_kb_up, g_kb_down;
static int g_mouse_down;

enum { B_A = 0, B_B, B_SELECT, B_START, B_RIGHT, B_LEFT, B_UP, B_DOWN, B_R, B_L, B_X, B_Y };

/* ---------------------------------------------------------------- XInput (carregado dinamicamente) */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XPad;
typedef struct { DWORD dwPacketNumber; XPad Gamepad; } XState;
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XState *);
static XInputGetState_t pXInputGetState;

static void load_xinput(void) {
    const char *dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (int i = 0; i < 3 && !pXInputGetState; i++) {
        HMODULE m = LoadLibraryA(dlls[i]);
        if (m) pXInputGetState = (XInputGetState_t)(void *)GetProcAddress(m, "XInputGetState");
    }
}

static int axis(SHORT v) {
    int x = v;
    if (x > -7849 && x < 7849) return 0;          /* zona morta padrão do XInput */
    return x * 156 / 32767;
}

static void update_input(void) {
    uint32_t b = g_kb_buttons;
    int cx = (g_kb_right - g_kb_left) * 156, cy = (g_kb_up - g_kb_down) * 156;
    if (pXInputGetState) {
        for (DWORD i = 0; i < 4; i++) {
            XState st;
            if (pXInputGetState(i, &st) != 0) continue;
            WORD w = st.Gamepad.wButtons;
            if (w & 0x0001) b |= 1u << B_UP;
            if (w & 0x0002) b |= 1u << B_DOWN;
            if (w & 0x0004) b |= 1u << B_LEFT;
            if (w & 0x0008) b |= 1u << B_RIGHT;
            if (w & 0x0010) b |= 1u << B_START;
            if (w & 0x0020) b |= 1u << B_SELECT;
            if (w & 0x0100) b |= 1u << B_L;
            if (w & 0x0200) b |= 1u << B_R;
            if (w & 0x1000) b |= 1u << B_B;        /* botão de baixo */
            if (w & 0x2000) b |= 1u << B_A;        /* botão da direita */
            if (w & 0x4000) b |= 1u << B_Y;        /* botão da esquerda */
            if (w & 0x8000) b |= 1u << B_X;        /* botão de cima */
            if (st.Gamepad.bLeftTrigger > 100) b |= 1u << B_L;
            if (st.Gamepad.bRightTrigger > 100) b |= 1u << B_R;
            int ax = axis(st.Gamepad.sThumbLX), ay = axis(st.Gamepad.sThumbLY);
            if (ax || ay) { cx = ax; cy = ay; }
        }
    }
    g_host_buttons = b; g_host_circle_x = cx; g_host_circle_y = cy;
}

static int key_bit(WPARAM vk) {
    switch (vk) {
    case 'A': return B_A; case 'S': return B_B; case 'Z': return B_X; case 'X': return B_Y;
    case 'Q': return B_L; case 'W': return B_R;
    case VK_RETURN: case 'M': return B_START;
    case VK_BACK: case 'N': return B_SELECT;
    case 'T': return B_UP; case 'G': return B_DOWN; case 'F': return B_LEFT; case 'H': return B_RIGHT;
    }
    return -1;
}

/* ---------------------------------------------------------------- mouse -> tela de toque */
static void mouse_to_touch(int mx, int my, int down) {
    int w = g_dst.right - g_dst.left, h = g_dst.bottom - g_dst.top;
    if (!down || w <= 0 || h <= 0) { g_host_touch = 0; return; }
    int x = (mx - g_dst.left) * CW / w, y = (my - g_dst.top) * CH / h;
    int tx = x - 40, ty = y - 240;
    if (tx < 0 || tx >= 320 || ty < 0 || ty >= 240) { g_host_touch = 0; return; }
    g_host_touch_x = tx; g_host_touch_y = ty; g_host_touch = 1;
}

/* ---------------------------------------------------------------- janela */
static void paint(HWND w) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(w, &ps);
    RECT rc; GetClientRect(w, &rc);
    int cw = rc.right, ch = rc.bottom;
    int dw = cw, dh = cw * CH / CW;                     /* mantém a proporção 5:6 */
    if (dh > ch) { dh = ch; dw = ch * CW / CH; }
    int dx = (cw - dw) / 2, dy = (ch - dh) / 2;
    g_dst.left = dx; g_dst.top = dy; g_dst.right = dx + dw; g_dst.bottom = dy + dh;
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT r;
    if (dx > 0) { r.left = 0; r.top = 0; r.right = dx; r.bottom = ch; FillRect(dc, &r, black); r.left = dx + dw; r.right = cw; FillRect(dc, &r, black); }
    if (dy > 0) { r.left = 0; r.top = 0; r.right = cw; r.bottom = dy; FillRect(dc, &r, black); r.top = dy + dh; r.bottom = ch; FillRect(dc, &r, black); }
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = CW; bi.bmiHeader.biHeight = -CH;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, dw >= CW * 2 ? COLORONCOLOR : HALFTONE);
    EnterCriticalSection(&g_cs);
    StretchDIBits(dc, dx, dy, dw, dh, 0, 0, CW, CH, g_canvas, &bi, DIB_RGB_COLORS, SRCCOPY);
    LeaveCriticalSection(&g_cs);
    EndPaint(w, &ps);
}

static LRESULT CALLBACK wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
        int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        switch (wp) {
        case VK_LEFT: g_kb_left = down; break; case VK_RIGHT: g_kb_right = down; break;
        case VK_UP: g_kb_up = down; break; case VK_DOWN: g_kb_down = down; break;
        default: {
            int bit = key_bit(wp);
            if (bit >= 0) { if (down) g_kb_buttons |= 1u << bit; else g_kb_buttons &= ~(1u << bit); }
        }
        }
        update_input();
        if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) break;   /* Alt+F4 etc. continuam funcionando */
        return 0;
    }
    case WM_KILLFOCUS:                                   /* solta tudo ao trocar de janela */
        g_kb_buttons = 0; g_kb_left = g_kb_right = g_kb_up = g_kb_down = 0; update_input(); return 0;
    case WM_LBUTTONDOWN: g_mouse_down = 1; SetCapture(w); mouse_to_touch((short)LOWORD(lp), (short)HIWORD(lp), 1); return 0;
    case WM_MOUSEMOVE: if (g_mouse_down) mouse_to_touch((short)LOWORD(lp), (short)HIWORD(lp), 1); return 0;
    case WM_LBUTTONUP: g_mouse_down = 0; ReleaseCapture(); mouse_to_touch(0, 0, 0); return 0;
    case WM_TIMER: update_input(); return 0;
    case WM_CLOSE: case WM_DESTROY: ExitProcess(0); return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static DWORD WINAPI window_thread(LPVOID arg) {
    HANDLE ready = (HANDLE)arg;
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = L"PokemonXPort";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassW(&wc);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int ch = sh * 85 / 100; if (ch > CH * 2) ch = CH * 2; if (ch < CH) ch = CH;
    RECT r = { 0, 0, ch * CW / CH, ch };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_wnd = CreateWindowExW(0, wc.lpszClassName,
                            L"Pokémon X — setas: andar | A: A | S: B | Z: X | X: Y | Q/W: L/R | Enter: START | mouse: toque",
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                            NULL, NULL, inst, NULL);
    ShowWindow(g_wnd, SW_SHOW);
    SetTimer(g_wnd, 1, 8, NULL);                         /* lê o controle ~120 vezes por segundo */
    SetEvent(ready);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    ExitProcess(0);
    return 0;
}

void display_init(void) {
    if (getenv("SEM_JANELA")) return;
    typedef BOOL (WINAPI *dpi_t)(void);
    dpi_t dpi = (dpi_t)(void *)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDPIAware");
    if (dpi) dpi();
    InitializeCriticalSection(&g_cs);
    load_xinput();
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    CreateThread(NULL, 0, window_thread, ready, 0, NULL);
    WaitForSingleObject(ready, 5000);
    g_display_on = 1;
}

void display_frame(const uint32_t *top, const uint32_t *bottom) {
    EnterCriticalSection(&g_cs);
    memcpy(g_canvas, top, sizeof(uint32_t) * 400 * 240);
    for (int y = 0; y < 240; y++) {
        uint32_t *row = g_canvas + (240 + y) * CW;
        memset(row, 0, 40 * 4); memset(row + 360, 0, 40 * 4);
        memcpy(row + 40, bottom + y * 320, 320 * 4);
    }
    LeaveCriticalSection(&g_cs);
    if (g_wnd) InvalidateRect(g_wnd, NULL, FALSE);
}

void audio_push(const int16_t *samples, int frames) { (void)samples; (void)frames; }   /* som só no backend SDL */

/* a janela Win32 simples não tem OpenGL: renderer em software */
int gpu_gl_begin(void) { return 0; }
void gpu_gl_end(void) { }
void *gpu_gl_proc(const char *name) { (void)name; return 0; }

void display_frame_hd(const uint32_t *top, int sc) { (void)top; (void)sc; }
void display_frame_hd_bot(const uint32_t *bot, int sc) { (void)bot; (void)sc; }

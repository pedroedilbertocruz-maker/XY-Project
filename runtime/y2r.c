/* y2r:u — conversor de vídeo YUV -> RGB do 3DS (usado pelos vídeos .moflex).
 * Comandos e formatos: 3dbrew "Y2R Services". O jogo envia os planos Y/U/V (ou YUYV) por "DMA" em
 * blocos (SetSendingY/U/V), configura formato, rotação, alinhamento e coeficientes, e chama
 * StartConversion; a saída vai para o buffer de recepção (SetReceiving) e o evento de fim é sinalizado.
 */
#include "kernel.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct { uint32_t addr, size; uint16_t unit, gap; } Xfer;
static struct {
    uint32_t in_fmt, out_fmt, rotation, block, line_width, lines, alpha;
    int16_t coef[8];
    Xfer y, u, v, yuyv, dst;
    uint32_t end_event; int end_irq;
} g;

/* coeficientes padrão (3dbrew): ITU-R BT.601, BT.709 e variantes "escaladas" */
static const int16_t k_std[4][8] = {
    { 0x100, 0x166, 0xB6, 0x58, 0x1C5, -0x166F, 0x10EE, -0x1C5B },
    { 0x100, 0x193, 0x77, 0x2F, 0x1DB, -0x1933, 0xA7C, -0x1D51 },
    { 0x12A, 0x198, 0xD0, 0x64, 0x204, -0x1BDE, 0x10F2, -0x229B },
    { 0x12A, 0x1CA, 0x88, 0x36, 0x21C, -0x1F04, 0x99C, -0x2421 },
};

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* lê o byte i de um plano enviado em blocos (unidade de transferência + intervalo) */
static uint8_t xfer_byte(const Xfer *x, uint32_t i) {
    uint32_t unit = x->unit ? x->unit : 0xFFFFFFFFu;
    return MEM8(x->addr + i + (i / unit) * x->gap);
}

extern long g_probe_count;
static void convert(void) {
    int w = (int)g.line_width, h = (int)g.lines;
    if (!w || !h || !g.dst.addr) return;
    static int nconv; nconv++;
    if (getenv("Y2R_LOG"))
        fprintf(stderr, "[y2r] (sondas %ld) conversão %d: %dx%d entrada=%u saída=%u bloco=%u Y=%08x/%u U=%08x V=%08x -> %08x (unidade %u, intervalo %u)\n",
                g_probe_count, nconv, w, h, g.in_fmt, g.out_fmt, g.block, g.y.addr, g.y.size, g.u.addr, g.v.addr, g.dst.addr, g.dst.unit, g.dst.gap);
    FILE *dump = NULL;
    if (getenv("Y2R_DUMP") && nconv % (atoi(getenv("Y2R_DUMP")) > 0 ? atoi(getenv("Y2R_DUMP")) : 1) == 0) { char fn[64]; snprintf(fn, sizeof fn, "y2r_%03d.pgm", nconv); dump = fopen(fn, "wb"); }
    if (dump) {   /* plano Y como imagem em tons de cinza, para conferir o vídeo decodificado */
        fprintf(dump, "P5 %d %d 255\n", w, h);
        for (int i = 0; i < w * h; i++) fputc(g.in_fmt == 4 ? xfer_byte(&g.yuyv, i * 2) : xfer_byte(&g.y, i), dump);
        fclose(dump);
    }
    const int16_t *c = g.coef;
    int bpp = g.out_fmt == 0 ? 4 : g.out_fmt == 1 ? 3 : 2;
    uint32_t out_unit = g.dst.unit ? g.dst.unit : 0xFFFFFFFFu;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int Y, U, V;
            switch (g.in_fmt) {
            case 0: Y = xfer_byte(&g.y, y * w + x); U = xfer_byte(&g.u, y * (w / 2) + x / 2); V = xfer_byte(&g.v, y * (w / 2) + x / 2); break;  /* 4:2:2 */
            case 1: Y = xfer_byte(&g.y, y * w + x); U = xfer_byte(&g.u, (y / 2) * (w / 2) + x / 2); V = xfer_byte(&g.v, (y / 2) * (w / 2) + x / 2); break; /* 4:2:0 */
            case 4: { uint32_t b = (y * w + (x & ~1)) * 2; Y = xfer_byte(&g.yuyv, b + (x & 1) * 2); U = xfer_byte(&g.yuyv, b + 1); V = xfer_byte(&g.yuyv, b + 3); break; }
            default: Y = xfer_byte(&g.y, y * w + x); U = xfer_byte(&g.u, y * w + x); V = xfer_byte(&g.v, y * w + x); /* 4:4:4 etc. */
            }
            /* fórmula com o arredondamento do hardware (como na implementação de referência do Citra) */
            int cY = c[0] * Y;
            int r = ((cY + c[1] * V) >> 3) + c[5] + 0x18;
            int gg = ((cY - c[2] * V - c[3] * U) >> 3) + c[6] + 0x18;
            int b = ((cY + c[4] * U) >> 3) + c[7] - 0x18;
            r >>= 5; gg >>= 5; b >>= 5;
            uint8_t R = (uint8_t)clamp8(r), G = (uint8_t)clamp8(gg), B = (uint8_t)clamp8(b), A = (uint8_t)g.alpha;
            /* destino: linear ou em blocos 8x8 (ordem de Morton) */
            uint32_t idx;
            if (g.block) {
                int lx = x & 7, ly = y & 7;
                int m = (lx & 1) | ((ly & 1) << 1) | ((lx & 2) << 1) | ((ly & 2) << 2) | ((lx & 4) << 2) | ((ly & 4) << 3);
                idx = (uint32_t)(((y >> 3) * (w >> 3) + (x >> 3)) * 64 + m);
            } else idx = (uint32_t)(y * w + x);
            uint32_t off = idx * bpp;
            uint32_t a = g.dst.addr + off + (off / out_unit) * g.dst.gap;
            switch (g.out_fmt) {
            case 0: MEM8(a) = A; MEM8(a + 1) = B; MEM8(a + 2) = G; MEM8(a + 3) = R; break;
            case 1: MEM8(a) = B; MEM8(a + 1) = G; MEM8(a + 2) = R; break;
            case 2: MEM16(a) = (uint16_t)(((R >> 3) << 11) | ((G >> 3) << 6) | ((B >> 3) << 1) | (A >> 7)); break;
            default: MEM16(a) = (uint16_t)(((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3));
            }
        }
}

static void set_xfer(Xfer *x, uint32_t *cmd) { x->addr = cmd[1]; x->size = cmd[2]; x->unit = (uint16_t)cmd[3]; x->gap = (uint16_t)cmd[4]; }

int y2r(Ctx *c, uint32_t *cmd) {
    uint32_t id = cmd[0] >> 16;
    #define OK(n) do { cmd[0] = (id << 16) | ((n) << 6); cmd[1] = 0; } while (0)
    switch (id) {
    case 0x01: g.in_fmt = cmd[1]; OK(1); return 1;
    case 0x02: OK(2); cmd[2] = g.in_fmt; return 1;
    case 0x03: g.out_fmt = cmd[1]; OK(1); return 1;
    case 0x04: OK(2); cmd[2] = g.out_fmt; return 1;
    case 0x05: g.rotation = cmd[1]; OK(1); return 1;
    case 0x06: OK(2); cmd[2] = g.rotation; return 1;
    case 0x07: g.block = cmd[1]; OK(1); return 1;
    case 0x08: OK(2); cmd[2] = g.block; return 1;
    case 0x0D: g.end_irq = (int)cmd[1]; OK(1); return 1;
    case 0x0E: OK(2); cmd[2] = (uint32_t)g.end_irq; return 1;
    case 0x0F:                                         /* GetTransferEndEvent */
        if (!g.end_event) g.end_event = k_new(OBJ_EVENT, "y2r:fim");
        cmd[0] = 0x000F0042; cmd[1] = 0; cmd[2] = 0; cmd[3] = g.end_event; return 1;
    case 0x10: set_xfer(&g.y, cmd); OK(1); return 1;
    case 0x11: set_xfer(&g.u, cmd); OK(1); return 1;
    case 0x12: set_xfer(&g.v, cmd); OK(1); return 1;
    case 0x13: set_xfer(&g.yuyv, cmd); OK(1); return 1;
    case 0x14: case 0x15: case 0x16: case 0x17: case 0x19: OK(2); cmd[2] = 1; return 1;   /* terminou */
    case 0x18: set_xfer(&g.dst, cmd); OK(1); return 1;
    case 0x1A: g.line_width = cmd[1]; OK(1); return 1;
    case 0x1B: OK(2); cmd[2] = g.line_width; return 1;
    case 0x1C: g.lines = cmd[1]; OK(1); return 1;
    case 0x1D: OK(2); cmd[2] = g.lines; return 1;
    case 0x1E: memcpy(g.coef, &cmd[1], 16); OK(1); return 1;
    case 0x20: if (cmd[1] < 4) memcpy(g.coef, k_std[cmd[1]], 16); OK(1); return 1;
    case 0x22: g.alpha = cmd[1] & 0xFF; OK(1); return 1;
    case 0x23: OK(2); cmd[2] = g.alpha; return 1;
    case 0x26:                                         /* StartConversion */
        convert();
        if (g.end_event) k_signal(g.end_event);
        OK(1); return 1;
    case 0x27: OK(1); return 1;
    case 0x28: OK(2); cmd[2] = 0; return 1;            /* IsBusyConversion: não */
    case 0x29: {                                       /* SetPackageParameter (todos os parâmetros de uma vez) */
        uint8_t *p = (uint8_t *)&cmd[1];
        g.in_fmt = p[0]; g.out_fmt = p[1]; g.rotation = p[2]; g.block = p[3];
        g.line_width = *(uint16_t *)(p + 4); g.lines = *(uint16_t *)(p + 6);
        uint8_t std = p[8]; if (std < 4) memcpy(g.coef, k_std[std], 16);
        g.alpha = *(uint16_t *)(p + 10);
        OK(1); return 1;
    }
    case 0x2A: case 0x2B: case 0x2C: OK(1); return 1;  /* PingProcess / DriverInitialize / DriverFinalize */
    case 0x09: case 0x0B: case 0x24: OK(1); return 1;  /* dithering: ignorado */
    }
    return 0;
    #undef OK
}

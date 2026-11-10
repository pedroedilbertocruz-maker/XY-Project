/* Decodificador AAC do DSP (pipe binário 3). A música do jogo (sound/*.aac, ADTS) é decodificada pelo DSP
 * do 3DS: o jogo manda um pedido de 32 bytes {codec, comando, resultado, dados} e o DSP escreve PCM 16 bits,
 * um canal em cada endereço de destino. Formato da mensagem: o mesmo que o Citra/Azahar documentam
 * (audio_core/hle/decoder.h). A decodificação usa o FAAD2 (faad2/, GPL-2.0-or-later).
 *
 * Pedido "decodificar": +8 origem (física), +12 tamanho, +16 destino canal 0, +20 destino canal 1.
 * Resposta: +8 taxa (enum), +12 canais, +16 tamanho lido, +28 número de amostras por canal.
 */
#include "kernel.h"
#include "plat.h"
#include "faad2/include/neaacdec.h"
#include <stdio.h>
#include <string.h>

static uint32_t p2v(uint32_t p) {
    if (p >= 0x18000000u && p < 0x18600000u) return p + 0x07000000u;
    if (p >= 0x20000000u && p < 0x28000000u) return p - 0x0C000000u;
    return p;
}

static uint32_t rate_enum(unsigned long r) {
    switch (r) {
    case 48000: return 0; case 44100: return 1; case 32000: return 2; case 24000: return 3;
    case 22050: return 4; case 16000: return 5; case 12000: return 6; case 11025: return 7; default: return 8;
    }
}

static NeAACDecHandle g_dec; static int g_ready;
static void reset(void) {
    if (g_dec) NeAACDecClose(g_dec);
    g_dec = NeAACDecOpen(); g_ready = 0;
    NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(g_dec);
    cfg->outputFormat = FAAD_FMT_16BIT; cfg->downMatrix = 0;
    NeAACDecSetConfiguration(g_dec, cfg);
}

void dsp_aac_request(const uint8_t req[32], uint8_t resp[32]) {
    uint16_t codec, cmd; memcpy(&codec, req, 2); memcpy(&cmd, req + 2, 2);
    memset(resp, 0, 32);
    memcpy(resp, req, 4);                             /* mesmo codec e comando; resultado 0 = sucesso */
    if (codec != 1) return;                           /* só decodificação AAC */
    if (cmd == 0) { reset(); return; }                /* Init */
    if (cmd == 2) { if (g_dec) NeAACDecClose(g_dec); g_dec = NULL; return; }   /* Shutdown */
    if (cmd != 1) return;
    if (!g_dec) reset();
    uint32_t src, size, dst0, dst1;
    memcpy(&src, req + 8, 4); memcpy(&size, req + 12, 4); memcpy(&dst0, req + 16, 4); memcpy(&dst1, req + 20, 4);
    uint8_t *in = g_mem + p2v(src);
    plat_touch(in, size);
    unsigned long rate = 0; unsigned char ch = 0;
    if (!g_ready) {
        if (NeAACDecInit(g_dec, in, size, &rate, &ch) < 0) {
            static int warned; if (!warned++) fprintf(stderr, "[aac] não consegui iniciar o decodificador\n");
            return;
        }
        g_ready = 1;
    }
    NeAACDecFrameInfo info;
    int16_t *pcm = NeAACDecDecode(g_dec, &info, in, size);
    if (!pcm || info.error) {
        static int warned; if (warned++ < 3) fprintf(stderr, "[aac] erro: %s\n", NeAACDecGetErrorMessage(info.error));
        uint32_t z = 0; memcpy(resp + 16, &size, 4); memcpy(resp + 28, &z, 4);
        return;
    }
    uint32_t chans = info.channels ? info.channels : 1, frames = (uint32_t)(info.samples / chans);
    int16_t *o0 = (int16_t *)(g_mem + p2v(dst0)), *o1 = (int16_t *)(g_mem + p2v(dst1));
    plat_touch(o0, frames * 2);
    if (chans > 1 && dst1) plat_touch(o1, frames * 2);
    for (uint32_t i = 0; i < frames; i++) {
        o0[i] = pcm[i * chans];
        if (chans > 1 && dst1) o1[i] = pcm[i * chans + 1];
    }
    uint32_t re = rate_enum(info.samplerate), consumed = size;
    memcpy(resp + 8, &re, 4); memcpy(resp + 12, &chans, 4); memcpy(resp + 16, &consumed, 4); memcpy(resp + 28, &frames, 4);
}

/* ldr:ro em HLE: carregamento de módulos CRO.
 *
 * No console, LoadCRO mapeia o arquivo .cro num endereço escolhido pelo jogo, reajusta ("rebase")
 * o cabeçalho e as tabelas para endereços absolutos, aplica as relocações e liga os imports.
 * Aqui o código de cada CRO já foi recompilado e ligado estaticamente num endereço fixo
 * (tools/link.py). Então o LoadCRO:
 *   1. copia o arquivo para o endereço pedido (o jogo lê o cabeçalho e as tabelas de lá);
 *   2. reajusta o cabeçalho e as tabelas como o ldr real (offsets -> endereço pedido + offset);
 *   3. faz a tabela de segmentos apontar para o código/dados JÁ LIGADOS na nossa imagem, para que
 *      exports e o nnroControlObject_ levem às funções recompiladas;
 *   4. restaura os dados do módulo ao estado inicial (o jogo pode descarregar e recarregar);
 *   5. encadeia o módulo na lista do CRS (campos next/prev), como o ldr real.
 * Formato: 3dbrew "CRO0"; lógica de rebase e registro baseada na descrição do ldr no 3dbrew.
 */
#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char name[48];
    uint32_t base, file_size;
    uint32_t seg_addr[6], seg_size[6], seg_id[6];
    uint8_t *pristine_data; uint32_t data_addr, data_size, bss_addr, bss_size;
    uint32_t mapped;          /* endereço onde o jogo carregou (0 = não carregado) */
} Module;

static Module g_mods[160]; static int g_nmods;
static uint32_t g_crs;        /* endereço do CRS reajustado */
/* a janela usa para saber o contexto (batalha, mochila...) e arrumar a tela de baixo */
int ldr_loaded(const char *name) {
    for (int i = 0; i < g_nmods; i++) if (g_mods[i].mapped && !strcmp(g_mods[i].name, name)) return 1;
    return 0;
}
extern int g_ipc_trace;

static void load_modules(void) {
    if (g_nmods) return;
    FILE *f = fopen("modules.txt", "r");
    if (!f) { fprintf(stderr, "[ldr] modules.txt não encontrado (gere com tools/pack.py)\n"); exit(1); }
    Module m;
    while (fscanf(f, "%47s %u %u", m.name, &m.base, &m.file_size) == 3) {
        for (int i = 0; i < 6; i++) if (fscanf(f, "%u %u %u", &m.seg_addr[i], &m.seg_size[i], &m.seg_id[i]) != 3) exit(1);
        m.data_addr = m.data_size = m.bss_addr = m.bss_size = 0; m.pristine_data = NULL; m.mapped = 0;
        for (int i = 0; i < 6; i++) {
            if (m.seg_id[i] == 2 && m.seg_size[i]) { m.data_addr = m.seg_addr[i]; m.data_size = m.seg_size[i]; }
            if (m.seg_id[i] == 3 && m.seg_size[i]) { m.bss_addr = m.seg_addr[i]; m.bss_size = m.seg_size[i]; }
        }
        if (m.data_size) { m.pristine_data = malloc(m.data_size); memcpy(m.pristine_data, g_mem + m.data_addr, m.data_size); }
        g_mods[g_nmods++] = m;
    }
    fclose(f);
}

/* campos do cabeçalho que são offsets de tabelas/strings (somados ao endereço de carga) */
static const uint32_t k_header_offsets[] = {
    0x84, 0xB0, 0xB8, 0xC0, 0xC8, 0xD0, 0xD8, 0xE0, 0xE8, 0xF0, 0xF8, 0x100, 0x108, 0x110, 0x118, 0x120, 0x128, 0x130 };

static void rebase_tables(uint32_t a) {
    #define H(o) MEM32(a + (o))
    /* exports nomeados: {nome, tag} */
    for (uint32_t i = 0, t = H(0xD0); i < H(0xD4); i++) MEM32(t + i * 8) += a;
    /* módulos importados: {nome, tabela indexada, n, tabela anônima, n} */
    for (uint32_t i = 0, t = H(0xF0); i < H(0xF4); i++) {
        MEM32(t + i * 20) += a; MEM32(t + i * 20 + 4) += a; MEM32(t + i * 20 + 12) += a;
    }
    /* imports nomeados {nome, lote}, indexados {índice, lote}, anônimos {tag, lote} */
    for (uint32_t i = 0, t = H(0x100); i < H(0x104); i++) { MEM32(t + i * 8) += a; MEM32(t + i * 8 + 4) += a; }
    for (uint32_t i = 0, t = H(0x108); i < H(0x10C); i++) MEM32(t + i * 8 + 4) += a;
    for (uint32_t i = 0, t = H(0x110); i < H(0x114); i++) MEM32(t + i * 8 + 4) += a;
    #undef H
}

/* liga o módulo na lista encadeada do CRS (cabeçalho: +0x88 próximo, +0x8C anterior) */
static void register_module(uint32_t a, int auto_link) {
    if (!g_crs) return;
    uint32_t head = auto_link ? MEM32(g_crs + 0x88) : MEM32(g_crs + 0x8C);
    if (head) {
        uint32_t tail = MEM32(head + 0x8C);
        MEM32(a + 0x8C) = tail; MEM32(tail + 0x88) = a; MEM32(head + 0x8C) = a;
    } else {
        MEM32(a + 0x8C) = a;
        if (auto_link) MEM32(g_crs + 0x88) = a; else MEM32(g_crs + 0x8C) = a;
    }
    MEM32(a + 0x88) = 0;
}

static void unregister_module(uint32_t a) {
    if (!g_crs) return;
    for (int list = 0; list < 2; list++) {
        uint32_t *headp = (uint32_t *)(g_mem + g_crs + (list ? 0x8C : 0x88));
        uint32_t head = *headp;
        if (!head) continue;
        for (uint32_t m = head; m; m = MEM32(m + 0x88)) {
            if (m != a) continue;
            uint32_t next = MEM32(a + 0x88), prev = MEM32(a + 0x8C);
            if (a == head) { *headp = next; if (next) MEM32(next + 0x8C) = prev; }
            else { MEM32(prev + 0x88) = next; if (next) MEM32(next + 0x8C) = prev; else MEM32(head + 0x8C) = prev; }
            MEM32(a + 0x88) = MEM32(a + 0x8C) = 0;
            return;
        }
    }
}

int ldr(Ctx *c, uint32_t *cmd) {
    uint32_t id = cmd[0] >> 16;
    load_modules();
    switch (id) {
    case 0x0001: {                 /* Initialize(crs, tamanho, mapeado, desc, processo) */
        uint32_t src = cmd[1], size = cmd[2], dst = cmd[3];
        if (dst != src) memmove(g_mem + dst, g_mem + src, size);
        for (unsigned k = 0; k < sizeof k_header_offsets / 4; k++) {
            uint32_t o = k_header_offsets[k];
            if (o == 0xB0 || o == 0xB8) continue;      /* no CRS, código/dados já são absolutos */
            if (MEM32(dst + o) || o >= 0xC8) MEM32(dst + o) += dst;
        }
        rebase_tables(dst);
        MEM32(dst + 0x88) = MEM32(dst + 0x8C) = 0;
        g_crs = dst;
        fprintf(stderr, "[ldr] Initialize: CRS em %08x (%u bytes)\n", dst, size);
        cmd[0] = 0x00010040; cmd[1] = 0; return 1;
    }
    case 0x0002: cmd[0] = 0x00020040; cmd[1] = 0; return 1;                  /* LoadCRR */
    case 0x0003: cmd[0] = 0x00030040; cmd[1] = 0; return 1;                  /* UnloadCRR */
    case 0x0004: case 0x0009: {    /* LoadCRO(fonte, mapeado, tamanho, dados, 0, tam. dados, bss, tam. bss, autolink, fix, crr) */
        uint32_t src = cmd[1], dst = cmd[2], size = cmd[3];
        int auto_link = (int)cmd[9];
        char name[64] = {0};
        uint32_t no = MEM32(src + 0xC0), nl = MEM32(src + 0xC4);
        if (nl && nl < 63 && no < size) memcpy(name, g_mem + src + no, nl);
        Module *m = NULL;
        for (int i = 0; i < g_nmods; i++) if (!strcmp(g_mods[i].name, name)) m = &g_mods[i];
        { extern void pica_gl_q60_pause(int); if (!strcmp(name, "DllBattle")) pica_gl_q60_pause(240); }   /* 60 fps: abertura da batalha só com quadros reais */
        fprintf(stderr, "[ldr] LoadCRO \"%s\" fonte=%08x -> %08x (%u bytes, dados %08x/%u, bss %08x/%u)%s\n",
                name, src, dst, size, cmd[4], cmd[6], cmd[7], cmd[8], m ? "" : " — MÓDULO DESCONHECIDO");
        if (!m) { cmd[0] = (id << 16) | 0x80; cmd[1] = 0xD9012C11; cmd[2] = 0; return 1; }
        if (dst != src) memmove(g_mem + dst, g_mem + src, size);
        /* dados do módulo de volta ao estado inicial; bss zerado */
        if (m->data_size) memcpy(g_mem + m->data_addr, m->pristine_data, m->data_size);
        if (m->bss_size) memset(g_mem + m->bss_addr, 0, m->bss_size);
        /* rebase do cabeçalho e das tabelas para o endereço de carga */
        for (unsigned k = 0; k < sizeof k_header_offsets / 4; k++) MEM32(dst + k_header_offsets[k]) += dst;
        rebase_tables(dst);
        /* segmentos -> código e dados recompilados/ligados na nossa imagem */
        uint32_t segt = MEM32(dst + 0xC8), nseg = MEM32(dst + 0xCC);
        for (uint32_t i = 0; i < nseg && i < 6; i++) MEM32(segt + i * 12) = m->seg_addr[i];
        register_module(dst, auto_link);
        m->mapped = dst;
        cmd[0] = (id << 16) | 0x80; cmd[1] = 0; cmd[2] = size; return 1;
    }
    case 0x0005: {                 /* UnloadCRO(mapeado, 0, processo) */
        uint32_t a = cmd[1];
        for (int i = 0; i < g_nmods; i++) if (g_mods[i].mapped == a) {
            fprintf(stderr, "[ldr] UnloadCRO \"%s\"\n", g_mods[i].name); g_mods[i].mapped = 0;
            if (!strcmp(g_mods[i].name, "DllBattle")) { extern void nuzlocke_fim_batalha(void); nuzlocke_fim_batalha(); } }
        unregister_module(a);
        cmd[0] = 0x00050040; cmd[1] = 0; return 1;
    }
    case 0x0006: case 0x0007: case 0x0008:   /* LinkCRO / UnlinkCRO / Shutdown: já ligados estaticamente */
        cmd[0] = (id << 16) | 0x40; cmd[1] = 0; return 1;
    }
    return 0;
}

/* relatório de erro (crash.c): módulos CRO carregados agora */
void ldr_listar(FILE *o) {
    int n = 0;
    for (int i = 0; i < g_nmods; i++) if (g_mods[i].mapped) { fprintf(o, "%s%s", n ? ", " : "[crash] módulos: ", g_mods[i].name); n++; }
    fprintf(o, n ? "\n" : "[crash] módulos: nenhum\n");
}

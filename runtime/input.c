/* Entrada: atualiza a memória compartilhada do HID (botões, direcional analógico e tela de toque)
 * a cada ~1/60 s, no layout que a biblioteca hid do jogo lê (3dbrew "HID Shared Memory"):
 *   +0x00 ticks do último reinício do índice, +0x08 anterior, +0x10 índice da última entrada,
 *   +0x1C estado atual, +0x28 8 entradas {estado, apertados, soltos, analógico x/y} de 16 bytes;
 *   toque em +0xA8: ticks, ticks anterior, índice (+0xB8), entrada bruta (+0xC0), 8 entradas (+0xC8).
 * Fontes de entrada: a janela (display.c, quando compilada com SDL) e um roteiro em TECLAS:
 *   TECLAS="300:A,310:-,400:DOWN,410:-,500:T160x120,505:T-"   (quadro:botões; "-" solta; T = toque)
 */
#include "kernel.h"
#include "plat.h"
#include "idioma_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint32_t g_hid_shmem_addr;
extern long g_gsp_frames;

volatile uint32_t g_buttons;              /* bits do 3DS: A=0 B=1 SELECT=2 START=3 DIR=4 ESQ=5 CIMA=6 BAIXO=7 R=8 L=9 X=10 Y=11 */
volatile int g_touch_down, g_touch_x, g_touch_y;
volatile int g_circle_x, g_circle_y;

static uint32_t g_prev_buttons; static int g_pad_index = 7, g_touch_index = 7;

static uint32_t parse_buttons(const char *s) {
    static const struct { const char *n; uint32_t bit; } k[] = {
        { "A", 0 }, { "B", 1 }, { "SELECT", 2 }, { "START", 3 }, { "RIGHT", 4 }, { "LEFT", 5 }, { "UP", 6 }, { "DOWN", 7 },
        { "R", 8 }, { "L", 9 }, { "X", 10 }, { "Y", 11 } };
    uint32_t b = 0; char tmp[64]; snprintf(tmp, sizeof tmp, "%s", s);
    for (char *t = strtok(tmp, "+"); t; t = strtok(NULL, "+"))
        for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) if (!strcmp(t, k[i].n)) b |= 1u << k[i].bit;
    return b;
}

/* aplica os eventos do roteiro TECLAS cujo quadro já chegou */
static void scripted_input(void) {
    static char *script; static char *cur; static int init;
    if (!init) { init = 1; const char *e = getenv("TECLAS"); if (e) { script = strdup(e); cur = script; } }
    while (cur && *cur) {
        char *comma = strchr(cur, ','); char item[80];
        size_t len = comma ? (size_t)(comma - cur) : strlen(cur);
        if (len >= sizeof item) len = sizeof item - 1;
        memcpy(item, cur, len); item[len] = 0;
        long frame = atol(item); char *what = strchr(item, ':');
        if (!what || g_gsp_frames < frame) break;
        what++;
        if (what[0] == 'T') {
            if (what[1] == '-') g_touch_down = 0;
            else { g_touch_down = 1; g_touch_x = atoi(what + 1); char *x = strchr(what, 'x'); g_touch_y = x ? atoi(x + 1) : 0; }
        } else if (what[0] == '-') g_buttons = 0;
        else g_buttons = parse_buttons(what);
        fprintf(stderr, "[input] quadro %ld: %s\n", g_gsp_frames, what);
        cur = comma ? comma + 1 : NULL;
    }
}

/* entrada ao vivo: se existir o arquivo teclas_agora.txt (ex.: "A" ou "DOWN+B" ou "-"), aplica e apaga */
static void live_input(void) {
    static int tick; if (++tick % 6) return;
    { FILE *m = fopen("memoria_agora.txt", "r");      /* depuração: retrato da memória do jogo (nome no arquivo) */
      if (m) { char nm[256] = {0}; if (!fgets(nm, sizeof nm, m)) nm[0] = 0; fclose(m); remove("memoria_agora.txt");
               nm[strcspn(nm, "\r\n")] = 0; if (!nm[0]) snprintf(nm, sizeof nm, "mem_%ld", g_gsp_frames);
               static const uint32_t rg[][2] = { { 0x00100000u, 0x00600000u }, { 0x08000000u, 0x04000000u } };
               char p2[300]; snprintf(p2, sizeof p2, "%s.bin", nm); FILE *o = fopen(p2, "wb");
               if (o) { for (int i = 0; i < 2; i++) fwrite(g_mem + rg[i][0], 1, rg[i][1], o); fclose(o); }
               fprintf(stderr, "[input] memória salva em %s (quadro %ld)\n", p2, g_gsp_frames); } }
    FILE *f = fopen("teclas_agora.txt", "r"); if (!f) return;
    char line[64] = {0}; if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); remove("teclas_agora.txt");
    line[strcspn(line, "\r\n")] = 0;
    if (line[0] == 'T') {                          /* toque: T160x120 / T- */
        if (line[1] == '-') g_touch_down = 0;
        else { g_touch_down = 1; g_touch_x = atoi(line + 1); char *x = strchr(line, 'x'); g_touch_y = x ? atoi(x + 1) : 0; }
    } else if (line[0] == '-' || !line[0]) g_buttons = 0; else g_buttons = parse_buttons(line);
    fprintf(stderr, "[input] ao vivo, quadro %ld: %s\n", g_gsp_frames, line);
}

/* tela de idioma do jogo (só sem save): confirma sozinha o idioma escolhido no menu (services.c) */
int ldr_loaded(const char *name); int idioma_jogo_auto(void);
int g_lang_select;                                   /* 1 enquanto a tela de idioma está aberta (o menu a esconde) */
static uint32_t lang_auto(void) {
    static long t0 = -1; static int on = -1;
    if (on < 0) on = idioma_jogo_auto();
    g_lang_select = ldr_loaded("DllLangSelect");
    if (!on || !g_lang_select) { t0 = -1; return 0; }
    if (t0 < 0) { t0 = g_gsp_frames; fprintf(stderr, "[idioma] tela de idioma do jogo: confirmando sozinho\n"); }
    long d = g_gsp_frames - t0;
    return d >= 90 && (d - 90) % 40 < 6 ? 1u << 0 : 0;      /* A por 6 quadros a cada 40 */
}

void shared_time_update(void);

/* ---- Nuzlocke Mode (opção do menu; NUZLOCKE=1): Pokémon que desmaia em batalha é apagado.
 * No fim de cada batalha (ldr.c avisa quando o módulo DllBattle sai da memória), a equipe é procurada na memória
 * do jogo (a equipe viva, com 6 ponteiros para os Pokémon e a contagem, e a cópia que vai para o save, com 6 vagas
 * de 0x104 bytes no formato PK6; tudo cifrado e conferido pela soma de verificação) e quem está com 0 de HP sai da equipe (as outras sobem). O mesmo Pokémon também sai da equipe do
 * save no disco (services.c), para que reiniciar não o traga de volta. Se todos desmaiaram, o primeiro fica
 * (a equipe não pode ficar vazia) e o jogo segue para o Centro Pokémon, como sempre. Ovos não contam. */
volatile int g_opt_nuzlocke;
int g_ui_lang = -1;                                 /* idioma do programa (idioma_ui.h) */
void (*g_ui_aviso)(const char *msg);                 /* display_sdl.c: aviso na tela */
static long g_nz_fim = -1;
static const char *const k_ordem[24] = { "ABCD","ABDC","ACBD","ACDB","ADBC","ADCB","BACD","BADC","BCAD","BCDA","BDAC","BDCA",
                                         "CABD","CADB","CBAD","CBDA","CDAB","CDBA","DABC","DACB","DBAC","DBCA","DCAB","DCBA" };
static void pk6_cripto(uint8_t *d, const uint8_t *s, uint32_t ec) {   /* (cifrar = decifrar: xor com o mesmo fluxo) */
    int ini[2] = { 8, 0xE8 }, fim[2] = { 0xE8, 0x104 };
    memcpy(d, s, 8);
    for (int r = 0; r < 2; r++) {
        uint32_t seed = ec;
        for (int i = ini[r]; i < fim[r]; i += 2) {
            seed = seed * 0x41C64E6Du + 0x6073u;
            uint16_t v = (uint16_t)(s[i] | s[i + 1] << 8) ^ (uint16_t)(seed >> 16);
            d[i] = (uint8_t)v; d[i + 1] = (uint8_t)(v >> 8);
        }
    }
}
static int pk6_valido(const uint8_t *dec) {
    uint16_t ck = 0; for (int i = 8; i < 0xE8; i += 2) ck += (uint16_t)(dec[i] | dec[i + 1] << 8);
    return ck == (uint16_t)(dec[6] | dec[7] << 8) && ck != 0;
}
static const uint8_t *pk6_bloco(const uint8_t *dec, char b) {   /* bloco A..D já decifrado */
    uint32_t ec = (uint32_t)dec[0] | dec[1] << 8 | dec[2] << 16 | (uint32_t)dec[3] << 24;
    const char *o = k_ordem[((ec >> 13) & 31) % 24];
    return dec + 8 + (strchr(o, b) - o) * 0x38;
}
int nuzlocke_save_remover(const uint32_t *ecs, int n);   /* services.c */
/* uma equipe na memória: 6 vagas, cada uma com a parte principal (0xE8 bytes) e a de batalha (0x1C) cifradas */
typedef struct { uint32_t core[6], calc[6], cont; } NzEquipe;
static uint32_t g_nz_mortos[32]; static int g_nz_nm;  /* chaves (EC) de quem desmaiou nesta batalha */
static int nz_ler(const NzEquipe *e, int k, uint8_t *raw, uint8_t *dec) {
    memcpy(raw, g_mem + e->core[k], 0xE8); memcpy(raw + 0xE8, g_mem + e->calc[k], 0x1C);
    pk6_cripto(dec, raw, (uint32_t)raw[0] | raw[1] << 8 | raw[2] << 16 | (uint32_t)raw[3] << 24);
    return pk6_valido(dec);
}
static int nz_valida(uint32_t a) { return a >= 0x08000000u && a + 0x200 < 0x0C000000u; }
static int nz_morto(uint32_t ec) { for (int i = 0; i < g_nz_nm; i++) if (g_nz_mortos[i] == ec) return 1; return 0; }
/* a equipe viva: 6 ponteiros para os Pokémon (cada um aponta para a sua parte principal em +0x40 e a de batalha em
 * +0x198) e a contagem (um byte) em +0x18 */
static int nz_viva(uint32_t a, NzEquipe *e) {
    if (a < 0x08000000u || a + 0x20 >= 0x0C000000u || (a & 3)) return 0;
    uint32_t c = MEM8(a + 0x18); if (c < 1 || c > 6) return 0;
    for (int k = 0; k < 6; k++) {
        uint32_t p = MEM32(a + 4 * k);
        if (!nz_valida(p) || MEM32(p + 4) != p + 0x40 || MEM32(p + 8) != p + 0x198) return 0;
        e->core[k] = p + 0x40; e->calc[k] = p + 0x198;
    }
    e->cont = a + 0x18; return 1;
}
/* A memória tem várias cópias da equipe (as da batalha, restos de telas como a do PC, a que vai para o save) e
 * algumas ficam velhas: um Pokémon curado no Centro pode continuar com 0 de HP numa cópia esquecida. A verdadeira é
 * a que o jogo aponta de um lugar fixo; esse lugar é aprendido no mapa, onde só a verdadeira é apontada. */
static uint32_t g_nz_dono[8]; static int g_nz_ndono;
static int nz_real(NzEquipe *e);
static void nz_aprender(void) {
    uint32_t cand[32]; int nc = 0; NzEquipe e;
    if (nz_real(&e)) return;                            /* o lugar já aprendido continua valendo */
    for (uint32_t a = 0x08000000u; a + 0x20 < 0x0C000000u && nc < 32; a += 4) if (nz_viva(a, &e)) cand[nc++] = a;
    if (!nc) return;
    uint32_t lo = cand[0], hi = cand[nc - 1], refs[32][8]; int nr[32] = {0};
    for (uint32_t a = 0x08000000u; a < 0x0C000000u; a += 4) {
        uint32_t v = MEM32(a); if (v < lo || v > hi) continue;
        for (int i = 0; i < nc; i++) if (cand[i] == v && nr[i] < 8) refs[i][nr[i]++] = a;
    }
    int m = -1; for (int i = 0; i < nc; i++) if (nr[i] && (m < 0 || nr[i] > nr[m])) m = i;
    if (m < 0) return;
    g_nz_ndono = nr[m]; memcpy(g_nz_dono, refs[m], sizeof(uint32_t) * (size_t)nr[m]);
    fprintf(stderr, "[nuzlocke] equipe verdadeira em %08x (apontada de %d lugar(es), %d cópia(s) na memória)\n", cand[m], nr[m], nc);
}
static int nz_real(NzEquipe *e) {                       /* a equipe que os "donos" aprendidos apontam */
    uint32_t alvo = 0; int votos = 0;
    for (int i = 0; i < g_nz_ndono; i++) {
        uint32_t t = MEM32(g_nz_dono[i]); NzEquipe x; if (!nz_viva(t, &x)) continue;
        int v = 0; for (int j = 0; j < g_nz_ndono; j++) if (MEM32(g_nz_dono[j]) == t) v++;
        if (v > votos) { votos = v; alvo = t; }
    }
    return alvo && nz_viva(alvo, e);
}
/* passa por todas as equipes na memória: a viva (6 ponteiros para Pokémon + contagem) e a cópia que vai para o save
 * (6 vagas de 0x104 bytes seguidas + contagem) */
static int nz_achar(int (*fn)(const NzEquipe *, void *), void *ctx) {
    int achou = 0;
    for (uint32_t a = 0x08000000u; a + 0x61C < 0x0C000000u; a += 4) {
        NzEquipe e;
        uint32_t c1 = MEM8(a + 0x18);          /* a contagem é um byte (os outros 3 podem ter outras coisas) */
        if (c1 >= 1 && c1 <= 6) {
            int ok = 1;
            for (int k = 0; k < 6 && ok; k++) {
                uint32_t p = MEM32(a + 4 * k);
                if (!nz_valida(p) || MEM32(p + 4) != p + 0x40 || MEM32(p + 8) != p + 0x198) ok = 0;
                else { e.core[k] = p + 0x40; e.calc[k] = p + 0x198; }
            }
            if (ok) { e.cont = a + 0x18; if (fn(&e, ctx) >= 0) achou++; continue; }
        }
        uint32_t c2 = MEM8(a + 0x618);
        if (c2 < 1 || c2 > 6 || MEM16(a + 4) != 0) continue;
        for (int k = 0; k < 6; k++) { e.core[k] = a + k * 0x104; e.calc[k] = e.core[k] + 0xE8; }
        e.cont = a + 0x618;
        if (fn(&e, ctx) >= 0) achou++;
    }
    return achou;
}
/* anota quem está com 0 de HP (ovos não contam) */
static int nz_anotar(const NzEquipe *e, void *ctx) {
    (void)ctx; uint32_t cnt = MEM8(e->cont); if (cnt < 1 || cnt > 6) return -1;
    uint8_t raw[0x104], dec[0x104];
    for (uint32_t k = 0; k < cnt; k++) if (!nz_ler(e, (int)k, raw, dec)) return -1;
    for (uint32_t k = 0; k < cnt; k++) {
        nz_ler(e, (int)k, raw, dec);
        int ovo = (pk6_bloco(dec, 'B')[0x34 + 3] >> 6) & 1;
        uint16_t hp = (uint16_t)(dec[0xF0] | dec[0xF1] << 8);
        uint32_t ec = MEM32(e->core[k]);
        if (!hp && !ovo && !nz_morto(ec) && g_nz_nm < 32) g_nz_mortos[g_nz_nm++] = ec;
    }
    return 0;
}
typedef struct { uint32_t ecs[6]; int nec; char nomes[200]; int removidos; uint32_t meus[36]; int nmeus; } NzRes;
/* os Pokémon do jogador: os da cópia da equipe que vai para o save (as equipes dos adversários não usam esse formato) */
static int nz_meus(const NzEquipe *e, void *ctx) {
    NzRes *r = ctx; uint32_t cnt = MEM8(e->cont);
    if (e->cont != e->core[0] + 0x618 || cnt < 1 || cnt > 6) return -1;
    uint8_t raw[0x104], dec[0x104];
    for (uint32_t k = 0; k < cnt; k++) if (!nz_ler(e, (int)k, raw, dec)) return -1;
    for (uint32_t k = 0; k < cnt && r->nmeus < 36; k++) r->meus[r->nmeus++] = MEM32(e->core[k]);
    return 0;
}
/* tira da equipe quem foi anotado (mesmo que o jogo já tenha curado, como depois de perder a batalha) */
static int nz_remover(const NzEquipe *e, void *ctx) {
    NzRes *r = ctx;
    uint32_t cnt = MEM8(e->cont); if (cnt < 1 || cnt > 6) return -1;
    uint8_t raw[6][0x104], dec[0x104];
    for (uint32_t k = 0; k < cnt; k++) if (!nz_ler(e, (int)k, raw[k], dec)) return -1;
    int minha = 0;                                          /* só mexe em equipe que tem Pokémon do jogador */
    for (uint32_t k = 0; k < cnt; k++) for (int i = 0; i < r->nmeus; i++) if (r->meus[i] == MEM32(e->core[k])) minha = 1;
    if (!minha) return -1;
    int manter[6], nm = 0;
    for (uint32_t k = 0; k < cnt; k++) if (!nz_morto(MEM32(e->core[k]))) manter[nm++] = (int)k;
    if (nm == (int)cnt) return 0;
    if (nm == 0) manter[nm++] = 0;                          /* a equipe não pode ficar vazia: o primeiro fica */
    for (uint32_t k = 0; k < cnt; k++) {                    /* nomes e chaves de quem sai */
        int fica = 0; for (int i = 0; i < nm; i++) if (manter[i] == (int)k) fica = 1;
        uint32_t ec = MEM32(e->core[k]); if (fica) continue;
        int novo = 1; for (int i = 0; i < r->nec; i++) if (r->ecs[i] == ec) novo = 0;
        if (!novo || r->nec >= 6) continue;
        r->ecs[r->nec++] = ec;
        nz_ler(e, (int)k, raw[k], dec);
        const uint8_t *b = pk6_bloco(dec, 'B'); char nm8[40] = ""; int j = 0;      /* apelido (UTF-16 -> UTF-8) */
        for (int c = 0; c < 12; c++) {
            uint16_t ch = (uint16_t)(b[2 * c] | b[2 * c + 1] << 8); if (!ch) break;
            if (ch < 0x80) nm8[j++] = (char)ch;
            else if (ch < 0x800) { nm8[j++] = (char)(0xC0 | ch >> 6); nm8[j++] = (char)(0x80 | (ch & 0x3F)); }
            else { nm8[j++] = (char)(0xE0 | ch >> 12); nm8[j++] = (char)(0x80 | (ch >> 6 & 0x3F)); nm8[j++] = (char)(0x80 | (ch & 0x3F)); }
        }
        nm8[j] = 0;
        if (strlen(r->nomes) + j + 3 < sizeof r->nomes) { if (r->nomes[0]) strcat(r->nomes, "\n"); strcat(r->nomes, nm8); }
    }
    static const uint8_t z[0x104]; uint8_t vazia[0x104]; pk6_cripto(vazia, z, 0);   /* vaga vazia: zeros cifrados */
    uint8_t novo[6][0x104];
    for (int k = 0; k < 6; k++) memcpy(novo[k], k < nm ? raw[manter[k]] : vazia, 0x104);
    for (int k = 0; k < 6; k++) { memcpy(g_mem + e->core[k], novo[k], 0xE8); memcpy(g_mem + e->calc[k], novo[k] + 0xE8, 0x1C); }
    MEM8(e->cont) = (uint8_t)nm;
    r->removidos += (int)cnt - nm;
    return 0;
}
/* ldr.c: o módulo da batalha está saindo. Neste instante o jogo já devolveu o HP da batalha para a equipe, mas ainda
 * não curou ninguém (perder a batalha cura todo mundo logo depois), então é aqui que se anota quem desmaiou. */
/* HP atual de um Pokémon (pela chave EC) na equipe; -1 se não está nela. *ovo = 1 se for ovo */
static int nz_hp(const NzEquipe *e, uint32_t ec, int *ovo) {
    uint32_t cnt = MEM8(e->cont); uint8_t raw[0x104], dec[0x104];
    for (uint32_t k = 0; k < cnt && k < 6; k++) {
        if (MEM32(e->core[k]) != ec || !nz_ler(e, (int)k, raw, dec)) continue;
        if (ovo) *ovo = (pk6_bloco(dec, 'B')[0x34 + 3] >> 6) & 1;
        return dec[0xF0] | dec[0xF1] << 8;
    }
    return -1;
}
static int g_nz_derrota;                                /* a equipe inteira desmaiou (o jogo cura todo mundo depois) */
/* olha a equipe verdadeira: anota quem está com 0 de HP e se a equipe inteira caiu. Roda a cada quadro da batalha
 * até a checagem no mapa, porque o jogo devolve o HP da batalha para a equipe e (se você perdeu) cura todo mundo em
 * momentos que variam de um PC para outro. */
static int nz_vigiar(void) {
    NzEquipe e; if (!nz_real(&e)) return 0;
    nz_anotar(&e, NULL);
    int vivos = 0, nao_ovos = 0; uint32_t cnt = MEM8(e.cont);
    for (uint32_t k = 0; k < cnt; k++) { int ovo = 0; int hp = nz_hp(&e, MEM32(e.core[k]), &ovo); if (!ovo) { nao_ovos++; if (hp > 0) vivos++; } }
    if (nao_ovos && !vivos) g_nz_derrota = 1;
    return 1;
}
void nuzlocke_fim_batalha(void) {
    if (!g_opt_nuzlocke) return;
    int n0 = g_nz_nm;
    if (!nz_vigiar()) fprintf(stderr, "[nuzlocke] equipe verdadeira não encontrada no fim da batalha: ninguém é apagado\n");
    fprintf(stderr, "[nuzlocke] fim de batalha: %d com 0 de HP anotado(s) até agora\n", g_nz_nm); (void)n0;
    g_nz_fim = g_gsp_frames;
}
static void nuzlocke_checar(void) {
    NzEquipe ev;
    if (!nz_real(&ev)) { g_nz_nm = 0; g_nz_derrota = 0; fprintf(stderr, "[nuzlocke] equipe verdadeira não encontrada: ninguém é apagado\n"); return; }
    nz_anotar(&ev, NULL);                                   /* (se o HP só voltou para a equipe depois) */
    /* conferência final: só sai quem está na equipe com 0 de HP agora, ou quem estava com 0 no fim de uma batalha
     * perdida (aí o jogo já curou todo mundo). Ovos nunca. */
    int n = 0;
    for (int i = 0; i < g_nz_nm; i++) {
        int ovo = 0, hp = nz_hp(&ev, g_nz_mortos[i], &ovo);
        if (hp < 0 || ovo || (hp > 0 && !g_nz_derrota)) {
            fprintf(stderr, "[nuzlocke] %08x não sai (HP %d%s)\n", g_nz_mortos[i], hp, ovo ? ", ovo" : "");
            continue;
        }
        g_nz_mortos[n++] = g_nz_mortos[i];
    }
    g_nz_nm = n; g_nz_derrota = 0;
    if (!g_nz_nm) { fprintf(stderr, "[nuzlocke] ninguém desmaiou\n"); return; }
    NzRes r; memset(&r, 0, sizeof r);
    nz_achar(nz_meus, &r);
    int achou = nz_achar(nz_remover, &r);
    g_nz_nm = 0;
    if (r.removidos && r.nec) {
        nuzlocke_save_remover(r.ecs, r.nec);
        char lista[220] = ""; int q = 0;                    /* "<apelido> foi embora pra sempre" (vários: "A, B e C foram...") */
        for (char *n = r.nomes; n && *n; q++) {
            char *f = strchr(n, '\n'); if (f) *f = 0;
            if (q) strcat(lista, f ? TR(", ", ", ", ", ", "、") : TR(" e ", " and ", " y ", "と"));
            strncat(lista, n, sizeof lista - strlen(lista) - 1);
            n = f ? f + 1 : NULL;
        }
        char msg[260];
        if (q > 1) snprintf(msg, sizeof msg, TR("%s foram embora pra sempre", "%s are gone forever", "%s se fueron para siempre", "%sは 永遠に 去っていった"), lista);
        else snprintf(msg, sizeof msg, TR("%s foi embora pra sempre", "%s is gone forever", "%s se fue para siempre", "%sは 永遠に 去っていった"), lista);
        fprintf(stderr, "[nuzlocke] %s (equipe(s) na memória: %d)\n", msg, achou);
        if (g_ui_aviso) g_ui_aviso(msg);
    } else fprintf(stderr, "[nuzlocke] só o primeiro desmaiou e a equipe não pode ficar vazia: ele fica\n");
}
/* "reiniciar jogo" do menu (RESET_DIRETO): pula a abertura e o título apertando A até a tela de continuar abrir */
static uint32_t reset_auto(void) {
    static int on = -1, fim; static long t0 = -1;
    if (on < 0) on = getenv("RESET_DIRETO") != NULL;
    if (!on || fim) return 0;
    if (ldr_loaded("DllStartMenu") || g_gsp_frames > 6000) { fim = 1; fprintf(stderr, "[input] reinício: tela de continuar\n"); return 0; }
    if (!ldr_loaded("DllTitle")) { t0 = -1; return 0; }
    if (t0 < 0) t0 = g_gsp_frames;
    long d = g_gsp_frames - t0;
    return d >= 60 && (d - 60) % 30 < 5 ? 1u << 0 : 0;     /* A por 5 quadros a cada 30 */
}

void hid_update(void) {
    { static long t_campo = -1; static int aprendido;      /* Nuzlocke: aprende onde fica a equipe ao entrar no mapa */
      if (!ldr_loaded("DllField")) { t_campo = -1; aprendido = 0; }
      else if (t_campo < 0) t_campo = g_gsp_frames;
      else if (!aprendido && g_gsp_frames - t_campo > 60 && g_opt_nuzlocke && g_nz_fim < 0) { aprendido = 1; nz_aprender(); } }
    if (g_opt_nuzlocke && (g_nz_fim >= 0 || ldr_loaded("DllBattle"))) nz_vigiar();   /* durante e logo depois da batalha */
    if (g_nz_fim >= 0 && g_gsp_frames - g_nz_fim > 20 && ldr_loaded("DllField")) { g_nz_fim = -1; if (g_opt_nuzlocke) nuzlocke_checar(); }
    { static uint64_t last; uint64_t t = k_now_ns(); if (t - last > 60000000000ull) { last = t; shared_time_update(); } }   /* relógio do PC, 1x por minuto */
    scripted_input();
    live_input();
    uint32_t auto_a = lang_auto() | reset_auto();
    uint32_t m = g_hid_shmem_addr;
    if (!m) return;
    uint64_t ticks = k_now_ns() / 1000 * 268111856ull / 1000000ull;
    /* ---- botões */
    uint32_t b = g_buttons | g_host_buttons | auto_a;         /* roteiro + teclado/controle (+ tela de idioma) */
    int cx = g_circle_x + g_host_circle_x, cy = g_circle_y + g_host_circle_y;
    if (cx > 156) cx = 156; if (cx < -156) cx = -156; if (cy > 156) cy = 156; if (cy < -156) cy = -156;
    int touch = g_touch_down || g_host_touch;
    int tx = g_host_touch ? g_host_touch_x : g_touch_x, ty = g_host_touch ? g_host_touch_y : g_touch_y;
    if (cx > 40) b |= 1u << 28; if (cx < -40) b |= 1u << 29; if (cy > 40) b |= 1u << 30; if (cy < -40) b |= 1u << 31;
    g_pad_index = (g_pad_index + 1) & 7;
    if (g_pad_index == 0) { MEM32(m + 0x08) = MEM32(m); MEM32(m + 0x0C) = MEM32(m + 4); MEM32(m) = (uint32_t)ticks; MEM32(m + 4) = (uint32_t)(ticks >> 32); }
    uint32_t e = m + 0x28 + g_pad_index * 16;
    MEM32(e) = b; MEM32(e + 4) = b & ~g_prev_buttons; MEM32(e + 8) = g_prev_buttons & ~b;
    MEM16(e + 12) = (uint16_t)(int16_t)cx; MEM16(e + 14) = (uint16_t)(int16_t)cy;
    MEM32(m + 0x1C) = b;
    MEM32(m + 0x10) = (uint32_t)g_pad_index;
    g_prev_buttons = b;
    /* ---- toque (coordenadas da tela de baixo, 320x240) */
    uint32_t t = m + 0xA8;
    g_touch_index = (g_touch_index + 1) & 7;
    if (g_touch_index == 0) { MEM32(t + 8) = MEM32(t); MEM32(t + 0xC) = MEM32(t + 4); MEM32(t) = (uint32_t)ticks; MEM32(t + 4) = (uint32_t)(ticks >> 32); }
    uint32_t te = t + 0x20 + g_touch_index * 8;
    MEM16(te) = (uint16_t)(touch ? tx : 0); MEM16(te + 2) = (uint16_t)(touch ? ty : 0);
    MEM32(te + 4) = touch ? 1 : 0;
    MEM32(t + 0x10) = (uint32_t)g_touch_index;
    MEM16(t + 0x18) = MEM16(te); MEM16(t + 0x1A) = MEM16(te + 2); MEM32(t + 0x1C) = MEM32(te + 4);
}

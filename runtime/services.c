/* Serviços do sistema do 3DS em HLE (srv:, APT, hid, ...). Cada sessão é um objeto do kernel
 * com o nome do serviço; SendSyncRequest cai em ipc(), que despacha pelo nome. */
#include "kernel.h"
#include "plat.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int g_ipc_trace = -1;   /* LOG_IPC=0 desliga o registro de cada chamada de serviço */

/* memória compartilhada do HID (onde o jogo lê botões e touch) */
uint32_t g_hid_shmem_handle, g_hid_shmem_addr;

/* ---------------- srv: ---------------- */
static int srv(Ctx *c, uint32_t *cmd) {
    uint32_t id = cmd[0] >> 16;
    switch (id) {
    case 0x0001: cmd[0] = 0x00010040; cmd[1] = 0; return 1;                 /* RegisterClient */
    case 0x0002: {                                                          /* EnableNotification */
        uint32_t h = k_new(OBJ_SEMAPHORE, "srv:notificação"); k_get(h)->max = 64;
        cmd[0] = 0x00020042; cmd[1] = 0; cmd[2] = 0; cmd[3] = h; return 1;
    }
    case 0x0005: {                                                          /* GetServiceHandle */
        char name[9] = {0}; memcpy(name, &cmd[1], 8);
        fprintf(stderr, "[srv] GetServiceHandle(\"%s\")\n", name);
        cmd[0] = 0x00050042; cmd[1] = 0; cmd[2] = 0; cmd[3] = k_new(OBJ_SESSION, name); return 1;
    }
    case 0x0009: case 0x000A: cmd[0] = (id << 16) | 0x40; cmd[1] = 0; return 1;  /* Subscribe / Unsubscribe */
    case 0x000B: cmd[0] = 0x000B0080; cmd[1] = 0; cmd[2] = 0; return 1;           /* ReceiveNotification */
    }
    return 0;
}

/* ---------------- APT: gerenciador de aplicativos ----------------
 * Ao abrir um app, o sistema entrega o parâmetro "Wakeup" e sinaliza o evento de parâmetro. */
static int g_apt_wakeup_pending = 1;
static uint32_t g_apt_lock, g_apt_notify_event, g_apt_param_event;
static int apt(Ctx *c, uint32_t *cmd) {
    uint32_t id = cmd[0] >> 16;
    switch (id) {
    case 0x0001:                                   /* GetLockHandle */
        if (!g_apt_lock) g_apt_lock = k_new(OBJ_MUTEX, "apt:lock");
        cmd[0] = 0x000100C2; cmd[1] = 0; cmd[2] = 0; cmd[3] = 0; cmd[4] = 0; cmd[5] = g_apt_lock; return 1;
    case 0x0002:                                   /* Initialize -> eventos de notificação e de parâmetro */
        if (!g_apt_notify_event) {
            g_apt_notify_event = k_new(OBJ_EVENT, "apt:notificação");
            g_apt_param_event = k_new(OBJ_EVENT, "apt:parâmetro");
        }
        cmd[0] = 0x00020043; cmd[1] = 0; cmd[2] = 0x04000000; cmd[3] = g_apt_notify_event; cmd[4] = g_apt_param_event;
        return 1;
    case 0x0003:                                   /* Enable: a partir daqui o Wakeup pode ser entregue */
        cmd[0] = 0x00030040; cmd[1] = 0;
        if (g_apt_wakeup_pending && g_apt_param_event) k_get(g_apt_param_event)->signaled = 1;
        return 1;
    case 0x0009: cmd[0] = 0x00090080; cmd[1] = 0; cmd[2] = 1; return 1;      /* IsRegistered */
    case 0x000B: cmd[0] = 0x000B0080; cmd[1] = 0; cmd[2] = 0; return 1;      /* InquireNotification: nada */
    case 0x000D: case 0x000E: {                    /* ReceiveParameter / GlanceParameter */
        if (!g_apt_wakeup_pending) { cmd[0] = (id << 16) | 0x40; cmd[1] = 0xC8A0CFEF; return 1; }
        if (id == 0x000D) g_apt_wakeup_pending = 0;
        fprintf(stderr, "[apt] entregando o parâmetro Wakeup ao jogo\n");
        uint32_t buf = MEM32(c->tls + 0x184);
        cmd[0] = (id << 16) | 0x104; cmd[1] = 0; cmd[2] = 0x101; cmd[3] = 1; cmd[4] = 0;
        cmd[5] = 0x10; cmd[6] = 0; cmd[7] = 2; cmd[8] = buf;
        return 1;
    }
    case 0x0022: case 0x0027:
        fprintf(stderr, "[apt] o jogo pediu para fechar (cmd %04x)\n", id);
        cmd[0] = (id << 16) | 0x40; cmd[1] = 0; return 1;
    case 0x0044:                                   /* GetSharedFont */
        /* bloco da fonte: [0] estado (2 = carregada), [1] região, [2] tamanho; a fonte em si fica em +0x80.
         * Ainda não temos uma fonte real: só marcamos como carregada. */
        MEM32(0x18000000u) = 2; MEM32(0x18000004u) = 1; MEM32(0x18000008u) = 0;
        cmd[0] = 0x00440082; cmd[1] = 0; cmd[2] = 0x18000000u; cmd[3] = 0; cmd[4] = k_new(OBJ_SHMEM, "apt:fonte"); return 1;
    case 0x004B: cmd[0] = 0x004B0082; cmd[1] = 0; cmd[2] = 0; cmd[3] = 0; cmd[4] = 0; return 1; /* AppletUtility */
    case 0x0050: cmd[0] = 0x00500080; cmd[1] = 0; cmd[2] = 0; return 1;     /* GetApplicationCpuTimeLimit */
    }
    return 0;
}

/* ---------------- hid: controle, touch, sensores ---------------- */
static uint32_t g_hid_events[5];
static int hid(Ctx *c, uint32_t *cmd) {
    uint32_t id = cmd[0] >> 16;
    if (id == 0x000A) {                            /* GetIPCHandles: memória compartilhada + 5 eventos */
        if (!g_hid_shmem_handle) {
            g_hid_shmem_handle = k_new(OBJ_SHMEM, "hid:memória");
            g_hid_events[0] = k_new_periodic_event("hid:pad0", 16666667);   /* ~60 Hz */
            g_hid_events[1] = k_new_periodic_event("hid:pad1", 16666667);
            g_hid_events[2] = k_new(OBJ_EVENT, "hid:acelerômetro");
            g_hid_events[3] = k_new(OBJ_EVENT, "hid:giroscópio");
            g_hid_events[4] = k_new(OBJ_EVENT, "hid:debugpad");
            extern void hid_update(void);
            k_add_periodic(hid_update, 16666667);   /* atualiza botões/toque na memória compartilhada */
        }
        cmd[0] = 0x000A0007; cmd[1] = 0; cmd[2] = 0x14000000; cmd[3] = g_hid_shmem_handle;
        for (int i = 0; i < 5; i++) cmd[4 + i] = g_hid_events[i];
        return 1;
    }
    return 0;
}

/* ---------------- fs:USER ----------------
 * RomFS: imagem virtual "level 3" (tools/romfs_build.py). Os metadados ficam em memória e os dados
 * vêm dos arquivos extraídos (variável ROMFS = pasta romfs, ROMFS_META = pasta com romfs_meta.bin). */
#include <stdlib.h>
typedef struct { uint64_t off, size; char path[520]; int missing_logged; } RomfsEntry;
static uint8_t *g_romfs_meta; static uint64_t g_romfs_meta_len;
static RomfsEntry *g_romfs; static int g_romfs_n;
static uint64_t g_romfs_size;
static const char *g_romfs_dir;

/* Modo .3ds: JOGO_3DS=arquivo e JOGO_3DS_ROMFS="offset tamanho" (de tools/ctr.py prep): o RomFS é lido
 * direto do dump, a partir do nível 3, sem extrair nada. */
#include <fcntl.h>
#include <unistd.h>
static int g_romfs_fd = -1, g_rom_fd = -1; static uint64_t g_romfs_base;
static void romfs_load(void) {
    if (g_romfs_meta || g_romfs_fd >= 0) return;
    if (getenv("JOGO_3DS") && !getenv("ROMFS_META")) {
        g_romfs_fd = plat_open_env("JOGO_3DS");
        if (g_romfs_fd < 0) { perror(getenv("JOGO_3DS")); exit(1); }
        unsigned long long b = 0, sz = 0;
        if (!getenv("JOGO_3DS_ROMFS") || sscanf(getenv("JOGO_3DS_ROMFS"), "%llu %llu", &b, &sz) != 2) {
            fprintf(stderr, "[fs] defina JOGO_3DS_ROMFS (veja jogar.sh)\n"); exit(1); }
        g_romfs_base = b; g_romfs_size = sz;
        fprintf(stderr, "[fs] RomFS direto do .3ds (nível 3 em %#llx)\n", b);
        return;
    }
    /* mods (jogar.py, ROMFS_META + JOGO_3DS): o índice aponta para o .3ds ("@offset") ou para o arquivo do mod */
    if (getenv("JOGO_3DS")) { g_rom_fd = plat_open_env("JOGO_3DS"); if (g_rom_fd < 0) { perror(getenv("JOGO_3DS")); exit(1); } }
    const char *md = getenv("ROMFS_META") ? getenv("ROMFS_META") : "romfs_virtual";
    g_romfs_dir = getenv("ROMFS") ? getenv("ROMFS") : "romfs";
    char p[512]; snprintf(p, sizeof p, "%s/romfs_meta.bin", md);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "[fs] não achei %s (gere com tools/romfs_build.py)\n", p); exit(1); }
    fseek(f, 0, SEEK_END); g_romfs_meta_len = (uint64_t)ftell(f); fseek(f, 0, SEEK_SET);
    g_romfs_meta = malloc(g_romfs_meta_len);
    if (fread(g_romfs_meta, 1, g_romfs_meta_len, f) != g_romfs_meta_len) exit(1);
    fclose(f);
    snprintf(p, sizeof p, "%s/romfs_index.txt", md);
    f = fopen(p, "r"); if (!f) exit(1);
    int cap = 1024; g_romfs = malloc(sizeof(RomfsEntry) * cap);
    unsigned long long o, sz; char path[520];
    while (fscanf(f, "%llu %llu %519[^\n]\n", &o, &sz, path) == 3) {
        if (g_romfs_n == cap) g_romfs = realloc(g_romfs, sizeof(RomfsEntry) * (cap *= 2));
        RomfsEntry *e = &g_romfs[g_romfs_n++]; memset(e, 0, sizeof *e);
        e->off = o; e->size = sz; snprintf(e->path, sizeof e->path, "%s", path);
        if (o + sz > g_romfs_size) g_romfs_size = o + sz;
    }
    fclose(f);
    fprintf(stderr, "[fs] RomFS virtual: %d arquivos, %.1f MB\n", g_romfs_n, g_romfs_size / 1e6);
}

/* lê [off, off+len) da imagem virtual do RomFS */
static void romfs_read(uint64_t off, uint32_t len, uint8_t *dst) {
    plat_touch(dst, len);
    if (g_romfs_fd >= 0) {
        plat_touch(dst, len);
        long long got = plat_pread(g_romfs_fd, dst, len, g_romfs_base + off);
        if (got < (long long)len) memset(dst + (got > 0 ? got : 0), 0, len - (got > 0 ? (size_t)got : 0));
        return;
    }
    while (len) {
        if (off < g_romfs_meta_len) {
            uint32_t n = (uint32_t)((g_romfs_meta_len - off) < len ? (g_romfs_meta_len - off) : len);
            memcpy(dst, g_romfs_meta + off, n); off += n; dst += n; len -= n; continue;
        }
        int lo = 0, hi = g_romfs_n - 1, k = -1;            /* último arquivo com início <= off */
        while (lo <= hi) { int m = (lo + hi) / 2; if (g_romfs[m].off <= off) { k = m; lo = m + 1; } else hi = m - 1; }
        RomfsEntry *e = k >= 0 ? &g_romfs[k] : NULL;
        if (!e || off >= e->off + e->size) {               /* preenchimento entre arquivos / fim */
            uint64_t next = (k + 1 < g_romfs_n) ? g_romfs[k + 1].off : off + len;
            uint32_t n = (uint32_t)((next - off) < len ? (next - off) : len);
            memset(dst, 0, n); off += n; dst += n; len -= n; continue;
        }
        uint32_t n = (uint32_t)((e->off + e->size - off) < len ? (e->off + e->size - off) : len);
        int okr;
        if (e->path[0] == '@' && g_rom_fd >= 0)          /* dado do próprio .3ds */
            okr = plat_pread(g_rom_fd, dst, n, strtoull(e->path + 1, NULL, 10) + (off - e->off)) == (long long)n;
        else {                                            /* arquivo de mod (caminho absoluto) ou da pasta ROMFS */
            static int cfd = -1; static int ck = -1;      /* o último arquivo aberto fica aberto */
            if (ck != k) {
                if (cfd >= 0) close(cfd);
                char p[1100];
                if (e->path[0] == '/' || (e->path[0] && e->path[1] == ':')) snprintf(p, sizeof p, "%s", e->path);
                else snprintf(p, sizeof p, "%s/%s", g_romfs_dir, e->path);
                cfd = plat_open_utf8(p); ck = cfd >= 0 ? k : -1;
            }
            okr = cfd >= 0 && plat_pread(cfd, dst, n, off - e->off) == (long long)n;
        }
        if (!okr) {
            if (!e->missing_logged) { fprintf(stderr, "[romfs] FALTANDO: %s (+%llx, %u bytes)\n", e->path, (unsigned long long)(off - e->off), n); e->missing_logged = 1; }
            memset(dst, 0, n);
        } else if (getenv("FSTRACE")) fprintf(stderr, "[romfs] %s +%llx (%u bytes)\n", e->path, (unsigned long long)(off - e->off), n);
        off += n; dst += n; len -= n;
    }
}

typedef enum { FILE_ROMFS = 1 } FileKind;
static void fs_path(uint32_t type, uint32_t ptr, uint32_t size, char *out, int n) {
    if (type == 3 || type == 4) {            /* ASCII / UTF-16 */
        int k = 0;
        for (uint32_t i = 0; i < size && k < n - 1; i += (type == 4 ? 2 : 1)) {
            uint8_t ch = MEM8(ptr + i); if (!ch) break; out[k++] = (char)ch;
        }
        out[k] = 0;
    } else if (type == 2) {                  /* binário */
        int k = 0; for (uint32_t i = 0; i < size && k < n - 3; i++) k += snprintf(out + k, n - k, "%02x", MEM8(ptr + i));
    } else snprintf(out, n, "(vazio)");
}

/* ---------------- arquivos de save no PC ----------------
 * SaveData (4), ExtSaveData (6/7) e SDMC (9) viram pastas em SAVE_DIR (padrão "save"):
 *   save/savedata/   save/extdata/<id>/   save/sdmc/
 * Seguem os erros do 3DS: SaveData sem formatar -> "não formatado"; ExtSaveData inexistente -> "não encontrado". */
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#define FS_NOT_FOUND      0xC8804478u
#define FS_NOT_FORMATTED  0xC8A04554u
#define FS_ALREADY_EXISTS 0xC82044BEu
#define FS_DIR_NOT_FOUND  0xC8804471u
#define FS_FILE_NOT_FOUND 0xC8804470u

typedef struct { int used; uint32_t id; char dir[400]; } Archive;
static Archive g_arch[64];
typedef struct { int used; int is_dir; char path[600]; DIR *d; } HostFile;
static HostFile g_hf[256];

static const char *save_root(void) { return getenv("SAVE_DIR") ? getenv("SAVE_DIR") : "save"; }
static void mkdirs(const char *p) {
    char t[700]; snprintf(t, sizeof t, "%s", p);
    for (char *q = t + 1; *q; q++) if (*q == '/') { *q = 0; plat_mkdir(t); *q = '/'; }
    plat_mkdir(t);
}
static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
/* parâmetros de formatação de um save (como o FS do 3DS guarda), num arquivo ao lado da pasta */
static void fmt_save(const char *dir, const uint32_t fi[4]) {
    char p[460]; snprintf(p, sizeof p, "%s.formato", dir);
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "%u %u %u %u\n", fi[0], fi[1], fi[2], fi[3]); fclose(f); }
}
static void fmt_load(const char *dir, uint32_t fi[4]) {
    char p[460]; snprintf(p, sizeof p, "%s.formato", dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    uint32_t t[4];
    if (fscanf(f, "%u %u %u %u", &t[0], &t[1], &t[2], &t[3]) == 4) for (int i = 0; i < 4; i++) fi[i] = t[i];
    fclose(f);
}

/* pasta do arquivo de save para (id, caminho binário do arquivo) */
static void archive_dir(uint32_t id, uint32_t ptype, uint32_t pptr, uint32_t psize, char *out, int n) {
    if (id == 4) snprintf(out, n, "%s/savedata", save_root());
    else if (id == 6 || id == 7) {
        uint32_t lo = psize >= 8 ? MEM32(pptr + 4) : 0, hi = psize >= 12 ? MEM32(pptr + 8) : 0;
        snprintf(out, n, "%s/extdata/%08x%08x", save_root(), hi, lo);
    } else if (id == 9) snprintf(out, n, "%s/sdmc", save_root());
    else out[0] = 0;
}

/* caminho dentro do arquivo -> caminho no PC (sem "..") */
static int host_path(uint32_t ah, uint32_t ptype, uint32_t pptr, uint32_t psize, char *out, int n) {
    if (ah >= 64 || !g_arch[ah].used) return 0;
    char rel[300]; fs_path(ptype, pptr, psize, rel, sizeof rel);
    if (strstr(rel, "..")) return 0;
    snprintf(out, n, "%s%s%s", g_arch[ah].dir, rel[0] == '/' ? "" : "/", rel);
    return 1;
}

static uint32_t new_file_session(const char *path, int dir) {
    for (int i = 0; i < 256; i++) if (!g_hf[i].used) {
        g_hf[i].used = 1; g_hf[i].is_dir = dir; snprintf(g_hf[i].path, sizeof g_hf[i].path, "%s", path);
        g_hf[i].d = dir ? opendir(path) : NULL;
        uint32_t h = k_new(OBJ_SESSION, dir ? "fs:pasta" : "fs:arquivo"); k_get(h)->user = 0x100 + i;
        return h;
    }
    return 0;
}

static int fs(Ctx *c, uint32_t *cmd) {
    uint32_t id = cmd[0] >> 16;
    char ap[128], fp[256], hp[700];
    switch (id) {
    case 0x0861: case 0x0862: cmd[0] = (id << 16) | 0x40; cmd[1] = 0; return 1;  /* Initialize / SetPriority */
    case 0x0803: {           /* OpenFileDirectly(trans, archive, apType, apSize, fpType, fpSize, flags, attr, apDesc, ap, fpDesc, fp) */
        fs_path(cmd[3], cmd[10], cmd[4], ap, sizeof ap);
        fs_path(cmd[5], cmd[12], cmd[6], fp, sizeof fp);
        fprintf(stderr, "[fs] OpenFileDirectly arquivo=%08x caminho=%s flags=%x\n", cmd[2], fp, cmd[7]);
        if (cmd[2] == 3 && cmd[5] == 2 && MEM32(cmd[12]) == 0) {   /* SelfNCCH, tipo 0 = RomFS */
            romfs_load();
            uint32_t h = k_new(OBJ_SESSION, "fs:arquivo"); k_get(h)->user = FILE_ROMFS;
            cmd[0] = 0x08030042; cmd[1] = 0; cmd[2] = 0x10; cmd[3] = h; return 1;
        }
        cmd[0] = 0x08030042; cmd[1] = FS_NOT_FOUND; cmd[2] = 0x10; cmd[3] = 0; return 1;
    }
    case 0x080C: {           /* OpenArchive(id, pathType, pathSize, desc, ptr) */
        fs_path(cmd[2], cmd[5], cmd[3], ap, sizeof ap);
        char dir[400]; archive_dir(cmd[1], cmd[2], cmd[5], cmd[3], dir, sizeof dir);
        uint32_t res = 0;
        if (!dir[0]) res = FS_NOT_FOUND;
        else if (cmd[1] == 9) mkdirs(dir);
        else if (!is_dir(dir)) res = cmd[1] == 4 ? FS_NOT_FORMATTED : FS_NOT_FOUND;
        fprintf(stderr, "[fs] OpenArchive id=%x caminho=%s -> %s %s\n", cmd[1], ap, dir[0] ? dir : "(não suportado)", res ? "(não existe)" : "");
        uint32_t arch_id = cmd[1];
        cmd[0] = 0x080C00C0; cmd[1] = res; cmd[2] = 0; cmd[3] = 0;
        if (!res) for (int i = 1; i < 64; i++) if (!g_arch[i].used) {
            g_arch[i].used = 1; g_arch[i].id = arch_id; snprintf(g_arch[i].dir, sizeof g_arch[i].dir, "%s", dir);
            cmd[2] = i; break;
        }
        return 1;
    }
    case 0x080E: if (cmd[1] < 64) g_arch[cmd[1]].used = 0; cmd[0] = 0x080E0040; cmd[1] = 0; return 1;  /* CloseArchive */
    case 0x0802: {           /* OpenFile(trans, ah lo, ah hi, pathType, pathSize, flags, attr, desc, ptr) */
        uint32_t flags = cmd[6];
        int ok = host_path(cmd[2], cmd[4], cmd[9], cmd[5], hp, sizeof hp);   /* lê os argumentos antes de escrever a resposta */
        cmd[0] = 0x08020042; cmd[2] = 0x10; cmd[3] = 0;
        if (!ok) { cmd[1] = FS_NOT_FOUND; return 1; }
        fprintf(stderr, "[fs] OpenFile %s flags=%x\n", hp, flags);
        if (!exists(hp)) {
            if (!(flags & 4)) { cmd[1] = FS_FILE_NOT_FOUND; return 1; }   /* sem a flag CREATE */
            FILE *f = fopen(hp, "wb"); if (f) fclose(f);
        }
        cmd[1] = 0; cmd[3] = new_file_session(hp, 0); return 1;
    }
    case 0x0804: case 0x0805: case 0x0806: case 0x0807: {  /* DeleteFile / RenameFile(-) / DeleteDirectory(Recursively) */
        if (host_path(cmd[2], cmd[4], id == 0x0804 ? cmd[7] : cmd[7], cmd[5], hp, sizeof hp)) {
            fprintf(stderr, "[fs] apagar %s\n", hp);
            if (id == 0x0804) remove(hp); else if (id != 0x0805) plat_rmdir(hp);
        }
        cmd[0] = (id << 16) | 0x40; cmd[1] = 0; return 1;
    }
    case 0x0808: {           /* CreateFile(trans, ah lo, hi, pathType, pathSize, attr, size lo, size hi, desc, ptr) */
        cmd[0] = 0x08080040;
        if (!host_path(cmd[2], cmd[4], cmd[10], cmd[5], hp, sizeof hp)) { cmd[1] = FS_NOT_FOUND; return 1; }
        if (exists(hp)) { cmd[1] = FS_ALREADY_EXISTS; return 1; }
        uint64_t size = ((uint64_t)cmd[8] << 32) | cmd[7];
        FILE *f = fopen(hp, "wb");
        if (f) { if (size) { fseek(f, (long)size - 1, SEEK_SET); fputc(0, f); } fclose(f); }
        fprintf(stderr, "[fs] CreateFile %s (%llu bytes)\n", hp, (unsigned long long)size);
        cmd[1] = f ? 0 : FS_NOT_FOUND; return 1;
    }
    case 0x0809: {           /* CreateDirectory(trans, ah lo, hi, pathType, pathSize, attr, desc, ptr) */
        cmd[0] = 0x08090040;
        if (!host_path(cmd[2], cmd[4], cmd[8], cmd[5], hp, sizeof hp)) { cmd[1] = FS_NOT_FOUND; return 1; }
        cmd[1] = exists(hp) ? FS_ALREADY_EXISTS : 0; mkdirs(hp);
        fprintf(stderr, "[fs] CreateDirectory %s\n", hp);
        return 1;
    }
    case 0x080B: {           /* OpenDirectory(ah lo, hi, pathType, pathSize, desc, ptr) */
        int ok = host_path(cmd[1], cmd[3], cmd[6], cmd[4], hp, sizeof hp) && is_dir(hp);
        cmd[0] = 0x080B0042; cmd[2] = 0x10; cmd[3] = 0;
        if (!ok) { cmd[1] = FS_DIR_NOT_FOUND; return 1; }
        cmd[1] = 0; cmd[3] = new_file_session(hp, 1); return 1;
    }
    case 0x0845: {           /* GetFormatInfo(id, pathType, pathSize, desc, ptr) */
        /* o jogo compara com o que pediu na formatação; diferente = "dados corrompidos" */
        char dir[400]; archive_dir(cmd[1], cmd[2], cmd[5], cmd[3], dir, sizeof dir);
        int ok = dir[0] && is_dir(dir);
        uint32_t fi[4] = { 0x100000, 16, 64, 0 };
        if (ok) fmt_load(dir, fi);
        fprintf(stderr, "[fs] GetFormatInfo id=%x %s -> %s (%u bytes, %u pastas, %u arquivos, dup %u)\n", cmd[1], dir,
                ok ? "formatado" : "não formatado", fi[0], fi[1], fi[2], fi[3]);
        cmd[0] = 0x08450140; cmd[1] = ok ? 0 : (cmd[1] == 4 ? FS_NOT_FORMATTED : FS_NOT_FOUND);
        cmd[2] = fi[0]; cmd[3] = fi[1]; cmd[4] = fi[2]; cmd[5] = fi[3]; return 1;
    }
    case 0x084C: {           /* FormatSaveData(id, tipo, tam, bloco, pastas, arquivos, baldes, baldes, duplicado, ...) */
        char dir[400]; archive_dir(cmd[1], cmd[2], cmd[11], cmd[3], dir, sizeof dir);
        fprintf(stderr, "[fs] FormatSaveData %s\n", dir);
        if (dir[0]) {
            mkdirs(dir);
            uint32_t fi[4] = { cmd[4] * 512, cmd[5], cmd[6], cmd[9] & 0xFF };
            fmt_save(dir, fi);
        }
        cmd[0] = 0x084C0040; cmd[1] = 0; return 1;
    }
    case 0x0851: {           /* CreateExtSaveData(info: mídia, pad, id lo, id hi, ...) */
        /* layout (3dbrew): cmd[1..4] = ExtSaveDataInfo {u8 mídia, pad[3], u64 id, u32 unk}; o id fica em cmd[2]/cmd[3] */
        char dir[400]; snprintf(dir, sizeof dir, "%s/extdata/%08x%08x", save_root(), cmd[3], cmd[2]);
        fprintf(stderr, "[fs] CreateExtSaveData %s\n", dir);
        mkdirs(dir);
        { uint32_t fi[4] = { 0, cmd[5], cmd[6], 0 }; fmt_save(dir, fi); }
        cmd[0] = 0x08510040; cmd[1] = 0; return 1;
    }
    case 0x080D: cmd[0] = 0x080D0042; cmd[1] = 0; cmd[2] = 0; cmd[3] = 0; return 1;   /* ControlArchive (commit do save) */
    case 0x0814: cmd[0] = 0x08140080; cmd[1] = 0; cmd[2] = 0x10000000; cmd[3] = 0; return 1;  /* GetFreeBytes: 256 MB */
    case 0x0817: case 0x0818: cmd[0] = (id << 16) | 0x80; cmd[1] = 0; cmd[2] = 1; return 1; /* SD presente / gravável */
    }
    return 0;
}

/* comandos numa sessão de arquivo aberto (RomFS ou arquivo do PC) */
static int fs_file(Ctx *c, KObj *o, uint32_t *cmd) {
    uint32_t id = cmd[0] >> 16;
    HostFile *hf = o->user >= 0x100 ? &g_hf[o->user - 0x100] : NULL;
    if (hf && hf->is_dir) {
        if (id == 0x0801) {          /* Read(count, desc, ptr): entradas de 0x228 bytes */
            uint32_t want = cmd[1], ptr = cmd[3], n = 0; struct dirent *e;
            while (n < want && hf->d && (e = readdir(hf->d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                uint8_t *ent = g_mem + ptr + n * 0x228; memset(ent, 0, 0x228);
                for (int k = 0; e->d_name[k] && k < 0x105; k++) ent[k * 2] = (uint8_t)e->d_name[k];
                char full[900]; snprintf(full, sizeof full, "%s/%s", hf->path, e->d_name);
                struct stat st; stat(full, &st);
                ent[0x21C] = S_ISDIR(st.st_mode); *(uint64_t *)(ent + 0x220) = (uint64_t)st.st_size;
                n++;
            }
            cmd[0] = 0x08010082; cmd[1] = 0; cmd[2] = n; return 1;
        }
        if (hf->d) closedir(hf->d); hf->d = NULL; hf->used = 0;
        cmd[0] = (id << 16) | 0x40; cmd[1] = 0; return 1;
    }
    switch (id) {
    case 0x0802: {           /* Read(offset u64, tamanho, desc, ptr) */
        uint64_t off = ((uint64_t)cmd[2] << 32) | cmd[1];
        uint32_t len = cmd[3], ptr = cmd[5], desc = cmd[4];
        if (getenv("FSTRACE") || (getenv("WATCH") && strtoul(getenv("WATCH"), 0, 16) - ptr < len))
            fprintf(stderr, "[fs] Read offset=%llx tamanho=%x -> %08x..%08x (desc %08x, tls %08x, lr %08x)\n", (unsigned long long)off, len, ptr, ptr + len, desc, c->tls, c->r[14]);
        if (o->user == FILE_ROMFS) {
            if (off >= g_romfs_size) len = 0; else if (off + len > g_romfs_size) len = (uint32_t)(g_romfs_size - off);
            romfs_read(off, len, g_mem + ptr);
        } else if (hf) {
            FILE *f = fopen(hf->path, "rb"); size_t got = 0;
            plat_touch(g_mem + ptr, len);
            if (f) { fseek(f, (long)off, SEEK_SET); got = fread(g_mem + ptr, 1, len, f); fclose(f); }
            len = (uint32_t)got;
        }
        cmd[0] = 0x08020082; cmd[1] = 0; cmd[2] = len; cmd[3] = desc; cmd[4] = ptr; return 1;
    }
    case 0x0803: {           /* Write(offset u64, tamanho, flags, desc, ptr) */
        uint64_t off = ((uint64_t)cmd[2] << 32) | cmd[1];
        uint32_t len = cmd[3], ptr = cmd[6]; size_t put = 0;
        plat_touch(g_mem + ptr, len);
        if (hf) { FILE *f = fopen(hf->path, "r+b"); if (f) { fseek(f, (long)off, SEEK_SET); put = fwrite(g_mem + ptr, 1, len, f); fclose(f); } }
        cmd[0] = 0x08030082; cmd[1] = 0; cmd[2] = (uint32_t)put; return 1;
    }
    case 0x0804: {           /* GetSize */
        uint64_t sz = g_romfs_size;
        if (hf) { struct stat st; sz = stat(hf->path, &st) == 0 ? (uint64_t)st.st_size : 0; }
        cmd[0] = 0x080400C0; cmd[1] = 0; cmd[2] = (uint32_t)sz; cmd[3] = (uint32_t)(sz >> 32); return 1;
    }
    case 0x0805: {           /* SetSize */
        if (hf) { uint64_t sz = ((uint64_t)cmd[2] << 32) | cmd[1]; if (plat_truncate(hf->path, sz)) {} }
        cmd[0] = 0x08050040; cmd[1] = 0; return 1;
    }
    case 0x0808: if (hf) hf->used = 0; cmd[0] = 0x08080040; cmd[1] = 0; return 1;   /* Close */
    case 0x0809: cmd[0] = 0x08090040; cmd[1] = 0; return 1;                           /* Flush */
    case 0x080A: cmd[0] = 0x080A0040; cmd[1] = 0; return 1;                           /* SetPriority */
    case 0x080B: cmd[0] = 0x080B0080; cmd[1] = 0; cmd[2] = 0; return 1;               /* GetPriority */
    case 0x080C: {           /* OpenLinkFile: nova sessão para o mesmo arquivo (o jogo lê o vídeo por duas) */
        uint32_t h;
        if (hf) h = new_file_session(hf->path, 0);
        else { h = k_new(OBJ_SESSION, "fs:arquivo"); k_get(h)->user = o->user; }
        cmd[0] = 0x080C0042; cmd[1] = 0; cmd[2] = 0x10; cmd[3] = h; return 1;
    }
    }
    return 0;
}

int gsp(Ctx *c, uint32_t *cmd);
int ldr(Ctx *c, uint32_t *cmd);
int y2r(Ctx *c, uint32_t *cmd);

/* ---------------- dsp::DSP: processador de áudio ----------------
 * Protocolo de controle + mixagem (dsp_hle.cpp, núcleo de áudio do Azahar): ao receber "Initialize" no pipe de
 * áudio (canal 2), o DSP responde com a quantidade e os endereços (em palavras do DSP) das 15 estruturas
 * na memória compartilhada (0x1FF50000 e 0x1FF70000, dois buffers alternados) e passa a gerar um
 * "quadro de áudio" a cada ~4,9 ms, sinalizando o evento registrado para o pipe de áudio. */
#define DSP_MEM0 0x1FF50000u
#define DSP_MEM1 0x1FF70000u
static uint32_t g_dsp_sem_event, g_dsp_pipe_event[4];
static uint8_t g_dsp_pipe3[256]; static uint32_t g_dsp_pipe3_len;
void dsp_aac_request(const uint8_t req[32], uint8_t resp[32]);   /* dsp_aac.c */
static uint8_t g_dsp_pipe2[64]; static uint32_t g_dsp_pipe2_len;
static int g_dsp_running;
/* endereços (palavras do DSP) das 15 estruturas, no layout do núcleo de áudio do Azahar (dsp_hle.cpp):
 * contador de quadro, configurações e estados das vozes, coeficientes ADPCM, configuração/estado do DSP,
 * amostras finais, mixagens intermediárias, compressor, depuração e 5 áreas desconhecidas */
static const uint16_t g_dsp_struct_addr[15] = {
    0xBFFF, 0x9E92, 0x8680, 0xA792, 0x9430, 0x8400, 0x8540, 0x9492,
    0x8710, 0x8410, 0xA912, 0xAA12, 0xAAD2, 0xAC52, 0xAC5C };
int dsp_hle_frame(int16_t *out);

extern int g_periodic_defer; extern uint64_t g_periodic_late_ns;
volatile long g_dsp_waits;   /* quadros em que o DSP esperou o jogo terminar o anterior (estatística) */
static void dsp_frame(void) {
    if (!g_dsp_running) return;
    /* No 3DS o jogo atende o aviso de cada quadro bem antes do próximo. Aqui, quando o PC atrasa, dois quadros
     * podem vencer em seguida; misturar o segundo antes de a thread de som do jogo atender o primeiro
     * (configurar as vozes, enfileirar mais som) deixa vozes sem dados -> picote. Então, se o aviso anterior
     * ainda não foi atendido, espera um pouco (até 12 ms além do horário; depois segue de qualquer jeito). */
    if (g_dsp_pipe_event[2]) {
        KObj *ev = k_get(g_dsp_pipe_event[2]);
        if (ev && ev->signaled && g_periodic_late_ns < 12000000ull) { g_periodic_defer = 1; g_dsp_waits++; return; }
    }
    /* o DSP mistura um quadro (160 amostras) a partir da memória compartilhada e avisa o jogo */
    int16_t pcm[160 * 2];
    int n = dsp_hle_frame(pcm);
    audio_push(pcm, n);
    if (g_dsp_pipe_event[2]) k_set_signaled(g_dsp_pipe_event[2]);
    if (g_dsp_sem_event) k_set_signaled(g_dsp_sem_event);
}

static int dsp(Ctx *c, uint32_t *cmd) {
    uint32_t id = cmd[0] >> 16;
    switch (id) {
    case 0x0011:                                  /* LoadComponent(tamanho, máscaras, desc, ptr) */
        fprintf(stderr, "[dsp] firmware carregado (%u bytes)\n", cmd[1]);
        cmd[0] = 0x00110082; cmd[1] = 0; cmd[2] = 1; cmd[3] = cmd[4]; cmd[4] = cmd[5]; return 1;
    case 0x0001: case 0x0002: cmd[0] = (id << 16) | 0x80; cmd[1] = 0; cmd[2] = 1; return 1;  /* RecvData(IsReady) */
    case 0x000C: { uint32_t a = cmd[1]; cmd[0] = 0x000C0080; cmd[1] = 0; cmd[2] = (a << 1) + 0x1FF40000u; return 1; }
    case 0x000D: {                                /* WriteProcessPipe(canal, tamanho, desc, ptr) */
        uint32_t ch = cmd[1], n = cmd[2], ptr = cmd[4];
        if (g_ipc_trace > 0 && ch != 3) {
            fprintf(stderr, "[dsp] pipe %u <- %u bytes:", ch, n);
            for (uint32_t i = 0; i < n && i < 16; i++) fprintf(stderr, " %02x", MEM8(ptr + i));
            fprintf(stderr, "\n");
        }
        if (ch == 2 && n >= 4) {
            uint32_t state = MEM32(ptr);
            if (state == 0 || state == 2) {      /* Initialize / Wakeup: responde com os endereços */
                uint16_t *w = (uint16_t *)g_dsp_pipe2;
                w[0] = 15;
                for (int i = 0; i < 15; i++) w[1 + i] = g_dsp_struct_addr[i];
                g_dsp_pipe2_len = 32;
                if (!g_dsp_running) { g_dsp_running = 1; k_add_periodic_catchup(dsp_frame, 4888000, 20); }
            } else if (state == 1 || state == 3) {   /* Shutdown / Sleep */
                g_dsp_running = 0; g_dsp_pipe2_len = 0;
            }
            if (g_dsp_pipe_event[2]) k_set_signaled(g_dsp_pipe_event[2]);
        }
        if (ch == 3 && n >= 32) {                /* pipe binário: decodificador AAC do DSP (música do jogo) */
            uint8_t req[32], resp[32];
            plat_touch(g_mem + ptr, 32); memcpy(req, g_mem + ptr, 32);
            dsp_aac_request(req, resp);
            if (g_dsp_pipe3_len + 32 <= sizeof g_dsp_pipe3) { memcpy(g_dsp_pipe3 + g_dsp_pipe3_len, resp, 32); g_dsp_pipe3_len += 32; }
            if (g_dsp_pipe_event[3]) k_set_signaled(g_dsp_pipe_event[3]);
        }
        cmd[0] = 0x000D0040; cmd[1] = 0; return 1;
    }
    case 0x000F:                                  /* GetPipeReadableSize */
        { uint32_t ch = cmd[1]; cmd[0] = 0x000F0080; cmd[1] = 0; cmd[2] = ch == 2 ? g_dsp_pipe2_len : ch == 3 ? g_dsp_pipe3_len : 0; return 1; }
    case 0x0010: case 0x000E: {                   /* ReadPipeIfPossible / ReadPipe(canal, par, tamanho) */
        uint32_t ch = cmd[1], want = cmd[3] & 0xFFFF, got = 0, buf = MEM32(c->tls + 0x184);
        if (ch == 2 && g_dsp_pipe2_len) {
            got = want < g_dsp_pipe2_len ? want : g_dsp_pipe2_len;
            memcpy(g_mem + buf, g_dsp_pipe2, got);
            memmove(g_dsp_pipe2, g_dsp_pipe2 + got, g_dsp_pipe2_len - got); g_dsp_pipe2_len -= got;
        } else if (ch == 3 && g_dsp_pipe3_len) {
            got = want < g_dsp_pipe3_len ? want : g_dsp_pipe3_len;
            memcpy(g_mem + buf, g_dsp_pipe3, got);
            memmove(g_dsp_pipe3, g_dsp_pipe3 + got, g_dsp_pipe3_len - got); g_dsp_pipe3_len -= got;
        }
        if (g_ipc_trace > 0 && ch != 3) fprintf(stderr, "[dsp] pipe %u -> %u de %u bytes\n", ch, got, want);
        if (id == 0x0010) { cmd[0] = 0x00100082; cmd[1] = 0; cmd[2] = got; cmd[3] = (got << 14) | 2; cmd[4] = buf; }
        else { cmd[0] = 0x000E0042; cmd[1] = 0; cmd[2] = (got << 14) | 2; cmd[3] = buf; }
        return 1;
    }
    case 0x0015:                                  /* RegisterInterruptEvents(tipo, canal, desc, evento) */
        if (cmd[1] == 2 && cmd[2] < 4) g_dsp_pipe_event[cmd[2]] = cmd[4];
        cmd[0] = 0x00150040; cmd[1] = 0; return 1;
    case 0x0016:                                  /* GetSemaphoreEventHandle */
        if (!g_dsp_sem_event) g_dsp_sem_event = k_new(OBJ_EVENT, "dsp:semáforo");
        cmd[0] = 0x00160042; cmd[1] = 0; cmd[2] = 0; cmd[3] = g_dsp_sem_event; return 1;
    case 0x001F: cmd[0] = 0x001F0080; cmd[1] = 0; cmd[2] = 0; return 1;      /* GetHeadphoneStatus */
    }
    return 0;
}

/* ---------------- idioma do jogo (opção do menu: IDIOMA_JOGO = en, es, fr, de, it, ja, ko) ----------------
 * O jogo pergunta o idioma (módulo DllLangSelect) só quando não há save; depois o idioma fica num byte do
 * save (bloco 17, do treinador: 0x14000 + 0x2D; 1 japonês, 2 inglês, 3 francês, 4 italiano, 5 alemão,
 * 7 espanhol, 8 coreano), protegido por um CRC16 na tabela de blocos (0x65414, 8 bytes por bloco).
 *   - save existente: antes do jogo abrir, o byte é trocado e o CRC refeito (uma cópia do save original fica
 *     em savedata/main.antes_do_idioma, só na primeira troca);
 *   - sem save: o idioma do console (cfg) passa a ser o escolhido, o que o põe no topo da lista do jogo com o
 *     cursor nele, e input.c confirma sozinho (A, A) enquanto a DllLangSelect estiver carregada. */
static const struct { const char *k; int cfg, save; } g_idiomas[] = {
    { "ja", 0, 1 }, { "en", 1, 2 }, { "fr", 2, 3 }, { "de", 3, 5 }, { "it", 4, 4 }, { "es", 5, 7 }, { "ko", 7, 8 } };
static int idioma_idx(void) {
    const char *e = getenv("IDIOMA_JOGO"); if (!e || !*e) return -1;
    for (unsigned i = 0; i < sizeof g_idiomas / sizeof g_idiomas[0]; i++) if (!strcmp(e, g_idiomas[i].k)) return (int)i;
    return -1;
}
int idioma_jogo_cfg(void) { int i = idioma_idx(); return i < 0 ? 1 : g_idiomas[i].cfg; }
int idioma_jogo_auto(void) { return idioma_idx() >= 0 && !getenv("SEM_IDIOMA_AUTO"); }
static uint16_t crc16_ccitt(const uint8_t *p, uint32_t n) {
    uint16_t c = 0xFFFF;
    while (n--) { c ^= (uint16_t)(*p++ << 8); for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1); }
    return c;
}
/* idioma guardado no save (código do menu: en, es, ...) ou NULL sem save */
const char *idioma_jogo_save(void) {
    char p[512]; snprintf(p, sizeof p, "%s/savedata/main", save_root());
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    uint8_t b = 0; int ok = fseek(f, 0x14000 + 0x2D, SEEK_SET) == 0 && fread(&b, 1, 1, f) == 1; fclose(f);
    if (!ok) return NULL;
    for (unsigned i = 0; i < sizeof g_idiomas / sizeof g_idiomas[0]; i++) if (g_idiomas[i].save == b) return g_idiomas[i].k;
    return NULL;
}
/* conserto do save (X/Y), antes do jogo abrir:
 *   - save copiado de outro lugar (Citra, JKSM, outro PC) vem sem o savedata.formato que este runtime guarda na
 *     formatação; sem ele o fs respondia um formato genérico e o jogo dizia "dados corrompidos". Com um main do
 *     X/Y (0x65600 bytes, tabela "FEEB"), o .formato é recriado com o que o jogo usa (256 KB, 1 pasta, 1 arquivo,
 *     duplicado);
 *   - blocos com o CRC16 errado (save editado por fora) têm o CRC refeito (cópia do original em
 *     main.antes_do_reparo, só na primeira vez). */
void save_reparar(void) {
    char p[512]; snprintf(p, sizeof p, "%s/savedata/main", save_root());
    FILE *f = fopen(p, "rb"); if (!f) return;
    enum { SZ = 0x65600, TAB = 0x65414 };
    uint8_t *d = malloc(SZ + 1); size_t n = d ? fread(d, 1, SZ + 1, f) : 0; fclose(f);
    if (n != SZ || memcmp(d + 0x65410, "FEEB", 4)) { free(d); return; }      /* não é um save do X/Y */
    char fm[540]; snprintf(fm, sizeof fm, "%s/savedata.formato", save_root());
    FILE *t = fopen(fm, "r");
    if (t) fclose(t);
    else {
        uint32_t fi[4] = { 0x40000, 1, 1, 1 };
        char dir[512]; snprintf(dir, sizeof dir, "%s/savedata", save_root()); fmt_save(dir, fi);
        fprintf(stderr, "[save] savedata.formato recriado (save trazido de outro lugar)\n");
    }
    int ruins = 0; uint32_t off = 0;
    for (int k = 0; k < 64; k++) {
        uint32_t len; uint16_t ck; memcpy(&len, d + TAB + k * 8, 4); memcpy(&ck, d + TAB + k * 8 + 6, 2);
        if (!len) break;
        if (off + len > 0x65400) break;
        uint16_t c = crc16_ccitt(d + off, len);
        if (c != ck) { memcpy(d + TAB + k * 8 + 6, &c, 2); ruins++; }
        off += (len + 0x1FF) & ~0x1FFu;
    }
    if (ruins) {
        char b[540]; snprintf(b, sizeof b, "%s.antes_do_reparo", p);
        FILE *o = fopen(b, "rb");
        if (o) fclose(o); else {                         /* guarda o arquivo de antes (ainda intacto no disco) */
            FILE *r = fopen(p, "rb"), *w = fopen(b, "wb");
            if (r && w) { uint8_t buf[65536]; size_t m; while ((m = fread(buf, 1, sizeof buf, r)) > 0) fwrite(buf, 1, m, w); }
            if (r) fclose(r); if (w) fclose(w);
        }
        char tmp[540]; snprintf(tmp, sizeof tmp, "%s.novo", p);
        o = fopen(tmp, "wb");
        if (o && fwrite(d, 1, SZ, o) == SZ && fclose(o) == 0) { remove(p); rename(tmp, p);
            fprintf(stderr, "[save] %d bloco(s) com verificação errada consertado(s)\n", ruins); }
        else if (o) fclose(o);
    }
    free(d);
}
/* Nuzlocke: tira da equipe do save no disco os Pokémon com estes EC (bloco 18, 0x14200: 6 x 0x104 + contagem) */
int nuzlocke_save_remover(const uint32_t *ecs, int n) {
    char p[512]; snprintf(p, sizeof p, "%s/savedata/main", save_root());
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    enum { SZ = 0x65600, TAB = 0x65414, PT = 0x14200 };
    uint8_t *d = malloc(SZ); size_t got = d ? fread(d, 1, SZ, f) : 0; fclose(f);
    if (got != SZ || memcmp(d + 0x65410, "FEEB", 4)) { free(d); return 0; }
    uint32_t cnt = d[PT + 0x618]; if (cnt > 6)   /* a contagem é um byte */ { free(d); return 0; }
    uint8_t novo[6][0x104]; int nm = 0, tirados = 0;
    for (uint32_t k = 0; k < cnt; k++) {
        uint32_t ec; memcpy(&ec, d + PT + k * 0x104, 4); int sai = 0;
        for (int i = 0; i < n; i++) if (ecs[i] == ec) sai = 1;
        if (sai) tirados++; else memcpy(novo[nm++], d + PT + k * 0x104, 0x104);
    }
    if (!tirados || nm == 0) { free(d); return 0; }
    for (int k = nm; k < 6; k++) {                    /* vaga vazia: zeros cifrados com EC 0 */
        memset(novo[k], 0, 0x104); uint32_t seed = 0;
        for (int i = 8; i < 0xE8; i += 2) { seed = seed * 0x41C64E6Du + 0x6073u; novo[k][i] = (uint8_t)(seed >> 16); novo[k][i + 1] = (uint8_t)(seed >> 24); }
        seed = 0;
        for (int i = 0xE8; i < 0x104; i += 2) { seed = seed * 0x41C64E6Du + 0x6073u; novo[k][i] = (uint8_t)(seed >> 16); novo[k][i + 1] = (uint8_t)(seed >> 24); }
    }
    memcpy(d + PT, novo, sizeof novo); d[PT + 0x618] = (uint8_t)nm;
    uint32_t off = 0;                                  /* refaz o CRC do bloco que contém a equipe */
    for (int k = 0; k < 64; k++) {
        uint32_t len; memcpy(&len, d + TAB + k * 8, 4); if (!len) break;
        if (off <= PT && PT < off + len) { uint16_t c = crc16_ccitt(d + off, len); memcpy(d + TAB + k * 8 + 6, &c, 2); break; }
        off += (len + 0x1FF) & ~0x1FFu;
    }
    char tmp[540]; snprintf(tmp, sizeof tmp, "%s.novo", p);
    f = fopen(tmp, "wb");
    if (f && fwrite(d, 1, SZ, f) == SZ && fclose(f) == 0) { remove(p); rename(tmp, p); fprintf(stderr, "[nuzlocke] %d tirado(s) da equipe do save\n", tirados); }
    else if (f) fclose(f);
    free(d); return tirados;
}
void idioma_jogo_aplicar(void) {
    int i = idioma_idx(); if (i < 0) return;
    char p[512]; snprintf(p, sizeof p, "%s/savedata/main", save_root());
    FILE *f = fopen(p, "rb"); if (!f) return;                       /* sem save: a confirmação automática cuida */
    enum { SZ = 0x65600, TAB = 0x65414, MYST = 0x14000 };
    uint8_t *d = malloc(SZ); size_t n = d ? fread(d, 1, SZ, f) : 0; fclose(f);
    if (n != SZ || memcmp(d + 0x65410, "FEEB", 4)) { free(d); return; }      /* não é um save do X/Y */
    uint32_t len; uint16_t id; memcpy(&len, d + TAB + 17 * 8, 4); memcpy(&id, d + TAB + 17 * 8 + 4, 2);
    uint16_t ck; memcpy(&ck, d + TAB + 17 * 8 + 6, 2);
    if (id != 17 || len > 0x400 || crc16_ccitt(d + MYST, len) != ck) {
        fprintf(stderr, "[idioma] save com formato inesperado: idioma não trocado\n"); free(d); return; }
    uint8_t old = d[MYST + 0x2D], want = (uint8_t)g_idiomas[i].save;
    if (old == want) { free(d); return; }
    char b[540]; snprintf(b, sizeof b, "%s.antes_do_idioma", p);
    FILE *t = fopen(b, "rb");
    if (t) fclose(t); else { t = fopen(b, "wb"); if (t) { fwrite(d, 1, SZ, t); fclose(t); } }
    d[MYST + 0x2D] = want;
    ck = crc16_ccitt(d + MYST, len); memcpy(d + TAB + 17 * 8 + 6, &ck, 2);
    char tmp[540]; snprintf(tmp, sizeof tmp, "%s.novo", p);
    f = fopen(tmp, "wb");
    if (f && fwrite(d, 1, SZ, f) == SZ && fclose(f) == 0) {
        remove(p);
        if (rename(tmp, p) == 0) fprintf(stderr, "[idioma] idioma do save trocado: %d -> %d (%s)\n", old, want, g_idiomas[i].k);
    } else if (f) fclose(f);
    free(d);
}

/* ---------------- cfg: configuração do console ----------------
 * Blocos de configuração (3dbrew "Config Savegame"). Idioma: variável IDIOMA_3DS
 * (0 japonês, 1 inglês, 2 francês, 3 alemão, 4 italiano, 5 espanhol, 7 coreano); padrão inglês. */
static int cfg(Ctx *c, uint32_t *cmd) {
    uint32_t id = cmd[0] >> 16;
    switch (id) {
    case 0x0001: case 0x0401: case 0x0801: {       /* GetConfigInfoBlk2(tamanho, bloco, desc, ptr) */
        uint32_t size = cmd[1], blk = cmd[2], ptr = cmd[4];
        uint8_t *d = g_mem + ptr;
        memset(d, 0, size);
        int lang = getenv("IDIOMA_3DS") ? atoi(getenv("IDIOMA_3DS")) : idioma_jogo_cfg();
        switch (blk) {
        case 0x000A0002: d[0] = (uint8_t)lang; break;                       /* idioma */
        case 0x000B0000: if (size >= 4) { d[2] = 1; d[3] = 49; } break;   /* país: EUA (49), estado 1 */
        case 0x000A0000: {                                                  /* nome de usuário (UTF-16) */
            const char *n = getenv("NOME_3DS") ? getenv("NOME_3DS") : "Jogador";
            for (int k = 0; n[k] && 2 * k + 1 < (int)size && k < 10; k++) d[2 * k] = (uint8_t)n[k];
            break; }
        case 0x000A0001: if (size >= 2) { d[0] = 1; d[1] = 1; } break;    /* aniversário: 1/1 */
        case 0x00070001: d[0] = 1; break;                                   /* som: estéreo */
        case 0x00050005: {                                                  /* câmera estéreo (projeção 3D): valores de fábrica */
            static const float k[8] = { 62.0f, 289.0f, 76.80000305f, 46.08000183f, 10.0f, 5.0f, 55.58000183f, 21.56999969f };
            memcpy(d, k, size < sizeof k ? size : sizeof k); break; }
        }
        cmd[0] = (id << 16) | 0x42; cmd[1] = 0; cmd[2] = (size << 4) | 0xC; cmd[3] = ptr; return 1;
    }
    case 0x0002: cmd[0] = 0x00020080; cmd[1] = 0; cmd[2] = 1; return 1;      /* SecureInfoGetRegion: EUA */
    case 0x0003: cmd[0] = 0x00030080; cmd[1] = 0; cmd[2] = 0x12345678; return 1; /* GenHashConsoleUnique */
    case 0x0004: cmd[0] = 0x00040080; cmd[1] = 0; cmd[2] = 0; return 1;      /* GetRegionCanadaUSA */
    case 0x0005: cmd[0] = 0x00050080; cmd[1] = 0; cmd[2] = 0; return 1;      /* GetSystemModel: 3DS */
    }
    return 0;
}

/* ---------------- cam:u: câmeras ----------------
 * O PC não tem as câmeras do 3DS: elas "funcionam" e entregam imagem preta. O Pokémon-Amie abre a câmera
 * (reconhecimento de rosto) e o microfone; sem os eventos que esses serviços devolvem, o jogo esperava num
 * handle inválido e abortava. Comandos: 3dbrew "Camera Services". Portas: bit 0 = câmera 1, bit 1 = câmera 2. */
static uint32_t g_cam_vsync[2], g_cam_erro[2], g_cam_fim[2];
static int camu(Ctx *c, uint32_t *cmd) {
    (void)c;
    uint32_t id = cmd[0] >> 16, porta = cmd[1] & 3; int p = porta == 2 ? 1 : 0;
    switch (id) {
    case 0x0003: cmd[0] = 0x00030080; cmd[1] = 0; cmd[2] = 0; return 1;              /* IsBusy: não */
    case 0x0005:                                                                     /* GetVsyncInterruptEvent */
        if (!g_cam_vsync[p]) g_cam_vsync[p] = k_new_periodic_event("cam:vsync", 33333333);   /* ~30 quadros/s */
        cmd[0] = 0x00050042; cmd[1] = 0; cmd[2] = 0; cmd[3] = g_cam_vsync[p]; return 1;
    case 0x0006:                                                                     /* GetBufferErrorInterruptEvent */
        if (!g_cam_erro[p]) g_cam_erro[p] = k_new(OBJ_EVENT, "cam:erro");
        cmd[0] = 0x00060042; cmd[1] = 0; cmd[2] = 0; cmd[3] = g_cam_erro[p]; return 1;
    case 0x0007: {                                                                   /* SetReceiving(dst, porta, tamanho, unidade) */
        uint32_t dst = cmd[1], tam = cmd[3]; p = (cmd[2] & 3) == 2 ? 1 : 0;
        if (!g_cam_fim[p]) g_cam_fim[p] = k_new(OBJ_EVENT, "cam:recebido");
        if (dst && tam && tam <= (8u << 20))                                         /* imagem preta (YUV422: Y 16, U/V 128) */
            for (uint32_t i = 0; i < tam; i++) MEM8(dst + i) = (i & 1) ? 0x80 : 0x10;
        k_signal(g_cam_fim[p]);
        cmd[0] = 0x00070042; cmd[1] = 0; cmd[2] = 0; cmd[3] = g_cam_fim[p]; return 1;
    }
    case 0x0008: cmd[0] = 0x00080080; cmd[1] = 0; cmd[2] = 1; return 1;              /* IsFinishedReceiving: sim */
    case 0x000A: cmd[0] = 0x000A0080; cmd[1] = 0; cmd[2] = 16; return 1;             /* GetMaxLines */
    case 0x000C: cmd[0] = 0x000C0080; cmd[1] = 0; cmd[2] = 256; return 1;            /* GetTransferBytes */
    case 0x000D: cmd[0] = 0x000D0080; cmd[1] = 0; cmd[2] = 5120; return 1;           /* GetMaxBytes */
    case 0x000F: cmd[0] = 0x000F0080; cmd[1] = 0; cmd[2] = 0; return 1;              /* IsTrimming */
    case 0x0011: cmd[0] = 0x00110140; cmd[1] = 0; cmd[2] = 0; cmd[3] = 0; cmd[4] = 640; cmd[5] = 480; return 1;   /* GetTrimmingParams */
    }
    cmd[0] = (id << 16) | 0x40; cmd[1] = 0;                                          /* o resto (configurações): ok */
    return 1;
}

/* ---------------- mic:u: microfone ----------------
 * Sem microfone: a amostragem "roda" em silêncio (a memória compartilhada fica zerada) e o evento de buffer
 * cheio dispara de tempos em tempos para quem espera por ele não travar. */
static uint32_t g_mic_evento; static int g_mic_amostrando, g_mic_ganho = 40, g_mic_energia;
static int micu(Ctx *c, uint32_t *cmd) {
    (void)c;
    uint32_t id = cmd[0] >> 16;
    switch (id) {
    case 0x0003: g_mic_amostrando = 1; break;                                        /* StartSampling */
    case 0x0005: g_mic_amostrando = 0; break;                                        /* StopSampling */
    case 0x0006: cmd[0] = 0x00060080; cmd[1] = 0; cmd[2] = g_mic_amostrando; return 1; /* IsSampling */
    case 0x0007:                                                                     /* GetEventHandle */
        if (!g_mic_evento) g_mic_evento = k_new_periodic_event("mic:cheio", 100000000);
        cmd[0] = 0x00070042; cmd[1] = 0; cmd[2] = 0; cmd[3] = g_mic_evento; return 1;
    case 0x0008: g_mic_ganho = cmd[1] & 0xFF; break;                                 /* SetGain */
    case 0x0009: cmd[0] = 0x00090080; cmd[1] = 0; cmd[2] = (uint32_t)g_mic_ganho; return 1;
    case 0x000A: g_mic_energia = cmd[1] & 1; break;                                  /* SetPower */
    case 0x000B: cmd[0] = 0x000B0080; cmd[1] = 0; cmd[2] = (uint32_t)g_mic_energia; return 1;
    case 0x000E: cmd[0] = 0x000E0080; cmd[1] = 0; cmd[2] = 0; return 1;              /* GetClamp */
    }
    cmd[0] = (id << 16) | 0x40; cmd[1] = 0;
    return 1;
}

void ipc(Ctx *c, uint32_t handle) {
    uint32_t *cmd = (uint32_t *)(g_mem + c->tls + 0x80);
    const char *name = k_name(handle);
    uint32_t hdr = cmd[0];
    if (g_ipc_trace < 0) g_ipc_trace = getenv("LOG_IPC") ? atoi(getenv("LOG_IPC")) : 1;
    if (g_ipc_trace)
        fprintf(stderr, "[ipc] %-10s cmd %04x (normal=%u translate=%u)\n", name, hdr >> 16, (hdr >> 6) & 63, hdr & 63);
    if (!strcmp(name, "srv:") && srv(c, cmd)) return;
    if (!strncmp(name, "APT:", 4) && apt(c, cmd)) return;
    if (!strncmp(name, "hid:", 4) && hid(c, cmd)) return;
    if (!strcmp(name, "fs:USER") && fs(c, cmd)) return;
    if (!strcmp(name, "gsp::Gpu") && gsp(c, cmd)) return;
    if (!strcmp(name, "dsp::DSP") && dsp(c, cmd)) return;
    if (!strcmp(name, "ldr:ro") && ldr(c, cmd)) return;
    if (!strncmp(name, "cfg:", 4) && cfg(c, cmd)) return;
    if (!strcmp(name, "y2r:u") && y2r(c, cmd)) return;
    if (!strcmp(name, "cam:u") && camu(c, cmd)) return;
    if (!strcmp(name, "mic:u") && micu(c, cmd)) return;
    if ((!strcmp(name, "fs:arquivo") || !strcmp(name, "fs:pasta")) && fs_file(c, k_get(handle), cmd)) return;
    if (!strcmp(name, "err:f") && hdr >> 16 == 1) {   /* ThrowFatalError: o jogo parou com um erro fatal */
        fprintf(stderr, "[err:f] ERRO FATAL tipo=%u código=%08x pc=%08x lr=%08x; dados:", cmd[1] & 0xFF, cmd[2], cmd[3], c->r[14]);
        for (int k = 4; k < 33; k++) fprintf(stderr, " %08x", cmd[k]);
        fprintf(stderr, "\n[err:f] pilha (sp=%08x):", c->r[13]);
        for (int k = 0; k < 96; k++) fprintf(stderr, " %08x", MEM32(c->r[13] + 4 * k));
        fprintf(stderr, "\n");
        if (!getenv("ERRF_CONTINUA")) exit(9);
    }
    /* serviço ainda não implementado: responde sucesso vazio e segue, registrando no log */
    { static uint32_t vistos[256]; static int nv; uint32_t h = 0;                    /* (cada comando novo aparece 1 vez no log) */
      for (const char *q = name; *q; q++) h = h * 31 + (unsigned char)*q;
      h ^= hdr >> 16; int novo = 1;
      for (int i = 0; i < nv; i++) if (vistos[i] == h) { novo = 0; break; }
      if (novo && nv < 256) { vistos[nv++] = h; if (!g_ipc_trace) fprintf(stderr, "[ipc] %s cmd %04x ainda sem implementação (respondido com sucesso)\n", name, hdr >> 16); } }
    cmd[0] = (hdr & 0xFFFF0000u) | 0x40; cmd[1] = 0;
    for (int k = 2; k < 10; k++) cmd[k] = 0;
}

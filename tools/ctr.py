"""Leitura direta de um dump do 3DS: .3ds/.cci (NCSD) ou .cxi (NCCH), sem ferramentas externas.

Formatos (3dbrew): NCSD (tabela de partições), NCCH (cabeçalho + exheader + ExeFS + RomFS),
ExeFS (.code comprimido com "BLZ", LZSS de trás para frente), RomFS (IVFC, dados no nível 3).

O arquivo precisa estar DECIFRADO (flag NoCrypto). Um dump cifrado pode ser decifrado no próprio
console com o GodMode9 ("Decrypt file"); as chaves do console não podem vir junto com este projeto.

Uso:
  python3 tools/ctr.py info  jogo.3ds
  python3 tools/ctr.py prep  jogo.3ds <pasta>    # code.bin, exheader.bin, módulos do RomFS, romfs.txt
  python3 tools/ctr.py ls    jogo.3ds            # lista o RomFS
"""
import os, struct, sys

MEDIA = 0x200


def _L(pt, en):                                 # mensagens no idioma do lançador (IDIOMA=pt/en)
    return en if os.environ.get('IDIOMA') == 'en' else pt


class CtrError(Exception):
    pass


def blz_decompress(comp):
    """Descompressão do .code (mesma lógica do ctrtool: lê do fim para o começo)."""
    top_bottom, extra = struct.unpack_from('<II', comp, len(comp) - 8)
    out = bytearray(len(comp) + extra)
    out[:len(comp)] = comp
    src = len(comp) - (top_bottom >> 24)
    stop = len(comp) - (top_bottom & 0xFFFFFF)
    dst = len(out)
    while src > stop:
        src -= 1
        ctrl = comp[src]
        for _ in range(8):
            if src <= stop or src <= 0 or dst <= 0:
                break
            if ctrl & 0x80:
                src -= 2
                seg = comp[src] | (comp[src + 1] << 8)
                size = ((seg >> 12) & 0xF) + 3
                dist = (seg & 0xFFF) + 3
                for _ in range(size):
                    dst -= 1
                    out[dst] = out[dst + dist]
            else:
                src -= 1
                dst -= 1
                out[dst] = comp[src]
            ctrl = (ctrl << 1) & 0xFF
    return bytes(out)


class Ncch:
    def __init__(self, f, base):
        self.f, self.base = f, base
        h = self.read(0, 0x200)
        if h[0x100:0x104] != b'NCCH':
            raise CtrError(_L('partição sem cabeçalho NCCH (arquivo corrompido ou cifrado por inteiro)',
                               'partition without an NCCH header (corrupted or fully encrypted file)'))
        self.program_id = struct.unpack_from('<Q', h, 0x118)[0]
        self.exh_size = struct.unpack_from('<I', h, 0x180)[0]
        flags = h[0x188:0x190]
        self.nocrypto = bool(flags[7] & 4)
        self.exefs_off, self.exefs_size = (x * MEDIA for x in struct.unpack_from('<II', h, 0x1A0))
        self.romfs_off, self.romfs_size = (x * MEDIA for x in struct.unpack_from('<II', h, 0x1B0))

    def read(self, off, n):
        self.f.seek(self.base + off)
        d = self.f.read(n)
        if len(d) != n:
            raise CtrError(_L('arquivo truncado', 'truncated file'))
        return d

    def exheader(self):
        return self.read(0x200, 0x800)

    def exefs_files(self):
        h = self.read(self.exefs_off, 0x200)
        files = {}
        for i in range(10):
            name, off, size = struct.unpack_from('<8sII', h, i * 16)
            name = name.rstrip(b'\0').decode('ascii', 'replace')
            if name:
                files[name] = (self.exefs_off + 0x200 + off, size)
        return files

    def code(self):
        exh = self.exheader()
        off, size = self.exefs_files()['.code']
        data = self.read(off, size)
        if exh[0xD] & 1:
            data = blz_decompress(data)
        return data

    def level3_offset(self):
        """Início do nível 3 do IVFC (onde ficam as tabelas e os dados do RomFS), relativo à partição."""
        h = self.read(self.romfs_off, 0x60)
        if h[:4] != b'IVFC':
            raise CtrError(_L('RomFS sem cabeçalho IVFC', 'RomFS without an IVFC header'))
        master = struct.unpack_from('<I', h, 0x08)[0]
        l3_log2 = struct.unpack_from('<I', h, 0x4C)[0]
        bs = 1 << l3_log2
        cand = [(0x60 + master + bs - 1) // bs * bs, (0x5C + master + bs - 1) // bs * bs]
        for c in cand + list(range(0x1000, 0x10000, 0x1000)):
            if struct.unpack_from('<I', self.read(self.romfs_off + c, 4))[0] == 0x28:
                return self.romfs_off + c
        raise CtrError(_L('não achei o nível 3 do RomFS', 'RomFS level 3 not found'))


def open_image(path):
    f = open(path, 'rb')
    head = f.read(0x200)
    if head[0x100:0x104] == b'NCSD':
        off, size = struct.unpack_from('<II', head, 0x120)       # partição 0 = o jogo
        return Ncch(f, off * MEDIA)
    if head[0x100:0x104] == b'NCCH':
        return Ncch(f, 0)
    raise CtrError(_L('não é um .3ds/.cci (NCSD) nem .cxi (NCCH)', 'not a .3ds/.cci (NCSD) or .cxi (NCCH) file'))


class Romfs:
    """Nível 3 do RomFS: diretórios, arquivos e dados."""
    def __init__(self, n):
        self.n = n
        self.l3 = n.level3_offset()
        h = n.read(self.l3, 0x28)
        (_, self.dh_off, self.dh_len, self.dm_off, self.dm_len, self.fh_off, self.fh_len,
         self.fm_off, self.fm_len, self.data_off) = struct.unpack_from('<10I', h)
        self.dm = n.read(self.l3 + self.dm_off, self.dm_len)
        self.fm = n.read(self.l3 + self.fm_off, self.fm_len)

    def _name(self, buf, off, length):
        return buf[off:off + length].decode('utf-16-le')

    def walk(self, d=0, prefix=''):
        """gera (caminho, offset absoluto na imagem, tamanho)"""
        parent, sibling, child, file_ = struct.unpack_from('<4I', self.dm, d)
        f = file_
        while f != 0xFFFFFFFF:
            _, fsib, doff, dsize, _, nlen = struct.unpack_from('<IIQQII', self.fm, f)
            yield prefix + self._name(self.fm, f + 0x20, nlen), self.l3 + self.data_off + doff, dsize
            f = fsib
        c = child
        while c != 0xFFFFFFFF:
            _, csib, _, _, _, nlen = struct.unpack_from('<6I', self.dm, c)
            yield from self.walk(c, prefix + self._name(self.dm, c + 0x18, nlen) + '/')
            c = csib


def check(n):
    if not n.nocrypto:
        raise CtrError(_L('o arquivo está CIFRADO. Decifre o dump no console com o GodMode9 '
                          '("Decrypt file") e use a cópia decifrada.',
                          'the file is ENCRYPTED. Decrypt your dump on the console with GodMode9 '
                          '("Decrypt file") and use the decrypted copy.'))


def prep(path, outdir):
    n = open_image(path)
    check(n)
    os.makedirs(os.path.join(outdir, 'romfs'), exist_ok=True)
    with open(os.path.join(outdir, 'exheader.bin'), 'wb') as o:
        o.write(n.exheader())
    code = n.code()
    with open(os.path.join(outdir, 'code.bin'), 'wb') as o:
        o.write(code)
    rf = Romfs(n)
    count = 0
    for p, off, size in rf.walk():
        # só o que o recompilador precisa: módulos (.cro), static.crs e as listas .crr
        if p.endswith('.cro') or p == 'static.crs' or p.startswith('.crr/'):
            dst = os.path.join(outdir, 'romfs', p)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            n.f.seek(n.base + off)
            with open(dst, 'wb') as o:
                o.write(n.f.read(size))
            count += 1
    # onde está o nível 3 no arquivo: o runtime lê o RomFS direto do .3ds a partir daqui
    with open(os.path.join(outdir, 'romfs.txt'), 'w') as o:
        o.write(f'{n.base + rf.l3} {n.romfs_size}\n')
    print(f'programa {n.program_id:016x}: code.bin {len(code)} bytes, {count} módulos, RomFS em {n.base + rf.l3:#x}')
    return n


def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    cmd, path = sys.argv[1], sys.argv[2]
    try:
        if cmd == 'info':
            n = open_image(path)
            print(f'programa {n.program_id:016x} cifrado={not n.nocrypto} ExeFS={n.exefs_off:#x}+{n.exefs_size:#x} '
                  f'RomFS={n.romfs_off:#x}+{n.romfs_size:#x}')
            if n.nocrypto:
                print('ExeFS:', n.exefs_files())
        elif cmd == 'ls':
            n = open_image(path); check(n)
            for p, off, size in Romfs(n).walk():
                print(f'{off:#014x} {size:12d} {p}')
        elif cmd == 'prep':
            prep(path, sys.argv[3])
        else:
            print(__doc__); sys.exit(1)
    except CtrError as e:
        print('erro:', e); sys.exit(2)


if __name__ == '__main__':
    main()

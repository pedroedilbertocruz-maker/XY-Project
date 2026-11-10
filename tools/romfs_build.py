"""Gera um RomFS virtual (formato "level 3" do 3DS) a partir da pasta romfs extraída.

O jogo abre o próprio RomFS como um arquivo binário e lê as tabelas sozinho, então o runtime
precisa servir uma imagem nesse formato. Para não copiar 1,8 GB, geramos só os metadados
(cabeçalho + tabelas) e um índice "offset -> arquivo"; o runtime lê os dados dos arquivos reais.

Uso: python3 romfs_build.py <pasta_romfs | listagem.json> <saida_dir>
Saída: romfs_meta.bin (cabeçalho e tabelas) e romfs_index.txt (offset tamanho caminho, um por linha).
Formato: https://www.3dbrew.org/wiki/RomFS
"""
import json, os, struct, sys

def hash_count(n):
    if n < 3: return 3
    if n < 19: return n | 1
    c = n
    while any(c % p == 0 for p in (2, 3, 5, 7, 11, 13, 17)):
        c += 1
    return c

def path_hash(parent, name_u16):
    h = parent ^ 123456789
    for ch in name_u16:
        h = ((h >> 5) | (h << 27)) & 0xFFFFFFFF
        h ^= ch
    return h

def u16(name):
    b = name.encode('utf-16-le')
    return b, [struct.unpack_from('<H', b, i)[0] for i in range(0, len(b), 2)]

def pad4(b):
    return b + b'\0' * ((-len(b)) % 4)

class Dir:
    def __init__(self, name, parent):
        self.name, self.parent = name, parent
        self.dirs, self.files = [], []
        self.off = 0

def build_tree(entries):
    root = Dir('', None)
    dirs = {'': root}
    def get(path):
        if path in dirs: return dirs[path]
        parent, _, name = path.rpartition('/')
        d = Dir(name, get(parent)); dirs[path] = d; d.parent.dirs.append(d)
        return d
    for path, size in entries:
        parent, _, name = path.rpartition('/')
        get(parent).files.append([name, size, path, 0])
    return root

def main(src, outdir):
    if os.path.isdir(src):
        entries = []
        for base, ds, fs in os.walk(src):
            for f in fs:
                full = os.path.join(base, f)
                entries.append((os.path.relpath(full, src).replace(os.sep, '/'), os.path.getsize(full)))
    else:
        entries = [(e['name'].replace('\\', '/'), e['size']) for e in json.load(open(src)) if e['type'] == 'file']
    gerar(entries, outdir)


def gerar(entries, outdir, fonte=None, quiet=False):
    """entries: [(caminho, tamanho)]; fonte: caminho -> de onde o runtime lê os dados (padrão: o próprio caminho,
    relativo à pasta ROMFS; "@offset" = no .3ds; caminho absoluto = arquivo de um mod)"""
    root = build_tree(entries)
    # ordem: diretórios em largura; arquivos agrupados por diretório (nomes em ordem)
    order, q = [], [root]
    while q:
        d = q.pop(0); order.append(d)
        d.dirs.sort(key=lambda x: x.name); d.files.sort(key=lambda x: x[0])
        q.extend(d.dirs)
    off = 0
    for d in order:
        d.off = off; off += 0x18 + len(pad4(u16(d.name)[0]))
    dir_meta_len = off
    off = 0; files = []
    for d in order:
        for f in d.files:
            f[3] = off; files.append((d, f)); off += 0x20 + len(pad4(u16(f[0])[0]))
    file_meta_len = off
    # dados: alinhados em 16 bytes
    data_off = 0; data = []
    for d, f in files:
        data.append((data_off, f[1], f[2])); f.append(data_off)
        data_off = (data_off + f[1] + 15) & ~15
    # tabelas de hash
    nd, nf = hash_count(len(order)), hash_count(len(files))
    dir_hash = [0xFFFFFFFF] * nd; file_hash = [0xFFFFFFFF] * nf
    dir_next = {}; file_next = {}
    for d in order:
        h = path_hash(d.parent.off if d.parent else 0, u16(d.name)[1]) % nd
        dir_next[d.off] = dir_hash[h]; dir_hash[h] = d.off
    for d, f in files:
        h = path_hash(d.off, u16(f[0])[1]) % nf
        file_next[f[3]] = file_hash[h]; file_hash[h] = f[3]
    NONE = 0xFFFFFFFF
    dm = b''
    for d in order:
        nb = u16(d.name)[0]
        sib = d.parent.dirs[d.parent.dirs.index(d) + 1].off if d.parent and d.parent.dirs.index(d) + 1 < len(d.parent.dirs) else NONE
        dm += struct.pack('<IIIIII', d.parent.off if d.parent else 0, sib,
                          d.dirs[0].off if d.dirs else NONE, d.files[0][3] if d.files else NONE,
                          dir_next[d.off], len(nb)) + pad4(nb)
    fm = b''
    for d, f in files:
        nb = u16(f[0])[0]
        i = d.files.index(f)
        sib = d.files[i + 1][3] if i + 1 < len(d.files) else NONE
        fm += struct.pack('<IIQQII', d.off, sib, f[4], f[1], file_next[f[3]], len(nb)) + pad4(nb)
    assert len(dm) == dir_meta_len and len(fm) == file_meta_len
    hdr_len = 0x28
    dh = struct.pack(f'<{nd}I', *dir_hash); fh = struct.pack(f'<{nf}I', *file_hash)
    o_dh = hdr_len; o_dm = o_dh + len(dh); o_fh = o_dm + len(dm); o_fm = o_fh + len(fh)
    o_data = (o_fm + len(fm) + 15) & ~15
    header = struct.pack('<10I', hdr_len, o_dh, len(dh), o_dm, len(dm), o_fh, len(fh), o_fm, len(fm), o_data)
    meta = header + dh + dm + fh + fm
    meta += b'\0' * (o_data - len(meta))
    os.makedirs(outdir, exist_ok=True)
    open(os.path.join(outdir, 'romfs_meta.bin'), 'wb').write(meta)
    with open(os.path.join(outdir, 'romfs_index.txt'), 'w', encoding='utf-8', newline='\n') as o:
        for doff, size, path in data:
            o.write(f'{o_data + doff} {size} {fonte.get(path, path) if fonte else path}\n')
    if not quiet: print(f'RomFS virtual: {len(order)} pastas, {len(files)} arquivos, metadados {len(meta)} bytes, '
          f'imagem total {(o_data + data_off) / 1e6:.1f} MB')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])

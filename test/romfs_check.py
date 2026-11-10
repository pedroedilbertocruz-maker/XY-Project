"""Lê o RomFS virtual como o jogo faria (pelas tabelas de hash) e confere cada arquivo do índice."""
import struct, sys
meta = open(sys.argv[1] + '/romfs_meta.bin', 'rb').read()
idx = [l.split(' ', 2) for l in open(sys.argv[1] + '/romfs_index.txt').read().splitlines()]
h = struct.unpack_from('<10I', meta, 0)
o_dh, l_dh, o_dm, l_dm, o_fh, l_fh, o_fm, l_fm, o_data = h[1:]
def H(parent, name):
    x = parent ^ 123456789
    for ch in name.encode('utf-16-le')[::2] if False else [ord(c) for c in name]:
        x = ((x >> 5) | (x << 27)) & 0xFFFFFFFF; x ^= ch
    return x
def find_dir(parent, name):
    n = l_dh // 4; off = struct.unpack_from('<I', meta, o_dh + 4 * (H(parent, name) % n))[0]
    while off != 0xFFFFFFFF:
        p, sib, child, fil, nxt, nl = struct.unpack_from('<6I', meta, o_dm + off)
        if p == parent and meta[o_dm + off + 0x18:o_dm + off + 0x18 + nl].decode('utf-16-le') == name: return off
        off = nxt
def find_file(parent, name):
    n = l_fh // 4; off = struct.unpack_from('<I', meta, o_fh + 4 * (H(parent, name) % n))[0]
    while off != 0xFFFFFFFF:
        p, sib, doff, size, nxt, nl = struct.unpack_from('<IIQQII', meta, o_fm + off)
        if p == parent and meta[o_fm + off + 0x20:o_fm + off + 0x20 + nl].decode('utf-16-le') == name: return doff, size
        off = nxt
root = find_dir(0, '')
ok = 0
for off, size, path in idx:
    d = root
    *parts, fname = path.split('/')
    for p in parts: d = find_dir(d, p); assert d is not None, path
    r = find_file(d, fname)
    assert r is not None, path
    assert o_data + r[0] == int(off) and r[1] == int(size), (path, r, off, size)
    ok += 1
# percorre a árvore pelos ponteiros de filho/irmão e conta os arquivos
def walk(d):
    p, sib, child, fil, nxt, nl = struct.unpack_from('<6I', meta, o_dm + d); n = 0
    f = fil
    while f != 0xFFFFFFFF: n += 1; f = struct.unpack_from('<I', meta, o_fm + f + 4)[0]
    c = child
    while c != 0xFFFFFFFF: n += walk(c); c = struct.unpack_from('<I', meta, o_dm + c + 4)[0]
    return n
print(f'{ok}/{len(idx)} arquivos encontrados pelo hash; percorrendo a árvore: {walk(root)}')

"""Monta um .3ds DECIFRADO de teste a partir de peças já extraídas, para validar tools/ctr.py e o
modo de leitura direta do runtime sem precisar do arquivo original.
O .code é comprimido com um compressor BLZ independente (escrito aqui), e o RomFS recebe o
cabeçalho IVFC e o nível 3 (tabelas de romfs_virtual + dados dos arquivos, gravados esparsos).
Uso: python3 test/make_test_3ds.py code.bin exheader.bin romfs_virtual/ pasta_romfs saida.3ds"""
import os, struct, sys

def blz_compress(data):
    r = data[::-1]
    n = len(r)
    head = {}
    tokens = []                       # (literal byte) ou (dist, len)
    i = 0
    while i < n:
        best_len, best_dist = 0, 0
        if i + 3 <= n:
            key = r[i:i + 3]
            for j in reversed(head.get(key, [])[-16:]):
                d = i - j
                if d < 3 or d > 0x1002: continue
                l = 0
                while l < 18 and i + l < n and r[j + l] == r[i + l]: l += 1
                if l > best_len: best_len, best_dist = l, d
            head.setdefault(key, []).append(i)
        if best_len >= 3:
            tokens.append((best_dist, best_len))
            for k in range(1, best_len):
                if i + k + 3 <= n: head.setdefault(r[i + k:i + k + 3], []).append(i + k)
            i += best_len
        else:
            tokens.append(r[i]); i += 1
    stream = bytearray()
    for b in range(0, len(tokens), 8):
        grp = tokens[b:b + 8]
        ctrl = 0
        body = bytearray()
        for k, t in enumerate(grp):
            if isinstance(t, tuple):
                ctrl |= 0x80 >> k
                seg = ((t[1] - 3) << 12) | (t[0] - 3)
                body += bytes([seg >> 8, seg & 0xFF])
            else:
                body.append(t)
        stream.append(ctrl); stream += body
    body = bytes(stream[::-1])
    pad = (-len(body)) % 4
    bottom = 8 + pad
    top = len(body) + bottom
    comp = body + b'\xff' * pad
    total = len(comp) + 8
    extra = len(data) - total
    assert extra >= 0
    return comp + struct.pack('<II', (bottom << 24) | top, extra)

def main():
    code, exh, vdir, rdir, out = sys.argv[1:6]
    code = open(code, 'rb').read()
    exh = bytearray(open(exh, 'rb').read()[:0x800].ljust(0x800, b'\0'))
    exh[0xD] |= 1
    comp = blz_compress(code)
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
    from ctr import blz_decompress
    assert blz_decompress(comp) == code, 'BLZ não volta ao original'
    print(f'.code: {len(code)} -> {len(comp)} bytes (ida e volta ok)')
    MU = 0x200
    al = lambda x, a=MU: (x + a - 1) // a * a
    # ExeFS
    exefs = bytearray(0x200)
    exefs[0:8] = b'.code\0\0\0'; struct.pack_into('<II', exefs, 8, 0, len(comp))
    exefs += comp; exefs = exefs.ljust(al(len(exefs)), b'\0')
    # RomFS: IVFC + nível 3 alinhado a 0x1000
    meta = open(os.path.join(vdir, 'romfs_meta.bin'), 'rb').read()
    idx = [l.split(None, 2) for l in open(os.path.join(vdir, 'romfs_index.txt'))]
    l3size = max(int(o) + int(s) for o, s, _ in idx)
    master = 0x20
    ivfc = bytearray(0x60)
    ivfc[0:4] = b'IVFC'; struct.pack_into('<II', ivfc, 4, 0x10000, master)
    struct.pack_into('<QQI', ivfc, 0x3C, 0, l3size, 12)
    l3_rel = al(0x60 + master, 0x1000)
    romfs_size = al(l3_rel + l3size)
    ncch_off = 0x4000
    exh_off, exefs_off = 0x200, 0xA00
    romfs_off = al(exefs_off + len(exefs), 0x1000)
    ncch_size = romfs_off + romfs_size
    h = bytearray(0x200)
    h[0x100:0x104] = b'NCCH'; struct.pack_into('<I', h, 0x104, ncch_size // MU)
    struct.pack_into('<Q', h, 0x118, 0x0004000000055D00)
    struct.pack_into('<I', h, 0x180, 0x400)
    h[0x188 + 7] = 4                                     # NoCrypto
    struct.pack_into('<II', h, 0x1A0, exefs_off // MU, len(exefs) // MU)
    struct.pack_into('<II', h, 0x1B0, romfs_off // MU, romfs_size // MU)
    ncsd = bytearray(0x200)
    ncsd[0x100:0x104] = b'NCSD'; struct.pack_into('<I', ncsd, 0x104, (ncch_off + ncch_size) // MU)
    struct.pack_into('<II', ncsd, 0x120, ncch_off // MU, ncch_size // MU)
    with open(out, 'wb') as o:
        o.write(ncsd)
        o.seek(ncch_off); o.write(h); o.write(exh); o.seek(ncch_off + exefs_off); o.write(exefs)
        base = ncch_off + romfs_off
        o.seek(base); o.write(ivfc)
        o.seek(base + l3_rel); o.write(meta)
        for off, size, path in idx:
            p = os.path.join(rdir, path.strip())
            if os.path.exists(p) and os.path.getsize(p) == int(size):
                o.seek(base + l3_rel + int(off))
                with open(p, 'rb') as f:
                    while True:
                        b = f.read(1 << 24)
                        if not b: break
                        o.write(b)
        o.truncate(ncch_off + ncch_size)
    print('gerado', out, f'nível 3 em {base + l3_rel:#x}')

main()

"""Linker estático: code.bin + static.crs + todos os .cro -> uma imagem de memória única.

Cada CRO recebe um endereço fixo (a partir de CRO_BASE), relocações internas e imports
entre módulos são aplicados, e o resultado vira um mapa de memória que o recompilador usa.
"""
import struct, json, glob, os, sys, collections
sys.path.insert(0, os.path.dirname(__file__))
from cro import CRO, SEG_NAMES, RELOC_NAMES, tag

CRO_BASE = 0x40000000  # fora do espaço que o jogo usa (o ldr:ro do jogo mapeia CROs logo após o code.bin)

def align(x, a=0x1000):
    return (x + a - 1) & ~(a - 1)

class Image:
    def __init__(self):
        self.regions = []  # (addr, bytearray, name, kind)
        self.ptrs = []     # (onde, alvo) de cada relocação aplicada
    def add(self, addr, data, name, kind):
        self.regions.append([addr, bytearray(data), name, kind])
    def find(self, addr):
        for r in self.regions:
            if r[0] <= addr < r[0] + len(r[1]):
                return r
        raise KeyError(hex(addr))
    def r32(self, a):
        r = self.find(a); return struct.unpack_from('<I', r[1], a - r[0])[0]
    def w32(self, a, v):
        r = self.find(a); struct.pack_into('<I', r[1], a - r[0], v & 0xFFFFFFFF)

def apply(img, where, typ, sym, stats, issues):
    sym &= 0xFFFFFFFF
    stats[RELOC_NAMES.get(typ, typ)] += 1
    if typ == 0:
        return
    img.ptrs.append((where, sym))
    if typ in (2, 38):          # endereço absoluto
        img.w32(where, sym)
    elif typ == 3:              # relativo
        img.w32(where, sym - where)
    elif typ in (28, 29):       # BL/B em ARM
        old = img.r32(where)
        off = (sym - (where + 8))
        if sym & 1:             # alvo Thumb: BL vira BLX
            off = (sym & ~1) - (where + 8)
            h = (off >> 1) & 1
            img.w32(where, 0xFA000000 | (h << 24) | ((off >> 2) & 0xFFFFFF))
        else:
            img.w32(where, (old & 0xFF000000) | ((off >> 2) & 0xFFFFFF))
        if not -0x2000000 <= off < 0x2000000:
            issues.append(f'branch fora de alcance em {where:#x} -> {sym:#x} (precisa de veneer)')
    elif typ == 42:
        img.w32(where, (img.r32(where) & 0x80000000) | ((sym - where) & 0x7FFFFFFF))
    elif typ == 10:
        issues.append(f'THM_CALL em {where:#x} não implementado')
    else:
        issues.append(f'tipo {typ} em {where:#x} não implementado')

def build(code_bin, exheader_info, romfs, outdir):
    img = Image()
    code = open(code_bin, 'rb').read()
    crs = CRO(os.path.join(romfs, 'static.crs'), 'static')
    text_end = 0x100000 + 0x43C000; ro_end = text_end + 0x6A000
    img.add(0x100000, code[:0x43C000], 'static', 'text')
    img.add(0x53C000, code[0x43C000:0x4A6000], 'static', 'rodata')
    data = code[0x4A6000:]
    img.add(0x5A6000, data + b'\0' * (0xBA918 - len(data)), 'static', 'data+bss')
    # o CRS também mapeia uma área de 0x1f4 bytes em 0x4000000 (rodata extra)
    seg_addr = {'|static|': [s[0] for s in crs.segments]}
    mods = {'|static|': crs}
    layout = []
    addr = CRO_BASE
    # módulos de teste dos desenvolvedores (DllTest*) vão por último, para o endereço dos outros não
    # depender de eles estarem no dump; arquivos que não são CRO válidos são ignorados com aviso
    paths = sorted(glob.glob(os.path.join(romfs, '*.cro')), key=lambda p: (os.path.basename(p).startswith('DllTest'), p))
    for p in paths:
        try:
            c = CRO(p)
        except AssertionError as e:
            print('aviso: módulo ignorado:', e); continue
        base = addr
        img.add(base, c.raw, c.name, 'cro')
        bss = base + align(len(c.raw))
        addrs = []
        for off, sz, sid in c.segments:
            addrs.append(bss if sid == 3 and sz else base + off)
        if c.bss_size:
            img.add(bss, b'\0' * c.bss_size, c.name, 'bss')
        seg_addr[c.name] = addrs
        mods[c.name] = c
        end = bss + align(c.bss_size)
        layout.append({'name': c.name, 'base': base, 'end': end,
                       'segments': [{'kind': SEG_NAMES.get(sid, sid), 'addr': a, 'size': sz}
                                    for (off, sz, sid), a in zip(c.segments, addrs) if sz]})
        addr = align(end, 0x10000)
    stats = collections.Counter(); issues = []
    def resolve(mod, t):
        i, off = tag(t); return seg_addr[mod][i] + off
    unresolved = collections.Counter()
    for name, c in mods.items():
        for t, typ, symseg, add in c.relocs:
            apply(img, resolve(name, t), typ, seg_addr[name][symseg] + add, stats, issues)
        for mname, idx, anon in c.import_modules:
            if mname not in mods:
                unresolved[mname] += len(idx) + len(anon); continue
            src = mods[mname]
            for i, batch in idx:
                sym = resolve(mname, src.exports_indexed[i])
                for t, typ, add in batch:
                    apply(img, resolve(name, t), typ, sym + add, stats, issues)
            for symtag, batch in anon:
                sym = resolve(mname, symtag)
                for t, typ, add in batch:
                    apply(img, resolve(name, t), typ, sym + add, stats, issues)
        for sname, batch in c.imports_named:
            hit = None
            for mn, m in mods.items():
                for en, et in m.exports_named:
                    if en == sname: hit = resolve(mn, et)
            if hit is None:
                unresolved[sname] += 1; continue
            for t, typ, add in batch:
                apply(img, resolve(name, t), typ, hit + add, stats, issues)
    symbols = {}
    for mn, m in mods.items():
        for en, et in m.exports_named:
            symbols[f'{mn}::{en}' if en.startswith('nnro') else en] = resolve(mn, et)
        if mn != '|static|':
            for k, v in (('OnLoad', m.on_load), ('OnExit', m.on_exit), ('OnUnresolved', m.on_unresolved)):
                if v != 0xFFFFFFFF: symbols[f'{mn}::{k}'] = resolve(mn, v)
    os.makedirs(outdir, exist_ok=True)
    regions = []
    for a, b, n, k in sorted(img.regions):
        fn = f'{a:08x}_{n.strip("|")}_{k.replace("+", "_")}.bin'
        open(os.path.join(outdir, fn), 'wb').write(b)
        regions.append({'addr': a, 'size': len(b), 'module': n, 'kind': k, 'file': fn})
    json.dump({'regions': regions, 'modules': layout, 'symbols': symbols,
               'reloc_stats': stats, 'issues': issues[:200], 'issue_count': len(issues),
               'unresolved': unresolved}, open(os.path.join(outdir, 'map.json'), 'w'), indent=1)
    return img, layout, stats, issues, unresolved, symbols

if __name__ == '__main__':
    img, layout, stats, issues, unres, syms = build(sys.argv[1], None, sys.argv[2], sys.argv[3])
    print('módulos:', len(layout), 'fim da área de CROs:', hex(layout[-1]['end']))
    print('relocações aplicadas:', dict(stats))
    print('problemas:', len(issues), issues[:5])
    print('não resolvidos:', dict(unres))
    print('símbolos:', len(syms))

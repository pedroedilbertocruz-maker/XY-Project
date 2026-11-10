"""Parser do formato CRO0 (módulos dinâmicos do 3DS) e do static.crs."""
import struct

SEG_NAMES = {0: 'text', 1: 'rodata', 2: 'data', 3: 'bss'}
RELOC_NAMES = {0: 'NONE', 2: 'ABS32', 3: 'REL32', 10: 'THM_CALL', 28: 'ARM_CALL',
               29: 'ARM_JUMP24', 38: 'TARGET1', 42: 'PREL31'}

def tag(t):
    return t & 0xF, t >> 4

class CRO:
    def __init__(self, path, name=None):
        self.path = path
        self.raw = open(path, 'rb').read()
        d = self.raw
        assert d[0x80:0x84] == b'CRO0', f'{path}: magic inválido {d[0x80:0x84]!r}'
        u = lambda o: struct.unpack_from('<I', d, o)[0]
        self.u = u
        self.file_size = u(0x90)
        self.bss_size = u(0x94)
        self.ctrl_obj, self.on_load, self.on_exit, self.on_unresolved = u(0xA0), u(0xA4), u(0xA8), u(0xAC)
        self.code_off, self.code_size = u(0xB0), u(0xB4)
        self.data_off, self.data_size = u(0xB8), u(0xBC)
        self.name = self.cstr(u(0xC0)) if u(0xC4) else (name or '?')
        H = {}
        for i, k in enumerate(['segment', 'export_named', 'export_indexed', 'export_strings',
                               'export_tree', 'import_module', 'import_patch', 'import_named',
                               'import_indexed', 'import_anon', 'import_strings', 'static_anchor',
                               'reloc', 'static_reloc']):
            H[k] = (u(0xC8 + i * 8), u(0xCC + i * 8))
        self.H = H
        o, n = H['segment']
        self.segments = [struct.unpack_from('<III', d, o + i * 12) for i in range(n)]  # (offset,size,id)
        o, n = H['export_named']
        self.exports_named = []
        for i in range(n):
            no, t = struct.unpack_from('<II', d, o + i * 8)
            self.exports_named.append((self.cstr(no), t))
        o, n = H['export_indexed']
        self.exports_indexed = [u(o + i * 4) for i in range(n)]
        o, n = H['import_named']
        self.imports_named = []
        for i in range(n):
            no, batch = struct.unpack_from('<II', d, o + i * 8)
            self.imports_named.append((self.cstr(no), self.batch(batch)))
        o, n = H['import_module']
        self.import_modules = []
        for i in range(n):
            no, io, inum, ao, anum = struct.unpack_from('<IIIII', d, o + i * 20)
            idx = [(struct.unpack_from('<I', d, io + j * 8)[0],
                    self.batch(struct.unpack_from('<I', d, io + j * 8 + 4)[0])) for j in range(inum)]
            anon = [(struct.unpack_from('<I', d, ao + j * 8)[0],
                     self.batch(struct.unpack_from('<I', d, ao + j * 8 + 4)[0])) for j in range(anum)]
            self.import_modules.append((self.cstr(no), idx, anon))
        o, n = H['reloc']
        self.relocs = []
        for i in range(n):
            t, typ, symseg, _, _, add = struct.unpack_from('<IBBBBI', d, o + i * 12)
            self.relocs.append((t, typ, symseg, add))

    def cstr(self, o):
        e = self.raw.index(b'\0', o)
        return self.raw[o:e].decode('ascii', 'replace')

    def batch(self, o):
        """Lista de patches (alvo, tipo, addend) até a marca de fim de lote."""
        out = []
        while True:
            t, typ, last, _, _, add = struct.unpack_from('<IBBBBI', self.raw, o)
            out.append((t, typ, add))
            if last:
                return out
            o += 12

    def summary(self):
        segs = ', '.join(f'{SEG_NAMES.get(i, i)}@{off:#x}+{sz:#x}' for off, sz, i in self.segments)
        return (f'{self.name}: {segs}; exports {len(self.exports_named)} nomeados/'
                f'{len(self.exports_indexed)} indexados; imports {len(self.imports_named)} nomeados, '
                f'{len(self.import_modules)} módulos; relocs {len(self.relocs)}')

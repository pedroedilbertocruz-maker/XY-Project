"""Empacota as regiões da imagem linkada (out/image) num único arquivo que o runtime carrega,
e escreve modules.txt (layout de cada CRO) para o ldr:ro do runtime.
Linha de modules.txt: nome base tamanho_arquivo seg0_addr seg0_size seg0_id seg1_addr ...  (6 segmentos)"""
import json, os, struct, sys
d = sys.argv[1]
m = json.load(open(os.path.join(d, 'map.json')))
with open(sys.argv[2], 'wb') as o:
    for r in m['regions']:
        data = open(os.path.join(d, r['file']), 'rb').read()
        o.write(struct.pack('<II', r['addr'], len(data)) + data)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cro import CRO
romfs = sys.argv[3] if len(sys.argv) > 3 else None
with open(os.path.join(os.path.dirname(sys.argv[2]), 'modules.txt'), 'w') as o:
    regs = {(r['module'], r['kind']): r for r in m['regions']}
    for mod in m['modules']:
        segs = []
        file_size = regs[(mod['name'], 'cro')]['size']
        raw = open(os.path.join(d, regs[(mod['name'], 'cro')]['file']), 'rb').read()
        c = CRO.__new__(CRO); c.raw = raw
        n = struct.unpack_from('<I', raw, 0xCC)[0]; off = struct.unpack_from('<I', raw, 0xC8)[0]
        bss = mod['base'] + ((file_size + 0xFFF) & ~0xFFF)
        for i in range(6):
            if i < n:
                so, sz, sid = struct.unpack_from('<III', raw, off + 12 * i)
                segs += [bss if sid == 3 else mod['base'] + so, sz, sid]
            else:
                segs += [0, 0, 0]
        o.write(f"{mod['name']} {mod['base']} {file_size} " + ' '.join(str(x) for x in segs) + '\n')
print('regiões:', len(m['regions']), 'módulos:', len(m['modules']))

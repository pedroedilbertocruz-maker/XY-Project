"""Roda o recompilador sobre code.bin + todos os CROs e gera um .c por módulo."""
import bisect, collections, json, os, struct, sys, time
sys.path.insert(0, os.path.dirname(__file__))
import link, funcs
from recomp import Recompiler

code_bin, romfs, outdir = sys.argv[1:4]
img, layout, _stats, _issues, _unres, symbols = link.build(code_bin, None, romfs, os.path.join(outdir, 'image'))
rs, arm, thumb = funcs.discover(img, layout)
regions = sorted((r[0], bytes(r[1])) for r in img.regions)
starts = [r[0] for r in regions]

def read32(a):
    k = bisect.bisect_right(starts, a) - 1
    if k >= 0:
        base, d = regions[k]
        if a + 4 <= base + len(d):
            return struct.unpack_from('<I', d, a - base)[0]
    return 0

tstarts = sorted(rs)
tkeys = [s for s, e, n in tstarts]
def module_of(a):
    k = bisect.bisect_right(tkeys, a) - 1
    if k >= 0 and a < tstarts[k][1]:
        return tstarts[k][2]
def is_code(a):
    return module_of(a) is not None

# fecha o conjunto de funções: chamadas descobertas na tradução podem revelar funções novas
arm_set, thumb_set = set(arm), set(thumb)
# exports dos módulos e pontos de entrada do cabeçalho (nnroControlObject_, OnLoad...): a biblioteca
# de módulos do jogo chama essas funções por ponteiro calculado a partir do cabeçalho do CRO
for sname, a in symbols.items():
    if is_code(a & ~1):
        (thumb_set if a & 1 else arm_set).add(a & ~1)
# sementes extras registradas pelo runtime (alvos indiretos que a análise estática não achou)
for extra in (os.path.join(outdir, 'extra_funcs.txt'),
              os.path.join(os.path.dirname(os.path.abspath(__file__)), 'extra_funcs_conhecidas.txt')):
    if not os.path.exists(extra): continue
    for l in open(extra):
        if not l.strip(): continue
        a = int(l.split()[0], 16)
        if is_code(a & ~1):
            (thumb_set if a & 1 else arm_set).add(a & ~1)
# pontos de entrada secundários: geram função (para chamadas indiretas), mas saltos internos continuam goto
secondary = {a for a in getattr(funcs.discover, 'secondary', set()) if a not in arm_set}
for rnd in range(10):
    rc = Recompiler(read32, arm_set, is_code, thumb_set)
    for f in sorted(arm_set | secondary): rc.function(f)
    for f in sorted(thumb_set): rc.function(f, thumb=True)
    new_a = {a for a in rc.calls if is_code(a)} - arm_set
    new_t = {a for a in rc.tcalls if is_code(a)} - thumb_set
    print(f'rodada {rnd}: {len(arm_set)} ARM, {len(thumb_set)} Thumb; novas: {len(new_a)} ARM, {len(new_t)} Thumb')
    if not new_a and not new_t: break
    arm_set |= new_a; thumb_set |= new_t
secondary -= arm_set
arm, thumb = arm_set | secondary, thumb_set

rc = Recompiler(read32, arm_set, is_code, thumb_set)   # só arm_set vira alvo de tail call
by_mod = collections.defaultdict(list)
for f in sorted(arm):
    by_mod[module_of(f)].append((f, False))
for f in sorted(thumb):
    by_mod[module_of(f)].append((f, True))

src = os.path.join(outdir, 'src'); os.makedirs(src, exist_ok=True)
changed_files = []
def write_if_changed(path, text):
    """Só reescreve o .c se o conteúdo mudou, para o make recompilar apenas o necessário."""
    if os.path.exists(path) and open(path).read() == text:
        return
    open(path, 'w').write(text)
    changed_files.append(os.path.basename(path))
t0 = time.time()
per_mod = {}
for mod, fs in sorted(by_mod.items()):
    before = dict(rc.stats)
    name = mod.strip('|')
    CHUNK = 1500   # funções por arquivo .c, para compilar em paralelo e com pouca memória
    for k in range(0, len(fs), CHUNK):
        part = fs[k:k + CHUNK]
        rc.calls, rc.tcalls = set(), set()
        bodies = [rc.function(f, thumb=t)[0] for f, t in part]
        suffix = f'_{k // CHUNK:03d}' if len(fs) > CHUNK else ''
        text = ['#include "rt.h"\n']
        for t in sorted(rc.calls | {f for f, th in part if not th}):
            text.append(f'void F_{t:08x}(Ctx *c);\n')
        for t in sorted(rc.tcalls | {f for f, th in part if th}):
            text.append(f'void T_{t:08x}(Ctx *c);\n')
        text.append('\n\n'.join(bodies) + '\n')
        write_if_changed(os.path.join(src, f'{name}{suffix}.c'), ''.join(text))
    per_mod[name] = {k: rc.stats[k] - before[k] for k in rc.stats}
import io
with io.StringIO() as o:
    o.write('#include "rt.h"\n')
    for f in sorted(arm):
        o.write(f'void F_{f:08x}(Ctx *c);\n')
    for f in sorted(thumb):
        o.write(f'void T_{f:08x}(Ctx *c);\n')
    entries = sorted([(f, f'F_{f:08x}') for f in arm] + [(f | 1, f'T_{f:08x}') for f in thumb])
    o.write('const struct FuncEntry { uint32_t addr; void (*fn)(Ctx *); } g_functable[] = {\n')
    for a, n in entries:
        o.write(f'  {{0x{a:08x}u, {n}}},\n')
    o.write(f'}};\nconst int g_functable_len = {len(entries)};\n')
    write_if_changed(os.path.join(src, 'functable.c'), o.getvalue())
s = rc.stats
print(f'arquivos .c alterados: {len(changed_files)}', changed_files[:5])
print(f'{s["funcs"]} funções, {s["insns"]} instruções, {s["unimpl"]} não suportadas '
      f'({100 * (1 - s["unimpl"] / s["insns"]):.2f}% traduzido) em {time.time() - t0:.0f}s')
top = sorted(rc.unimpl_hist.items(), key=lambda kv: -kv[1])[:15]
print('bits 27:20 mais comuns entre as não suportadas:', top)
json.dump({'total': s, 'modules': per_mod, 'unimpl_hist': rc.unimpl_hist, 'thumb_functions': sorted(thumb)},
          open(os.path.join(outdir, 'recomp_stats.json'), 'w'), indent=1)

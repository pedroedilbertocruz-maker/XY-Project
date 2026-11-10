"""Teste diferencial do recompilador.

1. compila tests.c para ARM11 MPCore (CPU do 3DS) com VFPv2 e linka num ELF
2. recompila o código de máquina ARM para C com tools/recomp.py
3. compila tests.c nativo (x86) e compara cada resultado com o código recompilado
"""
import os, re, struct, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'tools'))
from recomp import Recompiler

B = os.path.join(HERE, 'build'); os.makedirs(B, exist_ok=True)
def sh(*a):
    subprocess.run(a, check=True)

MODE = os.environ.get('MODE', 'arm')
ARMFLAGS = ['--target=armv6k-none-eabi', '-mcpu=mpcore', '-mthumb' if MODE == 'thumb' else '-marm',
            '-mfpu=vfpv2', '-mfloat-abi=hard'] + (['-DTHUMB_TEST'] if MODE == 'thumb' else []) + [
            '-O2', '-ffreestanding', '-fno-builtin']
sh('clang', *ARMFLAGS, '-c', f'{HERE}/tests.c', '-o', f'{B}/tests_arm.o')
sh('ld.lld', '-Ttext=0x100000', '--entry=0', '-static', '-nostdlib', f'{B}/tests_arm.o', '-o', f'{B}/tests_arm.elf')

# ---- leitor de ELF32 mínimo
elf = open(f'{B}/tests_arm.elf', 'rb').read()
shoff, = struct.unpack_from('<I', elf, 0x20)
shentsize, shnum, shstrndx = struct.unpack_from('<HHH', elf, 0x2E)
secs = []
for k in range(shnum):
    secs.append(struct.unpack_from('<IIIIIIIIII', elf, shoff + k * shentsize))
strtab_sec = secs[shstrndx]
def nm(off, tab): e = elf.index(b'\0', tab[4] + off); return elf[tab[4] + off:e].decode()
mem = {}
loaded = []
for s in secs:
    name = nm(s[0], strtab_sec)
    if s[3] and s[1] in (1, 8) and s[2] & 2:   # SHF_ALLOC
        data = elf[s[4]:s[4] + s[5]] if s[1] == 1 else b'\0' * s[5]
        loaded.append((name, s[3], data, bool(s[2] & 4)))
syms = {}
for s in secs:
    if s[1] == 2:
        strs = secs[s[6]]
        for k in range(s[5] // 16):
            n, val, size, info, oth, shn = struct.unpack_from('<IIIBBH', elf, s[4] + k * 16)
            if info & 0xF == 2:
                syms[nm(n, strs)] = val
text = [(a, a + len(d)) for n, a, d, x in loaded if x]
image = {}
for n, a, d, x in loaded:
    image[a] = d
def read32(a):
    for base, d in image.items():
        if base <= a < base + len(d) - 3:
            return struct.unpack_from('<I', d, a - base)[0]
    return 0
is_code = lambda a: any(s <= a < e for s, e in text)

# ---- recompilação
funcs = sorted(v for v in set(syms.values()) if not v & 1)
tfuncs = sorted(v & ~1 for v in set(syms.values()) if v & 1)
rc = Recompiler(read32, set(funcs), is_code, set(tfuncs))
fname = lambda v: f'T_{v & ~1:08x}' if v & 1 else f'F_{v:08x}'
bodies = []
done_a, done_t = set(), set()
while True:   # fecha o conjunto: traduz também funções descobertas só pelas chamadas
    todo_a = (set(funcs) | rc.calls) - done_a
    todo_t = (set(tfuncs) | rc.tcalls) - done_t
    if not todo_a and not todo_t: break
    for f in sorted(todo_a): bodies.append(rc.function(f)[0]); done_a.add(f)
    for f in sorted(todo_t): bodies.append(rc.function(f, thumb=True)[0]); done_t.add(f)
with open(f'{B}/recompiled.c', 'w') as o:
    o.write('#include "rt.h"\n')
    for f in sorted(done_a): o.write(f'void F_{f:08x}(Ctx *c);\n')
    for f in sorted(done_t): o.write(f'void T_{f:08x}(Ctx *c);\n')
    o.write('\n\n'.join(bodies))
    o.write('\n\nconst struct { uint32_t addr; void (*fn)(Ctx *); } g_functable[] = {\n')
    for f in sorted(done_a): o.write(f'  {{0x{f:08x}u, F_{f:08x}}},\n')
    for f in sorted(done_t): o.write(f'  {{0x{f | 1:08x}u, T_{f:08x}}},\n')
    o.write('};\nconst int g_functable_len = %d;\n' % (len(done_a) + len(done_t)))

# ---- harness
cases = re.findall(r'X\((\w+), ([^,]+), ([^)]+)\)', open(f'{HERE}/cases.h').read())
h = ['#include "rt.h"', '#include <stdio.h>', '#include <stdlib.h>', '#include <sys/mman.h>', '#include <inttypes.h>',
     'uint8_t *g_mem;', 'extern const struct { uint32_t addr; void (*fn)(Ctx *); } g_functable[];',
     'extern const int g_functable_len;',
     'void call_indirect(Ctx *c, uint32_t a) { for (int i = 0; i < g_functable_len; i++) '
     'if (g_functable[i].addr == a) { g_functable[i].fn(c); return; } '
     'fprintf(stderr, "call_indirect: %08x desconhecido\\n", a); exit(2); }',
     'void svc(Ctx *c, uint32_t n) { fprintf(stderr, "svc %x\\n", n); exit(2); }',
     'void UNIMPL(Ctx *c, uint32_t a, uint32_t w) { fprintf(stderr, "instrução não suportada %08x: %08x\\n", a, w); exit(3); }']
for n, a, d, x in loaded:
    h.append(f'static const uint8_t sec_{n.strip(".").replace(".", "_")}[] = {{{",".join(str(b) for b in d) or "0"}}};')
protos = {'t_mul64': 'int64_t nat_t_mul64(int32_t, int32_t);', 't_float': 'float nat_t_float(float, float);',
          't_double': 'double nat_t_double(double, int);', 't_fcmp': 'int nat_t_fcmp(float, float);',
          't_f2i': 'int nat_t_f2i(float);', 't_u64': 'uint64_t nat_t_u64(uint64_t, uint64_t);',
          }
for t in sorted(set(c[0] for c in cases) | set(protos)):
    if t not in syms: continue
    h.append(protos.get(t, f'int nat_{t}(int, int);'))
h.extend(f'void {fname(a)}(Ctx *c);' for a in sorted(set(syms.values())))
h.append('static int fails = 0, total = 0;')
h.append('static void reset(Ctx *c) { memset(c, 0, sizeof *c); c->r[13] = 0x0fff0000u; c->r[14] = 0xdeadbeef;')
for n, a, d, x in loaded:
    h.append(f'  memcpy(g_mem + 0x{a:x}, sec_{n.strip(".").replace(".", "_")}, {len(d)});')
h.append('}')
h.append('#define CHECK(name, got, exp, fmt) do { total++; if ((got) != (exp)) { fails++; '
         'printf("FALHOU %-10s recompilado=" fmt " nativo=" fmt "\\n", name, got, exp); } } while (0)')
h.append('int main(void) { Ctx c; g_mem = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, '
         'MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);')
for t, a, b in cases:
    if t not in syms: continue
    h.append(f'  reset(&c); c.r[0] = (uint32_t)({a}); c.r[1] = (uint32_t)({b}); {fname(syms[t])}(&c); '
             f'CHECK("{t}", (int)c.r[0], (int)nat_{t}({a}, {b}), "%d");')
fcases = [('t_float', '2.5f', '-1.25f'), ('t_float', '100.0f', '3.0f'), ('t_fcmp', '1.0f', '2.0f'),
          ('t_fcmp', '2.0f', '2.0f'), ('t_fcmp', '3.0f', '-2.0f'), ('t_f2i', '7.3f', None), ('t_f2i', '-2.9f', None)]
for t, a, b in fcases:
    if t not in syms: continue
    setup = f'c.s[0] = {a};' + (f' c.s[1] = {b};' if b else '')
    call = f'nat_{t}({a}' + (f', {b})' if b else ')')
    if t == 't_float':
        h.append(f'  reset(&c); {setup} {fname(syms[t])}(&c); CHECK("{t}", c.s[0], {call}, "%f");')
    else:
        h.append(f'  reset(&c); {setup} {fname(syms[t])}(&c); CHECK("{t}", (int)c.r[0], {call}, "%d");')
if 't_double' in syms: h.append(f'  reset(&c); c.d[0] = 3.75; c.r[0] = 20; {fname(syms["t_double"])}(&c); CHECK("t_double", c.d[0], nat_t_double(3.75, 20), "%.12f");')
for a, b in ((('123456', '-789'), ('-2147483647', '-2147483647'), ('65536', '65536')) if 't_mul64' in syms else ()):
    h.append(f'  reset(&c); c.r[0] = (uint32_t)({a}); c.r[1] = (uint32_t)({b}); {fname(syms["t_mul64"])}(&c); '
             f'CHECK("t_mul64", (int64_t)(((uint64_t)c.r[1] << 32) | c.r[0]), nat_t_mul64({a}, {b}), "%" PRId64);')
for a, b in ((('0x123456789abcdefull', '0xfedcba9876543210ull'), ('1ull', '2ull')) if 't_u64' in syms else ()):
    h.append(f'  reset(&c); c.r[0] = (uint32_t){a}; c.r[1] = (uint32_t)({a} >> 32); c.r[2] = (uint32_t){b}; '
             f'c.r[3] = (uint32_t)({b} >> 32); {fname(syms["t_u64"])}(&c); '
             f'CHECK("t_u64", ((uint64_t)c.r[1] << 32) | c.r[0], nat_t_u64({a}, {b}), "%" PRIu64);')
h.append('  printf("%d/%d testes passaram\\n", total - fails, total); return fails != 0; }')
open(f'{B}/harness.c', 'w').write('\n'.join(h) + '\n')

RT = os.path.join(HERE, '..', 'runtime')
sh('clang', '-O1', '-c', f'{HERE}/tests.c', '-o', f'{B}/tests_native.o')
sh('llvm-objcopy', '--prefix-symbols=nat_', f'{B}/tests_native.o')
sh('clang', '-O1', '-w', f'-I{RT}', f'{B}/recompiled.c', f'{B}/harness.c', f'{B}/tests_native.o', '-lm', '-o', f'{B}/harness')
print(f'instruções traduzidas: {rc.stats["insns"] - rc.stats["unimpl"]}/{rc.stats["insns"]} em {len(funcs)} funções')
r = subprocess.run([f'{B}/harness'], capture_output=True, text=True)
print(r.stdout + r.stderr)
sys.exit(r.returncode)

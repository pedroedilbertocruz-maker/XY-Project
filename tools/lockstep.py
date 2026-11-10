"""Compara o código recompilado com o interpretador de referência (tools/armi.py), por intervalos.
Cada par de sondas consecutivas do log vira um teste: o interpretador parte dos registradores da
primeira, executa até o endereço da segunda e compara os registradores.
Uso: python3 tools/lockstep.py <log> [retratos de memória: ini=arquivo ...]"""
import re, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from armi import CPU, Memory

log = sys.argv[1]
snaps = [(int(a.split('=')[0], 16), a.split('=')[1]) for a in sys.argv[2:]]
probes = []
for l in open(log):
    m = re.match(r'\[probe ([0-9a-f]+)\]', l)
    if not m: continue
    d = dict(re.findall(r'(r\d+)=([0-9a-f]+)', l)); f = re.search(r'nzcv=(\d)(\d)(\d)(\d)', l)
    t = re.search(r'tls=([0-9a-f]+)', l)
    probes.append((int(m.group(1), 16), [int(d[f'r{i}'], 16) for i in range(15)], [int(x) for x in f.groups()], int(t.group(1), 16)))
print(f'{len(probes)} sondas')
base = Memory(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'image'), snaps)
checked = 0
for k in range(len(probes) - 1):
    a0, regs, fl, tls = probes[k]
    a1, exp, efl, _ = probes[k + 1]
    if exp[13] != regs[13] and abs(exp[13] - regs[13]) > 0x1000: continue   # outra thread
    cpu = CPU(base); cpu.tls = tls
    cpu.r[:15] = regs; cpu.r[15] = a0
    cpu.n, cpu.z, cpu.c, cpu.v = fl
    trace = []
    ok = True
    while True:
        trace.append(cpu.r[15])
        try: cpu.step()
        except Exception as e: print(f'intervalo {k}: parou ({e})'); ok = False; break
        if cpu.r[15] == a1: break
        if len(trace) > 20000: print(f'intervalo {k}: não chegou em {a1:08x}'); ok = False; break
    if not ok: continue
    checked += 1
    diff = [i for i in range(15) if cpu.r[i] != exp[i]]
    fdiff = [cpu.n, cpu.z, cpu.c, cpu.v] != efl
    if diff or fdiff:
        print(f'DIVERGÊNCIA no intervalo {k} ({a0:08x} -> {a1:08x}, {len(trace)} instruções):')
        for i in diff: print(f'  r{i}: referência={cpu.r[i]:08x} recompilado={exp[i]:08x}')
        if fdiff: print(f'  flags: referência={[cpu.n, cpu.z, cpu.c, cpu.v]} recompilado={efl}')
        print('  caminho:', ' '.join(f'{a:08x}' for a in trace[-60:]))
        sys.exit(0)
print(f'{checked} intervalos conferidos, sem divergências')

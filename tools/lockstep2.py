"""Lockstep contínuo: o interpretador parte da primeira sonda e segue executando com sua própria memória,
comparando os registradores a cada sonda da mesma thread (mesma faixa de sp). Evita a "deriva" dos retratos.
Uso: python3 tools/lockstep2.py <log> ini=arquivo ... [--max N]"""
import re, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from armi import CPU, Memory
args = [a for a in sys.argv[2:] if '=' in a]
log = sys.argv[1]
probes = []
for l in open(log):
    m = re.match(r'\[probe ([0-9a-f]+)\]', l)
    if not m: continue
    d = dict(re.findall(r'(r\d+)=([0-9a-f]+)', l)); f = re.search(r'nzcv=(\d)(\d)(\d)(\d)', l)
    t = re.search(r'tls=([0-9a-f]+)', l)
    probes.append((int(m.group(1), 16), [int(d[f'r{i}'], 16) for i in range(15)], [int(x) for x in f.groups()], int(t.group(1), 16)))
mem = Memory(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'image'),
             [(int(a.split('=')[0], 16), a.split('=')[1]) for a in args])
a0, regs, fl, tls = probes[0]
probes = [p for p in probes if p[3] == tls]
cpu = CPU(mem); cpu.tls = tls; cpu.r[:15] = regs; cpu.r[15] = a0; cpu.n, cpu.z, cpu.c, cpu.v = fl
addrs = {p[0] for p in probes}
hist = []
for k in range(1, len(probes)):
    a1, exp, efl, _ = probes[k]
    n = 0
    while True:
        hist.append(cpu.r[15]); hist = hist[-200:]
        try: cpu.step()
        except (StopIteration, NotImplementedError) as e:      # svc: ressincroniza os registradores na próxima sonda
            print(f'  ({e}; ressincronizando na sonda {k})'); n = -1; cpu.r[:15] = exp; cpu.r[15] = a1; cpu.n, cpu.z, cpu.c, cpu.v = efl; break
        n += 1
        if cpu.r[15] in addrs: break
        if n > 300_000:
            print(f'  (perdido antes da sonda {k}; ressincronizando)'); cpu.r[:15] = exp; cpu.r[15] = a1; cpu.n, cpu.z, cpu.c, cpu.v = efl; break
    diff = [i for i in range(15) if cpu.r[i] != exp[i]]
    if cpu.r[15] != a1 or diff or [cpu.n, cpu.z, cpu.c, cpu.v] != efl:
        print(f'DIVERGÊNCIA na sonda {k}: ref pc={cpu.r[15]:08x} esperado {a1:08x}')
        for i in diff: print(f'  r{i}: referência={cpu.r[i]:08x} recompilado={exp[i]:08x}')
        print('  flags', [cpu.n, cpu.z, cpu.c, cpu.v], efl)
        print('  caminho:', ' '.join(f'{a:08x}' for a in hist[-80:]))
        sys.exit(0)
print(len(probes), 'sondas conferidas, sem divergência')

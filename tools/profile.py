"""Perfil por amostragem do executável em execução (gdb): conta onde a thread ATIVA está.
Uso: python3 tools/profile.py <pid> [amostras]"""
import re, subprocess, sys, collections
pid, n = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 40
idle = re.compile(r'^(__|pthread|futex|clock|syscall|lll|schedule|pause|poll|nanosleep|start_thread|clone|thread_main|k_svc|block|svc$)')
cnt = collections.Counter()
for _ in range(n):
    out = subprocess.run(['gdb', '-p', pid, '-batch', '-ex', 'thread apply all bt 12'], capture_output=True, text=True).stdout
    for th in out.split('\nThread ')[1:]:
        fns = re.findall(r'^#\d+\s+(?:0x[0-9a-f]+ in )?([A-Za-z_0-9]+)', th, re.M)
        if not fns or idle.match(fns[0]) or any(f == 'schedule' for f in fns[:3]): continue
        cnt['<'.join(fns[:2])] += 1
for k, v in cnt.most_common(15): print(f'{v:5d}  {k}')

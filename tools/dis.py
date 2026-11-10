"""Desmonta a imagem linkada: python3 tools/dis.py <endereço hex> [n instruções] [thumb]"""
import json, os, struct, subprocess, sys, tempfile
here = os.path.dirname(os.path.abspath(__file__))
img = os.path.join(here, '..', 'out', 'image')
m = json.load(open(os.path.join(img, 'map.json')))
addr = int(sys.argv[1], 16); n = int(sys.argv[2]) if len(sys.argv) > 2 else 40
thumb = len(sys.argv) > 3
for r in m['regions']:
    if r['addr'] <= addr < r['addr'] + r['size']:
        data = open(os.path.join(img, r['file']), 'rb').read()
        start = max(r['addr'], addr - 4 * (n // 2)); end = min(r['addr'] + r['size'], start + 4 * n)
        chunk = data[start - r['addr']:end - r['addr']]
        with tempfile.TemporaryDirectory() as t:
            b = os.path.join(t, 'c.bin'); open(b, 'wb').write(chunk)
            e = os.path.join(t, 'c.o')
            subprocess.run(['llvm-objcopy', '-I', 'binary', '-O', 'elf32-littlearm', '--rename-section',
                            '.data=.text,code', b, e], check=True)
            out = subprocess.run(['llvm-objdump', '-d', '--mcpu=mpcore', f'--adjust-vma={start:#x}',
                                  '--triple=' + ('thumbv6k' if thumb else 'armv6k'), e],
                                 capture_output=True, text=True).stdout
        print(f'== {r["module"]} {r["kind"]}')
        for l in out.splitlines():
            t = l.strip()
            if ':' in t and t.split(':')[0] and all(ch in '0123456789abcdef' for ch in t.split(':')[0]):
                off = int(t.split(':')[0], 16)
                print(f'{start + off:08x}:' + t.split(':', 1)[1].split('<_binary')[0].split('@')[0])
        break

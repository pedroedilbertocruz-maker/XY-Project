"""Descoberta de funções na imagem linkada (ARM e Thumb)."""
import struct, json, sys, os, collections
sys.path.insert(0, os.path.dirname(__file__))
import link

def text_ranges(layout):
    rs = [(0x100000, 0x100000 + 0x43BEDC, '|static|')]
    for m in layout:
        for s in m['segments']:
            if s['kind'] == 'text':
                rs.append((s['addr'], s['addr'] + s['size'], m['name']))
    return rs

def discover(img, layout):
    rs = text_ranges(layout)
    def in_text(a):
        for s, e, n in rs:
            if s <= a < e: return n
    arm, thumb = collections.Counter(), collections.Counter()
    secondary = set()
    # 1) ponteiros vindos de relocações (vtables, tabelas, callbacks)
    for w, t in img.ptrs:
        if in_text(t & ~1):
            (thumb if t & 1 else arm)[t & ~1] += 1
    # 2) BL / BLX imediatos e ponteiros literais dentro do código
    for s, e, n in rs:
        r = img.find(s); base = r[0]; buf = r[1]
        for a in range(s, e - 3, 4):
            w = struct.unpack_from('<I', buf, a - base)[0]
            top = w >> 24
            if (top & 0xF) == 0xB and (w >> 28) != 0xF:   # BL com qualquer condição
                top = 0xEB
            if top == 0xEB or top in (0xFA, 0xFB):
                off = ((w & 0xFFFFFF) ^ 0x800000) - 0x800000
                t = a + 8 + off * 4
                if top in (0xFA, 0xFB):
                    t += ((w >> 24) & 1) * 2
                    if in_text(t): thumb[t] += 1
                elif in_text(t): arm[t] += 1
    # 2b) chamadas no estilo ARMv4: `add lr, pc, #imm` (ou `mov lr, pc`) e logo depois um `b`/`bCC alvo`
    #     (sem link): o alvo é uma função que volta para o endereço posto em lr.
    for s, e, n in rs:
        r = img.find(s); base = r[0]; buf = r[1]
        W = lambda a: struct.unpack_from('<I', buf, a - base)[0]
        for a in range(s + 16, e - 3, 4):
            w = W(a)
            if (w >> 25) & 7 != 5 or (w >> 24) & 1 or w >> 28 == 0xF: continue
            for back in range(1, 5):
                p = W(a - 4 * back)
                if (p >> 25) & 7 == 5 and p >> 28 != 0xF and not (p >> 24) & 1: continue
                if p >> 28 != 0xE: break
                ret = None
                if p & 0x0FFFFFFF == 0x01A0E00F: ret = a - 4 * back + 8
                elif p & 0x0FFFF000 == 0x028FE000:
                    rot = ((p >> 8) & 0xF) * 2; imm = p & 0xFF
                    ret = a - 4 * back + 8 + (((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm)
                if ret is not None:
                    if a < ret <= a + 64:
                        t = (a + 8 + (((w & 0xFFFFFF) ^ 0x800000) - 0x800000) * 4) & 0xFFFFFFFF
                        if in_text(t):
                            rt = img.find(t); wt = struct.unpack_from('<I', rt[1], t - rt[0])[0]
                            if abs(t - a) > 128 or wt & 0xFFFF4000 == 0xE92D4000: arm[t] += 1
                    break
                if (p >> 25) & 7 == 5 and not (p >> 24) & 1: continue
                if (p >> 12) & 0xF == 14 and (p >> 26) & 3 in (0, 1): break
                if p & 0x0E10A000 == 0x08104000: break
    # 3) ponteiros absolutos no code.bin (vtables, tabelas, literal pools): ele não tem relocações.
    #    ARM: aceita se o alvo é uma instrução ARM incondicional (ou push).
    #    Thumb (endereço ímpar): aceita se começa com push, ou se está numa sequência de ponteiros
    #    para código (vtables misturam funções ARM e Thumb).
    TLO, THI = 0x100000, 0x100000 + 0x43BEDC
    for lo, hi in ((0x100000, 0x53C000), (0x53C000, 0x5A6000), (0x5A6000, 0x5A6000 + 0x440C0)):
        r = img.find(lo); base = r[0]; buf = r[1]
        end = min(hi, base + len(buf)) - 3
        words = {}
        for a in range(lo, end, 4):
            v = struct.unpack_from('<I', buf, a - base)[0]
            if TLO <= v < THI: words[a] = v
        for a, v in words.items():
            if v & 3 == 0:
                rv = img.find(v); ins = struct.unpack_from('<I', rv[1], v - rv[0])[0]
                if ins >> 28 == 0xE:   # instrução ARM incondicional (inclui thunks de um único `b`)
                    arm[v] += 1
            elif v & 3 in (1, 3):
                t = v & ~1; rv = img.find(t)
                hw = struct.unpack_from('<H', rv[1], t - rv[0])[0]
                in_table = lo >= 0x53C000 and ((a - 4) in words or (a + 4) in words)
                looks_arm = t & 3 == 0 and struct.unpack_from('<I', rv[1], t - rv[0])[0] >> 28 == 0xE
                if hw >> 8 == 0xB5 or (in_table and not looks_arm):
                    thumb[t] += 1
    # 4) tabelas de ponteiros relativos (ex.: construtores globais de C++ do ARMCC: alvo = entrada + valor).
    #    Só conta sequências de >= 8 entradas seguidas que apontam para o texto (tabelas de verdade).
    TEXT_LO, TEXT_HI = 0x100000, 0x100000 + 0x43BEDC
    for lo, hi in ((0x53C000, 0x5A6000), (0x5A6000, 0x5A6000 + 0x440C0)):
        r = img.find(lo); base = r[0]; buf = r[1]
        run = []
        def flush():
            if len(run) >= 8:
                for t in run:
                    if t & 1: thumb[t & ~1] += 1
                    elif t & 3 == 0: arm[t] += 1
            run.clear()
        for a in range(lo, min(hi, base + len(buf)) - 3, 4):
            v = struct.unpack_from('<I', buf, a - base)[0]
            t = (a + v) & 0xFFFFFFFF
            if v >= 0x80000000 and TEXT_LO <= t < TEXT_HI and (t & 3) in (0, 1, 3):
                run.append(t)
            else:
                flush()
        flush()
    # 5) início de bloco depois de um fim de fluxo (`mov pc, lr`, `bx lr`, `pop {..pc}`, `b`), só perto de
    #    saltos computados `add pc, rX, rY` (handlers alcançados por tabelas com base calculada, como no
    #    decodificador de vídeo em assembly). Viram pontos de entrada secundários (ver recomp_game.py).
    def ends_flow(w):
        if w >> 28 != 0xE: return False
        return (w == 0xE1A0F00E or w == 0xE12FFF1E or (w & 0xFFFF8000) == 0xE8BD8000
                or (w & 0x0F000000) == 0x0A000000 or (w & 0x0FFFFFF0) == 0x012FFF10)
    for s_, e_, n_ in rs:
        r = img.find(s_); base = r[0]; buf = r[1]
        W = lambda a: struct.unpack_from('<I', buf, a - base)[0]
        sites = [a for a in range(s_, e_ - 3, 4)
                 if W(a) & 0x0FF0F000 == 0x0080F000 and not (W(a) >> 25) & 1 and (W(a) >> 16) & 0xF != 15 and W(a) & 0xF != 15]
        for site in sites:
            prev = 0
            for a in range(max(s_, site - 0x4000), min(e_ - 3, site + 0x4000), 4):
                w = W(a)
                if ends_flow(prev) and w >> 28 == 0xE and (w >> 25) & 7 != 5 and a not in arm:
                    secondary.add(a)
                prev = w
    # 7) prólogos e ponteiros Thumb no código (funções chamadas só por ponteiro montado em tempo de execução,
    #    que nenhuma das regras acima vê: callbacks passados como argumento, como no centro Pokémon).
    #    Thumb: `push {.., lr}` logo depois de um fim de função (`bx lr`, `pop {..pc}`, `b`, preenchimento).
    #    ARM: `push {.., lr}` logo depois de um fim de função ou de dados (literal pool).
    #    Ponteiros ímpares dentro do código (literal pools) que apontam para o código: funções Thumb.
    #    Compilar alguma função a mais não faz mal (nunca é chamada); faltar uma derruba o jogo.
    def t_end(h):
        return h == 0x4770 or (h >> 8) == 0xBD or (h >> 11) == 0x1C or h in (0x0000, 0x46C0, 0xBF00)
    def a_end(p):
        return p >> 28 != 0xE or p == 0xE12FFF1E or (p & 0xFFFF8000) == 0xE8BD8000 or (p & 0x0F000000) == 0x0A000000 or p == 0xE1A0F00E
    for s_, e_, n_ in rs:
        r = img.find(s_); base = r[0]; buf = r[1]
        H = lambda a: struct.unpack_from('<H', buf, a - base)[0]
        W = lambda a: struct.unpack_from('<I', buf, a - base)[0]
        for a in range(s_ + 2, e_ - 2, 2):
            if H(a) >> 8 == 0xB5 and t_end(H(a - 2)) and a not in thumb: thumb[a] += 1
        for a in range(s_ + 4, e_ - 4, 4):
            w = W(a)
            if w & 0xFFFF4000 == 0xE92D4000 and a_end(W(a - 4)) and a not in arm: arm[a] += 1
            if w & 1 and in_text(w & ~1) and (w & ~1) not in thumb: thumb[w & ~1] += 1
    # 6) entry point
    arm[0x100000] += 1
    discover.secondary = secondary
    return rs, arm, thumb

if __name__ == '__main__':
    img, layout, *_ = link.build(sys.argv[1], None, sys.argv[2], sys.argv[3])
    rs, arm, thumb = discover(img, layout)
    per = collections.Counter()
    for a in arm:
        for s, e, n in rs:
            if s <= a < e: per[n] += 1
    out = {'arm': sorted(arm), 'thumb': sorted(thumb)}
    json.dump(out, open(os.path.join(sys.argv[3], 'functions.json'), 'w'))
    print('funções ARM:', len(arm), ' Thumb:', len(thumb))
    print('por módulo (top 10):', per.most_common(10))

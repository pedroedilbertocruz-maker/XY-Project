"""Interpretador ARM (ARMv6, sem VFP/Thumb) de referência, para verificar o código recompilado em lockstep.

Escrito de forma independente do recompilador, a partir da semântica do manual da arquitetura ARM.
Uso típico (ver tools/lockstep.py): carregar a imagem linkada + retratos de memória do runtime,
partir dos registradores impressos por uma sonda e comparar com as sondas seguintes.
"""
import json, os, struct

M32 = 0xFFFFFFFF


class Memory:
    def __init__(self, image_dir, snapshots=()):
        self.pages = {}            # página de 4 KB -> bytearray
        m = json.load(open(os.path.join(image_dir, 'map.json')))
        for r in m['regions']:
            self.load(r['addr'], open(os.path.join(image_dir, r['file']), 'rb').read())
        for addr, path in snapshots:
            self.load(addr, open(path, 'rb').read())

    def load(self, addr, data):
        for off in range(0, len(data), 4096):
            a = addr + off
            pg = self.page(a)
            chunk = data[off:off + 4096]
            pg[a & 0xFFF:(a & 0xFFF) + len(chunk)] = chunk

    def page(self, a):
        k = a >> 12
        p = self.pages.get(k)
        if p is None:
            p = self.pages[k] = bytearray(4096)
        return p

    def r8(self, a): return self.page(a)[a & 0xFFF]
    def w8(self, a, v): self.page(a)[a & 0xFFF] = v & 0xFF
    def r16(self, a): return self.r8(a) | (self.r8(a + 1) << 8)
    def w16(self, a, v): self.w8(a, v); self.w8(a + 1, v >> 8)
    def r32(self, a): return self.r16(a) | (self.r16(a + 2) << 16)
    def w32(self, a, v): self.w16(a, v); self.w16(a + 2, v >> 16)


def ror(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & M32 if n else v


def sx(v, bits):
    return v - (1 << bits) if v >> (bits - 1) else v


class CPU:
    def __init__(self, mem):
        self.m = mem
        self.r = [0] * 16
        self.n = self.z = self.c = self.v = 0
        self.steps = 0
        self.tls = 0

    def cond(self, c):
        n, z, cf, v = self.n, self.z, self.c, self.v
        return [z, not z, cf, not cf, n, not n, v, not v, cf and not z, (not cf) or z,
                n == v, n != v, (not z) and n == v, z or n != v, True, True][c]

    def shift(self, val, typ, amt, by_reg):
        """devolve (resultado, carry); amt já resolvido. Semântica de deslocamento do ARM."""
        c = self.c
        if by_reg:
            if amt == 0: return val, c
            if typ == 0:
                if amt < 32: return (val << amt) & M32, (val >> (32 - amt)) & 1
                return 0, (val & 1) if amt == 32 else 0
            if typ == 1:
                if amt < 32: return val >> amt, (val >> (amt - 1)) & 1
                return 0, (val >> 31) if amt == 32 else 0
            if typ == 2:
                if amt < 32: return (sx(val, 32) >> amt) & M32, (val >> (amt - 1)) & 1
                return (M32 if val >> 31 else 0), val >> 31
            amt &= 31
            if amt == 0: return val, val >> 31
            return ror(val, amt), (val >> (amt - 1)) & 1
        if typ == 0:
            if amt == 0: return val, c
            return (val << amt) & M32, (val >> (32 - amt)) & 1
        if typ == 1:
            if amt == 0: return 0, val >> 31
            return val >> amt, (val >> (amt - 1)) & 1
        if typ == 2:
            if amt == 0: return (M32 if val >> 31 else 0), val >> 31
            return (sx(val, 32) >> amt) & M32, (val >> (amt - 1)) & 1
        if amt == 0: return ((c << 31) | (val >> 1)) & M32, val & 1
        return ror(val, amt), (val >> (amt - 1)) & 1

    def reg(self, i, pc):
        return (pc + 8) & M32 if i == 15 else self.r[i]

    def step(self):
        pc = self.r[15]
        w = self.m.r32(pc)
        self.steps += 1
        cond = w >> 28
        nxt = (pc + 4) & M32
        self.r[15] = nxt
        if cond == 0xF:
            if w & 0xFE000000 == 0xFA000000:      # BLX imm -> Thumb (não suportado aqui)
                raise NotImplementedError(f'BLX para Thumb em {pc:08x}')
            if w & 0xFD70F000 == 0xF550F000: return   # PLD
            if w == 0xF57FF01F: return                 # CLREX
            raise NotImplementedError(f'{pc:08x}: {w:08x}')
        if not self.cond(cond): return
        r = self.r
        if (w >> 25) & 7 == 5:                     # B / BL
            off = sx(w & 0xFFFFFF, 24) * 4
            if (w >> 24) & 1: r[14] = nxt
            r[15] = (pc + 8 + off) & M32
            return
        if w & 0x0FFFFFF0 == 0x012FFF10: r[15] = r[w & 0xF] & ~1 & M32; return              # BX
        if w & 0x0FFFFFF0 == 0x012FFF30: t = r[w & 0xF]; r[14] = nxt; r[15] = t & ~1; return  # BLX reg
        if (w >> 24) & 0xF == 0xF: raise StopIteration(f'svc {w & 0xFFFFFF:x} em {pc:08x}')
        if w & 0x0FC000F0 == 0x00000090:           # MUL / MLA
            rd, rn, rs, rm = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15
            res = (r[rm] * r[rs] + (r[rn] if (w >> 21) & 1 else 0)) & M32
            r[rd] = res
            if (w >> 20) & 1: self.n, self.z = res >> 31, int(res == 0)
            return
        if w & 0x0F8000F0 == 0x00800090:           # UMULL/UMLAL/SMULL/SMLAL
            hi, lo, rs, rm = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15
            if (w >> 22) & 1: p = sx(r[rm], 32) * sx(r[rs], 32)
            else: p = r[rm] * r[rs]
            if (w >> 21) & 1: p += (r[hi] << 32) | r[lo]
            p &= (1 << 64) - 1
            r[lo], r[hi] = p & M32, p >> 32
            if (w >> 20) & 1: self.n, self.z = p >> 63, int(p == 0)
            return
        if w & 0x0FF00FFF == 0x01900F9F: r[(w >> 12) & 15] = self.m.r32(r[(w >> 16) & 15]); return   # LDREX
        if w & 0x0FF00FF0 == 0x01800F90: self.m.w32(r[(w >> 16) & 15], r[w & 15]); r[(w >> 12) & 15] = 0; return  # STREX
        if w & 0x0FFF0FFF == 0x0E1D0F70: r[(w >> 12) & 15] = self.tls; return   # mrc p15 TLS
        if w & 0x0FFF0FF0 == 0x016F0F10:           # CLZ
            v = r[w & 15]; r[(w >> 12) & 15] = 32 - v.bit_length(); return
        if w & 0x0E000090 == 0x00000090 and (w >> 5) & 3:   # LDRH/STRH/LDRSB/LDRSH/LDRD/STRD
            P, U, I, W, L = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
            rn, rd, sh = (w >> 16) & 15, (w >> 12) & 15, (w >> 5) & 3
            off = (((w >> 4) & 0xF0) | (w & 0xF)) if I else r[w & 15]
            base = self.reg(rn, pc)
            addr = (base + off if U else base - off) & M32
            ea = addr if P else base
            if L:
                if sh == 1: val = self.m.r16(ea)
                elif sh == 2: val = sx(self.m.r8(ea), 8) & M32
                else: val = sx(self.m.r16(ea), 16) & M32
                if not P or W: r[rn] = addr
                r[rd] = val
            else:
                if sh == 1: self.m.w16(ea, r[rd])
                elif sh == 2: r[rd] = self.m.r32(ea); r[rd + 1] = self.m.r32(ea + 4)   # LDRD
                else: self.m.w32(ea, r[rd]); self.m.w32(ea + 4, r[rd + 1])                # STRD
                if not P or W: r[rn] = addr
            return
        if (w >> 26) & 3 == 0:                     # processamento de dados
            if w & 0x0F900000 == 0x01000000 and not (w & (1 << 20)):
                raise NotImplementedError(f'{pc:08x}: {w:08x} (misc)')
            op, S = (w >> 21) & 15, (w >> 20) & 1
            rn, rd = (w >> 16) & 15, (w >> 12) & 15
            a = self.reg(rn, pc)
            if w & (1 << 25):
                rot = ((w >> 8) & 15) * 2
                b = ror(w & 0xFF, rot); sc = (b >> 31) if rot else self.c
            else:
                rm = w & 15
                if w & 0x10:
                    amt = r[(w >> 8) & 15] & 0xFF
                    val = r[rm] if rm != 15 else (pc + 12) & M32
                    b, sc = self.shift(val, (w >> 5) & 3, amt, True)
                else:
                    b, sc = self.shift(self.reg(rm, pc), (w >> 5) & 3, (w >> 7) & 31, False)
            c_in = self.c
            logical = op in (0, 1, 8, 9, 12, 13, 14, 15)
            if op in (0, 8): res = a & b
            elif op in (1, 9): res = a ^ b
            elif op in (2, 10): res = a - b
            elif op == 3: res = b - a
            elif op in (4, 11): res = a + b
            elif op == 5: res = a + b + c_in
            elif op == 6: res = a - b - (1 - c_in)
            elif op == 7: res = b - a - (1 - c_in)
            elif op == 12: res = a | b
            elif op == 13: res = b
            elif op == 14: res = a & ~b & M32
            else: res = ~b & M32
            res32 = res & M32
            if S or op in (8, 9, 10, 11):
                self.n, self.z = res32 >> 31, int(res32 == 0)
                if logical: self.c = sc
                else:
                    if op in (4, 11, 5): x, y = a, b
                    elif op in (2, 10, 6): x, y = a, b
                    else: x, y = b, a
                    if op in (4, 11, 5):
                        self.c = int(res > M32)
                        self.v = ((x ^ res32) & (y ^ res32)) >> 31
                    else:
                        self.c = int(res >= 0)
                        self.v = ((x ^ y) & (x ^ res32)) >> 31
            if op in (8, 9, 10, 11): return
            r[rd] = res32
            if rd == 15: r[15] = res32 & ~3
            return
        if (w >> 26) & 3 == 1:                     # LDR/STR (palavra/byte)
            P, U, B, W, L = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
            rn, rd = (w >> 16) & 15, (w >> 12) & 15
            if w & (1 << 25):
                off, _ = self.shift(r[w & 15], (w >> 5) & 3, (w >> 7) & 31, False)
            else:
                off = w & 0xFFF
            base = self.reg(rn, pc)
            addr = (base + off if U else base - off) & M32
            ea = addr if P else base
            if L:
                val = self.m.r8(ea) if B else self.m.r32(ea)
                if not P or W: r[rn] = addr
                r[rd] = val
                if rd == 15: r[15] = val & ~1 & M32
            else:
                val = self.reg(rd, pc)
                if B: self.m.w8(ea, val)
                else: self.m.w32(ea, val)
                if not P or W: r[rn] = addr
            return
        if (w >> 25) & 7 == 4:                     # LDM/STM
            P, U, S, W, L = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
            rn = (w >> 16) & 15
            regs = [i for i in range(16) if (w >> i) & 1]
            n = len(regs)
            base = r[rn]
            start = base + (4 if P else 0) if U else base - 4 * n + (0 if P else 4)
            newb = (base + 4 * n if U else base - 4 * n) & M32
            vals = []
            for k, ri in enumerate(regs):
                a = (start + 4 * k) & M32
                if L: vals.append(self.m.r32(a))
                else: self.m.w32(a, r[ri] if ri != 15 else (pc + 12) & M32)
            if W: r[rn] = newb
            if L:
                for ri, v in zip(regs, vals):
                    r[ri] = v
                if 15 in regs: r[15] = r[15] & ~1 & M32
            return
        raise NotImplementedError(f'{pc:08x}: {w:08x}')

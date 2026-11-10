"""Recompilador estático ARMv6K (+VFPv2) -> C.

Cada função ARM vira uma função C `void F_xxxxxxxx(Ctx *c)`. O estado da CPU fica em Ctx
(registradores, flags, registradores VFP). A memória do jogo é acessada pelos macros MEM*.
Instruções ainda não suportadas viram chamadas a UNIMPL(), para o runtime avisar.
"""
import os
NANCHECK = bool(os.environ.get('NANCHECK'))
import struct

COND = ['c->z', '!c->z', 'c->c', '!c->c', 'c->n', '!c->n', 'c->v', '!c->v',
        '(c->c && !c->z)', '(!c->c || c->z)', '(c->n == c->v)', '(c->n != c->v)',
        '(!c->z && c->n == c->v)', '(c->z || c->n != c->v)', None, None]

def sx(v, bits):
    return (v ^ (1 << (bits - 1))) - (1 << (bits - 1))

def ror32(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & 0xFFFFFFFF if n else v


class Insn:
    __slots__ = ('addr', 'w', 'code', 'ok', 'succ', 'kind', 'target', 'table', 'size')
    def __init__(self, addr, w, size=4):
        self.addr, self.w, self.size = addr, w, size
        self.code, self.ok = [], True
        self.succ = []        # sucessores dentro da função
        self.kind = None      # 'ret', 'jmp', 'tail', None
        self.target = None
        self.table = None


# Ganchos ligados por opções do menu, achados pela assinatura (as primeiras instruções), não pelo endereço: assim
# valem para o X e para o Y. Cada um testa uma variável do runtime (rt.h) e, ligado, muda o comportamento.
HOOKS = [
    # Poké Radar: carga da bateria (ldrh r0,[r0,#10]; bic r0,r0,#0xC000; bx lr) -> sempre 50 (cheia)
    ((0xE1D000BA, 0xE3C00903, 0xE12FFF1E),
     '  if (__builtin_expect(g_opt_radar, 0)) { c->r[0] = 50u; return; }   /* opção: Poké Radar sempre carregado */'),
    # Poké Radar: soma à carga (min(carga + n, 50)) -> soma 50 (enche de uma vez)
    ((0xE1D020BA, 0xE3C23903, 0xE0811003, 0xE2022903, 0xE6FF1071, 0xE3510032),
     '  if (__builtin_expect(g_opt_radar, 0)) c->r[1] = 50u;   /* opção: Poké Radar sempre carregado */'),
    # XP ganho em batalha (DllBattle): a função que aplica o XP da batalha recebe em r2 uma tabela com 6 entradas de
    # 20 bytes (uma por vaga da equipe: XP em u32, depois os EVs); multiplicar ali vale para a mensagem "ganhou N de
    # XP", para o Exp. Share e para a subida de nível
    ((0xE92D4FF0, 0xE24DD04C, 0xE1A07001, 0xE1A06002, 0xE1A0B000, 0xE3A00000, 0xE1A04000, 0xE1A0A000,
      0xE58D0038, 0xE58D0034, 0xE28B0B03, 0xE2800F73),
     '  if (__builtin_expect(g_opt_xp > 1, 0)) for (uint32_t k_ = 0; k_ < 6; k_++) {   /* opção: XP ganho em batalha */\n'
     '      uint64_t x_ = (uint64_t)MEM32(c->r[2] + k_ * 20u) * (uint32_t)g_opt_xp; MEM32(c->r[2] + k_ * 20u) = x_ > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)x_; }'),
]


class Recompiler:
    def __init__(self, read32, func_starts, is_code, thumb_starts=()):
        self.read32 = read32          # endereço -> palavra de 32 bits da imagem linkada
        self.funcs = set(func_starts)
        self.tfuncs = set(thumb_starts)
        self.tcalls = set()           # funções Thumb chamadas
        self.thumb_lit = {}           # último literal carregado por registrador (Thumb)
        # sondas de depuração: PROBES="10d71c,111bb8" insere probe(c, endereço) antes dessas instruções
        import os as _os
        self.probes = {int(x, 16) for x in _os.environ.get('PROBES', '').split(',') if x.strip()}
        self.is_code = is_code        # endereço -> bool (dentro de um segmento de texto)
        self.calls = set()            # funções chamadas (para declarar extern)
        self.stats = {'insns': 0, 'unimpl': 0, 'funcs': 0}
        self.unimpl_hist = {}

    # ------------------------------------------------------------ operandos
    def reg(self, r, addr):
        return f'0x{addr + 8:08x}u' if r == 15 else f'c->r[{r}]'

    def shifter(self, w, addr, need_carry):
        """Operando 2 de instruções de processamento de dados -> (expr, carry_expr)."""
        if w & (1 << 25):
            rot = ((w >> 8) & 0xF) * 2
            imm = ror32(w & 0xFF, rot)
            carry = (f'{imm >> 31}u' if rot else 'c->c')
            return f'0x{imm:08x}u', carry
        rm = self.reg(w & 0xF, addr)
        typ = (w >> 5) & 3
        if w & 0x10:  # deslocamento por registrador
            rs = self.reg((w >> 8) & 0xF, addr)
            fn = ['sh_lsl', 'sh_lsr', 'sh_asr', 'sh_ror'][typ]
            return f'{fn}(c, {rm}, {rs} & 0xff, &sc)', 'sc'
        n = (w >> 7) & 0x1F
        if typ == 0:
            if n == 0: return rm, 'c->c'
            return f'({rm} << {n})', f'(({rm} >> {32 - n}) & 1)'
        if typ == 1:
            if n == 0: return '0u', f'({rm} >> 31)'
            return f'({rm} >> {n})', f'(({rm} >> {n - 1}) & 1)'
        if typ == 2:
            if n == 0: n = 32
            return f'(uint32_t)((int32_t){rm} >> {min(n, 31)})', f'(({rm} >> {n - 1}) & 1)'
        if n == 0:
            return f'((c->c << 31) | ({rm} >> 1))', f'({rm} & 1)'
        return f'ROR32({rm}, {n})', f'(({rm} >> {n - 1}) & 1)'

    # ------------------------------------------------------------ decodificação
    def decode(self, addr, w, entry):
        i = Insn(addr, w)
        cond = w >> 28
        out = i.code
        nxt = addr + 4
        if cond == 0xF:
            return self.decode_uncond(i, w, nxt)
        # ----- branches
        if (w >> 25) & 7 == 5:
            link = (w >> 24) & 1
            t = (addr + 8 + sx(w & 0xFFFFFF, 24) * 4) & 0xFFFFFFFF
            if not self.is_code(t):   # alvo fora do código (dado decodificado como instrução)
                out.append(f'c->r[14] = 0x{nxt:08x}u; call_indirect(c, 0x{t:08x}u);' if link
                           else f'c->tret = ret_; call_indirect(c, 0x{t:08x}u); return;')
                i.succ = [nxt] if (link or cond != 0xE) else []
                if not link: i.kind = 'tail'
            elif link:
                self.calls.add(t)
                out.append(f'c->r[14] = 0x{nxt:08x}u; {self.fn(t)}(c);')
                i.succ = [nxt]
            elif (ret := self.lr_return(addr)) is not None and addr < ret <= addr + 64 and t in self.funcs \
                    and (abs(t - addr) > 128 or self.read32(t) & 0xFFFF4000 == 0xE92D4000):
                # `add lr, pc, #imm` seguido de `b`/`bCC alvo`: chamada no estilo ARMv4 que volta em `ret`
                self.calls.add(t)
                out.append(f'c->r[14] = 0x{ret:08x}u; {self.fn(t)}(c);' + ('' if ret == nxt else f' goto L_{ret:08x};'))
                i.succ = [ret] if ret == nxt else [ret] + ([nxt] if cond != 0xE else [])
                if ret != nxt: i.kind, i.target = 'jmp', ret
            elif t in self.funcs and t != entry:
                self.calls.add(t)
                out.append(f'TAIL({self.fn(t)});')
                i.kind = 'tail'
                i.succ = [nxt] if cond != 0xE else []
            else:
                out.append(f'goto L_{t:08x};')
                i.kind, i.target = 'jmp', t
                i.succ = [t] + ([nxt] if cond != 0xE else [])
            return self.wrap(i, cond)
        i.succ = [nxt]
        # ----- BX / BLX registrador
        if w & 0x0FFFFFF0 == 0x012FFF10:
            rm = w & 0xF
            ret = self.lr_return(addr) if rm != 14 else None
            if ret is not None:
                # `mov lr, pc` / `add lr, pc, #imm` antes de `bx rX`: chamada no estilo ARMv4
                out.append(f'call_indirect(c, {self.reg(rm, addr)});' + ('' if ret == nxt else f' goto L_{ret:08x};'))
                i.succ = [ret] if ret == nxt else [ret] + ([nxt] if cond != 0xE else [])
                if ret != nxt: i.kind, i.target = 'jmp', ret
                return self.wrap(i, cond)
            # `bx lr`: retorno se lr é o retorno esperado; senão (lr preparado localmente, como no
            # decodificador de vídeo: `add lr, pc, #a; add lr, lr, #b; add pc, ...`) é um salto
            out.append(f'JUMP({self.reg(rm, addr)});')
            i.kind = 'ret'
            i.succ = [nxt] if cond != 0xE else []
            return self.wrap(i, cond)
        if w & 0x0FFFFFF0 == 0x012FFF30:
            out.append(f'{{ uint32_t t = {self.reg(w & 0xF, addr)}; c->r[14] = 0x{nxt:08x}u; call_indirect(c, t); }}')
            return self.wrap(i, cond)
        # ----- SVC
        if (w >> 24) & 0xF == 0xF:
            out.append(f'svc(c, 0x{w & 0xFFFFFF:x});')
            return self.wrap(i, cond)
        # ----- multiplicações
        if w & 0x0F0000F0 == 0x00000090 and not (w >> 23) & 1 and not (w >> 22) & 1:
            rd, rn, rs, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 8) & 0xF, w & 0xF
            acc = f' + c->r[{rn}]' if (w >> 21) & 1 else ''
            out.append(f'c->r[{rd}] = c->r[{rm}] * c->r[{rs}]{acc};')
            if (w >> 20) & 1: out.append(f'SET_NZ(c->r[{rd}]);')
            return self.wrap(i, cond)
        if w & 0x0F8000F0 == 0x00800090:
            hi, lo, rs, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 8) & 0xF, w & 0xF
            signed, acc = (w >> 22) & 1, (w >> 21) & 1
            mul = (f'(uint64_t)((int64_t)(int32_t)c->r[{rm}] * (int32_t)c->r[{rs}])' if signed
                   else f'(uint64_t)c->r[{rm}] * c->r[{rs}]')
            a = f' + (((uint64_t)c->r[{hi}] << 32) | c->r[{lo}])' if acc else ''
            out.append(f'{{ uint64_t u = {mul}{a}; c->r[{lo}] = (uint32_t)u; c->r[{hi}] = (uint32_t)(u >> 32);'
                       + (' c->n = (uint32_t)(u >> 63); c->z = (u == 0);' if (w >> 20) & 1 else '') + ' }')
            return self.wrap(i, cond)
        # ----- multiplicações de 16 bits (SMULxy, SMLAxy, SMULWy, SMLAWy, SMLALxy)
        if w & 0x0F900090 == 0x01000080:
            op = (w >> 21) & 3
            rd, rn, rs, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 8) & 0xF, w & 0xF
            x, y = (w >> 5) & 1, (w >> 6) & 1
            hm = f'(int32_t)(int16_t)(c->r[{rm}] >> {16 * x})'
            hs = f'(int32_t)(int16_t)(c->r[{rs}] >> {16 * y})'
            if op == 3:
                out.append(f'c->r[{rd}] = (uint32_t)({hm} * {hs});')
            elif op == 0:
                out.append(f'c->r[{rd}] = (uint32_t)({hm} * {hs}) + c->r[{rn}];')
            elif op == 1:
                wy = f'(int64_t)(int32_t)c->r[{rm}] * {hs}'
                if x:   # SMULWy
                    out.append(f'c->r[{rd}] = (uint32_t)(({wy}) >> 16);')
                else:   # SMLAWy
                    out.append(f'c->r[{rd}] = (uint32_t)(({wy}) >> 16) + c->r[{rn}];')
            else:       # SMLALxy: RdHi=rd, RdLo=rn
                out.append(f'{{ uint64_t u = (((uint64_t)c->r[{rd}] << 32) | c->r[{rn}]) + (uint64_t)(int64_t)({hm} * {hs}); '
                           f'c->r[{rn}] = (uint32_t)u; c->r[{rd}] = (uint32_t)(u >> 32); }}')
            return self.wrap(i, cond)
        # ----- SIMD paralelo do ARMv6 (UADD8, UQSUB8, SADD16, ...)
        if w & 0x0F800F10 == 0x06000F10 and (w >> 20) & 7 in (1, 2, 3, 5, 6, 7) and (w >> 5) & 7 in (0, 1, 2, 3, 4, 7):
            out.append(f'c->r[{(w >> 12) & 0xF}] = par_addsub(c, {(w >> 20) & 7}, {(w >> 5) & 7}, '
                       f'c->r[{(w >> 16) & 0xF}], c->r[{w & 0xF}]);')
            return self.wrap(i, cond)
        if w & 0x0FF00FF0 == 0x06800FB0:   # SEL
            out.append(f'c->r[{(w >> 12) & 0xF}] = simd_sel(c, c->r[{(w >> 16) & 0xF}], c->r[{w & 0xF}]);')
            return self.wrap(i, cond)
        if w & 0x0FF00030 == 0x06800010:   # PKHBT / PKHTB
            rn, rd, sh, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 7) & 0x1F, w & 0xF
            if (w >> 6) & 1:
                v = f'(uint32_t)((int32_t)c->r[{rm}] >> {sh or 31})'
                out.append(f'c->r[{rd}] = (c->r[{rn}] & 0xffff0000u) | ({v} & 0xffffu);')
            else:
                out.append(f'c->r[{rd}] = (c->r[{rn}] & 0xffffu) | ((c->r[{rm}] << {sh}) & 0xffff0000u);')
            return self.wrap(i, cond)
        if w & 0x0FB003F0 == 0x06800070:   # SXTB16 / UXTB16 (+ variantes com soma)
            rn, rd, rot, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, ((w >> 10) & 3) * 8, w & 0xF
            src = f'ROR32(c->r[{rm}], {rot})' if rot else f'c->r[{rm}]'
            if (w >> 22) & 1:
                lo, hi = f'({src} & 0xffu)', f'(({src} >> 16) & 0xffu)'
            else:
                lo, hi = f'(uint32_t)(int8_t){src}', f'(uint32_t)(int8_t)({src} >> 16)'
            if rn != 15:
                lo, hi = f'(c->r[{rn}] + {lo})', f'((c->r[{rn}] >> 16) + {hi})'
            out.append(f'c->r[{rd}] = ({lo} & 0xffffu) | ({hi} << 16);')
            return self.wrap(i, cond)
        if w & 0x0FA00030 == 0x06A00010:   # SSAT / USAT
            uns = (w >> 22) & 1
            sat, rd, sh, typ, rn = (w >> 16) & 0x1F, (w >> 12) & 0xF, (w >> 7) & 0x1F, (w >> 6) & 1, w & 0xF
            v = (f'(int64_t)((int32_t)c->r[{rn}] >> {sh or 31})' if typ else f'(int64_t)(int32_t)(c->r[{rn}] << {sh})')
            out.append(f'c->r[{rd}] = sat_u(c, {v}, {sat});' if uns else
                       f'c->r[{rd}] = (uint32_t)sat_s(c, {v}, {sat + 1});')
            return self.wrap(i, cond)
        if w & 0x0FB00FF0 == 0x06A00F30:   # SSAT16 / USAT16
            uns = (w >> 22) & 1
            sat = (w >> 16) & 0xF
            out.append(f'c->r[{(w >> 12) & 0xF}] = sat16x2(c, c->r[{w & 0xF}], {sat if uns else sat + 1}, {uns});')
            return self.wrap(i, cond)
        if w & 0x0F900FF0 == 0x01000050:   # QADD / QSUB / QDADD / QDSUB
            out.append(f'c->r[{(w >> 12) & 0xF}] = qadd_sub(c, {(w >> 21) & 3}, c->r[{w & 0xF}], c->r[{(w >> 16) & 0xF}]);')
            return self.wrap(i, cond)
        if w & 0x0FF00090 == 0x07000010:   # SMLAD / SMLSD / SMUAD / SMUSD
            rd, ra, rm, rn = (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 8) & 0xF, w & 0xF
            m = f'ROR32(c->r[{rm}], 16)' if (w >> 5) & 1 else f'c->r[{rm}]'
            p1 = f'(int64_t)(int16_t)c->r[{rn}] * (int16_t){m}'
            p2 = f'(int64_t)(int16_t)(c->r[{rn}] >> 16) * (int16_t)({m} >> 16)'
            op = '-' if (w >> 6) & 1 else '+'
            acc = '' if ra == 15 else f' + (int32_t)c->r[{ra}]'
            out.append(f'c->r[{rd}] = (uint32_t)({p1} {op} {p2}{acc});')
            return self.wrap(i, cond)
        if w & 0x0FB0FFF0 == 0x0120F000 and not (w >> 22) & 1:   # MSR APSR, rX
            out.append(f'unpack_cpsr(c, c->r[{w & 0xF}], {(w >> 19) & 1}, {(w >> 18) & 1});')
            return self.wrap(i, cond)
        if w & 0x0FBF0FFF == 0x010F0000 and not (w >> 22) & 1:   # MRS rX, APSR
            out.append(f'c->r[{(w >> 12) & 0xF}] = pack_cpsr(c);')
            return self.wrap(i, cond)
        if w & 0x0FFF0F1F == 0x0E070F1A or w & 0x0FFF0FFF == 0x0E070F95:   # barreiras / cache (cp15 c7)
            return self.wrap(i, cond)
        # ----- LDREXB/H/D e STREXB/H/D (ARMv6K)
        if w & 0x0F900FFF == 0x01900F9F and (w >> 21) & 3 in (1, 2, 3):   # LDREXD/B/H
            sz = (w >> 21) & 3; rt, rn = (w >> 12) & 0xF, (w >> 16) & 0xF
            if sz == 1: out.append(f'{{ uint32_t ea = c->r[{rn}]; c->r[{rt}] = MEM32(ea); c->r[{rt + 1}] = MEM32(ea + 4); }}')
            else: out.append(f'c->r[{rt}] = {"MEM8" if sz == 2 else "MEM16"}(c->r[{rn}]);')
            return self.wrap(i, cond)
        if w & 0x0F900FF0 == 0x01800F90 and (w >> 21) & 3 in (1, 2, 3):   # STREXD/B/H
            sz = (w >> 21) & 3; rd, rn, rt = (w >> 12) & 0xF, (w >> 16) & 0xF, w & 0xF
            if sz == 1: out.append(f'{{ uint32_t ea = c->r[{rn}]; MEM32(ea) = c->r[{rt}]; MEM32(ea + 4) = c->r[{rt + 1}]; c->r[{rd}] = 0; }}')
            else: out.append(f'{"MEM8" if sz == 2 else "MEM16"}(c->r[{rn}]) = ({"uint8_t" if sz == 2 else "uint16_t"})c->r[{rt}]; c->r[{rd}] = 0;')
            return self.wrap(i, cond)
        # ----- LDREX / STREX
        if w & 0x0FF00FFF == 0x01900F9F:
            out.append(f'c->r[{(w >> 12) & 0xF}] = MEM32(c->r[{(w >> 16) & 0xF}]);')
            return self.wrap(i, cond)
        if w & 0x0FF00FF0 == 0x01800F90:
            out.append(f'MEM32(c->r[{(w >> 16) & 0xF}]) = c->r[{w & 0xF}]; c->r[{(w >> 12) & 0xF}] = 0;')
            return self.wrap(i, cond)
        # ----- CLZ
        if w & 0x0FFF0FF0 == 0x016F0F10:
            out.append(f'c->r[{(w >> 12) & 0xF}] = CLZ32(c->r[{w & 0xF}]);')
            return self.wrap(i, cond)
        # ----- extensões ARMv6 (SXTB/UXTH/... e SXTAB/...)
        if w & 0x0F8003F0 == 0x06800070 and (w >> 20) & 0xF in (0xA, 0xB, 0xE, 0xF):
            op = (w >> 20) & 0xF
            rn, rd, rot, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, ((w >> 10) & 3) * 8, w & 0xF
            src = f'ROR32(c->r[{rm}], {rot})' if rot else f'c->r[{rm}]'
            ext = {0xA: f'(uint32_t)(int32_t)(int8_t){src}', 0xB: f'(uint32_t)(int32_t)(int16_t){src}',
                   0xE: f'({src} & 0xffu)', 0xF: f'({src} & 0xffffu)'}[op]
            add = '' if rn == 15 else f'c->r[{rn}] + '
            out.append(f'c->r[{rd}] = {add}{ext};')
            return self.wrap(i, cond)
        if w & 0x0FFF0FF0 == 0x06BF0F30:
            out.append(f'c->r[{(w >> 12) & 0xF}] = __builtin_bswap32(c->r[{w & 0xF}]);')
            return self.wrap(i, cond)
        if w & 0x0FFF0FF0 == 0x06BF0FB0:
            rm = w & 0xF
            out.append(f'c->r[{(w >> 12) & 0xF}] = ((c->r[{rm}] & 0x00ff00ffu) << 8) | ((c->r[{rm}] >> 8) & 0x00ff00ffu);')
            return self.wrap(i, cond)
        # ----- NOP / hints
        if w & 0x0FFFFF00 == 0x0320F000:
            return self.wrap(i, cond)
        # ----- leitura do TLS (mrc p15, 0, rX, c13, c0, 3)
        if w & 0x0FFF0FFF == 0x0E1D0F70:
            out.append(f'c->r[{(w >> 12) & 0xF}] = c->tls;')
            return self.wrap(i, cond)
        # ----- load/store meia-palavra, signed e duplo
        if w & 0x0E000090 == 0x00000090 and (w >> 5) & 3:
            return self.wrap(self.ldst_misc(i, w), cond)
        # ----- processamento de dados
        if (w >> 26) & 3 == 0 and w & 0x02000090 == 0x00000090:
            return self.unimpl(i, cond)   # espaço de multiplicação/acessos extras não reconhecido: nunca é aritmética
        if (w >> 26) & 3 == 0 and not (w & 0x0F900000 == 0x01000000):
            return self.wrap(self.dataproc(i, w, entry), cond)
        # ----- load/store palavra/byte
        if (w >> 26) & 3 == 1 and not (w & 0x02000010 == 0x02000010):
            return self.wrap(self.ldst(i, w), cond)
        # ----- LDM/STM
        if (w >> 25) & 7 == 4:
            return self.wrap(self.ldm(i, w), cond)
        # ----- VFP
        if (w >> 9) & 7 == 5 and (w >> 25) & 7 in (6, 7):
            r = self.vfp(i, w)
            if r is not None:
                return self.wrap(r, cond)
        return self.unimpl(i, cond)

    def decode_uncond(self, i, w, nxt):
        i.succ = [nxt]
        if w & 0xFE000000 == 0xFA000000:  # BLX imediato -> Thumb
            t = (i.addr + 8 + sx(w & 0xFFFFFF, 24) * 4 + ((w >> 24) & 1) * 2) & 0xFFFFFFFF
            self.tcalls.add(t)
            i.code.append(f'c->r[14] = 0x{nxt:08x}u; T_{t:08x}(c);')
            return i
        if w & 0xFD70F000 == 0xF550F000:  # PLD
            return i
        if w == 0xF57FF01F:                # CLREX: LDREX/STREX sempre têm sucesso aqui, então é no-op
            return i
        return self.unimpl(i, 0xE)

    def unimpl(self, i, cond):
        i.ok = False
        i.code = [f'UNIMPL(c, 0x{i.addr:08x}u, 0x{i.w:08x}u);']
        key = f'{(i.w >> 20) & 0xFF:02x}'
        self.unimpl_hist[key] = self.unimpl_hist.get(key, 0) + 1
        return self.wrap(i, cond)

    def wrap(self, i, cond):
        if cond != 0xE and COND[cond] and i.code:
            body = ' '.join(i.code)
            i.code = [f'if ({COND[cond]}) {{ {body} }}']
            # instrução condicional nunca encerra o fluxo: se a condição falhar, segue para a próxima
            nxt = i.addr + i.size
            if nxt not in i.succ:
                i.succ.append(nxt)
        return i

    def fn(self, t):
        return f'F_{t:08x}'

    # ------------------------------------------------------------ dataproc
    def dataproc(self, i, w, entry):
        op = (w >> 21) & 0xF
        s = (w >> 20) & 1
        rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
        a = self.reg(rn, i.addr)
        logical = op in (0, 1, 8, 9, 12, 13, 14, 15)
        b, carry = self.shifter(w, i.addr, s and logical)
        out = i.code
        pre = 'uint32_t sc = c->c; ' if 'sc' in b else ''
        res = {0: f'{a} & {b}', 1: f'{a} ^ {b}', 2: f'{a} - {b}', 3: f'{b} - {a}', 4: f'{a} + {b}',
               5: f'{a} + {b} + c->c', 6: f'{a} - {b} - !c->c', 7: f'{b} - {a} - !c->c',
               8: f'{a} & {b}', 9: f'{a} ^ {b}', 10: f'{a} - {b}', 11: f'{a} + {b}',
               12: f'{a} | {b}', 13: f'{b}', 14: f'{a} & ~{b}', 15: f'~{b}'}[op]
        test = op in (8, 9, 10, 11)
        if rn == 15 and (w & (1 << 25)) and op in (2, 4) and rd != 15:
            # ADR (`add/sub rX, pc, #imm`): se aponta para código ARM, é provável ponteiro de sub-rotina
            imm = ror32(w & 0xFF, ((w >> 8) & 0xF) * 2)
            t = (i.addr + 8 + (imm if op == 4 else -imm)) & 0xFFFFFFFF
            if t & 3 == 0 and self.is_code(t) and self.read32(t) >> 28 == 0xE:
                self.calls.add(t)
            # ADR longo em duas instruções: `sub rX, pc, #a; sub rX, rX, #b` (só se o alvo começa com push)
            if self.is_code(i.addr + 4):
                n2 = self.read32(i.addr + 4)
                if n2 >> 28 == 0xE and n2 & 0x0FE00000 in (0x02400000, 0x02800000) \
                        and (n2 >> 16) & 0xF == rd and (n2 >> 12) & 0xF == rd:
                    imm2 = ror32(n2 & 0xFF, ((n2 >> 8) & 0xF) * 2)
                    t2 = (t + (imm2 if (n2 >> 21) & 0xF == 4 else -imm2)) & 0xFFFFFFFF
                    if t2 & 3 == 0 and self.is_code(t2) and self.read32(t2) >> 16 == 0xE92D:
                        self.calls.add(t2)
        flags = ''
        if s or test:
            if logical:
                flags = f' SET_NZ(r); c->c = {carry};'
            else:
                x, y = (b, a) if op in (3, 7) else (a, b)
                fn = {2: 'SUB', 3: 'SUB', 10: 'SUB', 4: 'ADD', 11: 'ADD', 5: 'ADC', 6: 'SBC', 7: 'SBC'}[op]
                flags = f' FLAGS_{fn}(x, y, r);'
                pre += f'uint32_t x = {x}, y = {y}; '
                res = {'SUB': 'x - y', 'ADD': 'x + y', 'ADC': 'x + y + c->c', 'SBC': 'x - y - !c->c'}[fn]
        if test:
            out.append(f'{{ {pre}uint32_t r = {res};{flags} }}')
            return i
        if rd == 15:
            # escrita no PC: retorno, tabela de salto ou salto indireto
            if op == 13 and not (w & (1 << 25)) and w & 0xFFF == 14:
                out.append('JUMP(c->r[14]);'); i.kind = 'ret'; i.succ = []
                return i
            if op == 4 and rn == 15 and not (w & (1 << 25)) and (w >> 4) & 0xFF == 0x10:  # add pc, pc, rX, lsl #2
                n = self.jumptable_size(i.addr)
                if n is not None:
                    rm = w & 0xF
                    cases = [i.addr + 8 + 4 * k for k in range(n)]
                    out.append(f'switch (c->r[{rm}]) {{ ' + ' '.join(
                        f'case {k}: goto L_{t:08x};' for k, t in enumerate(cases)) + ' }')
                    i.succ = cases + [i.addr + 4]
                    i.table = cases
                    return i
            ret = self.lr_return(i.addr)
            if op == 4 and not (w & (1 << 25)) and rn != 15:
                self.relative_table_seeds(i.addr, rn, w & 0xF)
            if ret is not None:   # chamada estilo ARMv4 (lr preparado antes)
                out.append(f'{{ {pre}uint32_t r = {res}; call_indirect(c, r); }}' + ('' if ret == i.addr + 4 else f' goto L_{ret:08x};'))
                if ret != i.addr + 4: i.kind, i.target, i.succ = 'jmp', ret, [ret]
                return i
            out.append(f'{{ {pre}uint32_t r = {res}; JUMP(r); }}')
            i.kind = 'ret'; i.succ = []
            return i
        out.append(f'{{ {pre}uint32_t r = {res};{flags} c->r[{rd}] = r; }}')
        return i

    def lr_return(self, addr):
        """Se uma das 4 instruções antes de `addr` pôs um endereço fixo em lr (`mov lr, pc` /
        `add lr, pc, #imm`) e nada mais mexeu em lr depois, devolve esse endereço (retorno da chamada)."""
        extra = 0
        for back in range(1, 6):
            a = addr - 4 * back
            if not self.is_code(a): return None
            p = self.read32(a)
            # escrita incondicional em pc (add pc, ..., ldr pc, ..., bx): o fluxo não chega daqui
            if p >> 28 == 0xE and (((p >> 26) & 3 == 0 and (p >> 12) & 0xF == 15 and (p & 0x0FFFFFF0) != 0x012FFF10 and (p >> 21) & 0xF not in (8, 9, 10, 11))
                                    or ((p >> 26) & 3 == 1 and (p >> 20) & 1 and (p >> 12) & 0xF == 15)
                                    or (p & 0x0FFFFFF0) == 0x012FFF10
                                    or (p & 0x0E108000) == 0x08108000):
                return None
            if p & 0x0FFFF000 == 0x028EE000 and p >> 28 == 0xE:          # add lr, lr, #imm (soma ao endereço)
                extra += ror32(p & 0xFF, ((p >> 8) & 0xF) * 2); continue
            if (p >> 25) & 7 == 5 and p >> 28 != 0xF and not (p >> 24) & 1: continue   # outro desvio (bne alvo)
            if p >> 28 != 0xE: return None
            if p & 0x0FFFFFFF == 0x01A0E00F: return a + 8 + extra
            if p & 0x0FFFF000 == 0x028FE000: return (a + 8 + ror32(p & 0xFF, ((p >> 8) & 0xF) * 2) + extra) & 0xFFFFFFFF
            if (p >> 25) & 7 == 5:
                if (p >> 24) & 1: return None                       # bl no meio
                continue                                            # outro desvio que também é chamada (bne alvo)
            rd = (p >> 12) & 0xF
            if rd == 14 and (p >> 26) & 3 in (0, 1): return None   # outra escrita em lr
            if p & 0x0E10A000 == 0x08104000: return None            # ldm com lr
        return None

    def relative_table_seeds(self, addr, ra, rb):
        base = None
        for back in range(1, 6):
            p = self.read32(addr - 4 * back)
            if p & 0x0FEF0000 == 0x024F0000 or p & 0x0FEF0000 == 0x028F0000:   # sub/add rX, pc, #imm
                if (p >> 12) & 0xF in (ra, rb):
                    imm = ror32(p & 0xFF, ((p >> 8) & 0xF) * 2)
                    base = (addr - 4 * back + 8 + (imm if (p >> 21) & 0xF == 4 else -imm)) & 0xFFFFFFFF
                    break
        if base is None: return
        for k in range(256):
            e = self.read32(base + 4 * k)
            t = (base + e) & 0xFFFFFFFF
            if e > 0x100000 and e < 0xFFF00000 or not self.is_code(t) or t & 3: break
            self.calls.add(t)

    def sets_lr_to(self, a, value):
        """A instrução em `a` põe `value` em lr? (`mov lr, pc` ou `add lr, pc, #imm`)"""
        if not self.is_code(a): return False
        p = self.read32(a)
        if p & 0x0FFFFFFF == 0x01A0E00F: return a + 8 == value
        if p & 0x0FFFF000 == 0x028FE000: return a + 8 + ror32(p & 0xFF, ((p >> 8) & 0xF) * 2) == value
        return False

    def jumptable_size(self, addr):
        """Número de entradas da tabela, a partir do `cmp rX, #N` que a protege.
        Condição LS (índice <= N) -> N+1 entradas; CC/LO (índice < N) -> N entradas."""
        cond = self.read32(addr) >> 28
        for back in (4, 8):
            p = self.read32(addr - back)
            if p & 0x0FF00000 == 0x03500000:  # cmp rX, #imm
                n = ror32(p & 0xFF, ((p >> 8) & 0xF) * 2)
                if cond == 9: return n + 1      # LS
                if cond == 3: return n          # CC
                return None
        return None

    # ------------------------------------------------------------ load/store
    def address(self, i, w, off):
        rn = (w >> 16) & 0xF
        p, u, wb = (w >> 24) & 1, (w >> 23) & 1, (w >> 21) & 1
        base = self.reg(rn, i.addr)
        sign = '+' if u else '-'
        if rn == 15 and p and not wb and off.endswith('u') and off.startswith('0x'):
            ea = (i.addr + 8 + (int(off[:-1], 16) if u else -int(off[:-1], 16))) & 0xFFFFFFFF
            return f'0x{ea:08x}u', None, ea
        ea = f'{base} {sign} {off}' if p else base
        upd = f'c->r[{rn}] = {base} {sign} {off};' if (not p or wb) else None
        return ea, upd, None

    def ldst(self, i, w):
        load, byte = (w >> 20) & 1, (w >> 22) & 1
        rd = (w >> 12) & 0xF
        if w & (1 << 25):
            off, _ = self.shifter(w & ~(1 << 25), i.addr, False)
            off = f'({off})'
        else:
            off = f'0x{w & 0xFFF:x}u'
        ea, upd, const = self.address(i, w, off)
        out = i.code
        if load and const is not None and not byte and self.is_code(const):
            # carga de literal no pool: o código é somente leitura, então o valor vira constante
            v = self.read32(const)
            if rd == 15:
                if v in self.funcs:   # veneer `ldr pc, =alvo`: vira chamada direta
                    self.calls.add(v)
                    out.append(f'TAIL({self.fn(v)});')
                else:
                    out.append(f'c->tret = ret_; call_indirect(c, 0x{v:08x}u); return;')
                i.kind = 'ret'; i.succ = []
            else:
                out.append(f'c->r[{rd}] = 0x{v:08x}u;')
            return i
        m = 'MEM8' if byte else 'MEM32'
        if (load and rd == 15 and (w >> 16) & 0xF == 15 and w & 0x03A00FF0 == 0x03800100):
            # ldr pc, [pc, rX, lsl #2]: tabela de salto com endereços absolutos (estilo ARMCC)
            n = self.jumptable_size(i.addr)
            if n is not None:
                rm = w & 0xF
                cases = [self.read32(i.addr + 8 + 4 * k) for k in range(n)]
                out.append(f'switch (c->r[{rm}]) {{ ' + ' '.join(
                    f'case {k}: goto L_{t:08x};' for k, t in enumerate(cases)) + ' }')
                i.succ = cases + [i.addr + 4]
                i.table = cases
                return i
        if load:
            if rd == 15:
                rn = (w >> 16) & 0xF
                if rn == 13 and upd:
                    out.append(f'{upd} return;')
                else:
                    out.append(f'{{ uint32_t ea = {ea}; uint32_t t = {m}(ea);' + (f' {upd}' if upd else '') +
                               ' JUMP(t); }')
                i.kind = 'ret'; i.succ = []
                return i
            out.append(f'{{ uint32_t ea = {ea}; uint32_t t = {m}(ea);' + (f' {upd}' if upd else '') +
                       f' c->r[{rd}] = t; }}')
        else:
            val = self.reg(rd, i.addr)
            out.append(f'{{ uint32_t ea = {ea}; {m}(ea) = ({"uint8_t" if byte else "uint32_t"}){val};' +
                       (f' {upd}' if upd else '') + ' }')
        return i

    def ldst_misc(self, i, w):
        load = (w >> 20) & 1
        sh = (w >> 5) & 3
        rd = (w >> 12) & 0xF
        off = (f'0x{((w >> 4) & 0xF0) | (w & 0xF):x}u' if (w >> 22) & 1 else f'c->r[{w & 0xF}]')
        ea, upd, const = self.address(i, w, off)
        if const is not None:
            ea = f'0x{const:08x}u'
        u = f' {upd}' if upd else ''
        out = i.code
        if sh == 1:
            if load: out.append(f'{{ uint32_t ea = {ea}; uint32_t t = MEM16(ea);{u} c->r[{rd}] = t; }}')
            else: out.append(f'{{ uint32_t ea = {ea}; MEM16(ea) = (uint16_t)c->r[{rd}];{u} }}')
        elif load:
            t = 'int8_t' if sh == 2 else 'int16_t'
            m = 'MEM8' if sh == 2 else 'MEM16'
            out.append(f'{{ uint32_t ea = {ea}; uint32_t t = (uint32_t)(int32_t)({t}){m}(ea);{u} c->r[{rd}] = t; }}')
        elif sh == 2:   # LDRD
            out.append(f'{{ uint32_t ea = {ea}; uint32_t t0 = MEM32(ea), t1 = MEM32(ea + 4);{u} '
                       f'c->r[{rd}] = t0; c->r[{rd + 1}] = t1; }}')
        else:           # STRD
            out.append(f'{{ uint32_t ea = {ea}; MEM32(ea) = c->r[{rd}]; MEM32(ea + 4) = c->r[{rd + 1}];{u} }}')
        return i

    def ldm(self, i, w):
        p, u, s, wb, load = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
        rn = (w >> 16) & 0xF
        regs = [r for r in range(16) if w & (1 << r)]
        n = len(regs)
        if s or not regs:
            return self.unimpl(i, 0xE)
        start = {(0, 1): '0', (1, 1): '4', (0, 0): f'-{4 * n - 4}', (1, 0): f'-{4 * n}'}[(p, u)]
        out = [f'uint32_t a = c->r[{rn}] + (uint32_t)({start});']
        newbase = f'c->r[{rn}] {"+" if u else "-"} {4 * n}u'
        for k, r in enumerate(regs):
            if load:
                if r != 15: out.append(f'c->r[{r}] = MEM32(a + {4 * k});')
            else:
                v = f'0x{i.addr + 12:08x}u' if r == 15 else f'c->r[{r}]'
                out.append(f'MEM32(a + {4 * k}) = {v};')
        if wb and not (load and rn in regs):
            out.insert(1, f'uint32_t nb = {newbase};')
            out.append(f'c->r[{rn}] = nb;')
        if load and 15 in regs:
            if rn == 13:
                out.append('return;')
            else:
                out.append(f'JUMP(MEM32(a + {4 * regs.index(15)}));')
            i.kind = 'ret'; i.succ = []
        i.code.append('{ ' + ' '.join(out) + ' }')
        return i

    # ------------------------------------------------------------ VFP (VFPv2)
    def vfp(self, i, w):
        dbl = (w >> 8) & 1
        D, N, M = (w >> 22) & 1, (w >> 7) & 1, (w >> 5) & 1
        vd, vn, vm = (w >> 12) & 0xF, (w >> 16) & 0xF, w & 0xF
        def S(v, b): return f'c->s[{(v << 1) | b}]'
        def Dr(v, b): return f'c->d[{v | (b << 4)}]'
        R = Dr if dbl else S
        d, n, m = R(vd, D), R(vn, N), R(vm, M)
        out = i.code
        top = (w >> 24) & 0xF
        # VLDR / VSTR
        if w & 0x0F200E00 == 0x0D000A00:
            imm = (w & 0xFF) * 4
            rn = (w >> 16) & 0xF
            sign = '+' if (w >> 23) & 1 else '-'
            base = f'0x{(i.addr + 8) & ~3:08x}u' if rn == 15 else f'c->r[{rn}]'
            ea = f'{base} {sign} {imm}u'
            ty = 'MEMF64' if dbl else 'MEMF32'
            out.append(f'{d} = {ty}({ea});' if (w >> 20) & 1 else f'{ty}({ea}) = {d};')
            if NANCHECK and (w >> 20) & 1:
                out.append(f'if ({d} != {d}) nan_log(c, 0x{i.addr:08x}u, (double)({ea}), -1);')
            return i
        # VLDM / VSTM / VPUSH / VPOP
        if top in (0xC, 0xD) and w & 0x0E000E00 == 0x0C000A00 and (w >> 23) & 3 in (1, 2):
            p, u, wb, load = (w >> 24) & 1, (w >> 23) & 1, (w >> 21) & 1, (w >> 20) & 1
            rn, imm = (w >> 16) & 0xF, w & 0xFF
            if p == 1 and u == 1:
                return None
            first = (vd | (D << 4)) if dbl else ((vd << 1) | D)
            cnt = imm // 2 if dbl else imm
            ops = [f'uint32_t a = c->r[{rn}]' + (f' - {imm * 4}u;' if p else ';')]
            for k in range(cnt):
                reg = f'c->d[{first + k}]' if dbl else f'c->s[{first + k}]'
                ty = 'MEMF64' if dbl else 'MEMF32'
                step = 8 if dbl else 4
                ops.append(f'{reg} = {ty}(a + {k * step});' if load else f'{ty}(a + {k * step}) = {reg};')
                if NANCHECK and load and rn != 13:
                    ops.append(f'if ({reg} != {reg}) nan_log(c, 0x{i.addr:08x}u, (double)(a + {k * step}), -2);')
            if wb:
                ops.append(f'c->r[{rn}] = c->r[{rn}] {"+" if u else "-"} {imm * 4}u;' if not p else
                           f'c->r[{rn}] = a;')
            out.append('{ ' + ' '.join(ops) + ' }')
            return i
        # VMOV ARM <-> simples
        if w & 0x0FE00F7F == 0x0E000A10:
            rt = (w >> 12) & 0xF
            s = f'c->s[{(vn << 1) | N}]'
            out.append(f'memcpy(&c->r[{rt}], &{s}, 4);' if (w >> 20) & 1 else f'memcpy(&{s}, &c->r[{rt}], 4);')
            return i
        # VMOV dois ARM <-> double / dois simples
        if w & 0x0FE00FD0 == 0x0C400B10 or w & 0x0FE00FD0 == 0x0C400A10:
            rt, rt2 = (w >> 12) & 0xF, (w >> 16) & 0xF
            if w & 0x100:
                tgt = f'c->d[{vm | (M << 4)}]'
                if (w >> 20) & 1:
                    out.append(f'{{ uint64_t u; memcpy(&u, &{tgt}, 8); c->r[{rt}] = (uint32_t)u; c->r[{rt2}] = (uint32_t)(u >> 32); }}')
                else:
                    out.append(f'{{ uint64_t u = ((uint64_t)c->r[{rt2}] << 32) | c->r[{rt}]; memcpy(&{tgt}, &u, 8); }}')
            else:
                s0 = (vm << 1) | M
                if (w >> 20) & 1:
                    out.append(f'memcpy(&c->r[{rt}], &c->s[{s0}], 4); memcpy(&c->r[{rt2}], &c->s[{s0 + 1}], 4);')
                else:
                    out.append(f'memcpy(&c->s[{s0}], &c->r[{rt}], 4); memcpy(&c->s[{s0 + 1}], &c->r[{rt2}], 4);')
            return i
        # VMRS
        if w & 0x0FFF0FFF == 0x0EF10A10:
            rt = (w >> 12) & 0xF
            if rt == 15:
                out.append('c->n = c->fpscr >> 31; c->z = (c->fpscr >> 30) & 1; c->c = (c->fpscr >> 29) & 1; c->v = (c->fpscr >> 28) & 1;')
            else:
                out.append(f'c->r[{rt}] = c->fpscr;')
            return i
        if w & 0x0FFF0FFF == 0x0EE10A10:
            out.append(f'c->fpscr = c->r[{(w >> 12) & 0xF}];')
            return i
        if top != 0xE or (w >> 4) & 1:
            return None
        opc1 = ((w >> 23) & 1) << 2 | ((w >> 20) & 3)
        op = (w >> 6) & 1
        T = 'double' if dbl else 'float'
        bin_ops = {(0, 0): f'{d} += {n} * {m};', (0, 1): f'{d} -= {n} * {m};',
                   (1, 0): f'{d} = -{d} + {n} * {m};', (1, 1): f'{d} = -{d} - {n} * {m};',
                   (2, 0): f'{d} = {n} * {m};', (2, 1): f'{d} = -({n} * {m});',
                   (3, 0): f'{d} = {n} + {m};', (3, 1): f'{d} = {n} - {m};',
                   (4, 0): f'{d} = {n} / {m};'}
        if (opc1, op) in bin_ops:
            out.append(bin_ops[(opc1, op)])
            if NANCHECK:   # depuração: registra onde um NaN nasce (NANCHECK=1 na geração)
                out.append(f'if ({d} != {d}) nan_log(c, 0x{i.addr:08x}u, (double){n}, (double){m});')
            return i
        if opc1 != 7:
            return None
        opc2, opc3 = vn, (w >> 6) & 3
        if (opc3 & 1) == 0:
            return None  # VMOV imediato (VFPv3), inexistente no ARM11
        if opc2 == 0:
            out.append(f'{d} = {m};' if opc3 == 1 else f'{d} = fabs{"" if dbl else "f"}({m});')
        elif opc2 == 1:
            out.append(f'{d} = -{m};' if opc3 == 1 else f'{d} = sqrt{"" if dbl else "f"}({m});')
            if NANCHECK and opc3 != 1: out.append(f'if ({d} != {d}) nan_log(c, 0x{i.addr:08x}u, (double){m}, 0);')
        elif opc2 in (4, 5):
            rhs = f'({T})0' if opc2 == 5 else m
            out.append(f'VCMP(c, {d}, {rhs});')
        elif opc2 == 7 and opc3 == 3:
            if dbl:
                out.append(f'c->s[{(vd << 1) | D}] = (float){Dr(vm, M)};')
            else:
                out.append(f'c->d[{vd | (D << 4)}] = (double){S(vm, M)};')
        elif opc2 == 8:
            src = f'c->s[{(vm << 1) | M}]'
            ty = 'int32_t' if (w >> 7) & 1 else 'uint32_t'
            out.append(f'{{ uint32_t t; memcpy(&t, &{src}, 4); {d} = ({T})({ty})t; }}')
        elif opc2 in (12, 13):
            dst = f'c->s[{(vd << 1) | D}]'
            fn = 'f2s' if opc2 == 13 else 'f2u'
            out.append(f'{{ uint32_t t = {fn}{"d" if dbl else "f"}({m}); memcpy(&{dst}, &t, 4); }}')
        else:
            return None
        return i

    # ------------------------------------------------------------ funções
    def function(self, entry, thumb=False, max_insns=20000):
        self.thumb_lit = {}
        insns = {}
        work = [entry]
        while work:
            a = work.pop()
            while a not in insns and self.is_code(a) and len(insns) < max_insns:
                ins = self.decode_thumb(a, entry) if thumb else self.decode(a, self.read32(a), entry)
                insns[a] = ins
                self.stats['insns'] += 1
                if not ins.ok: self.stats['unimpl'] += 1
                nxt = a + ins.size
                for t in ins.succ[1:]:
                    work.append(t)
                if not ins.succ:
                    break
                if ins.succ[0] != nxt:
                    work.append(ins.succ[0]); break
                a = nxt
        labels = {entry}
        for ins in insns.values():
            for t in ins.succ:
                if t != ins.addr + ins.size: labels.add(t)
            if ins.kind == 'jmp': labels.add(ins.target)
            if ins.table: labels.update(ins.table)
        name = f'T_{entry:08x}' if thumb else self.fn(entry)
        lines = [f'void {name}(Ctx *c) {{']
        if __import__('os').environ.get('RECOMP_VERIFY'):     # verificação contra o interpretador (runtime/vfy.c)
            lines.append(f'  VFY_ENTRY(0x{entry | (1 if thumb else 0):08x}u, {name});')
        lines.append('  uint32_t ret_ = c->tret ? c->tret : c->r[14]; c->tret = 0; (void)ret_;   /* retorno esperado */')
        if not thumb:                                        # opções do menu que mexem no jogo (runtime: g_opt_*)
            for sig, code in HOOKS:
                if all(self.read32(entry + 4 * k) == w for k, w in enumerate(sig)):
                    lines.append(code)
        if insns and min(insns) != entry:
            # há código compartilhado num endereço menor que a entrada: começa pela entrada
            lines.append(f'  goto L_{entry:08x};')
        prev = None
        for a in sorted(insns):
            ins = insns[a]
            pend = prev.addr + prev.size if prev else None
            if prev is not None and a != pend and pend in prev.succ:
                lines.append(f'  goto L_{pend:08x};')
            if a in labels or prev is None or a != pend:
                lines.append(f'L_{a:08x}: (void)0;')
            if a in self.probes:
                lines.append(f'  probe(c, 0x{a:08x}u);')
            for l in ins.code:
                lines.append('  ' + l)
            prev = ins
        if prev is not None:
            pend = prev.addr + prev.size
            if pend in prev.succ and pend not in insns:
                if not thumb and pend in self.funcs:
                    self.calls.add(pend)
                    lines.append(f'  TAIL({self.fn(pend)});')
                elif thumb and pend in self.tfuncs:
                    self.tcalls.add(pend)
                    lines.append(f'  TAIL(T_{pend:08x});')
                else:
                    lines.append(f'  UNIMPL(c, 0x{pend:08x}u, 0);')
        lines.append('}')
        self.stats['funcs'] += 1
        return '\n'.join(lines), len(insns)

    # ------------------------------------------------------------ Thumb (ARMv6, Thumb-1)
    def read16(self, a):
        return (self.read32(a & ~3) >> (16 * ((a >> 1) & 1))) & 0xFFFF

    def as_arm(self, a, w, entry):
        """Traduz uma instrução Thumb pelo seu equivalente ARM (PC lido como a+4, como no Thumb)."""
        i = self.decode(a - 4, w, entry)
        i.addr, i.size = a, 2
        i.succ = [a + 2] if i.succ else []
        return i

    def thumb_branch(self, i, t, cond, entry):
        if not self.is_code(t):
            i.code.append(f'c->tret = ret_; call_indirect(c, 0x{t | 1:08x}u); return;')
            i.kind = 'tail'
            i.succ = [i.addr + 2] if cond != 0xE else []
        elif t in self.tfuncs and t != entry:
            self.tcalls.add(t)
            i.code.append(f'TAIL(T_{t:08x});')
            i.kind = 'tail'
            i.succ = [i.addr + 2] if cond != 0xE else []
        else:
            i.code.append(f'goto L_{t:08x};')
            i.kind, i.target = 'jmp', t
            i.succ = [t] + ([i.addr + 2] if cond != 0xE else [])
        return self.wrap(i, cond)

    def decode_thumb(self, a, entry):
        h = self.read16(a)
        i = Insn(a, h, 2)
        i.succ = [a + 2]
        out = i.code
        pc = a + 4
        top5 = h >> 11
        lo3, mid3 = h & 7, (h >> 3) & 7
        if top5 <= 2:                                   # LSL/LSR/ASR imediato
            return self.as_arm(a, 0xE1B00000 | lo3 << 12 | ((h >> 6) & 31) << 7 | top5 << 5 | mid3, entry)
        if top5 == 3:                                   # ADD/SUB registrador ou imediato de 3 bits
            rn, rm = mid3, (h >> 6) & 7
            if (h >> 10) & 1:
                base = 0xE2500000 if (h >> 9) & 1 else 0xE2900000
                return self.as_arm(a, base | rn << 16 | lo3 << 12 | rm, entry)
            base = 0xE0500000 if (h >> 9) & 1 else 0xE0900000
            return self.as_arm(a, base | rn << 16 | lo3 << 12 | rm, entry)
        if top5 >> 2 == 1:                              # MOV/CMP/ADD/SUB imediato de 8 bits
            op, rd, imm = (h >> 11) & 3, (h >> 8) & 7, h & 0xFF
            w = [0xE3B00000 | rd << 12, 0xE3500000 | rd << 16,
                 0xE2900000 | rd << 16 | rd << 12, 0xE2500000 | rd << 16 | rd << 12][op] | imm
            return self.as_arm(a, w, entry)
        if h >> 10 == 0x10:                             # operações da ULA
            op, rd, rm = (h >> 6) & 0xF, lo3, mid3
            dn = rd << 16 | rd << 12
            w = {0: 0xE0100000 | dn | rm, 1: 0xE0300000 | dn | rm,
                 2: 0xE1B00010 | rd << 12 | rm << 8 | rd, 3: 0xE1B00030 | rd << 12 | rm << 8 | rd,
                 4: 0xE1B00050 | rd << 12 | rm << 8 | rd, 5: 0xE0B00000 | dn | rm, 6: 0xE0D00000 | dn | rm,
                 7: 0xE1B00070 | rd << 12 | rm << 8 | rd, 8: 0xE1100000 | rd << 16 | rm,
                 9: 0xE2700000 | rm << 16 | rd << 12, 10: 0xE1500000 | rd << 16 | rm,
                 11: 0xE1700000 | rd << 16 | rm, 12: 0xE1900000 | dn | rm,
                 13: 0xE0100090 | rd << 16 | rd << 8 | rm, 14: 0xE1D00000 | dn | rm,
                 15: 0xE1F00000 | rd << 12 | rm}[op]
            return self.as_arm(a, w, entry)
        if h >> 10 == 0x11:                             # registradores altos e BX/BLX
            op = (h >> 8) & 3
            rd = ((h >> 7) & 1) << 3 | lo3
            rm = (h >> 3) & 0xF
            src = f'0x{pc:08x}u' if rm == 15 else f'c->r[{rm}]'
            if op == 3:
                if (h >> 7) & 1:                        # BLX rm
                    out.append(f'{{ uint32_t t = {src}; c->r[14] = 0x{(a + 2) | 1:08x}u; call_indirect(c, t); }}')
                    return i
                out.append(f'JUMP({src});')
                i.kind = 'ret'; i.succ = []
                return i
            if rd == 15:
                if op == 2 and rm == 14:
                    out.append('JUMP(c->r[14]);')
                else:
                    v = f'{src}' if op == 2 else f'0x{pc:08x}u + {src}'
                    out.append(f'JUMP(({v}) | 1);')
                i.kind = 'ret'; i.succ = []
                return i
            if op == 1:
                return self.as_arm(a, 0xE1500000 | rd << 16 | rm, entry)
            if op == 0:
                if rm == 15 and rd in self.thumb_lit:
                    # `ldr rX, =lit; add rX, pc`: ponteiro relativo ao PC (callbacks em código Thumb do ARMCC)
                    t = (self.thumb_lit[rd] + pc) & 0xFFFFFFFF
                    if self.is_code(t & ~1):
                        (self.tcalls if t & 1 else self.calls).add(t & ~1)
                out.append(f'c->r[{rd}] = c->r[{rd}] + {src};')
            else:
                out.append(f'c->r[{rd}] = {src};')
            return i
        if top5 == 9:                                   # LDR rd, [pc, #imm]
            rd = (h >> 8) & 7
            ea = (pc & ~3) + (h & 0xFF) * 4
            if self.is_code(ea):
                self.thumb_lit[rd] = self.read32(ea)
                out.append(f'c->r[{rd}] = 0x{self.read32(ea):08x}u;')
            else:
                out.append(f'c->r[{rd}] = MEM32(0x{ea:08x}u);')
            return i
        if h >> 12 == 5:                                # load/store com offset em registrador
            op, rm, rn, rd = (h >> 9) & 7, (h >> 6) & 7, mid3, lo3
            ea = f'c->r[{rn}] + c->r[{rm}]'
            out.append([f'MEM32({ea}) = c->r[{rd}];', f'MEM16({ea}) = (uint16_t)c->r[{rd}];',
                        f'MEM8({ea}) = (uint8_t)c->r[{rd}];', f'c->r[{rd}] = (uint32_t)(int32_t)(int8_t)MEM8({ea});',
                        f'c->r[{rd}] = MEM32({ea});', f'c->r[{rd}] = MEM16({ea});', f'c->r[{rd}] = MEM8({ea});',
                        f'c->r[{rd}] = (uint32_t)(int32_t)(int16_t)MEM16({ea});'][op])
            return i
        if h >> 13 == 3 or h >> 12 == 8:                # load/store com offset imediato
            imm, rn, rd = (h >> 6) & 31, mid3, lo3
            load = (h >> 11) & 1
            if h >> 12 == 8:
                m, off, ty = 'MEM16', imm * 2, 'uint16_t'
            elif (h >> 12) & 1:
                m, off, ty = 'MEM8', imm, 'uint8_t'
            else:
                m, off, ty = 'MEM32', imm * 4, 'uint32_t'
            ea = f'c->r[{rn}] + {off}u'
            out.append(f'c->r[{rd}] = {m}({ea});' if load else f'{m}({ea}) = ({ty})c->r[{rd}];')
            return i
        if h >> 12 == 9:                                # load/store relativo ao SP
            rd, off = (h >> 8) & 7, (h & 0xFF) * 4
            out.append(f'c->r[{rd}] = MEM32(c->r[13] + {off}u);' if (h >> 11) & 1 else
                       f'MEM32(c->r[13] + {off}u) = c->r[{rd}];')
            return i
        if h >> 12 == 0xA:                              # ADD rd, pc/sp, #imm
            rd, off = (h >> 8) & 7, (h & 0xFF) * 4
            out.append(f'c->r[{rd}] = c->r[13] + {off}u;' if (h >> 11) & 1 else
                       f'c->r[{rd}] = 0x{(pc & ~3) + off:08x}u;')
            return i
        if h >> 12 == 0xB:                              # diversos
            if h >> 8 == 0xB0:
                off = (h & 0x7F) * 4
                out.append(f'c->r[13] = c->r[13] {"-" if (h >> 7) & 1 else "+"} {off}u;')
                return i
            if h >> 8 == 0xB2:
                op, rm, rd = (h >> 6) & 3, mid3, lo3
                out.append(f'c->r[{rd}] = ' + ['(uint32_t)(int32_t)(int16_t)', '(uint32_t)(int32_t)(int8_t)',
                                              '(uint16_t)', '(uint8_t)'][op] + f'c->r[{rm}];')
                return i
            if h >> 9 == 0x5A:                          # PUSH
                return self.as_arm(a, 0xE92D0000 | (h & 0xFF) | ((h >> 8) & 1) << 14, entry)
            if h >> 9 == 0x5E:                          # POP
                return self.as_arm(a, 0xE8BD0000 | (h & 0xFF) | ((h >> 8) & 1) << 15, entry)
            if h >> 8 == 0xBA and (h >> 6) & 3 != 2:
                op, rm, rd = (h >> 6) & 3, mid3, lo3
                if op == 0: out.append(f'c->r[{rd}] = __builtin_bswap32(c->r[{rm}]);')
                elif op == 1: out.append(f'c->r[{rd}] = ((c->r[{rm}] & 0x00ff00ffu) << 8) | ((c->r[{rm}] >> 8) & 0x00ff00ffu);')
                else: out.append(f'c->r[{rd}] = (uint32_t)(int32_t)(int16_t)__builtin_bswap16((uint16_t)c->r[{rm}]);')
                return i
            if h & 0xFFE8 == 0xB660:                    # CPS
                return i
            return self.unimpl(i, 0xE)
        if h >> 12 == 0xC:                              # STMIA/LDMIA rn!
            rn = (h >> 8) & 7
            base = 0xE8B00000 if (h >> 11) & 1 else 0xE8A00000
            return self.as_arm(a, base | rn << 16 | (h & 0xFF), entry)
        if h >> 12 == 0xD:                              # B condicional / SVC
            cond = (h >> 8) & 0xF
            if cond == 0xF:
                out.append(f'svc(c, 0x{h & 0xFF:x});')
                return i
            if cond == 0xE:
                return self.unimpl(i, 0xE)
            return self.thumb_branch(i, (pc + sx(h & 0xFF, 8) * 2) & 0xFFFFFFFF, cond, entry)
        if top5 == 0x1C:                                # B
            return self.thumb_branch(i, (pc + sx(h & 0x7FF, 11) * 2) & 0xFFFFFFFF, 0xE, entry)
        if top5 == 0x1E:                                # BL / BLX (par de 16 bits)
            h2 = self.read16(a + 2)
            if h2 >> 11 not in (0x1F, 0x1D):
                return self.unimpl(i, 0xE)
            off = (sx(h & 0x7FF, 11) << 12) | ((h2 & 0x7FF) << 1)
            t = (pc + off) & 0xFFFFFFFF
            i.size = 4
            i.succ = [a + 4]
            ret = f'c->r[14] = 0x{(a + 4) | 1:08x}u; '
            if h2 >> 11 == 0x1F:
                if self.is_code(t):
                    self.tcalls.add(t); out.append(ret + f'T_{t:08x}(c);')
                else:
                    out.append(ret + f'call_indirect(c, 0x{t | 1:08x}u);')
            else:
                t &= ~3
                if self.is_code(t):
                    self.calls.add(t); out.append(ret + f'{self.fn(t)}(c);')
                else:
                    out.append(ret + f'call_indirect(c, 0x{t:08x}u);')
            return i
        return self.unimpl(i, 0xE)

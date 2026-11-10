"""Junta mods que trocam o MESMO arquivo do RomFS (ex.: uma tradução e um mod que adiciona um Pokémon, os dois
mexendo no arquivo de textos a/0/7/4).

Arquivos GARC (o formato de pacote dos jogos de Pokémon do 3DS) são juntados por item: o item que só um mod mudou
vem desse mod. Se mais de um mod mudou o mesmo item e ele é um arquivo de texto da 6ª geração, a junção é por linha:
a linha que só um mod mudou vem dele; quando os dois mudaram a mesma linha, vence o mod que mudou MENOS linhas no
arquivo (a mudança pontual, como o nome de um item novo, vence a tradução inteira). Linhas novas são acrescentadas.
Itens binários mudados por mais de um mod: vence o mod com menos itens mudados no pacote. Arquivos que não são
GARC: vence o último mod da lista (ordem alfabética).

Formatos: GARC v4 (CRAG/OTAF/BTAF/BMIF) e texto da 6ª geração (linhas cifradas com chave 0x7C89 + 0x2983*n,
girando 3 bits por caractere), como documentados pela comunidade (pk3DS / Project Pokémon).
"""
import struct


# ---------------------------------------------------------------- GARC
def garc_ler(d):
    """-> lista de itens; cada item é uma lista de subarquivos (bytes); ou None se não for GARC v4"""
    if len(d) < 0x1C or d[:4] != b'CRAG':
        return None
    hl = struct.unpack_from('<I', d, 4)[0]
    data_off = struct.unpack_from('<I', d, 0x10)[0]
    o = hl
    if d[o:o + 4] != b'OTAF':
        return None
    o += struct.unpack_from('<I', d, o + 4)[0]
    if d[o:o + 4] != b'BTAF':
        return None
    nb = struct.unpack_from('<I', d, o + 8)[0]
    p = o + 12
    itens = []
    for _ in range(nb):
        bits = struct.unpack_from('<I', d, p)[0]; p += 4
        subs = []
        for k in range(32):
            if bits >> k & 1:
                s, e, ln = struct.unpack_from('<III', d, p); p += 12
                subs.append((k, d[data_off + s:data_off + s + ln]))
        itens.append(subs)
    return itens


def garc_escrever(itens):
    fatb = b''; offs = []; dados = bytearray(); maior = 0
    for subs in itens:
        offs.append(len(fatb))
        bits = 0
        for k, _ in subs:
            bits |= 1 << k
        fatb += struct.pack('<I', bits)
        for _, b in subs:
            s = len(dados)
            dados += b
            fatb += struct.pack('<III', s, s + len(b), len(b))
            while len(dados) % 4:
                dados += b'\0'
            maior = max(maior, (len(b) + 3) & ~3)
    fato = b'OTAF' + struct.pack('<IHH', 12 + 4 * len(itens), len(itens), 0xFFFF) + b''.join(struct.pack('<I', o) for o in offs)
    fatb = b'BTAF' + struct.pack('<II', 12 + len(fatb), len(itens)) + fatb
    fimb = b'BMIF' + struct.pack('<II', 12, len(dados))
    data_off = 0x1C + len(fato) + len(fatb) + len(fimb)
    cab = b'CRAG' + struct.pack('<IHHIIII', 0x1C, 0xFEFF, 0x0400, 4, data_off, data_off + len(dados), maior)
    return cab + fato + fatb + fimb + bytes(dados)


# ---------------------------------------------------------------- texto da 6ª geração
def texto_ler(b):
    """-> lista de linhas (tuplas de u16, sem o terminador) ou None"""
    try:
        secs, n, total, init, sdo = struct.unpack_from('<HHIII', b, 0)
        if secs != 1 or sdo != 0x10 or n > 20000 or sdo + 4 + 8 * n > len(b):
            return None
        linhas = []; key = 0x7C89
        for i in range(n):
            off, ln, _ = struct.unpack_from('<IHH', b, sdo + 4 + i * 8)
            if sdo + off + ln * 2 > len(b):
                return None
            k = key; cs = []
            for j in range(ln):
                cs.append(struct.unpack_from('<H', b, sdo + off + j * 2)[0] ^ k)
                k = ((k << 3) | (k >> 13)) & 0xFFFF
            if 0 in cs:
                cs = cs[:cs.index(0)]
            linhas.append(tuple(cs))
            key = (key + 0x2983) & 0xFFFF
        return linhas
    except struct.error:
        return None


def texto_escrever(linhas):
    n = len(linhas)
    tab = b''; dados = b''; key = 0x7C89
    base = 4 + 8 * n
    for i, cs in enumerate(linhas):
        cs = list(cs) + [0]
        k = key; enc = b''
        for c in cs:
            enc += struct.pack('<H', c ^ k); k = ((k << 3) | (k >> 13)) & 0xFFFF
        while len(enc) % 4:
            enc += struct.pack('<H', k); k = ((k << 3) | (k >> 13)) & 0xFFFF
        tab += struct.pack('<IHH', base + len(dados), len(cs), 0)
        dados += enc
        key = (key + 0x2983) & 0xFFFF
    sec = struct.pack('<I', 4 + len(tab) + len(dados)) + tab + dados
    return struct.pack('<HHIII', 1, n, len(sec), 0, 0x10) + sec


def juntar_texto(base, versoes):
    """base: linhas originais; versoes: [linhas de cada mod] -> linhas juntadas"""
    mud = [sum(1 for i in range(max(len(base), len(v))) if i >= len(base) or i >= len(v) or v[i] != base[i]) for v in versoes]
    ordem = sorted(range(len(versoes)), key=lambda m: -mud[m])      # quem muda mais primeiro; o pontual sobrescreve
    n = max([len(base)] + [len(v) for v in versoes])
    out = []
    for i in range(n):
        linha = base[i] if i < len(base) else None
        for m in ordem:
            v = versoes[m]
            if i < len(v) and (i >= len(base) or v[i] != base[i]):
                linha = v[i]
        out.append(linha if linha is not None else ())
    return out


# ---------------------------------------------------------------- junção
def juntar(base, versoes):
    """base: bytes do arquivo original; versoes: [bytes de cada mod, na ordem da lista].
    -> (bytes juntados, descrição) ou (None, motivo) se não der para juntar (aí vale o último)"""
    gb = garc_ler(base)
    gv = [garc_ler(v) for v in versoes]
    if gb is None or any(g is None for g in gv):
        return None, 'não é um pacote GARC'
    n = max([len(gb)] + [len(g) for g in gv])
    mudados = [sum(1 for i in range(n) if i >= len(gb) or i >= len(g) or g[i] != gb[i]) for g in gv]
    res = []; por_linha = conflito = 0
    for i in range(n):
        orig = gb[i] if i < len(gb) else None
        quem = [m for m, g in enumerate(gv) if i < len(g) and g[i] != orig]
        if not quem:
            res.append(orig if orig is not None else [])
            continue
        if len(quem) == 1:
            res.append(gv[quem[0]][i])
            continue
        item = None
        if orig is not None and len(orig) == 1 and all(len(gv[m][i]) == 1 for m in quem):
            tb = texto_ler(orig[0][1])
            tv = [texto_ler(gv[m][i][0][1]) for m in quem]
            if tb is not None and all(t is not None for t in tv):
                item = [(orig[0][0], texto_escrever(juntar_texto(tb, tv)))]
                por_linha += 1
        if item is None:                                  # binário: vence o mod com menos itens mudados
            m = min(quem, key=lambda m: (mudados[m], -m))
            item = gv[m][i]; conflito += 1
        res.append(item)
    return garc_escrever(res), f'{n} itens, {por_linha} juntados por linha, {conflito} em conflito'

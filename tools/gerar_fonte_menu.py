"""Gera runtime/fonte_menu.h: as fontes do menu (no jogo e no lançador), a partir da Inter e, para o japonês,
da Noto Sans CJK JP (as duas com licença SIL Open Font License 1.1, que permite embutir e redistribuir). Três
tamanhos, em tons de cinza, com os zeros comprimidos (0 seguido do número de zeros). Tamanhos pensados para
uma tela de 1440 linhas; o desenho escala para as outras.

Os caracteres japoneses são os que aparecem nos textos do programa (menu do jogo, lançador): rode de novo
depois de mudar uma tradução.

  python3 -I tools/gerar_fonte_menu.py [pasta das fontes Inter] [pasta da Noto Sans CJK]
"""
import os, sys
from PIL import Image, ImageDraw, ImageFont

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIR = sys.argv[1] if len(sys.argv) > 1 else '/usr/share/fonts/opentype/inter'
CJK_DIR = sys.argv[2] if len(sys.argv) > 2 else '/usr/share/fonts/opentype/noto'


from fontTools.ttLib import TTFont  # noqa: E402
NA_INTER = TTFont(os.path.join(DIR, 'Inter-Bold.otf')).getBestCmap()


def cjk(c):                                   # caracteres que a Inter não tem (japonês): vêm da Noto Sans CJK JP
    return ord(c) > 127 and ord(c) not in NA_INTER


TEXTOS = ['runtime/menu_jogo.h', 'runtime/idioma_ui.h', 'lancador/lancador.c']
LATIN = [chr(c) for c in range(32, 127)] + [chr(c) for c in range(0xA0, 0x100)] + list('–—…•‹›‘’“”✓←→↑↓')
# japonês e qualquer outro símbolo dos textos que não esteja na lista acima (ex.: ※)
JA = sorted({c for t in TEXTOS for c in open(os.path.join(RAIZ, t), encoding='utf-8').read() if ord(c) > 127 and c not in LATIN})
FONTES = [  # nome, arquivo, tamanho em pixels, caracteres
    ('TITULO', 'Inter-ExtraBold.otf', 128, list(' PokémonXY')),
    ('ITEM', 'Inter-Bold.otf', 46, LATIN + JA),
    ('PEQ', 'Inter-SemiBold.otf', 30, LATIN + JA),
]
CJK_ARQ = {'ITEM': ('NotoSansCJK-Bold.ttc', 0.92), 'PEQ': ('NotoSansCJK-Medium.ttc', 0.92)}   # (índice 0: JP)

def rle(b):
    out = bytearray(); i = 0
    while i < len(b):
        if b[i] == 0:
            n = 1
            while i + n < len(b) and b[i + n] == 0 and n < 255: n += 1
            out += bytes((0, n)); i += n
        else:
            out.append(b[i]); i += 1
    return bytes(out)

out = os.path.join(RAIZ, 'runtime', 'fonte_menu.h')
dados = bytearray(); fontes = []
for nome, arq, px, chars in FONTES:
    f0 = ImageFont.truetype(os.path.join(DIR, arq), px)
    asc, desc = f0.getmetrics()
    fj = ImageFont.truetype(os.path.join(CJK_DIR, CJK_ARQ[nome][0]), int(round(px * CJK_ARQ[nome][1])), index=0) if nome in CJK_ARQ else None
    gl = []
    for ch in chars:
        f = fj if (fj and cjk(ch)) else f0
        try:                                  # (sem desenho na Inter, como o ※: tenta a Noto)
            if ch != ' ' and f.getmask(ch).getbbox() is None:
                if f is f0 and fj and fj.getmask(ch).getbbox() is not None: f = fj
                else: continue
        except Exception: continue
        adv = f.getlength(ch)
        l, t, r, b = f.getbbox(ch, anchor='ls')
        w, h = max(0, r - l), max(0, b - t)
        if w and h:
            im = Image.new('L', (w + 2, h + 2), 0)
            ImageDraw.Draw(im).text((1 - l, 1 - t), ch, font=f, fill=255, anchor='ls')
            w, h = w + 2, h + 2; l -= 1; t -= 1
            off = len(dados); dados += rle(im.tobytes())
        else:
            off = len(dados); w = h = 0
        gl.append((ord(ch), int(round(adv * 16)), w, h, l, t, off))
    fontes.append((nome, px, asc, desc, gl))

with open(out, 'w', encoding='utf-8') as o:
    o.write('/* Gerado por tools/gerar_fonte_menu.py a partir da Inter (Rasmus Andersson; SIL Open Font License 1.1, que\n'
            ' * permite embutir e redistribuir). Glifos em tons de cinza; zeros comprimidos: 0 seguido da contagem. */\n')
    o.write('typedef struct { unsigned cp; unsigned short adv16; unsigned char w, h; signed char x0, y0; unsigned off; } FmGlifo;\n')
    o.write('typedef struct { int px, asc, desc, n; const FmGlifo *g; } FmFonte;\n')
    for nome, px, asc, desc, gl in fontes:
        o.write(f'static const FmGlifo fm_{nome.lower()}_g[{len(gl)}] = {{\n')
        for g in gl: o.write('  {%d,%d,%d,%d,%d,%d,%d},\n' % g)
        o.write('};\n')
    o.write('static const unsigned char fm_px[] = {\n')
    for i in range(0, len(dados), 40):
        o.write(','.join(str(b) for b in dados[i:i + 40]) + ',\n')
    o.write('};\n')
    o.write('enum { ' + ', '.join(f'FM_{n}' for n, *_ in fontes) + ' };\n')
    o.write('static const FmFonte fm_fontes[] = {\n')
    for nome, px, asc, desc, gl in fontes:
        o.write(f'  {{ {px}, {asc}, {desc}, {len(gl)}, fm_{nome.lower()}_g }},\n')
    o.write('};\n')
print(f'{out}: {len(dados)} bytes de glifos')

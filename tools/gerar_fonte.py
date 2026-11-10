"""Gera runtime/fonte_aviso.h: uma fonte em bitmap (tons de cinza) para os avisos na janela do jogo
("Controle conectado", etc.), a partir da DejaVu Sans Bold (licença livre: Bitstream Vera / DejaVu, que
permite embutir e redistribuir) e, para o japonês, da Noto Sans CJK JP Bold (SIL Open Font License 1.1).
Latin-1 (português, espanhol...), hiragana e katakana (apelidos de Pokémon) e os caracteres japoneses dos
textos dos avisos: rode de novo depois de mudar uma tradução.

  python3 -I tools/gerar_fonte.py [caminho/DejaVuSans-Bold.ttf] [caminho/NotoSansCJK-Bold.ttc]
"""
import os, sys
from PIL import Image, ImageDraw, ImageFont

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TTF = sys.argv[1] if len(sys.argv) > 1 else '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
ALTURA = 26                                   # pixels por linha (a janela escala conforme o tamanho)
CJK = sys.argv[2] if len(sys.argv) > 2 else '/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc'
from fontTools.ttLib import TTFont  # noqa: E402
TEM = TTFont(TTF).getBestCmap()
CHARS = [chr(c) for c in range(32, 127)] + [chr(c) for c in range(0xA1, 0x100) if c != 0xAD] + list('–—…→•')
KANA = [chr(c) for c in range(0x3041, 0x3097)] + [chr(c) for c in range(0x30A1, 0x30FF)] + list('、。「」・ー！？（）：～')
TEXTOS = ['runtime/input.c', 'runtime/pica_vk.c', 'runtime/display_sdl.c', 'runtime/captura.c']
JA = sorted({c for t in TEXTOS for c in open(os.path.join(RAIZ, t), encoding='utf-8').read() if ord(c) > 127 and ord(c) not in TEM})
CHARS += [c for c in KANA + JA if c not in CHARS]

f = ImageFont.truetype(TTF, 20)
fj = ImageFont.truetype(CJK, 19, index=0)          # (índice 0 da coleção: JP)
asc, desc = f.getmetrics()
base = (ALTURA - (asc + desc)) // 2 + asc     # linha de base dentro da célula
glifos = []
for ch in CHARS:
    fo = f if ord(ch) in TEM else fj
    w = max(1, int(round(fo.getlength(ch))))
    im = Image.new('L', (w, ALTURA), 0)
    ImageDraw.Draw(im).text((0, base), ch, font=fo, fill=255, anchor='ls')
    glifos.append((ord(ch), im.size[0], im.tobytes()))

out = os.path.join(RAIZ, 'runtime', 'fonte_aviso.h')
with open(out, 'w', encoding='utf-8') as o:
    o.write('/* Gerado por tools/gerar_fonte.py a partir da DejaVu Sans Bold (fontes DejaVu: licença livre derivada da\n'
            ' * Bitstream Vera, que permite embutir e redistribuir). Bitmap em tons de cinza, uma célula por caractere. */\n')
    o.write(f'#define FONTE_ALTURA {ALTURA}\n#define FONTE_N {len(glifos)}\n')
    o.write('static const struct { unsigned cp; unsigned char w; unsigned off; } g_fonte_idx[FONTE_N] = {\n')
    off = 0
    for cp, w, _ in glifos:
        o.write(f'  {{ {cp}, {w}, {off} }},\n'); off += w * ALTURA
    o.write('};\nstatic const unsigned char g_fonte_px[] = {\n')
    dados = b''.join(px for _, _, px in glifos)
    for i in range(0, len(dados), 32):
        o.write('  ' + ','.join(str(b) for b in dados[i:i + 32]) + ',\n')
    o.write('};\n')
print(f'{out}: {len(glifos)} caracteres, {off} bytes')

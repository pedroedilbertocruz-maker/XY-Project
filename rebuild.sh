#!/bin/sh
# Regenera o C do jogo, recompila tudo, empacota a imagem e roda.
# Uso: CODE=caminho/code.bin ROMFS=caminho/romfs ./rebuild.sh
set -e
cd "$(dirname "$0")"
CODE=${CODE:-in/code.bin}
ROMFS=${ROMFS:-romfs}
python3 tools/recomp_game.py "$CODE" "$ROMFS" out | grep -E "funções|alterados"
cp tools/Makefile.out out/Makefile
mkdir -p out/obj && (cd out && make -j"$(nproc)" -k > build.log 2>&1; echo "erros de compilação: $(grep -c ERRO build.log || true)")
python3 tools/pack.py out/image out/image.pack > /dev/null
./run.sh

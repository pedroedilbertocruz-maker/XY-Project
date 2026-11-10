#!/bin/sh
# Linux/WSL: ./jogar.sh caminho/do/jogo.3ds   (no Windows use o jogar.bat)
cd "$(dirname "$0")" && exec python3 tools/jogar.py "$@"

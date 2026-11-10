#!/bin/sh
# Compila o lançador (XY Project.exe) com o llvm-mingw. Uso: MINGW=/caminho/llvm-mingw/bin ./compilar.sh
set -e
cd "$(dirname "$0")"
M=${MINGW:-}
P=${M:+$M/}x86_64-w64-mingw32-
${P}windres lancador.rc -O coff -o lancador.res
${P}clang -O2 -Wall -municode -mwindows -static lancador.c lancador.res \
    -lcomdlg32 -lshell32 -lole32 -lwinmm -s -o "../XY Project.exe"
rm -f lancador.res
echo "ok: ../XY Project.exe"

#!/bin/sh
# Recompila só o runtime, linka e roda o jogo (log em out/run.log). T=segundos de limite (padrão 60).
set -e
cd "$(dirname "$0")"
mkdir -p out/rt
for f in runtime kernel services gsp ldr input y2r plat display_null; do clang -O1 -g -w -Iruntime -c runtime/$f.c -o out/rt/$f.o; done
for f in pica pica_draw pica_shader pica_light pica_gl; do clang -O2 -march=native -fno-math-errno -g -w -Iruntime -c runtime/$f.c -o out/rt/$f.o; done   # renderer: otimizado
for f in dsp_hle audio/audio_core/hle/source audio/audio_core/hle/mixers audio/audio_core/hle/filter audio/audio_core/codec audio/audio_core/interpolate; do   # som (C++)
    o=out/rt/$(basename $f).o; [ -f "$o" ] && [ "$o" -nt runtime/$f.cpp ] || clang++ -std=c++20 -O2 -w -Iruntime/audio -c runtime/$f.cpp -o "$o"; done
FAAD="-DHAVE_INTTYPES_H=1 -DHAVE_MEMCPY=1 -DHAVE_STRING_H=1 -DHAVE_STRINGS_H=1 -DHAVE_SYS_STAT_H=1 -DHAVE_SYS_TYPES_H=1 -DHAVE_LRINTF=1 -DPACKAGE_VERSION=\"2.11\" -Iruntime/faad2/libfaad -Iruntime/faad2/include"
mkdir -p out/rt/faad
for f in runtime/faad2/libfaad/*.c; do o=out/rt/faad/$(basename $f .c).o; [ -f "$o" ] || clang -O2 -w $FAAD -c $f -o $o; done   # AAC (músicas)
clang -O1 -g -w -Iruntime -Iruntime/faad2/include -c runtime/dsp_aac.c -o out/rt/dsp_aac.o
clang++ -rdynamic out/obj/*.o out/rt/*.o out/rt/faad/*.o -lm -lpthread -o out/pokemon_x
cd out
status=0
LOG_IPC=${LOG_IPC:-0} ROMFS=${ROMFS:-../romfs} ROMFS_META=${ROMFS_META:-romfs_virtual} timeout "${T:-60}" ./pokemon_x image.pack > run.log 2>&1 || status=$?
[ "$status" = 124 ] && echo "parou pelo limite de tempo (o jogo ainda estava rodando)" || echo "saiu com código $status"
echo "log: out/run.log ($(wc -l < run.log) linhas)"

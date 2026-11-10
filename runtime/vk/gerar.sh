#!/bin/sh
# Gera vk_shaders.h (SPIR-V embutido) a partir dos shaders em vk/. Precisa do glslangValidator (Khronos);
# só é preciso rodar quando um shader mudar: o vk_shaders.h gerado vai junto com o código.
set -e
G=${GLSLANG:-glslangValidator}
cd "$(dirname "$0")"
OUT=../vk_shaders.h
echo "/* gerado por vk/gerar.sh a partir dos shaders em vk/ (glslangValidator -V) */" > $OUT
for f in main.vert main.frag full.vert conv.frag; do
  n=$(echo $f | tr '.' '_')
  $G -V --target-env vulkan1.3 $f -o /tmp/$n.spv >/dev/null
  python3 -c "
import sys,struct
d=open('/tmp/$n.spv','rb').read(); w=struct.unpack('<%dI'%(len(d)//4),d)
print('static const uint32_t k_spv_$n[] = {')
for i in range(0,len(w),8): print('    '+', '.join('0x%08x'%x for x in w[i:i+8])+',')
print('};')" >> $OUT
done

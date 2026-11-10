# XY Project

Port nativo de Pokémon X e Pokémon Y (3DS) para PC · Native PC port of Pokémon X and Pokémon Y (3DS) ·
Port nativo de Pokémon X y Pokémon Y (3DS) para PC · ポケットモンスター X・Y（3DS）の PC ネイティブ移植

**[Português](#português) · [English](#english) · [Español](#español) · [日本語](#日本語)**

---

## Português

Este projeto transforma o código de máquina do Pokémon X e do Pokémon Y (ARM11 do 3DS) em **C nativo**
que compila e roda no PC, sem emulador de CPU. Nenhum arquivo do jogo vem junto:
cada pessoa usa o dump do próprio cartucho.

### Estado atual

| Etapa | Status |
|---|---|
| Parser dos módulos CRO/CRS (110 módulos `.cro` + `static.crs`) | pronto |
| Linker estático: `code.bin` + todos os CROs numa imagem única | pronto (CROs em `0x40000000`) |
| Descoberta de funções (BL/BLX, vtables, literais, tabelas relativas, exports dos CROs, chamadas no estilo ARMv4, ADR) | ~74.150 funções |
| Recompilador ARMv6K + VFPv2 + SIMD do ARMv6 + Thumb-1 → C | 3,0 milhões de instruções, 99,1% traduzidas |
| Testes diferenciais (ARM e Thumb, -O1/-O2/-Os/-O3) | 83/83 ARM |
| Kernel HLE: threads, escalonador de núcleo único com preempção por prazo, eventos, mutexes, timers, árbitro | pronto |
| Serviços: srv:, APT, hid, cfg, fs (RomFS + saves em pastas do PC), dsp (protocolo), gsp, ldr:ro, y2r | prontos o suficiente |
| **GPU PICA200 em software**: listas de comando, shaders de vértice, rasterização, texturas (14 formatos + ETC1), TEV, blending, profundidade/estêncil, **iluminação por pixel** (8 luzes, LUTs, mapa de normais) | pronto |
| Entrada: teclado, mouse (toque) e controle XInput na janela do Windows; roteiro `TECLAS` e `teclas_agora.txt` | pronto |
| Vídeo Mobiclip (`.moflex`): decodificador do próprio jogo + conversão YUV→RGB do `y2r` | roda |
| Seleção de idioma → "Begin game" → vídeo de abertura completo (Xerneas, "Presented by Game Freak") → A → introdução (`DllIntro`) | pronto |
| Introdução do Professor Sycamore em 3D: cenário, Poké Ball iluminada, quadros com Pokémon, diálogos, perguntas e escolha da aparência do personagem | **chega aqui** |
| Introdução completa até o quarto do personagem (primeiro mapa, menu do PSS na tela de baixo) | etapa 7.5 |
| Modelos 3D dos personagens (professor, Gogoat, personagem do jogador nos espelhos) e animação do logo "X" | **aparecem** (faltava o salto entre listas de comandos da GPU, registradores 0x238-0x23D) |
| Desempenho: rasterização em todos os núcleos + cache de vértices (~1,6x num PC de 2 núcleos; mais com mais núcleos) | etapa 6.1 |
| Leitura direta do `.3ds` decifrado + recompilação automática na primeira execução | pronto |
| Windows: `jogar.bat` baixa Python portátil + llvm-mingw + SDL2, compila e abre a janela | **funciona** (testado pelo usuário) |
| **GPU na placa de vídeo (OpenGL 3.3)**: rasterização, texturas, iluminação, TEV, blending, profundidade/estêncil num shader; vértices e recorte continuam na CPU | etapa 7 (o software fica como reserva: `RENDER=software`) |
| OpenGL: framebuffers ficam na GPU entre listas e quadros (só voltam à memória quando a CPU precisa ler); limpezas de tela viram glClear; shader de vértice em todos os núcleos | etapa 7.1 |
| GPU assíncrona: os comandos GX rodam numa thread própria em paralelo com o jogo (como no 3DS); listas da PICA copiadas no envio | etapa 7.3 (`GPU_SINCRONA=1` desliga) |
| Shader de geometria da PICA (partículas, brilhos, raios de luz, efeitos de golpes) | etapa 7.7 (`PICA_SEM_GS=1` desliga) |
| Shader de vértice ~6-9x mais rápido: instruções decodificadas uma vez por programa e executadas em lotes de 16 vértices (SIMD), com máscara para desvios divergentes; plano de leitura de atributos por desenho; getenv em cache; grupo de threads com distribuição dinâmica de blocos (não espera threads que acordam tarde) | etapa 7.8 (`PICA_VSCHECK=1` confere o lote com o interpretador escalar) |
| Treinador na abertura das batalhas: respeita o bloqueio de escrita de cor/profundidade (0x113/0x115); profundidade+estêncil lida como textura mesmo sendo o buffer do próprio desenho | corrigido na 7.9 |
| Kernel: CloseHandle libera os handles (antes esgotavam em ~25 min de jogo) | corrigido na 7.9 |
| GPU: texturas projetivas (tc0/tc0w), mipmaps com filtro/bias/níveis do 0x84, neblina (fog) com a tabela 0xE6-0xEF | 7.9 |
| Transferência de tela linear->blocos (e flip / tiled->tiled): mosaico no fim da cena do Fletchling; profundidade lida como cor com a precisão do float24 (tracejado nos contornos) | 7.9 |
| Resolução interna 1x-4x (F8 ou RESOLUCAO=n): superfícies da GPU em escala, redução/ampliação por blit ao ir/voltar da memória, tela de cima mostrada em escala na janela | 8.0 |
| Layout: tela de cima em 16:9, tela de baixo sob demanda no canto (Tab), mochila abre no canto | 8.0 |
| Som: o DSP espera o jogo atender o quadro anterior (picotes) e a fila de saída se ajusta sozinha (45-150 ms) | 8.1 |
| Windows: esperas do kernel precisas (temporizador de alta resolução + evento, contador de alta precisão); antes acordavam até 17 ms atrasadas e o jogo perdia o VBlank (fps oscilando abaixo de 30) | 8.1 |
| Lançador `XY Project.exe` (ROM, resolução, filtro de textura, botões do controle, idioma, JOGAR); controle: cursor de toque no analógico direito (R3 toca, L3 mostra a tela de baixo), botões por posição ou rótulo, controles DirectInput genéricos, aviso na tela ao conectar/desconectar e filtros de textura no renderer (bilinear forçado, anisotrópico 16x, bicúbico Catmull-Rom; F7) | 8.2 |
| **Pokémon Y** roda com o mesmo programa (o lançador e o `jogar.py` recompilam qualquer uma das duas versões; a janela mostra "Pokémon X" ou "Pokémon Y"); projeto renomeado para Pokémon XY | 8.2 |
| Tela de baixo também em alta resolução (antes só a de cima era mostrada em escala); ícone do Pokémon XY na janela e na barra de tarefas do jogo; threads do jogo com prioridade acima do normal no Windows (SEM_PRIORIDADE=1 desliga) | 8.3 |
| **60 fps** (em pausa: fora do lançador; só para testes com `FPS=60 FPS60_TESTE=1`): a lógica do jogo continua a 30 passos por segundo; a cada quadro, o renderer OpenGL desenha de novo o quadro com cada vértice na metade do caminho entre o quadro anterior e o atual (em cópias das superfícies, sem tocar na memória do jogo) e a janela mostra intermediário e real, um por VBlank. Troca de cena ou câmera: o quadro só repete. Custa ~1 VBlank (17 ms) de atraso e o dobro de trabalho da placa de vídeo. Linha [60fps] no perfil | 8.3 |
| **Vulkan 1.3** (opção "Renderizador de vídeo" no lançador, `RENDER=vulkan`): mesmo trabalho do OpenGL (shaders portados em `runtime/vk/`, embutidos já compilados em `vk_shaders.h` por `vk/gerar.sh`), tudo fora da tela, com lotes de comandos enviados no fim de cada lista e leituras assíncronas das telas. Se o Vulkan não abrir (sem placa 1.3, driver antigo), volta ao OpenGL sozinho | 8.4 |
| Busca de funções mais completa na primeira compilação (prólogos ARM/Thumb e ponteiros para código Thumb nos literal pools): +1.900 funções, inclusive as chamadas só por ponteiro montado em tempo de execução (crash no centro Pokémon). Botão **Recompilar tudo** no lançador (refaz o jogo a partir da ROM; o save fica) | 8.3 |
| Contornos dos personagens sumindo depois de batalhas (framebuffer em cache preso a textura apagada) | corrigido na 8.1 |
| Desempenho: recorte de triângulos com aceitação direta e sem cópias; cópia das telas da GPU assíncrona (PBO + fence) sem segurar o jogo; a janela só mostra uma tela com a cópia concluída e nunca volta para um quadro mais velho (GL_LEITURA_SINCRONA=1 volta à cópia na hora); linha [ritmo] no perfil | 8.1 |
| Som: anel com ajuste fino de velocidade (sem estalos quando a fila esvazia ou enche) | 7.9 |
| Texturas ETC1: modo individual com as cores dos sub-blocos trocadas (contornos picotados) | corrigido na 7.6 |
| Barra de título com fps (quadros desenhados por segundo), velocidade (100% = 60 VBlanks/s) e renderer em uso | etapa 7 |
| **Som**: DSP em HLE (mixagem das 24 vozes, filtros, ADPCM/PCM, do Azahar) + músicas AAC pelo faad2 | novo nesta etapa |
| **Janela SDL2** (Windows e Linux): tela redimensionável, tela cheia (F11), teclado, controles, mouse como toque, som | novo (a janela Win32 sem som fica como reserva) |

### Bugs do recompilador encontrados rodando o jogo (todos com teste de regressão)

* Retornos condicionais (`ldreq pc`, `popgt {..., pc}`) cortavam o código seguinte (`t_condret`).
* `pop {r3}; bx r3` era tratado como salto para outra função (`t_bxret`).
* Chamadas no estilo ARMv4 (`mov lr, pc` / `add lr, pc, #n` antes de `bx`, `mov pc` ou `add pc`)
  — o decodificador de vídeo em assembly usa muito (`t_v4call`).
* A mesma ideia com desvio direto: `add lr, pc, #n; bne sub-rotina` (`t_v4b`). Antes, a sub-rotina era
  "colada" dentro da função e o decodificador perdia o sincronismo do fluxo de bits.
* Chamada de cauda que volta por um `lr` empilhado antes (`mov pc, r4` para a IDCT, que termina com
  `pop {..., pc}`): a função alvo agora herda o retorno esperado de quem saltou (`c->tret`), em vez de
  executar a continuação duas vezes (`t_tailret`).

Na GPU e nos serviços, nesta etapa: endereço dos vértices calculado no espaço físico (antes do
`p2v`), sentido do descarte de faces (0x40) invertido, e o bloco de configuração 0x00050005 (câmera
estéreo) vazio, que fazia a matriz de projeção virar NaN e sumir com todo o 3D.

Na etapa 7: a GPU não seguia os "saltos" das listas de comandos (CMDBUF_JUMP, registradores 0x23C/0x23D),
que os modelos 3D usam para encadear as listas prontas guardadas nos arquivos do jogo. Sem isso o
professor, os Pokémon e o logo animado simplesmente não eram desenhados. Depuração: `PICA_JUMPLOG=1`,
`PICA_DUMPV=n1,n2` (estatística dos vértices dos desenhos com n vértices).

Também na etapa 7: a transferência de tela com redução 2x2 (usada para "fotografar" a tela nas transições)
escrevia com o dobro da largura e passava do buffer do jogo, corrompendo um objeto vizinho — o jogo fechava ao
sair da introdução para o quarto do personagem. E o som: quando o kernel atrasava, quadros do DSP eram pulados
(buracos no áudio); agora os atrasados são recuperados.

Na etapa 6: o `GetFormatInfo` do save devolvia valores fixos em vez dos que o jogo pediu ao formatar;
a partir da segunda abertura o jogo dizia "The saved game data is corrupted". Agora os parâmetros
ficam em `save/savedata.formato` (saves criados antes desta etapa: apague a pasta `save`).

### Ferramentas de depuração

* `PROBES=10d71c,111bb8 python3 tools/recomp_game.py ...` — insere sondas que imprimem os registradores
  nesses endereços (`PROBE_MEM=r0` mostra a memória apontada, `PROBE_MAX=n` limita).
  `DUMP_RANGES="08000000-09000000,..." DUMP_AT=n` grava a memória na sonda n.
* `python3 tools/lockstep2.py out/p.log 08000000=out/mem_08000000.bin ...` — compara o código recompilado
  com um **interpretador ARM de referência** (`tools/armi.py`, escrito à parte) sonda a sonda.
* `WATCHW=0fffff70` — vigia de escrita exato (página protegida + passo único), com a pilha de funções.
* `python3 tools/dis.py <endereço> [n] [thumb]` — desmonta a imagem linkada.
* `python3 tools/profile.py <pid>` — perfil por amostragem (gdb) da thread ativa.
* Alvos indiretos desconhecidos vão para `out/extra_funcs.txt` e viram sementes na próxima geração.
* `SHOT=shots/f SHOT_EVERY=60` — salva as duas telas em PPM.
* GPU: `PICA_PERFIL=1` (a cada 300 quadros, % do tempo em vértices, desenho, GPU->memória, texturas), `GL_DEBUG_FRAME=n`, `RENDER=software` (desliga o OpenGL), `PICA_THREADS=n` (threads de rasterização; padrão = núcleos), `PICA_STATS=n`, `PICA_DRAWLOG=1`, `PICA_DUMPTEX=1`, `PICA_DEBUG=1`, `PICA_OFF=1`.
* Vídeo: `Y2R_LOG=1`, `Y2R_DUMP=30` (salva o plano Y de 1 em cada 30 quadros).
* `kill -USR1 <pid>` — mostra o estado de cada thread (o que cada uma espera).
* `THREADS_LOG=1` — registra a criação de threads (prioridade e núcleo).
* `NANCHECK=1` na geração do C: registra onde um NaN nasce nas contas de ponto flutuante do jogo.
* `PICA_NANLOG=1`, `PICA_VSTRACE=quadro` (+ `PICA_VSTRACE_N=nº de vértices`): uniforms com NaN e rastreio do shader.
* `ERRF_CONTINUA=1` — não encerra no erro fatal do jogo (`err:f`), que agora é impresso com a pilha.

### Variáveis de execução

* `ROMFS=/caminho/romfs` — pasta romfs extraída do seu cartucho.
* `TECLAS="300:A,306:-,450:A,456:-"` — roteiro de botões (quadro:botões; `-` solta; `T160x120` toca a tela).
* `IDIOMA_3DS` (padrão 1 = inglês), `NOME_3DS`, `SAVE_DIR` (padrão `save/`).

### Como funciona

1. `tools/cro.py` lê o formato CRO0 (segmentos, exports, imports, relocações).
2. `tools/link.py` coloca o `code.bin` em `0x00100000` e cada CRO a partir de `0x40000000`.
   Em tempo de execução, o `ldr:ro` (`runtime/ldr.c`) copia o CRO para onde o jogo pede e aponta a
   tabela de segmentos para o código já recompilado.
3. `tools/funcs.py` descobre funções; `tools/recomp_game.py` itera até nenhuma função nova aparecer.
4. `tools/recomp.py` traduz cada função para `void F_xxxxxxxx(Ctx *c)` (ou `T_` para Thumb).
5. `runtime/`: `runtime.c` (carregador, chamadas indiretas, depuração), `kernel.c` (threads),
   `services.c` (srv, APT, hid, fs, cfg, dsp), `gsp.c` (filas, telas), `pica*.c` (GPU),
   `input.c` (botões/toque), `y2r.c` (YUV→RGB), `ldr.c` (módulos).

### Como jogar no Windows

1. Descompacte esta pasta num lugar com uns 3 GB livres (o código gerado e compilado fica aqui).
2. Abra o **`XY Project.exe`** (o lançador). Nele:
   - **ROM do jogo**: clique em *Procurar…* e escolha o seu `.3ds` **decifrado** (ou arraste o arquivo
     para a janela, ou para cima do `XY Project.exe`);
   - **Resolução interna**: 1x (original, 400×240) a 4x (1600×960) — dá para trocar no jogo com F8;
   - **Filtro de textura**: original (como no 3DS), suave (bilinear), anisotrópico 16x ou bicúbico +
     anisotrópico 16x — dá para trocar no jogo com F7;
   - **Renderizador de vídeo**: OpenGL ou Vulkan (experimental; se não funcionar na placa, o jogo volta ao OpenGL);
   - **Idioma do programa**: Português, English, Español ou 日本語 — escolhido na primeira vez que o lançador abre
     (botão *Idioma* para trocar; no jogo, *Opções → Idioma do programa*, na hora). Vale para o lançador, as
     mensagens da preparação, o menu do jogo, os avisos na tela e a barra de título; não muda o idioma do jogo;
   - **JOGAR**.
3. Na primeira vez o lançador baixa, só dentro da pasta `ferramentas`, um Python portátil (python.org), o
   compilador llvm-mingw e a SDL2 (GitHub), lê o dump, recompila o código para C e compila (de 10 a 40
   minutos, conforme o PC), mostrando o progresso. Das próximas vezes o jogo abre na hora. Enquanto o jogo
   está aberto o lançador fica minimizado e volta quando o jogo fecha; se algo der errado, os detalhes
   aparecem na janela dele.

As escolhas ficam em `lancador.ini`. O `jogar.bat` continua funcionando do jeito antigo (janela de texto) e
usa a mesma ROM. O código do lançador está em `lancador/` (C puro com a API do Windows; o
`lancador/compilar.sh` refaz o `.exe` com o llvm-mingw).

Controles (teclado no padrão do Citra/Azahar; controles de Xbox, PlayStation e Switch pelo mapeamento da SDL, e outros
(DirectInput) por um mapeamento padrão — o `jogo.log` mostra o nome do controle conectado):

| 3DS | Teclado | Controle (Xbox e compatíveis) |
|---|---|---|
| Direcional analógico | setas | analógico esquerdo |
| Direcional digital | T F G H | direcional |
| A / B / X / Y | A / S / Z / X | botões nas posições do 3DS (o da direita é o A); no lançador dá para usar os rótulos (A do Xbox = A) |
| L / R | Q / W | LB / RB |
| START / SELECT | Enter / Backspace | Start / Back |
| Tela de toque | mouse (botão esquerdo) na tela de baixo | analógico direito move um cursor na tela de baixo; o clique dele (R3) toca e, segurado, arrasta |
| Tela cheia | F11 | — |
| Mostrar/esconder a tela de baixo | Tab | clique do analógico esquerdo (L3) |
| Filtro de textura (original, suave, anisotrópico, bicúbico) | F7 | — |
| Resolução interna (1x–4x) | F8 | — |
| Layout (tela de cima cheia / telas empilhadas) | F9 | — |
| Acelerar o jogo (segurar; 2x, 3x ou 4x) | Espaço | RT (gatilho direito) |
| Captura de tela | F12 | — |
| Patins (liga/desliga) | P | LT (gatilho esquerdo) |

As teclas e os botões do controle podem ser trocados no menu do jogo (**Controles**): escolha a ação, a coluna
(teclado ou controle) e aperte Enter (ou clique); depois aperte a tecla ou o botão novo (Esc cancela). Uma tecla
ou botão usado em outra ação sai de lá. Na mesma página ficam o esquema dos botões (por posição ou por rótulo), a
velocidade ao acelerar e *Restaurar o padrão*. As setas do teclado continuam sendo o Circle Pad. As escolhas ficam
no `opcoes.txt`.

**Acelerar**: enquanto a tecla/botão está segurado, o jogo inteiro anda mais rápido (o relógio do jogo também),
sem som; um aviso "» 2x" aparece no canto. Útil para andar e treinar; num PC fraco o ganho fica limitado pelo
que o processador aguenta. **Mostrar FPS** (em *Opções*) põe os quadros por segundo no mesmo canto.

**Patins** (P ou LT): desligados, o Circle Pad (analógico e setas) passa a mover o personagem como o direcional,
andando (ou correndo, segurando B) em vez de patinar; ligados, tudo volta ao normal do jogo. Um ícone de patins
aparece no canto por alguns segundos ao ligar, e o mesmo ícone com um X ao desligar. A escolha fica salva. Para usar o ícone do
próprio jogo, salve uma imagem dele (um print seu, PNG) como `icones/patins.png` na pasta do projeto; o fundo liso
em volta é tirado sozinho. Sem esse arquivo aparece um ícone genérico. Nenhuma imagem vem com o projeto; ao
compartilhar o projeto, não inclua a pasta `icones`.

**Capturas de tela** (F12): a tela de cima na resolução interna escolhida (2x, 4K...) e, se a de baixo estiver
aparecendo, ela embaixo, num PNG em `capturas/` na pasta do projeto.

Controles que a SDL não conhece ganham um **mapeamento automático** ao serem plugados (a partir dos botões,
eixos e direcionais que informam; o aviso diz "mapeado automaticamente"). Antes disso o jogo consulta o banco de
controles da comunidade da SDL (`ferramentas/gamecontrollerdb.txt`, baixado pelo `jogar.py`/`instalar.ps1`,
licença zlib). Para corrigir um controle à mão, crie um `controles.txt` na pasta do projeto com linhas no formato
da SDL (as mesmas do `gamecontrollerdb.txt`). O `jogo.log` lista os controles vistos e o mapeamento usado.

**Arte de fundo do menu** (opcional): coloque uma imagem sua em `fundos/fundo_x.jpg` e/ou `fundos/fundo_y.jpg`
(ou `.png`) na pasta do projeto. Ela aparece ao fundo do menu do jogo da versão correspondente enquanto a
imagem do jogo ainda não está na tela (a primeira abertura, a tela de idioma do jogo, carregamentos). Nenhuma
imagem vem com o projeto; ao compartilhar o projeto, não inclua a pasta `fundos`. A arte fica animada: a imagem se aproxima
e desliza devagar, com partículas de luz subindo e um brilho suave na cor da versão (`MENU_ANIMADO=0` deixa parada).

**Texturas personalizadas** (pacotes no formato do Citra/Azahar, os que trazem PNG com nomes como
`tex1_256x256_24FD4EDB55375DB2_13_mip0.png`): copie o conteúdo do pacote para `texturas/<id do jogo>/`
(`texturas/0004000000055D00` no X, `texturas/0004000000055E00` no Y; a pasta é criada na primeira vez que o jogo
abre; subpastas valem) ou direto em `texturas/`. Liga e desliga no menu do jogo (*Opções → Texturas
personalizadas*), na hora. Vale para pacotes sem `pack.json` (o hash antigo do Citra), que são a maioria dos de
Pokémon X/Y. Nenhum pacote vem com o projeto: use os que você baixou. O `jogo.log` diz quantas texturas achou.

Os saves ficam em `jogos/<id>/save` e o log em `jogos/<id>/jogo.log`. Cada vez que o jogo abre, uma cópia do
save vai para `jogos/<id>/backups/save_<data>.zip` (ficam as 5 últimas); para voltar a uma, feche o jogo, apague
a pasta `save` e extraia o zip na pasta `jogos/<id>`.

Se o jogo fechar com um erro, um **relatório** é salvo em `jogos/<id>/erros/relatorio_<data>.txt` (o tipo do
erro, onde aconteceu, a thread e os registradores do jogo, os módulos carregados e o fim do log); o *VER
DETALHES* do lançador abre o relatório. É o arquivo para mandar junto ao relatar o problema.
Ao atualizar este projeto (descompactar a versão nova por cima), o `jogar.bat` percebe e recompila só o runtime.
Para refazer tudo, apague a pasta `jogos/<id>`.
Para trocar de jogo, escolha outra ROM no lançador (ou apague o `jogo.txt`, se usar o `jogar.bat`).

### Como rodar no Linux

Precisa de `python3`, `clang`/`clang++` e, para ter janela e som, da SDL2 de desenvolvimento
(`sudo apt install libsdl2-dev`): `./jogar.sh caminho/do/jogo.3ds`. Sem SDL2 roda sem janela
(`SHOT=pasta/s SHOT_EVERY=60` para capturas; botões por `TECLAS=...` ou `teclas_agora.txt` em `jogos/<id>/`;
`AUDIO_WAV=som.raw` grava o som cru, estéreo 16 bits a 32728 Hz).

Modo de desenvolvimento (pastas já extraídas): `CODE=in/code.bin ROMFS=romfs ./rebuild.sh`.
Testes: `python3 test/run_tests.py` e `python3 test/make_test_3ds.py` (monta um .3ds de teste).

### Próximos passos

1. Medir o OpenGL no Windows (fps da barra de título) e tirar os gargalos que sobrarem na CPU
   (cópia framebuffer memória <-> GPU a cada lista, decodificação de texturas, shader de vértice).
2. Conferir o fps fora de casa depois da 7.8; o fundo das batalhas na cidade que não aparece na abertura da batalha.
3. Vulkan: primeira versão pronta (opção no lançador); medir e comparar com o OpenGL.

### Licenças de terceiros

* `runtime/audio/` — HLE de áudio do Azahar/Citra, GPL-2.0 ou posterior (`runtime/audio/LICENSE.txt`).
* `runtime/faad2/` — decodificador AAC faad2, GPL-2.0 ou posterior (`runtime/faad2/COPYING`).
* SDL2 (baixada pelo `instalar.ps1`), licença zlib.
* `runtime/stb_image.h` — leitor de PNG/JPEG de Sean Barrett, domínio público (ou MIT, à escolha).
* `runtime/stb_image_write.h` — gravador de PNG (capturas de tela) de Sean Barrett, domínio público (ou MIT, à escolha).
* `runtime/texpack.c` — inclui o CityHash64 da Google (licença MIT, aviso no arquivo).
* SDL_GameControllerDB (`ferramentas/gamecontrollerdb.txt`, baixado na primeira execução), licença zlib.
* `runtime/fonte_aviso.h` — bitmap gerado (`tools/gerar_fonte.py`) a partir da fonte DejaVu Sans Bold, licença das fontes DejaVu (livre, derivada da Bitstream Vera), e da Noto Sans CJK JP (japonês), SIL Open Font License 1.1.
* `runtime/fonte_menu.h` — bitmap gerado (`tools/gerar_fonte_menu.py`) a partir da Inter e da Noto Sans CJK JP (japonês), as duas SIL Open Font License 1.1.
Por incluir código GPL, o executável gerado também fica sob a GPL; nada disso inclui dados do jogo.

---

## English

This project turns the machine code of Pokémon X and Pokémon Y (the 3DS's ARM11) into **native C**
that compiles and runs on the PC, with no CPU emulator. No game files are included:
everyone uses a dump of their own cartridge.

### Current state

| Stage | Status |
|---|---|
| CRO/CRS module parser (110 `.cro` modules + `static.crs`) | done |
| Static linker: `code.bin` + every CRO in a single image | done (CROs at `0x40000000`) |
| Function discovery (BL/BLX, vtables, literals, relative tables, CRO exports, ARMv4-style calls, ADR) | ~74,150 functions |
| ARMv6K + VFPv2 + ARMv6 SIMD + Thumb-1 → C recompiler | 3.0 million instructions, 99.1% translated |
| Differential tests (ARM and Thumb, -O1/-O2/-Os/-O3) | 83/83 ARM |
| HLE kernel: threads, single-core scheduler with deadline preemption, events, mutexes, timers, arbiter | done |
| Services: srv:, APT, hid, cfg, fs (RomFS + saves in PC folders), dsp (protocol), gsp, ldr:ro, y2r | good enough |
| **Software PICA200 GPU**: command lists, vertex shaders, rasterization, textures (14 formats + ETC1), TEV, blending, depth/stencil, **per-pixel lighting** (8 lights, LUTs, normal maps) | done |
| Input: keyboard, mouse (touch) and XInput controller in the Windows window; `TECLAS` script and `teclas_agora.txt` | done |
| Mobiclip video (`.moflex`): the game's own decoder + `y2r` YUV→RGB conversion | runs |
| Language selection → "Begin game" → full opening video (Xerneas, "Presented by Game Freak") → A → introduction (`DllIntro`) | done |
| Professor Sycamore's 3D introduction: scenery, lit Poké Ball, Pokémon frames, dialogue, questions and choosing the character's look | **gets here** |
| Full introduction up to the character's room (first map, PSS menu on the bottom screen) | stage 7.5 |
| 3D character models (professor, Gogoat, the player character in the mirrors) and the "X" logo animation | **appear** (the jump between GPU command lists, registers 0x238-0x23D, was missing) |
| Performance: rasterization on all cores + vertex cache (~1.6x on a 2-core PC; more with more cores) | stage 6.1 |
| Reads the decrypted `.3ds` directly + automatic recompilation on the first run | done |
| Windows: `jogar.bat` downloads portable Python + llvm-mingw + SDL2, compiles and opens the window | **works** (tested by the user) |
| **GPU on the graphics card (OpenGL 3.3)**: rasterization, textures, lighting, TEV, blending, depth/stencil in one shader; vertices and clipping stay on the CPU | stage 7 (software remains as a fallback: `RENDER=software`) |
| OpenGL: framebuffers stay on the GPU between lists and frames (they only go back to memory when the CPU needs to read them); screen clears become glClear; vertex shader on all cores | stage 7.1 |
| Asynchronous GPU: GX commands run on their own thread in parallel with the game (as on the 3DS); PICA lists copied on submission | stage 7.3 (`GPU_SINCRONA=1` turns it off) |
| PICA geometry shader (particles, sparkles, light rays, move effects) | stage 7.7 (`PICA_SEM_GS=1` turns it off) |
| Vertex shader ~6-9x faster: instructions decoded once per program and run in batches of 16 vertices (SIMD), with a mask for divergent branches; per-draw attribute read plan; cached getenv; thread pool with dynamic block distribution (doesn't wait for threads that wake up late) | stage 7.8 (`PICA_VSCHECK=1` checks the batch against the scalar interpreter) |
| Trainer at the start of battles: honors the color/depth write lock (0x113/0x115); depth+stencil read as a texture even when it is the draw's own buffer | fixed in 7.9 |
| Kernel: CloseHandle frees handles (they used to run out after ~25 min of play) | fixed in 7.9 |
| GPU: projective textures (tc0/tc0w), mipmaps with the filter/bias/levels of 0x84, fog with the 0xE6-0xEF table | 7.9 |
| Linear->tiled display transfer (and flip / tiled->tiled): mosaic at the end of the Fletchling scene; depth read as color with float24 precision (dashed outlines) | 7.9 |
| Internal resolution 1x-4x (F8 or RESOLUCAO=n): GPU surfaces scaled, down/upscaled by blit when going to/from memory, top screen shown scaled in the window | 8.0 |
| Layout: top screen in 16:9, bottom screen on demand in the corner (Tab), the bag opens in the corner | 8.0 |
| Sound: the DSP waits for the game to handle the previous frame (crackles) and the output queue adjusts itself (45-150 ms) | 8.1 |
| Windows: precise kernel waits (high-resolution timer + event, high-precision counter); they used to wake up to 17 ms late and the game missed the VBlank (fps wobbling below 30) | 8.1 |
| `XY Project.exe` launcher (ROM, resolution, texture filter, controller buttons, language, PLAY); controller: touch cursor on the right stick (R3 touches, L3 shows the bottom screen), buttons by position or label, generic DirectInput controllers, on-screen notice when connecting/disconnecting, and texture filters in the renderer (forced bilinear, 16x anisotropic, Catmull-Rom bicubic; F7) | 8.2 |
| **Pokémon Y** runs with the same program (the launcher and `jogar.py` recompile either version; the window shows "Pokémon X" or "Pokémon Y"); project renamed to Pokémon XY | 8.2 |
| Bottom screen also in high resolution (before, only the top one was shown scaled); Pokémon XY icon on the game window and taskbar; game threads at above-normal priority on Windows (SEM_PRIORIDADE=1 turns it off) | 8.3 |
| **60 fps** (on hold: not in the launcher; only for testing with `FPS=60 FPS60_TESTE=1`): game logic still runs at 30 steps per second; every frame, the OpenGL renderer redraws the frame with each vertex halfway between the previous and current frame (on copies of the surfaces, without touching game memory) and the window shows the in-between and the real frame, one per VBlank. Scene or camera change: the frame just repeats. Costs ~1 VBlank (17 ms) of latency and twice the graphics card work. [60fps] line in the profile | 8.3 |
| **Vulkan 1.3** ("Video renderer" option in the launcher, `RENDER=vulkan`): same work as OpenGL (shaders ported in `runtime/vk/`, embedded precompiled in `vk_shaders.h` by `vk/gerar.sh`), all off-screen, with command batches submitted at the end of each list and asynchronous screen readbacks. If Vulkan doesn't open (no 1.3 card, old driver), it falls back to OpenGL on its own | 8.4 |
| More complete function search on the first compilation (ARM/Thumb prologues and pointers to Thumb code in literal pools): +1,900 functions, including those called only through pointers built at run time (crash at the Pokémon Center). **Recompile everything** button in the launcher (rebuilds the game from the ROM; the save stays) | 8.3 |
| Character outlines disappearing after battles (cached framebuffer tied to a deleted texture) | fixed in 8.1 |
| Performance: triangle clipping with trivial accept and no copies; asynchronous GPU screen copy (PBO + fence) without holding the game; the window only shows a screen whose copy is done and never goes back to an older frame (GL_LEITURA_SINCRONA=1 goes back to the immediate copy); [ritmo] line in the profile | 8.1 |
| Sound: ring buffer with fine speed adjustment (no pops when the queue empties or fills) | 7.9 |
| ETC1 textures: individual mode with the sub-block colors swapped (jagged outlines) | fixed in 7.6 |
| Title bar with fps (frames drawn per second), speed (100% = 60 VBlanks/s) and the renderer in use | stage 7 |
| **Sound**: HLE DSP (mixing of the 24 voices, filters, ADPCM/PCM, from Azahar) + AAC music via faad2 | new in this stage |
| **SDL2 window** (Windows and Linux): resizable, fullscreen (F11), keyboard, controllers, mouse as touch, sound | new (the Win32 window without sound remains as a fallback) |

### Recompiler bugs found while running the game (all with a regression test)

* Conditional returns (`ldreq pc`, `popgt {..., pc}`) cut off the following code (`t_condret`).
* `pop {r3}; bx r3` was treated as a jump to another function (`t_bxret`).
* ARMv4-style calls (`mov lr, pc` / `add lr, pc, #n` before `bx`, `mov pc` or `add pc`)
  — the assembly video decoder uses them a lot (`t_v4call`).
* The same idea with a direct branch: `add lr, pc, #n; bne subroutine` (`t_v4b`). Before, the subroutine was
  "glued" inside the function and the decoder lost sync with the bitstream.
* Tail call that returns through an `lr` pushed earlier (`mov pc, r4` into the IDCT, which ends with
  `pop {..., pc}`): the target function now inherits the return expected by the caller that jumped (`c->tret`),
  instead of running the continuation twice (`t_tailret`).

In the GPU and services, in this stage: vertex address computed in physical space (before
`p2v`), face culling direction (0x40) inverted, and the empty 0x00050005 config block (stereo
camera), which turned the projection matrix into NaN and made all 3D disappear.

In stage 7: the GPU didn't follow the "jumps" in the command lists (CMDBUF_JUMP, registers 0x23C/0x23D),
which the 3D models use to chain the prebuilt lists stored in the game files. Without that, the
professor, the Pokémon and the animated logo simply weren't drawn. Debugging: `PICA_JUMPLOG=1`,
`PICA_DUMPV=n1,n2` (vertex statistics for draws with n vertices).

Also in stage 7: the display transfer with 2x2 downscaling (used to "photograph" the screen in transitions)
wrote at twice the width and overran the game's buffer, corrupting a neighboring object — the game closed when
leaving the introduction for the character's room. And sound: when the kernel ran late, DSP frames were skipped
(gaps in the audio); late ones are now caught up.

In stage 6: the save's `GetFormatInfo` returned fixed values instead of the ones the game asked for when formatting;
from the second launch on the game said "The saved game data is corrupted". The parameters now
live in `save/savedata.formato` (saves created before this stage: delete the `save` folder).

### Debugging tools

* `PROBES=10d71c,111bb8 python3 tools/recomp_game.py ...` — inserts probes that print the registers
  at those addresses (`PROBE_MEM=r0` shows the memory pointed to, `PROBE_MAX=n` limits).
  `DUMP_RANGES="08000000-09000000,..." DUMP_AT=n` dumps memory at probe n.
* `python3 tools/lockstep2.py out/p.log 08000000=out/mem_08000000.bin ...` — compares the recompiled code
  with a **reference ARM interpreter** (`tools/armi.py`, written separately) probe by probe.
* `WATCHW=0fffff70` — exact write watch (protected page + single step), with the function stack.
* `python3 tools/dis.py <address> [n] [thumb]` — disassembles the linked image.
* `python3 tools/profile.py <pid>` — sampling profile (gdb) of the active thread.
* Unknown indirect targets go to `out/extra_funcs.txt` and become seeds for the next generation.
* `SHOT=shots/f SHOT_EVERY=60` — saves both screens as PPM.
* GPU: `PICA_PERFIL=1` (every 300 frames, % of time in vertices, drawing, GPU->memory, textures), `GL_DEBUG_FRAME=n`, `RENDER=software` (turns OpenGL off), `PICA_THREADS=n` (rasterization threads; default = cores), `PICA_STATS=n`, `PICA_DRAWLOG=1`, `PICA_DUMPTEX=1`, `PICA_DEBUG=1`, `PICA_OFF=1`.
* Video: `Y2R_LOG=1`, `Y2R_DUMP=30` (saves the Y plane of 1 in every 30 frames).
* `kill -USR1 <pid>` — shows the state of each thread (what each one is waiting for).
* `THREADS_LOG=1` — logs thread creation (priority and core).
* `NANCHECK=1` when generating the C: logs where a NaN is born in the game's floating-point math.
* `PICA_NANLOG=1`, `PICA_VSTRACE=frame` (+ `PICA_VSTRACE_N=number of vertices`): uniforms with NaN and shader tracing.
* `ERRF_CONTINUA=1` — doesn't exit on the game's fatal error (`err:f`), which is now printed with the stack.

### Run-time variables

* `ROMFS=/path/romfs` — romfs folder extracted from your cartridge.
* `TECLAS="300:A,306:-,450:A,456:-"` — button script (frame:buttons; `-` releases; `T160x120` touches the screen).
* `IDIOMA_3DS` (default 1 = English), `NOME_3DS`, `SAVE_DIR` (default `save/`).

### How it works

1. `tools/cro.py` reads the CRO0 format (segments, exports, imports, relocations).
2. `tools/link.py` places `code.bin` at `0x00100000` and each CRO from `0x40000000` on.
   At run time, `ldr:ro` (`runtime/ldr.c`) copies the CRO to where the game asks and points the
   segment table at the already-recompiled code.
3. `tools/funcs.py` discovers functions; `tools/recomp_game.py` iterates until no new function shows up.
4. `tools/recomp.py` translates each function into `void F_xxxxxxxx(Ctx *c)` (or `T_` for Thumb).
5. `runtime/`: `runtime.c` (loader, indirect calls, debugging), `kernel.c` (threads),
   `services.c` (srv, APT, hid, fs, cfg, dsp), `gsp.c` (queues, screens), `pica*.c` (GPU),
   `input.c` (buttons/touch), `y2r.c` (YUV→RGB), `ldr.c` (modules).

### How to play on Windows

1. Unzip this folder somewhere with about 3 GB free (the generated and compiled code lives here).
2. Open **`XY Project.exe`** (the launcher). In it:
   - **Game ROM**: click *Browse…* and choose your **decrypted** `.3ds` (or drag the file
     onto the window, or onto `XY Project.exe`);
   - **Internal resolution**: 1x (original, 400×240) to 4x (1600×960) — can be changed in game with F8;
   - **Texture filter**: original (as on the 3DS), smooth (bilinear), 16x anisotropic or bicubic +
     16x anisotropic — can be changed in game with F7;
   - **Video renderer**: OpenGL or Vulkan (experimental; if it doesn't work on your card, the game falls back to OpenGL);
   - **Program language**: Português, English, Español or 日本語 — chosen the first time the launcher opens
     (*Language* button to change it; in game, *Settings → Program language*, instantly). It applies to the launcher, the
     setup messages, the in-game menu, on-screen notices and the title bar; it doesn't change the game's language;
   - **PLAY**.
3. The first time, the launcher downloads, only inside the `ferramentas` folder, a portable Python (python.org), the
   llvm-mingw compiler and SDL2 (GitHub), reads the dump, recompiles the code to C and compiles it (10 to 40
   minutes, depending on the PC), showing the progress. After that the game opens right away. While the game
   is open the launcher stays minimized and comes back when the game closes; if something goes wrong, the details
   show up in its window.

Choices are stored in `lancador.ini`. `jogar.bat` still works the old way (text window) and
uses the same ROM. The launcher's code is in `lancador/` (plain C with the Windows API;
`lancador/compilar.sh` rebuilds the `.exe` with llvm-mingw).

Controls (keyboard in the Citra/Azahar layout; Xbox, PlayStation and Switch controllers through SDL's mapping, and others
(DirectInput) through a default mapping — `jogo.log` shows the name of the connected controller):

| 3DS | Keyboard | Controller (Xbox and compatible) |
|---|---|---|
| Circle Pad | arrow keys | left stick |
| D-Pad | T F G H | D-pad |
| A / B / X / Y | A / S / Z / X | buttons in the 3DS positions (the right one is A); the launcher can use the labels instead (Xbox A = A) |
| L / R | Q / W | LB / RB |
| START / SELECT | Enter / Backspace | Start / Back |
| Touch screen | mouse (left button) on the bottom screen | right stick moves a cursor on the bottom screen; its click (R3) touches and, held, drags |
| Fullscreen | F11 | — |
| Show/hide the bottom screen | Tab | left stick click (L3) |
| Texture filter (original, smooth, anisotropic, bicubic) | F7 | — |
| Internal resolution (1x–4x) | F8 | — |
| Layout (full top screen / stacked screens) | F9 | — |
| Fast-forward (hold; 2x, 3x or 4x) | Space | RT (right trigger) |
| Screenshot | F12 | — |
| Roller Skates (on/off) | P | LT (left trigger) |

Keys and controller buttons can be changed in the in-game menu (**Controls**): choose the action, the column
(keyboard or controller) and press Enter (or click); then press the new key or button (Esc cancels). A key
or button used by another action is removed from it. The same page has the button layout (by position or by label), the
fast-forward speed and *Restore defaults*. The keyboard arrows remain the Circle Pad. Choices are saved
in `opcoes.txt`.

**Fast-forward**: while the key/button is held, the whole game runs faster (the game clock too),
without sound; a "» 2x" notice shows up in the corner. Handy for walking and training; on a weak PC the gain is limited by
what the processor can handle. **Show FPS** (in *Settings*) puts the frames per second in the same corner.

**Roller Skates** (P or LT): when off, the Circle Pad (stick and arrows) moves the character like the D-pad,
walking (or running, holding B) instead of skating; when on, everything is back to the game's normal behavior. A skates icon
shows up in the corner for a few seconds when turning them on, and the same icon with an X when turning them off. The choice is saved. To use the
game's own icon, save an image of it (your own screenshot, PNG) as `icones/patins.png` in the project folder; the flat background
around it is removed automatically. Without that file a generic icon is shown. No image comes with the project; when
sharing the project, don't include the `icones` folder.

**Screenshots** (F12): the top screen at the chosen internal resolution (2x, 4K...) and, if the bottom one is
showing, it below, as a PNG in `capturas/` in the project folder.

Controllers SDL doesn't know get an **automatic mapping** when plugged in (from the buttons,
axes and hats they report; the notice says "mapped automatically"). Before that the game checks SDL's community
controller database (`ferramentas/gamecontrollerdb.txt`, downloaded by `jogar.py`/`instalar.ps1`,
zlib license). To fix a controller by hand, create a `controles.txt` in the project folder with lines in SDL's
format (the same as `gamecontrollerdb.txt`). `jogo.log` lists the controllers seen and the mapping used.

**Menu background art** (optional): put an image of your own at `fundos/fundo_x.jpg` and/or `fundos/fundo_y.jpg`
(or `.png`) in the project folder. It shows behind the in-game menu of the matching version while the
game's picture isn't on screen yet (the first launch, the game's language screen, loading). No
image comes with the project; when sharing the project, don't include the `fundos` folder. The art is animated: the image slowly
zooms and drifts, with light particles rising and a soft glow in the version's color (`MENU_ANIMADO=0` keeps it still).

**Custom textures** (packs in the Citra/Azahar format, the ones with PNGs named like
`tex1_256x256_24FD4EDB55375DB2_13_mip0.png`): copy the pack's contents to `texturas/<game id>/`
(`texturas/0004000000055D00` for X, `texturas/0004000000055E00` for Y; the folder is created the first time the game
opens; subfolders work) or straight into `texturas/`. Turn it on and off in the in-game menu (*Settings → Custom
textures*), instantly. Works for packs without `pack.json` (Citra's old hash), which are most of the
Pokémon X/Y ones. No pack comes with the project: use the ones you downloaded. `jogo.log` says how many textures it found.

Saves are in `jogos/<id>/save` and the log in `jogos/<id>/jogo.log`. Every time the game opens, a copy of the
save goes to `jogos/<id>/backups/save_<date>.zip` (the last 5 are kept); to go back to one, close the game, delete
the `save` folder and extract the zip into the `jogos/<id>` folder.

If the game closes with an error, a **report** is saved to `jogos/<id>/erros/relatorio_<date>.txt` (the kind of
error, where it happened, the game's thread and registers, the loaded modules and the end of the log); the launcher's
*SHOW DETAILS* opens the report. That's the file to send along when reporting the problem.
When updating this project (unzipping the new version over it), `jogar.bat` notices and recompiles only the runtime.
To rebuild everything, delete the `jogos/<id>` folder.
To switch games, choose another ROM in the launcher (or delete `jogo.txt`, if you use `jogar.bat`).

### How to run on Linux

You need `python3`, `clang`/`clang++` and, to get a window and sound, the SDL2 development package
(`sudo apt install libsdl2-dev`): `./jogar.sh path/to/game.3ds`. Without SDL2 it runs with no window
(`SHOT=folder/s SHOT_EVERY=60` for screenshots; buttons via `TECLAS=...` or `teclas_agora.txt` in `jogos/<id>/`;
`AUDIO_WAV=som.raw` records the raw sound, 16-bit stereo at 32728 Hz).

Development mode (folders already extracted): `CODE=in/code.bin ROMFS=romfs ./rebuild.sh`.
Tests: `python3 test/run_tests.py` and `python3 test/make_test_3ds.py` (builds a test .3ds).

### Next steps

1. Measure OpenGL on Windows (fps in the title bar) and remove the remaining CPU bottlenecks
   (memory <-> GPU framebuffer copy on every list, texture decoding, vertex shader).
2. Check the fps outdoors after 7.8; the battle background in the city that doesn't show at the start of the battle.
3. Vulkan: first version done (option in the launcher); measure and compare with OpenGL.

### Third-party licenses

* `runtime/audio/` — audio HLE from Azahar/Citra, GPL-2.0 or later (`runtime/audio/LICENSE.txt`).
* `runtime/faad2/` — faad2 AAC decoder, GPL-2.0 or later (`runtime/faad2/COPYING`).
* SDL2 (downloaded by `instalar.ps1`), zlib license.
* `runtime/stb_image.h` — PNG/JPEG reader by Sean Barrett, public domain (or MIT, your choice).
* `runtime/stb_image_write.h` — PNG writer (screenshots) by Sean Barrett, public domain (or MIT, your choice).
* `runtime/texpack.c` — includes Google's CityHash64 (MIT license, notice in the file).
* SDL_GameControllerDB (`ferramentas/gamecontrollerdb.txt`, downloaded on the first run), zlib license.
* `runtime/fonte_aviso.h` — bitmap generated (`tools/gerar_fonte.py`) from the DejaVu Sans Bold font, DejaVu fonts license (free, derived from Bitstream Vera), and from Noto Sans CJK JP (Japanese), SIL Open Font License 1.1.
* `runtime/fonte_menu.h` — bitmap generated (`tools/gerar_fonte_menu.py`) from Inter and Noto Sans CJK JP (Japanese), both SIL Open Font License 1.1.
Because it includes GPL code, the generated executable is also under the GPL; none of this includes game data.

---

## Español

Este proyecto convierte el código máquina de Pokémon X y Pokémon Y (el ARM11 de la 3DS) en **C nativo**
que se compila y se ejecuta en el PC, sin emulador de CPU. No se incluye ningún archivo del juego:
cada persona usa el volcado de su propio cartucho.

### Estado actual

| Etapa | Estado |
|---|---|
| Lector de los módulos CRO/CRS (110 módulos `.cro` + `static.crs`) | listo |
| Enlazador estático: `code.bin` + todos los CRO en una sola imagen | listo (CRO en `0x40000000`) |
| Descubrimiento de funciones (BL/BLX, vtables, literales, tablas relativas, exports de los CRO, llamadas al estilo ARMv4, ADR) | ~74.150 funciones |
| Recompilador ARMv6K + VFPv2 + SIMD de ARMv6 + Thumb-1 → C | 3,0 millones de instrucciones, 99,1 % traducidas |
| Pruebas diferenciales (ARM y Thumb, -O1/-O2/-Os/-O3) | 83/83 ARM |
| Kernel HLE: hilos, planificador de un solo núcleo con expropiación por plazo, eventos, mutex, temporizadores, árbitro | listo |
| Servicios: srv:, APT, hid, cfg, fs (RomFS + partidas en carpetas del PC), dsp (protocolo), gsp, ldr:ro, y2r | suficientemente listos |
| **GPU PICA200 por software**: listas de comandos, shaders de vértices, rasterización, texturas (14 formatos + ETC1), TEV, mezcla, profundidad/esténcil, **iluminación por píxel** (8 luces, LUT, mapas de normales) | listo |
| Entrada: teclado, ratón (táctil) y mando XInput en la ventana de Windows; guion `TECLAS` y `teclas_agora.txt` | listo |
| Vídeo Mobiclip (`.moflex`): decodificador del propio juego + conversión YUV→RGB de `y2r` | funciona |
| Selección de idioma → "Begin game" → vídeo de apertura completo (Xerneas, "Presented by Game Freak") → A → introducción (`DllIntro`) | listo |
| Introducción del Profesor Ciprés en 3D: escenario, Poké Ball iluminada, cuadros con Pokémon, diálogos, preguntas y elección del aspecto del personaje | **llega aquí** |
| Introducción completa hasta el cuarto del personaje (primer mapa, menú del PSS en la pantalla inferior) | etapa 7.5 |
| Modelos 3D de los personajes (profesor, Gogoat, el personaje del jugador en los espejos) y animación del logo "X" | **aparecen** (faltaba el salto entre listas de comandos de la GPU, registros 0x238-0x23D) |
| Rendimiento: rasterización en todos los núcleos + caché de vértices (~1,6x en un PC de 2 núcleos; más con más núcleos) | etapa 6.1 |
| Lectura directa del `.3ds` descifrado + recompilación automática en la primera ejecución | listo |
| Windows: `jogar.bat` descarga Python portátil + llvm-mingw + SDL2, compila y abre la ventana | **funciona** (probado por el usuario) |
| **GPU en la tarjeta gráfica (OpenGL 3.3)**: rasterización, texturas, iluminación, TEV, mezcla, profundidad/esténcil en un shader; vértices y recorte siguen en la CPU | etapa 7 (el software queda de reserva: `RENDER=software`) |
| OpenGL: los framebuffers se quedan en la GPU entre listas y cuadros (solo vuelven a la memoria cuando la CPU necesita leerlos); las limpiezas de pantalla pasan a ser glClear; shader de vértices en todos los núcleos | etapa 7.1 |
| GPU asíncrona: los comandos GX corren en su propio hilo en paralelo con el juego (como en la 3DS); listas de la PICA copiadas al enviarse | etapa 7.3 (`GPU_SINCRONA=1` lo desactiva) |
| Shader de geometría de la PICA (partículas, destellos, rayos de luz, efectos de movimientos) | etapa 7.7 (`PICA_SEM_GS=1` lo desactiva) |
| Shader de vértices ~6-9x más rápido: instrucciones decodificadas una vez por programa y ejecutadas en lotes de 16 vértices (SIMD), con máscara para saltos divergentes; plan de lectura de atributos por dibujo; getenv en caché; grupo de hilos con reparto dinámico de bloques (no espera a los hilos que despiertan tarde) | etapa 7.8 (`PICA_VSCHECK=1` compara el lote con el intérprete escalar) |
| Entrenador al empezar los combates: respeta el bloqueo de escritura de color/profundidad (0x113/0x115); profundidad+esténcil leída como textura aunque sea el búfer del propio dibujo | corregido en 7.9 |
| Kernel: CloseHandle libera los handles (antes se agotaban en ~25 min de juego) | corregido en 7.9 |
| GPU: texturas proyectivas (tc0/tc0w), mipmaps con filtro/bias/niveles de 0x84, niebla (fog) con la tabla 0xE6-0xEF | 7.9 |
| Transferencia de pantalla lineal->bloques (y volteo / bloques->bloques): mosaico al final de la escena de Fletchling; profundidad leída como color con la precisión de float24 (contornos discontinuos) | 7.9 |
| Resolución interna 1x-4x (F8 o RESOLUCAO=n): superficies de la GPU a escala, reducción/ampliación por blit al ir/volver de la memoria, pantalla superior mostrada a escala en la ventana | 8.0 |
| Disposición: pantalla superior en 16:9, pantalla inferior a demanda en la esquina (Tab), la mochila se abre en la esquina | 8.0 |
| Sonido: el DSP espera a que el juego atienda el cuadro anterior (chasquidos) y la cola de salida se ajusta sola (45-150 ms) | 8.1 |
| Windows: esperas del kernel precisas (temporizador de alta resolución + evento, contador de alta precisión); antes despertaban hasta 17 ms tarde y el juego perdía el VBlank (fps oscilando por debajo de 30) | 8.1 |
| Lanzador `XY Project.exe` (ROM, resolución, filtro de texturas, botones del mando, idioma, JUGAR); mando: cursor táctil en el stick derecho (R3 toca, L3 muestra la pantalla inferior), botones por posición o por etiqueta, mandos DirectInput genéricos, aviso en pantalla al conectar/desconectar y filtros de textura en el renderizador (bilineal forzado, anisotrópico 16x, bicúbico Catmull-Rom; F7) | 8.2 |
| **Pokémon Y** funciona con el mismo programa (el lanzador y `jogar.py` recompilan cualquiera de las dos versiones; la ventana muestra "Pokémon X" o "Pokémon Y"); proyecto renombrado a Pokémon XY | 8.2 |
| Pantalla inferior también en alta resolución (antes solo la superior se mostraba a escala); icono de Pokémon XY en la ventana y en la barra de tareas del juego; hilos del juego con prioridad superior a la normal en Windows (SEM_PRIORIDADE=1 lo desactiva) | 8.3 |
| **60 fps** (en pausa: fuera del lanzador; solo para pruebas con `FPS=60 FPS60_TESTE=1`): la lógica del juego sigue a 30 pasos por segundo; en cada cuadro, el renderizador OpenGL vuelve a dibujar el cuadro con cada vértice a medio camino entre el cuadro anterior y el actual (en copias de las superficies, sin tocar la memoria del juego) y la ventana muestra el intermedio y el real, uno por VBlank. Cambio de escena o de cámara: el cuadro solo se repite. Cuesta ~1 VBlank (17 ms) de retraso y el doble de trabajo de la tarjeta gráfica. Línea [60fps] en el perfil | 8.3 |
| **Vulkan 1.3** (opción "Renderizador de vídeo" en el lanzador, `RENDER=vulkan`): el mismo trabajo que OpenGL (shaders portados en `runtime/vk/`, incluidos ya compilados en `vk_shaders.h` por `vk/gerar.sh`), todo fuera de pantalla, con lotes de comandos enviados al final de cada lista y lecturas asíncronas de las pantallas. Si Vulkan no se abre (sin tarjeta 1.3, controlador antiguo), vuelve solo a OpenGL | 8.4 |
| Búsqueda de funciones más completa en la primera compilación (prólogos ARM/Thumb y punteros a código Thumb en los literal pools): +1.900 funciones, incluidas las que solo se llaman por punteros montados en tiempo de ejecución (cierre en el Centro Pokémon). Botón **Recompilar todo** en el lanzador (rehace el juego a partir de la ROM; la partida se conserva) | 8.3 |
| Contornos de los personajes que desaparecían tras los combates (framebuffer en caché atado a una textura borrada) | corregido en 8.1 |
| Rendimiento: recorte de triángulos con aceptación directa y sin copias; copia asíncrona de las pantallas de la GPU (PBO + fence) sin detener el juego; la ventana solo muestra una pantalla con la copia terminada y nunca vuelve a un cuadro más viejo (GL_LEITURA_SINCRONA=1 vuelve a la copia inmediata); línea [ritmo] en el perfil | 8.1 |
| Sonido: búfer circular con ajuste fino de velocidad (sin chasquidos cuando la cola se vacía o se llena) | 7.9 |
| Texturas ETC1: modo individual con los colores de los subbloques intercambiados (contornos dentados) | corregido en 7.6 |
| Barra de título con fps (cuadros dibujados por segundo), velocidad (100 % = 60 VBlanks/s) y renderizador en uso | etapa 7 |
| **Sonido**: DSP en HLE (mezcla de las 24 voces, filtros, ADPCM/PCM, de Azahar) + música AAC con faad2 | nuevo en esta etapa |
| **Ventana SDL2** (Windows y Linux): redimensionable, pantalla completa (F11), teclado, mandos, ratón como táctil, sonido | nuevo (la ventana Win32 sin sonido queda de reserva) |

### Errores del recompilador encontrados al ejecutar el juego (todos con prueba de regresión)

* Los retornos condicionales (`ldreq pc`, `popgt {..., pc}`) cortaban el código siguiente (`t_condret`).
* `pop {r3}; bx r3` se trataba como un salto a otra función (`t_bxret`).
* Llamadas al estilo ARMv4 (`mov lr, pc` / `add lr, pc, #n` antes de `bx`, `mov pc` o `add pc`)
  — el decodificador de vídeo en ensamblador las usa mucho (`t_v4call`).
* La misma idea con salto directo: `add lr, pc, #n; bne subrutina` (`t_v4b`). Antes, la subrutina quedaba
  "pegada" dentro de la función y el decodificador perdía la sincronía del flujo de bits.
* Llamada de cola que vuelve por un `lr` apilado antes (`mov pc, r4` hacia la IDCT, que termina con
  `pop {..., pc}`): la función destino ahora hereda el retorno esperado por quien saltó (`c->tret`), en lugar de
  ejecutar la continuación dos veces (`t_tailret`).

En la GPU y los servicios, en esta etapa: dirección de los vértices calculada en el espacio físico (antes de
`p2v`), sentido del descarte de caras (0x40) invertido, y el bloque de configuración 0x00050005 (cámara
estéreo) vacío, que convertía la matriz de proyección en NaN y hacía desaparecer todo el 3D.

En la etapa 7: la GPU no seguía los "saltos" de las listas de comandos (CMDBUF_JUMP, registros 0x23C/0x23D),
que los modelos 3D usan para encadenar las listas ya preparadas guardadas en los archivos del juego. Sin eso, el
profesor, los Pokémon y el logo animado simplemente no se dibujaban. Depuración: `PICA_JUMPLOG=1`,
`PICA_DUMPV=n1,n2` (estadística de los vértices de los dibujos con n vértices).

También en la etapa 7: la transferencia de pantalla con reducción 2x2 (usada para "fotografiar" la pantalla en las transiciones)
escribía con el doble de ancho y se salía del búfer del juego, corrompiendo un objeto vecino: el juego se cerraba al
pasar de la introducción al cuarto del personaje. Y el sonido: cuando el kernel se retrasaba, se saltaban cuadros del DSP
(huecos en el audio); ahora los atrasados se recuperan.

En la etapa 6: el `GetFormatInfo` de la partida devolvía valores fijos en lugar de los que el juego pidió al formatear;
a partir de la segunda apertura el juego decía "The saved game data is corrupted". Ahora los parámetros
quedan en `save/savedata.formato` (partidas creadas antes de esta etapa: borra la carpeta `save`).

### Herramientas de depuración

* `PROBES=10d71c,111bb8 python3 tools/recomp_game.py ...` — inserta sondas que imprimen los registros
  en esas direcciones (`PROBE_MEM=r0` muestra la memoria apuntada, `PROBE_MAX=n` limita).
  `DUMP_RANGES="08000000-09000000,..." DUMP_AT=n` vuelca la memoria en la sonda n.
* `python3 tools/lockstep2.py out/p.log 08000000=out/mem_08000000.bin ...` — compara el código recompilado
  con un **intérprete ARM de referencia** (`tools/armi.py`, escrito aparte) sonda a sonda.
* `WATCHW=0fffff70` — vigilancia de escritura exacta (página protegida + paso a paso), con la pila de funciones.
* `python3 tools/dis.py <dirección> [n] [thumb]` — desensambla la imagen enlazada.
* `python3 tools/profile.py <pid>` — perfil por muestreo (gdb) del hilo activo.
* Los destinos indirectos desconocidos van a `out/extra_funcs.txt` y se vuelven semillas en la siguiente generación.
* `SHOT=shots/f SHOT_EVERY=60` — guarda las dos pantallas en PPM.
* GPU: `PICA_PERFIL=1` (cada 300 cuadros, % del tiempo en vértices, dibujo, GPU->memoria, texturas), `GL_DEBUG_FRAME=n`, `RENDER=software` (desactiva OpenGL), `PICA_THREADS=n` (hilos de rasterización; por defecto = núcleos), `PICA_STATS=n`, `PICA_DRAWLOG=1`, `PICA_DUMPTEX=1`, `PICA_DEBUG=1`, `PICA_OFF=1`.
* Vídeo: `Y2R_LOG=1`, `Y2R_DUMP=30` (guarda el plano Y de 1 de cada 30 cuadros).
* `kill -USR1 <pid>` — muestra el estado de cada hilo (qué espera cada uno).
* `THREADS_LOG=1` — registra la creación de hilos (prioridad y núcleo).
* `NANCHECK=1` al generar el C: registra dónde nace un NaN en los cálculos de coma flotante del juego.
* `PICA_NANLOG=1`, `PICA_VSTRACE=cuadro` (+ `PICA_VSTRACE_N=nº de vértices`): uniforms con NaN y rastreo del shader.
* `ERRF_CONTINUA=1` — no cierra ante el error fatal del juego (`err:f`), que ahora se imprime con la pila.

### Variables de ejecución

* `ROMFS=/ruta/romfs` — carpeta romfs extraída de tu cartucho.
* `TECLAS="300:A,306:-,450:A,456:-"` — guion de botones (cuadro:botones; `-` suelta; `T160x120` toca la pantalla).
* `IDIOMA_3DS` (por defecto 1 = inglés), `NOME_3DS`, `SAVE_DIR` (por defecto `save/`).

### Cómo funciona

1. `tools/cro.py` lee el formato CRO0 (segmentos, exports, imports, reubicaciones).
2. `tools/link.py` coloca `code.bin` en `0x00100000` y cada CRO a partir de `0x40000000`.
   En tiempo de ejecución, `ldr:ro` (`runtime/ldr.c`) copia el CRO a donde el juego pide y apunta la
   tabla de segmentos al código ya recompilado.
3. `tools/funcs.py` descubre funciones; `tools/recomp_game.py` itera hasta que no aparece ninguna función nueva.
4. `tools/recomp.py` traduce cada función a `void F_xxxxxxxx(Ctx *c)` (o `T_` para Thumb).
5. `runtime/`: `runtime.c` (cargador, llamadas indirectas, depuración), `kernel.c` (hilos),
   `services.c` (srv, APT, hid, fs, cfg, dsp), `gsp.c` (colas, pantallas), `pica*.c` (GPU),
   `input.c` (botones/táctil), `y2r.c` (YUV→RGB), `ldr.c` (módulos).

### Cómo jugar en Windows

1. Descomprime esta carpeta en un lugar con unos 3 GB libres (el código generado y compilado se guarda aquí).
2. Abre **`XY Project.exe`** (el lanzador). En él:
   - **ROM del juego**: haz clic en *Buscar…* y elige tu `.3ds` **descifrado** (o arrastra el archivo
     a la ventana, o sobre `XY Project.exe`);
   - **Resolución interna**: 1x (original, 400×240) a 4x (1600×960); se puede cambiar en el juego con F8;
   - **Filtro de texturas**: original (como en la 3DS), suave (bilineal), anisotrópico 16x o bicúbico +
     anisotrópico 16x; se puede cambiar en el juego con F7;
   - **Renderizador de vídeo**: OpenGL o Vulkan (experimental; si no funciona en tu tarjeta, el juego vuelve a OpenGL);
   - **Idioma del programa**: Português, English, Español o 日本語; se elige la primera vez que se abre el lanzador
     (botón *Idioma* para cambiarlo; en el juego, *Opciones → Idioma del programa*, al instante). Vale para el lanzador, los
     mensajes de preparación, el menú del juego, los avisos en pantalla y la barra de título; no cambia el idioma del juego;
   - **JUGAR**.
3. La primera vez, el lanzador descarga, solo dentro de la carpeta `ferramentas`, un Python portátil (python.org), el
   compilador llvm-mingw y SDL2 (GitHub), lee el volcado, recompila el código a C y lo compila (de 10 a 40
   minutos, según el PC), mostrando el progreso. Las siguientes veces el juego se abre al momento. Mientras el juego
   está abierto el lanzador queda minimizado y vuelve cuando el juego se cierra; si algo sale mal, los detalles
   aparecen en su ventana.

Las elecciones se guardan en `lancador.ini`. `jogar.bat` sigue funcionando a la manera antigua (ventana de texto) y
usa la misma ROM. El código del lanzador está en `lancador/` (C puro con la API de Windows;
`lancador/compilar.sh` rehace el `.exe` con llvm-mingw).

Controles (teclado con la distribución de Citra/Azahar; mandos de Xbox, PlayStation y Switch mediante el mapeo de SDL, y otros
(DirectInput) mediante un mapeo por defecto; `jogo.log` muestra el nombre del mando conectado):

| 3DS | Teclado | Mando (Xbox y compatibles) |
|---|---|---|
| Botón deslizante (Circle Pad) | flechas | stick izquierdo |
| Cruceta | T F G H | cruceta |
| A / B / X / Y | A / S / Z / X | botones en las posiciones de la 3DS (el de la derecha es A); en el lanzador se pueden usar las etiquetas (A de Xbox = A) |
| L / R | Q / W | LB / RB |
| START / SELECT | Enter / Retroceso | Start / Back |
| Pantalla táctil | ratón (botón izquierdo) en la pantalla inferior | el stick derecho mueve un cursor en la pantalla inferior; su clic (R3) toca y, mantenido, arrastra |
| Pantalla completa | F11 | — |
| Mostrar/ocultar la pantalla inferior | Tab | clic del stick izquierdo (L3) |
| Filtro de texturas (original, suave, anisotrópico, bicúbico) | F7 | — |
| Resolución interna (1x–4x) | F8 | — |
| Disposición (pantalla superior completa / pantallas apiladas) | F9 | — |
| Acelerar el juego (mantener; 2x, 3x o 4x) | Espacio | RT (gatillo derecho) |
| Captura de pantalla | F12 | — |
| Patines (activar/desactivar) | P | LT (gatillo izquierdo) |

Las teclas y los botones del mando se pueden cambiar en el menú del juego (**Controles**): elige la acción, la columna
(teclado o mando) y pulsa Enter (o haz clic); después pulsa la tecla o el botón nuevo (Esc cancela). Una tecla
o botón usado en otra acción se quita de ella. En la misma página están el esquema de los botones (por posición o por etiqueta), la
velocidad al acelerar y *Restaurar valores predeterminados*. Las flechas del teclado siguen siendo el Circle Pad. Las elecciones se guardan
en `opcoes.txt`.

**Acelerar**: mientras se mantiene la tecla/botón, todo el juego va más rápido (también el reloj del juego),
sin sonido; aparece un aviso "» 2x" en la esquina. Útil para caminar y entrenar; en un PC modesto la ganancia queda limitada por
lo que aguanta el procesador. **Mostrar FPS** (en *Opciones*) pone los cuadros por segundo en la misma esquina.

**Patines** (P o LT): desactivados, el Circle Pad (stick y flechas) mueve al personaje como la cruceta,
caminando (o corriendo, manteniendo B) en lugar de patinar; activados, todo vuelve al comportamiento normal del juego. Un icono de patines
aparece en la esquina unos segundos al activarlos, y el mismo icono con una X al desactivarlos. La elección se guarda. Para usar el icono del
propio juego, guarda una imagen de él (una captura tuya, PNG) como `icones/patins.png` en la carpeta del proyecto; el fondo liso
alrededor se quita solo. Sin ese archivo aparece un icono genérico. Ninguna imagen viene con el proyecto; al
compartir el proyecto, no incluyas la carpeta `icones`.

**Capturas de pantalla** (F12): la pantalla superior a la resolución interna elegida (2x, 4K...) y, si la inferior se está
mostrando, esta debajo, en un PNG en `capturas/` en la carpeta del proyecto.

Los mandos que SDL no conoce reciben un **mapeo automático** al conectarse (a partir de los botones,
ejes y crucetas que informan; el aviso dice "mapeado automáticamente"). Antes de eso el juego consulta la base de
mandos de la comunidad de SDL (`ferramentas/gamecontrollerdb.txt`, descargada por `jogar.py`/`instalar.ps1`,
licencia zlib). Para corregir un mando a mano, crea un `controles.txt` en la carpeta del proyecto con líneas en el formato
de SDL (las mismas de `gamecontrollerdb.txt`). `jogo.log` lista los mandos vistos y el mapeo usado.

**Arte de fondo del menú** (opcional): pon una imagen tuya en `fundos/fundo_x.jpg` y/o `fundos/fundo_y.jpg`
(o `.png`) en la carpeta del proyecto. Aparece de fondo en el menú del juego de la versión correspondiente mientras la
imagen del juego todavía no está en pantalla (la primera apertura, la pantalla de idioma del juego, las cargas). Ninguna
imagen viene con el proyecto; al compartir el proyecto, no incluyas la carpeta `fundos`. El arte está animado: la imagen se acerca
y se desliza despacio, con partículas de luz que suben y un brillo suave del color de la versión (`MENU_ANIMADO=0` la deja quieta).

**Texturas personalizadas** (paquetes en el formato de Citra/Azahar, los que traen PNG con nombres como
`tex1_256x256_24FD4EDB55375DB2_13_mip0.png`): copia el contenido del paquete a `texturas/<id del juego>/`
(`texturas/0004000000055D00` en X, `texturas/0004000000055E00` en Y; la carpeta se crea la primera vez que el juego
se abre; las subcarpetas valen) o directamente en `texturas/`. Se activa y desactiva en el menú del juego (*Opciones → Texturas
personalizadas*), al instante. Vale para paquetes sin `pack.json` (el hash antiguo de Citra), que son la mayoría de los de
Pokémon X/Y. Ningún paquete viene con el proyecto: usa los que hayas descargado. `jogo.log` dice cuántas texturas encontró.

Las partidas están en `jogos/<id>/save` y el registro en `jogos/<id>/jogo.log`. Cada vez que el juego se abre, una copia de la
partida va a `jogos/<id>/backups/save_<fecha>.zip` (se conservan las 5 últimas); para volver a una, cierra el juego, borra
la carpeta `save` y extrae el zip en la carpeta `jogos/<id>`.

Si el juego se cierra con un error, se guarda un **informe** en `jogos/<id>/erros/relatorio_<fecha>.txt` (el tipo de
error, dónde ocurrió, el hilo y los registros del juego, los módulos cargados y el final del registro); *VER
DETALLES* en el lanzador abre el informe. Es el archivo que hay que enviar al reportar el problema.
Al actualizar este proyecto (descomprimiendo la versión nueva encima), `jogar.bat` lo nota y recompila solo el runtime.
Para rehacerlo todo, borra la carpeta `jogos/<id>`.
Para cambiar de juego, elige otra ROM en el lanzador (o borra `jogo.txt`, si usas `jogar.bat`).

### Cómo ejecutar en Linux

Necesitas `python3`, `clang`/`clang++` y, para tener ventana y sonido, SDL2 de desarrollo
(`sudo apt install libsdl2-dev`): `./jogar.sh ruta/del/juego.3ds`. Sin SDL2 funciona sin ventana
(`SHOT=carpeta/s SHOT_EVERY=60` para capturas; botones por `TECLAS=...` o `teclas_agora.txt` en `jogos/<id>/`;
`AUDIO_WAV=som.raw` graba el sonido en bruto, estéreo de 16 bits a 32728 Hz).

Modo de desarrollo (carpetas ya extraídas): `CODE=in/code.bin ROMFS=romfs ./rebuild.sh`.
Pruebas: `python3 test/run_tests.py` y `python3 test/make_test_3ds.py` (monta un .3ds de prueba).

### Próximos pasos

1. Medir OpenGL en Windows (fps de la barra de título) y quitar los cuellos de botella que queden en la CPU
   (copia del framebuffer memoria <-> GPU en cada lista, decodificación de texturas, shader de vértices).
2. Comprobar los fps al aire libre después de la 7.8; el fondo de los combates en la ciudad que no aparece al empezar el combate.
3. Vulkan: primera versión lista (opción en el lanzador); medir y comparar con OpenGL.

### Licencias de terceros

* `runtime/audio/` — HLE de audio de Azahar/Citra, GPL-2.0 o posterior (`runtime/audio/LICENSE.txt`).
* `runtime/faad2/` — decodificador AAC faad2, GPL-2.0 o posterior (`runtime/faad2/COPYING`).
* SDL2 (descargada por `instalar.ps1`), licencia zlib.
* `runtime/stb_image.h` — lector de PNG/JPEG de Sean Barrett, dominio público (o MIT, a elección).
* `runtime/stb_image_write.h` — escritor de PNG (capturas de pantalla) de Sean Barrett, dominio público (o MIT, a elección).
* `runtime/texpack.c` — incluye CityHash64 de Google (licencia MIT, aviso en el archivo).
* SDL_GameControllerDB (`ferramentas/gamecontrollerdb.txt`, descargado en la primera ejecución), licencia zlib.
* `runtime/fonte_aviso.h` — mapa de bits generado (`tools/gerar_fonte.py`) a partir de la fuente DejaVu Sans Bold, licencia de las fuentes DejaVu (libre, derivada de Bitstream Vera), y de Noto Sans CJK JP (japonés), SIL Open Font License 1.1.
* `runtime/fonte_menu.h` — mapa de bits generado (`tools/gerar_fonte_menu.py`) a partir de Inter y Noto Sans CJK JP (japonés), ambas SIL Open Font License 1.1.
Como incluye código GPL, el ejecutable generado también queda bajo la GPL; nada de esto incluye datos del juego.

---

## 日本語

このプロジェクトは、ポケットモンスター X・Y のマシンコード（3DS の ARM11）を **ネイティブな C 言語** に変換し、
CPU エミュレーターなしで PC 上でコンパイル・実行できるようにするものです。ゲームのファイルは一切含まれていません。
各自が自分のカートリッジから吸い出したデータ（ダンプ）を使います。

### 現在の状況

| 段階 | 状況 |
|---|---|
| CRO/CRS モジュールの解析（`.cro` 110 個 + `static.crs`） | 完了 |
| 静的リンカー：`code.bin` と全 CRO を 1 つのイメージに | 完了（CRO は `0x40000000`） |
| 関数の検出（BL/BLX、vtable、リテラル、相対テーブル、CRO のエクスポート、ARMv4 形式の呼び出し、ADR） | 約 74,150 関数 |
| ARMv6K + VFPv2 + ARMv6 SIMD + Thumb-1 → C の再コンパイラー | 300 万命令、99.1% を変換 |
| 差分テスト（ARM と Thumb、-O1/-O2/-Os/-O3） | ARM 83/83 |
| HLE カーネル：スレッド、期限による横取りつきシングルコアスケジューラー、イベント、ミューテックス、タイマー、アービター | 完了 |
| サービス：srv:、APT、hid、cfg、fs（RomFS + PC のフォルダーにセーブ）、dsp（プロトコル）、gsp、ldr:ro、y2r | 十分に動作 |
| **ソフトウェアによる PICA200 GPU**：コマンドリスト、頂点シェーダー、ラスタライズ、テクスチャ（14 形式 + ETC1）、TEV、ブレンド、深度／ステンシル、**ピクセル単位のライティング**（ライト 8 個、LUT、法線マップ） | 完了 |
| 入力：Windows のウィンドウでキーボード、マウス（タッチ）、XInput コントローラー。`TECLAS` スクリプトと `teclas_agora.txt` | 完了 |
| Mobiclip 動画（`.moflex`）：ゲーム自身のデコーダー + `y2r` による YUV→RGB 変換 | 動作 |
| 言語選択 →「Begin game」→ オープニング動画全編（ゼルネアス、「Presented by Game Freak」）→ A → 導入部（`DllIntro`） | 完了 |
| プラターヌ博士の 3D 導入部：背景、光るモンスターボール、ポケモンの額縁、会話、質問、主人公の見た目の選択 | **ここまで到達** |
| 主人公の部屋までの導入部全体（最初のマップ、下画面の PSS メニュー） | 段階 7.5 |
| キャラクターの 3D モデル（博士、ゴーゴート、鏡に映る主人公）と「X」ロゴのアニメーション | **表示される**（GPU コマンドリスト間のジャンプ、レジスター 0x238〜0x23D が未対応だった） |
| 性能：全コアでのラスタライズ + 頂点キャッシュ（2 コアの PC で約 1.6 倍、コアが多いほどさらに向上） | 段階 6.1 |
| 復号済み `.3ds` を直接読み込み + 初回実行時に自動で再コンパイル | 完了 |
| Windows：`jogar.bat` がポータブル版 Python + llvm-mingw + SDL2 をダウンロードし、コンパイルしてウィンドウを開く | **動作**（ユーザーが確認済み） |
| **グラフィックカード上の GPU（OpenGL 3.3）**：ラスタライズ、テクスチャ、ライティング、TEV、ブレンド、深度／ステンシルを 1 つのシェーダーで処理。頂点とクリッピングは CPU のまま | 段階 7（ソフトウェアは予備として残る：`RENDER=software`） |
| OpenGL：フレームバッファーはリストやフレームをまたいで GPU に残る（CPU が読む必要があるときだけメモリーに戻る）。画面クリアは glClear に。頂点シェーダーは全コアで実行 | 段階 7.1 |
| 非同期 GPU：GX コマンドはゲームと並行して専用スレッドで実行（3DS と同じ）。PICA のリストは送信時にコピー | 段階 7.3（`GPU_SINCRONA=1` で無効） |
| PICA のジオメトリシェーダー（パーティクル、きらめき、光線、技のエフェクト） | 段階 7.7（`PICA_SEM_GS=1` で無効） |
| 頂点シェーダーが約 6〜9 倍高速に：命令はプログラムごとに 1 回だけデコードし、16 頂点ずつまとめて実行（SIMD）、分岐が分かれる場合はマスクで処理。描画ごとの属性読み取りプラン。getenv のキャッシュ。ブロックを動的に割り振るスレッドプール（起きるのが遅れたスレッドを待たない） | 段階 7.8（`PICA_VSCHECK=1` でバッチをスカラーインタープリターと照合） |
| バトル開始時のトレーナー：色／深度の書き込みロック（0x113/0x115）に対応。描画自身のバッファーであっても深度+ステンシルをテクスチャとして読む | 7.9 で修正 |
| カーネル：CloseHandle がハンドルを解放する（以前は約 25 分のプレイで枯渇していた） | 7.9 で修正 |
| GPU：投影テクスチャ（tc0/tc0w）、0x84 のフィルター／バイアス／レベルに従うミップマップ、0xE6〜0xEF のテーブルによるフォグ | 7.9 |
| 線形→タイルの画面転送（反転、タイル→タイルも）：ヤヤコマのシーン終わりのモザイク。深度を float24 の精度で色として読む（輪郭の破線） | 7.9 |
| 内部解像度 1x〜4x（F8 または RESOLUCAO=n）：GPU のサーフェスを拡大し、メモリーとの往復時に blit で縮小／拡大、上画面を拡大したままウィンドウに表示 | 8.0 |
| レイアウト：上画面は 16:9、下画面は必要なときに隅に表示（Tab）、バッグは隅に開く | 8.0 |
| サウンド：DSP はゲームが前のフレームを処理するのを待つ（ノイズ対策）。出力キューは自動で調整（45〜150 ms） | 8.1 |
| Windows：カーネルの待機が正確に（高解像度タイマー + イベント、高精度カウンター）。以前は最大 17 ms 遅れて起き、ゲームが VBlank を逃していた（fps が 30 未満でふらつく） | 8.1 |
| ランチャー `XY Project.exe`（ROM、解像度、テクスチャフィルター、コントローラーのボタン、言語、プレイ）。コントローラー：右スティックでタッチカーソル（R3 でタッチ、L3 で下画面を表示）、ボタンは位置または表記で、汎用 DirectInput コントローラー、接続／切断時の画面通知、レンダラーのテクスチャフィルター（強制バイリニア、16 倍異方性、Catmull-Rom バイキュービック。F7） | 8.2 |
| **ポケットモンスター Y** も同じプログラムで動作（ランチャーと `jogar.py` はどちらのバージョンも再コンパイル。ウィンドウには「Pokémon X」または「Pokémon Y」と表示）。プロジェクト名を Pokémon XY に変更 | 8.2 |
| 下画面も高解像度に（以前は上画面だけ拡大表示）。ゲームのウィンドウとタスクバーに Pokémon XY のアイコン。Windows ではゲームのスレッドを通常より高い優先度に（SEM_PRIORIDADE=1 で無効） | 8.3 |
| **60 fps**（保留中：ランチャーには出していない。テスト専用 `FPS=60 FPS60_TESTE=1`）：ゲームのロジックは毎秒 30 ステップのまま。毎フレーム、OpenGL レンダラーが各頂点を前のフレームと今のフレームの中間に置いてもう一度描画し（サーフェスのコピー上で行い、ゲームのメモリーには触れない）、ウィンドウは中間フレームと本来のフレームを VBlank ごとに交互に表示。シーンやカメラが切り替わるとフレームを繰り返すだけ。遅延は約 1 VBlank（17 ms）、グラフィックカードの負荷は 2 倍。プロファイルに [60fps] の行 | 8.3 |
| **Vulkan 1.3**（ランチャーの「映像レンダラー」、`RENDER=vulkan`）：OpenGL と同じ処理（シェーダーは `runtime/vk/` に移植し、`vk/gerar.sh` でコンパイル済みのものを `vk_shaders.h` に埋め込み）。すべてオフスクリーンで、各リストの終わりにコマンドをまとめて送信し、画面は非同期で読み戻す。Vulkan が開けない場合（1.3 非対応のカード、古いドライバー）は自動で OpenGL に戻る | 8.4 |
| 初回コンパイル時の関数探索をより徹底（ARM/Thumb のプロローグ、リテラルプール内の Thumb コードへのポインター）：+1,900 関数。実行時に組み立てたポインター経由でしか呼ばれない関数も含む（ポケモンセンターでのクラッシュ）。ランチャーに **すべて再コンパイル** ボタン（ROM からゲームを作り直す。セーブは残る） | 8.3 |
| バトル後にキャラクターの輪郭が消える（キャッシュされたフレームバッファーが削除済みテクスチャに結びついていた） | 8.1 で修正 |
| 性能：三角形のクリッピングを即時受け入れ・コピーなしに。GPU 画面の非同期コピー（PBO + フェンス）でゲームを止めない。ウィンドウはコピーが終わった画面だけを表示し、古いフレームに戻ることはない（GL_LEITURA_SINCRONA=1 で即時コピーに戻す）。プロファイルに [ritmo] の行 | 8.1 |
| サウンド：速度を細かく調整するリングバッファー（キューが空になったり満杯になったりしてもプチッという音が出ない） | 7.9 |
| ETC1 テクスチャ：サブブロックの色が入れ替わる individual モード（ギザギザの輪郭） | 7.6 で修正 |
| タイトルバーに fps（1 秒あたりの描画フレーム数）、速度（100% = 毎秒 60 VBlank）、使用中のレンダラーを表示 | 段階 7 |
| **サウンド**：HLE の DSP（24 ボイスのミキシング、フィルター、ADPCM/PCM。Azahar から）+ faad2 による AAC の音楽 | この段階で追加 |
| **SDL2 ウィンドウ**（Windows と Linux）：サイズ変更、全画面（F11）、キーボード、コントローラー、マウスでタッチ、サウンド | 追加（サウンドなしの Win32 ウィンドウは予備として残る） |

### ゲームを動かして見つかった再コンパイラーのバグ（すべて回帰テストあり）

* 条件付きリターン（`ldreq pc`、`popgt {..., pc}`）が後続のコードを切り捨てていた（`t_condret`）。
* `pop {r3}; bx r3` が別の関数へのジャンプとして扱われていた（`t_bxret`）。
* ARMv4 形式の呼び出し（`bx`、`mov pc`、`add pc` の前の `mov lr, pc` / `add lr, pc, #n`）
  — アセンブリで書かれた動画デコーダーが多用している（`t_v4call`）。
* 同じ考え方の直接分岐：`add lr, pc, #n; bne サブルーチン`（`t_v4b`）。以前はサブルーチンが
  関数の中に「貼り付けられて」しまい、デコーダーがビットストリームとの同期を失っていた。
* 前に積んだ `lr` で戻る末尾呼び出し（IDCT への `mov pc, r4`。IDCT は `pop {..., pc}` で終わる）：
  ジャンプ先の関数は、ジャンプ元が期待する戻り先を引き継ぐようになった（`c->tret`）。以前は続きの処理が
  2 回実行されていた（`t_tailret`）。

この段階の GPU とサービス：頂点アドレスを物理空間で計算（`p2v` の前）、面カリング（0x40）の向きが
逆だった、空の設定ブロック 0x00050005（立体視カメラ）のせいで射影行列が NaN になり 3D がすべて消えていた。

段階 7：GPU がコマンドリストの「ジャンプ」（CMDBUF_JUMP、レジスター 0x23C/0x23D）に従っていなかった。
3D モデルはこれを使って、ゲームファイルに保存された作成済みリストをつなげている。これがないと
博士、ポケモン、ロゴのアニメーションがまったく描画されなかった。デバッグ：`PICA_JUMPLOG=1`、
`PICA_DUMPV=n1,n2`（頂点数 n の描画の頂点統計）。

同じく段階 7：2x2 縮小つきの画面転送（場面転換で画面を「撮影」するのに使う）が
2 倍の幅で書き込み、ゲームのバッファーをはみ出して隣のオブジェクトを壊していた。そのため導入部から
主人公の部屋に移るときにゲームが終了していた。またサウンドでは、カーネルが遅れると DSP のフレームが飛ばされ
（音の途切れ）ていたが、遅れた分を取り戻すようにした。

段階 6：セーブの `GetFormatInfo` が、ゲームがフォーマット時に指定した値ではなく固定値を返していた。
そのため 2 回目以降の起動で「The saved game data is corrupted」と表示されていた。現在は
パラメーターを `save/savedata.formato` に保存する（この段階より前に作ったセーブは `save` フォルダーを削除）。

### デバッグツール

* `PROBES=10d71c,111bb8 python3 tools/recomp_game.py ...` — 指定アドレスでレジスターを表示する
  プローブを挿入（`PROBE_MEM=r0` で指す先のメモリーを表示、`PROBE_MAX=n` で回数を制限）。
  `DUMP_RANGES="08000000-09000000,..." DUMP_AT=n` で n 番目のプローブ時にメモリーを保存。
* `python3 tools/lockstep2.py out/p.log 08000000=out/mem_08000000.bin ...` — 再コンパイルしたコードを
  **参照用 ARM インタープリター**（`tools/armi.py`、別途作成）とプローブごとに比較。
* `WATCHW=0fffff70` — 正確な書き込み監視（保護ページ + シングルステップ）、関数スタックつき。
* `python3 tools/dis.py <アドレス> [n] [thumb]` — リンク済みイメージを逆アセンブル。
* `python3 tools/profile.py <pid>` — 実行中スレッドのサンプリングプロファイル（gdb）。
* 不明な間接ジャンプ先は `out/extra_funcs.txt` に記録され、次の生成でシードになる。
* `SHOT=shots/f SHOT_EVERY=60` — 両画面を PPM で保存。
* GPU：`PICA_PERFIL=1`（300 フレームごとに頂点・描画・GPU→メモリー・テクスチャにかかった時間の割合）、`GL_DEBUG_FRAME=n`、`RENDER=software`（OpenGL を無効化）、`PICA_THREADS=n`（ラスタライズのスレッド数。既定はコア数）、`PICA_STATS=n`、`PICA_DRAWLOG=1`、`PICA_DUMPTEX=1`、`PICA_DEBUG=1`、`PICA_OFF=1`。
* 動画：`Y2R_LOG=1`、`Y2R_DUMP=30`（30 フレームに 1 回 Y プレーンを保存）。
* `kill -USR1 <pid>` — 各スレッドの状態（それぞれが何を待っているか）を表示。
* `THREADS_LOG=1` — スレッドの作成（優先度とコア）を記録。
* C の生成時に `NANCHECK=1`：ゲームの浮動小数点計算で NaN が生じる場所を記録。
* `PICA_NANLOG=1`、`PICA_VSTRACE=フレーム`（+ `PICA_VSTRACE_N=頂点数`）：NaN を含む uniform とシェーダーのトレース。
* `ERRF_CONTINUA=1` — ゲームの致命的エラー（`err:f`）で終了しない。エラーはスタックつきで表示される。

### 実行時の変数

* `ROMFS=/パス/romfs` — 自分のカートリッジから取り出した romfs フォルダー。
* `TECLAS="300:A,306:-,450:A,456:-"` — ボタン入力スクリプト（フレーム:ボタン。`-` で離す。`T160x120` で画面をタッチ）。
* `IDIOMA_3DS`（既定 1 = 英語）、`NOME_3DS`、`SAVE_DIR`（既定 `save/`）。

### 仕組み

1. `tools/cro.py` が CRO0 形式（セグメント、エクスポート、インポート、再配置）を読む。
2. `tools/link.py` が `code.bin` を `0x00100000` に、各 CRO を `0x40000000` 以降に配置する。
   実行時には `ldr:ro`（`runtime/ldr.c`）がゲームの指定する場所に CRO をコピーし、
   セグメントテーブルを再コンパイル済みのコードに向ける。
3. `tools/funcs.py` が関数を見つけ、`tools/recomp_game.py` が新しい関数が出なくなるまで繰り返す。
4. `tools/recomp.py` が各関数を `void F_xxxxxxxx(Ctx *c)`（Thumb は `T_`）に変換する。
5. `runtime/`：`runtime.c`（ローダー、間接呼び出し、デバッグ）、`kernel.c`（スレッド）、
   `services.c`（srv、APT、hid、fs、cfg、dsp）、`gsp.c`（キュー、画面）、`pica*.c`（GPU）、
   `input.c`（ボタン／タッチ）、`y2r.c`（YUV→RGB）、`ldr.c`（モジュール）。

### Windows で遊ぶには

1. このフォルダーを空き容量 3 GB ほどの場所に展開します（生成・コンパイルされたコードはここに置かれます）。
2. **`XY Project.exe`**（ランチャー）を開き、次を設定します：
   - **ゲームの ROM**：*参照…* をクリックして **復号済み** の `.3ds` を選びます（ファイルを
     ウィンドウ、または `XY Project.exe` の上にドラッグしても可）。
   - **内部解像度**：1x（オリジナル、400×240）〜 4x（1600×960）。ゲーム中は F8 で切り替え可能。
   - **テクスチャフィルター**：オリジナル（3DS と同じ）、なめらか（バイリニア）、16 倍異方性、バイキュービック +
     16 倍異方性。ゲーム中は F7 で切り替え可能。
   - **映像レンダラー**：OpenGL または Vulkan（試験的。カードで動かない場合は自動で OpenGL に戻ります）。
   - **プログラムの言語**：Português、English、Español、日本語。ランチャーを初めて開いたときに選びます
     （変更は *言語* ボタン。ゲーム中は *設定 → プログラムの言語* ですぐに反映）。ランチャー、
     準備中のメッセージ、ゲーム内メニュー、画面上の通知、タイトルバーに適用され、ゲーム自体の言語は変わりません。
   - **プレイ**。
3. 初回は、ランチャーが `ferramentas` フォルダーの中だけにポータブル版 Python（python.org）、
   llvm-mingw コンパイラー、SDL2（GitHub）をダウンロードし、ダンプを読み込んでコードを C に再コンパイルし、コンパイルします
   （PC によって 10〜40 分）。進行状況が表示されます。2 回目以降はすぐにゲームが開きます。ゲームを
   開いている間ランチャーは最小化され、ゲームを閉じると戻ってきます。問題が起きた場合は、その詳細が
   ランチャーのウィンドウに表示されます。

選んだ設定は `lancador.ini` に保存されます。`jogar.bat` も従来どおり（テキストウィンドウで）動作し、
同じ ROM を使います。ランチャーのコードは `lancador/` にあります（Windows API を使った素の C。
`lancador/compilar.sh` で llvm-mingw を使って `.exe` を作り直せます）。

操作方法（キーボードは Citra/Azahar と同じ配置。Xbox、PlayStation、Switch のコントローラーは SDL のマッピングで、その他の
コントローラー（DirectInput）は標準マッピングで動作。接続したコントローラーの名前は `jogo.log` に出ます）：

| 3DS | キーボード | コントローラー（Xbox と互換機） |
|---|---|---|
| スライドパッド | 矢印キー | 左スティック |
| 十字ボタン | T F G H | 十字キー |
| A / B / X / Y | A / S / Z / X | 3DS と同じ位置のボタン（右が A）。ランチャーで表記どおりにも設定可能（Xbox の A = A） |
| L / R | Q / W | LB / RB |
| START / SELECT | Enter / Backspace | Start / Back |
| タッチスクリーン | 下画面上でマウス（左ボタン） | 右スティックで下画面のカーソルを動かし、押し込み（R3）でタッチ、押したままでドラッグ |
| 全画面 | F11 | — |
| 下画面の表示／非表示 | Tab | 左スティック押し込み（L3） |
| テクスチャフィルター（オリジナル、なめらか、異方性、バイキュービック） | F7 | — |
| 内部解像度（1x〜4x） | F8 | — |
| レイアウト（上画面いっぱい／画面を縦に並べる） | F9 | — |
| 早送り（長押し。2 倍、3 倍、4 倍） | Space | RT（右トリガー） |
| スクリーンショット | F12 | — |
| ローラースケート（オン／オフ） | P | LT（左トリガー） |

キーとコントローラーのボタンは、ゲーム内メニューの **操作設定** で変更できます。操作と列
（キーボードまたはコントローラー）を選んで Enter を押す（またはクリック）と、新しいキーやボタンを押すよう求められます（Esc でキャンセル）。
ほかの操作に使われていたキーやボタンは、そちらから外れます。同じページで、ボタンの配置（位置／表記）、
早送りの速度、*初期設定に戻す* も選べます。キーボードの矢印キーは常にスライドパッドです。設定は
`opcoes.txt` に保存されます。

**早送り**：キー／ボタンを押している間、ゲーム全体（ゲーム内の時計も）が速く進み、
音は消えます。画面の隅に「» 2x」と表示されます。移動やレベル上げに便利です。性能の低い PC では、
プロセッサーの処理能力の範囲でしか速くなりません。*設定* の **FPS を表示** で、同じ隅に毎秒のフレーム数を表示します。

**ローラースケート**（P または LT）：オフにすると、スライドパッド（スティックと矢印キー）で十字ボタンと同じように主人公が動き、
滑らずに歩きます（B を押しながらで走ります）。オンにすると、ゲーム本来の動きに戻ります。オンにしたときは
スケートのアイコンが数秒間隅に表示され、オフにしたときは同じアイコンに × が付いて表示されます。設定は保存されます。ゲーム本来の
アイコンを使いたい場合は、その画像（自分で撮ったスクリーンショット、PNG）をプロジェクトフォルダーの `icones/patins.png` として保存してください。
周りの単色の背景は自動で取り除かれます。このファイルがない場合は汎用のアイコンが表示されます。画像はプロジェクトに含まれていません。
プロジェクトを共有するときは `icones` フォルダーを含めないでください。

**スクリーンショット**（F12）：選んだ内部解像度（2x、4K など）の上画面と、下画面が
表示されていればその下に下画面を並べて、プロジェクトフォルダーの `capturas/` に PNG で保存します。

SDL が知らないコントローラーは、接続時に **自動でマッピング** されます（報告されたボタン、
軸、ハットから作成。通知に「自動でマッピングしました」と表示）。その前に、ゲームは SDL のコミュニティによる
コントローラーデータベース（`ferramentas/gamecontrollerdb.txt`、`jogar.py`／`instalar.ps1` がダウンロード、
zlib ライセンス）を参照します。コントローラーを手動で直すには、プロジェクトフォルダーに `controles.txt` を作り、SDL の
形式（`gamecontrollerdb.txt` と同じ）で行を書きます。`jogo.log` には、検出したコントローラーと使ったマッピングが記録されます。

**メニューの背景アート**（任意）：自分の画像をプロジェクトフォルダーの `fundos/fundo_x.jpg` や `fundos/fundo_y.jpg`
（または `.png`）に置きます。ゲームの画面がまだ表示されていない間（初回起動、ゲームの言語選択画面、読み込み中）、
対応するバージョンのゲーム内メニューの背景に表示されます。画像は
プロジェクトに含まれていません。プロジェクトを共有するときは `fundos` フォルダーを含めないでください。背景アートはアニメーションします：画像がゆっくり
ズーム・移動し、光の粒が立ちのぼり、バージョンの色でやわらかく光ります（`MENU_ANIMADO=0` で静止）。

**カスタムテクスチャ**（Citra/Azahar 形式のパック。`tex1_256x256_24FD4EDB55375DB2_13_mip0.png` のような
名前の PNG が入っているもの）：パックの中身を `texturas/<ゲーム ID>/` にコピーします
（X は `texturas/0004000000055D00`、Y は `texturas/0004000000055E00`。フォルダーはゲームを初めて
開いたときに作られます。サブフォルダーも可）。`texturas/` に直接置いても構いません。ゲーム内メニュー（*設定 → カスタム
テクスチャ*）ですぐにオン／オフできます。`pack.json` のないパック（Citra の旧ハッシュ）に対応しており、
ポケットモンスター X・Y 用のパックの大半はこれです。パックはプロジェクトに含まれていません。自分でダウンロードしたものを使ってください。見つかったテクスチャの数は `jogo.log` に出ます。

セーブは `jogos/<id>/save`、ログは `jogos/<id>/jogo.log` にあります。ゲームを開くたびに、
セーブのコピーが `jogos/<id>/backups/save_<日付>.zip` に作られます（最新 5 個を保持）。元に戻すには、ゲームを閉じて
`save` フォルダーを削除し、zip を `jogos/<id>` フォルダーに展開します。

ゲームがエラーで終了した場合は、**エラーレポート** が `jogos/<id>/erros/relatorio_<日付>.txt` に保存されます（エラーの
種類、発生場所、ゲームのスレッドとレジスター、読み込まれていたモジュール、ログの末尾）。ランチャーの
*詳細を見る* でレポートが開きます。問題を報告するときは、このファイルを添付してください。
このプロジェクトを更新するとき（新しいバージョンを上書き展開したとき）は、`jogar.bat` が変更に気づいてランタイムだけを再コンパイルします。
すべて作り直すには `jogos/<id>` フォルダーを削除してください。
ゲームを切り替えるには、ランチャーで別の ROM を選びます（`jogar.bat` を使う場合は `jogo.txt` を削除）。

### Linux で動かすには

`python3`、`clang`/`clang++` と、ウィンドウとサウンドを使うために SDL2 の開発用パッケージ
（`sudo apt install libsdl2-dev`）が必要です：`./jogar.sh ゲームへのパス.3ds`。SDL2 がない場合はウィンドウなしで動作します
（スクリーンショットは `SHOT=フォルダー/s SHOT_EVERY=60`。ボタンは `TECLAS=...` または `jogos/<id>/` の `teclas_agora.txt`。
`AUDIO_WAV=som.raw` で生の音声を記録、16 ビットステレオ 32728 Hz）。

開発モード（展開済みのフォルダー）：`CODE=in/code.bin ROMFS=romfs ./rebuild.sh`。
テスト：`python3 test/run_tests.py` と `python3 test/make_test_3ds.py`（テスト用の .3ds を作成）。

### 今後の予定

1. Windows で OpenGL を計測し（タイトルバーの fps）、CPU に残っているボトルネックを取り除く
   （リストごとのフレームバッファーのメモリー <-> GPU コピー、テクスチャのデコード、頂点シェーダー）。
2. 7.8 以降の屋外での fps を確認。街でのバトル開始時に背景が表示されない問題。
3. Vulkan：最初のバージョンは完成（ランチャーで選択可能）。計測して OpenGL と比較する。

### サードパーティのライセンス

* `runtime/audio/` — Azahar/Citra のオーディオ HLE、GPL-2.0 以降（`runtime/audio/LICENSE.txt`）。
* `runtime/faad2/` — AAC デコーダー faad2、GPL-2.0 以降（`runtime/faad2/COPYING`）。
* SDL2（`instalar.ps1` がダウンロード）、zlib ライセンス。
* `runtime/stb_image.h` — Sean Barrett による PNG/JPEG リーダー、パブリックドメイン（または MIT、選択可）。
* `runtime/stb_image_write.h` — Sean Barrett による PNG ライター（スクリーンショット）、パブリックドメイン（または MIT、選択可）。
* `runtime/texpack.c` — Google の CityHash64 を含む（MIT ライセンス、ファイル内に表記）。
* SDL_GameControllerDB（`ferramentas/gamecontrollerdb.txt`、初回実行時にダウンロード）、zlib ライセンス。
* `runtime/fonte_aviso.h` — DejaVu Sans Bold フォント（DejaVu フォントライセンス。自由、Bitstream Vera の派生）と Noto Sans CJK JP（日本語、SIL Open Font License 1.1）から生成したビットマップ（`tools/gerar_fonte.py`）。
* `runtime/fonte_menu.h` — Inter と Noto Sans CJK JP（日本語）から生成したビットマップ（`tools/gerar_fonte_menu.py`）、どちらも SIL Open Font License 1.1。
GPL のコードを含むため、生成された実行ファイルも GPL に従います。いずれにもゲームのデータは含まれていません。

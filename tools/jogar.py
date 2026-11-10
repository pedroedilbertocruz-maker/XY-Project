"""Abre um dump do jogo (.3ds/.cci/.cxi DECIFRADO), recompila na primeira vez e joga.
Funciona no Windows (com as ferramentas baixadas pelo instalar.ps1) e no Linux.

  python tools/jogar.py caminho/do/jogo.3ds [--refazer] [--sem-janela]

Na primeira vez: lê o código e os módulos do dump, recompila o código ARM para C e compila tudo
(de 10 a 40 minutos, conforme o PC). Depois disso abre direto. O RomFS é lido do próprio dump.
Arquivos gerados, saves e log ficam em jogos/<id do programa>/.
"""
import concurrent.futures, glob, os, shutil, subprocess, sys, time

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(RAIZ, 'tools')
RUNTIME = os.path.join(RAIZ, 'runtime')
WIN = os.name == 'nt'
sys.path.insert(0, TOOLS)
import ctr  # noqa: E402
import romfs_build  # noqa: E402

LANG = os.environ.get('IDIOMA', 'pt')          # idioma do programa (o lançador passa IDIOMA=pt/en/es/ja)
LANC = bool(os.environ.get('LANCADOR'))        # chamado pelo lançador: manda marcas de progresso (@@...)


def L(pt, en, es=None, ja=None):
    return {'en': en, 'es': es or en, 'ja': ja or en}.get(LANG, pt)

RT_COMMON = ['runtime', 'kernel', 'services', 'gsp', 'ldr', 'input', 'y2r', 'plat', 'interp', 'vfy', 'crash']
RT_FAST = ['pica', 'pica_draw', 'pica_shader', 'pica_light', 'pica_gl', 'pica_vk', 'texpack', 'captura']
# som: HLE do DSP (do Azahar, C++) e decodificador AAC (faad2) para as músicas
RT_CPP = ['dsp_hle', 'audio/audio_core/hle/source', 'audio/audio_core/hle/mixers', 'audio/audio_core/hle/filter',
          'audio/audio_core/codec', 'audio/audio_core/interpolate']
FAAD_DEFS = ['-DHAVE_INTTYPES_H=1', '-DHAVE_MEMCPY=1', '-DHAVE_STRING_H=1', '-DHAVE_STRINGS_H=1', '-DHAVE_SYS_STAT_H=1',
             '-DHAVE_SYS_TYPES_H=1', '-DHAVE_LRINTF=1', '-DPACKAGE_VERSION="2.11"']
SDL_WIN = os.path.join(RAIZ, 'ferramentas', 'SDL2', 'x86_64-w64-mingw32')


def achar_sdl():
    """(flags de compilação, flags de link, dll a copiar) da SDL2, ou None se não houver."""
    if WIN:
        if os.path.exists(os.path.join(SDL_WIN, 'lib', 'libSDL2.dll.a')):
            return (['-I', os.path.join(SDL_WIN, 'include', 'SDL2')],
                    # caminho direto da biblioteca de importação: com -static, o -lSDL2 pegaria a libSDL2.a
                    # (estática), que precisa de dezenas de bibliotecas do sistema
                    [os.path.join(SDL_WIN, 'lib', 'libSDL2.dll.a')], os.path.join(SDL_WIN, 'bin', 'SDL2.dll'))
        return None
    if shutil.which('sdl2-config'):
        cf = subprocess.run(['sdl2-config', '--cflags'], capture_output=True, text=True).stdout.split()
        lf = subprocess.run(['sdl2-config', '--libs'], capture_output=True, text=True).stdout.split()
        return cf, lf, None
    return None


def achar_clang():
    local = os.path.join(RAIZ, 'ferramentas', 'llvm-mingw', 'bin', 'clang.exe' if WIN else 'clang')
    if os.path.exists(local):
        return local
    c = shutil.which('clang')
    if not c:
        sys.exit(L('não achei o compilador clang. No Windows, rode instalar.ps1 (ou jogar.bat, que chama ele).',
                   'clang compiler not found. On Windows, run instalar.ps1 (or jogar.bat, which calls it).',
                   'no encontré el compilador clang. En Windows, ejecuta instalar.ps1 (o jogar.bat, que lo llama).',
                   'clang コンパイラーが見つかりません。Windows では instalar.ps1（または それを呼ぶ jogar.bat）を実行してください。'))
    return c


OPCOES = os.path.join(RAIZ, 'opcoes.txt')     # escolhas do lançador e do menu do jogo (chave=valor)


def opcoes_ler():
    o = {}
    try:
        with open(OPCOES, encoding='utf-8-sig') as f:
            for linha in f:
                if '=' in linha:
                    k, v = linha.rstrip('\r\n').split('=', 1)
                    o[k.strip()] = v.strip()
    except OSError:
        pass
    return o


def opcoes_env():
    """opcoes.txt -> variáveis que o jogo lê"""
    o, e = opcoes_ler(), {}
    if o.get('render') in ('opengl', 'vulkan'): e['RENDER'] = o['render']
    if o.get('resolucao', '') in ('1', '2', '3', '4', '6', '9'):   # 6 = 2K, 9 = 4K
        e['RESOLUCAO'] = o['resolucao']
    if o.get('filtro', '') in ('0', '1', '2', '3'): e['FILTRO_TEXTURA'] = o['filtro']
    if o.get('modo_tela') in ('janela', 'sem_bordas', 'tela_cheia'): e['MODO_TELA'] = o['modo_tela']   # janela / sem bordas / tela cheia
    if o.get('botoes') in ('posicao', 'rotulo'): e['CONTROLE_BOTOES'] = o['botoes']
    if o.get('idioma_jogo') in ('en', 'es', 'fr', 'de', 'it', 'ja', 'ko'): e['IDIOMA_JOGO'] = o['idioma_jogo']
    if o.get('idioma') in ('pt', 'en', 'es', 'ja'): e['IDIOMA'] = o['idioma']   # idioma do programa
    if o.get('radar_cheio') in ('0', '1'): e['RADAR_CHEIO'] = o['radar_cheio']   # Poké Radar sempre carregado
    if o.get('xp') in ('1', '2', '4', '6', '8'): e['XP_MULT'] = o['xp']   # XP ganho em batalha (multiplicador)
    if o.get('nuzlocke') in ('0', '1'): e['NUZLOCKE'] = o['nuzlocke']   # Nuzlocke Mode
    if o.get('texturas') in ('0', '1'): e['TEXTURAS'] = o['texturas']
    if o.get('volume', '').isdigit(): e['VOLUME'] = o['volume']        # volume do som (0 a 100)   # texturas personalizadas ligadas/desligadas
    return e


TEXTURAS = os.path.join(RAIZ, 'texturas')       # pacotes de texturas do Citra/Azahar: texturas/<ID do jogo>/... (PNG tex1_*)


def texturas_pasta(program_id):
    """pasta do pacote de texturas deste jogo: texturas/<ID>, texturas/load/textures/<ID> (como no Citra) ou a
    própria texturas/ (pacote copiado direto nela). Cria texturas/<ID> vazia para mostrar onde colocar."""
    tid = f'{program_id:016X}'
    for c in (os.path.join(TEXTURAS, tid), os.path.join(TEXTURAS, tid.lower()),
              os.path.join(TEXTURAS, 'load', 'textures', tid), os.path.join(TEXTURAS, 'load', 'textures', tid.lower())):
        if os.path.isdir(c) and os.listdir(c):
            return c
    try:
        os.makedirs(os.path.join(TEXTURAS, tid), exist_ok=True)
        if any(not (len(a) == 16 and all(ch in '0123456789abcdefABCDEF' for ch in a)) and a != 'load' for a in os.listdir(TEXTURAS)):
            return TEXTURAS
    except OSError:
        pass
    return os.path.join(TEXTURAS, tid)


MODS = os.path.join(RAIZ, 'mods')              # cada mod numa pasta: mods/<nome>/romfs/... (LayeredFS)


def mod_romfs(pasta, program_id):
    """a pasta romfs de um mod: mods/<nome>/romfs, mods/<nome>/<titleid>/romfs ou mods/<nome>/luma/titles/<titleid>/romfs"""
    tid = f'{program_id:016x}'
    for c in (os.path.join(pasta, 'romfs'), os.path.join(pasta, tid, 'romfs'), os.path.join(pasta, tid.upper(), 'romfs'),
              os.path.join(pasta, 'luma', 'titles', tid, 'romfs'), os.path.join(pasta, 'luma', 'titles', tid.upper(), 'romfs')):
        if os.path.isdir(c):
            return c
    return None


def mods_disponiveis(program_id):
    if not os.path.isdir(MODS):
        return []
    return sorted(m for m in os.listdir(MODS)
                  if os.path.isdir(os.path.join(MODS, m)) and mod_romfs(os.path.join(MODS, m), program_id))


def mods_preparar(n, jogo, d):
    """mods ligados (opcoes.txt: mods=a|b): monta um RomFS virtual com os arquivos do .3ds e, por cima, os dos
    mods (o último da lista vence). O runtime lê os dados do .3ds ("@offset") ou do arquivo do mod."""
    try:
        os.makedirs(MODS, exist_ok=True)
        leia = os.path.join(MODS, 'LEIA-ME.txt')
        if not os.path.exists(leia):
            with open(leia, 'w', encoding='utf-8') as o:
                o.write('Mods do Pokémon X/Y (LayeredFS)\n\n'
                        'Coloque cada mod numa pasta aqui dentro, com a pasta romfs do mod dentro dela:\n'
                        '    mods/Nome do mod/romfs/a/0/...\n'
                        '(também valem os formatos mods/Nome/<id do título>/romfs e mods/Nome/luma/titles/<id>/romfs)\n\n'
                        'Depois ligue ou desligue cada mod em MODS, no menu do jogo, e reinicie.\n'
                        'Os arquivos do mod substituem os do jogo com o mesmo caminho; nada na ROM é alterado.\n'
                        'Mods que mudam o código (exefs/code.bin, .ips) ainda não são aceitos.\n')
    except OSError:
        pass
    lig = [m for m in opcoes_ler().get('mods', '').split('|') if m]
    disp = mods_disponiveis(n.program_id)
    ativos = [m for m in lig if m in disp]
    env = {'MODS_DIR': MODS, 'MODS_LISTA': '|'.join(disp), 'MODS_ATIVOS': '|'.join(ativos)}
    if not ativos:
        return env
    rf = ctr.Romfs(n)
    tam, fonte = {}, {}
    for p, off, size in rf.walk():
        tam[p] = size; fonte[p] = f'@{n.base + off}'
    trocados = novos = 0
    for m in ativos:
        raiz = mod_romfs(os.path.join(MODS, m), n.program_id)
        for base, _, arqs in os.walk(raiz):
            for a in arqs:
                full = os.path.abspath(os.path.join(base, a))
                rel = os.path.relpath(full, raiz).replace(os.sep, '/')
                if rel in tam: trocados += 1
                else: novos += 1
                tam[rel] = os.path.getsize(full); fonte[rel] = full.replace('\\', '/')
    saida = os.path.join(d, 'romfs_mods')
    romfs_build.gerar(sorted(tam.items()), saida, fonte, quiet=True)
    passo(L(f'mods ligados: {", ".join(ativos)} ({trocados} arquivos trocados, {novos} novos)',
            f'mods enabled: {", ".join(ativos)} ({trocados} files replaced, {novos} new)',
            f'mods activados: {", ".join(ativos)} ({trocados} archivos reemplazados, {novos} nuevos)',
            f'有効なMOD：{", ".join(ativos)}（置き換え {trocados} 件、新規 {novos} 件）'))
    env['ROMFS_META'] = saida
    return env


def passo(msg):
    print(f'== {msg}', flush=True)


def compilar(clang, d):
    clangxx = os.path.join(os.path.dirname(clang), os.path.basename(clang).replace('clang', 'clang++'))
    if not os.path.exists(clangxx):
        clangxx = shutil.which('clang++') or clangxx
    sdl = achar_sdl()
    src = sorted(glob.glob(os.path.join(d, 'src', '*.c')))
    os.makedirs(os.path.join(d, 'obj'), exist_ok=True)
    os.makedirs(os.path.join(d, 'rt'), exist_ok=True)
    tarefas = []
    for f in src:
        o = os.path.join(d, 'obj', os.path.basename(f)[:-2] + '.o')
        if not os.path.exists(o) or os.path.getmtime(o) < os.path.getmtime(f):
            tarefas.append(([clang, '-O1', '-w', '-I', RUNTIME, '-c', f, '-o', o], f))
    for v in glob.glob(os.path.join(d, 'rt', 'display_*.o')):
        os.remove(v)                                  # só uma janela entra no link
    for n in RT_COMMON:
        tarefas.append(([clang, '-O1', '-w', '-I', RUNTIME, '-c', os.path.join(RUNTIME, n + '.c'), '-o',
                         os.path.join(d, 'rt', n + '.o')], n))
    if sdl:
        disp, extra = 'display_sdl', sdl[0]
    else:
        disp, extra = ('display_win32' if WIN else 'display_null'), []
        print(L('   (sem SDL2: ' + ('janela simples do Windows, sem som' if WIN else 'sem janela') + ')',
                '   (no SDL2: ' + ('plain Windows window, no sound' if WIN else 'no window') + ')',
                '   (sin SDL2: ' + ('ventana simple de Windows, sin sonido' if WIN else 'sin ventana') + ')',
                '   （SDL2なし：' + ('Windowsの簡易ウィンドウ、音なし' if WIN else 'ウィンドウなし') + '）'), flush=True)
    tarefas.append(([clang, '-O2', '-fno-math-errno', '-w', '-I', RUNTIME] + extra + ['-c', os.path.join(RUNTIME, disp + '.c'), '-o',   # -O2: o cartucho do menu é desenhado em software
                     os.path.join(d, 'rt', disp + '.o')], disp))
    tarefas.append(([clang, '-O1', '-w', '-I', RUNTIME, '-I', os.path.join(RUNTIME, 'faad2', 'include'), '-c',
                     os.path.join(RUNTIME, 'dsp_aac.c'), '-o', os.path.join(d, 'rt', 'dsp_aac.o')], 'dsp_aac'))
    for n in RT_CPP:
        tarefas.append(([clangxx, '-std=c++20', '-O2', '-w', '-I', os.path.join(RUNTIME, 'audio'), '-c',
                         os.path.join(RUNTIME, n + '.cpp'), '-o', os.path.join(d, 'rt', os.path.basename(n) + '.o')], n))
    os.makedirs(os.path.join(d, 'rt', 'faad'), exist_ok=True)
    for f in sorted(glob.glob(os.path.join(RUNTIME, 'faad2', 'libfaad', '*.c'))):
        o = os.path.join(d, 'rt', 'faad', os.path.basename(f)[:-2] + '.o')
        if not os.path.exists(o):
            tarefas.append(([clang, '-O2', '-w'] + FAAD_DEFS + ['-I', os.path.join(RUNTIME, 'faad2', 'libfaad'),
                             '-I', os.path.join(RUNTIME, 'faad2', 'include'), '-c', f, '-o', o], 'faad2'))
    for n in RT_FAST:
        tarefas.append(([clang, '-O2', '-march=native', '-fno-math-errno', '-w', '-I', RUNTIME, '-c', os.path.join(RUNTIME, n + '.c'), '-o',
                         os.path.join(d, 'rt', n + '.o')], n))
    if WIN:                                       # ícone da janela do jogo (a SDL usa o 1º ícone do .exe)
        ico = os.path.join(RAIZ, 'lancador', 'icone.ico')
        windres = os.path.join(os.path.dirname(clang), 'x86_64-w64-mingw32-windres.exe')
        if os.path.exists(ico) and os.path.exists(windres):
            rc = os.path.join(d, 'rt', 'icone.rc')
            with open(rc, 'w', encoding='utf-8') as o:
                o.write('1 ICON "' + ico.replace('\\', '/') + '"\n')
            tarefas.append(([windres, rc, '-O', 'coff', '-o', os.path.join(d, 'rt', 'icone.o')], 'icone'))
    total, feitos, erros = len(tarefas), 0, []
    inicio = time.time()

    def roda(t):
        cmd, nome = t
        r = subprocess.run(cmd, capture_output=True, text=True)
        return nome, r.returncode, r.stderr

    nucleos = os.cpu_count() or 2
    with concurrent.futures.ThreadPoolExecutor(max_workers=nucleos) as ex:
        for nome, rc, err in ex.map(roda, tarefas):
            feitos += 1
            if rc != 0:
                erros.append((nome, err))
            if LANC:
                print(f'@@PROGRESSO {feitos} {total}', flush=True)
            if feitos % 10 == 0 or feitos == total:
                el = time.time() - inicio
                print(L(f'   {feitos}/{total} arquivos compilados ({el / 60:.1f} min)',
                        f'   {feitos}/{total} files compiled ({el / 60:.1f} min)',
                        f'   {feitos}/{total} archivos compilados ({el / 60:.1f} min)',
                        f'   {feitos}/{total} ファイルをコンパイル済み（{el / 60:.1f} 分）'), flush=True)
    if erros:
        with open(os.path.join(d, 'build.log'), 'w', encoding='utf-8') as o:
            for nome, err in erros:
                o.write(f'ERRO {nome}\n{err}\n')
        sys.exit(L(f'{len(erros)} arquivos não compilaram; veja {os.path.join(d, "build.log")}',
                   f'{len(erros)} files failed to compile; see {os.path.join(d, "build.log")}',
                   f'{len(erros)} archivos no compilaron; mira {os.path.join(d, "build.log")}',
                   f'{len(erros)} 個のファイルがコンパイルできませんでした。{os.path.join(d, "build.log")} を確認してください'))
    exe = os.path.join(d, 'jogo.exe' if WIN else 'jogo')
    objs = (glob.glob(os.path.join(d, 'obj', '*.o')) + glob.glob(os.path.join(d, 'rt', '*.o'))
            + glob.glob(os.path.join(d, 'rt', 'faad', '*.o')))
    # Windows: a lista de objetos é longa demais para a linha de comando; vai num arquivo de resposta
    rsp = os.path.join(d, 'objetos.rsp')
    with open(rsp, 'w') as o:
        o.write('\n'.join('"' + p.replace('\\', '/') + '"' for p in objs))
    libs = sdl[1] if sdl else []
    if WIN:
        # -static: libc++/winpthreads embutidos; a SDL2 continua como SDL2.dll ao lado do jogo.exe
        cmd = [clangxx, '@' + rsp, '-static', '-o', exe] + libs + ['-lgdi32', '-luser32', '-lpthread',
                                                                   '-Wl,--stack,16777216']
        if sdl and sdl[2] and os.path.exists(sdl[2]):
            shutil.copy2(sdl[2], d)
    else:
        cmd = [clangxx, '@' + rsp, '-rdynamic', '-o', exe] + libs + ['-lm', '-lpthread']
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(L('falha ao linkar:\n', 'link failed:\n', 'falló el enlazado:\n', 'リンクに失敗しました：\n') + r.stderr[-3000:])
    return exe


GCDB_URL = 'https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/master/gamecontrollerdb.txt'


def banco_controles():
    """banco de mapeamentos de controles da comunidade da SDL (licença zlib), em ferramentas/; baixado na primeira
    vez. Sem internet o jogo abre igual (os controles comuns a SDL já conhece; os outros ganham mapeamento automático)."""
    dest = os.path.join(RAIZ, 'ferramentas', 'gamecontrollerdb.txt')
    if not os.path.exists(dest) or time.time() - os.path.getmtime(dest) > 90 * 86400:   # atualiza a cada ~3 meses
        try:
            import urllib.request
            with urllib.request.urlopen(GCDB_URL, timeout=15) as r:
                dados = r.read()
            if b'platform:' in dados:
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with open(dest, 'wb') as o:
                    o.write(dados)
                print(L('   banco de controles atualizado', '   controller database updated', '   base de datos de mandos actualizada', '   コントローラーのデータベースを更新しました'), flush=True)
        except Exception:
            pass
    return dest if os.path.exists(dest) else ''


def extras_novos(d, exe):
    """o runtime registra em extra_funcs.txt os destinos de salto que não eram funções conhecidas; a lista de
    sementes conhecidas do projeto também pode ter sido atualizada"""
    t = os.path.getmtime(exe)
    for f in (os.path.join(d, 'extra_funcs.txt'), os.path.join(TOOLS, 'extra_funcs_conhecidas.txt')):
        if os.path.exists(f) and os.path.getmtime(f) > t:
            return True
    return False


def runtime_mudou(exe):
    t = os.path.getmtime(exe)
    for raiz, _, arqs in os.walk(RUNTIME):
        for a in arqs:
            if a.endswith(('.c', '.h', '.cpp')) and os.path.getmtime(os.path.join(raiz, a)) > t:
                return True
    ico = os.path.join(RAIZ, 'lancador', 'icone.ico')     # ícone trocado: refaz o .exe para a janela usar o novo
    return WIN and os.path.exists(ico) and os.path.getmtime(ico) > t


def recompilador_mudou(exe):
    """o tradutor ARM -> C (recomp.py) é mais novo que o executável: o código C gerado precisa ser refeito"""
    t = os.path.getmtime(exe)
    return any(os.path.exists(f) and os.path.getmtime(f) > t
               for f in (os.path.join(TOOLS, 'recomp.py'), os.path.join(TOOLS, 'recomp_game.py')))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args:
        sys.exit(__doc__)
    jogo = os.path.abspath(args[0])
    refazer = '--refazer' in sys.argv
    try:
        n = ctr.open_image(jogo)
        ctr.check(n)
    except ctr.CtrError as e:
        sys.exit(L(f'erro: {e}', f'error: {e}', f'error: {e}', f'エラー：{e}'))
    except OSError as e:
        sys.exit(L(f'não consegui abrir {jogo}: {e}', f'could not open {jogo}: {e}', f'no pude abrir {jogo}: {e}', f'{jogo} を開けませんでした：{e}'))
    d = os.path.join(RAIZ, 'jogos', f'{n.program_id:016x}')
    exe = os.path.join(d, 'jogo.exe' if WIN else 'jogo')
    if refazer or not os.path.exists(exe):
        passo(L(f'primeira execução: preparando {n.program_id:016x} em {d}',
                f'first run: preparing {n.program_id:016x} in {d}',
                f'primera ejecución: preparando {n.program_id:016x} en {d}',
                f'初回起動：{n.program_id:016x} を {d} に準備中'))
        ctr.prep(jogo, d)
        passo(L('recompilando o código ARM para C (alguns minutos)', 'recompiling the ARM code to C (a few minutes)',
                'recompilando el código ARM a C (unos minutos)', 'ARMコードをCに再コンパイル中（数分かかります）'))
        r = subprocess.run([sys.executable, os.path.join(TOOLS, 'recomp_game.py'),
                            os.path.join(d, 'code.bin'), os.path.join(d, 'romfs'), d])
        if r.returncode != 0:
            sys.exit(L('a recompilação falhou', 'recompilation failed', 'la recompilación falló', '再コンパイルに失敗しました'))
        subprocess.run([sys.executable, os.path.join(TOOLS, 'pack.py'), os.path.join(d, 'image'),
                        os.path.join(d, 'image.pack')], check=True, stdout=subprocess.DEVNULL)
        passo(L('compilando (na primeira vez demora; as próximas aberturas são imediatas)',
                'compiling (slow the first time; later launches are instant)',
                'compilando (la primera vez tarda; las siguientes aperturas son inmediatas)',
                'コンパイル中（初回は時間がかかります。次回からはすぐ起動します）'))
        compilar(achar_clang(), d)
        passo(L('pronto', 'done', 'listo', '完了'))
    elif recompilador_mudou(exe):
        passo(L('o recompilador foi corrigido: refazendo o código C do jogo (alguns minutos)',
                'the recompiler was fixed: regenerating the game C code (a few minutes)',
                'el recompilador fue corregido: rehaciendo el código C del juego (unos minutos)',
                '再コンパイラーが修正されました：ゲームのCコードを作り直しています（数分かかります）'))
        r = subprocess.run([sys.executable, os.path.join(TOOLS, 'recomp_game.py'),
                            os.path.join(d, 'code.bin'), os.path.join(d, 'romfs'), d])
        if r.returncode != 0:
            sys.exit(L('a recompilação falhou', 'recompilation failed', 'la recompilación falló', '再コンパイルに失敗しました'))
        subprocess.run([sys.executable, os.path.join(TOOLS, 'pack.py'), os.path.join(d, 'image'),
                        os.path.join(d, 'image.pack')], check=True, stdout=subprocess.DEVNULL)
        compilar(achar_clang(), d)
    elif extras_novos(d, exe):
        passo(L('o jogo pediu funções novas (extra_funcs.txt): recompilando só o que mudou',
                'the game asked for new functions (extra_funcs.txt): recompiling only what changed',
                'el juego pidió funciones nuevas (extra_funcs.txt): recompilando solo lo que cambió',
                'ゲームが新しい関数を要求しました（extra_funcs.txt）：変更された部分だけ再コンパイル中'))
        r = subprocess.run([sys.executable, os.path.join(TOOLS, 'recomp_game.py'),
                            os.path.join(d, 'code.bin'), os.path.join(d, 'romfs'), d])
        if r.returncode != 0:
            sys.exit(L('a recompilação falhou', 'recompilation failed', 'la recompilación falló', '再コンパイルに失敗しました'))
        subprocess.run([sys.executable, os.path.join(TOOLS, 'pack.py'), os.path.join(d, 'image'),
                        os.path.join(d, 'image.pack')], check=True, stdout=subprocess.DEVNULL)
        compilar(achar_clang(), d)
    elif runtime_mudou(exe):
        passo(L('o runtime foi atualizado: recompilando só ele', 'the runtime was updated: recompiling only it',
                'el runtime se actualizó: recompilando solo él', 'ランタイムが更新されました：ランタイムだけ再コンパイル中'))
        compilar(achar_clang(), d)
    with open(os.path.join(d, 'romfs.txt')) as f:
        romfs = f.read().strip()
    base = dict(os.environ)
    backup_save(d)                                  # cópia do save antes de jogar (backups/, as 5 últimas)
    while True:
        env = dict(base)
        env.update(opcoes_env())                    # escolhas do menu (opcoes.txt): valem a cada abertura
        env.pop('ROMFS_META', None)
        env.update(mods_preparar(n, jogo, d))       # mods (pasta mods/): RomFS com os arquivos trocados
        env['JOGO_3DS'] = jogo
        env['JOGO_3DS_ROMFS'] = romfs
        env.setdefault('LOG_IPC', '0')
        env.setdefault('PICA_PERFIL', '1')      # linha de desempenho a cada 300 quadros no jogo.log
        env['JOGO_VERSAO'] = {0x0004000000055D00: 'X', 0x0004000000055E00: 'Y'}.get(n.program_id, 'XY')   # título e cores
        env['CONTROLES_DB'] = banco_controles()  # mapeamentos de controles (SDL_GameControllerDB)
        env['CONTROLES_EXTRA'] = os.path.join(RAIZ, 'controles.txt')   # ajustes do jogador (opcional, formato da SDL)
        env['OPCOES'] = OPCOES                   # o menu do jogo grava as escolhas aqui
        env['CARTUCHOS'] = os.path.join(RAIZ, 'cartuchos')   # cartucho 3D do menu (malha e etiquetas)
        env['FUNDOS'] = os.path.join(RAIZ, 'fundos')         # arte de fundo do menu (fundo_x/fundo_y; opcional, do usuário)
        env['TEXTURAS_DIR'] = texturas_pasta(n.program_id)   # texturas personalizadas (opcional; nenhuma vem com o projeto)
        env['CAPTURAS'] = os.path.join(RAIZ, 'capturas')     # F12: capturas de tela
        env['ICONES'] = os.path.join(RAIZ, 'icones')         # ícone dos patins (icones/patins.png; opcional, do usuário)
        env.setdefault('MENU', '0' if '--sem-menu' in sys.argv else '1')
        if '--sem-janela' in sys.argv:
            env['SEM_JANELA'] = '1'
        passo(L('abrindo o jogo (log em ', 'opening the game (log in ', 'abriendo el juego (registro en ', 'ゲームを起動中（ログ：') + os.path.join(d, 'jogo.log') + ')')
        if LANC:
            print('@@JOGO_ABERTO', flush=True)
        with open(os.path.join(d, 'jogo.log'), 'w') as log:
            r = subprocess.run([exe, 'image.pack'], cwd=d, env=env, stderr=log)
        base.pop('RESET_DIRETO', None)
        if r.returncode == 79:                  # menu: "reiniciar jogo": abre de novo direto na tela de continuar
            passo(L('reiniciando o jogo', 'resetting the game', 'reiniciando el juego', 'ゲームをリセット中'))
            base['RESET_DIRETO'] = '1'
            continue
        if r.returncode == 77:                  # menu: "reiniciar agora" (renderizador ou idioma trocado)
            passo(L('reiniciando o jogo com as novas opções', 'restarting the game with the new settings', 'reiniciando el juego con las nuevas opciones', '新しい設定でゲームを再起動中'))
            continue
        if r.returncode == 78:                  # menu: "trocar ROM"
            sys.exit(78)
        break
    if r.returncode not in (0, None):
        try:
            rel = relatorio_erro(d, r.returncode, n, jogo)
            print(L('relatório do erro: ', 'error report: ', 'informe del error: ', 'エラーレポート：') + rel, flush=True)
            if LANC:
                print('@@RELATORIO ' + rel, flush=True)
        except Exception as e:                  # (o relatório nunca pode atrapalhar a mensagem de erro)
            print(f'   ({e})', flush=True)
        print(L(f'o jogo terminou com código {r.returncode}; veja {os.path.join(d, "jogo.log")}',
                f'the game exited with code {r.returncode}; see {os.path.join(d, "jogo.log")}',
                f'el juego terminó con código {r.returncode}; mira {os.path.join(d, "jogo.log")}',
                f'ゲームがコード {r.returncode} で終了しました。{os.path.join(d, "jogo.log")} を確認してください'), flush=True)
        sys.exit(3)


def backup_save(d):
    """Cópia do save antes de cada abertura: backups/save_<data>.zip na pasta do jogo (fica as 5 últimas)."""
    sv = os.path.join(d, 'save')
    if not os.path.isdir(sv) or not any(os.scandir(sv)):
        return
    import zipfile
    bk = os.path.join(d, 'backups')
    os.makedirs(bk, exist_ok=True)
    nome = os.path.join(bk, time.strftime('save_%Y-%m-%d_%H-%M-%S.zip'))
    try:
        with zipfile.ZipFile(nome + '.tmp', 'w', zipfile.ZIP_DEFLATED) as z:
            for raiz, _, arqs in os.walk(sv):
                for a in arqs:
                    f = os.path.join(raiz, a)
                    z.write(f, os.path.relpath(f, d))
        os.replace(nome + '.tmp', nome)
    except OSError as e:
        print(L(f'   (não consegui copiar o save: {e})', f'   (could not back up the save: {e})',
                f'   (no pude copiar la partida: {e})', f'   （セーブをバックアップできませんでした：{e}）'), flush=True)
        return
    antigos = sorted(glob.glob(os.path.join(bk, 'save_*.zip')))
    for f in antigos[:-5]:
        try:
            os.remove(f)
        except OSError:
            pass


CODIGOS = {
    3: ('instrução do processador ainda não suportada', 'processor instruction not supported yet', 'instrucción del procesador aún no soportada', '未対応のプロセッサ命令'),
    5: ('função do jogo não encontrada na recompilação', 'game function missing from the recompilation', 'función del juego ausente en la recompilación', '再コンパイルにないゲーム関数'),
    6: ('o jogo parou sozinho (svcBreak)', 'the game stopped itself (svcBreak)', 'el juego se detuvo solo (svcBreak)', 'ゲームが自ら停止しました（svcBreak）'),
    7: ('chamada de sistema não implementada', 'system call not implemented', 'llamada al sistema no implementada', '未実装のシステムコール'),
    8: ('o jogo travou (todas as threads esperando)', 'the game froze (all threads waiting)', 'el juego se congeló (todos los hilos esperando)', 'ゲームがフリーズしました（全スレッドが待機）'),
    9: ('erro fatal informado pelo jogo', 'fatal error reported by the game', 'error fatal informado por el juego', 'ゲームが報告した致命的エラー'),
    10: ('o programa caiu (acesso inválido de memória ou parecido)', 'the program crashed (invalid memory access or similar)', 'el programa se cayó (acceso inválido a memoria o similar)', 'プログラムがクラッシュしました（不正なメモリアクセスなど）'),
}


def relatorio_erro(d, codigo, n, jogo):
    """Monta erros/relatorio_<data>.txt com o que o log diz do erro (para mandar a quem for consertar)."""
    try:
        with open(os.path.join(d, 'jogo.log'), encoding='utf-8', errors='replace') as f:
            linhas = f.read().splitlines()
    except OSError:
        linhas = []
    pasta = os.path.join(d, 'erros')
    os.makedirs(pasta, exist_ok=True)
    cam = os.path.join(pasta, time.strftime('relatorio_%Y-%m-%d_%H-%M-%S.txt'))
    mods = []                                  # módulos (CRO) carregados no fim, pelo log
    for l in linhas:
        if ('LoadCRO "' in l or 'UnloadCRO "' in l) and l.count('"') >= 2:
            for m in [l.split('"')[1]]:
                if 'UnloadCRO' in l:
                    if m in mods:
                        mods.remove(m)
                elif m not in mods:
                    mods.append(m)
    sig = (L(*CODIGOS[codigo]) if codigo in CODIGOS else L('erro desconhecido', 'unknown error', 'error desconocido', '不明なエラー'))
    with open(cam, 'w', encoding='utf-8') as o:
        o.write('XY Project - ' + L('relatório de erro', 'error report', 'informe de error', 'エラーレポート') + '\n')
        o.write(time.strftime('%Y-%m-%d %H:%M:%S') + '\n\n')
        o.write(L('Jogo', 'Game', 'Juego', 'ゲーム') + f': {os.path.basename(jogo)} ({n.program_id:016X})\n')
        o.write(L('Código de saída', 'Exit code', 'Código de salida', '終了コード') + f': {codigo} - {sig}\n')
        o.write(L('Sistema', 'System', 'Sistema', 'システム') + f': {sys.platform}, Python {sys.version.split()[0]}\n\n')
        imp = [l for l in linhas if l.startswith(('[crash]', '[err:f]', '[kernel]', '[fatal]')) or 'svcBreak' in l or 'não suportada' in l or 'não encontrada' in l]
        if imp:
            o.write('== ' + L('o que o log diz do erro', 'what the log says about the error', 'lo que dice el registro del error', 'ログのエラー情報') + ' ==\n')
            o.write('\n'.join(imp[-60:]) + '\n\n')
        if mods:
            o.write('== ' + L('módulos carregados (pelo log)', 'loaded modules (from the log)', 'módulos cargados (según el registro)', '読み込まれたモジュール（ログより）') + ' ==\n' + ', '.join(mods) + '\n\n')
        o.write('== ' + L('últimas linhas do log', 'last log lines', 'últimas líneas del registro', 'ログの最後の行') + ' ==\n')
        o.write('\n'.join(linhas[-150:]) + '\n')
    for f in sorted(glob.glob(os.path.join(pasta, 'relatorio_*.txt')))[:-10]:   # fica os 10 últimos
        try:
            os.remove(f)
        except OSError:
            pass
    return cam


if __name__ == '__main__':
    main()

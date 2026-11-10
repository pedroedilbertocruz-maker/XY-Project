# Baixa as ferramentas que o jogar.bat precisa, numa pasta "ferramentas" ao lado deste arquivo
# (nada é instalado no sistema): Python portátil (python.org), o compilador llvm-mingw (clang)
# e a SDL2 (janela, controle e som).
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$raiz = Split-Path -Parent $MyInvocation.MyCommand.Path
$en = $env:IDIOMA -and $env:IDIOMA -ne 'pt'   # o lançador passa IDIOMA=pt/en/es/ja: fora do português, mensagens em inglês
function Msg($pt, $en_) { if ($en) { Write-Host $en_ } else { Write-Host $pt } }
$ferr = Join-Path $raiz 'ferramentas'
New-Item -ItemType Directory -Force $ferr | Out-Null

$py = Join-Path $ferr 'python\python.exe'
if (!(Test-Path $py)) {
    Msg 'Baixando o Python portatil (cerca de 11 MB)...' 'Downloading portable Python (about 11 MB)...'
    $zip = Join-Path $ferr 'python.zip'
    Invoke-WebRequest 'https://www.python.org/ftp/python/3.12.7/python-3.12.7-embed-amd64.zip' -OutFile $zip -UseBasicParsing
    Expand-Archive $zip (Join-Path $ferr 'python') -Force
    Remove-Item $zip
}

$clang = Join-Path $ferr 'llvm-mingw\bin\clang.exe'
if (!(Test-Path $clang)) {
    $ver = '20240619'
    Msg 'Baixando o compilador llvm-mingw (cerca de 170 MB; pode demorar)...' 'Downloading the llvm-mingw compiler (about 170 MB; this may take a while)...'
    $zip = Join-Path $ferr 'llvm.zip'
    Invoke-WebRequest "https://github.com/mstorsjo/llvm-mingw/releases/download/$ver/llvm-mingw-$ver-ucrt-x86_64.zip" -OutFile $zip -UseBasicParsing
    Msg 'Descompactando...' 'Extracting...'
    Expand-Archive $zip $ferr -Force
    Remove-Item $zip
    $dir = Join-Path $ferr "llvm-mingw-$ver-ucrt-x86_64"
    if (Test-Path (Join-Path $ferr 'llvm-mingw')) { Remove-Item -Recurse -Force (Join-Path $ferr 'llvm-mingw') }
    Rename-Item $dir 'llvm-mingw'
}
$sdl = Join-Path $ferr 'SDL2\x86_64-w64-mingw32\lib\libSDL2.dll.a'
if (!(Test-Path $sdl)) {
    $ver = '2.30.9'
    Msg 'Baixando a SDL2 (janela, controle e som; cerca de 15 MB)...' 'Downloading SDL2 (window, controller and sound; about 15 MB)...'
    $zip = Join-Path $ferr 'sdl.zip'
    Invoke-WebRequest "https://github.com/libsdl-org/SDL/releases/download/release-$ver/SDL2-devel-$ver-mingw.zip" -OutFile $zip -UseBasicParsing
    Expand-Archive $zip $ferr -Force
    Remove-Item $zip
    if (Test-Path (Join-Path $ferr 'SDL2')) { Remove-Item -Recurse -Force (Join-Path $ferr 'SDL2') }
    Rename-Item (Join-Path $ferr "SDL2-$ver") 'SDL2'
}
$db = Join-Path $ferr 'gamecontrollerdb.txt'
if (!(Test-Path $db)) {
    # banco de controles da comunidade da SDL (licença zlib); sem ele o jogo funciona igual
    try { Invoke-WebRequest 'https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/master/gamecontrollerdb.txt' -OutFile $db -UseBasicParsing } catch { }
}
Msg 'Ferramentas prontas.' 'Tools ready.'

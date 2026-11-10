@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"
rem Pokemon XY - port nativo (Pokemon X e Y). Arraste o .3ds DECIFRADO para cima deste arquivo (ou de dois cliques).
if not exist "ferramentas\python\python.exe" goto instalar
if not exist "ferramentas\llvm-mingw\bin\clang.exe" goto instalar
if not exist "ferramentas\SDL2\x86_64-w64-mingw32\lib\libSDL2.dll.a" goto instalar
goto escolher
:instalar
echo Primeira vez: baixando as ferramentas (Python portatil, compilador e SDL2)...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0instalar.ps1"
if errorlevel 1 goto falhou
:escolher
set "JOGO="
if not "%~1"=="" (set "JOGO=%~1" & goto temjogo)
if exist "jogo.txt" (set /p JOGO=<jogo.txt & goto temjogo)
goto pedir
:pedir
echo.
echo Arraste o arquivo .3ds do jogo para esta janela e aperte Enter:
set /p "JOGO="
  
:temjogo
if not defined JOGO goto pedir
set JOGO=%JOGO:"=%
if not defined JOGO goto pedir
if exist "%JOGO%" goto existe
echo Arquivo nao encontrado: "%JOGO%"
del jogo.txt 2>nul
goto pedir
:existe
>jogo.txt echo %JOGO%
set PYTHONUTF8=1
"ferramentas\python\python.exe" "tools\jogar.py" "%JOGO%"
pause
exit /b 0
:falhou
echo Nao consegui baixar as ferramentas. Verifique a internet e tente de novo.
pause
exit /b 1

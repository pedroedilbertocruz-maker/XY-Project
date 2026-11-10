@echo off
rem Recompila o lancador (XY Project.exe) com o llvm-mingw baixado pelo instalar.ps1.
setlocal
cd /d "%~dp0"
set "M=%~dp0..\ferramentas\llvm-mingw\bin"
if not exist "%M%\clang.exe" (echo Rode o jogar.bat ou o instalar.ps1 primeiro para baixar o compilador. & pause & exit /b 1)
"%M%\x86_64-w64-mingw32-windres.exe" lancador.rc -O coff -o lancador.res || goto erro
"%M%\clang.exe" -O2 -Wall -municode -mwindows -static lancador.c lancador.res -lcomdlg32 -lshell32 -lole32 -lwinmm -s -o "..\XY Project.exe" || goto erro
del lancador.res
echo Pronto: XY Project.exe
pause
exit /b 0
:erro
echo Falhou.
pause
exit /b 1

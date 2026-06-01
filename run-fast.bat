@chcp 65001 >nul 2>&1
@echo off
REM Fast Windows launcher for AMD Zen 5 CPUs with MSYS2 UCRT64 GCC.
REM Prerequisite: MSYS2 with mingw-w64-ucrt-x86_64-gcc installed.
cd /d "%~dp0"

if not exist "C:\msys64\ucrt64\bin\g++.exe" (
    echo [run-fast] g++ not found.
    echo [run-fast] Install MSYS2, then open "MSYS2 UCRT64" and run:
    echo [run-fast]   pacman -Syu
    echo [run-fast]   pacman -S mingw-w64-ucrt-x86_64-gcc
    pause
    exit /b 1
)

echo [run-fast] Compiler: MSYS2 UCRT64 g++
echo [run-fast] GCC arch: znver5
call "%~dp0run.bat" %*

@echo off
REM ============================================================
REM PBB 名字评分测号器 — 一键启动 (Windows)
REM
REM 要求: Python 3.8+ + C++ 编译器 (Visual Studio Build Tools 或 MinGW)
REM 自动:
REM   1. 检查 Python 和 C++ 编译器
REM   2. 创建虚拟环境 .venv (首次)
REM   3. 安装 Python 依赖到 .venv
REM   4. 启动 main.py
REM ============================================================
cd /d "%~dp0"

REM ── 检查 Python ──
where python >nul 2>&1
if %errorlevel% neq 0 (
    echo [run] Python not found. Please install Python 3.8+:
    echo   https://www.python.org/downloads/
    pause
    exit /b 1
)

REM ── 检查 C++ 编译器 ──
set HAS_CC=0
where cl    >nul 2>&1 && set HAS_CC=1
where g++   >nul 2>&1 && set HAS_CC=1
where clang++ >nul 2>&1 && set HAS_CC=1
if %HAS_CC%==0 (
    echo [run] No C++ compiler found. Please install one of:
    echo   Visual Studio Build Tools: https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022
    echo     ^(Select "Desktop development with C++" workload^)
    echo   or MinGW-w64: https://www.mingw-w64.org/
    pause
    exit /b 1
)
echo [run] Compiler found (cl / g++ / clang++)

REM ── 虚拟环境 ──
if not exist ".venv\Scripts\python.exe" (
    echo [run] Creating virtual environment...
    python -m venv .venv
    if %errorlevel% neq 0 (
        echo ERROR: venv creation failed
        pause
        exit /b 1
    )
    echo [run] Installing Python dependencies...
    .venv\Scripts\pip install --quiet pyyaml setuptools pybind11
    if %errorlevel% neq 0 (
        echo WARNING: pip install failed. Try setting mirror:
        echo   pip config set global.index-url https://mirrors.sjtug.sjtu.edu.cn/pypi/web/simple
    )
    REM Python ^< 3.11 needs tomli
    python -c "import sys; sys.exit(0 if sys.version_info >= (3,11) else 1)" 2>nul
    if %errorlevel% neq 0 .venv\Scripts\pip install --quiet tomli
)

REM ── 启动 ──
.venv\Scripts\python main.py %*
if %errorlevel% neq 0 pause

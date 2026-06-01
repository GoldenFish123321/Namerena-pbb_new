@echo off
chcp 65001 >nul 2>&1
REM PBB Name Scoring Tester — One-click launcher (Windows)
cd /d "%~dp0"

REM ---- Find Python (uv first, then PATH) ----
set PYTHON=
where uv >nul 2>&1 && set HAS_UV=1

if defined HAS_UV (
    for /f "delims=" %%i in ('uv python find 2^>nul') do set PYTHON=%%i
)
if not defined PYTHON (
    where python  >nul 2>&1 && set PYTHON=python
    where python3 >nul 2>&1 && set PYTHON=python3
)

if not defined PYTHON (
    if defined HAS_UV (
        echo [run] Installing Python 3.13 via uv ...
        uv python install 3.13
        for /f "delims=" %%i in ('uv python find 3.13') do set PYTHON=%%i
    ) else (
        echo [run] Python not found. Install one of:
        echo   uv: https://docs.astral.sh/uv/
        echo   Python: https://www.python.org/downloads/
        pause
        exit /b 1
    )
)
echo [run] Python: %PYTHON%

REM ---- Check C++ compiler ----
set HAS_CC=0
where cl      >nul 2>&1 && set HAS_CC=1
where g++     >nul 2>&1 && set HAS_CC=1
where clang++ >nul 2>&1 && set HAS_CC=1
if %HAS_CC%==0 (
    echo [run] No C++ compiler found. Install:
    echo   VS Build Tools: https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022
    echo   MinGW-w64: https://www.mingw-w64.org/
    pause
    exit /b 1
)
echo [run] Compiler found

REM ---- Virtual environment ----
if not exist ".venv\Scripts\python.exe" (
    echo [run] Creating .venv ...
    if defined HAS_UV (
        uv venv .venv --python "%PYTHON%"
    ) else (
        "%PYTHON%" -m venv .venv
    )
    if %errorlevel% neq 0 (
        echo ERROR: venv creation failed
        pause
        exit /b 1
    )
    echo [run] Installing deps ...
    if defined HAS_UV (
        uv pip install --python .venv\Scripts\python.exe pyyaml setuptools pybind11 tomli
    ) else (
        .venv\Scripts\pip install --quiet pyyaml setuptools pybind11
        python -c "import sys; sys.exit(0 if sys.version_info >= (3,11) else 1)" 2>nul
        if %errorlevel% neq 0 .venv\Scripts\pip install --quiet tomli
    )
    if %errorlevel% neq 0 (
        echo WARNING: pip install failed. Try mirror:
        echo   pip config set global.index-url https://mirrors.sjtug.sjtu.edu.cn/pypi/web/simple
    )
)

REM ---- Run ----
.venv\Scripts\python main.py %*
if %errorlevel% neq 0 pause

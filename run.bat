@chcp 65001 >nul 2>&1
@echo off
REM PBB Name Scoring Tester — One-click launcher (Windows)
REM Options: -y  skip all confirmations (CI/automation)
cd /d "%~dp0"

REM ---- Parse -y flag ----
set YES=0
set MAIN_ARGS=
:parse_args
if "%~1"=="" goto :done_args
if "%~1"=="-y" (set YES=1) else (set MAIN_ARGS=%MAIN_ARGS% %1)
shift
goto :parse_args
:done_args

REM ---- Intel oneAPI (auto-source if installed) ----
for %%d in ("C:\Program Files (x86)\Intel\oneAPI" "C:\Program Files\Intel\oneAPI") do (
    if exist "%%~d\setvars.bat" (
        echo [run] Intel oneAPI found, loading ...
        call "%%~d\setvars.bat" >nul 2>&1
    )
)

REM ---- Find Python ----
set PYTHON=
where uv >nul 2>&1 && set HAS_UV=1

if defined HAS_UV (
    for /f "delims=" %%i in ('uv python find --no-project 2^>nul') do (
        if not defined PYTHON set PYTHON=%%i
    )
)
if not defined PYTHON where python  >nul 2>&1 && set PYTHON=python
if not defined PYTHON where python3 >nul 2>&1 && set PYTHON=python3

if not defined PYTHON (
    if defined HAS_UV (
        echo [run] Installing Python 3.13 via uv ...
        uv python install 3.13
        for /f "delims=" %%i in ('uv python find 3.13') do set PYTHON=%%i
    )
    if not defined PYTHON (
        echo [run] Python not found.
        pause
        exit /b 1
    )
)
echo [run] Python: %PYTHON%

REM ---- AMD Zen5: MSYS2 UCRT64 GCC (auto-detect, priority over PATH g++) ----
if exist "C:\msys64\ucrt64\bin\g++.exe" (
    echo [run] Zen5: MSYS2 UCRT64 g++ detected
)

REM ---- Check C++ compiler ----
where cl      >nul 2>&1 && set HAS_CC=1
where g++     >nul 2>&1 && set HAS_CC=1
where clang++ >nul 2>&1 && set HAS_CC=1
if not defined HAS_CC (
    echo [run] No C++ compiler found.
    pause
    exit /b 1
)
echo [run] Compiler found

REM ---- Virtual environment ----
if not exist ".venv\Scripts\python.exe" (
    echo [run] Creating .venv ...
    if defined HAS_UV (
        uv venv .venv --python "%PYTHON%"
        if errorlevel 1 (
            echo ERROR: venv creation failed
            pause
            exit /b 1
        )
    ) else (
        "%PYTHON%" -m venv .venv
        if errorlevel 1 (
            echo ERROR: venv creation failed
            pause
            exit /b 1
        )
    )
)

REM Ensure deps every run (pip handles already-installed)
if defined HAS_UV (
    uv pip install --python .venv\Scripts\python.exe pyyaml setuptools pybind11 tomli >nul 2>&1
) else (
    .venv\Scripts\python -m pip install --quiet pyyaml setuptools pybind11 >nul 2>&1
    python -c "import sys; sys.exit(0 if sys.version_info >= (3,11) else 1)" 2>nul
    if errorlevel 1 .venv\Scripts\python -m pip install --quiet tomli >nul 2>&1
)

REM ---- Run ----
.venv\Scripts\python main.py %MAIN_ARGS%
if errorlevel 1 pause

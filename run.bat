@echo off
REM ============================================================
REM PBB 名字评分测号器 — 一键启动 (Windows)
REM
REM 自动完成:
REM   1. 创建虚拟环境 .venv (首次)
REM   2. 安装 Python 依赖到 .venv
REM   3. 启动 python main.py %*
REM ============================================================
cd /d "%~dp0"

REM ── 虚拟环境 (首次创建) ──
if not exist ".venv\Scripts\python.exe" (
    echo [run] Creating virtual environment...
    python -m venv .venv
    .venv\Scripts\pip install --quiet pyyaml pybind11 setuptools
    REM Python ^< 3.11 需要 tomli
    python -c "import sys; sys.exit(0 if sys.version_info >= (3,11) else 1)" 2>nul
    if errorlevel 1 .venv\Scripts\pip install --quiet tomli
)

REM ── 启动 ──
.venv\Scripts\python main.py %*

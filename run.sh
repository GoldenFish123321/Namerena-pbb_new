#!/bin/sh
# ============================================================
# PBB 名字评分测号器 — 一键启动 (Linux / Termux)
#
# 自动完成:
#   1. 检测并安装系统依赖: python3, g++, python3-venv
#   2. 创建虚拟环境 .venv (首次)
#   3. 安装 Python 依赖到 .venv
#   4. 启动 main.py
#
# 选项: -y 跳过所有确认 (CI/Docker 用)
# ============================================================
set -e

# ── -y 自动确认 ──
_yes=false
_main_args=""
for _a in "$@"; do
    case "$_a" in -y) _yes=true ;; *) _main_args="$_main_args $_a" ;; esac
done
set -- $_main_args

_confirm() {
    $_yes && return 0
    printf "%s (y/n) " "$1"
    read -r _ans
    [ "$_ans" = "y" ] || [ "$_ans" = "Y" ]
}

# ── 包管理器检测 ──
_is_termux=false
if [ -d /data/data/com.termux ]; then
    _is_termux=true
    _pkg="pkg install -y"
    _gcc_pkg="clang"
elif command -v apt >/dev/null 2>&1; then
    if [ "$(id -u)" = "0" ]; then
        _pkg="apt install -y"       # Docker/root: no sudo
    else
        _pkg="sudo apt install -y"
    fi
    _gcc_pkg="g++"
elif command -v dnf >/dev/null 2>&1; then
    _pkg="sudo dnf install -y"
    _gcc_pkg="gcc-c++"
elif command -v pacman >/dev/null 2>&1; then
    _pkg="sudo pacman -S --noconfirm"
    _gcc_pkg="gcc"
else
    echo "ERROR: 无法检测包管理器 (支持 apt/dnf/pacman/pkg)" >&2
    exit 1
fi
# ── Python ──
if ! command -v python3 >/dev/null 2>&1; then
    echo "[run] python3 not found."
    if _confirm "Install python3?"; then
        $_pkg python3
    else
        echo "[run] python3 required, aborting."; exit 1
    fi
fi

# ── C++ 编译器 ──
if ! command -v g++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1; then
    echo "[run] No C++ compiler found (g++/clang++)."
    if _confirm "Install $_gcc_pkg?"; then
        $_pkg $_gcc_pkg
    else
        echo "[run] C++ compiler required, aborting."; exit 1
    fi
fi

# ── python3-venv (Debian/Ubuntu 不随 python3 安装) ──
if ! python3 -c "import venv" 2>/dev/null; then
    echo "[run] python3-venv not found."
    if _confirm "Install python3-venv?"; then
        $_is_termux || $_pkg python3-venv 2>/dev/null || true
    else
        echo "[run] Skipping, venv will use system python3 as fallback"
    fi
fi

cd "$(dirname "$0")"

# ── Intel oneAPI (自动加载环境) ──
for _dir in "/opt/intel/oneapi" "$HOME/intel/oneapi"; do
    if [ -f "$_dir/setvars.sh" ]; then
        echo "[run] Intel oneAPI found, loading ..."
        . "$_dir/setvars.sh" >/dev/null 2>&1 || true
        break
    fi
done

# ── 虚拟环境 ──
_use_venv=true
if [ ! -f .venv/bin/python3 ]; then
    echo "[run] Creating virtual environment..."
    python3 -m venv .venv 2>/dev/null || {
        echo "[run] venv failed (python3-venv not installed), using system python3"
        echo "[run]   Tip: apt install python3-venv  to enable venv"
        _use_venv=false
    }
fi

if $_use_venv; then
    _python=".venv/bin/python3"
    _pip=".venv/bin/python3 -m pip install --quiet"
else
    if _confirm "Install pyyaml,pybind11 to system Python?"; then
        _python="python3"
        _pip="python3 -m pip install --quiet --break-system-packages"
    else
        _python="python3"
        _pip="echo '[run] Skipping system pip install'"
    fi
fi

# ── pip (Python 包管理器) ──
if ! python3 -m pip --version >/dev/null 2>&1; then
    echo "[run] pip not available."
    if _confirm "Install python3-pip?"; then
        $_pkg python3-pip
    else
        echo "[run] pip required for Python deps, aborting."; exit 1
    fi
fi

# 安装依赖
$_pip pyyaml setuptools pybind11 2>/dev/null || {
    echo "WARNING: pip install 失败, 请检查网络" >&2
}
_py_minor=$(python3 -c "import sys; print(sys.version_info.minor)")
if [ "$_py_minor" -lt 11 ]; then
    $_pip tomli 2>/dev/null || true
fi

# ── 启动 ──
exec $_python main.py "$@"

#!/bin/sh
# ============================================================
# PBB 名字评分测号器 — 一键启动 (Linux / Termux)
#
# 自动完成:
#   1. 检测并安装系统依赖: python3, g++, python3-venv
#   2. 创建虚拟环境 .venv (首次)
#   3. 安装 Python 依赖到 .venv
#   4. 启动 main.py
# ============================================================
set -e

# ── 包管理器检测 ──
_is_termux=false
if [ -d /data/data/com.termux ]; then
    _is_termux=true
    _pkg="pkg install -y"
    _gcc_pkg="clang"           # Termux 用 clang
elif command -v apt >/dev/null 2>&1; then
    _pkg="sudo apt install -y"
    _gcc_pkg="g++"
elif command -v dnf >/dev/null 2>&1; then
    _pkg="sudo dnf install -y"
    _gcc_pkg="gcc-c++"
elif command -v pacman >/dev/null 2>&1; then
    _pkg="sudo pacman -S --noconfirm"
    _gcc_pkg="gcc"
else
    echo "ERROR: 无法检测包管理器 (支持 apt/dnf/pacman/pkg)" >&2
    echo "  请手动安装: python3, g++, python3-venv" >&2
    exit 1
fi

# ── Python ──
if ! command -v python3 >/dev/null 2>&1; then
    echo "[run] Installing python3..."
    $_pkg python3
fi

# ── C++ 编译器 ──
if ! command -v g++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1; then
    echo "[run] Installing C++ compiler..."
    $_pkg $_gcc_pkg
fi

# ── python3-venv (Debian/Ubuntu 不随 python3 安装) ──
if ! python3 -c "import venv" 2>/dev/null; then
    echo "[run] Installing python3-venv..."
    $_is_termux || $_pkg python3-venv 2>/dev/null || true
fi

cd "$(dirname "$0")"

# ── 虚拟环境 ──
if [ ! -f .venv/bin/python3 ]; then
    echo "[run] Creating virtual environment..."
    python3 -m venv .venv || {
        echo "ERROR: venv 创建失败, 请手动执行: python3 -m venv .venv" >&2
        exit 1
    }
    # 安装 Python 依赖
    _pip=".venv/bin/pip install --quiet"
    $_pip pyyaml setuptools pybind11  || {
        echo "WARNING: pip install 失败, 请检查网络 (国内可设镜像: pip config set global.index-url https://mirrors.sjtug.sjtu.edu.cn/pypi/web/simple)" >&2
    }
    # Python < 3.11 需要 tomli
    if [ "$(python3 -c 'import sys; print(sys.version_info.minor)')" -lt 11 ]; then
        $_pip tomli 2>/dev/null || true
    fi
fi

# ── 启动 ──
exec .venv/bin/python3 main.py "$@"

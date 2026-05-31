#!/bin/sh
# ============================================================
# PBB 名字评分测号器 — 一键启动 (Linux / Termux)
#
# 自动完成:
#   1. 检测 Termux → 装 python + clang
#   2. 创建虚拟环境 .venv (首次)
#   3. 安装 Python 依赖到 .venv
#   4. 启动 python3 main.py "$@"
# ============================================================
set -e

# ── Termux 环境: 自动装系统包 ──
if [ -d /data/data/com.termux ]; then
    echo "[run] Termux detected"
    command -v python3 >/dev/null 2>&1 || pkg install -y python
    command -v g++      >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1 || pkg install -y clang
elif ! command -v python3 >/dev/null 2>&1; then
    echo "ERROR: python3 not found" >&2
    exit 1
fi

cd "$(dirname "$0")"

# ── 虚拟环境 (首次创建) ──
if [ ! -d .venv ]; then
    echo "[run] Creating virtual environment..."
    python3 -m venv .venv
    .venv/bin/pip install --quiet pyyaml pybind11 setuptools
    # Python < 3.11 需要 tomli, >= 3.11 内置 tomllib
    if [ "$(python3 -c 'import sys; print(sys.version_info.minor)')" -lt 11 ]; then
        .venv/bin/pip install --quiet tomli
    fi
fi

# ── 启动 ──
exec .venv/bin/python3 main.py "$@"

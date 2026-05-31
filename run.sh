#!/bin/sh
# PBB 一键启动 — Linux / Termux

# Detect Termux
if [ -d /data/data/com.termux ]; then
    echo "[run] Termux detected"
    command -v python3 >/dev/null 2>&1 || pkg install -y python
    command -v g++ >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1 || pkg install -y clang
elif ! command -v python3 >/dev/null 2>&1; then
    echo "ERROR: python3 not found"
    exit 1
fi

cd "$(dirname "$0")"
exec python3 main.py "$@"

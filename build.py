#!/usr/bin/env python3
"""PBB build script — compile pbb_core (.so/.pyd) + pbb_engine (executable)."""
import os, sys, glob, subprocess, shutil

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
REQUIRED_PY = (3, 8)


def check_python():
    if sys.version_info < REQUIRED_PY:
        print(f"ERROR: Python {REQUIRED_PY[0]}.{REQUIRED_PY[1]}+ required", file=sys.stderr)
        sys.exit(1)


def check_deps():
    missing = []
    try: import yaml
    except ImportError: missing.append("pyyaml")
    try: import setuptools
    except ImportError: missing.append("setuptools")
    try: import pybind11
    except ImportError: missing.append("pybind11")
    if sys.version_info < (3, 11):
        try: import tomli
        except ImportError: missing.append("tomli")
    if missing:
        print(f"ERROR: missing deps: {', '.join(missing)}", file=sys.stderr)
        print("  Use run.sh or run.bat to auto-install", file=sys.stderr)
        sys.exit(1)


def _engine_bin():
    name = "pbb_engine" + (".exe" if sys.platform == "win32" else "")
    return os.path.join(BASE_DIR, name)


def _pbb_core_exists():
    return bool(glob.glob(os.path.join(BASE_DIR, "pbb_core*.so")) or
                glob.glob(os.path.join(BASE_DIR, "pbb_core*.pyd")))


def _compile_engine():
    src = os.path.join(BASE_DIR, "engine_main.cpp")
    out = _engine_bin()
    inc = os.path.join(BASE_DIR, "src")

    if sys.platform == "win32":
        if shutil.which("g++"):
            cmd = ["g++", "-std=c++17", "-O3", "-funroll-loops", "-ffast-math",
                   f"-I{inc}", "-o", out, src]
        elif shutil.which("cl"):
            cmd = ["cl", "/std:c++17", "/Ox", "/EHsc", "/utf-8", "/w",
                   f"/I{inc}", f"/Fe:{out}", src]
        else:
            print("ERROR: No C++ compiler found", file=sys.stderr)
            sys.exit(1)
    else:
        flags = ["-std=c++17", "-O3", "-funroll-loops", "-ffast-math"]
        try:
            if "avx2" in open("/proc/cpuinfo").read().lower():
                flags += ["-mavx2", "-mfma"]
        except: pass
        cmd = ["g++"] + flags + [f"-I{inc}", "-o", out, src]

    print(f"[build] {' '.join(cmd)}", file=sys.stderr)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"[build] FAILED:\n{r.stderr}", file=sys.stderr)
        sys.exit(1)


def ensure_all(rebuild=False):
    check_python()
    check_deps()
    os.chdir(BASE_DIR)

    if rebuild or not _pbb_core_exists():
        print("[build] pbb_core ...", file=sys.stderr)
        subprocess.run([sys.executable, os.path.join(BASE_DIR, "setup.py"),
                        "build_ext", "--inplace"], check=True)

    if rebuild or not os.path.exists(_engine_bin()):
        _compile_engine()

    return _engine_bin()

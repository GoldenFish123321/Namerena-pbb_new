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
                glob.glob(os.path.join(BASE_DIR, "pbb_core*.pyd")) or
                glob.glob(os.path.join(BASE_DIR, "build/lib*", "pbb_core*")))


def _find_compiler():
    """返回 (compiler_name, flags) 或 None."""
    # icpx: 检查 PATH + oneAPI 默认路径
    icpx = shutil.which("icpx")
    if not icpx:
        for d in [r"C:\Program Files (x86)\Intel\oneAPI", r"C:\Program Files\Intel\oneAPI",
                  "/opt/intel/oneapi"]:
            for ver in ["latest", "2026.0", "2025.0"]:
                p = os.path.join(d, "compiler", ver, "bin",
                                 "icpx.exe" if sys.platform == "win32" else "icpx")
                if os.path.exists(p): icpx = p; break
    if icpx:
        flags = ["-std=c++17", "-w", "-O3", "-ipo", "-ffast-math",
                 "-funroll-loops", "-qopt-mem-layout-trans=4", "-qopt-prefetch=5"]
        if sys.platform == "win32":
            flags += ["-xCORE-AVX2", "-qopenmp"]
        else:
            flags += ["-xHost", "-finline-functions", "-lpthread"]
        return ("icpx", flags)

    # g++
    if shutil.which("g++"):
        flags = ["-std=c++17", "-O3", "-funroll-loops", "-ffast-math"]
        if sys.platform != "win32":
            try:
                if "avx2" in open("/proc/cpuinfo").read().lower():
                    flags += ["-mavx2", "-mfma"]
            except: pass
        return ("g++", flags)

    # MSVC
    if sys.platform == "win32" and shutil.which("cl"):
        return ("cl", ["/std:c++17", "/Ox", "/EHsc", "/utf-8", "/w"])

    return None


def _compile_engine():
    cc = _find_compiler()
    if not cc:
        print("ERROR: No C++ compiler found (icpx/g++/cl)", file=sys.stderr)
        sys.exit(1)

    name, flags = cc
    src = os.path.join(BASE_DIR, "engine_main.cpp")
    out = _engine_bin()
    inc = os.path.join(BASE_DIR, "src")

    if name == "cl":
        cmd = ["cl"] + flags + [f"/I{inc}", f"/Fe:{out}", src]
    else:
        cmd = [name] + flags + [f"-I{inc}", "-o", out, src]

    print(f"[build] {name}: {' '.join(cmd)}", file=sys.stderr)
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
        from pybind11.setup_helpers import Pybind11Extension, build_ext
        from setuptools import setup

        cc = _find_compiler()
        # pbb_core 桥接层, 性能不敏感. setup() 在 Windows 上固定用 MSVC,
        # 所以始终给 MSVC 旗标 (或 icpx/g++ 的 GNU 旗标在 Linux 上).
        if sys.platform == "win32":
            flags = ["/std:c++17", "/Ox", "/utf-8", "/w"]
        else:
            flags = ["-std=c++17", "-O3", "-funroll-loops", "-ffast-math"]

        ext = Pybind11Extension("pbb_core", ["src/bridge.cpp"],
            cxx_std=17, include_dirs=["src"],
            extra_compile_args=flags)
        setup(name="pbb_core", version="1.0.0",
              ext_modules=[ext], cmdclass={"build_ext": build_ext},
              script_args=["build_ext", "--inplace"])

    if rebuild or not os.path.exists(_engine_bin()):
        _compile_engine()

    return _engine_bin()

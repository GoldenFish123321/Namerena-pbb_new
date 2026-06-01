#!/usr/bin/env python3
"""PBB build script — compile pbb_core (.so/.pyd) + pbb_engine (executable)."""
import os, sys, glob, subprocess, shutil, sysconfig

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


def _pbb_core_path():
    """pbb_core 输出路径 (pbb_core<ext>)"""
    ext = sysconfig.get_config_var("EXT_SUFFIX") or (".pyd" if sys.platform == "win32" else ".so")
    return os.path.join(BASE_DIR, "pbb_core" + ext)


def _pbb_core_exists():
    return bool(glob.glob(os.path.join(BASE_DIR, "pbb_core*.so")) or
                glob.glob(os.path.join(BASE_DIR, "pbb_core*.pyd")))


def _find_compiler():
    """返回 (compiler_name, flags, is_msvc) 或 None."""
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
        return (icpx, flags, False)

    if shutil.which("g++"):
        flags = ["-std=c++17", "-O3", "-funroll-loops", "-ffast-math"]
        if sys.platform != "win32":
            try:
                if "avx2" in open("/proc/cpuinfo").read().lower():
                    flags += ["-mavx2", "-mfma"]
            except: pass
        return ("g++", flags, False)

    if sys.platform == "win32" and shutil.which("cl"):
        return ("cl", ["/std:c++17", "/Ox", "/EHsc", "/utf-8", "/w"], True)

    return None


def _py_include():
    """Python C API include dirs."""
    inc = sysconfig.get_config_var("INCLUDEPY")
    if inc: return [inc]
    # fallback
    return [os.path.join(sys.prefix, "include")]


def _py_link_flags():
    """Python linkage."""
    if sys.platform == "win32":
        ver = f"{sys.version_info.major}{sys.version_info.minor}"
        libdir = sysconfig.get_config_var("LIBDIR") or os.path.join(sys.prefix, "libs")
        lib = os.path.join(libdir, f"python{ver}.lib")
        return [lib] if os.path.exists(lib) else [f"/LIBPATH:{libdir}", f"python{ver}.lib"]
    libdir = sysconfig.get_config_var("LIBDIR") or os.path.join(sys.prefix, "lib")
    ldflags = sysconfig.get_config_var("LDFLAGS") or ""
    return [f"-L{libdir}"] + ldflags.split()


def _compile(name, flags, is_msvc, src, out, extra_includes=[], extra_link=[],
             shared=False):
    """统一的编译函数."""
    if is_msvc:
        cmd = [name] + flags
        for d in extra_includes:
            cmd.append(f"/I{d}")
        if shared:
            cmd.append("/LD")
        cmd.append(src)
        if shared:
            cmd += ["/link"] + extra_link + [f"/OUT:{out}"]
        else:
            cmd += [f"/Fe:{out}"]
    else:
        cmd = [name] + flags
        for d in extra_includes:
            cmd += [f"-I{d}"]
        if shared:
            cmd += ["-shared"]
            if sys.platform != "win32":
                cmd += ["-fPIC"]
        cmd += ["-o", out, src]
        cmd += extra_link
    return cmd


def _compile_pbb_core():
    """直接用 _find_compiler() 编译 bridge.cpp → pbb_core.{so,pyd}."""
    import pybind11
    cc = _find_compiler()
    if not cc:
        print("ERROR: No C++ compiler found", file=sys.stderr)
        sys.exit(1)

    name, flags, is_msvc = cc
    src = os.path.join(BASE_DIR, "src", "bridge.cpp")
    out = _pbb_core_path()
    includes = [os.path.join(BASE_DIR, "src"),
                pybind11.get_include()] + _py_include()
    link = _py_link_flags()

    cmd = _compile(name, flags, is_msvc, src, out,
                   extra_includes=includes, extra_link=link, shared=True)
    print(f"[build] pbb_core: {' '.join(cmd)}", file=sys.stderr)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"[build] FAILED:\n{r.stderr}", file=sys.stderr)
        sys.exit(1)


def _compile_engine():
    cc = _find_compiler()
    if not cc:
        print("ERROR: No C++ compiler found", file=sys.stderr)
        sys.exit(1)

    name, flags, is_msvc = cc
    src = os.path.join(BASE_DIR, "engine_main.cpp")
    out = _engine_bin()
    includes = [os.path.join(BASE_DIR, "src")]

    cmd = _compile(name, flags, is_msvc, src, out, extra_includes=includes)
    print(f"[build] engine: {' '.join(cmd)}", file=sys.stderr)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"[build] FAILED:\n{r.stderr}", file=sys.stderr)
        sys.exit(1)


def ensure_all(rebuild=False):
    check_python()
    check_deps()
    os.chdir(BASE_DIR)

    if rebuild or not _pbb_core_exists():
        _compile_pbb_core()

    if rebuild or not os.path.exists(_engine_bin()):
        _compile_engine()

    return _engine_bin()

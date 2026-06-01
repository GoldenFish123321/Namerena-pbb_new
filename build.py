#!/usr/bin/env python3
"""PBB build script — compile pbb_core (.so/.pyd) + pbb_engine (executable)."""
import os, sys, glob, subprocess, shutil, sysconfig, tempfile

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
    ext = sysconfig.get_config_var("EXT_SUFFIX") or (".pyd" if sys.platform == "win32" else ".so")
    return os.path.join(BASE_DIR, "pbb_core" + ext)


def _pbb_core_exists():
    return bool(glob.glob(os.path.join(BASE_DIR, "pbb_core*.so")) or
                glob.glob(os.path.join(BASE_DIR, "pbb_core*.pyd")))


def _compiler_probe(compiler, flag):
    """Test if compiler accepts a SIMD flag. Returns True if the flag works."""
    src = tempfile.NamedTemporaryFile(mode='w', suffix='.cpp', delete=False)
    src.write("int main(){return 0;}\n")
    src.close()
    obj = src.name + (".obj" if "cl" in os.path.basename(compiler) else ".o")
    is_msvc = os.path.basename(compiler).startswith("cl")
    std_flag = "/std:c++17" if is_msvc else "-std=c++17"
    try:
        r = subprocess.run([compiler, std_flag, "-c", flag, src.name,
                            ("/Fo:" if is_msvc else "-o") + obj],
                           capture_output=True, timeout=15, encoding='utf-8', errors='replace')
        return r.returncode == 0
    except:
        return False
    finally:
        for f in [src.name, obj]:
            if os.path.exists(f): os.unlink(f)


def _detect_simd(compiler):
    """Returns (additional_flags, simd_name)."""
    is_win = sys.platform == "win32"
    is_msvc = os.path.basename(compiler).startswith("cl")

    if not is_win:
        import platform
        machine = platform.machine()
        if machine.startswith("aarch") or machine.startswith("arm"):
            return ([], "NEON")

    if is_msvc:
        candidates = [(["/arch:AVX512"], "AVX-512"), (["/arch:AVX2"], "AVX2")]
    else:
        candidates = [(["-mavx512f", "-mavx512bw", "-mfma"], "AVX-512"),
                      (["-mavx2", "-mfma"], "AVX2")]

    if not is_win:
        try:
            cpuinfo = open("/proc/cpuinfo").read().lower()
            for flags, name in candidates:
                key = "avx512f" if "AVX-512" in name else "avx2"
                if key in cpuinfo:
                    return (flags, name)
        except: pass

    for flags, name in candidates:
        if _compiler_probe(compiler, flags[0]):
            return (flags, name)

    return ([], "none")


def _find_compilers(for_core=False):
    """Returns list of (name, flags, simd_name, is_msvc) in priority order.

    Rules:
      - pbb_core (for_core=True) MUST match Python's compiler ABI.
        Windows → cl only.  Other platforms → same as engine.
      - Engine (for_core=False) picks best available compiler.
        Linux:   icpx (-xHost) → g++
        ARM:     g++ (NEON mandatory)
        Windows: icpx (-xHost) → g++ (AVX-512) → cl
    """
    result = []
    is_win = sys.platform == "win32"

    # Find icpx binary (Linux + Windows)
    icpx = shutil.which("icpx")
    if not icpx:
        paths = [r"C:\Program Files (x86)\Intel\oneAPI", r"C:\Program Files\Intel\oneAPI",
                 "/opt/intel/oneapi"]
        for d in paths:
            for ver in ["latest", "2026.0", "2025.0"]:
                p = os.path.join(d, "compiler", ver, "bin",
                                 "icpx.exe" if is_win else "icpx")
                if os.path.exists(p): icpx = p; break

    if for_core and is_win:
        # pbb_core on Windows: cl only (Python ABI)
        if shutil.which("cl"):
            simd_flags, simd_name = _detect_simd("cl")
            result.append(("cl", ["/std:c++17", "/Ox", "/EHsc", "/utf-8", "/w"] + simd_flags, simd_name, True))
        return result

    if is_win:
        # Windows engine: icpx (-xHost) → g++ (AVX-512) → cl
        if icpx:
            flags = ["-std=c++17", "-w", "-O3", "-ipo", "-ffast-math",
                     "-funroll-loops", "-qopt-mem-layout-trans=4", "-qopt-prefetch=5",
                     "-qopenmp", "-xHost", "-finline-functions"]
            result.append((icpx, flags, "auto (-xHost)", False))
        if shutil.which("g++"):
            base_flags = ["-std=c++17", "-O3", "-funroll-loops", "-ffast-math"]
            simd_flags, simd_name = _detect_simd("g++")
            result.append(("g++", base_flags + simd_flags, simd_name, False))
        if shutil.which("cl"):
            simd_flags, simd_name = _detect_simd("cl")
            result.append(("cl", ["/std:c++17", "/Ox", "/EHsc", "/utf-8", "/w"] + simd_flags, simd_name, True))
        return result

    # Linux / ARM
    if icpx:
        flags = ["-std=c++17", "-w", "-O3", "-ipo", "-ffast-math",
                 "-funroll-loops", "-qopt-mem-layout-trans=4", "-qopt-prefetch=5",
                 "-lpthread", "-xHost", "-finline-functions"]
        result.append((icpx, flags, "auto (-xHost)", False))
    if shutil.which("g++"):
        base_flags = ["-std=c++17", "-O3", "-funroll-loops", "-ffast-math"]
        simd_flags, simd_name = _detect_simd("g++")
        result.append(("g++", base_flags + simd_flags, simd_name, False))

    return result


def _py_include():
    inc = sysconfig.get_config_var("INCLUDEPY")
    if inc: return [inc]
    return [os.path.join(sys.prefix, "include")]


def _py_link_flags():
    if sys.platform == "win32":
        ver = f"{sys.version_info.major}{sys.version_info.minor}"
        libdir = sysconfig.get_config_var("LIBDIR") or os.path.join(sys.prefix, "libs")
        lib = os.path.join(libdir, f"python{ver}.lib")
        return [lib] if os.path.exists(lib) else [f"/LIBPATH:{libdir}", f"python{ver}.lib"]
    libdir = sysconfig.get_config_var("LIBDIR") or os.path.join(sys.prefix, "lib")
    ldflags = sysconfig.get_config_var("LDFLAGS") or ""
    flags = [f"-L{libdir}"] + ldflags.split()
    try:
        pyld = subprocess.run(["python3-config", "--ldflags", "--embed"],
                              capture_output=True, text=True, encoding='utf-8', errors='replace')
        if pyld.returncode == 0 and "-lpython" in pyld.stdout:
            flags += pyld.stdout.strip().split()
    except: pass
    return flags


def _compile(name, flags, is_msvc, src, out, extra_includes=[], extra_link=[],
             shared=False):
    if is_msvc:
        cmd = [name] + flags
        for d in extra_includes:
            cmd.append(f"/I{d}")
        if shared: cmd.append("/LD")
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
    """Compile bridge.cpp → pbb_core.{so,pyd}. Tries compilers in priority order."""
    import pybind11
    compilers = _find_compilers(for_core=True)
    if not compilers:
        print("ERROR: No C++ compiler found", file=sys.stderr)
        sys.exit(1)

    src = os.path.join(BASE_DIR, "src", "bridge.cpp")
    out = _pbb_core_path()
    includes = [os.path.join(BASE_DIR, "src"), pybind11.get_include()] + _py_include()
    link = _py_link_flags()

    last_err = ""
    for name, flags, simd_name, is_msvc in compilers:
        # MinGW g++ on Windows: static link runtime to avoid missing DLL errors
        extra_link = link[:]
        if sys.platform == "win32" and not is_msvc and "g++" in str(name):
            flags = flags + ["-static-libgcc", "-static-libstdc++"]

        cmd = _compile(name, flags, is_msvc, src, out,
                       extra_includes=includes, extra_link=extra_link, shared=True)
        print(f"[build] pbb_core [{name}]: {' '.join(cmd)}", file=sys.stderr)
        print(f"[build] SIMD: {simd_name}", file=sys.stderr)
        r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
        if r.returncode == 0:
            return  # success
        last_err = r.stderr
        print(f"[build] {name} failed, trying next compiler ...", file=sys.stderr)

    print(f"[build] All compilers failed:\n{last_err[-1000:]}", file=sys.stderr)
    sys.exit(1)


def _compile_engine():
    """Compile engine_main.cpp → pbb_engine. Tries compilers in priority order."""
    compilers = _find_compilers()  # g++ preferred for engine
    if not compilers:
        print("ERROR: No C++ compiler found", file=sys.stderr)
        sys.exit(1)

    src = os.path.join(BASE_DIR, "engine_main.cpp")
    out = _engine_bin()
    includes = [os.path.join(BASE_DIR, "src")]

    last_err = ""
    for name, flags, simd_name, is_msvc in compilers:
        cmd = _compile(name, flags, is_msvc, src, out, extra_includes=includes)
        print(f"[build] engine [{name}]: {' '.join(cmd)}", file=sys.stderr)
        print(f"[build] SIMD: {simd_name}", file=sys.stderr)
        r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
        if r.returncode == 0:
            return
        last_err = r.stderr
        print(f"[build] {name} failed, trying next compiler ...", file=sys.stderr)

    print(f"[build] All compilers failed:\n{last_err[-1000:]}", file=sys.stderr)
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

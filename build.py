#!/usr/bin/env python3
"""
PBB build system — compiler detection, SIMD probing, unified compilation.

Called by main.py via ensure_all(rebuild, verbose).
Not meant to be run directly — use ./run.sh or run.bat.
"""
import os, sys, shutil, subprocess, tempfile, sysconfig

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.path.join(BASE_DIR, "build")


def check_python():
    if sys.version_info < (3, 10):
        print("ERROR: Python 3.10+ required", file=sys.stderr)
        sys.exit(1)


def check_deps():
    missing = []
    try: import pybind11
    except ImportError: missing.append("pybind11")
    try: import yaml
    except ImportError: pass  # optional
    if missing:
        print(f"ERROR: Missing Python packages: {', '.join(missing)}", file=sys.stderr)
        print("  Use run.sh or run.bat to auto-install", file=sys.stderr)
        sys.exit(1)


def _engine_bin():
    name = "pbb_engine" + (".exe" if sys.platform == "win32" else "")
    return os.path.join(BUILD_DIR, name)


def _pbb_core_path():
    ext = sysconfig.get_config_var("EXT_SUFFIX") or (".pyd" if sys.platform == "win32" else ".so")
    return os.path.join(BUILD_DIR, "pbb_core" + ext)


def _pbb_core_exists():
    return os.path.exists(_pbb_core_path())


def _compiler_probe(compiler, flag):
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
    is_icpx = "icpx" in os.path.basename(compiler).lower()

    if not is_win:
        import platform
        machine = platform.machine()
        if machine.startswith("aarch") or machine.startswith("arm"):
            return ([], "NEON")

    if is_msvc:
        candidates = [(["/arch:AVX512"], "AVX-512"), (["/arch:AVX2"], "AVX2")]
    elif is_icpx:
        # Windows: AVX2 only (AVX-512 causes LLVM crash on bridge.cpp + mixed-runtime issues)
        if is_win:
            candidates = [(["-xCORE-AVX2"], "AVX2")]
        else:
            candidates = [(["-xCORE-AVX512"], "AVX-512"), (["-xCORE-AVX2"], "AVX2")]
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


def _gcc_arch_flags(compiler, verbose=False):
    """Detect GCC -march support. znver5 for Zen5, fallback to native/generic."""
    arch = "znver5"
    flag = f"-march={arch}"
    if _compiler_probe(compiler, flag):
        return [flag], ", march=znver5"

    fallback = "-march=native"
    if _compiler_probe(compiler, fallback):
        if verbose:
            print(f"[build] WARNING: {compiler} 不支持 {flag}; 使用 {fallback}", file=sys.stderr)
        return [fallback], ", march=native"

    if verbose:
        print(f"[build] WARNING: {compiler} 不支持 -march; 跳过 CPU 优化", file=sys.stderr)
    return [], ""


def _find_compilers(verbose=False):
    """Detect all available compilers, return in performance order.
    Each entry: (name, flags, simd_name, is_msvc).
    Order: icpx > g++ > cl, regardless of platform or target.
    """
    result = []
    is_win = sys.platform == "win32"

    # ---- Detect all compilers ----
    icpx = shutil.which("icpx")
    gpp  = shutil.which("g++")
    cl   = shutil.which("cl") if is_win else None

    # icpx: search oneAPI install paths if not in PATH
    if not icpx:
        for d in [r"C:\Program Files (x86)\Intel\oneAPI", r"C:\Program Files\Intel\oneAPI",
                  "/opt/intel/oneapi"]:
            for ver in ["latest", "2026.0", "2025.0"]:
                p = os.path.join(d, "compiler", ver, "bin",
                                 "icpx.exe" if is_win else "icpx")
                if os.path.exists(p): icpx = p; break

    # ---- Build compiler entries ----
    entries = []

    if icpx:
        if is_win:
            # -xHost still crashes on Arrow Lake (LLVM bug in bridge.cpp).
            # -xCORE-AVX2 is the only stable choice for both pbb_core and engine.
            flags = ["-std=c++17", "-w", "-O3", "-ipo", "-ffast-math",
                     "-funroll-loops", "-qopt-mem-layout-trans=4", "-qopt-prefetch=5",
                     "-qopenmp", "-xCORE-AVX2"]
            entries.append((icpx, flags, "AVX2", False))
        else:
            flags = ["-std=c++17", "-w", "-O3", "-ipo", "-ffast-math",
                     "-funroll-loops", "-qopt-mem-layout-trans=4", "-qopt-prefetch=5",
                     "-lpthread", "-xHost", "-finline-functions"]
            entries.append((icpx, flags, "auto (-xHost)", False))

    if gpp:
        base = ["-std=c++17", "-O3", "-funroll-loops", "-ffast-math"]
        arch_flags, arch_name = _gcc_arch_flags("g++", verbose)
        simd_flags, simd_name = _detect_simd("g++")
        entries.append(("g++", base + arch_flags + simd_flags, simd_name + arch_name, False))

    if cl:
        simd_flags, simd_name = _detect_simd("cl")
        entries.append(("cl", ["/std:c++17", "/Ox", "/EHsc", "/utf-8", "/w"] + simd_flags, simd_name, True))

    # ---- Order: icpx > g++ > cl on all platforms ----

    return entries  # default: icpx > g++ > cl


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


def _mingw_python_link() -> list[str]:
    """Generate MinGW-compatible libpython from MSVC python DLL via gendef+dlltool.
    Returns list of link flags (e.g. ['-Lbuild/mingw_lib', '-lpython314']).
    Cached — only generated once per session.
    """
    cache_dir = os.path.join(BUILD_DIR, "mingw_lib")
    ver = f"{sys.version_info.major}{sys.version_info.minor}"
    lib_a = os.path.join(cache_dir, f"libpython{ver}.a")

    if os.path.exists(lib_a):
        return [f"-L{cache_dir}", f"-lpython{ver}"]

    # Find python DLL
    dll = None
    candidates = [
        os.path.join(sys.base_prefix, f"python{ver}.dll"),
        os.path.join(sys.prefix, f"python{ver}.dll"),
    ]
    # uv installs Python to a non-standard path — exec dir is reliable
    exe_dir = os.path.dirname(getattr(sys, '_base_executable', sys.executable))
    candidates.append(os.path.join(exe_dir, f"python{ver}.dll"))
    # Also check directory of full python3.dll if found
    for c_dir in [sys.base_prefix, sys.prefix, exe_dir]:
        try:
            import glob as _glob
            hits = _glob.glob(os.path.join(c_dir, f"python{ver}*.dll"))
            for h in hits:
                if h not in candidates:
                    candidates.append(h)
        except:
            pass
    for c in candidates:
        if os.path.exists(c):
            dll = c
            break
    if not dll:
        import ctypes, glob as _glob
        try:
            dll = _glob.glob(os.path.join(sys.prefix, "python*.dll"))
            if dll:
                dll = dll[0]
        except:
            pass
    if not dll:
        print("[build] WARNING: cannot find python DLL for gendef", file=sys.stderr)
        return _py_link_flags()

    os.makedirs(cache_dir, exist_ok=True)

    # Find gendef/dlltool (bundled with MinGW g++)
    gendef = shutil.which("gendef")
    dlltool = shutil.which("dlltool")
    # Fallback to msys2 paths
    if not gendef:
        for p in [r"C:\msys64\mingw64\bin\gendef.exe", r"C:\msys64\ucrt64\bin\gendef.exe"]:
            if os.path.exists(p): gendef = p; break
    if not dlltool:
        for p in [r"C:\msys64\mingw64\bin\dlltool.exe", r"C:\msys64\ucrt64\bin\dlltool.exe"]:
            if os.path.exists(p): dlltool = p; break

    if not gendef or not dlltool:
        print("[build] WARNING: gendef/dlltool not found — falling back to MSVC .lib", file=sys.stderr)
        return _py_link_flags()

    def_file = os.path.join(cache_dir, f"python{ver}.def")
    r = subprocess.run([gendef, dll], capture_output=True, cwd=cache_dir, encoding='utf-8', errors='replace')
    if r.returncode != 0:
        print(f"[build] WARNING: gendef failed: {r.stderr[-200:]}", file=sys.stderr)
        return _py_link_flags()
    # gendef outputs to current dir, named python{ver}.def
    gendef_out = os.path.join(cache_dir, os.path.basename(dll).replace(".dll", ".def"))
    if not os.path.exists(gendef_out) and os.path.exists(os.path.join(os.getcwd(), os.path.basename(dll).replace(".dll", ".def"))):
        # gendef wrote to cwd
        import shutil as _shutil
        _shutil.move(os.path.join(os.getcwd(), os.path.basename(dll).replace(".dll", ".def")), gendef_out)
    if not os.path.exists(gendef_out):
        # try common naming
        for f in os.listdir(cache_dir):
            if f.endswith(".def"):
                gendef_out = os.path.join(cache_dir, f)
                break

    r2 = subprocess.run([dlltool, "-d", gendef_out, "-l", lib_a, "-D", dll],
                         capture_output=True, cwd=cache_dir, encoding='utf-8', errors='replace')
    if r2.returncode != 0:
        print(f"[build] WARNING: dlltool failed: {r2.stderr[-200:]}", file=sys.stderr)
        return _py_link_flags()

    if os.path.exists(lib_a):
        print(f"[build] Generated MinGW lib: {lib_a}", file=sys.stderr)
        return [f"-L{cache_dir}", f"-lpython{ver}"]
    return _py_link_flags()


def _cxxflags_override(target, compiler_name, auto_flags, simd_name, verbose):
    """If PBB_CXXFLAGS set, use it directly; otherwise return auto_flags.
    PBB_CXXFLAGS can be a space-separated string of compiler flags.
    Example: PBB_CXXFLAGS="-march=native -Ofast -funroll-loops -ffast-math"
    """
    env_flags = os.environ.get("PBB_CXXFLAGS", "").strip()
    if not env_flags:
        return auto_flags
    user_flags = env_flags.split()
    if verbose or True:  # always show override
        print(f"[build] {target}: overriding flags → PBB_CXXFLAGS={env_flags}", file=sys.stderr)
    return user_flags


def _compile(name, flags, is_msvc, src, out, extra_includes=[], extra_link=[],
             shared=False):
    if is_msvc:
        cmd = [name] + flags
        for d in extra_includes:
            cmd.append(f"/I{d}")
        # Redirect .obj to build/
        obj_dir = os.path.dirname(out)
        cmd.append(f"/Fo{obj_dir}/")
        if shared: cmd.append("/LD")
        cmd.append(src)
        if shared:
            implib = os.path.join(obj_dir, os.path.splitext(os.path.basename(out))[0] + ".lib")
            cmd += ["/link"] + extra_link + [f"/OUT:{out}", f"/IMPLIB:{implib}"]
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


def _compile_pbb_core(verbose=False):
    """Compile bridge.cpp → pbb_core.{so,pyd}. Tries compilers in priority order."""
    import pybind11
    compilers = _find_compilers(verbose)  # icpx > g++ > cl
    if not compilers:
        print("ERROR: No C++ compiler found", file=sys.stderr)
        sys.exit(1)

    os.makedirs(BUILD_DIR, exist_ok=True)
    src = os.path.join(BASE_DIR, "src", "bridge.cpp")
    out = _pbb_core_path()
    includes = [os.path.join(BASE_DIR, "src"), pybind11.get_include()] + _py_include()
    link = _py_link_flags()

    last_err = ""
    for name, flags, simd_name, is_msvc in compilers:
        # MinGW g++ on Windows: static link runtime to avoid missing DLL errors
        extra_link = link[:]
        if sys.platform == "win32" and not is_msvc and "g++" in str(name):
            flags = flags + ["-static", "-static-libgcc", "-static-libstdc++"]
            extra_link = _mingw_python_link()

        # PBB_CXXFLAGS 环境变量: 覆盖自动检测的 flags
        overrides = _cxxflags_override("pbb_core", name, flags, simd_name, verbose)

        cmd = _compile(name, overrides, is_msvc, src, out,
                       extra_includes=includes, extra_link=extra_link, shared=True)
        if verbose:
            print(f"[build] pbb_core [{name}]: {' '.join(cmd)}", file=sys.stderr)
        print(f"[build] pbb_core: {name} ({simd_name})", file=sys.stderr)
        r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
        if r.returncode == 0:
            return  # success
        last_err = r.stderr
        print(f"[build] {name} failed, trying next compiler ...", file=sys.stderr)

    print(f"[build] All compilers failed:\n{last_err[-1000:]}", file=sys.stderr)
    sys.exit(1)


def _compile_engine(verbose=False):
    """Compile engine_main.cpp → pbb_engine. Tries compilers in priority order."""
    compilers = _find_compilers(verbose)
    if not compilers:
        print("ERROR: No C++ compiler found", file=sys.stderr)
        sys.exit(1)

    os.makedirs(BUILD_DIR, exist_ok=True)
    src = os.path.join(BASE_DIR, "engine_main.cpp")
    out = _engine_bin()
    includes = [os.path.join(BASE_DIR, "src")]

    last_err = ""
    for name, flags, simd_name, is_msvc in compilers:
        if sys.platform == "win32" and not is_msvc and "g++" in str(name):
            flags = flags + ["-static", "-static-libgcc", "-static-libstdc++"]

        # PBB_CXXFLAGS 环境变量: 覆盖自动检测的 flags
        overrides = _cxxflags_override("engine", name, flags, simd_name, verbose)

        cmd = _compile(name, overrides, is_msvc, src, out, extra_includes=includes)
        if verbose:
            print(f"[build] engine [{name}]: {' '.join(cmd)}", file=sys.stderr)
        print(f"[build] engine: {name} ({simd_name})", file=sys.stderr)
        r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
        if r.returncode == 0:
            return
        last_err = r.stderr
        print(f"[build] {name} failed, trying next compiler ...", file=sys.stderr)

    print(f"[build] All compilers failed:\n{last_err[-1000:]}", file=sys.stderr)
    sys.exit(1)


def _env_info(verbose=False):
    """Print environment summary: OS, CPU, Python, available compilers + SIMD."""
    import platform
    print(f"[env] OS: {sys.platform} {platform.machine()}", file=sys.stderr)
    try:
        cpu = platform.processor() or "unknown"
        cores = os.cpu_count() or "?"
        print(f"[env] CPU: {cpu} ({cores} cores)", file=sys.stderr)
    except:
        pass
    print(f"[env] Python: {sys.version.split()[0]}", file=sys.stderr)

    # Show all detected compilers with their SIMD levels
    compilers = _find_compilers(verbose)
    names = []
    for name, flags, simd_name, is_msvc in compilers:
        names.append(f"{name}({simd_name})")
    print(f"[env] compilers: {' -> '.join(names) if names else 'NONE'}", file=sys.stderr)


def ensure_all(rebuild=False, verbose=False):
    check_python()
    check_deps()
    os.chdir(BASE_DIR)
    _env_info(verbose)

    if rebuild or not _pbb_core_exists():
        _compile_pbb_core(verbose)

    if rebuild or not os.path.exists(_engine_bin()):
        _compile_engine(verbose)

    return _engine_bin()

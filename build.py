#!/usr/bin/env python3
"""
PBB build system — compiler detection, SIMD probing, unified compilation.

Called by main.py via ensure_all(rebuild, verbose).
Not meant to be run directly — use ./run.sh or run.bat.
"""
import os, sys, shutil, subprocess, tempfile, sysconfig, ctypes, platform

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.path.join(BASE_DIR, "build")


# ---- CPU feature detection (no compilation required) ----

# Cache: read once, reused by both icpx and g++ VNNI checks
_VNNI_CACHE = None


def _cpuid_win(leaf, subleaf, ret_reg):
    """Execute CPUID instruction on Windows x86-64 via ctypes.

    Args:
        leaf: CPUID leaf (EAX).
        subleaf: CPUID subleaf (ECX).
        ret_reg: Register to return: "eax", "ebx", "ecx", or "edx".

    Returns the register value as unsigned int, or 0 on failure.
    """
    # Build x86-64 machine code
    if subleaf:
        code = bytearray([0xB9,
                          subleaf & 0xFF, (subleaf >> 8) & 0xFF,
                          (subleaf >> 16) & 0xFF, (subleaf >> 24) & 0xFF])
    else:
        code = bytearray([0x31, 0xC9])  # xor ecx, ecx
    code += bytes([0xB8,
                   leaf & 0xFF, (leaf >> 8) & 0xFF,
                   (leaf >> 16) & 0xFF, (leaf >> 24) & 0xFF])
    code += bytes([0x0F, 0xA2])  # cpuid
    if ret_reg == "ecx":
        code += bytes([0x89, 0xC8])  # mov eax, ecx
    elif ret_reg == "ebx":
        code += bytes([0x89, 0xD8])  # mov eax, ebx
    elif ret_reg == "edx":
        code += bytes([0x89, 0xD0])  # mov eax, edx
    # else "eax": already in eax, nothing to do
    code += bytes([0xC3])  # ret

    kernel32 = ctypes.windll.kernel32
    # CRITICAL: on 64-bit Python, set restype/argtypes to avoid pointer truncation
    kernel32.VirtualAlloc.restype = ctypes.c_void_p
    kernel32.VirtualFree.restype = ctypes.c_int
    kernel32.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulong]

    MEM_COMMIT = 0x1000
    MEM_RESERVE = 0x2000
    PAGE_EXECUTE_READWRITE = 0x40
    MEM_RELEASE = 0x8000

    ptr = kernel32.VirtualAlloc(0, len(code), MEM_COMMIT | MEM_RESERVE,
                                PAGE_EXECUTE_READWRITE)
    if not ptr:
        return 0

    ctypes.memmove(ptr, bytes(code), len(code))
    func = ctypes.CFUNCTYPE(ctypes.c_uint)(ptr)
    val = func()
    kernel32.VirtualFree(ptr, 0, MEM_RELEASE)
    return val


def _cpu_has_vnni():
    """Check if host CPU supports AVX-VNNI.

    AVX-VNNI (VPDPBUSD with YMM/XMM): CPUID leaf 7, subleaf 1, EAX bit 4.
    AVX512-VNNI (VPDPBUSD with ZMM):  CPUID leaf 7, subleaf 0, ECX bit 11.
    Either one qualifies.  Result is cached — safe to call multiple times.
    """
    global _VNNI_CACHE
    if _VNNI_CACHE is not None:
        return _VNNI_CACHE

    result = False

    # Linux / Android / Termux / BSD: read /proc/cpuinfo
    if sys.platform != "win32" and sys.platform != "darwin":
        try:
            with open("/proc/cpuinfo") as f:
                info = f.read().lower()
                result = "avxvnni" in info or "avx512_vnni" in info or "avx512vnni" in info
        except Exception:
            result = False

    # macOS: targeted sysctl queries (much faster than sysctl -a)
    elif sys.platform == "darwin":
        try:
            for key in ("machdep.cpu.leaf7_features", "hw.optional.avx512vnni",
                        "hw.optional.avxvnni"):
                r = subprocess.run(["sysctl", "-n", key], capture_output=True,
                                   text=True, timeout=5, encoding="utf-8", errors="replace")
                if r.returncode == 0 and ("avxvnni" in r.stdout.lower() or
                                           "avx512" in r.stdout.lower()):
                    result = True
                    break
        except Exception:
            result = False

    # Windows: CPUID via ctypes (x86-64 only)
    else:
        try:
            machine = platform.machine().lower()
            if machine not in ("amd64", "x86_64", "x64"):
                result = False
            else:
                # AVX-VNNI: leaf 7, subleaf 1, EAX bit 4
                max_subleaf = _cpuid_win(7, 0, "eax") & 0xFFFFFFFF
                if max_subleaf >= 1:
                    eax1 = _cpuid_win(7, 1, "eax")
                    if eax1 & (1 << 4):
                        result = True

                # AVX512-VNNI: leaf 7, subleaf 0, ECX bit 11 (fallback)
                if not result:
                    ecx0 = _cpuid_win(7, 0, "ecx")
                    if ecx0 & (1 << 11):
                        result = True
        except Exception:
            result = False

    _VNNI_CACHE = result
    return result


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
            simd_label = "AVX2"
            # AVX-VNNI: both compiler AND CPU must support it
            if _compiler_probe(icpx, "-mavxvnni") and _cpu_has_vnni():
                flags.append("-mavxvnni")
                simd_label = "AVX2+VNNI"
            entries.append((icpx, flags, simd_label, False))
        else:
            flags = ["-std=c++17", "-w", "-O3", "-ipo", "-ffast-math",
                     "-funroll-loops", "-qopt-mem-layout-trans=4", "-qopt-prefetch=5",
                     "-lpthread", "-xHost", "-finline-functions"]
            entries.append((icpx, flags, "auto (-xHost)", False))

    if gpp:
        base = ["-std=c++17", "-O3", "-funroll-loops", "-ffast-math"]
        arch_flags, arch_name = _gcc_arch_flags("g++", verbose)
        simd_flags, simd_name = _detect_simd("g++")
        # AVX-VNNI: both compiler (GCC 9+/Clang 10+) AND CPU must support it
        if _compiler_probe("g++", "-mavxvnni") and _cpu_has_vnni():
            simd_flags.append("-mavxvnni")
            simd_name = simd_name + "+VNNI"
        entries.append(("g++", base + arch_flags + simd_flags, simd_name + arch_name, False))

    if cl:
        simd_flags, simd_name = _detect_simd("cl")
        entries.append(("cl", ["/std:c++17", "/Ox", "/EHsc", "/utf-8", "/w"] + simd_flags, simd_name, True))

    # ---- Order: icpx > g++ > cl on all platforms ----

    return entries  # default: icpx > g++ > cl


def _py_include():
    inc = sysconfig.get_config_var("INCLUDEPY")
    if inc:
        path = os.path.join(inc, "Python.h")
        if os.path.exists(path):
            return [inc]
    # Fallback: try sys.prefix/include
    fallback = os.path.join(sys.prefix, "include")
    if os.path.exists(os.path.join(fallback, "Python.h")):
        return [fallback]
    # Neither worked — give clear instructions
    ver = f"{sys.version_info.major}.{sys.version_info.minor}"
    print(f"[build] ERROR: Python.h not found at INCLUDEPY or sys.prefix/include",
          file=sys.stderr)
    print(f"[build] Install Python dev headers:", file=sys.stderr)
    if sys.platform == "linux":
        print(f"  sudo apt install python{ver}-dev", file=sys.stderr)
    elif sys.platform == "darwin":
        print(f"  brew install python@{ver}", file=sys.stderr)
    else:
        print(f"  Reinstall Python with 'Development headers' checked", file=sys.stderr)
    return [fallback]  # let the compiler fail with its own error


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

    all_errors = []  # accumulate all compiler errors, not just the last
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
        err_preview = r.stderr.strip()[-500:]
        print(f"[build] {os.path.basename(str(name))} failed: {err_preview[:200]}", file=sys.stderr)
        all_errors.append((os.path.basename(str(name)), r.stderr.strip()))

    print(f"[build] All compilers failed ({len(all_errors)} tried):", file=sys.stderr)
    for compiler_name, err in all_errors:
        print(f"[build] --- {compiler_name} ---\n{err[-800:]}\n", file=sys.stderr)
    sys.exit(1)


def _detect_pair_width() -> int:
    """Detect optimal PAIR_WIDTH based on CPU microarchitecture.

    Returns:
        2 for ARM (in-order CPUs like Cortex-A55 can't handle more),
        5 for x86 Golden Cove / Zen4 / Zen5 (large ROB),
        6 for x86 Zen5+ / Lion Cove+ (ultra-wide ROB, experimental),
        4 for other x86 (default safe value).

    Override: set PBB_PAIR_WIDTH env var (e.g. PBB_PAIR_WIDTH=6).
    """
    import platform

    # Allow manual override via environment variable
    env_override = os.environ.get("PBB_PAIR_WIDTH", "")
    if env_override and env_override.isdigit():
        val = int(env_override)
        if 2 <= val <= 6:
            return val

    machine = platform.machine()
    if machine.startswith("aarch") or machine.startswith("arm"):
        return 2

    # Try /proc/cpuinfo on Linux/macOS/BSD
    if sys.platform != "win32":
        try:
            with open("/proc/cpuinfo") as f:
                info = f.read().lower()
                # Intel Golden Cove: 12th/13th/14th Gen, Core Ultra (Arrow Lake)
                if any(kw in info for kw in ["12th gen", "13th gen", "14th gen",
                                              "core ultra", "core  ultra"]):
                    return 5
                # AMD Zen4 (Ryzen 7xxx/8xxx) and Zen5 (Ryzen 9xxx)
                if "amd" in info:
                    import re
                    m = re.search(r"ryzen\s+(\d)", info)
                    if m and int(m.group(1)) >= 7:
                        return 5
        except Exception:
            pass
    else:
        # Windows: use CPUID to detect microarchitecture
        try:
            if machine.lower() not in ("amd64", "x86_64", "x64"):
                return 4

            # CPUID leaf 1 EAX: family/model/stepping
            eax1 = _cpuid_win(1, 0, "eax")
            if eax1 == 0:
                return 4

            base_family = (eax1 >> 8) & 0xF
            base_model  = (eax1 >> 4) & 0xF
            ext_model   = (eax1 >> 16) & 0xF
            ext_family  = (eax1 >> 20) & 0xFF

            family = base_family
            if base_family == 0xF:
                family += ext_family

            model = base_model
            if base_family == 0x6 or base_family == 0xF:
                model |= ext_model << 4

            # Detect vendor via CPUID leaf 0 EBX (first 4 chars of vendor string)
            ebx0 = _cpuid_win(0, 0, "ebx") & 0xFFFFFFFF
            is_intel = (ebx0 == 0x756E6547)   # "Genu" in little-endian
            is_amd   = (ebx0 == 0x68747541)   # "Auth" in little-endian

            if is_intel:
                # Golden Cove / Raptor Cove / Redwood Cove / Lion Cove / Cougar Cove
                # (all modern Intel P-core designs with large ROB)
                # Alder Lake: 0x97/0x9A, Raptor Lake: 0xB7/0xBA/0xBF,
                # Meteor Lake: 0xAA/0xAC, Arrow Lake: 0xC5/0xC6/0xB5,
                # Lunar Lake: 0xBD, Panther Lake: 0xCC/0xE5, Wildcat Lake: 0xD5
                if model in (0x97, 0x9A, 0xB7, 0xBA, 0xBF, 0xAA, 0xAC,
                             0xC5, 0xC6, 0xB5, 0xBD, 0xCC, 0xE5, 0xD5):
                    return 5
            elif is_amd:
                # Zen4 (family 0x19=25), Zen5 (family 0x1A=26)
                if family >= 0x1A:
                    return 6
                elif family >= 0x19:
                    return 5
        except Exception:
            pass

    return 4  # default safe value


def _compile_engine(verbose=False):
    """Compile engine_main.cpp → pbb_engine. Tries compilers in priority order."""
    compilers = _find_compilers(verbose)
    if not compilers:
        print("ERROR: No C++ compiler found", file=sys.stderr)
        sys.exit(1)

    pair_width = _detect_pair_width()

    os.makedirs(BUILD_DIR, exist_ok=True)
    src = os.path.join(BASE_DIR, "engine_main.cpp")
    out = _engine_bin()
    includes = [os.path.join(BASE_DIR, "src")]

    all_errors = []
    for name, flags, simd_name, is_msvc in compilers:
        if sys.platform == "win32" and not is_msvc and "g++" in str(name):
            flags = flags + ["-static", "-static-libgcc", "-static-libstdc++"]

        # Add PAIR_WIDTH based on CPU microarchitecture
        flags = flags + [f"-DPAIR_WIDTH={pair_width}"]

        # PBB_CXXFLAGS 环境变量: 覆盖自动检测的 flags
        overrides = _cxxflags_override("engine", name, flags, simd_name, verbose)

        cmd = _compile(name, overrides, is_msvc, src, out, extra_includes=includes)
        if verbose:
            print(f"[build] engine [{name}]: {' '.join(cmd)}", file=sys.stderr)
        print(f"[build] engine: {name} ({simd_name}, PAIR_WIDTH={pair_width})", file=sys.stderr)
        r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
        if r.returncode == 0:
            return
        err_preview = r.stderr.strip()[-500:]
        print(f"[build] {os.path.basename(str(name))} failed: {err_preview[:200]}", file=sys.stderr)
        all_errors.append((os.path.basename(str(name)), r.stderr.strip()))

    print(f"[build] All compilers failed ({len(all_errors)} tried):", file=sys.stderr)
    for compiler_name, err in all_errors:
        print(f"[build] --- {compiler_name} ---\n{err[-800:]}\n", file=sys.stderr)
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

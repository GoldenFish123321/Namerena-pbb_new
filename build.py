#!/usr/bin/env python3
"""
PBB 编译模块 — 所有环境检测和 C++ 编译集中在此.

提供:
  check_python()  — Python 版本检查
  check_deps()    — Python 依赖检查
  ensure_all()    — 编译 pbb_core (.so) + pbb_engine (可执行)
"""
import os, sys, glob, subprocess, shutil

REQUIRED_PY = (3, 8)
BASE_DIR = os.path.dirname(os.path.abspath(__file__))


# ═══════════════════════════════════════════════════════════════
# 环境检测
# ═══════════════════════════════════════════════════════════════

def check_python():
    """检查 Python 版本 >= 3.8."""
    if sys.version_info < REQUIRED_PY:
        print(f"ERROR: Python {REQUIRED_PY[0]}.{REQUIRED_PY[1]}+ required", file=sys.stderr)
        sys.exit(1)


def check_deps():
    """检查 Python 依赖是否已安装 (由 run.sh/run.bat 负责安装)."""
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
        print(f"ERROR: 缺少依赖: {', '.join(missing)}", file=sys.stderr)
        print("  请用 run.sh 或 run.bat 启动 (自动创建虚拟环境并安装依赖)", file=sys.stderr)
        sys.exit(1)


# ═══════════════════════════════════════════════════════════════
# 编译: pybind11 桥接模块
# ═══════════════════════════════════════════════════════════════

def ensure_pbb_core(rebuild=False):
    """编译 pybind11 模块 (pbb_core*.so / .pyd), 提供字符集数据 + 评分函数.
    
    rebuild=False: 只在 .so 不存在时编译 (增量).
    rebuild=True:  强制重编.
    """
    so_pattern = os.path.join(BASE_DIR, "pbb_core.cpython-*.so")
    # Windows: .pyd 格式
    if sys.platform == "win32" and not glob.glob(so_pattern):
        so_pattern = os.path.join(BASE_DIR, "pbb_core*.pyd")
    need = rebuild or not glob.glob(so_pattern)
    if not need:
        return
    
    from pybind11.setup_helpers import Pybind11Extension, build_ext
    from setuptools import setup

    # 编译旗标 (复用 _compile_flags)
    extra_flags = _compile_flags()
    
    print("[build] Building pbb_core...", file=sys.stderr)
    ext = Pybind11Extension(
        "pbb_core",
        [os.path.join(BASE_DIR, "src", "bridge.cpp")],
        cxx_std=17,
        include_dirs=[os.path.join(BASE_DIR, "src")],
        extra_compile_args=extra_flags,
        extra_link_args=[],
    )
    # 抑制 setuptools 编译日志
    import io
    _old_stdout = sys.stdout
    sys.stdout = io.StringIO()
    try:
        setup(
            name="pbb_core",
            version="1.0.0",
            description="PBB Name Scoring Core",
            ext_modules=[ext],
            cmdclass={"build_ext": build_ext},
            script_args=["build_ext", "--inplace"],
        )
    finally:
        sys.stdout = _old_stdout


def _compile_flags():
    """返回 pybind11 编译旗标 (根据实际编译器选择)."""
    if sys.platform == "win32":
        # 探测 distutils 将使用哪个编译器
        try:
            from setuptools._distutils.ccompiler import new_compiler
            cc = new_compiler()
            if hasattr(cc, 'compiler_type') and cc.compiler_type == 'msvc':
                return ["/std:c++17", "/Ox", "/utf-8", "/w"]
        except Exception:
            pass
        # 回退: 有 cl.exe 用 MSVC 旗标, 否则用 GNU
        if shutil.which("cl"):
            return ["/std:c++17", "/Ox", "/utf-8"]
        return ["-std=c++17", "-O3", "-funroll-loops", "-ffast-math"]
    flags = ["-std=c++17", "-O3", "-funroll-loops", "-ffast-math",
             "-fno-plt", "-fno-semantic-interposition"]
    if _detect_avx2():
        flags.extend(["-mavx2", "-mfma"])
    return flags


# ═══════════════════════════════════════════════════════════════
# 编译: C++ 引擎
# ═══════════════════════════════════════════════════════════════

def _detect_avx2():
    """检测 CPU 是否支持 AVX2 指令集 (仅 Linux, 读 /proc/cpuinfo)."""
    try:
        with open("/proc/cpuinfo") as f:
            return "avx2" in f.read().lower()
    except FileNotFoundError:
        return False


def build_engine(rebuild=False):
    """编译 C++ 引擎 (engine_main.cpp → pbb_engine).
    
    编译策略 (按平台/编译器):
      Linux x86_64  — g++ -O3 -mavx2 -mfma
      Linux ARM     — g++ -O3 (无 SIMD)
      Windows MSVC  — cl /Ox (全优化)
      Windows MinGW — g++ -O3
    
    rebuild=False: 只在源码比二进制新时重编 (增量).
    rebuild=True:  强制重编.
    """
    bin_path = os.path.join(BASE_DIR, "pbb_engine")
    if sys.platform == "win32":
        bin_path += ".exe"
    src_dir = os.path.join(BASE_DIR, "src")
    main_cpp = os.path.join(BASE_DIR, "engine_main.cpp")

    # 增量编译检查
    need = rebuild or not os.path.exists(bin_path)
    if not need:
        bin_time = os.path.getmtime(bin_path)
        all_src = [main_cpp] + [
            os.path.join(src_dir, f) for f in os.listdir(src_dir)
            if f.endswith((".hpp", ".cpp"))
        ]
        for f in all_src:
            if os.path.getmtime(f) > bin_time:
                need = True
                break
    if not need:
        return bin_path

    # 编译器 + 旗标选择 (优先级: icpx > g++ > MSVC cl)
    if shutil.which("icpx"):
        # Intel oneAPI DPC++/C++ Compiler (最佳性能)
        flags = ["-std=c++17", "-w", "-O3", "-ipo", "-ffast-math",
                 "-funroll-loops", "-qopt-mem-layout-trans=4", "-qopt-prefetch=5"]
        if sys.platform == "win32":
            flags.extend(["-xCORE-AVX2", "-qopenmp"])
        else:
            flags.extend(["-xHost", "-finline-functions", "-lpthread"])
        cmd = ["icpx"] + flags + ["-Isrc", "-o", bin_path, main_cpp]
    elif shutil.which("g++"):
        # GNU g++ (通用回退)
        flags = ["-std=c++17", "-O3", "-funroll-loops", "-ffast-math"]
        if _detect_avx2():
            flags.extend(["-mavx2", "-mfma"])
        cmd = ["g++"] + flags + ["-Isrc", "-o", bin_path, main_cpp]
    elif sys.platform == "win32" and shutil.which("cl"):
        # MSVC (Windows only)
        flags = ["/std:c++17", "/Ox", "/EHsc", "/utf-8", "/w"]
        cmd = ["cl"] + flags + [f"/I{src_dir}", f"/Fe:{bin_path}", main_cpp]
    else:
        print("ERROR: 未找到 C++ 编译器 (icpx / g++ / MSVC cl).", file=sys.stderr)
        print("  安装 Intel oneAPI: https://www.intel.com/content/www/us/en/developer/tools/oneapi/overview.html", file=sys.stderr)
        sys.exit(1)

    print(f"[build] Compiling: {' '.join(cmd)}", file=sys.stderr)
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        print(f"[build] FAILED:\n{r.stderr}", file=sys.stderr)
        sys.exit(1)
    return bin_path


# ═══════════════════════════════════════════════════════════════
# 统一入口
# ═══════════════════════════════════════════════════════════════

def ensure_all(rebuild=False):
    """环境检测 + 编译全部组件, 返回 C++ 引擎可执行文件路径."""
    check_python()
    check_deps()
    os.chdir(BASE_DIR)
    ensure_pbb_core(rebuild)
    return build_engine(rebuild)

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
    need = rebuild or not glob.glob(so_pattern)
    if not need:
        return
    print("[build] Building pbb_core...", file=sys.stderr)
    subprocess.check_call(
        [sys.executable, os.path.join(BASE_DIR, "setup.py"), "build_ext", "--inplace"],
        stdout=subprocess.DEVNULL)


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

    # 编译器 + 旗标选择
    if sys.platform == "win32":
        if shutil.which("cl"):
            flags = ["/std:c++17", "/Ox", "/EHsc"]
            cmd = ["cl"] + flags + [f"/I{src_dir}", f"/Fe:{bin_path}", main_cpp]
        elif shutil.which("g++"):
            flags = ["-std=c++17", "-O3", "-funroll-loops", "-ffast-math"]
            cmd = ["g++"] + flags + ["-Isrc", "-o", bin_path, main_cpp]
        else:
            print("ERROR: 未找到 C++ 编译器. 请安装 Visual Studio Build Tools 或 MinGW.",
                  file=sys.stderr)
            sys.exit(1)
    else:
        flags = ["-std=c++17", "-O3", "-funroll-loops", "-ffast-math"]
        if _detect_avx2():
            flags.extend(["-mavx2", "-mfma"])
            print("[build] AVX2 detected", file=sys.stderr)
        cmd = ["g++"] + flags + ["-Isrc", "-o", bin_path, main_cpp]

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

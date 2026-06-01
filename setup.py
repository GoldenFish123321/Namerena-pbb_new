from pybind11.setup_helpers import Pybind11Extension, build_ext
from setuptools import setup
import sys, os, shutil

# 编译器检测: icpx > g++ > MSVC
if sys.platform == "win32":
    # icpx: 检查 PATH + oneAPI 默认路径
    icpx = shutil.which("icpx")
    if not icpx:
        for d in [r"C:\Program Files (x86)\Intel\oneAPI", r"C:\Program Files\Intel\oneAPI"]:
            for ver in ["latest", "2026.0", "2025.0"]:
                p = os.path.join(d, "compiler", ver, "bin", "icpx.exe")
                if os.path.exists(p): icpx = p; break
    if icpx or shutil.which("g++"):
        flags = ["-std=c++17", "-O3", "-funroll-loops", "-ffast-math"]
    else:
        flags = ["/std:c++17", "/Ox", "/utf-8", "/w"]
else:
    flags = ["-std=c++17", "-O3", "-funroll-loops", "-ffast-math",
             "-fno-plt", "-fno-semantic-interposition"]
    try:
        if "avx2" in open("/proc/cpuinfo").read().lower():
            flags += ["-mavx2", "-mfma"]
    except: pass

ext = Pybind11Extension("pbb_core", ["src/bridge.cpp"],
    cxx_std=17, include_dirs=["src"],
    extra_compile_args=flags)

setup(name="pbb_core", version="1.0.0",
      ext_modules=[ext], cmdclass={"build_ext": build_ext})

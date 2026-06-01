from pybind11.setup_helpers import Pybind11Extension, build_ext
from setuptools import setup
import sys, os

flags = (["/std:c++17", "/Ox", "/utf-8", "/w"] if sys.platform == "win32" else
         ["-std=c++17", "-O3", "-funroll-loops", "-ffast-math",
          "-fno-plt", "-fno-semantic-interposition"])

if sys.platform != "win32":
    try:
        if "avx2" in open("/proc/cpuinfo").read().lower():
            flags += ["-mavx2", "-mfma"]
    except: pass

ext = Pybind11Extension("pbb_core", ["src/bridge.cpp"],
    cxx_std=17, include_dirs=["src"],
    extra_compile_args=flags)

setup(name="pbb_core", version="1.0.0",
      ext_modules=[ext], cmdclass={"build_ext": build_ext})

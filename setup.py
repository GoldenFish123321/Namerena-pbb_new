from pybind11.setup_helpers import Pybind11Extension, build_ext
from setuptools import setup
import sys
import os

# 编译旗标: Windows MSVC vs Linux GCC/Clang
if sys.platform == "win32":
    extra_flags = ["/std:c++17", "/O2"]
    print("[setup] Windows MSVC build")
else:
    has_avx2 = os.path.exists('/proc/cpuinfo') and 'avx2' in open('/proc/cpuinfo').read().lower()
    extra_flags = ["-std=c++17", "-O3", "-funroll-loops", "-ffast-math",
                   "-fno-plt", "-fno-semantic-interposition"]
    if has_avx2:
        extra_flags.extend(["-mavx2", "-mfma"])
        print("[setup] AVX2 detected — enabling SIMD optimizations")
    else:
        print("[setup] AVX2 not detected — using scalar fallback")

ext_modules = [
    Pybind11Extension(
        "pbb_core",
        ["src/bridge.cpp"],
        cxx_std=17,
        include_dirs=["src"],
        extra_compile_args=extra_flags,
        extra_link_args=[],
    ),
]

setup(
    name="pbb_core",
    version="1.0.0",
    author="Hermes",
    description="PBB Name Scoring Core — C++ backend with SIMD",
    ext_modules=ext_modules,
    cmdclass={"build_ext": build_ext},
    zip_safe=False,
    python_requires=">=3.8",
)

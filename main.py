#!/usr/bin/env python3
"""
PBB 名字评分测号器 — Python 编排层.

职责:
  1. 环境检测 + 自动安装依赖 (pyyaml/pybind11/setuptools/tomli)
  2. 自动编译 C++ 引擎 (engine_main.cpp → pbb_engine) 和 pybind11 桥接模块
  3. 解析配置文件 (JSON/YAML/TOML 三格式)
  4. 构建字符集 → stdin 管道传参 → 启动 C++ 子进程
  5. 实时读取引擎进度 → 统计最终结果

用法:
  ./run.sh -c config.yaml --threads -1     # Linux/Termux
  run.bat -c config.yaml                   # Windows
  python3 main.py -c config.yaml           # 直接调用
  python3 main.py                           # 自动查找 config.json/yaml/toml
"""
import json, os, sys, time, random, argparse, subprocess, shutil, glob

REQUIRED_PY = (3, 8)                                                    # 最低 Python 版本
BASE_DIR = os.path.dirname(os.path.abspath(__file__))                   # 项目根目录


# ═══════════════════════════════════════════════════════════════
# 环境检测 + 自动安装
# ═══════════════════════════════════════════════════════════════

def check_python():
    """检查 Python 版本 >= 3.8."""
    if sys.version_info < REQUIRED_PY:
        print(f"ERROR: Python {REQUIRED_PY[0]}.{REQUIRED_PY[1]}+ required", file=sys.stderr)
        sys.exit(1)


def pip_install(pkg):
    """静默 pip install (跳过系统包保护)."""
    subprocess.check_call(
        [sys.executable, "-m", "pip", "install", pkg, "--break-system-packages"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def ensure_deps():
    """检查并自动安装缺失的 Python 依赖.
    
    需要的包:
      pyyaml     — YAML 配置解析
      setuptools — pybind11 构建依赖 (Python 3.12+ 移除了 distutils)
      pybind11   — C++/Python 桥接
      tomli      — TOML 配置解析 (Python < 3.11, 3.11+ 内置 tomllib)
    """
    deps = []
    try: import yaml
    except ImportError: deps.append("pyyaml")
    try: import setuptools
    except ImportError: deps.append("setuptools")
    try: import pybind11
    except ImportError: deps.append("pybind11")
    if sys.version_info < (3, 11):
        try: import tomli
        except ImportError: deps.append("tomli")
    if deps:
        print(f"[main] Installing: {', '.join(deps)}", file=sys.stderr)
        for d in deps:
            pip_install(d)


# ═══════════════════════════════════════════════════════════════
# 编译: pybind11 桥接模块 + C++ 引擎
# ═══════════════════════════════════════════════════════════════

def ensure_pbb_core():
    """编译 pybind11 模块 (pbb_core*.so / .pyd), 提供字符集数据 + 评分函数.
    
    只在 .so 文件不存在时编译, 增量构建.
    """
    so_pattern = os.path.join(BASE_DIR, "pbb_core.cpython-*.so")
    if not glob.glob(so_pattern):
        print("[main] Building pbb_core...", file=sys.stderr)
        subprocess.check_call(
            [sys.executable, os.path.join(BASE_DIR, "setup.py"), "build_ext", "--inplace"],
            stdout=subprocess.DEVNULL)


def detect_avx2():
    """检测 CPU 是否支持 AVX2 指令集 (仅 Linux, 读 /proc/cpuinfo)."""
    try:
        with open("/proc/cpuinfo") as f:
            return "avx2" in f.read().lower()
    except FileNotFoundError:
        return False


def build_engine():
    """编译 C++ 引擎 (engine_main.cpp → pbb_engine).
    
    编译策略 (按平台/编译器):
      Linux x86_64  — g++ -O3 -mavx2 -mfma
      Linux ARM     — g++ -O3 (无 SIMD)
      Windows MSVC  — cl /O2 (自动检测)
      Windows MinGW — g++ -O2 (回退)
    
    增量编译: 只在源码比二进制新时重编.
    """
    bin_path = os.path.join(BASE_DIR, "pbb_engine")
    if sys.platform == "win32":
        bin_path += ".exe"          # Windows 可执行文件扩展名
    src_dir = os.path.join(BASE_DIR, "src")
    main_cpp = os.path.join(BASE_DIR, "engine_main.cpp")

    # 检查是否需要重编: 二进制不存在 或 任何源文件更新
    need = not os.path.exists(bin_path)
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

    # 编译旗标 + 编译器选择
    if sys.platform == "win32":
        # Windows: 优先 MSVC, 回退 MinGW g++
        if shutil.which("cl"):
            flags = ["/std:c++17", "/Ox", "/EHsc"]          # /Ox = MSVC 全优化 = g++ -O3
            cmd = ["cl"] + flags + [f"/I{src_dir}", f"/Fe:{bin_path}", main_cpp]
        elif shutil.which("g++"):
            flags = ["-std=c++17", "-O3", "-funroll-loops", "-ffast-math"]
            cmd = ["g++"] + flags + ["-Isrc", "-o", bin_path, main_cpp]
        else:
            print("ERROR: 未找到 C++ 编译器. 请安装 Visual Studio Build Tools 或 MinGW.",
                  file=sys.stderr)
            sys.exit(1)
    else:
        # Linux / Termux: g++ (或 clang++)
        flags = ["-std=c++17", "-O3", "-funroll-loops", "-ffast-math"]
        if detect_avx2():
            flags.extend(["-mavx2", "-mfma"])
            print("[main] AVX2 detected", file=sys.stderr)
        cmd = ["g++"] + flags + ["-Isrc", "-o", bin_path, main_cpp]

    print(f"[main] Compiling: {' '.join(cmd)}", file=sys.stderr)
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        print(f"[main] Build FAILED:\n{r.stderr}", file=sys.stderr)
        sys.exit(1)
    return bin_path


# ═══════════════════════════════════════════════════════════════
# 主流程
# ═══════════════════════════════════════════════════════════════

def main():
    # ── 1. 环境准备 ──
    check_python()
    ensure_deps()
    os.chdir(BASE_DIR)                     # 切换工作目录到项目根
    ensure_pbb_core()                      # pybind11 桥接模块
    import pbb_core                        # 导入字符集/评分 API
    engine_bin = build_engine()            # C++ 引擎

    # ── 2. 解析配置文件 ──
    try: import yaml; HAS_YAML = True
    except ImportError: HAS_YAML = False

    parser = argparse.ArgumentParser(description="PBB 名字评分测号器")
    parser.add_argument("-c", "--config", default=None,
                        help="配置文件路径 (默认依次尝试 config.json/yaml/toml)")
    parser.add_argument("--threads", type=int, help="覆盖线程数, -1=自动")
    args = parser.parse_args()

    # 自动查找配置 or 验证用户指定路径
    config_path = args.config
    if config_path is None:
        for name in ("config.json", "config.yaml", "config.toml"):
            if os.path.exists(name):
                config_path = name
                break
        if config_path is None:
            print("ERROR: 未找到配置文件 (尝试了 config.json/yaml/toml)", file=sys.stderr)
            print("  提示: 复制 config.example.* → config.* 并修改", file=sys.stderr)
            sys.exit(1)
        print(f"[main] Auto config: {config_path}", file=sys.stderr)
    elif not os.path.exists(config_path):
        print(f"ERROR: 配置文件不存在: {config_path}", file=sys.stderr)
        sys.exit(1)

    # 按扩展名选择解析器
    suffix = os.path.splitext(config_path)[1].lower()
    if suffix in ('.yaml', '.yml'):
        if not HAS_YAML:
            print("ERROR: PyYAML not installed", file=sys.stderr)
            sys.exit(1)
        with open(config_path, encoding="utf-8") as f:
            config = yaml.safe_load(f)
    elif suffix == '.toml':
        try: import tomllib
        except ImportError: import tomli as tomllib
        with open(config_path, "rb") as f:
            config = tomllib.load(f)
    else:
        with open(config_path, encoding="utf-8") as f:
            config = json.load(f)

    # 线程数: 命令行优先 > 配置文件; -1 = 自动检测
    if args.threads is not None:
        config["threads"]["worker_threads"] = args.threads
    if config["threads"]["worker_threads"] == -1:
        config["threads"]["worker_threads"] = os.cpu_count() or 4
        print(f"[main] Auto threads: {config['threads']['worker_threads']}", file=sys.stderr)

    # ── 3. 构建字符集 ──
    # 从配置中读取字符集类型, 通过 pbb_core API 组装为字节流.
    # 字节流以 hex 编码后通过 stdin 传给 C++ 引擎.
    pbb_core.init_exhanzi()
    cs = config["character_set"]
    scl = cs["single_char_length"]          # 单字符字节数 (1/2/3/4)
    buf = bytearray()

    for t in cs["types"]:
        # ── 1 字节字符集 ──
        if scl == 1:
            if t == 1:   buf.extend(b'0123456789')
            elif t == 2: buf.extend(b'abcdefghijklmnopqrstuvwxyz')
            elif t == 3: buf.extend(b'ABCDEFGHIJKLMNOPQRSTUVWXYZ')
            elif t in (4, 5): buf.extend(bytes([int(x) & 0xFF for x in cs.get("custom_values", [])]))

        # ── 2 字节字符集 ──
        elif scl == 2:
            if t == 1:   buf.extend(pbb_core.get_xila_bytes())          # 小写希腊
            elif t == 2: buf.extend(pbb_core.get_XILA_bytes())          # 大写希腊
            elif t == 3: buf.extend(pbb_core.get_ewen_bytes())          # 小写俄文
            elif t == 4: buf.extend(pbb_core.get_EWEN_bytes())          # 大写俄文
            elif t == 5: buf.extend(pbb_core.get_lading_bytes())        # 小写拉丁
            elif t == 6: buf.extend(pbb_core.get_LADING_bytes())        # 大写拉丁
            elif t == 7: buf.extend(bytes([int(x) & 0xFF for x in cs.get("custom_values", [])]))
            elif t == 8:
                for r in cs.get("unicode_ranges", []):
                    for cp in range(r["start"], r["end"] + 1):
                        buf.extend(pbb_core.encode_unicode(cp))

        # ── 3 字节字符集 ──
        elif scl == 3:
            if t == 1:
                pbb_core.load_hanzi(19968, 40959)                       # 全部汉字
                buf.extend(pbb_core.get_charset_bytes())
                pbb_core.reset_charset()
            elif t == 2: buf.extend(pbb_core.get_hanzi_bytes())         # 常用汉字
            elif t == 3: buf.extend(pbb_core.get_pingjia_bytes())       # 平假名
            elif t == 4: buf.extend(pbb_core.get_pianjia_bytes())       # 片假名
            elif t == 5: buf.extend(pbb_core.get_mangwen_bytes())       # 盲文
            elif t == 6: buf.extend(pbb_core.get_extended_hanzi_3_bytes())
            elif t == 7: buf.extend(bytes([int(x) & 0xFF for x in cs.get("custom_values", [])]))
            elif t == 8:
                for r in cs.get("unicode_ranges", []):
                    for cp in range(r["start"], r["end"] + 1):
                        buf.extend(pbb_core.encode_unicode(cp))

        # ── 4 字节字符集 ──
        elif scl == 4:
            if t == 1:   buf.extend(pbb_core.get_extended_hanzi_4_bytes())
            elif t == 2: buf.extend(bytes([int(x) & 0xFF for x in cs.get("custom_values", [])]))
            elif t == 3:
                for r in cs.get("unicode_ranges", []):
                    for cp in range(r["start"], r["end"] + 1):
                        buf.extend(pbb_core.encode_unicode(cp))

    charset_bytes = bytes(buf)
    charset_len = len(charset_bytes) // scl if scl else 0                # 字符总数

    # ── 4. 组装引擎参数 ──
    # 前缀/后缀: '+' 表示空字符串
    prefixes = [p["name"] for p in config["prefixes"]]
    suffixes = [s["name"] for s in config["suffixes"]]
    prefixes = ["" if x == "+" else x for x in prefixes]
    suffixes = ["" if x == "+" else x for x in suffixes]

    # 输出文件名: '+' = 随机数字, '-' = 时间戳, 其他 = 自定义
    result_file = config["output"]["result_file"]
    if result_file == "+":
        result_file = f"out-{''.join(str(random.randint(0, 9)) for _ in range(7))}.txt"
    elif result_file == "-":
        result_file = f"out-{int(time.time())}.txt"

    en = config["enumeration"]
    cl = config["collection"]
    out_cfg = config["output"]
    ranges = en.get("ranges", [{}])[0]

    # 构建 key=value 文本, 通过 stdin 管道传给 C++ 引擎
    # charset_bytes 以 hex 编码 (避免二进制字符破坏管道)
    params = (
        f"team_name={config['team_name']}\n"
        f"n_threads={config['threads']['worker_threads']}\n"
        f"scl={scl}\ncharset_len={charset_len}\ncharset_bytes={charset_bytes.hex()}\n"
        f"prefixes={','.join(prefixes)}\nsuffixes={','.join(suffixes)}\n"
        f"mode={en['mode']}\nvariable_len={en['variable_length']}\n"
        f"range_L={ranges.get('start', 0)}\nrange_R={ranges.get('end', int(1e18))}\n"
        f"xp_min={cl['xp_min']}\nxd_min={cl['xd_min']}\n"
        f"collect_mode={cl.get('collect_mode', 0)}\n"
        f"output_xp={out_cfg.get('output_xp', 1)}\n"
        f"output_log={out_cfg.get('log_output', 1)}\n"
        f"output_speed={out_cfg.get('speed_output', 1)}\n"
        f"result_file={result_file}\n"
    )

    os.makedirs("out", exist_ok=True)
    print(f"[main] Threads: {config['threads']['worker_threads']}, Mode: {en['mode']}", file=sys.stderr)

    # ── 5. 启动 C++ 引擎 (实时读取进度) ──
    t0 = time.time()
    proc = subprocess.Popen(
        [engine_bin],
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    proc.stdin.write(params)
    proc.stdin.close()

    # 逐行转发引擎的 stderr 输出 (进度 / 速度 / 结果计数)
    for line in proc.stderr:
        line = line.strip()
        if line:
            print(f"  {line}", file=sys.stderr, flush=True)
    proc.wait()
    elapsed = time.time() - t0

    if proc.returncode != 0:
        print(f"[main] Engine failed ({proc.returncode})", file=sys.stderr)
        sys.exit(1)

    # ── 6. 统计结果 ──
    # 引擎将结果写到 out/<result_file>, 每行格式:
    #   <名字>@<队伍名> <XP> <XD>   (output_xp=1)
    #   <名字>@<队伍名>              (output_xp=0)
    out_path = os.path.join("out", result_file)
    count, mxp, mxd = 0, 0, 0
    if os.path.exists(out_path):
        with open(out_path) as f:
            for line in f:
                if not line.strip():
                    continue
                count += 1
                parts = line.rsplit(" ", 2)
                if len(parts) == 3:
                    try:
                        mxp = max(mxp, int(parts[1]))
                        mxd = max(mxd, int(parts[2]))
                    except ValueError:
                        pass

    rL, rR = ranges.get("start", 0), ranges.get("end", int(1e18))
    print(f"\n[main] Done in {elapsed:.1f}s", file=sys.stderr)
    print(f"[main] Found: {count}, Max: XP={mxp} XD={mxd}", file=sys.stderr)
    if elapsed > 0 and rR > rL:
        speed = (rR - rL) / elapsed
        print(f"[main] Speed: {speed*86400/1e12:.4f}T/d ({speed/1e6:.2f}M/s)", file=sys.stderr)


if __name__ == "__main__":
    main()

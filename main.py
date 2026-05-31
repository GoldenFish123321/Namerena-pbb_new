#!/usr/bin/env python3
"""
PBB 名字评分测号器 — 一键启动, 自动环境检测 + 编译 + 运行.

Usage:
  ./run.sh -c config.yaml --threads -1     # Linux/Termux
  run.bat -c config.yaml                   # Windows
  python3 main.py -c config.yaml           # 直接调用
"""

import json, os, sys, time, random, argparse, subprocess, shutil, glob

REQUIRED_PY = (3, 8)
BASE_DIR = os.path.dirname(os.path.abspath(__file__))


# ===== 环境检测 + 自动安装 =====
def check_python():
    if sys.version_info < REQUIRED_PY:
        print(f"ERROR: Python {REQUIRED_PY[0]}.{REQUIRED_PY[1]}+ required", file=sys.stderr)
        sys.exit(1)

def pip_install(pkg):
    subprocess.check_call([sys.executable, "-m", "pip", "install", pkg, "--break-system-packages"],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def ensure_deps():
    deps = []
    try: import yaml
    except ImportError: deps.append("pyyaml")
    # pybind11 需要 setuptools (Python 3.12+ 移除了 distutils)
    try: import setuptools
    except ImportError: deps.append("setuptools")
    try: import pybind11
    except ImportError: deps.append("pybind11")
    # TOML: Python 3.11+ 内置 tomllib, 旧版需要 tomli
    if sys.version_info < (3, 11):
        try: import tomli
        except ImportError: deps.append("tomli")
    if deps:
        print(f"[main] Installing: {', '.join(deps)}", file=sys.stderr)
        for d in deps: pip_install(d)

def ensure_pbb_core():
    so = os.path.join(BASE_DIR, "pbb_core.cpython-*.so")
    if not glob.glob(so):
        print("[main] Building pbb_core...", file=sys.stderr)
        subprocess.check_call([sys.executable, os.path.join(BASE_DIR, "setup.py"), "build_ext", "--inplace"],
                              stdout=subprocess.DEVNULL)

def detect_avx2():
    try:
        with open("/proc/cpuinfo") as f: return "avx2" in f.read().lower()
    except FileNotFoundError: return False

def build_engine():
    bin_path = os.path.join(BASE_DIR, "pbb_engine")
    src_dir = os.path.join(BASE_DIR, "src")
    main_cpp = os.path.join(BASE_DIR, "engine_main.cpp")

    need = not os.path.exists(bin_path)
    if not need:
        bt = os.path.getmtime(bin_path)
        for f in [main_cpp] + [os.path.join(src_dir, x) for x in os.listdir(src_dir) if x.endswith((".hpp", ".cpp"))]:
            if os.path.getmtime(f) > bt: need = True; break
    if not need: return bin_path

    flags = ["-std=c++17", "-O3", "-funroll-loops", "-ffast-math"]
    if detect_avx2():
        flags.extend(["-mavx2", "-mfma"])
        print("[main] AVX2 detected", file=sys.stderr)
    if sys.platform == "win32": flags = ["/std:c++17", "/O2"]

    cmd = ["g++"] + flags + ["-Isrc", "-o", bin_path, main_cpp]
    print(f"[main] Compiling: {' '.join(cmd)}", file=sys.stderr)
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0: print(f"[main] Build FAILED:\n{r.stderr}", file=sys.stderr); sys.exit(1)
    return bin_path


# ===== 主流程 =====
def main():
    check_python()
    ensure_deps()
    os.chdir(BASE_DIR)
    ensure_pbb_core()
    import pbb_core
    engine_bin = build_engine()

    # ---- 解析配置 ----
    try: import yaml; HAS_YAML = True
    except ImportError: HAS_YAML = False

    parser = argparse.ArgumentParser(description="PBB Name Scoring Tester")
    parser.add_argument("-c", "--config", default="config.json")
    parser.add_argument("--threads", type=int, help="覆盖线程数, -1=自动")
    args = parser.parse_args()

    config_path = args.config
    suffix = os.path.splitext(config_path)[1].lower()
    if suffix in ('.yaml', '.yml'):
        if not HAS_YAML: print("ERROR: PyYAML not installed", file=sys.stderr); sys.exit(1)
        with open(config_path, encoding="utf-8") as f: config = yaml.safe_load(f)
    elif suffix == '.toml':
        try:
            import tomllib
        except ImportError:
            import tomli as tomllib
        with open(config_path, "rb") as f: config = tomllib.load(f)
    else:
        with open(config_path, encoding="utf-8") as f: config = json.load(f)

    if args.threads is not None:
        config["threads"]["worker_threads"] = args.threads
    if config["threads"]["worker_threads"] == -1:
        config["threads"]["worker_threads"] = os.cpu_count() or 4
        print(f"[main] Auto threads: {config['threads']['worker_threads']}", file=sys.stderr)

    # ---- 构建字符集 ----
    pbb_core.init_exhanzi()
    cs = config["character_set"]; scl = cs["single_char_length"]
    buf = bytearray()
    for t in cs["types"]:
        if scl == 1:
            if t == 1: buf.extend(b'0123456789')
            elif t == 2: buf.extend(b'abcdefghijklmnopqrstuvwxyz')
            elif t == 3: buf.extend(b'ABCDEFGHIJKLMNOPQRSTUVWXYZ')
            elif t in (4,5): buf.extend(bytes([int(x)&0xFF for x in cs.get("custom_values",[])]))
        elif scl == 2:
            if t == 1: buf.extend(pbb_core.get_xila_bytes())
            elif t == 2: buf.extend(pbb_core.get_XILA_bytes())
            elif t == 3: buf.extend(pbb_core.get_ewen_bytes())
            elif t == 4: buf.extend(pbb_core.get_EWEN_bytes())
            elif t == 5: buf.extend(pbb_core.get_lading_bytes())
            elif t == 6: buf.extend(pbb_core.get_LADING_bytes())
            elif t == 7: buf.extend(bytes([int(x)&0xFF for x in cs.get("custom_values",[])]))
            elif t == 8:
                for r in cs.get("unicode_ranges",[]):
                    for cp in range(r["start"],r["end"]+1): buf.extend(pbb_core.encode_unicode(cp))
        elif scl == 3:
            if t == 1: pbb_core.load_hanzi(19968,40959); buf.extend(pbb_core.get_charset_bytes()); pbb_core.reset_charset()
            elif t == 2: buf.extend(pbb_core.get_hanzi_bytes())
            elif t == 3: buf.extend(pbb_core.get_pingjia_bytes())
            elif t == 4: buf.extend(pbb_core.get_pianjia_bytes())
            elif t == 5: buf.extend(pbb_core.get_mangwen_bytes())
            elif t == 6: buf.extend(pbb_core.get_extended_hanzi_3_bytes())
            elif t == 7: buf.extend(bytes([int(x)&0xFF for x in cs.get("custom_values",[])]))
            elif t == 8:
                for r in cs.get("unicode_ranges",[]):
                    for cp in range(r["start"],r["end"]+1): buf.extend(pbb_core.encode_unicode(cp))
        elif scl == 4:
            if t == 1: buf.extend(pbb_core.get_extended_hanzi_4_bytes())
            elif t == 2: buf.extend(bytes([int(x)&0xFF for x in cs.get("custom_values",[])]))
            elif t == 3:
                for r in cs.get("unicode_ranges",[]):
                    for cp in range(r["start"],r["end"]+1): buf.extend(pbb_core.encode_unicode(cp))
    charset_bytes = bytes(buf)
    charset_len = len(charset_bytes) // scl if scl else 0

    # ---- 前缀/后缀 + 输出文件名 ----
    prefixes = [p["name"] for p in config["prefixes"]]
    suffixes = [s["name"] for s in config["suffixes"]]
    prefixes = ["" if x == "+" else x for x in prefixes]
    suffixes = ["" if x == "+" else x for x in suffixes]

    result_file = config["output"]["result_file"]
    if result_file == "+": result_file = f"out-{''.join(str(random.randint(0,9)) for _ in range(7))}.txt"
    elif result_file == "-": result_file = f"out-{int(time.time())}.txt"

    en = config["enumeration"]; cl = config["collection"]; out = config["output"]
    ranges = en.get("ranges", [{}])[0]

    # ---- stdin 传参 ----
    params = ""
    params += f"team_name={config['team_name']}\n"
    params += f"n_threads={config['threads']['worker_threads']}\n"
    params += f"scl={scl}\ncharset_len={charset_len}\ncharset_bytes={charset_bytes.hex()}\n"
    params += f"prefixes={','.join(prefixes)}\nsuffixes={','.join(suffixes)}\n"
    params += f"mode={en['mode']}\nvariable_len={en['variable_length']}\n"
    params += f"range_L={ranges.get('start',0)}\nrange_R={ranges.get('end',int(1e18))}\n"
    params += f"xp_min={cl['xp_min']}\nxd_min={cl['xd_min']}\n"
    params += f"collect_mode={cl.get('collect_mode',0)}\n"
    params += f"output_xp={out.get('output_xp',1)}\noutput_log={out.get('log_output',1)}\noutput_speed={out.get('speed_output',1)}\n"
    params += f"result_file={result_file}\n"

    os.makedirs("out", exist_ok=True)
    print(f"[main] Threads: {config['threads']['worker_threads']}, Mode: {en['mode']}", file=sys.stderr)

    # ---- 启动引擎 (实时读取 stderr) ----
    t0 = time.time()
    proc = subprocess.Popen([engine_bin], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    proc.stdin.write(params)
    proc.stdin.close()
    for line in proc.stderr:
        line = line.strip()
        if line: print(f"  {line}", file=sys.stderr, flush=True)
    proc.wait()
    elapsed = time.time() - t0

    if proc.returncode != 0:
        print(f"[main] Engine failed ({proc.returncode})", file=sys.stderr); sys.exit(1)

    # ---- 统计结果 ----
    out_path = os.path.join("out", result_file)
    count, mxp, mxd = 0, 0, 0
    if os.path.exists(out_path):
        with open(out_path) as f:
            for line in f:
                if not line.strip(): continue
                count += 1
                parts = line.rsplit(" ", 2)
                if len(parts) == 3:
                    try: mxp = max(mxp, int(parts[1])); mxd = max(mxd, int(parts[2]))
                    except ValueError: pass

    rL, rR = ranges.get("start", 0), ranges.get("end", int(1e18))
    print(f"\n[main] Done in {elapsed:.1f}s", file=sys.stderr)
    print(f"[main] Found: {count}, Max: XP={mxp} XD={mxd}", file=sys.stderr)
    if elapsed > 0 and rR > rL:
        speed = (rR - rL) / elapsed
        print(f"[main] Speed: {speed*86400/1e12:.4f}T/d ({speed/1e6:.2f}M/s)", file=sys.stderr)


if __name__ == "__main__":
    main()

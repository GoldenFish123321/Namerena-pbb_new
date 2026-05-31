#!/usr/bin/env python3
"""
PBB 名字评分测号器 — Python 编排层.

职责:
  1. 解析配置文件 (JSON/YAML/TOML 三格式)
  2. 构建字符集 → stdin 管道传参 → 启动 C++ 子进程
  3. 实时读取引擎进度 → 统计最终结果

用法:
  ./run.sh -c config.yaml --threads 8    # Linux/Termux
  run.bat -c config.yaml                 # Windows
  python3 main.py -c config.yaml         # 直接调用
  python3 main.py                         # 自动查找 config.json/yaml/toml
"""
import json, os, sys, time, random, argparse, subprocess

from build import ensure_all          # 编译模块: 环境检测 + C++ 编译

BASE_DIR = os.path.dirname(os.path.abspath(__file__))


# ═══════════════════════════════════════════════════════════════
# 主流程
# ═══════════════════════════════════════════════════════════════

def main():
    # ── 1. 解析命令行 ──
    parser = argparse.ArgumentParser(description="PBB 名字评分测号器")
    parser.add_argument("-c", "--config", default=None,
                        help="配置文件路径 (默认依次尝试 config.json/yaml/toml)")
    parser.add_argument("--rebuild", action="store_true", help="强制重编 C++ 引擎")
    parser.add_argument("--threads", type=int, help="覆盖: threads.worker_threads (-1=自动)")
    parser.add_argument("--team", help="覆盖: team_name")
    parser.add_argument("--mode", type=int, help="覆盖: enumeration.mode (1=顺序 2/3/4=随机)")
    parser.add_argument("--vlen", type=int, help="覆盖: enumeration.variable_length")
    parser.add_argument("--range-start", type=int, help="覆盖: enumeration.ranges[0].start")
    parser.add_argument("--range-end", type=int, help="覆盖: enumeration.ranges[0].end")
    parser.add_argument("--xp-min", type=int, help="覆盖: collection.xp_min")
    parser.add_argument("--xd-min", type=int, help="覆盖: collection.xd_min")
    parser.add_argument("--collect-mode", type=int, choices=[0,1,2], help="覆盖: collection.collect_mode")
    parser.add_argument("--output-xp", type=int, choices=[0,1], help="覆盖: output.output_xp")
    parser.add_argument("--scl", type=int, choices=[1,2,3,4], help="覆盖: character_set.single_char_length")
    parser.add_argument("--types", help="覆盖: character_set.types (逗号分隔, 如 1,2,3)")
    parser.add_argument("--custom-values", help="覆盖: character_set.custom_values (字符串)")
    args = parser.parse_args()

    # ── 2. 环境准备 ──
    engine_bin = ensure_all(rebuild=args.rebuild)
    import pbb_core

    # ── 3. 解析配置文件 ──
    try: import yaml; HAS_YAML = True
    except ImportError: HAS_YAML = False

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

    # 命令行覆盖配置 (--xxx 直接覆写 config 对应字段)
    if args.team:   config["team_name"] = args.team
    if args.mode is not None: config["enumeration"]["mode"] = args.mode
    if args.vlen is not None: config["enumeration"]["variable_length"] = args.vlen
    if args.range_start is not None: config["enumeration"]["ranges"][0]["start"] = args.range_start
    if args.range_end is not None:   config["enumeration"]["ranges"][0]["end"] = args.range_end
    if args.xp_min is not None: config["collection"]["xp_min"] = args.xp_min
    if args.xd_min is not None: config["collection"]["xd_min"] = args.xd_min
    if args.collect_mode is not None: config["collection"]["collect_mode"] = args.collect_mode
    if args.output_xp is not None: config["output"]["output_xp"] = args.output_xp
    if args.scl is not None: config["character_set"]["single_char_length"] = args.scl
    if args.types is not None:
        config["character_set"]["types"] = [int(x.strip()) for x in args.types.split(",")]
    if args.custom_values is not None:
        config["character_set"]["custom_values"] = args.custom_values

    # ── 4. 构建字符集 ──
    # 从配置中读取字符集类型, 通过 pbb_core API 组装为字节流.
    # 字节流以 hex 编码后通过 stdin 传给 C++ 引擎.

    def _custom_bytes(values):
        """解析 custom_values: 字符串 → UTF-8 编码, 数字列表 → 字节数组 (兼容旧格式)."""
        if isinstance(values, str):
            return values.encode("utf-8")
        return bytes([int(x) & 0xFF for x in values])

    pbb_core.init_exhanzi()
    cs = config["character_set"]
    scl = cs["single_char_length"]
    buf = bytearray()

    for t in cs["types"]:
        # ── 1 字节字符集 ──
        if scl == 1:
            if t == 1:   buf.extend(b'0123456789')
            elif t == 2: buf.extend(b'abcdefghijklmnopqrstuvwxyz')
            elif t == 3: buf.extend(b'ABCDEFGHIJKLMNOPQRSTUVWXYZ')
            elif t in (4, 5): buf.extend(_custom_bytes(cs.get("custom_values", [])))

        # ── 2 字节字符集 ──
        elif scl == 2:
            if t == 1:   buf.extend(pbb_core.get_xila_bytes())          # 小写希腊
            elif t == 2: buf.extend(pbb_core.get_XILA_bytes())          # 大写希腊
            elif t == 3: buf.extend(pbb_core.get_ewen_bytes())          # 小写俄文
            elif t == 4: buf.extend(pbb_core.get_EWEN_bytes())          # 大写俄文
            elif t == 5: buf.extend(pbb_core.get_lading_bytes())        # 小写拉丁
            elif t == 6: buf.extend(pbb_core.get_LADING_bytes())        # 大写拉丁
            elif t == 7: buf.extend(_custom_bytes(cs.get("custom_values", [])))
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
            elif t == 7: buf.extend(_custom_bytes(cs.get("custom_values", [])))
            elif t == 8:
                for r in cs.get("unicode_ranges", []):
                    for cp in range(r["start"], r["end"] + 1):
                        buf.extend(pbb_core.encode_unicode(cp))

        # ── 4 字节字符集 ──
        elif scl == 4:
            if t == 1:   buf.extend(pbb_core.get_extended_hanzi_4_bytes())
            elif t == 2: buf.extend(_custom_bytes(cs.get("custom_values", [])))
            elif t == 3:
                for r in cs.get("unicode_ranges", []):
                    for cp in range(r["start"], r["end"] + 1):
                        buf.extend(pbb_core.encode_unicode(cp))

    charset_bytes = bytes(buf)
    charset_len = len(charset_bytes) // scl if scl else 0

    # ── 5. 组装引擎参数 ──
    prefixes = [p["name"] for p in config["prefixes"]]
    suffixes = [s["name"] for s in config["suffixes"]]
    prefixes = ["" if x == "+" else x for x in prefixes]
    suffixes = ["" if x == "+" else x for x in suffixes]

    result_file = config["output"]["result_file"]
    if result_file == "+":
        result_file = f"out-{''.join(str(random.randint(0, 9)) for _ in range(7))}.txt"
    elif result_file == "-":
        result_file = f"out-{int(time.time())}.txt"

    en = config["enumeration"]
    cl = config["collection"]
    out_cfg = config["output"]
    ranges = en.get("ranges", [{}])[0]

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
    if cl.get('collect_mode', 0) == 2:
        st = cl.get('special_thresholds', {})
        eight_v = cl.get('eight_v_min', 0)
        params += (
            f"c_eight_v_min={st.get('eight_v_min', eight_v)}\n"
            f"c_seven_v_min={st.get('seven_v_min', eight_v)}\n"
            f"c_hl_min={st.get('hl_min', eight_v)}\n"
            f"c_hp398_min={st.get('hp398_eight_v_min', eight_v)}\n"
        )

    os.makedirs("out", exist_ok=True)
    print(f"[main] Threads: {config['threads']['worker_threads']}, Mode: {en['mode']}", file=sys.stderr)

    # ── 6. 启动 C++ 引擎 (实时读取进度) ──
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

    for line in proc.stderr:
        line = line.strip()
        if line:
            print(f"  {line}", file=sys.stderr, flush=True)
    proc.wait()
    elapsed = time.time() - t0

    if proc.returncode != 0:
        print(f"[main] Engine failed ({proc.returncode})", file=sys.stderr)
        sys.exit(1)

    # ── 7. 统计结果 ──
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

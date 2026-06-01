#!/usr/bin/env python3
"""
PBB 名字评分测号器 — Python 编排层.

职责:
  1. 解析配置文件 (JSON/YAML/TOML 三格式) + 命令行覆盖
  2. 调用 engine.run() 启动 C++ 子进程
  3. 统计并输出结果

编译由 build.py 负责, 依赖由 run.sh/run.bat 安装到 .venv。

用法:
  ./run.sh -c config.yaml --threads 8    # Linux/Termux
  run.bat -c config.yaml                 # Windows
  python3 main.py -c config.yaml         # 直接调用
  python3 main.py                         # 自动查找 config.json/yaml/toml
"""
import json, os, sys, time, argparse, uuid
from datetime import datetime

from build import ensure_all          # 编译模块
from engine import run as run_engine  # 引擎执行

BASE_DIR = os.path.dirname(os.path.abspath(__file__))


def _resolve_result_file(raw: str) -> str:
    """解析 result_file 配置值.

    +  → 随机 hex 名 (result_<8hex>.txt)
    -  → 时间戳名   (result_<YYYYMMDD_HHMMSS>.txt)
    其他 → 原样使用
    自动去除前导 out/ (引擎内部已加)
    """
    raw = raw.strip() if raw else ""
    if raw == "+":
        resolved = f"result_{uuid.uuid4().hex[:8]}.txt"
    elif raw == "-" or raw == "":
        resolved = f"result_{datetime.now().strftime('%Y%m%d_%H%M%S')}.txt"
    else:
        resolved = raw
    # 引擎内部统一加 out/ 前缀, 此处去除避免双写
    if resolved.startswith("out/") or resolved.startswith("out\\"):
        resolved = resolved[4:]
    return resolved


def _print_results(results: list, output_xp: int):
    """将结果列表打印到 stdout (格式与输出文件一致)."""
    for r in results:
        if output_xp:
            print(f"{r['name']} {r['xp']} {r['xd']}")
        else:
            print(r["name"])


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
    parser.add_argument("-o", "--output-file", help="覆盖: output.result_file (+ =随机, - =时间戳, 其他=自定义)")
    parser.add_argument("-O", "--output-dest", choices=["file", "stdout", "both"], default=None,
                        help="输出目标: file=仅文件(默认) stdout=仅终端 both=文件+终端")
    parser.add_argument("--debug", action="store_true", help="启用引擎 debug 诊断输出")
    parser.add_argument("--scl", type=int, choices=[1,2,3,4], help="覆盖: character_set.single_char_length")
    parser.add_argument("--types", help="覆盖: character_set.types (逗号分隔, 如 1,2,3)")
    parser.add_argument("--custom-values", help="覆盖: character_set.custom_values (字符串)")
    args = parser.parse_args()

    # ── 2. 环境准备 ──
    engine_bin = ensure_all(rebuild=args.rebuild)

    # ── 3. 解析配置文件 ──
    try: import yaml; HAS_YAML = True
    except ImportError: HAS_YAML = False

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

    suffix = os.path.splitext(config_path)[1].lower()
    if suffix in ('.yaml', '.yml'):
        if not HAS_YAML: print("ERROR: PyYAML not installed", file=sys.stderr); sys.exit(1)
        with open(config_path, encoding="utf-8") as f: cfg = yaml.safe_load(f)
    elif suffix == '.toml':
        try: import tomllib
        except ImportError: import tomli as tomllib
        with open(config_path, "rb") as f: cfg = tomllib.load(f)
    else:
        with open(config_path, encoding="utf-8") as f: cfg = json.load(f)

    # ── 4. 组装引擎配置 (配置文件 + CLI 覆盖) ──
    en = cfg["enumeration"]
    cl = cfg["collection"]
    out_cfg = cfg["output"]
    rng = en.get("ranges", [{}])[0]

    # 解析输出文件名: 配置文件 → CLI 覆盖
    result_file = _resolve_result_file(out_cfg.get("result_file", "result.txt"))
    if args.output_file is not None:
        result_file = _resolve_result_file(args.output_file)

    # 输出目标: CLI > 默认 file
    output_dest = args.output_dest or "file"

    task_config = {
        "team_name":      cfg["team_name"],
        "character_set":  cfg["character_set"],
        "prefixes":       [p["name"] for p in cfg["prefixes"]],
        "suffixes":       [s["name"] for s in cfg["suffixes"]],
        "scl":            cfg["character_set"]["single_char_length"],
        "vlen":           en["variable_length"],
        "mode":           en["mode"],
        "range_start":    rng.get("start", 0),
        "range_end":      rng.get("end", int(1e18)),
        "xp_min":         cl["xp_min"],
        "xd_min":         cl["xd_min"],
        "collect_mode":   cl.get("collect_mode", 0),
        "output_xp":      out_cfg.get("output_xp", 1),
        "output_log":     out_cfg.get("log_output", 1),
        "output_speed":   out_cfg.get("speed_output", 1),
        "debug_mode":     cfg.get("debug_mode", 0),
        "n_threads":      cfg["threads"]["worker_threads"],
    }
    # collect_mode=2: 阈值
    if task_config["collect_mode"] == 2:
        st = cl.get("special_thresholds", {})
        ev = cl.get("eight_v_min", 0)
        task_config.update({
            "c_eight_v_min": st.get("eight_v_min", ev),
            "c_seven_v_min": st.get("seven_v_min", ev),
            "c_hl_min":      st.get("hl_min", ev),
            "c_hp398_min":   st.get("hp398_eight_v_min", ev),
        })

    # CLI 覆盖
    if args.team:           task_config["team_name"] = args.team
    if args.mode is not None:    task_config["mode"] = args.mode
    if args.vlen is not None:    task_config["vlen"] = args.vlen
    if args.range_start is not None: task_config["range_start"] = args.range_start
    if args.range_end is not None:   task_config["range_end"] = args.range_end
    if args.xp_min is not None: task_config["xp_min"] = args.xp_min
    if args.xd_min is not None: task_config["xd_min"] = args.xd_min
    if args.collect_mode is not None: task_config["collect_mode"] = args.collect_mode
    if args.output_xp is not None: task_config["output_xp"] = args.output_xp
    if args.debug:             task_config["debug_mode"] = 1
    if args.threads is not None: task_config["n_threads"] = args.threads
    if args.scl is not None: task_config["character_set"]["single_char_length"] = args.scl
    if args.types is not None:
        task_config["character_set"]["types"] = [int(x.strip()) for x in args.types.split(",")]
    if args.custom_values is not None:
        task_config["character_set"]["custom_values"] = args.custom_values

    # ── 5. 执行 ──
    import os as _os
    n = task_config.get("n_threads", -1)
    if n == -1:
        n = _os.cpu_count() or 4
    task_config["n_threads"] = n

    # stdout 模式: 使用临时目录 (引擎写文件, Python 读取后自动清理)
    if output_dest == "stdout":
        out_dir = None       # run() 内部创建 TemporaryDirectory, 用完即删
        print(f"[main] Threads: {n}, Mode: {task_config['mode']}, Output: stdout", file=sys.stderr)
    elif output_dest == "both":
        out_dir = "."
        print(f"[main] Threads: {n}, Mode: {task_config['mode']}, Output: file+stdout → out/{result_file}", file=sys.stderr)
    else:  # file
        out_dir = "."
        print(f"[main] Threads: {n}, Mode: {task_config['mode']}, Output: out/{result_file}", file=sys.stderr)

    t0 = time.time()
    result = run_engine(task_config, engine_bin, out_dir=out_dir, result_file=result_file)
    elapsed = time.time() - t0

    # ── 6. 输出 ──
    count = len(result["results"])
    speed = result["speed"]
    output_xp = task_config["output_xp"]

    # 终端打印结果 (stdout / both 模式)
    if output_dest in ("stdout", "both"):
        _print_results(result["results"], output_xp)

    print(f"\n[main] Done in {elapsed:.1f}s", file=sys.stderr)
    print(f"[main] Found: {count}, Max: XP={result['max_xp']} XD={result['max_xd']}", file=sys.stderr)
    if speed > 0:
        print(f"[main] Speed: {speed*86400/1e12:.4f}T/d ({speed/1e6:.2f}M/s)", file=sys.stderr)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
PBB 名字评分测号器 — Python 编排层.

职责:
  1. 解析配置文件 (JSON/YAML/TOML 三格式) + 命令行覆盖
  2. 校验配置合法性
  3. 调用 engine.run() 启动 C++ 子进程
  4. 统计并输出结果

编译由 build.py 负责, 依赖由 run.sh/run.bat 安装到 .venv。

用法:
  ./run.sh -c config.yaml --threads 8    # Linux/Termux
  run.bat -c config.yaml                 # Windows
  python3 main.py -c config.yaml         # 直接调用
  python3 main.py                         # 自动查找 config.json/yaml/toml

config → engine 键名映射: 见 config_schema.py (CONFIG_MAP)，唯一定义处。
新增配置字段只需在 CONFIG_MAP 加一行，main.py 和 engine.py 自动同步。
"""
import json, os, sys, time, argparse, uuid
from datetime import datetime
from typing import Any

from build import ensure_all          # 编译模块
from engine import run as run_engine  # 引擎执行
from config_schema import (           # config→engine 映射唯一真相源
    CONFIG_MAP, read_config, engine_default,
)

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
APP_VERSION = "0.1.1"


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


def _die(msg: str):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def _validate_config(cfg: dict):
    """校验配置字段合法性, 不合法立即退出.

    使用 CONFIG_MAP 中的 config_path 读取值，发现命名不一致时直接报错。
    """
    # ── debug_mode ──
    dm = _read_cfg(cfg, "debug_mode")
    if dm not in (0, 1):
        _die(f"debug_mode 必须为 0 或 1, 当前: {dm}")

    # ── team_name ──
    tn = _read_cfg(cfg, "team_name")
    if not tn or not isinstance(tn, str):
        _die("team_name 不能为空")

    # ── prefixes ──
    pfx = cfg.get("prefixes", [])
    if not isinstance(pfx, list) or not pfx:
        _die("prefixes 至少需要一个元素")
    for i, p in enumerate(pfx):
        if not isinstance(p.get("name"), str):
            _die(f"prefixes[{i}].name 必须为字符串")

    # ── suffixes ──
    sfx = cfg.get("suffixes", [])
    if not isinstance(sfx, list) or not sfx:
        _die("suffixes 至少需要一个元素")
    for i, s in enumerate(sfx):
        if not isinstance(s.get("name"), str):
            _die(f"suffixes[{i}].name 必须为字符串")

    # ── character_set ──
    cs = cfg.get("character_set", {})
    scl = cs.get("single_char_length")
    if not isinstance(scl, int) or scl < 1:
        _die(f"character_set.single_char_length 必须为正整数, 当前: {scl}")

    types = cs.get("types", [])
    if not isinstance(types, list) or not types:
        _die("character_set.types 至少需要一个类型")
    scl_type_max = {1: 5, 2: 8, 3: 8, 4: 3}
    tmax = scl_type_max.get(scl, 2)  # scl >= 5: 1=自定义 2=Unicode区间
    for t in types:
        if not isinstance(t, int) or t < 1 or t > tmax:
            _die(f"character_set.types: scl={scl} 时 type 必须为 1~{tmax}, 当前: {t}")

    custom = cs.get("custom_values")
    if custom is not None:
        if not isinstance(custom, (str, list)):
            _die("character_set.custom_values 必须为字符串或数字列表")
        if isinstance(custom, str) and len(custom) == 0:
            _die("character_set.custom_values 不能为空字符串")
        if isinstance(custom, list) and len(custom) == 0:
            _die("character_set.custom_values 不能为空列表")

    # ── enumeration ──
    en = cfg.get("enumeration", {})
    mode = en.get("mode")
    if mode not in (1, 2, 3, 4):
        _die(f"enumeration.mode 必须为 1/2/3/4, 当前: {mode}")

    vlen = en.get("variable_length")
    if not isinstance(vlen, int) or vlen < 1 or vlen > 16:
        _die(f"enumeration.variable_length 必须为 1~16, 当前: {vlen}")

    ranges = en.get("ranges", [])
    if not isinstance(ranges, list) or not ranges:
        _die("enumeration.ranges 至少需要一个区间")
    for i, r in enumerate(ranges):
        start = r.get("start", 0)
        end = r.get("end", int(1e18))
        if not isinstance(start, int) or start < 0:
            _die(f"enumeration.ranges[{i}].start 必须 >= 0, 当前: {start}")
        if not isinstance(end, int) or (end != -1 and end <= start):
            _die(f"enumeration.ranges[{i}].end 必须 > start (或 -1=无限), 当前: end={end} start={start}")

    # ── collection (使用 _read_cfg 统一读取) ──
    xpm = _read_cfg(cfg, "collection.xp_min")
    if not isinstance(xpm, int) or xpm < 0 or xpm > 9999:
        _die(f"collection.xp_min 必须为 0~9999, 当前: {xpm}")

    xdm = _read_cfg(cfg, "collection.xd_min")
    if not isinstance(xdm, int) or xdm < 0 or xdm > 9999:
        _die(f"collection.xd_min 必须为 0~9999, 当前: {xdm}")

    cm = _read_cfg(cfg, "collection.collect_mode")
    if cm not in (0, 1, 2):
        _die(f"collection.collect_mode 必须为 0/1/2, 当前: {cm}")

    if cm == 2:
        cl = cfg.get("collection", {})
        st = cl.get("special_thresholds", {})
        for key, label, vmax in [
            ("eight_v_min", "八围最低", 999),
            ("seven_v_min", "七围最低", 999),
            ("hl_min", "HL 最低", 99),
            ("hp398_eight_v_min", "HP398 八围最低", 999),
        ]:
            v = st.get(key)
            if v is not None and (not isinstance(v, int) or v < 0 or v > vmax):
                _die(f"collection.special_thresholds.{key} ({label}) 必须为 0~{vmax}, 当前: {v}")

    # ── output (使用 _read_cfg 统一读取) ──
    ox = _read_cfg(cfg, "output.output_xp")
    if ox not in (0, 1):
        _die(f"output.output_xp 必须为 0 或 1, 当前: {ox}")

    ol = _read_cfg(cfg, "output.log_output")
    if ol not in (0, 1):
        _die(f"output.log_output 必须为 0 或 1, 当前: {ol}")

    os_ = _read_cfg(cfg, "output.speed_output")
    if os_ not in (0, 1):
        _die(f"output.speed_output 必须为 0 或 1, 当前: {os_}")

    # ── threads (使用 _read_cfg 统一读取) ──
    wt = _read_cfg(cfg, "threads.worker_threads")
    if not isinstance(wt, int) or wt < -1 or wt == 0 or wt > 256:
        _die(f"threads.worker_threads 必须为 -1(自动) 或 1~256, 当前: {wt}")


# ── CONFIG_MAP 索引 (启动时构建, 避免每次遍历) ──────────────────────────────
_config_index: dict = {fd.config_path: fd for fd in CONFIG_MAP}


def _read_cfg(cfg: dict, config_path: str) -> Any:
    """从配置 dict 按 config_path 读取值, 使用 CONFIG_MAP 定义的默认值."""
    fd = _config_index.get(config_path)
    if fd is None:
        raise KeyError(f"未知 config_path: {config_path} (未在 CONFIG_MAP 中定义)")
    return read_config(cfg, fd)


def _build_task_config(cfg: dict) -> dict:
    """从配置 dict 构建 engine 层 task_config，统一使用 engine 键名.

    新增字段只需在 CONFIG_MAP 加一行，此函数自动包含。
    特殊字段 (character_set/prefixes/suffixes/ranges) 手工补充。
    """
    en = cfg["enumeration"]
    cl = cfg["collection"]
    out_cfg = cfg["output"]
    rng = en.get("ranges", [{}])[0]

    # ── 标量字段: 从 CONFIG_MAP 自动生成 ──
    tc: dict = {}
    for fd in CONFIG_MAP:
        tc[fd.engine_key] = read_config(cfg, fd)

    # ── 手工字段: 需要变换的非标量 ──
    tc["character_set"] = cfg["character_set"]
    tc["prefixes"] = [p["name"] for p in cfg["prefixes"]]
    tc["suffixes"] = [s["name"] for s in cfg["suffixes"]]
    tc["scl"] = cfg["character_set"]["single_char_length"]
    tc["range_L"] = rng.get("start", 0)
    tc["range_R"] = rng.get("end", int(1e18))
    # result_file: 优先 CONFIG_MAP 默认 "result.txt"
    tc["result_file"] = tc.get("result_file", "result.txt")

    return tc


def main():
    print(f"[main] Version: {APP_VERSION}", file=sys.stderr)

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
    parser.add_argument("--output-dest", choices=["file", "stdout", "both"], default=None,
                        help="输出目标: file=仅文件(默认) stdout=仅终端 both=文件+终端")
    parser.add_argument("--debug", action="store_true", help="启用引擎 debug 诊断输出")
    parser.add_argument("--seed", type=int, help="覆盖: 随机种子 (mode 2/3/4 确定性随机)")
    parser.add_argument("--scl", type=int, choices=[1,2,3,4], help="覆盖: character_set.single_char_length")
    parser.add_argument("--types", help="覆盖: character_set.types (逗号分隔, 如 1,2,3)")
    parser.add_argument("--custom-values", help="覆盖: character_set.custom_values (字符串)")
    args = parser.parse_args()

    # ── 2. 环境准备 ──
    engine_bin = ensure_all(rebuild=args.rebuild, verbose=args.debug)

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

    # ── 3.5 配置合法性校验 ──
    _validate_config(cfg)

    # ── 4. 组装引擎配置 (CONFIG_MAP 驱动 + CLI 覆盖) ──
    task_config = _build_task_config(cfg)

    # -1 = 几乎无限 (uint64_t max ≈ 1.8×10^19)
    if task_config["range_R"] == -1:
        task_config["range_R"] = 2**64 - 1

    # collect_mode=2: 阈值 (从 CONFIG_MAP 自动包含, 无需手动 add)
    # 但需确认 collect_mode 一致性
    if task_config.get("collect_mode") != 2:
        for k in ("c_eight_v_min", "c_seven_v_min", "c_hl_min", "c_hp398_min"):
            task_config.pop(k, None)

    # CLI 覆盖 (使用 engine 键名)
    if args.team:           task_config["team_name"] = args.team
    if args.mode is not None:    task_config["mode"] = args.mode
    if args.vlen is not None:    task_config["variable_len"] = args.vlen
    if args.range_start is not None: task_config["range_L"] = args.range_start
    if args.range_end is not None:   task_config["range_R"] = args.range_end
    if args.xp_min is not None: task_config["xp_min"] = args.xp_min
    if args.xd_min is not None: task_config["xd_min"] = args.xd_min
    if args.collect_mode is not None: task_config["collect_mode"] = args.collect_mode
    if args.output_xp is not None: task_config["output_xp"] = args.output_xp
    if args.debug:             task_config["debug_mode"] = 1
    if args.seed is not None:  task_config["seed"] = args.seed
    if args.threads is not None: task_config["n_threads"] = args.threads
    if args.scl is not None: task_config["character_set"]["single_char_length"] = args.scl
    if args.types is not None:
        task_config["character_set"]["types"] = [int(x.strip()) for x in args.types.split(",")]
    if args.custom_values is not None:
        # 支持字符串和逗号分隔的数字列表 (如 240,159,153)
        val = args.custom_values
        try:
            parts = [int(x.strip()) for x in val.split(",")]
            if len(parts) > 1:
                task_config["character_set"]["custom_values"] = parts
            else:
                task_config["character_set"]["custom_values"] = val
        except ValueError:
            task_config["character_set"]["custom_values"] = val

    # ── 5. 执行 ──
    n = task_config.get("n_threads", -1)
    if n == -1:
        n = os.cpu_count() or 4
    task_config["n_threads"] = n

    # 解析输出文件名: 配置文件 → CLI 覆盖
    result_file = _resolve_result_file(task_config.get("result_file", "result.txt"))
    if args.output_file is not None:
        result_file = _resolve_result_file(args.output_file)

    # 输出目标: CLI > 默认 file
    output_dest = args.output_dest or "file"

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
    try:
        result = run_engine(task_config, engine_bin, out_dir=out_dir, result_file=result_file)
    except KeyboardInterrupt:
        print("\n[main] Interrupted", file=sys.stderr)
        sys.exit(130)
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
